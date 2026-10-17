// LikeOS Intel High Definition Audio - the PCI controller.
//
// One instance per HD Audio function on the PCI bus.  Bring-up, in order:
//
//   1. PCI: power state D0, BAR0 decode and an uncached mapping, bus
//      mastering, the vendor configuration-space fixups (Intel traffic
//      class select, the snoop controls of Intel PCH/SCH, ATI and NVIDIA).
//   2. GCAP: how many input, output and bidirectional DMA engines there
//      are and whether 64-bit addresses work; version.
//   3. Skylake and later: the extended capability list (processing pipe,
//      software position based FIFO, multiple links, time sync, DMA
//      resume).  The processing pipe is put into coupled mode so the host
//      DMA engines feed the link directly, SPIB is switched off, link 0 is
//      powered and its clock moved off the 6 MHz power-on default.
//   4. Controller reset (CRST low, then high, with the settling times the
//      specification asks for), the codec wake mask, the command rings
//      (CORB/RIRB) or the immediate command interface, the DMA position
//      buffer, interrupts (MSI, else INTx routed through the ACPI _PRT,
//      else none: commands are polled and a high resolution timer drives
//      the periods).
//   5. Codec discovery from STATESTS; each codec is handed to the codec
//      layer (hda_codec_probe), which enumerates and parses it.
//   6. The sound card: one analog PCM device on the first codec with an
//      audio function, HDMI/DP PCM devices from the display codecs, the
//      mixer controls delegated to the codecs, and a diagnostic dump.
//
// Commands: one verb is in flight per controller at a time.  The issuer
// writes the CORB and then polls for the response itself, draining the
// RIRB under the command lock; the interrupt handler drains it as well.
// Either way a solicited response lands in pending_resp[codec] and an
// unsolicited one in the unsolicited queue, which a work item hands to the
// codec layer.  A codec that stops answering through the rings is tried
// once through the immediate command registers; if that works the
// controller switches to immediate commands for good (some chipsets have
// a broken CORB).
//
// Every register read of all-ones means the device went away (hot removal,
// a GPU switched off under its audio function): the controller is marked
// gone, everything stops touching it, and the card is disconnected.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda_regs.h"
#include <kernel/mm/memory.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/hrtimer.h>

#define HDA_MAX_CTRLS 8
#define HDA_DRV "hda"

/* timeouts (microseconds unless noted) */
#define HDA_RESET_TIMEOUT_US 100000 /* CRST to settle */
#define HDA_RESET_HOLD_US 150 /* time held in reset (spec: >= 100 us) */
#define HDA_CODEC_WAKE_US 1000 /* after reset (spec: >= 521 us) */
#define HDA_CTX_WAKE_US 10000 /* the slow ones */
#define HDA_CMD_TIMEOUT_US 100000 /* per attempt, through the rings */
#define HDA_CMD_RETRY_TIMEOUT_US 200000
#define HDA_IMM_TIMEOUT_US 50000 /* immediate command */
#define HDA_CMD_ACQUIRE_US 2000000 /* waiting for another command */
#define HDA_DMA_STOP_US 100000
#define HDA_JACK_POLL_MS 1000
#define HDA_CDCLK_DEFAULT_KHZ 450000

/* ---- the private controller --------------------------------------------- */

struct hda_priv {
	struct hda_controller c;
	int index;

	/* command transport */
	volatile int cmd_busy;
	volatile int rings_live; /* CORB/RIRB programmed and running */
	uint16_t outstanding[HDA_MAX_CODECS];
	uint32_t corb_size_code, rirb_size_code;
	void *cmdbuf; /* one page: CORB at 0, RIRB at 2048 */
	uint64_t cmdbuf_phys;
	uint32_t cmds_sent, imm_cmds;
	int imm_fallback_logged;
	uint32_t timeout_reports;

	/* the unsolicited queue's work item */
	volatile int unsol_work_pending;
	uint32_t unsol_dropped;

	/* interrupts */
	int irq_mode; /* 0 none, 1 MSI, 2 INTx */
	int irq_registered;
	uint8_t msi_cap;
	volatile uint32_t irq_total, irq_stream, irq_ctrl, rirb_overruns,
		corb_errors;
	volatile uint16_t wake_mask; /* STATESTS bits seen after init */

	/* codecs */
	uint16_t probed_mask, failed_mask;
	int analog_addr; /* -1 when none */
	int dsp_owned;

	/* device state */
	volatile int gone_handled;
	int released;
	int card_registered;
	int display_power_held;
	int jack_poll_running;
	uint8_t revision;
	uint16_t subsys_vendor, subsys_device;
	uint32_t mmio_pages;
	uint32_t ml_links;
	uint32_t ml_scf;
	void *posbuf_page;

	/* timer-driven periods (no usable interrupt) and the watchdog that
	 * notices a stream whose interrupts never arrive */
	hrtimer_t timer[HDA_MAX_STREAMS];
	uint64_t timer_period_ns[HDA_MAX_STREAMS];
	uint64_t timer_next_ns[HDA_MAX_STREAMS];
	volatile int timer_mode[HDA_MAX_STREAMS]; /* 0 off, 1 watchdog, 2 periodic */
	uint32_t timer_irq_base[HDA_MAX_STREAMS];
	int watchdog_logged;
};

#define TM_OFF 0
#define TM_WATCHDOG 1
#define TM_PERIODIC 2

static struct hda_priv *g_hda[HDA_MAX_CTRLS];
static int g_hda_count;
static int g_hda_probed;
static volatile int g_hda_sched_ready;
static volatile int g_hda_cdclk_khz = HDA_CDCLK_DEFAULT_KHZ;

static struct hda_priv *priv_of(struct hda_controller *c)
{
	for (int i = 0; i < g_hda_count; i++)
		if (g_hda[i] && &g_hda[i]->c == c)
			return g_hda[i];
	return NULL;
}

static struct hda_priv *priv_of_pci(const pci_device_t *pci)
{
	if (!pci)
		return NULL;
	for (int i = 0; i < g_hda_count; i++) {
		struct hda_priv *p = g_hda[i];
		if (!p || !p->c.pci)
			continue;
		if (p->c.pci == pci ||
		    (p->c.pci->bus == pci->bus && p->c.pci->device == pci->device &&
		     p->c.pci->function == pci->function))
			return p;
	}
	return NULL;
}

/* ---- timing helpers ----------------------------------------------------------- */

static uint32_t tmo(struct hda_controller *c, uint32_t us)
{
	/* emulated controllers answer when the host gets round to it */
	return (c->flags & HDA_F_VM) ? us * 4 : us;
}

static int hda_can_sleep(void)
{
	return g_hda_sched_ready && irqs_enabled() && preempt_count_get() == 0;
}

/* ---- register access ---------------------------------------------------------- */

static int reg_ok(struct hda_controller *c, uint32_t reg, uint32_t width)
{
	return c && c->mmio && !(c->flags & HDA_FX_GONE) &&
	       (uint64_t)reg + width <= c->mmio_size;
}

uint8_t hda_read8(struct hda_controller *c, uint32_t reg)
{
	if (!reg_ok(c, reg, 1))
		return 0xff;
	return *(volatile uint8_t *)(c->mmio + reg);
}

uint16_t hda_read16(struct hda_controller *c, uint32_t reg)
{
	if (!reg_ok(c, reg, 2))
		return 0xffff;
	return *(volatile uint16_t *)(c->mmio + reg);
}

uint32_t hda_read32(struct hda_controller *c, uint32_t reg)
{
	if (!reg_ok(c, reg, 4))
		return 0xffffffffu;
	return *(volatile uint32_t *)(c->mmio + reg);
}

void hda_write8(struct hda_controller *c, uint32_t reg, uint8_t v)
{
	if (reg_ok(c, reg, 1))
		*(volatile uint8_t *)(c->mmio + reg) = v;
}

void hda_write16(struct hda_controller *c, uint32_t reg, uint16_t v)
{
	if (reg_ok(c, reg, 2))
		*(volatile uint16_t *)(c->mmio + reg) = v;
}

void hda_write32(struct hda_controller *c, uint32_t reg, uint32_t v)
{
	if (reg_ok(c, reg, 4))
		*(volatile uint32_t *)(c->mmio + reg) = v;
}

int hda_ctrl_dead(struct hda_controller *c)
{
	return !c || !c->mmio || (c->flags & HDA_FX_GONE) != 0;
}

static void hda_gone_work(void *arg);

/* An all-ones read: confirm with GCAP (never all-ones on a live part) and
 * mark the controller gone. */
static int hda_check_gone(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;

	if (c->flags & HDA_FX_GONE)
		return 1;
	if (!c->mmio)
		return 1;
	if (*(volatile uint16_t *)(c->mmio + HDA_REG_GCAP) != 0xffff)
		return 0;
	c->flags |= HDA_FX_GONE;
	if (!p->gone_handled) {
		p->gone_handled = 1;
		snd_schedule_work(hda_gone_work, p, 0);
	}
	return 1;
}

/* ---- PCI configuration -------------------------------------------------------- */

static void cfg_update8(const pci_device_t *pci, uint8_t off, uint8_t mask,
			uint8_t val)
{
	uint8_t v = pci_cfg_read8(pci, off);
	uint8_t n = (uint8_t)((v & ~mask) | (val & mask));
	if (n != v)
		pci_cfg_write8(pci, off, n);
}

/* Bring the function to D0 if firmware left it in a low power state. */
static void hda_pci_set_d0(const pci_device_t *pci)
{
	uint8_t pm = pci_find_capability(pci, HDA_PCI_CAP_PM);
	uint16_t pmcsr;

	if (!pm)
		return;
	pmcsr = pci_cfg_read16(pci, (uint8_t)(pm + 4));
	if (pmcsr == 0xffff || (pmcsr & 3) == 0)
		return;
	pci_cfg_write16(pci, (uint8_t)(pm + 4), (uint16_t)(pmcsr & ~3u));
	/* D3hot -> D0 takes up to 10 ms */
	lapic_delay_ms(10);
}

static void hda_pci_fixups(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	const pci_device_t *pci = c->pci;
	int snoop = !(c->flags & HDA_FX_SNOOP_OFF);

	/* Traffic class 0 for all transactions: other classes produce
	 * static on several codecs. */
	if (!(c->flags & HDA_FX_NO_TCSEL))
		cfg_update8(pci, HDA_PCI_TCSEL, 0x07, 0);

	if (c->flags & HDA_F_ATI_SNOOP)
		cfg_update8(pci, HDA_PCI_ATI_MISC_CNTR2, 0x07,
			    snoop ? HDA_PCI_ATI_ENABLE_SNOOP : 0);

	if (c->flags & HDA_F_NVIDIA_SNOOP) {
		cfg_update8(pci, HDA_PCI_NV_TRANSREG, 0x0f, HDA_PCI_NV_ENABLE_COHBITS);
		cfg_update8(pci, HDA_PCI_NV_ISTRM_COH, 0x01, HDA_PCI_NV_ENABLE_COHBIT);
		cfg_update8(pci, HDA_PCI_NV_OSTRM_COH, 0x01, HDA_PCI_NV_ENABLE_COHBIT);
	}

	if (c->flags & (HDA_F_INTEL_SNOOP | HDA_F_SCH_SNOOP)) {
		uint16_t devc = pci_cfg_read16(pci, HDA_PCI_INTEL_DEVC);
		if (devc != 0xffff) {
			uint16_t want = snoop ? (uint16_t)(devc & ~HDA_PCI_DEVC_NOSNOOP) :
						(uint16_t)(devc | HDA_PCI_DEVC_NOSNOOP);
			if (want != devc)
				pci_cfg_write16(pci, HDA_PCI_INTEL_DEVC, want);
		}
	}
}

/* ATI SB600 and older southbridges claim 64-bit DMA they cannot do: the
 * SMBus function's revision tells them apart. */
static int ati_sb_is_old(void)
{
	int n = 0;
	const pci_device_t *devs = pci_get_devices(&n);

	for (int i = 0; devs && i < n; i++) {
		if (devs[i].vendor_id == 0x1002 && devs[i].device_id == 0x4385) {
			uint8_t rev = pci_cfg_read8(&devs[i], 0x08);
			return rev < 0x30;
		}
	}
	return 0;
}

/* The legacy interrupt line: the ACPI routing table of the host bridge
 * (swizzling the pin through each PCI-PCI bridge on the way up), then the
 * firmware's line register. */
static const pci_device_t *bridge_above(unsigned char bus)
{
	int n = 0;
	const pci_device_t *devs = pci_get_devices(&n);

	for (int i = 0; devs && i < n; i++) {
		const pci_device_t *b = &devs[i];
		if (b->class_code != 0x06 || b->subclass != 0x04)
			continue;
		uint32_t buses = pci_cfg_read32_dev(b, 0x18);
		if (((buses >> 8) & 0xff) == bus)
			return b;
	}
	return pci_find_bridge_for_bus(bus);
}

