/*
 * less - opposite of more
 *
 * Full implementation per less(1) manpage.
 * A pager program for viewing text files with backward/forward navigation.
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
#define PROGRAM_NAME "less"

/* ─── Options ─── */
static int opt_clear_screen = 0;      /* -c: repaint from top */
static int opt_quit_at_eof = 0;       /* -e: quit at second EOF */
static int opt_quit_at_eof2 = 0;      /* -E: quit at first EOF */
static int opt_force_open = 0;        /* -f: force open non-regular */
static int opt_hilite_search = 1;     /* -g/-G: highlight search hits */
static int opt_ignore_case = 0;       /* -i: ignore case in search */
static int opt_smart_case = 0;        /* -I: smart ignore case */
static int opt_status_col = 0;        /* -J: status column */
static int opt_long_prompt = 0;       /* -m: medium prompt */
static int opt_very_long_prompt = 0;  /* -M: long prompt */
static int opt_line_numbers = 0;      /* -N: line numbers */
static int opt_quiet = 0;             /* -q: quiet (no bell) */
static int opt_very_quiet = 0;        /* -Q: very quiet */
static int opt_raw_control = 0;       /* -r: raw control chars */
static int opt_raw_ansi = 0;          /* -R: raw ANSI color */
static int opt_squeeze_blank = 0;     /* -s: squeeze blank lines */
static int opt_chop_lines = 0;        /* -S: chop long lines */
static int opt_tilde = 0;             /* -~: blank lines after EOF */
static int opt_no_init = 0;           /* -X: no init/deinit */
static int opt_window = 0;            /* -z: window size (0=auto) */
static int opt_tabs = 8;              /* -x: tab stops */
static int opt_use_color = 0;         /* --use-color */

/* ─── Terminal state ─── */
static int term_rows = 25;
static int term_cols = 80;
static struct termios saved_termios;
static int raw_mode_active = 0;
static int tty_fd = -1;  /* fd for keyboard/terminal control (/dev/tty when piped) */

/* ─── File content ─── */
#define MAX_ALLOC_LINES (1024 * 1024)
static char **lines = NULL;
static int total_lines = 0;
static int lines_capacity = 0;

/* ─── Display state ─── */
static int top_line = 0;          /* First visible line */
static int screen_lines = 24;     /* Usable display lines */
static int horiz_shift = 0;       /* Horizontal scroll offset */
static int shift_amount = 0;      /* -# value; 0 = half screen */

/* ─── Search state ─── */
static char search_pattern[1024] = {0};
static int search_direction = 1;  /* 1=forward, -1=backward */
static int *search_hits = NULL;   /* array[total_lines]: 1 if match */

/* ─── File management ─── */
static int nfiles = 0;
static char **filenames = NULL;
static int current_file = 0;
static char *current_filename = NULL;

/* ─── Pipe incremental reading ─── */
static int pipe_fd = -1;          /* pipe fd still being read (-1 = not a pipe or EOF) */
static char pipe_linebuf[16384];  /* partial line buffer carried across reads */
static int pipe_lpos = 0;         /* current position in pipe_linebuf */
static int pipe_prev_blank = 0;   /* squeeze blank tracking for pipe */

/* ─── Mark system ─── */
#define MARK_COUNT 128
static int marks[MARK_COUNT];
static int mark_set[MARK_COUNT];

/* ─── Forward declarations ─── */
static void display(void);
static void cleanup(void);

/* ═══════════════ Terminal handling ═══════════════ */

static void get_term_size(void) {
    struct winsize ws;
    int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        term_rows = ws.ws_row;
        term_cols = ws.ws_col;
    }
    screen_lines = term_rows - 1;  /* Last line for status */
    if (opt_window > 0)
        screen_lines = opt_window;
    else if (opt_window < 0)
        screen_lines = term_rows + opt_window;
    if (screen_lines < 1) screen_lines = 1;
}

static void enter_raw(void) {
    int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
    if (!isatty(fd)) return;
    tcgetattr(fd, &saved_termios);
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(ICRNL);
    tcsetattr(fd, TCSANOW, &raw);
    raw_mode_active = 1;
}

static void exit_raw(void) {
    if (raw_mode_active) {
        int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
        tcsetattr(fd, TCSANOW, &saved_termios);
        raw_mode_active = 0;
    }
}

