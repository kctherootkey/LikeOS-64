// LikeOS -- the memory object control tables of Intel graphics.
//
// Every surface a batch touches names, in a few bits of its state, an
// entry of a table that says how the engine caches it: whether reads
// go through the last-level cache the CPU shares, whether the engine's
// own L3 keeps lines of it, how quickly they age out.  The table lives
// in registers the driver fills, and which entry means what is fixed by
// the graphics stack above: entry 0 caches nowhere, entry 1 leaves the
// choice to the page table, entry 2 caches everywhere.  A part whose
// table was never written runs with whatever the registers held at
// reset -- and then everything the samplers fetch through an entry the
// driver never set comes out wrong, while plain fills, which cache
// nothing, come out right.
//
// The table has two halves.  The per-engine half lives at a base that
// depends on the engine and is written as registers.  The L3 half is
// one table for the render engine, two entries a register; it belongs
// to the render context, so besides being written as registers it is
// loaded from the batch whose saved state becomes the golden image.
//
// Gen12 made the per-engine half one table for the whole GT (the global
// MOCS registers), and every platform since has its own table, fixed by
// the graphics stack's contract with the kernel: Tiger Lake's cannot
// change any more, Meteor Lake's has sixteen entries and points at page
// attribute table entries rather than naming caches.  The discrete parts
// leave the per-engine half to the hardware and only the L3 half is
// written.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2015-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_gt.h>
#include <kernel/dev/gpu/i915/i915_reg.h>

/* the per-engine half of an entry */
#define MOCS_LE_CACHEABILITY(v) ((uint32_t)(v) << 0) /* 0 page table, 1 uncached, 2 write-through, 3 write-back */
#define MOCS_LE_TARGET_CACHE(v) ((uint32_t)(v) << 2) /* 0 page table, 1 LLC, 2 LLC and eLLC */
#define MOCS_LE_AGE(v) ((uint32_t)(v) << 4)
/* the L3 half */
#define MOCS_L3_CACHEABILITY(v) ((uint32_t)(v) << 4) /* 0 direct, 1 uncached, 3 write-back */

#define MOCS_UNCACHED 0
#define MOCS_PAGE_TABLE 1
#define MOCS_CACHED 2

struct mocs_entry {
	uint32_t control; /* the per-engine half */
	uint16_t l3;
};

/* Gen9 (Skylake and its relatives): 64 entries, the three the stack
 * relies on set as it expects, the last one as the L3 needs it (it is
 * the entry the L3 names for everything it evicts, and the one that
 * forces an access past the L3: write-back in the shared cache, never
 * in L3), every other one the same as the page table entry so an
 * index nobody should use still behaves. */
#define GEN9_MOCS_ENTRIES 64
#define MOCS_L3_EVICT 63

static const struct mocs_entry gen9_mocs[] = {
	[MOCS_UNCACHED] = { MOCS_LE_CACHEABILITY(1) | MOCS_LE_TARGET_CACHE(2),
			    MOCS_L3_CACHEABILITY(1) },
	[MOCS_PAGE_TABLE] = { MOCS_LE_CACHEABILITY(0) | MOCS_LE_TARGET_CACHE(2) | MOCS_LE_AGE(3),
			      MOCS_L3_CACHEABILITY(3) },
	[MOCS_CACHED] = { MOCS_LE_CACHEABILITY(3) | MOCS_LE_TARGET_CACHE(2) | MOCS_LE_AGE(3),
			  MOCS_L3_CACHEABILITY(3) },
	[MOCS_L3_EVICT] = { MOCS_LE_CACHEABILITY(3) | MOCS_LE_TARGET_CACHE(1) | MOCS_LE_AGE(3),
			    MOCS_L3_CACHEABILITY(1) },
};

unsigned i915_mocs_entries(int gen)
{
	return gen == 9 ? GEN9_MOCS_ENTRIES : 0;
}

static const struct mocs_entry *mocs_entry(int gen, unsigned index)
{
	if (gen != 9)
		return NULL;
	if (index < sizeof(gen9_mocs) / sizeof(gen9_mocs[0]) &&
	    (index <= MOCS_CACHED || index == MOCS_L3_EVICT))
		return &gen9_mocs[index];
	return &gen9_mocs[MOCS_PAGE_TABLE];
}

uint32_t i915_mocs_control_value(int gen, unsigned index)
{
	const struct mocs_entry *m = mocs_entry(gen, index);
	return m ? m->control : 0;
}

/* Register `reg' of the L3 table holds entries 2*reg (low half) and
 * 2*reg+1 (high half). */
