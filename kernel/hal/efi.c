// LikeOS -- UEFI runtime services: the page table, the calls, the guard.
//
// See efi.h for the overview.  The parts, in the order a call meets them:
//
// The page table.  A PML4 of our own whose lower half maps, one-to-one
// (virtual == physical), every region the bootloader reported with the
// EFI_MEMORY_RUNTIME attribute, and the bounce area and stack below; its
// upper half is the running address space's, copied in before every call
// so the kernel -- the exception and NMI handlers in particular -- stays
// reachable while the firmware runs.  Nothing in it is user-accessible.
//
//   runtime services code      read/write/execute.  Code and data should
//                              be split by permission, but firmware has
//                              been seen writing into its code regions,
//                              so code stays writable.
//   runtime services data      read/write, no execute (unless the table
//                              of entry points says the firmware runs
//                              code from there: that region is then
//                              made executable, with a note in the log).
//   memory-mapped I/O          read/write, no execute, uncached (the
//                              flash controller of the variable store).
//   anything else marked       read/write, no execute; uncached unless
//   runtime (reserved, NVS)    the firmware says write-back is allowed.
//
// The bounce area.  Every buffer the firmware is handed -- the variable
// name, the vendor GUID, the data, the sizes -- is copied into pages of
// our own that the table maps one-to-one, so the firmware only ever sees
// physical addresses, as physical mode promises it.  The kernel reaches
// the same pages through the direct map.
//
// The stack.  64 KiB, mapped one-to-one as well, with an unmapped page
// below it, so a firmware that recurses deeply cannot write over the bounce
// area or a kernel stack.
//
// The call.  efi_rt_call() runs with g_efi_lock held (callers sleep for
// it): the preemption gate is raised (percpu.h), the task's FPU registers
// are saved (kernel_fpu_begin) and the FPU is reset to the state the UEFI
// calling convention requires, interrupts are disabled on this CPU, CR3 is
// loaded with our table, and efi_call_thunk() switches to the stack and
// calls the entry point with the UEFI x64 calling convention (ms_abi).  Afterwards CR3,
// interrupts, the FPU and the gate are restored in reverse order.
//
// Why interrupts stay off for the whole call, slow SetVariable included.
// An interrupt taken inside the firmware runs its handler on the firmware's
// stack and on our table, and the handlers are not fit for that: the
// framebuffer console blits with SSE from interrupt context under
// kernel_fpu_begin(), which nests inside this call's own bracket without
// saving anything -- the firmware's live XMM registers would be overwritten
// under it.  So the firmware runs undisturbed and the time it takes cannot be
// bounded from here; what is bounded is that each window is one firmware
// call (an enumeration is many short calls, interrupts on between them), and
// a call that keeps interrupts off for long is reported with its duration.
//
// The gate is for the firmware that turns interrupts on by itself (`sti'):
// the handlers then run anyway, but the exit of the interrupt does not switch
// this CPU to another task while CR3 is our table.  An exception in such a
// handler is not taken for the firmware's (see efi_rt_fault).
//
// This CPU is not taken out of the address-space accounting for the call
// (sched_mmu_track_*): it keeps counting as a holder of the address space
// it left, which is the safe direction to be wrong in -- an invalidation
// meant for that address space still reaches it (once interrupts are back
// on), and the CR3 load on the way back drops every non-global
// translation the call made.
//
// The guard.  An exception taken while g_efi_in_call is set and CR3 is
// our table (only the calling CPU can have it loaded) is the firmware's:
// efi_rt_fault() records it and points the exception's return at the end
// of the thunk with EFI_ABORTED in RAX; the thunk then restores the
// kernel's stack and callee-saved registers, which the firmware may have
// left in any state.  The runtime services are disabled from then on --
// whatever state the firmware is in, calling it again is not safe.  What
// cannot be contained: a firmware that overruns its stack into the guard
// page (the exception frame itself cannot be written: double fault), one
// that loops forever, and one that writes over kernel memory.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/hal/efi.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/storage.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ---- what the bootloader passed ------------------------------------- */

struct efi_region {
	uint32_t type;
	int exec; /* mapped executable */
	uint64_t phys;
	uint64_t pages;
	uint64_t attr;
};

static struct {
	int valid;
	uint64_t st; /* EFI_SYSTEM_TABLE, physical */
	uint64_t rt; /* EFI_RUNTIME_SERVICES, physical */
	uint32_t memdesc_version;
	uint32_t memdesc_size;
	uint32_t fw_revision;
	uint32_t spec_revision;
	char vendor[64];
	uint32_t n;
	uint32_t dropped;
	struct efi_region r[BOOT_EFI_RT_MAX];
} g_efi_boot;

/* ---- runtime state --------------------------------------------------- */

static DEFINE_MUTEX(g_efi_lock); /* one firmware call at a time */
static int g_efi_enabled; /* calls may be made (under g_efi_lock) */
static uint64_t *g_efi_pml4; /* our table (direct map) */
static uint64_t g_efi_pml4_phys;

/* Bounce area, stack and guard page: one contiguous allocation.
 *
 *   [guard: 1 page, unmapped][stack: EFI_STACK_PAGES][bounce: EFI_BOUNCE_PAGES]
 */
#define EFI_STACK_PAGES 16 /* 64 KiB */
#define EFI_BOUNCE_DATA_OFF PAGE_SIZE
#define EFI_BOUNCE_PAGES (1 + EFI_VAR_DATA_MAX / PAGE_SIZE)
#define EFI_AREA_PAGES (1 + EFI_STACK_PAGES + EFI_BOUNCE_PAGES)
static uint64_t g_efi_area_phys; /* the guard page */
static uint64_t g_efi_stack_top; /* physical == the address the firmware sees */
static uint64_t g_efi_bounce_phys;

