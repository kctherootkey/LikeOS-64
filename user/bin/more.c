/*
 * more - display the contents of a file in a terminal
 *
 * Full implementation per more(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <getopt.h>

#define VERSION "1.0"
#define PROGRAM_NAME "more"

/* Options */
static int opt_silent = 0;        /* -d: friendly prompts */
static int opt_logical = 0;       /* -l: don't pause at ^L */
static int opt_exit_on_eof = 0;   /* -e: exit on EOF */
static int opt_no_pause = 0;      /* -f: count logical lines */
static int opt_print_over = 0;    /* -p: clear screen before display */
static int opt_clean_print = 0;   /* -c: paint from top */
static int opt_squeeze = 0;       /* -s: squeeze blank lines */
static int opt_plain = 0;         /* -u: suppress underline (ignored) */
static int opt_lines = 0;         /* -n: lines per screenful (0 = use terminal) */
static int opt_start_line = 0;    /* +number: start at line */
static char *opt_search = NULL;   /* +/string: search before display */

/* Terminal info */
static int term_rows = 25;
static int term_cols = 80;
static struct termios saved_termios;
static int raw_mode = 0;
static int tty_fd = -1;  /* fd for keyboard/terminal control (/dev/tty when piped) */

/* File list */
static int file_count = 0;
static int file_index = 0;
static char **file_names = NULL;

/* Line buffer */
#define LINE_BUF_SIZE 8192
#define MAX_LINES 65536

/* File content buffer */
static char **lines = NULL;
static int total_lines = 0;
static int current_line = 0;
static int scroll_size = 11;
static int lines_capacity = 0;

/* Pipe incremental reading */
static int pipe_fd = -1;
static char pipe_linebuf[LINE_BUF_SIZE];
static int pipe_lpos = 0;
static int pipe_prev_blank = 0;

static void get_term_size(void) {
    struct winsize ws;
    int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        term_rows = ws.ws_row;
        term_cols = ws.ws_col;
    }
    if (opt_lines > 0)
        term_rows = opt_lines;
}

static void enter_raw_mode(void) {
    int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
    if (!isatty(fd))
        return;
    tcgetattr(fd, &saved_termios);
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(fd, TCSANOW, &raw);
    raw_mode = 1;
}

static void exit_raw_mode(void) {
    if (raw_mode) {
        int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
        tcsetattr(fd, TCSANOW, &saved_termios);
        raw_mode = 0;
    }
}

static void write_str(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

static void clear_screen(void) {
    write_str("\033[?25l\033[2J\033[H\033[?25h");
}

static void clear_line(void) {
    write_str("\r\033[K");
}

/* Read a single character from terminal */
static int read_char(void) {
    char c;
    int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
    if (read(fd, &c, 1) != 1)
        return -1;
    return (unsigned char)c;
}

/* Display the --More-- prompt */
static void show_prompt(const char *filename) {
    char buf[256];
    if (pipe_fd >= 0) {
        /* Pipe still open — don't show percentage, it would be misleading */
        snprintf(buf, sizeof(buf), "--More--");
    } else if (total_lines > 0) {
        int pct = (current_line * 100) / total_lines;
        if (pct > 100) pct = 100;
        snprintf(buf, sizeof(buf), "--More--(%d%%)", pct);
    } else {
        snprintf(buf, sizeof(buf), "--More--");
    }
    /* Display in reverse video */
    write_str("\033[7m");
    write_str(buf);
    write_str("\033[0m");
    (void)filename;
}

static void clear_prompt(void) {
    clear_line();
}

/* Free loaded lines */
static void free_lines(void) {
    if (lines) {
        for (int i = 0; i < total_lines; i++)
            free(lines[i]);
        free(lines);
        lines = NULL;
    }
    total_lines = 0;
    current_line = 0;
    lines_capacity = 0;
}

static int add_line_more(const char *text, int len) {
    if (total_lines >= lines_capacity) {
        int new_cap = lines_capacity ? lines_capacity * 2 : 1024;
        if (new_cap > MAX_LINES) new_cap = MAX_LINES;
        if (total_lines >= new_cap) return -1;
        char **new_lines = (char **)realloc(lines, (size_t)new_cap * sizeof(char *));
        if (!new_lines) return -1;
        lines = new_lines;
        lines_capacity = new_cap;
    }
    lines[total_lines] = (char *)malloc((size_t)(len + 1));
    if (!lines[total_lines]) return -1;
    memcpy(lines[total_lines], text, (size_t)len);
    lines[total_lines][len] = '\0';
    total_lines++;
    return 0;
}

/* Read more data from pipe (non-blocking). Returns 1 if new lines, 0 if nothing, -1 on EOF */
static int pipe_read_more(void) {
    if (pipe_fd < 0) return -1;
    char buf[4096];
    int added = 0;
    for (;;) {
        ssize_t r = read(pipe_fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            goto eof;
        }
        if (r == 0) goto eof;
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || pipe_lpos >= LINE_BUF_SIZE - 1) {
                int is_blank = (pipe_lpos == 0);
                if (opt_squeeze && is_blank && pipe_prev_blank)
                    { pipe_lpos = 0; continue; }
                pipe_prev_blank = is_blank;
                add_line_more(pipe_linebuf, pipe_lpos);
                pipe_lpos = 0;
                added = 1;
            } else {
                pipe_linebuf[pipe_lpos++] = buf[i];
            }
        }
    }
    return added ? 1 : 0;
