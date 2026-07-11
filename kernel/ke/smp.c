// LikeOS-64 - SMP (Symmetric Multi-Processing) Implementation
// AP startup, CPU synchronization, and SMP management

#include <kernel/ke/smp.h>
#include <kernel/hal/acpi.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/cpu_pstate.h>
#include <kernel/dev/video/fb_optimize.h> // fb_pat_program_wc_this_cpu (per-CPU PAT)
#include <kernel/ke/percpu.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/sched.h> // For sched_enable_smp()
#include <kernel/ke/timer.h> // For timer_rdtsc()
#include <kernel/uapi/bug.h>

// ============================================================================
// External Trampoline Symbols
// ============================================================================

extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

// Offsets within trampoline (must match ap_trampoline.S)
#define AP_TRAMPOLINE_PML4_OFFSET 0x108
#define AP_TRAMPOLINE_STACK_OFFSET 0x110
#define AP_TRAMPOLINE_CPU_OFFSET 0x118
#define AP_TRAMPOLINE_ENTRY_OFFSET 0x120

// ============================================================================
// SMP State
// ============================================================================

static smp_state_t g_smp_state = SMP_STATE_BSP_ONLY;
static volatile uint32_t g_aps_started = 0;
static uint32_t g_cpu_count = 1; // At least BSP
static smp_barrier_t g_startup_barrier;

// UP (Uniprocessor) mode flag - when set to 1, spinlocks skip spinning
// since there's only one CPU and spinning would cause deadlocks.
// This is global so spinlock inline functions in sched.h can access it.
volatile uint32_t g_smp_up_mode =
	1; // Default to UP mode until SMP is initialized

// AP trampoline address (set from boot_info or fallback to default)
static uint64_t g_ap_trampoline_addr = 0;

// Per-AP stacks
static uint8_t *g_ap_stacks[MAX_CPUS] = { 0 };

// Volatile flag for AP startup synchronization
static volatile int g_ap_ready = 0;

// ============================================================================
// AP Entry Point
// ============================================================================

// Enable SSE/FPU on this CPU
static inline void ap_enable_sse(void)
{
	uint64_t cr0, cr4;

	// Read CR0
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));

	// Match BSP CR0 settings (0x80010033):
	// Clear CD (bit 30) - enable caching (INIT sets CD=1)
	// Clear NW (bit 29) - enable write-through (INIT sets NW=1)
	// Clear EM (bit 2)  - disable x87 emulation
	// Set   NE (bit 5)  - native FPU error reporting
	// Set   MP (bit 1)  - enable FPU monitoring
	cr0 &= ~((1ULL << 30) | (1ULL << 29) | (1ULL << 2)); // Clear CD, NW, EM
	cr0 |= (1ULL << 5) | (1ULL << 1); // Set NE, MP

	__asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

	// Read CR4
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

	// Match BSP CR4 settings (0x6e8):
	// Set OSFXSR (bit 9)    - enable FXSAVE/FXRSTOR
	// Set OSXMMEXCPT (bit 10) - enable SIMD exceptions
	// Set MCE (bit 6)       - Machine Check Enable
	// Set DE  (bit 3)       - Debugging Extensions (DR4/DR5 trapping)
	cr4 |= (1ULL << 10) | (1ULL << 9) | (1ULL << 6) | (1ULL << 3);

	__asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

	// Initialize FPU
	__asm__ volatile("fninit");
}

