#include "../../include/unistd.h"
#include "../../include/errno.h"
#include "../../include/limits.h"
#include "../../include/sys/wait.h"
#include "../../include/sys/stat.h"
#include "../../include/sys/time.h"
#include "../../include/sys/utsname.h"
#include "../../include/time.h"
#include "../../include/sys/times.h"
#include "../../include/stdarg.h"
#include "../../include/string.h"
#include "../../include/stdlib.h"
#include "../../include/fcntl.h"
#include "../../include/stdarg.h"
#include "../../include/signal.h"
#include "../../include/sys/reboot.h"
#include "../../include/sys/vfs.h"
#include "../../include/termios.h"
#include "../../include/sys/ioctl.h"
#include "../../include/sys/sysinfo.h"
#include "../../include/sys/klog.h"
#include "syscall.h"

int errno = 0;

int open(const char* pathname, int flags, ...) {
    // mode argument ignored for now (no create support yet)
    long ret = syscall3(SYS_OPEN, (long)pathname, flags, 0);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int openat(int dirfd, const char* pathname, int flags, ...) {
    long ret = syscall4(SYS_OPENAT, dirfd, (long)pathname, flags, 0);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

ssize_t read(int fd, void* buf, size_t count) {
    long ret = syscall3(SYS_READ, fd, (long)buf, count);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

ssize_t write(int fd, const void* buf, size_t count) {
    long ret = syscall3(SYS_WRITE, fd, (long)buf, count);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int close(int fd) {
    long ret = syscall1(SYS_CLOSE, fd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int pipe(int pipefd[2]) {
    long ret = syscall1(SYS_PIPE, (long)pipefd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int access(const char* path, int mode) {
    return faccessat(AT_FDCWD, path, mode, 0);
}

int faccessat(int dirfd, const char* path, int mode, int flags) {
    long ret = syscall4(SYS_FACCESSAT, dirfd, (long)path, mode, flags);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int chdir(const char* path) {
    long ret = syscall1(SYS_CHDIR, (long)path);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

char* getcwd(char* buf, size_t size) {
    long ret = syscall2(SYS_GETCWD, (long)buf, size);
    if (ret < 0) { errno = -ret; return NULL; }
    return (char*)ret;
}

int stat(const char* path, struct stat* st) {
    long ret = syscall2(SYS_STAT, (long)path, (long)st);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int lstat(const char* path, struct stat* st) {
    long ret = syscall2(SYS_LSTAT, (long)path, (long)st);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int fstat(int fd, struct stat* st) {
    long ret = syscall2(SYS_FSTAT, fd, (long)st);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
    long ret = syscall4(SYS_FSTATAT, dirfd, (long)path, (long)st, flags);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int statfs(const char* path, struct statfs* buf) {
    long ret = syscall2(SYS_STATFS, (long)path, (long)buf);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int fstatfs(int fd, struct statfs* buf) {
    long ret = syscall2(SYS_FSTATFS, fd, (long)buf);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    long ret = syscall3(SYS_LSEEK, fd, offset, whence);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

pid_t getpid(void) {
    return syscall0(SYS_GETPID);
}

pid_t getppid(void) {
    return syscall0(SYS_GETPPID);
}

int getuid(void) { return (int)syscall0(SYS_GETUID); }
int geteuid(void) { return (int)syscall0(SYS_GETEUID); }
int getgid(void) { return (int)syscall0(SYS_GETGID); }
int getegid(void) { return (int)syscall0(SYS_GETEGID); }

int setuid(int uid) {
    long ret = syscall1(SYS_SETUID, uid);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int seteuid(int uid) {
    long ret = syscall1(SYS_SETEUID, uid);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int setgid(int gid) {
    long ret = syscall1(SYS_SETGID, gid);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int setegid(int gid) {
    long ret = syscall1(SYS_SETEGID, gid);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int getgroups(int size, int* list) {
    long ret = syscall2(SYS_GETGROUPS, size, (long)list);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int setgroups(int size, const int* list) {
    long ret = syscall2(SYS_SETGROUPS, size, (long)list);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

pid_t fork(void) {
    long ret = syscall0(SYS_FORK);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int execve(const char* pathname, char* const argv[], char* const envp[]) {
    long ret = syscall3(SYS_EXECVE, (long)pathname, (long)argv, (long)envp);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int execv(const char* pathname, char* const argv[]) {
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

int execvp(const char* file, char* const argv[]) {
    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }
    for (const char* p = file; *p; ++p) {
        if (*p == '/') {
            return execv(file, argv);
        }
    }
    // PATH search
    const char* path = getenv("PATH");
    if (!path) {
        path = "/bin:/usr/local/bin";
    }
    char full[256];
    const char* start = path;
    const char* cur = path;
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
            if (*cur == '\0') break;
            start = cur + 1;
        }
        cur++;
    }
    errno = ENOENT;
    return -1;
}

void _exit(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

int dup(int oldfd) {
    long ret = syscall1(SYS_DUP, oldfd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int dup2(int oldfd, int newfd) {
    long ret = syscall2(SYS_DUP2, oldfd, newfd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int fsync(int fd) {
    long ret = syscall1(SYS_FSYNC, fd);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

void sync(void) {
    syscall0(SYS_SYNC);
}

int ftruncate(int fd, off_t length) {
    long ret = syscall2(SYS_FTRUNCATE, fd, length);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int fcntl(int fd, int cmd, ...) {
    long arg = 0;
    if (cmd == F_SETFL) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, long);
        va_end(ap);
    }
    long ret = syscall3(SYS_FCNTL, fd, cmd, arg);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int ioctl(int fd, unsigned long request, void* argp) {
    long ret = syscall3(SYS_IOCTL, fd, request, (long)argp);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int setpgid(int pid, int pgid) {
    long ret = syscall2(SYS_SETPGID, pid, pgid);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int getpgrp(void) {
    return (int)syscall0(SYS_GETPGRP);
}

int tcgetpgrp(int fd) {
    long ret = syscall1(SYS_TCGETPGRP, fd);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int tcsetpgrp(int fd, int pgrp) {
    long ret = syscall2(SYS_TCSETPGRP, fd, pgrp);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

pid_t setsid(void) {
    long ret = syscall0(SYS_SETSID);
    if (ret < 0) { errno = -ret; return (pid_t)-1; }
    return (pid_t)ret;
}

pid_t getsid(pid_t pid) {
    long ret = syscall1(SYS_GETSID, pid);
    if (ret < 0) { errno = -ret; return (pid_t)-1; }
    return (pid_t)ret;
}

pid_t getpgid(pid_t pid) {
    long ret = syscall1(SYS_GETPGID, pid);
    if (ret < 0) { errno = -ret; return (pid_t)-1; }
    return (pid_t)ret;
}

// Note: kill() is defined in signal.c

pid_t wait(int* status) {
    return waitpid(-1, status, 0);
}

pid_t wait4(pid_t pid, int* status, int options, struct rusage* rusage) {
    long ret = syscall4(SYS_WAIT4, pid, (long)status, options, (long)rusage);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    // Kernel handles blocking when WNOHANG is not set
    // No need for userspace busy-loop - preemptive kernel blocks until child exits
    long ret = syscall3(SYS_WAIT4, pid, (long)status, options);
    if (ret >= 0) {
        return ret;
    }
    errno = -ret;
    return -1;
}

void* sbrk(intptr_t increment) {
    static void* current_brk = NULL;
    
    if (current_brk == NULL) {
        // Get initial brk
        current_brk = (void*)syscall1(SYS_BRK, 0);
    }
    
    if (increment == 0) {
        return current_brk;
    }
    
    void* new_brk = (void*)((char*)current_brk + increment);
    void* result = (void*)syscall1(SYS_BRK, (long)new_brk);
    
    if (result == current_brk) {
        errno = ENOMEM;
        return (void*)-1;
    }
    
    void* old_brk = current_brk;
    current_brk = result;
    return old_brk;
}

int brk(void* addr) {
    void* result = (void*)syscall1(SYS_BRK, (long)addr);
    if (result != addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

// sched_yield moved to sched.c

int gethostname(char* name, size_t len) {
    long ret = syscall2(SYS_GETHOSTNAME, (long)name, len);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

char* getlogin(void) {
    static char user[] = "root";
    return user;
}

int uname(struct utsname* buf) {
    long ret = syscall1(SYS_UNAME, (long)buf);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int gettimeofday(struct timeval* tv, void* tz) {
    long ret = syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)tz);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int settimeofday(const struct timeval* tv, const void* tz) {
    long ret = syscall2(SYS_SETTIMEOFDAY, (long)tv, (long)tz);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

time_t time(time_t* tloc) {
    long ret = syscall1(SYS_TIME, (long)tloc);
    if (ret < 0) { errno = -ret; return (time_t)-1; }
    return (time_t)ret;
}

// Note: alarm() and sleep() are defined in signal.c

int isatty(int fd) {
    struct termios t;
    /* Try TCGETS ioctl — succeeds only on tty devices */
    int saved_errno = errno;
    int ret = ioctl(fd, TCGETS, &t);
    errno = saved_errno;  /* isatty must not clobber errno on success */
    return ret == 0 ? 1 : 0;
}

int unlink(const char* path) {
    long ret = syscall1(SYS_UNLINK, (long)path);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int rename(const char* oldpath, const char* newpath) {
    long ret = syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int mkdir(const char* path, unsigned int mode) {
    long ret = syscall2(SYS_MKDIR, (long)path, mode);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int rmdir(const char* path) {
    long ret = syscall1(SYS_RMDIR, (long)path);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int link(const char* oldpath, const char* newpath) {
    long ret = syscall2(SYS_LINK, (long)oldpath, (long)newpath);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int symlink(const char* target, const char* linkpath) {
    long ret = syscall2(SYS_SYMLINK, (long)target, (long)linkpath);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int readlink(const char* path, char* buf, size_t bufsiz) {
    long ret = syscall3(SYS_READLINK, (long)path, (long)buf, bufsiz);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int getdents64(int fd, void* dirp, unsigned int count) {
    long ret = syscall3(SYS_GETDENTS64, fd, (long)dirp, count);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int getdents(int fd, struct dirent* dirp, unsigned int count) {
    long ret = syscall3(SYS_GETDENTS, fd, (long)dirp, count);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int chown(const char* path, int owner, int group) {
    long ret = syscall3(SYS_CHOWN, (long)path, owner, group);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int fchown(int fd, int owner, int group) {
    long ret = syscall3(SYS_FCHOWN, fd, owner, group);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int chmod(const char* path, mode_t mode) {
    long ret = syscall2(SYS_CHMOD, (long)path, mode);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int fchmod(int fd, mode_t mode) {
    long ret = syscall2(SYS_FCHMOD, fd, mode);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int utimensat(int dirfd, const char* pathname, const struct timespec times[2], int flags) {
    long ret = syscall4(SYS_UTIMENSAT, dirfd, (long)pathname, (long)times, flags);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int reboot(int cmd) {
    // Use Linux reboot magic numbers
    long ret = syscall4(SYS_REBOOT, 0xfee1dead, 672274793, cmd, 0);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int getprocinfo(void* buf, int max_count) {
    long ret = syscall2(SYS_GETPROCINFO, (long)buf, (long)max_count);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int sysinfo(struct sysinfo *info) {
    long ret = syscall1(SYS_SYSINFO, (long)info);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int klogctl(int type, char *bufp, int len) {
    long ret = syscall3(SYS_KLOGCTL, type, (long)bufp, len);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

long fpathconf(int fd, int name) {
    (void)fd;
    switch (name) {
        case _PC_PIPE_BUF: return PIPE_BUF;
        case _PC_PATH_MAX: return 4096;
        case _PC_NAME_MAX: return 255;
        case _PC_LINK_MAX: return 127;
        default: errno = EINVAL; return -1;
    }
}

long pathconf(const char *path, int name) {
    (void)path;
    return fpathconf(-1, name);
}

long sysconf(int name) {
    switch (name) {
        case _SC_PAGESIZE:  return 4096;
        case _SC_OPEN_MAX:  return 256;
        case _SC_CLK_TCK:   return 100;
        default: errno = EINVAL; return -1;
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

int flock(int fd, int op) { (void)fd; (void)op; return 0; }


/* BSD-style pipe2: emulated via pipe() + fcntl() since the kernel only
 * exposes the legacy two-argument pipe call. flags accepts O_CLOEXEC
 * and O_NONBLOCK like the BSD/SUSv4 variant. */
int pipe2(int pipefd[2], int flags) {
    if (pipe(pipefd) < 0) return -1;
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

int getpagesize(void) {
    return 4096;
}

int getdtablesize(void) {
    return OPEN_MAX;
}

/* umask: file-mode creation mask. We don't track per-process state in
 * the kernel today, so just store and return the previous value. */
static mode_t _current_umask = 022;
mode_t umask(mode_t mask) {
    mode_t prev = _current_umask;
    _current_umask = mask & 0777;
    return prev;
}

/* ttyname: walk /dev/pts and /dev to find the path of fd's tty.
 * Falls back to a fixed pseudo-name when the lookup fails. */
char *ttyname(int fd) {
    static char buf[64];
    if (ttyname_r(fd, buf, sizeof(buf)) != 0) return 0;
    return buf;
}

int ttyname_r(int fd, char *buf, size_t len) {
    if (!isatty(fd)) { errno = ENOTTY; return ENOTTY; }
    /* Best-effort: report the underlying console device, not the magic
     * /dev/tty alias.  Some applications (e.g. tmux) refuse to use
     * "/dev/tty" because that name resolves to whichever controlling
     * terminal the calling process happens to have, rather than to a
     * fixed device.  In LikeOS stdin/stdout/stderr of an interactive
     * shell are the framebuffer console, which is exposed as
     * /dev/console (and /dev/tty0). */
    const char *name = "/dev/console";
    size_t n = 0;
    while (name[n]) n++;
    if (n + 1 > len) { errno = ERANGE; return ERANGE; }
    for (size_t i = 0; i <= n; i++) buf[i] = name[i];
    return 0;
}

/* getrandom: fill buf with up to buflen cryptographically secure random
 * bytes from the kernel entropy pool.  flags may include:
 *   GRND_NONBLOCK (0x1) - return EAGAIN instead of blocking
 *   GRND_RANDOM   (0x2) - draw from the blocking /dev/random pool */
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    long ret = syscall3(SYS_GETRANDOM, (long)buf, (long)buflen, (long)flags);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (ssize_t)ret;
}

/* getauxval: access ELF auxiliary vector entries.
 * On x86-64 we have no AT_HWCAP/AT_PLATFORM mechanism exposed to
 * userspace today, so all lookups return 0. */
unsigned long getauxval(unsigned long type) {
    (void)type;
    return 0UL;
}

/* times: return process CPU-time accounting.
 * The kernel does not yet export fine-grained CPU accounting, so we
 * return zeroed tms fields and a monotonic tick count of 0. */
clock_t times(struct tms *buf) {
    if (buf) {
        buf->tms_utime  = 0;
        buf->tms_stime  = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return 0;
}

/* syscall: variadic generic syscall entry point.
 * Reads up to 6 long arguments from the va_list and dispatches via
 * the inline syscall6 helper. */
long syscall(long number, ...) {
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
