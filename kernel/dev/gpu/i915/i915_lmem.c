// LikeOS -- device local memory of the discrete Intel parts (DG1, DG2,
// Battlemage).
//
// A discrete card has its own memory, trained by the card's firmware
// before the driver runs (which says so in a bit of the graphics unit's
// control word).  The GPU addresses it by device physical address, an
// offset from zero; page table entries carry a "local" bit that says the
// address is one of those rather than a system address.  The processor
// sees it through BAR2: all of it when the platform gave the card a
// large enough BAR, else only the first 256 MB ("small BAR").
//
// Not all of it is the driver's.  At the top the firmware keeps the
// global GTT (GSM), the data stolen memory (DSM, which the display can
// use and where nothing of ours goes) and, on DG2, the flat compression
// metadata (one byte of CCS per 256 bytes of memory, behind every page).
// Battlemage (Xe2) sizes its tile in a register of its own, may have the
// flat CCS switched off by its firmware, gives the CCS base in a pair of
// registers in the hardware's per-L3-node view (scaled by the enabled
// nodes to an address), and keeps the WOPCM at the top of the data
// stolen memory, which is therefore not all the display's.
// What remains is handed out by a bitmap allocator, in 64 KB chunks on
// DG2 and Battlemage -- the unit their page tables want local memory
// mapped in -- and in
// 4 KB pages on DG1, whose first megabyte is left alone for the
// firmware's legacy use.  An object's pages[] are device addresses with
// I915_LMEM_PAGE_TAG set, so the rest of the driver keeps treating them
// as an array of pages; only CPU access and page table entries look at
// the tag.
//
// The processor reaches local memory through a one-page write-combining
// window onto BAR2 that is moved from page to page under a lock (a
// window that lies in the direct map is used as it is).  A new object is
// cleared through it; memory beyond the processor's reach is used only
// once the GT code has registered a GPU clear for it.
//
// The display reads only local memory, so an object in system memory
// that is to be scanned out is shown through a copy in local memory,
// kept up to date at every bind and on every dirty rectangle.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/uapi/drm/i915_drm.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>

/* ---- registers --------------------------------------------------------------------- */

/* The graphics unit's control word: the firmware sets LMEM_INIT once the
 * card's memory is trained and healthy.  If training failed the GT is
 * kept powered down and nothing on the card answers. */
#define GU_CNTL 0x101010
#define GU_CNTL_LMEM_INIT (1u << 7)

/* DG2: the tile's local memory range (bits 15:8, in GB) and where the
 * flat CCS begins (bits 31:8, in 64 KB units).  Registers with a copy per
 * memory slice: read through the steering. */
#define XEHP_TILE0_ADDR_RANGE 0x4900
#define XEHP_TILE_LMEM_RANGE_SHIFT 8
#define XEHP_FLAT_CCS_BASE_ADDR 0x4910
#define XEHP_CCS_BASE_SHIFT 8

/* Xe2 discrete (Battlemage): the tile's memory as bits 17:8 (size) and
 * 7:1 (offset) in GB; the flat CCS's base as the low register's bits
 * 31:6 (address bits 31:6) with its enable in bit 0 and the high one's
 * bits 7:0 (address bits 39:32), both in the hardware's view of one L3
 * node, multiplied by the enabled nodes (MIRROR_FUSE3 bits 31:16) to
 * give the address; and the size of the WOPCM at the top of the data
 * stolen memory (STOLEN_RESERVED bits 9:7). */
#define SG_TILE_ADDR_RANGE(tile) (0x1083a0 + (tile) * 4)
#define SG_TILE_SIZE_GB(v) (((v) >> 8) & 0x3ff)
#define SG_TILE_OFFSET_GB(v) (((v) >> 1) & 0x7f)
#define XE2_FLAT_CCS_BASE_RANGE_LOWER 0x8800
#define XE2_FLAT_CCS_ENABLE (1u << 0)
#define XE2_FLAT_CCS_BASE_LOWER_ADDR_MASK 0xffffffc0u
#define XE2_FLAT_CCS_BASE_RANGE_UPPER 0x8804
#define XE2_FLAT_CCS_BASE_UPPER_ADDR_MASK 0xffu
#define MIRROR_FUSE3 0x9118
#define XE2_NODE_ENABLE_MASK 0xffff0000u
#define STOLEN_RESERVED 0x1082c0
#define WOPCM_SIZE_FIELD(v) (((v) >> 7) & 0x7)

/* Base of the GTT stolen memory and of the data stolen memory, as
 * device addresses, 64-bit (also in i915_reg.h). */
#ifndef GEN6_GSMBASE
#define GEN6_GSMBASE 0x108100
#endif
#ifndef GEN6_DSMBASE
#define GEN6_DSMBASE 0x1080c0
#endif
#define LMEM_BDSM_MASK 0xfffffffffff00000ULL

#define SZ_64K (64u << 10)
#define SZ_1M (1u << 20)
#define SZ_1G (1ULL << 30)

/* GGTT entry: present, and the address bits. */
#define GGTT_PTE_PRESENT (1ULL << 0)
#define GGTT_PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/* The window's page attributes: write-combining (PAT entry 1). */
#define WIN_FLAGS (PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL | PAGE_NO_EXECUTE | PAGE_WRITE_THROUGH)

/* ---- state --------------------------------------------------------------------------- */

struct lmem {
	int present;
	int has_64k;
	uint32_t chunk; /* bytes per allocation unit */
	uint32_t chunk_pages;
	uint64_t size; /* bytes objects may use, from device address 0 */
	uint64_t io_base, io_size; /* BAR2 */
	uint64_t visible; /* bytes of `size' the processor reaches */
	uint32_t nchunks, visible_chunks;
	uint64_t *bitmap; /* a bit per chunk, set = in use */
	uint32_t free_chunks, free_visible_chunks;
	spinlock_t lock; /* the allocator */
	/* the processor's way in: a window page, or the direct map */
	spinlock_t win_lock;
	int linear;
	uint64_t win_va, win_pa;
	uint64_t lin_va;
	uint64_t dsm_base, dsm_size;
	i915_lmem_clear_fn clear;
	uint32_t reserved_chunks; /* the firmware's framebuffer, low memory */
};

static struct lmem lm;

/* The scanout copies of system-memory objects. */
struct shadow {
	struct shadow *next;
	struct drm_gem_object *obj; /* not referenced: removed when it is freed */
	struct drm_gem_object *copy; /* referenced */
	uint32_t ggtt;
};
static struct shadow *shadows;
static spinlock_t shadow_lock;

/* What a suspend saved. */
struct backup {
	struct backup *next;
	struct drm_gem_object *obj; /* referenced */
	uint64_t *pages; /* system pages, one per object page */
};
static struct backup *backups;

