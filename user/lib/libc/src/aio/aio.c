/*
 * aio.c - POSIX asynchronous I/O on a pool of worker threads.
 *
 * Every request gets a record (struct aio_req) on the queue of its file
 * descriptor (struct aio_fdq).  A descriptor's queue runs strictly one
 * request at a time, in priority order, so the requests of one descriptor
 * never overtake each other and aio_fsync() follows everything queued
 * before it.  Descriptors run in parallel: a worker thread takes the head
 * request of any descriptor that is not running yet, does the I/O with the
 * ordinary system calls, records the result in the aiocb, and announces
 * the completion.  Workers are created on demand up to the limit
 * (aio_init(), default 20) and exit after sitting idle for aio_idle_time
 * seconds (default 1).  Worker threads block every signal, so a
 * notification signal goes to an application thread.
 *
 * Waiting (aio_suspend(), lio_listio(LIO_WAIT)) sleeps on a futex word,
 * aio_done_gen, that every completion increments: a waiter samples it,
 * checks its requests, and sleeps only if the word is still unchanged, so
 * a completion between the check and the sleep is never missed.  A signal
 * interrupts the sleep and aio_suspend() then fails with EINTR.
 *
 * Locking: one mutex, aio_lock, guards the queues, the request records and
 * the thread counts.  It is never held across I/O or notification.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/futex.h>
#include <time.h>
#include <unistd.h>

/* The futex helpers live in the system call layer. */
int futex_wait(volatile int *uaddr, int val, const struct timespec *timeout);
int futex_wake(volatile int *uaddr, int count);

/* Internal operation codes: the lio opcodes plus the two sync kinds. */
#define AIO_OP_READ   LIO_READ
#define AIO_OP_WRITE  LIO_WRITE
#define AIO_OP_FSYNC  16
#define AIO_OP_DSYNC  17

enum aio_state { REQ_QUEUED, REQ_RUNNING };

struct aio_fdq;
struct aio_group;

struct aio_req {
	struct aiocb *cb;
	int op;
	enum aio_state state;
	pid_t pid;                /* the submitter, target of SIGEV_SIGNAL */
	struct sigevent sev;      /* aio_sigevent, copied at submission */
	int group_last;           /* finished its batch (set under aio_lock) */
	int group_waiting;        /* the batch is a LIO_WAIT one */
	struct aio_req *next;     /* next request of the same descriptor */
	struct aio_fdq *fdq;
	struct aio_group *group;  /* lio_listio() batch, or NULL */
};

struct aio_fdq {
	int fd;
	int running;              /* the head request is being executed */
	struct aio_req *head;
	struct aio_fdq *next;     /* all descriptors with requests */
};

/* One lio_listio() call. */
struct aio_group {
	int remaining;            /* requests of the batch not finished */
	int waiting;              /* LIO_WAIT: the caller frees the record */
	struct sigevent sev;      /* LIO_NOWAIT notification */
	pid_t pid;
};

static pthread_mutex_t aio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t aio_work = PTHREAD_COND_INITIALIZER;
static struct aio_fdq *aio_fdqs;     /* descriptors with queued requests */
static int aio_nthreads;             /* worker threads alive */
static int aio_idle;                 /* of those, waiting for work */
static int aio_pending;              /* runnable descriptors not taken yet */
static int aio_max_threads = 20;
static int aio_idle_seconds = 1;
static volatile int aio_done_gen;    /* futex word: bumped per completion */
static int aio_atfork_done;

/* ---------------------------------------------------------------------- */
/* Notification                                                           */
/* ---------------------------------------------------------------------- */

struct aio_thread_note {
	void (*fn)(sigval_t);
	sigval_t value;
};

static void *aio_notify_thread(void *arg)
{
	struct aio_thread_note note = *(struct aio_thread_note *)arg;

	free(arg);
	note.fn(note.value);
	return NULL;
}

