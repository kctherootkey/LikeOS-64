/*
 * mv - move (rename) files
 *
 * Full implementation per mv(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "mv"
#define COPY_BUF_SIZE 32768

/* Interactive mode */
enum interactive_mode {
    INTER_DEFAULT = 0,  /* overwrite without asking */
    INTER_FORCE,        /* -f: never prompt */
    INTER_INTERACTIVE,  /* -i: always prompt */
    INTER_NO_CLOBBER,   /* -n: never overwrite */
};

/* Update mode */
enum update_mode {
    UPDATE_ALL = 0,
    UPDATE_NONE,
    UPDATE_OLDER,
};

/* Backup type */
enum backup_type {
    BACKUP_NONE = 0,
    BACKUP_SIMPLE,
    BACKUP_NUMBERED,
    BACKUP_EXISTING,
};

static enum interactive_mode opt_interactive = INTER_DEFAULT;
static enum update_mode opt_update = UPDATE_ALL;
static enum backup_type opt_backup = BACKUP_NONE;
static int opt_verbose = 0;
static int opt_debug = 0;
static int opt_no_copy = 0;
static int opt_strip_trailing_slashes = 0;
static int opt_no_target_directory = 0;
static char *opt_target_directory = NULL;
static char *opt_suffix = NULL;

static void usage(void) {
    fprintf(stderr,
        "Usage: " PROGRAM_NAME " [OPTION]... [-T] SOURCE DEST\n"
        "  or:  " PROGRAM_NAME " [OPTION]... SOURCE... DIRECTORY\n"
        "  or:  " PROGRAM_NAME " [OPTION]... -t DIRECTORY SOURCE...\n"
        "Rename SOURCE to DEST, or move SOURCE(s) to DIRECTORY.\n"
        "\n"
        "      --backup[=CONTROL]    make a backup of each existing destination file\n"
        "  -b                        like --backup but does not accept an argument\n"
        "      --debug               explain how a file is moved. Implies -v\n"
        "  -f, --force               do not prompt before overwriting\n"
        "  -i, --interactive         prompt before overwrite\n"
        "  -n, --no-clobber          do not overwrite an existing file\n"
        "      --no-copy             do not copy if renaming fails\n"
        "      --strip-trailing-slashes  remove any trailing slashes from each\n"
        "                            SOURCE argument\n"
        "  -S, --suffix=SUFFIX       override the usual backup suffix\n"
        "  -t, --target-directory=DIRECTORY  move all SOURCE arguments into DIRECTORY\n"
        "  -T, --no-target-directory  treat DEST as a normal file\n"
        "      --update[=UPDATE]     control which existing files are updated;\n"
        "                            UPDATE={all,none,older(default)}\n"
        "  -u                        equivalent to --update[=older]\n"
        "  -v, --verbose             explain what is being done\n"
        "      --help                display this help and exit\n"
        "      --version             output version information and exit\n");
}

static void version(void) {
    printf(PROGRAM_NAME " " VERSION "\n");
}

/* Strip trailing slashes from a string (in place). */
static void strip_trailing_slashes(char *s) {
    size_t len = strlen(s);
    while (len > 1 && s[len - 1] == '/') {
        s[--len] = '\0';
    }
}

/* Get the basename component of a path. */
static const char *my_basename(const char *path) {
    const char *p = path + strlen(path);
    while (p > path && *(p - 1) == '/')
        p--;
    while (p > path && *(p - 1) != '/')
        p--;
    return p;
}

/* Build a destination path: dir/basename(src) */
static void build_dest_path(char *dest, size_t dest_size,
                            const char *dir, const char *src) {
    const char *base = my_basename(src);
    snprintf(dest, dest_size, "%s/%s", dir, base);
}

