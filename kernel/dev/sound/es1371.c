// LikeOS Ensoniq AudioPCI driver: ES1370, ES1371, ES1373, Creative CT5880
// and Ectiva EV1938 ("Sound Blaster PCI 64/128").
//
// This is the sound card VMware offers as "Sound Blaster PCI (es1371)" and
// QEMU as "-device ES1370", and it was on countless real boards in the late
// nineties.  The chip is simple: three bus-master DMA channels that loop over
// one physically contiguous buffer each and raise an interrupt every N
// frames, plus a mixer chip.
//
//   DAC2  playback, the main output (PCM device 0, playback)
//   ADC   capture (PCM device 0, capture)
//   DAC1  a second playback channel (PCM device 1) - mixed in hardware
//
// The two generations differ in where the sound goes after the DMA engine:
//
//   ES1370   DAC2 and the ADC share one programmable clock
//            (1411200 / (div + 2) Hz); DAC1 runs at one of four fixed rates.
//            The analog side is an AKM AK4531, a write-only mixer/codec on a
//            private serial port.  DAC2 enters it as "voice", DAC1 as "FM".
//   ES1371+  every channel runs through a hardware sample rate converter
//            (SRC) that turns any rate from 4 to 48 kHz into the 48 kHz
//            AC'97 link.  The converter is a small RAM-based machine whose
//            per-channel increment is a 6.15 fixed-point number; the AC'97
//            codec register is reached through the same port, and must only
//            be touched while the converter's link state machine is parked.
//
// Interrupts arrive on a (possibly shared) level-triggered INTx line found
// through the ACPI routing table.  If no line can be had the driver falls
// back to a high-resolution timer per running channel, which keeps playback
// going at period granularity.
//
// Locking:
//   es->reg_lock  CONTROL, SERIAL, MEM_PAGE (the paged window), the cached
//                 copies of CONTROL/SERIAL, channel run state, the ES1370
//                 shared-clock bookkeeping.  Taken with interrupts off; the
//                 interrupt handler takes it; it nests inside the core's
//                 substream lock (trigger/pointer are called under it).
//   es->src_lock  the ES1371 SRC RAM port and AC'97 codec port, and the
//                 ES1370 AK4531 port.  Never held together with reg_lock.
//   es->mix_lock  the cached mixer settings (volumes, mutes, inputs).
//                 Ordered before src_lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/sound/snd.h>
#include <kernel/hal/pci.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include "es1371.h"

#define ES_DRV "es1371"

#define ES_MAX_CARDS 4

/* Bound on every hardware busy-wait (port reads).  On real hardware one
 * read is about a microsecond, so this is ~40 ms worst case. */
#define ES_POLL 0xa000u
#define ES_CODEC_READ_TRIES 3
/* AC'97 codec readiness after the cold reset. */
#define ES_CODEC_READY_MS 1000u
#define ES_CODEC_READY_STEP_MS 5u

/* Default mixer settings at probe (0..100). */
#define ES_DEFAULT_MASTER 80u
#define ES_DEFAULT_PCM 100u
#define ES_DEFAULT_CAPTURE 50u

/* ---- chip identification ---------------------------------------------------- */

enum es_chip {
	ES_CHIP_ES1370 = 0,
	ES_CHIP_ES1371,
	ES_CHIP_ES1373,
	ES_CHIP_CT5880,
	ES_CHIP_EV1938,
};

static const char *const es_chip_names[] = {
	[ES_CHIP_ES1370] = "ES1370", [ES_CHIP_ES1371] = "ES1371",
	[ES_CHIP_ES1373] = "ES1373", [ES_CHIP_CT5880] = "CT5880",
	[ES_CHIP_EV1938] = "EV1938",
};

#define ESF_AK4531 (1u << 0) /* AK4531 mixer on the private codec port */
#define ESF_AC97 (1u << 1) /* AC'97 codec behind the SRC */
#define ESF_SRC (1u << 2) /* hardware sample rate converter */
#define ESF_SPDIF (1u << 3) /* S/PDIF output logic present (left off) */
#define ESF_AC97_RESET (1u << 4) /* needs the status-register link reset */
#define ESF_BYPASS (1u << 5) /* SRC bypass bits exist */
#define ESF_EV1938 (1u << 6) /* codec accesses carry the EV1938 bit */
#define ESF_REAR (1u << 7) /* four-channel rear output logic */

#define ES_REV_ANY (-1)

struct es_model {
	uint16_t vendor, device;
	int16_t rev; /* ES_REV_ANY or an exact revision */
	uint8_t chip;
	uint32_t flags;
	const char *name;
};

/* Most specific first: the first match wins. */
static const struct es_model es_models[] = {
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_ES1370, ES_REV_ANY, ES_CHIP_ES1370,
	  ESF_AK4531, "Ensoniq AudioPCI ES1370" },

	{ ES_VENDOR_ENSONIQ, ES_DEVICE_ES1371, ES_REV_ES1373_A, ES_CHIP_ES1373,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR,
	  "Ensoniq AudioPCI ES1373 rev A" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_ES1371, ES_REV_ES1373_B, ES_CHIP_ES1373,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR,
	  "Ensoniq AudioPCI ES1373 rev B" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_ES1371, ES_REV_CT5880_A, ES_CHIP_CT5880,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR |
		  ESF_AC97_RESET,
	  "Creative Sound Blaster PCI CT5880-A" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_ES1371, ES_REV_ES1373_8, ES_CHIP_ES1373,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR |
		  ESF_AC97_RESET,
	  "Ensoniq AudioPCI ES1373 rev 8" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_ES1371, ES_REV_ES1371_B, ES_CHIP_ES1371,
	  ESF_AC97 | ESF_SRC, "Ensoniq AudioPCI ES1371 rev B" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_ES1371, ES_REV_ANY, ES_CHIP_ES1371,
	  ESF_AC97 | ESF_SRC, "Ensoniq AudioPCI ES1371" },

	{ ES_VENDOR_ENSONIQ, ES_DEVICE_CT5880, ES_REV_CT5880_C, ES_CHIP_CT5880,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR |
		  ESF_AC97_RESET,
	  "Creative Sound Blaster PCI CT5880-C" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_CT5880, ES_REV_CT5880_D, ES_CHIP_CT5880,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR |
		  ESF_AC97_RESET,
	  "Creative Sound Blaster PCI CT5880-D" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_CT5880, ES_REV_CT5880_E, ES_CHIP_CT5880,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR |
		  ESF_AC97_RESET,
	  "Creative Sound Blaster PCI CT5880-E" },
	{ ES_VENDOR_ENSONIQ, ES_DEVICE_CT5880, ES_REV_ANY, ES_CHIP_CT5880,
	  ESF_AC97 | ESF_SRC | ESF_SPDIF | ESF_BYPASS | ESF_REAR,
	  "Creative Sound Blaster PCI CT5880" },

	{ ES_VENDOR_ECTIVA, ES_DEVICE_EV1938, ES_REV_ANY, ES_CHIP_EV1938,
	  ESF_AC97 | ESF_SRC | ESF_EV1938, "Ectiva EV1938" },
};

/* Boards whose speaker amplifier hangs off GPIO output 0 of the ES1371
 * family and stays silent unless it is driven high. */
struct es_subsys {
	uint16_t vendor, device;
	const char *what;
};

static const struct es_subsys es_amp_gpio_boards[] = {
	{ 0x107b, 0x2150, "Gateway Solo 2150" },
	{ 0x13bd, 0x100c, "Sharp Mebius PC-MJ100V" },
	{ 0x1102, 0x5938, "Targa Xtender 300" },
	{ 0x1102, 0x8938, "IPC Topnote G" },
};

/* ---- channels ---------------------------------------------------------------- */

enum { ES_CH_DAC1 = 0, ES_CH_DAC2 = 1, ES_CH_ADC = 2, ES_NUM_CH = 3 };

/* Fixed per-channel register layout. */
struct es_chan_desc {
	const char *name;
	uint32_t ctrl_en; /* CONTROL enable bit */
	uint32_t int_en; /* SERIAL interrupt enable bit */
	uint32_t stat; /* STATUS pending bit */
	uint32_t pause; /* SERIAL pause bit, 0 if the channel has none */
	uint32_t loop_sel; /* SERIAL stop-mode bit (kept clear: loop mode) */
	uint8_t page; /* memory page of the frame registers */
	uint8_t frame_reg, size_reg, count_reg;
	uint8_t mode_shift; /* position of the 2-bit mode in SERIAL */
	uint8_t src_base; /* ES1371 SRC RAM channel base */
	uint32_t src_freeze; /* SMPRATE accumulator-freeze bit */
	uint8_t src_vol; /* SRC RAM volume pair */
	int dir; /* AUDIO_DIR_* */
};

static const struct es_chan_desc es_chan_desc[ES_NUM_CH] = {
	[ES_CH_DAC1] = { "DAC1", ES_CTRL_DAC1_EN, ES_SER_P1_INT_EN,
			 ES_STAT_DAC1, ES_SER_P1_PAUSE, ES_SER_P1_LOOP_SEL,
			 ES_PAGE_DAC, ES_REG_DAC1_FRAME, ES_REG_DAC1_SIZE,
			 ES_REG_DAC1_COUNT, ES_SER_P1_MODE_SHIFT,
			 ES1371_SRC_DAC1, ES1371_SMP_DIS_P1,
			 ES1371_SRC_VOL_DAC1, AUDIO_DIR_PLAYBACK },
	[ES_CH_DAC2] = { "DAC2", ES_CTRL_DAC2_EN, ES_SER_P2_INT_EN,
			 ES_STAT_DAC2, ES_SER_P2_PAUSE, ES_SER_P2_LOOP_SEL,
			 ES_PAGE_DAC, ES_REG_DAC2_FRAME, ES_REG_DAC2_SIZE,
			 ES_REG_DAC2_COUNT, ES_SER_P2_MODE_SHIFT,
			 ES1371_SRC_DAC2, ES1371_SMP_DIS_P2,
			 ES1371_SRC_VOL_DAC2, AUDIO_DIR_PLAYBACK },
	[ES_CH_ADC] = { "ADC", ES_CTRL_ADC_EN, ES_SER_R1_INT_EN, ES_STAT_ADC,
			0, ES_SER_R1_LOOP_SEL, ES_PAGE_ADC, ES_REG_ADC_FRAME,
			ES_REG_ADC_SIZE, ES_REG_ADC_COUNT,
			ES_SER_R1_MODE_SHIFT, ES1371_SRC_ADC,
			ES1371_SMP_DIS_R1, ES1371_SRC_VOL_ADC,
			AUDIO_DIR_CAPTURE },
};

struct es_card;

struct es_chan {
	struct es_card *es;
	int id; /* ES_CH_* */
	const struct es_chan_desc *d;
	struct snd_pcm_substream *ss; /* bound at probe */

	/* configured by hw_params, used by prepare (process context) */
	int configured;
	uint32_t mode; /* ES_MODE_* */
	uint32_t rate;
	uint32_t frame_bytes;
	uint32_t buffer_bytes;
	uint32_t period_bytes;
	uint32_t period_frames;
	uint64_t dma_phys;
	uint64_t period_ns;

	/* run state, under reg_lock */
	volatile int running;
	int opened;
	uint32_t last_pos;

	/* timer fallback when no interrupt line is available */
	hrtimer_t poll_timer;

	/* statistics */
	uint64_t irqs;
	uint64_t starts;
};

/* Mixer controls this driver implements (indexes into the cached state). */
#define ES_NCTL (AUDIO_CTL_MAX + 1)

/* Capture inputs */
enum {
	ES_IN_MIC = 0,
	ES_IN_LINE,
	ES_IN_CD,
	ES_IN_AUX, /* AC'97 only */
	ES_IN_MIX, /* AC'97 only: what is playing */
	ES_IN_COUNT,
};

static const char *const es_input_names[ES_IN_COUNT] = {
	"Microphone", "Line In", "CD", "Aux In", "Stereo Mix",
};

struct es_card {
	const pci_device_t *pci;
	const struct es_model *model;
	uint8_t chip;
	uint32_t flags;
	uint8_t rev;
	uint16_t subvendor, subdevice;
	uint16_t port;
	uint32_t io_size;
	char chip_label[64];

	struct snd_card *card;
	struct snd_pcm *pcm_main; /* DAC2 + ADC */
	struct snd_pcm *pcm_aux; /* DAC1 */

	spinlock_t reg_lock;
	spinlock_t src_lock;
	spinlock_t mix_lock;

	uint32_t ctrl; /* shadow of CONTROL */
	uint32_t sctrl; /* shadow of SERIAL */
	uint32_t cssr; /* what we write to STATUS (ES1371 family) */

	struct es_chan ch[ES_NUM_CH];

	/* interrupt */
	int irq_gsi;
	int irq_polling;
	volatile int dead; /* the device stopped answering */
	uint64_t irq_total;
	uint64_t irq_foreign; /* shared line, not ours */
	uint32_t irq_unknown_run; /* consecutive interrupts with no source */
	uint64_t irq_unknown;

	/* ES1370 */
	void *phantom; /* scratch page the ADC's phantom frame points at */
	uint64_t phantom_phys;
	uint32_t pclk_users; /* bitmask of ES_CH_* holding the shared clock */
	uint32_t pclk_div;
	uint8_t ak[AK4531_NREGS]; /* write-only codec: our shadow */

	/* ES1371 SRC / AC'97 */
	int src_state_ok; /* the SRC link-state handshake works */
	uint32_t src_timeouts;
	uint32_t codec_timeouts;
	int codec_ok;
	uint16_t ac97_caps; /* AC97_RESET */
	uint16_t ac97_extid; /* AC97_EXTENDED_ID */
	uint32_t ac97_id;
	int master_max; /* 31 or 63 attenuation steps */
	int has_headphone;
	char codec_name[64];

