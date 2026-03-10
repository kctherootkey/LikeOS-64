/*
 * yes - output a string repeatedly until killed
 *
 * Full implementation per yes(1) manpage.
 * Supports: --help, --version
 *
 * Repeatedly output a line with all specified STRING(s), or 'y'.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PROGRAM_NAME "yes"
#define VERSION      "1.0"

static void print_help(void)
{
    printf("Usage: yes [STRING]...\n");
    printf("  or:  yes OPTION\n\n");
    printf("Repeatedly output a line with all specified STRING(s), or 'y'.\n\n");
    printf("      --help     display this help and exit\n");
    printf("      --version  output version information and exit\n");
}

static void print_version(void)
{
    printf("%s (LikeOS) %s\n", PROGRAM_NAME, VERSION);
}

int main(int argc, char *argv[])
{
    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            print_version();
            return 0;
        }
    }

    /*
     * Build the output line: concatenation of all arguments separated
     * by spaces, or "y" if none given.  Buffer it for efficiency.
     */
    char buf[8192];
    int pos = 0;

    if (argc <= 1) {
        buf[0] = 'y';
        buf[1] = '\n';
        pos = 2;
    } else {
        for (int i = 1; i < argc; i++) {
            const char *s = argv[i];
            while (*s && pos < (int)sizeof(buf) - 2) {
                buf[pos++] = *s++;
            }
            if (i < argc - 1 && pos < (int)sizeof(buf) - 2)
                buf[pos++] = ' ';
        }
        if (pos < (int)sizeof(buf) - 1)
            buf[pos++] = '\n';
    }

    /* Write in large chunks for performance */
    for (;;) {
        ssize_t w = write(STDOUT_FILENO, buf, (size_t)pos);
        if (w < 0)
            return 1;
    }
}
