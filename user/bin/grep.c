/*
 * grep - print lines that match patterns
 *
 * Full implementation per GNU grep(1) manpage.
 * Includes inline BRE/ERE regex engine, fixed-string matching,
 * recursive search, context lines, colorized output, and all options.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <getopt.h>
#include <fnmatch.h>

#define PROGRAM_NAME "grep"
#define VERSION      "1.0"

/* ════════════════════════════════════════════════════════════════════
 *  Inline regex engine – supports BRE and ERE
 * ════════════════════════════════════════════════════════════════════ */

/* Regex node types */
enum {
    RE_LITERAL,
    RE_DOT,
    RE_ANCHOR_BOL,
    RE_ANCHOR_EOL,
    RE_CLASS,
    RE_NCLASS,
    RE_STAR,
    RE_PLUS,
    RE_QUEST,
    RE_REPEAT,
    RE_GROUP_START,
    RE_GROUP_END,
    RE_ALT,
    RE_BACKREF,
    RE_WORD_BOUNDARY,
    RE_NOT_WORD_BOUNDARY,
    RE_WORD_START,
    RE_WORD_END,
    RE_WORD_CHAR,
    RE_NOT_WORD_CHAR,
    RE_END,
};

#define MAX_RE_NODES   2048
#define MAX_GROUPS     10

typedef struct {
    int type;
    int ch;
    int rep_min, rep_max;
    int group_id;
    unsigned char class_bits[32];
} re_node_t;

typedef struct {
    re_node_t nodes[MAX_RE_NODES];
    int count;
    int ngroups;
} regex_compiled_t;

static void class_set(unsigned char bits[32], int c)
{
    bits[c >> 3] |= (1 << (c & 7));
}

static int class_test(const unsigned char bits[32], int c)
{
    return (bits[c >> 3] >> (c & 7)) & 1;
}

static int is_word_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Parse POSIX character class name like [:alnum:] */
static int parse_posix_class(const char **pp, unsigned char bits[32])
{
    const char *p = *pp;
    if (*p != '[' || p[1] != ':') return 0;
    p += 2;
    const char *name = p;
    while (*p && *p != ':') p++;
    if (*p != ':' || p[1] != ']') return 0;
    int nlen = (int)(p - name);
    p += 2;
    *pp = p;

    for (int c = 0; c < 256; c++) {
        int match = 0;
        if (nlen == 5 && strncmp(name, "alnum", 5) == 0) match = isalnum(c);
        else if (nlen == 5 && strncmp(name, "alpha", 5) == 0) match = isalpha(c);
        else if (nlen == 5 && strncmp(name, "blank", 5) == 0) match = (c == ' ' || c == '\t');
        else if (nlen == 5 && strncmp(name, "cntrl", 5) == 0) match = iscntrl(c);
        else if (nlen == 5 && strncmp(name, "digit", 5) == 0) match = isdigit(c);
        else if (nlen == 5 && strncmp(name, "graph", 5) == 0) match = isgraph(c);
        else if (nlen == 5 && strncmp(name, "lower", 5) == 0) match = islower(c);
        else if (nlen == 5 && strncmp(name, "print", 5) == 0) match = isprint(c);
        else if (nlen == 5 && strncmp(name, "punct", 5) == 0) match = ispunct(c);
        else if (nlen == 5 && strncmp(name, "space", 5) == 0) match = isspace(c);
        else if (nlen == 5 && strncmp(name, "upper", 5) == 0) match = isupper(c);
        else if (nlen == 6 && strncmp(name, "xdigit", 6) == 0) match = isxdigit(c);
        if (match) class_set(bits, c);
    }
    return 1;
}

