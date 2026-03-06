/*
 * cp - copy files and directories
 *
 * Full implementation per cp(1) manpage.
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
#define PROGRAM_NAME "cp"
#define COPY_BUF_SIZE 32768

/* Deref mode for symlinks */
enum deref_mode {
    DEREF_UNDEFINED = 0,
    DEREF_NEVER,        /* -P, --no-dereference */
    DEREF_ALWAYS,       /* -L, --dereference */
    DEREF_COMMAND_LINE, /* -H: follow only command-line symlinks */
};

/* Update mode */
enum update_mode {
    UPDATE_ALL = 0,     /* default: always overwrite */
    UPDATE_NONE,        /* --update=none / -n */
    UPDATE_OLDER,       /* --update=older / -u */
};

/* Backup type */
enum backup_type {
    BACKUP_NONE = 0,
    BACKUP_SIMPLE,
    BACKUP_NUMBERED,
    BACKUP_EXISTING,
};

/* Preserve flags */
#define PRESERVE_MODE       0x01
#define PRESERVE_OWNERSHIP  0x02
#define PRESERVE_TIMESTAMPS 0x04
#define PRESERVE_LINKS      0x08
#define PRESERVE_CONTEXT    0x10
#define PRESERVE_XATTR      0x20
#define PRESERVE_ALL        0x3F

static int opt_recursive = 0;
static int opt_force = 0;
static int opt_interactive = 0;
static int opt_verbose = 0;
static int opt_debug = 0;
static int opt_no_target_directory = 0;
static char *opt_target_directory = NULL;
static int opt_strip_trailing_slashes = 0;
static int opt_remove_destination = 0;
static int opt_one_file_system = 0;
static int opt_attributes_only = 0;
static int opt_copy_contents = 0;
static int opt_parents = 0;
static int opt_make_link = 0;       /* -l: hard link */
static int opt_make_symlink = 0;    /* -s: symbolic link */
static enum deref_mode opt_deref = DEREF_UNDEFINED;
static enum update_mode opt_update = UPDATE_ALL;
static enum backup_type opt_backup = BACKUP_NONE;
static int opt_preserve = 0;       /* bitmask of PRESERVE_* */
static int opt_no_preserve = 0;
static char *opt_suffix = NULL;

static void usage(void) {
    fprintf(stderr,
        "Usage: " PROGRAM_NAME " [OPTION]... [-T] SOURCE DEST\n"
        "  or:  " PROGRAM_NAME " [OPTION]... SOURCE... DIRECTORY\n"
        "  or:  " PROGRAM_NAME " [OPTION]... -t DIRECTORY SOURCE...\n"
        "Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.\n"
        "\n"
        "  -a, --archive             same as -dR --preserve=all\n"
        "      --attributes-only     don't copy the file data, just the attributes\n"
        "      --backup[=CONTROL]    make a backup of each existing destination file\n"
        "  -b                        like --backup but does not accept an argument\n"
        "      --copy-contents       copy contents of special files when recursive\n"
        "  -d                        same as --no-dereference --preserve=links\n"
        "      --debug               explain how a file is copied.  Implies -v\n"
        "  -f, --force               if an existing destination file cannot be opened,\n"
        "                            remove it and try again\n"
        "  -i, --interactive         prompt before overwrite\n"
        "  -H                        follow command-line symbolic links in SOURCE\n"
        "  -l, --link                hard link files instead of copying\n"
        "  -L, --dereference         always follow symbolic links in SOURCE\n"
        "  -n, --no-clobber          do not overwrite an existing file\n"
        "  -P, --no-dereference      never follow symbolic links in SOURCE\n"
        "  -p                        same as --preserve=mode,ownership,timestamps\n"
        "      --preserve[=ATTR_LIST]  preserve the specified attributes\n"
        "      --no-preserve=ATTR_LIST  don't preserve the specified attributes\n"
        "      --parents             use full source file name under DIRECTORY\n"
        "  -R, -r, --recursive       copy directories recursively\n"
        "      --remove-destination  remove each existing destination file before\n"
        "                            attempting to open it\n"
        "      --strip-trailing-slashes  remove any trailing slashes from each\n"
        "                            SOURCE argument\n"
        "  -s, --symbolic-link       make symbolic links instead of copying\n"
        "  -S, --suffix=SUFFIX       override the usual backup suffix\n"
        "  -t, --target-directory=DIRECTORY  copy all SOURCE arguments into DIRECTORY\n"
        "  -T, --no-target-directory  treat DEST as a normal file\n"
        "      --update[=UPDATE]     control which existing files are updated\n"
        "  -u                        equivalent to --update[=older]\n"
        "  -v, --verbose             explain what is being done\n"
        "  -x, --one-file-system     stay on this file system\n"
        "      --help                display this help and exit\n"
        "      --version             output version information and exit\n");
}

