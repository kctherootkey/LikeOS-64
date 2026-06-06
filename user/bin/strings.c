/*
 * strings - print the sequences of printable characters in files
 *
 * Implements all GNU binutils strings options per manpage.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>

#define PROGRAM_NAME "strings"
#define VERSION      "1.0"

#define DEFAULT_MIN_LEN 4
#define MAX_STRING_BUF  (1024 * 1024)  /* 1MB max string buffer */

/* Encoding types */
enum encoding {
    ENC_S = 0,  /* single 7-bit byte (default) */
    ENC_S_CAP,  /* single 8-bit byte */
    ENC_B,      /* 16-bit big endian */
    ENC_L,      /* 16-bit little endian */
    ENC_B_CAP,  /* 32-bit big endian */
    ENC_L_CAP,  /* 32-bit little endian */
};

/* Radix types for -t */
enum radix {
    RAD_NONE = 0,
    RAD_OCTAL,
    RAD_HEX,
    RAD_DECIMAL,
};

static int opt_print_filename = 0;       /* -f */
static int opt_min_len = DEFAULT_MIN_LEN;/* -n */
static enum radix opt_radix = RAD_NONE;  /* -t */
static enum encoding opt_encoding = ENC_S;/* -e */
static int opt_include_whitespace = 0;   /* -w */
static const char *opt_separator = NULL; /* -s */

static void usage(int status)
{
    if (status != EXIT_SUCCESS) {
        fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
    } else {
        printf("Usage: %s [OPTION]... [FILE]...\n"
               "Print the sequences of printable characters in files.\n\n"
               "  -a, --all, -              Scan the entire file (default)\n"
               "  -d, --data                Only scan data sections (ignored, scans all)\n"
               "  -f, --print-file-name     Print the name of the file before each string\n"
               "  -n, --bytes=MIN-LEN       Print sequences of at least MIN-LEN characters (default 4)\n"
               "      -MIN-LEN              Shorthand for -n MIN-LEN\n"
               "  -o                        Like -t o (print offset in octal)\n"
               "  -t, --radix=RADIX         Print the offset within the file using RADIX:\n"
               "                            o (octal), x (hex), d (decimal)\n"
               "  -w, --include-all-whitespace  Include all whitespace as valid string characters\n"
               "  -e, --encoding=ENCODING   Select character encoding:\n"
               "                            s = 7-bit (default), S = 8-bit, b = 16-bit big-endian,\n"
               "                            l = 16-bit little-endian, B = 32-bit big-endian,\n"
               "                            L = 32-bit little-endian\n"
               "  -s, --output-separator=SEP  Use SEP instead of newline as output separator\n"
               "  -v, -V, --version         Print version and exit\n"
               "      --help                Display this help and exit\n",
               PROGRAM_NAME);
    }
    exit(status);
}

static void version(void)
{
    printf("%s (%s) %s\n", PROGRAM_NAME, "LikeOS binutils", VERSION);
    exit(EXIT_SUCCESS);
}

static int is_string_char(int c)
{
    if (opt_include_whitespace) {
        /* Include all whitespace (tab, newline, vertical tab, form feed, carriage return, space) */
        if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == ' ')
            return 1;
    }
    /* Standard: printable ASCII (and tab) */
    if (c >= 32 && c <= 126)
        return 1;
    if (c == '\t')
        return 1;
    return 0;
}

static int is_string_char_8bit(int c)
{
    if (opt_include_whitespace) {
        if (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == ' ')
            return 1;
    }
    if (c >= 32 && c <= 126)
        return 1;
    if (c >= 128 && c <= 255)
        return 1;
    if (c == '\t')
        return 1;
    return 0;
}

static void print_offset(unsigned long offset)
{
    switch (opt_radix) {
    case RAD_OCTAL:
        printf("%7lo ", offset);
        break;
    case RAD_HEX:
        printf("%7lx ", offset);
        break;
    case RAD_DECIMAL:
        printf("%7lu ", offset);
        break;
    case RAD_NONE:
        break;
    }
}

static void print_string(const char *filename, const char *str, int len, unsigned long offset)
{
    if (opt_print_filename)
        printf("%s: ", filename);
    print_offset(offset);
    fwrite(str, 1, len, stdout);
    if (opt_separator)
        printf("%s", opt_separator);
    else
        putchar('\n');
}

