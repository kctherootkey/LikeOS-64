#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// File operations
int open(const char* pathname, int flags, ...);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int pipe(int pipefd[2]);

// Access and directories
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char* path, int mode);
int faccessat(int dirfd, const char* path, int mode, int flags);
int chdir(const char* path);
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

// Process groups / terminal
int setpgid(int pid, int pgid);
int getpgrp(void);
int tcgetpgrp(int fd);
int tcsetpgrp(int fd, int pgrp);
int kill(int pid, int sig);

// Misc
unsigned int alarm(unsigned int seconds);
unsigned int sleep(unsigned int seconds);
int gethostname(char* name, size_t len);
int sethostname(const char *name, size_t len);
char* getlogin(void);
int fsync(int fd);
void sync(void);
int ftruncate(int fd, off_t length);
int fcntl(int fd, int cmd, ...);
int isatty(int fd);
int unlink(const char* path);
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

// Scheduling
int sched_yield(void);

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

// confstr
#define _CS_PATH  0

#endif