	/* mixer (mix_lock) */
	uint32_t vol_l[ES_NCTL], vol_r[ES_NCTL];
	uint32_t mute[ES_NCTL];
	uint32_t input; /* ES_IN_* */
	uint32_t input_mode; /* AUDIO_OUTSEL_* */
	uint32_t output_mode;
};

static struct es_card *es_cards[ES_MAX_CARDS];
static int es_ncards;

/* ---- port access ------------------------------------------------------------- */

static inline uint32_t es_rd32(struct es_card *es, uint16_t off)
{
	return inl((uint16_t)(es->port + off));
}

static inline void es_wr32(struct es_card *es, uint16_t off, uint32_t v)
{
	outl((uint16_t)(es->port + off), v);
}

static inline uint16_t es_rd16(struct es_card *es, uint16_t off)
{
	return inw((uint16_t)(es->port + off));
}

static inline void es_wr16(struct es_card *es, uint16_t off, uint16_t v)
{
	outw((uint16_t)(es->port + off), v);
}

static inline uint8_t es_rd8(struct es_card *es, uint16_t off)
{
	return inb((uint16_t)(es->port + off));
}

static inline void es_wr8(struct es_card *es, uint16_t off, uint8_t v)
{
	outb((uint16_t)(es->port + off), v);
}

static inline int es_is1370(const struct es_card *es)
{
	return es->chip == ES_CHIP_ES1370;
}

/* A dword read of all ones from a register that has reserved zero bits
 * means the device is gone (surprise removal, or a VM that dropped it). */
static int es_check_alive(struct es_card *es)
{
	if (es->dead)
		return 0;
	if (es_rd32(es, ES_REG_STATUS) == 0xffffffffu) {
		es->dead = 1;
		snd_info(ES_DRV, "%s: device stopped responding\n",
			 es->chip_label);
		return 0;
	}
	return 1;
}

/* ---- ES1371 sample rate converter ------------------------------------------- */

/* Wait until the SRC RAM port is idle; returns the port's current value
 * (with the control bits a caller must carry over). */
static uint32_t es1371_src_ready(struct es_card *es)
{
	uint32_t v = 0;

	for (uint32_t i = 0; i < ES_POLL; i++) {
		v = es_rd32(es, ES1371_REG_SMPRATE);
		if (v == 0xffffffffu)
			return 0;
		if (!(v & ES1371_SMP_BUSY))
			return v;
	}
	es->src_timeouts++;
	return v & ~ES1371_SMP_BUSY;
}

/* Wait for the converter's internal link state; 1 if it was seen. */
static int es1371_src_wait_state(struct es_card *es, uint32_t state)
{
	for (uint32_t i = 0; i < ES_POLL; i++) {
		uint32_t v = es_rd32(es, ES1371_REG_SMPRATE);

		if (v == 0xffffffffu)
			return 0;
		if ((v & ES1371_SMP_STATE_MASK) == state)
			return 1;
	}
	return 0;
}

/* Write one SRC RAM word.  src_lock held. */
static void es1371_src_write(struct es_card *es, uint32_t addr, uint32_t data)
{
	uint32_t r = es1371_src_ready(es) & ES1371_SMP_KEEP;

	r |= ES1371_SMP_ADDR(addr) | (data & ES1371_SMP_DATA_MASK);
	es_wr32(es, ES1371_REG_SMPRATE, r | ES1371_SMP_WE);
}

/* Read one SRC RAM word.  src_lock held.
 *
 * A read is a write of the address without WE; the converter then fetches
 * the word during its next idle slot.  The data is valid once the port is
 * idle again and the link state machine is back in its "safe" state. */
static uint32_t es1371_src_read(struct es_card *es, uint32_t addr)
{
	uint32_t orig = es1371_src_ready(es);
	uint32_t keep = orig & ES1371_SMP_KEEP;
	uint32_t v;

	es_wr32(es, ES1371_REG_SMPRATE,
		keep | ES1371_SMP_ADDR(addr) | ES1371_SMP_STATE_SAFE);
	v = es1371_src_ready(es);
	if (es->src_state_ok &&
	    (v & ES1371_SMP_STATE_MASK) != ES1371_SMP_STATE_SAFE) {
		for (uint32_t i = 0; i < ES_POLL; i++) {
			v = es_rd32(es, ES1371_REG_SMPRATE);
			if ((v & ES1371_SMP_STATE_MASK) ==
			    ES1371_SMP_STATE_SAFE)
				break;
		}
	}
	/* Leave the port addressing the word without the state request. */
	es_wr32(es, ES1371_REG_SMPRATE, keep | ES1371_SMP_ADDR(addr));
	return v & ES1371_SMP_DATA_MASK;
}

/* Set the accumulator-freeze bits (SMP_DIS_*) to `bits' for the channels
 * in `mask', keeping the rest.  src_lock held. */
static void es1371_src_freeze(struct es_card *es, uint32_t mask,
			      uint32_t bits)
{
	uint32_t r = es1371_src_ready(es) & ES1371_SMP_KEEP;

	r = (r & ~mask) | (bits & mask);
	es_wr32(es, ES1371_REG_SMPRATE, r);
}

/* Program a playback converter for `rate' Hz (4000..48000).
 *
 * The converter steps through the source stream with a 6.15 fixed-point
 * increment measured in units of 3 kHz: 48 kHz output pulls 16 input
 * frames per 3 kHz tick, so increment = rate / 3000 in 1/32768ths:
 *
 *     freq = round(rate * 32768 / 3000)
 *
 * which is at most 16 << 15 for 48 kHz and never overflows 32 bits.  The
 * integer part goes into bits 15..10 of the INT_REGS word (whose low byte
 * holds converter state that must be preserved) and the fraction into
 * VFREQ_FRAC.  The accumulator is frozen while the two words change so the
 * converter never runs with half a ratio. */
static void es1371_dac_rate(struct es_card *es, int ch, uint32_t rate)
{
	const struct es_chan_desc *d = &es_chan_desc[ch];
	uint64_t fl;
	uint32_t freq, ints;

	if (rate < 4000)
		rate = 4000;
	if (rate > 48000)
		rate = 48000;
	freq = (uint32_t)((((uint64_t)rate << 15) + 1500u) / 3000u);

	spin_lock_irqsave(&es->src_lock, &fl);
	es1371_src_freeze(es, d->src_freeze, d->src_freeze);
	ints = es1371_src_read(es, d->src_base + ES1371_SRC_INT_REGS) & 0x00ffu;
	ints |= (freq >> 5) & 0xfc00u;
	es1371_src_write(es, d->src_base + ES1371_SRC_INT_REGS, ints);
	es1371_src_write(es, d->src_base + ES1371_SRC_VFREQ_FRAC,
			 freq & 0x7fffu);
	es1371_src_freeze(es, d->src_freeze, 0);
	spin_unlock_irqrestore(&es->src_lock, fl);
}

/* Program the capture converter for `rate' Hz (4000..48000).
 *
 * Capture runs the other way: the 48 kHz link is decimated to `rate'.  The
 * converter filters over n = rate / 3000 taps per output sample (n odd
 * values 9, 11, 13 and 15 do not work with the filter tables and are
 * stepped down by one), with a truncation length derived from n and a
 * stride of (48000 / rate) * n in 6.15 fixed point.  Rates of 24 kHz and
 * above use the short filter (limit 239), lower rates the long one (limit
 * 119, flagged by bit 15).  The ADC volume words compensate the gain of
 * the n-tap accumulation: n << 8 is unity. */
static void es1371_adc_rate(struct es_card *es, uint32_t rate)
{
	uint32_t n, truncm, freq, trunc_word, ints;
	uint64_t fl;

	if (rate < 4000)
		rate = 4000;
	if (rate > 48000)
		rate = 48000;
	n = rate / 3000u; /* 1..16 */
	if (n == 9 || n == 11 || n == 13 || n == 15)
		n--;
	truncm = (21u * n - 1u) | 1u;
	freq = (uint32_t)((48000ull << 15) / rate) * n;
	if (rate >= 24000) {
		if (truncm > 239)
			truncm = 239;
		trunc_word = (((239u - truncm) >> 1) << 9) | (n << 4);
	} else {
		if (truncm > 119)
			truncm = 119;
		trunc_word = 0x8000u | (((119u - truncm) >> 1) << 9) | (n << 4);
	}

	spin_lock_irqsave(&es->src_lock, &fl);
	es1371_src_freeze(es, ES1371_SMP_DIS_R1, ES1371_SMP_DIS_R1);
	es1371_src_write(es, ES1371_SRC_ADC + ES1371_SRC_TRUNC_N, trunc_word);
	ints = es1371_src_read(es, ES1371_SRC_ADC + ES1371_SRC_INT_REGS) &
	       0x00ffu;
	ints |= (freq >> 5) & 0xfc00u;
	es1371_src_write(es, ES1371_SRC_ADC + ES1371_SRC_INT_REGS, ints);
	es1371_src_write(es, ES1371_SRC_ADC + ES1371_SRC_VFREQ_FRAC,
			 freq & 0x7fffu);
	es1371_src_write(es, ES1371_SRC_VOL_ADC, n << 8);
	es1371_src_write(es, ES1371_SRC_VOL_ADC + 1, n << 8);
	es1371_src_freeze(es, ES1371_SMP_DIS_R1, 0);
	spin_unlock_irqrestore(&es->src_lock, fl);
}

/* Bring the converter up from scratch: disabled, RAM cleared, every
 * channel given a valid ratio and unity volume, then enabled.  Enabling
 * the converter with garbage in its RAM can wedge it until power-off, so
 * the order matters. */
static void es1371_src_init(struct es_card *es)
{
	uint64_t fl;

	spin_lock_irqsave(&es->src_lock, &fl);
	es1371_src_ready(es);
	es_wr32(es, ES1371_REG_SMPRATE, ES1371_SMP_DISABLE);
	for (uint32_t a = 0; a < ES1371_SRC_RAM_WORDS; a++)
		es1371_src_write(es, a, 0);
	/* playback channels: 16 taps, ratio 1.0 (48 kHz) until programmed */
	es1371_src_write(es, ES1371_SRC_DAC1 + ES1371_SRC_TRUNC_N, 16u << 4);
	es1371_src_write(es, ES1371_SRC_DAC1 + ES1371_SRC_INT_REGS, 16u << 10);
	es1371_src_write(es, ES1371_SRC_DAC2 + ES1371_SRC_TRUNC_N, 16u << 4);
	es1371_src_write(es, ES1371_SRC_DAC2 + ES1371_SRC_INT_REGS, 16u << 10);
	es1371_src_write(es, ES1371_SRC_VOL_ADC, ES1371_SRC_UNITY_VOL);
	es1371_src_write(es, ES1371_SRC_VOL_ADC + 1, ES1371_SRC_UNITY_VOL);
	es1371_src_write(es, ES1371_SRC_VOL_DAC1, ES1371_SRC_UNITY_VOL);
	es1371_src_write(es, ES1371_SRC_VOL_DAC1 + 1, ES1371_SRC_UNITY_VOL);
	es1371_src_write(es, ES1371_SRC_VOL_DAC2, ES1371_SRC_UNITY_VOL);
	es1371_src_write(es, ES1371_SRC_VOL_DAC2 + 1, ES1371_SRC_UNITY_VOL);
	spin_unlock_irqrestore(&es->src_lock, fl);

	/* sensible ratios on every channel before the converter runs */
	es1371_adc_rate(es, 48000);
	es1371_dac_rate(es, ES_CH_DAC1, 48000);
	es1371_dac_rate(es, ES_CH_DAC2, 48000);

	spin_lock_irqsave(&es->src_lock, &fl);
	es1371_src_ready(es);
	es_wr32(es, ES1371_REG_SMPRATE, 0); /* converter on, nothing frozen */
	spin_unlock_irqrestore(&es->src_lock, fl);
}

/* ---- ES1371 AC'97 codec access ------------------------------------------------ */

static uint32_t es1371_codec_flag(const struct es_card *es)
{
	return (es->flags & ESF_EV1938) ? EV1938_CODEC_MAGIC : 0;
}

/* Wait for the codec port to finish the previous access. */
static int es1371_codec_idle(struct es_card *es)
{
	for (uint32_t i = 0; i < ES_POLL; i++) {
		uint32_t v = es_rd32(es, ES1371_REG_CODEC);

		if (v == 0xffffffffu)
			return -ENODEV;
		if (!(v & ES1371_CODEC_WIP))
			return 0;
	}
	es->codec_timeouts++;
	return -ETIMEDOUT;
}

/* Park the converter so a codec access cannot collide with its own use of
 * the AC-link: request the "safe" state, wait for the state machine to pass
 * through idle into it.  Returns the port value to restore afterwards.
 *
 * Emulations do not always model the state bits; the first time the wait
 * fails the handshake is switched off for good, so a VM does not pay two
 * full timeouts on every codec register. */
static uint32_t es1371_src_park(struct es_card *es)
{
	uint32_t saved = es1371_src_ready(es);

	es_wr32(es, ES1371_REG_SMPRATE,
		(saved & ES1371_SMP_KEEP) | ES1371_SMP_STATE_SAFE);
	if (es->src_state_ok) {
		if (!es1371_src_wait_state(es, ES1371_SMP_STATE_IDLE) ||
		    !es1371_src_wait_state(es, ES1371_SMP_STATE_SAFE)) {
			es->src_state_ok = 0;
			snd_info(ES_DRV,
				 "%s: SRC link state not reported, "
				 "accessing the codec without it\n",
				 es->chip_label);
		}
	}
	return saved & ~(ES1371_SMP_WE | ES1371_SMP_BUSY);
}

static void es1371_src_unpark(struct es_card *es, uint32_t saved)
{
	es1371_src_ready(es);
	es_wr32(es, ES1371_REG_SMPRATE, saved);
}

