// LikeOS-64 System Call Interface
#ifndef _KERNEL_SYSCALL_H_
#define _KERNEL_SYSCALL_H_

#include "types.h"

// Syscall numbers - Linux-compatible where possible
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_LSEEK       8
#define SYS_MMAP        9
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_PIPE        22
#define SYS_YIELD       24   // sched_yield
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_GETPID      39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT        60
#define SYS_WAIT4       61
#define SYS_GETPPID     110

// File descriptor limits
#define MAX_FDS         1024

// Standard file descriptors
#define STDIN_FD        0
#define STDOUT_FD       1
#define STDERR_FD       2

// Open flags
#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_CREAT         0x0040
#define O_TRUNC         0x0200
#define O_APPEND        0x0400

// mmap protection flags
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

// mmap flags
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS

// mmap failure return
#define MAP_FAILED      ((void*)-1)

// Seek whence
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

// Error codes
#define EAGAIN          11  // Resource temporarily unavailable
#define EBADF           9   // Bad file descriptor
#define ECHILD          10  // No child processes
#define ENOMEM          12  // Out of memory
#define EACCES          13  // Permission denied
#define EFAULT          14  // Bad address
#define ENOEXEC         8   // Exec format error
#define EINVAL          22  // Invalid argument
#define EMFILE          24  // Too many open files
#define ESPIPE          29  // Illegal seek
#define ENOSYS          38  // Function not implemented

// Syscall handler prototype
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5);

#endif // _KERNEL_SYSCALL_H_
