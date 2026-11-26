/* Kernel side of the epoll host test: the services kernel/net/eventpoll.c
 * uses -- the descriptor table, objects with wait queues, the task and its
 * sleep, the clock, the allocator -- and the test's entry points (ep-api.h).
 *
 * Compiled freestanding with the kernel headers and host/net/prelude.h, like
 * the stack under host/test-net.sh: spinlocks are checked (taking one twice
 * is a fatal deadlock, sleeping with one held is fatal).  The wait-queue
 * walker is the kernel's own (kernel/ke/waitq.h).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/net.h>
#include <kernel/net/eventpoll.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/syscalls.h>
#include <kernel/mm/memory.h>
#include <kernel/fs/file.h>
#include <kernel/ke/syscall.h>
#include "ep-api.h"

uint64_t test_irq_flag = 0x200;
int test_spins_held;
uint64_t test_trylock_fails;

static uint64_t clock_ns = 1000000000ULL;
static uint64_t wait_deadline_ns;
static int sig_pending;
static uint64_t total_memory = 1ULL << 30;
static uint64_t wakeups;
static int warnings;
static int objects_live;

static task_t task;

void test_fatal(const char *what, const char *name)
{
	(void)name;
	hx_fatal(what);
}

void test_fatalf(const char *fmt, ...)
{
	hx_fatal(fmt);
}

void test_bug(const char *cond, const char *file, int line, const char *func)
{
	(void)file;
	(void)line;
	(void)func;
	hx_fatal(cond ? cond : "BUG");
}

void bug_report_atomic_sleep(const char *file, int line, const char *func)
{
	(void)file;
	(void)line;
	(void)func;
	hx_fatal("sleep in atomic context");
}

unsigned long long hs_now_ns(void)
{
	return clock_ns;
}

int kprintf(const char *fmt, ...)
{
	__builtin_va_list ap;

	if (fmt[0] == 'W' || fmt[0] == 'B')
		warnings++;
	__builtin_va_start(ap, fmt);
	hx_log(fmt, ap);
	__builtin_va_end(ap);
	return 0;
}

void *kalloc(size_t n)
{
	return hx_malloc(n);
}

void kfree(void *p)
{
	hx_free(p);
}

void mm_memset(void *p, int c, size_t n)
{
	unsigned char *b = p;

	for (size_t i = 0; i < n; i++)
		b[i] = (unsigned char)c;
}

void mm_get_memory_stats(memory_stats_t *st)
{
	st->total_memory = total_memory;
}

task_t *sched_current(void)
{
	return &task;
}

int signal_pending(struct task *t)
{
	(void)t;
	return sig_pending;
}

int capable(void)
{
	return 1;
}

int sched_claim_wake(task_t *t, task_state_t from)
{
	task_state_t exp = from;

	return __atomic_compare_exchange_n(&t->state, &exp, TASK_READY, 0,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

void sched_enqueue_ready(task_t *t)
{
	if (t != &task)
		hx_fatal("enqueue of an unknown task");
	if (t->state != TASK_READY)
		hx_fatal("enqueue of a task that was not claimed");
	wakeups++;
}

void sched_schedule(void)
{
	if (test_spins_held)
		hx_fatal("schedule with a spinlock held");
	if (!test_irq_flag)
		hx_fatal("schedule with interrupts off");
	if (task.state == TASK_BLOCKED)
		hx_sleep_hook();
	if (task.state == TASK_BLOCKED) {
		/* Nothing woke it: the timer does, at the deadline. */
		if (!wait_deadline_ns)
			hx_fatal("epoll_wait sleeps for ever: nothing wakes it");
		if (clock_ns < wait_deadline_ns)
			clock_ns = wait_deadline_ns;
		if (!sched_claim_wake(&task, TASK_BLOCKED))
			hx_fatal("timer could not claim the task");
	}
	task.state = TASK_RUNNING;
}

void hrtimer_wait_arm(struct task *t, uint64_t abs_ns)
{
	(void)t;
	wait_deadline_ns = abs_ns;
}

void hrtimer_wait_disarm(struct task *t)
{
	(void)t;
	wait_deadline_ns = 0;
}

uint64_t hrtimer_now_ns(void)
{
	return clock_ns;
}

uint64_t timer_ticks(void)
{
	return clock_ns / 5000000ULL;
}

uint64_t timer_ns_to_ticks(uint64_t ns)
{
	return ns / 5000000ULL;
}

