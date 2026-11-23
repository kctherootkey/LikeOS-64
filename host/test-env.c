/*
 * Host test for the C library's environment (user/lib/libc/src/stdlib/env.c).
 *
 * env.c is compiled against the LikeOS headers and every symbol it defines is
 * renamed with an lk_ prefix (see host/test-env.sh), so it runs next to the
 * host C library: lk_getenv, lk_setenv, ..., lk_environ.  Its allocator and
 * string calls go to the host, and the whole thing is built with
 * AddressSanitizer, so a write out of bounds or a read of freed memory stops
 * the test where it happens.
 *
 * Checked:
 *  - start-up: environ IS the envp[] it was handed; auxv found after it;
 *  - random sequences of setenv/unsetenv/putenv/getenv/clearenv, direct
 *    replacement of environ by an array of the test's own (later freed, so a
 *    stale reference to it is caught), environ set to NULL, entries without
 *    '=' and duplicate names -- after every step environ, getenv, env_count
 *    and env_iter must agree exactly with a simple model;
 *  - values of 4095 bytes, 1000+ variables at once;
 *  - POSIX details: putenv stores the caller's string, getenv points into
 *    environ, unsetenv removes every duplicate, invalid names fail EINVAL;
 *  - pointers handed out by getenv stay readable (with their old contents)
 *    after the variable is changed or removed;
 *  - readers in other threads (getenv, walking environ) while one thread
 *    keeps changing the environment: no crash, no freed memory touched, and
 *    every value read is one that was really set.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **lk_environ;
char *lk_getenv(const char *name);
int lk_setenv(const char *name, const char *value, int overwrite);
int lk_unsetenv(const char *name);
int lk_putenv(char *string);
int lk_clearenv(void);
int lk_env_iter(int *cookie, const char **name, const char **value);
int lk_env_count(void);
void lk___libc_init_environ(char **envp);
void lk___env_fork_prepare(void);
void lk___env_fork_parent(void);
void lk___env_fork_child(void);
extern void *lk___libc_auxv;

/* env.c tells the allocator when the environment exists. */
static int g_malloc_ready;
void __malloc_env_ready(void)
{
	g_malloc_ready++;
}

static int g_fail, g_checks;

#define CHECK(c, ...)                                                   \
	do {                                                            \
		g_checks++;                                             \
		if (!(c)) {                                             \
			g_fail++;                                       \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__,       \
				__LINE__);                              \
			fprintf(stderr, __VA_ARGS__);                   \
			fputc('\n', stderr);                            \
			if (g_fail > 20)                                \
				exit(1);                                \
		}                                                       \
	} while (0)

/* ---- The model: the environment as an ordered list of strings ---------- */

#define MODEL_MAX 20000
static char *m_ent[MODEL_MAX]; /* copies of the expected entries */
static int m_n;

static size_t namelen(const char *s)
{
	size_t n = 0;
	while (s[n] && s[n] != '=')
		n++;
	return n;
}

static int m_find(const char *name)
{
	size_t l = strlen(name);
	for (int i = 0; i < m_n; i++)
		if (strncmp(m_ent[i], name, l) == 0 && m_ent[i][l] == '=')
			return i;
	return -1;
}

static char *mk(const char *name, const char *value)
{
	size_t a = strlen(name), b = strlen(value);
	char *s = malloc(a + b + 2);
	memcpy(s, name, a);
	s[a] = '=';
	memcpy(s + a + 1, value, b + 1);
	return s;
}

static void m_set(const char *name, const char *value, int ow)
{
	int i = m_find(name);
	if (i >= 0) {
		if (ow) {
			free(m_ent[i]);
			m_ent[i] = mk(name, value);
		}
		return;
	}
	m_ent[m_n++] = mk(name, value);
}

static void m_put(const char *s)
{
	size_t l = namelen(s);
	char name[8192];
	memcpy(name, s, l);
	name[l] = '\0';
	int i = m_find(name);
	if (i >= 0) {
		free(m_ent[i]);
		m_ent[i] = strdup(s);
		return;
	}
	m_ent[m_n++] = strdup(s);
}

static void m_unset(const char *name)
{
	size_t l = strlen(name);
	int w = 0;
	for (int r = 0; r < m_n; r++) {
		if (strncmp(m_ent[r], name, l) == 0 && m_ent[r][l] == '=') {
			free(m_ent[r]);
			continue;
		}
		m_ent[w++] = m_ent[r];
	}
	m_n = w;
}

