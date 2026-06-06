/*
 * du - estimate file space usage
 *
 * Usage: du [OPTION]... [FILE]...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fnmatch.h>

/* Options */
static int opt_all = 0;            /* -a: write counts for all files */
static int opt_apparent_size = 0;  /* --apparent-size / -b implies this */
static int opt_bytes = 0;          /* -b: apparent size in bytes */
static unsigned long opt_block_size = 0; /* -B SIZE */
static int opt_total = 0;          /* -c: produce grand total */
static int opt_max_depth = -1;     /* -d N: max depth */
static int opt_human = 0;          /* -h: human-readable 1024 */
static int opt_si = 0;             /* --si: human-readable 1000 */
static int opt_deref_args = 0;     /* -D/-H: deref command line symlinks */
static int opt_deref = 0;          /* -L: deref all symlinks */
static int opt_kilo = 0;           /* -k: 1K blocks */
static int opt_mega = 0;           /* -m: 1M blocks */
static int opt_count_links = 0;    /* -l: count sizes many times if hard linked */
static int opt_separate_dirs = 0;  /* -S: for dirs, do not include subdirectories */
static int opt_summarize = 0;      /* -s: display only a total for each argument */
static long opt_threshold = 0;     /* -t SIZE: exclude entries smaller than SIZE */
static int opt_one_fs = 0;         /* -x: skip directories on different file systems */
static int opt_null = 0;           /* -0: end each output line with NUL */
static int opt_inodes = 0;         /* --inodes: list inode usage */
static int opt_time = 0;           /* --time: show time of last modification */
static const char *opt_exclude = NULL; /* --exclude=PATTERN */

/* Grand total */
static unsigned long grand_total = 0;

/* ================================================================
 * Human-readable formatting
 * ================================================================ */
static void human_size(unsigned long bytes, int si, char *buf, size_t bufsz)
{
    int base = si ? 1000 : 1024;
    const char *units = si ? "kMGTPE" : "KMGTPE";

    if (bytes < (unsigned long)base) {
        snprintf(buf, bufsz, "%lu", bytes);
        return;
    }

    double val = (double)bytes;
    int idx = -1;
    while (val >= base && units[idx + 1]) {
        val /= base;
        idx++;
    }
    if (idx < 0) {
        snprintf(buf, bufsz, "%lu", bytes);
    } else if (val >= 10.0) {
        snprintf(buf, bufsz, "%lu%c", (unsigned long)(val + 0.5), units[idx]);
    } else {
        unsigned long whole = (unsigned long)val;
        unsigned long frac = (unsigned long)((val - whole) * 10 + 0.5);
        if (frac >= 10) { whole++; frac = 0; }
        snprintf(buf, bufsz, "%lu.%lu%c", whole, frac, units[idx]);
    }
}

/* Get effective block size */
static unsigned long get_block_size(void)
{
    if (opt_bytes) return 1;
    if (opt_block_size > 0) return opt_block_size;
    if (opt_human || opt_si) return 1; /* format later */
    if (opt_kilo) return 1024;
    if (opt_mega) return 1048576;
    return 1024; /* default 1K blocks */
}

/* Format value for output */
static void format_value(unsigned long bytes, char *buf, size_t bufsz)
{
    if (opt_human) {
        human_size(bytes, 0, buf, bufsz);
    } else if (opt_si) {
        human_size(bytes, 1, buf, bufsz);
    } else {
        unsigned long blk = get_block_size();
        if (blk <= 1)
            snprintf(buf, bufsz, "%lu", bytes);
        else
            snprintf(buf, bufsz, "%lu", (bytes + blk - 1) / blk);
    }
}

/* Print a du entry */
static void print_entry(unsigned long bytes, const char *path, unsigned long mtime)
{
    /* Threshold filter */
    if (opt_threshold > 0 && (long)bytes < opt_threshold) return;
    if (opt_threshold < 0 && (long)bytes > -opt_threshold) return;

    char vbuf[32];
    format_value(bytes, vbuf, sizeof(vbuf));

    if (opt_time) {
        printf("%s\t%lu\t%s", vbuf, mtime, path);
    } else {
        printf("%s\t%s", vbuf, path);
    }

    if (opt_null)
        putchar('\0');
    else
        putchar('\n');
}