/* Offsets inside the first bounce page. */
#define EB_NAME 0 /* EFI_VAR_NAME_MAX UCS-2 units */
#define EB_GUID (EFI_VAR_NAME_MAX * 2) /* 16 bytes */
#define EB_ATTR (EB_GUID + 16) /* UINT32 */
#define EB_SIZE (EB_ATTR + 8) /* UINTN */
#define EB_Q0 (EB_SIZE + 8) /* QueryVariableInfo: 3 x UINT64 */
#define EB_Q1 (EB_Q0 + 8)
#define EB_Q2 (EB_Q1 + 8)
#define EB_TIME (EB_Q2 + 8) /* EFI_TIME, 16 bytes */
#define EB_TCAP (EB_TIME + 16) /* EFI_TIME_CAPABILITIES, 12 bytes */
#define EB_END (EB_TCAP + 16)

/* The runtime services table: entry points by byte offset. */
#define RT_SIGNATURE 0x56524553544e5552ULL /* "RUNTSERV" */
#define RT_HDR_REVISION 8
#define RT_HDR_SIZE 12
#define RT_GET_TIME 24
#define RT_GET_VARIABLE 72
#define RT_GET_NEXT_VARIABLE_NAME 80
#define RT_SET_VARIABLE 88
#define RT_QUERY_VARIABLE_INFO 128
#define RT_TABLE_BYTES 136

static uint64_t g_rt_get_time;
static uint64_t g_rt_get_variable;
static uint64_t g_rt_get_next_variable_name;
static uint64_t g_rt_set_variable;
static uint64_t g_rt_query_variable_info; /* 0 before UEFI 2.0 */

/* The guard's view of the call in progress.  Set and cleared by the
 * calling CPU with interrupts off; read by its own exception handler. */
static volatile int g_efi_in_call;
static volatile int g_efi_faulted;
static int g_efi_irq_warned;
static uint64_t g_efi_fault_vector, g_efi_fault_error, g_efi_fault_rip,
	g_efi_fault_cr2;

/* The kernel stack pointer the thunk returns to (see below). */
uint64_t g_efi_thunk_rsp;

/* ---- the thunk ------------------------------------------------------- */

/*
 * uint64_t efi_call_thunk(uint64_t fn, uint64_t a1, uint64_t a2,
 *                         uint64_t a3, uint64_t a4, uint64_t a5,
 *                         uint64_t stack_top);
 *
 * Called with the System V convention, calls `fn' with the UEFI x64 (ms_abi)
 * one on the firmware stack: a1..a4 in RCX, RDX, R8, R9, a5 on the stack
 * above 32 bytes of shadow space, RSP 16-byte aligned at the call.  The
 * callee-saved registers of the System V side are pushed on the KERNEL
 * stack and g_efi_thunk_rsp remembers where, so efi_call_thunk_resume
 * can return correctly whether the firmware came back by itself or the
 * exception guard sent the CPU there.
 */
extern uint64_t efi_call_thunk(uint64_t fn, uint64_t a1, uint64_t a2,
			       uint64_t a3, uint64_t a4, uint64_t a5,
			       uint64_t stack_top);
extern char efi_call_thunk_resume[];

__asm__(".text\n"
	".globl efi_call_thunk\n"
	".type efi_call_thunk, @function\n"
	"efi_call_thunk:\n"
	"	pushq %rbp\n"
	"	pushq %rbx\n"
	"	pushq %r12\n"
	"	pushq %r13\n"
	"	pushq %r14\n"
	"	pushq %r15\n"
	"	movabsq $g_efi_thunk_rsp, %rax\n"
	"	movq %rsp, (%rax)\n"
	/* the 7th argument: above the return address and six pushes */
	"	movq 56(%rsp), %rax\n"
	"	andq $-16, %rax\n"
	"	movq %rax, %rsp\n"
	/* 32 bytes of shadow space and the fifth argument, aligned */
	"	subq $48, %rsp\n"
	"	movq %r9, 32(%rsp)\n"
	"	movq %r8, %r9\n"
	"	movq %rcx, %r8\n"
	"	movq %rsi, %rcx\n"
	"	cld\n"
	"	callq *%rdi\n"
	".globl efi_call_thunk_resume\n"
	"efi_call_thunk_resume:\n"
	"	movabsq $g_efi_thunk_rsp, %rcx\n"
	"	movq (%rcx), %rsp\n"
	"	popq %r15\n"
	"	popq %r14\n"
	"	popq %r13\n"
	"	popq %r12\n"
	"	popq %rbx\n"
	"	popq %rbp\n"
	"	cld\n"
	"	retq\n"
	".size efi_call_thunk, . - efi_call_thunk\n");

/* ---- small helpers --------------------------------------------------- */

static inline uint64_t efi_read_cr3(void)
{
	uint64_t v;

	__asm__ volatile("mov %%cr3, %0" : "=r"(v));
	return v;
}

