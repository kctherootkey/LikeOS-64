/*
 * df - report file system disk space usage
 *
 * Usage: df [OPTION]... [FILE]...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/vfs.h>

/* Output column flags for --output */
#define COL_SOURCE   (1 << 0)
#define COL_FSTYPE   (1 << 1)
#define COL_SIZE     (1 << 2)
#define COL_USED     (1 << 3)
#define COL_AVAIL    (1 << 4)
#define COL_PCENT    (1 << 5)
#define COL_TARGET   (1 << 6)
#define COL_ITOTAL   (1 << 7)
#define COL_IUSED    (1 << 8)
#define COL_IAVAIL   (1 << 9)
#define COL_IPCENT   (1 << 10)
#define COL_FILE     (1 << 11)

#define COL_DEFAULT  (COL_SOURCE|COL_SIZE|COL_USED|COL_AVAIL|COL_PCENT|COL_TARGET)

/* Options */
static int opt_all = 0;          /* -a: include pseudo filesystems */
static int opt_human = 0;        /* -h: human-readable (1024) */
static int opt_si = 0;           /* -H: human-readable (1000) */
static int opt_inodes = 0;       /* -i: show inode info */
static int opt_block_1k = 0;     /* -k: 1K blocks */
static int opt_local = 0;        /* -l: local filesystems only */
static int opt_posix = 0;        /* -P: POSIX output format */
static int opt_show_type = 0;    /* -T: show filesystem type */
static int opt_total = 0;        /* --total: show grand total */
static unsigned long opt_block_size = 0; /* -B N: block size */
static unsigned int opt_columns = 0;     /* --output=... */
static const char *opt_type = NULL;      /* -t TYPE */
static const char *opt_exclude = NULL;   /* -x TYPE */

/* Totals */
static unsigned long total_blocks = 0;
static unsigned long total_used = 0;
static unsigned long total_avail = 0;
static unsigned long total_files = 0;
static unsigned long total_fused = 0;
static unsigned long total_favail = 0;

/* Format a size as human-readable */
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
        /* One decimal for small values */
        unsigned long whole = (unsigned long)val;
        unsigned long frac = (unsigned long)((val - whole) * 10 + 0.5);
        if (frac >= 10) { whole++; frac = 0; }
        snprintf(buf, bufsz, "%lu.%lu%c", whole, frac, units[idx]);
    }
}

/* Get effective block size for display */
static unsigned long get_block_size(void)
{
    if (opt_block_size > 0) return opt_block_size;
    if (opt_human || opt_si) return 1; /* bytes, formatted later */
    if (opt_block_1k) return 1024;
    return 1024; /* default 1K blocks */
}

/* Scale a byte count to display blocks */
static unsigned long scale(unsigned long bytes, unsigned long blksz)
{
    if (blksz <= 1) return bytes;
    return (bytes + blksz - 1) / blksz;
}

/* Format a value for display */
static void format_value(unsigned long bytes, char *buf, size_t bufsz)
{
    if (opt_human) {
        human_size(bytes, 0, buf, bufsz);
    } else if (opt_si) {
        human_size(bytes, 1, buf, bufsz);
    } else {
        unsigned long blk = get_block_size();
        snprintf(buf, bufsz, "%lu", scale(bytes, blk));
    }
}

/* ================================================================
 * Print filesystem entry
 * ================================================================ */
static void print_fs(const char *source, const char *fstype,
                     const char *target, const struct statfs *st,
                     const char *file)
{
    unsigned long blksize = st->f_bsize ? st->f_bsize : st->f_frsize;
    if (blksize == 0) blksize = 1;

    unsigned long total_bytes = st->f_blocks * blksize;
    unsigned long free_bytes  = st->f_bfree * blksize;
    unsigned long avail_bytes = st->f_bavail * blksize;
    unsigned long used_bytes  = total_bytes - free_bytes;

    /* Accumulate totals */
    total_blocks += total_bytes;
    total_used   += used_bytes;
    total_avail  += avail_bytes;
    total_files  += st->f_files;
    total_fused  += st->f_files - st->f_ffree;
    total_favail += st->f_ffree;

    /* Use-percent */
    int pct = 0;
    if (total_bytes > 0) {
        unsigned long nonpriv = used_bytes + avail_bytes;
        if (nonpriv > 0)
            pct = (int)((used_bytes * 100 + nonpriv - 1) / nonpriv);
    }

    int ipct = 0;
    if (st->f_files > 0) {
        unsigned long iused = st->f_files - st->f_ffree;
        ipct = (int)((iused * 100 + st->f_files - 1) / st->f_files);
    }

    /* Custom --output columns */
    unsigned int cols = opt_columns ? opt_columns : COL_DEFAULT;

    /* Override columns for -i mode */
    if (opt_inodes && !opt_columns)
        cols = COL_SOURCE | COL_ITOTAL | COL_IUSED | COL_IAVAIL | COL_IPCENT | COL_TARGET;

    /* Add type column if -T */
    if (opt_show_type && !opt_columns)
        cols |= COL_FSTYPE;

    char vbuf[32];

    if (cols & COL_SOURCE)
        printf("%-20s", source);
    if (cols & COL_FSTYPE)
        printf(" %-6s", fstype);
    if (cols & COL_SIZE) {
        format_value(total_bytes, vbuf, sizeof(vbuf));
        printf(" %10s", vbuf);
    }
    if (cols & COL_USED) {
        format_value(used_bytes, vbuf, sizeof(vbuf));
        printf(" %10s", vbuf);
    }
    if (cols & COL_AVAIL) {
        format_value(avail_bytes, vbuf, sizeof(vbuf));
        printf(" %10s", vbuf);
    }
    if (cols & COL_PCENT)
        printf(" %4d%%", pct);
    if (cols & COL_ITOTAL)
        printf(" %10lu", st->f_files);
    if (cols & COL_IUSED)
        printf(" %10lu", st->f_files - st->f_ffree);
    if (cols & COL_IAVAIL)
        printf(" %10lu", st->f_ffree);
    if (cols & COL_IPCENT)
        printf(" %4d%%", ipct);
    if (cols & COL_TARGET)
        printf(" %s", target);
    if (cols & COL_FILE)
        printf(" %s", file ? file : "-");

    putchar('\n');
}

