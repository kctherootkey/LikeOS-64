// LikeOS -- host unit test of the whole-path code of the VFS layer: the
// path-buffer allocator (kernel/fs/pathbuf.c), path canonicalisation
// (normalize_path / normalize_path_append, kernel/fs/namei.c) and the
// walk's own path work in kernel/fs/vfs.c (vfs_canon, the NAME_MAX check,
// vfs_hop_join, vfs_abspath, the walk's buffers), at and around the limit:
// paths of 4095, 4096 and 4097 bytes, names of 255 and 256 bytes.
//
// Built by host/test-vfs-path.sh against the filesystem harness's kernel
// services (host/test-fs-stubs.c, prelude), with AddressSanitizer and
// UndefinedBehaviorSanitizer.  kernel/fs/vfs.c is included here so that its
// static helpers can be called; what it calls of the rest of the kernel and
// is not exercised stays unresolved at link time.
//
// Copyright (C) 2026 The LikeOS Project

#include "kernel/fs/vfs.c"
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/ke/task_path.h>

extern long test_kalloc_live;

void test_task_init(uint32_t uid, uint32_t gid);
extern long test_warnings;

static int failures, checks;

#define CHECK(c)                                                        \
	do {                                                            \
		checks++;                                               \
		if (!(c)) {                                             \
			h_printf("FAIL line %d: %s\n", __LINE__, #c);   \
			failures++;                                     \
		}                                                       \
	} while (0)

static unsigned long slen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static int seq(const char *a, const char *b)
{
	return kstrcmp(a, b) == 0;
}

/* "/aaa.../aaa..." of exactly `len' bytes: names of `seg' bytes (the last
 * one shorter). */
static char *mkpath(char *out, unsigned long len, unsigned long seg, char c)
{
	unsigned long n = 0;

	while (n < len) {
		out[n++] = '/';
		for (unsigned long k = 0; k < seg && n < len; k++)
			out[n++] = c;
	}
	/* a path must not end in the slash just written */
	if (out[len - 1] == '/')
		out[len - 1] = c;
	out[len] = 0;
	return out;
}

static char big1[3 * VFS_MAX_PATH], big2[3 * VFS_MAX_PATH],
	big3[3 * VFS_MAX_PATH];

static void test_pathbuf(void)
{
	char *b[100];
	unsigned long base = pathbuf_in_use();

	for (int i = 0; i < 100; i++) {
		b[i] = pathbuf_get();
		CHECK(b[i] != NULL);
		if (b[i]) {
			/* the whole buffer is ours */
			mm_memset(b[i], 'q', PATHBUF_SIZE);
			b[i][PATHBUF_SIZE - 1] = 0;
		}
	}
	CHECK(pathbuf_in_use() == base + 100);
	for (int i = 0; i < 100; i++)
		for (int j = i + 1; j < 100; j++)
			CHECK(b[i] != b[j]);
	for (int i = 0; i < 100; i++)
		pathbuf_put(b[i]);
	pathbuf_put(NULL);
	CHECK(pathbuf_in_use() == base);
	/* reuse after put */
	char *x = pathbuf_get();
	CHECK(x != NULL);
	pathbuf_put(x);
	CHECK(pathbuf_in_use() == base);
}

static void test_normalize(void)
{
	char *out = pathbuf_get();
	size_t len;

	CHECK(out != NULL);
	/* ordinary paths */
	CHECK(normalize_path("/", "//a//b/./c/..", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/a/b"));
	CHECK(normalize_path("/x/y", "../z/", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/x/z"));
	CHECK(normalize_path("/", "../../..", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/"));
	CHECK(normalize_path("/a", ".", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/a"));
	CHECK(normalize_path("/a/b", "/c", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/c"));
	CHECK(normalize_path(NULL, "rel", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/rel"));
	CHECK(normalize_path("/", "x", out, 1) == -EINVAL);

	/* absolute: 4095 bytes fit exactly, 4096 do not */
	mkpath(big1, 4095, 200, 'a');
	CHECK(normalize_path("/", big1, out, PATHBUF_SIZE) == 0 &&
	      seq(out, big1) && slen(out) == 4095);
	mkpath(big1, 4096, 200, 'a');
	CHECK(normalize_path("/", big1, out, PATHBUF_SIZE) == -ENAMETOOLONG);
	mkpath(big1, 4097, 200, 'a');
	CHECK(normalize_path("/", big1, out, PATHBUF_SIZE) == -ENAMETOOLONG);

	/* names: 255 bytes fine, 256 refused (never cut) */
	big1[0] = '/';
	for (int i = 1; i <= 255; i++)
		big1[i] = 'n';
	big1[256] = 0;
	CHECK(normalize_path("/", big1, out, PATHBUF_SIZE) == 0 &&
	      slen(out) == 256);
	big1[256] = 'n';
	big1[257] = 0;
	CHECK(normalize_path("/", big1, out, PATHBUF_SIZE) == -ENAMETOOLONG);

	/* many components: more than any fixed table (2047 of them) */
	{
		unsigned long n = 0;
		while (n + 2 <= 4095) {
			big1[n++] = '/';
			big1[n++] = 'c';
		}
		big1[n] = 0;
		CHECK(normalize_path("/", big1, out, PATHBUF_SIZE) == 0 &&
		      seq(out, big1));
	}

	/* relative against a long working directory: only the result has to
	 * fit, not base + path joined */
	mkpath(big2, 4000, 200, 'w');
	CHECK(normalize_path(big2, "x", out, PATHBUF_SIZE) == 0 &&
	      slen(out) == 4002);
	/* 4000 + "/" + 94 = 4095: fits */
	for (int i = 0; i < 94; i++)
		big3[i] = 'r';
	big3[94] = 0;
	CHECK(normalize_path(big2, big3, out, PATHBUF_SIZE) == 0 &&
	      slen(out) == 4095);
	big3[94] = 'r';
	big3[95] = 0;
	CHECK(normalize_path(big2, big3, out, PATHBUF_SIZE) == -ENAMETOOLONG);
	/* joined 8000 bytes, but ".." takes it back below the limit */
	{
		unsigned long n = 0;
		for (int k = 0; k < 20; k++) { /* the base has 20 names */
			big3[n++] = '.';
			big3[n++] = '.';
			big3[n++] = '/';
		}
		mkpath(big3 + n, 3500, 200, 'z');
		/* (mkpath starts with '/': "../" + "/zzz" is fine) */
		CHECK(normalize_path(big2, big3, out, PATHBUF_SIZE) == 0 &&
		      slen(out) == 3500);
	}

	/* normalize_path_append with a bounded piece */
	out[0] = '/';
	out[1] = 0;
	len = 1;
	CHECK(normalize_path_append(out, PATHBUF_SIZE, &len, "abc/def", 5) ==
		      0 &&
	      seq(out, "/abc/d") && len == 6);
	CHECK(normalize_path_append(out, PATHBUF_SIZE, &len, "../..",
				    (size_t)-1) == 0 &&
	      seq(out, "/") && len == 1);
	pathbuf_put(out);
}

static void test_canon(void)
{
	char *buf = pathbuf_get(), *cb;
	const char *r;

	CHECK(buf != NULL);
	CHECK(vfs_canon("/a/b", buf) != buf); /* canonical: as it is */
	r = vfs_canon("/a//b/./c/../", buf);
	CHECK(r == buf && seq(buf, "/a/b/")); /* trailing slash kept */
	r = vfs_canon("rel/../x", buf);
	CHECK(r && seq(r, "rel/../x")); /* relative: not touched */
	mkpath(big1, 4093, 200, 'a');
	big1[4093] = '/';
	big1[4094] = '.';
	big1[4095] = 0;
	r = vfs_canon(big1, buf);
	CHECK(r == buf && slen(buf) == 4093);

	/* vfs_canon_get takes a buffer only when one is needed */
	unsigned long base = pathbuf_in_use();
	r = vfs_canon_get("/plain/path", &cb);
	CHECK(cb == NULL && seq(r, "/plain/path") && pathbuf_in_use() == base);
	r = vfs_canon_get("/odd//path", &cb);
	CHECK(cb != NULL && seq(r, "/odd/path") &&
	      pathbuf_in_use() == base + 1);
	pathbuf_put(cb);
	CHECK(pathbuf_in_use() == base);

	/* NAME_MAX, checked once for every filesystem */
	big1[0] = '/';
	for (int i = 1; i <= 255; i++)
		big1[i] = 'n';
	big1[256] = 0;
	CHECK(!vfs_name_too_long(big1));
	big1[256] = 'n';
	big1[257] = 0;
	CHECK(vfs_name_too_long(big1));
	CHECK(!vfs_name_too_long("relative/name"));
	pathbuf_put(buf);
}

/* The component `name' of path `s' (after a slash, before a slash or the
 * end). */
static const char *find(const char *s, const char *name)
{
	unsigned long n = slen(name);

	for (const char *p = s; *p; p++)
		if (p[0] == '/' && kstrncmp(p + 1, name, n) == 0 &&
		    (p[1 + n] == '/' || p[1 + n] == 0))
			return p + 1;
	return NULL;
}

static const char *last_slash(const char *s)
{
	const char *r = NULL;

	for (; *s; s++)
		if (*s == '/')
			r = s;
	return r;
}

static void copy(char *d, const char *s)
{
	while ((*d++ = *s++))
		;
}

/* vfs_hop_join through a walk as vfs_link_hop sets it up. */
static struct vfs_walk g_hw;

static int hop(task_t *cur, const char *op, const char *link_at,
	       const char *target, char **res)
{
	unsigned long cs = (unsigned long)(find(op, link_at) - op);
	unsigned long ce = cs + slen(link_at);
	int r;

	pathbuf_put(g_hw.next);
	g_hw.next = pathbuf_get();
	r = vfs_hop_join(&g_hw, cur, op, cs, ce, target);
	*res = g_hw.next;
	return r;
}

static void test_hop(task_t *cur)
{
	char *res;
	unsigned long base = pathbuf_in_use();

	task_path_set(cur, TASK_PATH_ROOT, NULL);
	CHECK(hop(cur, "/a/l/b", "l", "x/y", &res) == ST_OK &&
	      seq(res, "/a/x/y/b"));
	CHECK(hop(cur, "/a/l/b", "l", "/abs", &res) == ST_OK &&
	      seq(res, "/abs/b"));
	CHECK(hop(cur, "/a/l/", "l", "../t", &res) == ST_OK &&
	      seq(res, "/t/")); /* trailing slash kept */
	CHECK(hop(cur, "/l", "l", "/", &res) == ST_OK && seq(res, "/"));
	/* a jailed task: absolute targets from its root */
	task_path_set(cur, TASK_PATH_ROOT, task_path_new("/jail"));
	CHECK(hop(cur, "/jail/d/l/f", "l", "/etc", &res) == ST_OK &&
	      seq(res, "/jail/etc/f"));
	CHECK(hop(cur, "/jail/d/l", "l", "../../../..", &res) == ST_OK &&
	      seq(res, "/jail"));
	task_path_set(cur, TASK_PATH_ROOT, NULL);

	/* long: the link's directory is ~3900 bytes, the target ~3000 bytes
	 * of "../x/" pairs -- joined over 6900, leading to ~3900 */
	mkpath(big1, 3900, 200, 'd');
	{
		unsigned long n = slen(big1);
		const char *last = last_slash(big1) + 1;
		unsigned long ll = slen(last);
		unsigned long t = 0;

		big1[n++] = '/';
		big1[n++] = 'L';
		big1[n] = 0;
		while (t + 4 + ll < 3000) {
			big2[t++] = '.';
			big2[t++] = '.';
			big2[t++] = '/';
			for (unsigned long k = 0; k < ll; k++)
				big2[t++] = last[k];
			big2[t++] = '/';
		}
		big2[t++] = 'f';
		big2[t] = 0;
		CHECK(hop(cur, big1, "L", big2, &res) == ST_OK);
		/* the result: the directory, then "f" */
		big1[n - 2] = 0;
		CHECK(slen(res) == slen(big1) + 2 && res[slen(big1)] == '/' &&
		      res[slen(big1) + 1] == 'f');
	}
	/* a result that does not fit: ENAMETOOLONG, nothing cut */
	mkpath(big1, 4000, 200, 'd');
	{
		unsigned long n = slen(big1);
		big1[n++] = '/';
		big1[n++] = 'L';
		big1[n] = 0;
		mkpath(big2, 200, 200, 'e');
		CHECK(hop(cur, big1, "L", big2 + 1, &res) == ST_NAMETOOLONG);
	}
	/* the hop's own buffer comes back too */
	pathbuf_put(g_hw.next);
	g_hw.next = NULL;
	CHECK(pathbuf_in_use() == base);
}

static void test_abspath(task_t *cur)
{
	char *buf;
	const char *r;
	int err = 0;
	unsigned long base = pathbuf_in_use();

	task_path_set(cur, TASK_PATH_CWD, NULL);
	r = vfs_abspath("/abs", &buf, &err);
	CHECK(r && seq(r, "/abs") && buf == NULL);
	r = vfs_abspath("rel", &buf, &err);
	CHECK(r && seq(r, "/rel") && buf);
	pathbuf_put(buf);
	mkpath(big2, 4000, 200, 'c');
	task_path_set(cur, TASK_PATH_CWD, task_path_new(big2));
	for (int i = 0; i < 94; i++)
		big3[i] = 'r';
	big3[94] = 0;
	r = vfs_abspath(big3, &buf, &err); /* 4000 + 1 + 94 = 4095 */
	CHECK(r && slen(r) == 4095);
	pathbuf_put(buf);
	big3[94] = 'r';
	big3[95] = 0;
	err = 0;
	r = vfs_abspath(big3, &buf, &err); /* 4096: refused, not cut */
	CHECK(r == NULL && buf == NULL && err == ST_NAMETOOLONG);
	task_path_set(cur, TASK_PATH_CWD, NULL);
	CHECK(pathbuf_in_use() == base);
}

/* The walk itself: canonical paths cost nothing, odd ones a buffer, and
 * everything goes back at the end. */
static int walk_probe(const char *path)
{
	return path ? (int)slen(path) : -1;
}

static void test_walk(void)
{
	struct vfs_walk w;
	const char *p;
	int r;
	unsigned long base = pathbuf_in_use();

	p = "/plain";
	VFS_WALK(w, p, 1, r, walk_probe(p));
	CHECK(r == 6 && p == NULL && pathbuf_in_use() == base);
	p = "/a//b/../c";
	VFS_WALK(w, p, 1, r, walk_probe(p));
	CHECK(r == 4 && pathbuf_in_use() == base);
	/* a 256-byte name: the operation never runs */
	big1[0] = '/';
	for (int i = 1; i <= 256; i++)
		big1[i] = 'n';
	big1[257] = 0;
	p = big1;
	VFS_WALK(w, p, 1, r, walk_probe(p));
	CHECK(r == ST_NAMETOOLONG && pathbuf_in_use() == base);
	/* VFS_WALK_RUN keeps the path until vfs_walk_free */
	p = "/x/./y";
	VFS_WALK_RUN(w, p, 1, r, walk_probe(p));
	CHECK(r == 4 && p && seq(p, "/x/y") && pathbuf_in_use() == base + 1);
	vfs_walk_free(&w);
	CHECK(pathbuf_in_use() == base);
	CHECK(sched_current()->vfs_walk == NULL);
}

/* The task's paths (kernel/ke/task_path.c): shared by reference across
 * fork, private per task, a reader's reference outliving a chdir and the
 * task's end (AddressSanitizer would catch a use after free), nothing
 * left behind. */
static void test_task_paths(task_t *cur)
{
	static task_t child, thread;
	long live = test_kalloc_live;
	struct task_path *r1, *r2, *e;

	task_path_set(cur, TASK_PATH_CWD, NULL);
	CHECK(seq(task_cwd(cur), "/") && seq(task_root(cur), "") &&
	      seq(task_exe(cur), ""));
	CHECK(task_path_ref(cur, TASK_PATH_CWD) == NULL);

	mkpath(big1, 4095, 200, 'p');
	task_path_set(cur, TASK_PATH_CWD, task_path_new(big1));
	task_path_set(cur, TASK_PATH_EXE, task_path_new("/bin/prog"));
	CHECK(seq(task_cwd(cur), big1) && slen(task_cwd(cur)) == 4095);
	mkpath(big3, 4096, 200, 'q');
	CHECK(task_path_new(big3) == NULL); /* longer than any path */

	/* fork: the copy forgets, then shares by reference */
	mm_memcpy(&child, cur, sizeof(child));
	task_paths_clear(&child);
	CHECK(child.fs_cwd == NULL && child.fs_exe == NULL);
	task_paths_fork(&child, cur);
	CHECK(child.fs_cwd == cur->fs_cwd && child.fs_cwd->refs == 2);
	CHECK(seq(task_exe(&child), "/bin/prog"));
	/* a second task (a thread) the same way */
	mm_memcpy(&thread, cur, sizeof(thread));
	task_paths_clear(&thread);
	task_paths_fork(&thread, cur);
	CHECK(cur->fs_cwd->refs == 3);

	/* every task's own: chdir in the child changes nothing else */
	task_path_set(&child, TASK_PATH_CWD, task_path_new("/elsewhere"));
	CHECK(seq(task_cwd(&child), "/elsewhere"));
	CHECK(seq(task_cwd(cur), big1) && seq(task_cwd(&thread), big1));
	CHECK(cur->fs_cwd->refs == 2);

	/* a reader's reference outlives the chdir and the task's end */
	r1 = task_path_ref(&thread, TASK_PATH_CWD);
	r2 = task_path_ref(&child, TASK_PATH_CWD);
	e = task_path_ref(&child, TASK_PATH_EXE);
	task_path_set(&thread, TASK_PATH_CWD, task_path_new("/t"));
	task_paths_release(&child);
	CHECK(child.fs_cwd == NULL && child.fs_exe == NULL);
	CHECK(r1 && seq(r1->s, big1) && r1->len == 4095);
	CHECK(r2 && seq(r2->s, "/elsewhere"));
	CHECK(e && seq(e->s, "/bin/prog"));
	task_path_put(r1);
	task_path_put(r2);
	task_path_put(e);
	CHECK(task_path_ref(&child, TASK_PATH_CWD) == NULL);
	task_paths_release(&thread);
	task_paths_release(&thread); /* twice: nothing left to drop */

	/* the jail root */
	task_path_set(cur, TASK_PATH_ROOT, task_path_new("/jail"));
	CHECK(seq(task_root(cur), "/jail"));
	task_path_set(cur, TASK_PATH_ROOT, NULL);
	CHECK(seq(task_root(cur), ""));

	task_path_set(cur, TASK_PATH_CWD, NULL);
	task_path_set(cur, TASK_PATH_EXE, NULL);
	CHECK(test_kalloc_live == live); /* every string freed */
}

/* The syscall layer's path building: "" is ENOENT, the chroot root goes in
 * front once, a descriptor's real path is taken back into the jail. */
static void test_build(task_t *cur)
{
	char *out = pathbuf_get();
	int in;
	const char *r;

	CHECK(build_at_path(cur, AT_FDCWD, "", out, PATHBUF_SIZE) == -ENOENT);
	big1[0] = 0;
	CHECK(canon_task_path(big1, PATHBUF_SIZE) == -ENOENT);
	CHECK(build_at_path(cur, AT_FDCWD, "x", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/x"));
	task_path_set(cur, TASK_PATH_ROOT, task_path_new("/jail"));
	task_path_set(cur, TASK_PATH_CWD, task_path_new("/a"));
	CHECK(build_at_path(cur, AT_FDCWD, "x", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/jail/a/x"));
	CHECK(build_at_path(cur, AT_FDCWD, "../../..", out, PATHBUF_SIZE) ==
		      0 &&
	      seq(out, "/jail"));
	CHECK(build_at_path(cur, AT_FDCWD, "/etc", out, PATHBUF_SIZE) == 0 &&
	      seq(out, "/jail/etc"));
	r = jail_relative(cur, "/jail/d/e", &in);
	CHECK(in && seq(r, "/d/e"));
	r = jail_relative(cur, "/jail", &in);
	CHECK(in && seq(r, "/"));
	r = jail_relative(cur, "/jailbreak/x", &in);
	CHECK(!in && seq(r, "/jailbreak/x"));
	r = jail_relative(cur, "/other", &in);
	CHECK(!in && seq(r, "/other"));
	task_path_set(cur, TASK_PATH_ROOT, NULL);
	r = jail_relative(cur, "/jail/d", &in);
	CHECK(in && seq(r, "/jail/d"));
	task_path_set(cur, TASK_PATH_CWD, NULL);
	pathbuf_put(out);
}

int main(void)
{
	test_task_init(0, 0);
	task_t *cur = sched_current();
	unsigned long base;

	pathbuf_init();
	base = pathbuf_in_use();
	test_pathbuf();
	test_normalize();
	test_canon();
	test_hop(cur);
	test_abspath(cur);
	test_walk();
	test_task_paths(cur);
	test_build(cur);
	CHECK(pathbuf_in_use() == base);
	CHECK(test_warnings == 0);
	h_printf("test-vfs-path: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
