/* Host test of kernel/net/eventpoll.c (host/test-net.sh epoll).
 *
 * The real epoll code and the kernel's wait-queue walker run against
 * objects with wait queues (ep_kside.c): readiness, edge and level
 * triggering, one-shot, exclusive wakeups, RDHUP, nested sets and their
 * loop and depth limits, removal when the last descriptor of an object is
 * closed, thousands of items, maxevents, fairness, the blocking wait and its
 * wake-ups, and every error the calls report.  Each test closes everything
 * and checks that every object was freed (ASan checks the rest).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "ep-api.h"

#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLRDHUP 0x2000
#define EPOLLIN POLLIN
#define EPOLLOUT POLLOUT
#define EPOLLERR POLLERR
#define EPOLLHUP POLLHUP
#define EPOLLRDHUP POLLRDHUP
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP (1U << 29)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 0x80000

struct ev {
	uint32_t events;
	uint64_t data;
} __attribute__((packed));

static int failures, checks;
static const char *cur_test;

void hx_fatal(const char *what)
{
	fprintf(stderr, "FATAL in %s: %s\n", cur_test ? cur_test : "?", what);
	abort();
}

void hx_log(const char *fmt, __builtin_va_list ap)
{
	fprintf(stderr, "  kernel: ");
	vfprintf(stderr, fmt, ap);
}

void *hx_malloc(unsigned long n)
{
	return malloc(n);
}

void hx_free(void *p)
{
	free(p);
}

static void (*sleep_hook)(void);
static int hook_calls;

void hx_sleep_hook(void)
{
	hook_calls++;
	if (sleep_hook)
		sleep_hook();
}

#define CHECK(c)                                                          \
	do {                                                              \
		checks++;                                                 \
		if (!(c)) {                                               \
			failures++;                                       \
			fprintf(stderr, "  FAIL %s:%d [%s]: %s\n", __FILE__, \
				__LINE__, cur_test, #c);                  \
		}                                                         \
	} while (0)

static int ep_new(void)
{
	long e = kx_epoll_create1(0);

	CHECK(e >= 3);
	return (int)e;
}

static long add(int ep, int fd, unsigned events, uint64_t data)
{
	return kx_epoll_ctl(ep, EPOLL_CTL_ADD, fd, events, data);
}

static long wait0(int ep, struct ev *out, int max)
{
	return kx_epoll_wait(ep, out, max, 0);
}

/* ---------------------------------------------------------------------- */

static void t_level(void)
{
	int ep = ep_new(), a = kx_open(KX_OBJ, 0);
	struct ev out[4];

	CHECK(add(ep, a, EPOLLIN, 0x1234) == 0);
	CHECK(wait0(ep, out, 4) == 0);
	kx_set_ready(a, POLLIN, 1);
	CHECK(wait0(ep, out, 4) == 1);
	CHECK(out[0].data == 0x1234 && out[0].events == EPOLLIN);
	/* level: still ready, reported again */
	CHECK(wait0(ep, out, 4) == 1);
	/* readiness gone without a wake: dropped at the next look */
	kx_set_ready(a, 0, 0);
	CHECK(wait0(ep, out, 4) == 0);
	CHECK(wait0(ep, out, 4) == 0);
	/* errors and hangups are always reported */
	kx_set_ready(a, POLLHUP, 1);
	CHECK(wait0(ep, out, 4) == 1 && out[0].events == EPOLLHUP);
	kx_reset();
}

static void t_edge(void)
{
	int ep = ep_new(), a = kx_open(KX_OBJ, 0);
	struct ev out[4];

	CHECK(add(ep, a, EPOLLIN | EPOLLET, 7) == 0);
	kx_set_ready(a, POLLIN, 1);
	CHECK(wait0(ep, out, 4) == 1 && out[0].data == 7);
	CHECK(wait0(ep, out, 4) == 0); /* edge: not again */
	kx_set_ready(a, POLLIN, 1); /* a new event */
	CHECK(wait0(ep, out, 4) == 1);
	CHECK(wait0(ep, out, 4) == 0);
	/* ready at ADD time: reported once */
	int b = kx_open(KX_OBJ, POLLIN);
	CHECK(add(ep, b, EPOLLIN | EPOLLET, 8) == 0);
	CHECK(wait0(ep, out, 4) == 1 && out[0].data == 8);
	CHECK(wait0(ep, out, 4) == 0);
	kx_reset();
}

