// LikeOS -- shared state of the pre-Broadwell GT backend.
//
// The i915_legacy_*.c files keep what the Gen8 driver keeps in places a
// ring-buffer part does not have (the PPGTT directory in the global GTT,
// one ring per engine, the fence registers' real geometry, the GT FIFO
// count) in one structure here.  There is one graphics device per
// system, so there is one of these.  Nothing outside the legacy files
// includes this header; they talk to the rest of the driver through
// i915_legacy.h.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_LEGACY_PRIV_H
#define KERNEL_DEV_GPU_I915_LEGACY_PRIV_H

#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/dev/gpu/i915/i915_legacy_reg.h>

/* How the global GTT's entries are written. */
enum leg_pte_kind {
	LEG_PTE_I830, /* gen2, i915G/GM, i945G/GM: 32-bit physical */
	LEG_PTE_I965, /* G33, Pineview, gen4, Ironlake: bits 35:32 in 7:4 */
	LEG_PTE_SNB,
	LEG_PTE_IVB,
	LEG_PTE_BYT,
	LEG_PTE_HSW,
	LEG_PTE_IRIS, /* Haswell with eDRAM */
};

/* Cache attribute of a binding: the four levels the interface knows. */
enum leg_cache {
	LEG_CACHE_NONE = 0,
	LEG_CACHE_LLC = 1, /* snooped where the part has no LLC */
	LEG_CACHE_L3_LLC = 2,
	LEG_CACHE_WT = 3,
};

/* One engine's ring and the bookkeeping of what it has loaded.  The
 * pseudo logical ring `rq_lrc' is what requests point at: its ring is
 * the engine's, so the engine code's retirement moves the ring head as
 * requests complete. */
struct leg_engine {
	struct i915_lrc rq_lrc;
	uint32_t effective_size; /* i830/845 may not put the tail in the last 128 bytes */
	uint32_t irq_enable_mask; /* in the GT (or gen2-4 IIR, PM) register */
	uint32_t context_size; /* render context image, 0 = none */
	uint32_t reset_domain; /* Gen6/7 GDRST bit */
	uint64_t hws_phys; /* physical status page (gen2-4 without G33) */
	int hws_physical;
	int resumed;
};

/* A fence register as the hardware holds it. */
struct leg_fence {
	uint32_t start, size, stride, tiling;
	int pin; /* scanout and GTT mappings: may not be stolen */
	int map_pinned; /* one of the pins is the holder's aperture mapping */
	uint64_t lru;
};

/* The Gen6/7 PPGTT: one directory of 512 entries, kept in the global
 * GTT's own entry array (a 2 MB range of the global space is given up
 * for it), each entry naming a page table of 1024 entries. */
struct leg_ppgtt {
	int present;
	uint32_t pd_ggtt; /* where the directory's range starts */
	uint32_t pp_dir; /* what PP_DIR_BASE takes */
	uint64_t pt_phys[GEN6_PDES]; /* 0 = the scratch table */
	uint64_t scratch_pt;
	uint32_t scratch_pte;
};

struct i915_legacy_state {
	struct i915_device *i915;
	/* platform facts the descriptor does not carry */
	int mobile;
	int is_g4x;
	int is_g33; /* G33 and Pineview */
	int has_edram;
	int has_snoop;
	int hws_physical;
	uint64_t dma_limit; /* the highest physical address a GTT entry can name */

	/* the GTT */
	enum leg_pte_kind pte_kind;
	uint64_t gtt_phys;
	uint32_t gtt_entries;
	uint64_t mappable_end;
	uint32_t pgetbl_save;
	uint64_t scratch_phys;
	uint32_t scratch_pte;
	spinlock_t gtt_lock;
	/* pages of the global space bound by the shared address space
	 * (gen2-5: the global GTT is the client's space) */
	uint8_t *vm_owned;
	struct i915_vm *vm; /* the one address space every context shares */
	struct leg_ppgtt ppgtt;
	/* the chipset write-buffer flush page (gen3/4), when the firmware
	 * set one up */
	volatile uint32_t *ifp_page;
	int dma_warned;

	/* the GT scratch area (gen2: 256 KB for the CS TLB workaround) */
	struct drm_gem_object *scratch_obj;
	uint32_t scratch_ggtt;

	/* fences */
	struct leg_fence fences[32];
	uint64_t fence_clock;
	uint32_t swizzle_x, swizzle_y;
	int swizzle_unknown;

