/*
 * curses.c - ncurses-compatible implementation for LikeOS
 *
 * Implements the curses API subset needed by GNU nano using ANSI/VT100
 * escape sequences.  LikeOS's kernel tty driver (kernel/ke/tty.c) already
 * processes these sequences, so we simply emit them through stdout.
 *
 * Based on the ncurses 6.5 API (source in ports/lib/ncurses-6.5/).
 *
 * Copyright (C) 2026 LikeOS Project
 */

#include "curses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

/* ===================================================================
 * Internal helpers
 * =================================================================== */

/* Raw output buffer – we batch writes for efficiency */
#define OUTBUF_SZ  8192
static char _outbuf[OUTBUF_SZ];
static int  _outpos = 0;

static void _flush(void)
{
    if (_outpos > 0) {
        (void)write(STDOUT_FILENO, _outbuf, _outpos);
        _outpos = 0;
    }
}

static void _emit(const char *s, int len)
{
    while (len > 0) {
        int space = OUTBUF_SZ - _outpos;
        int chunk = (len < space) ? len : space;
        memcpy(_outbuf + _outpos, s, chunk);
        _outpos += chunk;
        s   += chunk;
        len -= chunk;
        if (_outpos >= OUTBUF_SZ)
            _flush();
    }
}

static void _emits(const char *s)
{
    _emit(s, strlen(s));
}

static void _emitf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        _emit(buf, n);
}

/* ===================================================================
 * Global state
 * =================================================================== */

WINDOW *stdscr  = NULL;
WINDOW *curscr  = NULL;
int LINES       = 24;
int COLS        = 80;
int COLORS      = 256;
int COLOR_PAIRS = 256;
int TABSIZE     = 8;
int ESCDELAY    = 50;

static bool _is_endwin   = false;
static bool _use_env     = true;
static int  _cursor_vis  = 1;       /* 0=invisible, 1=normal, 2=very visible */
static struct termios _saved_tios;
static bool _raw_mode    = false;
static bool _echo_mode   = true;
static bool _halfdelay_mode = false;
static int  _halfdelay_tenths = 0;

/* Color pair table */
#define MAX_PAIRS 256
static struct { short fg, bg; } _pairs[MAX_PAIRS];
static bool _color_started = false;
static bool _defaults_ok   = false;

/* Ungetch ring buffer */
#define UNGETCH_MAX 64
static int _ungetch_buf[UNGETCH_MAX];
static int _ungetch_head = 0;
static int _ungetch_tail = 0;
static int _ungetch_count = 0;

/* Key definition table */
#define MAX_KEYDEFS 128
static struct {
    char seq[16];
    int  code;
} _keydefs[MAX_KEYDEFS];
static int _nkeydefs = 0;

/* Mouse state */
static mmask_t _mouse_mask = 0;
static MEVENT  _last_mouse;

/* SIGWINCH handler */
static volatile sig_atomic_t _got_resize = 0;

static void _sigwinch_handler(int sig)
{
    (void)sig;
    _got_resize = 1;
}

/* ===================================================================
 * Window allocation / deallocation
 * =================================================================== */

static WINDOW *_alloc_win(int nlines, int ncols, int begy, int begx)
{
    WINDOW *w = (WINDOW *)calloc(1, sizeof(WINDOW));
    if (!w) return NULL;

    w->_maxy = nlines - 1;
    w->_maxx = ncols  - 1;
    w->_begy = begy;
    w->_begx = begx;
    w->_cury = 0;
    w->_curx = 0;
    w->_attrs = A_NORMAL;
    w->_color = 0;
    w->_keypad = false;
    w->_nodelay = false;
    w->_scrollok = false;
    w->_clearok = false;
    w->_leaveok = false;
    w->_delay = -1;
    w->_touched = true;

    /* Allocate backing store */
    w->_line = (chtype **)calloc(nlines, sizeof(chtype *));
    w->_dirty = (bool *)calloc(nlines, sizeof(bool));
    if (!w->_line || !w->_dirty) {
        free(w->_line);
        free(w->_dirty);
        free(w);
        return NULL;
    }
    for (int i = 0; i < nlines; i++) {
        w->_line[i] = (chtype *)calloc(ncols, sizeof(chtype));
        if (!w->_line[i]) {
            for (int j = 0; j < i; j++) free(w->_line[j]);
            free(w->_line);
            free(w->_dirty);
            free(w);
            return NULL;
        }
        for (int j = 0; j < ncols; j++)
            w->_line[i][j] = ' ';
        w->_dirty[i] = true;
    }

    return w;
}

static void _free_win(WINDOW *w)
{
    if (!w) return;
    if (w->_line) {
        for (int i = 0; i <= w->_maxy; i++)
            free(w->_line[i]);
        free(w->_line);
    }
    free(w->_dirty);
    free(w);
}

/* ===================================================================
 * Terminal size detection
 * =================================================================== */

static void _detect_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        LINES = ws.ws_row;
        COLS  = ws.ws_col;
    }
}

/* ===================================================================
 * Apply current attributes to terminal
 * =================================================================== */

static attr_t _cur_applied_attrs = A_NORMAL;
static short  _cur_applied_pair  = 0;

static void _apply_attrs(attr_t attrs, short pair)
{
    if (attrs == _cur_applied_attrs && pair == _cur_applied_pair)
        return;

    /* Reset all */
    _emits("\033[0m");

    if (attrs & A_BOLD)
        _emits("\033[1m");
    if (attrs & A_DIM)
        _emits("\033[2m");
    if (attrs & A_ITALIC)
        _emits("\033[3m");
    if (attrs & A_UNDERLINE)
        _emits("\033[4m");
    if (attrs & A_BLINK)
        _emits("\033[5m");
    if (attrs & A_REVERSE)
        _emits("\033[7m");
    if (attrs & A_STANDOUT)
        _emits("\033[7m");  /* standout = reverse video */

    if (pair > 0 && pair < MAX_PAIRS) {
        short fg = _pairs[pair].fg;
        short bg = _pairs[pair].bg;
        if (fg >= 0 && fg < 8)
            _emitf("\033[%dm", 30 + fg);
        else if (fg >= 8 && fg < 16)
            _emitf("\033[%dm", 90 + fg - 8);
        else if (fg >= 16)
            _emitf("\033[38;5;%dm", fg);

        if (bg >= 0 && bg < 8)
            _emitf("\033[%dm", 40 + bg);
        else if (bg >= 8 && bg < 16)
            _emitf("\033[%dm", 100 + bg - 8);
        else if (bg >= 16)
            _emitf("\033[48;5;%dm", bg);
    }

    _cur_applied_attrs = attrs;
    _cur_applied_pair  = pair;
}