static int hda_intx_gsi(const pci_device_t *pci)
{
	uint8_t pin = pci->interrupt_pin;
	uint32_t gsi = 0;

	if (pin >= 1 && pin <= 4) {
		uint8_t lpin = (uint8_t)(pin - 1);
		uint8_t ldev = pci->device;
		unsigned char bus = pci->bus;
		int depth = 0;

		while (bus != 0 && depth < 8) {
			const pci_device_t *br = bridge_above(bus);
			if (!br)
				break;
			lpin = (uint8_t)((lpin + ldev) % 4);
			ldev = br->device;
			bus = br->bus;
			depth++;
		}
		if (bus == 0 &&
		    acpi_pci_lookup_irq("\\\\_SB_.PCI0", ldev, lpin, &gsi) == 0 &&
		    gsi <= 223)
			return (int)gsi;
	}
	if (pci->interrupt_line != 0xff && pci->interrupt_line <= 23)
		return pci->interrupt_line;
	return -1;
}

static void hda_msi_disable(struct hda_priv *p)
{
	uint8_t cap = p->msi_cap;
	uint16_t ctl;

	if (!cap)
		return;
	ctl = pci_cfg_read16(p->c.pci, (uint8_t)(cap + 2));
	if (ctl != 0xffff && (ctl & 1))
		pci_cfg_write16(p->c.pci, (uint8_t)(cap + 2), (uint16_t)(ctl & ~1u));
}

/* ---- the command rings ------------------------------------------------------ */

static uint32_t ring_entries(uint8_t sizereg, uint32_t *code)
{
	uint8_t cap = sizereg & 0xf0;

	/* a register reading 0 (ULi, some emulations) has no capability
	 * bits: 256 entries is the size every controller supports */
	if (sizereg == 0xff || cap == 0 || (cap & HDA_RINGSIZE_CAP_256)) {
		*code = HDA_RINGSIZE_SZ_256;
		return 256;
	}
	if (cap & HDA_RINGSIZE_CAP_16) {
		*code = HDA_RINGSIZE_SZ_16;
		return 16;
	}
	*code = HDA_RINGSIZE_SZ_2;
	return 2;
}

static void cmd_io_stop(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint64_t fl;
	int i;

	spin_lock_irqsave(&c->cmd_lock, &fl);
	p->rings_live = 0;
	spin_unlock_irqrestore(&c->cmd_lock, fl);
	hda_write8(c, HDA_REG_RIRBCTL, 0);
	hda_write8(c, HDA_REG_CORBCTL, 0);
	for (i = 0; i < HDA_DMA_STOP_US / 10; i++) {
		uint8_t r = hda_read8(c, HDA_REG_RIRBCTL);
		uint8_t q = hda_read8(c, HDA_REG_CORBCTL);
		if (r == 0xff || q == 0xff)
			break;
		if (!(r & HDA_RIRBCTL_DMAEN) && !(q & HDA_CORBCTL_RUN))
			break;
		lapic_delay_us(10);
	}
}

/* Program and start the CORB and RIRB.  With immediate commands (the
 * table says so, or the fallback chose it) the CORB stays off; the RIRB
 * still runs on the parts that deliver unsolicited responses through it
 * while commands go through IC/IR. */
static int cmd_io_init(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint64_t corb_phys, rirb_phys;
	uint8_t rirbctl;
	uint16_t v;
	int i;

	if (!p->cmdbuf)
		return -ENOMEM;
	{
		/* keep the interrupt handler out of the rings meanwhile */
		uint64_t fl;
		spin_lock_irqsave(&c->cmd_lock, &fl);
		p->rings_live = 0;
		spin_unlock_irqrestore(&c->cmd_lock, fl);
	}
	cmd_io_stop(p);
	kmemset(p->cmdbuf, 0, 4096);

	corb_phys = p->cmdbuf_phys;
	rirb_phys = p->cmdbuf_phys + 2048;
	c->corb = (uint32_t *)p->cmdbuf;
	c->rirb = (uint64_t *)((uint8_t *)p->cmdbuf + 2048);
	c->corb_phys = corb_phys;
	c->rirb_phys = rirb_phys;

	/* CORB */
	c->corb_entries = ring_entries(hda_read8(c, HDA_REG_CORBSIZE), &p->corb_size_code);
	hda_write32(c, HDA_REG_CORBLBASE, (uint32_t)corb_phys);
	hda_write32(c, HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32));
	hda_write8(c, HDA_REG_CORBSIZE,
		   (uint8_t)((hda_read8(c, HDA_REG_CORBSIZE) & ~HDA_RINGSIZE_SZ_MASK) |
			     p->corb_size_code));
	hda_write16(c, HDA_REG_CORBWP, 0);
	c->corb_wp = 0;

	/* CORB read pointer reset: set the bit, wait for the controller to
	 * show it, clear it, wait for it to read back clear.  Some parts
	 * clear it themselves. */
	hda_write16(c, HDA_REG_CORBRP, HDA_CORBRP_RST);
	if (!(c->flags & HDA_FX_CORBRP_SELF_CLEAR)) {
		for (i = 0; i < 1000; i++) {
			v = hda_read16(c, HDA_REG_CORBRP);
			if (v == 0xffff || (v & HDA_CORBRP_RST))
				break;
			lapic_delay_us(1);
		}
		if (i == 1000)
			snd_info(HDA_DRV, "CORB read pointer reset not acknowledged\n");
		hda_write16(c, HDA_REG_CORBRP, 0);
		for (i = 0; i < 1000; i++) {
			v = hda_read16(c, HDA_REG_CORBRP);
			if (v == 0xffff || !(v & HDA_CORBRP_RST))
				break;
			lapic_delay_us(1);
		}
		if (i == 1000)
			snd_info(HDA_DRV, "CORB read pointer stuck in reset\n");
	} else {
		lapic_delay_us(10);
	}

	/* RIRB */
	c->rirb_entries = ring_entries(hda_read8(c, HDA_REG_RIRBSIZE), &p->rirb_size_code);
	hda_write32(c, HDA_REG_RIRBLBASE, (uint32_t)rirb_phys);
	hda_write32(c, HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32));
	hda_write8(c, HDA_REG_RIRBSIZE,
		   (uint8_t)((hda_read8(c, HDA_REG_RIRBSIZE) & ~HDA_RINGSIZE_SZ_MASK) |
			     p->rirb_size_code));
	hda_write16(c, HDA_REG_RIRBWP, HDA_RIRBWP_RST);
	c->rirb_rp = 0;
	/* an interrupt for every response */
	hda_write16(c, HDA_REG_RINTCNT, 1);

	for (i = 0; i < HDA_MAX_CODECS; i++) {
		p->outstanding[i] = 0;
		c->resp_ready[i] = 0;
	}

	rirbctl = HDA_RIRBCTL_DMAEN;
	if (p->irq_mode && !(c->flags & HDA_FX_POLL_RIRB))
		rirbctl |= HDA_RIRBCTL_RINTCTL | HDA_RIRBCTL_OIC;
	if (!c->single_cmd || (c->flags & HDA_F_SINGLE_CMD))
		hda_write8(c, HDA_REG_RIRBCTL, rirbctl);
	if (!c->single_cmd)
		hda_write8(c, HDA_REG_CORBCTL, HDA_CORBCTL_RUN |
			   (p->irq_mode ? HDA_CORBCTL_CMEIE : 0));
	{
		uint64_t fl;
		spin_lock_irqsave(&c->cmd_lock, &fl);
		p->rings_live = !c->single_cmd || (c->flags & HDA_F_SINGLE_CMD);
		spin_unlock_irqrestore(&c->cmd_lock, fl);
	}

	/* unsolicited responses */
	if (!(c->flags & HDA_F_NO_UNSOL)) {
		uint32_t g = hda_read32(c, HDA_REG_GCTL);
		if (g != 0xffffffffu)
			hda_write32(c, HDA_REG_GCTL, g | HDA_GCTL_UNSOL);
	}
	return 0;
}

static void unsol_push_locked(struct hda_priv *p, uint32_t res, uint32_t addr)
{
	struct hda_controller *c = &p->c;
	const uint32_t qsize = sizeof(c->unsol_queue) / sizeof(c->unsol_queue[0]);

	if (c->unsol_head - c->unsol_tail >= qsize) {
		p->unsol_dropped++;
		return;
	}
	c->unsol_queue[c->unsol_head % qsize] = res;
	c->unsol_queue[(c->unsol_head + 1) % qsize] = addr;
	c->unsol_head += 2;
}

static void hda_unsol_work(void *arg);
static void hda_mark_jack_poll(struct hda_priv *p);
static void hda_start_jack_poll(struct hda_priv *p);

static void unsol_kick(struct hda_priv *p)
{
	if (p->unsol_work_pending)
		return;
	p->unsol_work_pending = 1;
	if (snd_schedule_work(hda_unsol_work, p, 0) < 0)
		p->unsol_work_pending = 0;
}

/* Move every new RIRB entry out of the ring.  cmd_lock held.  Returns the
 * number of unsolicited responses queued. */
static int rirb_drain_locked(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint16_t wpreg;
	uint32_t wp, guard = 0;
	int unsol = 0;

	if (!p->rings_live || !c->rirb || !c->rirb_entries)
		return 0;
	wpreg = hda_read16(c, HDA_REG_RIRBWP);
	if (wpreg == 0xffff) {
		hda_check_gone(p);
		return 0;
	}
	wp = wpreg & 0xff;
	if (wp >= c->rirb_entries)
		wp %= c->rirb_entries;

	while (c->rirb_rp != wp && guard++ < 512) {
		uint32_t rp = (c->rirb_rp + 1) % c->rirb_entries;
		volatile uint64_t *e = (volatile uint64_t *)&c->rirb[rp];
		uint64_t ent = *e;
		uint32_t res = (uint32_t)ent;
		uint32_t ex = (uint32_t)(ent >> 32);
		uint32_t addr = ex & HDA_RIRB_EX_CODEC_MASK;

		c->rirb_rp = rp;
		if (addr >= HDA_MAX_CODECS)
			continue;
		if (ex & HDA_RIRB_EX_UNSOL) {
			unsol_push_locked(p, res, addr);
			unsol++;
			continue;
		}
		if (p->outstanding[addr] == 0)
			continue; /* late answer to a command that timed out */
		p->outstanding[addr]--;
		if (p->outstanding[addr] == 0) {
			c->pending_resp[addr] = res;
			c->resp_ready[addr] = 1;
		}
	}
	return unsol;
}

static int cmd_acquire(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint64_t fl;
	uint64_t start = hrtimer_now_ns();
	uint32_t limit = tmo(c, HDA_CMD_ACQUIRE_US);

	for (uint32_t i = 0; i < limit; i += 2) {
		spin_lock_irqsave(&c->cmd_lock, &fl);
		if (!p->cmd_busy) {
			p->cmd_busy = 1;
			spin_unlock_irqrestore(&c->cmd_lock, fl);
			return 0;
		}
		spin_unlock_irqrestore(&c->cmd_lock, fl);
		if (hda_can_sleep()) {
			sched_yield_in_kernel();
			if (start && hrtimer_now_ns() - start > (uint64_t)limit * 1000)
				break;
		} else {
			lapic_delay_us(2);
		}
	}
	return -EBUSY;
}

static void cmd_release(struct hda_priv *p)
{
	uint64_t fl;

	spin_lock_irqsave(&p->c.cmd_lock, &fl);
	p->cmd_busy = 0;
	spin_unlock_irqrestore(&p->c.cmd_lock, fl);
}

/* Through the CORB.  0 with the response, -ETIMEDOUT, -EBUSY (ring full),
 * -ENODEV (gone). */
