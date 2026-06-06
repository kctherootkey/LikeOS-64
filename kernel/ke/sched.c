// LikeOS-64 Per-CPU Preemptive Scheduler
// ============================================================================
// SCHEDULER TYPE: O(1) RR with Preemption
// ============================================================================
//
// This is a simple O(1) preemptive RR scheduler with per-CPU run
// queues. Both enqueue and dequeue operations are O(1) using FIFO linked lists.
//
// Key characteristics:
//   - Time complexity: O(1) for enqueue, dequeue, and context switch
//   - Scheduling policy: RR with fixed time slices (SCHED_TIME_SLICE)
//   - Preemption: Timer-driven preemptive multitasking via sched_preempt()
//   - SMP support: True per-CPU run queues with load balancing
//   - No priority levels (all tasks are equal priority)
//
// Architecture:
//   - Each CPU has its own run queue (percpu->runqueue_head/tail) protected
//     by percpu->runqueue_lock.  schedule() / sched_preempt() only touch the
//     *local* CPU's queue – zero cross-CPU lock contention in the hot path.
//   - A global all-tasks linked list (g_task_list_head, via task->next)
//     protected by g_task_list_lock is used for administrative operations
//     (find_by_id, wake_channel, signal delivery, dump, etc.).  These are
//     cold paths where a global lock is acceptable.
//   - task->rq_next links the task into a per-CPU run queue.
//   - task->on_cpu records which CPU the task is assigned to.
//   - load_balance() is called periodically by the BSP timer to pull tasks
//     from the busiest CPU to the least-loaded one.
//
// Pre-SMP boot: sched_init() runs before percpu_init(), so per-CPU data is
// not yet available.  We use a simple legacy path until sched_enable_smp()
// is called by smp_init().

#include <kernel/ke/sched.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/interrupt.h>
#include <kernel/uapi/types.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/pipe.h>
#include <kernel/io/tty.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/futex.h>
#include <kernel/net/net.h>
#include <kernel/mm/slab.h>
#include <kernel/dev/rand/random.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/usb/usb_msd.h>

extern void user_mode_iret_trampoline(void);
extern void ctx_switch_asm(uint64_t** old_sp, uint64_t* new_sp);

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Preemption control – global counter (used pre-SMP; post-SMP uses per-CPU)
volatile int g_preempt_count = 0;

// Global all-tasks linked list (linear, via task->next)
static task_t* g_task_list_head = NULL;
spinlock_t g_task_list_lock = SPINLOCK_INIT("task_list");
// Approximate task count maintained atomically alongside the list.
// Updated under g_task_list_lock but readable lock-free for pre-allocation.
static int g_task_list_count = 0;

// SMP mode flag – when true, use per-CPU current task and run queues
int g_smp_initialized = 0;

// Legacy global current (pre-SMP only; post-SMP uses percpu->current_task)
static task_t* g_current = 0;

// PID allocator (non-static for sys_clone in syscall.c)
int g_next_id = 1;

// Kernel page table and default kernel stack
static uint64_t* g_kernel_pml4 = 0;
static uint64_t g_default_kernel_stack = 0;

// Current kernel stack for syscall handler is now per-CPU: percpu_t::syscall_kernel_rsp
// (set in switch_address_space, read by syscall_entry via GS:8)

// Debug/statistics
static volatile uint64_t g_total_schedules = 0;
static uint64_t g_preempt_count_total = 0;

// ============================================================================
// LOAD AVERAGE TRACKING
// ============================================================================
// Fixed-point arithmetic (shifted by 16 bits)
// Exponential decay coefficients for 5-second sampling:
//   1 min:  exp(-5/60)  * 65536 ≈ 60350
//   5 min:  exp(-5/300) * 65536 ≈ 64462
//  15 min:  exp(-5/900) * 65536 ≈ 65173
#define LOADAVG_FSHIFT  16
#define LOADAVG_FIXED_1 (1UL << LOADAVG_FSHIFT)
#define LOADAVG_EXP_1   60350UL   // 1 minute
#define LOADAVG_EXP_5   64462UL   // 5 minutes
#define LOADAVG_EXP_15  65173UL   // 15 minutes

static unsigned long g_loadavg[3] = {0, 0, 0};
static spinlock_t g_loadavg_lock = SPINLOCK_INIT("loadavg");
static uint64_t g_loadavg_last_tick = 0;

// ============================================================================
// DEAD THREAD REAPING
// ============================================================================
// Threads (exit_signal == 0) can never be waited on via waitpid, so they must
// be freed automatically.  We can't free them inside sched_mark_task_exited
// because the thread is still running on its own kernel stack.  Instead, after
// every context switch we check if the previous task was a zombie thread and
// free it — by that point we've switched to a different stack and it's safe.
#define DEAD_THREAD_MAX 64
static task_t* g_dead_threads[DEAD_THREAD_MAX];
static volatile int g_dead_thread_count = 0;
static spinlock_t g_dead_thread_lock = SPINLOCK_INIT("dead_threads");

// Bootstrap and BSP idle tasks (statically allocated)
static task_t g_bootstrap_task;
static task_t g_idle_task;
/* The idle task runs in ring 0, and because the IRQ entry stub does NOT
 * switch stacks when it interrupts CPL==0 code, every interrupt taken while a
 * CPU sits in its idle hlt loop runs its ENTIRE handler chain on this stack:
 * timer_irq_handler → sched_wake_expired_sleepers → sched_load_balance →
 * sched_preempt → ctx_switch, plus softirq_drain() (the full network RX path —
 * exactly what a long download generates), plus a possibly-nested reschedule
 * IPI.  The old 4 KB was far too small for that chain and had no guard page,
 * so an overflow silently corrupted adjacent heap (manifesting as a context
 * switch resuming through a clobbered pointer with current_task still == the
 * idle task).  Match the real per-task kernel-stack size. */
static uint8_t g_idle_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

// Per-AP idle tasks (dynamically allocated in sched_init_ap)
#define MAX_AP_IDLE 64
static task_t* g_ap_idle_tasks[MAX_AP_IDLE] = {0};
static uint8_t* g_ap_idle_stacks[MAX_AP_IDLE] = {0};

// Load balance interval (every N timer ticks per CPU)
#define LOAD_BALANCE_INTERVAL 50

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void task_trampoline(void);
static void idle_entry(void* arg);

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Validate user pointer is in user space
static inline bool validate_user_ptr(uint64_t ptr, size_t len) {
    if (ptr < 0x10000) return false;  // Reject low addresses (NULL deref protection)
    if (ptr >= 0x7FFFFFFFFFFF) return false;  // Beyond user space
    if (ptr + len < ptr) return false;  // Overflow check
    return true;
}

static inline int is_idle_task(const task_t* t) {
    if (t == &g_idle_task) return 1;
    for (int i = 0; i < MAX_AP_IDLE; i++) {
        if (g_ap_idle_tasks[i] == t) return 1;
    }
    return 0;
}

static inline int is_bootstrap_task(const task_t* t) {
    return t == &g_bootstrap_task;
}

/* Close every open fd on `task`.  Extracted from sched_mark_task_exited so
 * the same code can be re-used by dead_thread_reap() when the exit-path
 * was entered from IRQ context and had to defer the close (see the
 * fds_pending_close flag in struct task).  Callers MUST be in process
 * context — vfs_close → fat32_close → pagecache_flush_file may sleep on
 * the FAT32 sleeping mutex. */
static void task_close_open_files(task_t* task)
{
    if (!task) return;
    if (task->files) {
        for (int i = 0; i < TASK_MAX_FDS; i++) {
            if (task->fd_table[i]) {
                uint64_t marker = (uint64_t)task->fd_table[i];
                if (marker >= 1 && marker <= 3) {
                    /* stdio markers — no refcount */
                } else if (IS_SOCKET_FD(task->fd_table[i])) {
                    int idx = SOCKET_FD_IDX(task->fd_table[i]);
                    sock_close(idx);
                } else if (IS_UNIX_SOCKET_FD(task->fd_table[i])) {
                    int ufd = (int)(uintptr_t)task->fd_table[i];
                    unix_close(ufd);
                } else if (IS_EPOLL_FD(task->fd_table[i])) {
                    int idx = EPOLL_FD_IDX(task->fd_table[i]);
                    extern epoll_instance_t epoll_instances[];
                    if (idx >= 0 && idx < MAX_EPOLL_INSTANCES)
                        epoll_instances[idx].active = 0;
                } else if (pipe_is_end(task->fd_table[i])) {
                    pipe_close_end((pipe_end_t*)task->fd_table[i]);
                } else {
                    vfs_close(task->fd_table[i]);
                }
                task->fd_table[i] = NULL;
            }
        }
        files_struct_put(task->files);
        task->files = NULL;
    } else {
        for (int i = 0; i < TASK_MAX_FDS; i++) {
            if (task->fd_table[i]) {
                uint64_t marker = (uint64_t)task->fd_table[i];
                if (marker >= 1 && marker <= 3) {
                    task->fd_table[i] = NULL;
                } else if (IS_SOCKET_FD(task->fd_table[i])) {
                    int idx = SOCKET_FD_IDX(task->fd_table[i]);
                    task->fd_table[i] = NULL;
                    sock_close(idx);
                } else if (IS_UNIX_SOCKET_FD(task->fd_table[i])) {
                    int ufd = (int)(uintptr_t)task->fd_table[i];
                    task->fd_table[i] = NULL;
                    unix_close(ufd);
                } else if (IS_EPOLL_FD(task->fd_table[i])) {
                    int idx = EPOLL_FD_IDX(task->fd_table[i]);
                    task->fd_table[i] = NULL;
                    extern epoll_instance_t epoll_instances[];
                    if (idx >= 0 && idx < MAX_EPOLL_INSTANCES)
                        epoll_instances[idx].active = 0;
                } else if (pipe_is_end(task->fd_table[i])) {
                    pipe_close_end((pipe_end_t*)task->fd_table[i]);
                    task->fd_table[i] = NULL;
                } else {
                    vfs_close(task->fd_table[i]);
                    task->fd_table[i] = NULL;
                }
            }
        }
    }
}

// Queue a dead thread for deferred reaping (called from sched_mark_task_exited)
static void dead_thread_queue(task_t* task) {
    if (!task) return;
    uint64_t flags;
    spin_lock_irqsave(&g_dead_thread_lock, &flags);
    /* Reject duplicate insertions.  Two paths can both try to queue the
     * same zombie thread: (1) the deferred_zombie hand-off in
     * sched_schedule/sched_run_ready/sched_preempt when the thread exits
     * on its own CPU, and (2) sched_reparent_children when the thread's
     * parent is being torn down before dead_thread_reap got around to
     * processing the first queue entry.  Without this guard the second
     * reap pass would kfree() then dereference an already-freed task_t. */
    if (task->on_dead_queue) {
        spin_unlock_irqrestore(&g_dead_thread_lock, flags);
        return;
    }
    if (g_dead_thread_count < DEAD_THREAD_MAX) {
        g_dead_threads[g_dead_thread_count++] = task;
        task->on_dead_queue = true;
    } else {
        // Overflow – shouldn't happen unless many threads exit simultaneously.
        // Drop on the floor; leak is better than corruption.
        WARN_ON_ONCE(g_dead_thread_count >= DEAD_THREAD_MAX);  /* dead_thread_queue overflow: too many threads exiting simultaneously */
        kprintf("WARN: dead_thread_queue overflow (pid %d dropped)\n", task->id);
    }
    spin_unlock_irqrestore(&g_dead_thread_lock, flags);
}

// Reap all dead threads.  Called AFTER ctx_switch_asm when we are safely on a
// different kernel stack and can free the previous thread's stack.
static void dead_thread_reap(void) {
    if (g_dead_thread_count == 0) return;  // Fast path – no lock needed

    task_t* batch[DEAD_THREAD_MAX];
    int count;

    uint64_t flags;
    spin_lock_irqsave(&g_dead_thread_lock, &flags);
    count = g_dead_thread_count;
    for (int i = 0; i < count; i++) {
        batch[i] = g_dead_threads[i];
        /* Clear the queued flag while still under the lock.  After this
         * point a concurrent dead_thread_queue() for the same task would
         * succeed — which is the correct behavior, because we are about
         * to free this task and any later reference is a bug elsewhere. */
        if (batch[i]) batch[i]->on_dead_queue = false;
    }
    g_dead_thread_count = 0;
    spin_unlock_irqrestore(&g_dead_thread_lock, flags);

    for (int i = 0; i < count; i++) {
        /* If sched_mark_task_exited had to defer fd-closing because it
         * was called from IRQ context (signal_deliver_irq dispatching
         * SIG_DFL_TERM from the timer ISR — pagecache_flush_file would
         * otherwise sleep on the FAT32 mutex with IRQs disabled), do it
         * now.  dead_thread_reap is called from the scheduling-tail
         * paths in sched_schedule / sched_run_ready / sched_preempt
         * after ctx_switch_asm, when we're back on a normal task's
         * stack with IRQs enabled — sleeping is legal here. */
        if (batch[i]) {
            /* If the task was killed while holding a filesystem-private
             * sleeping lock (e.g. SIGINT on curl while it was sleeping
             * inside fat32_write_impl, waiting on a USB MSD completion —
             * confirmed via the debug dump showing
             * "FAT32 io_lock: held=1 owner=<dead-tid>"), force-release it
             * BEFORE the deferred fd-close tries to reacquire it.  The
             * dead owner can never run fat32_io_unlock, so without this
             * the next fat32_close → pagecache_flush_file blocks forever
             * and every subsequent FS op (including execve reading
             * /bin/<cmd>) hangs. */
            vfs_release_locks_for_task(batch[i]->id);

            if (batch[i]->fds_pending_close) {
                batch[i]->fds_pending_close = false;
                task_close_open_files(batch[i]);
            }
        }
        sched_remove_task(batch[i]);
    }
}

// ============================================================================
// ADDRESS SPACE SWITCHING
// ============================================================================