/* ===================================================================
 * Initialization / termination
 * =================================================================== */

static void _setup_default_keys(void);

WINDOW *initscr(void)
{
    if (_use_env)
        _detect_size();

    stdscr = _alloc_win(LINES, COLS, 0, 0);
    curscr = _alloc_win(LINES, COLS, 0, 0);

    if (!stdscr || !curscr) {
        fprintf(stderr, "initscr: cannot allocate windows\n");
        exit(1);
    }

    /* curscr represents "what is currently on the physical screen".
     * Since we're about to clear the screen, curscr should be all spaces
     * with NO dirty lines.  Only wnoutrefresh() should mark curscr dirty. */
    for (int i = 0; i <= curscr->_maxy; i++)
        curscr->_dirty[i] = false;
    curscr->_touched = false;

    /* Save terminal state and switch to raw mode */
    tcgetattr(STDIN_FILENO, &_saved_tios);

    /* Enter alternate screen buffer */
    _emits("\033[?1049h");
    /* Reset all attributes to default BEFORE clearing screen,
     * so the clear uses black background and not stale shell colors */
    _emits("\033[0m");
    _cur_applied_attrs = A_NORMAL;
    _cur_applied_pair  = 0;
    /* Enable cursor position reports */
    _emits("\033[?25h");  /* show cursor */
    /* Clear screen */
    _emits("\033[2J\033[H");
    _flush();

    _is_endwin = false;

    /* Set up SIGWINCH handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = _sigwinch_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    /* Set up default key definitions */
    _setup_default_keys();

    /* Initialize color pairs to defaults */
    for (int i = 0; i < MAX_PAIRS; i++) {
        _pairs[i].fg = -1;  /* default */
        _pairs[i].bg = -1;
    }

    return stdscr;
}

int endwin(void)
{
    if (!stdscr) return ERR;

    /* Reset attributes */
    _emits("\033[0m");
    /* Show cursor */
    _emits("\033[?25h");
    /* Leave alternate screen buffer */
    _emits("\033[?1049l");
    /* Disable mouse if it was enabled */
    if (_mouse_mask) {
        _emits("\033[?1000l\033[?1002l\033[?1006l");
        _mouse_mask = 0;
    }
    _flush();

    /* Restore terminal */
    if (_raw_mode)
        tcsetattr(STDIN_FILENO, TCSANOW, &_saved_tios);
    _raw_mode = false;
    _is_endwin = true;

    _cur_applied_attrs = A_NORMAL;
    _cur_applied_pair  = 0;

    return OK;
}

int isendwin(void)
{
    return _is_endwin;
}

/* ===================================================================
 * Window management
 * =================================================================== */

WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    if (nlines <= 0) nlines = LINES - begin_y;
    if (ncols  <= 0) ncols  = COLS  - begin_x;
    return _alloc_win(nlines, ncols, begin_y, begin_x);
}

int delwin(WINDOW *win)
{
    if (!win || win == stdscr || win == curscr) return ERR;
    _free_win(win);
    return OK;
}

WINDOW *subwin(WINDOW *parent, int nlines, int ncols, int begin_y, int begin_x)
{
    (void)parent;
    return newwin(nlines, ncols, begin_y, begin_x);
}

WINDOW *derwin(WINDOW *parent, int nlines, int ncols, int begin_y, int begin_x)
{
    if (!parent) return NULL;
    return newwin(nlines, ncols, parent->_begy + begin_y, parent->_begx + begin_x);
}

int wresize(WINDOW *win, int lines, int columns)
{
    if (!win) return ERR;

    /* Reallocate backing store */
    chtype **new_line = (chtype **)calloc(lines, sizeof(chtype *));
    bool   *new_dirty = (bool *)calloc(lines, sizeof(bool));
    if (!new_line || !new_dirty) {
        free(new_line);
        free(new_dirty);
        return ERR;
    }

    for (int i = 0; i < lines; i++) {
        new_line[i] = (chtype *)calloc(columns, sizeof(chtype));
        if (!new_line[i]) {
            for (int j = 0; j < i; j++) free(new_line[j]);
            free(new_line);
            free(new_dirty);
            return ERR;
        }
        /* Copy old data if available */
        if (i <= win->_maxy && win->_line[i]) {
            int copy_cols = (columns < win->_maxx + 1) ? columns : (win->_maxx + 1);
            memcpy(new_line[i], win->_line[i], copy_cols * sizeof(chtype));
            for (int j = copy_cols; j < columns; j++)
                new_line[i][j] = ' ';
        } else {
            for (int j = 0; j < columns; j++)
                new_line[i][j] = ' ';
        }
        new_dirty[i] = true;
    }

    /* Free old */
    if (win->_line) {
        for (int i = 0; i <= win->_maxy; i++)
            free(win->_line[i]);
        free(win->_line);
    }
    free(win->_dirty);

    win->_line  = new_line;
    win->_dirty = new_dirty;
    win->_maxy  = lines - 1;
    win->_maxx  = columns - 1;

    if (win->_cury > win->_maxy) win->_cury = win->_maxy;
    if (win->_curx > win->_maxx) win->_curx = win->_maxx;

    win->_touched = true;
    return OK;
}

/* ===================================================================
 * Output
 * =================================================================== */

int wmove(WINDOW *win, int y, int x)
{
    if (!win) return ERR;
    if (y < 0 || y > win->_maxy || x < 0 || x > win->_maxx) return ERR;
    win->_cury = y;
    win->_curx = x;
    return OK;
}

