#include "../../include/unistd.h"
#include "../../include/errno.h"
#include "syscall.h"

int errno = 0;

int open(const char* pathname, int flags, ...) {
    // mode argument ignored for now (no create support yet)
    long ret = syscall3(SYS_OPEN, (long)pathname, flags, 0);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

ssize_t read(int fd, void* buf, size_t count) {
    long ret = syscall3(SYS_READ, fd, (long)buf, count);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

ssize_t write(int fd, const void* buf, size_t count) {
    long ret = syscall3(SYS_WRITE, fd, (long)buf, count);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int close(int fd) {
    long ret = syscall1(SYS_CLOSE, fd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    // TODO: Implement SYS_LSEEK in kernel
    (void)fd; (void)offset; (void)whence;
    errno = ENOSYS;
    return -1;
}

pid_t getpid(void) {
    return syscall0(SYS_GETPID);
}

void _exit(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

void* sbrk(intptr_t increment) {
    static void* current_brk = NULL;
    
    if (current_brk == NULL) {
        // Get initial brk
        current_brk = (void*)syscall1(SYS_BRK, 0);
    }
    
    if (increment == 0) {
        return current_brk;
    }
    
    void* new_brk = (void*)((char*)current_brk + increment);
    void* result = (void*)syscall1(SYS_BRK, (long)new_brk);
    
    if (result == current_brk) {
        errno = ENOMEM;
        return (void*)-1;
    }
    
    void* old_brk = current_brk;
    current_brk = result;
    return old_brk;
}

int brk(void* addr) {
    void* result = (void*)syscall1(SYS_BRK, (long)addr);
    if (result != addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int sched_yield(void) {
    return syscall0(SYS_YIELD);
}
