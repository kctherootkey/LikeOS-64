// LikeOS-64 VT (Virtual Terminal) layer
// ANSI/VT100/VT220/xterm terminal emulator extracted from tty.c.
//
// Responsibilities:
//   - ANSI/CSI/OSC/DCS/CHARSET escape sequence parser
//   - SGR (Select Graphic Rendition): colors, bold, reverse, 256-color, true-color
//   - Cursor state (position, save/restore via DECSC/DECRC and SCOSC/SCORC)
//   - Scroll region (DECSTBM)
//   - Alternate screen buffer (DEC private modes 1047 / 1049)
//   - Cell buffer: per-cell (char, fg_rgb, bg_rgb) for full alt-screen swap and dirty redraw
//   - Dirty-row tracking: marks which rows changed; vt_flush_dirty() re-renders from cell buffer
//   - DSR / DA / XTWINOPS terminal query replies

#include <kernel/vt.h>
#include <kernel/console.h>
#include <kernel/memory.h>
#include <kernel/bug.h>

/* Forward declarations from tty.c (same kernel link unit).
 * tty_enqueue_read_locked must be called with tty_lock held (IRQs off);
 * it is used by the DSR/DA reply injection path which runs inside
 * tty_write under that lock.                                            */
extern void tty_enqueue_read_locked(tty_t *tty, char c);

/* itoa_simple: in tty.c (also used for mouse SGR reports there).       */
extern int  itoa_simple(int val, char *buf);

/* Internal helper in console.c: VGA palette index → 24-bit RGB.       */
extern uint32_t vga_to_rgb(uint8_t v);

/* =========================================================================
 * DSR/DA reply pending flag.
 *
 * Set when the ANSI parser injects a query reply (DSR/DA/XTWINOPS) into
 * the TTY read buffer.  tty_write() checks this AFTER releasing tty_lock
 * and wakes blocked readers then — scheduling calls cannot be made while
 * tty_lock is held with IRQs off.
 * ======================================================================== */
static volatile int g_console_reply_pending = 0;

/* =========================================================================
 * Cell buffer globals (single-console design — one VT instance).
 * ======================================================================== */
static vt_cell_t g_main_cells[VT_MAX_ROWS * VT_MAX_COLS];
static vt_cell_t g_alt_cells [VT_MAX_ROWS * VT_MAX_COLS];
static uint8_t   g_dirty_rows[VT_MAX_ROWS];

/* The single global console VT instance.                                */
struct vt_state g_console_vt;

/* Internal memmove for overlapping cell buffer operations.
 * Kernel libc has mm_memcpy but no memmove; implement it here.         */
static void vt_memmove(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s || d >= s + n)
        mm_memcpy(d, s, n);
    else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
}

/* =========================================================================
 * SGR color tables
 * ======================================================================== */

/* ANSI SGR color index 0-7 → VGA palette index */
static const uint8_t ansi_to_vga_fg[8] = {
    0,   /* 0 = black   → VGA 0  */
    4,   /* 1 = red     → VGA 4  */
    2,   /* 2 = green   → VGA 2  */
    6,   /* 3 = yellow  → VGA 6 (brown) */
    1,   /* 4 = blue    → VGA 1  */
    5,   /* 5 = magenta → VGA 5  */
    3,   /* 6 = cyan    → VGA 3  */
    7,   /* 7 = white   → VGA 7 (light grey) */
};

/* Approximate xterm-256 index to closest 16-color VGA palette entry.   */
static uint8_t xterm256_to_vga(int n) {
    if (n < 0)   n = 0;
    if (n > 255) n = 255;
    if (n < 8)   return ansi_to_vga_fg[n];
    if (n < 16)  return ansi_to_vga_fg[n - 8] + 8;
    if (n >= 232) {
        int g = n - 232;
        if (g < 4)  return 0;
        if (g < 10) return 8;
        if (g < 18) return 7;
        return 15;
    }
    int idx = n - 16;
    int r = idx / 36;
    int g = (idx / 6) % 6;
    int b = idx % 6;
    int bright = (r > 2 || g > 2 || b > 2) ? 8 : 0;
    uint8_t base;
    if (r == g && g == b) {
        if (r == 0)      base = 0;
        else if (r <= 2) { base = 0; bright = 8; }
        else if (r <= 4) { base = 7; bright = 0; }
        else             { base = 7; bright = 8; }
    } else if (r >= g && r >= b)        base = ansi_to_vga_fg[1];
    else if (g >= r && g >= b)          base = ansi_to_vga_fg[2];
    else                                base = ansi_to_vga_fg[4];
    if (r >= 3 && g >= 3 && b < 3)      base = ansi_to_vga_fg[3];
    else if (r >= 3 && b >= 3 && g < 3) base = ansi_to_vga_fg[5];
    else if (g >= 3 && b >= 3 && r < 3) base = ansi_to_vga_fg[6];
    return base + (bright ? 8 : 0);
}

/* =========================================================================
 * Parser helpers
 * ======================================================================== */

static void vt_reset_parser(struct vt_state *vt) {
    vt->state       = ANSI_NORMAL;
    vt->nparam      = 0;
    vt->cur_param   = 0;
    vt->have_digit  = 0;
    vt->private_mode = 0;
    vt->intermediate = 0;
}

/* =========================================================================
 * Color helpers
 * ======================================================================== */

/* Re-compute the active fg/bg pixel colors from the current SGR state
 * and push them to the framebuffer console.  Also caches the resolved
 * values in vt->resolved_fg/bg_rgb so cell-level updates don't need to
 * re-derive them.                                                        */
static void vt_recompute_colors(struct vt_state *vt) {
    uint32_t fg = vt->fg_is_rgb ? vt->fg_rgb : vga_to_rgb(vt->cur_vga_fg);
    uint32_t bg = vt->bg_is_rgb ? vt->bg_rgb : vga_to_rgb(vt->cur_vga_bg);
    if (vt->reverse) { uint32_t t = fg; fg = bg; bg = t; }
    vt->resolved_fg_rgb = fg;
    vt->resolved_bg_rgb = bg;
    console_set_color_rgb(fg, bg);
}