static void m_clear(void)
{
	for (int i = 0; i < m_n; i++)
		free(m_ent[i]);
	m_n = 0;
}

/* Compare everything the interface shows with the model. */
static void verify(const char *what)
{
	char **v = lk_environ;
	int n = 0;

	CHECK(v != NULL, "%s: environ is NULL", what);
	if (!v)
		return;
	while (v[n])
		n++;
	CHECK(n == m_n, "%s: environ has %d entries, model %d", what, n, m_n);
	for (int i = 0; i < n && i < m_n; i++)
		CHECK(strcmp(v[i], m_ent[i]) == 0,
		      "%s: entry %d is '%.60s', model '%.60s'", what, i, v[i],
		      m_ent[i]);

	/* env_count / env_iter: only entries with '=' and a name. */
	int with_eq = 0;
	for (int i = 0; i < m_n; i++) {
		size_t l = namelen(m_ent[i]);
		if (l && m_ent[i][l] == '=')
			with_eq++;
	}
	CHECK(lk_env_count() == with_eq, "%s: env_count %d, model %d", what,
	      lk_env_count(), with_eq);
	int cookie = 0, k = 0, mi = 0;
	const char *en, *ev;
	while (lk_env_iter(&cookie, &en, &ev)) {
		while (mi < m_n && !(namelen(m_ent[mi]) &&
				     m_ent[mi][namelen(m_ent[mi])] == '='))
			mi++;
		if (mi >= m_n) {
			CHECK(0, "%s: env_iter returned too many", what);
			break;
		}
		size_t l = namelen(m_ent[mi]);
		CHECK(strlen(en) == l && strncmp(en, m_ent[mi], l) == 0 &&
			      strcmp(ev, m_ent[mi] + l + 1) == 0,
		      "%s: env_iter item %d differs", what, k);
		mi++;
		k++;
	}
	CHECK(k == with_eq, "%s: env_iter gave %d, model %d", what, k,
	      with_eq);
}

/* getenv agrees with the model's first match, and points into environ. */
static void verify_get(const char *name)
{
	char *g = lk_getenv(name);
	int i = m_find(name);

	if (i < 0) {
		CHECK(g == NULL, "getenv(%s) not NULL", name);
		return;
	}
	CHECK(g && strcmp(g, m_ent[i] + strlen(name) + 1) == 0,
	      "getenv(%s) wrong", name);
	CHECK(g && g == lk_environ[i] + strlen(name) + 1,
	      "getenv(%s) does not point into environ[%d]", name, i);
}

/* ---- Random driving ------------------------------------------------------ */

static uint64_t g_rng = 88172645463325252ULL;
static uint64_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return g_rng;
}

#define NNAMES 1300
static char g_names[NNAMES][40];

static char g_valbuf[8192];
static const char *rand_value(void)
{
	uint64_t r = rnd() % 100;
	size_t len;

	if (r < 5)
		len = 4095;
	else if (r < 15)
		len = 512 + rnd() % 3000;
	else if (r < 25)
		len = 0;
	else
		len = rnd() % 40;
	/* A few distinct contents per length, so values repeat (the pool). */
	char c = (char)('a' + rnd() % 4);
	for (size_t i = 0; i < len; i++)
		g_valbuf[i] = (char)(c + (i % 7 == 6 ? 1 : 0));
	g_valbuf[len] = '\0';
	return g_valbuf;
}

/* Arrays the test installed as environ itself, freed once environ has moved
 * on -- so a stale reference to one is a use after free ASan reports. */
#define OWN_MAX 64
static char **g_own[OWN_MAX];
static char *g_own_strs[OWN_MAX][64];
static int g_own_nstr[OWN_MAX];
static int g_nown;

static void release_own(int keep_current)
{
	int w = 0;
	for (int i = 0; i < g_nown; i++) {
		if (keep_current && g_own[i] == lk_environ) {
			g_own[w] = g_own[i];
			g_own_nstr[w] = g_own_nstr[i];
			memcpy(g_own_strs[w], g_own_strs[i],
			       sizeof(g_own_strs[i]));
			w++;
			continue;
		}
		/* The strings may still be in the environment (copied
		 * pointers): only the ARRAY is the test's to free. */
		free(g_own[i]);
	}
	g_nown = w;
}