eof:
    if (pipe_lpos > 0) { add_line_more(pipe_linebuf, pipe_lpos); pipe_lpos = 0; added = 1; }
    pipe_fd = -1;
    return added ? 1 : -1;
}

/* Load file into lines array. Returns 0 on success. */
static int load_file(int fd) {
    free_lines();

    char buf[4096];
    char linebuf[LINE_BUF_SIZE];
    int line_pos = 0;
    int prev_blank = 0;

    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) return -1;
        if (r == 0) break;

        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || line_pos >= LINE_BUF_SIZE - 1) {
                int is_blank = (line_pos == 0);
                if (opt_squeeze && is_blank && prev_blank)
                    { line_pos = 0; continue; }
                prev_blank = is_blank;
                if (add_line_more(linebuf, line_pos) < 0) return -1;
                line_pos = 0;
            } else {
                linebuf[line_pos++] = buf[i];
            }
        }
    }

    if (line_pos > 0) {
        if (add_line_more(linebuf, line_pos) < 0) return -1;
    }

    return 0;
}

/* Load initial data from pipe, then return for interactive paging */
static int load_pipe_more(int fd) {
    free_lines();
    pipe_lpos = 0;
    pipe_prev_blank = 0;

    int fl = fcntl(fd, F_GETFL);
    pipe_fd = fd;  /* set immediately so EOF paths can clear it */

    int screen_lines = term_rows - 1;
    int need_lines = screen_lines + 1;

    /* Read at least one screenful (blocking) */
    char buf[4096];
    while (total_lines < need_lines) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            pipe_fd = -1;
            return total_lines > 0 ? 0 : -1;
        }
        if (r == 0) {
            if (pipe_lpos > 0) { add_line_more(pipe_linebuf, pipe_lpos); pipe_lpos = 0; }
            pipe_fd = -1;
            break;
        }
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || pipe_lpos >= LINE_BUF_SIZE - 1) {
                int is_blank = (pipe_lpos == 0);
                if (opt_squeeze && is_blank && pipe_prev_blank)
                    { pipe_lpos = 0; continue; }
                pipe_prev_blank = is_blank;
                add_line_more(pipe_linebuf, pipe_lpos);
                pipe_lpos = 0;
            } else {
                pipe_linebuf[pipe_lpos++] = buf[i];
            }
        }
    }

    /* Set non-blocking for subsequent reads */
    if (pipe_fd >= 0) {
        fcntl(pipe_fd, F_SETFL, fl | O_NONBLOCK);
    }

    return 0;
}

