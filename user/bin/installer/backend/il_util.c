/* libinstaller -- small helpers every module uses.
 *
 * Strings and paths, whole-file reads and atomic replacement, directory
 * trees (copy, chown, remove), random bytes, time, and running the external
 * tools (mkfs.fat, mke2fs, e2fsck, likeos-efiboot) with their output
 * captured into the log.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "il_internal.h"

/* ---- strings ------------------------------------------------------ */

size_t il_strlcpy(char *dst, const char *src, size_t len)
{
	size_t n = strlen(src);

	if (len) {
		size_t c = n < len - 1 ? n : len - 1;

		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return n;
}

size_t il_strlcat(char *dst, const char *src, size_t len)
{
	size_t d = strnlen(dst, len);

	if (d == len)
		return d + strlen(src);
	return d + il_strlcpy(dst + d, src, len - d);
}

char *il_trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
			 e[-1] == '\n'))
		*--e = '\0';
	return s;
}

int il_starts_with(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

int il_ends_with(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);

	return a >= b && strcmp(s + a - b, suffix) == 0;
}

int il_path_join(char *buf, size_t len, const char *a, const char *b)
{
	size_t la = strlen(a);
	int n;

	while (la > 1 && a[la - 1] == '/')
		la--;
	while (*b == '/')
		b++;
	if (la == 0)
		n = snprintf(buf, len, "/%s", b);
	else if (la == 1 && a[0] == '/')
		n = snprintf(buf, len, "/%s", b);
	else if (*b)
		n = snprintf(buf, len, "%.*s/%s", (int)la, a, b);
	else
		n = snprintf(buf, len, "%.*s", (int)la, a);
	if (n < 0 || (size_t)n >= len)
		return -ENAMETOOLONG;
	return 0;
}

int il_path_in(char *buf, size_t len, const char *root, const char *path)
{
	return il_path_join(buf, len, root && root[0] ? root : "/", path);
}

/* ---- files ---------------------------------------------------------- */

int il_mkdir_p(const char *path, mode_t mode)
{
	char tmp[IL_PATH_MAX];
	size_t n;

	if (il_strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp))
		return -ENAMETOOLONG;
	n = strlen(tmp);
	while (n > 1 && tmp[n - 1] == '/')
		tmp[--n] = '\0';
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, mode) != 0 && errno != EEXIST)
			return -errno;
		*p = '/';
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		return -errno;
	if (!il_is_dir(tmp))
		return -ENOTDIR;
	return 0;
}

