// memstat - Display memory statistics for LikeOS-64
// Usage: memstat

#include <stdio.h>

#define SYS_MEMSTATS 300

static long syscall0(long num) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

int main(void) {
    printf("Memory statistics:\n");
    syscall0(SYS_MEMSTATS);
    return 0;
}
