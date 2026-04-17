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

// Signal syscalls
#define SYS_RT_SIGACTION    250
#define SYS_RT_SIGPROCMASK  251
#define SYS_RT_SIGPENDING   252
#define SYS_RT_SIGTIMEDWAIT 253
#define SYS_RT_SIGQUEUEINFO 254
#define SYS_RT_SIGSUSPEND   255
#define SYS_RT_SIGRETURN    256
#define SYS_SIGALTSTACK     257
#define SYS_TKILL           258
#define SYS_TGKILL          259
#define SYS_ALARM           260
#define SYS_SETITIMER       261
#define SYS_GETITIMER       262
#define SYS_TIMER_CREATE    263
#define SYS_TIMER_SETTIME   264
#define SYS_TIMER_GETTIME   265
#define SYS_TIMER_GETOVERRUN 266
#define SYS_TIMER_DELETE    267
#define SYS_SIGNALFD        268
#define SYS_SIGNALFD4       269
#define SYS_PAUSE           270
#define SYS_NANOSLEEP       271
#define SYS_CLOCK_GETTIME   272
#define SYS_CLOCK_GETRES    273

// Clock IDs for clock_gettime
#define CLOCK_REALTIME      0
#define CLOCK_MONOTONIC     1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

// SMP/Threading syscalls (using 310+ to avoid conflicts)
#define SYS_CLONE           310
#define SYS_VFORK           311
#define SYS_EXIT_GROUP      312
#define SYS_GETTID          313
#define SYS_SET_TID_ADDRESS 314
#define SYS_FUTEX           315
#define SYS_SET_ROBUST_LIST 316
#define SYS_GET_ROBUST_LIST 317
#define SYS_ARCH_PRCTL      318  // TLS support (ARCH_SET_FS/ARCH_GET_FS)
#define SYS_FUTEX_REQUEUE   319  // Futex requeue operation

// Scheduling syscalls (using 320+ to avoid conflicts)
#define SYS_SCHED_SETAFFINITY       320
#define SYS_SCHED_GETAFFINITY       321
#define SYS_SCHED_SETSCHEDULER      322
#define SYS_SCHED_GETSCHEDULER      323
#define SYS_SCHED_SETPARAM          324
#define SYS_SCHED_GETPARAM          325
#define SYS_SCHED_GET_PRIORITY_MAX  326
#define SYS_SCHED_GET_PRIORITY_MIN  327
#define SYS_SCHED_RR_GET_INTERVAL   328

// Memory protection
#define SYS_MPROTECT        329

// System management
#define SYS_REBOOT          330

// Process information (LikeOS specific)
#define SYS_GETPROCINFO     331

// Filesystem extended syscalls
#define SYS_UTIMENSAT       332
#define SYS_STATFS          333
#define SYS_FSTATFS         334

// System information and kernel log
#define SYS_SYSINFO         335  // Get system info (memory, uptime, loadavg)
#define SYS_KLOGCTL         336  // Kernel ring buffer read/clear
#define SYS_SETTIMEOFDAY    337  // Set system time (and sync to CMOS RTC)
#define SYS_SYNC            338  // Flush all dirty caches to disk

// Socket syscalls (using 340+ to avoid conflicts)
#define SYS_SOCKET          340
#define SYS_BIND            341
#define SYS_LISTEN          342
#define SYS_ACCEPT          343
#define SYS_CONNECT         344
#define SYS_SENDTO          345
#define SYS_RECVFROM        346
#define SYS_SEND            347
#define SYS_RECV            348
#define SYS_SHUTDOWN        349
#define SYS_SETSOCKOPT      350
#define SYS_GETSOCKOPT      351
#define SYS_GETPEERNAME     352
#define SYS_GETSOCKNAME     353
#define SYS_SOCKETPAIR      354
#define SYS_ACCEPT4         355
#define SYS_SENDMSG         356
#define SYS_RECVMSG         357
#define SYS_SENDFILE        358
#define SYS_SELECT          359
#define SYS_PSELECT6        360
#define SYS_POLL            361
#define SYS_PPOLL           362
#define SYS_EPOLL_CREATE    363
#define SYS_EPOLL_CREATE1   364
#define SYS_EPOLL_CTL       365
#define SYS_EPOLL_WAIT      366
#define SYS_EPOLL_PWAIT     367
#define SYS_DUP3            368
#define SYS_DNS_RESOLVE     369
#define SYS_SETHOSTNAME     370
#define SYS_NET_GETINFO     371  // Get network info (ARP table, route table, connections)
#define SYS_DHCP_CONTROL    372  // DHCP client control (discover, release, renew)
#define SYS_RAW_SEND        373  // Send raw IP/ICMP/ARP packet
#define SYS_RAW_RECV        374  // Receive raw IP/ICMP/ARP packet
#define SYS_DNS_RESOLVE_REVERSE 375  // Reverse DNS lookup (PTR)

// NET_GETINFO sub-commands
#define NET_GET_ARP_TABLE       1
#define NET_GET_ROUTE_TABLE     2
#define NET_GET_TCP_CONNECTIONS 3
#define NET_GET_UDP_SOCKETS     4
#define NET_GET_IFACE_STATS     5
#define NET_DNS_QUERY           6