static int corb_cmd(struct hda_priv *p, uint32_t cmd, uint32_t addr,
		    uint32_t timeout_us, uint32_t *res)
{
	struct hda_controller *c = &p->c;
	uint64_t fl, start;
	uint32_t wp, rp, unsol = 0;
	uint16_t rpreg;

	spin_lock_irqsave(&c->cmd_lock, &fl);
	if (hda_ctrl_dead(c) || !p->rings_live || !c->corb || !c->corb_entries) {
		spin_unlock_irqrestore(&c->cmd_lock, fl);
		return -ENODEV;
	}
	wp = (c->corb_wp + 1) % c->corb_entries;
	rpreg = hda_read16(c, HDA_REG_CORBRP);
	if (rpreg == 0xffff) {
		spin_unlock_irqrestore(&c->cmd_lock, fl);
		hda_check_gone(p);
		return -ENODEV;
	}
	rp = (rpreg & 0xff) % c->corb_entries;
	if (wp == rp) {
		/* full: the controller stopped fetching */
		spin_unlock_irqrestore(&c->cmd_lock, fl);
		return -EBUSY;
	}
	c->resp_ready[addr] = 0;
	p->outstanding[addr]++;
	((volatile uint32_t *)c->corb)[wp] = cmd;
	__asm__ volatile("mfence" ::: "memory");
	c->corb_wp = wp;
	hda_write16(c, HDA_REG_CORBWP, (uint16_t)wp);
	p->cmds_sent++;
	spin_unlock_irqrestore(&c->cmd_lock, fl);

	/* Poll for the answer.  The interrupt handler may take it out of the
	 * ring first; either way it ends up in pending_resp. */
	start = hrtimer_now_ns();
	for (uint32_t waited = 0; waited < timeout_us;) {
		spin_lock_irqsave(&c->cmd_lock, &fl);
		unsol += (uint32_t)rirb_drain_locked(p);
		if (c->resp_ready[addr]) {
			c->resp_ready[addr] = 0;
			if (res)
				*res = c->pending_resp[addr];
			spin_unlock_irqrestore(&c->cmd_lock, fl);
			if (unsol)
				unsol_kick(p);
			return 0;
		}
		spin_unlock_irqrestore(&c->cmd_lock, fl);
		if (hda_ctrl_dead(c))
			return -ENODEV;
		if (waited > 1000 && hda_can_sleep()) {
			sched_yield_in_kernel();
			if (start) {
				uint64_t el = (hrtimer_now_ns() - start) / 1000;
				waited = el > 0xffffffffu ? 0xffffffffu : (uint32_t)el;
			} else {
				waited += 100;
			}
		} else {
			uint32_t step = waited < 200 ? 1 : 10;
			lapic_delay_us(step);
			waited += step;
		}
	}

	/* Timed out.  Forget the expectation, so a late answer is dropped
	 * rather than credited to a later command. */
	spin_lock_irqsave(&c->cmd_lock, &fl);
	unsol += (uint32_t)rirb_drain_locked(p);
	if (c->resp_ready[addr]) {
		c->resp_ready[addr] = 0;
		if (res)
			*res = c->pending_resp[addr];
		spin_unlock_irqrestore(&c->cmd_lock, fl);
		if (unsol)
			unsol_kick(p);
		return 0;
	}
	p->outstanding[addr] = 0;
	spin_unlock_irqrestore(&c->cmd_lock, fl);
	if (unsol)
		unsol_kick(p);
	return -ETIMEDOUT;
}

/* Through the immediate command registers. */
static int imm_cmd(struct hda_priv *p, uint32_t cmd, uint32_t timeout_us,
		   uint32_t *res)
{
	struct hda_controller *c = &p->c;
	uint16_t irs;
	uint32_t i;

	for (i = 0; i < timeout_us; i++) {
		irs = hda_read16(c, HDA_REG_IRS);
		if (irs == 0xffff) {
			hda_check_gone(p);
			return -ENODEV;
		}
		if (!(irs & HDA_IRS_ICB))
			break;
		lapic_delay_us(1);
	}
	if (i == timeout_us)
		return -EBUSY;

	/* clear a stale valid bit, write the command, start it */
	hda_write16(c, HDA_REG_IRS, (uint16_t)(hda_read16(c, HDA_REG_IRS) | HDA_IRS_IRV));
	hda_write32(c, HDA_REG_IC, cmd);
	hda_write16(c, HDA_REG_IRS,
		    (uint16_t)((hda_read16(c, HDA_REG_IRS) & ~HDA_IRS_IRV) | HDA_IRS_ICB));
	p->imm_cmds++;

	for (i = 0; i < timeout_us; i++) {
		irs = hda_read16(c, HDA_REG_IRS);
		if (irs == 0xffff) {
			hda_check_gone(p);
			return -ENODEV;
		}
		if (irs & HDA_IRS_IRV) {
			uint32_t r = hda_read32(c, HDA_REG_IR);
			hda_write16(c, HDA_REG_IRS, HDA_IRS_IRV);
			if (res)
				*res = r;
			return 0;
		}
		lapic_delay_us(1);
	}
	return -ETIMEDOUT;
}

int hda_cmd(struct hda_controller *ctrl, uint32_t cmd, uint32_t *res)
{
	struct hda_priv *p = priv_of(ctrl);
	uint32_t addr = cmd >> 28;
	int rc;

	if (res)
		*res = 0xffffffffu;
	if (!p)
		return -ENODEV;
	if (addr >= HDA_MAX_CODECS)
		return -EINVAL;
	if (hda_ctrl_dead(ctrl) || p->released)
		return -ENODEV;
	if (cmd_acquire(p))
		return -EBUSY;

	if (ctrl->single_cmd) {
		rc = imm_cmd(p, cmd, tmo(ctrl, HDA_IMM_TIMEOUT_US), res);
		if (rc == -EBUSY || rc == -ETIMEDOUT) {
			/* one more try after letting the codec settle */
			lapic_delay_us(100);
			rc = imm_cmd(p, cmd, tmo(ctrl, HDA_IMM_TIMEOUT_US), res);
		}
		if (rc)
			ctrl->cmd_timeouts++;
		cmd_release(p);
		return rc == -EBUSY ? -EIO : rc;
	}

	rc = corb_cmd(p, cmd, addr, tmo(ctrl, HDA_CMD_TIMEOUT_US), res);
	if (rc == -ETIMEDOUT || rc == -EBUSY) {
		ctrl->cmd_timeouts++;
		if (p->timeout_reports < 8) {
			p->timeout_reports++;
			snd_info(HDA_DRV, "codec %u: no response to verb %08x, retrying\n",
				 addr, cmd);
		}
		/* resynchronise the rings (a lost CORB/RIRB pointer is the
		 * common cause) and try once more */
		if (rc == -EBUSY) {
			cmd_io_init(p);
		}
		rc = corb_cmd(p, cmd, addr, tmo(ctrl, HDA_CMD_RETRY_TIMEOUT_US), res);
	}
	if (rc == -ETIMEDOUT || rc == -EBUSY) {
		/* Does the codec answer through the immediate interface?  The
		 * rings must be stopped while it is used. */
		uint32_t r = 0;
		int irc;

		cmd_io_stop(p);
		irc = imm_cmd(p, cmd, tmo(ctrl, HDA_IMM_TIMEOUT_US), &r);
		if (irc == 0) {
			ctrl->single_cmd = 1;
			if (!p->imm_fallback_logged) {
				p->imm_fallback_logged = 1;
				snd_info(HDA_DRV, "command rings unreliable, switching to immediate commands\n");
			}
			/* keep the RIRB for unsolicited responses where the
			 * table allows it; otherwise both rings stay off */
			if (ctrl->flags & HDA_F_SINGLE_CMD)
				cmd_io_init(p);
			else {
				/* no RIRB, no unsolicited responses: poll */
				hda_mark_jack_poll(p);
				hda_start_jack_poll(p);
			}
			if (res)
				*res = r;
			rc = 0;
		} else {
			/* the codec itself is silent: restart the rings */
			cmd_io_init(p);
			rc = (irc == -ENODEV) ? -ENODEV : -ETIMEDOUT;
		}
	}
	cmd_release(p);
	if (rc == -EBUSY)
		rc = -EIO;
	return rc;
}

/* ---- unsolicited responses, jack polling, removal -------------------------- */

static void hda_unsol_dispatch(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	const uint32_t qsize = sizeof(c->unsol_queue) / sizeof(c->unsol_queue[0]);

	for (int n = 0; n < (int)(qsize / 2); n++) {
		uint64_t fl;
		uint32_t res, addr;

		spin_lock_irqsave(&c->cmd_lock, &fl);
		if (c->unsol_tail == c->unsol_head) {
			spin_unlock_irqrestore(&c->cmd_lock, fl);
			break;
		}
		res = c->unsol_queue[c->unsol_tail % qsize];
		addr = c->unsol_queue[(c->unsol_tail + 1) % qsize];
		c->unsol_tail += 2;
		spin_unlock_irqrestore(&c->cmd_lock, fl);

		if (addr < HDA_MAX_CODECS && c->codec[addr] && !p->released &&
		    !hda_ctrl_dead(c))
			hda_codec_unsol(c->codec[addr], res);
	}
}

static void hda_unsol_work(void *arg)
{
	struct hda_priv *p = arg;
	struct hda_controller *c = &p->c;

	p->unsol_work_pending = 0;
	hda_unsol_dispatch(p);
	/* more arrived while we were busy and nobody rescheduled us */
	if (c->unsol_tail != c->unsol_head)
		unsol_kick(p);
}

static void hda_jack_poll_work(void *arg)
{
	struct hda_priv *p = arg;
	struct hda_controller *c = &p->c;
	uint64_t fl;
	int any = 0;

	if (p->released || hda_ctrl_dead(c)) {
		p->jack_poll_running = 0;
		return;
	}
	/* Without an interrupt the RIRB is only read by command issuers:
	 * pick up unsolicited responses that arrived in between. */
	spin_lock_irqsave(&c->cmd_lock, &fl);
	rirb_drain_locked(p);
	spin_unlock_irqrestore(&c->cmd_lock, fl);
	hda_unsol_dispatch(p);

	for (int a = 0; a < HDA_MAX_CODECS; a++) {
		struct hda_codec *cd = c->codec[a];
		if (!cd || !cd->spec.jack_poll)
			continue;
		hda_codec_poll_jacks(cd);
		any = 1;
	}
	if (any || c->polling) {
		if (snd_schedule_work(hda_jack_poll_work, p, HDA_JACK_POLL_MS) < 0)
			p->jack_poll_running = 0;
	} else {
		p->jack_poll_running = 0;
	}
}

/* Start the periodic jack poll if some codec (or the controller) needs
 * it and it is not running yet. */
static void hda_start_jack_poll(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	int need = c->polling;

	if (p->jack_poll_running || p->released)
		return;
	for (int a = 0; a < HDA_MAX_CODECS; a++)
		if (c->codec[a] && c->codec[a]->spec.jack_poll)
			need = 1;
	if (!need)
		return;
	p->jack_poll_running = 1;
	if (snd_schedule_work(hda_jack_poll_work, p, HDA_JACK_POLL_MS) < 0)
		p->jack_poll_running = 0;
}

static void hda_gone_work(void *arg)
{
	struct hda_priv *p = arg;
	struct hda_controller *c = &p->c;

	snd_info(HDA_DRV, "%s: device not responding (removed?), disconnecting\n",
		 c->bus);
	for (int i = 0; i < HDA_MAX_STREAMS; i++) {
		p->timer_mode[i] = TM_OFF;
		hrtimer_cancel(&p->timer[i]);
		c->streams[i].running = 0;
	}
	if (c->card && p->card_registered)
		snd_card_disconnect(c->card);
}

/* ---- interrupt handler ------------------------------------------------------- */

static int hda_irq(void *arg)
{
	struct hda_priv *p = arg;
	struct hda_controller *c = &p->c;
	uint32_t sts;
	int unsol = 0;

	if (!c->mmio || p->released || (c->flags & HDA_FX_GONE))
		return 0;
	sts = hda_read32(c, HDA_REG_INTSTS);
	if (sts == 0xffffffffu) {
		hda_check_gone(p);
		return 0;
	}
	if (!(sts & (HDA_INT_SIS_MASK | HDA_INT_CIS)))
		return 0; /* not ours (shared line) */
	p->irq_total++;

	/* stream descriptors */
	if (sts & HDA_INT_SIS_MASK) {
		for (int i = 0; i < c->nstreams; i++) {
			if (!(sts & (1u << i)))
				continue;
			p->irq_stream++;
			hda_stream_irq(&c->streams[i]);
		}
	}

	/* controller: responses, CORB errors, codec state changes */
	if (sts & HDA_INT_CIS) {
		uint8_t rs = hda_read8(c, HDA_REG_RIRBSTS);
		uint8_t cs = hda_read8(c, HDA_REG_CORBSTS);
		uint16_t ss = hda_read16(c, HDA_REG_STATESTS);

		p->irq_ctrl++;
		if (rs != 0xff && (rs & (HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_RIRBOIS))) {
			hda_write8(c, HDA_REG_RIRBSTS,
				   rs & (HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_RIRBOIS));
			if (rs & HDA_RIRBSTS_RIRBOIS)
				p->rirb_overruns++;
			spin_lock(&c->cmd_lock);
			unsol = rirb_drain_locked(p);
			spin_unlock(&c->cmd_lock);
		}
		if (cs != 0xff && (cs & HDA_CORBSTS_CMEI)) {
			hda_write8(c, HDA_REG_CORBSTS, HDA_CORBSTS_CMEI);
			p->corb_errors++;
		}
		if (ss != 0xffff && (ss & HDA_STATESTS_MASK)) {
			hda_write16(c, HDA_REG_STATESTS, ss & HDA_STATESTS_MASK);
			p->wake_mask |= ss & HDA_STATESTS_MASK;
		}
	}
	if (unsol)
		unsol_kick(p);
	return 1;
}

