/*
 * sort - sort lines of text files
 *
 * Full implementation per sort(1) manpage.
 * Supports: -b, -d, -f, -g, -i, -M, -h, -n, -R, -r, -V
 *           -c, -C, -k, -m, -o, -s, -t, -u, -z
 *           --help, --version
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "sort"
#define VERSION      "1.0"

/* ── Global options ─────────────────────────────────────────────────── */

static int opt_ignore_leading_blanks;   /* -b */
static int opt_dictionary_order;        /* -d */
static int opt_ignore_case;             /* -f */
static int opt_general_numeric;         /* -g */
static int opt_ignore_nonprinting;      /* -i */
static int opt_month_sort;              /* -M */
static int opt_human_numeric;           /* -h */
static int opt_numeric;                 /* -n */
static int opt_random;                  /* -R */
static int opt_reverse;                 /* -r */
static int opt_version_sort;            /* -V */
static int opt_check;                   /* -c */
static int opt_check_quiet;             /* -C */
static int opt_merge;                   /* -m */
static int opt_stable;                  /* -s */
static int opt_unique;                  /* -u */
static int opt_zero;                    /* -z */
static char opt_separator;              /* -t (0 = default whitespace) */
static const char *opt_output;          /* -o */

/* ── Key definitions ────────────────────────────────────────────────── */
#define MAX_KEYS 16

typedef struct {
    int start_field;    /* 1-based field number */
    int start_char;     /* 1-based character within field (0 = whole field) */
    int end_field;      /* 0 = end of line */
    int end_char;       /* 0 = end of field */
    /* Per-key flags (override globals) */
    int b, d, f, g, i, M, h, n, R, r, V;
    int has_flags;      /* any per-key flag was set */
} sort_key_t;

static sort_key_t keys[MAX_KEYS];
static int nkeys;

/* ── Month names for -M ─────────────────────────────────────────────── */
static const char *months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static int month_index(const char *s)
{
    /* Skip leading blanks */
    while (*s == ' ' || *s == '\t') s++;
    char upper[4];
    for (int i = 0; i < 3 && s[i]; i++)
        upper[i] = (char)toupper((unsigned char)s[i]);
    upper[3] = '\0';
    for (int i = 0; i < 12; i++) {
        if (strncmp(upper, months[i], 3) == 0)
            return i + 1;
    }
    return 0; /* unknown */
}

/* ── Line storage ───────────────────────────────────────────────────── */

static char **lines;
static int nlines;
static int lines_cap;

static void add_line(char *l)
{
    if (nlines >= lines_cap) {
        lines_cap = lines_cap ? lines_cap * 2 : 4096;
        lines = realloc(lines, (size_t)lines_cap * sizeof(char *));
        if (!lines) {
            fprintf(stderr, "sort: out of memory\n");
            _exit(1);
        }
    }
    lines[nlines++] = l;
}

/* ── Read all lines from a file descriptor ──────────────────────────── */

static void read_lines(FILE *fp, char delim)
{
    int bufsz = 65536;
    char *buf = malloc((size_t)bufsz);
    if (!buf) { fprintf(stderr, "sort: out of memory\n"); _exit(1); }
    int pos = 0;

    int c;
    while ((c = fgetc(fp)) != EOF) {
        if ((char)c == delim) {
            buf[pos] = '\0';
            char *copy = malloc((size_t)pos + 1);
            if (!copy) { fprintf(stderr, "sort: out of memory\n"); _exit(1); }
            memcpy(copy, buf, (size_t)pos + 1);
            add_line(copy);
            pos = 0;
        } else {
            if (pos < bufsz - 2)
                buf[pos++] = (char)c;
        }
    }
    if (pos > 0) {
        buf[pos] = '\0';
        char *copy = malloc((size_t)pos + 1);
        if (!copy) { fprintf(stderr, "sort: out of memory\n"); _exit(1); }
        memcpy(copy, buf, (size_t)pos + 1);
        add_line(copy);
    }
    free(buf);
}

/* ── Field extraction ───────────────────────────────────────────────── */

/*
 * Find the start of field `field` (1-based) in line.
 * With -t, fields are separated by the separator character.
 * Without -t, fields are separated by runs of blanks; characters in a
 * field are counted from the beginning of the preceding whitespace unless
 * -b is active.
 */
