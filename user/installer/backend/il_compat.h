/* libinstaller -- system calls that a C library may not offer yet.
 *
 * The installer runs on the host (tests, against glibc) and on LikeOS.
 * Some calls it needs -- mount, umount2, the *at family, lchown, syncfs --
 * are being added to the LikeOS C library while this code is written.  The
 * LikeOS build passes -DIL_HAVE_<CALL>=1 for every call its <unistd.h>,
 * <fcntl.h>, <sys/stat.h> and <sys/mount.h> declare (mk/installer.mk greps
 * the headers), so a call that appears later is picked up by the next
 * build without a change here.  A host build against glibc has them all.
 *
 * Every wrapper takes the directory descriptor + name AND the full path of
 * the same object: with the *at call the descriptor form is used (safe
 * against symbolic links planted in the path); without it the path form is
 * used, after the caller checked the parent chain with lstat.  Calls with
 * no path equivalent (mount, umount2) report ENOSYS at run time.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef IL_COMPAT_H
#define IL_COMPAT_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <sys/mount.h>
#ifndef IL_HAVE_MOUNT
#define IL_HAVE_MOUNT 1
#endif
#ifndef IL_HAVE_UMOUNT2
#define IL_HAVE_UMOUNT2 1
#endif
#ifndef IL_HAVE_MKDIRAT
#define IL_HAVE_MKDIRAT 1
#endif
#ifndef IL_HAVE_SYMLINKAT
#define IL_HAVE_SYMLINKAT 1
#endif
#ifndef IL_HAVE_LINKAT
#define IL_HAVE_LINKAT 1
#endif
#ifndef IL_HAVE_FCHOWNAT
#define IL_HAVE_FCHOWNAT 1
#endif
#ifndef IL_HAVE_LCHOWN
#define IL_HAVE_LCHOWN 1
#endif
#ifndef IL_HAVE_FCHMODAT
#define IL_HAVE_FCHMODAT 1
#endif
#ifndef IL_HAVE_RENAMEAT
#define IL_HAVE_RENAMEAT 1
#endif
#ifndef IL_HAVE_SYNCFS
#define IL_HAVE_SYNCFS 1
#endif
#ifndef IL_HAVE_MKNODAT
#define IL_HAVE_MKNODAT 1
#endif
#ifndef IL_HAVE_UTIMENSAT
#define IL_HAVE_UTIMENSAT 1
#endif
#ifndef IL_HAVE_FUTIMENS
#define IL_HAVE_FUTIMENS 1
#endif
#else /* the LikeOS C library: what mk/installer.mk found */
#include <sys/mount.h>
#ifndef IL_HAVE_MOUNT
#define IL_HAVE_MOUNT 0
#endif
#ifndef IL_HAVE_UMOUNT2
#define IL_HAVE_UMOUNT2 0
#endif
#ifndef IL_HAVE_MKDIRAT
#define IL_HAVE_MKDIRAT 0
#endif
#ifndef IL_HAVE_SYMLINKAT
#define IL_HAVE_SYMLINKAT 0
#endif
#ifndef IL_HAVE_LINKAT
#define IL_HAVE_LINKAT 0
#endif
#ifndef IL_HAVE_FCHOWNAT
#define IL_HAVE_FCHOWNAT 0
#endif
#ifndef IL_HAVE_LCHOWN
#define IL_HAVE_LCHOWN 0
#endif
#ifndef IL_HAVE_FCHMODAT
#define IL_HAVE_FCHMODAT 0
#endif
#ifndef IL_HAVE_RENAMEAT
#define IL_HAVE_RENAMEAT 0
#endif
#ifndef IL_HAVE_SYNCFS
#define IL_HAVE_SYNCFS 0
#endif
#ifndef IL_HAVE_MKNODAT
#define IL_HAVE_MKNODAT 0
#endif
#ifndef IL_HAVE_UTIMENSAT
#define IL_HAVE_UTIMENSAT 0
#endif
#ifndef IL_HAVE_FUTIMENS
#define IL_HAVE_FUTIMENS 0
#endif
#endif

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_SYMLINK_FOLLOW
#define AT_SYMLINK_FOLLOW 0x400
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0200000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT ((1L << 30) - 2L)
#endif
#ifndef UTIME_NOW
#define UTIME_NOW ((1L << 30) - 1L)
#endif

/* Mount flags of the established interface, for C libraries whose
 * <sys/mount.h> does not spell them yet. */
#ifndef MS_RDONLY
#define MS_RDONLY      1UL
#endif
#ifndef MS_NOSUID
#define MS_NOSUID      2UL
#endif
#ifndef MS_NODEV
#define MS_NODEV       4UL
#endif
#ifndef MS_NOEXEC
#define MS_NOEXEC      8UL
#endif
#ifndef MS_SYNCHRONOUS
#define MS_SYNCHRONOUS 16UL
#endif
#ifndef MS_REMOUNT
#define MS_REMOUNT     32UL
#endif
#ifndef MS_NOATIME
#define MS_NOATIME     1024UL
#endif
#ifndef MS_RELATIME
#define MS_RELATIME    (1UL << 21)
#endif
#ifndef MNT_FORCE
#define MNT_FORCE      1
#endif
#ifndef MNT_DETACH
#define MNT_DETACH     2
#endif
#ifndef UMOUNT_NOFOLLOW
#define UMOUNT_NOFOLLOW 8
#endif

