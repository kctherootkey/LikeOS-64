#include "../../include/sys/mman.h"
#include "../../include/errno.h"
#include "syscall.h"

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) {
    long ret = syscall6(SYS_MMAP, (long)addr, length, prot, flags, fd, offset);
    if (ret < 0 && ret > -4096) {
        errno = -ret;
        return MAP_FAILED;
    }
    return (void*)ret;
}

int munmap(void* addr, size_t length) {
    long ret = syscall2(SYS_MUNMAP, (long)addr, length);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}
