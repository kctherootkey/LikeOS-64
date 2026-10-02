// LikeOS -- registers with one copy per slice, subslice or bank.
//
// Part of the GT's register file is replicated: each slice, subslice,
// L3 bank or memory slice has its own copy of the register at the same
// offset.  A write reaches every copy at once (multicast).  A read is
// answered by the one copy a selector register names -- and a copy whose
// unit is fused off, or powered down, answers with zero.  So a read is
// steered at a unit that exists: by default at the first subslice the
// fuses leave (the Gen12 workaround list programs the selector so), and
// for the ranges whose units are counted differently (L3 banks, memory
// slices, Meteor Lake's dual subslices) at the first of those.
//
// Meteor Lake moved the selector, and shares it with the GuC firmware:
// a hardware semaphore is taken around every steered access, and the
// selector is left in multicast mode, as the firmware expects to find
// it.  Xe2 and Xe3 keep that and count their units differently again: L3
// banks within nodes, nodes as group and instance, the SQIDI and PSMI
// units by node, and dual subslices in groups of a size the GuC's
// hardware configuration table states.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2022-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

struct mcr_range {
	uint32_t start, end; /* inclusive; an all-zero entry ends a table */
};

/* In the order the ranges are looked up in. */
enum steering_type {
	STEER_L3BANK,
	STEER_NODE,
	STEER_MSLICE,
	STEER_LNCF,
	STEER_DSS,
	STEER_OADDRM,
	STEER_SQIDI_PSMI,
	STEER_GAM,
	STEER_INSTANCE0,
	STEER_TYPES
};

static const struct mcr_range icl_l3bank_steering_table[] = {
	{ 0x00b100, 0x00b3ff },
	{ 0, 0 },
};

/* DG2: the memory slices and the LNCF units behind them */
static const struct mcr_range dg2_mslice_steering_table[] = {
	{ 0x00dd00, 0x00ddff },
	{ 0x00e900, 0x00ffff }, /* 0xea00-0xefff is unused */
	{ 0, 0 },
};

static const struct mcr_range dg2_lncf_steering_table[] = {
	{ 0x00b000, 0x00b0ff },
	{ 0x00d880, 0x00d8ff },
	{ 0, 0 },
};

/* Meteor Lake: units whose instance 0 always exists */
static const struct mcr_range xelpg_instance0_steering_table[] = {
	{ 0x000b00, 0x000bff }, /* SQIDI */
	{ 0x001000, 0x001fff }, /* SQIDI */
	{ 0x004000, 0x0048ff }, /* GAM */
	{ 0x008700, 0x0087ff }, /* SQIDI */
	{ 0x00b000, 0x00b0ff }, /* NODE */
	{ 0x00c800, 0x00cfff }, /* GAM */
	{ 0x00d880, 0x00d8ff }, /* NODE */
	{ 0x00dd00, 0x00ddff }, /* OAAL2 */
	{ 0, 0 },
};

static const struct mcr_range xelpg_l3bank_steering_table[] = {
	{ 0x00b100, 0x00b3ff },
	{ 0, 0 },
};

/* the slice ranges are steered as dual subslices too */
static const struct mcr_range xelpg_dss_steering_table[] = {
	{ 0x005200, 0x0052ff }, /* SLICE */
	{ 0x005500, 0x007fff }, /* SLICE */
	{ 0x008140, 0x00815f }, /* SLICE (0x8140-0x814f), DSS (0x8150-0x815f) */
	{ 0x0094d0, 0x00955f }, /* SLICE (0x94d0-0x951f), DSS (0x9520-0x955f) */
	{ 0x009680, 0x0096ff }, /* DSS */
	{ 0x00d800, 0x00d87f }, /* SLICE */
	{ 0x00dc00, 0x00dcff }, /* SLICE */
	{ 0x00de80, 0x00e8ff }, /* DSS (0xe000-0xe0ff reserved) */
	{ 0, 0 },
};

/* Meteor Lake's media GT (absolute offsets) */
static const struct mcr_range xelpmp_oaddrm_steering_table[] = {
	{ 0x393200, 0x39323f },
	{ 0x393400, 0x3934ff },
	{ 0, 0 },
};

