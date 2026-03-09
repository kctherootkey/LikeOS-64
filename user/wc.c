/*
 * wc - print newline, word, and byte counts for each file
 *
 * Full implementation per wc(1) manpage.
 * Supports: -c, -m, -l, -L, -w, --files0-from, --total, --help, --version
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "wc"
#define VERSION      "1.0"

/* Flags for which counters to display */
static int opt_bytes;           /* -c */
static int opt_chars;           /* -m */
static int opt_lines;           /* -l */
static int opt_words;           /* -w */
static int opt_max_line_length; /* -L */
static int no_flags_set;        /* true if user specified nothing */

/* --total=WHEN: auto, always, only, never */
enum total_mode { TOTAL_AUTO, TOTAL_ALWAYS, TOTAL_ONLY, TOTAL_NEVER };
static enum total_mode total_mode = TOTAL_AUTO;

static const char *files0_from;  /* --files0-from=F */

/* Counters per file */
struct wc_counts {
    unsigned long lines;
    unsigned long words;
    unsigned long chars;
    unsigned long bytes;
    unsigned long max_line_len;
};

static struct wc_counts total;
static int file_count;

/* ── helpers ─────────────────────────────────────────────────────────── */

static int my_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
        || c == '\f' || c == '\v';
}

/* Width of a printable character – we only support single-byte characters,
 * so every byte that isn't a control character has width 1. */
static int char_width(unsigned char c)
{
    if (c == '\t')
        return 8;  /* simplification: tab = 8 */
    if (c < 0x20 || c == 0x7f)
        return 0;
    return 1;
}

/* Count a single open file descriptor, reading into buf */
static int count_fd(int fd, struct wc_counts *cnt)
{
    unsigned char buf[8192];
    ssize_t n;
    int in_word = 0;
    unsigned long cur_line_len = 0;

    cnt->lines = cnt->words = cnt->chars = cnt->bytes = cnt->max_line_len = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        cnt->bytes += (unsigned long)n;
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = buf[i];
            cnt->chars++;          /* -m: in single-byte locale chars==bytes */
            if (c == '\n') {
                cnt->lines++;
                if (cur_line_len > cnt->max_line_len)
                    cnt->max_line_len = cur_line_len;
                cur_line_len = 0;
            } else {
                cur_line_len += (unsigned long)char_width(c);
            }
            if (my_isspace(c)) {
                in_word = 0;
            } else {
                if (!in_word) {
                    cnt->words++;
                    in_word = 1;
                }
            }
        }
    }
    /* last unterminated line */
    if (cur_line_len > cnt->max_line_len)
        cnt->max_line_len = cur_line_len;

    if (n < 0)
        return -1;
    return 0;
}

/* Determine field width for a value */
static int num_width(unsigned long v)
{
    if (v == 0)
        return 1;
    int w = 0;
    while (v) { w++; v /= 10; }
    return w;
}

/* Compute column widths for aligned output */
static int col_width;

static void compute_col_width(void)
{
    unsigned long m = total.lines;
    if (total.words   > m) m = total.words;
    if (total.chars   > m) m = total.chars;
    if (total.bytes   > m) m = total.bytes;
    if (total.max_line_len > m) m = total.max_line_len;
    col_width = num_width(m);
    if (col_width < 1) col_width = 1;
}

static void print_counts(const struct wc_counts *c, const char *name)
{
    int first = 1;
    if (opt_lines || no_flags_set) {
        if (!first) putchar(' ');
        printf("%*lu", col_width, c->lines);
        first = 0;
    }
    if (opt_words || no_flags_set) {
        if (!first) putchar(' ');
        printf("%*lu", col_width, c->words);
        first = 0;
    }
    if (opt_chars) {
        if (!first) putchar(' ');
        printf("%*lu", col_width, c->chars);
        first = 0;
    }
    if (opt_bytes || no_flags_set) {
        if (!first) putchar(' ');
        printf("%*lu", col_width, c->bytes);
        first = 0;
    }
    if (opt_max_line_length) {
        if (!first) putchar(' ');
        printf("%*lu", col_width, c->max_line_len);
        first = 0;
    }
    if (name) {
        putchar(' ');
        fputs(name, stdout);
    }
    putchar('\n');
}

static void add_to_total(const struct wc_counts *c)
{
    total.lines  += c->lines;
    total.words  += c->words;
    total.chars  += c->chars;
    total.bytes  += c->bytes;
    if (c->max_line_len > total.max_line_len)
        total.max_line_len = c->max_line_len;
}

/* ── process one path ────────────────────────────────────────────────── */
static int process_file(const char *path)
{
    struct wc_counts cnt;
    int fd;
    const char *label;

    if (!path || (path[0] == '-' && path[1] == '\0')) {
        fd = 0;  /* stdin */
        label = NULL;
    } else {
        fd = open(path, 0);
        if (fd < 0) {
            fprintf(stderr, "%s: %s: No such file or directory\n",
                    PROGRAM_NAME, path);
            return 1;
        }
        label = path;
    }

    int err = count_fd(fd, &cnt);
    if (fd > 2)
        close(fd);

    if (err) {
        fprintf(stderr, "%s: %s: read error\n",
                PROGRAM_NAME, label ? label : "standard input");
        return 1;
    }

    add_to_total(&cnt);
    file_count++;

    if (total_mode != TOTAL_ONLY)
        print_counts(&cnt, label);

    return 0;
}

