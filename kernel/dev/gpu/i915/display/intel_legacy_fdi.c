// LikeOS -- FDI links and PCH transcoders of Ironlake, Sandy Bridge and Ivy Bridge.
//
// From Ironlake to Ivy Bridge the CPU carries the pipes but the ports
// (VGA, LVDS, HDMI, SDVO and DisplayPort B to D) sit on the south display
// in the PCH, Ibex Peak or Cougar/Panther Point.  A pipe reaches them over
// FDI, a link of up to four lanes at 2.7 GHz (Ironlake: what the BIOS set
// the FDI PLL to) that must be trained like a DisplayPort link before
// pixels cross it.  On the PCH side a transcoder per pipe regenerates the
// timing from a copy of the CPU transcoder's registers and drives the
// port.  Ivy Bridge has a third pipe but only eight lanes: pipe C borrows
// two of pipe B's lanes, so B may use four only while C is off.  This file
// sizes the links, trains them, and walks the transcoders on and off.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2021 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* The FDI link clock in kHz: Ironlake runs what the BIOS programmed the
 * FDI PLL to, Sandy and Ivy Bridge a fixed 2.7 GHz (270 MHz symbol clock). */
static uint32_t g_fdi_pll_khz;
/* Ivy Bridge parts with pipe C fused off have the whole FDI B link for
 * pipe B. */
static int g_ivb_pipe_c_fused;

static char pipe_name(int pipe)
{
	return (char)('A' + pipe);
}

static void fdi_pll_freq_update(struct lg_display *d)
{
	if (d->is_ilk) {
		uint32_t fb = lg_rd(d, FDI_PLL_BIOS_0) & FDI_PLL_FB_CLOCK_MASK;

		g_fdi_pll_khz = (fb + 2) * 10000;
	} else if (d->is_snb || d->is_ivb) {
		g_fdi_pll_khz = 270000;
	} else {
		return;
	}
	i915_dbg("[drm] i915: FDI PLL freq=%u kHz\n", g_fdi_pll_khz);
}

void lg_fdi_init(struct lg_display *d)
{
	fdi_pll_freq_update(d);

	g_ivb_pipe_c_fused = 0;
	if (d->is_ivb && (lg_rd(d, FUSE_STRAP) & IVB_PIPE_C_DISABLE)) {
		g_ivb_pipe_c_fused = 1;
		i915_dbg("[drm] i915: FDI: pipe C fused off\n");
	}
}

uint32_t lg_fdi_link_freq(struct lg_display *d)
{
	if (!g_fdi_pll_khz)
		fdi_pll_freq_update(d);
	return g_fdi_pll_khz;
}

/* Lanes needed for `bpp' bits per pixel at `clock' kHz over lanes of
 * `link' kHz.  Each lane moves one 8-bit symbol per link clock; a 5%
 * margin covers spread spectrum (at most 2.5% centre spread) so the link
 * is never oversubscribed. */
static int fdi_lanes_required(uint32_t clock, uint32_t link, int bpp)
{
	uint64_t bps = (uint64_t)clock * (uint32_t)bpp * 21 / 20;
	uint64_t per_lane = (uint64_t)link * 8;

	return (int)((bps + per_lane - 1) / per_lane);
}

/* Lanes and M/N for the configuration as it stands. */
static void fdi_compute_m_n(struct lg_display *d, struct lg_config *cfg)
{
	uint32_t link = lg_fdi_link_freq(d);
	uint32_t clock = cfg->t.clock;

	cfg->fdi_lanes = fdi_lanes_required(clock, link, cfg->pipe_bpp);
	lg_link_compute_m_n(cfg->pipe_bpp, cfg->fdi_lanes, clock, link, &cfg->fdi_m_n);
}

/* Two bits per colour less on the pipe, when the link is too narrow for
 * the depth.  A DP port behind the PCH carries the pipe's pixels too, so
 * its M/N follows the new depth.  0, or -EINVAL at 6 bpc already. */
static int fdi_lower_bpp(struct lg_display *d, struct lg_config *cfg)
{
	if (cfg->pipe_bpp <= 6 * 3)
		return -EINVAL;

	cfg->pipe_bpp -= 2 * 3;
	i915_dbg("[drm] i915: FDI: pipe %c bpp lowered to %d\n", pipe_name(cfg->pipe),
		 cfg->pipe_bpp);
	if (cfg->has_dp_encoder && cfg->lane_count > 0 && cfg->port_clock)
		lg_link_compute_m_n(cfg->pipe_bpp, cfg->lane_count, cfg->t.clock,
				    cfg->port_clock, &cfg->dp_m_n);
	fdi_compute_m_n(d, cfg);
	return 0;
}

