/*
 * mounttest -- the mount table and the directory-relative name calls
 *
 * Mounts filesystems at a scratch directory and checks what the kernel does
 * with them: files created, read, renamed and removed on a mounted
 * filesystem; rename and link across a mount point (EXDEV); ".." out of a
 * mount's root; getcwd inside a mount; statfs; unmount refused while a file
 * is open or a working directory is inside (EBUSY) and allowed afterwards;
 * data surviving an unmount and a new mount; read-only mounts and remounts;
 * lazy unmount; mknod (device, FIFO, socket); fchownat/lchown with
 * AT_SYMLINK_NOFOLLOW; linkat of a symlink with and without
 * AT_SYMLINK_FOLLOW; utimensat with nanoseconds, the access time,
 * UTIME_OMIT and AT_SYMLINK_NOFOLLOW; futimens; 32-bit owner ids surviving
 * a remount; /proc/mounts, /proc/cmdline and /dev/kmsg.  Symbolic links
 * across mounts: a link on the parent filesystem into a mount, an absolute
 * link on a mounted filesystem to /etc, ".." out of a mount's root through
 * a link, O_NOFOLLOW, a trailing slash, ELOOP for loops and over-long
 * chains.  A mount point cannot be renamed or removed (EBUSY); renaming a
 * directory above one moves the mount along.  A terminal device node made
 * on a mounted filesystem becomes the controlling terminal when opened.
 * The root's st_dev is the device mountinfo shows.
 *
 * Usage (as root):
 *   mounttest --yes DEVICE        ext4 tests, then vfat tests, then tmpfs:
 *                                 DEVICE (a scratch partition, e.g.
 *                                 /dev/sdb1) is FORMATTED -- twice -- with
 *                                 mke2fs and mkfs.fat; whatever is on it
 *                                 is lost.
 *   mounttest --yes --ext4 DEVICE only the ext4 tests (formats DEVICE)
 *   mounttest --yes --vfat DEVICE only the vfat tests (formats DEVICE)
 *   mounttest --tmpfs             only the tests that need no device
 *   mounttest --yes --exfat DEVICE  exFAT: mkfs.exfat, then the common
 *                                 tests and exFAT's own (Unicode names,
 *                                 names without case, a large file kept
 *                                 across mounts, truncate)
 *   mounttest --yes --ntfs DEVICE   the same for NTFS (mkfs.ntfs)
 *   ... --preformatted            with --exfat or --ntfs: DEVICE already
 *                                 holds that filesystem (made on another
 *                                 system); it is not formatted, but the
 *                                 tests still write to it -- a scratch
 *                                 volume only
 *   mounttest --iso DEVICE        ISO 9660, read-only: DEVICE (a CD/DVD
 *                                 drive with a disc, or a disk holding an
 *                                 image) is mounted read-only, walked and
 *                                 read completely; every write must fail
 *                                 with EROFS.  Nothing is written.
 *
 * Without --yes nothing is formatted and the program stops.  A device that
 * is mounted (it appears in /proc/mounts) is refused.  The exit status is
 * the number of failed checks (capped at 125); every check prints a PASS or
 * FAIL line.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/vfs.h>

#define EXT4_MAGIC 0xEF53UL
#define MSDOS_MAGIC 0x4d44UL
#define TMPFS_MAGIC 0x01021994UL
#define EXFAT_MAGIC 0x2011BAB0UL
#define NTFS_MAGIC 0x5346544EUL
#define ISOFS_MAGIC 0x9660UL

static int g_fail, g_pass;
static char g_base[64]; /* scratch directory: /tmp/mounttest.XXXXXX */
static char g_mnt[96]; /* the mount point inside it */

static void check(int ok, const char *what)
{
	if (ok)
		g_pass++;
	else
		g_fail++;
	printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	fflush(stdout);
}

static void checkf(int ok, const char *fmt, const char *arg)
{
	char line[256];

	snprintf(line, sizeof(line), fmt, arg);
	check(ok, line);
}

static void path_in(char *out, size_t cap, const char *name)
{
	snprintf(out, cap, "%s/%s", g_mnt, name);
}

static int write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t n = strlen(text);

	if (fd < 0)
		return -1;
	if (write(fd, text, n) != (ssize_t)n) {
		close(fd);
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

/* Is `dir' listed as a mount point in /proc/mounts (with `type', if not
 * NULL)? */
static int listed(const char *dir, const char *type)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[1024];
	int found = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char src[512], mp[512], fs[64];
		if (sscanf(line, "%511s %511s %63s", src, mp, fs) != 3)
			continue;
		if (strcmp(mp, dir) == 0 && (!type || strcmp(fs, type) == 0))
			found = 1;
	}
	fclose(f);
	return found;
}

/* The device is not mounted anywhere. */
static int device_mounted(const char *dev)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[1024];
	int found = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char src[512];
		if (sscanf(line, "%511s", src) == 1 && strcmp(src, dev) == 0)
			found = 1;
	}
	fclose(f);
	return found;
}

