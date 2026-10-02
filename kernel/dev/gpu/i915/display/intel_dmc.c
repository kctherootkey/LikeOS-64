// LikeOS -- loading the display microcontroller (DMC).
//
// The DMC is a small controller in the display engine that runs the
// display's deep power states: with the pipes idle it can power the
// display core down and bring it back on its own.  Its program comes as
// a firmware file (one per platform, chosen per silicon stepping);
// loading it is writing the program into the controller's memory
// through a register window and making the few register writes the
// image asks for.
//
// From Tiger Lake a package also carries a program for each pipe's own
// controller; Meteor Lake's mtl_dmc.bin has the main one and four.  The
// event handlers a program arms through its register writes are first
// all disarmed, the pipe controllers' stay disarmed (their events are
// for features this driver does not use), and on Tiger Lake and Alder
// Lake-S two of the main controller's are disarmed or corrected as their
// shipped firmware needs.
//
// Battlemage loads bmg_dmc.bin, Lunar Lake xe2lpd_dmc.bin, Panther Lake
// xe3lpd_dmc.bin, Wildcat Lake xe3lpd_3002_dmc.bin and Nova Lake
// xe3p_lpd_dmc.bin (the device table names them); from display version
// 20 the programs may be up to 32 KB, the pipe DMCs have an interrupt
// and flip queues of their own that are put in a known state whenever a
// pipe DMC is enabled or disabled, and the pipe DMCs take their flip
// queue timestamps from their own pipe.
//
// The program is loaded so that the hardware is as the firmware package
// expects it; the power states themselves (DC5/DC6) are not enabled by
// this driver.  Entering them is the controller's decision whenever the
// display is idle, and getting back out relies on power-well and clock
// state this driver does not track yet; with the states off the display
// simply stays powered, which is what every other part of the driver
// assumes.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dmc.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/io/console.h>
#include <kernel/ke/firmware.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* What was loaded, kept for the pipe controllers that need their
 * program or registers again when their pipe comes back. */
static struct {
	void *blob;
	struct intel_dmc_image img[INTEL_DMC_MAX_IDS];
	uint32_t present;
} g_dmc;

/* ---- the stepping the images are chosen by -------------------------------- */

/* A display stepping as the packages name it: a letter and a digit;
 * the step index counts four substeps to a letter (A0 = 0, B0 = 4). */
static void step_name(int step, char *st, char *sub)
{
	*st = (char)('A' + step / 4);
	*sub = (char)('0' + step % 4);
}

#define S_A0 0
#define S_A2 2
#define S_B0 4
#define S_B1 5
#define S_C0 8
#define S_D0 12
#define S_E0 16
#define S_G0 24
#define S_H0 28
#define S_I1 33
#define S_J0 36
#define S_NONE -1

/* The display stepping from the PCI revision through a platform table;
 * a revision the table skips takes the next one listed, one past the
 * table is unknown. */
static int step_from_map(const int8_t *map, int n, int rev)
{
	if (rev < 0)
		return S_NONE;
	while (rev < n && map[rev] == S_NONE)
		rev++;
	return rev < n ? map[rev] : S_NONE;
}

static int dev_in(uint16_t id, const uint16_t *ids, int n)
{
	for (int i = 0; i < n; i++)
		if (ids[i] == id)
			return 1;
	return 0;
}

