/*
 * curses.h - ncurses-compatible header for LikeOS
 *
 * Minimal ncurses API implementation using ANSI/VT100 escape sequences.
 * Provides the subset of ncurses needed by GNU nano and other curses programs.
 * Based on ncurses 6.5 API from ports/lib/ncurses-6.5/.
 *
 * Copyright (C) 2026 LikeOS Project
 */

#ifndef _CURSES_H
#define _CURSES_H
#define _CURSES_H_  /* Also used by nano's place_the_cursor() for wnoutrefresh */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Types ===== */

typedef uint32_t chtype;
typedef uint32_t attr_t;
typedef unsigned long mmask_t;

typedef struct _win_st {
    int _cury, _curx;       /* current cursor position */
    int _maxy, _maxx;       /* max y/x (0-based, so rows-1 / cols-1) */
    int _begy, _begx;       /* screen position of upper-left corner */
    attr_t _attrs;          /* current attributes */
    short _color;           /* current color pair */
    bool _keypad;           /* keypad mode enabled */
    bool _nodelay;          /* non-blocking input */
    bool _scrollok;         /* scrolling allowed */
    bool _clearok;          /* clear on next refresh */
    bool _leaveok;          /* leave cursor after update */
    int _delay;             /* input timeout (-1 = blocking) */
    /* backing store */
    chtype **_line;         /* line contents [_maxy+1][_maxx+1] */
    bool *_dirty;           /* dirty line flags */
    bool _touched;          /* any line dirty */
} WINDOW;

typedef struct {
    short id;
    mmask_t bstate;
    int x, y, z;
} MEVENT;

/* ===== Global variables ===== */

extern WINDOW *stdscr;
extern WINDOW *curscr;
extern int LINES;
extern int COLS;
extern int COLORS;
extern int COLOR_PAIRS;
extern int TABSIZE;
extern int ESCDELAY;

/* ===== Boolean ===== */

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef ERR
#define ERR (-1)
#endif
#ifndef OK
#define OK  0
#endif

/* ===== Attributes ===== */

#define A_NORMAL     0x00000000U
#define A_STANDOUT   0x00010000U
#define A_UNDERLINE  0x00020000U
#define A_REVERSE    0x00040000U
#define A_BLINK      0x00080000U
#define A_DIM        0x00100000U
#define A_BOLD       0x00200000U
#define A_ITALIC     0x00800000U
#define A_CHARTEXT   0x000000FFU
#define A_COLOR      0x0000FF00U

#define COLOR_PAIR(n) (((n) & 0xFF) << 8)
#define PAIR_NUMBER(a) (((a) & A_COLOR) >> 8)

/* ===== Colors ===== */

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7

/* ===== Key codes ===== */

#define KEY_MIN       0x101
#define KEY_DOWN      0x102
#define KEY_UP        0x103
#define KEY_LEFT      0x104
#define KEY_RIGHT     0x105
#define KEY_HOME      0x106
#define KEY_BACKSPACE 0x107
#define KEY_F0        0x108
#define KEY_F(n)      (KEY_F0 + (n))
#define KEY_DC        0x14A    /* Delete character */
#define KEY_IC        0x14B    /* Insert character */
#define KEY_NPAGE     0x152    /* Next page (Page Down) */
#define KEY_PPAGE     0x153    /* Previous page (Page Up) */
#define KEY_END       0x168    /* End */
#define KEY_ENTER     0x157    /* Enter */
#define KEY_BTAB      0x161    /* Back-tab */
#define KEY_BEG       0x162
#define KEY_CANCEL    0x163
#define KEY_SF        0x150    /* Scroll forward (shift-down) */
#define KEY_SR        0x151    /* Scroll backward (shift-up) */
#define KEY_A1        0x1C1    /* Upper left of keypad */
#define KEY_A3        0x1C3    /* Upper right of keypad */
#define KEY_B2        0x1C4    /* Center of keypad */
#define KEY_C1        0x1C7    /* Lower left of keypad */
#define KEY_C3        0x1C9    /* Lower right of keypad */
#define KEY_MOUSE     0x199
#define KEY_RESIZE    0x19A
#define KEY_MAX       0x1FF
#define KEY_EOL       0x19F
#define KEY_SUSPEND   0x181
#define KEY_SSUSPEND  0x182
#define KEY_SBEG      0x183
#define KEY_SCANCEL   0x184
#define KEY_SDC       0x185
#define KEY_SEND      0x186
#define KEY_SHOME     0x187
#define KEY_SIC       0x188
#define KEY_SLEFT     0x189
#define KEY_SNEXT     0x18A
#define KEY_SPREVIOUS 0x18B
#define KEY_SRIGHT    0x18C
#define KEY_SUP       0x18D
#define KEY_SDOWN     0x18E
#define KEY_SF        0x150
#define KEY_SR        0x151

/* Additional key codes */
#define KEY_ALT_L     0x1A0
#define KEY_ALT_R     0x1A1
#define KEY_CONTROL_L 0x1A2
#define KEY_CONTROL_R 0x1A3
#define KEY_SHIFT_L   0x1A4
#define KEY_SHIFT_R   0x1A5
/* KEY_CENTER, KEY_COMBO, KEY_FRESH are defined by nano itself */
#define KEY_MAX       0x1FF

