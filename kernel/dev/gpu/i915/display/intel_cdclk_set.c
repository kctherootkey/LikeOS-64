// LikeOS -- changing the display core clock (CDCLK).
//
// CDCLK has to keep up with the pixels the pipes move: their pixel rate
// (two pixels a clock from display version 10), the planes' rates, the
// memory reads of a pipe (version 12 on), the bandwidth of the data
// buffer slices.  Each part has its own ladder of frequencies and its
// own way between them:
//
//   Broadwell     the LCPLL's CDCLK divider, with CDCLK on FCLK while it
//                 changes and the pcode told before and after;
//   Skylake..     DPLL0 at 8.1 or 8.64 GHz and a divider, inside a pcode
//   Comet Lake    prepare/notify handshake;
//   Broxton,      the DE PLL at a ratio of 19.2 MHz and the CD2X divider;
//   Gemini Lake   the divider alone may change under one running pipe;
//   Cannon Lake.. the CDCLK PLL from the DSSM reference with the ratio in
//   Alder Lake    its enable register, pcode prepare and the new voltage
//                 level after; Alder Lake-P crawls the PLL to a new ratio
//                 while it runs;
//   DG2, Meteor   a fixed PLL ratio and a 16-cycle squash waveform,
//   Lake and on   crawl and squash combined through a midpoint; no pcode
//                 handshake from version 14, where PM demand carries the
//                 frequency and voltage instead (raised before a change
//                 up, lowered after a change down).
//
// A change that cannot be made under running pipes is refused with
// -EBUSY rather than made behind their back.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_wm_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- the pcode mailbox ------------------------------------------------------------- */

static int gen7_mailbox_status(uint32_t mbox)
{
	switch (mbox & PCODE_ERROR_MASK) {
	case PCODE_SUCCESS: return 0;
	case PCODE_ILLEGAL_CMD: return -ENXIO;
	case PCODE_GEN7_TIMEOUT: return -ETIMEDOUT;
	case PCODE_GEN7_ILLEGAL_DATA: return -EINVAL;
	case PCODE_GEN11_ILLEGAL_SUBCOMMAND: return -ENXIO;
	case PCODE_GEN11_LOCKED: return -EBUSY;
	case PCODE_GEN11_REJECTED: return -EACCES;
	case PCODE_GEN7_MIN_FREQ_OUT_OF_RANGE: return -EINVAL;
	default: return 0;
	}
}

int intel_pcode_rw(struct i915_device *i915, uint32_t mbox, uint32_t *val, uint32_t *val1,
		   uint32_t fast_timeout_us, uint32_t slow_timeout_ms, int is_read)
{
	uint32_t m = 0;
	int done = 0;

	if (i915_read32(i915, PCODE_MAILBOX) & PCODE_READY)
		return -EAGAIN;
	i915_write32(i915, PCODE_DATA, *val);
	i915_write32(i915, PCODE_DATA1, val1 ? *val1 : 0);
	i915_write32(i915, PCODE_MAILBOX, PCODE_READY | mbox);
	for (uint32_t t = 0; t <= fast_timeout_us; t++) {
		m = i915_read32(i915, PCODE_MAILBOX);
		if (!(m & PCODE_READY)) {
			done = 1;
			break;
		}
		lapic_delay_us(1);
	}
	for (uint32_t t = 0; !done && t < slow_timeout_ms * 10; t++) {
		lapic_delay_us(100);
		m = i915_read32(i915, PCODE_MAILBOX);
		if (!(m & PCODE_READY))
			done = 1;
	}
	if (!done)
		return -ETIMEDOUT;
	if (is_read) {
		*val = i915_read32(i915, PCODE_DATA);
		if (val1)
			*val1 = i915_read32(i915, PCODE_DATA1);
	}
	return gen7_mailbox_status(m);
}

static int pcode_write_timeout(struct i915_device *i915, uint32_t mbox, uint32_t val,
			       uint32_t timeout_ms)
{
	return intel_pcode_rw(i915, mbox, &val, NULL, 250, timeout_ms, 0);
}

static int pcode_try_request(struct i915_device *i915, uint32_t mbox, uint32_t request,
			     uint32_t reply_mask, uint32_t reply, int *status)
{
	*status = intel_pcode_rw(i915, mbox, &request, NULL, 500, 0, 1);
	return *status == 0 && (request & reply_mask) == reply;
}

/* Resend the request until the pcode acknowledges it: first for
 * `timeout_base_ms', then (as a slow pcode may need more requests to
 * get through) for another 50 ms. */
int intel_pcode_request(struct i915_device *i915, uint32_t mbox, uint32_t request,
			uint32_t reply_mask, uint32_t reply, uint32_t timeout_base_ms)
{
	int status = 0;

	if (pcode_try_request(i915, mbox, request, reply_mask, reply, &status))
		return 0;
	for (uint32_t t = 0; t < timeout_base_ms * 100; t++) {
		lapic_delay_us(10);
		if (pcode_try_request(i915, mbox, request, reply_mask, reply, &status))
			return 0;
	}
	for (uint32_t t = 0; t < 50 * 100; t++) {
		if (pcode_try_request(i915, mbox, request, reply_mask, reply, &status))
			return 0;
		lapic_delay_us(10);
	}
	return status ? status : -ETIMEDOUT;
}

/* ---- state ----------------------------------------------------------------------------- */

struct cdclk_config {
	uint32_t cdclk, vco, ref, bypass;
	uint8_t voltage_level;
	uint8_t joined_mbus;
};

struct cdclk_vals {
	uint32_t cdclk;
	uint16_t refclk;
	uint16_t waveform;
	uint8_t ratio;
};

#define VCO_UNKNOWN 0xffffffffu

static struct {
	struct i915_device *i915;
	const struct intel_wm_platform *plat;
	const struct cdclk_vals *table;
	struct cdclk_config hw;
	uint32_t max_cdclk;
	uint32_t skl_preferred_vco;
	uint8_t dg2_pipes; /* the pipe count the pcode was told */
	int ver;
	int ready;
} g_cd;

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t div_round_up(uint64_t a, uint64_t b)
{
	return b ? (uint32_t)((a + b - 1) / b) : 0;
}

static uint32_t div_round_closest(uint64_t a, uint64_t b)
{
	return b ? (uint32_t)((a + b / 2) / b) : 0;
}

static int hweight16(uint32_t v)
{
	int n = 0;
	for (v &= 0xffff; v; v &= v - 1)
		n++;
	return n;
}

static int is_power_of_2(uint32_t v)
{
	return v && !(v & (v - 1));
}

static int plat_is(struct i915_device *i915, int platform)
{
	return i915->info->platform == platform;
}

static int is_cnl(struct i915_device *i915)
{
	return plat_is(i915, I915_PLATFORM_CANNONLAKE);
}

static int is_lp(struct i915_device *i915)
{
	return plat_is(i915, I915_PLATFORM_BROXTON) || plat_is(i915, I915_PLATFORM_GEMINILAKE);
}

/* Broxton and everything from display version 10: the DE/CDCLK PLL and
 * the CD2X divider. */
static int bxt_style(struct i915_device *i915)
{
	return g_cd.ver >= 10 || plat_is(i915, I915_PLATFORM_BROXTON);
}

/* The CDCLK PLL's ratio in its enable register, pcode prepare/notify
 * through SKL_PCODE_CDCLK_CONTROL. */
static int icl_style(struct i915_device *i915)
{
	return g_cd.ver >= 11 || is_cnl(i915);
}

static int has_crawl(void)
{
	return !!(g_cd.plat->flags & INTEL_WM_CDCLK_CRAWL);
}

static int has_squash(void)
{
	return !!(g_cd.plat->flags & INTEL_WM_CDCLK_SQUASH);
}

static int ppc(void)
{
	return (g_cd.plat->flags & INTEL_WM_2PPC) ? 2 : 1;
}

static uint8_t active_pipes(struct i915_device *i915)
{
	uint8_t m = 0;
	int n = i915->info->num_pipes > INTEL_MAX_PIPES ? INTEL_MAX_PIPES : i915->info->num_pipes;

	for (int p = 0; p < n; p++)
		if (i915->display.pipes[p].active)
			m |= (uint8_t)(1u << p);
	/* The firmware's pipe, while it still scans out, runs from this
	 * clock as much as any of the driver's: a change that needs every
	 * pipe off does not happen under it. */
	if (i915->boot_scanout.pipe >= 0 && i915->boot_scanout.pipe < n)
		m |= (uint8_t)(1u << i915->boot_scanout.pipe);
	return m;
}

static int wait_bits(struct i915_device *i915, uint32_t reg, uint32_t bits, int set,
		     uint32_t timeout_us)
{
	for (uint32_t t = 0; t <= timeout_us; t += 2) {
		uint32_t v = i915_read32(i915, reg) & bits;
		if (set ? v == bits : v == 0)
			return 0;
		lapic_delay_us(2);
	}
	return -ETIMEDOUT;
}

static void wait_for_vblank(struct i915_device *i915, int pipe)
{
	uint32_t f0 = i915_read32(i915, PIPE_FRMCOUNT(pipe));

	for (int t = 0; t < 1000; t++) {
		if (i915_read32(i915, PIPE_FRMCOUNT(pipe)) != f0)
			return;
		lapic_delay_us(100);
	}
}

/* ---- the frequency tables ------------------------------------------------------------- */

static const struct cdclk_vals bxt_cdclk_table[] = {
	{ 144000, 19200, 0, 60 }, { 288000, 19200, 0, 60 }, { 384000, 19200, 0, 60 },
	{ 576000, 19200, 0, 60 }, { 624000, 19200, 0, 65 }, { 0, 0, 0, 0 },
};

static const struct cdclk_vals glk_cdclk_table[] = {
	{ 79200, 19200, 0, 33 }, { 158400, 19200, 0, 33 }, { 316800, 19200, 0, 33 },
	{ 0, 0, 0, 0 },
};

static const struct cdclk_vals cnl_cdclk_table[] = {
	{ 168000, 19200, 0, 35 }, { 336000, 19200, 0, 35 }, { 528000, 19200, 0, 55 },
	{ 168000, 24000, 0, 28 }, { 336000, 24000, 0, 28 }, { 528000, 24000, 0, 44 },
	{ 0, 0, 0, 0 },
};

