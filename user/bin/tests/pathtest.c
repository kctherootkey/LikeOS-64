// LikeOS - pathtest: path names up to PATH_MAX on every filesystem
//
// Copyright (C) 2026 The LikeOS Project

/*
 * NAME
 *   pathtest - check path names of up to 4095 bytes (PATH_MAX with the NUL)
 *
 * SYNOPSIS
 *   pathtest DIR [DIR...]
 *
 * DESCRIPTION
 *   For every DIR (e.g. /tmp, /ram, a directory on the root filesystem, a
 *   mounted FAT, exFAT or NTFS volume, a mounted ISO image) a work directory
 *   pathtest.<pid>.<n> is made inside it and the following is checked; every
 *   check prints a PASS, FAIL or INFO line:
 *
 *    1. a tree of directories made with mkdir and absolute paths, names of
 *       200 to 255 bytes, the deepest path 3980 bytes; a name of 255 bytes
 *       works and one of 256 fails with ENAMETOOLONG; a path of exactly
 *       4095 bytes works and one of 4096 fails with ENAMETOOLONG; readdir
 *       lists every long name.
 *    2. open/write/read/stat/lstat/access/chmod/utimensat/truncate/rename/
 *       unlink of a file at a 4000-byte path; the same with the *at() calls
 *       relative to directory descriptors at the bottom and the top.
 *    3. chdir down the tree one relative name at a time; getcwd returns the
 *       whole path (ERANGE with a buffer too small, getcwd(NULL, 0));
 *       relative calls from the deep working directory; ".." back up.
 *    4. symbolic links with long targets (absolute 3980 bytes, exactly 4095
 *       bytes, a relative one climbing back to the top, one of many "../"
 *       near the limit), readlink, following, chdir and getcwd through a
 *       link, a chain of links, ELOOP.  Skipped (INFO) where the filesystem
 *       has no symbolic links.
 *    5. rename of files and directories between the top and the bottom; a
 *       new name of 4096 bytes fails with ENAMETOOLONG; a directory moved
 *       so that the paths below it would pass the limit.
 *    6. execve of a binary copied to a 4000-byte (and a 4095-byte) path,
 *       absolute and relative to the deep working directory; a "#!" script
 *       at depth with a #! line of about 250 bytes (must work) and one whose
 *       #! line names the interpreter by a 4000-byte path (reported only).
 *       Skipped (INFO) on noexec mounts and where files cannot be executed.
 *    7. ENAMETOOLONG for paths of 4096 and more bytes in every call that
 *       takes a path, and for a relative name that only becomes too long
 *       joined with the working directory.
 *    8. ordinary paths: "./x", "a/../b", "//", a trailing slash, links in
 *       the middle of a path, ".." through a linked directory.
 *    9. everything made is removed again (also after failures) and the work
 *       directory must be gone.
 *
 *   A read-only DIR (an ISO image, a read-only mount) gets read-only checks
 *   only: the deepest directory and file found below DIR are reached with
 *   stat, open, readdir, chdir step by step and getcwd; mkdir must fail with
 *   EROFS; the ENAMETOOLONG checks of section 7.  Nothing is written.
 *
 *   On FAT, exFAT, NTFS and ISO 9660 a name-length check that comes out
 *   differently is reported as INFO, not FAIL: those formats cap names
 *   themselves.
 *
 *   The exit status is the number of failed checks (at most 125); 0 means
 *   everything passed.  The --exit and --script arguments are used by the
 *   program itself for the exec checks.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <sys/xattr.h>

#define PMAX PATH_MAX /* 4096: holds the longest valid path and its NUL */
#define LIM (PATH_MAX - 1) /* 4095: the longest valid path */
#define BIG (3 * PATH_MAX) /* for paths that are too long on purpose */
#define TDEEP 3980 /* length of the path of the deepest directory */
#define TFILE 4000 /* length of the path of the files at depth */
#define MAXC 40 /* most directory levels below the work directory */

#define EXT4_MAGIC 0xEF53UL
#define MSDOS_MAGIC 0x4d44UL
#define TMPFS_MAGIC 0x01021994UL
#define EXFAT_MAGIC 0x2011BAB0UL
#define NTFS_MAGIC 0x5346544EUL
#define ISOFS_MAGIC 0x9660UL

static int g_pass, g_fail, g_info;

/* The directory under test and the work directory made in it. */
static char g_dir[PMAX];
static char g_base[PMAX];
static size_t g_blen;
/* The tree below the work directory: g_comp[0] .. g_comp[g_ncomp - 1]. */
static char g_comp[MAXC][NAME_MAX + 1];
static int g_ncomp;
static char g_deep[PMAX];
static size_t g_dlen;
static int g_tree; /* the whole tree was made */

/* The filesystem. */
static const char *g_fsname;
static int g_ro, g_noexec, g_cap, g_tmpfs;
static int g_links; /* symbolic links can be made here */

/* What the exec checks copy: this program (g_self) or a "true". */
static char g_src[PMAX];
static int g_self;

static char g_iobuf[65536];

/* ------------------------------------------------------------ output ---- */

static void say(const char *tag, const char *fmt, va_list ap)
{
	char line[700];

	vsnprintf(line, sizeof(line), fmt, ap);
	printf("%-4s  %s\n", tag, line);
	fflush(stdout);
}

static void pass(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	say("PASS", fmt, ap);
	va_end(ap);
	g_pass++;
}

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	say("FAIL", fmt, ap);
	va_end(ap);
	g_fail++;
}

static void info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	say("INFO", fmt, ap);
	va_end(ap);
	g_info++;
}

static const char *en(int e)
{
	static char other[32];

	switch (e) {
	case 0: return "no error";
	case EPERM: return "EPERM";
	case ENOENT: return "ENOENT";
	case EIO: return "EIO";
	case E2BIG: return "E2BIG";
	case ENOEXEC: return "ENOEXEC";
	case EBADF: return "EBADF";
	case ENOMEM: return "ENOMEM";
	case EACCES: return "EACCES";
	case EFAULT: return "EFAULT";
	case EBUSY: return "EBUSY";
	case EEXIST: return "EEXIST";
	case EXDEV: return "EXDEV";
	case ENOTDIR: return "ENOTDIR";
	case EISDIR: return "EISDIR";
	case EINVAL: return "EINVAL";
	case ENOSPC: return "ENOSPC";
	case EROFS: return "EROFS";
	case ERANGE: return "ERANGE";
	case ENAMETOOLONG: return "ENAMETOOLONG";
	case ENOSYS: return "ENOSYS";
	case ENOTEMPTY: return "ENOTEMPTY";
	case ELOOP: return "ELOOP";
	case EOPNOTSUPP: return "EOPNOTSUPP";
	default:
		snprintf(other, sizeof(other), "errno %d", e);
		return other;
	}
}

/* `ret' is a call's result: -1 with errno `want' passes. */
static void expect_err(long ret, int want, const char *fmt, ...)
{
	int e = errno;
	char what[500];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	if (ret == -1 && e == want)
		pass("%s -> %s", what, en(want));
	else if (ret == -1)
		fail("%s -> %s, expected %s", what, en(e), en(want));
	else
		fail("%s succeeded (%ld), expected %s", what, ret, en(want));
}

/* `ret' >= 0 passes. */
static void expect_ok(long ret, const char *fmt, ...)
{
	int e = errno;
	char what[500];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	if (ret >= 0)
		pass("%s", what);
	else
		fail("%s -> %s", what, en(e));
}

static void check(int ok, const char *fmt, ...)
{
	char what[500];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	if (ok)
		pass("%s", what);
	else
		fail("%s", what);
}

/* A check whose outcome the filesystem may decide (`cap'): INFO, not FAIL. */
static void soft(int ok, int cap, const char *fmt, ...)
{
	char what[500];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(what, sizeof(what), fmt, ap);
	va_end(ap);
	if (ok)
		pass("%s", what);
	else if (cap)
		info("%s (filesystem %s)", what, g_fsname);
	else
		fail("%s", what);
}

static unsigned long ul(size_t n)
{
	return (unsigned long)n;
}

/* ------------------------------------------------------------- paths ---- */

/* A name of `len' bytes: tag, two digits, then letters and digits only (no
 * case pairs, nothing a FAT name could not hold). */
static void mkname(char *o, size_t len, char tag, int idx)
{
	static const char fill[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	char head[8];
	size_t h = (size_t)snprintf(head, sizeof(head), "%c%02d", tag,
				    idx % 100);

	for (size_t i = 0; i < len; i++)
		o[i] = i < h ? head[i] : fill[(i * 7 + (size_t)idx) % 36];
	o[len] = '\0';
}

/* "a/b" into o (cap bytes); its length, or 0 when it does not fit. */
static size_t pjoin(char *o, size_t cap, const char *a, const char *b)
{
	int n = snprintf(o, cap, "%s/%s", a, b);

	if (n < 0 || (size_t)n >= cap) {
		o[0] = '\0';
		return 0;
	}
	return (size_t)n;
}

/* The path of level k of the tree (0: the work directory). */
static size_t level_path(char *o, int k)
{
	size_t n = strlen(g_base);

	memcpy(o, g_base, n + 1);
	for (int i = 0; i < k; i++) {
		size_t c = strlen(g_comp[i]);
		o[n++] = '/';
		memcpy(o + n, g_comp[i], c + 1);
		n += c;
	}
	return n;
}

/* `d' followed by made-up names of at most 200 bytes, exactly `total'
 * bytes long (total >= strlen(d) + 2).  o must hold total + 1 bytes. */
static void make_len(char *o, const char *d, size_t total, char tag)
{
	size_t len = strlen(d);
	int i = 0;

	memcpy(o, d, len + 1);
	if (len == 1 && d[0] == '/')
		len = 0;
	while (len < total) {
		size_t rem = total - len - 1, k = rem > 200 ? 200 : rem;

		if (rem > 200 && rem - k < 2)
			k = rem - 2;
		o[len++] = '/';
		mkname(o + len, k, tag, i++);
		len += k;
	}
	o[len] = '\0';
}

static int write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t n = strlen(text);

	if (fd < 0)
		return -1;
	if (write(fd, text, n) != (ssize_t)n) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return close(fd);
}

static int read_matches(const char *path, const char *text)
{
	char buf[256];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return 0;
	buf[n] = '\0';
	return strcmp(buf, text) == 0;
}

