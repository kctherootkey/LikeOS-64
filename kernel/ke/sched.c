// LikeOS-64 Preemptive Scheduler with Full Kernel Preemption
#include "../../include/kernel/sched.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/types.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/pipe.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/signal.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/syscall.h"  // For MAP_SHARED
#include "../../include/kernel/percpu.h"   // For per-CPU current task

extern void user_mode_iret_trampoline(void);
extern void ctx_switch_asm(uint64_t** old_sp, uint64_t* new_sp);

// Preemption control - global counter (per-CPU for SMP in future)
volatile int g_preempt_count = 0;

// Scheduler spinlock for SMP safety
spinlock_t g_sched_lock = SPINLOCK_INIT("sched");

// SMP mode flag - when true, use per-CPU current task via GS segment
// Set by smp_init() after per-CPU data is initialized
int g_smp_initialized = 0;

#define SCHED_BOOTSTRAP_INTERVAL 10  // Run bootstrap every N yields (was 50)

static task_t g_bootstrap_task;
static task_t g_idle_task;
static uint8_t g_idle_stack[4096] __attribute__((aligned(16)));
static uint8_t g_bootstrap_stack[8192] __attribute__((aligned(16)));
static task_t* g_current = 0;  // Legacy global - used before SMP init
static int g_next_id = 1;
static uint64_t g_yield_count = 0;  // Counter to ensure bootstrap runs periodically
static uint64_t* g_kernel_pml4 = 0;
static uint64_t g_default_kernel_stack = 0;
static uint64_t g_preempt_count_total = 0;  // Debug: total preemptions

// Current kernel stack for syscall handler (per-task)
#include "../../include/kernel/pipe.h"
uint64_t g_current_kernel_stack = 0;

static inline int is_idle_task(const task_t* t) {
    return t == &g_idle_task;
}

static void task_trampoline(void);
static void idle_entry(void* arg);

static inline void switch_address_space(task_t* prev, task_t* next) {
    uint64_t* prev_pml4 = prev->pml4 ? prev->pml4 : g_kernel_pml4;
    uint64_t* next_pml4 = next->pml4 ? next->pml4 : g_kernel_pml4;
    
    if (next->privilege == TASK_USER && next->kernel_stack_top != 0) {
        tss_set_kernel_stack(next->kernel_stack_top);
        g_current_kernel_stack = next->kernel_stack_top;
    } else {
        tss_set_kernel_stack(g_default_kernel_stack);
        g_current_kernel_stack = g_default_kernel_stack;
    }
    
    if (prev_pml4 != next_pml4) {
        mm_switch_address_space(next_pml4);
    }
}

static void enqueue_task(task_t* t) {
    if (!g_current) {
        g_current = t;
        t->next = t;
        return;
    }
    t->next = g_current->next;
    g_current->next = t;
}

void sched_init(void) {
    g_kernel_pml4 = mm_get_current_address_space();
    g_default_kernel_stack = tss_get_kernel_stack();
    
    // Bootstrap task represents the current execution context (main kernel loop)
    // We'll context-switch away from it when user tasks run
    // For now, just set a placeholder - it will be properly set on first switch
    g_bootstrap_task.sp = 0;  // Will be set by ctx_switch_asm on first switch
    g_bootstrap_task.pml4 = NULL;
    g_bootstrap_task.entry = 0;
    g_bootstrap_task.arg = 0;
    g_bootstrap_task.state = TASK_RUNNING;
    g_bootstrap_task.privilege = TASK_KERNEL;
    g_bootstrap_task.id = 0;
    g_bootstrap_task.next = &g_bootstrap_task;
    g_bootstrap_task.user_stack_top = 0;
    g_bootstrap_task.kernel_stack_top = 0;
    g_bootstrap_task.kernel_stack_base = NULL;
    g_bootstrap_task.pgid = 0;
    g_bootstrap_task.sid = 0;
    g_bootstrap_task.ctty = NULL;
    g_bootstrap_task.wait_next = NULL;
    g_bootstrap_task.wait_channel = NULL;
    g_bootstrap_task.wakeup_tick = 0;
    // Preemption fields
    g_bootstrap_task.need_resched = 0;
    g_bootstrap_task.remaining_ticks = SCHED_TIME_SLICE;
    g_bootstrap_task.preempt_frame = NULL;
    g_current = &g_bootstrap_task;

    sched_add_task(idle_entry, 0, g_idle_stack, sizeof(g_idle_stack));
    kprintf("Preemptive scheduler initialized (time slice=%d ticks)\n", SCHED_TIME_SLICE);
}