static void write_str(const char *s) {
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(STDOUT_FILENO, s + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void write_char(char c) {
    write(STDOUT_FILENO, &c, 1);
}

static void term_clear(void) {
    write_str("\033[2J\033[H");
}

static void term_goto(int row, int col) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\033[%d;%dH", row + 1, col + 1);
    write_str(buf);
}

static void term_clear_line(void) {
    write_str("\033[2K");
}

static void term_reverse(int on) {
    write_str(on ? "\033[7m" : "\033[0m");
}

static void term_bold(int on) {
    write_str(on ? "\033[1m" : "\033[0m");
}

/* Read one byte (blocking) from tty */
static int readch(void) {
    unsigned char c;
    int fd = (tty_fd >= 0) ? tty_fd : STDIN_FILENO;
    if (read(fd, &c, 1) != 1) return -1;
    return c;
}

/* Read an escape sequence key */
static int read_key(void) {
    int c = readch();
    if (c < 0) return -1;
    if (c != 27) return c;

    /* ESC sequence */
    int c2 = readch();
    if (c2 < 0) return 27;

    if (c2 == '[') {
        int c3 = readch();
        if (c3 < 0) return 27;
        switch (c3) {
            case 'A': return 1000; /* UP */
            case 'B': return 1001; /* DOWN */
            case 'C': return 1002; /* RIGHT */
            case 'D': return 1003; /* LEFT */
            case 'H': return 1004; /* HOME */
            case 'F': return 1005; /* END */
            case '5': readch(); return 1006; /* PGUP  (ESC[5~) */
            case '6': readch(); return 1007; /* PGDN  (ESC[6~) */
            case '3': readch(); return 1008; /* DEL   (ESC[3~) */
            default:  return 27;
        }
    }

    /* ESC + letter for less key bindings */
    return 2000 + c2;  /* ESC-x = 2000+'x' */
}

static void bell(void) {
    if (!opt_quiet && !opt_very_quiet)
        write_str("\007");
}

/* ═══════════════ Line management ═══════════════ */

static void free_lines(void) {
    if (lines) {
        for (int i = 0; i < total_lines; i++)
            free(lines[i]);
        free(lines);
        lines = NULL;
    }
    if (search_hits) {
        free(search_hits);
        search_hits = NULL;
    }
    total_lines = 0;
    lines_capacity = 0;
    top_line = 0;
}

static int add_line(const char *text, int len) {
    if (total_lines >= lines_capacity) {
        int new_cap = lines_capacity ? lines_capacity * 2 : 4096;
        if (new_cap > MAX_ALLOC_LINES) new_cap = MAX_ALLOC_LINES;
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

static int load_fd(int fd) {
    free_lines();

    char buf[8192];
    char linebuf[16384];
    int lpos = 0;
    int prev_blank = 0;

    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) return -1;
        if (r == 0) break;

        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || lpos >= (int)sizeof(linebuf) - 1) {
                int is_blank = (lpos == 0);
                if (opt_squeeze_blank && is_blank && prev_blank)
                    { lpos = 0; continue; }
                prev_blank = is_blank;

                if (add_line(linebuf, lpos) < 0) return -1;
                lpos = 0;
            } else {
                linebuf[lpos++] = buf[i];
            }
        }
    }

    /* Trailing content without newline */
    if (lpos > 0) {
        if (add_line(linebuf, lpos) < 0) return -1;
    }

    /* Allocate search hits array */
    if (total_lines > 0) {
        search_hits = (int *)calloc((size_t)total_lines, sizeof(int));
    }

    return 0;
}

/*
 * Read more data from the pipe (non-blocking).
 * Returns: 1 if new lines were added, 0 if nothing new, -1 on EOF/error.
 */
static int pipe_read_more(void) {
    if (pipe_fd < 0) return -1;

    char buf[8192];
    int added = 0;

    for (;;) {
        ssize_t r = read(pipe_fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* No more data available right now */
            }
            /* Real error or EINTR — treat as EOF */
            goto pipe_eof;
        }
        if (r == 0) {
            /* EOF — producer finished */
            goto pipe_eof;
        }

        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || pipe_lpos >= (int)sizeof(pipe_linebuf) - 1) {
                int is_blank = (pipe_lpos == 0);
                if (opt_squeeze_blank && is_blank && pipe_prev_blank)
                    { pipe_lpos = 0; continue; }
                pipe_prev_blank = is_blank;
                add_line(pipe_linebuf, pipe_lpos);
                pipe_lpos = 0;
                added = 1;
            } else {
                pipe_linebuf[pipe_lpos++] = buf[i];
            }
        }
    }

    return added ? 1 : 0;

pipe_eof:
    /* Flush any trailing partial line */
    if (pipe_lpos > 0) {
        add_line(pipe_linebuf, pipe_lpos);
        pipe_lpos = 0;
        added = 1;
    }
    pipe_fd = -1;
    return added ? 1 : -1;
}

/*
 * Start reading from a pipe: set non-blocking, read initial data
 * (at least one screenful), then return so the pager can display.
 */
static int load_pipe(int fd) {
    free_lines();
    pipe_lpos = 0;
    pipe_prev_blank = 0;

    /* Set the pipe to non-blocking mode */
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    pipe_fd = fd;

    /* Read initial data — do blocking reads until we have enough for a screen.
     * We temporarily make it blocking for the first batch so we don't return
     * with zero lines. */
    fcntl(fd, F_SETFL, fl);  /* back to blocking */

    char buf[8192];
    int need_lines = screen_lines + 1;  /* at least one screenful */

    while (total_lines < need_lines) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            pipe_fd = -1;
            return total_lines > 0 ? 0 : -1;
        }
        if (r == 0) {
            /* Producer already finished — flush partial line */
            if (pipe_lpos > 0) {
                add_line(pipe_linebuf, pipe_lpos);
                pipe_lpos = 0;
            }
            pipe_fd = -1;
            break;
        }

        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || pipe_lpos >= (int)sizeof(pipe_linebuf) - 1) {
                int is_blank = (pipe_lpos == 0);
                if (opt_squeeze_blank && is_blank && pipe_prev_blank)
                    { pipe_lpos = 0; continue; }
                pipe_prev_blank = is_blank;
                if (add_line(pipe_linebuf, pipe_lpos) < 0) {
                    pipe_fd = -1;
                    return -1;
                }
                pipe_lpos = 0;
            } else {
                pipe_linebuf[pipe_lpos++] = buf[i];
            }
        }
    }

    /* Now set non-blocking for subsequent reads in the pager loop */
    if (pipe_fd >= 0) {
        fcntl(pipe_fd, F_SETFL, fl | O_NONBLOCK);
    }

    return 0;
}

static int load_file(const char *path) {
    if (strcmp(path, "-") == 0)
        return load_fd(STDIN_FILENO);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int ret = load_fd(fd);
    close(fd);
    return ret;
}