/* Run a program, quietly; 0 when it exited 0. */
static int run(char *const argv[])
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Do `a' and `b' name the same object? */
static int same_file(const char *a, const char *b)
{
	struct stat sa, sb;

	return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
	       sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* The first `n' bytes of two files are the same (and there are some). */
static int same_start(const char *a, const char *b, size_t n)
{
	char x[128], y[128];
	int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
	ssize_t na = -1, nb = -2;

	if (n > sizeof(x))
		n = sizeof(x);
	if (fa >= 0)
		na = read(fa, x, n);
	if (fb >= 0)
		nb = read(fb, y, n);
	if (fa >= 0)
		close(fa);
	if (fb >= 0)
		close(fb);
	return na > 0 && na == nb && memcmp(x, y, (size_t)na) == 0;
}

/* ---- symbolic links across mounts ------------------------------------------
 * A filesystem is mounted at g_mnt; g_base, the directory it is mounted in,
 * is on the parent filesystem. */

static void test_links(const char *fstype)
{
	char a[160], b[160], c[160], d[160], buf[256];
	struct stat st, st2, mst;

	printf("== symbolic links across the %s mount\n", fstype);
	if (stat(g_mnt, &mst) != 0) {
		check(0, "stat the mount point");
		return;
	}

	/* A link on the parent filesystem into the mount. */
	path_in(a, sizeof(a), "data.txt");
	check(write_file(a, "on the mount\n") == 0, "a file on the mount");
	snprintf(b, sizeof(b), "%s/abs-into", g_base);
	unlink(b);
	check(symlink(a, b) == 0 && read_matches(b, "on the mount\n"),
	      "a link on the parent filesystem into the mount (absolute): "
	      "read through it");
	check(same_file(a, b), "stat through it names the file on the mount");
	check(lstat(b, &st) == 0 && S_ISLNK(st.st_mode) &&
		      stat(g_base, &st2) == 0 && st.st_dev == st2.st_dev,
	      "lstat names the link itself, on the parent filesystem");
	errno = 0;
	check(open(b, O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP,
	      "O_NOFOLLOW on the link: ELOOP");
	snprintf(c, sizeof(c), "%s/rel-into", g_base);
	unlink(c);
	check(symlink("mnt/data.txt", c) == 0 && same_file(a, c),
	      "a relative link into the mount");
	snprintf(c, sizeof(c), "%s/dir-into", g_base);
	unlink(c);
	check(symlink("mnt", c) == 0, "a link to the mount point");
	snprintf(buf, sizeof(buf), "%s/data.txt", c);
	check(read_matches(buf, "on the mount\n"),
	      "a path through it continues on the mount");
	snprintf(buf, sizeof(buf), "%s/", c);
	check(lstat(buf, &st) == 0 && S_ISDIR(st.st_mode) &&
		      st.st_dev == mst.st_dev && st.st_ino == mst.st_ino,
	      "lstat with a trailing slash follows the link: the mount's root");
	check(lstat(c, &st) == 0 && S_ISLNK(st.st_mode),
	      "lstat without one stops at the link");
	snprintf(d, sizeof(d), "%s/dangling", g_base);
	unlink(d);
	path_in(buf, sizeof(buf), "made.txt");
	check(symlink(buf, d) == 0 && write_file(d, "made\n") == 0 &&
		      read_matches(buf, "made\n"),
	      "O_CREAT through a dangling link makes the file on the mount");
	errno = 0;
	check(open(d, O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 && errno == EEXIST,
	      "O_CREAT|O_EXCL on a link: EEXIST");
	unlink(buf);
	unlink(d);

	/* An absolute link on the mounted filesystem: from the root. */
	path_in(c, sizeof(c), "etc");
	check(symlink("/etc", c) == 0, "an absolute link on the mount to /etc");
	path_in(buf, sizeof(buf), "etc/passwd");
	check(same_file(buf, "/etc/passwd") &&
		      same_start(buf, "/etc/passwd", 64),
	      "it leads to the root's /etc, not the mount's");
	unlink(c);

	/* ".." out of the mount's root, through a link. */
	snprintf(d, sizeof(d), "%s/updir", g_base);
	mkdir(d, 0755);
	path_in(c, sizeof(c), "up");
	check(symlink("../updir", c) == 0 && same_file(c, d),
	      "a link to ../updir on the mount's root leaves the mount");
	path_in(buf, sizeof(buf), "sub");
	mkdir(buf, 0755);
	path_in(buf, sizeof(buf), "sub/up2");
	check(symlink("../../updir", buf) == 0 && same_file(buf, d),
	      "the same from a directory below it");
	unlink(buf);
	path_in(buf, sizeof(buf), "sub");
	rmdir(buf);
	unlink(c);
	rmdir(d);

	/* Loops, and how long a chain may be. */
	path_in(c, sizeof(c), "loop1");
	path_in(d, sizeof(d), "loop2");
	symlink("loop2", c);
	symlink("loop1", d);
	errno = 0;
	check(stat(c, &st) < 0 && errno == ELOOP, "a loop: ELOOP");
	unlink(c);
	unlink(d);
	snprintf(c, sizeof(c), "%s/xloop", g_base);
	path_in(d, sizeof(d), "yloop");
	unlink(c);
	symlink(d, c);
	symlink(c, d);
	errno = 0;
	check(stat(c, &st) < 0 && errno == ELOOP,
	      "a loop through two filesystems: ELOOP");
	unlink(c);
	unlink(d);
	{
		/* chainN -> data.txt, chain(i) -> chain(i+1) */
		char from[160], to[32];
		int ok = 1;

		for (int i = 44; i >= 0; i--) {
			snprintf(from, sizeof(from), "%s/chain%d", g_mnt, i);
			if (i == 44)
				snprintf(to, sizeof(to), "data.txt");
			else
				snprintf(to, sizeof(to), "chain%d", i + 1);
			if (symlink(to, from) != 0)
				ok = 0;
		}
		path_in(c, sizeof(c), "chain10"); /* 35 links */
		check(ok && read_matches(c, "on the mount\n"),
		      "a chain of 35 links is followed");
		path_in(c, sizeof(c), "chain0"); /* 45 links */
		errno = 0;
		check(stat(c, &st) < 0 && errno == ELOOP,
		      "a chain of 45 links: ELOOP");
		for (int i = 0; i <= 44; i++) {
			snprintf(from, sizeof(from), "%s/chain%d", g_mnt, i);
			unlink(from);
		}
	}

	snprintf(c, sizeof(c), "%s/rel-into", g_base);
	unlink(c);
	snprintf(c, sizeof(c), "%s/dir-into", g_base);
	unlink(c);
	unlink(b);
	unlink(a);
}

/* A mount point cannot be renamed or removed; renaming a directory above
 * one takes the mount along. */
static void test_rename_mount_points(void)
{
	char anc[128], anc2[128], mp[160], mp2[160], f[192];

	printf("== renames around mount points\n");
	snprintf(anc, sizeof(anc), "%s/anc", g_base);
	snprintf(anc2, sizeof(anc2), "%s/anc2", g_base);
	snprintf(mp, sizeof(mp), "%s/m", anc);
	mkdir(anc, 0755);
	mkdir(mp, 0755);
	if (mount("tmpfs", mp, "tmpfs", 0, "size=64k") != 0) {
		check(0, "mount a tmpfs below a directory");
		rmdir(mp);
		rmdir(anc);
		return;
	}
	snprintf(f, sizeof(f), "%s/f", mp);
	write_file(f, "moved along\n");
	snprintf(mp2, sizeof(mp2), "%s/m2", anc);
	errno = 0;
	check(rename(mp, mp2) < 0 && errno == EBUSY,
	      "rename of a mount point: EBUSY");
	mkdir(mp2, 0755);
	errno = 0;
	check(rename(mp2, mp) < 0 && errno == EBUSY,
	      "rename onto a mount point: EBUSY");
	rmdir(mp2);
	errno = 0;
	check(rmdir(mp) < 0 && errno == EBUSY, "rmdir of a mount point: EBUSY");
	check(rename(anc, anc2) == 0, "rename of the directory above it");
	snprintf(mp2, sizeof(mp2), "%s/m", anc2);
	snprintf(f, sizeof(f), "%s/f", mp2);
	check(listed(mp2, "tmpfs") && !listed(mp, NULL),
	      "/proc/mounts shows the mount at its new place");
	check(read_matches(f, "moved along\n"),
	      "the mounted filesystem is reached by the new name");
	check(umount(mp2) == 0, "umount it there");
	rmdir(mp2);
	rmdir(anc2);
}

/* A terminal device node made on a mounted filesystem opens like the /dev
 * node: in a new session, without O_NOCTTY, it becomes the controlling
 * terminal -- /dev/tty then writes to it. */
static void test_tty_node(void)
{
	char node[160], got[256];
	struct stat st;
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	const char *slave;
	size_t have = 0;

	printf("== a terminal node on a mounted filesystem\n");
	if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0 ||
	    !(slave = ptsname(m)) || stat(slave, &st) != 0) {
		check(0, "a new pseudo-terminal");
		if (m >= 0)
			close(m);
		return;
	}
	path_in(node, sizeof(node), "pty");
	check(mknod(node, S_IFCHR | 0620, st.st_rdev) == 0,
	      "mknod of the pseudo-terminal's slave on the mount");
	pid_t pid = fork();
	if (pid == 0) {
		int fd, t;

		setsid();
		fd = open(node, O_RDWR);
		t = open("/dev/tty", O_WRONLY);
		if (fd < 0 || t < 0 || write(t, "ctty-ok\n", 8) != 8)
			_exit(1);
		_exit(0);
	}
	int status = -1;
	waitpid(pid, &status, 0);
	for (int tries = 0; tries < 20 && have + 1 < sizeof(got); tries++) {
		struct pollfd p = { .fd = m, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, 100) <= 0)
			continue;
		n = read(m, got + have, sizeof(got) - 1 - have);
		if (n <= 0)
			break;
		have += (size_t)n;
		got[have] = '\0';
		if (strstr(got, "ctty-ok"))
			break;
	}
	got[have] = '\0';
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
		      strstr(got, "ctty-ok") != NULL,
	      "opening it made it the controlling terminal (/dev/tty reaches it)");
	pid = fork();
	if (pid == 0) {
		setsid();
		_exit(open(node, O_RDWR | O_NOCTTY) >= 0 ? 0 : 1);
	}
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "it opens with O_NOCTTY too");
	unlink(node);
	close(m);
}

/* The root filesystem's st_dev is the device /proc/self/mountinfo shows. */
static void test_root_dev(void)
{
	FILE *f = fopen("/proc/self/mountinfo", "r");
	char line[1024];
	unsigned maj = 0, min = 0;
	int found = 0;
	struct stat st;

	if (!f) {
		check(0, "open /proc/self/mountinfo");
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		int id, parent;
		char mp[512];

		if (sscanf(line, "%d %d %u:%u %*s %511s", &id, &parent, &maj,
			   &min, mp) == 5 &&
		    strcmp(mp, "/") == 0) {
			found = 1;
			break;
		}
	}
	fclose(f);
	check(found && stat("/", &st) == 0 &&
		      (maj == 0 && min == 0 ?
			       1 :
			       (major(st.st_dev) == maj && minor(st.st_dev) == min)),
	      "stat(\"/\").st_dev is the root's device in mountinfo");
	check(stat("/etc", &st) == 0 &&
		      (maj == 0 && min == 0 ?
			       1 :
			       (major(st.st_dev) == maj && minor(st.st_dev) == min)),
	      "so is every file's on it");
}

/* ---- tests every mounted filesystem gets -------------------------------- */

static void test_common(const char *fstype, unsigned long magic,
			int persistent, const char *dev, const char *opts)
{
	char a[160], b[160], d[160], buf[256];
	struct stat st, st2, pst;
	struct statfs sf;
	struct statvfs sv;

	checkf(listed(g_mnt, fstype), "%s: listed in /proc/mounts", fstype);

	/* statfs / statvfs on the mount */
	check(statfs(g_mnt, &sf) == 0 && (unsigned long)sf.f_type == magic &&
		      sf.f_blocks > 0,
	      "statfs: type and size of the mounted filesystem");
	check(statvfs(g_mnt, &sv) == 0 && !(sv.f_flag & ST_RDONLY),
	      "statvfs: read-write mount");

	/* the mount point is the filesystem's root, on a device of its own */
	check(stat(g_mnt, &st) == 0 && stat(g_base, &pst) == 0 &&
		      st.st_dev != pst.st_dev,
	      "mount point: st_dev differs from the directory it is on");

	/* create, write, read, rename, unlink */
	path_in(a, sizeof(a), "a.txt");
	path_in(b, sizeof(b), "b.txt");
	check(write_file(a, "hello mount\n") == 0 && read_matches(a, "hello mount\n"),
	      "create, write and read back a file");
	check(rename(a, b) == 0 && access(a, F_OK) != 0 && read_matches(b, "hello mount\n"),
	      "rename within the filesystem");
	snprintf(buf, sizeof(buf), "%s/outside.txt", g_base);
	errno = 0;
	check(rename(b, buf) < 0 && errno == EXDEV,
	      "rename across the mount point: EXDEV");
	errno = 0;
	check(link(b, buf) < 0 && errno == EXDEV,
	      "link across the mount point: EXDEV");
	check(unlink(b) == 0 && access(b, F_OK) != 0, "unlink");

	/* directories, getcwd and ".." out of the mount */
	path_in(d, sizeof(d), "dir");
	check(mkdir(d, 0755) == 0, "mkdir");
	check(chdir(d) == 0 && getcwd(buf, sizeof(buf)) && strcmp(buf, d) == 0,
	      "getcwd inside the mount");
	check(chdir("..") == 0 && getcwd(buf, sizeof(buf)) &&
		      strcmp(buf, g_mnt) == 0,
	      "chdir .. to the mount's root");
	check(chdir("..") == 0 && getcwd(buf, sizeof(buf)) &&
		      strcmp(buf, g_base) == 0 && stat(".", &st) == 0 &&
		      st.st_dev == pst.st_dev && st.st_ino == pst.st_ino,
	      "chdir .. out of the mount's root reaches the parent filesystem");
	snprintf(buf, sizeof(buf), "%s/dir/../..", g_mnt);
	check(stat(buf, &st) == 0 && st.st_dev == pst.st_dev &&
		      st.st_ino == pst.st_ino,
	      "a path with .. through the mount point names the parent");
	snprintf(buf, sizeof(buf), "%s/./dir/.", g_mnt);
	check(stat(buf, &st) == 0 && stat(d, &st2) == 0 &&
		      st.st_ino == st2.st_ino && st.st_dev == st2.st_dev,
	      "a path with . components stays in the mount");

	/* directory-relative calls */
	int dfd = open(d, O_RDONLY | O_DIRECTORY);
	check(dfd >= 0, "open the directory");
	if (dfd >= 0) {
		check(mkdirat(dfd, "sub", 0700) == 0 &&
			      fstatat(dfd, "sub", &st, 0) == 0 &&
			      S_ISDIR(st.st_mode),
		      "mkdirat");
		int fd = openat(dfd, "f", O_WRONLY | O_CREAT, 0644);
		if (fd >= 0)
			close(fd);
		check(fd >= 0 && renameat(dfd, "f", dfd, "g") == 0 &&
			      fstatat(dfd, "g", &st, 0) == 0,
		      "openat + renameat");
		errno = 0;
		fd = openat(dfd, "h", O_WRONLY | O_CREAT, 0644);
		if (fd >= 0)
			close(fd);
		check(renameat2(dfd, "g", dfd, "h", RENAME_NOREPLACE) < 0 &&
			      errno == EEXIST,
		      "renameat2 RENAME_NOREPLACE: EEXIST");
		check(unlinkat(dfd, "g", 0) == 0 && unlinkat(dfd, "h", 0) == 0 &&
			      unlinkat(dfd, "sub", AT_REMOVEDIR) == 0,
		      "unlinkat");
		/* FAT and exFAT keep no modes, NTFS keeps them only with
		 * ACLs: there it only has to be accepted */
		check(fchmodat(dfd, ".", 0750, 0) == 0 && stat(d, &st) == 0 &&
			      ((fstype && (strcmp(fstype, "vfat") == 0 ||
					   strcmp(fstype, "exfat") == 0 ||
					   strcmp(fstype, "ntfs") == 0)) ||
			       (st.st_mode & 0777) == 0750),
		      "fchmodat");
		close(dfd);
	}

	/* syncfs */
	int sfd = open(g_mnt, O_RDONLY | O_DIRECTORY);
	check(sfd >= 0 && syncfs(sfd) == 0, "syncfs on the mount");
	if (sfd >= 0)
		close(sfd);

	/* unmount refused while something uses the filesystem */
	path_in(a, sizeof(a), "busy.txt");
	int fd = open(a, O_WRONLY | O_CREAT, 0644);
	errno = 0;
	check(fd >= 0 && umount(g_mnt) < 0 && errno == EBUSY,
	      "umount with a file open: EBUSY");
	if (fd >= 0)
		close(fd);
	check(chdir(d) == 0, "chdir into the mount");
	errno = 0;
	check(umount(g_mnt) < 0 && errno == EBUSY,
	      "umount with a working directory inside: EBUSY");
	check(chdir("/") == 0, "chdir out again");
	check(rmdir(g_mnt) < 0, "rmdir of a mount point fails");

	/* data survives an unmount and a new mount */
	if (persistent) {
		path_in(a, sizeof(a), "keep.txt");
		check(write_file(a, "kept across mounts\n") == 0, "write a file to keep");
		check(umount(g_mnt) == 0 && !listed(g_mnt, NULL),
		      "umount after everything was closed");
		check(mount(dev, g_mnt, fstype, 0, opts) == 0,
		      "mount the filesystem again");
		check(read_matches(a, "kept across mounts\n"),
		      "the file is still there with its contents");

		/* read-only */
		check(mount(dev, g_mnt, fstype, MS_REMOUNT | MS_RDONLY, opts) == 0,
		      "remount read-only");
		errno = 0;
		path_in(b, sizeof(b), "ro.txt");
		check(open(b, O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS,
		      "create on a read-only mount: EROFS");
		check(statvfs(g_mnt, &sv) == 0 && (sv.f_flag & ST_RDONLY),
		      "statvfs reports ST_RDONLY");
		check(read_matches(a, "kept across mounts\n"),
		      "reading still works read-only");
		check(mount(dev, g_mnt, fstype, MS_REMOUNT, opts) == 0,
		      "remount read-write");
		check(write_file(b, "rw again\n") == 0 && unlink(b) == 0,
		      "writing works again");
	}
}

/* ---- ext4 ---------------------------------------------------------------- */

static void test_ext4(const char *dev)
{
	char a[160], b[160], c[160], buf[256];
	struct stat st, st2;
	char *mk[] = { "mke2fs", "-F", "-q", "-t", "ext4", "-I", "256",
		       (char *)dev, NULL };

	printf("== ext4 on %s\n", dev);
	if (run(mk) != 0) {
		check(0, "mke2fs");
		return;
	}
	check(mount(dev, g_mnt, "ext4", 0, NULL) == 0, "mount ext4");
	if (!listed(g_mnt, "ext4"))
		return;
	test_common("ext4", EXT4_MAGIC, 1, dev, NULL);

	/* special files */
	path_in(a, sizeof(a), "null");
	check(mknod(a, S_IFCHR | 0666, makedev(1, 3)) == 0 &&
		      stat(a, &st) == 0 && S_ISCHR(st.st_mode) &&
		      major(st.st_rdev) == 1 && minor(st.st_rdev) == 3,
	      "mknod a character device node (1:3)");
	int fd = open(a, O_WRONLY);
	check(fd >= 0 && write(fd, "x", 1) == 1,
	      "the node opens the device it names (/dev/null)");
	if (fd >= 0)
		close(fd);
	path_in(b, sizeof(b), "blk");
	check(mknod(b, S_IFBLK | 0600, makedev(259, 70000)) == 0 &&
		      stat(b, &st) == 0 && S_ISBLK(st.st_mode) &&
		      major(st.st_rdev) == 259 && minor(st.st_rdev) == 70000,
	      "mknod a block device node with a large minor (new encoding)");
	path_in(c, sizeof(c), "fifo");
	check(mkfifo(c, 0644) == 0 && stat(c, &st) == 0 && S_ISFIFO(st.st_mode),
	      "mkfifo");
	unlink(a);
	unlink(b);
	unlink(c);

	/* symlinks: lchown, linkat, utimensat NOFOLLOW */
	path_in(a, sizeof(a), "target");
	path_in(b, sizeof(b), "link");
	write_file(a, "target\n");
	check(symlink("target", b) == 0, "symlink");
	check(lchown(b, 1234, 5678) == 0 && lstat(b, &st) == 0 &&
		      st.st_uid == 1234 && st.st_gid == 5678 &&
		      stat(a, &st2) == 0 && st2.st_uid == 0,
	      "lchown changes the link, not its target");
	check(fchownat(AT_FDCWD, b, 4321, 8765, AT_SYMLINK_NOFOLLOW) == 0 &&
		      lstat(b, &st) == 0 && st.st_uid == 4321,
	      "fchownat AT_SYMLINK_NOFOLLOW");
	check(fchownat(AT_FDCWD, b, 11, 12, 0) == 0 && stat(a, &st2) == 0 &&
		      st2.st_uid == 11 && st2.st_gid == 12,
	      "fchownat without the flag follows the link");
	path_in(c, sizeof(c), "hardlink-to-link");
	check(linkat(AT_FDCWD, b, AT_FDCWD, c, 0) == 0 && lstat(c, &st) == 0 &&
		      S_ISLNK(st.st_mode) && lstat(b, &st2) == 0 &&
		      st.st_ino == st2.st_ino && st2.st_nlink == 2,
	      "linkat of a symlink links the symlink itself");
	unlink(c);
	check(linkat(AT_FDCWD, b, AT_FDCWD, c, AT_SYMLINK_FOLLOW) == 0 &&
		      lstat(c, &st) == 0 && S_ISREG(st.st_mode) &&
		      stat(a, &st2) == 0 && st.st_ino == st2.st_ino,
	      "linkat AT_SYMLINK_FOLLOW links the target");
	unlink(c);

	struct timespec ts[2];
	ts[0].tv_sec = 1000000000;
	ts[0].tv_nsec = 123456789;
	ts[1].tv_sec = 1100000000;
	ts[1].tv_nsec = 987654321;
	check(utimensat(AT_FDCWD, a, ts, 0) == 0 && stat(a, &st) == 0 &&
		      st.st_atim.tv_sec == 1000000000 &&
		      st.st_atim.tv_nsec == 123456789 &&
		      st.st_mtim.tv_sec == 1100000000 &&
		      st.st_mtim.tv_nsec == 987654321,
	      "utimensat: access and modification time with nanoseconds");
	ts[0].tv_nsec = UTIME_OMIT;
	ts[1].tv_sec = 1200000000;
	ts[1].tv_nsec = 5;
	check(utimensat(AT_FDCWD, a, ts, 0) == 0 && stat(a, &st) == 0 &&
		      st.st_atim.tv_sec == 1000000000 &&
		      st.st_mtim.tv_sec == 1200000000 &&
		      st.st_mtim.tv_nsec == 5,
	      "utimensat: UTIME_OMIT leaves the access time");
	ts[0].tv_sec = 900000000;
	ts[0].tv_nsec = 1;
	ts[1].tv_sec = 900000000;
	ts[1].tv_nsec = 2;
	check(utimensat(AT_FDCWD, b, ts, AT_SYMLINK_NOFOLLOW) == 0 &&
		      lstat(b, &st) == 0 && st.st_mtim.tv_sec == 900000000 &&
		      stat(a, &st2) == 0 && st2.st_mtim.tv_sec == 1200000000,
	      "utimensat AT_SYMLINK_NOFOLLOW sets the link's own times");
	fd = open(a, O_RDWR);
	ts[0].tv_nsec = UTIME_NOW;
	ts[1].tv_sec = 1300000000;
	ts[1].tv_nsec = 42;
	check(fd >= 0 && futimens(fd, ts) == 0 && fstat(fd, &st) == 0 &&
		      st.st_mtim.tv_sec == 1300000000 && st.st_mtim.tv_nsec == 42,
	      "futimens");
	if (fd >= 0)
		close(fd);

	/* an absolute link on the ext4 mount leads from the root */
	path_in(c, sizeof(c), "etc");
	path_in(buf, sizeof(buf), "etc/passwd");
	check(symlink("/etc", c) == 0 && same_file(buf, "/etc/passwd"),
	      "an absolute link on ext4 to /etc leads to the root's /etc");
	unlink(c);

	/* readlinkat */
	int dfd = open(g_mnt, O_RDONLY | O_DIRECTORY);
	ssize_t n = dfd >= 0 ? readlinkat(dfd, "link", buf, sizeof(buf)) : -1;
	check(n == 6 && memcmp(buf, "target", 6) == 0, "readlinkat");
	if (dfd >= 0) {
		check(symlinkat("target", dfd, "link2") == 0 &&
			      readlinkat(dfd, "link2", buf, sizeof(buf)) == 6,
		      "symlinkat");
		unlinkat(dfd, "link2", 0);
		close(dfd);
	}

	/* 32-bit owner ids, also after a remount */
	check(chown(a, 100000, 200000) == 0 && stat(a, &st) == 0 &&
		      st.st_uid == 100000 && st.st_gid == 200000,
	      "chown to 32-bit ids");
	check(umount(g_mnt) == 0 && mount(dev, g_mnt, "ext4", 0, NULL) == 0 &&
		      stat(a, &st) == 0 && st.st_uid == 100000 &&
		      st.st_gid == 200000 && st.st_mtim.tv_sec == 1300000000 &&
		      st.st_mtim.tv_nsec == 42,
	      "32-bit ids and the times are stored on disk");

	/* lazy unmount: the open file keeps working */
	fd = open(a, O_RDONLY);
	check(fd >= 0 && umount2(g_mnt, MNT_DETACH) == 0 && !listed(g_mnt, NULL),
	      "umount2 MNT_DETACH with a file open");
	check(fd >= 0 && read(fd, buf, 7) == 7 && memcmp(buf, "target\n", 7) == 0,
	      "the open file is still readable after the detach");
	if (fd >= 0)
		close(fd);
	/* nosuid,nodev,noexec are kept and shown */
	check(mount(dev, g_mnt, "ext4", MS_NOSUID | MS_NODEV | MS_NOEXEC,
		    NULL) == 0,
	      "mount again (nosuid,nodev,noexec) after the detached one finished");
	path_in(c, sizeof(c), "null2");
	mknod(c, S_IFCHR | 0666, makedev(1, 3));
	errno = 0;
	fd = open(c, O_WRONLY);
	check(fd < 0 && errno == EACCES, "nodev: a device node does not open");
	if (fd >= 0)
		close(fd);
	unlink(c);
	check(umount(g_mnt) == 0, "umount ext4");
}

/* ---- vfat ---------------------------------------------------------------- */

static void test_vfat(const char *dev)
{
	char a[160];
	struct stat st;
	const char *opts = "uid=1000,gid=1001,umask=027,shortname=mixed,utf8";
	char *mk[] = { "mkfs.fat", "-F", "32", (char *)dev, NULL };

	printf("== vfat on %s\n", dev);
	if (run(mk) != 0) {
		check(0, "mkfs.fat");
		return;
	}
	check(mount(dev, g_mnt, "vfat", 0, opts) == 0,
	      "mount vfat with uid=, gid=, umask=, shortname= and an unknown option");
	if (!listed(g_mnt, "vfat"))
		return;
	path_in(a, sizeof(a), "file.txt");
	write_file(a, "fat\n");
	check(stat(a, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1001 &&
		      (st.st_mode & 0777) == 0750,
	      "the options are what stat reports");
	test_common("vfat", MSDOS_MAGIC, 1, dev, opts);
	check(umount(g_mnt) == 0, "umount vfat");
	check(mount(dev, g_mnt, "auto", MS_RDONLY, NULL) == 0 &&
		      listed(g_mnt, "vfat"),
	      "fstype auto recognises vfat");
	umount(g_mnt);
}

/* ---- exFAT and NTFS ------------------------------------------------------ */

/* A file of `size' bytes with a pattern that differs in every 4 KiB. */
static int write_pattern(const char *path, size_t size)
{
	static unsigned char buf[65536];
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t done = 0;

	if (fd < 0)
		return -1;
	while (done < size) {
		size_t n = size - done < sizeof(buf) ? size - done : sizeof(buf);

		for (size_t i = 0; i < n; i++)
			buf[i] = (unsigned char)(((done + i) >> 12) * 31 +
						 (done + i) * 7);
		if (write(fd, buf, n) != (ssize_t)n) {
			close(fd);
			return -1;
		}
		done += n;
	}
	return close(fd);
}

static int pattern_matches(const char *path, size_t size)
{
	static unsigned char buf[65536];
	int fd = open(path, O_RDONLY);
	size_t done = 0;

	if (fd < 0)
		return 0;
	while (done < size) {
		size_t want = size - done < sizeof(buf) ? size - done :
							   sizeof(buf);
		ssize_t n = read(fd, buf, want);

		if (n <= 0) {
			close(fd);
			return 0;
		}
		for (ssize_t i = 0; i < n; i++)
			if (buf[i] != (unsigned char)(((done + (size_t)i) >> 12) *
							      31 +
						      (done + (size_t)i) * 7)) {
				close(fd);
				return 0;
			}
		done += (size_t)n;
	}
	{
		char extra;
		ssize_t n = read(fd, &extra, 1);

		close(fd);
		return n == 0;
	}
}

/* Is `name' listed in directory `dir'? */
static int dir_lists(const char *dir, const char *name)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, name) == 0)
			found = 1;
	closedir(d);
	return found;
}

