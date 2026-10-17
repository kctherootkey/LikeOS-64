// LikeOS Intel ICH AC'97 controller driver (and register-compatible parts).
//
// The controller has two register windows:
//   NAMBAR  (BAR0, I/O; MMBAR = BAR2, memory, on ICH4 and later): a direct
//           window onto the codec registers, 0x80 bytes per codec;
//   NABMBAR (BAR1, I/O; MBBAR = BAR3, memory, on ICH4 and later): the bus
//           master engines (PCM in at 0x00, PCM out at 0x10, mic in at
//           0x20, more on ICH4+) and the global control/status registers.
//
// Each engine walks a buffer descriptor list of 32 eight-byte entries
// (buffer address, 16-bit sample count, interrupt-on-completion) up to the
// "last valid index".  The ring the sound core hands us is cut into
// periods; the 32 BDL entries hold consecutive periods and every
// completion interrupt advances LVI by one and refills the entry that was
// just played with the period that follows the one before it.  The
// engine therefore never reaches LVI while interrupts arrive, any period
// count works (not only divisors of 32), and the DMA position is the
// period of the current entry (CIV) plus what the engine has consumed of
// it (the entry length minus PICB).
//
// Supported: Intel 82801AA/AB/BA/CA/DB/EB/ESB/FB/GB/ESB2 and 440MX (the
// "ICH AC97" of VirtualBox and QEMU's -device AC97 are the 82801AA), SiS
// 7012 (status and position registers swapped, positions in bytes),
// NVIDIA nForce/nForce2/nForce3/CK8/CK804/MCP04/MCP51 and AMD 768/8111.
// The ALi M5455 uses a different register model and is not claimed.
//
// Copyright (C) 2026 The LikeOS Project

#include "ac97_codec.h"
#include <kernel/ke/syscall.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/acpi.h>
#include <kernel/hal/pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define DRV "ac97"

/* ---- registers ------------------------------------------------------------ */

/* Bus master engine blocks (offsets in NABMBAR) */
#define ICH_PI 0x00 /* PCM in */
#define ICH_PO 0x10 /* PCM out */
#define ICH_MC 0x20 /* mic in */
#define ICH_MC2 0x40 /* ICH4: mic in 2 */
#define ICH_PI2 0x50 /* ICH4: PCM in 2 */
#define ICH_SP 0x60 /* ICH4: S/PDIF out */
#define ICH_NV_SP 0x70 /* nForce: S/PDIF out */

/* Registers inside a block */
#define ICH_OFF_BDBAR 0x00 /* dword: BDL base (8-byte aligned) */
#define ICH_OFF_CIV 0x04 /* byte: current index */
#define ICH_OFF_LVI 0x05 /* byte: last valid index */
#define ICH_OFF_SR 0x06 /* word: status (0x08 on SiS) */
#define ICH_OFF_PICB 0x08 /* word: position in current buffer (0x06 SiS) */
#define ICH_OFF_PIV 0x0a /* byte: prefetched index */
#define ICH_OFF_CR 0x0b /* byte: control */

#define ICH_IDX_MASK 0x1f
#define ICH_BDL_ENTRIES 32

/* SR */
#define ICH_SR_DCH 0x01 /* DMA controller halted */
#define ICH_SR_CELV 0x02 /* current equals last valid */
#define ICH_SR_LVBCI 0x04 /* last valid buffer completed */
#define ICH_SR_BCIS 0x08 /* buffer completed (IOC) */
#define ICH_SR_FIFOE 0x10 /* FIFO error */
#define ICH_SR_ACK (ICH_SR_LVBCI | ICH_SR_BCIS | ICH_SR_FIFOE)

/* CR */
#define ICH_CR_RPBM 0x01 /* run */
#define ICH_CR_RR 0x02 /* reset registers */
#define ICH_CR_LVBIE 0x04
#define ICH_CR_FEIE 0x08
#define ICH_CR_IOCE 0x10

/* BDL entry control word */
#define ICH_BD_IOC 0x80000000u
#define ICH_BD_BUP 0x40000000u /* send zeros after the last valid buffer */

/* Global registers */
#define ICH_GLOB_CNT 0x2c
#define ICH_GC_GIE 0x00000001
#define ICH_GC_COLD 0x00000002 /* 0 = cold reset asserted */
#define ICH_GC_WARM 0x00000004
#define ICH_GC_ACLINK_OFF 0x00000008
#define ICH_GC_PRIE 0x00000010
#define ICH_GC_SRIE 0x00000020
#define ICH_GC_TRIE 0x00000040
#define ICH_GC_PCM_246_MASK 0x00300000
#define ICH_GC_PCM_20BIT 0x00400000
#define ICH_GC_SIS_PCM_246_MASK 0x000000c0
#define ICH_GC_SPDIF_MASK 0xc0000000

#define ICH_GLOB_STA 0x30
#define ICH_GS_GSCI 0x00000001
#define ICH_GS_MIINT 0x00000002
#define ICH_GS_MOINT 0x00000004
#define ICH_GS_NVSPINT 0x00000010
#define ICH_GS_PIINT 0x00000020
#define ICH_GS_POINT 0x00000040
#define ICH_GS_MCINT 0x00000080
#define ICH_GS_PCR 0x00000100 /* primary codec ready */
#define ICH_GS_SCR 0x00000200 /* secondary codec ready */
#define ICH_GS_PRI 0x00000400 /* primary resume */
#define ICH_GS_SRI 0x00000800
#define ICH_GS_RCS 0x00008000 /* codec read timed out */
#define ICH_GS_AD3 0x00010000
#define ICH_GS_MD3 0x00020000
#define ICH_GS_SIS_TCR 0x00040000
#define ICH_GS_SIS_TRI 0x00080000
#define ICH_GS_SAMPLE_CAP 0x00c00000
#define ICH_GS_MULTICHAN_CAP 0x00300000
#define ICH_GS_M2INT 0x01000000
#define ICH_GS_P2INT 0x02000000
#define ICH_GS_SPINT 0x04000000
#define ICH_GS_BCS 0x08000000
#define ICH_GS_TCR 0x10000000 /* ICH4: tertiary codec ready */
#define ICH_GS_TRI 0x20000000

#define ICH_ACC_SEMA 0x34 /* byte: codec access semaphore */
#define ICH_CAS 0x01

#define ICH_SDM 0x80 /* ICH4: SDATA_IN map */
#define ICH_SDM_LDI_MASK 0x03

#define ICH_SIS_OUT_CTRL 0x4c /* SiS 7012: bit 0 unmutes the output */

/* Controller families with different register details */
enum ich_type {
	ICH_TYPE_INTEL, /* ICH0-ICH3, 440MX, AMD 768/8111 */
	ICH_TYPE_ICH4, /* ICH4 and later: 3 codecs, SDM, MMIO BARs */
	ICH_TYPE_SIS, /* SiS 7012 */
	ICH_TYPE_NFORCE, /* NVIDIA nForce family */
};

#define ICH_F_NO_COLD_RESET (1u << 0)
#define ICH_F_440MX (1u << 1)

struct ich_id {
	uint16_t vendor, device;
	uint8_t type;
	uint32_t flags;
	const char *name;
};

static const struct ich_id ich_ids[] = {
	{ 0x8086, 0x2415, ICH_TYPE_INTEL, 0, "Intel 82801AA (ICH) AC'97" },
	{ 0x8086, 0x2425, ICH_TYPE_INTEL, 0, "Intel 82801AB (ICH0) AC'97" },
	{ 0x8086, 0x2445, ICH_TYPE_INTEL, 0, "Intel 82801BA (ICH2) AC'97" },
	{ 0x8086, 0x2485, ICH_TYPE_INTEL, 0, "Intel 82801CA (ICH3) AC'97" },
	{ 0x8086, 0x24c5, ICH_TYPE_ICH4, 0, "Intel 82801DB (ICH4) AC'97" },
	{ 0x8086, 0x24d5, ICH_TYPE_ICH4, 0, "Intel 82801EB (ICH5) AC'97" },
	{ 0x8086, 0x25a6, ICH_TYPE_ICH4, 0, "Intel 6300ESB AC'97" },
	{ 0x8086, 0x266e, ICH_TYPE_ICH4, 0, "Intel 82801FB (ICH6) AC'97" },
	{ 0x8086, 0x27de, ICH_TYPE_ICH4, 0, "Intel 82801GB (ICH7) AC'97" },
	{ 0x8086, 0x2698, ICH_TYPE_ICH4, 0, "Intel 631xESB AC'97" },
	{ 0x8086, 0x7195, ICH_TYPE_INTEL, ICH_F_440MX, "Intel 440MX AC'97" },
	{ 0x1039, 0x7012, ICH_TYPE_SIS, 0, "SiS 7012 AC'97" },
	{ 0x10de, 0x01b1, ICH_TYPE_NFORCE, 0, "NVIDIA nForce AC'97" },
	{ 0x10de, 0x006a, ICH_TYPE_NFORCE, 0, "NVIDIA nForce2 AC'97" },
	{ 0x10de, 0x00da, ICH_TYPE_NFORCE, 0, "NVIDIA nForce3 AC'97" },
	{ 0x10de, 0x00ea, ICH_TYPE_NFORCE, 0, "NVIDIA nForce3 250 (CK8S) AC'97" },
	{ 0x10de, 0x008a, ICH_TYPE_NFORCE, 0, "NVIDIA nForce2 400 (CK8) AC'97" },
	{ 0x10de, 0x0059, ICH_TYPE_NFORCE, 0, "NVIDIA nForce4 (CK804) AC'97" },
	{ 0x10de, 0x003a, ICH_TYPE_NFORCE, 0, "NVIDIA MCP04 AC'97" },
	{ 0x10de, 0x026b, ICH_TYPE_NFORCE, 0, "NVIDIA MCP51 AC'97" },
	{ 0x1022, 0x746d, ICH_TYPE_INTEL, 0, "AMD 8111 AC'97" },
	{ 0x1022, 0x7445, ICH_TYPE_INTEL, 0, "AMD 768 AC'97" },
	{ 0, 0, 0, 0, 0 }
};

