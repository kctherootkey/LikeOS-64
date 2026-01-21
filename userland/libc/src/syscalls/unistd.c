#include "../../include/unistd.h"
#include "../../include/errno.h"
#include "../../include/sys/wait.h"
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

int pipe(int pipefd[2]) {
    long ret = syscall1(SYS_PIPE, (long)pipefd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    long ret = syscall3(SYS_LSEEK, fd, offset, whence);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

pid_t getpid(void) {
    return syscall0(SYS_GETPID);
}

pid_t getppid(void) {
    return syscall0(SYS_GETPPID);
}

pid_t fork(void) {
    long ret = syscall0(SYS_FORK);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int execve(const char* pathname, char* const argv[], char* const envp[]) {
    long ret = syscall3(SYS_EXECVE, (long)pathname, (long)argv, (long)envp);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

void _exit(int status) {
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

int dup(int oldfd) {
    long ret = syscall1(SYS_DUP, oldfd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

int dup2(int oldfd, int newfd) {
    long ret = syscall2(SYS_DUP2, oldfd, newfd);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

pid_t wait(int* status) {
    return waitpid(-1, status, 0);
}

pid_t wait4(pid_t pid, int* status, int options, void* rusage) {
    (void)rusage;
    long ret = syscall3(SYS_WAIT4, pid, (long)status, options);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    // If WNOHANG is not set, loop until child exits
    while (1) {
        long ret = syscall3(SYS_WAIT4, pid, (long)status, options);
        if (ret >= 0) {
            return ret;
        }
        // EAGAIN (11) means no child has exited yet - retry
        // ECHILD (10) means no children exist - return error
        if (ret == -11 && !(options & WNOHANG)) {
            // Yield and retry
            sched_yield();
            continue;
        }
        errno = -ret;
        return -1;
    }
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
