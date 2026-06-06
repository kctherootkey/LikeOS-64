/*
 * tr - translate or delete characters
 *
 * Full implementation per tr(1) manpage.
 * Supports: -c, -C, -d, -s, -t
 *           --help, --version
 *
 * Character classes: [:alnum:] [:alpha:] [:blank:] [:cntrl:] [:digit:]
 *   [:graph:] [:lower:] [:print:] [:punct:] [:space:] [:upper:] [:xdigit:]
 * Equivalence classes: [=CHAR=]
 * Ranges: CHAR1-CHAR2
 * Repeats: [CHAR*] [CHAR*REPEAT]
 * Escape sequences: \\ \a \b \f \n \r \t \v \NNN (octal)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define PROGRAM_NAME "tr"
#define VERSION      "1.0"

/* ── Options ────────────────────────────────────────────────────────── */
static int opt_complement;      /* -c / -C */
static int opt_delete;          /* -d */
static int opt_squeeze;         /* -s */
static int opt_truncate;        /* -t */

/* ── Character set (256 bytes, one per possible byte value) ─────────── */

typedef struct {
    unsigned char chars[256];
    int len;
} charset_t;

static void cs_clear(charset_t *cs) { cs->len = 0; }

static void cs_add(charset_t *cs, unsigned char c)
{
    if (cs->len < 256)
        cs->chars[cs->len++] = c;
}

/* ── Parse escape sequence ──────────────────────────────────────────── */

static unsigned char parse_escape(const char **pp)
{
    const char *p = *pp;
    p++; /* skip backslash */
    unsigned char c;
    switch (*p) {
    case '\\': c = '\\'; p++; break;
    case 'a':  c = '\a'; p++; break;
    case 'b':  c = '\b'; p++; break;
    case 'f':  c = '\f'; p++; break;
    case 'n':  c = '\n'; p++; break;
    case 'r':  c = '\r'; p++; break;
    case 't':  c = '\t'; p++; break;
    case 'v':  c = '\v'; p++; break;
    default:
        /* Octal: \NNN (1-3 digits) */
        if (*p >= '0' && *p <= '7') {
            int val = 0;
            for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++, p++)
                val = val * 8 + (*p - '0');
            c = (unsigned char)(val & 0xff);
        } else {
            c = '\\';
            /* Don't advance past the character after backslash */
        }
        break;
    }
    *pp = p;
    return c;
}

/* ── Parse a character class ────────────────────────────────────────── */

static int parse_class(const char **pp, charset_t *cs)
{
    const char *p = *pp;
    /* Must start with [: */
    if (p[0] != '[' || p[1] != ':') return 0;
    p += 2;

    const char *end = strstr(p, ":]");
    if (!end) return 0;

    int clen = (int)(end - p);
    char name[32];
    if (clen >= (int)sizeof(name)) return 0;
    memcpy(name, p, (size_t)clen);
    name[clen] = '\0';

    *pp = end + 2;

    for (int c = 0; c < 256; c++) {
        int match = 0;
        if (strcmp(name, "alnum") == 0)  match = isalnum(c);
        else if (strcmp(name, "alpha") == 0)  match = isalpha(c);
        else if (strcmp(name, "blank") == 0)  match = (c == ' ' || c == '\t');
        else if (strcmp(name, "cntrl") == 0)  match = iscntrl(c);
        else if (strcmp(name, "digit") == 0)  match = isdigit(c);
        else if (strcmp(name, "graph") == 0)  match = isgraph(c);
        else if (strcmp(name, "lower") == 0)  match = islower(c);
        else if (strcmp(name, "print") == 0)  match = isprint(c);
        else if (strcmp(name, "punct") == 0)  match = ispunct(c);
        else if (strcmp(name, "space") == 0)  match = isspace(c);
        else if (strcmp(name, "upper") == 0)  match = isupper(c);
        else if (strcmp(name, "xdigit") == 0) match = isxdigit(c);

        if (match)
            cs_add(cs, (unsigned char)c);
    }
    return 1;
}

/* ── Parse equivalence class [=CHAR=] ───────────────────────────────── */

static int parse_equiv(const char **pp, charset_t *cs)
{
    const char *p = *pp;
    if (p[0] != '[' || p[1] != '=') return 0;
    unsigned char ch = (unsigned char)p[2];
    if (p[3] != '=' || p[4] != ']') return 0;
    *pp = p + 5;
    cs_add(cs, ch);
    return 1;
}

/* ── Parse repeat [CHAR*] or [CHAR*REPEAT] ──────────────────────────── */