/* Boards whose codec clock is known, so it is not measured. */
struct ich_clock_quirk {
	uint16_t vendor, device;
	uint32_t clock;
};

static const struct ich_clock_quirk ich_clock_quirks[] = {
	{ 0x0e11, 0x008a, 41000 }, /* AD1885 */
	{ 0x1014, 0x0581, 48000 }, /* AD1981B */
	{ 0x1028, 0x00be, 44100 }, /* AD1885 */
	{ 0x1028, 0x0177, 48000 }, /* AD1980 */
	{ 0x1028, 0x01ad, 48000 }, /* AD1981B */
	{ 0x1043, 0x80f3, 48000 }, /* AD1985 */
	{ 0, 0, 0 }
};

/* Boards on which a cold link reset loses the codec. */
static const struct {
	uint16_t vendor, device;
} ich_no_cold_reset[] = {
	{ 0x1014, 0x051f }, /* ThinkPad R32 */
	{ 0, 0 }
};

/* ---- driver state --------------------------------------------------------- */

struct ich_chip;

struct ich_stream {
	struct ich_chip *chip;
	int dir; /* AUDIO_DIR_* */
	int present;
	uint32_t reg; /* engine block offset */
	uint32_t sr_off, picb_off;
	uint32_t int_bit; /* GLOB_STA bit */
	uint32_t pos_shift; /* bytes = PICB << pos_shift */

	/* the 32-entry BDL (in chip->bdl_area) */
	volatile uint32_t *bdl;
	uint32_t bdl_phys;
	uint8_t bdl_period[ICH_BDL_ENTRIES];

	/* runtime, under chip->lock */
	struct snd_pcm_substream *ss;
	uint32_t buf_phys;
	uint32_t buffer_bytes, period_bytes, periods;
	uint32_t rate;
	uint32_t civ, lvi, lvi_period;
	int configured; /* hw_params done */
	int prepared; /* BDL loaded and engine reset */
	int running; /* engine started (not paused) */
	int paused;
	uint32_t last_pos;
	uint32_t last_period, last_done;

	/* interrupt health and polling */
	hrtimer_t timer;
	uint64_t poll_ns;
	int poll_fast; /* interrupts are not arriving: poll per period */
	int poll_hits; /* consecutive polls that found unserviced work */
	uint64_t irqs_seen; /* s->irqs at the previous poll */
	uint64_t irqs, polls, poll_found, periods_done;
	uint32_t fifo_errors, lvi_stalls;
};

struct ich_chip {
	const pci_device_t *pci;
	const struct ich_id *idinfo;
	int type;
	uint16_t ssvid, ssdid;
	int in_vm;

	/* register windows */
	int mix_mmio, bm_mmio;
	uint16_t mix_port, bm_port;
	volatile uint8_t *mix_mem, *bm_mem;
	uint64_t mix_base, bm_base;
	uint32_t mix_size, bm_size;
	uint64_t mix_map, bm_map; /* virtual (page aligned) for unmapping */
	size_t mix_pages, bm_pages;
	uint16_t pci_cmd_saved;

	spinlock_t lock; /* engine registers and stream runtime */

	/* codecs */
	unsigned int max_codecs;
	uint32_t codec_bits[3]; /* GLOB_STA ready bit per SDIN */
	uint32_t codec_isr_bits; /* all of them */
	uint32_t ready_seen; /* ready bits found after reset */
	unsigned int sdin[AC97_MAX_CODECS]; /* codec number -> SDIN line */
	int sdin_known;
	struct ac97_codec codec;
	uint32_t clock;

	/* engines */
	void *bdl_area;
	uint64_t bdl_phys;
	struct ich_stream st[2]; /* [AUDIO_DIR_PLAYBACK] = PO, [CAPTURE] = PI */
	uint32_t int_mask;
	uint32_t engine_regs[8];
	unsigned int n_engines;

	/* interrupt */
	int gsi;
	int irq_ok;
	volatile int irq_armed;
	uint64_t irq_count, irq_foreign;
	int gone;
	int gone_reported;

	struct snd_card *card;
	struct snd_pcm *pcm;
};

/* ---- register access ------------------------------------------------------ */

/* Every access is checked against the window size, so a short BAR (or a
 * register only some parts have) reads as all-ones and ignores writes
 * instead of touching a neighbouring device. */

static inline int win_ok(uint32_t size, uint32_t off, uint32_t width)
{
	return off + width <= size;
}

static uint8_t bm_rd8(struct ich_chip *c, uint32_t off)
{
	if (!win_ok(c->bm_size, off, 1))
		return 0xff;
	if (c->bm_mmio)
		return *(volatile uint8_t *)(c->bm_mem + off);
	return inb((uint16_t)(c->bm_port + off));
}

static uint16_t bm_rd16(struct ich_chip *c, uint32_t off)
{
	if (!win_ok(c->bm_size, off, 2))
		return 0xffff;
	if (c->bm_mmio)
		return *(volatile uint16_t *)(c->bm_mem + off);
	return inw((uint16_t)(c->bm_port + off));
}

static uint32_t bm_rd32(struct ich_chip *c, uint32_t off)
{
	if (!win_ok(c->bm_size, off, 4))
		return 0xffffffffu;
	if (c->bm_mmio)
		return *(volatile uint32_t *)(c->bm_mem + off);
	return inl((uint16_t)(c->bm_port + off));
}

static void bm_wr8(struct ich_chip *c, uint32_t off, uint8_t v)
{
	if (!win_ok(c->bm_size, off, 1))
		return;
	if (c->bm_mmio)
		*(volatile uint8_t *)(c->bm_mem + off) = v;
	else
		outb((uint16_t)(c->bm_port + off), v);
}

static void bm_wr16(struct ich_chip *c, uint32_t off, uint16_t v)
{
	if (!win_ok(c->bm_size, off, 2))
		return;
	if (c->bm_mmio)
		*(volatile uint16_t *)(c->bm_mem + off) = v;
	else
		outw((uint16_t)(c->bm_port + off), v);
}

static void bm_wr32(struct ich_chip *c, uint32_t off, uint32_t v)
{
	if (!win_ok(c->bm_size, off, 4))
		return;
	if (c->bm_mmio)
		*(volatile uint32_t *)(c->bm_mem + off) = v;
	else
		outl((uint16_t)(c->bm_port + off), v);
}

static uint16_t mix_rd16(struct ich_chip *c, uint32_t off)
{
	if (!win_ok(c->mix_size, off, 2))
		return 0xffff;
	if (c->mix_mmio)
		return *(volatile uint16_t *)(c->mix_mem + off);
	return inw((uint16_t)(c->mix_port + off));
}

static void mix_wr16(struct ich_chip *c, uint32_t off, uint16_t v)
{
	if (!win_ok(c->mix_size, off, 2))
		return;
	if (c->mix_mmio)
		*(volatile uint16_t *)(c->mix_mem + off) = v;
	else
		outw((uint16_t)(c->mix_port + off), v);
}

static inline void ich_mb(void)
{
	__asm__ volatile("mfence" ::: "memory");
}

/* The SR register of an engine is 16 bits wide on Intel parts but only
 * its low byte carries status; byte access works everywhere. */
static uint8_t st_sr(struct ich_chip *c, struct ich_stream *s)
{
	return bm_rd8(c, s->reg + s->sr_off);
}

static void st_ack(struct ich_chip *c, struct ich_stream *s, uint8_t bits)
{
	bm_wr8(c, s->reg + s->sr_off, bits);
}

/* ---- codec bus ------------------------------------------------------------ */

static uint32_t codec_ready_bit(struct ich_chip *c, unsigned int num)
{
	if (!c->sdin_known || num >= AC97_MAX_CODECS)
		return c->codec_isr_bits;
	if (c->sdin[num] >= 3)
		return c->codec_isr_bits;
	return c->codec_bits[c->sdin[num]];
}

/* Codec ready, then the access semaphore.  Reading ACC_SEMA returns the
 * semaphore and takes it; the controller drops it once the codec cycle
 * completes.  A semaphore that stays busy (some register accesses to
 * non-existent codec registers never release it) is ignored after a
 * millisecond: the access itself still works. */
static int codec_access_begin(struct ich_chip *c, unsigned int num)
{
	uint32_t sta = bm_rd32(c, ICH_GLOB_STA);
	int i;

	if (sta == 0xffffffffu)
		return -ENODEV;
	/* No ready bit seen at all after reset (some emulations): try
	 * anyway, the read timeout bit will tell. */
	if (c->ready_seen && !(sta & codec_ready_bit(c, num)))
		return -EIO;
	for (i = 0; i < 100; i++) {
		if (!(bm_rd8(c, ICH_ACC_SEMA) & ICH_CAS))
			return 0;
		snd_udelay(10);
	}
	return -EBUSY;
}

static int ich_codec_read(void *bus, unsigned int num, unsigned int reg,
			  uint16_t *val)
{
	struct ich_chip *c = bus;
	uint32_t off, sta;
	uint16_t v;
	int rc;

	*val = 0xffff;
	if (num >= c->max_codecs || reg > 0x7e || (reg & 1))
		return -EINVAL;
	off = num * 0x80 + reg;
	if (!win_ok(c->mix_size, off, 2))
		return -EINVAL;
	rc = codec_access_begin(c, num);
	if (rc == -EIO || rc == -ENODEV)
		return rc;
	v = mix_rd16(c, off);
	sta = bm_rd32(c, ICH_GLOB_STA);
	if (sta != 0xffffffffu && (sta & ICH_GS_RCS)) {
		/* read timed out: clear RCS (write-1-to-clear) and nothing
		 * else, so pending interrupt status is not lost */
		bm_wr32(c, ICH_GLOB_STA, ICH_GS_RCS);
		return -EIO;
	}
	*val = v;
	return 0;
}

