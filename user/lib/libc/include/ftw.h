/*
 * ftw.h - file tree walk.
 *
 * nftw() walks a directory hierarchy calling `fn` once per entry.  The
 * callback receives the path, that entry's stat result (or the reason it
 * has none), a type code, and a struct FTW locating the basename and depth.
 * ftw() is the historical interface, kept as a thin wrapper.
 */
#ifndef _FTW_H
#define _FTW_H

#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type codes handed to the callback. */
#define FTW_F   0  /* regular (or other non-directory) file */
#define FTW_D   1  /* directory, reported before its contents */
#define FTW_DNR 2  /* directory that could not be read */
#define FTW_NS  3  /* stat failed; the struct stat contents are undefined */
#define FTW_SL  4  /* symbolic link (with FTW_PHYS) */
#define FTW_DP  5  /* directory, reported AFTER its contents (FTW_DEPTH) */
#define FTW_SLN 6  /* symbolic link naming a nonexisting file */

/* Flags to nftw(). */
#define FTW_PHYS   1  /* lstat, do not follow symbolic links */
#define FTW_MOUNT  2  /* stay within one filesystem */
#define FTW_CHDIR  4  /* chdir into each directory before reading it */
#define FTW_DEPTH  8  /* depth-first: contents before the directory itself */

struct FTW {
	int base;  /* offset of the basename within the path argument */
	int level; /* depth below the starting point (start itself is 0) */
};

int nftw(const char *path,
	 int (*fn)(const char *, const struct stat *, int, struct FTW *),
	 int fd_limit, int flags);

int ftw(const char *path,
	int (*fn)(const char *, const struct stat *, int), int fd_limit);

#ifdef __cplusplus
}
#endif

#endif /* _FTW_H */
