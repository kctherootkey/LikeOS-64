/*
 * hexdump - display file contents in hexadecimal, decimal, octal, or ascii
 *
 * Usage: hexdump [options] [file ...]
 *
 * Options:
 *   -b           One-byte octal display
 *   -c           One-byte character display
 *   -C           Canonical hex+ASCII display
 *   -d           Two-byte decimal display
 *   -e format    Specify a format string
 *   -f file      Specify a format file
 *   -n length    Interpret only length bytes of input
 *   -o           Two-byte octal display
 *   -s offset    Skip offset bytes from the beginning
 *   -v           Display all input data (no squeezing)
 *   -x           Two-byte hexadecimal display (default)
 *   -h, --help   Display help
 *   -V, --version Display version
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

/* ================================================================
 * Format unit: describes one output element
 * ================================================================ */
enum fmt_conv {
    FMT_HEX,     /* %x */
    FMT_DEC,     /* %d, %u */
    FMT_OCT,     /* %o */
    FMT_CHAR,    /* %c, _c */
    FMT_STRING,  /* %s, _p */
    FMT_ADDR,    /* %_a, %_A */
    FMT_LITERAL, /* literal characters */
};

struct fmt_unit {
    enum fmt_conv conv;
    int byte_count;     /* bytes consumed per iteration (1, 2, 4) */
    int iteration_count;/* how many times to repeat */
    char literal[64];   /* for FMT_LITERAL */
    int lit_len;
    char pad_char;      /* '0' or ' ' */
    int width;          /* field width */
    int uppercase;      /* for hex: uppercase letters */
    int addr_type;      /* for _a: 'd','o','x' */
};

#define MAX_FMT_UNITS 64

struct format {
    struct fmt_unit units[MAX_FMT_UNITS];
    int count;
    int bytes_per_line; /* total bytes consumed per output line */
};

#define MAX_FORMATS 16
static struct format g_formats[MAX_FORMATS];
static int g_nformats = 0;

/* Options */
static long opt_length = -1;   /* -n: max bytes to read, -1 = all */
static long opt_skip = 0;      /* -s: bytes to skip */
static int opt_verbose = 0;    /* -v: no squeezing of duplicate lines */

/* ================================================================
 * Parse offset with optional radix suffix
 * Supports 0x prefix, and suffixes b (512), k (1024), m (1048576)
 * ================================================================ */
static long parse_offset(const char *s)
{
    char *end;
    long val = strtol(s, &end, 0);
    while (*end) {
        switch (*end) {
        case 'b': val *= 512; end++; break;
        case 'k': case 'K': val *= 1024; end++; break;
        case 'm': case 'M': val *= 1048576; end++; break;
        default: return val;
        }
    }
    return val;
}

/* ================================================================
 * Setup built-in formats
 * ================================================================ */

/* -x: Two-byte hexadecimal display (default) */
static void setup_hex16(void)
{
    struct format *f = &g_formats[g_nformats++];
    f->count = 0;
    f->bytes_per_line = 16;

    /* Address */
    struct fmt_unit *u = &f->units[f->count++];
    u->conv = FMT_ADDR;
    u->addr_type = 'x';
    u->width = 7;
    u->pad_char = '0';

    /* 8 x 2-byte hex values */
    u = &f->units[f->count++];
    u->conv = FMT_HEX;
    u->byte_count = 2;
    u->iteration_count = 8;
    u->width = 5;
    u->pad_char = ' ';

    /* Newline */
    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = '\n';
    u->lit_len = 1;
}

/* -o: Two-byte octal display */
static void setup_oct16(void)
{
    struct format *f = &g_formats[g_nformats++];
    f->count = 0;
    f->bytes_per_line = 16;

    struct fmt_unit *u = &f->units[f->count++];
    u->conv = FMT_ADDR;
    u->addr_type = 'x';
    u->width = 7;
    u->pad_char = '0';

    u = &f->units[f->count++];
    u->conv = FMT_OCT;
    u->byte_count = 2;
    u->iteration_count = 8;
    u->width = 7;
    u->pad_char = ' ';

    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = '\n';
    u->lit_len = 1;
}

