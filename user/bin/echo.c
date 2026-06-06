/*
 * echo - display a line of text
 *
 * Full implementation per echo(1) manpage.
 * Supports: -n, -e, -E, --help, --version
 * Escape sequences (with -e):
 *   \\  \a  \b  \c  \e  \f  \n  \r  \t  \v  \0NNN  \xHH
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PROGRAM_NAME "echo"
#define VERSION      "1.0"

static int opt_newline = 1;   /* print trailing newline (default yes) */
static int opt_escapes;       /* interpret backslash escapes */

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

/* Print a string, interpreting escape sequences if opt_escapes is set.
 * Returns 0 normally, 1 if \c was encountered (stop all output). */
static int print_arg(const char *s)
{
    for (; *s; s++) {
        if (opt_escapes && *s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case '\\': putchar('\\'); break;
            case 'a':  putchar('\a'); break;
            case 'b':  putchar('\b'); break;
            case 'c':  return 1; /* produce no further output */
            case 'e':  putchar(0x1b); break;
            case 'f':  putchar('\f'); break;
            case 'n':  putchar('\n'); break;
            case 'r':  putchar('\r'); break;
            case 't':  putchar('\t'); break;
            case 'v':  putchar('\v'); break;
            case '0': {
                /* \0NNN – octal value (1 to 3 digits) */
                int val = 0;
                int digits = 0;
                const char *p = s + 1;
                while (digits < 3 && is_octal(*p)) {
                    val = val * 8 + (*p - '0');
                    p++;
                    digits++;
                }
                putchar(val & 0xff);
                s = p - 1;
                break;
            }
            case 'x': {
                /* \xHH – hex value (1 to 2 digits) */
                int val = 0;
                int digits = 0;
                const char *p = s + 1;
                while (digits < 2 && is_hex(*p)) {
                    val = val * 16 + hex_val(*p);
                    p++;
                    digits++;
                }
                if (digits > 0) {
                    putchar(val & 0xff);
                    s = p - 1;
                } else {
                    /* \x without hex digits: print literally */
                    putchar('\\');
                    putchar('x');
                }
                break;
            }
            default:
                /* Unknown escape: print backslash + character */
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

static void usage(void)
{
    printf(
"Usage: %s [SHORT-OPTION]... [STRING]...\n"
"  or:  %s LONG-OPTION\n"
"Echo the STRING(s) to standard output.\n"
"\n"
"  -n     do not output the trailing newline\n"
"  -e     enable interpretation of backslash escapes\n"
"  -E     disable interpretation of backslash escapes (default)\n"
"  --help    display this help and exit\n"
"  --version output version information and exit\n"
"\n"
"If -e is in effect, the following sequences are recognized:\n"
"  \\\\   backslash           \\a   alert (BEL)\n"
"  \\b   backspace           \\c   produce no further output\n"
"  \\e   escape              \\f   form feed\n"
"  \\n   new line            \\r   carriage return\n"
"  \\t   horizontal tab      \\v   vertical tab\n"
"  \\0NNN byte with octal value NNN (1 to 3 digits)\n"
"  \\xHH  byte with hexadecimal value HH (1 to 2 digits)\n",
    PROGRAM_NAME, PROGRAM_NAME);
}

static void version(void)
{
    printf("%s %s\n", PROGRAM_NAME, VERSION);
}

int main(int argc, char *argv[])
{
    int arg_start = 1;

    /* Parse options manually (echo has unusual option parsing:
     * only leading option-like args are treated as options,
     * and only specific ones). */
    while (arg_start < argc) {
        const char *a = argv[arg_start];
        if (a[0] != '-' || a[1] == '\0')
            break;

        /* Check for --help and --version */
        if (strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            version();
            return 0;
        }

        /* Must be a sequence of only n, e, E characters */
        const char *p = a + 1;
        int valid = 1;
        while (*p) {
            if (*p != 'n' && *p != 'e' && *p != 'E') {
                valid = 0;
                break;
            }
            p++;
        }
        if (!valid)
            break;

        /* Process the flag characters */
        p = a + 1;
        while (*p) {
            switch (*p) {
            case 'n': opt_newline = 0; break;
            case 'e': opt_escapes = 1; break;
            case 'E': opt_escapes = 0; break;
            }
            p++;
        }
        arg_start++;
    }

    /* Print arguments separated by spaces */
    for (int i = arg_start; i < argc; i++) {
        if (i > arg_start)
            putchar(' ');
        if (print_arg(argv[i]))
            return 0;  /* \c encountered */
    }

    if (opt_newline)
        putchar('\n');

    fflush(stdout);
    return 0;
}