static int ich_codec_write(void *bus, unsigned int num, unsigned int reg,
			   uint16_t val)
{
	struct ich_chip *c = bus;
	uint32_t off;
	int rc;

	if (num >= c->max_codecs || reg > 0x7e || (reg & 1))
		return -EINVAL;
	off = num * 0x80 + reg;
	if (!win_ok(c->mix_size, off, 2))
		return -EINVAL;
	rc = codec_access_begin(c, num);
	if (rc == -EIO || rc == -ENODEV)
		return rc;
	mix_wr16(c, off, val);
	return 0;
}

static const struct ac97_bus_ops ich_bus_ops = {
	.read = ich_codec_read,
	.write = ich_codec_write,
};

/* ---- helpers -------------------------------------------------------------- */

static int cpu_has_hypervisor(void)
{
	uint32_t a = 1, b, cx = 0, d;
	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(cx), "=d"(d));
	(void)b;
	(void)d;
	return (cx >> 31) & 1;
}

static int detect_vm(const struct ich_chip *c)
{
	if (c->ssvid == 0x1af4 && c->ssdid == 0x1100)
		return 1; /* QEMU/KVM emulated sound */
	if (c->ssvid == 0x1ab8)
		return 1; /* Parallels */
	return cpu_has_hypervisor();
}

static uint32_t clock_quirk(const struct ich_chip *c)
{
	const struct ich_clock_quirk *q;
	for (q = ich_clock_quirks; q->clock; q++)
		if (q->vendor == c->ssvid && q->device == c->ssdid)
			return q->clock;
	return 0;
}

static int no_cold_reset(const struct ich_chip *c)
{
	int i;
	if (c->idinfo->flags & ICH_F_NO_COLD_RESET)
		return 1;
	for (i = 0; ich_no_cold_reset[i].vendor; i++)
		if (ich_no_cold_reset[i].vendor == c->ssvid &&
		    ich_no_cold_reset[i].device == c->ssdid)
			return 1;
	return 0;
}

static struct ich_stream *ss_stream(struct snd_pcm_substream *ss)
{
	struct ich_chip *c;

	if (!ss || !ss->pcm || !ss->pcm->private_data)
		return 0;
	if (ss->direction != AUDIO_DIR_PLAYBACK &&
	    ss->direction != AUDIO_DIR_CAPTURE)
		return 0;
	c = ss->pcm->private_data;
	return &c->st[ss->direction];
}

/* Formatted append into a bounded buffer. */
struct ich_dbuf {
	char *buf;
	size_t len;
	size_t pos;
};

static void dbuf_printf(struct ich_dbuf *d, const char *fmt, ...)
{
	__builtin_va_list ap;
	if (!d->buf || d->len == 0 || d->pos + 1 >= d->len)
		return;
	__builtin_va_start(ap, fmt);
	kvsnprintf(d->buf + d->pos, d->len - d->pos, fmt, ap);
	__builtin_va_end(ap);
	d->pos += kstrlen(d->buf + d->pos);
	if (d->pos >= d->len)
		d->pos = d->len - 1;
}

/* ---- PCI resources -------------------------------------------------------- */

static int map_window(const struct pci_bar *b, uint32_t min_size,
		      uint32_t max_size, int *mmio, uint16_t *port,
		      volatile uint8_t **mem, uint64_t *map, size_t *pages,
		      uint32_t *size, uint64_t *base)
{
	uint64_t sz = b->size;

	if (b->base == 0)
		return -ENODEV;
	if (sz == 0)
		sz = min_size;
	if (sz < min_size)
		return -ENODEV;
	if (sz > max_size)
		sz = max_size;

	if (b->flags & PCI_BAR_IO) {
		if (b->base > 0xffff || b->base + sz > 0x10000)
			return -ENODEV;
		*mmio = 0;
		*port = (uint16_t)b->base;
		*size = (uint32_t)sz;
		*base = b->base;
		return 0;
	} else {
		uint64_t off = b->base & 0xfffull;
		size_t np = (size_t)((off + sz + 0xfff) >> 12);
		uint64_t v = mm_map_mmio_flags(b->base, np, MM_MMIO_UC);
		if (!v)
			return -ENOMEM;
		*mmio = 1;
		*mem = (volatile uint8_t *)v;
		*map = v & ~0xfffull;
		*pages = np;
		*size = (uint32_t)sz;
		*base = b->base;
		return 0;
	}
}

static void unmap_windows(struct ich_chip *c)
{
	if (c->mix_mmio && c->mix_map)
		mm_unmap_mmio(c->mix_map, c->mix_pages);
	if (c->bm_mmio && c->bm_map)
		mm_unmap_mmio(c->bm_map, c->bm_pages);
	c->mix_map = c->bm_map = 0;
	c->mix_size = c->bm_size = 0;
}

/* NAMBAR is 256 bytes of I/O (two codecs) or 512 bytes of MMIO (three);
 * NABMBAR is 64 bytes on ICH0-3 and 256 on ICH4+ and the clones. */
static int ich_map(struct ich_chip *c)
{
	struct pci_bar b0, b1, b2, b3;
	int have2 = 0, have3 = 0, rc;

	kmemset(&b0, 0, sizeof(b0));
	kmemset(&b1, 0, sizeof(b1));
	kmemset(&b2, 0, sizeof(b2));
	kmemset(&b3, 0, sizeof(b3));

	/* ICH4+ and nForce: the memory windows when the firmware set them
	 * up (the I/O windows may be disabled there). */
	if (c->type == ICH_TYPE_ICH4 || c->type == ICH_TYPE_NFORCE) {
		have2 = pci_bar_decode(c->pci, 2, &b2) == 0 &&
			!(b2.flags & PCI_BAR_IO) && b2.base;
		have3 = pci_bar_decode(c->pci, 3, &b3) == 0 &&
			!(b3.flags & PCI_BAR_IO) && b3.base;
	}

	rc = -ENODEV;
	if (have2)
		rc = map_window(&b2, 0x100, 0x200, &c->mix_mmio, &c->mix_port,
				&c->mix_mem, &c->mix_map, &c->mix_pages,
				&c->mix_size, &c->mix_base);
	if (rc) {
		if (pci_bar_decode(c->pci, 0, &b0) != 0)
			return -ENODEV;
		rc = map_window(&b0, 0x80, 0x200, &c->mix_mmio, &c->mix_port,
				&c->mix_mem, &c->mix_map, &c->mix_pages,
				&c->mix_size, &c->mix_base);
		if (rc)
			return rc;
	}

	rc = -ENODEV;
	if (have3)
		rc = map_window(&b3, 0x40, 0x100, &c->bm_mmio, &c->bm_port,
				&c->bm_mem, &c->bm_map, &c->bm_pages,
				&c->bm_size, &c->bm_base);
	if (rc) {
		if (pci_bar_decode(c->pci, 1, &b1) != 0) {
			unmap_windows(c);
			return -ENODEV;
		}
		rc = map_window(&b1, 0x40, 0x100, &c->bm_mmio, &c->bm_port,
				&c->bm_mem, &c->bm_map, &c->bm_pages,
				&c->bm_size, &c->bm_base);
		if (rc) {
			unmap_windows(c);
			return rc;
		}
	}
	return 0;
}

static void ich_enable_pci(struct ich_chip *c)
{
	uint16_t cmd = pci_cfg_read16(c->pci, 0x04);

	c->pci_cmd_saved = cmd;
	if (!c->mix_mmio || !c->bm_mmio)
		cmd |= 0x0001; /* I/O space */
	if (c->mix_mmio || c->bm_mmio)
		cmd |= 0x0002; /* memory space */
	cmd |= 0x0004; /* bus master */
	cmd &= (uint16_t)~PCI_CMD_INTX_DISABLE;
	pci_cfg_write16(c->pci, 0x04, cmd);
}

/* ---- engines -------------------------------------------------------------- */

/* Stop an engine and reset its registers (CIV, LVI, PICB, PIV, SR, CR).
 * Bounded busy waits; callable with interrupts off. */
static void engine_reset(struct ich_chip *c, uint32_t reg, uint32_t sr_off)
{
	int i;

	bm_wr8(c, reg + ICH_OFF_CR, 0);
	for (i = 0; i < 200; i++) {
		uint8_t sr = bm_rd8(c, reg + sr_off);
		if (sr == 0xff || (sr & ICH_SR_DCH))
			break;
		snd_udelay(5);
	}
	bm_wr8(c, reg + ICH_OFF_CR, ICH_CR_RR);
	for (i = 0; i < 200; i++) {
		uint8_t cr = bm_rd8(c, reg + ICH_OFF_CR);
		if (cr == 0xff || !(cr & ICH_CR_RR))
			break;
		snd_udelay(5);
	}
	bm_wr8(c, reg + sr_off, ICH_SR_ACK);
}

static void stream_engine_reset(struct ich_chip *c, struct ich_stream *s)
{
	engine_reset(c, s->reg, s->sr_off);
	s->running = 0;
	s->paused = 0;
	s->prepared = 0;
}

static uint32_t bd_ctl(const struct ich_stream *s)
{
	uint32_t ctl = ICH_BD_IOC | ((s->period_bytes >> s->pos_shift) & 0xffff);
	if (s->dir == AUDIO_DIR_PLAYBACK)
		ctl |= ICH_BD_BUP;
	return ctl;
}

