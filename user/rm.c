/*
 * rm - remove files or directories
 *
 * Full implementation per rm(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "rm"

/* Interactive mode levels */
enum interactive_mode {
    INTER_NEVER  = 0,
    INTER_ONCE   = 1,   /* -I: prompt once for >3 files or if recursive */
    INTER_ALWAYS = 2,   /* -i: prompt before every removal */
};

static int opt_force = 0;
static int opt_recursive = 0;
static int opt_dir = 0;        /* -d, --dir: remove empty directories */
static int opt_verbose = 0;
static int opt_preserve_root = 1;   /* default: do not remove '/' */
static int opt_one_file_system = 0;
static enum interactive_mode opt_interactive = INTER_NEVER;

static void usage(void) {
    fprintf(stderr,
        "Usage: " PROGRAM_NAME " [OPTION]... [FILE]...\n"
        "Remove (unlink) the FILE(s).\n"
        "\n"
        "  -f, --force           ignore nonexistent files and arguments, never prompt\n"
        "  -i                    prompt before every removal\n"
        "  -I                    prompt once before removing more than three files,\n"
        "                        or when removing recursively\n"
        "      --interactive[=WHEN]  prompt according to WHEN: never, once (-I),\n"
        "                        or always (-i); without WHEN, prompt always\n"
        "      --one-file-system when removing a hierarchy recursively, skip any\n"
        "                        directory that is on a file system different from\n"
        "                        that of the corresponding command line argument\n"
        "      --no-preserve-root  do not treat '/' specially\n"
        "      --preserve-root[=all]  do not remove '/' (default)\n"
        "  -r, -R, --recursive   remove directories and their contents recursively\n"
        "  -d, --dir             remove empty directories\n"
        "  -v, --verbose         explain what is being done\n"
        "      --help            display this help and exit\n"
        "      --version         output version information and exit\n");
}

static void version(void) {
    printf(PROGRAM_NAME " " VERSION "\n");
}

/* Prompt user for confirmation. Returns 1 for yes, 0 for no. */
static int prompt_yes(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    char buf[16];
    if (fgets(buf, sizeof(buf), stdin) == NULL)
        return 0;
    return (buf[0] == 'y' || buf[0] == 'Y');
}

/* Check if a path is "/" (root). */
static int is_root(const char *path) {
    /* Normalize: skip trailing slashes */
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    return (len == 1 && path[0] == '/');
}

/* Remove a single file (not directory). */
static int remove_file(const char *path) {
    if (opt_interactive == INTER_ALWAYS) {
        if (!prompt_yes(PROGRAM_NAME ": remove '%s'? ", path))
            return 0;
    }

    if (unlink(path) < 0) {
        if (!opt_force) {
            fprintf(stderr, PROGRAM_NAME ": cannot remove '%s': %s\n",
                    path, strerror(errno));
            return -1;
        }
        return 0;
    }

    if (opt_verbose)
        printf("removed '%s'\n", path);
    return 0;
}

/* Forward declaration for recursive removal */
static int remove_entry(const char *path);

/* Recursively remove a directory and its contents. */
static int remove_dir_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        if (!opt_force) {
            fprintf(stderr, PROGRAM_NAME ": cannot open directory '%s': %s\n",
                    path, strerror(errno));
            return -1;
        }
        return 0;
    }

    int errors = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[4096];
        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);
        if (plen + 1 + nlen >= sizeof(child)) {
            fprintf(stderr, PROGRAM_NAME ": path too long\n");
            errors++;
            continue;
        }
        memcpy(child, path, plen);
        if (plen > 0 && path[plen - 1] != '/')
            child[plen++] = '/';
        memcpy(child + plen, ent->d_name, nlen + 1);

        if (remove_entry(child) < 0)
            errors++;
    }
    closedir(d);

    /* Now remove the (now-empty) directory itself */
    if (opt_interactive == INTER_ALWAYS) {
        if (!prompt_yes(PROGRAM_NAME ": remove directory '%s'? ", path))
            return 0;
    }

    if (rmdir(path) < 0) {
        fprintf(stderr, PROGRAM_NAME ": cannot remove '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    if (opt_verbose)
        printf("removed directory '%s'\n", path);

    return errors > 0 ? -1 : 0;
}

/* Remove a single entry (file or directory, possibly recursive). */
static int remove_entry(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (opt_force && errno == ENOENT)
            return 0;
        fprintf(stderr, PROGRAM_NAME ": cannot stat '%s': %s\n",
                path, strerror(errno));
        return opt_force ? 0 : -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (opt_recursive) {
            /* Check preserve-root */
            if (opt_preserve_root && is_root(path)) {
                fprintf(stderr, PROGRAM_NAME ": it is dangerous to operate recursively on '/'\n");
                fprintf(stderr, PROGRAM_NAME ": use --no-preserve-root to override this failsafe\n");
                return -1;
            }
            return remove_dir_recursive(path);
        } else if (opt_dir) {
            /* -d: remove empty directory */
            if (opt_interactive == INTER_ALWAYS) {
                if (!prompt_yes(PROGRAM_NAME ": remove directory '%s'? ", path))
                    return 0;
            }
            if (rmdir(path) < 0) {
                fprintf(stderr, PROGRAM_NAME ": cannot remove '%s': %s\n",
                        path, strerror(errno));
                return -1;
            }
            if (opt_verbose)
                printf("removed directory '%s'\n", path);
            return 0;
        } else {
            fprintf(stderr, PROGRAM_NAME ": cannot remove '%s': Is a directory\n", path);
            return -1;
        }
    }

    return remove_file(path);
}