static inline void efi_write_cr3(uint64_t v)
{
	__asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline uint64_t efi_read_cr2(void)
{
	uint64_t v;

	__asm__ volatile("mov %%cr2, %0" : "=r"(v));
	return v;
}

static inline uint64_t efi_read_flags(void)
{
	uint64_t f;

	__asm__ volatile("pushfq; popq %0" : "=r"(f) : : "memory");
	return f;
}

/* The FPU as the calling convention wants it: x87 control word 0x037F
 * (all exceptions masked, extended precision, round to nearest) and
 * MXCSR 0x1F80 (all exceptions masked).  The task's own values were saved
 * by kernel_fpu_begin() and come back with kernel_fpu_end(). */
static inline void efi_fpu_reset(void)
{
	uint32_t mxcsr = 0x1f80;

	__asm__ volatile("fninit\n\tldmxcsr %0" : : "m"(mxcsr) : "memory");
}

static inline uint8_t *efi_bounce(uint64_t off)
{
	return (uint8_t *)phys_to_virt(g_efi_bounce_phys) + off;
}

static inline uint64_t efi_bounce_fw(uint64_t off)
{
	return g_efi_bounce_phys + off;
}

static uint64_t efi_ucs2_len(const uint16_t *s, uint64_t max)
{
	uint64_t n = 0;

	while (n < max && s[n])
		n++;
	return n;
}

static int efi_region_of(uint64_t addr, uint64_t len)
{
	for (uint32_t i = 0; i < g_efi_boot.n; i++) {
		const struct efi_region *r = &g_efi_boot.r[i];
		uint64_t end = r->phys + r->pages * PAGE_SIZE;

		if (addr >= r->phys && addr < end && len <= end - addr)
			return (int)i;
	}
	return -1;
}

static const char *efi_type_name(uint32_t t)
{
	switch (t) {
	case EFI_RUNTIME_SERVICES_CODE:
		return "code";
	case EFI_RUNTIME_SERVICES_DATA:
		return "data";
	case EFI_MEMORY_MAPPED_IO:
		return "mmio";
	case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
		return "mmio-port";
	case EFI_RESERVED_MEMORY_TYPE:
		return "reserved";
	case EFI_ACPI_MEMORY_NVS:
		return "acpi-nvs";
	case EFI_ACPI_RECLAIM_MEMORY:
		return "acpi-reclaim";
	case EFI_PAL_CODE:
		return "pal-code";
	default:
		return "other";
	}
}

/* ---- the boot information ------------------------------------------- */

void efi_boot_info_save(const boot_info_t *bi)
{
	uint32_t n;

	mm_memset(&g_efi_boot, 0, sizeof(g_efi_boot));
	if (!bi || bi->efi_magic != BOOT_EFI_MAGIC)
		return;
	g_efi_boot.st = bi->efi_system_table;
	g_efi_boot.rt = bi->efi_runtime_services;
	g_efi_boot.memdesc_version = bi->efi_memdesc_version;
	g_efi_boot.memdesc_size = bi->efi_memdesc_size;
	g_efi_boot.fw_revision = bi->efi_fw_revision;
	g_efi_boot.spec_revision = bi->efi_spec_revision;
	for (n = 0; n + 1 < sizeof(g_efi_boot.vendor) && bi->efi_fw_vendor[n];
	     n++) {
		char c = bi->efi_fw_vendor[n];

		g_efi_boot.vendor[n] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}
	g_efi_boot.vendor[n] = 0;
	g_efi_boot.dropped = bi->efi_rt_dropped;
	n = bi->efi_rt_count;
	if (n > BOOT_EFI_RT_MAX)
		n = BOOT_EFI_RT_MAX;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t phys = bi->efi_rt_regions[i].phys;
		uint64_t pages = bi->efi_rt_regions[i].pages;
		struct efi_region *r = &g_efi_boot.r[g_efi_boot.n];

		/* Mappable one-to-one in the lower half, page aligned, not
		 * empty: anything else is a broken map entry. */
		if ((phys & (PAGE_SIZE - 1)) || pages == 0 ||
		    pages > (1ULL << 34) || phys >= (1ULL << 46) ||
		    phys + pages * PAGE_SIZE > (1ULL << 46)) {
			g_efi_boot.dropped++;
			continue;
		}
		r->type = bi->efi_rt_regions[i].type;
		r->phys = phys;
		r->pages = pages;
		r->attr = bi->efi_rt_regions[i].attribute;
		g_efi_boot.n++;
	}
	g_efi_boot.valid = g_efi_boot.rt != 0;
}

const char *efi_fw_vendor(void)
{
	return g_efi_boot.vendor;
}

uint32_t efi_fw_revision(void)
{
	return g_efi_boot.fw_revision;
}

uint32_t efi_spec_revision(void)
{
	return g_efi_boot.spec_revision;
}

int efi_runtime_available(void)
{
	return __atomic_load_n(&g_efi_enabled, __ATOMIC_ACQUIRE);
}

/* ---- the page table --------------------------------------------------- */

#define EFI_PTE_TABLE (PAGE_PRESENT | PAGE_WRITABLE)
#define EFI_2M (2ULL * 1024 * 1024)

/* The table below `table[idx]', made when missing; a 2 MiB leaf there is
 * split into 4 KiB entries with the same flags.  NULL when out of memory. */
static uint64_t *efi_pt_child(uint64_t *table, unsigned idx, int may_split)
{
	uint64_t e = table[idx];

	if (e & PAGE_PRESENT) {
		if (!(e & PAGE_SIZE_FLAG))
			return (uint64_t *)phys_to_virt(e & PTE_ADDR_MASK);
		if (!may_split)
			return NULL;
	}
	uint64_t p = mm_allocate_physical_page();

	if (!p)
		return NULL;
	uint64_t *child = (uint64_t *)phys_to_virt(p);

	mm_memset(child, 0, PAGE_SIZE);
	if (e & PAGE_PRESENT) {
		/* split a large leaf: same flags, PS dropped */
		uint64_t base = e & PTE_ADDR_MASK & ~(EFI_2M - 1);
		uint64_t fl = e & PTE_FLAGS_MASK & ~(uint64_t)PAGE_SIZE_FLAG;

		for (unsigned i = 0; i < 512; i++)
			child[i] = (base + (uint64_t)i * PAGE_SIZE) | fl;
	}
	table[idx] = p | EFI_PTE_TABLE;
	return child;
}

/* Map [phys, phys + pages * 4 KiB) one-to-one with leaf flags `fl',
 * replacing what was there.  2 MiB leaves where both ends allow it. */
static int efi_map_identity(uint64_t phys, uint64_t pages, uint64_t fl)
{
	uint64_t a = phys, end = phys + pages * PAGE_SIZE;

	while (a < end) {
		unsigned i4 = (unsigned)((a >> 39) & 511);
		unsigned i3 = (unsigned)((a >> 30) & 511);
		unsigned i2 = (unsigned)((a >> 21) & 511);
		unsigned i1 = (unsigned)((a >> 12) & 511);
		uint64_t *pdpt = efi_pt_child(g_efi_pml4, i4, 0);
		uint64_t *pd, *pt;

		if (!pdpt)
			return -ENOMEM;
		pd = efi_pt_child(pdpt, i3, 0);
		if (!pd)
			return -ENOMEM;
		if (!(a & (EFI_2M - 1)) && end - a >= EFI_2M &&
		    (!(pd[i2] & PAGE_PRESENT) || (pd[i2] & PAGE_SIZE_FLAG))) {
			pd[i2] = a | fl | PAGE_SIZE_FLAG;
			a += EFI_2M;
			continue;
		}
		pt = efi_pt_child(pd, i2, 1);
		if (!pt)
			return -ENOMEM;
		pt[i1] = a | fl;
		a += PAGE_SIZE;
	}
	return 0;
}