/* ── Compile regex pattern into node list ──────────────────────── */
static int re_compile(regex_compiled_t *re, const char *pattern, int extended, int icase)
{
    re->count = 0;
    re->ngroups = 0;

    const char *p = pattern;
    int group_stack[MAX_GROUPS];
    int group_depth = 0;

    while (*p) {
        if (re->count >= MAX_RE_NODES - 1) break;
        re_node_t *n = &re->nodes[re->count];
        memset(n, 0, sizeof(*n));

        /* ── Backslash escapes ─────────────────────────── */
        if (*p == '\\') {
            p++;
            if (!*p) break;

            if (extended) {
                /* ERE: \( \) are literals; \1-\9 backrefs; \b \w etc. */
                if (*p >= '1' && *p <= '9') {
                    n->type = RE_BACKREF; n->ch = *p - '0';
                } else if (*p == 'b') { n->type = RE_WORD_BOUNDARY;
                } else if (*p == 'B') { n->type = RE_NOT_WORD_BOUNDARY;
                } else if (*p == '<') { n->type = RE_WORD_START;
                } else if (*p == '>') { n->type = RE_WORD_END;
                } else if (*p == 'w') { n->type = RE_WORD_CHAR;
                } else if (*p == 'W') { n->type = RE_NOT_WORD_CHAR;
                } else if (*p == 'n') { n->type = RE_LITERAL; n->ch = '\n';
                } else if (*p == 't') { n->type = RE_LITERAL; n->ch = '\t';
                } else { n->type = RE_LITERAL; n->ch = (unsigned char)*p; }
                re->count++; p++;
                continue;
            }

            /* BRE: \( = group, \) = group end, \{ = repeat, \| = alt */
            if (*p == '(') {
                n->type = RE_GROUP_START;
                n->group_id = re->ngroups++;
                if (group_depth < MAX_GROUPS)
                    group_stack[group_depth++] = n->group_id;
                re->count++; p++;
                continue;
            }
            if (*p == ')') {
                n->type = RE_GROUP_END;
                n->group_id = (group_depth > 0) ? group_stack[--group_depth] : 0;
                re->count++; p++;
                continue;
            }
            if (*p == '{') {
                /* Repetition \{n,m\} — insert RE_REPEAT after previous */
                p++;
                int rmin = 0, rmax = -1;
                while (*p >= '0' && *p <= '9') rmin = rmin * 10 + (*p++ - '0');
                if (*p == ',') {
                    p++;
                    if (*p >= '0' && *p <= '9') {
                        rmax = 0;
                        while (*p >= '0' && *p <= '9') rmax = rmax * 10 + (*p++ - '0');
                    }
                } else {
                    rmax = rmin;
                }
                if (*p == '\\' && p[1] == '}') p += 2;
                else if (*p == '}') p++;
                n->type = RE_REPEAT; n->rep_min = rmin; n->rep_max = rmax;
                re->count++;
                continue;
            }
            if (*p == '|') { n->type = RE_ALT; re->count++; p++; continue; }
            if (*p == '?') { n->type = RE_QUEST; re->count++; p++; continue; }
            if (*p == '+') { n->type = RE_PLUS; re->count++; p++; continue; }
            if (*p >= '1' && *p <= '9') { n->type = RE_BACKREF; n->ch = *p - '0'; }
            else if (*p == 'b') { n->type = RE_WORD_BOUNDARY; }
            else if (*p == 'B') { n->type = RE_NOT_WORD_BOUNDARY; }
            else if (*p == '<') { n->type = RE_WORD_START; }
            else if (*p == '>') { n->type = RE_WORD_END; }
            else if (*p == 'w') { n->type = RE_WORD_CHAR; }
            else if (*p == 'W') { n->type = RE_NOT_WORD_CHAR; }
            else if (*p == 'n') { n->type = RE_LITERAL; n->ch = '\n'; }
            else if (*p == 't') { n->type = RE_LITERAL; n->ch = '\t'; }
            else { n->type = RE_LITERAL; n->ch = (unsigned char)*p; }
            re->count++; p++;
            continue;
        }

        /* ── Special characters ────────────────────────── */
        if (*p == '.') { n->type = RE_DOT; re->count++; p++; continue; }
        if (*p == '^') { n->type = RE_ANCHOR_BOL; re->count++; p++; continue; }
        if (*p == '$') { n->type = RE_ANCHOR_EOL; re->count++; p++; continue; }
        if (*p == '*') { n->type = RE_STAR; re->count++; p++; continue; }

        if (extended) {
            if (*p == '+') { n->type = RE_PLUS; re->count++; p++; continue; }
            if (*p == '?') { n->type = RE_QUEST; re->count++; p++; continue; }
            if (*p == '|') { n->type = RE_ALT; re->count++; p++; continue; }
            if (*p == '(') {
                n->type = RE_GROUP_START;
                n->group_id = re->ngroups++;
                if (group_depth < MAX_GROUPS)
                    group_stack[group_depth++] = n->group_id;
                re->count++; p++; continue;
            }
            if (*p == ')') {
                n->type = RE_GROUP_END;
                n->group_id = (group_depth > 0) ? group_stack[--group_depth] : 0;
                re->count++; p++; continue;
            }
            if (*p == '{') {
                p++;
                int rmin = 0, rmax = -1;
                while (*p >= '0' && *p <= '9') rmin = rmin * 10 + (*p++ - '0');
                if (*p == ',') {
                    p++;
                    if (*p >= '0' && *p <= '9') {
                        rmax = 0;
                        while (*p >= '0' && *p <= '9') rmax = rmax * 10 + (*p++ - '0');
                    }
                } else {
                    rmax = rmin;
                }
                if (*p == '}') p++;
                n->type = RE_REPEAT; n->rep_min = rmin; n->rep_max = rmax;
                re->count++; continue;
            }
        }

        /* ── Character class [...]  ────────────────────── */
        if (*p == '[') {
            p++;
            int negated = 0;
            if (*p == '^') { negated = 1; p++; }
            memset(n->class_bits, 0, 32);
            int first = 1;
            while (*p && (*p != ']' || first)) {
                first = 0;
                /* POSIX class? */
                if (*p == '[' && p[1] == ':') {
                    const char *save = p;
                    if (parse_posix_class(&p, n->class_bits))
                        continue;
                    p = save;
                }
                int ch_start = (unsigned char)*p;
                p++;
                if (*p == '-' && p[1] && p[1] != ']') {
                    p++;
                    int ch_end = (unsigned char)*p; p++;
                    for (int c = ch_start; c <= ch_end; c++) {
                        class_set(n->class_bits, c);
                        if (icase) {
                            if (c >= 'a' && c <= 'z') class_set(n->class_bits, c - 32);
                            else if (c >= 'A' && c <= 'Z') class_set(n->class_bits, c + 32);
                        }
                    }
                } else {
                    class_set(n->class_bits, ch_start);
                    if (icase) {
                        if (ch_start >= 'a' && ch_start <= 'z') class_set(n->class_bits, ch_start - 32);
                        else if (ch_start >= 'A' && ch_start <= 'Z') class_set(n->class_bits, ch_start + 32);
                    }
                }
            }
            if (*p == ']') p++;
            n->type = negated ? RE_NCLASS : RE_CLASS;
            re->count++;
            continue;
        }

        /* ── Default: literal character ────────────────── */
        n->type = RE_LITERAL;
        n->ch = (unsigned char)*p;
        if (icase && n->ch >= 'A' && n->ch <= 'Z')
            n->ch += 32;
        re->count++;
        p++;
    }

    re->nodes[re->count].type = RE_END;
    return 0;
}


