// LikeOS AHCI driver - host controller bring-up, kahci thread, recovery, power
//
// One struct ahci_hba per AHCI controller.  This file takes a controller
// from "a PCI function with class 01/06/01" to "every port running", keeps
// it running, and puts it to rest:
//
//   BRING-UP (ahci_init, boot, interrupts off, no scheduler -- everything
//   is polled and every delay busy-waits):
//     1. BIOS/OS handoff (AHCI 1.3.1 section 10.6.3): when CAP2.BOH says
//        the firmware may still own the controller, set BOHC.OOS, give the
//        BIOS 25 ms to clear BOS -- or, if it raises BB in that time, at
//        least 2 s to finish what it was doing.
//     2. Quiesce what the firmware left running: every implemented port's
//        command engine (ST/CR) and FIS receive engine (FRE/FR) stopped.
//     3. HBA reset (section 10.4.3): GHC.AE first (some controllers ignore
//        HR otherwise), GHC.HR, wait up to 1 s for it to self-clear, AE
//        again; then restore the write-once registers (CAP, CAP2, PI) that
//        the reset cleared on chipsets whose firmware programs them.
//     4. Capabilities: CAP/CAP2/VS/PI decoded, quirks applied (no NCQ, no
//        PMP, 32-bit DMA, phantom ports ...), logged in one line.
//     5. Global initialisation: command completion coalescing off (one
//        interrupt per completion -- the block layer wants its latency),
//        interrupts globally off until a handler exists, IS cleared.
//     6. Ports: every implemented port allocated (ahci_port_alloc) and
//        initialised (ahci_port_init); with staggered spin-up (CAP.SSS)
//        the ports are spun up one at a time, AHCI_SPINUP_STAGGER_MS
//        apart, so the drives' spin-up currents do not add up.
//     7. Probe: every port's link brought up and its device identified and
//        registered (ahci_port_probe, which goes on into the ATA layer).
//
//   RUNNING (ahci_late_init, after sti): the interrupt mode is chosen
//   (MSI-X, MSI, INTx, or the 1 ms poll timer) and one kernel thread per
//   controller, kahci<N>, is started.  It sleeps until work is kicked at it
//   from any context -- error handling, timeouts, hot-plug, link power
//   management, controller recovery -- and also wakes
//   on its own every so often as a backstop for lost interrupts.
//
//   RECOVERY (ahci_hba_recover, kahci only): when a port's error handling
//   gives up at the COMRESET rung, or the controller signals a host bus
//   error it cannot recover from, the whole controller is reset: every
//   port's eh_mutex taken in ascending order, every port frozen and
//   stopped, the commands in flight handed back for requeueing, the PCI
//   state restored, HBA reset, the global and per-port state rebuilt, and
//   each port's error handling asked to hard-reset its link and revalidate
//   its devices.  Repeated failures within a minute mark the controller
//   dead and detach its devices instead of looping.
//
//   SHUTDOWN / SUSPEND / RESUME: the controller-level halves of the
//   system power paths (the disks were flushed by the block layer first).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/timer.h>

/* ==========================================================================
 * Global state
 * ========================================================================== */

struct ahci_hba *g_ahci_hbas;
spinlock_t g_ahci_list_lock = SPINLOCK_INIT("ahci_list");
int g_ahci_nr_hbas;
int g_ahci_next_port_no = 1;

/* The kahci threads' stacks: static, so a thread can be started on a
 * system that is short of contiguous memory. */
static uint8_t g_kahci_stacks[AHCI_MAX_HBAS][AHCI_KAHCI_STACK_SIZE]
	__attribute__((aligned(16)));
/* kahci sleep/wake: a leaf lock per controller, so a kick from any
 * context (with any driver lock held) can never deadlock against it. */
static spinlock_t g_kahci_lock[AHCI_MAX_HBAS];
/* CAP2 as the firmware left it (restored after HR like CAP and PI). */
static uint32_t g_saved_cap2[AHCI_MAX_HBAS];
/* Recovery history: refuse to loop resetting a controller that cannot be
 * recovered. */
#define AHCI_RECOVER_WINDOW_NS (60ULL * 1000000000ULL)
#define AHCI_RECOVER_MAX_IN_WINDOW 3
static uint64_t g_recover_ns[AHCI_MAX_HBAS][AHCI_RECOVER_MAX_IN_WINDOW];
/* Set once a controller was shut down (never touched again). */
static volatile int g_hba_shut[AHCI_MAX_HBAS];

/* kahci timing */
/* kahci sleeps until it is woken by work -- error handling scheduled
 * (errors, timeouts, hot-plug, LPM, resets), a drained port whose EH
 * waits, a controller recovery, a resume, a stop.  Only a controller
 * without interrupts (polled mode) also wakes it periodically. */
#define AHCI_KAHCI_NAP_FOREVER 0
#define AHCI_KAHCI_NAP_POLL_MS 1000 /* polled-mode controller */
#define AHCI_KAHCI_STOP_WAIT_MS 2000

/* might_sleep() only means something once the scheduler runs: at boot the
 * same paths legitimately busy-wait with interrupts off. */
#define ahci_might_sleep()                     \
	do {                                   \
		if (sched_current() != NULL)   \
			might_sleep();         \
	} while (0)

static int ahci_hba_idx_ok(const struct ahci_hba *hba)
{
	return hba && hba->index >= 0 && hba->index < AHCI_MAX_HBAS;
}

static const char *ahci_hba_state_name(enum ahci_hba_state s)
{
	switch (s) {
	case AHCI_HBA_PROBING:
		return "probing";
	case AHCI_HBA_POLLED:
		return "polled";
	case AHCI_HBA_RUNNING:
		return "running";
	case AHCI_HBA_RESETTING:
		return "resetting";
	case AHCI_HBA_SUSPENDED:
		return "suspended";
	case AHCI_HBA_DEAD:
		return "dead";
	default:
		return "?";
	}
}

static void ahci_hba_set_state(struct ahci_hba *hba, enum ahci_hba_state s)
{
	uint64_t fl;
	enum ahci_hba_state old;

	spin_lock_irqsave(&hba->lock, &fl);
	old = hba->state;
	hba->state = s;
	spin_unlock_irqrestore(&hba->lock, fl);
	if (old != s)
		kprintf("[ahci] %s: state %s -> %s\n", hba->name,
			ahci_hba_state_name(old), ahci_hba_state_name(s));
}

/* ==========================================================================
 * Allocation and lookup
 * ========================================================================== */

static void ahci_poll_timer_fn(hrtimer_t *t);

struct ahci_hba *ahci_hba_alloc(const pci_device_t *pci,
				const struct ahci_pci_id *id)
{
	struct ahci_hba *hba;
	uint64_t fl;
	int idx;

	BUG_ON(pci == NULL);
	if (g_ahci_nr_hbas >= AHCI_MAX_HBAS) {
		kprintf("[ahci] more than %d controllers: %02x:%02x.%u ignored\n",
			AHCI_MAX_HBAS, pci->bus, pci->device, pci->function);
		return NULL;
	}
	hba = kcalloc(1, sizeof(*hba));
	if (!hba) {
		kprintf("[ahci] out of memory for the controller at %02x:%02x.%u\n",
			pci->bus, pci->device, pci->function);
		return NULL;
	}
	idx = g_ahci_nr_hbas;
	hba->index = idx;
	ksnprintf(hba->name, sizeof(hba->name), "ahci%d", idx);
	hba->pci = pci;
	hba->id = id;
	hba->quirks = id ? id->quirks : 0;
	hba->state = AHCI_HBA_PROBING;
	spinlock_init(&hba->lock, "ahci_hba");
	wq_head_init(&hba->kahci_wq, "kahci");
	hrtimer_init(&hba->poll_timer, ahci_poll_timer_fn, hba);
	hba->irq_mode = AHCI_IRQ_POLL;
	hba->irq_vector = -1;
	hba->irq_gsi = -1;
	hba->kahci_stack = g_kahci_stacks[idx];
	hba->first_port_no = 0;
	spinlock_init(&g_kahci_lock[idx], "kahci_wake");
	g_saved_cap2[idx] = 0;
	for (int i = 0; i < AHCI_RECOVER_MAX_IN_WINDOW; i++)
		g_recover_ns[idx][i] = 0;
	g_hba_shut[idx] = 0;

	/* Append, so the list is in probe order (ahci0 first). */
	spin_lock_irqsave(&g_ahci_list_lock, &fl);
	if (!g_ahci_hbas) {
		g_ahci_hbas = hba;
	} else {
		struct ahci_hba *h = g_ahci_hbas;

		while (h->next)
			h = h->next;
		h->next = hba;
	}
	g_ahci_nr_hbas++;
	spin_unlock_irqrestore(&g_ahci_list_lock, fl);
	return hba;
}

struct ahci_hba *ahci_hba_get(int index)
{
	struct ahci_hba *h;
	uint64_t fl;