/* Deliver one sigevent; errors cannot be reported to anyone. */
static void aio_sigevent_deliver(const struct sigevent *sev, pid_t pid)
{
	if (sev->sigev_notify == SIGEV_SIGNAL) {
		if (sev->sigev_signo > 0)
			sigqueue(pid, sev->sigev_signo, sev->sigev_value);
		return;
	}
	if (sev->sigev_notify == SIGEV_THREAD && sev->sigev_notify_function) {
		struct aio_thread_note *note = malloc(sizeof(*note));
		pthread_attr_t own;
		pthread_attr_t *attr = sev->sigev_notify_attributes;
		pthread_t tid;

		if (!note)
			return;
		note->fn = sev->sigev_notify_function;
		note->value = sev->sigev_value;
		if (!attr) {
			pthread_attr_init(&own);
			pthread_attr_setdetachstate(&own, PTHREAD_CREATE_DETACHED);
			attr = &own;
		}
		if (pthread_create(&tid, attr, aio_notify_thread, note) != 0)
			free(note);
		if (attr == &own)
			pthread_attr_destroy(&own);
	}
}

/* Wake every aio_suspend()/lio_listio(LIO_WAIT) sleeper to re-check. */
static void aio_wake_waiters(void)
{
	__atomic_add_fetch(&aio_done_gen, 1, __ATOMIC_SEQ_CST);
	futex_wake(&aio_done_gen, INT32_MAX);
}

/* Account a request of `group' as finished; aio_lock held.  Returns 1 when
 * it was the last one. */
static int aio_group_put_locked(struct aio_group *group)
{
	if (!group)
		return 0;
	return --group->remaining == 0;
}

/*
 * Publish the result of a request: the return value first, then the error
 * word with release ordering, so a reader that sees the error code change
 * also sees the return value.  From this moment the application owns the
 * aiocb again (it may reuse or free it), so nothing reads it afterwards:
 * the notification uses the request's own copy of the sigevent.
 */
static void aio_publish(struct aiocb *cb, ssize_t ret, int err)
{
	cb->__req = NULL;
	cb->__return_value = ret;
	__atomic_store_n(&cb->__error_code, err, __ATOMIC_RELEASE);
}

/*
 * Announce a request that is published and detached from every queue and
 * batch (its group_last says whether it finished its batch).  Called
 * without aio_lock.  A LIO_WAIT batch belongs to its waiting caller, which
 * frees it; a LIO_NOWAIT batch is announced and freed by its last request.
 */
static void aio_announce(struct aio_req *req)
{
	struct aio_group *group = req->group;

	aio_sigevent_deliver(&req->sev, req->pid);
	if (group && req->group_last && !req->group_waiting) {
		aio_sigevent_deliver(&group->sev, group->pid);
		free(group);
	}
	aio_wake_waiters();
	free(req);
}

/* Detach a published request from its batch; aio_lock held.  Whether the
 * batch is waited for is read here, while the record is certainly alive. */
static void aio_detach_group_locked(struct aio_req *req)
{
	if (!req->group)
		return;
	req->group_waiting = req->group->waiting;
	req->group_last = aio_group_put_locked(req->group);
}

/* ---------------------------------------------------------------------- */
/* Queues                                                                 */
/* ---------------------------------------------------------------------- */

static struct aio_fdq *aio_fdq_find_locked(int fd)
{
	for (struct aio_fdq *q = aio_fdqs; q; q = q->next)
		if (q->fd == fd)
			return q;
	return NULL;
}

static void aio_fdq_unlink_locked(struct aio_fdq *q)
{
	for (struct aio_fdq **pp = &aio_fdqs; *pp; pp = &(*pp)->next) {
		if (*pp == q) {
			*pp = q->next;
			free(q);
			return;
		}
	}
}

/* Insert after every request of equal or higher priority (a smaller or
 * equal aio_reqprio), never ahead of the one that is running. */
static void aio_fdq_insert_locked(struct aio_fdq *q, struct aio_req *req)
{
	struct aio_req **pp = &q->head;

	if (q->running && *pp)
		pp = &(*pp)->next;
	while (*pp && (*pp)->cb->aio_reqprio <= req->cb->aio_reqprio)
		pp = &(*pp)->next;
	req->next = *pp;
	*pp = req;
}

/* ---------------------------------------------------------------------- */
/* Workers                                                                */
/* ---------------------------------------------------------------------- */

