/*
 * uniq - report or omit repeated lines
 *
 * Full implementation per uniq(1) manpage.
 * Supports: -c, -d, -D, -f, -i, -s, -u, -z, -w
 *           --all-repeated[=METHOD], --group[=METHOD]
 *           --help, --version
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>

#define PROGRAM_NAME "uniq"
#define VERSION      "1.0"

static int opt_count;               /* -c: prefix with count */
static int opt_repeated;            /* -d: only print duplicates (first of group) */
static int opt_all_repeated;        /* -D: print all duplicate lines */
static int opt_unique_only;         /* -u: only print unique lines */
static int opt_skip_fields;         /* -f N */
static int opt_skip_chars;          /* -s N */
static int opt_check_chars;         /* -w N (0 = unlimited) */
static int opt_ignore_case;         /* -i */
static int opt_zero;                /* -z */

/* --all-repeated=METHOD */
enum repeat_method { REP_NONE, REP_PREPEND, REP_SEPARATE };
static enum repeat_method opt_all_rep_method = REP_NONE;

/* --group=METHOD */
enum group_method { GRP_OFF, GRP_SEPARATE, GRP_PREPEND, GRP_APPEND, GRP_BOTH };
static enum group_method opt_group = GRP_OFF;

/* ── Skip fields and characters ─────────────────────────────────────── */

static const char *skip_fields_and_chars(const char *s)
{
    /* Skip N fields (runs of blanks + non-blanks) */
    for (int f = 0; f < opt_skip_fields && *s; f++) {
        while (*s == ' ' || *s == '\t') s++;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    /* Skip N characters */
    for (int c = 0; c < opt_skip_chars && *s; c++)
        s++;
    return s;
}

/* ── Case-insensitive compare helpers ───────────────────────────────── */

static int my_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int my_strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n && *a && *b; i++, a++, b++) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ── Compare two lines (after skipping fields/chars) ────────────────── */

static int lines_equal(const char *a, const char *b)
{
    const char *pa = skip_fields_and_chars(a);
    const char *pb = skip_fields_and_chars(b);

    if (opt_check_chars > 0) {
        if (opt_ignore_case)
            return my_strncasecmp(pa, pb, (size_t)opt_check_chars) == 0;
        else
            return strncmp(pa, pb, (size_t)opt_check_chars) == 0;
    }
    if (opt_ignore_case)
        return my_strcasecmp(pa, pb) == 0;
    return strcmp(pa, pb) == 0;
}

/* ── Help / version ─────────────────────────────────────────────────── */

static void print_help(void)
{
    printf("Usage: uniq [OPTION]... [INPUT [OUTPUT]]\n");
    printf("Filter adjacent matching lines from INPUT (or standard input),\n");
    printf("writing to OUTPUT (or standard output).\n\n");
    printf("  -c, --count            prefix lines by the number of occurrences\n");
    printf("  -d, --repeated         only print duplicate lines, one for each group\n");
    printf("  -D                     print all duplicate lines\n");
    printf("      --all-repeated[=METHOD]  like -D, but allow separating groups;\n");
    printf("                               METHOD={none(default),prepend,separate}\n");
    printf("  -f, --skip-fields=N    avoid comparing the first N fields\n");
    printf("      --group[=METHOD]   show all items, separating groups with an empty line;\n");
    printf("                         METHOD={separate(default),prepend,append,both}\n");
    printf("  -i, --ignore-case      ignore differences in case when comparing\n");
    printf("  -s, --skip-chars=N     avoid comparing the first N characters\n");
    printf("  -u, --unique           only print unique lines\n");
    printf("  -z, --zero-terminated  line delimiter is NUL, not newline\n");
    printf("  -w, --check-chars=N    compare no more than N characters in lines\n");
    printf("      --help             display this help and exit\n");
    printf("      --version          output version information and exit\n");
}

static void print_version(void)
{
    printf("%s (LikeOS) %s\n", PROGRAM_NAME, VERSION);
}

/* ── Line reading ───────────────────────────────────────────────────── */

static char *read_line(FILE *fp, char delim)
{
    int cap = 256;
    char *buf = malloc((size_t)cap);
    if (!buf) return NULL;
    int len = 0;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if ((char)c == delim) {
            buf[len] = '\0';
            return buf;
        }
        if (len >= cap - 1) {
            cap *= 2;
            char *tmp = realloc(buf, (size_t)cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)c;
    }
    if (len > 0) {
        buf[len] = '\0';
        return buf;
    }
    free(buf);
    return NULL;
}

/* ── Output a line with optional count prefix ───────────────────────── */