static int same_file(const char *a, const char *b)
{
	struct stat x, y;

	if (stat(a, &x) != 0 || stat(b, &y) != 0)
		return 0;
	return x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

static int is_dir(const char *p)
{
	struct stat st;

	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int gone(const char *p)
{
	struct stat st;

	return lstat(p, &st) == -1 && errno == ENOENT;
}

static int copy_file(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY), out, e;
	ssize_t n;

	if (in < 0)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0755);
	if (out < 0) {
		e = errno;
		close(in);
		errno = e;
		return -1;
	}
	while ((n = read(in, g_iobuf, sizeof(g_iobuf))) > 0) {
		if (write(out, g_iobuf, (size_t)n) != n) {
			n = -1;
			break;
		}
	}
	e = errno;
	close(in);
	if (close(out) != 0 && n == 0) {
		e = errno;
		n = -1;
	}
	if (n < 0) {
		unlink(dst);
		errno = e;
		return -1;
	}
	return 0;
}

static int is_elf(const char *p)
{
	unsigned char m[4];
	int fd = open(p, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = read(fd, m, 4);
	close(fd);
	return n == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' &&
	       m[3] == 'F';
}

/* ----------------------------------------------------------- removal ---- */

/* Remove `name' in the directory `pfd' and everything below it.  A link is
 * recognised with readlinkat and removed itself, never followed. */
static int rm_at(int pfd, const char *name, int depth)
{
	char lk[8];
	struct stat st;
	int fd, rc = 0;
	DIR *d;
	struct dirent *e;
	char (*names)[NAME_MAX + 1] = NULL;
	size_t cnt = 0, cap = 0;

	if (readlinkat(pfd, name, lk, sizeof(lk)) >= 0)
		return unlinkat(pfd, name, 0);
	if (fstatat(pfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlinkat(pfd, name, 0);
	if (depth > 64)
		return -1;
	fd = openat(pfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	d = fdopendir(fd);
	if (!d) {
		close(fd);
		return -1;
	}
	/* Names first, removal after: no directory changes under readdir. */
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (cnt == cap) {
			size_t nc = cap ? cap * 2 : 16;
			void *nn = realloc(names, nc * sizeof(*names));
			if (!nn) {
				rc = -1;
				break;
			}
			names = nn;
			cap = nc;
		}
		snprintf(names[cnt++], NAME_MAX + 1, "%s", e->d_name);
	}
	for (size_t i = 0; i < cnt; i++)
		if (rm_at(dirfd(d), names[i], depth + 1) != 0)
			rc = -1;
	free(names);
	closedir(d);
	if (unlinkat(pfd, name, AT_REMOVEDIR) != 0)
		rc = -1;
	return rc;
}

/* Remove the absolute path `p' and everything below it. */
static int rm_tree(const char *p)
{
	char parent[PMAX];
	const char *slash = strrchr(p, '/');
	int pfd, rc;

	if (!slash || strlen(p) >= sizeof(parent))
		return -1;
	memcpy(parent, p, (size_t)(slash - p));
	parent[slash - p] = '\0';
	if (!parent[0])
		strcpy(parent, "/");
	pfd = open(parent, O_RDONLY | O_DIRECTORY);
	if (pfd < 0)
		return -1;
	rc = rm_at(pfd, slash + 1, 0);
	close(pfd);
	return rc;
}

/* ------------------------------------------------------ 1. the tree ---- */

static void sec_tree(void)
{
	static const size_t cyc[] = { 255, 200, 254, 230, 255, 211, 240, 249 };
	char p[PMAX], n[NAME_MAX + 2], big[BIG];
	size_t cur = g_blen;
	int fd, r, bad;

	printf("-- 1. deep directories\n");

	/* Names of 255 and 256 bytes. */
	mkname(n, NAME_MAX, 'n', 0);
	pjoin(p, sizeof(p), g_base, n);
	r = mkdir(p, 0755);
	soft(r == 0, g_cap, "mkdir of a 255-byte name%s%s", r ? " -> " : "",
	     r ? en(errno) : "");
	if (r == 0)
		expect_ok(rmdir(p), "rmdir of a 255-byte name");
	fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
	soft(fd >= 0, g_cap, "open(O_CREAT) of a 255-byte name%s%s",
	     fd < 0 ? " -> " : "", fd < 0 ? en(errno) : "");
	if (fd >= 0) {
		close(fd);
		expect_ok(unlink(p), "unlink of a 255-byte name");
	}
	mkname(n, NAME_MAX + 1, 'n', 1);
	pjoin(p, sizeof(p), g_base, n);
	r = mkdir(p, 0755);
	soft(r == -1 && errno == ENAMETOOLONG, g_cap,
	     "mkdir of a 256-byte name -> %s, expected ENAMETOOLONG",
	     r ? en(errno) : "success");
	if (r == 0)
		rmdir(p);
	fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
	soft(fd == -1 && errno == ENAMETOOLONG, g_cap,
	     "open(O_CREAT) of a 256-byte name -> %s, expected ENAMETOOLONG",
	     fd < 0 ? en(errno) : "success");
	if (fd >= 0) {
		close(fd);
		unlink(p);
	}

	/* The tree: names of 200..255 bytes until the path is TDEEP long. */
	g_ncomp = 0;
	g_tree = 0;
	while (g_ncomp < MAXC) {
		size_t rem = TDEEP - cur - 1, len;
		int last = rem <= NAME_MAX;

		if (last) {
			len = rem;
		} else {
			len = cyc[g_ncomp % 8];
			if (rem - len - 1 < 1)
				len = rem - 2;
		}
		mkname(g_comp[g_ncomp], len, 'd', g_ncomp);
		level_path(p, g_ncomp + 1);
		if (mkdir(p, 0755) != 0) {
			fail("mkdir of level %d (a %lu-byte path) -> %s",
			     g_ncomp + 1, ul(strlen(p)), en(errno));
			return;
		}
		g_ncomp++;
		cur = strlen(p);
		if (last)
			break;
	}
	if (cur != TDEEP) {
		fail("tree stopped at %lu bytes", ul(cur));
		return;
	}
	g_tree = 1;
	memcpy(g_deep, p, cur + 1);
	g_dlen = cur;
	pass("mkdir of %d levels with absolute paths, the deepest %lu bytes",
	     g_ncomp, ul(g_dlen));

	/* Every level is a directory whose listing holds the next name. */
	bad = -1;
	for (int k = 0; k < g_ncomp && bad < 0; k++) {
		DIR *d;
		struct dirent *e;
		int found = 0;

		level_path(p, k);
		if (!is_dir(p) || !(d = opendir(p))) {
			bad = k;
			break;
		}
		while ((e = readdir(d)) != NULL)
			if (strcmp(e->d_name, g_comp[k]) == 0)
				found = 1;
		closedir(d);
		if (!found)
			bad = k;
	}
	check(bad < 0, "stat and readdir of every level list the long names%s",
	      bad < 0 ? "" : " (missing at a level)");
	if (bad >= 0)
		info("first level without its name: %d", bad);
	check(is_dir(g_deep), "stat of the %lu-byte deepest directory",
	      ul(g_dlen));

	/* The path of exactly 4095 bytes, then 4096. */
	mkname(n, LIM - g_dlen - 1, 'm', 0);
	pjoin(p, sizeof(p), g_deep, n);
	expect_ok(mkdir(p, 0755), "mkdir of a path of exactly %lu bytes",
		  ul(strlen(p)));
	check(is_dir(p), "stat of the 4095-byte directory");
	expect_ok(rmdir(p), "rmdir of the 4095-byte directory");
	mkname(n, LIM - g_dlen, 'm', 1);
	pjoin(big, sizeof(big), g_deep, n);
	expect_err(mkdir(big, 0755), ENAMETOOLONG, "mkdir of a %lu-byte path",
		   ul(strlen(big)));
}

/* ----------------------------------------------------- 2. the files ---- */

static void check_mtime(const char *what, const struct stat *st, long want)
{
	long d = (long)st->st_mtime - want;

	if (d < 0)
		d = -d;
	soft(d == 0 || (g_cap && d <= 2), g_cap, "%s: mtime %ld (set %ld)",
	     what, (long)st->st_mtime, want);
}

static void sec_files(void)
{
	char f[PMAX], f2[PMAX], n[NAME_MAX + 1], rel[PMAX];
	struct stat st;
	struct timespec ts[2];
	int fd, dfd, ok;
	size_t i;

	printf("-- 2. files at depth\n");
	ts[0].tv_sec = 1600000000;
	ts[0].tv_nsec = 0;
	ts[1].tv_sec = 1700000000;
	ts[1].tv_nsec = 0;

	mkname(n, TFILE - g_dlen - 1, 'f', 0);
	pjoin(f, sizeof(f), g_deep, n);
	fd = open(f, O_WRONLY | O_CREAT | O_EXCL, 0644);
	expect_ok(fd, "open(O_CREAT) of a %lu-byte path", ul(strlen(f)));
	if (fd < 0)
		return;
	for (i = 0; i < 1500; i++)
		g_iobuf[i] = (char)('A' + i % 23);
	check(write(fd, g_iobuf, 1500) == 1500, "write 1500 bytes");
	close(fd);
	fd = open(f, O_RDONLY);
	ok = fd >= 0 && read(fd, g_iobuf + 2000, 1600) == 1500 &&
	     memcmp(g_iobuf, g_iobuf + 2000, 1500) == 0;
	if (fd >= 0)
		close(fd);
	check(ok, "open and read back the 1500 bytes");
	ok = stat(f, &st) == 0;
	check(ok && S_ISREG(st.st_mode) && st.st_size == 1500,
	      "stat: a regular file of 1500 bytes");
	ok = lstat(f, &st) == 0;
	check(ok && S_ISREG(st.st_mode) && st.st_size == 1500,
	      "lstat: a regular file of 1500 bytes");
	expect_ok(access(f, R_OK | W_OK), "access(R_OK|W_OK)");
	if (chmod(f, 0600) != 0)
		soft(0, g_cap, "chmod 0600 -> %s", en(errno));
	else
		soft(stat(f, &st) == 0 && (st.st_mode & 0777) == 0600, g_cap,
		     "chmod 0600: mode now %o", (unsigned)(st.st_mode & 0777));
	expect_ok(utimensat(AT_FDCWD, f, ts, 0), "utimensat");
	if (stat(f, &st) == 0)
		check_mtime("utimensat", &st, ts[1].tv_sec);
	expect_ok(truncate(f, 100), "truncate to 100 bytes");
	check(stat(f, &st) == 0 && st.st_size == 100, "stat: 100 bytes");
	mkname(n, TFILE - g_dlen - 1, 'g', 0);
	pjoin(f2, sizeof(f2), g_deep, n);
	expect_ok(rename(f, f2), "rename between two 4000-byte paths");
	check(gone(f) && stat(f2, &st) == 0 && st.st_size == 100,
	      "old name gone, new name holds the file");
	expect_ok(unlink(f2), "unlink of a 4000-byte path");
	check(gone(f2), "the file is gone");

	/* Exactly 4095 bytes. */
	mkname(n, LIM - g_dlen - 1, 'e', 0);
	pjoin(f, sizeof(f), g_deep, n);
	ok = write_file(f, "limit");
	expect_ok(ok, "create a file at a path of exactly %lu bytes",
		  ul(strlen(f)));
	if (ok == 0) {
		check(read_matches(f, "limit"), "read it back");
		expect_ok(unlink(f), "unlink of the 4095-byte path");
	}

	/* The *at() calls relative to the deepest directory. */
	dfd = open(g_deep, O_RDONLY | O_DIRECTORY);
	expect_ok(dfd, "open(O_DIRECTORY) of the deepest directory");
	if (dfd >= 0) {
		fd = openat(dfd, "atfile", O_WRONLY | O_CREAT | O_EXCL, 0644);
		expect_ok(fd, "openat(O_CREAT)");
		if (fd >= 0) {
			check(write(fd, "at", 2) == 2, "write through openat");
			close(fd);
		}
		check(fstatat(dfd, "atfile", &st, 0) == 0 && st.st_size == 2,
		      "fstatat: 2 bytes");
		pjoin(f, sizeof(f), g_deep, "atfile");
		check(read_matches(f, "at"),
		      "the absolute path reads what openat wrote");
		expect_ok(faccessat(dfd, "atfile", R_OK, 0), "faccessat");
		if (fchmodat(dfd, "atfile", 0640, 0) != 0)
			soft(0, g_cap, "fchmodat 0640 -> %s", en(errno));
		else
			soft(fstatat(dfd, "atfile", &st, 0) == 0 &&
				     (st.st_mode & 0777) == 0640,
			     g_cap, "fchmodat 0640: mode now %o",
			     (unsigned)(st.st_mode & 0777));
		expect_ok(utimensat(dfd, "atfile", ts, 0), "utimensat(dirfd)");
		if (fstatat(dfd, "atfile", &st, 0) == 0)
			check_mtime("utimensat(dirfd)", &st, ts[1].tv_sec);
		expect_ok(mkdirat(dfd, "atdir", 0755), "mkdirat");
		check(fstatat(dfd, "atdir", &st, 0) == 0 && S_ISDIR(st.st_mode),
		      "fstatat: a directory");
		expect_ok(renameat(dfd, "atfile", dfd, "atdir/atfile2"),
			  "renameat into the new directory");
		check(fstatat(dfd, "atdir/atfile2", &st, 0) == 0 &&
			      fstatat(dfd, "atfile", &st, 0) == -1,
		      "renameat moved the file");
		expect_ok(unlinkat(dfd, "atdir/atfile2", 0), "unlinkat");
		expect_ok(unlinkat(dfd, "atdir", AT_REMOVEDIR),
			  "unlinkat(AT_REMOVEDIR)");
		check(fstatat(dfd, "atdir", &st, 0) == -1, "both are gone");
		close(dfd);
	}

	/* A descriptor one level up, naming the last level in the path. */
	level_path(f, g_ncomp - 1);
	dfd = open(f, O_RDONLY | O_DIRECTORY);
	snprintf(rel, sizeof(rel), "%s/atfile3", g_comp[g_ncomp - 1]);
	if (dfd >= 0) {
		fd = openat(dfd, rel, O_WRONLY | O_CREAT | O_EXCL, 0644);
		expect_ok(fd, "openat(O_CREAT) through the last level");
		if (fd >= 0)
			close(fd);
		pjoin(f, sizeof(f), g_deep, "atfile3");
		check(stat(f, &st) == 0, "stat of it by its absolute path");
		expect_ok(unlinkat(dfd, rel, 0), "unlinkat through the last level");
		close(dfd);
	} else {
		fail("open of the level above the deepest -> %s", en(errno));
	}

	/* A descriptor at the top and a relative path of ~3970 bytes. */
	dfd = open(g_base, O_RDONLY | O_DIRECTORY);
	snprintf(rel, sizeof(rel), "%s/atfile4", g_deep + g_blen + 1);
	if (dfd >= 0) {
		fd = openat(dfd, rel, O_WRONLY | O_CREAT | O_EXCL, 0644);
		expect_ok(fd, "openat(O_CREAT) of a %lu-byte relative path",
			  ul(strlen(rel)));
		if (fd >= 0)
			close(fd);
		check(fstatat(dfd, rel, &st, 0) == 0 && S_ISREG(st.st_mode),
		      "fstatat of the long relative path");
		expect_ok(faccessat(dfd, rel, F_OK, 0),
			  "faccessat of the long relative path");
		expect_ok(unlinkat(dfd, rel, 0),
			  "unlinkat of the long relative path");
		close(dfd);
	} else {
		fail("open of the work directory -> %s", en(errno));
	}
}

/* --------------------------------------------- 3. the working directory -- */

static void sec_cwd(void)
{
	char cwd[PMAX], p[PMAX];
	char *r;
	struct stat a, b;
	int bad = -1, e = 0;

	printf("-- 3. working directory at depth\n");
	if (chdir(g_base) != 0) {
		fail("chdir to the work directory -> %s", en(errno));
		return;
	}
	for (int k = 0; k < g_ncomp; k++) {
		if (chdir(g_comp[k]) != 0) {
			e = errno;
			bad = k;
			break;
		}
	}
	if (bad >= 0) {
		fail("chdir down one relative name at a time: level %d -> %s",
		     bad + 1, en(e));
		chdir("/");
		return;
	}
	pass("chdir down %d levels one relative name at a time", g_ncomp);
	r = getcwd(cwd, sizeof(cwd));
	check(r && strcmp(cwd, g_deep) == 0,
	      "getcwd returns the whole %lu-byte path", ul(g_dlen));
	r = getcwd(NULL, 0);
	check(r && strcmp(r, g_deep) == 0, "getcwd(NULL, 0) returns it too");
	free(r);
	r = getcwd(cwd, g_dlen);
	expect_err(r ? 0 : -1, ERANGE, "getcwd with a %lu-byte buffer",
		   ul(g_dlen));
	r = getcwd(cwd, g_dlen + 1);
	check(r && strcmp(cwd, g_deep) == 0,
	      "getcwd with a buffer of exactly %lu bytes", ul(g_dlen + 1));
	r = getcwd(cwd, 10);
	expect_err(r ? 0 : -1, ERANGE, "getcwd with a 10-byte buffer");
	r = getcwd(NULL, 100);
	expect_err(r ? 0 : -1, ERANGE, "getcwd(NULL, 100)");
	free(r);

	/* Relative calls from down here. */
	expect_ok(write_file("rel", "relative"), "create a file by a relative name");
	check(stat("rel", &a) == 0 && a.st_size == 8, "stat of the relative name");
	pjoin(p, sizeof(p), g_deep, "rel");
	check(read_matches(p, "relative"), "its absolute path reads the same");
	expect_ok(rename("rel", "./rel2"), "rename by relative names");
	check(gone("rel") && read_matches("rel2", "relative"),
	      "the old name is gone, the new one reads");
	expect_ok(access("rel2", R_OK), "access by a relative name");
	expect_ok(unlink("rel2"), "unlink by a relative name");
	expect_ok(mkdir("sub", 0755), "mkdir by a relative name");
	expect_ok(chdir("sub"), "chdir into it");
	pjoin(p, sizeof(p), g_deep, "sub");
	r = getcwd(cwd, sizeof(cwd));
	check(r && strcmp(cwd, p) == 0, "getcwd: %lu bytes", ul(strlen(p)));
	expect_ok(chdir(".."), "chdir(\"..\")");
	r = getcwd(cwd, sizeof(cwd));
	check(r && strcmp(cwd, g_deep) == 0, "getcwd back at the deepest");
	expect_ok(rmdir("sub"), "rmdir by a relative name");

	/* "..". */
	level_path(p, g_ncomp - 2);
	check(stat("../..", &a) == 0 && stat(p, &b) == 0 &&
		      a.st_ino == b.st_ino && a.st_dev == b.st_dev,
	      "stat(\"../..\") is two levels up");
	snprintf(p, sizeof(p), "./../%s/.", g_comp[g_ncomp - 1]);
	check(same_file(p, g_deep), "stat(\"./../NAME/.\") is the deepest");
	bad = -1;
	for (int k = g_ncomp; k > 0; k--) {
		if (chdir("..") != 0 || !getcwd(cwd, sizeof(cwd))) {
			bad = k;
			break;
		}
		level_path(p, k - 1);
		if (strcmp(cwd, p) != 0) {
			bad = k;
			break;
		}
	}
	check(bad < 0, "chdir(\"..\") %d times: getcwd right at every level%s",
	      g_ncomp, bad < 0 ? "" : " (wrong at a level)");
	expect_ok(chdir(g_deep), "chdir with the whole absolute path");
	r = getcwd(cwd, sizeof(cwd));
	check(r && strcmp(cwd, g_deep) == 0, "getcwd after it");
	chdir("/");
}

/* ------------------------------------------------- 4. symbolic links ---- */

static int link_unsupported(int e)
{
	return e == EPERM || e == ENOSYS || e == EOPNOTSUPP;
}

static void link_reads(const char *lp, const char *target, const char *what)
{
	static char buf[BIG];
	size_t tl = strlen(target);
	int n = readlink(lp, buf, sizeof(buf));

	check(n >= 0 && (size_t)n == tl && memcmp(buf, target, tl) == 0,
	      "readlink of %s returns all %lu bytes (got %d)", what, ul(tl),
	      n);
}

static void sec_links(void)
{
	char lp[PMAX], l2[PMAX], tf[PMAX], cwd[PMAX], p[PMAX];
	static char t[BIG], buf[BIG];
	struct stat st;
	size_t tl, want;
	int n, r, k;

	printf("-- 4. symbolic links\n");
	pjoin(lp, sizeof(lp), g_base, "Lprobe");
	if (symlink("probe", lp) != 0) {
		int e = errno;

		g_links = 0;
		if (link_unsupported(e))
			info("no symbolic links here (%s): link checks skipped",
			     en(e));
		else
			fail("symlink -> %s", en(e));
		return;
	}
	unlink(lp);
	g_links = 1;
	pjoin(tf, sizeof(tf), g_deep, "tf");
	if (write_file(tf, "hello") != 0) {
		fail("create %s -> %s", "tf at depth", en(errno));
		return;
	}

	/* An absolute target of TDEEP bytes. */
	pjoin(lp, sizeof(lp), g_base, "Labs");
	expect_ok(symlink(g_deep, lp), "symlink with a %lu-byte absolute target",
		  ul(g_dlen));
	link_reads(lp, g_deep, "it");
	n = readlink(lp, buf, 64);
	check(n == 64 && memcmp(buf, g_deep, 64) == 0,
	      "readlink into a 64-byte buffer gives 64 bytes (got %d)", n);
	r = lstat(lp, &st);
	soft(r == 0 && S_ISLNK(st.st_mode) && (size_t)st.st_size == g_dlen,
	     g_cap, "lstat: a link of size %lu (got %ld)", ul(g_dlen),
	     r == 0 ? (long)st.st_size : -1L);
	check(same_file(lp, g_deep), "stat through it is the deepest directory");
	pjoin(p, sizeof(p), lp, "tf");
	check(read_matches(p, "hello"), "open of a file through it");
	expect_ok(chdir(lp), "chdir through it");
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		fail("getcwd after chdir through the link -> %s", en(errno));
	else if (strcmp(cwd, g_deep) == 0)
		pass("getcwd after chdir through the link: the resolved path");
	else if (strcmp(cwd, lp) == 0)
		/* The kernel keeps the working directory as the path it
		 * was entered by (lexically, ".." included): an answer
		 * of its own, not an error. */
		info("getcwd after chdir through the link shows the link's "
		     "path (the working directory is kept lexically)");
	else
		fail("getcwd after chdir through the link: a %lu-byte path "
		     "that is neither", ul(strlen(cwd)));
	check(stat("tf", &st) == 0, "relative stat from there");
	chdir("/");

	/* A target of exactly 4095 bytes: DEEP/././...//tf */
	memcpy(t, g_deep, g_dlen);
	tl = g_dlen;
	t[tl++] = '/';
	while (LIM - 2 - tl >= 2) {
		t[tl++] = '.';
		t[tl++] = '/';
	}
	if (tl < LIM - 2)
		t[tl++] = '/';
	t[tl++] = 't';
	t[tl++] = 'f';
	t[tl] = '\0';
	pjoin(lp, sizeof(lp), g_base, "Lmax");
	r = symlink(t, lp);
	if (r == 0) {
		pass("symlink with a target of exactly %lu bytes", ul(tl));
		link_reads(lp, t, "it");
		check(same_file(lp, tf), "stat through it is the file");
		check(read_matches(lp, "hello"), "open through it");
	} else if (errno == ENAMETOOLONG && !g_tmpfs) {
		info("symlink with a %lu-byte target -> ENAMETOOLONG (the "
		     "filesystem's own limit on a link)", ul(tl));
	} else {
		fail("symlink with a target of exactly %lu bytes -> %s", ul(tl),
		     en(errno));
	}

	/* At depth, back to the top: ./././ ... ../../.. (joined with the
	 * link's directory and "/Labs/tf" it stays within the limit). */
	want = LIM - g_dlen - 1 - 8;
	if ((size_t)g_ncomp * 3 <= want) {
		tl = 0;
		while (tl + 2 + (size_t)g_ncomp * 3 - 1 <= want) {
			t[tl++] = '.';
			t[tl++] = '/';
		}
		for (k = 0; k < g_ncomp; k++) {
			memcpy(t + tl, k < g_ncomp - 1 ? "../" : "..", 3);
			tl += k < g_ncomp - 1 ? 3 : 2;
		}
		t[tl] = '\0';
		pjoin(lp, sizeof(lp), g_deep, "Lup");
		expect_ok(symlink(t, lp),
			  "symlink at depth with a %lu-byte relative target "
			  "of %d \"..\"", ul(tl), g_ncomp);
		link_reads(lp, t, "it");
		check(same_file(lp, g_base),
		      "stat through it is the work directory");
		pjoin(p, sizeof(p), lp, "Labs/tf");
		check(read_matches(p, "hello"),
		      "back down through the top link: the file");
	}

	/* At the top: "../" many times, then the deepest directory from the
	 * root; used as LINK/tf the joined path is exactly 4095 bytes. */
	want = LIM - g_blen - 1 - 3;
	if (want >= g_dlen - 1 + 3) {
		size_t ups = (want - (g_dlen - 1)) / 3;

		tl = 0;
		for (size_t i = 0; i < ups; i++) {
			memcpy(t + tl, "../", 3);
			tl += 3;
		}
		while (tl + g_dlen - 1 < want)
			t[tl++] = '/';
		memcpy(t + tl, g_deep + 1, g_dlen);
		tl += g_dlen - 1;
		pjoin(lp, sizeof(lp), g_base, "Lfar");
		expect_ok(symlink(t, lp),
			  "symlink with a %lu-byte target of %lu \"..\"", ul(tl),
			  ul(ups));
		link_reads(lp, t, "it");
		check(same_file(lp, g_deep), "stat through it is the deepest");
		pjoin(p, sizeof(p), lp, "tf");
		check(read_matches(p, "hello"),
		      "open of LINK/tf (joined: %lu bytes)",
		      ul(g_blen + 1 + tl + 3));
	} else {
		info("work directory path too long for the many-\"..\" link");
	}

	/* A chain: C3 -> C2 -> /abs/C1 -> Labs -> deepest. */
	pjoin(lp, sizeof(lp), g_base, "C1");
	expect_ok(symlink("Labs", lp), "symlink C1 -> Labs");
	pjoin(l2, sizeof(l2), g_base, "C2");
	expect_ok(symlink(lp, l2), "symlink C2 -> absolute C1");
	pjoin(lp, sizeof(lp), g_base, "C3");
	expect_ok(symlink("C2", lp), "symlink C3 -> C2");
	check(same_file(lp, g_deep), "stat of C3 is the deepest directory");
	pjoin(p, sizeof(p), lp, "tf");
	check(read_matches(p, "hello"), "open of C3/tf");
	if (chdir(lp) == 0) {
		check(getcwd(cwd, sizeof(cwd)) && same_file(".", g_deep),
		      "chdir through the chain");
		chdir("/");
	} else {
		fail("chdir through the chain -> %s", en(errno));
	}

	/* Loops. */
	pjoin(lp, sizeof(lp), g_base, "Lself");
	expect_ok(symlink("Lself", lp), "symlink Lself -> Lself");
	expect_err(stat(lp, &st), ELOOP, "stat of a link to itself");
	expect_err(open(lp, O_RDONLY), ELOOP, "open of a link to itself");
	pjoin(lp, sizeof(lp), g_base, "La");
	pjoin(l2, sizeof(l2), g_base, "Lb");
	expect_ok(symlink("Lb", lp), "symlink La -> Lb");
	expect_ok(symlink("La", l2), "symlink Lb -> La");
	expect_err(stat(lp, &st), ELOOP, "stat of a two-link loop");

	/* Removing the links leaves their targets alone. */
	{
		static const char *const top[] = { "Labs", "Lmax", "Lfar", "C1",
						   "C2", "C3", "Lself", "La",
						   "Lb" };
		int bad = 0;

		for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); i++) {
			pjoin(lp, sizeof(lp), g_base, top[i]);
			if (unlink(lp) != 0 && errno != ENOENT)
				bad++;
		}
		pjoin(lp, sizeof(lp), g_deep, "Lup");
		if (unlink(lp) != 0 && errno != ENOENT)
			bad++;
		check(bad == 0 && is_dir(g_deep) && read_matches(tf, "hello"),
		      "unlink of the links; their targets remain");
	}
	unlink(tf);
}

/* ------------------------------------------------------------ 5. rename -- */

static void sec_rename(void)
{
	char a[PMAX], b[PMAX], c[PMAX], n[NAME_MAX + 2];
	static char big[BIG];
	int r;

	printf("-- 5. rename between depth and the top\n");
	pjoin(a, sizeof(a), g_deep, "mvfile");
	pjoin(b, sizeof(b), g_base, "mvfile");
	pjoin(c, sizeof(c), g_deep, "mvfile2");
	if (write_file(a, "move") != 0) {
		fail("create a file at depth -> %s", en(errno));
	} else {
		expect_ok(rename(a, b), "rename of a file from depth to the top");
		check(gone(a) && read_matches(b, "move"), "moved up");
		expect_ok(rename(b, c), "rename of a file from the top to depth");
		check(gone(b) && read_matches(c, "move"), "moved down");
		unlink(c);
	}

	pjoin(a, sizeof(a), g_base, "mvdir");
	pjoin(b, sizeof(b), g_deep, "mvdir");
	pjoin(c, sizeof(c), g_base, "mvdir2");
	if (mkdir(a, 0755) != 0) {
		fail("mkdir at the top -> %s", en(errno));
	} else {
		char f[PMAX];

		pjoin(f, sizeof(f), a, "f");
		write_file(f, "dir");
		expect_ok(rename(a, b),
			  "rename of a directory from the top to depth");
		pjoin(f, sizeof(f), b, "f");
		check(gone(a) && read_matches(f, "dir"),
		      "moved down with its content");
		expect_ok(rename(b, c),
			  "rename of a directory from depth to the top");
		pjoin(f, sizeof(f), c, "f");
		check(gone(b) && read_matches(f, "dir"),
		      "moved up with its content");
		unlink(f);
		rmdir(c);
	}

	/* A new name of 4096 bytes; exactly 4095 works. */
	pjoin(a, sizeof(a), g_base, "rf");
	if (write_file(a, "rf") == 0) {
		mkname(n, LIM - g_dlen, 'q', 0);
		pjoin(big, sizeof(big), g_deep, n);
		expect_err(rename(a, big), ENAMETOOLONG,
			   "rename to a %lu-byte path", ul(strlen(big)));
		expect_err(rename(big, a), ENAMETOOLONG,
			   "rename from a %lu-byte path", ul(strlen(big)));
		check(read_matches(a, "rf"), "the file is still there");
		mkname(n, LIM - g_dlen - 1, 'q', 1);
		pjoin(b, sizeof(b), g_deep, n);
		expect_ok(rename(a, b), "rename to a path of exactly %lu bytes",
			  ul(strlen(b)));
		expect_ok(rename(b, a), "and back");
		unlink(a);
	} else {
		fail("create a file at the top -> %s", en(errno));
	}

	/* Move the first level under a 150-byte name: the deepest path
	 * would become ~4130 bytes. */
	mkname(n, 150, 'p', 0);
	pjoin(c, sizeof(c), g_base, n);
	if (mkdir(c, 0755) == 0) {
		level_path(a, 1);
		pjoin(b, sizeof(b), c, g_comp[0]);
		r = rename(a, b);
		if (r == -1 && errno == ENAMETOOLONG) {
			pass("moving a directory so the paths below it would "
			     "pass %d bytes -> ENAMETOOLONG", LIM);
		} else if (r == 0) {
			info("moving a directory so the deepest path becomes "
			     "%lu bytes is allowed", ul(g_dlen + 151));
			expect_ok(rename(b, a), "moving it back");
			check(is_dir(g_deep), "the deepest directory is back");
		} else {
			fail("moving a directory so the paths below it pass "
			     "the limit -> %s", en(errno));
		}
		rmdir(c);
	} else {
		fail("mkdir of a 150-byte name -> %s", en(errno));
	}
}

/* -------------------------------------------------------------- 6. exec -- */

/* Run `path' in a child (after chdir to `cwd' when not NULL); *err is the
 * errno of a failed exec, 0 when the exec happened. */
static int run(const char *path, char *const argv[], const char *cwd,
	       int *status, int *err)
{
	int pfd[2], e = 0;
	pid_t pid;
	ssize_t n;

	*err = 0;
	*status = 0;
	if (pipe2(pfd, O_CLOEXEC) != 0)
		return -1;
	fflush(stdout);
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pfd[0]);
		if (cwd && chdir(cwd) != 0) {
			e = errno;
			if (write(pfd[1], &e, sizeof(e)) < 0)
				_exit(125);
			_exit(126);
		}
		execv(path, argv);
		e = errno;
		if (write(pfd[1], &e, sizeof(e)) < 0)
			_exit(125);
		_exit(127);
	}
	close(pfd[1]);
	n = read(pfd[0], &e, sizeof(e));
	close(pfd[0]);
	if (waitpid(pid, status, 0) != pid)
		return -1;
	if (n == (ssize_t)sizeof(e))
		*err = e;
	return 0;
}