int main(int argc, char **argv) {
    enum {
        OPT_INTERACTIVE = 256,
        OPT_ONE_FILE_SYSTEM,
        OPT_NO_PRESERVE_ROOT,
        OPT_PRESERVE_ROOT,
        OPT_HELP,
        OPT_VERSION,
    };
    static const struct option long_options[] = {
        {"force",            no_argument,       NULL, 'f'},
        {"interactive",      optional_argument, NULL, OPT_INTERACTIVE},
        {"one-file-system",  no_argument,       NULL, OPT_ONE_FILE_SYSTEM},
        {"no-preserve-root", no_argument,       NULL, OPT_NO_PRESERVE_ROOT},
        {"preserve-root",    optional_argument, NULL, OPT_PRESERVE_ROOT},
        {"recursive",        no_argument,       NULL, 'r'},
        {"dir",              no_argument,       NULL, 'd'},
        {"verbose",          no_argument,       NULL, 'v'},
        {"help",             no_argument,       NULL, OPT_HELP},
        {"version",          no_argument,       NULL, OPT_VERSION},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "fiIrRdv", long_options, NULL)) != -1) {
        switch (c) {
        case 'f':
            opt_force = 1;
            opt_interactive = INTER_NEVER;
            break;
        case 'i':
            opt_interactive = INTER_ALWAYS;
            opt_force = 0;
            break;
        case 'I':
            opt_interactive = INTER_ONCE;
            opt_force = 0;
            break;
        case OPT_INTERACTIVE:
            if (!optarg || strcmp(optarg, "always") == 0)
                opt_interactive = INTER_ALWAYS;
            else if (strcmp(optarg, "once") == 0)
                opt_interactive = INTER_ONCE;
            else if (strcmp(optarg, "never") == 0)
                opt_interactive = INTER_NEVER;
            else {
                fprintf(stderr, PROGRAM_NAME ": invalid argument '%s' for '--interactive'\n", optarg);
                return 1;
            }
            break;
        case 'r':
        case 'R':
            opt_recursive = 1;
            break;
        case 'd':
            opt_dir = 1;
            break;
        case 'v':
            opt_verbose = 1;
            break;
        case OPT_ONE_FILE_SYSTEM:
            opt_one_file_system = 1;
            break;
        case OPT_NO_PRESERVE_ROOT:
            opt_preserve_root = 0;
            break;
        case OPT_PRESERVE_ROOT:
            opt_preserve_root = 1;
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
        if (opt_force)
            return 0;
        fprintf(stderr, PROGRAM_NAME ": missing operand\n");
        usage();
        return 1;
    }

    int nfiles = argc - optind;

    /* -I (INTER_ONCE): prompt once if >3 files or recursive */
    if (opt_interactive == INTER_ONCE) {
        if (nfiles > 3 || opt_recursive) {
            if (!prompt_yes(PROGRAM_NAME ": remove %d argument%s%s? ",
                            nfiles, nfiles > 1 ? "s" : "",
                            opt_recursive ? " recursively" : "")) {
                return 0;
            }
        }
        /* After the one prompt, don't prompt again */
    }

    int errors = 0;
    for (int i = optind; i < argc; i++) {
        if (remove_entry(argv[i]) < 0)
            errors++;
    }

    return errors > 0 ? 1 : 0;
}
