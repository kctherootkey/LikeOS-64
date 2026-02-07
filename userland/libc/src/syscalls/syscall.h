// Syscall numbers (must match kernel)
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

// Process syscalls
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_WAIT4       61
#define SYS_GETPPID     110
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_PIPE        22

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
#define SYS_PAUSE           270
#define SYS_NANOSLEEP       271
#define SYS_CLOCK_GETTIME   272
#define SYS_CLOCK_GETRES    273

// Syscall wrapper - uses inline assembly to invoke syscall instruction
static inline long syscall0(long number) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall1(long number, long arg1) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall2(long number, long arg1, long arg2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall3(long number, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall4(long number, long arg1, long arg2, long arg3, long arg4) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall5(long number, long arg1, long arg2, long arg3, long arg4, long arg5) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long syscall6(long number, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8 __asm__("r8") = arg5;
    register long r9 __asm__("r9") = arg6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}