static uint64_t efi_region_flags(const struct efi_region *r)
{
	uint64_t fl = PAGE_PRESENT | PAGE_WRITABLE;
	int uncached;

	switch (r->type) {
	case EFI_MEMORY_MAPPED_IO:
	case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
		uncached = 1;
		break;
	default:
		/* write-back when the firmware allows it, else uncached */
		uncached = !(r->attr & EFI_MEMORY_WB);
		break;
	}
	if (uncached)
		fl |= PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH; /* PAT entry 3: UC */
	if (!r->exec)
		fl |= PAGE_NO_EXECUTE;
	return fl;
}

static int efi_map_region(struct efi_region *r)
{
	return efi_map_identity(r->phys, r->pages, efi_region_flags(r));
}

/* Undo efi_map_identity() for [phys, phys + pages * 4 KiB): the leaves go,
 * the page-table pages stay for the next mapping.  No TLB invalidation is
 * needed: our table is live only inside a call, every call loads it into
 * CR3 afresh, and none of its translations are global. */
static void efi_unmap_identity(uint64_t phys, uint64_t pages)
{
	uint64_t a = phys, end = phys + pages * PAGE_SIZE;

	while (a < end) {
		unsigned i4 = (unsigned)((a >> 39) & 511);
		unsigned i3 = (unsigned)((a >> 30) & 511);
		unsigned i2 = (unsigned)((a >> 21) & 511);
		unsigned i1 = (unsigned)((a >> 12) & 511);
		uint64_t e4 = g_efi_pml4[i4], e3, e2;
		uint64_t *pdpt, *pd, *pt;

		if (!(e4 & PAGE_PRESENT)) {
			a += PAGE_SIZE;
			continue;
		}
		pdpt = (uint64_t *)phys_to_virt(e4 & PTE_ADDR_MASK);
		e3 = pdpt[i3];
		if (!(e3 & PAGE_PRESENT) || (e3 & PAGE_SIZE_FLAG)) {
			a += PAGE_SIZE;
			continue;
		}
		pd = (uint64_t *)phys_to_virt(e3 & PTE_ADDR_MASK);
		e2 = pd[i2];
		if (!(e2 & PAGE_PRESENT)) {
			a += PAGE_SIZE;
			continue;
		}
		if (e2 & PAGE_SIZE_FLAG) {
			/* a 2 MiB leaf: efi_map_identity made one only for a
			 * chunk the range covers whole */
			pd[i2] = 0;
			a = (a & ~(EFI_2M - 1)) + EFI_2M;
			continue;
		}
		pt = (uint64_t *)phys_to_virt(e2 & PTE_ADDR_MASK);
		pt[i1] = 0;
		a += PAGE_SIZE;
	}
}

/* ---- large reads ------------------------------------------------------- */

/* A bounce region of `pages' pages for one call that carries more than the
 * fixed bounce area holds: fresh memory, mapped one-to-one in our table for
 * the duration.  Its physical address (what the firmware is handed), or 0.
 * Under g_efi_lock, like every change to our table. */
static uint64_t efi_big_map(uint64_t pages)
{
	uint64_t phys;

	mutex_assert_held(&g_efi_lock);
	phys = mm_allocate_contiguous_pages(pages);
	if (!phys)
		return 0;
	if (phys + pages * PAGE_SIZE > (1ULL << 46) ||
	    efi_map_identity(phys, pages,
			     PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE)) {
		efi_unmap_identity(phys, pages);
		mm_free_contiguous_pages(phys, pages);
		return 0;
	}
	return phys;
}

static void efi_big_unmap(uint64_t phys, uint64_t pages)
{
	mutex_assert_held(&g_efi_lock);
	efi_unmap_identity(phys, pages);
	mm_free_contiguous_pages(phys, pages);
}

/* ---- the call ------------------------------------------------------- */

/* A call that kept interrupts off on this CPU for longer than this is
 * reported: each new longest one, so a firmware that is always slow says so
 * once or twice and not at every call. */
#define EFI_SLOW_CALL_US 20000ULL
static uint64_t g_efi_slowest_us; /* under g_efi_lock */

static void efi_note_duration(const char *what, uint64_t tsc)
{
	uint64_t hz = lapic_get_tsc_freq();
	uint64_t us;

	if (!hz)
		return; /* no calibrated clock to tell */
	us = tsc / (hz / 1000000ULL ? hz / 1000000ULL : 1);
	if (us < EFI_SLOW_CALL_US || us <= g_efi_slowest_us)
		return;
	g_efi_slowest_us = us;
	kprintf("EFI: %s ran %llu.%03llu ms with interrupts off on cpu %u (longest so far)\n",
		what, (unsigned long long)(us / 1000),
		(unsigned long long)(us % 1000), this_cpu_id());
}

/* One firmware call; g_efi_lock held, g_efi_enabled checked by the caller.
 * Returns the firmware's status, or EFI_ABORTED when the guard caught an
 * exception (the runtime services are then off). */