int i915_lmem_present(const struct i915_device *i915)
{
	(void)i915;
	return lm.present;
}

int i915_lmem_has_64k_pages(const struct i915_device *i915)
{
	(void)i915;
	return lm.present && lm.has_64k;
}

uint32_t i915_lmem_min_page_size(const struct i915_device *i915)
{
	(void)i915;
	return lm.present ? lm.chunk : 4096;
}

uint64_t i915_lmem_size(const struct i915_device *i915)
{
	(void)i915;
	return lm.present ? lm.size : 0;
}

uint64_t i915_lmem_cpu_visible_size(const struct i915_device *i915)
{
	(void)i915;
	return lm.present ? lm.visible : 0;
}

uint64_t i915_lmem_avail(const struct i915_device *i915)
{
	(void)i915;
	return lm.present ? (uint64_t)lm.free_chunks * lm.chunk : 0;
}

uint64_t i915_lmem_cpu_visible_avail(const struct i915_device *i915)
{
	(void)i915;
	return lm.present ? (uint64_t)lm.free_visible_chunks * lm.chunk : 0;
}

void i915_lmem_set_clear_hook(i915_lmem_clear_fn fn)
{
	lm.clear = fn;
}

/* ---- the processor's window ------------------------------------------------------------ */

/* The kernel address of local memory at device address `dpa', valid
 * while win_lock is held (the window moves).  NULL beyond the BAR. */
static uint8_t *win_map(uint64_t dpa)
{
	uint64_t pa;

	if (dpa >= lm.visible)
		return NULL;
	pa = lm.io_base + (dpa & ~0xfffULL);
	if (lm.linear)
		return (uint8_t *)lm.lin_va + (dpa & ~0xfffULL) + (dpa & 0xfff);
	if (pa != lm.win_pa) {
		if (!mm_map_page_no_shootdown(lm.win_va, pa, WIN_FLAGS))
			return NULL;
		lm.win_pa = pa;
	}
	/* Another processor may have moved the window since this one last
	 * looked through it: whatever this one cached of it is stale. */
	mm_flush_tlb(lm.win_va);
	return (uint8_t *)lm.win_va + (dpa & 0xfff);
}

static int window_init(void)
{
	uint64_t last = lm.io_base + lm.visible - 4096;
	uint64_t va = mm_map_mmio_flags(last, 1, MM_MMIO_WC);

	if (!va)
		return -ENOMEM;
	spinlock_init(&lm.win_lock, "i915_lmem_win");
	if (va == (uint64_t)phys_to_virt(last)) {
		/* BAR2 lies inside the direct map: the whole window is made
		 * write-combining there and addressed linearly. */
		lm.lin_va = mm_map_mmio_flags(lm.io_base, lm.visible / 4096, MM_MMIO_WC);
		if (!lm.lin_va)
			return -ENOMEM;
		lm.linear = 1;
		return 0;
	}
	lm.linear = 0;
	lm.win_va = va;
	lm.win_pa = last;
	return 0;
}

static void window_fini(void)
{
	if (lm.linear && lm.lin_va)
		mm_unmap_mmio(lm.lin_va, lm.visible / 4096);
	else if (lm.win_va)
		mm_unmap_mmio(lm.win_va, 1);
	lm.lin_va = 0;
	lm.win_va = 0;
	lm.linear = 0;
}

/* One piece of a page of local memory: read into, write from, or (buf
 * NULL on a write) zero.  0 or -ENXIO. */
static int lmem_page_io(uint64_t dpa, void *buf, uint32_t len, int write)
{
	uint64_t fl;
	uint8_t *va;

	spin_lock_irqsave(&lm.win_lock, &fl);
	va = win_map(dpa);
	if (!va) {
		spin_unlock_irqrestore(&lm.win_lock, fl);
		return -ENXIO;
	}
	if (!write)
		mm_memcpy(buf, va, len);
	else if (buf)
		mm_memcpy(va, buf, len);
	else
		mm_memset(va, 0, len);
	spin_unlock_irqrestore(&lm.win_lock, fl);
	return 0;
}

/* Write-combined stores are buffered: out to the card before the GPU
 * is told to look. */
static void wc_flush(void)
{
	__asm__ volatile("sfence" ::: "memory");
}

/* ---- the allocator ---------------------------------------------------------------------- */

static inline int bm_test(uint32_t c)
{
	return (lm.bitmap[c >> 6] >> (c & 63)) & 1;
}

static inline void bm_mark(uint32_t c)
{
	lm.bitmap[c >> 6] |= 1ULL << (c & 63);
	lm.free_chunks--;
	if (c < lm.visible_chunks)
		lm.free_visible_chunks--;
}

/* 1 when the chunk was in use (and is free now). */
static inline int bm_release(uint32_t c)
{
	if (!bm_test(c))
		return 0;
	lm.bitmap[c >> 6] &= ~(1ULL << (c & 63));
	lm.free_chunks++;
	if (c < lm.visible_chunks)
		lm.free_visible_chunks++;
	return 1;
}

/* The first free run of `n' chunks in [lo, hi): lowest first, or highest
 * first; -1 when there is none. */
static int64_t find_run(uint32_t lo, uint32_t hi, uint32_t n, int topdown)
{
	uint32_t run = 0;

	if (!n || hi <= lo || hi - lo < n)
		return -1;
	if (!topdown) {
		uint32_t start = lo;
		for (uint32_t c = lo; c < hi;) {
			if (!(c & 63) && c + 64 <= hi && lm.bitmap[c >> 6] == ~0ULL) {
				run = 0;
				c += 64;
				start = c;
				continue;
			}
			if (bm_test(c)) {
				run = 0;
				start = ++c;
				continue;
			}
			if (++run == n)
				return start;
			c++;
		}
		return -1;
	}
	for (uint32_t c = hi; c > lo;) {
		c--;
		if ((c & 63) == 63 && c >= lo + 63 && lm.bitmap[c >> 6] == ~0ULL) {
			run = 0;
			c -= 63;
			continue;
		}
		if (bm_test(c)) {
			run = 0;
			continue;
		}
		if (++run == n)
			return c;
	}
	return -1;
}

/* Any `n' free chunks in [lo, hi), in address order or from the top;
 * the count found (all of them, or fewer when the range is short). */
static uint32_t collect(uint32_t lo, uint32_t hi, uint32_t n, int topdown, uint32_t *out)
{
	uint32_t got = 0;

	if (hi <= lo)
		return 0;
	if (!topdown) {
		for (uint32_t c = lo; c < hi && got < n;) {
			if (!(c & 63) && c + 64 <= hi && lm.bitmap[c >> 6] == ~0ULL) {
				c += 64;
				continue;
			}
			if (!bm_test(c))
				out[got++] = c;
			c++;
		}
		return got;
	}
	for (uint32_t c = hi; c > lo && got < n;) {
		c--;
		if (!bm_test(c))
			out[got++] = c;
	}
	return got;
}

