/* <sys/syscall.h> -- system call numbers, for syscall(2).
 *
 * The numbers the kernel dispatches on (include/kernel/ke/syscall.h).  The
 * libc's own wrappers use them through src/syscalls/syscall.h; a program
 * that needs a call the libc does not wrap yet uses syscall(SYS_xxx, ...). */
#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_LSEEK       8
#define SYS_BRK         12
#define SYS_MMAP        9
#define SYS_MUNMAP      11
#define SYS_EXIT        60
#define SYS_GETPID      39
#define SYS_YIELD       24
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_WAIT4       61
#define SYS_GETPPID     110
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_PIPE        22
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
#define SYS_SYNC        338
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
#define SYS_MEMSTATS        300
#define SYS_CLONE           310
#define SYS_VFORK           311
#define SYS_EXIT_GROUP      312
#define SYS_GETTID          313
#define SYS_SET_TID_ADDRESS 314
#define SYS_FUTEX           315
#define SYS_SET_ROBUST_LIST 316
#define SYS_GET_ROBUST_LIST 317
#define SYS_ARCH_PRCTL      318
#define SYS_FUTEX_REQUEUE   319
#define SYS_SCHED_SETAFFINITY       320
#define SYS_SCHED_GETAFFINITY       321
#define SYS_SCHED_SETSCHEDULER      322
#define SYS_SCHED_GETSCHEDULER      323
#define SYS_SCHED_SETPARAM          324
#define SYS_SCHED_GETPARAM          325
#define SYS_SCHED_GET_PRIORITY_MAX  326
#define SYS_SCHED_GET_PRIORITY_MIN  327
#define SYS_SCHED_RR_GET_INTERVAL   328
#define SYS_MPROTECT        329
#define SYS_MADVISE         406
#define SYS_REBOOT          330
#define SYS_GETPROCINFO     331
#define SYS_UTIMENSAT       332
#define SYS_STATFS          333
#define SYS_FSTATFS         334
#define SYS_SYSINFO         335
#define SYS_KLOGCTL         336
#define SYS_SETTIMEOFDAY    337
#define SYS_SOCKET      340
#define SYS_BIND        341
#define SYS_LISTEN      342
#define SYS_ACCEPT      343
#define SYS_CONNECT     344
#define SYS_SENDTO      345
#define SYS_RECVFROM    346
#define SYS_SEND        347
#define SYS_RECV        348
#define SYS_SHUTDOWN    349
#define SYS_SETSOCKOPT  350
#define SYS_GETSOCKOPT  351
#define SYS_GETPEERNAME 352
#define SYS_GETSOCKNAME 353
#define SYS_SOCKETPAIR  354
#define SYS_ACCEPT4     355
#define SYS_SENDMSG     356
#define SYS_RECVMSG     357
#define SYS_SENDFILE    358
#define SYS_SELECT      359
#define SYS_PSELECT6    360
#define SYS_POLL        361
#define SYS_PPOLL       362
#define SYS_EPOLL_CREATE  363
#define SYS_EPOLL_CREATE1 364
#define SYS_EPOLL_CTL   365
#define SYS_EPOLL_WAIT  366
#define SYS_EPOLL_PWAIT 367
#define SYS_DUP3        368
#define SYS_DNS_RESOLVE 369
#define SYS_SETHOSTNAME 370
#define SYS_NET_GETINFO 371
#define SYS_DHCP_CONTROL 372
#define SYS_RAW_SEND    373
#define SYS_RAW_RECV    374
#define SYS_DNS_RESOLVE_REVERSE 375
#define SYS_SET_DNS_SERVER 376
#define SYS_SETSID      380
#define SYS_GETSID      381
#define SYS_GETPGID     382
#define SYS_GETRUSAGE   383
#define SYS_READV       384
#define SYS_WRITEV      385
#define SYS_GETRANDOM   386
#define SYS_SETRESUID   387
#define SYS_GETRESUID   388
#define SYS_SETRESGID   389
#define SYS_GETRESGID   390
#define SYS_SETXATTR    391   /* (path, name, val, size, flags|0x40000000=nofollow) */
#define SYS_GETXATTR    392   /* (path, name, val, size, nofollow) */
#define SYS_LISTXATTR   393   /* (path, list, size, nofollow) */
#define SYS_REMOVEXATTR 394   /* (path, name, nofollow) */
#define SYS_FSETXATTR   395   /* (fd, name, val, size, flags) */
#define SYS_FGETXATTR   396   /* (fd, name, val, size) */
#define SYS_FLISTXATTR  397   /* (fd, list, size) */
#define SYS_FREMOVEXATTR 398  /* (fd, name) */
#define SYS_DEBUG_DUMP  399   /* () root-only: dump kernel diag tables to tty */
#define SYS_UNLINKAT  405
#define SYS_CHROOT      400   /* (path) root-only: confine path resolution to a subtree */
#define SYS_SHMGET      401
#define SYS_SHMAT       402
#define SYS_SHMDT       403
#define SYS_SHMCTL      404
#define SYS_GETPROCMAPS 407   /* (pid, procmapinfo*, procmap*, max) */
#define SYS_MINCORE     409   /* (addr, length, vec) page residency, one byte per page */
#define SYS_CLOCK_NANOSLEEP 410 /* (clockid, flags, req, rem) */
#define SYS_MREMAP      411 /* (old_addr, old_size, new_size, flags, new_addr) */
#define SYS_EVENTFD2    412
#define SYS_TIMERFD_CREATE  413
#define SYS_TIMERFD_SETTIME 414
#define SYS_TIMERFD_GETTIME 415
#define SYS_MEMFD_CREATE    416
#define SYS_PREAD64     417
#define SYS_PWRITE64    418
#define SYS_FDATASYNC   419
#define SYS_FALLOCATE   420
#define SYS_TKILL       258   /* (tid, sig) signal ONE thread */
#define SYS_TGKILL      259   /* (tgid, tid, sig) same, group-checked */
#define SYS_PTRACE      408   /* (request, pid, addr, data) — see sys/ptrace.h */

/* Lower-case aliases, the spelling most portable code uses. */
#define SYS_futex SYS_FUTEX
#define SYS_gettid SYS_GETTID
#define SYS_memfd_create SYS_MEMFD_CREATE
#define SYS_getrandom SYS_GETRANDOM
#define SYS_clock_gettime SYS_CLOCK_GETTIME

#endif /* _SYS_SYSCALL_H */