__no_stack_protector void ap_entry(void)
{
	// CRITICAL: Enable SSE/FPU first, before any code that might use SSE
	// (such as optimized memcpy in kernel functions)
	ap_enable_sse();

	// CRITICAL: Load kernel's GDT and IDT first thing!
	// The AP is currently using the trampoline's minimal GDT but with
	// KERNEL-COMPATIBLE selectors (CS=0x08, DS/SS=0x10) so no far jump needed.
	// Just load the kernel GDT/IDT and we're good to go.

	// Load kernel GDT
	void *gdt_desc = gdt_get_descriptor();
	__asm__ volatile("lgdt (%0)" : : "r"(gdt_desc) : "memory");

	// No far jump needed! The trampoline uses CS=0x08 and DS/SS=0x10
	// which are exactly the same selectors as the kernel GDT.
	// The selectors point to equivalent descriptors, so loading the
	// kernel GDT seamlessly replaces the trampoline GDT.

	// Load kernel IDT
	void *idt_desc = interrupts_get_idt_descriptor();
	__asm__ volatile("lidt (%0)" : : "r"(idt_desc) : "memory");

	// Now we can safely use the kernel's exception handlers

	// We're now running on an AP in long mode with kernel GDT/IDT
	// Get our CPU ID from the trampoline data
	uint32_t cpu_id = *(volatile uint32_t *)(phys_to_virt(
		g_ap_trampoline_addr + AP_TRAMPOLINE_CPU_OFFSET));
	BUG_ON(cpu_id == 0); /* AP with cpu_id==0 would alias the BSP */
	BUG_ON(cpu_id >= MAX_CPUS); /* AP cpu_id out of percpu array bounds */

	// Get our APIC ID via CPUID (safe before lapic_init)
	// We use CPUID because LAPIC MMIO access may not be safe until lapic_init()
	uint32_t apic_id = lapic_get_id_cpuid();

	// Initialize per-CPU data for this AP
	percpu_init_cpu(cpu_id, apic_id);

	// Initialize per-CPU TSS (each AP needs its own TSS for RSP0)
	tss_init_ap(cpu_id);

	// Enable NX bit in EFER (per-CPU MSR)
	mm_enable_nx();

	// Enable SMEP/SMAP (per-CPU CR4 bits)
	mm_enable_smep_smap();

	// Program IA32_PAT entry 1 = WC (per-CPU MSR; must match the BSP or the
	// framebuffer runs effective-UC on this AP -- ~90x slower flushes).
	fb_pat_program_wc_this_cpu();

	// Request max-performance frequency behavior on this AP (per-CPU HWP MSR).
	cpu_pstate_init(0);

	// Initialize SYSCALL/SYSRET MSRs (STAR, LSTAR, SFMASK, EFER.SCE are per-CPU)
	// Without this, any 'syscall' instruction on this AP would cause #UD
	mm_initialize_syscall();

	// Initialize LAPIC for this CPU
	lapic_init();

	// Initialize per-CPU scheduler (creates idle task, sets current_task)
	sched_init_ap(cpu_id);

	// Signal that we're ready
	__atomic_fetch_add(&g_aps_started, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&g_ap_ready, 1, __ATOMIC_SEQ_CST);

	smp_dbg("SMP: AP %u started (APIC ID %u)\n", cpu_id, apic_id);

	// Enable interrupts
	__asm__ volatile("sti");

	// Start LAPIC timer for this CPU
	lapic_timer_start(100); // 100 Hz

	// Enter idle loop - the scheduler/timer will preempt us when work arrives.
	// When a task is enqueued to our run queue (by fork, wake, or load balance),
	// the enqueuer sends a reschedule IPI which wakes us from HLT, and the
	// next timer tick will call sched_preempt() to switch to the new task.
	while (1) {
		__asm__ volatile("sti; hlt");
	}
}

// ============================================================================
// SMP Initialization
// ============================================================================

void smp_init(uint64_t trampoline_addr)
{
	smp_dbg("SMP: Initializing...\n");

	// Set AP trampoline address (from bootloader or fallback to default)
	if (trampoline_addr != 0 && trampoline_addr < 0x100000 &&
	    (trampoline_addr & 0xFFF) == 0) {
		g_ap_trampoline_addr = trampoline_addr;
		smp_dbg("SMP: Using bootloader-provided trampoline at 0x%lx\n",
			trampoline_addr);
	} else {
		g_ap_trampoline_addr = AP_TRAMPOLINE_ADDR_DEFAULT;
		if (trampoline_addr != 0) {
			smp_dbg("SMP: Invalid trampoline address 0x%lx, using default 0x%lx\n",
				trampoline_addr, g_ap_trampoline_addr);
		} else {
			smp_dbg("SMP: Using default trampoline address 0x%lx\n",
				g_ap_trampoline_addr);
		}
	}

	// Get CPU count from ACPI
	g_cpu_count = acpi_get_cpu_count();
	if (g_cpu_count == 0) {
		g_cpu_count = 1; // At least BSP
	}

	if (g_cpu_count > MAX_CPUS) {
		smp_dbg("SMP: Limiting CPU count from %u to %u\n", g_cpu_count,
			MAX_CPUS);
		g_cpu_count = MAX_CPUS;
	}

	smp_dbg("SMP: %u CPU(s) detected\n", g_cpu_count);

	// Set UP mode flag based on CPU count
	// When only one CPU is present, spinlocks should not spin
	g_smp_up_mode = (g_cpu_count == 1) ? 1 : 0;

	// Initialize BSP's LAPIC
	lapic_init();

	// Calibrate LAPIC timer on BSP before starting any APs.
	// This populates the global lapic_timer_freq so that APs simply
	// reuse the cached value — no per-AP calibration, no CMOS/PIT
	// contention, no races.
	lapic_timer_calibrate();

	// Update BSP's per-CPU data with APIC ID
	percpu_t *bsp = this_cpu();
	bsp->apic_id = lapic_get_id();

	// Enable SMP mode in scheduler (use per-CPU current task)
	sched_enable_smp();

	if (g_cpu_count == 1) {
		smp_dbg("SMP: Single CPU system, no APs to start\n");
		g_smp_state = SMP_STATE_RUNNING;
		return;
	}

	// Initialize startup barrier
	smp_barrier_init(&g_startup_barrier, g_cpu_count);

	g_smp_state = SMP_STATE_STARTING_APS;
}

