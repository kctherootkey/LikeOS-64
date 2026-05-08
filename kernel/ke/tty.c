// LikeOS-64 TTY/PTY subsystem
#include "../../include/kernel/tty.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/usb_serial.h"
#include "../../include/kernel/signal.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/net.h"

#define TTY_MAX_PTYS 16

// Spinlock for TTY buffer access
static spinlock_t tty_lock = SPINLOCK_INIT("tty");

// Security: Validate user pointer is in user space
static bool tty_validate_user_ptr(uint64_t ptr, size_t len) {
    if (ptr < 0x10000) return false;  // Reject low addresses (NULL deref protection)
    if (ptr >= 0x7FFFFFFFFFFF) return false;  // Beyond user space
    if (ptr + len < ptr) return false;  // Overflow check
    return true;
}

// Security: SMAP-aware copy from kernel to user
static int tty_copy_to_user(void* user_dst, const void* kernel_src, size_t len) {
    if (!tty_validate_user_ptr((uint64_t)user_dst, len)) {
        return -EFAULT;
    }
    smap_disable();
    mm_memcpy(user_dst, kernel_src, len);
    smap_enable();
    return 0;
}

// Security: SMAP-aware copy from user to kernel
static int tty_copy_from_user(void* kernel_dst, const void* user_src, size_t len) {
    if (!tty_validate_user_ptr((uint64_t)user_src, len)) {
        return -EFAULT;
    }
    smap_disable();
    mm_memcpy(kernel_dst, user_src, len);
    smap_enable();
    return 0;
}

/* PTY master ring buffer.
 *
 * The slave-side process writes here through tty_write -> tty->output.
 * The master-side reader (e.g. tmux) drains it through tty_pty_master_read.
 *
 * Sizing: must accommodate one full screen redraw of a full-screen app
 * (top, less, nano) plus enough headroom for bursts like `dmesg` or
 * `ps aux` (which write tens of KiB in a tight loop).  64 KiB is
 * comfortable for any reasonable terminal size; the master reader
 * drains at IRQ-rate so it almost never fills.
 *
 * On overflow we drop trailing bytes (non-blocking).  We do NOT block
 * the slave writer on a wait queue — a slave that gets killed while
 * blocked there would be reaped while still linked into the queue,
 * and the next wake would walk freed memory.  With 64 KiB and no SMP
 * race on the indices, drops are vanishingly rare for normal apps.
 *
 * Locking: 'lock' protects master_buf/m_head/m_tail/m_count and
 * slave_open.  Acquired with IRQs disabled because tty_pty_master_read
 * can be called from syscall context on one CPU while a slave write
 * runs on another.
 */
#define PTY_MASTER_BUF_SIZE 262144u
typedef struct pty {
    int id;
    tty_t slave;
    char master_buf[PTY_MASTER_BUF_SIZE];
    uint32_t m_head;
    uint32_t m_tail;
    uint32_t m_count;
    spinlock_t lock;
    task_t* master_read_waiters;
    int master_open;
    int slave_open;
} pty_t;

static tty_t g_console_tty;
static pty_t g_ptys[TTY_MAX_PTYS];

/* Wake all tasks parked on a wait queue identified by 'waiters'.
 *
 * The pointer value (e.g. &tty->read_waiters or &pty->master_read_waiters)
 * is used purely as a stable channel identifier — we no longer maintain a
 * linked list rooted at *waiters.  Instead, sched_wake_channel walks the
 * global task list (under g_task_list_lock) and wakes every task whose
 * wait_channel matches this address.
 *
 * This avoids a use-after-free that the old linked-list design suffered
 * from: when a task blocked here was killed (e.g. SIGKILL of nano from a
 * second tmux pane), sched_remove_task freed the task struct without
 * unlinking it from any custom wait queues, and the next wake walked
 * t->wait_next through freed memory and page-faulted in tty_wake_readers.
 *
 * 'waiters' itself is no longer touched (kept as a parameter only so
 * existing call sites need no change).
 */
static void tty_wake_readers(task_t** waiters) {
    sched_wake_channel((void*)waiters);
}

static void tty_enqueue_read(tty_t* tty, char c) {
    uint64_t flags;
    spin_lock_irqsave(&tty_lock, &flags);

    if (tty->read_count >= sizeof(tty->read_buf)) {
        spin_unlock_irqrestore(&tty_lock, flags);
        return;
    }
    tty->read_buf[tty->read_tail] = c;
    tty->read_tail = (tty->read_tail + 1) % sizeof(tty->read_buf);
    tty->read_count++;

    spin_unlock_irqrestore(&tty_lock, flags);
}

/* Lock-free variant: caller MUST already hold tty_lock.  Used from the
 * ANSI parser's DSR/DA reply injection, which runs under tty_write's
 * lock and would otherwise self-deadlock the same CPU. */
static void tty_enqueue_read_locked(tty_t* tty, char c) {
    if (tty->read_count >= sizeof(tty->read_buf)) return;
    tty->read_buf[tty->read_tail] = c;
    tty->read_tail = (tty->read_tail + 1) % sizeof(tty->read_buf);
    tty->read_count++;
}

/* Set by the ANSI parser when it injects a query reply (DSR/DA/XTWINOPS)
 * into the read buffer.  tty_write checks this *after* releasing tty_lock
 * and wakes blocked readers then \u2014 we must not call into the scheduler
 * while holding tty_lock with IRQs off. */
static volatile int g_console_reply_pending = 0;

static int tty_dequeue_read(tty_t* tty, char* out) {
    uint64_t flags;
    spin_lock_irqsave(&tty_lock, &flags);

    if (tty->read_count == 0) {
        spin_unlock_irqrestore(&tty_lock, flags);
        return 0;
    }
    *out = tty->read_buf[tty->read_head];
    tty->read_head = (tty->read_head + 1) % sizeof(tty->read_buf);
    tty->read_count--;

    spin_unlock_irqrestore(&tty_lock, flags);
    return 1;
}

/* ======================================================================
 * ANSI CSI (Control Sequence Introducer) escape sequence parser
 * Handles ESC [ Pn ; Pn ... m  (SGR – Select Graphic Rendition)
 * Also silently consumes other CSI sequences (cursor movement, etc.)
 * ====================================================================== */
enum ansi_state {
    ANSI_NORMAL = 0,
    ANSI_ESC,         /* saw ESC (0x1B) */
    ANSI_CSI,         /* saw ESC [ */
    ANSI_OSC,         /* saw ESC ] — collecting until ST or BEL */
    ANSI_OSC_ESC,     /* saw ESC inside OSC, waiting for '\\' (ST) */
    ANSI_DCS,         /* saw ESC P / X / ^ / _ — eat until ST */
    ANSI_DCS_ESC,     /* saw ESC inside DCS, waiting for '\\' */
    ANSI_CHARSET,     /* saw ESC ( ) * + — designate G0/G1/G2/G3, eat 1 byte */
    ANSI_HASH,        /* saw ESC #  — eat 1 byte (DECALN etc.) */
};

static enum ansi_state g_ansi_state = ANSI_NORMAL;
#define ANSI_MAX_PARAMS 16
static int  g_ansi_params[ANSI_MAX_PARAMS];
static int  g_ansi_nparam = 0;
static int  g_ansi_cur_param = 0;       /* accumulating current number */
static int  g_ansi_have_digit = 0;      /* have we seen a digit for cur param? */
/* CSI intermediate byte (>, =, !, SP, $, ?).  We must remember it because
 * '?' marks DEC private modes, and '>' selects DA2/XTVERSION variants.
 * Without this, e.g. \e[>c (DA2) was being treated as \e[c (DA1) which
 * gives tmux the wrong terminal class and breaks redraw. */
static char g_ansi_intermediate = 0;

/* Forward decls used by DSR/DA reply path. */
static void tty_inject_string(tty_t* tty, const char* s);
static int  itoa_simple(int val, char* buf);

/* Saved cursor / SGR state for DECSC (ESC 7) / SCOSC (CSI s).
 * tmux relies on this to safely paint the status bar in a different bg color
 * and then revert; if we don't restore SGR, the bg-color "leaks" and the
 * implicit BCE on the next region scroll fills the new bottom row in that
 * leaked color (which produced the "green blank line above status bar"). */
static int g_saved_cur_row = 0;
static int g_saved_cur_col = 0;
static int g_saved_valid   = 0;
static uint8_t  g_saved_vga_fg     = 15;
static uint8_t  g_saved_vga_bg     = 0;
static int      g_saved_bold       = 0;
static int      g_saved_reverse    = 0;
static int      g_saved_fg_is_rgb  = 0;
static int      g_saved_bg_is_rgb  = 0;
static uint32_t g_saved_fg_rgb     = 0xFFFFFFFFu;
static uint32_t g_saved_bg_rgb     = 0x00000000u;

/* Scroll region (DECSTBM).  0..max_rows-1 inclusive; -1 means "full screen". */
static int g_scroll_top = -1;
static int g_scroll_bot = -1;

/* Scrollback line count at the point when the alternate screen was entered
 * (mode 1049h).  Used to restore the pre-alt-screen viewport on 1049l. */
static uint32_t g_alt_screen_sb_total = 0;

