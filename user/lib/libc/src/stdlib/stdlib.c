/*
 * Copyright (C) 2026 The LikeOS Project
 */

#include <elf.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <limits.h>
#include <locale.h>

/* atexit / at_quick_exit and the exit handler list live in cxa_atexit.c: they
 * are the same mechanism as the C++ static-destructor registry, which has to
 * record which shared object each handler came from so dlclose() can run and
 * remove that object's handlers before its pages go away. */
void __libc_run_fini_array(void);
void __cxa_finalize(void *dso);


void exit(int status)
{
	/* NULL means every handler regardless of which object registered it:
	 * atexit(3) handlers, and the destructors of every static C++ object
	 * still standing.  Most recently registered first. */
	__cxa_finalize(NULL);
	/* Destructors run after atexit handlers and before the streams are
	 * flushed, so anything a destructor prints still reaches the output. */
	__libc_run_fini_array();
	/* Every open stream, not only the standard two.  Anything a program
	 * wrote to a file it did not fclose was being thrown away here. */
	fflush(NULL);
	_exit(status);
}

void quick_exit(int status)
{
	exit(status);
}

/* C99 7.20.4.4.  Deliberately none of exit()'s work: no handlers, no
 * destructors, no flush -- a caller reaching for this one has decided that
 * running them is the wrong thing, usually because it is a child process that
 * would otherwise flush a buffer its parent already owns. */
void _Exit(int status)
{
	_exit(status);
}

void abort(void)
{
	/* POSIX: abort() must raise SIGABRT so the process terminates with a
	 * signal (WIFSIGNALED true), not a normal exit.  The old _exit(1) made
	 * a heap-corruption / assertion abort indistinguishable from a benign
	 * exit(1), so a parent (teststress) waiting on the child could not tell
	 * a crash from an ordinary failure and did not stop.
	 *
	 * SIGABRT must be UNBLOCKED first.  abort() is defined to end the
	 * process whatever the caller has done to its signal mask, and a
	 * blocked SIGABRT would otherwise just sit pending: the allocator would
	 * detect heap corruption, call abort(), and the program would carry on
	 * running with a corrupt heap.  Worker threads that mask signals so
	 * only one thread handles them are an ordinary reason for the mask to
	 * be set, so this is not a hypothetical case.
	 *
	 * If a handler catches it and returns, restore the default action,
	 * unblock again (the handler may have re-blocked it) and re-raise, so
	 * abort() cannot be trapped into returning.  _exit is the last resort. */
	extern int raise(int sig);
	sigset_t abrt_set;

	sigemptyset(&abrt_set);
	sigaddset(&abrt_set, SIGABRT);
	sigprocmask(SIG_UNBLOCK, &abrt_set, NULL);

	raise(SIGABRT);

	signal(SIGABRT, SIG_DFL);
	sigprocmask(SIG_UNBLOCK, &abrt_set, NULL);
	raise(SIGABRT);
	_exit(127);
}

/* basename()/dirname() — POSIX pathname helpers.
 *
 * <libgen.h> also defines these as static inline for code that includes it,
 * but they must exist as real EXPORTED symbols in libc.so for code that
 * references them externally — e.g. curl's lib/mime.c, which is compiled with
 * HAVE_BASENAME but without HAVE_LIBGEN_H, so it calls basename() expecting
 * the system libc to provide it.  Behaviour matches the libgen.h inlines.
 * Both may modify the input string. */
char *basename(char *path)
{
	static char dot[] = ".";
	if (!path || !*path)
		return dot;

	/* Strip trailing slashes (but keep a lone "/"). */
	char *end = path + __builtin_strlen(path) - 1;
	while (end > path && *end == '/')
		*end-- = '\0';

	char *last_slash = (char *)0;
	for (char *p = path; *p; p++)
		if (*p == '/')
			last_slash = p;

	if (last_slash) {
		if (*(last_slash + 1) == '\0')
			return last_slash; /* path was "/" (or "///") */
		return last_slash + 1;
	}
	return path;
}

char *dirname(char *path)
{
	static char dot[] = ".";
	if (!path || !*path)
		return dot;

	char *last_slash = (char *)0;
	for (char *p = path; *p; p++)
		if (*p == '/')
			last_slash = p;

	if (!last_slash)
		return dot;
	if (last_slash == path)
		return "/";

	*last_slash = '\0';
	return path;
}

int atoi(const char *nptr)
{
	return (int)atol(nptr);
}