/* Xe2 and Xe3 (absolute offsets for the media GT's) */
static const struct mcr_range xe2lpg_dss_steering_table[] = {
	{ 0x005200, 0x0052ff }, /* SLICE */
	{ 0x005500, 0x007fff }, /* SLICE */
	{ 0x008140, 0x00815f }, /* SLICE (0x8140-0x814f), DSS (0x8150-0x815f) */
	{ 0x0094d0, 0x00955f }, /* SLICE (0x94d0-0x951f), DSS (0x9520-0x955f) */
	{ 0x009680, 0x0096ff }, /* DSS */
	{ 0x00d800, 0x00d87f }, /* SLICE */
	{ 0x00dc00, 0x00dcff }, /* SLICE */
	{ 0x00de00, 0x00e8ff }, /* DSS (0xe000-0xe0ff reserved) */
	{ 0x00e980, 0x00e9ff }, /* SLICE */
	{ 0x013000, 0x0133ff }, /* DSS (0x13000-0x131ff), SLICE (0x13200-0x133ff) */
	{ 0, 0 },
};

static const struct mcr_range xe2lpg_sqidi_psmi_steering_table[] = {
	{ 0x000b00, 0x000bff },
	{ 0x001000, 0x001fff },
	{ 0, 0 },
};

static const struct mcr_range xe2lpg_instance0_steering_table[] = {
	{ 0x004000, 0x004aff }, /* GAM, rsvd, GAMWKR */
	{ 0x008700, 0x00887f }, /* SQIDI, MEMPIPE */
	{ 0x00c800, 0x00cfff }, /* GAM */
	{ 0x00dd00, 0x00ddff }, /* MEMPIPE */
	{ 0x00e900, 0x00e97f }, /* MEMPIPE */
	{ 0x00f000, 0x00ffff }, /* GAM, GAMWKR */
	{ 0x013400, 0x0135ff }, /* MEMPIPE */
	{ 0, 0 },
};

static const struct mcr_range xe2_node_steering_table[] = {
	{ 0x00b000, 0x00b0ff },
	{ 0x00d880, 0x00d8ff },
	{ 0, 0 },
};

static const struct mcr_range xe3p_lpg_instance0_steering_table[] = {
	{ 0x004000, 0x004aff }, /* GAM, rsvd, GAMWKR */
	{ 0x008700, 0x00887f }, /* NODE */
	{ 0x00b000, 0x00b3ff }, /* NODE, L3BANK */
	{ 0x00b500, 0x00b6ff }, /* PSMI */
	{ 0x00c800, 0x00cfff }, /* GAM */
	{ 0x00d880, 0x00d8ff }, /* NODE */
	{ 0x00dd00, 0x00dd7f }, /* MEMPIPE */
	{ 0x00f000, 0x00ffff }, /* GAM, GAMWKR */
	{ 0x013400, 0x0135ff }, /* MEMPIPE */
	{ 0, 0 },
};

static const struct mcr_range xe2lpm_gpmxmt_steering_table[] = {
	{ 0x388160, 0x38817f },
	{ 0x389480, 0x3894cf },
	{ 0, 0 },
};

static const struct mcr_range xe2lpm_instance0_steering_table[] = {
	{ 0x384000, 0x3847df }, /* GAM, rsvd, GAM */
	{ 0x384900, 0x384aff }, /* GAM */
	{ 0x389560, 0x3895ff }, /* MEDIAINF */
	{ 0x38b600, 0x38b8ff }, /* L3BANK */
	{ 0x38c800, 0x38d07f }, /* GAM, MEDIAINF */
	{ 0x38f000, 0x38f0ff }, /* GAM */
	{ 0x393c00, 0x393c7f }, /* MEDIAINF */
	{ 0, 0 },
};

static const struct mcr_range xe3lpm_instance0_steering_table[] = {
	{ 0x384000, 0x3841ff }, /* GAM */
	{ 0x384400, 0x3847df }, /* GAM */
	{ 0x384900, 0x384aff }, /* GAM */
	{ 0x389560, 0x3895ff }, /* MEDIAINF */
	{ 0x38b600, 0x38b8ff }, /* L3BANK */
	{ 0x38c800, 0x38d07f }, /* GAM, MEDIAINF */
	{ 0x38d0d0, 0x38f0ff }, /* MEDIAINF, rsvd, GAM */
	{ 0x393c00, 0x393c7f }, /* MEDIAINF */
	{ 0, 0 },
};

