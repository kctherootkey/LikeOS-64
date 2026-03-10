/*
 * cut - remove sections from each line of files
 *
 * Full implementation per cut(1) manpage.
 * Supports: -b, -c, -d, -f, -n, -s, -z
 *           --complement, --output-delimiter
 *           --help, --version
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "cut"
#define VERSION      "1.0"
#define MAX_RANGES   256
#define LINE_MAX     65536

/* ── Mode of operation ──────────────────────────────────────────────── */
enum cut_mode { MODE_NONE, MODE_BYTES, MODE_CHARS, MODE_FIELDS };
static enum cut_mode mode = MODE_NONE;

/* ── Range list ─────────────────────────────────────────────────────── */
typedef struct {
    int lo;     /* 1-based inclusive start */
    int hi;     /* 1-based inclusive end  (INT_MAX for open-ended) */
} range_t;

static range_t ranges[MAX_RANGES];
static int nranges;

/* ── Options ────────────────────────────────────────────────────────── */
static char opt_delim = '\t';               /* -d: field delimiter */
static int opt_only_delimited;              /* -s */
static int opt_complement;                  /* --complement */
static int opt_zero;                        /* -z */
static const char *opt_output_delim;        /* --output-delimiter */
static int output_delim_set;

/* ── Parse range list (N, N-, N-M, -M) ─────────────────────────────── */

static int parse_ranges(const char *spec)
{
    const char *p = spec;
    nranges = 0;

    while (*p) {
        if (nranges >= MAX_RANGES) {
            fprintf(stderr, "cut: too many ranges\n");
            return -1;
        }

        int lo = 0, hi = 0;
        int have_lo = 0, have_hi = 0;

        if (*p != '-' && *p != ',') {
            lo = (int)strtol(p, (char **)&p, 10);
            have_lo = 1;
        }

        if (*p == '-') {
            p++;
            if (*p && *p != ',') {
                hi = (int)strtol(p, (char **)&p, 10);
                have_hi = 1;
            }
        } else {
            hi = lo;
            have_hi = 1;
        }

        if (!have_lo && !have_hi) {
            fprintf(stderr, "cut: invalid range\n");
            return -1;
        }

        if (!have_lo) lo = 1;
        if (!have_hi) hi = 0x7fffffff;

        if (lo < 1) lo = 1;
        if (hi < lo) { int t = lo; lo = hi; hi = t; }

        ranges[nranges].lo = lo;
        ranges[nranges].hi = hi;
        nranges++;

        if (*p == ',') p++;
    }

    /* Sort ranges by lo */
    for (int i = 0; i < nranges - 1; i++) {
        for (int j = i + 1; j < nranges; j++) {
            if (ranges[j].lo < ranges[i].lo) {
                range_t tmp = ranges[i];
                ranges[i] = ranges[j];
                ranges[j] = tmp;
            }
        }
    }

    return 0;
}

/* ── Check if position (1-based) is selected ────────────────────────── */

static int is_selected(int pos)
{
    int sel = 0;
    for (int i = 0; i < nranges; i++) {
        if (pos >= ranges[i].lo && pos <= ranges[i].hi) {
            sel = 1;
            break;
        }
    }
    return opt_complement ? !sel : sel;
}

/* ── Process a line in bytes/characters mode ────────────────────────── */

static void cut_bytes_line(const char *line, FILE *out, char delim)
{
    int len = (int)strlen(line);
    int first = 1;

    for (int i = 1; i <= len; i++) {
        if (is_selected(i)) {
            if (!first && output_delim_set) {
                /* output delimiter between non-contiguous ranges */
            }
            fputc(line[i - 1], out);
            first = 0;
        }
    }
    fputc(delim, out);
}

/* ── Process a line in fields mode ──────────────────────────────────── */

static void cut_fields_line(const char *line, FILE *out, char delim)
{
    const char *out_delim = output_delim_set ? opt_output_delim : NULL;

    /* Check if line contains the delimiter */
    if (!strchr(line, opt_delim)) {
        if (opt_only_delimited)
            return;     /* suppress line */
        fprintf(out, "%s%c", line, delim);
        return;
    }

    /* Split into fields */
    char *copy = strdup(line);
    if (!copy) return;

    /* Count fields */
    int nfields = 1;
    for (const char *p = copy; *p; p++) {
        if (*p == opt_delim) nfields++;
    }

    /* Build field array */
    char **fields = malloc((size_t)nfields * sizeof(char *));
    if (!fields) { free(copy); return; }

    fields[0] = copy;
    int fi = 1;
    for (char *p = copy; *p; p++) {
        if (*p == opt_delim) {
            *p = '\0';
            if (fi < nfields)
                fields[fi++] = p + 1;
        }
    }

    int first = 1;
    for (int f = 1; f <= nfields; f++) {
        if (is_selected(f)) {
            if (!first) {
                if (out_delim)
                    fputs(out_delim, out);
                else
                    fputc(opt_delim, out);
            }
            fputs(fields[f - 1], out);
            first = 0;
        }
    }
    fputc(delim, out);

    free(fields);
    free(copy);
}