static void bdl_set(struct ich_stream *s, uint32_t idx, uint32_t period)
{
	s->bdl_period[idx] = (uint8_t)period;
	s->bdl[idx * 2] = s->buf_phys + period * s->period_bytes;
	s->bdl[idx * 2 + 1] = bd_ctl(s);
}

/* Load the whole BDL with consecutive periods and point the engine at
 * it.  The engine must be reset (halted, CIV = 0). */
static void stream_load(struct ich_chip *c, struct ich_stream *s)
{
	uint32_t i;

	for (i = 0; i < ICH_BDL_ENTRIES; i++)
		bdl_set(s, i, i % s->periods);
	ich_mb();
	s->civ = 0;
	s->lvi = ICH_IDX_MASK;
	s->lvi_period = s->bdl_period[ICH_IDX_MASK];
	s->last_pos = 0;
	s->last_period = 0;
	s->last_done = 0;
	bm_wr32(c, s->reg + ICH_OFF_BDBAR, s->bdl_phys);
	bm_wr8(c, s->reg + ICH_OFF_LVI, (uint8_t)s->lvi);
	st_ack(c, s, ICH_SR_ACK);
	s->prepared = 1;
}

/* Account for completed buffers: advance LVI past each one and refill
 * the entry that was just played with the next period of the ring.
 * chip->lock held.  Returns 1 when at least one period completed. */
static int stream_update(struct ich_chip *c, struct ich_stream *s)
{
	uint8_t sr = st_sr(c, s);
	uint32_t civ, step, i;
	int elapsed = 0;

	if (sr == 0xff)
		return 0; /* gone, or a bogus read */
	if (!(sr & ICH_SR_ACK))
		return 0;
	if (!s->prepared || s->periods == 0) {
		st_ack(c, s, sr & ICH_SR_ACK);
		return 0;
	}

	if (sr & ICH_SR_BCIS) {
		civ = bm_rd8(c, s->reg + ICH_OFF_CIV) & ICH_IDX_MASK;
		if (civ == s->civ)
			step = 1; /* completion seen before CIV moved */
		else
			step = (civ - s->civ) & ICH_IDX_MASK;
		s->civ = (s->civ + step) & ICH_IDX_MASK;
		for (i = 0; i < step; i++) {
			s->lvi = (s->lvi + 1) & ICH_IDX_MASK;
			s->lvi_period = (s->lvi_period + 1) % s->periods;
			bdl_set(s, s->lvi, s->lvi_period);
		}
		ich_mb();
		bm_wr8(c, s->reg + ICH_OFF_LVI, (uint8_t)s->lvi);
		s->periods_done += step;
		elapsed = 1;
	}
	if (sr & ICH_SR_FIFOE)
		s->fifo_errors++;
	st_ack(c, s, sr & ICH_SR_ACK);

	/* Fell a whole BDL behind: the engine halted at LVI.  The new LVI
	 * written above lets it continue (RPBM is still set).  Rewriting
	 * CR here would be wrong: on some implementations setting RPBM
	 * again skips to the prefetched entry. */
	if ((sr & ICH_SR_LVBCI) && s->running)
		s->lvi_stalls++;
	return elapsed;
}

/* ---- polling and interrupt health ----------------------------------------- */

static uint64_t period_ns(const struct ich_stream *s)
{
	uint64_t frames;

	if (!s->rate)
		return 10000000ull;
	frames = s->period_bytes / 4; /* S16 stereo */
	return frames * 1000000000ull / s->rate;
}

static void stream_set_poll(struct ich_chip *c, struct ich_stream *s)
{
	uint64_t p = period_ns(s);

	if (!c->irq_ok || s->poll_fast) {
		p /= 2; /* twice per period */
		if (p < 1000000ull)
			p = 1000000ull;
	} else {
		/* a slow watchdog that notices lost interrupts */
		p *= 4;
		if (p < 50000000ull)
			p = 50000000ull;
	}
	s->poll_ns = p;
}

/* Timer callback: interrupt context.  Picks up completions the interrupt
 * did not (no IRQ line, or a line that never fires). */
static void stream_timer(hrtimer_t *t)
{
	struct ich_stream *s = t->arg;
	struct ich_chip *c = s->chip;
	struct snd_pcm_substream *ss = 0;
	uint64_t fl;
	int el = 0, warn = 0;

	spin_lock_irqsave(&c->lock, &fl);
	if (!s->running || c->gone) {
		spin_unlock_irqrestore(&c->lock, fl);
		return;
	}
	s->polls++;
	el = stream_update(c, s);
	if (el) {
		s->poll_found++;
		ss = s->ss;
		/* The watchdog found completions the interrupt should have
		 * handled.  Once can be a race with a late interrupt; three
		 * times in a row with no interrupt in between means the line
		 * does not work. */
		if (s->irqs == s->irqs_seen)
			s->poll_hits++;
		else
			s->poll_hits = 0;
		if (c->irq_ok && !s->poll_fast && s->poll_hits >= 3) {
			s->poll_fast = 1;
			warn = 1;
			stream_set_poll(c, s);
		}
	} else if (s->irqs != s->irqs_seen) {
		s->poll_hits = 0;
	}
	s->irqs_seen = s->irqs;
	hrtimer_start(&s->timer, hrtimer_now_ns() + s->poll_ns);
	spin_unlock_irqrestore(&c->lock, fl);

	if (warn)
		kprintf("[snd] %s: interrupts not arriving for %s; polling\n",
			DRV,
			s->dir == AUDIO_DIR_PLAYBACK ? "playback" : "capture");
	if (el && ss)
		snd_pcm_period_elapsed(ss);
}

static void ich_disconnect_work(void *arg)
{
	struct ich_chip *c = arg;
	if (c->card)
		snd_card_disconnect(c->card);
}

static int ich_irq(void *arg)
{
	struct ich_chip *c = arg;
	uint32_t sta, pend;
	int dir;

	if (!c->irq_armed)
		return 0;
	sta = bm_rd32(c, ICH_GLOB_STA);
	if (sta == 0xffffffffu) {
		/* surprise removal or a powered-off function: not ours to
		 * claim, and nothing to touch */
		if (!c->gone && pci_cfg_read16(c->pci, 0x00) == 0xffff) {
			c->gone = 1;
			if (!c->gone_reported) {
				/* removing the nodes sleeps: not here */
				c->gone_reported = 1;
				snd_schedule_work(ich_disconnect_work, c, 0);
			}
		}
		return 0;
	}
	pend = sta & c->int_mask;
	if (!pend) {
		c->irq_foreign++;
		return 0;
	}
	c->irq_count++;

	for (dir = 0; dir < 2; dir++) {
		struct ich_stream *s = &c->st[dir];
		struct snd_pcm_substream *ss;
		int el;

		if (!s->present || !(pend & s->int_bit))
			continue;
		spin_lock(&c->lock);
		s->irqs++;
		el = stream_update(c, s);
		ss = (el && s->running) ? s->ss : 0;
		spin_unlock(&c->lock);
		if (ss)
			snd_pcm_period_elapsed(ss);
	}
	/* The per-engine bits mirror the engine status (cleared above);
	 * nForce latches them, so acknowledge exactly what was seen. */
	bm_wr32(c, ICH_GLOB_STA, pend);
	return 1;
}

/* Interrupt for the device's INTx pin: the ACPI routing table first (with
 * the bridge swizzle for a function behind a PCI bridge), the firmware's
 * line register after. */
static int ich_find_gsi(struct ich_chip *c)
{
	const pci_device_t *p = c->pci;
	uint8_t pin = p->interrupt_pin;
	uint32_t gsi = 0;

	if (pin >= 1 && pin <= 4) {
		uint8_t lpin = (uint8_t)(pin - 1);
		uint8_t ldev = p->device;
		if (p->bus != 0) {
			const pci_device_t *br = pci_find_bridge_for_bus(p->bus);
			if (br) {
				lpin = (uint8_t)((lpin + p->device) % 4);
				ldev = br->device;
			}
		}
		if (acpi_pci_lookup_irq("\\\\_SB_.PCI0", ldev, lpin, &gsi) == 0 &&
		    gsi <= 23)
			return (int)gsi;
	}
	if (p->interrupt_line != 0xff && p->interrupt_line <= 23 &&
	    p->interrupt_line != 0)
		return p->interrupt_line;
	return -1;
}

static void ich_setup_irq(struct ich_chip *c)
{
	int gsi = ich_find_gsi(c);
	int rc;

	c->gsi = -1;
	c->irq_ok = 0;
	if (gsi < 0) {
		snd_info(DRV, "no interrupt line; using timer polling\n");
		return;
	}
	/* level triggered, active low, shared: the handler checks its own
	 * status before claiming */
	rc = irq_request_gsi(gsi, ich_irq, c, "ac97", 1, 1);
	if (rc) {
		snd_info(DRV, "cannot get GSI %d (%d); using timer polling\n",
			 gsi, rc);
		return;
	}
	c->gsi = gsi;
	c->irq_ok = 1;
}

static void ich_free_irq(struct ich_chip *c)
{
	if (c->irq_ok && c->gsi >= 0) {
		c->irq_armed = 0;
		irq_free(32 + c->gsi, ich_irq, c);
	}
	c->irq_ok = 0;
	c->gsi = -1;
}

/* ---- controller init ------------------------------------------------------ */

static void ich_stop_all_engines(struct ich_chip *c)
{
	unsigned int i;

	for (i = 0; i < c->n_engines; i++)
		bm_wr8(c, c->engine_regs[i] + ICH_OFF_CR, 0);
	for (i = 0; i < c->n_engines; i++) {
		uint32_t sr = c->type == ICH_TYPE_SIS ? ICH_OFF_PICB : ICH_OFF_SR;
		engine_reset(c, c->engine_regs[i], sr);
	}
}

