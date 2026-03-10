/*
 * true - do nothing, successfully
 *
 * Full implementation per true(1) manpage.
 * Supports: --help, --version
 *
 * Exit with a status code indicating success.
 */
#include <stdio.h>
#include <string.h>

#define PROGRAM_NAME "true"
#define VERSION      "1.0"

int main(int argc, char *argv[])
{
    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0) {
            printf("Usage: true [ignored command line arguments]\n");
            printf("  or:  true OPTION\n\n");
            printf("Exit with a status code indicating success.\n\n");
            printf("      --help     display this help and exit\n");
            printf("      --version  output version information and exit\n");
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("%s (LikeOS) %s\n", PROGRAM_NAME, VERSION);
            return 0;
        }
    }

    return 0;
}