static int display_step(struct i915_device *i915)
{
	static const int8_t skl[] = { -1, -1, -1, -1, -1, -1, S_G0, S_H0, -1, S_J0, S_I1 };
	static const int8_t kbl[] = { -1, S_B0, S_B0, S_B0, S_C0, S_B1, S_B1, S_C0 };
	static const int8_t bxt[] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, S_C0, S_C0, S_D0, S_E0 };
	static const int8_t glk[] = { -1, -1, -1, S_B0 };
	static const int8_t icl[] = { -1, -1, -1, -1, -1, -1, -1, S_D0 };
	static const int8_t jsl_ehl[] = { S_A0, S_B0 };
	static const int8_t tgl[] = { S_B0, S_D0 };
	static const int8_t tgl_uy[] = { S_A0, S_C0, S_C0, S_D0 };
	static const int8_t dg1[] = { S_A0, S_B0 };
	static const int8_t rkl[] = { S_A0, S_B0, -1, -1, S_C0 };
	static const int8_t adls[] = { S_A0, S_A2, -1, -1, S_B0, -1, -1, -1, S_B0, -1, -1, -1, S_C0 };
	static const int8_t rpls[] = { -1, -1, -1, -1, S_D0, -1, -1, -1, -1, -1, -1, -1, S_C0 };
	static const int8_t adlp[] = { S_A0, -1, -1, -1, S_B0, -1, -1, -1, S_C0, -1, -1, -1, S_D0 };
	static const int8_t adln[] = { S_D0 };
	static const int8_t rplpu[] = { -1, -1, -1, -1, S_E0 };
	static const int8_t dg2_g10[] = { S_A0, S_A0, -1, -1, S_B0, -1, -1, -1, S_C0 };
	static const int8_t dg2_g11[] = { S_B0, -1, -1, -1, S_C0, S_C0 };
	static const int8_t dg2_g12[] = { S_C0, S_C0 };
	static const uint16_t tgl_uy_ids[] = { 0x9A40, 0x9A49, 0x9A59, 0x9A78, 0x9AC0, 0x9AC9,
					       0x9AD9, 0x9AF8 };
	static const uint16_t rpls_ids[] = { 0xA780, 0xA781, 0xA782, 0xA783, 0xA788, 0xA789,
					     0xA78A, 0xA78B };
	static const uint16_t adln_ids[] = { 0x46D0, 0x46D1, 0x46D2, 0x46D3, 0x46D4 };
	static const uint16_t rplpu_ids[] = { 0xA720, 0xA7A0, 0xA7A8, 0xA7AA, 0xA7AB, 0xA721,
					      0xA7A1, 0xA7A9, 0xA7AC, 0xA7AD };
	static const uint16_t dg2_g10_ids[] = { 0x56A0, 0x56A1, 0x56A2, 0x56BE, 0x56BF, 0x5690,
						0x5691, 0x5692 };
	static const uint16_t dg2_g11_ids[] = { 0x56A5, 0x56A6, 0x56B0, 0x56B1, 0x56BA, 0x56BB,
						0x56BC, 0x56BD, 0x5693, 0x5694, 0x5695 };
#define MAP(t) step_from_map((t), (int)sizeof(t), rev)
#define IDS(t) dev_in(i915->devid, (t), (int)(sizeof(t) / sizeof((t)[0])))
	int rev = pci_cfg_read8(i915->pci, 0x08);

	/* display version 14 reports its own */
	if (i915->info->display_ver >= 14)
		return i915->display.display_step == 0xff ? S_NONE : i915->display.display_step;
	switch (i915->info->platform) {
	case I915_PLATFORM_SKYLAKE: return MAP(skl);
	case I915_PLATFORM_KABYLAKE:
	case I915_PLATFORM_COFFEELAKE:
	case I915_PLATFORM_COMETLAKE: return MAP(kbl);
	case I915_PLATFORM_BROXTON: return MAP(bxt);
	case I915_PLATFORM_GEMINILAKE: return MAP(glk);
	case I915_PLATFORM_ICELAKE: return MAP(icl);
	case I915_PLATFORM_ELKHARTLAKE:
	case I915_PLATFORM_JASPERLAKE: return MAP(jsl_ehl);
	case I915_PLATFORM_TIGERLAKE: return IDS(tgl_uy_ids) ? MAP(tgl_uy) : MAP(tgl);
	case I915_PLATFORM_DG1: return MAP(dg1);
	case I915_PLATFORM_ROCKETLAKE: return MAP(rkl);
	case I915_PLATFORM_ALDERLAKE_S: return IDS(rpls_ids) ? MAP(rpls) : MAP(adls);
	case I915_PLATFORM_ALDERLAKE_P:
		if (IDS(adln_ids))
			return MAP(adln);
		if (IDS(rplpu_ids))
			return MAP(rplpu);
		return MAP(adlp);
	case I915_PLATFORM_DG2:
		if (IDS(dg2_g10_ids))
			return MAP(dg2_g10);
		if (IDS(dg2_g11_ids))
			return MAP(dg2_g11);
		return MAP(dg2_g12);
	default:
		return S_NONE;
	}