uint32_t i915_mocs_l3cc_value(int gen, unsigned reg)
{
	const struct mocs_entry *lo = mocs_entry(gen, 2 * reg);
	const struct mocs_entry *hi = mocs_entry(gen, 2 * reg + 1);
	if (!lo || !hi)
		return 0;
	return (uint32_t)lo->l3 | ((uint32_t)hi->l3 << 16);
}

static void mocs12_render_l3cc(struct i915_device *i915);

/* Where an engine's half of the table lives. */
static uint32_t mocs_base(const struct i915_engine *e)
{
	uint32_t base;
	switch (e->class) {
	case 0: base = GEN9_RCS_MOCS_BASE; break;
	case 1: base = GEN9_BCS_MOCS_BASE; break;
	case 2: base = GEN9_VCS_MOCS_BASE; break;
	case 3: base = GEN9_VECS_MOCS_BASE; break;
	default: return 0;
	}
	return base + (uint32_t)e->instance * 0x100;
}

/* Write the whole table for one engine, and the L3 table when the engine
 * is the render engine.  Called at engine init and after a reset. */
void i915_mocs_init_engine(struct i915_device *i915, struct i915_engine *e)
{
	int gen = i915->info->gen;
	unsigned n = i915_mocs_entries(gen);
	uint32_t base = mocs_base(e);

	/* Gen12 on: the global table is the GT's; the render engine's init
	 * writes the L3 half once more */
	if (i915->info->gen_x10 >= 120) {
		if (e->class == 0)
			mocs12_render_l3cc(i915);
		return;
	}
	if (!n || !base)
		return;
	for (unsigned i = 0; i < n; i++)
		i915_write32(i915, base + i * 4, i915_mocs_control_value(gen, i));
	if (e->class != 0)
		return;
	/* The L3 half is context state: the engine saves and restores it
	 * around every switch, so this write lasts only until the first
	 * context runs; every context loads its own copy from its ring. */
	for (unsigned i = 0; i < n / 2; i++)
		i915_write32(i915, GEN9_LNCFCMOCS(i), i915_mocs_l3cc_value(gen, i));
}

/* Load the L3 table from a batch of the render engine, as register
 * loads.  Returns the number of dwords emitted. */
uint32_t i915_mocs_l3cc_emit(struct i915_device *i915, int engine_class,
			     uint32_t *ring, uint32_t max_dwords)
{
	int gen = i915->info->gen;
	unsigned n = i915_mocs_entries(gen) / 2;
	uint32_t k = 0;

	if (!n || engine_class != 0 || max_dwords < 2 + n * 2)
		return 0;
	ring[k++] = MI_LOAD_REGISTER_IMM(n);
	for (unsigned i = 0; i < n; i++) {
		ring[k++] = GEN9_LNCFCMOCS(i);
		ring[k++] = i915_mocs_l3cc_value(gen, i);
	}
	if (k & 1)
		ring[k++] = MI_NOOP;
	return k;
}

/* ---- Gen12 and later --------------------------------------------------------- */

#define LE_CACHEABILITY(v) ((uint32_t)(v) << 0)
#define LE_TGT_CACHE(v) ((uint32_t)(v) << 2)
#define LE_LRUM(v) ((uint32_t)(v) << 4)
#define LE_AOM(v) ((uint32_t)(v) << 6)
#define LE_RSC(v) ((uint32_t)(v) << 7)
#define LE_SCC(v) ((uint32_t)(v) << 8)
#define LE_SCF(v) ((uint32_t)(v) << 14)
#define LE_SSE(v) ((uint32_t)(v) << 17)
#define L4_CACHEABILITY(v) ((uint32_t)(v) << 2)
#define IG_PAT(v) ((uint32_t)(v) << 8)
#define L3_ESC(v) ((v) << 0)
#define L3_SCC(v) ((v) << 1)
#define L3_CACHEABILITY(v) ((v) << 4)
#define L3_GLBGO(v) ((v) << 6)
#define L3_LKUP(v) ((v) << 7)

#define LE_0_PAGETABLE LE_CACHEABILITY(0)
#define LE_1_UC LE_CACHEABILITY(1)
#define LE_3_WB LE_CACHEABILITY(3)
#define LE_TC_0_PAGETABLE LE_TGT_CACHE(0)
#define LE_TC_1_LLC LE_TGT_CACHE(1)
#define L3_1_UC L3_CACHEABILITY(1)
#define L3_3_WB L3_CACHEABILITY(3)
#define L4_1_WT L4_CACHEABILITY(1)
#define L4_3_UC L4_CACHEABILITY(3)

