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
// Copyright (C) 2026 The LikeOS Project

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