static const struct cdclk_vals icl_cdclk_table[] = {
	{ 172800, 19200, 0, 18 }, { 192000, 19200, 0, 20 }, { 307200, 19200, 0, 32 },
	{ 326400, 19200, 0, 68 }, { 556800, 19200, 0, 58 }, { 652800, 19200, 0, 68 },
	{ 180000, 24000, 0, 15 }, { 192000, 24000, 0, 16 }, { 312000, 24000, 0, 26 },
	{ 324000, 24000, 0, 54 }, { 552000, 24000, 0, 46 }, { 648000, 24000, 0, 54 },
	{ 172800, 38400, 0, 9 },  { 192000, 38400, 0, 10 }, { 307200, 38400, 0, 16 },
	{ 326400, 38400, 0, 34 }, { 556800, 38400, 0, 29 }, { 652800, 38400, 0, 34 },
	{ 0, 0, 0, 0 },
};

static const struct cdclk_vals rkl_cdclk_table[] = {
	{ 172800, 19200, 0, 36 }, { 192000, 19200, 0, 40 }, { 307200, 19200, 0, 64 },
	{ 326400, 19200, 0, 136 }, { 556800, 19200, 0, 116 }, { 652800, 19200, 0, 136 },
	{ 180000, 24000, 0, 30 }, { 192000, 24000, 0, 32 }, { 312000, 24000, 0, 52 },
	{ 324000, 24000, 0, 108 }, { 552000, 24000, 0, 92 }, { 648000, 24000, 0, 108 },
	{ 172800, 38400, 0, 18 }, { 192000, 38400, 0, 20 }, { 307200, 38400, 0, 32 },
	{ 326400, 38400, 0, 68 }, { 556800, 38400, 0, 58 }, { 652800, 38400, 0, 68 },
	{ 0, 0, 0, 0 },
};

static const struct cdclk_vals adlp_cdclk_table[] = {
	{ 172800, 19200, 0, 27 }, { 192000, 19200, 0, 20 }, { 307200, 19200, 0, 32 },
	{ 556800, 19200, 0, 58 }, { 652800, 19200, 0, 68 },
	{ 176000, 24000, 0, 22 }, { 192000, 24000, 0, 16 }, { 312000, 24000, 0, 26 },
	{ 552000, 24000, 0, 46 }, { 648000, 24000, 0, 54 },
	{ 179200, 38400, 0, 14 }, { 192000, 38400, 0, 10 }, { 307200, 38400, 0, 16 },
	{ 556800, 38400, 0, 29 }, { 652800, 38400, 0, 34 },
	{ 0, 0, 0, 0 },
};

static const struct cdclk_vals rplu_cdclk_table[] = {
	{ 172800, 19200, 0, 27 }, { 192000, 19200, 0, 20 }, { 307200, 19200, 0, 32 },
	{ 480000, 19200, 0, 50 }, { 556800, 19200, 0, 58 }, { 652800, 19200, 0, 68 },
	{ 176000, 24000, 0, 22 }, { 192000, 24000, 0, 16 }, { 312000, 24000, 0, 26 },
	{ 480000, 24000, 0, 40 }, { 552000, 24000, 0, 46 }, { 648000, 24000, 0, 54 },
	{ 179200, 38400, 0, 14 }, { 192000, 38400, 0, 10 }, { 307200, 38400, 0, 16 },
	{ 480000, 38400, 0, 25 }, { 556800, 38400, 0, 29 }, { 652800, 38400, 0, 34 },
	{ 0, 0, 0, 0 },
};

static const struct cdclk_vals dg2_cdclk_table[] = {
	{ 163200, 38400, 0x8888, 34 }, { 204000, 38400, 0x9248, 34 },
	{ 244800, 38400, 0xa4a4, 34 }, { 285600, 38400, 0xa54a, 34 },
	{ 326400, 38400, 0xaaaa, 34 }, { 367200, 38400, 0xad5a, 34 },
	{ 408000, 38400, 0xb6b6, 34 }, { 448800, 38400, 0xdbb6, 34 },
	{ 489600, 38400, 0xeeee, 34 }, { 530400, 38400, 0xf7de, 34 },
	{ 571200, 38400, 0xfefe, 34 }, { 612000, 38400, 0xfffe, 34 },
	{ 652800, 38400, 0xffff, 34 }, { 0, 0, 0, 0 },
};

static const struct cdclk_vals mtl_cdclk_table[] = {
	{ 172800, 38400, 0xad5a, 16 }, { 192000, 38400, 0xb6b6, 16 },
	{ 307200, 38400, 0x0000, 16 }, { 480000, 38400, 0x0000, 25 },
	{ 556800, 38400, 0x0000, 29 }, { 652800, 38400, 0x0000, 34 },
	{ 0, 0, 0, 0 },
};

static const struct cdclk_vals xe2lpd_cdclk_table[] = {
	{ 153600, 38400, 0xaaaa, 16 }, { 172800, 38400, 0xad5a, 16 },
	{ 192000, 38400, 0xb6b6, 16 }, { 211200, 38400, 0xdbb6, 16 },
	{ 230400, 38400, 0xeeee, 16 }, { 249600, 38400, 0xf7de, 16 },
	{ 268800, 38400, 0xfefe, 16 }, { 288000, 38400, 0xfffe, 16 },
	{ 307200, 38400, 0xffff, 16 }, { 330000, 38400, 0xdbb6, 25 },
	{ 360000, 38400, 0xeeee, 25 }, { 390000, 38400, 0xf7de, 25 },
	{ 420000, 38400, 0xfefe, 25 }, { 450000, 38400, 0xfffe, 25 },
	{ 480000, 38400, 0xffff, 25 }, { 487200, 38400, 0xfefe, 29 },
	{ 522000, 38400, 0xfffe, 29 }, { 556800, 38400, 0xffff, 29 },
	{ 571200, 38400, 0xfefe, 34 }, { 612000, 38400, 0xfffe, 34 },
	{ 652800, 38400, 0xffff, 34 }, { 0, 0, 0, 0 },
};

/* Battlemage: the single frequency of Wa_15015413771. */
static const struct cdclk_vals xe2hpd_cdclk_table[] = {
	{ 652800, 38400, 0xffff, 34 }, { 0, 0, 0, 0 },
};

static const struct cdclk_vals xe3lpd_cdclk_table[] = {
	{ 153600, 38400, 0xaaaa, 16 }, { 172800, 38400, 0xad5a, 16 },
	{ 192000, 38400, 0xb6b6, 16 }, { 211200, 38400, 0xdbb6, 16 },
	{ 230400, 38400, 0xeeee, 16 }, { 249600, 38400, 0xf7de, 16 },
	{ 268800, 38400, 0xfefe, 16 }, { 288000, 38400, 0xfffe, 16 },
	{ 307200, 38400, 0xffff, 16 }, { 326400, 38400, 0xffff, 17 },
	{ 345600, 38400, 0xffff, 18 }, { 364800, 38400, 0xffff, 19 },
	{ 384000, 38400, 0xffff, 20 }, { 403200, 38400, 0xffff, 21 },
	{ 422400, 38400, 0xffff, 22 }, { 441600, 38400, 0xffff, 23 },
	{ 460800, 38400, 0xffff, 24 }, { 480000, 38400, 0xffff, 25 },
	{ 499200, 38400, 0xffff, 26 }, { 518400, 38400, 0xffff, 27 },
	{ 537600, 38400, 0xffff, 28 }, { 556800, 38400, 0xffff, 29 },
	{ 576000, 38400, 0xffff, 30 }, { 595200, 38400, 0xffff, 31 },
	{ 614400, 38400, 0xffff, 32 }, { 633600, 38400, 0xffff, 33 },
	{ 652800, 38400, 0xffff, 34 }, { 672000, 38400, 0xffff, 35 },
	{ 691200, 38400, 0xffff, 36 }, { 0, 0, 0, 0 },
};

/* Nova Lake: the PLL at ratio 21 squashed below 403.2 MHz, then
 * crawled up to 787.2 MHz. */
static const struct cdclk_vals xe3p_lpd_cdclk_table[] = {
	{ 151200, 38400, 0xa4a4, 21 }, { 176400, 38400, 0xaa54, 21 },
	{ 201600, 38400, 0xaaaa, 21 }, { 226800, 38400, 0xad5a, 21 },
	{ 252000, 38400, 0xb6b6, 21 }, { 277200, 38400, 0xdbb6, 21 },
	{ 302400, 38400, 0xeeee, 21 }, { 327600, 38400, 0xf7de, 21 },
	{ 352800, 38400, 0xfefe, 21 }, { 378000, 38400, 0xfffe, 21 },
	{ 403200, 38400, 0xffff, 21 }, { 422400, 38400, 0xffff, 22 },
	{ 441600, 38400, 0xffff, 23 }, { 460800, 38400, 0xffff, 24 },
	{ 480000, 38400, 0xffff, 25 }, { 499200, 38400, 0xffff, 26 },
	{ 518400, 38400, 0xffff, 27 }, { 537600, 38400, 0xffff, 28 },
	{ 556800, 38400, 0xffff, 29 }, { 576000, 38400, 0xffff, 30 },
	{ 595200, 38400, 0xffff, 31 }, { 614400, 38400, 0xffff, 32 },
	{ 633600, 38400, 0xffff, 33 }, { 652800, 38400, 0xffff, 34 },
	{ 672000, 38400, 0xffff, 35 }, { 691200, 38400, 0xffff, 36 },
	{ 710400, 38400, 0xffff, 37 }, { 729600, 38400, 0xffff, 38 },
	{ 748800, 38400, 0xffff, 39 }, { 768000, 38400, 0xffff, 40 },
	{ 787200, 38400, 0xffff, 41 }, { 0, 0, 0, 0 },
};