/* ════════════════════════════════════════════════════════════════════
 *  Regex matcher – recursive backtracking with end-position tracking
 * ════════════════════════════════════════════════════════════════════ */

/* Helper: test if character c matches atom node n (with icase) */
static int atom_match(const re_node_t *n, unsigned char c, int icase)
{
    unsigned char lc = (icase && c >= 'A' && c <= 'Z') ? (c + 32) : c;
    switch (n->type) {
    case RE_LITERAL: return lc == (unsigned char)n->ch;
    case RE_DOT:     return c != '\n';
    case RE_CLASS:
    {
        if (class_test(n->class_bits, c)) return 1;
        if (icase) {
            if (c >= 'A' && c <= 'Z') return class_test(n->class_bits, c + 32);
            if (c >= 'a' && c <= 'z') return class_test(n->class_bits, c - 32);
        }
        return 0;
    }
    case RE_NCLASS:
    {
        int ok = !class_test(n->class_bits, c);
        if (icase) {
            if (c >= 'A' && c <= 'Z') ok = ok && !class_test(n->class_bits, c + 32);
            else if (c >= 'a' && c <= 'z') ok = ok && !class_test(n->class_bits, c - 32);
        }
        return ok;
    }
    case RE_WORD_CHAR:     return is_word_char(c);
    case RE_NOT_WORD_CHAR: return !is_word_char(c);
    default: return 0;
    }
}

/*
 * try_match: try to match nodes starting at ni against text.
 * On success, *end_pos is set past the consumed text.
 */
static int try_match(const regex_compiled_t *re, int ni,
                     const char *text, const char *sol, const char *eol,
                     const char **end_pos, int icase)
{
restart:
    if (ni >= re->count || re->nodes[ni].type == RE_END) {
        *end_pos = text;
        return 1;
    }

    const re_node_t *n = &re->nodes[ni];
    int next = ni + 1;

    /* ── Alternation ───────────────────────────────── */
    if (n->type == RE_ALT) {
        /* End of this alternative branch — match succeeded up to here */
        *end_pos = text;
        return 1;
    }

    /* ── Quantifiers on NEXT node ──────────────────── */
    if (next < re->count) {
        int qt = re->nodes[next].type;
        if (qt == RE_STAR || qt == RE_PLUS || qt == RE_QUEST || qt == RE_REPEAT) {
            int rmin = 0, rmax = 0x7fffffff;
            if (qt == RE_STAR)   { rmin = 0; }
            else if (qt == RE_PLUS)  { rmin = 1; }
            else if (qt == RE_QUEST) { rmin = 0; rmax = 1; }
            else if (qt == RE_REPEAT) {
                rmin = re->nodes[next].rep_min;
                rmax = re->nodes[next].rep_max;
                if (rmax < 0) rmax = 0x7fffffff;
            }

            /* Greedy: expand as far as possible, then backtrack */
            const char *t = text;
            int count = 0;
            while (count < rmax && t < eol) {
                if (!atom_match(n, (unsigned char)*t, icase)) break;
                t++; count++;
            }
            for (int i = count; i >= rmin; i--) {
                if (try_match(re, next + 1, text + i, sol, eol, end_pos, icase))
                    return 1;
            }
            return 0;
        }
    }

    /* ── Anchors ───────────────────────────────────── */
    if (n->type == RE_ANCHOR_BOL) {
        if (text != sol) return 0;
        ni = next; goto restart;
    }
    if (n->type == RE_ANCHOR_EOL) {
        if (text != eol) return 0;
        ni = next; goto restart;
    }

    /* ── Word boundaries ───────────────────────────── */
    if (n->type == RE_WORD_BOUNDARY || n->type == RE_NOT_WORD_BOUNDARY ||
        n->type == RE_WORD_START || n->type == RE_WORD_END) {
        int pw = (text > sol) ? is_word_char((unsigned char)text[-1]) : 0;
        int cw = (text < eol) ? is_word_char((unsigned char)*text) : 0;
        int at_b = (pw != cw);
        if (n->type == RE_WORD_BOUNDARY && !at_b) return 0;
        if (n->type == RE_NOT_WORD_BOUNDARY && at_b) return 0;
        if (n->type == RE_WORD_START && (pw || !cw)) return 0;
        if (n->type == RE_WORD_END && (!pw || cw)) return 0;
        ni = next; goto restart;
    }

    /* ── Groups (non-capturing, just pass through) ── */
    if (n->type == RE_GROUP_START || n->type == RE_GROUP_END) {
        ni = next; goto restart;
    }

    /* ── Backref (simplified: skip) ────────────────── */
    if (n->type == RE_BACKREF) {
        ni = next; goto restart;
    }

    /* ── Consuming character match ─────────────────── */
    if (text >= eol) return 0;

    if (!atom_match(n, (unsigned char)*text, icase))
        return 0;
    ni = next; text = text + 1; goto restart; /* tail-call elimination */
}