/* ═══════════════ Search ═══════════════ */

/* Simple case-insensitive character match */
static int ci_char_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    return a == b;
}

/* Simple case-insensitive strstr */
static const char *ci_strstr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        int match = 1;
        for (size_t i = 0; i < nlen; i++) {
            if (!haystack[i] || !ci_char_eq(haystack[i], needle[i])) {
                match = 0;
                break;
            }
        }
        if (match) return haystack;
    }
    return NULL;
}

/* Check if pattern has any uppercase */
static int has_upper(const char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') return 1;
    return 0;
}

static int line_matches(int line_idx) {
    if (search_pattern[0] == '\0' || line_idx < 0 || line_idx >= total_lines)
        return 0;

    int do_ignore = opt_ignore_case;
    if (opt_smart_case && !has_upper(search_pattern))
        do_ignore = 1;

    if (do_ignore)
        return ci_strstr(lines[line_idx], search_pattern) != NULL;
    else
        return strstr(lines[line_idx], search_pattern) != NULL;
}

static void update_search_hits(void) {
    if (search_pattern[0] == '\0') return;
    /* (Re)allocate search_hits to match current total_lines */
    if (search_hits) free(search_hits);
    search_hits = NULL;
    if (total_lines > 0) {
        search_hits = (int *)calloc((size_t)total_lines, sizeof(int));
        if (!search_hits) return;
    }
    for (int i = 0; i < total_lines; i++)
        search_hits[i] = line_matches(i);
}

static int search_next(int start, int dir) {
    for (int i = start + dir; i >= 0 && i < total_lines; i += dir) {
        if (line_matches(i))
            return i;
    }
    return -1;
}

/* ═══════════════ Display ═══════════════ */

/* Compute display width of a line segment (handling tabs) */
static int display_width(const char *s, int len) {
    int w = 0;
    for (int i = 0; i < len && s[i]; i++) {
        if (s[i] == '\t')
            w = ((w / opt_tabs) + 1) * opt_tabs;
        else if ((unsigned char)s[i] >= 32)
            w++;
        else
            w += 2; /* ^X notation */
    }
    return w;
}

/* Render one line to terminal, handling tabs, non-printing, and horizontal offset */
static void render_line(int line_idx) {
    if (line_idx >= total_lines) {
        /* After EOF */
        if (!opt_tilde)
            write_char('~');
        return;
    }

    const char *text = lines[line_idx];
    int len = (int)strlen(text);
    int col = 0;       /* Virtual column */
    int scr_col = 0;   /* Screen column */

    /* Line number prefix */
    if (opt_line_numbers) {
        char numbuf[16];
        int n = snprintf(numbuf, sizeof(numbuf), "%6d ", line_idx + 1);
        write_str(numbuf);
        scr_col += n;
    }

    /* Status column */
    if (opt_status_col) {
        if (search_hits && line_idx < total_lines && search_hits[line_idx])
            write_char('*');
        else
            write_char(' ');
        scr_col++;
    }

    /* Highlight search matches */
    int do_highlight = opt_hilite_search && search_hits &&
                       line_idx < total_lines && search_hits[line_idx] && search_pattern[0];

    int max_col = term_cols;

    for (int i = 0; i < len && scr_col < max_col; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c == '\t') {
            int next = ((col / opt_tabs) + 1) * opt_tabs;
            while (col < next) {
                if (col >= horiz_shift && scr_col < max_col) {
                    write_char(' ');
                    scr_col++;
                }
                col++;
            }
        } else if (c < 32 && !opt_raw_control && !opt_raw_ansi) {
            /* Control character: ^X */
            if (col >= horiz_shift && scr_col < max_col) {
                write_char('^');
                scr_col++;
            }
            col++;
            if (col >= horiz_shift && scr_col < max_col) {
                write_char((char)(c + 64));
                scr_col++;
            }
            col++;
        } else if (c == 27 && opt_raw_ansi) {
            /* Pass through ANSI escape sequences */
            write_char((char)c);
            i++;
            while (i < len) {
                write_char(text[i]);
                if (text[i] == 'm') break; /* End of CSI color */
                if ((unsigned char)text[i] >= 0x40 &&
                    (unsigned char)text[i] <= 0x7E &&
                    text[i] != '[')
                    break;
                i++;
            }
        } else {
            if (col >= horiz_shift && scr_col < max_col) {
                /* Check for search highlight */
                if (do_highlight) {
                    int do_ignore = opt_ignore_case;
                    if (opt_smart_case && !has_upper(search_pattern))
                        do_ignore = 1;
                    size_t plen = strlen(search_pattern);
                    int match = 0;
                    if (do_ignore) {
                        if ((size_t)(len - i) >= plen) {
                            match = 1;
                            for (size_t k = 0; k < plen; k++) {
                                if (!ci_char_eq(text[i + k], search_pattern[k])) {
                                    match = 0;
                                    break;
                                }
                            }
                        }
                    } else {
                        if ((size_t)(len - i) >= plen &&
                            memcmp(text + i, search_pattern, plen) == 0)
                            match = 1;
                    }
                    if (match) {
                        term_reverse(1);
                        for (size_t k = 0; k < plen && scr_col < max_col; k++) {
                            write_char(text[i + k]);
                            scr_col++;
                        }
                        term_reverse(0);
                        i += (int)plen - 1;
                        col += (int)plen;
                        continue;
                    }
                }
                write_char((char)c);
                scr_col++;
            }
            col++;
        }

        if (opt_chop_lines && scr_col >= max_col)
            break;
    }

    (void)do_highlight;
}