static void exec_expect(const char *path, char *const argv[], const char *cwd,
			int want, const char *what)
{
	int st, e;

	if (run(path, argv, cwd, &st, &e) != 0) {
		fail("%s: fork/wait -> %s", what, en(errno));
		return;
	}
	if (e && g_cap && e == EACCES)
		info("%s -> EACCES (filesystem %s)", what, g_fsname);
	else if (e)
		fail("%s -> %s", what, en(e));
	else if (WIFEXITED(st) && WEXITSTATUS(st) == want)
		pass("%s: exit status %d", what, want);
	else
		fail("%s: wait status 0x%x, expected exit %d", what,
		     (unsigned)st, want);
}

static void sec_exec(void)
{
	char bin[PMAX], bin2[PMAX], n[NAME_MAX + 1], n2[NAME_MAX + 1];
	char rel[NAME_MAX + 3], idir[PMAX], interp[PMAX], scr[PMAX];
	char sname[NAME_MAX + 1], line[PMAX + 64];
	char *av[4];
	struct stat st;
	int want, st2, e, ilen;

	printf("-- 6. exec at depth\n");
	if (g_noexec) {
		info("noexec mount: exec checks skipped");
		return;
	}
	if (!g_src[0]) {
		info("no program found to copy: exec checks skipped");
		return;
	}
	mkname(n, TFILE - g_dlen - 1, 'x', 0);
	pjoin(bin, sizeof(bin), g_deep, n);
	if (copy_file(g_src, bin) != 0) {
		fail("copy of %s to a %lu-byte path -> %s", g_src,
		     ul(strlen(bin)), en(errno));
		return;
	}
	chmod(bin, 0755);
	if (stat(bin, &st) != 0 || !(st.st_mode & S_IXUSR)) {
		info("files here are not executable (mode %o): exec checks "
		     "skipped", (unsigned)(st.st_mode & 0777));
		unlink(bin);
		return;
	}
	if (g_self) {
		av[1] = "--exit";
		av[2] = "7";
		av[3] = NULL;
		want = 7;
	} else {
		av[1] = NULL;
		want = 0;
	}
	av[0] = bin;
	exec_expect(bin, av, NULL, want, "execve of a 4000-byte absolute path");
	snprintf(rel, sizeof(rel), "./%s", n);
	av[0] = rel;
	exec_expect(rel, av, g_deep, want,
		    "execve of ./NAME from the deep working directory");
	av[0] = n;
	exec_expect(n, av, g_deep, want,
		    "execve of a bare NAME from the deep working directory");
	mkname(n2, LIM - g_dlen - 1, 'y', 0);
	pjoin(bin2, sizeof(bin2), g_deep, n2);
	if (rename(bin, bin2) == 0) {
		av[0] = bin2;
		exec_expect(bin2, av, NULL, want,
			    "execve of a path of exactly 4095 bytes");
		rename(bin2, bin);
	} else {
		fail("rename to a 4095-byte path -> %s", en(errno));
	}

	/* "#!" scripts.  The interpreter: this program (with --script) or a
	 * shell, at a path that makes the #! line about 250 bytes long. */
	if (!g_self && access("/bin/sh", X_OK) != 0) {
		info("no /bin/sh: #! checks skipped");
		unlink(bin);
		return;
	}
	ilen = 240;
	if ((int)g_blen + 1 + 10 + 3 > ilen) {
		info("work directory path too long for the #! checks");
		unlink(bin);
		return;
	}
	mkname(sname, (size_t)ilen - g_blen - 1 - 3, 'i', 0);
	pjoin(idir, sizeof(idir), g_base, sname);
	pjoin(interp, sizeof(interp), idir, "pt");
	if (mkdir(idir, 0755) != 0) {
		fail("mkdir for the interpreter -> %s", en(errno));
		unlink(bin);
		return;
	}
	if (g_self)
		e = rename(bin, interp);
	else if (g_links)
		e = symlink("/bin/sh", interp);
	else
		e = copy_file("/bin/sh", interp);
	if (e != 0) {
		fail("interpreter at a %d-byte path -> %s", ilen, en(errno));
		unlink(bin);
		rmdir(idir);
		return;
	}
	if (!g_self)
		chmod(interp, 0755);
	mkname(sname, TFILE - g_dlen - 1, 's', 0);
	pjoin(scr, sizeof(scr), g_deep, sname);
	if (g_self)
		snprintf(line, sizeof(line), "#!%s --script\n", interp);
	else
		snprintf(line, sizeof(line), "#!%s\nexit 42\n", interp);
	if (write_file(scr, line) != 0) {
		fail("write the script -> %s", en(errno));
	} else {
		char what[160];

		chmod(scr, 0755);
		snprintf(what, sizeof(what),
			 "#! script at a 4000-byte path, #! line of %lu bytes",
			 ul(strchr(line, '\n') - line));
		av[0] = scr;
		av[1] = NULL;
		exec_expect(scr, av, NULL, 42, what);
		snprintf(rel, sizeof(rel), "./%s", sname);
		av[0] = rel;
		exec_expect(rel, av, g_deep, 42,
			    "#! script by ./NAME from the deep directory");

		/* The interpreter at depth: a #! line of ~4000 bytes. */
		if (!g_self)
			unlink(bin);
		if (g_self)
			e = rename(interp, bin);
		else if (g_links)
			e = symlink("/bin/sh", bin);
		else
			e = copy_file("/bin/sh", bin);
		if (e == 0) {
			chmod(bin, 0755);
			if (g_self)
				snprintf(line, sizeof(line),
					 "#!%s --script\n", bin);
			else
				snprintf(line, sizeof(line),
					 "#!%s\nexit 42\n", bin);
			if (write_file(scr, line) != 0) {
				fail("rewrite the script -> %s", en(errno));
			} else if (run(scr, av, g_deep, &st2, &e) != 0) {
				fail("fork/wait -> %s", en(errno));
			} else if (e) {
				info("#! line of %lu bytes -> %s (the kernel "
				     "caps the #! line)",
				     ul(strchr(line, '\n') - line), en(e));
			} else if (WIFEXITED(st2) && WEXITSTATUS(st2) == 42) {
				pass("#! line of %lu bytes works",
				     ul(strchr(line, '\n') - line));
			} else {
				fail("#! line of %lu bytes: wait status 0x%x",
				     ul(strchr(line, '\n') - line),
				     (unsigned)st2);
			}
			if (g_self)
				rename(bin, interp);
		} else {
			info("interpreter at depth -> %s", en(errno));
		}
		unlink(scr);
	}
	unlink(interp);
	unlink(bin);
	rmdir(idir);
}