#define GEN9_NUM_MOCS_ENTRIES 64 /* 62 and 63 reserved, but programmed */
#define MTL_NUM_MOCS_ENTRIES 16
#define I915_MOCS_PTE 1

struct mocs12_entry {
	uint32_t control;
	uint16_t l3;
	uint16_t used;
};

#define MOCS_ENTRY(idx, ctl, l3cc) [idx] = { (ctl), (uint16_t)(l3cc), 1 }

/* Gen11's entries, which Gen12 keeps */
#define GEN11_MOCS_ENTRIES                                                                 \
	/* Entries 0 and 1 are defined per platform */                                    \
	/* Base - L3 + LLC */                                                              \
	MOCS_ENTRY(2, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_3_WB),                        \
	/* Base - Uncached */                                                              \
	MOCS_ENTRY(3, LE_1_UC | LE_TC_1_LLC, L3_1_UC),                                     \
	/* Base - L3 */                                                                    \
	MOCS_ENTRY(4, LE_1_UC | LE_TC_1_LLC, L3_3_WB),                                     \
	/* Base - LLC */                                                                   \
	MOCS_ENTRY(5, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC),                        \
	/* Age 0 - LLC */                                                                  \
	MOCS_ENTRY(6, LE_3_WB | LE_TC_1_LLC | LE_LRUM(1), L3_1_UC),                        \
	/* Age 0 - L3 + LLC */                                                             \
	MOCS_ENTRY(7, LE_3_WB | LE_TC_1_LLC | LE_LRUM(1), L3_3_WB),                        \
	/* Age: Don't Chg. - LLC */                                                        \
	MOCS_ENTRY(8, LE_3_WB | LE_TC_1_LLC | LE_LRUM(2), L3_1_UC),                        \
	/* Age: Don't Chg. - L3 + LLC */                                                   \
	MOCS_ENTRY(9, LE_3_WB | LE_TC_1_LLC | LE_LRUM(2), L3_3_WB),                        \
	/* No AOM - LLC */                                                                 \
	MOCS_ENTRY(10, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_AOM(1), L3_1_UC),           \
	/* No AOM - L3 + LLC */                                                            \
	MOCS_ENTRY(11, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_AOM(1), L3_3_WB),           \
	/* No AOM; Age 0 - LLC */                                                          \
	MOCS_ENTRY(12, LE_3_WB | LE_TC_1_LLC | LE_LRUM(1) | LE_AOM(1), L3_1_UC),           \
	/* No AOM; Age 0 - L3 + LLC */                                                     \
	MOCS_ENTRY(13, LE_3_WB | LE_TC_1_LLC | LE_LRUM(1) | LE_AOM(1), L3_3_WB),           \
	/* No AOM; Age:DC - LLC */                                                         \
	MOCS_ENTRY(14, LE_3_WB | LE_TC_1_LLC | LE_LRUM(2) | LE_AOM(1), L3_1_UC),           \
	/* No AOM; Age:DC - L3 + LLC */                                                    \
	MOCS_ENTRY(15, LE_3_WB | LE_TC_1_LLC | LE_LRUM(2) | LE_AOM(1), L3_3_WB),           \
	/* Bypass LLC - Uncached (EHL+) */                                                 \
	MOCS_ENTRY(16, LE_1_UC | LE_TC_1_LLC | LE_SCF(1), L3_1_UC),                        \
	/* Bypass LLC - L3 (Read-Only) (EHL+) */                                           \
	MOCS_ENTRY(17, LE_1_UC | LE_TC_1_LLC | LE_SCF(1), L3_3_WB),                        \
	/* Self-Snoop - L3 + LLC */                                                        \
	MOCS_ENTRY(18, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SSE(3), L3_3_WB),           \
	/* Skip Caching - L3 + LLC(12.5%) */                                               \
	MOCS_ENTRY(19, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SCC(7), L3_3_WB),           \
	/* Skip Caching - L3 + LLC(25%) */                                                 \
	MOCS_ENTRY(20, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SCC(3), L3_3_WB),           \
	/* Skip Caching - L3 + LLC(50%) */                                                 \
	MOCS_ENTRY(21, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_SCC(1), L3_3_WB),           \
	/* Skip Caching - L3 + LLC(75%) */                                                 \
	MOCS_ENTRY(22, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_RSC(1) | LE_SCC(3), L3_3_WB), \
	/* Skip Caching - L3 + LLC(87.5%) */                                               \
	MOCS_ENTRY(23, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3) | LE_RSC(1) | LE_SCC(7), L3_3_WB), \
	/* HW Reserved - SW program but never use */                                       \
	MOCS_ENTRY(62, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC),                       \
	/* HW Reserved - SW program but never use */                                       \
	MOCS_ENTRY(63, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC)