static uint32_t wait_codec_ready(struct ich_chip *c, uint32_t ms)
{
	uint32_t waited = 0, sta;

	for (;;) {
		sta = bm_rd32(c, ICH_GLOB_STA);
		if (sta == 0xffffffffu)
			return 0;
		if (sta & c->codec_isr_bits)
			return sta & c->codec_isr_bits;
		if (waited >= ms)
			return 0;
		snd_mdelay(1);
		waited++;
	}
}

static int ich_warm_reset(struct ich_chip *c)
{
	uint32_t cnt = bm_rd32(c, ICH_GLOB_CNT);
	int i;

	cnt &= ~(ICH_GC_ACLINK_OFF | ICH_GC_GIE | ICH_GC_PRIE | ICH_GC_SRIE |
		 ICH_GC_TRIE);
	/* finish a pending cold reset, or pulse a warm one */
	if (!(cnt & ICH_GC_COLD))
		cnt |= ICH_GC_COLD;
	else
		cnt |= ICH_GC_WARM;
	bm_wr32(c, ICH_GLOB_CNT, cnt);
	for (i = 0; i < 250; i++) {
		uint32_t v = bm_rd32(c, ICH_GLOB_CNT);
		if (v == 0xffffffffu)
			return -ENODEV;
		if (!(v & ICH_GC_WARM))
			return 0;
		snd_mdelay(1);
	}
	snd_info(DRV, "warm reset still in progress (GLOB_CNT 0x%08x)\n",
		 bm_rd32(c, ICH_GLOB_CNT));
	return -EIO;
}

static void ich_cold_reset(struct ich_chip *c)
{
	uint32_t cnt = bm_rd32(c, ICH_GLOB_CNT);

	cnt &= ~(ICH_GC_ACLINK_OFF | ICH_GC_GIE | ICH_GC_PRIE | ICH_GC_SRIE |
		 ICH_GC_TRIE | ICH_GC_WARM);
	/* assert AC_RESET# for well over the 1 us minimum, release it */
	bm_wr32(c, ICH_GLOB_CNT, cnt & ~ICH_GC_COLD);
	snd_udelay(20);
	bm_wr32(c, ICH_GLOB_CNT, cnt | ICH_GC_COLD);
	snd_mdelay(1);
}

static int ich_chip_init(struct ich_chip *c)
{
	uint32_t sta, ready, more_ms;

	ich_stop_all_engines(c);

	/* clear latched status (write-1-to-clear bits only) */
	sta = bm_rd32(c, ICH_GLOB_STA);
	if (sta == 0xffffffffu)
		return -ENODEV;
	bm_wr32(c, ICH_GLOB_STA,
		sta & (ICH_GS_RCS | ICH_GS_PRI | ICH_GS_SRI | ICH_GS_TRI |
		       ICH_GS_GSCI | ICH_GS_MCINT | ICH_GS_POINT |
		       ICH_GS_PIINT |
		       (c->type == ICH_TYPE_NFORCE ? ICH_GS_NVSPINT : 0) |
		       (c->type == ICH_TYPE_SIS ? ICH_GS_SIS_TRI : 0)));

	if (no_cold_reset(c)) {
		if (ich_warm_reset(c) == -ENODEV)
			return -ENODEV;
	} else {
		ich_cold_reset(c);
	}

	ready = wait_codec_ready(c, c->in_vm ? 200 : 1000);
	if (!ready) {
		/* a link that came up without the codec: one warm reset */
		ich_warm_reset(c);
		ready = wait_codec_ready(c, c->in_vm ? 100 : 500);
	}
	if (ready) {
		/* the other codecs answer within a short while of the first */
		more_ms = c->in_vm ? 0 : 50;
		while (more_ms-- && ready != c->codec_isr_bits) {
			snd_mdelay(1);
			sta = bm_rd32(c, ICH_GLOB_STA);
			if (sta == 0xffffffffu)
				break;
			ready |= sta & c->codec_isr_bits;
		}
	}
	c->ready_seen = ready;

	/* two channels, 16-bit samples */
	{
		uint32_t cnt = bm_rd32(c, ICH_GLOB_CNT);
		if (c->type == ICH_TYPE_SIS)
			cnt &= ~ICH_GC_SIS_PCM_246_MASK;
		else
			cnt &= ~(ICH_GC_PCM_246_MASK | ICH_GC_PCM_20BIT);
		bm_wr32(c, ICH_GLOB_CNT, cnt);
	}

	if (c->type == ICH_TYPE_SIS) {
		/* the 7012 starts with its output muted */
		if (win_ok(c->bm_size, ICH_SIS_OUT_CTRL, 2))
			bm_wr16(c, ICH_SIS_OUT_CTRL,
				bm_rd16(c, ICH_SIS_OUT_CTRL) | 0x0001);
		else
			snd_info(DRV, "SiS output control outside the window\n");
	}
	return ready ? 0 : -EIO;
}

/* Which SDIN line answered the last codec read (ICH4+). */
static void ich_map_sdin(struct ich_chip *c, unsigned int ncodecs)
{
	unsigned int i;

	unsigned int k = 0;

	for (i = 0; i < AC97_MAX_CODECS; i++)
		c->sdin[i] = i < 3 ? i : 0;
	if (c->type != ICH_TYPE_ICH4 || !win_ok(c->bm_size, ICH_SDM, 1)) {
		/* Without the SDM register the n-th codec that came up is
		 * the one on the n-th ready line: a board with only the
		 * secondary line populated still addresses its codec as
		 * number 0. */
		if (c->ready_seen) {
			for (i = 0; i < c->max_codecs && k < AC97_MAX_CODECS;
			     i++)
				if (c->ready_seen & c->codec_bits[i])
					c->sdin[k++] = i;
		}
		c->sdin_known = 1;
		return;
	}
	for (i = 0; i < ncodecs && i < 3; i++) {
		uint16_t v;
		ich_codec_read(c, i, AC97_RESET, &v);
		c->sdin[i] = bm_rd8(c, ICH_SDM) & ICH_SDM_LDI_MASK;
		if (c->sdin[i] >= 3)
			c->sdin[i] = 0;
	}
	c->sdin_known = 1;
}

static int ich_attach_codec(struct ich_chip *c)
{
	uint32_t flags = c->in_vm ? AC97_CTRL_IN_VM : 0;
	unsigned int ncodecs = 0, i, num;
	int rc = -ENODEV;

	for (i = 0; i < c->max_codecs; i++)
		if (c->ready_seen & c->codec_bits[i])
			ncodecs++;
	if (ncodecs == 0)
		ncodecs = 1; /* no ready bits: try the primary anyway */

	/* During the SDIN map, any ready bit will do. */
	c->sdin_known = 0;
	ich_map_sdin(c, ncodecs);

	for (num = 0; num < ncodecs; num++) {
		rc = ac97_codec_probe(&c->codec, &ich_bus_ops, c, num, c->ssvid,
				      c->ssdid, flags);
		if (rc == 0)
			break;
		snd_info(DRV, "codec %u: %s\n", num,
			 rc == -ENODEV ? "not an audio codec" : "no answer");
	}
	if (rc)
		return rc;
	return ac97_codec_init(&c->codec);
}

/* Measure the codec's real sample clock: play 50 ms of silence at a
 * nominal 48 kHz and see how far the engine got.  Bare metal only (an
 * emulated codec runs at whatever pace its backend has). */
#define ICH_MEASURE_BYTES 32768u
#define ICH_MEASURE_PERIOD 8192u

static void ich_measure_clock(struct ich_chip *c)
{
	struct ich_stream *s = &c->st[AUDIO_DIR_PLAYBACK];
	uint32_t quirk = clock_quirk(c);
	void *buf;
	uint64_t phys, t0, t1, pos = 0, rate;
	uint32_t civ = 0, picb = 0, i;
	uint64_t fl;
	int attempt;

	c->clock = 48000;
	if (quirk) {
		c->clock = quirk;
		ac97_codec_set_clock(&c->codec, quirk);
		snd_info(DRV, "board %04x:%04x: codec clock %u Hz\n", c->ssvid,
			 c->ssdid, quirk);
		return;
	}
	if (c->in_vm || !c->codec.vra || !s->present)
		return;

	buf = snd_dma_alloc(ICH_MEASURE_BYTES, 1, 0, &phys);
	if (!buf)
		return;
	if (phys + ICH_MEASURE_BYTES > 0x100000000ull) {
		snd_dma_free(buf, ICH_MEASURE_BYTES);
		return;
	}
	if (ac97_codec_set_rate(&c->codec, AUDIO_DIR_PLAYBACK, 48000)) {
		snd_dma_free(buf, ICH_MEASURE_BYTES);
		return;
	}

	for (attempt = 0; attempt < 3; attempt++) {
		spin_lock_irqsave(&c->lock, &fl);
		stream_engine_reset(c, s);
		s->buf_phys = (uint32_t)phys;
		s->buffer_bytes = ICH_MEASURE_BYTES;
		s->period_bytes = ICH_MEASURE_PERIOD;
		s->periods = ICH_MEASURE_BYTES / ICH_MEASURE_PERIOD;
		stream_load(c, s);
		/* no IOCE: the interrupt is not wanted here */
		bm_wr8(c, s->reg + ICH_OFF_CR, ICH_CR_RPBM);
		t0 = hrtimer_now_ns();
		spin_unlock_irqrestore(&c->lock, fl);

		snd_mdelay(50);

		spin_lock_irqsave(&c->lock, &fl);
		for (i = 0; i < 100; i++) {
			civ = bm_rd8(c, s->reg + ICH_OFF_CIV) & ICH_IDX_MASK;
			picb = bm_rd16(c, s->reg + s->picb_off);
			if (picb != 0 &&
			    civ == (bm_rd8(c, s->reg + ICH_OFF_CIV) &
				    ICH_IDX_MASK))
				break;
			snd_udelay(10);
		}
		t1 = hrtimer_now_ns();
		stream_engine_reset(c, s);
		spin_unlock_irqrestore(&c->lock, fl);

		if (picb == 0 || picb == 0xffff || t1 <= t0)
			continue;
		pos = (uint64_t)civ * ICH_MEASURE_PERIOD + ICH_MEASURE_PERIOD -
		      ((uint64_t)picb << s->pos_shift);
		break;
	}
	s->configured = 0;
	s->buf_phys = 0;
	s->periods = 0;
	snd_dma_free(buf, ICH_MEASURE_BYTES);

	if (pos == 0 || t1 <= t0) {
		snd_info(DRV, "clock measurement failed; assuming 48000 Hz\n");
		return;
	}
	/* frames per second actually played for a nominal 48000 */
	rate = (pos / 4) * 1000000000ull / (t1 - t0);
	if (rate < 40000 || rate >= 60000) {
		snd_info(DRV, "measured codec rate %u Hz rejected\n",
			 (uint32_t)rate);
		return;
	}
	if (rate >= 47500 && rate <= 48500) {
		snd_info(DRV, "codec clock 48000 Hz (measured %u)\n",
			 (uint32_t)rate);
		return;
	}
	/* register value = wanted rate * clock / 48000 */
	c->clock = (uint32_t)(48000ull * 48000ull / rate);
	ac97_codec_set_clock(&c->codec, c->clock);
	snd_info(DRV, "codec runs at %u Hz for 48000: clock set to %u\n",
		 (uint32_t)rate, c->clock);
}