int lg_fdi_compute_config(struct lg_display *d, struct lg_config *cfg)
{
	if (!cfg->has_pch_encoder) {
		cfg->fdi_lanes = 0;
		return 0;
	}

	if (!lg_fdi_link_freq(d) || !cfg->t.clock)
		return -EINVAL;

	/* FDI is a binary signal at about 2.7 GHz, each octet coded in 10
	 * bits; the link clock is the symbol rate in kHz and the pipe's
	 * dot clock is in kHz too.  A link has at most four lanes; when the
	 * depth needs more, take two bits per colour off and try again. */
	fdi_compute_m_n(d, cfg);
	while (cfg->fdi_lanes > 4) {
		if (fdi_lower_bpp(d, cfg)) {
			i915_dbg("[drm] i915: FDI: pipe %c needs %d lanes at %u kHz\n",
				 pipe_name(cfg->pipe), cfg->fdi_lanes, cfg->t.clock);
			return -EINVAL;
		}
	}

	i915_dbg("[drm] i915: FDI: pipe %c %d lanes, bpp %d\n", pipe_name(cfg->pipe),
		 cfg->fdi_lanes, cfg->pipe_bpp);
	return 0;
}

/* Lanes pipe p takes from the FDI receivers. */
static int pipe_required_fdi_lanes(struct lg_config *const cfgs[LG_MAX_PIPES], int p)
{
	if (p < LG_MAX_PIPES && cfgs[p] && cfgs[p]->has_pch_encoder)
		return cfgs[p]->fdi_lanes;
	return 0;
}

/* The lane rules for pipe `pipe'.  0, or -EINVAL with *reduce the pipe
 * whose link should get narrower. */
static int ilk_check_fdi_lanes(struct lg_display *d, int pipe,
			       struct lg_config *const cfgs[LG_MAX_PIPES], int *reduce)
{
	const struct lg_config *cfg = cfgs[pipe];

	*reduce = pipe;

	i915_dbg("[drm] i915: checking FDI config on pipe %c, lanes %d\n",
		 pipe_name(pipe), cfg->fdi_lanes);
	if (cfg->fdi_lanes > 4) {
		i915_dbg("[drm] i915: invalid FDI lane config on pipe %c: %d lanes\n",
			 pipe_name(pipe), cfg->fdi_lanes);
		return -EINVAL;
	}

	if (d->num_pipes == 2)
		return 0;

	/* Ivy Bridge with three pipes: FDI B and C share the B lanes. */
	switch (pipe) {
	case 0:
		return 0;
	case 1:
		if (cfg->fdi_lanes <= 2 || g_ivb_pipe_c_fused)
			return 0;
		if (pipe_required_fdi_lanes(cfgs, 2) > 0) {
			i915_dbg("[drm] i915: invalid shared FDI lane config on pipe %c: %d lanes\n",
				 pipe_name(pipe), cfg->fdi_lanes);
			return -EINVAL;
		}
		return 0;
	case 2:
		if (g_ivb_pipe_c_fused) {
			i915_dbg("[drm] i915: FDI: pipe C is fused off\n");
			*reduce = -1;
			return -EINVAL;
		}
		if (cfg->fdi_lanes > 2) {
			i915_dbg("[drm] i915: only 2 lanes on pipe %c: required %d lanes\n",
				 pipe_name(pipe), cfg->fdi_lanes);
			return -EINVAL;
		}
		if (pipe_required_fdi_lanes(cfgs, 1) > 2) {
			i915_dbg("[drm] i915: FDI link B uses too many lanes to enable link C\n");
			*reduce = 1;
			return -EINVAL;
		}
		return 0;
	default:
		return 0;
	}
}

int lg_fdi_check_lanes(struct lg_display *d, struct lg_config *const cfgs[LG_MAX_PIPES])
{
	int p, reduce, ret;
	int npipes = d->num_pipes < LG_MAX_PIPES ? d->num_pipes : LG_MAX_PIPES;

restart:
	for (p = 0; p < npipes; p++) {
		if (!cfgs[p] || !cfgs[p]->has_pch_encoder)
			continue;

		ret = ilk_check_fdi_lanes(d, p, cfgs, &reduce);
		if (ret == 0)
			continue;

		/* Narrow the link that is in the way, if its depth allows,
		 * and look at every pipe again. */
		if (reduce >= 0 && reduce < npipes && cfgs[reduce] &&
		    cfgs[reduce]->has_pch_encoder && !fdi_lower_bpp(d, cfgs[reduce]))
			goto restart;

		i915_dbg("[drm] i915: FDI lanes do not fit (pipe %c)\n", pipe_name(p));
		return -EINVAL;
	}
	return 0;
}

/* ---- link training ------------------------------------------------------------------ */

/* Cougar Point: FDI C takes two of FDI B's lanes when bifurcation is on.
 * Only while neither receiver is enabled. */
static void cpt_set_fdi_bc_bifurcation(struct lg_display *d, int enable)
{
	uint32_t temp = lg_rd(d, SOUTH_CHICKEN1);

	if (!!(temp & FDI_BC_BIFURCATION_SELECT) == !!enable)
		return;

	if ((lg_rd(d, FDI_RX_CTL(1)) & FDI_RX_ENABLE) ||
	    (lg_rd(d, FDI_RX_CTL(2)) & FDI_RX_ENABLE))
		kprintf("[drm] i915: FDI B/C bifurcation changed with a receiver on\n");

	temp &= ~FDI_BC_BIFURCATION_SELECT;
	if (enable)
		temp |= FDI_BC_BIFURCATION_SELECT;

	i915_dbg("[drm] i915: %sabling FDI C rx\n", enable ? "en" : "dis");
	lg_wr(d, SOUTH_CHICKEN1, temp);
	lg_posting_read(d, SOUTH_CHICKEN1);
}

