// LikeOS-64 Cooperative Scheduler
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

extern void user_mode_iret_trampoline(void);
extern void ctx_switch_asm(uint64_t** old_sp, uint64_t* new_sp);


#define SCHED_SLICE_TICKS 10
#define SCHED_BOOTSTRAP_INTERVAL 10  // Run bootstrap every N yields (was 50)

static task_t g_bootstrap_task;
static task_t g_idle_task;
static uint8_t g_idle_stack[4096] __attribute__((aligned(16)));
static uint8_t g_bootstrap_stack[8192] __attribute__((aligned(16)));
static task_t* g_current = 0;
static int g_next_id = 1;
static uint64_t g_slice_accum = 0;
static uint64_t g_yield_count = 0;  // Counter to ensure bootstrap runs periodically
static uint64_t* g_kernel_pml4 = 0;
static uint64_t g_default_kernel_stack = 0;

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
    g_current = &g_bootstrap_task;

    sched_add_task(idle_entry, 0, g_idle_stack, sizeof(g_idle_stack));
    kprintf("Scheduler initialized\n");
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
    (void)from_timer;
    if (!start || !start->next) {
        return start;
    }
    
    task_t* it = start->next;
    task_t* idle_candidate = 0;
    task_t* bootstrap_candidate = 0;
    
    // First, check the start task itself (in case it's bootstrap and ready)
    if (is_bootstrap_task(start) && (start->state == TASK_READY || start->state == TASK_RUNNING)) {
        bootstrap_candidate = start;
    }
    
    while (it != start) {
        if (it->state == TASK_READY || it->state == TASK_RUNNING) {
            // Prefer regular user tasks first
            if (!is_idle_task(it) && !is_bootstrap_task(it)) {
                g_yield_count = 0;  // Reset counter when switching to user task
                return it;
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
    
    // Periodically let bootstrap run for system housekeeping (shell, USB polling)
    // This prevents user tasks from completely starving the kernel loop
    if (bootstrap_candidate && g_yield_count >= SCHED_BOOTSTRAP_INTERVAL) {
        g_yield_count = 0;
        return bootstrap_candidate;
    }
    
    // If current task is a user task and ready, keep running it
    if (!is_idle_task(start) && !is_bootstrap_task(start) && 
        (start->state == TASK_READY || start->state == TASK_RUNNING)) {
        return start;
    }
    
    // Prefer bootstrap over idle
    // Bootstrap runs the main kernel loop (shell_tick, etc.)
    if (bootstrap_candidate) {
        return bootstrap_candidate;
    }
    if (idle_candidate) {
        return idle_candidate;
    }
    return start;
}

void sched_tick(void) {
    if (!g_current) {
        return;
    }
    g_slice_accum++;
}

// Debug: track yield calls for watchdog
static volatile uint64_t g_total_yields = 0;

void sched_yield(void) {
    if (!g_current) {
        return;
    }
    g_slice_accum = 0;
    g_yield_count++;  // Track yields to ensure bootstrap runs periodically
    g_total_yields++;  // Debug counter
    task_t* next = pick_next(g_current, 0);  // from_timer=0
    if (next == g_current) {
        return;
    }
    
    if (next->sp == 0) {
        kprintf("FATAL: sched_yield next->sp is NULL!\n");
        for(;;) __asm__ volatile("hlt");
    }
    
    task_t* prev = g_current;
    g_current = next;
    if (prev->state != TASK_ZOMBIE) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    switch_address_space(prev, next);
    ctx_switch_asm(&prev->sp, next->sp);
}

void sched_run_ready(void) {
    if (!g_current || g_slice_accum < SCHED_SLICE_TICKS) {
        return;
    }
    g_slice_accum = 0;
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
    if (prev->state != TASK_ZOMBIE) {
        prev->state = TASK_READY;
    }
    next->state = TASK_RUNNING;
    
    switch_address_space(prev, next);
    ctx_switch_asm(&prev->sp, next->sp);
}

task_t* sched_current(void) {
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
        sched_yield();
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
    
    // Clone address space with COW
    uint64_t* child_pml4 = mm_clone_address_space(cur->pml4);
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
    
    // Initialize signal state for child
    signal_init_task(child);
    
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
            sched_yield();
        }
        return;
    }
    
    if (sig == SIGSTOP) {
        task->state = TASK_STOPPED;
        signal_send(task, sig, &info);
        if (task == sched_current()) {
            sched_yield();
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
                sched_yield();
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
                sched_yield();
            }
            break;
        case SIG_DFL_STOP:
            task->state = TASK_STOPPED;
            if (task == sched_current()) {
                sched_yield();
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
    kprintf("Ticks: %llu  IRQ0: %llu  TotalIRQ: %llu  Yields: %llu\n", 
            timer_ticks(), g_irq0_count, g_total_irq_count, g_total_yields);
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