int waddch(WINDOW *win, chtype ch)
{
    if (!win) return ERR;

    int y = win->_cury;
    int x = win->_curx;

    if (y > win->_maxy) return ERR;

    char c = (char)(ch & A_CHARTEXT);

    /* Handle special characters */
    if (c == '\n') {
        /* Fill rest of line with spaces */
        while (x <= win->_maxx) {
            win->_line[y][x] = ' ' | (win->_attrs | (ch & ~A_CHARTEXT & ~A_CHARTEXT));
            x++;
        }
        win->_dirty[y] = true;
        win->_touched = true;
        y++;
        x = 0;
        if (y > win->_maxy) {
            if (win->_scrollok) {
                wscrl(win, 1);
                y = win->_maxy;
            } else {
                y = win->_maxy;
            }
        }
        win->_cury = y;
        win->_curx = x;
        return OK;
    }

    if (c == '\r') {
        win->_curx = 0;
        return OK;
    }

    if (c == '\t') {
        int spaces = TABSIZE - (x % TABSIZE);
        for (int i = 0; i < spaces && x <= win->_maxx; i++, x++) {
            win->_line[y][x] = ' ' | win->_attrs;
        }
        win->_dirty[y] = true;
        win->_touched = true;
        win->_curx = x;
        return OK;
    }

    if (c == '\b') {
        if (win->_curx > 0) win->_curx--;
        return OK;
    }

    if (x <= win->_maxx) {
        attr_t a = ch & ~A_CHARTEXT;
        if (a == A_NORMAL)
            a = win->_attrs;
        win->_line[y][x] = (ch & A_CHARTEXT) | a;
        win->_dirty[y] = true;
        win->_touched = true;
        x++;
    }

    if (x > win->_maxx) {
        x = 0;
        y++;
        if (y > win->_maxy) {
            if (win->_scrollok) {
                wscrl(win, 1);
                y = win->_maxy;
            } else {
                y = win->_maxy;
                x = win->_maxx;
            }
        }
    }

    win->_cury = y;
    win->_curx = x;
    return OK;
}

int waddstr(WINDOW *win, const char *str)
{
    if (!win || !str) return ERR;
    while (*str)
        waddch(win, (chtype)(unsigned char)*str++);
    return OK;
}

int waddnstr(WINDOW *win, const char *str, int n)
{
    if (!win || !str) return ERR;
    if (n < 0)
        return waddstr(win, str);
    for (int i = 0; i < n && str[i]; i++)
        waddch(win, (chtype)(unsigned char)str[i]);
    return OK;
}

int mvwaddch(WINDOW *win, int y, int x, chtype ch)
{
    if (wmove(win, y, x) == ERR) return ERR;
    return waddch(win, ch);
}

int mvwaddstr(WINDOW *win, int y, int x, const char *str)
{
    if (wmove(win, y, x) == ERR) return ERR;
    return waddstr(win, str);
}

int mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n)
{
    if (wmove(win, y, x) == ERR) return ERR;
    return waddnstr(win, str, n);
}

int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...)
{
    if (wmove(win, y, x) == ERR) return ERR;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(win, buf);
}

int wprintw(WINDOW *win, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return waddstr(win, buf);
}

/* ===================================================================
 * Refresh / Update
 * =================================================================== */

int wnoutrefresh(WINDOW *win)
{
    if (!win || !curscr) return ERR;

    /* Copy dirty lines from win into curscr at the window's screen position */
    for (int wy = 0; wy <= win->_maxy; wy++) {
        if (!win->_dirty[wy]) continue;

        int sy = win->_begy + wy;
        if (sy < 0 || sy > curscr->_maxy) continue;

        for (int wx = 0; wx <= win->_maxx; wx++) {
            int sx = win->_begx + wx;
            if (sx < 0 || sx > curscr->_maxx) continue;
            curscr->_line[sy][sx] = win->_line[wy][wx];
            curscr->_dirty[sy] = true;
        }
        win->_dirty[wy] = false;
    }
    win->_touched = false;

    /* Remember cursor position */
    curscr->_cury = win->_begy + win->_cury;
    curscr->_curx = win->_begx + win->_curx;

    return OK;
}

int doupdate(void)
{
    if (!curscr) return ERR;

    /* Force-reset terminal attributes at the start of every update
     * so the kernel ANSI state is known-good regardless of what
     * happened between doupdate calls. */
    _emits("\033[0m");
    _cur_applied_attrs = A_NORMAL;
    _cur_applied_pair  = 0;

    int last_y = -1, last_x = -1;
    attr_t last_attrs = A_NORMAL;
    short  last_pair  = 0;

    for (int y = 0; y <= curscr->_maxy; y++) {
        if (!curscr->_dirty[y]) continue;

        for (int x = 0; x <= curscr->_maxx; x++) {
            /* Skip the bottom-right corner cell to avoid triggering
             * a terminal scroll when the cursor wraps past it. */
            if (y == curscr->_maxy && x == curscr->_maxx)
                continue;

            chtype ch = curscr->_line[y][x];
            attr_t a = ch & ~A_CHARTEXT;
            short  p = PAIR_NUMBER(a);
            a &= ~A_COLOR;  /* strip color for attribute comparison */

            /* Position cursor if not sequential */
            if (y != last_y || x != last_x) {
                _emitf("\033[%d;%dH", y + 1, x + 1);
            }

            /* Apply attributes if changed */
            if (a != last_attrs || p != last_pair) {
                _apply_attrs(a, p);
                last_attrs = a;
                last_pair  = p;
            }

            char c = (char)(ch & A_CHARTEXT);
            if (c < ' ') c = ' ';
            _emit(&c, 1);

            last_y = y;
            last_x = x + 1;
        }
        curscr->_dirty[y] = false;
    }

    /* Position cursor at the logical cursor position */
    if (curscr->_cury >= 0 && curscr->_curx >= 0)
        _emitf("\033[%d;%dH", curscr->_cury + 1, curscr->_curx + 1);

    _flush();
    return OK;
}

int wrefresh(WINDOW *win)
{
    wnoutrefresh(win);
    return doupdate();
}

int redrawwin(WINDOW *win)
{
    if (!win) return ERR;
    for (int i = 0; i <= win->_maxy; i++)
        win->_dirty[i] = true;
    win->_touched = true;
    return OK;
}

int wredrawln(WINDOW *win, int beg_line, int num_lines)
{
    if (!win) return ERR;
    for (int i = beg_line; i < beg_line + num_lines && i <= win->_maxy; i++)
        win->_dirty[i] = true;
    win->_touched = true;
    return OK;
}