/* ---- PCM operations ------------------------------------------------------- */

static int ich_pcm_open(struct snd_pcm_substream *ss)
{
	struct ich_stream *s = ss_stream(ss);
	struct ich_chip *c;
	uint64_t fl;

	if (!s)
		return -EINVAL;
	c = s->chip;
	if (c->gone)
		return -ENODEV;
	if (!s->present)
		return -ENODEV;
	spin_lock_irqsave(&c->lock, &fl);
	s->ss = ss;
	s->configured = 0;
	s->prepared = 0;
	s->poll_fast = 0;
	s->poll_hits = 0;
	spin_unlock_irqrestore(&c->lock, fl);
	ss->private_data = s;
	return 0;
}

static void stream_quiesce(struct ich_chip *c, struct ich_stream *s)
{
	uint64_t fl;

	spin_lock_irqsave(&c->lock, &fl);
	if (!c->gone)
		stream_engine_reset(c, s);
	s->running = 0;
	s->paused = 0;
	spin_unlock_irqrestore(&c->lock, fl);
	/* With `running' clear under the lock the callback no longer
	 * re-arms; cancel what may still be queued. */
	hrtimer_cancel(&s->timer);
}

static void ich_pcm_close(struct snd_pcm_substream *ss)
{
	struct ich_stream *s = ss_stream(ss);
	struct ich_chip *c;
	uint64_t fl;

	if (!s)
		return;
	c = s->chip;
	stream_quiesce(c, s);
	spin_lock_irqsave(&c->lock, &fl);
	s->ss = 0;
	s->configured = 0;
	s->buf_phys = 0;
	s->periods = 0;
	spin_unlock_irqrestore(&c->lock, fl);
}

static int ich_pcm_hw_params(struct snd_pcm_substream *ss)
{
	struct ich_stream *s = ss_stream(ss);
	struct ich_chip *c;
	const struct audio_params *p;
	uint32_t pmax;
	int rc;
	uint64_t fl;

	if (!s)
		return -EINVAL;
	c = s->chip;
	if (c->gone)
		return -ENODEV;
	p = &ss->params;

	/* What the engine can do; the core negotiated within the hw
	 * description, this re-checks before anything is programmed. */
	if (p->format != AUDIO_FMT_S16_LE || p->channels != 2)
		return -EINVAL;
	if (!(ac97_codec_rates(&c->codec, s->dir) & audio_rate_bit(p->rate)))
		return -EINVAL;
	pmax = c->type == ICH_TYPE_SIS ? 32768u : 65536u;
	if (ss->period_bytes < 64 || ss->period_bytes > pmax ||
	    (ss->period_bytes & 3) != 0)
		return -EINVAL;
	if (ss->periods < 2 || ss->periods > 255)
		return -EINVAL;
	if ((uint64_t)ss->period_bytes * ss->periods != ss->buffer_bytes)
		return -EINVAL;
	if (ss->dma_phys == 0 || (ss->dma_phys & 7) ||
	    ss->dma_phys + ss->buffer_bytes > 0x100000000ull)
		return -EINVAL;
	/* the sample count field is 16 bits */
	if ((ss->period_bytes >> s->pos_shift) > 0xfffe)
		return -EINVAL;

	stream_quiesce(c, s);

	rc = ac97_codec_set_rate(&c->codec, s->dir, p->rate);
	if (rc)
		return -EINVAL;
	if (s->dir == AUDIO_DIR_CAPTURE)
		ac97_codec_capture_power(&c->codec, 1);

	spin_lock_irqsave(&c->lock, &fl);
	if (s->dir == AUDIO_DIR_PLAYBACK) {
		uint32_t cnt = bm_rd32(c, ICH_GLOB_CNT);
		if (cnt != 0xffffffffu) {
			if (c->type == ICH_TYPE_SIS)
				cnt &= ~ICH_GC_SIS_PCM_246_MASK;
			else
				cnt &= ~(ICH_GC_PCM_246_MASK |
					 ICH_GC_PCM_20BIT);
			bm_wr32(c, ICH_GLOB_CNT, cnt);
		}
	}
	s->buf_phys = (uint32_t)ss->dma_phys;
	s->buffer_bytes = ss->buffer_bytes;
	s->period_bytes = ss->period_bytes;
	s->periods = ss->periods;
	s->rate = p->rate;
	s->configured = 1;
	s->prepared = 0;
	s->poll_hits = 0;
	stream_set_poll(c, s);
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

static int ich_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct ich_stream *s = ss_stream(ss);
	struct ich_chip *c;
	uint64_t fl;

	if (!s)
		return -EINVAL;
	c = s->chip;
	stream_quiesce(c, s);
	spin_lock_irqsave(&c->lock, &fl);
	/* the engine must never again fetch from the ring being freed */
	if (!c->gone)
		bm_wr32(c, s->reg + ICH_OFF_BDBAR, 0);
	s->configured = 0;
	s->buf_phys = 0;
	s->periods = 0;
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

static int ich_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct ich_stream *s = ss_stream(ss);
	struct ich_chip *c;
	uint64_t fl;

	if (!s)
		return -EINVAL;
	c = s->chip;
	if (c->gone)
		return -ENODEV;
	if (!s->configured)
		return -EINVAL;
	hrtimer_cancel(&s->timer);
	spin_lock_irqsave(&c->lock, &fl);
	stream_engine_reset(c, s);
	stream_load(c, s);
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

static void stream_start_timer(struct ich_chip *c, struct ich_stream *s)
{
	(void)c;
	hrtimer_start(&s->timer, hrtimer_now_ns() + s->poll_ns);
}

/* Lock (ss->lock) held by the core, interrupts off: no sleeping, only
 * bounded register waits. */
static int ich_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct ich_stream *s = ss_stream(ss);
	struct ich_chip *c;
	int i, rc = 0;

	if (!s)
		return -EINVAL;
	c = s->chip;
	if (c->gone)
		return -ENODEV;

	spin_lock(&c->lock);
	switch (cmd) {
	case SND_TRIGGER_START:
		if (!s->configured) {
			rc = -EINVAL;
			break;
		}
		if (!s->prepared) {
			engine_reset(c, s->reg, s->sr_off);
			stream_load(c, s);
		}
		st_ack(c, s, ICH_SR_ACK);
		bm_wr8(c, s->reg + ICH_OFF_CR, ICH_CR_IOCE | ICH_CR_RPBM);
		s->running = 1;
		s->paused = 0;
		s->poll_hits = 0;
		stream_start_timer(c, s);
		break;
	case SND_TRIGGER_RESUME:
		if (!s->configured || !s->prepared) {
			rc = -EINVAL;
			break;
		}
		bm_wr8(c, s->reg + ICH_OFF_CR, ICH_CR_IOCE | ICH_CR_RPBM);
		s->running = 1;
		s->paused = 0;
		stream_start_timer(c, s);
		break;
	case SND_TRIGGER_PAUSE:
		/* clearing RPBM halts the engine where it is; setting it
		 * again continues from the same sample */
		bm_wr8(c, s->reg + ICH_OFF_CR, ICH_CR_IOCE);
		for (i = 0; i < 200; i++) {
			uint8_t sr = st_sr(c, s);
			if (sr == 0xff || (sr & ICH_SR_DCH))
				break;
			snd_udelay(5);
		}
		s->running = 0;
		s->paused = 1;
		break;
	case SND_TRIGGER_STOP:
		engine_reset(c, s->reg, s->sr_off);
		s->running = 0;
		s->paused = 0;
		s->prepared = 0;
		break;
	default:
		rc = -EINVAL;
		break;
	}
	spin_unlock(&c->lock);
	return rc;
}

/* DMA position in bytes within the ring.  CIV and PICB cannot be read
 * atomically: read CIV around PICB and retry while it moves.  A PICB of
 * zero is ambiguous (an entry finished, the next not yet loaded, or the
 * engine not started); the last good position is kept then. */