/* ================================================================
 * Print header
 * ================================================================ */
static void print_header(void)
{
    unsigned int cols = opt_columns ? opt_columns : COL_DEFAULT;
    if (opt_inodes && !opt_columns)
        cols = COL_SOURCE | COL_ITOTAL | COL_IUSED | COL_IAVAIL | COL_IPCENT | COL_TARGET;
    if (opt_show_type && !opt_columns)
        cols |= COL_FSTYPE;

    const char *size_label = "1K-blocks";
    if (opt_human) size_label = "Size";
    else if (opt_si) size_label = "Size";
    else if (opt_block_size > 0) {
        static char blabel[32];
        snprintf(blabel, sizeof(blabel), "%luB-blocks", opt_block_size);
        size_label = blabel;
    }

    if (cols & COL_SOURCE)
        printf("%-20s", "Filesystem");
    if (cols & COL_FSTYPE)
        printf(" %-6s", "Type");
    if (cols & COL_SIZE)
        printf(" %10s", size_label);
    if (cols & COL_USED)
        printf(" %10s", "Used");
    if (cols & COL_AVAIL)
        printf(" %10s", "Available");
    if (cols & COL_PCENT)
        printf(" %5s", "Use%");
    if (cols & COL_ITOTAL)
        printf(" %10s", "Inodes");
    if (cols & COL_IUSED)
        printf(" %10s", "IUsed");
    if (cols & COL_IAVAIL)
        printf(" %10s", "IFree");
    if (cols & COL_IPCENT)
        printf(" %5s", "IUse%");
    if (cols & COL_TARGET)
        printf(" %s", "Mounted on");
    if (cols & COL_FILE)
        printf(" %s", "File");

    putchar('\n');
}

/* ================================================================
 * Parse --output column list
 * ================================================================ */
static unsigned int parse_output_columns(const char *arg)
{
    unsigned int cols = 0;
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = buf;
    while (tok && *tok) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';

        if (strcmp(tok, "source") == 0)      cols |= COL_SOURCE;
        else if (strcmp(tok, "fstype") == 0)  cols |= COL_FSTYPE;
        else if (strcmp(tok, "size") == 0)    cols |= COL_SIZE;
        else if (strcmp(tok, "used") == 0)    cols |= COL_USED;
        else if (strcmp(tok, "avail") == 0)   cols |= COL_AVAIL;
        else if (strcmp(tok, "pcent") == 0)   cols |= COL_PCENT;
        else if (strcmp(tok, "target") == 0)  cols |= COL_TARGET;
        else if (strcmp(tok, "itotal") == 0)  cols |= COL_ITOTAL;
        else if (strcmp(tok, "iused") == 0)   cols |= COL_IUSED;
        else if (strcmp(tok, "iavail") == 0)  cols |= COL_IAVAIL;
        else if (strcmp(tok, "ipcent") == 0)  cols |= COL_IPCENT;
        else if (strcmp(tok, "file") == 0)    cols |= COL_FILE;
        else {
            fprintf(stderr, "df: unknown output column '%s'\n", tok);
            exit(1);
        }

        tok = comma ? comma + 1 : NULL;
    }
    return cols;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: df [OPTION]... [FILE]...\n"
        "Show information about the file system on which each FILE resides,\n"
        "or all file systems by default.\n"
        "\n"
        "  -a, --all             include pseudo, duplicate, inaccessible file systems\n"
        "  -B, --block-size=SIZE scale sizes by SIZE before printing\n"
        "      --total           elicit a grand total\n"
        "  -h, --human-readable  print sizes in powers of 1024 (e.g., 1023M)\n"
        "  -H, --si              print sizes in powers of 1000 (e.g., 1.1G)\n"
        "  -i, --inodes          list inode information instead of block usage\n"
        "  -k                    like --block-size=1K\n"
        "  -l, --local           limit listing to local file systems\n"
        "      --no-sync         do not invoke sync before getting usage info (default)\n"
        "  -P, --portability     use the POSIX output format\n"
        "      --sync            invoke sync before getting usage info\n"
        "  -t, --type=TYPE       limit listing to file systems of type TYPE\n"
        "  -T, --print-type      print file system type\n"
        "  -x, --exclude-type=TYPE   limit listing to file systems not of type TYPE\n"
        "      --output[=FIELD_LIST]  use the output format defined by FIELD_LIST\n"
        "      --help            display this help and exit\n"
        "      --version         output version information and exit\n");
}