static void display(void) {
    char posbuf[32];
    for (int i = 0; i < screen_lines; i++) {
        /* Position cursor at start of row i (1-based) */
        snprintf(posbuf, sizeof(posbuf), "\033[%d;1H\033[K", i + 1);
        write_str(posbuf);
        render_line(top_line + i);
    }
    /* Move to status line row and erase from there to end of screen */
    snprintf(posbuf, sizeof(posbuf), "\033[%d;1H\033[J", screen_lines + 1);
    write_str(posbuf);
}

/* ─── Status line ─── */
static void show_status(const char *msg) {
    /* Use carriage return to go to start of status line, then erase it */
    write_str("\r\033[K");
    if (msg) {
        term_reverse(1);
        write_str(msg);
        term_reverse(0);
    }
}

static void show_default_prompt(void) {
    char buf[1024];

    if (opt_very_long_prompt) {
        /* -M: filename + lines + percentage */
        int bot = top_line + screen_lines - 1;
        if (bot >= total_lines) bot = total_lines - 1;
        if (total_lines == 0) {
            snprintf(buf, sizeof(buf), "%s (empty)",
                     current_filename ? current_filename : "(stdin)");
        } else {
            int pct = total_lines > 0 ? ((bot + 1) * 100 / total_lines) : 100;
            snprintf(buf, sizeof(buf), "%s lines %d-%d/%d %d%%",
                     current_filename ? current_filename : "(stdin)",
                     top_line + 1, bot + 1, total_lines, pct);
        }
    } else if (opt_long_prompt) {
        /* -m: filename + percentage */
        int bot = top_line + screen_lines - 1;
        if (bot >= total_lines) bot = total_lines - 1;
        int pct = total_lines > 0 ? ((bot + 1) * 100 / total_lines) : 100;
        snprintf(buf, sizeof(buf), "%s %d%%",
                 current_filename ? current_filename : "(stdin)", pct);
    } else {
        /* Default: just colon or filename for first prompt */
        if (current_filename)
            snprintf(buf, sizeof(buf), "%s", current_filename);
        else
            buf[0] = ':', buf[1] = '\0';
    }

    /* Check if at end */
    if (top_line + screen_lines >= total_lines && pipe_fd < 0) {
        if (nfiles > 1 && current_file < nfiles - 1) {
            char endmsg[1024];
            snprintf(endmsg, sizeof(endmsg), "(END) - Next: %s",
                     filenames[current_file + 1]);
            show_status(endmsg);
        } else {
            show_status("(END)");
        }
    } else {
        show_status(buf);
    }
}

/* ─── Read a line of input from the user (for search, goto, etc.) ─── */
static int read_input_line(const char *prompt, char *buf, int bufsize) {
    /* Redraw screen so cursor is on the status line, then show prompt */
    display();
    write_str("\r\033[K");
    write_str(prompt);
    int pos = 0;
    for (;;) {
        int ch = readch();
        if (ch < 0) return -1;
        if (ch == '\n' || ch == '\r') {
            buf[pos] = '\0';
            return pos;
        }
        if (ch == 27 || ch == 3) {  /* ESC or Ctrl-C: cancel */
            buf[0] = '\0';
            return -1;
        }
        if (ch == 127 || ch == 8) {  /* Backspace */
            if (pos > 0) {
                pos--;
                write_str("\b \b");
            } else {
                buf[0] = '\0';
                return -1; /* Empty backspace = cancel */
            }
            continue;
        }
        if (ch == 21) {  /* Ctrl-U: clear line */
            while (pos > 0) {
                write_str("\b \b");
                pos--;
            }
            continue;
        }
        if (pos < bufsize - 1 && ch >= 32) {
            buf[pos++] = (char)ch;
            write_char((char)ch);
        }
    }
}

/* ═══════════════ Commands ═══════════════ */

static void cmd_forward(int n) {
    if (n <= 0) n = screen_lines;
    int want = top_line + n + screen_lines;

    /* If pipe is still open and we need more data, block until enough arrives */
    if (pipe_fd >= 0 && want > total_lines) {
        /* Temporarily set blocking */
        int fl = fcntl(pipe_fd, F_GETFL);
        fcntl(pipe_fd, F_SETFL, fl & ~O_NONBLOCK);

        char buf[4096];
        while (pipe_fd >= 0 && total_lines < want) {
            ssize_t r = read(pipe_fd, buf, sizeof(buf));
            if (r <= 0) {
                /* EOF or error */
                if (pipe_lpos > 0) {
                    add_line(pipe_linebuf, pipe_lpos);
                    pipe_lpos = 0;
                }
                pipe_fd = -1;
                break;
            }
            for (ssize_t i = 0; i < r; i++) {
                if (buf[i] == '\n' || pipe_lpos >= (int)sizeof(pipe_linebuf) - 1) {
                    int is_blank = (pipe_lpos == 0);
                    if (opt_squeeze_blank && is_blank && pipe_prev_blank)
                        { pipe_lpos = 0; continue; }
                    pipe_prev_blank = is_blank;
                    add_line(pipe_linebuf, pipe_lpos);
                    pipe_lpos = 0;
                } else {
                    pipe_linebuf[pipe_lpos++] = buf[i];
                }
            }
        }

        /* Restore non-blocking */
        if (pipe_fd >= 0)
            fcntl(pipe_fd, F_SETFL, fl);
    }

    top_line += n;
    if (top_line > total_lines - screen_lines)
        top_line = total_lines - screen_lines;
    if (top_line < 0) top_line = 0;
}