static const struct cdclk_vals *pick_table(struct i915_device *i915)
{
	if (g_cd.ver >= 35)
		return xe3p_lpd_cdclk_table;
	if (g_cd.ver >= 30)
		return xe3lpd_cdclk_table;
	if (g_cd.ver >= 20)
		return xe2lpd_cdclk_table;
	if (g_cd.plat->ip >= 1401)
		return xe2hpd_cdclk_table;
	if (g_cd.ver >= 14)
		return mtl_cdclk_table;
	if (plat_is(i915, I915_PLATFORM_DG2))
		return dg2_cdclk_table;
	if (plat_is(i915, I915_PLATFORM_ALDERLAKE_P))
		return i915->subplatform == I915_SUBPLATFORM_RPL_U ? rplu_cdclk_table :
								      adlp_cdclk_table;
	if (plat_is(i915, I915_PLATFORM_ROCKETLAKE))
		return rkl_cdclk_table;
	if (g_cd.ver >= 11)
		return icl_cdclk_table;
	if (is_cnl(i915))
		return cnl_cdclk_table;
	if (plat_is(i915, I915_PLATFORM_GEMINILAKE))
		return glk_cdclk_table;
	if (plat_is(i915, I915_PLATFORM_BROXTON))
		return bxt_cdclk_table;
	return NULL;
}

/* ---- voltage levels ------------------------------------------------------------------- */

static uint8_t calc_voltage_level_tbl(uint32_t cdclk, const uint32_t *max, int n)
{
	for (int i = 0; i < n; i++)
		if (cdclk <= max[i])
			return (uint8_t)i;
	return (uint8_t)(n - 1);
}

static uint8_t calc_voltage_level(struct i915_device *i915, uint32_t cdclk)
{
	static const uint32_t icl[] = { 312000, 556800, 652800 };
	/* the specification says 556.8 MHz for the top step, but some
	 * boards boot at 652.8 */
	static const uint32_t ehl[] = { 180000, 312000, 326400, 652800 };
	static const uint32_t tgl[] = { 312000, 326400, 556800, 652800 };
	static const uint32_t rplu[] = { 312000, 480000, 556800, 652800 };

	switch (i915->info->platform) {
	case I915_PLATFORM_BROADWELL:
		switch (cdclk) {
		case 450000: return 0;
		case 540000: return 1;
		case 675000: return 3;
		default: return 2; /* 337.5 MHz */
		}
	case I915_PLATFORM_HASWELL:
		return 0;
	default:
		break;
	}
	if (g_cd.ver == 9 && !plat_is(i915, I915_PLATFORM_BROXTON)) {
		if (cdclk > 540000)
			return 3;
		if (cdclk > 450000)
			return 2;
		if (cdclk > 337500)
			return 1;
		return 0;
	}
	if (is_lp(i915))
		return (uint8_t)div_round_up(cdclk, 25000);
	if (is_cnl(i915))
		return cdclk > 336000 ? 2 : cdclk > 168000 ? 1 : 0;
	if (g_cd.ver >= 30)
		return 0; /* the power controller does not need it */
	if (g_cd.ver >= 14 ||
	    (plat_is(i915, I915_PLATFORM_ALDERLAKE_P) && i915->subplatform == I915_SUBPLATFORM_RPL_U))
		return calc_voltage_level_tbl(cdclk, rplu, 4);
	if (g_cd.ver >= 12)
		return calc_voltage_level_tbl(cdclk, tgl, 4);
	if (plat_is(i915, I915_PLATFORM_JASPERLAKE) || plat_is(i915, I915_PLATFORM_ELKHARTLAKE))
		return calc_voltage_level_tbl(cdclk, ehl, 4);
	return calc_voltage_level_tbl(cdclk, icl, 3);
}

/* What the ports need: a port clock above 594 MHz raises the voltage. */
static uint8_t ddi_min_voltage_level(struct i915_device *i915, uint32_t port_clock)
{
	if (port_clock <= 594000)
		return 0;
	if (g_cd.ver >= 14)
		return 1;
	if (g_cd.ver >= 12)
		return 2;
	if (plat_is(i915, I915_PLATFORM_JASPERLAKE) || plat_is(i915, I915_PLATFORM_ELKHARTLAKE))
		return 3;
	if (g_cd.ver >= 11)
		return 1;
	if (is_cnl(i915))
		return 2;
	return 0;
}

/* ---- calculating a frequency ------------------------------------------------------------- */

static uint32_t skl_calc_cdclk(uint32_t min_cdclk, uint32_t vco)
{
	if (vco == 8640000) {
		if (min_cdclk > 540000)
			return 617143;
		if (min_cdclk > 432000)
			return 540000;
		if (min_cdclk > 308571)
			return 432000;
		return 308571;
	}
	if (min_cdclk > 540000)
		return 675000;
	if (min_cdclk > 450000)
		return 540000;
	if (min_cdclk > 337500)
		return 450000;
	return 337500;
}

static uint32_t bdw_calc_cdclk(uint32_t min_cdclk)
{
	if (min_cdclk > 540000)
		return 675000;
	if (min_cdclk > 450000)
		return 540000;
	if (min_cdclk > 337500)
		return 450000;
	return 337500;
}

static uint32_t bxt_calc_cdclk(uint32_t min_cdclk)
{
	for (int i = 0; g_cd.table && g_cd.table[i].refclk; i++)
		if (g_cd.table[i].refclk == g_cd.hw.ref && g_cd.table[i].cdclk >= min_cdclk)
			return g_cd.table[i].cdclk;
	i915_dbg("[drm] i915: no CDCLK of %u kHz or more with a %u kHz reference\n", min_cdclk,
		 g_cd.hw.ref);
	return g_cd.max_cdclk;
}

static uint32_t bxt_calc_cdclk_pll_vco(uint32_t cdclk)
{
	if (cdclk == g_cd.hw.bypass)
		return 0;
	for (int i = 0; g_cd.table && g_cd.table[i].refclk; i++)
		if (g_cd.table[i].refclk == g_cd.hw.ref && g_cd.table[i].cdclk == cdclk)
			return g_cd.hw.ref * g_cd.table[i].ratio;
	return 0;
}

uint32_t intel_cdclk_round_khz(struct i915_device *i915, uint32_t min_khz)
{
	if (!g_cd.ready)
		return i915->display.cdclk_khz;
	if (bxt_style(i915))
		return bxt_calc_cdclk(min_khz);
	if (g_cd.ver == 9)
		return skl_calc_cdclk(min_khz, g_cd.skl_preferred_vco ? g_cd.skl_preferred_vco :
								     8100000);
	if (plat_is(i915, I915_PLATFORM_BROADWELL))
		return bdw_calc_cdclk(min_khz);
	return g_cd.max_cdclk;
}

uint32_t intel_cdclk_max_khz(struct i915_device *i915)
{
	return g_cd.ready ? g_cd.max_cdclk : i915->display.cdclk_khz;
}

static int cdclk_squash_divider(uint16_t waveform)
{
	return hweight16(waveform ? waveform : 0xffff);
}

/* 2 x the CD2X divider */
static uint32_t cdclk_divider(uint32_t cdclk, uint32_t vco, uint16_t waveform)
{
	return div_round_closest((uint64_t)vco * (uint32_t)cdclk_squash_divider(waveform),
				 (uint64_t)cdclk * 16);
}

static uint16_t cdclk_squash_waveform(uint32_t cdclk)
{
	if (cdclk == g_cd.hw.bypass)
		return 0;
	for (int i = 0; g_cd.table && g_cd.table[i].refclk; i++)
		if (g_cd.table[i].refclk == g_cd.hw.ref && g_cd.table[i].cdclk == cdclk)
			return g_cd.table[i].waveform;
	return 0xffff;
}

int intel_cdclk_mdclk_ratio(struct i915_device *i915)
{
	(void)i915;
	/* from display version 20 the memory clock is the CDCLK PLL's;
	 * before, CD2X */
	if (g_cd.ready && g_cd.ver >= 20 && g_cd.hw.cdclk)
		return (int)div_round_up(g_cd.hw.vco, g_cd.hw.cdclk);
	return 2;
}

static int mdclk_ratio_of(const struct cdclk_config *c)
{
	if (g_cd.ver >= 20 && c->cdclk)
		return (int)div_round_up(c->vco, c->cdclk);
	return 2;
}

/* ---- reading the clock back --------------------------------------------------------------- */

static void hsw_get_cdclk(struct i915_device *i915, struct cdclk_config *c)
{
	uint32_t lcpll = i915_read32(i915, HSW_LCPLL_CTL);
	uint32_t freq = lcpll & HSW_LCPLL_CLK_FREQ_MASK;
	int ult = (i915->devid & 0xff00) == 0x0a00;

	if (lcpll & HSW_LCPLL_CD_SOURCE_FCLK)
		c->cdclk = 800000;
	else if (i915_read32(i915, HSW_FUSE_STRAP) & HSW_CDCLK_LIMIT)
		c->cdclk = 450000;
	else if (freq == HSW_LCPLL_CLK_FREQ_450)
		c->cdclk = 450000;
	else if (ult)
		c->cdclk = 337500;
	else
		c->cdclk = 540000;
}

static void bdw_get_cdclk(struct i915_device *i915, struct cdclk_config *c)
{
	uint32_t lcpll = i915_read32(i915, HSW_LCPLL_CTL);
	uint32_t freq = lcpll & HSW_LCPLL_CLK_FREQ_MASK;

	if (lcpll & HSW_LCPLL_CD_SOURCE_FCLK)
		c->cdclk = 800000;
	else if (i915_read32(i915, HSW_FUSE_STRAP) & HSW_CDCLK_LIMIT)
		c->cdclk = 450000;
	else if (freq == HSW_LCPLL_CLK_FREQ_450)
		c->cdclk = 450000;
	else if (freq == HSW_LCPLL_CLK_FREQ_540_BDW)
		c->cdclk = 540000;
	else if (freq == HSW_LCPLL_CLK_FREQ_337_5_BDW)
		c->cdclk = 337500;
	else
		c->cdclk = 675000;
	/* it cannot be read: at least what the frequency needs */
	c->voltage_level = calc_voltage_level(i915, c->cdclk);
}

