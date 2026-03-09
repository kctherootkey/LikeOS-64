/*
 * head - output the first part of files
 *
 * Full implementation per head(1) manpage.
 * Supports: -c [-]NUM, -n [-]NUM, -q, -v, -z, --help, --version
 * NUM may have multiplier suffixes: b, kB, K, MB, M, GB, G, etc.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "head"
#define VERSION      "1.0"

/* Mode of operation */
enum mode { MODE_LINES, MODE_BYTES };
static enum mode mode = MODE_LINES;
static long count = 10;     /* default: 10 lines */
static int from_end;        /* if 1, "all but last N" */
static int opt_quiet;       /* -q */
static int opt_verbose;     /* -v */
static int opt_zero;        /* -z: NUL as delimiter instead of newline */

/* ── parse NUM with optional multiplier suffix ───────────────────────── */
static long parse_num(const char *s, int *negate)
{
    *negate = 0;
    if (*s == '-') { *negate = 1; s++; }
    else if (*s == '+') { s++; }

    char *end = NULL;
    long val = strtol(s, &end, 10);
    if (val < 0) val = -val;

    if (end && *end) {
        switch (*end) {
        case 'b':  val *= 512;                        break;
        case 'K':  val *= 1024;                       break;
        case 'M':
            if (end[1] == 'B') val *= 1000L * 1000;
            else               val *= 1024L * 1024;
            break;
        case 'G':
            if (end[1] == 'B') val *= 1000L * 1000 * 1000;
            else               val *= 1024L * 1024 * 1024;
            break;
        case 'k':
            if (end[1] == 'B') val *= 1000;
            else               val *= 1024;
            break;
        default:
            break;
        }
    }
    return val;
}

/* ── output first N lines from fd ────────────────────────────────────── */
static int head_lines_first(int fd, long n, char delim)
{
    if (n <= 0)
        return 0;
    unsigned char buf[8192];
    long lines_seen = 0;
    ssize_t nr;
    while ((nr = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < nr; i++) {
            putchar(buf[i]);
            if (buf[i] == (unsigned char)delim) {
                lines_seen++;
                if (lines_seen >= n)
                    return 0;
            }
        }
    }
    return (nr < 0) ? 1 : 0;
}