#undef MAP
#undef IDS
}

static void stepping_for(struct i915_device *i915, char *st, char *sub)
{
	int step = display_step(i915);
	*st = INTEL_DMC_ANY_STEPPING;
	*sub = INTEL_DMC_ANY_STEPPING;
	if (step >= 0)
		step_name(step, st, sub);
}

/* The largest program each platform's controller holds, in bytes. */
static uint32_t max_fw_bytes(struct i915_device *i915)
{
	uint8_t p = i915->info->platform;
	if (i915->info->display_ver >= 20)
		return 0x8000;
	if (i915->info->display_ver >= 14)
		return 0x7000;
	if (p == I915_PLATFORM_DG2 || p == I915_PLATFORM_ALDERLAKE_P)
		return 0x20000;
	if (i915->info->display_ver >= 11)
		return 0x6000;
	if (p == I915_PLATFORM_GEMINILAKE)
		return 0x4000;
	return 0x3000;
}

/* ---- the event handlers --------------------------------------------------------- */

static uint32_t dmc_reg_base(struct i915_device *i915, int id)
{
	if (id == INTEL_DMC_ID_MAIN)
		return DMC_MAIN_REG_BASE;
	return (i915->info->display_ver >= 13 ? DMC_ADLP_PIPE_REG_BASE : DMC_TGL_PIPE_REG_BASE) +
	       0x400 * (uint32_t)(id - 1);
}

static uint32_t evt_ctl(struct i915_device *i915, int id, int h)
{
	return dmc_reg_base(i915, id) + DMC_EVT_CTL_OFF + 4 * (uint32_t)h;
}

static uint32_t evt_htp(struct i915_device *i915, int id, int h)
{
	return dmc_reg_base(i915, id) + DMC_EVT_HTP_OFF + 4 * (uint32_t)h;
}

static int is_evt_ctl(struct i915_device *i915, int id, uint32_t reg)
{
	return reg >= evt_ctl(i915, id, 0) &&
	       reg < evt_ctl(i915, id, DMC_EVENT_HANDLER_COUNT_GEN12);
}

static int is_evt_htp(struct i915_device *i915, int id, uint32_t reg)
{
	return reg >= evt_htp(i915, id, 0) &&
	       reg < evt_htp(i915, id, DMC_EVENT_HANDLER_COUNT_GEN12);
}

static uint32_t evt_id(uint32_t ctl)
{
	return (ctl & DMC_EVT_CTL_EVENT_ID_MASK) >> DMC_EVT_CTL_EVENT_ID_SHIFT;
}

/* A handler made inert: never triggered.  The enable bit cannot be
 * cleared once set, so it is carried over. */
static uint32_t evt_ctl_disabled(uint32_t ctl)
{
	return (ctl & DMC_EVT_CTL_ENABLE) | (DMC_EVT_CTL_TYPE_EDGE_0_1 << DMC_EVT_CTL_TYPE_SHIFT) |
	       (DMC_EVENT_FALSE << DMC_EVT_CTL_EVENT_ID_SHIFT);
}