/* =========================================================================
 * Cursor tracking
 * ======================================================================== */

/* Move cursor to (row, col); clamp to screen; sync to console.c.
 * Always use this instead of calling console_set_cursor_pos directly so
 * that vt_state's cursor tracking stays canonical.                      */
static void vt_set_cursor(struct vt_state *vt, int row, int col) {
    int rows = vt->rows > 0 ? vt->rows : 25;
    int cols = vt->cols > 0 ? vt->cols : 80;
    WARN_ON(vt->rows == 0 || vt->cols == 0);  /* vt_set_cursor called with zero terminal dimensions */
    if (row < 0)    row = 0;
    if (col < 0)    col = 0;
    if (row >= rows) row = rows - 1;
    if (col >= cols) col = cols - 1;
    vt->cur_row     = row;
    vt->cur_col     = col;
    vt->pending_wrap = 0;
    console_set_cursor_pos((uint32_t)row, (uint32_t)col);
}

/* =========================================================================
 * Save / restore cursor + SGR (DECSC/DECRC, SCOSC/SCORC)
 * ======================================================================== */

static void vt_save_state(struct vt_state *vt) {
    vt->saved_cur_row   = vt->cur_row;
    vt->saved_cur_col   = vt->cur_col;
    vt->saved_vga_fg    = vt->cur_vga_fg;
    vt->saved_vga_bg    = vt->cur_vga_bg;
    vt->saved_bold      = vt->bold;
    vt->saved_reverse   = vt->reverse;
    vt->saved_fg_is_rgb = vt->fg_is_rgb;
    vt->saved_bg_is_rgb = vt->bg_is_rgb;
    vt->saved_fg_rgb    = vt->fg_rgb;
    vt->saved_bg_rgb    = vt->bg_rgb;
    vt->saved_valid     = 1;
}

static void vt_restore_state(struct vt_state *vt) {
    BUG_ON(vt == NULL);
    if (!vt->saved_valid) return;
    vt_set_cursor(vt, vt->saved_cur_row, vt->saved_cur_col);
    vt->cur_vga_fg  = vt->saved_vga_fg;
    vt->cur_vga_bg  = vt->saved_vga_bg;
    vt->bold        = vt->saved_bold;
    vt->reverse     = vt->saved_reverse;
    vt->fg_is_rgb   = vt->saved_fg_is_rgb;
    vt->bg_is_rgb   = vt->saved_bg_is_rgb;
    vt->fg_rgb      = vt->saved_fg_rgb;
    vt->bg_rgb      = vt->saved_bg_rgb;
    vt_recompute_colors(vt);
}

/* =========================================================================
 * SGR (Select Graphic Rendition) parser
 * ======================================================================== */

static void vt_apply_sgr(struct vt_state *vt) {
    /* No params → SGR 0 (reset) */
    if (vt->nparam == 0 && !vt->have_digit) {
        vt->params[0] = 0;
        vt->nparam = 1;
    } else if (vt->have_digit) {
        if (vt->nparam < ANSI_MAX_PARAMS)
            vt->params[vt->nparam++] = vt->cur_param;
    }
    for (int i = 0; i < vt->nparam; i++) {
        int p = vt->params[i];
        if (p == 38 && (i + 2) < vt->nparam && vt->params[i + 1] == 5) {
            int n = vt->params[i + 2] & 0xFF;
            vt->cur_vga_fg = xterm256_to_vga(n);
            if (vt->bold && vt->cur_vga_fg < 8) vt->cur_vga_fg += 8;
            vt->fg_is_rgb = 0;
            i += 2;
            continue;
        }
        if (p == 48 && (i + 2) < vt->nparam && vt->params[i + 1] == 5) {
            int n = vt->params[i + 2] & 0xFF;
            vt->cur_vga_bg = xterm256_to_vga(n);
            vt->bg_is_rgb = 0;
            i += 2;
            continue;
        }
        if (p == 38 && (i + 4) < vt->nparam && vt->params[i + 1] == 2) {
            uint32_t r = (uint32_t)(vt->params[i + 2] & 0xFF);
            uint32_t g = (uint32_t)(vt->params[i + 3] & 0xFF);
            uint32_t b = (uint32_t)(vt->params[i + 4] & 0xFF);
            vt->fg_rgb    = (r << 16) | (g << 8) | b;
            vt->fg_is_rgb = 1;
            i += 4;
            continue;
        }
        if (p == 48 && (i + 4) < vt->nparam && vt->params[i + 1] == 2) {
            uint32_t r = (uint32_t)(vt->params[i + 2] & 0xFF);
            uint32_t g = (uint32_t)(vt->params[i + 3] & 0xFF);
            uint32_t b = (uint32_t)(vt->params[i + 4] & 0xFF);
            vt->bg_rgb    = (r << 16) | (g << 8) | b;
            vt->bg_is_rgb = 1;
            i += 4;
            continue;
        }
        if (p == 0) {
            vt->cur_vga_fg = 15;
            vt->cur_vga_bg = 0;
            vt->bold       = 0;
            vt->reverse    = 0;
            vt->fg_is_rgb  = 0;
            vt->bg_is_rgb  = 0;
        } else if (p == 1) {
            vt->bold = 1;
            if (!vt->fg_is_rgb && vt->cur_vga_fg < 8) vt->cur_vga_fg += 8;
        } else if (p == 22) {
            int was_bold = vt->bold;
            vt->bold = 0;
            if (was_bold && !vt->fg_is_rgb && vt->cur_vga_fg >= 9 && vt->cur_vga_fg <= 14)
                vt->cur_vga_fg -= 8;
        } else if (p >= 30 && p <= 37) {
            vt->cur_vga_fg = ansi_to_vga_fg[p - 30];
            if (vt->bold) vt->cur_vga_fg += 8;
            vt->fg_is_rgb = 0;
        } else if (p == 39) {
            vt->cur_vga_fg = 15;
            vt->fg_is_rgb  = 0;
        } else if (p >= 40 && p <= 47) {
            vt->cur_vga_bg = ansi_to_vga_fg[p - 40];
            vt->bg_is_rgb  = 0;
        } else if (p == 49) {
            vt->cur_vga_bg = 0;
            vt->bg_is_rgb  = 0;
        } else if (p >= 90 && p <= 97) {
            vt->cur_vga_fg = ansi_to_vga_fg[p - 90] + 8;
            vt->fg_is_rgb  = 0;
        } else if (p >= 100 && p <= 107) {
            vt->cur_vga_bg = ansi_to_vga_fg[p - 100] + 8;
            vt->bg_is_rgb  = 0;
        } else if (p == 7) {
            vt->reverse = 1;
        } else if (p == 27) {
            vt->reverse = 0;
        }
        /* Other SGR codes silently ignored */
    }
    vt_recompute_colors(vt);
}

