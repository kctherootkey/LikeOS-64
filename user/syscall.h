// LikeOS-64 Userspace Syscall Interface
#ifndef _USER_SYSCALL_H_
#define _USER_SYSCALL_H_

typedef unsigned long size_t;
typedef long ssize_t;
typedef long int64_t;
typedef unsigned long uint64_t;

// Syscall numbers (Linux-compatible)
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_MMAP        9
#define SYS_BRK         12
#define SYS_YIELD       24
#define SYS_GETPID      39
#define SYS_EXIT        60

// Standard file descriptors
#define STDIN_FD        0
#define STDOUT_FD       1
#define STDERR_FD       2

// mmap flags
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

#define MAP_FAILED      ((void*)-1)

// Raw syscall interface
static inline int64_t syscall0(int64_t num) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall1(int64_t num, int64_t a1) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall2(int64_t num, int64_t a1, int64_t a2) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall3(int64_t num, int64_t a1, int64_t a2, int64_t a3) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall4(int64_t num, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall6(int64_t num, int64_t a1, int64_t a2, int64_t a3, 
                                int64_t a4, int64_t a5, int64_t a6) {
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    register int64_t r8 __asm__("r8") = a5;
    register int64_t r9 __asm__("r9") = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// Syscall wrappers
static inline void exit(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static inline ssize_t write(int fd, const void* buf, size_t count) {
    return syscall3(SYS_WRITE, fd, (int64_t)buf, count);
}

static inline ssize_t read(int fd, void* buf, size_t count) {
    return syscall3(SYS_READ, fd, (int64_t)buf, count);
}

static inline int open(const char* pathname, int flags) {
    return (int)syscall3(SYS_OPEN, (int64_t)pathname, flags, 0);
}

static inline int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

static inline void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) {
    return (void*)syscall6(SYS_MMAP, (int64_t)addr, length, prot, flags, fd, offset);
}

static inline void* brk(void* addr) {
    return (void*)syscall1(SYS_BRK, (int64_t)addr);
}

static inline int sched_yield(void) {
    return (int)syscall0(SYS_YIELD);
}

static inline int getpid(void) {
    return (int)syscall0(SYS_GETPID);
}

// Helper: get string length
static inline size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

// Helper: print string to stdout
static inline void print(const char* s) {
    write(STDOUT_FD, s, strlen(s));
}

// Helper: print a number (decimal)
static inline void print_num(long n) {
    char buf[32];
    int i = 0;
    int neg = 0;
    
    if (n < 0) {
        neg = 1;
        n = -n;
    }
    
    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }
    
    if (neg) buf[i++] = '-';
    
    // Reverse
    char out[32];
    int j = 0;
    while (i > 0) {
        out[j++] = buf[--i];
    }
    out[j] = '\0';
    
    write(STDOUT_FD, out, j);
}

// Helper: print hex number
static inline void print_hex(unsigned long n) {
    char buf[20];
    const char* hex = "0123456789abcdef";
    int i = 0;
    
    buf[i++] = '0';
    buf[i++] = 'x';
    
    if (n == 0) {
        buf[i++] = '0';
    } else {
        char tmp[16];
        int j = 0;
        while (n > 0) {
            tmp[j++] = hex[n & 0xf];
            n >>= 4;
        }
        while (j > 0) {
            buf[i++] = tmp[--j];
        }
    }
    buf[i] = '\0';
    
    write(STDOUT_FD, buf, i);
}

#endif // _USER_SYSCALL_H_