/* Extended key codes for modified keys (Ctrl, Ctrl+Shift, Alt variants).
 * These must be unique so that nano's key_defined()/get_keycode() can
 * distinguish e.g. Ctrl+Left from Shift+Left. */
#define KEY_CLEFT     0x220  /* Ctrl+Left */
#define KEY_CRIGHT    0x221  /* Ctrl+Right */
#define KEY_CUP       0x222  /* Ctrl+Up */
#define KEY_CDOWN     0x223  /* Ctrl+Down */
#define KEY_CHOME     0x224  /* Ctrl+Home */
#define KEY_CEND      0x225  /* Ctrl+End */
#define KEY_CDN       KEY_CDOWN
#define KEY_CSLEFT    0x230  /* Ctrl+Shift+Left */
#define KEY_CSRIGHT   0x231  /* Ctrl+Shift+Right */
#define KEY_CSUP      0x232  /* Ctrl+Shift+Up */
#define KEY_CSDOWN    0x233  /* Ctrl+Shift+Down */
#define KEY_CSHOME    0x234  /* Ctrl+Shift+Home */
#define KEY_CSEND     0x235  /* Ctrl+Shift+End */
#define KEY_CDC       0x236  /* Ctrl+Delete */
#define KEY_CIC       0x237  /* Ctrl+Insert */
#define KEY_CSDC      0x238  /* Ctrl+Shift+Delete */
#define KEY_SPPAGE    0x240  /* Shift+PgUp */
#define KEY_SNPAGE    0x241  /* Shift+PgDn */
#define KEY_CPPAGE    0x242  /* Ctrl+PgUp */
#define KEY_CNPAGE    0x243  /* Ctrl+PgDn */
#define KEY_ALEFT     0x250  /* Alt+Left */
#define KEY_ARIGHT    0x251  /* Alt+Right */
#define KEY_AUP       0x252  /* Alt+Up */
#define KEY_ADOWN     0x253  /* Alt+Down */
#define KEY_AHOME     0x254  /* Alt+Home */
#define KEY_AEND      0x255  /* Alt+End */
#define KEY_APPAGE    0x256  /* Alt+PgUp */
#define KEY_ANPAGE    0x257  /* Alt+PgDn */
#define KEY_AIC       0x258  /* Alt+Insert */
#define KEY_ADC       0x259  /* Alt+Delete */
#define KEY_SALEFT    0x260  /* Shift+Alt+Left */
#define KEY_SARIGHT   0x261  /* Shift+Alt+Right */
#define KEY_SAUP      0x262  /* Shift+Alt+Up */
#define KEY_SADOWN    0x263  /* Shift+Alt+Down */

/* ===== Mouse ===== */

#define NCURSES_MOUSE_VERSION 2
#define ALL_MOUSE_EVENTS  0x1FFFFFFFL
#define BUTTON1_PRESSED        0x002L
#define BUTTON1_RELEASED       0x001L
#define BUTTON1_CLICKED        0x004L
#define BUTTON1_DOUBLE_CLICKED 0x008L
#define BUTTON2_PRESSED        0x040L
#define BUTTON3_PRESSED        0x200L
#define BUTTON4_PRESSED        0x00080000L  /* scroll up */
#define BUTTON5_PRESSED        0x01000000L  /* scroll down */
#define BUTTON_SHIFT           0x04000000L
#define BUTTON_CTRL            0x08000000L
#define BUTTON_ALT             0x10000000L
#define REPORT_MOUSE_POSITION  0x08000000L

/* ===== ACS characters (line drawing) ===== */

#define ACS_ULCORNER  ('l' | A_NORMAL)
#define ACS_LLCORNER  ('m' | A_NORMAL)
#define ACS_URCORNER  ('k' | A_NORMAL)
#define ACS_LRCORNER  ('j' | A_NORMAL)
#define ACS_HLINE     ('q' | A_NORMAL)
#define ACS_VLINE     ('x' | A_NORMAL)
#define ACS_PLUS      ('n' | A_NORMAL)
#define ACS_LTEE      ('t' | A_NORMAL)
#define ACS_RTEE      ('u' | A_NORMAL)
#define ACS_BTEE      ('v' | A_NORMAL)
#define ACS_TTEE      ('w' | A_NORMAL)

/* ===== Initialization / termination ===== */

WINDOW *initscr(void);
int endwin(void);
int isendwin(void);

/* ===== Window management ===== */

WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
int delwin(WINDOW *win);
WINDOW *subwin(WINDOW *parent, int nlines, int ncols, int begin_y, int begin_x);
WINDOW *derwin(WINDOW *parent, int nlines, int ncols, int begin_y, int begin_x);
int wresize(WINDOW *win, int lines, int columns);

/* ===== Output ===== */

