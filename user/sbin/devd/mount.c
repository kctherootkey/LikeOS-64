// devd: mounting, unmounting and ejecting for users.
//
// Mount points are /media/<user>/<name>.  /media and /media/<user> belong to
// root and are never writable by anyone else (the user gets r-x on their own
// directory through an access ACL, or the directory is 0755 where an ACL
// cannot be set), so no user can put a symbolic link or anything else on
// the path between devd's checks and the mount: there is no race to win.
// Each component is checked with lstat() before every mount anyway.
//
// Every mount is nosuid and nodev: a stick can carry neither a set-id
// program nor a device node that works.
//
// Copyright (C) 2026 The LikeOS Project

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "devd.h"

/* ---- paths --------------------------------------------------------------------- */

/* /media/<user> names the user by name.  An account without a name, or
 * with one that is not a plain name, gets no directory at all: a number in
 * its place could be another account's name, and two users would then share
 * one directory. */
static int user_dirname(uid_t uid, char *out, size_t cap)
{
	struct passwd *pw = getpwuid(uid);

	if (!pw || !pw->pw_name || !username_ok(pw->pw_name) ||
	    strlen(pw->pw_name) >= cap || pw->pw_uid != uid)
		return -EPERM;
	strlcpy_d(out, pw->pw_name, cap);
	return 0;
}

int mnt_is_devd_path(const char *path, long *uid)
{
	char user[DEVD_USERDIR_MAX];
	const char *p, *slash;
	size_t n;
	struct passwd *pw;

	if (strncmp(path, DEVD_MEDIA "/", sizeof(DEVD_MEDIA)) != 0)
		return 0;
	p = path + sizeof(DEVD_MEDIA);
	slash = strchr(p, '/');
	if (!slash)
		return 0;
	n = (size_t)(slash - p);
	if (n == 0 || n >= sizeof(user))
		return 0;
	memcpy(user, p, n);
	user[n] = '\0';
	if (!username_ok(user))
		return 0;
	p = slash + 1;
	if (!*p || strchr(p, '/') || strcmp(p, ".") == 0 ||
	    strcmp(p, "..") == 0)
		return 0;
	pw = getpwnam(user);
	if (pw) {
		*uid = (long)pw->pw_uid;
		return 1;
	}
	return 0;
}

/* A directory devd may mount below: real, root's, not writable by group or
 * others. */
static int dir_trusted(const char *path)
{
	struct stat st;

	if (lstat(path, &st) < 0)
		return -errno;
	if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022))
		return -EPERM;
	return 0;
}

static int media_root(void)
{
	struct stat st;
	int fd;

	if (mkdir(DEVD_MEDIA, 0755) < 0 && errno != EEXIST)
		return -errno;
	fd = open(DEVD_MEDIA, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
		close(fd);
		log_err(DEVD_MEDIA " is not a directory of root's; not "
			"mounting anything");
		return -EPERM;
	}
	if ((st.st_mode & 07777) != 0755 && fchmod(fd, 0755) < 0)
		log_warn("chmod " DEVD_MEDIA ": %s", strerror(errno));
	close(fd);
	return dir_trusted(DEVD_MEDIA);
}

/* ---- the user's directory and its ACL ------------------------------------------- */

#define ACL_VERSION 1
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

static size_t acl_put(uint8_t *p, uint16_t tag, uint16_t perm, int with_id,
		      uint32_t id)
{
	p[0] = (uint8_t)tag;
	p[1] = (uint8_t)(tag >> 8);
	p[2] = (uint8_t)perm;
	p[3] = (uint8_t)(perm >> 8);
	if (!with_id)
		return 4;
	p[4] = (uint8_t)id;
	p[5] = (uint8_t)(id >> 8);
	p[6] = (uint8_t)(id >> 16);
	p[7] = (uint8_t)(id >> 24);
	return 8;
}