static int es1371_codec_write(struct es_card *es, uint32_t reg, uint16_t val)
{
	uint64_t fl;
	uint32_t saved;
	int rc;

	if (es->dead)
		return -ENODEV;
	spin_lock_irqsave(&es->src_lock, &fl);
	rc = es1371_codec_idle(es);
	if (rc == 0) {
		saved = es1371_src_park(es);
		es_wr32(es, ES1371_REG_CODEC,
			ES1371_CODEC_ADDR(reg) | ES1371_CODEC_DATA(val) |
				es1371_codec_flag(es));
		es1371_src_unpark(es, saved);
	}
	spin_unlock_irqrestore(&es->src_lock, fl);
	return rc;
}

static int es1371_codec_read(struct es_card *es, uint32_t reg, uint16_t *out)
{
	int rc = -ETIMEDOUT;

	if (es->dead)
		return -ENODEV;
	for (int attempt = 0; attempt < ES_CODEC_READ_TRIES; attempt++) {
		uint64_t fl;
		uint32_t saved, v;

		spin_lock_irqsave(&es->src_lock, &fl);
		rc = es1371_codec_idle(es);
		if (rc) {
			spin_unlock_irqrestore(&es->src_lock, fl);
			if (rc == -ENODEV)
				return rc;
			continue;
		}
		saved = es1371_src_park(es);
		es_wr32(es, ES1371_REG_CODEC,
			ES1371_CODEC_ADDR(reg) | ES1371_CODEC_PIRD |
				es1371_codec_flag(es));
		es1371_src_unpark(es, saved);
		/* the read runs over the link; then the data shows up */
		(void)es1371_codec_idle(es);
		rc = -ETIMEDOUT;
		for (uint32_t i = 0; i < ES_POLL; i++) {
			v = es_rd32(es, ES1371_REG_CODEC);
			if (v == 0xffffffffu) {
				rc = -ENODEV;
				break;
			}
			if (v & ES1371_CODEC_RDY) {
				if (es->flags & ESF_EV1938) {
					/* the EV1938 latches the data a little
					 * after it flags it ready */
					for (int k = 0; k < 100; k++)
						(void)es_rd32(es,
							      ES_REG_CONTROL);
					v = es_rd32(es, ES1371_REG_CODEC);
				}
				*out = (uint16_t)ES1371_CODEC_DATA(v);
				rc = 0;
				break;
			}
		}
		spin_unlock_irqrestore(&es->src_lock, fl);
		if (rc != -ETIMEDOUT)
			return rc;
		es->codec_timeouts++;
	}
	return rc;
}

/* Read-modify-write of a codec register. */
static int es1371_codec_update(struct es_card *es, uint32_t reg,
			       uint16_t mask, uint16_t val)
{
	uint16_t old;
	int rc = es1371_codec_read(es, reg, &old);

	if (rc)
		return rc;
	return es1371_codec_write(es, reg, (uint16_t)((old & ~mask) | (val & mask)));
}

/* ---- AC'97 codec names ---------------------------------------------------------- */

struct ac97_name {
	uint32_t id;
	uint32_t mask;
	const char *name;
};

/* Codecs found next to the ES1371 family and in virtual machines.  An
 * entry matches when (id & mask) == entry id. */
static const struct ac97_name ac97_names[] = {
	{ 0x41445303, 0xffffffff, "Analog Devices AD1819" },
	{ 0x41445340, 0xffffffff, "Analog Devices AD1881" },
	{ 0x41445348, 0xffffffff, "Analog Devices AD1881A" },
	{ 0x41445360, 0xffffffff, "Analog Devices AD1885" },
	{ 0x41445361, 0xffffffff, "Analog Devices AD1886" },
	{ 0x41445362, 0xffffffff, "Analog Devices AD1887" },
	{ 0x41445363, 0xffffffff, "Analog Devices AD1886A" },
	{ 0x41445370, 0xffffffff, "Analog Devices AD1980" },
	{ 0x41445372, 0xffffffff, "Analog Devices AD1981A" },
	{ 0x41445374, 0xffffffff, "Analog Devices AD1981B" },
	{ 0x41445375, 0xffffffff, "Analog Devices AD1985" },
	{ 0x414b4d00, 0xffffffff, "Asahi Kasei AK4540" },
	{ 0x414b4d01, 0xffffffff, "Asahi Kasei AK4542" },
	{ 0x414b4d02, 0xffffffff, "Asahi Kasei AK4543" },
	{ 0x414c4300, 0xfffffff0, "Realtek ALC100" },
	{ 0x414c4710, 0xfffffff0, "Realtek ALC200" },
	{ 0x414c4720, 0xfffffff0, "Realtek ALC650" },
	{ 0x414c4730, 0xffffffff, "Realtek ALC101" },
	{ 0x414c4740, 0xfffffff0, "Realtek ALC202" },
	{ 0x414c4750, 0xfffffff0, "Realtek ALC250" },
	{ 0x414c4760, 0xfffffff0, "Realtek ALC655" },
	{ 0x414c4780, 0xfffffff0, "Realtek ALC658" },
	{ 0x414c4790, 0xfffffff0, "Realtek ALC850" },
	{ 0x434d4941, 0xffffffff, "C-Media CMI9738" },
	{ 0x434d4961, 0xffffffff, "C-Media CMI9739" },
	{ 0x434d4978, 0xffffffff, "C-Media CMI9761" },
	{ 0x43525900, 0xfffffff8, "Cirrus Logic CS4297" },
	{ 0x43525910, 0xfffffff8, "Cirrus Logic CS4297A" },
	{ 0x43525920, 0xfffffff8, "Cirrus Logic CS4298" },
	{ 0x43525928, 0xfffffff8, "Cirrus Logic CS4294" },
	{ 0x43525930, 0xfffffff8, "Cirrus Logic CS4299" },
	{ 0x43525948, 0xfffffff8, "Cirrus Logic CS4201" },
	{ 0x43525958, 0xfffffff8, "Cirrus Logic CS4205" },
	{ 0x43585428, 0xfffffff8, "Conexant CX20468" },
	{ 0x44543031, 0xfffffff0, "Diamond Technology DT0398" },
	{ 0x45838308, 0xffffffff, "ESS ES1921" },
	{ 0x49434501, 0xffffffff, "ICEnsemble ICE1230" },
	{ 0x49434511, 0xffffffff, "ICEnsemble ICE1232" },
	{ 0x49434514, 0xffffffff, "ICEnsemble ICE1232A" },
	{ 0x49434551, 0xffffffff, "VIA VT1616" },
	{ 0x49434552, 0xffffffff, "VIA VT1616i" },
	{ 0x4e534331, 0xffffffff, "National LM4549" },
	{ 0x4e534350, 0xffffffff, "National LM4550" },
	{ 0x50534304, 0xffffffff, "Philips UCB1400" },
	{ 0x53494c22, 0xffffffff, "Silicon Labs Si3036" },
	{ 0x53494c23, 0xffffffff, "Silicon Labs Si3038" },
	{ 0x54524102, 0xffffffff, "TriTech TR28022" },
	{ 0x54524103, 0xffffffff, "TriTech TR28023" },
	{ 0x54524106, 0xffffffff, "TriTech TR28026" },
	{ 0x54524108, 0xffffffff, "TriTech TR28028" },
	{ 0x54524123, 0xffffffff, "TriTech TR28602" },
	{ 0x54584e20, 0xffffffff, "Texas Instruments TLC320AD9xC" },
	{ 0x56494161, 0xffffffff, "VIA VT1612A" },
	{ 0x56494170, 0xffffffff, "VIA VT1617A" },
	{ 0x574d4c00, 0xffffffff, "Wolfson WM9701A" },
	{ 0x574d4c03, 0xffffffff, "Wolfson WM9703/WM9707" },
	{ 0x574d4c04, 0xffffffff, "Wolfson WM9704" },
	{ 0x574d4c05, 0xffffffff, "Wolfson WM9705" },
	{ 0x574d4d09, 0xffffffff, "Wolfson WM9709" },
	{ 0x574d4c12, 0xffffffff, "Wolfson WM9711/WM9712" },
	{ 0x594d4800, 0xffffffff, "Yamaha YMF743" },
	{ 0x594d4802, 0xffffffff, "Yamaha YMF752" },
	{ 0x594d4803, 0xffffffff, "Yamaha YMF753" },
	{ 0x83847600, 0xffffffff, "SigmaTel STAC9700/83/84" },
	{ 0x83847604, 0xffffffff, "SigmaTel STAC9701/03/04/05" },
	{ 0x83847605, 0xffffffff, "SigmaTel STAC9704" },
	{ 0x83847608, 0xffffffff, "SigmaTel STAC9708/11" },
	{ 0x83847609, 0xffffffff, "SigmaTel STAC9721/23" },
	{ 0x83847644, 0xffffffff, "SigmaTel STAC9744/45" },
	{ 0x83847650, 0xffffffff, "SigmaTel STAC9750/51" },
	{ 0x83847652, 0xffffffff, "SigmaTel STAC9752/53" },
	{ 0x83847656, 0xffffffff, "SigmaTel STAC9756/57" },
	{ 0x83847666, 0xffffffff, "SigmaTel STAC9766/67" },
};

struct ac97_vendor {
	uint32_t prefix; /* the upper 24 bits of the ID */
	const char *name;
};

static const struct ac97_vendor ac97_vendors[] = {
	{ 0x414453, "Analog Devices" }, { 0x414b4d, "Asahi Kasei" },
	{ 0x414c43, "Realtek" },	{ 0x414c47, "Realtek" },
	{ 0x434d49, "C-Media" },	{ 0x435258, "Cirrus Logic" },
	{ 0x435359, "Cirrus Logic" },	{ 0x435854, "Conexant" },
	{ 0x445430, "Diamond Technology" }, { 0x454343, "eMicro" },
	{ 0x458383, "ESS" },		{ 0x485253, "Intersil" },
	{ 0x494345, "ICEnsemble" },	{ 0x495445, "ITE" },
	{ 0x4e5343, "National Semiconductor" }, { 0x505343, "Philips" },
	{ 0x53494c, "Silicon Labs" },	{ 0x53544d, "STMicroelectronics" },
	{ 0x545241, "TriTech" },	{ 0x54584e, "Texas Instruments" },
	{ 0x564941, "VIA" },		{ 0x574d4c, "Wolfson" },
	{ 0x594d48, "Yamaha" },		{ 0x838476, "SigmaTel" },
};

static void ac97_describe(uint32_t id, char *buf, size_t len)
{
	for (size_t i = 0; i < sizeof(ac97_names) / sizeof(ac97_names[0]);
	     i++) {
		if ((id & ac97_names[i].mask) == ac97_names[i].id) {
			ksnprintf(buf, len, "%s", ac97_names[i].name);
			return;
		}
	}
	for (size_t i = 0; i < sizeof(ac97_vendors) / sizeof(ac97_vendors[0]);
	     i++) {
		if ((id >> 8) == ac97_vendors[i].prefix) {
			ksnprintf(buf, len, "%s AC97 codec %02x",
				  ac97_vendors[i].name, id & 0xffu);
			return;
		}
	}
	ksnprintf(buf, len, "AC97 codec %08x", id);
}

/* ---- volume arithmetic (integer only) ------------------------------------------- */

static inline uint32_t es_clamp_pct(uint32_t v)
{
	return v > 100 ? 100 : v;
}

/* 0..100 onto an attenuation register where 0 is loudest and `max' is
 * quietest, linear in dB (each step is the same number of dB). */
static inline uint32_t es_pct_to_att(uint32_t pct, uint32_t max)
{
	pct = es_clamp_pct(pct);
	return ((100u - pct) * max + 50u) / 100u;
}

/* Same, for a gain register whose loudest usable value is `top' (0 dB)
 * and quietest `bottom'. */
static inline uint32_t es_pct_to_gain_reg(uint32_t pct, uint32_t top,
					  uint32_t bottom)
{
	pct = es_clamp_pct(pct);
	return top + ((100u - pct) * (bottom - top) + 50u) / 100u;
}

/* ---- AC'97 mixer (ES1371 family) -------------------------------------------- */

/* Master: 1.5 dB attenuation steps, 5 or 6 bits per channel. */
static uint16_t es_ac97_master_word(struct es_card *es, uint32_t l, uint32_t r,
				    uint32_t mute)
{
	uint32_t max = (uint32_t)es->master_max;
	uint32_t al = es_pct_to_att(l, max), ar = es_pct_to_att(r, max);
	uint16_t w = (uint16_t)((al << 8) | ar);

	if (mute || (l == 0 && r == 0))
		w |= AC97_MUTE;
	return w;
}

/* PCM out: 0x08 is 0 dB, 0x1f is -34.5 dB.  The top of the slider is 0 dB:
 * the +12 dB above it only clips a full-scale stream. */
static uint16_t es_ac97_pcm_word(uint32_t l, uint32_t r, uint32_t mute)
{
	uint32_t gl = es_pct_to_gain_reg(l, AC97_PCM_0DB, AC97_PCM_MAX);
	uint32_t gr = es_pct_to_gain_reg(r, AC97_PCM_0DB, AC97_PCM_MAX);
	uint16_t w = (uint16_t)((gl << 8) | gr);

	if (mute || (l == 0 && r == 0))
		w |= AC97_MUTE;
	return w;
}

/* Record gain: 0..15 in 1.5 dB steps from 0 dB up. */
static uint16_t es_ac97_capture_word(uint32_t l, uint32_t r, uint32_t mute)
{
	uint32_t gl = (es_clamp_pct(l) * 15u + 50u) / 100u;
	uint32_t gr = (es_clamp_pct(r) * 15u + 50u) / 100u;
	uint16_t w = (uint16_t)((gl << 8) | gr);

	if (mute)
		w |= AC97_MUTE;
	return w;
}