long atol(const char *nptr)
{
	return strtol(nptr, NULL, 10);
}

long long atoll(const char *nptr)
{
	return strtoll(nptr, NULL, 10);
}

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
	       c == '\v';
}

static int is_digit(int c)
{
	return c >= '0' && c <= '9';
}

long strtol(const char *nptr, char **endptr, int base)
{
	const char *s = nptr;
	long acc = 0;
	int neg = 0;

	// Skip whitespace
	while (is_space(*s)) {
		s++;
	}

	// Handle sign
	if (*s == '-') {
		neg = 1;
		s++;
	} else if (*s == '+') {
		s++;
	}

	// Auto-detect base
	if (base == 0) {
		if (*s == '0') {
			if (s[1] == 'x' || s[1] == 'X') {
				base = 16;
				s += 2;
			} else {
				base = 8;
			}
		} else {
			base = 10;
		}
	} else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}

	// Convert digits
	while (*s) {
		int digit;
		if (is_digit(*s)) {
			digit = *s - '0';
		} else if (*s >= 'a' && *s <= 'z') {
			digit = *s - 'a' + 10;
		} else if (*s >= 'A' && *s <= 'Z') {
			digit = *s - 'A' + 10;
		} else {
			break;
		}

		if (digit >= base) {
			break;
		}

		acc = acc * base + digit;
		s++;
	}

	if (endptr) {
		*endptr = (char *)s;
	}

	return neg ? -acc : acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
	// For simplicity, use strtol and cast
	return (unsigned long)strtol(nptr, endptr, base);
}

long long strtoll(const char *nptr, char **endptr, int base)
{
	// Similar to strtol but with long long
	return (long long)strtol(nptr, endptr, base);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
	return (unsigned long long)strtoll(nptr, endptr, base);
}

/* ── Floating-point string conversion ───────────────────────────────── */

double strtod(const char *nptr, char **endptr)
{
	const char *s = nptr;

	/* Skip leading whitespace */
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' ||
	       *s == '\f' || *s == '\v')
		s++;

	/* Sign */
	int neg = 0;
	if (*s == '-') {
		neg = 1;
		s++;
	} else if (*s == '+') {
		s++;
	}

	/* Handle special values: inf, infinity, nan */
	if ((s[0] == 'i' || s[0] == 'I') && (s[1] == 'n' || s[1] == 'N') &&
	    (s[2] == 'f' || s[2] == 'F')) {
		s += 3;
		if ((s[0] == 'i' || s[0] == 'I') &&
		    (s[1] == 'n' || s[1] == 'N') &&
		    (s[2] == 'i' || s[2] == 'I') &&
		    (s[3] == 't' || s[3] == 'T') &&
		    (s[4] == 'y' || s[4] == 'Y'))
			s += 5;
		if (endptr)
			*endptr = (char *)s;
		/* Use integer trick for infinity: 1.0/0.0 is +inf */
		double inf = 1e308 * 10.0;
		return neg ? -inf : inf;
	}
	if ((s[0] == 'n' || s[0] == 'N') && (s[1] == 'a' || s[1] == 'A') &&
	    (s[2] == 'n' || s[2] == 'N')) {
		s += 3;
		/* Skip optional (n-char-sequence) */
		if (*s == '(') {
			const char *p = s + 1;
			while (*p && *p != ')')
				p++;
			if (*p == ')')
				s = p + 1;
		}
		if (endptr)
			*endptr = (char *)s;
		double nan_val = 0.0 / 0.0;
		return nan_val;
	}

	/* Handle 0x hex float */
	int hex = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		/* Only if followed by hex digit or '.' */
		const char *t = s + 2;
		if ((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'f') ||
		    (*t >= 'A' && *t <= 'F') || *t == '.') {
			hex = 1;
			s += 2;
		}
	}

	double result = 0.0;
	int got_digit = 0;

	if (hex) {
		/* Hex float: digits.digits p exponent */
		while (1) {
			int d;
			if (*s >= '0' && *s <= '9')
				d = *s - '0';
			else if (*s >= 'a' && *s <= 'f')
				d = *s - 'a' + 10;
			else if (*s >= 'A' && *s <= 'F')
				d = *s - 'A' + 10;
			else
				break;
			result = result * 16.0 + d;
			got_digit = 1;
			s++;
		}
		if (*s == '.') {
			s++;
			double frac = 1.0 / 16.0;
			while (1) {
				int d;
				if (*s >= '0' && *s <= '9')
					d = *s - '0';
				else if (*s >= 'a' && *s <= 'f')
					d = *s - 'a' + 10;
				else if (*s >= 'A' && *s <= 'F')
					d = *s - 'A' + 10;
				else
					break;
				result += d * frac;
				frac /= 16.0;
				got_digit = 1;
				s++;
			}
		}
		/* Binary exponent: p[+-]digits */
		if (got_digit && (*s == 'p' || *s == 'P')) {
			s++;
			int exp_neg = 0;
			if (*s == '-') {
				exp_neg = 1;
				s++;
			} else if (*s == '+') {
				s++;
			}
			int exp_val = 0;
			while (*s >= '0' && *s <= '9') {
				exp_val = exp_val * 10 + (*s - '0');
				s++;
			}
			double base = 2.0;
			double mult = 1.0;
			while (exp_val > 0) {
				if (exp_val & 1)
					mult *= base;
				base *= base;
				exp_val >>= 1;
			}
			if (exp_neg)
				result /= mult;
			else
				result *= mult;
		}
	} else {
		/* Decimal float: digits.digits e exponent */
		while (*s >= '0' && *s <= '9') {
			result = result * 10.0 + (*s - '0');
			got_digit = 1;
			s++;
		}
		if (*s == '.') {
			s++;
			double frac = 0.1;
			while (*s >= '0' && *s <= '9') {
				result += (*s - '0') * frac;
				frac *= 0.1;
				got_digit = 1;
				s++;
			}
		}
		/* Decimal exponent: e[+-]digits */
		if (got_digit && (*s == 'e' || *s == 'E')) {
			s++;
			int exp_neg = 0;
			if (*s == '-') {
				exp_neg = 1;
				s++;
			} else if (*s == '+') {
				s++;
			}
			int exp_val = 0;
			int exp_digits = 0;
			while (*s >= '0' && *s <= '9') {
				exp_val = exp_val * 10 + (*s - '0');
				exp_digits = 1;
				s++;
			}
			if (exp_digits) {
				/* Apply power of 10 via repeated squaring */
				double base = 10.0;
				double mult = 1.0;
				int e = exp_val;
				while (e > 0) {
					if (e & 1)
						mult *= base;
					base *= base;
					e >>= 1;
				}
				if (exp_neg)
					result /= mult;
				else
					result *= mult;
			}
		}
	}

	if (!got_digit) {
		/* No valid conversion; set endptr to nptr */
		if (endptr)
			*endptr = (char *)nptr;
		return 0.0;
	}

	if (endptr)
		*endptr = (char *)s;
	return neg ? -result : result;
}