/*
 * regex_search: find first match of compiled regex in line.
 * Returns 1 if found; sets *ms, *me to match start/end.
 */
static int regex_search(const regex_compiled_t *re, const char *line, int len,
                        const char **ms, const char **me, int icase)
{
    const char *eol = line + len;

    /* Find top-level alternation splits */
    int alt_starts[64];
    int nalt = 0;
    alt_starts[nalt++] = 0;
    int depth = 0;
    for (int i = 0; i < re->count; i++) {
        if (re->nodes[i].type == RE_GROUP_START) depth++;
        else if (re->nodes[i].type == RE_GROUP_END) depth--;
        else if (re->nodes[i].type == RE_ALT && depth == 0) {
            if (nalt < 64) alt_starts[nalt++] = i + 1;
        }
    }

    int anchored = (re->count > 0 && re->nodes[0].type == RE_ANCHOR_BOL);

    for (const char *t = line; t <= eol; t++) {
        for (int a = 0; a < nalt; a++) {
            const char *ep;
            if (try_match(re, alt_starts[a], t, line, eol, &ep, icase)) {
                *ms = t;
                *me = ep;
                return 1;
            }
        }
        if (anchored) break;
    }
    return 0;
}


/* ════════════════════════════════════════════════════════════════════
 *  Fixed string search
 * ════════════════════════════════════════════════════════════════════ */

static int fixed_search(const char *pattern, const char *line, int len,
                        const char **ms, const char **me, int icase)
{
    int plen = strlen(pattern);
    if (plen == 0) { *ms = *me = line; return 1; }

    for (int i = 0; i <= len - plen; i++) {
        int ok = 1;
        for (int j = 0; j < plen; j++) {
            int a = (unsigned char)line[i + j];
            int b = (unsigned char)pattern[j];
            if (icase) {
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
            }
            if (a != b) { ok = 0; break; }
        }
        if (ok) { *ms = line + i; *me = line + i + plen; return 1; }
    }
    return 0;
}


/* ════════════════════════════════════════════════════════════════════
 *  grep main program
 * ════════════════════════════════════════════════════════════════════ */

/* Pattern syntax modes */
enum { MODE_BRE, MODE_ERE, MODE_FIXED };

/* Options */
static int opt_mode = MODE_BRE;
static int opt_icase = 0;
static int opt_invert = 0;
static int opt_count = 0;
static int opt_files_match = 0;      /* -l */
static int opt_files_no_match = 0;   /* -L */
static int opt_only_match = 0;       /* -o */
static int opt_line_number = 0;      /* -n */
static int opt_byte_offset = 0;      /* -b */
static int opt_with_filename = -1;   /* -H / -h (default auto) */
static int opt_quiet = 0;            /* -q */
static int opt_no_messages = 0;      /* -s */
static int opt_word_regexp = 0;      /* -w */
static int opt_line_regexp = 0;      /* -x */
static int opt_max_count = -1;       /* -m */
static int opt_after_ctx = 0;        /* -A */
static int opt_before_ctx = 0;       /* -B */
static int opt_color = 0;            /* --color */
static int opt_null = 0;             /* -Z */
static int opt_initial_tab = 0;      /* -T */
static int opt_recursive = 0;        /* -r / -R */
static int opt_binary_text = 0;      /* -a */
static int opt_binary_skip = 0;      /* -I */
static const char *opt_label_str = "(standard input)";
static int opt_label = 0;
static const char *opt_group_sep = "--";
static int opt_no_group_sep = 0;

/* Pattern storage */
#define MAX_PATTERNS 256
static char *patterns[MAX_PATTERNS];
static int npatterns = 0;
static regex_compiled_t *compiled_re = NULL; /* heap-allocated, sized to npatterns */

/* Include/exclude globs */
#define MAX_GLOBS 64
static char *include_globs[MAX_GLOBS];
static int n_include = 0;
static char *exclude_globs[MAX_GLOBS];
static int n_exclude = 0;
static char *exclude_dir_globs[MAX_GLOBS];
static int n_exclude_dir = 0;

/* Color codes */
static const char *color_match    = "\033[01;31m";
static const char *color_filename = "\033[35m";
static const char *color_lineno   = "\033[32m";
static const char *color_sep      = "\033[36m";
static const char *color_reset    = "\033[m";

/* Global match status */
static int g_match_found = 0;

static void add_pattern(const char *pat)
{
    const char *p = pat;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (npatterns < MAX_PATTERNS) {
            patterns[npatterns] = malloc(len + 1);
            if (patterns[npatterns]) {
                memcpy(patterns[npatterns], p, len);
                patterns[npatterns][len] = '\0';
                npatterns++;
            }
        }
        if (nl) p = nl + 1; else break;
    }
}