#define GEN_DSS_PER_GSLICE 4
#define GEN_DSS_PER_MSLICE 8

static const struct mcr_range *steering_table[STEER_TYPES];
static const struct mcr_range *media_steering_table[STEER_TYPES];
static spinlock_t mcr_lock;

static unsigned first_bit(uint32_t v)
{
	for (unsigned i = 0; i < 32; i++)
		if (v & (1u << i))
			return i;
	return 0;
}

void i915_mcr_init(struct i915_device *i915)
{
	unsigned ip = i915->gt_ip;

	spinlock_init(&mcr_lock, "i915_mcr");
	for (int t = 0; t < STEER_TYPES; t++)
		steering_table[t] = media_steering_table[t] = NULL;
	i915->mcr_group = i915->mcr_instance = 0;
	if (i915->info->gen < 11)
		return;

	/* A memory slice is missing only when its L3 and all the dual
	 * subslices of its quadrant are. */
	if (i915->info->platform == I915_PLATFORM_DG2) {
		uint32_t dss = i915->dss_geometry | i915->dss_compute;
		i915->mslice_mask = 0;
		for (unsigned q = 0; q < 4; q++)
			if ((dss >> (q * GEN_DSS_PER_MSLICE)) & 0xff)
				i915->mslice_mask |= 1u << q;
		i915->mslice_mask |= i915_read32(i915, GEN10_MIRROR_FUSE3) & GEN12_MEML3_EN_MASK;
	}
	if (i915->has_media_gt) {
		if (i915->media_ip >= I915_IP(30, 0)) {
			media_steering_table[STEER_OADDRM] = xe2lpm_gpmxmt_steering_table;
			media_steering_table[STEER_INSTANCE0] = xe3lpm_instance0_steering_table;
		} else if (i915->media_ip >= I915_IP(13, 1)) {
			media_steering_table[STEER_OADDRM] = xe2lpm_gpmxmt_steering_table;
			media_steering_table[STEER_INSTANCE0] = xe2lpm_instance0_steering_table;
		} else {
			media_steering_table[STEER_OADDRM] = xelpmp_oaddrm_steering_table;
		}
	}
	/* the dual subslices of a steering group: four on every part since
	 * Xe_HP until the GuC's table says otherwise (i915_mcr_set_dss_per_group) */
	if (!i915->dss_per_group)
		i915->dss_per_group = GEN_DSS_PER_GSLICE;

	if (ip >= I915_IP(35, 10)) {
		/* the L3 bank and node targets are i915_gtt.c's, from the
		 * fuses; Xe3P has no ranges of those kinds of its own */
		steering_table[STEER_DSS] = xe2lpg_dss_steering_table;
		steering_table[STEER_INSTANCE0] = xe3p_lpg_instance0_steering_table;
	} else if (ip >= I915_IP(20, 0)) {
		steering_table[STEER_DSS] = xe2lpg_dss_steering_table;
		steering_table[STEER_SQIDI_PSMI] = xe2lpg_sqidi_psmi_steering_table;
		steering_table[STEER_INSTANCE0] = xe2lpg_instance0_steering_table;
		steering_table[STEER_L3BANK] = xelpg_l3bank_steering_table;
		steering_table[STEER_NODE] = xe2_node_steering_table;
	} else if (ip >= I915_IP(12, 70)) {
		uint32_t fuse;
		/* Wa_14016747170: the early steppings report the missing
		 * L3 banks elsewhere */
		if ((ip == I915_IP(12, 70) || ip == I915_IP(12, 71)) &&
		    i915->gt_step < I915_STEP_B0)
			fuse = MTL_GT_L3_EXC_MASK(i915_read32(i915, MTL_GT_ACTIVITY_FACTOR));
		else
			fuse = GT_L3_EXC_MASK(i915_read32(i915, XEHP_FUSE4));
		/* each bit stands for a pair of banks */
		i915->l3bank_mask = 0;
		for (unsigned i = 0; i < 3; i++)
			if (fuse & (1u << i))
				i915->l3bank_mask |= 0x3u << (2 * i);
		steering_table[STEER_INSTANCE0] = xelpg_instance0_steering_table;
		steering_table[STEER_L3BANK] = xelpg_l3bank_steering_table;
		steering_table[STEER_DSS] = xelpg_dss_steering_table;
	} else if (i915->info->platform == I915_PLATFORM_DG2) {
		/* The default steering (a geometry slice and a dual
		 * subslice in it) is chosen so that, where possible, the
		 * same slice id also names an LNCF and a memory slice that
		 * exist; those ranges then need no steering of their own. */
		uint32_t dss = i915->dss_geometry | i915->dss_compute;
		uint32_t slices = 0, lncf = 0;
		for (unsigned g = 0; g < 8; g++)
			if ((dss >> (g * GEN_DSS_PER_GSLICE)) & 0xf)
				slices |= 1u << g;
		for (unsigned m = 0; m < 4; m++)
			if (i915->mslice_mask & (1u << m))
				lncf |= 0x3u << (m * 2);
		steering_table[STEER_MSLICE] = dg2_mslice_steering_table;
		steering_table[STEER_LNCF] = dg2_lncf_steering_table;
		if (slices & lncf) {
			slices &= lncf;
			steering_table[STEER_LNCF] = NULL;
		}
		if (slices & i915->mslice_mask) {
			slices &= i915->mslice_mask;
			steering_table[STEER_MSLICE] = NULL;
		}
		i915->mcr_group = (uint8_t)first_bit(slices);
		i915->mcr_instance =
			(uint8_t)(first_bit((dss >> (i915->mcr_group * GEN_DSS_PER_GSLICE)) & 0xf));
	} else if (ip < I915_IP(12, 55)) {
		/* Gen11 and Gen12.0: reads of everything else are steered at
		 * the first subslice of slice 0 (the selector default the
		 * workaround list sets); the L3 bank ranges need their own
		 * steering only when that subslice's bank is fused off. */
		uint32_t ss = first_bit(i915->subslice_mask[0]);
		i915->l3bank_mask = ~i915_read32(i915, GEN10_MIRROR_FUSE3) & GEN10_L3BANK_MASK;
		i915->mcr_group = 0;
		i915->mcr_instance = (uint8_t)ss;
		if (!(i915->l3bank_mask & (1u << ss)))
			steering_table[STEER_L3BANK] = icl_l3bank_steering_table;
	}
	i915_dbg("[drm] i915: register steering: L3 banks %x, memory slices %x, default %u/%u\n",
		 i915->l3bank_mask, i915->mslice_mask, i915->mcr_group, i915->mcr_instance);
}

