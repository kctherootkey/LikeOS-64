// LikeOS -- per-process graphics address spaces (Gen8+ 4-level tables).
//
// A 48-bit address space of the same shape as the CPU's: a page map
// level 4 of 512 entries to page directory pointers, to page directories,
// to page tables, to 4 KB pages.  Xe2 and later walk five levels: a
// fifth table above, whose first entry leads to the 48 bits a client
// has.  Unmapped ranges point at scratch tables
// that lead to one scratch page, so the hardware never walks into an
// absent entry (which it treats as a fault and stops on).  Tables are
// allocated as ranges are bound and kept until the space dies.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2021-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_xe2_reg.h>
#include <kernel/dev/gpu/i915/i915_lmem.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define ENTRIES 512
#define ADDR_MASK 0x0000FFFFFFFFF000ULL

static int page_list_add(struct i915_page_list *l, uint64_t phys)
{
	if (l->n == l->cap) {
		uint32_t cap = l->cap ? l->cap * 2 : 64;
		uint64_t *p = kalloc(cap * sizeof(uint64_t));
		if (!p)
			return -ENOMEM;
		if (l->phys) {
			mm_memcpy(p, l->phys, l->n * sizeof(uint64_t));
			kfree(l->phys);
		}
		l->phys = p;
		l->cap = cap;
	}
	l->phys[l->n++] = phys;
	return 0;
}

/* A few pages set aside before the address space is locked.  Tables are
 * needed while the lock is held and interrupts are off, where an
 * allocation cannot reclaim -- and an allocation that cannot reclaim
 * fails as soon as the page cache holds the free memory, which is most
 * of the time on a machine that has been running for a few seconds.
 * Three is the depth of the table walk: one level can be missing at
 * each of the three levels above a page table. */
#define VM_SPARE_TABLES 3

struct vm_spares {
	uint64_t page[VM_SPARE_TABLES];
	int n;
};

/* One table, allocated the slow way: only the scratch tables and the
 * top level are made like this, at creation time, where reclaim is
 * allowed and no lock is held. */
static uint64_t table_alloc(struct i915_vm *vm)
{
	uint64_t phys = mm_allocate_physical_page_reclaim();
	if (!phys)
		return 0;
	mm_memset(phys_to_virt(phys), 0, 4096);
	if (page_list_add(&vm->tables, phys) != 0) {
		mm_free_physical_page(phys);
		return 0;
	}
	return phys;
}

static void spares_release(struct vm_spares *sp)
{
	while (sp->n > 0)
		mm_free_physical_page(sp->page[--sp->n]);
}

/* Called with the lock NOT held, so reclaim can run. */
static int spares_fill(struct vm_spares *sp)
{
	while (sp->n < VM_SPARE_TABLES) {
		uint64_t phys = mm_allocate_physical_page_reclaim();
		if (!phys) {
			spares_release(sp);
			return -ENOMEM;
		}
		mm_memset(phys_to_virt(phys), 0, 4096);
		sp->page[sp->n++] = phys;
	}
	return 0;
}

/* Take one of the reserved pages and record it as this space's, so it is
 * freed when the space dies.  Zero when the reserve is empty. */
static uint64_t table_take(struct i915_vm *vm, struct vm_spares *sp)
{
	if (sp->n <= 0)
		return 0;
	uint64_t phys = sp->page[sp->n - 1];
	if (page_list_add(&vm->tables, phys) != 0)
		return 0;
	sp->n--;
	return phys;
}

static void table_fill(uint64_t table_phys, uint64_t entry)
{
	uint64_t *t = phys_to_virt(table_phys);
	for (int i = 0; i < ENTRIES; i++)
		t[i] = entry;
}

/* A directory entry.  Xe2 on: with the attributes the hardware reads the
 * table below through -- the cached, coherent entry (2) for the system
 * memory the tables are in -- in its two index bits. */
static inline uint64_t pde_encode(uint64_t phys)
{
	uint64_t pde = (phys & ADDR_MASK) | GEN8_PDE_PRESENT | GEN8_PDE_RW;

	if (g_i915.gt_ip >= I915_IP(20, 0)) {
		uint32_t pat = i915_pat_index(&g_i915, 0);
		if (pat & 1)
			pde |= GEN12_PPGTT_PTE_PAT0;
		if (pat & 2)
			pde |= GEN12_PPGTT_PTE_PAT1;
	}
	return pde;
}

/* A page's entry names its page attribute table entry by index: Gen12
 * spreads the index over bits 3, 4 and 7 (and Meteor Lake's fourth bit
 * over bit 62); before Gen12 the same bits are the PWT, PCD and PAT
 * bits whose combination is the index -- uncached at 3, the display's
 * write-through at 2. */