static void skl_dpll0_update(struct i915_device *i915, struct cdclk_config *c)
{
	uint32_t val;

	c->ref = 24000;
	c->vco = 0;
	val = i915_read32(i915, LCPLL1_CTL);
	if (!(val & LCPLL_PLL_ENABLE) || !(val & LCPLL_PLL_LOCK))
		return;
	val = i915_read32(i915, DPLL_CTRL1);
	if ((val & (DPLL_CTRL1_HDMI_MODE(0) | DPLL_CTRL1_SSC(0) | DPLL_CTRL1_OVERRIDE(0))) !=
	    (uint32_t)DPLL_CTRL1_OVERRIDE(0))
		return;
	switch ((val & DPLL_CTRL1_LINK_RATE_MASK(0)) >> DPLL_CTRL1_LINK_RATE_SHIFT(0)) {
	case DPLL_CTRL1_LINK_RATE_810:
	case DPLL_CTRL1_LINK_RATE_1350:
	case DPLL_CTRL1_LINK_RATE_1620:
	case DPLL_CTRL1_LINK_RATE_2700:
		c->vco = 8100000;
		break;
	case DPLL_CTRL1_LINK_RATE_1080:
	case DPLL_CTRL1_LINK_RATE_2160:
		c->vco = 8640000;
		break;
	}
}

static void skl_get_cdclk(struct i915_device *i915, struct cdclk_config *c)
{
	uint32_t cdctl;

	skl_dpll0_update(i915, c);
	c->cdclk = c->bypass = c->ref;
	if (c->vco) {
		cdctl = i915_read32(i915, SKL_CDCLK_CTL) & CDCLK_FREQ_SEL_MASK_SKL;
		int v8640 = c->vco == 8640000;
		if (cdctl == (uint32_t)CDCLK_FREQ_450_432)
			c->cdclk = v8640 ? 432000 : 450000;
		else if (cdctl == (uint32_t)CDCLK_FREQ_337_308)
			c->cdclk = v8640 ? 308571 : 337500;
		else if (cdctl == (uint32_t)CDCLK_FREQ_540)
			c->cdclk = 540000;
		else
			c->cdclk = v8640 ? 617143 : 675000;
	}
	c->voltage_level = calc_voltage_level(i915, c->cdclk);
}

static void bxt_de_pll_readout(struct i915_device *i915, struct cdclk_config *c)
{
	uint32_t val, ratio;

	if (plat_is(i915, I915_PLATFORM_DG2)) {
		c->ref = 38400;
	} else if (g_cd.ver >= 11) {
		switch (i915_read32(i915, SKL_DSSM_REG) & ICL_DSSM_REFCLK_MASK) {
		case ICL_DSSM_REFCLK_19_2MHz: c->ref = 19200; break;
		case ICL_DSSM_REFCLK_38_4MHz: c->ref = 38400; break;
		default: c->ref = 24000; break;
		}
	} else if (is_cnl(i915)) {
		c->ref = (i915_read32(i915, SKL_DSSM_REG) & CNL_DSSM_CDCLK_PLL_REFCLK_24MHz) ? 24000 :
											    19200;
	} else {
		c->ref = 19200;
	}
	val = i915_read32(i915, CDCLK_PLL_ENABLE);
	if (!(val & CDCLK_PLL_EN) || !(val & CDCLK_PLL_LOCKED)) {
		c->vco = 0;
		return;
	}
	if (icl_style(i915))
		ratio = val & 0xff;
	else
		ratio = i915_read32(i915, BXT_DE_PLL_CTL_REG) & 0xff;
	c->vco = ratio * c->ref;
}

static void bxt_get_cdclk(struct i915_device *i915, struct cdclk_config *c)
{
	uint32_t squash_ctl = 0, divider;
	uint32_t div;

	bxt_de_pll_readout(i915, c);
	if (g_cd.ver >= 12)
		c->bypass = c->ref / 2;
	else if (g_cd.ver >= 11)
		c->bypass = 50000;
	else
		c->bypass = c->ref;
	if (c->vco == 0) {
		c->cdclk = c->bypass;
		goto out;
	}
	divider = i915_read32(i915, SKL_CDCLK_CTL) & CD2X_DIV_SEL_MASK;
	switch (divider) {
	case CD2X_DIV_SEL_1: div = 2; break;
	case CD2X_DIV_SEL_1_5: div = 3; break;
	case CD2X_DIV_SEL_2: div = 4; break;
	default: div = 8; break;
	}
	if (has_squash())
		squash_ctl = i915_read32(i915, CDCLK_SQUASH_REG);
	if (squash_ctl & CDCLK_SQUASH_EN) {
		uint32_t size = CDCLK_SQUASH_WINDOW_GET(squash_ctl) + 1;
		uint32_t waveform = CDCLK_SQUASH_WAVE_GET(squash_ctl) >> (16 - size);
		c->cdclk = div_round_closest((uint64_t)hweight16(waveform) * c->vco, size * div);
	} else {
		c->cdclk = div_round_closest(c->vco, div);
	}
out:
	if (g_cd.ver >= 20)
		c->joined_mbus = !!(i915_read32(i915, SKL_MBUS_CTL) & SKL_MBUS_JOIN);
	c->voltage_level = calc_voltage_level(i915, c->cdclk);
}

static void get_cdclk(struct i915_device *i915, struct cdclk_config *c)
{
	mm_memset(c, 0, sizeof(*c));
	if (plat_is(i915, I915_PLATFORM_HASWELL))
		hsw_get_cdclk(i915, c);
	else if (plat_is(i915, I915_PLATFORM_BROADWELL))
		bdw_get_cdclk(i915, c);
	else if (bxt_style(i915))
		bxt_get_cdclk(i915, c);
	else
		skl_get_cdclk(i915, c);
}

/* The rest of the driver keeps its copy in display.cdclk_*. */
static void update_cdclk(struct i915_device *i915)
{
	uint8_t vl = g_cd.hw.voltage_level;
	struct intel_display *d = &i915->display;

	get_cdclk(i915, &g_cd.hw);
	/* the voltage level cannot be read back on Ice Lake and later */
	if (icl_style(i915))
		g_cd.hw.voltage_level = (uint8_t)umax(vl, g_cd.hw.voltage_level);
	d->cdclk_khz = g_cd.hw.cdclk;
	d->cdclk_vco_khz = g_cd.hw.vco;
	if (g_cd.hw.ref)
		d->cdclk_ref_khz = g_cd.hw.ref;
	d->cdclk_voltage_level = g_cd.hw.voltage_level;
}

static void update_max_cdclk(struct i915_device *i915)
{
	uint32_t m;

	if (g_cd.ver >= 35)
		m = 787200;
	else if (g_cd.plat->ip >= 3002)
		m = 480000;
	else if (g_cd.ver >= 30)
		m = 691200;
	else if (plat_is(i915, I915_PLATFORM_JASPERLAKE) || plat_is(i915, I915_PLATFORM_ELKHARTLAKE))
		m = g_cd.hw.ref == 24000 ? 552000 : 556800;
	else if (g_cd.ver >= 11)
		m = g_cd.hw.ref == 24000 ? 648000 : 652800;
	else if (is_cnl(i915))
		m = 528000;
	else if (plat_is(i915, I915_PLATFORM_GEMINILAKE))
		m = 316800;
	else if (plat_is(i915, I915_PLATFORM_BROXTON))
		m = 624000;
	else if (g_cd.ver == 9) {
		uint32_t limit = i915_read32(i915, SKL_DFSM) & SKL_DFSM_CDCLK_LIMIT_MASK;
		uint32_t max_cdclk;
		if (limit == SKL_DFSM_CDCLK_LIMIT_675)
			max_cdclk = 617143;
		else if (limit == SKL_DFSM_CDCLK_LIMIT_540)
			max_cdclk = 540000;
		else if (limit == SKL_DFSM_CDCLK_LIMIT_450)
			max_cdclk = 432000;
		else
			max_cdclk = 308571;
		m = skl_calc_cdclk(max_cdclk, g_cd.skl_preferred_vco ? g_cd.skl_preferred_vco :
								       8100000);
	} else if (plat_is(i915, I915_PLATFORM_BROADWELL)) {
		uint32_t low = i915->devid & 0xf;
		/* ULX (xE) 450, ULT (x6, xB) 540, the rest 675 MHz */
		if (i915_read32(i915, HSW_FUSE_STRAP) & HSW_CDCLK_LIMIT)
			m = 450000;
		else if (low == 0xe)
			m = 450000;
		else if (low == 0x6 || low == 0xb)
			m = 540000;
		else
			m = 675000;
	} else {
		m = g_cd.hw.cdclk;
	}
	g_cd.max_cdclk = m;
}

/* ---- Broadwell -------------------------------------------------------------------------- */

static uint32_t bdw_cdclk_freq_sel(uint32_t cdclk)
{
	switch (cdclk) {
	case 450000: return HSW_LCPLL_CLK_FREQ_450;
	case 540000: return HSW_LCPLL_CLK_FREQ_540_BDW;
	case 675000: return HSW_LCPLL_CLK_FREQ_675_BDW;
	default: return HSW_LCPLL_CLK_FREQ_337_5_BDW;
	}
}