/* What exFAT and NTFS volumes share beyond the common tests. */
static void test_foreign_rw(const char *fstype, const char *dev,
			    const char *opts, int nocase)
{
	char a[192], b[192];
	struct stat st;
	const size_t big = 3 * 1024 * 1024 + 123;

	printf("== %s: names, a large file, truncate\n", fstype);
	path_in(a, sizeof(a), "Gr\xc3\xb6\xc3\x9f" "e \xe2\x82\xac.txt");
	check(write_file(a, "unicode\n") == 0 && read_matches(a, "unicode\n"),
	      "a file with a Unicode name");
	check(dir_lists(g_mnt, "Gr\xc3\xb6\xc3\x9f" "e \xe2\x82\xac.txt"),
	      "readdir lists the Unicode name as it was written");
	if (nocase) {
		path_in(b, sizeof(b), "GR\xc3\x96\xc3\x9f" "E \xe2\x82\xac.TXT");
		check(read_matches(b, "unicode\n"),
		      "the name is found in other case");
		errno = 0;
		check(open(b, O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 &&
			      errno == EEXIST,
		      "creating it in other case: EEXIST");
	}
	unlink(a);
	{
		char longname[256], full[512];

		memset(longname, 'n', 255);
		longname[255] = '\0';
		snprintf(full, sizeof(full), "%s/%s", g_mnt, longname);
		check(write_file(full, "long\n") == 0 && unlink(full) == 0,
		      "a name of 255 characters");
	}

	path_in(a, sizeof(a), "big.bin");
	check(write_pattern(a, big) == 0 && stat(a, &st) == 0 &&
		      (size_t)st.st_size == big,
	      "write a file of 3 MiB and a bit");
	check(pattern_matches(a, big), "read it back");
	check(umount(g_mnt) == 0 && mount(dev, g_mnt, fstype, 0, opts) == 0,
	      "umount and mount again");
	check(pattern_matches(a, big), "the large file is intact after the "
				       "new mount");
	check(truncate(a, 5000) == 0 && stat(a, &st) == 0 &&
		      st.st_size == 5000 && pattern_matches(a, 5000),
	      "truncate shrinks it, the start stays");
	check(truncate(a, 100000) == 0 && stat(a, &st) == 0 &&
		      st.st_size == 100000,
	      "truncate extends it");
	{
		char buf[4096];
		int fd = open(a, O_RDONLY);
		ssize_t n = -1;
		int zero = 1;

		if (fd >= 0 && lseek(fd, 50000, SEEK_SET) == 50000)
			n = read(fd, buf, sizeof(buf));
		for (ssize_t i = 0; i < n; i++)
			if (buf[i])
				zero = 0;
		if (fd >= 0)
			close(fd);
		check(n == (ssize_t)sizeof(buf) && zero,
		      "the extended part reads as zeros");
	}
	check(unlink(a) == 0, "unlink the large file");

	/* a directory tree, renamed and removed */
	path_in(a, sizeof(a), "tree");
	path_in(b, sizeof(b), "tree2");
	check(mkdir(a, 0755) == 0, "mkdir a directory");
	{
		char f[256];
		int ok = 1;

		for (int i = 0; i < 300; i++) {
			snprintf(f, sizeof(f), "%s/file-%03d", a, i);
			if (write_file(f, "x\n") != 0)
				ok = 0;
		}
		check(ok, "300 files in it");
		check(rename(a, b) == 0, "rename the directory");
		for (int i = 0; i < 300; i++) {
			snprintf(f, sizeof(f), "%s/file-%03d", b, i);
			if (!read_matches(f, "x\n") || unlink(f) != 0)
				ok = 0;
		}
		check(ok, "every file is there under the new name, then gone");
		check(rmdir(b) == 0, "rmdir the empty directory");
	}
	check(umount(g_mnt) == 0, "umount");
}

static void test_exfat(const char *dev, int preformatted)
{
	char a[160];
	struct stat st;
	const char *opts = "uid=1000,gid=1001,fmask=0137,dmask=0027";
	char *mk[] = { "mkfs.exfat", "-L", "MOUNTTEST", (char *)dev, NULL };

	printf("== exfat on %s%s\n", dev,
	       preformatted ? " (formatted elsewhere)" : "");
	if (!preformatted && run(mk) != 0) {
		check(0, "mkfs.exfat (use --preformatted for a volume made "
			 "elsewhere)");
		return;
	}
	check(mount(dev, g_mnt, "exfat", 0, opts) == 0,
	      "mount exfat with uid=, gid=, fmask=, dmask=");
	if (!listed(g_mnt, "exfat"))
		return;
	path_in(a, sizeof(a), "owner.txt");
	write_file(a, "exfat\n");
	check(stat(a, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1001 &&
		      (st.st_mode & 0777) == 0640,
	      "the options are what stat reports");
	unlink(a);
	test_common("exfat", EXFAT_MAGIC, 1, dev, opts);
	test_foreign_rw("exfat", dev, opts, 1);
}

static void test_ntfs(const char *dev, int preformatted)
{
	char a[160];
	struct stat st;
	const char *opts = "uid=1000,gid=1001,fmask=0137,dmask=0027";
	char *mk[] = { "mkfs.ntfs", "-F", "-f", "-q", "-L", "MOUNTTEST",
		       (char *)dev, NULL };

	printf("== ntfs on %s%s\n", dev,
	       preformatted ? " (formatted elsewhere)" : "");
	if (!preformatted && run(mk) != 0) {
		check(0, "mkfs.ntfs (use --preformatted for a volume made "
			 "elsewhere)");
		return;
	}
	check(mount(dev, g_mnt, "ntfs", 0, opts) == 0,
	      "mount ntfs with uid=, gid=, fmask=, dmask=");
	if (!listed(g_mnt, "ntfs"))
		return;
	path_in(a, sizeof(a), "owner.txt");
	write_file(a, "ntfs\n");
	check(stat(a, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1001 &&
		      (st.st_mode & 0777) == 0640,
	      "the options are what stat reports");
	unlink(a);
	test_common("ntfs", NTFS_MAGIC, 1, dev, opts);
	test_foreign_rw("ntfs", dev, opts, 0);
}

/* ---- ISO 9660: read-only -------------------------------------------------- */

#define ISO_WALK_DEPTH 24
#define ISO_WALK_MAX 200000

struct walk {
	long dirs, files, links, entries;
	long long bytes;
	int bad_size, bad_read, bad_link, too_deep;
};

/* Every directory listed, every file read to its end, every link read. */
static void iso_walk(const char *dir, int depth, struct walk *w)
{
	DIR *d;
	struct dirent *e;

	if (depth > ISO_WALK_DEPTH) {
		w->too_deep = 1;
		return;
	}
	d = opendir(dir);
	if (!d) {
		w->bad_read++;
		return;
	}
	w->dirs++;
	while ((e = readdir(d)) && w->entries < ISO_WALK_MAX) {
		char p[1024];
		struct stat st;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		w->entries++;
		if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >=
		    (int)sizeof(p))
			continue;
		if (lstat(p, &st) != 0) {
			w->bad_read++;
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			iso_walk(p, depth + 1, w);
		} else if (S_ISLNK(st.st_mode)) {
			char t[1024];
			ssize_t n = readlink(p, t, sizeof(t));

			w->links++;
			if (n <= 0)
				w->bad_link++;
		} else if (S_ISREG(st.st_mode)) {
			static char buf[65536];
			long long got = 0;
			int fd = open(p, O_RDONLY);
			ssize_t n;

			w->files++;
			if (fd < 0) {
				w->bad_read++;
				continue;
			}
			while ((n = read(fd, buf, sizeof(buf))) > 0)
				got += n;
			if (n < 0)
				w->bad_read++;
			close(fd);
			if (got != (long long)st.st_size)
				w->bad_size++;
			w->bytes += got;
		}
	}
	closedir(d);
}

static void test_iso(const char *dev)
{
	struct statfs sf;
	struct statvfs sv;
	struct walk w;
	char a[192], first[256] = "";
	DIR *d;
	struct dirent *e;

	printf("== iso9660 on %s (read-only; nothing is written)\n", dev);
	check(mount(dev, g_mnt, "iso9660", MS_RDONLY | MS_NOSUID | MS_NODEV,
		    NULL) == 0,
	      "mount iso9660 read-only");
	if (!listed(g_mnt, "iso9660"))
		return;
	check(statfs(g_mnt, &sf) == 0 && (unsigned long)sf.f_type == ISOFS_MAGIC &&
		      sf.f_blocks > 0 && sf.f_bfree == 0,
	      "statfs: type, size, nothing free");
	check(statvfs(g_mnt, &sv) == 0 && (sv.f_flag & ST_RDONLY),
	      "statvfs: read-only");

	memset(&w, 0, sizeof(w));
	iso_walk(g_mnt, 0, &w);
	printf("     %ld directories, %ld files (%lld bytes), %ld links\n",
	       w.dirs, w.files, w.bytes, w.links);
	check(w.dirs >= 1 && !w.too_deep, "walk the whole tree");
	check(w.bad_read == 0, "every directory and file opens and reads");
	check(w.bad_size == 0, "every file reads exactly its size");
	check(w.bad_link == 0, "every symbolic link reads");

	/* writes: all refused */
	path_in(a, sizeof(a), "mounttest-new.txt");
	errno = 0;
	check(open(a, O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS,
	      "create: EROFS");
	errno = 0;
	path_in(a, sizeof(a), "mounttest-dir");
	check(mkdir(a, 0755) < 0 && errno == EROFS, "mkdir: EROFS");
	d = opendir(g_mnt);
	while (d && (e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
			snprintf(first, sizeof(first), "%s", e->d_name);
			break;
		}
	if (d)
		closedir(d);
	if (first[0]) {
		struct stat st;
		char to[192];

		path_in(a, sizeof(a), first);
		path_in(to, sizeof(to), "mounttest-renamed");
		errno = 0;
		check(rename(a, to) < 0 && errno == EROFS, "rename: EROFS");
		if (lstat(a, &st) == 0 && S_ISREG(st.st_mode)) {
			int fd;

			errno = 0;
			fd = open(a, O_WRONLY);
			check(fd < 0 && errno == EROFS,
			      "open an existing file for writing: EROFS");
			if (fd >= 0)
				close(fd);
			errno = 0;
			check(unlink(a) < 0 && errno == EROFS, "unlink: EROFS");
			errno = 0;
			check(truncate(a, 0) < 0 && errno == EROFS,
			      "truncate: EROFS");
		} else if (lstat(a, &st) == 0 && S_ISDIR(st.st_mode)) {
			errno = 0;
			check(rmdir(a) < 0 && errno == EROFS, "rmdir: EROFS");
		}
		errno = 0;
		check(chmod(a, 0777) < 0 && errno == EROFS, "chmod: EROFS");
	}
	errno = 0;
	check(mount(dev, g_mnt, "iso9660", MS_REMOUNT, NULL) < 0,
	      "remount read-write is refused");
	check(statvfs(g_mnt, &sv) == 0 && (sv.f_flag & ST_RDONLY),
	      "still read-only");
	check(umount(g_mnt) == 0 && !listed(g_mnt, NULL), "umount");

	/* asked for read-write: refused, or read-only all the same */
	if (mount(dev, g_mnt, "iso9660", 0, NULL) == 0) {
		check(statvfs(g_mnt, &sv) == 0 && (sv.f_flag & ST_RDONLY),
		      "a read-write mount request gives a read-only mount");
		path_in(a, sizeof(a), "mounttest-new.txt");
		errno = 0;
		check(open(a, O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS,
		      "and creating still fails with EROFS");
		check(umount(g_mnt) == 0, "umount");
	} else {
		check(errno == EROFS || errno == EACCES,
		      "a read-write mount request is refused (EROFS)");
	}
}

/* ---- tmpfs and the rest --------------------------------------------------- */

static void test_tmpfs(void)
{
	char a[160];
	struct stat st;
	struct statfs sf;

	printf("== tmpfs\n");
	check(mount("tmpfs", g_mnt, "tmpfs", 0, "size=1m,mode=0700") == 0,
	      "mount tmpfs size=1m,mode=0700");
	if (!listed(g_mnt, "tmpfs"))
		return;
	check(stat(g_mnt, &st) == 0 && (st.st_mode & 07777) == 0700,
	      "mode= sets the root directory's mode");
	check(statfs(g_mnt, &sf) == 0 && sf.f_type == (long)TMPFS_MAGIC &&
		      sf.f_blocks * sf.f_bsize == 1024 * 1024,
	      "size= sets the size");
	test_common("tmpfs", TMPFS_MAGIC, 0, NULL, NULL);
	path_in(a, sizeof(a), "big");
	int fd = open(a, O_WRONLY | O_CREAT, 0644);
	static char chunk[65536];
	ssize_t w = 0;
	errno = 0;
	for (int i = 0; fd >= 0 && i < 32; i++) {
		w = write(fd, chunk, sizeof(chunk));
		if (w < 0)
			break;
	}
	check(w < 0 && errno == ENOSPC, "writing past size=: ENOSPC");
	if (fd >= 0)
		close(fd);
	unlink(a);
	test_links("tmpfs");
	test_tty_node();
	check(umount(g_mnt) == 0, "umount tmpfs");
	test_rename_mount_points();
}

static void test_misc(void)
{
	char buf[1024];
	int fd;
	ssize_t n;

	printf("== /proc and /dev\n");
	check(listed("/", NULL) && listed("/proc", "proc") &&
		      listed("/dev", "devfs"),
	      "/proc/mounts lists /, /proc and /dev");
	{
		FILE *f = fopen("/proc/mounts", "r");
		char src[512], mp[512];
		check(f && fscanf(f, "%511s %511s", src, mp) == 2 &&
			      strcmp(mp, "/") == 0,
		      "/proc/mounts lists the root first");
		if (f)
			fclose(f);
	}
	fd = open("/proc/cmdline", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
	check(n > 0 && buf[n - 1] == '\n', "/proc/cmdline ends with a newline");
	if (fd >= 0)
		close(fd);
	fd = open("/dev/kmsg", O_WRONLY);
	const char *msg = "mounttest: a line for the kernel log\n";
	check(fd >= 0 && write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg),
	      "/dev/kmsg takes a line (dmesg shows \"[mounttest:<pid>] ...\")");
	if (fd >= 0)
		close(fd);
	errno = 0;
	check(mount("none", g_mnt, "nosuchfs", 0, NULL) < 0 && errno == ENODEV,
	      "mount of an unknown type: ENODEV");
	errno = 0;
	check(umount(g_base) < 0 && errno == EINVAL,
	      "umount of a directory that is no mount point: EINVAL");
}

static void usage(void)
{
	fprintf(stderr,
		"usage: mounttest --yes [--ext4|--vfat] DEVICE   (DEVICE is formatted!)\n"
		"       mounttest --yes --exfat|--ntfs [--preformatted] DEVICE\n"
		"       mounttest --iso DEVICE                   (read-only)\n"
		"       mounttest --tmpfs\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int yes = 0, only_ext4 = 0, only_vfat = 0, only_tmpfs = 0;
	int only_exfat = 0, only_ntfs = 0, only_iso = 0, preformatted = 0;
	const char *dev = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--yes") == 0)
			yes = 1;
		else if (strcmp(argv[i], "--ext4") == 0)
			only_ext4 = 1;
		else if (strcmp(argv[i], "--vfat") == 0)
			only_vfat = 1;
		else if (strcmp(argv[i], "--tmpfs") == 0)
			only_tmpfs = 1;
		else if (strcmp(argv[i], "--exfat") == 0)
			only_exfat = 1;
		else if (strcmp(argv[i], "--ntfs") == 0)
			only_ntfs = 1;
		else if (strcmp(argv[i], "--iso") == 0)
			only_iso = 1;
		else if (strcmp(argv[i], "--preformatted") == 0)
			preformatted = 1;
		else if (argv[i][0] == '-')
			usage();
		else
			dev = argv[i];
	}
	if (geteuid() != 0) {
		fprintf(stderr, "mounttest: must run as root\n");
		return 2;
	}
	if (only_ext4 + only_vfat + only_tmpfs + only_exfat + only_ntfs +
		    only_iso > 1 ||
	    (preformatted && !only_exfat && !only_ntfs))
		usage();
	if (!only_tmpfs) {
		/* --iso writes nothing: no --yes needed */
		if (!dev || (!yes && !only_iso))
			usage();
		struct stat st;
		if (stat(dev, &st) != 0 || !S_ISBLK(st.st_mode)) {
			fprintf(stderr, "mounttest: %s is not a block device\n",
				dev);
			return 2;
		}
		if (device_mounted(dev)) {
			fprintf(stderr, "mounttest: %s is mounted; refusing\n",
				dev);
			return 2;
		}
	}

	snprintf(g_base, sizeof(g_base), "/tmp/mounttest.XXXXXX");
	if (!mkdtemp(g_base)) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(g_mnt, sizeof(g_mnt), "%s/mnt", g_base);
	if (mkdir(g_mnt, 0755) != 0) {
		perror("mkdir");
		return 2;
	}

	test_misc();
	test_root_dev();
	if (only_exfat)
		test_exfat(dev, preformatted);
	else if (only_ntfs)
		test_ntfs(dev, preformatted);
	else if (only_iso)
		test_iso(dev);
	if (!only_exfat && !only_ntfs && !only_iso) {
		if (!only_tmpfs && !only_vfat)
			test_ext4(dev);
		if (!only_tmpfs && !only_ext4)
			test_vfat(dev);
		if (!only_ext4 && !only_vfat)
			test_tmpfs();
	}

	/* leave nothing mounted behind */
	while (listed(g_mnt, NULL) && umount2(g_mnt, MNT_DETACH) == 0)
		;
	rmdir(g_mnt);
	char outside[96];
	snprintf(outside, sizeof(outside), "%s/outside.txt", g_base);
	unlink(outside);
	rmdir(g_base);

	printf("mounttest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail > 125 ? 125 : g_fail;
}