static uint32_t ich_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct ich_stream *s = ss_stream(ss);
	struct ich_chip *c;
	uint32_t civ = 0, civ2, picb = 0, period, done, pos;
	int tries, stable = 0;

	if (!s)
		return 0;
	c = s->chip;
	spin_lock(&c->lock);
	if (!s->configured || !s->prepared || s->periods == 0 || c->gone ||
	    (!s->running && !s->paused)) {
		pos = s->last_pos;
		goto out;
	}
	for (tries = 0; tries < 10; tries++) {
		uint8_t b = bm_rd8(c, s->reg + ICH_OFF_CIV);
		if (b == 0xff)
			break;
		civ = b & ICH_IDX_MASK;
		picb = bm_rd16(c, s->reg + s->picb_off);
		if (picb == 0xffff)
			break;
		if (picb == 0) {
			snd_udelay(10);
			continue;
		}
		civ2 = bm_rd8(c, s->reg + ICH_OFF_CIV) & ICH_IDX_MASK;
		if (civ2 != civ)
			continue;
		/* an emulated register file does not change between two
		 * reads in the same instant; real hardware may */
		if (c->in_vm || picb == bm_rd16(c, s->reg + s->picb_off)) {
			stable = 1;
			break;
		}
	}
	if (!stable) {
		pos = s->last_pos;
		goto out;
	}
	period = s->bdl_period[civ];
	if (period >= s->periods)
		period = 0;
	done = picb << s->pos_shift;
	if (done > s->period_bytes)
		done = s->period_bytes;
	done = s->period_bytes - done;
	/* PICB reloads to the full length a moment before CIV advances:
	 * never let the position step back inside the same period */
	if (period == s->last_period && done < s->last_done &&
	    s->last_done - done < s->period_bytes / 2)
		done = s->last_done;
	pos = period * s->period_bytes + done;
	if (pos >= s->buffer_bytes)
		pos -= s->buffer_bytes;
	s->last_period = period;
	s->last_done = done;
	s->last_pos = pos;
out:
	spin_unlock(&c->lock);
	if (pos >= ss->buffer_bytes)
		pos = 0;
	return pos;
}

static const struct snd_pcm_ops ich_pcm_ops = {
	.open = ich_pcm_open,
	.close = ich_pcm_close,
	.hw_params = ich_pcm_hw_params,
	.hw_free = ich_pcm_hw_free,
	.prepare = ich_pcm_prepare,
	.trigger = ich_pcm_trigger,
	.pointer = ich_pcm_pointer,
	.delay = 0,
};

/* ---- controls ------------------------------------------------------------- */

static struct ich_chip *card_chip(struct snd_card *card)
{
	return card ? (struct ich_chip *)card->private_data : 0;
}

static int ich_get_volume(struct snd_card *card, uint32_t ctl,
			  struct audio_volume *v)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_get_volume(&c->codec, ctl, v);
}

static int ich_set_volume(struct snd_card *card, uint32_t ctl, uint32_t left,
			  uint32_t right)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_set_volume(&c->codec, ctl, left, right);
}

static int ich_get_mute(struct snd_card *card, uint32_t ctl, uint32_t *mute)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_get_mute(&c->codec, ctl, mute);
}

static int ich_set_mute(struct snd_card *card, uint32_t ctl, uint32_t mute)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_set_mute(&c->codec, ctl, mute);
}

static int ich_get_outputs(struct snd_card *card, struct audio_outputs *o)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_get_outputs(&c->codec, o);
}

static int ich_set_output(struct snd_card *card,
			  const struct audio_setoutput *s)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_set_output(&c->codec, s);
}

static int ich_get_inputs(struct snd_card *card, struct audio_outputs *o)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_get_inputs(&c->codec, o);
}

static int ich_set_input(struct snd_card *card,
			 const struct audio_setoutput *s)
{
	struct ich_chip *c = card_chip(card);
	if (!c || c->gone)
		return -ENODEV;
	return ac97_codec_set_input(&c->codec, s);
}

static const char *type_name(int type)
{
	switch (type) {
	case ICH_TYPE_ICH4:
		return "ICH4+";
	case ICH_TYPE_SIS:
		return "SiS";
	case ICH_TYPE_NFORCE:
		return "nForce";
	default:
		return "ICH";
	}
}

static void diag_controller(struct ich_chip *c, struct ich_dbuf *d)
{
	uint32_t cnt = bm_rd32(c, ICH_GLOB_CNT);
	uint32_t sta = bm_rd32(c, ICH_GLOB_STA);

	dbuf_printf(d, "%s (%04x:%04x, %s register model)\n", c->idinfo->name,
		    c->pci->vendor_id, c->pci->device_id, type_name(c->type));
	dbuf_printf(d, "  pci %02x:%02x.%u  subsystem %04x:%04x%s\n",
		    c->pci->bus, c->pci->device, c->pci->function, c->ssvid,
		    c->ssdid, c->in_vm ? "  (virtual machine)" : "");
	dbuf_printf(d, "  mixer %s 0x%llx (%u bytes), bus master %s 0x%llx "
		       "(%u bytes)\n",
		    c->mix_mmio ? "mmio" : "io", c->mix_base, c->mix_size,
		    c->bm_mmio ? "mmio" : "io", c->bm_base, c->bm_size);
	if (c->irq_ok)
		dbuf_printf(d, "  interrupt GSI %d: %llu handled, %llu "
			       "foreign\n",
			    c->gsi, c->irq_count, c->irq_foreign);
	else
		dbuf_printf(d, "  no interrupt: timer polling\n");
	dbuf_printf(d, "  GLOB_CNT 0x%08x  GLOB_STA 0x%08x  codecs ready:%s%s%s"
		       "\n",
		    cnt, sta, (sta & c->codec_bits[0]) ? " primary" : "",
		    (sta & c->codec_bits[1]) ? " secondary" : "",
		    (c->max_codecs > 2 && (sta & c->codec_bits[2]))
			    ? " tertiary"
			    : "");
	if (c->type == ICH_TYPE_ICH4 && win_ok(c->bm_size, ICH_SDM, 1))
		dbuf_printf(d, "  SDM 0x%02x  codec SDIN map %u %u %u\n",
			    bm_rd8(c, ICH_SDM), c->sdin[0], c->sdin[1],
			    c->sdin[2]);
	dbuf_printf(d, "  link clock %u Hz\n", c->clock);
}

static void diag_streams(struct ich_chip *c, struct ich_dbuf *d)
{
	int dir;

	for (dir = 0; dir < 2; dir++) {
		struct ich_stream *s = &c->st[dir];
		if (!s->present)
			continue;
		dbuf_printf(d, "%s engine at 0x%02x: %s%s%s\n",
			    dir == AUDIO_DIR_PLAYBACK ? "PCM out" : "PCM in",
			    s->reg, s->configured ? "configured" : "idle",
			    s->running ? ", running" : "",
			    s->paused ? ", paused" : "");
		dbuf_printf(d, "  BDBAR 0x%08x CIV %u LVI %u PIV %u SR 0x%02x "
			       "PICB %u CR 0x%02x\n",
			    bm_rd32(c, s->reg + ICH_OFF_BDBAR),
			    bm_rd8(c, s->reg + ICH_OFF_CIV) & ICH_IDX_MASK,
			    bm_rd8(c, s->reg + ICH_OFF_LVI) & ICH_IDX_MASK,
			    bm_rd8(c, s->reg + ICH_OFF_PIV) & ICH_IDX_MASK,
			    st_sr(c, s), bm_rd16(c, s->reg + s->picb_off),
			    bm_rd8(c, s->reg + ICH_OFF_CR));
		if (s->configured)
			dbuf_printf(d, "  %u Hz, ring %u bytes = %u x %u, "
				       "position %u\n",
				    s->rate, s->buffer_bytes, s->periods,
				    s->period_bytes, s->last_pos);
		dbuf_printf(d, "  periods %llu, interrupts %llu, polls %llu "
			       "(%llu found work%s), FIFO errors %u, LVI stalls "
			       "%u\n",
			    s->periods_done, s->irqs, s->polls, s->poll_found,
			    s->poll_fast ? ", fast polling" : "",
			    s->fifo_errors, s->lvi_stalls);
	}
}

static long ich_diag(struct snd_card *card, uint32_t what, char *buf,
		     size_t len)
{
	struct ich_chip *c = card_chip(card);
	struct ich_dbuf d = { buf, len, 0 };

	if (!c || !buf || len == 0)
		return 0;
	buf[0] = 0;
	switch (what) {
	case AUDIO_DIAG_CODECS:
		diag_controller(c, &d);
		d.pos += (size_t)ac97_codec_diag(&c->codec, 1, buf + d.pos,
						 len - d.pos);
		break;
	case AUDIO_DIAG_STREAMS:
		diag_streams(c, &d);
		break;
	case AUDIO_DIAG_SUMMARY:
	default:
		diag_controller(c, &d);
		d.pos += (size_t)ac97_codec_diag(&c->codec, 0, buf + d.pos,
						 len - d.pos);
		if (d.pos < len)
			diag_streams(c, &d);
		break;
	}
	if (d.pos >= len)
		d.pos = len - 1;
	buf[d.pos] = 0;
	return (long)d.pos;
}

static const struct snd_ctl_ops ich_ctl_ops = {
	.get_volume = ich_get_volume,
	.set_volume = ich_set_volume,
	.get_mute = ich_get_mute,
	.set_mute = ich_set_mute,
	.get_outputs = ich_get_outputs,
	.set_output = ich_set_output,
	.get_inputs = ich_get_inputs,
	.set_input = ich_set_input,
	.diag = ich_diag,
};

/* ---- probe ---------------------------------------------------------------- */