/* =========================================================================
 * Cell buffer helpers
 * ======================================================================== */

/* Fill cells in row, columns [col_start..col_end] with a blank cell
 * using the current resolved background color.                          */
static void vt_cells_fill_row_range(struct vt_state *vt, int row,
                                    int col_start, int col_end) {
    if (row < 0 || row >= VT_MAX_ROWS) return;
    if (col_start < 0) col_start = 0;
    if (col_end >= VT_MAX_COLS) col_end = VT_MAX_COLS - 1;
    vt_cell_t blank;
    blank.ch     = ' ';
    blank._pad[0] = blank._pad[1] = blank._pad[2] = 0;
    blank.fg_rgb = vt->resolved_fg_rgb;
    blank.bg_rgb = vt->resolved_bg_rgb;
    for (int c = col_start; c <= col_end; c++)
        vt->cells[row * VT_MAX_COLS + c] = blank;
    vt->dirty[row] = 1;
}

/* Scroll cell rows [top..bot] UP by n: shift content, blank bottom rows. */
static void vt_cells_scroll_up(struct vt_state *vt, int n, int top, int bot) {
    if (n <= 0) return;
    int cols = vt->cols > 0 ? vt->cols : 80;
    int move = bot - top - n + 1;
    if (move > 0) {
        for (int r = top; r < top + move && r < VT_MAX_ROWS; r++) {
            int src = r + n;
            if (src > bot || src >= VT_MAX_ROWS) break;
            mm_memcpy(&vt->cells[r       * VT_MAX_COLS],
                      &vt->cells[src     * VT_MAX_COLS],
                      (uint32_t)(cols * (int)sizeof(vt_cell_t)));
            vt->dirty[r] = 1;
        }
    }
    for (int r = bot - n + 1; r <= bot && r < VT_MAX_ROWS; r++)
        vt_cells_fill_row_range(vt, r, 0, cols - 1);
}

/* Scroll cell rows [top..bot] DOWN by n: shift content, blank top rows. */
static void vt_cells_scroll_down(struct vt_state *vt, int n, int top, int bot) {
    if (n <= 0) return;
    int cols = vt->cols > 0 ? vt->cols : 80;
    int move = bot - top - n + 1;
    if (move > 0) {
        for (int r = bot; r >= top + n && r >= 0; r--) {
            int src = r - n;
            if (src < top || r >= VT_MAX_ROWS) continue;
            mm_memcpy(&vt->cells[r   * VT_MAX_COLS],
                      &vt->cells[src * VT_MAX_COLS],
                      (uint32_t)(cols * (int)sizeof(vt_cell_t)));
            vt->dirty[r] = 1;
        }
    }
    for (int r = top; r < top + n && r <= bot && r < VT_MAX_ROWS; r++)
        vt_cells_fill_row_range(vt, r, 0, cols - 1);
}

/* IL — Insert Lines: push current row and below down, blank n rows from cursor. */
static void vt_cells_insert_lines(struct vt_state *vt, int n, int top __attribute__((unused)), int bot) {
    /* cursor row acts as new top for the insert */
    vt_cells_scroll_down(vt, n, vt->cur_row, bot);
}

/* DL — Delete Lines: remove n lines at cursor, scroll remainder up. */
static void vt_cells_delete_lines(struct vt_state *vt, int n, int top __attribute__((unused)), int bot) {
    vt_cells_scroll_up(vt, n, vt->cur_row, bot);
}

/* ICH — Insert Characters: shift cells right from cursor col. */
static void vt_cells_insert_chars(struct vt_state *vt, int n) {
    int row  = vt->cur_row;
    int cols = vt->cols > 0 ? vt->cols : 80;
    if (row < 0 || row >= VT_MAX_ROWS) return;
    int start = vt->cur_col;
    int end   = cols - 1;
    int move  = end - start - n + 1;
    if (move > 0) {
        vt_memmove(&vt->cells[row * VT_MAX_COLS + start + n],
                   &vt->cells[row * VT_MAX_COLS + start],
                   (uint32_t)(move * (int)sizeof(vt_cell_t)));
    }
    vt_cells_fill_row_range(vt, row, start, start + n - 1);
}

/* DCH — Delete Characters: shift cells left at cursor, blank tail. */
static void vt_cells_delete_chars(struct vt_state *vt, int n) {
    int row  = vt->cur_row;
    int cols = vt->cols > 0 ? vt->cols : 80;
    if (row < 0 || row >= VT_MAX_ROWS) return;
    int start = vt->cur_col;
    int move  = cols - start - n;
    if (move > 0) {
        vt_memmove(&vt->cells[row * VT_MAX_COLS + start],
                   &vt->cells[row * VT_MAX_COLS + start + n],
                   (uint32_t)(move * (int)sizeof(vt_cell_t)));
    }
    vt_cells_fill_row_range(vt, row, cols - n, cols - 1);
}

/* ECH — Erase Characters: overwrite n cells from cursor with blanks. */
static void vt_cells_erase_chars(struct vt_state *vt, int n) {
    vt_cells_fill_row_range(vt, vt->cur_row, vt->cur_col, vt->cur_col + n - 1);
}

