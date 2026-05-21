// LikeOS-64 VT (Virtual Terminal) layer
// ANSI/VT100/VT220/xterm escape sequence parser and terminal emulator state.
// Sits between tty.c (line discipline, PTY) and console.c (framebuffer renderer).
//
// Architecture:
//   apps/shell
//        ↓
//   tty.c  — transport, termios, line-discipline, PTY read/write, echo, signals
//        ↓
//   vt.c   — ANSI/CSI/DCS/OSC parser, SGR, cursor state, scroll region,
//             alt-screen buffer, cell buffer, dirty tracking
//        ↓
//   console.c — framebuffer renderer bridge, cursor blink timer, scrollback
//        ↓
//   fb_optimize.c — pixel-level drawing

#ifndef _KERNEL_VT_H_
#define _KERNEL_VT_H_

#include "types.h"
#include "tty.h"

/* ---- Cell buffer --------------------------------------------------- */

/* Per-cell rendering attributes: the canonical screen backing store.
 * Stored as resolved RGB so cell-level redraw needs no SGR state.      */
typedef struct {
    uint8_t  ch;       /* rendered character (ASCII/Latin-1) */
    uint8_t  _pad[3];
    uint32_t fg_rgb;   /* resolved foreground RGB (after bold, reverse)  */
    uint32_t bg_rgb;   /* resolved background RGB                        */
} vt_cell_t;

/* Maximum terminal dimensions supported by the static cell buffers.
 * 80 rows covers 4K at 16-px font; 256 cols covers 4K at 8-px font.   */
#define VT_MAX_ROWS  80
#define VT_MAX_COLS  256

/* ---- ANSI/CSI parser states --------------------------------------- */

typedef enum {
    ANSI_NORMAL = 0,
    ANSI_ESC,          /* saw ESC (0x1B)                                 */
    ANSI_CSI,          /* saw ESC [                                      */
    ANSI_OSC,          /* saw ESC ] — collecting until ST or BEL         */
    ANSI_OSC_ESC,      /* saw ESC inside OSC, waiting for '\\' (ST)      */
    ANSI_DCS,          /* saw ESC P/X/^/_ — eat until ST                 */
    ANSI_DCS_ESC,      /* saw ESC inside DCS, waiting for '\\'           */
    ANSI_CHARSET,      /* saw ESC ( ) * + — eat 1 byte                   */
    ANSI_HASH,         /* saw ESC # — eat 1 byte                         */
} ansi_state_t;

#define ANSI_MAX_PARAMS 16

/* ---- Complete VT terminal emulator state -------------------------- */

struct vt_state {
    /* ---- ANSI/CSI parser ------------------------------------------ */
    ansi_state_t state;
    int          params[ANSI_MAX_PARAMS];
    int          nparam;
    int          cur_param;
    int          have_digit;
    char         intermediate;   /* '?' '>' '=' '<' or 0x20..0x2F      */
    int          private_mode;   /* saw '?' after CSI                   */

    /* ---- SGR (color / attribute) state ----------------------------- */
    uint8_t      cur_vga_fg;     /* VGA palette fg index (0-15)         */
    uint8_t      cur_vga_bg;     /* VGA palette bg index (0-15)         */
    int          bold;
    int          reverse;
    int          fg_is_rgb;      /* 1 if fg is true-color               */
    int          bg_is_rgb;      /* 1 if bg is true-color               */
    uint32_t     fg_rgb;         /* true-color fg value                 */
    uint32_t     bg_rgb;         /* true-color bg value                 */

    /* ---- Resolved rendering colors (updated by vt_recompute_colors) */
    uint32_t     resolved_fg_rgb; /* effective fg after bold + reverse  */
    uint32_t     resolved_bg_rgb; /* effective bg after bold + reverse  */

    /* ---- Saved cursor + SGR (DECSC / SCOSC) ------------------------ */
    int      saved_cur_row;
    int      saved_cur_col;
    int      saved_valid;
    uint8_t  saved_vga_fg;
    uint8_t  saved_vga_bg;
    int      saved_bold;
    int      saved_reverse;
    int      saved_fg_is_rgb;
    int      saved_bg_is_rgb;
    uint32_t saved_fg_rgb;
    uint32_t saved_bg_rgb;

    /* ---- Scroll region (DECSTBM) ----------------------------------- */
    int      scroll_top;  /* -1 = full screen                          */
    int      scroll_bot;  /* -1 = full screen                          */

    /* ---- Alternate screen ------------------------------------------ */
    int      in_alt_screen;
    uint32_t alt_screen_sb_total; /* scrollback snapshot at 1049h entry */

    /* ---- Cursor tracking (canonical; always in sync with console.c) */
    int      cur_row;
    int      cur_col;
    int      pending_wrap; /* deferred xenl wrap flag                   */

    /* ---- Cell buffer (screen backing store) ------------------------ */
    vt_cell_t *cells;  /* points to g_main_cells or g_alt_cells        */
    uint8_t   *dirty;  /* dirty[row] = 1 if row modified since last flush */

    /* ---- Terminal dimensions --------------------------------------- */
    int cols;
    int rows;

    /* ---- Back-link to associated tty (DSR/DA reply injection) ------ */
    tty_t *tty;
};

/* ---- Public API ---------------------------------------------------- */

/* Initialise the VT instance; queries console for current dimensions.  */
void vt_init(struct vt_state *vt, int cols, int rows, tty_t *tty);

/* Process one byte of TTY output through the ANSI parser.              */
void vt_process_char(struct vt_state *vt, char c);

/* Update terminal size (e.g. after framebuffer resize).                */
void vt_resize(struct vt_state *vt, int cols, int rows);

/* Full soft reset: SGR defaults, scroll region, alt-screen exit.       */
void vt_reset(struct vt_state *vt);

/* Re-render all dirty rows from the cell buffer (used on alt-screen
 * exit and after resize to restore screen content).                    */
void vt_flush_dirty(struct vt_state *vt);

/* tty->output function pointer target.  Routes to g_console_vt.       */
void vt_output_char(tty_t *tty, char c);

/* Called by tty_write after releasing tty_lock: returns 1 and clears
 * the pending flag if the ANSI parser enqueued a DSR/DA reply.        */
int  vt_consume_reply_pending(void);

/* The single global console VT instance (defined in vt.c).            */
extern struct vt_state g_console_vt;

#endif /* _KERNEL_VT_H_ */