static void ivb_update_fdi_bc_bifurcation(struct lg_display *d, const struct lg_config *cfg)
{
	/* With pipe C fused off there is no FDI C to split the lanes for. */
	if (d->num_pipes < 3 || g_ivb_pipe_c_fused)
		return;

	switch (cfg->pipe) {
	case 1:
		cpt_set_fdi_bc_bifurcation(d, cfg->fdi_lanes <= 2);
		break;
	case 2:
		cpt_set_fdi_bc_bifurcation(d, 1);
		break;
	default:
		break;
	}
}

/* Training done: both ends to normal pixel transfer. */
static void fdi_normal_train(struct lg_display *d, int pipe)
{
	uint32_t reg, temp;

	reg = FDI_TX_CTL(pipe);
	temp = lg_rd(d, reg);
	if (d->is_ivb) {
		temp &= ~FDI_LINK_TRAIN_NONE_IVB;
		temp |= FDI_LINK_TRAIN_NONE_IVB | FDI_TX_ENHANCE_FRAME_ENABLE;
	} else {
		temp &= ~FDI_LINK_TRAIN_NONE;
		temp |= FDI_LINK_TRAIN_NONE | FDI_TX_ENHANCE_FRAME_ENABLE;
	}
	lg_wr(d, reg, temp);

	reg = FDI_RX_CTL(pipe);
	temp = lg_rd(d, reg);
	if (d->pch == LG_PCH_CPT) {
		temp &= ~FDI_LINK_TRAIN_PATTERN_MASK_CPT;
		temp |= FDI_LINK_TRAIN_NORMAL_CPT;
	} else {
		temp &= ~FDI_LINK_TRAIN_NONE;
		temp |= FDI_LINK_TRAIN_NONE;
	}
	lg_wr(d, reg, temp | FDI_RX_ENHANCE_FRAME_ENABLE);

	/* wait one idle pattern time */
	lg_posting_read(d, reg);
	lg_udelay(1000);

	/* Ivy Bridge wants error correction on. */
	if (d->is_ivb)
		lg_rmw(d, reg, 0, FDI_FS_ERRC_ENABLE | FDI_FE_ERRC_ENABLE);
}

/* The receiver checks the transfer unit size during training, so it must
 * know it first: the one the CPU transcoder's data M carries. */
static void fdi_write_tu_size(struct lg_display *d, int pipe)
{
	lg_wr(d, FDI_RX_TUSIZE1(pipe), lg_rd(d, PIPE_DATA_M1(d, pipe)) & TU_SIZE_MASK);
}

/* Unmask bit and symbol lock in the receiver's IIR for the result. */
static void fdi_unmask_lock_bits(struct lg_display *d, int pipe)
{
	uint32_t reg = FDI_RX_IMR(pipe);
	uint32_t temp = lg_rd(d, reg);

	temp &= ~FDI_RX_SYMBOL_LOCK;
	temp &= ~FDI_RX_BIT_LOCK;
	lg_wr(d, reg, temp);
	lg_posting_read(d, reg);
	lg_udelay(150);
}