static int in_table(const struct mcr_range *t, uint32_t reg)
{
	if (!t)
		return 0;
	for (; t->end; t++)
		if (reg >= t->start && reg <= t->end)
			return 1;
	return 0;
}

/* A unit of the kind that exists. */
static void nonterminated(struct i915_device *i915, int type, uint8_t *group, uint8_t *instance)
{
	uint32_t dss;

	switch (type) {
	case STEER_L3BANK:
		if (i915->gt_ip >= I915_IP(20, 0)) {
			*group = i915->l3bank_group;
			*instance = i915->l3bank_instance;
			break;
		}
		*group = 0;
		*instance = (uint8_t)first_bit(i915->l3bank_mask);
		break;
	case STEER_NODE:
		*group = i915->node_group;
		*instance = i915->node_instance;
		break;
	case STEER_SQIDI_PSMI:
		*group = i915->sqidi_group;
		*instance = i915->sqidi_instance;
		break;
	case STEER_MSLICE:
		*group = (uint8_t)first_bit(i915->mslice_mask);
		*instance = 0;
		break;
	case STEER_LNCF:
		/* an LNCF is present wherever its memory slice is */
		*group = (uint8_t)(first_bit(i915->mslice_mask) << 1);
		*instance = 0;
		break;
	case STEER_GAM:
		*group = i915->info->platform == I915_PLATFORM_DG2 ? 1 : 0;
		*instance = 0;
		break;
	case STEER_DSS:
		if (i915->gt_ip >= I915_IP(20, 0)) {
			/* the first of either pipeline's, in groups of the
			 * size the hardware configuration names */
			uint32_t gd = i915->dss_geometry ? first_bit(i915->dss_geometry) : 32;
			uint32_t cd = i915->dss_compute ? first_bit(i915->dss_compute) : 32;
			dss = gd < cd ? gd : cd;
			if (dss == 32)
				dss = 0;
			*group = (uint8_t)(dss / i915->dss_per_group);
			*instance = (uint8_t)(dss % i915->dss_per_group);
			break;
		}
		dss = first_bit(i915->dss_geometry | i915->dss_compute);
		*group = (uint8_t)(dss / GEN_DSS_PER_GSLICE);
		*instance = (uint8_t)(dss % GEN_DSS_PER_GSLICE);
		break;
	case STEER_OADDRM: {
		/* the media slice 0 unless all of it is fused off (from Xe3P
		 * media on a slice has two engines of each kind) */
		uint32_t first = I915_FW_VDBOX0 | I915_FW_VEBOX0;
		if (i915->media_ip >= I915_IP(35, 0))
			first |= I915_FW_VDBOX1 | I915_FW_VEBOX1;
		/* Xe2 on: the engines the fuses leave are the ones with a
		 * forcewake domain (i915_uncore.c reads the fuses for that) */
		if (i915->media_ip >= I915_IP(13, 1))
			*group = (i915->fw_present & first) ? 0 : 1;
		else
			*group = (i915->info->engine_mask & first) || (i915->sfc_mask & 1) ? 0 : 1;
		*instance = 0;
		break;
	}
	default:
		*group = 0;
		*instance = 0;
		break;
	}
}

