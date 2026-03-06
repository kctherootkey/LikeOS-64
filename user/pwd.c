/*
 * pwd - print name of current/working directory
 *
 * Full implementation per pwd(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "pwd"

static void usage(void) {
    printf("Usage: %s [OPTION]...\n", PROGRAM_NAME);
    printf("Print the full filename of the current working directory.\n\n");
    printf("  -L, --logical    use PWD from environment, even if it contains symlinks\n");
    printf("  -P, --physical   avoid all symlinks\n");
    printf("      --help       display this help and exit\n");
    printf("      --version    output version information and exit\n");
    printf("\nIf no option is specified, -P is assumed.\n");
}

static void version(void) {
    printf("%s %s\n", PROGRAM_NAME, VERSION);
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"logical",  no_argument, 0, 'L'},
        {"physical", no_argument, 0, 'P'},
        {"help",     no_argument, 0, 'h'},
        {"version",  no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int use_logical = 0;

    int c;
    while ((c = getopt_long(argc, argv, "LP", long_options, NULL)) != -1) {
        switch (c) {
            case 'L':
                use_logical = 1;
                break;
            case 'P':
                use_logical = 0;
                break;
            case 'h':
                usage();
                return 0;
            case 'v':
                version();
                return 0;
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
                return 1;
        }
    }

    if (use_logical) {
        /*
         * -L: use PWD from environment.
         * If PWD is set and starts with /, print it.
         * Otherwise fall back to getcwd().
         */
        const char *pwd_env = getenv("PWD");
        if (pwd_env && pwd_env[0] == '/') {
            printf("%s\n", pwd_env);
            return 0;
        }
    }

    /* -P (default): use getcwd() to get the physical path */
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        fprintf(stderr, "%s: error retrieving current directory: %s\n",
                PROGRAM_NAME, strerror(errno));
        return 1;
    }

    printf("%s\n", buf);
    return 0;
}