/* Display one screenful of lines starting at current_line */
static void display_screen(int num_lines) {
    int displayed = 0;
    while (displayed < num_lines) {
        /* If we've consumed all loaded lines but pipe is still open, block for more */
        if (current_line >= total_lines && pipe_fd >= 0) {
            int fl = fcntl(pipe_fd, F_GETFL);
            fcntl(pipe_fd, F_SETFL, fl & ~O_NONBLOCK);  /* blocking */

            char buf[4096];
            int got_lines = 0;
            while (!got_lines && pipe_fd >= 0) {
                ssize_t r = read(pipe_fd, buf, sizeof(buf));
                if (r <= 0) {
                    if (pipe_lpos > 0) {
                        add_line_more(pipe_linebuf, pipe_lpos);
                        pipe_lpos = 0;
                    }
                    pipe_fd = -1;
                    break;
                }
                for (ssize_t i = 0; i < r; i++) {
                    if (buf[i] == '\n' || pipe_lpos >= LINE_BUF_SIZE - 1) {
                        int is_blank = (pipe_lpos == 0);
                        if (opt_squeeze && is_blank && pipe_prev_blank)
                            { pipe_lpos = 0; continue; }
                        pipe_prev_blank = is_blank;
                        add_line_more(pipe_linebuf, pipe_lpos);
                        pipe_lpos = 0;
                        got_lines = 1;
                    } else {
                        pipe_linebuf[pipe_lpos++] = buf[i];
                    }
                }
            }

            /* Restore non-blocking */
            if (pipe_fd >= 0)
                fcntl(pipe_fd, F_SETFL, fl);
        }

        if (current_line >= total_lines)
            break;

        write_str(lines[current_line]);
        write_str("\n");
        current_line++;
        displayed++;
    }
}

/* Search for a pattern starting from current_line */
static int search_forward(const char *pattern) {
    for (int i = current_line; i < total_lines; i++) {
        if (strstr(lines[i], pattern) != NULL) {
            current_line = i;
            return 0;
        }
    }
    return -1;  /* Not found */
}

/* Display help screen */
static void show_help(void) {
    write_str("\n");
    write_str("Most commands can be preceded by a decimal number k.\n");
    write_str("SPACE        Display next k lines (default: screen size)\n");
    write_str("z            Display next k lines (default: screen size)\n");
    write_str("RETURN       Display next k lines (default: 1)\n");
    write_str("d or ^D      Scroll k lines (default: 11)\n");
    write_str("q or Q       Exit\n");
    write_str("s            Skip forward k lines (default: 1)\n");
    write_str("f            Skip forward k screenfuls (default: 1)\n");
    write_str("b or ^B      Skip backwards k screenfuls (default: 1)\n");
    write_str("=            Display current line number\n");
    write_str("/pattern     Search for pattern\n");
    write_str("n            Search for next occurrence of last pattern\n");
    write_str(":n           Go to next file\n");
    write_str(":p           Go to previous file\n");
    write_str(":f           Display current file name and line number\n");
    write_str("h or ?       Display this help\n");
    write_str(".            Repeat previous command\n");
}

