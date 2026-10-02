// LikeOS -- which variant and which silicon revision a part is.
//
// The workarounds a part needs depend on more than its generation: on
// the variant of the platform (Tiger Lake's UP3/UP4 parts, Raptor Lake's
// parts that share Alder Lake's graphics, DG2's three dies), on the IP
// version of the graphics and media blocks, and on the stepping -- the
// revision of the silicon, A0 for the first one taped out.  Before
// Meteor Lake the stepping is read from the PCI revision id through a
// table per platform, the same revision id meaning different steppings
// on different variants.  From Meteor Lake on the graphics and the media
// block each report their IP version and stepping in a GMD_ID register
// of their own, in an always-on part of the chip.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/io/console.h>

/* ---- the variants, by device id ------------------------------------------ */

static const uint16_t tgl_uy_ids[] = { 0x9a40, 0x9a49, 0x9a59, 0x9a78,
				       0x9ac0, 0x9ac9, 0x9ad9, 0x9af8 };
static const uint16_t adl_n_ids[] = { 0x46d0, 0x46d1, 0x46d2, 0x46d3, 0x46d4 };
static const uint16_t rpl_s_ids[] = { 0xa780, 0xa781, 0xa782, 0xa783,
				      0xa788, 0xa789, 0xa78a, 0xa78b };
static const uint16_t rpl_u_ids[] = { 0xa721, 0xa7a1, 0xa7a9, 0xa7ac, 0xa7ad };
static const uint16_t rpl_p_ids[] = { 0xa720, 0xa7a0, 0xa7a8, 0xa7aa, 0xa7ab };
static const uint16_t dg2_g10_ids[] = { 0x56a0, 0x56a1, 0x56a2, 0x56be, 0x56bf,
					0x5690, 0x5691, 0x5692, 0x56c0, 0x56c2 };
static const uint16_t dg2_g11_ids[] = { 0x56a5, 0x56a6, 0x56b0, 0x56b1, 0x56ba,
					0x56bb, 0x56bc, 0x56bd, 0x5693, 0x5694,
					0x5695, 0x56c1 };
static const uint16_t dg2_g12_ids[] = { 0x56a3, 0x56a4, 0x56b2, 0x56b3, 0x5696, 0x5697 };
static const uint16_t bmg_g21_ids[] = { 0xe202, 0xe209, 0xe20b, 0xe20c, 0xe20d,
					0xe210, 0xe211, 0xe212, 0xe216 };

static int id_in(uint16_t id, const uint16_t *ids, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		if (ids[i] == id)
			return 1;
	return 0;
}

#define ID_IN(id, table) id_in(id, table, sizeof(table) / sizeof(table[0]))

static uint8_t subplatform_of(const struct i915_device *i915)
{
	uint16_t id = i915->devid;

	switch (i915->info->platform) {
	case I915_PLATFORM_TIGERLAKE:
		return ID_IN(id, tgl_uy_ids) ? I915_SUBPLATFORM_TGL_UY : 0;
	case I915_PLATFORM_ALDERLAKE_S:
		return ID_IN(id, rpl_s_ids) ? I915_SUBPLATFORM_RPL_S : 0;
	case I915_PLATFORM_ALDERLAKE_P:
		if (ID_IN(id, adl_n_ids))
			return I915_SUBPLATFORM_ADL_N;
		if (ID_IN(id, rpl_u_ids))
			return I915_SUBPLATFORM_RPL_U;
		if (ID_IN(id, rpl_p_ids))
			return I915_SUBPLATFORM_RPL_P;
		return 0;
	case I915_PLATFORM_DG2:
		if (ID_IN(id, dg2_g10_ids))
			return I915_SUBPLATFORM_DG2_G10;
		if (ID_IN(id, dg2_g11_ids))
			return I915_SUBPLATFORM_DG2_G11;
		if (ID_IN(id, dg2_g12_ids))
			return I915_SUBPLATFORM_DG2_G12;
		return 0;
	case I915_PLATFORM_BATTLEMAGE:
		return ID_IN(id, bmg_g21_ids) ? I915_SUBPLATFORM_BMG_G21 : 0;
	default:
		return 0;
	}
}

/* ---- steppings from the revision id ---------------------------------------- */

