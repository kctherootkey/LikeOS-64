/*
 * which - locate a command
 *
 * Usage: which [-as] filename ...
 *
 * Searches PATH for executables matching the given names.
 *
 * Options:
 *   -a   Print all matching pathnames of each argument
 *   -s   No output, just return 0 if all found, 1 if any missing
 *
 * Exit status:
 *   0  All specified commands were found and executable
 *   1  One or more commands not found or not executable
 *   2  Invalid option specified
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>

static int find_in_path(const char *name, int show_all, int silent)
{
    /* If name contains '/', check it directly */
    const char *p;
    for (p = name; *p; p++) {
        if (*p == '/') {
            struct stat st;
            if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
                if (!silent)
                    printf("%s\n", name);
                return 0;
            }
            return 1;
        }
    }

    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/local/bin";

    int found = 0;
    const char *start = path;
    const char *cur = path;

    while (1) {
        if (*cur == ':' || *cur == '\0') {
            size_t dirlen = (size_t)(cur - start);
            size_t namelen = strlen(name);
            char full[1024];

            if (dirlen == 0) {
                /* Empty component means current directory */
                full[0] = '.';
                full[1] = '/';
                strcpy(full + 2, name);
            } else if (dirlen + 1 + namelen + 1 <= sizeof(full)) {
                memcpy(full, start, dirlen);
                if (full[dirlen - 1] != '/') {
                    full[dirlen] = '/';
                    strcpy(full + dirlen + 1, name);
                } else {
                    strcpy(full + dirlen, name);
                }
            } else {
                goto next;
            }

            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                if (!silent)
                    printf("%s\n", full);
                found = 1;
                if (!show_all)
                    return 0;
            }

next:
            if (*cur == '\0')
                break;
            start = cur + 1;
        }
        cur++;
    }

    return found ? 0 : 1;
}

int main(int argc, char *argv[])
{
    int opt;
    int show_all = 0;
    int silent = 0;

    while ((opt = getopt(argc, argv, "as")) != -1) {
        switch (opt) {
        case 'a':
            show_all = 1;
            break;
        case 's':
            silent = 1;
            break;
        default:
            fprintf(stderr, "Usage: which [-as] filename ...\n");
            return 2;
        }
    }

    if (optind >= argc) {
        if (!silent)
            fprintf(stderr, "Usage: which [-as] filename ...\n");
        return 1;
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        if (find_in_path(argv[i], show_all, silent) != 0)
            ret = 1;
    }

    return ret;
}