/* ===================================================================
 * Clearing
 * =================================================================== */

int werase(WINDOW *win)
{
    if (!win) return ERR;
    for (int y = 0; y <= win->_maxy; y++) {
        for (int x = 0; x <= win->_maxx; x++)
            win->_line[y][x] = ' ';
        win->_dirty[y] = true;
    }
    win->_cury = 0;
    win->_curx = 0;
    win->_touched = true;
    return OK;
}

int wclear(WINDOW *win)
{
    if (werase(win) == ERR) return ERR;
    win->_clearok = true;
    return OK;
}

int wclrtoeol(WINDOW *win)
{
    if (!win) return ERR;
    int y = win->_cury;
    for (int x = win->_curx; x <= win->_maxx; x++)
        win->_line[y][x] = ' ';
    win->_dirty[y] = true;
    win->_touched = true;
    return OK;
}

int wclrtobot(WINDOW *win)
{
    if (!win) return ERR;
    wclrtoeol(win);
    for (int y = win->_cury + 1; y <= win->_maxy; y++) {
        for (int x = 0; x <= win->_maxx; x++)
            win->_line[y][x] = ' ';
        win->_dirty[y] = true;
    }
    win->_touched = true;
    return OK;
}

/* ===================================================================
 * Attributes
 * =================================================================== */

int wattron(WINDOW *win, int attrs)
{
    if (!win) return ERR;
    win->_attrs |= (attr_t)attrs;
    if (attrs & A_COLOR)
        win->_color = PAIR_NUMBER(attrs);
    return OK;
}

int wattroff(WINDOW *win, int attrs)
{
    if (!win) return ERR;
    win->_attrs &= ~(attr_t)attrs;
    if (attrs & A_COLOR)
        win->_color = 0;
    return OK;
}

int wattrset(WINDOW *win, int attrs)
{
    if (!win) return ERR;
    win->_attrs = (attr_t)attrs;
    win->_color = PAIR_NUMBER(attrs);
    return OK;
}

int wbkgdset(WINDOW *win, chtype ch)
{
    if (!win) return ERR;
    /* We just apply the attributes */
    win->_attrs = ch & ~A_CHARTEXT;
    return OK;
}

/* ===================================================================
 * Color
 * =================================================================== */

int start_color(void)
{
    _color_started = true;
    COLORS = 256;
    COLOR_PAIRS = 256;
    return OK;
}

bool has_colors(void)
{
    return true;
}

int init_pair(short pair, short fg, short bg)
{
    if (pair < 0 || pair >= MAX_PAIRS) return ERR;
    _pairs[pair].fg = fg;
    _pairs[pair].bg = bg;
    return OK;
}

int use_default_colors(void)
{
    _defaults_ok = true;
    return OK;
}

/* ===================================================================
 * Cursor
 * =================================================================== */

int curs_set(int visibility)
{
    int old = _cursor_vis;
    _cursor_vis = visibility;

    if (visibility == 0)
        _emits("\033[?25l");
    else
        _emits("\033[?25h");

    _flush();
    return old;
}

/* ===================================================================
 * Scrolling
 * =================================================================== */

int scrollok(WINDOW *win, bool bf)
{
    if (!win) return ERR;
    win->_scrollok = bf;
    return OK;
}

int wscrl(WINDOW *win, int n)
{
    if (!win || !win->_scrollok) return ERR;

    if (n > 0) {
        /* Scroll up */
        for (int i = 0; i < n && n <= win->_maxy; i++) {
            chtype *tmp = win->_line[0];
            for (int y = 0; y < win->_maxy; y++)
                win->_line[y] = win->_line[y + 1];
            win->_line[win->_maxy] = tmp;
            /* Clear the new bottom line */
            for (int x = 0; x <= win->_maxx; x++)
                win->_line[win->_maxy][x] = ' ';
        }
    } else if (n < 0) {
        /* Scroll down */
        n = -n;
        for (int i = 0; i < n && n <= win->_maxy; i++) {
            chtype *tmp = win->_line[win->_maxy];
            for (int y = win->_maxy; y > 0; y--)
                win->_line[y] = win->_line[y - 1];
            win->_line[0] = tmp;
            for (int x = 0; x <= win->_maxx; x++)
                win->_line[0][x] = ' ';
        }
    }

    /* Mark all lines dirty */
    for (int y = 0; y <= win->_maxy; y++)
        win->_dirty[y] = true;
    win->_touched = true;

    return OK;
}

int wsetscrreg(WINDOW *win, int top, int bot)
{
    (void)win; (void)top; (void)bot;
    /* Not implementing scroll regions - nano uses wscrl() directly */
    return OK;
}

/* ===================================================================
 * Options
 * =================================================================== */

int keypad(WINDOW *win, bool bf)
{
    if (!win) return ERR;
    win->_keypad = bf;
    return OK;
}

int meta(WINDOW *win, bool bf)
{
    (void)win; (void)bf;
    return OK;
}

int raw(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    if (!_raw_mode)
        _saved_tios = t;
    cfmakeraw(&t);
    /* We keep ISIG for signal handling, but disable ICANON and ECHO */
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    _raw_mode = true;
    _echo_mode = false;
    return OK;
}

int noraw(void)
{
    if (_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &_saved_tios);
        _raw_mode = false;
    }
    return OK;
}

int cbreak(void)
{
    return raw();
}

int nocbreak(void)
{
    return noraw();
}

int echo(void)
{
    _echo_mode = true;
    return OK;
}

int noecho(void)
{
    _echo_mode = false;
    return OK;
}

int nodelay(WINDOW *win, bool bf)
{
    if (!win) return ERR;
    win->_nodelay = bf;
    win->_delay = bf ? 0 : -1;
    return OK;
}

int halfdelay(int tenths)
{
    _halfdelay_mode = true;
    _halfdelay_tenths = tenths;
    return OK;
}

int clearok(WINDOW *win, bool bf)
{
    if (!win) return ERR;
    win->_clearok = bf;
    return OK;
}

int leaveok(WINDOW *win, bool bf)
{
    if (!win) return ERR;
    win->_leaveok = bf;
    return OK;
}