/* Meteor Lake: the selector belongs to whoever holds the semaphore. */
static void steer_lock(struct i915_device *i915, uint32_t gsi)
{
	if (i915->gt_ip < I915_IP(12, 70))
		return;
	static int said;
	i915_fw_get(i915, gsi ? I915_FW_MEDIA_GT : I915_FW_GT);
	/* 100 ms, with the processor's interrupts off (the caller holds
	 * mcr_lock); once the semaphore has failed to come, 1 ms -- a
	 * semaphore that never answers must not take a tenth of a second
	 * out of every steered access */
	int limit = said ? 100 : 10000;
	for (int t = 0; t < limit; t++) {
		if (i915_read32_fw(i915, MTL_STEER_SEMAPHORE + gsi) == 0x1)
			return;
		lapic_delay_us(10);
	}
	if (!said++)
		kprintf("[drm] i915: register steering semaphore timed out\n");
}

static void steer_unlock(struct i915_device *i915, uint32_t gsi)
{
	if (i915->gt_ip < I915_IP(12, 70))
		return;
	i915_write32_fw(i915, MTL_STEER_SEMAPHORE + gsi, 0x1);
	i915_fw_put(i915, gsi ? I915_FW_MEDIA_GT : I915_FW_GT);
}

static uint32_t steered_read(struct i915_device *i915, uint32_t reg, uint32_t gsi,
			     uint8_t group, uint8_t instance)
{
	uint32_t val;
	uint64_t fl;

	/* the processors among themselves first, then against the
	 * firmware: a processor that took the hardware semaphore and then
	 * waited for the lock would leave the other spinning on the
	 * semaphore, and give up on it, while the first holds it */
	spin_lock_irqsave(&mcr_lock, &fl);
	steer_lock(i915, gsi);
	if (i915->gt_ip >= I915_IP(12, 70)) {
		/* Reads stay in multicast mode (Wa_22013088509); the read
		 * is answered by the instance named all the same. */
		i915_write32(i915, MTL_MCR_SELECTOR + gsi,
			     MTL_MCR_GROUPID(group) | MTL_MCR_INSTANCEID(instance) |
				     GEN11_MCR_MULTICAST);
		val = i915_read32(i915, reg);
	} else {
		uint32_t old = i915_read32(i915, GEN8_MCR_SELECTOR);
		uint32_t mcr = (old & ~(GEN11_MCR_SLICE_MASK | GEN11_MCR_SUBSLICE_MASK)) |
			       GEN11_MCR_SLICE(group) | GEN11_MCR_SUBSLICE(instance);
		i915_write32(i915, GEN8_MCR_SELECTOR, mcr);
		val = i915_read32(i915, reg);
		i915_write32(i915, GEN8_MCR_SELECTOR, old);
	}
	steer_unlock(i915, gsi);
	spin_unlock_irqrestore(&mcr_lock, fl);
	return val;
}