	spin_lock_irqsave(&g_ahci_list_lock, &fl);
	for (h = g_ahci_hbas; h; h = h->next)
		if (h->index == index)
			break;
	spin_unlock_irqrestore(&g_ahci_list_lock, fl);
	return h;
}

int ahci_hba_count(void)
{
	return g_ahci_nr_hbas;
}

/* ==========================================================================
 * BIOS/OS handoff, AHCI enable, reset
 * ========================================================================== */

int ahci_hba_bios_handoff(struct ahci_hba *hba)
{
	uint32_t cap2, bohc, v = 0;
	uint64_t fl;
	int rc;

	BUG_ON(hba == NULL || hba->mmio == NULL);

	/* CAP2 exists from AHCI 1.2 on; BOH is its bit 0. */
	if (ahci_hba_readl(hba, AHCI_VS) < AHCI_VS_1_2)
		return 0;
	cap2 = ahci_hba_readl(hba, AHCI_CAP2);
	if (cap2 == 0xFFFFFFFFu || !(cap2 & AHCI_CAP2_BOH))
		return 0;

	bohc = ahci_hba_readl(hba, AHCI_BOHC);
	if (bohc == 0xFFFFFFFFu)
		return -ENODEV;
	if ((bohc & AHCI_BOHC_OOS) && !(bohc & AHCI_BOHC_BOS))
		return 0; /* already ours (a re-init after a reset) */

	kprintf("[ahci] %s: requesting ownership from the firmware (BOHC 0x%08x)\n",
		hba->name, bohc);

	/* Set OOS.  OOC is RW1C: writing it back as 0 leaves it alone. */
	spin_lock_irqsave(&hba->lock, &fl);
	ahci_hba_writel(hba, AHCI_BOHC,
			(bohc | AHCI_BOHC_OOS) & ~AHCI_BOHC_OOC);
	(void)ahci_hba_readl(hba, AHCI_BOHC);
	spin_unlock_irqrestore(&hba->lock, fl);

	/* The BIOS clears BOS within 25 ms -- or raises BB to say it needs
	 * more time to finish outstanding commands. */
	rc = ahci_wait_reg(hba->mmio, AHCI_BOHC, AHCI_BOHC_BOS, 0,
			   AHCI_BOHC_BOS_WAIT_MS, 1000, &v);
	if (rc == 0)
		goto owned;

	if (v & AHCI_BOHC_BB)
		kprintf("[ahci] %s: firmware busy (BB), waiting up to %u ms\n",
			hba->name, AHCI_BOHC_BB_WAIT_MS);
	else
		kprintf("[ahci] %s: firmware did not release the controller in %u ms\n",
			hba->name, AHCI_BOHC_BOS_WAIT_MS);
	/* At least 2 s once BB was seen; without BB the same bound keeps a
	 * broken firmware from stalling the boot forever. */
	rc = ahci_wait_reg(hba->mmio, AHCI_BOHC, AHCI_BOHC_BOS | AHCI_BOHC_BB,
			   0, AHCI_BOHC_BB_WAIT_MS, 1000, &v);
	if (rc != 0) {
		WARN(1, "[ahci] %s: BIOS/OS handoff timed out (BOHC 0x%08x), taking over anyway",
		     hba->name, v);
		/* Clear the SMI-on-change enable so the firmware is not
		 * invoked again behind our back. */
		spin_lock_irqsave(&hba->lock, &fl);
		v = ahci_hba_readl(hba, AHCI_BOHC);
		ahci_hba_writel(hba, AHCI_BOHC,
				(v | AHCI_BOHC_OOS) &
					~(AHCI_BOHC_SOOE | AHCI_BOHC_OOC));
		spin_unlock_irqrestore(&hba->lock, fl);
		return -ETIMEDOUT;
	}

owned:
	spin_lock_irqsave(&hba->lock, &fl);
	v = ahci_hba_readl(hba, AHCI_BOHC);
	if (v & AHCI_BOHC_OOC)
		ahci_hba_writel(hba, AHCI_BOHC, v); /* RW1C the change bit */
	spin_unlock_irqrestore(&hba->lock, fl);
	kprintf("[ahci] %s: controller owned by the OS (BOHC 0x%08x)\n",
		hba->name, ahci_hba_readl(hba, AHCI_BOHC));
	return 0;
}

int ahci_hba_enable_ahci(struct ahci_hba *hba)
{
	uint32_t ghc;
	uint64_t fl;

	BUG_ON(hba == NULL || hba->mmio == NULL);

	/* Some controllers need AE written several times before it sticks;
	 * on AHCI-only controllers (CAP.SAM) it reads 1 always. */
	for (int i = 0; i < 5; i++) {
		ghc = ahci_hba_readl(hba, AHCI_GHC);
		if (ghc == 0xFFFFFFFFu)
			return -ENODEV;
		if (ghc & AHCI_GHC_AE)
			return 0;
		spin_lock_irqsave(&hba->lock, &fl);
		ahci_hba_writel(hba, AHCI_GHC,
				(ghc | AHCI_GHC_AE) & ~AHCI_GHC_HR);
		(void)ahci_hba_readl(hba, AHCI_GHC);
		spin_unlock_irqrestore(&hba->lock, fl);
		ahci_msleep(10);
	}
	ghc = ahci_hba_readl(hba, AHCI_GHC);
	if (ghc & AHCI_GHC_AE)
		return 0;
	WARN(1, "[ahci] %s: GHC.AE does not stick (GHC 0x%08x)", hba->name,
	     ghc);
	return -EIO;
}

/* The firmware's values of the write-once registers, read once, before
 * the first reset can clear them. */
static void ahci_hba_save_initial_config(struct ahci_hba *hba)
{
	if (hba->saved_cap)
		return;
	hba->saved_cap = ahci_hba_readl(hba, AHCI_CAP);
	hba->saved_pi = ahci_hba_readl(hba, AHCI_PI);
	if (ahci_hba_idx_ok(hba))
		g_saved_cap2[hba->index] =
			ahci_hba_readl(hba, AHCI_VS) >= AHCI_VS_1_2 ?
				ahci_hba_readl(hba, AHCI_CAP2) :
				0;
}

/* Put them back after a reset (writes to read-only bits are ignored). */
static void ahci_hba_restore_initial_config(struct ahci_hba *hba)
{
	uint64_t fl;

	if (!hba->saved_cap)
		return;
	spin_lock_irqsave(&hba->lock, &fl);
	ahci_hba_writel(hba, AHCI_CAP, hba->saved_cap);
	if (ahci_hba_idx_ok(hba) && g_saved_cap2[hba->index])
		ahci_hba_writel(hba, AHCI_CAP2, g_saved_cap2[hba->index]);
	ahci_hba_writel(hba, AHCI_PI, hba->saved_pi);
	(void)ahci_hba_readl(hba, AHCI_PI);
	spin_unlock_irqrestore(&hba->lock, fl);
}

int ahci_hba_reset(struct ahci_hba *hba)
{
	uint32_t ghc, v = 0;
	uint64_t fl, t0;
	int rc;

	BUG_ON(hba == NULL || hba->mmio == NULL);
	ahci_might_sleep();

	ahci_hba_save_initial_config(hba);

	/* AHCI mode first: on several controllers HR has no effect (or
	 * resets the legacy side) unless AE is set. */
	rc = ahci_hba_enable_ahci(hba);
	if (rc)
		return rc;

	t0 = hrtimer_now_ns();
	spin_lock_irqsave(&hba->lock, &fl);
	ghc = ahci_hba_readl(hba, AHCI_GHC);
	if (!(ghc & AHCI_GHC_HR))
		ahci_hba_writel(hba, AHCI_GHC, ghc | AHCI_GHC_HR);
	(void)ahci_hba_readl(hba, AHCI_GHC); /* post the write */
	spin_unlock_irqrestore(&hba->lock, fl);

	/* The HBA clears HR when the reset is complete (within 1 s). */
	rc = ahci_wait_reg(hba->mmio, AHCI_GHC, AHCI_GHC_HR, 0,
			   AHCI_HR_TIMEOUT_MS, 1000, &v);
	if (rc) {
		kprintf("[ahci] %s: HBA reset timed out (GHC 0x%08x)\n",
			hba->name, v);
		hba->stats.hba_resets++;
		return -ETIMEDOUT;
	}

	/* HR cleared AE (unless the controller is AHCI-only). */
	rc = ahci_hba_enable_ahci(hba);
	if (rc)
		return rc;
	ahci_hba_restore_initial_config(hba);
	ahci_pci_reset_quirks(hba);

	hba->generation++;
	hba->stats.hba_resets++;
	kprintf("[ahci] %s: HBA reset done (%llu us, generation %u)\n",
		hba->name,
		(unsigned long long)((hrtimer_now_ns() - t0) / 1000ULL),
		hba->generation);
	return 0;
}

/* ==========================================================================
 * Capabilities
 * ========================================================================== */