bool validate_user_ptr(uint64_t ptr, size_t len)
{
	(void)len;
	return ptr != 0 && ptr != 0xdead0000ULL;
}

int copy_to_user(void *dst, const void *src, size_t n)
{
	if ((uintptr_t)dst == 0xdead0000ULL)
		return -1;
	__builtin_memcpy(dst, src, n);
	return 0;
}

int copy_from_user(void *dst, const void *src, size_t n)
{
	__builtin_memcpy(dst, src, n);
	return 0;
}

int poll_sigmask_install(uint64_t umask_ptr, kernel_sigset_t *saved)
{
	(void)umask_ptr;
	(void)saved;
	return 0;
}

void mutex_init(struct mutex *m, const char *name)
{
	m->locked = 0;
	m->name = name;
}

void mutex_lock(struct mutex *m)
{
	if (m->locked)
		hx_fatal("mutex taken twice");
	if (test_spins_held)
		hx_fatal("mutex_lock with a spinlock held");
	m->locked = 1;
}

void mutex_unlock(struct mutex *m)
{
	if (!m->locked)
		hx_fatal("mutex_unlock of a free mutex");
	m->locked = 0;
}

void mutex_destroy(struct mutex *m)
{
	if (m->locked)
		hx_fatal("mutex destroyed while held");
}

/* ---------------------------------------------------------------------------
 * Objects
 * ------------------------------------------------------------------------ */

#define KOBJ_MAGIC 0x4b4f424a4b4f424aULL

struct kobj {
	uint64_t magic;
	int kind;
	int refs; /* KX_OBJ/KX_PLAIN: descriptors + lookups */
	int descs; /* KX_UNIXLIKE: descriptors */
	int memrefs; /* KX_UNIXLIKE: memory (descriptors, lookups, items) */
	unsigned mask;
	int close_fd_on_poll; /* >= 0: closes this descriptor when asked */
	struct wait_queue_head wq;
};

static int is_kobj(void *e)
{
	return (uintptr_t)e > 0x100000 && !IS_EPOLL_FD(e) &&
	       ((struct kobj *)e)->magic == KOBJ_MAGIC;
}

static void kobj_free(struct kobj *o)
{
	if (o->wq.first)
		hx_fatal("object freed with an entry still on its queue");
	o->magic = 0;
	objects_live--;
	hx_free(o);
}

static void kobj_put(struct kobj *o)
{
	if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) > 0)
		return;
	/* The last reference: epoll items go first, as in vfs_close(). */
	eventpoll_release(o);
	kobj_free(o);
}

static void kobj_memput(struct kobj *o)
{
	if (--o->memrefs == 0)
		kobj_free(o);
}

vfs_file_t *fdget(task_t *t, int fd)
{
	void *e;

	if (fd < 0 || fd >= TASK_MAX_FDS)
		return NULL;
	e = task_fds(t)[fd];
	if (e && !fdhold(e))
		e = NULL;
	return e;
}

int fdhold(vfs_file_t *entry)
{
	void *e = entry;

	if (IS_EPOLL_FD(e))
		return epoll_tryget(EPOLL_FD_IDX(e));
	if (!is_kobj(e))
		hx_fatal("fdhold of a non-object");
	if (((struct kobj *)e)->kind == KX_UNIXLIKE)
		((struct kobj *)e)->memrefs++;
	else
		((struct kobj *)e)->refs++;
	return 1;
}

void fdput(vfs_file_t *entry)
{
	void *e = entry;

	if (!e)
		return;
	if (IS_EPOLL_FD(e)) {
		epoll_put(EPOLL_FD_IDX(e));
		return;
	}
	if (!is_kobj(e))
		hx_fatal("fdput of a non-object");
	if (((struct kobj *)e)->kind == KX_UNIXLIKE)
		kobj_memput(e);
	else
		kobj_put(e);
}

int fd_install_from(task_t *t, vfs_file_t *file, int from)
{
	for (int i = from; i < TASK_MAX_FDS; i++) {
		if (!task_fds(t)[i]) {
			task_fds(t)[i] = file;
			task_set_fd_flags(t, (unsigned)i, 0);
			return i;
		}
	}
	return -EMFILE;
}

void poll_wait(struct poll_table *pt, void *owner, struct wait_queue_head *h)
{
	if (!pt || !h)
		return;
	if (pt->qproc)
		pt->qproc(pt, owner, h);
}

