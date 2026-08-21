// LikeOS-64 -- fork, clone and vfork.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/fs/icache.h>
#include <kernel/dev/rand/random.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>

// DEPRECATED: These global externs are no longer used directly.
// Syscall context is now stored per-CPU in percpu_t and copied to task->syscall_*.
// Kept for backward compatibility but should not be referenced in new code.
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;
extern uint64_t syscall_saved_user_rbp;
extern uint64_t syscall_saved_user_rbx;
extern uint64_t syscall_saved_user_r12;
extern uint64_t syscall_saved_user_r13;
extern uint64_t syscall_saved_user_r14;
extern uint64_t syscall_saved_user_r15;

// External: user_mode_iret_trampoline from syscall.asm
extern void user_mode_iret_trampoline(void);
extern void fork_child_return(void);

// SYS_FORK - fork current process
int64_t sys_fork(void)
{
	task_t *cur = sched_current();
	if (!cur || cur->privilege != TASK_USER) {
		return -1;
	}

	// Use task-local copies of user context, not globals!
	// Globals can be overwritten if preemption switches to another task.
	uint64_t user_rip = cur->syscall_rip; // Where to resume execution
	uint64_t user_rsp = cur->syscall_rsp; // User stack pointer
	uint64_t user_rflags = cur->syscall_rflags; // Saved RFLAGS

	// Create child with cloned address space and file descriptors
	task_t *child = sched_fork_current();
	if (!child) {
		return -1; // Fork failed
	}

	// Set up child's kernel stack to return to userspace
	// When the child is scheduled, it will resume at user_rip with fork() returning 0
	//
	// Stack layout (from top to bottom):
	// 1. Saved user callee-saved registers (RBP, RBX, R12-R15)
	// 2. IRET frame: SS, RSP, RFLAGS, CS, RIP (to return to userspace)
	// 3. RAX value (0 for child's fork return value)
	// 4. Saved callee-saved registers for ctx_switch_asm (r15-rbp)
	// 5. Return address (fork_child_return trampoline)

	uint64_t *k_sp = (uint64_t *)child->kernel_stack_top;
	k_sp = (uint64_t *)((uint64_t)k_sp & ~0xFUL); // Align to 16 bytes

	// Push user callee-saved registers (fork_child_return will restore these)
	// IMPORTANT: Use task-local copies (cur->syscall_*) not globals!
	// Globals can be overwritten by preemption switching to another task.
	*(--k_sp) = cur->syscall_r15;
	*(--k_sp) = cur->syscall_r14;
	*(--k_sp) = cur->syscall_r13;
	*(--k_sp) = cur->syscall_r12;
	*(--k_sp) = cur->syscall_rbx;
	*(--k_sp) = cur->syscall_rbp;

	// Push IRET frame (used by fork_child_return to return to userspace)
	*(--k_sp) = 0x1B; // SS: user data segment
	*(--k_sp) = user_rsp; // User stack pointer
	*(--k_sp) = user_rflags | 0x200; // RFLAGS with interrupts enabled
	*(--k_sp) = 0x23; // CS: user code segment
	*(--k_sp) = user_rip; // Resume at parent's fork() call site

	// Push fork return value for child (0)
	*(--k_sp) = 0; // RAX = 0 (child sees fork() return 0)

	// Push callee-saved registers (ctx_switch_asm will restore these)
	*(--k_sp) = (uint64_t)
		fork_child_return; // Return address: sets RAX=0 and does IRET
	// RFLAGS restored by ctx_switch_asm's popfq.  IF set, matching the state a
	// fresh child used to inherit from the switching path's sti (sched_schedule
	// / sched_run_ready both sti *before* ctx_switch_asm).  fork_child_return
	// only requires that sched_after_fork_child run before its iretq, not that
	// interrupts be off.
	*(--k_sp) = 0x202; // RFLAGS (kernel): reserved bit 1 + IF
	*(--k_sp) = 0; // RBP (kernel)
	*(--k_sp) = 0; // RBX (kernel)
	*(--k_sp) = 0; // R12 (kernel)
	*(--k_sp) = 0; // R13 (kernel)
	*(--k_sp) = 0; // R14 (kernel)
	*(--k_sp) = 0; // R15 (kernel)

	child->sp = k_sp;

	// CRITICAL: Save child PID BEFORE enqueueing!
	// On SMP, another CPU might run the child, it exits, and dead_thread_reap
	// frees it before we return from sched_enqueue_ready. Accessing child->id
	// after enqueue would be use-after-free.
	int32_t child_pid = child->id;

	/* Hand the child to the same tracer, if it asked for that.
	 *
	 * Done BEFORE the child is made runnable, so it cannot reach user code
	 * untraced -- the whole point of following a fork is to be attached
	 * before the child's first instruction, and a child that gets a head
	 * start is a child whose early breakpoints are missed.  The child stops
	 * itself at the next opportunity, in its own context.
	 *
	 * The tracer is recorded as the process, matching every other place a
	 * tracer is named (see ptrace_lock_tracee). */
	bool follow_child = (cur->tracer_pid != 0 &&
			     (cur->ptrace_options & PTRACE_O_TRACEFORK));

	if (follow_child) {
		child->tracer_pid = cur->tracer_pid;
		child->tracer_incarnation = cur->tracer_incarnation;
		child->ptrace_options = cur->ptrace_options;

		/* Parked instead of enqueued, so it cannot reach user code
		 * before the tracer has seen it.  Following a fork exists to
		 * let a debugger attach to the child BEFORE its first
		 * instruction; a child given a head start is a child whose
		 * early breakpoints are missed.  PTRACE_CONT on it is what
		 * releases it, and that enqueues. */
		task_ptrace_stop(child, SIGTRAP, PTRACE_EVENT_FORK, 0);
	} else {
		sched_enqueue_ready(child);
	}

	// Set need_resched so the parent yields at the next opportunity,
	// giving the new child process a chance to start promptly.
	cur->need_resched = 1;

	/* And tell the tracer about the parent's side of the fork, carrying the
	 * new pid for PTRACE_GETEVENTMSG -- otherwise a tracer has no way to
	 * learn which process the child stop it is about to see belongs to.
	 * `child' is deliberately not touched here: it may already have been
	 * reaped, so only the saved pid is used. */
	if (follow_child)
		task_ptrace_stop(cur, SIGTRAP, PTRACE_EVENT_FORK,
				 (unsigned long)child_pid);

	// Parent returns child's PID (saved before enqueue to avoid use-after-free)
	return child_pid;
}

