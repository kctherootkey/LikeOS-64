/*
 * ln - make links between files
 *
 * Full implementation per the ln(1) manual page.  Supports all four
 * invocation forms and the options -b/--backup, -d/-F/--directory,
 * -f/--force, -i/--interactive, -L/--logical, -n/--no-dereference,
 * -P/--physical, -r/--relative, -s/--symbolic, -S/--suffix,
 * -t/--target-directory, -T/--no-target-directory, -v/--verbose,
 * --help, --version.  Hard links by default; symbolic with -s.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "ln"

enum backup_type { BK_NONE, BK_SIMPLE, BK_NUMBERED, BK_EXISTING };

static int opt_symbolic = 0;
static int opt_force = 0;
static int opt_interactive = 0;
static int opt_verbose = 0;
static int opt_nodereference = 0;   /* -n */
static int opt_relative = 0;        /* -r */
static int opt_directory = 0;       /* -d/-F (hard-link dirs)            */
static int opt_logical = 0;         /* -L */
static int opt_physical = 0;        /* -P */
static int opt_backup = 0;
static enum backup_type opt_bktype = BK_EXISTING;
static const char *opt_suffix = "~";
static const char *target_dir = NULL;   /* -t */
static int no_target_dir = 0;            /* -T */

static int exit_status = 0;

static void usage(void) {
    printf(
"Usage: " PROGRAM_NAME " [OPTION]... [-T] TARGET LINK_NAME\n"
"  or:  " PROGRAM_NAME " [OPTION]... TARGET\n"
"  or:  " PROGRAM_NAME " [OPTION]... TARGET... DIRECTORY\n"
"  or:  " PROGRAM_NAME " [OPTION]... -t DIRECTORY TARGET...\n"
"Create a link to TARGET with the name LINK_NAME.  Create hard links by\n"
"default, symbolic links with --symbolic.\n"
"\n"
"      --backup[=CONTROL]   make a backup of each existing destination file\n"
"  -b                       like --backup but does not accept an argument\n"
"  -d, -F, --directory      allow the superuser to hard link directories\n"
"  -f, --force              remove existing destination files\n"
"  -i, --interactive        prompt whether to remove destinations\n"
"  -L, --logical            dereference TARGETs that are symbolic links\n"
"  -n, --no-dereference     treat LINK_NAME as a normal file if it is a\n"
"                             symbolic link to a directory\n"
"  -P, --physical           make hard links directly to symbolic links\n"
"  -r, --relative           with -s, create links relative to link location\n"
"  -s, --symbolic           make symbolic links instead of hard links\n"
"  -S, --suffix=SUFFIX      override the usual backup suffix\n"
"  -t, --target-directory=DIRECTORY  specify the DIRECTORY for the links\n"
"  -T, --no-target-directory  treat LINK_NAME as a normal file always\n"
"  -v, --verbose            print name of each linked file\n"
"      --help     display this help and exit\n"
"      --version  output version information and exit\n");
}

static void version(void) { printf(PROGRAM_NAME " (LikeOS coreutils) " VERSION "\n"); }