static int bdw_set_cdclk(struct i915_device *i915, const struct cdclk_config *c)
{
	uint32_t v = i915_read32(i915, HSW_LCPLL_CTL);
	int ret;

	if ((v & (HSW_LCPLL_PLL_DISABLE | HSW_LCPLL_PLL_LOCK | HSW_LCPLL_CD_CLOCK_DISABLE |
		  HSW_LCPLL_ROOT_CD_CLOCK_DISABLE | HSW_LCPLL_CD2X_CLOCK_DISABLE |
		  HSW_LCPLL_POWER_DOWN_ALLOW | HSW_LCPLL_CD_SOURCE_FCLK)) != HSW_LCPLL_PLL_LOCK) {
		kprintf("[drm] i915: CDCLK cannot change: the LCPLL is not running it\n");
		return -EIO;
	}
	ret = pcode_write_timeout(i915, BDW_PCODE_DISPLAY_FREQ_CHANGE_REQ, 0, 1);
	if (ret) {
		kprintf("[drm] i915: the pcode did not take the CDCLK change request (%d)\n", ret);
		return ret;
	}
	i915_write32(i915, HSW_LCPLL_CTL, i915_read32(i915, HSW_LCPLL_CTL) | HSW_LCPLL_CD_SOURCE_FCLK);
	/* the specification says 1 us; testing says longer */
	if (wait_bits(i915, HSW_LCPLL_CTL, HSW_LCPLL_CD_SOURCE_FCLK_DONE, 1, 100))
		kprintf("[drm] i915: CDCLK did not switch to FCLK\n");
	v = i915_read32(i915, HSW_LCPLL_CTL) & ~HSW_LCPLL_CLK_FREQ_MASK;
	i915_write32(i915, HSW_LCPLL_CTL, v | bdw_cdclk_freq_sel(c->cdclk));
	i915_write32(i915, HSW_LCPLL_CTL, i915_read32(i915, HSW_LCPLL_CTL) & ~HSW_LCPLL_CD_SOURCE_FCLK);
	if (wait_bits(i915, HSW_LCPLL_CTL, HSW_LCPLL_CD_SOURCE_FCLK_DONE, 0, 1))
		kprintf("[drm] i915: CDCLK did not switch back to the LCPLL\n");
	pcode_write_timeout(i915, HSW_PCODE_DE_WRITE_FREQ_REQ, c->voltage_level, 1);
	i915_write32(i915, HSW_CDCLK_FREQ, div_round_closest(c->cdclk, 1000) - 1);
	return 0;
}

/* ---- Skylake ------------------------------------------------------------------------------ */

/* kHz to the .1 MHz fixed point (with the -1 MHz offset) of CDCLK_CTL */
static uint32_t skl_cdclk_decimal(uint32_t cdclk)
{
	return div_round_closest(cdclk - 1000, 500);
}

static void skl_dpll0_enable(struct i915_device *i915, uint32_t vco)
{
	uint32_t v = i915_read32(i915, DPLL_CTRL1);

	/* DPLL0 at the lowest link rate of the VCO; the panel's link picks
	 * its rate later, within the same VCO */
	v &= ~(DPLL_CTRL1_HDMI_MODE(0) | DPLL_CTRL1_SSC(0) | DPLL_CTRL1_LINK_RATE_MASK(0));
	v |= DPLL_CTRL1_OVERRIDE(0) |
	     DPLL_CTRL1_LINK_RATE(vco == 8640000 ? DPLL_CTRL1_LINK_RATE_1080 :
						  DPLL_CTRL1_LINK_RATE_810, 0);
	i915_write32(i915, DPLL_CTRL1, v);
	(void)i915_read32(i915, DPLL_CTRL1);
	i915_write32(i915, LCPLL1_CTL, i915_read32(i915, LCPLL1_CTL) | LCPLL_PLL_ENABLE);
	if (wait_bits(i915, LCPLL1_CTL, LCPLL_PLL_LOCK, 1, 5000))
		kprintf("[drm] i915: DPLL0 did not lock\n");
	g_cd.hw.vco = vco;
	g_cd.skl_preferred_vco = vco;
}

static void skl_dpll0_disable(struct i915_device *i915)
{
	i915_write32(i915, LCPLL1_CTL, i915_read32(i915, LCPLL1_CTL) & ~LCPLL_PLL_ENABLE);
	if (wait_bits(i915, LCPLL1_CTL, LCPLL_PLL_LOCK, 0, 1000))
		kprintf("[drm] i915: DPLL0 did not stop\n");
	g_cd.hw.vco = 0;
}

static uint32_t skl_cdclk_freq_sel(uint32_t cdclk)
{
	switch (cdclk) {
	case 450000:
	case 432000: return CDCLK_FREQ_450_432;
	case 540000: return CDCLK_FREQ_540;
	case 617143:
	case 675000: return CDCLK_FREQ_675_617;
	default: return CDCLK_FREQ_337_308;
	}
}

static int skl_set_cdclk(struct i915_device *i915, const struct cdclk_config *c)
{
	uint32_t freq_select, ctl;
	int ret;

	/* WA #1183: 308 and 617 MHz are not supported on Skylake itself */
	if (plat_is(i915, I915_PLATFORM_SKYLAKE) && c->vco == 8640000)
		kprintf("[drm] i915: Skylake with an 8.64 GHz DPLL0 (WA #1183)\n");
	ret = intel_pcode_request(i915, PCODE_SKL_CDCLK_CONTROL, PCODE_SKL_CDCLK_PREPARE_FOR_CHANGE,
				  PCODE_SKL_CDCLK_READY_FOR_CHANGE,
				  PCODE_SKL_CDCLK_READY_FOR_CHANGE, 3);
	if (ret) {
		kprintf("[drm] i915: the PCU did not take the CDCLK change (%d)\n", ret);
		return ret;
	}
	freq_select = skl_cdclk_freq_sel(c->cdclk);
	if (g_cd.hw.vco != 0 && g_cd.hw.vco != c->vco)
		skl_dpll0_disable(i915);
	ctl = i915_read32(i915, SKL_CDCLK_CTL);
	if (g_cd.hw.vco != c->vco) {
		/* WA #1183 */
		ctl &= ~(CDCLK_FREQ_SEL_MASK_SKL | CDCLK_DECIMAL_MASK);
		ctl |= freq_select | skl_cdclk_decimal(c->cdclk);
		i915_write32(i915, SKL_CDCLK_CTL, ctl);
	}
	/* WA #1183 */
	ctl |= CDCLK_DIVMUX_CD_OVERRIDE;
	i915_write32(i915, SKL_CDCLK_CTL, ctl);
	(void)i915_read32(i915, SKL_CDCLK_CTL);
	if (g_cd.hw.vco != c->vco && c->vco)
		skl_dpll0_enable(i915, c->vco);
	/* WA #1183 */
	ctl &= ~(CDCLK_FREQ_SEL_MASK_SKL | CDCLK_DECIMAL_MASK);
	i915_write32(i915, SKL_CDCLK_CTL, ctl);
	ctl |= freq_select | skl_cdclk_decimal(c->cdclk);
	i915_write32(i915, SKL_CDCLK_CTL, ctl);
	/* WA #1183 */
	ctl &= ~CDCLK_DIVMUX_CD_OVERRIDE;
	i915_write32(i915, SKL_CDCLK_CTL, ctl);
	(void)i915_read32(i915, SKL_CDCLK_CTL);
	/* tell the PCU */
	intel_pcode_rw(i915, PCODE_SKL_CDCLK_CONTROL, &(uint32_t){ c->voltage_level }, NULL, 250,
		       1, 0);
	return 0;
}

/* ---- Broxton and later ------------------------------------------------------------------- */

static void bxt_de_pll_disable(struct i915_device *i915)
{
	i915_write32(i915, CDCLK_PLL_ENABLE, 0);
	if (wait_bits(i915, CDCLK_PLL_ENABLE, CDCLK_PLL_LOCKED, 0, 1000))
		kprintf("[drm] i915: the DE PLL did not unlock\n");
	g_cd.hw.vco = 0;
}

static void bxt_de_pll_enable(struct i915_device *i915, uint32_t vco)
{
	uint32_t ratio = div_round_closest(vco, g_cd.hw.ref);
	uint32_t v = i915_read32(i915, BXT_DE_PLL_CTL_REG) & ~0xffu;

	i915_write32(i915, BXT_DE_PLL_CTL_REG, v | BXT_DE_PLL_RATIO(ratio));
	i915_write32(i915, CDCLK_PLL_ENABLE, CDCLK_PLL_EN);
	if (wait_bits(i915, CDCLK_PLL_ENABLE, CDCLK_PLL_LOCKED, 1, 1000))
		kprintf("[drm] i915: the DE PLL did not lock\n");
	g_cd.hw.vco = vco;
}

static void icl_cdclk_pll_disable(struct i915_device *i915)
{
	/* Wa_13012396614: the memory clock off the CDCLK PLL first */
	if (g_cd.plat->ip == 3000 || g_cd.plat->ip == 3500) {
		uint32_t v = i915_read32(i915, SKL_CDCLK_CTL) & ~MDCLK_SOURCE_SEL_MASK;
		i915_write32(i915, SKL_CDCLK_CTL, v | MDCLK_SOURCE_SEL_CD2XCLK);
	}
	i915_write32(i915, CDCLK_PLL_ENABLE, i915_read32(i915, CDCLK_PLL_ENABLE) & ~CDCLK_PLL_EN);
	if (wait_bits(i915, CDCLK_PLL_ENABLE, CDCLK_PLL_LOCKED, 0, 1000))
		kprintf("[drm] i915: the CDCLK PLL did not unlock\n");
	g_cd.hw.vco = 0;
}

static void icl_cdclk_pll_enable(struct i915_device *i915, uint32_t vco)
{
	uint32_t v = CDCLK_PLL_RATIO(div_round_closest(vco, g_cd.hw.ref));

	i915_write32(i915, CDCLK_PLL_ENABLE, v);
	i915_write32(i915, CDCLK_PLL_ENABLE, v | CDCLK_PLL_EN);
	if (wait_bits(i915, CDCLK_PLL_ENABLE, CDCLK_PLL_LOCKED, 1, 1000))
		kprintf("[drm] i915: the CDCLK PLL did not lock\n");
	g_cd.hw.vco = vco;
}

/* A new ratio without stopping the PLL. */
static void adlp_cdclk_pll_crawl(struct i915_device *i915, uint32_t vco)
{
	uint32_t v = CDCLK_PLL_RATIO(div_round_closest(vco, g_cd.hw.ref)) | CDCLK_PLL_EN;

	i915_write32(i915, CDCLK_PLL_ENABLE, v);
	v |= CDCLK_PLL_FREQ_REQ;
	i915_write32(i915, CDCLK_PLL_ENABLE, v);
	if (wait_bits(i915, CDCLK_PLL_ENABLE, CDCLK_PLL_LOCKED | CDCLK_PLL_FREQ_REQ_ACK, 1, 1000))
		kprintf("[drm] i915: the CDCLK PLL did not acknowledge the new ratio\n");
	v &= ~CDCLK_PLL_FREQ_REQ;
	i915_write32(i915, CDCLK_PLL_ENABLE, v);
	g_cd.hw.vco = vco;
}