static int process_file_7bit(FILE *fp, const char *filename)
{
    char *buf = malloc(MAX_STRING_BUF);
    if (!buf) {
        fprintf(stderr, "%s: memory allocation failed\n", PROGRAM_NAME);
        return 1;
    }

    int c;
    int len = 0;
    unsigned long offset = 0;
    unsigned long start_offset = 0;

    while ((c = fgetc(fp)) != EOF) {
        if (is_string_char(c)) {
            if (len == 0)
                start_offset = offset;
            if (len < MAX_STRING_BUF - 1)
                buf[len++] = (char)c;
        } else {
            if (len >= opt_min_len) {
                buf[len] = '\0';
                print_string(filename, buf, len, start_offset);
            }
            len = 0;
        }
        offset++;
    }
    /* Flush any remaining string */
    if (len >= opt_min_len) {
        buf[len] = '\0';
        print_string(filename, buf, len, start_offset);
    }

    free(buf);
    return 0;
}

static int process_file_8bit(FILE *fp, const char *filename)
{
    char *buf = malloc(MAX_STRING_BUF);
    if (!buf) {
        fprintf(stderr, "%s: memory allocation failed\n", PROGRAM_NAME);
        return 1;
    }

    int c;
    int len = 0;
    unsigned long offset = 0;
    unsigned long start_offset = 0;

    while ((c = fgetc(fp)) != EOF) {
        if (is_string_char_8bit(c)) {
            if (len == 0)
                start_offset = offset;
            if (len < MAX_STRING_BUF - 1)
                buf[len++] = (char)c;
        } else {
            if (len >= opt_min_len) {
                buf[len] = '\0';
                print_string(filename, buf, len, start_offset);
            }
            len = 0;
        }
        offset++;
    }
    if (len >= opt_min_len) {
        buf[len] = '\0';
        print_string(filename, buf, len, start_offset);
    }

    free(buf);
    return 0;
}

static int process_file_16(FILE *fp, const char *filename, int big_endian)
{
    char *buf = malloc(MAX_STRING_BUF);
    if (!buf) {
        fprintf(stderr, "%s: memory allocation failed\n", PROGRAM_NAME);
        return 1;
    }

    int len = 0;
    unsigned long offset = 0;
    unsigned long start_offset = 0;
    unsigned char pair[2];

    while (fread(pair, 1, 2, fp) == 2) {
        unsigned int ch;
        if (big_endian)
            ch = ((unsigned int)pair[0] << 8) | pair[1];
        else
            ch = pair[0] | ((unsigned int)pair[1] << 8);

        if (ch < 256 && is_string_char((int)ch)) {
            if (len == 0)
                start_offset = offset;
            if (len < MAX_STRING_BUF - 1)
                buf[len++] = (char)ch;
        } else {
            if (len >= opt_min_len) {
                buf[len] = '\0';
                print_string(filename, buf, len, start_offset);
            }
            len = 0;
        }
        offset += 2;
    }
    if (len >= opt_min_len) {
        buf[len] = '\0';
        print_string(filename, buf, len, start_offset);
    }

    free(buf);
    return 0;
}

static int process_file_32(FILE *fp, const char *filename, int big_endian)
{
    char *buf = malloc(MAX_STRING_BUF);
    if (!buf) {
        fprintf(stderr, "%s: memory allocation failed\n", PROGRAM_NAME);
        return 1;
    }

    int len = 0;
    unsigned long offset = 0;
    unsigned long start_offset = 0;
    unsigned char quad[4];

    while (fread(quad, 1, 4, fp) == 4) {
        unsigned long ch;
        if (big_endian)
            ch = ((unsigned long)quad[0] << 24) | ((unsigned long)quad[1] << 16) |
                 ((unsigned long)quad[2] << 8)  | quad[3];
        else
            ch = quad[0] | ((unsigned long)quad[1] << 8) |
                 ((unsigned long)quad[2] << 16) | ((unsigned long)quad[3] << 24);

        if (ch < 256 && is_string_char((int)ch)) {
            if (len == 0)
                start_offset = offset;
            if (len < MAX_STRING_BUF - 1)
                buf[len++] = (char)ch;
        } else {
            if (len >= opt_min_len) {
                buf[len] = '\0';
                print_string(filename, buf, len, start_offset);
            }
            len = 0;
        }
        offset += 4;
    }
    if (len >= opt_min_len) {
        buf[len] = '\0';
        print_string(filename, buf, len, start_offset);
    }

    free(buf);
    return 0;
}

