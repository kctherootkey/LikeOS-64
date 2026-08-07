#ifndef _UNISTD_H
#define _UNISTD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* POSIX conformance level implemented by this libc (required by POSIX;
 * ported software keys feature selection off it, e.g. termios vs old
 * BSD tty interfaces). */
#define _POSIX_VERSION  200809L
#define _POSIX2_VERSION 200809L

/* POSIX option macros.  These are how a program asks what the system actually
 * supports, and leaving them out is not neutral: code that feature-tests falls
 * back to an older interface.  X11's Xos_r.h is exactly that case — without
 * _POSIX_THREAD_SAFE_FUNCTIONS it selects a four-argument getpwnam_r that no
 * POSIX system has had in decades, and the build fails on the arity.
 *
 * Only options that are genuinely implemented are declared here. */
#define _POSIX_THREAD_SAFE_FUNCTIONS 200809L /* the *_r family */
#define _POSIX_THREADS               200809L /* pthreads (inside libc) */
#define _POSIX_REENTRANT_FUNCTIONS   1
#define _POSIX_MAPPED_FILES          200809L /* mmap/munmap/msync */
#define _POSIX_SHARED_MEMORY_OBJECTS 200809L /* shm_open/shm_unlink */
#define _POSIX_MEMORY_PROTECTION     200809L /* mprotect */
#define _POSIX_JOB_CONTROL           1
#define _POSIX_SAVED_IDS             1
#define _POSIX_TIMERS                200809L
#define _POSIX_MONOTONIC_CLOCK       200809L
#define _POSIX_READER_WRITER_LOCKS   200809L
#define _POSIX_SPIN_LOCKS            200809L
#define _POSIX_BARRIERS              200809L
#define _POSIX_SEMAPHORES            (-1) /* sem_open/sem_init: not implemented */
#define _POSIX_MESSAGE_PASSING       (-1) /* mq_*: not implemented */
#define _POSIX_PRIORITY_SCHEDULING   (-1) /* sched_setscheduler: not implemented */

// File operations
int open(const char* pathname, int flags, ...);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);
int getpagesize(void);
int getdtablesize(void);

// Access and directories
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char* path, int mode);
int faccessat(int dirfd, const char* path, int mode, int flags);
int chdir(const char* path);
int fchdir(int fd);
int chroot(const char* path);
char* getcwd(char* buf, size_t size);

// User/group IDs
int getuid(void);
int geteuid(void);
int getgid(void);
int getegid(void);
int getgroups(int size, int* list);
int setuid(int uid);
int seteuid(int uid);
int setgid(int gid);
int setegid(int gid);
int setgroups(int size, const int* list);
int setreuid(int ruid, int euid);
int setregid(int rgid, int egid);
int setresuid(int ruid, int euid, int suid);
int setresgid(int rgid, int egid, int sgid);
/* Pointers to uid_t and gid_t, not to int.  Every other system declares them
 * that way, and software that has to prototype them itself -- GLib does,
 * because they are missing from some systems' headers -- writes the
 * conventional signature and then collides with one that does not match. */
int getresuid(uid_t* ruid, uid_t* euid, uid_t* suid);
int getresgid(gid_t* rgid, gid_t* egid, gid_t* sgid);
int initgroups(const char* user, gid_t group);

// Process groups / terminal
int setpgid(int pid, int pgid);
int getpgrp(void);
/* POSIX spells this with no arguments and defines it as setpgid(0, 0).  The
 * two-argument BSD form of the same name is not provided: the two cannot
 * coexist, and this is the one POSIX standardised. */
pid_t setpgrp(void);
int tcgetpgrp(int fd);
int tcsetpgrp(int fd, int pgrp);
int kill(int pid, int sig);

// Sessions
pid_t setsid(void);
pid_t getsid(pid_t pid);
pid_t getpgid(pid_t pid);

/* Process environment vector. Defined in libc; may be NULL. */
extern char **environ;

// Misc
unsigned int alarm(unsigned int seconds);
unsigned int sleep(unsigned int seconds);
/* Implemented in libc but not previously declared here, so every caller got an
 * implicit declaration — which -Werror=implicit turns into a build failure. */