int idlok(WINDOW *win, bool bf)
{
    (void)win; (void)bf;
    return OK;
}

int notimeout(WINDOW *win, bool bf)
{
    (void)win; (void)bf;
    return OK;
}

void wtimeout(WINDOW *win, int delay)
{
    if (!win) return;
    win->_delay = delay;
    win->_nodelay = (delay == 0);
}

int set_escdelay(int size)
{
    ESCDELAY = size;
    return OK;
}

void use_env(bool f)
{
    _use_env = f;
}

/* ===================================================================
 * Input - escape sequence parsing
 * =================================================================== */

static void _setup_default_keys(void)
{
    _nkeydefs = 0;

    /* Function keys */
    struct { const char *seq; int code; } defs[] = {
        { "\033OP",     KEY_F(1)  },
        { "\033OQ",     KEY_F(2)  },
        { "\033OR",     KEY_F(3)  },
        { "\033OS",     KEY_F(4)  },
        { "\033[15~",   KEY_F(5)  },
        { "\033[17~",   KEY_F(6)  },
        { "\033[18~",   KEY_F(7)  },
        { "\033[19~",   KEY_F(8)  },
        { "\033[20~",   KEY_F(9)  },
        { "\033[21~",   KEY_F(10) },
        { "\033[23~",   KEY_F(11) },
        { "\033[24~",   KEY_F(12) },
        /* Arrow keys */
        { "\033[A",     KEY_UP    },
        { "\033[B",     KEY_DOWN  },
        { "\033[C",     KEY_RIGHT },
        { "\033[D",     KEY_LEFT  },
        /* Home/End */
        { "\033[H",     KEY_HOME  },
        { "\033[F",     KEY_END   },
        { "\033[1~",    KEY_HOME  },
        { "\033[4~",    KEY_END   },
        { "\033OH",     KEY_HOME  },
        { "\033OF",     KEY_END   },
        /* Insert/Delete */
        { "\033[2~",    KEY_IC    },
        { "\033[3~",    KEY_DC    },
        /* Page Up/Down */
        { "\033[5~",    KEY_PPAGE },
        { "\033[6~",    KEY_NPAGE },
        /* Shifted arrows (xterm) */
        { "\033[1;2A",  KEY_SUP    },
        { "\033[1;2B",  KEY_SDOWN  },
        { "\033[1;2C",  KEY_SRIGHT },
        { "\033[1;2D",  KEY_SLEFT  },
        /* Shifted Home/End */
        { "\033[1;2H",  KEY_SHOME  },
        { "\033[1;2F",  KEY_SEND   },
        /* Ctrl arrows (unique codes) */
        { "\033[1;5A",  KEY_CUP    },
        { "\033[1;5B",  KEY_CDOWN  },
        { "\033[1;5C",  KEY_CRIGHT },
        { "\033[1;5D",  KEY_CLEFT  },
        /* Ctrl+Shift arrows (unique codes) */
        { "\033[1;6A",  KEY_CSUP   },
        { "\033[1;6B",  KEY_CSDOWN },
        { "\033[1;6C",  KEY_CSRIGHT},
        { "\033[1;6D",  KEY_CSLEFT },
        /* Ctrl Home/End */
        { "\033[1;5H",  KEY_CHOME  },
        { "\033[1;5F",  KEY_CEND   },
        /* Ctrl+Shift Home/End */
        { "\033[1;6H",  KEY_CSHOME },
        { "\033[1;6F",  KEY_CSEND  },
        /* Shift Delete/Insert */
        { "\033[3;2~",  KEY_SDC    },
        { "\033[2;2~",  KEY_SIC    },
        /* Ctrl Delete/Insert */
        { "\033[3;5~",  KEY_CDC    },
        { "\033[2;5~",  KEY_CIC    },
        /* Ctrl+Shift Delete */
        { "\033[3;6~",  KEY_CSDC   },
        /* Shift PgUp/PgDn */
        { "\033[5;2~",  KEY_SPPAGE },
        { "\033[6;2~",  KEY_SNPAGE },
        /* Ctrl PgUp/PgDn */
        { "\033[5;5~",  KEY_CPPAGE },
        { "\033[6;5~",  KEY_CNPAGE },
        /* Alt arrows */
        { "\033[1;3A",  KEY_AUP    },
        { "\033[1;3B",  KEY_ADOWN  },
        { "\033[1;3C",  KEY_ARIGHT },
        { "\033[1;3D",  KEY_ALEFT  },
        /* Alt Home/End */
        { "\033[1;3H",  KEY_AHOME  },
        { "\033[1;3F",  KEY_AEND   },
        /* Alt PgUp/PgDn */
        { "\033[5;3~",  KEY_APPAGE },
        { "\033[6;3~",  KEY_ANPAGE },
        /* Alt Insert/Delete */
        { "\033[2;3~",  KEY_AIC    },
        { "\033[3;3~",  KEY_ADC    },
        /* Shift+Alt arrows */
        { "\033[1;4A",  KEY_SAUP   },
        { "\033[1;4B",  KEY_SADOWN },
        { "\033[1;4C",  KEY_SARIGHT},
        { "\033[1;4D",  KEY_SALEFT },
        /* Back-tab */
        { "\033[Z",     KEY_BTAB   },
        { NULL, 0 }
    };

    for (int i = 0; defs[i].seq; i++) {
        if (_nkeydefs < MAX_KEYDEFS) {
            strncpy(_keydefs[_nkeydefs].seq, defs[i].seq, 15);
            _keydefs[_nkeydefs].seq[15] = '\0';
            _keydefs[_nkeydefs].code = defs[i].code;
            _nkeydefs++;
        }
    }
}

static int _read_byte(int timeout_ms)
{
    if (timeout_ms == 0) {
        /* Non-blocking: use O_NONBLOCK */
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        unsigned char c;
        int n = read(STDIN_FILENO, &c, 1);
        fcntl(STDIN_FILENO, F_SETFL, flags);
        if (n <= 0) return ERR;
        return c;
    } else if (timeout_ms > 0) {
        /* Timed wait using VMIN/VTIME */
        struct termios t;
        tcgetattr(STDIN_FILENO, &t);
        struct termios save = t;
        t.c_cc[VMIN] = 0;
        /* VTIME is in tenths of seconds */
        int vtime = (timeout_ms + 99) / 100;
        if (vtime > 255) vtime = 255;
        t.c_cc[VTIME] = (unsigned char)vtime;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        unsigned char c;
        int n = read(STDIN_FILENO, &c, 1);
        tcsetattr(STDIN_FILENO, TCSANOW, &save);
        if (n <= 0) return ERR;
        return c;
    } else {
        /* Blocking */
        unsigned char c;
        int n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) return ERR;
        return c;
    }
}