/* user::rwx user:<uid>:r-x group::r-x mask::r-x other::--- in the form the
 * kernel evaluates (version 1; the named entries carry an id). */
static size_t acl_build(uint8_t *buf, uid_t uid)
{
	size_t n = 0;

	buf[0] = ACL_VERSION;
	buf[1] = buf[2] = buf[3] = 0;
	n = 4;
	n += acl_put(buf + n, ACL_USER_OBJ, 7, 0, 0);
	n += acl_put(buf + n, ACL_USER, 5, 1, (uint32_t)uid);
	n += acl_put(buf + n, ACL_GROUP_OBJ, 5, 0, 0);
	n += acl_put(buf + n, ACL_MASK, 5, 0, 0);
	n += acl_put(buf + n, ACL_OTHER, 0, 0, 0);
	return n;
}

static int user_dir(const char *path, uid_t uid)
{
	struct stat st;
	uint8_t acl[32], back[32];
	size_t alen;
	ssize_t blen;
	int fd;

	if (mkdir(path, 0750) < 0 && errno != EEXIST)
		return -errno;
	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
		close(fd);
		log_err("%s is not a directory of root's", path);
		return -EPERM;
	}
	if (st.st_gid != 0 && fchown(fd, 0, 0) < 0) {
		close(fd);
		return -errno;
	}
	if (uid == 0) {
		if ((st.st_mode & 07777) != 0750 && fchmod(fd, 0750) < 0)
			log_warn("chmod %s: %s", path, strerror(errno));
		close(fd);
		return dir_trusted(path);
	}
	if ((st.st_mode & 07777) != 0750 && fchmod(fd, 0750) < 0) {
		close(fd);
		return -errno;
	}
	close(fd);
	/* The ACL is set and read back in this process: nothing is forked, so
	 * no process of the user's ever holds devd's descriptors.  Only a file
	 * system that has no ACLs at all gets the open fallback; any other
	 * failure refuses the mount rather than exposing the user's drives. */
	alen = acl_build(acl, uid);
	if (setxattr(path, "system.posix_acl_access", acl, alen, 0) < 0) {
		int e = errno;

		if (e != ENOTSUP && e != EOPNOTSUPP)
			return -e;
		/* readable by everybody instead, still writable by root
		 * alone */
		log_warn("no ACLs on %s; using mode 0755", path);
		if (chmod(path, 0755) < 0)
			return -errno;
		return dir_trusted(path);
	}
	blen = getxattr(path, "system.posix_acl_access", back, sizeof(back));
	if (blen != (ssize_t)alen || memcmp(back, acl, alen) != 0) {
		log_err("the access ACL on %s did not stick", path);
		return -EIO;
	}
	return dir_trusted(path);
}

/* ---- start-up ----------------------------------------------------------------- */

int media_setup(void)
{
	static struct mnt_ent m[256];
	int nm, r;
	DIR *top;
	struct dirent *u;

	r = media_root();
	if (r < 0)
		return r;
	nm = mnt_read(m, 256);
	if (nm < 0)
		nm = 0;
	top = opendir(DEVD_MEDIA);
	if (!top)
		return 0;
	/* empty directories left by an earlier run: nothing is mounted on
	 * them and nothing else could have put them there */
	while ((u = readdir(top))) {
		char upath[DEVD_PATH_MAX];
		DIR *ud;
		struct dirent *v;

		if (!username_ok(u->d_name))
			continue;
		snprintf(upath, sizeof(upath), DEVD_MEDIA "/%s", u->d_name);
		if (dir_trusted(upath) < 0)
			continue;
		ud = opendir(upath);
		if (!ud)
			continue;
		while ((v = readdir(ud))) {
			char path[DEVD_PATH_MAX];
			struct stat st;
			int mounted = 0;

			if (strcmp(v->d_name, ".") == 0 ||
			    strcmp(v->d_name, "..") == 0)
				continue;
			if (snprintf(path, sizeof(path), "%s/%s", upath,
				     v->d_name) >= (int)sizeof(path))
				continue;
			for (int i = 0; i < nm; i++)
				if (strcmp(m[i].path, path) == 0)
					mounted = 1;
			if (mounted || lstat(path, &st) < 0 ||
			    !S_ISDIR(st.st_mode) || st.st_uid != 0)
				continue;
			if (rmdir(path) == 0)
				log_info("removed stale %s", path);
		}
		closedir(ud);
	}
	closedir(top);
	return 0;
}