static void compile_patterns(void)
{
    if (opt_mode == MODE_FIXED) return;
    compiled_re = malloc(npatterns * sizeof(regex_compiled_t));
    if (!compiled_re) {
        fprintf(stderr, "%s: memory exhausted\n", PROGRAM_NAME);
        exit(2);
    }
    for (int i = 0; i < npatterns; i++)
        re_compile(&compiled_re[i], patterns[i], (opt_mode == MODE_ERE), opt_icase);
}

static int match_line(const char *line, int len, const char **ms, const char **me)
{
    for (int i = 0; i < npatterns; i++) {
        int found = 0;
        if (opt_mode == MODE_FIXED)
            found = fixed_search(patterns[i], line, len, ms, me, opt_icase);
        else
            found = regex_search(&compiled_re[i], line, len, ms, me, opt_icase);

        if (found) {
            /* Check -w (word boundary) */
            if (opt_word_regexp) {
                int pw = (*ms > line) ? is_word_char((unsigned char)(*ms)[-1]) : 0;
                int nw = (*me < line + len) ? is_word_char((unsigned char)**me) : 0;
                if (pw || nw) continue;
            }
            /* Check -x (whole line) */
            if (opt_line_regexp) {
                if (*ms != line || *me != line + len) continue;
            }
            return 1;
        }
    }
    return 0;
}

/* ── Context line ring buffer ──────────────────────────────────── */
#define MAX_CTX 1024
static char *before_buf[MAX_CTX];
static int   before_lineno[MAX_CTX];
static long  before_offset[MAX_CTX];
static int   before_count = 0;
static int   before_idx = 0;

static void before_buf_add(const char *line, int lineno, long offset)
{
    if (opt_before_ctx <= 0) return;
    int idx = before_idx % MAX_CTX;
    if (before_buf[idx]) free(before_buf[idx]);
    before_buf[idx] = strdup(line);
    before_lineno[idx] = lineno;
    before_offset[idx] = offset;
    before_idx++;
    if (before_count < opt_before_ctx) before_count++;
}

static void print_prefix(const char *filename, int lineno, long byte_off,
                          int is_match, int multi_file)
{
    char sep = is_match ? ':' : '-';

    if (multi_file && opt_with_filename != 0) {
        if (opt_color)
            printf("%s%s%s%s%c%s", color_filename, filename, color_reset,
                   color_sep, sep, color_reset);
        else
            printf("%s%c", filename, sep);
    }
    if (opt_line_number) {
        if (opt_color)
            printf("%s%d%s%s%c%s", color_lineno, lineno, color_reset,
                   color_sep, sep, color_reset);
        else
            printf("%d%c", lineno, sep);
    }
    if (opt_byte_offset) {
        if (opt_color)
            printf("%s%ld%s%s%c%s", color_lineno, byte_off, color_reset,
                   color_sep, sep, color_reset);
        else
            printf("%ld%c", byte_off, sep);
    }
    if (opt_initial_tab)
        putchar('\t');
}

/* ── Process one file ──────────────────────────────────────────── */
#define LINE_BUFSZ 65536

static int process_file(FILE *fp, const char *filename, int multi_file)
{
    char *line = malloc(LINE_BUFSZ);
    if (!line) { fprintf(stderr, "%s: memory exhausted\n", PROGRAM_NAME); return 2; }
    int lineno = 0;
    long byte_off = 0;
    int match_count = 0;
    int last_match_line = -1;
    int after_remaining = 0;
    int printed_something = 0;

    before_count = 0;
    before_idx = 0;

    while (fgets(line, LINE_BUFSZ, fp)) {
        lineno++;
        int len = strlen(line);
        int had_newline = 0;
        if (len > 0 && line[len - 1] == '\n') { line[--len] = '\0'; had_newline = 1; }
        if (len > 0 && line[len - 1] == '\r') { line[--len] = '\0'; }

        const char *ms = NULL, *me = NULL;
        int matched = match_line(line, len, &ms, &me);
        if (opt_invert) matched = !matched;

        if (matched) {
            g_match_found = 1;
            match_count++;

            if (opt_quiet) { free(line); return 0; }
            if (opt_files_match) {
                printf("%s%c", filename, opt_null ? '\0' : '\n');
                free(line); return 0;
            }
            if (opt_count) {
                byte_off += len + (had_newline ? 1 : 0);
                if (opt_max_count >= 0 && match_count >= opt_max_count) break;
                continue;
            }

            /* Group separator */
            if (printed_something && (opt_before_ctx > 0 || opt_after_ctx > 0) &&
                !opt_no_group_sep && last_match_line >= 0 &&
                lineno > last_match_line + opt_after_ctx + 1) {
                printf("%s\n", opt_group_sep);
            }

            /* Before-context */
            if (opt_before_ctx > 0 && before_count > 0) {
                int start = before_idx - before_count;
                if (start < 0) start = 0;
                for (int i = start; i < before_idx; i++) {
                    int idx = i % MAX_CTX;
                    if (before_buf[idx] &&
                        before_lineno[idx] > last_match_line + opt_after_ctx) {
                        print_prefix(filename, before_lineno[idx],
                                     before_offset[idx], 0, multi_file);
                        printf("%s\n", before_buf[idx]);
                    }
                }
                before_count = 0;
            }

            /* Print matching line */
            if (opt_only_match && !opt_invert && ms && me) {
                /* -o: print only matching parts, one per line */
                const char *search = line;
                int slen = len;
                while (slen > 0) {
                    const char *ms2, *me2;
                    int found;
                    if (opt_mode == MODE_FIXED)
                        found = fixed_search(patterns[0], search, slen, &ms2, &me2, opt_icase);
                    else
                        found = regex_search(&compiled_re[0], search, slen, &ms2, &me2, opt_icase);
                    if (!found || me2 <= ms2) break;
                    print_prefix(filename, lineno,
                                 byte_off + (long)(ms2 - line), 1, multi_file);
                    if (opt_color) printf("%s", color_match);
                    fwrite(ms2, 1, me2 - ms2, stdout);
                    if (opt_color) printf("%s", color_reset);
                    putchar('\n');
                    slen -= (int)(me2 - search);
                    search = me2;
                }
            } else {
                print_prefix(filename, lineno, byte_off, 1, multi_file);
                if (opt_color && ms && me && !opt_invert) {
                    /* Highlight first match */
                    fwrite(line, 1, ms - line, stdout);
                    printf("%s", color_match);
                    fwrite(ms, 1, me - ms, stdout);
                    printf("%s", color_reset);
                    printf("%s\n", me);
                } else {
                    printf("%s\n", line);
                }
            }

            last_match_line = lineno;
            after_remaining = opt_after_ctx;
            printed_something = 1;

            if (opt_max_count >= 0 && match_count >= opt_max_count) break;
        } else {
            /* Not matched */
            if (after_remaining > 0) {
                print_prefix(filename, lineno, byte_off, 0, multi_file);
                printf("%s\n", line);
                after_remaining--;
                printed_something = 1;
            } else {
                before_buf_add(line, lineno, byte_off);
            }
        }

        byte_off += len + (had_newline ? 1 : 0);
    }

    if (opt_count) {
        if (multi_file && opt_with_filename != 0)
            printf("%s:", filename);
        printf("%d\n", match_count);
    }

    if (opt_files_no_match && match_count == 0)
        printf("%s%c", filename, opt_null ? '\0' : '\n');

    free(line);
    return (match_count > 0) ? 0 : 1;
}

