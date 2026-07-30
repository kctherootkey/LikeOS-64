#include <unistd.h>
#include <stdio.h>    /* snprintf: fchdir builds a /dev/fd path */
#include <errno.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <sys/times.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/reboot.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <sys/klog.h>
#include <sys/xattr.h>
#include "syscall.h"

int errno = 0;

int open(const char *pathname, int flags, ...)
{
	// mode argument ignored for now (no create support yet)
	long ret = syscall3(SYS_OPEN, (long)pathname, flags, 0);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	long ret = syscall4(SYS_OPENAT, dirfd, (long)pathname, flags, 0);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

ssize_t read(int fd, void *buf, size_t count)
{
	long ret = syscall3(SYS_READ, fd, (long)buf, count);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

ssize_t write(int fd, const void *buf, size_t count)
{
	long ret = syscall3(SYS_WRITE, fd, (long)buf, count);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int close(int fd)
{
	long ret = syscall1(SYS_CLOSE, fd);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int pipe(int pipefd[2])
{
	long ret = syscall1(SYS_PIPE, (long)pipefd);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int access(const char *path, int mode)
{
	return faccessat(AT_FDCWD, path, mode, 0);
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
	long ret = syscall4(SYS_FACCESSAT, dirfd, (long)path, mode, flags);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int chdir(const char *path)
{
	long ret = syscall1(SYS_CHDIR, (long)path);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int chroot(const char *path)
{
	long ret = syscall1(SYS_CHROOT, (long)path);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

char *getcwd(char *buf, size_t size)
{
	/* buf == NULL asks us to allocate (the widely-used extension, and what
	 * a shell calls: bash uses getcwd(0, 0) for every prompt and after
	 * every cd).  Passing the NULL straight to the kernel made it fail
	 * with EFAULT, which surfaced as
	 *   "shell-init: error retrieving current directory: getcwd: cannot
	 *    access parent directories: Bad address".
	 * size == 0 with a buffer means "how big?" and is EINVAL, as specified;
	 * size == 0 with no buffer means "as large as needed". */
	if (!buf) {
		char tmp[PATH_MAX];
		long r = syscall2(SYS_GETCWD, (long)tmp, sizeof(tmp));
		if (r < 0) {
			errno = -r;
			return NULL;
		}
		size_t need = strlen(tmp) + 1;
		if (size != 0 && need > size) {
			errno = ERANGE;
			return NULL;
		}
		char *out = malloc(size != 0 ? size : need);
		if (!out) {
			errno = ENOMEM;
			return NULL;
		}
		memcpy(out, tmp, need);
		return out;
	}
	if (size == 0) {
		errno = EINVAL;
		return NULL;
	}
	long ret = syscall2(SYS_GETCWD, (long)buf, size);
	if (ret < 0) {
		errno = -ret;
		return NULL;
	}
	return (char *)ret;
}

int stat(const char *path, struct stat *st)
{
	long ret = syscall2(SYS_STAT, (long)path, (long)st);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int lstat(const char *path, struct stat *st)
{
	long ret = syscall2(SYS_LSTAT, (long)path, (long)st);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int fstat(int fd, struct stat *st)
{
	long ret = syscall2(SYS_FSTAT, fd, (long)st);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
	long ret = syscall4(SYS_FSTATAT, dirfd, (long)path, (long)st, flags);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int statfs(const char *path, struct statfs *buf)
{
	long ret = syscall2(SYS_STATFS, (long)path, (long)buf);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int fstatfs(int fd, struct statfs *buf)
{
	long ret = syscall2(SYS_FSTATFS, fd, (long)buf);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

/* statvfs/fstatvfs: the POSIX filesystem-statistics interface.  The kernel
 * only exposes the BSD statfs layout, so translate it into the statvfs one
 * that callers (df, sftp-server) expect.  f_frsize falls back to the block
 * size, and f_favail mirrors f_ffree (no root reservation on inodes). */
static void statfs_to_statvfs(const struct statfs *s, struct statvfs *v)
{
	v->f_bsize = s->f_bsize;
	v->f_frsize = s->f_frsize ? s->f_frsize : s->f_bsize;
	v->f_blocks = s->f_blocks;
	v->f_bfree = s->f_bfree;
	v->f_bavail = s->f_bavail;
	v->f_files = s->f_files;
	v->f_ffree = s->f_ffree;
	v->f_favail = s->f_ffree;
	v->f_fsid = 0;
	v->f_flag = s->f_flags;
	v->f_namemax = s->f_namelen;
}

int statvfs(const char *path, struct statvfs *buf)
{
	struct statfs sf;
	if (statfs(path, &sf) != 0)
		return -1;
	statfs_to_statvfs(&sf, buf);
	return 0;
}

int fstatvfs(int fd, struct statvfs *buf)
{
	struct statfs sf;
	if (fstatfs(fd, &sf) != 0)
		return -1;
	statfs_to_statvfs(&sf, buf);
	return 0;
}

off_t lseek(int fd, off_t offset, int whence)
{
	long ret = syscall3(SYS_LSEEK, fd, offset, whence);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

pid_t getpid(void)
{
	return syscall0(SYS_GETPID);
}

pid_t getppid(void)
{
	return syscall0(SYS_GETPPID);
}

int getuid(void)
{
	return (int)syscall0(SYS_GETUID);
}
int geteuid(void)
{
	return (int)syscall0(SYS_GETEUID);
}
int getgid(void)
{
	return (int)syscall0(SYS_GETGID);
}
int getegid(void)
{
	return (int)syscall0(SYS_GETEGID);
}

int setuid(int uid)
{
	long ret = syscall1(SYS_SETUID, uid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int seteuid(int uid)
{
	long ret = syscall1(SYS_SETEUID, uid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int setgid(int gid)
{
	long ret = syscall1(SYS_SETGID, gid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int setegid(int gid)
{
	long ret = syscall1(SYS_SETEGID, gid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int setresuid(int ruid, int euid, int suid)
{
	long ret = syscall3(SYS_SETRESUID, ruid, euid, suid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int setresgid(int rgid, int egid, int sgid)
{
	long ret = syscall3(SYS_SETRESGID, rgid, egid, sgid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int getresuid(int *ruid, int *euid, int *suid)
{
	long ret = syscall3(SYS_GETRESUID, (long)ruid, (long)euid, (long)suid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int getresgid(int *rgid, int *egid, int *sgid)
{
	long ret = syscall3(SYS_GETRESGID, (long)rgid, (long)egid, (long)sgid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int getgroups(int size, int *list)
{
	long ret = syscall2(SYS_GETGROUPS, size, (long)list);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int setgroups(int size, const int *list)
{
	long ret = syscall2(SYS_SETGROUPS, size, (long)list);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

/* Allocator fork hooks (src/malloc/malloc.c).  The allocator's locks must be
 * held across fork so the child never inherits a lock left locked by another
 * thread mid-allocation. */
extern void __malloc_fork_prepare(void);
extern void __malloc_fork_parent(void);
extern void __malloc_fork_child(void);
/* Pthread fork hooks (src/pthread/pthread.c) — same protocol for the thread
 * list / TSD / zombie-stack locks, plus child-side reinitialisation to a
 * single-threaded state.  Without these, a child forked while another thread
 * held __thread_list_lock (e.g. inside pthread_exit's detached cleanup)
 * inherited the lock in the held state and spun forever in its own
 * pthread_exit. */
extern void __pthread_fork_prepare(void);
extern void __pthread_fork_parent(void);
extern void __pthread_fork_child(void);

pid_t fork(void)
{
	/* Lock order: pthread outside, malloc inside (pthread code allocates;
	 * the allocator never calls into pthread).  Released in reverse. */
	__pthread_fork_prepare();
	__malloc_fork_prepare();
	long ret = syscall0(SYS_FORK);
	if (ret == 0) {
		__malloc_fork_child();
		__pthread_fork_child();
	} else {
		__malloc_fork_parent();
		__pthread_fork_parent();
	}
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int execve(const char *pathname, char *const argv[], char *const envp[])
{
	long ret = syscall3(SYS_EXECVE, (long)pathname, (long)argv, (long)envp);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int execv(const char *pathname, char *const argv[])
{
	/*
     * Build envp from the libc static environment storage so that
     * child processes inherit the current environment.
     */
	int n = env_count();
	if (n <= 0)
		return execve(pathname, argv, NULL);

	/* Static buffers are fine: execve replaces the process on success */
	static char bufs[MAX_ENV_VARS][MAX_ENV_SIZE * 2 + 2];
	char *envp[MAX_ENV_VARS + 1];

	int cookie = 0;
	const char *name, *value;
	int i = 0;
	while (env_iter(&cookie, &name, &value) && i < MAX_ENV_VARS) {
		size_t nlen = strlen(name);
		size_t vlen = strlen(value);
		if (nlen + 1 + vlen + 1 > sizeof(bufs[i])) {
			/* skip oversized entry */
			continue;
		}
		memcpy(bufs[i], name, nlen);
		bufs[i][nlen] = '=';
		memcpy(bufs[i] + nlen + 1, value, vlen + 1);
		envp[i] = bufs[i];
		i++;
	}
	envp[i] = NULL;
	return execve(pathname, argv, envp);
}

int execvp(const char *file, char *const argv[])
{
	if (!file || !*file) {
		errno = ENOENT;
		return -1;
	}
	for (const char *p = file; *p; ++p) {
		if (*p == '/') {
			return execv(file, argv);
		}
	}
	// PATH search
	const char *path = getenv("PATH");
	if (!path) {
		path = "/bin:/usr/local/bin";
	}
	char full[256];
	const char *start = path;
	const char *cur = path;
	while (1) {
		if (*cur == ':' || *cur == '\0') {
			size_t len = (size_t)(cur - start);
			if (len + 1 + strlen(file) + 1 < sizeof(full)) {
				memcpy(full, start, len);
				if (len > 0 && full[len - 1] == '/') {
					strcpy(full + len, file);
				} else {
					full[len] = '/';
					strcpy(full + len + 1, file);
				}
				execv(full, argv);
			}
			if (*cur == '\0')
				break;
			start = cur + 1;
		}
		cur++;
	}
	errno = ENOENT;
	return -1;
}

void _exit(int status)
{
	syscall1(SYS_EXIT, status);
	__builtin_unreachable();
}

int dup(int oldfd)
{
	long ret = syscall1(SYS_DUP, oldfd);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int dup2(int oldfd, int newfd)
{
	long ret = syscall2(SYS_DUP2, oldfd, newfd);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

int fsync(int fd)
{
	long ret = syscall1(SYS_FSYNC, fd);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

void sync(void)
{
	syscall0(SYS_SYNC);
}

int ftruncate(int fd, off_t length)
{
	long ret = syscall2(SYS_FTRUNCATE, fd, length);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

/* truncate(): there is no path-based truncate syscall, so open the file for
 * writing and use ftruncate().  The permission and existence errors surface
 * from open() exactly as a direct syscall would report them. */
/* creat(): the original way to make a file, kept because POSIX requires it and
 * plenty of code still uses it.  It is exactly open() with the three flags
 * that spell "create it, empty, for writing". */
/* remove(): the C-standard way to delete a name, without the caller having to
 * know whether it is a file or a directory.  unlink() refuses directories, so
 * fall back to rmdir() for those. */
int remove(const char *pathname)
{
	if (!pathname) {
		errno = EINVAL;
		return -1;
	}
	if (unlink(pathname) == 0)
		return 0;
	if (errno == EISDIR || errno == EPERM)
		return rmdir(pathname);
	return -1;
}

int creat(const char *pathname, mode_t mode)
{
	return open(pathname, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int truncate(const char *path, off_t length)
{
	int fd, rc, saved;

	if (!path) {
		errno = EINVAL;
		return -1;
	}
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	rc = ftruncate(fd, length);
	saved = errno;
	close(fd);
	if (rc < 0)
		errno = saved;
	return rc;
}

/* getpriority/setpriority: the scheduler has no nice level, so every process
 * runs at the default priority 0.  Report that honestly and accept a request
 * to change it without pretending to have applied one, rather than failing —
 * callers that lower their own priority treat an error as fatal. */
int getpriority(int which, id_t who)
{
	if (which != PRIO_PROCESS && which != PRIO_PGRP &&
	    which != PRIO_USER) {
		errno = EINVAL;
		return -1;
	}
	(void)who;
	errno = 0;
	return 0;
}

int setpriority(int which, id_t who, int prio)
{
	if (which != PRIO_PROCESS && which != PRIO_PGRP &&
	    which != PRIO_USER) {
		errno = EINVAL;
		return -1;
	}
	if (prio < -20 || prio > 19) {
		errno = EINVAL;
		return -1;
	}
	(void)who;
	return 0;
}

int fcntl(int fd, int cmd, ...)
{
	long arg = 0;
	if (cmd == F_SETFL || cmd == F_SETFD || cmd == F_DUPFD ||
	    cmd == F_DUPFD_CLOEXEC) {
		va_list ap;
		va_start(ap, cmd);
		arg = va_arg(ap, long);
		va_end(ap);
	}
	long ret = syscall3(SYS_FCNTL, fd, cmd, arg);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int ioctl(int fd, unsigned long request, void *argp)
{
	long ret = syscall3(SYS_IOCTL, fd, request, (long)argp);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int setpgid(int pid, int pgid)
{
	long ret = syscall2(SYS_SETPGID, pid, pgid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int getpgrp(void)
{
	return (int)syscall0(SYS_GETPGRP);
}

int tcgetpgrp(int fd)
{
	long ret = syscall1(SYS_TCGETPGRP, fd);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int tcsetpgrp(int fd, int pgrp)
{
	long ret = syscall2(SYS_TCSETPGRP, fd, pgrp);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

pid_t setsid(void)
{
	long ret = syscall0(SYS_SETSID);
	if (ret < 0) {
		errno = -ret;
		return (pid_t)-1;
	}
	return (pid_t)ret;
}

pid_t getsid(pid_t pid)
{
	long ret = syscall1(SYS_GETSID, pid);
	if (ret < 0) {
		errno = -ret;
		return (pid_t)-1;
	}
	return (pid_t)ret;
}

pid_t getpgid(pid_t pid)
{
	long ret = syscall1(SYS_GETPGID, pid);
	if (ret < 0) {
		errno = -ret;
		return (pid_t)-1;
	}
	return (pid_t)ret;
}

// Note: kill() is defined in signal.c

pid_t wait(int *status)
{
	return waitpid(-1, status, 0);
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage)
{
	long ret =
		syscall4(SYS_WAIT4, pid, (long)status, options, (long)rusage);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return ret;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
	// Kernel handles blocking when WNOHANG is not set
	// No need for userspace busy-loop - preemptive kernel blocks until child exits
	// The 4th argument MUST be passed explicitly: SYS_WAIT4 treats it as a
	// `struct rusage *` and writes through it when non-NULL.
	long ret = syscall4(SYS_WAIT4, pid, (long)status, options, 0);
	if (ret >= 0) {
		return ret;
	}
	errno = -ret;
	return -1;
}

void *sbrk(intptr_t increment)
{
	static void *current_brk = NULL;

	if (current_brk == NULL) {
		// Get initial brk
		current_brk = (void *)syscall1(SYS_BRK, 0);
	}

	if (increment == 0) {
		return current_brk;
	}

	void *new_brk = (void *)((char *)current_brk + increment);
	void *result = (void *)syscall1(SYS_BRK, (long)new_brk);

	if (result == current_brk) {
		errno = ENOMEM;
		return (void *)-1;
	}

	void *old_brk = current_brk;
	current_brk = result;
	return old_brk;
}

int brk(void *addr)
{
	void *result = (void *)syscall1(SYS_BRK, (long)addr);
	if (result != addr) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

// sched_yield moved to sched.c

int gethostname(char *name, size_t len)
{
	long ret = syscall2(SYS_GETHOSTNAME, (long)name, len);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

/* getlogin()/getlogin_r()/setlogin() live in src/pwd/creds.c so they can share
 * the stored login-name state with setlogin(). */

int uname(struct utsname *buf)
{
	long ret = syscall1(SYS_UNAME, (long)buf);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
	long ret = syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)tz);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int settimeofday(const struct timeval *tv, const void *tz)
{
	long ret = syscall2(SYS_SETTIMEOFDAY, (long)tv, (long)tz);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

time_t time(time_t *tloc)
{
	long ret = syscall1(SYS_TIME, (long)tloc);
	if (ret < 0) {
		errno = -ret;
		return (time_t)-1;
	}
	return (time_t)ret;
}

// Note: alarm() and sleep() are defined in signal.c

int isatty(int fd)
{
	struct termios t;
	/* Try TCGETS ioctl — succeeds only on tty devices */
	int saved_errno = errno;
	int ret = ioctl(fd, TCGETS, &t);
	errno = saved_errno; /* isatty must not clobber errno on success */
	return ret == 0 ? 1 : 0;
}

int unlink(const char *path)
{
	long ret = syscall1(SYS_UNLINK, (long)path);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int rename(const char *oldpath, const char *newpath)
{
	long ret = syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int mkdir(const char *path, unsigned int mode)
{
	long ret = syscall2(SYS_MKDIR, (long)path, mode);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int rmdir(const char *path)
{
	long ret = syscall1(SYS_RMDIR, (long)path);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

/* The kernel has no special-file inodes (FIFOs, device nodes) on disk yet,
 * so mknod/mkfifo report ENOSYS honestly rather than pretending.  Programs
 * with a fallback (e.g. process substitution via /dev/fd) take it. */
int mknod(const char *path, mode_t mode, dev_t dev)
{
	(void)path;
	(void)mode;
	(void)dev;
	errno = ENOSYS;
	return -1;
}

int mkfifo(const char *path, mode_t mode)
{
	return mknod(path, (mode & 07777) | S_IFIFO, 0);
}

int link(const char *oldpath, const char *newpath)
{
	long ret = syscall2(SYS_LINK, (long)oldpath, (long)newpath);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int symlink(const char *target, const char *linkpath)
{
	long ret = syscall2(SYS_SYMLINK, (long)target, (long)linkpath);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int readlink(const char *path, char *buf, size_t bufsiz)
{
	long ret = syscall3(SYS_READLINK, (long)path, (long)buf, bufsiz);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int getdents64(int fd, void *dirp, unsigned int count)
{
	long ret = syscall3(SYS_GETDENTS64, fd, (long)dirp, count);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int getdents(int fd, struct dirent *dirp, unsigned int count)
{
	long ret = syscall3(SYS_GETDENTS, fd, (long)dirp, count);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int chown(const char *path, int owner, int group)
{
	long ret = syscall3(SYS_CHOWN, (long)path, owner, group);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int fchown(int fd, int owner, int group)
{
	long ret = syscall3(SYS_FCHOWN, fd, owner, group);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int chmod(const char *path, mode_t mode)
{
	long ret = syscall2(SYS_CHMOD, (long)path, mode);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int fchmod(int fd, mode_t mode)
{
	long ret = syscall2(SYS_FCHMOD, fd, mode);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int utimensat(int dirfd, const char *pathname, const struct timespec times[2],
	      int flags)
{
	long ret = syscall4(SYS_UTIMENSAT, dirfd, (long)pathname, (long)times,
			    flags);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

#include <utime.h>

int utime(const char *path, const struct utimbuf *times)
{
	struct timespec ts[2];
	if (!times) {
		ts[0].tv_sec = 0;
		ts[0].tv_nsec = UTIME_NOW;
		ts[1].tv_sec = 0;
		ts[1].tv_nsec = UTIME_NOW;
	} else {
		ts[0].tv_sec = times->actime;
		ts[0].tv_nsec = 0;
		ts[1].tv_sec = times->modtime;
		ts[1].tv_nsec = 0;
	}
	return utimensat(AT_FDCWD, path, ts, 0);
}

int utimes(const char *path, const struct timeval tv[2])
{
	struct timespec ts[2];
	if (!tv) {
		ts[0].tv_sec = 0;
		ts[0].tv_nsec = UTIME_NOW;
		ts[1].tv_sec = 0;
		ts[1].tv_nsec = UTIME_NOW;
	} else {
		ts[0].tv_sec = tv[0].tv_sec;
		ts[0].tv_nsec = (long)tv[0].tv_usec * 1000;
		ts[1].tv_sec = tv[1].tv_sec;
		ts[1].tv_nsec = (long)tv[1].tv_usec * 1000;
	}
	return utimensat(AT_FDCWD, path, ts, 0);
}

int reboot(int cmd)
{
	// Use Linux reboot magic numbers
	long ret = syscall4(SYS_REBOOT, 0xfee1dead, 672274793, cmd, 0);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int getprocinfo(void *buf, int max_count)
{
	long ret = syscall2(SYS_GETPROCINFO, (long)buf, (long)max_count);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

int sysinfo(struct sysinfo *info)
{
	long ret = syscall1(SYS_SYSINFO, (long)info);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int klogctl(int type, char *bufp, int len)
{
	long ret = syscall3(SYS_KLOGCTL, type, (long)bufp, len);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

long fpathconf(int fd, int name)
{
	(void)fd;
	switch (name) {
	case _PC_PIPE_BUF:
		return PIPE_BUF;
	case _PC_PATH_MAX:
		return 4096;
	case _PC_NAME_MAX:
		return 255;
	case _PC_LINK_MAX:
		return 127;
	default:
		errno = EINVAL;
		return -1;
	}
}

long pathconf(const char *path, int name)
{
	(void)path;
	return fpathconf(-1, name);
}

long sysconf(int name)
{
	switch (name) {
	case _SC_PAGESIZE:
		return 4096;
	case _SC_OPEN_MAX:
		return 256;
	case _SC_CLK_TCK:
		return 100;
	default:
		errno = EINVAL;
		return -1;
	}
}

#include <stdarg.h>

int execl(const char *pathname, const char *arg, ...)
{
	/* Count args */
	va_list ap;
	int argc = 1;
	va_start(ap, arg);
	while (va_arg(ap, const char *) != NULL)
		argc++;
	va_end(ap);

	/* Build argv array */
	char *argv[argc + 1];
	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (int i = 1; i <= argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);

	return execv(pathname, argv);
}

int execlp(const char *file, const char *arg, ...)
{
	/* Count args */
	va_list ap;
	int argc = 1;
	va_start(ap, arg);
	while (va_arg(ap, const char *) != NULL)
		argc++;
	va_end(ap);

	/* Build argv array */
	char *argv[argc + 1];
	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (int i = 1; i <= argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);

	return execvp(file, argv);
}

int futimens(int fd, const struct timespec times[2])
{
	/* Use utimensat with AT_FDCWD-like approach */
	/* For now, stub - nano uses this for timestamp preservation */
	(void)fd;
	(void)times;
	errno = ENOSYS;
	return -1;
}

int flock(int fd, int op)
{
	(void)fd;
	(void)op;
	return 0;
}

/* BSD-style pipe2: emulated via pipe() + fcntl() since the kernel only
 * exposes the legacy two-argument pipe call. flags accepts O_CLOEXEC
 * and O_NONBLOCK like the BSD/SUSv4 variant. */
int pipe2(int pipefd[2], int flags)
{
	if (pipe(pipefd) < 0)
		return -1;
	if (flags & O_CLOEXEC) {
		fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
		fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
	}
	if (flags & O_NONBLOCK) {
		int fl0 = fcntl(pipefd[0], F_GETFL, 0);
		int fl1 = fcntl(pipefd[1], F_GETFL, 0);
		fcntl(pipefd[0], F_SETFL, fl0 | O_NONBLOCK);
		fcntl(pipefd[1], F_SETFL, fl1 | O_NONBLOCK);
	}
	return 0;
}

int getpagesize(void)
{
	return 4096;
}

int getdtablesize(void)
{
	return OPEN_MAX;
}

/* umask: file-mode creation mask. We don't track per-process state in
 * the kernel today, so just store and return the previous value. */
static mode_t _current_umask = 022;
mode_t umask(mode_t mask)
{
	mode_t prev = _current_umask;
	_current_umask = mask & 0777;
	return prev;
}

/* ttyname: walk /dev/pts and /dev to find the path of fd's tty.
 * Falls back to a fixed pseudo-name when the lookup fails. */
char *ttyname(int fd)
{
	static char buf[64];
	if (ttyname_r(fd, buf, sizeof(buf)) != 0)
		return 0;
	return buf;
}

int ttyname_r(int fd, char *buf, size_t len)
{
	if (!isatty(fd)) {
		errno = ENOTTY;
		return ENOTTY;
	}
	/* Best-effort: report the underlying console device, not the magic
     * /dev/tty alias.  Some applications (e.g. tmux) refuse to use
     * "/dev/tty" because that name resolves to whichever controlling
     * terminal the calling process happens to have, rather than to a
     * fixed device.  In LikeOS stdin/stdout/stderr of an interactive
     * shell are the framebuffer console, which is exposed as
     * /dev/console (and /dev/tty0). */
	const char *name = "/dev/console";
	size_t n = 0;
	while (name[n])
		n++;
	if (n + 1 > len) {
		errno = ERANGE;
		return ERANGE;
	}
	for (size_t i = 0; i <= n; i++)
		buf[i] = name[i];
	return 0;
}

/* getrandom: fill buf with up to buflen cryptographically secure random
 * bytes from the kernel entropy pool.  flags may include:
 *   GRND_NONBLOCK (0x1) - return EAGAIN instead of blocking
 *   GRND_RANDOM   (0x2) - draw from the blocking /dev/random pool */
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
	long ret =
		syscall3(SYS_GETRANDOM, (long)buf, (long)buflen, (long)flags);
	if (ret < 0) {
		errno = (int)(-ret);
		return -1;
	}
	return (ssize_t)ret;
}

/* getauxval: access ELF auxiliary vector entries.
 * On x86-64 we have no AT_HWCAP/AT_PLATFORM mechanism exposed to
 * userspace today, so all lookups return 0. */
unsigned long getauxval(unsigned long type)
{
	(void)type;
	return 0UL;
}

/* setpgrp: put this process into its own process group.
 *
 * Equivalent to setpgid(0, 0), which is how POSIX defines it.  It exists as a
 * separate name for historical reasons: BSD once had a two-argument
 * setpgrp(pid, pgid), System V had this no-argument form, and POSIX settled on
 * the System V one.  Code still reaches for it -- xterm calls it when handing
 * a pty slave to a child -- and it costs one line to provide.
 *
 * Returns the new process-group ID (which is this process's own PID) on
 * success, so callers do not need a second call to find out what it became. */
pid_t setpgrp(void)
{
	if (setpgid(0, 0) != 0)
		return -1;
	return getpgrp();
}

/* fchdir: change the working directory to the one an open descriptor refers to.
 *
 * There is no fchdir syscall, so this goes through /dev/fd/N, which devfs
 * publishes as a symlink to the descriptor's path.  Resolving that symlink and
 * chdir()ing to its target is precisely what fchdir means, and it is the
 * reason /dev/fd exists.
 *
 * The one way this differs from a real fchdir is that it re-resolves by NAME:
 * if the directory has been renamed or replaced since the descriptor was
 * opened, this follows the name to wherever it leads now, whereas a kernel
 * fchdir would follow the descriptor to the original directory.  Programs use
 * fchdir to return to a directory they are holding open, and the name is
 * almost always still correct; the alternative -- not providing it at all --
 * is worse.
 *
 * EBADF for a descriptor that is not open, ENOTDIR for one that is not a
 * directory, both from the chdir() underneath. */
int fchdir(int fd)
{
	char path[32];

	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	snprintf(path, sizeof(path), "/dev/fd/%d", fd);
	return chdir(path);
}

/* times: process CPU-time accounting, in clock ticks.
 *
 * The unit here is sysconf(_SC_CLK_TCK) ticks -- NOT the CLOCKS_PER_SEC that
 * clock() counts in.  The two interfaces measure the same thing in different
 * units, and mixing them up is the classic way to be off by a factor of ten
 * thousand.
 *
 * The child fields stay zero: reaped children's times are not accumulated onto
 * the parent.  wait4() reports each child's own usage as it is reaped, which is
 * where those figures are available.
 *
 * The return value is elapsed real time since an arbitrary point in the past,
 * as POSIX specifies -- callers use differences of it, never its absolute
 * value. */
clock_t times(struct tms *buf)
{
	struct rusage ru;
	long hz = sysconf(_SC_CLK_TCK);
	struct timespec now;

	if (hz <= 0)
		hz = 100;

	if (buf) {
		if (getrusage(RUSAGE_SELF, &ru) != 0)
			return (clock_t)-1;
		buf->tms_utime = (clock_t)(ru.ru_utime.tv_sec * hz +
					   ru.ru_utime.tv_usec * hz / 1000000);
		buf->tms_stime = (clock_t)(ru.ru_stime.tv_sec * hz +
					   ru.ru_stime.tv_usec * hz / 1000000);
		buf->tms_cutime = 0;
		buf->tms_cstime = 0;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return (clock_t)-1;
	return (clock_t)(now.tv_sec * hz + now.tv_nsec / (1000000000L / hz));
}

/* clock: processor time consumed by this process, in CLOCKS_PER_SEC units.
 *
 * Built on CLOCK_PROCESS_CPUTIME_ID, which the kernel serves from the per-task
 * user+system tick counters, so this really is CPU time and not elapsed time.
 * Resolution is one timer tick, so a process that has run for less than a tick
 * reads zero -- that is the sampling rate, not a failure, and callers that need
 * finer granularity should measure a longer interval. */
clock_t clock(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
		return (clock_t)-1;

	/* Scale to CLOCKS_PER_SEC (10^6): seconds up, nanoseconds down.  Doing
	 * it in this order keeps the whole computation in integers without
	 * overflowing until the process has used ~292,000 years of CPU. */
	return (clock_t)(ts.tv_sec * (time_t)CLOCKS_PER_SEC +
			 ts.tv_nsec / 1000);
}

/* syscall: variadic generic syscall entry point.
 * Reads up to 6 long arguments from the va_list and dispatches via
 * the inline syscall6 helper. */
long syscall(long number, ...)
{
	va_list ap;
	long a1, a2, a3, a4, a5, a6;
	va_start(ap, number);
	a1 = va_arg(ap, long);
	a2 = va_arg(ap, long);
	a3 = va_arg(ap, long);
	a4 = va_arg(ap, long);
	a5 = va_arg(ap, long);
	a6 = va_arg(ap, long);
	va_end(ap);
	return syscall6(number, a1, a2, a3, a4, a5, a6);
}

/* ---- extended attributes (sys/xattr.h) ---- */
int setxattr(const char *path, const char *name, const void *value, size_t size,
	     int flags)
{
	long r = syscall5(SYS_SETXATTR, (long)path, (long)name, (long)value,
			  (long)size, (long)flags);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}
int lsetxattr(const char *path, const char *name, const void *value,
	      size_t size, int flags)
{
	long r = syscall5(SYS_SETXATTR, (long)path, (long)name, (long)value,
			  (long)size, (long)(flags | XATTR_SYS_NOFOLLOW));
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}
int fsetxattr(int fd, const char *name, const void *value, size_t size,
	      int flags)
{
	long r = syscall5(SYS_FSETXATTR, fd, (long)name, (long)value,
			  (long)size, (long)flags);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}
ssize_t getxattr(const char *path, const char *name, void *value, size_t size)
{
	long r = syscall5(SYS_GETXATTR, (long)path, (long)name, (long)value,
			  (long)size, 0);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return r;
}
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size)
{
	long r = syscall5(SYS_GETXATTR, (long)path, (long)name, (long)value,
			  (long)size, 1);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return r;
}
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size)
{
	long r = syscall4(SYS_FGETXATTR, fd, (long)name, (long)value,
			  (long)size);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return r;
}
ssize_t listxattr(const char *path, char *list, size_t size)
{
	long r = syscall4(SYS_LISTXATTR, (long)path, (long)list, (long)size, 0);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return r;
}
ssize_t llistxattr(const char *path, char *list, size_t size)
{
	long r = syscall4(SYS_LISTXATTR, (long)path, (long)list, (long)size, 1);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return r;
}
ssize_t flistxattr(int fd, char *list, size_t size)
{
	long r = syscall3(SYS_FLISTXATTR, fd, (long)list, (long)size);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return r;
}
int removexattr(const char *path, const char *name)
{
	long r = syscall3(SYS_REMOVEXATTR, (long)path, (long)name, 0);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}
int lremovexattr(const char *path, const char *name)
{
	long r = syscall3(SYS_REMOVEXATTR, (long)path, (long)name, 1);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}
int fremovexattr(int fd, const char *name)
{
	long r = syscall2(SYS_FREMOVEXATTR, fd, (long)name);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}