/* Main pager loop for one file */
static int page_file(const char *filename) {
    int screen_lines = term_rows - 1; /* Reserve one for prompt */
    static char last_search[256] = {0};
    int prev_cmd = ' ';
    int prev_count = 0;

    /* Handle +number: start at specified line */
    if (opt_start_line > 0) {
        current_line = opt_start_line - 1;
        if (current_line < 0) current_line = 0;
        if (current_line > total_lines) current_line = total_lines;
    }

    /* Handle +/string: search before display */
    if (opt_search) {
        if (search_forward(opt_search) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "\nPattern not found: %s\n", opt_search);
            write_str(msg);
        }
    }

    /* Display first screen */
    if (opt_print_over || opt_clean_print)
        clear_screen();

    display_screen(screen_lines);

    if (current_line >= total_lines) {
        if (opt_exit_on_eof || (pipe_fd < 0 && tty_fd < 0 && !isatty(STDIN_FILENO)))
            return 0;
    }

    /* Interactive loop */
    for (;;) {
        /* Pull in new data from pipe (non-blocking) */
        pipe_read_more();
        if (current_line >= total_lines && pipe_fd < 0) {
            if (opt_exit_on_eof)
                return 0;
            /* Show (END) prompt */
            write_str("\033[7m(END)\033[0m");
            int ch = read_char();
            clear_prompt();
            if (ch == 'q' || ch == 'Q' || ch == 3) /* q, Q, or Ctrl-C */
                return -1;  /* Signal exit */
            if (ch == ':') {
                int ch2 = read_char();
                if (ch2 == 'n') return 1;   /* Next file */
                if (ch2 == 'p') return -2;  /* Prev file */
            }
            continue;
        }

        show_prompt(filename);

        /* Read numeric prefix */
        int count = 0;
        int has_count = 0;
        int ch = read_char();
        if (ch < 0) return -1;

        while (ch >= '0' && ch <= '9') {
            count = count * 10 + (ch - '0');
            has_count = 1;
            ch = read_char();
            if (ch < 0) return -1;
        }

        clear_prompt();

        switch (ch) {
            case ' ':  /* Display next screen */
            case 'z': {
                int n = has_count ? count : screen_lines;
                if (ch == 'z' && has_count)
                    screen_lines = count; /* New default */
                if (opt_print_over || opt_clean_print)
                    clear_screen();
                display_screen(n);
                prev_cmd = ch;
                prev_count = n;
                break;
            }

            case '\n':  /* Display next line(s) */
            case '\r': {
                int n = has_count ? count : 1;
                display_screen(n);
                prev_cmd = '\n';
                prev_count = n;
                break;
            }

            case 'd':   /* Scroll half screen */
            case 4: {   /* Ctrl-D */
                int n = has_count ? count : scroll_size;
                if (has_count) scroll_size = count;
                display_screen(n);
                prev_cmd = 'd';
                prev_count = n;
                break;
            }

            case 'q':
            case 'Q':
            case 3:   /* Ctrl-C */
                return -1;

            case 's': {  /* Skip forward lines */
                int n = has_count ? count : 1;
                current_line += n;
                if (current_line > total_lines)
                    current_line = total_lines;
                display_screen(screen_lines);
                prev_cmd = 's';
                prev_count = n;
                break;
            }

            case 'f': {  /* Skip forward screens */
                int n = has_count ? count : 1;
                current_line += n * screen_lines;
                if (current_line > total_lines)
                    current_line = total_lines;
                display_screen(screen_lines);
                prev_cmd = 'f';
                prev_count = n;
                break;
            }

            case 'b':    /* Skip backwards */
            case 2: {    /* Ctrl-B */
                int n = has_count ? count : 1;
                current_line -= n * screen_lines;
                if (current_line < 0) current_line = 0;
                if (opt_print_over || opt_clean_print)
                    clear_screen();
                display_screen(screen_lines);
                prev_cmd = 'b';
                prev_count = n;
                break;
            }

            case '=': {  /* Display current line number */
                char msg[64];
                snprintf(msg, sizeof(msg), "\n%d\n", current_line);
                write_str(msg);
                break;
            }

            case '/': {  /* Search */
                write_str("/");
                char pattern[256];
                int plen = 0;
                for (;;) {
                    int pc = read_char();
                    if (pc == '\n' || pc == '\r') break;
                    if (pc == 127 || pc == 8) {
                        if (plen > 0) {
                            plen--;
                            write_str("\b \b");
                        }
                        continue;
                    }
                    if (plen < (int)sizeof(pattern) - 1) {
                        pattern[plen++] = (char)pc;
                        char ch_buf[2] = {(char)pc, 0};
                        write_str(ch_buf);
                    }
                }
                pattern[plen] = '\0';
                write_str("\n");

                if (plen > 0)
                    strncpy(last_search, pattern, sizeof(last_search) - 1);

                int repeat = has_count ? count : 1;
                int found = 0;
                int save = current_line;
                for (int k = 0; k < repeat; k++) {
                    if (search_forward(last_search) == 0) {
                        found = 1;
                        current_line++; /* Move past match for next search */
                    }
                }
                if (found) {
                    current_line--; /* Back to last match */
                    display_screen(screen_lines);
                } else {
                    current_line = save;
                    char msg[300];
                    snprintf(msg, sizeof(msg), "Pattern not found: %s\n", last_search);
                    write_str(msg);
                }
                prev_cmd = '/';
                break;
            }

            case 'n': {  /* Repeat search */
                if (last_search[0] == '\0') {
                    if (opt_silent)
                        write_str("[No previous search pattern]\n");
                    else
                        write_str("\007"); /* bell */
                    break;
                }
                int repeat = has_count ? count : 1;
                int found = 0;
                int save = current_line;
                for (int k = 0; k < repeat; k++) {
                    if (search_forward(last_search) == 0) {
                        found = 1;
                        current_line++;
                    }
                }
                if (found) {
                    current_line--;
                    display_screen(screen_lines);
                } else {
                    current_line = save;
                    char msg[300];
                    snprintf(msg, sizeof(msg), "Pattern not found: %s\n", last_search);
                    write_str(msg);
                }
                prev_cmd = 'n';
                break;
            }

            case 'h':
            case '?':
                show_help();
                break;

            case ':': {
                int ch2 = read_char();
                if (ch2 == 'n') {
                    return 1;  /* Next file */
                } else if (ch2 == 'p') {
                    return -2;  /* Previous file */
                } else if (ch2 == 'f') {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "\n\"%s\" line %d\n",
                             filename, current_line);
                    write_str(msg);
                }
                break;
            }

            case 12:  /* Ctrl-L: Redraw */
                clear_screen();
                /* Redraw from current position minus one screenful */
                {
                    int start = current_line - screen_lines;
                    if (start < 0) start = 0;
                    current_line = start;
                    display_screen(screen_lines);
                }
                break;

            case '.':  /* Repeat previous command */
                /* Simplified: re-execute last movement */
                if (prev_cmd == ' ' || prev_cmd == 'z')
                    display_screen(prev_count ? prev_count : screen_lines);
                else if (prev_cmd == '\n')
                    display_screen(prev_count ? prev_count : 1);
                else if (prev_cmd == 'd')
                    display_screen(prev_count ? prev_count : scroll_size);
                break;

            default:
                if (opt_silent) {
                    write_str("[Press space to continue, 'q' to quit.]\n");
                } else {
                    write_str("\007"); /* bell */
                }
                break;
        }
    }
}