/* Create a backup of an existing file if requested. */
static int make_backup(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;

    const char *suffix = opt_suffix ? opt_suffix : "~";
    char backup[4096];
    if (opt_backup == BACKUP_SIMPLE || opt_backup == BACKUP_EXISTING) {
        snprintf(backup, sizeof(backup), "%s%s", path, suffix);
        if (rename(path, backup) < 0) {
            fprintf(stderr, PROGRAM_NAME ": cannot create backup '%s': %d\n",
                    backup, errno);
            return -1;
        }
        if (opt_verbose || opt_debug)
            printf("backup: '%s' -> '%s'\n", path, backup);
    } else if (opt_backup == BACKUP_NUMBERED) {
        for (int i = 1; i < 9999; i++) {
            snprintf(backup, sizeof(backup), "%s.~%d~", path, i);
            if (stat(backup, &st) < 0) {
                if (rename(path, backup) < 0) {
                    fprintf(stderr, PROGRAM_NAME ": cannot create backup '%s': %d\n",
                            backup, errno);
                    return -1;
                }
                if (opt_verbose || opt_debug)
                    printf("backup: '%s' -> '%s'\n", path, backup);
                return 0;
            }
        }
        fprintf(stderr, PROGRAM_NAME ": too many backups for '%s'\n", path);
        return -1;
    }
    return 0;
}

/* Forward declaration */
static int copy_recursive(const char *src, const char *dest);

/* Copy a regular file (fallback when rename fails across devices). */
static int copy_file(const char *src, const char *dest) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        fprintf(stderr, PROGRAM_NAME ": cannot open '%s': %d\n", src, errno);
        return -1;
    }

    int dfd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dfd < 0) {
        fprintf(stderr, PROGRAM_NAME ": cannot create '%s': %d\n", dest, errno);
        close(sfd);
        return -1;
    }

    char buf[COPY_BUF_SIZE];
    ssize_t nread;
    int ret = 0;

    while ((nread = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < nread) {
            ssize_t w = write(dfd, buf + written, (size_t)(nread - written));
            if (w < 0) {
                fprintf(stderr, PROGRAM_NAME ": error writing '%s': %d\n",
                        dest, errno);
                ret = -1;
                goto done;
            }
            written += w;
        }
    }

    if (nread < 0) {
        fprintf(stderr, PROGRAM_NAME ": error reading '%s': %d\n", src, errno);
        ret = -1;
    }

done:
    close(sfd);
    close(dfd);

    /* Try to preserve mode/ownership */
    if (ret == 0) {
        struct stat st;
        if (stat(src, &st) == 0) {
            chmod(dest, st.st_mode & 07777);
            chown(dest, (int)st.st_uid, (int)st.st_gid);
        }
    }

    return ret;
}

/* Remove a directory tree recursively (for cross-device move cleanup). */
static int remove_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            remove_recursive(child);
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

/* Copy directory recursively (for cross-device move). */
static int copy_recursive(const char *src, const char *dest) {
    struct stat st;
    if (stat(src, &st) < 0) {
        fprintf(stderr, PROGRAM_NAME ": cannot stat '%s': %d\n", src, errno);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dest, st.st_mode & 07777) < 0 && errno != EEXIST) {
            fprintf(stderr, PROGRAM_NAME ": cannot create directory '%s': %d\n",
                    dest, errno);
            return -1;
        }

        DIR *d = opendir(src);
        if (!d) {
            fprintf(stderr, PROGRAM_NAME ": cannot open directory '%s': %d\n",
                    src, errno);
            return -1;
        }

        int errors = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char src_child[4096], dst_child[4096];
            snprintf(src_child, sizeof(src_child), "%s/%s", src, ent->d_name);
            snprintf(dst_child, sizeof(dst_child), "%s/%s", dest, ent->d_name);
            if (copy_recursive(src_child, dst_child) < 0)
                errors++;
        }
        closedir(d);
        return errors > 0 ? -1 : 0;
    }

    return copy_file(src, dest);
}