/* putenv strings must live as long as they are in the environment; keep
 * them all. */
static char *keep_putenv(const char *s)
{
	return strdup(s);
}

static void random_run(int steps, int names_used)
{
	for (int step = 0; step < steps; step++) {
		const char *name = g_names[rnd() % (uint64_t)names_used];
		uint64_t op = rnd() % 1000;
		char what[64];

		if (op < 450) {
			int ow = (int)(rnd() % 4 != 0);
			const char *val = rand_value();
			CHECK(lk_setenv(name, val, ow) == 0, "setenv");
			m_set(name, val, ow);
			snprintf(what, sizeof(what), "setenv %d", step);
		} else if (op < 600) {
			CHECK(lk_unsetenv(name) == 0, "unsetenv");
			m_unset(name);
			snprintf(what, sizeof(what), "unsetenv %d", step);
		} else if (op < 750) {
			const char *val = rand_value();
			char *s = mk(name, val);
			char *k = keep_putenv(s);
			free(s);
			CHECK(lk_putenv(k) == 0, "putenv");
			m_put(k);
			/* the caller's string itself is in environ */
			int idx = m_find(name);
			CHECK(lk_environ[idx] == k,
			      "putenv did not store the caller's string");
			snprintf(what, sizeof(what), "putenv %d", step);
		} else if (op < 755) {
			lk_clearenv();
			m_clear();
			snprintf(what, sizeof(what), "clearenv %d", step);
		} else if (op < 765 && g_nown < OWN_MAX) {
			/* Replace environ with an array of our own: a few of
			 * the current entries, duplicates, an entry without
			 * '=', and an empty name. */
			int k = (int)(rnd() % 6);
			char **arr = malloc(sizeof(char *) * 64);
			int an = 0, ns = 0;
			for (int i = 0; i < k && i < m_n; i++) {
				int src = (int)(rnd() % (uint64_t)m_n);
				arr[an++] = g_own_strs[g_nown][ns++] =
					strdup(m_ent[src]);
			}
			if (rnd() % 2)
				arr[an++] = g_own_strs[g_nown][ns++] =
					strdup("NOEQUALS");
			if (rnd() % 3 == 0)
				arr[an++] = g_own_strs[g_nown][ns++] =
					strdup("=emptyname");
			arr[an] = NULL;
			g_own[g_nown] = arr;
			g_own_nstr[g_nown] = ns;
			g_nown++;
			lk_environ = arr;
			m_clear();
			for (int i = 0; i < an; i++)
				m_ent[m_n++] = strdup(arr[i]);
			snprintf(what, sizeof(what), "replace %d", step);
		} else if (op < 768) {
			lk_environ = NULL;
			m_clear();
			/* getenv on a NULL environ */
			CHECK(lk_getenv(name) == NULL, "getenv with NULL environ");
			/* the next change must make a valid array again */
			CHECK(lk_setenv(name, "x", 1) == 0, "setenv after NULL");
			m_set(name, "x", 1);
			snprintf(what, sizeof(what), "null %d", step);
		} else {
			verify_get(name);
			continue;
		}
		/* Arrays of ours no longer installed may go now. */
		release_own(1);
		verify(what);
		if (step % 7 == 0)
			verify_get(name);
		if (m_n > MODEL_MAX - 10) {
			lk_clearenv();
			m_clear();
		}
	}
}

/* ---- Fixed checks ----------------------------------------------------------- */