static inline void switch_address_space(task_t* prev, task_t* next) {
    BUG_ON(next == NULL);
    WARN_ON_ONCE(next->privilege == TASK_USER && next->kernel_stack_top == 0);  /* user task has no kernel stack: syscall/interrupt would overwrite user stack */
    (void)prev;  // Not used — we check actual hardware CR3 below
    uint64_t* next_pml4 = next->pml4 ? next->pml4 : g_kernel_pml4;

    if (next->privilege == TASK_USER && next->kernel_stack_top != 0) {
        tss_set_kernel_stack(next->kernel_stack_top);
        this_cpu()->syscall_kernel_rsp = next->kernel_stack_top;
    } else {
        // Use this CPU's kernel stack from TSS (not BSP's global default)
        uint64_t cpu_kernel_stack = tss_get_kernel_stack();
        tss_set_kernel_stack(cpu_kernel_stack);
        this_cpu()->syscall_kernel_rsp = cpu_kernel_stack;
    }

    // Compare against the ACTUAL hardware CR3, not prev->pml4.
    // After sched_mark_task_exited or cross-CPU SIGKILL, prev->pml4 may
    // be NULL while the real CR3 still holds the user PML4.  The old
    // comparison wrongly concluded "already on kernel PML4" and skipped
    // the switch, leaving a freed/stale PML4 in CR3.  When that page is
    // later recycled and zeroed, PML4[511] (kernel mapping) is wiped and
    // the next kernel instruction fetch causes a triple fault.
    uint64_t actual_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(actual_cr3));
    uint64_t next_phys = virt_to_phys(next_pml4);
    if ((actual_cr3 & ~0xFFFULL) != next_phys) {
        mm_switch_address_space(next_pml4);
    }
    
    // Load TLS (FS base) for the next task
    if (next->privilege == TASK_USER) {
        task_load_tls(next);
    }
}

// ============================================================================
// GLOBAL TASK LIST MANAGEMENT
// ============================================================================
// The global list links every task (regardless of state) via task->next.
// Protected by g_task_list_lock.  Used for admin operations only.

void task_list_add(task_t* t) {
    t->next = g_task_list_head;
    g_task_list_head = t;
    __atomic_fetch_add(&g_task_list_count, 1, __ATOMIC_RELAXED);
}