/* ================================================================
 * Recursive du computation
 * Returns the total bytes for this entry (for aggregation)
 * ================================================================ */
static unsigned long du_walk(const char *path, int depth, int is_cmdline_arg)
{
    struct stat st;
    int (*statfn)(const char*, struct stat*) = lstat;

    /* Dereference: -L always, -D/-H only for cmdline args */
    if (opt_deref || (opt_deref_args && is_cmdline_arg))
        statfn = stat;

    if (statfn(path, &st) != 0) {
        fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
        return 0;
    }

    /* Exclude pattern */
    if (opt_exclude) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (fnmatch(opt_exclude, base, 0) == 0)
            return 0;
    }

    /* File size: apparent size or block-allocated size */
    unsigned long file_bytes;
    if (opt_apparent_size || opt_bytes) {
        file_bytes = (unsigned long)st.st_size;
    } else {
        /* Estimate block usage: round up to 4096 (typical cluster) */
        file_bytes = ((unsigned long)st.st_size + 4095) & ~4095UL;
        if (st.st_size > 0 && file_bytes == 0)
            file_bytes = 4096;
    }

    unsigned long latest_mtime = (unsigned long)st.st_mtime;

    if (!S_ISDIR(st.st_mode)) {
        /* Regular file (or symlink, etc) */
        if (opt_all || (opt_summarize && is_cmdline_arg)) {
            if (opt_max_depth < 0 || depth <= opt_max_depth) {
                if (!opt_summarize || is_cmdline_arg)
                    print_entry(file_bytes, path, latest_mtime);
            }
        }
        grand_total += file_bytes;
        return file_bytes;
    }

    /* Directory: recurse */
    unsigned long dir_total = file_bytes; /* Directory entry itself */
    unsigned long subdir_total = 0;

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "du: cannot read directory '%s': %s\n", path, strerror(errno));
        grand_total += dir_total;
        return dir_total;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char child[4096];
        int plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/')
            snprintf(child, sizeof(child), "%s%s", path, de->d_name);
        else
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);

        unsigned long child_bytes = du_walk(child, depth + 1, 0);

        /* For -S, don't include subdirectory sizes in parent.
         * Use d_type from readdir() to avoid a second lstat() per child. */
        if (de->d_type == DT_DIR) {
            subdir_total += child_bytes;
        }

        dir_total += child_bytes;
    }
    closedir(d);

    unsigned long display_total = opt_separate_dirs ? (dir_total - subdir_total) : dir_total;

    /* Print this directory */
    if (opt_summarize) {
        if (is_cmdline_arg)
            print_entry(display_total, path, latest_mtime);
    } else if (opt_max_depth < 0 || depth <= opt_max_depth) {
        print_entry(display_total, path, latest_mtime);
    }

    grand_total += file_bytes; /* Only count this dir's own entry once */
    /* Note: children were already added to grand_total in their own du_walk calls */
    /* Undo the double-count: we added file_bytes at top + children added themselves */
    /* Actually, let's restructure: only leaf files add to grand_total */
    /* Fix: don't add dir_total to grand_total here, children handle themselves */
    grand_total -= file_bytes; /* Remove: children already counted */
    grand_total += file_bytes; /* But we do need this dir's own entry */

    return dir_total;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: du [OPTION]... [FILE]...\n"
        "Summarize disk usage of the set of FILEs, recursively for directories.\n"
        "\n"
        "  -0, --null            end each output line with NUL, not newline\n"
        "  -a, --all             write counts for all files, not just directories\n"
        "      --apparent-size   print apparent sizes rather than disk usage\n"
        "  -b, --bytes           equivalent to --apparent-size --block-size=1\n"
        "  -B, --block-size=SIZE scale sizes by SIZE\n"
        "  -c, --total           produce a grand total\n"
        "  -d, --max-depth=N     print total for directory only if N or fewer levels deep\n"
        "  -D, --dereference-args  dereference only symlinks listed on command line\n"
        "  -h, --human-readable  print sizes in human readable format (1K 234M 2G)\n"
        "  -H                    equivalent to -D (dereference command-line symlinks)\n"
        "      --si              like -h, but use powers of 1000 not 1024\n"
        "  -k                    like --block-size=1K\n"
        "  -l, --count-links     count sizes many times if hard linked\n"
        "  -L, --dereference     dereference all symbolic links\n"
        "  -m                    like --block-size=1M\n"
        "  -P, --no-dereference  do not follow any symbolic links (default)\n"
        "  -S, --separate-dirs   for directories do not include size of subdirectories\n"
        "  -s, --summarize       display only a total for each argument\n"
        "      --inodes          list inode usage information instead of block usage\n"
        "  -t, --threshold=SIZE  exclude entries smaller than SIZE if positive\n"
        "      --time            show time of the last modification\n"
        "  -x, --one-file-system skip directories on different file systems\n"
        "      --exclude=PATTERN exclude files that match PATTERN\n"
        "      --help            display this help and exit\n"
        "      --version         output version information and exit\n");
}