static int _match_escape(void)
{
    /* We've read ESC (0x1B). Try to read more bytes to match sequences. */
    char seq[32];
    int  seq_len = 0;
    seq[seq_len++] = '\033';

    /* Read the next byte with a short timeout */
    int b = _read_byte(ESCDELAY);
    if (b == ERR) {
        /* Just an escape key press */
        return '\033';
    }
    seq[seq_len++] = (char)b;

    /* For CSI sequences (\033[...) and SS3 (\033O...) */
    if (b == '[' || b == 'O') {
        /* Read up to 16 more bytes looking for terminating character */
        for (int i = 0; i < 16; i++) {
            b = _read_byte(ESCDELAY);
            if (b == ERR) break;
            seq[seq_len++] = (char)b;

            /* Check for terminating character (letter or ~) */
            if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~') {
                break;
            }
        }
    }
    seq[seq_len] = '\0';

    /* Try to match against key definitions */
    for (int i = 0; i < _nkeydefs; i++) {
        if (strcmp(seq, _keydefs[i].seq) == 0)
            return _keydefs[i].code;
    }

    /* Mouse SGR: \033[<...M or \033[<...m */
    if (seq_len > 3 && seq[1] == '[' && seq[2] == '<') {
        /* Parse SGR mouse: \033[<Cb;Cx;CyM or m */
        int cb = 0, cx = 0, cy = 0;
        char *p = seq + 3;
        cb = (int)strtol(p, &p, 10); if (*p == ';') p++;
        cx = (int)strtol(p, &p, 10); if (*p == ';') p++;
        cy = (int)strtol(p, &p, 10);
        char term = *p;

        _last_mouse.x = cx - 1;
        _last_mouse.y = cy - 1;
        _last_mouse.bstate = 0;

        if (term == 'M') {
            /* Press */
            switch (cb & 3) {
                case 0: _last_mouse.bstate = BUTTON1_PRESSED; break;
                case 1: _last_mouse.bstate = BUTTON2_PRESSED; break;
                case 2: _last_mouse.bstate = BUTTON3_PRESSED; break;
            }
            if (cb & 64) {
                /* Scroll */
                if ((cb & 3) == 0) _last_mouse.bstate = BUTTON4_PRESSED;
                else               _last_mouse.bstate = BUTTON5_PRESSED;
            }
        } else if (term == 'm') {
            /* Release */
            _last_mouse.bstate = BUTTON1_RELEASED;
        }

        if (cb & 4)  _last_mouse.bstate |= BUTTON_SHIFT;
        if (cb & 8)  _last_mouse.bstate |= BUTTON_ALT;
        if (cb & 16) _last_mouse.bstate |= BUTTON_CTRL;

        return KEY_MOUSE;
    }

    /* Classic mouse: \033[M + 3 bytes */
    if (seq_len >= 3 && seq[1] == '[' && seq[2] == 'M' && seq_len >= 6) {
        int cb = (unsigned char)seq[3] - 32;
        int cx = (unsigned char)seq[4] - 32;
        int cy = (unsigned char)seq[5] - 32;

        _last_mouse.x = cx - 1;
        _last_mouse.y = cy - 1;
        _last_mouse.bstate = 0;

        switch (cb & 3) {
            case 0: _last_mouse.bstate = BUTTON1_PRESSED; break;
            case 1: _last_mouse.bstate = BUTTON2_PRESSED; break;
            case 2: _last_mouse.bstate = BUTTON3_PRESSED; break;
            case 3: _last_mouse.bstate = BUTTON1_RELEASED; break;
        }

        return KEY_MOUSE;
    }

    /* For Meta+key (ESC followed by printable char), return Meta */
    if (seq_len == 2 && (unsigned char)seq[1] >= 0x20) {
        /* Push back the second byte and return ESC */
        ungetch((unsigned char)seq[1]);
        return '\033';
    }

    /* Unrecognized sequence - push back all bytes except ESC */
    for (int i = seq_len - 1; i >= 1; i--)
        ungetch((unsigned char)seq[i]);
    return '\033';
}

int wgetch(WINDOW *win)
{
    if (!win) return ERR;

    /* Per ncurses spec: if the window has been modified since the last
     * refresh, call wrefresh() to push changes to the screen before
     * blocking for input.  This is critical for nano's update_line()
     * which writes to midwin without an explicit wnoutrefresh(). */
    if (win->_touched) {
        wrefresh(win);
    }

    /* Flush output before reading */
    _flush();

    /* Check resize signal */
    if (_got_resize) {
        _got_resize = 0;
        _detect_size();
        return KEY_RESIZE;
    }

    /* Check ungetch buffer first */
    if (_ungetch_count > 0) {
        int ch = _ungetch_buf[_ungetch_head];
        _ungetch_head = (_ungetch_head + 1) % UNGETCH_MAX;
        _ungetch_count--;
        return ch;
    }

    /* Determine timeout */
    int timeout_ms = -1;  /* blocking by default */
    if (win->_nodelay)
        timeout_ms = 0;
    else if (win->_delay >= 0)
        timeout_ms = win->_delay;
    else if (_halfdelay_mode)
        timeout_ms = _halfdelay_tenths * 100;

    int b = _read_byte(timeout_ms);
    if (b == ERR) return ERR;

    /* Handle escape sequences if keypad mode is on */
    if (b == '\033' && win->_keypad)
        return _match_escape();

    /* Backspace / Delete mapping */
    if (b == 127)
        return KEY_BACKSPACE;

    return b;
}

int ungetch(int ch)
{
    if (_ungetch_count >= UNGETCH_MAX) return ERR;
    _ungetch_buf[_ungetch_tail] = ch;
    _ungetch_tail = (_ungetch_tail + 1) % UNGETCH_MAX;
    _ungetch_count++;
    return OK;
}