static ssize_t aio_do_io(struct aio_req *req, int *err)
{
	struct aiocb *cb = req->cb;
	ssize_t ret;

	do {
		switch (req->op) {
		case AIO_OP_READ:
			ret = pread(cb->aio_fildes, (void *)cb->aio_buf,
				    cb->aio_nbytes, cb->aio_offset);
			/* Pipes, FIFOs and sockets have no position: there the
			 * offset is meaningless and the plain read is meant. */
			if (ret == -1 && errno == ESPIPE)
				ret = read(cb->aio_fildes, (void *)cb->aio_buf,
					   cb->aio_nbytes);
			break;
		case AIO_OP_WRITE: {
			int fl = fcntl(cb->aio_fildes, F_GETFL);

			/* Appending descriptors append, in queue order. */
			if (fl != -1 && (fl & O_APPEND))
				ret = write(cb->aio_fildes, (const void *)cb->aio_buf,
					    cb->aio_nbytes);
			else
				ret = pwrite(cb->aio_fildes, (const void *)cb->aio_buf,
					     cb->aio_nbytes, cb->aio_offset);
			if (ret == -1 && errno == ESPIPE)
				ret = write(cb->aio_fildes, (const void *)cb->aio_buf,
					    cb->aio_nbytes);
			break;
		}
		case AIO_OP_FSYNC:
			ret = fsync(cb->aio_fildes);
			break;
		case AIO_OP_DSYNC:
			ret = fdatasync(cb->aio_fildes);
			break;
		default:
			errno = EINVAL;
			ret = -1;
			break;
		}
	} while (ret == -1 && errno == EINTR);

	*err = ret == -1 ? errno : 0;
	return ret;
}

/* The first descriptor whose head request is ready; aio_lock held. */
static struct aio_req *aio_take_locked(void)
{
	for (struct aio_fdq *q = aio_fdqs; q; q = q->next) {
		if (!q->running && q->head) {
			q->running = 1;
			q->head->state = REQ_RUNNING;
			aio_pending--;
			return q->head;
		}
	}
	return NULL;
}

static void *aio_worker(void *unused)
{
	sigset_t all;

	(void)unused;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);

	pthread_mutex_lock(&aio_lock);
	for (;;) {
		struct aio_req *req = aio_take_locked();

		if (!req) {
			struct timespec deadline;
			int rc;

			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_sec += aio_idle_seconds;
			aio_idle++;
			rc = pthread_cond_timedwait(&aio_work, &aio_lock, &deadline);
			aio_idle--;
			if (rc == ETIMEDOUT && aio_pending == 0)
				break;
			continue;
		}

		pthread_mutex_unlock(&aio_lock);
		int err;
		ssize_t ret = aio_do_io(req, &err);

		/* Published under the lock and before the batch count drops:
		 * a lio_listio() waiter that sees its batch complete sees every
		 * result, and aio_cancel() never meets a request whose aiocb
		 * the application already took back. */
		pthread_mutex_lock(&aio_lock);
		aio_publish(req->cb, ret, err);
		struct aio_fdq *q = req->fdq;

		q->head = req->next;
		q->running = 0;
		if (q->head)
			aio_pending++; /* the next one of this descriptor is runnable */
		else
			aio_fdq_unlink_locked(q);
		aio_detach_group_locked(req);
		pthread_mutex_unlock(&aio_lock);

		aio_announce(req);

		pthread_mutex_lock(&aio_lock);
	}
	aio_nthreads--;
	pthread_mutex_unlock(&aio_lock);
	return NULL;
}

/* A forked child has none of the parent's workers: start empty.  The
 * parent's request records are left behind (they belong to its threads). */
static void aio_atfork_child(void)
{
	pthread_mutex_init(&aio_lock, NULL);
	pthread_cond_init(&aio_work, NULL);
	aio_fdqs = NULL;
	aio_nthreads = 0;
	aio_idle = 0;
	aio_pending = 0;
}