static inline uint64_t pte_encode(struct i915_device *i915, uint64_t phys, uint32_t pat)
{
	uint64_t pte = (phys & ADDR_MASK) | GEN8_PDE_PRESENT | GEN8_PDE_RW;

	if (i915->info->gen_x10 >= 120) {
		if (pat & 1)
			pte |= GEN12_PPGTT_PTE_PAT0;
		if (pat & 2)
			pte |= GEN12_PPGTT_PTE_PAT1;
		if (pat & 4)
			pte |= GEN12_PPGTT_PTE_PAT2;
		if ((pat & 8) && i915->gt_ip >= I915_IP(12, 70))
			pte |= MTL_PPGTT_PTE_PAT3;
		if ((pat & 16) && i915->gt_ip >= I915_IP(20, 0))
			pte |= XE2_PPGTT_PTE_PAT4;
		return pte;
	}
	if (pat == 3)
		pte |= GEN8_PTE_CACHE_PWT | GEN8_PTE_CACHE_PCD;
	else if (pat == 2)
		pte |= GEN8_PTE_CACHE_PCD;
	return pte;
}

/* Flush the CPU's writes to a table the hardware reads: on parts without
 * a shared LLC the table memory is not snooped. */
static void table_flush(struct i915_device *i915, uint64_t phys)
{
	if (i915->info->flags & I915_INFO_HAS_LLC)
		return;
	uint8_t *va = phys_to_virt(phys);
	for (int i = 0; i < 4096; i += 64)
		__asm__ volatile("clflush (%0)" ::"r"(va + i) : "memory");
	__asm__ volatile("mfence" ::: "memory");
}

struct i915_vm *i915_vm_create(struct i915_device *i915)
{
	/* before Broadwell every context shares one space */
	if (i915_is_legacy(i915))
		return i915_legacy_vm_create(i915);
	struct i915_vm *vm = kalloc(sizeof(*vm));
	if (!vm)
		return NULL;
	mm_memset(vm, 0, sizeof(*vm));
	vm->refs = 1;
	spinlock_init(&vm->lock, "i915_vm");
	vm->scratch_page = table_alloc(vm);
	vm->scratch_pt = table_alloc(vm);
	vm->scratch_pd = table_alloc(vm);
	vm->scratch_pdp = table_alloc(vm);
	vm->pml4 = table_alloc(vm);
	if (!vm->scratch_page || !vm->scratch_pt || !vm->scratch_pd ||
	    !vm->scratch_pdp || !vm->pml4) {
		i915_vm_put(vm);
		return NULL;
	}
	table_fill(vm->scratch_pt, pte_encode(i915, vm->scratch_page, i915_pat_index(i915, 0)));
	table_fill(vm->scratch_pd, pde_encode(vm->scratch_pt));
	table_fill(vm->scratch_pdp, pde_encode(vm->scratch_pd));
	table_fill(vm->pml4, pde_encode(vm->scratch_pdp));
	table_flush(i915, vm->scratch_pt);
	table_flush(i915, vm->scratch_pd);
	table_flush(i915, vm->scratch_pdp);
	if (i915_vm_3lvl(i915)) {
		/* the 4 GB a three-level space has: its directories now,
		 * since the contexts' registers name them and nothing
		 * rewrites those later */
		uint64_t pdp = table_alloc(vm);
		uint64_t *pml4 = phys_to_virt(vm->pml4), *pdpv;
		if (!pdp) {
			i915_vm_put(vm);
			return NULL;
		}
		table_fill(pdp, pde_encode(vm->scratch_pd));
		pdpv = phys_to_virt(pdp);
		for (int i = 0; i < 4; i++) {
			vm->pd32[i] = table_alloc(vm);
			if (!vm->pd32[i]) {
				i915_vm_put(vm);
				return NULL;
			}
			table_fill(vm->pd32[i], pde_encode(vm->scratch_pt));
			table_flush(i915, vm->pd32[i]);
			pdpv[i] = pde_encode(vm->pd32[i]);
		}
		table_flush(i915, pdp);
		pml4[0] = pde_encode(pdp);
	}
	table_flush(i915, vm->pml4);
	if (i915->gt_ip >= I915_IP(20, 0)) {
		/* the fifth level: the 48-bit space at its first entry */
		uint64_t *top;
		vm->scratch_pml4 = table_alloc(vm);
		vm->pml5 = table_alloc(vm);
		if (!vm->scratch_pml4 || !vm->pml5) {
			i915_vm_put(vm);
			return NULL;
		}
		table_fill(vm->scratch_pml4, pde_encode(vm->scratch_pdp));
		table_fill(vm->pml5, pde_encode(vm->scratch_pml4));
		top = phys_to_virt(vm->pml5);
		top[0] = pde_encode(vm->pml4);
		/* an address with bit 47 set reaches the engine sign-extended
		 * (bits 56:48 all set): the last entry leads to the same
		 * fourth level, so either form finds the same page */
		top[ENTRIES - 1] = pde_encode(vm->pml4);
		table_flush(i915, vm->scratch_pml4);
		table_flush(i915, vm->pml5);
	}
	/* relocation clients: objects that must stay below 4 GB start at
	 * 1 MB (nothing lands on the null page), the rest from 4 GB up */
	vm->alloc_low = 1ULL << 20;
	vm->alloc_high = 1ULL << 32;
	return vm;
}

