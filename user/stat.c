#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

static void print_stat(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("stat: cannot stat %s (%d)\n", path, errno);
        return;
    }
    char type = '-';
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        type = 'd';
    } else if ((st.st_mode & S_IFMT) == S_IFREG) {
        type = 'f';
    }
    printf("%s: type=%c size=%lu\n", path, type, (unsigned long)st.st_size);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: stat <path>\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        print_stat(argv[i]);
    }
    return 0;
}