void sched_add_task(task_entry_t entry, void* arg, void* stack_mem, size_t stack_size) {
    if (!entry || !stack_mem || stack_size < 128) {
        return;
    }
    task_t* t = (entry == idle_entry) ? &g_idle_task : (task_t*)kalloc(sizeof(task_t));
    if (!t) {
        return;
    }

    uint64_t* sp = (uint64_t*)((uint8_t*)stack_mem + stack_size);
    sp = (uint64_t*)((uint64_t)sp & ~0xFUL);
    *(--sp) = (uint64_t)task_trampoline;
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    t->sp = sp;
    t->pml4 = NULL;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->privilege = TASK_KERNEL;
    t->id = g_next_id++;
    t->user_stack_top = 0;
    t->kernel_stack_top = 0;
    t->kernel_stack_base = NULL;
    t->pgid = 0;
    t->sid = 0;
    t->ctty = NULL;
    t->wait_next = NULL;
    t->wait_channel = NULL;
    t->wakeup_tick = 0;
    
    // Initialize preemption fields
    t->need_resched = 0;
    t->remaining_ticks = SCHED_TIME_SLICE;
    t->preempt_frame = NULL;
    
    // Initialize signal state
    signal_init_task(t);
    
    // Initialize process hierarchy
    t->parent = NULL;
    t->first_child = NULL;
    t->next_sibling = NULL;
    t->exit_code = 0;
    t->has_exited = false;
    t->is_fork_child = false;
    
    // Initialize cwd
    t->cwd[0] = '/';
    t->cwd[1] = 0;
    
    // Initialize file descriptor table (kernel tasks don't use fd_table)
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        t->fd_table[i] = NULL;
    }
    t->brk_start = 0;
    t->brk = 0;
    t->mmap_base = 0;
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        t->mmap_regions[i].in_use = false;
    }

    enqueue_task(t);
}

task_t* sched_add_user_task(task_entry_t entry, void* arg, uint64_t* pml4, uint64_t user_stack, uint64_t kernel_stack) {
    (void)kernel_stack;
    if (!entry || !pml4) {
        return NULL;
    }
    
    task_t* t = (task_t*)kalloc(sizeof(task_t));
    if (!t) {
        return NULL;
    }
    
    uint8_t* k_stack_mem = (uint8_t*)kalloc(8192);
    if (!k_stack_mem) {
        kfree(t);
        return NULL;
    }
    
    // Align stack top to 16 bytes for ABI compliance
    uint64_t k_stack_top = ((uint64_t)(k_stack_mem + 8192)) & ~0xFUL;
    uint64_t* k_sp = (uint64_t*)k_stack_top;
    
    // IRET frame: SS, RSP, RFLAGS, CS, RIP
    *(--k_sp) = 0x1B;                    // SS
    *(--k_sp) = user_stack;              // RSP (points to argc on user stack)
    *(--k_sp) = 0x202;                   // RFLAGS (IF enabled)
    *(--k_sp) = 0x23;                    // CS
    *(--k_sp) = (uint64_t)entry;         // RIP
    
    // Callee-saved registers
    *(--k_sp) = (uint64_t)user_mode_iret_trampoline;
    *(--k_sp) = 0; // rbp
    *(--k_sp) = 0; // rbx
    *(--k_sp) = 0; // r12
    *(--k_sp) = 0; // r13
    *(--k_sp) = 0; // r14
    *(--k_sp) = 0; // r15

    t->sp = k_sp;
    t->pml4 = pml4;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->privilege = TASK_USER;
    t->id = g_next_id++;
    t->user_stack_top = user_stack;
    t->kernel_stack_top = k_stack_top;  // Use aligned stack top
    t->kernel_stack_base = k_stack_mem; // Save original allocation for freeing
    t->pgid = t->id;
    t->sid = t->id;
    t->ctty = tty_get_console();
    t->wait_next = NULL;
    t->wait_channel = NULL;
    t->wakeup_tick = 0;
    
    // Initialize preemption fields
    t->need_resched = 0;
    t->remaining_ticks = SCHED_TIME_SLICE;
    t->preempt_frame = NULL;
    
    // Initialize signal state
    signal_init_task(t);
    
    // Initialize process hierarchy
    t->parent = NULL;
    t->first_child = NULL;
    t->next_sibling = NULL;
    t->exit_code = 0;
    t->has_exited = false;
    t->is_fork_child = false;
    
    // Initialize cwd
    t->cwd[0] = '/';
    t->cwd[1] = 0;
    
    // Initialize file descriptor table (all NULL)
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        t->fd_table[i] = NULL;
    }
    
    // Initialize memory management
    // Memory layout:
    //   Code:   0x00400000 - 0x00500000 (1MB)
    //   Heap:   0x00500000 - grows up
    //   ...
    //   mmap:   below stack, grows down
    //   Stack:  at user_stack, grows down
    t->brk_start = 0x500000;  // 5MB - after code area
    t->brk = t->brk_start;
    // mmap area starts 4MB below stack and grows downward
    t->mmap_base = user_stack - (4 * 1024 * 1024);
    
    // Initialize mmap regions
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        t->mmap_regions[i].in_use = false;
    }

    enqueue_task(t);
    
    return t;
}