struct range {
	uint32_t lo, hi;
	int topdown;
};

/* Where an object may go, in order, by what it is for.  Memory the
 * processor cannot reach is used only when something can clear it. */
static int ranges_for(unsigned flags, struct range *r)
{
	uint32_t vis = lm.visible_chunks, tot = lm.nchunks;
	int hidden_ok = lm.clear != NULL && vis < tot;

	if ((flags & I915_LMEM_ALLOC_CPU_ACCESS) || !hidden_ok) {
		r[0] = (struct range){ 0, vis, 0 };
		return 1;
	}
	if (flags & I915_LMEM_ALLOC_GPU_ONLY) {
		/* keep out of the processor's window, which is precious on
		 * a small BAR */
		r[0] = (struct range){ vis, tot, 1 };
		r[1] = (struct range){ 0, vis, 1 };
		return 2;
	}
	r[0] = (struct range){ 0, tot, 0 };
	return 1;
}

/* `n' chunks into out[]; 0 or -ENOSPC.  Prefers one run. */
static int chunks_alloc(uint32_t n, unsigned flags, uint32_t *out)
{
	struct range r[2];
	int nr = ranges_for(flags, r);
	uint64_t fl;
	int rc = -ENOSPC;

	spin_lock_irqsave(&lm.lock, &fl);
	for (int i = 0; i < nr && rc; i++) {
		int64_t s = find_run(r[i].lo, r[i].hi, n, r[i].topdown);
		if (s >= 0) {
			for (uint32_t k = 0; k < n; k++)
				out[k] = (uint32_t)s + k;
			rc = 0;
		}
	}
	if (rc && !(flags & I915_LMEM_ALLOC_CONTIGUOUS)) {
		/* scattered: whatever is free, across the ranges in order */
		uint32_t got = 0;
		for (int i = 0; i < nr && got < n; i++) {
			uint32_t g = collect(r[i].lo, r[i].hi, n - got, r[i].topdown, out + got);
			/* the next range must not hand out these again */
			for (uint32_t k = 0; k < g; k++)
				bm_mark(out[got + k]);
			got += g;
		}
		if (got == n) {
			spin_unlock_irqrestore(&lm.lock, fl);
			return 0;
		}
		for (uint32_t k = 0; k < got; k++)
			bm_release(out[k]);
	}
	if (rc == 0)
		for (uint32_t k = 0; k < n; k++)
			bm_mark(out[k]);
	spin_unlock_irqrestore(&lm.lock, fl);
	return rc;
}

/* Taken for good: the firmware's framebuffer and low memory. */
static void reserve_range(uint64_t dpa, uint64_t len)
{
	uint64_t fl;
	uint32_t first, last;

	if (!len || dpa >= lm.size)
		return;
	if (dpa + len > lm.size)
		len = lm.size - dpa;
	first = (uint32_t)(dpa / lm.chunk);
	last = (uint32_t)((dpa + len - 1) / lm.chunk);
	spin_lock_irqsave(&lm.lock, &fl);
	for (uint32_t c = first; c <= last; c++) {
		if (!bm_test(c)) {
			bm_mark(c);
			lm.reserved_chunks++;
		}
	}
	spin_unlock_irqrestore(&lm.lock, fl);
}

/* ---- probing ------------------------------------------------------------------------------ */

static uint64_t read64(struct i915_device *i915, uint32_t reg)
{
	return i915_read64(i915, reg);
}

/* DG2 and its kind (flat CCS): the tile's memory less everything from
 * the start of the CCS up, which is the firmware's.  `tile' is the
 * whole of the tile's memory (the stolen memory is placed against it). */
static int probe_flat_ccs(struct i915_device *i915, uint64_t *usable, uint64_t *tile)
{
	uint64_t range = i915_mcr_read(i915, XEHP_TILE0_ADDR_RANGE) & 0xffff;
	uint64_t ccs = i915_mcr_read(i915, XEHP_FLAT_CCS_BASE_ADDR);
	uint64_t size = (range >> XEHP_TILE_LMEM_RANGE_SHIFT) * SZ_1G;
	uint64_t ccs_base = (ccs >> XEHP_CCS_BASE_SHIFT) * SZ_64K;

	if (size < ccs_base) {
		kprintf("[drm] i915: local memory: CCS base %llx beyond the tile's %llu MB\n",
			(unsigned long long)ccs_base, (unsigned long long)(size >> 20));
		return -EIO;
	}
	/* The CCS base register not filled in: everything would look
	 * stolen. */
	if (!ccs_base) {
		kprintf("[drm] i915: local memory: the CCS base register holds no address\n");
		return -EIO;
	}
	*tile = size;
	*usable = ccs_base;
	return 0;
}

/* Battlemage: the tile from its address range register; usable is
 * everything below the flat CCS where the firmware left it enabled, else
 * below the GTT's stolen memory. */
static int probe_xe2_dgfx(struct i915_device *i915, uint64_t *usable, uint64_t *tile)
{
	uint32_t range = i915_read32(i915, SG_TILE_ADDR_RANGE(0));
	uint64_t size = (uint64_t)SG_TILE_SIZE_GB(range) * SZ_1G;
	uint64_t tile_offset = (uint64_t)SG_TILE_OFFSET_GB(range) * SZ_1G;
	uint32_t lo = i915_mcr_read(i915, XE2_FLAT_CCS_BASE_RANGE_LOWER);
	uint64_t offset;

	if (!size) {
		kprintf("[drm] i915: local memory: the tile's address range is empty (%08x)\n", range);
		return -EIO;
	}
	if (lo & XE2_FLAT_CCS_ENABLE) {
		uint32_t hi = i915_mcr_read(i915, XE2_FLAT_CCS_BASE_RANGE_UPPER);
		uint32_t nodes = (i915_read32(i915, MIRROR_FUSE3) & XE2_NODE_ENABLE_MASK) >> 16;
		uint32_t enabled = 0;
		uint64_t ccs_size = size / 512;

		for (; nodes; nodes &= nodes - 1)
			enabled++;
		offset = (uint64_t)(hi & XE2_FLAT_CCS_BASE_UPPER_ADDR_MASK) << 32;
		offset |= lo & XE2_FLAT_CCS_BASE_LOWER_ADDR_MASK;
		offset *= enabled;
		/* the first address the compression hardware owns, rounded
		 * down: everything below goes to the allocator */
		offset &= ~4095ULL;
		i915_dbg("[drm] i915: local memory: flat CCS at %llx (%u L3 nodes)\n",
			 (unsigned long long)offset, enabled);
		if (!offset || offset + ccs_size > read64(i915, GEN6_GSMBASE)) {
			kprintf("[drm] i915: local memory: the flat CCS at %llx runs into the GTT's memory\n",
				(unsigned long long)offset);
			return -EIO;
		}
	} else {
		/* the firmware switched compression off: nothing is kept
		 * for it */
		offset = read64(i915, GEN6_GSMBASE);
		i915_dbg("[drm] i915: local memory: flat CCS disabled by the firmware\n");
	}
	if (offset <= tile_offset)
		return -EIO;
	*tile = size;
	*usable = offset - tile_offset;
	return 0;
}