/* Erase display (ED): mode 0=below, 1=above, 2=all, 3=all+scrollback. */
static void vt_cells_erase_display(struct vt_state *vt, int mode) {
    int rows = vt->rows > 0 ? vt->rows : 25;
    int cols = vt->cols > 0 ? vt->cols : 80;
    int row  = vt->cur_row;
    int col  = vt->cur_col;
    if (mode == 0) {
        vt_cells_fill_row_range(vt, row, col, cols - 1);
        for (int r = row + 1; r < rows && r < VT_MAX_ROWS; r++)
            vt_cells_fill_row_range(vt, r, 0, cols - 1);
    } else if (mode == 1) {
        for (int r = 0; r < row && r < VT_MAX_ROWS; r++)
            vt_cells_fill_row_range(vt, r, 0, cols - 1);
        vt_cells_fill_row_range(vt, row, 0, col);
    } else {
        /* mode 2 and 3: clear all */
        for (int r = 0; r < rows && r < VT_MAX_ROWS; r++)
            vt_cells_fill_row_range(vt, r, 0, cols - 1);
    }
}

/* Erase line (EL): mode 0=to end, 1=to start, 2=whole line. */
static void vt_cells_erase_line(struct vt_state *vt, int mode) {
    int cols = vt->cols > 0 ? vt->cols : 80;
    int row  = vt->cur_row;
    int col  = vt->cur_col;
    if (mode == 0)
        vt_cells_fill_row_range(vt, row, col, cols - 1);
    else if (mode == 1)
        vt_cells_fill_row_range(vt, row, 0, col);
    else
        vt_cells_fill_row_range(vt, row, 0, cols - 1);
}

/* =========================================================================
 * vt_put_char — write a character, updating cell buffer + console.c
 *
 * Tracks cursor position in vt_state so no console_get_cursor_pos() call
 * is needed.  Handles LF implicit scroll, deferred xenl wrap, CR, BS, TAB
 * to keep the cell buffer in sync with what console.c renders.
 * ======================================================================== */
static void vt_put_char(struct vt_state *vt, char c) {
    unsigned char ch = (unsigned char)c;
    int rows = vt->rows > 0 ? vt->rows : 25;
    int cols = vt->cols > 0 ? vt->cols : 80;
    int bot  = (vt->scroll_bot >= 0) ? vt->scroll_bot : rows - 1;
    int top  = (vt->scroll_top >= 0) ? vt->scroll_top : 0;

    /* ---- LF / VT / FF: line feed ----------------------------------- */
    if (ch == 0x0A || ch == 0x0B || ch == 0x0C) {
        /* Pending wrap from previous write at last column: wrap first. */
        if (vt->pending_wrap) {
            vt->pending_wrap = 0;
            vt->cur_col = 0;
            if (vt->cur_row < bot)
                vt->cur_row++;
            else
                vt_cells_scroll_up(vt, 1, top, bot);
        }
        /* LF: advance row, scroll if at scroll-region bottom. */
        if (vt->cur_row >= bot)
            vt_cells_scroll_up(vt, 1, top, bot);
        else
            vt->cur_row++;
        console_putchar_batch(c);
        return;
    }

    /* ---- CR: carriage return --------------------------------------- */
    if (ch == 0x0D) {
        vt->pending_wrap = 0;
        vt->cur_col = 0;
        console_putchar_batch(c);
        return;
    }

    /* ---- BS: non-destructive backspace ---------------------------- */
    if (ch == 0x08) {
        vt->pending_wrap = 0;
        if (vt->cur_col > 0)
            vt->cur_col--;
        else if (vt->cur_row > 0) {
            vt->cur_row--;
            vt->cur_col = cols - 1;
        }
        console_putchar_batch(c);
        return;
    }

    /* ---- TAB ------------------------------------------------------ */
    if (ch == 0x09) {
        vt->pending_wrap = 0;
        int spaces = ((vt->cur_col + 8) & ~7) - vt->cur_col;
        for (int i = 0; i < spaces; i++) {
            if (vt->cur_col >= cols) break;
            if (vt->cur_row < VT_MAX_ROWS && vt->cur_col < VT_MAX_COLS) {
                vt_cell_t blank;
                blank.ch = ' ';
                blank._pad[0] = blank._pad[1] = blank._pad[2] = 0;
                blank.fg_rgb = vt->resolved_fg_rgb;
                blank.bg_rgb = vt->resolved_bg_rgb;
                vt->cells[vt->cur_row * VT_MAX_COLS + vt->cur_col] = blank;
                vt->dirty[vt->cur_row] = 1;
            }
            vt->cur_col++;
        }
        if (vt->cur_col >= cols) vt->cur_col = cols - 1;
        console_putchar_batch(c);
        return;
    }

    /* ---- Other C0 controls (BEL, SI, SO, etc.): pass through ------ */
    if (ch < 0x20) {
        console_putchar_batch(c);
        return;
    }

    /* ---- Printable character -------------------------------------- */

    /* Handle deferred xenl wrap: previous write was at last column.
     * console.c triggers the wrap on the next printable; mirror it. */
    if (vt->pending_wrap) {
        vt->pending_wrap = 0;
        vt->cur_col = 0;
        if (vt->cur_row < bot)
            vt->cur_row++;
        else
            vt_cells_scroll_up(vt, 1, top, bot);
    }

    /* Write cell into backing store. */
    if (vt->cur_row < VT_MAX_ROWS && vt->cur_col < VT_MAX_COLS) {
        vt_cell_t cell;
        cell.ch = ch;
        cell._pad[0] = cell._pad[1] = cell._pad[2] = 0;
        cell.fg_rgb = vt->resolved_fg_rgb;
        cell.bg_rgb = vt->resolved_bg_rgb;
        vt->cells[vt->cur_row * VT_MAX_COLS + vt->cur_col] = cell;
        vt->dirty[vt->cur_row] = 1;
    }

    console_putchar_batch(c);

    /* Advance cursor; set pending_wrap if we hit the last column. */
    if (vt->cur_col + 1 >= cols)
        vt->pending_wrap = 1;
    else
        vt->cur_col++;
}