static void ahci_vs_str(uint32_t vs, char *buf, int cap)
{
	uint32_t maj = AHCI_VS_MAJOR(vs), min = AHCI_VS_MINOR(vs);

	if (vs == AHCI_VS_0_95)
		ksnprintf(buf, (size_t)cap, "0.95");
	else if ((min & 0xFF) == 0)
		ksnprintf(buf, (size_t)cap, "%x.%x", maj, (min >> 8) & 0xFF);
	else
		ksnprintf(buf, (size_t)cap, "%x.%x.%x", maj, (min >> 8) & 0xFF,
			  min & 0xFF);
}

static const char *ahci_iss_str(uint32_t iss)
{
	switch (iss) {
	case AHCI_ISS_GEN1:
		return "1.5 Gbps";
	case AHCI_ISS_GEN2:
		return "3 Gbps";
	case AHCI_ISS_GEN3:
		return "6 Gbps";
	default:
		return "? Gbps";
	}
}

/* Set bits of a mask (no libgcc in the kernel: no __builtin_popcount). */
static int ahci_popcount32(uint32_t v)
{
	int n = 0;

	for (; v; v &= v - 1)
		n++;
	return n;
}

/* fls for a 32-bit mask: index of the highest set bit + 1, 0 for none */
static int ahci_fls32(uint32_t v)
{
	return v ? 32 - __builtin_clz(v) : 0;
}

void ahci_hba_read_caps(struct ahci_hba *hba)
{
	struct ahci_caps *c;
	uint32_t cap, cap2, q, pi, np_mask;
	char vs[16], flags[256];
	int pos = 0, max_by_bar;

	BUG_ON(hba == NULL || hba->mmio == NULL);
	c = &hba->caps;
	q = hba->quirks;

	hba->vs = ahci_hba_readl(hba, AHCI_VS);
	hba->cap = cap = ahci_hba_readl(hba, AHCI_CAP);
	hba->cap2 = cap2 =
		hba->vs >= AHCI_VS_1_2 ? ahci_hba_readl(hba, AHCI_CAP2) : 0;
	hba->pi = pi = ahci_hba_readl(hba, AHCI_PI);

	kmemset(c, 0, sizeof(*c));
	c->n_ports = (cap & AHCI_CAP_NP_MASK) + 1;
	c->n_slots = ((cap & AHCI_CAP_NCS_MASK) >> AHCI_CAP_NCS_SHIFT) + 1;
	c->iss = (cap & AHCI_CAP_ISS_MASK) >> AHCI_CAP_ISS_SHIFT;
	c->s64a = !!(cap & AHCI_CAP_S64A);
	c->sncq = !!(cap & AHCI_CAP_SNCQ);
	c->ssntf = !!(cap & AHCI_CAP_SSNTF);
	c->smps = !!(cap & AHCI_CAP_SMPS);
	c->sss = !!(cap & AHCI_CAP_SSS);
	c->salp = !!(cap & AHCI_CAP_SALP);
	c->sal = !!(cap & AHCI_CAP_SAL);
	c->sclo = !!(cap & AHCI_CAP_SCLO);
	c->sam = !!(cap & AHCI_CAP_SAM);
	c->spm = !!(cap & AHCI_CAP_SPM);
	c->fbss = !!(cap & AHCI_CAP_FBSS);
	c->pmd = !!(cap & AHCI_CAP_PMD);
	c->ssc = !!(cap & AHCI_CAP_SSC);
	c->psc = !!(cap & AHCI_CAP_PSC);
	c->cccs = !!(cap & AHCI_CAP_CCCS);
	c->ems = !!(cap & AHCI_CAP_EMS);
	c->sxs = !!(cap & AHCI_CAP_SXS);
	c->boh = !!(cap2 & AHCI_CAP2_BOH);
	c->nvmp = !!(cap2 & AHCI_CAP2_NVMP);
	c->apst = !!(cap2 & AHCI_CAP2_APST);
	c->sds = !!(cap2 & AHCI_CAP2_SDS);
	c->sadm = !!(cap2 & AHCI_CAP2_SADM);
	c->deso = !!(cap2 & AHCI_CAP2_DESO);

	/* Quirks that take capabilities away (or give one back) */
	if (q & AHCI_Q_NO_NCQ)
		c->sncq = 0;
	if (q & AHCI_Q_NO_PMP) {
		c->spm = 0;
		c->fbss = 0;
	}
	if (q & AHCI_Q_NO_FBS)
		c->fbss = 0;
	if ((q & AHCI_Q_YES_FBS) && c->spm)
		c->fbss = 1;
	if (q & AHCI_Q_NO_SNTF)
		c->ssntf = 0;
	if (q & AHCI_Q_NO_SXS)
		c->sxs = 0;
	if (q & AHCI_Q_NO_SSS)
		c->sss = 0;
	if (q & AHCI_Q_NO_LPM) {
		c->salp = 0;
		c->apst = 0;
	}
	if (q & AHCI_Q_NO_DEVSLP) {
		c->sds = 0;
		c->sadm = 0;
	}
	if (q & (AHCI_Q_NO_CLO))
		c->sclo = 0;
	hba->dma64 = c->s64a && !(q & (AHCI_Q_32BIT_ONLY | AHCI_Q_AMD_32BIT));

	/* The implemented-port map.  Fix what firmware and silicon get
	 * wrong, in the order the problems stack up. */
	np_mask = c->n_ports >= 32 ? 0xFFFFFFFFu : ((1u << c->n_ports) - 1);
	if (pi == 0 || pi == 0xFFFFFFFFu || (q & AHCI_Q_FORCE_PI)) {
		kprintf("[ahci] %s: PI 0x%08x unusable, deriving from CAP.NP: 0x%08x\n",
			hba->name, pi, np_mask);
		pi = np_mask;
	}
	if ((q & AHCI_Q_ASMEDIA_PORTS) && (pi & ~np_mask)) {
		kprintf("[ahci] %s: ignoring phantom ports 0x%08x\n", hba->name,
			pi & ~np_mask);
		pi &= np_mask;
	}
	if (q & AHCI_Q_MV_PATA) {
		uint32_t mv = hba->pci && hba->pci->device_id == 0x6121 ? 0x3u :
									    0xFu;

		if (pi & ~mv)
			kprintf("[ahci] %s: hiding the PATA port: 0x%x -> 0x%x\n",
				hba->name, pi, pi & mv);
		pi &= mv;
	}
	if (ahci_fls32(pi) > (int)c->n_ports) {
		kprintf("[ahci] %s: PI 0x%08x names ports beyond CAP.NP (%u), trusting PI\n",
			hba->name, pi, c->n_ports);
		c->n_ports = (uint32_t)ahci_fls32(pi);
	}
	/* Never a port whose registers lie outside the mapped window. */
	max_by_bar = hba->mmio_size >= AHCI_PORT_BASE ?
			     (int)((hba->mmio_size - AHCI_PORT_BASE) /
				   AHCI_PORT_SIZE) :
			     0;
	if (max_by_bar < 32 && (pi >> max_by_bar)) {
		WARN(1, "[ahci] %s: ports 0x%08x lie beyond ABAR (0x%llx bytes)",
		     hba->name, pi & ~((1u << max_by_bar) - 1),
		     (unsigned long long)hba->mmio_size);
		pi &= (1u << max_by_bar) - 1;
	}
	hba->port_map = pi;

	if (c->ems) {
		hba->em_loc = ahci_hba_readl(hba, AHCI_EM_LOC);
		hba->em_ctl = ahci_hba_readl(hba, AHCI_EM_CTL);
	}

	/* The established controller line, then the capability flags */
	ahci_vs_str(hba->vs, vs, sizeof(vs));
	kprintf("[ahci] %s: AHCI %s %u slots %u ports %s 0x%x impl %s mode\n",
		hba->name, vs, c->n_slots, c->n_ports, ahci_iss_str(c->iss),
		hba->port_map, c->sam ? "SATA" : "SATA/IDE");

	flags[0] = 0;
#define AHCI_FLAG(cond, s)                                                    \
	do {                                                                  \
		if (cond)                                                     \
			pos += ksnprintf(flags + pos, sizeof(flags) - (size_t)pos, \
					 "%s ", s);                           \
		if (pos >= (int)sizeof(flags))                                \
			pos = (int)sizeof(flags) - 1;                         \
	} while (0)
	AHCI_FLAG(c->s64a, "64bit");
	AHCI_FLAG(c->sncq, "ncq");
	AHCI_FLAG(c->ssntf, "sntf");
	AHCI_FLAG(c->smps, "ilck");
	AHCI_FLAG(c->sss, "stag");
	AHCI_FLAG(c->salp, "pm");
	AHCI_FLAG(c->sal, "led");
	AHCI_FLAG(c->sclo, "clo");
	AHCI_FLAG(c->sam, "only");
	AHCI_FLAG(c->spm, "pmp");
	AHCI_FLAG(c->fbss, "fbs");
	AHCI_FLAG(c->pmd, "pio");
	AHCI_FLAG(c->ssc, "slum");
	AHCI_FLAG(c->psc, "part");
	AHCI_FLAG(c->cccs, "ccc");
	AHCI_FLAG(c->ems, "ems");
	AHCI_FLAG(c->sxs, "sxs");
	AHCI_FLAG(c->deso, "deso");
	AHCI_FLAG(c->sadm, "sadm");
	AHCI_FLAG(c->sds, "sds");
	AHCI_FLAG(c->apst, "apst");
	AHCI_FLAG(c->nvmp, "nvmp");
	AHCI_FLAG(c->boh, "boh");
#undef AHCI_FLAG
	kprintf("[ahci] %s: flags: %s(CAP 0x%08x CAP2 0x%08x%s)\n", hba->name,
		flags, cap, cap2, hba->dma64 ? "" : ", 32-bit DMA");
	if (c->ems)
		kprintf("[ahci] %s: enclosure management: buffer at dword 0x%x, %u dwords, ctl 0x%08x\n",
			hba->name, hba->em_loc >> AHCI_EM_LOC_OFST_SHIFT,
			hba->em_loc & AHCI_EM_LOC_SZ_MASK, hba->em_ctl);
}