/* Battlemage keeps the WOPCM at the top of the data stolen memory: its
 * size, 0 where the field holds a value the hardware does not define. */
static uint64_t xe2_wopcm_size(struct i915_device *i915)
{
	uint32_t v = WOPCM_SIZE_FIELD(read64(i915, STOLEN_RESERVED));

	/* 0..3: 1, 2, 4, 8 MB; 5 and 6: 16 and 32 MB; 4 and 7 undefined */
	if (v == 5 || v == 6)
		return (uint64_t)(1u << (v - 1)) * SZ_1M;
	if (v <= 3)
		return (uint64_t)(1u << v) * SZ_1M;
	return 0;
}

/* DG1: everything below the GTT's stolen memory; the stolen memory sits
 * at the top of what BAR2 covers. */
static int probe_dg1(struct i915_device *i915, uint64_t *usable, uint64_t *tile)
{
	*usable = read64(i915, GEN6_GSMBASE);
	*tile = i915->bar_lmem.size;
	return *usable ? 0 : -EIO;
}

/* The data stolen memory: from its base up to the end of the memory,
 * whole megabytes; reached through BAR2 when BAR2 covers all of it. */
static void probe_stolen(struct i915_device *i915, uint64_t tile)
{
	uint64_t dsm_base = read64(i915, GEN6_DSMBASE) & LMEM_BDSM_MASK;

	lm.dsm_base = lm.dsm_size = 0;
	i915->stolen_base = 0;
	i915->stolen_size = 0;
	i915->stolen_io = 0;
	if (tile < dsm_base) {
		kprintf("[drm] i915: stolen memory at %llx lies beyond the card's %llu MB; none used\n",
			(unsigned long long)dsm_base, (unsigned long long)(tile >> 20));
		return;
	}
	lm.dsm_base = dsm_base;
	lm.dsm_size = tile - dsm_base;
	/* Battlemage: the WOPCM at its top is the firmware's */
	if (i915->info->platform == I915_PLATFORM_BATTLEMAGE) {
		uint64_t wopcm = xe2_wopcm_size(i915);
		if (!wopcm || wopcm > lm.dsm_size) {
			kprintf("[drm] i915: the WOPCM size is not one the hardware defines; no stolen memory used\n");
			lm.dsm_base = lm.dsm_size = 0;
			return;
		}
		lm.dsm_size -= wopcm;
	}
	/* a few KB of the platform's own may follow it: whole megabytes */
	lm.dsm_size &= ~((uint64_t)SZ_1M - 1);
	i915->stolen_base = lm.dsm_base;
	i915->stolen_size = lm.dsm_size;
	if (i915->bar_lmem.size >= tile)
		i915->stolen_io = i915->bar_lmem.base + dsm_base;
}

/* The firmware's framebuffer: wherever the global GTT entries of the
 * plane it left enabled point in local memory.  The boot console keeps
 * drawing there until the driver's console takes over, and falls back
 * to it if that fails, so it stays reserved. */
static void reserve_boot_fb(struct i915_device *i915)
{
	uint32_t surf, h, stride_units, tiled;
	uint64_t bytes, first, n, total = 0;
	volatile uint64_t *gtt = (volatile uint64_t *)i915->gtt_virt;

	if (i915->boot_scanout.pipe < 0 || !gtt)
		return;
	surf = i915->boot_scanout.surf & ~0xfffu;
	h = ((i915->boot_scanout.size >> 16) & 0x1fff) + 1;
	stride_units = i915->boot_scanout.stride & 0x7ff;
	/* linear strides are in 64-byte units, tiled ones in tiles: count
	 * the widest tile to be safe */
	tiled = (i915->boot_scanout.plane_ctl >> 10) & 7;
	bytes = (uint64_t)stride_units * (tiled ? 512 : 64) * h;
	if (bytes < 4096)
		bytes = 4096;
	if (bytes > (256ULL << 20))
		bytes = 256ULL << 20;
	first = surf / 4096;
	n = (bytes + 4095) / 4096;
	if (first + n > i915->gtt_size / 8)
		n = first < i915->gtt_size / 8 ? i915->gtt_size / 8 - first : 0;
	for (uint64_t i = 0; i < n; i++) {
		uint64_t pte = gtt[first + i];
		if (!(pte & GGTT_PTE_PRESENT) || !(pte & I915_LMEM_GGTT_PTE_LM))
			break;
		reserve_range(pte & GGTT_PTE_ADDR_MASK, 4096);
		total += 4096;
	}
	if (total)
		i915_dbg("[drm] i915: the firmware's framebuffer: %llu KB of local memory kept\n",
			 (unsigned long long)(total >> 10));
}