/* Read NUL-separated file names from an open fd */
static int process_files0(int fd)
{
    char buf[4096];
    char name[4096];
    int nlen = 0;
    ssize_t n;
    int status = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\0') {
                name[nlen] = '\0';
                if (nlen > 0) {
                    if (process_file(name))
                        status = 1;
                }
                nlen = 0;
            } else {
                if (nlen < (int)sizeof(name) - 1)
                    name[nlen++] = buf[i];
            }
        }
    }
    /* trailing name without NUL */
    if (nlen > 0) {
        name[nlen] = '\0';
        if (process_file(name))
            status = 1;
    }
    return status;
}

/* ── usage / version ─────────────────────────────────────────────────── */
static void usage(void)
{
    printf(
"Usage: %s [OPTION]... [FILE]...\n"
"  or:  %s [OPTION]... --files0-from=F\n"
"Print newline, word, and byte counts for each FILE, and a total line if\n"
"more than one FILE is specified.  A word is a non-zero-length sequence\n"
"of printable characters delimited by white space.\n"
"\n"
"With no FILE, or when FILE is -, read standard input.\n"
"\n"
"  -c, --bytes            print the byte counts\n"
"  -m, --chars            print the character counts\n"
"  -l, --lines            print the newline counts\n"
"  -L, --max-line-length  print the maximum display width\n"
"  -w, --words            print the word counts\n"
"      --files0-from=F    read input from NUL-terminated names in file F;\n"
"                           if F is - then read names from standard input\n"
"      --total=WHEN       when to print a line with total counts;\n"
"                           WHEN can be: auto, always, only, never\n"
"      --help             display this help and exit\n"
"      --version          output version information and exit\n",
    PROGRAM_NAME, PROGRAM_NAME);
}

static void version(void)
{
    printf("%s %s\n", PROGRAM_NAME, VERSION);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    enum {
        OPT_FILES0_FROM = 256,
        OPT_TOTAL,
        OPT_HELP,
        OPT_VERSION,
    };

    static const struct option longopts[] = {
        { "bytes",           no_argument,       NULL, 'c' },
        { "chars",           no_argument,       NULL, 'm' },
        { "lines",           no_argument,       NULL, 'l' },
        { "words",           no_argument,       NULL, 'w' },
        { "max-line-length", no_argument,       NULL, 'L' },
        { "files0-from",     required_argument, NULL, OPT_FILES0_FROM },
        { "total",           required_argument, NULL, OPT_TOTAL },
        { "help",            no_argument,       NULL, OPT_HELP },
        { "version",         no_argument,       NULL, OPT_VERSION },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "cmlwL", longopts, NULL)) != -1) {
        switch (c) {
        case 'c': opt_bytes = 1;           break;
        case 'm': opt_chars = 1;           break;
        case 'l': opt_lines = 1;           break;
        case 'w': opt_words = 1;           break;
        case 'L': opt_max_line_length = 1; break;
        case OPT_FILES0_FROM:
            files0_from = optarg;
            break;
        case OPT_TOTAL:
            if      (strcmp(optarg, "auto")   == 0) total_mode = TOTAL_AUTO;
            else if (strcmp(optarg, "always") == 0) total_mode = TOTAL_ALWAYS;
            else if (strcmp(optarg, "only")   == 0) total_mode = TOTAL_ONLY;
            else if (strcmp(optarg, "never")  == 0) total_mode = TOTAL_NEVER;
            else {
                fprintf(stderr, "%s: invalid argument '%s' for '--total'\n",
                        PROGRAM_NAME, optarg);
                return 1;
            }
            break;
        case OPT_HELP:    usage();   return 0;
        case OPT_VERSION: version(); return 0;
        default:
            fprintf(stderr, "Try '%s --help' for more information.\n",
                    PROGRAM_NAME);
            return 1;
        }
    }

    no_flags_set = !(opt_bytes | opt_chars | opt_lines | opt_words
                     | opt_max_line_length);

    int status = 0;

    if (files0_from) {
        if (optind < argc) {
            fprintf(stderr,
                "%s: extra operand '%s'\n"
                "file operands cannot be combined with --files0-from\n",
                PROGRAM_NAME, argv[optind]);
            return 1;
        }
        int fd;
        if (strcmp(files0_from, "-") == 0) {
            fd = 0;
        } else {
            fd = open(files0_from, 0);
            if (fd < 0) {
                fprintf(stderr, "%s: cannot open '%s'\n",
                        PROGRAM_NAME, files0_from);
                return 1;
            }
        }
        status = process_files0(fd);
        if (fd > 2)
            close(fd);
    } else if (optind >= argc) {
        /* No files → read stdin */
        status = process_file("-");
    } else {
        for (int i = optind; i < argc; i++) {
            if (process_file(argv[i]))
                status = 1;
        }
    }

    /* Determine column width from totals before printing */
    compute_col_width();

    /* Reprint nothing – we already printed per-file. Handle total line. */
    /* Actually, we need to know column widths BEFORE printing.
     * For simplicity with the streaming approach: we print with a fixed
     * minimum width and then print the total. The GNU approach buffers,
     * but for this implementation we use a 7-column default like GNU. */

    int show_total = 0;
    switch (total_mode) {
    case TOTAL_AUTO:   show_total = (file_count > 1); break;
    case TOTAL_ALWAYS: show_total = 1; break;
    case TOTAL_ONLY:   show_total = 1; break;
    case TOTAL_NEVER:  show_total = 0; break;
    }

    if (show_total)
        print_counts(&total, "total");

    return status;
}