static const uint8_t es_ac97_rec_code[ES_IN_COUNT] = {
	[ES_IN_MIC] = AC97_REC_MIC,   [ES_IN_LINE] = AC97_REC_LINE,
	[ES_IN_CD] = AC97_REC_CD,     [ES_IN_AUX] = AC97_REC_AUX,
	[ES_IN_MIX] = AC97_REC_STEREO_MIX,
};

/* Push one cached control to the codec.  mix_lock held by the caller is
 * not required; values are passed in. */
static int es_ac97_apply(struct es_card *es, uint32_t ctl, uint32_t l,
			 uint32_t r, uint32_t mute)
{
	int rc;

	if (!es->codec_ok)
		return -ENODEV;
	switch (ctl) {
	case AUDIO_CTL_MASTER: {
		uint16_t w = es_ac97_master_word(es, l, r, mute);

		rc = es1371_codec_write(es, AC97_MASTER, w);
		/* the headphone/aux-out amplifier follows the master */
		if (rc == 0 && es->has_headphone)
			rc = es1371_codec_write(es, AC97_HEADPHONE, w);
		return rc;
	}
	case AUDIO_CTL_PCM:
		return es1371_codec_write(es, AC97_PCM,
					  es_ac97_pcm_word(l, r, mute));
	case AUDIO_CTL_CAPTURE:
		return es1371_codec_write(es, AC97_REC_GAIN,
					  es_ac97_capture_word(l, r, mute));
	case AUDIO_CTL_MIC_BOOST:
		return es1371_codec_update(es, AC97_MIC, AC97_MIC_BOOST,
					   l ? AC97_MIC_BOOST : 0);
	default:
		return -ENOENT;
	}
}

static int es_ac97_apply_input(struct es_card *es, uint32_t in)
{
	uint16_t code;

	if (!es->codec_ok)
		return -ENODEV;
	if (in >= ES_IN_COUNT)
		return -EINVAL;
	code = es_ac97_rec_code[in];
	return es1371_codec_write(es, AC97_REC_SEL,
				  (uint16_t)((code << 8) | code));
}

/* Cold-reset follow-up: wait for the codec, identify it and put every
 * register this driver relies on into a known state. */
static int es1371_ac97_init(struct es_card *es)
{
	uint16_t v, id1 = 0, id2 = 0;
	uint32_t waited = 0;
	int ready = 0, failures = 0;

	/* The analog sections of some codecs take most of a second to come
	 * up after the reset.  A read that times out (as opposed to one that
	 * answers "not ready yet") three times means there is no codec. */
	while (waited < ES_CODEC_READY_MS) {
		int rc = es1371_codec_read(es, AC97_POWERDOWN, &v);

		if (rc == -ENODEV)
			return rc;
		if (rc) {
			if (++failures >= 3)
				break;
		} else if ((v & AC97_PD_READY) == AC97_PD_READY) {
			ready = 1;
			break;
		}
		snd_mdelay(ES_CODEC_READY_STEP_MS);
		waited += ES_CODEC_READY_STEP_MS;
	}

	if (es1371_codec_read(es, AC97_VENDOR_ID1, &id1) ||
	    es1371_codec_read(es, AC97_VENDOR_ID2, &id2)) {
		snd_info(ES_DRV, "%s: no AC97 codec answers\n",
			 es->chip_label);
		return -ENODEV;
	}
	es->ac97_id = ((uint32_t)id1 << 16) | id2;
	if (es->ac97_id == 0 || es->ac97_id == 0xffffffffu) {
		snd_info(ES_DRV, "%s: AC97 codec ID %08x is not valid\n",
			 es->chip_label, es->ac97_id);
		return -ENODEV;
	}
	if (!ready)
		snd_info(ES_DRV,
			 "%s: AC97 codec never reported ready (%u ms), "
			 "continuing\n",
			 es->chip_label, waited);

	es->codec_ok = 1;
	ac97_describe(es->ac97_id, es->codec_name, sizeof(es->codec_name));
	if (es1371_codec_read(es, AC97_RESET, &es->ac97_caps))
		es->ac97_caps = 0;
	if (es1371_codec_read(es, AC97_EXTENDED_ID, &es->ac97_extid))
		es->ac97_extid = 0;
	es->has_headphone = (es->ac97_caps & AC97_RESET_CAP_HP) ? 1 : 0;

	/* everything powered */
	es1371_codec_write(es, AC97_POWERDOWN, 0);

	/* Master width: a 5-bit register ignores bit 5 and reads it back as
	 * 0x1f (some) or 0x00 (others); a 6-bit one keeps 0x20. */
	es->master_max = 31;
	if (es1371_codec_write(es, AC97_MASTER, 0xa020) == 0 &&
	    es1371_codec_read(es, AC97_MASTER, &v) == 0 &&
	    (v & 0x3f3f) == 0x2020)
		es->master_max = 63;

	/* Analog inputs stay out of the playback mix (they would add hiss);
	 * they still reach the ADC through the record selector. */
	es1371_codec_write(es, AC97_PC_BEEP, 0x8000);
	es1371_codec_write(es, AC97_PHONE, 0x8008);
	es1371_codec_write(es, AC97_MIC, 0x8008);
	es1371_codec_write(es, AC97_LINE, 0x8808);
	es1371_codec_write(es, AC97_CD, 0x8808);
	es1371_codec_write(es, AC97_VIDEO, 0x8808);
	es1371_codec_write(es, AC97_AUX, 0x8808);
	es1371_codec_write(es, AC97_MASTER_MONO, 0x8000);
	es1371_codec_write(es, AC97_GENERAL_PURPOSE, 0);

	/* The link runs at 48 kHz; rate conversion is the SRC's job.  If the
	 * codec has variable rate audio, make sure it is off and both
	 * converters sit at 48 kHz. */
	if (es->ac97_extid & AC97_EI_VRA) {
		es1371_codec_update(es, AC97_EXTENDED_STATUS, AC97_EI_VRA, 0);
		es1371_codec_write(es, AC97_PCM_FRONT_DAC_RATE, 48000);
		es1371_codec_write(es, AC97_PCM_LR_ADC_RATE, 48000);
	}

	snd_info(ES_DRV, "%s: codec %s (id %08x, caps %04x, ext %04x, "
		 "master %d steps%s)\n",
		 es->chip_label, es->codec_name, es->ac97_id, es->ac97_caps,
		 es->ac97_extid, es->master_max + 1,
		 es->has_headphone ? ", headphone amp" : "");
	return 0;
}

/* ---- AK4531 mixer (ES1370) ---------------------------------------------------- */

