// LikeOS -- device local memory of the discrete Intel parts.
//
// DG1 (Iris Xe MAX), DG2 (Arc A-series) and Battlemage (Arc B-series)
// carry their own memory.  The GPU reaches it by device physical address
// (an offset into the card's memory) through page table entries marked
// "local"; the processor reaches the part of it that the second PCI BAR
// covers, which is all of it with a resizable BAR and only the first
// 256 MB without.  The top of that memory belongs to the card's
// firmware: the GTT itself (GSM), the data stolen memory (DSM; on
// Battlemage with the WOPCM at its top), and on DG2 and Battlemage the
// flat compression metadata (CCS) that shadows every page (on Battlemage
// unless its firmware switched compression off).
//
// An object backed by local memory is an ordinary drm_gem_object whose
// pages[] entries are device physical addresses with I915_LMEM_PAGE_TAG
// set, one entry per 4 KB page, so every walk over pages[] keeps working
// and only the places that turn an entry into a CPU address or a page
// table entry need to look at the tag.  On DG2 and Battlemage local
// memory is handed out in 64 KB chunks, 64 KB aligned, and must be
// mapped 64 KB at a time in the graphics address spaces (the PS64 bit of
// a group of sixteen entries); DG1 hands it out a page at a time.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2019-2025 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_LMEM_H
#define KERNEL_DEV_GPU_I915_LMEM_H

#include <kernel/uapi/types.h>
#include <kernel/dev/gpu/drm.h>

struct i915_device;
struct drm_i915_memory_region_info;
struct drm_i915_gem_create_ext_memory_regions;

/* ---- the page representation ---------------------------------------------------- */

/* A pages[] entry naming a page of local memory: the device physical
 * address with this bit.  Bit 63 is above any address a page table or a
 * processor takes, so code that masks an entry to an address drops it. */
#define I915_LMEM_PAGE_TAG (1ULL << 63)

static inline int i915_lmem_page(uint64_t entry)
{
	return (entry & I915_LMEM_PAGE_TAG) != 0;
}

static inline uint64_t i915_lmem_page_dpa(uint64_t entry)
{
	return entry & ~I915_LMEM_PAGE_TAG;
}

/* An object's pages are all of one kind; the first says which. */
static inline int i915_lmem_object(const struct drm_gem_object *o)
{
	return o && o->pages && o->npages && i915_lmem_page(o->pages[0]);
}

/* ---- page table bits ------------------------------------------------------------- */

/* The "local memory" bit of a global GTT entry and of a per-process
 * entry (Gen12 discrete), and DG2's 64 KB page hint: set on each of
 * sixteen consecutive entries that map one 64 KB aligned, contiguous
 * 64 KB of local memory at a 64 KB aligned address. */
#define I915_LMEM_GGTT_PTE_LM (1ULL << 1)
#define I915_LMEM_PPGTT_PTE_LM (1ULL << 11)
#define I915_LMEM_PTE_PS64 (1ULL << 8)

/* ---- probing ---------------------------------------------------------------------- */

/* Once at driver load, after the register window, forcewake, the MCR
 * steering and the GTT probe (i915_gtt_probe()) and before anything
 * binds, maps or creates an object.  On a discrete part: checks that the
 * firmware trained the memory, sizes it, takes BAR2 over as the local
 * memory window (bar_lmem; bar_aperture is cleared -- these parts have
 * no graphics aperture, so no fences and no GTT mmaps), points
 * stolen_base/stolen_size/stolen_io at the data stolen memory in local
 * memory, reserves what the firmware's framebuffer occupies and starts
 * the allocator.  Returns 0 (also on an integrated part, where it does
 * nothing), or a negative errno: the device cannot be driven. */
int i915_lmem_probe(struct i915_device *i915);
void i915_lmem_fini(struct i915_device *i915);

/* The part has local memory and it is up. */
int i915_lmem_present(const struct i915_device *i915);
/* Local memory pages are 64 KB (DG2) rather than 4 KB (DG1). */
int i915_lmem_has_64k_pages(const struct i915_device *i915);
/* The allocation unit: 64 KB on DG2, 4 KB on DG1. */
uint32_t i915_lmem_min_page_size(const struct i915_device *i915);

/* Sizes in bytes: the memory objects may use, the part of it the
 * processor can reach, and what is free of each. */
uint64_t i915_lmem_size(const struct i915_device *i915);
uint64_t i915_lmem_cpu_visible_size(const struct i915_device *i915);
uint64_t i915_lmem_avail(const struct i915_device *i915);
uint64_t i915_lmem_cpu_visible_avail(const struct i915_device *i915);

/* ---- backing storage --------------------------------------------------------------- */

#define I915_LMEM_ALLOC_CONTIGUOUS (1u << 0) /* one run of device addresses */
#define I915_LMEM_ALLOC_CPU_ACCESS (1u << 1) /* within the processor's window */
#define I915_LMEM_ALLOC_GPU_ONLY (1u << 2) /* prefer what the processor cannot see */
#define I915_LMEM_ALLOC_NO_CLEAR (1u << 3) /* the caller overwrites all of it */

/* Local memory for an object that has no pages yet (o->npages set by
 * drm_gem_alloc()): o->pages is allocated and filled with tagged device
 * addresses, and the memory is cleared (by the processor through BAR2,
 * or by the registered GPU clear for memory the processor cannot see).
 * 0, -ENOSPC when no placement the flags allow has room, -ENOMEM. */
int i915_lmem_alloc_pages(struct i915_device *i915, struct drm_gem_object *o, unsigned flags);
/* Give an object's local memory back (gem_release_pages calls this for
 * a local object; the pages array itself stays the core's to free). */