	/* engines */
	struct leg_engine eng[I915_NUM_ENGINES];
	uint8_t *golden; /* the render context image every context starts from */
	uint32_t golden_size;
	int gpu_reset_clobbers_display;

	/* interrupts */
	uint32_t gen2_imr; /* gen2-4 / VLV display IMR the GT side owns */
	uint32_t gt_imr, pm_imr;
	int irq_gsi;

	/* uncore */
	uint32_t fifo_count;
	int has_fifo;
	int has_fpga_dbg;

	/* power */
	uint8_t rp0, rp1, rpn, rpe, max_freq, min_freq, cur_freq;
	int rps_enabled, rps_dynamic;
	int last_adj;
	int power_mode;
	uint32_t pm_events;
	uint32_t rc6_ctl;
	spinlock_t sb_lock; /* the PCU mailbox (the IOSF sideband has its own) */
};

extern struct i915_legacy_state g_leg;

/* ---- helpers shared by the legacy files -------------------------------- */

static inline int leg_gen(const struct i915_device *i915)
{
	return i915->info->gen;
}

static inline int leg_is(const struct i915_device *i915, int platform)
{
	return i915->info->platform == platform;
}

/* 16-bit accesses (gen2's HWSTAM, Ironlake's MEMSWCTL): no forcewake on
 * the parts that have them. */
static inline uint16_t leg_read16(struct i915_device *i915, uint32_t reg)
{
	return *(volatile uint16_t *)(i915->mmio_virt + reg);
}

static inline void leg_write16(struct i915_device *i915, uint32_t reg, uint16_t v)
{
	*(volatile uint16_t *)(i915->mmio_virt + reg) = v;
}

static inline void leg_rmw(struct i915_device *i915, uint32_t reg, uint32_t clr, uint32_t set)
{
	uint32_t v = i915_read32(i915, reg);
	i915_write32(i915, reg, (v & ~clr) | set);
}

static inline void leg_clflush_range(const void *p, uint32_t bytes)
{
	const uint8_t *b = (const uint8_t *)((uintptr_t)p & ~63ULL);
	const uint8_t *e = (const uint8_t *)p + bytes;
	for (; b < e; b += 64)
		__asm__ volatile("clflush (%0)" ::"r"(b) : "memory");
	__asm__ volatile("mfence" ::: "memory");
}

/* i915_legacy_gtt.c */
uint32_t leg_pte_encode(struct i915_device *i915, uint64_t phys, enum leg_cache level, int readonly);
void leg_ggtt_invalidate(struct i915_device *i915);
void leg_ggtt_write(struct i915_device *i915, uint32_t index, uint32_t pte);
int leg_ggtt_alloc(struct i915_device *i915, uint32_t npages, uint32_t align_pages,
		   uint32_t lo, uint32_t hi, int top_down, uint32_t *first);
void leg_ggtt_free(struct i915_device *i915, uint32_t first, uint32_t npages);
enum leg_cache leg_cache_of(struct i915_device *i915, const struct drm_gem_object *o, int uncached);
void leg_ppgtt_rewrite_pd(struct i915_device *i915);
/* bind a driver object at a given cache level (contexts on IVB want L3) */
int leg_ggtt_bind_level(struct i915_device *i915, struct drm_gem_object *o, enum leg_cache level,
			int mappable, uint32_t *ggtt);

/* i915_legacy_fence.c */
void leg_detect_swizzle(struct i915_device *i915);

/* i915_legacy_wa.c */
void leg_engine_workarounds(struct i915_device *i915, struct i915_engine *e);
uint32_t leg_ctx_workarounds_emit(struct i915_device *i915, struct i915_engine *e,
				  uint32_t *cs, uint32_t max);
void leg_gt_workarounds(struct i915_device *i915);
void leg_init_swizzling(struct i915_device *i915);
void leg_init_unused_rings(struct i915_device *i915);
void leg_ppgtt_enable(struct i915_device *i915);
void leg_init_clock_gating(struct i915_device *i915);

/* i915_legacy_ring.c */
void leg_engine_signal(struct i915_engine *e);

/* i915_legacy_rps.c */
void leg_rps_irq(struct i915_device *i915, uint32_t pm_iir);

/* i915_legacy_uncore.c */
int leg_pcode_read(struct i915_device *i915, uint32_t mbox, uint32_t *val, uint32_t *val1);
int leg_pcode_write(struct i915_device *i915, uint32_t mbox, uint32_t val);

#endif