/* -d: Two-byte decimal display */
static void setup_dec16(void)
{
    struct format *f = &g_formats[g_nformats++];
    f->count = 0;
    f->bytes_per_line = 16;

    struct fmt_unit *u = &f->units[f->count++];
    u->conv = FMT_ADDR;
    u->addr_type = 'x';
    u->width = 7;
    u->pad_char = '0';

    u = &f->units[f->count++];
    u->conv = FMT_DEC;
    u->byte_count = 2;
    u->iteration_count = 8;
    u->width = 6;
    u->pad_char = ' ';

    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = '\n';
    u->lit_len = 1;
}

/* -b: One-byte octal display */
static void setup_oct8(void)
{
    struct format *f = &g_formats[g_nformats++];
    f->count = 0;
    f->bytes_per_line = 16;

    struct fmt_unit *u = &f->units[f->count++];
    u->conv = FMT_ADDR;
    u->addr_type = 'x';
    u->width = 7;
    u->pad_char = '0';

    u = &f->units[f->count++];
    u->conv = FMT_OCT;
    u->byte_count = 1;
    u->iteration_count = 16;
    u->width = 4;
    u->pad_char = ' ';

    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = '\n';
    u->lit_len = 1;
}

/* -c: One-byte character display */
static void setup_char(void)
{
    struct format *f = &g_formats[g_nformats++];
    f->count = 0;
    f->bytes_per_line = 16;

    struct fmt_unit *u = &f->units[f->count++];
    u->conv = FMT_ADDR;
    u->addr_type = 'x';
    u->width = 7;
    u->pad_char = '0';

    u = &f->units[f->count++];
    u->conv = FMT_CHAR;
    u->byte_count = 1;
    u->iteration_count = 16;
    u->width = 4;
    u->pad_char = ' ';

    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = '\n';
    u->lit_len = 1;
}

/* -C: Canonical hex+ASCII display */
static void setup_canonical(void)
{
    struct format *f = &g_formats[g_nformats++];
    f->count = 0;
    f->bytes_per_line = 16;

    /* Address */
    struct fmt_unit *u = &f->units[f->count++];
    u->conv = FMT_ADDR;
    u->addr_type = 'x';
    u->width = 8;
    u->pad_char = '0';

    /* First 8 bytes hex */
    u = &f->units[f->count++];
    u->conv = FMT_HEX;
    u->byte_count = 1;
    u->iteration_count = 8;
    u->width = 3;
    u->pad_char = ' ';

    /* Extra space between groups */
    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = ' ';
    u->lit_len = 1;

    /* Next 8 bytes hex */
    u = &f->units[f->count++];
    u->conv = FMT_HEX;
    u->byte_count = 1;
    u->iteration_count = 8;
    u->width = 3;
    u->pad_char = ' ';

    /* "  |" */
    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = ' ';
    u->literal[1] = ' ';
    u->literal[2] = '|';
    u->lit_len = 3;

    /* ASCII representation */
    u = &f->units[f->count++];
    u->conv = FMT_STRING;
    u->byte_count = 1;
    u->iteration_count = 16;
    u->width = 0; /* no padding */

    /* "|" */
    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = '|';
    u->lit_len = 1;

    /* Newline */
    u = &f->units[f->count++];
    u->conv = FMT_LITERAL;
    u->literal[0] = '\n';
    u->lit_len = 1;
}

/* ================================================================
 * Parse -e format string
 *
 * Format: "repeat/byte_count \"format_string\""
 * ================================================================ */
static int parse_escape(const char **pp)
{
    const char *p = *pp;
    p++; /* skip backslash */
    int ch;
    switch (*p) {
    case 'a':  ch = '\a'; p++; break;
    case 'b':  ch = '\b'; p++; break;
    case 'f':  ch = '\f'; p++; break;
    case 'n':  ch = '\n'; p++; break;
    case 'r':  ch = '\r'; p++; break;
    case 't':  ch = '\t'; p++; break;
    case 'v':  ch = '\v'; p++; break;
    case '\\': ch = '\\'; p++; break;
    case '"':  ch = '"';  p++; break;
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
        ch = 0;
        for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++, p++)
            ch = ch * 8 + (*p - '0');
        break;
    }
    default: ch = *p; if (*p) p++; break;
    }
    *pp = p;
    return ch;
}