static void disable_all_event_handlers(struct i915_device *i915, int id)
{
	if (i915->info->display_ver < 12)
		return;
	for (int h = 0; h < DMC_EVENT_HANDLER_COUNT_GEN12; h++) {
		i915_write32(i915, evt_ctl(i915, id, h),
			     (DMC_EVT_CTL_TYPE_EDGE_0_1 << DMC_EVT_CTL_TYPE_SHIFT) |
				     (DMC_EVENT_FALSE << DMC_EVT_CTL_EVENT_ID_SHIFT));
		i915_write32(i915, evt_htp(i915, id, h), 0);
	}
}

/* The shipped Tiger Lake and Alder Lake-S main firmware arms its
 * refresh-rate handler on the undelayed vblank where the delayed one is
 * meant; Alder Lake-S does not restore that handler after DC6 at all,
 * so there it is cleared outright. */
static void fixup_events(struct i915_device *i915, int id, struct intel_dmc_image *img)
{
	uint8_t p = i915->info->platform;
	if (i915->info->display_ver < 12)
		return;
	for (uint32_t i = 0; i + 1 < img->mmio_count; i++) {
		uint32_t rc = img->mmioaddr[i], rh = img->mmioaddr[i + 1];
		if (!is_evt_ctl(i915, id, rc) || !is_evt_htp(i915, id, rh))
			continue;
		if (rc - evt_ctl(i915, id, 0) != rh - evt_htp(i915, id, 0))
			continue;
		if (evt_id(img->mmiodata[i]) != MAINDMC_EVENT_VBLANK_A)
			continue;
		if (p == I915_PLATFORM_ALDERLAKE_S && id == INTEL_DMC_ID_MAIN) {
			img->mmiodata[i] = 0;
			img->mmiodata[i + 1] = 0;
		} else if (p == I915_PLATFORM_TIGERLAKE || p == I915_PLATFORM_ALDERLAKE_S) {
			img->mmiodata[i] &= ~DMC_EVT_CTL_EVENT_ID_MASK;
			img->mmiodata[i] |= MAINDMC_EVENT_VBLANK_DELAYED_A << DMC_EVT_CTL_EVENT_ID_SHIFT;
		}
	}
}

/* The handlers left disarmed: every pipe controller's, and on Tiger
 * Lake the main one's flip queue (millisecond clock) handler, and the
 * refresh-rate one on Tiger Lake and Alder Lake-S. */
static int disarm_event(struct i915_device *i915, int id, uint32_t reg, uint32_t data)
{
	uint8_t p = i915->info->platform;
	if (!is_evt_ctl(i915, id, reg))
		return 0;
	if (id != INTEL_DMC_ID_MAIN)
		return 1;
	if (p == I915_PLATFORM_TIGERLAKE && evt_id(data) == MAINDMC_EVENT_CLK_MSEC)
		return 1;
	if ((p == I915_PLATFORM_TIGERLAKE || p == I915_PLATFORM_ALDERLAKE_S) &&
	    evt_id(data) == MAINDMC_EVENT_VBLANK_DELAYED_A)
		return 1;
	return 0;
}

static void dmc_load_mmio(struct i915_device *i915, int id)
{
	const struct intel_dmc_image *img = &g_dmc.img[id];
	for (uint32_t i = 0; i < img->mmio_count; i++) {
		uint32_t v = img->mmiodata[i];
		if (disarm_event(i915, id, img->mmioaddr[i], v))
			v = evt_ctl_disabled(v);
		i915_write32(i915, img->mmioaddr[i], v);
	}
}

static void dmc_load_program(struct i915_device *i915, int id)
{
	const struct intel_dmc_image *img = &g_dmc.img[id];
	const uint8_t *p = img->payload;

	disable_all_event_handlers(i915, id);
	for (uint32_t i = 0; i < img->payload_dwords; i++, p += 4) {
		uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
			     ((uint32_t)p[3] << 24);
		i915_write32_fw(i915, img->start_mmioaddr + i * 4, v);
	}
	dmc_load_mmio(i915, id);
}