static void test_startup(void)
{
	static char *stack[] = { "A=1", "NOEQ", "B=two", "=x", "A=dup", NULL,
				 (char *)6, (char *)4096, NULL, NULL };

	lk___libc_init_environ(stack);
	CHECK(lk_environ == stack, "environ is not the envp handed over");
	CHECK(lk___libc_auxv == (void *)&stack[6], "auxv not found");
	CHECK(g_malloc_ready == 1, "__malloc_env_ready not called");
	CHECK(lk_getenv("A") && strcmp(lk_getenv("A"), "1") == 0,
	      "getenv A");
	CHECK(lk_getenv("NOEQ") == NULL, "entry without = found");
	CHECK(lk_getenv("") == NULL, "empty name found");
	CHECK(lk_getenv("A=1") == NULL, "name with = found");
	CHECK(lk_getenv(NULL) == NULL, "NULL name");
	CHECK(lk_env_count() == 3, "env_count %d", lk_env_count());

	/* unsetenv removes every duplicate and edits the kernel's array in
	 * place (it is only replaced when it has to grow). */
	CHECK(lk_unsetenv("A") == 0, "unsetenv A");
	CHECK(lk_getenv("A") == NULL, "duplicate A survived");
	CHECK(lk_environ == stack, "unsetenv moved environ");
	CHECK(strcmp(stack[0], "NOEQ") == 0 && strcmp(stack[1], "B=two") == 0 &&
		      strcmp(stack[2], "=x") == 0 && stack[3] == NULL,
	      "unsetenv did not compact in order");
	/* replacing in place */
	CHECK(lk_setenv("B", "three", 1) == 0, "setenv B");
	CHECK(lk_environ == stack && strcmp(stack[1], "B=three") == 0,
	      "overwrite not in place");
	/* growing copies; the kernel's array is left as it was */
	CHECK(lk_setenv("C", "c", 0) == 0, "setenv C");
	CHECK(lk_environ != stack, "grew in place into the kernel array");
	CHECK(stack[3] == NULL, "kernel array written past its end");

	/* invalid names */
	errno = 0;
	CHECK(lk_setenv("", "x", 1) == -1 && errno == EINVAL, "setenv ''");
	errno = 0;
	CHECK(lk_setenv("X=Y", "x", 1) == -1 && errno == EINVAL,
	      "setenv X=Y");
	errno = 0;
	CHECK(lk_setenv(NULL, "x", 1) == -1 && errno == EINVAL,
	      "setenv NULL");
	errno = 0;
	CHECK(lk_unsetenv("") == -1 && errno == EINVAL, "unsetenv ''");
	errno = 0;
	CHECK(lk_unsetenv("A=B") == -1 && errno == EINVAL, "unsetenv A=B");
	errno = 0;
	CHECK(lk_putenv("=v") == -1 && errno == EINVAL, "putenv =v");
	/* putenv without '=' removes */
	CHECK(lk_putenv("C") == 0 && lk_getenv("C") == NULL, "putenv C");

	/* clearenv leaves a valid, empty array */
	lk_clearenv();
	CHECK(lk_environ && lk_environ[0] == NULL, "clearenv");
	CHECK(lk_env_count() == 0, "count after clearenv");
}

static void test_pointer_lifetime(void)
{
	lk_clearenv();
	lk_setenv("KEEP", "first", 1);
	char *p1 = lk_getenv("KEEP");
	lk_setenv("KEEP", "second", 1);
	char *p2 = lk_getenv("KEEP");
	CHECK(strcmp(p1, "first") == 0, "old getenv pointer changed");
	CHECK(strcmp(p2, "second") == 0, "new value");
	lk_unsetenv("KEEP");
	CHECK(strcmp(p2, "second") == 0, "pointer after unsetenv");
	/* the same pair again is the same string (reused, not leaked anew) */
	lk_setenv("KEEP", "first", 1);
	CHECK(lk_getenv("KEEP") == p1, "NAME=VALUE not reused");

	/* putenv: later changes to the caller's string are visible */
	static char mine[] = "MINE=abc";
	lk_putenv(mine);
	mine[5] = 'X';
	CHECK(strcmp(lk_getenv("MINE"), "Xbc") == 0, "putenv string copied");
	lk_unsetenv("MINE");

	/* An array environ has outgrown stays readable: a program (or another
	 * thread inside getenv) may still hold it. */
	lk_clearenv();
	lk_setenv("G0", "x", 1);
	char **old = lk_environ;
	for (int i = 0; i < 100; i++) {
		char nm[16];
		snprintf(nm, sizeof(nm), "G%d", i);
		lk_setenv(nm, "y", 1);
	}
	CHECK(lk_environ != old, "array did not grow");
	int n = 0;
	while (old[n])
		n++;
	CHECK(n >= 1 && strncmp(old[0], "G0=", 3) == 0, "outgrown array changed");
	lk_clearenv();
}