ssize_t il_read_file(const char *path, char *buf, size_t len)
{
	int fd;
	size_t got = 0;

	if (!len)
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	while (got < len - 1) {
		ssize_t n = read(fd, buf + got, len - 1 - got);

		if (n < 0) {
			int e = errno;

			if (e == EINTR)
				continue;
			close(fd);
			return -e;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	buf[got] = '\0';
	return (ssize_t)got;
}

int il_read_file_alloc(const char *path, char **out, size_t *len_out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t cap = 4096, got = 0;
	char *buf;

	*out = NULL;
	if (len_out)
		*len_out = 0;
	if (fd < 0)
		return -errno;
	buf = malloc(cap);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	for (;;) {
		ssize_t n;

		if (got + 1 >= cap) {
			char *nb;

			if (cap >= (64u << 20)) {
				free(buf);
				close(fd);
				return -EFBIG;
			}
			nb = realloc(buf, cap * 2);
			if (!nb) {
				free(buf);
				close(fd);
				return -ENOMEM;
			}
			buf = nb;
			cap *= 2;
		}
		n = read(fd, buf + got, cap - 1 - got);
		if (n < 0) {
			int e = errno;

			if (e == EINTR)
				continue;
			free(buf);
			close(fd);
			return -e;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	buf[got] = '\0';
	*out = buf;
	if (len_out)
		*len_out = got;
	return 0;
}

int il_read_line(const char *path, char *buf, size_t len)
{
	ssize_t n = il_read_file(path, buf, len);
	char *t;

	if (n < 0) {
		if (len)
			buf[0] = '\0';
		return (int)n;
	}
	buf[strcspn(buf, "\n")] = '\0';
	t = il_trim(buf);
	if (t != buf)
		memmove(buf, t, strlen(t) + 1);
	return 0;
}

int il_read_u64(const char *path, uint64_t *out)
{
	char buf[64];
	char *end;
	int rc = il_read_line(path, buf, sizeof(buf));

	if (rc)
		return rc;
	errno = 0;
	*out = strtoull(buf, &end, 10);
	if (errno || end == buf)
		return -EINVAL;
	return 0;
}

/* File systems without Unix permissions (FAT, on the EFI system partition)
 * may refuse a chmod; that is no reason to fail a write there. */
static int il_mode_unsupported(int e)
{
	return e == EPERM || e == ENOTSUP || e == EOPNOTSUPP || e == ENOSYS;
}

static int il_write_all_fd(int fd, const void *data, size_t len)
{
	const char *p = data;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static void il_fsync_dir_of(const char *path)
{
	char dir[IL_PATH_MAX];
	char *slash;
	int fd;

	il_strlcpy(dir, path, sizeof(dir));
	slash = strrchr(dir, '/');
	if (!slash)
		il_strlcpy(dir, ".", sizeof(dir));
	else if (slash == dir)
		dir[1] = '\0';
	else
		*slash = '\0';
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

int il_write_file_atomic(const char *path, const void *data, size_t len,
			 mode_t mode, uid_t uid, gid_t gid)
{
	char tmp[IL_PATH_MAX];
	int fd, rc;

	if (snprintf(tmp, sizeof(tmp), "%s.il-new", path) >= (int)sizeof(tmp))
		return -ENAMETOOLONG;
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		  mode & 07777);
	if (fd < 0)
		return -errno;
	rc = il_write_all_fd(fd, data, len);
	if (!rc && (uid != (uid_t)-1 || gid != (gid_t)-1)) {
		if (fchown(fd, uid, gid) != 0 && errno != EPERM)
			rc = -errno;
	}
	/* After the owner: a chown clears the set-id bits. */
	if (!rc && fchmod(fd, mode & 07777) != 0 && !il_mode_unsupported(errno))
		rc = -errno;
	if (!rc && fsync(fd) != 0)
		rc = -errno;
	if (close(fd) != 0 && !rc)
		rc = -errno;
	if (!rc && rename(tmp, path) != 0)
		rc = -errno;
	if (rc) {
		unlink(tmp);
		return rc;
	}
	il_fsync_dir_of(path);
	return 0;
}

int il_copy_file(const char *src, const char *dst, int mode)
{
	struct stat st;
	char *buf;
	int in, out, rc = 0;
	const size_t bufsize = 256 * 1024;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -errno;
	if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
		rc = S_ISREG(st.st_mode) ? -errno : -EINVAL;
		close(in);
		return rc;
	}
	if (mode < 0)
		mode = (int)(st.st_mode & 07777);
	unlink(dst);
	out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		   (mode_t)mode);
	if (out < 0) {
		rc = -errno;
		close(in);
		return rc;
	}
	buf = malloc(bufsize);
	if (!buf) {
		close(in);
		close(out);
		unlink(dst);
		return -ENOMEM;
	}
	for (;;) {
		ssize_t n = read(in, buf, bufsize);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (n == 0)
			break;
		rc = il_write_all_fd(out, buf, (size_t)n);
		if (rc)
			break;
	}
	free(buf);
	if (!rc && fchmod(out, (mode_t)mode) != 0 && !il_mode_unsupported(errno))
		rc = -errno;
	if (!rc && fsync(out) != 0)
		rc = -errno;
	if (close(out) != 0 && !rc)
		rc = -errno;
	close(in);
	if (rc)
		unlink(dst);
	return rc;
}

int il_file_exists(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

int il_is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t il_file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;
	return (uint64_t)st.st_size;
}

/* ---- directory trees ------------------------------------------------ */

#define IL_TREE_MAX_DEPTH 64

static void il_set_times_path(const char *path, const struct stat *st)
{
	struct timespec ts[2];

	ts[0] = st->st_atim;
	ts[1] = st->st_mtim;
	il_utimensat_nofollow(AT_FDCWD, path, path, ts);
}

static int il_copy_tree_rec(il_ctx *ctx, const char *src, const char *dst,
			    uid_t uid, gid_t gid, int depth)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	if (depth > IL_TREE_MAX_DEPTH)
		return il_set_error(ctx, ELOOP, "%s: directories nested too "
				    "deeply", src);
	if (lstat(src, &st) != 0)
		return il_set_error(ctx, errno, "%s: %s", src, strerror(errno));
	if (S_ISLNK(st.st_mode)) {
		char target[IL_PATH_MAX];
		ssize_t n = readlink(src, target, sizeof(target) - 1);

		if (n < 0)
			return il_set_error(ctx, errno, "%s: %s", src,
					    strerror(errno));
		target[n] = '\0';
		unlink(dst);
		if (symlink(target, dst) != 0)
			return il_set_error(ctx, errno, "%s: %s", dst,
					    strerror(errno));
		if (il_lchownat(AT_FDCWD, dst, dst,
				uid == (uid_t)-1 ? st.st_uid : uid,
				gid == (gid_t)-1 ? st.st_gid : gid, 1) != 0 &&
		    errno != EPERM && errno != ENOSYS)
			il_log(ctx, IL_LOG_WARN, "%s: owner not set: %s", dst,
			       strerror(errno));
		il_set_times_path(dst, &st);
		return 0;
	}
	if (S_ISREG(st.st_mode)) {
		rc = il_copy_file(src, dst, (int)(st.st_mode & 07777));
		if (rc)
			return il_set_error(ctx, -rc, "%s: %s", dst,
					    strerror(-rc));
		if (chown(dst, uid == (uid_t)-1 ? st.st_uid : uid,
			  gid == (gid_t)-1 ? st.st_gid : gid) != 0 &&
		    errno != EPERM)
			il_log(ctx, IL_LOG_WARN, "%s: owner not set: %s", dst,
			       strerror(errno));
		chmod(dst, st.st_mode & 07777);
		il_set_times_path(dst, &st);
		return 0;
	}
	if (!S_ISDIR(st.st_mode)) {
		il_log(ctx, IL_LOG_WARN, "%s: not a file, directory or link; "
		       "left out", src);
		return 0;
	}
	if (mkdir(dst, 0700) != 0 && errno != EEXIST)
		return il_set_error(ctx, errno, "%s: %s", dst, strerror(errno));
	dir = opendir(src);
	if (!dir)
		return il_set_error(ctx, errno, "%s: %s", src, strerror(errno));
	while ((de = readdir(dir)) != NULL) {
		char s2[IL_PATH_MAX], d2[IL_PATH_MAX];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (il_path_join(s2, sizeof(s2), src, de->d_name) ||
		    il_path_join(d2, sizeof(d2), dst, de->d_name)) {
			rc = il_set_error(ctx, ENAMETOOLONG, "%s/%s: path too "
					  "long", src, de->d_name);
			break;
		}
		rc = il_copy_tree_rec(ctx, s2, d2, uid, gid, depth + 1);
		if (rc)
			break;
	}
	closedir(dir);
	if (rc)
		return rc;
	if (chown(dst, uid == (uid_t)-1 ? st.st_uid : uid,
		  gid == (gid_t)-1 ? st.st_gid : gid) != 0 && errno != EPERM)
		il_log(ctx, IL_LOG_WARN, "%s: owner not set: %s", dst,
		       strerror(errno));
	chmod(dst, st.st_mode & 07777);
	il_set_times_path(dst, &st);
	return 0;
}

int il_copy_tree(il_ctx *ctx, const char *src, const char *dst, uid_t uid,
		 gid_t gid)
{
	return il_copy_tree_rec(ctx, src, dst, uid, gid, 0);
}

static int il_chown_tree_rec(il_ctx *ctx, const char *path, uid_t uid,
			     gid_t gid, int depth)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	if (depth > IL_TREE_MAX_DEPTH)
		return il_set_error(ctx, ELOOP, "%s: nested too deeply", path);
	if (lstat(path, &st) != 0)
		return il_set_error(ctx, errno, "%s: %s", path, strerror(errno));
	if (S_ISLNK(st.st_mode)) {
		if (il_lchownat(AT_FDCWD, path, path, uid, gid, 1) != 0 &&
		    errno != ENOSYS && errno != EPERM)
			il_log(ctx, IL_LOG_WARN, "%s: owner not set: %s", path,
			       strerror(errno));
		return 0;
	}
	if (chown(path, uid, gid) != 0) {
		if (errno != EPERM)
			return il_set_error(ctx, errno, "%s: cannot change the "
					    "owner: %s", path, strerror(errno));
		il_log(ctx, IL_LOG_DEBUG, "%s: owner not set (not permitted)",
		       path);
	}
	/* chown clears set-id bits of files; put the mode back. */
	if (!S_ISDIR(st.st_mode)) {
		if (st.st_mode & (S_ISUID | S_ISGID))
			chmod(path, st.st_mode & 07777);
		return 0;
	}
	dir = opendir(path);
	if (!dir)
		return il_set_error(ctx, errno, "%s: %s", path, strerror(errno));
	while ((de = readdir(dir)) != NULL) {
		char p2[IL_PATH_MAX];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (il_path_join(p2, sizeof(p2), path, de->d_name)) {
			rc = il_set_error(ctx, ENAMETOOLONG, "%s: path too long",
					  path);
			break;
		}
		rc = il_chown_tree_rec(ctx, p2, uid, gid, depth + 1);
		if (rc)
			break;
	}
	closedir(dir);
	return rc;
}

int il_chown_tree(il_ctx *ctx, const char *path, uid_t uid, gid_t gid)
{
	return il_chown_tree_rec(ctx, path, uid, gid, 0);
}

static int il_remove_tree_rec(il_ctx *ctx, const char *path, int depth)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	if (depth > IL_TREE_MAX_DEPTH)
		return il_set_error(ctx, ELOOP, "%s: nested too deeply", path);
	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : il_set_error(ctx, errno, "%s: %s",
							  path, strerror(errno));
	if (!S_ISDIR(st.st_mode)) {
		if (unlink(path) != 0 && errno != ENOENT)
			return il_set_error(ctx, errno, "%s: %s", path,
					    strerror(errno));
		return 0;
	}
	dir = opendir(path);
	if (!dir)
		return il_set_error(ctx, errno, "%s: %s", path, strerror(errno));
	while ((de = readdir(dir)) != NULL) {
		char p2[IL_PATH_MAX];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (il_path_join(p2, sizeof(p2), path, de->d_name)) {
			rc = il_set_error(ctx, ENAMETOOLONG, "%s: path too long",
					  path);
			break;
		}
		rc = il_remove_tree_rec(ctx, p2, depth + 1);
		if (rc)
			break;
	}
	closedir(dir);
	if (rc)
		return rc;
	if (rmdir(path) != 0 && errno != ENOENT)
		return il_set_error(ctx, errno, "%s: %s", path, strerror(errno));
	return 0;
}

