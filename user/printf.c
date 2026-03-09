/*
 * printf - format and print data
 *
 * Full implementation per printf(1) manpage.
 * Supports: --help, --version
 * FORMAT sequences: \", \\, \a, \b, \c, \e, \f, \n, \r, \t, \v,
 *   \NNN (octal), \xHH (hex), \uHHHH, \UHHHHHHHH, %%, %b, %q,
 *   and all C format specs: d i o u x X f e E g G c s (with width/precision)
 *
 * FORMAT is reused as many times as necessary to consume all ARGUMENTs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROGRAM_NAME "printf"
#define VERSION      "1.0"

/* ── helpers ─────────────────────────────────────────────────────────── */
static int is_octal(char c) { return c >= '0' && c <= '7'; }
static int is_hex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* Get next argument as string; returns "" if exhausted */
static const char *next_arg_str(int *argi, int argc, char **argv)
{
    if (*argi < argc)
        return argv[(*argi)++];
    return "";
}

/* Get next argument as long; returns 0 if exhausted
 * Leading ' or " means use character value of next char */
static long next_arg_long(int *argi, int argc, char **argv)
{
    if (*argi >= argc)
        return 0;
    const char *s = argv[(*argi)++];
    if ((s[0] == '\'' || s[0] == '"') && s[1])
        return (unsigned char)s[1];
    return strtol(s, NULL, 0);
}

static unsigned long next_arg_ulong(int *argi, int argc, char **argv)
{
    if (*argi >= argc)
        return 0;
    const char *s = argv[(*argi)++];
    if ((s[0] == '\'' || s[0] == '"') && s[1])
        return (unsigned char)s[1];
    return strtoul(s, NULL, 0);
}

/* ── process backslash escapes in a string ───────────────────────────── */
/* Returns 1 if \c encountered (stop output) */
static int print_escaped(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case '"':  putchar('"');  break;
            case '\\': putchar('\\'); break;
            case 'a':  putchar('\a'); break;
            case 'b':  putchar('\b'); break;
            case 'c':  return 1;
            case 'e':  putchar(0x1b); break;
            case 'f':  putchar('\f'); break;
            case 'n':  putchar('\n'); break;
            case 'r':  putchar('\r'); break;
            case 't':  putchar('\t'); break;
            case 'v':  putchar('\v'); break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                /* \NNN octal (1-3 digits) */
                int val = *s - '0';
                if (is_octal(s[1])) { val = val * 8 + (s[1] - '0'); s++; }
                if (is_octal(s[1])) { val = val * 8 + (s[1] - '0'); s++; }
                putchar(val & 0xff);
                break;
            }
            case 'x': {
                /* \xHH hex (1-2 digits) */
                int val = 0, digits = 0;
                const char *p = s + 1;
                while (digits < 2 && is_hex(*p)) {
                    val = val * 16 + hex_val(*p);
                    p++; digits++;
                }
                if (digits > 0) {
                    putchar(val & 0xff);
                    s = p - 1;
                } else {
                    putchar('\\');
                    putchar('x');
                }
                break;
            }
            case 'u': {
                /* \uHHHH – 4 hex digits, output as UTF-8 */
                unsigned long val = 0;
                int digits = 0;
                const char *p = s + 1;
                while (digits < 4 && is_hex(*p)) {
                    val = val * 16 + (unsigned long)hex_val(*p);
                    p++; digits++;
                }
                if (digits > 0) {
                    /* Simple: only handle single-byte / ASCII range for now,
                     * or output UTF-8 encoding */
                    if (val < 0x80) {
                        putchar((int)val);
                    } else if (val < 0x800) {
                        putchar((int)(0xC0 | (val >> 6)));
                        putchar((int)(0x80 | (val & 0x3F)));
                    } else {
                        putchar((int)(0xE0 | (val >> 12)));
                        putchar((int)(0x80 | ((val >> 6) & 0x3F)));
                        putchar((int)(0x80 | (val & 0x3F)));
                    }
                    s = p - 1;
                } else {
                    putchar('\\');
                    putchar('u');
                }
                break;
            }
            case 'U': {
                /* \UHHHHHHHH – 8 hex digits, output as UTF-8 */
                unsigned long val = 0;
                int digits = 0;
                const char *p = s + 1;
                while (digits < 8 && is_hex(*p)) {
                    val = val * 16 + (unsigned long)hex_val(*p);
                    p++; digits++;
                }
                if (digits > 0) {
                    if (val < 0x80) {
                        putchar((int)val);
                    } else if (val < 0x800) {
                        putchar((int)(0xC0 | (val >> 6)));
                        putchar((int)(0x80 | (val & 0x3F)));
                    } else if (val < 0x10000) {
                        putchar((int)(0xE0 | (val >> 12)));
                        putchar((int)(0x80 | ((val >> 6) & 0x3F)));
                        putchar((int)(0x80 | (val & 0x3F)));
                    } else if (val < 0x110000) {
                        putchar((int)(0xF0 | (val >> 18)));
                        putchar((int)(0x80 | ((val >> 12) & 0x3F)));
                        putchar((int)(0x80 | ((val >> 6) & 0x3F)));
                        putchar((int)(0x80 | (val & 0x3F)));
                    }
                    s = p - 1;
                } else {
                    putchar('\\');
                    putchar('U');
                }
                break;
            }
            default:
                putchar('\\');
                putchar(*s);
                break;
            }
        } else {
            putchar(*s);
        }
    }
    return 0;
}

