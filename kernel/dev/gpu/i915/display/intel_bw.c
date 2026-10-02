// LikeOS -- the memory behind the display: DRAM, bandwidth, SAGV.
//
// The memory controller may move between frequency points (QGV points,
// "SAGV" when it does so by itself) to save power; while it switches the
// display cannot read, and at the lower points it gets less bandwidth.
// What memory is fitted, how many channels and points it has, and their
// timings come from the memory controller's registers (Skylake to Ice
// Lake), the pcode mailbox (Ice Lake to Alder Lake) or a block of display
// registers (Meteor Lake on).  From them the bandwidth each point
// delivers to 1..N planes is worked out once, and every configuration is
// checked against it: on display versions 11 to 13 the pcode is told
// which points the display cannot live with -- the restriction widened
// before a change and narrowed after it -- and on version 14 the peak
// bandwidth of the point to hold goes into the PM demand request.
// Before Ice Lake SAGV is simply allowed or not, depending on whether
// every plane has a watermark level that rides out the switch.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/i915/intel_wm_regs.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define NUM_QGV_POINTS 8
#define NUM_PSF_GV_POINTS 3
#define NUM_BW_GROUPS 6
#define DEPROGBWPCLIMIT 60
#define PEAK_BW_THRESHOLD 20000

enum sagv_status {
	SAGV_UNKNOWN = 0,
	SAGV_DISABLED,
	SAGV_ENABLED,
	SAGV_NOT_CONTROLLED,
};

struct qgv_point {
	uint16_t dclk, t_rp, t_rdpre, t_rc, t_ras, t_rcd;
};

struct qgv_info {
	struct qgv_point points[NUM_QGV_POINTS];
	uint8_t psf_clk[NUM_PSF_GV_POINTS]; /* multiples of 16.666 MHz */
	uint8_t num_qgv_points;
	uint8_t num_psf_points;
	uint8_t t_bl;
	uint8_t max_numchannels;
	uint8_t channel_width;
	uint8_t deinterleave;
};

struct bw_info {
	uint32_t deratedbw[NUM_QGV_POINTS]; /* MB/s */
	uint8_t num_planes;
};

static struct {
	struct intel_dram_info dram;
	struct bw_info max[NUM_BW_GROUPS];
	uint32_t psf_bw[NUM_PSF_GV_POINTS];
	uint32_t peakbw[NUM_QGV_POINTS];
	uint8_t num_qgv_points;
	uint8_t num_psf_gv_points;
	int sagv_status;
	uint32_t sagv_block_time_us;
	uint16_t qgv_points_mask; /* what the pcode was told to avoid */
	uint16_t new_mask; /* the next configuration's, between pre and post */
	uint16_t qgv_peakbw; /* display version 14: for PM demand */
	int ver;
	int has_sagv_hw; /* the platform has SAGV at all */
} g_bw;

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t div_round_up(uint32_t a, uint32_t b)
{
	return b ? (a + b - 1) / b : 0;
}

static uint32_t div_round_closest(uint32_t a, uint32_t b)
{
	return b ? (a + b / 2) / b : 0;
}

static int is_power_of_2(uint32_t v)
{
	return v && !(v & (v - 1));
}

static int plat_is(struct i915_device *i915, int platform)
{
	return i915->info->platform == platform;
}

static uint32_t mchbar_read(struct i915_device *i915, uint32_t reg)
{
	return i915_read32(i915, reg);
}

/* ---- DRAM ---------------------------------------------------------------------------- */

struct dimm_info {
	uint16_t size; /* Gb for the whole DIMM */
	uint8_t width, ranks;
};

struct channel_info {
	struct dimm_info dimm_l, dimm_s;
	uint8_t ranks;
	uint8_t is_16gb_dimm;
};

static int dimm_num_devices(const struct dimm_info *d)
{
	return d->ranks * 64 / (d->width ? d->width : 1);
}

static int is_16gb_dimm(const struct dimm_info *d)
{
	int n = dimm_num_devices(d);
	return d->size / (n ? n : 1) >= 16;
}

/* The DIMMs of a channel: display version 11 widened the fields. */
static void skl_dimm_info(int ver, uint32_t val, int l, struct dimm_info *d)
{
	uint32_t size, width, rank;

	if (ver >= 11) {
		size = l ? (val & 0x7f) : ((val >> 16) & 0x7f);
		width = l ? ((val >> 7) & 3) : ((val >> 24) & 3);
		rank = l ? ((val >> 9) & 3) : ((val >> 26) & 3);
		d->size = (uint16_t)(size * 8 / 2);
	} else {
		size = l ? (val & 0x3f) : ((val >> 16) & 0x3f);
		width = l ? ((val >> 8) & 3) : ((val >> 24) & 3);
		rank = l ? ((val >> 10) & 1) : ((val >> 26) & 1);
		d->size = (uint16_t)(size * 8);
	}
	if (d->size == 0) {
		d->width = 0;
		d->ranks = 0;
		return;
	}
	d->width = width <= 2 ? (uint8_t)(8 << width) : 0;
	d->ranks = (uint8_t)(rank + 1);
}

static int skl_channel_info(int ver, uint32_t val, struct channel_info *ch)
{
	skl_dimm_info(ver, val, 1, &ch->dimm_l);
	skl_dimm_info(ver, val, 0, &ch->dimm_s);
	if (ch->dimm_l.size == 0 && ch->dimm_s.size == 0)
		return -EINVAL;
	if (ch->dimm_l.ranks == 2 || ch->dimm_s.ranks == 2)
		ch->ranks = 2;
	else if (ch->dimm_l.ranks == 1 && ch->dimm_s.ranks == 1)
		ch->ranks = 2;
	else
		ch->ranks = 1;
	ch->is_16gb_dimm = is_16gb_dimm(&ch->dimm_l) || is_16gb_dimm(&ch->dimm_s);
	return 0;
}

static int channels_equal(const struct channel_info *a, const struct channel_info *b)
{
	return a->ranks == b->ranks && a->is_16gb_dimm == b->is_16gb_dimm &&
	       a->dimm_l.size == b->dimm_l.size && a->dimm_l.width == b->dimm_l.width &&
	       a->dimm_l.ranks == b->dimm_l.ranks && a->dimm_s.size == b->dimm_s.size &&
	       a->dimm_s.width == b->dimm_s.width && a->dimm_s.ranks == b->dimm_s.ranks;
}

