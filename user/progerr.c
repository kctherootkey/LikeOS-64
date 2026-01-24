#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static void usage(const char* prog) {
    printf("Usage: %s <mode>\n", prog);
    printf("  modes:\n");
    printf("    ill        - illegal instruction\n");
    printf("    baduser    - write to invalid user address\n");
    printf("    badkernel  - write to kernel address\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "ill") == 0 || strcmp(argv[1], "illegal") == 0) {
        __asm__ volatile("ud2");
        return 0;
    }

    if (strcmp(argv[1], "baduser") == 0 || strcmp(argv[1], "invalid") == 0) {
        volatile uint64_t* p = (uint64_t*)0x1ULL;
        *p = 0xDEADBEEFCAFEBABEULL;
        return 0;
    }

    if (strcmp(argv[1], "badkernel") == 0 || strcmp(argv[1], "kernel") == 0) {
        volatile uint64_t* p = (uint64_t*)0xFFFFFFFF80000000ULL;
        *p = 0xDEADBEEFCAFEBABEULL;
        return 0;
    }

    usage(argv[0]);
    return 1;
}