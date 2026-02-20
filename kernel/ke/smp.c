// LikeOS-64 - SMP (Symmetric Multi-Processing) Implementation
// AP startup, CPU synchronization, and SMP management

#include "../../include/kernel/smp.h"
#include "../../include/kernel/acpi.h"
#include "../../include/kernel/lapic.h"
#include "../../include/kernel/percpu.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/sched.h"  // For sched_enable_smp()

// ============================================================================
// External Trampoline Symbols
// ============================================================================

extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

// Offsets within trampoline (must match ap_trampoline.S)
#define AP_TRAMPOLINE_PML4_OFFSET   0x108
#define AP_TRAMPOLINE_STACK_OFFSET  0x110
#define AP_TRAMPOLINE_CPU_OFFSET    0x118
#define AP_TRAMPOLINE_ENTRY_OFFSET  0x120

// ============================================================================
// SMP State
// ============================================================================

static smp_state_t g_smp_state = SMP_STATE_BSP_ONLY;
static volatile uint32_t g_aps_started = 0;
static uint32_t g_cpu_count = 1;  // At least BSP
static smp_barrier_t g_startup_barrier;

// AP trampoline address (set from boot_info or fallback to default)
static uint64_t g_ap_trampoline_addr = 0;

// Per-AP stacks
static uint8_t* g_ap_stacks[MAX_CPUS] = {0};

// Volatile flag for AP startup synchronization
static volatile int g_ap_ready = 0;

// ============================================================================
// Delay Functions
// ============================================================================

static void pit_delay_us(uint32_t us) {
    uint32_t ticks = (us * 1193182ULL) / 1000000;
    if (ticks == 0) ticks = 1;
    if (ticks > 65535) ticks = 65535;
    
    outb(0x61, (inb(0x61) & 0xFD) | 0x01);
    outb(0x43, 0xB0);
    outb(0x42, ticks & 0xFF);
    outb(0x42, (ticks >> 8) & 0xFF);
    
    while ((inb(0x61) & 0x20) == 0) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static void pit_delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        pit_delay_us(1000);
    }
}

// ============================================================================
// AP Entry Point
// ============================================================================

// Enable SSE/FPU on this CPU
static inline void ap_enable_sse(void) {
    uint64_t cr0, cr4;
    
    // Read CR0
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    
    // Clear CR0.EM (bit 2) - disable x87 emulation
    // Set CR0.MP (bit 1) - enable FPU monitoring
    cr0 &= ~(1ULL << 2);  // Clear EM
    cr0 |= (1ULL << 1);   // Set MP
    
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    
    // Read CR4
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    
    // Set CR4.OSFXSR (bit 9) - enable FXSAVE/FXRSTOR
    // Set CR4.OSXMMEXCPT (bit 10) - enable SIMD exceptions
    cr4 |= (1ULL << 9);   // OSFXSR
    cr4 |= (1ULL << 10);  // OSXMMEXCPT
    
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
    
    // Initialize FPU
    __asm__ volatile("fninit");
}

void ap_entry(void) {
    // CRITICAL: Enable SSE/FPU first, before any code that might use SSE
    // (such as optimized memcpy in kernel functions)
    ap_enable_sse();
    
    // CRITICAL: Load kernel's GDT and IDT first thing!
    // The AP is currently using the trampoline's minimal GDT but with
    // KERNEL-COMPATIBLE selectors (CS=0x08, DS/SS=0x10) so no far jump needed.
    // Just load the kernel GDT/IDT and we're good to go.
    
    // Load kernel GDT
    void* gdt_desc = gdt_get_descriptor();
    __asm__ volatile("lgdt (%0)" : : "r"(gdt_desc) : "memory");
    
    // No far jump needed! The trampoline uses CS=0x08 and DS/SS=0x10
    // which are exactly the same selectors as the kernel GDT.
    // The selectors point to equivalent descriptors, so loading the
    // kernel GDT seamlessly replaces the trampoline GDT.
    
    // Load kernel IDT
    void* idt_desc = interrupts_get_idt_descriptor();
    __asm__ volatile("lidt (%0)" : : "r"(idt_desc) : "memory");
    
    // Now we can safely use the kernel's exception handlers
    
    // We're now running on an AP in long mode with kernel GDT/IDT
    // Get our CPU ID from the trampoline data
    uint32_t cpu_id = *(volatile uint32_t*)(phys_to_virt(g_ap_trampoline_addr + AP_TRAMPOLINE_CPU_OFFSET));
    
    // Get our APIC ID
    uint32_t apic_id = lapic_get_id();
    
    // Initialize per-CPU data for this AP
    percpu_init_cpu(cpu_id, apic_id);
    
    // Initialize per-CPU TSS (each AP needs its own TSS for RSP0)
    tss_init_ap(cpu_id);
    
    // Initialize LAPIC for this CPU
    lapic_init();
    
    // Signal that we're ready
    __atomic_fetch_add(&g_aps_started, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_ap_ready, 1, __ATOMIC_SEQ_CST);
    
    smp_dbg("SMP: AP %u started (APIC ID %u)\n", cpu_id, apic_id);
    
    // Enable interrupts
    __asm__ volatile("sti");
    
    // Start LAPIC timer for this CPU
    lapic_timer_start(100);  // 100 Hz
    
    // Enter idle loop - the scheduler will give us work
    while (1) {
        __asm__ volatile("hlt");
        
        // Check if we have work to do
        percpu_t* cpu = this_cpu();
        if (cpu->need_resched) {
            cpu->need_resched = 0;
            // sched_schedule() will be called
        }
    }
}