/* =========================================================================
 * vt_flush_dirty — re-render dirty rows from the cell buffer.
 *
 * Used for alt-screen exit (restore main screen) and resize.
 * Iterates every dirty row, sets cursor + color per cell, calls
 * console_putchar_batch.  Runs inside console_batch_begin/end for speed.
 * ======================================================================== */
void vt_flush_dirty(struct vt_state *vt) {
    int rows = vt->rows > 0 ? vt->rows : 25;
    int cols = vt->cols > 0 ? vt->cols : 80;
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;

    console_batch_begin();
    for (int r = 0; r < rows; r++) {
        if (!vt->dirty[r]) continue;
        for (int c = 0; c < cols; c++) {
            vt_cell_t *cell = &vt->cells[r * VT_MAX_COLS + c];
            console_set_cursor_pos((uint32_t)r, (uint32_t)c);
            console_set_color_rgb(cell->fg_rgb, cell->bg_rgb);
            console_putchar_batch((char)cell->ch);
        }
        vt->dirty[r] = 0;
    }
    console_batch_end();

    /* Restore active color so subsequent output uses the right colors. */
    vt_recompute_colors(vt);
}

/* Redraw ALL rows from cell buffer regardless of dirty flags.
 * Used on alt-screen exit to restore the entire main screen.           */
__attribute__((unused))
static void vt_redraw_all_from_cells(struct vt_state *vt) {
    int rows = vt->rows > 0 ? vt->rows : 25;
    int cols = vt->cols > 0 ? vt->cols : 80;
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;
    for (int r = 0; r < rows; r++) vt->dirty[r] = 1;
    vt_flush_dirty(vt);
}

/* =========================================================================
 * vt_process_char — full ANSI/VT escape sequence dispatcher
 *
 * This is the tty->output function that processes each byte of terminal
 * output.  It is the heart of the VT layer, converted from the original
 * tty_output_console() in tty.c.  All g_ansi_* globals → vt->* fields.
 * ======================================================================== */
