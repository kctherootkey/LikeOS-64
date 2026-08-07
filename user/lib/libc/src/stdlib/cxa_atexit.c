/*
 * LikeOS-64 cxa_atexit.c - Exit-time and thread-exit-time handler registration
 *
 * One list serves atexit(3), __cxa_atexit and __cxa_thread_atexit_impl, because
 * they differ only in what they record, not in when they run.  atexit() is the
 * degenerate case: a handler taking no argument and belonging to no particular
 * shared object.
 *
 * Why this is not simply a table of function pointers:
 *
 *   Every C++ object with static storage duration registers its destructor
 *   through __cxa_atexit, tagged with the __dso_handle of the object it lives
 *   in.  That tag is what makes dlclose() safe: the loader runs the closing
 *   object's .fini_array, which calls __cxa_finalize(__dso_handle), which runs
 *   and REMOVES exactly that object's destructors.  Without it they would stay
 *   on the list and be called at exit -- through a function pointer into pages
 *   that were unmapped, which faults in a way that names neither the library
 *   nor the destructor.
 *
 *   The list also has to grow.  A fixed table is fine for atexit(3), whose
 *   minimum is 32 entries, but a C++ program registers one entry per static
 *   object across every translation unit of every library it loads.
 *
 * The first block is static, so registration works before the allocator does --
 * a constructor running out of .init_array can register a destructor before
 * main(), and failing there would be silent.
 */

#include <stdlib.h>
#include <stddef.h>

/* Where a handler came from and how to call it. */
struct exit_handler {
	void (*fn)(void *);
	void *arg;
	void *dso;
};

#define EXIT_BLOCK_SIZE 32

struct exit_block {
	struct exit_block *next;
	int count;
	struct exit_handler h[EXIT_BLOCK_SIZE];
};

static struct exit_block exit_first;
static struct exit_block *exit_head = &exit_first;

/*
 * Registration can come from any thread, and finalisation walks the same list,
 * so both are serialised.  A spin lock rather than a pthread mutex: this runs
 * before and after the threading machinery is usable -- from a constructor
 * before main, and from exit() after other threads may already be gone -- and
 * must not depend on it.
 */
static volatile int exit_lock;

static void exit_lock_acquire(void)
{
	while (__atomic_exchange_n(&exit_lock, 1, __ATOMIC_ACQUIRE))
		__builtin_ia32_pause();
}

static void exit_lock_release(void)
{
	__atomic_store_n(&exit_lock, 0, __ATOMIC_RELEASE);
}

/*
 * Register `fn(arg)` to run at exit, on behalf of the shared object `dso`.
 *
 * Returns 0, or -1 if no memory could be found for the entry -- which the
 * caller must check: a C++ static destructor that is not registered simply
 * never runs, and nothing else reports it.
 */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
	struct exit_block *b;

	if (!fn)
		return -1;

	exit_lock_acquire();
	b = exit_head;
	if (b->count >= EXIT_BLOCK_SIZE) {
		struct exit_block *nb;

		/* Dropped while allocating: malloc may take its own locks, and
		 * an allocator that calls back into registration would deadlock
		 * against a lock we still held. */
		exit_lock_release();
		nb = malloc(sizeof(*nb));
		if (!nb)
			return -1;
		nb->count = 0;
		exit_lock_acquire();
		/* Another thread may have added a block in the meantime; only
		 * one of them becomes the head and the loser is given back. */
		if (exit_head->count >= EXIT_BLOCK_SIZE) {
			nb->next = exit_head;
			exit_head = nb;
			b = nb;
		} else {
			exit_lock_release();
			free(nb);
			exit_lock_acquire();
			b = exit_head;
		}
	}
	b->h[b->count].fn = fn;
	b->h[b->count].arg = arg;
	b->h[b->count].dso = dso;
	b->count++;
	exit_lock_release();
	return 0;
}

/* atexit(3) in terms of the above: a handler that ignores its argument. */
static void atexit_thunk(void *fn)
{
	((void (*)(void))fn)();
}

int atexit(void (*func)(void))
{
	if (!func)
		return -1;
	return __cxa_atexit(atexit_thunk, (void *)func, NULL);
}

int at_quick_exit(void (*func)(void))
{
	return atexit(func);
}

/*
 * Run every handler belonging to `dso`, most recently registered first, and
 * remove them.  A NULL `dso` means all of them, which is what exit() wants.
 *
 * Each entry is cleared BEFORE it is called.  A handler is entitled to call
 * exit() itself, and to register further handlers while running; clearing first
 * is what stops the same destructor running twice in either case.  The list is
 * re-scanned from the head each time round for the same reason: it can have
 * grown since the last call.
 */
void __cxa_finalize(void *dso)
{
	for (;;) {
		void (*fn)(void *) = NULL;
		void *arg = NULL;

		exit_lock_acquire();
		for (struct exit_block *b = exit_head; b && !fn; b = b->next) {
			for (int i = b->count - 1; i >= 0; i--) {
				struct exit_handler *h = &b->h[i];

				if (!h->fn)
					continue;
				if (dso && h->dso != dso)
					continue;
				fn = h->fn;
				arg = h->arg;
				h->fn = NULL;
				break;
			}
		}
		exit_lock_release();

		if (!fn)
			return;
		fn(arg);
	}
}

/*
 * The thread-local counterpart, for `thread_local` objects with destructors.
 *
 * These belong to one thread and must run when THAT thread ends, not at exit:
 * the storage they refer to is the thread's own and is gone once it has been
 * reaped.  So the list is per-thread and needs no locking -- only its owner
 * ever touches it -- and the entries are chained rather than blocked, because a
 * thread registering any at all is the exception.
 */
struct thread_handler {
	void (*fn)(void *);
	void *arg;
	void *dso;
	struct thread_handler *next;
};

static __thread struct thread_handler *thread_handlers;

int __cxa_thread_atexit_impl(void (*fn)(void *), void *arg, void *dso)
{
	struct thread_handler *t;

	if (!fn)
		return -1;
	t = malloc(sizeof(*t));
	if (!t)
		return -1;
	t->fn = fn;
	t->arg = arg;
	t->dso = dso;
	t->next = thread_handlers;
	thread_handlers = t;
	return 0;
}

/*
 * Called from the thread exit path, once per thread, before its stack and
 * control block are released.  Unlinks each entry before calling it, so a
 * destructor that ends the thread again finds an empty list rather than
 * running its neighbours a second time.
 */
void __libc_thread_finalize(void)
{
	while (thread_handlers) {
		struct thread_handler *t = thread_handlers;

		thread_handlers = t->next;
		t->fn(t->arg);
		free(t);
	}
}