void smp_boot_aps(void)
{
	if (g_cpu_count <= 1) {
		return;
	}

	smp_dbg("SMP: Starting %u Application Processor(s)...\n",
		g_cpu_count - 1);

	// Copy trampoline code to low memory (at bootloader-reserved address)
	size_t trampoline_size =
		(size_t)(ap_trampoline_end - ap_trampoline_start);
	void *trampoline_dest = phys_to_virt(g_ap_trampoline_addr);
	mm_memcpy(trampoline_dest, ap_trampoline_start, trampoline_size);

	// CRITICAL: Identity-map the trampoline page so APs can execute after enabling paging
	// The kernel removed identity mapping earlier, but APs need it to complete the mode switch
	if (!mm_identity_map_for_smp(g_ap_trampoline_addr,
				     trampoline_size + 0x200)) {
		kprintf("SMP: ERROR: Failed to identity-map trampoline!\n");
		return;
	}

	smp_dbg("SMP: Trampoline copied to 0x%lx, size=%u bytes\n",
		g_ap_trampoline_addr, (uint32_t)trampoline_size);

	// Get PML4 physical address for APs (same as BSP)
	uint64_t pml4_phys;
	__asm__ volatile("mov %%cr3, %0" : "=r"(pml4_phys));
	pml4_phys &= 0x000FFFFFFFFFF000ULL;
	BUG_ON(pml4_phys == 0);
	BUG_ON(pml4_phys & (PAGE_SIZE - 1)); /* PML4 must be page-aligned */

	smp_dbg("SMP: BSP PML4 physical address = 0x%lx\n", pml4_phys);

	// Store PML4 address in trampoline
	volatile uint64_t *pml4_ptr = (volatile uint64_t *)(phys_to_virt(
		g_ap_trampoline_addr + AP_TRAMPOLINE_PML4_OFFSET));
	*pml4_ptr = pml4_phys;

	// Memory barrier to ensure write is visible
	__asm__ volatile("mfence" ::: "memory");

	// Verify the write
	uint64_t verify = *pml4_ptr;
	smp_dbg("SMP: PML4 written to 0x%lx, readback = 0x%lx\n",
		g_ap_trampoline_addr + AP_TRAMPOLINE_PML4_OFFSET, verify);

	if (verify != pml4_phys) {
		kprintf("SMP: ERROR: PML4 write verification failed!\n");
		return;
	}

	// Store entry point address
	*(volatile uint64_t *)(phys_to_virt(g_ap_trampoline_addr +
					    AP_TRAMPOLINE_ENTRY_OFFSET)) =
		(uint64_t)ap_entry;
	__asm__ volatile("mfence" ::: "memory");

	// Start each AP
	acpi_info_t *acpi_info = acpi_get_info();
	BUG_ON(acpi_info == NULL);
	uint32_t ap_index = 1; // Skip BSP (index 0)

	for (uint32_t i = 0; i < acpi_info->cpu_count && ap_index < g_cpu_count;
	     i++) {
		cpu_info_t *cpu = &acpi_info->cpus[i];

		// Skip BSP
		if (cpu->bsp || cpu->apic_id == this_cpu()->apic_id) {
			continue;
		}

		// Skip disabled CPUs
		if (!cpu->enabled && !cpu->online_capable) {
			continue;
		}

		smp_dbg("SMP: Starting AP %u (APIC ID %u)...\n", ap_index,
			cpu->apic_id);

		// Allocate stack for this AP
		g_ap_stacks[ap_index] = (uint8_t *)kalloc(AP_STACK_SIZE);
		if (!g_ap_stacks[ap_index]) {
			kprintf("SMP: Failed to allocate stack for AP %u\n",
				ap_index);
			continue;
		}

		// Store stack pointer (stack grows down, so point to top)
		uint64_t stack_top =
			(uint64_t)(g_ap_stacks[ap_index] + AP_STACK_SIZE);
		stack_top &= ~0xFULL; // 16-byte align
		*(volatile uint64_t *)(phys_to_virt(
			g_ap_trampoline_addr + AP_TRAMPOLINE_STACK_OFFSET)) =
			stack_top;

		// Store CPU ID
		*(volatile uint32_t *)(phys_to_virt(g_ap_trampoline_addr +
						    AP_TRAMPOLINE_CPU_OFFSET)) =
			ap_index;

		// Reset ready flag
		__atomic_store_n(&g_ap_ready, 0, __ATOMIC_SEQ_CST);

		// Send INIT IPI
		lapic_send_init(cpu->apic_id);

		// Wait 10ms (use TSC-based delay — PIT is unreliable on some hardware)
		lapic_delay_ms(10);

		// Send first SIPI (vector = page number)
		lapic_send_sipi(cpu->apic_id, g_ap_trampoline_addr >> 12);

		// Wait 200us
		lapic_delay_us(200);

		// If AP hasn't started, send second SIPI
		if (!__atomic_load_n(&g_ap_ready, __ATOMIC_SEQ_CST)) {
			lapic_send_sipi(cpu->apic_id,
					g_ap_trampoline_addr >> 12);

			// Wait for AP to start (with timeout)
			uint32_t timeout = AP_STARTUP_TIMEOUT_MS;
			while (!__atomic_load_n(&g_ap_ready,
						__ATOMIC_SEQ_CST) &&
			       timeout > 0) {
				lapic_delay_ms(1);
				timeout--;
			}
		}

		if (__atomic_load_n(&g_ap_ready, __ATOMIC_SEQ_CST)) {
			cpu->started = true;
			ap_index++;
		} else {
			// Give it a bit more time - there's a race between AP setting g_ap_ready
			// and BSP checking it
			lapic_delay_ms(50);
			if (__atomic_load_n(&g_ap_ready, __ATOMIC_SEQ_CST)) {
				cpu->started = true;
				ap_index++;
			} else {
				kprintf("SMP: AP %u (APIC ID %u) failed to start\n",
					ap_index, cpu->apic_id);
				// IMPORTANT: Still increment ap_index so the next AP gets a unique ID
				// Otherwise two APs could get the same cpu_id, corrupting per-CPU data
				ap_index++;
			}
		}
	}

	// All APs have started (or timed out), remove trampoline identity mapping
	mm_remove_smp_identity_map(g_ap_trampoline_addr,
				   trampoline_size + 0x200);

	g_smp_state = SMP_STATE_RUNNING;
	smp_dbg("SMP: %u AP(s) started successfully\n", g_aps_started);
}

