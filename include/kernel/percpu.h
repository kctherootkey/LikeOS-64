// LikeOS-64 - Per-CPU Data Infrastructure for SMP
// Provides per-CPU variables accessed via GS segment

#ifndef _KERNEL_PERCPU_H_
#define _KERNEL_PERCPU_H_

#include "types.h"
#include "sched.h"
#include "interrupt.h"

// Maximum CPUs supported
#define MAX_CPUS    64

// Per-CPU data area size (must be page-aligned for easy allocation)
#define PERCPU_SIZE 4096

// ============================================================================
// Per-CPU Data Structure
// ============================================================================

struct percpu {
    // Self-pointer for quick access (GS:0 == &this_percpu)
    struct percpu* self;                      // GS:0
    
    // Syscall scratch area — accessed from assembly via GS: prefix.
    // MUST remain at FIXED offsets; NASM constants in syscall.asm depend on this.
    uint64_t syscall_kernel_rsp;              // GS:8   — kernel RSP for current user task
    uint64_t syscall_user_rsp;                // GS:16  — scratch: saves user RSP on syscall entry
    
    // Per-CPU syscall context (for signal delivery, fork, etc.)
    // These replace the global syscall_saved_user_* variables.
    uint64_t syscall_saved_user_rip;          // GS:24  — user RIP for sysret
    uint64_t syscall_saved_user_rflags;       // GS:32  — user RFLAGS for sysret
    uint64_t syscall_saved_user_rbp;          // GS:40  — callee-saved
    uint64_t syscall_saved_user_rbx;          // GS:48  — callee-saved
    uint64_t syscall_saved_user_r12;          // GS:56  — callee-saved
    uint64_t syscall_saved_user_r13;          // GS:64  — callee-saved
    uint64_t syscall_saved_user_r14;          // GS:72  — callee-saved
    uint64_t syscall_saved_user_r15;          // GS:80  — callee-saved
    uint64_t syscall_saved_user_rax;          // GS:88  — syscall return value
    uint64_t syscall_signal_pending;          // GS:96  — signal number if pending, 0 otherwise
    
    // CPU identification
    uint32_t cpu_id;            // Logical CPU index (0 = BSP)
    uint32_t apic_id;           // LAPIC APIC ID
    
    // Current task (replaces global g_current)
    task_t* current_task;
    
    // Idle task for this CPU
    task_t* idle_task;
    
    // Preemption control (replaces global g_preempt_count)
    volatile int preempt_count;
    
    // Nested interrupt count
    volatile int interrupt_nesting;
    
    // Need reschedule flag
    volatile int need_resched;
    
    // Per-CPU run queue for scheduler
    task_t* runqueue_head;      // Head of ready task list
    task_t* runqueue_tail;      // Tail for O(1) enqueue
    uint32_t runqueue_length;   // Number of tasks in queue
    spinlock_t runqueue_lock;   // Lock for this CPU's run queue
    
    // Per-CPU statistics
    uint64_t context_switches;
    uint64_t interrupts;
    uint64_t timer_ticks;
    
    // Per-CPU kernel stack (for interrupt/exception handling)
    uint64_t kernel_stack_top;
    
    // Per-CPU TSS
    struct tss_entry* tss;
    
    // Per-CPU GDT pointer (if using per-CPU GDT) - optional
    // struct gdt_entry* gdt;
    
    // Padding to ensure page alignment and cache line separation
    uint8_t padding[PERCPU_SIZE - 224];  // Adjust based on actual struct size (was 144, added 80 for syscall context)
} __attribute__((aligned(64)));

typedef struct percpu percpu_t;

// Static assertions to verify structure layout matches assembly constants
// These MUST match the %define constants in syscall.asm
_Static_assert(__builtin_offsetof(percpu_t, self) == 0, "percpu: self must be at offset 0");
_Static_assert(__builtin_offsetof(percpu_t, syscall_kernel_rsp) == 8, "percpu: syscall_kernel_rsp must be at offset 8");
_Static_assert(__builtin_offsetof(percpu_t, syscall_user_rsp) == 16, "percpu: syscall_user_rsp must be at offset 16");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_rip) == 24, "percpu: syscall_saved_user_rip must be at offset 24");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_rflags) == 32, "percpu: syscall_saved_user_rflags must be at offset 32");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_rbp) == 40, "percpu: syscall_saved_user_rbp must be at offset 40");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_rbx) == 48, "percpu: syscall_saved_user_rbx must be at offset 48");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_r12) == 56, "percpu: syscall_saved_user_r12 must be at offset 56");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_r13) == 64, "percpu: syscall_saved_user_r13 must be at offset 64");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_r14) == 72, "percpu: syscall_saved_user_r14 must be at offset 72");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_r15) == 80, "percpu: syscall_saved_user_r15 must be at offset 80");
_Static_assert(__builtin_offsetof(percpu_t, syscall_saved_user_rax) == 88, "percpu: syscall_saved_user_rax must be at offset 88");
_Static_assert(__builtin_offsetof(percpu_t, syscall_signal_pending) == 96, "percpu: syscall_signal_pending must be at offset 96");