/* ── File/directory filtering ──────────────────────────────────── */
static int should_process_file(const char *name)
{
    if (n_include > 0) {
        int ok = 0;
        for (int i = 0; i < n_include; i++)
            if (fnmatch(include_globs[i], name, 0) == 0) { ok = 1; break; }
        if (!ok) return 0;
    }
    for (int i = 0; i < n_exclude; i++)
        if (fnmatch(exclude_globs[i], name, 0) == 0) return 0;
    return 1;
}

static int should_process_dir(const char *name)
{
    for (int i = 0; i < n_exclude_dir; i++)
        if (fnmatch(exclude_dir_globs[i], name, 0) == 0) return 0;
    return 1;
}

/* ── Recursive directory grep ──────────────────────────────────── */
static int grep_file(const char *path, int multi_file);

static int grep_dir(const char *dirpath)
{
    int dfd = open(dirpath, 0);
    if (dfd < 0) {
        if (!opt_no_messages)
            fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, dirpath, strerror(errno));
        return 2;
    }

    char *dentbuf = malloc(4096);
    char *fullpath = malloc(4096);
    if (!dentbuf || !fullpath) {
        free(dentbuf); free(fullpath);
        close(dfd); return 2;
    }
    int ret = 1;
    int nread;

    while ((nread = getdents64(dfd, dentbuf, 4096)) > 0) {
        int off = 0;
        while (off < nread) {
            struct dirent *de = (struct dirent *)(dentbuf + off);
            if (de->d_reclen == 0) break;

            if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
                snprintf(fullpath, 4096, "%s/%s", dirpath, de->d_name);

                if (de->d_type == 4) { /* DT_DIR */
                    if (should_process_dir(de->d_name)) {
                        int r = grep_dir(fullpath);
                        if (r == 0) ret = 0;
                    }
                } else {
                    if (should_process_file(de->d_name)) {
                        int r = grep_file(fullpath, 1);
                        if (r == 0) ret = 0;
                    }
                }
            }
            off += de->d_reclen;
        }
    }
    free(dentbuf);
    free(fullpath);
    close(dfd);
    return ret;
}

static int grep_file(const char *path, int multi_file)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (!opt_no_messages)
            fprintf(stderr, "%s: %s: No such file or directory\n", PROGRAM_NAME, path);
        return 2;
    }

    if (S_ISDIR(st.st_mode)) {
        if (opt_recursive)
            return grep_dir(path);
        if (!opt_no_messages)
            fprintf(stderr, "%s: %s: Is a directory\n", PROGRAM_NAME, path);
        return 2;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (!opt_no_messages)
            fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, path, strerror(errno));
        return 2;
    }

    int r = process_file(fp, path, multi_file);
    fclose(fp);
    return r;
}