float strtof(const char *nptr, char **endptr)
{
	return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr)
{
	return (long double)strtod(nptr, endptr);
}

/* The conversions in a given locale.  A locale decides only the radix
 * character here, and every locale this system has -- the global one and
 * any newlocale() returns -- uses '.', so they convert exactly as the
 * functions above.  Software that wants numbers parsed independently of the
 * user's locale (configuration files, device databases) calls these with a
 * "C" locale object; it gets that behaviour. */
double strtod_l(const char *nptr, char **endptr, locale_t loc)
{
	(void)loc;
	return strtod(nptr, endptr);
}

float strtof_l(const char *nptr, char **endptr, locale_t loc)
{
	(void)loc;
	return strtof(nptr, endptr);
}

long double strtold_l(const char *nptr, char **endptr, locale_t loc)
{
	(void)loc;
	return strtold(nptr, endptr);
}

double atof(const char *nptr)
{
	return strtod(nptr, NULL);
}

/* realpath(3): the canonical absolute name of an existing file -- every
 * ".", ".." and symbolic link resolved, component by component, the way
 * POSIX specifies it.  A component that does not exist fails the call with
 * ENOENT, a non-directory in the middle with ENOTDIR, more than
 * RP_MAX_LINKS links with ELOOP.  (It used to be textual normalisation
 * only: /dev/disk/by-partlabel/x came back as itself instead of the
 * /dev/sdXN it points at, and tools that find a disk's sysfs entry by name
 * looked for a disk called "x".) */
#define RP_MAX_LINKS 40