// ============================================================================
// SMP Initialization
// ============================================================================

void smp_init(uint64_t trampoline_addr) {
    smp_dbg("SMP: Initializing...\n");
    
    // Set AP trampoline address (from bootloader or fallback to default)
    if (trampoline_addr != 0 && trampoline_addr < 0x100000 && (trampoline_addr & 0xFFF) == 0) {
        g_ap_trampoline_addr = trampoline_addr;
        smp_dbg("SMP: Using bootloader-provided trampoline at 0x%lx\n", trampoline_addr);
    } else {
        g_ap_trampoline_addr = AP_TRAMPOLINE_ADDR_DEFAULT;
        if (trampoline_addr != 0) {
            smp_dbg("SMP: Invalid trampoline address 0x%lx, using default 0x%lx\n", 
                    trampoline_addr, g_ap_trampoline_addr);
        } else {
            smp_dbg("SMP: Using default trampoline address 0x%lx\n", g_ap_trampoline_addr);
        }
    }
    
    // Get CPU count from ACPI
    g_cpu_count = acpi_get_cpu_count();
    if (g_cpu_count == 0) {
        g_cpu_count = 1;  // At least BSP
    }
    
    if (g_cpu_count > MAX_CPUS) {
        smp_dbg("SMP: Limiting CPU count from %u to %u\n", g_cpu_count, MAX_CPUS);
        g_cpu_count = MAX_CPUS;
    }
    
    smp_dbg("SMP: %u CPU(s) detected\n", g_cpu_count);
    
    // Initialize BSP's LAPIC
    lapic_init();
    
    // Update BSP's per-CPU data with APIC ID
    percpu_t* bsp = this_cpu();
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

void smp_boot_aps(void) {
    if (g_cpu_count <= 1) {
        return;
    }
    
    smp_dbg("SMP: Starting %u Application Processor(s)...\n", g_cpu_count - 1);
    
    // Copy trampoline code to low memory (at bootloader-reserved address)
    size_t trampoline_size = (size_t)(ap_trampoline_end - ap_trampoline_start);
    void* trampoline_dest = phys_to_virt(g_ap_trampoline_addr);
    mm_memcpy(trampoline_dest, ap_trampoline_start, trampoline_size);
    
    // CRITICAL: Identity-map the trampoline page so APs can execute after enabling paging
    // The kernel removed identity mapping earlier, but APs need it to complete the mode switch
    if (!mm_identity_map_for_smp(g_ap_trampoline_addr, trampoline_size + 0x200)) {
        kprintf("SMP: ERROR: Failed to identity-map trampoline!\n");
        return;
    }
    
    smp_dbg("SMP: Trampoline copied to 0x%lx, size=%u bytes\n", g_ap_trampoline_addr, (uint32_t)trampoline_size);
    
    // Get PML4 physical address for APs (same as BSP)
    uint64_t pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    pml4_phys &= 0x000FFFFFFFFFF000ULL;
    
    smp_dbg("SMP: BSP PML4 physical address = 0x%lx\n", pml4_phys);
    
    // Store PML4 address in trampoline
    volatile uint64_t* pml4_ptr = (volatile uint64_t*)(phys_to_virt(g_ap_trampoline_addr + AP_TRAMPOLINE_PML4_OFFSET));
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
    *(volatile uint64_t*)(phys_to_virt(g_ap_trampoline_addr + AP_TRAMPOLINE_ENTRY_OFFSET)) = (uint64_t)ap_entry;
    __asm__ volatile("mfence" ::: "memory");
    
    // Start each AP
    acpi_info_t* acpi_info = acpi_get_info();
    uint32_t ap_index = 1;  // Skip BSP (index 0)
    
    for (uint32_t i = 0; i < acpi_info->cpu_count && ap_index < g_cpu_count; i++) {
        cpu_info_t* cpu = &acpi_info->cpus[i];
        
        // Skip BSP
        if (cpu->bsp || cpu->apic_id == this_cpu()->apic_id) {
            continue;
        }
        
        // Skip disabled CPUs
        if (!cpu->enabled && !cpu->online_capable) {
            continue;
        }
        
        smp_dbg("SMP: Starting AP %u (APIC ID %u)...\n", ap_index, cpu->apic_id);
        
        // Allocate stack for this AP
        g_ap_stacks[ap_index] = (uint8_t*)kalloc(AP_STACK_SIZE);
        if (!g_ap_stacks[ap_index]) {
            kprintf("SMP: Failed to allocate stack for AP %u\n", ap_index);
            continue;
        }
        
        // Store stack pointer (stack grows down, so point to top)
        uint64_t stack_top = (uint64_t)(g_ap_stacks[ap_index] + AP_STACK_SIZE);
        stack_top &= ~0xFULL;  // 16-byte align
        *(volatile uint64_t*)(phys_to_virt(g_ap_trampoline_addr + AP_TRAMPOLINE_STACK_OFFSET)) = stack_top;
        
        // Store CPU ID
        *(volatile uint32_t*)(phys_to_virt(g_ap_trampoline_addr + AP_TRAMPOLINE_CPU_OFFSET)) = ap_index;
        
        // Reset ready flag
        __atomic_store_n(&g_ap_ready, 0, __ATOMIC_SEQ_CST);
        
        // Send INIT IPI
        lapic_send_init(cpu->apic_id);
        
        // Wait 10ms
        pit_delay_ms(10);
        
        // Send first SIPI (vector = page number)
        lapic_send_sipi(cpu->apic_id, g_ap_trampoline_addr >> 12);
        
        // Wait 200us
        pit_delay_us(200);
        
        // If AP hasn't started, send second SIPI
        if (!__atomic_load_n(&g_ap_ready, __ATOMIC_SEQ_CST)) {
            lapic_send_sipi(cpu->apic_id, g_ap_trampoline_addr >> 12);
            
            // Wait for AP to start (with timeout)
            uint32_t timeout = AP_STARTUP_TIMEOUT_MS;
            while (!__atomic_load_n(&g_ap_ready, __ATOMIC_SEQ_CST) && timeout > 0) {
                pit_delay_ms(1);
                timeout--;
            }
        }
        
        if (__atomic_load_n(&g_ap_ready, __ATOMIC_SEQ_CST)) {
            cpu->started = true;
            ap_index++;
        } else {
            kprintf("SMP: AP %u (APIC ID %u) failed to start\n", ap_index, cpu->apic_id);
        }
    }
    
    // All APs have started (or timed out), remove trampoline identity mapping
    mm_remove_smp_identity_map(g_ap_trampoline_addr, trampoline_size + 0x200);
    
    g_smp_state = SMP_STATE_RUNNING;
    smp_dbg("SMP: %u AP(s) started successfully\n", g_aps_started);
}

void smp_wait_for_aps(void) {
    // Wait for all expected APs to start
    while (__atomic_load_n(&g_aps_started, __ATOMIC_SEQ_CST) < g_cpu_count - 1) {
        __asm__ volatile("pause" ::: "memory");
    }
}

uint32_t smp_get_cpu_count(void) {
    return g_cpu_count;
}

uint32_t smp_get_aps_started(void) {
    return g_aps_started;
}

bool smp_is_enabled(void) {
    return g_cpu_count > 1;
}

smp_state_t smp_get_state(void) {
    return g_smp_state;
}

// ============================================================================
// CPU Synchronization Barriers
// ============================================================================

void smp_barrier_init(smp_barrier_t* barrier, uint32_t count) {
    barrier->count = count;
    barrier->waiting = 0;
    barrier->sense = 0;
}

void smp_barrier_wait(smp_barrier_t* barrier) {
    uint32_t local_sense = !barrier->sense;
    
    if (__atomic_add_fetch(&barrier->waiting, 1, __ATOMIC_SEQ_CST) == barrier->count) {
        // Last one to arrive - reset and release
        barrier->waiting = 0;
        __atomic_store_n(&barrier->sense, local_sense, __ATOMIC_SEQ_CST);
    } else {
        // Wait for sense to change
        while (__atomic_load_n(&barrier->sense, __ATOMIC_SEQ_CST) != local_sense) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
}

// ============================================================================
// Cross-CPU Function Calls (IPIs)
// ============================================================================

void smp_send_reschedule(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count) {
        return;
    }
    
    percpu_t* target = percpu_get(cpu_id);
    if (target) {
        lapic_send_ipi(target->apic_id, IPI_RESCHEDULE_VECTOR);
    }
}

void smp_send_reschedule_all(void) {
    lapic_send_ipi_all_excl_self(IPI_RESCHEDULE_VECTOR);
}

void smp_tlb_shootdown(void) {
    lapic_send_ipi_all_excl_self(IPI_TLB_SHOOTDOWN);
}

void smp_halt_others(void) {
    lapic_send_ipi_all_excl_self(IPI_HALT_VECTOR);
}
