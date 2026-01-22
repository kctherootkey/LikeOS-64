#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

static int cat_fd(int fd) {
    char buf[512];
    while (1) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            break;
        }
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(r - off));
            if (w < 0) {
                return -1;
            }
            off += w;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        if (cat_fd(STDIN_FILENO) != 0) {
            printf("cat: read error (%d)\n", errno);
            return 1;
        }
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("cat: cannot open %s (%d)\n", argv[i], errno);
            continue;
        }
        if (cat_fd(fd) != 0) {
            printf("cat: read error (%d)\n", errno);
            close(fd);
            return 1;
        }
        close(fd);
    }
    return 0;
}