void smp_wait_for_aps(void)
{
	// Wait for all expected APs to start
	while (__atomic_load_n(&g_aps_started, __ATOMIC_SEQ_CST) <
	       g_cpu_count - 1) {
		__asm__ volatile("pause" ::: "memory");
	}
}

uint32_t smp_get_cpu_count(void)
{
	return g_cpu_count;
}

uint32_t smp_get_aps_started(void)
{
	return g_aps_started;
}

bool smp_is_enabled(void)
{
	return g_cpu_count > 1;
}

smp_state_t smp_get_state(void)
{
	return g_smp_state;
}

// ============================================================================
// CPU Synchronization Barriers
// ============================================================================

void smp_barrier_init(smp_barrier_t *barrier, uint32_t count)
{
	barrier->count = count;
	barrier->waiting = 0;
	barrier->sense = 0;
}

void smp_barrier_wait(smp_barrier_t *barrier)
{
	uint32_t local_sense = !barrier->sense;

	if (__atomic_add_fetch(&barrier->waiting, 1, __ATOMIC_SEQ_CST) ==
	    barrier->count) {
		// Last one to arrive - reset and release
		barrier->waiting = 0;
		__atomic_store_n(&barrier->sense, local_sense,
				 __ATOMIC_SEQ_CST);
	} else {
		// Wait for sense to change
		while (__atomic_load_n(&barrier->sense, __ATOMIC_SEQ_CST) !=
		       local_sense) {
			__asm__ volatile("pause" ::: "memory");
		}
	}
}

// ============================================================================
// Cross-CPU Function Calls (IPIs)
// ============================================================================