static void parse_format_string(const char *s)
{
    if (g_nformats >= MAX_FORMATS) {
        fprintf(stderr, "hexdump: too many format strings\n");
        exit(1);
    }

    struct format *f = &g_formats[g_nformats++];
    f->count = 0;
    f->bytes_per_line = 0;

    /* Parse: [repeat_count/byte_count] "fmt" ... */
    while (*s) {
        /* Skip whitespace */
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;

        int repeat = 1;
        int byte_cnt = 1;

        /* Optional repeat/byte_count */
        if (isdigit((unsigned char)*s)) {
            repeat = (int)strtol(s, (char**)&s, 10);
            if (*s == '/') {
                s++;
                byte_cnt = (int)strtol(s, (char**)&s, 10);
            }
            while (*s == ' ' || *s == '\t') s++;
        }

        if (*s != '"') {
            /* Might be just whitespace at end */
            break;
        }
        s++; /* skip opening quote */

        /* Parse format string until closing quote */
        while (*s && *s != '"') {
            if (f->count >= MAX_FMT_UNITS) break;

            if (*s == '%') {
                s++;
                struct fmt_unit *u = &f->units[f->count++];
                memset(u, 0, sizeof(*u));
                u->iteration_count = repeat;
                u->byte_count = byte_cnt;
                u->pad_char = ' ';

                /* Flags */
                if (*s == '0') { u->pad_char = '0'; s++; }

                /* Width */
                u->width = 0;
                while (isdigit((unsigned char)*s)) {
                    u->width = u->width * 10 + (*s - '0');
                    s++;
                }

                /* Conversion */
                switch (*s) {
                case 'd': case 'i':
                    u->conv = FMT_DEC;
                    s++;
                    break;
                case 'o':
                    u->conv = FMT_OCT;
                    s++;
                    break;
                case 'x':
                    u->conv = FMT_HEX;
                    u->uppercase = 0;
                    s++;
                    break;
                case 'X':
                    u->conv = FMT_HEX;
                    u->uppercase = 1;
                    s++;
                    break;
                case 'u':
                    u->conv = FMT_DEC;
                    s++;
                    break;
                case 'c':
                    u->conv = FMT_CHAR;
                    u->byte_count = 1;
                    s++;
                    break;
                case 's':
                    u->conv = FMT_STRING;
                    s++;
                    break;
                case '_': {
                    s++;
                    switch (*s) {
                    case 'a':
                        s++;
                        u->conv = FMT_ADDR;
                        u->addr_type = *s ? *s : 'x';
                        if (*s) s++;
                        u->iteration_count = 0;
                        u->byte_count = 0;
                        break;
                    case 'A':
                        s++;
                        u->conv = FMT_ADDR;
                        u->addr_type = *s ? *s : 'x';
                        if (*s) s++;
                        u->iteration_count = 0;
                        u->byte_count = 0;
                        break;
                    case 'c':
                        u->conv = FMT_CHAR;
                        u->byte_count = 1;
                        s++;
                        break;
                    case 'p':
                        u->conv = FMT_STRING;
                        u->byte_count = 1;
                        s++;
                        break;
                    case 'u':
                        u->conv = FMT_CHAR;
                        u->byte_count = 1;
                        s++;
                        break;
                    default:
                        /* Unknown _ conversion, treat as literal */
                        f->count--;
                        break;
                    }
                    break;
                }
                case '%':
                    u->conv = FMT_LITERAL;
                    u->literal[0] = '%';
                    u->lit_len = 1;
                    u->iteration_count = 0;
                    u->byte_count = 0;
                    s++;
                    break;
                default:
                    /* Unknown conversion, skip */
                    f->count--;
                    if (*s) s++;
                    break;
                }

                f->bytes_per_line += u->iteration_count * u->byte_count;
            } else if (*s == '\\') {
                /* Escape sequence as literal */
                struct fmt_unit *u = &f->units[f->count++];
                memset(u, 0, sizeof(*u));
                u->conv = FMT_LITERAL;
                u->literal[0] = (char)parse_escape(&s);
                u->lit_len = 1;
            } else {
                /* Literal character */
                struct fmt_unit *u = &f->units[f->count++];
                memset(u, 0, sizeof(*u));
                u->conv = FMT_LITERAL;
                u->literal[0] = *s++;
                u->lit_len = 1;
            }
        }
        if (*s == '"') s++;
        while (*s == ' ' || *s == '\t') s++;
    }

    if (f->bytes_per_line == 0)
        f->bytes_per_line = 16;
}