static int skl_dram_get_channels_info(struct i915_device *i915, struct intel_dram_info *di,
				      int ver)
{
	struct channel_info ch0, ch1;

	mm_memset(&ch0, 0, sizeof(ch0));
	mm_memset(&ch1, 0, sizeof(ch1));
	/* 16 Gb DIMMs are assumed until shown otherwise */
	di->has_16gb_dimms = 1;
	if (skl_channel_info(ver, mchbar_read(i915, SKL_MAD_DIMM_CH0), &ch0) == 0)
		di->num_channels++;
	if (skl_channel_info(ver, mchbar_read(i915, SKL_MAD_DIMM_CH1), &ch1) == 0)
		di->num_channels++;
	if (di->num_channels == 0) {
		kprintf("[drm] i915: the memory controller reports no channels\n");
		return -EINVAL;
	}
	if (ch0.ranks == 0 && ch1.ranks == 0)
		return -EINVAL;
	di->has_16gb_dimms = ch0.is_16gb_dimm || ch1.is_16gb_dimm;
	di->symmetric_memory = channels_equal(&ch0, &ch1) &&
			       (ch0.dimm_s.size == 0 ||
				(ch0.dimm_l.size == ch0.dimm_s.size &&
				 ch0.dimm_l.width == ch0.dimm_s.width &&
				 ch0.dimm_l.ranks == ch0.dimm_s.ranks));
	return 0;
}

static int skl_get_dram_info(struct i915_device *i915, struct intel_dram_info *di)
{
	switch (mchbar_read(i915, SKL_MAD_INTER_CHANNEL) & SKL_DRAM_DDR_TYPE_MASK) {
	case SKL_DRAM_DDR_TYPE_DDR3: di->type = INTEL_DRAM_DDR3; break;
	case SKL_DRAM_DDR_TYPE_DDR4: di->type = INTEL_DRAM_DDR4; break;
	case SKL_DRAM_DDR_TYPE_LPDDR3: di->type = INTEL_DRAM_LPDDR3; break;
	default: di->type = INTEL_DRAM_LPDDR4; break;
	}
	return skl_dram_get_channels_info(i915, di, 9);
}

/* Broxton / Gemini Lake: one DUNIT register per channel. */
static int bxt_dimm_size(uint32_t val)
{
	switch ((val >> 6) & 7) {
	case 0: return 4;
	case 1: return 6;
	case 2: return 8;
	case 3: return 12;
	case 4: return 16;
	default: return 0;
	}
}

static int bxt_get_dram_info(struct i915_device *i915, struct intel_dram_info *di)
{
	uint8_t valid_ranks = 0;

	for (int i = BXT_D_CR_DRP0_DUNIT_START; i <= BXT_D_CR_DRP0_DUNIT_END; i++) {
		uint32_t val = mchbar_read(i915, BXT_D_CR_DRP0_DUNIT(i));
		int size, ranks = 0, type = INTEL_DRAM_UNKNOWN;

		if (val == 0xffffffffu)
			continue;
		di->num_channels++;
		size = bxt_dimm_size(val);
		if (size) {
			switch (val & 3) {
			case 1: ranks = 1; break;
			case 3: ranks = 2; break;
			default: ranks = 0; break;
			}
			switch ((val >> 22) & 7) {
			case 0: type = INTEL_DRAM_DDR3; break;
			case 1: type = INTEL_DRAM_LPDDR3; break;
			case 2: type = INTEL_DRAM_LPDDR4; break;
			case 4: type = INTEL_DRAM_DDR4; break;
			default: type = INTEL_DRAM_UNKNOWN; break;
			}
		}
		if (valid_ranks == 0)
			valid_ranks = (uint8_t)ranks;
		if (type != INTEL_DRAM_UNKNOWN)
			di->type = (uint8_t)type;
	}
	if (di->type == INTEL_DRAM_UNKNOWN || valid_ranks == 0)
		return -EINVAL;
	return 0;
}

static int icl_pcode_read_mem_global_info(struct i915_device *i915, struct intel_dram_info *di,
					  int ver)
{
	uint32_t val = 0;
	int ret;

	ret = intel_pcode_rw(i915, ICL_PCODE_MEM_SUBSYSYSTEM_INFO | ICL_PCODE_MEM_SS_READ_GLOBAL_INFO,
			     &val, NULL, 500, 20, 1);
	if (ret)
		return ret;
	if (ver >= 12) {
		switch (val & 0xf) {
		case 0: di->type = INTEL_DRAM_DDR4; break;
		case 1: di->type = INTEL_DRAM_DDR5; break;
		case 2: di->type = INTEL_DRAM_LPDDR5; break;
		case 3: di->type = INTEL_DRAM_LPDDR4; break;
		case 4: di->type = INTEL_DRAM_DDR3; break;
		case 5: di->type = INTEL_DRAM_LPDDR3; break;
		default: return -EINVAL;
		}
	} else {
		switch (val & 0xf) {
		case 0: di->type = INTEL_DRAM_DDR4; break;
		case 1: di->type = INTEL_DRAM_DDR3; break;
		case 2: di->type = INTEL_DRAM_LPDDR3; break;
		case 3: di->type = INTEL_DRAM_LPDDR4; break;
		default: return -EINVAL;
		}
	}
	di->num_channels = (uint8_t)((val & 0xf0) >> 4);
	di->num_qgv_points = (uint8_t)((val & 0xf00) >> 8);
	di->num_psf_gv_points = (uint8_t)((val & 0x3000) >> 12);
	return 0;
}

static int xelpdp_get_dram_info(struct i915_device *i915, struct intel_dram_info *di, int ver)
{
	uint32_t val = i915_read32(i915, MEM_SS_INFO_GLOBAL);

	switch (MEM_SS_DDR_TYPE(val)) {
	case 0: di->type = INTEL_DRAM_DDR4; break;
	case 1: di->type = INTEL_DRAM_DDR5; break;
	case 2: di->type = INTEL_DRAM_LPDDR5; break;
	case 3: di->type = INTEL_DRAM_LPDDR4; break;
	case 4: di->type = INTEL_DRAM_DDR3; break;
	case 5: di->type = INTEL_DRAM_LPDDR3; break;
	case 8: di->type = INTEL_DRAM_GDDR; break;
	case 9: di->type = INTEL_DRAM_GDDR_ECC; break;
	default: return -EINVAL;
	}
	di->num_channels = (uint8_t)MEM_SS_POPULATED_CH(val);
	di->num_qgv_points = (uint8_t)MEM_SS_ENABLED_QGV_POINTS(val);
	/* Wa_16030862157: a saturated channel field means sixteen */
	if (ver == 35 && di->num_channels == 0xf)
		di->num_channels = 16;
	if (ver >= 35)
		di->ecc_impacting_de_bw = !!(val & (1u << 15));
	return 0;
}