int i915_lmem_probe(struct i915_device *i915)
{
	uint64_t usable = 0, tile = 0, gsm;
	uint32_t words;
	int rc;

	mm_memset(&lm, 0, sizeof(lm));
	spinlock_init(&lm.lock, "i915_lmem");
	spinlock_init(&shadow_lock, "i915_lmem_shadow");
	shadows = NULL;
	backups = NULL;
	if (!(i915->info->flags & I915_INFO_IS_DGFX))
		return 0;

	/* BAR2 is the window onto local memory on these parts; there is no
	 * graphics aperture. */
	if (!i915->bar_lmem.size) {
		i915->bar_lmem = i915->bar_aperture;
		i915->bar_aperture.base = 0;
		i915->bar_aperture.size = 0;
	}
	if (!i915->bar_lmem.size || (i915->bar_lmem.flags & PCI_BAR_IO)) {
		kprintf("[drm] i915: no local memory BAR\n");
		return -ENODEV;
	}

	/* The firmware trains the memory and says whether it is healthy;
	 * if not, the GT stays powered down. */
	if (!(i915_read32_fw(i915, GU_CNTL) & GU_CNTL_LMEM_INIT)) {
		kprintf("[drm] i915: the card's firmware did not initialise its local memory\n");
		return -ENODEV;
	}

	switch (i915->info->platform) {
	case I915_PLATFORM_DG2:
		lm.has_64k = 1;
		rc = probe_flat_ccs(i915, &usable, &tile);
		break;
	case I915_PLATFORM_DG1:
		lm.has_64k = 0;
		rc = probe_dg1(i915, &usable, &tile);
		break;
	case I915_PLATFORM_BATTLEMAGE:
		/* 64 KB pages in the page tables, as on DG2 */
		lm.has_64k = 1;
		rc = probe_xe2_dgfx(i915, &usable, &tile);
		break;
	default:
		kprintf("[drm] i915: the local memory layout of %s is not known\n", i915->info->name);
		return -ENODEV;
	}
	if (rc)
		return rc;

	/* Nothing the GTT's or the data stolen memory occupies is ours,
	 * whichever of them comes first. */
	gsm = read64(i915, GEN6_GSMBASE);
	if (gsm && gsm < usable)
		usable = gsm;
	probe_stolen(i915, tile);
	if (lm.dsm_size && lm.dsm_base < usable)
		usable = lm.dsm_base;

	lm.chunk = lm.has_64k ? SZ_64K : 4096;
	lm.chunk_pages = lm.chunk / 4096;
	lm.size = usable & ~((uint64_t)lm.chunk - 1);
	lm.io_base = i915->bar_lmem.base;
	lm.io_size = i915->bar_lmem.size;
	lm.visible = lm.io_size < lm.size ? lm.io_size : lm.size;
	lm.visible &= ~((uint64_t)lm.chunk - 1);
	lm.nchunks = (uint32_t)(lm.size / lm.chunk);
	lm.visible_chunks = (uint32_t)(lm.visible / lm.chunk);
	if (!lm.nchunks || !lm.visible_chunks) {
		kprintf("[drm] i915: no usable local memory (%llu MB, BAR %llu MB)\n",
			(unsigned long long)(lm.size >> 20), (unsigned long long)(lm.io_size >> 20));
		return -ENODEV;
	}

	words = (lm.nchunks + 63) / 64;
	lm.bitmap = kalloc((size_t)words * sizeof(uint64_t));
	if (!lm.bitmap)
		return -ENOMEM;
	mm_memset(lm.bitmap, 0, (size_t)words * sizeof(uint64_t));
	/* the bits past the last chunk are never free */
	for (uint32_t c = lm.nchunks; c < words * 64; c++)
		lm.bitmap[c >> 6] |= 1ULL << (c & 63);
	lm.free_chunks = lm.nchunks;
	lm.free_visible_chunks = lm.visible_chunks;

	rc = window_init();
	if (rc) {
		kprintf("[drm] i915: cannot map the local memory BAR at %llx\n",
			(unsigned long long)lm.io_base);
		kfree(lm.bitmap);
		lm.bitmap = NULL;
		return rc;
	}
	lm.present = 1;

	/* DG1 keeps its first megabyte for the firmware's legacy use. */
	if (i915->info->platform == I915_PLATFORM_DG1)
		reserve_range(0, SZ_1M);
	reserve_boot_fb(i915);

	kprintf("[drm] i915: local memory %llu MB (%llu MB reachable through BAR2 at %llx%s), stolen %llu MB at %llx\n",
		(unsigned long long)(lm.size >> 20), (unsigned long long)(lm.visible >> 20),
		(unsigned long long)lm.io_base, lm.visible < lm.size ? ", small BAR" : "",
		(unsigned long long)(lm.dsm_size >> 20), (unsigned long long)lm.dsm_base);
	if (lm.visible < lm.size)
		kprintf("[drm] i915: only %llu MB of the card's memory is in reach of the processor; enabling \"Resizable BAR\" in the firmware setup gives it all\n",
			(unsigned long long)(lm.visible >> 20));
	return 0;
}

void i915_lmem_fini(struct i915_device *i915)
{
	(void)i915;
	if (!lm.present)
		return;
	window_fini();
	if (lm.bitmap)
		kfree(lm.bitmap);
	lm.bitmap = NULL;
	lm.present = 0;
}

/* ---- objects' backing ------------------------------------------------------------------ */

static int clear_object(struct i915_device *i915, struct drm_gem_object *o)
{
	int hidden = 0;

	for (uint32_t i = 0; i < o->npages; i++) {
		if (i915_lmem_page_dpa(o->pages[i]) >= lm.visible) {
			hidden = 1;
			break;
		}
	}
	if (hidden)
		return lm.clear ? lm.clear(i915, o) : -ENXIO;
	for (uint32_t i = 0; i < o->npages; i++) {
		int rc = lmem_page_io(i915_lmem_page_dpa(o->pages[i]), NULL, 4096, 1);
		if (rc)
			return rc;
	}
	wc_flush();
	return 0;
}

static void chunks_free_pages(const uint64_t *pages, uint32_t npages)
{
	uint64_t fl;

	spin_lock_irqsave(&lm.lock, &fl);
	for (uint32_t i = 0; i < npages; i++) {
		if (!i915_lmem_page(pages[i]))
			continue;
		uint64_t c = i915_lmem_page_dpa(pages[i]) / lm.chunk;
		if (c < lm.nchunks)
			bm_release((uint32_t)c);
	}
	spin_unlock_irqrestore(&lm.lock, fl);
}

/* A pages[] array for `npages' pages of fresh local memory. */
static uint64_t *pages_alloc(uint32_t npages, unsigned flags)
{
	uint32_t n = (uint32_t)(((uint64_t)npages * 4096 + lm.chunk - 1) / lm.chunk);
	uint32_t *chunks = kalloc((size_t)n * sizeof(uint32_t));
	uint64_t *pages;

	if (!chunks)
		return NULL;
	pages = kalloc((size_t)npages * sizeof(uint64_t));
	if (!pages || chunks_alloc(n, flags, chunks)) {
		if (pages)
			kfree(pages);
		kfree(chunks);
		return NULL;
	}
	for (uint32_t i = 0; i < npages; i++)
		pages[i] = I915_LMEM_PAGE_TAG |
			   ((uint64_t)chunks[i / lm.chunk_pages] * lm.chunk +
			    (uint64_t)(i % lm.chunk_pages) * 4096);
	kfree(chunks);
	return pages;
}

int i915_lmem_alloc_pages(struct i915_device *i915, struct drm_gem_object *o, unsigned flags)
{
	uint64_t *pages;
	int rc;

	if (!lm.present)
		return -ENODEV;
	if (!o || o->pages || !o->npages)
		return -EINVAL;
	pages = pages_alloc(o->npages, flags);
	if (!pages)
		return -ENOSPC;
	o->pages = pages;
	if (!(flags & I915_LMEM_ALLOC_NO_CLEAR)) {
		rc = clear_object(i915, o);
		if (rc) {
			chunks_free_pages(o->pages, o->npages);
			kfree(o->pages);
			o->pages = NULL;
			return rc;
		}
	}
	return 0;
}