/* ------------------------------------------------------ 7. too long ---- */

static void toolong_one(const char *big, int writable)
{
	static char rel[BIG];
	char sh[PMAX];
	struct stat st;
	struct timespec ts[2];
	char *av[2];
	size_t bl = strlen(big);
	int r;

	ts[0].tv_sec = ts[1].tv_sec = 1700000000;
	ts[0].tv_nsec = ts[1].tv_nsec = 0;
	av[0] = "x";
	av[1] = NULL;
	expect_err(open(big, O_RDONLY), ENAMETOOLONG, "open of %lu bytes", ul(bl));
	r = open(big, O_WRONLY | O_CREAT, 0644);
	if (!writable && r == -1 && errno == EROFS)
		info("open(O_CREAT) of %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "open(O_CREAT) of %lu bytes", ul(bl));
	expect_err(stat(big, &st), ENAMETOOLONG, "stat of %lu bytes", ul(bl));
	expect_err(lstat(big, &st), ENAMETOOLONG, "lstat of %lu bytes", ul(bl));
	expect_err(access(big, F_OK), ENAMETOOLONG, "access of %lu bytes",
		   ul(bl));
	expect_err(chdir(big), ENAMETOOLONG, "chdir of %lu bytes", ul(bl));
	expect_err(readlink(big, sh, sizeof(sh)), ENAMETOOLONG,
		   "readlink of %lu bytes", ul(bl));
	expect_err(execv(big, av), ENAMETOOLONG, "execve of %lu bytes", ul(bl));
	r = mkdir(big, 0755);
	if (!writable && r == -1 && errno == EROFS)
		info("mkdir of %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "mkdir of %lu bytes", ul(bl));
	r = rmdir(big);
	if (!writable && r == -1 && errno == EROFS)
		info("rmdir of %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "rmdir of %lu bytes", ul(bl));
	r = unlink(big);
	if (!writable && r == -1 && errno == EROFS)
		info("unlink of %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "unlink of %lu bytes", ul(bl));
	r = truncate(big, 0);
	if (!writable && r == -1 && errno == EROFS)
		info("truncate of %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "truncate of %lu bytes", ul(bl));
	r = chmod(big, 0644);
	if (!writable && r == -1 && errno == EROFS)
		info("chmod of %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "chmod of %lu bytes", ul(bl));
	r = utimensat(AT_FDCWD, big, ts, 0);
	if (!writable && r == -1 && errno == EROFS)
		info("utimensat of %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "utimensat of %lu bytes", ul(bl));

	/* The other argument is a short, existing name. */
	if (writable) {
		pjoin(sh, sizeof(sh), g_base, "tl");
		write_file(sh, "tl");
	} else {
		snprintf(sh, sizeof(sh), "%s", g_dir);
	}
	r = rename(big, sh);
	if (!writable && r == -1 && errno == EROFS)
		info("rename from %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "rename from %lu bytes", ul(bl));
	r = rename(sh, big);
	if (!writable && r == -1 && errno == EROFS)
		info("rename to %lu bytes on read-only -> EROFS", ul(bl));
	else
		expect_err(r, ENAMETOOLONG, "rename to %lu bytes", ul(bl));
	r = symlink("x", big);
	if (!writable && r == -1 && errno == EROFS)
		info("symlink at %lu bytes on read-only -> EROFS", ul(bl));
	else if (writable && !g_links && r == -1 && link_unsupported(errno))
		info("symlink at %lu bytes -> %s (no links here)", ul(bl),
		     en(errno));
	else
		expect_err(r, ENAMETOOLONG, "symlink at a %lu-byte path",
			   ul(bl));
	if (writable) {
		check(read_matches(sh, "tl"), "the short name is untouched");
		unlink(sh);
		pjoin(sh, sizeof(sh), g_base, "Ltl");
		r = symlink(big, sh);
		if (!g_links && r == -1 && link_unsupported(errno))
			info("symlink with a %lu-byte target -> %s (no links "
			     "here)", ul(bl), en(errno));
		else
			expect_err(r, ENAMETOOLONG,
				   "symlink with a %lu-byte target", ul(bl));
		if (r == 0)
			unlink(sh);
	}

	/* The *at() calls with a relative path of the same length. */
	{
		int dfd = open(g_dir, O_RDONLY | O_DIRECTORY);

		make_len(rel, ".", bl, 'r');
		if (dfd < 0) {
			fail("open of %s -> %s", g_dir, en(errno));
			return;
		}
		expect_err(openat(dfd, rel, O_RDONLY), ENAMETOOLONG,
			   "openat of a relative %lu bytes", ul(strlen(rel)));
		expect_err(fstatat(dfd, rel, &st, 0), ENAMETOOLONG,
			   "fstatat of a relative %lu bytes", ul(bl));
		expect_err(faccessat(dfd, rel, F_OK, 0), ENAMETOOLONG,
			   "faccessat of a relative %lu bytes", ul(bl));
		expect_err(readlinkat(dfd, rel, sh, sizeof(sh)), ENAMETOOLONG,
			   "readlinkat of a relative %lu bytes", ul(bl));
		r = mkdirat(dfd, rel, 0755);
		if (!writable && r == -1 && errno == EROFS)
			info("mkdirat on read-only -> EROFS");
		else
			expect_err(r, ENAMETOOLONG,
				   "mkdirat of a relative %lu bytes", ul(bl));
		r = unlinkat(dfd, rel, 0);
		if (!writable && r == -1 && errno == EROFS)
			info("unlinkat on read-only -> EROFS");
		else
			expect_err(r, ENAMETOOLONG,
				   "unlinkat of a relative %lu bytes", ul(bl));
		r = renameat(dfd, rel, dfd, "x");
		if (!writable && r == -1 && errno == EROFS)
			info("renameat on read-only -> EROFS");
		else
			expect_err(r, ENAMETOOLONG,
				   "renameat of a relative %lu bytes", ul(bl));
		r = symlinkat("x", dfd, rel);
		if (!writable && r == -1 && errno == EROFS)
			info("symlinkat on read-only -> EROFS");
		else if (!g_links && r == -1 && link_unsupported(errno))
			info("symlinkat -> %s (no links here)", en(errno));
		else
			expect_err(r, ENAMETOOLONG,
				   "symlinkat at a relative %lu bytes", ul(bl));
		close(dfd);
	}
}

/* `d': an existing directory of length dl.  Paths of 4096 and more below
 * it, a 4095-byte one that does not exist, a 256-byte name, and relative
 * names that are too long only joined with the working directory. */
static void sec_toolong(const char *d, int writable)
{
	static char big[BIG], full[BIG];
	char cwd[PMAX], n[NAME_MAX + 2];
	const char *rel;
	struct stat st;
	size_t dl = strlen(d);
	int r, e;

	printf("-- 7. ENAMETOOLONG\n");
	if (dl + 2 > LIM) {
		info("directory too deep for these checks");
		return;
	}
	make_len(big, d, LIM, 'u');
	r = stat(big, &st);
	e = errno;
	check(r == -1 && e == ENOENT,
	      "stat of a missing path of exactly 4095 bytes -> %s "
	      "(expected ENOENT)", r ? en(e) : "success");
	make_len(big, d, LIM + 1, 'v');
	toolong_one(big, writable);
	make_len(big, d, 9000, 'w');
	toolong_one(big, writable);

	mkname(n, NAME_MAX + 1, 'z', 0);
	pjoin(big, sizeof(big), d, n);
	r = stat(big, &st);
	soft(r == -1 && errno == ENAMETOOLONG, g_cap,
	     "stat of a 256-byte name -> %s, expected ENAMETOOLONG",
	     r ? en(errno) : "success");

	/* Relative names joined with a deep working directory. */
	if (chdir(d) != 0) {
		fail("chdir to %lu bytes -> %s", ul(dl), en(errno));
		return;
	}
	check(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, d) == 0,
	      "getcwd at the %lu-byte directory", ul(dl));
	make_len(full, d, LIM + 1, 'j');
	rel = full + dl + 1;
	r = stat(rel, &st);
	e = errno;
	if (r == -1 && e == ENAMETOOLONG)
		pass("stat of a %lu-byte relative name, %lu bytes joined with "
		     "the cwd -> ENAMETOOLONG", ul(strlen(rel)), ul(LIM + 1));
	else if (r == -1 && e == ENOENT)
		info("stat of a relative name, %lu bytes joined with the cwd "
		     "-> ENOENT (no limit on the joined path)", ul(LIM + 1));
	else
		fail("stat of a relative name, %lu bytes joined with the cwd "
		     "-> %s, expected ENAMETOOLONG", ul(LIM + 1),
		     r ? en(e) : "success");
	if (writable && !strchr(rel, '/')) {
		int fd = open(rel, O_WRONLY | O_CREAT | O_EXCL, 0644);

		e = errno;
		if (fd == -1 && e == ENAMETOOLONG) {
			pass("open(O_CREAT) of a relative name, %lu bytes "
			     "joined with the cwd -> ENAMETOOLONG",
			     ul(LIM + 1));
		} else if (fd >= 0) {
			close(fd);
			info("open(O_CREAT) of a relative name, %lu bytes "
			     "joined with the cwd, created the file",
			     ul(LIM + 1));
			unlink(rel);
		} else {
			fail("open(O_CREAT) of a relative name, %lu bytes "
			     "joined with the cwd -> %s", ul(LIM + 1), en(e));
		}
		r = mkdir(rel, 0755);
		e = errno;
		if (r == -1 && e == ENAMETOOLONG) {
			pass("mkdir of a relative name, %lu bytes joined -> "
			     "ENAMETOOLONG", ul(LIM + 1));
		} else if (r == 0) {
			info("mkdir of a relative name, %lu bytes joined, "
			     "made the directory", ul(LIM + 1));
			rmdir(rel);
		} else {
			fail("mkdir of a relative name, %lu bytes joined -> %s",
			     ul(LIM + 1), en(e));
		}
		{
			int dfd = open(".", O_RDONLY | O_DIRECTORY);

			if (dfd >= 0) {
				fd = openat(dfd, rel, O_WRONLY | O_CREAT | O_EXCL,
					    0644);
				e = errno;
				if (fd == -1 && e == ENAMETOOLONG) {
					pass("openat(O_CREAT) of a name, %lu "
					     "bytes joined with the dirfd's "
					     "path -> ENAMETOOLONG",
					     ul(LIM + 1));
				} else if (fd >= 0) {
					close(fd);
					info("openat(O_CREAT) of a name, %lu "
					     "bytes joined with the dirfd's "
					     "path, created the file",
					     ul(LIM + 1));
					unlinkat(dfd, rel, 0);
				} else {
					fail("openat(O_CREAT) of a name, %lu "
					     "bytes joined -> %s", ul(LIM + 1),
					     en(e));
				}
				close(dfd);
			}
		}

		/* Joined, exactly 4095 bytes: works. */
		mkname(n, LIM - dl - 1, 'k', 0);
		expect_ok(write_file(n, "joined"),
			  "create a relative name, exactly 4095 bytes joined "
			  "with the cwd");
		check(read_matches(n, "joined"), "read it back");
		expect_ok(unlink(n), "unlink it by the relative name");
	} else if (!writable) {
		make_len(full, d, LIM, 'k');
		r = stat(full + dl + 1, &st);
		e = errno;
		check(r == -1 && e == ENOENT,
		      "stat of a missing relative name, exactly 4095 bytes "
		      "joined -> %s (expected ENOENT)", r ? en(e) : "success");
	}
	chdir("/");
}

/* ------------------------------ 8b. empty names, /dev names, chroot ---- */

/* An empty path names nothing: ENOENT for every call -- never the working
 * directory (it is a fresh, empty directory here, so a call that acted on it
 * shows).  The /dev decision of open() is made on the canonical name.  In a
 * chroot (root only, in a child), chdir() looks inside the jail and a
 * descriptor's relative paths stay in it. */
static void sec_names(void)
{
	char p[PMAX], q[PMAX], buf[64];
	struct stat st;
	int fd;

	printf("-- 8b. empty names, /dev names, chroot\n");
	pjoin(p, sizeof(p), g_base, "nm");
	if (mkdir(p, 0755) != 0) {
		fail("mkdir %s -> %s", p, en(errno));
		return;
	}
	pjoin(q, sizeof(q), p, "f");
	write_file(q, "nmf");
	if (chdir(p) != 0) {
		fail("chdir to the scratch directory -> %s", en(errno));
		return;
	}
	expect_err(open("", O_RDONLY), ENOENT, "open(\"\")");
	expect_err(open("", O_RDONLY | O_DIRECTORY), ENOENT,
		   "open(\"\", O_DIRECTORY)");
	expect_err(openat(AT_FDCWD, "", O_RDONLY), ENOENT,
		   "openat(AT_FDCWD, \"\")");
	expect_err(stat("", &st), ENOENT, "stat(\"\")");
	expect_err(lstat("", &st), ENOENT, "lstat(\"\")");
	expect_err(fstatat(AT_FDCWD, "", &st, 0), ENOENT,
		   "fstatat(AT_FDCWD, \"\", 0)");
	expect_err(access("", F_OK), ENOENT, "access(\"\")");
	expect_err(faccessat(AT_FDCWD, "", F_OK, 0), ENOENT,
		   "faccessat(\"\")");
	expect_err(chdir(""), ENOENT, "chdir(\"\")");
	expect_err(mkdir("", 0755), ENOENT, "mkdir(\"\")");
	expect_err(rmdir(""), ENOENT, "rmdir(\"\")");
	expect_err(unlink(""), ENOENT, "unlink(\"\")");
	expect_err(unlinkat(AT_FDCWD, "", AT_REMOVEDIR), ENOENT,
		   "unlinkat(\"\", AT_REMOVEDIR)");
	expect_err(rename("", "x"), ENOENT, "rename(\"\", x)");
	expect_err(rename("f", ""), ENOENT, "rename(f, \"\")");
	expect_err(link("f", ""), ENOENT, "link(f, \"\")");
	expect_err(link("", "x"), ENOENT, "link(\"\", x)");
	expect_err(symlink("", "x"), ENOENT, "symlink with an empty target");
	expect_err(symlink("f", ""), ENOENT, "symlink(f, \"\")");
	expect_err(readlink("", buf, sizeof(buf)), ENOENT,
		   "readlink(\"\")");
	expect_err(chmod("", 0700), ENOENT, "chmod(\"\")");
	expect_err(chown("", getuid(), getgid()), ENOENT, "chown(\"\")");
	expect_err(utimensat(AT_FDCWD, "", NULL, 0), ENOENT,
		   "utimensat(AT_FDCWD, \"\")");
	expect_err(truncate("", 0), ENOENT, "truncate(\"\")");
	expect_err(mkfifo("", 0600), ENOENT, "mkfifo(\"\")");
	expect_err(chroot(""), geteuid() == 0 ? ENOENT : EPERM,
		   "chroot(\"\")");
	{
		char *av[] = { (char *)"x", NULL };
		pid_t pid = fork();
		if (pid == 0) {
			execve("", av, av + 1);
			_exit(errno == ENOENT ? 0 : 1);
		}
		int ws = 0;
		waitpid(pid, &ws, 0);
		check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0,
		      "execve(\"\") -> ENOENT");
	}
	{
		char v[8];
		long r = getxattr("", "user.x", v, sizeof(v));
		if (r == -1 && errno == ENOENT)
			pass("getxattr(\"\") -> ENOENT");
		else if (r == -1 && (errno == ENOSYS || errno == EOPNOTSUPP))
			info("getxattr: %s", en(errno));
		else
			fail("getxattr(\"\") -> %s", r == -1 ? en(errno) : "ok");
	}
	fd = inotify_init1(0);
	if (fd >= 0) {
		expect_err(inotify_add_watch(fd, "", IN_ALL_EVENTS), ENOENT,
			   "inotify_add_watch(\"\")");
		close(fd);
	}
	/* AT_EMPTY_PATH still means the descriptor's own file */
	fd = open("f", O_RDONLY);
	expect_ok(fd >= 0 ? fstatat(fd, "", &st, AT_EMPTY_PATH) : -1,
		  "fstatat(fd, \"\", AT_EMPTY_PATH) is the file itself");
	if (fd >= 0)
		close(fd);
	check(is_dir(p) && read_matches(q, "nmf"),
	      "the working directory and its file are untouched");

	/* /dev names, decided on the canonical path */
	fd = open("//dev/null", O_RDWR);
	check(fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode),
	      "open(\"//dev/null\") is the device");
	if (fd >= 0)
		close(fd);
	fd = open("/dev/./null", O_RDWR);
	check(fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode),
	      "open(\"/dev/./null\") is the device");
	if (fd >= 0)
		close(fd);
	{
		char dq[PMAX];
		snprintf(dq, sizeof(dq), "/dev/..%s", q);
		check(read_matches(dq, "nmf"),
		      "open(\"/dev/..%s\") is that file, not a device", q);
	}
	chdir("/");

	/* chroot: in a child, so this process keeps its root */
	if (geteuid() != 0) {
		info("not root: the chroot checks are skipped");
		return;
	}
	{
		char top[PMAX];
		const char *s2 = strchr(g_base + 1, '/');
		size_t tl = s2 ? (size_t)(s2 - g_base) : 0;

		/* a directory that exists outside the jail and not in it:
		 * the first name of the work directory's path */
		memcpy(top, g_base, tl);
		top[tl] = '\0';
		pjoin(q, sizeof(q), p, "j/k");
		{
			char jd[PMAX];
			pjoin(jd, sizeof(jd), p, "j");
			mkdir(jd, 0755);
			mkdir(q, 0755);
		}
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			int bad = 0, dfd;

			if (chroot(p) != 0 || chdir("/") != 0)
				_exit(100);
			if (tl > 1 && chdir(top) == 0)
				bad |= 1; /* found outside the jail */
			dfd = open("/j", O_RDONLY | O_DIRECTORY);
			if (dfd < 0)
				bad |= 2;
			else {
				int k = openat(dfd, "k", O_RDONLY | O_DIRECTORY);
				if (k < 0)
					bad |= 4;
				else
					close(k);
				if (fstatat(dfd, "../j/k", &st, 0) != 0)
					bad |= 8;
				if (fstatat(dfd, "../../../f", &st, 0) != 0)
					bad |= 16; /* ".." stops at the jail */
				close(dfd);
			}
			if (chdir("/j") != 0 || chdir("k") != 0)
				bad |= 32;
			_exit(bad);
		}
		int ws = 0;
		waitpid(pid, &ws, 0);
		int code = WIFEXITED(ws) ? WEXITSTATUS(ws) : 255;
		if (code == 100)
			fail("chroot into the scratch directory failed");
		else {
			check(!(code & 1), "in a chroot, chdir(\"%s\") does not "
			      "reach outside the jail", top);
			check(!(code & 2), "in a chroot, open(\"/j\")");
			check(!(code & 4), "in a chroot, openat(dirfd, \"k\")");
			check(!(code & 8), "in a chroot, fstatat(dirfd, \"../j/k\")");
			check(!(code & 16), "in a chroot, \"..\" from a descriptor "
			      "stops at the jail's top");
			check(!(code & 32), "in a chroot, chdir(\"/j\") and "
			      "chdir(\"k\")");
		}
	}
}

/* ------------------------------------------------- 8. ordinary paths ---- */

static void sec_regress(void)
{
	char p[PMAX], q[PMAX], cwd[PMAX];
	struct stat st;
	int fd, r;

	printf("-- 8. ordinary paths\n");
	if (chdir(g_base) != 0) {
		fail("chdir to the work directory -> %s", en(errno));
		return;
	}
	expect_ok(mkdir("r", 0755), "mkdir r");
	expect_ok(mkdir("r/a", 0755), "mkdir r/a");
	expect_ok(mkdir("./r/b", 0755), "mkdir ./r/b");
	expect_ok(mkdir("r/a/s", 0755), "mkdir r/a/s");
	expect_ok(write_file("./r/x", "x"), "create ./r/x");
	check(stat("r/a/../b", &st) == 0 && S_ISDIR(st.st_mode),
	      "stat r/a/../b: a directory");
	check(read_matches("r/a/../x", "x"), "open r/a/../x");
	check(read_matches("./r/./x", "x"), "open ./r/./x");
	snprintf(p, sizeof(p), "%s//r///x", g_base);
	check(read_matches(p, "x"), "open BASE//r///x");
	snprintf(p, sizeof(p), "/%s/r/x", g_base);
	check(read_matches(p, "x"), "open //BASE/r/x");
	check(is_dir("r/a/"), "stat r/a/ (trailing slash)");
	check(is_dir("r/a//."), "stat r/a//.");
	expect_err(stat("r/x/", &st), ENOTDIR, "stat r/x/ (a file)");
	expect_err(open("r/x/", O_RDONLY), ENOTDIR, "open r/x/ (a file)");
	expect_ok(mkdir("r/c/", 0755), "mkdir r/c/ (trailing slash)");
	expect_ok(rmdir("r/c/"), "rmdir r/c/ (trailing slash)");
	expect_ok(rename("r/x", "r/b/../y"), "rename r/x r/b/../y");
	check(read_matches("r/y", "x"), "r/y holds it");
	expect_ok(chdir("r/a/../b/"), "chdir r/a/../b/");
	snprintf(p, sizeof(p), "%s/r/b", g_base);
	check(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, p) == 0, "getcwd there");
	chdir(g_base);

	if (g_links) {
		expect_ok(symlink("a", "r/la"), "symlink r/la -> a");
		expect_ok(write_file("r/a/f", "f"), "create r/a/f");
		check(read_matches("r/la/f", "f"), "open r/la/f (link in the middle)");
		snprintf(p, sizeof(p), "%s/r/la/s/../f", g_base);
		check(read_matches(p, "f"), "open BASE/r/la/s/../f");
		expect_ok(symlink("a/s", "r/ls"), "symlink r/ls -> a/s");
		snprintf(p, sizeof(p), "%s/r/a", g_base);
		snprintf(q, sizeof(q), "%s/r", g_base);
		r = stat("r/ls/..", &st);
		if (r == 0 && same_file("r/ls/..", p))
			pass("r/ls/.. is r/a (the link's target's parent)");
		else if (r == 0 && same_file("r/ls/..", q))
			info("r/ls/.. is r (\"..\" taken by name, not through "
			     "the link)");
		else
			fail("stat r/ls/.. -> %s", r ? en(errno) : "elsewhere");
		if (chdir("r/ls") == 0) {
			r = chdir("..");
			if (r == 0 && same_file(".", p))
				pass("chdir r/ls, chdir ..: r/a");
			else if (r == 0 && same_file(".", q))
				info("chdir r/ls, chdir ..: r (by name)");
			else
				fail("chdir r/ls, chdir .. -> %s",
				     r ? en(errno) : "elsewhere");
		} else {
			fail("chdir r/ls -> %s", en(errno));
		}
		chdir(g_base);
		unlink("r/ls");
		unlink("r/la");
		unlink("r/a/f");
	}
	fd = open("r", O_RDONLY | O_DIRECTORY);
	expect_ok(fd, "open r (O_DIRECTORY)");
	if (fd >= 0)
		close(fd);
	unlink("r/y");
	rmdir("r/a/s");
	rmdir("r/a");
	rmdir("r/b");
	expect_ok(rmdir("r"), "rmdir r after emptying it");
	chdir("/");
}

/* ------------------------------------------------------ read-only DIR ---- */

static char g_rdeep[PMAX], g_rfile[PMAX];
static size_t g_rdlen, g_rflen;
static long g_rvisit;

static void ro_walk(char *p, size_t len, int depth)
{
	DIR *d;
	struct dirent *e;
	struct stat st;

	if (depth > 64 || g_rvisit > 20000 || !(d = opendir(p)))
		return;
	while ((e = readdir(d)) != NULL) {
		size_t nl = strlen(e->d_name);

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (len + 1 + nl > LIM)
			continue;
		g_rvisit++;
		p[len] = '/';
		memcpy(p + len + 1, e->d_name, nl + 1);
		if (lstat(p, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				if (len + 1 + nl > g_rdlen) {
					g_rdlen = len + 1 + nl;
					memcpy(g_rdeep, p, g_rdlen + 1);
				}
				ro_walk(p, len + 1 + nl, depth + 1);
			} else if (S_ISREG(st.st_mode) && len + 1 + nl > g_rflen) {
				g_rflen = len + 1 + nl;
				memcpy(g_rfile, p, g_rflen + 1);
			}
		}
		p[len] = '\0';
	}
	closedir(d);
}

static void sec_readonly(int erofs_seen)
{
	char p[PMAX], cwd[PMAX];
	DIR *d;
	int r, levels = 0, bad = -1;

	printf("-- read-only: deepest existing paths\n");
	g_rdlen = g_rflen = 0;
	g_rvisit = 0;
	snprintf(p, sizeof(p), "%s", g_dir);
	snprintf(g_rdeep, sizeof(g_rdeep), "%s", g_dir);
	g_rdlen = strlen(g_dir);
	ro_walk(p, strcmp(g_dir, "/") ? strlen(g_dir) : 0, 0);
	info("walked %ld entries; deepest directory %lu bytes, longest file "
	     "path %lu bytes", g_rvisit, ul(g_rdlen), ul(g_rflen));
	check(is_dir(g_rdeep), "stat of the deepest directory");
	d = opendir(g_rdeep);
	check(d != NULL, "opendir of the deepest directory");
	if (d)
		closedir(d);
	if (g_rflen) {
		int fd = open(g_rfile, O_RDONLY);
		char c;

		check(fd >= 0 && read(fd, &c, 1) >= 0,
		      "open and read of the longest file path");
		if (fd >= 0)
			close(fd);
	}
	if (!erofs_seen) {
		pjoin(p, sizeof(p), strcmp(g_dir, "/") ? g_dir : "", "pathtest-ro");
		r = mkdir(p, 0755);
		if (r == -1 && errno == EROFS) {
			pass("mkdir on the read-only filesystem -> EROFS");
		} else if (r == 0) {
			rmdir(p);
			fail("mkdir on the read-only filesystem succeeded");
		} else {
			info("mkdir on the read-only filesystem -> %s",
			     en(errno));
		}
	} else {
		pass("mkdir on the read-only filesystem -> EROFS");
	}

	/* chdir down one name at a time, getcwd, back up with "..". */
	if (chdir(g_dir) != 0) {
		fail("chdir %s -> %s", g_dir, en(errno));
		return;
	}
	{
		char *s = g_rdeep + (strcmp(g_dir, "/") ? strlen(g_dir) : 0);
		char comp[NAME_MAX + 1];

		while (*s == '/') {
			size_t c = 0;

			s++;
			while (s[c] && s[c] != '/')
				c++;
			if (c > NAME_MAX)
				break;
			memcpy(comp, s, c);
			comp[c] = '\0';
			s += c;
			if (chdir(comp) != 0) {
				bad = levels;
				break;
			}
			levels++;
		}
	}
	check(bad < 0, "chdir down %d levels one name at a time", levels);
	check(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, g_rdeep) == 0,
	      "getcwd at the deepest directory");
	expect_err(getcwd(cwd, g_rdlen) ? 0 : -1, ERANGE,
		   "getcwd with a %lu-byte buffer", ul(g_rdlen));
	bad = -1;
	snprintf(p, sizeof(p), "%s", g_rdeep);
	for (int k = levels; k > 0; k--) {
		char *sl = strrchr(p, '/');

		if (chdir("..") != 0 || !getcwd(cwd, sizeof(cwd))) {
			bad = k;
			break;
		}
		if (sl == p)
			sl[1] = '\0';
		else if (sl)
			*sl = '\0';
		if (strcmp(cwd, p) != 0) {
			bad = k;
			break;
		}
	}
	check(bad < 0, "chdir(\"..\") back up: getcwd right at every level");
	chdir("/");

	/* Ordinary paths. */
	snprintf(p, sizeof(p), "%s/.", g_dir);
	check(same_file(p, g_dir), "stat DIR/.");
	snprintf(p, sizeof(p), "%s//", g_dir);
	check(is_dir(p), "stat DIR//");
	if (g_rdlen > strlen(g_dir)) {
		snprintf(p, sizeof(p), "%s/..", g_rdeep);
		snprintf(cwd, sizeof(cwd), "%s", g_rdeep);
		*strrchr(cwd, '/') = '\0';
		if (!cwd[0])
			strcpy(cwd, "/");
		check(same_file(p, cwd), "stat DEEPEST/.. is its parent");
		snprintf(p, sizeof(p), "%s/", g_rdeep);
		check(is_dir(p), "stat DEEPEST/ (trailing slash)");
	}
	sec_toolong(g_rdeep, 0);
}

/* ------------------------------------------------------------- per DIR ---- */

static void detect_fs(const char *dir)
{
	struct statfs sf;
	struct statvfs sv;

	g_fsname = "unknown";
	g_ro = g_noexec = g_cap = g_tmpfs = 0;
	if (statfs(dir, &sf) == 0) {
		switch (sf.f_type) {
		case EXT4_MAGIC:
			g_fsname = "ext4";
			break;
		case TMPFS_MAGIC:
			g_fsname = "tmpfs";
			g_tmpfs = 1;
			break;
		case MSDOS_MAGIC:
			g_fsname = "vfat";
			g_cap = 1;
			break;
		case EXFAT_MAGIC:
			g_fsname = "exfat";
			g_cap = 1;
			break;
		case NTFS_MAGIC:
			g_fsname = "ntfs";
			g_cap = 1;
			break;
		case ISOFS_MAGIC:
			g_fsname = "iso9660";
			g_cap = 1;
			break;
		default:
			break;
		}
	}
	if (statvfs(dir, &sv) == 0) {
		if (sv.f_flag & ST_RDONLY)
			g_ro = 1;
		if (sv.f_flag & ST_NOEXEC)
			g_noexec = 1;
		info("filesystem %s%s%s, names up to %lu bytes", g_fsname,
		     g_ro ? ", read-only" : "", g_noexec ? ", noexec" : "",
		     sv.f_namemax);
	}
	if (access(dir, W_OK) != 0 && errno == EROFS)
		g_ro = 1;
}

static void run_dir(const char *arg, int idx)
{
	int f0 = g_fail, p0 = g_pass, i0 = g_info, erofs = 0;

	printf("==== %s\n", arg);
	chdir("/");
	g_tree = 0;
	g_ncomp = 0;
	g_links = 0;
	g_base[0] = '\0';
	if (!realpath(arg, g_dir)) {
		fail("%s: cannot resolve -> %s", arg, en(errno));
		return;
	}
	if (!is_dir(g_dir)) {
		fail("%s: not a directory", g_dir);
		return;
	}
	detect_fs(g_dir);
	if (!g_ro) {
		int e = EEXIST;

		for (int n = 0; n < 100 && e == EEXIST; n++) {
			snprintf(g_base, sizeof(g_base), "%s/pathtest.%d.%d.%d",
				 strcmp(g_dir, "/") ? g_dir : "", (int)getpid(),
				 idx, n);
			e = mkdir(g_base, 0755) == 0 ? 0 : errno;
		}
		if (e != 0) {
			if (e == EROFS) {
				g_ro = 1;
				erofs = 1;
			} else {
				info("cannot make a work directory in %s (%s): "
				     "read-only checks only", g_dir, en(e));
				g_ro = 1;
			}
			g_base[0] = '\0';
		}
	}
	if (g_ro) {
		sec_readonly(erofs);
	} else {
		g_blen = strlen(g_base);
		info("work directory %s", g_base);
		if (g_blen + 300 > TDEEP) {
			fail("the path of DIR is too long for the deep checks");
		} else {
			sec_tree();
			if (g_tree) {
				sec_files();
				sec_cwd();
				sec_links();
				sec_rename();
				sec_exec();
				sec_toolong(g_deep, 1);
			} else {
				fail("no deep tree: sections 2-7 skipped");
			}
		}
		sec_regress();
		sec_names();

		printf("-- 9. cleanup\n");
		chdir("/");
		if (rm_tree(g_base) != 0)
			rm_tree(g_base);
		check(gone(g_base), "the work directory and everything in it "
		      "removed");
		if (!gone(g_base))
			info("left behind: %s", g_base);
	}
	printf("==== %s: %d passed, %d failed, %d info\n\n", g_dir,
	       g_pass - p0, g_fail - f0, g_info - i0);
	fflush(stdout);
}

/* The program the exec checks copy: this one, or a "true". */
static void find_src(const char *argv0)
{
	char p[PMAX];

	g_src[0] = '\0';
	g_self = 0;
	if (is_elf("/proc/self/exe")) {
		strcpy(g_src, "/proc/self/exe");
		g_self = 1;
		return;
	}
	if (strchr(argv0, '/') && realpath(argv0, p) && is_elf(p)) {
		strcpy(g_src, p);
		g_self = 1;
		return;
	}
	{
		const char *path = getenv("PATH");
		const char *s = path ? path : "/bin:/usr/bin:/usr/local/bin";

		while (*s) {
			const char *c = strchr(s, ':');
			size_t n = c ? (size_t)(c - s) : strlen(s);

			if (n && n + 2 + strlen(argv0) < sizeof(p)) {
				memcpy(p, s, n);
				p[n] = '/';
				strcpy(p + n + 1, argv0);
				if (is_elf(p)) {
					strcpy(g_src, p);
					g_self = 1;
					return;
				}
			}
			s += n;
			if (*s == ':')
				s++;
		}
	}
	if (is_elf("/bin/true"))
		strcpy(g_src, "/bin/true");
	else if (is_elf("/usr/bin/true"))
		strcpy(g_src, "/usr/bin/true");
}

int main(int argc, char **argv)
{
	/* Used by the exec checks on a copy of this program. */
	if (argc >= 3 && strcmp(argv[1], "--exit") == 0)
		return atoi(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "--script") == 0)
		return access(argv[2], R_OK) == 0 ? 42 : 43;

	if (argc < 2 || argv[1][0] == '-') {
		fprintf(stderr,
			"usage: pathtest DIR [DIR...]\n"
			"  checks paths of up to %d bytes in a work directory "
			"made in each DIR\n"
			"  (removed afterwards); a read-only DIR gets "
			"read-only checks\n",
			LIM);
		return 2;
	}
	find_src(argv[0]);
	if (g_src[0])
		printf("exec checks copy %s%s\n", g_src,
		       g_self ? " (this program)" : "");
	for (int i = 1; i < argc; i++)
		run_dir(argv[i], i);
	printf("pathtest: %d passed, %d failed, %d info\n", g_pass, g_fail,
	       g_info);
	return g_fail > 125 ? 125 : g_fail;
}