int intel_dram_detect(struct i915_device *i915)
{
	struct intel_dram_info *di = &g_bw.dram;
	int ver = intel_wm_display_ver(i915);
	int ret;

	mm_memset(di, 0, sizeof(*di));
	if (plat_is(i915, I915_PLATFORM_DG2) || ver < 9)
		return 0;
	if (ver >= 14)
		ret = xelpdp_get_dram_info(i915, di, ver);
	else if (ver >= 12)
		ret = icl_pcode_read_mem_global_info(i915, di, ver);
	else if (ver >= 11) {
		ret = skl_dram_get_channels_info(i915, di, ver);
		if (ret == 0)
			ret = icl_pcode_read_mem_global_info(i915, di, ver);
	} else if (plat_is(i915, I915_PLATFORM_BROXTON) || plat_is(i915, I915_PLATFORM_GEMINILAKE))
		ret = bxt_get_dram_info(i915, di);
	else
		ret = skl_get_dram_info(i915, di);
	di->detected = ret == 0;
	i915_dbg("[drm] i915: DRAM type %u, %u channels, %u QGV points, %u PSF points, symmetric %s, 16 Gb DIMMs %s%s\n",
		 di->type, di->num_channels, di->num_qgv_points, di->num_psf_gv_points,
		 di->symmetric_memory ? "yes" : "no", di->has_16gb_dimms ? "yes" : "no",
		 ret ? " (incomplete)" : "");
	return 0;
}

const struct intel_dram_info *intel_dram_get(struct i915_device *i915)
{
	(void)i915;
	return &g_bw.dram;
}

/* ---- QGV points -------------------------------------------------------------------------- */

static int dclk_freq_mhz(uint32_t ratio)
{
	/* multiples of 16.666 MHz (100/6) */
	return (int)div_round_closest(ratio * 100, 6);
}

static int dg1_mchbar_read_qgv_point_info(struct i915_device *i915, struct qgv_point *sp)
{
	uint32_t val = mchbar_read(i915, SA_PERF_STATUS_0_0_0_MCHBAR_PC);
	uint32_t dclk_ratio = DG1_QCLK_RATIO(val);

	if (val & DG1_QCLK_REFERENCE)
		dclk_ratio *= 6; /* 100 MHz */
	else
		dclk_ratio *= 8; /* 133 MHz */
	val = mchbar_read(i915, SKL_MC_BIOS_DATA_0_0_0_MCHBAR_PCU);
	if (val & DG1_GEAR_TYPE)
		dclk_ratio *= 2;
	sp->dclk = (uint16_t)dclk_freq_mhz(dclk_ratio);
	if (sp->dclk == 0)
		return -EINVAL;
	val = mchbar_read(i915, MCHBAR_CH0_CR_TC_PRE_0_0_0_MCHBAR);
	sp->t_rp = val & 0x7f;
	sp->t_rdpre = (val >> 11) & 0x3f;
	val = mchbar_read(i915, MCHBAR_CH0_CR_TC_PRE_0_0_0_MCHBAR_HIGH);
	sp->t_rcd = (val >> 9) & 0x7f;
	sp->t_ras = (val >> 1) & 0xff;
	sp->t_rc = sp->t_rp + sp->t_ras;
	return 0;
}

static int icl_pcode_read_qgv_point_info(struct i915_device *i915, struct qgv_point *sp,
					 int point)
{
	uint32_t val = 0, val2 = 0;
	int ret;

	ret = intel_pcode_rw(i915, ICL_PCODE_MEM_SUBSYSYSTEM_INFO |
				   ICL_PCODE_MEM_SS_READ_QGV_POINT_INFO(point),
			     &val, &val2, 500, 20, 1);
	if (ret)
		return ret;
	sp->dclk = (uint16_t)dclk_freq_mhz(val & 0xffff);
	sp->t_rp = (val & 0xff0000) >> 16;
	sp->t_rcd = (val & 0xff000000) >> 24;
	sp->t_rdpre = val2 & 0xff;
	sp->t_ras = (val2 & 0xff00) >> 8;
	sp->t_rc = sp->t_rp + sp->t_ras;
	return 0;
}

static int mtl_read_qgv_point_info(struct i915_device *i915, struct qgv_point *sp, int point)
{
	uint32_t val = i915_read32(i915, MEM_SS_INFO_QGV_POINT_LOW(point));
	uint32_t val2 = i915_read32(i915, MEM_SS_INFO_QGV_POINT_HIGH(point));

	sp->dclk = (uint16_t)dclk_freq_mhz(MTL_DCLK(val));
	sp->t_rp = (uint16_t)MTL_TRP(val);
	sp->t_rcd = (uint16_t)MTL_TRCD(val);
	sp->t_rdpre = (uint16_t)MTL_TRDPRE(val2);
	sp->t_ras = (uint16_t)MTL_TRAS(val2);
	sp->t_rc = sp->t_rp + sp->t_ras;
	return 0;
}

static int read_qgv_point_info(struct i915_device *i915, struct qgv_point *sp, int point)
{
	if (g_bw.ver >= 14)
		return mtl_read_qgv_point_info(i915, sp, point);
	if (plat_is(i915, I915_PLATFORM_DG1))
		return dg1_mchbar_read_qgv_point_info(i915, sp);
	return icl_pcode_read_qgv_point_info(i915, sp, point);
}

static int adls_pcode_read_psf_gv_point_info(struct i915_device *i915, uint8_t *clk)
{
	uint32_t val = 0;
	int ret = intel_pcode_rw(i915, ICL_PCODE_MEM_SUBSYSYSTEM_INFO |
					   ADL_PCODE_MEM_SS_READ_PSF_GV_INFO,
				 &val, NULL, 500, 20, 1);
	if (ret)
		return ret;
	for (int i = 0; i < NUM_PSF_GV_POINTS; i++) {
		clk[i] = val & 0xff;
		val >>= 8;
	}
	return 0;
}

