/*
 * The process environment: environ, getenv, setenv, unsetenv, putenv,
 * clearenv, and the start-up code that hands the kernel's envp[] to them.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

/*
 * `environ` IS the environment.  There is no second copy behind it: getenv
 * reads it, setenv/putenv/unsetenv edit it, and execv passes it to the
 * kernel as it stands.  That is the only arrangement that stays right when a
 * program reaches past these functions -- a shell or GLib iterating environ,
 * a program assigning environ an array of its own, or editing entries in
 * place -- because whatever the program did is exactly what the next getenv
 * and the next exec see.  There is no fixed limit on the number of
 * variables or on the length of a value.
 *
 * Storage:
 *  - At start-up environ is the kernel's envp[] on the initial stack.
 *  - The first change that needs a longer array copies the entries into one
 *    allocated here ("owned"), with room to grow.  An owned array outgrown
 *    is NOT freed: another thread may be walking it in getenv right now, and
 *    a program may still hold the old pointer.  The arrays grow by doubling,
 *    so everything ever left behind is less than the array in use.
 *  - A string made by setenv is never freed either, for the same reason:
 *    getenv hands out pointers into it, and code that keeps such a pointer
 *    across a later setenv of the same name is common.  Instead every string
 *    setenv ever made is kept in a hash set and reused when the same
 *    NAME=VALUE is set again, so a program that switches a variable between
 *    a few values does not grow; memory is bounded by the number of distinct
 *    NAME=VALUE pairs a process ever sets.
 *  - putenv stores the caller's string itself, as POSIX requires: later
 *    changes to that string change the environment.
 *  - An array the program installed itself (or the kernel's) is edited in
 *    place for replacements and removals, and copied into an owned array
 *    when it has to grow; it is never realloc'd or freed here.
 *
 * Locking: every function that changes the environment holds env_lock.
 * getenv takes no lock (it is called from signal handlers and in hot loops
 * by ported code); because nothing it can reach is ever freed, a getenv that
 * races a change may miss the change but never reads freed memory.  Writers
 * publish in an order that keeps the array NULL-terminated at every moment.
 */

#include <elf.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The empty environment: what environ is before start-up and after clearenv
 * when there is no owned array to reuse.  Never written. */
static char *env_empty[1] = { (char *)0 };
char **environ = env_empty;

/* The array allocated here most recently, and its capacity in entries
 * (including the terminating NULL).  Only ever extended in place while
 * environ still points at it. */
static char **env_owned;
static size_t env_owned_cap;

/* Every NAME=VALUE string setenv made, open addressing, for reuse. */
static char **env_pool;
static size_t env_pool_cap; /* a power of two, or 0 */
static size_t env_pool_used;

static volatile int env_lock;

static void env_lock_take(void)
{
	unsigned spins = 0;

	while (__atomic_exchange_n(&env_lock, 1, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&env_lock, __ATOMIC_RELAXED)) {
			/* The holder may be inside malloc; give it the CPU
			 * rather than spinning for a whole time slice. */
			if (++spins >= 64) {
				spins = 0;
				sched_yield();
			} else {
				__asm__ volatile("pause" ::: "memory");
			}
		}
	}
}

static void env_lock_drop(void)
{
	__atomic_store_n(&env_lock, 0, __ATOMIC_RELEASE);
}

/* fork() holds the lock across the system call (see there), so the child
 * never inherits it held by a thread that does not exist in the child. */
void __env_fork_prepare(void)
{
	env_lock_take();
}

void __env_fork_parent(void)
{
	env_lock_drop();
}

void __env_fork_child(void)
{
	__atomic_store_n(&env_lock, 0, __ATOMIC_RELAXED);
}

static char **env_load(void)
{
	return __atomic_load_n(&environ, __ATOMIC_ACQUIRE);
}

static void env_publish(char **v)
{
	__atomic_store_n(&environ, v, __ATOMIC_RELEASE);
}

static void env_store(char **slot, char *s)
{
	__atomic_store_n(slot, s, __ATOMIC_RELEASE);
}

/* Length of a variable name: up to '=' or the end.  A NAME that is empty or
 * contains '=' is invalid for every function here. */
static size_t env_namelen(const char *name)
{
	size_t n = 0;

	while (name[n] && name[n] != '=')
		n++;
	return n;
}

/* Does entry `e` define the variable `name` of length `nlen`? */
static int env_match(const char *e, const char *name, size_t nlen)
{
	return e && strncmp(e, name, nlen) == 0 && e[nlen] == '=';
}

/* ---- The string pool ------------------------------------------------------ */

