// LikeOS-64 Preemptive Scheduler with Full Kernel Preemption and SMP Spinlocks
#ifndef _KERNEL_SCHED_H_
#define _KERNEL_SCHED_H_

#include <kernel/ke/fpu.h>
#include <kernel/uapi/types.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/cred.h>

// Forward declaration
struct vfs_file;
struct tty;
struct task;

/* Maximum file descriptors per task.
 *
 * Userspace must be told the same number: OPEN_MAX in user/lib/libc/limits.h,
 * which FOPEN_MAX, NOFILE, sysconf(_SC_OPEN_MAX), getdtablesize() and
 * getrlimit(RLIMIT_NOFILE) all derive from.  They disagreed for a long time --
 * libc said 256 while this said 1024 -- so anything that sized a table by what
 * libc reported got a quarter of the descriptors it could actually have.
 *
 * Not free to raise: task_t embeds fd_table[] and fd_flags[] (9 KB at 1024, of
 * a 56 KB task_t) and fork() copies all of it, and files_struct_t embeds the
 * same pair again.  1024 is what a conventional system offers by default. */
#define TASK_MAX_FDS 1024

// Maximum memory regions per task (for mmap tracking)
/* Address-space regions per process.  The dynamic linker maps roughly one
 * region per PT_LOAD (~4 per shared object) with no reservation mapping, so a
 * program with a large DSO graph — an X server plus its client libraries and
 * dlopen'd driver modules — needs well over a hundred.  Raising this grows
 * task_t; see the note in sched_fork_current() about not copying the table
 * onto the kernel stack. */
/*
 * Per-task mmap region slots.
 *
 * One slot per PT_LOAD segment, not per library, and a shared object has four
 * of them (rodata, text, relro, data).  Claws Mail pulls in 58 libraries =
 * 230 segments before it has mapped a heap, a thread stack or a single
 * dlopen'd module; at 256 it ran out partway through dlopening the spell
 * checker, and the loader -- which cannot report "out of address space", only
 * "cannot find" -- said the library was missing.
 *
 * 512 is twice the measured peak.  It is not free: task_t embeds the array, so
 * this is 40KB per task and fork() memcpy's all of it, which is why it is a
 * measured number rather than a generous one.  Exhaustion is at least loud now
 * (the WARN in sys_mmap names the pid and the limit).
 */
/* Ceiling on a process's mapped regions, NOT a preallocation.
 *
 * The table used to be an array of this size inlined into every task_t, which
 * put a hard floor under the limit: at 80 bytes a record, 65530 entries would
 * be 5 MB in each task_t, and a task_t is allocated per THREAD.  It is now
 * grown on demand from MMAP_REGIONS_INITIAL, so an ordinary process pays for
 * the few dozen regions it has and only a process that really maps this much
 * pays for the table.  That is the arrangement every other system uses: the
 * number is a limit to refuse past, not memory to reserve.
 *
 * Kept under 0xFFFF because the merge pass indexes slots with uint16_t. */
#define TASK_MAX_MMAP 65530
/* What a task starts with.  Big enough that a normal program never grows the
 * table (a shell, an editor and an X client all sit well under this), small
 * enough that a task_t costs a few kilobytes rather than megabytes. */
#define MMAP_REGIONS_INITIAL 64

// ============================================================================
// CPU FEATURE FLAGS
// ============================================================================

// CPUID leaf 7 extended features (EBX)
#define CPU_FEATURE_FSGSBASE (1 << 0) // FSGSBASE instructions supported
#define CPU_FEATURE_SMEP (1 << 7) // Supervisor Mode Execution Prevention
#define CPU_FEATURE_SMAP (1 << 20) // Supervisor Mode Access Prevention

// Global CPU feature flags (detected at boot)
extern uint32_t g_cpu_features_ext;

// Preemption configuration
// Time slice in timer ticks (at 100Hz, 2 ticks = 20ms for better responsiveness)
#define SCHED_TIME_SLICE 2

// ============================================================================
// SMP-READY SPINLOCK IMPLEMENTATION
// ============================================================================

// UP (Uniprocessor) mode flag - when set, spinlocks only use interrupt disable
// This avoids deadlocks on single-CPU systems where spinning would be fatal
// Defined in smp.c, set to 1 if only one CPU is active
extern volatile uint32_t g_smp_up_mode;

typedef struct spinlock {
	volatile uint32_t locked; // 0 = unlocked, 1 = locked
	volatile uint32_t
		owner_cpu; // For debugging: CPU that holds lock (0xFFFFFFFF = none)
	const char *name; // Lock name for debugging
} spinlock_t;

// Static initializer for spinlock
#define SPINLOCK_INIT(n)                                          \
	{                                                         \
		.locked = 0, .owner_cpu = 0xFFFFFFFF, .name = (n) \
	}

// Initialize a spinlock at runtime
static inline void spinlock_init(spinlock_t *lock, const char *name)
{
	lock->locked = 0;
	lock->owner_cpu = 0xFFFFFFFF;
	lock->name = name;
}

/* How long spin_lock() waits before it says so.
 *
 * A spinlock here spins forever, with interrupts disabled, and prints
 * nothing.  So a lock that is never released presents as a machine that
 * stopped -- no message, no oops, nothing to tell it apart from a wedged
 * device, a stuck display or a task that simply went to sleep and was never
 * woken.  Every one of those is a different bug, and the console cannot
 * distinguish them.
 *
 * The threshold has to sit above the longest LEGITIMATE hold in this kernel,
 * which is the SVGA FIFO: svga_fifo_wait_progress() waits for the host to
 * drain the command FIFO with svga_fifo_lock held and gives up after two
 * seconds (SVGA_BUSY_TIMEOUT_US).  A `pause' is tens of cycles, so 2^29 of
 * them is several seconds on any processor this runs on -- comfortably past
 * that, and far past every other hold, which are all bounded by a handful of
 * instructions.
 *
 * Reporting does not break the lock or change what happens next: the waiter
 * goes back to spinning.  This only turns a silent freeze into a named one.
 */
#define SPIN_STALL_PAUSES (1u << 29)

/* Defined in ke/stack_guard.c, beside the other diagnostics that have to run
 * from a wedged context: prints the lock's name and the caller chain that
 * reached it.  Declared here because spin_lock() below is what calls it. */
void spin_report_stall(spinlock_t *lock);

/* ============================================================================
 * mm_rwsem_t — the address-space read/write semaphore
 *
 * A SLEEPING lock, so it may only be taken from process context.  It lives
 * here rather than in the mm headers because it is embedded in task_t and
 * spinlock_t is defined just above; the operations are declared in
 * <kernel/mm/rwsem.h> and implemented in kernel/mm/rwsem.c.
 *
 * Held for READING by anything that only walks the address space — the page
 * fault handlers and the pre-fault shield.  Held for WRITING by anything that
 * changes its shape: mmap, munmap, mprotect, brk, fork's clone of the parent,
 * exec's teardown and address-space destruction.  Until this existed, nothing
 * at all serialised those two groups: threads share one page table, so a fault
 * and an unmap of the same address ran concurrently by default.
 * ========================================================================== */
typedef struct mm_rwsem {
	volatile int readers; /* active shared holders                     */
	volatile int writer; /* 1 while exclusive is held                 */
	volatile uint64_t owner; /* exclusive owner's task id                 */
	volatile int wdepth; /* exclusive recursion depth                 */
	volatile int w_wait; /* writers queued (fairness hint)            */
	spinlock_t lock; /* protects the fields above                 */
} mm_rwsem_t;

#define MM_RWSEM_INIT(n)                                              \
	{                                                             \
		.readers = 0, .writer = 0, .owner = (uint64_t)-1,     \
		.wdepth = 0, .w_wait = 0, .lock = SPINLOCK_INIT(n)    \
	}

// Acquire spinlock (SMP-safe using atomic compare-and-swap)
// On UP systems, we don't spin - interrupts must be disabled by caller
static inline void spin_lock(spinlock_t *lock)
{
	// In UP mode, if interrupts are disabled, no other context can run,
	// so we can "acquire" the lock without spinning
	if (g_smp_up_mode) {
		// Just mark as locked for debugging/assertions
		__asm__ volatile("" ::: "memory");
		lock->locked = 1;
		return;
	}
	// SMP mode: actual spinlock with atomic CAS
	uint32_t spins = 0;
	while (1) {
		uint32_t expected = 0;
		uint32_t desired = 1;
		uint32_t old;
		__asm__ volatile("lock cmpxchgl %2, %1"
				 : "=a"(old), "+m"(lock->locked)
				 : "r"(desired), "0"(expected)
				 : "memory", "cc");
		if (old == 0) {
			__asm__ volatile("" ::: "memory"); // Memory barrier
			return;
		}
		// Spin with PAUSE instruction (reduces power, improves SMP performance)
		__asm__ volatile("pause" ::: "memory");
		/* Long past any hold this kernel takes: say which lock, and
		 * keep waiting.  See SPIN_STALL_PAUSES. */
		if (__builtin_expect(++spins == SPIN_STALL_PAUSES, 0)) {
			spins = 0;
			spin_report_stall(lock);
		}
	}
}