static void version(void) {
    printf(PROGRAM_NAME " " VERSION "\n");
}

/* Parse a preserve attribute list like "mode,ownership,timestamps" */
static int parse_preserve(const char *list) {
    if (!list || !*list)
        return PRESERVE_MODE | PRESERVE_OWNERSHIP | PRESERVE_TIMESTAMPS;

    int flags = 0;
    char buf[256];
    size_t len = strlen(list);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, list, len + 1);

    char *tok = buf;
    while (*tok) {
        char *end = tok;
        while (*end && *end != ',') end++;
        int last = (*end == '\0');
        *end = '\0';

        if (strcmp(tok, "mode") == 0)
            flags |= PRESERVE_MODE;
        else if (strcmp(tok, "ownership") == 0)
            flags |= PRESERVE_OWNERSHIP;
        else if (strcmp(tok, "timestamps") == 0)
            flags |= PRESERVE_TIMESTAMPS;
        else if (strcmp(tok, "links") == 0)
            flags |= PRESERVE_LINKS;
        else if (strcmp(tok, "context") == 0)
            flags |= PRESERVE_CONTEXT;
        else if (strcmp(tok, "xattr") == 0)
            flags |= PRESERVE_XATTR;
        else if (strcmp(tok, "all") == 0)
            flags |= PRESERVE_ALL;
        else {
            fprintf(stderr, PROGRAM_NAME ": unrecognized attribute: '%s'\n", tok);
            return -1;
        }

        if (last) break;
        tok = end + 1;
    }
    return flags;
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
    const char *end = p;
    while (p > path && *(p - 1) != '/')
        p--;
    (void)end;
    return p;
}

/* Build a destination path: dir/basename(src) */
static void build_dest_path(char *dest, size_t dest_size,
                            const char *dir, const char *src) {
    const char *base = my_basename(src);
    size_t dlen = strlen(dir);
    size_t blen = strlen(base);
    if (dlen + 1 + blen >= dest_size) {
        /* truncate if too long */
        snprintf(dest, dest_size, "%s/%s", dir, base);
        return;
    }
    memcpy(dest, dir, dlen);
    if (dlen > 0 && dir[dlen - 1] != '/')
        dest[dlen++] = '/';
    memcpy(dest + dlen, base, blen + 1);
}

/* Create a backup of an existing file if requested. */
static int make_backup(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0)
        return 0; /* No file to backup */

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

/* Apply preserved attributes after copy. */
static void apply_attributes(const char *dest, const struct stat *src_st) {
    if (opt_preserve & PRESERVE_MODE)
        chmod(dest, src_st->st_mode & 07777);
    if (opt_preserve & PRESERVE_OWNERSHIP)
        chown(dest, (int)src_st->st_uid, (int)src_st->st_gid);
    if (opt_preserve & PRESERVE_TIMESTAMPS) {
        struct timespec times[2];
        times[0].tv_sec = (long)src_st->st_atime;
        times[0].tv_nsec = 0;
        times[1].tv_sec = (long)src_st->st_mtime;
        times[1].tv_nsec = 0;
        utimensat(-100, dest, times, 0);
    }
}