/* Move a single source to a single destination. */
static int do_move(const char *src, const char *dest) {
    struct stat dst_st;
    int dest_exists = (lstat(dest, &dst_st) == 0);

    /* Check update mode */
    if (dest_exists) {
        if (opt_update == UPDATE_NONE)
            return 0;
        if (opt_update == UPDATE_OLDER) {
            struct stat src_st;
            if (stat(src, &src_st) == 0) {
                if (src_st.st_mtime <= dst_st.st_mtime)
                    return 0;
            }
        }
    }

    /* Check interactive mode */
    if (dest_exists) {
        if (opt_interactive == INTER_NO_CLOBBER)
            return 0;
        if (opt_interactive == INTER_INTERACTIVE) {
            char buf[16];
            fprintf(stderr, PROGRAM_NAME ": overwrite '%s'? ", dest);
            if (fgets(buf, sizeof(buf), stdin) == NULL ||
                (buf[0] != 'y' && buf[0] != 'Y'))
                return 0;
        }
    }

    /* Make backup if requested */
    if (dest_exists && opt_backup != BACKUP_NONE) {
        if (make_backup(dest) < 0)
            return -1;
    }

    /* Try rename first (same filesystem, fast path) */
    if (rename(src, dest) == 0) {
        if (opt_verbose || opt_debug)
            printf("renamed '%s' -> '%s'\n", src, dest);
        return 0;
    }

    /* If rename failed with EXDEV (cross-device), fall back to copy + remove */
    if (errno == EXDEV) {
        if (opt_no_copy) {
            fprintf(stderr, PROGRAM_NAME ": cannot move '%s' to '%s': cross-device move "
                    "and --no-copy specified\n", src, dest);
            return -1;
        }

        if (opt_debug)
            printf("  rename failed (cross-device), falling back to copy+remove\n");

        struct stat src_st;
        if (stat(src, &src_st) < 0) {
            fprintf(stderr, PROGRAM_NAME ": cannot stat '%s': %d\n", src, errno);
            return -1;
        }

        int ret;
        if (S_ISDIR(src_st.st_mode)) {
            ret = copy_recursive(src, dest);
        } else {
            ret = copy_file(src, dest);
        }

        if (ret < 0) {
            fprintf(stderr, PROGRAM_NAME ": failed to copy '%s' to '%s'\n",
                    src, dest);
            return -1;
        }

        /* Remove source after successful copy */
        if (S_ISDIR(src_st.st_mode))
            ret = remove_recursive(src);
        else
            ret = unlink(src);

        if (ret < 0) {
            fprintf(stderr, PROGRAM_NAME ": warning: copied '%s' to '%s' "
                    "but failed to remove source\n", src, dest);
        }

        if (opt_verbose || opt_debug)
            printf("'%s' -> '%s'\n", src, dest);
        return 0;
    }

    /* rename failed for another reason */
    fprintf(stderr, PROGRAM_NAME ": cannot move '%s' to '%s': %d\n",
            src, dest, errno);
    return -1;
}