/* ==========================================================================
 * Global control helpers
 * ========================================================================== */

void ahci_hba_irq_enable(struct ahci_hba *hba)
{
	uint64_t fl;
	uint32_t ghc;

	BUG_ON(hba == NULL);
	spin_lock_irqsave(&hba->lock, &fl);
	ghc = ahci_hba_readl(hba, AHCI_GHC);
	if (ghc != 0xFFFFFFFFu && !(ghc & AHCI_GHC_IE)) {
		ahci_hba_writel(hba, AHCI_GHC,
				(ghc | AHCI_GHC_IE | AHCI_GHC_AE) & ~AHCI_GHC_HR);
		(void)ahci_hba_readl(hba, AHCI_GHC);
	}
	spin_unlock_irqrestore(&hba->lock, fl);
}

void ahci_hba_irq_disable(struct ahci_hba *hba)
{
	uint64_t fl;
	uint32_t ghc;

	BUG_ON(hba == NULL);
	spin_lock_irqsave(&hba->lock, &fl);
	ghc = ahci_hba_readl(hba, AHCI_GHC);
	if (ghc != 0xFFFFFFFFu && (ghc & AHCI_GHC_IE)) {
		ahci_hba_writel(hba, AHCI_GHC,
				ghc & ~(AHCI_GHC_IE | AHCI_GHC_HR));
		(void)ahci_hba_readl(hba, AHCI_GHC);
	}
	spin_unlock_irqrestore(&hba->lock, fl);
}

/* Command completion coalescing off: the driver wants an interrupt per
 * completion; coalescing trades latency the block layer cares about for
 * interrupts it does not. */
static void ahci_hba_ccc_off(struct ahci_hba *hba)
{
	uint64_t fl;
	uint32_t ccc;

	if (!hba->caps.cccs)
		return;
	spin_lock_irqsave(&hba->lock, &fl);
	ccc = ahci_hba_readl(hba, AHCI_CCC_CTL);
	hba->saved_ccc = ccc;
	if (ccc & AHCI_CCC_EN) {
		ahci_hba_writel(hba, AHCI_CCC_CTL, ccc & ~AHCI_CCC_EN);
		(void)ahci_hba_readl(hba, AHCI_CCC_CTL);
	}
	spin_unlock_irqrestore(&hba->lock, fl);
	if (ccc & AHCI_CCC_EN)
		kprintf("[ahci] %s: command completion coalescing turned off (CCC_CTL 0x%08x)\n",
			hba->name, ccc);
}

static void ahci_hba_clear_is(struct ahci_hba *hba)
{
	uint64_t fl;
	uint32_t is;

	spin_lock_irqsave(&hba->lock, &fl);
	is = ahci_hba_readl(hba, AHCI_IS);
	if (is && is != 0xFFFFFFFFu)
		ahci_hba_writel(hba, AHCI_IS, is);
	spin_unlock_irqrestore(&hba->lock, fl);
}

/* Stop whatever the firmware left running on the implemented ports, with
 * raw register accesses (the port objects do not exist yet): ST off, wait
 * CR; FRE off, wait FR; PxIE off; PxIS/PxSERR cleared. */
static void ahci_hba_stop_ports_raw(struct ahci_hba *hba, uint32_t pi)
{
	for (uint32_t b = pi; b; b &= b - 1) {
		int i = __builtin_ctz(b);
		volatile uint8_t *pm;
		uint32_t cmd, v = 0;

		if ((uint64_t)AHCI_PORT_BASE + (uint64_t)(i + 1) * AHCI_PORT_SIZE >
		    hba->mmio_size)
			continue;
		pm = hba->mmio + AHCI_PORT_BASE + (uint32_t)i * AHCI_PORT_SIZE;
		ahci_writel(pm, AHCI_PxIE, 0);
		cmd = ahci_readl(pm, AHCI_PxCMD);
		if (cmd == 0xFFFFFFFFu)
			continue;
		if (cmd & (AHCI_PxCMD_ST | AHCI_PxCMD_CR)) {
			ahci_writel(pm, AHCI_PxCMD, cmd & ~AHCI_PxCMD_ST);
			if (ahci_wait_reg(pm, AHCI_PxCMD, AHCI_PxCMD_CR, 0,
					  AHCI_ENGINE_STOP_MS, 100, &v))
				kprintf("[ahci] %s: port %d engine did not stop (PxCMD 0x%08x)\n",
					hba->name, i, v);
		}
		cmd = ahci_readl(pm, AHCI_PxCMD);
		if (cmd & (AHCI_PxCMD_FRE | AHCI_PxCMD_FR)) {
			ahci_writel(pm, AHCI_PxCMD, cmd & ~AHCI_PxCMD_FRE);
			if (ahci_wait_reg(pm, AHCI_PxCMD, AHCI_PxCMD_FR, 0,
					  AHCI_FIS_RX_STOP_MS, 100, &v))
				kprintf("[ahci] %s: port %d FIS receive did not stop (PxCMD 0x%08x)\n",
					hba->name, i, v);
		}
		ahci_writel(pm, AHCI_PxSERR, AHCI_SERR_ALL);
		v = ahci_readl(pm, AHCI_PxIS);
		if (v && v != 0xFFFFFFFFu)
			ahci_writel(pm, AHCI_PxIS, v);
	}
}

/* Fill in what a port object needs from the controller when the port
 * allocator left it open (system-wide number and name, register window,
 * slot count). */
static void ahci_hba_port_fixup(struct ahci_hba *hba, struct ahci_port *port,
				int index, int ordinal)
{
	if (!port->hba)
		port->hba = hba;
	if (WARN_ON(port->hba != hba))
		port->hba = hba;
	port->index = index;
	if (!port->mmio)
		port->mmio = hba->mmio + AHCI_PORT_BASE +
			     (uint32_t)index * AHCI_PORT_SIZE;
	/* The HBA reserved one number per port it can have (CAP.NP), so a
	 * port's number follows its position on the controller. */
	if (!port->port_no)
		port->port_no = hba->first_port_no + index;
	(void)ordinal;
	if (!port->name[0])
		ksnprintf(port->name, sizeof(port->name), "ata%d",
			  port->port_no);
	if (!port->nr_slots)
		port->nr_slots = hba->caps.n_slots;
	if (!port->host_link.port)
		port->host_link.port = port;
	if (!port->host_link.name[0]) {
		port->host_link.is_host = 1;
		ksnprintf(port->host_link.name, sizeof(port->host_link.name),
			  "link%d", port->port_no);
	}
	port->flags |= AHCI_PORT_F_IMPLEMENTED;
}

/* Initialise every allocated port (registers programmed, spin-up),
 * staggered when the controller wants it.  `locked': the caller holds the
 * ports' eh_mutexes already (recovery, resume). */
static int ahci_hba_init_ports(struct ahci_hba *hba, int locked)
{
	int stagger = hba->caps.sss && !(hba->quirks & AHCI_Q_NO_SSS);
	int n = 0, fails = 0;

	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];
		int rc;

		if (!port)
			continue;
		if (stagger && n > 0)
			ahci_msleep(AHCI_SPINUP_STAGGER_MS);
		if (!locked)
			ata_mutex_lock(&port->eh_mutex);
		rc = ahci_port_init(port);
		if (!locked)
			ata_mutex_unlock(&port->eh_mutex);
		if (rc) {
			kprintf("[ahci] %s: port %d (%s) init failed (%d)\n",
				hba->name, i, port->name, rc);
			fails++;
		}
		n++;
	}
	return fails ? -EIO : 0;
}

/* ==========================================================================
 * Controller bring-up
 * ========================================================================== */