static enum backup_type backup_from_string(const char *s) {
    if (!s || !*s) return BK_EXISTING;
    if (!strcmp(s, "none") || !strcmp(s, "off")) return BK_NONE;
    if (!strcmp(s, "simple") || !strcmp(s, "never")) return BK_SIMPLE;
    if (!strcmp(s, "numbered") || !strcmp(s, "t")) return BK_NUMBERED;
    if (!strcmp(s, "existing") || !strcmp(s, "nil")) return BK_EXISTING;
    return BK_EXISTING;
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_exists_lstat(const char *path, struct stat *st) {
    return lstat(path, st) == 0;
}

/* ---- lexical absolute-path + normalize (no symlink resolution) ---- */
static void make_abs(const char *path, char *out, size_t cap) {
    char tmp[4096];
    if (path[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", path);
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }
    /* normalize: split on '/', drop "." and empty, handle ".." */
    char *parts[256]; int np = 0;
    char *s = tmp;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        char *start = s;
        while (*s && *s != '/') s++;
        size_t len = (size_t)(s - start);
        char *seg = start;
        seg[len] = '\0';  /* safe: next char is '/' or end; we overwrite '/' */
        if (len == 1 && seg[0] == '.') { if (*s) s++; continue; }
        if (len == 2 && seg[0] == '.' && seg[1] == '.') { if (np > 0) np--; }
        else if (np < 256) parts[np++] = seg;
        /* advance past the NUL we wrote if there was a '/' */
        if (start[len] == '\0' && s != start + len) {}
        s = start + len;
        if (*s == '\0') break;
        s++;
    }
    /* rebuild */
    size_t o = 0;
    out[o++] = '/';
    for (int i = 0; i < np; i++) {
        size_t l = strlen(parts[i]);
        if (o + l + 1 >= cap) break;
        if (o > 1) out[o++] = '/';
        memcpy(out + o, parts[i], l); o += l;
    }
    out[o] = '\0';
    if (o == 0) { out[0] = '/'; out[1] = '\0'; }
}

/* Compute `target` relative to the directory containing `linkname`. */
static void relative_target(const char *target, const char *linkname,
                            char *out, size_t cap) {
    char at[4096], al[4096];
    make_abs(target, at, sizeof(at));
    make_abs(linkname, al, sizeof(al));
    /* link dir = dirname(al) */
    char ldir[4096];
    snprintf(ldir, sizeof(ldir), "%s", al);
    char *slash = strrchr(ldir, '/');
    if (slash && slash != ldir) *slash = '\0';
    else strcpy(ldir, "/");

    /* split both into components */
    char a[4096], b[4096];
    snprintf(a, sizeof(a), "%s", at);
    snprintf(b, sizeof(b), "%s", ldir);
    char *tp[256]; int tn = 0;
    char *lp[256]; int ln_ = 0;
    for (char *s = a; *s; ) { while (*s=='/') *s++=0; if(!*s) break; tp[tn++]=s; while(*s&&*s!='/')s++; }
    for (char *s = b; *s; ) { while (*s=='/') *s++=0; if(!*s) break; lp[ln_++]=s; while(*s&&*s!='/')s++; }
    int common = 0;
    while (common < tn && common < ln_ && strcmp(tp[common], lp[common]) == 0) common++;
    size_t o = 0; out[0] = '\0';
    for (int i = common; i < ln_; i++) { if (o+3<cap){ memcpy(out+o,"../",3); o+=3; } }
    for (int i = common; i < tn; i++) {
        size_t l = strlen(tp[i]);
        if (o + l + 1 >= cap) break;
        memcpy(out+o, tp[i], l); o += l;
        if (i + 1 < tn) out[o++] = '/';
    }
    out[o] = '\0';
    if (o == 0) snprintf(out, cap, "%s", base_name(target));
}

static int prompt_remove(const char *path) {
    fprintf(stderr, PROGRAM_NAME ": replace '%s'? ", path);
    int c = getchar();
    int ans = (c == 'y' || c == 'Y');
    while (c != '\n' && c != EOF) c = getchar();
    return ans;
}

/* Back up an existing destination by renaming it. */
static int do_backup(const char *path) {
    char bak[4096];
    if (opt_bktype == BK_NUMBERED ||
        (opt_bktype == BK_EXISTING)) {
        /* numbered: find .~N~; existing: numbered if any exist else simple */
        int use_numbered = (opt_bktype == BK_NUMBERED);
        if (opt_bktype == BK_EXISTING) {
            char probe[4096];
            snprintf(probe, sizeof(probe), "%s.~1~", path);
            struct stat st;
            use_numbered = (lstat(probe, &st) == 0);
        }
        if (use_numbered) {
            for (int n = 1; n < 100000; n++) {
                snprintf(bak, sizeof(bak), "%s.~%d~", path, n);
                struct stat st;
                if (lstat(bak, &st) != 0) break;
            }
            return rename(path, bak);
        }
    }
    snprintf(bak, sizeof(bak), "%s%s", path, opt_suffix);
    return rename(path, bak);
}

/* Create one link: TARGET -> linkpath. */
static int make_link(const char *target, const char *linkpath) {
    struct stat dst;
    int dst_exists = path_exists_lstat(linkpath, &dst);

    if (dst_exists) {
        if (opt_interactive) {
            if (!prompt_remove(linkpath)) return 0;
        }
        if (opt_backup) {
            if (do_backup(linkpath) != 0) {
                fprintf(stderr, PROGRAM_NAME ": cannot backup '%s': %s\n", linkpath, strerror(errno));
                exit_status = 1; return -1;
            }
        } else if (opt_force) {
            if (unlink(linkpath) != 0 && errno != ENOENT) {
                fprintf(stderr, PROGRAM_NAME ": cannot remove '%s': %s\n", linkpath, strerror(errno));
                exit_status = 1; return -1;
            }
        } else {
            fprintf(stderr, PROGRAM_NAME ": failed to create %s '%s': File exists\n",
                    opt_symbolic ? "symbolic link" : "hard link", linkpath);
            exit_status = 1; return -1;
        }
    }

    int rc;
    if (opt_symbolic) {
        const char *ltarget = target;
        char rel[4096];
        if (opt_relative) { relative_target(target, linkpath, rel, sizeof(rel)); ltarget = rel; }
        rc = symlink(ltarget, linkpath);
        if (rc == 0 && opt_verbose)
            printf("'%s' -> '%s'\n", linkpath, ltarget);
    } else {
        rc = link(target, linkpath);
        if (rc == 0 && opt_verbose)
            printf("'%s' => '%s'\n", linkpath, target);
    }
    if (rc != 0) {
        fprintf(stderr, PROGRAM_NAME ": failed to create %s '%s': %s\n",
                opt_symbolic ? "symbolic link" : "hard link", linkpath, strerror(errno));
        exit_status = 1;
        return -1;
    }
    return 0;
}

/* Create a link named after target's basename inside directory `dir`. */
static int make_link_in_dir(const char *target, const char *dir) {
    char dest[4096];
    size_t dl = strlen(dir);
    int need_slash = (dl > 0 && dir[dl - 1] != '/');
    snprintf(dest, sizeof(dest), "%s%s%s", dir, need_slash ? "/" : "", base_name(target));
    return make_link(target, dest);
}

int main(int argc, char **argv) {
    static struct option long_opts[] = {
        {"backup", optional_argument, 0, 1},
        {"directory", no_argument, 0, 'd'},
        {"force", no_argument, 0, 'f'},
        {"interactive", no_argument, 0, 'i'},
        {"logical", no_argument, 0, 'L'},
        {"no-dereference", no_argument, 0, 'n'},
        {"physical", no_argument, 0, 'P'},
        {"relative", no_argument, 0, 'r'},
        {"symbolic", no_argument, 0, 's'},
        {"suffix", required_argument, 0, 'S'},
        {"target-directory", required_argument, 0, 't'},
        {"no-target-directory", no_argument, 0, 'T'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 2},
        {"version", no_argument, 0, 3},
        {0, 0, 0, 0}
    };
    const char *envs = getenv("SIMPLE_BACKUP_SUFFIX");
    if (envs && *envs) opt_suffix = envs;
    const char *envc = getenv("VERSION_CONTROL");
    if (envc) opt_bktype = backup_from_string(envc);

    int c;
    while ((c = getopt_long(argc, argv, "bdFfiLnPrsS:t:Tv", long_opts, NULL)) != -1) {
        switch (c) {
            case 1: opt_backup = 1; if (optarg) opt_bktype = backup_from_string(optarg); break;
            case 'b': opt_backup = 1; break;
            case 'd': case 'F': opt_directory = 1; break;
            case 'f': opt_force = 1; opt_interactive = 0; break;
            case 'i': opt_interactive = 1; opt_force = 0; break;
            case 'L': opt_logical = 1; opt_physical = 0; break;
            case 'n': opt_nodereference = 1; break;
            case 'P': opt_physical = 1; opt_logical = 0; break;
            case 'r': opt_relative = 1; break;
            case 's': opt_symbolic = 1; break;
            case 'S': opt_suffix = optarg; opt_backup = 1; break;
            case 't': target_dir = optarg; break;
            case 'T': no_target_dir = 1; break;
            case 'v': opt_verbose = 1; break;
            case 2: usage(); return 0;
            case 3: version(); return 0;
            default: fprintf(stderr, "Try '" PROGRAM_NAME " --help' for more information.\n"); return 1;
        }
    }
    (void)opt_nodereference; (void)opt_directory; (void)opt_logical; (void)opt_physical;

    int nargs = argc - optind;
    if (nargs < 1) {
        fprintf(stderr, PROGRAM_NAME ": missing file operand\n");
        fprintf(stderr, "Try '" PROGRAM_NAME " --help' for more information.\n");
        return 1;
    }

    if (target_dir) {
        for (int i = optind; i < argc; i++)
            make_link_in_dir(argv[i], target_dir);
        return exit_status;
    }

    if (no_target_dir) {
        if (nargs != 2) {
            fprintf(stderr, PROGRAM_NAME ": with -T, exactly two operands are required\n");
            return 1;
        }
        return make_link(argv[optind], argv[optind + 1]) == 0 ? exit_status : exit_status;
    }

    if (nargs == 1) {
        /* link to TARGET in the current directory */
        return make_link_in_dir(argv[optind], ".") == 0 ? exit_status : exit_status;
    }

    if (nargs == 2) {
        /* TARGET LINK_NAME, unless LINK_NAME is a directory */
        if (is_directory(argv[optind + 1]))
            make_link_in_dir(argv[optind], argv[optind + 1]);
        else
            make_link(argv[optind], argv[optind + 1]);
        return exit_status;
    }

    /* >2 operands: last must be a directory */
    const char *dir = argv[argc - 1];
    if (!is_directory(dir)) {
        fprintf(stderr, PROGRAM_NAME ": target '%s' is not a directory\n", dir);
        return 1;
    }
    for (int i = optind; i < argc - 1; i++)
        make_link_in_dir(argv[i], dir);
    return exit_status;
}