static int es1370_ak_write(struct es_card *es, uint8_t reg, uint8_t val)
{
	uint64_t fl;
	int rc = -ETIMEDOUT;

	if (reg >= AK4531_NREGS)
		return -EINVAL;
	if (es->dead)
		return -ENODEV;
	spin_lock_irqsave(&es->src_lock, &fl);
	es->ak[reg] = val;
	for (uint32_t i = 0; i < ES_POLL; i++) {
		uint32_t st = es_rd32(es, ES_REG_STATUS);

		if (st == 0xffffffffu) {
			rc = -ENODEV;
			break;
		}
		if (!(st & ES1370_STAT_CSTAT)) {
			es_wr16(es, ES1370_REG_CODEC,
				(uint16_t)(((uint16_t)reg << 8) | val));
			rc = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&es->src_lock, fl);
	if (rc == -ETIMEDOUT)
		es->codec_timeouts++;
	return rc;
}

/* AK4531 input routing for each capture source: {left SW1, right SW1}. */
static const uint8_t es_ak_input_sw[3][2] = {
	[ES_IN_MIC] = { AK4531_SW1_MIC, AK4531_SW1_MIC },
	[ES_IN_LINE] = { AK4531_SW1_LINE_L, AK4531_SW1_LINE_R },
	[ES_IN_CD] = { AK4531_SW1_CD_L, AK4531_SW1_CD_R },
};

static int es_ak_apply_input(struct es_card *es, uint32_t in, int all)
{
	uint8_t l = 0, r = 0;

	if (all) {
		for (int i = 0; i < 3; i++) {
			l |= es_ak_input_sw[i][0];
			r |= es_ak_input_sw[i][1];
		}
	} else {
		if (in > ES_IN_CD)
			return -EINVAL;
		l = es_ak_input_sw[in][0];
		r = es_ak_input_sw[in][1];
	}
	es1370_ak_write(es, AK4531_LIN_SW1, l);
	es1370_ak_write(es, AK4531_RIN_SW1, r);
	es1370_ak_write(es, AK4531_LIN_SW2, 0);
	return es1370_ak_write(es, AK4531_RIN_SW2, 0);
}

static int es_ak_apply(struct es_card *es, uint32_t ctl, uint32_t l,
		       uint32_t r, uint32_t mute)
{
	switch (ctl) {
	case AUDIO_CTL_MASTER: {
		uint8_t m = (mute || (l == 0 && r == 0)) ? AK4531_MUTE : 0;

		es1370_ak_write(es, AK4531_LMASTER,
				(uint8_t)(m | es_pct_to_att(l, AK4531_MASTER_MAX)));
		return es1370_ak_write(
			es, AK4531_RMASTER,
			(uint8_t)(m | es_pct_to_att(r, AK4531_MASTER_MAX)));
	}
	case AUDIO_CTL_PCM: {
		/* both DACs: DAC2 is the "voice" input, DAC1 the "FM" one */
		uint8_t m = (mute || (l == 0 && r == 0)) ? AK4531_MUTE : 0;
		uint8_t gl = (uint8_t)(m | es_pct_to_gain_reg(l, AK4531_GAIN_0DB,
							      AK4531_GAIN_MAX));
		uint8_t gr = (uint8_t)(m | es_pct_to_gain_reg(r, AK4531_GAIN_0DB,
							      AK4531_GAIN_MAX));

		es1370_ak_write(es, AK4531_LVOICE, gl);
		es1370_ak_write(es, AK4531_RVOICE, gr);
		es1370_ak_write(es, AK4531_LFM, gl);
		return es1370_ak_write(es, AK4531_RFM, gr);
	}
	case AUDIO_CTL_MIC_BOOST:
		return es1370_ak_write(es, AK4531_MIC_GAIN, l ? 1 : 0);
	default:
		return -ENOENT;
	}
}

static void es1370_ak_init(struct es_card *es)
{
	/* out of reset and power-down, clocks from the ES1370 */
	es1370_ak_write(es, AK4531_RESET, 0x03);
	snd_udelay(100);
	es1370_ak_write(es, AK4531_CLOCK, 0x00);

	/* Every analog input at 0 dB and unmuted: the output mixer switches
	 * below decide what is heard, the input switches what is recorded. */
	es1370_ak_write(es, AK4531_LCD, AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_RCD, AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_LLINE, AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_RLINE, AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_LAUXA, AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_RAUXA, AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_MONO1, AK4531_MUTE | AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_MONO2, AK4531_MUTE | AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_MIC, AK4531_GAIN_0DB);
	es1370_ak_write(es, AK4531_MONO_OUT, AK4531_MUTE);

	/* output mixer: only the two DACs */
	es1370_ak_write(es, AK4531_OUT_SW1, AK4531_SW1_FM_L | AK4531_SW1_FM_R);
	es1370_ak_write(es, AK4531_OUT_SW2,
			AK4531_SW2_VOICE_L | AK4531_SW2_VOICE_R);
	es1370_ak_write(es, AK4531_AD_IN, 0x00); /* ADC from the input mixer */
	es1370_ak_write(es, AK4531_MIC_GAIN, 0x00);

	ksnprintf(es->codec_name, sizeof(es->codec_name), "AKM AK4531");
	es->codec_ok = 1;
}

/* ---- mixer state, common to both codecs -------------------------------------- */

static int es_ctl_supported(const struct es_card *es, uint32_t ctl)
{
	switch (ctl) {
	case AUDIO_CTL_MASTER:
	case AUDIO_CTL_PCM:
	case AUDIO_CTL_MIC_BOOST:
		return es->codec_ok;
	case AUDIO_CTL_CAPTURE:
		return es->codec_ok && !es_is1370(es); /* AK4531 has none */
	default:
		return 0;
	}
}

static int es_mute_supported(const struct es_card *es, uint32_t ctl)
{
	return ctl != AUDIO_CTL_MIC_BOOST && es_ctl_supported(es, ctl);
}

static int es_hw_apply(struct es_card *es, uint32_t ctl, uint32_t l,
		       uint32_t r, uint32_t mute)
{
	if (es_is1370(es))
		return es_ak_apply(es, ctl, l, r, mute);
	return es_ac97_apply(es, ctl, l, r, mute);
}

/* dB (hundredths) and step count of a control at a 0..100 setting. */
static void es_ctl_db(const struct es_card *es, uint32_t ctl, uint32_t pct,
		      int32_t *db, int32_t *db_min, int32_t *db_max,
		      uint32_t *steps)
{
	*db = 0;
	*db_min = 0;
	*db_max = 0;
	*steps = 0;
	switch (ctl) {
	case AUDIO_CTL_MASTER:
		if (es_is1370(es)) {
			*db = -200 * (int32_t)es_pct_to_att(pct, AK4531_MASTER_MAX);
			*db_min = -200 * AK4531_MASTER_MAX;
			*steps = AK4531_MASTER_MAX + 1;
		} else {
			uint32_t max = (uint32_t)es->master_max;

			*db = -150 * (int32_t)es_pct_to_att(pct, max);
			*db_min = -150 * (int32_t)max;
			*steps = max + 1;
		}
		break;
	case AUDIO_CTL_PCM:
		if (es_is1370(es)) {
			uint32_t g = es_pct_to_gain_reg(pct, AK4531_GAIN_0DB,
							AK4531_GAIN_MAX);

			*db = 200 * ((int32_t)AK4531_GAIN_0DB - (int32_t)g);
			*db_min = 200 * ((int32_t)AK4531_GAIN_0DB -
					 AK4531_GAIN_MAX);
			*steps = AK4531_GAIN_MAX - AK4531_GAIN_0DB + 1;
		} else {
			uint32_t g = es_pct_to_gain_reg(pct, AC97_PCM_0DB,
							AC97_PCM_MAX);

			*db = 150 * ((int32_t)AC97_PCM_0DB - (int32_t)g);
			*db_min = 150 * ((int32_t)AC97_PCM_0DB - AC97_PCM_MAX);
			*steps = AC97_PCM_MAX - AC97_PCM_0DB + 1;
		}
		break;
	case AUDIO_CTL_CAPTURE:
		*db = 150 * (int32_t)((es_clamp_pct(pct) * 15u + 50u) / 100u);
		*db_max = 150 * 15;
		*steps = 16;
		break;
	case AUDIO_CTL_MIC_BOOST:
		*db_max = es_is1370(es) ? 3000 : 2000;
		*db = pct ? *db_max : 0;
		*steps = 2;
		break;
	}
}

static void es_mixer_defaults(struct es_card *es)
{
	uint64_t fl;

	spin_lock_irqsave(&es->mix_lock, &fl);
	for (uint32_t c = 0; c < ES_NCTL; c++) {
		es->vol_l[c] = es->vol_r[c] = 0;
		es->mute[c] = 0;
	}
	es->vol_l[AUDIO_CTL_MASTER] = es->vol_r[AUDIO_CTL_MASTER] =
		ES_DEFAULT_MASTER;
	es->vol_l[AUDIO_CTL_PCM] = es->vol_r[AUDIO_CTL_PCM] = ES_DEFAULT_PCM;
	es->vol_l[AUDIO_CTL_CAPTURE] = es->vol_r[AUDIO_CTL_CAPTURE] =
		ES_DEFAULT_CAPTURE;
	es->input = ES_IN_MIC;
	es->input_mode = AUDIO_OUTSEL_AUTO;
	es->output_mode = AUDIO_OUTSEL_AUTO;
	spin_unlock_irqrestore(&es->mix_lock, fl);

	if (!es->codec_ok)
		return;
	es_hw_apply(es, AUDIO_CTL_MASTER, ES_DEFAULT_MASTER, ES_DEFAULT_MASTER,
		    0);
	es_hw_apply(es, AUDIO_CTL_PCM, ES_DEFAULT_PCM, ES_DEFAULT_PCM, 0);
	es_hw_apply(es, AUDIO_CTL_MIC_BOOST, 0, 0, 0);
	if (es_is1370(es)) {
		es_ak_apply_input(es, ES_IN_MIC, 0);
	} else {
		es_hw_apply(es, AUDIO_CTL_CAPTURE, ES_DEFAULT_CAPTURE,
			    ES_DEFAULT_CAPTURE, 0);
		es_ac97_apply_input(es, ES_IN_MIC);
	}
}

/* ---- snd_ctl_ops --------------------------------------------------------------- */

static struct es_card *es_from_card(struct snd_card *card)
{
	return card ? (struct es_card *)card->private_data : NULL;
}

static int es_get_volume(struct snd_card *card, uint32_t ctl,
			 struct audio_volume *v)
{
	struct es_card *es = es_from_card(card);
	uint64_t fl;
	uint32_t l, r;

	if (!es || !v)
		return -EINVAL;
	if (ctl >= ES_NCTL || !es_ctl_supported(es, ctl))
		return -ENOENT;
	spin_lock_irqsave(&es->mix_lock, &fl);
	l = es->vol_l[ctl];
	r = es->vol_r[ctl];
	spin_unlock_irqrestore(&es->mix_lock, fl);

	kmemset(v, 0, sizeof(*v));
	v->control = ctl;
	v->left = l;
	v->right = r;
	es_ctl_db(es, ctl, l, &v->db_left, &v->db_min, &v->db_max, &v->steps);
	es_ctl_db(es, ctl, r, &v->db_right, &v->db_min, &v->db_max, &v->steps);
	return 0;
}

static int es_set_volume(struct snd_card *card, uint32_t ctl, uint32_t left,
			 uint32_t right)
{
	struct es_card *es = es_from_card(card);
	uint64_t fl;
	uint32_t mute;

	if (!es)
		return -EINVAL;
	if (ctl >= ES_NCTL || !es_ctl_supported(es, ctl))
		return -ENOENT;
	if (left > 100 || right > 100)
		return -EINVAL;
	if (ctl == AUDIO_CTL_MIC_BOOST) {
		/* a switch: anything above the middle is on */
		left = right = (left >= 50 || right >= 50) ? 100 : 0;
	}
	spin_lock_irqsave(&es->mix_lock, &fl);
	es->vol_l[ctl] = left;
	es->vol_r[ctl] = right;
	mute = es->mute[ctl];
	spin_unlock_irqrestore(&es->mix_lock, fl);
	return es_hw_apply(es, ctl, left, right, mute);
}

static int es_get_mute(struct snd_card *card, uint32_t ctl, uint32_t *mute)
{
	struct es_card *es = es_from_card(card);
	uint64_t fl;

	if (!es || !mute)
		return -EINVAL;
	if (ctl >= ES_NCTL || !es_mute_supported(es, ctl))
		return -ENOENT;
	spin_lock_irqsave(&es->mix_lock, &fl);
	*mute = es->mute[ctl];
	spin_unlock_irqrestore(&es->mix_lock, fl);
	return 0;
}

static int es_set_mute(struct snd_card *card, uint32_t ctl, uint32_t mute)
{
	struct es_card *es = es_from_card(card);
	uint64_t fl;
	uint32_t l, r;

	if (!es)
		return -EINVAL;
	if (ctl >= ES_NCTL || !es_mute_supported(es, ctl))
		return -ENOENT;
	if (mute > 1)
		return -EINVAL;
	spin_lock_irqsave(&es->mix_lock, &fl);
	es->mute[ctl] = mute;
	l = es->vol_l[ctl];
	r = es->vol_r[ctl];
	spin_unlock_irqrestore(&es->mix_lock, fl);
	return es_hw_apply(es, ctl, l, r, mute);
}

/* The card has one analog output: the line-out jack (on most boards also
 * the speaker/headphone jack, through the codec's master amplifier).  It
 * has no jack sensing, so it is always reported present. */
static int es_get_outputs(struct snd_card *card, struct audio_outputs *o)
{
	struct es_card *es = es_from_card(card);
	struct audio_output_desc *d;
	uint64_t fl;

	if (!es || !o)
		return -EINVAL;
	kmemset(o, 0, sizeof(*o));
	o->count = 1;
	spin_lock_irqsave(&es->mix_lock, &fl);
	o->mode = es->output_mode;
	spin_unlock_irqrestore(&es->mix_lock, fl);
	o->selected = 0;
	d = &o->out[0];
	d->id = 0;
	d->type = AUDIO_OUT_LINE;
	d->flags = AUDIO_OUTF_FIXED;
	d->present = 1;
	d->active = 1;
	ksnprintf(d->name, sizeof(d->name), "Line Out");
	return 0;
}

static int es_set_output(struct snd_card *card, const struct audio_setoutput *s)
{
	struct es_card *es = es_from_card(card);
	uint64_t fl;

	if (!es || !s)
		return -EINVAL;
	switch (s->mode) {
	case AUDIO_OUTSEL_AUTO:
	case AUDIO_OUTSEL_ALL:
		break;
	case AUDIO_OUTSEL_ID:
		if (s->id != 0)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	spin_lock_irqsave(&es->mix_lock, &fl);
	es->output_mode = s->mode;
	spin_unlock_irqrestore(&es->mix_lock, fl);
	return 0;
}

static uint32_t es_num_inputs(const struct es_card *es)
{
	if (!es->codec_ok)
		return 0;
	return es_is1370(es) ? 3u : (uint32_t)ES_IN_COUNT;
}

static int es_get_inputs(struct snd_card *card, struct audio_outputs *o)
{
	struct es_card *es = es_from_card(card);
	uint32_t n, sel, mode;
	uint64_t fl;

	if (!es || !o)
		return -EINVAL;
	n = es_num_inputs(es);
	if (n == 0)
		return -ENOENT;
	spin_lock_irqsave(&es->mix_lock, &fl);
	sel = es->input;
	mode = es->input_mode;
	spin_unlock_irqrestore(&es->mix_lock, fl);

	kmemset(o, 0, sizeof(*o));
	o->count = n;
	o->mode = mode;
	o->selected = sel;
	for (uint32_t i = 0; i < n && i < AUDIO_MAX_OUTPUTS; i++) {
		struct audio_output_desc *d = &o->out[i];

		d->id = i;
		/* no input-specific type exists; line-level sources are
		 * described as line, the rest by name only */
		d->type = (i == ES_IN_LINE || i == ES_IN_CD || i == ES_IN_AUX) ?
				  AUDIO_OUT_LINE :
				  0;
		d->flags = AUDIO_OUTF_FIXED;
		d->present = 1;
		d->active = (mode == AUDIO_OUTSEL_ALL) ? 1 : (i == sel);
		ksnprintf(d->name, sizeof(d->name), "%s", es_input_names[i]);
	}
	return 0;
}

static int es_set_input(struct snd_card *card, const struct audio_setoutput *s)
{
	struct es_card *es = es_from_card(card);
	uint32_t n, in;
	uint64_t fl;
	int rc;

	if (!es || !s)
		return -EINVAL;
	n = es_num_inputs(es);
	if (n == 0)
		return -ENOENT;
	switch (s->mode) {
	case AUDIO_OUTSEL_AUTO:
		in = ES_IN_MIC;
		break;
	case AUDIO_OUTSEL_ID:
		if (s->id >= n)
			return -EINVAL;
		in = s->id;
		break;
	case AUDIO_OUTSEL_ALL:
		/* the AK4531 input mixer can sum sources; the AC'97 record
		 * selector picks exactly one */
		if (!es_is1370(es))
			return -EINVAL;
		in = ES_IN_MIC;
		break;
	default:
		return -EINVAL;
	}
	if (es_is1370(es))
		rc = es_ak_apply_input(es, in, s->mode == AUDIO_OUTSEL_ALL);
	else
		rc = es_ac97_apply_input(es, in);
	if (rc)
		return rc;
	spin_lock_irqsave(&es->mix_lock, &fl);
	es->input = in;
	es->input_mode = s->mode;
	spin_unlock_irqrestore(&es->mix_lock, fl);
	return 0;
}

/* ---- diagnostics ---------------------------------------------------------------- */

struct es_text {
	char *buf;
	size_t len;
	size_t pos;
};

static void es_tprintf(struct es_text *t, const char *fmt, ...)
{
	va_list ap;

	if (!t->buf || t->len == 0 || t->pos + 1 >= t->len)
		return;
	__builtin_va_start(ap, fmt);
	kvsnprintf(t->buf + t->pos, t->len - t->pos, fmt, ap);
	__builtin_va_end(ap);
	t->pos += kstrlen(t->buf + t->pos);
	if (t->pos >= t->len)
		t->pos = t->len - 1;
}

/* Print hundredths of a dB as "-12.50". */
static void es_tdb(struct es_text *t, int32_t db)
{
	uint32_t a = (uint32_t)(db < 0 ? -db : db);

	es_tprintf(t, "%s%u.%02u dB", db < 0 ? "-" : "", a / 100u, a % 100u);
}

static const char *es_mode_name(uint32_t mode)
{
	switch (mode) {
	case ES_MODE_8_MONO:
		return "U8 mono";
	case ES_MODE_8_STEREO:
		return "U8 stereo";
	case ES_MODE_16_MONO:
		return "S16 mono";
	default:
		return "S16 stereo";
	}
}

static void es_diag_summary(struct es_card *es, struct es_text *t)
{
	const pci_device_t *p = es->pci;

	es_tprintf(t, "%s (%s rev %02x)\n", es->model->name,
		   es_chip_names[es->chip], es->rev);
	es_tprintf(t, "  pci %02x:%02x.%x  id %04x:%04x  subsystem %04x:%04x\n",
		   p->bus, p->device, p->function, p->vendor_id, p->device_id,
		   es->subvendor, es->subdevice);
	es_tprintf(t, "  i/o 0x%x (%u bytes)  ", es->port, es->io_size);
	if (es->irq_polling)
		es_tprintf(t, "interrupt: none (timer polling)\n");
	else
		es_tprintf(t, "interrupt: gsi %d (level, shared)\n",
			   es->irq_gsi);
	es_tprintf(t, "  codec: %s", es->codec_ok ? es->codec_name : "none");
	if (!es_is1370(es) && es->codec_ok)
		es_tprintf(t, " (id %08x caps %04x ext %04x)", es->ac97_id,
			   es->ac97_caps, es->ac97_extid);
	es_tprintf(t, "\n  features:%s%s%s%s%s%s\n",
		   (es->flags & ESF_SRC) ? " src" : "",
		   (es->flags & ESF_AC97) ? " ac97" : "",
		   (es->flags & ESF_AK4531) ? " ak4531" : "",
		   (es->flags & ESF_SPDIF) ? " spdif(off)" : "",
		   (es->flags & ESF_REAR) ? " rear(off)" : "",
		   (es->flags & ESF_AC97_RESET) ? " link-reset" : "");
	es_tprintf(t,
		   "  interrupts: %llu ours, %llu foreign, %llu unknown\n",
		   (unsigned long long)es->irq_total,
		   (unsigned long long)es->irq_foreign,
		   (unsigned long long)es->irq_unknown);
	if (!es_is1370(es))
		es_tprintf(t,
			   "  SRC handshake %s, %u SRC timeouts, %u codec "
			   "timeouts\n",
			   es->src_state_ok ? "on" : "off", es->src_timeouts,
			   es->codec_timeouts);
	if (es->dead)
		es_tprintf(t, "  DEVICE NOT RESPONDING\n");

	for (uint32_t c = 0; c < ES_NCTL; c++) {
		int32_t dbl, dbr, mn, mx;
		uint32_t steps, l, r, m;
		uint64_t fl;

		if (!es_ctl_supported(es, c))
			continue;
		spin_lock_irqsave(&es->mix_lock, &fl);
		l = es->vol_l[c];
		r = es->vol_r[c];
		m = es->mute[c];
		spin_unlock_irqrestore(&es->mix_lock, fl);
		es_ctl_db(es, c, l, &dbl, &mn, &mx, &steps);
		es_ctl_db(es, c, r, &dbr, &mn, &mx, &steps);
		es_tprintf(t, "  control %u: %u/%u (", c, l, r);
		es_tdb(t, dbl);
		es_tprintf(t, " / ");
		es_tdb(t, dbr);
		es_tprintf(t, ")%s\n", m ? " muted" : "");
	}
	if (es->codec_ok)
		es_tprintf(t, "  capture input: %s%s\n",
			   es_input_names[es->input < ES_IN_COUNT ? es->input :
								   0],
			   es->input_mode == AUDIO_OUTSEL_ALL ? " (all)" : "");
}

static void es_diag_codec(struct es_card *es, struct es_text *t)
{
	if (!es->codec_ok) {
		es_tprintf(t, "no codec\n");
		return;
	}
	if (es_is1370(es)) {
		uint8_t shadow[AK4531_NREGS];
		uint64_t fl;

		spin_lock_irqsave(&es->src_lock, &fl);
		kmemcpy(shadow, es->ak, sizeof(shadow));
		spin_unlock_irqrestore(&es->src_lock, fl);
		es_tprintf(t, "AK4531 registers (write-only, shadow):\n");
		for (uint32_t r = 0; r < AK4531_NREGS; r++)
			es_tprintf(t, "  %02x: %02x%s", r, shadow[r],
				   (r % 8) == 7 ? "\n" : "");
		es_tprintf(t, "\n");
		return;
	}
	es_tprintf(t, "%s AC97 registers:\n", es->codec_name);
	for (uint32_t r = 0; r < 0x80; r += 2) {
		uint16_t v;

		if (es1371_codec_read(es, r, &v))
			es_tprintf(t, "  %02x: ----", r);
		else
			es_tprintf(t, "  %02x: %04x", r, v);
		if ((r % 16) == 14)
			es_tprintf(t, "\n");
	}
	{
		uint64_t fl;
		uint32_t w[12];
		static const uint8_t addrs[12] = {
			ES1371_SRC_DAC1 + 0, ES1371_SRC_DAC1 + 1,
			ES1371_SRC_DAC1 + 3, ES1371_SRC_DAC2 + 0,
			ES1371_SRC_DAC2 + 1, ES1371_SRC_DAC2 + 3,
			ES1371_SRC_ADC + 0,  ES1371_SRC_ADC + 1,
			ES1371_SRC_ADC + 3,  ES1371_SRC_VOL_DAC1,
			ES1371_SRC_VOL_DAC2, ES1371_SRC_VOL_ADC,
		};

		spin_lock_irqsave(&es->src_lock, &fl);
		for (int i = 0; i < 12; i++)
			w[i] = es1371_src_read(es, addrs[i]);
		spin_unlock_irqrestore(&es->src_lock, fl);
		es_tprintf(t, "SRC RAM:\n");
		for (int i = 0; i < 12; i++)
			es_tprintf(t, "  [%02x] %04x%s", addrs[i], w[i],
				   (i % 3) == 2 ? "\n" : "");
	}
}

static void es_diag_streams(struct es_card *es, struct es_text *t)
{
	uint32_t ctrl, sctrl, status, cnt[ES_NUM_CH], fsz[ES_NUM_CH];
	uint64_t fl;

	spin_lock_irqsave(&es->reg_lock, &fl);
	ctrl = es->ctrl;
	sctrl = es->sctrl;
	status = es_rd32(es, ES_REG_STATUS);
	for (int i = 0; i < ES_NUM_CH; i++) {
		const struct es_chan_desc *d = &es_chan_desc[i];

		es_wr32(es, ES_REG_MEM_PAGE, d->page);
		fsz[i] = es_rd32(es, d->size_reg);
		cnt[i] = es_rd32(es, d->count_reg);
	}
	spin_unlock_irqrestore(&es->reg_lock, fl);

	es_tprintf(t, "CONTROL %08x  SERIAL %08x  STATUS %08x\n", ctrl, sctrl,
		   status);
	for (int i = 0; i < ES_NUM_CH; i++) {
		struct es_chan *ch = &es->ch[i];

		es_tprintf(t, "%s: %s%s", ch->d->name,
			   ch->opened ? "open" : "closed",
			   ch->running ? ", running" : "");
		if (ch->configured)
			es_tprintf(t,
				   ", %s %u Hz, buffer %u bytes, period %u "
				   "frames, dma 0x%llx",
				   es_mode_name(ch->mode), ch->rate,
				   ch->buffer_bytes, ch->period_frames,
				   (unsigned long long)ch->dma_phys);
		es_tprintf(t,
			   "\n   position %u bytes, count %u/%u, %llu irqs, "
			   "%llu starts\n",
			   ES_FSIZE_CURRENT_BYTES(fsz[i]),
			   ES_COUNT_CURRENT(cnt[i]), cnt[i] & 0xffffu,
			   (unsigned long long)ch->irqs,
			   (unsigned long long)ch->starts);
	}
	if (es_is1370(es))
		es_tprintf(t, "shared DAC2/ADC clock: divider %u (%u Hz), "
			   "users %x\n",
			   es->pclk_div, ES1370_PCLK / (es->pclk_div + 2u),
			   es->pclk_users);
}

static long es_diag(struct snd_card *card, uint32_t what, char *buf,
		    size_t len)
{
	struct es_card *es = es_from_card(card);
	struct es_text t = { buf, len, 0 };

	if (!es || !buf || len == 0)
		return -EINVAL;
	buf[0] = '\0';
	switch (what) {
	case AUDIO_DIAG_SUMMARY:
		es_diag_summary(es, &t);
		break;
	case AUDIO_DIAG_CODECS:
		es_diag_summary(es, &t);
		es_diag_codec(es, &t);
		break;
	case AUDIO_DIAG_STREAMS:
		es_diag_streams(es, &t);
		break;
	default:
		return -EINVAL;
	}
	return (long)t.pos;
}

static const struct snd_ctl_ops es_ctl_ops = {
	.get_volume = es_get_volume,
	.set_volume = es_set_volume,
	.get_mute = es_get_mute,
	.set_mute = es_set_mute,
	.get_outputs = es_get_outputs,
	.set_output = es_set_output,
	.get_inputs = es_get_inputs,
	.set_input = es_set_input,
	.diag = es_diag,
};

/* ---- ES1370 shared clock ---------------------------------------------------------- */

/* DAC1's fixed rates, by WTSRSEL value. */
static const uint32_t es1370_dac1_rates[4] = { 5512, 11025, 22050, 44100 };

static int es1370_dac1_sel(uint32_t rate)
{
	for (int i = 0; i < 4; i++)
		if (es1370_dac1_rates[i] == rate)
			return i;
	return -1;
}

/* The divider for an exact rate on the shared clock, or 0. */
static uint32_t es1370_pclk_div(uint32_t rate)
{
	uint32_t div;

	if (rate == 0 || ES1370_PCLK % rate)
		return 0;
	div = ES1370_PCLK / rate;
	if (div < 2)
		return 0;
	div -= 2;
	if (div < ES1370_PCLKDIV_MIN || div > ES1370_PCLKDIV_MAX)
		return 0;
	return div;
}

/* DAC2 and the ADC share one clock.  A channel claims it at hw_params; a
 * second channel may only join at the same rate. */
static int es1370_pclk_claim(struct es_card *es, int ch, uint32_t rate)
{
	uint32_t div = es1370_pclk_div(rate);
	uint32_t others;
	uint64_t fl;
	int rc = 0;

	if (!div)
		return -EINVAL;
	spin_lock_irqsave(&es->reg_lock, &fl);
	others = es->pclk_users & ~(1u << ch);
	if (others && es->pclk_div != div) {
		rc = -EBUSY;
	} else {
		es->pclk_users |= 1u << ch;
		if (es->pclk_div != div) {
			es->pclk_div = div;
			es->ctrl = (es->ctrl & ~ES1370_CTRL_PCLKDIV_MASK) |
				   ES1370_CTRL_PCLKDIV(div);
			es_wr32(es, ES_REG_CONTROL, es->ctrl);
		}
	}
	spin_unlock_irqrestore(&es->reg_lock, fl);
	return rc;
}

static void es1370_pclk_release(struct es_card *es, int ch)
{
	uint64_t fl;

	spin_lock_irqsave(&es->reg_lock, &fl);
	es->pclk_users &= ~(1u << ch);
	spin_unlock_irqrestore(&es->reg_lock, fl);
}

/* ---- PCM -------------------------------------------------------------------------- */

static struct es_chan *es_chan_of(struct snd_pcm_substream *ss)
{
	struct es_card *es;

	if (!ss || !ss->pcm)
		return NULL;
	es = (struct es_card *)ss->pcm->private_data;
	if (!es)
		return NULL;
	if (ss->pcm == es->pcm_aux)
		return ss->direction == AUDIO_DIR_PLAYBACK ? &es->ch[ES_CH_DAC1] :
							     NULL;
	if (ss->pcm == es->pcm_main)
		return ss->direction == AUDIO_DIR_PLAYBACK ?
			       &es->ch[ES_CH_DAC2] :
			       &es->ch[ES_CH_ADC];
	return NULL;
}

/* Stop a channel's DMA.  reg_lock held. */
static void es_chan_halt_locked(struct es_card *es, struct es_chan *ch)
{
	es->ctrl &= ~ch->d->ctrl_en;
	es_wr32(es, ES_REG_CONTROL, es->ctrl);
	if (ch->d->pause) {
		es->sctrl &= ~ch->d->pause;
		es_wr32(es, ES_REG_SERIAL, es->sctrl);
	}
	ch->running = 0;
}

static void es_poll_timer_fn(hrtimer_t *t)
{
	struct es_chan *ch = (struct es_chan *)t->arg;
	uint64_t now, next;

	if (!ch || !ch->running || ch->es->dead)
		return;
	snd_pcm_period_elapsed(ch->ss);
	if (!ch->running)
		return;
	now = hrtimer_now_ns();
	next = t->expires_ns + ch->period_ns;
	if (next <= now) /* fell behind: do not try to catch up in a burst */
		next = now + ch->period_ns;
	hrtimer_start(t, next);
}

static int es_pcm_open(struct snd_pcm_substream *ss)
{
	struct es_chan *ch = es_chan_of(ss);
	struct es_card *es;
	uint64_t fl;

	if (!ch)
		return -ENODEV;
	es = ch->es;
	if (es->dead)
		return -ENODEV;
	spin_lock_irqsave(&es->reg_lock, &fl);
	if (ch->opened) {
		spin_unlock_irqrestore(&es->reg_lock, fl);
		return -EBUSY;
	}
	ch->opened = 1;
	ch->configured = 0;
	ch->running = 0;
	ch->last_pos = 0;
	spin_unlock_irqrestore(&es->reg_lock, fl);
	ss->private_data = ch;
	return 0;
}

static void es_pcm_quiesce(struct es_chan *ch)
{
	struct es_card *es = ch->es;
	uint64_t fl;

	spin_lock_irqsave(&es->reg_lock, &fl);
	es_chan_halt_locked(es, ch);
	/* interrupt enable off too, so a stale pending bit cannot fire */
	es->sctrl &= ~ch->d->int_en;
	es_wr32(es, ES_REG_SERIAL, es->sctrl);
	spin_unlock_irqrestore(&es->reg_lock, fl);
	hrtimer_cancel(&ch->poll_timer);
}

static void es_pcm_close(struct snd_pcm_substream *ss)
{
	struct es_chan *ch = es_chan_of(ss);
	struct es_card *es;
	uint64_t fl;

	if (!ch)
		return;
	es = ch->es;
	es_pcm_quiesce(ch);
	if (es_is1370(es) && ch->id != ES_CH_DAC1)
		es1370_pclk_release(es, ch->id);
	spin_lock_irqsave(&es->reg_lock, &fl);
	ch->opened = 0;
	ch->configured = 0;
	spin_unlock_irqrestore(&es->reg_lock, fl);
}

static int es_pcm_hw_params(struct snd_pcm_substream *ss)
{
	struct es_chan *ch = es_chan_of(ss);
	struct es_card *es;
	const struct audio_params *p;
	uint32_t mode, fb, period_frames;
	int rc;

	if (!ch)
		return -ENODEV;
	es = ch->es;
	if (es->dead)
		return -ENODEV;
	p = &ss->params;

	/* format and channels: 8-bit unsigned or 16-bit signed, 1 or 2 */
	if (p->format == AUDIO_FMT_U8)
		mode = 0;
	else if (p->format == AUDIO_FMT_S16_LE)
		mode = 2;
	else
		return -EINVAL;
	if (p->channels == 2)
		mode |= 1;
	else if (p->channels != 1)
		return -EINVAL;
	fb = (mode & 2 ? 2u : 1u) * p->channels;
	if (ss->frame_bytes && ss->frame_bytes != fb)
		return -EINVAL;

	/* the ring: the frame size register counts longwords in 16 bits */
	if (ss->buffer_bytes == 0 || (ss->buffer_bytes & 3u) ||
	    ss->buffer_bytes > ES_FSIZE_MAX_BYTES ||
	    ss->buffer_bytes % fb)
		return -EINVAL;
	if (ss->period_bytes == 0 || ss->period_bytes % fb ||
	    ss->period_bytes > ss->buffer_bytes)
		return -EINVAL;
	period_frames = ss->period_bytes / fb;
	if (period_frames == 0 || period_frames > 0x10000u)
		return -EINVAL;
	/* 32-bit bus master, longword aligned */
	if ((ss->dma_phys & 3u) ||
	    ss->dma_phys + ss->buffer_bytes > 0x100000000ull ||
	    ss->dma_phys == 0)
		return -EINVAL;

	/* rate */
	if (es_is1370(es)) {
		if (ch->id == ES_CH_DAC1) {
			if (es1370_dac1_sel(p->rate) < 0)
				return -EINVAL;
		} else {
			rc = es1370_pclk_claim(es, ch->id, p->rate);
			if (rc)
				return rc;
		}
	} else if (p->rate < 4000 || p->rate > 48000) {
		return -EINVAL;
	}

	ch->mode = mode;
	ch->rate = p->rate;
	ch->frame_bytes = fb;
	ch->buffer_bytes = ss->buffer_bytes;
	ch->period_bytes = ss->period_bytes;
	ch->period_frames = period_frames;
	ch->dma_phys = ss->dma_phys;
	ch->period_ns = ((uint64_t)period_frames * 1000000000ull) / p->rate;
	if (ch->period_ns < 100000ull)
		ch->period_ns = 100000ull;
	ch->configured = 1;
	return 0;
}

static int es_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct es_chan *ch = es_chan_of(ss);

	if (!ch)
		return -ENODEV;
	es_pcm_quiesce(ch);
	if (es_is1370(ch->es) && ch->id != ES_CH_DAC1)
		es1370_pclk_release(ch->es, ch->id);
	ch->configured = 0;
	return 0;
}


/* Program the channel to the start of its ring, stopped. */
static int es_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct es_chan *ch = es_chan_of(ss);
	const struct es_chan_desc *d;
	struct es_card *es;
	uint64_t fl;
	uint32_t s;

	if (!ch)
		return -ENODEV;
	es = ch->es;
	if (!ch->configured)
		return -EINVAL;
	if (!es_check_alive(es))
		return -ENODEV;
	d = ch->d;

	hrtimer_cancel(&ch->poll_timer);

	/* the converter first, while the channel is certainly stopped */
	if (!es_is1370(es)) {
		spin_lock_irqsave(&es->reg_lock, &fl);
		es_chan_halt_locked(es, ch);
		spin_unlock_irqrestore(&es->reg_lock, fl);
		if (ch->id == ES_CH_ADC)
			es1371_adc_rate(es, ch->rate);
		else
			es1371_dac_rate(es, ch->id, ch->rate);
	}

	spin_lock_irqsave(&es->reg_lock, &fl);
	es_chan_halt_locked(es, ch);

	/* frame address and size; writing the size also zeroes the
	 * current-position half of the register */
	es_wr32(es, ES_REG_MEM_PAGE, d->page);
	es_wr32(es, d->frame_reg, (uint32_t)ch->dma_phys);
	es_wr32(es, d->size_reg, (ch->buffer_bytes >> 2) - 1u);

	/* serial control: loop mode, not paused, the sample format, the
	 * period interrupt on */
	s = es->sctrl;
	s &= ~(d->loop_sel | d->pause |
	       (ES_SER_MODE_MASK << d->mode_shift));
	s |= d->int_en | (ch->mode << d->mode_shift);
	if (ch->id == ES_CH_DAC2) {
		/* DAC2 needs its fetch increments: one longword step per
		 * frame end for 8-bit data, two for 16-bit */
		s &= ~(ES_SER_P2_END_INC_MASK | ES_SER_P2_ST_INC_MASK |
		       ES_SER_P2_DAC_SEN);
		s |= ES_SER_P2_END_INC((ch->mode & 2) ? 2 : 1) |
		     ES_SER_P2_ST_INC(0);
	} else if (ch->id == ES_CH_DAC1) {
		s &= ~ES_SER_P1_SCT_RLD;
	}
	es->sctrl = s;
	es_wr32(es, ES_REG_SERIAL, es->sctrl);

	/* frames per interrupt, minus one */
	es_wr32(es, d->count_reg, ch->period_frames - 1u);

	if (es_is1370(es) && ch->id == ES_CH_DAC1) {
		int sel = es1370_dac1_sel(ch->rate);

		es->ctrl = (es->ctrl & ~ES1370_CTRL_WTSRSEL_MASK) |
			   ES1370_CTRL_WTSRSEL(sel < 0 ? 3 : sel);
	} else if (es_is1370(es)) {
		es->ctrl = (es->ctrl & ~ES1370_CTRL_PCLKDIV_MASK) |
			   ES1370_CTRL_PCLKDIV(es->pclk_div);
	}
	es_wr32(es, ES_REG_CONTROL, es->ctrl);
	ch->last_pos = 0;
	spin_unlock_irqrestore(&es->reg_lock, fl);
	return 0;
}

