/*
 * mkdir - make directories
 *
 * Full implementation per mkdir(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "mkdir"

static int opt_verbose = 0;
static int opt_parents = 0;
static unsigned int opt_mode = 0777; /* default: a=rwx minus umask */
static int opt_mode_set = 0;

static void usage(void) {
    fprintf(stderr,
        "Usage: " PROGRAM_NAME " [OPTION]... DIRECTORY...\n"
        "Create the DIRECTORY(ies), if they do not already exist.\n"
        "\n"
        "  -m, --mode=MODE   set file mode (as in chmod), not a=rwx - umask\n"
        "  -p, --parents     no error if existing, make parent directories as needed\n"
        "  -v, --verbose     print a message for each created directory\n"
        "      --help        display this help and exit\n"
        "      --version     output version information and exit\n");
}

static void version(void) {
    printf(PROGRAM_NAME " " VERSION "\n");
}

/* Parse a simple octal mode string like "755" or "0755".
 * Returns the parsed mode or (unsigned int)-1 on error. */
static unsigned int parse_mode(const char *s) {
    unsigned int mode = 0;
    if (!s || !*s)
        return (unsigned int)-1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '7')
            return (unsigned int)-1;
        mode = (mode << 3) | (unsigned int)(*p - '0');
    }
    return mode & 07777;
}

/* Create parent directories as needed for path.
 * Only creates directories that don't exist yet.
 * Does NOT create the final component (that's done by the caller). */
static int make_parents(const char *path, unsigned int parent_mode) {
    char buf[4096];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        fprintf(stderr, PROGRAM_NAME ": path too long: '%s'\n", path);
        return -1;
    }
    memcpy(buf, path, len + 1);

    /* Walk the path, creating each directory component */
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(buf, &st) < 0) {
                if (mkdir(buf, parent_mode) < 0 && errno != EEXIST) {
                    fprintf(stderr, PROGRAM_NAME ": cannot create directory '%s': %d\n",
                            buf, errno);
                    return -1;
                }
                if (opt_verbose)
                    printf(PROGRAM_NAME ": created directory '%s'\n", buf);
            }
            *p = '/';
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    static const struct option long_options[] = {
        {"mode",    required_argument, NULL, 'm'},
        {"parents", no_argument,       NULL, 'p'},
        {"verbose", no_argument,       NULL, 'v'},
        {"help",    no_argument,       NULL, 'H'},
        {"version", no_argument,       NULL, 'V'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "m:pv", long_options, NULL)) != -1) {
        switch (c) {
        case 'm':
            opt_mode = parse_mode(optarg);
            if (opt_mode == (unsigned int)-1) {
                fprintf(stderr, PROGRAM_NAME ": invalid mode '%s'\n", optarg);
                return 1;
            }
            opt_mode_set = 1;
            break;
        case 'p':
            opt_parents = 1;
            break;
        case 'v':
            opt_verbose = 1;
            break;
        case 'H':
            usage();
            return 0;
        case 'V':
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
        const char *dir = argv[i];

        if (opt_parents) {
            /* Create all ancestor directories with default mode (0777)
             * Parent directories are NOT affected by -m */
            if (make_parents(dir, 0777) < 0) {
                errors++;
                continue;
            }
        }

        if (mkdir(dir, opt_mode) < 0) {
            if (opt_parents && errno == EEXIST) {
                /* With -p, existing directory is not an error */
                struct stat st;
                if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
                    continue;
            }
            fprintf(stderr, PROGRAM_NAME ": cannot create directory '%s': %d\n",
                    dir, errno);
            errors++;
            continue;
        }

        if (opt_verbose)
            printf(PROGRAM_NAME ": created directory '%s'\n", dir);
    }

    return errors > 0 ? 1 : 0;
}