static uint32_t cd2x_pipe(struct i915_device *i915, int pipe)
{
	if (g_cd.ver >= 11)
		return pipe < 0 ? ICL_CD2X_PIPE_NONE : ICL_CD2X_PIPE(pipe);
	(void)i915;
	return pipe < 0 ? BXT_CD2X_PIPE_NONE : BXT_CD2X_PIPE(pipe);
}

static uint32_t cd2x_div_sel(uint32_t cdclk, uint32_t vco, uint16_t waveform)
{
	switch (cdclk_divider(cdclk, vco, waveform)) {
	case 3: return CD2X_DIV_SEL_1_5;
	case 4: return CD2X_DIV_SEL_2;
	case 8: return CD2X_DIV_SEL_4;
	default: return CD2X_DIV_SEL_1; /* 2, and the bypass */
	}
}

static void squash_program(struct i915_device *i915, uint16_t waveform)
{
	uint32_t v = 0;

	if (waveform)
		v = CDCLK_SQUASH_EN | CDCLK_SQUASH_WINDOW(0xf) | CDCLK_SQUASH_WAVE(waveform);
	i915_write32(i915, CDCLK_SQUASH_REG, v);
}

static uint32_t bxt_cdclk_ctl(struct i915_device *i915, const struct cdclk_config *c, int pipe)
{
	uint16_t waveform = cdclk_squash_waveform(c->cdclk);
	uint32_t v = cd2x_div_sel(c->cdclk, c->vco, waveform);

	if (g_cd.ver < 30)
		v |= cd2x_pipe(i915, pipe);
	/* SSA precharge on from 500 MHz */
	if (is_lp(i915) && c->cdclk >= 500000)
		v |= CD2X_SSA_PRECHARGE_ENABLE;
	if (g_cd.ver >= 20) {
		if ((g_cd.plat->ip == 3000 || g_cd.plat->ip == 3500) && c->vco == 0)
			v |= MDCLK_SOURCE_SEL_CD2XCLK;
		else
			v |= MDCLK_SOURCE_SEL_CDCLK_PLL;
	} else {
		v |= skl_cdclk_decimal(c->cdclk);
	}
	return v;
}

static int pll_enable_wa_needed(struct i915_device *i915)
{
	return (g_cd.plat->ip == 2000 || g_cd.plat->ip == 1400 ||
		plat_is(i915, I915_PLATFORM_DG2)) && g_cd.hw.vco > 0 && g_cd.hw.vco != VCO_UNKNOWN;
}

static void _bxt_set_cdclk(struct i915_device *i915, const struct cdclk_config *c, int pipe)
{
	if (has_crawl() && g_cd.hw.vco > 0 && c->vco > 0 && g_cd.hw.vco != VCO_UNKNOWN) {
		if (g_cd.hw.vco != c->vco)
			adlp_cdclk_pll_crawl(i915, c->vco);
	} else if (icl_style(i915)) {
		/* wa_15010685871: DG2, Meteor Lake */
		if (pll_enable_wa_needed(i915))
			squash_program(i915, 0);
		if (g_cd.hw.vco != 0 && g_cd.hw.vco != c->vco)
			icl_cdclk_pll_disable(i915);
		if (g_cd.hw.vco != c->vco && c->vco)
			icl_cdclk_pll_enable(i915, c->vco);
		if (!c->vco)
			g_cd.hw.vco = 0;
	} else {
		if (g_cd.hw.vco != 0 && g_cd.hw.vco != c->vco)
			bxt_de_pll_disable(i915);
		if (g_cd.hw.vco != c->vco && c->vco)
			bxt_de_pll_enable(i915, c->vco);
		if (!c->vco)
			g_cd.hw.vco = 0;
	}
	if (has_squash())
		squash_program(i915, cdclk_squash_waveform(c->cdclk));
	i915_write32(i915, SKL_CDCLK_CTL, bxt_cdclk_ctl(i915, c, pipe));
	if (pipe >= 0)
		wait_for_vblank(i915, pipe);
}

/* Crawl and squash at once go through a midpoint: up, the new waveform
 * at the old VCO first; down, the new VCO with the old waveform. */
static int crawl_and_squash_midpoint(const struct cdclk_config *old, const struct cdclk_config *new,
				     struct cdclk_config *mid)
{
	uint16_t old_wf, new_wf, mid_wf;
	uint32_t old_div, new_div, mid_div;

	if (old->vco == VCO_UNKNOWN || !has_crawl() || !has_squash())
		return 0;
	old_wf = cdclk_squash_waveform(old->cdclk);
	new_wf = cdclk_squash_waveform(new->cdclk);
	if (old->vco == 0 || new->vco == 0 || old->vco == new->vco || old_wf == new_wf)
		return 0;
	old_div = cdclk_divider(old->cdclk, old->vco, old_wf);
	new_div = cdclk_divider(new->cdclk, new->vco, new_wf);
	/* more midpoints would be needed to change the divider as well */
	if (old_div != new_div)
		return 0;
	*mid = *new;
	if (cdclk_squash_divider(new_wf) > cdclk_squash_divider(old_wf)) {
		mid->vco = old->vco;
		mid_div = old_div;
		mid_wf = new_wf;
	} else {
		mid->vco = new->vco;
		mid_div = new_div;
		mid_wf = old_wf;
	}
	mid->cdclk = div_round_closest((uint64_t)cdclk_squash_divider(mid_wf) * mid->vco,
				       16ull * mid_div);
	return 1;
}

static int bxt_set_cdclk(struct i915_device *i915, const struct cdclk_config *c, int pipe)
{
	struct cdclk_config mid;
	uint32_t old_cdclk = g_cd.hw.cdclk;
	int ret = 0;

	/* tell the power controller (version 14 and DG2 do without) */
	if (g_cd.ver >= 14 || plat_is(i915, I915_PLATFORM_DG2))
		;
	else if (icl_style(i915))
		ret = intel_pcode_request(i915, PCODE_SKL_CDCLK_CONTROL,
					  PCODE_SKL_CDCLK_PREPARE_FOR_CHANGE,
					  PCODE_SKL_CDCLK_READY_FOR_CHANGE,
					  PCODE_SKL_CDCLK_READY_FOR_CHANGE, 3);
	else
		/* 150 us by the specification, 2 ms in practice */
		ret = pcode_write_timeout(i915, HSW_PCODE_DE_WRITE_FREQ_REQ, 0x80000000u, 2);
	if (ret) {
		kprintf("[drm] i915: the PCU did not take the CDCLK change to %u kHz (%d)\n",
			c->cdclk, ret);
		return ret;
	}
	if (g_cd.ver >= 20 && c->cdclk < old_cdclk)
		intel_dbuf_mdclk_ratio_update(i915, mdclk_ratio_of(c), c->joined_mbus);
	if (crawl_and_squash_midpoint(&g_cd.hw, c, &mid)) {
		_bxt_set_cdclk(i915, &mid, pipe);
		_bxt_set_cdclk(i915, c, pipe);
	} else {
		_bxt_set_cdclk(i915, c, pipe);
	}
	if (g_cd.ver >= 20 && c->cdclk > old_cdclk)
		intel_dbuf_mdclk_ratio_update(i915, mdclk_ratio_of(c), c->joined_mbus);
	if (g_cd.ver >= 14)
		;
	else if (icl_style(i915) && !plat_is(i915, I915_PLATFORM_DG2))
		ret = intel_pcode_rw(i915, PCODE_SKL_CDCLK_CONTROL, &(uint32_t){ c->voltage_level },
				     NULL, 250, 1, 0);
	else if (!icl_style(i915))
		/* no specified timeout; 2 ms by experiment */
		ret = pcode_write_timeout(i915, HSW_PCODE_DE_WRITE_FREQ_REQ, c->voltage_level, 2);
	if (ret)
		kprintf("[drm] i915: the PCU did not take the CDCLK voltage level %u (%d)\n",
			c->voltage_level, ret);
	return 0;
}

/* ---- DG2: CDCLK and the pipe count told to the pcode ------------------------------------------ */

static void dg2_pcode_notify(struct i915_device *i915, uint8_t voltage_level, uint8_t pipes,
			     uint32_t cdclk_khz, int cdclk_valid, int pipes_valid)
{
	uint32_t mask;
	int ret;

	if (!plat_is(i915, I915_PLATFORM_DG2))
		return;
	/* the field is in MHz (its maximum is DISPLAY_TO_PCODE_CDCLK_MAX) */
	mask = DISPLAY_TO_PCODE_CDCLK(umin(div_round_up(cdclk_khz, 1000),
					   DISPLAY_TO_PCODE_CDCLK_MAX)) |
	       DISPLAY_TO_PCODE_PIPE_COUNT(pipes) | DISPLAY_TO_PCODE_VOLTAGE(voltage_level);
	if (cdclk_valid)
		mask |= DISPLAY_TO_PCODE_CDCLK_VALID;
	if (pipes_valid)
		mask |= DISPLAY_TO_PCODE_PIPE_COUNT_VALID;
	ret = intel_pcode_request(i915, PCODE_SKL_CDCLK_CONTROL,
				  PCODE_SKL_CDCLK_PREPARE_FOR_CHANGE | mask,
				  PCODE_SKL_CDCLK_READY_FOR_CHANGE, PCODE_SKL_CDCLK_READY_FOR_CHANGE,
				  3);
	if (ret)
		kprintf("[drm] i915: the PCU did not take the display configuration (%d)\n", ret);
}

static int popcount8(uint32_t v)
{
	int n = 0;
	for (v &= 0xff; v; v &= v - 1)
		n++;
	return n;
}

/* ---- the change ----------------------------------------------------------------------------- */

static int clock_changed(const struct cdclk_config *a, const struct cdclk_config *b)
{
	return a->cdclk != b->cdclk || a->vco != b->vco || a->ref != b->ref;
}