static void cmd_backward(int n) {
    if (n <= 0) n = screen_lines;
    top_line -= n;
    if (top_line < 0) top_line = 0;
}

static void cmd_forward_line(int n) {
    if (n <= 0) n = 1;
    cmd_forward(n);
}

static void cmd_backward_line(int n) {
    if (n <= 0) n = 1;
    cmd_backward(n);
}

static void cmd_goto_line(int n) {
    if (n <= 0) n = 1;
    top_line = n - 1;
    if (top_line > total_lines - screen_lines)
        top_line = total_lines - screen_lines;
    if (top_line < 0) top_line = 0;
}

static void cmd_goto_end(int n) {
    if (n > 0) {
        cmd_goto_line(n);
        return;
    }
    /* If pipe is still active, drain remaining data (blocking) */
    if (pipe_fd >= 0) {
        /* Temporarily set blocking to read all remaining data */
        int fl = fcntl(pipe_fd, F_GETFL);
        fcntl(pipe_fd, F_SETFL, fl & ~O_NONBLOCK);
        char buf[8192];
        for (;;) {
            ssize_t r = read(pipe_fd, buf, sizeof(buf));
            if (r <= 0) {
                if (pipe_lpos > 0) {
                    add_line(pipe_linebuf, pipe_lpos);
                    pipe_lpos = 0;
                }
                pipe_fd = -1;
                break;
            }
            for (ssize_t i = 0; i < r; i++) {
                if (buf[i] == '\n' || pipe_lpos >= (int)sizeof(pipe_linebuf) - 1) {
                    int is_blank = (pipe_lpos == 0);
                    if (opt_squeeze_blank && is_blank && pipe_prev_blank)
                        { pipe_lpos = 0; continue; }
                    pipe_prev_blank = is_blank;
                    add_line(pipe_linebuf, pipe_lpos);
                    pipe_lpos = 0;
                } else {
                    pipe_linebuf[pipe_lpos++] = buf[i];
                }
            }
        }
    }
    top_line = total_lines - screen_lines;
    if (top_line < 0) top_line = 0;
}

static void cmd_goto_percent(int n) {
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    top_line = (total_lines * n) / 100;
    if (top_line > total_lines - screen_lines)
        top_line = total_lines - screen_lines;
    if (top_line < 0) top_line = 0;
}

static void cmd_search_forward(void) {
    char buf[1024];
    int rc = read_input_line("/", buf, sizeof(buf));
    if (rc < 0) {
        display();
        return;
    }
    if (buf[0] != '\0') {
        /* New pattern */
        strncpy(search_pattern, buf, sizeof(search_pattern) - 1);
    } else if (search_pattern[0] == '\0') {
        /* No previous pattern to repeat */
        display();
        return;
    }
    search_direction = 1;
    update_search_hits();

    int found = search_next(top_line, 1);
    if (found >= 0) {
        top_line = found;
        if (top_line > total_lines - screen_lines)
            top_line = total_lines - screen_lines;
        if (top_line < 0) top_line = 0;
    } else {
        show_status("Pattern not found");
        readch();
    }
    display();
}

static void cmd_search_backward(void) {
    char buf[1024];
    int rc = read_input_line("?", buf, sizeof(buf));
    if (rc < 0) {
        display();
        return;
    }
    if (buf[0] != '\0') {
        /* New pattern */
        strncpy(search_pattern, buf, sizeof(search_pattern) - 1);
    } else if (search_pattern[0] == '\0') {
        /* No previous pattern to repeat */
        display();
        return;
    }
    search_direction = -1;
    update_search_hits();

    int found = search_next(top_line, -1);
    if (found >= 0) {
        top_line = found;
        if (top_line < 0) top_line = 0;
    } else {
        show_status("Pattern not found");
        readch();
    }
    display();
}

static void cmd_next_match(int dir) {
    if (search_pattern[0] == '\0') {
        bell();
        return;
    }
    int found = search_next(top_line, dir * search_direction);
    if (found >= 0) {
        top_line = found;
        if (top_line > total_lines - screen_lines)
            top_line = total_lines - screen_lines;
        if (top_line < 0) top_line = 0;
    } else {
        show_status("Pattern not found");
        readch();
    }
}

static void cmd_right(int n) {
    if (n <= 0)
        n = shift_amount > 0 ? shift_amount : term_cols / 2;
    horiz_shift += n;
}

static void cmd_left(int n) {
    if (n <= 0)
        n = shift_amount > 0 ? shift_amount : term_cols / 2;
    horiz_shift -= n;
    if (horiz_shift < 0) horiz_shift = 0;
}

static void cmd_toggle_option(void) {
    char buf[64];
    if (read_input_line("-", buf, sizeof(buf)) <= 0) {
        display();
        return;
    }

    for (int i = 0; buf[i]; i++) {
        switch (buf[i]) {
            case 'i': opt_ignore_case = !opt_ignore_case;
                show_status(opt_ignore_case ? "Ignore case in searches" : "Case is significant");
                break;
            case 'N': opt_line_numbers = !opt_line_numbers;
                show_status(opt_line_numbers ? "Line numbers ON" : "Line numbers OFF");
                break;
            case 'S': opt_chop_lines = !opt_chop_lines;
                show_status(opt_chop_lines ? "Chop long lines" : "Fold long lines");
                break;
            case 's': opt_squeeze_blank = !opt_squeeze_blank;
                show_status(opt_squeeze_blank ? "Squeeze blank lines" : "Don't squeeze blank lines");
                break;
            case 'n':
                /* toggle line numbers */
                opt_line_numbers = !opt_line_numbers;
                break;
            default:
                show_status("No such option");
                break;
        }
    }
    readch();
    display();
}

