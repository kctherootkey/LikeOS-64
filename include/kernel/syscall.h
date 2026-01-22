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

// Extended syscalls
#define SYS_STAT        200
#define SYS_LSTAT       201
#define SYS_FSTAT       202
#define SYS_ACCESS      203
#define SYS_CHDIR       204
#define SYS_GETCWD      205
#define SYS_UMASK       206
#define SYS_GETUID      207
#define SYS_GETGID      208
#define SYS_GETEUID     209
#define SYS_GETEGID     210
#define SYS_GETGROUPS   211
#define SYS_SETGROUPS   212
#define SYS_GETHOSTNAME 213
#define SYS_UNAME       214
#define SYS_TIME        215
#define SYS_GETTIMEOFDAY 216
#define SYS_FSYNC       217
#define SYS_FTRUNCATE   218
#define SYS_FCNTL       219
#define SYS_IOCTL       220
#define SYS_SETPGID     221
#define SYS_GETPGRP     222
#define SYS_TCGETPGRP   223
#define SYS_TCSETPGRP   224
#define SYS_KILL        225
#define SYS_SETUID      227
#define SYS_SETGID      228
#define SYS_SETEUID     229
#define SYS_SETEGID     230
#define SYS_UNLINK      231
#define SYS_RENAME      232
#define SYS_MKDIR       233
#define SYS_RMDIR       234
#define SYS_LINK        235
#define SYS_SYMLINK     236
#define SYS_READLINK    237
#define SYS_CHMOD       238
#define SYS_FCHMOD      239
#define SYS_CHOWN       240
#define SYS_FCHOWN      241
#define SYS_OPENAT      242
#define SYS_FSTATAT     243
#define SYS_FACCESSAT   244
#define SYS_GETDENTS64  245
#define SYS_GETDENTS    246

// Special dirfd value for *at() syscalls
#define AT_FDCWD        -100

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
#define EINTR           4   // Interrupted system call
#define ESRCH           3   // No such process
#define EIO             5   // I/O error
#define ENOMEM          12  // Out of memory
#define EACCES          13  // Permission denied
#define EFAULT          14  // Bad address
#define ENOEXEC         8   // Exec format error
#define ENOENT          2   // No such file or directory
#define EPERM           1   // Operation not permitted
#define ENOTDIR         20  // Not a directory
#define ENOTTY          25  // Not a typewriter
#define EINVAL          22  // Invalid argument
#define EMFILE          24  // Too many open files
#define ESPIPE          29  // Illegal seek
#define ENOSYS          38  // Function not implemented

// Syscall handler prototype
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5);

#endif // _KERNEL_SYSCALL_H_