/* Ironlake with Ibex Peak. */
static void ilk_fdi_link_train(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	uint32_t reg, temp, tries;

	fdi_write_tu_size(d, pipe);

	/* Train 1 */
	fdi_unmask_lock_bits(d, pipe);

	/* CPU FDI TX and PCH FDI RX on, pattern 1 */
	reg = FDI_TX_CTL(pipe);
	temp = lg_rd(d, reg);
	temp &= ~FDI_DP_PORT_WIDTH_MASK;
	temp |= FDI_DP_PORT_WIDTH(cfg->fdi_lanes);
	temp &= ~FDI_LINK_TRAIN_NONE;
	temp |= FDI_LINK_TRAIN_PATTERN_1;
	lg_wr(d, reg, temp | FDI_TX_ENABLE);

	reg = FDI_RX_CTL(pipe);
	temp = lg_rd(d, reg);
	temp &= ~FDI_LINK_TRAIN_NONE;
	temp |= FDI_LINK_TRAIN_PATTERN_1;
	lg_wr(d, reg, temp | FDI_RX_ENABLE);

	lg_posting_read(d, reg);
	lg_udelay(150);

	/* Ironlake: the clock pointer goes on only after FDI is enabled. */
	lg_wr(d, FDI_RX_CHICKEN(pipe), FDI_RX_PHASE_SYNC_POINTER_OVR);
	lg_wr(d, FDI_RX_CHICKEN(pipe),
	      FDI_RX_PHASE_SYNC_POINTER_OVR | FDI_RX_PHASE_SYNC_POINTER_EN);

	reg = FDI_RX_IIR(pipe);
	for (tries = 0; tries < 5; tries++) {
		temp = lg_rd(d, reg);
		i915_dbg("[drm] i915: FDI_RX_IIR 0x%x\n", temp);
		if (temp & FDI_RX_BIT_LOCK) {
			i915_dbg("[drm] i915: FDI train 1 done\n");
			lg_wr(d, reg, temp | FDI_RX_BIT_LOCK);
			break;
		}
	}
	if (tries == 5)
		kprintf("[drm] i915: FDI train 1 failed on pipe %c\n", pipe_name(pipe));

	/* Train 2 */
	lg_rmw(d, FDI_TX_CTL(pipe), FDI_LINK_TRAIN_NONE, FDI_LINK_TRAIN_PATTERN_2);
	lg_rmw(d, FDI_RX_CTL(pipe), FDI_LINK_TRAIN_NONE, FDI_LINK_TRAIN_PATTERN_2);
	lg_posting_read(d, FDI_RX_CTL(pipe));
	lg_udelay(150);

	reg = FDI_RX_IIR(pipe);
	for (tries = 0; tries < 5; tries++) {
		temp = lg_rd(d, reg);
		i915_dbg("[drm] i915: FDI_RX_IIR 0x%x\n", temp);
		if (temp & FDI_RX_SYMBOL_LOCK) {
			lg_wr(d, reg, temp | FDI_RX_SYMBOL_LOCK);
			i915_dbg("[drm] i915: FDI train 2 done\n");
			break;
		}
	}
	if (tries == 5)
		kprintf("[drm] i915: FDI train 2 failed on pipe %c\n", pipe_name(pipe));

	i915_dbg("[drm] i915: FDI train done\n");
}

/* Voltage swing / pre-emphasis steps of Sandy Bridge B and later. */
static const uint32_t snb_b_fdi_train_param[] = {
	FDI_LINK_TRAIN_400MV_0DB_SNB_B,
	FDI_LINK_TRAIN_400MV_6DB_SNB_B,
	FDI_LINK_TRAIN_600MV_3_5DB_SNB_B,
	FDI_LINK_TRAIN_800MV_0DB_SNB_B,
};
#define SNB_FDI_TRAIN_STEPS (sizeof(snb_b_fdi_train_param) / sizeof(snb_b_fdi_train_param[0]))

/* Sandy Bridge (Cougar Point, or Ibex Peak on early boards): each step of
 * swing and emphasis in turn until the receiver locks. */