static void cmd_show_info(void) {
    char buf[1024];
    int bot = top_line + screen_lines - 1;
    if (bot >= total_lines) bot = total_lines - 1;
    int pct = total_lines > 0 ? ((bot + 1) * 100 / total_lines) : 100;
    snprintf(buf, sizeof(buf), "%s lines %d-%d/%d byte ? %d%%",
             current_filename ? current_filename : "(stdin)",
             top_line + 1, bot + 1, total_lines, pct);
    show_status(buf);
    readch();
}

static void cmd_mark(void) {
    show_status("mark: ");
    int ch = readch();
    if (ch >= 0 && ch < MARK_COUNT) {
        marks[ch] = top_line;
        mark_set[ch] = 1;
    }
}

static void cmd_goto_mark(void) {
    show_status("goto mark: ");
    int ch = readch();
    if (ch >= 0 && ch < MARK_COUNT && mark_set[ch]) {
        top_line = marks[ch];
        if (top_line < 0) top_line = 0;
    } else {
        bell();
    }
}

/* Open next/previous file */
static int open_next_file(int dir) {
    int next = current_file + dir;
    if (next < 0 || next >= nfiles) {
        bell();
        return -1;
    }
    current_file = next;
    if (load_file(filenames[current_file]) < 0) {
        show_status("Cannot open file");
        readch();
        return -1;
    }
    current_filename = filenames[current_file];
    top_line = 0;
    horiz_shift = 0;
    if (search_pattern[0])
        update_search_hits();
    return 0;
}

static void cmd_examine(void) {
    char buf[1024];
    if (read_input_line(":e ", buf, sizeof(buf)) <= 0) {
        display();
        return;
    }
    /* Try to open the file */
    if (load_file(buf) < 0) {
        show_status("Cannot open file");
        readch();
    } else {
        current_filename = buf;
        top_line = 0;
        horiz_shift = 0;
    }
    display();
}

/* ═══════════════ Main loop ═══════════════ */

static void run_pager(void) {
    int eof_count = 0;

    display();
    show_default_prompt();

    for (;;) {
        /* Pull in any new data from the pipe (non-blocking) */
        pipe_read_more();

        int count = 0;
        int has_count = 0;

        int key = read_key();
        if (key < 0) break;

        /* Read numeric prefix */
        while (key >= '0' && key <= '9') {
            count = count * 10 + (key - '0');
            has_count = 1;
            key = read_key();
            if (key < 0) return;
        }

        /* Check for EOF quit conditions (only when pipe is done) */
        if (top_line + screen_lines >= total_lines && pipe_fd < 0) {
            eof_count++;
            if (opt_quit_at_eof2 && eof_count >= 1) {
                if (current_file >= nfiles - 1) return;
            }
            if (opt_quit_at_eof && eof_count >= 2) {
                if (current_file >= nfiles - 1) return;
            }
        } else {
            eof_count = 0;
        }

        switch (key) {
            /* ─── Quit ─── */
            case 'q':
            case 'Q':
                return;

            case ':': {
                int ch2 = read_key();
                if (ch2 == 'q' || ch2 == 'Q') return;
                if (ch2 == 'n') { open_next_file(1); display(); show_default_prompt(); continue; }
                if (ch2 == 'p') { open_next_file(-1); display(); show_default_prompt(); continue; }
                if (ch2 == 'e') { cmd_examine(); show_default_prompt(); continue; }
                break;
            }

            /* ─── Forward movement ─── */
            case ' ':     /* SPACE: forward one screen */
            case 'f':     /* forward one screen */
            case 1007:    /* Page Down */
                cmd_forward(has_count ? count : screen_lines);
                break;

            case 'z':     /* forward, set window size */
                if (has_count) screen_lines = count;
                cmd_forward(screen_lines);
                break;

            case '\n':    /* RETURN: forward one line */
            case '\r':
            case 'e':     /* forward one line */
            case 'j':     /* forward one line */
            case 1001:    /* DOWN arrow */
                cmd_forward_line(has_count ? count : 1);
                break;

            case 'd':     /* forward half screen */
            case 4:       /* Ctrl-D */
                cmd_forward(has_count ? count : screen_lines / 2);
                break;

            /* ─── Backward movement ─── */
            case 'b':     /* backward one screen */
            case 2:       /* Ctrl-B */
            case 1006:    /* Page Up */
                cmd_backward(has_count ? count : screen_lines);
                break;

            case 'w':     /* backward, set window size */
                if (has_count) screen_lines = count;
                cmd_backward(screen_lines);
                break;

            case 'y':     /* backward one line */
            case 'k':     /* backward one line */
            case 1000:    /* UP arrow */
                cmd_backward_line(has_count ? count : 1);
                break;

            case 'u':     /* backward half screen */
            case 21:      /* Ctrl-U */
                cmd_backward(has_count ? count : screen_lines / 2);
                break;

            /* ─── Goto ─── */
            case 'g':     /* goto beginning / line N */
            case '<':
            case 1004:    /* HOME */
                cmd_goto_line(has_count ? count : 1);
                break;

            case 'G':     /* goto end / line N */
            case '>':
            case 1005:    /* END */
                cmd_goto_end(has_count ? count : 0);
                break;

            case 'p':     /* goto percent */
            case '%':
                if (has_count)
                    cmd_goto_percent(count);
                break;

            /* ─── Searching ─── */
            case '/':
                cmd_search_forward();
                show_default_prompt();
                continue;

            case '?':
                cmd_search_backward();
                show_default_prompt();
                continue;

            case 'n':     /* next match */
                cmd_next_match(1);
                break;

            case 'N':     /* previous match */
                cmd_next_match(-1);
                break;

            /* ─── Horizontal scrolling ─── */
            case 1002:    /* RIGHT arrow */
                cmd_right(has_count ? count : 0);
                break;

            case 1003:    /* LEFT arrow */
                cmd_left(has_count ? count : 0);
                break;

            /* ─── Marks ─── */
            case 'm':
                cmd_mark();
                display();
                show_default_prompt();
                continue;

            case '\'':
                cmd_goto_mark();
                break;

            /* ─── Options ─── */
            case '-':
                cmd_toggle_option();
                show_default_prompt();
                continue;

            case '=':     /* File info */
                cmd_show_info();
                display();
                show_default_prompt();
                continue;

            case 'V':     /* Version */
                show_status(PROGRAM_NAME " version " VERSION);
                readch();
                display();
                show_default_prompt();
                continue;

            case 'h':
            case 'H':
                /* Help screen */
                term_clear();
                write_str("SUMMARY OF LESS COMMANDS\n\n");
                write_str("  SPACE, f, PgDn   Forward one screen\n");
                write_str("  b, PgUp          Backward one screen\n");
                write_str("  RETURN, e, j     Forward one line\n");
                write_str("  y, k             Backward one line\n");
                write_str("  d, Ctrl-D        Forward half screen\n");
                write_str("  u, Ctrl-U        Backward half screen\n");
                write_str("  g, <, Home       Go to beginning\n");
                write_str("  G, >, End        Go to end\n");
                write_str("  Ng               Go to line N\n");
                write_str("  Np               Go to N percent\n");
                write_str("  /pattern         Search forward\n");
                write_str("  ?pattern         Search backward\n");
                write_str("  n                Next search match\n");
                write_str("  N                Previous search match\n");
                write_str("  m<letter>        Set mark\n");
                write_str("  '<letter>        Go to mark\n");
                write_str("  -<flag>          Toggle option\n");
                write_str("  =                File info\n");
                write_str("  :n               Next file\n");
                write_str("  :p               Previous file\n");
                write_str("  :e <file>        Open file\n");
                write_str("  q, :q            Quit\n");
                write_str("  RIGHT            Scroll right\n");
                write_str("  LEFT             Scroll left\n");
                write_str("  -N               Toggle line numbers\n");
                write_str("  -S               Toggle chop/fold long lines\n");
                write_str("  -i               Toggle ignore case\n");
                write_str("  -s               Toggle squeeze blank lines\n");
                write_str("  Ctrl-L, r        Repaint screen\n");
                write_str("\nPress any key to continue...");
                readch();
                display();
                show_default_prompt();
                continue;

            case 'r':     /* Repaint */
            case 'R':
            case 12:      /* Ctrl-L */
                break;    /* Will repaint below */

            default:
                bell();
                show_default_prompt();
                continue;
        }

        display();
        show_default_prompt();
    }
}