static int can_cd2x_update(struct i915_device *i915, const struct cdclk_config *a,
			   const struct cdclk_config *b)
{
	if (!bxt_style(i915))
		return 0;
	if (has_squash())
		return 0;
	return a->cdclk != b->cdclk && a->vco != 0 && a->vco == b->vco && a->ref == b->ref;
}

static int can_squash(const struct cdclk_config *a, const struct cdclk_config *b)
{
	if (!has_squash())
		return 0;
	return a->cdclk != b->cdclk && a->vco != 0 && a->vco == b->vco && a->ref == b->ref;
}

static int can_crawl(const struct cdclk_config *a, const struct cdclk_config *b)
{
	if (!has_crawl() || !a->cdclk || !b->cdclk)
		return 0;
	/* the divider may not change while crawling */
	return a->vco != 0 && b->vco != 0 && a->vco != b->vco &&
	       div_round_closest(a->vco, a->cdclk) == div_round_closest(b->vco, b->cdclk) &&
	       a->ref == b->ref;
}

static int can_crawl_and_squash(const struct cdclk_config *a, const struct cdclk_config *b)
{
	if (a->vco == 0 || b->vco == 0 || a->vco == VCO_UNKNOWN)
		return 0;
	if (!has_crawl() || !has_squash())
		return 0;
	return a->vco != b->vco && cdclk_squash_waveform(a->cdclk) != cdclk_squash_waveform(b->cdclk);
}

/* The configuration for `cdclk' on this platform (VCO, voltage). */
static void make_config(struct i915_device *i915, uint32_t cdclk, uint32_t skl_vco,
			uint8_t min_vl, struct cdclk_config *c)
{
	*c = g_cd.hw;
	c->cdclk = cdclk;
	if (bxt_style(i915)) {
		c->vco = bxt_calc_cdclk_pll_vco(cdclk);
		c->voltage_level = (uint8_t)umax(min_vl, calc_voltage_level(i915, cdclk));
	} else if (g_cd.ver == 9) {
		c->vco = skl_vco;
		c->voltage_level = calc_voltage_level(i915, cdclk);
	} else {
		c->voltage_level = calc_voltage_level(i915, cdclk);
	}
	c->joined_mbus = (uint8_t)intel_wm_mbus_joined(i915);
}

/* -1: impossible with the pipes up; else the pipe to synchronise the
 * divider change to (or -2: none needed). */
static int change_method(struct i915_device *i915, const struct cdclk_config *new)
{
	uint8_t active = active_pipes(i915);

	if (!clock_changed(&g_cd.hw, new))
		return -2;
	if (can_crawl_and_squash(&g_cd.hw, new) || can_squash(&g_cd.hw, new) ||
	    can_crawl(&g_cd.hw, new))
		return -2;
	if (!active)
		return -2;
	if (is_power_of_2(active) && can_cd2x_update(i915, &g_cd.hw, new)) {
		for (int p = 0; p < 8; p++)
			if (active & (1u << p))
				return p;
	}
	return -1;
}

/* Skylake's DPLL0 VCO.  An eDP link at 2.16 or 4.32 GHz would need
 * 8.64 GHz, but here DPLL0 doubles as a port PLL at the rate the
 * firmware gave it (intel_dpll.c), so the VCO is kept as it is running;
 * with DPLL0 off it comes up at the preferred 8.1 GHz (or what it last
 * was). */
static uint32_t skl_target_vco(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg)
{
	(void)i915;
	(void)cfg;
	if (g_cd.hw.vco && g_cd.hw.vco != VCO_UNKNOWN)
		return g_cd.hw.vco;
	return g_cd.skl_preferred_vco ? g_cd.skl_preferred_vco : 8100000;
}

static int cdclk_set_config(struct i915_device *i915, struct cdclk_config *new, uint8_t pipes)
{
	struct intel_display *d = &i915->display;
	uint32_t old_cdclk = g_cd.hw.cdclk;
	int method, raising, ret = 0;

	if (!clock_changed(&g_cd.hw, new) && new->voltage_level == g_cd.hw.voltage_level)
		return 0;
	if (plat_is(i915, I915_PLATFORM_HASWELL))
		return new->cdclk <= g_cd.hw.cdclk ? 0 : -EINVAL;
	method = change_method(i915, new);
	if (method == -1) {
		i915_dbg("[drm] i915: CDCLK %u -> %u kHz needs every pipe off\n", g_cd.hw.cdclk,
			 new->cdclk);
		return -EBUSY;
	}
	raising = new->cdclk > g_cd.hw.cdclk || new->voltage_level > g_cd.hw.voltage_level;
	kprintf("[drm] i915: CDCLK %u -> %u kHz (VCO %u kHz, voltage level %u)\n", g_cd.hw.cdclk,
		new->cdclk, new->vco, new->voltage_level);

	/* DG2: the frequency, at the top voltage, before */
	if (plat_is(i915, I915_PLATFORM_DG2))
		dg2_pcode_notify(i915, DISPLAY_TO_PCODE_VOLTAGE_MAX,
				 pipes > g_cd.dg2_pipes ? pipes : 0, umax(new->cdclk, g_cd.hw.cdclk),
				 new->cdclk != g_cd.hw.cdclk, pipes > g_cd.dg2_pipes);
	/* display version 14: PM demand raised before a change up */
	if (g_cd.ver >= 14 && raising) {
		d->cdclk_khz = umax(new->cdclk, g_cd.hw.cdclk);
		d->cdclk_voltage_level = (uint8_t)umax(new->voltage_level, g_cd.hw.voltage_level);
		intel_pmdemand_update(i915);
	}

	/* no GMBUS transfer while the clock it is divided from changes */
	for (int t = 0; d->gmbus_busy && t < 10000; t++)
		lapic_delay_us(10);

	if (clock_changed(&g_cd.hw, new) || bxt_style(i915) || g_cd.ver == 9) {
		if (plat_is(i915, I915_PLATFORM_BROADWELL))
			ret = bdw_set_cdclk(i915, new);
		else if (bxt_style(i915))
			ret = bxt_set_cdclk(i915, new, method >= 0 ? method : -1);
		else
			ret = skl_set_cdclk(i915, new);
	}
	if (ret == 0 && icl_style(i915))
		g_cd.hw.voltage_level = new->voltage_level;
	update_cdclk(i915);
	if (ret == 0)
		d->cdclk_voltage_level = g_cd.hw.voltage_level = new->voltage_level;

	if (plat_is(i915, I915_PLATFORM_DG2)) {
		dg2_pcode_notify(i915, new->voltage_level, pipes < g_cd.dg2_pipes ? pipes : 0,
				 new->cdclk, new->cdclk != old_cdclk, pipes < g_cd.dg2_pipes);
		g_cd.dg2_pipes = pipes;
	}
	/* display version 14: PM demand lowered after a change down */
	if (g_cd.ver >= 14 && !raising)
		intel_pmdemand_update(i915);
	if (g_cd.hw.cdclk != new->cdclk)
		kprintf("[drm] i915: CDCLK reads %u kHz after the change to %u kHz\n",
			g_cd.hw.cdclk, new->cdclk);
	return ret;
}

int intel_cdclk_set_level(struct i915_device *i915, uint32_t khz, uint8_t min_voltage_level)
{
	struct cdclk_config new;

	if (!g_cd.ready)
		return -ENODEV;
	if (khz > g_cd.max_cdclk)
		return -EINVAL;
	make_config(i915, intel_cdclk_round_khz(i915, khz), skl_target_vco(i915, NULL),
		    min_voltage_level, &new);
	return cdclk_set_config(i915, &new, (uint8_t)popcount8(active_pipes(i915)));
}

int intel_cdclk_set(struct i915_device *i915, uint32_t khz)
{
	return intel_cdclk_set_level(i915, khz, 0);
}

int intel_cdclk_change_needs_pipes_off(struct i915_device *i915, uint32_t khz)
{
	struct cdclk_config new;

	if (!g_cd.ready)
		return 0;
	make_config(i915, intel_cdclk_round_khz(i915, khz), skl_target_vco(i915, NULL), 0, &new);
	if (plat_is(i915, I915_PLATFORM_HASWELL))
		return 0;
	return change_method(i915, &new) == -1;
}

/* ---- what a configuration needs ------------------------------------------------------------- */

/* The pipe's pixel rate as CDCLK sees it: with the scaler downscaling,
 * the horizontal factor's fraction counts double at two pixels a clock. */
static uint32_t pixel_rate_cdclk(const struct intel_wm_pipe_cfg *c)
{
	uint32_t dst_w = c->hdisplay, dst_h = c->vdisplay;
	uint32_t hscale, vscale;

	if (!c->scaled || !dst_w || !dst_h)
		return c->clock_khz;
	hscale = umax((uint32_t)(((uint64_t)c->src_w << 16) / dst_w), 0x10000);
	vscale = umax((uint32_t)(((uint64_t)c->src_h << 16) / dst_h), 0x10000);
	hscale = (hscale & ~0xffffu) + (uint32_t)ppc() * (hscale & 0xffff);
	return (uint32_t)(((uint64_t)c->clock_khz * hscale * vscale + 0xffffffffull) >> 32);
}

static uint32_t pipe_pixel_rate(const struct intel_wm_pipe_cfg *c)
{
	uint32_t dst_w, dst_h;

	if (!c->scaled)
		return c->clock_khz;
	dst_w = umin(c->src_w, c->hdisplay);
	dst_h = umin(c->src_h, c->vdisplay);
	if (!dst_w || !dst_h)
		return c->clock_khz;
	return div_round_up((uint64_t)c->clock_khz * c->src_w * c->src_h, (uint64_t)dst_w * dst_h);
}