void i915_lmem_free_pages(struct i915_device *i915, struct drm_gem_object *o)
{
	(void)i915;
	if (!lm.present || !o || !o->pages)
		return;
	chunks_free_pages(o->pages, o->npages);
}

int i915_lmem_migrate(struct i915_device *i915, struct drm_gem_object *o, unsigned flags)
{
	uint64_t *pages, *old;
	int rc = 0;

	(void)i915;
	if (!lm.present)
		return -ENODEV;
	if (!o || !o->pages || !o->npages || o->pages_borrowed)
		return -EINVAL;
	if (i915_lmem_object(o))
		return 0;
	/* the contents go through the processor */
	pages = pages_alloc(o->npages, flags | I915_LMEM_ALLOC_CPU_ACCESS);
	if (!pages)
		return -ENOSPC;
	for (uint32_t i = 0; i < o->npages && !rc; i++)
		rc = lmem_page_io(i915_lmem_page_dpa(pages[i]), phys_to_virt(o->pages[i]), 4096, 1);
	if (rc) {
		chunks_free_pages(pages, o->npages);
		kfree(pages);
		return rc;
	}
	wc_flush();
	old = o->pages;
	o->pages = pages;
	for (uint32_t i = 0; i < o->npages; i++)
		if (old[i])
			mm_free_physical_page(old[i]);
	kfree(old);
	return 0;
}

/* ---- the uAPI's object creation ------------------------------------------------------- */

int i915_lmem_parse_regions(struct i915_device *i915,
			    const struct drm_i915_gem_create_ext_memory_regions *mr,
			    struct i915_lmem_placement *pl)
{
	struct drm_i915_gem_memory_class_instance r[4];

	if (mr->pad || !mr->num_regions || mr->num_regions > 4)
		return -EINVAL;
	/* the extension may name the placements once */
	if (pl->n)
		return -EINVAL;
	if (!mr->regions || !validate_user_ptr(mr->regions, mr->num_regions * sizeof(r[0])) ||
	    copy_from_user(r, (const void *)(uintptr_t)mr->regions, mr->num_regions * sizeof(r[0])))
		return -EFAULT;
	for (uint32_t i = 0; i < mr->num_regions; i++) {
		uint32_t cls = r[i].memory_class;
		if (r[i].memory_instance != 0)
			return -EINVAL;
		if (cls == I915_MEMORY_CLASS_DEVICE && !i915_lmem_present(i915))
			return -EINVAL;
		if (cls != I915_MEMORY_CLASS_SYSTEM && cls != I915_MEMORY_CLASS_DEVICE)
			return -EINVAL;
		if (pl->mask & (1u << cls))
			return -EINVAL; /* named twice */
		pl->region[i] = (uint8_t)cls;
		pl->mask |= 1u << cls;
	}
	pl->n = mr->num_regions;
	return 0;
}

struct drm_gem_object *i915_lmem_object_create(struct i915_device *i915, uint64_t size,
					       const struct i915_lmem_placement *pl,
					       int cpu_access, int *err)
{
	static const struct i915_lmem_placement sys = {
		1, { I915_MEMORY_CLASS_SYSTEM, 0, 0, 0 }, 1u << I915_MEMORY_CLASS_SYSTEM
	};
	struct drm_gem_object *o;
	uint64_t page = 4096;
	unsigned flags = 0;
	int rc = -ENOMEM;

	if (!pl || !pl->n)
		pl = &sys;
	if (cpu_access) {
		/* asked for local memory the processor can map: there must
		 * be system memory to fall back to when the window is full */
		if (pl->n == 1 || !(pl->mask & (1u << I915_MEMORY_CLASS_SYSTEM))) {
			*err = -EINVAL;
			return NULL;
		}
		flags |= I915_LMEM_ALLOC_CPU_ACCESS;
	} else if (pl->n > 1 || pl->region[0] != I915_MEMORY_CLASS_SYSTEM) {
		flags |= I915_LMEM_ALLOC_GPU_ONLY;
	}
	/* the largest page of any placement, so the object fits wherever
	 * it ends up */
	if ((pl->mask & (1u << I915_MEMORY_CLASS_DEVICE)) && lm.present)
		page = lm.chunk;
	size = (size + page - 1) & ~(page - 1);
	if (!size) {
		*err = -EINVAL;
		return NULL;
	}
	if (size > (64ULL << 30)) {
		*err = -E2BIG;
		return NULL;
	}
	o = drm_gem_alloc(&i915->drm, DRM_GEM_BO, size);
	if (!o) {
		*err = -ENOMEM;
		return NULL;
	}
	for (uint32_t i = 0; i < pl->n; i++) {
		if (pl->region[i] == I915_MEMORY_CLASS_DEVICE)
			rc = i915_lmem_alloc_pages(i915, o, flags);
		else
			rc = drm_gem_alloc_pages(o);
		if (rc == 0)
			break;
	}
	if (rc) {
		drm_gem_put(o);
		*err = rc == -ENOSPC ? -ENOSPC : -ENOMEM;
		return NULL;
	}
	if (i915->drm.drv->gem_init && i915->drm.drv->gem_init(o) != 0) {
		drm_gem_put(o);
		*err = -ENOMEM;
		return NULL;
	}
	*err = 0;
	return o;
}

/* ---- processor access --------------------------------------------------------------- */

uint64_t i915_lmem_page_cpu_phys(const struct i915_device *i915, uint64_t entry)
{
	uint64_t dpa;

	(void)i915;
	if (!i915_lmem_page(entry))
		return entry;
	dpa = i915_lmem_page_dpa(entry);
	if (!lm.present || dpa >= lm.visible)
		return 0;
	return lm.io_base + dpa;
}

uint64_t i915_lmem_gem_page_phys(struct drm_gem_object *o, uint64_t index)
{
	if (!o->pages || index >= o->npages)
		return 0;
	return i915_lmem_page_cpu_phys(NULL, o->pages[index]);
}

unsigned i915_lmem_mmap_kind(const struct drm_gem_object *o)
{
	return i915_lmem_object(o) ? 1 /* write-combining */ : 2 /* write-back */;
}