static void gen6_fdi_link_train(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	uint32_t reg, temp, i, retry;

	fdi_write_tu_size(d, pipe);

	/* Train 1 */
	fdi_unmask_lock_bits(d, pipe);

	reg = FDI_TX_CTL(pipe);
	temp = lg_rd(d, reg);
	temp &= ~FDI_DP_PORT_WIDTH_MASK;
	temp |= FDI_DP_PORT_WIDTH(cfg->fdi_lanes);
	temp &= ~FDI_LINK_TRAIN_NONE;
	temp |= FDI_LINK_TRAIN_PATTERN_1;
	temp &= ~FDI_LINK_TRAIN_VOL_EMP_MASK;
	temp |= FDI_LINK_TRAIN_400MV_0DB_SNB_B;
	lg_wr(d, reg, temp | FDI_TX_ENABLE);

	lg_wr(d, FDI_RX_MISC(pipe), FDI_RX_TP1_TO_TP2_48 | FDI_RX_FDI_DELAY_90);

	reg = FDI_RX_CTL(pipe);
	temp = lg_rd(d, reg);
	if (d->pch == LG_PCH_CPT) {
		temp &= ~FDI_LINK_TRAIN_PATTERN_MASK_CPT;
		temp |= FDI_LINK_TRAIN_PATTERN_1_CPT;
	} else {
		temp &= ~FDI_LINK_TRAIN_NONE;
		temp |= FDI_LINK_TRAIN_PATTERN_1;
	}
	lg_wr(d, reg, temp | FDI_RX_ENABLE);

	lg_posting_read(d, reg);
	lg_udelay(150);

	for (i = 0; i < SNB_FDI_TRAIN_STEPS; i++) {
		lg_rmw(d, FDI_TX_CTL(pipe), FDI_LINK_TRAIN_VOL_EMP_MASK,
		       snb_b_fdi_train_param[i]);
		lg_posting_read(d, FDI_TX_CTL(pipe));
		lg_udelay(500);

		for (retry = 0; retry < 5; retry++) {
			reg = FDI_RX_IIR(pipe);
			temp = lg_rd(d, reg);
			i915_dbg("[drm] i915: FDI_RX_IIR 0x%x\n", temp);
			if (temp & FDI_RX_BIT_LOCK) {
				lg_wr(d, reg, temp | FDI_RX_BIT_LOCK);
				i915_dbg("[drm] i915: FDI train 1 done\n");
				break;
			}
			lg_udelay(50);
		}
		if (retry < 5)
			break;
	}
	if (i == SNB_FDI_TRAIN_STEPS)
		kprintf("[drm] i915: FDI train 1 failed on pipe %c\n", pipe_name(pipe));

	/* Train 2 */
	reg = FDI_TX_CTL(pipe);
	temp = lg_rd(d, reg);
	temp &= ~FDI_LINK_TRAIN_NONE;
	temp |= FDI_LINK_TRAIN_PATTERN_2;
	if (d->is_snb) {
		temp &= ~FDI_LINK_TRAIN_VOL_EMP_MASK;
		temp |= FDI_LINK_TRAIN_400MV_0DB_SNB_B;
	}
	lg_wr(d, reg, temp);

	reg = FDI_RX_CTL(pipe);
	temp = lg_rd(d, reg);
	if (d->pch == LG_PCH_CPT) {
		temp &= ~FDI_LINK_TRAIN_PATTERN_MASK_CPT;
		temp |= FDI_LINK_TRAIN_PATTERN_2_CPT;
	} else {
		temp &= ~FDI_LINK_TRAIN_NONE;
		temp |= FDI_LINK_TRAIN_PATTERN_2;
	}
	lg_wr(d, reg, temp);

	lg_posting_read(d, reg);
	lg_udelay(150);

	for (i = 0; i < SNB_FDI_TRAIN_STEPS; i++) {
		lg_rmw(d, FDI_TX_CTL(pipe), FDI_LINK_TRAIN_VOL_EMP_MASK,
		       snb_b_fdi_train_param[i]);
		lg_posting_read(d, FDI_TX_CTL(pipe));
		lg_udelay(500);

		for (retry = 0; retry < 5; retry++) {
			reg = FDI_RX_IIR(pipe);
			temp = lg_rd(d, reg);
			i915_dbg("[drm] i915: FDI_RX_IIR 0x%x\n", temp);
			if (temp & FDI_RX_SYMBOL_LOCK) {
				lg_wr(d, reg, temp | FDI_RX_SYMBOL_LOCK);
				i915_dbg("[drm] i915: FDI train 2 done\n");
				break;
			}
			lg_udelay(50);
		}
		if (retry < 5)
			break;
	}
	if (i == SNB_FDI_TRAIN_STEPS)
		kprintf("[drm] i915: FDI train 2 failed on pipe %c\n", pipe_name(pipe));

	i915_dbg("[drm] i915: FDI train done\n");
}

/* Ivy Bridge: manual training, every swing/emphasis step tried twice,
 * the link taken down and up again between attempts. */
static void ivb_manual_fdi_link_train(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	uint32_t reg, temp, i, j;

	ivb_update_fdi_bc_bifurcation(d, cfg);

	fdi_write_tu_size(d, pipe);

	/* Train 1 */
	fdi_unmask_lock_bits(d, pipe);

	i915_dbg("[drm] i915: FDI_RX_IIR before link train 0x%x\n",
		 lg_rd(d, FDI_RX_IIR(pipe)));

	for (j = 0; j < SNB_FDI_TRAIN_STEPS * 2; j++) {
		/* off first, in case this is a retry */
		reg = FDI_TX_CTL(pipe);
		temp = lg_rd(d, reg);
		temp &= ~(FDI_LINK_TRAIN_AUTO | FDI_LINK_TRAIN_NONE_IVB);
		temp &= ~FDI_TX_ENABLE;
		lg_wr(d, reg, temp);

		reg = FDI_RX_CTL(pipe);
		temp = lg_rd(d, reg);
		temp &= ~FDI_LINK_TRAIN_AUTO;
		temp &= ~FDI_LINK_TRAIN_PATTERN_MASK_CPT;
		temp &= ~FDI_RX_ENABLE;
		lg_wr(d, reg, temp);

		/* CPU FDI TX and PCH FDI RX on */
		reg = FDI_TX_CTL(pipe);
		temp = lg_rd(d, reg);
		temp &= ~FDI_DP_PORT_WIDTH_MASK;
		temp |= FDI_DP_PORT_WIDTH(cfg->fdi_lanes);
		temp |= FDI_LINK_TRAIN_PATTERN_1_IVB;
		temp &= ~FDI_LINK_TRAIN_VOL_EMP_MASK;
		temp |= snb_b_fdi_train_param[j / 2];
		temp |= FDI_COMPOSITE_SYNC;
		lg_wr(d, reg, temp | FDI_TX_ENABLE);

		lg_wr(d, FDI_RX_MISC(pipe), FDI_RX_TP1_TO_TP2_48 | FDI_RX_FDI_DELAY_90);

		reg = FDI_RX_CTL(pipe);
		temp = lg_rd(d, reg);
		temp |= FDI_LINK_TRAIN_PATTERN_1_CPT;
		temp |= FDI_COMPOSITE_SYNC;
		lg_wr(d, reg, temp | FDI_RX_ENABLE);

		lg_posting_read(d, reg);
		lg_udelay(1); /* 0.5 us is enough */

		for (i = 0; i < 4; i++) {
			reg = FDI_RX_IIR(pipe);
			temp = lg_rd(d, reg);
			i915_dbg("[drm] i915: FDI_RX_IIR 0x%x\n", temp);
			if ((temp & FDI_RX_BIT_LOCK) || (lg_rd(d, reg) & FDI_RX_BIT_LOCK)) {
				lg_wr(d, reg, temp | FDI_RX_BIT_LOCK);
				i915_dbg("[drm] i915: FDI train 1 done, level %u\n", i);
				break;
			}
			lg_udelay(1); /* 0.5 us is enough */
		}
		if (i == 4) {
			i915_dbg("[drm] i915: FDI train 1 fail on vswing %u\n", j / 2);
			continue;
		}

		/* Train 2 */
		lg_rmw(d, FDI_TX_CTL(pipe), FDI_LINK_TRAIN_NONE_IVB,
		       FDI_LINK_TRAIN_PATTERN_2_IVB);
		lg_rmw(d, FDI_RX_CTL(pipe), FDI_LINK_TRAIN_PATTERN_MASK_CPT,
		       FDI_LINK_TRAIN_PATTERN_2_CPT);
		lg_posting_read(d, FDI_RX_CTL(pipe));
		lg_udelay(2); /* 1.5 us is enough */

		for (i = 0; i < 4; i++) {
			reg = FDI_RX_IIR(pipe);
			temp = lg_rd(d, reg);
			i915_dbg("[drm] i915: FDI_RX_IIR 0x%x\n", temp);
			if ((temp & FDI_RX_SYMBOL_LOCK) ||
			    (lg_rd(d, reg) & FDI_RX_SYMBOL_LOCK)) {
				lg_wr(d, reg, temp | FDI_RX_SYMBOL_LOCK);
				i915_dbg("[drm] i915: FDI train 2 done, level %u\n", i);
				goto train_done;
			}
			lg_udelay(2); /* 1.5 us is enough */
		}
		i915_dbg("[drm] i915: FDI train 2 fail on vswing %u\n", j / 2);
	}
	kprintf("[drm] i915: FDI link training failed on pipe %c\n", pipe_name(pipe));

train_done:
	i915_dbg("[drm] i915: FDI train done\n");
}