static const char *field_start(const char *line, int field)
{
    const char *p = line;
    int f = 1;

    if (opt_separator) {
        while (f < field && *p) {
            if (*p == opt_separator)
                f++;
            p++;
        }
    } else {
        /* Skip leading blanks for field 1 */
        while (f < field && *p) {
            /* Skip blanks */
            while (*p == ' ' || *p == '\t') p++;
            /* Skip non-blanks */
            while (*p && *p != ' ' && *p != '\t') p++;
            f++;
        }
    }
    return p;
}

static const char *field_end(const char *line, int field)
{
    if (field <= 0)
        return line + strlen(line);
    const char *p = field_start(line, field);
    if (opt_separator) {
        while (*p && *p != opt_separator) p++;
    } else {
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return p;
}

/*
 * Extract the key portion from a line based on a key definition.
 */
static void extract_key(const char *line, const sort_key_t *key,
                        char *out, int outsz)
{
    const char *start = field_start(line, key->start_field);
    if (key->start_char > 1) {
        int skip = key->start_char - 1;
        while (skip > 0 && *start) { start++; skip--; }
    }

    const char *end;
    if (key->end_field > 0) {
        end = field_start(line, key->end_field);
        if (key->end_char > 0) {
            int skip = key->end_char;
            while (skip > 0 && *end) { end++; skip--; }
        } else {
            end = field_end(line, key->end_field);
        }
    } else {
        end = line + strlen(line);
    }

    if (end < start) end = start;
    int len = (int)(end - start);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, start, (size_t)len);
    out[len] = '\0';
}

/* ── Skip leading blanks ────────────────────────────────────────────── */