int ahci_hba_init(struct ahci_hba *hba)
{
	int rc, ordinal = 0;
	uint32_t map;

	BUG_ON(hba == NULL || hba->mmio == NULL);

	/* 0. AHCI mode first (AHCI 1.3.1 section 3.1.2, GHC.AE): with AE
	 * clear a controller that also implements a legacy interface need
	 * not answer the AHCI register set, so CAP/PI are read and the
	 * ports stopped only after AE is set.  A controller that refuses is
	 * tried once more by the reset below. */
	rc = ahci_hba_enable_ahci(hba);
	if (rc == -ENODEV)
		return rc;

	ahci_hba_save_initial_config(hba);

	/* 1. The firmware lets go. */
	rc = ahci_hba_bios_handoff(hba);
	if (rc == -ENODEV)
		return rc;

	/* 2. Whatever it left running stops. */
	ahci_hba_irq_disable(hba);
	ahci_hba_stop_ports_raw(hba, hba->saved_pi ? hba->saved_pi :
						     ahci_hba_readl(hba, AHCI_PI));

	/* 3. Reset. */
	rc = ahci_hba_reset(hba);
	if (rc) {
		ahci_hba_set_state(hba, AHCI_HBA_DEAD);
		return rc;
	}

	/* 4. Capabilities. */
	ahci_hba_read_caps(hba);
	if (hba->caps.n_slots < 2) {
		/* The internal slot is reserved for error handling; a single
		 * slot leaves none for the I/O path.  Run anyway (EH then
		 * shares the slot), but say so. */
		kprintf("[ahci] %s: only %u command slot(s)\n", hba->name,
			hba->caps.n_slots);
	}

	/* 5. Global state: CCC off, interrupts off until a handler exists
	 * (a level-triggered INTx asserted with nobody acknowledging it
	 * would storm a shared line), stale status cleared. */
	ahci_hba_ccc_off(hba);
	ahci_hba_irq_disable(hba);
	ahci_hba_clear_is(hba);
	hba->saved_ghc = ahci_hba_readl(hba, AHCI_GHC);

	/* 6. Ports. */
	hba->first_port_no = g_ahci_next_port_no;
	map = hba->port_map;
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port;

		if (!(map & (1u << i)))
			continue;
		rc = ahci_port_alloc(hba, i);
		port = hba->ports[i];
		if (rc || !port) {
			kprintf("[ahci] %s: port %d allocation failed (%d)\n",
				hba->name, i, rc ? rc : -ENOMEM);
			hba->port_map &= ~(1u << i);
			hba->ports[i] = NULL;
			continue;
		}
		ahci_hba_port_fixup(hba, port, i, ordinal);
		port->polled = 1;
		ordinal++;
	}
	/* Reserve the whole range: ata<first> .. ata<first + NP - 1>. */
	{
		int span = (int)hba->caps.n_ports;
		int hi = ahci_fls32(hba->saved_pi | hba->port_map);

		if (hi > span)
			span = hi;
		if (g_ahci_next_port_no < hba->first_port_no + span)
			g_ahci_next_port_no = hba->first_port_no + span;
	}
	if (ordinal == 0) {
		kprintf("[ahci] %s: no usable port\n", hba->name);
		return -ENODEV;
	}

	ahci_hba_init_ports(hba, 0);
	ahci_hba_clear_is(hba);

	/* Diagnostics: the controller, its ports and their host links. */
	ahci_sysfs_add_hba(hba);
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		if (!port)
			continue;
		ahci_sysfs_add_port(port);
		ahci_sysfs_add_link(&port->host_link);
	}

	/* GHC.IE only once an interrupt handler is registered (late init);
	 * until then everything is polled. */
	if (hba->irq_mode != AHCI_IRQ_POLL)
		ahci_hba_irq_enable(hba);
	ahci_hba_set_state(hba, AHCI_HBA_POLLED);
	return 0;
}

/* Devices hanging off a port (host link and port multiplier links). */
static int ahci_port_count_devices(const struct ahci_port *port)
{
	int n = port->host_link.dev ? 1 : 0;

	if (port->pmp_attached) {
		for (uint32_t l = 0; l < port->nr_pmp_links &&
				     l < AHCI_PMP_MAX_LINKS;
		     l++)
			if (port->pmp_links[l].dev)
				n++;
	}
	return n;
}

/* Diagnostics directories for every device on a port (idempotent: a
 * device that has one keeps it; a re-attached name reuses its files). */
static void ahci_port_sysfs_devices(struct ahci_port *port)
{
	if (port->host_link.dev)
		ahci_sysfs_add_device(port->host_link.dev);
	if (port->pmp_attached) {
		for (uint32_t l = 0; l < port->nr_pmp_links &&
				     l < AHCI_PMP_MAX_LINKS;
		     l++) {
			ahci_sysfs_add_link(&port->pmp_links[l]);
			if (port->pmp_links[l].dev)
				ahci_sysfs_add_device(port->pmp_links[l].dev);
		}
	}
}

static void ahci_hba_lock_all_ports(struct ahci_hba *hba);
static void ahci_hba_unlock_all_ports(struct ahci_hba *hba);

int ahci_hba_start_ports(struct ahci_hba *hba)
{
	struct ahci_port *together[AHCI_MAX_PORTS];
	int cr_rc[AHCI_MAX_PORTS], cr_idx[AHCI_MAX_PORTS];
	int devices = 0, n_together = 0;

	BUG_ON(hba == NULL);
	if (hba->state == AHCI_HBA_DEAD)
		return 0;

	/* The polled boot probe resets the links of all ports at once (the
	 * waiting for them overlaps; nothing else does); each port's probe
	 * then goes on below.  A port with a multiplier attached recovers
	 * it on its own. */
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		cr_idx[i] = -1;
		if (!port || hba->kahci_running || port->pmp_attached)
			continue;
		cr_idx[i] = n_together;
		together[n_together++] = port;
	}
	if (n_together) {
		ahci_hba_lock_all_ports(hba);
		ahci_ports_comreset_together(together, n_together, cr_rc);
		ahci_hba_unlock_all_ports(hba);
	}

	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];
		int rc, n;

		if (!port)
			continue;
		ata_mutex_lock(&port->eh_mutex);
		if (cr_idx[i] >= 0)
			rc = ahci_port_probe_after_comreset(port,
							    cr_rc[cr_idx[i]]);
		else
			rc = ahci_port_probe(port);
		ata_mutex_unlock(&port->eh_mutex);
		if (rc)
			kprintf("[ahci] %s: probe failed (%d)\n", port->name, rc);

		/* Fan-out links and devices get their diagnostics
		 * directories now. */
		ahci_port_sysfs_devices(port);

		/* Errors latched during the probe: before kahci exists, the
		 * polled error handler runs them now. */
		if (port->eh_pending && !hba->kahci_running)
			ahci_eh_poll_run(port);

		n = ahci_port_count_devices(port);
		devices += n;
	}
	kprintf("[ahci] %s: %d device%s on %d port%s\n", hba->name, devices,
		devices == 1 ? "" : "s", ahci_popcount32(hba->port_map),
		ahci_popcount32(hba->port_map) == 1 ? "" : "s");
	return devices;
}

/* ==========================================================================
 * kahci: the per-controller worker thread
 * ========================================================================== */

void ahci_kahci_kick(struct ahci_hba *hba, uint32_t work)
{
	uint64_t fl;

	if (unlikely(!ahci_hba_idx_ok(hba)))
		return;
	spin_lock_irqsave(&g_kahci_lock[hba->index], &fl);
	hba->kahci_work |= work;
	spin_unlock_irqrestore(&g_kahci_lock[hba->index], fl);
	if (hba->kahci)
		wq_wake_all(&hba->kahci_wq);
}

/* Take the pending work bits (atomically with respect to kicks). */
static uint32_t ahci_kahci_take_work(struct ahci_hba *hba)
{
	uint64_t fl;
	uint32_t w;

	spin_lock_irqsave(&g_kahci_lock[hba->index], &fl);
	w = hba->kahci_work;
	hba->kahci_work = 0;
	spin_unlock_irqrestore(&g_kahci_lock[hba->index], fl);
	return w;
}

/* Sleep until kicked or `nap_ms' passed (0: until kicked).  The work
 * word is checked and the task published as blocked under the same lock
 * a kick takes, so a kick can never fall between the check and the
 * sleep. */
static void ahci_kahci_sleep(struct ahci_hba *hba, uint32_t nap_ms)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;
	uint64_t fl;

	BUG_ON(cur == NULL);
	might_sleep();

	wq_entry_init(&we, cur);
	spin_lock_irqsave(&g_kahci_lock[hba->index], &fl);
	if (hba->kahci_work) {
		spin_unlock_irqrestore(&g_kahci_lock[hba->index], fl);
		return;
	}
	wq_add(&hba->kahci_wq, &we);
	cur->wait_channel = &hba->kahci_wq;
	cur->wakeup_tick = nap_ms ? timer_ticks() + timer_ms_to_ticks(nap_ms) + 1
				  : 0;
	task_set_blocked(cur);
	spin_unlock_irqrestore(&g_kahci_lock[hba->index], fl);
	sched_schedule();
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	wq_remove(&hba->kahci_wq, &we);
}