int main(int argc, char **argv) {
    enum {
        OPT_BACKUP = 256,
        OPT_DEBUG,
        OPT_NO_COPY,
        OPT_STRIP_TRAILING_SLASHES,
        OPT_UPDATE,
        OPT_HELP,
        OPT_VERSION,
    };

    static const struct option long_options[] = {
        {"backup",                 optional_argument, NULL, OPT_BACKUP},
        {"debug",                  no_argument,       NULL, OPT_DEBUG},
        {"force",                  no_argument,       NULL, 'f'},
        {"interactive",            no_argument,       NULL, 'i'},
        {"no-clobber",             no_argument,       NULL, 'n'},
        {"no-copy",                no_argument,       NULL, OPT_NO_COPY},
        {"no-target-directory",    no_argument,       NULL, 'T'},
        {"strip-trailing-slashes", no_argument,       NULL, OPT_STRIP_TRAILING_SLASHES},
        {"suffix",                 required_argument, NULL, 'S'},
        {"target-directory",       required_argument, NULL, 't'},
        {"update",                 optional_argument, NULL, OPT_UPDATE},
        {"verbose",                no_argument,       NULL, 'v'},
        {"help",                   no_argument,       NULL, OPT_HELP},
        {"version",                no_argument,       NULL, OPT_VERSION},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "bfinS:t:TuvZ", long_options, NULL)) != -1) {
        switch (c) {
        case 'b':
            opt_backup = BACKUP_SIMPLE;
            break;
        case OPT_BACKUP:
            if (!optarg || strcmp(optarg, "existing") == 0 || strcmp(optarg, "nil") == 0)
                opt_backup = BACKUP_EXISTING;
            else if (strcmp(optarg, "numbered") == 0 || strcmp(optarg, "t") == 0)
                opt_backup = BACKUP_NUMBERED;
            else if (strcmp(optarg, "simple") == 0 || strcmp(optarg, "never") == 0)
                opt_backup = BACKUP_SIMPLE;
            else if (strcmp(optarg, "none") == 0 || strcmp(optarg, "off") == 0)
                opt_backup = BACKUP_NONE;
            else {
                fprintf(stderr, PROGRAM_NAME ": invalid backup type '%s'\n", optarg);
                return 1;
            }
            break;
        case OPT_DEBUG:
            opt_debug = 1;
            opt_verbose = 1;
            break;
        case 'f':
            opt_interactive = INTER_FORCE;
            break;
        case 'i':
            opt_interactive = INTER_INTERACTIVE;
            break;
        case 'n':
            opt_interactive = INTER_NO_CLOBBER;
            break;
        case OPT_NO_COPY:
            opt_no_copy = 1;
            break;
        case 'S':
            opt_suffix = optarg;
            break;
        case 't':
            opt_target_directory = optarg;
            break;
        case 'T':
            opt_no_target_directory = 1;
            break;
        case 'u':
            opt_update = UPDATE_OLDER;
            break;
        case OPT_UPDATE:
            if (!optarg || strcmp(optarg, "older") == 0)
                opt_update = UPDATE_OLDER;
            else if (strcmp(optarg, "all") == 0)
                opt_update = UPDATE_ALL;
            else if (strcmp(optarg, "none") == 0)
                opt_update = UPDATE_NONE;
            else {
                fprintf(stderr, PROGRAM_NAME ": invalid update type '%s'\n", optarg);
                return 1;
            }
            break;
        case 'v':
            opt_verbose = 1;
            break;
        case 'Z':
            /* SELinux: not supported, silently ignore */
            break;
        case OPT_STRIP_TRAILING_SLASHES:
            opt_strip_trailing_slashes = 1;
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

    int nargs = argc - optind;
    if (nargs < 1) {
        fprintf(stderr, PROGRAM_NAME ": missing file operand\n");
        usage();
        return 1;
    }

    /* Strip trailing slashes if requested */
    if (opt_strip_trailing_slashes) {
        for (int i = optind; i < argc; i++)
            strip_trailing_slashes(argv[i]);
    }

    int errors = 0;

    if (opt_target_directory) {
        /* -t DIRECTORY SOURCE... */
        for (int i = optind; i < argc; i++) {
            char dest[4096];
            build_dest_path(dest, sizeof(dest), opt_target_directory, argv[i]);
            if (do_move(argv[i], dest) < 0)
                errors++;
        }
    } else if (opt_no_target_directory || nargs == 2) {
        if (nargs < 2) {
            fprintf(stderr, PROGRAM_NAME ": missing destination file operand after '%s'\n",
                    argv[optind]);
            return 1;
        }

        const char *src = argv[optind];
        const char *dest = argv[optind + 1];

        if (!opt_no_target_directory && nargs == 2) {
            struct stat dst_st;
            if (stat(dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
                char full_dest[4096];
                build_dest_path(full_dest, sizeof(full_dest), dest, src);
                if (do_move(src, full_dest) < 0)
                    errors++;
                return errors > 0 ? 1 : 0;
            }
        }

        if (do_move(src, dest) < 0)
            errors++;
    } else {
        /* Multiple sources: last arg must be directory */
        const char *dest = argv[argc - 1];
        struct stat dst_st;
        if (stat(dest, &dst_st) < 0 || !S_ISDIR(dst_st.st_mode)) {
            fprintf(stderr, PROGRAM_NAME ": target '%s' is not a directory\n", dest);
            return 1;
        }
        for (int i = optind; i < argc - 1; i++) {
            char full_dest[4096];
            build_dest_path(full_dest, sizeof(full_dest), dest, argv[i]);
            if (do_move(argv[i], full_dest) < 0)
                errors++;
        }
    }

    return errors > 0 ? 1 : 0;
}
