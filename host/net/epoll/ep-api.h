/* The boundary between the kernel side of the epoll host test (the real
 * kernel/net/eventpoll.c and ep_kside.c, compiled freestanding with the
 * kernel headers) and the host side (test_epoll.c, with the C library).
 * Plain C types only.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_EPOLL_API_H
#define HOST_NET_EPOLL_API_H

/* ---- host services (test_epoll.c) ---- */
void hx_fatal(const char *what) __attribute__((noreturn));
void hx_log(const char *fmt, __builtin_va_list ap);
void *hx_malloc(unsigned long n);
void hx_free(void *p);
/* Called when the test task sleeps in epoll_wait(): may make objects ready
 * (which wakes the task) or let the clock run. */
void hx_sleep_hook(void);

/* ---- the kernel side as the test sees it (ep_kside.c) ---- */

/* Object kinds: like a socket, pipe or file (removed from every set by its
 * last close), like a UNIX socket (kept in memory by the items, dropped
 * once its last descriptor closes), a plain file (nothing to wait on). */
#define KX_OBJ 1
#define KX_UNIXLIKE 2
#define KX_PLAIN 3

/* Readiness bits are the POLL* values. */
int kx_open(int kind, unsigned mask); /* a new object on a new fd */
int kx_dup(int fd); /* another fd for the same object */
int kx_close(int fd);
int kx_set_ready(int fd, unsigned mask, int wake); /* set, and wake */
int kx_wake(int fd, unsigned key); /* wake with a key, state unchanged */
int kx_close_on_poll(int fd); /* closes fd while epoll_wait asks it */
int kx_free_queue(int fd); /* the queue goes before the object */
int kx_nwaiters(int fd); /* entries on the object's queue */
int kx_objects_live(void); /* objects not yet freed */
int kx_fd_object_refs(int fd);

/* The syscalls, by descriptor, with errno as -errno. */
long kx_epoll_create(int size);
long kx_epoll_create1(int flags);
long kx_epoll_ctl(int epfd, int op, int fd, unsigned events,
		  unsigned long long data);
long kx_epoll_ctl_null(int epfd, int op, int fd); /* event NULL */
/* events: an array of 12-byte records (events, data) */
long kx_epoll_wait(int epfd, void *events, int maxevents, int timeout_ms);
long kx_epoll_wait_bad_buffer(int epfd, int maxevents);
/* poll() on an epoll descriptor (its own readiness) */
unsigned kx_poll_fd(int fd, unsigned events);

void kx_set_signal_pending(int on);
void kx_set_total_memory(unsigned long long bytes);
unsigned long long kx_now_ns(void);
void kx_advance_ns(unsigned long long ns);
int kx_task_blocked(void);
unsigned long long kx_wakeups(void); /* tasks made runnable */
int kx_warnings(void);
void kx_reset(void); /* close every fd, check everything is freed */

#endif