/* Copy a single regular file. */
static int copy_file_data(const char *src, const char *dest,
                          const struct stat *src_st) {
    if (opt_attributes_only) {
        /* Create the file but don't copy data */
        int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            fprintf(stderr, PROGRAM_NAME ": cannot create '%s': %d\n", dest, errno);
            return -1;
        }
        close(fd);
        apply_attributes(dest, src_st);
        return 0;
    }

    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        fprintf(stderr, PROGRAM_NAME ": cannot open '%s' for reading: %d\n",
                src, errno);
        return -1;
    }

    int dfd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dfd < 0) {
        if (opt_force) {
            /* Try removing destination and retrying */
            unlink(dest);
            dfd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        }
        if (dfd < 0) {
            fprintf(stderr, PROGRAM_NAME ": cannot create '%s': %d\n",
                    dest, errno);
            close(sfd);
            return -1;
        }
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
        fprintf(stderr, PROGRAM_NAME ": error reading '%s': %d\n",
                src, errno);
        ret = -1;
    }

done:
    close(sfd);
    close(dfd);

    if (ret == 0)
        apply_attributes(dest, src_st);

    return ret;
}

/* Forward declaration */
static int do_copy(const char *src, const char *dest, int is_cmdline_arg);

/* Copy directory recursively. */
static int copy_directory(const char *src, const char *dest,
                          const struct stat *src_st) {
    /* Create destination directory */
    struct stat dst_st;
    if (stat(dest, &dst_st) < 0) {
        if (mkdir(dest, src_st->st_mode & 07777) < 0) {
            fprintf(stderr, PROGRAM_NAME ": cannot create directory '%s': %d\n",
                    dest, errno);
            return -1;
        }
        if (opt_verbose || opt_debug)
            printf("created directory '%s'\n", dest);
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

        if (do_copy(src_child, dst_child, 0) < 0)
            errors++;
    }
    closedir(d);

    /* Apply attributes to the directory */
    apply_attributes(dest, src_st);

    return errors > 0 ? -1 : 0;
}

/* Copy a single source to a single destination.
 * is_cmdline_arg is true for top-level invocations (affects -H deref). */