static void test_big(void)
{
	char *val = malloc(4096);
	memset(val, 'v', 4095);
	val[4095] = '\0';

	lk_clearenv();
	m_clear();
	for (int i = 0; i < 1200; i++) {
		val[0] = (char)('A' + i % 26);
		CHECK(lk_setenv(g_names[i], val, 1) == 0, "setenv big %d", i);
		m_set(g_names[i], val, 1);
	}
	verify("1200 x 4095");
	for (int i = 0; i < 1200; i++) {
		char *g = lk_getenv(g_names[i]);
		CHECK(g && strlen(g) == 4095 && g[0] == (char)('A' + i % 26),
		      "big value %d", i);
	}
	/* a long name too */
	char *lname = malloc(5000);
	memset(lname, 'N', 4999);
	lname[4999] = '\0';
	CHECK(lk_setenv(lname, val, 1) == 0, "long name");
	CHECK(lk_getenv(lname) && strlen(lk_getenv(lname)) == 4095,
	      "long name value");
	lk_unsetenv(lname);
	free(lname);
	lk_clearenv();
	m_clear();
	free(val);
}

/* ---- Threads ------------------------------------------------------------------ */

static volatile int g_stop;
static long g_reads;

static void *reader(void *arg)
{
	(void)arg;
	long n = 0;
	while (!g_stop) {
		for (int i = 0; i < 16; i++) {
			char *g = lk_getenv(g_names[i]);
			if (g) {
				/* every value the writer sets is "t" + digits */
				CHECK(g[0] == 't', "thread read '%.20s'", g);
			}
		}
		char **v = lk_environ;
		for (int i = 0; v && v[i]; i++) {
			volatile char c = v[i][0];
			(void)c;
		}
		const char *en, *ev;
		int cookie = 0;
		while (lk_env_iter(&cookie, &en, &ev))
			;
		n++;
	}
	__atomic_fetch_add(&g_reads, n, __ATOMIC_RELAXED);
	return NULL;
}

static void test_threads(void)
{
	pthread_t th[4];
	char val[32];

	lk_clearenv();
	m_clear();
	g_stop = 0;
	for (int i = 0; i < 4; i++)
		pthread_create(&th[i], NULL, reader, NULL);
	for (int it = 0; it < 200000; it++) {
		const char *name = g_names[rnd() % 16];
		switch (rnd() % 4) {
		case 0:
		case 1:
			snprintf(val, sizeof(val), "t%llu",
				 (unsigned long long)(rnd() % 50));
			lk_setenv(name, val, 1);
			break;
		case 2:
			lk_unsetenv(name);
			break;
		default:
			if (rnd() % 64 == 0)
				lk_clearenv();
			else {
				/* grow and shrink the array */
				int top = 16 + (int)(rnd() % 40);
				for (int k = 16; k < top; k++)
					lk_setenv(g_names[k], "t1", 1);
				for (int k = 16; k < 56; k++)
					lk_unsetenv(g_names[k]);
			}
		}
	}
	g_stop = 1;
	for (int i = 0; i < 4; i++)
		pthread_join(th[i], NULL);
	CHECK(g_reads > 0, "readers did not run");

	/* fork hooks: prepare/parent and prepare/child leave it usable */
	lk___env_fork_prepare();
	lk___env_fork_parent();
	lk___env_fork_prepare();
	lk___env_fork_child();
	CHECK(lk_setenv("AFTERFORK", "1", 1) == 0, "lock after fork hooks");
	lk_clearenv();
}

int main(int argc, char **argv)
{
	int steps = argc > 1 ? atoi(argv[1]) : 60000;

	for (int i = 0; i < NNAMES; i++)
		snprintf(g_names[i], sizeof(g_names[i]), "V%d_%c", i,
			 'a' + i % 26);

	test_startup();
	test_pointer_lifetime();
	test_big();

	/* Random runs: a few names (many collisions, duplicates through
	 * putenv/replace), then many (1000+ variables). */
	lk_clearenv();
	m_clear();
	for (int seed = 1; seed <= 4; seed++) {
		g_rng = 0x9e3779b97f4a7c15ULL * (uint64_t)seed;
		random_run(steps, seed % 2 ? 12 : NNAMES);
	}
	release_own(0);
	/* After the arrays are gone the environment must not reach them. */
	lk_clearenv();
	m_clear();
	verify("after release");

	test_threads();

	printf("test-env: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