static void output_line(FILE *out, const char *line, int count, char delim)
{
    if (opt_count)
        fprintf(out, "%7d %s%c", count, line, delim);
    else
        fprintf(out, "%s%c", line, delim);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"count",         no_argument,       0, 'c'},
        {"repeated",      no_argument,       0, 'd'},
        {"all-repeated",  optional_argument, 0, 'D'},
        {"skip-fields",   required_argument, 0, 'f'},
        {"group",         optional_argument, 0, 'G'},
        {"ignore-case",   no_argument,       0, 'i'},
        {"skip-chars",    required_argument, 0, 's'},
        {"unique",        no_argument,       0, 'u'},
        {"zero-terminated", no_argument,     0, 'z'},
        {"check-chars",   required_argument, 0, 'w'},
        {"help",          no_argument,       0, 'H'},
        {"version",       no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "cdDf:is:uzw:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'c': opt_count = 1; break;
        case 'd': opt_repeated = 1; break;
        case 'D':
            opt_all_repeated = 1;
            if (optarg) {
                if (strcmp(optarg, "prepend") == 0)
                    opt_all_rep_method = REP_PREPEND;
                else if (strcmp(optarg, "separate") == 0)
                    opt_all_rep_method = REP_SEPARATE;
                else
                    opt_all_rep_method = REP_NONE;
            }
            break;
        case 'f': opt_skip_fields = atoi(optarg); break;
        case 'G':
            if (!optarg || strcmp(optarg, "separate") == 0)
                opt_group = GRP_SEPARATE;
            else if (strcmp(optarg, "prepend") == 0)
                opt_group = GRP_PREPEND;
            else if (strcmp(optarg, "append") == 0)
                opt_group = GRP_APPEND;
            else if (strcmp(optarg, "both") == 0)
                opt_group = GRP_BOTH;
            break;
        case 'i': opt_ignore_case = 1; break;
        case 's': opt_skip_chars = atoi(optarg); break;
        case 'u': opt_unique_only = 1; break;
        case 'z': opt_zero = 1; break;
        case 'w': opt_check_chars = atoi(optarg); break;
        case 'H': print_help(); return 0;
        case 'V': print_version(); return 0;
        default:
            fprintf(stderr, "Try 'uniq --help' for more information.\n");
            return 1;
        }
    }

    /* Open input/output files */
    FILE *in = stdin;
    FILE *out = stdout;

    if (optind < argc && strcmp(argv[optind], "-") != 0) {
        in = fopen(argv[optind], "r");
        if (!in) {
            fprintf(stderr, "uniq: %s: No such file or directory\n", argv[optind]);
            return 1;
        }
        optind++;
    }
    if (optind < argc && strcmp(argv[optind], "-") != 0) {
        out = fopen(argv[optind], "w");
        if (!out) {
            fprintf(stderr, "uniq: %s: cannot create\n", argv[optind]);
            return 1;
        }
    }

    char delim = opt_zero ? '\0' : '\n';

    /*
     * Read all lines into memory for --group and --all-repeated modes.
     * For simple modes, process line-by-line to save memory.
     */
    if (opt_group != GRP_OFF || opt_all_repeated) {
        /* Read all lines */
        char **all_lines = NULL;
        int nlines = 0, cap = 0;
        char *l;
        while ((l = read_line(in, delim)) != NULL) {
            if (nlines >= cap) {
                cap = cap ? cap * 2 : 1024;
                all_lines = realloc(all_lines, (size_t)cap * sizeof(char *));
            }
            all_lines[nlines++] = l;
        }

        if (opt_group != GRP_OFF) {
            /* --group mode: print all lines, separate groups with blank lines */
            int first_group = 1;
            for (int i = 0; i < nlines; i++) {
                int is_new_group = (i == 0 || !lines_equal(all_lines[i], all_lines[i - 1]));
                if (is_new_group && !first_group) {
                    if (opt_group == GRP_SEPARATE || opt_group == GRP_BOTH ||
                        opt_group == GRP_PREPEND)
                        fputc(delim, out);
                }
                if (is_new_group && first_group) {
                    if (opt_group == GRP_PREPEND || opt_group == GRP_BOTH)
                        fputc(delim, out);
                    first_group = 0;
                }
                fprintf(out, "%s%c", all_lines[i], delim);
                /* Check if this is the last of its group */
                int is_last = (i == nlines - 1 || !lines_equal(all_lines[i], all_lines[i + 1]));
                if (is_last && (opt_group == GRP_APPEND || opt_group == GRP_BOTH))
                    fputc(delim, out);
            }
        } else {
            /* --all-repeated (-D) mode */
            int first_group = 1;
            for (int i = 0; i < nlines; ) {
                /* Count group size */
                int cnt = 1;
                while (i + cnt < nlines && lines_equal(all_lines[i], all_lines[i + cnt]))
                    cnt++;

                if (cnt > 1) {
                    /* Duplicate group */
                    if (!first_group && opt_all_rep_method == REP_SEPARATE)
                        fputc(delim, out);
                    if (opt_all_rep_method == REP_PREPEND)
                        fputc(delim, out);
                    for (int j = 0; j < cnt; j++)
                        fprintf(out, "%s%c", all_lines[i + j], delim);
                    first_group = 0;
                }
                i += cnt;
            }
        }

        for (int i = 0; i < nlines; i++) free(all_lines[i]);
        free(all_lines);
    } else {
        /* Standard mode: line-by-line processing */
        char *prev = read_line(in, delim);
        if (!prev) goto done;
        int count = 1;

        char *cur;
        while ((cur = read_line(in, delim)) != NULL) {
            if (lines_equal(prev, cur)) {
                count++;
                free(cur);
            } else {
                /* Output the previous group */
                if (opt_unique_only) {
                    if (count == 1)
                        output_line(out, prev, count, delim);
                } else if (opt_repeated) {
                    if (count > 1)
                        output_line(out, prev, count, delim);
                } else {
                    output_line(out, prev, count, delim);
                }
                free(prev);
                prev = cur;
                count = 1;
            }
        }

        /* Output the last group */
        if (prev) {
            if (opt_unique_only) {
                if (count == 1)
                    output_line(out, prev, count, delim);
            } else if (opt_repeated) {
                if (count > 1)
                    output_line(out, prev, count, delim);
            } else {
                output_line(out, prev, count, delim);
            }
            free(prev);
        }
    }

done:
    if (in != stdin) fclose(in);
    if (out != stdout) fclose(out);
    return 0;
}