/* ---- mount ---------------------------------------------------------------------- */

int dev_node_ok(const struct dev *d)
{
	char path[DEVD_PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "/dev/%s", d->name);
	if (lstat(path, &st) < 0 || !S_ISBLK(st.st_mode) ||
	    major(st.st_rdev) != d->major || minor(st.st_rdev) != d->minor)
		return 0;
	return 1;
}

int do_mount(struct dev *d, uid_t uid, gid_t gid, char *out, size_t cap,
	     const char **msg)
{
	char user[DEVD_USERDIR_MAX], udir[DEVD_PATH_MAX], base[DEVD_LABEL_MAX];
	char cand[DEVD_LABEL_MAX], path[DEVD_PATH_MAX], dev[DEVD_PATH_MAX];
	char opts[128];
	const struct fs_type *t;
	struct passwd *pw;
	struct stat st;
	int r, made = 0;

	if (!dev_is_volume(d)) {
		*msg = "not a volume (it has partitions)";
		return -EINVAL;
	}
	if (d->state != DS_READY) {
		*msg = "the volume is being formatted";
		return -EBUSY;
	}
	if (d->ejected) {
		*msg = "the drive was ejected; unplug it and plug it in again";
		return -ENODEV;
	}
	if (d->devd_mount) {
		if ((long)uid == d->mountuid || uid == 0) {
			strlcpy_d(out, d->mounted, cap);
			return 0;
		}
		*msg = "mounted for another user";
		return -EBUSY;
	}
	if (d->foreign_mount) {
		*msg = "already mounted";
		return -EBUSY;
	}
	t = fs_by_id(d->pr.fstype);
	if (!d->supported || !t || !t->kernel_type) {
		*msg = "unsupported file system";
		return -ENOTSUP;
	}
	if (!dev_node_ok(d)) {
		*msg = "device node missing";
		return -ENODEV;
	}
	pw = getpwuid(uid);
	if (pw)
		gid = pw->pw_gid;

	r = media_root();
	if (r < 0) {
		*msg = "/media is not safe to mount on";
		return r;
	}
	r = user_dirname(uid, user, sizeof(user));
	if (r < 0) {
		*msg = "this account has no plain user name";
		return r;
	}
	snprintf(udir, sizeof(udir), DEVD_MEDIA "/%s", user);
	r = user_dir(udir, uid);
	if (r < 0) {
		*msg = "cannot prepare the user's media directory";
		return r;
	}

	mountname_choose(base, sizeof(base), d->pr.label, d->pr.uuid, d->name);
	for (unsigned i = 0; i < 100 && !made; i++) {
		if (i == 0)
			strlcpy_d(cand, base, sizeof(cand));
		else
			mountname_suffix(cand, sizeof(cand), base, i);
		if (snprintf(path, sizeof(path), "%s/%s", udir, cand) >=
		    (int)sizeof(path)) {
			*msg = "mount path too long";
			return -ENAMETOOLONG;
		}
		if (mkdir(path, 0700) == 0)
			made = 1;
		else if (errno != EEXIST) {
			*msg = "cannot create the mount directory";
			return -errno;
		}
	}
	if (!made) {
		*msg = "no free mount directory name";
		return -EEXIST;
	}
	/* the whole path once more, right before the mount */
	if (dir_trusted(DEVD_MEDIA) < 0 || dir_trusted(udir) < 0 ||
	    lstat(path, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0) {
		rmdir(path);
		*msg = "mount path is not safe";
		return -EPERM;
	}
	t->mount_opts(opts, sizeof(opts), uid, gid);
	snprintf(dev, sizeof(dev), "/dev/%s", d->name);
	if (mount(dev, path, t->kernel_type, MS_NOSUID | MS_NODEV,
		  opts[0] ? opts : NULL) < 0) {
		int e = errno;

		rmdir(path);
		log_warn("mount %s on %s failed: %s", dev, path, strerror(e));
		*msg = "the kernel refused to mount it";
		return -(e ? e : EIO);
	}
	log_info("mounted %s on %s for uid %u", dev, path, (unsigned)uid);
	strlcpy_d(out, path, cap);
	return 0;
}

/* sync, then a normal unmount, then the directory goes */
int unmount_path(const char *path, const char **msg)
{
	long uid;
	int fd;

	if (!mnt_is_devd_path(path, &uid)) {
		*msg = "not a devd mount";
		return -EINVAL;
	}
	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0) {
		if (syncfs(fd) < 0)
			log_warn("syncfs %s: %s", path, strerror(errno));
		close(fd);
	}
	if (umount2(path, 0) < 0) {
		int e = errno;

		*msg = e == EBUSY ? "the volume is in use" : "unmount failed";
		return -(e ? e : EIO);
	}
	if (rmdir(path) < 0)
		log_warn("rmdir %s: %s", path, strerror(errno));
	log_info("unmounted %s", path);
	return 0;
}