/* Map ANSI SGR color index (0-7) to VGA palette index */
static uint8_t ansi_to_vga_fg[8] = {
    0,   /* 0 = black   → VGA 0  */
    4,   /* 1 = red     → VGA 4  */
    2,   /* 2 = green   → VGA 2  */
    6,   /* 3 = yellow  → VGA 6 (brown) */
    1,   /* 4 = blue    → VGA 1  */
    5,   /* 5 = magenta → VGA 5  */
    3,   /* 6 = cyan    → VGA 3  */
    7,   /* 7 = white   → VGA 7 (light grey) */
};

static uint8_t g_cur_vga_fg = 15;  /* VGA_COLOR_WHITE */
static uint8_t g_cur_vga_bg = 0;   /* VGA_COLOR_BLACK */
static int     g_ansi_bold    = 0;
static int     g_ansi_reverse = 0;
static int     g_ansi_private = 0; /* DEC private mode: saw '?' after CSI */

static void ansi_reset_state(void) {
    g_ansi_state = ANSI_NORMAL;
    g_ansi_nparam = 0;
    g_ansi_cur_param = 0;
    g_ansi_have_digit = 0;
    g_ansi_private = 0;
    g_ansi_intermediate = 0;
}

/* True-color SGR support.  Tracks whether the current fg/bg is an explicit
 * 24-bit RGB value rather than a VGA palette index, so we can re-issue the
 * exact color (and so reverse-video swaps still work). */
static int      g_fg_is_rgb = 0;
static int      g_bg_is_rgb = 0;
static uint32_t g_fg_rgb    = 0xFFFFFFFFu;
static uint32_t g_bg_rgb    = 0x00000000u;

/* Approximate a 256-color xterm index to the closest 16-color VGA palette
 * entry.  Indices 0..15 map via the existing table.  16..231 form a 6x6x6
 * RGB cube; 232..255 are a 24-step grayscale ramp. */
static uint8_t xterm256_to_vga(int n) {
    if (n < 0)  n = 0;
    if (n > 255) n = 255;
    if (n < 8)  return ansi_to_vga_fg[n];
    if (n < 16) return ansi_to_vga_fg[n - 8] + 8;
    if (n >= 232) {
        /* Grayscale ramp: 0=black .. 23=white. */
        int g = n - 232;
        if (g < 4)  return 0;       /* VGA black           */
        if (g < 10) return 8;       /* VGA dark grey       */
        if (g < 18) return 7;       /* VGA light grey      */
        return 15;                  /* VGA white           */
    }
    /* 6x6x6 cube: index = 16 + 36*r + 6*g + b, each 0..5. */
    int idx = n - 16;
    int r = idx / 36;
    int g = (idx / 6) % 6;
    int b = idx % 6;
    int bright = (r > 2 || g > 2 || b > 2) ? 8 : 0;
    uint8_t base;
    if (r == g && g == b) {
        if (r == 0)        base = 0;     /* black */
        else if (r <= 2)   { base = 0; bright = 8; }  /* dark grey  */
        else if (r <= 4)   { base = 7; bright = 0; }  /* light grey */
        else               { base = 7; bright = 8; }  /* white      */
    } else if (r >= g && r >= b)         base = ansi_to_vga_fg[1]; /* red */
    else if (g >= r && g >= b)           base = ansi_to_vga_fg[2]; /* green */
    else                                 base = ansi_to_vga_fg[4]; /* blue */
    /* Mix-cases — pick by dominant pair. */
    if (r >= 3 && g >= 3 && b < 3)       base = ansi_to_vga_fg[3]; /* yellow */
    else if (r >= 3 && b >= 3 && g < 3)  base = ansi_to_vga_fg[5]; /* magenta */
    else if (g >= 3 && b >= 3 && r < 3)  base = ansi_to_vga_fg[6]; /* cyan */
    return base + (bright ? 8 : 0);
}

/* Forward decl: needed by save/restore helpers and by SGR apply path. */
extern uint32_t vga_to_rgb(uint8_t v);   /* internal helper in console.c */

/* Re-emit the active SGR state as concrete fg/bg pixel colors so that all
 * subsequent draw_char and BCE-style fills (region scroll, line erase, etc.)
 * use the right colors.  This is the single point of truth that bridges
 * tmux/ncurses-level SGR state to the framebuffer console's bg_color. */
static void ansi_recompute_colors(void) {
    uint32_t fg_rgb = g_fg_is_rgb ? g_fg_rgb : vga_to_rgb(g_cur_vga_fg);
    uint32_t bg_rgb = g_bg_is_rgb ? g_bg_rgb : vga_to_rgb(g_cur_vga_bg);
    if (g_ansi_reverse) {
        uint32_t t = fg_rgb; fg_rgb = bg_rgb; bg_rgb = t;
    }
    console_set_color_rgb(fg_rgb, bg_rgb);
}

/* Snapshot/restore the *complete* SGR state (and cursor position) for
 * DECSC/DECRC and SCOSC/SCORC.  Real terminals save: cursor pos, SGR,
 * charset, origin-mode, wrap.  We save what we actually track. */
static void ansi_save_state(void) {
    uint32_t r, c2;
    console_get_cursor_pos(&r, &c2);
    g_saved_cur_row   = (int)r;
    g_saved_cur_col   = (int)c2;
    g_saved_vga_fg    = g_cur_vga_fg;
    g_saved_vga_bg    = g_cur_vga_bg;
    g_saved_bold      = g_ansi_bold;
    g_saved_reverse   = g_ansi_reverse;
    g_saved_fg_is_rgb = g_fg_is_rgb;
    g_saved_bg_is_rgb = g_bg_is_rgb;
    g_saved_fg_rgb    = g_fg_rgb;
    g_saved_bg_rgb    = g_bg_rgb;
    g_saved_valid     = 1;
}

static void ansi_restore_state(void) {
    if (!g_saved_valid) return;
    console_set_cursor_pos((uint32_t)g_saved_cur_row, (uint32_t)g_saved_cur_col);
    g_cur_vga_fg   = g_saved_vga_fg;
    g_cur_vga_bg   = g_saved_vga_bg;
    g_ansi_bold    = g_saved_bold;
    g_ansi_reverse = g_saved_reverse;
    g_fg_is_rgb    = g_saved_fg_is_rgb;
    g_bg_is_rgb    = g_saved_bg_is_rgb;
    g_fg_rgb       = g_saved_fg_rgb;
    g_bg_rgb       = g_saved_bg_rgb;
    ansi_recompute_colors();
}

static void ansi_apply_sgr(void) {
    /* If no params, treat as SGR 0 (reset) */
    if (g_ansi_nparam == 0 && !g_ansi_have_digit) {
        g_ansi_params[0] = 0;
        g_ansi_nparam = 1;
    } else if (g_ansi_have_digit) {
        /* flush last accumulated param */
        if (g_ansi_nparam < ANSI_MAX_PARAMS)
            g_ansi_params[g_ansi_nparam++] = g_ansi_cur_param;
    }
    for (int i = 0; i < g_ansi_nparam; i++) {
        int p = g_ansi_params[i];
        if (p == 38 && (i + 2) < g_ansi_nparam && g_ansi_params[i + 1] == 5) {
            /* 256-color foreground: 38;5;N */
            int n = g_ansi_params[i + 2] & 0xFF;
            g_cur_vga_fg = xterm256_to_vga(n);
            if (g_ansi_bold && g_cur_vga_fg < 8) g_cur_vga_fg += 8;
            g_fg_is_rgb = 0;
            i += 2;
            continue;
        }
        if (p == 48 && (i + 2) < g_ansi_nparam && g_ansi_params[i + 1] == 5) {
            /* 256-color background: 48;5;N */
            int n = g_ansi_params[i + 2] & 0xFF;
            g_cur_vga_bg = xterm256_to_vga(n);
            g_bg_is_rgb = 0;
            i += 2;
            continue;
        }
        if (p == 38 && (i + 4) < g_ansi_nparam && g_ansi_params[i + 1] == 2) {
            /* True-color foreground: 38;2;R;G;B */
            uint32_t r = (uint32_t)(g_ansi_params[i + 2] & 0xFF);
            uint32_t g = (uint32_t)(g_ansi_params[i + 3] & 0xFF);
            uint32_t b = (uint32_t)(g_ansi_params[i + 4] & 0xFF);
            g_fg_rgb = (r << 16) | (g << 8) | b;
            g_fg_is_rgb = 1;
            i += 4;
            continue;
        }
        if (p == 48 && (i + 4) < g_ansi_nparam && g_ansi_params[i + 1] == 2) {
            /* True-color background: 48;2;R;G;B */
            uint32_t r = (uint32_t)(g_ansi_params[i + 2] & 0xFF);
            uint32_t g = (uint32_t)(g_ansi_params[i + 3] & 0xFF);
            uint32_t b = (uint32_t)(g_ansi_params[i + 4] & 0xFF);
            g_bg_rgb = (r << 16) | (g << 8) | b;
            g_bg_is_rgb = 1;
            i += 4;
            continue;
        }
        if (p == 0) {
            /* Reset */
            g_cur_vga_fg = 15; /* white */
            g_cur_vga_bg = 0;  /* black */
            g_ansi_bold = 0;
            g_ansi_reverse = 0;
            g_fg_is_rgb = 0;
            g_bg_is_rgb = 0;
        } else if (p == 1) {
            /* Bold / bright */
            g_ansi_bold = 1;
            /* If we already have a color set, make it bright */
            if (!g_fg_is_rgb && g_cur_vga_fg < 8)
                g_cur_vga_fg += 8;
        } else if (p == 22) {
            /* Normal intensity — but the terminal's default foreground
             * is white (15) on our framebuffer, so dropping a previously-
             * unmodified default-fg from 15 to 7 (light grey) is wrong.
             * Only down-shift bright palette entries that were explicitly
             * set via 30-37 + bold, leaving the "default white" intact. */
            int was_bold = g_ansi_bold;
            g_ansi_bold = 0;
            if (was_bold && !g_fg_is_rgb && g_cur_vga_fg >= 9 && g_cur_vga_fg <= 14)
                g_cur_vga_fg -= 8;
        } else if (p >= 30 && p <= 37) {
            /* Foreground color */
            g_cur_vga_fg = ansi_to_vga_fg[p - 30];
            if (g_ansi_bold) g_cur_vga_fg += 8;
            g_fg_is_rgb = 0;
        } else if (p == 39) {
            /* Default foreground — terminal default is white (15). */
            g_cur_vga_fg = 15;
            g_fg_is_rgb = 0;
        } else if (p >= 40 && p <= 47) {
            /* Background color */
            g_cur_vga_bg = ansi_to_vga_fg[p - 40];
            g_bg_is_rgb = 0;
        } else if (p == 49) {
            /* Default background */
            g_cur_vga_bg = 0;
            g_bg_is_rgb = 0;
        } else if (p >= 90 && p <= 97) {
            /* Bright foreground */
            g_cur_vga_fg = ansi_to_vga_fg[p - 90] + 8;
            g_fg_is_rgb = 0;
        } else if (p >= 100 && p <= 107) {
            /* Bright background */
            g_cur_vga_bg = ansi_to_vga_fg[p - 100] + 8;
            g_bg_is_rgb = 0;
        } else if (p == 7) {
            /* Reverse video */
            g_ansi_reverse = 1;
        } else if (p == 27) {
            /* Reverse video off */
            g_ansi_reverse = 0;
        }
        /* Other SGR codes silently ignored */
    }
    ansi_recompute_colors();
}

