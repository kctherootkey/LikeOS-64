// LikeOS-64 Preemptive Scheduler with Full Kernel Preemption and SMP Spinlocks
#ifndef _KERNEL_SCHED_H_
#define _KERNEL_SCHED_H_

#include "types.h"
#include "vfs.h"
#include "signal.h"

// Forward declaration
struct vfs_file;
struct tty;
struct task;

// Maximum file descriptors per task
#define TASK_MAX_FDS    1024

// Maximum memory regions per task (for mmap tracking)
#define TASK_MAX_MMAP   64

// ============================================================================
// CPU FEATURE FLAGS
// ============================================================================

// CPUID leaf 7 extended features (EBX)
#define CPU_FEATURE_FSGSBASE    (1 << 0)   // FSGSBASE instructions supported
#define CPU_FEATURE_SMEP        (1 << 7)   // Supervisor Mode Execution Prevention
#define CPU_FEATURE_SMAP        (1 << 20)  // Supervisor Mode Access Prevention

// Global CPU feature flags (detected at boot)
extern uint32_t g_cpu_features_ext;

// Preemption configuration
// Time slice in timer ticks (at 100Hz, 2 ticks = 20ms for better responsiveness)
#define SCHED_TIME_SLICE    2

// ============================================================================
// SMP-READY SPINLOCK IMPLEMENTATION
// ============================================================================

// UP (Uniprocessor) mode flag - when set, spinlocks only use interrupt disable
// This avoids deadlocks on single-CPU systems where spinning would be fatal
// Defined in smp.c, set to 1 if only one CPU is active
extern volatile uint32_t g_smp_up_mode;

typedef struct spinlock {
    volatile uint32_t locked;    // 0 = unlocked, 1 = locked
    volatile uint32_t owner_cpu; // For debugging: CPU that holds lock (0xFFFFFFFF = none)
    const char* name;            // Lock name for debugging
} spinlock_t;

// Static initializer for spinlock
#define SPINLOCK_INIT(n) { .locked = 0, .owner_cpu = 0xFFFFFFFF, .name = (n) }

// Initialize a spinlock at runtime
static inline void spinlock_init(spinlock_t* lock, const char* name) {
    lock->locked = 0;
    lock->owner_cpu = 0xFFFFFFFF;
    lock->name = name;
}

// Acquire spinlock (SMP-safe using atomic compare-and-swap)
// On UP systems, we don't spin - interrupts must be disabled by caller
static inline void spin_lock(spinlock_t* lock) {
    // In UP mode, if interrupts are disabled, no other context can run,
    // so we can "acquire" the lock without spinning
    if (g_smp_up_mode) {
        // Just mark as locked for debugging/assertions
        __asm__ volatile("" ::: "memory");
        lock->locked = 1;
        return;
    }
    // SMP mode: actual spinlock with atomic CAS
    while (1) {
        uint32_t expected = 0;
        uint32_t desired = 1;
        uint32_t old;
        __asm__ volatile (
            "lock cmpxchgl %2, %1"
            : "=a"(old), "+m"(lock->locked)
            : "r"(desired), "0"(expected)
            : "memory", "cc"
        );
        if (old == 0) {
            __asm__ volatile("" ::: "memory"); // Memory barrier
            return;
        }
        // Spin with PAUSE instruction (reduces power, improves SMP performance)
        __asm__ volatile("pause" ::: "memory");
    }
}

// Try to acquire spinlock, return 1 if acquired, 0 if failed
static inline int spin_trylock(spinlock_t* lock) {
    // In UP mode, always succeed if interrupts are disabled
    if (g_smp_up_mode) {
        __asm__ volatile("" ::: "memory");
        lock->locked = 1;
        return 1;
    }
    // SMP mode: actual atomic trylock
    uint32_t expected = 0;
    uint32_t desired = 1;
    uint32_t old;
    __asm__ volatile (
        "lock cmpxchgl %2, %1"
        : "=a"(old), "+m"(lock->locked)
        : "r"(desired), "0"(expected)
        : "memory", "cc"
    );
    if (old == 0) {
        __asm__ volatile("" ::: "memory");
        return 1;
    }
    return 0;
}

