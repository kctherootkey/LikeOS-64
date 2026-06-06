/*
 * cat - concatenate files and print on the standard output
 *
 * Full implementation per cat(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "cat"

static int opt_number_nonblank = 0;  /* -b */
static int opt_show_ends = 0;        /* -E */
static int opt_number = 0;           /* -n */
static int opt_squeeze_blank = 0;    /* -s */
static int opt_show_tabs = 0;        /* -T */
static int opt_show_nonprinting = 0; /* -v */

static void usage(void) {
    printf("Usage: %s [OPTION]... [FILE]...\n", PROGRAM_NAME);
    printf("Concatenate FILE(s) to standard output.\n\n");
    printf("With no FILE, or when FILE is -, read standard input.\n\n");
    printf("  -A, --show-all           equivalent to -vET\n");
    printf("  -b, --number-nonblank    number nonempty output lines, overrides -n\n");
    printf("  -e                       equivalent to -vE\n");
    printf("  -E, --show-ends          display $ at end of each line\n");
    printf("  -n, --number             number all output lines\n");
    printf("  -s, --squeeze-blank      suppress repeated empty output lines\n");
    printf("  -t                       equivalent to -vT\n");
    printf("  -T, --show-tabs          display TAB characters as ^I\n");
    printf("  -u                       (ignored)\n");
    printf("  -v, --show-nonprinting   use ^ and M- notation, except for LFD and TAB\n");
    printf("      --help               display this help and exit\n");
    printf("      --version            output version information and exit\n");
    printf("\nExamples:\n");
    printf("  cat f - g  Output f's contents, then standard input, then g's contents.\n");
    printf("  cat        Copy standard input to standard output.\n");
}

static void version(void) {
    printf("%s %s\n", PROGRAM_NAME, VERSION);
}

/* Write exactly n bytes to stdout */
static int write_all(const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, buf + off, n - off);
        if (w < 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/* Simple cat: no options active, just copy data */
static int simple_cat(int fd) {
    char buf[4096];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) return -1;
        if (r == 0) break;
        if (write_all(buf, (size_t)r) < 0) return -1;
    }
    return 0;
}