/* Tiger Lake and Rocket Lake: fixed by contract.  Reserved and
 * unspecified entries get entry 1's values. */
static const struct mocs12_entry tgl_mocs_table[] = {
	MOCS_ENTRY(I915_MOCS_PTE, LE_0_PAGETABLE | LE_TC_0_PAGETABLE, L3_1_UC),
	GEN11_MOCS_ENTRIES,
	/* Implicitly enable L1 - HDC:L1 + L3 + LLC */
	MOCS_ENTRY(48, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_3_WB),
	/* Implicitly enable L1 - HDC:L1 + L3 */
	MOCS_ENTRY(49, LE_1_UC | LE_TC_1_LLC, L3_3_WB),
	/* Implicitly enable L1 - HDC:L1 + LLC */
	MOCS_ENTRY(50, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC),
	/* Implicitly enable L1 - HDC:L1 */
	MOCS_ENTRY(51, LE_1_UC | LE_TC_1_LLC, L3_1_UC),
	/* HW Special Case (CCS) */
	MOCS_ENTRY(60, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC),
	/* HW Special Case (Displayable) */
	MOCS_ENTRY(61, LE_1_UC | LE_TC_1_LLC, L3_3_WB),
};

/* Alder Lake and the other integrated Gen12 parts */
static const struct mocs12_entry gen12_mocs_table[] = {
	GEN11_MOCS_ENTRIES,
	/* Implicitly enable L1 - HDC:L1 + L3 + LLC */
	MOCS_ENTRY(48, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_3_WB),
	/* Implicitly enable L1 - HDC:L1 + L3 */
	MOCS_ENTRY(49, LE_1_UC | LE_TC_1_LLC, L3_3_WB),
	/* Implicitly enable L1 - HDC:L1 + LLC */
	MOCS_ENTRY(50, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC),
	/* Implicitly enable L1 - HDC:L1 */
	MOCS_ENTRY(51, LE_1_UC | LE_TC_1_LLC, L3_1_UC),
	/* HW Special Case (CCS) */
	MOCS_ENTRY(60, LE_3_WB | LE_TC_1_LLC | LE_LRUM(3), L3_1_UC),
	/* HW Special Case (Displayable) */
	MOCS_ENTRY(61, LE_1_UC | LE_TC_1_LLC, L3_3_WB),
};

/* DG1: the L3 half only */
static const struct mocs12_entry dg1_mocs_table[] = {
	/* UC */
	MOCS_ENTRY(1, 0, L3_1_UC),
	/* WB - L3 */
	MOCS_ENTRY(5, 0, L3_3_WB),
	/* WB - L3 50% */
	MOCS_ENTRY(6, 0, L3_ESC(1) | L3_SCC(1) | L3_3_WB),
	/* WB - L3 25% */
	MOCS_ENTRY(7, 0, L3_ESC(1) | L3_SCC(3) | L3_3_WB),
	/* WB - L3 12.5% */
	MOCS_ENTRY(8, 0, L3_ESC(1) | L3_SCC(7) | L3_3_WB),
	/* HDC:L1 + L3 */
	MOCS_ENTRY(48, 0, L3_3_WB),
	/* HDC:L1 */
	MOCS_ENTRY(49, 0, L3_1_UC),
	/* HW Reserved */
	MOCS_ENTRY(60, 0, L3_1_UC),
	MOCS_ENTRY(61, 0, L3_1_UC),
	MOCS_ENTRY(62, 0, L3_1_UC),
	MOCS_ENTRY(63, 0, L3_1_UC),
};

/* DG2: the L3 half only */
static const struct mocs12_entry dg2_mocs_table[] = {
	/* UC - Coherent; GO:L3 */
	MOCS_ENTRY(0, 0, L3_1_UC | L3_LKUP(1)),
	/* UC - Coherent; GO:Memory */
	MOCS_ENTRY(1, 0, L3_1_UC | L3_GLBGO(1) | L3_LKUP(1)),
	/* UC - Non-Coherent; GO:Memory */
	MOCS_ENTRY(2, 0, L3_1_UC | L3_GLBGO(1)),
	/* WB - LC */
	MOCS_ENTRY(3, 0, L3_3_WB | L3_LKUP(1)),
};