/* ---- timer-driven periods ---------------------------------------------------- */

static void hda_timer_fn(hrtimer_t *t)
{
	struct hda_stream *st = t->arg;
	struct hda_controller *c;
	struct hda_priv *p;
	int idx;

	if (!st || !(c = st->ctrl) || !(p = priv_of(c)))
		return;
	idx = st->index;
	if (idx < 0 || idx >= HDA_MAX_STREAMS)
		return;
	if (p->timer_mode[idx] == TM_OFF || !st->running || hda_ctrl_dead(c))
		return;

	if (p->timer_mode[idx] == TM_WATCHDOG) {
		if (st->irq_count != p->timer_irq_base[idx]) {
			p->timer_mode[idx] = TM_OFF; /* interrupts arrive */
			return;
		}
		/* Several periods without a single completion interrupt:
		 * the interrupt is not reaching us.  Drive the stream from
		 * the timer, and keep the device from asserting a line that
		 * may be someone else's. */
		if (!p->watchdog_logged) {
			p->watchdog_logged = 1;
			snd_info(HDA_DRV, "%s: no stream interrupts, using timer-driven periods\n",
				 c->bus);
		}
		c->polling = 1;
		hda_write32(c, HDA_REG_INTCTL, 0);
		p->timer_mode[idx] = TM_PERIODIC;
		p->timer_next_ns[idx] = hrtimer_now_ns();
		/* every other running stream loses its interrupts too */
		for (int j = 0; j < c->nstreams; j++) {
			struct hda_stream *o = &c->streams[j];
			if (j == idx || !o->running || !p->timer_period_ns[j] ||
			    p->timer_mode[j] == TM_PERIODIC)
				continue;
			p->timer_mode[j] = TM_PERIODIC;
			p->timer_next_ns[j] = hrtimer_now_ns() + p->timer_period_ns[j];
			hrtimer_start(&p->timer[j], p->timer_next_ns[j]);
		}
	}

	/* The completion status is set with or without the interrupt
	 * enabled: let the stream code account for it (position checks),
	 * and only report a period ourselves when it did not. */
	uint32_t before = st->irq_count;
	hda_stream_irq(st);
	if (st->irq_count == before && st->substream && st->running)
		snd_pcm_period_elapsed(st->substream);

	if (p->timer_mode[idx] != TM_PERIODIC || !st->running)
		return;
	uint64_t now = hrtimer_now_ns();
	uint64_t next = p->timer_next_ns[idx] + p->timer_period_ns[idx];
	if (next <= now)
		next = now + p->timer_period_ns[idx];
	p->timer_next_ns[idx] = next;
	hrtimer_start(t, next);
}

static void hda_timer_arm(struct hda_priv *p, struct hda_stream *st)
{
	int idx = st->index;
	uint64_t period = p->timer_period_ns[idx];

	if (!period)
		return;
	if (p->c.polling) {
		p->timer_mode[idx] = TM_PERIODIC;
		p->timer_next_ns[idx] = hrtimer_now_ns() + period;
		hrtimer_start(&p->timer[idx], p->timer_next_ns[idx]);
	} else {
		/* watch for interrupts over a few periods */
		p->timer_mode[idx] = TM_WATCHDOG;
		p->timer_irq_base[idx] = st->irq_count;
		hrtimer_start_rel(&p->timer[idx], period * 4 + 50000000ull);
	}
}

static void hda_timer_disarm(struct hda_priv *p, struct hda_stream *st)
{
	p->timer_mode[st->index] = TM_OFF;
	/* the callback checks the mode; cancelling here never waits */
	hrtimer_cancel(&p->timer[st->index]);
}

/* From hda_stream.c whenever an engine starts or stops, whoever drives it
 * (the analog PCM here, the HDMI PCMs): the watchdog or the timer that
 * stands in for a missing interrupt follows the engine. */
void hda_ctrl_stream_event(struct hda_stream *st, int running)
{
	struct hda_priv *p;
	uint32_t rate, ch, bits, fb;
	uint64_t ns;
	int idx;

	if (!st || !st->ctrl || !(p = priv_of(st->ctrl)))
		return; /* not one of ours (the DSP driver reuses the engines) */
	idx = st->index;
	if (idx < 0 || idx >= HDA_MAX_STREAMS)
		return;
	if (!running) {
		p->timer_mode[idx] = TM_OFF;
		return;
	}
	rate = hda_format_rate(st->format);
	ch = hda_format_channels(st->format);
	bits = hda_format_bits(st->format);
	fb = ch * (bits <= 8 ? 1 : (bits <= 16 ? 2 : 4));
	if (!rate || !fb || !st->period_bytes)
		return;
	ns = ((uint64_t)st->period_bytes * 1000000000ull) / ((uint64_t)rate * fb);
	if (ns < 1000000ull)
		ns = 1000000ull;
	if (ns > 2000000000ull)
		ns = 2000000000ull;
	p->timer_period_ns[idx] = ns;
	hda_timer_arm(p, st);
}

/* ---- Skylake+ extended capabilities ------------------------------------------- */

static void hda_parse_xcaps(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint32_t off = hda_read32(c, HDA_REG_LLCH) & HDA_XCAP_NEXT_MASK;

	for (int n = 0; off && n < HDA_XCAP_MAX_WALK; n++) {
		uint32_t hdr, id;

		if ((off & 3) || off + 8 > c->mmio_size || off < 0x80)
			break;
		hdr = hda_read32(c, off);
		if (hdr == 0xffffffffu)
			break;
		id = (hdr >> HDA_XCAP_ID_SHIFT) & HDA_XCAP_ID_MASK;
		switch (id) {
		case HDA_XCAP_ID_GTS:
			c->gtscap = off;
			break;
		case HDA_XCAP_ID_ML:
			c->mlcap = off;
			break;
		case HDA_XCAP_ID_PP:
			c->ppcap = off;
			break;
		case HDA_XCAP_ID_SPB:
			c->spbcap = off;
			break;
		case HDA_XCAP_ID_DRSM:
			c->drsmcap = off;
			break;
		default:
			break;
		}
		uint32_t next = hdr & HDA_XCAP_NEXT_MASK;
		if (next <= off)
			break; /* the list only goes forward */
		off = next;
	}
}

/* Host DMA straight to the link (coupled mode) and no SPIB limits: the
 * state a DSP firmware or the BIOS may have left behind would starve the
 * streams. */
static void hda_skl_pp_spb(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;

	if (c->ppcap) {
		uint32_t ppctl = hda_read32(c, c->ppcap + HDA_PP_PPCTL);
		if (ppctl != 0xffffffffu &&
		    (ppctl & (HDA_PPCTL_GPROCEN | HDA_PPCTL_PIE))) {
			hda_write32(c, c->ppcap + HDA_PP_PPCTL,
				    ppctl & ~(HDA_PPCTL_GPROCEN | HDA_PPCTL_PIE | 0x3fffffffu));
			snd_info(HDA_DRV, "processing pipe was decoupled, now coupled\n");
		}
	}
	if (c->spbcap)
		hda_write32(c, c->spbcap + HDA_SPB_SPBFCCTL, 0);
}