static void task_list_remove(task_t* t) {
    if (!t) return;
    if (g_task_list_head == t) {
        g_task_list_head = t->next;
        t->next = NULL;
        __atomic_fetch_sub(&g_task_list_count, 1, __ATOMIC_RELAXED);
        return;
    }
    for (task_t* prev = g_task_list_head; prev; prev = prev->next) {
        if (prev->next == t) {
            prev->next = t->next;
            t->next = NULL;
            __atomic_fetch_sub(&g_task_list_count, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

// ============================================================================
// PER-CPU RUN QUEUE MANAGEMENT
// ============================================================================
// Per-CPU run queues use task->rq_next for linkage.
// Only READY tasks live in a run queue.
// Caller MUST hold the target CPU's runqueue_lock.

static void rq_enqueue_locked(percpu_t* cpu, task_t* task) {
    lockdep_assert_held(&cpu->runqueue_lock);
    BUG_ON(cpu == NULL);
    BUG_ON(task == NULL);
    uint32_t cpu_id = cpu - percpu_get(0);
    
    // Prevent double-enqueue
    if (task->on_rq) {
        WARN_ON(task->on_rq);  /* double-enqueue detected */
        kprintf("BUG: double-enqueue pid %d cpu %u\n", task->id, cpu_id);
        return;
    }
    if (cpu->runqueue_head == task || cpu->runqueue_tail == task) {
        kprintf("BUG: pid %d already in queue cpu %u\n", task->id, cpu_id);
        return;
    }
    if (task->state == TASK_RUNNING) {
        WARN_ON(task->state == TASK_RUNNING);  /* enqueuing RUNNING task: CPU accounting bug */
        kprintf("BUG: enqueue RUNNING pid %d cpu %u\n", task->id, cpu_id);
        return;
    }
    
    task->rq_next = NULL;
    task->on_rq = true;
    if (cpu->runqueue_tail) {
        cpu->runqueue_tail->rq_next = task;
    } else {
        cpu->runqueue_head = task;
    }
    cpu->runqueue_tail = task;
    cpu->runqueue_length++;
}

static task_t* rq_dequeue_locked(percpu_t* cpu) {
    lockdep_assert_held(&cpu->runqueue_lock);
    BUG_ON(cpu == NULL);
    task_t* task = cpu->runqueue_head;
    if (task) {
        cpu->runqueue_head = task->rq_next;
        if (!cpu->runqueue_head) {
            cpu->runqueue_tail = NULL;
        }
        task->rq_next = NULL;
        task->on_rq = false;
        cpu->runqueue_length--;
        WARN_ON((long)cpu->runqueue_length < 0);  /* runqueue underflow */
        
        if (cpu->runqueue_head == task) {
            WARN_ON_ONCE(cpu->runqueue_head == task);  /* dequeued task still at runqueue_head: circular rq_next self-link */
            kprintf("BUG: circular list detected after dequeue pid %d!\n", task->id);
        }
    }
    return task;
}

// Remove a specific task from any run queue it might be on.
// Acquires the appropriate CPU's runqueue_lock.
static void rq_remove(task_t* task) {
    if (!task->on_rq) return;

    percpu_t* cpu = percpu_get(task->on_cpu);
    if (!cpu) return;

    uint64_t flags;
    spin_lock_irqsave(&cpu->runqueue_lock, &flags);

    if (!task->on_rq) {
        // Race: already removed
        spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
        return;
    }

    if (cpu->runqueue_head == task) {
        cpu->runqueue_head = task->rq_next;
        if (!cpu->runqueue_head) cpu->runqueue_tail = NULL;
    } else {
        for (task_t* prev = cpu->runqueue_head; prev; prev = prev->rq_next) {
            if (prev->rq_next == task) {
                prev->rq_next = task->rq_next;
                if (cpu->runqueue_tail == task) cpu->runqueue_tail = prev;
                break;
            }
        }
    }
    task->rq_next = NULL;
    task->on_rq = false;
    cpu->runqueue_length--;
    WARN_ON((long)cpu->runqueue_length < 0);  /* rq_remove: runqueue length underflow */

    spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
}

// Enqueue a READY task to its assigned CPU's run queue.
// If the target CPU is remote, send a reschedule IPI.
void sched_enqueue_ready(task_t* task) {
    if (!task || is_idle_task(task)) return;

    uint32_t target_cpu = task->on_cpu;
    percpu_t* cpu = percpu_get(target_cpu);
    
    if (!cpu) {
        target_cpu = 0;
        task->on_cpu = 0;
        cpu = percpu_get(0);
    }

    uint64_t flags;
    spin_lock_irqsave(&cpu->runqueue_lock, &flags);
    
    if (!task->on_rq && task->state == TASK_READY) {
        rq_enqueue_locked(cpu, task);
    }
    spin_unlock_irqrestore(&cpu->runqueue_lock, flags);

    // If enqueued to a remote CPU, send IPI to wake it from HLT
    if (g_smp_initialized && target_cpu != this_cpu_id()) {
        smp_send_reschedule(target_cpu);
    }
}

// ============================================================================
// TASK INITIALIZER HELPER
// ============================================================================

static void task_init_common(task_t* t) {
    BUG_ON(t == NULL);
    t->next = NULL;
    t->rq_next = NULL;
    t->on_rq = false;
    t->on_cpu = 0;
    t->cpu_affinity = 0;       // 0 = allowed on all CPUs
    t->user_stack_top = 0;
    t->kernel_stack_top = 0;
    t->kernel_stack_base = NULL;
    t->pgid = 0;
    t->sid = 0;
    t->ctty = NULL;
    t->wait_next = NULL;
    t->wait_channel = NULL;
    t->wakeup_tick = 0;
    t->need_resched = 0;
    t->remaining_ticks = SCHED_TIME_SLICE;
    t->preempt_frame = NULL;
    t->parent = NULL;
    t->first_child = NULL;
    t->next_sibling = NULL;
    t->exit_code = 0;
    t->has_exited = false;
    t->exit_lock = 0;
    t->is_fork_child = false;
    signal_init_task(t);
    t->comm[0] = '\0';
    t->cmdline[0] = '\0';
    t->environ[0] = '\0';
    t->start_tick = timer_ticks();
    t->utime_ticks = 0;
    t->stime_ticks = 0;
    t->cwd[0] = '/';
    t->cwd[1] = 0;
    for (int i = 0; i < TASK_MAX_FDS; i++) t->fd_table[i] = NULL;
    t->brk_start = 0;
    t->brk = 0;
    t->mmap_base = 0;
    for (int i = 0; i < TASK_MAX_MMAP; i++) t->mmap_regions[i].in_use = false;
    
    // Thread group support
    t->tgid = t->id;           // Will be set properly after id is assigned
    t->group_leader = t;       // Initially points to self
    t->thread_group_next = t;  // Circular list of one
    t->thread_group_prev = t;
    t->nr_threads = 1;
    t->group_exit_code = 0;
    t->group_exiting = false;
    t->exit_signal = SIGCHLD;  // Default for processes
    
    // CLONE_CHILD_CLEARTID support
    t->clear_child_tid = NULL;
    t->set_child_tid = NULL;
    
    // Per-task stack canary: unique per task, loaded into GS:104 on every
    // context switch so the task sees its own canary on any CPU.
    t->stack_canary = generate_stack_canary();

    // TLS support
    t->fs_base = 0;
    t->gs_base = 0;
    
    // Robust futex support
    t->robust_list = NULL;
    t->robust_list_len = 0;
    
    // Shared structures (NULL = use legacy per-task fields)
    t->mm = NULL;
    t->files = NULL;
    t->sighand = NULL;
}

// ============================================================================
// SCHEDULER INITIALISATION
// ============================================================================

void sched_init(void) {
    g_kernel_pml4 = mm_get_current_address_space();
    g_default_kernel_stack = tss_get_kernel_stack();

    // Bootstrap task (kernel main loop, always CPU 0)
    mm_memset(&g_bootstrap_task, 0, sizeof(task_t));
    g_bootstrap_task.sp = 0;
    g_bootstrap_task.pml4 = NULL;
    g_bootstrap_task.entry = 0;
    g_bootstrap_task.arg = 0;
    g_bootstrap_task.state = TASK_RUNNING;
    g_bootstrap_task.privilege = TASK_KERNEL;
    g_bootstrap_task.id = 0;
    task_init_common(&g_bootstrap_task);
    // The bootstrap task is already running: stack frames already contain the
    // current per-CPU canary.  Inherit it so epilogue checks keep passing.
    g_bootstrap_task.stack_canary = this_cpu()->stack_canary;
    g_bootstrap_task.on_cpu = 0;
    // Name the bootstrap/kernel task
    mm_memcpy(g_bootstrap_task.comm, "kernel", 7);

    // Legacy pre-SMP current
    g_current = &g_bootstrap_task;

    // Add bootstrap to global task list
    task_list_add(&g_bootstrap_task);

    // Create BSP idle task
    sched_add_task(idle_entry, 0, g_idle_stack, sizeof(g_idle_stack));
    // Name the BSP idle task (Linux-like)
    mm_memcpy(g_idle_task.comm, "kernel idle/0", 14);

    kprintf("Preemptive scheduler initialized (time slice=%d ticks)\n", SCHED_TIME_SLICE);
}

task_t* sched_add_task(task_entry_t entry, void* arg, void* stack_mem, size_t stack_size) {
    BUG_ON(entry == NULL);
    BUG_ON(stack_mem == NULL);
    BUG_ON(stack_size < 128);
    if (!entry || !stack_mem || stack_size < 128) return NULL;

    int is_idle = (entry == idle_entry);
    task_t* t = is_idle ? &g_idle_task : (task_t*)kalloc(sizeof(task_t));
    if (!t) return NULL;

    // Set up kernel stack with return to task_trampoline
    uint64_t* sp = (uint64_t*)((uint8_t*)stack_mem + stack_size);
    sp = (uint64_t*)((uint64_t)sp & ~0xFUL);
    *(--sp) = (uint64_t)task_trampoline;
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    mm_memset(t, 0, sizeof(task_t));
    t->sp = sp;
    t->pml4 = NULL;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->privilege = TASK_KERNEL;
    t->id = g_next_id++;
    task_init_common(t);
    t->on_cpu = 0;  // Default to BSP

    // Add to global task list
    task_list_add(t);

    // For idle task: don't add to run queue (it's the fallback)
    // For normal tasks: enqueue to run queue if SMP is ready
    if (!is_idle && g_smp_initialized) {
        t->on_cpu = percpu_find_least_loaded_cpu();
        t->state = TASK_READY;
        sched_enqueue_ready(t);
    }
    return t;
}

task_t* sched_add_user_task(task_entry_t entry, void* arg, uint64_t* pml4,
                            uint64_t user_stack, uint64_t kernel_stack) {
    BUG_ON(entry == NULL || pml4 == NULL);
    (void)kernel_stack;
    if (!entry || !pml4) return NULL;

    task_t* t = (task_t*)kalloc(sizeof(task_t));
    if (!t) return NULL;

    uint8_t* k_stack_mem = (uint8_t*)mm_alloc_guarded_kstack(KERNEL_STACK_SIZE);
    if (!k_stack_mem) { kfree(t); return NULL; }

    uint64_t k_stack_top = ((uint64_t)(k_stack_mem + KERNEL_STACK_SIZE)) & ~0xFUL;
    uint64_t* k_sp = (uint64_t*)k_stack_top;

    // IRET frame
    *(--k_sp) = 0x1B;                    // SS
    *(--k_sp) = user_stack;              // RSP
    *(--k_sp) = 0x202;                   // RFLAGS (IF)
    *(--k_sp) = 0x23;                    // CS
    *(--k_sp) = (uint64_t)entry;         // RIP
    // Callee-saved registers
    *(--k_sp) = (uint64_t)user_mode_iret_trampoline;
    *(--k_sp) = 0; *(--k_sp) = 0; *(--k_sp) = 0;
    *(--k_sp) = 0; *(--k_sp) = 0; *(--k_sp) = 0;

    mm_memset(t, 0, sizeof(task_t));
    t->sp = k_sp;
    t->pml4 = pml4;
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->privilege = TASK_USER;
    t->id = g_next_id++;
    WARN_ON_ONCE(g_next_id <= 0);  /* PID counter overflowed: too many tasks created or pid_t wrap-around */
    task_init_common(t);
    t->user_stack_top = user_stack;
    t->kernel_stack_top = k_stack_top;
    t->kernel_stack_base = k_stack_mem;
    t->pgid = t->id;
    t->sid = t->id;
    t->ctty = tty_get_console();
    t->brk_start = 0x500000;
    t->brk = t->brk_start;
    t->mmap_base = user_stack - (4 * 1024 * 1024);

    // Assign to least-loaded CPU
    if (g_smp_initialized) {
        t->on_cpu = percpu_find_least_loaded_cpu();
    } else {
        t->on_cpu = 0;
    }

    // Add to global task list
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    task_list_add(t);
    spin_unlock_irqrestore(&g_task_list_lock, flags);

    // NOTE: Do NOT enqueue here.  The caller (elf_exec) must call
    // sched_enqueue_ready(t) AFTER completing all per-task setup (e.g.
    // setup_user_tls_canary).  Enqueueing before setup_user_tls_canary
    // creates an SMP race: an AP can pick up the task immediately, run
    // arch_prctl to set fs_base = &_bootstrap_tls, and then the BSP
    // overwrites fs_base = USER_INITIAL_TLS_VA, corrupting TLS on the
    // next context switch and causing a false stack-protector mismatch.

    return t;
}

// ============================================================================
// CORE SCHEDULING (PER-CPU HOT PATH)
// ============================================================================

void sched_tick(void) {
    // Statistics only – time slice management is in timer_irq_handler
}

// In-kernel cooperative yield.  Mirrors sys_yield() but callable directly
// from kernel-mode busy-wait loops (e.g. loopback recv loops) where waiting
// only on timer preemption can starve a peer task pinned to the same CPU.
void sched_yield_in_kernel(void) {
    task_t* cur = sched_current();
    if (!cur) return;
    cur->remaining_ticks = 0;
    cur->state = TASK_READY;
    sched_schedule();
}

// Core scheduling function – select next task from local run queue and switch.
// Caller must set current task's state before calling (TASK_BLOCKED, TASK_ZOMBIE,
// TASK_READY, etc.).
void sched_schedule(void) {
    if (!g_smp_initialized) {
        // Pre-SMP: nothing to schedule yet (timer hasn't started)
        return;
    }

    percpu_t* cpu = this_cpu();
    task_t* cur = cpu->current_task;
    if (!cur) return;

    uint64_t flags;
    spin_lock_irqsave(&cpu->runqueue_lock, &flags);

    g_total_schedules++;

    // Dequeue next ready task from local queue FIRST
    task_t* next = rq_dequeue_locked(cpu);
    int my_cpu = this_cpu_id();

    // If we dequeued ourselves, it means we were blocked but got woken
    // before we reached sched_schedule().  This is a normal race between
    // the blocking path (set BLOCKED, release lock, call schedule) and
    // the wakeup path (set READY, enqueue).  Just keep running.
    if (next == cur) {
        cur->state = TASK_RUNNING;
        cur->remaining_ticks = SCHED_TIME_SLICE;
        cur->need_resched = 0;
        spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
        return;
    }

    // If we got nothing, try idle task
    if (!next) {
        next = cpu->idle_task;
    }

    // If still nothing or same as cur, stay on current
    if (!next || next == cur) {
        if (cur && !cur->has_exited) {
            cur->state = TASK_RUNNING;
            cur->remaining_ticks = SCHED_TIME_SLICE;
            cur->need_resched = 0;
        }
        spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
        return;
    }

    // Check sp BEFORE enqueueing cur — if next is unusable, fall through
    // to the idle task instead of bailing out entirely.  The old code
    // returned immediately, which left zombie/exited tasks stuck as
    // current_task because the only alternative (e.g. bootstrap, sp=0)
    // was always rejected.
    if (next->sp == 0 || next->state == TASK_ZOMBIE) {
        WARN_ON(next->state == TASK_ZOMBIE && !is_idle_task(next));  /* zombie dequeued onto CPU */
        if (!is_idle_task(next) && next->state != TASK_ZOMBIE) rq_enqueue_locked(cpu, next);
        next = cpu->idle_task;
        // idle_task might be cur (e.g. idle calling schedule) — handle below
    }

    // Final sanity: if next is still cur or NULL or invalid, stay on current
    if (!next || next == cur || next->sp == 0) {
        if (cur && !cur->has_exited) {
            cur->state = TASK_RUNNING;
            cur->remaining_ticks = SCHED_TIME_SLICE;
        }
        spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
        return;
    }

    // We have a valid different task to switch to
    // Now enqueue current if it's runnable (voluntary yield)
    // Check !cur->on_rq to avoid double-enqueue when wake_channel
    // already enqueued this task while it was still running as cur.
    if (!cur->has_exited &&
        (cur->state == TASK_READY || cur->state == TASK_RUNNING) &&
        !is_idle_task(cur) && !cur->on_rq) {
        cur->state = TASK_READY;
        rq_enqueue_locked(cpu, cur);
    }

    task_t* prev = cur;

    next->state = TASK_RUNNING;
    next->remaining_ticks = SCHED_TIME_SLICE;
    next->need_resched = 0;
    cpu->context_switches++;

    // Release lock but keep interrupts DISABLED through the context switch.
    spin_unlock(&cpu->runqueue_lock);

    // Switch address space BEFORE updating current_task.  sched_remove_task
    // on another CPU spins on (p->current_task == task) to decide when it is
    // safe to free the task's PML4.  If we update current_task first, the
    // remote CPU can free prev's PML4 while CR3 still holds it on this CPU,
    // leading to a stale PML4 kernel-text page fault when the freed page is
    // recycled and PML4[511] is zeroed.
    switch_address_space(prev, next);

    // CR3 no longer holds prev's PML4 — now safe to hand off current_task.
    cpu->current_task = next;
    set_current(next);

    /* Install the incoming task's canary into GS:104 BEFORE re-enabling
     * interrupts.  If we did this after `sti`, the post-`sti` window
     * would have current_task == next but GS:104 still holding prev's
     * canary.  Any IRQ handler or nested stack-protected callee entered
     * in that window would save prev's canary in its prologue, then see
     * next's canary in its epilogue once we get around to updating
     * GS:104, triggering a spurious __stack_chk_fail. */
    this_cpu()->stack_canary = next->stack_canary;

    // The timer-driven preempt handler must NOT preempt us between here and
    // ctx_switch_asm: it would see current_task == next and save the wrong
    // RSP into next->sp.  The in_context_switch flag tells sched_preempt
    // to skip this CPU.
    this_cpu()->in_context_switch = 1;
    __asm__ volatile("" ::: "memory");  // Compiler barrier — same-CPU store ordering is guaranteed on x86
    __asm__ volatile("sti");

    // Save zombie pointer BEFORE the switch — we are still on prev's stack.
    // Queue it AFTER ctx_switch_asm when we are on a safe stack.
    // If there is already an unprocessed deferred_zombie (e.g. because
    // fork_child_return skipped the post-switch check), queue it now
    // to prevent overwriting and leaking it.
    if (prev->state == TASK_ZOMBIE && prev->exit_signal == 0) {
        if (this_cpu()->deferred_zombie) {
            dead_thread_queue(this_cpu()->deferred_zombie);
        }
        this_cpu()->deferred_zombie = prev;
    }

    /* SMP double-run guard.  Zero next->sp BEFORE switching to it.  A task
     * whose sp == 0 is "committed to a CPU" and the sp==0 checks above make
     * every other CPU refuse to resume it.  ctx_switch_asm restores a valid
     * sp for `prev` at the instant it saves prev's frame, so a task only
     * becomes resumable again once it is fully off its kernel stack.
     *
     * Without this, a task T that blocks on CPU B is woken on CPU A during
     * the window between sched_schedule releasing runqueue_lock and
     * ctx_switch_asm saving T's context — A reads T's STALE saved sp and
     * resumes T while B is still executing on T's kernel stack.  Both CPUs
     * write the same stack; the saved trap frame near kernel_stack_top is
     * corrupted and the next return-to-user iretq consumes garbage
     * (observed as a userspace crash with RIP=0 and kernel register/RSP
     * values leaked into user mode). */
    uint64_t* next_sp = next->sp;
    next->sp = 0;
    ctx_switch_asm(&prev->sp, next_sp);

    // Resumed on the new task's stack.  Always clear the guard — the task
    // we just resumed could have been suspended from any scheduling path.
    this_cpu()->in_context_switch = 0;
    __asm__ volatile("sti");

    // Queue and reap deferred zombie now that we are on a safe stack.
    if (this_cpu()->deferred_zombie) {
        dead_thread_queue(this_cpu()->deferred_zombie);
        this_cpu()->deferred_zombie = NULL;
    }
    dead_thread_reap();

    // CRITICAL: if we resumed as a task that has since been zombied (e.g.
    // SIGKILL'd from another CPU while we were blocked mid-syscall, or
    // sched_mark_task_exited ran on our behalf), DO NOT return to the
    // caller — the caller is a syscall handler that still holds user
    // pointers from before the kill, and our pml4 has been freed.
    // switch_address_space() on the next scheduler entry will set CR3 to
    // g_kernel_pml4 (NULL pml4 fallback), and any copy_to_user attempt
    // would fault with cr2 in user space and current_task being us —
    // an unrecoverable kernel-mode #PF.  Re-enter the scheduler; with
    // state==ZOMBIE we will be skipped and another task picked.
    task_t* me = this_cpu()->current_task;
    if (me && (me->has_exited || me->state == TASK_ZOMBIE)) {
        for (;;) {
            sched_schedule();   /* should never return */
            __asm__ volatile("cli; hlt");
        }
    }
}

// Called from BSP main loop to check if reschedule is needed
void sched_run_ready(void) {
    if (!g_smp_initialized) return;

    task_t* cur = sched_current();
    if (!cur || !cur->need_resched) return;

    percpu_t* cpu = this_cpu();
    uint64_t flags;
    spin_lock_irqsave(&cpu->runqueue_lock, &flags);

    cur->need_resched = 0;

    // Check if there's anything to switch to
    if (!cpu->runqueue_head) {
        spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
        return;
    }

    // Dequeue next task FIRST
    task_t* next = rq_dequeue_locked(cpu);
    
    // If we got nothing or ourselves back, try idle task
    if (!next || next == cur) {
        if (next == cur) {
            kprintf("BUG: run_ready dequeued cur (pid %d)!\n", cur->id);
        }
        next = cpu->idle_task;
    }
    
    // If still nothing or same as cur, stay on current
    if (!next || next == cur) {
        if (cur && !cur->has_exited) { cur->state = TASK_RUNNING; cur->remaining_ticks = SCHED_TIME_SLICE; }
        spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
        return;
    }

    // Check sp BEFORE enqueueing cur — fall through to idle if next is bad
    if (next->sp == 0 || next->state == TASK_ZOMBIE) {
        if (!is_idle_task(next) && next->state != TASK_ZOMBIE) rq_enqueue_locked(cpu, next);
        next = cpu->idle_task;
    }

    if (!next || next == cur || next->sp == 0) {
        if (cur && !cur->has_exited) { cur->state = TASK_RUNNING; }
        spin_unlock_irqrestore(&cpu->runqueue_lock, flags);
        return;
    }

    // We have a valid different task - now enqueue current if runnable
    if (!cur->has_exited && !is_idle_task(cur) &&
        (cur->state == TASK_READY || cur->state == TASK_RUNNING)) {
        cur->state = TASK_READY;
        rq_enqueue_locked(cpu, cur);
    }

    task_t* prev = cur;

    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    next->remaining_ticks = SCHED_TIME_SLICE;
    next->need_resched = 0;
    cpu->context_switches++;

    // Release lock but keep interrupts DISABLED through the context switch
    // (same race prevention as in sched_schedule).
    spin_unlock(&cpu->runqueue_lock);

    // Switch address space before updating current_task (same rationale as
    // sched_schedule — see the detailed comment there).
    switch_address_space(prev, next);

    // CR3 no longer holds prev's PML4 — now safe to hand off current_task.
    cpu->current_task = next;
    set_current(next);

    /* Install the incoming task's canary into GS:104 BEFORE re-enabling
     * interrupts.  Same rationale as sched_schedule() — preventing a
     * post-`sti` window where current_task == next but GS:104 still
     * has prev's canary, which would cause any IRQ-context stack-
     * protected callee to save prev's canary and then later mismatch. */
    this_cpu()->stack_canary = next->stack_canary;

    // Same in_context_switch guard as sched_schedule (see comment there).
    this_cpu()->in_context_switch = 1;
    __asm__ volatile("" ::: "memory");
    __asm__ volatile("sti");

    // Save zombie pointer BEFORE the switch.
    // Flush any leftover deferred_zombie first (see sched_schedule comment).
    if (prev->state == TASK_ZOMBIE && prev->exit_signal == 0) {
        if (this_cpu()->deferred_zombie) {
            dead_thread_queue(this_cpu()->deferred_zombie);
        }
        this_cpu()->deferred_zombie = prev;
    }

    /* SMP double-run guard — zero next->sp before switching; see the
     * detailed comment in sched_schedule().  Prevents another CPU from
     * resuming a task while this CPU is still on its kernel stack. */
    uint64_t* next_sp = next->sp;
    next->sp = 0;
    ctx_switch_asm(&prev->sp, next_sp);

    // Resumed on the new task's stack.  Always clear the guard.
    this_cpu()->in_context_switch = 0;
    __asm__ volatile("sti");

    // Queue and reap deferred zombie now that we are on a safe stack.
    if (this_cpu()->deferred_zombie) {
        dead_thread_queue(this_cpu()->deferred_zombie);
        this_cpu()->deferred_zombie = NULL;
    }
    dead_thread_reap();
}

task_t* sched_current(void) {
    if (g_smp_initialized) {
        return current();  // percpu.h – reads GS:offset
    }
    return g_current;
}

int sched_has_user_tasks(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    for (task_t* t = g_task_list_head; t; t = t->next) {
        if (t->privilege == TASK_USER &&
            (t->state == TASK_READY || t->state == TASK_RUNNING || t->state == TASK_BLOCKED) &&
            !t->has_exited && !is_idle_task(t)) {
            spin_unlock_irqrestore(&g_task_list_lock, flags);
            return 1;
        }
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);
    return 0;
}

static void task_trampoline(void) {
    // We arrived here from ctx_switch_asm → ret (fresh task, never scheduled
    // before).  The scheduling function (sched_schedule / sched_run_ready) set
    // in_context_switch = 1 before ctx_switch_asm but the normal post-switch
    // cleanup (in_context_switch = 0, deferred zombie reap) was never executed
    // because we diverged to this trampoline instead of returning to the
    // scheduling function.  Clear the flag now, otherwise sched_preempt will
    // bail out on every timer tick and this CPU can never do preemptive
    // scheduling again.
    this_cpu()->in_context_switch = 0;

    // Process any deferred zombie from the previous task on this CPU.
    if (this_cpu()->deferred_zombie) {
        dead_thread_queue(this_cpu()->deferred_zombie);
        this_cpu()->deferred_zombie = NULL;
    }
    dead_thread_reap();

    task_t* cur = sched_current();
    if (cur && cur->entry) {
        cur->entry(cur->arg);
    }
    cur = sched_current();
    if (cur) {
        cur->state = TASK_ZOMBIE;
        cur->has_exited = true;
    }
    for (;;) {
        sched_schedule();
        __asm__ volatile("hlt");
    }
}

// Called from fork_child_return (assembly) to perform the same post-switch
// cleanup that sched_schedule / sched_run_ready / sched_preempt do after
// ctx_switch_asm.  Fresh tasks (fork/clone children) diverge to
// fork_child_return instead of returning to the scheduling function, so
// without this call in_context_switch stays permanently set to 1 on the
// CPU, blocking all future preemptive scheduling.
void sched_after_fork_child(void) {
    this_cpu()->in_context_switch = 0;

    // Load TLS for the child before returning to userspace.
    // The normal context-switch path calls task_load_tls() after
    // ctx_switch_asm, but fork_child_return bypasses that path.
    // Without this, FS_BASE MSR is stale/zero and the dynamic linker
    // crashes on the first lazy PLT resolution (%fs-relative access).
    task_t* child = sched_current();
    if (child && child->privilege == TASK_USER) {
        task_load_tls(child);
    }

    // Queue deferred zombie but do NOT reap here — fork_child_return calls
    // us before iretq with IRQs disabled.  Same deadlock risk as
    // sched_preempt: dead_thread_reap → sched_remove_task →
    // smp_tlb_shootdown_sync cannot safely run with IRQs off.
    if (this_cpu()->deferred_zombie) {
        dead_thread_queue(this_cpu()->deferred_zombie);
        this_cpu()->deferred_zombie = NULL;
    }
}

static void idle_entry(void* arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("sti");
        __asm__ volatile("hlt");
    }
}

// ============================================================================
// PROCESS HIERARCHY MANAGEMENT
// ============================================================================

task_t* sched_find_task_by_id(uint32_t id) {
    // Walk global task list
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    for (task_t* t = g_task_list_head; t; t = t->next) {
        if ((uint32_t)t->id == id) {
            spin_unlock_irqrestore(&g_task_list_lock, flags);
            return t;
        }
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);
    return NULL;
}

// Same but caller must hold g_task_list_lock
task_t* sched_find_task_by_id_locked(uint32_t id) {
    for (task_t* t = g_task_list_head; t; t = t->next) {
        if ((uint32_t)t->id == id)
            return t;
    }
    return NULL;
}

void sched_add_child(task_t* parent, task_t* child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

void sched_remove_child(task_t* parent, task_t* child) {
    if (!parent || !child || child->parent != parent) return;
    if (parent->first_child == child) {
        parent->first_child = child->next_sibling;
    } else {
        task_t* prev = parent->first_child;
        while (prev && prev->next_sibling != child) prev = prev->next_sibling;
        if (prev) prev->next_sibling = child->next_sibling;
    }
    child->parent = NULL;
    child->next_sibling = NULL;
}

void sched_reparent_children(task_t* task) {
    if (!task) return;
    task_t* child = task->first_child;
    while (child) {
        task_t* nxt = child->next_sibling;
        /* Bootstrap (PID 0) never calls waitpid.  If we'd reparent a
         * zombie to it, the zombie would live forever.  Queue it for
         * deferred reaping instead. */
        if (child->state == TASK_ZOMBIE) {
            child->parent = &g_bootstrap_task;   /* keep field consistent */
            child->next_sibling = NULL;
            dead_thread_queue(child);
        } else {
            child->parent = &g_bootstrap_task;
            child->next_sibling = g_bootstrap_task.first_child;
            g_bootstrap_task.first_child = child;
        }
        child = nxt;
    }
    task->first_child = NULL;
}

void sched_remove_task(task_t* task) {
    if (!task || task == &g_bootstrap_task || task == &g_idle_task) return;
    if (is_idle_task(task)) return;

    // SMP SAFETY: If the task is currently executing on another CPU, we must
    // wait for it to stop before freeing its resources (especially kernel stack).
    // This can happen when killing a task that's running on a different CPU.
    if (sched_is_smp()) {
        // Check if any CPU has this task as current_task
        uint32_t online = percpu_get_online_count();
        int retries = 0;
        const int max_retries = 10000;
        
        while (retries < max_retries) {
            bool still_running = false;
            for (uint32_t cpu = 0; cpu < online; cpu++) {
                percpu_t* p = percpu_get(cpu);
                if (p && p->current_task == task) {
                    still_running = true;
                    break;
                }
            }
            
            if (!still_running) {
                break;  // Task is not running on any CPU
            }
            
            // Task is still running - send reschedule IPI and yield
            smp_send_reschedule_all();
            
            // Brief pause to let the other CPU context switch
            for (volatile int i = 0; i < 1000; i++) {
                __asm__ volatile("pause" ::: "memory");
            }
            
            retries++;
        }
        
        if (retries >= max_retries) {
            kprintf("sched_remove_task: WARNING: task %d still running after %d retries\n",
                    task->id, max_retries);
            // SAFETY: Do NOT proceed with destruction!  The task's PML4 may
            // still be loaded in a CPU's CR3.  Destroying the address space
            // would free the PML4 page, which gets zeroed on reuse, wiping
            // PML4[511] (kernel mapping) and causing a triple fault.
            // Leak the task rather than crash the system.
            return;
        }
    }

    // Remove from parent's child list
    if (task->parent) sched_remove_child(task->parent, task);

    // Reparent children to init
    sched_reparent_children(task);

    // Remove from per-CPU run queue
    if (task->on_rq) rq_remove(task);

    /* If sched_mark_task_exited deferred fd-closing because it was called
     * from IRQ context (signal_deliver_irq dispatching SIG_DFL_TERM from
     * the keyboard ISR), do it now.  Without this, the FDs leak forever:
     * dead_thread_reap only handles thread zombies (exit_signal == 0); a
     * normal process like curl never goes through it, so its
     * fds_pending_close flag would be ignored.  Also force-release any
     * filesystem locks the dying task was holding (e.g. fat32_io_lock
     * mid-write while sleeping on a USB MSD completion) — otherwise
     * every subsequent FS op blocks forever on the orphaned mutex. */
    vfs_release_locks_for_task(task->id);
    if (task->fds_pending_close) {
        task->fds_pending_close = false;
        task_close_open_files(task);
    }

    // Remove from global task list
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    task_list_remove(task);
    spin_unlock_irqrestore(&g_task_list_lock, flags);

    // NOTE: File descriptors are NOT re-closed below.  sched_mark_task_exited()
    // already closed all FDs in the non-deferred path (or this function did
    // the deferred close just above).  Closing them again here would
    // double-free pipe ends / vfs_file_t objects whose memory may have been
    // reallocated.  The redundant close was the root cause of SLAB double-free
    // crashes observed on VMware SMP.

    // SMP barrier: Before destroying address space, ensure no other CPU has
    // TLB entries pointing to pages we're about to free. This prevents races
    // where another CPU uses stale TLB entries to access freed/reallocated pages.
    if (smp_is_enabled()) {
        smp_tlb_shootdown_sync();
    }

    // Release mm_struct (deferred from sched_mark_task_exited).
    // The spin-wait above guarantees the task is no longer running on any
    // CPU, so no CR3 register still references this PML4.
    if (task->mm) {
        mm_struct_put(task->mm);
        task->mm = NULL;
        task->pml4 = NULL;  // PML4 was owned by mm_struct
    }

    // Destroy address space (legacy path — tasks without mm_struct)
    if (task->pml4) {
        mm_destroy_address_space(task->pml4);
        task->pml4 = NULL;
    }

    // Free kernel stack - the TLB shootdown above already synchronized with
    // all CPUs, ensuring none are still in the context switch epilogue using
    // this task's kernel stack.
    if (task->kernel_stack_base && task->privilege == TASK_USER) {
        mm_free_guarded_kstack(task->kernel_stack_base, KERNEL_STACK_SIZE);
    }

    kfree(task);
}

// ============================================================================
// FORK
// ============================================================================

task_t* sched_fork_current(void) {
    task_t* cur = sched_current();
    if (!cur || cur->privilege != TASK_USER) return NULL;

    task_t* child = (task_t*)kalloc(sizeof(task_t));
    if (!child) return NULL;

    // Build shared region list for COW
    uint64_t shared_regions[TASK_MAX_MMAP * 2];
    int num_shared = 0;
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        if (cur->mmap_regions[i].in_use && (cur->mmap_regions[i].flags & MAP_SHARED)) {
            shared_regions[num_shared * 2] = cur->mmap_regions[i].start;
            shared_regions[num_shared * 2 + 1] = cur->mmap_regions[i].start + cur->mmap_regions[i].length;
            num_shared++;
        }
    }

    uint64_t* child_pml4;
    if (num_shared > 0) {
        child_pml4 = mm_clone_address_space_with_shared(cur->pml4, shared_regions, num_shared);
    } else {
        child_pml4 = mm_clone_address_space(cur->pml4);
    }
    if (!child_pml4) { kfree(child); return NULL; }

    uint8_t* k_stack_mem = (uint8_t*)mm_alloc_guarded_kstack(KERNEL_STACK_SIZE);
    if (!k_stack_mem) {
        mm_destroy_address_space(child_pml4);
        kfree(child);
        return NULL;
    }
    uint64_t k_stack_top = ((uint64_t)(k_stack_mem + KERNEL_STACK_SIZE)) & ~0xFUL;

    // Copy parent
    mm_memcpy(child, cur, sizeof(task_t));

    // Child-specific fields
    child->id = g_next_id++;
    child->pml4 = child_pml4;
    child->state = TASK_READY;
    child->kernel_stack_top = k_stack_top;
    child->kernel_stack_base = k_stack_mem;
    child->rq_next = NULL;
    child->on_rq = false;
    child->parent = cur;
    child->first_child = NULL;
    child->next_sibling = NULL;
    child->exit_code = 0;
    child->has_exited = false;
    child->exit_lock = 0;
    child->is_fork_child = true;
    child->wait_next = NULL;
    child->wait_channel = NULL;
    child->wakeup_tick = 0;
    child->need_resched = 0;
    child->remaining_ticks = SCHED_TIME_SLICE;
    child->preempt_frame = NULL;
    child->start_tick = timer_ticks();
    child->utime_ticks = 0;
    child->stime_ticks = 0;
    
    // Thread group: fork creates a new process (new thread group)
    thread_group_init(child);
    
    // Clear clone-related fields (not inherited)
    child->clear_child_tid = NULL;
    child->set_child_tid = NULL;
    child->robust_list = NULL;
    child->robust_list_len = 0;
    
    // TLS: fork preserves parent's TLS (fs_base/gs_base are already
    // copied by the mm_memcpy above).  The dynamic linker uses %fs for
    // lazy PLT resolution; zeroing fs_base leaves the child with a stale
    // FS_BASE MSR from whatever task previously ran on its CPU, causing
    // wrong symbol resolution or crashes on the first unresolved PLT call.
    
    // Shared structures: fork creates independent copies
    child->mm = NULL;      // We're using legacy pml4 field
    child->files = NULL;   // We're using legacy fd_table
    child->sighand = NULL; // We're using legacy signals.action

    // Assign child to PARENT's CPU.  This serialises parent and child
    // during the critical fork-to-exec window: the parent blocks in
    // waitpid almost immediately, so the child gets the CPU and can
    // complete setpgid → getpid → tcsetpgrp → execvp without any
    // cross-CPU races on COW pages or scheduler state.  The load
    // balancer will redistribute the child later if needed.
    // (Using percpu_find_least_loaded_cpu() here sent the child to a
    // remote CPU on systems with many cores—e.g. Dell 14-CPU—causing
    // races that never triggered on VMs or 4-CPU Lenovo hardware.)
    child->on_cpu = cur->on_cpu;

    // Copy signal handlers
    signal_fork_copy(child, cur);

    // Add to parent's child list
    sched_add_child(cur, child);

    // Duplicate file descriptors
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (cur->fd_table[i]) {
            uint64_t marker = (uint64_t)cur->fd_table[i];
            if (marker >= 1 && marker <= 3) {
                child->fd_table[i] = cur->fd_table[i];
            } else if (IS_SOCKET_FD(cur->fd_table[i])) {
                int idx = SOCKET_FD_IDX(cur->fd_table[i]);
                net_socket_t* s = sock_get(idx);
                if (s) __atomic_fetch_add(&s->ref_count, 1, __ATOMIC_ACQ_REL);
                child->fd_table[i] = cur->fd_table[i];
            } else if (IS_UNIX_SOCKET_FD(cur->fd_table[i])) {
                unix_socket_t* us = unix_get((int)(uintptr_t)cur->fd_table[i]);
                if (us) __atomic_fetch_add(&us->ref_count, 1, __ATOMIC_ACQ_REL);
                child->fd_table[i] = cur->fd_table[i];
            } else if (IS_EPOLL_FD(cur->fd_table[i])) {
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

    // Copy mmap regions
    for (int i = 0; i < TASK_MAX_MMAP; i++) {
        child->mmap_regions[i] = cur->mmap_regions[i];
    }

    // Add to global task list
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    task_list_add(child);
    spin_unlock_irqrestore(&g_task_list_lock, flags);

    // NOTE: Do NOT enqueue child here.  The caller (sys_fork) must first
    // set up child->sp (kernel stack with IRET frame) before the child
    // can be scheduled.  On SMP, enqueueing here races: another CPU picks
    // up the child and does ctx_switch_asm before sp is initialised,
    // reading garbage and crashing.

    return child;
}

// ============================================================================
// WAIT / WAKE / SLEEP
// ============================================================================

uint32_t sched_get_ppid(task_t* task) {
    if (!task || !task->parent) return 0;
    return task->parent->id;
}

void sched_reap_zombies(task_t* parent) {
    if (!parent) return;

    #define MAX_ZOMBIES 32
    task_t* zombies[MAX_ZOMBIES];
    int zombie_count = 0;

    task_t* child = parent->first_child;
    while (child && zombie_count < MAX_ZOMBIES) {
        task_t* nxt = child->next_sibling;
        if (child->has_exited && child->state == TASK_ZOMBIE) {
            zombies[zombie_count++] = child;
        }
        child = nxt;
    }

    for (int i = 0; i < zombie_count; i++) {
        sched_remove_task(zombies[i]);
    }
}

// Wake a single blocked task and enqueue it to its CPU's run queue
static void sched_wake_task(task_t* task) {
    if (task && task->state == TASK_BLOCKED) {
        task->state = TASK_READY;
        task->wait_channel = NULL;
        task->wakeup_tick = 0;
        sched_enqueue_ready(task);
    }
}

// Wake all tasks blocked on a channel
void sched_wake_channel(void* channel) {
    if (!channel) return;

    // Collect tasks to wake while holding the task list lock,
    // then enqueue them after releasing it (lock ordering: task_list before rq).
    task_t* to_wake[16];
    int nwake = 0;

    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    for (task_t* t = g_task_list_head; t; t = t->next) {
        if (t->state == TASK_BLOCKED && t->wait_channel == channel) {
            t->state = TASK_READY;
            t->wait_channel = NULL;
            if (nwake < 16) {
                to_wake[nwake++] = t;
            }
        }
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);

    // Enqueue only the tasks we actually woke (not a blanket READY scan).
    for (int i = 0; i < nwake; i++) {
        sched_enqueue_ready(to_wake[i]);
    }
}

// Wake tasks whose sleep timer has expired
void sched_wake_expired_sleepers(uint64_t current_tick) {
    // Collect tasks that we actually wake, then enqueue only those.
    // A blanket "READY + !on_rq" scan is dangerous on SMP because it can
    // re-enqueue a task that's currently RUNNING but hasn't been marked as
    // such yet (or was momentarily marked READY by a buggy caller).
    task_t* to_wake[16];
    int nwake = 0;

    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    for (task_t* t = g_task_list_head; t; t = t->next) {
        // Check signal timers for ALL tasks
        signal_check_timers(t, current_tick);

        // Check sleep timer
        if (t->state == TASK_BLOCKED && t->wakeup_tick != 0) {
            if (current_tick >= t->wakeup_tick) {
                t->state = TASK_READY;
                t->wakeup_tick = 0;
                if (nwake < 16) to_wake[nwake++] = t;
            }
        }

        // Wake blocked tasks with pending signals
        if (t->state == TASK_BLOCKED && signal_pending(t)) {
            t->state = TASK_READY;
            t->wakeup_tick = 0;
            if (nwake < 16) to_wake[nwake++] = t;
        }
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);

    // Enqueue only the tasks we actually transitioned from BLOCKED→READY
    for (int i = 0; i < nwake; i++) {
        sched_enqueue_ready(to_wake[i]);
    }
}

void sched_mark_task_exited(task_t* task, int status) {
    if (!task) return;

    // Atomic guard against concurrent calls from multiple CPUs.
    // Scenario: CPU 0 sends SIGKILL → sched_signal_task → sched_mark_task_exited.
    // Meanwhile CPU 1's timer fires → signal_deliver_irq dequeues the same SIGKILL
    // → sched_mark_task_exited.  Without an atomic guard, both CPUs enter the body,
    // close FDs twice → SLAB double-free.
    // Use atomic test-and-set on exit_lock (NOT has_exited) so that waitpid does
    // not see the task as "exited" until cleanup is fully complete.
    if (__sync_lock_test_and_set(&task->exit_lock, 1)) {
        return;  // Another CPU already started exit processing
    }

    task->exit_code = status;
    
    // ========================================================================
    // THREAD GROUP EXIT HANDLING
    // ========================================================================
    
    // Process robust futex list before anything else
    exit_robust_list(task);
    
    // Handle clear_child_tid (CLONE_CHILD_CLEARTID)
    // This writes 0 to the address and wakes any futex waiters.
    if (task->clear_child_tid) {
        /* The write is to a USER-SPACE address in the dying task's
         * address space.  sched_mark_task_exited can be called
         * cross-CPU (e.g. SIGINT from the keyboard IRQ on CPU 0
         * killing a curl thread that lives in a different mm on
         * CPU 5).  In that case the current CPU's CR3 is the
         * sender's (or the kernel) PML4 and the user address is
         * not mapped — writing it page-faults the kernel.
         * mm_user_addr_mapped uses current CR3, so it correctly
         * reports false in the cross-CPU case and we skip the write.
         * pthread_join still wakes via futex_wake_for_task below
         * (which uses the dying task's PML4 to compute the key). */
        if (mm_user_addr_mapped((uint64_t)task->clear_child_tid, sizeof(int))) {
            smap_disable();
            *(volatile int*)task->clear_child_tid = 0;
            smap_enable();
        }

        // Wake any threads waiting on this futex (pthread_join uses this).
        // Use futex_wake_for_task with the dying task's PML4, because
        // sched_mark_task_exited can be called cross-CPU (e.g. SIGKILL
        // from exit_group) where sched_current() is the sender, not the
        // victim.  Using the wrong PML4 produces a wrong futex key and
        // the wake silently fails to find the waiter.
        futex_wake_for_task((uint64_t)task->clear_child_tid, 1, task);
        task->clear_child_tid = NULL;
    }
    
    // Clean up any futex waiter entries belonging to this task.
    // If the task was killed (e.g. SIGKILL from exit_group) while BLOCKED
    // in futex_wait, its waiter entry is still in the hash bucket.  The
    // normal cleanup (run by the task after sched_schedule returns) will
    // never execute.  Stale entries silently consume wake slots in later
    // futex_wake calls and cause deadlocks.
    futex_cleanup_task(task);
    
    // Remove from thread group
    thread_group_remove(task);
    
    // CRITICAL: Do NOT release mm_struct here!  The PML4 owned by the
    // mm_struct may still be loaded in this CPU's (or another CPU's) CR3.
    // If mm_struct_put freed the address space now, the PML4 page would
    // return to the PT-pool freelist, and a later allocate_pt_page would
    // zero it — wiping PML4[511] (kernel-code mapping) and causing a
    // triple fault.  Defer mm_struct_put to sched_remove_task, which
    // first waits for the task to stop running on ALL CPUs.
    // (task->mm and task->pml4 are intentionally kept alive here.)
    
    /* Close all open fds.  CRITICAL: vfs_close on a regular file calls
     * fat32_close → pagecache_flush_file, which takes the FAT32 sleeping
     * mutex.  Sleeping is illegal with IRQs disabled — and one of the
     * paths into this function is signal_deliver_irq dispatching
     * SIG_DFL_TERM from the timer ISR, where IRQs are off.  Detect that
     * case by checking the IF flag and defer the close to
     * dead_thread_reap() (which runs in process context after the next
     * context switch).  Without this guard, Ctrl+C'ing curl mid-download
     * deadlocks the kernel with
     *   "WARNING: might_sleep() called with IRQs disabled at
     *    kernel/fs/pagecache.c:830 pagecache_flush_file()". */
    if (irqs_disabled()) {
        task->fds_pending_close = true;
    } else {
        task_close_open_files(task);
    }
    
    if (task->sighand) {
        sighand_struct_put(task->sighand);
        task->sighand = NULL;
    }

    sched_reparent_children(task);

    // Disable interrupts for the critical section of state changes
    uint64_t irq_flags = local_irq_save();

    // Remove from run queue if it's on one
    if (task->on_rq) rq_remove(task);

    // Signal to waitpid and other observers that exit processing is complete.
    task->has_exited = true;
    task->state = TASK_ZOMBIE;
    // NOTE: Do NOT set task->sp = 0 here! The task might still be running
    // on another CPU and will crash when trying to save context. The zombie
    // state check in scheduler functions prevents it from being scheduled.

    // Only send exit_signal (SIGCHLD) if:
    // 1. This is the thread group leader (or a normal process)
    // 2. All threads in the group have exited
    // Threads (exit_signal == 0) don't notify parent
    bool should_notify_parent = (task->exit_signal != 0);
    
    // For thread group leader, only notify when all threads have exited
    if (task == task->group_leader && task->nr_threads > 0) {
        should_notify_parent = false;  // Still have threads running
    }

    // Wake parent if blocked in waitpid
    if (should_notify_parent && task->parent && task->parent->state == TASK_BLOCKED) {
        if (task->parent->wait_channel == task->parent) {
            sched_wake_task(task->parent);
        }
    }

    /* Orphan reaping: if this task's parent is bootstrap (PID 0), no one
     * will ever waitpid() on it.  Mark it the same way thread zombies are
     * marked so that the next context switch defers it for reaping.  We
     * cannot free it here because we are still running on its stack. */
    if (task->parent == &g_bootstrap_task && task->exit_signal != 0 &&
        (task != task->group_leader || task->nr_threads == 0)) {
        /* Reuse the thread-zombie path: clearing exit_signal makes
         * sched_switch_to() route this task to dead_thread_queue. */
        task->exit_signal = 0;
    }

    local_irq_restore(irq_flags);
}

// ============================================================================
// SIGNAL DELIVERY
// ============================================================================

void sched_signal_task(task_t* task, int sig) {
    if (!task) return;

    // Skip already-exited/zombie tasks.  A cross-CPU kill or exception
    // may have already marked this task as exited.
    if (task->has_exited || task->state == TASK_ZOMBIE) return;

    siginfo_t info;
    mm_memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = SI_USER;

    if (sig == SIGKILL) {
        signal_send(task, sig, &info);
        task->exit_code = 128 + sig;
        sched_mark_task_exited(task, 128 + sig);
        // On SMP, do NOT call sched_schedule() here - the exception handler will
        // loop with 'sti; hlt' and the timer will safely preempt us. Calling
        // sched_schedule() while on this task's kernel stack creates a race:
        // another CPU can free the stack via waitpid before ctx_switch.
        // On single-CPU, this race can't happen, so schedule immediately.
        if (task == sched_current()) {
            if (!smp_is_enabled()) {
                sched_schedule();
            }
        }
        return;
    }

    if (sig == SIGSTOP) {
        if (task->on_rq) rq_remove(task);
        task->state = TASK_STOPPED;
        signal_send(task, sig, &info);
        if (task == sched_current()) sched_schedule();
        return;
    }

    if (sig == SIGCONT) {
        if (task->state == TASK_STOPPED) {
            task->state = TASK_READY;
            sched_enqueue_ready(task);
        }
        signal_send(task, sig, &info);
        return;
    }

    if (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        struct k_sigaction* act = &task->signals.action[sig];
        if (act->sa_handler == SIG_DFL) {
            if (task->on_rq) rq_remove(task);
            task->state = TASK_STOPPED;
            signal_send(task, sig, &info);
            if (task == sched_current()) sched_schedule();
            return;
        }
        signal_send(task, sig, &info);
        if (task->state == TASK_BLOCKED) {
            task->state = TASK_READY;
            sched_enqueue_ready(task);
        }
        return;
    }

    struct k_sigaction* act = &task->signals.action[sig];

    if (act->sa_handler == SIG_IGN) return;

    if (act->sa_handler != SIG_DFL) {
        signal_send(task, sig, &info);
        if (task->state == TASK_BLOCKED) {
            task->state = TASK_READY;
            sched_enqueue_ready(task);
        }
        return;
    }

    // SIG_DFL
    int def_action = sig_default_action(sig);
    signal_send(task, sig, &info);

    switch (def_action) {
        case SIG_DFL_TERM:
        case SIG_DFL_CORE:
            task->exit_code = 128 + sig;
            sched_mark_task_exited(task, 128 + sig);
            // On SMP, let timer preemption safely switch us off this stack.
            // On single-CPU, schedule immediately (no race possible).
            if (task == sched_current() && !smp_is_enabled()) {
                sched_schedule();
            }
            break;
        case SIG_DFL_STOP:
            if (task->on_rq) rq_remove(task);
            task->state = TASK_STOPPED;
            // Stopped tasks can safely schedule since they're not being freed
            if (task == sched_current()) sched_schedule();
            break;
        case SIG_DFL_IGN:
            break;
        case SIG_DFL_CONT:
            if (task->state == TASK_STOPPED) {
                task->state = TASK_READY;
                sched_enqueue_ready(task);
            }
            break;
    }
}

void sched_signal_pgrp(int pgid, int sig) {
    if (pgid <= 0) return;
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    // Collect targets first to avoid holding task_list_lock during signal delivery
    #define MAX_PGRP_TARGETS 32
    task_t* targets[MAX_PGRP_TARGETS];
    int count = 0;
    for (task_t* t = g_task_list_head; t && count < MAX_PGRP_TARGETS; t = t->next) {
        if (t->pgid == pgid && t->state != TASK_ZOMBIE && t->privilege != TASK_KERNEL) {
            targets[count++] = t;
        }
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);
    for (int i = 0; i < count; i++) {
        sched_signal_task(targets[i], sig);
    }
}

int sched_pgid_exists(int pgid) {
    if (pgid <= 0) return 0;
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    for (task_t* t = g_task_list_head; t; t = t->next) {
        if (t->pgid == pgid && t->state != TASK_ZOMBIE) {
            spin_unlock_irqrestore(&g_task_list_lock, flags);
            return 1;
        }
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);
    return 0;
}

// ============================================================================
// DEBUG DUMP
// ============================================================================

extern volatile uint64_t g_irq0_count;
extern volatile uint64_t g_total_irq_count;
extern uint64_t timer_ticks(void);

void sched_dump_tasks(struct tty *tty) {
    static const char* state_names[] = { "READY", "RUN", "BLOCK", "STOP", "ZOMBIE" };
    tty_printf(tty, "\n=== Debug Dump (Ctrl+D) ===\n");
    tty_printf(tty, "Ticks: %llu  IRQ0: %llu  TotalIRQ: %llu  Schedules: %llu\n",
            timer_ticks(), g_irq0_count, g_total_irq_count, g_total_schedules);
    tty_printf(tty, "FreeMem: %llu KB  CPUs: %u\n", mm_get_free_pages() * 4,
            percpu_get_online_count());

    // Per-CPU run queue stats - take locks to get consistent snapshot
    uint32_t online = percpu_get_online_count();
    for (uint32_t c = 0; c < online; c++) {
        percpu_t* cpu = percpu_get(c);
        if (cpu) {
            spin_lock(&cpu->runqueue_lock);
            int head_pid = cpu->runqueue_head ? ((task_t*)cpu->runqueue_head)->id : -1;
            uint32_t rq_len = cpu->runqueue_length;
            uint64_t ctx_sw = cpu->context_switches;
            spin_unlock(&cpu->runqueue_lock);
            tty_printf(tty, "  CPU%u: rq_len=%u ctx_sw=%llu head_pid=%d\n",
                    c, rq_len, ctx_sw, head_pid);
        }
    }

    tty_printf(tty, " TID  TGID PPID  CPU  State   onRQ  #Th  Ldr  lastRIP           userRIP            waitCh\n");
    tty_printf(tty, "----  ---- ----  ---  ------  ----  ---  ---  ----------------  ----------------  ----------------\n");

    /* Snapshot the task list while holding g_task_list_lock, then release
     * before calling tty_printf.  tty_write on a PTY slave calls
     * tty_wake_readers → sched_wake_channel which re-acquires
     * g_task_list_lock — holding it here would deadlock the system. */
    /* Allocate snapshot buffer sized to the current task count + slack for
     * any tasks created between the count read and the lock acquisition. */
    int snap_cap = __atomic_load_n(&g_task_list_count, __ATOMIC_RELAXED) + 16;
    struct {
        int id, tgid, ppid, on_cpu, on_rq, nr_threads;
        uint64_t last_rip, user_rip;
        void *wait_channel;
        uint8_t state;
        char is_leader, marker;
    } *snaps = slab_alloc((size_t)snap_cap * sizeof(*snaps));
    if (!snaps) {
        tty_printf(tty, "[sched_dump: out of memory]\n");
        return;
    }
    int nsnaps = 0;

    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    task_t* cur = sched_current();
    for (task_t* t = g_task_list_head; t && nsnaps < snap_cap; t = t->next) {
        snaps[nsnaps].id         = t->id;
        snaps[nsnaps].tgid       = t->tgid;
        snaps[nsnaps].ppid       = t->parent ? t->parent->id : 0;
        snaps[nsnaps].on_cpu     = t->on_cpu;
        snaps[nsnaps].on_rq      = t->on_rq;
        snaps[nsnaps].nr_threads = t->group_leader ? t->group_leader->nr_threads : 1;
        snaps[nsnaps].last_rip   = t->preempt_frame ? t->preempt_frame->rip : 0;
        snaps[nsnaps].user_rip   = t->syscall_rip;
        snaps[nsnaps].state      = (uint8_t)t->state;
        snaps[nsnaps].wait_channel = t->wait_channel;
        snaps[nsnaps].is_leader  = (t == t->group_leader) ? 'L' : '-';
        snaps[nsnaps].marker     = (t == cur) ? '*' : ' ';
        nsnaps++;
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);

    for (int i = 0; i < nsnaps; i++) {
        const char* sn = (snaps[i].state <= TASK_ZOMBIE) ? state_names[snaps[i].state] : "???";
        tty_printf(tty, "%c%3d  %4d %4d  %3u  %-6s  %4d  %3d   %c   %016lx  %016lx  %016lx\n",
                snaps[i].marker, snaps[i].id, snaps[i].tgid, snaps[i].ppid,
                snaps[i].on_cpu, sn, snaps[i].on_rq, snaps[i].nr_threads,
                snaps[i].is_leader, snaps[i].last_rip, snaps[i].user_rip,
                (uint64_t)snaps[i].wait_channel);
    }
    tty_printf(tty, "=======================================================================\n");
    /* Show FAT32 I/O lock state — useful to detect orphaned-lock hangs
     * (e.g., a task killed while holding the mutex). */
    {
        extern volatile int      fat32_io_locked;
        extern volatile int      fat32_io_depth;
        extern volatile uint64_t fat32_io_owner;
        tty_printf(tty, "FAT32 io_lock: held=%d depth=%d owner=%lld\n",
                fat32_io_locked, fat32_io_depth, (long long)fat32_io_owner);
    }
    /* Show USB MSD per-device lock states — fat32_io_lock is nested over
     * msd->io_locked; if curl dies mid-write both can orphan. */
    {
        extern usb_msd_device_t* g_msd_devices[8];
        extern int g_msd_count;
        for (int mi = 0; mi < g_msd_count && mi < 8; mi++) {
            usb_msd_device_t* m = g_msd_devices[mi];
            if (m) {
                tty_printf(tty, "MSD[%d] io_lock: held=%d owner=%lld\n",
                        mi, m->io_locked, (long long)m->io_owner);
            }
        }
    }
    slab_free(snaps);

    // Dump futex trace ring buffer for debugging missed wakeups
    // (only compiled in when FUTEX_TRACE_DEBUG is defined in futex.c)
    extern void futex_dump_trace(void);
    futex_dump_trace();
}

/* sched_print_tasks - lock-free task list dump to kprintf.
 * Called from kernel_oops/panic where locks may be broken.
 * Walks g_task_list_head without acquiring g_task_list_lock. */
void sched_print_tasks(void) {
    static const char* snames[] = { "READY", "RUN", "BLOCK", "STOP", "ZOMBIE" };
    kprintf("Scheduler state (no lock):\n");
    int total = 0, running = 0, blocked = 0;
    task_t *t = g_task_list_head;
    while (t && total < 256) {
        const char *s = (t->state < 5) ? snames[t->state] : "?";
        if (t->state == TASK_RUNNING) running++;
        else if (t->state == TASK_BLOCKED) blocked++;
        kprintf("  [%d] tgid=%d %-12s %-8s cpu=%u rip=%016llx\n",
                t->id, t->tgid, t->comm[0] ? t->comm : "(anon)", s,
                t->on_cpu,
                t->preempt_frame ? t->preempt_frame->rip : t->syscall_rip);
        t = t->next;
        total++;
    }
    kprintf("  total=%d running=%d blocked=%d\n", total, running, blocked);
}

// ============================================================================
// PREEMPTIVE SCHEDULING
// ============================================================================

int sched_need_resched(void) {
    task_t* cur = sched_current();
    if (!cur) return 0;
    return cur->need_resched;
}

void sched_set_need_resched(task_t* t) {
    if (t) t->need_resched = 1;
}

// Called from timer IRQ with interrupts disabled
void sched_preempt(interrupt_frame_t* frame) {
    if (!g_smp_initialized) return;

    percpu_t* cpu = this_cpu();
    
    // CRITICAL: If this CPU is between current_task update and ctx_switch_asm,
    // current_task already points to "next" but we are still on "prev"'s
    // kernel stack.  Preempting here would save the wrong RSP into next->sp,
    // permanently corrupting its saved stack pointer → triple fault later.
    if (cpu->in_context_switch) return;
    
    task_t* cur = cpu->current_task;
    if (!cur) return;
    
    // Don't preempt the bootstrap task - it's handling critical init code
    if (is_bootstrap_task(cur)) return;

    // Try-lock to avoid deadlock if lock is already held
    uint64_t flags = local_irq_save();
    if (!spin_trylock(&cpu->runqueue_lock)) {
        local_irq_restore(flags);
        return;
    }

    int my_cpu = this_cpu_id();
    cur->need_resched = 0;
    cur->preempt_frame = frame;

    // First, dequeue the next task to see what we're switching to
    task_t* next = rq_dequeue_locked(cpu);
    
    // If we dequeued ourselves (wake_channel enqueued cur while it was
    // still running), just stay on current — this is a normal race.
    if (next == cur) {
        cur->state = TASK_RUNNING;
        cur->remaining_ticks = SCHED_TIME_SLICE;
        cur->preempt_frame = NULL;
        spin_unlock(&cpu->runqueue_lock);
        local_irq_restore(flags);
        return;
    }
    
    // If queue was empty, try idle task
    if (!next) {
        next = cpu->idle_task;
    }

    // If next is still cur (idle == cur??) or no next, stay on current
    if (!next || next == cur) {
        if (cur && !cur->has_exited) {
            cur->state = TASK_RUNNING;
            cur->remaining_ticks = SCHED_TIME_SLICE;
        }
        cur->preempt_frame = NULL;
        spin_unlock(&cpu->runqueue_lock);
        local_irq_restore(flags);
        return;
    }

    // Check sp BEFORE enqueueing cur — fall through to idle if next is bad
    if (next->sp == 0 || next->state == TASK_ZOMBIE) {
        if (!is_idle_task(next) && next->state != TASK_ZOMBIE) rq_enqueue_locked(cpu, next);
        next = cpu->idle_task;
    }

    if (!next || next == cur || next->sp == 0) {
        if (cur && !cur->has_exited) {
            cur->remaining_ticks = SCHED_TIME_SLICE;
        }
        cur->preempt_frame = NULL;
        spin_unlock(&cpu->runqueue_lock);
        local_irq_restore(flags);
        return;
    }

    // We have a valid different task to switch to
    // Now enqueue current if it's runnable (not idle, not exited)
    // Check !cur->on_rq to avoid double-enqueue when wake_channel
    // already enqueued this task while it was still running as cur.
    if (!cur->has_exited &&
        (cur->state == TASK_READY || cur->state == TASK_RUNNING) &&
        !is_idle_task(cur) && !cur->on_rq) {
        cur->state = TASK_READY;
        rq_enqueue_locked(cpu, cur);
    }

    next->remaining_ticks = SCHED_TIME_SLICE;

    task_t* prev = cur;

    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    g_preempt_count_total++;
    cpu->context_switches++;

    // Release lock but keep interrupts disabled through the switch
    spin_unlock(&cpu->runqueue_lock);

    // Switch address space before updating current_task (same rationale as
    // sched_schedule — see the detailed comment there).
    switch_address_space(prev, next);

    // CR3 no longer holds prev's PML4 — now safe to hand off current_task.
    cpu->current_task = next;
    set_current(next);

    /* Install the incoming task's canary into GS:104 IMMEDIATELY after
     * publishing current_task = next, so there is no window where
     * current_task and GS:104 disagree.  IRQs are still off here
     * (sched_preempt was entered from a hardware interrupt), so the
     * window only matters if any code between this point and
     * ctx_switch_asm reads the canary — but keeping them in lockstep
     * is the simplest invariant. */
    this_cpu()->stack_canary = next->stack_canary;

    // CRITICAL SMP FIX: Save zombie pointer in per-CPU data BEFORE the switch.
    // Do NOT queue yet — we are still on prev's kernel stack.
    // Flush any leftover deferred_zombie first (see sched_schedule comment).
    if (prev->state == TASK_ZOMBIE && prev->exit_signal == 0) {
        if (this_cpu()->deferred_zombie) {
            dead_thread_queue(this_cpu()->deferred_zombie);
        }
        this_cpu()->deferred_zombie = prev;
    }

    /* SMP double-run guard — zero next->sp before switching; see the
     * detailed comment in sched_schedule().  Prevents another CPU from
     * resuming a task while this CPU is still on its kernel stack. */
    uint64_t* next_sp = next->sp;
    next->sp = 0;
    ctx_switch_asm(&prev->sp, next_sp);

    // Resumed from timer preemption. IF is left at 0 — the interrupt
    // epilogue (iretq in irq_common_stub) restores the correct RFLAGS
    // for this task when it exits the interrupt frame.
    //
    // CRITICAL: Always clear in_context_switch.  We don't know which
    // scheduling function originally suspended the task we just resumed;
    // if it was sched_schedule or sched_run_ready, they set the flag on
    // this CPU before ctx_switch_asm.  Leaving it set would permanently
    // block all future preemptions on this CPU.
    this_cpu()->in_context_switch = 0;
    task_t* resumed = sched_current();
    if (resumed) resumed->preempt_frame = NULL;

    // Queue deferred zombie now that we are on a safe stack.
    // NOTE: Do NOT call dead_thread_reap() here!  sched_preempt runs in
    // interrupt context (IRQs disabled by hardware on entry to the timer/IPI
    // handler).  dead_thread_reap → sched_remove_task → smp_tlb_shootdown_sync
    // sends IPIs and waits for remote acks.  If another CPU already holds
    // g_tlb_shootdown_lock and is waiting for *us* to ack its TLB shootdown,
    // we deadlock: we can't ack (IRQs off) and it can't release the lock
    // (waiting for our ack).  Reaping is deferred to the next voluntary
    // sched_schedule / sched_run_ready which runs with IRQs enabled.
    if (this_cpu()->deferred_zombie) {
        dead_thread_queue(this_cpu()->deferred_zombie);
        this_cpu()->deferred_zombie = NULL;
    }
}

// ============================================================================
// SMP INTEGRATION
// ============================================================================

// Called by smp_init() after per-CPU data is set up on BSP.
// Migrates existing tasks from the pre-SMP global state to per-CPU structures.
void sched_enable_smp(void) {
    percpu_t* bsp = this_cpu();

    // Set BSP's current_task
    bsp->current_task = g_current;
    set_current(g_current);

    // Set BSP's idle task
    bsp->idle_task = &g_idle_task;
    g_idle_task.on_cpu = 0;

    // Initialize syscall_kernel_rsp for BSP
    // This is critical for syscall handling in SMP mode
    bsp->syscall_kernel_rsp = tss_get_kernel_stack();

    // Enqueue all READY tasks (except idle and bootstrap) to CPU 0's run queue
    for (task_t* t = g_task_list_head; t; t = t->next) {
        if (t->state == TASK_READY && !is_idle_task(t) && !is_bootstrap_task(t)) {
            t->on_cpu = 0;
            uint64_t flags;
            spin_lock_irqsave(&bsp->runqueue_lock, &flags);
            rq_enqueue_locked(bsp, t);
            spin_unlock_irqrestore(&bsp->runqueue_lock, flags);
        }
    }

    g_smp_initialized = 1;
    kprintf("Scheduler: SMP mode enabled (per-CPU run queues active)\n");
}

int sched_is_smp(void) {
    return g_smp_initialized;
}

// Initialize per-CPU scheduler for an AP.
// Creates idle task, sets current_task, enables scheduling on that CPU.
void sched_init_ap(uint32_t cpu_id) {
    if (cpu_id == 0 || cpu_id >= MAX_AP_IDLE) return;

    percpu_t* cpu = this_cpu();

    // Allocate idle task + stack for this AP.  The AP idle loop runs IRQ +
    // softirq (network RX) + scheduler chains on this stack in ring 0 (the IRQ
    // stub does not switch stacks for CPL==0), so it needs the same size and
    // guard page as a real task kernel stack — a 4 KB unguarded stack
    // silently overflowed under load (see g_idle_stack comment).
    g_ap_idle_stacks[cpu_id] = (uint8_t*)mm_alloc_guarded_kstack(KERNEL_STACK_SIZE);
    if (!g_ap_idle_stacks[cpu_id]) {
        kprintf("sched_init_ap: failed to allocate idle stack for CPU %u\n", cpu_id);
        return;
    }

    task_t* idle = (task_t*)kalloc(sizeof(task_t));
    if (!idle) {
        mm_free_guarded_kstack(g_ap_idle_stacks[cpu_id], KERNEL_STACK_SIZE);
        g_ap_idle_stacks[cpu_id] = NULL;
        kprintf("sched_init_ap: failed to allocate idle task for CPU %u\n", cpu_id);
        return;
    }

    // Set up idle task kernel stack
    uint64_t* sp = (uint64_t*)(g_ap_idle_stacks[cpu_id] + KERNEL_STACK_SIZE);
    sp = (uint64_t*)((uint64_t)sp & ~0xFUL);
    *(--sp) = (uint64_t)task_trampoline;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    mm_memset(idle, 0, sizeof(task_t));
    idle->sp = sp;
    idle->pml4 = NULL;
    idle->entry = idle_entry;
    idle->arg = NULL;
    idle->state = TASK_RUNNING;  // Start as running (AP's initial task)
    idle->privilege = TASK_KERNEL;
    idle->id = g_next_id++;
    task_init_common(idle);
    /* The AP idle task is "already running": we set it current just below and
     * the AP falls straight into the idle loop on its own kernel stack.  Those
     * initial frames are therefore built with %gs:104 == this CPU's PER-CPU
     * canary, NOT the fresh random canary task_init_common() just assigned.
     * Inherit the per-CPU canary so that when the idle task is later resumed
     * through a normal context switch — which installs idle->stack_canary into
     * %gs:104 — the bringup frames' epilogue checks still match.  This mirrors
     * the bootstrap-task handling in sched_init().  Without it the first
     * preempt of an AP idle task trips a spurious __stack_chk_fail, seen as a
     * rare "KERNEL STACK SMASH" on a kernel idle/N task under heavy SMP load
     * (two different but valid canaries: frame holds the per-CPU value, GS:104
     * holds the random per-task value). */
    idle->stack_canary = cpu->stack_canary;
    idle->on_cpu = cpu_id;
    // Name the AP idle task (Linux-like: kernel idle/N)
    {
        char name[32] = "kernel idle/";
        int pos = 12; /* length of "kernel idle/" */
        if (cpu_id >= 10) name[pos++] = '0' + (cpu_id / 10);
        name[pos++] = '0' + (cpu_id % 10);
        name[pos] = '\0';
        for (int i = 0; i <= pos; i++) idle->comm[i] = name[i];
    }

    g_ap_idle_tasks[cpu_id] = idle;

    // Add idle task to global list
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    task_list_add(idle);
    spin_unlock_irqrestore(&g_task_list_lock, flags);

    // Set as this CPU's idle task and current task
    cpu->idle_task = idle;
    cpu->current_task = idle;
    set_current(idle);

    // Initialize syscall_kernel_rsp for this AP's kernel task
    // This is critical: without this, syscalls on this CPU before any
    // context switch would use an invalid (zero) kernel stack pointer
    cpu->syscall_kernel_rsp = tss_get_kernel_stack();

    kprintf("Scheduler: CPU %u initialized (idle task PID %d)\n", cpu_id, idle->id);
}

// ============================================================================
// LOAD BALANCING
// ============================================================================

// Pull-based load balancer.
// Called periodically from the timer on each CPU (or just BSP).
// Finds the busiest CPU and pulls one task from it to the calling CPU.
void sched_load_balance(void) {
    if (!g_smp_initialized) return;

    uint32_t online = percpu_get_online_count();
    if (online <= 1) return;

    uint32_t my_cpu = this_cpu_id();
    percpu_t* me = this_cpu();
    uint32_t my_len = me->runqueue_length;

    // Find busiest CPU
    uint32_t busiest_cpu = my_cpu;
    uint32_t busiest_len = my_len;
    for (uint32_t c = 0; c < online; c++) {
        if (c == my_cpu) continue;
        percpu_t* cpu = percpu_get(c);
        if (cpu && cpu->runqueue_length > busiest_len) {
            busiest_len = cpu->runqueue_length;
            busiest_cpu = c;
        }
    }

    // Pull if busiest has at least 1 more task than us
    if (busiest_cpu == my_cpu || busiest_len < my_len + 1) return;

    // Pull one task from the busiest CPU
    percpu_t* src = percpu_get(busiest_cpu);
    if (!src) return;

    // Lock ordering: always lock lower CPU ID first to prevent deadlock
    uint64_t flags;
    percpu_t* first = (my_cpu < busiest_cpu) ? me : src;
    percpu_t* second = (my_cpu < busiest_cpu) ? src : me;
    
    spin_lock_irqsave(&first->runqueue_lock, &flags);
    spin_lock(&second->runqueue_lock);

    // Re-check imbalance under locks (might have changed)
    if (src->runqueue_length < me->runqueue_length + 1) {
        spin_unlock(&second->runqueue_lock);
        spin_unlock_irqrestore(&first->runqueue_lock, flags);
        return;
    }

    // Find a migratable task:
    // - Must be READY state (not RUNNING - could be current on source CPU)
    // - Not idle or bootstrap
    // - User task (kernel tasks may have CPU affinity)
    // - Must be allowed to run on destination CPU (check cpu_affinity)
    task_t* prev_rq = NULL;
    task_t* migrate = NULL;
    for (task_t* t = src->runqueue_head; t; t = t->rq_next) {
        // Check affinity: 0 means all CPUs allowed, otherwise check bitmask
        bool affinity_ok = (t->cpu_affinity == 0) || 
                           (t->cpu_affinity & (1ULL << my_cpu));
        
        if (!is_idle_task(t) && !is_bootstrap_task(t) && 
            t->privilege == TASK_USER && t->state == TASK_READY && affinity_ok) {
            migrate = t;
            break;
        }
        prev_rq = t;
    }

    if (!migrate) {
        spin_unlock(&second->runqueue_lock);
        spin_unlock_irqrestore(&first->runqueue_lock, flags);
        return;
    }

    // Remove from source run queue
    if (prev_rq) {
        prev_rq->rq_next = migrate->rq_next;
    } else {
        src->runqueue_head = migrate->rq_next;
    }
    if (src->runqueue_tail == migrate) {
        src->runqueue_tail = prev_rq;
    }
    migrate->rq_next = NULL;
    migrate->on_rq = false;
    src->runqueue_length--;

    // Update on_cpu BEFORE adding to destination queue
    // Both locks are held, so no race with scheduler
    migrate->on_cpu = my_cpu;

    // Add to our run queue
    rq_enqueue_locked(me, migrate);

    spin_unlock(&second->runqueue_lock);
    spin_unlock_irqrestore(&first->runqueue_lock, flags);
}

// ============================================================================
// CPU FEATURE DETECTION
// ============================================================================

// Global CPU extended features (detected at boot)
uint32_t g_cpu_features_ext = 0;

// Detect extended CPU features (CPUID leaf 7)
void detect_cpu_features_ext(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // Check if CPUID leaf 7 is supported
    eax = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    if (eax < 7) {
        return;  // Leaf 7 not supported
    }
    
    // Query CPUID leaf 7, subleaf 0
    eax = 7;
    ecx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));
    
    // FSGSBASE: bit 0 of EBX
    if (ebx & (1 << 0)) {
        g_cpu_features_ext |= CPU_FEATURE_FSGSBASE;
    }
}

// Check if FSGSBASE instructions are supported
bool cpu_has_fsgsbase(void) {
    return (g_cpu_features_ext & CPU_FEATURE_FSGSBASE) != 0;
}

// Enable FSGSBASE if supported (must be called on each CPU)
void enable_fsgsbase(void) {
    if (!cpu_has_fsgsbase()) return;
    
    // Set CR4.FSGSBASE (bit 16)
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 16);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

// ============================================================================
// THREAD GROUP MANAGEMENT
// ============================================================================

// Initialize a task as a thread group leader (its own group of 1)
void thread_group_init(task_t* leader) {
    if (!leader) return;
    
    leader->tgid = leader->id;
    leader->group_leader = leader;
    leader->thread_group_next = leader;  // Circular list: points to self
    leader->thread_group_prev = leader;
    leader->nr_threads = 1;
    leader->exit_signal = SIGCHLD;  // Process exit sends SIGCHLD to parent
    leader->group_exiting = false;
    leader->group_exit_code = 0;
}

// Add a thread to an existing thread group
void thread_group_add(task_t* leader, task_t* thread) {
    if (!leader || !thread) return;
    
    // Thread inherits leader's tgid
    thread->tgid = leader->tgid;
    thread->group_leader = leader;
    thread->exit_signal = 0;  // Threads don't send signals on exit
    thread->group_exiting = false;
    
    // Insert into circular list (after leader)
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    
    thread->thread_group_next = leader->thread_group_next;
    thread->thread_group_prev = leader;
    leader->thread_group_next->thread_group_prev = thread;
    leader->thread_group_next = thread;
    leader->nr_threads++;
    
    spin_unlock_irqrestore(&g_task_list_lock, flags);
}

// Remove a thread from its thread group
void thread_group_remove(task_t* thread) {
    if (!thread || !thread->group_leader) return;
    
    task_t* leader = thread->group_leader;
    
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    
    // Remove from circular list
    thread->thread_group_prev->thread_group_next = thread->thread_group_next;
    thread->thread_group_next->thread_group_prev = thread->thread_group_prev;
    
    // Decrement thread count in leader
    if (leader->nr_threads > 0) {
        leader->nr_threads--;
    }
    
    // Clear thread's group links
    thread->thread_group_next = thread;
    thread->thread_group_prev = thread;
    
    spin_unlock_irqrestore(&g_task_list_lock, flags);
}

// Get number of threads in a thread group
int thread_group_count(task_t* task) {
    if (!task || !task->group_leader) return 1;
    return task->group_leader->nr_threads;
}

// Signal all threads in a thread group
void thread_group_signal_all(task_t* task, int sig) {
    if (!task || !task->group_leader) return;
    
    task_t* leader = task->group_leader;
    task_t* t = leader;
    
    do {
        sched_signal_task(t, sig);
        t = t->thread_group_next;
    } while (t != leader);
}

// ============================================================================
// SHARED STRUCTURE MANAGEMENT: mm_struct
// ============================================================================

// Create a new mm_struct with an existing PML4
mm_struct_t* mm_struct_create(uint64_t* pml4) {
    mm_struct_t* mm = (mm_struct_t*)kalloc(sizeof(mm_struct_t));
    if (!mm) return NULL;
    
    mm->pml4 = pml4;
    mm->refcount = 1;
    spinlock_init(&mm->lock, "mm");
    mm->brk_start = 0;
    mm->brk = 0;
    mm->mmap_base = 0;
    mm->mmap_regions = NULL;
    mm->mmap_count = 0;
    mm->mmap_capacity = 0;
    
    return mm;
}

// Clone an mm_struct (for fork - creates COW copy of address space)
mm_struct_t* mm_struct_clone(mm_struct_t* src) {
    if (!src) return NULL;
    
    uint64_t flags;
    spin_lock_irqsave(&src->lock, &flags);
    
    // Clone the address space with COW
    uint64_t* new_pml4 = mm_clone_address_space(src->pml4);
    if (!new_pml4) {
        spin_unlock_irqrestore(&src->lock, flags);
        return NULL;
    }
    
    mm_struct_t* mm = (mm_struct_t*)kalloc(sizeof(mm_struct_t));
    if (!mm) {
        mm_destroy_address_space(new_pml4);
        spin_unlock_irqrestore(&src->lock, flags);
        return NULL;
    }
    
    mm->pml4 = new_pml4;
    mm->refcount = 1;
    spinlock_init(&mm->lock, "mm");
    mm->brk_start = src->brk_start;
    mm->brk = src->brk;
    mm->mmap_base = src->mmap_base;
    
    // Clone mmap regions array if present
    if (src->mmap_regions && src->mmap_count > 0) {
        mm->mmap_regions = (mmap_region_t*)kalloc(src->mmap_capacity * sizeof(mmap_region_t));
        if (mm->mmap_regions) {
            mm_memcpy(mm->mmap_regions, src->mmap_regions, 
                      src->mmap_count * sizeof(mmap_region_t));
            mm->mmap_count = src->mmap_count;
            mm->mmap_capacity = src->mmap_capacity;
        }
    } else {
        mm->mmap_regions = NULL;
        mm->mmap_count = 0;
        mm->mmap_capacity = 0;
    }
    
    spin_unlock_irqrestore(&src->lock, flags);
    return mm;
}

// Increment mm_struct reference count
void mm_struct_get(mm_struct_t* mm) {
    if (!mm) return;
    __atomic_fetch_add(&mm->refcount, 1, __ATOMIC_SEQ_CST);
}

// Decrement mm_struct reference count, free if it reaches 0
void mm_struct_put(mm_struct_t* mm) {
    if (!mm) return;
    
    int old = __atomic_fetch_sub(&mm->refcount, 1, __ATOMIC_SEQ_CST);
    if (old == 1) {
        // Last reference - free the address space
        // CRITICAL: TLB shootdown BEFORE destroying address space!
        // Other CPUs may still have stale TLB entries pointing to pages
        // we're about to free. Without this, those CPUs could access
        // freed/reallocated memory causing corruption or triple faults.
        if (smp_is_enabled()) {
            smp_tlb_shootdown_sync();
        }
        if (mm->pml4) {
            mm_destroy_address_space(mm->pml4);
        }
        if (mm->mmap_regions) {
            kfree(mm->mmap_regions);
        }
        kfree(mm);
    }
}

// ============================================================================
// SHARED STRUCTURE MANAGEMENT: files_struct
// ============================================================================

// Create a new files_struct
files_struct_t* files_struct_create(void) {
    files_struct_t* files = (files_struct_t*)kalloc(sizeof(files_struct_t));
    if (!files) return NULL;
    
    files->refcount = 1;
    spinlock_init(&files->lock, "files");
    
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        files->fd_table[i] = NULL;
    }
    
    return files;
}

// Clone a files_struct (for fork - duplicates all file descriptors)
files_struct_t* files_struct_clone(files_struct_t* src) {
    if (!src) return NULL;
    
    files_struct_t* files = files_struct_create();
    if (!files) return NULL;
    
    uint64_t flags;
    spin_lock_irqsave(&src->lock, &flags);
    
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (src->fd_table[i]) {
            uint64_t marker = (uint64_t)src->fd_table[i];
            if (marker >= 1 && marker <= 3) {
                // Stdin/stdout/stderr markers
                files->fd_table[i] = src->fd_table[i];
            } else if (IS_SOCKET_FD(src->fd_table[i])) {
                int idx = SOCKET_FD_IDX(src->fd_table[i]);
                net_socket_t* s = sock_get(idx);
                if (s) __atomic_fetch_add(&s->ref_count, 1, __ATOMIC_ACQ_REL);
                files->fd_table[i] = src->fd_table[i];
            } else if (IS_UNIX_SOCKET_FD(src->fd_table[i])) {
                unix_socket_t* us = unix_get((int)(uintptr_t)src->fd_table[i]);
                if (us) __atomic_fetch_add(&us->ref_count, 1, __ATOMIC_ACQ_REL);
                files->fd_table[i] = src->fd_table[i];
            } else if (IS_EPOLL_FD(src->fd_table[i])) {
                files->fd_table[i] = src->fd_table[i];
            } else if (pipe_is_end(src->fd_table[i])) {
                // Pipe end - duplicate it
                pipe_end_t* new_end = pipe_dup_end((pipe_end_t*)src->fd_table[i]);
                files->fd_table[i] = (vfs_file_t*)new_end;
            } else {
                // Regular file - duplicate it
                files->fd_table[i] = vfs_dup(src->fd_table[i]);
            }
        }
    }
    
    spin_unlock_irqrestore(&src->lock, flags);
    return files;
}

// Increment files_struct reference count
void files_struct_get(files_struct_t* files) {
    if (!files) return;
    __atomic_add_fetch(&files->refcount, 1, __ATOMIC_SEQ_CST);
}

// Decrement files_struct reference count, close all files and free if it reaches 0
void files_struct_put(files_struct_t* files) {
    if (!files) return;
    
    int old = __atomic_fetch_sub(&files->refcount, 1, __ATOMIC_SEQ_CST);
    if (old == 1) {
        // Last reference - close all files
        for (int i = 0; i < TASK_MAX_FDS; i++) {
            if (files->fd_table[i]) {
                uint64_t marker = (uint64_t)files->fd_table[i];
                if (marker >= 1 && marker <= 3) {
                    files->fd_table[i] = NULL;
                } else if (IS_SOCKET_FD(files->fd_table[i])) {
                    int idx = SOCKET_FD_IDX(files->fd_table[i]);
                    files->fd_table[i] = NULL;
                    sock_close(idx);
                } else if (IS_UNIX_SOCKET_FD(files->fd_table[i])) {
                    int ufd = (int)(uintptr_t)files->fd_table[i];
                    files->fd_table[i] = NULL;
                    unix_close(ufd);
                } else if (IS_EPOLL_FD(files->fd_table[i])) {
                    int idx = EPOLL_FD_IDX(files->fd_table[i]);
                    files->fd_table[i] = NULL;
                    extern epoll_instance_t epoll_instances[];
                    if (idx >= 0 && idx < MAX_EPOLL_INSTANCES)
                        epoll_instances[idx].active = 0;
                } else if (pipe_is_end(files->fd_table[i])) {
                    pipe_close_end((pipe_end_t*)files->fd_table[i]);
                } else {
                    vfs_close(files->fd_table[i]);
                }
                files->fd_table[i] = NULL;
            }
        }
        kfree(files);
    }
}

// ============================================================================
// SHARED STRUCTURE MANAGEMENT: sighand_struct
// ============================================================================

// Create a new sighand_struct
sighand_struct_t* sighand_struct_create(void) {
    sighand_struct_t* sighand = (sighand_struct_t*)kalloc(sizeof(sighand_struct_t));
    if (!sighand) return NULL;
    
    sighand->refcount = 1;
    spinlock_init(&sighand->lock, "sighand");
    
    // Initialize all handlers to default
    for (int i = 0; i < 65; i++) {
        sighand->action[i].sa_handler = SIG_DFL;
        sighand->action[i].sa_flags = 0;
        sighand->action[i].sa_restorer = NULL;
        sighand->action[i].sa_mask.sig[0] = 0;
    }
    
    return sighand;
}

// Clone a sighand_struct (for fork - copies signal handlers)
sighand_struct_t* sighand_struct_clone(sighand_struct_t* src) {
    if (!src) return NULL;
    
    sighand_struct_t* sighand = (sighand_struct_t*)kalloc(sizeof(sighand_struct_t));
    if (!sighand) return NULL;
    
    sighand->refcount = 1;
    spinlock_init(&sighand->lock, "sighand");
    
    uint64_t flags;
    spin_lock_irqsave(&src->lock, &flags);
    
    for (int i = 0; i < 65; i++) {
        sighand->action[i] = src->action[i];
    }
    
    spin_unlock_irqrestore(&src->lock, flags);
    return sighand;
}

// Increment sighand_struct reference count
void sighand_struct_get(sighand_struct_t* sighand) {
    if (!sighand) return;
    __atomic_add_fetch(&sighand->refcount, 1, __ATOMIC_SEQ_CST);
}

// Decrement sighand_struct reference count, free if it reaches 0
void sighand_struct_put(sighand_struct_t* sighand) {
    if (!sighand) return;
    
    int old = __atomic_fetch_sub(&sighand->refcount, 1, __ATOMIC_SEQ_CST);
    if (old == 1) {
        kfree(sighand);
    }
}

// ============================================================================
// TLS (THREAD LOCAL STORAGE) SUPPORT
// ============================================================================

// MSR addresses for FS/GS base
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Set FS base for a task
void task_set_fs_base(task_t* task, uint64_t base) {
    if (!task) return;
    task->fs_base = base;
}

// Get FS base for a task
uint64_t task_get_fs_base(task_t* task) {
    if (!task) return 0;
    return task->fs_base;
}

// Load TLS (FS base) on context switch
// Called when switching to a user task
void task_load_tls(task_t* task) {
    if (!task || task->fs_base == 0) return;
    
    if (cpu_has_fsgsbase()) {
        // Use WRFSBASE instruction (faster)
        __asm__ volatile("wrfsbase %0" : : "r"(task->fs_base));
    } else {
        // Fall back to MSR write
        wrmsr(MSR_FS_BASE, task->fs_base);
    }
}

// ============================================================================
// LOAD AVERAGE AND SYSTEM STATISTICS
// ============================================================================

// Helper: compute exponentially weighted moving average
static unsigned long calc_load(unsigned long load, unsigned long exp, unsigned long active) {
    unsigned long newload;
    newload = load * exp + active * LOADAVG_FIXED_1 * (LOADAVG_FIXED_1 - exp);
    // Both exp and the factor are in <<16 space, so we shift back once
    return newload / LOADAVG_FIXED_1;
}

// Called periodically from timer IRQ (every 5 seconds = 500 ticks at 100Hz)
void sched_calc_load(void) {
    // Count runnable tasks (TASK_READY or TASK_RUNNING, excluding idle/pid0)
    //
    // MUST use spin_lock_irqsave: g_task_list_lock is taken from the timer IRQ
    // (sched_wake_expired_sleepers) and from process context elsewhere.  Holding
    // it with IRQs enabled lets the timer fire on this CPU and self-deadlock when
    // the handler tries to re-acquire it.  Although sched_calc_load currently
    // runs only from the timer (IRQs already off), use irqsave for consistency
    // and to stay correct if it is ever called from process context.
    int nr_active = 0;
    uint64_t tl_flags;
    spin_lock_irqsave(&g_task_list_lock, &tl_flags);
    task_t* t = g_task_list_head;
    while (t) {
        if (t->id != 0 && !t->has_exited &&
            (t->state == TASK_READY || t->state == TASK_RUNNING)) {
            // Skip idle tasks
            int is_idle = 0;
            if (g_smp_initialized) {
                // Check if this is any CPU's idle task (check by comm)
                if (t->comm[0] == 'i' && t->comm[1] == 'd' &&
                    t->comm[2] == 'l' && t->comm[3] == 'e')
                    is_idle = 1;
            }
            if (!is_idle)
                nr_active++;
        }
        t = t->next;
    }
    spin_unlock_irqrestore(&g_task_list_lock, tl_flags);

    uint64_t flags;
    spin_lock_irqsave(&g_loadavg_lock, &flags);
    g_loadavg[0] = calc_load(g_loadavg[0], LOADAVG_EXP_1, (unsigned long)nr_active);
    g_loadavg[1] = calc_load(g_loadavg[1], LOADAVG_EXP_5, (unsigned long)nr_active);
    g_loadavg[2] = calc_load(g_loadavg[2], LOADAVG_EXP_15, (unsigned long)nr_active);
    spin_unlock_irqrestore(&g_loadavg_lock, flags);
}

void sched_get_loadavg(unsigned long loads[3]) {
    uint64_t flags;
    spin_lock_irqsave(&g_loadavg_lock, &flags);
    loads[0] = g_loadavg[0];
    loads[1] = g_loadavg[1];
    loads[2] = g_loadavg[2];
    spin_unlock_irqrestore(&g_loadavg_lock, flags);
}

int sched_get_nr_running(void) {
    int count = 0;
    // MUST use spin_lock_irqsave: this runs in process context (e.g. via the
    // sysinfo path) with IRQs enabled.  g_task_list_lock is also taken from the
    // timer IRQ (sched_wake_expired_sleepers / sched_calc_load).  Holding it
    // with IRQs on lets the timer fire on this CPU and self-deadlock when the
    // handler spins to re-acquire it — a wedged CPU stops scheduling entirely.
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    task_t* t = g_task_list_head;
    while (t) {
        if (!t->has_exited && (t->state == TASK_READY || t->state == TASK_RUNNING))
            count++;
        t = t->next;
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);
    return count;
}

int sched_get_nr_procs(void) {
    int count = 0;
    // MUST use spin_lock_irqsave — same reason as sched_get_nr_running above.
    // Reachable from the sysinfo() syscall in process context with IRQs on,
    // while the timer IRQ also acquires g_task_list_lock.
    uint64_t flags;
    spin_lock_irqsave(&g_task_list_lock, &flags);
    task_t* t = g_task_list_head;
    while (t) {
        if (!t->has_exited)
            count++;
        t = t->next;
    }
    spin_unlock_irqrestore(&g_task_list_lock, flags);
    return count;
}