static int parse_repeat(const char **pp, charset_t *cs, int target_len)
{
    const char *p = *pp;
    if (p[0] != '[') return 0;

    unsigned char ch;
    const char *q = p + 1;
    if (*q == '\\') {
        ch = parse_escape(&q);
    } else {
        ch = (unsigned char)*q;
        q++;
    }

    if (*q != '*') return 0;
    q++;

    int repeat;
    if (*q == ']') {
        /* [CHAR*] — fill to length of ARRAY1 */
        repeat = target_len - cs->len;
        if (repeat < 0) repeat = 0;
        q++;
    } else {
        /* [CHAR*REPEAT] */
        if (*q == '0') {
            /* Octal */
            repeat = (int)strtol(q, (char **)&q, 8);
        } else {
            repeat = (int)strtol(q, (char **)&q, 10);
        }
        if (*q == ']') q++;
    }

    for (int i = 0; i < repeat; i++)
        cs_add(cs, ch);

    *pp = q;
    return 1;
}

/* ── Parse a full string specification ──────────────────────────────── */

static void parse_string(const char *spec, charset_t *cs, int target_len)
{
    cs_clear(cs);
    const char *p = spec;

    while (*p) {
        /* Character class [:name:] */
        if (parse_class(&p, cs)) continue;

        /* Equivalence class [=c=] */
        if (parse_equiv(&p, cs)) continue;

        /* Repeat [c*] or [c*N] */
        if (parse_repeat(&p, cs, target_len)) continue;

        /* Escape sequence */
        if (*p == '\\') {
            unsigned char c = parse_escape(&p);
            /* Check for range: c-d */
            if (*p == '-' && p[1]) {
                p++;
                unsigned char end_c;
                if (*p == '\\')
                    end_c = parse_escape(&p);
                else {
                    end_c = (unsigned char)*p;
                    p++;
                }
                if (end_c >= c) {
                    for (unsigned int i = c; i <= end_c; i++)
                        cs_add(cs, (unsigned char)i);
                } else {
                    cs_add(cs, c);
                }
            } else {
                cs_add(cs, c);
            }
            continue;
        }

        /* Range: CHAR1-CHAR2 */
        if (p[1] == '-' && p[2]) {
            unsigned char start = (unsigned char)*p;
            unsigned char end_c;
            p += 2;
            if (*p == '\\')
                end_c = parse_escape(&p);
            else {
                end_c = (unsigned char)*p;
                p++;
            }
            if (end_c >= start) {
                for (unsigned int i = start; i <= end_c; i++)
                    cs_add(cs, (unsigned char)i);
            } else {
                cs_add(cs, start);
            }
            continue;
        }

        /* Plain character */
        cs_add(cs, (unsigned char)*p);
        p++;
    }
}

/* ── Help / version ─────────────────────────────────────────────────── */

static void print_help(void)
{
    printf("Usage: tr [OPTION]... STRING1 [STRING2]\n");
    printf("Translate, squeeze, and/or delete characters from standard input,\n");
    printf("writing to standard output.\n\n");
    printf("  -c, -C, --complement    use the complement of ARRAY1\n");
    printf("  -d, --delete            delete characters in ARRAY1, do not translate\n");
    printf("  -s, --squeeze-repeats   replace each sequence of a repeated character\n");
    printf("                          that is listed in the last specified ARRAY,\n");
    printf("                          with a single occurrence of that character\n");
    printf("  -t, --truncate-set1     first truncate ARRAY1 to length of ARRAY2\n");
    printf("      --help              display this help and exit\n");
    printf("      --version           output version information and exit\n\n");
    printf("Interpreted sequences:\n");
    printf("  \\NNN   character with octal value NNN (1-3 octal digits)\n");
    printf("  \\\\     backslash\n");
    printf("  \\a \\b \\f \\n \\r \\t \\v  as in C\n");
    printf("  CHAR1-CHAR2     all characters from CHAR1 to CHAR2 in ascending order\n");
    printf("  [CHAR*]         in ARRAY2, copies of CHAR until length of ARRAY1\n");
    printf("  [CHAR*REPEAT]   REPEAT copies of CHAR\n");
    printf("  [:alnum:] [:alpha:] [:blank:] [:cntrl:] [:digit:] [:graph:]\n");
    printf("  [:lower:] [:print:] [:punct:] [:space:] [:upper:] [:xdigit:]\n");
    printf("  [=CHAR=]        all characters equivalent to CHAR\n");
}