static void t_oneshot(void)
{
	int ep = ep_new(), a = kx_open(KX_OBJ, POLLIN);
	struct ev out[4];

	CHECK(add(ep, a, EPOLLIN | EPOLLONESHOT, 1) == 0);
	CHECK(wait0(ep, out, 4) == 1);
	CHECK(wait0(ep, out, 4) == 0);
	kx_set_ready(a, POLLIN, 1);
	CHECK(wait0(ep, out, 4) == 0); /* disarmed */
	CHECK(kx_epoll_ctl(ep, EPOLL_CTL_MOD, a, EPOLLIN | EPOLLONESHOT, 2) ==
	      0);
	CHECK(wait0(ep, out, 4) == 1 && out[0].data == 2);
	CHECK(wait0(ep, out, 4) == 0);
	kx_reset();
}

static void t_rdhup_and_keys(void)
{
	int ep = ep_new(), a = kx_open(KX_OBJ, 0), b = kx_open(KX_OBJ, 0);
	struct ev out[4];

	CHECK(add(ep, a, EPOLLIN | EPOLLRDHUP, 1) == 0);
	CHECK(add(ep, b, EPOLLIN, 2) == 0);
	kx_set_ready(a, POLLIN | POLLRDHUP, 1);
	kx_set_ready(b, POLLIN | POLLRDHUP, 1);
	long n = wait0(ep, out, 4);
	CHECK(n == 2);
	for (int i = 0; i < n; i++) {
		if (out[i].data == 1)
			CHECK(out[i].events == (EPOLLIN | EPOLLRDHUP));
		else
			CHECK(out[i].events == EPOLLIN); /* not asked */
	}
	kx_reset();

	/* A wake whose key names nothing the item wants is ignored. */
	ep = ep_new();
	a = kx_open(KX_OBJ, 0);
	CHECK(add(ep, a, EPOLLOUT | EPOLLET, 3) == 0);
	kx_set_ready(a, POLLOUT, 0);
	kx_wake(a, POLLIN);
	CHECK(wait0(ep, out, 4) == 0);
	kx_wake(a, POLLOUT);
	CHECK(wait0(ep, out, 4) == 1 && out[0].events == EPOLLOUT);
	kx_reset();
}

/* EPOLLEXCLUSIVE: of two sets watching one object exclusively, a wake
 * goes to the one with a waiter, and the other is not told. */
static int excl_fd;
static void excl_hook(void)
{
	kx_set_ready(excl_fd, POLLIN, 1);
}

static void t_exclusive(void)
{
	int ep1 = ep_new(), ep2 = ep_new(), a = kx_open(KX_OBJ, 0);
	struct ev out[4];

	CHECK(add(ep1, a, EPOLLIN | EPOLLEXCLUSIVE, 1) == 0);
	CHECK(add(ep2, a, EPOLLIN | EPOLLEXCLUSIVE, 2) == 0);
	excl_fd = a;
	sleep_hook = excl_hook;
	CHECK(kx_epoll_wait(ep1, out, 4, -1) == 1 && out[0].data == 1);
	sleep_hook = NULL;
	CHECK(wait0(ep2, out, 4) == 0); /* not woken: exclusive */
	/* without a waiter anywhere, every exclusive set hears of it */
	kx_set_ready(a, POLLIN, 1);
	CHECK(wait0(ep2, out, 4) == 1);
	/* the rules */
	CHECK(kx_epoll_ctl(ep1, EPOLL_CTL_MOD, a, EPOLLIN, 1) == -EINVAL);
	int b = kx_open(KX_OBJ, 0);
	CHECK(add(ep1, b, EPOLLIN | EPOLLEXCLUSIVE | EPOLLONESHOT, 1) ==
	      -EINVAL);
	CHECK(add(ep1, b, EPOLLIN, 1) == 0);
	CHECK(kx_epoll_ctl(ep1, EPOLL_CTL_MOD, b, EPOLLIN | EPOLLEXCLUSIVE,
			   1) == -EINVAL);
	CHECK(add(ep1, ep2, EPOLLIN | EPOLLEXCLUSIVE, 1) == -EINVAL);
	kx_reset();
}