/* Make sure somebody will run the queued work; aio_lock held. */
static int aio_kick_locked(void)
{
	if (aio_idle > 0) {
		pthread_cond_signal(&aio_work);
		return 0;
	}
	if (aio_nthreads < aio_max_threads) {
		pthread_attr_t attr;
		pthread_t tid;
		int rc;

		if (!aio_atfork_done) {
			pthread_atfork(NULL, NULL, aio_atfork_child);
			aio_atfork_done = 1;
		}
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		rc = pthread_create(&tid, &attr, aio_worker, NULL);
		pthread_attr_destroy(&attr);
		if (rc == 0) {
			aio_nthreads++;
			return 0;
		}
		/* With no worker at all nothing would ever run the request. */
		if (aio_nthreads == 0)
			return EAGAIN;
	}
	/* Every worker is busy: one of them picks it up when it is done. */
	return 0;
}

/* Validate and queue one request.  Returns 0 or an errno value; on
 * failure nothing was queued. */
static int aio_enqueue(struct aiocb *cb, int op, struct aio_group *group)
{
	struct aio_req *req;
	struct aio_fdq *q;
	int fl;

	if (!cb)
		return EINVAL;
	fl = fcntl(cb->aio_fildes, F_GETFL);
	if (fl == -1)
		return EBADF;
	switch (op) {
	case AIO_OP_READ:
		if ((fl & O_ACCMODE) == O_WRONLY)
			return EBADF;
		break;
	case AIO_OP_WRITE:
	case AIO_OP_FSYNC:
	case AIO_OP_DSYNC:
		if ((fl & O_ACCMODE) == O_RDONLY)
			return EBADF;
		break;
	default:
		return EINVAL;
	}
	if (cb->aio_reqprio < 0)
		return EINVAL;
	if (cb->aio_sigevent.sigev_notify != SIGEV_NONE &&
	    cb->aio_sigevent.sigev_notify != SIGEV_SIGNAL &&
	    cb->aio_sigevent.sigev_notify != SIGEV_THREAD)
		return EINVAL;
	/* SIGEV_SIGNAL is 0, so a zero-filled aiocb asks for signal 0: valid,
	 * and it delivers nothing (as kill() with signal 0 does). */
	if (cb->aio_sigevent.sigev_notify == SIGEV_SIGNAL &&
	    (cb->aio_sigevent.sigev_signo < 0 || cb->aio_sigevent.sigev_signo >= NSIG))
		return EINVAL;

	req = calloc(1, sizeof(*req));
	if (!req)
		return EAGAIN;
	req->cb = cb;
	req->op = op;
	req->state = REQ_QUEUED;
	req->pid = getpid();
	req->sev = cb->aio_sigevent;
	req->group = group;

	pthread_mutex_lock(&aio_lock);
	q = aio_fdq_find_locked(cb->aio_fildes);
	if (!q) {
		q = calloc(1, sizeof(*q));
		if (!q) {
			pthread_mutex_unlock(&aio_lock);
			free(req);
			return EAGAIN;
		}
		q->fd = cb->aio_fildes;
		q->next = aio_fdqs;
		aio_fdqs = q;
	}
	int was_idle = !q->running && !q->head;

	req->fdq = q;
	cb->__return_value = 0;
	__atomic_store_n(&cb->__error_code, EINPROGRESS, __ATOMIC_RELEASE);
	cb->__req = req;
	aio_fdq_insert_locked(q, req);
	if (was_idle)
		aio_pending++;

	int rc = aio_kick_locked();
	if (rc) {
		/* Undo: nobody can run it. */
		struct aio_req **pp = &q->head;

		while (*pp != req)
			pp = &(*pp)->next;
		*pp = req->next;
		if (was_idle)
			aio_pending--;
		if (!q->head && !q->running)
			aio_fdq_unlink_locked(q);
		cb->__req = NULL;
		pthread_mutex_unlock(&aio_lock);
		free(req);
		return rc;
	}
	pthread_mutex_unlock(&aio_lock);
	return 0;
}

