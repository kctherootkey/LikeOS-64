/*
 * rmdir - remove empty directories
 *
 * Full implementation per rmdir(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "rmdir"

static int opt_verbose = 0;
static int opt_parents = 0;
static int opt_ignore_non_empty = 0;

static void usage(void) {
    fprintf(stderr,
        "Usage: " PROGRAM_NAME " [OPTION]... DIRECTORY...\n"
        "Remove the DIRECTORY(ies), if they are empty.\n"
        "\n"
        "      --ignore-fail-on-non-empty\n"
        "                    ignore each failure to remove a non-empty directory\n"
        "  -p, --parents     remove DIRECTORY and its ancestors; e.g.,\n"
        "                    'rmdir -p a/b' is similar to 'rmdir a/b a'\n"
        "  -v, --verbose     output a diagnostic for every directory processed\n"
        "      --help        display this help and exit\n"
        "      --version     output version information and exit\n");
}

static void version(void) {
    printf(PROGRAM_NAME " " VERSION "\n");
}

/* Remove a single directory, respecting our options.
 * Returns 0 on success, -1 on error (already reported). */
static int do_rmdir(const char *dir) {
    if (rmdir(dir) < 0) {
        if (opt_ignore_non_empty && errno == ENOTEMPTY)
            return -1;  /* silently ignored */
        fprintf(stderr, PROGRAM_NAME ": failed to remove '%s': %d\n",
                dir, errno);
        return -1;
    }
    if (opt_verbose)
        printf(PROGRAM_NAME ": removing directory, '%s'\n", dir);
    return 0;
}

/* Remove directory and then walk up parent components.
 * "rmdir -p a/b/c" is like "rmdir a/b/c a/b a" */
static int do_rmdir_parents(const char *path) {
    char buf[4096];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        fprintf(stderr, PROGRAM_NAME ": path too long: '%s'\n", path);
        return -1;
    }
    memcpy(buf, path, len + 1);

    /* Remove trailing slashes */
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';

    /* First remove the given directory */
    if (do_rmdir(buf) < 0) {
        if (opt_ignore_non_empty)
            return 0;
        return -1;
    }

    /* Now strip one path component at a time and remove each parent */
    while (len > 0) {
        /* Find last slash */
        char *slash = NULL;
        for (char *p = buf + len - 1; p > buf; p--) {
            if (*p == '/') {
                slash = p;
                break;
            }
        }
        if (!slash)
            break;

        *slash = '\0';
        len = (size_t)(slash - buf);

        /* Remove trailing slashes */
        while (len > 1 && buf[len - 1] == '/') {
            buf[--len] = '\0';
        }

        if (len == 0)
            break;

        if (do_rmdir(buf) < 0) {
            if (opt_ignore_non_empty)
                return 0;
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    enum {
        OPT_IGNORE_NON_EMPTY = 256,
        OPT_HELP,
        OPT_VERSION,
    };
    static const struct option long_options[] = {
        {"ignore-fail-on-non-empty", no_argument, NULL, OPT_IGNORE_NON_EMPTY},
        {"parents",                  no_argument, NULL, 'p'},
        {"verbose",                  no_argument, NULL, 'v'},
        {"help",                     no_argument, NULL, OPT_HELP},
        {"version",                  no_argument, NULL, OPT_VERSION},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "pv", long_options, NULL)) != -1) {
        switch (c) {
        case 'p':
            opt_parents = 1;
            break;
        case 'v':
            opt_verbose = 1;
            break;
        case OPT_IGNORE_NON_EMPTY:
            opt_ignore_non_empty = 1;
            break;
        case OPT_HELP:
            usage();
            return 0;
        case OPT_VERSION:
            version();
            return 0;
        default:
            usage();
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, PROGRAM_NAME ": missing operand\n");
        usage();
        return 1;
    }

    int errors = 0;
    for (int i = optind; i < argc; i++) {
        if (opt_parents) {
            if (do_rmdir_parents(argv[i]) < 0)
                errors++;
        } else {
            if (do_rmdir(argv[i]) < 0) {
                if (!opt_ignore_non_empty)
                    errors++;
            }
        }
    }

    return errors > 0 ? 1 : 0;
}