static uint64_t env_hash_bytes(uint64_t h, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		h ^= (unsigned char)p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

#define ENV_HASH_SEED 0xcbf29ce484222325ULL

static uint64_t env_hash_pair(const char *name, size_t nlen,
			      const char *value, size_t vlen)
{
	uint64_t h = env_hash_bytes(ENV_HASH_SEED, name, nlen);

	h = env_hash_bytes(h, "=", 1);
	return env_hash_bytes(h, value, vlen);
}

static char *env_pool_find(const char *name, size_t nlen, const char *value,
			   uint64_t h)
{
	if (!env_pool_cap)
		return NULL;
	size_t mask = env_pool_cap - 1;
	for (size_t i = (size_t)h & mask;; i = (i + 1) & mask) {
		char *s = env_pool[i];
		if (!s)
			return NULL;
		if (env_match(s, name, nlen) &&
		    strcmp(s + nlen + 1, value) == 0)
			return s;
	}
}

static void env_pool_put(char **tab, size_t cap, char *s, uint64_t h)
{
	size_t mask = cap - 1;
	size_t i = (size_t)h & mask;

	while (tab[i])
		i = (i + 1) & mask;
	tab[i] = s;
}

/* Remember `s` for reuse.  A pool that cannot grow just stops deduplicating;
 * the string itself is still good. */
static void env_pool_add(char *s, uint64_t h)
{
	if ((env_pool_used + 1) * 2 > env_pool_cap) {
		size_t ncap = env_pool_cap ? env_pool_cap * 2 : 64;
		char **ntab = calloc(ncap, sizeof(*ntab));
		if (!ntab)
			return;
		for (size_t i = 0; i < env_pool_cap; i++)
			if (env_pool[i])
				env_pool_put(ntab, ncap, env_pool[i],
					     env_hash_bytes(ENV_HASH_SEED,
							    env_pool[i],
							    strlen(env_pool[i])));
		free(env_pool);
		env_pool = ntab;
		env_pool_cap = ncap;
	}
	env_pool_put(env_pool, env_pool_cap, s, h);
	env_pool_used++;
}

/* "NAME=VALUE", made once and shared by every setenv of the same pair. */
static char *env_make(const char *name, size_t nlen, const char *value)
{
	size_t vlen = strlen(value);
	uint64_t h = env_hash_pair(name, nlen, value, vlen);
	char *s = env_pool_find(name, nlen, value, h);

	if (s)
		return s;
	if (nlen > SIZE_MAX - 2 - vlen)
		return NULL;
	s = malloc(nlen + vlen + 2);
	if (!s)
		return NULL;
	memcpy(s, name, nlen);
	s[nlen] = '=';
	memcpy(s + nlen + 1, value, vlen + 1);
	env_pool_add(s, h);
	return s;
}

/* ---- Editing the array ----------------------------------------------------- */

/* Install `s` for the variable `name`: over the first entry that defines it
 * if there is one, appended otherwise.  Called with the lock held.  Returns
 * 0, or -1 with errno ENOMEM. */
static int env_install(char *s, const char *name, size_t nlen)
{
	char **v = env_load();
	size_t n = 0;

	if (v) {
		for (; v[n]; n++) {
			if (env_match(v[n], name, nlen)) {
				env_store(&v[n], s);
				return 0;
			}
		}
	}

	/* Append.  In place only in the owned array and only while there is
	 * room: the NULL goes in one slot further first, so the array is
	 * terminated whichever of the two stores a reader sees. */
	if (v && v == env_owned && n + 2 <= env_owned_cap) {
		env_store(&v[n + 1], NULL);
		env_store(&v[n], s);
		return 0;
	}

	size_t ncap = (n + 2) * 2;
	if (ncap < 16)
		ncap = 16;
	if (ncap > SIZE_MAX / sizeof(char *)) {
		errno = ENOMEM;
		return -1;
	}
	char **nv = malloc(ncap * sizeof(char *));
	if (!nv) {
		errno = ENOMEM;
		return -1;
	}
	if (n)
		memcpy(nv, v, n * sizeof(char *));
	nv[n] = s;
	nv[n + 1] = NULL;
	/* The array outgrown is left alone -- see the top of the file. */
	env_owned = nv;
	env_owned_cap = ncap;
	env_publish(nv);
	return 0;
}

/* Is there an entry for `name`?  Called with the lock held. */
static int env_present(const char *name, size_t nlen)
{
	char **v = env_load();

	if (v)
		for (size_t i = 0; v[i]; i++)
			if (env_match(v[i], name, nlen))
				return 1;
	return 0;
}

/* Drop every entry for `name`, keeping the order of the rest.  Called with
 * the lock held. */
static void env_remove(const char *name, size_t nlen)
{
	char **v = env_load();
	size_t r, w = 0;

	if (!v)
		return;
	for (r = 0; v[r]; r++) {
		char *e = v[r];
		if (env_match(e, name, nlen))
			continue;
		if (w != r)
			env_store(&v[w], e);
		w++;
	}
	if (w != r)
		env_store(&v[w], NULL);
}

/* ---- The interface --------------------------------------------------------- */

char *getenv(const char *name)
{
	if (!name)
		return NULL;
	size_t nlen = env_namelen(name);
	if (nlen == 0 || name[nlen] != '\0')
		return NULL;

	char **v = env_load();
	if (!v)
		return NULL;
	for (size_t i = 0;; i++) {
		char *e = __atomic_load_n(&v[i], __ATOMIC_ACQUIRE);
		if (!e)
			return NULL;
		if (env_match(e, name, nlen))
			return e + nlen + 1;
	}
}

int setenv(const char *name, const char *value, int overwrite)
{
	if (!name || !value) {
		errno = EINVAL;
		return -1;
	}
	size_t nlen = env_namelen(name);
	if (nlen == 0 || name[nlen] != '\0') {
		errno = EINVAL;
		return -1;
	}

	env_lock_take();
	int r = 0;
	if (overwrite || !env_present(name, nlen)) {
		char *s = env_make(name, nlen, value);
		if (!s) {
			errno = ENOMEM;
			r = -1;
		} else {
			r = env_install(s, name, nlen);
		}
	}
	env_lock_drop();
	return r;
}

int unsetenv(const char *name)
{
	if (!name) {
		errno = EINVAL;
		return -1;
	}
	size_t nlen = env_namelen(name);
	if (nlen == 0 || name[nlen] != '\0') {
		errno = EINVAL;
		return -1;
	}

	env_lock_take();
	env_remove(name, nlen);
	env_lock_drop();
	return 0;
}

int putenv(char *string)
{
	if (!string) {
		errno = EINVAL;
		return -1;
	}
	size_t nlen = env_namelen(string);
	if (string[nlen] != '=') {
		/* No '=': remove the variable, as the C libraries this code
		 * is usually written against do. */
		return unsetenv(string);
	}
	if (nlen == 0) {
		errno = EINVAL;
		return -1;
	}

	env_lock_take();
	int r = env_install(string, string, nlen);
	env_lock_drop();
	return r;
}

int clearenv(void)
{
	env_lock_take();
	/* Reuse the owned array when environ is still it -- the usual
	 * clearenv-then-setenv sequence then allocates nothing -- and fall back
	 * to the static empty one otherwise.  environ is never left NULL:
	 * programs iterate it without checking. */
	char **v = env_load();
	if (v && v == env_owned)
		env_store(&v[0], NULL);
	else
		env_publish(env_empty);
	env_lock_drop();
	return 0;
}

/* Walk the environment as name/value pairs (see <stdlib.h>).  The name is
 * copied into a per-thread buffer, valid until the thread's next call; the
 * value points into the entry itself.  Entries without '=' are skipped. */
static __thread char *env_iter_buf;
static __thread size_t env_iter_cap;

int env_iter(int *cookie, const char **name, const char **value)
{
	if (!cookie || !name || !value || *cookie < 0)
		return 0;

	char **v = env_load();
	if (!v)
		return 0;

	/* The cookie is an index; never step past the terminator, even when
	 * the array shrank between two calls. */
	size_t idx = 0;
	while (idx < (size_t)*cookie && v[idx])
		idx++;
	if (idx < (size_t)*cookie)
		return 0;

	for (; v[idx]; idx++) {
		const char *e = v[idx];
		size_t nlen = env_namelen(e);
		if (e[nlen] != '=' || nlen == 0)
			continue;
		if (nlen + 1 > env_iter_cap) {
			char *b = realloc(env_iter_buf, nlen + 1);
			if (!b)
				return 0;
			env_iter_buf = b;
			env_iter_cap = nlen + 1;
		}
		memcpy(env_iter_buf, e, nlen);
		env_iter_buf[nlen] = '\0';
		*name = env_iter_buf;
		*value = e + nlen + 1;
		*cookie = (int)(idx + 1);
		return 1;
	}
	return 0;
}

int env_count(void)
{
	char **v = env_load();
	int n = 0;

	if (v)
		for (size_t i = 0; v[i]; i++) {
			size_t nlen = env_namelen(v[i]);
			if (nlen && v[i][nlen] == '=')
				n++;
		}
	return n;
}

/* ---- Start-up ------------------------------------------------------------- */

/* The allocator reads its tunables from the environment, and until this
 * function runs there is no environment to read -- see __malloc_env_ready(). */
extern void __malloc_env_ready(void);

/* The auxiliary vector follows the environment's terminating NULL on the
 * initial stack; found once here for getauxval(). */
Elf64_auxv_t *__libc_auxv;

/*
 * Called from _start (crt0.S, crt1.S) before main() with the envp[] the
 * kernel placed on the stack.  environ becomes that array itself: it is
 * NULL-terminated, lives as long as the process, and is what main() gets
 * as its third argument too.
 *
 * Shared-library constructors run before this, so a setenv made in one is
 * replaced here by the environment the process was started with.
 */
void __libc_init_environ(char **envp)
{
	if (!envp) {
		env_publish(env_empty);
		return;
	}
	{
		char **p = envp;
		while (*p)
			p++;
		__libc_auxv = (Elf64_auxv_t *)(p + 1);
	}
	env_publish(envp);
	__malloc_env_ready();
}