static void t_errors(void)
{
	int ep = ep_new(), a = kx_open(KX_OBJ, 0), p = kx_open(KX_PLAIN, 0);
	struct ev out[4];

	CHECK(kx_epoll_create(0) == -EINVAL);
	CHECK(kx_epoll_create(-1) == -EINVAL);
	CHECK(kx_epoll_create1(1) == -EINVAL);
	long ep_c = kx_epoll_create1(EPOLL_CLOEXEC);
	CHECK(ep_c >= 3);
	CHECK(add(ep, a, EPOLLIN, 0) == 0);
	CHECK(add(ep, a, EPOLLIN, 0) == -EEXIST);
	CHECK(add(ep, p, EPOLLIN, 0) == -EPERM); /* a plain file */
	CHECK(add(ep, 200, EPOLLIN, 0) == -EBADF);
	CHECK(add(a, a, EPOLLIN, 0) == -EINVAL); /* not a set */
	CHECK(add(250, a, EPOLLIN, 0) == -EBADF);
	CHECK(add(ep, ep, EPOLLIN, 0) == -EINVAL); /* itself */
	int epd = kx_dup(ep);
	CHECK(add(ep, epd, EPOLLIN, 0) == -EINVAL); /* itself, by a dup */
	CHECK(kx_epoll_ctl(ep, 9, a, EPOLLIN, 0) == -EINVAL);
	CHECK(kx_epoll_ctl_null(ep, EPOLL_CTL_ADD, a) == -EFAULT);
	CHECK(kx_epoll_ctl(ep, EPOLL_CTL_MOD, p, EPOLLIN, 0) == -ENOENT);
	CHECK(kx_epoll_ctl_null(ep, EPOLL_CTL_DEL, p) == -ENOENT);
	CHECK(kx_epoll_ctl_null(ep, EPOLL_CTL_DEL, a) == 0);
	CHECK(kx_epoll_ctl_null(ep, EPOLL_CTL_DEL, a) == -ENOENT);
	/* maxevents */
	CHECK(kx_epoll_wait(ep, out, 0, 0) == -EINVAL);
	CHECK(kx_epoll_wait(ep, out, -1, 0) == -EINVAL);
	CHECK(kx_epoll_wait(ep, out, INT_MAX / 12 + 1, 0) == -EINVAL);
	CHECK(kx_epoll_wait(ep, out, INT_MAX / 12, 0) == 0);
	CHECK(kx_epoll_wait(a, out, 4, 0) == -EINVAL);
	CHECK(kx_epoll_wait(201, out, 4, 0) == -EBADF);
	/* a buffer that cannot be written: EFAULT when nothing was copied */
	CHECK(add(ep, a, EPOLLIN, 0) == 0);
	kx_set_ready(a, POLLIN, 1);
	CHECK(kx_epoll_wait_bad_buffer(ep, 4) == -EFAULT);
	CHECK(kx_warnings() == 0);
	kx_reset();
}

static void t_close_removes(void)
{
	int ep = ep_new(), a = kx_open(KX_OBJ, POLLIN);
	struct ev out[4];

	CHECK(add(ep, a, EPOLLIN, 1) == 0);
	CHECK(kx_nwaiters(a) == 1);
	/* another descriptor of the same object keeps the item */
	int a2 = kx_dup(a);
	kx_close(a);
	CHECK(wait0(ep, out, 4) == 1 && out[0].data == 1);
	/* the last one removes it: nothing reported, nothing leaked */
	kx_close(a2);
	CHECK(kx_objects_live() == 0);
	CHECK(wait0(ep, out, 4) == 0);
	/* the number is free again, and a new object takes it */
	int b = kx_open(KX_OBJ, POLLIN);
	CHECK(b == a);
	CHECK(add(ep, b, EPOLLIN, 2) == 0);
	CHECK(wait0(ep, out, 4) == 1 && out[0].data == 2);
	/* removed while on the ready list */
	kx_close(b);
	CHECK(wait0(ep, out, 4) == 0);
	/* the last descriptor closed while an epoll_wait is asking the
	 * object (another thread): no event, the item goes, nothing leaks */
	int d = kx_open(KX_OBJ, POLLIN);
	CHECK(add(ep, d, EPOLLIN, 4) == 0);
	kx_close_on_poll(d);
	CHECK(wait0(ep, out, 4) <= 1);
	CHECK(kx_objects_live() == 0);
	CHECK(wait0(ep, out, 4) == 0);
	/* the same for a set inside a set */
	int inner = ep_new(), e = kx_open(KX_OBJ, POLLIN);
	CHECK(add(inner, e, EPOLLIN, 5) == 0);
	CHECK(add(ep, inner, EPOLLIN, 6) == 0);
	kx_close_on_poll(e);
	CHECK(wait0(ep, out, 4) <= 1);
	kx_close(inner);
	CHECK(kx_objects_live() == 0);
	CHECK(wait0(ep, out, 4) == 0);
	/* a queue freed before its object: the items leave it, and DEL and
	 * the close afterwards do not touch it again */
	int q = kx_open(KX_OBJ, 0);
	CHECK(add(ep, q, EPOLLIN, 8) == 0);
	kx_free_queue(q);
	CHECK(kx_nwaiters(q) == 0);
	kx_set_ready(q, POLLIN, 1);
	CHECK(wait0(ep, out, 4) == 0); /* it hears nothing more */
	CHECK(kx_epoll_ctl_null(ep, EPOLL_CTL_DEL, q) == 0);
	kx_close(q);
	/* the set closed first: its items leave the objects' queues */
	int c = kx_open(KX_OBJ, 0);
	CHECK(add(ep, c, EPOLLIN, 3) == 0);
	CHECK(kx_nwaiters(c) == 1);
	kx_close(ep);
	CHECK(kx_nwaiters(c) == 0);
	kx_reset();
}