/* ================================================================
 * Parse -f format file
 * ================================================================ */
static void parse_format_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "hexdump: cannot open '%s': %s\n", path, strerror(errno));
        exit(1);
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        int len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0) continue;
        parse_format_string(line);
    }
    fclose(fp);
}

/* ================================================================
 * Read a value from buffer (little-endian)
 * ================================================================ */
static unsigned long read_val(const unsigned char *buf, int bytes, int avail)
{
    unsigned long val = 0;
    int n = bytes < avail ? bytes : avail;
    for (int i = 0; i < n; i++)
        val |= (unsigned long)buf[i] << (i * 8);
    return val;
}

/* ================================================================
 * Print a single format unit value
 * ================================================================ */
static void print_unit_value(const struct fmt_unit *u, unsigned long val,
                             const unsigned char *rawbyte, int avail)
{
    char buf[64];
    int len = 0;

    switch (u->conv) {
    case FMT_HEX:
        if (u->uppercase)
            len = snprintf(buf, sizeof(buf), "%lX", val);
        else
            len = snprintf(buf, sizeof(buf), "%lx", val);
        break;
    case FMT_DEC:
        len = snprintf(buf, sizeof(buf), "%lu", val);
        break;
    case FMT_OCT:
        len = snprintf(buf, sizeof(buf), "%lo", val);
        break;
    case FMT_CHAR: {
        unsigned char c = (unsigned char)(val & 0xFF);
        if (c == '\0')       { buf[0]='\\'; buf[1]='0'; len=2; }
        else if (c == '\a')  { buf[0]='\\'; buf[1]='a'; len=2; }
        else if (c == '\b')  { buf[0]='\\'; buf[1]='b'; len=2; }
        else if (c == '\f')  { buf[0]='\\'; buf[1]='f'; len=2; }
        else if (c == '\n')  { buf[0]='\\'; buf[1]='n'; len=2; }
        else if (c == '\r')  { buf[0]='\\'; buf[1]='r'; len=2; }
        else if (c == '\t')  { buf[0]='\\'; buf[1]='t'; len=2; }
        else if (c == '\v')  { buf[0]='\\'; buf[1]='v'; len=2; }
        else if (c >= 0x20 && c <= 0x7e) { buf[0]=(char)c; len=1; }
        else { len = snprintf(buf, sizeof(buf), "%03o", c); }
        break;
    }
    case FMT_STRING: {
        unsigned char c = (unsigned char)(val & 0xFF);
        if (c >= 0x20 && c <= 0x7e)
            buf[0] = (char)c;
        else
            buf[0] = '.';
        len = 1;
        break;
    }
    default:
        return;
    }
    buf[len] = '\0';

    /* Padding */
    if (u->width > len) {
        int pad = u->width - len;
        for (int i = 0; i < pad; i++)
            putchar(u->pad_char);
    }
    fputs(buf, stdout);
}

/* ================================================================
 * Print address
 * ================================================================ */
static void print_addr(const struct fmt_unit *u, unsigned long addr)
{
    char buf[32];
    int len;
    switch (u->addr_type) {
    case 'd': len = snprintf(buf, sizeof(buf), "%lu", addr); break;
    case 'o': len = snprintf(buf, sizeof(buf), "%lo", addr); break;
    case 'x':
    default:  len = snprintf(buf, sizeof(buf), "%lx", addr); break;
    }
    if (u->width > len) {
        int pad = u->width - len;
        for (int i = 0; i < pad; i++)
            putchar(u->pad_char);
    }
    fputs(buf, stdout);
}

