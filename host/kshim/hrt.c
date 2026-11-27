/* Host side of the kernel shim (host/kshim): the C library services the
 * kernel-side code reaches through h_* functions -- printing, memory,
 * threads, time, and the global wait sequence behind the wait queues.
 * Compiled against the C library, never with the kernel headers.
 *
 * Copyright (C) 2026 The LikeOS Project */
#define _GNU_SOURCE
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int h_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	return n;
}

int h_vsnprintf(char *b, unsigned long n, const char *fmt, va_list ap)
{
	return vsnprintf(b, n, fmt, ap);
}

void *h_calloc(unsigned long n, unsigned long m)
{
	return calloc(n ? n : 1, m ? m : 1);
}

void h_free(void *p)
{
	free(p);
}

void h_abort(void)
{
	fflush(stdout);
	abort();
}

void h_exit(int code)
{
	fflush(stdout);
	exit(code);
}

void h_yield(void)
{
	sched_yield();
}

void h_usleep(unsigned long us)
{
	usleep((useconds_t)us);
}

unsigned long long h_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull +
	       (unsigned long long)ts.tv_nsec;
}

struct h_thread_start {
	void (*fn)(void *);
	void *arg;
};

static void *h_thread_main(void *p)
{
	struct h_thread_start s = *(struct h_thread_start *)p;

	free(p);
	s.fn(s.arg);
	return NULL;
}

int h_thread_create(void (*fn)(void *), void *arg)
{
	struct h_thread_start *s = malloc(sizeof(*s));
	pthread_attr_t attr;
	pthread_t t;
	int rc;

	if (!s)
		return -1;
	s->fn = fn;
	s->arg = arg;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 1 << 20);
	rc = pthread_create(&t, &attr, h_thread_main, s);
	pthread_attr_destroy(&attr);
	if (rc) {
		free(s);
		return -1;
	}
	return 0;
}

static pthread_mutex_t g_wq_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wq_c = PTHREAD_COND_INITIALIZER;
static unsigned long g_wq_seq;

unsigned long h_wq_seq(void)
{
	unsigned long s;

	pthread_mutex_lock(&g_wq_m);
	s = g_wq_seq;
	pthread_mutex_unlock(&g_wq_m);
	return s;
}

void h_wq_wait(unsigned long seq)
{
	struct timespec ts;

	pthread_mutex_lock(&g_wq_m);
	while (g_wq_seq == seq) {
		/* Timed, so a lost wake shows up as slowness, never as a
		 * hang of the whole run; the waiter re-tests its
		 * condition either way. */
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50 * 1000 * 1000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		if (pthread_cond_timedwait(&g_wq_c, &g_wq_m, &ts) != 0)
			break;
	}
	pthread_mutex_unlock(&g_wq_m);
}

void h_wq_wake(void)
{
	pthread_mutex_lock(&g_wq_m);
	g_wq_seq++;
	pthread_cond_broadcast(&g_wq_c);
	pthread_mutex_unlock(&g_wq_m);
}

unsigned long h_rand(void)
{
	static __thread unsigned int seed;

	if (!seed)
		seed = (unsigned int)(h_now_ns() ^ (unsigned long)pthread_self());
	return (unsigned long)rand_r(&seed);
}

void h_print_stack(void)
{
	void *bt[32];
	int n = backtrace(bt, 32);

	backtrace_symbols_fd(bt, n, 2);
}