// ============================================================================
// Per-CPU Access Macros
// ============================================================================

// Read GS base (returns pointer to current CPU's percpu structure)
static inline percpu_t* get_gs_base(void) {
    percpu_t* base;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(base));
    return base;
}

// Get current CPU's percpu data
static inline percpu_t* this_cpu(void) {
    return get_gs_base();
}

// Get current CPU ID
static inline uint32_t this_cpu_id(void) {
    uint32_t id;
    __asm__ volatile("movl %%gs:%c1, %0" 
        : "=r"(id) 
        : "i"(__builtin_offsetof(percpu_t, cpu_id)));
    return id;
}

// Get current task (replaces sched_current())
static inline task_t* current(void) {
    task_t* task;
    __asm__ volatile("movq %%gs:%c1, %0" 
        : "=r"(task) 
        : "i"(__builtin_offsetof(percpu_t, current_task)));
    return task;
}

// Set current task
static inline void set_current(task_t* task) {
    __asm__ volatile("movq %0, %%gs:%c1"
        :
        : "r"(task), "i"(__builtin_offsetof(percpu_t, current_task))
        : "memory");
}

// ============================================================================
// Preemption Control (Per-CPU)
// ============================================================================

// Disable preemption on this CPU
static inline void percpu_preempt_disable(void) {
    __asm__ volatile("" ::: "memory");
    __asm__ volatile("incl %%gs:%c0"
        :
        : "i"(__builtin_offsetof(percpu_t, preempt_count))
        : "memory");
    __asm__ volatile("" ::: "memory");
}

// Enable preemption on this CPU
static inline void percpu_preempt_enable(void) {
    __asm__ volatile("" ::: "memory");
    __asm__ volatile("decl %%gs:%c0"
        :
        : "i"(__builtin_offsetof(percpu_t, preempt_count))
        : "memory");
    __asm__ volatile("" ::: "memory");
}

// Get preemption count
static inline int percpu_preempt_count(void) {
    int count;
    __asm__ volatile("movl %%gs:%c1, %0"
        : "=r"(count)
        : "i"(__builtin_offsetof(percpu_t, preempt_count)));
    return count;
}

// Check if preemption is enabled
static inline int percpu_preemption_enabled(void) {
    return percpu_preempt_count() == 0;
}

// ============================================================================
// MSR Definitions for GS Base
// ============================================================================

#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

// Write GS base via MSR
static inline void write_gs_base(uint64_t base) {
    uint32_t low = base & 0xFFFFFFFF;
    uint32_t high = base >> 32;
    __asm__ volatile("wrmsr" : : "c"(MSR_GS_BASE), "a"(low), "d"(high));
}

// Read GS base via MSR
static inline uint64_t read_gs_base_msr(void) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(MSR_GS_BASE));
    return ((uint64_t)high << 32) | low;
}

// Write kernel GS base (for SWAPGS)
static inline void write_kernel_gs_base(uint64_t base) {
    uint32_t low = base & 0xFFFFFFFF;
    uint32_t high = base >> 32;
    __asm__ volatile("wrmsr" : : "c"(MSR_KERNEL_GS_BASE), "a"(low), "d"(high));
}

// SWAPGS instruction wrapper
static inline void swapgs(void) {
    __asm__ volatile("swapgs" ::: "memory");
}

// ============================================================================
// Per-CPU Initialization Functions
// ============================================================================

// Initialize per-CPU infrastructure (called once by BSP)
void percpu_init(void);

// Initialize per-CPU data for the current CPU (called by each CPU)
void percpu_init_cpu(uint32_t cpu_id, uint32_t apic_id);

// Allocate per-CPU area for a new CPU (returns virtual address)
percpu_t* percpu_alloc(uint32_t cpu_id);

// Get percpu data for a specific CPU
percpu_t* percpu_get(uint32_t cpu_id);

// Get number of online CPUs
uint32_t percpu_get_online_count(void);

// ============================================================================
// Per-CPU Run Queue Functions
// ============================================================================

// Enqueue task to this CPU's run queue
void percpu_runqueue_enqueue(task_t* task);

// Dequeue next task from this CPU's run queue
task_t* percpu_runqueue_dequeue(void);

// Enqueue task to specific CPU's run queue
void percpu_runqueue_enqueue_cpu(uint32_t cpu_id, task_t* task);

// Get run queue length for a CPU
uint32_t percpu_runqueue_length(uint32_t cpu_id);

// Find CPU with shortest run queue (for load balancing)
uint32_t percpu_find_least_loaded_cpu(void);

#endif // _KERNEL_PERCPU_H_