/* Index: the PCI revision id; value: the stepping (0 = not a known one). */
static const uint8_t tgl_uy_steps[] = { I915_STEP_A0, I915_STEP_B0, I915_STEP_B1, I915_STEP_C0 };
static const uint8_t tgl_steps[] = { I915_STEP_A0, I915_STEP_B0 };
static const uint8_t rkl_steps[] = { I915_STEP_A0, I915_STEP_B0, 0, 0, I915_STEP_C0 };
static const uint8_t dg1_steps[] = { I915_STEP_A0, I915_STEP_B0 };
static const uint8_t adls_steps[] = { [0x0] = I915_STEP_A0, [0x1] = I915_STEP_A0,
				      [0x4] = I915_STEP_B0, [0x8] = I915_STEP_C0,
				      [0xc] = I915_STEP_D0 };
static const uint8_t adlp_steps[] = { [0x0] = I915_STEP_A0, [0x4] = I915_STEP_B0,
				      [0x8] = I915_STEP_C0, [0xc] = I915_STEP_C0 };
static const uint8_t rpls_steps[] = { [0x4] = I915_STEP_D0, [0xc] = I915_STEP_D0 };
static const uint8_t rplp_steps[] = { [0x4] = I915_STEP_C0 };
static const uint8_t adln_steps[] = { [0x0] = I915_STEP_A0 };
static const uint8_t dg2_g10_steps[] = { [0x0] = I915_STEP_A0, [0x1] = I915_STEP_A1,
					 [0x4] = I915_STEP_B0, [0x8] = I915_STEP_C0 };
static const uint8_t dg2_g11_steps[] = { [0x0] = I915_STEP_A0, [0x4] = I915_STEP_B0,
					 [0x5] = I915_STEP_B1 };
static const uint8_t dg2_g12_steps[] = { [0x0] = I915_STEP_A0, [0x1] = I915_STEP_A1 };
static const uint8_t icl_steps[] = { [7] = I915_STEP_D0 };
static const uint8_t ehl_steps[] = { I915_STEP_A0, I915_STEP_B0 };

/* The stepping a revision id names in a table.  A revision the table
 * does not know is taken as the next one it does know, or, past its end,
 * as newer than all of them. */
static uint8_t step_from(const uint8_t *steps, unsigned n, uint8_t revid)
{
	if (revid < n && steps[revid])
		return steps[revid];
	while (revid < n && !steps[revid])
		revid++;
	return revid < n ? steps[revid] : I915_STEP_FUTURE;
}

#define STEP_FROM(table, revid) step_from(table, sizeof(table), revid)

static uint8_t step_of_revid(const struct i915_device *i915)
{
	uint8_t r = i915->revid;

	switch (i915->subplatform) {
	case I915_SUBPLATFORM_TGL_UY: return STEP_FROM(tgl_uy_steps, r);
	case I915_SUBPLATFORM_ADL_N: return STEP_FROM(adln_steps, r);
	case I915_SUBPLATFORM_RPL_S: return STEP_FROM(rpls_steps, r);
	case I915_SUBPLATFORM_RPL_P: return STEP_FROM(rplp_steps, r);
	case I915_SUBPLATFORM_DG2_G10: return STEP_FROM(dg2_g10_steps, r);
	case I915_SUBPLATFORM_DG2_G11: return STEP_FROM(dg2_g11_steps, r);
	case I915_SUBPLATFORM_DG2_G12: return STEP_FROM(dg2_g12_steps, r);
	default: break;
	}
	switch (i915->info->platform) {
	case I915_PLATFORM_TIGERLAKE: return STEP_FROM(tgl_steps, r);
	case I915_PLATFORM_ROCKETLAKE: return STEP_FROM(rkl_steps, r);
	case I915_PLATFORM_DG1: return STEP_FROM(dg1_steps, r);
	case I915_PLATFORM_ALDERLAKE_S: return STEP_FROM(adls_steps, r);
	case I915_PLATFORM_ALDERLAKE_P: return STEP_FROM(adlp_steps, r);
	case I915_PLATFORM_ICELAKE: return STEP_FROM(icl_steps, r);
	case I915_PLATFORM_ELKHARTLAKE:
	case I915_PLATFORM_JASPERLAKE: return STEP_FROM(ehl_steps, r);
	default: return I915_STEP_NONE;
	}
}

/* ---- the IP versions ----------------------------------------------------------- */

static uint16_t ip_of_descriptor(const struct intel_device_info *info)
{
	switch (info->gen_x10) {
	case 125: return I915_IP(12, 55);
	case 127: return I915_IP(12, 70);
	default: return I915_IP(info->gen_x10 / 10, (info->gen_x10 % 10) * 10);
	}
}

/* GMD_ID's stepping field counts A0 as 0, A1 as 1 ... B0 as 4: the same
 * order as the driver's steppings, one off. */