int i915_mcr_steering(struct i915_device *i915, uint32_t reg, uint8_t *group,
		      uint8_t *instance)
{
	const struct mcr_range **tables = steering_table;

	if (i915->has_media_gt && reg >= I915_MEDIA_GT_BASE && reg < I915_MEDIA_GT_BASE + 0x40000)
		tables = media_steering_table;
	for (int t = 0; t < STEER_TYPES; t++) {
		if (!in_table(tables[t], reg))
			continue;
		nonterminated(i915, t, group, instance);
		return 1;
	}
	/* a register in no table: steered at 0/0, as a guess */
	*group = 0;
	*instance = 0;
	return 0;
}

void i915_mcr_set_dss_per_group(struct i915_device *i915, unsigned n)
{
	if (n && n <= 16)
		i915->dss_per_group = (uint8_t)n;
}

uint32_t i915_mcr_read(struct i915_device *i915, uint32_t reg)
{
	uint32_t gsi = 0;
	const struct mcr_range **tables = steering_table;
	uint8_t group, instance;

	if (i915->info->gen < 11)
		return i915_read32(i915, reg);
	if (i915->has_media_gt && reg >= I915_MEDIA_GT_BASE && reg < I915_MEDIA_GT_BASE + 0x40000) {
		gsi = I915_MEDIA_GT_BASE;
		tables = media_steering_table;
	}
	for (int t = 0; t < STEER_TYPES; t++) {
		if (!in_table(tables[t], reg))
			continue;
		nonterminated(i915, t, &group, &instance);
		return steered_read(i915, reg, gsi, group, instance);
	}
	return i915_read32(i915, reg);
}

void i915_mcr_write(struct i915_device *i915, uint32_t reg, uint32_t val)
{
	uint32_t gsi = 0;
	uint64_t fl;

	if (i915->gt_ip < I915_IP(12, 70)) {
		/* the selector is left in multicast mode: a plain write */
		i915_write32(i915, reg, val);
		return;
	}
	if (i915->has_media_gt && reg >= I915_MEDIA_GT_BASE && reg < I915_MEDIA_GT_BASE + 0x40000)
		gsi = I915_MEDIA_GT_BASE;
	spin_lock_irqsave(&mcr_lock, &fl);
	steer_lock(i915, gsi);
	i915_write32(i915, MTL_MCR_SELECTOR + gsi, GEN11_MCR_MULTICAST);
	i915_write32(i915, reg, val);
	steer_unlock(i915, gsi);
	spin_unlock_irqrestore(&mcr_lock, fl);
}

static void set_steering(struct i915_device *i915, uint32_t reg, unsigned slice, unsigned ss)
{
	uint32_t v = i915_read32(i915, reg);

	v &= ~(GEN11_MCR_SLICE_MASK | GEN11_MCR_SUBSLICE_MASK);
	v |= GEN11_MCR_SLICE(slice) | GEN11_MCR_SUBSLICE(ss);
	i915_write32(i915, reg, v);
}

/* Xe_HP (DG2): the default steering, after every GT reset.  The SQIDI
 * units have selectors of their own; DG2-G11 has only instances 2 and 3
 * of them, so 2 is used everywhere.  The GAM units must be steered at
 * group 1. */
void i915_mcr_program_defaults(struct i915_device *i915)
{
	if (i915->info->platform != I915_PLATFORM_DG2)
		return;
	set_steering(i915, GEN8_MCR_SELECTOR, i915->mcr_group, i915->mcr_instance);
	set_steering(i915, MCFG_MCR_SELECTOR, 0, 2);
	set_steering(i915, SF_MCR_SELECTOR, 0, 2);
	set_steering(i915, GAM_MCR_SELECTOR, 1, 0);
}