void smp_send_reschedule(uint32_t cpu_id)
{
	WARN_ON(cpu_id >= g_cpu_count);
	if (cpu_id >= g_cpu_count) {
		return;
	}

	percpu_t *target = percpu_get(cpu_id);
	if (target) {
		lapic_send_ipi(target->apic_id, IPI_RESCHEDULE_VECTOR);
	}
}

void smp_send_reschedule_all(void)
{
	lapic_send_ipi_all_excl_self(IPI_RESCHEDULE_VECTOR);
}

void smp_tlb_shootdown(void)
{
	lapic_send_ipi_all_excl_self(IPI_TLB_SHOOTDOWN);
}

// Monotonic generation counter for TLB shootdowns.  Each new shootdown
// increments this counter.  The per-CPU array records the last generation
// each CPU has processed.  The sender waits until all remote CPUs' recorded
// generation >= the new generation.
//
// Compared to a reset-to-zero count:
//  - Late acks from a timed-out previous shootdown read the CURRENT
//    (newer) generation and store it, so they can only help the current
//    or future shootdown — never corrupt a stale count.
//  - If two IPIs are coalesced (edge-triggered LAPIC drops the second
//    because the first is still pending in the IRR), the single delivery
//    reads the current gen (which may already be the newer one) and
//    records it.  Because the handler performs a full CR3 reload, all PTE
//    modifications visible at that point are flushed regardless.
static volatile uint64_t g_tlb_gen = 0;
static volatile uint64_t g_tlb_cpu_gen[MAX_CPUS]; // zero-initialised by BSS

// Spinlock to serialize TLB shootdowns so only one sender is active at a
// time (required: the sender iterates g_tlb_cpu_gen while holding the lock).
static spinlock_t g_tlb_shootdown_lock = SPINLOCK_INIT("tlb_shootdown");

void smp_tlb_shootdown_ack(void)
{
	uint32_t cpu_id = this_cpu_id();
	// Read the current generation AFTER the TLB flush (see interrupt.c).
	// Storing gen N means "my TLB is coherent through all modifications
	// committed before generation N was published".
	uint64_t gen = __atomic_load_n(&g_tlb_gen, __ATOMIC_ACQUIRE);
	__atomic_store_n(&g_tlb_cpu_gen[cpu_id], gen, __ATOMIC_RELEASE);
}

// Set once the BSP has parked the other CPUs via smp_halt_others().
// After this point, remote CPUs will never ACK IPIs again, so any further
// TLB-shootdown sync would always time out.  We short-circuit it: a local
// invlpg/CR3 reload by the caller is sufficient since the BSP is the only
// CPU still executing.
static volatile int g_smp_others_halted = 0;

// ============================================================================
// NMI-based "where is that CPU stuck?" diagnostic
//
// When smp_tlb_shootdown_sync() times out waiting for a CPU to ACK, that CPU
// is almost certainly spinning in a cli'd kernel section and cannot service
// fixed-delivery IPIs.  NMIs ignore the IF flag, so we can still get a
// snapshot of where the CPU actually is by sending an NMI and having the
// receiving CPU's INT 2 handler record its RIP, RSP, syscall_nr, and
// current task into a per-CPU buffer.  The sender then prints what it
// captured.  No locks: the handler writes to its own slot only, the sender
// only reads after the armed→captured handshake completes.
// ============================================================================

typedef struct {
	volatile int armed; /* 0=idle, 1=sender armed, 2=handler captured */
	uint64_t rip;
	uint64_t rsp;
	uint64_t rflags;
	int syscall_nr;
	int preempt_count;
	int interrupt_nesting;
	int tid;
	char comm[16];
} smp_nmi_capture_t;

static smp_nmi_capture_t g_nmi_capture[MAX_CPUS];

/* Match interrupt.c's REGS_* layout.  These are file-local constants so
 * smp.c doesn't have to include the private header. */
#define SMP_REGS_RIP 17
#define SMP_REGS_RSP 20 /* iret frame: rip,cs,rflags,rsp,ss */
#define SMP_REGS_RFLAGS 19
#define SMP_REGS_CS 18