static void usage(void) {
    printf("Usage: %s [options] file ...\n\n", PROGRAM_NAME);
    printf("A filter for paging through text one screenful at a time.\n\n");
    printf("Options:\n");
    printf("  -d, --silent         friendly prompts\n");
    printf("  -l, --logical        do not pause after ^L\n");
    printf("  -e, --exit-on-eof    exit on End-Of-File\n");
    printf("  -f, --no-pause       count logical lines\n");
    printf("  -p, --print-over     clear screen before display\n");
    printf("  -c, --clean-print    paint from top, clearing each line\n");
    printf("  -s, --squeeze        squeeze multiple blank lines\n");
    printf("  -u, --plain          suppress underlining (ignored)\n");
    printf("  -n, --lines number   lines per screenful\n");
    printf("  +number              start at line number\n");
    printf("  +/string             search for string before display\n");
    printf("  -h, --help           display help text and exit\n");
    printf("  -V, --version        print version and exit\n");
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"silent",      no_argument,       0, 'd'},
        {"logical",     no_argument,       0, 'l'},
        {"exit-on-eof", no_argument,       0, 'e'},
        {"no-pause",    no_argument,       0, 'f'},
        {"print-over",  no_argument,       0, 'p'},
        {"clean-print", no_argument,       0, 'c'},
        {"squeeze",     no_argument,       0, 's'},
        {"plain",       no_argument,       0, 'u'},
        {"lines",       required_argument, 0, 'n'},
        {"help",        no_argument,       0, 'h'},
        {"version",     no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    /* Process +number and +/string before getopt */
    /* We need to handle these before getopt since they start with + */
    int real_argc = 0;
    char **real_argv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!real_argv) return 1;

    real_argv[real_argc++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+' && argv[i][1] == '/') {
            opt_search = argv[i] + 2;
        } else if (argv[i][0] == '+' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            opt_start_line = atoi(argv[i] + 1);
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            /* -number means --lines number */
            opt_lines = atoi(argv[i] + 1);
        } else {
            real_argv[real_argc++] = argv[i];
        }
    }
    real_argv[real_argc] = NULL;

    int c;
    optind = 1;
    while ((c = getopt_long(real_argc, real_argv, "dlefpcsun:hV", long_options, NULL)) != -1) {
        switch (c) {
            case 'd': opt_silent = 1; break;
            case 'l': opt_logical = 1; break;
            case 'e': opt_exit_on_eof = 1; break;
            case 'f': opt_no_pause = 1; break;
            case 'p': opt_print_over = 1; break;
            case 'c': opt_clean_print = 1; break;
            case 's': opt_squeeze = 1; break;
            case 'u': opt_plain = 1; break;
            case 'n':
                opt_lines = atoi(optarg);
                break;
            case 'h':
                usage();
                free(real_argv);
                return 0;
            case 'V':
                printf("%s %s\n", PROGRAM_NAME, VERSION);
                free(real_argv);
                return 0;
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", PROGRAM_NAME);
                free(real_argv);
                return 1;
        }
    }

    /* Suppress unused warnings */
    (void)opt_logical;
    (void)opt_no_pause;
    (void)opt_plain;

    /* If stdin is not a tty (piped), open /dev/tty for keyboard input */
    if (!isatty(STDIN_FILENO)) {
        tty_fd = open("/dev/tty", O_RDWR);
    }

    get_term_size();
    enter_raw_mode();

    file_count = real_argc - optind;
    file_names = &real_argv[optind];
    file_index = 0;

    int ret = 0;

    if (file_count == 0) {
        /* Read stdin (pipe) — use incremental loading */
        if (load_pipe_more(STDIN_FILENO) != 0) {
            fprintf(stderr, "%s: cannot read standard input: %s\n",
                    PROGRAM_NAME, strerror(errno));
            ret = 1;
        } else {
            page_file("(stdin)");
        }
    } else {
        while (file_index < file_count) {
            const char *fname = file_names[file_index];

            if (file_count > 1) {
                char header[512];
                snprintf(header, sizeof(header), "\n:::::::::::::\n%s\n:::::::::::::\n", fname);
                write_str(header);
            }

            int fd = open(fname, O_RDONLY);
            if (fd < 0) {
                char msg[512];
                snprintf(msg, sizeof(msg), "%s: %s: %s\n",
                         PROGRAM_NAME, fname, strerror(errno));
                write_str(msg);
                file_index++;
                ret = 1;
                continue;
            }

            if (load_file(fd) != 0) {
                char msg[512];
                snprintf(msg, sizeof(msg), "%s: %s: read error\n",
                         PROGRAM_NAME, fname);
                write_str(msg);
                close(fd);
                file_index++;
                ret = 1;
                continue;
            }
            close(fd);

            int result = page_file(fname);
            if (result == -1) {
                /* User quit */
                break;
            } else if (result == -2) {
                /* Previous file */
                file_index--;
                if (file_index < 0) file_index = 0;
            } else {
                /* Next file (result >= 0) */
                file_index++;
            }
        }
    }

    write_str("\n");
    free_lines();
    exit_raw_mode();
    if (pipe_fd >= 0) {
        close(pipe_fd);
        pipe_fd = -1;
    }
    if (tty_fd >= 0) {
        close(tty_fd);
        tty_fd = -1;
    }
    free(real_argv);
    return ret;
}