static void print_version(void)
{
    printf("%s (LikeOS) %s\n", PROGRAM_NAME, VERSION);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* Parse options manually since we need STRING1 [STRING2] positionals */
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1]) {
        const char *a = argv[argi];
        if (strcmp(a, "--") == 0) { argi++; break; }
        if (strcmp(a, "--help") == 0) { print_help(); return 0; }
        if (strcmp(a, "--version") == 0) { print_version(); return 0; }
        if (strcmp(a, "--complement") == 0) { opt_complement = 1; argi++; continue; }
        if (strcmp(a, "--delete") == 0) { opt_delete = 1; argi++; continue; }
        if (strcmp(a, "--squeeze-repeats") == 0) { opt_squeeze = 1; argi++; continue; }
        if (strcmp(a, "--truncate-set1") == 0) { opt_truncate = 1; argi++; continue; }

        /* Short options (can be grouped: -ds, -cs, etc.) */
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'c': case 'C': opt_complement = 1; break;
            case 'd': opt_delete = 1; break;
            case 's': opt_squeeze = 1; break;
            case 't': opt_truncate = 1; break;
            default:
                fprintf(stderr, "tr: invalid option -- '%c'\n", *p);
                fprintf(stderr, "Try 'tr --help' for more information.\n");
                return 1;
            }
        }
        argi++;
    }

    if (argi >= argc) {
        fprintf(stderr, "tr: missing operand\n");
        fprintf(stderr, "Try 'tr --help' for more information.\n");
        return 1;
    }

    const char *string1 = argv[argi++];
    const char *string2 = (argi < argc) ? argv[argi++] : NULL;

    /* Parse STRING1 */
    charset_t set1;
    parse_string(string1, &set1, 0);

    /* Build complement of set1 if requested */
    if (opt_complement) {
        unsigned char in_set[256] = {0};
        for (int i = 0; i < set1.len; i++)
            in_set[set1.chars[i]] = 1;
        cs_clear(&set1);
        for (int c = 0; c < 256; c++) {
            if (!in_set[c])
                cs_add(&set1, (unsigned char)c);
        }
    }

    /* Parse STRING2 if provided */
    charset_t set2;
    cs_clear(&set2);
    if (string2) {
        parse_string(string2, &set2, set1.len);

        /* If -t: truncate set1 to length of set2 */
        if (opt_truncate && set1.len > set2.len)
            set1.len = set2.len;

        /* Extend set2 to length of set1 by repeating last character */
        if (set2.len > 0 && set2.len < set1.len) {
            unsigned char last = set2.chars[set2.len - 1];
            while (set2.len < set1.len)
                cs_add(&set2, last);
        }
    }

    /* Build translation table */
    unsigned char xlate[256];
    for (int i = 0; i < 256; i++)
        xlate[i] = (unsigned char)i;

    if (!opt_delete && string2) {
        /* Set up translation mapping */
        for (int i = 0; i < set1.len && i < set2.len; i++)
            xlate[set1.chars[i]] = set2.chars[i];
    }

    /* Build membership sets for delete/squeeze */
    unsigned char in_set1[256] = {0};
    unsigned char in_squeeze[256] = {0};

    for (int i = 0; i < set1.len; i++)
        in_set1[set1.chars[i]] = 1;

    /* Squeeze set is the last specified ARRAY */
    if (opt_squeeze) {
        if (string2) {
            for (int i = 0; i < set2.len; i++)
                in_squeeze[set2.chars[i]] = 1;
        } else {
            for (int i = 0; i < set1.len; i++)
                in_squeeze[set1.chars[i]] = 1;
        }
    }

    /* ── Main processing loop ──────────────────────────────────────── */
    int c;
    int last_out = -1;

    while ((c = getchar()) != EOF) {
        unsigned char uc = (unsigned char)c;

        if (opt_delete) {
            /* Delete mode: drop characters in set1 */
            if (in_set1[uc])
                continue;
            /* After deletion, squeeze if -s */
            if (opt_squeeze && in_squeeze[uc]) {
                if (uc == (unsigned char)last_out)
                    continue;
            }
            putchar(uc);
            last_out = uc;
        } else if (string2) {
            /* Translate mode */
            unsigned char out = xlate[uc];
            /* Squeeze after translation */
            if (opt_squeeze && in_squeeze[out]) {
                if (out == (unsigned char)last_out)
                    continue;
            }
            putchar(out);
            last_out = out;
        } else if (opt_squeeze) {
            /* Squeeze only (no STRING2, just -s) */
            if (in_squeeze[uc]) {
                if (uc == (unsigned char)last_out)
                    continue;
            }
            putchar(uc);
            last_out = uc;
        } else {
            putchar(uc);
        }
    }

    return 0;
}