static void fdi_link_train(struct lg_display *d, const struct lg_config *cfg)
{
	if (d->is_ivb)
		ivb_manual_fdi_link_train(d, cfg);
	else if (d->is_snb)
		gen6_fdi_link_train(d, cfg);
	else
		ilk_fdi_link_train(d, cfg);
}

/* ---- FDI PLLs and the link on and off ------------------------------------------------ */

/* The pipe's bits per colour as PIPECONF holds them (bits 7:5), moved to
 * where FDI_RX_CTL keeps them (18:16). */
static uint32_t fdi_rx_bpc_from_pipeconf(struct lg_display *d, int pipe)
{
	return (lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_BPC_MASK) << 11;
}

void lg_fdi_pll_enable(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	uint32_t reg, temp;

	/* PCH FDI RX PLL on; wait out its warm-up plus the DMI latency. */
	reg = FDI_RX_CTL(pipe);
	temp = lg_rd(d, reg);
	temp &= ~(FDI_DP_PORT_WIDTH_MASK | FDI_BPC_MASK);
	temp |= FDI_DP_PORT_WIDTH(cfg->fdi_lanes);
	temp |= fdi_rx_bpc_from_pipeconf(d, pipe);
	lg_wr(d, reg, temp | FDI_RX_PLL_ENABLE);

	lg_posting_read(d, reg);
	lg_udelay(200);

	/* from the raw clock to the PCD clock */
	lg_rmw(d, reg, 0, FDI_PCDCLK);
	lg_posting_read(d, reg);
	lg_udelay(200);

	/* CPU FDI TX PLL on (always on, on Ironlake) */
	reg = FDI_TX_CTL(pipe);
	temp = lg_rd(d, reg);
	if ((temp & FDI_TX_PLL_ENABLE) == 0) {
		lg_wr(d, reg, temp | FDI_TX_PLL_ENABLE);
		lg_posting_read(d, reg);
		lg_udelay(100);
	}
}

static void ilk_fdi_pll_disable(struct lg_display *d, int pipe)
{
	/* from the PCD clock back to the raw clock */
	lg_rmw(d, FDI_RX_CTL(pipe), FDI_PCDCLK, 0);

	/* CPU FDI TX PLL off */
	lg_rmw(d, FDI_TX_CTL(pipe), FDI_TX_PLL_ENABLE, 0);
	lg_posting_read(d, FDI_TX_CTL(pipe));
	lg_udelay(100);

	/* PCH FDI RX PLL off; let the clocks stop. */
	lg_rmw(d, FDI_RX_CTL(pipe), FDI_RX_PLL_ENABLE, 0);
	lg_posting_read(d, FDI_RX_CTL(pipe));
	lg_udelay(100);
}