static inline int is_bootstrap_task(const task_t* t) {
    return t == &g_bootstrap_task;
}

static task_t* pick_next(task_t* start, int from_timer) {
    if (!start || !start->next) {
        return start;
    }
    
    task_t* it = start->next;
    task_t* idle_candidate = 0;
    task_t* bootstrap_candidate = 0;
    task_t* other_user_task = 0;
    
    // First, check the start task itself (in case it's bootstrap and ready)
    if (is_bootstrap_task(start) && (start->state == TASK_READY || start->state == TASK_RUNNING) && !start->has_exited) {
        bootstrap_candidate = start;
    }
    
    while (it != start) {
        // Skip exited/zombie tasks - they must never be scheduled
        if (it->has_exited) {
            it = it->next;
            if (!it) break;
            continue;
        }
        if (it->state == TASK_READY || it->state == TASK_RUNNING) {
            // Track user tasks that are not the current one
            if (!is_idle_task(it) && !is_bootstrap_task(it)) {
                if (!other_user_task) {
                    other_user_task = it;
                }
            }
            if (is_idle_task(it)) {
                idle_candidate = it;
            }
            if (is_bootstrap_task(it)) {
                bootstrap_candidate = it;
            }
        }
        it = it->next;
        if (!it) break;
    }
    
    // If there's another user task ready, switch to it (round-robin)
    if (other_user_task) {
        g_yield_count = 0;
        return other_user_task;
    }
    
    // No other user tasks - increment yield count for bootstrap interval
    g_yield_count++;
    
    // Periodically let bootstrap run for system housekeeping (shell, USB polling)
    // This prevents a single user task from completely starving the kernel loop
    if (bootstrap_candidate && g_yield_count >= SCHED_BOOTSTRAP_INTERVAL) {
        g_yield_count = 0;
        return bootstrap_candidate;
    }
    
    // If called from timer preemption, occasionally let idle/bootstrap run
    // This ensures system responsiveness even with a single CPU-bound task
    if (from_timer && (g_yield_count % 10) == 0) {
        if (bootstrap_candidate && bootstrap_candidate != start) {
            return bootstrap_candidate;
        }
        if (idle_candidate && idle_candidate != start) {
            return idle_candidate;
        }
    }
    
    // If current task is runnable and not exited, keep running it
    if (!start->has_exited && (start->state == TASK_READY || start->state == TASK_RUNNING)) {
        return start;
    }
    
    // Current task is blocked/stopped/zombie/exited, must switch
    if (idle_candidate) {
        return idle_candidate;
    }
    if (bootstrap_candidate) {
        return bootstrap_candidate;
    }
    
    // FATAL: No runnable task found - should never happen
    // At minimum, idle task should always be READY
    kprintf("FATAL: pick_next: no runnable task! start=%p state=%d\n",
            start, start ? start->state : -1);
    kprintf("  idle_candidate=%p bootstrap_candidate=%p\n",
            idle_candidate, bootstrap_candidate);
    for(;;) __asm__ volatile("hlt");
}

void sched_tick(void) {
    // In preemptive mode, time slice tracking is done in timer_irq_handler
    // This function is kept for backward compatibility and statistics
    if (!g_current) {
        return;
    }
    // Statistics only - preemption is handled by timer_irq_handler
}

// Debug: track schedule calls for watchdog
static volatile uint64_t g_total_schedules = 0;

