/*
 * mntent.h - reading and writing mount tables (fstab, mtab, /proc/mounts)
 *
 * A mount table is a text file with one filesystem per line and six fields
 * separated by blanks: the device, the mount point, the filesystem type, the
 * comma-separated options, the dump frequency and the fsck pass number.  The
 * last two are optional and read as 0 when missing.  Blanks inside a field are
 * written as octal escapes (\040 for a space, \011 tab, \012 newline, \134 a
 * backslash).  Lines starting with '#' and blank lines are skipped.
 *
 * getmntent() returns a pointer to storage that the next call overwrites;
 * getmntent_r() is the reentrant form that fills caller-supplied storage.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _MNTENT_H
#define _MNTENT_H

#include <stdio.h>
#include <paths.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The traditional names of the two tables. */
#define MNTTAB  _PATH_MNTTAB    /* static table: "/etc/fstab" */
#define MOUNTED _PATH_MOUNTED   /* table of mounted filesystems: "/etc/mtab" */

/* Common filesystem types. */
#define MNTTYPE_IGNORE "ignore" /* entry to be ignored */
#define MNTTYPE_NFS    "nfs"
#define MNTTYPE_SWAP   "swap"

/* Common mount options. */
#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO       "ro"
#define MNTOPT_RW       "rw"
#define MNTOPT_SUID     "suid"
#define MNTOPT_NOSUID   "nosuid"
#define MNTOPT_NOAUTO   "noauto"

struct mntent {
	char *mnt_fsname; /* device or server of the filesystem */
	char *mnt_dir;    /* where it is mounted */
	char *mnt_type;   /* filesystem type */
	char *mnt_opts;   /* comma-separated options */
	int mnt_freq;     /* dump frequency in days */
	int mnt_passno;   /* fsck pass number */
};

FILE *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
struct mntent *getmntent_r(FILE *stream, struct mntent *mntbuf, char *buf,
			   int buflen);
int addmntent(FILE *stream, const struct mntent *mnt);
int endmntent(FILE *stream);
char *hasmntopt(const struct mntent *mnt, const char *opt);

#ifdef __cplusplus
}
#endif

#endif /* _MNTENT_H */