static void tty_output_console(tty_t* tty, char c) {
    /* tty is used by DSR/DA reply injectors below. */
    unsigned char ch = (unsigned char)c;

    /* VT-spec global rules that apply in *every* parser state:
     *  - 0x1B (ESC) immediately aborts any in-progress sequence and starts
     *    a new one.  Without this, back-to-back sequences like \e[2J\e[H
     *    cause the second ESC to be eaten by the first sequence's
     *    dispatcher, then the following '[' renders as the printable
     *    character '['.  This is the source of the visible "[K", "[0m",
     *    "[0]" leaking into rendered output.
     *  - 0x18 (CAN) and 0x1A (SUB) cancel the current sequence with no
     *    further action.  They must NOT be drawn.
     * (OSC/DCS payloads are exempt: an ESC there might be the start of
     * the ESC-\\ ST terminator; we keep their existing handling.) */
    if (ch == 0x1B) {
        if (g_ansi_state == ANSI_OSC || g_ansi_state == ANSI_DCS) {
            /* Could be ST (ESC \\) — handled in those states. */
        } else {
            ansi_reset_state();
            g_ansi_state = ANSI_ESC;
            return;
        }
    } else if ((ch == 0x18 || ch == 0x1A) &&
               g_ansi_state != ANSI_NORMAL) {
        ansi_reset_state();
        return;
    }

    switch (g_ansi_state) {
    case ANSI_NORMAL:
        if (ch == 0x1B) {
            g_ansi_state = ANSI_ESC;
            return;
        }
        console_putchar_batch(c);
        return;

    case ANSI_ESC:
        switch (ch) {
        case '[':
            g_ansi_state = ANSI_CSI;
            g_ansi_nparam = 0;
            g_ansi_cur_param = 0;
            g_ansi_have_digit = 0;
            g_ansi_private = 0;
            return;
        case ']':
            g_ansi_state = ANSI_OSC;
            return;
        case 'P': case 'X': case '^': case '_':
            /* DCS / SOS / PM / APC — consume until ST. */
            g_ansi_state = ANSI_DCS;
            return;
        case '(': case ')': case '*': case '+':
            /* Designate character set — next byte selects the set; ignore. */
            g_ansi_state = ANSI_CHARSET;
            return;
        case '#':
            /* DECALN etc. — eat one more byte. */
            g_ansi_state = ANSI_HASH;
            return;
        case '7':
            /* DECSC — save cursor + SGR + attributes. */
            ansi_save_state();
            ansi_reset_state();
            return;
        case '8':
            /* DECRC — restore cursor + SGR + attributes. */
            ansi_restore_state();
            ansi_reset_state();
            return;
        case 'D': {
            /* IND — Index: cursor down, scroll region up at bottom. */
            uint32_t r, col;
            console_get_cursor_pos(&r, &col);
            int bot = (g_scroll_bot >= 0) ? g_scroll_bot
                                          : (int)g_console_tty.winsz.ws_row - 1;
            int top = (g_scroll_top >= 0) ? g_scroll_top : 0;
            if ((int)r == bot) {
                console_scroll_region_up(1, top, bot);
            } else {
                console_set_cursor_pos(r + 1, col);
            }
            ansi_reset_state();
            return;
        }
        case 'M': {
            /* RI — Reverse Index: cursor up, scroll region down at top. */
            uint32_t r, col;
            console_get_cursor_pos(&r, &col);
            int top = (g_scroll_top >= 0) ? g_scroll_top : 0;
            int bot = (g_scroll_bot >= 0) ? g_scroll_bot
                                          : (int)g_console_tty.winsz.ws_row - 1;
            if ((int)r == top) {
                console_scroll_region_down(1, top, bot);
            } else if (r > 0) {
                console_set_cursor_pos(r - 1, col);
            }
            ansi_reset_state();
            return;
        }
        case 'E': {
            /* NEL — Next Line: CR + LF. */
            uint32_t r, col;
            console_get_cursor_pos(&r, &col);
            int bot = (g_scroll_bot >= 0) ? g_scroll_bot
                                          : (int)g_console_tty.winsz.ws_row - 1;
            int top = (g_scroll_top >= 0) ? g_scroll_top : 0;
            if ((int)r == bot) {
                console_scroll_region_up(1, top, bot);
                console_set_cursor_pos((uint32_t)bot, 0);
            } else {
                console_set_cursor_pos(r + 1, 0);
            }
            ansi_reset_state();
            return;
        }
        case 'c':
            /* RIS — Reset to Initial State.  Soft reset: clear screen,
             * home cursor, drop scroll region, clear saved state. */
            console_set_color(15, 0);
            g_cur_vga_fg = 15;
            g_cur_vga_bg = 0;
            g_ansi_bold = 0;
            g_ansi_reverse = 0;
            g_fg_is_rgb = 0;
            g_bg_is_rgb = 0;
            g_scroll_top = -1;
            g_scroll_bot = -1;
            console_set_scroll_region(-1, -1);
            g_saved_valid = 0;
            console_erase_display(2);
            console_set_cursor_pos(0, 0);
            ansi_reset_state();
            return;
        case '\\':
            /* Stray ST outside any string — just ignore. */
            ansi_reset_state();
            return;
        case '=': case '>':
            /* DECPAM / DECPNM — keypad mode; ignore. */
            ansi_reset_state();
            return;
        default:
            /* Unknown two-byte ESC sequence — drop quietly. */
            ansi_reset_state();
            return;
        }

    case ANSI_CHARSET:
    case ANSI_HASH:
        /* Eat one byte and return to normal. */
        ansi_reset_state();
        return;

    case ANSI_OSC:
        if (ch == 0x07) {            /* BEL terminates OSC */
            ansi_reset_state();
            return;
        }
        if (ch == 0x1B) {            /* possible ESC \ (ST) */
            g_ansi_state = ANSI_OSC_ESC;
            return;
        }
        /* Discard payload byte. */
        return;

    case ANSI_OSC_ESC:
        /* Either '\\' completes ST, or any other byte continues OSC. */
        if (ch == '\\') {
            ansi_reset_state();
        } else {
            /* Not a real ST — go back to collecting OSC bytes. */
            g_ansi_state = ANSI_OSC;
        }
        return;

    case ANSI_DCS:
        if (ch == 0x07) {
            ansi_reset_state();
            return;
        }
        if (ch == 0x1B) {
            g_ansi_state = ANSI_DCS_ESC;
            return;
        }
        return;

    case ANSI_DCS_ESC:
        if (ch == '\\') {
            ansi_reset_state();
        } else {
            g_ansi_state = ANSI_DCS;
        }
        return;

    case ANSI_CSI:
        /* VT spec: C0 controls (BS, HT, LF, VT, FF, CR, BEL, SO, SI)
         * arriving inside a CSI sequence are EXECUTED IN PLACE; the
         * sequence continues.  Without this, programs that use CR/LF
         * inside long status-line repaints ended up aborting the CSI
         * via the default dispatcher and leaking the rest as text. */
        if (ch == 0x08 || ch == 0x09 || ch == 0x0A || ch == 0x0B ||
            ch == 0x0C || ch == 0x0D || ch == 0x07) {
            console_putchar_batch((char)ch);
            return;
        }
        if (ch == '?' && !g_ansi_have_digit && g_ansi_nparam == 0) {
            /* DEC private mode indicator: ESC [ ? ... */
            g_ansi_private = 1;
            g_ansi_intermediate = '?';
            return;
        }
        /* Track other private/private-leading intermediate indicators
         * (>, =, <) at the start of the parameter list. */
        if ((ch == '>' || ch == '=' || ch == '<')
            && !g_ansi_have_digit && g_ansi_nparam == 0) {
            g_ansi_intermediate = ch;
            return;
        }
        /* VT spec intermediate bytes (0x20\u20130x2F: SP ! " # $ % & ' ( ) * + , - . /).
         * Per ECMA-48 these may appear AFTER parameters and must be
         * collected; the sequence only ends on a final byte (0x40\u20130x7E).
         * Without this, e.g. ESC[ q (set cursor style) and ESC[!p
         * (DECSTR) leaked the trailing letter as visible text. */
        if (ch >= 0x20 && ch <= 0x2F) {
            /* Remember the last intermediate; we don't dispatch them
             * individually but we must not abort the sequence. */
            g_ansi_intermediate = (char)ch;
            return;
        }
        if (ch >= '0' && ch <= '9') {
            g_ansi_cur_param = g_ansi_cur_param * 10 + (ch - '0');
            g_ansi_have_digit = 1;
            return;
        }
        if (ch == ';') {
            if (g_ansi_nparam < ANSI_MAX_PARAMS) {
                g_ansi_params[g_ansi_nparam++] = g_ansi_have_digit ? g_ansi_cur_param : 0;
            }
            g_ansi_cur_param = 0;
            g_ansi_have_digit = 0;
            return;
        }
        /* Final byte — the command character */
        if (ch == 'm') {
            ansi_apply_sgr();
            ansi_reset_state();
            return;
        }

        /* Flush last param if we have digits */
        if (g_ansi_have_digit && g_ansi_nparam < ANSI_MAX_PARAMS) {
            g_ansi_params[g_ansi_nparam++] = g_ansi_cur_param;
        }

        /* Convenience: first param defaulting to 1 (most movement commands). */
        #define P1_OR(def) ((g_ansi_nparam >= 1 && g_ansi_params[0] > 0) ? g_ansi_params[0] : (def))

        switch (ch) {
        case 'H': case 'f': {
            /* CUP — Cursor Position: ESC [ row ; col H  (1-based). */
            int row = (g_ansi_nparam >= 1 && g_ansi_params[0] > 0) ? g_ansi_params[0] - 1 : 0;
            int col = (g_ansi_nparam >= 2 && g_ansi_params[1] > 0) ? g_ansi_params[1] - 1 : 0;
            console_set_cursor_pos((uint32_t)row, (uint32_t)col);
            break;
        }
        case 'J': {
            int mode = (g_ansi_nparam >= 1) ? g_ansi_params[0] : 0;
            console_erase_display(mode);
            break;
        }
        case 'K': {
            int mode = (g_ansi_nparam >= 1) ? g_ansi_params[0] : 0;
            console_erase_line(mode);
            break;
        }
        case 'A': {
            int n = P1_OR(1);
            uint32_t row, col;
            console_get_cursor_pos(&row, &col);
            row = (row >= (uint32_t)n) ? row - (uint32_t)n : 0;
            console_set_cursor_pos(row, col);
            break;
        }
        case 'B': case 'e': {
            int n = P1_OR(1);
            uint32_t row, col;
            console_get_cursor_pos(&row, &col);
            console_set_cursor_pos(row + (uint32_t)n, col);
            break;
        }
        case 'C': case 'a': {
            int n = P1_OR(1);
            uint32_t row, col;
            console_get_cursor_pos(&row, &col);
            console_set_cursor_pos(row, col + (uint32_t)n);
            break;
        }
        case 'D': {
            int n = P1_OR(1);
            uint32_t row, col;
            console_get_cursor_pos(&row, &col);
            col = (col >= (uint32_t)n) ? col - (uint32_t)n : 0;
            console_set_cursor_pos(row, col);
            break;
        }
        case 'E': {
            /* CNL — Cursor Next Line: down N, col 0. */
            int n = P1_OR(1);
            uint32_t row, col;
            console_get_cursor_pos(&row, &col);
            console_set_cursor_pos(row + (uint32_t)n, 0);
            break;
        }
        case 'F': {
            /* CPL — Cursor Preceding Line: up N, col 0. */
            int n = P1_OR(1);
            uint32_t row, col;
            console_get_cursor_pos(&row, &col);
            row = (row >= (uint32_t)n) ? row - (uint32_t)n : 0;
            console_set_cursor_pos(row, 0);
            break;
        }
        case 'G': case '`': {
            /* CHA / HPA — Cursor Horizontal Absolute (1-based column). */
            int col = P1_OR(1) - 1;
            if (col < 0) col = 0;
            uint32_t row, c2;
            console_get_cursor_pos(&row, &c2);
            console_set_cursor_pos(row, (uint32_t)col);
            break;
        }
        case 'd': {
            /* VPA — Vertical Position Absolute (1-based row). */
            int row = P1_OR(1) - 1;
            if (row < 0) row = 0;
            uint32_t r, col;
            console_get_cursor_pos(&r, &col);
            console_set_cursor_pos((uint32_t)row, col);
            break;
        }
        case 'L': {
            /* IL — Insert Lines (within scroll region). */
            int n = P1_OR(1);
            int top = (g_scroll_top >= 0) ? g_scroll_top : 0;
            int bot = (g_scroll_bot >= 0) ? g_scroll_bot
                                          : (int)g_console_tty.winsz.ws_row - 1;
            console_insert_lines(n, top, bot);
            break;
        }
        case 'M': {
            /* DL — Delete Lines (within scroll region). */
            int n = P1_OR(1);
            int top = (g_scroll_top >= 0) ? g_scroll_top : 0;
            int bot = (g_scroll_bot >= 0) ? g_scroll_bot
                                          : (int)g_console_tty.winsz.ws_row - 1;
            console_delete_lines(n, top, bot);
            break;
        }
        case '@': {
            /* ICH — Insert Characters. */
            console_insert_chars(P1_OR(1));
            break;
        }
        case 'P': {
            /* DCH — Delete Characters. */
            console_delete_chars(P1_OR(1));
            break;
        }
        case 'X': {
            /* ECH — Erase Characters. */
            console_erase_chars(P1_OR(1));
            break;
        }
        case 'S': {
            /* SU — Scroll Up within region. */
            int n = P1_OR(1);
            int top = (g_scroll_top >= 0) ? g_scroll_top : 0;
            int bot = (g_scroll_bot >= 0) ? g_scroll_bot
                                          : (int)g_console_tty.winsz.ws_row - 1;
            console_scroll_region_up(n, top, bot);
            break;
        }
        case 'T': {
            /* SD — Scroll Down within region. */
            int n = P1_OR(1);
            int top = (g_scroll_top >= 0) ? g_scroll_top : 0;
            int bot = (g_scroll_bot >= 0) ? g_scroll_bot
                                          : (int)g_console_tty.winsz.ws_row - 1;
            console_scroll_region_down(n, top, bot);
            break;
        }
        case 'r': {
            /* DECSTBM — Set Top and Bottom Margins (1-based, inclusive). */
            int rows = (int)g_console_tty.winsz.ws_row;
            if (rows <= 0) rows = 25;
            int top = (g_ansi_nparam >= 1 && g_ansi_params[0] > 0) ? g_ansi_params[0] - 1 : 0;
            int bot = (g_ansi_nparam >= 2 && g_ansi_params[1] > 0) ? g_ansi_params[1] - 1 : rows - 1;
            if (top < 0) top = 0;
            if (bot >= rows) bot = rows - 1;
            if (top >= bot) { top = 0; bot = rows - 1; }
            g_scroll_top = top;
            g_scroll_bot = bot;
            /* Make the framebuffer console honor the region for implicit
             * scrolls from \\n and from line wrap, so pinned status rows
             * below the region are preserved. */
            if (top == 0 && bot == rows - 1) {
                console_set_scroll_region(-1, -1);
            } else {
                console_set_scroll_region(top, bot);
            }
            /* DECSTBM also homes the cursor to (top, 0) — or (1,1) of origin mode. */
            console_set_cursor_pos((uint32_t)top, 0);
            break;
        }
        case 's': {
            /* SCOSC — Save Cursor Position (only if no params; with params
             * this would be DECSLRM which we don't support). */
            if (g_ansi_nparam == 0) {
                ansi_save_state();
            }
            break;
        }
        case 'u': {
            /* SCORC — Restore Cursor Position (and SGR). */
            ansi_restore_state();
            break;
        }
        case 'n': {
            /* DSR — Device Status Report.
             *   \e[5n  → \e[0n  (terminal OK)
             *   \e[6n  → \e[<row>;<col>R  (cursor pos, 1-based)
             * Reply is enqueued lock-free because we are called under
             * tty_lock from tty_write — see tty_enqueue_read_locked. */
            if (g_ansi_intermediate == '?') break;
            int p = (g_ansi_nparam >= 1) ? g_ansi_params[0] : 0;
            if (p == 5) {
                tty_enqueue_read_locked(tty, 0x1B);
                tty_enqueue_read_locked(tty, '[');
                tty_enqueue_read_locked(tty, '0');
                tty_enqueue_read_locked(tty, 'n');
                g_console_reply_pending = 1;
            } else if (p == 6) {
                uint32_t r, c2;
                console_get_cursor_pos(&r, &c2);
                char num[12]; int ln;
                tty_enqueue_read_locked(tty, 0x1B);
                tty_enqueue_read_locked(tty, '[');
                ln = itoa_simple((int)(r + 1), num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(tty, num[i]);
                tty_enqueue_read_locked(tty, ';');
                ln = itoa_simple((int)(c2 + 1), num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(tty, num[i]);
                tty_enqueue_read_locked(tty, 'R');
                g_console_reply_pending = 1;
            }
            break;
        }
        case 'c': {
            /* DA — Device Attributes.
             *   \e[c    → DA1: \e[?1;2c  (VT100 with advanced video)
             *   \e[>c   → DA2: \e[>41;0;0c  (xterm-class) */
            if (g_ansi_intermediate == '?') break;
            const char* reply = (g_ansi_intermediate == '>')
                                ? "\033[>41;0;0c"
                                : "\033[?1;2c";
            for (const char* p = reply; *p; ++p) {
                tty_enqueue_read_locked(tty, *p);
            }
            g_console_reply_pending = 1;
            break;
        }
        case 't': {
            /* XTWINOPS 18: report text-area size in characters. */
            int op = (g_ansi_nparam >= 1) ? g_ansi_params[0] : 0;
            if (op == 18) {
                char num[12]; int ln;
                tty_enqueue_read_locked(tty, 0x1B);
                tty_enqueue_read_locked(tty, '[');
                tty_enqueue_read_locked(tty, '8');
                tty_enqueue_read_locked(tty, ';');
                ln = itoa_simple((int)g_console_tty.winsz.ws_row, num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(tty, num[i]);
                tty_enqueue_read_locked(tty, ';');
                ln = itoa_simple((int)g_console_tty.winsz.ws_col, num);
                for (int i = 0; i < ln; i++) tty_enqueue_read_locked(tty, num[i]);
                tty_enqueue_read_locked(tty, 't');
                g_console_reply_pending = 1;
            }
            break;
        }
        case 'h': case 'l': {
            int enable = (ch == 'h') ? 1 : 0;
            if (g_ansi_private) {
                int mode = (g_ansi_nparam >= 1) ? g_ansi_params[0] : 0;
                if (mode == 25) {
                    if (enable) console_cursor_enable();
                    else        console_cursor_disable();
                } else if (mode == 1000) {
                    g_console_tty.mouse_tracking = enable;
                    if (!enable) {
                        g_console_tty.mouse_btn_event = 0;
                        g_console_tty.mouse_sgr_mode = 0;
                    }
                } else if (mode == 1002) {
                    g_console_tty.mouse_btn_event = enable;
                    if (!enable)
                        g_console_tty.mouse_sgr_mode = 0;
                } else if (mode == 1006) {
                    g_console_tty.mouse_sgr_mode = enable;
                } else if (mode == 1047 || mode == 1049) {
                    /* Alternate screen buffer.  We have no separate cell buffer,
                     * so we emulate with scrollback viewport save/restore.
                     * On entry: snapshot the scrollback position, clear the
                     * screen, and home the cursor.
                     * On exit: restore the viewport to the pre-entry content. */
                    if (enable) {
                        if (mode == 1049) {
                            ansi_save_state();
                            g_alt_screen_sb_total = console_get_sb_total();
                        }
                        console_erase_display(2);
                        console_set_cursor_pos(0, 0);
                    } else {
                        /* Restore scrollback viewport to show pre-alt-screen
                         * content (console_restore_alt_screen clears + redraws
                         * the framebuffer from the saved viewport position). */
                        if (mode == 1049) {
                            console_restore_alt_screen(g_alt_screen_sb_total);
                            if (g_saved_valid) {
                                ansi_restore_state();
                            } else {
                                console_set_cursor_pos(0, 0);
                            }
                        } else {
                            console_erase_display(2);
                            console_set_cursor_pos(0, 0);
                        }
                    }
                } else if (mode == 1048) {
                    if (enable) {
                        ansi_save_state();
                    } else {
                        ansi_restore_state();
                    }
                }
                /* Other DEC private modes silently ignored. */
            }
            /* Non-private h/l (ANSI modes) silently ignored. */
            break;
        }
        default:
            /* All other CSI sequences silently consumed (DSR n, DA c, etc.). */
            break;
        }
        #undef P1_OR
        ansi_reset_state();
        return;
    }

    /* Fallback */
    ansi_reset_state();
    console_putchar_batch(c);
}

static pty_t* tty_get_pty(int id) {
    if (id < 0 || id >= TTY_MAX_PTYS) {
        return NULL;
    }
    if (g_ptys[id].id != id) {
        return NULL;
    }
    return &g_ptys[id];
}

/* Bulk non-blocking enqueue into the master ring.  Returns bytes
 * actually copied (may be less than len if the ring fills).  Wakes any
 * blocked master reader if at least one byte was accepted. */
static long pty_master_enqueue_bulk(pty_t* pty, const char* buf, long len) {
    if (!pty || !buf || len <= 0) return 0;
    uint64_t flags;
    spin_lock_irqsave(&pty->lock, &flags);
    uint32_t free = PTY_MASTER_BUF_SIZE - pty->m_count;
    uint32_t to_copy = (uint32_t)len;
    int dropped = 0;
    if (to_copy > free) { to_copy = free; dropped = 1; }
    for (uint32_t i = 0; i < to_copy; i++) {
        pty->master_buf[pty->m_tail] = buf[i];
        pty->m_tail = (pty->m_tail + 1) % PTY_MASTER_BUF_SIZE;
    }
    pty->m_count += to_copy;
    spin_unlock_irqrestore(&pty->lock, flags);
    if (to_copy > 0) {
        tty_wake_readers(&pty->master_read_waiters);
    }
    return (long)to_copy;
}

/* Single-byte non-blocking enqueue.  Used by the ECHO path inside
 * tty_input_char (master-side context: keystroke arriving from the
 * master's tty_pty_master_write).  Drops on full — ECHO is one byte at
 * a time so loss is benign. */
static void pty_master_enqueue(pty_t* pty, char c) {
    if (!pty) {
        return;
    }
    pty_master_enqueue_bulk(pty, &c, 1);
}

static void tty_output_pty_slave(tty_t* tty, char c) {
    if (!tty || !tty->priv) {
        return;
    }
    pty_t* pty = (pty_t*)tty->priv;
    pty_master_enqueue(pty, c);
}

static void tty_set_default_termios(tty_t* tty) {
    tty->term.c_iflag = ICRNL;
    /* Output post-processing on by default: translate bare LF to CR+LF.
     * Without this, programs like ls/ps that emit only '\n' between
     * lines render the next line starting at the previous column,
     * producing the "run-on" appearance.  Apps that need raw output
     * (tmux master, full-screen apps) clear OPOST via tcsetattr. */
    tty->term.c_oflag = OPOST | ONLCR;
    tty->term.c_cflag = 0;
    tty->term.c_lflag = ISIG | ICANON | ECHO;
    tty->term.c_cc[VINTR] = 3;   // Ctrl+C
    tty->term.c_cc[VQUIT] = 28;  // Ctrl+\
    // Use volatile to prevent compiler from optimizing away the write
    volatile cc_t* verase_ptr = &tty->term.c_cc[VERASE];
    *verase_ptr = 8;  // Backspace (ASCII 8)
    tty->term.c_cc[VKILL] = 21;  // Ctrl+U
    tty->term.c_cc[VEOF] = 4;    // Ctrl+D
    tty->term.c_cc[VSTART] = 17; // Ctrl+Q
    tty->term.c_cc[VSTOP] = 19;  // Ctrl+S
    tty->term.c_cc[VSUSP] = 26;  // Ctrl+Z
    tty->term.c_cc[VMIN] = 1;    // Block until at least 1 byte available
    tty->term.c_cc[VTIME] = 0;   // No timeout
}

void tty_init(void) {
    mm_memset(&g_console_tty, 0, sizeof(g_console_tty));
    g_console_tty.id = 1;  // 1-based so 0 means "no tty"
    g_console_tty.is_pty = 0;
    g_console_tty.is_master = 0;
    g_console_tty.fg_pgid = 0;
    g_console_tty.output = tty_output_console;

    /* Query actual console dimensions from the framebuffer driver */
    uint32_t rows = 25, cols = 80;
    console_get_dimensions(&rows, &cols);
    g_console_tty.winsz.ws_row = (unsigned short)rows;
    g_console_tty.winsz.ws_col = (unsigned short)cols;
    tty_set_default_termios(&g_console_tty);

    for (int i = 0; i < TTY_MAX_PTYS; ++i) {
        mm_memset(&g_ptys[i], 0, sizeof(pty_t));
        g_ptys[i].id = -1;
        spinlock_init(&g_ptys[i].lock, "pty");
    }
}

tty_t* tty_get_console(void) {
    return &g_console_tty;
}

void tty_reset_termios(tty_t* tty) {
    if (!tty) {
        return;
    }
    tty_set_default_termios(tty);
    tty->canon_len = 0;
    tty->read_head = 0;
    tty->read_tail = 0;
    tty->read_count = 0;
    tty->eof_pending = 0;
}

static void tty_signal_pgrp(tty_t* tty, int sig) {
    if (!tty || tty->fg_pgid == 0) {
        return;
    }
    sched_signal_pgrp(tty->fg_pgid, sig);
    // Wake any blocked readers so they can see they've been signaled/killed
    tty_wake_readers(&tty->read_waiters);
}

void tty_input_char_raw(tty_t* tty, char c) {
    if (!tty) return;
    tty_enqueue_read(tty, c);
    tty_wake_readers(&tty->read_waiters);
}

/* Helper: inject a string into the TTY read buffer (raw, no line discipline) */
static void tty_inject_string(tty_t* tty, const char* s) {
    while (*s) {
        tty_enqueue_read(tty, *s++);
    }
}

/* Simple integer-to-decimal helper for escape sequence generation */
static int itoa_simple(int val, char* buf) {
    if (val < 0) val = 0;
    char tmp[12];
    int len = 0;
    if (val == 0) { tmp[len++] = '0'; }
    else {
        while (val > 0) {
            tmp[len++] = '0' + (val % 10);
            val /= 10;
        }
    }
    /* reverse */
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

/*
 * tty_mouse_report - Generate SGR mouse escape sequences from mouse events.
 * Called from the mouse IRQ handler when mouse tracking modes are enabled.
 *
 * pixel_x, pixel_y: mouse position in pixels
 * buttons: current button state bitmask (bit0=left, bit1=right, bit2=middle)
 * prev_buttons: previous button state bitmask (for detecting press/release)
 *
 * SGR format: \033[<Cb;Cx;CyM  (press) or \033[<Cb;Cx;Cym  (release)
 *   Cb: 0=left, 1=middle, 2=right, +32=motion, +64=scroll
 *   Cx, Cy: 1-based cell coordinates
 */
void tty_mouse_report(int pixel_x, int pixel_y, uint8_t buttons, uint8_t prev_buttons) {
    tty_t* tty = &g_console_tty;

    /* Only report if mouse tracking is enabled */
    if (!tty->mouse_tracking && !tty->mouse_btn_event)
        return;

    /* Convert pixel coordinates to cell coordinates (1-based) */
    uint32_t rows, cols;
    console_get_dimensions(&rows, &cols);
    if (rows == 0 || cols == 0) return;

    /* Get character dimensions: default 8x16 but use actual from console */
    uint32_t cw = 8, ch = 16;  /* default char size */
    /* We use screen_width/cols and screen_height/rows for cell size */
    /* Actually, use console char dimensions directly */
    extern uint32_t char_width;
    extern uint32_t char_height;
    cw = char_width;
    ch = char_height;
    if (cw == 0) cw = 8;
    if (ch == 0) ch = 16;

    int cx = (pixel_x / (int)cw) + 1;  /* 1-based column */
    int cy = (pixel_y / (int)ch) + 1;  /* 1-based row */
    if (cx < 1) cx = 1;
    if (cy < 1) cy = 1;
    if (cx > (int)cols) cx = (int)cols;
    if (cy > (int)rows) cy = (int)rows;

    /* Detect button changes */
    uint8_t pressed  = buttons & ~prev_buttons;  /* newly pressed */
    uint8_t released = prev_buttons & ~buttons;  /* newly released */
    int motion = (buttons == prev_buttons) && (buttons != 0);

    /* Helper macro for building and injecting an SGR mouse sequence */
    #define INJECT_SGR_MOUSE(cb_val, is_release) do { \
        char seq[32]; \
        int pos = 0; \
        seq[pos++] = '\033'; \
        seq[pos++] = '['; \
        seq[pos++] = '<'; \
        pos += itoa_simple(cb_val, seq + pos); \
        seq[pos++] = ';'; \
        pos += itoa_simple(cx, seq + pos); \
        seq[pos++] = ';'; \
        pos += itoa_simple(cy, seq + pos); \
        seq[pos++] = (is_release) ? 'm' : 'M'; \
        seq[pos] = '\0'; \
        tty_inject_string(tty, seq); \
    } while(0)

    /* Report button presses */
    if (pressed & 0x01) {         /* left press */
        INJECT_SGR_MOUSE(0, 0);
    }
    if (pressed & 0x04) {         /* middle press */
        INJECT_SGR_MOUSE(1, 0);
    }
    if (pressed & 0x02) {         /* right press */
        INJECT_SGR_MOUSE(2, 0);
    }

    /* Report button releases */
    if (released & 0x01) {        /* left release */
        INJECT_SGR_MOUSE(0, 1);
    }
    if (released & 0x04) {        /* middle release */
        INJECT_SGR_MOUSE(1, 1);
    }
    if (released & 0x02) {        /* right release */
        INJECT_SGR_MOUSE(2, 1);
    }

    /* Report motion with button held (mode 1002: button-event tracking) */
    if (motion && tty->mouse_btn_event) {
        int cb = 32;  /* motion flag */
        if (buttons & 0x01) cb |= 0;       /* left + motion */
        else if (buttons & 0x04) cb |= 1;  /* middle + motion */
        else if (buttons & 0x02) cb |= 2;  /* right + motion */
        INJECT_SGR_MOUSE(cb, 0);
    }

    #undef INJECT_SGR_MOUSE

    /* Wake readers waiting for input */
    if (pressed || released || (motion && tty->mouse_btn_event))
        tty_wake_readers(&tty->read_waiters);
}

/*
 * tty_mouse_report_scroll - Generate SGR mouse escape for scroll wheel events.
 * scroll_delta: negative = scroll up, positive = scroll down
 */
void tty_mouse_report_scroll(int pixel_x, int pixel_y, int scroll_delta) {
    tty_t* tty = &g_console_tty;

    if (!tty->mouse_tracking && !tty->mouse_btn_event)
        return;

    uint32_t rows, cols;
    console_get_dimensions(&rows, &cols);
    if (rows == 0 || cols == 0) return;

    extern uint32_t char_width;
    extern uint32_t char_height;
    uint32_t cw = char_width ? char_width : 8;
    uint32_t ch = char_height ? char_height : 16;

    int cx = (pixel_x / (int)cw) + 1;
    int cy = (pixel_y / (int)ch) + 1;
    if (cx < 1) cx = 1;
    if (cy < 1) cy = 1;
    if (cx > (int)cols) cx = (int)cols;
    if (cy > (int)rows) cy = (int)rows;

    /* SGR scroll: Cb=64 for scroll-up, Cb=65 for scroll-down */
    int cb = (scroll_delta < 0) ? 64 : 65;

    char seq[32];
    int pos = 0;
    seq[pos++] = '\033';
    seq[pos++] = '[';
    seq[pos++] = '<';
    pos += itoa_simple(cb, seq + pos);
    seq[pos++] = ';';
    pos += itoa_simple(cx, seq + pos);
    seq[pos++] = ';';
    pos += itoa_simple(cy, seq + pos);
    seq[pos++] = 'M';
    seq[pos] = '\0';
    tty_inject_string(tty, seq);
    tty_wake_readers(&tty->read_waiters);
}

void tty_input_char(tty_t* tty, char c, int ctrl) {
    if (!tty || c == 0) {
        return;
    }

    if (ctrl && c >= 'A' && c <= 'Z') {
        c = (char)((c - 'A' + 1) & 0x1F);
    } else if (ctrl && c >= 'a' && c <= 'z') {
        c = (char)((c - 'a' + 1) & 0x1F);
    }

    if (tty->term.c_iflag & ICRNL) {
        if (c == '\r') {
            c = '\n';
        }
    }

    if (tty->term.c_lflag & ISIG) {
        if (c == tty->term.c_cc[VINTR]) {
            // Echo ^C if ECHO is set
            if (tty->term.c_lflag & ECHO) {
                tty->output(tty, '^');
                tty->output(tty, 'C');
                tty->output(tty, '\n');
            }
            // Clear any pending canonical input
            tty->canon_len = 0;
            tty_signal_pgrp(tty, SIGINT);
            return;
        }
        if (c == tty->term.c_cc[VQUIT]) {
            // Echo ^\\ if ECHO is set
            if (tty->term.c_lflag & ECHO) {
                tty->output(tty, '^');
                tty->output(tty, '\\');
                tty->output(tty, '\n');
            }
            tty->canon_len = 0;
            tty_signal_pgrp(tty, SIGQUIT);
            return;
        }
        if (c == tty->term.c_cc[VSUSP]) {
            // Echo ^Z if ECHO is set
            if (tty->term.c_lflag & ECHO) {
                tty->output(tty, '^');
                tty->output(tty, 'Z');
                tty->output(tty, '\n');
            }
            tty->canon_len = 0;
            tty_signal_pgrp(tty, SIGTSTP);
            return;
        }
    }

    if (tty->term.c_lflag & ICANON) {
        // Handle backspace: check VERASE (typically 8) or DEL (127)
        if (c == tty->term.c_cc[VERASE] || c == 127) {
            if (tty->canon_len > 0) {
                tty->canon_len--;
                if (tty->term.c_lflag & ECHO) {
                    tty->output(tty, '\b');
                    tty->output(tty, ' ');
                    tty->output(tty, '\b');
                }
            }
            return;
        }
        if (c == tty->term.c_cc[VKILL]) {
            while (tty->canon_len > 0) {
                tty->canon_len--;
                if (tty->term.c_lflag & ECHO) {
                    tty->output(tty, '\b');
                    tty->output(tty, ' ');
                    tty->output(tty, '\b');
                }
            }
            return;
        }
        if (c == tty->term.c_cc[VEOF]) {
            if (tty->canon_len == 0) {
                tty->eof_pending = 1;
                tty_wake_readers(&tty->read_waiters);
                return;
            }
            for (uint16_t i = 0; i < tty->canon_len; ++i) {
                tty_enqueue_read(tty, tty->canon_buf[i]);
            }
            tty->canon_len = 0;
            tty_wake_readers(&tty->read_waiters);
            return;
        }
        if (tty->canon_len < sizeof(tty->canon_buf)) {
            tty->canon_buf[tty->canon_len++] = c;
        }
        if (tty->term.c_lflag & ECHO) {
            if (c == '\n' && (tty->term.c_oflag & OPOST) &&
                (tty->term.c_oflag & ONLCR)) {
                tty->output(tty, '\r');
            }
            tty->output(tty, c);
        }
        if (c == '\n') {
            for (uint16_t i = 0; i < tty->canon_len; ++i) {
                tty_enqueue_read(tty, tty->canon_buf[i]);
            }
            tty->canon_len = 0;
            tty_wake_readers(&tty->read_waiters);
        }
        return;
    }

    tty_enqueue_read(tty, c);
    if (tty->term.c_lflag & ECHO) {
        if (c == '\n' && (tty->term.c_oflag & OPOST) &&
            (tty->term.c_oflag & ONLCR)) {
            tty->output(tty, '\r');
        }
        tty->output(tty, c);
    }
    tty_wake_readers(&tty->read_waiters);
}

long tty_read(tty_t* tty, void* buf, long count, int nonblock) {
    if (!tty || !buf || count <= 0) {
        return 0;
    }

    task_t* cur = sched_current();
    char* out = (char*)buf;
    long read = 0;

    /* In non-canonical mode, honour VMIN / VTIME (POSIX semantics).
     * VMIN=0, VTIME=0 → never block (return 0 if nothing available).
     * VMIN=0, VTIME>0 → wait up to VTIME*100ms for a byte, then return 0.
     * VMIN>0, VTIME=0 → block until VMIN bytes available (default). */
    int vmin_zero = 0;
    uint64_t vtime_deadline = 0; /* 0 = no deadline */
    if (!(tty->term.c_lflag & ICANON) && tty->term.c_cc[VMIN] == 0) {
        vmin_zero = 1;
        unsigned char vtime = tty->term.c_cc[VTIME];
        if (vtime > 0) {
            /* VTIME is in tenths of a second; compute deadline in ticks */
            uint32_t freq = timer_get_frequency();
            uint64_t timeout_ticks = ((uint64_t)vtime * freq + 9) / 10;
            vtime_deadline = timer_ticks() + timeout_ticks;
        } else {
            /* VMIN=0, VTIME=0: pure non-blocking */
            nonblock = 1;
        }
    }

    while (read < count) {
        // Check if we've been killed or have a pending signal
        if (cur && (cur->state == TASK_ZOMBIE || signal_pending(cur))) {
            // Handle pending signal
            if (signal_pending(cur)) {
                return read > 0 ? read : -EINTR;
            }
            return read > 0 ? read : -EINTR;
        }
        if (tty->eof_pending && tty->read_count == 0) {
            tty->eof_pending = 0;
            return read;
        }
        char c = 0;
        if (!tty_dequeue_read(tty, &c)) {
            if (read > 0)
                break;
            if (nonblock) {
                /* POSIX: O_NONBLOCK with no data → EAGAIN, not EOF. */
                return -EAGAIN;
            }
            /* VMIN=0, VTIME>0: timed wait — block with a deadline */
            if (vtime_deadline && cur) {
                if (timer_ticks() >= vtime_deadline) {
                    break; /* timeout expired, return 0 */
                }
                /* Park atomically wrt the producer: set BLOCKED+channel,
                 * then re-check read_count under tty_lock.  If a producer
                 * already enqueued a byte (and its wake fired before we
                 * marked ourselves BLOCKED), undo the park and loop. */
                uint64_t _f;
                cur->wait_channel = (void*)&tty->read_waiters;
                cur->wakeup_tick = vtime_deadline;
                cur->state = TASK_BLOCKED;
                spin_lock_irqsave(&tty_lock, &_f);
                if (tty->read_count > 0) {
                    cur->state = TASK_READY;
                    cur->wait_channel = NULL;
                    cur->wakeup_tick = 0;
                    spin_unlock_irqrestore(&tty_lock, _f);
                    continue;
                }
                spin_unlock_irqrestore(&tty_lock, _f);
                sched_schedule();
                if (cur->state == TASK_ZOMBIE || cur->has_exited || signal_pending(cur)) {
                    if (signal_pending(cur)) {
                        return read > 0 ? read : -EINTR;
                    }
                    return read > 0 ? read : -EINTR;
                }
                /* Woke up — could be data arrival or timeout; loop to check */
                continue;
            }
            if (cur) {
                /* Park atomically wrt the producer: set BLOCKED+channel,
                 * then re-check read_count under tty_lock to close the
                 * lost-wakeup race window between tty_dequeue_read (which
                 * dropped tty_lock before returning 0) and our own park.
                 * Without this, a producer that enqueues+wakes here can
                 * find no blocked task and we sleep forever — which is
                 * exactly what kept ncurses-based readers (nano, top)
                 * stuck while non-ncurses readers (less) drained the
                 * buffer in single large reads and rarely re-blocked. */
                uint64_t _f;
                cur->wait_channel = (void*)&tty->read_waiters;
                cur->state = TASK_BLOCKED;
                spin_lock_irqsave(&tty_lock, &_f);
                if (tty->read_count > 0) {
                    cur->state = TASK_READY;
                    cur->wait_channel = NULL;
                    spin_unlock_irqrestore(&tty_lock, _f);
                    continue;
                }
                spin_unlock_irqrestore(&tty_lock, _f);
                sched_schedule();
                // Check if we were killed or have a pending signal
                if (cur->state == TASK_ZOMBIE || cur->has_exited || signal_pending(cur)) {
                    // Handle pending signal
                    if (signal_pending(cur)) {
                        return read > 0 ? read : -EINTR;
                    }
                    return read > 0 ? read : -EINTR;
                }
                continue;
            }
            break;
        }
        // SMAP-aware write to user buffer
        smap_disable();
        out[read++] = c;
        smap_enable();
        if ((tty->term.c_lflag & ICANON) && c == '\n') {
            break;
        }
    }

    /* O_NONBLOCK with no data → EAGAIN; VMIN=0 with no data → 0 */
    if (read == 0 && nonblock && !vmin_zero) {
        return -EAGAIN;
    }
    return read;
}

long tty_write(tty_t* tty, const void* buf, long count) {
    if (!tty || !buf || count <= 0) {
        return 0;
    }

    /* PTY slave writes go to a per-pty ring buffer that has its own lock
     * and a wait queue for blocking when full.  Holding the global
     * tty_lock here would (a) be unnecessary because the ring is per-pty
     * and (b) prevent the writer from sleeping when the ring is full
     * (tty_lock is taken with IRQs off).  Use the blocking enqueue path
     * instead so bursts like `ps aux` / `dmesg` / a top-screen redraw
     * never silently lose bytes \u2014 dropped bytes corrupt ANSI sequences
     * and confuse the master-side terminal emulator (e.g. tmux). */
    if (tty->is_pty && !tty->is_master && tty->priv) {
        pty_t* pty = (pty_t*)tty->priv;
        const char* in = (const char*)buf;
        long total = 0;
        #define PTY_OUT_CHUNK 512
        char tmpbuf[PTY_OUT_CHUNK];
        char outbuf[PTY_OUT_CHUNK * 2]; /* worst case: every byte expands \n -> \r\n */
        int do_opost = (tty->term.c_oflag & OPOST) && (tty->term.c_oflag & ONLCR);
        while (total < count) {
            long chunk = count - total;
            if (chunk > PTY_OUT_CHUNK) chunk = PTY_OUT_CHUNK;
            smap_disable();
            for (long i = 0; i < chunk; i++) tmpbuf[i] = in[total + i];
            smap_enable();
            long out_len = 0;
            if (do_opost) {
                for (long i = 0; i < chunk; i++) {
                    char c = tmpbuf[i];
                    if (c == '\n') outbuf[out_len++] = '\r';
                    outbuf[out_len++] = c;
                }
            } else {
                for (long i = 0; i < chunk; i++) outbuf[out_len++] = tmpbuf[i];
            }
            (void)pty_master_enqueue_bulk(pty, outbuf, out_len);
            /* Always advance by the user-visible chunk size: ring overflow
             * drops bytes silently rather than blocking the writer.  See
             * comment on PTY_MASTER_BUF_SIZE for why blocking is unsafe. */
            total += chunk;
        }
        #undef PTY_OUT_CHUNK
        return total;
    }

    // Copy user buffer into a small kernel-side staging buffer so we do
    // one SMAP window per chunk instead of per character, and hold the
    // tty_lock for the entire write so two CPUs can't interleave chars.
    #define TTY_WRITE_CHUNK 256
    char tmp[TTY_WRITE_CHUNK];
    char mirror_tmp[TTY_WRITE_CHUNK];
    long written = 0;
    int mirror_console = (tty == tty_get_console());

    // Enter batch mode: suppresses cursor updates on other CPUs
    // and enables rate-limited VRAM flushing (~50fps)
    console_batch_begin();

    while (written < count) {
        long chunk = count - written;
        if (chunk > TTY_WRITE_CHUNK) chunk = TTY_WRITE_CHUNK;

        // Bulk copy from user space
        smap_disable();
        for (long i = 0; i < chunk; i++)
            tmp[i] = ((const char*)buf)[written + i];
        smap_enable();

        uint64_t flags;
        long mirror_len = 0;
        spin_lock_irqsave(&tty_lock, &flags);
        for (long i = 0; i < chunk; i++) {
            char c = tmp[i];
            /* Output post-processing (OPOST/ONLCR): translate bare LF
             * to CR+LF so cursor returns to column 0.  Skip when the
             * caller has cleared OPOST (raw mode — tmux, full-screen
             * apps), which set their own \r\n explicitly. */
            if ((tty->term.c_oflag & OPOST) &&
                (tty->term.c_oflag & ONLCR) && c == '\n') {
                if (mirror_console && mirror_len < (long)sizeof(mirror_tmp))
                    mirror_tmp[mirror_len++] = '\r';
                tty->output(tty, '\r');
            }
            if (mirror_console && mirror_len < (long)sizeof(mirror_tmp)) {
                mirror_tmp[mirror_len++] = c;
            }
            tty->output(tty, c);
        }
        spin_unlock_irqrestore(&tty_lock, flags);

        if (mirror_console && mirror_len > 0) {
            usbserial_log_write(mirror_tmp, (uint32_t)mirror_len);
        }

        /* If the parser injected a DSR/DA reply into our read buffer
         * during this chunk, wake any blocked readers now that we’ve
         * dropped tty_lock.  Doing the wake under tty_lock is unsafe:
         * sched_enqueue_ready may take scheduler locks and we hold
         * tty_lock with IRQs off. */
        if (mirror_console && g_console_reply_pending) {
            g_console_reply_pending = 0;
            tty_wake_readers(&tty->read_waiters);
        }

        // Rate-limited VRAM flush (~50fps) — skips if too recent
        console_flush();

        written += chunk;
    }

    // End batch mode: unconditional final flush to ensure last frame is visible
    console_batch_end();

    return count;
    #undef TTY_WRITE_CHUNK
}

int tty_ioctl(tty_t* tty, unsigned long req, void* argp, task_t* cur) {
    if (!tty) {
        return -ENOTTY;
    }
    switch (req) {
        case TCGETS:
            if (!argp) return -EFAULT;
            // Security: Use SMAP-aware copy to user space
            return tty_copy_to_user(argp, &tty->term, sizeof(termios_k_t));
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            if (!argp) return -EFAULT;
            // Security: Use SMAP-aware copy from user space
            {
                /* Only log when termios actually CHANGES, not the
                 * spinning re-arm done by ncurses' input timing loop. */
                int _r = tty_copy_from_user(&tty->term, argp, sizeof(termios_k_t));
                return _r;
            }
        case TIOCGPGRP: {
            if (!argp) return -EFAULT;
            int pgid = tty->fg_pgid;
            // Security: Use SMAP-aware copy to user space
            return tty_copy_to_user(argp, &pgid, sizeof(int));
        }
        case TIOCSPGRP: {
            if (!argp) return -EFAULT;
            int pgid;
            // Security: Use SMAP-aware copy from user space
            int ret = tty_copy_from_user(&pgid, argp, sizeof(int));
            if (ret != 0) return ret;
            tty->fg_pgid = pgid;
            return 0;
        }
        case TIOCSCTTY:
            if (!cur) return -EINVAL;
            cur->ctty = tty;
            return 0;
        case TIOCGWINSZ:
            if (!argp) return -EFAULT;
            // Security: Use SMAP-aware copy to user space
            return tty_copy_to_user(argp, &tty->winsz, sizeof(struct winsize));
        case TIOCSWINSZ:
            if (!argp) return -EFAULT;
            // Security: Use SMAP-aware copy from user space
            return tty_copy_from_user(&tty->winsz, argp, sizeof(struct winsize));
        case TIOCSGUARD:
            if (tty == tty_get_console()) {
                console_set_prompt_guard();
                return 0;
            }
            return 0;
        default:
            return -ENOTTY;
    }
}

int tty_pty_allocate(int* out_id) {
    if (!out_id) {
        return -EINVAL;
    }
    for (int i = 0; i < TTY_MAX_PTYS; ++i) {
        if (g_ptys[i].id == -1) {
            pty_t* pty = &g_ptys[i];
            mm_memset(pty, 0, sizeof(pty_t));
            spinlock_init(&pty->lock, "pty");
            pty->id = i;
            pty->master_open = 1;
            pty->slave_open = 0;
            pty->slave.id = i;
            pty->slave.is_pty = 1;
            pty->slave.is_master = 0;
            pty->slave.output = tty_output_pty_slave;
            pty->slave.priv = pty;
            tty_set_default_termios(&pty->slave);
            pty->slave.winsz.ws_row = 25;
            pty->slave.winsz.ws_col = 80;
            if (out_id) {
                *out_id = i;
            }
            return 0;
        }
    }
    return -ENOSYS;
}

tty_t* tty_get_pty_slave(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return NULL;
    }
    return &pty->slave;
}

int tty_pty_slave_open(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return -EINVAL;
    }
    pty->slave_open = 1;
    return 0;
}

int tty_pty_is_allocated(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return 0;
    }
    return pty->master_open || pty->slave_open;
}