// Core scheduling function - selects next task and switches to it
// Does NOT modify current task's state - caller must set it appropriately before calling
// (TASK_BLOCKED, TASK_ZOMBIE, TASK_READY, etc.)
void sched_schedule(void) {
    if (!g_current) {
        return;
    }
    
    // Acquire scheduler lock (for SMP safety)
    // Note: We don't use preempt_disable here because we're about to switch tasks
    // and each task needs its own preemption state
    spin_lock(&g_sched_lock);
    
    g_yield_count++;  // Track schedules
    g_total_schedules++;  // Debug counter
    
    task_t* next = pick_next(g_current, 0);  // from_timer=0
    if (next == g_current) {
        // No context switch needed, just reset time slice
        g_current->remaining_ticks = SCHED_TIME_SLICE;
        g_current->need_resched = 0;
        spin_unlock(&g_sched_lock);
        return;
    }
    
    if (!next || next->sp == 0) {
        kprintf("FATAL: sched_schedule next is NULL or sp is NULL!\n");
        spin_unlock(&g_sched_lock);
        for(;;) __asm__ volatile("hlt");
    }
    
    task_t* prev = g_current;
    g_current = next;
    
    // Update per-CPU current task for SMP
    if (g_smp_initialized) {
        set_current(next);
    }
    
    // Note: We do NOT touch prev->state here - caller already set it
    // (could be TASK_BLOCKED, TASK_ZOMBIE, TASK_STOPPED, or TASK_READY)
    
    next->state = TASK_RUNNING;
    next->remaining_ticks = SCHED_TIME_SLICE;
    next->need_resched = 0;
    
    spin_unlock(&g_sched_lock);
    
    switch_address_space(prev, next);
    ctx_switch_asm(&prev->sp, next->sp);
    
    // We return here when this task is scheduled again
    // g_preempt_count should be 0 for normal operation
}

void sched_run_ready(void) {
    // In preemptive mode, check if reschedule is needed
    if (!g_current || !sched_need_resched()) {
        return;
    }
    g_current->remaining_ticks = SCHED_TIME_SLICE;
    g_current->need_resched = 0;
    task_t* next = pick_next(g_current, 0);  // from_timer=0, exclude bootstrap
    
    if (!next) {
        return;
    }
    
    if (next == g_current || (is_idle_task(next) && !is_idle_task(g_current))) {
        return;
    }
    
    if (next->sp == 0) {
        kprintf("FATAL: next->sp is NULL!\n");
        for(;;) __asm__ volatile("hlt");
    }
    
    task_t* prev = g_current;
    g_current = next;
    
    // Update per-CPU current task for SMP
    if (g_smp_initialized) {
        set_current(next);
    }
    
    if (prev->state != TASK_ZOMBIE) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    switch_address_space(prev, next);
    ctx_switch_asm(&prev->sp, next->sp);
}

task_t* sched_current(void) {
    // When SMP is initialized, use per-CPU current task
    if (g_smp_initialized) {
        return current();  // From percpu.h - reads GS:offset
    }
    return g_current;
}

int sched_has_user_tasks(void) {
    // Check if there are any active user tasks (not idle, not zombie)
    if (!g_current) {
        return 0;
    }
    
    task_t* start = g_current;
    task_t* t = g_current;
    int count = 0;
    do {
        if (t->privilege == TASK_USER) {
            if ((t->state == TASK_READY || t->state == TASK_RUNNING || t->state == TASK_BLOCKED) && !is_idle_task(t)) {
                count++;
            }
        }
        t = t->next;
    } while (t && t != start);
    
    return count > 0;
}

static void task_trampoline(void) {
    task_t* cur = g_current;
    if (cur && cur->entry) {
        cur->entry(cur->arg);
    }
    cur->state = TASK_ZOMBIE;
    for (;;) {
        sched_schedule();
        __asm__ volatile ("hlt");
    }
}

static void idle_entry(void* arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
}

// ============================================================================
// PROCESS HIERARCHY MANAGEMENT
// ============================================================================

// Find a task by its ID
task_t* sched_find_task_by_id(uint32_t id) {
    if (!g_current) return NULL;
    
    task_t* start = g_current;
    task_t* t = g_current;
    do {
        if (t->id == id) {
            return t;
        }
        t = t->next;
    } while (t && t != start);
    
    return NULL;
}