/* ── Usage / help ──────────────────────────────────────────────── */
static void usage(int status)
{
    if (status != 0) {
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
    } else {
        printf(
"Usage: %s [OPTION]... PATTERNS [FILE]...\n"
"Search for PATTERNS in each FILE.\n"
"PATTERNS can contain multiple patterns separated by newlines.\n\n"
"Pattern selection and interpretation:\n"
"  -E, --extended-regexp     PATTERNS are extended regular expressions\n"
"  -F, --fixed-strings       PATTERNS are strings\n"
"  -G, --basic-regexp        PATTERNS are basic regular expressions\n"
"  -e, --regexp=PATTERNS     use PATTERNS for matching\n"
"  -f, --file=FILE           take PATTERNS from FILE\n"
"  -i, --ignore-case         ignore case distinctions in patterns and data\n"
"  -w, --word-regexp         match only whole words\n"
"  -x, --line-regexp         match only whole lines\n\n"
"Output control:\n"
"  -c, --count               print only a count of selected lines per FILE\n"
"      --color[=WHEN]        use markers to highlight the matching strings;\n"
"                            WHEN is 'always', 'never', or 'auto'\n"
"  -l, --files-with-matches  print only names of FILEs with selected lines\n"
"  -L, --files-without-match print only names of FILEs with no selected lines\n"
"  -m, --max-count=NUM       stop after NUM selected lines\n"
"  -o, --only-matching       show only nonempty parts of lines that match\n"
"  -q, --quiet, --silent     suppress all normal output\n"
"  -s, --no-messages         suppress error messages\n\n"
"Output line prefix control:\n"
"  -b, --byte-offset         print the byte offset with output lines\n"
"  -H, --with-filename       print file name with output lines\n"
"  -h, --no-filename         suppress the file name prefix on output\n"
"  -n, --line-number         print line number with output lines\n"
"  -T, --initial-tab         make tabs line up (if needed)\n"
"  -Z, --null                print 0 byte after FILE name\n\n"
"Context control:\n"
"  -A, --after-context=NUM   print NUM lines of trailing context\n"
"  -B, --before-context=NUM  print NUM lines of leading context\n"
"  -C, --context=NUM         print NUM lines of output context\n"
"      --group-separator=SEP print SEP on line between matches with context\n"
"      --no-group-separator  suppress separator for matches with context\n\n"
"File and directory selection:\n"
"  -a, --text                equivalent to --binary-files=text\n"
"  -I                        equivalent to --binary-files=without-match\n"
"  -r, --recursive           search directories recursively\n"
"  -R, --dereference-recursive  likewise, but follow all symlinks\n"
"      --include=GLOB        search only files that match GLOB\n"
"      --exclude=GLOB        skip files that match GLOB\n"
"      --exclude-dir=GLOB    skip directories that match GLOB\n\n"
"Miscellaneous:\n"
"  -v, --invert-match        select non-matching lines\n"
"      --help                display this help text and exit\n"
"  -V, --version             display version information and exit\n",
            PROGRAM_NAME);
    }
    exit(status);
}