/* ================================================================
 * Main dump loop
 * ================================================================ */

/* Buffer for reading input */
#define BUF_SIZE 65536
static unsigned char g_buf[BUF_SIZE];

static void do_dump(FILE **files, int nfiles)
{
    /* Determine bytes per line from first format */
    int bytes_per_line = 16;
    if (g_nformats > 0 && g_formats[0].bytes_per_line > 0)
        bytes_per_line = g_formats[0].bytes_per_line;

    unsigned long offset = (unsigned long)opt_skip;
    long remaining = opt_length;

    /* Open and skip in files */
    int cur_file = 0;
    unsigned char linebuf[256];
    unsigned char prev_line[256];
    int prev_valid = 0;
    int star_printed = 0;

    /* Read line by line */
    while (1) {
        if (remaining == 0) break;

        /* Fill linebuf */
        int got = 0;
        while (got < bytes_per_line) {
            if (remaining >= 0 && got >= remaining) break;

            int ch;
            while (cur_file < nfiles) {
                ch = fgetc(files[cur_file]);
                if (ch != EOF) break;
                cur_file++;
            }
            if (cur_file >= nfiles) break;
            linebuf[got++] = (unsigned char)ch;
        }
        if (got == 0) break;

        if (remaining > 0) remaining -= got;

        /* Squeeze duplicate lines unless -v */
        if (!opt_verbose && prev_valid && got == bytes_per_line) {
            if (memcmp(linebuf, prev_line, got) == 0) {
                if (!star_printed) {
                    printf("*\n");
                    star_printed = 1;
                }
                offset += got;
                continue;
            }
        }
        star_printed = 0;
        memcpy(prev_line, linebuf, got);
        prev_valid = (got == bytes_per_line);

        /* Output each format */
        for (int fi = 0; fi < g_nformats; fi++) {
            struct format *f = &g_formats[fi];
            int pos = 0; /* byte position within linebuf */

            for (int ui = 0; ui < f->count; ui++) {
                struct fmt_unit *u = &f->units[ui];

                if (u->conv == FMT_LITERAL) {
                    for (int i = 0; i < u->lit_len; i++)
                        putchar(u->literal[i]);
                    continue;
                }

                if (u->conv == FMT_ADDR) {
                    print_addr(u, offset);
                    continue;
                }

                /* Data conversion units */
                int iters = u->iteration_count;
                for (int it = 0; it < iters; it++) {
                    int avail = got - pos;
                    if (avail <= 0) {
                        /* Pad remaining with spaces */
                        if (u->width > 0) {
                            for (int w = 0; w < u->width; w++)
                                putchar(' ');
                        }
                    } else {
                        unsigned long val = read_val(linebuf + pos, u->byte_count, avail);
                        print_unit_value(u, val, linebuf + pos, avail);
                        pos += u->byte_count;
                    }
                }
            }
        }

        offset += got;
    }

    /* Print final offset (end marker) */
    if (g_nformats > 0) {
        /* Find first address unit and print final offset */
        for (int ui = 0; ui < g_formats[0].count; ui++) {
            if (g_formats[0].units[ui].conv == FMT_ADDR) {
                print_addr(&g_formats[0].units[ui], offset);
                putchar('\n');
                break;
            }
        }
    }
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: hexdump [OPTION]... [FILE]...\n"
        "Display file contents in hexadecimal, decimal, octal, or ascii.\n"
        "\n"
        "  -b           one-byte octal display\n"
        "  -c           one-byte character display\n"
        "  -C           canonical hex+ASCII display\n"
        "  -d           two-byte decimal display\n"
        "  -e FORMAT    specify a format string\n"
        "  -f FILE      specify a format file\n"
        "  -n LENGTH    interpret only LENGTH bytes of input\n"
        "  -o           two-byte octal display\n"
        "  -s OFFSET    skip OFFSET bytes from the beginning\n"
        "  -v           display all input data (no squeezing)\n"
        "  -x           two-byte hexadecimal display (default)\n"
        "  -h, --help   display this help and exit\n"
        "  -V, --version output version information and exit\n");
}