// DHCP_CONTROL sub-commands
#define DHCP_CMD_DISCOVER   1
#define DHCP_CMD_RELEASE    2
#define DHCP_CMD_RENEW      3
#define DHCP_CMD_STATUS     4

// Kernel log control operations (for SYS_KLOGCTL)
#define SYSLOG_ACTION_READ       2
#define SYSLOG_ACTION_READ_ALL   3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR      5
#define SYSLOG_ACTION_SIZE_BUFFER 10

// sysinfo structure returned by SYS_SYSINFO
typedef struct k_sysinfo {
    long     uptime;          // Seconds since boot
    unsigned long loads[3];   // 1, 5, 15 minute load averages (fixed-point << 16)
    unsigned long totalram;   // Total usable main memory in bytes
    unsigned long freeram;    // Available memory in bytes
    unsigned long sharedram;  // Shared memory (kernel heap used)
    unsigned long bufferram;  // Memory used by buffers (0 for now)
    unsigned long totalswap;  // Total swap space (0)
    unsigned long freeswap;   // Free swap space (0)
    unsigned short procs;     // Number of current processes
    unsigned long totalhigh;  // Total high memory (0)
    unsigned long freehigh;   // Free high memory (0)
    unsigned int  mem_unit;   // Memory unit size in bytes
    unsigned long cached;     // Kernel heap used as proxy for cached
    unsigned long available;  // Estimated available memory
} k_sysinfo_t;

// Debug/diagnostic syscalls (LikeOS specific)
#define SYS_MEMSTATS        300  // Print memory stats

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
#define O_EXCL          0x0080
#define O_TRUNC         0x0200
#define O_APPEND        0x0400
#define O_NONBLOCK      0x0800
#define O_CLOEXEC       0x80000

// Socket creation flags (OR'd into type)
#define SOCK_NONBLOCK   O_NONBLOCK
#define SOCK_CLOEXEC    O_CLOEXEC

// fcntl commands
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#ifndef FD_CLOEXEC
#define FD_CLOEXEC      1
#endif

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
#define EPIPE           32  // Broken pipe
#define EBUSY           16  // Device or resource busy
#define EEXIST          17  // File exists
#define EXDEV           18  // Cross-device link
#define EISDIR          21  // Is a directory
#define ENOSPC          28  // No space left on device
#define EROFS           30  // Read-only file system
#define ENAMETOOLONG    36  // File name too long
#define ENOSYS          38  // Function not implemented
#define ENOTEMPTY       39  // Directory not empty
#define ETIMEDOUT      110  // Connection timed out
#define ENOTSOCK        88  // Socket operation on non-socket
#define EDESTADDRREQ    89  // Destination address required
#define EPROTOTYPE      91  // Protocol wrong type for socket
#define ENOPROTOOPT     92  // Protocol not available
#define ESOCKTNOSUPPORT 94  // Socket type not supported
#define EOPNOTSUPP      95  // Operation not supported
#define EAFNOSUPPORT    97  // Address family not supported
#define EADDRINUSE      98  // Address already in use
#define ENETDOWN       100  // Network is down
#define ENETUNREACH    101  // Network is unreachable
#define ECONNABORTED   103  // Connection aborted
#define ECONNRESET     104  // Connection reset by peer
#define EISCONN        106  // Already connected
#define ENOTCONN       107  // Not connected
#define ECONNREFUSED   111  // Connection refused
#define EINPROGRESS    115  // Operation now in progress
#define ERANGE          34  // Math result not representable
#define ELOOP           40  // Too many symbolic links
#define ENFILE          23  // File table overflow

// Syscall handler prototype
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2,
                        uint64_t a3, uint64_t a4, uint64_t a5);

// ============================================================================
// Process info structure for SYS_GETPROCINFO
// ============================================================================
typedef struct procinfo {
    int     pid;            // Process ID
    int     ppid;           // Parent PID
    int     tgid;           // Thread group ID
    int     pgid;           // Process group ID
    int     sid;            // Session ID
    int     uid;            // Real user ID
    int     gid;            // Real group ID
    int     euid;           // Effective user ID
    int     egid;           // Effective group ID
    int     state;          // 0=READY 1=RUNNING 2=BLOCKED 3=STOPPED 4=ZOMBIE
    int     nice;           // Nice value (always 0 for now)
    int     nr_threads;     // Number of threads in thread group
    int     on_cpu;         // CPU number the task runs on
    int     exit_code;      // Exit status (for zombies)
    int     tty_nr;         // Controlling terminal (0 = none)
    int     is_kernel;      // 1 if kernel task, 0 if user
    uint64_t start_tick;    // Tick when process started
    uint64_t utime_ticks;   // User-mode ticks
    uint64_t stime_ticks;   // Kernel-mode ticks
    uint64_t vsz;           // Virtual memory size (bytes)
    uint64_t rss;           // Resident set size (pages)
    char    comm[256];      // Process name (basename of executable)
    char    cmdline[1024];  // Full command line (argv joined by spaces)
    char    environ[2048];  // Environment (envp joined by spaces)
    char    cwd[256];       // Current working directory
} procinfo_t;

#endif // _KERNEL_SYSCALL_H_