int smp_nmi_capture_record(uint64_t *regs)
{
	/* CRITICAL: this runs on the IST2 stack from the NMI vector, BEFORE
     * isr_common_stub does any swapgs (it doesn't).  If the NMI
     * interrupted user mode, GS_BASE is still the user task's TLS base
     * and any %gs: access — including this_cpu_id() and this_cpu() —
     * would dereference user memory.  In musl/glibc, gs_base is often 0
     * or unmapped near 0, so the %gs:104 read used by the stack
     * protector or the %gs:128 read used by this_cpu_id() would #PF.
     * That nested fault is then routed by exception_handler's INT 14
     * path as a user-mode SIGSEGV, silently killing whichever user task
     * was running on the NMI target — observed as "one tmux pane stops
     * printing while the other keeps going."
     *
     * Defensive policy: if CS in the iret frame is user (CPL=3), do not
     * touch per-CPU state at all and just absorb the NMI.  We can't
     * usefully capture a stuck-CPU diagnostic from a CPU that was
     * actively running user code anyway.
     *
     * Similarly: even in kernel mode, if armed != 1 (no probe pending
     * for this CPU, or already captured by a previous NMI), absorb
     * rather than fall through to kernel_oops.  Spurious platform NMIs
     * (VMware snapshot signals, perfctr overflow, watchdogs) would
     * otherwise convert into a silent system kill via smp_halt_others. */

	uint64_t cs = regs[SMP_REGS_CS];
	if ((cs & 3) == 3) {
		/* NMI interrupted user mode — refuse to touch %gs: and absorb. */
		return 1;
	}

	/* Kernel mode: safe to use the kernel GS base.  Use the per-CPU
     * armed flag as the "this NMI is mine" signal. */
	uint32_t cpu = this_cpu_id();
	if (cpu >= MAX_CPUS)
		return 1; /* absorb rather than oops on garbage */

	smp_nmi_capture_t *c = &g_nmi_capture[cpu];
	int armed = __atomic_load_n(&c->armed, __ATOMIC_ACQUIRE);

	if (armed == 1) {
		c->rip = regs[SMP_REGS_RIP];
		c->rsp = regs[SMP_REGS_RSP];
		c->rflags = regs[SMP_REGS_RFLAGS];

		percpu_t *p = this_cpu();
		c->preempt_count = p ? p->preempt_count : 0;
		c->interrupt_nesting = p ? p->interrupt_nesting : 0;
		c->syscall_nr = p ? p->current_syscall_nr : -1;

		task_t *t = p ? p->current_task : NULL;
		c->tid = t ? t->id : -1;
		if (t) {
			int i;
			for (i = 0; i < 15 && t->comm[i]; i++)
				c->comm[i] = t->comm[i];
			c->comm[i] = 0;
		} else {
			c->comm[0] = 0;
		}
		__atomic_store_n(&c->armed, 2, __ATOMIC_RELEASE);
	}
	/* For armed==0 (no probe pending) or armed==2 (already captured by a
     * prior NMI in this probe round), silently absorb.  We trade the
     * ability to oops on a "real" platform NMI for not converting
     * spurious NMIs into smp_halt_others. */
	return 1;
}

/* Sender side: probe one lagging CPU.  Returns 1 if capture succeeded and
 * fills *out; 0 if the NMI never landed (CPU is wedged so badly even NMI
 * doesn't deliver — e.g. SHUTDOWN/INIT or stuck in a triple-fault loop). */
static int smp_nmi_probe(uint32_t cpu, smp_nmi_capture_t *out)
{
	if (cpu >= MAX_CPUS)
		return 0;
	smp_nmi_capture_t *c = &g_nmi_capture[cpu];

	/* Reset and arm. */
	c->armed = 0;
	__atomic_store_n(&c->armed, 1, __ATOMIC_RELEASE);

	percpu_t *p = percpu_get(cpu);
	if (!p) {
		c->armed = 0;
		return 0;
	}
	lapic_send_nmi(p->apic_id);

	/* NMI delivery is essentially synchronous, but be generous: spin
     * ~50 ms before giving up.  We poll with pause; this thread is not
     * holding any lock at this point (sync's main lock was released
     * before the wait loop). */
	uint64_t tsc_freq = lapic_get_tsc_freq();
	uint64_t tsc_50ms = (tsc_freq ? tsc_freq : 1000000000ULL) / 20;
	uint64_t deadline = timer_rdtsc() + tsc_50ms;
	while (timer_rdtsc() < deadline) {
		if (__atomic_load_n(&c->armed, __ATOMIC_ACQUIRE) == 2) {
			*out = *c;
			return 1;
		}
		__asm__ volatile("pause" ::: "memory");
	}
	return 0;
}

int smp_others_halted(void)
{
	return g_smp_others_halted;
}