int i915_lmem_object_rw(struct i915_device *i915, struct drm_gem_object *o, uint64_t offset,
			void *buf, uint64_t len, int write)
{
	uint8_t *b = buf;
	int touched_lmem = 0;

	(void)i915;
	if (!o || !o->pages || offset > o->size || len > o->size - offset)
		return -EINVAL;
	while (len) {
		uint32_t page = (uint32_t)(offset / 4096);
		uint32_t in = (uint32_t)(offset & 4095);
		uint32_t chunk = 4096 - in;
		uint64_t e = o->pages[page];
		if (chunk > len)
			chunk = (uint32_t)len;
		if (i915_lmem_page(e)) {
			int rc = lmem_page_io(i915_lmem_page_dpa(e) + in, b, chunk, write);
			if (rc)
				return rc;
			touched_lmem = 1;
		} else {
			uint8_t *va = (uint8_t *)phys_to_virt(e) + in;
			if (write)
				mm_memcpy(va, b, chunk);
			else
				mm_memcpy(b, va, chunk);
		}
		b += chunk;
		offset += chunk;
		len -= chunk;
	}
	if (write && touched_lmem)
		wc_flush();
	return 0;
}

int i915_lmem_object_rw_user(struct i915_device *i915, struct drm_gem_object *o,
			     uint64_t offset, uint64_t uptr, uint64_t len, int write)
{
	uint8_t *bounce;
	int rc = 0;

	if (!o || !o->pages || offset > o->size || len > o->size - offset)
		return -EINVAL;
	if (len && (!uptr || !validate_user_ptr(uptr, len)))
		return -EFAULT;
	/* the window is held with interrupts off; a client's page may
	 * fault, so its bytes pass through a kernel page */
	bounce = kalloc(4096);
	if (!bounce)
		return -ENOMEM;
	while (len && !rc) {
		uint32_t chunk = 4096 - (uint32_t)(offset & 4095);
		if (chunk > len)
			chunk = (uint32_t)len;
		if (write) {
			if (copy_from_user(bounce, (const void *)(uintptr_t)uptr, chunk))
				rc = -EFAULT;
			else
				rc = i915_lmem_object_rw(i915, o, offset, bounce, chunk, 1);
		} else {
			rc = i915_lmem_object_rw(i915, o, offset, bounce, chunk, 0);
			if (!rc && copy_to_user((void *)(uintptr_t)uptr, bounce, chunk))
				rc = -EFAULT;
		}
		offset += chunk;
		uptr += chunk;
		len -= chunk;
	}
	kfree(bounce);
	return rc;
}

/* ---- graphics address spaces ----------------------------------------------------------- */

uint64_t i915_lmem_ggtt_pte_bits(const struct i915_device *i915, uint64_t entry)
{
	(void)i915;
	return i915_lmem_page(entry) ? I915_LMEM_GGTT_PTE_LM : 0;
}

/* DG2 maps local memory 64 KB at a time: the sixteen entries of an
 * aligned 64 KB of address space, all naming one aligned, contiguous
 * 64 KB of local memory, each carry the hint. */
uint64_t i915_lmem_ppgtt_pte_bits(const struct i915_device *i915, const uint64_t *pages,
				  uint32_t idx, uint32_t n, uint64_t va)
{
	uint64_t bits, base;
	uint32_t pos, first;

	(void)i915;
	if (!i915_lmem_page(pages[idx]))
		return 0;
	bits = I915_LMEM_PPGTT_PTE_LM;
	if (!lm.has_64k)
		return bits;
	pos = (uint32_t)((va >> 12) & 15);
	if (idx < pos)
		return bits;
	first = idx - pos;
	if (first + 16 > n)
		return bits;
	base = pages[first];
	if (!i915_lmem_page(base) || (i915_lmem_page_dpa(base) & (SZ_64K - 1)))
		return bits;
	for (uint32_t k = 1; k < 16; k++)
		if (pages[first + k] != base + (uint64_t)k * 4096)
			return bits;
	return bits | I915_LMEM_PTE_PS64;
}

uint32_t i915_lmem_gtt_align_pages(const struct i915_device *i915, const struct drm_gem_object *o)
{
	(void)i915;
	return lm.has_64k && i915_lmem_object(o) ? SZ_64K / 4096 : 1;
}

/* ---- the uAPI's region query ----------------------------------------------------------- */

int i915_lmem_query_regions(struct i915_device *i915, struct drm_i915_memory_region_info *out,
			    int max)
{
	int n = 0;

	if (n < max) {
		struct drm_i915_memory_region_info *r = &out[n];
		mm_memset(r, 0, sizeof(*r));
		r->region.memory_class = I915_MEMORY_CLASS_SYSTEM;
		r->region.memory_instance = 0;
		r->probed_size = i915->total_ram_bytes;
		r->unallocated_size = (uint64_t)mm_get_free_pages() * 4096;
		r->probed_cpu_visible_size = r->probed_size;
		r->unallocated_cpu_visible_size = r->unallocated_size;
	}
	n++;
	if (!lm.present)
		return n;
	if (n < max) {
		struct drm_i915_memory_region_info *r = &out[n];
		mm_memset(r, 0, sizeof(*r));
		r->region.memory_class = I915_MEMORY_CLASS_DEVICE;
		r->region.memory_instance = 0;
		r->probed_size = lm.size;
		r->unallocated_size = i915_lmem_avail(i915);
		r->probed_cpu_visible_size = lm.visible;
		r->unallocated_cpu_visible_size = i915_lmem_cpu_visible_avail(i915);
	}
	return n + 1;
}

/* ---- scanout copies ---------------------------------------------------------------------- */

/* Copy [offset, offset + len) of a system object into its local copy. */
static void shadow_copy(struct drm_gem_object *src, struct drm_gem_object *dst, uint64_t offset,
			uint64_t len)
{
	if (offset >= src->size || offset >= dst->size)
		return;
	if (len > src->size - offset)
		len = src->size - offset;
	if (len > dst->size - offset)
		len = dst->size - offset;
	while (len) {
		uint32_t page = (uint32_t)(offset / 4096);
		uint32_t in = (uint32_t)(offset & 4095);
		uint32_t chunk = 4096 - in;
		if (chunk > len)
			chunk = (uint32_t)len;
		if (!i915_lmem_page(src->pages[page]))
			(void)lmem_page_io(i915_lmem_page_dpa(dst->pages[page]) + in,
					   (uint8_t *)phys_to_virt(src->pages[page]) + in, chunk, 1);
		offset += chunk;
		len -= chunk;
	}
}

static struct shadow *shadow_find(struct drm_gem_object *o)
{
	for (struct shadow *s = shadows; s; s = s->next)
		if (s->obj == o)
			return s;
	return NULL;
}