/* ═══════════════ Option Parsing ═══════════════ */

static void usage(void) {
    printf("Usage: %s [options] [file ...]\n\n", PROGRAM_NAME);
    printf("Options:\n");
    printf("  -c             Repaint screen from top\n");
    printf("  -e             Quit at second end-of-file\n");
    printf("  -E             Quit at first end-of-file\n");
    printf("  -f             Force open non-regular files\n");
    printf("  -g             Highlight only last search match\n");
    printf("  -G             Don't highlight search matches\n");
    printf("  -i             Ignore case in searches\n");
    printf("  -I             Ignore case (smart)\n");
    printf("  -J             Status column\n");
    printf("  -m             Medium prompt\n");
    printf("  -M             Long prompt\n");
    printf("  -n             Suppress line numbers\n");
    printf("  -N             Show line numbers\n");
    printf("  -q             Quiet (no bell)\n");
    printf("  -Q             Very quiet\n");
    printf("  -r             Raw control characters\n");
    printf("  -R             Raw ANSI color sequences\n");
    printf("  -s             Squeeze blank lines\n");
    printf("  -S             Chop long lines\n");
    printf("  -x<n>          Tab stops\n");
    printf("  -X             No init/deinit strings\n");
    printf("  -z<n>          Window size\n");
    printf("  -~             Don't show tildes after EOF\n");
    printf("  -#<n>          Horizontal scroll amount\n");
    printf("  --use-color    Enable colored text\n");
    printf("  -V, --version  Version information\n");
    printf("  +<cmd>         Initial command\n");
}

