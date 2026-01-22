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
char* getlogin(void);
int fsync(int fd);
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
void _exit(int status) __attribute__((noreturn));

// File descriptor operations
int dup(int oldfd);
int dup2(int oldfd, int newfd);

// Memory
void* sbrk(intptr_t increment);
int brk(void* addr);

#endif