/* ---- wrappers ---------------------------------------------------- */

/* All of them return 0 / -1 with errno set, like the calls themselves. */

static inline int il_sys_mount(const char *src, const char *target,
			       const char *fstype, unsigned long flags,
			       const void *data)
{
#if IL_HAVE_MOUNT
	return mount(src, target, fstype, flags, data);
#else
	(void)src; (void)target; (void)fstype; (void)flags; (void)data;
	errno = ENOSYS;
	return -1;
#endif
}

static inline int il_sys_umount2(const char *target, int flags)
{
#if IL_HAVE_UMOUNT2
	return umount2(target, flags);
#else
	(void)target; (void)flags;
	errno = ENOSYS;
	return -1;
#endif
}

static inline int il_mkdirat(int dirfd, const char *name, const char *full,
			     mode_t mode)
{
#if IL_HAVE_MKDIRAT
	(void)full;
	return mkdirat(dirfd, name, mode);
#else
	(void)dirfd; (void)name;
	return mkdir(full, mode);
#endif
}

static inline int il_symlinkat(const char *target, int dirfd,
			       const char *name, const char *full)
{
#if IL_HAVE_SYMLINKAT
	(void)full;
	return symlinkat(target, dirfd, name);
#else
	(void)dirfd; (void)name;
	return symlink(target, full);
#endif
}

/* A hard link: never follows a symbolic link named as the source. */
static inline int il_linkat(int olddirfd, const char *oldname,
			    const char *oldfull, int newdirfd,
			    const char *newname, const char *newfull)
{
#if IL_HAVE_LINKAT
	(void)oldfull; (void)newfull;
	return linkat(olddirfd, oldname, newdirfd, newname, 0);
#else
	(void)olddirfd; (void)oldname; (void)newdirfd; (void)newname;
	return link(oldfull, newfull);
#endif
}

/* Ownership of the object itself, never of what a symbolic link names.
 * Without fchownat/lchown a symbolic link keeps the owner it was created
 * with: ENOSYS tells the caller so. */
static inline int il_lchownat(int dirfd, const char *name, const char *full,
			      uid_t uid, gid_t gid, int is_symlink)
{
#if IL_HAVE_FCHOWNAT
	(void)full; (void)is_symlink;
	return fchownat(dirfd, name, uid, gid, AT_SYMLINK_NOFOLLOW);
#elif IL_HAVE_LCHOWN
	(void)dirfd; (void)name; (void)is_symlink;
	return lchown(full, uid, gid);
#else
	(void)dirfd; (void)name;
	if (is_symlink) {
		errno = ENOSYS;
		return -1;
	}
	return chown(full, uid, gid);
#endif
}

/* Permissions of an object that is not a symbolic link. */
static inline int il_fchmodat(int dirfd, const char *name, const char *full,
			      mode_t mode)
{
#if IL_HAVE_FCHMODAT
	(void)full;
	return fchmodat(dirfd, name, mode, 0);
#else
	(void)dirfd; (void)name;
	return chmod(full, mode);
#endif
}

static inline int il_renameat(int olddirfd, const char *oldname,
			      const char *oldfull, int newdirfd,
			      const char *newname, const char *newfull)
{
#if IL_HAVE_RENAMEAT
	(void)oldfull; (void)newfull;
	return renameat(olddirfd, oldname, newdirfd, newname);
#else
	(void)olddirfd; (void)oldname; (void)newdirfd; (void)newname;
	return rename(oldfull, newfull);
#endif
}

static inline int il_mknodat(int dirfd, const char *name, const char *full,
			     mode_t mode, dev_t dev)
{
#if IL_HAVE_MKNODAT
	(void)full;
	return mknodat(dirfd, name, mode, dev);
#else
	(void)dirfd; (void)name;
	return mknod(full, mode, dev);
#endif
}

/* Timestamps of the object itself (AT_SYMLINK_NOFOLLOW). */
static inline int il_utimensat_nofollow(int dirfd, const char *name,
					const char *full,
					const struct timespec ts[2])
{
#if IL_HAVE_UTIMENSAT
	(void)full;
	return utimensat(dirfd, name, ts, AT_SYMLINK_NOFOLLOW);
#else
	(void)dirfd; (void)name; (void)full; (void)ts;
	errno = ENOSYS;
	return -1;
#endif
}

static inline int il_futimens(int fd, const struct timespec ts[2])
{
#if IL_HAVE_FUTIMENS
	return futimens(fd, ts);
#else
	(void)fd; (void)ts;
	errno = ENOSYS;
	return -1;
#endif
}

/* Flushes the file system that fd lives on (all of them without syncfs). */
static inline int il_syncfs(int fd)
{
#if IL_HAVE_SYNCFS
	return syncfs(fd);
#else
	(void)fd;
	sync();
	return 0;
#endif
}

#endif /* IL_COMPAT_H */