static uint32_t crtc_min_cdclk(struct i915_device *i915, const struct intel_wm_pipe_cfg *c)
{
	uint32_t rate = pixel_rate_cdclk(c);
	uint32_t min_cdclk, plane;

	/* the pixel rate, at 100% of CDCLK (Haswell on) */
	min_cdclk = div_round_up((uint64_t)rate * 100, 100ull * (uint32_t)ppc());
	/* "Maximum Pipe Read Bandwidth" (display version 12+) */
	if (g_cd.ver >= 12 && c->plane_on)
		min_cdclk = umax(min_cdclk, div_round_up((uint64_t)pipe_pixel_rate(c) * c->cpp * 10,
							 512));
	/* the plane: two pixels a clock from version 10 (Gemini Lake 10/8
	 * and Skylake 9/8 for 64-bit pixels, which are not used here) */
	if (c->plane_on) {
		if (g_cd.ver >= 10)
			plane = div_round_up(rate, 2);
		else
			plane = rate;
		min_cdclk = umax(min_cdclk, plane);
	}
	if (c->has_audio) {
		if (c->is_dp && c->port_clock_khz >= 540000 && c->lanes == 4) {
			if (g_cd.ver == 10)
				min_cdclk = umax(min_cdclk, 316800); /* WA #1145 */
			else if (g_cd.ver == 9 || plat_is(i915, I915_PLATFORM_BROADWELL))
				min_cdclk = umax(min_cdclk, 432000); /* WA #1144 */
		}
		/* twice the Azalia BCLK */
		if (g_cd.ver >= 9)
			min_cdclk = umax(min_cdclk, 2 * 96000);
	}
	return min_cdclk;
}

static struct intel_wm_pipe_cfg g_req_cfg[INTEL_MAX_PIPES];

uint32_t intel_cdclk_required_khz(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
				  uint8_t *min_voltage_level)
{
	struct intel_wm_result res;
	uint32_t min_cdclk = 0;
	uint8_t vl = 0, enabled = 0;
	int n = i915->info->num_pipes > INTEL_MAX_PIPES ? INTEL_MAX_PIPES : i915->info->num_pipes;

	if (!cfg) {
		intel_wm_cfg_from_hw(i915, g_req_cfg);
		cfg = g_req_cfg;
	}
	/* the DBUF bandwidth (counted even for a configuration that does
	 * not fit) */
	if (g_cd.ver >= 9) {
		intel_wm_compute(i915, cfg, &res);
		min_cdclk = res.dbuf_bw_min_cdclk;
	}
	for (int p = 0; p < n; p++) {
		if (!cfg[p].active)
			continue;
		enabled |= (uint8_t)(1u << p);
		min_cdclk = umax(min_cdclk, crtc_min_cdclk(i915, &cfg[p]));
		vl = (uint8_t)umax(vl, ddi_min_voltage_level(i915, cfg[p].port_clock_khz));
	}
	/* Gemini Lake: with more than one pipe, high enough for audio, so
	 * that its workaround does not keep changing CDCLK */
	if (plat_is(i915, I915_PLATFORM_GEMINILAKE) && enabled && !is_power_of_2(enabled))
		min_cdclk = umax(min_cdclk, 2 * 96000);
	if (min_voltage_level)
		*min_voltage_level = vl;
	return min_cdclk;
}

int intel_cdclk_update(struct i915_device *i915, const struct intel_wm_pipe_cfg *cfg,
		       int allow_decrease)
{
	struct cdclk_config new;
	uint32_t need, target;
	uint8_t vl = 0, pipes = 0;
	int ret;

	if (!g_cd.ready || plat_is(i915, I915_PLATFORM_HASWELL))
		return 0;
	/* Never lowered under the firmware's pipe: what that pipe needs is in
	 * no configuration of the driver's, so a lower clock would starve it. */
	if (i915->boot_scanout.pipe >= 0)
		allow_decrease = 0;
	if (cfg) {
		for (int p = 0; p < INTEL_MAX_PIPES && p < i915->info->num_pipes; p++)
			pipes += cfg[p].active ? 1 : 0;
	} else {
		pipes = (uint8_t)popcount8(active_pipes(i915));
	}
	need = intel_cdclk_required_khz(i915, cfg, &vl);
	if (need > g_cd.max_cdclk) {
		kprintf("[drm] i915: the configuration needs CDCLK %u kHz, more than the %u kHz there is\n",
			need, g_cd.max_cdclk);
		need = g_cd.max_cdclk;
	}
	target = intel_cdclk_round_khz(i915, need);
	make_config(i915, target, skl_target_vco(i915, cfg), vl, &new);
	if (new.cdclk > g_cd.hw.cdclk || new.voltage_level > g_cd.hw.voltage_level) {
		/* never lower one part while raising the other */
		if (new.cdclk < g_cd.hw.cdclk && !allow_decrease)
			make_config(i915, g_cd.hw.cdclk, new.vco, vl, &new);
		ret = cdclk_set_config(i915, &new, pipes);
		if (ret == -EBUSY)
			kprintf("[drm] i915: CDCLK must rise to %u kHz, which needs every pipe off\n",
				new.cdclk);
		return ret;
	}
	if (!allow_decrease || (new.cdclk == g_cd.hw.cdclk &&
				new.voltage_level == g_cd.hw.voltage_level))
		goto dg2;
	/* lowering is a saving, not a need: never at the cost of stopping
	 * the running pipes */
	if (change_method(i915, &new) == -1)
		goto dg2;
	return cdclk_set_config(i915, &new, pipes);
dg2:
	/* DG2: the pcode follows the pipe count even without a change (up
	 * before the pipe comes on, down after it went off) */
	if (plat_is(i915, I915_PLATFORM_DG2)) {
		if (pipes > g_cd.dg2_pipes || (allow_decrease && pipes < g_cd.dg2_pipes)) {
			dg2_pcode_notify(i915, g_cd.hw.voltage_level, pipes, g_cd.hw.cdclk, 0, 1);
			g_cd.dg2_pipes = pipes;
		}
	}
	return 0;
}

/* ---- initialisation ------------------------------------------------------------------------- */

/* What the firmware left, made consistent: a CDCLK_CTL decimal field
 * that does not match the frequency is corrected; a PLL that is off or a
 * frequency the platform does not have gets the lowest valid setting
 * (only when no pipe could be running from it). */
static void sanitize_cdclk(struct i915_device *i915)
{
	uint32_t cdctl, expected;
	int bad = 0;

	if (plat_is(i915, I915_PLATFORM_HASWELL) || plat_is(i915, I915_PLATFORM_BROADWELL))
		return;
	if (g_cd.hw.vco == 0 || g_cd.hw.cdclk == g_cd.hw.bypass) {
		bad = 1;
	} else if (bxt_style(i915)) {
		if (bxt_calc_cdclk(g_cd.hw.cdclk) != g_cd.hw.cdclk ||
		    bxt_calc_cdclk_pll_vco(g_cd.hw.cdclk) != g_cd.hw.vco)
			bad = 1;
	}
	if (!bad) {
		cdctl = i915_read32(i915, SKL_CDCLK_CTL);
		if (bxt_style(i915)) {
			expected = bxt_cdclk_ctl(i915, &g_cd.hw, -1);
			/* the pipe field: the firmware may have synced to a pipe */
			if (g_cd.ver < 30) {
				uint32_t pm = g_cd.ver >= 11 ? ICL_CD2X_PIPE_MASK : BXT_CD2X_PIPE_MASK;
				cdctl = (cdctl & ~pm) | cd2x_pipe(i915, -1);
			}
		} else {
			expected = (cdctl & CDCLK_FREQ_SEL_MASK_SKL) | skl_cdclk_decimal(g_cd.hw.cdclk);
		}
		if (cdctl != expected) {
			if (g_cd.ver < 20) {
				cdctl &= ~CDCLK_DECIMAL_MASK;
				cdctl |= expected & CDCLK_DECIMAL_MASK;
			}
			if (cdctl != expected) {
				bad = 1;
			} else {
				i915_dbg("[drm] i915: CDCLK_CTL decimal field corrected\n");
				i915_write32(i915, SKL_CDCLK_CTL, expected);
			}
		}
	}
	if (!bad)
		return;
	if (active_pipes(i915) || i915->boot_scanout.pipe >= 0) {
		kprintf("[drm] i915: CDCLK as the firmware left it is not a valid setting; left alone while a pipe runs\n");
		return;
	}
	kprintf("[drm] i915: CDCLK as the firmware left it is not a valid setting; reprogrammed\n");
	g_cd.hw.cdclk = 0;
	g_cd.hw.vco = VCO_UNKNOWN;
	{
		struct cdclk_config c = g_cd.hw;
		if (bxt_style(i915)) {
			c.cdclk = bxt_calc_cdclk(0);
			c.vco = bxt_calc_cdclk_pll_vco(c.cdclk);
			c.voltage_level = calc_voltage_level(i915, c.cdclk);
			bxt_set_cdclk(i915, &c, -1);
		} else {
			c.vco = g_cd.skl_preferred_vco ? g_cd.skl_preferred_vco : 8100000;
			c.cdclk = skl_calc_cdclk(0, c.vco);
			c.voltage_level = calc_voltage_level(i915, c.cdclk);
			skl_set_cdclk(i915, &c);
		}
		update_cdclk(i915);
		g_cd.hw.voltage_level = c.voltage_level;
		i915->display.cdclk_voltage_level = c.voltage_level;
	}
}

int intel_cdclk_set_init(struct i915_device *i915)
{
	g_cd.i915 = i915;
	g_cd.plat = intel_wm_platform_get(i915);
	if (!g_cd.plat)
		return -ENODEV;
	g_cd.ver = g_cd.plat->ip / 100;
	g_cd.table = pick_table(i915);
	get_cdclk(i915, &g_cd.hw);
	/* the firmware did not say: assume what the frequency needs */
	if (i915->display.cdclk_voltage_level > g_cd.hw.voltage_level)
		g_cd.hw.voltage_level = i915->display.cdclk_voltage_level;
	if (g_cd.ver == 9 && !bxt_style(i915) && g_cd.hw.vco)
		g_cd.skl_preferred_vco = g_cd.hw.vco;
	update_max_cdclk(i915);
	g_cd.ready = 1;
	sanitize_cdclk(i915);
	update_cdclk(i915);
	g_cd.dg2_pipes = (uint8_t)popcount8(active_pipes(i915));
	kprintf("[drm] i915: CDCLK %u kHz (VCO %u, reference %u, voltage level %u), at most %u kHz\n",
		g_cd.hw.cdclk, g_cd.hw.vco, g_cd.hw.ref, g_cd.hw.voltage_level, g_cd.max_cdclk);
	return 0;
}