static int ml_set_power(struct hda_controller *c, uint32_t base, int on)
{
	uint32_t v = hda_read32(c, base + HDA_ML_LCTL);

	if (v == 0xffffffffu)
		return -ENODEV;
	v &= ~HDA_ML_LCTL_SPA;
	if (on)
		v |= HDA_ML_LCTL_SPA;
	hda_write32(c, base + HDA_ML_LCTL, v);
	for (int i = 0; i < 200; i++) {
		uint32_t r = hda_read32(c, base + HDA_ML_LCTL);
		if (r == 0xffffffffu)
			return -ENODEV;
		if (!!(r & HDA_ML_LCTL_CPA) == !!on)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

/* Link 0 is the HDA link of the codecs.  Make sure it is powered and that
 * its clock is not the 6 MHz power-on default (codecs on the newer parts
 * need 24 MHz to enumerate reliably). */
static void hda_skl_links(struct hda_priv *p)
{
	static const uint8_t scf_pref[] = { 2, 3, 1, 4, 5 }; /* 24, 48, 12, 96, 192 MHz */
	struct hda_controller *c = &p->c;
	uint32_t mlcd, base, lcap, lctl;

	if (!c->mlcap)
		return;
	mlcd = hda_read32(c, c->mlcap + HDA_ML_MLCD);
	if (mlcd == 0xffffffffu)
		return;
	p->ml_links = (mlcd & 0xf) + 1;

	base = c->mlcap + HDA_ML_LINK_BASE;
	if (base + HDA_ML_LINK_STRIDE > c->mmio_size)
		return;
	lcap = hda_read32(c, base + HDA_ML_LCAP);
	lctl = hda_read32(c, base + HDA_ML_LCTL);
	if (lcap == 0xffffffffu || lctl == 0xffffffffu)
		return;
	if (lcap & HDA_ML_LCAP_ALT)
		return; /* not an HDA link */

	/* SPA and CPA must agree before SPA may be changed */
	for (int i = 0; i < 100; i++) {
		if (!!(lctl & HDA_ML_LCTL_SPA) == !!(lctl & HDA_ML_LCTL_CPA))
			break;
		lapic_delay_us(10);
		lctl = hda_read32(c, base + HDA_ML_LCTL);
	}

	if ((lctl & HDA_ML_LCTL_SCF_MASK) == 0 && (lcap & 0x3e)) {
		uint32_t scf = 0;
		for (unsigned i = 0; i < sizeof(scf_pref); i++) {
			if (lcap & (1u << scf_pref[i])) {
				scf = scf_pref[i];
				break;
			}
		}
		if (scf) {
			int rc = ml_set_power(c, base, 0);
			lapic_delay_us(100);
			if (rc == 0) {
				lctl = hda_read32(c, base + HDA_ML_LCTL);
				lctl = (lctl & ~HDA_ML_LCTL_SCF_MASK) | scf;
				hda_write32(c, base + HDA_ML_LCTL, lctl);
				p->ml_scf = scf;
			}
		}
	} else {
		p->ml_scf = lctl & HDA_ML_LCTL_SCF_MASK;
	}
	if (ml_set_power(c, base, 1))
		snd_info(HDA_DRV, "HDA link did not power up\n");
	lapic_delay_us(100);
}

/* ---- display coordination ------------------------------------------------------ */

static void hda_display_power(struct hda_priv *p)
{
	void *disp = NULL;
	const struct snd_display_audio_ops *ops;

	if (p->display_power_held)
		return;
	if (!(p->c.flags & (HDA_F_PCH_PWR_WELL | HDA_F_HSW_DISPLAY)))
		return;
	ops = snd_display_audio_ops(&disp);
	if (!ops || !ops->get_power)
		return;
	ops->get_power(disp);
	p->display_power_held = 1;
}

/* Haswell/Broadwell display HDA: the 24 MHz link clock is derived from the
 * display core clock, BCLK = CDCLK * M / N. */
static void hda_hsw_set_bclk(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint16_t m, n;

	if (!(c->flags & HDA_F_HSW_DISPLAY))
		return;
	switch (g_hda_cdclk_khz) {
	case 337500:
		m = 16;
		n = 225;
		break;
	case 540000:
		m = 4;
		n = 90;
		break;
	case 675000:
		m = 8;
		n = 225;
		break;
	case 450000:
	default:
		m = 4;
		n = 75;
		break;
	}
	hda_write16(c, HDA_REG_HSW_EM4, m);
	hda_write16(c, HDA_REG_HSW_EM5, n);
}

void hda_controller_set_cdclk(int cdclk_khz)
{
	if (cdclk_khz <= 0)
		return;
	g_hda_cdclk_khz = cdclk_khz;
	for (int i = 0; i < g_hda_count; i++) {
		struct hda_priv *p = g_hda[i];
		if (p && !p->released && !hda_ctrl_dead(&p->c))
			hda_hsw_set_bclk(p);
	}
}

/* ---- controller reset and initialisation -------------------------------------- */

static int hda_ctrl_reset(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint32_t g;
	int i;

	/* forget old state changes, quiesce DMA */
	hda_write16(c, HDA_REG_STATESTS, HDA_STATESTS_MASK);
	hda_streams_stop_all(c);
	cmd_io_stop(p);
	hda_write32(c, HDA_REG_INTCTL, 0);

	/* into reset */
	g = hda_read32(c, HDA_REG_GCTL);
	if (g == 0xffffffffu) {
		hda_check_gone(p);
		return -ENODEV;
	}
	hda_write32(c, HDA_REG_GCTL, g & ~HDA_GCTL_CRST);
	for (i = 0; i < HDA_RESET_TIMEOUT_US / 100; i++) {
		g = hda_read32(c, HDA_REG_GCTL);
		if (g == 0xffffffffu || !(g & HDA_GCTL_CRST))
			break;
		lapic_delay_us(100);
	}
	if (g == 0xffffffffu)
		return -ENODEV;
	/* hold reset long enough for every codec to see it */
	lapic_delay_us(HDA_RESET_HOLD_US);

	/* out of reset */
	hda_write32(c, HDA_REG_GCTL, hda_read32(c, HDA_REG_GCTL) | HDA_GCTL_CRST);
	for (i = 0; i < HDA_RESET_TIMEOUT_US / 100; i++) {
		g = hda_read32(c, HDA_REG_GCTL);
		if (g == 0xffffffffu || (g & HDA_GCTL_CRST))
			break;
		lapic_delay_us(100);
	}
	if (g == 0xffffffffu || !(g & HDA_GCTL_CRST))
		return -EIO;

	/* codecs need >= 521 us after reset to request their address */
	if (c->flags & HDA_F_CTX_RESET_WAIT)
		lapic_delay_us(HDA_CTX_WAKE_US);
	else
		lapic_delay_us(tmo(c, HDA_CODEC_WAKE_US));
	return 0;
}

/* Reset and bring up everything up to (not including) the codecs.
 * Leaves the codec mask in c->codec_mask. */
static int hda_init_chip(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	const pci_device_t *pci = c->pci;
	uint32_t cgctl = 0;
	uint16_t mask;
	int rc;

	/* Skylake+: miscellaneous backbone clock gating off across reset */
	if (c->flags & HDA_F_SKL_PLUS) {
		cgctl = pci_cfg_read32_dev(pci, HDA_PCI_INTEL_CGCTL);
		if (cgctl != 0xffffffffu)
			pci_cfg_write32_dev(pci, HDA_PCI_INTEL_CGCTL,
					    cgctl & ~HDA_PCI_CGCTL_MISCBDCGE);
	}

	rc = hda_ctrl_reset(p);
	if (rc == -EIO) {
		/* some controllers want a second attempt after a pause */
		lapic_delay_ms(10);
		rc = hda_ctrl_reset(p);
	}

	if ((c->flags & HDA_F_SKL_PLUS) && cgctl != 0xffffffffu)
		pci_cfg_write32_dev(pci, HDA_PCI_INTEL_CGCTL,
				    cgctl | HDA_PCI_CGCTL_MISCBDCGE);
	if (rc)
		return rc;

	mask = hda_read16(c, HDA_REG_STATESTS);
	if (mask == 0xffff) {
		hda_check_gone(p);
		return -ENODEV;
	}

	if (c->flags & HDA_F_SKL_PLUS) {
		hda_skl_pp_spb(p);
		hda_skl_links(p);
		/* a link brought up just now announces its codecs again */
		uint16_t m2 = hda_read16(c, HDA_REG_STATESTS);
		if (m2 != 0xffff)
			mask |= m2;
	}
	/* Apollo Lake: fetch earlier to avoid noise on 24/32-bit streams */
	if (pci->vendor_id == 0x8086 && pci->device_id == 0x5a98) {
		uint32_t v = hda_read32(c, HDA_REG_VS_EM4L);
		if (v != 0xffffffffu)
			hda_write32(c, HDA_REG_VS_EM4L, v & HDA_EM4L_DMA_LATENCY_KEEP);
	}
	hda_hsw_set_bclk(p);

	mask &= HDA_STATESTS_MASK;
	if (c->flags & HDA_F_PROBE_MASK_1)
		mask &= 1;
	if (mask == HDA_STATESTS_MASK) {
		/* every address claims a codec: a floating SDIN.  Real links
		 * have at most four codecs. */
		snd_info(HDA_DRV, "implausible codec mask 0x7fff, probing 0-3 only\n");
		mask = 0x000f;
	}
	c->codec_mask = mask;
	hda_write16(c, HDA_REG_STATESTS, HDA_STATESTS_MASK);

	/* codec state changes wake us (hot-plugged display codecs) */
	hda_write16(c, HDA_REG_WAKEEN, HDA_STATESTS_MASK);

	rc = cmd_io_init(p);
	if (rc)
		return rc;

	/* position buffer */
	if (c->posbuf) {
		kmemset((void *)c->posbuf, 0, 4096);
		hda_write32(c, HDA_REG_DPLBASE, (uint32_t)c->posbuf_phys | HDA_DPLBASE_ENABLE);
		hda_write32(c, HDA_REG_DPUBASE, (uint32_t)(c->posbuf_phys >> 32));
	}

	/* interrupts: controller and global; stream bits as they start */
	if (p->irq_mode && !c->polling)
		hda_write32(c, HDA_REG_INTCTL, HDA_INT_GIE | HDA_INT_CIE);
	else
		hda_write32(c, HDA_REG_INTCTL, 0);
	return 0;
}

static void hda_setup_irq(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	const pci_device_t *pci = c->pci;
	int vec = -1, rc = -ENODEV;

	c->irq_vector = -1;
	c->irq_gsi = -1;
	c->msi = 0;
	p->irq_mode = 0;

	if (!(c->flags & HDA_F_NO_MSI)) {
		p->msi_cap = pci_find_capability(pci, PCI_CAP_MSI);
		if (p->msi_cap) {
			rc = irq_request_msi(pci, 0, hda_irq, p, "hda", &vec);
			if (rc == 0) {
				c->irq_vector = vec;
				c->msi = 1;
				p->irq_mode = 1;
				p->irq_registered = 1;
				return;
			}
			p->msi_cap = 0;
		}
	}

	int gsi = hda_intx_gsi(pci);
	if (gsi >= 0) {
		uint16_t cmd = pci_cfg_read16(pci, 0x04);
		if (cmd != 0xffff && (cmd & PCI_CMD_INTX_DISABLE))
			pci_cfg_write16(pci, 0x04, (uint16_t)(cmd & ~PCI_CMD_INTX_DISABLE));
		rc = irq_request_gsi(gsi, hda_irq, p, "hda", 1, 1);
		if (rc == 0) {
			c->irq_gsi = gsi;
			c->irq_vector = 32 + gsi;
			p->irq_mode = 2;
			p->irq_registered = 1;
			return;
		}
	}
	snd_info(HDA_DRV, "%s: no usable interrupt (%d), polling\n", c->bus, rc);
	c->polling = 1;
}

static void hda_free_irq(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;

	if (!p->irq_registered)
		return;
	hda_write32(c, HDA_REG_INTCTL, 0);
	irq_free(c->irq_vector, hda_irq, p);
	if (p->irq_mode == 1) {
		hda_msi_disable(p);
		irq_free_vector(c->irq_vector);
	}
	p->irq_registered = 0;
	p->irq_mode = 0;
	c->irq_vector = -1;
}

/* ---- codecs ---------------------------------------------------------------------- */

static struct hda_codec *analog_codec(struct hda_controller *c)
{
	struct hda_priv *p = priv_of(c);

	if (p && p->analog_addr >= 0 && p->analog_addr < HDA_MAX_CODECS &&
	    c->codec[p->analog_addr])
		return c->codec[p->analog_addr];
	for (int a = 0; a < HDA_MAX_CODECS; a++) {
		struct hda_codec *cd = c->codec[a];
		if (cd && cd->afg && !cd->is_hdmi)
			return cd;
	}
	return NULL;
}

static int hda_probe_codecs(struct hda_priv *p, uint16_t mask)
{
	struct hda_controller *c = &p->c;
	int found = 0;

	for (int a = 0; a < HDA_MAX_CODECS; a++) {
		struct hda_codec *cd = NULL;
		int rc;

		if (!(mask & (1u << a)) || (p->probed_mask & (1u << a)))
			continue;
		if (hda_ctrl_dead(c))
			break;
		rc = hda_codec_probe(c, a, &cd);
		if (rc || !cd) {
			p->failed_mask |= (uint16_t)(1u << a);
			snd_info(HDA_DRV, "codec %d: probe failed (%d)\n", a, rc);
			continue;
		}
		c->codec[a] = cd;
		c->ncodecs++;
		p->probed_mask |= (uint16_t)(1u << a);
		p->failed_mask &= (uint16_t)~(1u << a);
		found++;
		/* the codec layer logs vendor and chip */
		if (p->analog_addr < 0 && cd->afg && !cd->is_hdmi)
			p->analog_addr = a;
	}
	return found;
}

/* Without unsolicited responses the codecs must poll their jacks. */
static void hda_mark_jack_poll(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	int need = (c->flags & HDA_F_NO_UNSOL) || c->polling ||
		   (c->single_cmd && !(c->flags & HDA_F_SINGLE_CMD));

	if (!need)
		return;
	for (int a = 0; a < HDA_MAX_CODECS; a++)
		if (c->codec[a])
			c->codec[a]->spec.jack_poll = 1;
}

static void hda_update_dsp_owned(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	int was = p->dsp_owned;

	p->dsp_owned = (c->flags & HDA_F_DSP_PRESENT) && !analog_codec(c);
	if (p->dsp_owned && !was)
		snd_info(HDA_DRV, "%s: audio DSP present and no analog codec on the HDA link: "
			 "speakers/microphones need the SOF driver; HDA keeps the display codecs\n",
			 c->bus);
}

/* ---- PCM operations (the analog PCM) ---------------------------------------------- */

static uint32_t fmt_bits(uint32_t fmt)
{
	switch (fmt) {
	case AUDIO_FMT_U8:
		return 8;
	case AUDIO_FMT_S16_LE:
		return 16;
	case AUDIO_FMT_S20_LE:
		return 20;
	case AUDIO_FMT_S24_LE:
		return 24;
	case AUDIO_FMT_S32_LE:
		return 32;
	default:
		return 0;
	}
}

static int hda_pcm_open(struct snd_pcm_substream *ss)
{
	struct hda_codec *cd = ss->pcm ? ss->pcm->private_data : NULL;
	struct hda_controller *c;
	struct hda_stream *st;

	if (!cd || !(c = cd->ctrl))
		return -ENODEV;
	if (hda_ctrl_dead(c))
		return -ENODEV;
	st = hda_stream_assign(c, ss->direction);
	if (!st)
		return -EBUSY;
	st->substream = ss;
	st->codec = cd;
	st->hdmi = 0;
	ss->private_data = st;
	return 0;
}

static void hda_pcm_close(struct snd_pcm_substream *ss)
{
	struct hda_stream *st = ss->private_data;
	struct hda_priv *p;

	if (!st)
		return;
	p = priv_of(st->ctrl);
	if (p)
		hda_timer_disarm(p, st);
	hda_stream_release(st);
	ss->private_data = NULL;
}

static int hda_pcm_hw_params(struct snd_pcm_substream *ss)
{
	struct hda_stream *st = ss->private_data;
	struct hda_codec *cd;
	struct hda_priv *p;
	uint32_t bits, rate, ch;
	uint16_t fmt;
	int rc;

	if (!st || !(cd = st->codec) || !(p = priv_of(st->ctrl)))
		return -EINVAL;
	bits = fmt_bits(ss->params.format);
	rate = ss->params.rate;
	ch = ss->params.channels;
	if (!bits)
		return -EINVAL;
	fmt = hda_calc_format(rate, ch, bits);
	if (!fmt)
		return -EINVAL;

	rc = hda_stream_setup(st, ss->dma_phys, ss->buffer_bytes, ss->period_bytes, fmt);
	if (rc)
		return rc;
	return hda_codec_setup_stream(cd, ss->direction, st->tag, (int)ch, fmt);
}

static int hda_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct hda_stream *st = ss->private_data;
	struct hda_priv *p;

	if (!st)
		return 0;
	p = priv_of(st->ctrl);
	if (p)
		hda_timer_disarm(p, st);
	hda_stream_stop(st);
	if (st->codec && !hda_ctrl_dead(st->ctrl))
		hda_codec_cleanup_stream(st->codec, ss->direction);
	hda_stream_reset(st);
	st->format = 0;
	st->nbdl = 0;
	return 0;
}

static int hda_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct hda_stream *st = ss->private_data;
	struct hda_priv *p;

	if (!st || !st->format || !st->buffer_bytes)
		return -EINVAL;
	if (hda_ctrl_dead(st->ctrl))
		return -ENODEV;
	p = priv_of(st->ctrl);
	if (p)
		hda_timer_disarm(p, st);
	/* reprogram from scratch: the reset puts the position at 0 */
	return hda_stream_setup(st, st->buf_phys, st->buffer_bytes, st->period_bytes,
				st->format);
}