/* Y tiling is assumed possible wherever the part has it. */
static int is_y_tile(struct i915_device *i915)
{
	return !(plat_is(i915, I915_PLATFORM_DG2) || g_bw.ver >= 14);
}

static int icl_init_qgv_info(struct i915_device *i915, const struct intel_dram_info *di,
			     struct qgv_info *qi)
{
	qi->num_qgv_points = di->num_qgv_points;
	qi->num_psf_points = di->num_psf_gv_points;

	if (g_bw.ver >= 14) {
		switch (di->type) {
		case INTEL_DRAM_DDR4:
			qi->t_bl = 4; qi->max_numchannels = 2; qi->channel_width = 64;
			qi->deinterleave = 2;
			break;
		case INTEL_DRAM_DDR5:
			qi->t_bl = 8; qi->max_numchannels = 4; qi->channel_width = 32;
			qi->deinterleave = 2;
			break;
		case INTEL_DRAM_LPDDR4:
		case INTEL_DRAM_LPDDR5:
			qi->t_bl = 16;
			qi->max_numchannels = g_bw.ver == 35 ? 16 : 8; /* Wa_16030862157 */
			qi->channel_width = 16;
			qi->deinterleave = 4;
			break;
		case INTEL_DRAM_GDDR:
		case INTEL_DRAM_GDDR_ECC:
			qi->channel_width = 32;
			break;
		default:
			return -EINVAL;
		}
	} else if (g_bw.ver >= 12) {
		int y = is_y_tile(i915);
		switch (di->type) {
		case INTEL_DRAM_DDR4:
			qi->t_bl = y ? 8 : 4; qi->max_numchannels = 2; qi->channel_width = 64;
			qi->deinterleave = y ? 1 : 2;
			break;
		case INTEL_DRAM_DDR5:
			qi->t_bl = y ? 16 : 8; qi->max_numchannels = 4; qi->channel_width = 32;
			qi->deinterleave = y ? 1 : 2;
			break;
		case INTEL_DRAM_LPDDR4:
			if (plat_is(i915, I915_PLATFORM_ROCKETLAKE)) {
				qi->t_bl = 8; qi->max_numchannels = 4; qi->channel_width = 32;
				qi->deinterleave = 2;
				break;
			}
			/* fall through */
		case INTEL_DRAM_LPDDR5:
			qi->t_bl = 16; qi->max_numchannels = 8; qi->channel_width = 16;
			qi->deinterleave = y ? 2 : 4;
			break;
		default:
			qi->t_bl = 16; qi->max_numchannels = 1;
			break;
		}
	} else if (g_bw.ver == 11) {
		qi->t_bl = di->type == INTEL_DRAM_DDR4 ? 4 : 8;
		qi->max_numchannels = 1;
	}
	return 0;
}

static int icl_get_qgv_points(struct i915_device *i915, const struct intel_dram_info *di,
			      struct qgv_info *qi)
{
	if (icl_init_qgv_info(i915, di, qi))
		return -EINVAL;
	if (qi->num_qgv_points > NUM_QGV_POINTS)
		qi->num_qgv_points = NUM_QGV_POINTS;
	for (int i = 0; i < qi->num_qgv_points; i++) {
		struct qgv_point *sp = &qi->points[i];
		int ret = read_qgv_point_info(i915, sp, i);
		if (ret) {
			i915_dbg("[drm] i915: QGV point %d could not be read\n", i);
			return ret;
		}
		i915_dbg("[drm] i915: QGV %d: DCLK %u tRP %u tRDPRE %u tRAS %u tRCD %u tRC %u\n", i,
			 sp->dclk, sp->t_rp, sp->t_rdpre, sp->t_ras, sp->t_rcd, sp->t_rc);
	}
	if (qi->num_psf_points > 0 && adls_pcode_read_psf_gv_point_info(i915, qi->psf_clk)) {
		kprintf("[drm] i915: the PSF points could not be read; they are left out\n");
		qi->num_psf_points = 0;
	}
	return 0;
}

static uint32_t adl_calc_psf_bw(uint32_t clk)
{
	/* 64 bytes per clock of 16.666 MHz */
	return div_round_closest(64 * clk * 100, 6);
}

static uint32_t icl_sagv_max_dclk(const struct qgv_info *qi)
{
	uint32_t dclk = 0;
	for (int i = 0; i < qi->num_qgv_points; i++)
		dclk = umax(dclk, qi->points[i].dclk);
	return dclk;
}

/* The limits of the SoC (as opposed to those of the display IP). */
struct soc_bw_params {
	uint8_t deprogbwlimit; /* GB/s */
	uint8_t derating; /* % */
};

static const struct soc_bw_params *get_soc_bw_params(struct i915_device *i915)
{
	static const struct soc_bw_params icl = { 25, 10 }, tgl = { 34, 10 }, rkl = { 20, 10 },
					  adl_s = { 38, 10 }, adl_p = { 38, 20 },
					  bmg = { 53, 30 }, bmg_ecc = { 53, 45 },
					  ptl = { 65, 10 }, wcl = { 22, 10 };

	switch (i915->info->platform) {
	case I915_PLATFORM_ICELAKE:
	case I915_PLATFORM_JASPERLAKE:
	case I915_PLATFORM_ELKHARTLAKE: return &icl;
	case I915_PLATFORM_TIGERLAKE:
	case I915_PLATFORM_DG1: return &tgl;
	case I915_PLATFORM_ROCKETLAKE: return &rkl;
	case I915_PLATFORM_ALDERLAKE_S:
	case I915_PLATFORM_METEORLAKE:
	case I915_PLATFORM_LUNARLAKE: return &adl_s;
	case I915_PLATFORM_ALDERLAKE_P: return &adl_p;
	case I915_PLATFORM_BATTLEMAGE:
		return g_bw.dram.type == INTEL_DRAM_GDDR_ECC ? &bmg_ecc : &bmg;
	case I915_PLATFORM_PANTHERLAKE:
	case I915_PLATFORM_NOVALAKE_S:
	case I915_PLATFORM_NOVALAKE_P: return &ptl;
	case I915_PLATFORM_WILDCATLAKE: return &wcl;
	default: return NULL;
	}
}

/* The limits of the display IP. */
struct display_bw_params {
	uint16_t displayrtids;
	uint8_t deburst;
};