/* ===================================================================
 * Mouse
 * =================================================================== */

mmask_t mousemask(mmask_t newmask, mmask_t *oldmask)
{
    if (oldmask)
        *oldmask = _mouse_mask;

    if (newmask && !_mouse_mask) {
        /* Enable mouse tracking (SGR mode) */
        _emits("\033[?1000h\033[?1002h\033[?1006h");
        _flush();
    } else if (!newmask && _mouse_mask) {
        /* Disable mouse tracking */
        _emits("\033[?1000l\033[?1002l\033[?1006l");
        _flush();
    }

    _mouse_mask = newmask;
    return newmask;
}

int getmouse(MEVENT *event)
{
    if (!event) return ERR;
    *event = _last_mouse;
    return OK;
}

bool wmouse_trafo(const WINDOW *win, int *pY, int *pX, bool to_screen)
{
    if (!win || !pY || !pX) return false;

    if (to_screen) {
        *pY += win->_begy;
        *pX += win->_begx;
    } else {
        *pY -= win->_begy;
        *pX -= win->_begx;
        if (*pY < 0 || *pY > win->_maxy || *pX < 0 || *pX > win->_maxx)
            return false;
    }
    return true;
}

/* ===================================================================
 * Misc
 * =================================================================== */

int beep(void)
{
    _emits("\007");
    _flush();
    return OK;
}

int flash(void)
{
    /* Reverse video briefly - just emit BEL as fallback */
    return beep();
}

int napms(int ms)
{
    if (ms > 0)
        usleep(ms * 1000);
    return OK;
}

static char _keyname_buf[32];

char *keyname(int c)
{
    if (c >= 0 && c < 32) {
        snprintf(_keyname_buf, sizeof(_keyname_buf), "^%c", c + 64);
    } else if (c == 127) {
        snprintf(_keyname_buf, sizeof(_keyname_buf), "^?");
    } else if (c >= KEY_MIN && c <= KEY_MAX) {
        snprintf(_keyname_buf, sizeof(_keyname_buf), "KEY_%d", c);
    } else if (c >= 32 && c < 127) {
        _keyname_buf[0] = (char)c;
        _keyname_buf[1] = '\0';
    } else {
        snprintf(_keyname_buf, sizeof(_keyname_buf), "\\%03o", (unsigned)c);
    }
    return _keyname_buf;
}

int key_defined(const char *definition)
{
    if (!definition) return 0;
    for (int i = 0; i < _nkeydefs; i++) {
        if (strcmp(definition, _keydefs[i].seq) == 0)
            return _keydefs[i].code;
    }
    return 0;
}

int define_key(const char *definition, int keycode)
{
    if (!definition) return ERR;

    /* Check if already defined, update */
    for (int i = 0; i < _nkeydefs; i++) {
        if (_keydefs[i].code == keycode) {
            strncpy(_keydefs[i].seq, definition, 15);
            _keydefs[i].seq[15] = '\0';
            return OK;
        }
    }

    /* Add new definition */
    if (_nkeydefs < MAX_KEYDEFS) {
        strncpy(_keydefs[_nkeydefs].seq, definition, 15);
        _keydefs[_nkeydefs].seq[15] = '\0';
        _keydefs[_nkeydefs].code = keycode;
        _nkeydefs++;
        return OK;
    }

    return ERR;
}

/* ===================================================================
 * Terminfo stubs
 * =================================================================== */

/* We provide minimal tigetstr for the capabilities nano queries */

char *tigetstr(const char *capname)
{
    if (!capname) return (char *)-1;

    /* nano uses tigetstr to look up keys like "kDC5", "kLFT5", etc. */
    /* Return -1 (not found) for capabilities we don't support */
    /* Common ones we could handle: */
    if (strcmp(capname, "smcup") == 0) return "\033[?1049h";
    if (strcmp(capname, "rmcup") == 0) return "\033[?1049l";
    if (strcmp(capname, "civis") == 0) return "\033[?25l";
    if (strcmp(capname, "cnorm") == 0) return "\033[?25h";
    if (strcmp(capname, "clear") == 0) return "\033[2J\033[H";
    if (strcmp(capname, "el")   == 0) return "\033[K";
    if (strcmp(capname, "ed")   == 0) return "\033[J";
    if (strcmp(capname, "cup")  == 0) return "\033[%i%p1%d;%p2%dH";
    if (strcmp(capname, "bold") == 0) return "\033[1m";
    if (strcmp(capname, "rev")  == 0) return "\033[7m";
    if (strcmp(capname, "sgr0") == 0) return "\033[0m";
    if (strcmp(capname, "smul") == 0) return "\033[4m";
    if (strcmp(capname, "rmul") == 0) return "\033[24m";

    /* key capabilities - return the escape sequences for them */
    if (strcmp(capname, "kcuu1") == 0) return "\033[A";
    if (strcmp(capname, "kcud1") == 0) return "\033[B";
    if (strcmp(capname, "kcuf1") == 0) return "\033[C";
    if (strcmp(capname, "kcub1") == 0) return "\033[D";
    if (strcmp(capname, "khome") == 0) return "\033[H";
    if (strcmp(capname, "kend")  == 0) return "\033[F";
    if (strcmp(capname, "kdch1") == 0) return "\033[3~";
    if (strcmp(capname, "kich1") == 0) return "\033[2~";
    if (strcmp(capname, "knp")   == 0) return "\033[6~";
    if (strcmp(capname, "kpp")   == 0) return "\033[5~";

    /* Shifted / Ctrl key variants that nano looks for */
    if (strcmp(capname, "kDC")   == 0) return "\033[3;2~";
    if (strcmp(capname, "kDC5")  == 0) return "\033[3;5~";
    if (strcmp(capname, "kDC6")  == 0) return "\033[3;6~";
    if (strcmp(capname, "kDC3")  == 0) return "\033[3;3~";
    if (strcmp(capname, "kIC3")  == 0) return "\033[2;3~";
    if (strcmp(capname, "kLFT5") == 0) return "\033[1;5D";
    if (strcmp(capname, "kLFT6") == 0) return "\033[1;6D";
    if (strcmp(capname, "kLFT3") == 0) return "\033[1;3D";
    if (strcmp(capname, "kLFT4") == 0) return "\033[1;4D";
    if (strcmp(capname, "kRIT5") == 0) return "\033[1;5C";
    if (strcmp(capname, "kRIT6") == 0) return "\033[1;6C";
    if (strcmp(capname, "kRIT3") == 0) return "\033[1;3C";
    if (strcmp(capname, "kRIT4") == 0) return "\033[1;4C";
    if (strcmp(capname, "kUP5")  == 0) return "\033[1;5A";
    if (strcmp(capname, "kUP6")  == 0) return "\033[1;6A";
    if (strcmp(capname, "kUP3")  == 0) return "\033[1;3A";
    if (strcmp(capname, "kUP4")  == 0) return "\033[1;4A";
    if (strcmp(capname, "kUP")   == 0) return "\033[1;2A";
    if (strcmp(capname, "kDN5")  == 0) return "\033[1;5B";
    if (strcmp(capname, "kDN6")  == 0) return "\033[1;6B";
    if (strcmp(capname, "kDN3")  == 0) return "\033[1;3B";
    if (strcmp(capname, "kDN4")  == 0) return "\033[1;4B";
    if (strcmp(capname, "kDN")   == 0) return "\033[1;2B";
    if (strcmp(capname, "kHOM5") == 0) return "\033[1;5H";
    if (strcmp(capname, "kHOM6") == 0) return "\033[1;6H";
    if (strcmp(capname, "kHOM3") == 0) return "\033[1;3H";
    if (strcmp(capname, "kHOM4") == 0) return "\033[1;4H";
    if (strcmp(capname, "kEND5") == 0) return "\033[1;5F";
    if (strcmp(capname, "kEND6") == 0) return "\033[1;6F";
    if (strcmp(capname, "kEND3") == 0) return "\033[1;3F";
    if (strcmp(capname, "kEND4") == 0) return "\033[1;4F";
    if (strcmp(capname, "kIC5")  == 0) return "\033[2;5~";
    if (strcmp(capname, "kPRV3") == 0) return "\033[5;3~";
    if (strcmp(capname, "kNXT3") == 0) return "\033[6;3~";

    /* Not found */
    return (char *)-1;
}