static void t_unixlike(void)
{
	int ep = ep_new(), u = kx_open(KX_UNIXLIKE, POLLIN);
	struct ev out[4];

	CHECK(add(ep, u, EPOLLIN, 9) == 0);
	CHECK(wait0(ep, out, 4) == 1);
	int u2 = kx_dup(u);
	kx_close(u);
	CHECK(wait0(ep, out, 4) == 1); /* still open through u2 */
	kx_close(u2); /* the last descriptor: hung up, then dropped */
	CHECK(wait0(ep, out, 4) == 0);
	CHECK(kx_objects_live() == 0); /* the item let go of it */
	/* closed while idle (no wake reaches the set): freed with the set */
	int v = kx_open(KX_UNIXLIKE, 0);
	CHECK(add(ep, v, EPOLLIN | EPOLLET, 1) == 0);
	CHECK(wait0(ep, out, 4) == 0);
	kx_close(v);
	CHECK(wait0(ep, out, 4) == 0);
	kx_close(ep);
	CHECK(kx_objects_live() == 0);
	kx_reset();
}

static void t_nested(void)
{
	int ep1 = ep_new(), ep2 = ep_new(), a = kx_open(KX_OBJ, 0);
	struct ev out[4];

	CHECK(add(ep1, a, EPOLLIN, 1) == 0);
	CHECK(add(ep2, ep1, EPOLLIN, 2) == 0);
	CHECK(wait0(ep2, out, 4) == 0);
	CHECK(kx_poll_fd(ep1, POLLIN) == 0);
	kx_set_ready(a, POLLIN, 1);
	CHECK(kx_poll_fd(ep1, POLLIN) == POLLIN || kx_poll_fd(ep1, POLLIN) ==
							   (POLLIN | 0x40));
	CHECK(wait0(ep2, out, 4) == 1 && out[0].data == 2);
	CHECK(wait0(ep1, out, 4) == 1 && out[0].data == 1);
	kx_set_ready(a, 0, 0);
	CHECK(kx_poll_fd(ep1, POLLIN) == 0);
	CHECK(wait0(ep2, out, 4) == 0);
	/* loops */
	CHECK(add(ep1, ep2, EPOLLIN, 0) == -ELOOP);
	int ep3 = ep_new();
	CHECK(add(ep3, ep2, EPOLLIN, 0) == 0);
	CHECK(add(ep1, ep3, EPOLLIN, 0) == -ELOOP);
	/* closing the inner set removes the outer's item */
	kx_close(ep1);
	CHECK(wait0(ep2, out, 4) == 0);
	kx_reset();

	/* depth: a chain of 5 sets is the most */
	int e[7];
	for (int i = 0; i < 7; i++)
		e[i] = ep_new();
	for (int i = 0; i < 4; i++)
		CHECK(add(e[i], e[i + 1], EPOLLIN, (uint64_t)i) == 0);
	CHECK(add(e[4], e[5], EPOLLIN, 4) == -ELOOP);
	/* ... counted through the parents too */
	CHECK(add(e[6], e[0], EPOLLIN, 0) == -ELOOP);
	int o = kx_open(KX_OBJ, POLLIN);
	CHECK(add(e[4], o, EPOLLIN, 77) == 0);
	CHECK(wait0(e[0], out, 4) == 1 && out[0].data == 0);
	kx_reset();
}