// Try to acquire spinlock, return 1 if acquired, 0 if failed
static inline int spin_trylock(spinlock_t *lock)
{
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
	__asm__ volatile("lock cmpxchgl %2, %1"
			 : "=a"(old), "+m"(lock->locked)
			 : "r"(desired), "0"(expected)
			 : "memory", "cc");
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
static inline void spin_unlock(spinlock_t *lock)
{
	__asm__ volatile(
		"" ::
			: "memory"); // compiler barrier (release semantics on x86)
	lock->locked = 0;
	lock->owner_cpu = 0xFFFFFFFF;
}

// Check if spinlock is held
static inline int spin_is_locked(spinlock_t *lock)
{
	return lock->locked != 0;
}

// Save interrupt flags and disable interrupts
static inline uint64_t local_irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\t"
			 "popq %0\n\t"
			 "cli"
			 : "=r"(flags)
			 :
			 : "memory");
	return flags;
}

// Are interrupts on right now?  What a sleeping wait has to ask before it
// yields: a context switch with interrupts disabled resumes some other task
// with them still off, and the CPU this one was on can no longer be
// preempted -- so code that CAN yield must spin instead when the answer is
// no.  (RFLAGS.IF, bit 9.)
static inline int irqs_enabled(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\t"
			 "popq %0"
			 : "=r"(flags)::"memory");
	return (flags & (1ULL << 9)) != 0;
}

// Restore interrupt flags
static inline void local_irq_restore(uint64_t flags)
{
	__asm__ volatile("pushq %0\n\t"
			 "popfq"
			 :
			 : "r"(flags)
			 : "memory", "cc");
}

