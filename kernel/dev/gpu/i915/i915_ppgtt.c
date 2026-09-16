// LikeOS -- per-process graphics address spaces (Gen8+ 4-level tables).
//
// A 48-bit address space of the same shape as the CPU's: a page map
// level 4 of 512 entries to page directory pointers, to page directories,
// to page tables, to 4 KB pages.  Unmapped ranges point at scratch tables
// that lead to one scratch page, so the hardware never walks into an
// absent entry (which it treats as a fault and stops on).  Tables are
// allocated as ranges are bound and kept until the space dies.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
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

static inline uint64_t pde_encode(uint64_t phys)
{
	return (phys & ADDR_MASK) | GEN8_PDE_PRESENT | GEN8_PDE_RW;
}

static inline uint64_t pte_encode(struct i915_device *i915, uint64_t phys,
				  int uncached)
{
	uint64_t pte = (phys & ADDR_MASK) | GEN8_PDE_PRESENT | GEN8_PDE_RW;
	if (i915->info->gen_x10 >= 120) {
		/* PAT index in bits 3,4,7 (index 3 = uncached) */
		if (uncached)
			pte |= GEN8_PTE_CACHE_PWT | GEN8_PTE_CACHE_PCD;
		return pte;
	}
	if (uncached)
		pte |= GEN8_PTE_CACHE_PWT | GEN8_PTE_CACHE_PCD;
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
	table_fill(vm->scratch_pt, pte_encode(i915, vm->scratch_page, 0));
	table_fill(vm->scratch_pd, pde_encode(vm->scratch_pt));
	table_fill(vm->scratch_pdp, pde_encode(vm->scratch_pd));
	table_fill(vm->pml4, pde_encode(vm->scratch_pdp));
	table_flush(i915, vm->scratch_pt);
	table_flush(i915, vm->scratch_pd);
	table_flush(i915, vm->scratch_pdp);
	table_flush(i915, vm->pml4);
	/* relocation clients get addresses from 4 GB up, clear of anything
	 * a 32-bit-minded client pins low */
	vm->alloc_next = 1ULL << 32;
	return vm;
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
		table_fill(pt_phys, pte_encode(i915, vm->scratch_page, 0));
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
	struct i915_device *i915 = &g_i915;
	uint64_t fl;

	if (addr & 0xfff || addr + (uint64_t)npages * 4096 > I915_VM_SIZE)
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
			pt[(p >> 12) & 0x1ff] =
				pte_encode(i915, pages[done + i], uncached);
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

	spin_lock_irqsave(&vm->lock, &fl);
	for (uint32_t i = 0; i < npages; i++) {
		uint64_t a = addr + (uint64_t)i * 4096;
		uint64_t *pt = pt_for(vm, a, 0, NULL);
		if (!pt)
			continue;
		pt[(a >> 12) & 0x1ff] = pte_encode(i915, vm->scratch_page, 0);
		if (((a >> 12) & 0x1ff) == 0x1ff || i == npages - 1)
			table_flush(i915, virt_to_phys(pt));
	}
	vm->tlb_dirty = 1;
	spin_unlock_irqrestore(&vm->lock, fl);
}