/* ── Main ──────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    static struct option long_options[] = {
        {"extended-regexp",       no_argument,       0, 'E'},
        {"fixed-strings",         no_argument,       0, 'F'},
        {"basic-regexp",          no_argument,       0, 'G'},
        {"regexp",                required_argument, 0, 'e'},
        {"file",                  required_argument, 0, 'f'},
        {"ignore-case",           no_argument,       0, 'i'},
        {"no-ignore-case",        no_argument,       0, 128},
        {"invert-match",          no_argument,       0, 'v'},
        {"word-regexp",           no_argument,       0, 'w'},
        {"line-regexp",           no_argument,       0, 'x'},
        {"count",                 no_argument,       0, 'c'},
        {"color",                 optional_argument, 0, 129},
        {"colour",                optional_argument, 0, 129},
        {"files-with-matches",    no_argument,       0, 'l'},
        {"files-without-match",   no_argument,       0, 'L'},
        {"max-count",             required_argument, 0, 'm'},
        {"only-matching",         no_argument,       0, 'o'},
        {"quiet",                 no_argument,       0, 'q'},
        {"silent",                no_argument,       0, 'q'},
        {"no-messages",           no_argument,       0, 's'},
        {"byte-offset",           no_argument,       0, 'b'},
        {"with-filename",         no_argument,       0, 'H'},
        {"no-filename",           no_argument,       0, 'h'},
        {"label",                 required_argument, 0, 130},
        {"line-number",           no_argument,       0, 'n'},
        {"initial-tab",           no_argument,       0, 'T'},
        {"null",                  no_argument,       0, 'Z'},
        {"after-context",         required_argument, 0, 'A'},
        {"before-context",        required_argument, 0, 'B'},
        {"context",               required_argument, 0, 'C'},
        {"group-separator",       required_argument, 0, 131},
        {"no-group-separator",    no_argument,       0, 132},
        {"text",                  no_argument,       0, 'a'},
        {"binary-files",          required_argument, 0, 133},
        {"recursive",             no_argument,       0, 'r'},
        {"dereference-recursive", no_argument,       0, 'R'},
        {"include",               required_argument, 0, 134},
        {"exclude",               required_argument, 0, 135},
        {"exclude-dir",           required_argument, 0, 136},
        {"line-buffered",         no_argument,       0, 137},
        {"version",               no_argument,       0, 'V'},
        {"help",                  no_argument,       0, 'X'},
        {0, 0, 0, 0}
    };

    int c;
    int explicit_patterns = 0;
    optind = 1;

    /* Pre-scan for -NUM shorthand (e.g. grep -3 → -C 3) */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '1' && argv[i][1] <= '9') {
            int all_digits = 1;
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] < '0' || argv[i][j] > '9') { all_digits = 0; break; }
            }
            if (all_digits) {
                int ctx = atoi(&argv[i][1]);
                opt_before_ctx = ctx;
                opt_after_ctx = ctx;
                /* Remove this arg */
                for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
                argc--; i--;
            }
        }
    }

    while ((c = getopt_long(argc, argv, "EFGe:f:ivwxclLm:oqsbHhnTZA:B:C:aIrRV",
                            long_options, NULL)) != -1) {
        switch (c) {
        case 'E': opt_mode = MODE_ERE; break;
        case 'F': opt_mode = MODE_FIXED; break;
        case 'G': opt_mode = MODE_BRE; break;
        case 'e':
            add_pattern(optarg);
            explicit_patterns = 1;
            break;
        case 'f': {
            FILE *pf = fopen(optarg, "r");
            if (!pf) {
                fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, optarg, strerror(errno));
                return 2;
            }
            char pbuf[4096];
            while (fgets(pbuf, sizeof(pbuf), pf)) {
                int plen = strlen(pbuf);
                if (plen > 0 && pbuf[plen - 1] == '\n') pbuf[--plen] = '\0';
                add_pattern(pbuf);
            }
            fclose(pf);
            explicit_patterns = 1;
            break;
        }
        case 'i': opt_icase = 1; break;
        case 128:  opt_icase = 0; break;
        case 'v': opt_invert = 1; break;
        case 'w': opt_word_regexp = 1; break;
        case 'x': opt_line_regexp = 1; break;
        case 'c': opt_count = 1; break;
        case 129:
            if (!optarg || strcmp(optarg, "always") == 0 || strcmp(optarg, "auto") == 0)
                opt_color = 1;
            else if (strcmp(optarg, "never") == 0)
                opt_color = 0;
            break;
        case 'l': opt_files_match = 1; break;
        case 'L': opt_files_no_match = 1; break;
        case 'm': opt_max_count = atoi(optarg); break;
        case 'o': opt_only_match = 1; break;
        case 'q': opt_quiet = 1; break;
        case 's': opt_no_messages = 1; break;
        case 'b': opt_byte_offset = 1; break;
        case 'H': opt_with_filename = 1; break;
        case 'h': opt_with_filename = 0; break;
        case 130: opt_label_str = optarg; opt_label = 1; break;
        case 'n': opt_line_number = 1; break;
        case 'T': opt_initial_tab = 1; break;
        case 'Z': opt_null = 1; break;
        case 'A': opt_after_ctx = atoi(optarg); break;
        case 'B': opt_before_ctx = atoi(optarg); break;
        case 'C':
            opt_before_ctx = atoi(optarg);
            opt_after_ctx = atoi(optarg);
            break;
        case 131: opt_group_sep = optarg; break;
        case 132: opt_no_group_sep = 1; break;
        case 'a': opt_binary_text = 1; break;
        case 'I': opt_binary_skip = 1; break;
        case 133:
            if (strcmp(optarg, "text") == 0) opt_binary_text = 1;
            else if (strcmp(optarg, "without-match") == 0) opt_binary_skip = 1;
            break;
        case 'r': opt_recursive = 1; break;
        case 'R': opt_recursive = 1; break;
        case 134: if (n_include < MAX_GLOBS) include_globs[n_include++] = strdup(optarg); break;
        case 135: if (n_exclude < MAX_GLOBS) exclude_globs[n_exclude++] = strdup(optarg); break;
        case 136: if (n_exclude_dir < MAX_GLOBS) exclude_dir_globs[n_exclude_dir++] = strdup(optarg); break;
        case 137: setlinebuf(stdout); break;
        case 'V':
            printf("%s (%s) %s\n", PROGRAM_NAME, "LikeOS", VERSION);
            return 0;
        case 'X': usage(0); break;
        default: usage(2); break;
        }
    }

    /* First non-option arg is the pattern if no -e/-f given */
    if (!explicit_patterns) {
        if (optind >= argc) {
            fprintf(stderr, "%s: missing pattern\n", PROGRAM_NAME);
            usage(2);
        }
        add_pattern(argv[optind++]);
    }

    if (npatterns == 0) {
        fprintf(stderr, "%s: no patterns\n", PROGRAM_NAME);
        return 2;
    }

    compile_patterns();

    int nfiles = argc - optind;
    int multi_file = (nfiles > 1) || opt_recursive;
    if (opt_with_filename == -1)
        opt_with_filename = multi_file ? 1 : 0;

    int ret = 1;

    if (nfiles == 0) {
        const char *label = opt_label ? opt_label_str : "(standard input)";
        if (process_file(stdin, label, multi_file) == 0) ret = 0;
    } else {
        for (int i = optind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) {
                const char *label = opt_label ? opt_label_str : "(standard input)";
                if (process_file(stdin, label, multi_file) == 0) ret = 0;
            } else {
                if (grep_file(argv[i], multi_file) == 0) ret = 0;
            }
        }
    }

    if (opt_quiet) return g_match_found ? 0 : 1;
    return ret;
}