static uint8_t gmd_step(uint32_t v)
{
	uint32_t s = GMD_ID_STEP(v) + I915_STEP_A0;
	return s >= I915_STEP_FUTURE ? I915_STEP_FUTURE : (uint8_t)s;
}

void i915_step_init(struct i915_device *i915)
{
	const struct intel_device_info *info = i915->info;

	i915->subplatform = subplatform_of(i915);
	i915->gt_ip = i915->media_ip = ip_of_descriptor(info);
	i915->has_media_gt = 0;
	i915->media_subip = 0;
	i915->ggtt_wa_reg = i915->ggtt_wa_every = i915->ggtt_wa_count = 0;
	/* Wa_15015404425: Lunar Lake and Panther Lake (Wildcat Lake is a
	 * Panther Lake) want four dummy writes ahead of every register
	 * read, from the first one on; from the B0 stepping of Panther
	 * Lake's media on they need it no longer (below). */
	i915->mmio_read_flush = info->platform == I915_PLATFORM_LUNARLAKE ||
				info->platform == I915_PLATFORM_PANTHERLAKE ||
				info->platform == I915_PLATFORM_WILDCATLAKE;
	if (info->gen_x10 >= 127) {
		/* The GMD_ID registers are always on: no forcewake. */
		uint32_t g = i915_read32_fw(i915, GMD_ID_GRAPHICS);
		uint32_t m = i915_read32_fw(i915, GMD_ID_MEDIA);
		if (GMD_ID_ARCH(g)) {
			i915->gt_ip = (uint16_t)I915_IP(GMD_ID_ARCH(g), GMD_ID_RELEASE(g));
			i915->gt_step = gmd_step(g);
		} else {
			/* Wa_22012778468: early parts report nothing; they
			 * are 12.70 */
			i915->gt_ip = I915_IP(12, 70);
			i915->gt_step = I915_STEP_A0;
		}
		if (GMD_ID_ARCH(m)) {
			i915->media_ip = (uint16_t)I915_IP(GMD_ID_ARCH(m), GMD_ID_RELEASE(m));
			i915->media_step = gmd_step(m);
			i915->media_subip = (uint8_t)GMD_ID_SUBIP_FLAG(m);
			i915->has_media_gt = 1;
		}
		if ((info->platform == I915_PLATFORM_PANTHERLAKE ||
		     info->platform == I915_PLATFORM_WILDCATLAKE) &&
		    i915->has_media_gt && i915->media_step >= I915_STEP_B0)
			i915->mmio_read_flush = 0;
		/* Wa_22019338487: Lunar Lake's media GT (and Panther Lake's
		 * before B0) every 63 writes of the global GTT, Battlemage's
		 * primary GT every 1100 */
		if (i915->has_media_gt &&
		    (i915->media_ip == I915_IP(20, 0) ||
		     (i915->media_ip == I915_IP(30, 0) && i915->media_step < I915_STEP_B0))) {
			i915->ggtt_wa_reg = GMD_ID_MEDIA;
			i915->ggtt_wa_every = 63;
		} else if (i915->gt_ip == I915_IP(20, 1)) {
			i915->ggtt_wa_reg = GMD_ID_GRAPHICS;
			i915->ggtt_wa_every = 1100;
		}
		kprintf("[drm] i915: graphics IP %u.%02u stepping %s, media IP %u.%02u stepping %s\n",
			i915->gt_ip / 100, i915->gt_ip % 100, i915_step_name(i915->gt_step),
			i915->media_ip / 100, i915->media_ip % 100,
			i915->has_media_gt ? i915_step_name(i915->media_step) : "--");
		return;
	}
	i915->gt_step = i915->media_step = step_of_revid(i915);
	if (i915->gt_step)
		i915_dbg("[drm] i915: revision %02x is stepping %s\n", i915->revid,
			 i915_step_name(i915->gt_step));
}

const char *i915_step_name(uint8_t step)
{
	static char names[11][4][3];
	static int ready;

	if (!ready) {
		for (int l = 0; l < 11; l++)
			for (int n = 0; n < 4; n++) {
				names[l][n][0] = (char)('A' + l);
				names[l][n][1] = (char)('0' + n);
				names[l][n][2] = 0;
			}
		ready = 1;
	}
	if (step == I915_STEP_NONE)
		return "unknown";
	if (step >= I915_STEP_FUTURE)
		return "future";
	return names[(step - 1) / 4][(step - 1) % 4];
}