void i915_lmem_free_pages(struct i915_device *i915, struct drm_gem_object *o);
/* Move an object's system-memory pages into local memory, contents
 * included.  Only for an object nothing maps or binds yet (a buffer the
 * core just created for scanout); 0 or a negative errno, the object
 * unchanged on failure. */
int i915_lmem_migrate(struct i915_device *i915, struct drm_gem_object *o, unsigned flags);

/* For memory the processor cannot reach: a synchronous GPU fill of the
 * object with zeroes (a blitter fill and a wait), supplied by the GT
 * code once it can submit.  Until one is registered, objects are placed
 * only where the processor can clear them. */
typedef int (*i915_lmem_clear_fn)(struct i915_device *i915, struct drm_gem_object *o);
void i915_lmem_set_clear_hook(i915_lmem_clear_fn fn);

/* An object for a GEM_CREATE_EXT: `pl' the placements in order of
 * preference (from i915_lmem_parse_regions(); NULL means system memory),
 * `cpu_access' the NEEDS_CPU_ACCESS flag.  The size is rounded up to the
 * largest page size among the placements.  Returns the object with one
 * reference and its driver state made (gem_init), or NULL with *err set. */
struct i915_lmem_placement {
	uint32_t n;
	uint8_t region[4]; /* I915_MEMORY_CLASS_SYSTEM / _DEVICE, instance 0 */
	uint32_t mask; /* bit per class */
};
int i915_lmem_parse_regions(struct i915_device *i915,
			    const struct drm_i915_gem_create_ext_memory_regions *mr,
			    struct i915_lmem_placement *pl);
struct drm_gem_object *i915_lmem_object_create(struct i915_device *i915, uint64_t size,
					       const struct i915_lmem_placement *pl,
					       int cpu_access, int *err);

/* ---- processor access ----------------------------------------------------------------- */

/* The processor's (BAR2) address of a local page entry, for a user
 * mapping; 0 when the processor cannot reach it.  For a system page the
 * entry itself. */
uint64_t i915_lmem_page_cpu_phys(const struct i915_device *i915, uint64_t entry);
/* gem_page_phys for any object: system pages as they are, local pages
 * through BAR2, 0 for a page out of range or out of reach. */
uint64_t i915_lmem_gem_page_phys(struct drm_gem_object *o, uint64_t index);
/* The mapping kind a FIXED mmap gets: write-combining for a local
 * object, write-back for a system one (the card snoops system memory). */
unsigned i915_lmem_mmap_kind(const struct drm_gem_object *o);
/* Copies between an object (any kind) and kernel memory, and between an
 * object and a client's memory (pread/pwrite).  0 or a negative errno
 * (-EFAULT, -ENXIO for local memory the processor cannot reach). */
int i915_lmem_object_rw(struct i915_device *i915, struct drm_gem_object *o, uint64_t offset,
			void *buf, uint64_t len, int write);
int i915_lmem_object_rw_user(struct i915_device *i915, struct drm_gem_object *o,
			     uint64_t offset, uint64_t uptr, uint64_t len, int write);

/* ---- graphics address spaces ----------------------------------------------------------- */

/* Extra bits for the global GTT entry of pages[] entry `entry'. */
uint64_t i915_lmem_ggtt_pte_bits(const struct i915_device *i915, uint64_t entry);
/* Extra bits for the per-process entry of pages[idx] of a binding of
 * `n' pages, the entry mapping graphics address `va': the local bit,
 * and on DG2 the 64 KB hint where the whole aligned group qualifies. */
uint64_t i915_lmem_ppgtt_pte_bits(const struct i915_device *i915, const uint64_t *pages,
				  uint32_t idx, uint32_t n, uint64_t va);
/* The alignment, in 4 KB pages, a binding of the object needs in any
 * graphics address space: 16 for a local object on DG2, else 1. */
uint32_t i915_lmem_gtt_align_pages(const struct i915_device *i915, const struct drm_gem_object *o);

/* ---- the uAPI ---------------------------------------------------------------------------- */

/* The regions for DRM_I915_QUERY_MEMORY_REGIONS: system memory, then
 * local memory where there is some.  Fills up to `max' entries and
 * returns how many there are. */
int i915_lmem_query_regions(struct i915_device *i915, struct drm_i915_memory_region_info *out,
			    int max);

/* ---- scanout ------------------------------------------------------------------------------ */

/* The display of a discrete part reads only local memory.  For an
 * object in system memory (the console's framebuffer, a client's
 * system-memory buffer) a copy in local memory is scanned out instead:
 * made on first use, brought up to date on every call, bound in the
 * global GTT; *ggtt gets its address.  For a local object nothing is
 * done and 1 is returned (bind the object itself); 0 when the copy is
 * what to scan out; a negative errno. */
int i915_lmem_scanout_shadow(struct i915_device *i915, struct drm_gem_object *o, uint32_t *ggtt);
/* The client wrote these rectangles of `fb' (DIRTYFB, the console):
 * copied into the scanout copy if the object has one.  Returns 1 when it
 * had one (nothing else to do), 0 otherwise. */
int i915_lmem_scanout_dirty(struct i915_device *i915, struct drm_framebuffer *fb,
			    const struct drm_mode_rect_k *rects, uint32_t n);
/* The object goes away: its scanout copy with it. */
void i915_lmem_object_free(struct i915_device *i915, struct drm_gem_object *o);

/* ---- power ---------------------------------------------------------------------------- */

/* The card's memory does not survive the card losing power: every local
 * object's contents are copied to system memory before a suspend and
 * back after the resume (before the GTT is rewritten). */
int i915_lmem_suspend(struct i915_device *i915);
void i915_lmem_resume(struct i915_device *i915);

#endif
