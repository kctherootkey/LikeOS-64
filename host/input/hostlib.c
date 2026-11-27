/* C library side of the input core harness (host/test-input.sh).
 *
 * The kernel-side units are compiled freestanding against the kernel's own
 * headers; whatever they need from the host -- memory, printing, threads,
 * the clock, files -- goes through these h_* functions, which take plain C
 * types only.
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

void *h_malloc(unsigned long n)
{
	return malloc(n);
}

void *h_calloc(unsigned long n, unsigned long m)
{
	return calloc(n, m);
}

void h_free(void *p)
{
	free(p);
}

void *h_memset(void *p, int c, unsigned long n)
{
	return memset(p, c, n);
}

void *h_memcpy(void *d, const void *s, unsigned long n)
{
	return memcpy(d, s, n);
}

int h_memcmp(const void *a, const void *b, unsigned long n)
{
	return memcmp(a, b, n);
}

unsigned long h_strlen(const char *s)
{
	return strlen(s);
}

int h_strcmp(const char *a, const char *b)
{
	return strcmp(a, b);
}

int h_strncmp(const char *a, const char *b, unsigned long n)
{
	return strncmp(a, b, n);
}

char *h_strstr(const char *h, const char *n)
{
	return strstr(h, n);
}

unsigned long long h_strtoull(const char *s, char **end, int base)
{
	return strtoull(s, end, base);
}

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

int h_eprintf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vfprintf(stderr, fmt, ap);
	va_end(ap);
	return n;
}

int h_vsnprintf(char *b, unsigned long n, const char *fmt, va_list ap)
{
	return vsnprintf(b, n, fmt, ap);
}

int h_snprintf(char *b, unsigned long n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(b, n, fmt, ap);
	va_end(ap);
	return r;
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

char *h_getenv(const char *name)
{
	return getenv(name);
}

void *h_read_file(const char *path, unsigned long *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf) {
		buf[n] = 0;
		*len = (unsigned long)n;
	}
	return buf;
}

void h_yield(void)
{
	sched_yield();
}

void h_usleep(unsigned int us)
{
	usleep(us);
}

unsigned long long h_monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ULL +
	       (unsigned long long)ts.tv_nsec;
}

void h_print_stack(void)
{
	void *pcs[64];
	int n = backtrace(pcs, 64);

	backtrace_symbols_fd(pcs, n, 2);
}

int h_thread_start(void *(*fn)(void *), void *arg, unsigned long *handle)
{
	pthread_t t;
	int rc = pthread_create(&t, NULL, fn, arg);

	if (rc == 0)
		*handle = (unsigned long)t;
	return rc;
}

void h_thread_join(unsigned long handle)
{
	pthread_join((pthread_t)handle, NULL);
}

static __thread unsigned int rnd_state = 12345;

void h_srandom(unsigned int seed)
{
	rnd_state = seed ? seed : 1;
}

unsigned long h_random(void)
{
	return (unsigned long)rand_r(&rnd_state);
}