/* Called by the core with the substream lock held and interrupts off. */
static int es_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct es_chan *ch = es_chan_of(ss);
	const struct es_chan_desc *d;
	struct es_card *es;
	uint64_t fl;
	int start_timer = 0;

	if (!ch)
		return -ENODEV;
	es = ch->es;
	d = ch->d;
	if (es->dead && (cmd == SND_TRIGGER_START || cmd == SND_TRIGGER_RESUME))
		return -ENODEV;

	spin_lock_irqsave(&es->reg_lock, &fl);
	switch (cmd) {
	case SND_TRIGGER_START:
		if (!ch->configured) {
			spin_unlock_irqrestore(&es->reg_lock, fl);
			return -EINVAL;
		}
		es->sctrl |= d->int_en;
		es->sctrl &= ~d->pause;
		es_wr32(es, ES_REG_SERIAL, es->sctrl);
		es->ctrl |= d->ctrl_en;
		es_wr32(es, ES_REG_CONTROL, es->ctrl);
		ch->running = 1;
		ch->starts++;
		start_timer = es->irq_polling;
		break;
	case SND_TRIGGER_STOP:
		es_chan_halt_locked(es, ch);
		break;
	case SND_TRIGGER_PAUSE:
		if (d->pause) {
			es->sctrl |= d->pause;
			es_wr32(es, ES_REG_SERIAL, es->sctrl);
		} else {
			/* the ADC has no pause bit; disabling it keeps the
			 * position, the frame registers are not rewritten */
			es->ctrl &= ~d->ctrl_en;
			es_wr32(es, ES_REG_CONTROL, es->ctrl);
		}
		ch->running = 0;
		break;
	case SND_TRIGGER_RESUME:
		if (d->pause) {
			es->sctrl &= ~d->pause;
			es_wr32(es, ES_REG_SERIAL, es->sctrl);
		} else {
			es->ctrl |= d->ctrl_en;
			es_wr32(es, ES_REG_CONTROL, es->ctrl);
		}
		ch->running = 1;
		start_timer = es->irq_polling;
		break;
	default:
		spin_unlock_irqrestore(&es->reg_lock, fl);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&es->reg_lock, fl);

	/* The timer callback takes the substream lock through the core, so
	 * it is only ever armed here, never cancelled under that lock: a
	 * stopped channel's callback sees running == 0 and does nothing. */
	if (start_timer)
		hrtimer_start_rel(&ch->poll_timer, ch->period_ns);
	return 0;
}