/* Meteor Lake: sixteen entries naming page attribute table entries */
static const struct mocs12_entry mtl_mocs_table[] = {
	/* Error - Reserved for Non-Use */
	MOCS_ENTRY(0, IG_PAT(0), L3_LKUP(1) | L3_3_WB),
	/* Cached - L3 + L4 */
	MOCS_ENTRY(1, IG_PAT(1), L3_LKUP(1) | L3_3_WB),
	/* L4 - GO:L3 */
	MOCS_ENTRY(2, IG_PAT(1), L3_LKUP(1) | L3_1_UC),
	/* Uncached - GO:L3 */
	MOCS_ENTRY(3, IG_PAT(1) | L4_3_UC, L3_LKUP(1) | L3_1_UC),
	/* L4 - GO:Mem */
	MOCS_ENTRY(4, IG_PAT(1), L3_LKUP(1) | L3_GLBGO(1) | L3_1_UC),
	/* Uncached - GO:Mem */
	MOCS_ENTRY(5, IG_PAT(1) | L4_3_UC, L3_LKUP(1) | L3_GLBGO(1) | L3_1_UC),
	/* L4 - L3:NoLKUP; GO:L3 */
	MOCS_ENTRY(6, IG_PAT(1), L3_1_UC),
	/* Uncached - L3:NoLKUP; GO:L3 */
	MOCS_ENTRY(7, IG_PAT(1) | L4_3_UC, L3_1_UC),
	/* L4 - L3:NoLKUP; GO:Mem */
	MOCS_ENTRY(8, IG_PAT(1), L3_GLBGO(1) | L3_1_UC),
	/* Uncached - L3:NoLKUP; GO:Mem */
	MOCS_ENTRY(9, IG_PAT(1) | L4_3_UC, L3_GLBGO(1) | L3_1_UC),
	/* Display - L3; L4:WT */
	MOCS_ENTRY(14, IG_PAT(1) | L4_1_WT, L3_LKUP(1) | L3_3_WB),
	/* CCS - Non-Displayable */
	MOCS_ENTRY(15, IG_PAT(1), L3_GLBGO(1) | L3_1_UC),
};

/* Xe2 and Xe3: sixteen entries, the L3 and L4 policies in the global
 * table itself and no L3 half; entry 0 defers to the page attribute
 * table */
#define XE2_L3_0_WB (0u << 4)
#define XE2_L3_3_UC (3u << 4)
#define L4_0_WB L4_CACHEABILITY(0)
#define XE2_NUM_MOCS_ENTRIES 16

static const struct mocs12_entry xe2_mocs_table[] = {
	/* Defer to PAT */
	MOCS_ENTRY(0, XE2_L3_0_WB | L4_3_UC, 0),
	/* Cached L3, Uncached L4 */
	MOCS_ENTRY(1, IG_PAT(1) | XE2_L3_0_WB | L4_3_UC, 0),
	/* Uncached L3, Cached L4 */
	MOCS_ENTRY(2, IG_PAT(1) | XE2_L3_3_UC | L4_0_WB, 0),
	/* Uncached L3 + L4 */
	MOCS_ENTRY(3, IG_PAT(1) | XE2_L3_3_UC | L4_3_UC, 0),
	/* Cached L3 + L4 */
	MOCS_ENTRY(4, IG_PAT(1) | XE2_L3_0_WB | L4_0_WB, 0),
};

struct mocs12_table {
	const struct mocs12_entry *table;
	unsigned size, n_entries;
	uint8_t uc_index, wb_index, unused;
	int control; /* the per-engine half is written (not on discrete parts) */
	int l3cc; /* the L3 half exists (not from Xe2 on) */
};

#define MOCS_TABLE(t) .table = (t), .size = sizeof(t) / sizeof((t)[0])