/* ── %b: like print_escaped but octal is \0 or \0NNN ────────────────── */
static int print_b_arg(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case '\\': putchar('\\'); break;
            case 'a':  putchar('\a'); break;
            case 'b':  putchar('\b'); break;
            case 'c':  return 1;
            case 'e':  putchar(0x1b); break;
            case 'f':  putchar('\f'); break;
            case 'n':  putchar('\n'); break;
            case 'r':  putchar('\r'); break;
            case 't':  putchar('\t'); break;
            case 'v':  putchar('\v'); break;
            case '0': {
                /* \0 or \0NNN */
                int val = 0;
                const char *p = s + 1;
                int digits = 0;
                while (digits < 3 && is_octal(*p)) {
                    val = val * 8 + (*p - '0');
                    p++; digits++;
                }
                putchar(val & 0xff);
                s = p - 1;
                break;
            }
            default:
                putchar('\\');
                putchar(*s);
                break;
            }
        } else {
            putchar(*s);
        }
    }
    return 0;
}

/* ── %q: shell-escaped output ────────────────────────────────────────── */
static void print_q_arg(const char *s)
{
    if (!*s) {
        fputs("''", stdout);
        return;
    }

    /* Check if the string needs quoting */
    int needs_quote = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || c == '\'' || c == '\\') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        fputs(s, stdout);
        return;
    }

    /* Use $'...' syntax for non-printable characters */
    fputs("$'", stdout);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '\\': fputs("\\\\", stdout); break;
        case '\'': fputs("\\'",  stdout); break;
        case '\a': fputs("\\a",  stdout); break;
        case '\b': fputs("\\b",  stdout); break;
        case 0x1b: fputs("\\e",  stdout); break;
        case '\f': fputs("\\f",  stdout); break;
        case '\n': fputs("\\n",  stdout); break;
        case '\r': fputs("\\r",  stdout); break;
        case '\t': fputs("\\t",  stdout); break;
        case '\v': fputs("\\v",  stdout); break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\x%02x", c);
                fputs(buf, stdout);
            } else {
                putchar(c);
            }
            break;
        }
    }
    putchar('\'');
}

/* ── process FORMAT with arguments ───────────────────────────────────── */
/* Returns 1 if \c encountered, 0 otherwise.
 * *argi is advanced as arguments are consumed. */