/* The DMA position, from the current-longword half of the frame size
 * register.  Called with the substream lock held, interrupts off. */
static uint32_t es_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct es_chan *ch = es_chan_of(ss);
	struct es_card *es;
	uint64_t fl;
	uint32_t v, pos;

	if (!ch || !ch->configured || ch->buffer_bytes == 0)
		return 0;
	es = ch->es;
	if (es->dead)
		return ch->last_pos;
	spin_lock_irqsave(&es->reg_lock, &fl);
	es_wr32(es, ES_REG_MEM_PAGE, ch->d->page);
	v = es_rd32(es, ch->d->size_reg);
	spin_unlock_irqrestore(&es->reg_lock, fl);
	if (v == 0xffffffffu)
		return ch->last_pos;
	pos = ES_FSIZE_CURRENT_BYTES(v);
	if (pos >= ch->buffer_bytes)
		pos = 0; /* wrapping right now */
	/* whole frames only */
	pos -= pos % ch->frame_bytes;
	ch->last_pos = pos;
	return pos;
}

static const struct snd_pcm_ops es_pcm_ops = {
	.open = es_pcm_open,
	.close = es_pcm_close,
	.hw_params = es_pcm_hw_params,
	.hw_free = es_pcm_hw_free,
	.prepare = es_pcm_prepare,
	.trigger = es_pcm_trigger,
	.pointer = es_pcm_pointer,
	.delay = NULL,
};

static void es_fill_hw(const struct es_card *es, int ch, struct snd_pcm_hw *hw)
{
	kmemset(hw, 0, sizeof(*hw));
	hw->formats = AUDIO_FMT_BIT(AUDIO_FMT_U8) |
		      AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
	if (es_is1370(es)) {
		/* exact rates of the fixed DAC1 clocks and the 1411200 Hz
		 * shared clock */
		hw->rates = AUDIO_RATE_11025 | AUDIO_RATE_22050 |
			    AUDIO_RATE_44100;
	} else {
		hw->rates = AUDIO_RATE_8000 | AUDIO_RATE_11025 |
			    AUDIO_RATE_16000 | AUDIO_RATE_22050 |
			    AUDIO_RATE_32000 | AUDIO_RATE_44100 |
			    AUDIO_RATE_48000;
	}
	(void)ch;
	hw->channels_min = 1;
	hw->channels_max = 2;
	hw->period_bytes_min = 64;
	/* 65536 frames per interrupt at most; 64 KiB keeps that true for
	 * every format */
	hw->period_bytes_max = 65536;
	hw->periods_min = 2;
	hw->periods_max = 1024;
	hw->buffer_bytes_max = ES_FSIZE_MAX_BYTES;
	hw->buffer_align = 4;
	hw->period_align = 4;
	hw->dma32 = 1;
	hw->dma_coherent = 1; /* PCI bus masters snoop on this platform */
}

/* ---- interrupt -------------------------------------------------------------------- */

static int es_irq(void *arg)
{
	struct es_card *es = (struct es_card *)arg;
	uint32_t status, ack, sctrl;
	uint32_t elapsed = 0;
	uint64_t fl;

	if (!es || es->dead)
		return 0;
	status = es_rd32(es, ES_REG_STATUS);
	if (status == 0xffffffffu) {
		/* gone: never claim a shared line on all-ones */
		return 0;
	}
	if (!(status & ES_STAT_INTR)) {
		es->irq_foreign++;
		return 0;
	}

	spin_lock_irqsave(&es->reg_lock, &fl);
	es->irq_total++;
	ack = status & (ES_STAT_DAC1 | ES_STAT_DAC2 | ES_STAT_ADC);
	/* Acknowledge by dropping and restoring each pending channel's
	 * interrupt enable. */
	sctrl = es->sctrl;
	for (int i = 0; i < ES_NUM_CH; i++) {
		const struct es_chan_desc *d = &es_chan_desc[i];

		if (ack & d->stat) {
			sctrl &= ~d->int_en;
			if (es->ch[i].running)
				elapsed |= 1u << i;
		}
	}
	if (ack) {
		es_wr32(es, ES_REG_SERIAL, sctrl);
		es_wr32(es, ES_REG_SERIAL, es->sctrl);
	}
	if (status & ES_STAT_UART) {
		/* The UART is never enabled; drain and silence it. */
		for (int i = 0; i < 16 &&
				(es_rd8(es, ES_REG_UART_STATUS) & ES_UART_RXRDY);
		     i++)
			(void)es_rd8(es, ES_REG_UART_DATA);
		es_wr8(es, ES_REG_UART_CONTROL, 0);
	}
	if (!ack && !(status & ES_STAT_UART)) {
		/* INTR with no source we know: count it; if it keeps coming
		 * the chip is confused, so switch every source off rather
		 * than let a level-triggered line storm. */
		es->irq_unknown++;
		if (++es->irq_unknown_run >= 1000) {
			es->sctrl &= ~(ES_SER_P1_INT_EN | ES_SER_P2_INT_EN |
				       ES_SER_R1_INT_EN);
			es_wr32(es, ES_REG_SERIAL, es->sctrl);
			es->ctrl &= ~(ES_CTRL_CCB_INTRM | ES_CTRL_UART_EN);
			if (!es_is1370(es))
				es->ctrl &= ~ES1371_CTRL_PWR_INTRM;
			es_wr32(es, ES_REG_CONTROL, es->ctrl);
			es->irq_unknown_run = 0;
		}
	} else {
		es->irq_unknown_run = 0;
	}
	spin_unlock_irqrestore(&es->reg_lock, fl);

	/* Outside reg_lock: the core takes the substream lock and calls back
	 * into es_pcm_pointer, which takes reg_lock. */
	for (int i = 0; i < ES_NUM_CH; i++) {
		if (elapsed & (1u << i)) {
			es->ch[i].irqs++;
			snd_pcm_period_elapsed(es->ch[i].ss);
		}
	}
	return 1;
}

/* The global system interrupt of the device's INTx pin: the ACPI routing
 * table of the root bus, walking up through PCI-to-PCI bridges with the
 * standard pin rotation, then the firmware's interrupt line register. */