int waddch(WINDOW *win, chtype ch);
int waddstr(WINDOW *win, const char *str);
int waddnstr(WINDOW *win, const char *str, int n);
int mvwaddch(WINDOW *win, int y, int x, chtype ch);
int mvwaddstr(WINDOW *win, int y, int x, const char *str);
int mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int wprintw(WINDOW *win, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* stdscr convenience */
#define addch(ch)           waddch(stdscr, (ch))
#define addstr(str)         waddstr(stdscr, (str))
#define addnstr(str, n)     waddnstr(stdscr, (str), (n))
#define mvaddch(y, x, ch)   mvwaddch(stdscr, (y), (x), (ch))
#define mvaddstr(y, x, str) mvwaddstr(stdscr, (y), (x), (str))
#define printw(...)          wprintw(stdscr, __VA_ARGS__)

/* ===== Input ===== */

int wgetch(WINDOW *win);
int ungetch(int ch);

#define getch() wgetch(stdscr)

/* ===== Refresh ===== */

int wrefresh(WINDOW *win);
int wnoutrefresh(WINDOW *win);
int doupdate(void);
int redrawwin(WINDOW *win);
int wredrawln(WINDOW *win, int beg_line, int num_lines);

#define refresh() wrefresh(stdscr)

/* ===== Cursor / move ===== */

int wmove(WINDOW *win, int y, int x);
int curs_set(int visibility);

#define move(y, x) wmove(stdscr, (y), (x))

#define getyx(win, y, x)    do { (y) = (win)->_cury; (x) = (win)->_curx; } while(0)
#define getbegyx(win, y, x) do { (y) = (win)->_begy; (x) = (win)->_begx; } while(0)
#define getmaxyx(win, y, x) do { (y) = (win)->_maxy + 1; (x) = (win)->_maxx + 1; } while(0)
#define getmaxy(win)        ((win)->_maxy + 1)
#define getmaxx(win)        ((win)->_maxx + 1)
#define getcury(win)        ((win)->_cury)
#define getcurx(win)        ((win)->_curx)

/* ===== Clearing ===== */

int wclear(WINDOW *win);
int werase(WINDOW *win);
int wclrtoeol(WINDOW *win);
int wclrtobot(WINDOW *win);

#define clear()    wclear(stdscr)
#define erase()    werase(stdscr)
#define clrtoeol() wclrtoeol(stdscr)

/* ===== Attributes ===== */

int wattron(WINDOW *win, int attrs);
int wattroff(WINDOW *win, int attrs);
int wattrset(WINDOW *win, int attrs);
int wbkgdset(WINDOW *win, chtype ch);

#define attron(a)  wattron(stdscr, (a))
#define attroff(a) wattroff(stdscr, (a))
#define attrset(a) wattrset(stdscr, (a))

/* ===== Color ===== */

int start_color(void);
bool has_colors(void);
int init_pair(short pair, short fg, short bg);
int use_default_colors(void);

/* ===== Scrolling ===== */

int scrollok(WINDOW *win, bool bf);
int wscrl(WINDOW *win, int n);
int wsetscrreg(WINDOW *win, int top, int bot);

#define scroll(win)      wscrl((win), 1)
#define scrl(n)          wscrl(stdscr, (n))
#define setscrreg(t, b)  wsetscrreg(stdscr, (t), (b))

/* ===== Options ===== */

int keypad(WINDOW *win, bool bf);
int meta(WINDOW *win, bool bf);
int nodelay(WINDOW *win, bool bf);
int halfdelay(int tenths);
int raw(void);
int noraw(void);
int cbreak(void);
int nocbreak(void);
int echo(void);
int noecho(void);
int clearok(WINDOW *win, bool bf);
int leaveok(WINDOW *win, bool bf);
int idlok(WINDOW *win, bool bf);
int notimeout(WINDOW *win, bool bf);
void wtimeout(WINDOW *win, int delay);
int set_escdelay(int size);

#define timeout(d) wtimeout(stdscr, (d))

/* ===== Mouse ===== */

mmask_t mousemask(mmask_t newmask, mmask_t *oldmask);
int getmouse(MEVENT *event);
bool wmouse_trafo(const WINDOW *win, int *pY, int *pX, bool to_screen);

/* ===== Misc ===== */

int beep(void);
int flash(void);
int napms(int ms);
char *keyname(int c);
int key_defined(const char *definition);
int define_key(const char *definition, int keycode);
void use_env(bool f);

/* ===== Extra functions used by nano ===== */

int mouseinterval(int erval);
int nonl(void);
int typeahead(int fd);
bool wenclose(const WINDOW *win, int y, int x);
char *tgetstr(const char *id, char **area);

/* ===== Terminfo ===== */

char *tigetstr(const char *capname);
int tigetnum(const char *capname);
int tigetflag(const char *capname);
int setupterm(const char *term, int filedes, int *errret);
int del_curterm(void *oterm);
int putp(const char *str);
int tputs(const char *str, int affcnt, int (*putfunc)(int));
char *tparm(const char *str, ...);
char *tiparm(const char *str, ...);

/* ===== Screen ===== */

int resizeterm(int lines, int columns);
int is_term_resized(int lines, int columns);

#ifdef __cplusplus
}
#endif

#endif /* _CURSES_H */