int main(int argc, char **argv)
{
    int builtin_format = 0; /* 0=none set, will use -x default */

    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *opt = argv[i];

        if (strcmp(opt, "--") == 0) { i++; break; }
        if (strcmp(opt, "--help") == 0 || strcmp(opt, "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(opt, "--version") == 0 || strcmp(opt, "-V") == 0) {
            printf("hexdump (LikeOS) 1.0\n");
            return 0;
        }

        /* Single-char options */
        const char *p = opt + 1;
        while (*p) {
            switch (*p) {
            case 'b':
                setup_oct8();
                builtin_format = 1;
                p++;
                break;
            case 'c':
                setup_char();
                builtin_format = 1;
                p++;
                break;
            case 'C':
                setup_canonical();
                builtin_format = 1;
                p++;
                break;
            case 'd':
                setup_dec16();
                builtin_format = 1;
                p++;
                break;
            case 'o':
                setup_oct16();
                builtin_format = 1;
                p++;
                break;
            case 'x':
                setup_hex16();
                builtin_format = 1;
                p++;
                break;
            case 'v':
                opt_verbose = 1;
                p++;
                break;
            case 'e':
                p++;
                if (*p) {
                    parse_format_string(p);
                    builtin_format = 1;
                } else {
                    i++;
                    if (i >= argc) { fprintf(stderr, "hexdump: -e requires argument\n"); return 1; }
                    parse_format_string(argv[i]);
                    builtin_format = 1;
                }
                goto next_arg;
            case 'f':
                p++;
                if (*p) {
                    parse_format_file(p);
                    builtin_format = 1;
                } else {
                    i++;
                    if (i >= argc) { fprintf(stderr, "hexdump: -f requires argument\n"); return 1; }
                    parse_format_file(argv[i]);
                    builtin_format = 1;
                }
                goto next_arg;
            case 'n':
                p++;
                if (*p) {
                    opt_length = parse_offset(p);
                } else {
                    i++;
                    if (i >= argc) { fprintf(stderr, "hexdump: -n requires argument\n"); return 1; }
                    opt_length = parse_offset(argv[i]);
                }
                goto next_arg;
            case 's':
                p++;
                if (*p) {
                    opt_skip = parse_offset(p);
                } else {
                    i++;
                    if (i >= argc) { fprintf(stderr, "hexdump: -s requires argument\n"); return 1; }
                    opt_skip = parse_offset(argv[i]);
                }
                goto next_arg;
            default:
                fprintf(stderr, "hexdump: invalid option -- '%c'\n", *p);
                usage();
                return 1;
            }
        }
next_arg:
        i++;
    }

    /* Default format if none specified */
    if (!builtin_format) {
        setup_hex16();
    }

    /* Open files */
    FILE *files[256];
    int nfiles = 0;

    if (i >= argc) {
        /* stdin */
        files[nfiles++] = stdin;
    } else {
        for (; i < argc && nfiles < 256; i++) {
            if (strcmp(argv[i], "-") == 0) {
                files[nfiles++] = stdin;
            } else {
                FILE *fp = fopen(argv[i], "r");
                if (!fp) {
                    fprintf(stderr, "hexdump: cannot open '%s': %s\n",
                            argv[i], strerror(errno));
                    return 1;
                }
                files[nfiles++] = fp;
            }
        }
    }

    /* Skip bytes */
    if (opt_skip > 0) {
        long to_skip = opt_skip;
        for (int fi = 0; fi < nfiles && to_skip > 0; fi++) {
            while (to_skip > 0) {
                int ch = fgetc(files[fi]);
                if (ch == EOF) break;
                to_skip--;
            }
        }
    }

    do_dump(files, nfiles);

    /* Close files */
    for (int fi = 0; fi < nfiles; fi++) {
        if (files[fi] != stdin)
            fclose(files[fi]);
    }

    return 0;
}