// ============================================================================
// SMP/THREADING SYSCALLS - FULL IMPLEMENTATION
// ============================================================================

// Clone flags (conventional Unix ABI values)
#define CLONE_VM 0x00000100 // Share memory space
#define CLONE_FS 0x00000200 // Share filesystem info
#define CLONE_FILES 0x00000400 // Share file descriptors
#define CLONE_SIGHAND 0x00000800 // Share signal handlers
#define CLONE_PTRACE 0x00002000 // Allow tracing child
#define CLONE_VFORK 0x00004000 // vfork() semantics
#define CLONE_PARENT 0x00008000 // Same parent as cloner
#define CLONE_THREAD 0x00010000 // Same thread group
#define CLONE_NEWNS 0x00020000 // New mount namespace
#define CLONE_SYSVSEM 0x00040000 // Share SysV semaphore undo
#define CLONE_SETTLS 0x00080000 // Set TLS
#define CLONE_PARENT_SETTID 0x00100000 // Set parent's TID
#define CLONE_CHILD_CLEARTID 0x00200000 // Clear child's TID on exit
#define CLONE_DETACHED 0x00400000 // Unused
#define CLONE_UNTRACED 0x00800000 // Cannot force trace
#define CLONE_CHILD_SETTID 0x01000000 // Set child's TID
#define CLONE_STOPPED 0x02000000 // Start in TASK_STOPPED
#define CLONE_NEWUTS 0x04000000 // New UTS namespace
#define CLONE_NEWIPC 0x08000000 // New IPC namespace