// Release spinlock
// On x86, stores are not reordered with stores (TSO model), so a compiler
// barrier is sufficient for release semantics.  mfence (~33-100 cycles)
// was needlessly serialising the pipeline on every unlock kernel-wide.
static inline void spin_unlock(spinlock_t* lock) {
    __asm__ volatile("" ::: "memory");  // compiler barrier (release semantics on x86)
    lock->locked = 0;
    lock->owner_cpu = 0xFFFFFFFF;
}

// Check if spinlock is held
static inline int spin_is_locked(spinlock_t* lock) {
    return lock->locked != 0;
}

// Save interrupt flags and disable interrupts
static inline uint64_t local_irq_save(void) {
    uint64_t flags;
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

// Restore interrupt flags
static inline void local_irq_restore(uint64_t flags) {
    __asm__ volatile (
        "pushq %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

// Spinlock with interrupt save/restore (for use in interrupt handlers)
static inline void spin_lock_irqsave(spinlock_t* lock, uint64_t* flags) {
    *flags = local_irq_save();
    spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    local_irq_restore(flags);
}

// ============================================================================
// PREEMPTION CONTROL (Full Kernel Preemption)
// ============================================================================

// Global preemption counter (per-CPU for SMP, single for UP)
// When > 0, preemption is disabled in the current context
extern volatile int g_preempt_count;

// Disable kernel preemption (increment counter)
static inline void preempt_disable(void) {
    __asm__ volatile("" ::: "memory");
    g_preempt_count++;
    __asm__ volatile("" ::: "memory");
}

// Enable kernel preemption (decrement counter)
static inline void preempt_enable(void) {
    __asm__ volatile("" ::: "memory");
    g_preempt_count--;
    __asm__ volatile("" ::: "memory");
}

// Get current preemption count
static inline int preempt_count_get(void) {
    return g_preempt_count;
}

// Check if preemption is currently enabled
static inline int preemption_enabled(void) {
    return g_preempt_count == 0;
}

// ============================================================================
// THREAD GROUP SUPPORT STRUCTURES
// ============================================================================

// mm_struct - Shared address space descriptor (for CLONE_VM)
// When threads share address space, they point to the same mm_struct.
// Reference counting ensures the address space is freed only when all
// threads exit.
typedef struct mm_struct {
    uint64_t* pml4;              // Page table base (CR3)
    volatile int refcount;        // Number of tasks sharing this mm
    spinlock_t lock;              // Protects mm operations
    
    // Heap management
    uint64_t brk_start;          // Initial program break (heap start)
    uint64_t brk;                // Current program break (heap end)
    uint64_t mmap_base;          // Base address for mmap allocations
    
    // Memory region tracking
    struct mmap_region* mmap_regions;  // Dynamic mmap region array
    int mmap_count;              // Number of active mmap regions
    int mmap_capacity;           // Allocated capacity
} mm_struct_t;

// files_struct - Shared file descriptor table (for CLONE_FILES)
typedef struct files_struct {
    volatile int refcount;        // Number of tasks sharing this fd table
    spinlock_t lock;              // Protects fd table operations
    struct vfs_file* fd_table[TASK_MAX_FDS];  // File descriptor array
} files_struct_t;

// sighand_struct - Shared signal handlers (for CLONE_SIGHAND)
typedef struct sighand_struct {
    volatile int refcount;        // Number of tasks sharing these handlers
    spinlock_t lock;              // Protects signal handler operations
    struct k_sigaction action[65]; // Signal handlers (NSIG = 65)
} sighand_struct_t;

// Forward declarations for robust futex support (full definitions in futex.h)
struct robust_list;
struct robust_list_head;

// ============================================================================
// TASK DEFINITIONS
// ============================================================================

typedef void (*task_entry_t)(void* arg);

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_STOPPED,
    TASK_ZOMBIE
} task_state_t;

// Task privilege level
typedef enum {
    TASK_KERNEL = 0,   // Ring 0
    TASK_USER = 3      // Ring 3
} task_privilege_t;

// Memory region for mmap tracking
typedef struct mmap_region {
    uint64_t start;     // Virtual start address
    uint64_t length;    // Length in bytes
    uint64_t prot;      // Protection flags (PROT_READ, PROT_WRITE, PROT_EXEC)
    uint64_t flags;     // MAP_ANONYMOUS, MAP_PRIVATE, etc.
    int fd;             // File descriptor (-1 for anonymous)
    uint64_t offset;    // Offset in file
    bool in_use;        // Whether this slot is used
} mmap_region_t;

// Saved interrupt frame for preemptive context switch
// Layout must match push order in irq_common_stub
typedef struct interrupt_frame {
    // Pushed by irq_common_stub (in reverse order of struct)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    // Pushed by IRQ macro
    uint64_t int_no, err_code;
    // Pushed by CPU on interrupt
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame_t;

typedef struct task {
    uint64_t* sp;          // Saved stack pointer (cooperative switch)
    uint64_t* pml4;        // Page table base (CR3) - NULL for kernel tasks (uses kernel PML4)
    task_entry_t entry;    // Entry function
    void* arg;             // Entry argument
    task_state_t state;
    task_privilege_t privilege;  // Ring level
    struct task* next;     // Global task list link (linear, all tasks)
    struct task* rq_next;   // Per-CPU run queue link (NULL when not in rq)
    uint32_t on_cpu;        // CPU this task is currently assigned to
    uint64_t cpu_affinity;  // Bitmask of allowed CPUs (0 = all CPUs allowed)
    bool on_rq;             // Whether currently in a per-CPU run queue
    int id;
    
    // Preemption support
    volatile int need_resched;       // Set by timer when time slice expired
    int remaining_ticks;             // Remaining time slice ticks
    interrupt_frame_t* preempt_frame; // Saved interrupt frame (NULL if cooperative switch)
    
    // Process hierarchy
    struct task* parent;        // Parent task (NULL for init)
    struct task* first_child;   // First child in linked list
    struct task* next_sibling;  // Next sibling in parent's child list
    
    // Exit status tracking
    int exit_code;              // Exit status for waitpid
    bool has_exited;            // True when exit() was called
    volatile int exit_lock;     // Atomic guard for sched_mark_task_exited (0=unlocked)
    bool is_fork_child;         // True if this is a newly forked child (should return 0)
    
    // User mode support
    uint64_t user_stack_top;    // User stack virtual address (for user tasks)
    uint64_t kernel_stack_top;  // Kernel stack for syscalls/interrupts (for user tasks)
    void* kernel_stack_base;    // Kernel stack allocation base (for freeing)

    // Job control / session
    int pgid;
    int sid;
    struct tty* ctty;

    // Wait linkage for blocking I/O
    struct task* wait_next;
    void* wait_channel;
    
    // Timer-based sleep support
    uint64_t wakeup_tick;           // Tick count when task should wake (0 = not sleeping)
    
    // Signal handling state
    task_signal_state_t signals;    // Full signal state
    
    // Saved syscall context for signal delivery (per-task, not global)
    uint64_t syscall_rsp;           // User RSP on syscall entry
    uint64_t syscall_rip;           // User RIP (return address)
    uint64_t syscall_rflags;        // User RFLAGS
    uint64_t syscall_rax;           // Syscall return value (for sigreturn)
    uint64_t syscall_rbp;           // Callee-saved
    uint64_t syscall_rbx;           // Callee-saved
    uint64_t syscall_r12;           // Callee-saved
    uint64_t syscall_r13;           // Callee-saved
    uint64_t syscall_r14;           // Callee-saved
    uint64_t syscall_r15;           // Callee-saved
    uint64_t syscall_kernel_rsp;    // Kernel RSP for syscall return (set before call)
    
    // Process name (set from argv[0] basename on execve)
    char comm[256];
    
    // Full command line (argv joined by spaces, set on execve)
    char cmdline[1024];
    
    // Environment string (envp joined by spaces, set on execve)
    char environ[2048];
    
    // Timing / accounting
    uint64_t start_tick;        // Tick count when task was created
    uint64_t utime_ticks;       // Ticks spent in user mode
    uint64_t stime_ticks;       // Ticks spent in kernel mode
    
    // Current working directory
    char cwd[256];
    
    // Implicit console I/O flags (O_NONBLOCK etc., for when fd 0/1/2 are
    // not backed by a real vfs_file but use the console TTY directly).
    uint32_t console_flags;

    // File descriptor table (legacy - used when files == NULL)
    struct vfs_file* fd_table[TASK_MAX_FDS];
    
    // Memory management (legacy - used when mm == NULL)
    uint64_t brk;               // Current program break (heap end)
    uint64_t brk_start;         // Initial program break (heap start)
    mmap_region_t mmap_regions[TASK_MAX_MMAP];  // mmap'd regions
    uint64_t mmap_base;         // Base address for mmap allocations
    
    // ========================================================================
    // THREAD GROUP SUPPORT (POSIX threads / clone)
    // ========================================================================
    
    // Thread group identification
    int tgid;                       // Thread group ID (= id for group leader, = leader's id for threads)
    struct task* group_leader;      // Pointer to thread group leader
    struct task* thread_group_next; // Next thread in same thread group (circular list)
    struct task* thread_group_prev; // Previous thread in thread group (for O(1) removal)
    volatile int nr_threads;        // Thread count in group (only valid in leader)
    volatile int group_exit_code;   // Exit code for exit_group()
    volatile bool group_exiting;    // Set when exit_group() is called
    
    // Exit behavior
    int exit_signal;                // Signal to send parent on exit (SIGCHLD for processes, 0 for threads)
    
    // CLONE_CHILD_CLEARTID / set_tid_address support
    uint64_t* clear_child_tid;      // Address to write 0 and futex-wake on exit
    uint64_t* set_child_tid;        // Address to write TID on creation
    
    // TLS (Thread Local Storage) support
    uint64_t fs_base;               // FS segment base for user TLS
    uint64_t gs_base;               // GS segment base (usually not used by user)
    
    // Robust futex support
    struct robust_list_head* robust_list;  // Robust futex list head
    size_t robust_list_len;                // Size of robust list head structure
    
    // Shared structures (NULL = use legacy per-task fields)
    mm_struct_t* mm;                // Shared address space (CLONE_VM)
    files_struct_t* files;          // Shared file descriptors (CLONE_FILES)
    sighand_struct_t* sighand;      // Shared signal handlers (CLONE_SIGHAND)
} task_t;

void sched_init(void);
task_t* sched_add_task(task_entry_t entry, void* arg, void* stack_mem, size_t stack_size);
task_t* sched_add_user_task(task_entry_t entry, void* arg, uint64_t* pml4, uint64_t user_stack, uint64_t kernel_stack);
void sched_tick(void);
void sched_schedule(void);    // Core preemptive scheduler - switch to next ready task
void sched_yield_in_kernel(void); // In-kernel cooperative yield (no syscall)
void sched_run_ready(void);
task_t* sched_current(void);
int sched_has_user_tasks(void);  // Check if any user tasks are running

// Preemptive scheduling API
void sched_preempt(interrupt_frame_t* frame);  // Called from timer IRQ, performs context switch
int sched_need_resched(void);                  // Check if reschedule is needed
void sched_set_need_resched(task_t* t);        // Mark task as needing reschedule
void sched_wake_expired_sleepers(uint64_t current_tick);  // Wake tasks whose sleep timer expired
void sched_wake_channel(void* channel);        // Wake all tasks waiting on a channel

// Global task list lock (protects the all-tasks linked list)
extern spinlock_t g_task_list_lock;

// SMP support
void sched_enable_smp(void);   // Called after per-CPU init to enable per-CPU current task
int sched_is_smp(void);        // Check if SMP mode is enabled

// Per-CPU scheduling API
void sched_init_ap(uint32_t cpu_id);     // Initialize per-CPU scheduler for AP
void sched_enqueue_ready(task_t* task);  // Enqueue task to its assigned CPU's run queue
void sched_load_balance(void);           // Pull tasks from busiest CPU (called from timer)

// Process management
task_t* sched_fork_current(void);           // Fork current task with COW
void sched_remove_task(task_t* task);       // Remove task from scheduler
task_t* sched_find_task_by_id(uint32_t pid); // Find task by PID
task_t* sched_find_task_by_id_locked(uint32_t pid); // Find task by PID (caller holds g_task_list_lock)
void sched_add_child(task_t* parent, task_t* child);  // Add child to parent
void sched_remove_child(task_t* parent, task_t* child);  // Remove child from parent
void sched_reparent_children(task_t* task); // Reparent children to init (task 1)
uint32_t sched_get_ppid(task_t* task);      // Get parent PID
void sched_reap_zombies(task_t* parent);    // Reap all zombie children of parent
void sched_mark_task_exited(task_t* task, int status);
void sched_signal_task(task_t* task, int sig);
void sched_signal_pgrp(int pgid, int sig);
int sched_pgid_exists(int pgid);
void sched_dump_tasks(void);  // Debug: dump all task states

// ============================================================================
// THREAD GROUP MANAGEMENT
// ============================================================================

// Thread group operations
void thread_group_init(task_t* leader);      // Initialize task as thread group leader
void thread_group_add(task_t* leader, task_t* thread);  // Add thread to group
void thread_group_remove(task_t* thread);    // Remove thread from group
int thread_group_count(task_t* task);        // Get number of threads in group
void thread_group_signal_all(task_t* task, int sig);  // Signal all threads in group

// Iterate over all threads in a thread group
#define for_each_thread(leader, t) \
    for ((t) = (leader); (t); (t) = ((t)->thread_group_next == (leader) ? NULL : (t)->thread_group_next))

// ============================================================================
// SHARED STRUCTURE MANAGEMENT
// ============================================================================

// mm_struct operations
mm_struct_t* mm_struct_create(uint64_t* pml4);  // Create new mm_struct
mm_struct_t* mm_struct_clone(mm_struct_t* src); // Clone mm_struct (for fork)
void mm_struct_get(mm_struct_t* mm);             // Increment refcount
void mm_struct_put(mm_struct_t* mm);             // Decrement refcount, free if 0

// files_struct operations
files_struct_t* files_struct_create(void);       // Create new files_struct
files_struct_t* files_struct_clone(files_struct_t* src);  // Clone (for fork)
void files_struct_get(files_struct_t* files);    // Increment refcount
void files_struct_put(files_struct_t* files);    // Decrement refcount, free if 0

// sighand_struct operations
sighand_struct_t* sighand_struct_create(void);   // Create new sighand_struct
sighand_struct_t* sighand_struct_clone(sighand_struct_t* src);  // Clone (for fork)
void sighand_struct_get(sighand_struct_t* sighand);  // Increment refcount
void sighand_struct_put(sighand_struct_t* sighand);  // Decrement refcount, free if 0

// ============================================================================
// TLS (THREAD LOCAL STORAGE) SUPPORT
// ============================================================================

// Set FS base for a task (used by CLONE_SETTLS and arch_prctl)
void task_set_fs_base(task_t* task, uint64_t base);
uint64_t task_get_fs_base(task_t* task);

// Apply FS base on context switch (called by scheduler)
void task_load_tls(task_t* task);

// Check if FSGSBASE instructions are supported
bool cpu_has_fsgsbase(void);

// ============================================================================
// INTERNAL FUNCTIONS (exported for syscall.c)
// ============================================================================

// Global task ID allocator
extern int g_next_id;

// Add task to global task list (must hold g_task_list_lock)
void task_list_add(task_t* t);

// ============================================================================
// LOAD AVERAGE AND SYSTEM STATISTICS
// ============================================================================
void sched_calc_load(void);            // Update load averages (call from timer)
void sched_get_loadavg(unsigned long loads[3]); // Get 1/5/15 min load averages (<<16 fixed-point)
int sched_get_nr_running(void);        // Count of runnable tasks
int sched_get_nr_procs(void);          // Total process count

#endif // _KERNEL_SCHED_H_
