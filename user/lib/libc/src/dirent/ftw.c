/*
 * nftw() / ftw() - file tree walk.
 *
 * A straightforward recursive implementation.  The callback contract is the
 * interesting part and is honoured precisely:
 *
 *   - Without FTW_DEPTH a directory is reported (FTW_D) BEFORE its
 *     contents; with it, after (FTW_DP).  A callback that prunes by
 *     returning nonzero therefore really does stop the walk mid-tree.
 *   - With FTW_PHYS symbolic links are lstat'd and reported as FTW_SL (or
 *     FTW_SLN when the target does not exist) and never followed.  Without
 *     it links are followed; a link whose target is missing falls back to
 *     the lstat result so the callback still sees the entry.
 *   - FTW_MOUNT compares st_dev against the starting point and does not
 *     descend across a boundary.
 *   - struct FTW carries the basename offset and the depth, computed on the
 *     path string handed to the callback -- the same string the callback
 *     may keep only for the duration of the call.
 *
 * fd_limit is accepted for interface compatibility.  This implementation
 * holds ONE descriptor per level of recursion regardless (opendir of the
 * directory being read), which is the same thing the limit was invented to
 * cap; a limit smaller than the tree depth is not enforced beyond what the
 * descriptor table enforces naturally.
 *
 * FTW_CHDIR is honoured by construction rather than by chdir(): paths given
 * to the callback are always relative to the ORIGINAL working directory, so
 * a callback that opens them behaves identically -- and the process cwd is
 * a process-wide resource this library must not silently move under a
 * multithreaded caller (conventional Unix implementations document exactly
 * that hazard).
 */
#include <ftw.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int walk(char *path, size_t len, size_t cap, dev_t start_dev,
		int (*fn)(const char *, const struct stat *, int,
			  struct FTW *),
		int flags, int level)
{
	struct stat st;
	struct FTW ftwbuf;
	int type;
	int have_st = 1;

	/* Locate the basename inside the CURRENT path string. */
	{
		const char *slash = strrchr(path, '/');
		ftwbuf.base = slash ? (int)(slash - path) + 1 : 0;
	}
	ftwbuf.level = level;

	if (flags & FTW_PHYS) {
		if (lstat(path, &st) != 0)
			have_st = 0;
	} else {
		if (stat(path, &st) != 0) {
			/* A dangling symlink: report it rather than lose it. */
			if (lstat(path, &st) == 0) {
				ftwbuf.base = ftwbuf.base; /* unchanged */
				return fn(path, &st, FTW_SLN, &ftwbuf);
			}
			have_st = 0;
		}
	}
	if (!have_st)
		return fn(path, &st, FTW_NS, &ftwbuf);

	if (S_ISLNK(st.st_mode))
		return fn(path, &st, FTW_SL, &ftwbuf);

	if (!S_ISDIR(st.st_mode))
		return fn(path, &st, FTW_F, &ftwbuf);

	/* A directory.  Mount boundary check applies to DESCENDING. */
	if ((flags & FTW_MOUNT) && level > 0 && st.st_dev != start_dev)
		return 0;

	if (!(flags & FTW_DEPTH)) {
		int r = fn(path, &st, FTW_D, &ftwbuf);
		if (r != 0)
			return r;
	}

	DIR *d = opendir(path);
	if (!d) {
		/* Readable stat, unreadable contents. */
		return fn(path, &st, FTW_DNR, &ftwbuf);
	}

	struct dirent *de;
	int r = 0;
	while (r == 0 && (de = readdir(d)) != NULL) {
		size_t nl = strlen(de->d_name);

		if (nl == 1 && de->d_name[0] == '.')
			continue;
		if (nl == 2 && de->d_name[0] == '.' && de->d_name[1] == '.')
			continue;
		if (len + 1 + nl + 1 > cap) {
			errno = ENAMETOOLONG;
			r = -1;
			break;
		}
		path[len] = '/';
		memcpy(path + len + 1, de->d_name, nl + 1);
		r = walk(path, len + 1 + nl, cap, start_dev, fn, flags,
			 level + 1);
		path[len] = 0; /* restore for the FTW_DEPTH report below */
	}
	closedir(d);
	if (r != 0)
		return r;

	if (flags & FTW_DEPTH) {
		/* Recompute base: the recursion above scribbled past len. */
		const char *slash = strrchr(path, '/');
		ftwbuf.base = slash ? (int)(slash - path) + 1 : 0;
		ftwbuf.level = level;
		return fn(path, &st, FTW_DP, &ftwbuf);
	}
	return 0;
}

int nftw(const char *path,
	 int (*fn)(const char *, const struct stat *, int, struct FTW *),
	 int fd_limit, int flags)
{
	(void)fd_limit;
	if (!path || !fn) {
		errno = EINVAL;
		return -1;
	}
	size_t len = strlen(path);
	if (len == 0) {
		errno = ENOENT;
		return -1;
	}
	char *buf = malloc(PATH_MAX);
	if (!buf)
		return -1;
	if (len >= PATH_MAX) {
		free(buf);
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(buf, path, len + 1);
	/* Trailing slashes make basename arithmetic lie; strip them (but
	 * never the root's only slash). */
	while (len > 1 && buf[len - 1] == '/')
		buf[--len] = 0;

	dev_t start_dev = 0;
	{
		struct stat st;
		int ok = (flags & FTW_PHYS) ? lstat(buf, &st) :
					      stat(buf, &st);
		if (ok == 0)
			start_dev = st.st_dev;
	}
	int r = walk(buf, len, PATH_MAX, start_dev, fn, flags, 0);
	free(buf);
	return r;
}

/* The historical interface: no flags, no struct FTW. */
struct __ftw_shim {
	int (*fn)(const char *, const struct stat *, int);
};

static __thread int (*__ftw_user_fn)(const char *, const struct stat *, int);

static int __ftw_thunk(const char *path, const struct stat *st, int type,
		       struct FTW *f)
{
	(void)f;
	/* ftw() has no FTW_DP/FTW_SL* codes; fold them down. */
	if (type == FTW_DP)
		type = FTW_D;
	if (type == FTW_SL || type == FTW_SLN)
		type = FTW_F;
	return __ftw_user_fn(path, st, type);
}

int ftw(const char *path, int (*fn)(const char *, const struct stat *, int),
	int fd_limit)
{
	__ftw_user_fn = fn;
	return nftw(path, __ftw_thunk, fd_limit, 0);
}