/* The pipe controllers' clocks may not be gated while they are loaded:
 * Meteor Lake keeps pipes A and B ungated for good; Alder Lake-P and
 * DG2 all four while loading, A and B afterwards. */
static void pipedmc_clock_gating(struct i915_device *i915, int loading)
{
	if (i915->info->platform == I915_PLATFORM_METEORLAKE && loading) {
		i915_write32(i915, GEN9_CLKGATE_DIS_0,
			     i915_read32(i915, GEN9_CLKGATE_DIS_0) | MTL_PIPEDMC_GATING_DIS(PIPE_A) |
				     MTL_PIPEDMC_GATING_DIS(PIPE_B));
	} else if (i915->info->display_ver == 13) {
		for (int pipe = loading ? PIPE_A : PIPE_C; pipe <= PIPE_D; pipe++) {
			uint32_t v = i915_read32(i915, CLKGATE_DIS_PSL_EXT(pipe));
			i915_write32(i915, CLKGATE_DIS_PSL_EXT(pipe),
				     loading ? (v | PIPEDMC_GATING_DIS) : (v & ~PIPEDMC_GATING_DIS));
		}
	}
}

static void dmc_program(struct i915_device *i915)
{
	pipedmc_clock_gating(i915, 1);
	for (int id = 0; id < INTEL_DMC_MAX_IDS; id++)
		if (g_dmc.present & (1u << id))
			dmc_load_program(i915, id);
	/* version 20+: each pipe DMC's flip queue timestamps from its pipe */
	if (i915->info->display_ver >= 20)
		i915_write32(i915, DMC_FQ_W2_PTS_CFG_SEL,
			     DMC_W2_PTS_CONFIG_SELECT(PIPE_D) | DMC_W2_PTS_CONFIG_SELECT(PIPE_C) |
				     DMC_W2_PTS_CONFIG_SELECT(PIPE_B) |
				     DMC_W2_PTS_CONFIG_SELECT(PIPE_A));
	/* keep the cores and the memory-up path out of the controller's
	 * hands while the display is driven */
	uint32_t dbg = i915_read32(i915, DC_STATE_DEBUG);
	dbg |= DC_STATE_DEBUG_MASK_MEMORY_UP | DC_STATE_DEBUG_MASK_CORES;
	i915_write32(i915, DC_STATE_DEBUG, dbg);
	(void)i915_read32(i915, DC_STATE_DEBUG);
	/* version 35: the DC state counters on */
	if (i915->info->display_ver >= 35)
		i915_write32(i915, DC_COUNT_EN, DC_COUNT_EN_COUNTER_ENABLE);
	pipedmc_clock_gating(i915, 0);
}

/* ---- the pipe DMCs of version 20 on ---------------------------------------------- */

/* The interrupts of a pipe DMC that are reported: version 35 the flip
 * queue and errors, 30 the faults as well; 20 leaves the error out (it
 * fires on Lunar Lake's pipe B at the first DC state transition). */
static uint32_t pipedmc_interrupt_mask(struct i915_device *i915)
{
	if (i915->info->display_ver >= 35)
		return PIPEDMC_FLIPQ_PROG_DONE | PIPEDMC_ERROR;
	if (i915->info->display_ver >= 30)
		return PIPEDMC_FLIPQ_PROG_DONE | PIPEDMC_GTT_FAULT | PIPEDMC_ATS_FAULT |
		       PIPEDMC_ERROR;
	return PIPEDMC_FLIPQ_PROG_DONE | PIPEDMC_GTT_FAULT | PIPEDMC_ATS_FAULT;
}

/* The flip queues empty and stopped (this driver does not use them). */
static void pipedmc_flipq_reset(struct i915_device *i915, int pipe)
{
	i915_write32(i915, PIPEDMC_FQ_CTRL(pipe), 0);
	i915_write32(i915, PIPEDMC_SCANLINECMPLOWER(pipe), 0);
	i915_write32(i915, PIPEDMC_SCANLINECMPUPPER(pipe), 0);
	for (int fq = 0; fq < PIPEDMC_FLIPQ_COUNT; fq++) {
		i915_write32(i915, PIPEDMC_FPQ_HP(pipe, fq), 0);
		i915_write32(i915, PIPEDMC_FPQ_CHP(pipe, fq), 0);
	}
	i915_write32(i915, PIPEDMC_FPQ_ATOMIC_TP(pipe), 0);
}