// Add a child to parent's child list
void sched_add_child(task_t* parent, task_t* child) {
    if (!parent || !child) return;
    
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

// Remove a child from parent's child list
void sched_remove_child(task_t* parent, task_t* child) {
    if (!parent || !child || child->parent != parent) return;
    
    if (parent->first_child == child) {
        parent->first_child = child->next_sibling;
    } else {
        task_t* prev = parent->first_child;
        while (prev && prev->next_sibling != child) {
            prev = prev->next_sibling;
        }
        if (prev) {
            prev->next_sibling = child->next_sibling;
        }
    }
    child->parent = NULL;
    child->next_sibling = NULL;
}

// Reparent all children to init (task 0)
void sched_reparent_children(task_t* task) {
    if (!task) return;
    
    task_t* child = task->first_child;
    while (child) {
        task_t* next = child->next_sibling;
        child->parent = &g_bootstrap_task;  // Reparent to init (task 0)
        child->next_sibling = g_bootstrap_task.first_child;
        g_bootstrap_task.first_child = child;
        child = next;
    }
    task->first_child = NULL;
}

// Remove task from scheduler run queue
static void remove_from_runqueue(task_t* t) {
    if (!g_current || !t) return;
    
    // Can't remove current or idle
    if (t == g_current || t == &g_idle_task || t == &g_bootstrap_task) return;
    
    // Find predecessor in circular list
    task_t* prev = g_current;
    while (prev->next != t && prev->next != g_current) {
        prev = prev->next;
    }
    
    if (prev->next == t) {
        prev->next = t->next;
        t->next = NULL;
    }
}

// Remove a task completely (after being reaped)
void sched_remove_task(task_t* task) {
    if (!task || task == &g_bootstrap_task || task == &g_idle_task) return;
    
    // Remove from parent's child list
    if (task->parent) {
        sched_remove_child(task->parent, task);
    }
    
    // Reparent any children to init
    sched_reparent_children(task);
    
    // Remove from run queue
    remove_from_runqueue(task);
    
    // Close all open file descriptors
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (task->fd_table[i]) {
            // Check for console dup markers (magic pointers 1, 2, 3)
            uint64_t marker = (uint64_t)task->fd_table[i];
            if (marker >= 1 && marker <= 3) {
                // Just clear the marker, don't call vfs_close
                task->fd_table[i] = NULL;
            } else if (pipe_is_end(task->fd_table[i])) {
                pipe_close_end((pipe_end_t*)task->fd_table[i]);
                task->fd_table[i] = NULL;
            } else {
                vfs_close(task->fd_table[i]);
                task->fd_table[i] = NULL;
            }
        }
    }
    
    // Destroy address space if user task
    if (task->pml4) {
        mm_destroy_address_space(task->pml4);
        task->pml4 = NULL;
    }
    

    
    // Free kernel stack if allocated
    if (task->kernel_stack_base && task->privilege == TASK_USER) {
        kfree(task->kernel_stack_base);
    }
    
    // Free the task structure
    kfree(task);
}

// Fork the current task - creates child with cloned address space
task_t* sched_fork_current(void) {
    task_t* cur = sched_current();
    if (!cur || cur->privilege != TASK_USER) {
        return NULL;
    }
    
    // Allocate new task structure
    task_t* child = (task_t*)kalloc(sizeof(task_t));
    if (!child) {
        return NULL;
    }
    
    // Build list of MAP_SHARED regions for the clone function
    // Format: pairs of (start, end) addresses
    uint64_t shared_regions[TASK_MAX_MMAP * 2];
    int num_shared = 0;
    
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        if (cur->mmap_regions[i].in_use && 
            (cur->mmap_regions[i].flags & MAP_SHARED)) {
            shared_regions[num_shared * 2] = cur->mmap_regions[i].start;
            shared_regions[num_shared * 2 + 1] = cur->mmap_regions[i].start + cur->mmap_regions[i].length;
            num_shared++;
        }
    }
    
    // Clone address space with COW (but keep MAP_SHARED regions truly shared)
    uint64_t* child_pml4;
    if (num_shared > 0) {
        child_pml4 = mm_clone_address_space_with_shared(cur->pml4, shared_regions, num_shared);
    } else {
        child_pml4 = mm_clone_address_space(cur->pml4);
    }
    if (!child_pml4) {
        kfree(child);
        return NULL;
    }
    
    // Allocate kernel stack for child
    uint8_t* k_stack_mem = (uint8_t*)kalloc(8192);
    if (!k_stack_mem) {
        mm_destroy_address_space(child_pml4);
        kfree(child);
        return NULL;
    }
    
    // Align stack top to 16 bytes for ABI compliance
    uint64_t k_stack_top = ((uint64_t)(k_stack_mem + 8192)) & ~0xFUL;
    
    // Copy most fields from parent using mm_memcpy (no libc in kernel)
    mm_memcpy(child, cur, sizeof(task_t));
    
    // Set up child-specific fields
    child->id = g_next_id++;
    child->pml4 = child_pml4;
    child->state = TASK_READY;
    child->kernel_stack_top = k_stack_top;  // Use aligned stack top
    child->kernel_stack_base = k_stack_mem; // Save original allocation for freeing
    child->next = NULL;  // Will be set by enqueue_task
    
    // Process hierarchy
    child->parent = cur;
    child->first_child = NULL;
    child->next_sibling = NULL;
    child->exit_code = 0;
    child->has_exited = false;
    child->is_fork_child = true;  // Child should return 0 from fork
    child->wait_next = NULL;
    child->wait_channel = NULL;
    child->wakeup_tick = 0;
    
    // Copy signal handlers from parent (POSIX: inherited across fork)
    signal_fork_copy(child, cur);
    
    // Add to parent's child list
    sched_add_child(cur, child);
    
    // Duplicate file descriptors (increment refcounts)
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (cur->fd_table[i]) {
            // Check for console dup markers (magic pointers 1, 2, 3)
            uint64_t marker = (uint64_t)cur->fd_table[i];
            if (marker >= 1 && marker <= 3) {
                // Just copy the marker, don't call vfs_dup
                child->fd_table[i] = cur->fd_table[i];
            } else if (pipe_is_end(cur->fd_table[i])) {
                pipe_end_t* new_end = pipe_dup_end((pipe_end_t*)cur->fd_table[i]);
                child->fd_table[i] = (vfs_file_t*)new_end;
            } else {
                child->fd_table[i] = vfs_dup(cur->fd_table[i]);
            }
        } else {
            child->fd_table[i] = NULL;
        }
    }
    
    // Copy mmap regions (pages are COW, no need to change region metadata)
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        child->mmap_regions[i] = cur->mmap_regions[i];
    }
    
    // The kernel stack needs to be set up so that when we switch to child,
    // it will return from the fork syscall with return value 0.
    // We copy parent's kernel stack and adjust the return value.
    // Note: The actual implementation depends on how we enter this function
    // (via syscall). For now, we set up a fresh return path.
    
    // Set up child's kernel stack for returning from syscall
    // This will be done by the caller (sys_fork)
    
    // Add to scheduler run queue
    enqueue_task(child);
    
    return child;
}