/* An address for an object the client did not place: next fit from the
 * zone's cursor, wrapping to the zone's start once, and the binding
 * attached under the same hold of the lock so nothing else can take the
 * gap in between.  The zone below 4 GB is for objects without the 48-bit
 * flag -- the contract a relocation client relies on when it writes a
 * 32-bit address into a command -- and it stays clear of the other zone
 * so that it is never eaten by objects that could go anywhere. */
int i915_vm_vma_alloc(struct i915_vm *vm, struct i915_vma *v, uint32_t npages,
		      uint64_t align, int low)
{
	uint64_t zone_start = low ? (1ULL << 20) : (1ULL << 32);
	uint64_t zone_end = low ? (1ULL << 32) : i915_vm_total(&g_i915);
	uint64_t *cursor = low ? &vm->alloc_low : &vm->alloc_high;
	uint64_t fl, a;

	if (i915_is_legacy(&g_i915))
		return i915_legacy_vm_vma_alloc(vm, v, npages, align, low);
	if (i915_vm_3lvl(&g_i915) && !low) {
		/* nothing above 4 GB in a three-level space */
		zone_start = 1ULL << 20;
		zone_end = 1ULL << 32;
		cursor = &vm->alloc_low;
	}
	if (align < 4096)
		align = 4096;
	if (align & (align - 1))
		return -EINVAL;
	spin_lock_irqsave(&vm->lock, &fl);
	if (v->attached)
		i915_vma_list_detach(&vm->vmas, v);
	a = i915_vma_list_find_gap(vm->vmas, *cursor, zone_end, npages, align);
	if (!a)
		a = i915_vma_list_find_gap(vm->vmas, zone_start, zone_end, npages, align);
	if (a) {
		*cursor = a + (uint64_t)npages * 4096;
		v->addr = a;
		v->npages = npages;
		v->allocated = 1;
		i915_vma_list_attach(&vm->vmas, v);
	}
	spin_unlock_irqrestore(&vm->lock, fl);
	return a ? 0 : -ENOSPC;
}

uint64_t i915_vm_root(struct i915_vm *vm)
{
	if (vm->pml5)
		return pde_encode(vm->pml5);
	return vm->pml4;
}

void i915_vm_get(struct i915_vm *vm)
{
	__atomic_fetch_add(&vm->refs, 1, __ATOMIC_ACQ_REL);
}

