/*
 * term.h - minimal terminfo/termcap header for LikeOS
 * Provides tigetstr/tigetnum/tputs stubs (implementation in curses.c)
 */
#ifndef _TERM_H
#define _TERM_H

#include "curses.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Version sentinels - tmux gates a few helpers (e.g. del_curterm, tiparm_s)
 * on the major/minor version of ncurses.  We export 6.x to enable the
 * modern code paths; the compatible declarations live in curses.h.
 */
#define NCURSES_VERSION_MAJOR 6
#define NCURSES_VERSION_MINOR 4
#define NCURSES_VERSION       "6.4"

/*
 * `cur_term` is a global opaque cookie identifying the active terminfo
 * context.  Our libcurses only supports a single context, so this is
 * just a non-null sentinel.
 */
extern void* cur_term;

/* Provided by curses.c */
/* tigetstr, tigetnum, tigetflag, setupterm, del_curterm, putp, tputs, tparm */

/*
 * The termcap-era capability lookups.
 *
 * curses.c has always implemented these; only the declarations were missing,
 * which C tolerates by assuming int-returning and C++ does not tolerate at all.
 * gdb asks tgetnum ("Co") how many colours the terminal has, so without these
 * it fails to compile rather than falling back to a monochrome answer.
 *
 * tgetstr returns a pointer, so a caller that had to guess would have guessed
 * wrong even where the guess was allowed.
 */
int tgetent(char* bp, const char* name);
int tgetnum(const char* id);
int tgetflag(const char* id);
char* tgetstr(const char* id, char** area);

#ifdef __cplusplus
}
#endif

#endif /* _TERM_H */