int main(int argc, char **argv)
{
    static struct option longopts[] = {
        { "all",            no_argument,       NULL, 'a' },
        { "block-size",     required_argument, NULL, 'B' },
        { "total",          no_argument,       NULL, 256 },
        { "human-readable", no_argument,       NULL, 'h' },
        { "si",             no_argument,       NULL, 'H' },
        { "inodes",         no_argument,       NULL, 'i' },
        { "local",          no_argument,       NULL, 'l' },
        { "no-sync",        no_argument,       NULL, 257 },
        { "portability",    no_argument,       NULL, 'P' },
        { "sync",           no_argument,       NULL, 258 },
        { "type",           required_argument, NULL, 't' },
        { "print-type",     no_argument,       NULL, 'T' },
        { "exclude-type",   required_argument, NULL, 'x' },
        { "output",         required_argument, NULL, 259 },
        { "help",           no_argument,       NULL, 260 },
        { "version",        no_argument,       NULL, 261 },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "aB:hHiklPTt:x:", longopts, NULL)) != -1) {
        switch (c) {
        case 'a': opt_all = 1; break;
        case 'B': opt_block_size = strtoul(optarg, NULL, 10); break;
        case 'h': opt_human = 1; opt_si = 0; break;
        case 'H': opt_si = 1; opt_human = 0; break;
        case 'i': opt_inodes = 1; break;
        case 'k': opt_block_1k = 1; break;
        case 'l': opt_local = 1; break;
        case 'P': opt_posix = 1; break;
        case 'T': opt_show_type = 1; break;
        case 't': opt_type = optarg; break;
        case 'x': opt_exclude = optarg; break;
        case 256: opt_total = 1; break;
        case 257: /* --no-sync, default */ break;
        case 258: /* --sync */ sync(); break;
        case 259: opt_columns = parse_output_columns(optarg); break;
        case 260: usage(); return 0;
        case 261: printf("df (LikeOS) 1.0\n"); return 0;
        default: usage(); return 1;
        }
    }

    /* The OS has a single root filesystem.
     * If specific files are given, call statfs on each.
     * Otherwise, show the root filesystem. */

    print_header();

    if (optind < argc) {
        /* statfs for each file argument */
        for (int i = optind; i < argc; i++) {
            struct statfs st;
            if (statfs(argv[i], &st) != 0) {
                fprintf(stderr, "df: '%s': %s\n", argv[i], strerror(errno));
                continue;
            }
            /* Determine fstype name from magic */
            const char *fstype = "unknown";
            if (st.f_type == 0x4d44) fstype = "vfat";

            /* Filter by type */
            if (opt_type && strcmp(opt_type, fstype) != 0) continue;
            if (opt_exclude && strcmp(opt_exclude, fstype) == 0) continue;

            print_fs("rootfs", fstype, "/", &st, argv[i]);
        }
    } else {
        /* Show root filesystem */
        struct statfs st;
        if (statfs("/", &st) != 0) {
            fprintf(stderr, "df: cannot access '/': %s\n", strerror(errno));
            return 1;
        }
        const char *fstype = "unknown";
        if (st.f_type == 0x4d44) fstype = "vfat";

        if (opt_type && strcmp(opt_type, fstype) != 0) goto done;
        if (opt_exclude && strcmp(opt_exclude, fstype) == 0) goto done;

        print_fs("rootfs", fstype, "/", &st, NULL);

        /* Show devfs if -a */
        if (opt_all) {
            struct statfs devst;
            memset(&devst, 0, sizeof(devst));
            devst.f_bsize = 4096;
            devst.f_type = 0x1373; /* devfs */
            if (!opt_type || strcmp(opt_type, "devfs") == 0) {
                if (!opt_exclude || strcmp(opt_exclude, "devfs") != 0) {
                    const char *dtype = "devfs";
                    print_fs("devfs", dtype, "/dev", &devst, NULL);
                }
            }
        }
    }

done:
    if (opt_total) {
        /* Print totals line */
        struct statfs tst;
        memset(&tst, 0, sizeof(tst));
        tst.f_bsize = 1;
        tst.f_blocks = total_blocks;
        tst.f_bfree  = total_blocks - total_used;
        tst.f_bavail = total_avail;
        tst.f_files  = total_files;
        tst.f_ffree  = total_favail;
        print_fs("total", "-", "-", &tst, NULL);
    }

    return 0;
}