static int aio_submit(struct aiocb *cb, int op)
{
	int rc = aio_enqueue(cb, op, NULL);

	if (rc) {
		if (cb) {
			cb->__return_value = -1;
			__atomic_store_n(&cb->__error_code, rc, __ATOMIC_RELEASE);
		}
		errno = rc;
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Interface                                                              */
/* ---------------------------------------------------------------------- */

int aio_read(struct aiocb *aiocbp)
{
	return aio_submit(aiocbp, AIO_OP_READ);
}

int aio_write(struct aiocb *aiocbp)
{
	return aio_submit(aiocbp, AIO_OP_WRITE);
}

int aio_fsync(int op, struct aiocb *aiocbp)
{
	if (op != O_SYNC && op != O_DSYNC) {
		errno = EINVAL;
		return -1;
	}
	return aio_submit(aiocbp, op == O_SYNC ? AIO_OP_FSYNC : AIO_OP_DSYNC);
}

int aio_error(const struct aiocb *aiocbp)
{
	return __atomic_load_n(&aiocbp->__error_code, __ATOMIC_ACQUIRE);
}

ssize_t aio_return(struct aiocb *aiocbp)
{
	if (__atomic_load_n(&aiocbp->__error_code, __ATOMIC_ACQUIRE) == EINPROGRESS) {
		errno = EINVAL;
		return -1;
	}
	return aiocbp->__return_value;
}

/* Remaining time to `end' (CLOCK_MONOTONIC); 0 when it has passed. */
static int aio_remaining(const struct timespec *end, struct timespec *left)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	left->tv_sec = end->tv_sec - now.tv_sec;
	left->tv_nsec = end->tv_nsec - now.tv_nsec;
	if (left->tv_nsec < 0) {
		left->tv_nsec += 1000000000L;
		left->tv_sec--;
	}
	return left->tv_sec > 0 || (left->tv_sec == 0 && left->tv_nsec > 0);
}

int aio_suspend(const struct aiocb *const list[], int nent,
		const struct timespec *timeout)
{
	struct timespec end, left;

	if (nent < 0 || (nent > 0 && !list)) {
		errno = EINVAL;
		return -1;
	}
	if (timeout) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
		    timeout->tv_nsec >= 1000000000L) {
			errno = EINVAL;
			return -1;
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		end.tv_sec += timeout->tv_sec;
		end.tv_nsec += timeout->tv_nsec;
		if (end.tv_nsec >= 1000000000L) {
			end.tv_nsec -= 1000000000L;
			end.tv_sec++;
		}
	}

	for (;;) {
		int gen = __atomic_load_n(&aio_done_gen, __ATOMIC_ACQUIRE);
		int any = 0;

		for (int i = 0; i < nent; i++) {
			if (!list[i])
				continue;
			any = 1;
			if (aio_error(list[i]) != EINPROGRESS)
				return 0;
		}
		/* No request named at all: nothing is in progress. */
		if (!any)
			return 0;
		if (timeout && !aio_remaining(&end, &left)) {
			errno = EAGAIN;
			return -1;
		}
		if (futex_wait(&aio_done_gen, gen, timeout ? &left : NULL) == -1) {
			if (errno == EINTR)
				return -1;
			/* EAGAIN: the word moved; ETIMEDOUT: re-checked above. */
		}
	}
}

int aio_cancel(int fildes, struct aiocb *aiocbp)
{
	struct aio_req *cancelled = NULL, **tail = &cancelled;
	int result = AIO_ALLDONE;
	struct aio_fdq *q;

	if (fcntl(fildes, F_GETFL) == -1) {
		errno = EBADF;
		return -1;
	}
	if (aiocbp && aiocbp->aio_fildes != fildes) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&aio_lock);
	q = aio_fdq_find_locked(fildes);
	if (q) {
		struct aio_req **pp = &q->head;

		while (*pp) {
			struct aio_req *req = *pp;

			if (aiocbp && req->cb != aiocbp) {
				pp = &req->next;
				continue;
			}
			if (req->state == REQ_RUNNING) {
				result = AIO_NOTCANCELED;
				pp = &req->next;
				continue;
			}
			*pp = req->next;
			req->next = NULL;
			*tail = req;
			tail = &req->next;
		}
		if (cancelled && result != AIO_NOTCANCELED)
			result = AIO_CANCELED;
		if (!q->running) {
			/* The descriptor was runnable; it may not be any more. */
			if (!q->head)
				aio_pending--;
		}
		if (!q->head && !q->running)
			aio_fdq_unlink_locked(q);
	}
	/* A cancelled request completes with ECANCELED: published and taken
	 * out of its batch under the lock, announced as any completion is. */
	for (struct aio_req *req = cancelled; req; req = req->next) {
		aio_publish(req->cb, -1, ECANCELED);
		aio_detach_group_locked(req);
	}
	pthread_mutex_unlock(&aio_lock);

	while (cancelled) {
		struct aio_req *req = cancelled;

		cancelled = req->next;
		aio_announce(req);
	}
	return result;
}