static void ilk_fdi_disable(struct lg_display *d, int pipe)
{
	uint32_t reg, temp;

	/* CPU FDI TX and PCH FDI RX off */
	lg_rmw(d, FDI_TX_CTL(pipe), FDI_TX_ENABLE, 0);
	lg_posting_read(d, FDI_TX_CTL(pipe));

	reg = FDI_RX_CTL(pipe);
	temp = lg_rd(d, reg);
	temp &= ~FDI_BPC_MASK;
	temp |= fdi_rx_bpc_from_pipeconf(d, pipe);
	lg_wr(d, reg, temp & ~FDI_RX_ENABLE);

	lg_posting_read(d, reg);
	lg_udelay(100);

	/* Ironlake: the clock pointer goes off after FDI is down. */
	if (d->pch == LG_PCH_IBX)
		lg_wr(d, FDI_RX_CHICKEN(pipe), FDI_RX_PHASE_SYNC_POINTER_OVR);

	/* Leave both ends on training pattern 1. */
	lg_rmw(d, FDI_TX_CTL(pipe), FDI_LINK_TRAIN_NONE, FDI_LINK_TRAIN_PATTERN_1);

	reg = FDI_RX_CTL(pipe);
	temp = lg_rd(d, reg);
	if (d->pch == LG_PCH_CPT) {
		temp &= ~FDI_LINK_TRAIN_PATTERN_MASK_CPT;
		temp |= FDI_LINK_TRAIN_PATTERN_1_CPT;
	} else {
		temp &= ~FDI_LINK_TRAIN_NONE;
		temp |= FDI_LINK_TRAIN_PATTERN_1;
	}
	/* the receiver's bpc follows PIPECONF */
	temp &= ~FDI_BPC_MASK;
	temp |= fdi_rx_bpc_from_pipeconf(d, pipe);
	lg_wr(d, reg, temp);

	lg_posting_read(d, reg);
	lg_udelay(100);
}

/* ---- PCH transcoders ----------------------------------------------------------------- */

static void pch_transcoder_set_m1_n1(struct lg_display *d, int pipe,
				     const struct lg_link_m_n *m_n)
{
	lg_wr(d, PCH_TRANS_DATA_M1(pipe), TU_SIZE(m_n->tu) | m_n->data_m);
	lg_wr(d, PCH_TRANS_DATA_N1(pipe), m_n->data_n);
	lg_wr(d, PCH_TRANS_LINK_M1(pipe), m_n->link_m);
	lg_wr(d, PCH_TRANS_LINK_N1(pipe), m_n->link_n);
}

/* The PCH transcoder runs the CPU transcoder's timing. */
static void ilk_pch_transcoder_set_timings(struct lg_display *d, int pipe)
{
	lg_wr(d, PCH_TRANS_HTOTAL(pipe), lg_rd(d, TRANS_HTOTAL(d, pipe)));
	lg_wr(d, PCH_TRANS_HBLANK(pipe), lg_rd(d, TRANS_HBLANK(d, pipe)));
	lg_wr(d, PCH_TRANS_HSYNC(pipe), lg_rd(d, TRANS_HSYNC(d, pipe)));

	lg_wr(d, PCH_TRANS_VTOTAL(pipe), lg_rd(d, TRANS_VTOTAL(d, pipe)));
	lg_wr(d, PCH_TRANS_VBLANK(pipe), lg_rd(d, TRANS_VBLANK(d, pipe)));
	lg_wr(d, PCH_TRANS_VSYNC(pipe), lg_rd(d, TRANS_VSYNC(d, pipe)));
	lg_wr(d, PCH_TRANS_VSYNCSHIFT(pipe), lg_rd(d, TRANS_VSYNCSHIFT(d, pipe)));
}

static void ilk_enable_pch_transcoder(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	uint32_t reg, val, pipeconf_val;

	if (d->pch == LG_PCH_CPT) {
		reg = TRANS_CHICKEN2(pipe);
		val = lg_rd(d, reg);
		/* The timing override must be set before the transcoder is
		 * enabled. */
		val |= TRANS_CHICKEN2_TIMING_OVERRIDE;
		/* frame start delay as the CPU pipe's (one frame start) */
		val &= ~TRANS_CHICKEN2_FRAME_START_DELAY_MASK;
		val |= TRANS_CHICKEN2_FRAME_START_DELAY(0);
		lg_wr(d, reg, val);
	}

	reg = PCH_TRANSCONF(pipe);
	val = lg_rd(d, reg);
	pipeconf_val = lg_rd(d, PIPECONF(d, pipe));

	if (d->pch == LG_PCH_IBX) {
		/* frame start delay as the CPU pipe's */
		val &= ~TRANS_FRAME_START_DELAY_MASK;
		val |= TRANS_FRAME_START_DELAY(0);

		/* The transcoder's bpc follows PIPECONF, except that HDMI
		 * takes 8 bpc here for both 8 and 12 bpc. */
		val &= ~TRANS_BPC_MASK;
		if (lg_cfg_has(cfg, LG_OUTPUT_HDMI))
			val |= TRANS_BPC_8;
		else
			val |= pipeconf_val & PIPECONF_BPC_MASK;
	}

	val &= ~TRANS_INTERLACE_MASK;
	if ((pipeconf_val & PIPECONF_INTERLACE_MASK) == PIPECONF_INTERLACE_IF_ID_ILK) {
		if (d->pch == LG_PCH_IBX && lg_cfg_has(cfg, LG_OUTPUT_SDVO))
			val |= TRANS_INTERLACE_LEGACY_VSYNC_IBX;
		else
			val |= TRANS_INTERLACE_INTERLACED;
	} else {
		val |= TRANS_INTERLACE_PROGRESSIVE;
	}

	lg_wr(d, reg, val | TRANS_ENABLE);
	if (lg_wait(d, reg, TRANS_STATE_ENABLE, TRANS_STATE_ENABLE, 100000))
		kprintf("[drm] i915: failed to enable PCH transcoder %c\n", pipe_name(pipe));
}

