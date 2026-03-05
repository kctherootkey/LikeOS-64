// ld-likeos.so — Minimal syscall stubs (no libc dependency)
#ifndef _RTLD_SYSCALL_H
#define _RTLD_SYSCALL_H

// Self-contained type definitions (no system headers needed)
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef unsigned long      size_t;
typedef signed long        ssize_t;

#define NULL ((void *)0)

// Syscall numbers (match kernel)
#define __NR_read       0
#define __NR_write      1
#define __NR_open       2
#define __NR_close      3
#define __NR_lseek      8
#define __NR_mmap       9
#define __NR_munmap     11
#define __NR_mprotect   329
#define __NR_brk        12
#define __NR_exit       60
#define __NR_arch_prctl 318

// mmap constants
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void*)-1)

// open flags
#define O_RDONLY 0

// lseek whence
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

// arch_prctl codes
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static inline long __syscall0(long n) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n) : "rcx","r11","memory");
    return ret;
}
static inline long __syscall1(long n, long a) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n),"D"(a) : "rcx","r11","memory");
    return ret;
}
static inline long __syscall2(long n, long a, long b) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n),"D"(a),"S"(b) : "rcx","r11","memory");
    return ret;
}
static inline long __syscall3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n),"D"(a),"S"(b),"d"(c) : "rcx","r11","memory");
    return ret;
}
static inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n),"D"(a1),"S"(a2),"d"(a3),
                     "r"(r10),"r"(r8),"r"(r9) : "rcx","r11","memory");
    return ret;
}

static inline int rtld_open(const char* path, int flags) {
    return (int)__syscall3(__NR_open, (long)path, flags, 0);
}
static inline long rtld_read(int fd, void* buf, size_t n) {
    return __syscall3(__NR_read, fd, (long)buf, (long)n);
}
static inline int rtld_close(int fd) {
    return (int)__syscall1(__NR_close, fd);
}
static inline long rtld_lseek(int fd, long off, int whence) {
    return __syscall3(__NR_lseek, fd, off, whence);
}
static inline void* rtld_mmap(void* addr, size_t len, int prot, int flags, int fd, long off) {
    long r = __syscall6(__NR_mmap, (long)addr, (long)len, prot, flags, fd, off);
    if (r < 0 && r > -4096) return MAP_FAILED;
    return (void*)r;
}
static inline int rtld_munmap(void* addr, size_t len) {
    return (int)__syscall2(__NR_munmap, (long)addr, (long)len);
}
static inline int rtld_mprotect(void* addr, size_t len, int prot) {
    return (int)__syscall3(__NR_mprotect, (long)addr, (long)len, prot);
}
static inline void rtld_write_str(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    __syscall3(__NR_write, 1, (long)s, (long)n);
}
static inline void rtld_exit(int code) {
    __syscall1(__NR_exit, code);
    __builtin_unreachable();
}
static inline int rtld_arch_prctl(int code, unsigned long addr) {
    return (int)__syscall2(__NR_arch_prctl, code, (long)addr);
}

#endif // _RTLD_SYSCALL_H