/* Cat with options: line-by-line processing */
static int cat_file(int fd) {
    /* Check if any option requires line processing */
    int need_processing = opt_number_nonblank || opt_show_ends ||
                          opt_number || opt_squeeze_blank ||
                          opt_show_tabs || opt_show_nonprinting;

    if (!need_processing)
        return simple_cat(fd);

    static unsigned long line_number = 1;
    static int last_was_blank = 0;
    char buf[4096];
    char outbuf[16384];
    int out_pos = 0;
    int at_line_start = 1;

    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) return -1;
        if (r == 0) break;

        for (ssize_t i = 0; i < r; i++) {
            unsigned char c = (unsigned char)buf[i];

            /* Handle newline */
            if (c == '\n') {
                int is_blank = at_line_start;

                if (opt_squeeze_blank && is_blank && last_was_blank) {
                    at_line_start = 1;
                    continue;
                }
                last_was_blank = is_blank;

                /* Number the line at start if needed */
                if (at_line_start) {
                    if (opt_number && !opt_number_nonblank) {
                        int n = snprintf(outbuf + out_pos,
                                         sizeof(outbuf) - (size_t)out_pos,
                                         "%6lu\t", line_number++);
                        out_pos += n;
                    }
                    /* -b: blank lines don't get numbers */
                }

                if (opt_show_ends)
                    outbuf[out_pos++] = '$';

                outbuf[out_pos++] = '\n';
                at_line_start = 1;

                if (out_pos > (int)(sizeof(outbuf) - 256)) {
                    if (write_all(outbuf, (size_t)out_pos) < 0) return -1;
                    out_pos = 0;
                }
                continue;
            }

            last_was_blank = 0;

            /* Number at start of line */
            if (at_line_start) {
                if (opt_number_nonblank || opt_number) {
                    int n = snprintf(outbuf + out_pos,
                                     sizeof(outbuf) - (size_t)out_pos,
                                     "%6lu\t", line_number++);
                    out_pos += n;
                }
                at_line_start = 0;
            }

            /* Tab handling */
            if (c == '\t') {
                if (opt_show_tabs) {
                    outbuf[out_pos++] = '^';
                    outbuf[out_pos++] = 'I';
                } else {
                    outbuf[out_pos++] = '\t';
                }
            }
            /* Show non-printing characters */
            else if (opt_show_nonprinting) {
                if (c >= 128) {
                    outbuf[out_pos++] = 'M';
                    outbuf[out_pos++] = '-';
                    c -= 128;
                }
                if (c < 32) {
                    outbuf[out_pos++] = '^';
                    outbuf[out_pos++] = (char)(c + 64);
                } else if (c == 127) {
                    outbuf[out_pos++] = '^';
                    outbuf[out_pos++] = '?';
                } else {
                    outbuf[out_pos++] = (char)c;
                }
            } else {
                outbuf[out_pos++] = (char)c;
            }

            if (out_pos > (int)(sizeof(outbuf) - 256)) {
                if (write_all(outbuf, (size_t)out_pos) < 0) return -1;
                out_pos = 0;
            }
        }
    }

    if (out_pos > 0) {
        if (write_all(outbuf, (size_t)out_pos) < 0) return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"show-all",         no_argument, 0, 'A'},
        {"number-nonblank",  no_argument, 0, 'b'},
        {"show-ends",        no_argument, 0, 'E'},
        {"number",           no_argument, 0, 'n'},
        {"squeeze-blank",    no_argument, 0, 's'},
        {"show-tabs",        no_argument, 0, 'T'},
        {"show-nonprinting", no_argument, 0, 'v'},
        {"help",             no_argument, 0, 'H'},
        {"version",          no_argument, 0, 'V'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "AbeEnstTuv", long_options, NULL)) != -1) {
        switch (c) {
            case 'A':
                opt_show_nonprinting = 1;
                opt_show_ends = 1;
                opt_show_tabs = 1;
                break;
            case 'b':
                opt_number_nonblank = 1;
                break;
            case 'e':
                opt_show_nonprinting = 1;
                opt_show_ends = 1;
                break;
            case 'E':
                opt_show_ends = 1;
                break;
            case 'n':
                opt_number = 1;
                break;
            case 's':
                opt_squeeze_blank = 1;
                break;
            case 't':
                opt_show_nonprinting = 1;
                opt_show_tabs = 1;
                break;
            case 'T':
                opt_show_tabs = 1;
                break;
            case 'u':
                /* ignored */
                break;
            case 'v':
                opt_show_nonprinting = 1;
                break;
            case 'H':
                usage();
                return 0;
            case 'V':
                version();
                return 0;
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
                return 1;
        }
    }

    /* -b overrides -n */
    if (opt_number_nonblank)
        opt_number = 0;

    int errors = 0;

    if (optind >= argc) {
        /* No files: read stdin */
        if (cat_file(STDIN_FILENO) != 0) {
            fprintf(stderr, "%s: -: %s\n", PROGRAM_NAME, strerror(errno));
            errors++;
        }
    } else {
        for (int i = optind; i < argc; i++) {
            if (strcmp(argv[i], "-") == 0) {
                if (cat_file(STDIN_FILENO) != 0) {
                    fprintf(stderr, "%s: -: %s\n", PROGRAM_NAME, strerror(errno));
                    errors++;
                }
            } else {
                int fd = open(argv[i], O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, argv[i], strerror(errno));
                    errors++;
                    continue;
                }
                if (cat_file(fd) != 0) {
                    fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, argv[i], strerror(errno));
                    errors++;
                }
                close(fd);
            }
        }
    }

    return errors > 0 ? 1 : 0;
}