static int mocs12_settings(const struct i915_device *i915, struct mocs12_table *t)
{
	unsigned ip = i915->gt_ip;
	int platform = i915->info->platform;

	if (ip >= I915_IP(20, 0)) {
		*t = (struct mocs12_table){ MOCS_TABLE(xe2_mocs_table), .n_entries = XE2_NUM_MOCS_ENTRIES,
					    .uc_index = 3, .wb_index = 4, .unused = 4 };
		t->control = 1; /* discrete or not */
		t->l3cc = 0;
		return 1;
	}
	if (ip >= I915_IP(12, 70) && ip <= I915_IP(12, 74)) {
		*t = (struct mocs12_table){ MOCS_TABLE(mtl_mocs_table), .n_entries = MTL_NUM_MOCS_ENTRIES,
					    .uc_index = 9, .unused = 1 };
	} else if (platform == I915_PLATFORM_DG2) {
		*t = (struct mocs12_table){ MOCS_TABLE(dg2_mocs_table), .n_entries = GEN9_NUM_MOCS_ENTRIES,
					    .uc_index = 1, .unused = 3 };
	} else if (platform == I915_PLATFORM_DG1) {
		*t = (struct mocs12_table){ MOCS_TABLE(dg1_mocs_table), .n_entries = GEN9_NUM_MOCS_ENTRIES,
					    .uc_index = 1, .unused = 5 };
	} else if (platform == I915_PLATFORM_TIGERLAKE || platform == I915_PLATFORM_ROCKETLAKE) {
		*t = (struct mocs12_table){ MOCS_TABLE(tgl_mocs_table), .n_entries = GEN9_NUM_MOCS_ENTRIES,
					    .uc_index = 3, .unused = I915_MOCS_PTE };
	} else if (ip >= I915_IP(12, 0)) {
		*t = (struct mocs12_table){ MOCS_TABLE(gen12_mocs_table), .n_entries = GEN9_NUM_MOCS_ENTRIES,
					    .uc_index = 3, .unused = 2 };
	} else {
		return 0;
	}
	t->control = !(i915->info->flags & I915_INFO_IS_DGFX);
	t->l3cc = 1;
	return 1;
}

/* An entry the table leaves unspecified takes the values of the entry
 * the platform names for it. */
static uint32_t mocs12_control(const struct mocs12_table *t, unsigned i)
{
	if (i < t->size && t->table[i].used)
		return t->table[i].control;
	return t->table[t->unused].control;
}

static uint16_t mocs12_l3(const struct mocs12_table *t, unsigned i)
{
	if (i < t->size && t->table[i].used)
		return t->table[i].l3;
	return t->table[t->unused].l3;
}

/* The L3 half, two entries a register.  Xe_HP on, the registers have a
 * copy per L3 bank and are written to all of them. */
static void mocs12_init_l3cc(struct i915_device *i915, const struct mocs12_table *t, uint32_t gsi)
{
	for (unsigned i = 0; i < (t->n_entries + 1) / 2; i++) {
		uint32_t v = (uint32_t)mocs12_l3(t, 2 * i) | ((uint32_t)mocs12_l3(t, 2 * i + 1) << 16);
		if (i915->gt_ip >= I915_IP(12, 55))
			i915_mcr_write(i915, gsi + GEN9_LNCFCMOCS(i), v);
		else
			i915_write32(i915, gsi + GEN9_LNCFCMOCS(i), v);
	}
}

/* Gen12 on: the global table and the L3 half, for the primary GT and the
 * media GT, after every GT reset.  The L3 half is written early: the
 * GuC's own memory traffic goes through it too. */
void i915_mocs_init(struct i915_device *i915)
{
	struct mocs12_table t;

	if (!mocs12_settings(i915, &t))
		return;
	for (int media = 0; media <= (i915->has_media_gt ? 1 : 0); media++) {
		uint32_t gsi = media ? I915_MEDIA_GT_BASE : 0;
		/* Xe2 on: the primary GT's global table has a copy per unit
		 * and is written to all of them; the media GT's has one */
		int mcr = !media && i915->gt_ip >= I915_IP(20, 0);
		if (t.control)
			for (unsigned i = 0; i < t.n_entries; i++) {
				if (mcr)
					i915_mcr_write(i915, GEN12_GLOBAL_MOCS(i), mocs12_control(&t, i));
				else
					i915_write32(i915, gsi + GEN12_GLOBAL_MOCS(i),
						     mocs12_control(&t, i));
			}
		if (t.l3cc)
			mocs12_init_l3cc(i915, &t, gsi);
	}
}

static void mocs12_render_l3cc(struct i915_device *i915)
{
	struct mocs12_table t;

	if (mocs12_settings(i915, &t) && t.l3cc)
		mocs12_init_l3cc(i915, &t, 0);
}

unsigned i915_mocs_uc_index(struct i915_device *i915)
{
	struct mocs12_table t;
	return mocs12_settings(i915, &t) ? t.uc_index : 0;
}

unsigned i915_mocs_wb_index(struct i915_device *i915)
{
	struct mocs12_table t;
	return mocs12_settings(i915, &t) ? t.wb_index : 0;
}