static efi_status_t efi_rt_call(uint64_t fn, const char *what, uint64_t a1,
				uint64_t a2, uint64_t a3, uint64_t a4,
				uint64_t a5)
{
	uint64_t irq, cr3, after, t0, t1;
	efi_status_t st;

	mutex_assert_held(&g_efi_lock);
	percpu_preempt_disable();
	kernel_fpu_begin();
	irq = local_irq_save();
	efi_fpu_reset();
	cr3 = efi_read_cr3();
	/* The upper half of the address space this CPU is on -- the kernel,
	 * the direct map, the handlers -- exactly as it is now. */
	{
		const uint64_t *cur =
			(const uint64_t *)phys_to_virt(cr3 & PTE_ADDR_MASK);

		for (unsigned i = 256; i < 512; i++)
			g_efi_pml4[i] = cur[i];
	}
	g_efi_faulted = 0;
	__atomic_store_n(&g_efi_in_call, 1, __ATOMIC_SEQ_CST);
	t0 = timer_rdtsc();
	efi_write_cr3(g_efi_pml4_phys);
	st = efi_call_thunk(fn, a1, a2, a3, a4, a5, g_efi_stack_top);
	efi_write_cr3(cr3);
	t1 = timer_rdtsc();
	__atomic_store_n(&g_efi_in_call, 0, __ATOMIC_SEQ_CST);
	after = efi_read_flags();
	local_irq_restore(irq);
	kernel_fpu_end();
	percpu_preempt_enable();

	efi_note_duration(what, t1 - t0);

	if ((after & 0x200) && !g_efi_irq_warned) {
		/* The firmware enabled interrupts and left them on: whatever
		 * arrived meanwhile ran on our table.  Worth knowing, once. */
		g_efi_irq_warned = 1;
		kprintf("EFI: warning: %s returned with interrupts enabled\n",
			what);
	}
	if (g_efi_faulted) {
		__atomic_store_n(&g_efi_enabled, 0, __ATOMIC_RELEASE);
		kprintf("EFI: %s raised exception %llu (error code %llx) at %llx, cr2 %llx -- runtime services disabled\n",
			what, (unsigned long long)g_efi_fault_vector,
			(unsigned long long)g_efi_fault_error,
			(unsigned long long)g_efi_fault_rip,
			(unsigned long long)g_efi_fault_cr2);
		return EFI_ABORTED;
	}
	return st;
}

int efi_rt_fault(uint64_t vector, uint64_t error_code, uint64_t *rip,
		 uint64_t *rflags, uint64_t *rax)
{
	if (!__atomic_load_n(&g_efi_in_call, __ATOMIC_ACQUIRE))
		return 0;
	/* Only the calling CPU runs on our table. */
	if ((efi_read_cr3() & PTE_ADDR_MASK) != g_efi_pml4_phys)
		return 0;
	/* Debug traps, NMIs and machine checks are not the firmware's. */
	if (vector == 1 || vector == 2 || vector == 3 || vector == 18)
		return 0;
	/* Nor an exception in kernel code: the firmware runs in the lower
	 * half (its regions are mapped one-to-one there), the kernel in the
	 * upper.  An interrupt handler that ran because the firmware turned
	 * interrupts on executes kernel code on our table, and a fault in it
	 * is the kernel's to report, not a reason to abandon the call. */
	if (*rip >= 0xffff800000000000ULL)
		return 0;
	g_efi_fault_vector = vector;
	g_efi_fault_error = error_code;
	g_efi_fault_rip = *rip;
	g_efi_fault_cr2 = vector == 14 ? efi_read_cr2() : 0;
	g_efi_faulted = 1;
	*rip = (uint64_t)efi_call_thunk_resume;
	*rax = EFI_ABORTED;
	/* No trap flag, string direction up, no SMAP override; interrupts
	 * as efi_rt_call set them for the call (off), even when the
	 * firmware had turned them on. */
	*rflags &= ~(0x100ULL | 0x200ULL | 0x400ULL | 0x40000ULL);
	return 1;
}

/* ---- the services --------------------------------------------------- */