void vt_process_char(struct vt_state *vt, char c) {
    unsigned char ch = (unsigned char)c;

    /* VT global rules: ESC immediately aborts any in-progress sequence;
     * CAN/SUB cancel silently (except inside OSC/DCS where ESC may be
     * the start of the ST terminator).                                  */
    if (ch == 0x1B) {
        if (vt->state == ANSI_OSC || vt->state == ANSI_DCS) {
            /* handled inside those states */
        } else {
            vt_reset_parser(vt);
            vt->state = ANSI_ESC;
            return;
        }
    } else if ((ch == 0x18 || ch == 0x1A) && vt->state != ANSI_NORMAL) {
        vt_reset_parser(vt);
        return;
    }

    switch (vt->state) {

    /* -------------------------------------------------------------- */
    case ANSI_NORMAL:
        if (ch == 0x1B) { vt->state = ANSI_ESC; return; }
        vt_put_char(vt, c);
        return;

    /* -------------------------------------------------------------- */
    case ANSI_ESC:
        switch (ch) {
        case '[':
            vt->state      = ANSI_CSI;
            vt->nparam     = 0;
            vt->cur_param  = 0;
            vt->have_digit = 0;
            vt->private_mode = 0;
            vt->intermediate = 0;
            return;
        case ']':
            vt->state = ANSI_OSC;
            return;
        case 'P': case 'X': case '^': case '_':
            vt->state = ANSI_DCS;
            return;
        case '(': case ')': case '*': case '+':
            vt->state = ANSI_CHARSET;
            return;
        case '#':
            vt->state = ANSI_HASH;
            return;
        case '7':
            vt_save_state(vt);
            vt_reset_parser(vt);
            return;
        case '8':
            vt_restore_state(vt);
            vt_reset_parser(vt);
            return;
        case 'D': {
            /* IND — Index: cursor down, scroll at region bottom. */
            int bot = (vt->scroll_bot >= 0) ? vt->scroll_bot
                                            : (vt->rows > 0 ? vt->rows - 1 : 24);
            int top = (vt->scroll_top >= 0) ? vt->scroll_top : 0;
            if (vt->cur_row == bot) {
                vt_cells_scroll_up(vt, 1, top, bot);
                console_scroll_region_up(1, top, bot);
            } else {
                vt_set_cursor(vt, vt->cur_row + 1, vt->cur_col);
            }
            vt_reset_parser(vt);
            return;
        }
        case 'M': {
            /* RI — Reverse Index: cursor up, scroll at region top. */
            int bot = (vt->scroll_bot >= 0) ? vt->scroll_bot
                                            : (vt->rows > 0 ? vt->rows - 1 : 24);
            int top = (vt->scroll_top >= 0) ? vt->scroll_top : 0;
            if (vt->cur_row == top) {
                vt_cells_scroll_down(vt, 1, top, bot);
                console_scroll_region_down(1, top, bot);
            } else if (vt->cur_row > 0) {
                vt_set_cursor(vt, vt->cur_row - 1, vt->cur_col);
            }
            vt_reset_parser(vt);
            return;
        }
        case 'E': {
            /* NEL — Next Line: CR + LF. */
            int bot = (vt->scroll_bot >= 0) ? vt->scroll_bot
                                            : (vt->rows > 0 ? vt->rows - 1 : 24);
            int top = (vt->scroll_top >= 0) ? vt->scroll_top : 0;
            if (vt->cur_row == bot) {
                vt_cells_scroll_up(vt, 1, top, bot);
                console_scroll_region_up(1, top, bot);
                vt_set_cursor(vt, bot, 0);
            } else {
                vt_set_cursor(vt, vt->cur_row + 1, 0);
            }
            vt_reset_parser(vt);
            return;
        }
        case 'c':
            /* RIS — Reset to Initial State. */
            console_set_color(15, 0);
            vt->cur_vga_fg  = 15;
            vt->cur_vga_bg  = 0;
            vt->bold        = 0;
            vt->reverse     = 0;
            vt->fg_is_rgb   = 0;
            vt->bg_is_rgb   = 0;
            vt->scroll_top  = -1;
            vt->scroll_bot  = -1;
            vt->saved_valid = 0;
            console_set_scroll_region(-1, -1);
            vt_cells_erase_display(vt, 2);
            console_erase_display(2);
            vt_set_cursor(vt, 0, 0);
            vt_recompute_colors(vt);
            vt_reset_parser(vt);
            return;
        case '\\':
            vt_reset_parser(vt);
            return;
        case '=': case '>':
            vt_reset_parser(vt);
            return;
        default:
            vt_reset_parser(vt);
            return;
        }

    /* -------------------------------------------------------------- */
    case ANSI_CHARSET:
    case ANSI_HASH:
        vt_reset_parser(vt);
        return;

    /* -------------------------------------------------------------- */
    case ANSI_OSC:
        if (ch == 0x07) { vt_reset_parser(vt); return; }
        if (ch == 0x1B) { vt->state = ANSI_OSC_ESC; return; }
        return;

    case ANSI_OSC_ESC:
        if (ch == '\\') vt_reset_parser(vt);
        else            vt->state = ANSI_OSC;
        return;

    /* -------------------------------------------------------------- */
    case ANSI_DCS:
        if (ch == 0x07) { vt_reset_parser(vt); return; }
        if (ch == 0x1B) { vt->state = ANSI_DCS_ESC; return; }
        return;

    case ANSI_DCS_ESC:
        if (ch == '\\') vt_reset_parser(vt);
        else            vt->state = ANSI_DCS;
        return;

    /* -------------------------------------------------------------- */
    case ANSI_CSI:
        /* C0 controls inside CSI are executed in place (ANSI spec).   */
        if (ch == 0x08 || ch == 0x09 || ch == 0x0A || ch == 0x0B ||
            ch == 0x0C || ch == 0x0D || ch == 0x07) {
            /* Pass to vt_put_char so control chars update cursor state. */
            vt_put_char(vt, c);
            return;
        }
        if (ch == '?' && !vt->have_digit && vt->nparam == 0) {
            vt->private_mode  = 1;
            vt->intermediate  = '?';
            return;
        }
        if ((ch == '>' || ch == '=' || ch == '<')
            && !vt->have_digit && vt->nparam == 0) {
            vt->intermediate = ch;
            return;
        }
        /* ECMA-48 intermediate bytes 0x20..0x2F */
        if (ch >= 0x20 && ch <= 0x2F) {
            vt->intermediate = (char)ch;
            return;
        }
        if (ch >= '0' && ch <= '9') {
            vt->cur_param  = vt->cur_param * 10 + (ch - '0');
            vt->have_digit = 1;
            return;
        }
        if (ch == ';') {
            if (vt->nparam < ANSI_MAX_PARAMS)
                vt->params[vt->nparam++] = vt->have_digit ? vt->cur_param : 0;
            vt->cur_param  = 0;
            vt->have_digit = 0;
            return;
        }

        /* SGR: flush and apply immediately */
        if (ch == 'm') {
            vt_apply_sgr(vt);
            vt_reset_parser(vt);
            return;
        }

        /* Flush last param if digits seen */
        if (vt->have_digit && vt->nparam < ANSI_MAX_PARAMS)
            vt->params[vt->nparam++] = vt->cur_param;

        #define P1_OR(def) ((vt->nparam >= 1 && vt->params[0] > 0) ? vt->params[0] : (def))

        switch (ch) {

        case 'H': case 'f': {
            /* CUP — Cursor Position (1-based). */
            int row = (vt->nparam >= 1 && vt->params[0] > 0) ? vt->params[0] - 1 : 0;
            int col = (vt->nparam >= 2 && vt->params[1] > 0) ? vt->params[1] - 1 : 0;
            vt_set_cursor(vt, row, col);
            break;
        }

        case 'J': {
            int mode = vt->nparam >= 1 ? vt->params[0] : 0;
            vt_cells_erase_display(vt, mode);
            console_erase_display(mode);
            break;
        }

        case 'K': {
            int mode = vt->nparam >= 1 ? vt->params[0] : 0;
            vt_cells_erase_line(vt, mode);
            console_erase_line(mode);
            break;
        }

        case 'A': {
            int n = P1_OR(1);
            int r = vt->cur_row - n;
            if (r < 0) r = 0;
            vt_set_cursor(vt, r, vt->cur_col);
            break;
        }

        case 'B': case 'e': {
            int n = P1_OR(1);
            vt_set_cursor(vt, vt->cur_row + n, vt->cur_col);
            break;
        }

        case 'C': case 'a': {
            int n = P1_OR(1);
            vt_set_cursor(vt, vt->cur_row, vt->cur_col + n);
            break;
        }

        case 'D': {
            int n = P1_OR(1);
            int c = vt->cur_col - n;
            if (c < 0) c = 0;
            vt_set_cursor(vt, vt->cur_row, c);
            break;
        }

        case 'E': {
            /* CNL — Cursor Next Line: down N, col 0. */
            vt_set_cursor(vt, vt->cur_row + P1_OR(1), 0);
            break;
        }

        case 'F': {
            /* CPL — Cursor Preceding Line: up N, col 0. */
            int r = vt->cur_row - P1_OR(1);
            if (r < 0) r = 0;
            vt_set_cursor(vt, r, 0);
            break;
        }

        case 'G': case '`': {
            /* CHA / HPA — Column absolute (1-based). */
            int col = P1_OR(1) - 1;
            if (col < 0) col = 0;
            vt_set_cursor(vt, vt->cur_row, col);
            break;
        }

        case 'd': {
            /* VPA — Row absolute (1-based). */
            int row = P1_OR(1) - 1;
            if (row < 0) row = 0;
            vt_set_cursor(vt, row, vt->cur_col);
            break;
        }

        case 'L': {
            /* IL — Insert Lines. */
            int n   = P1_OR(1);
            int bot = (vt->scroll_bot >= 0) ? vt->scroll_bot
                                            : (vt->rows > 0 ? vt->rows - 1 : 24);
            int top = (vt->scroll_top >= 0) ? vt->scroll_top : 0;
            vt_cells_insert_lines(vt, n, top, bot);
            console_insert_lines(n, top, bot);
            break;
        }

        case 'M': {
            /* DL — Delete Lines. */
            int n   = P1_OR(1);
            int bot = (vt->scroll_bot >= 0) ? vt->scroll_bot
                                            : (vt->rows > 0 ? vt->rows - 1 : 24);
            int top = (vt->scroll_top >= 0) ? vt->scroll_top : 0;
            vt_cells_delete_lines(vt, n, top, bot);
            console_delete_lines(n, top, bot);
            break;
        }

        case '@': {
            /* ICH — Insert Characters. */
            int n = P1_OR(1);
            vt_cells_insert_chars(vt, n);
            console_insert_chars(n);
            break;
        }

        case 'P': {
            /* DCH — Delete Characters. */
            int n = P1_OR(1);
            vt_cells_delete_chars(vt, n);
            console_delete_chars(n);
            break;
        }

        case 'X': {
            /* ECH — Erase Characters. */
            int n = P1_OR(1);
            vt_cells_erase_chars(vt, n);
            console_erase_chars(n);
            break;
        }

        case 'S': {
            /* SU — Scroll Up. */
            int n   = P1_OR(1);
            int bot = (vt->scroll_bot >= 0) ? vt->scroll_bot
                                            : (vt->rows > 0 ? vt->rows - 1 : 24);
            int top = (vt->scroll_top >= 0) ? vt->scroll_top : 0;
            vt_cells_scroll_up(vt, n, top, bot);
            console_scroll_region_up(n, top, bot);
            break;
        }

        case 'T': {
            /* SD — Scroll Down. */
            int n   = P1_OR(1);
            int bot = (vt->scroll_bot >= 0) ? vt->scroll_bot
                                            : (vt->rows > 0 ? vt->rows - 1 : 24);
            int top = (vt->scroll_top >= 0) ? vt->scroll_top : 0;
            vt_cells_scroll_down(vt, n, top, bot);
            console_scroll_region_down(n, top, bot);
            break;
        }

        case 'r': {
            /* DECSTBM — Set Top and Bottom Margins (1-based). */
            int rows2 = vt->rows > 0 ? vt->rows : 25;
            int top = (vt->nparam >= 1 && vt->params[0] > 0) ? vt->params[0] - 1 : 0;
            int bot = (vt->nparam >= 2 && vt->params[1] > 0) ? vt->params[1] - 1 : rows2 - 1;
            if (top < 0) top = 0;
            if (bot >= rows2) bot = rows2 - 1;
            if (top >= bot) { top = 0; bot = rows2 - 1; }
            vt->scroll_top = top;
            vt->scroll_bot = bot;
            if (top == 0 && bot == rows2 - 1)
                console_set_scroll_region(-1, -1);
            else
                console_set_scroll_region(top, bot);
            vt_set_cursor(vt, top, 0);
            break;
        }

        case 's': {
            /* SCOSC — Save Cursor. */
            if (vt->nparam == 0) vt_save_state(vt);
            break;
        }

        case 'u': {
            /* SCORC — Restore Cursor. */
            vt_restore_state(vt);
            break;
        }

        case 'n': {
            /* DSR — Device Status Report.
             * \e[5n → \e[0n   (terminal OK)
             * \e[6n → \e[row;colR  (cursor position, 1-based)
             * Reply is injected lock-free; we hold tty_lock via tty_write. */
            if (vt->intermediate == '?') break;
            int p = vt->nparam >= 1 ? vt->params[0] : 0;
            if (p == 5) {
                tty_enqueue_read_locked(vt->tty, 0x1B);
                tty_enqueue_read_locked(vt->tty, '[');
                tty_enqueue_read_locked(vt->tty, '0');
                tty_enqueue_read_locked(vt->tty, 'n');
                g_console_reply_pending = 1;
            } else if (p == 6) {
                char num[12]; int ln;
                tty_enqueue_read_locked(vt->tty, 0x1B);
                tty_enqueue_read_locked(vt->tty, '[');
                ln = itoa_simple(vt->cur_row + 1, num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(vt->tty, num[i]);
                tty_enqueue_read_locked(vt->tty, ';');
                ln = itoa_simple(vt->cur_col + 1, num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(vt->tty, num[i]);
                tty_enqueue_read_locked(vt->tty, 'R');
                g_console_reply_pending = 1;
            }
            break;
        }

        case 'c': {
            /* DA — Device Attributes. */
            if (vt->intermediate == '?') break;
            const char *reply = (vt->intermediate == '>')
                                ? "\033[>41;0;0c"
                                : "\033[?1;2c";
            for (const char *p = reply; *p; ++p)
                tty_enqueue_read_locked(vt->tty, *p);
            g_console_reply_pending = 1;
            break;
        }

        case 't': {
            /* XTWINOPS 18: report text-area size in characters. */
            int op = vt->nparam >= 1 ? vt->params[0] : 0;
            if (op == 18) {
                char num[12]; int ln;
                tty_enqueue_read_locked(vt->tty, 0x1B);
                tty_enqueue_read_locked(vt->tty, '[');
                tty_enqueue_read_locked(vt->tty, '8');
                tty_enqueue_read_locked(vt->tty, ';');
                ln = itoa_simple((int)vt->tty->winsz.ws_row, num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(vt->tty, num[i]);
                tty_enqueue_read_locked(vt->tty, ';');
                ln = itoa_simple((int)vt->tty->winsz.ws_col, num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(vt->tty, num[i]);
                tty_enqueue_read_locked(vt->tty, 't');
                g_console_reply_pending = 1;
            }
            break;
        }

        case 'h': case 'l': {
            int enable = (ch == 'h') ? 1 : 0;
            if (vt->private_mode) {
                int mode = vt->nparam >= 1 ? vt->params[0] : 0;
                if (mode == 25) {
                    if (enable) console_cursor_enable();
                    else        console_cursor_disable();
                } else if (mode == 1000) {
                    vt->tty->mouse_tracking = (uint8_t)enable;
                    if (!enable) {
                        vt->tty->mouse_btn_event = 0;
                        vt->tty->mouse_sgr_mode  = 0;
                    }
                } else if (mode == 1002) {
                    vt->tty->mouse_btn_event = (uint8_t)enable;
                    if (!enable) vt->tty->mouse_sgr_mode = 0;
                } else if (mode == 1006) {
                    vt->tty->mouse_sgr_mode = (uint8_t)enable;
                } else if (mode == 1047 || mode == 1049) {
                    /* Alternate screen buffer swap via cell buffers.
                     *
                     * Entry (h): snapshot save state, switch cells pointer
                     * to alt buffer (pre-cleared), clear display, home cursor.
                     *
                     * Exit  (l): switch cells pointer back to main buffer,
                     * restore the main screen by re-rendering from the cell
                     * buffer.  This replaces the scrollback-viewport hack
                     * (console_restore_alt_screen) with a proper cell redraw
                     * so the main screen is pixel-perfectly restored even when
                     * the scrollback was modified while in alt-screen.        */
                    if (enable) {
                        if (mode == 1049) {
                            vt_save_state(vt);
                            vt->alt_screen_sb_total = console_get_sb_total();
                        }
                        /* Switch to alt cell buffer; blank it. */
                        vt->cells = g_alt_cells;
                        vt->in_alt_screen = 1;
                        int rows2 = vt->rows > 0 ? vt->rows : 25;
                        int cols2 = vt->cols > 0 ? vt->cols : 80;
                        for (int r = 0; r < rows2 && r < VT_MAX_ROWS; r++)
                            vt_cells_fill_row_range(vt, r, 0, cols2 - 1);
                        console_erase_display(2);
                        vt_set_cursor(vt, 0, 0);
                    } else {
                        /* Switch back to main cell buffer, redraw. */
                        vt->cells = g_main_cells;
                        vt->in_alt_screen = 0;
                        /* Mark all main rows dirty so vt_redraw_all re-renders them. */
                        int rows2 = vt->rows > 0 ? vt->rows : 25;
                        for (int r = 0; r < rows2 && r < VT_MAX_ROWS; r++)
                            vt->dirty[r] = 1;
                        vt_flush_dirty(vt);
                        if (mode == 1049) {
                            if (vt->saved_valid) {
                                vt_restore_state(vt);
                            } else {
                                vt_set_cursor(vt, 0, 0);
                            }
                        } else {
                            vt_set_cursor(vt, 0, 0);
                        }
                    }
                } else if (mode == 1048) {
                    if (enable) vt_save_state(vt);
                    else        vt_restore_state(vt);
                }
                /* Other DEC private modes silently ignored. */
            }
            /* Non-private ANSI modes silently ignored. */
            break;
        }

        default:
            /* All other CSI sequences silently consumed. */
            break;
        }

        #undef P1_OR
        vt_reset_parser(vt);
        return;
    }

    /* Fallback: if we somehow end up here, reset and render. */
    vt_reset_parser(vt);
    vt_put_char(vt, c);
}

/* =========================================================================
 * Public API
 * ======================================================================== */

void vt_init(struct vt_state *vt, int cols, int rows, tty_t *tty) {
    mm_memset(vt, 0, sizeof(*vt));
    vt->cols        = cols;
    vt->rows        = rows;
    vt->tty         = tty;
    vt->cur_vga_fg  = 15;   /* white */
    vt->cur_vga_bg  = 0;    /* black */
    vt->scroll_top  = -1;
    vt->scroll_bot  = -1;
    vt->cells       = g_main_cells;
    vt->dirty       = g_dirty_rows;

    /* Sync cursor with whatever the console driver has now (handles the
     * kernel boot output that happened before the VT layer started).   */
    uint32_t r = 0, c = 0;
    console_get_cursor_pos(&r, &c);
    vt->cur_row = (int)r;
    vt->cur_col = (int)c;

    /* Resolved colors default: white on black. */
    vt->resolved_fg_rgb = vga_to_rgb(15);
    vt->resolved_bg_rgb = vga_to_rgb(0);

    mm_memset(g_main_cells, 0, sizeof(g_main_cells));
    mm_memset(g_alt_cells,  0, sizeof(g_alt_cells));
    mm_memset(g_dirty_rows, 0, sizeof(g_dirty_rows));
}

void vt_resize(struct vt_state *vt, int cols, int rows) {
    vt->cols = cols;
    vt->rows = rows;
    /* Clamp cursor to new size. */
    if (vt->cur_row >= rows) vt->cur_row = rows - 1;
    if (vt->cur_col >= cols) vt->cur_col = cols - 1;
    /* After resize, mark all rows dirty so they can be redrawn. */
    for (int r = 0; r < rows && r < VT_MAX_ROWS; r++)
        vt->dirty[r] = 1;
}

void vt_reset(struct vt_state *vt) {
    /* Soft reset: restore SGR defaults, scroll region, alt-screen. */
    if (vt->in_alt_screen) {
        vt->cells = g_main_cells;
        vt->in_alt_screen = 0;
    }
    vt->cur_vga_fg  = 15;
    vt->cur_vga_bg  = 0;
    vt->bold        = 0;
    vt->reverse     = 0;
    vt->fg_is_rgb   = 0;
    vt->bg_is_rgb   = 0;
    vt->scroll_top  = -1;
    vt->scroll_bot  = -1;
    vt->saved_valid = 0;
    vt->pending_wrap = 0;
    vt_reset_parser(vt);
    vt_recompute_colors(vt);
    console_set_scroll_region(-1, -1);
}

/* tty->output function pointer target.  Dispatches to the global
 * console VT instance.  Signature matches void (*output)(tty_t*, char). */
void vt_output_char(tty_t *tty, char c) {
    (void)tty;  /* back-link already stored in g_console_vt.tty */
    vt_process_char(&g_console_vt, c);
}

int vt_consume_reply_pending(void) {
    if (g_console_reply_pending) {
        g_console_reply_pending = 0;
        return 1;
    }
    return 0;
}