// Get parent PID
uint32_t sched_get_ppid(task_t* task) {
    if (!task || !task->parent) {
        return 0;  // Init or no parent
    }
    return task->parent->id;
}

// Reap all zombie children of a parent task
void sched_reap_zombies(task_t* parent) {
    if (!parent) return;
    
    // Collect zombies first to avoid list corruption during removal
    #define MAX_ZOMBIES 32
    task_t* zombies[MAX_ZOMBIES];
    int zombie_count = 0;
    
    task_t* child = parent->first_child;
    while (child && zombie_count < MAX_ZOMBIES) {
        task_t* next = child->next_sibling;
        if (child->has_exited && child->state == TASK_ZOMBIE) {
            zombies[zombie_count++] = child;
        }
        child = next;
    }
    
    // Now remove all collected zombies
    for (int i = 0; i < zombie_count; i++) {
        sched_remove_task(zombies[i]);
    }
}

// Wake up a task that's waiting (blocked)
static void sched_wake_task(task_t* task) {
    if (task && task->state == TASK_BLOCKED) {
        task->state = TASK_READY;
        task->wait_channel = NULL;
        task->wakeup_tick = 0;
    }
}

// Wake all tasks waiting on a specific channel
void sched_wake_channel(void* channel) {
    if (!channel || !g_current) return;
    
    task_t* start = g_current;
    task_t* t = start;
    
    do {
        if (t->state == TASK_BLOCKED && t->wait_channel == channel) {
            t->state = TASK_READY;
            t->wait_channel = NULL;
        }
        t = t->next;
    } while (t && t != start);
}

// Wake all tasks whose sleep timer has expired
// Called from timer interrupt
void sched_wake_expired_sleepers(uint64_t current_tick) {
    if (!g_current) return;
    
    task_t* start = g_current;
    task_t* t = start;
    
    do {
        // Check signal timers for ALL tasks (not just current)
        // This ensures alarm() fires even when task is sleeping
        signal_check_timers(t, current_tick);
        
        // Check if task is sleeping and its timer has expired
        if (t->state == TASK_BLOCKED && t->wakeup_tick != 0) {
            if (current_tick >= t->wakeup_tick) {
                t->state = TASK_READY;
                t->wakeup_tick = 0;
            }
        }
        
        // Wake blocked tasks that have pending signals
        if (t->state == TASK_BLOCKED && signal_pending(t)) {
            t->state = TASK_READY;
            t->wakeup_tick = 0;
        }
        
        t = t->next;
    } while (t && t != start);
}