int tigetnum(const char *capname)
{
    if (!capname) return -2;
    if (strcmp(capname, "colors") == 0) return COLORS;
    if (strcmp(capname, "pairs")  == 0) return COLOR_PAIRS;
    if (strcmp(capname, "cols")   == 0) return COLS;
    if (strcmp(capname, "lines")  == 0) return LINES;
    return -1;
}

int tigetflag(const char *capname)
{
    if (!capname) return -1;
    if (strcmp(capname, "ccc") == 0)  return 0;  /* can't change colors */
    if (strcmp(capname, "bce") == 0)  return 1;  /* back color erase */
    return -1;
}

int setupterm(const char *term, int filedes, int *errret)
{
    (void)term; (void)filedes;
    if (errret) *errret = 1;  /* success */
    return OK;
}

int del_curterm(void *oterm)
{
    (void)oterm;
    return OK;
}

int putp(const char *str)
{
    if (!str) return ERR;
    _emits(str);
    _flush();
    return OK;
}

int tputs(const char *str, int affcnt, int (*putfunc)(int))
{
    (void)affcnt;
    if (!str) return ERR;
    if (putfunc) {
        while (*str)
            putfunc((unsigned char)*str++);
    } else {
        _emits(str);
        _flush();
    }
    return OK;
}

char *tparm(const char *str, ...)
{
    /* Minimal tparm: handle %i, %p1%d, %p2%d for cup */
    static char result[256];
    if (!str) return NULL;

    va_list ap;
    va_start(ap, str);
    long params[9] = {0};
    for (int i = 0; i < 9; i++)
        params[i] = va_arg(ap, long);
    va_end(ap);

    bool incr = false;
    int ri = 0;
    const char *s = str;
    while (*s && ri < 250) {
        if (*s == '%') {
            s++;
            if (*s == 'i') {
                incr = true;
                s++;
            } else if (*s == 'p') {
                s++;
                int pn = *s - '1';
                s++;
                if (*s == '%') s++;
                if (*s == 'd') {
                    long val = (pn >= 0 && pn < 9) ? params[pn] : 0;
                    if (incr) val++;
                    ri += snprintf(result + ri, 256 - ri, "%ld", val);
                    s++;
                }
            } else if (*s == '%') {
                result[ri++] = '%';
                s++;
            } else if (*s == 'd') {
                ri += snprintf(result + ri, 256 - ri, "%ld", params[0]);
                s++;
            } else {
                result[ri++] = '%';
            }
        } else {
            result[ri++] = *s++;
        }
    }
    result[ri] = '\0';
    return result;
}

char *tiparm(const char *str, ...)
{
    /* Forward to tparm with the same args */
    va_list ap;
    va_start(ap, str);
    long p1 = va_arg(ap, long);
    long p2 = va_arg(ap, long);
    va_end(ap);
    return tparm(str, p1, p2);
}

/* ===================================================================
 * Screen resize
 * =================================================================== */

int resizeterm(int lines, int columns)
{
    LINES = lines;
    COLS = columns;

    if (stdscr) wresize(stdscr, lines, columns);
    if (curscr) wresize(curscr, lines, columns);

    return OK;
}

int is_term_resized(int lines, int columns)
{
    return (lines != LINES || columns != COLS);
}

/* ===================================================================
 * Extra functions needed by nano
 * =================================================================== */

int mouseinterval(int erval)
{
    (void)erval;
    return 166; /* default ncurses value */
}

int nonl(void)
{
    /* Disable newline translation - no-op for our ANSI backend */
    return OK;
}

int typeahead(int fd)
{
    (void)fd;
    /* Control typeahead checking - no-op */
    return OK;
}

bool wenclose(const WINDOW *win, int y, int x)
{
    if (!win) return false;
    return (y >= win->_begy && y < win->_begy + win->_maxy + 1 &&
            x >= win->_begx && x < win->_begx + win->_maxx + 1);
}

char *tgetstr(const char *id, char **area)
{
    (void)id;
    (void)area;
    return (char *)0; /* not found */
}