static const struct display_bw_params *get_display_bw_params(struct i915_device *i915)
{
	static const struct display_bw_params gen11 = { 128, 8 }, gen12 = { 256, 16 },
					       xelpdp = { 256, 32 };

	if (g_bw.ver >= 14)
		return &xelpdp;
	if (g_bw.ver >= 12)
		/* Rocket Lake's memory interface is Ice Lake's */
		return plat_is(i915, I915_PLATFORM_ROCKETLAKE) ? &gen11 : &gen12;
	if (g_bw.ver == 11)
		return &gen11;
	return NULL;
}

static int icl_get_bw_info(struct i915_device *i915, const struct intel_dram_info *di,
			   const struct soc_bw_params *soc, const struct display_bw_params *disp)
{
	struct qgv_info qi;
	int num_channels = di->num_channels ? di->num_channels : 1;
	int ipqdepth, ipqdepthpch = 16;
	uint32_t dclk_max, maxdebw;

	mm_memset(&qi, 0, sizeof(qi));
	if (icl_get_qgv_points(i915, di, &qi)) {
		i915_dbg("[drm] i915: no memory subsystem information; bandwidth limits ignored\n");
		return -EINVAL;
	}
	dclk_max = icl_sagv_max_dclk(&qi);
	maxdebw = umin(soc->deprogbwlimit * 1000u, dclk_max * 16 * 6 / 10);
	ipqdepth = (int)umin((uint32_t)ipqdepthpch, disp->displayrtids / (uint32_t)num_channels);
	qi.deinterleave = (uint8_t)div_round_up((uint32_t)num_channels, is_y_tile(i915) ? 4 : 2);
	g_bw.num_qgv_points = qi.num_qgv_points;
	g_bw.num_psf_gv_points = qi.num_psf_points;

	for (int i = 0; i < NUM_BW_GROUPS; i++) {
		struct bw_info *bi = &g_bw.max[i];
		int clpchgroup = (disp->deburst * qi.deinterleave / num_channels) << i;
		if (clpchgroup <= 0)
			clpchgroup = 1;
		bi->num_planes = (uint8_t)((ipqdepth - clpchgroup) / clpchgroup + 1);
		for (int j = 0; j < qi.num_qgv_points; j++) {
			const struct qgv_point *sp = &qi.points[j];
			uint32_t ct = umax(sp->t_rc, sp->t_rp + sp->t_rcd +
						      (uint32_t)(clpchgroup - 1) * qi.t_bl +
						      sp->t_rdpre);
			uint32_t bw = ct ? sp->dclk * (uint32_t)clpchgroup * 32 *
						   (uint32_t)num_channels / ct : 0;
			bi->deratedbw[j] = umin(maxdebw, bw * (100 - soc->derating) / 100);
		}
	}
	return 0;
}

static uint32_t tgl_peakbw(int num_channels, int channel_width, uint32_t dclk)
{
	return (uint32_t)num_channels * (uint32_t)(channel_width / 8) * dclk;
}

static void xe3_add_peakbw_threshold(void)
{
	uint8_t points = g_bw.num_qgv_points;

	if (g_bw.ver < 30 || points >= NUM_QGV_POINTS || points <= 1)
		return;
	g_bw.num_qgv_points++;
	g_bw.peakbw[points] = PEAK_BW_THRESHOLD;
	for (int i = 0; i < NUM_BW_GROUPS; i++)
		g_bw.max[i].deratedbw[points] = PEAK_BW_THRESHOLD;
}

static int tgl_get_bw_info(struct i915_device *i915, const struct intel_dram_info *di,
			   const struct soc_bw_params *soc, const struct display_bw_params *disp)
{
	struct qgv_info qi;
	int num_channels = di->num_channels ? di->num_channels : 1;
	int ipqdepth, ipqdepthpch = 16, clperchgroup;
	uint32_t maxdebw, peakbw;

	mm_memset(&qi, 0, sizeof(qi));
	if (icl_get_qgv_points(i915, di, &qi)) {
		i915_dbg("[drm] i915: no memory subsystem information; bandwidth limits ignored\n");
		return -EINVAL;
	}
	if (g_bw.ver < 14 && (di->type == INTEL_DRAM_LPDDR4 || di->type == INTEL_DRAM_LPDDR5))
		num_channels *= 2;
	if (num_channels < qi.max_numchannels && g_bw.ver >= 12)
		qi.deinterleave = (uint8_t)umax(qi.deinterleave / 2u, 1);
	if (qi.max_numchannels != 0 && num_channels > qi.max_numchannels)
		num_channels = qi.max_numchannels;
	peakbw = tgl_peakbw(num_channels, qi.channel_width, icl_sagv_max_dclk(&qi));
	maxdebw = umin(soc->deprogbwlimit * 1000u, peakbw * DEPROGBWPCLIMIT / 100);
	ipqdepth = (int)umin((uint32_t)ipqdepthpch, disp->displayrtids / (uint32_t)num_channels);
	/* Wa_16030862157: at least one cacheline per block per channel */
	clperchgroup = 4 * (int)umax(8u / (uint32_t)num_channels, 1) * qi.deinterleave;
	g_bw.num_qgv_points = qi.num_qgv_points;
	g_bw.num_psf_gv_points = qi.num_psf_points;
	g_bw.max[0].num_planes = 0xff;

	for (int i = 0; i < NUM_BW_GROUPS; i++) {
		struct bw_info *bi = &g_bw.max[i];
		int clpchgroup = (disp->deburst * qi.deinterleave / num_channels) << i;
		if (clpchgroup <= 0)
			clpchgroup = 1;
		if (i < NUM_BW_GROUPS - 1) {
			struct bw_info *bi_next = &g_bw.max[i + 1];
			if (clpchgroup < clperchgroup)
				bi_next->num_planes = (uint8_t)((ipqdepth - clpchgroup) / clpchgroup);
			else
				bi_next->num_planes = 0;
		}
		for (int j = 0; j < qi.num_qgv_points; j++) {
			const struct qgv_point *sp = &qi.points[j];
			uint32_t ct = umax(sp->t_rc, sp->t_rp + sp->t_rcd +
						      (uint32_t)(clpchgroup - 1) * qi.t_bl +
						      sp->t_rdpre);
			uint32_t bw = ct ? sp->dclk * (uint32_t)clpchgroup * 32 *
						   (uint32_t)num_channels / ct : 0;
			bi->deratedbw[j] = umin(maxdebw, bw * (100 - soc->derating) / 100);
		}
	}
	for (int i = 0; i < qi.num_qgv_points; i++)
		g_bw.peakbw[i] = tgl_peakbw(num_channels, qi.channel_width, qi.points[i].dclk);
	xe3_add_peakbw_threshold();
	for (int i = 0; i < qi.num_psf_points; i++)
		g_bw.psf_bw[i] = adl_calc_psf_bw(qi.psf_clk[i]);
	return 0;
}