int lio_listio(int mode, struct aiocb *const list[], int nent,
	       struct sigevent *sig)
{
	struct aio_group *group;
	int failed = 0, queued = 0;

	if ((mode != LIO_WAIT && mode != LIO_NOWAIT) || nent < 0 ||
	    (nent > 0 && !list)) {
		errno = EINVAL;
		return -1;
	}
	if (mode == LIO_NOWAIT && sig && sig->sigev_notify != SIGEV_NONE &&
	    sig->sigev_notify != SIGEV_SIGNAL && sig->sigev_notify != SIGEV_THREAD) {
		errno = EINVAL;
		return -1;
	}

	group = calloc(1, sizeof(*group));
	if (!group) {
		errno = EAGAIN;
		return -1;
	}
	group->waiting = mode == LIO_WAIT;
	group->pid = getpid();
	if (mode == LIO_NOWAIT && sig)
		group->sev = *sig;
	else
		group->sev.sigev_notify = SIGEV_NONE;

	/* Hold one reference for the submission loop itself, so the batch
	 * cannot complete (and be freed) before every entry is queued. */
	group->remaining = 1;

	for (int i = 0; i < nent; i++) {
		struct aiocb *cb = list[i];
		int rc;

		if (!cb || cb->aio_lio_opcode == LIO_NOP)
			continue;
		if (cb->aio_lio_opcode != LIO_READ && cb->aio_lio_opcode != LIO_WRITE) {
			cb->__return_value = -1;
			__atomic_store_n(&cb->__error_code, EINVAL, __ATOMIC_RELEASE);
			failed = 1;
			continue;
		}
		pthread_mutex_lock(&aio_lock);
		group->remaining++;
		pthread_mutex_unlock(&aio_lock);
		rc = aio_enqueue(cb, cb->aio_lio_opcode, group);
		if (rc) {
			pthread_mutex_lock(&aio_lock);
			group->remaining--;
			pthread_mutex_unlock(&aio_lock);
			cb->__return_value = -1;
			__atomic_store_n(&cb->__error_code, rc, __ATOMIC_RELEASE);
			failed = 1;
			continue;
		}
		queued++;
	}

	/* Drop the submission reference. */
	pthread_mutex_lock(&aio_lock);
	int last = aio_group_put_locked(group);
	pthread_mutex_unlock(&aio_lock);

	if (mode == LIO_NOWAIT) {
		/* Nothing queued, or everything already done: announce now. */
		if (last) {
			aio_sigevent_deliver(&group->sev, group->pid);
			free(group);
		}
		if (failed) {
			errno = EIO;
			return -1;
		}
		return 0;
	}

	/* LIO_WAIT: sleep until the batch is complete.  A signal does not end
	 * the wait early -- the requests would be left without anyone to
	 * collect the batch record -- so EINTR is not reported. */
	for (;;) {
		int gen = __atomic_load_n(&aio_done_gen, __ATOMIC_ACQUIRE);
		int left;

		pthread_mutex_lock(&aio_lock);
		left = group->remaining;
		pthread_mutex_unlock(&aio_lock);
		if (left == 0)
			break;
		futex_wait(&aio_done_gen, gen, NULL);
	}
	free(group);

	if (!failed) {
		for (int i = 0; i < nent; i++) {
			struct aiocb *cb = list[i];

			if (cb && cb->aio_lio_opcode != LIO_NOP &&
			    aio_error(cb) != 0) {
				failed = 1;
				break;
			}
		}
	}
	(void)queued;
	if (failed) {
		errno = EIO;
		return -1;
	}
	return 0;
}

void aio_init(const struct aioinit *init)
{
	if (!init)
		return;
	pthread_mutex_lock(&aio_lock);
	if (init->aio_threads > 0)
		aio_max_threads = init->aio_threads;
	if (init->aio_idle_time > 0)
		aio_idle_seconds = init->aio_idle_time;
	pthread_mutex_unlock(&aio_lock);
}
