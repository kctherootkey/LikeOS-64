/*
 * <aio.h> -- POSIX asynchronous I/O.
 *
 * aio_read()/aio_write()/aio_fsync() queue a request described by a
 * struct aiocb and return at once; the request runs on a pool of worker
 * threads inside the C library (the kernel sees ordinary pread/pwrite/
 * fsync calls).  aio_error() tells whether it has finished (EINPROGRESS
 * while it has not), aio_return() gives its result, aio_suspend() sleeps
 * until one of a set has finished, aio_cancel() withdraws requests that
 * have not started, and lio_listio() submits a batch, optionally waiting
 * for all of it.
 *
 * Requests on the same file descriptor run one after the other, in
 * priority order (a larger aio_reqprio runs later) and, among equal
 * priorities, in submission order; requests on different descriptors run
 * in parallel, up to the thread limit set by aio_init() (default 20).
 * aio_fsync() therefore covers every request queued on the descriptor
 * before it.  A write on a descriptor opened O_APPEND appends.
 *
 * Completion is announced as aio_sigevent says: SIGEV_NONE, SIGEV_SIGNAL
 * (sigev_signo queued to the submitting process with sigev_value), or
 * SIGEV_THREAD (sigev_notify_function(sigev_value) on a new thread, made
 * with sigev_notify_attributes when given, detached otherwise).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _AIO_H
#define _AIO_H

#include <sys/types.h>
#include <signal.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct aiocb {
	int aio_fildes;              /* file descriptor */
	int aio_lio_opcode;          /* lio_listio(): LIO_READ/LIO_WRITE/LIO_NOP */
	int aio_reqprio;             /* priority decrement (>= 0) */
	volatile void *aio_buf;      /* data buffer */
	size_t aio_nbytes;           /* length of the transfer */
	struct sigevent aio_sigevent; /* completion notification */
	off_t aio_offset;            /* file offset */

	/* Owned by the C library while the request is queued or running. */
	void *__req;                 /* the internal request record */
	volatile int __error_code;   /* EINPROGRESS, 0 or the error */
	int __pad;
	volatile ssize_t __return_value; /* the operation's return value */
	char __reserved[32];
};

/* aio_cancel() results. */
#define AIO_CANCELED    0  /* every request named was cancelled */
#define AIO_NOTCANCELED 1  /* at least one was already running */
#define AIO_ALLDONE     2  /* none was pending any more */

/* aio_lio_opcode values. */
#define LIO_READ  0
#define LIO_WRITE 1
#define LIO_NOP   2

/* lio_listio() modes. */
#define LIO_WAIT   0  /* return once every request has finished */
#define LIO_NOWAIT 1  /* return at once; notify when all have finished */

/* aio_init() tuning (a GNU extension, same layout). */
struct aioinit {
	int aio_threads;   /* the most worker threads */
	int aio_num;       /* expected number of simultaneous requests */
	int aio_locks;     /* unused */
	int aio_usedba;    /* unused */
	int aio_debug;     /* unused */
	int aio_numusers;  /* unused */
	int aio_idle_time; /* seconds an idle worker waits before it exits */
	int aio_reserved;
};

int aio_read(struct aiocb *aiocbp);
int aio_write(struct aiocb *aiocbp);
int aio_fsync(int op, struct aiocb *aiocbp);
int aio_error(const struct aiocb *aiocbp);
ssize_t aio_return(struct aiocb *aiocbp);
int aio_suspend(const struct aiocb *const list[], int nent,
		const struct timespec *timeout);
int aio_cancel(int fildes, struct aiocb *aiocbp);
int lio_listio(int mode, struct aiocb *const list[], int nent,
	       struct sigevent *sig);
void aio_init(const struct aioinit *init);

/* off_t is 64 bits wide, so the large-file names are the same functions. */
#if defined(_LARGEFILE64_SOURCE) || defined(_GNU_SOURCE)
#define aiocb64 aiocb
#define aio_read64 aio_read
#define aio_write64 aio_write
#define aio_fsync64 aio_fsync
#define aio_error64 aio_error
#define aio_return64 aio_return
#define aio_suspend64 aio_suspend
#define aio_cancel64 aio_cancel
#define lio_listio64 lio_listio
#endif

#ifdef __cplusplus
}
#endif

#endif /* _AIO_H */