static void t_many(void)
{
	enum { N = 2000 };
	static int fds[N];
	static struct ev out[N + 16];
	static int seen[N];
	int ep = ep_new();

	for (int i = 0; i < N; i++) {
		fds[i] = kx_open(KX_OBJ, POLLIN);
		CHECK(add(ep, fds[i], EPOLLIN, (uint64_t)i) == 0);
	}
	CHECK(kx_epoll_wait(ep, out, N + 16, 0) == N);
	/* fairness: 100 at a time, every item once in 20 calls */
	memset(seen, 0, sizeof(seen));
	for (int r = 0; r < N / 100; r++) {
		long n = kx_epoll_wait(ep, out, 100, 0);
		CHECK(n == 100);
		for (int i = 0; i < n; i++)
			if (out[i].data < N)
				seen[out[i].data]++;
	}
	int once = 1;
	for (int i = 0; i < N; i++)
		once &= seen[i] == 1;
	CHECK(once);
	/* closing half removes half */
	for (int i = 0; i < N; i += 2)
		kx_close(fds[i]);
	CHECK(kx_epoll_wait(ep, out, N + 16, 0) == N / 2);
	kx_reset();
}

static void t_limit(void)
{
	/* 1 MB of memory: the limit is the floor, 4096 watches in all the
	 * sets together (two sets of 2100 here) */
	enum { N = 2100 };
	static int fds[N];
	int ep1 = ep_new(), ep2 = ep_new(), refused = 0, first = -1;

	for (int i = 0; i < N; i++) {
		fds[i] = kx_open(KX_OBJ, 0);
		CHECK(add(ep1, fds[i], EPOLLIN, 0) == 0);
	}
	for (int i = 0; i < N; i++) {
		long r = add(ep2, fds[i], EPOLLIN, 0);

		if (r == -ENOSPC) {
			refused++;
			if (first < 0)
				first = i;
		} else {
			CHECK(r == 0);
		}
	}
	CHECK(first == 4096 - N && refused == 2 * N - 4096);
	/* freed watches can be used again */
	kx_close(fds[0]);
	CHECK(add(ep2, fds[N - 1], EPOLLIN, 0) == 0);
	kx_reset();
}

static int block_fd;
static void block_hook(void)
{
	kx_set_ready(block_fd, POLLIN, 1);
}

static void t_blocking(void)
{
	int ep = ep_new(), a = kx_open(KX_OBJ, 0);
	struct ev out[4];

	CHECK(add(ep, a, EPOLLIN, 5) == 0);
	/* woken by the object */
	block_fd = a;
	sleep_hook = block_hook;
	unsigned long long w0 = kx_wakeups();
	CHECK(kx_epoll_wait(ep, out, 4, -1) == 1 && out[0].data == 5);
	CHECK(kx_wakeups() == w0 + 1);
	sleep_hook = NULL;
	kx_set_ready(a, 0, 0);
	/* timeout */
	unsigned long long t0 = kx_now_ns();
	hook_calls = 0;
	CHECK(kx_epoll_wait(ep, out, 4, 50) == 0);
	CHECK(kx_now_ns() - t0 >= 50000000ULL);
	CHECK(hook_calls >= 1);
	/* a signal ends the wait */
	kx_set_signal_pending(1);
	CHECK(kx_epoll_wait(ep, out, 4, -1) == -EINTR);
	kx_set_signal_pending(0);
	/* ready already: no sleep at all */
	kx_set_ready(a, POLLIN, 1);
	hook_calls = 0;
	CHECK(kx_epoll_wait(ep, out, 4, -1) == 1);
	CHECK(hook_calls == 0);
	kx_reset();
}

int main(void)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
		{ "epoll_limit", t_limit }, /* first: the limit is set once */
		{ "epoll_level", t_level },
		{ "epoll_edge", t_edge },
		{ "epoll_oneshot", t_oneshot },
		{ "epoll_rdhup_keys", t_rdhup_and_keys },
		{ "epoll_exclusive", t_exclusive },
		{ "epoll_errors", t_errors },
		{ "epoll_close_removes", t_close_removes },
		{ "epoll_unixlike", t_unixlike },
		{ "epoll_nested", t_nested },
		{ "epoll_many", t_many },
		{ "epoll_blocking", t_blocking },
	};
	int failed_tests = 0;

	kx_set_total_memory(1ULL << 20);
	for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int f0 = failures;

		cur_test = tests[i].name;
		tests[i].fn();
		if (kx_warnings()) {
			fprintf(stderr, "  FAIL [%s]: kernel warning\n",
				cur_test);
			failures++;
		}
		printf("%s %s\n", failures == f0 ? "PASS" : "FAIL", cur_test);
		if (failures != f0)
			failed_tests++;
	}
	printf("test-epoll: %d passed, %d failed (%d checks)\n",
	       (int)(sizeof(tests) / sizeof(tests[0])) - failed_tests,
	       failed_tests, checks);
	return failed_tests ? 1 : 0;
}