/* Is a controller reset requested by any port's error handling? */
static int ahci_hba_reset_requested(const struct ahci_hba *hba)
{
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		const struct ahci_port *port = hba->ports[i];

		if (port && (port->eh_pending & AHCI_EH_HBA_RESET))
			return 1;
	}
	return 0;
}

static void ahci_hba_gone(struct ahci_hba *hba);

static void kahci_main(void *arg)
{
	struct ahci_hba *hba = arg;
	uint32_t held = 0; /* work that arrived while suspended */

	BUG_ON(hba == NULL);
	hba->kahci_running = 1;
	kprintf("[ahci] %s: kahci%d running\n", hba->name, hba->index);

	for (;;) {
		uint32_t work = ahci_kahci_take_work(hba);
		uint32_t nap;
		enum ahci_hba_state st;

		if (work & AHCI_WORK_STOP)
			break;
		if (work)
			hba->stats.kahci_wakeups++;

		st = hba->state;
		if (st == AHCI_HBA_DEAD) {
			ahci_hba_gone(hba);
			ahci_kahci_sleep(hba, AHCI_KAHCI_NAP_FOREVER);
			continue;
		}
		if (st == AHCI_HBA_SUSPENDED) {
			/* Keep what arrived for after the resume (not as a
			 * kick: that would end every sleep at once). */
			held |= work & ~AHCI_WORK_POLL;
			ahci_kahci_sleep(hba, AHCI_KAHCI_NAP_FOREVER);
			continue;
		}
		work |= held;
		held = 0;

		/* 1. Controller recovery comes first: it supersedes every
		 * port-level action pending. */
		if ((work & AHCI_WORK_HBA_RESET) ||
		    ahci_hba_reset_requested(hba)) {
			struct blk_cpl_ctx doneq;

			/* The requests of the commands the recovery ends are
			 * finished after it released every port (its done
			 * queue). */
			blk_cpl_begin(&doneq);
			blk_cpl_collect_done(&doneq);
			ahci_hba_recover(hba);
			blk_cpl_end(&doneq);
		}
		if (hba->state == AHCI_HBA_DEAD)
			continue;

		/* 2. Polled controllers: drive completions. */
		if ((work & AHCI_WORK_POLL) || hba->irq_mode == AHCI_IRQ_POLL)
			ahci_hba_poll(hba);

		/* 4. Link power management requests (sysfs): every port
		 * applies its policy through EH. */
		if (work & AHCI_WORK_LPM) {
			for (int i = 0; i < AHCI_MAX_PORTS; i++)
				if (hba->ports[i])
					ahci_eh_schedule(hba->ports[i],
							 AHCI_EH_LPM);
		}

		/* 6. Error handling of every port that has something
		 * pending (errors, hot-plug, resets, LPM ...). */
		for (int i = 0; i < AHCI_MAX_PORTS; i++) {
			struct ahci_port *port = hba->ports[i];

			if (!port)
				continue;
			if (port->eh_pending & AHCI_EH_HBA_RESET) {
				/* escalated meanwhile: the next round
				 * recovers the controller, without a nap */
				ahci_kahci_kick(hba, AHCI_WORK_HBA_RESET);
				break;
			}
			if (port->eh_pending || port->host_link.eh_pending) {
				port->stats.eh_runs++;
				ahci_eh_run(port);
				/* hot-plug may have attached a device; the
				 * links are stable under the eh_mutex */
				ata_mutex_lock(&port->eh_mutex);
				ahci_port_sysfs_devices(port);
				ata_mutex_unlock(&port->eh_mutex);
			}
		}

		/* No lost-interrupt polling of the registers while commands
		 * are in flight.  The device raises its interrupt when it
		 * completes something; when it is merely slow -- a virtual
		 * disk whose host is busy writing back can hold every command
		 * for seconds -- reading its registers does not make it faster,
		 * and on a hypervisor every such read is an exit the device
		 * model may hold up for just as long, under the port lock with
		 * interrupts off, so that every other CPU wanting the port
		 * spun behind it and the whole guest stopped.  A completion
		 * that really went unreported is found the way any other
		 * stuck command is: by its timeout and error handling. */

		nap = hba->irq_mode == AHCI_IRQ_POLL ? AHCI_KAHCI_NAP_POLL_MS
						     : AHCI_KAHCI_NAP_FOREVER;
		ahci_kahci_sleep(hba, nap);
	}

	hba->kahci_running = 0;
	kprintf("[ahci] %s: kahci%d stopped\n", hba->name, hba->index);
	/* A kernel thread with a static stack is never reaped: park,
	 * discarding whatever is still kicked at a stopped controller. */
	for (;;) {
		(void)ahci_kahci_take_work(hba);
		ahci_kahci_sleep(hba, AHCI_KAHCI_NAP_FOREVER);
	}
}

int ahci_kahci_start(struct ahci_hba *hba)
{
	task_t *t;

	BUG_ON(hba == NULL);
	if (!ahci_hba_idx_ok(hba))
		return -EINVAL;
	if (hba->kahci)
		return 0;
	if (sched_current() == NULL) {
		WARN(1, "[ahci] %s: kahci started before the scheduler",
		     hba->name);
		return -EAGAIN;
	}
	t = sched_add_task(kahci_main, hba, hba->kahci_stack,
			   AHCI_KAHCI_STACK_SIZE);
	if (!t) {
		kprintf("[ahci] %s: cannot start kahci%d\n", hba->name,
			hba->index);
		return -ENOMEM;
	}
	ksnprintf(t->comm, sizeof(t->comm), "kahci%d", hba->index);
	hba->kahci = t;
	/* Anything latched during the polled boot gets handled now. */
	ahci_kahci_kick(hba, AHCI_WORK_EH);
	return 0;
}

/* The polled-mode interrupt substitute (hrtimer, IRQ context): drives the
 * completions itself, like an interrupt would, and re-arms. */
static void ahci_poll_timer_fn(hrtimer_t *t)
{
	struct ahci_hba *hba = t->arg;

	if (!hba || hba->irq_mode != AHCI_IRQ_POLL)
		return;
	if (ahci_hba_idx_ok(hba) && g_hba_shut[hba->index])
		return;
	if (hba->state == AHCI_HBA_RUNNING || hba->state == AHCI_HBA_POLLED)
		ahci_hba_poll_timer(hba);
	if (hba->state != AHCI_HBA_DEAD)
		hrtimer_start_rel(&hba->poll_timer,
				  (uint64_t)AHCI_POLL_INTERVAL_US * 1000ULL);
}

/* ==========================================================================
 * Controller recovery
 * ========================================================================== */

/* Record a recovery; 1 when there were too many within the window. */
static int ahci_hba_recover_too_often(struct ahci_hba *hba)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t *h = g_recover_ns[hba->index];
	int recent = 0, oldest = 0;

	for (int i = 0; i < AHCI_RECOVER_MAX_IN_WINDOW; i++) {
		if (h[i] && now - h[i] < AHCI_RECOVER_WINDOW_NS)
			recent++;
		if (h[i] < h[oldest])
			oldest = i;
	}
	h[oldest] = now;
	return recent >= AHCI_RECOVER_MAX_IN_WINDOW;
}

static void ahci_hba_lock_all_ports(struct ahci_hba *hba)
{
	for (int i = 0; i < AHCI_MAX_PORTS; i++)
		if (hba->ports[i])
			ata_mutex_lock(&hba->ports[i]->eh_mutex);
}

static void ahci_hba_unlock_all_ports(struct ahci_hba *hba)
{
	for (int i = AHCI_MAX_PORTS - 1; i >= 0; i--)
		if (hba->ports[i])
			ata_mutex_unlock(&hba->ports[i]->eh_mutex);
}

/* Freeze and stop one port (its eh_mutex held): no new commands, PxIE
 * masked, engines stopped. */
static void ahci_hba_quiesce_port(struct ahci_port *port)
{
	uint64_t fl;

	spin_lock_irqsave(&port->lock, &fl);
	if (port->state != AHCI_PORT_FROZEN && port->state != AHCI_PORT_UNUSED)
		ahci_eh_freeze_locked(port);
	ahci_port_irq_disable_locked(port);
	spin_unlock_irqrestore(&port->lock, fl);
	/* A controller that fell off the bus reads all ones: nothing to stop,
	 * and waiting for CR/FR to clear would only time out. */
	if (ahci_port_readl(port, AHCI_PxCMD) == 0xFFFFFFFFu)
		return;
	/* Commands in flight are claimed with the stop: the polled
	 * completion path (ahci_cmd_poll_port completes even on a frozen
	 * port) must not take the PxCI bits the stop clears for finished
	 * commands before ahci_port_fail_all() hands them back. */
	if (ahci_port_stop_engine_abort(port))
		kprintf("[ahci] %s: engine did not stop before the HBA reset\n",
			port->name);
	if (ahci_port_stop_fis_rx(port))
		kprintf("[ahci] %s: FIS receive did not stop before the HBA reset\n",
			port->name);
}

/* The controller cannot be recovered: fail everything, detach the
 * devices, disable the ports.  All eh_mutexes held. */