/* Does a pipe DMC need its registers written again when its pipe comes
 * back: version 30 and 35 for pipes C and D (in PG0, not restored by the
 * main DMC); not on Lunar Lake, Battlemage or DG2; Alder Lake-P and
 * Meteor Lake for pipes C and D. */
static int need_pipedmc_load_mmio(struct i915_device *i915, int pipe)
{
	int ver = i915->info->display_ver;

	if (ver >= 30 && ver <= 35)
		return pipe >= PIPE_C;
	if (ver == 20 || i915->info->platform == I915_PLATFORM_BATTLEMAGE ||
	    i915->info->platform == I915_PLATFORM_DG2)
		return 0;
	if (ver == 13 || ver == 14)
		return pipe >= PIPE_C;
	return 0;
}

/* The windows each controller's register writes may fall in. */
static void mmio_window(struct i915_device *i915, int id, uint32_t *lo, uint32_t *hi)
{
	if (i915->info->display_ver < 12) {
		*lo = DMC_MMIO_START_RANGE;
		*hi = DMC_MMIO_END_RANGE;
	} else if (id == INTEL_DMC_ID_MAIN) {
		*lo = DMC_MMIO_START_RANGE; /* a version-1 header still may */
		*hi = DMC_MMIO_END_RANGE;
	} else if (i915->info->display_ver >= 13) {
		*lo = ADLP_PIPE_MMIO_START;
		*hi = ADLP_PIPE_MMIO_END;
	} else {
		*lo = TGL_PIPE_MMIO_START(id);
		*hi = TGL_PIPE_MMIO_END(id);
	}
}

int intel_dmc_load(struct i915_device *i915)
{
	void *data = NULL;
	size_t len = 0;
	char st, sub;

	if (!(i915->info->flags & I915_INFO_HAS_DMC) || !i915->info->dmc_fw)
		return 0;
	if (i915->display.dmc_loaded) {
		/* A resume: the program memory went with the power. */
		if (g_dmc.present)
			dmc_program(i915);
		return 0;
	}
	int rc = firmware_request(i915->info->dmc_fw, &data, &len);
	if (rc) {
		kprintf("[drm] i915: DMC firmware %s not available (%d); the display's deep power states stay off\n",
			i915->info->dmc_fw, rc);
		return rc;
	}
	stepping_for(i915, &st, &sub);
	mm_memset(&g_dmc, 0, sizeof(g_dmc));
	uint32_t max_dwords = max_fw_bytes(i915) / 4;
	int ids = i915->info->display_ver >= 12 ? INTEL_DMC_MAX_IDS : 1;
	for (int id = 0; id < ids; id++) {
		uint32_t lo, hi;
		struct intel_dmc_image *img = &g_dmc.img[id];
		mmio_window(i915, id, &lo, &hi);
		rc = intel_dmc_parse_id(data, (unsigned)len, st, sub, id, max_dwords, lo, hi,
					DMC_MMIO_START_RANGE, img);
		if (rc) {
			if (id == INTEL_DMC_ID_MAIN) {
				kprintf("[drm] i915: DMC firmware %s rejected (%d) for stepping %c%c\n",
					i915->info->dmc_fw, rc, st, sub);
				firmware_release(data);
				return -EINVAL;
			}
			continue; /* no program for that pipe */
		}
		/* a Gen12+ main image's writes stay in its own window */
		if (id == INTEL_DMC_ID_MAIN && img->header_ver == 3) {
			int bad = 0;
			for (uint32_t i = 0; i < img->mmio_count; i++)
				if (img->mmioaddr[i] < TGL_MAIN_MMIO_START ||
				    img->mmioaddr[i] > TGL_MAIN_MMIO_END)
					bad = 1;
			if (bad) {
				kprintf("[drm] i915: DMC firmware %s writes outside its window\n",
					i915->info->dmc_fw);
				firmware_release(data);
				return -EINVAL;
			}
		}
		fixup_events(i915, id, img);
		g_dmc.present |= 1u << id;
	}
	g_dmc.blob = data;
	dmc_program(i915);
	i915->display.dmc_loaded = 1;
	i915->display.dmc_ids_loaded = g_dmc.present;
	i915->display.dmc_version = g_dmc.img[0].version;
	kprintf("[drm] i915: DMC %u.%u loaded from %s (image %c%c, %u dwords at %05x, %u register writes, pipe programs %x); DC states not enabled\n",
		g_dmc.img[0].version >> 16, g_dmc.img[0].version & 0xffff, i915->info->dmc_fw,
		g_dmc.img[0].stepping, g_dmc.img[0].substepping, g_dmc.img[0].payload_dwords,
		g_dmc.img[0].start_mmioaddr, g_dmc.img[0].mmio_count, g_dmc.present >> 1);
	/* the images point into the file, which stays for the pipes'
	 * reloads */
	return 0;
}