int i915_lmem_scanout_shadow(struct i915_device *i915, struct drm_gem_object *o, uint32_t *ggtt)
{
	struct drm_gem_object *copy;
	struct shadow *s;
	struct i915_bo *cbo;
	uint64_t fl;
	int rc;

	if (!lm.present || i915_lmem_object(o))
		return 1;
	if (!o || !o->pages || !o->npages)
		return -EINVAL;

	spin_lock_irqsave(&shadow_lock, &fl);
	s = shadow_find(o);
	spin_unlock_irqrestore(&shadow_lock, fl);
	if (!s) {
		s = kalloc(sizeof(*s));
		if (!s)
			return -ENOMEM;
		copy = drm_gem_alloc(&i915->drm, DRM_GEM_BO, o->size);
		if (!copy) {
			kfree(s);
			return -ENOMEM;
		}
		rc = i915_lmem_alloc_pages(i915, copy, I915_LMEM_ALLOC_CPU_ACCESS |
							I915_LMEM_ALLOC_NO_CLEAR);
		if (rc == 0 && i915->drm.drv->gem_init)
			rc = i915->drm.drv->gem_init(copy);
		if (rc == 0) {
			cbo = copy->priv;
			rc = i915_ggtt_bind_scanout(i915, copy, &s->ggtt);
			if (rc == 0 && cbo) {
				cbo->ggtt = s->ggtt;
				cbo->bound = 1;
				cbo->caching = I915_CACHING_NONE;
			}
		}
		if (rc) {
			drm_gem_put(copy);
			kfree(s);
			kprintf("[drm] i915: no local memory for a scanout copy of %llu KB (%d)\n",
				(unsigned long long)(o->size >> 10), rc);
			return rc;
		}
		s->obj = o;
		s->copy = copy;
		spin_lock_irqsave(&shadow_lock, &fl);
		s->next = shadows;
		shadows = s;
		spin_unlock_irqrestore(&shadow_lock, fl);
		i915_dbg("[drm] i915: scanout of a %llu KB system-memory buffer through a local copy at %08x\n",
			 (unsigned long long)(o->size >> 10), s->ggtt);
	}
	/* whatever the buffer holds now is what the screen should show */
	shadow_copy(o, s->copy, 0, o->size);
	wc_flush();
	*ggtt = s->ggtt;
	return 0;
}

int i915_lmem_scanout_dirty(struct i915_device *i915, struct drm_framebuffer *fb,
			    const struct drm_mode_rect_k *rects, uint32_t n)
{
	struct shadow *s;
	uint64_t fl;
	uint32_t cpp;

	(void)i915;
	if (!lm.present || !fb || !fb->obj)
		return 0;
	spin_lock_irqsave(&shadow_lock, &fl);
	s = shadow_find(fb->obj);
	spin_unlock_irqrestore(&shadow_lock, fl);
	if (!s)
		return 0;
	cpp = fb->bpp / 8;
	if (!cpp)
		cpp = 4;
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR || !n || !rects) {
		/* a tiled surface interleaves rectangles across all of it */
		shadow_copy(fb->obj, s->copy, fb->offset, (uint64_t)fb->pitch * fb->height);
	} else {
		for (uint32_t r = 0; r < n; r++) {
			int32_t x1 = rects[r].x1 < 0 ? 0 : rects[r].x1;
			int32_t y1 = rects[r].y1 < 0 ? 0 : rects[r].y1;
			int32_t x2 = rects[r].x2 > (int32_t)fb->width ? (int32_t)fb->width : rects[r].x2;
			int32_t y2 = rects[r].y2 > (int32_t)fb->height ? (int32_t)fb->height : rects[r].y2;
			if (x2 <= x1 || y2 <= y1)
				continue;
			for (int32_t y = y1; y < y2; y++)
				shadow_copy(fb->obj, s->copy,
					    (uint64_t)fb->offset + (uint64_t)y * fb->pitch +
						    (uint64_t)x1 * cpp,
					    (uint64_t)(x2 - x1) * cpp);
		}
	}
	wc_flush();
	return 1;
}

void i915_lmem_object_free(struct i915_device *i915, struct drm_gem_object *o)
{
	struct shadow *s, **pp;
	uint64_t fl;

	(void)i915;
	if (!lm.present || !o)
		return;
	spin_lock_irqsave(&shadow_lock, &fl);
	for (pp = &shadows; (s = *pp) != NULL; pp = &s->next) {
		if (s->obj == o) {
			*pp = s->next;
			break;
		}
	}
	spin_unlock_irqrestore(&shadow_lock, fl);
	if (!s)
		return;
	/* the copy's own free unbinds it and gives its memory back */
	drm_gem_put(s->copy);
	kfree(s);
}

/* ---- power ---------------------------------------------------------------------------- */

int i915_lmem_suspend(struct i915_device *i915)
{
	struct drm_device *dev = &i915->drm;
	unsigned saved = 0;

	if (!lm.present)
		return 0;
	for (struct drm_gem_object *o = dev->objects; o; o = o->next) {
		struct backup *b;
		int rc = 0;

		if (!i915_lmem_object(o) || !o->refs)
			continue;
		b = kalloc(sizeof(*b));
		if (!b)
			return -ENOMEM;
		b->pages = kalloc((size_t)o->npages * sizeof(uint64_t));
		if (!b->pages) {
			kfree(b);
			return -ENOMEM;
		}
		mm_memset(b->pages, 0, (size_t)o->npages * sizeof(uint64_t));
		for (uint32_t i = 0; i < o->npages && !rc; i++) {
			uint64_t dpa = i915_lmem_page_dpa(o->pages[i]);
			if (dpa >= lm.visible) {
				rc = -ENXIO; /* not in reach: lost over the suspend */
				break;
			}
			b->pages[i] = mm_allocate_physical_page_reclaim();
			if (!b->pages[i]) {
				rc = -ENOMEM;
				break;
			}
			rc = lmem_page_io(dpa, phys_to_virt(b->pages[i]), 4096, 0);
		}
		if (rc) {
			for (uint32_t i = 0; i < o->npages; i++)
				if (b->pages[i])
					mm_free_physical_page(b->pages[i]);
			kfree(b->pages);
			kfree(b);
			if (rc == -ENXIO)
				continue;
			return rc;
		}
		drm_gem_get(o);
		b->obj = o;
		b->next = backups;
		backups = b;
		saved++;
	}
	i915_dbg("[drm] i915: %u local objects saved for the suspend\n", saved);
	return 0;
}

void i915_lmem_resume(struct i915_device *i915)
{
	(void)i915;
	while (backups) {
		struct backup *b = backups;
		struct drm_gem_object *o = b->obj;

		backups = b->next;
		for (uint32_t i = 0; i < o->npages; i++) {
			if (!b->pages[i])
				continue;
			(void)lmem_page_io(i915_lmem_page_dpa(o->pages[i]), phys_to_virt(b->pages[i]),
					   4096, 1);
			mm_free_physical_page(b->pages[i]);
		}
		kfree(b->pages);
		kfree(b);
		drm_gem_put(o);
	}
	wc_flush();
}
