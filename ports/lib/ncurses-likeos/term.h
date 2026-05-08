/*
 * term.h - minimal terminfo/termcap header for LikeOS
 * Provides tigetstr/tigetnum/tputs stubs (implementation in curses.c)
 */
#ifndef _TERM_H
#define _TERM_H

#include "curses.h"

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

#endif /* _TERM_H */