static void dg2_get_bw_info(struct i915_device *i915)
{
	g_bw.num_qgv_points = 1;
	g_bw.max[0].num_planes = 0xff;
	g_bw.max[0].deratedbw[0] = i915->subplatform == I915_SUBPLATFORM_DG2_G11 ? 38000 : 50000;
	/* the bandwidth does not depend on the plane count */
	for (int i = 1; i < NUM_BW_GROUPS; i++)
		g_bw.max[i] = g_bw.max[0];
}

static int xe2_hpd_get_bw_info(struct i915_device *i915, const struct intel_dram_info *di,
			       const struct soc_bw_params *soc)
{
	struct qgv_info qi;
	int num_channels = di->num_channels;
	uint32_t peakbw, maxdebw;

	mm_memset(&qi, 0, sizeof(qi));
	if (icl_get_qgv_points(i915, di, &qi))
		return -EINVAL;
	peakbw = tgl_peakbw(num_channels, qi.channel_width, icl_sagv_max_dclk(&qi));
	maxdebw = umin(soc->deprogbwlimit * 1000u, peakbw * DEPROGBWPCLIMIT / 100);
	g_bw.num_qgv_points = qi.num_qgv_points;
	g_bw.max[0].num_planes = 0xff;
	for (int i = 0; i < qi.num_qgv_points; i++) {
		uint32_t bw = tgl_peakbw(num_channels, qi.channel_width, qi.points[i].dclk);
		g_bw.max[0].deratedbw[i] = umin(maxdebw, (100 - soc->derating) * bw / 100);
		g_bw.peakbw[i] = bw;
	}
	for (int i = 1; i < NUM_BW_GROUPS; i++)
		g_bw.max[i] = g_bw.max[0];
	return 0;
}

void intel_bw_init_hw(struct i915_device *i915)
{
	const struct intel_wm_platform *plat = intel_wm_platform_get(i915);
	const struct soc_bw_params *soc;
	const struct display_bw_params *disp;

	g_bw.ver = intel_wm_display_ver(i915);
	g_bw.has_sagv_hw = plat && (plat->flags & INTEL_WM_HAS_SAGV);
	g_bw.num_qgv_points = 0;
	g_bw.num_psf_gv_points = 0;
	g_bw.sagv_status = SAGV_UNKNOWN;
	g_bw.qgv_points_mask = 0;
	g_bw.new_mask = 0;
	g_bw.qgv_peakbw = 0xffff;
	mm_memset(g_bw.max, 0, sizeof(g_bw.max));
	mm_memset(g_bw.peakbw, 0, sizeof(g_bw.peakbw));
	mm_memset(g_bw.psf_bw, 0, sizeof(g_bw.psf_bw));
	if (!plat || g_bw.ver < 11)
		return;
	soc = get_soc_bw_params(i915);
	disp = get_display_bw_params(i915);
	if (g_bw.ver >= 14 && plat->ip >= 1401 && (plat->flags & INTEL_WM_DGFX)) {
		if (soc)
			xe2_hpd_get_bw_info(i915, &g_bw.dram, soc);
	} else if (plat_is(i915, I915_PLATFORM_DG2)) {
		dg2_get_bw_info(i915);
	} else if (g_bw.ver >= 12) {
		if (soc && disp)
			tgl_get_bw_info(i915, &g_bw.dram, soc, disp);
	} else if (g_bw.ver == 11) {
		if (soc && disp)
			icl_get_bw_info(i915, &g_bw.dram, soc, disp);
	}
	/* with SAGV off in the firmware there is one point, which the pcode
	 * must not be asked to restrict */
	g_bw.sagv_status = g_bw.num_qgv_points == 1 ? SAGV_NOT_CONTROLLED : SAGV_ENABLED;
}

static unsigned icl_max_bw_index(int num_planes, int qgv_point)
{
	if (qgv_point >= g_bw.num_qgv_points)
		return ~0u;
	if (num_planes < 1)
		num_planes = 1;
	for (unsigned i = 0; i < NUM_BW_GROUPS; i++)
		if (num_planes >= g_bw.max[i].num_planes)
			return i;
	return ~0u;
}

static unsigned tgl_max_bw_index(int num_planes, int qgv_point)
{
	if (qgv_point >= g_bw.num_qgv_points)
		return ~0u;
	for (int i = NUM_BW_GROUPS - 1; i >= 0; i--)
		if (num_planes <= g_bw.max[i].num_planes)
			return (unsigned)i;
	return ~0u;
}

static uint32_t icl_qgv_bw(int num_active_planes, int qgv_point)
{
	unsigned idx = g_bw.ver >= 12 ? tgl_max_bw_index(num_active_planes, qgv_point) :
					icl_max_bw_index(num_active_planes, qgv_point);
	if (idx >= NUM_BW_GROUPS)
		return 0;
	return g_bw.max[idx].deratedbw[qgv_point];
}

static uint16_t icl_qgv_points_mask(void)
{
	uint16_t qgv = 0, psf = 0;

	/* only the points the pcode advertised may be masked */
	if (g_bw.num_qgv_points > 0)
		qgv = (uint16_t)((1u << g_bw.num_qgv_points) - 1);
	if (g_bw.num_psf_gv_points > 0)
		psf = (uint16_t)((1u << g_bw.num_psf_gv_points) - 1);
	return (uint16_t)(ICL_PCODE_REQ_QGV_PT(qgv) | ADLS_PCODE_REQ_PSF_PT(psf));
}

static int is_sagv_enabled(uint16_t points_mask)
{
	return !is_power_of_2(~points_mask & icl_qgv_points_mask() & ICL_PCODE_REQ_QGV_PT_MASK);
}