long tty_pty_master_read(int id, void* buf, long count, int nonblock) {
    pty_t* pty = tty_get_pty(id);
    if (!pty || !buf || count <= 0) {
        return -EINVAL;
    }
    task_t* cur = sched_current();
    char* out = (char*)buf;
    long read = 0;
    while (read < count) {
        uint64_t flags;
        spin_lock_irqsave(&pty->lock, &flags);

        /* Drain whatever is queued under the lock. */
        while (pty->m_count > 0 && read < count) {
            char c = pty->master_buf[pty->m_head];
            pty->m_head = (pty->m_head + 1) % PTY_MASTER_BUF_SIZE;
            pty->m_count--;
            spin_unlock_irqrestore(&pty->lock, flags);
            /* SMAP-aware write to user buffer (must be done with the lock
             * dropped — user-space access can fault). */
            smap_disable();
            out[read++] = c;
            smap_enable();
            spin_lock_irqsave(&pty->lock, &flags);
        }

        if (read > 0) {
            spin_unlock_irqrestore(&pty->lock, flags);
            break;
        }

        /* Slave end is closed and the master ring is empty: EOF. */
        if (!pty->slave_open) {
            spin_unlock_irqrestore(&pty->lock, flags);
            break;
        }
        if (nonblock) {
            spin_unlock_irqrestore(&pty->lock, flags);
            break;
        }
        if (!cur) {
            spin_unlock_irqrestore(&pty->lock, flags);
            break;
        }
        if (signal_pending(cur)) {
            spin_unlock_irqrestore(&pty->lock, flags);
            return -EINTR;
        }
        cur->state = TASK_BLOCKED;
        cur->wait_channel = (void*)&pty->master_read_waiters;
        spin_unlock_irqrestore(&pty->lock, flags);
        sched_schedule();
    }
    return read;
}