void sched_mark_task_exited(task_t* task, int status) {
    if (!task) {
        return;
    }
    task->exit_code = status;

    // Close all open file descriptors
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (task->fd_table[i]) {
            uint64_t marker = (uint64_t)task->fd_table[i];
            if (marker >= 1 && marker <= 3) {
                task->fd_table[i] = NULL;
            } else if (pipe_is_end(task->fd_table[i])) {
                pipe_close_end((pipe_end_t*)task->fd_table[i]);
                task->fd_table[i] = NULL;
            } else {
                vfs_close(task->fd_table[i]);
                task->fd_table[i] = NULL;
            }
        }
    }

    sched_reparent_children(task);

    __asm__ volatile ("cli");
    task->has_exited = true;
    task->state = TASK_ZOMBIE;
    
    // CRITICAL: Clear sp so this task can never be context-switched to again
    // This prevents resuming a zombie's saved kernel context which would
    // return through the exception handler and iret to the faulting instruction
    task->sp = 0;
    
    // Wake up parent if it's waiting in waitpid()
    if (task->parent && task->parent->state == TASK_BLOCKED) {
        // Check if parent is waiting for children (wait_channel == parent itself)
        if (task->parent->wait_channel == task->parent) {
            sched_wake_task(task->parent);
        }
    }
}

void sched_signal_task(task_t* task, int sig) {
    if (!task) {
        return;
    }
    
    // Build siginfo
    siginfo_t info;
    mm_memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = SI_USER;
    
    // SIGKILL and SIGSTOP cannot be caught or ignored
    if (sig == SIGKILL) {
        signal_send(task, sig, &info);
        task->exit_code = 128 + sig;
        sched_mark_task_exited(task, 128 + sig);
        if (task == sched_current()) {
            sched_schedule();
        }
        return;
    }
    
    if (sig == SIGSTOP) {
        task->state = TASK_STOPPED;
        signal_send(task, sig, &info);
        if (task == sched_current()) {
            sched_schedule();
        }
        return;
    }
    
    if (sig == SIGCONT) {
        if (task->state == TASK_STOPPED) {
            task->state = TASK_READY;
        }
        signal_send(task, sig, &info);
        return;
    }
    
    // For stop signals (SIGTSTP, SIGTTIN, SIGTTOU), check handler
    if (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        struct k_sigaction* act = &task->signals.action[sig];
        if (act->sa_handler == SIG_DFL) {
            // Default action: stop
            task->state = TASK_STOPPED;
            signal_send(task, sig, &info);
            if (task == sched_current()) {
                sched_schedule();
            }
            return;
        }
        // Has handler - send signal normally
        signal_send(task, sig, &info);
        // Wake if blocked
        if (task->state == TASK_BLOCKED) {
            task->state = TASK_READY;
        }
        return;
    }
    
    // For other signals (SIGINT, SIGTERM, SIGQUIT, etc.), check for handler
    struct k_sigaction* act = &task->signals.action[sig];
    
    if (act->sa_handler == SIG_IGN) {
        // Signal is ignored - do nothing
        return;
    }
    
    if (act->sa_handler != SIG_DFL) {
        // Has a user handler - send signal, don't kill
        signal_send(task, sig, &info);
        // Wake if blocked so it can handle the signal
        if (task->state == TASK_BLOCKED) {
            task->state = TASK_READY;
        }
        return;
    }
    
    // SIG_DFL - check default action
    int def_action = sig_default_action(sig);
    signal_send(task, sig, &info);
    
    switch (def_action) {
        case SIG_DFL_TERM:
        case SIG_DFL_CORE:
            // Default action is terminate
            task->exit_code = 128 + sig;
            sched_mark_task_exited(task, 128 + sig);
            if (task == sched_current()) {
                sched_schedule();
            }
            break;
        case SIG_DFL_STOP:
            task->state = TASK_STOPPED;
            if (task == sched_current()) {
                sched_schedule();
            }
            break;
        case SIG_DFL_IGN:
            // Ignore
            break;
        case SIG_DFL_CONT:
            if (task->state == TASK_STOPPED) {
                task->state = TASK_READY;
            }
            break;
    }
}

void sched_signal_pgrp(int pgid, int sig) {
    if (pgid <= 0 || !g_current) {
        return;
    }
    task_t* start = g_current;
    task_t* t = g_current;
    do {
        if (t->pgid == pgid && t->state != TASK_ZOMBIE) {
            sched_signal_task(t, sig);
        }
        t = t->next;
    } while (t && t != start);
}

int sched_pgid_exists(int pgid) {
    if (pgid <= 0 || !g_current) {
        return 0;
    }
    task_t* start = g_current;
    task_t* t = g_current;
    do {
        if (t->pgid == pgid && t->state != TASK_ZOMBIE) {
            return 1;
        }
        t = t->next;
    } while (t && t != start);
    return 0;
}

