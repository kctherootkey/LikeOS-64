/*
 * fstab-util - what init, mount and umount share about mount tables.
 *
 * Reading /etc/fstab and /proc/mounts into memory, splitting a mount
 * option string into the mount(2) flags and the filesystem's own options,
 * finding the device a source names (UUID=, LABEL=, PARTUUID=, PARTLABEL=
 * through /dev/disk/by-*), waiting for a device to appear, and the checker
 * (fsck) of a filesystem type.
 *
 * Nothing in here prints anything or exits: the callers decide what a
 * failure means and how to say it.  Functions returning int give 0 on
 * success and a negative errno value on failure.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_FSTAB_UTIL_H
#define LIKEOS_FSTAB_UTIL_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/mount.h>

#define FSU_FSTAB  "/etc/fstab"
#define FSU_MOUNTS "/proc/mounts"

/* Longest path handled (the kernel's VFS_MAX_PATH is 512). */
#define FSU_PATH_MAX 512
/* Longest filesystem-specific option string mount(2) takes. */
#define FSU_DATA_MAX 1024

/* ---- mount tables ------------------------------------------------------ */

/* One line of a mount table, with private copies of the fields. */
struct fsu_entry {
	char *spec;   /* device or label: /dev/sda1, UUID=..., tmpfs */
	char *dir;    /* mount point */
	char *type;   /* filesystem type */
	char *opts;   /* comma-separated options */
	int freq;     /* dump frequency */
	int passno;   /* fsck pass */
	int line;     /* position in the table, 1-based */
};

struct fsu_table {
	struct fsu_entry *e;
	int n;
	int cap;
};

/* Reads a whole table (fstab or /proc/mounts).  An empty file gives an
 * empty table; a missing one -ENOENT. */
int fsu_table_load(const char *path, struct fsu_table *t);
void fsu_table_free(struct fsu_table *t);

/* The LAST entry of a mount table whose mount point is `dir' (the one on
 * top), or NULL. */
const struct fsu_entry *fsu_table_find_dir(const struct fsu_table *t,
					   const char *dir);
/* The last entry whose source is `spec', or NULL. */
const struct fsu_entry *fsu_table_find_spec(const struct fsu_table *t,
					    const char *spec);

/* ---- options ------------------------------------------------------------ */

struct fsu_opts {
	unsigned long flags;     /* MS_* to pass to mount(2) */
	unsigned long given;     /* MS_* bits named either way (set/cleared) */
	char data[FSU_DATA_MAX]; /* the filesystem's own options */
	int ro_given;            /* "ro", "rw" or "defaults" appeared */
	int rw_given;            /* "rw" itself appeared last of ro/rw */
	int noauto;
	int nofail;
	int remount;
	int bind;                /* bind / rbind / move: not offered */
	int loop;                /* loop[=...]: not offered */
	int mkdir;               /* X-mount.mkdir[=mode]: create the point */
	mode_t mkdir_mode;
};

/* Splits `opts' (may be NULL) into flags and filesystem data, on top of
 * what `o' already holds (fsu_opts_init() first).  Later words win over
 * earlier ones, so "ro,rw" is read-write.  -E2BIG when the filesystem
 * options do not fit. */
void fsu_opts_init(struct fsu_opts *o);
int fsu_opts_parse(struct fsu_opts *o, const char *opts);

/* The flag words of `flags' ("ro,nosuid,noatime"), always starting with
 * ro or rw. */
void fsu_flags_text(unsigned long flags, char *buf, size_t len);

/* The MS_* flags an option string of /proc/mounts names. */
unsigned long fsu_flags_of(const char *opts);

/* The MS_* bits that matter when comparing two mounts' flags. */
#define FSU_FLAG_MASK                                                        \
	(MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_SYNCHRONOUS |    \
	 MS_DIRSYNC | MS_NOSYMFOLLOW | MS_NOATIME | MS_NODIRATIME |         \
	 MS_RELATIME | MS_STRICTATIME | MS_LAZYTIME)

/* Is `word' (no '=') one of the options of the comma list `opts'? */
int fsu_has_opt(const char *opts, const char *word);

/* ---- sources ------------------------------------------------------------ */

enum fsu_tag {
	FSU_TAG_NONE = 0,
	FSU_TAG_UUID,
	FSU_TAG_LABEL,
	FSU_TAG_PARTUUID,
	FSU_TAG_PARTLABEL,
};

/* The kind of tag `spec' is ("UUID=..." etc.) and its value without
 * quotes in `val'; FSU_TAG_NONE for anything else. */
int fsu_spec_tag(const char *spec, char *val, size_t len);

/* Does `spec' name a block device (a tag or a path below /dev)? */
int fsu_spec_is_device(const char *spec);

/* The device `spec' names, as its real path (/dev/sda1), in `out'.  A tag
 * is looked up in /dev/disk/by-<tag>; a path is resolved through its
 * links.  -ENOENT while there is no such device (yet), -EINVAL for a spec
 * that is not a device at all.  A label such as "tmpfs" is copied
 * unchanged with 0. */
int fsu_resolve(const char *spec, char *out, size_t len);

/* fsu_resolve(), retried every 100 ms while the device is missing and the
 * monotonic clock is before `deadline_ms' (fsu_now_ms() time base). */
int fsu_wait_resolve(const char *spec, char *out, size_t len,
		     long long deadline_ms);

/* Milliseconds of the monotonic clock. */
long long fsu_now_ms(void);

/* The escaped form a /dev/disk/by-label or by-partlabel link name has
 * ("My Disk" -> "My\x20Disk"). */
int fsu_link_escape(const char *in, char *out, size_t len);

/* ---- paths ----------------------------------------------------------------- */

/* An absolute, canonical mount point: relative paths are taken from the
 * working directory, "." and ".." and doubled slashes go, a trailing slash
 * goes, and links are resolved when the path exists. */
int fsu_canon_dir(const char *in, char *out, size_t len);

/* mkdir -p with `mode' for every directory made. */
int fsu_mkdir_p(const char *path, mode_t mode);

/* ---- filesystem checkers ------------------------------------------------- */

/* The checker of `type' and the argument for an automatic repair run:
 * ext2/3/4 /sbin/fsck.<type> (e2fsck) -p, vfat /sbin/fsck.vfat -a, any
 * other type /sbin/fsck.<type> -p.  -ENOENT when there is no checker. */
int fsu_fsck_program(const char *type, char *prog, size_t len,
		     const char **arg);

/* Is `type' a FAT type (vfat, fat32, msdos, fat)? */
int fsu_type_is_fat(const char *type);

/* Kernel filesystems mounted at boot (proc, sysfs, devfs ...): never
 * unmounted, never mounted from a table. */
int fsu_type_is_kernel(const char *type);

#endif /* LIKEOS_FSTAB_UTIL_H */