long tty_pty_master_write(int id, const void* buf, long count) {
    pty_t* pty = tty_get_pty(id);
    if (!pty || !buf || count <= 0) {
        return -EINVAL;
    }
    const char* in = (const char*)buf;
    for (long i = 0; i < count; ++i) {
        // SMAP-aware read from user buffer
        smap_disable();
        char c = in[i];
        smap_enable();
        tty_input_char(&pty->slave, c, 0);
    }
    return count;
}

int tty_pty_master_close(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return -EINVAL;
    }
    pty->master_open = 0;
    if (!pty->slave_open) {
        pty->id = -1;
    }
    return 0;
}

int tty_pty_slave_close(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return -EINVAL;
    }
    pty->slave_open = 0;
    /* Wake any task blocked in tty_pty_master_read so it can observe
     * EOF (read returns 0) and the master fd's poll set transitions to
     * POLLHUP.  Without this, the last shell `exit` leaves tmux's I/O
     * loop blocked indefinitely. */
    tty_wake_readers(&pty->master_read_waiters);
    if (!pty->master_open) {
        pty->id = -1;
    }
    return 0;
}

/* Poll a pty master endpoint. Returns POLLIN when there are bytes
 * queued from the slave, POLLOUT always (writes are always accepted),
 * POLLHUP when the slave end is closed and no data remains. */
int tty_pty_master_poll(int id, int events) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) return 0;
    int rev = 0;
    uint64_t flags;
    spin_lock_irqsave(&pty->lock, &flags);
    uint32_t count = pty->m_count;
    int slave_open = pty->slave_open;
    spin_unlock_irqrestore(&pty->lock, flags);
    if ((events & (POLLIN | POLLRDNORM)) && count > 0)
        rev |= POLLIN | POLLRDNORM;
    if (events & (POLLOUT | POLLWRNORM))
        rev |= POLLOUT | POLLWRNORM;
    if (!slave_open && count == 0)
        rev |= POLLHUP;
    return rev;
}