void smp_tlb_shootdown_sync(void)
{
	if (g_smp_others_halted)
		return;

	uint32_t online = percpu_get_online_count();
	if (online <= 1)
		return;

	// The lock serializes the gen-increment + IPI-send pair so each sender
	// gets a unique, monotonically increasing generation number.  The lock
	// is held only for those two operations; it is released BEFORE the
	// ack-wait loop so the sender can receive TLB IPIs from other CPUs
	// while waiting (see the unlock below).
	//
	// We must keep IRQs disabled while holding the lock to prevent the
	// timer IRQ from preempting and abandoning the lock (lock-holder
	// preemption deadlock).  The lock hold time is tiny (~10 ns: one
	// atomic add + mfence + LAPIC write), so this is safe.
	//
	// While spinning on the trylock we open brief IRQ windows so this CPU
	// can respond to TLB IPIs from the current lock holder.
	uint64_t irq_flags = local_irq_save();
	while (!spin_trylock(&g_tlb_shootdown_lock)) {
		local_irq_restore(irq_flags);
		// Brief window: IRQs enabled — respond to pending TLB IPIs etc.
		__asm__ volatile("pause" ::: "memory");
		irq_flags = local_irq_save();
	}
	// Lock acquired with IRQs disabled.

	// Advance the global generation.  The ack handler on each remote CPU
	// reads this value AFTER its CR3 reload and stores it in g_tlb_cpu_gen.
	// Using __ATOMIC_SEQ_CST ensures the store is globally ordered — in
	// particular, all preceding PTE writes (done by the caller before
	// invoking us) are visible to any CPU that subsequently reads gen and
	// then reloads CR3.
	uint64_t new_gen = __atomic_add_fetch(&g_tlb_gen, 1, __ATOMIC_SEQ_CST);

	// Belt-and-suspenders mfence: guarantees all non-atomic PTE writes by
	// the caller are flushed from the store buffer before the IPI is sent.
	__asm__ volatile("mfence" ::: "memory");

	lapic_send_ipi_all_excl_self(IPI_TLB_SHOOTDOWN);

	// CRITICAL FIX: Release the lock and re-enable IRQs BEFORE the wait loop.
	//
	// Previously the wait held IRQs disabled for up to ~3 ms (10 M pauses).
	// During that window the SENDER could not receive TLB IPIs sent by other
	// CPUs.  Those IPIs accumulated in the LAPIC IRR (coalesced to one), the
	// other senders timed out, and the lagging CPU's per-CPU gen never
	// advanced — producing the repeating "CPU3 gen=786 (expected>=795)"
	// timeout messages.
	//
	// Why this is safe:
	//  • smp_tlb_shootdown_sync() is never called from interrupt handlers
	//    (confirmed: e1000e_remap_dma_region is init-time, not IRQ path;
	//     slab_free is process context only).
	//  • sched_preempt() explicitly does NOT call dead_thread_reap()
	//    (see sched.c comment), so the timer-IRQ re-entrancy concern that
	//    motivated holding the lock through the wait is not a real risk.
	//  • With per-CPU gen tracking, concurrent senders are correct: each
	//    gets a unique monotone gen from the atomic add; the ack handler
	//    stores the CURRENT g_tlb_gen after CR3 reload, which satisfies all
	//    senders whose gen <= the stored value.
	//  • Task migration during the wait is safe: IPI handlers update
	//    g_tlb_cpu_gen[cpu] on the hardware CPU, independent of which task
	//    is scheduled there.  The wait loop's cpu_gen[c] reads are correct
	//    regardless of where the waiting task runs.
	spin_unlock(&g_tlb_shootdown_lock);
	local_irq_restore(irq_flags); // IRQs re-enabled for the wait loop

	uint32_t my_cpu = this_cpu_id();

	// Wait for every remote CPU to record gen >= new_gen in its per-CPU
	// slot.  A CPU records new_gen (or higher) only after completing a full
	// CR3 reload, so seeing the value here proves the TLB was flushed.
	//
	// Correctness under IPI coalescing: if two IPI deliveries to a CPU are
	// merged into one (edge-triggered LAPIC), the single handler fires,
	// reads g_tlb_gen (which may already be >= new_gen by then), reloads
	// CR3, and records that value.  The single CR3 reload covers all PTE
	// modifications through the recorded generation, so coherence holds.
	//
	// With IRQs now enabled, this CPU can receive and ACK TLB IPIs from
	// other senders while waiting — eliminating the starvation that caused
	// the timeouts.
	// Time-based deadline: 200 ms expressed in TSC cycles.
	//
	// Each lagging CPU is re-IPIed every ~10 ms so that a vCPU that was
	// descheduled (or whose initial IPI was coalesced by the edge-triggered
	// LAPIC) receives a fresh IPI as soon as it is rescheduled.  Without
	// retries, a vCPU that was simply off-CPU when the broadcast arrived
	// would never get another chance to ACK within the deadline.
	uint64_t tsc_freq = lapic_get_tsc_freq();
	// Fall back to a conservative 1 GHz estimate if not calibrated yet.
	uint64_t tsc_1ms = (tsc_freq != 0) ? (tsc_freq / 1000) : 1000000ULL;
	uint64_t tsc_10ms = tsc_1ms * 10;
	uint64_t tsc_1000ms =
		tsc_1ms *
		1000; /* VirtualBox can deschedule a vCPU for >200 ms under load */
	uint64_t start_tsc = timer_rdtsc();
	uint64_t next_retry_tsc = start_tsc + tsc_10ms;

	for (;;) {
		bool all_acked = true;
		for (uint32_t c = 0; c < online; c++) {
			if (c == my_cpu)
				continue;
			if (__atomic_load_n(&g_tlb_cpu_gen[c],
					    __ATOMIC_ACQUIRE) < new_gen) {
				all_acked = false;
				break;
			}
		}
		if (all_acked)
			return;

		uint64_t now = timer_rdtsc();
		if (now - start_tsc >= tsc_1000ms)
			break;

		// Every ~10 ms, re-send the IPI unicast to each CPU that is still
		// lagging.  This recovers from two failure modes:
		//   (a) The initial broadcast was received while the vCPU had IRQs
		//       disabled (e.g. holding a spinlock) — the LAPIC may have
		//       dropped or coalesced it.
		//   (b) The vCPU was descheduled by the hypervisor when the IPI
		//       arrived and the pending-bit was lost on context-switch.
		// A unicast re-IPI is cheap (~200 ns) and harmless: if the CPU has
		// already ACKed, the extra IPI fires the handler which re-records
		// the current gen (already >= new_gen), so no harm done.
		if (now >= next_retry_tsc) {
			for (uint32_t c = 0; c < online; c++) {
				if (c == my_cpu)
					continue;
				if (__atomic_load_n(&g_tlb_cpu_gen[c],
						    __ATOMIC_ACQUIRE) <
				    new_gen) {
					percpu_t *cp = percpu_get(c);
					if (cp)
						lapic_send_ipi(
							cp->apic_id,
							IPI_TLB_SHOOTDOWN);
				}
			}
			next_retry_tsc = now + tsc_10ms;
		}

		__asm__ volatile("pause" ::: "memory");
	}

	// Timed out — log which CPUs are lagging and NMI-probe each one so we
	// see WHERE it is stuck (RIP/RSP/task/syscall_nr).  NMIs ignore the
	// target's IF flag, so even a CPU spinning in a cli'd section will
	// service the INT 2 handler and record its state into g_nmi_capture.
	kprintf("SMP: TLB shootdown sync timeout (gen=%llu)\n",
		(unsigned long long)new_gen);
	for (uint32_t c = 0; c < online; c++) {
		if (c == my_cpu)
			continue;
		uint64_t cgen =
			__atomic_load_n(&g_tlb_cpu_gen[c], __ATOMIC_ACQUIRE);
		if (cgen < new_gen) {
			kprintf("  CPU%u gen=%llu (expected>=%llu)\n", c,
				(unsigned long long)cgen,
				(unsigned long long)new_gen);

			smp_nmi_capture_t cap;
			if (smp_nmi_probe(c, &cap)) {
				kprintf("    NMI: RIP=%016llx RSP=%016llx RFLAGS=%016llx\n",
					(unsigned long long)cap.rip,
					(unsigned long long)cap.rsp,
					(unsigned long long)cap.rflags);
				kprintf("    NMI: task=%s tid=%d syscall_nr=%d "
					"preempt=%d in_irq_nest=%d\n",
					cap.comm[0] ? cap.comm : "?", cap.tid,
					cap.syscall_nr, cap.preempt_count,
					cap.interrupt_nesting);
			} else {
				kprintf("    NMI: probe failed (no capture within 50 ms)\n");
			}
		}
	}
}

void smp_halt_others(void)
{
	lapic_send_ipi_all_excl_self(IPI_HALT_VECTOR);
	g_smp_others_halted = 1;
}