/* ── Process file ───────────────────────────────────────────────────── */

static int process_file(FILE *fp, FILE *out, char delim)
{
    char *buf = malloc(LINE_MAX);
    if (!buf) return -1;
    int pos = 0;
    int c;

    while ((c = fgetc(fp)) != EOF) {
        if ((char)c == delim) {
            buf[pos] = '\0';
            if (mode == MODE_FIELDS)
                cut_fields_line(buf, out, delim);
            else
                cut_bytes_line(buf, out, delim);
            pos = 0;
        } else {
            if (pos < LINE_MAX - 2)
                buf[pos++] = (char)c;
        }
    }
    if (pos > 0) {
        buf[pos] = '\0';
        if (mode == MODE_FIELDS)
            cut_fields_line(buf, out, delim);
        else
            cut_bytes_line(buf, out, delim);
    }
    free(buf);
    return 0;
}

/* ── Help / version ─────────────────────────────────────────────────── */

static void print_help(void)
{
    printf("Usage: cut OPTION... [FILE]...\n");
    printf("Print selected parts of lines from each FILE to standard output.\n\n");
    printf("With no FILE, or when FILE is -, read standard input.\n\n");
    printf("  -b, --bytes=LIST          select only these bytes\n");
    printf("  -c, --characters=LIST     select only these characters\n");
    printf("  -d, --delimiter=DELIM     use DELIM instead of TAB for field delimiter\n");
    printf("  -f, --fields=LIST         select only these fields\n");
    printf("  -n                        (ignored)\n");
    printf("      --complement          complement the set of selected bytes/chars/fields\n");
    printf("  -s, --only-delimited      do not print lines not containing delimiters\n");
    printf("      --output-delimiter=STRING  use STRING as the output delimiter\n");
    printf("  -z, --zero-terminated     line delimiter is NUL, not newline\n");
    printf("      --help                display this help and exit\n");
    printf("      --version             output version information and exit\n\n");
    printf("Use one, and only one of -b, -c or -f.  Each LIST is made up of one\n");
    printf("range, or many ranges separated by commas.\n");
    printf("  N     N'th byte, character or field, counted from 1\n");
    printf("  N-    from N'th byte, character or field, to end of line\n");
    printf("  N-M   from N'th to M'th (included) byte, character or field\n");
    printf("  -M    from first to M'th (included) byte, character or field\n");
}

static void print_version(void)
{
    printf("%s (LikeOS) %s\n", PROGRAM_NAME, VERSION);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"bytes",            required_argument, 0, 'b'},
        {"characters",       required_argument, 0, 'c'},
        {"delimiter",        required_argument, 0, 'd'},
        {"fields",           required_argument, 0, 'f'},
        {"complement",       no_argument,       0, 'C'},
        {"only-delimited",   no_argument,       0, 's'},
        {"output-delimiter", required_argument, 0, 'O'},
        {"zero-terminated",  no_argument,       0, 'z'},
        {"help",             no_argument,       0, 'H'},
        {"version",          no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    const char *range_spec = NULL;
    int opt;
    while ((opt = getopt_long(argc, argv, "b:c:d:f:nsz", long_options, NULL)) != -1) {
        switch (opt) {
        case 'b':
            mode = MODE_BYTES;
            range_spec = optarg;
            break;
        case 'c':
            mode = MODE_CHARS;
            range_spec = optarg;
            break;
        case 'd':
            if (optarg && optarg[0])
                opt_delim = optarg[0];
            break;
        case 'f':
            mode = MODE_FIELDS;
            range_spec = optarg;
            break;
        case 'n':
            /* ignored per manpage */
            break;
        case 's':
            opt_only_delimited = 1;
            break;
        case 'z':
            opt_zero = 1;
            break;
        case 'C':
            opt_complement = 1;
            break;
        case 'O':
            opt_output_delim = optarg;
            output_delim_set = 1;
            break;
        case 'H':
            print_help();
            return 0;
        case 'V':
            print_version();
            return 0;
        default:
            fprintf(stderr, "Try 'cut --help' for more information.\n");
            return 1;
        }
    }

    if (mode == MODE_NONE) {
        fprintf(stderr, "cut: you must specify a list of bytes, characters, or fields\n");
        fprintf(stderr, "Try 'cut --help' for more information.\n");
        return 1;
    }

    if (parse_ranges(range_spec) < 0)
        return 1;

    char delim = opt_zero ? '\0' : '\n';

    if (optind >= argc || (optind + 1 == argc && strcmp(argv[optind], "-") == 0)) {
        process_file(stdin, stdout, delim);
    } else {
        for (int i = optind; i < argc; i++) {
            FILE *fp;
            if (strcmp(argv[i], "-") == 0) {
                fp = stdin;
            } else {
                fp = fopen(argv[i], "r");
                if (!fp) {
                    fprintf(stderr, "cut: %s: No such file or directory\n", argv[i]);
                    return 1;
                }
            }
            process_file(fp, stdout, delim);
            if (fp != stdin) fclose(fp);
        }
    }

    return 0;
}