// Forward declare from sched.c
extern void thread_group_add(task_t *leader, task_t *thread);
extern void thread_group_init(task_t *leader);
extern mm_struct_t *mm_struct_create(uint64_t *pml4);
extern void mm_struct_get(mm_struct_t *mm);
extern files_struct_t *files_struct_create(void);
extern files_struct_t *files_struct_clone(files_struct_t *src);
extern void files_struct_get(files_struct_t *files);
extern sighand_struct_t *sighand_struct_create(void);
extern sighand_struct_t *sighand_struct_clone(sighand_struct_t *src);
extern void sighand_struct_get(sighand_struct_t *sighand);
extern void task_set_fs_base(task_t *task, uint64_t base);

// PID allocator (from sched.c)
extern int g_next_id;
extern spinlock_t g_task_list_lock;

// SYS_CLONE - create a new thread or process
// Full implementation with all CLONE_* flags
int64_t sys_clone(uint64_t flags, uint64_t child_stack,
		  uint64_t parent_tidptr, uint64_t child_tidptr,
		  uint64_t tls)
{
	task_t *cur = sched_current();
	if (!cur || cur->privilege != TASK_USER) {
		return -EPERM;
	}

	// Validate flag combinations
	// CLONE_THREAD requires CLONE_SIGHAND which requires CLONE_VM
	if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) {
		return -EINVAL;
	}
	if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) {
		return -EINVAL;
	}

	// Extract flag meanings
	bool share_vm = (flags & CLONE_VM) != 0;
	bool share_files = (flags & CLONE_FILES) != 0;
	bool share_sighand = (flags & CLONE_SIGHAND) != 0;
	bool is_thread = (flags & CLONE_THREAD) != 0;
	bool set_tls = (flags & CLONE_SETTLS) != 0;
	bool set_parent_tid = (flags & CLONE_PARENT_SETTID) != 0;
	bool set_child_tid = (flags & CLONE_CHILD_SETTID) != 0;
	bool clear_child_tid = (flags & CLONE_CHILD_CLEARTID) != 0;

	// For threads, child_stack is required
	if (is_thread && child_stack == 0) {
		return -EINVAL;
	}

	// Allocate child task structure
	task_t *child = (task_t *)kalloc(sizeof(task_t));
	if (!child) {
		return -ENOMEM;
	}

	// Allocate kernel stack for child (guarded: not-present guard page below)
	uint8_t *k_stack_mem =
		(uint8_t *)mm_alloc_guarded_kstack(KERNEL_STACK_SIZE);
	if (!k_stack_mem) {
		kfree(child);
		return -ENOMEM;
	}
	uint64_t k_stack_top =
		((uint64_t)(k_stack_mem + KERNEL_STACK_SIZE)) & ~0xFUL;

	// Initialize child from parent
	mm_memcpy(child, cur, sizeof(task_t));

	/* Per-task state the copy must not leave pointing at the parent, for
	 * EVERY kind of clone -- a thread needs its own FPU register file just
	 * as much as a forked process does: the registers belong to the
	 * execution context, not to the address space it shares. */
	child->in_exit_teardown = false;
	child->in_exit_path = false;
	task_fpu_fork(child, cur);

	/* A new occupant of a pid, and not traced by whoever traces the parent.
	 * Both for the same reasons as in sched_fork_current(): a stale tracer
	 * link would park the new task in a stop nobody collects, and a reused
	 * pid must be distinguishable from the task that held it before.
	 * Tracing a new thread or child is opt-in (PTRACE_O_TRACECLONE /
	 * TRACEFORK), never inherited by the copy. */
	child->incarnation = task_next_incarnation();
	child->tracer_pid = 0;
	child->tracer_incarnation = 0;

	/* A new THREAD of a traced process is traced too.
	 *
	 * A thread is not a separate program: it shares the address space, the
	 * file descriptors and the identity of the process a debugger attached
	 * to, and a debugger that cannot see it cannot debug that process --
	 * it shows one stack for a program doing its work on five threads, and
	 * a fault on any other thread arrives with nobody to report it to.
	 *
	 * So the tracer follows the thread group.  A forked CHILD is a
	 * different matter and keeps the clearing above: it is its own
	 * process, and tracing it is opt-in through PTRACE_O_TRACEFORK,
	 * because a debugger that suddenly owned every descendant would stop
	 * processes it never asked about.
	 *
	 * The clone flags are what distinguishes them: CLONE_THREAD means this
	 * is another thread of the same process. */
	child->ptrace_options = 0;
	if ((flags & CLONE_THREAD) && cur->tracer_pid != 0) {
		child->tracer_pid = cur->tracer_pid;
		child->tracer_incarnation = cur->tracer_incarnation;

		/* And it is traced on the same TERMS.
		 *
		 * The options say which events stop the tracee, and they are
		 * asked for once, against the process, before any of its
		 * threads exist.  Leaving them at zero here meant only the
		 * thread the debugger had spoken to ever announced anything:
		 * a thread created by the main thread was reported, and a
		 * thread created by THAT thread was not, so a program whose
		 * threads spawn their own workers -- which is most of them --
		 * grew threads the debugger never heard about until something
		 * else happened to stop the process.  Inherited here, so every
		 * thread of a traced process reports what the tracer asked
		 * for, whichever thread it was born from. */
		child->ptrace_options = cur->ptrace_options;
	}

	child->ptrace_syscall_trace = 0;
	child->ptrace_in_syscall = 0;
	child->ptrace_stopped = 0;
	child->ptrace_stop_signo = 0;
	child->ptrace_event = 0;
	child->ptrace_msg = 0;
	child->ptrace_exiting = 0;
	child->ptrace_notify_seq = 0;
	/* The copy duplicated a pointer into the PARENT's kernel stack.  The
	 * child has its own and returns through a different path entirely, so
	 * that frame is not merely stale, it belongs to somebody else -- a
	 * tracer writing registers through it would corrupt the parent's
	 * in-progress syscall. */
	child->syscall_frame = NULL;
	child->syscall_regs_valid = 0;

	/* The copy above duplicated the POINTER to the parent's region table,
	 * not the table.  A CLONE_VM thread gets an empty one of its own --
	 * its bookkeeping is owner-routed to the group leader and it must hold
	 * no references of its own -- while a fork child gets a copy of the
	 * parent's.  Sharing the pointer would have the two free one
	 * allocation twice.
	 *
	 * Clone from the OWNER, not from the calling thread.  Because a
	 * CLONE_VM thread keeps an empty table on purpose, forking FROM one
	 * copied nothing: the child came up with no regions at all and died on
	 * its first write to a lazy page, with no region to explain the
	 * address.  Only a threaded program could hit it -- forking from a
	 * single-threaded one, the caller IS the owner. */
	task_t *mm_src = task_mm_owner(cur);
	bool got_regions;

	if (share_vm) {
		/* A CLONE_VM thread gets an empty table and takes no
		 * references: its bookkeeping is owner-routed to the group
		 * leader, and the leader-only release at exit would not
		 * balance them. */
		got_regions = mm_regions_init(child);
	} else {
		/* The copy and the references it implies are ONE locked step;
		 * see mm_regions_clone_ref(). */
		got_regions = mm_regions_clone_ref(child, mm_src);
	}
	if (!got_regions) {
		mm_free_guarded_kstack(k_stack_mem, KERNEL_STACK_SIZE);
		kfree(child);
		return -ENOMEM;
	}

	/* The rest of the leader-only address-space state, for the same reason
	 * and from the same place.  A thread's own copies of these are stale by
	 * design; a fork child becomes its own owner and must start from the
	 * values that were actually in use. */
	if (!share_vm) {
		child->brk_start = mm_src->brk_start;
		child->brk = mm_src->brk;
		child->mmap_base = mm_src->mmap_base;
	}

	/* A fresh address space needs a fresh lock.  The wholesale copy took
	 * the parent's, which another thread in the group may have been
	 * holding at that instant -- inherited locked, it would wedge the
	 * child on its first fault.  (A thread's own copy is never used, so
	 * resetting it unconditionally costs nothing.) */
	mm_rwsem_init(&child->mmap_lock, "mmap_lock");
	child->mm_rdepth = 0;

	/* Fresh kernel-stack canary — same rationale as sched_fork_current:
	 * the wholesale copy duplicated the parent's canary; the child's only
	 * kernel context is the hand-built fork_child_return frame, so no live
	 * frame carries the old value and regenerating is safe. */
	child->stack_canary = generate_stack_canary();

	// Assign unique ID (atomic; no lock needed just for the counter)
	uint64_t irq_flags;
	child->id = sched_alloc_task_id();

	// Basic child setup
	/* No unreported stop/continue of its own: the wholesale copy above
	 * duplicated the parent's, which would make the parent's next
	 * waitpid(WUNTRACED/WCONTINUED) report this running child as stopped. */
	child->jc_stop_signo = 0;
	child->term_sig = 0;
	child->sigmask_restore_pending = 0;
	child->jc_continued = 0;
	child->state = TASK_READY;
	child->kernel_stack_top = k_stack_top;
	child->kernel_stack_base = k_stack_mem;
	child->rq_next = NULL;
	child->on_rq = false;
	child->wait_next = NULL;
	child->wait_channel = NULL;
	child->wakeup_tick = 0;
	child->need_resched = 0;
	child->remaining_ticks = SCHED_TIME_SLICE;
	child->preempt_frame = NULL;
	child->exit_code = 0;
	child->has_exited = false;
	child->exit_lock = 0;
	child->is_fork_child = true;
	child->first_child = NULL;
	child->next_sibling = NULL;

	// Handle CLONE_VM (share address space)
	if (share_vm) {
		// Share parent's address space with reference counting
		if (cur->mm) {
			// Parent already uses mm_struct
			mm_struct_get(cur->mm);
			child->mm = cur->mm;
			child->pml4 = cur->mm->pml4;
		} else {
			// First time: create mm_struct from parent's legacy pml4
			cur->mm = mm_struct_create(cur->pml4);
			if (!cur->mm) {
				mm_free_guarded_kstack(k_stack_mem,
						       KERNEL_STACK_SIZE);
				/* The child already owns a region table (and, past the
				 * clone, references on every file-backed region);
				 * abandoning it here without this leaked that
				 * allocation on every failed fork. */
				mm_regions_free(child);
				kfree(child);
				return -ENOMEM;
			}
			cur->mm->brk = cur->brk;
			cur->mm->brk_start = cur->brk_start;
			cur->mm->mmap_base = cur->mmap_base;

			mm_struct_get(cur->mm);
			child->mm = cur->mm;
			child->pml4 = cur->pml4;
		}
	} else {
		/* COW clone of address space.  Held for writing for the same
		 * reason as the fork path in the scheduler: marking the parent
		 * copy-on-write and taking the child's reference must not be
		 * interleaved with a fault in another thread resolving the very
		 * page being marked. */
		task_t *mm_owner = task_mm_owner(cur);
		uint64_t *child_pml4;

		mm_write_lock(&mm_owner->mmap_lock);
		child_pml4 = mm_clone_address_space(cur->pml4);
		mm_write_unlock(&mm_owner->mmap_lock);
		if (!child_pml4) {
			mm_free_guarded_kstack(k_stack_mem, KERNEL_STACK_SIZE);
			mm_regions_free(child);
			kfree(child);
			return -ENOMEM;
		}
		child->pml4 = child_pml4;
		child->mm = NULL; // Use legacy fields
		/* Never inherited: the parent is executing fork, not exiting. */
		child->in_exit_teardown = false;
	}

	// Handle CLONE_FILES (share file descriptors)
	if (share_files) {
		if (cur->files) {
			files_struct_get(cur->files);
			child->files = cur->files;
		} else {
			/* First CLONE_FILES in this process: promote the
			 * caller's private table to a shared one.  The
			 * descriptors are MOVED, not duplicated — the shared
			 * table becomes the single owner of each reference, so
			 * a close() by any thread really closes the object and
			 * the reader on the other end of a pipe sees EOF.  (The
			 * previous code duplicated every reference into a table
			 * no lookup ever consulted; those copies were only
			 * released when the last thread exited, so nothing a
			 * threaded process closed was ever truly closed.) */
			cur->files = files_struct_create();
			if (!cur->files) {
				if (!share_vm && child->pml4) {
					mm_destroy_address_space(child->pml4);
				}
				mm_free_guarded_kstack(k_stack_mem,
						       KERNEL_STACK_SIZE);
				mm_regions_free(child);
				kfree(child);
				return -ENOMEM;
			}
			for (int i = 0; i < TASK_MAX_FDS; i++) {
				cur->files->fd_table[i] = cur->fd_table[i];
				/* Carry the close-on-exec bits over with the
				 * descriptors: task_get/set_fd_flags switch to
				 * the shared array the moment ->files is set,
				 * so without this every FD_CLOEXEC bit in the
				 * process silently vanished at the first
				 * pthread_create. */
				cur->files->fd_flags[i] = cur->fd_flags[i];
				cur->fd_table[i] = NULL;
				cur->fd_flags[i] = 0;
			}

			files_struct_get(cur->files);
			child->files = cur->files;
		}
		/* The wholesale task copy above duplicated the caller's private
		 * table into the child; it is not the child's to own — the
		 * shared files_struct holds every reference now. */
		for (int i = 0; i < TASK_MAX_FDS; i++) {
			child->fd_table[i] = NULL;
			child->fd_flags[i] = 0;
		}
	} else {
		/* A private table of the caller's descriptors, with a reference
		 * taken for each.  See fd_table_clone(): the read and the
		 * reference must be one locked step, and the pipe ends have to
		 * be duplicated after the lock is dropped. */
		child->files = NULL;
		fd_table_clone(child, cur);
	}

	// Handle CLONE_SIGHAND (share signal handlers)
	if (share_sighand) {
		if (cur->sighand) {
			sighand_struct_get(cur->sighand);
			child->sighand = cur->sighand;
		} else {
			// Create sighand_struct from parent's legacy signal handlers
			cur->sighand = sighand_struct_create();
			if (!cur->sighand) {
				// Cleanup and fail
				if (share_files && child->files) {
					// files_struct_put would be called in cleanup
				}
				if (!share_vm && child->pml4) {
					mm_destroy_address_space(child->pml4);
				}
				mm_free_guarded_kstack(k_stack_mem,
						       KERNEL_STACK_SIZE);
				mm_regions_free(child);
				kfree(child);
				return -ENOMEM;
			}
			// Copy signal handlers
			for (int i = 0; i < 65; i++) {
				cur->sighand->action[i] =
					cur->signals.action[i];
			}

			sighand_struct_get(cur->sighand);
			child->sighand = cur->sighand;
		}
	} else {
		// Copy signal handlers (already done by memcpy)
		child->sighand = NULL;
		signal_fork_copy(child, cur);
	}

	// Handle CLONE_THREAD (same thread group)
	if (is_thread) {
		// Add to parent's thread group
		thread_group_add(cur->group_leader, child);
		child->exit_signal = 0; // Threads don't send exit signal
		/* A thread is nobody's child: the PROCESS is, and the process
		 * is the group leader.
		 *
		 * This used to copy cur->parent, which gave every thread a
		 * pointer to a task it was never linked under -- nothing ever
		 * added it to that parent's child list.  Two things follow, and
		 * both are fatal:
		 *
		 *   - sched_reparent_children() repoints children by walking
		 *     the child LIST, so it never saw these.  When the parent
		 *     was freed, every thread of every child process was left
		 *     holding a dangling pointer to it.
		 *   - sched_remove_task() then did
		 *     sched_remove_child(task->parent, task) on the way out,
		 *     which walks that parent's sibling list looking for a
		 *     thread that was never on it -- straight through the freed
		 *     task's poison.  RAX = 0xfeedfacefeedface in
		 *     sched_remove_child, under a spinlock with interrupts off,
		 *     so the processor wedges holding the lock and the whole
		 *     machine halts.  Seen on every session teardown, where
		 *     parents die while other processes still have live threads.
		 *
		 * NULL is the honest value.  Anything that wants the process's
		 * parent asks the group leader (see sched_get_ppid). */
		child->parent = NULL;
	} else {
		/* New process (new thread group).
		 *
		 * The child belongs to the forking PROCESS, not to the thread
		 * that happened to call fork().  Hanging it off `cur' put a
		 * child forked by a worker thread on that thread's own child
		 * list, while sys_waitpid() looks for children on the thread
		 * group LEADER's list -- so nothing in the process could ever
		 * wait for it: waitpid() answered ECHILD and the child stayed a
		 * zombie for good.  It also made getppid() in the child report
		 * the forking thread's tid instead of the parent's pid.
		 *
		 * Any threaded program that spawns from a worker thread hits
		 * this, which is most of them -- GLib's g_spawn family forks
		 * and then waits for the intermediate child. */
		task_t *proc = cur->group_leader ? cur->group_leader : cur;

		thread_group_init(child);
		child->exit_signal = SIGCHLD;
		child->parent = proc;
		sched_add_child(proc, child);
	}

	// Handle CLONE_SETTLS
	if (set_tls) {
		task_set_fs_base(child, tls);
	} else {
		child->fs_base = 0;
	}

	// Handle CLONE_CHILD_CLEARTID
	if (clear_child_tid && child_tidptr) {
		if (validate_user_ptr(child_tidptr, sizeof(int))) {
			child->clear_child_tid = (uint64_t *)child_tidptr;
		}
	} else {
		child->clear_child_tid = NULL;
	}

	// Handle CLONE_CHILD_SETTID
	if (set_child_tid && child_tidptr) {
		if (validate_user_ptr(child_tidptr, sizeof(int))) {
			child->set_child_tid = (uint64_t *)child_tidptr;
			// This will be written when child starts
		}
	} else {
		child->set_child_tid = NULL;
	}

	// Handle CLONE_PARENT_SETTID
	if (set_parent_tid && parent_tidptr) {
		if (validate_user_ptr(parent_tidptr, sizeof(int))) {
			smap_disable();
			*(int *)parent_tidptr = child->id;
			smap_enable();
		}
	}

	// Clear robust list (not inherited)
	child->robust_list = NULL;
	child->robust_list_len = 0;

	/* The region table was given to the child by mm_regions_clone() right
	 * after the task_t copy, which is the only place it can be done. */

	// Assign child to parent's CPU (same rationale as sched_fork_current)
	child->on_cpu = cur->on_cpu;

	// Set up child's kernel stack to return to userspace
	uint64_t user_rip = cur->syscall_rip;
	uint64_t user_rsp = child_stack ? child_stack : cur->syscall_rsp;
	uint64_t user_rflags = cur->syscall_rflags;

	uint64_t *k_sp = (uint64_t *)child->kernel_stack_top;
	k_sp = (uint64_t *)((uint64_t)k_sp & ~0xFUL);

	// Push user callee-saved registers
	*(--k_sp) = cur->syscall_r15;
	*(--k_sp) = cur->syscall_r14;
	*(--k_sp) = cur->syscall_r13;
	*(--k_sp) = cur->syscall_r12;
	*(--k_sp) = cur->syscall_rbx;
	*(--k_sp) = cur->syscall_rbp;

	// IRET frame
	*(--k_sp) = 0x1B; // SS
	*(--k_sp) = user_rsp; // RSP
	*(--k_sp) = user_rflags | 0x200; // RFLAGS with IF
	*(--k_sp) = 0x23; // CS
	*(--k_sp) = user_rip; // RIP

	// Return value for child (0 or TID depending on CLONE_CHILD_SETTID)
	*(--k_sp) = 0; // RAX = 0 for child

	// Kernel callee-saved for context switch
	*(--k_sp) = (uint64_t)fork_child_return;
	// RFLAGS restored by ctx_switch_asm's popfq; IF set (see sys_fork).
	*(--k_sp) = 0x202; // RFLAGS (kernel): reserved bit 1 + IF
	*(--k_sp) = 0; // RBP
	*(--k_sp) = 0; // RBX
	*(--k_sp) = 0; // R12
	*(--k_sp) = 0; // R13
	*(--k_sp) = 0; // R14
	*(--k_sp) = 0; // R15

	child->sp = k_sp;

	// Add to global task list
	spin_lock_irqsave(&g_task_list_lock, &irq_flags);
	extern void task_list_add(task_t * t);
	task_list_add(child);
	spin_unlock_irqrestore(&g_task_list_lock, irq_flags);

	// Handle CLONE_CHILD_SETTID: write TID to child's address space
	// This must be done after child is set up but before scheduling
	if (set_child_tid && child->set_child_tid) {
		smap_disable();
		*(int *)(child->set_child_tid) = child->id;
		smap_enable();
	}

	// IMPORTANT: Save child->id BEFORE enqueueing!
	// Once enqueued, another CPU might run and free the child before we read it.
	int64_t child_pid = child->id;

	/* Tell the tracer a thread appeared, if it asked.
	 *
	 * A debugger that is not told has to notice by polling the thread list,
	 * so a thread that is created and does something interesting between
	 * two stops is never announced -- and a breakpoint it hits before the
	 * next refresh is reported against a thread nothing knows about.  The
	 * new thread's id travels as the event's value, which is what
	 * PTRACE_GETEVENTMSG returns.
	 *
	 * Reported before the thread is released, so a tracer that wants to set
	 * something up on it can do so before it runs.  Opt-in, like every
	 * other event: PTRACE_O_TRACECLONE. */
	bool report_clone = (cur->tracer_pid != 0 &&
			     (cur->ptrace_options & PTRACE_O_TRACECLONE));
	uint64_t child_incarnation = child->incarnation;

	/* A traced thread must not outrun its own announcement.
	 *
	 * Enqueueing first let the new thread run, finish, and raise its EXIT
	 * event on another CPU before the tracer had collected the CLONE event
	 * that names it -- the two events were then pending at once, and when
	 * the exit surfaced first, the tracer met a thread id it had never
	 * been told about and the exit report was effectively lost.  A
	 * short-lived thread under a debugger hit this about once in a hundred
	 * runs.
	 *
	 * So a REPORTED thread is born parked, in the same silent state the
	 * group stop uses (stopped, no event of its own -- the CLONE event on
	 * its creator is what names it), and the machinery that already exists
	 * releases it: the tracer's resume of the process runs
	 * task_ptrace_group_resume(), which frees silent parks and only them.
	 * The tracer therefore always learns the id before the thread has
	 * executed an instruction, which is also what lets it plant
	 * breakpoints in the thread before it starts.  An unreported thread is
	 * released immediately, as before. */
	if (report_clone) {
		child->ptrace_stopped = 1;
		child->ptrace_stop_signo = 0;
		child->state = TASK_STOPPED;
	} else {
		sched_enqueue_ready(child);
	}

	if (report_clone) {
		task_ptrace_stop(cur, SIGTRAP, PTRACE_EVENT_CLONE,
				 (unsigned long)child_pid);

		/* The stop can be REFUSED -- the tracer vanished between the
		 * report_clone check and the stop's own tracer resolution,
		 * which lazily clears our link.  Then nobody will ever
		 * group-resume the parked child: release it here.
		 *
		 * Through a fresh lookup, never through the pointer: in the
		 * NORMAL path the child was released while we were parked and
		 * may have run, exited and been freed already.  Id plus
		 * incarnation says whether the task in the slot is still the
		 * one we made; still silently parked says nobody released it. */
		if (cur->tracer_pid == 0) {
			uint64_t lf;
			task_t *c;

			spin_lock_irqsave(&g_task_list_lock, &lf);
			c = sched_find_task_by_id_locked((uint32_t)child_pid);
			if (c && c->incarnation == child_incarnation &&
			    c->ptrace_stopped && c->ptrace_stop_signo == 0 &&
			    c->state == TASK_STOPPED) {
				c->ptrace_stopped = 0;
				c->tracer_pid = 0;
			} else {
				c = NULL;
			}
			spin_unlock_irqrestore(&g_task_list_lock, lf);
			if (c && sched_claim_wake(c, TASK_STOPPED))
				sched_enqueue_ready(c);
		}
	}

	// Set need_resched so the parent yields at the next opportunity,
	// giving the newly created thread a chance to start promptly.
	// This is the standard behavior: thread creation is a reschedule point.
	cur->need_resched = 1;

	return child_pid;
}

// SYS_VFORK - create child that shares parent's memory until exec/exit
int64_t sys_vfork(void)
{
	// For now, implement as regular fork
	// True vfork semantics would suspend parent until child execs or exits
	return sys_fork();
}