int il_remove_tree(il_ctx *ctx, const char *path)
{
	if (!path || !path[0] || strcmp(path, "/") == 0)
		return il_set_error(ctx, EINVAL, "refusing to remove '%s'",
				    path ? path : "");
	return il_remove_tree_rec(ctx, path, 0);
}

/* ---- randomness and time -------------------------------------------- */

int il_random_bytes(void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;

	while (got < n) {
		ssize_t r = getrandom(p + got, n - got, 0);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		got += (size_t)r;
	}
	if (got < n) {
		int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

		if (fd < 0)
			return -errno;
		while (got < n) {
			ssize_t r = read(fd, p + got, n - got);

			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0) {
				close(fd);
				return -EIO;
			}
			got += (size_t)r;
		}
		close(fd);
	}
	return 0;
}

void il_hex(const void *data, size_t n, char *out)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *p = data;

	for (size_t i = 0; i < n; i++) {
		out[2 * i] = hex[p[i] >> 4];
		out[2 * i + 1] = hex[p[i] & 15];
	}
	out[2 * n] = '\0';
}

uint64_t il_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void il_sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

long il_days_since_epoch(void)
{
	time_t now = time(NULL);

	if (now < 0)
		return 0;
	return (long)(now / 86400);
}

/* ---- running tools ---------------------------------------------------- */