void i915_vm_put(struct i915_vm *vm)
{
	if (!vm)
		return;
	if (__atomic_sub_fetch(&vm->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	for (uint32_t i = 0; i < vm->tables.n; i++)
		mm_free_physical_page(vm->tables.phys[i]);
	if (vm->tables.phys)
		kfree(vm->tables.phys);
	kfree(vm);
}

/* The page table for `addr', creating the levels above it from the
 * caller's reserve as needed; NULL when a level is missing and the
 * reserve is empty.  Caller holds vm->lock. */
static uint64_t *pt_for(struct i915_vm *vm, uint64_t addr, int create,
			struct vm_spares *sp)
{
	struct i915_device *i915 = &g_i915;
	uint64_t *pml4 = phys_to_virt(vm->pml4);
	unsigned i4 = (addr >> 39) & 0x1ff, i3 = (addr >> 30) & 0x1ff,
		 i2 = (addr >> 21) & 0x1ff;
	uint64_t pdp_phys, pd_phys, pt_phys;

	pdp_phys = pml4[i4] & ADDR_MASK;
	if (pdp_phys == vm->scratch_pdp) {
		if (!create)
			return NULL;
		pdp_phys = table_take(vm, sp);
		if (!pdp_phys)
			return NULL;
		table_fill(pdp_phys, pde_encode(vm->scratch_pd));
		pml4[i4] = pde_encode(pdp_phys);
		table_flush(i915, pdp_phys);
		table_flush(i915, vm->pml4);
	}
	uint64_t *pdp = phys_to_virt(pdp_phys);
	pd_phys = pdp[i3] & ADDR_MASK;
	if (pd_phys == vm->scratch_pd) {
		if (!create)
			return NULL;
		pd_phys = table_take(vm, sp);
		if (!pd_phys)
			return NULL;
		table_fill(pd_phys, pde_encode(vm->scratch_pt));
		pdp[i3] = pde_encode(pd_phys);
		table_flush(i915, pd_phys);
		table_flush(i915, pdp_phys);
	}
	uint64_t *pd = phys_to_virt(pd_phys);
	pt_phys = pd[i2] & ADDR_MASK;
	if (pt_phys == vm->scratch_pt) {
		if (!create)
			return NULL;
		pt_phys = table_take(vm, sp);
		if (!pt_phys)
			return NULL;
		table_fill(pt_phys, pte_encode(i915, vm->scratch_page, i915_pat_index(i915, 0)));
		pd[i2] = pde_encode(pt_phys);
		table_flush(i915, pt_phys);
		table_flush(i915, pd_phys);
	}
	return phys_to_virt(pt_phys);
}

/* The page behind an address, or 0 when nothing is bound there.  For
 * diagnosis: reads the tables without the lock, as the engine does. */
uint64_t i915_vm_lookup(struct i915_vm *vm, uint64_t addr)
{
	if (i915_is_legacy(&g_i915))
		return i915_legacy_vm_lookup(vm, addr);
	uint64_t *pt = pt_for(vm, addr & 0x0000FFFFFFFFF000ULL, 0, NULL);
	if (!pt)
		return 0;
	uint64_t pte = pt[(addr >> 12) & 0x1ff];
	if (!(pte & GEN8_PDE_PRESENT))
		return 0;
	uint64_t phys = pte & ADDR_MASK;
	if (phys == vm->scratch_page)
		return 0;
	return phys | (addr & 0xfff);
}

int i915_vm_bind(struct i915_vm *vm, uint64_t addr, const uint64_t *pages,
		 uint32_t npages, int uncached)
{
	if (i915_is_legacy(&g_i915))
		return i915_legacy_vm_bind(vm, addr, pages, npages, uncached);
	return i915_vm_bind_pat(vm, addr, pages, npages, i915_pat_index(&g_i915, uncached));
}

int i915_vm_bind_pat(struct i915_vm *vm, uint64_t addr, const uint64_t *pages,
		     uint32_t npages, uint32_t pat)
{
	struct i915_device *i915 = &g_i915;
	uint64_t fl;

	if (i915_is_legacy(i915))
		return i915_legacy_vm_bind(vm, addr, pages, npages, i915_pat_is_uncached(i915, pat));
	if (addr & 0xfff ||
	    addr + (uint64_t)npages * 4096 > (i915_vm_3lvl(i915) ? 1ULL << 32 : I915_VM_SIZE))
		return -EINVAL;
	/* One run per page table: every page of a run shares its table, so
	 * the three pages reserved before each run cover everything the
	 * walk can have to create. */
	uint32_t done = 0;
	while (done < npages) {
		uint64_t a = addr + (uint64_t)done * 4096;
		uint32_t run = 512 - (uint32_t)((a >> 12) & 0x1ff);
		if (run > npages - done)
			run = npages - done;
		struct vm_spares sp = { { 0, 0, 0 }, 0 };
		if (spares_fill(&sp) != 0)
			return -ENOMEM;
		spin_lock_irqsave(&vm->lock, &fl);
		uint64_t *pt = pt_for(vm, a, 1, &sp);
		if (!pt) {
			spin_unlock_irqrestore(&vm->lock, fl);
			spares_release(&sp);
			return -ENOMEM;
		}
		for (uint32_t i = 0; i < run; i++) {
			uint64_t p = a + (uint64_t)i * 4096;
			pt[(p >> 12) & 0x1ff] = pte_encode(i915, pages[done + i], pat) |
						i915_lmem_ppgtt_pte_bits(i915, pages, done + i,
									 npages, p);
		}
		table_flush(i915, virt_to_phys(pt));
		vm->tlb_dirty = 1;
		spin_unlock_irqrestore(&vm->lock, fl);
		spares_release(&sp);
		done += run;
	}
	return 0;
}

void i915_vm_unbind(struct i915_vm *vm, uint64_t addr, uint32_t npages)
{
	struct i915_device *i915 = &g_i915;
	uint64_t fl;

	if (i915_is_legacy(i915)) {
		i915_legacy_vm_unbind(vm, addr, npages);
		return;
	}
	spin_lock_irqsave(&vm->lock, &fl);
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t a = addr + (uint64_t)i * 4096;
		uint64_t *pt = pt_for(vm, a, 0, NULL);
		if (!pt)
			continue;
		pt[(a >> 12) & 0x1ff] = pte_encode(i915, vm->scratch_page, i915_pat_index(i915, 0));
		if (((a >> 12) & 0x1ff) == 0x1ff || i == npages - 1)
			table_flush(i915, virt_to_phys(pt));
	}
	vm->tlb_dirty = 1;
	spin_unlock_irqrestore(&vm->lock, fl);
}