static void ahci_hba_give_up(struct ahci_hba *hba)
{
	uint64_t fl;

	kprintf("[ahci] %s: controller recovery failed -- disabling it\n",
		hba->name);
	ahci_hba_set_state(hba, AHCI_HBA_DEAD);
	ahci_hba_irq_disable(hba);
	hrtimer_cancel(&hba->poll_timer);
	/* No DMA any more: the commands whose slots it held can end. */
	ahci_pci_stop_dma(hba);
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		if (!port)
			continue;
		/* Engines stopped before the commands are failed and the
		 * devices dropped (idempotent when recovery got that far). */
		ahci_hba_quiesce_port(port);
		ahci_port_fail_all(port, -EIO);
		if (hba->dma_stopped)
			ahci_port_dma_done(port);
		if (port->pmp_attached) {
			for (uint32_t l = 0; l < port->nr_pmp_links &&
					     l < AHCI_PMP_MAX_LINKS;
			     l++)
				if (port->pmp_links[l].dev)
					ahci_link_detach(&port->pmp_links[l]);
		}
		if (port->host_link.dev)
			ahci_link_detach(&port->host_link);
		spin_lock_irqsave(&port->lock, &fl);
		port->state = AHCI_PORT_DISABLED;
		port->eh_pending = 0;
		spin_unlock_irqrestore(&port->lock, fl);
		wq_wake_all(&port->eh_wait);
	}
}

/* The interrupt handler found the function removed and marked the
 * controller dead: its commands are failed (buffers whose slots cannot be
 * confirmed clear stay with the hardware), its devices detached, its
 * ports disabled.  Once. */
static void ahci_hba_gone(struct ahci_hba *hba)
{
	uint64_t fl;
	int pending;

	spin_lock_irqsave(&hba->lock, &fl);
	pending = hba->gone_pending;
	hba->gone_pending = 0;
	spin_unlock_irqrestore(&hba->lock, fl);
	if (!pending)
		return;
	kprintf("[ahci] %s: controller removed -- disabling it\n", hba->name);
	ahci_hba_lock_all_ports(hba);
	ahci_hba_give_up(hba);
	ahci_hba_unlock_all_ports(hba);
}

int ahci_hba_recover(struct ahci_hba *hba)
{
	uint32_t old_cap, old_pi;
	uint64_t fl;
	int rc;

	BUG_ON(hba == NULL);
	ahci_might_sleep();

	if (hba->state == AHCI_HBA_DEAD || hba->state == AHCI_HBA_SUSPENDED)
		return -ENODEV;
	if (ahci_hba_idx_ok(hba) && g_hba_shut[hba->index])
		return -ENODEV;

	kprintf("[ahci] %s: controller recovery (generation %u)\n", hba->name,
		hba->generation);
	ahci_dump_hba(hba);

	/* Every port, ascending: the only place several eh_mutexes nest. */
	ahci_hba_lock_all_ports(hba);
	ahci_hba_set_state(hba, AHCI_HBA_RESETTING);
	hba->stats.host_errors++;

	if (ahci_hba_recover_too_often(hba)) {
		kprintf("[ahci] %s: %d controller resets within %llu s\n",
			hba->name, AHCI_RECOVER_MAX_IN_WINDOW,
			(unsigned long long)(AHCI_RECOVER_WINDOW_NS /
					     1000000000ULL));
		ahci_hba_give_up(hba);
		ahci_hba_unlock_all_ports(hba);
		return -EIO;
	}

	/* 1. Quiesce: frozen, no interrupts, engines stopped, the commands
	 * that were in flight handed back for requeueing. */
	ahci_hba_irq_disable(hba);
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		if (!port)
			continue;
		ahci_hba_quiesce_port(port);
		ahci_port_fail_all_for_reset(port, -ECANCELED);
		port->stats.hba_resets++;
		if (port->trace)
			ahci_trace(port, AHCI_TRACE_RESET, NULL, 0,
				   AHCI_EH_LVL_HBA_RESET);
	}

	/* 2. The function itself: a PCI error may have cost it its
	 * configuration. */
	if (pci_cfg_read16(hba->pci, 0x00) == 0xFFFF) {
		kprintf("[ahci] %s: the PCI function is gone\n", hba->name);
		ahci_hba_give_up(hba);
		ahci_hba_unlock_all_ports(hba);
		return -ENODEV;
	}
	ahci_pci_restore_state(hba);

	/* 3. Reset and rebuild the global state. */
	old_cap = hba->cap;
	old_pi = hba->port_map;
	rc = ahci_hba_bios_handoff(hba);
	if (rc == -ENODEV)
		goto dead;
	rc = ahci_hba_reset(hba);
	if (rc)
		goto dead;
	/* The reset stopped every DMA: the commands held for slots the
	 * controller still owned go back for requeueing. */
	for (int i = 0; i < AHCI_MAX_PORTS; i++)
		if (hba->ports[i])
			ahci_port_dma_done(hba->ports[i]);
	ahci_hba_read_caps(hba);
	if (hba->cap != old_cap)
		WARN(1, "[ahci] %s: CAP changed over the reset: 0x%08x -> 0x%08x",
		     hba->name, old_cap, hba->cap);
	if ((hba->port_map & old_pi) != old_pi) {
		WARN(1, "[ahci] %s: implemented ports changed over the reset: 0x%08x -> 0x%08x",
		     hba->name, old_pi, hba->port_map);
	}
	/* Only the ports that have objects keep running. */
	hba->port_map &= old_pi;
	ahci_hba_ccc_off(hba);
	ahci_hba_irq_disable(hba);
	ahci_hba_clear_is(hba);

	/* 4. Ports: registers reprogrammed from the DMA areas they own. */
	rc = ahci_hba_init_ports(hba, 1);
	if (rc)
		kprintf("[ahci] %s: some ports failed to re-initialise\n",
			hba->name);
	ahci_hba_clear_is(hba);
	if (hba->irq_mode != AHCI_IRQ_POLL)
		ahci_hba_irq_enable(hba);

	ahci_hba_set_state(hba, hba->kahci ? AHCI_HBA_RUNNING :
					    AHCI_HBA_POLLED);

	/* 5. Each port's EH hard-resets its link and revalidates what was
	 * there (or probes what arrived); the controller reset request is
	 * done. */
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];
		uint32_t actions;

		if (!port)
			continue;
		spin_lock_irqsave(&port->lock, &fl);
		port->eh_pending &= ~AHCI_EH_HBA_RESET;
		spin_unlock_irqrestore(&port->lock, fl);
		actions = ahci_port_count_devices(port) ?
				  (AHCI_EH_RESET_HARD | AHCI_EH_REVALIDATE) :
				  AHCI_EH_HOTPLUG;
		ahci_eh_schedule(port, actions);
	}
	ahci_hba_unlock_all_ports(hba);
	/* A port event that latched in IS between the last clear and the
	 * interrupt enable may never be signalled: collect it now, as the
	 * switch to interrupt mode at boot does. */
	if (hba->irq_mode != AHCI_IRQ_POLL)
		ahci_hba_poll(hba);
	if (hba->irq_mode == AHCI_IRQ_POLL && hba->kahci)
		hrtimer_start_rel(&hba->poll_timer,
				  (uint64_t)AHCI_POLL_INTERVAL_US * 1000ULL);
	kprintf("[ahci] %s: controller recovered (generation %u)\n", hba->name,
		hba->generation);
	return 0;

dead:
	ahci_hba_give_up(hba);
	ahci_hba_unlock_all_ports(hba);
	return rc ? rc : -EIO;
}

/* ==========================================================================
 * Shutdown, suspend, resume
 * ========================================================================== */

/* Stop the kahci thread and wait until it left its loop. */
static void ahci_kahci_stop(struct ahci_hba *hba)
{
	uint32_t waited = 0;

	if (!hba->kahci || !hba->kahci_running)
		return;
	ahci_kahci_kick(hba, AHCI_WORK_STOP);
	while (hba->kahci_running && waited < AHCI_KAHCI_STOP_WAIT_MS) {
		ahci_msleep(10);
		waited += 10;
	}
	if (hba->kahci_running)
		kprintf("[ahci] %s: kahci%d did not stop in %u ms\n", hba->name,
			hba->index, AHCI_KAHCI_STOP_WAIT_MS);
}

void ahci_hba_shutdown(struct ahci_hba *hba)
{
	BUG_ON(hba == NULL);
	ahci_might_sleep();

	if (!ahci_hba_idx_ok(hba) || g_hba_shut[hba->index])
		return;
	g_hba_shut[hba->index] = 1;

	/* No more error handling, polling or media checks. */
	ahci_kahci_stop(hba);
	hrtimer_cancel(&hba->poll_timer);

	if (hba->state != AHCI_HBA_DEAD && hba->state != AHCI_HBA_SUSPENDED) {
		for (int i = 0; i < AHCI_MAX_PORTS; i++)
			if (hba->ports[i])
				ahci_port_shutdown(hba->ports[i]);
	}
	ahci_hba_irq_disable(hba);
	ahci_hba_clear_is(hba);
	ahci_hba_set_state(hba, AHCI_HBA_SUSPENDED);
	kprintf("[ahci] %s: shut down\n", hba->name);
}