int efi_status_to_errno(efi_status_t s)
{
	switch (s) {
	case EFI_SUCCESS:
		return 0;
	case EFI_NOT_FOUND:
		return -ENOENT;
	case EFI_INVALID_PARAMETER:
	case EFI_BAD_BUFFER_SIZE:
		return -EINVAL;
	case EFI_BUFFER_TOO_SMALL:
		return -ENOSPC;
	case EFI_OUT_OF_RESOURCES:
		return -ENOSPC;
	case EFI_WRITE_PROTECTED:
		return -EROFS;
	case EFI_SECURITY_VIOLATION:
	case EFI_ACCESS_DENIED:
		return -EACCES;
	case EFI_UNSUPPORTED:
		return -EOPNOTSUPP;
	case EFI_NOT_READY:
	case EFI_TIMEOUT:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

/* Copy name + GUID into the bounce area; EFI_INVALID_PARAMETER when the
 * name is too long or not terminated. */
static efi_status_t efi_put_name(const uint16_t *name, const efi_guid_t *guid)
{
	uint64_t n = efi_ucs2_len(name, EFI_VAR_NAME_MAX);

	if (n >= EFI_VAR_NAME_MAX)
		return EFI_INVALID_PARAMETER;
	mm_memcpy(efi_bounce(EB_NAME), name, (n + 1) * 2);
	mm_memcpy(efi_bounce(EB_GUID), guid, 16);
	return EFI_SUCCESS;
}

efi_status_t efi_get_variable(const uint16_t *name, const efi_guid_t *guid,
			      uint32_t *attr, uint64_t *size, void *data)
{
	uint64_t want, got, buf_fw, big = 0, big_pages = 0;
	const uint8_t *buf;
	efi_status_t st;

	if (!name || !guid || !size || (*size && !data))
		return EFI_INVALID_PARAMETER;
	want = *size > EFI_VAR_READ_MAX ? EFI_VAR_READ_MAX : *size;
	mutex_lock(&g_efi_lock);
	if (!g_efi_enabled) {
		mutex_unlock(&g_efi_lock);
		return EFI_UNSUPPORTED;
	}
	st = efi_put_name(name, guid);
	if (st != EFI_SUCCESS) {
		mutex_unlock(&g_efi_lock);
		return st;
	}
	/* Never NULL, even for a size query: some firmware refuses a NULL
	 * buffer whatever the size. */
	buf = efi_bounce(EFI_BOUNCE_DATA_OFF);
	buf_fw = efi_bounce_fw(EFI_BOUNCE_DATA_OFF);
	if (want > EFI_VAR_DATA_MAX) {
		/* More than the fixed area holds: a region of the size asked
		 * for, for this call only. */
		big_pages = (want + PAGE_SIZE - 1) / PAGE_SIZE;
		big = efi_big_map(big_pages);
		if (!big) {
			mutex_unlock(&g_efi_lock);
			return EFI_OUT_OF_RESOURCES;
		}
		buf = (const uint8_t *)phys_to_virt(big);
		buf_fw = big;
	}
	*(uint32_t *)efi_bounce(EB_ATTR) = 0;
	*(uint64_t *)efi_bounce(EB_SIZE) = want;
	st = efi_rt_call(g_rt_get_variable, "GetVariable",
			 efi_bounce_fw(EB_NAME), efi_bounce_fw(EB_GUID),
			 efi_bounce_fw(EB_ATTR), efi_bounce_fw(EB_SIZE), buf_fw);
	got = *(uint64_t *)efi_bounce(EB_SIZE);
	if (st == EFI_SUCCESS || st == EFI_BUFFER_TOO_SMALL) {
		if (attr)
			*attr = *(uint32_t *)efi_bounce(EB_ATTR);
		*size = got;
		/* Too small for a variable that fits what was offered: the
		 * firmware contradicts itself.  (One larger than
		 * EFI_VAR_READ_MAX stays EFI_BUFFER_TOO_SMALL with its size,
		 * whatever room the caller has.) */
		if (st == EFI_BUFFER_TOO_SMALL && got <= want)
			st = EFI_DEVICE_ERROR; /* the firmware contradicts itself */
		if (st == EFI_SUCCESS) {
			if (got > want) {
				st = EFI_DEVICE_ERROR;
			} else if (got) {
				mm_memcpy(data, buf, got);
			}
		}
	}
	if (big)
		efi_big_unmap(big, big_pages);
	mutex_unlock(&g_efi_lock);
	return st;
}

efi_status_t efi_get_next_variable_name(uint64_t *name_size, uint16_t *name,
					efi_guid_t *guid)
{
	uint64_t cap, got;
	efi_status_t st;

	if (!name_size || !name || !guid || *name_size < 2)
		return EFI_INVALID_PARAMETER;
	cap = *name_size > EFI_VAR_NAME_MAX * 2 ? EFI_VAR_NAME_MAX * 2 :
						 *name_size;
	if (efi_ucs2_len(name, cap / 2) >= cap / 2)
		return EFI_INVALID_PARAMETER; /* not terminated */
	mutex_lock(&g_efi_lock);
	if (!g_efi_enabled) {
		mutex_unlock(&g_efi_lock);
		return EFI_UNSUPPORTED;
	}
	mm_memset(efi_bounce(EB_NAME), 0, EFI_VAR_NAME_MAX * 2);
	mm_memcpy(efi_bounce(EB_NAME), name,
		  (efi_ucs2_len(name, cap / 2) + 1) * 2);
	mm_memcpy(efi_bounce(EB_GUID), guid, 16);
	*(uint64_t *)efi_bounce(EB_SIZE) = cap;
	st = efi_rt_call(g_rt_get_next_variable_name, "GetNextVariableName",
			 efi_bounce_fw(EB_SIZE), efi_bounce_fw(EB_NAME),
			 efi_bounce_fw(EB_GUID), 0, 0);
	got = *(uint64_t *)efi_bounce(EB_SIZE);
	if (st == EFI_SUCCESS) {
		if (got > cap || got < 2) {
			st = EFI_DEVICE_ERROR;
		} else {
			mm_memcpy(name, efi_bounce(EB_NAME), got);
			name[got / 2 - 1] = 0; /* whatever the firmware did */
			mm_memcpy(guid, efi_bounce(EB_GUID), 16);
			*name_size = got;
		}
	} else if (st == EFI_BUFFER_TOO_SMALL) {
		*name_size = got;
	}
	mutex_unlock(&g_efi_lock);
	return st;
}

efi_status_t efi_set_variable(const uint16_t *name, const efi_guid_t *guid,
			      uint32_t attr, uint64_t size, const void *data)
{
	efi_status_t st;

	if (!name || !guid || (size && !data) || size > EFI_VAR_DATA_MAX)
		return EFI_INVALID_PARAMETER;
	mutex_lock(&g_efi_lock);
	if (!g_efi_enabled) {
		mutex_unlock(&g_efi_lock);
		return EFI_UNSUPPORTED;
	}
	st = efi_put_name(name, guid);
	if (st == EFI_SUCCESS) {
		if (size)
			mm_memcpy(efi_bounce(EFI_BOUNCE_DATA_OFF), data, size);
		st = efi_rt_call(g_rt_set_variable, "SetVariable",
				 efi_bounce_fw(EB_NAME), efi_bounce_fw(EB_GUID),
				 attr, size,
				 size ? efi_bounce_fw(EFI_BOUNCE_DATA_OFF) : 0);
	}
	mutex_unlock(&g_efi_lock);
	return st;
}

efi_status_t efi_query_variable_info(uint32_t attr, uint64_t *max_storage,
				     uint64_t *remaining, uint64_t *max_size)
{
	efi_status_t st;

	mutex_lock(&g_efi_lock);
	if (!g_efi_enabled || !g_rt_query_variable_info) {
		mutex_unlock(&g_efi_lock);
		return EFI_UNSUPPORTED;
	}
	mm_memset(efi_bounce(EB_Q0), 0, 24);
	st = efi_rt_call(g_rt_query_variable_info, "QueryVariableInfo", attr,
			 efi_bounce_fw(EB_Q0), efi_bounce_fw(EB_Q1),
			 efi_bounce_fw(EB_Q2), 0);
	if (st == EFI_SUCCESS) {
		if (max_storage)
			*max_storage = *(uint64_t *)efi_bounce(EB_Q0);
		if (remaining)
			*remaining = *(uint64_t *)efi_bounce(EB_Q1);
		if (max_size)
			*max_size = *(uint64_t *)efi_bounce(EB_Q2);
	}
	mutex_unlock(&g_efi_lock);
	return st;
}

efi_status_t efi_get_time(struct efi_time *t)
{
	efi_status_t st;

	if (!t)
		return EFI_INVALID_PARAMETER;
	mutex_lock(&g_efi_lock);
	if (!g_efi_enabled || !g_rt_get_time) {
		mutex_unlock(&g_efi_lock);
		return EFI_UNSUPPORTED;
	}
	mm_memset(efi_bounce(EB_TIME), 0, EB_END - EB_TIME);
	st = efi_rt_call(g_rt_get_time, "GetTime", efi_bounce_fw(EB_TIME), 0,
			 0, 0, 0);
	if (st == EFI_SUCCESS)
		mm_memcpy(t, efi_bounce(EB_TIME), sizeof(*t));
	mutex_unlock(&g_efi_lock);
	return st;
}

/* ---- set-up ------------------------------------------------------------ */

/* `efi=noruntime' (or `noefi') among the words of the command line. */
static int efi_cmdline_off(void)
{
	const char *s = storage_boot_cmdline();

	while (s && *s) {
		const char *w;
		size_t n;

		while (*s == ' ')
			s++;
		w = s;
		while (*s && *s != ' ')
			s++;
		n = (size_t)(s - w);
		if ((n == 13 && !kstrncmp(w, "efi=noruntime", 13)) ||
		    (n == 5 && !kstrncmp(w, "noefi", 5)))
			return 1;
	}
	return 0;
}

/* Read the first `len' bytes of the runtime services table through our
 * page table (they lie in a runtime region, checked by the caller) into
 * `out'; the rest of RT_TABLE_BYTES reads as zero. */
static void efi_read_rt_table(uint8_t *out, uint64_t len)
{
	uint64_t irq = local_irq_save();
	uint64_t cr3 = efi_read_cr3();
	const uint64_t *cur = (const uint64_t *)phys_to_virt(cr3 & PTE_ADDR_MASK);

	for (unsigned i = 256; i < 512; i++)
		g_efi_pml4[i] = cur[i];
	efi_write_cr3(g_efi_pml4_phys);
	mm_memset(out, 0, RT_TABLE_BYTES);
	mm_memcpy(out, (const void *)g_efi_boot.rt, len);
	efi_write_cr3(cr3);
	local_irq_restore(irq);
}

static uint64_t rd64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* An entry point must lie in a runtime region; one that lies in a region
 * mapped without execute permission makes that region executable. */
static int efi_check_entry(uint64_t fn, const char *what)
{
	int i = efi_region_of(fn, 1);

	if (fn == 0 || i < 0) {
		kprintf("EFI: %s entry point %llx is outside the runtime regions\n",
			what, (unsigned long long)fn);
		return -1;
	}
	if (!g_efi_boot.r[i].exec) {
		kprintf("EFI: %s entry point %llx lies in a %s region: mapping it executable\n",
			what, (unsigned long long)fn,
			efi_type_name(g_efi_boot.r[i].type));
		g_efi_boot.r[i].exec = 1;
		if (efi_map_region(&g_efi_boot.r[i]))
			return -1;
	}
	return 0;
}

void efi_init(void)
{
	uint64_t pool_lo, pool_hi, code = 0, data = 0, mmio = 0;
	uint8_t rt[RT_TABLE_BYTES];
	uint64_t rt_len = RT_TABLE_BYTES;
	efi_status_t st;
	int ri;

	if (!g_efi_boot.valid) {
		kprintf("EFI: the bootloader passed no runtime services\n");
		return;
	}
	kprintf("EFI: UEFI %u.%u, firmware \"%s\" revision %x, %u runtime regions\n",
		g_efi_boot.spec_revision >> 16,
		(g_efi_boot.spec_revision & 0xffff) / 10,
		g_efi_boot.vendor[0] ? g_efi_boot.vendor : "?",
		g_efi_boot.fw_revision, g_efi_boot.n);
	if (efi_cmdline_off()) {
		kprintf("EFI: runtime services off (efi=noruntime)\n");
		return;
	}
	if (g_efi_boot.dropped)
		kprintf("EFI: note: %u runtime region(s) were not passed on; a call that needs one is caught by the guard\n",
			g_efi_boot.dropped);

	/* The kernel's page-table pool is carved out by address, without
	 * consulting the firmware's map: the firmware's regions must lie
	 * elsewhere, or the first page table written there corrupts it. */
	mm_pt_pool_range(&pool_lo, &pool_hi);
	for (uint32_t i = 0; i < g_efi_boot.n; i++) {
		const struct efi_region *r = &g_efi_boot.r[i];
		uint64_t end = r->phys + r->pages * PAGE_SIZE;

		if (r->phys < pool_hi && end > pool_lo) {
			kprintf("EFI: runtime region %llx-%llx overlaps the kernel page-table pool %llx-%llx -- runtime services disabled\n",
				(unsigned long long)r->phys,
				(unsigned long long)end,
				(unsigned long long)pool_lo,
				(unsigned long long)pool_hi);
			return;
		}
	}
	ri = efi_region_of(g_efi_boot.rt, RT_TABLE_BYTES);
	if (ri < 0) {
		/* At least the header and the variable services must be
		 * inside; QueryVariableInfo is checked against HeaderSize. */
		rt_len = RT_SET_VARIABLE + 8;
		ri = efi_region_of(g_efi_boot.rt, rt_len);
		if (ri < 0) {
			kprintf("EFI: the runtime services table %llx is outside the runtime regions -- not used\n",
				(unsigned long long)g_efi_boot.rt);
			return;
		}
	}

	/* The page table. */
	g_efi_pml4_phys = mm_allocate_physical_page();
	if (!g_efi_pml4_phys) {
		kprintf("EFI: out of memory for the page table\n");
		return;
	}
	g_efi_pml4 = (uint64_t *)phys_to_virt(g_efi_pml4_phys);
	mm_memset(g_efi_pml4, 0, PAGE_SIZE);
	for (uint32_t i = 0; i < g_efi_boot.n; i++) {
		struct efi_region *r = &g_efi_boot.r[i];

		r->exec = r->type == EFI_RUNTIME_SERVICES_CODE;
		if (efi_map_region(r)) {
			kprintf("EFI: out of memory for the page table\n");
			return;
		}
		if (r->type == EFI_RUNTIME_SERVICES_CODE)
			code += r->pages;
		else if (r->type == EFI_MEMORY_MAPPED_IO ||
			 r->type == EFI_MEMORY_MAPPED_IO_PORT_SPACE)
			mmio += r->pages;
		else
			data += r->pages;
	}

	/* Guard page, stack, bounce area. */
	g_efi_area_phys = mm_allocate_contiguous_pages(EFI_AREA_PAGES);
	if (!g_efi_area_phys) {
		kprintf("EFI: out of memory for the call area\n");
		return;
	}
	if (efi_region_of(g_efi_area_phys, EFI_AREA_PAGES * PAGE_SIZE) >= 0) {
		/* cannot happen: those regions are reserved */
		kprintf("EFI: the call area landed in firmware memory -- not used\n");
		return;
	}
	mm_memset(phys_to_virt(g_efi_area_phys), 0, EFI_AREA_PAGES * PAGE_SIZE);
	g_efi_stack_top = g_efi_area_phys + (1 + EFI_STACK_PAGES) * PAGE_SIZE;
	g_efi_bounce_phys = g_efi_stack_top;
	if (efi_map_identity(g_efi_area_phys + PAGE_SIZE,
			     EFI_STACK_PAGES + EFI_BOUNCE_PAGES,
			     PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE)) {
		kprintf("EFI: out of memory for the page table\n");
		return;
	}
	if (g_efi_area_phys + EFI_AREA_PAGES * PAGE_SIZE > 0x100000000ULL)
		kprintf("EFI: note: the call area is above 4 GiB (%llx)\n",
			(unsigned long long)g_efi_area_phys);
	BUILD_BUG_ON(EB_END > PAGE_SIZE);

	/* The table of entry points. */
	efi_read_rt_table(rt, rt_len);
	if (rd64(rt) != RT_SIGNATURE) {
		kprintf("EFI: no runtime services table at %llx (signature %llx) -- not used\n",
			(unsigned long long)g_efi_boot.rt,
			(unsigned long long)rd64(rt));
		return;
	}
	{
		uint32_t rev = rd32(rt + RT_HDR_REVISION);
		uint32_t hsize = rd32(rt + RT_HDR_SIZE);

		if (hsize < RT_SET_VARIABLE + 8) {
			kprintf("EFI: runtime services table too short (%u bytes) -- not used\n",
				hsize);
			return;
		}
		g_rt_get_time = rd64(rt + RT_GET_TIME);
		g_rt_get_variable = rd64(rt + RT_GET_VARIABLE);
		g_rt_get_next_variable_name =
			rd64(rt + RT_GET_NEXT_VARIABLE_NAME);
		g_rt_set_variable = rd64(rt + RT_SET_VARIABLE);
		g_rt_query_variable_info = 0;
		if (rev >= 0x00020000 && hsize >= RT_TABLE_BYTES &&
		    rt_len == RT_TABLE_BYTES)
			g_rt_query_variable_info =
				rd64(rt + RT_QUERY_VARIABLE_INFO);
	}
	if (efi_check_entry(g_rt_get_variable, "GetVariable") ||
	    efi_check_entry(g_rt_get_next_variable_name,
			    "GetNextVariableName") ||
	    efi_check_entry(g_rt_set_variable, "SetVariable"))
		return;
	/* The optional ones are left out when they look wrong. */
	if (efi_check_entry(g_rt_get_time, "GetTime"))
		g_rt_get_time = 0;
	if (g_rt_query_variable_info &&
	    efi_check_entry(g_rt_query_variable_info, "QueryVariableInfo"))
		g_rt_query_variable_info = 0;

	kprintf("EFI: runtime regions mapped 1:1: %llu KiB code, %llu KiB data, %llu KiB I/O; call area at %llx\n",
		(unsigned long long)code * 4, (unsigned long long)data * 4,
		(unsigned long long)mmio * 4,
		(unsigned long long)g_efi_area_phys);

	/* A first call, the cheapest one: the first variable's name. */
	__atomic_store_n(&g_efi_enabled, 1, __ATOMIC_RELEASE);
	{
		uint16_t name[EFI_VAR_NAME_MAX];
		efi_guid_t guid;
		uint64_t size = sizeof(name);

		name[0] = 0;
		mm_memset(&guid, 0, sizeof(guid));
		st = efi_get_next_variable_name(&size, name, &guid);
		if (st != EFI_SUCCESS && st != EFI_NOT_FOUND) {
			kprintf("EFI: the first variable call failed (status %llx) -- runtime services disabled\n",
				(unsigned long long)st);
			__atomic_store_n(&g_efi_enabled, 0, __ATOMIC_RELEASE);
			return;
		}
	}
	{
		uint64_t total = 0, left = 0, max = 0;

		st = efi_query_variable_info(EFI_VARIABLE_NON_VOLATILE |
						     EFI_VARIABLE_BOOTSERVICE_ACCESS |
						     EFI_VARIABLE_RUNTIME_ACCESS,
					     &total, &left, &max);
		if (st == EFI_SUCCESS)
			kprintf("EFI: variable store %llu KiB, %llu KiB free, largest variable %llu bytes\n",
				(unsigned long long)total / 1024,
				(unsigned long long)left / 1024,
				(unsigned long long)max);
		else if (!efi_runtime_available())
			return; /* the guard caught it and said why */
	}
	kprintf("EFI: runtime services available (physical mode)\n");
}
