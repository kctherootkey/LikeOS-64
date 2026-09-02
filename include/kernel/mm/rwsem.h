// LikeOS-64 — address-space read/write semaphore
//
// The lock that makes address-space changes safe against page faults.
//
// Every process has one (on its thread-group leader; see task_mm_owner()).
// It is held for READING by code that only walks the address space, and for
// WRITING by code that changes its shape:
//
//   read   mm_handle_demand_fault(), mm_handle_cow_fault(),
//          mm_prefault_user_range()
//   write  sys_mmap, sys_munmap, sys_mprotect, sys_brk, shmat/shmdt,
//          fork's clone of the parent, exec's teardown, address-space destroy
//
// This is a SLEEPING lock: it may only be taken from process context with
// interrupts enabled.  Both acquire paths call might_sleep(), so a DEBUG build
// names any caller that gets that wrong instead of deadlocking silently.
//
// The type itself is declared in <kernel/ke/sched.h>, next to spinlock_t,
// because task_t embeds it.

#ifndef _KERNEL_MM_RWSEM_H_
#define _KERNEL_MM_RWSEM_H_

#include <kernel/ke/sched.h>

void mm_rwsem_init(mm_rwsem_t *sem, const char *name);

/* Shared acquisition.  Recursive: a task that already holds this semaphore
 * exclusively is granted a depth increment instead, and a task that already
 * holds it shared bypasses writer-preference (see task->mm_rdepth). */
void mm_read_lock(mm_rwsem_t *sem);
void mm_read_unlock(mm_rwsem_t *sem);
/* One attempt, never parks.  For callers that may not sleep -- see the page
 * fault paths, which use it to decide whether they may proceed at all. */
bool mm_read_trylock(mm_rwsem_t *sem);

/* Exclusive acquisition.  Recursive for the owning task. */
void mm_write_lock(mm_rwsem_t *sem);
void mm_write_unlock(mm_rwsem_t *sem);

/* Predicates for lockdep_assert-style checks.  Deliberately cheap and
 * advisory: they answer "is this held at all", which is what an assertion in
 * a fault path needs to catch a caller that forgot the lock entirely. */
bool mm_rwsem_is_locked(const mm_rwsem_t *sem);
bool mm_rwsem_is_write_locked(const mm_rwsem_t *sem);

/* Assert the current task holds `sem` at least for reading.  Compiles away
 * outside DEBUG builds, like lockdep_assert_held() in <kernel/uapi/bug.h>. */
#ifdef DEBUG
#define mm_assert_locked(sem)                                                 \
	do {                                                                  \
		if (unlikely(!mm_rwsem_is_locked(sem)))                       \
			kprintf("WARNING: mm_assert_locked(%s) FAILED at %s:%d %s()\n", \
				#sem, __FILE__, __LINE__, __func__);          \
	} while (0)
#define mm_assert_write_locked(sem)                                           \
	do {                                                                  \
		if (unlikely(!mm_rwsem_is_write_locked(sem)))                 \
			kprintf("WARNING: mm_assert_write_locked(%s) FAILED at %s:%d %s()\n", \
				#sem, __FILE__, __LINE__, __func__);          \
	} while (0)
#else
#define mm_assert_locked(sem)      \
	do {                       \
		(void)(sem);       \
	} while (0)
#define mm_assert_write_locked(sem) \
	do {                        \
		(void)(sem);        \
	} while (0)
#endif

#endif /* _KERNEL_MM_RWSEM_H_ */
