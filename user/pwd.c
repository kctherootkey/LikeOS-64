#include <stdio.h>
#include <unistd.h>

int main(void) {
    char buf[256];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        printf("/\n");
        return 0;
    }
    printf("%s\n", buf);
    return 0;
}
