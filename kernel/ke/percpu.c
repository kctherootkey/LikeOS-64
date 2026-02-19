// LikeOS-64 - Per-CPU Data Implementation
// Per-CPU variable management and GS segment setup

#include "../../include/kernel/percpu.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/acpi.h"

// ============================================================================
// Global Per-CPU Data
// ============================================================================

// Array of pointers to per-CPU data structures
static percpu_t* g_percpu_ptrs[MAX_CPUS] = {0};

// Number of CPUs online
static volatile uint32_t g_cpus_online = 0;

// BSP per-CPU data (statically allocated for bootstrap)
static percpu_t g_bsp_percpu __attribute__((aligned(4096)));

// ============================================================================
// Per-CPU Initialization
// ============================================================================

void percpu_init(void) {
    kprintf("PERCPU: Initializing per-CPU infrastructure\n");
    
    // Initialize BSP's per-CPU data
    mm_memset(&g_bsp_percpu, 0, sizeof(percpu_t));
    g_bsp_percpu.self = &g_bsp_percpu;
    g_bsp_percpu.cpu_id = 0;
    g_bsp_percpu.apic_id = 0;  // Will be updated from LAPIC
    g_bsp_percpu.current_task = NULL;
    g_bsp_percpu.idle_task = NULL;
    g_bsp_percpu.preempt_count = 0;
    g_bsp_percpu.interrupt_nesting = 0;
    g_bsp_percpu.need_resched = 0;
    g_bsp_percpu.runqueue_head = NULL;
    g_bsp_percpu.runqueue_tail = NULL;
    g_bsp_percpu.runqueue_length = 0;
    spinlock_init(&g_bsp_percpu.runqueue_lock, "cpu0_runqueue");
    g_bsp_percpu.context_switches = 0;
    g_bsp_percpu.interrupts = 0;
    g_bsp_percpu.timer_ticks = 0;
    
    g_percpu_ptrs[0] = &g_bsp_percpu;
    g_cpus_online = 1;
    
    // Set GS base to point to BSP's per-CPU data
    write_gs_base((uint64_t)&g_bsp_percpu);
    
    kprintf("PERCPU: BSP per-CPU data at 0x%lx\n", (uint64_t)&g_bsp_percpu);
}

void percpu_init_cpu(uint32_t cpu_id, uint32_t apic_id) {
    percpu_t* percpu;
    
    if (cpu_id == 0) {
        // BSP - already initialized
        percpu = &g_bsp_percpu;
    } else {
        // AP - allocate new per-CPU data
        percpu = percpu_alloc(cpu_id);
        if (!percpu) {
            kprintf("PERCPU: Failed to allocate per-CPU data for CPU %u\n", cpu_id);
            return;
        }
    }
    
    // Initialize per-CPU fields
    percpu->self = percpu;
    percpu->cpu_id = cpu_id;
    percpu->apic_id = apic_id;
    percpu->current_task = NULL;
    percpu->idle_task = NULL;
    percpu->preempt_count = 0;
    percpu->interrupt_nesting = 0;
    percpu->need_resched = 0;
    percpu->runqueue_head = NULL;
    percpu->runqueue_tail = NULL;
    percpu->runqueue_length = 0;
    
    char lock_name[32];
    // Simple string formatting without snprintf
    lock_name[0] = 'c';
    lock_name[1] = 'p';
    lock_name[2] = 'u';
    lock_name[3] = '0' + (cpu_id % 10);
    lock_name[4] = '_';
    lock_name[5] = 'r';
    lock_name[6] = 'q';
    lock_name[7] = '\0';
    spinlock_init(&percpu->runqueue_lock, lock_name);
    
    percpu->context_switches = 0;
    percpu->interrupts = 0;
    percpu->timer_ticks = 0;
    
    // Set GS base for this CPU
    write_gs_base((uint64_t)percpu);
    
    // Store in global array
    g_percpu_ptrs[cpu_id] = percpu;
    
    // Increment online CPU count atomically
    __atomic_fetch_add(&g_cpus_online, 1, __ATOMIC_SEQ_CST);
    
    kprintf("PERCPU: CPU %u initialized (APIC ID %u, percpu at 0x%lx)\n",
            cpu_id, apic_id, (uint64_t)percpu);
}

percpu_t* percpu_alloc(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return NULL;
    }
    
    // Allocate page-aligned per-CPU data
    // Use physical page allocator + kernel mapping
    uint64_t phys_page = mm_allocate_physical_page();
    if (phys_page == 0) {
        return NULL;
    }
    
    percpu_t* percpu = (percpu_t*)phys_to_virt(phys_page);
    mm_memset(percpu, 0, PERCPU_SIZE);
    
    return percpu;
}

percpu_t* percpu_get(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return NULL;
    }
    return g_percpu_ptrs[cpu_id];
}

uint32_t percpu_get_online_count(void) {
    return g_cpus_online;
}

// ============================================================================
// Per-CPU Run Queue Functions
// ============================================================================

void percpu_runqueue_enqueue(task_t* task) {
    percpu_t* cpu = this_cpu();
    uint64_t flags;
    
    spin_lock_irqsave(&cpu->runqueue_lock, &flags);
    
    task->next = NULL;
    
    if (cpu->runqueue_tail) {
        cpu->runqueue_tail->next = task;
        cpu->runqueue_tail = task;
    } else {
        cpu->runqueue_head = task;
        cpu->runqueue_tail = task;
    }
    
    cpu->runqueue_length++;
    
    spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
}

task_t* percpu_runqueue_dequeue(void) {
    percpu_t* cpu = this_cpu();
    uint64_t flags;
    task_t* task = NULL;
    
    spin_lock_irqsave(&cpu->runqueue_lock, &flags);
    
    if (cpu->runqueue_head) {
        task = cpu->runqueue_head;
        cpu->runqueue_head = task->next;
        
        if (!cpu->runqueue_head) {
            cpu->runqueue_tail = NULL;
        }
        
        cpu->runqueue_length--;
        task->next = NULL;
    }
    
    spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
    
    return task;
}

void percpu_runqueue_enqueue_cpu(uint32_t cpu_id, task_t* task) {
    if (cpu_id >= MAX_CPUS || !g_percpu_ptrs[cpu_id]) {
        return;
    }
    
    percpu_t* cpu = g_percpu_ptrs[cpu_id];
    uint64_t flags;
    
    spin_lock_irqsave(&cpu->runqueue_lock, &flags);
    
    task->next = NULL;
    
    if (cpu->runqueue_tail) {
        cpu->runqueue_tail->next = task;
        cpu->runqueue_tail = task;
    } else {
        cpu->runqueue_head = task;
        cpu->runqueue_tail = task;
    }
    
    cpu->runqueue_length++;
    
    spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
}

uint32_t percpu_runqueue_length(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS || !g_percpu_ptrs[cpu_id]) {
        return 0;
    }
    return g_percpu_ptrs[cpu_id]->runqueue_length;
}

uint32_t percpu_find_least_loaded_cpu(void) {
    uint32_t min_length = 0xFFFFFFFF;
    uint32_t min_cpu = 0;
    
    for (uint32_t i = 0; i < g_cpus_online; i++) {
        if (g_percpu_ptrs[i]) {
            uint32_t length = g_percpu_ptrs[i]->runqueue_length;
            if (length < min_length) {
                min_length = length;
                min_cpu = i;
            }
        }
    }
    
    return min_cpu;
}