static int do_copy(const char *src, const char *dest, int is_cmdline_arg) {
    struct stat src_st;

    /* Determine whether to follow symlinks based on deref mode */
    int follow_links = 0;
    if (opt_deref == DEREF_ALWAYS)
        follow_links = 1;
    else if (opt_deref == DEREF_COMMAND_LINE && is_cmdline_arg)
        follow_links = 1;
    else if (opt_deref == DEREF_UNDEFINED)
        follow_links = !opt_recursive; /* Default: follow unless recursive */

    if (follow_links) {
        if (stat(src, &src_st) < 0) {
            fprintf(stderr, PROGRAM_NAME ": cannot stat '%s': %d\n", src, errno);
            return -1;
        }
    } else {
        if (lstat(src, &src_st) < 0) {
            fprintf(stderr, PROGRAM_NAME ": cannot stat '%s': %d\n", src, errno);
            return -1;
        }
    }

    /* Check if destination exists */
    struct stat dst_st;
    int dest_exists = (lstat(dest, &dst_st) == 0);

    /* Handle update modes */
    if (dest_exists) {
        if (opt_update == UPDATE_NONE) {
            /* Don't overwrite, don't report error */
            return 0;
        }
        if (opt_update == UPDATE_OLDER) {
            /* Only overwrite if src is newer */
            if (src_st.st_mtime <= dst_st.st_mtime)
                return 0;
        }
    }

    /* Interactive mode: prompt before overwrite */
    if (dest_exists && opt_interactive) {
        char buf[16];
        fprintf(stderr, PROGRAM_NAME ": overwrite '%s'? ", dest);
        if (fgets(buf, sizeof(buf), stdin) == NULL || (buf[0] != 'y' && buf[0] != 'Y'))
            return 0;
    }

    /* Make backup if requested */
    if (dest_exists && opt_backup != BACKUP_NONE) {
        if (make_backup(dest) < 0)
            return -1;
    }

    /* Remove destination if requested */
    if (dest_exists && opt_remove_destination) {
        if (S_ISDIR(dst_st.st_mode))
            rmdir(dest);
        else
            unlink(dest);
    }

    /* Handle directory */
    if (S_ISDIR(src_st.st_mode)) {
        if (!opt_recursive) {
            fprintf(stderr, PROGRAM_NAME ": -r not specified; omitting directory '%s'\n", src);
            return -1;
        }
        return copy_directory(src, dest, &src_st);
    }

    /* Handle regular file */
    if (S_ISREG(src_st.st_mode)) {
        /* Hard link instead of copy */
        if (opt_make_link) {
            if (link(src, dest) < 0) {
                if (errno == EEXIST && (opt_force || opt_remove_destination)) {
                    unlink(dest);
                    if (link(src, dest) < 0) {
                        fprintf(stderr, PROGRAM_NAME ": cannot create hard link '%s': %d\n",
                                dest, errno);
                        return -1;
                    }
                } else {
                    fprintf(stderr, PROGRAM_NAME ": cannot create hard link '%s': %d\n",
                            dest, errno);
                    return -1;
                }
            }
            if (opt_verbose || opt_debug)
                printf("'%s' => '%s'\n", dest, src);
            return 0;
        }

        /* Symbolic link instead of copy */
        if (opt_make_symlink) {
            if (symlink(src, dest) < 0) {
                if (errno == EEXIST && (opt_force || opt_remove_destination)) {
                    unlink(dest);
                    if (symlink(src, dest) < 0) {
                        fprintf(stderr, PROGRAM_NAME ": cannot create symbolic link '%s': %d\n",
                                dest, errno);
                        return -1;
                    }
                } else {
                    fprintf(stderr, PROGRAM_NAME ": cannot create symbolic link '%s': %d\n",
                            dest, errno);
                    return -1;
                }
            }
            if (opt_verbose || opt_debug)
                printf("'%s' -> '%s'\n", dest, src);
            return 0;
        }

        /* Normal copy */
        if (opt_verbose || opt_debug)
            printf("'%s' -> '%s'\n", src, dest);
        return copy_file_data(src, dest, &src_st);
    }

    /* Other file types: try copying as regular file */
    if (opt_verbose || opt_debug)
        printf("'%s' -> '%s'\n", src, dest);
    return copy_file_data(src, dest, &src_st);
}