char *realpath(const char *path, char *resolved_path)
{
	char out[PATH_MAX]; /* resolved so far, "" = the root */
	char rest[PATH_MAX]; /* still to resolve */
	char link[PATH_MAX];
	size_t olen = 0, rlen;
	unsigned links = 0;
	const char *p;

	if (!path) {
		errno = EINVAL;
		return NULL;
	}
	if (!*path) {
		errno = ENOENT;
		return NULL;
	}
	rlen = strlen(path);
	if (rlen >= sizeof(rest)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	memcpy(rest, path, rlen + 1);
	out[0] = '\0';
	if (rest[0] != '/') {
		if (!getcwd(out, sizeof(out)))
			return NULL;
		olen = strlen(out);
		if (olen == 1 && out[0] == '/')
			olen = 0; /* the root is "" here */
		out[olen] = '\0';
	}

	p = rest;
	for (;;) {
		const char *c, *e;
		size_t clen;
		struct stat st;
		int more;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		c = p;
		e = c;
		while (*e && *e != '/')
			e++;
		clen = (size_t)(e - c);
		p = e;
		/* is another component (or a trailing slash) after this? */
		more = *p == '/';

		if (clen == 1 && c[0] == '.')
			continue;
		if (clen == 2 && c[0] == '.' && c[1] == '.') {
			while (olen > 0 && out[olen - 1] != '/')
				olen--;
			if (olen > 0)
				olen--; /* the separator */
			out[olen] = '\0';
			continue;
		}
		if (olen + 1 + clen >= sizeof(out)) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		out[olen] = '/';
		memcpy(out + olen + 1, c, clen);
		out[olen + 1 + clen] = '\0';

		if (lstat(out, &st) != 0)
			return NULL; /* errno from lstat: ENOENT, EACCES ... */
		if (S_ISLNK(st.st_mode)) {
			ssize_t n;
			size_t tail = strlen(p);

			if (++links > RP_MAX_LINKS) {
				errno = ELOOP;
				return NULL;
			}
			n = readlink(out, link, sizeof(link) - 1);
			if (n < 0)
				return NULL;
			if (n == 0) {
				errno = ENOENT;
				return NULL;
			}
			link[n] = '\0';
			/* the rest becomes: target + what was left (which
			 * starts with its own separator, if anything) */
			if ((size_t)n + tail + 1 >= sizeof(rest)) {
				errno = ENAMETOOLONG;
				return NULL;
			}
			memmove(rest + n, p, tail + 1);
			memcpy(rest, link, (size_t)n);
			p = rest;
			/* an absolute target restarts at the root, a relative
			 * one from the directory holding the link */
			if (link[0] == '/')
				olen = 0;
			out[olen] = '\0';
			continue;
		}
		olen += 1 + clen;
		if (more && !S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return NULL;
		}
	}
	if (olen == 0) {
		out[0] = '/';
		out[1] = '\0';
		olen = 1;
	}
	if (!resolved_path) {
		resolved_path = (char *)malloc(olen + 1);
		if (!resolved_path) {
			errno = ENOMEM;
			return NULL;
		}
	}
	memcpy(resolved_path, out, olen + 1);
	return resolved_path;
}

int abs(int n)
{
	return n < 0 ? -n : n;
}

long labs(long n)
{
	return n < 0 ? -n : n;
}

/* One sort serves both qsort() and qsort_r(): quicksort with a median-of-three
 * pivot, recursing into the smaller side and looping on the larger so stack
 * depth stays O(log n), and finishing short runs with insertion sort.  This
 * replaced a bubble sort — directory and font-name lists routinely have
 * thousands of entries, where O(n^2) is seconds rather than milliseconds. */
static void sort_swap(char *a, char *b, size_t size)
{
	if (a == b)
		return;
	for (size_t k = 0; k < size; k++) {
		char tmp = a[k];
		a[k] = b[k];
		b[k] = tmp;
	}
}

typedef int (*sort_cmp_t)(const void *, const void *, void *);

static int sort_call(sort_cmp_t cmp, const void *a, const void *b, void *arg)
{
	return cmp(a, b, arg);
}

static void sort_range(char *base, size_t nmemb, size_t size, sort_cmp_t cmp,
		       void *arg)
{
	while (nmemb > 8) {
		char *lo = base;
		char *hi = base + (nmemb - 1) * size;
		char *mid = base + (nmemb / 2) * size;
		char *i, *j;
		size_t left_n, right_n;

		/* Median of three, parked at lo, so already-sorted and
		 * reverse-sorted inputs do not degrade to O(n^2). */
		if (sort_call(cmp, mid, lo, arg) < 0)
			sort_swap(mid, lo, size);
		if (sort_call(cmp, hi, mid, arg) < 0) {
			sort_swap(hi, mid, size);
			if (sort_call(cmp, mid, lo, arg) < 0)
				sort_swap(mid, lo, size);
		}
		sort_swap(mid, lo, size);

		i = lo;
		j = hi + size;
		for (;;) {
			do {
				i += size;
			} while (i <= hi && sort_call(cmp, i, lo, arg) < 0);
			do {
				j -= size;
			} while (sort_call(cmp, j, lo, arg) > 0);
			if (i > j)
				break;
			sort_swap(i, j, size);
		}
		sort_swap(lo, j, size);

		left_n = (size_t)((j - lo) / (long)size);
		right_n = nmemb - left_n - 1;

		/* Recurse into the smaller partition, iterate on the larger. */
		if (left_n < right_n) {
			sort_range(lo, left_n, size, cmp, arg);
			base = j + size;
			nmemb = right_n;
		} else {
			sort_range(j + size, right_n, size, cmp, arg);
			nmemb = left_n;
		}
	}

	/* Short tail: insertion sort. */
	for (size_t k = 1; k < nmemb; k++) {
		char *cur = base + k * size;
		char *prev = cur - size;
		while (cur > base && sort_call(cmp, prev, cur, arg) > 0) {
			sort_swap(prev, cur, size);
			cur = prev;
			prev -= size;
		}
	}
}

/* Adapter so plain qsort() can share sort_range() without paying for an
 * indirect call through a captured argument. */
static int sort_plain_cmp(const void *a, const void *b, void *arg)
{
	int (*compar)(const void *, const void *) =
		(int (*)(const void *, const void *))arg;
	return compar(a, b);
}

void qsort(void *base, size_t nmemb, size_t size,
	   int (*compar)(const void *, const void *))
{
	if (!base || !compar || size == 0 || nmemb <= 1)
		return;
	sort_range((char *)base, nmemb, size, sort_plain_cmp, (void *)compar);
}

void qsort_r(void *base, size_t nmemb, size_t size,
	     int (*compar)(const void *, const void *, void *), void *arg)
{
	if (!base || !compar || size == 0 || nmemb <= 1)
		return;
	sort_range((char *)base, nmemb, size, compar, arg);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
	      int (*compar)(const void *, const void *))
{
	const char *b = base;
	size_t left = 0;
	size_t right = nmemb;

	while (left < right) {
		size_t mid = left + (right - left) / 2;
		const void *p = b + mid * size;
		int cmp = compar(key, p);

		if (cmp == 0) {
			return (void *)p;
		} else if (cmp < 0) {
			right = mid;
		} else {
			left = mid + 1;
		}
	}

	return NULL;
}

/* system: fork+exec /bin/sh -c command. Returns child status, or -1 on
 * fork/exec failure. NULL command merely tests for shell availability;
 * we always have /bin/sh, so return non-zero. */
int system(const char *command)
{
	extern int execl(const char *path, const char *arg, ...);
	extern int waitpid(int pid, int *status, int options);
	extern int fork(void);
	extern void _exit(int);
	if (!command)
		return 1;
	int pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl("/bin/sh", "sh", "-c", command, (char *)0);
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return status;
}

/* strtoimax / strtoumax */
#include <inttypes.h>
intmax_t strtoimax(const char *nptr, char **endptr, int base)
{
	return (intmax_t)strtoll(nptr, endptr, base);
}
uintmax_t strtoumax(const char *nptr, char **endptr, int base)
{
	return (uintmax_t)strtoull(nptr, endptr, base);
}

/* The remaining integer absolute-value and division functions.
 *
 * div()/ldiv()/lldiv() return quotient and remainder together because C's /
 * and % are two separate operations that most processors compute at once —
 * and because they are defined to truncate toward zero, which is the part
 * callers get wrong when they hand-roll it for negative operands. */
long long llabs(long long j)
{
	return j < 0 ? -j : j;
}

intmax_t imaxabs(intmax_t j)
{
	return j < 0 ? -j : j;
}

div_t div(int num, int den)
{
	div_t r;

	r.quot = num / den;
	r.rem = num % den;
	return r;
}

ldiv_t ldiv(long num, long den)
{
	ldiv_t r;

	r.quot = num / den;
	r.rem = num % den;
	return r;
}

lldiv_t lldiv(long long num, long long den)
{
	lldiv_t r;

	r.quot = num / den;
	r.rem = num % den;
	return r;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (size && nmemb > (size_t)-1 / size) {
		errno = ENOMEM;
		return NULL;
	}
	return realloc(ptr, nmemb * size);
}