short poll_entry(void *entry, int fd, short events, struct poll_table *pt,
		 int depth)
{
	struct kobj *o = entry;

	(void)fd;
	if (IS_EPOLL_FD(entry))
		return ep_eventpoll_poll(EPOLL_FD_IDX(entry), events, pt,
					 depth);
	if (!is_kobj(entry))
		hx_fatal("poll of a freed or unknown object");
	if (o->kind == KX_PLAIN) {
		if (pt)
			pt->nopoll = 1;
		return (short)(events & (POLLIN | POLLOUT));
	}
	poll_wait(pt, entry, &o->wq);
	if (!pt && o->close_fd_on_poll >= 0) {
		/* A sibling thread closing the descriptor while an epoll_wait
		 * looks at the object: the item is then removed while it is
		 * pinned, and freed by the epoll_wait. */
		int cfd = o->close_fd_on_poll;

		o->close_fd_on_poll = -1;
		kx_close(cfd);
	}
	return (short)(o->mask &
		       ((unsigned)(unsigned short)events | POLLERR | POLLHUP));
}

int poll_obj_tryhold(void *entry)
{
	struct kobj *o = entry;
	int old;

	if (IS_EPOLL_FD(entry))
		return epoll_tryget(EPOLL_FD_IDX(entry));
	if (!is_kobj(entry))
		hx_fatal("tryhold of a freed object");
	if (o->kind == KX_UNIXLIKE)
		return o->descs > 0;
	old = o->refs;
	while (old > 0)
		if (__atomic_compare_exchange_n(&o->refs, &old, old + 1, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			return 1;
	return 0;
}

void poll_obj_put(void *entry)
{
	if (IS_EPOLL_FD(entry)) {
		epoll_put(EPOLL_FD_IDX(entry));
		return;
	}
	if (((struct kobj *)entry)->kind == KX_UNIXLIKE)
		return;
	kobj_put(entry);
}

int poll_obj_item_hold(void *entry)
{
	if (is_kobj(entry) && ((struct kobj *)entry)->kind == KX_UNIXLIKE) {
		((struct kobj *)entry)->memrefs++;
		return 2;
	}
	return 1;
}

void poll_obj_item_put(void *entry)
{
	if (is_kobj(entry) && ((struct kobj *)entry)->kind == KX_UNIXLIKE)
		kobj_memput(entry);
}

int poll_obj_closed(void *entry)
{
	return is_kobj(entry) && ((struct kobj *)entry)->kind == KX_UNIXLIKE &&
	       ((struct kobj *)entry)->descs <= 0;
}

/* ---------------------------------------------------------------------------
 * The test's entry points
 * ------------------------------------------------------------------------ */

static void task_init(void)
{
	if (task.id)
		return;
	task.id = 1;
	task.state = TASK_RUNNING;
	for (int i = 0; i < 3; i++)
		task.fd_flags[i] = FD_STDIO_CLOSED;
}

int kx_open(int kind, unsigned mask)
{
	struct kobj *o = hx_malloc(sizeof(*o));
	int fd;

	task_init();
	mm_memset(o, 0, sizeof(*o));
	o->magic = KOBJ_MAGIC;
	o->kind = kind;
	o->mask = mask;
	o->refs = 1;
	o->descs = 1;
	o->memrefs = 1;
	o->close_fd_on_poll = -1;
	wq_head_init(&o->wq, "kobj");
	objects_live++;
	fd = fd_install_from(&task, (vfs_file_t *)o, 3);
	if (fd < 0)
		hx_fatal("fd table full");
	return fd;
}

int kx_dup(int fd)
{
	void *e = task_fds(&task)[fd];
	int nfd;

	if (!e)
		return -EBADF;
	if (IS_EPOLL_FD(e)) {
		epoll_get(EPOLL_FD_IDX(e));
	} else {
		struct kobj *o = e;

		if (o->kind == KX_UNIXLIKE) {
			o->descs++;
			o->memrefs++;
		} else {
			o->refs++;
		}
	}
	nfd = fd_install_from(&task, e, 3);
	return nfd;
}

int kx_close(int fd)
{
	void *e;

	if (fd < 0 || fd >= TASK_MAX_FDS || !(e = task_fds(&task)[fd]))
		return -EBADF;
	task_fds(&task)[fd] = NULL;
	if (IS_EPOLL_FD(e)) {
		epoll_put(EPOLL_FD_IDX(e));
		return 0;
	}
	if (((struct kobj *)e)->kind == KX_UNIXLIKE) {
		struct kobj *o = e;

		if (--o->descs == 0) {
			/* hung up: the closing end's queue is woken */
			o->mask |= POLLHUP;
			wq_wake_poll(&o->wq, 0);
		}
		kobj_memput(o);
		return 0;
	}
	kobj_put(e);
	return 0;
}

static struct kobj *fd_obj(int fd)
{
	void *e = (fd >= 0 && fd < TASK_MAX_FDS) ? task_fds(&task)[fd] : NULL;

	if (!e || !is_kobj(e))
		hx_fatal("not an object descriptor");
	return e;
}

int kx_set_ready(int fd, unsigned mask, int wake)
{
	struct kobj *o = fd_obj(fd);

	o->mask = mask;
	if (wake)
		wq_wake_poll(&o->wq, 0);
	return 0;
}

int kx_close_on_poll(int fd)
{
	fd_obj(fd)->close_fd_on_poll = fd;
	return 0;
}

int kx_free_queue(int fd)
{
	eventpoll_wq_free(&fd_obj(fd)->wq);
	return 0;
}

int kx_wake(int fd, unsigned key)
{
	wq_wake_poll(&fd_obj(fd)->wq, key);
	return 0;
}

int kx_nwaiters(int fd)
{
	int n = 0;

	for (struct wait_queue_entry *e = fd_obj(fd)->wq.first; e; e = e->next)
		n++;
	return n;
}

int kx_objects_live(void)
{
	return objects_live;
}

int kx_fd_object_refs(int fd)
{
	return fd_obj(fd)->refs;
}

long kx_epoll_create(int size)
{
	task_init();
	return sys_epoll_create((uint64_t)(int64_t)size);
}

long kx_epoll_create1(int flags)
{
	task_init();
	return sys_epoll_create1((uint64_t)(int64_t)flags);
}

long kx_epoll_ctl(int epfd, int op, int fd, unsigned events,
		  unsigned long long data)
{
	struct epoll_event ev;

	ev.events = events;
	ev.data.u64 = data;
	return sys_epoll_ctl((uint64_t)(int64_t)epfd, (uint64_t)op,
			     (uint64_t)(int64_t)fd, (uint64_t)(uintptr_t)&ev);
}

long kx_epoll_ctl_null(int epfd, int op, int fd)
{
	return sys_epoll_ctl((uint64_t)(int64_t)epfd, (uint64_t)op,
			     (uint64_t)(int64_t)fd, 0);
}

long kx_epoll_wait(int epfd, void *events, int maxevents, int timeout_ms)
{
	long r = sys_epoll_wait((uint64_t)(int64_t)epfd,
				(uint64_t)(uintptr_t)events,
				(uint64_t)(int64_t)maxevents,
				(uint64_t)(int64_t)timeout_ms);
	if (task.state != TASK_RUNNING)
		hx_fatal("epoll_wait returned with the task not running");
	if (test_spins_held || !test_irq_flag)
		hx_fatal("epoll_wait returned with a lock held");
	return r;
}

long kx_epoll_wait_bad_buffer(int epfd, int maxevents)
{
	return sys_epoll_wait((uint64_t)(int64_t)epfd, 0xdead0000ULL,
			      (uint64_t)(int64_t)maxevents, 0);
}

unsigned kx_poll_fd(int fd, unsigned events)
{
	void *e = fdget(&task, fd);
	short r;

	if (!e)
		return POLLNVAL;
	r = poll_entry(e, fd, (short)events, NULL, 0);
	fdput(e);
	return (unsigned)(unsigned short)r;
}

void kx_set_signal_pending(int on)
{
	sig_pending = on;
}

void kx_set_total_memory(unsigned long long bytes)
{
	total_memory = bytes;
}

unsigned long long kx_now_ns(void)
{
	return clock_ns;
}

void kx_advance_ns(unsigned long long ns)
{
	clock_ns += ns;
}

int kx_task_blocked(void)
{
	return task.state == TASK_BLOCKED;
}

unsigned long long kx_wakeups(void)
{
	return wakeups;
}

int kx_warnings(void)
{
	return warnings;
}

void kx_reset(void)
{
	task_init();
	for (int i = 3; i < TASK_MAX_FDS; i++)
		if (task_fds(&task)[i])
			kx_close(i);
	if (objects_live)
		hx_fatal("objects still allocated after every fd closed");
	sig_pending = 0;
}