int main(int argc, char **argv) {
    enum {
        OPT_ATTRIBUTES_ONLY = 256,
        OPT_BACKUP,
        OPT_COPY_CONTENTS,
        OPT_DEBUG,
        OPT_PARENTS,
        OPT_PRESERVE,
        OPT_NO_PRESERVE,
        OPT_REFLINK,
        OPT_REMOVE_DESTINATION,
        OPT_SPARSE,
        OPT_STRIP_TRAILING_SLASHES,
        OPT_UPDATE,
        OPT_HELP,
        OPT_VERSION,
    };

    static const struct option long_options[] = {
        {"archive",                no_argument,       NULL, 'a'},
        {"attributes-only",        no_argument,       NULL, OPT_ATTRIBUTES_ONLY},
        {"backup",                 optional_argument, NULL, OPT_BACKUP},
        {"copy-contents",          no_argument,       NULL, OPT_COPY_CONTENTS},
        {"debug",                  no_argument,       NULL, OPT_DEBUG},
        {"dereference",            no_argument,       NULL, 'L'},
        {"force",                  no_argument,       NULL, 'f'},
        {"interactive",            no_argument,       NULL, 'i'},
        {"link",                   no_argument,       NULL, 'l'},
        {"no-clobber",             no_argument,       NULL, 'n'},
        {"no-dereference",         no_argument,       NULL, 'P'},
        {"no-target-directory",    no_argument,       NULL, 'T'},
        {"one-file-system",        no_argument,       NULL, 'x'},
        {"parents",                no_argument,       NULL, OPT_PARENTS},
        {"preserve",               optional_argument, NULL, OPT_PRESERVE},
        {"no-preserve",            required_argument, NULL, OPT_NO_PRESERVE},
        {"recursive",              no_argument,       NULL, 'R'},
        {"reflink",                optional_argument, NULL, OPT_REFLINK},
        {"remove-destination",     no_argument,       NULL, OPT_REMOVE_DESTINATION},
        {"sparse",                 required_argument, NULL, OPT_SPARSE},
        {"strip-trailing-slashes", no_argument,       NULL, OPT_STRIP_TRAILING_SLASHES},
        {"suffix",                 required_argument, NULL, 'S'},
        {"symbolic-link",          no_argument,       NULL, 's'},
        {"target-directory",       required_argument, NULL, 't'},
        {"update",                 optional_argument, NULL, OPT_UPDATE},
        {"verbose",                no_argument,       NULL, 'v'},
        {"help",                   no_argument,       NULL, OPT_HELP},
        {"version",                no_argument,       NULL, OPT_VERSION},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "abdfilnprsuvxHLPRS:Tt:", long_options, NULL)) != -1) {
        switch (c) {
        case 'a': /* --archive: -dR --preserve=all */
            opt_deref = DEREF_NEVER;
            opt_preserve |= PRESERVE_LINKS;
            opt_recursive = 1;
            opt_preserve |= PRESERVE_ALL;
            break;
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
        case 'd': /* same as --no-dereference --preserve=links */
            opt_deref = DEREF_NEVER;
            opt_preserve |= PRESERVE_LINKS;
            break;
        case OPT_DEBUG:
            opt_debug = 1;
            opt_verbose = 1;
            break;
        case 'f':
            opt_force = 1;
            opt_interactive = 0;
            break;
        case 'i':
            opt_interactive = 1;
            opt_force = 0;
            break;
        case 'l':
            opt_make_link = 1;
            break;
        case 'n':
            opt_update = UPDATE_NONE;
            opt_interactive = 0;
            break;
        case 'p': /* same as --preserve=mode,ownership,timestamps */
            opt_preserve |= PRESERVE_MODE | PRESERVE_OWNERSHIP | PRESERVE_TIMESTAMPS;
            break;
        case OPT_PRESERVE: {
            int p = parse_preserve(optarg);
            if (p < 0) return 1;
            opt_preserve |= p;
            break;
        }
        case OPT_NO_PRESERVE: {
            int p = parse_preserve(optarg);
            if (p < 0) return 1;
            opt_no_preserve |= p;
            break;
        }
        case 'r':
        case 'R':
            opt_recursive = 1;
            break;
        case 's':
            opt_make_symlink = 1;
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
        case 'x':
            opt_one_file_system = 1;
            break;
        case 'H':
            opt_deref = DEREF_COMMAND_LINE;
            break;
        case 'L':
            opt_deref = DEREF_ALWAYS;
            break;
        case 'P':
            opt_deref = DEREF_NEVER;
            break;
        case OPT_ATTRIBUTES_ONLY:
            opt_attributes_only = 1;
            break;
        case OPT_COPY_CONTENTS:
            opt_copy_contents = 1;
            break;
        case OPT_PARENTS:
            opt_parents = 1;
            break;
        case OPT_REFLINK:
            /* reflink not supported on this filesystem, silently accept */
            break;
        case OPT_REMOVE_DESTINATION:
            opt_remove_destination = 1;
            break;
        case OPT_SPARSE:
            /* sparse not supported, silently accept */
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

    /* Remove no_preserve flags from preserve */
    opt_preserve &= ~opt_no_preserve;

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
            if (do_copy(argv[i], dest, 1) < 0)
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
            /* Check if dest is an existing directory */
            struct stat dst_st;
            if (stat(dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
                char full_dest[4096];
                build_dest_path(full_dest, sizeof(full_dest), dest, src);
                if (do_copy(src, full_dest, 1) < 0)
                    errors++;
                return errors > 0 ? 1 : 0;
            }
        }

        if (do_copy(src, dest, 1) < 0)
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
            if (do_copy(argv[i], full_dest, 1) < 0)
                errors++;
        }
    }

    return errors > 0 ? 1 : 0;
}