int do_unmount(struct dev *d, uid_t uid, const char **msg)
{
	char path[DEVD_PATH_MAX];

	if (!d->devd_mount) {
		*msg = d->mounted[0] ? "not mounted by devd" : "not mounted";
		return -EINVAL;
	}
	if (uid != 0 && (long)uid != d->mountuid) {
		*msg = "mounted for another user";
		return -EPERM;
	}
	strlcpy_d(path, d->mounted, sizeof(path));
	return unmount_path(path, msg);
}

int do_eject(const char *disk, uid_t uid, const char **msg)
{
	char paths[DEVD_MAX_DEVS][DEVD_PATH_MAX];
	char dev[DEVD_PATH_MAX];
	int np = 0, r = 0, fd;

	for (int i = 0; i < g_tab.n; i++) {
		struct dev *d = &g_tab.d[i];

		if (strcmp(d->disk, disk) != 0)
			continue;
		if (d->state != DS_READY) {
			*msg = "the drive is being formatted";
			return -EBUSY;
		}
		if (!d->devd_mount)
			continue;
		if (uid != 0 && (long)uid != d->mountuid) {
			*msg = "a volume is mounted for another user";
			return -EPERM;
		}
		strlcpy_d(paths[np++], d->mounted, DEVD_PATH_MAX);
	}
	for (int i = 0; i < np; i++) {
		r = unmount_path(paths[i], msg);
		if (r < 0)
			return r;
	}
	/* the drive's own write cache */
	snprintf(dev, sizeof(dev), "/dev/%s", disk);
	fd = open(dev, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd >= 0) {
		if (fsync(fd) < 0)
			log_warn("flushing %s: %s", dev, strerror(errno));
		close(fd);
	}
	log_info("%s ejected", disk);
	return 0;
}

void detach_removed(const struct dev *d)
{
	long uid;

	if (!mnt_is_devd_path(d->mounted, &uid))
		return;
	if (umount2(d->mounted, MNT_DETACH) < 0 && errno != EINVAL &&
	    errno != ENOENT)
		log_warn("detaching %s: %s", d->mounted, strerror(errno));
	if (rmdir(d->mounted) < 0 && errno != ENOENT)
		log_warn("rmdir %s: %s", d->mounted, strerror(errno));
	else
		log_info("%s gone: %s detached", d->name, d->mounted);
}