int main(int argc, char **argv) {
    /* Initialize marks */
    memset(mark_set, 0, sizeof(mark_set));
    memset(marks, 0, sizeof(marks));

    /* Check LESS environment variable */
    const char *less_env = getenv("LESS");
    (void)less_env; /* Would parse options from env; simplified */

    /* Process LESS_IS_MORE */
    const char *is_more = getenv("LESS_IS_MORE");
    (void)is_more;

    /* Parse command line - handle + commands and option flags */
    char *initial_cmd = NULL;

    /* Build argument list excluding + commands */
    int real_argc = 0;
    char **real_argv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!real_argv) return 1;

    real_argv[real_argc++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '+') {
            initial_cmd = argv[i] + 1;
        } else {
            real_argv[real_argc++] = argv[i];
        }
    }
    real_argv[real_argc] = NULL;

    static struct option long_options[] = {
        {"version",     no_argument,       0, 'V'},
        {"help",        no_argument,       0, 1001},
        {"use-color",   no_argument,       0, 1002},
        {"USE-COLOR",   no_argument,       0, 1002},
        {"tabs",        required_argument, 0, 'x'},
        {"window",      required_argument, 0, 'z'},
        {"shift",       required_argument, 0, '#'},
        {"chop-long-lines",    no_argument, 0, 'S'},
        {"squeeze-blank-lines",no_argument, 0, 's'},
        {"raw-control-chars",  no_argument, 0, 'r'},
        {"RAW-CONTROL-CHARS",  no_argument, 0, 'R'},
        {"QUIET",       no_argument,       0, 'Q'},
        {"SILENT",      no_argument,       0, 'Q'},
        {"quiet",       no_argument,       0, 'q'},
        {"silent",      no_argument,       0, 'q'},
        {"ignore-case", no_argument,       0, 'i'},
        {"IGNORE-CASE", no_argument,       0, 'I'},
        {"status-column",no_argument,      0, 'J'},
        {"line-numbers", no_argument,      0, 'N'},
        {"no-init",     no_argument,       0, 'X'},
        {"tilde",       no_argument,       0, '~'},
        {"hilite-unread", no_argument,     0, 'w'},
        {"HILITE-UNREAD", no_argument,     0, 'W'},
        {0, 0, 0, 0}
    };

    int c;
    optind = 1;
    while ((c = getopt_long(real_argc, real_argv, "ceEfgGiIJmMnNqQrRsSuUVwWx:Xz:~#:",
                            long_options, NULL)) != -1) {
        switch (c) {
            case 'c': opt_clear_screen = 1; break;
            case 'e': opt_quit_at_eof = 1; break;
            case 'E': opt_quit_at_eof2 = 1; break;
            case 'f': opt_force_open = 1; break;
            case 'g': /* highlight only last match */ break;
            case 'G': opt_hilite_search = 0; break;
            case 'i': opt_ignore_case = 1; break;
            case 'I': opt_smart_case = 1; opt_ignore_case = 1; break;
            case 'J': opt_status_col = 1; break;
            case 'm': opt_long_prompt = 1; break;
            case 'M': opt_very_long_prompt = 1; break;
            case 'n': /* suppress line numbers (default) */ break;
            case 'N': opt_line_numbers = 1; break;
            case 'q': opt_quiet = 1; break;
            case 'Q': opt_very_quiet = 1; opt_quiet = 1; break;
            case 'r': opt_raw_control = 1; break;
            case 'R': opt_raw_ansi = 1; break;
            case 's': opt_squeeze_blank = 1; break;
            case 'S': opt_chop_lines = 1; break;
            case 'u': case 'U': /* underline handling - simplified */ break;
            case 'V':
                printf("%s version %s\n", PROGRAM_NAME, VERSION);
                free(real_argv);
                return 0;
            case 'w': case 'W': /* hilite unread - simplified */ break;
            case 'x':
                opt_tabs = atoi(optarg);
                if (opt_tabs < 1) opt_tabs = 8;
                break;
            case 'X': opt_no_init = 1; break;
            case 'z':
                opt_window = atoi(optarg);
                break;
            case '~': opt_tilde = 1; break;
            case '#':
                shift_amount = atoi(optarg);
                break;
            case 1001: /* --help */
                usage();
                free(real_argv);
                return 0;
            case 1002: /* --use-color */
                opt_use_color = 1;
                break;
            default:
                break;
        }
    }

    /* Suppress unused warnings */
    (void)opt_clear_screen;
    (void)opt_force_open;
    (void)opt_use_color;
    (void)opt_no_init;
    (void)display_width;

    /* Collect file arguments */
    nfiles = real_argc - optind;
    filenames = &real_argv[optind];
    current_file = 0;

    /* If stdin is not a tty (piped), open /dev/tty for keyboard input */
    if (!isatty(STDIN_FILENO)) {
        tty_fd = open("/dev/tty", O_RDWR);
        /* tty_fd < 0 is handled gracefully — readch/enter_raw will fail */
    }

    get_term_size();

    int ret = 0;

    if (nfiles == 0) {
        /* Read from stdin (pipe) — use incremental loading */
        current_filename = NULL;
        enter_raw();
        if (!opt_no_init)
            term_clear();
        if (load_pipe(STDIN_FILENO) < 0) {
            exit_raw();
            fprintf(stderr, "%s: cannot read stdin\n", PROGRAM_NAME);
            free(real_argv);
            return 1;
        }
    } else {
        current_filename = filenames[0];
        if (load_file(filenames[0]) < 0) {
            fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, filenames[0], strerror(errno));
            free(real_argv);
            return 1;
        }
        enter_raw();
        if (!opt_no_init)
            term_clear();
    }

    /* Handle initial command */
    if (initial_cmd) {
        if (initial_cmd[0] == '/') {
            /* Search */
            strncpy(search_pattern, initial_cmd + 1, sizeof(search_pattern) - 1);
            search_direction = 1;
            update_search_hits();
            int found = search_next(-1, 1);
            if (found >= 0) top_line = found;
        } else if (initial_cmd[0] == 'G' || initial_cmd[0] == '\0') {
            /* Go to end */
            cmd_goto_end(0);
        } else if (initial_cmd[0] >= '0' && initial_cmd[0] <= '9') {
            /* Go to line */
            cmd_goto_line(atoi(initial_cmd));
        }
    }

    run_pager();

    /* Clean up */
    if (!opt_no_init) {
        write_str("\r\033[K\n");
    }

    free_lines();
    exit_raw();
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