// Spinlock with interrupt save/restore (for use in interrupt handlers)
static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
{
	*flags = local_irq_save();
	spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
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
static inline void preempt_disable(void)
{
	__asm__ volatile("" ::: "memory");
	g_preempt_count++;
	__asm__ volatile("" ::: "memory");
}

// Enable kernel preemption (decrement counter)
static inline void preempt_enable(void)
{
	__asm__ volatile("" ::: "memory");
	g_preempt_count--;
	__asm__ volatile("" ::: "memory");
}

// Get current preemption count
static inline int preempt_count_get(void)
{
	return g_preempt_count;
}

// Check if preemption is currently enabled
static inline int preemption_enabled(void)
{
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
	uint64_t *pml4; // Page table base (CR3)
	volatile int refcount; // Number of tasks sharing this mm
	spinlock_t lock; // Protects mm operations

	// Heap management
	uint64_t brk_start; // Initial program break (heap start)
	uint64_t brk; // Current program break (heap end)
	uint64_t mmap_base; // Base address for mmap allocations

	// Memory region tracking
	struct mmap_region *mmap_regions; // Dynamic mmap region array
	int mmap_count; // Number of active mmap regions
	int mmap_capacity; // Allocated capacity
} mm_struct_t;

// files_struct - Shared file descriptor table (for CLONE_FILES)
typedef struct files_struct {
	volatile int refcount; // Number of tasks sharing this fd table
	spinlock_t lock; // Protects fd table operations
	struct vfs_file *fd_table[TASK_MAX_FDS]; // File descriptor array
	uint8_t fd_flags[TASK_MAX_FDS]; // Per-fd flags (FD_CLOEXEC = 1)
} files_struct_t;

// sighand_struct - Shared signal handlers (for CLONE_SIGHAND)
typedef struct sighand_struct {
	volatile int refcount; // Number of tasks sharing these handlers
	spinlock_t lock; // Protects signal handler operations
	struct k_sigaction action[65]; // Signal handlers (NSIG = 65)
} sighand_struct_t;

// Forward declarations for robust futex support (full definitions in futex.h)
struct robust_list;
struct robust_list_head;

// ============================================================================
// TASK DEFINITIONS
// ============================================================================

typedef void (*task_entry_t)(void *arg);

typedef enum {
	TASK_READY = 0,
	TASK_RUNNING,
	TASK_BLOCKED,
	TASK_STOPPED,
	TASK_ZOMBIE
} task_state_t;

// Task privilege level
typedef enum {
	TASK_KERNEL = 0, // Ring 0
	TASK_USER = 3 // Ring 3
} task_privilege_t;

// Memory region for mmap tracking
struct vfs_file; /* forward — region may pin a file for demand paging */
struct mm_dirty_ops; /* forward — driver-registered write tracking */
typedef struct mmap_region {
	uint64_t start; // Virtual start address
	uint64_t length; // Length in bytes
	uint64_t prot; // Protection flags (PROT_READ, PROT_WRITE, PROT_EXEC)
	uint64_t flags; // MAP_ANONYMOUS, MAP_PRIVATE, etc.
	int fd; // File descriptor (-1 for anonymous)
	uint64_t offset; // Offset in file
	bool in_use; // Whether this slot is used
	/* Demand paging: lazy regions have NO pages mapped up front; the
	 * page-fault handler materialises them on first touch (zero-fill for
	 * anonymous, page-in via `file` for file-backed).  `file` holds a
	 * vfs reference so the mapping survives close(fd); fork adds a ref
	 * per child, munmap/exec/exit release it. */
	bool lazy;
	struct vfs_file *file; // backing file (NULL for anonymous)
	/* Device mapping (/dev/fb0): pages are device BAR memory mapped
	 * eagerly with PAGE_DEVICE PTEs — never freed to the physical
	 * allocator, shared (not copied) across fork. */
	bool device;
	uint64_t device_phys; // physical base backing region start
	/* Driver object whose pages this device mapping shows (registered
	 * device nodes and anonymous device files); pinned per record via
	 * get/put, released when the record goes away.  NULL for the older
	 * device mappings (/dev/fb0, /dev/shm), which pin through `file'. */
	void *dev_obj;
	void (*dev_get)(void *obj);
	void (*dev_put)(void *obj);
	/* Dirty tracking for the mapping, when the driver asked for it: the
	 * device holds a second copy of these pages and has to be told what
	 * the processor wrote.  Carried on EVERY record describing the
	 * mapping -- splits and forked copies inherit it with the rest of
	 * the record -- so the write-fault and unmap paths can reach the
	 * tracker from whichever record they meet.  NULL for mappings
	 * nothing watches.  See mm_dirty_ops in mm/memory.h. */
	const struct mm_dirty_ops *dev_dirty;
} mmap_region_t;

/* The user's registers as syscall_entry leaves them on the kernel stack.
 *
 * Layout must match the push order in syscall_entry (kernel/ke/syscall.asm);
 * the frame pointer it publishes through percpu_t::syscall_user_frame points
 * at the first field.  Note what the hardware has already destroyed by the
 * time this exists: SYSCALL puts the user's RIP in RCX and RFLAGS in R11, so
 * the user's own RCX and R11 are gone and those two slots carry the RIP and
 * RFLAGS instead. */
typedef struct syscall_user_frame {
	uint64_t r15, r14, r13, r12, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rbx, rax;
	uint64_t rip; /* pushed from rcx  */
	uint64_t rflags; /* pushed from r11  */
	uint64_t rsp; /* user stack pointer */
} syscall_user_frame_t;

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

/* Liveness stamp for task_t.  Set when a task is created, replaced with
 * TASK_MAGIC_DEAD immediately before the task_t is freed.  The enqueue path
 * checks it: a stale pointer enqueueing a task whose memory has been freed
 * (and possibly reused) is the recurring crash class behind "GPF on
 * feedface poison in the context switch" -- the check turns that from a
 * wild kernel fault hours later into a named WARN at the moment of the
 * offending enqueue, and refuses the enqueue so the fault never happens. */
#define TASK_MAGIC_ALIVE 0x4b534154u /* 'TASK' */
#define TASK_MAGIC_DEAD 0x44414544u /* 'DEAD' */

typedef struct task {
	uint32_t alive_magic; // TASK_MAGIC_ALIVE until the task_t is freed
	uint64_t *sp; // Saved stack pointer (cooperative switch)
	uint64_t *
		pml4; // Page table base (CR3) - NULL for kernel tasks (uses kernel PML4)
	task_entry_t entry; // Entry function
	void *arg; // Entry argument
	task_state_t state;
	task_privilege_t privilege; // Ring level
	struct task *next; // Global task list link (linear, all tasks)
	struct task *rq_next; // Per-CPU run queue link (NULL when not in rq)
	uint32_t on_cpu; // CPU this task is currently assigned to
	uint64_t cpu_affinity; // Bitmask of allowed CPUs (0 = all CPUs allowed)
	bool on_rq; // Whether currently in a per-CPU run queue
	/* Consecutive times a pick refused this task for sp == 0 and put it
	 * back on the queue.  Reset the moment it is actually picked, so a
	 * large value means "committed to a CPU that never gave it back". */
	uint32_t sp0_refusals;
	int id;

	// Preemption support
	volatile int need_resched; // Set by timer when time slice expired
	int remaining_ticks; // Remaining time slice ticks
	interrupt_frame_t *
		preempt_frame; // Saved interrupt frame (NULL if cooperative switch)

	// Process hierarchy
	struct task *parent; // Parent task (NULL for init)
	struct task *first_child; // First child in linked list
	struct task *next_sibling; // Next sibling in parent's child list

	// Exit status tracking
	int exit_code; // Exit status for waitpid
	/* The signal that terminated this task, or 0 if it exited normally via
	 * exit().  This is what distinguishes "killed by signal N" from
	 * "exit(128+N)" — both otherwise land in exit_code, so a program that
	 * exit()s with a status >= 128 (ssh uses 255) was misreported by
	 * waitpid as signalled/stopped.  Only waitpid consumes it. */
	int term_sig;
	bool has_exited; // True when exit() was called
	/* Job control: which stop signal stopped this task (0 = none) and
	 * whether a continue is unreported.  Set on the stop/continue paths,
	 * cleared when the parent collects the event via
	 * waitpid(WUNTRACED/WCONTINUED). */
	volatile int jc_stop_signo;
	volatile int jc_continued;

	/* ---- Tracing (ptrace) --------------------------------------------
	 *
	 * The tracer is identified by PID AND INCARNATION, never by pointer.
	 * There is no refcount on task_t and sched_find_task_by_id() hands back
	 * a bare pointer with the lock already dropped, so a stored tracer
	 * pointer would dangle the moment that task was reaped -- and this
	 * particular pointer is followed from the signal paths, where a fault
	 * is not survivable.  Every use re-resolves under g_task_list_lock and
	 * checks the incarnation, which is what makes a recycled slot
	 * detectable rather than merely unlikely.
	 *
	 * tracer_pid == 0 means untraced.  A tracer is NOT necessarily the
	 * parent: a debugger attaches to processes it did not create, which is
	 * exactly why this cannot reuse ->parent. */
	volatile int tracer_pid;
	volatile uint64_t tracer_incarnation;
	volatile uint32_t ptrace_options; /* PTRACE_O_* */

	/* Set by PTRACE_SYSCALL: stop at the next syscall boundary, both on the
	 * way in and on the way out.  Distinct from ptrace_options because it is
	 * a property of THIS resume rather than of the whole trace -- a tracer
	 * alternates PTRACE_SYSCALL and PTRACE_CONT freely, and a sticky option
	 * would keep stopping after it asked to stop stopping. */
	volatile int ptrace_syscall_trace;
	/* Guards against reporting the same syscall twice: set between the
	 * entry stop and the exit stop of one call. */
	volatile int ptrace_in_syscall;

	/* Set while the task is parked in a trace stop, as opposed to a
	 * job-control stop.  Both park the task in TASK_STOPPED -- reusing that
	 * proven machinery rather than adding a parallel scheduler state -- so
	 * this is what tells waitpid() which kind of event it is looking at and
	 * stops a tracer and a shell from consuming each other's stops. */
	volatile int ptrace_stopped;
	volatile int ptrace_stop_signo; /* signal that caused the stop */
	volatile int ptrace_event; /* PTRACE_EVENT_*, or 0 for a plain signal stop */
	volatile unsigned long ptrace_msg; /* value for PTRACE_GETEVENTMSG */

	/* Bumped on the TRACER every time one of its tracees records a trace
	 * stop, immediately before the wake.
	 *
	 * It exists so waitpid can close the sleep/wake race without holding
	 * two locks.  The events themselves live on the tracees and finding
	 * them means walking the task list, but the decision to sleep is taken
	 * under g_wait_lock -- and taking the task-list lock inside that would
	 * invent a lock order nothing else uses.  Sampling this counter before
	 * the scan and re-reading it under g_wait_lock answers the only
	 * question that matters, "did anything happen since I looked", with a
	 * single field read. */
	volatile uint64_t ptrace_notify_seq;

	/* Signal-delivery-stop.
	 *
	 * A traced task stops just BEFORE a signal is acted on, and the tracer
	 * decides what happens next: let it through, replace it, or discard it.
	 * That is the whole reason a debugger sees a fault at all -- without it
	 * a SIGSEGV runs its default action and the process is gone before the
	 * debugger is told anything, which is the behaviour this replaced.
	 *
	 * `ptrace_sig_delivery' distinguishes this stop from a trap stop,
	 * because PTRACE_CONT's signal argument means different things in the
	 * two cases: in a signal-delivery-stop it REPLACES the pending signal
	 * (and zero suppresses it), while in any other stop it is a new signal
	 * to send.  Without the distinction, resuming from a delivery stop with
	 * the same signal would queue it afresh, stop for it again, and never
	 * make progress.
	 *
	 * `ptrace_signal_injected' marks a signal the tracer asked for itself,
	 * so the delivery stop lets that one through instead of stopping for it
	 * again.  Without it a tracer passing a signal on resume would stop for
	 * the signal it just asked to deliver, forever. */
	volatile int ptrace_sig_delivery;
	volatile int ptrace_signal_injected;

	/* "Park yourself at the next safe point, quietly."
	 *
	 * When one thread of a traced process stops, they all stop -- that is
	 * what a debugger means by the process stopping, and without it
	 * `continue' restarts one thread while the rest are already running,
	 * and a backtrace of a sibling reads a stack that is moving under it.
	 *
	 * Only the thread that hit the event reports it; the others park with
	 * no event of their own, so waitpid reports the stop once rather than
	 * once per thread.  Set by task_ptrace_stop on the siblings, acted on
	 * by task_ptrace_group_park at the next return to user, cleared for
	 * the whole group by a resume. */
	volatile int ptrace_group_stop;

	/* Set once this task has reported PTRACE_EVENT_EXIT and been released
	 * from that stop: it is on its way out and must not stop again.
	 *
	 * The exit event is a promise.  A debugger told that a thread has gone
	 * drops it from its list -- that is the whole point of the event -- and
	 * from then on it has no thread to attribute anything to.  Report one
	 * more stop for that task and the debugger is handed a stop belonging
	 * to a thread it has already buried; gdb asserts on it outright
	 * (`thread->state != THREAD_EXITED' in get_thread_regcache) and the
	 * session is over.
	 *
	 * It also must not be PARKED by a sibling's group stop on its way to
	 * dying, or it would sit in the exit path holding a thread slot the
	 * group is waiting to lose. */
	volatile int ptrace_exiting;

	/* The siginfo of the signal this task stopped for, kept for
	 * PTRACE_GETSIGINFO.  A debugger asks at every signal stop: si_code and
	 * si_addr are what turn "stopped with SIGSEGV" into "wrote to 0x1",
	 * and the fault handler is the only place that knows them. */
	siginfo_t ptrace_siginfo;

	/* This task's own incarnation, stamped at creation from a counter that
	 * only ever increases.  PIDs are handed out monotonically today and so
	 * are unique in practice, but "in practice" is doing load-bearing work
	 * in a lifetime check; this makes the check independent of that. */
	uint64_t incarnation;

	/* Where this image's auxiliary vector sits on the user stack, set by
	 * elf_setup_stack on every exec.  A debugger reads it through
	 * PTRACE_GETAUXV: AT_ENTRY compared against e_entry in the file is how
	 * it works out the bias a position-independent executable was loaded
	 * at, without which every symbol address it reports is an offset from
	 * nowhere.  Zero on a task that has never exec'd. */
	uint64_t auxv_addr;
	uint64_t auxv_len;

	volatile int
		exit_lock; // Atomic guard for sched_mark_task_exited (0=unlocked)
	bool is_fork_child; // True if this is a newly forked child (should return 0)
	/* Set by sched_mark_task_exited when invoked from IRQ context (e.g.
     * signal_deliver_irq dispatching SIG_DFL_TERM from the timer ISR).
     * The fd-closing loop in that function can call fat32_close →
     * pagecache_flush_file → fat32_io_lock, which is a SLEEPING mutex —
     * calling it with IRQs disabled deadlocks the system (observed as
     * "WARNING: might_sleep() called with IRQs disabled at pagecache.c"
     * immediately followed by an unresponsive kernel when the user
     * Ctrl+C's curl during a 100 MB download).
     *
     * When this flag is set, sched_mark_task_exited skipped the fd
     * loop; dead_thread_reap (which runs in process context after the
     * next context switch) calls task_close_open_files() to finish the
     * job before sched_remove_task tears the task down. */
	bool fds_pending_close;

	/* Guard against double-queueing into g_dead_threads.  Set by
     * dead_thread_queue under g_dead_thread_lock; cleared by
     * dead_thread_reap before it calls sched_remove_task.  Without it,
     * a thread (exit_signal==0) queued via the deferred_zombie path can
     * be re-queued by sched_reparent_children when its parent is torn
     * down before dead_thread_reap got to it — the second reap reads
     * already-kfree'd slab memory and faults inside vfs_release_locks_for_task. */
	bool on_dead_queue;

	// User mode support
	uint64_t user_stack_top; // User stack virtual address (for user tasks)
	/* Size of that stack mapping.  Set on the exec paths beside
	 * user_stack_top and reported through SYS_GETPROCMAPS: the stack is
	 * mapped straight into the page tables rather than through the mmap
	 * region table, so without this field userspace has no way to learn
	 * its own main stack's extent -- which pthread_getattr_np() must
	 * report (a conservative garbage collector scans exactly that range). */
	uint64_t user_stack_size;
	uint64_t
		kernel_stack_top; // Kernel stack for syscalls/interrupts (for user tasks)
	void *kernel_stack_base; // Kernel stack allocation base (for freeing)

	// Process credentials: real/effective/saved uid+gid, fs ids, groups.
	// Inherited across fork/clone via the task-struct memcpy; fresh tasks
	// inherit the spawning task's creds (kernel tasks privileged), so none is
	// silently born root.  Enforcement consults cred.euid/egid/fsuid/etc.
	cred_t cred;

	/* File-mode creation mask.
	 *
	 * Per PROCESS, not per task and not global.  A global mask would let one
	 * user's umask change the modes of files another user creates; a
	 * per-task one would let two threads of the same process disagree about
	 * it.  The value that counts is the thread-group leader's, reached
	 * through task_umask()/task_set_umask(), so every thread of a process
	 * shares one mask -- which is what a process-wide filesystem context
	 * means.
	 *
	 * Inherited across fork/clone by the task-struct copy, and NOT reset by
	 * execve: a shell that sets a mask expects the commands it runs to
	 * inherit it. */
	uint32_t umask;

	/* Whether this process's private state may be handed to the user who
	 * owns it.  Zero once an exec has raised its privileges through a
	 * set-id bit.
	 *
	 * Without it the only thing standing between a user and a setuid
	 * program's memory is the id comparison in task_may_access(), which
	 * DERIVES protection from uid != euid.  That holds while the program is
	 * still privileged, but a binary that reads a secret as root and then
	 * drops back to the invoking user permanently (setuid(), which resets
	 * all three ids) becomes indistinguishable from a program the user
	 * started themselves -- while still holding what it read.  Recording
	 * the fact at exec time is the only way to keep refusing afterwards.
	 *
	 * Per PROCESS, like umask: reached through task_dumpable()/
	 * task_set_dumpable() so every thread of a process answers alike --
	 * they share the address space being protected.  Inherited across
	 * fork/clone by the task-struct copy, and recomputed on every execve. */
	uint32_t dumpable;

	// Job control / session
	int pgid;
	int sid;
	struct tty *ctty;

	// Wait linkage for blocking I/O
	struct task *wait_next;
	void *wait_channel;

	/* Count of filesystem rw-semaphores this task currently holds in
	 * SHARED mode.  A nested shared acquisition (e.g. a page-in during a
	 * read already holding a shared lock) must not defer to queued
	 * writers, or it deadlocks against them; a positive count bypasses
	 * writer-preference in the fs rwsem slow path. */
	int fs_rdepth;

	// Timer-based sleep support
	uint64_t
		wakeup_tick; // Tick count when task should wake (0 = not sleeping)

	// Signal handling state
	task_signal_state_t signals; // Full signal state

	// Saved syscall context for signal delivery (per-task, not global)
	uint64_t syscall_rsp; // User RSP on syscall entry
	uint64_t syscall_rip; // User RIP (return address)
	uint64_t syscall_rflags; // User RFLAGS
	uint64_t syscall_rax; // Syscall return value (for sigreturn)
	uint64_t syscall_rbp; // Callee-saved
	uint64_t syscall_rbx; // Callee-saved
	uint64_t syscall_r12; // Callee-saved
	uint64_t syscall_r13; // Callee-saved
	uint64_t syscall_r14; // Callee-saved
	uint64_t syscall_r15; // Callee-saved

	/* The caller-saved half, snapshotted from the kernel-stack frame that
	 * syscall_entry publishes.  A syscall return does not need these -- the
	 * ABI lets a syscall clobber them -- so nothing used to keep them, and
	 * a tracee stopped inside a syscall had no argument registers to show.
	 * They are also the syscall's own arguments, which is what a syscall
	 * entry/exit stop has to report.
	 *
	 * `syscall_regs_valid' says whether they describe this task's current
	 * kernel entry: they are only filled for a user task entering through
	 * syscall_entry, and reporting a previous syscall's arguments would be
	 * worse than reporting none. */
	uint64_t syscall_rdi, syscall_rsi, syscall_rdx;
	uint64_t syscall_r8, syscall_r9, syscall_r10;
	int syscall_regs_valid;

	/* The live frame those values were copied from, on this task's kernel
	 * stack, or NULL when not inside a syscall.
	 *
	 * The snapshot above is enough to REPORT registers, but not to change
	 * them: the return path restores from this frame -- sysret takes RFLAGS
	 * out of the popped r11 -- so a write to the snapshot would be read
	 * back correctly and then quietly discarded on return.  Setting the
	 * trap flag to single-step a tracee has the same requirement.
	 *
	 * Valid only for as long as the syscall is in progress, which is
	 * exactly as long as a tracee can be stopped inside one; cleared before
	 * syscall_handler returns, the same way preempt_frame is. */
	struct syscall_user_frame *syscall_frame;
	uint64_t
		syscall_kernel_rsp; // Kernel RSP for syscall return (set before call)

	/* A unix socket resolved from a descriptor by the current syscall, held
	 * for the length of that syscall so a sibling thread closing the same
	 * descriptor cannot destroy it mid-call.  Taken where the descriptor is
	 * resolved, released in one place when the syscall returns -- the
	 * alternative was a matching release on every early return of every
	 * socket syscall, which is how a reference gets leaked and a slot never
	 * comes back. */
	struct unix_socket *syscall_unix_ref;

	// Process name (set from argv[0] basename on execve)
	char comm[256];

	/* The executable this task is running, as an absolute path.
	 *
	 * `comm' is the basename and `cmdline' starts with argv[0], and neither
	 * is the file: a login shell's argv[0] is "-bash", which names nothing
	 * that can be opened.  A debugger attaching to a running process has no
	 * other way to find the binary -- and without it, symbols, breakpoints
	 * and shared-library relocation are all unavailable for a process it did
	 * not start itself.  Systems with a per-process filesystem publish the
	 * same string as /proc/PID/exe; this one reports it through ptrace. */
	char exe_path[256];

	// Full command line (argv joined by spaces, set on execve)
	char cmdline[1024];

	// Environment string (envp joined by spaces, set on execve)
	char environ[2048];

	// Timing / accounting
	uint64_t start_tick; // Tick count when task was created
	uint64_t utime_ticks; // Ticks spent in user mode
	uint64_t stime_ticks; // Ticks spent in kernel mode

	// Current working directory
	char cwd[256];

	/* ppoll()/pselect() install a temporary signal mask for the duration of
	 * the wait.  The caller's mask must NOT be put back before the kernel
	 * gets a chance to deliver what arrived — restoring it inline re-blocked
	 * the signal, so the handler never ran even though the wait had already
	 * returned EINTR.  Instead the original is parked here and put back
	 * either by the signal frame (so sigreturn restores it after the
	 * handler) or at syscall exit when no handler ran. */
	int sigmask_restore_pending;
	kernel_sigset_t sigmask_saved;

	/* chroot() jail root.  Empty means "no jail" (the whole system root).
	 * When set, textual path resolution (build_at_path) prepends it to the
	 * canonicalised absolute path, and the canonicaliser already clamps
	 * ".." at "/", so a jailed task cannot escape upward.  Inherited across
	 * fork and preserved across exec. */
	char root[256];

	// Implicit console I/O flags (O_NONBLOCK etc., for when fd 0/1/2 are
	// not backed by a real vfs_file but use the console TTY directly).
	uint32_t console_flags;

	// File descriptor table (legacy - used when files == NULL)
	struct vfs_file *fd_table[TASK_MAX_FDS];
	uint8_t fd_flags[TASK_MAX_FDS]; // Per-fd flags (FD_CLOEXEC = 1)

	// Memory management (legacy - used when mm == NULL)
	uint64_t brk; // Current program break (heap end)
	uint64_t brk_start; // Initial program break (heap start)
	/* Kernel return address of whoever put this task to sleep -- the
	 * WCHAN.  A hung process otherwise says nothing about WHY it is hung,
	 * and "ps shows it blocked" is not a diagnosis.  Recorded in
	 * sched_schedule() from the caller's return address, so no blocking
	 * site has to remember to set anything, and symbolised offline:
	 *   rm build/kernel.elf && make NO_STRIP=1
	 *   addr2line -f -e build/kernel.elf <wchan>
	 */
	uint64_t wchan_rip;

	/* Grown on demand; mmap_capacity entries, never above TASK_MAX_MMAP.
	 * Every task owns its own allocation -- a thread's is left empty
	 * because task_mm_owner() routes every lookup to the group leader,
	 * and fork gives the child a fresh copy rather than the parent's
	 * pointer. */
	mmap_region_t *mmap_regions;
	uint32_t mmap_capacity;
	/* One past the highest slot ever claimed.  The table only grows -- a
	 * process that once had four thousand mappings keeps the slots after
	 * it drops to forty -- and every lookup used to walk the whole
	 * CAPACITY.  At ~100 bytes a slot that is hundreds of kilobytes read
	 * per page fault, so a browser got slower the longer it ran and never
	 * got faster again.  Never lowered, so it can be read without the
	 * write lock: it is an upper bound, and nothing above it is in use. */
	uint32_t mmap_hwm;
	/* The slot that answered the last lookup.  Faults arrive in runs
	 * inside one mapping -- a heap being touched, a file being read -- so
	 * testing this first answers most of them without a scan at all.  A
	 * stale value costs nothing: it is re-checked against the address
	 * before it is used. */
	uint32_t mmap_hint;
	uint64_t mmap_base; // Base address for mmap allocations

	/* Guards this address space: the region table above, mmap_base, brk,
	 * and the page tables under `pml4`.  Only the THREAD-GROUP LEADER's
	 * copy is ever used — reach it through task_mm_owner(), exactly like
	 * the fields it protects. */
	mm_rwsem_t mmap_lock;
	/* Depth of this task's SHARED holds of any mmap_lock.  A nested shared
	 * acquisition (a fault taken while the pre-fault shield already holds
	 * the lock) must not defer to a queued writer, or the two deadlock:
	 * the writer waits for the first hold to drain and the first hold
	 * waits behind the writer.  Mirrors fs_rdepth above. */
	int mm_rdepth;

	// ========================================================================
	// THREAD GROUP SUPPORT (POSIX threads / clone)
	// ========================================================================

	// Thread group identification
	int tgid; // Thread group ID (= id for group leader, = leader's id for threads)
	struct task *group_leader; // Pointer to thread group leader
	struct task *
		thread_group_next; // Next thread in same thread group (circular list)
	struct task *
		thread_group_prev; // Previous thread in thread group (for O(1) removal)
	volatile int nr_threads; // Thread count in group (only valid in leader)
	/* Lifetime of the leader's task_t, as opposed to the leader's life.
	 *
	 * Every thread carries a bare `group_leader' pointer that is never
	 * cleared, and the exit path follows it long after the thread has been
	 * marked exited (task_close_open_files walks the LEADER's mmap table).
	 * nr_threads cannot protect that pointer: it counts threads that have
	 * not exited yet, and it reaches zero while the last thread is still
	 * inside its own teardown.  The parent's waitpid() only looks at
	 * has_exited, so it could reap and kfree the leader in exactly that
	 * window -- the thread then read the heap's free poison out of the
	 * leader's mmap table and dereferenced 0xdeadbeefdeadbeef.
	 *
	 * group_ref counts the live thread task_t STRUCTURES pointing here
	 * (the leader itself is not counted).  It is raised in
	 * thread_group_add and dropped in sched_remove_task just before the
	 * thread's kfree, so it spans the whole exit, not just the running
	 * life.  sched_remove_task refuses to destroy a leader while it is
	 * non-zero; the last thread out re-queues the leader for reaping.
	 * Both sides run under g_task_list_lock. */
	volatile int group_ref;
	/* Link for the dead-task queue.  The queue used to be a fixed array,
	 * and overflowing it DROPPED the task -- which never ran its
	 * destruction, so it never released its leader's group_ref, and that
	 * leader's whole address space was postponed for ever.  A dead task is
	 * off every other list, so it can carry the link itself and the queue
	 * cannot overflow. */
	struct task *dead_next;

	/* Set while the task is inside exit_mm_self(), i.e. executing its own
	 * address-space teardown.
	 *
	 * has_exited is set long before that teardown runs, and the scheduler
	 * refuses to requeue an exited task -- correctly, for one that has
	 * finished.  But mm_destroy_address_space() is deliberately
	 * preemptible: it walks 256 PML4 entries and frees thousands of pages
	 * in batches, dropping the allocator lock between them so interrupts
	 * can be serviced.  A timer tick anywhere in that walk therefore
	 * switched the task away for good, and everything it had not reached
	 * yet -- pages, stacks, page tables -- was never freed and never
	 * reachable again.  Nothing else can finish the walk on its behalf, so
	 * while this is set the task stays runnable. */
	volatile bool in_exit_teardown;

	/* Set the instant the task is declared exited, cleared only when it
	 * reaches the park loop -- i.e. while it is still EXECUTING its own
	 * exit path: waking waiters, returning through sys_exit, releasing its
	 * address space.
	 *
	 * has_exited and TASK_ZOMBIE are both set at the TOP of that path, and
	 * the scheduler read them as "finished" from that instant: the requeue
	 * path dropped the task for has_exited and every pick site skipped it
	 * for TASK_ZOMBIE.  A timer tick anywhere in the window therefore
	 * retired it for good, mid-sched_wake_wait_sleepers(): a zombie that
	 * had never finished waking its parent, and a parent that waited in
	 * waitpid() for ever.
	 *
	 * waitpid() finds children by has_exited, not by the state, so the task
	 * may run as READY here without being hidden from the parent -- and
	 * sched_remove_task() refuses to destroy it while this is set, so being
	 * reapable mid-exit cannot free the stack it is running on. */
	volatile bool in_exit_path;

	/* x87/SSE register file, saved and restored across context switches.
	 *
	 * Nothing did that before: FXSAVE/FXRSTOR appeared nowhere in the
	 * kernel, only OSFXSR being set in CR4.  So every task shared one set
	 * of FPU registers, and a task preempted mid-computation resumed with
	 * whatever the other task had left in them.  It shows up as a
	 * DETERMINISTIC arithmetic test failing once in a few hundred runs --
	 * a long-double round trip, say, where the value changed under the
	 * program rather than the code being wrong.
	 *
	 * FXSAVE needs 16-byte alignment and task_t comes from kalloc, so the
	 * area is oversized and the aligned start is computed once at init.
	 * `fpu_state` must be RECOMPUTED for a forked child: the wholesale
	 * task copy duplicates the pointer, which would leave the child saving
	 * its registers into its parent's task_t. */
	uint8_t fpu_area[FPU_STATE_MAX + FPU_STATE_ALIGN];
	uint8_t *fpu_state;

	/* Hardware debug registers: four watchpoint addresses, the status
	 * word as of this task's last #DB, and the control word.
	 *
	 * Per task rather than per CPU, and switched with the rest of the
	 * context, because a watchpoint belongs to the program being debugged
	 * and not to whatever core it happens to be running on.  DR6 is kept
	 * here rather than read live because the hardware register is cleared
	 * as soon as the fault is handled -- a debugger asks which watchpoint
	 * fired long after that, once the tracee has stopped.
	 *
	 * `dr_active' is the whole point of storing DR7 separately: reading and
	 * writing debug registers costs far more than an ordinary move, and
	 * almost no task has a watchpoint set, so the context switch tests this
	 * one byte and does nothing at all in the common case.
	 *
	 * Everything written here is sanitised first -- see ptrace_set_dbregs.
	 * These registers can halt the CPU on kernel addresses and, through
	 * DR7's GD bit, on access to the debug registers themselves, so a
	 * tracer's word is never taken for the value. */
	uint64_t dr_addr[4];
	uint64_t dr_status;
	uint64_t dr_control;
	volatile uint8_t dr_active;

	/* Non-zero while `fpu_state` holds the authoritative copy and the
	 * registers in the CPU are scratch -- set by kernel_fpu_begin().  The
	 * context switch consults it: saving on top of it would replace the
	 * task's real values with whatever the kernel left in the registers. */
	volatile int fpu_saved;
	/* Nesting depth of kernel_fpu_begin(). */
	volatile int fpu_kdepth;
	/* A transient pin on this task_t, taken by code that found the task on
	 * the global task list and must keep dereferencing it AFTER dropping
	 * that list's lock.
	 *
	 * The signal paths need exactly this: sched_kill_thread_group() and
	 * sys_exit_group() collect their siblings under g_task_list_lock and
	 * then signal them with the lock dropped, because signal_send() takes
	 * locks of its own.  A sibling reaped in that window was signalled
	 * through freed memory -- signal_send() reads `sigstate->sigfd_wq' out
	 * of the poisoned page and hands the poison to poll_notify_wq(), which
	 * spin-locks a wait-queue head at 0xfeedfacefeedfad6 and takes a GPF in
	 * the kernel.  Guarding with task_ptr_ok() before the call, which is
	 * what this used to do, narrows that window without closing it: the
	 * task can be freed between the check and the dereference.
	 *
	 * `condemned' is the other half.  It is set, under g_dead_thread_lock,
	 * by the destroy path at the instant it commits, so a pin taken under
	 * the same lock either wins (the destroy defers) or is refused (the
	 * caller skips that target).  There is no order in which both can
	 * believe they own the task_t. */
	volatile int pin_count;
	volatile bool condemned;

	/* Set when a leader's destruction was postponed because group_ref was
	 * still non-zero.  Tells the last thread to hand the leader back to
	 * dead_thread_queue() instead of leaking it. */
	volatile bool destroy_deferred;
	volatile int group_exit_code; // Exit code for exit_group()
	volatile bool group_exiting; // Set when exit_group() is called

	// Exit behavior
	int exit_signal; // Signal to send parent on exit (SIGCHLD for processes, 0 for threads)

	// CLONE_CHILD_CLEARTID / set_tid_address support
	uint64_t *clear_child_tid; // Address to write 0 and futex-wake on exit
	uint64_t *set_child_tid; // Address to write TID on creation

	// Per-task stack canary.  The context switcher writes this value into
	// the per-CPU GS:104 slot on every context switch so each task always
	// sees its own canary regardless of which CPU it runs on.
	uint64_t stack_canary;

	// TLS (Thread Local Storage) support
	uint64_t fs_base; // FS segment base for user TLS
	uint64_t gs_base; // GS segment base (usually not used by user)

	/* The user's DATA-SEGMENT SELECTORS, as they were on the last entry
	 * into the kernel from user mode.  A debugger asks for these; nothing
	 * else in the system does.
	 *
	 * They are snapshotted rather than read live because by the time a
	 * tracer looks, the tracee is parked and some other task owns the CPU
	 * whose segment registers would be read.  They are snapshotted rather
	 * than derived because the honest answer is whatever the processor
	 * actually holds, and that is knowable only at the moment of entry.
	 *
	 * The kernel never loads DS/ES/FS/GS after gdt_flush() at boot, so what
	 * is captured on entry IS what the program was running with.  In
	 * practice that is zero for every one of them: IRETQ to a lower
	 * privilege level nulls any of the four whose descriptor has DPL < CPL,
	 * and the boot value 0x10 is a ring-0 descriptor -- so the first return
	 * to user mode clears them and nothing ever sets them again.  SYSCALL
	 * and SYSRET leave them alone.  That matches what every x86-64 system
	 * reports, but it is measured here rather than assumed: a task that has
	 * never been to user mode reports zeroes because it has nothing else to
	 * report, not because zero was written in.
	 *
	 * Not restored on the way back out, and so not writable through
	 * PTRACE_SETREGS -- same reason CS and SS are not.  In 64-bit mode a
	 * data-segment selector selects nothing; FS and GS address through
	 * their MSR bases, which ARE per-task state and are reported
	 * separately as fs_base/gs_base. */
	uint16_t useg_ds, useg_es, useg_fs, useg_gs;

	// Robust futex support
	struct robust_list_head *robust_list; // Robust futex list head
	size_t robust_list_len; // Size of robust list head structure

	// Shared structures (NULL = use legacy per-task fields)
	mm_struct_t *mm; // Shared address space (CLONE_VM)
	files_struct_t *files; // Shared file descriptors (CLONE_FILES)
	sighand_struct_t *sighand; // Shared signal handlers (CLONE_SIGHAND)
} task_t;

/* THE descriptor table of `t`.  A thread created with CLONE_FILES shares one
 * files_struct with the rest of its thread group; every other task owns the
 * legacy in-task array.  EVERY fd lookup must go through this: the two arrays
 * used to diverge (flags were read from files->fd_flags while objects came
 * from the in-task table), which left a freshly cloned thread with an EMPTY
 * descriptor table — every fd >= 3 answered EBADF and its stdio fell back to
 * the console, bypassing the process's own redirection. */
static inline struct vfs_file **task_fds(task_t *t)
{
	return t->files ? t->files->fd_table : t->fd_table;
}

/* Helper: get the fd_flags byte for fd (uses files->fd_flags if shared, else task->fd_flags) */
static inline uint8_t task_get_fd_flags(task_t *t, unsigned fd)
{
	if (t->files)
		return t->files->fd_flags[fd];
	return t->fd_flags[fd];
}

/* Helper: set the fd_flags byte for fd */
static inline void task_set_fd_flags(task_t *t, unsigned fd, uint8_t flags)
{
	if (t->files)
		t->files->fd_flags[fd] = flags;
	else
		t->fd_flags[fd] = flags;
}

/* Descriptors 0/1/2 are the console when their table slot is EMPTY: the console
 * has no vfs_file object behind it, so "this is the terminal" is represented by
 * the absence of one.  That makes it indistinguishable from "closed" unless the
 * close is recorded separately -- which is what this flag does.
 *
 * It lives in the per-descriptor flag byte rather than as a sentinel POINTER in
 * the slot itself, because every descriptor-classifying site dereferences
 * whatever the slot holds.  A new pointer value would have to be excluded at
 * each of them and missing one faults the kernel; a flag can only be missed in
 * the harmless direction, where the descriptor reads as the console exactly as
 * it did before. */
#define FD_STDIO_CLOSED 0x02

/* True when `fd` is a standard descriptor still attached to the terminal. */
static inline int task_fd_is_console(task_t *t, uint64_t fd)
{
	return fd < 3 && task_fds(t)[fd] == NULL &&
	       !(task_get_fd_flags(t, (unsigned)fd) & FD_STDIO_CLOSED);
}

/* True when nothing occupies `fd`, so open()/dup() may claim it.  0/1/2 are
 * occupied by the console until the process closes them. */
static inline int task_fd_slot_free(task_t *t, unsigned fd)
{
	if (task_fds(t)[fd] != NULL)
		return 0;
	return fd >= 3 || (task_get_fd_flags(t, fd) & FD_STDIO_CLOSED);
}

void sched_init(void);
/* Allocate the next task id atomically.  Every task-creation path must use
 * this: a bare g_next_id++ races between CPUs and hands two live tasks the
 * same pid. */
int sched_alloc_task_id(void);
task_t *sched_add_task(task_entry_t entry, void *arg, void *stack_mem,
		       size_t stack_size);
/* Same, for a kernel thread that must run on one specific processor (per-CPU
 * service threads).  The binding has to be made here rather than by the caller
 * afterwards: the returned task may already be queued, and `on_cpu' names the
 * run queue that owns it. */
task_t *sched_add_task_on_cpu(task_entry_t entry, void *arg, void *stack_mem,
			      size_t stack_size, uint32_t cpu);
task_t *sched_add_user_task(task_entry_t entry, void *arg, uint64_t *pml4,
			    uint64_t user_stack, uint64_t kernel_stack);
void sched_tick(void);
void sched_schedule(
	void); // Core preemptive scheduler - switch to next ready task
void sched_yield_in_kernel(void); // In-kernel cooperative yield (no syscall)
void sched_run_ready(void);
/* Retire the boot thread so the boot processor schedules like any other.
 * Called once, at the end of init, and never returns. */
void sched_bsp_park(void);
task_t *sched_current(void);
int sched_has_user_tasks(void); // Check if any user tasks are running

/* Owner of the address-space bookkeeping (mmap_regions, brk, mmap_base).
 * Threads share the leader's address space, so all VM bookkeeping must be
 * read and written through the group leader — a thread's own task_t copy
 * of these fields is stale. */
/* Report what is still holding address spaces (diagnostics, DEBUG only).
 *
 * The switch is defined here as well as in mm/memory.h -- both are idempotent
 * -- because this header is often included first, and evaluating the test
 * before the definition exists would silently pick the stub in a DEBUG build
 * and collide with the real definition. */
#ifndef MM_LEAK_INSTRUMENTATION
#define MM_LEAK_INSTRUMENTATION DEBUG
#endif

/* Give a forked child its own copy of the parent's FPU state. */
void task_fpu_fork(task_t *child, task_t *parent);

/* Hand the hardware debug registers over as part of a context switch.  Does
 * nothing at all unless one of the two tasks actually has a watchpoint set. */
void task_debugreg_switch(task_t *prev, task_t *next);

/* Drop every watchpoint this task holds and disable the hardware if it is the
 * one running.  Called where debug state must not survive: exec (the image the
 * watchpoints referred to is gone, and if the exec gained privilege they would
 * be a window into it), and fork, since a child does not inherit them. */
void task_debugreg_clear(task_t *t);

/* Borrow the FPU/SSE registers for kernel code.
 *
 * The kernel runs on the CURRENT task's register file, and that file is only
 * written back to the task's save area at the next context switch -- so a
 * kernel SSE routine (the framebuffer blits) silently rewrites whatever
 * floating-point values the interrupted program had live, and the corrupted
 * values are what get saved.  Bracket any kernel use of these registers.
 *
 * Preemption inside the bracket is safe: begin() pushes the task's registers
 * to its save area and marks it authoritative, so the context switch leaves
 * that copy alone instead of overwriting it with the kernel's scratch. */
void kernel_fpu_begin(void);
void kernel_fpu_end(void);

#if MM_LEAK_INSTRUMENTATION
void sched_dump_task_leaks(void);
#else
static inline void sched_dump_task_leaks(void)
{
}
#endif

static inline task_t *task_mm_owner(task_t *t)
{
	return (t && t->group_leader) ? t->group_leader : t;
}

/* Register a lazy (demand-paged) region on a task — used by the ELF
 * loader for BSS ranges (file == NULL: zero-fill) and for demand-paged
 * executable/interpreter segments (file != NULL: page-in from `offset`).
 * Takes its own vfs reference on `file`.  Returns 0 on success, -1 if
 * the region table is full. */
int task_register_lazy_region(task_t *task, uint64_t start, uint64_t length,
			      uint64_t prot, struct vfs_file *file,
			      uint64_t offset);

// Preemptive scheduling API
void sched_preempt(
	interrupt_frame_t
		*frame); // Called from timer IRQ, performs context switch
int sched_need_resched(void); // Check if reschedule is needed
void sched_set_need_resched(task_t *t); // Mark task as needing reschedule
void sched_wake_expired_sleepers(
	uint64_t current_tick); // Wake tasks whose sleep timer expired
void sched_wake_channel(void *channel); // Wake all tasks waiting on a channel
/* Wake at most `max` tasks waiting on `channel`, in ONE pass over the task
 * list.  For broadcast channels whose waiter count is unbounded and where a
 * missed wake is self-correcting.
 *
 * sched_wake_channel() loops until a batch comes back partial, and its
 * termination argument -- that a woken task cannot re-block on the channel
 * before the call returns -- does not hold on SMP: a task woken and enqueued
 * on another CPU can be dispatched at once, find nothing to do and re-park
 * while the loop is still running, so the loop keeps finding work and keeps
 * re-taking the task list lock.  Harmless for a channel with a couple of
 * waiters; not for one that every polling process in the system waits on. */
int sched_wake_channel_once(void *channel, int max);
int sched_claim_wake(task_t *t,
		     task_state_t from); // Atomic from->READY claim (see sched.c)

/* Release the running task's address space now, in its own context, rather
 * than leaving it for the reaper.  Idempotent; the task must already be
 * unrunnable.  Called from sched_exit_park(). */
void exit_mm_self(task_t *task);

/* Where a finished thread goes: releases its address space, then parks with
 * interrupts enabled until it is reaped.  Never returns. */
__attribute__((noreturn)) void sched_exit_park(void);

/* Record which address space this CPU holds, around a CR3 load.
 *
 * `_enter` before the load, `_done` after it, both with the PHYSICAL page-table
 * root.  Between the two the CPU counts as holding the address space it is
 * leaving and the one it is entering, which is what makes it safe to invalidate
 * only the CPUs that are listed: naming one too many wastes an interrupt,
 * naming one too few leaves it using translations that have been withdrawn.
 *
 * Callers go through mm_switch_address_space(); these are exposed only for the
 * few places that load CR3 directly. */
void sched_mmu_track_enter(uint64_t pml4_phys);
void sched_mmu_track_done(uint64_t pml4_phys);

// Global task list lock (protects the all-tasks linked list)
extern spinlock_t g_task_list_lock;

/* Closes the sleep/wake race between waitpid() and the children it waits on.
 * Taken by the parent around "check for a reportable child, then mark myself
 * BLOCKED", and by a child around "is my parent asleep in waitpid?".  See the
 * long note at its definition in sched.c. */
extern spinlock_t g_wait_lock;

// SMP support
void sched_enable_smp(
	void); // Called after per-CPU init to enable per-CPU current task
int sched_is_smp(void); // Check if SMP mode is enabled

// Per-CPU scheduling API
void sched_init_ap(uint32_t cpu_id); // Initialize per-CPU scheduler for AP
void sched_enqueue_ready(
	task_t *task); // Enqueue task to its assigned CPU's run queue
void sched_load_balance(
	void); // Pull tasks from busiest CPU (called from timer)

// Process management
task_t *sched_fork_current(void); // Fork current task with COW
void sched_remove_task(task_t *task); // Remove task from scheduler
void sched_defer_reap(
	task_t *child); // waitpid reap: unlink from parent, destroy deferred
task_t *sched_find_task_by_id(uint32_t pid); // Find task by PID
task_t *sched_find_task_by_id_locked(
	uint32_t pid); // Find task by PID (caller holds g_task_list_lock)
task_t *sched_task_by_canary(
	uint64_t canary); // Crash diagnostic: lock-free, __stack_chk_fail only

/* Parent/child list edits.  The list is protected by g_wait_lock, which
 * sys_waitpid() also holds while walking it -- an unlocked edit both corrupts
 * the list and lets a waiter miss the exit it was about to sleep through.
 * The _locked forms are for callers that already hold it. */
void sched_add_child(task_t *parent, task_t *child); // Add child to parent
void sched_add_child_locked(task_t *parent, task_t *child);
void sched_remove_child(task_t *parent,
			task_t *child); // Remove child from parent
void sched_remove_child_locked(task_t *parent, task_t *child);
/* Tell a task's parent it has finished (wake waitpid, send SIGCHLD, or reap it
 * when the parent ignores SIGCHLD).  Idempotent. */
void sched_notify_parent_of_exit(task_t *task);

/* Threads signalled in one batch when a group is torn down. A larger group is
 * not a correctness problem: group_exiting is already set, so the stragglers go
 * on the next pass. */
#define TASK_GROUP_KILL_MAX 64

/* Terminate every OTHER thread of `task`'s group.  Used by exit_group and by a
 * fatal default-action signal, which ends the PROCESS and not just the thread
 * that received it. */
void sched_kill_thread_group(task_t *task, int exit_code);

/* Enter a job-control stop.  Called ONLY by the stopping task itself, from its
 * signal-delivery point: a signaller marks the signal pending and wakes the
 * target, and the target comes here.  Returns once continued -- or at once,
 * with the stop cancelled, when a continue had already raced in. */
void sched_do_signal_stop(task_t *t, int signo);

void sched_reparent_children(
	task_t *task); // Reparent children to init (task 1)
uint32_t sched_get_ppid(task_t *task); // Get parent PID
void sched_reap_zombies(task_t *parent); // Reap all zombie children of parent
void sched_mark_task_exited(task_t *task, int status);
void sched_signal_task(task_t *task, int sig);
/* Same, naming the process that asked for it: kill(2) and its group forms.
 * The pair (si_pid, si_uid) it records is the only account of the sender a
 * sigwaitinfo()/signalfd() reader ever gets.  Pass NULL for the kernel's own
 * signals (tty ^C, SIGPIPE, hangup), which have no sender. */
void sched_signal_task_from(task_t *task, int sig, task_t *sender);

// Record the /sbin/init task (PID 1).  Once set, the scheduler protects it
// from ordinary signals and panics if it ever exits.
extern task_t *g_init_task;
void sched_set_init_task(task_t *t);

// True for the kernel's swapper-class tasks (bootstrap + per-CPU idle), which
// should be hidden from process listings.
int sched_task_hidden(const task_t *t);
/* Kernel-originated group signal (tty job control, hangup): unconditional. */
/* Does process group `pgid' have a member in session `sid'?  1 yes, 0 no
 * live member found at all, -1 the group exists only in another session.
 * tcsetpgrp()'s session rule is the only caller. */
int sched_pgrp_in_session(int pgid, int sid);

void sched_signal_pgrp(int pgid, int sig);
/* kill(2)'s group forms: credential-checked per member, sig == 0 probes.
 * 0 if any member was signalled, -EPERM if none were permitted, -ESRCH if the
 * group is empty. */
int sched_signal_pgrp_checked(int pgid, int sig);
int sched_signal_all(struct task *sender, int sig);
int sched_pgid_exists(int pgid);
struct tty; // forward declaration for dump output
void sched_dump_tasks(struct tty *tty); // Debug: dump all task states
void sched_print_tasks(void); // Panic-safe: dump tasks via kprintf

// ============================================================================
// THREAD GROUP MANAGEMENT
// ============================================================================

// Thread group operations
void thread_group_init(
	task_t *leader); // Initialize task as thread group leader
void thread_group_add(task_t *leader, task_t *thread); // Add thread to group
void thread_group_remove(task_t *thread);
/* True if `t' is safe to dereference: non-NULL and a kernel address.  Used by
 * the thread-group ring walks, where a freed task_t reads back as the slab
 * poison -- non-NULL, non-canonical, and fatal to touch with the task-list
 * lock held and interrupts off. */
bool task_ptr_ok(const task_t *t); // Remove thread from group

/* Keep `t' allocated across a window where no lock protects it.
 *
 * pin: call with g_task_list_lock HELD -- being on that list is what proves
 * the task is alive at the instant of the call.  Returns false if the task is
 * already committed to destruction, in which case the caller must not touch
 * it again.  unpin: call with no locks held; the last unpin of a task whose
 * destruction was postponed hands it back to the reaper. */
bool sched_task_pin(task_t *t);
void sched_task_unpin(task_t *t);
/* Wake every thread of `proc`'s thread group that is parked in waitpid().
 * A wait belongs to the PROCESS -- children hang off the group leader and any
 * thread may reap them -- so a notifier that wakes only the task it recorded as
 * the parent leaves a waiting worker thread asleep for good. */
void sched_wake_wait_sleepers(task_t *proc);
int thread_group_count(task_t *task); // Get number of threads in group
void thread_group_signal_all(task_t *task,
			     int sig); // Signal all threads in group

// Iterate over all threads in a thread group
#define for_each_thread(leader, t)                                            \
	for ((t) = (leader); (t); (t) = ((t)->thread_group_next == (leader) ? \
						 NULL :                       \
						 (t)->thread_group_next))

// ============================================================================
// SHARED STRUCTURE MANAGEMENT
// ============================================================================

// mm_struct operations
mm_struct_t *mm_struct_create(uint64_t *pml4); // Create new mm_struct
mm_struct_t *mm_struct_clone(mm_struct_t *src); // Clone mm_struct (for fork)
void mm_struct_get(mm_struct_t *mm); // Increment refcount
void mm_struct_put(mm_struct_t *mm); // Decrement refcount, free if 0

// files_struct operations
files_struct_t *files_struct_create(void); // Create new files_struct
files_struct_t *files_struct_clone(files_struct_t *src); // Clone (for fork)
void files_struct_get(files_struct_t *files); // Increment refcount
void files_struct_put(files_struct_t *files); // Decrement refcount, free if 0

// sighand_struct operations
sighand_struct_t *sighand_struct_create(void); // Create new sighand_struct
sighand_struct_t *
sighand_struct_clone(sighand_struct_t *src); // Clone (for fork)
void sighand_struct_get(sighand_struct_t *sighand); // Increment refcount
void sighand_struct_put(
	sighand_struct_t *sighand); // Decrement refcount, free if 0

// ============================================================================
// TLS (THREAD LOCAL STORAGE) SUPPORT
// ============================================================================

// Set FS base for a task (used by CLONE_SETTLS and arch_prctl)
void task_set_fs_base(task_t *task, uint64_t base);
uint64_t task_get_fs_base(task_t *task);

// Apply FS base on context switch (called by scheduler)
void task_load_tls(task_t *task);

// Check if FSGSBASE instructions are supported
bool cpu_has_fsgsbase(void);

/* Snapshot the user's data-segment selectors into `t'.
 *
 * Called from every point where the kernel is entered FROM USER MODE and
 * nowhere else: the values are only the program's while nothing has run in
 * between, and a kernel-mode entry would record the kernel's.  See the
 * useg_* fields for why this is a snapshot and not a live read.
 *
 * Four register-to-register moves, on the syscall and interrupt entry paths.
 * `mov %seg, r32' zero-extends, so no partial-register merge is involved and
 * the cost is the four moves and four stores -- lost in the noise of an entry
 * that has already spilled fifteen general registers. */
static inline void task_capture_user_segments(task_t *t)
{
	unsigned int ds, es, fs, gs;

	if (!t)
		return;
	__asm__ volatile("mov %%ds, %0\n\t"
			 "mov %%es, %1\n\t"
			 "mov %%fs, %2\n\t"
			 "mov %%gs, %3"
			 : "=r"(ds), "=r"(es), "=r"(fs), "=r"(gs));
	t->useg_ds = (uint16_t)ds;
	t->useg_es = (uint16_t)es;
	t->useg_fs = (uint16_t)fs;
	t->useg_gs = (uint16_t)gs;
}

// ============================================================================
// INTERNAL FUNCTIONS (exported for syscall.c)
// ============================================================================

// Global task ID allocator
extern int g_next_id;

// Add task to global task list (must hold g_task_list_lock)
void task_list_add(task_t *t);

// ============================================================================
// LOAD AVERAGE AND SYSTEM STATISTICS
// ============================================================================
void sched_calc_load(void); // Update load averages (call from timer)
/* The calling process's file-mode creation mask, and a setter returning the
 * previous value.  Both resolve to the thread-group leader, so all threads of a
 * process see one mask. */
uint32_t task_umask(task_t *t);
uint32_t task_set_umask(task_t *t, uint32_t mask);

/* Whether a process's private state may be handed to the user who owns it.
 * Zero once an execve has raised its privileges through a set-id bit; see the
 * `dumpable' field.  Both resolve to the thread-group leader, so every thread
 * of a process answers alike -- they share the address space being protected. */
int task_dumpable(task_t *t);
void task_set_dumpable(task_t *t, int dumpable);

/* Next value for task_t.incarnation.  Monotonic and 64-bit: it names one
 * occupant of a pid, so a stored (pid, incarnation) pair can tell "still the
 * task I meant" from "that number belongs to somebody else now". */
uint64_t task_next_incarnation(void);

/* ---- Tracing (ptrace) -----------------------------------------------------
 * The scheduler-side half of ptrace: parking a tracee and telling its tracer.
 * The request dispatch itself lives in syscall.c. */

/* Is `t' traced, and by a tracer that still exists?  Resolves the stored
 * (pid, incarnation) pair; a tracer that has exited answers 0 and the link is
 * cleared.  Caller must hold g_task_list_lock. */
task_t *task_tracer_locked(task_t *t);

/* True if `t' is traced by a tracer that is not privileged.  Used by execve to
 * refuse a set-id transition: the tracer already controls this task
 * completely, so raising its ids would hand the tracer's owner the new
 * identity.  Takes g_task_list_lock itself. */
bool task_traced_by_unprivileged(task_t *t);

/* Park `t' in a trace stop and tell its tracer.  `signo' is the signal that
 * caused it (SIGTRAP for an event stop), `event' is a PTRACE_EVENT_* code or 0
 * for a plain signal-delivery stop, `msg' is the value PTRACE_GETEVENTMSG will
 * return.  Called by the task about to stop, in its own context. */
void task_ptrace_stop(task_t *t, int signo, int event, unsigned long msg);

/* Stop `t' because `signo' is about to be delivered to it, and report the
 * signal the tracer wants delivered instead -- `signo' if it said nothing,
 * another signal if it substituted one, or 0 to discard it.
 *
 * Returns `signo' unchanged for an untraced task, so callers can use it
 * without asking first.  Parks the caller, so it must only be used where
 * sleeping is allowed: the syscall return path and the exception handlers,
 * not from a hardware interrupt. */
int task_ptrace_signal_stop(task_t *t, int signo,
			    const siginfo_t *info);

/* Park this task if its process is in a trace stop.  Called on the way back to
 * user mode, where stopping is safe; does nothing for a task that is not
 * traced or whose process is running. */
void task_ptrace_group_park(task_t *t);

/* Stop / resume every thread of `t`'s process.  The stop is silent for the
 * siblings -- only the thread that hit the event reports one. */
void task_ptrace_group_stop_siblings(task_t *t);
void task_ptrace_group_resume(task_t *t);

/* Detach `t' from its tracer, resuming it if it is parked in a trace stop.
 * Used by PTRACE_DETACH and when a tracer exits. */
void task_ptrace_detach(task_t *t);

/* Does `tracer' have any tracee matching waitpid's pid selector?  Consumes
 * nothing -- it is how waitpid tells "nothing ready yet" (sleep) from "nothing
 * to wait for" (ECHILD). */
int task_has_tracees(task_t *tracer, int64_t pid_sel);

/* Collect one uncollected trace-stop event from a tracee of `tracer',
 * honouring the same pid selector.  Returns the tracee's pid and fills
 * *status, or 0 if there is nothing to report.  The event is consumed, so a
 * caller that only wants to know whether tracees exist must use
 * task_has_tracees() instead.
 *
 * Everything happens under g_task_list_lock and only plain values come back,
 * so no task pointer outlives the lock that keeps it valid. */
int task_reap_trace_stop(task_t *tracer, int64_t pid_sel, int *status);

void sched_get_loadavg(
	unsigned long
		loads[3]); // Get 1/5/15 min load averages (<<16 fixed-point)
int sched_get_nr_running(void); // Count of runnable tasks
int sched_get_nr_procs(void); // Total process count

#endif // _KERNEL_SCHED_H_