void intel_dmc_enable_pipe(struct i915_device *i915, int pipe)
{
	int id = INTEL_DMC_ID_PIPE(pipe);
	int ver = i915->info->display_ver;

	if (pipe < 0 || id >= INTEL_DMC_MAX_IDS || !(g_dmc.present & (1u << id)))
		return;
	/* Tiger Lake loses a pipe controller's program with PG1; Alder
	 * Lake-P, Meteor Lake and version 30 on restore pipes A and B
	 * themselves but not C and D's registers */
	if (ver == 12)
		dmc_load_program(i915, id);
	else if (need_pipedmc_load_mmio(i915, pipe))
		dmc_load_mmio(i915, id);
	if (ver >= 20) {
		uint32_t mask = pipedmc_interrupt_mask(i915);
		pipedmc_flipq_reset(i915, pipe);
		i915_write32(i915, PIPEDMC_INTERRUPT(pipe), mask);
		i915_write32(i915, PIPEDMC_INTERRUPT_MASK(pipe), ~mask);
	}
	if (ver >= 14)
		i915_write32(i915, MTL_PIPEDMC_CONTROL,
			     i915_read32(i915, MTL_PIPEDMC_CONTROL) | PIPEDMC_ENABLE_MTL(pipe));
	else
		i915_write32(i915, PIPEDMC_CONTROL(pipe),
			     i915_read32(i915, PIPEDMC_CONTROL(pipe)) | PIPEDMC_ENABLE);
}

void intel_dmc_disable_pipe(struct i915_device *i915, int pipe)
{
	int id = INTEL_DMC_ID_PIPE(pipe);

	if (pipe < 0 || id >= INTEL_DMC_MAX_IDS || !(g_dmc.present & (1u << id)))
		return;
	if (i915->info->display_ver >= 14)
		i915_write32(i915, MTL_PIPEDMC_CONTROL,
			     i915_read32(i915, MTL_PIPEDMC_CONTROL) & ~PIPEDMC_ENABLE_MTL(pipe));
	else
		i915_write32(i915, PIPEDMC_CONTROL(pipe),
			     i915_read32(i915, PIPEDMC_CONTROL(pipe)) & ~PIPEDMC_ENABLE);
	if (i915->info->display_ver >= 20) {
		i915_write32(i915, PIPEDMC_INTERRUPT_MASK(pipe), ~0u);
		i915_write32(i915, PIPEDMC_INTERRUPT(pipe), pipedmc_interrupt_mask(i915));
		pipedmc_flipq_reset(i915, pipe);
	}
}