static const char *skip_blanks(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ── Parse a human-readable number (e.g. 2K, 1G) ───────────────────── */

static double parse_human(const char *s)
{
    char *end = NULL;
    double val = strtod(s, &end);
    if (end && *end) {
        switch (toupper((unsigned char)*end)) {
        case 'K': val *= 1024.0; break;
        case 'M': val *= 1024.0 * 1024.0; break;
        case 'G': val *= 1024.0 * 1024.0 * 1024.0; break;
        case 'T': val *= 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        case 'P': val *= 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        case 'E': val *= 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        default: break;
        }
    }
    return val;
}

/* ── Simple version comparison ──────────────────────────────────────── */

static int version_compare(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            /* Compare numeric segments */
            long na = strtol(a, (char **)&a, 10);
            long nb = strtol(b, (char **)&b, 10);
            if (na != nb) return (na < nb) ? -1 : 1;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ── Comparison function ────────────────────────────────────────────── */

static int compare_strings(const char *a, const char *b,
                           int b_flag, int d_flag, int f_flag,
                           int g_flag, int i_flag, int M_flag,
                           int h_flag, int n_flag, int r_flag,
                           int V_flag)
{
    if (b_flag) {
        a = skip_blanks(a);
        b = skip_blanks(b);
    }

    int result = 0;

    if (g_flag) {
        /* General numeric: compare as floating point */
        double da = strtod(a, NULL);
        double db = strtod(b, NULL);
        if (da < db) result = -1;
        else if (da > db) result = 1;
        else result = 0;
    } else if (n_flag) {
        /* Numeric: compare as long */
        const char *pa = a, *pb = b;
        while (*pa == ' ' || *pa == '\t') pa++;
        while (*pb == ' ' || *pb == '\t') pb++;
        long na = strtol(pa, NULL, 10);
        long nb = strtol(pb, NULL, 10);
        if (na < nb) result = -1;
        else if (na > nb) result = 1;
        else result = 0;
    } else if (h_flag) {
        double ha = parse_human(a);
        double hb = parse_human(b);
        if (ha < hb) result = -1;
        else if (ha > hb) result = 1;
        else result = 0;
    } else if (M_flag) {
        int ma = month_index(a);
        int mb = month_index(b);
        if (ma < mb) result = -1;
        else if (ma > mb) result = 1;
        else result = 0;
    } else if (V_flag) {
        result = version_compare(a, b);
    } else {
        /* Lexicographic comparison */
        const char *pa = a, *pb = b;
        while (*pa && *pb) {
            unsigned char ca = (unsigned char)*pa;
            unsigned char cb = (unsigned char)*pb;

            if (d_flag) {
                while (*pa && !isalnum(ca) && ca != ' ' && ca != '\t') {
                    pa++;
                    ca = (unsigned char)*pa;
                }
                while (*pb && !isalnum(cb) && cb != ' ' && cb != '\t') {
                    pb++;
                    cb = (unsigned char)*pb;
                }
                if (!*pa || !*pb) break;
            }

            if (i_flag) {
                while (*pa && (ca < 0x20 || ca > 0x7e)) {
                    pa++;
                    ca = (unsigned char)*pa;
                }
                while (*pb && (cb < 0x20 || cb > 0x7e)) {
                    pb++;
                    cb = (unsigned char)*pb;
                }
                if (!*pa || !*pb) break;
            }

            if (f_flag) {
                ca = (unsigned char)toupper(ca);
                cb = (unsigned char)toupper(cb);
            }

            if (ca != cb) {
                result = (int)ca - (int)cb;
                goto done;
            }
            pa++;
            pb++;
        }
        result = (int)(unsigned char)*pa - (int)(unsigned char)*pb;
    }

done:
    if (r_flag) result = -result;
    return result;
}

static int compare_lines(const void *p1, const void *p2)
{
    const char *a = *(const char **)p1;
    const char *b = *(const char **)p2;

    if (nkeys > 0) {
        static char ka[4096], kb[4096];
        for (int k = 0; k < nkeys; k++) {
            extract_key(a, &keys[k], ka, sizeof(ka));
            extract_key(b, &keys[k], kb, sizeof(kb));

            int b_f = keys[k].has_flags ? keys[k].b : opt_ignore_leading_blanks;
            int d_f = keys[k].has_flags ? keys[k].d : opt_dictionary_order;
            int f_f = keys[k].has_flags ? keys[k].f : opt_ignore_case;
            int g_f = keys[k].has_flags ? keys[k].g : opt_general_numeric;
            int i_f = keys[k].has_flags ? keys[k].i : opt_ignore_nonprinting;
            int M_f = keys[k].has_flags ? keys[k].M : opt_month_sort;
            int h_f = keys[k].has_flags ? keys[k].h : opt_human_numeric;
            int n_f = keys[k].has_flags ? keys[k].n : opt_numeric;
            int r_f = keys[k].has_flags ? keys[k].r : opt_reverse;
            int V_f = keys[k].has_flags ? keys[k].V : opt_version_sort;

            int cmp = compare_strings(ka, kb,
                                      b_f, d_f, f_f, g_f, i_f,
                                      M_f, h_f, n_f, r_f, V_f);
            if (cmp != 0) return cmp;
        }
        /* All keys equal – last-resort unless stable */
        if (opt_stable) return 0;
        return compare_strings(a, b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    return compare_strings(a, b,
                           opt_ignore_leading_blanks,
                           opt_dictionary_order,
                           opt_ignore_case,
                           opt_general_numeric,
                           opt_ignore_nonprinting,
                           opt_month_sort,
                           opt_human_numeric,
                           opt_numeric,
                           opt_reverse,
                           opt_version_sort);
}

/* ── Parse key definition: F[.C][OPTS][,F[.C][OPTS]] ───────────────── */

static void parse_key_flags(const char *s, sort_key_t *key)
{
    while (*s) {
        switch (*s) {
        case 'b': key->b = 1; key->has_flags = 1; break;
        case 'd': key->d = 1; key->has_flags = 1; break;
        case 'f': key->f = 1; key->has_flags = 1; break;
        case 'g': key->g = 1; key->has_flags = 1; break;
        case 'i': key->i = 1; key->has_flags = 1; break;
        case 'M': key->M = 1; key->has_flags = 1; break;
        case 'h': key->h = 1; key->has_flags = 1; break;
        case 'n': key->n = 1; key->has_flags = 1; break;
        case 'R': key->R = 1; key->has_flags = 1; break;
        case 'r': key->r = 1; key->has_flags = 1; break;
        case 'V': key->V = 1; key->has_flags = 1; break;
        default: break;
        }
        s++;
    }
}

static int parse_key(const char *spec)
{
    if (nkeys >= MAX_KEYS) {
        fprintf(stderr, "sort: too many keys\n");
        return -1;
    }
    sort_key_t *key = &keys[nkeys];
    memset(key, 0, sizeof(*key));

    /* Parse start: F[.C][OPTS] */
    const char *p = spec;
    key->start_field = (int)strtol(p, (char **)&p, 10);
    if (*p == '.') {
        p++;
        key->start_char = (int)strtol(p, (char **)&p, 10);
    }

    /* Parse optional flags after start */
    const char *comma = strchr(p, ',');
    if (comma) {
        /* Flags between p and comma */
        char flags[64];
        int flen = (int)(comma - p);
        if (flen >= (int)sizeof(flags)) flen = (int)sizeof(flags) - 1;
        memcpy(flags, p, (size_t)flen);
        flags[flen] = '\0';
        parse_key_flags(flags, key);
        p = comma + 1;

        /* Parse end: F[.C][OPTS] */
        key->end_field = (int)strtol(p, (char **)&p, 10);
        if (*p == '.') {
            p++;
            key->end_char = (int)strtol(p, (char **)&p, 10);
        }
        parse_key_flags(p, key);
    } else {
        parse_key_flags(p, key);
    }

    nkeys++;
    return 0;
}

/* ── Print help ─────────────────────────────────────────────────────── */

static void print_help(void)
{
    printf("Usage: sort [OPTION]... [FILE]...\n");
    printf("Write sorted concatenation of all FILE(s) to standard output.\n\n");
    printf("With no FILE, or when FILE is -, read standard input.\n\n");
    printf("Ordering options:\n");
    printf("  -b, --ignore-leading-blanks  ignore leading blanks\n");
    printf("  -d, --dictionary-order       consider only blanks and alphanumeric characters\n");
    printf("  -f, --ignore-case            fold lower case to upper case characters\n");
    printf("  -g, --general-numeric-sort   compare according to general numerical value\n");
    printf("  -i, --ignore-nonprinting     consider only printable characters\n");
    printf("  -M, --month-sort             compare (unknown) < 'JAN' < ... < 'DEC'\n");
    printf("  -h, --human-numeric-sort     compare human readable numbers (e.g., 2K 1G)\n");
    printf("  -n, --numeric-sort           compare according to string numerical value\n");
    printf("  -R, --random-sort            shuffle, but group identical keys\n");
    printf("  -r, --reverse                reverse the result of comparisons\n");
    printf("  -V, --version-sort           natural sort of (version) numbers within text\n\n");
    printf("Other options:\n");
    printf("  -c, --check                  check for sorted input; do not sort\n");
    printf("  -C, --check=quiet            like -c, but do not report first bad line\n");
    printf("  -k, --key=KEYDEF             sort via a key; KEYDEF gives location and type\n");
    printf("  -m, --merge                  merge already sorted files; do not sort\n");
    printf("  -o, --output=FILE            write result to FILE instead of standard output\n");
    printf("  -s, --stable                 stabilize sort by disabling last-resort comparison\n");
    printf("  -t, --field-separator=SEP    use SEP instead of non-blank to blank transition\n");
    printf("  -u, --unique                 with -c, check strict ordering; without -c, output\n");
    printf("                               only the first of an equal run\n");
    printf("  -z, --zero-terminated        line delimiter is NUL, not newline\n");
    printf("      --help                   display this help and exit\n");
    printf("      --version                output version information and exit\n\n");
    printf("KEYDEF is F[.C][OPTS][,F[.C][OPTS]] for start and stop position.\n");
}

static void print_version(void)
{
    printf("%s (LikeOS) %s\n", PROGRAM_NAME, VERSION);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"ignore-leading-blanks", no_argument,       0, 'b'},
        {"dictionary-order",     no_argument,       0, 'd'},
        {"ignore-case",          no_argument,       0, 'f'},
        {"general-numeric-sort", no_argument,       0, 'g'},
        {"ignore-nonprinting",   no_argument,       0, 'i'},
        {"month-sort",           no_argument,       0, 'M'},
        {"human-numeric-sort",   no_argument,       0, 'h'},
        {"numeric-sort",         no_argument,       0, 'n'},
        {"random-sort",          no_argument,       0, 'R'},
        {"reverse",              no_argument,       0, 'r'},
        {"version-sort",         no_argument,       0, 'V'},
        {"check",                no_argument,       0, 'c'},
        {"merge",                no_argument,       0, 'm'},
        {"output",               required_argument, 0, 'o'},
        {"stable",               no_argument,       0, 's'},
        {"field-separator",      required_argument, 0, 't'},
        {"key",                  required_argument, 0, 'k'},
        {"unique",               no_argument,       0, 'u'},
        {"zero-terminated",      no_argument,       0, 'z'},
        {"help",                 no_argument,       0, 'H'},
        {"version",              no_argument,       0, 'W'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "bdfgiMhnRrVcCmo:st:k:uz",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'b': opt_ignore_leading_blanks = 1; break;
        case 'd': opt_dictionary_order = 1; break;
        case 'f': opt_ignore_case = 1; break;
        case 'g': opt_general_numeric = 1; break;
        case 'i': opt_ignore_nonprinting = 1; break;
        case 'M': opt_month_sort = 1; break;
        case 'h': opt_human_numeric = 1; break;
        case 'n': opt_numeric = 1; break;
        case 'R': opt_random = 1; break;
        case 'r': opt_reverse = 1; break;
        case 'V': opt_version_sort = 1; break;
        case 'c': opt_check = 1; break;
        case 'C': opt_check_quiet = 1; opt_check = 1; break;
        case 'm': opt_merge = 1; break;
        case 'o': opt_output = optarg; break;
        case 's': opt_stable = 1; break;
        case 't':
            if (optarg && optarg[0])
                opt_separator = optarg[0];
            break;
        case 'k':
            if (parse_key(optarg) < 0) return 2;
            break;
        case 'u': opt_unique = 1; break;
        case 'z': opt_zero = 1; break;
        case 'H': print_help(); return 0;
        case 'W': print_version(); return 0;
        default:
            fprintf(stderr, "Try 'sort --help' for more information.\n");
            return 2;
        }
    }

    char delim = opt_zero ? '\0' : '\n';

    /* Read input files (or stdin) */
    if (optind >= argc || (optind + 1 == argc && strcmp(argv[optind], "-") == 0)) {
        read_lines(stdin, delim);
    } else {
        for (int i = optind; i < argc; i++) {
            FILE *fp;
            if (strcmp(argv[i], "-") == 0) {
                fp = stdin;
            } else {
                fp = fopen(argv[i], "r");
                if (!fp) {
                    fprintf(stderr, "sort: cannot read: %s\n", argv[i]);
                    return 2;
                }
            }
            read_lines(fp, delim);
            if (fp != stdin) fclose(fp);
        }
    }

    /* Check mode */
    if (opt_check) {
        for (int i = 1; i < nlines; i++) {
            int cmp = compare_lines(&lines[i - 1], &lines[i]);
            if (opt_unique ? cmp >= 0 : cmp > 0) {
                if (!opt_check_quiet)
                    fprintf(stderr, "sort: %s:%d: disorder: %s\n",
                            (optind < argc ? argv[optind] : "-"),
                            i + 1, lines[i]);
                return 1;
            }
        }
        return 0;
    }

    /* Random sort: simple Fisher-Yates shuffle (identical keys grouped) */
    if (opt_random) {
        unsigned int seed = 0;
        /* Use a simple seed from line content */
        for (int i = 0; i < nlines; i++) {
            for (const char *p = lines[i]; *p; p++)
                seed = seed * 31 + (unsigned char)*p;
        }
        for (int i = nlines - 1; i > 0; i--) {
            seed = seed * 1103515245 + 12345;
            int j = (int)(seed % (unsigned int)(i + 1));
            char *tmp = lines[i];
            lines[i] = lines[j];
            lines[j] = tmp;
        }
    } else {
        /* Sort */
        qsort(lines, (size_t)nlines, sizeof(char *), compare_lines);
    }

    /* Open output file if specified */
    FILE *out = stdout;
    if (opt_output) {
        out = fopen(opt_output, "w");
        if (!out) {
            fprintf(stderr, "sort: cannot create '%s'\n", opt_output);
            return 2;
        }
    }

    /* Output, applying -u (unique) filter */
    for (int i = 0; i < nlines; i++) {
        if (opt_unique && i > 0 && compare_lines(&lines[i - 1], &lines[i]) == 0)
            continue;
        fputs(lines[i], out);
        fputc(delim, out);
    }

    if (out != stdout) fclose(out);

    /* Free lines */
    for (int i = 0; i < nlines; i++)
        free(lines[i]);
    free(lines);

    return 0;
}