static int icl_pcode_restrict_qgv_points(struct i915_device *i915, uint32_t points_mask)
{
	int ret;

	if (g_bw.ver >= 14)
		return 0;
	/* keep retrying for at least 1 ms */
	ret = intel_pcode_request(i915, ICL_PCODE_SAGV_DE_MEM_SS_CONFIG, points_mask,
				  ICL_PCODE_REP_QGV_MASK | ADLS_PCODE_REP_PSF_MASK,
				  ICL_PCODE_REP_QGV_SAFE | ADLS_PCODE_REP_PSF_SAFE, 1);
	if (ret < 0) {
		kprintf("[drm] i915: the memory points (mask 0x%x) could not be restricted (%d)\n",
			points_mask, ret);
		return ret;
	}
	g_bw.sagv_status = is_sagv_enabled((uint16_t)points_mask) ? SAGV_ENABLED : SAGV_DISABLED;
	return 0;
}

static uint32_t icl_max_bw_qgv_point_mask(int num_active_planes)
{
	uint32_t max_bw = 0, point = 0;

	for (int i = 0; i < g_bw.num_qgv_points; i++) {
		uint32_t r = icl_qgv_bw(num_active_planes, i);
		if (r > max_bw) {
			point = 1u << i;
			max_bw = r;
		}
	}
	return point;
}

static uint16_t icl_prepare_qgv_points_mask(uint32_t qgv_points, uint32_t psf_points)
{
	return (uint16_t)(~(ICL_PCODE_REQ_QGV_PT(qgv_points) | ADLS_PCODE_REQ_PSF_PT(psf_points)) &
			  icl_qgv_points_mask());
}

static uint32_t icl_max_bw_psf_gv_point_mask(void)
{
	uint32_t max_bw = 0, mask = 0;

	for (int i = 0; i < g_bw.num_psf_gv_points; i++) {
		uint32_t r = g_bw.psf_bw[i];
		if (r > max_bw) {
			mask = 1u << i;
			max_bw = r;
		} else if (r == max_bw) {
			mask |= 1u << i;
		}
	}
	return mask;
}

/* ---- SAGV ------------------------------------------------------------------------------ */

int intel_has_sagv(struct i915_device *i915)
{
	(void)i915;
	return g_bw.has_sagv_hw && g_bw.sagv_status != SAGV_NOT_CONTROLLED;
}

uint32_t intel_sagv_block_time_us(struct i915_device *i915)
{
	(void)i915;
	return g_bw.sagv_block_time_us;
}

uint16_t intel_bw_qgv_peakbw(struct i915_device *i915)
{
	(void)i915;
	return g_bw.qgv_peakbw;
}

static void skl_sagv_enable(struct i915_device *i915)
{
	int ret;

	if (!intel_has_sagv(i915) || g_bw.sagv_status == SAGV_ENABLED)
		return;
	ret = intel_pcode_rw(i915, GEN9_PCODE_SAGV_CONTROL, &(uint32_t){ GEN9_SAGV_ENABLE }, NULL,
			     250, 1, 0);
	/* some (pre-release) Skylake machines have no SAGV */
	if (plat_is(i915, I915_PLATFORM_SKYLAKE) && ret == -ENXIO) {
		g_bw.sagv_status = SAGV_NOT_CONTROLLED;
		return;
	}
	if (ret < 0) {
		kprintf("[drm] i915: SAGV could not be enabled (%d)\n", ret);
		return;
	}
	g_bw.sagv_status = SAGV_ENABLED;
}

static void skl_sagv_disable(struct i915_device *i915)
{
	int ret;

	if (!intel_has_sagv(i915) || g_bw.sagv_status == SAGV_DISABLED)
		return;
	/* keep retrying for at least 1 ms */
	ret = intel_pcode_request(i915, GEN9_PCODE_SAGV_CONTROL, GEN9_SAGV_DISABLE,
				  GEN9_SAGV_IS_DISABLED, GEN9_SAGV_IS_DISABLED, 1);
	if (plat_is(i915, I915_PLATFORM_SKYLAKE) && ret == -ENXIO) {
		g_bw.sagv_status = SAGV_NOT_CONTROLLED;
		return;
	}
	if (ret < 0) {
		kprintf("[drm] i915: SAGV could not be disabled (%d)\n", ret);
		return;
	}
	g_bw.sagv_status = SAGV_DISABLED;
}

static uint32_t sagv_block_time(struct i915_device *i915)
{
	if (g_bw.ver >= 14)
		return WM_LATENCY_QCLK_SAGV(i915_read32(i915, WM_LATENCY_SAGV));
	if (g_bw.ver >= 12) {
		uint32_t val = 0;
		if (intel_pcode_rw(i915, GEN12_PCODE_READ_SAGV_BLOCK_TIME_US, &val, NULL, 500, 20,
				   1)) {
			i915_dbg("[drm] i915: the SAGV block time could not be read\n");
			return 0;
		}
		return val;
	}
	if (g_bw.ver == 11)
		return 10;
	if (g_bw.has_sagv_hw)
		return 30;
	return 0;
}

/* Display version 11-13: SAGV off (the single fastest point) until a
 * configuration is known to allow it; before 11 SAGV is probed by
 * turning it off.  Then the block time the watermarks are checked
 * against. */
void intel_bw_init(struct i915_device *i915)
{
	if (g_bw.ver < 9)
		return;
	if (!g_bw.has_sagv_hw)
		g_bw.sagv_status = SAGV_NOT_CONTROLLED;
	if (intel_has_sagv(i915) && g_bw.ver >= 11 && g_bw.ver <= 13) {
		uint32_t qgv = icl_max_bw_qgv_point_mask(0);
		uint32_t psf = icl_max_bw_psf_gv_point_mask();
		g_bw.qgv_points_mask = icl_prepare_qgv_points_mask(qgv, psf);
		icl_pcode_restrict_qgv_points(i915, g_bw.qgv_points_mask);
	}
	if (g_bw.ver < 11)
		skl_sagv_disable(i915);
	g_bw.sagv_block_time_us = sagv_block_time(i915);
	if (g_bw.sagv_block_time_us > 0xffff)
		g_bw.sagv_block_time_us = 0;
	if (!intel_has_sagv(i915))
		g_bw.sagv_block_time_us = 0;
	i915_dbg("[drm] i915: SAGV %s, block time %u us, %u QGV points\n",
		 intel_has_sagv(i915) ? "controlled" : "not controlled", g_bw.sagv_block_time_us,
		 g_bw.num_qgv_points);
}