/* ── output all but last N lines from fd ─────────────────────────────── */
static int head_lines_allbut(int fd, long n, char delim)
{
    if (n <= 0) {
        /* all but 0 = everything */
        unsigned char buf[8192];
        ssize_t nr;
        while ((nr = read(fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, (size_t)nr, stdout);
        return (nr < 0) ? 1 : 0;
    }

    /* Buffer up to n lines in a circular buffer of line pointers */
    /* Simple approach: read whole file into memory, count lines from end */
    size_t cap = 65536, len = 0;
    unsigned char *data = malloc(cap);
    if (!data) return 1;

    ssize_t nr;
    while ((nr = read(fd, data + len, cap - len)) > 0) {
        len += (size_t)nr;
        if (len >= cap) {
            cap *= 2;
            unsigned char *tmp = realloc(data, cap);
            if (!tmp) { free(data); return 1; }
            data = tmp;
        }
    }

    /* Count n lines from end */
    long tail_lines = 0;
    size_t cut = len;
    for (size_t i = len; i > 0; i--) {
        if (data[i - 1] == (unsigned char)delim) {
            tail_lines++;
            if (tail_lines >= n) {
                cut = i - 1;
                break;
            }
        }
    }
    /* If we didn't find n delimiters, output nothing when tail_lines < n */
    if (tail_lines < n)
        cut = 0;

    if (cut > 0)
        fwrite(data, 1, cut, stdout);

    free(data);
    return (nr < 0) ? 1 : 0;
}

/* ── output first N bytes from fd ────────────────────────────────────── */
static int head_bytes_first(int fd, long n)
{
    unsigned char buf[8192];
    long remaining = n;
    while (remaining > 0) {
        ssize_t want = (remaining < (long)sizeof(buf)) ? remaining : (long)sizeof(buf);
        ssize_t nr = read(fd, buf, (size_t)want);
        if (nr <= 0)
            return (nr < 0) ? 1 : 0;
        fwrite(buf, 1, (size_t)nr, stdout);
        remaining -= nr;
    }
    return 0;
}

/* ── output all but last N bytes from fd ─────────────────────────────── */
static int head_bytes_allbut(int fd, long n)
{
    if (n <= 0) {
        unsigned char buf[8192];
        ssize_t nr;
        while ((nr = read(fd, buf, sizeof(buf))) > 0)
            fwrite(buf, 1, (size_t)nr, stdout);
        return (nr < 0) ? 1 : 0;
    }

    /* Read whole file, print all but last n bytes */
    size_t cap = 65536, len = 0;
    unsigned char *data = malloc(cap);
    if (!data) return 1;

    ssize_t nr;
    while ((nr = read(fd, data + len, cap - len)) > 0) {
        len += (size_t)nr;
        if (len >= cap) {
            cap *= 2;
            unsigned char *tmp = realloc(data, cap);
            if (!tmp) { free(data); return 1; }
            data = tmp;
        }
    }

    if (len > (size_t)n)
        fwrite(data, 1, len - (size_t)n, stdout);

    free(data);
    return (nr < 0) ? 1 : 0;
}

/* ── process one file ────────────────────────────────────────────────── */
static int process_file(const char *path, int show_header)
{
    int fd;
    const char *label;

    if (!path || (path[0] == '-' && path[1] == '\0')) {
        fd = 0;
        label = "standard input";
    } else {
        fd = open(path, 0);
        if (fd < 0) {
            fprintf(stderr, "%s: cannot open '%s' for reading: No such file or directory\n",
                    PROGRAM_NAME, path);
            return 1;
        }
        label = path;
    }

    if (show_header)
        printf("==> %s <==\n", label);

    char delim = opt_zero ? '\0' : '\n';
    int err;

    if (mode == MODE_LINES) {
        if (from_end)
            err = head_lines_allbut(fd, count, delim);
        else
            err = head_lines_first(fd, count, delim);
    } else {
        if (from_end)
            err = head_bytes_allbut(fd, count);
        else
            err = head_bytes_first(fd, count);
    }

    if (fd > 2)
        close(fd);
    return err;
}

/* ── usage / version ─────────────────────────────────────────────────── */
static void usage(void)
{
    printf(
"Usage: %s [OPTION]... [FILE]...\n"
"Print the first 10 lines of each FILE to standard output.\n"
"With more than one FILE, precede each with a header giving the file name.\n"
"\n"
"With no FILE, or when FILE is -, read standard input.\n"
"\n"
"  -c, --bytes=[-]NUM   print the first NUM bytes of each file;\n"
"                         with the leading '-', print all but the last\n"
"                         NUM bytes of each file\n"
"  -n, --lines=[-]NUM   print the first NUM lines instead of the first 10;\n"
"                         with the leading '-', print all but the last\n"
"                         NUM lines of each file\n"
"  -q, --quiet, --silent  never print headers giving file names\n"
"  -v, --verbose        always print headers giving file names\n"
"  -z, --zero-terminated  line delimiter is NUL, not newline\n"
"      --help           display this help and exit\n"
"      --version        output version information and exit\n"
"\n"
"NUM may have a multiplier suffix:\n"
"b 512, kB 1000, K 1024, MB 1000*1000, M 1024*1024, GB 1000*1000*1000,\n"
"G 1024*1024*1024, and so on for T, P, E, Z, Y, R, Q.\n",
    PROGRAM_NAME);
}

static void version(void)
{
    printf("%s %s\n", PROGRAM_NAME, VERSION);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    enum {
        OPT_HELP = 256,
        OPT_VERSION,
    };

    static const struct option longopts[] = {
        { "bytes",           required_argument, NULL, 'c' },
        { "lines",           required_argument, NULL, 'n' },
        { "quiet",           no_argument,       NULL, 'q' },
        { "silent",          no_argument,       NULL, 'q' },
        { "verbose",         no_argument,       NULL, 'v' },
        { "zero-terminated", no_argument,       NULL, 'z' },
        { "help",            no_argument,       NULL, OPT_HELP },
        { "version",         no_argument,       NULL, OPT_VERSION },
        { NULL, 0, NULL, 0 }
    };

    /* Handle obsolete -NUM syntax (e.g. head -10 means head -n 10) */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '1' && argv[i][1] <= '9') {
            int dummy = 0;
            count = parse_num(argv[i] + 1, &dummy);
            mode = MODE_LINES;
            from_end = 0;
            /* Remove this argument so getopt won't see it */
            for (int j = i; j < argc - 1; j++)
                argv[j] = argv[j + 1];
            argc--;
            i--;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            break;  /* stop at -- */
        } else if (argv[i][0] != '-') {
            break;  /* stop at first non-option (filename) */
        }
    }

    int c;
    while ((c = getopt_long(argc, argv, "c:n:qvz", longopts, NULL)) != -1) {
        switch (c) {
        case 'c':
            mode = MODE_BYTES;
            from_end = (optarg[0] == '-') ? 1 : 0;
            count = parse_num(optarg, &from_end);
            break;
        case 'n':
            mode = MODE_LINES;
            from_end = (optarg[0] == '-') ? 1 : 0;
            count = parse_num(optarg, &from_end);
            break;
        case 'q': opt_quiet   = 1; opt_verbose = 0; break;
        case 'v': opt_verbose = 1; opt_quiet   = 0; break;
        case 'z': opt_zero    = 1; break;
        case OPT_HELP:    usage();   return 0;
        case OPT_VERSION: version(); return 0;
        default:
            fprintf(stderr, "Try '%s --help' for more information.\n",
                    PROGRAM_NAME);
            return 1;
        }
    }

    int nfiles = argc - optind;
    if (nfiles == 0) nfiles = 1;  /* stdin */

    int status = 0;
    int need_separator = 0;

    if (optind >= argc) {
        /* stdin */
        int show = opt_verbose;
        if (process_file("-", show))
            status = 1;
    } else {
        for (int i = optind; i < argc; i++) {
            int show_header;
            if (opt_quiet)
                show_header = 0;
            else if (opt_verbose)
                show_header = 1;
            else
                show_header = (argc - optind > 1);

            if (need_separator && show_header)
                putchar('\n');

            if (process_file(argv[i], show_header))
                status = 1;
            need_separator = 1;
        }
    }

    return status;
}