int il_spawn(il_ctx *ctx, const char *const argv[], int *status, char *out,
	     size_t outlen)
{
	char cmd[1024];
	char linebuf[2048];
	size_t linelen = 0, outpos = 0;
	int pfd[2], wst = 0, rc = 0;
	pid_t pid;

	if (status)
		*status = -1;
	if (out && outlen)
		out[0] = '\0';
	if (!argv || !argv[0] || argv[0][0] != '/')
		return il_set_error(ctx, EINVAL, "internal: a tool must be run "
				    "by its absolute path");
	cmd[0] = '\0';
	for (int i = 0; argv[i]; i++) {
		if (i)
			il_strlcat(cmd, " ", sizeof(cmd));
		il_strlcat(cmd, argv[i], sizeof(cmd));
	}
	il_log(ctx, IL_LOG_INFO, "running: %s", cmd);
	if (access(argv[0], X_OK) != 0)
		return il_set_error(ctx, errno, "%s: %s", argv[0],
				    strerror(errno));
	/* Close-on-exec: a tool another thread starts meanwhile must not
	 * inherit the write end (the read below would never see the end). */
	if (pipe2(pfd, O_CLOEXEC) != 0)
		return il_set_error(ctx, errno, "pipe: %s", strerror(errno));
	pid = fork();
	if (pid < 0) {
		int e = errno;

		close(pfd[0]);
		close(pfd[1]);
		return il_set_error(ctx, e, "cannot start %s: %s", argv[0],
				    strerror(e));
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY);

		if (devnull >= 0) {
			dup2(devnull, 0);
			if (devnull > 2)
				close(devnull);
		}
		dup2(pfd[1], 1);
		dup2(pfd[1], 2);
		/* dup2 onto itself keeps close-on-exec (a parent without
		 * a standard descriptor got the pipe as 1 or 2). */
		fcntl(1, F_SETFD, 0);
		fcntl(2, F_SETFD, 0);
		if (pfd[1] > 2)
			close(pfd[1]);
		close(pfd[0]);
		/* A plain, predictable environment for the tool. */
		{
			static char *const envp[] = {
				"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
				"LANG=C",
				"LC_ALL=C",
				NULL,
			};

			execve(argv[0], (char *const *)argv, envp);
		}
		_exit(127);
	}
	close(pfd[1]);
	for (;;) {
		char buf[1024];
		ssize_t n = read(pfd[0], buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		for (ssize_t i = 0; i < n; i++) {
			if (out && outlen && outpos + 1 < outlen) {
				out[outpos++] = buf[i];
				out[outpos] = '\0';
			}
			if (buf[i] == '\n' || linelen + 1 >= sizeof(linebuf)) {
				il_log_text(ctx, IL_LOG_DEBUG, "  | ", linebuf,
					    linelen);
				linelen = 0;
				if (buf[i] == '\n')
					continue;
			}
			linebuf[linelen++] = buf[i];
		}
	}
	if (linelen)
		il_log_text(ctx, IL_LOG_DEBUG, "  | ", linebuf, linelen);
	close(pfd[0]);
	while (waitpid(pid, &wst, 0) < 0) {
		if (errno != EINTR) {
			rc = il_set_error(ctx, errno, "waiting for %s: %s",
					  argv[0], strerror(errno));
			break;
		}
	}
	if (rc)
		return rc;
	if (WIFEXITED(wst)) {
		if (status)
			*status = WEXITSTATUS(wst);
		il_log(ctx, WEXITSTATUS(wst) ? IL_LOG_WARN : IL_LOG_DEBUG,
		       "%s exited with status %d", argv[0], WEXITSTATUS(wst));
	} else if (WIFSIGNALED(wst)) {
		if (status)
			*status = 128 + WTERMSIG(wst);
		il_log(ctx, IL_LOG_WARN, "%s was killed by signal %d", argv[0],
		       WTERMSIG(wst));
	}
	return 0;
}

int il_run_tool(il_ctx *ctx, const char *what, const char *const argv[])
{
	int status = -1;
	int rc = il_spawn(ctx, argv, &status, NULL, 0);

	if (rc)
		return rc;
	if (status != 0)
		return il_set_error(ctx, EIO, "%s failed (%s exited with status "
				    "%d); the log %s has its output", what,
				    argv[0], status, ctx->log_path);
	return 0;
}