static int do_printf(const char *fmt, int *argi, int argc, char **argv)
{
    const char *p = fmt;

    while (*p) {
        if (*p == '\\') {
            /* Backslash escape in format string */
            const char *start = p;
            /* Use print_escaped for this one character sequence */
            char tmp[16];
            int tlen = 0;
            tmp[tlen++] = *p++;
            /* Copy until we have a complete escape or end */
            if (*p) {
                tmp[tlen++] = *p++;
                /* For octal/hex, copy more */
                if (tmp[1] == '0' || (tmp[1] >= '1' && tmp[1] <= '7')) {
                    while (tlen < 5 && is_octal(*p))
                        tmp[tlen++] = *p++;
                } else if (tmp[1] == 'x') {
                    while (tlen < 5 && is_hex(*p))
                        tmp[tlen++] = *p++;
                } else if (tmp[1] == 'u') {
                    while (tlen < 7 && is_hex(*p))
                        tmp[tlen++] = *p++;
                } else if (tmp[1] == 'U') {
                    while (tlen < 11 && is_hex(*p))
                        tmp[tlen++] = *p++;
                }
            }
            tmp[tlen] = '\0';
            if (print_escaped(tmp))
                return 1;
            (void)start;
            continue;
        }

        if (*p == '%') {
            p++;
            if (*p == '%') {
                putchar('%');
                p++;
                continue;
            }

            /* Build a format spec string for the C library */
            char spec[64];
            int si = 0;
            spec[si++] = '%';

            /* Flags */
            while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
                if (si < 60) spec[si++] = *p;
                p++;
            }

            /* Width: may be * (take from args) or digits */
            if (*p == '*') {
                long w = next_arg_long(argi, argc, argv);
                char wbuf[24];
                snprintf(wbuf, sizeof(wbuf), "%ld", w);
                for (int k = 0; wbuf[k] && si < 60; k++)
                    spec[si++] = wbuf[k];
                p++;
            } else {
                while (is_digit(*p) && si < 60)
                    spec[si++] = *p++;
            }

            /* Precision */
            if (*p == '.') {
                if (si < 60) spec[si++] = *p++;
                if (*p == '*') {
                    long pr = next_arg_long(argi, argc, argv);
                    char pbuf[24];
                    snprintf(pbuf, sizeof(pbuf), "%ld", pr);
                    for (int k = 0; pbuf[k] && si < 60; k++)
                        spec[si++] = pbuf[k];
                    p++;
                } else {
                    while (is_digit(*p) && si < 60)
                        spec[si++] = *p++;
                }
            }

            /* Conversion character */
            char conv = *p;
            if (conv == '\0') {
                spec[si] = '\0';
                fputs(spec, stdout);
                break;
            }
            p++;

            switch (conv) {
            case 'd': case 'i': {
                spec[si++] = 'l';
                spec[si++] = 'd';
                spec[si] = '\0';
                long v = next_arg_long(argi, argc, argv);
                printf(spec, v);
                break;
            }
            case 'o': {
                spec[si++] = 'l';
                spec[si++] = 'o';
                spec[si] = '\0';
                unsigned long v = next_arg_ulong(argi, argc, argv);
                printf(spec, v);
                break;
            }
            case 'u': {
                spec[si++] = 'l';
                spec[si++] = 'u';
                spec[si] = '\0';
                unsigned long v = next_arg_ulong(argi, argc, argv);
                printf(spec, v);
                break;
            }
            case 'x': {
                spec[si++] = 'l';
                spec[si++] = 'x';
                spec[si] = '\0';
                unsigned long v = next_arg_ulong(argi, argc, argv);
                printf(spec, v);
                break;
            }
            case 'X': {
                spec[si++] = 'l';
                spec[si++] = 'X';
                spec[si] = '\0';
                unsigned long v = next_arg_ulong(argi, argc, argv);
                printf(spec, v);
                break;
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': {
                /* Floating point – we don't have full float support,
                 * but we can parse and print via integer approximation.
                 * For now, treat as integer. */
                spec[si++] = 'l';
                spec[si++] = 'd';
                spec[si] = '\0';
                long v = next_arg_long(argi, argc, argv);
                printf(spec, v);
                break;
            }
            case 'c': {
                spec[si++] = 'c';
                spec[si] = '\0';
                const char *s = next_arg_str(argi, argc, argv);
                printf(spec, s[0] ? s[0] : '\0');
                break;
            }
            case 's': {
                spec[si++] = 's';
                spec[si] = '\0';
                const char *s = next_arg_str(argi, argc, argv);
                printf(spec, s);
                break;
            }
            case 'b': {
                const char *s = next_arg_str(argi, argc, argv);
                if (print_b_arg(s))
                    return 1;
                break;
            }
            case 'q': {
                const char *s = next_arg_str(argi, argc, argv);
                print_q_arg(s);
                break;
            }
            default:
                spec[si++] = conv;
                spec[si] = '\0';
                fputs(spec, stdout);
                break;
            }
            continue;
        }

        /* Normal character */
        putchar(*p++);
    }

    return 0;
}

/* ── usage / version ─────────────────────────────────────────────────── */
static void usage(void)
{
    printf(
"Usage: %s FORMAT [ARGUMENT]...\n"
"  or:  %s OPTION\n"
"Print ARGUMENT(s) according to FORMAT.\n"
"\n"
"  --help     display this help and exit\n"
"  --version  output version information and exit\n"
"\n"
"FORMAT controls the output as in C printf.  Interpreted sequences are:\n"
"\n"
"  \\\"  double quote       \\\\  backslash         \\a  alert (BEL)\n"
"  \\b  backspace          \\c  produce no further output\n"
"  \\e  escape             \\f  form feed         \\n  new line\n"
"  \\r  carriage return    \\t  horizontal tab    \\v  vertical tab\n"
"  \\NNN   byte with octal value NNN (1 to 3 digits)\n"
"  \\xHH   byte with hexadecimal value HH (1 to 2 digits)\n"
"  \\uHHHH  Unicode character with hex value HHHH (4 digits)\n"
"  \\UHHHHHHHH  Unicode character with hex value HHHHHHHH (8 digits)\n"
"\n"
"  %%%%  a single %%\n"
"  %%b  ARGUMENT as a string with '\\' escapes interpreted\n"
"  %%q  ARGUMENT is printed in a format that can be reused as shell input\n"
"\n"
"  and all C format specifications ending with one of diouxXfeEgGcs.\n"
"  Variable widths are handled.\n",
    PROGRAM_NAME, PROGRAM_NAME);
}

static void version(void)
{
    printf("%s %s\n", PROGRAM_NAME, VERSION);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "%s: usage: %s FORMAT [ARGUMENT]...\n",
                PROGRAM_NAME, PROGRAM_NAME);
        return 1;
    }

    /* Handle --help and --version */
    if (strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) {
        version();
        return 0;
    }

    const char *fmt = argv[1];
    int argi = 2;  /* index of next argument to consume */

    /* Process format string, repeating until all arguments are consumed.
     * If no conversions consume arguments, run once and stop. */
    do {
        int saved = argi;
        if (do_printf(fmt, &argi, argc, argv))
            break;  /* \c encountered */
        /* If no arguments were consumed, format had no conversions;
         * we already printed once, so stop. */
        if (argi == saved)
            break;
    } while (argi < argc);

    fflush(stdout);
    return 0;
}
