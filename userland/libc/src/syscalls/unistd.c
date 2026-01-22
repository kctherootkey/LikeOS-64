#include "../../include/unistd.h"
#include "../../include/errno.h"
#include "../../include/sys/wait.h"
#include "../../include/sys/stat.h"
#include "../../include/sys/time.h"
#include "../../include/sys/utsname.h"
#include "../../include/time.h"
#include "../../include/string.h"
#include "../../include/fcntl.h"
#include "../../include/stdarg.h"
#include "../../include/signal.h"
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
    return execve(pathname, argv, NULL);
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
        return execv(file, argv);
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

int kill(int pid, int sig) {
    if (sig == 0) {
        long ret = syscall2(SYS_KILL, pid, sig);
        if (ret < 0) { errno = -ret; return -1; }
        return 0;
    }
    if (pid == getpid()) {
        return raise(sig);
    }
    long ret = syscall2(SYS_KILL, pid, sig);
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

pid_t wait(int* status) {
    return waitpid(-1, status, 0);
}

pid_t wait4(pid_t pid, int* status, int options, void* rusage) {
    (void)rusage;
    long ret = syscall3(SYS_WAIT4, pid, (long)status, options);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    // If WNOHANG is not set, loop until child exits
    while (1) {
        long ret = syscall3(SYS_WAIT4, pid, (long)status, options);
        if (ret >= 0) {
            return ret;
        }
        // EAGAIN (11) means no child has exited yet - retry
        // ECHILD (10) means no children exist - return error
        if (ret == -11 && !(options & WNOHANG)) {
            // Yield and retry
            sched_yield();
            continue;
        }
        errno = -ret;
        return -1;
    }
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

int sched_yield(void) {
    return syscall0(SYS_YIELD);
}

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

time_t time(time_t* tloc) {
    long ret = syscall1(SYS_TIME, (long)tloc);
    if (ret < 0) { errno = -ret; return (time_t)-1; }
    return (time_t)ret;
}

static int alarm_active = 0;
static struct timeval alarm_deadline;

static void alarm_check(void) {
    if (!alarm_active) {
        return;
    }
    struct timeval now;
    if (gettimeofday(&now, 0) != 0) {
        return;
    }
    if (now.tv_sec > alarm_deadline.tv_sec ||
        (now.tv_sec == alarm_deadline.tv_sec && now.tv_usec >= alarm_deadline.tv_usec)) {
        alarm_active = 0;
        raise(SIGALRM);
    }
}

unsigned int alarm(unsigned int seconds) {
    unsigned int remaining = 0;
    if (alarm_active) {
        struct timeval now;
        if (gettimeofday(&now, 0) == 0) {
            long dsec = alarm_deadline.tv_sec - now.tv_sec;
            long dusec = alarm_deadline.tv_usec - now.tv_usec;
            if (dusec < 0) { dsec -= 1; dusec += 1000000; }
            if (dsec > 0) {
                remaining = (unsigned int)dsec;
                if (dusec > 0) {
                    remaining += 1; // round up
                }
            }
        }
    }

    if (seconds == 0) {
        alarm_active = 0;
        return remaining;
    }

    struct timeval now;
    if (gettimeofday(&now, 0) != 0) {
        errno = EINVAL;
        return remaining;
    }
    alarm_deadline.tv_sec = now.tv_sec + (long)seconds;
    alarm_deadline.tv_usec = now.tv_usec;
    alarm_active = 1;
    return remaining;
}

unsigned int sleep(unsigned int seconds) {
    struct timeval start;
    struct timeval now;
    gettimeofday(&start, 0);
    while (1) {
        alarm_check();
        gettimeofday(&now, 0);
        long elapsed = now.tv_sec - start.tv_sec;
        if (elapsed >= (long)seconds) break;
        sched_yield();
    }
    return 0;
}

int isatty(int fd) {
    return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
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