static int es_find_gsi(const pci_device_t *pci)
{
	uint8_t pin = pci->interrupt_pin;

	if (pin >= 1 && pin <= 4) {
		uint8_t p = (uint8_t)(pin - 1);
		uint8_t dev = pci->device;
		uint8_t bus = pci->bus;
		uint32_t gsi = 0;

		for (int depth = 0; bus != 0 && depth < 8; depth++) {
			const pci_device_t *br = pci_find_bridge_for_bus(bus);

			if (!br)
				break;
			p = (uint8_t)((p + dev) % 4);
			dev = br->device;
			bus = br->bus;
		}
		if (bus == 0 &&
		    acpi_pci_lookup_irq("\\\\_SB_.PCI0", dev, p, &gsi) == 0 &&
		    gsi < 224)
			return (int)gsi;
	}
	if (pci->interrupt_line != 0xff && pci->interrupt_line != 0 &&
	    pci->interrupt_line <= 23)
		return pci->interrupt_line;
	return -1;
}

static void es_irq_setup(struct es_card *es)
{
	uint16_t cmd = pci_cfg_read16(es->pci, 0x04);
	int gsi, rc;

	if (cmd & PCI_CMD_INTX_DISABLE)
		pci_cfg_write16(es->pci, 0x04,
				(uint16_t)(cmd & ~PCI_CMD_INTX_DISABLE));
	es->irq_gsi = -1;
	es->irq_polling = 1;
	gsi = es_find_gsi(es->pci);
	if (gsi < 0) {
		snd_info(ES_DRV, "%s: no interrupt line, using timers\n",
			 es->chip_label);
		return;
	}
	rc = irq_request_gsi(gsi, es_irq, es, ES_DRV, 1, 1);
	if (rc) {
		snd_info(ES_DRV,
			 "%s: GSI %d unavailable (%d), using timers\n",
			 es->chip_label, gsi, rc);
		return;
	}
	es->irq_gsi = gsi;
	es->irq_polling = 0;
}

/* ---- chip bring-up ---------------------------------------------------------------- */

static void es1370_chip_init(struct es_card *es)
{
	/* codec port on, DAC2/ADC clock at 22050 Hz until a stream asks */
	es->pclk_div = es1370_pclk_div(22050);
	es->pclk_users = 0;
	es->ctrl = ES1370_CTRL_CDC_EN | ES1370_CTRL_PCLKDIV(es->pclk_div) |
		   ES1370_CTRL_WTSRSEL(2);
	es->sctrl = 0;
	es_wr32(es, ES_REG_CONTROL, es->ctrl);
	es_wr32(es, ES_REG_SERIAL, es->sctrl);

	/* The ADC can overrun into whatever the phantom frame register
	 * points at; aim it at a scratch page of our own. */
	es_wr32(es, ES_REG_MEM_PAGE, ES_PAGE_ADC);
	es_wr32(es, ES_REG_PHANTOM_FRAME, (uint32_t)es->phantom_phys);
	es_wr32(es, ES_REG_PHANTOM_COUNT, 0);

	es_wr8(es, ES_REG_UART_CONTROL, 0);
	es_wr8(es, ES_REG_UART_RES, 0);
}

static int es_has_amp_gpio(const struct es_card *es)
{
	for (size_t i = 0;
	     i < sizeof(es_amp_gpio_boards) / sizeof(es_amp_gpio_boards[0]);
	     i++) {
		if (es_amp_gpio_boards[i].vendor == es->subvendor &&
		    es_amp_gpio_boards[i].device == es->subdevice) {
			snd_info(ES_DRV, "%s: %s, amplifier on GPIO 0\n",
				 es->chip_label, es_amp_gpio_boards[i].what);
			return 1;
		}
	}
	return 0;
}

static void es1371_chip_init(struct es_card *es)
{
	es->ctrl = 0;
	es->sctrl = 0;
	es->cssr = 0;
	if (es_has_amp_gpio(es))
		es->ctrl |= ES1371_CTRL_GPIO_OUT(1);
	if (es->flags & ESF_AC97_RESET)
		es->cssr |= ES1371_STAT_AC97_RST;

	es_wr32(es, ES_REG_CONTROL, es->ctrl);
	es_wr32(es, ES_REG_SERIAL, es->sctrl);
	es_wr32(es, ES1371_REG_LEGACY, 0); /* no SoundBlaster traps */

	if (es->flags & ESF_AC97_RESET) {
		/* the later parts hold the AC-link in reset until told; the
		 * codec then needs a moment to wake up */
		es_wr32(es, ES_REG_STATUS, es->cssr);
		snd_mdelay(20);
	}

	/* warm reset: pulse SYNC to start the codec's bit clock */
	es_wr32(es, ES_REG_CONTROL, es->ctrl | ES1371_CTRL_SYNC_RES);
	(void)es_rd32(es, ES_REG_CONTROL);
	snd_udelay(20);
	es_wr32(es, ES_REG_CONTROL, es->ctrl);

	es1371_src_init(es);

	/* cold reset of the codec through its reset register; done directly,
	 * the codec cannot answer the handshake yet */
	es_wr32(es, ES1371_REG_CODEC, ES1371_CODEC_ADDR(AC97_RESET) |
					      es1371_codec_flag(es));

	es_wr8(es, ES_REG_UART_CONTROL, 0);
	es_wr8(es, ES_REG_UART_RES, 0);
	es_wr32(es, ES_REG_STATUS, es->cssr);
}

/* ---- probe -------------------------------------------------------------------------- */

static const struct es_model *es_match(const pci_device_t *p, uint8_t rev)
{
	for (size_t i = 0; i < sizeof(es_models) / sizeof(es_models[0]); i++) {
		const struct es_model *m = &es_models[i];

		if (m->vendor != p->vendor_id || m->device != p->device_id)
			continue;
		if (m->rev != ES_REV_ANY && m->rev != rev)
			continue;
		return m;
	}
	return NULL;
}

static void es_release(struct es_card *es)
{
	if (!es)
		return;
	for (int i = 0; i < ES_NUM_CH; i++)
		hrtimer_cancel(&es->ch[i].poll_timer);
	if (es->port) {
		/* everything off: DMA, interrupts */
		es_wr32(es, ES_REG_SERIAL, 0);
		es_wr32(es, ES_REG_CONTROL,
			es_is1370(es) ? ES1370_CTRL_CDC_EN : 0);
	}
	if (!es->irq_polling && es->irq_gsi >= 0)
		irq_free(32 + es->irq_gsi, es_irq, es);
	if (es->card)
		snd_card_free(es->card);
	if (es->phantom)
		snd_dma_free(es->phantom, 4096);
	kfree(es);
}

static int es_probe_one(const pci_device_t *pci, const struct es_model *m,
			uint8_t rev)
{
	struct snd_pcm_hw play, cap, aux;
	struct pci_bar bar;
	struct es_card *es;
	uint16_t cmd;
	int rc;

	if (pci_bar_decode(pci, 0, &bar) != 0 || !(bar.flags & PCI_BAR_IO) ||
	    bar.base == 0 || bar.base > 0xffffu - ES_IO_SIZE ||
	    (bar.size && bar.size < ES_IO_SIZE)) {
		snd_info(ES_DRV, "%02x:%02x.%x: no usable I/O BAR\n", pci->bus,
			 pci->device, pci->function);
		return -ENODEV;
	}

	es = (struct es_card *)kalloc(sizeof(*es));
	if (!es)
		return -ENOMEM;
	kmemset(es, 0, sizeof(*es));
	es->pci = pci;
	es->model = m;
	es->chip = m->chip;
	es->flags = m->flags;
	es->rev = rev;
	es->subvendor = pci_cfg_read16(pci, 0x2c);
	es->subdevice = pci_cfg_read16(pci, 0x2e);
	es->port = (uint16_t)bar.base;
	es->io_size = (uint32_t)(bar.size ? bar.size : ES_IO_SIZE);
	es->irq_gsi = -1;
	es->irq_polling = 1;
	es->src_state_ok = 1;
	es->master_max = 31;
	spinlock_init(&es->reg_lock, "es1371_reg");
	spinlock_init(&es->src_lock, "es1371_src");
	spinlock_init(&es->mix_lock, "es1371_mix");
	ksnprintf(es->chip_label, sizeof(es->chip_label), "%s@%02x:%02x.%x",
		  es_chip_names[es->chip], pci->bus, pci->device,
		  pci->function);
	for (int i = 0; i < ES_NUM_CH; i++) {
		es->ch[i].es = es;
		es->ch[i].id = i;
		es->ch[i].d = &es_chan_desc[i];
		hrtimer_init(&es->ch[i].poll_timer, es_poll_timer_fn,
			     &es->ch[i]);
	}

	/* I/O decode and bus mastering on */
	cmd = pci_cfg_read16(pci, 0x04);
	cmd |= 0x0001 | 0x0004;
	pci_cfg_write16(pci, 0x04, cmd);
	/* a latency timer of 0 starves the bus master on old chipsets */
	if (pci_cfg_read8(pci, 0x0d) < 32)
		pci_cfg_write8(pci, 0x0d, 64);

	if (!es_check_alive(es)) {
		es->port = 0;
		es_release(es);
		return -ENODEV;
	}

	snd_info(ES_DRV, "%s: %s rev %02x, i/o 0x%x, subsystem %04x:%04x\n",
		 es->chip_label, m->name, rev, es->port, es->subvendor,
		 es->subdevice);

	/* quiesce whatever the firmware left running */
	es_wr32(es, ES_REG_SERIAL, 0);
	es_wr32(es, ES_REG_CONTROL, 0);

	if (es_is1370(es)) {
		es->phantom = snd_dma_alloc(4096, 1, 1, &es->phantom_phys);
		if (!es->phantom) {
			es_release(es);
			return -ENOMEM;
		}
		es1370_chip_init(es);
		es1370_ak_init(es);
	} else {
		es1371_chip_init(es);
		rc = es1371_ac97_init(es);
		if (rc) {
			/* Without a codec nothing is audible, but the DMA side
			 * still works; keep the card so the failure is visible
			 * through the diagnostics. */
			es->codec_ok = 0;
			ksnprintf(es->codec_name, sizeof(es->codec_name),
				  "none");
		}
	}

	rc = snd_card_new(ES_DRV, pci, &es->card);
	if (rc || !es->card) {
		snd_info(ES_DRV, "%s: snd_card_new failed (%d)\n",
			 es->chip_label, rc);
		es->card = NULL;
		es_release(es);
		return rc ? rc : -ENOMEM;
	}

	es_mixer_defaults(es);
	es_irq_setup(es);

	/* PCM 0: DAC2 + ADC */
	es_fill_hw(es, ES_CH_DAC2, &play);
	es_fill_hw(es, ES_CH_ADC, &cap);
	rc = snd_pcm_new(es->card, "AudioPCI DAC2/ADC", &es_pcm_ops, &play,
			 &cap, es, &es->pcm_main);
	if (rc || !es->pcm_main) {
		snd_info(ES_DRV, "%s: snd_pcm_new failed (%d)\n",
			 es->chip_label, rc);
		es_release(es);
		return rc ? rc : -ENOMEM;
	}
	es->ch[ES_CH_DAC2].ss = &es->pcm_main->sub[AUDIO_DIR_PLAYBACK];
	es->ch[ES_CH_ADC].ss = &es->pcm_main->sub[AUDIO_DIR_CAPTURE];

	/* PCM 1: DAC1, a second hardware-mixed playback channel */
	es_fill_hw(es, ES_CH_DAC1, &aux);
	rc = snd_pcm_new(es->card, "AudioPCI DAC1", &es_pcm_ops, &aux, NULL,
			 es, &es->pcm_aux);
	if (rc || !es->pcm_aux) {
		es->pcm_aux = NULL;
	} else {
		es->ch[ES_CH_DAC1].ss = &es->pcm_aux->sub[AUDIO_DIR_PLAYBACK];
	}

	es->card->private_data = es;
	es->card->ctl = &es_ctl_ops;
	es->card->num_outputs = 1;
	es->card->num_inputs = es_num_inputs(es);
	if (es->codec_ok)
		es->card->info_flags |= AUDIO_INFO_HW_VOLUME;
	ksnprintf(es->card->name, sizeof(es->card->name), "%s", m->name);
	ksnprintf(es->card->driver, sizeof(es->card->driver), "%s", ES_DRV);
	ksnprintf(es->card->codec, sizeof(es->card->codec), "%s",
		  es->codec_ok ? es->codec_name : "none");
	ksnprintf(es->card->bus, sizeof(es->card->bus),
		  "pci:0000:%02x:%02x.%x", pci->bus, pci->device,
		  pci->function);

	rc = snd_card_register(es->card);
	if (rc) {
		snd_info(ES_DRV, "%s: snd_card_register failed (%d)\n",
			 es->chip_label, rc);
		es_release(es);
		return rc;
	}

	if (es->irq_polling)
		snd_info(ES_DRV, "%s: card %d, codec %s, timer-driven\n",
			 es->chip_label, es->card->index, es->card->codec);
	else
		snd_info(ES_DRV, "%s: card %d, codec %s, GSI %d\n",
			 es->chip_label, es->card->index, es->card->codec,
			 es->irq_gsi);
	es_cards[es_ncards++] = es;
	return 0;
}

int es1371_probe_all(void)
{
	const pci_device_t *devs;
	int n = 0, found = 0;

	devs = pci_get_devices(&n);
	if (!devs || n <= 0)
		return 0;
	for (int i = 0; i < n && es_ncards < ES_MAX_CARDS; i++) {
		const pci_device_t *p = &devs[i];
		const struct es_model *m;
		uint8_t rev;

		if (p->vendor_id != ES_VENDOR_ENSONIQ &&
		    p->vendor_id != ES_VENDOR_ECTIVA)
			continue;
		rev = pci_cfg_read8(p, 0x08);
		m = es_match(p, rev);
		if (!m)
			continue;
		if (es_probe_one(p, m, rev) == 0)
			found++;
	}
	return found;
}