static int hda_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct hda_stream *st = ss->private_data;
	struct hda_priv *p;

	if (!st || !(p = priv_of(st->ctrl)))
		return -EINVAL;
	if (hda_ctrl_dead(st->ctrl))
		return -ENODEV;

	switch (cmd) {
	case SND_TRIGGER_START:
	case SND_TRIGGER_RESUME:
		if (!st->nbdl)
			return -EINVAL;
		hda_stream_start(st); /* arms the timer/watchdog */
		return 0;
	case SND_TRIGGER_STOP:
	case SND_TRIGGER_PAUSE:
		/* RUN off keeps the position: resuming continues there */
		hda_stream_stop(st);
		return 0;
	default:
		return -EINVAL;
	}
}

static uint32_t hda_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct hda_stream *st = ss->private_data;
	uint32_t pos;

	if (!st)
		return 0;
	pos = hda_stream_position(st);
	if (ss->buffer_bytes && pos >= ss->buffer_bytes)
		pos = 0;
	return pos;
}

static uint32_t hda_pcm_delay(struct snd_pcm_substream *ss)
{
	struct hda_stream *st = ss->private_data;
	uint32_t bytes = 0;

	if (!st || !ss->frame_bytes)
		return 0;
	if (ss->direction == AUDIO_DIR_PLAYBACK)
		bytes += hda_stream_fifo_bytes(st);
	bytes += hda_stream_link_delay(st);
	return bytes / ss->frame_bytes;
}

static const struct snd_pcm_ops hda_analog_ops = {
	.open = hda_pcm_open,
	.close = hda_pcm_close,
	.hw_params = hda_pcm_hw_params,
	.hw_free = hda_pcm_hw_free,
	.prepare = hda_pcm_prepare,
	.trigger = hda_pcm_trigger,
	.pointer = hda_pcm_pointer,
	.delay = hda_pcm_delay,
};

/* The controller's limits on top of what the codec can do. */
static void hda_fix_hw(struct hda_controller *c, struct snd_pcm_hw *hw)
{
	if (!hw->formats)
		return;
	hw->formats &= AUDIO_FMT_BIT(AUDIO_FMT_U8) | AUDIO_FMT_BIT(AUDIO_FMT_S16_LE) |
		       AUDIO_FMT_BIT(AUDIO_FMT_S20_LE) | AUDIO_FMT_BIT(AUDIO_FMT_S24_LE) |
		       AUDIO_FMT_BIT(AUDIO_FMT_S32_LE);
	if (!hw->formats)
		hw->formats = AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
	if (!hw->rates)
		hw->rates = AUDIO_RATE_48000;
	if (!hw->channels_min)
		hw->channels_min = 1;
	if (!hw->channels_max || hw->channels_max > 16)
		hw->channels_max = hw->channels_max ? 16 : 2;
	if (hw->channels_min > hw->channels_max)
		hw->channels_min = hw->channels_max;

	if (hw->period_align < 128)
		hw->period_align = 128;
	if (hw->buffer_align < 128)
		hw->buffer_align = 128;
	if (hw->period_bytes_min < 128)
		hw->period_bytes_min = 128;
	if (hw->periods_min < 2)
		hw->periods_min = 2;

	uint32_t pmax = HDA_MAX_BDL, bmax = 4u << 20, perb = 2u << 20;
	if (c->flags & HDA_F_ALIGN_BDLE_4K) {
		/* a period may need ceil(P/4K)+1 entries: keep within 256 */
		pmax = 32;
		perb = 7 * 4096;
	}
	if (!hw->periods_max || hw->periods_max > pmax)
		hw->periods_max = pmax;
	if (!hw->period_bytes_max || hw->period_bytes_max > perb)
		hw->period_bytes_max = perb;
	if (!hw->buffer_bytes_max || hw->buffer_bytes_max > bmax)
		hw->buffer_bytes_max = bmax;
	if (hw->period_bytes_min > hw->period_bytes_max)
		hw->period_bytes_min = hw->period_bytes_max;
	if (hw->periods_min > hw->periods_max)
		hw->periods_min = hw->periods_max;
	hw->dma32 = !c->addr64;
	/* rings are mapped uncached: correct whatever the snoop setting */
	hw->dma_coherent = 0;
}

/* ---- mixer controls (delegated to the codecs) ------------------------------------ */

/* The codecs in the order controls are tried: analog first. */
static int ctl_codecs(struct hda_controller *c, struct hda_codec **out, int max)
{
	struct hda_codec *an = analog_codec(c);
	int n = 0;

	if (an && n < max)
		out[n++] = an;
	for (int a = 0; a < HDA_MAX_CODECS && n < max; a++) {
		struct hda_codec *cd = c->codec[a];
		if (cd && cd != an && cd->afg)
			out[n++] = cd;
	}
	return n;
}

static struct hda_controller *card_ctrl(struct snd_card *card)
{
	struct hda_controller *c = card ? card->private_data : NULL;

	if (!c || !priv_of(c))
		return NULL;
	return c;
}

static int hda_ctl_get_volume(struct snd_card *card, uint32_t ctl,
			      struct audio_volume *v)
{
	struct hda_controller *c = card_ctrl(card);
	struct hda_codec *cs[HDA_MAX_CODECS];
	int n, rc = -ENOENT;

	if (!c || !v)
		return -ENODEV;
	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	n = ctl_codecs(c, cs, HDA_MAX_CODECS);
	for (int i = 0; i < n; i++) {
		rc = hda_codec_get_volume(cs[i], ctl, v);
		if (rc != -ENOENT)
			break;
	}
	return rc;
}

static int hda_ctl_set_volume(struct snd_card *card, uint32_t ctl, uint32_t l,
			      uint32_t r)
{
	struct hda_controller *c = card_ctrl(card);
	struct hda_codec *cs[HDA_MAX_CODECS];
	int n, rc = -ENOENT;

	if (!c)
		return -ENODEV;
	if (ctl > AUDIO_CTL_MAX || l > 100 || r > 100)
		return -EINVAL;
	if (hda_ctrl_dead(c))
		return -ENODEV;
	n = ctl_codecs(c, cs, HDA_MAX_CODECS);
	for (int i = 0; i < n; i++) {
		rc = hda_codec_set_volume(cs[i], ctl, l, r);
		if (rc != -ENOENT)
			break;
	}
	return rc;
}

static int hda_ctl_get_mute(struct snd_card *card, uint32_t ctl, uint32_t *mute)
{
	struct hda_controller *c = card_ctrl(card);
	struct hda_codec *cs[HDA_MAX_CODECS];
	int n, rc = -ENOENT;

	if (!c || !mute)
		return -ENODEV;
	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	n = ctl_codecs(c, cs, HDA_MAX_CODECS);
	for (int i = 0; i < n; i++) {
		rc = hda_codec_get_mute(cs[i], ctl, mute);
		if (rc != -ENOENT)
			break;
	}
	return rc;
}

static int hda_ctl_set_mute(struct snd_card *card, uint32_t ctl, uint32_t mute)
{
	struct hda_controller *c = card_ctrl(card);
	struct hda_codec *cs[HDA_MAX_CODECS];
	int n, rc = -ENOENT;

	if (!c)
		return -ENODEV;
	if (ctl > AUDIO_CTL_MAX || mute > 1)
		return -EINVAL;
	if (hda_ctrl_dead(c))
		return -ENODEV;
	n = ctl_codecs(c, cs, HDA_MAX_CODECS);
	for (int i = 0; i < n; i++) {
		rc = hda_codec_set_mute(cs[i], ctl, mute);
		if (rc != -ENOENT)
			break;
	}
	return rc;
}

/* Outputs of all codecs in one list: the analog codec's first (it also
 * decides mode/selected), then the display codecs'.  An id already in the
 * list is not added twice. */
typedef int (*list_fn)(struct hda_codec *c, struct audio_outputs *o);

static int merge_list(struct hda_controller *c, struct audio_outputs *o, list_fn fn)
{
	struct hda_codec *cs[HDA_MAX_CODECS];
	struct audio_outputs *tmp;
	int n, any = 0, first_rc = -ENOENT;

	kmemset(o, 0, sizeof(*o));
	tmp = kalloc(sizeof(*tmp));
	if (!tmp)
		return -ENOMEM;
	n = ctl_codecs(c, cs, HDA_MAX_CODECS);
	for (int i = 0; i < n; i++) {
		kmemset(tmp, 0, sizeof(*tmp));
		int rc = fn(cs[i], tmp);
		if (rc) {
			if (first_rc == -ENOENT)
				first_rc = rc;
			continue;
		}
		if (!any) {
			o->mode = tmp->mode;
			o->selected = tmp->selected;
		}
		any = 1;
		uint32_t cnt = tmp->count > AUDIO_MAX_OUTPUTS ? AUDIO_MAX_OUTPUTS : tmp->count;
		for (uint32_t k = 0; k < cnt && o->count < AUDIO_MAX_OUTPUTS; k++) {
			int dup = 0;
			for (uint32_t j = 0; j < o->count; j++)
				if (o->out[j].id == tmp->out[k].id)
					dup = 1;
			if (!dup)
				o->out[o->count++] = tmp->out[k];
		}
	}
	kfree(tmp);
	if (!any)
		return first_rc == -ENOENT ? -ENOENT : first_rc;
	return 0;
}

static int hda_ctl_get_outputs(struct snd_card *card, struct audio_outputs *o)
{
	struct hda_controller *c = card_ctrl(card);

	if (!c || !o)
		return -ENODEV;
	return merge_list(c, o, hda_codec_get_outputs);
}

static int hda_ctl_get_inputs(struct snd_card *card, struct audio_outputs *o)
{
	struct hda_controller *c = card_ctrl(card);

	if (!c || !o)
		return -ENODEV;
	return merge_list(c, o, hda_codec_get_inputs);
}

/* Which codec owns output (or input) `id'? */
static struct hda_codec *owner_of(struct hda_controller *c, uint32_t id, list_fn fn)
{
	struct hda_codec *cs[HDA_MAX_CODECS];
	struct audio_outputs *tmp;
	struct hda_codec *found = NULL;
	int n;

	tmp = kalloc(sizeof(*tmp));
	if (!tmp)
		return NULL;
	n = ctl_codecs(c, cs, HDA_MAX_CODECS);
	for (int i = 0; i < n && !found; i++) {
		kmemset(tmp, 0, sizeof(*tmp));
		if (fn(cs[i], tmp))
			continue;
		uint32_t cnt = tmp->count > AUDIO_MAX_OUTPUTS ? AUDIO_MAX_OUTPUTS : tmp->count;
		for (uint32_t k = 0; k < cnt; k++)
			if (tmp->out[k].id == id)
				found = cs[i];
	}
	kfree(tmp);
	return found;
}

static int hda_ctl_set_output(struct snd_card *card, const struct audio_setoutput *s)
{
	struct hda_controller *c = card_ctrl(card);
	struct hda_codec *cd;

	if (!c || !s)
		return -ENODEV;
	if (s->mode > AUDIO_OUTSEL_ALL)
		return -EINVAL;
	if (hda_ctrl_dead(c))
		return -ENODEV;
	if (s->mode == AUDIO_OUTSEL_ID) {
		cd = owner_of(c, s->id, hda_codec_get_outputs);
		if (!cd)
			return -ENOENT;
		return hda_codec_set_output(cd, s);
	}
	cd = analog_codec(c);
	if (!cd) {
		struct hda_codec *cs[1];
		if (ctl_codecs(c, cs, 1) < 1)
			return -ENOENT;
		cd = cs[0];
	}
	return hda_codec_set_output(cd, s);
}

static int hda_ctl_set_input(struct snd_card *card, const struct audio_setoutput *s)
{
	struct hda_controller *c = card_ctrl(card);
	struct hda_codec *cd;

	if (!c || !s)
		return -ENODEV;
	if (s->mode > AUDIO_OUTSEL_ALL)
		return -EINVAL;
	if (hda_ctrl_dead(c))
		return -ENODEV;
	if (s->mode == AUDIO_OUTSEL_ID) {
		cd = owner_of(c, s->id, hda_codec_get_inputs);
		if (!cd)
			return -ENOENT;
		return hda_codec_set_input(cd, s);
	}
	cd = analog_codec(c);
	if (!cd)
		return -ENOENT;
	return hda_codec_set_input(cd, s);
}

/* ---- diagnostics -------------------------------------------------------------------- */

static void bp(char *buf, size_t len, size_t *pos, const char *fmt, ...)
{
	va_list ap;

	if (!buf || *pos + 1 >= len)
		return;
	__builtin_va_start(ap, fmt);
	kvsnprintf(buf + *pos, len - *pos, fmt, ap);
	__builtin_va_end(ap);
	*pos += kstrlen(buf + *pos);
	if (*pos >= len)
		*pos = len - 1;
}