static int process_file(FILE *fp, const char *filename)
{
    switch (opt_encoding) {
    case ENC_S:
        return process_file_7bit(fp, filename);
    case ENC_S_CAP:
        return process_file_8bit(fp, filename);
    case ENC_B:
        return process_file_16(fp, filename, 1);
    case ENC_L:
        return process_file_16(fp, filename, 0);
    case ENC_B_CAP:
        return process_file_32(fp, filename, 1);
    case ENC_L_CAP:
        return process_file_32(fp, filename, 0);
    default:
        return process_file_7bit(fp, filename);
    }
}

int main(int argc, char **argv)
{
    static struct option long_options[] = {
        {"all",                   no_argument,       0, 'a'},
        {"data",                  no_argument,       0, 'd'},
        {"print-file-name",       no_argument,       0, 'f'},
        {"bytes",                 required_argument, 0, 'n'},
        {"radix",                 required_argument, 0, 't'},
        {"encoding",              required_argument, 0, 'e'},
        {"include-all-whitespace",no_argument,       0, 'w'},
        {"output-separator",      required_argument, 0, 's'},
        {"version",               no_argument,       0, 'V'},
        {"help",                  no_argument,       0, 'H'},
        {0, 0, 0, 0}
    };

    int c;
    optind = 1;

    /* Pre-scan for -NUMBER shorthand (e.g. -4, -10) */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '1' && argv[i][1] <= '9') {
            int all_digits = 1;
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] < '0' || argv[i][j] > '9') {
                    all_digits = 0;
                    break;
                }
            }
            if (all_digits) {
                opt_min_len = atoi(&argv[i][1]);
                /* Remove this arg by shifting */
                for (int j = i; j < argc - 1; j++)
                    argv[j] = argv[j + 1];
                argc--;
                i--;
            }
        }
    }

    while ((c = getopt_long(argc, argv, "adfn:ot:e:wvVs:", long_options, NULL)) != -1) {
        switch (c) {
        case 'a':
            /* Scan all (default behavior) */
            break;
        case 'd':
            /* Data sections only - we always scan all, ignore */
            break;
        case 'f':
            opt_print_filename = 1;
            break;
        case 'n':
            opt_min_len = atoi(optarg);
            if (opt_min_len < 1) {
                fprintf(stderr, "%s: invalid minimum string length '%s'\n",
                        PROGRAM_NAME, optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'o':
            opt_radix = RAD_OCTAL;
            break;
        case 't':
            if (optarg[0] == 'o')
                opt_radix = RAD_OCTAL;
            else if (optarg[0] == 'x')
                opt_radix = RAD_HEX;
            else if (optarg[0] == 'd')
                opt_radix = RAD_DECIMAL;
            else {
                fprintf(stderr, "%s: invalid radix '%s'\n", PROGRAM_NAME, optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'e':
            switch (optarg[0]) {
            case 's': opt_encoding = ENC_S; break;
            case 'S': opt_encoding = ENC_S_CAP; break;
            case 'b': opt_encoding = ENC_B; break;
            case 'l': opt_encoding = ENC_L; break;
            case 'B': opt_encoding = ENC_B_CAP; break;
            case 'L': opt_encoding = ENC_L_CAP; break;
            default:
                fprintf(stderr, "%s: invalid encoding '%s'\n", PROGRAM_NAME, optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'w':
            opt_include_whitespace = 1;
            break;
        case 'v':
        case 'V':
            version();
            break;
        case 's':
            opt_separator = optarg;
            break;
        case 'H':
            usage(EXIT_SUCCESS);
            break;
        default:
            usage(EXIT_FAILURE);
            break;
        }
    }

    int ret = 0;
    if (optind >= argc) {
        /* No files - read from stdin */
        ret = process_file(stdin, "{standard input}");
    } else {
        for (int i = optind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) {
                if (process_file(stdin, "{standard input}") != 0)
                    ret = 1;
            } else {
                FILE *fp = fopen(argv[i], "r");
                if (!fp) {
                    fprintf(stderr, "%s: '%s': No such file\n", PROGRAM_NAME, argv[i]);
                    ret = 1;
                    continue;
                }
                if (process_file(fp, argv[i]) != 0)
                    ret = 1;
                fclose(fp);
            }
        }
    }

    return ret;
}