static void ilk_disable_pch_transcoder(struct lg_display *d, int pipe)
{
	uint32_t reg = PCH_TRANSCONF(pipe);

	lg_rmw(d, reg, TRANS_ENABLE, 0);
	/* wait for the transcoder to report itself off */
	if (lg_wait(d, reg, TRANS_STATE_ENABLE, 0, 50000))
		kprintf("[drm] i915: failed to disable PCH transcoder %c\n", pipe_name(pipe));

	/* Clear the timing override again. */
	if (d->pch == LG_PCH_CPT)
		lg_rmw(d, TRANS_CHICKEN2(pipe), TRANS_CHICKEN2_TIMING_OVERRIDE, 0);
}

/* Cougar Point: the transcoder feeds a DisplayPort port itself. */
static void cpt_set_trans_dp_ctl(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;
	uint32_t reg = TRANS_DP_CTL(pipe);
	uint32_t bpc = (lg_rd(d, PIPECONF(d, pipe)) & PIPECONF_BPC_MASK) >> 5;
	uint32_t temp;
	int port = -1;

	if (cfg->output >= 0 && cfg->output < d->nout)
		port = d->out[cfg->output].port;
	if (port < LG_PORT_B || port > LG_PORT_D) {
		kprintf("[drm] i915: PCH DP on pipe %c has no port B..D (%d)\n",
			pipe_name(pipe), port);
		return;
	}

	temp = lg_rd(d, reg);
	temp &= ~(TRANS_DP_PORT_SEL_MASK | TRANS_DP_VSYNC_ACTIVE_HIGH |
		  TRANS_DP_HSYNC_ACTIVE_HIGH | TRANS_DP_BPC_MASK | TRANS_DP_ENH_FRAMING);
	temp |= TRANS_DP_OUTPUT_ENABLE;
	temp |= bpc << 9; /* the PIPECONF format, at bits 10:9 */
	if (cfg->enhanced_framing)
		temp |= TRANS_DP_ENH_FRAMING;

	if (cfg->t.flags & DRM_MODE_FLAG_PHSYNC)
		temp |= TRANS_DP_HSYNC_ACTIVE_HIGH;
	if (cfg->t.flags & DRM_MODE_FLAG_PVSYNC)
		temp |= TRANS_DP_VSYNC_ACTIVE_HIGH;

	temp |= TRANS_DP_PORT_SEL(port - LG_PORT_B);

	lg_wr(d, reg, temp);
}

void lg_pch_enable(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;

	/* Train the FDI link to the PCH. */
	fdi_link_train(d, cfg);

	/* The PCH PLL may come on any time before the transcoder; it also
	 * routes the PLL to the transcoder on Cougar Point, which must
	 * happen before the pixel multiplier is written to the PLL. */
	lg_pch_dpll_enable(d, cfg);

	/* the transcoder's timing (the panel sequencer is unlocked) */
	if (cfg->has_dp_encoder)
		pch_transcoder_set_m1_n1(d, pipe, &cfg->dp_m_n);
	ilk_pch_transcoder_set_timings(d, pipe);

	fdi_normal_train(d, pipe);

	if (d->pch == LG_PCH_CPT && cfg->has_dp_encoder)
		cpt_set_trans_dp_ctl(d, cfg);

	ilk_enable_pch_transcoder(d, cfg);
}

void lg_pch_disable(struct lg_display *d, const struct lg_config *cfg)
{
	ilk_fdi_disable(d, cfg->pipe);
}

void lg_pch_post_disable(struct lg_display *d, const struct lg_config *cfg)
{
	int pipe = cfg->pipe;

	ilk_disable_pch_transcoder(d, pipe);

	if (d->pch == LG_PCH_CPT)
		lg_rmw(d, TRANS_DP_CTL(pipe), TRANS_DP_OUTPUT_ENABLE | TRANS_DP_PORT_SEL_MASK,
		       TRANS_DP_PORT_SEL_NONE);

	ilk_fdi_pll_disable(d, pipe);

	lg_pch_dpll_disable(d, cfg);
}