static const char *posfix_name(uint32_t f)
{
	if (f & HDA_F_POSFIX_LPIB)
		return "LPIB";
	if (f & HDA_F_POSFIX_POSBUF)
		return "position buffer";
	if (f & HDA_F_POSFIX_COMBO)
		return "LPIB playback, position buffer capture";
	return "position buffer (auto)";
}

static void diag_summary(struct hda_priv *p, char *buf, size_t len, size_t *pos)
{
	struct hda_controller *c = &p->c;
	const pci_device_t *pci = c->pci;

	bp(buf, len, pos, "Controller: %s [%s]\n", c->name,
	   c->id && c->id->name ? c->id->name : "?");
	bp(buf, len, pos, "  PCI %04x:%04x subsys %04x:%04x rev %02x prog-if %02x at %s\n",
	   pci->vendor_id, pci->device_id, p->subsys_vendor, p->subsys_device,
	   p->revision, pci->prog_if, c->bus);
	bp(buf, len, pos, "  BAR0 %llx (%llu KiB), HDA %u.%u, GCAP %04x\n",
	   (unsigned long long)c->mmio_phys, (unsigned long long)(c->mmio_size >> 10),
	   c->vmaj, c->vmin, c->gcap);
	bp(buf, len, pos, "  Streams: %d in, %d out, %d bidirectional; %s DMA\n",
	   c->num_iss, c->num_oss, c->num_bss, c->addr64 ? "64-bit" : "32-bit");
	if (c->msi)
		bp(buf, len, pos, "  Interrupt: MSI, vector %d\n", c->irq_vector);
	else if (p->irq_mode == 2)
		bp(buf, len, pos, "  Interrupt: INTx, GSI %d (vector %d)\n", c->irq_gsi,
		   c->irq_vector);
	else
		bp(buf, len, pos, "  Interrupt: none\n");
	if (c->polling)
		bp(buf, len, pos, "  Periods: timer-driven\n");
	bp(buf, len, pos, "  Interrupts: %u total, %u stream, %u controller; "
			  "RIRB overruns %u, CORB errors %u\n",
	   p->irq_total, p->irq_stream, p->irq_ctrl, p->rirb_overruns, p->corb_errors);
	if (c->single_cmd)
		bp(buf, len, pos, "  Commands: immediate (%u sent), %u timeouts\n",
		   p->imm_cmds, c->cmd_timeouts);
	else
		bp(buf, len, pos, "  Commands: CORB %u / RIRB %u entries, %u sent, %u timeouts\n",
		   c->corb_entries, c->rirb_entries, p->cmds_sent, c->cmd_timeouts);
	bp(buf, len, pos, "  Unsolicited: %s, %u dropped\n",
	   (c->flags & HDA_F_NO_UNSOL) ? "off" : "on", p->unsol_dropped);
	bp(buf, len, pos, "  Position: %s; flags %08x\n", posfix_name(c->flags), c->flags);
	if (c->flags & HDA_F_SKL_PLUS)
		bp(buf, len, pos, "  Extended caps: PP %x SPB %x ML %x (%u links, SCF %u) "
				  "GTS %x DRSM %x\n",
		   c->ppcap, c->spbcap, c->mlcap, p->ml_links, p->ml_scf, c->gtscap,
		   c->drsmcap);
	if (c->flags & HDA_F_DSP_PRESENT)
		bp(buf, len, pos, "  Audio DSP: present%s\n",
		   p->dsp_owned ? ", owns the internal audio (SOF)" : "");
	bp(buf, len, pos, "  Codec mask %04x, probed %04x, failed %04x\n", c->codec_mask,
	   p->probed_mask, p->failed_mask);
	for (int a = 0; a < HDA_MAX_CODECS; a++) {
		struct hda_codec *cd = c->codec[a];
		if (!cd)
			continue;
		bp(buf, len, pos, "  Codec %d: %s %s, id %08x subsys %08x rev %08x%s%s\n", a,
		   cd->vendor_name, cd->chip_name, cd->vendor_id, cd->subsystem_id,
		   cd->revision_id, cd->is_hdmi ? ", HDMI/DP" : "",
		   (p->analog_addr == a) ? ", analog PCM" : "");
	}
}

static void diag_streams(struct hda_priv *p, char *buf, size_t len, size_t *pos)
{
	struct hda_controller *c = &p->c;

	bp(buf, len, pos, "Streams:\n");
	for (int i = 0; i < c->nstreams; i++) {
		struct hda_stream *st = &c->streams[i];
		const char *kind = i < c->num_iss ? "in" :
				   (i < c->num_iss + c->num_oss ? "out" : "bidi");
		uint32_t lpib = hda_read32(c, st->sd_off + HDA_SD_LPIB);
		uint32_t ctl = hda_read32(c, st->sd_off + HDA_SD_CTL);
		uint32_t pb = c->posbuf ? c->posbuf[i * HDA_POSBUF_STRIDE_DW] : 0;

		bp(buf, len, pos, "  #%d %-4s @%03x %s tag %d", i, kind, st->sd_off,
		   st->in_use ? (st->direction == AUDIO_DIR_PLAYBACK ? "play" : "capt") :
				"free",
		   st->tag);
		if (st->in_use && st->format)
			bp(buf, len, pos, " fmt %04x (%u Hz %uch %u bit) buf %u per %u bdl %u",
			   st->format, hda_format_rate(st->format),
			   hda_format_channels(st->format), hda_format_bits(st->format),
			   st->buffer_bytes, st->period_bytes, st->nbdl);
		bp(buf, len, pos, " ctl %06x sts %02x lpib %u posbuf %u irqs %u%s%s\n",
		   ctl & 0xffffffu, (ctl >> 24) & 0xff, lpib, pb, st->irq_count,
		   st->running ? " RUN" : "", st->hdmi ? " HDMI" : "");
	}
}

static long hda_ctl_diag(struct snd_card *card, uint32_t what, char *buf, size_t len)
{
	struct hda_controller *c = card_ctrl(card);
	struct hda_priv *p;
	size_t pos = 0;

	if (!c || !buf || len < 2)
		return -EINVAL;
	p = priv_of(c);
	buf[0] = '\0';
	diag_summary(p, buf, len, &pos);
	switch (what) {
	case AUDIO_DIAG_SUMMARY:
		break;
	case AUDIO_DIAG_STREAMS:
		diag_streams(p, buf, len, &pos);
		break;
	case AUDIO_DIAG_CODECS:
		for (int a = 0; a < HDA_MAX_CODECS && pos + 2 < len; a++) {
			struct hda_codec *cd = c->codec[a];
			if (!cd)
				continue;
			bp(buf, len, &pos, "\n");
			long n = hda_codec_dump(cd, buf + pos, len - pos);
			if (n > 0) {
				size_t real = kstrlen(buf + pos);
				pos += ((size_t)n < real) ? (size_t)n : real;
				if (pos >= len)
					pos = len - 1;
				buf[pos] = '\0';
			}
		}
		break;
	default:
		return -EINVAL;
	}
	return (long)pos;
}

static const struct snd_ctl_ops hda_ctl_ops = {
	.get_volume = hda_ctl_get_volume,
	.set_volume = hda_ctl_set_volume,
	.get_mute = hda_ctl_get_mute,
	.set_mute = hda_ctl_set_mute,
	.get_outputs = hda_ctl_get_outputs,
	.set_output = hda_ctl_set_output,
	.get_inputs = hda_ctl_get_inputs,
	.set_input = hda_ctl_set_input,
	.diag = hda_ctl_diag,
};

/* ---- the card ------------------------------------------------------------------------- */

static int hda_add_analog_pcm(struct hda_priv *p, struct hda_codec *an)
{
	struct hda_controller *c = &p->c;
	struct snd_pcm_hw play, cap;
	int rc;

	kmemset(&play, 0, sizeof(play));
	kmemset(&cap, 0, sizeof(cap));
	hda_codec_pcm_hw(an, AUDIO_DIR_PLAYBACK, &play);
	hda_codec_pcm_hw(an, AUDIO_DIR_CAPTURE, &cap);
	if (c->num_oss + c->num_bss == 0)
		play.formats = 0;
	if (c->num_iss + c->num_bss == 0)
		cap.formats = 0;
	hda_fix_hw(c, &play);
	hda_fix_hw(c, &cap);
	if (!play.formats && !cap.formats)
		return -ENODEV;
	rc = snd_pcm_new(c->card, "HDA Analog", &hda_analog_ops,
			 play.formats ? &play : NULL, cap.formats ? &cap : NULL, an,
			 &c->pcm_analog);
	if (rc) {
		snd_info(HDA_DRV, "%s: analog PCM: %d\n", c->bus, rc);
		c->pcm_analog = NULL;
		return rc;
	}
	if (play.formats)
		c->card->info_flags |= AUDIO_INFO_PLAYBACK;
	if (cap.formats)
		c->card->info_flags |= AUDIO_INFO_CAPTURE;
	return 0;
}

static void hda_add_hdmi_pcms(struct hda_priv *p, struct hda_codec *cd)
{
	struct hda_controller *c = &p->c;
	int before = c->card->npcm;
	int rc = hda_hdmi_add_pcms(cd);

	if (rc < 0) {
		snd_info(HDA_DRV, "codec %d: HDMI PCMs: %d\n", cd->addr, rc);
		return;
	}
	if (c->card->npcm > before)
		c->card->info_flags |= AUDIO_INFO_HDMI | AUDIO_INFO_PLAYBACK;
}

static void hda_card_strings(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	struct snd_card *card = c->card;
	struct hda_codec *an = analog_codec(c), *first = NULL;

	kstrncpy(card->name, c->name, sizeof(card->name) - 1);
	card->name[sizeof(card->name) - 1] = '\0';
	kstrncpy(card->bus, c->bus, sizeof(card->bus) - 1);
	card->bus[sizeof(card->bus) - 1] = '\0';
	if (!card->driver[0])
		kstrncpy(card->driver, "hda", sizeof(card->driver) - 1);

	if (!an)
		for (int a = 0; a < HDA_MAX_CODECS && !first; a++)
			if (c->codec[a])
				first = c->codec[a];
	if (!an)
		an = first;
	if (an)
		ksnprintf(card->codec, sizeof(card->codec), "%s %s",
			  an->vendor_name[0] ? an->vendor_name : "Unknown",
			  an->chip_name[0] ? an->chip_name : "codec");
	else
		card->codec[0] = '\0';
}

static void hda_card_counts(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	struct snd_card *card = c->card;
	struct audio_outputs *o = kalloc(sizeof(*o));

	if (!o)
		return;
	if (merge_list(c, o, hda_codec_get_outputs) == 0) {
		card->num_outputs = o->count;
		for (uint32_t i = 0; i < o->count && i < AUDIO_MAX_OUTPUTS; i++)
			if (o->out[i].flags & AUDIO_OUTF_JACK_DETECT)
				card->info_flags |= AUDIO_INFO_JACK_DETECT;
	}
	if (merge_list(c, o, hda_codec_get_inputs) == 0)
		card->num_inputs = o->count;
	kfree(o);
}

static int hda_create_card(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	struct snd_card *card = NULL;
	struct hda_codec *an;
	int rc;

	if (c->card || !c->ncodecs)
		return c->card ? 0 : -ENODEV;

	rc = snd_card_new("hda", c->pci, &card);
	if (rc || !card) {
		snd_info(HDA_DRV, "%s: cannot create the card (%d)\n", c->bus, rc);
		return rc ? rc : -ENOMEM;
	}
	c->card = card;
	card->private_data = c;
	card->ctl = &hda_ctl_ops;
	card->info_flags |= AUDIO_INFO_MMAP;

	an = analog_codec(c);
	if (an && !p->dsp_owned) {
		if (hda_add_analog_pcm(p, an) == 0)
			card->info_flags |= AUDIO_INFO_HW_VOLUME;
	}
	for (int a = 0; a < HDA_MAX_CODECS; a++) {
		struct hda_codec *cd = c->codec[a];
		if (cd && cd->is_hdmi && card->npcm < SND_MAX_PCMS)
			hda_add_hdmi_pcms(p, cd);
	}
	if (card->npcm == 0) {
		snd_info(HDA_DRV, "%s: no playable function on any codec\n", c->bus);
		c->card = NULL;
		c->pcm_analog = NULL;
		snd_card_free(card);
		return -ENODEV;
	}
	hda_card_strings(p);
	hda_card_counts(p);

	rc = snd_card_register(card);
	if (rc) {
		snd_info(HDA_DRV, "%s: card registration failed (%d)\n", c->bus, rc);
		c->card = NULL;
		c->pcm_analog = NULL;
		snd_card_free(card);
		return rc;
	}
	p->card_registered = 1;
	snd_info(HDA_DRV, "card %d: %s, %s, %d PCM%s\n", card->index, card->name,
		 card->codec[0] ? card->codec : "no codec name", card->npcm,
		 card->npcm == 1 ? "" : "s");
	return 0;
}