void ahci_shutdown_all(void)
{
	for (int idx = g_ahci_nr_hbas - 1; idx >= 0; idx--) {
		struct ahci_hba *hba = ahci_hba_get(idx);

		if (hba)
			ahci_hba_shutdown(hba);
	}
}

int ahci_hba_suspend(struct ahci_hba *hba)
{
	uint32_t suspended = 0;
	int rc = 0;

	BUG_ON(hba == NULL);
	ahci_might_sleep();

	if (hba->state != AHCI_HBA_RUNNING && hba->state != AHCI_HBA_POLLED)
		return hba->state == AHCI_HBA_SUSPENDED ? 0 : -EBUSY;
	if (ahci_hba_idx_ok(hba) && g_hba_shut[hba->index])
		return -ENODEV;

	kprintf("[ahci] %s: suspending\n", hba->name);
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		if (!port)
			continue;
		rc = ahci_port_suspend(port);
		if (rc) {
			kprintf("[ahci] %s: suspend failed (%d), resuming the others\n",
				port->name, rc);
			break;
		}
		suspended |= 1u << i;
	}
	if (rc) {
		/* Roll back exactly the ports suspended so far. */
		for (int i = 0; i < AHCI_MAX_PORTS; i++)
			if ((suspended & (1u << i)) && hba->ports[i])
				ahci_port_resume(hba->ports[i]);
		return rc;
	}

	hrtimer_cancel(&hba->poll_timer);
	ahci_hba_irq_disable(hba);
	hba->saved_ghc = ahci_hba_readl(hba, AHCI_GHC);
	if (hba->caps.cccs)
		hba->saved_ccc = ahci_hba_readl(hba, AHCI_CCC_CTL);
	ahci_hba_clear_is(hba);
	ahci_hba_set_state(hba, AHCI_HBA_SUSPENDED);

	ahci_pci_save_state(hba);
	if (!(hba->quirks & AHCI_Q_BROKEN_SUSPEND))
		ahci_pci_set_power(hba, 3);
	return 0;
}

int ahci_hba_resume(struct ahci_hba *hba)
{
	int rc;

	BUG_ON(hba == NULL);
	ahci_might_sleep();

	if (hba->state != AHCI_HBA_SUSPENDED)
		return hba->state == AHCI_HBA_DEAD ? -ENODEV : 0;
	if (ahci_hba_idx_ok(hba) && g_hba_shut[hba->index])
		return -ENODEV;

	kprintf("[ahci] %s: resuming\n", hba->name);
	ahci_pci_set_power(hba, 0);
	ahci_pci_restore_state(hba);
	if (pci_cfg_read16(hba->pci, 0x00) == 0xFFFF) {
		kprintf("[ahci] %s: the PCI function did not come back\n",
			hba->name);
		ahci_hba_set_state(hba, AHCI_HBA_DEAD);
		return -ENODEV;
	}

	/* The firmware may have taken the controller back while it slept;
	 * then reset and rebuild the global state as at boot. */
	ahci_hba_set_state(hba, AHCI_HBA_RESETTING);
	rc = ahci_hba_bios_handoff(hba);
	if (rc == -ENODEV)
		goto fail;
	ahci_hba_irq_disable(hba);
	rc = ahci_hba_reset(hba);
	if (rc)
		goto fail;
	{
		uint32_t old_map = hba->port_map;

		ahci_hba_read_caps(hba);
		hba->port_map &= old_map;
	}
	ahci_hba_ccc_off(hba);
	ahci_hba_clear_is(hba);
	ahci_hba_init_ports(hba, 0);
	ahci_hba_clear_is(hba);
	if (hba->irq_mode != AHCI_IRQ_POLL)
		ahci_hba_irq_enable(hba);
	ahci_hba_set_state(hba, hba->kahci ? AHCI_HBA_RUNNING :
					    AHCI_HBA_POLLED);
	/* An event latched between the last clear and the interrupt enable
	 * may never be signalled: collect it now. */
	if (hba->irq_mode != AHCI_IRQ_POLL)
		ahci_hba_poll(hba);

	/* Links up and devices revalidated, port by port. */
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		if (!port)
			continue;
		rc = ahci_port_resume(port);
		if (rc)
			kprintf("[ahci] %s: resume failed (%d)\n", port->name,
				rc);
	}
	if (hba->irq_mode == AHCI_IRQ_POLL && hba->kahci)
		hrtimer_start_rel(&hba->poll_timer,
				  (uint64_t)AHCI_POLL_INTERVAL_US * 1000ULL);
	ahci_kahci_kick(hba, AHCI_WORK_EH);
	kprintf("[ahci] %s: resumed\n", hba->name);
	return 0;

fail:
	kprintf("[ahci] %s: resume failed (%d)\n", hba->name, rc);
	ahci_hba_lock_all_ports(hba);
	ahci_hba_give_up(hba);
	ahci_hba_unlock_all_ports(hba);
	return rc ? rc : -EIO;
}

/* ==========================================================================
 * Boot entry points
 * ========================================================================== */

/* Undo a half-initialised controller (probe failure).  Only ahci_init()
 * calls this, during the polled boot probe: no interrupt handler is
 * registered for the controller yet (ahci_pci_setup_irq comes with
 * ahci_late_init), so a port freed here cannot be inside the handler. */
static void ahci_hba_abandon(struct ahci_hba *hba, int rc)
{
	kprintf("[ahci] %s: not usable (%d)\n", hba->name, rc);
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		struct ahci_port *port = hba->ports[i];

		if (!port)
			continue;
		/* Ports that never ran a command: safe to free. */
		if (port->active == 0 && port->slot_map == 0) {
			ahci_port_free(port);
			hba->ports[i] = NULL;
		}
	}
	hba->port_map = 0;
	if (hba->mmio)
		ahci_hba_irq_disable(hba);
	ahci_hba_set_state(hba, AHCI_HBA_DEAD);
}

void ahci_init(void)
{
	int n = 0, found = 0, devices = 0;
	const pci_device_t *devs = pci_get_devices(&n);

	ahci_layout_checks();
	ata_layout_checks();
	BUILD_BUG_ON(AHCI_MAX_HBAS > 64);
	BUILD_BUG_ON(AHCI_KAHCI_STACK_SIZE < 8192);

	if (!devs || n <= 0) {
		kprintf("[ahci] no PCI devices\n");
		return;
	}

	for (int i = 0; i < n; i++) {
		const pci_device_t *pci = &devs[i];
		const struct ahci_pci_id *id = ahci_pci_match(pci);
		struct ahci_hba *hba;
		int rc;

		if (!id)
			continue;
		found++;
		hba = ahci_hba_alloc(pci, id);
		if (!hba)
			continue;
		rc = ahci_pci_enable(hba);
		if (rc) {
			ahci_hba_abandon(hba, rc);
			continue;
		}
		rc = ahci_hba_init(hba);
		if (rc) {
			ahci_hba_abandon(hba, rc);
			continue;
		}
		devices += ahci_hba_start_ports(hba);
	}
	if (found == 0)
		kprintf("[ahci] no AHCI controller found\n");
	else
		kprintf("[ahci] %d controller%s, %d device%s\n", g_ahci_nr_hbas,
			g_ahci_nr_hbas == 1 ? "" : "s", devices,
			devices == 1 ? "" : "s");
}

void ahci_late_init(void)
{
	for (int idx = 0; idx < g_ahci_nr_hbas; idx++) {
		struct ahci_hba *hba = ahci_hba_get(idx);
		uint64_t fl;

		if (!hba || hba->state == AHCI_HBA_DEAD || !hba->mmio)
			continue;

		/* 1. An interrupt (or the poll timer). */
		ahci_pci_setup_irq(hba);
		for (int i = 0; i < AHCI_MAX_PORTS; i++) {
			struct ahci_port *port = hba->ports[i];

			if (!port)
				continue;
			spin_lock_irqsave(&port->lock, &fl);
			port->polled = hba->irq_mode == AHCI_IRQ_POLL;
			spin_unlock_irqrestore(&port->lock, fl);
		}
		if (hba->irq_mode != AHCI_IRQ_POLL)
			ahci_hba_irq_enable(hba);
		else
			hrtimer_start_rel(&hba->poll_timer,
					  (uint64_t)AHCI_POLL_INTERVAL_US *
						  1000ULL);

		/* 2. The worker. */
		if (ahci_kahci_start(hba) != 0)
			kprintf("[ahci] %s: running without kahci: no error recovery, no hot-plug\n",
				hba->name);
		ahci_hba_set_state(hba, AHCI_HBA_RUNNING);

		/* 3. Anything that finished between the last boot poll and
		 * the interrupt enable. */
		ahci_hba_poll(hba);
	}
	/* The optical drives found at boot start their media events work
	 * now that kblockd runs. */
	atapi_events_start();
}