int usleep(unsigned int usec);
int pause(void);
pid_t vfork(void);
int gethostname(char* name, size_t len);
int sethostname(const char *name, size_t len);
char* getlogin(void);
int getlogin_r(char* buf, size_t bufsize);
int setlogin(const char* name);
int fsync(int fd);
void sync(void);
int ftruncate(int fd, off_t length);
int truncate(const char* path, off_t length);
int fcntl(int fd, int cmd, ...);
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t len);
int unlink(const char* path);
int unlinkat(int dirfd, const char* pathname, int flags);
int rename(const char* oldpath, const char* newpath);
int mkdir(const char* path, unsigned int mode);
int rmdir(const char* path);
int link(const char* oldpath, const char* newpath);
int symlink(const char* target, const char* linkpath);
int readlink(const char* path, char* buf, size_t bufsiz);
int chown(const char* path, int owner, int group);
int fchown(int fd, int owner, int group);

// Timestamp operations
#include <time.h>
int utimensat(int dirfd, const char* pathname, const struct timespec times[2], int flags);

// getdents wrappers
struct dirent;
int getdents(int fd, struct dirent* dirp, unsigned int count);
int getdents64(int fd, void* dirp, unsigned int count);

// PTY helpers
int posix_openpt(int flags);
int grantpt(int fd);
int unlockpt(int fd);
char* ptsname(int fd);
int   ptsname_r(int fd, char* buf, size_t buflen);

/* getopt(): POSIX declares it here as well as in <getopt.h>, which is where
 * the GNU getopt_long extension lives.  Code that includes only <unistd.h> —
 * the portable spelling — must still see it. */
int getopt(int argc, char * const argv[], const char *optstring);
extern char *optarg;
extern int optind, opterr, optopt;

// Scheduling
int sched_yield(void);

// Root-only: dump kernel diagnostic tables (TCP/AF_UNIX/PTY/tasks) to the tty.
int debug_dump(void);

// Process operations
pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
int execve(const char* pathname, char* const argv[], char* const envp[]);
int execv(const char* pathname, char* const argv[]);
int execvp(const char* file, char* const argv[]);
int execl(const char *pathname, const char *arg, ... /*, (char *)NULL */);
int execlp(const char *file, const char *arg, ... /*, (char *)NULL */);
void _exit(int status) __attribute__((noreturn));
/* Ends THIS thread only; _exit() above ends the whole process (SYS_EXIT_GROUP).
 * Used by pthread_exit() on a joinable thread -- not an application interface. */
void __thread_exit(int status) __attribute__((noreturn));

// Timestamps
int futimens(int fd, const struct timespec times[2]);

// File descriptor operations
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);

// Memory
void* sbrk(intptr_t increment);
int brk(void* addr);

// System information
struct sysinfo;
int sysinfo(struct sysinfo *info);

// Kernel log
int klogctl(int type, char *bufp, int len);

// pathconf / fpathconf constants
#define PIPE_BUF        4096
#define _PC_PIPE_BUF    4
#define _PC_PATH_MAX    5
#define _PC_NAME_MAX    6
#define _PC_LINK_MAX    7

long fpathconf(int fd, int name);
long pathconf(const char *path, int name);
long sysconf(int name);

// sysconf constants
#define _SC_PAGESIZE      30
#define _SC_PAGE_SIZE     _SC_PAGESIZE
#define _SC_OPEN_MAX      4
#define _SC_CLK_TCK       2
/* Processor counts.  Values are the reference numbering, like the entries
 * above, so a program that hardcodes them (some configure scripts do) agrees
 * with us. */
#define _SC_NPROCESSORS_CONF  83
#define _SC_NPROCESSORS_ONLN  84

// confstr
#define _CS_PATH  0

// Entropy
/* flags for getrandom() */
#define GRND_NONBLOCK   0x0001u
#define GRND_RANDOM     0x0002u
#define GRND_INSECURE   0x0004u

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);

// ELF auxiliary vector
unsigned long getauxval(unsigned long type);

// Generic syscall entry point
long syscall(long number, ...);

#ifdef __cplusplus
}
#endif

#endif