/* ---- probe ------------------------------------------------------------------------------- */

static void hda_teardown(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;

	if (c->mmio && !hda_ctrl_dead(c)) {
		hda_streams_stop_all(c);
		cmd_io_stop(p);
		hda_write32(c, HDA_REG_INTCTL, 0);
		hda_write32(c, HDA_REG_DPLBASE, 0);
	}
	hda_free_irq(p);
	hda_streams_fini(c);
	if (p->cmdbuf) {
		snd_dma_free(p->cmdbuf, 4096);
		p->cmdbuf = NULL;
		c->corb = NULL;
		c->rirb = NULL;
	}
	if (p->posbuf_page) {
		snd_dma_free(p->posbuf_page, 4096);
		p->posbuf_page = NULL;
		c->posbuf = NULL;
	}
	if (c->mmio) {
		mm_unmap_mmio((uint64_t)(uintptr_t)c->mmio, p->mmio_pages);
		c->mmio = NULL;
	}
}

static int hda_probe_one(const pci_device_t *pci, const struct hda_pci_id *id)
{
	struct hda_priv *p;
	struct hda_controller *c;
	struct pci_bar bar;
	uint32_t sub;
	uint16_t gcap;
	int rc;

	if (g_hda_count >= HDA_MAX_CTRLS)
		return -ENOSPC;
	if (pci_bar_decode(pci, 0, &bar) != 0 || (bar.flags & PCI_BAR_IO) ||
	    !bar.base || bar.size < 0x1000) {
		snd_info(HDA_DRV, "%02x:%02x.%x: no usable register window\n", pci->bus,
			 pci->device, pci->function);
		return -ENODEV;
	}

	p = kcalloc(1, sizeof(*p));
	if (!p)
		return -ENOMEM;
	c = &p->c;
	p->index = g_hda_count;
	p->analog_addr = -1;
	c->pci = pci;
	c->id = id;
	c->flags = id->flags;
	spinlock_init(&c->cmd_lock, "hda_cmd");
	ksnprintf(c->bus, sizeof(c->bus), "pci:0000:%02x:%02x.%x", pci->bus, pci->device,
		  pci->function);
	kstrncpy(c->name, hda_pci_card_name(id, pci), sizeof(c->name) - 1);
	for (int i = 0; i < HDA_MAX_STREAMS; i++)
		hrtimer_init(&p->timer[i], hda_timer_fn, &c->streams[i]);

	/* identification and runtime quirks */
	p->revision = pci_cfg_read8(pci, 0x08);
	sub = pci_cfg_read32_dev(pci, 0x2c);
	p->subsys_vendor = (uint16_t)sub;
	p->subsys_device = (uint16_t)(sub >> 16);
	if (pci->prog_if == 0x80)
		c->flags |= HDA_F_DSP_PRESENT;
	if (p->subsys_vendor == 0x1af4 || p->subsys_vendor == 0x80ee ||
	    p->subsys_vendor == 0x15ad || pci->vendor_id == 0x15ad)
		c->flags |= HDA_F_VM;
	if (pci->vendor_id == 0x1002 && (c->flags & HDA_F_ATI_SNOOP) && ati_sb_is_old())
		c->flags |= HDA_F_NO_64BIT;

	/* PCI: power, decode, map */
	hda_pci_set_d0(pci);
	pci_enable_busmaster_mem(pci);
	c->mmio_phys = bar.base;
	c->mmio_size = bar.size > (1u << 20) ? (1u << 20) : bar.size;
	p->mmio_pages = (uint32_t)((c->mmio_size + 4095) / 4096);
	{
		uint64_t v = mm_map_mmio_flags(c->mmio_phys, p->mmio_pages, MM_MMIO_UC);
		if (!v) {
			snd_info(HDA_DRV, "%s: cannot map the registers\n", c->bus);
			kfree(p);
			return -ENOMEM;
		}
		c->mmio = (volatile uint8_t *)(uintptr_t)v;
	}

	gcap = hda_read16(c, HDA_REG_GCAP);
	if (gcap == 0xffff) {
		snd_info(HDA_DRV, "%s: registers read all-ones, skipped\n", c->bus);
		hda_teardown(p);
		kfree(p);
		return -ENODEV;
	}
	hda_pci_fixups(p);

	/* registered now: hda_cmd and the interrupt handler look us up */
	g_hda[g_hda_count++] = p;

	c->gcap = gcap;
	c->vmaj = hda_read8(c, HDA_REG_VMAJ);
	c->vmin = hda_read8(c, HDA_REG_VMIN);
	c->num_iss = (gcap & HDA_GCAP_ISS_MASK) >> HDA_GCAP_ISS_SHIFT;
	c->num_oss = (gcap & HDA_GCAP_OSS_MASK) >> HDA_GCAP_OSS_SHIFT;
	c->num_bss = (gcap & HDA_GCAP_BSS_MASK) >> HDA_GCAP_BSS_SHIFT;
	if (!c->num_iss && !c->num_oss && !c->num_bss) {
		/* GCAP gave nothing: the counts the chipsets are known for */
		if (pci->vendor_id == 0x10b9) {
			c->num_oss = 6;
			c->num_iss = 5;
		} else if (c->flags & HDA_F_AMD_GCAP_FIX) {
			c->num_oss = 8;
			c->num_iss = 0;
		} else {
			c->num_oss = 4;
			c->num_iss = 4;
		}
	}
	c->addr64 = (gcap & HDA_GCAP_64OK) && !(c->flags & HDA_F_NO_64BIT);
	/* the stream descriptors must fit the window we mapped */
	while (c->num_iss + c->num_oss + c->num_bss > 0 &&
	       HDA_SD_OFF(c->num_iss + c->num_oss + c->num_bss) > c->mmio_size) {
		if (c->num_bss)
			c->num_bss--;
		else if (c->num_iss)
			c->num_iss--;
		else
			c->num_oss--;
	}
	hda_streams_init(c);

	if (c->flags & HDA_F_SKL_PLUS)
		hda_parse_xcaps(p);
	if (c->flags & HDA_F_SINGLE_CMD)
		c->single_cmd = 1;

	/* DMA structures */
	p->cmdbuf = snd_dma_alloc(4096, !c->addr64, 0, &p->cmdbuf_phys);
	p->posbuf_page = snd_dma_alloc(4096, !c->addr64, 0, &c->posbuf_phys);
	if (!p->cmdbuf) {
		snd_info(HDA_DRV, "%s: out of DMA memory\n", c->bus);
		rc = -ENOMEM;
		goto fail;
	}
	if (p->posbuf_page)
		c->posbuf = (uint32_t *)p->posbuf_page;
	else
		c->flags |= HDA_F_POSFIX_LPIB;

	hda_display_power(p);
	hda_setup_irq(p);

	rc = hda_init_chip(p);
	if (rc) {
		snd_info(HDA_DRV, "%s: controller reset failed (%d)\n", c->bus, rc);
		goto fail;
	}

	snd_info(HDA_DRV, "%s (%04x:%04x) at %s, HDA %u.%u, %d in/%d out/%d bidi, %s, %s%s\n",
		 id->name, pci->vendor_id, pci->device_id, c->bus + 4, c->vmaj, c->vmin,
		 c->num_iss, c->num_oss, c->num_bss,
		 p->irq_mode == 1 ? "MSI" : (p->irq_mode == 2 ? "INTx" : "polled"),
		 c->single_cmd ? "immediate commands" : "CORB/RIRB",
		 (c->flags & HDA_F_DSP_PRESENT) ? ", audio DSP" : "");
	snd_info(HDA_DRV, "codec mask %04x\n", c->codec_mask);

	if (c->codec_mask)
		hda_probe_codecs(p, c->codec_mask);
	hda_update_dsp_owned(p);
	hda_mark_jack_poll(p);

	if (c->ncodecs)
		hda_create_card(p);
	else
		snd_info(HDA_DRV, "%s: no codecs yet; retrying after boot\n", c->bus);
	return p->card_registered ? 1 : 0;

fail:
	hda_teardown(p);
	p->released = 1; /* stays in the registry: the IRQ may still look */
	return rc;
}

int hda_probe_all(void)
{
	const pci_device_t *devs;
	int n = 0, cards = 0;

	if (g_hda_probed)
		return 0;
	g_hda_probed = 1;

	devs = pci_get_devices(&n);
	if (!devs || n <= 0)
		return 0;
	for (int i = 0; i < n && i < PCI_MAX_DEVICES; i++) {
		const pci_device_t *pci = &devs[i];
		const struct hda_pci_id *id;

		if (!hda_pci_is_hda(pci))
			continue;
		id = hda_pci_lookup(pci);
		if (!id)
			continue;
		if (hda_probe_one(pci, id) > 0)
			cards++;
	}
	return cards;
}

/* ---- late initialisation --------------------------------------------------------------- */

static void hda_late_one(struct hda_priv *p)
{
	struct hda_controller *c = &p->c;
	uint16_t st, want;
	int had_card = c->card != NULL;
	int found;

	if (p->released || hda_ctrl_dead(c))
		return;

	/* A display codec that was powered off at boot appears once the
	 * graphics driver is up: if nothing answered, reset again with the
	 * power well held. */
	if (!c->ncodecs && (c->flags & (HDA_F_HSW_DISPLAY | HDA_F_PCH_PWR_WELL))) {
		hda_display_power(p);
		if (hda_init_chip(p) == 0)
			snd_info(HDA_DRV, "%s: re-reset, codec mask %04x\n", c->bus,
				 c->codec_mask);
	}

	st = hda_read16(c, HDA_REG_STATESTS);
	if (st == 0xffff) {
		hda_check_gone(p);
		return;
	}
	st &= HDA_STATESTS_MASK;
	if (st)
		hda_write16(c, HDA_REG_STATESTS, st);
	want = (uint16_t)((st | p->wake_mask | p->failed_mask | c->codec_mask) &
			  ~p->probed_mask);
	if (c->flags & HDA_F_PROBE_MASK_1)
		want &= 1;
	p->wake_mask = 0;
	c->codec_mask |= want;
	/* one retry each: a codec that failed again stays failed */
	p->failed_mask = 0;

	found = want ? hda_probe_codecs(p, want) : 0;
	hda_update_dsp_owned(p);
	hda_mark_jack_poll(p);

	if (found) {
		if (!had_card) {
			hda_create_card(p);
		} else {
			/* A published card is fixed: its device nodes and PCM
			 * set exist already.  The codec still works for jack
			 * events and the mixer; its PCMs come with the next
			 * boot. */
			snd_info(HDA_DRV, "%s: %d codec(s) answered after the card was "
				 "published; their PCM devices need a reboot\n",
				 c->bus, found);
		}
	}

	/* jack polling where the codecs cannot tell us */
	hda_start_jack_poll(p);
}

void hda_late_init(void)
{
	g_hda_sched_ready = 1;
	for (int i = 0; i < g_hda_count; i++)
		if (g_hda[i])
			hda_late_one(g_hda[i]);
}

/* ---- coordination with the DSP driver --------------------------------------------------- */

int hda_controller_dsp_owned(const pci_device_t *pci)
{
	struct hda_priv *p = priv_of_pci(pci);

	if (!p || p->released)
		return -ENODEV;
	return p->dsp_owned ? 1 : 0;
}

int hda_controller_release(const pci_device_t *pci)
{
	struct hda_priv *p = priv_of_pci(pci);
	struct hda_controller *c;

	if (!p || p->released)
		return -ENODEV;
	c = &p->c;
	snd_info(HDA_DRV, "%s: handing the controller over\n", c->bus);
	for (int i = 0; i < HDA_MAX_STREAMS; i++) {
		p->timer_mode[i] = TM_OFF;
		hrtimer_cancel(&p->timer[i]);
	}
	p->released = 1;
	if (c->card && p->card_registered)
		snd_card_disconnect(c->card);
	if (c->mmio && !hda_ctrl_dead(c)) {
		hda_streams_stop_all(c);
		cmd_io_stop(p);
		hda_write32(c, HDA_REG_INTCTL, 0);
		hda_write32(c, HDA_REG_DPLBASE, 0);
		hda_write16(c, HDA_REG_WAKEEN, 0);
	}
	hda_free_irq(p);
	/* the register mapping, rings and codecs stay allocated: the card
	 * may still be referenced by open files until they close */
	return 0;
}