int main(int argc, char **argv)
{
    static struct option longopts[] = {
        { "null",            no_argument,       NULL, '0' },
        { "all",             no_argument,       NULL, 'a' },
        { "apparent-size",   no_argument,       NULL, 256 },
        { "bytes",           no_argument,       NULL, 'b' },
        { "block-size",      required_argument, NULL, 'B' },
        { "total",           no_argument,       NULL, 'c' },
        { "max-depth",       required_argument, NULL, 'd' },
        { "dereference-args",no_argument,       NULL, 'D' },
        { "human-readable",  no_argument,       NULL, 'h' },
        { "si",              no_argument,       NULL, 257 },
        { "count-links",     no_argument,       NULL, 'l' },
        { "dereference",     no_argument,       NULL, 'L' },
        { "no-dereference",  no_argument,       NULL, 'P' },
        { "separate-dirs",   no_argument,       NULL, 'S' },
        { "summarize",       no_argument,       NULL, 's' },
        { "inodes",          no_argument,       NULL, 258 },
        { "threshold",       required_argument, NULL, 't' },
        { "time",            no_argument,       NULL, 259 },
        { "one-file-system", no_argument,       NULL, 'x' },
        { "exclude",         required_argument, NULL, 260 },
        { "help",            no_argument,       NULL, 261 },
        { "version",         no_argument,       NULL, 262 },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "0abB:cd:DhHklLmPSst:x", longopts, NULL)) != -1) {
        switch (c) {
        case '0': opt_null = 1; break;
        case 'a': opt_all = 1; break;
        case 'b': opt_bytes = 1; opt_apparent_size = 1; opt_block_size = 1; break;
        case 'B': opt_block_size = strtoul(optarg, NULL, 10); break;
        case 'c': opt_total = 1; break;
        case 'd': opt_max_depth = atoi(optarg); break;
        case 'D': opt_deref_args = 1; break;
        case 'h': opt_human = 1; opt_si = 0; break;
        case 'H': opt_deref_args = 1; break;
        case 'k': opt_kilo = 1; break;
        case 'l': opt_count_links = 1; break;
        case 'L': opt_deref = 1; break;
        case 'm': opt_mega = 1; break;
        case 'P': opt_deref = 0; opt_deref_args = 0; break;
        case 'S': opt_separate_dirs = 1; break;
        case 's': opt_summarize = 1; opt_max_depth = 0; break;
        case 't': opt_threshold = strtol(optarg, NULL, 10); break;
        case 'x': opt_one_fs = 1; break;
        case 256: opt_apparent_size = 1; break;
        case 257: opt_si = 1; opt_human = 0; break;
        case 258: opt_inodes = 1; break;
        case 259: opt_time = 1; break;
        case 260: opt_exclude = optarg; break;
        case 261: usage(); return 0;
        case 262: printf("du (LikeOS) 1.0\n"); return 0;
        default: usage(); return 1;
        }
    }

    if (opt_summarize && opt_all) {
        fprintf(stderr, "du: cannot both summarize and show all entries\n");
        return 1;
    }

    /* Process file arguments, default to "." */
    if (optind >= argc) {
        du_walk(".", 0, 1);
    } else {
        for (int i = optind; i < argc; i++) {
            du_walk(argv[i], 0, 1);
        }
    }

    if (opt_total) {
        char vbuf[32];
        format_value(grand_total, vbuf, sizeof(vbuf));
        if (opt_null)
            printf("%s\ttotal\0", vbuf);
        else
            printf("%s\ttotal\n", vbuf);
    }

    return 0;
}
