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

// Scheduling
int sched_yield(void);

// Process operations
pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
void _exit(int status) __attribute__((noreturn));

// File descriptor operations
int dup(int oldfd);
int dup2(int oldfd, int newfd);

// Memory
void* sbrk(intptr_t increment);
int brk(void* addr);

#endif
