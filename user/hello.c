#include <stdio.h>

int main(int argc, char** argv) {
    printf("Hello from userland!\n");
    if (argc > 1 && argv && argv[1]) {
        printf("Argument: %s\n", argv[1]);
    }
    return 0;
}