static void ich_setup_streams(struct ich_chip *c)
{
	uint32_t sr_off = c->type == ICH_TYPE_SIS ? ICH_OFF_PICB : ICH_OFF_SR;
	uint32_t picb_off = c->type == ICH_TYPE_SIS ? ICH_OFF_SR : ICH_OFF_PICB;
	/* the 7012 counts bytes, everything else 16-bit samples */
	uint32_t shift = c->type == ICH_TYPE_SIS ? 0 : 1;
	int dir;

	c->n_engines = 0;
	c->engine_regs[c->n_engines++] = ICH_PI;
	c->engine_regs[c->n_engines++] = ICH_PO;
	c->engine_regs[c->n_engines++] = ICH_MC;
	if (c->type == ICH_TYPE_ICH4 && c->bm_size >= 0x70) {
		c->engine_regs[c->n_engines++] = ICH_MC2;
		c->engine_regs[c->n_engines++] = ICH_PI2;
		c->engine_regs[c->n_engines++] = ICH_SP;
	}
	if (c->type == ICH_TYPE_NFORCE && c->bm_size >= 0x80)
		c->engine_regs[c->n_engines++] = ICH_NV_SP;

	for (dir = 0; dir < 2; dir++) {
		struct ich_stream *s = &c->st[dir];
		kmemset(s, 0, sizeof(*s));
		s->chip = c;
		s->dir = dir;
		s->reg = dir == AUDIO_DIR_PLAYBACK ? ICH_PO : ICH_PI;
		s->int_bit = dir == AUDIO_DIR_PLAYBACK ? ICH_GS_POINT
						       : ICH_GS_PIINT;
		s->sr_off = sr_off;
		s->picb_off = picb_off;
		s->pos_shift = shift;
		/* BDLs at 256-byte offsets inside one page (8-byte
		 * alignment is all the engine needs) */
		s->bdl = (volatile uint32_t *)((uint8_t *)c->bdl_area +
					       (uint32_t)dir * 0x100);
		s->bdl_phys = (uint32_t)(c->bdl_phys + (uint32_t)dir * 0x100);
		s->present = win_ok(c->bm_size, s->reg + ICH_OFF_CR, 1);
		hrtimer_init(&s->timer, stream_timer, s);
		s->poll_ns = 10000000ull;
	}
	c->int_mask = ICH_GS_POINT | ICH_GS_PIINT;
}

static void fill_hw(struct ich_chip *c, int dir, struct snd_pcm_hw *hw)
{
	kmemset(hw, 0, sizeof(*hw));
	if (!c->st[dir].present)
		return;
	hw->formats = AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
	hw->rates = ac97_codec_rates(&c->codec, dir);
	hw->channels_min = 2;
	hw->channels_max = 2;
	hw->period_bytes_min = 256;
	hw->period_bytes_max = c->type == ICH_TYPE_SIS ? 32768 : 65536;
	hw->periods_min = 2;
	hw->periods_max = ICH_BDL_ENTRIES;
	hw->buffer_bytes_max = 512 * 1024;
	hw->buffer_align = 32;
	hw->period_align = 32;
	hw->dma32 = 1; /* 32-bit BDL addresses */
	hw->dma_coherent = 0;
}

static void ich_teardown(struct ich_chip *c)
{
	int dir;

	c->irq_armed = 0;
	for (dir = 0; dir < 2; dir++)
		if (c->st[dir].bdl)
			hrtimer_cancel(&c->st[dir].timer);
	if (c->bm_size)
		ich_stop_all_engines(c);
	ich_free_irq(c);
	if (c->codec.ready)
		ac97_codec_shutdown(&c->codec);
	if (c->bdl_area)
		snd_dma_free(c->bdl_area, 4096);
	c->bdl_area = 0;
	unmap_windows(c);
	if (c->pci)
		pci_cfg_write16(c->pci, 0x04, c->pci_cmd_saved);
}

static int ich_probe_one(const pci_device_t *pci, const struct ich_id *id)
{
	struct ich_chip *c;
	struct snd_pcm_hw play, cap;
	struct snd_card *card = 0;
	int rc;

	c = kalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	kmemset(c, 0, sizeof(*c));
	spinlock_init(&c->lock, "ac97-ich");
	c->pci = pci;
	c->idinfo = id;
	c->type = id->type;
	c->gsi = -1;
	c->clock = 48000;
	c->ssvid = pci_cfg_read16(pci, 0x2c);
	c->ssdid = pci_cfg_read16(pci, 0x2e);
	c->in_vm = detect_vm(c);
	c->pci_cmd_saved = pci_cfg_read16(pci, 0x04);

	switch (c->type) {
	case ICH_TYPE_ICH4:
		c->max_codecs = 3;
		c->codec_bits[0] = ICH_GS_PCR;
		c->codec_bits[1] = ICH_GS_SCR;
		c->codec_bits[2] = ICH_GS_TCR;
		break;
	case ICH_TYPE_SIS:
		c->max_codecs = 3;
		c->codec_bits[0] = ICH_GS_PCR;
		c->codec_bits[1] = ICH_GS_SCR;
		c->codec_bits[2] = ICH_GS_SIS_TCR;
		break;
	default:
		c->max_codecs = 2;
		c->codec_bits[0] = ICH_GS_PCR;
		c->codec_bits[1] = ICH_GS_SCR;
		c->codec_bits[2] = 0;
		break;
	}
	c->codec_isr_bits = c->codec_bits[0] | c->codec_bits[1] |
			    c->codec_bits[2];

	rc = ich_map(c);
	if (rc) {
		snd_info(DRV, "%s at %02x:%02x.%u: register windows not "
			      "usable (%d)\n",
			 id->name, pci->bus, pci->device, pci->function, rc);
		kfree(c);
		return rc;
	}
	/* three codecs need 0x180 bytes of mixer window */
	while (c->max_codecs > 1 &&
	       !win_ok(c->mix_size, (c->max_codecs - 1) * 0x80 + 0x7e, 2))
		c->max_codecs--;
	ich_enable_pci(c);

	c->bdl_area = snd_dma_alloc(4096, 1, 0, &c->bdl_phys);
	if (!c->bdl_area || c->bdl_phys + 4096 > 0x100000000ull) {
		snd_info(DRV, "no DMA memory for the descriptor lists\n");
		rc = -ENOMEM;
		goto fail;
	}
	kmemset(c->bdl_area, 0, 4096);
	ich_setup_streams(c);

	rc = ich_chip_init(c);
	if (rc == -ENODEV) {
		snd_info(DRV, "%s: controller does not respond\n", id->name);
		goto fail;
	}
	if (rc)
		snd_info(DRV, "%s: no codec reported ready (GLOB_STA 0x%08x)"
			      "; probing anyway\n",
			 id->name, bm_rd32(c, ICH_GLOB_STA));

	rc = ich_attach_codec(c);
	if (rc) {
		snd_info(DRV, "%s: no usable audio codec (%d)\n", id->name, rc);
		goto fail;
	}

	ich_measure_clock(c);
	ich_setup_irq(c);

	rc = snd_card_new("ac97", pci, &card);
	if (rc || !card) {
		if (!rc)
			rc = -ENOMEM;
		goto fail;
	}
	c->card = card;
	kstrncpy(card->name, id->name, sizeof(card->name) - 1);
	card->name[sizeof(card->name) - 1] = 0;
	kstrncpy(card->codec, c->codec.name, sizeof(card->codec) - 1);
	card->codec[sizeof(card->codec) - 1] = 0;
	ksnprintf(card->bus, sizeof(card->bus), "pci:0000:%02x:%02x.%u",
		  pci->bus, pci->device, pci->function);
	card->ctl = &ich_ctl_ops;
	card->private_data = c;
	card->info_flags |= AUDIO_INFO_HW_VOLUME;
	card->num_outputs = (uint32_t)c->codec.num_outputs;
	card->num_inputs = AC97_REC_NSOURCES;

	fill_hw(c, AUDIO_DIR_PLAYBACK, &play);
	fill_hw(c, AUDIO_DIR_CAPTURE, &cap);
	rc = snd_pcm_new(card, "AC'97 PCM", &ich_pcm_ops,
			 play.formats ? &play : 0, cap.formats ? &cap : 0, c,
			 &c->pcm);
	if (rc)
		goto fail_card;

	/* interrupts from here on: the streams exist */
	c->irq_armed = 1;
	rc = snd_card_register(card);
	if (rc)
		goto fail_card;

	snd_info(DRV, "%s at %02x:%02x.%u: %s, %s %s, codec %s\n", id->name,
		 pci->bus, pci->device, pci->function,
		 c->irq_ok ? "INTx" : "polled",
		 c->mix_mmio ? "mmio" : "io",
		 c->bm_mmio ? "mmio" : "io", c->codec.name);
	return 0;

fail_card:
	c->irq_armed = 0;
	snd_card_free(card);
	c->card = 0;
fail:
	ich_teardown(c);
	kfree(c);
	return rc;
}

static const struct ich_id *ich_match(const pci_device_t *p)
{
	const struct ich_id *id;

	for (id = ich_ids; id->name; id++)
		if (p->vendor_id == id->vendor && p->device_id == id->device)
			return id;
	return 0;
}

int ac97_probe_all(void)
{
	const pci_device_t *devs;
	int n = 0, i, cards = 0;

	devs = pci_get_devices(&n);
	if (!devs || n <= 0)
		return 0;
	for (i = 0; i < n && cards < SND_MAX_CARDS; i++) {
		const struct ich_id *id = ich_match(&devs[i]);
		if (!id)
			continue;
		/* only audio functions (some chipsets put a modem at the
		 * neighbouring function with the same register model) */
		if (devs[i].class_code != 0x04)
			continue;
		if (ich_probe_one(&devs[i], id) == 0)
			cards++;
	}
	return cards;
}