// Debug: Dump all tasks and their states
extern volatile uint64_t g_irq0_count;
extern volatile uint64_t g_total_irq_count;
extern uint64_t timer_ticks(void);

void sched_dump_tasks(void) {
    static const char* state_names[] = { "READY", "RUN", "BLOCK", "ZOMBIE", "STOP" };
    kprintf("\n=== Debug Dump (Ctrl+D) ===\n");
    kprintf("Ticks: %llu  IRQ0: %llu  TotalIRQ: %llu  Schedules: %llu\n", 
            timer_ticks(), g_irq0_count, g_total_irq_count, g_total_schedules);
    kprintf("FreeMem: %llu KB\n", mm_get_free_pages() * 4);
    kprintf("PID  PPID STATE   PGID  LastRIP          UserRIP\n");
    if (!g_current) {
        kprintf("(no tasks)\n");
        return;
    }
    task_t* start = g_current;
    task_t* t = g_current;
    do {
        const char* sn = (t->state <= TASK_STOPPED) ? state_names[t->state] : "???";
        int ppid = t->parent ? t->parent->id : 0;
        char marker = (t == g_current) ? '*' : ' ';
        // Get last kernel RIP from saved stack (ctx_switch_asm saves r15,r14,r13,r12,rbx,rbp, then ret addr at sp+48)
        uint64_t last_rip = 0;
        if (t->sp && t != g_current) {
            last_rip = t->sp[6];  // Return address is at offset 6 (after 6 pushed regs)
        }
        kprintf("%c%-4d %-4d %-7s %-4d  0x%016llx 0x%llx\n", marker, t->id, ppid, sn, t->pgid, last_rip, t->syscall_rip);
        t = t->next;
    } while (t && t != start);
    kprintf("===============================\n");
}

// ============ PREEMPTIVE SCHEDULING SUPPORT ============

// Check if the current task needs rescheduling
int sched_need_resched(void) {
    if (!g_current) return 0;
    return g_current->need_resched;
}

// Mark a task as needing rescheduling
void sched_set_need_resched(task_t* t) {
    if (t) {
        t->need_resched = 1;
    }
}

// Preemptive context switch called from timer interrupt
// This is called with interrupts disabled and must be very careful
void sched_preempt(interrupt_frame_t* frame) {
    if (!g_current) return;
    
    // Acquire scheduler lock (SMP-safe)
    // Use trylock to avoid deadlock if lock is held
    if (!spin_trylock(&g_sched_lock)) {
        // Lock held, skip this preemption attempt
        return;
    }
    
    // Clear the need_resched flag
    g_current->need_resched = 0;
    
    // Save the interrupt frame so we can resume later
    g_current->preempt_frame = frame;
    
    // Pick next task (use from_timer=1 for more aggressive scheduling)
    task_t* next = pick_next(g_current, 1);
    
    if (next == g_current || !next) {
        // No switch needed, just reset time slice
        g_current->remaining_ticks = SCHED_TIME_SLICE;
        g_current->preempt_frame = NULL;
        spin_unlock(&g_sched_lock);
        return;
    }
    
    if (next->sp == 0) {
        // Invalid target, don't switch
        g_current->remaining_ticks = SCHED_TIME_SLICE;
        g_current->preempt_frame = NULL;
        spin_unlock(&g_sched_lock);
        return;
    }
    
    // Reset time slice for next task
    next->remaining_ticks = SCHED_TIME_SLICE;
    
    // Perform the context switch
    task_t* prev = g_current;
    g_current = next;
    
    // Update per-CPU current task for SMP
    if (g_smp_initialized) {
        set_current(next);
    }
    
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    // Update preemption statistics
    g_preempt_count_total++;
    
    spin_unlock(&g_sched_lock);
    
    // Switch address space if needed
    switch_address_space(prev, next);
    
    // Perform the actual context switch
    // For preemption, we use the same ctx_switch_asm but
    // the registers are already saved on the interrupt stack
    ctx_switch_asm(&prev->sp, next->sp);
    
    // When we return here, this task has been resumed
    // Clear preempt_frame as we're no longer in preemption context
    if (g_current) {
        g_current->preempt_frame = NULL;
    }
}
// Enable SMP mode for scheduler
// Called by smp_init() after per-CPU data is set up
void sched_enable_smp(void) {
    // Set BSP's per-CPU current task before enabling SMP mode
    set_current(g_current);
    g_smp_initialized = 1;
    kprintf("Scheduler: SMP mode enabled\n");
}

// Get SMP mode status
int sched_is_smp(void) {
    return g_smp_initialized;
}