/* ---- checking a configuration ----------------------------------------------------------- */

static int mtl_find_qgv_points(uint32_t data_rate, int num_active_planes, int sagv_ok,
			       uint16_t *peakbw)
{
	uint32_t best_rate = ~0u, qgv_peak_bw = 0;

	/* without SAGV the PM demand asks for all ones: stay where you are */
	if (!sagv_ok) {
		*peakbw = 0xffff;
		return 0;
	}
	for (int i = 0; i < g_bw.num_qgv_points; i++) {
		uint32_t max_data_rate = icl_qgv_bw(num_active_planes, i);
		if (max_data_rate < data_rate)
			continue;
		if (max_data_rate < best_rate) {
			best_rate = max_data_rate;
			qgv_peak_bw = g_bw.peakbw[i];
		}
	}
	if (qgv_peak_bw == 0) {
		i915_dbg("[drm] i915: no memory point carries %u MB/s for %d planes\n", data_rate,
			 num_active_planes);
		return -EINVAL;
	}
	/* in units of 100 MB/s */
	*peakbw = (uint16_t)umin(qgv_peak_bw / 100, 0xffff);
	return 0;
}

static int icl_find_qgv_points(uint32_t data_rate, int num_active_planes, int sagv_ok,
			       uint16_t *mask)
{
	uint32_t qgv_points = 0, psf_points = 0;

	for (int i = 0; i < g_bw.num_qgv_points; i++)
		if (icl_qgv_bw(num_active_planes, i) >= data_rate)
			qgv_points |= 1u << i;
	for (int i = 0; i < g_bw.num_psf_gv_points; i++)
		if (g_bw.psf_bw[i] >= data_rate)
			psf_points |= 1u << i;
	/* at least one point must be left */
	if (qgv_points == 0) {
		i915_dbg("[drm] i915: no memory point carries %u MB/s for %d planes\n", data_rate,
			 num_active_planes);
		return -EINVAL;
	}
	if (g_bw.num_psf_gv_points > 0 && psf_points == 0) {
		i915_dbg("[drm] i915: no PSF point carries %u MB/s\n", data_rate);
		return -EINVAL;
	}
	/* without SAGV only the single fastest point */
	if (!sagv_ok)
		qgv_points = icl_max_bw_qgv_point_mask(num_active_planes);
	*mask = icl_prepare_qgv_points_mask(qgv_points, psf_points);
	return 0;
}

int intel_bw_check(struct i915_device *i915, const struct intel_wm_result *res, uint16_t *qgv)
{
	uint32_t data_rate = 0;
	int num_active_planes = 0;
	int sagv_ok = res->sagv_reject == 0;

	(void)i915;
	*qgv = g_bw.ver >= 14 ? 0xffff : 0;
	if (g_bw.ver < 11)
		return 0;
	/* the bandwidth could not be worked out: nothing to check against */
	if (g_bw.num_qgv_points == 0)
		return 0;
	for (int pipe = 0; pipe < INTEL_MAX_PIPES; pipe++) {
		data_rate += res->data_rate[pipe];
		num_active_planes += res->num_active_planes[pipe];
	}
	/* kB/s to MB/s; IOMMU translation (5% more from display version 13)
	 * is not in use here */
	data_rate = div_round_up(data_rate, 1000);
	if (g_bw.ver >= 14)
		return mtl_find_qgv_points(data_rate, num_active_planes, sagv_ok, qgv);
	return icl_find_qgv_points(data_rate, num_active_planes, sagv_ok, qgv);
}

/* Display version 14: the PM demand request carries the point, raised
 * to the larger of the old and new needs before a change and set to the
 * new one after it. */
static void mtl_qgv_update(struct i915_device *i915, const struct intel_wm_result *res, int pre)
{
	uint16_t q;

	if (intel_bw_check(i915, res, &q))
		q = 0xffff; /* nothing carries it: hold the memory where it is */
	if (pre) {
		g_bw.new_mask = q;
		if (q <= g_bw.qgv_peakbw)
			return;
		g_bw.qgv_peakbw = q;
	} else {
		if (g_bw.new_mask == g_bw.qgv_peakbw)
			return;
		g_bw.qgv_peakbw = g_bw.new_mask;
	}
	intel_pmdemand_set_qgv_peakbw(i915, g_bw.qgv_peakbw);
	intel_pmdemand_update(i915);
}

void intel_bw_pre_update(struct i915_device *i915, const struct intel_wm_result *res)
{
	uint16_t q;

	if (g_bw.ver >= 14) {
		mtl_qgv_update(i915, res, 1);
		return;
	}
	if (g_bw.ver < 9 || !intel_has_sagv(i915))
		return;
	if (g_bw.ver >= 11) {
		uint16_t old_mask = g_bw.qgv_points_mask, new_mask;
		if (intel_bw_check(i915, res, &q)) {
			/* too much for any point: hold the fastest */
			q = icl_prepare_qgv_points_mask(icl_max_bw_qgv_point_mask(0),
							icl_max_bw_psf_gv_point_mask());
		}
		g_bw.new_mask = q;
		new_mask = old_mask | q;
		/* points may only be masked before a change and unmasked
		 * after it, never both at once */
		if (new_mask != old_mask) {
			i915_dbg("[drm] i915: restricting memory points 0x%x -> 0x%x\n", old_mask,
				 new_mask);
			if (icl_pcode_restrict_qgv_points(i915, new_mask) == 0)
				g_bw.qgv_points_mask = new_mask;
		}
		return;
	}
	if (res->sagv_reject)
		skl_sagv_disable(i915);
}

void intel_bw_post_update(struct i915_device *i915, const struct intel_wm_result *res)
{
	if (g_bw.ver >= 14) {
		mtl_qgv_update(i915, res, 0);
		return;
	}
	if (g_bw.ver < 9 || !intel_has_sagv(i915))
		return;
	if (g_bw.ver >= 11) {
		if (g_bw.new_mask != g_bw.qgv_points_mask) {
			i915_dbg("[drm] i915: relaxing memory points 0x%x -> 0x%x\n",
				 g_bw.qgv_points_mask, g_bw.new_mask);
			if (icl_pcode_restrict_qgv_points(i915, g_bw.new_mask) == 0)
				g_bw.qgv_points_mask = g_bw.new_mask;
		}
		return;
	}
	if (!res->sagv_reject)
		skl_sagv_enable(i915);
}
