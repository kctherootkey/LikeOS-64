// LikeOS-64 Hardware Abstraction Layer - Console
// Framebuffer-based console and printf implementation for 64-bit kernel

#define BOOT_DEBUG 0

#include "../../include/kernel/console.h"
#include "../../include/kernel/serial.h"
#include "../../include/kernel/usb_serial.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/scrollbar.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/sched.h"  // For spinlock_t
#include "../../include/kernel/sysfont.h"  // For external PSF font loading
#include "../../include/kernel/timer.h"   // For timer_ticks() (flush rate-limiting)

#define SIZE_MAX ((size_t)-1)
#define NULL ((void*)0)

// ============================================================================
// SMP LOCKING
// ============================================================================
// Console lock for thread-safe output
static spinlock_t console_lock = SPINLOCK_INIT("console");

// String buffer for ksprintf/ksnprintf
typedef struct {
    char* buffer;
    size_t size;
    size_t pos;
} string_buffer_t;

// Forward declaration
static int kvprintf_to_buffer(const char* format, va_list args, string_buffer_t* sb);

// Helper function to write character to string buffer
static void string_putchar(char c, string_buffer_t* sb) {
    if (sb->pos < sb->size - 1) {
        sb->buffer[sb->pos++] = c;
    }
}

// Global console state
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;

// Active scroll region (DECSTBM).  -1/-1 means "not set" → full screen.
// Inclusive 0-based row indices.
static int g_region_top = -1;
static int g_region_bot = -1;
/* xenl ("eat newline glitch"): when a printable char is written into the
 * very last column, defer the wrap.  The cursor visually stays on that
 * cell; the wrap only happens when the *next* printable char arrives.
 * Any explicit cursor motion clears this state.  Required by tmux for
 * safe writes to the bottom-right corner of the screen — without it,
 * drawing the last cell of the status bar scrolls the whole screen. */
static int g_pending_wrap = 0;
/* When set, bare LF in console_putchar_unlocked advances the row without
 * resetting cursor_x.  Set by console_putchar_batch (tty output path) where
 * OPOST/ONLCR handles the CR separately; cleared for the kprintf direct path
 * which relies on LF implying a carriage return (ONLCR console semantics). */
static int g_lf_no_cr = 0;
// Static copy of framebuffer info - we copy from boot_info since boot_info is in identity-mapped memory
static framebuffer_info_t g_fb_info_copy;
static framebuffer_info_t* fb_info = 0;
static uint32_t fg_color = 0xFFFFFFFF; // White (RGB)
static uint32_t bg_color = 0x00000000; // Black (RGB)
static uint8_t current_vga_fg = VGA_COLOR_WHITE; // Track current VGA indices
static uint8_t current_vga_bg = VGA_COLOR_BLACK;

// Cursor blinking support
static uint8_t cursor_enabled = 0;     // Is cursor visible feature enabled
static uint8_t cursor_shown = 0;       // Current cursor visible state (for blinking)
static uint32_t cursor_blink_ticks = 0; // Tick counter for blinking
static uint32_t cursor_last_x = 0;     // Last cursor X position
static uint32_t cursor_last_y = 0;     // Last cursor Y position

// Batch output mode: suppresses cursor updates and rate-limits VRAM flushes
static volatile int g_console_batch_active = 0;
static uint64_t g_last_flush_tick = 0;
#define CONSOLE_FLUSH_INTERVAL 2  // Min ticks between VRAM flushes (~20ms at 100Hz = 50fps)

// Save buffer for pixels under the cursor (2 pixels wide x max 32 pixels tall)
#define CURSOR_SAVE_W 2
#define CURSOR_SAVE_MAX_H 32
static uint32_t cursor_saved_pixels[CURSOR_SAVE_W * CURSOR_SAVE_MAX_H];
static uint8_t  cursor_pixels_saved = 0; // Whether we have valid saved pixels
static uint32_t cursor_saved_x = 0;     // Position where pixels were saved
static uint32_t cursor_saved_y = 0;

// Font - simple 8x16 bitmap font (complete character set)
static const uint8_t font_8x16[128][16] = {
    // Space (32)
    [32] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    
    // Numbers
    ['0'] = {0x00, 0x00, 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['1'] = {0x00, 0x00, 0x18, 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['2'] = {0x00, 0x00, 0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['3'] = {0x00, 0x00, 0x3C, 0x66, 0x06, 0x06, 0x1C, 0x06, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['4'] = {0x00, 0x00, 0x06, 0x0E, 0x1E, 0x66, 0x66, 0x7F, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['5'] = {0x00, 0x00, 0x7E, 0x60, 0x60, 0x7C, 0x06, 0x06, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['6'] = {0x00, 0x00, 0x3C, 0x66, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['7'] = {0x00, 0x00, 0x7E, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['8'] = {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['9'] = {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    
    // Uppercase Letters
    ['A'] = {0x00, 0x00, 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['B'] = {0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['C'] = {0x00, 0x00, 0x3C, 0x66, 0x60, 0x60, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['D'] = {0x00, 0x00, 0x78, 0x6C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['E'] = {0x00, 0x00, 0x7E, 0x60, 0x60, 0x78, 0x78, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['F'] = {0x00, 0x00, 0x7E, 0x60, 0x60, 0x78, 0x78, 0x60, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['G'] = {0x00, 0x00, 0x3C, 0x66, 0x60, 0x60, 0x6E, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['H'] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x7E, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['I'] = {0x00, 0x00, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['J'] = {0x00, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x6C, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['K'] = {0x00, 0x00, 0x66, 0x6C, 0x78, 0x70, 0x70, 0x78, 0x6C, 0x66, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['L'] = {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['M'] = {0x00, 0x00, 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['N'] = {0x00, 0x00, 0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['O'] = {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['P'] = {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['Q'] = {0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['R'] = {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['S'] = {0x00, 0x00, 0x3C, 0x66, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['T'] = {0x00, 0x00, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['U'] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['V'] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['W'] = {0x00, 0x00, 0x63, 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['X'] = {0x00, 0x00, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x3C, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['Y'] = {0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['Z'] = {0x00, 0x00, 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00},
    
    // Lowercase letters
    ['a'] = {0x00, 0x00, 0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['b'] = {0x00, 0x00, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['c'] = {0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['d'] = {0x00, 0x00, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['e'] = {0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x60, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['f'] = {0x00, 0x00, 0x1C, 0x36, 0x30, 0x30, 0x7C, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['g'] = {0x00, 0x00, 0x00, 0x00, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00},
    ['h'] = {0x00, 0x00, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['i'] = {0x00, 0x00, 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['j'] = {0x00, 0x00, 0x06, 0x00, 0x0E, 0x06, 0x06, 0x06, 0x06, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00},
    ['k'] = {0x00, 0x00, 0x60, 0x60, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['l'] = {0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['m'] = {0x00, 0x00, 0x00, 0x00, 0x66, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['n'] = {0x00, 0x00, 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['o'] = {0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['p'] = {0x00, 0x00, 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00},
    ['q'] = {0x00, 0x00, 0x00, 0x00, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00},
    ['r'] = {0x00, 0x00, 0x00, 0x00, 0x7C, 0x66, 0x60, 0x60, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['s'] = {0x00, 0x00, 0x00, 0x00, 0x3E, 0x60, 0x60, 0x3C, 0x06, 0x06, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['t'] = {0x00, 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['u'] = {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['v'] = {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['w'] = {0x00, 0x00, 0x00, 0x00, 0x63, 0x63, 0x6B, 0x6B, 0x7F, 0x77, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['x'] = {0x00, 0x00, 0x00, 0x00, 0x66, 0x3C, 0x18, 0x18, 0x3C, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['y'] = {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00},
    ['z'] = {0x00, 0x00, 0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00},
    
    // Basic symbols
    ['-'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['.'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['?'] = {0x00, 0x00, 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    [':'] = {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    [';'] = {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['/'] = {0x00, 0x00, 0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['\\'] = {0x00, 0x00, 0x80, 0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['('] = {0x00, 0x00, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00},
    [')'] = {0x00, 0x00, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['@'] = {0x00, 0x00, 0x3C, 0x66, 0x66, 0x6E, 0x6E, 0x60, 0x62, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['#'] = {0x00, 0x00, 0x36, 0x36, 0x7F, 0x36, 0x36, 0x36, 0x7F, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['$'] = {0x00, 0x00, 0x0C, 0x3E, 0x6C, 0x68, 0x3E, 0x16, 0x36, 0x7C, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['%'] = {0x00, 0x00, 0x62, 0x66, 0x0C, 0x18, 0x30, 0x66, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['^'] = {0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['&'] = {0x00, 0x00, 0x38, 0x6C, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['*'] = {0x00, 0x00, 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['+'] = {0x00, 0x00, 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['='] = {0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['_'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00},
    [','] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00},
    ['<'] = {0x00, 0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['>'] = {0x00, 0x00, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['['] = {0x00, 0x00, 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    [']'] = {0x00, 0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['{'] = {0x00, 0x00, 0x0E, 0x18, 0x18, 0x18, 0x70, 0x18, 0x18, 0x18, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['}'] = {0x00, 0x00, 0x70, 0x18, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x18, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['|'] = {0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['~'] = {0x00, 0x00, 0x00, 0x00, 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['`'] = {0x00, 0x00, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['\''] = {0x00, 0x00, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['"'] = {0x00, 0x00, 0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

// Character dimensions - default values for built-in font, updated when external font loaded
#define DEFAULT_CHAR_WIDTH 8
#define DEFAULT_CHAR_HEIGHT 16
uint32_t char_width = DEFAULT_CHAR_WIDTH;
uint32_t char_height = DEFAULT_CHAR_HEIGHT;

// Macros to access current character dimensions
#define CHAR_WIDTH char_width
#define CHAR_HEIGHT char_height

// Calculate max text rows and columns
static uint32_t max_rows = 0;
static uint32_t max_cols = 0;

// VGA color constants for backward compatibility
#define VGA_COLOR_BLACK         0
#define VGA_COLOR_BLUE          1
#define VGA_COLOR_GREEN         2
#define VGA_COLOR_CYAN          3
#define VGA_COLOR_RED           4
#define VGA_COLOR_MAGENTA       5
#define VGA_COLOR_BROWN         6
#define VGA_COLOR_LIGHT_GREY    7
#define VGA_COLOR_DARK_GREY     8
#define VGA_COLOR_LIGHT_BLUE    9
#define VGA_COLOR_LIGHT_GREEN   10
#define VGA_COLOR_LIGHT_CYAN    11
#define VGA_COLOR_LIGHT_RED     12
#define VGA_COLOR_LIGHT_MAGENTA 13
#define VGA_COLOR_LIGHT_BROWN   14
#define VGA_COLOR_WHITE         15

// Convert VGA colors to RGB
uint32_t vga_to_rgb(uint8_t vga_color) {
    static const uint32_t vga_palette[16] = {
        0x000000, // Black
        0x0000AA, // Blue
        0x00AA00, // Green
        0x00AAAA, // Cyan
        0xAA0000, // Red
        0xAA00AA, // Magenta
        0xAA5500, // Brown
        0xAAAAAA, // Light Grey
        0x555555, // Dark Grey
        0x5555FF, // Light Blue
        0x55FF55, // Light Green
        0x55FFFF, // Light Cyan
        0xFF5555, // Light Red
        0xFF55FF, // Light Magenta
        0xFFFF55, // Yellow
        0xFFFFFF  // White
    };
    return vga_palette[vga_color & 0x0F];
}

// ================= Scrollback storage and helpers =================
// Forward declaration for draw_char used in render before definition
static void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

static console_scrollback_t g_sb;
static console_line_t g_lines[CONSOLE_SCROLLBACK_LINES];
static uint16_t g_prompt_guard_len = 0;

static inline uint32_t sb_capacity(void) { return CONSOLE_SCROLLBACK_LINES; }

static inline uint32_t sb_visible_lines(void) { return max_rows; }

static uint32_t sb_effective_total(void) {
    // Include current in-progress line. total_filled_lines counts completed lines.
    uint32_t cap = sb_capacity();
    uint64_t eff = (uint64_t)g_sb.total_filled_lines + 1ULL;
    if (eff > cap) eff = cap;
    return (uint32_t)eff;
}

static void sb_reset(void) {
    g_sb.lines = g_lines;
    g_sb.head = 0;
    g_sb.total_filled_lines = 0;
    g_sb.viewport_top = 0;
    g_sb.visible_lines = sb_visible_lines();
    g_sb.at_bottom = 1;
    g_sb.dragging_thumb = 0;
    g_sb.drag_start_y = 0;
    g_sb.drag_start_viewport = 0;
    g_prompt_guard_len = 0;
    // Zero first line
    for (uint32_t i = 0; i < sb_capacity(); ++i) {
        g_lines[i].length = 0;
        g_lines[i].write_pos = 0;
        g_lines[i].text[0] = '\0';
        g_lines[i].fg = VGA_COLOR_WHITE;
        g_lines[i].bg = VGA_COLOR_BLACK;
        for (uint32_t c = 0; c < CONSOLE_MAX_LINE_LENGTH; ++c) {
            g_lines[i].fg_attrs[c] = VGA_COLOR_WHITE;
            g_lines[i].bg_attrs[c] = VGA_COLOR_BLACK;
        }
    }
}

static console_line_t* sb_current_line(void) { return &g_sb.lines[g_sb.head]; }

static void sb_new_line(void) {
    // Advance head in ring
    g_sb.head = (g_sb.head + 1) % sb_capacity();
    if (g_sb.total_filled_lines < 0xFFFFFFFFu) g_sb.total_filled_lines++;
    g_prompt_guard_len = 0;
    // Clear the new line
    g_sb.lines[g_sb.head].length = 0;
    g_sb.lines[g_sb.head].write_pos = 0;
    g_sb.lines[g_sb.head].text[0] = '\0';
    g_sb.lines[g_sb.head].fg = current_vga_fg;
    g_sb.lines[g_sb.head].bg = current_vga_bg;
    for (uint32_t c = 0; c < CONSOLE_MAX_LINE_LENGTH; ++c) {
        g_sb.lines[g_sb.head].fg_attrs[c] = current_vga_fg;
        g_sb.lines[g_sb.head].bg_attrs[c] = current_vga_bg;
    }
    // Auto-follow: if at bottom, keep viewport pinned
    if (g_sb.at_bottom) {
        uint32_t eff = sb_effective_total();
        if (g_sb.visible_lines <= eff) {
            uint32_t max_vp = eff > g_sb.visible_lines ? (eff - g_sb.visible_lines) : 0;
            g_sb.viewport_top = max_vp;
        }
    }
}

static void sb_append_char(char c) {
    console_line_t* line = sb_current_line();
    if (c == '\n') {
        sb_new_line();
        return;
    }
    if (c == '\r') {
        /* Carriage return: reset write position without erasing visible content.
         * Subsequent writes overwrite from position 0, matching terminal
         * overwrite semantics.  The visible length (line->length) stays until
         * a write actually reaches or exceeds the old boundary. */
        line->write_pos = 0;
        return;
    }
    if (c == '\t') {
        // simple tab expansion: up to next 8-char boundary using spaces
        uint16_t next = (uint16_t)(((line->write_pos + 8) & ~7) - line->write_pos);
        for (uint16_t i = 0; i < next && line->write_pos < CONSOLE_MAX_LINE_LENGTH - 1; ++i) {
            uint16_t idx = line->write_pos;
            line->text[idx] = ' ';
            line->fg_attrs[idx] = current_vga_fg;
            line->bg_attrs[idx] = current_vga_bg;
            line->write_pos++;
            if (line->write_pos > line->length) {
                line->length = line->write_pos;
                line->text[line->length] = '\0';
            }
        }
        return;
    }
    if (c == '\b') {
        if (line->write_pos > 0) {
            if (line->write_pos <= g_prompt_guard_len) {
                return;
            }
            line->write_pos--;
            /* Truncate visible length to the new write position. */
            if (line->write_pos < line->length) {
                line->length = line->write_pos;
                line->text[line->length] = '\0';
            }
        }
        return;
    }
    if (line->write_pos < CONSOLE_MAX_LINE_LENGTH - 1) {
        uint16_t idx = line->write_pos;
        line->text[idx] = c;
        line->fg_attrs[idx] = current_vga_fg;
        line->bg_attrs[idx] = current_vga_bg;
        line->write_pos++;
        if (line->write_pos > line->length) {
            line->length = line->write_pos;
            line->text[line->length] = '\0';
        }
        line->fg = current_vga_fg;
        line->bg = current_vga_bg;
    }
}

static void console_render_view(void) {
    if (!fb_info) return;
    // Text area excludes scrollbar default width and margin
    uint32_t sb_total_w = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
    uint32_t text_w = (fb_info->horizontal_resolution > sb_total_w) ? (fb_info->horizontal_resolution - sb_total_w) : 0;
    uint32_t text_h = fb_info->vertical_resolution;

    // Clear text area
    fb_fill_rect(0, 0, text_w, text_h, bg_color);

    // Compute which logical line index corresponds to viewport_top in ring
    uint32_t eff = sb_effective_total();
    uint32_t start = g_sb.viewport_top; // 0..eff
    // Oldest logical line index maps to ring index: if total_filled <= cap, logical==ring
    uint32_t base_ring_idx;
    if (g_sb.total_filled_lines <= sb_capacity()) {
        base_ring_idx = 0;
    } else {
        // Oldest is head+1 mod cap
        base_ring_idx = (g_sb.head + 1) % sb_capacity();
    }
    // Render rows
    uint32_t rows = sb_visible_lines();
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t logical_idx = start + row;
        if (logical_idx >= eff) break;
        // Map logical to ring index
        uint32_t ring_idx = (g_sb.total_filled_lines <= sb_capacity())
            ? logical_idx
            : (base_ring_idx + logical_idx) % sb_capacity();
        console_line_t* line = &g_sb.lines[ring_idx];
        uint32_t y = row * CHAR_HEIGHT;
        // Draw characters
        uint32_t cols = (line->length < max_cols) ? line->length : max_cols;
        for (uint32_t col = 0; col < cols; ++col) {
            uint8_t ch_fg = line->fg_attrs[col];
            uint8_t ch_bg = line->bg_attrs[col];
            draw_char(line->text[col], col * CHAR_WIDTH, y, vga_to_rgb(ch_fg), vga_to_rgb(ch_bg));
        }
        // Clear remaining cells on the row
        if (cols < max_cols) {
            uint32_t px = cols * CHAR_WIDTH;
            uint32_t w = (max_cols - cols) * CHAR_WIDTH;
            if (w) fb_fill_rect(px, y, w, CHAR_HEIGHT, bg_color);
        }
    }
    // Mark dirty
    if (text_w && text_h) fb_mark_dirty(0, 0, text_w - 1, text_h - 1);
}

static void console_sync_scrollbar(void) {
    scrollbar_t* sb = scrollbar_get_system();
    if (!sb) return;
    scrollbar_content_t content = {
        .total_lines = sb_effective_total(),
        .visible_lines = sb_visible_lines(),
        .viewport_top = g_sb.viewport_top,
    };
    scrollbar_sync_content(sb, &content);
    scrollbar_render(sb);
}

// Draw a single pixel (32-bit BGRA format) - now uses optimized framebuffer
static void set_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    if (x >= fb_info->horizontal_resolution || y >= fb_info->vertical_resolution) return;
    
    // Use optimized framebuffer operations if available
    fb_double_buffer_t* fb_opt = get_fb_double_buffer();
    if (fb_opt) {
        fb_set_pixel(x, y, color);
    } else {
        // Fallback to direct framebuffer access
        uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
        uint32_t pixel_offset = y * fb_info->pixels_per_scanline + x;
        framebuffer[pixel_offset] = color;
    }
}

// Draw or clear the cursor at the current position
static void draw_cursor_at(uint32_t x, uint32_t y, uint8_t show) {
    if (!fb_info) return;
    
    uint32_t pixel_x = x * CHAR_WIDTH;
    uint32_t pixel_y = y * CHAR_HEIGHT;
    uint32_t h = char_height;
    if (h > CURSOR_SAVE_MAX_H) h = CURSOR_SAVE_MAX_H;
    
    if (show) {
        /* If there is already a cursor bar drawn at a DIFFERENT cell,
         * restore its saved pixels before overwriting cursor_saved_pixels
         * with the new cell's data.  Without this, every call to show at
         * position B while save data for position A still exists would
         * silently abandon A's bar on screen — it could never be erased
         * because the pixel-save record was lost.  Over multiple cursor
         * moves this accumulates ghost bars that cover character glyphs,
         * producing the "character erased at left" symptom in tmux/nano. */
        if (cursor_pixels_saved && (cursor_saved_x != x || cursor_saved_y != y)) {
            uint32_t old_px = cursor_saved_x * CHAR_WIDTH;
            uint32_t old_py = cursor_saved_y * CHAR_HEIGHT;
            for (uint32_t row2 = 0; row2 < h; row2++) {
                set_pixel(old_px,     old_py + row2,
                          cursor_saved_pixels[row2 * CURSOR_SAVE_W + 0]);
                set_pixel(old_px + 1, old_py + row2,
                          cursor_saved_pixels[row2 * CURSOR_SAVE_W + 1]);
            }
            cursor_pixels_saved = 0;
        }
        /* If the cursor is already shown at THIS exact cell, do not
         * re-save (we would capture bar pixels instead of character
         * pixels, locking the bar in permanently on the next hide). */
        if (cursor_pixels_saved && cursor_saved_x == x && cursor_saved_y == y)
            return;

        // Save the pixels currently under the cursor position
        fb_double_buffer_t* fb_opt = get_fb_double_buffer();
        for (uint32_t row = 0; row < h; row++) {
            for (uint32_t col = 0; col < CURSOR_SAVE_W; col++) {
                uint32_t px = pixel_x + col;
                uint32_t py = pixel_y + row;
                uint32_t pix;
                if (fb_opt) {
                    pix = fb_get_pixel(px, py);
                } else if (fb_info->framebuffer_base) {
                    uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
                    uint32_t offset = py * fb_info->pixels_per_scanline + px;
                    pix = framebuffer[offset];
                } else {
                    pix = 0;
                }
                cursor_saved_pixels[row * CURSOR_SAVE_W + col] = pix;
            }
        }
        cursor_saved_x = x;
        cursor_saved_y = y;
        cursor_pixels_saved = 1;
        
        // Draw the cursor (gray vertical bar, 2 pixels wide)
        uint32_t cursor_color = 0x00808080;
        for (uint32_t row = 0; row < h; row++) {
            set_pixel(pixel_x, pixel_y + row, cursor_color);
            set_pixel(pixel_x + 1, pixel_y + row, cursor_color);
        }
    } else {
        // Restore the saved pixels if we have them and position matches.
        // If not, the cursor cell has since been overdrawn by a glyph (or
        // a different cell has scrolled into this position) — leave the
        // screen alone.  The old "fallback to bg_color" path here erased
        // real content whenever a draw_char invalidated the saved pixels
        // between the cursor's show and hide phases (very common under
        // tmux, which emits CUP+glyph bursts during redraws).
        if (cursor_pixels_saved && cursor_saved_x == x && cursor_saved_y == y) {
            for (uint32_t row = 0; row < h; row++) {
                for (uint32_t col = 0; col < CURSOR_SAVE_W; col++) {
                    set_pixel(pixel_x + col, pixel_y + row,
                              cursor_saved_pixels[row * CURSOR_SAVE_W + col]);
                }
            }
            cursor_pixels_saved = 0;
        }
        /* If cursor_pixels_saved is 0 here, it means a glyph was drawn over
         * the cursor bar between the last show and this hide call — the glyph
         * already covered the 2-pixel bar.  Doing nothing is correct: no
         * remnant bar is visible, and a bg_color fill would erase the glyph
         * (visible as "left arrow erases a character" inside tmux/nano). */
    }
}

// Draw a character at specific position
// Optimized: writes directly to back buffer, marks dirty once per character
static void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_clr, uint32_t bg_clr) {
    unsigned char uc = (unsigned char)c;
    const uint8_t* glyph = (void*)0;
    uint32_t font_w, font_h, bpr;

    if (sysfont_is_loaded()) {
        glyph = sysfont_get_glyph(uc);
        if (!glyph) glyph = sysfont_get_glyph('?');
        font_w = sysfont_get_width();
        font_h = sysfont_get_height();
        bpr = (font_w + 7) / 8;
    } else {
        if (uc >= 128) uc = '?';
        glyph = font_8x16[uc];
        font_w = DEFAULT_CHAR_WIDTH;
        font_h = DEFAULT_CHAR_HEIGHT;
        bpr = 1;
    }
    if (!glyph) return;

    // Fast path: write directly to back buffer, mark dirty once
    fb_double_buffer_t* fb_opt = get_fb_double_buffer();
    if (fb_opt) {
        fb_draw_char_fast(x, y, glyph, font_w, font_h, bpr, fg_clr, bg_clr);
        return;
    }

    // Fallback: direct framebuffer pixel-by-pixel (pre-init only)
    for (uint32_t row = 0; row < font_h; row++) {
        for (uint32_t col = 0; col < font_w; col++) {
            uint32_t byte_idx = row * bpr + (col / 8);
            uint8_t bit_mask = 0x80 >> (col % 8);
            if (glyph[byte_idx] & bit_mask)
                set_pixel(x + col, y + row, fg_clr);
            else
                set_pixel(x + col, y + row, bg_clr);
        }
    }
}

// Initialize the console with framebuffer info
void console_init(framebuffer_info_t* fb) {
    // Copy the framebuffer info to our static storage so we don't depend on
    // the boot_info structure which is in identity-mapped memory
    if (fb) {
        g_fb_info_copy = *fb;  // Copy the structure
        fb_info = &g_fb_info_copy;  // Point to our copy
    } else {
        fb_info = 0;
    }
    cursor_x = 0;
    cursor_y = 0;
    fg_color = 0xFFFFFFFF; // White
    bg_color = 0x00000000; // Black
    current_vga_fg = VGA_COLOR_WHITE;
    current_vga_bg = VGA_COLOR_BLACK;
    // Initialize serial console early so kprintf mirrors to QEMU if present
#ifdef SERIAL_ENABLED
    serial_init();
#endif
    
    if (fb_info) {
        // Calculate text area width (exclude scrollbar area)
        uint32_t scrollbar_total_width = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
        uint32_t text_area_width = fb_info->horizontal_resolution - scrollbar_total_width;
        max_cols = text_area_width / CHAR_WIDTH;
        max_rows = fb_info->vertical_resolution / CHAR_HEIGHT;
    } else {
        // Fallback values
        max_cols = 80;
        max_rows = 25;
    }
    
    console_clear();
    // Initialize scrollback storage once fb and rows/cols are known
    sb_reset();
}

// Remap framebuffer to use direct map (call before removing identity mapping)
// This converts the framebuffer_base from identity-mapped address to direct-mapped address
void console_remap_to_direct_map(void) {
    if (fb_info && fb_info->framebuffer_base) {
        // The framebuffer base was passed as a physical address (identity-mapped by bootloader)
        uint64_t fb_phys = (uint64_t)fb_info->framebuffer_base;
        
        // Check if framebuffer is within our 4GB direct map range
        if (fb_phys >= 0x100000000ULL) {
            // Leave as identity-mapped - this will crash after identity map removal
            return;
        }
        
        // Convert it to use the direct map at PHYS_MAP_BASE
        fb_info->framebuffer_base = phys_to_virt(fb_phys);
    }
}

// Initialize framebuffer optimization system (call after console_init)
void console_init_fb_optimization(void) {
    if (!fb_info) {
#if BOOT_DEBUG
        kprintf("Console: No framebuffer available for optimization\n");
#endif
        return;
    }
    
    // Initialize framebuffer optimization system
    if (fb_optimize_init(fb_info) == 0) {
#if BOOT_DEBUG
        kprintf("Console: Framebuffer optimization enabled\n");
#endif
    } else {
#if BOOT_DEBUG
        kprintf("Console: Using direct framebuffer (no optimization)\n");
#endif
    }
}

// Apply the system font (after loading via sysfont_load)
// Updates character dimensions, recalculates rows/cols, and redraws the screen
void console_apply_sysfont(void) {
    if (!sysfont_is_loaded()) {
        return;
    }
    
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    
    // Update character dimensions from loaded font
    char_width = sysfont_get_width();
    char_height = sysfont_get_height();
    
    // Recalculate max rows and columns based on new font size
    if (fb_info) {
        uint32_t scrollbar_total_width = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
        uint32_t text_area_width = fb_info->horizontal_resolution - scrollbar_total_width;
        max_cols = text_area_width / char_width;
        max_rows = fb_info->vertical_resolution / char_height;
        
        // Update scrollback visible lines
        g_sb.visible_lines = max_rows;
    }
    
    // Hide cursor temporarily
    if (cursor_enabled && cursor_shown) {
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);
    }
    
    // Redraw the entire screen with the scrollback content using the new font
    console_render_view();
    console_sync_scrollbar();
    
    // Flush changes
    fb_flush_dirty_regions();
    
    spin_unlock_irqrestore(&console_lock, flags);
    
    kprintf("Console: using system font (%ux%u)\n", char_width, char_height);
}

// Clear the entire screen (text area only, preserve scrollbar)
void console_clear(void) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    
    // Calculate text area width (exclude scrollbar area)
    uint32_t scrollbar_total_width = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
    uint32_t text_area_width = fb_info->horizontal_resolution - scrollbar_total_width;
    
    fb_double_buffer_t* fb_opt = get_fb_double_buffer();
    if (fb_opt) {
        // Use optimized fill operation - only clear text area
        fb_fill_rect(0, 0, text_area_width, fb_info->vertical_resolution, bg_color);
        fb_flush_dirty_regions();  // Immediately update the screen
    } else {
        // Fallback to direct framebuffer access
        uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
        uint32_t pixels_per_line = fb_info->pixels_per_scanline;
        
        // Clear only the text area, preserve scrollbar
        for (uint32_t y = 0; y < fb_info->vertical_resolution; y++) {
            uint32_t* line = &framebuffer[y * pixels_per_line];
            for (uint32_t x = 0; x < text_area_width; x++) {
                line[x] = bg_color;
            }
        }
    }
    
    cursor_x = 0;
    cursor_y = 0;
    g_prompt_guard_len = 0;
    // reset viewport to top as well
    g_sb.viewport_top = 0;
    g_sb.at_bottom = 1;
}

// Set cursor position (0-based row, col)
void console_set_cursor_pos(uint32_t row, uint32_t col) {
    if (!fb_info) return;
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    // Hide cursor at old position
    if (cursor_enabled && cursor_shown) {
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);
    }

    if (row >= max_rows) row = max_rows - 1;
    if (col >= max_cols) col = max_cols - 1;
    cursor_y = row;
    cursor_x = col;
    g_pending_wrap = 0;

    // Sync cursor tracking to prevent stale position causing artifacts
    cursor_last_x = col;
    cursor_last_y = row;
    cursor_shown = 0;
    cursor_pixels_saved = 0;   /* old cell may now contain a glyph */
    cursor_blink_ticks = 0;

    // Ensure viewport is at bottom for subsequent output
    g_sb.at_bottom = 1;

    spin_unlock_irqrestore(&console_lock, flags);
}

void console_get_cursor_pos(uint32_t *row, uint32_t *col) {
    if (row) *row = cursor_y;
    if (col) *col = cursor_x;
}

// Erase display: 0=from cursor to end, 1=from start to cursor, 2=entire screen
void console_erase_display(int mode) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    uint32_t sb_total_w = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
    uint32_t text_w = (fb_info->horizontal_resolution > sb_total_w)
                     ? (fb_info->horizontal_resolution - sb_total_w) : 0;
    uint32_t scr_h = fb_info->vertical_resolution;

    // Hide cursor before erasing
    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_x, cursor_y, 0);

    if (mode == 2) {
        // Erase entire screen
        fb_fill_rect(0, 0, text_w, scr_h, bg_color);
        cursor_x = 0;
        cursor_y = 0;
        cursor_last_x = 0;
        cursor_last_y = 0;
        cursor_shown = 0;
        cursor_blink_ticks = 0;
    } else if (mode == 0) {
        // Erase from cursor to end: rest of current line + all lines below
        uint32_t y = cursor_y * CHAR_HEIGHT;
        uint32_t x = cursor_x * CHAR_WIDTH;
        // Clear rest of current line
        if (x < text_w)
            fb_fill_rect(x, y, text_w - x, CHAR_HEIGHT, bg_color);
        // Clear all lines below
        uint32_t below_y = (cursor_y + 1) * CHAR_HEIGHT;
        if (below_y < scr_h)
            fb_fill_rect(0, below_y, text_w, scr_h - below_y, bg_color);
    } else if (mode == 1) {
        // Erase from start to cursor: all lines above + start of current line
        uint32_t y = cursor_y * CHAR_HEIGHT;
        if (y > 0)
            fb_fill_rect(0, 0, text_w, y, bg_color);
        // Clear start of current line up to cursor
        uint32_t x = (cursor_x + 1) * CHAR_WIDTH;
        if (x > text_w) x = text_w;
        fb_fill_rect(0, y, x, CHAR_HEIGHT, bg_color);
    }

    fb_flush_dirty_regions();
    g_sb.at_bottom = 1;

    spin_unlock_irqrestore(&console_lock, flags);
}

// Erase line: 0=from cursor to end, 1=from start to cursor, 2=entire line
void console_erase_line(int mode) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    uint32_t sb_total_w = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
    uint32_t text_w = (fb_info->horizontal_resolution > sb_total_w)
                     ? (fb_info->horizontal_resolution - sb_total_w) : 0;
    uint32_t y = cursor_y * CHAR_HEIGHT;

    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_x, cursor_y, 0);

    if (mode == 2) {
        // Erase entire line
        fb_fill_rect(0, y, text_w, CHAR_HEIGHT, bg_color);
        /* Truncate scrollback line completely. */
        console_line_t* ln = sb_current_line();
        ln->write_pos = 0;
        ln->length = 0;
        ln->text[0] = '\0';
    } else if (mode == 0) {
        // Erase from cursor to end
        uint32_t x = cursor_x * CHAR_WIDTH;
        if (x < text_w)
            fb_fill_rect(x, y, text_w - x, CHAR_HEIGHT, bg_color);
        /* Truncate scrollback line to cursor_x so that EL0 after a readline
         * reprint (\\r + shorter-content) removes the stale tail characters. */
        console_line_t* ln = sb_current_line();
        if (cursor_x < ln->length) {
            ln->length = (uint16_t)cursor_x;
            ln->text[cursor_x] = '\0';
        }
        if (cursor_x < ln->write_pos)
            ln->write_pos = (uint16_t)cursor_x;
    } else if (mode == 1) {
        // Erase from start to cursor
        uint32_t x = (cursor_x + 1) * CHAR_WIDTH;
        if (x > text_w) x = text_w;
        fb_fill_rect(0, y, x, CHAR_HEIGHT, bg_color);
    }

    fb_flush_dirty_regions();

    spin_unlock_irqrestore(&console_lock, flags);
}

/* ----------------------------------------------------------------------
 * VT-style line / character editing primitives.
 * Used by terminal emulators (e.g. tmux) that drive the framebuffer
 * console as a real VT100/xterm.  Coordinates are 0-based row/col.
 * ---------------------------------------------------------------------- */

static uint32_t console_text_width_pixels(void) {
    if (!fb_info) return 0;
    uint32_t sb_total_w = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
    return (fb_info->horizontal_resolution > sb_total_w)
         ? (fb_info->horizontal_resolution - sb_total_w) : 0;
}

/* Scroll a rectangular region [top..bot] (rows, inclusive) up by N rows.
 * Top N rows of the region are discarded; bottom N rows become blank.    */
void console_scroll_region_up(int n, int top, int bot) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    if (n <= 0) return;
    if (top < 0) top = 0;
    if (bot >= (int)max_rows) bot = (int)max_rows - 1;
    if (top > bot) return;
    int region_rows = bot - top + 1;
    if (n > region_rows) n = region_rows;

    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    uint32_t text_w = console_text_width_pixels();
    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);
    mouse_show_cursor_noflush(0);

    uint32_t move_rows = (uint32_t)(region_rows - n);
    if (move_rows > 0) {
        fb_copy_rect(0, (uint32_t)(top * CHAR_HEIGHT),
                     0, (uint32_t)((top + n) * CHAR_HEIGHT),
                     text_w, move_rows * CHAR_HEIGHT);
    }
    /* Clear the freshly-vacated rows at the bottom of the region. */
    fb_fill_rect(0, (uint32_t)((top + (int)move_rows) * CHAR_HEIGHT),
                 text_w, (uint32_t)n * CHAR_HEIGHT, bg_color);

    mouse_show_cursor_noflush(1);
    fb_flush_dirty_regions();
    spin_unlock_irqrestore(&console_lock, flags);
}

/* Scroll a region down by N rows.
 * Bottom N rows discarded; top N rows become blank. */
void console_scroll_region_down(int n, int top, int bot) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    if (n <= 0) return;
    if (top < 0) top = 0;
    if (bot >= (int)max_rows) bot = (int)max_rows - 1;
    if (top > bot) return;
    int region_rows = bot - top + 1;
    if (n > region_rows) n = region_rows;

    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    uint32_t text_w = console_text_width_pixels();
    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);
    mouse_show_cursor_noflush(0);

    uint32_t move_rows = (uint32_t)(region_rows - n);
    if (move_rows > 0) {
        fb_copy_rect(0, (uint32_t)((top + n) * CHAR_HEIGHT),
                     0, (uint32_t)(top * CHAR_HEIGHT),
                     text_w, move_rows * CHAR_HEIGHT);
    }
    fb_fill_rect(0, (uint32_t)(top * CHAR_HEIGHT),
                 text_w, (uint32_t)n * CHAR_HEIGHT, bg_color);

    mouse_show_cursor_noflush(1);
    fb_flush_dirty_regions();
    spin_unlock_irqrestore(&console_lock, flags);
}

/* Insert N blank lines at and below the cursor row, scrolling
 * lines [cursor_y..bot] down within the active region. */
void console_insert_lines(int n, int top, int bot) {
    if (n <= 0) return;
    if ((int)cursor_y < top || (int)cursor_y > bot) return;
    console_scroll_region_down(n, (int)cursor_y, bot);
}

/* Delete N lines starting at the cursor row, scrolling
 * lines below up into [cursor_y..bot]. */
void console_delete_lines(int n, int top, int bot) {
    if (n <= 0) return;
    if ((int)cursor_y < top || (int)cursor_y > bot) return;
    console_scroll_region_up(n, (int)cursor_y, bot);
}

/* Insert N blank chars at the cursor: shift current-line tail right. */
void console_insert_chars(int n) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    if (n <= 0) return;
    if (n > (int)(max_cols - cursor_x)) n = (int)(max_cols - cursor_x);

    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    uint32_t text_w = console_text_width_pixels();
    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);

    uint32_t y = cursor_y * CHAR_HEIGHT;
    uint32_t x = cursor_x * CHAR_WIDTH;
    uint32_t shift = (uint32_t)n * CHAR_WIDTH;
    if (x + shift < text_w) {
        fb_copy_rect(x + shift, y, x, y, text_w - x - shift, CHAR_HEIGHT);
    }
    fb_fill_rect(x, y, shift, CHAR_HEIGHT, bg_color);

    fb_flush_dirty_regions();
    spin_unlock_irqrestore(&console_lock, flags);
}

/* Delete N chars at the cursor: shift current-line tail left. */
void console_delete_chars(int n) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    if (n <= 0) return;
    if (n > (int)(max_cols - cursor_x)) n = (int)(max_cols - cursor_x);

    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    uint32_t text_w = console_text_width_pixels();
    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);

    uint32_t y = cursor_y * CHAR_HEIGHT;
    uint32_t x = cursor_x * CHAR_WIDTH;
    uint32_t shift = (uint32_t)n * CHAR_WIDTH;
    if (x + shift < text_w) {
        fb_copy_rect(x, y, x + shift, y, text_w - x - shift, CHAR_HEIGHT);
    }
    /* Blank the right portion that's now exposed. */
    if (text_w > shift) {
        fb_fill_rect(text_w - shift, y, shift, CHAR_HEIGHT, bg_color);
    }

    fb_flush_dirty_regions();
    spin_unlock_irqrestore(&console_lock, flags);
}

/* Erase N chars starting at the cursor position, no shifting. */
void console_erase_chars(int n) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    if (n <= 0) return;
    if (n > (int)(max_cols - cursor_x)) n = (int)(max_cols - cursor_x);

    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);

    uint32_t y = cursor_y * CHAR_HEIGHT;
    uint32_t x = cursor_x * CHAR_WIDTH;
    fb_fill_rect(x, y, (uint32_t)n * CHAR_WIDTH, CHAR_HEIGHT, bg_color);

    fb_flush_dirty_regions();
    spin_unlock_irqrestore(&console_lock, flags);
}

void console_get_dimensions(uint32_t *rows, uint32_t *cols) {
    if (rows) *rows = max_rows;
    if (cols) *cols = max_cols;
}

void console_set_prompt_guard(void) {
    console_line_t* line = sb_current_line();
    g_prompt_guard_len = line ? line->length : 0;
}

void console_clear_prompt_guard(void) {
    g_prompt_guard_len = 0;
}

// Scroll the screen up by one line.  Honors active scroll region (DECSTBM):
// when set to a sub-range of the screen, only the region scrolls so that
// status bars / pinned chrome rows below the region are preserved.
static void console_scroll_up(void) {
    if (!fb_info || !fb_info->framebuffer_base) return;

    // Region-aware path: scroll only [region_top..region_bot] if that is a
    // proper sub-range of the screen.
    if (g_region_top >= 0 && g_region_bot >= 0
        && !(g_region_top == 0 && g_region_bot == (int)max_rows - 1)) {
        int top = g_region_top, bot = g_region_bot;
        if (top < 0) top = 0;
        if (bot >= (int)max_rows) bot = (int)max_rows - 1;
        if (top > bot) return;

        mouse_show_cursor_noflush(0);

        uint32_t sb_total_w = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
        uint32_t text_w = (fb_info->horizontal_resolution > sb_total_w)
                        ? (fb_info->horizontal_resolution - sb_total_w) : 0;
        int region_rows = bot - top + 1;
        if (region_rows > 1) {
            fb_copy_rect(0, (uint32_t)(top * (int)CHAR_HEIGHT),
                         0, (uint32_t)((top + 1) * (int)CHAR_HEIGHT),
                         text_w, (uint32_t)(region_rows - 1) * CHAR_HEIGHT);
        }
        fb_fill_rect(0, (uint32_t)(bot * (int)CHAR_HEIGHT),
                     text_w, CHAR_HEIGHT, bg_color);

        cursor_y = (uint32_t)bot;
        cursor_x = 0;
        mouse_show_cursor_noflush(1);
        return;
    }

    // Full-screen scroll (original behavior — preserves scrollback semantics).
    // Hide mouse cursor before scrolling to prevent artifacts (no VRAM flush)
    mouse_show_cursor_noflush(0);

    uint32_t screen_width = fb_info->horizontal_resolution;
    uint32_t height = fb_info->vertical_resolution;
    uint32_t scroll_lines = CHAR_HEIGHT;

    // Calculate text area width (exclude scrollbar area)
    uint32_t scrollbar_total_width = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
    uint32_t text_area_width = screen_width - scrollbar_total_width;

    fb_double_buffer_t* fb_opt = get_fb_double_buffer();
    if (fb_opt) {
        fb_copy_rect(0, 0, 0, scroll_lines, text_area_width, height - scroll_lines);
        fb_fill_rect(0, height - scroll_lines, text_area_width, scroll_lines, bg_color);
    } else {
        uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
        uint32_t pixels_per_line = fb_info->pixels_per_scanline;
        for (uint32_t y = scroll_lines; y < height; y++) {
            uint32_t* src_line = &framebuffer[y * pixels_per_line];
            uint32_t* dst_line = &framebuffer[(y - scroll_lines) * pixels_per_line];
            kmemcpy(dst_line, src_line, text_area_width * sizeof(uint32_t));
        }
        for (uint32_t y = height - scroll_lines; y < height; y++) {
            uint32_t* line = &framebuffer[y * pixels_per_line];
            for (uint32_t x = 0; x < text_area_width; x++) {
                line[x] = bg_color;
            }
        }
    }

    cursor_y = max_rows - 1;
    cursor_x = 0;
    mouse_show_cursor_noflush(1);
}

// Public: set / clear the active scroll region (0-based, inclusive).
// Pass top=0, bot=max_rows-1 (or top<0) to clear back to "full screen".
void console_set_scroll_region(int top, int bot) {
    if (top < 0 || bot < 0) {
        g_region_top = -1;
        g_region_bot = -1;
        return;
    }
    if (bot >= (int)max_rows) bot = (int)max_rows - 1;
    if (top > bot) { g_region_top = -1; g_region_bot = -1; return; }
    g_region_top = top;
    g_region_bot = bot;
}

void console_get_scroll_region(int *top, int *bot) {
    if (top) *top = g_region_top;
    if (bot) *bot = g_region_bot;
}

// Set console colors (VGA compatibility)
void console_set_color(uint8_t fg, uint8_t bg) {
    current_vga_fg = fg;
    current_vga_bg = bg;
    fg_color = vga_to_rgb(fg);
    bg_color = vga_to_rgb(bg);
}

// Set console colors as direct 24-bit RGB (truecolor SGR 38;2 / 48;2).
// VGA color tracking left unchanged so a subsequent SGR reset still works.
void console_set_color_rgb(uint32_t fg_rgb, uint32_t bg_rgb) {
    fg_color = fg_rgb;
    bg_color = bg_rgb;
}

// Print a single character
// Internal unlocked version - called when console_lock is already held
static void console_putchar_unlocked(char c) {
    if (!fb_info) return;
    // Mirror to serial (if present) before screen updates
#ifdef SERIAL_ENABLED
    if (serial_is_available()) {
        serial_write_char(c);
    }
#endif
    
    // Fast path drawing when at bottom; otherwise only update scrollback/scrollbar
    if (c == '\n') {
        g_pending_wrap = 0;
        sb_append_char(c);
        if (g_sb.at_bottom) {
            // Erase cursor at old position before moving
            if (cursor_enabled && cursor_shown) {
                draw_cursor_at(cursor_x, cursor_y, 0);
            }
            /* LF advances the row.  For the kprintf direct path (g_lf_no_cr==0)
             * also reset cursor_x (ONLCR console semantics).  For the tty
             * output path (g_lf_no_cr==1) leave cursor_x unchanged — the
             * caller handles column-0 via an explicit CR or OPOST injection,
             * and bare LF must be a pure row-advance so that full-screen apps
             * using bare LF for row advancement within a pane work correctly. */
            if (!g_lf_no_cr) cursor_x = 0;
            cursor_pixels_saved = 0;
            // Determine effective bottom: scroll region bottom if active and
            // cursor is inside the region, else physical bottom row.
            int eff_bot;
            int region_active = (g_region_top >= 0 && g_region_bot >= 0
                                 && !(g_region_top == 0
                                      && g_region_bot == (int)max_rows - 1));
            if (region_active && (int)cursor_y >= g_region_top
                && (int)cursor_y <= g_region_bot) {
                eff_bot = g_region_bot;
            } else {
                eff_bot = (int)max_rows - 1;
            }
            if ((int)cursor_y >= eff_bot) {
                if (region_active && (int)cursor_y > g_region_bot) {
                    // Cursor is below the scroll region (e.g. on a status row).
                    // Do not scroll — apps should not LF off pinned rows; just
                    // clamp the carriage return.
                } else {
                    // Scroll (region-aware via console_scroll_up()).
                    console_scroll_up();
                    console_sync_scrollbar();
                }
            } else {
                cursor_y++;
            }
            cursor_last_x = cursor_x;
            cursor_last_y = cursor_y;
            cursor_shown = 0;
            cursor_blink_ticks = 0;
        } else {
            console_sync_scrollbar();
        }
        return;
    }
    if (c == '\r') {
        /* Carriage return: move cursor to column 0.  MUST NOT erase the
         * line visually — real terminals don't, and tmux/readline emit \r
         * constantly for cursor positioning.  Erasing here caused blanked
         * ls/ps output and the scattered appearance after scroll.
         *
         * sb_append_char('\r') resets write_pos to 0 without clearing the
         * visible length — subsequent writes overwrite from the start of the
         * line.  This correctly reproduces terminal overwrite semantics in
         * the scrollback so that readline reprints of a partially-typed line
         * produce the final visible content rather than accumulated garbage. */
        g_pending_wrap = 0;
        sb_append_char('\r');
        if (g_sb.at_bottom) {
            if (cursor_enabled && cursor_shown) {
                draw_cursor_at(cursor_x, cursor_y, 0);
                /* Reset cursor_shown here so that the \n handler (which runs
                 * immediately after when OPOST/ONLCR expands \n->\r\n, or
                 * when tmux sends explicit \r\n) does NOT attempt a second
                 * hide at the new column-0 position.  Without this reset the
                 * bg-colour fallback in draw_cursor_at erased the first
                 * character of the current line — the "arrow key erases char"
                 * symptom seen only inside tmux (which uses many \r\n). */
                cursor_shown = 0;
                cursor_pixels_saved = 0;
            }
            cursor_x = 0;
            cursor_last_x = 0;
        }
        return;
    }
    if (c == '\t') {
        g_pending_wrap = 0;
        // Expand to spaces in buffer and draw spaces on screen
        // Determine spaces to next tab stop (8)
        console_line_t* line = sb_current_line();
        uint16_t current_len = line->length;
        sb_append_char(c);
        if (g_sb.at_bottom) {
            uint32_t spaces = ((current_len + 8) & ~7) - current_len;
            for (uint32_t i = 0; i < spaces; ++i) {
                if (cursor_x >= max_cols) {
                    // wrap (region-aware: don't scroll the screen if the
                    // cursor is below an active DECSTBM region)
                    int region_active = (g_region_top >= 0 && g_region_bot >= 0
                                         && !(g_region_top == 0
                                              && g_region_bot == (int)max_rows - 1));
                    int eff_bot = (region_active && (int)cursor_y >= g_region_top
                                   && (int)cursor_y <= g_region_bot)
                                ? g_region_bot : (int)max_rows - 1;
                    cursor_x = 0;
                    if ((int)cursor_y >= eff_bot) {
                        if (region_active && (int)cursor_y > g_region_bot) {
                            /* below scroll region — do not scroll */
                        } else {
                            console_scroll_up();
                        }
                    } else {
                        cursor_y++;
                    }
                }
                draw_char(' ', cursor_x * CHAR_WIDTH, cursor_y * CHAR_HEIGHT, fg_color, bg_color);
                // The sb_append_char already stored attributes; nothing extra here.
                cursor_x++;
            }
        } else {
        }
        return;
    }
    if (c == '\b') {
        g_pending_wrap = 0;
        /* VT100/ANSI BS (0x08) in the OUTPUT stream is non-destructive:
         * it moves the cursor one position to the left without erasing the
         * character there.  Terminal emulators (e.g. tmux) rely on bare BS
         * to move the cursor left efficiently; destructive interpretation
         * erases the character under the cursor, producing the "left arrow
         * key erases characters" symptom inside tmux/nano.
         *
         * The shell-prompt destructive-backspace effect is achieved by the
         * VERASE echo sequence "\b \b" (BS + space + BS): the space in the
         * middle erases the character.  So this non-destructive BS path
         * still produces the correct visual result for shell prompts.
         *
         * The public console_backspace() API (used by external callers that
         * need the legacy destructive behaviour) is separate. */
        if (g_sb.at_bottom) {
            if (cursor_enabled && cursor_shown) {
                draw_cursor_at(cursor_last_x, cursor_last_y, 0);
                cursor_shown = 0;
                cursor_pixels_saved = 0;
            }
            if (cursor_x > 0) {
                cursor_x--;
            } else if (cursor_y > 0) {
                cursor_y--;
                cursor_x = max_cols - 1;
            }
            cursor_last_x = cursor_x;
            cursor_last_y = cursor_y;
        }
        return;
    }
    // Normal printable character
    sb_append_char(c);
    if (g_sb.at_bottom) {
        // If the previous char filled the last column, perform the
        // deferred wrap NOW (xenl semantics: tmux relies on this so it
        // can safely paint the bottom-right cell of the screen).
        if (g_pending_wrap) {
            g_pending_wrap = 0;
            int region_active = (g_region_top >= 0 && g_region_bot >= 0
                                 && !(g_region_top == 0
                                      && g_region_bot == (int)max_rows - 1));
            int eff_bot = (region_active && (int)cursor_y >= g_region_top
                           && (int)cursor_y <= g_region_bot)
                        ? g_region_bot : (int)max_rows - 1;
            cursor_x = 0;
            if ((int)cursor_y >= eff_bot) {
                if (region_active && (int)cursor_y > g_region_bot) {
                    /* below scroll region — do not scroll */
                } else {
                    console_scroll_up();
                }
            } else {
                cursor_y++;
            }
        }
        // Draw at current cursor cell - character will overwrite cursor
        uint32_t px = cursor_x * CHAR_WIDTH;
        uint32_t py = cursor_y * CHAR_HEIGHT;
        draw_char(c, px, py, fg_color, bg_color);
        /* Invalidate the pixel-save only when the glyph physically
         * overwrites the cursor bar (same cell).  If the glyph is at a
         * different cell the bar at cursor_saved_x/y is still intact and
         * cursor_pixels_saved must remain 1 so draw_cursor_at can restore
         * it (and so the stale-bar cleanup in draw_cursor_at show path can
         * detect and erase it).  Clearing it here unconditionally was the
         * root cause of ghost cursor bars left on screen after tmux drew
         * characters at non-cursor positions. */
        if (cursor_saved_x == cursor_x && cursor_saved_y == cursor_y)
            cursor_pixels_saved = 0;
        if (cursor_x + 1 >= max_cols) {
            // Don't wrap yet — defer.  Cursor stays glued to the last
            // column; the actual wrap happens when the next printable
            // arrives or is forcibly cleared by a cursor-motion op.
            cursor_x = max_cols - 1;
            g_pending_wrap = 1;
        } else {
            cursor_x++;
        }
        // Update cursor tracking AFTER position change so cursor_update knows position changed.
        // We did NOT draw a cursor here — we drew a glyph — so cursor_shown stays 0.
        // The blink timer will paint a real cursor on its next tick.
        if (cursor_enabled) {
            cursor_last_x = cursor_x;
            cursor_last_y = cursor_y;
            cursor_shown = 0;
            cursor_blink_ticks = 0;
        }
    } else {
    // Not at bottom: do nothing visually here
    }
}

// Public locked version - safe for external callers (flushes after every char)
void console_putchar(char c) {
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    console_putchar_unlocked(c);
    fb_flush_dirty_regions();
    spin_unlock_irqrestore(&console_lock, flags);
}

// Batch version - processes character but defers VRAM flush.
// Caller MUST call console_flush() when the batch is done.
void console_putchar_batch(char c) {
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    /* Mark LF as a pure row-advance for the tty output path.  The kprintf
     * direct path (console_putchar_unlocked called without this flag) uses
     * ONLCR semantics (LF resets cursor_x).  The tty path handles cursor_x
     * via an explicit CR from OPOST injection or CSI sequences. */
    if (c == '\n') g_lf_no_cr = 1;
    console_putchar_unlocked(c);
    g_lf_no_cr = 0;
    spin_unlock_irqrestore(&console_lock, flags);
}

// Flush deferred framebuffer changes to VRAM (rate-limited during batch output)
void console_flush(void) {
    uint64_t now = timer_ticks();
    if (now - g_last_flush_tick < CONSOLE_FLUSH_INTERVAL) {
        return;  // Skip — will be flushed by next interval or batch_end
    }
    g_last_flush_tick = now;
    fb_flush_dirty_regions();
}

// Begin batch output mode: suppresses cursor updates and enables rate-limiting
void console_batch_begin(void) {
    g_console_batch_active = 1;
}

// Return the current number of completed scrollback lines.
// Used by the alternate-screen save/restore pair.
uint32_t console_get_sb_total(void) {
    return g_sb.total_filled_lines;
}

// Restore the viewport to show the content that was visible before the
// alternate screen was entered.  saved_total must be the value returned by
// console_get_sb_total() at the time of 1049h.  Redraws the framebuffer.
void console_restore_alt_screen(uint32_t saved_total) {
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);

    // Hide cursor before touching the screen.
    if (cursor_enabled && cursor_shown)
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);
    cursor_shown = 0;
    cursor_pixels_saved = 0;

    uint32_t cap = sb_capacity();

    /* Roll the ring back to where it was when the alternate screen started.
     * Lines written during the alternate screen (nano's status bar, edited
     * content, etc.) are discarded — they were conceptually on a separate
     * buffer.  This gives a clean slate matching real terminal behaviour:
     * the pre-app content is restored and new shell output begins there. */
    if (saved_total <= g_sb.total_filled_lines) {
        uint32_t lines_to_remove = g_sb.total_filled_lines - saved_total;
        if (lines_to_remove > cap) lines_to_remove = cap;
        /* Rewind head and total_filled_lines.  The ring slots we abandon are
         * simply overwritten on the next sb_new_line(). */
        for (uint32_t i = 0; i < lines_to_remove; i++) {
            g_sb.head = (g_sb.head == 0) ? (cap - 1) : (g_sb.head - 1);
        }
        g_sb.total_filled_lines = saved_total;
    }

    /* Reset the current scrollback slot to blank.  During the alt-screen
     * session nano wrote its UI content into this slot (since CUP moves
     * cursor_x/y but never advances g_sb.head).  The slot held a fresh
     * blank line at the moment 1049h was processed, so blanking it here
     * correctly restores the pre-nano state and prevents nano's status
     * line (e.g. "[ Reading /etc/services ]") from appearing on the same
     * screen line as the restored shell prompt. */
    {
        console_line_t* cur = sb_current_line();
        cur->length = 0;
        cur->write_pos = 0;
        cur->text[0] = '\0';
        cur->fg = VGA_COLOR_WHITE;
        cur->bg = VGA_COLOR_BLACK;
        for (uint32_t ci = 0; ci < CONSOLE_MAX_LINE_LENGTH; ci++) {
            cur->fg_attrs[ci] = VGA_COLOR_WHITE;
            cur->bg_attrs[ci] = VGA_COLOR_BLACK;
        }
    }

    /* Pin the viewport to the bottom of the (now-trimmed) ring so that the
     * restored prompt is visible and new output flows normally. */
    uint32_t eff = sb_effective_total();
    uint32_t max_vp = (eff > max_rows) ? (eff - max_rows) : 0;
    g_sb.viewport_top = max_vp;
    g_sb.at_bottom = 1;
    g_prompt_guard_len = 0;

    console_render_view();
    fb_flush_dirty_regions();
    spin_unlock_irqrestore(&console_lock, flags);
}

// End batch output mode: flushes any remaining dirty content unconditionally
void console_batch_end(void) {
    g_console_batch_active = 0;
    /* If the cursor is enabled but not currently shown (e.g. because a CUP
     * sequence set cursor_shown=0 and no \033[?25h followed in this batch),
     * show it now so the user sees the cursor after every key event without
     * waiting up to 50 blink ticks. */
    if (cursor_enabled && !cursor_shown && g_sb.at_bottom) {
        uint64_t flags;
        spin_lock_irqsave(&console_lock, &flags);
        if (cursor_enabled && !cursor_shown && g_sb.at_bottom) {
            cursor_last_x = cursor_x;
            cursor_last_y = cursor_y;
            cursor_blink_ticks = 0;
            draw_cursor_at(cursor_x, cursor_y, 1);
            cursor_shown = 1;
        }
        spin_unlock_irqrestore(&console_lock, flags);
    } else if (cursor_enabled && cursor_shown) {
        /* Cursor is already shown.  Check if cursor_x/y moved since it was
         * last drawn (e.g. tmux sent 25h during the batch, then moved the
         * cursor without a 25l/25h pair).  If so, hide at old position and
         * re-draw at new position so cursor_saved_pixels always holds the
         * character pixels, not stale bar pixels. */
        uint64_t flags;
        spin_lock_irqsave(&console_lock, &flags);
        if (cursor_enabled && cursor_shown && g_sb.at_bottom &&
            (cursor_last_x != cursor_x || cursor_last_y != cursor_y)) {
            draw_cursor_at(cursor_last_x, cursor_last_y, 0);
            cursor_shown = 0;
            cursor_pixels_saved = 0;
            cursor_last_x = cursor_x;
            cursor_last_y = cursor_y;
            cursor_blink_ticks = 0;
            draw_cursor_at(cursor_x, cursor_y, 1);
            cursor_shown = 1;
        }
        spin_unlock_irqrestore(&console_lock, flags);
    }
    g_last_flush_tick = timer_ticks();
    fb_flush_dirty_regions();
}

// Print a string
void console_puts(const char* str) {
    if (!str) return;
    
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    while (*str) {
        console_putchar_unlocked(*str);
        str++;
    }
    fb_flush_dirty_regions();  // Single flush for entire string
    spin_unlock_irqrestore(&console_lock, flags);
}

// Dummy scroll function for compatibility
void console_scroll(void) {
    // legacy hook: scroll one line up in viewport
    console_scroll_up_line();
}

// Handle backspace - move cursor back and erase character
void console_backspace(void) {
    if (!fb_info) return;
    console_line_t* line = sb_current_line();
    if (line && line->length <= g_prompt_guard_len) {
        return;
    }
    // Reflect backspace in scrollback current line
    sb_append_char('\b');
    if (!g_sb.at_bottom) {
        // Do not touch the screen when scrolled up
        console_sync_scrollbar();
        return;
    }
    // Visual backspace when at bottom
    if (cursor_x == 0 && cursor_y == 0) return; // nothing to erase
    // Erase cursor at old position before moving
    if (cursor_enabled && cursor_shown) {
        draw_cursor_at(cursor_x, cursor_y, 0);
    }
    if (cursor_x > 0) {
        cursor_x--;
    } else {
        // Move to end of previous line
        cursor_y = (cursor_y > 0) ? (cursor_y - 1) : 0;
        cursor_x = (max_cols > 0) ? (max_cols - 1) : 0;
    }
    uint32_t px = cursor_x * CHAR_WIDTH;
    uint32_t py = cursor_y * CHAR_HEIGHT;
    draw_char(' ', px, py, fg_color, bg_color);
    // No flush here — deferred to caller (tty_write batch or console_putchar)
}

// ================= Cursor Control Functions =================
void console_cursor_enable(void) {
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    /* If the cursor is already visible (e.g. drawn by console_batch_end),
     * hide it first before re-saving pixels.  Without this, draw_cursor_at
     * (show) would overwrite cursor_saved_pixels with the cursor-bar pixels
     * rather than the character pixels.  A subsequent hide would then restore
     * the cursor bar instead of the character, effectively erasing it — the
     * "left arrow erases a character inside tmux" symptom. */
    if (cursor_enabled && cursor_shown) {
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);
        cursor_shown = 0;
        cursor_pixels_saved = 0;
    }
    // Update tracking to current position first
    cursor_last_x = cursor_x;
    cursor_last_y = cursor_y;
    cursor_enabled = 1;
    cursor_shown = 1;
    cursor_blink_ticks = 0;
    // Draw cursor at current position
    if (g_sb.at_bottom) {
        draw_cursor_at(cursor_x, cursor_y, 1);
        fb_flush_dirty_regions();
    }
    spin_unlock_irqrestore(&console_lock, flags);
}

void console_cursor_disable(void) {
    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    if (cursor_enabled && cursor_shown) {
        // Erase cursor at the position where it was last drawn
        draw_cursor_at(cursor_last_x, cursor_last_y, 0);
        fb_flush_dirty_regions();
    }
    cursor_enabled = 0;
    cursor_shown = 0;
    // Update tracking to current position
    cursor_last_x = cursor_x;
    cursor_last_y = cursor_y;
    spin_unlock_irqrestore(&console_lock, flags);
}

void console_cursor_update(void) {
    // Skip during batch output to prevent SMP-race mid-batch VRAM flushes
    if (g_console_batch_active) return;

    uint64_t flags;
    spin_lock_irqsave(&console_lock, &flags);
    if (!cursor_enabled || !g_sb.at_bottom) {
        spin_unlock_irqrestore(&console_lock, flags);
        return;
    }
    
    // If cursor position changed, output is actively happening.
    // Just update tracking without drawing to prevent cursor flash artifacts.
    if (cursor_last_x != cursor_x || cursor_last_y != cursor_y) {
        // Erase cursor at old position if it was visible there
        if (cursor_shown) {
            draw_cursor_at(cursor_last_x, cursor_last_y, 0);
            fb_flush_dirty_regions();
        }
        cursor_last_x = cursor_x;
        cursor_last_y = cursor_y;
        cursor_shown = 0;
        cursor_blink_ticks = 0;
        spin_unlock_irqrestore(&console_lock, flags);
        return;
    }
    
    cursor_blink_ticks++;
    
    // Blink every ~50 ticks (adjust to get desired blink rate)
    if (cursor_blink_ticks >= 50) {
        cursor_blink_ticks = 0;
        cursor_shown = !cursor_shown;
        draw_cursor_at(cursor_x, cursor_y, cursor_shown);
        fb_flush_dirty_regions();
    }
    spin_unlock_irqrestore(&console_lock, flags);
}

// ================= Scrollback public APIs =================
void console_scroll_up_line(void) {
    if (g_sb.viewport_top > 0) {
        g_sb.viewport_top--;
        g_sb.at_bottom = 0;
    mouse_show_cursor(0);
    console_render_view();
    console_sync_scrollbar();
    fb_flush_dirty_regions();
    mouse_show_cursor(1);
    }
}

void console_scroll_down_line(void) {
    uint32_t eff = sb_effective_total();
    uint32_t max_vp = (eff > g_sb.visible_lines) ? (eff - g_sb.visible_lines) : 0;
    if (g_sb.viewport_top < max_vp) {
        g_sb.viewport_top++;
        if (g_sb.viewport_top >= max_vp) g_sb.at_bottom = 1; else g_sb.at_bottom = 0;
    mouse_show_cursor(0);
    console_render_view();
    console_sync_scrollbar();
    fb_flush_dirty_regions();
    mouse_show_cursor(1);
    }
}

void console_set_viewport_top(uint32_t line) {
    uint32_t eff = sb_effective_total();
    uint32_t max_vp = (eff > g_sb.visible_lines) ? (eff - g_sb.visible_lines) : 0;
    if (line > max_vp) line = max_vp;
    g_sb.viewport_top = line;
    g_sb.at_bottom = (line >= max_vp);
    mouse_show_cursor(0);
    console_render_view();
    console_sync_scrollbar();
    fb_flush_dirty_regions();
    mouse_show_cursor(1);
}

void console_scroll_to_bottom(void) {
    if (g_sb.at_bottom) {
        return;
    }
    uint32_t eff = sb_effective_total();
    uint32_t max_vp = (eff > g_sb.visible_lines) ? (eff - g_sb.visible_lines) : 0;
    g_sb.viewport_top = max_vp;
    g_sb.at_bottom = 1;
    mouse_show_cursor(0);
    console_render_view();
    console_sync_scrollbar();
    fb_flush_dirty_regions();
    mouse_show_cursor(1);
}

void console_handle_mouse_event(int x, int y, uint8_t left_pressed) {
    scrollbar_t* sb = scrollbar_get_system();
    if (!sb) return;
    // Arrow clicks
    if (left_pressed) {
        if (scrollbar_hit_up(sb, (uint32_t)x, (uint32_t)y)) {
            console_scroll_up_line();
            return;
        }
        if (scrollbar_hit_down(sb, (uint32_t)x, (uint32_t)y)) {
            console_scroll_down_line();
            return;
        }
        // Begin drag if thumb
    if (!g_sb.dragging_thumb && scrollbar_hit_thumb(sb, (uint32_t)x, (uint32_t)y)) {
            g_sb.dragging_thumb = 1;
            g_sb.drag_start_y = y;
            g_sb.drag_start_viewport = g_sb.viewport_top;
            return;
        }
        // Track click (outside buttons and thumb): page up/down like GNOME Chrome
        // Check if inside the track area
        uint32_t track_top = sb->track_y;
        uint32_t track_bottom = sb->track_y + sb->track_height;
        if ((uint32_t)x >= sb->x && (uint32_t)x < sb->x + sb->width && (uint32_t)y >= track_top && (uint32_t)y < track_bottom) {
            // Determine page size and direction relative to thumb
            uint32_t page = (g_sb.visible_lines > 1) ? (g_sb.visible_lines - 1) : g_sb.visible_lines;
            if ((uint32_t)y < sb->thumb_y) {
                // Page up
                uint32_t new_top = (g_sb.viewport_top > page) ? (g_sb.viewport_top - page) : 0;
                console_set_viewport_top(new_top);
            } else if ((uint32_t)y >= sb->thumb_y + sb->thumb_height) {
                // Page down
                uint32_t eff = sb_effective_total();
                uint32_t max_vp = (eff > g_sb.visible_lines) ? (eff - g_sb.visible_lines) : 0;
                uint32_t add = page;
                uint32_t new_top = g_sb.viewport_top + add;
                if (new_top > max_vp) new_top = max_vp;
                console_set_viewport_top(new_top);
            }
            return;
        }
    } else {
        g_sb.dragging_thumb = 0;
    }
    // Drag update
    if (g_sb.dragging_thumb) {
        // Map delta Y to viewport range
        uint32_t track_range = (sb->track_height > sb->thumb_height) ? (sb->track_height - sb->thumb_height) : 0;
        uint32_t eff = sb_effective_total();
        uint32_t max_vp = (eff > g_sb.visible_lines) ? (eff - g_sb.visible_lines) : 0;
        if (track_range > 0 && max_vp > 0) {
            int dy = y - g_sb.drag_start_y;
            // Compute proportional change: dy/track_range * max_vp
            int d_view = (int)((int64_t)dy * (int64_t)max_vp / (int64_t)track_range);
            int new_vp = (int)g_sb.drag_start_viewport + d_view;
            if (new_vp < 0) new_vp = 0;
            if (new_vp > (int)max_vp) new_vp = (int)max_vp;
            console_set_viewport_top((uint32_t)new_vp);
        }
    }
}

void console_handle_mouse_wheel(int delta) {
    // Positive delta should scroll down; negative delta scroll up (GNOME/Chrome behavior)
    if (delta > 0) {
        uint32_t steps = (delta > 3) ? 3 : (uint32_t)delta;
        for (uint32_t i = 0; i < steps; ++i) console_scroll_down_line();
    } else if (delta < 0) {
        uint32_t steps = (-delta > 3) ? 3 : (uint32_t)(-delta);
        for (uint32_t i = 0; i < steps; ++i) console_scroll_up_line();
    }
    
    // If we're back at bottom and cursor should be visible, draw it
    if (cursor_enabled && g_sb.at_bottom) {
        cursor_shown = 1;
        cursor_blink_ticks = 0;
        cursor_last_x = cursor_x;
        cursor_last_y = cursor_y;
        draw_cursor_at(cursor_x, cursor_y, 1);
        fb_flush_dirty_regions();
    }
}

// Show framebuffer optimization status
void console_show_fb_status(void) {
    fb_print_optimization_status();
}

// Show framebuffer performance statistics
void console_show_fb_stats(void) {
    fb_print_performance_stats();
}

// String utilities for printf
size_t kstrlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

char* kstrcpy(char* dest, const char* src) {
    char* orig_dest = dest;
    while ((*dest++ = *src++));
    return orig_dest;
}

int kstrncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
        --n;
    }
    return n == (size_t)-1 ? 0 : *(unsigned char*)s1 - *(unsigned char*)s2;
}

int kstrcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

// Memory set
void* kmemset(void* ptr, int value, size_t size) {
    unsigned char* p = (unsigned char*)ptr;
    while (size--) {
        *p++ = (unsigned char)value;
    }
    return ptr;
}

// Memory copy
void* kmemcpy(void* dest, const void* src, size_t size) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (size--) {
        *d++ = *s++;
    }
    return dest;
}

// Memory compare
int kmemcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

// Convert integer to string
static char* itoa(int64_t value, char* str, int base, int is_signed) {
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    int64_t tmp_value;
    
    // Handle negative numbers for signed conversion
    if (is_signed && value < 0) {
        value = -value;
        *ptr++ = '-';
        ptr1++;
    }
    
    // Convert to string (reverse order)
    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789abcdef"[tmp_value - value * base];
    } while (value);
    
    *ptr-- = '\0';
    
    // Reverse string (except sign)
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
    
    return str;
}

// Convert unsigned integer to string
static char* utoa(uint64_t value, char* str, int base) {
    char* ptr = str;
    char* ptr1 = str;
    char tmp_char;
    uint64_t tmp_value;
    
    // Convert to string (reverse order)
    do {
        tmp_value = value;
        value /= base;
        *ptr++ = "0123456789ABCDEF"[tmp_value - value * base];
    } while (value);
    
    *ptr-- = '\0';
    
    // Reverse string
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
    
    return str;
}

// Variable arguments support using GCC built-ins
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)

// Core printf implementation  
int kvprintf(const char* format, va_list args) {
    return kvprintf_to_buffer(format, args, NULL);
}

// Internal printf implementation that can write to either console or buffer
static int kvprintf_to_buffer(const char* format, va_list args, string_buffer_t* sb) {
    int count = 0;
    
    while (*format) {
        if (*format != '%') {
            if (sb) {
                string_putchar(*format, sb);
            } else {
                console_putchar_unlocked(*format);
            }
            count++;
            format++;
            continue;
        }
        
        format++; // Skip '%'
        
        // Parse flags
        int left_align = 0;
        int pad_zero = 0;
        
        while (1) {
            switch (*format) {
                case '-': left_align = 1; format++; continue;
                case '0': pad_zero = 1; format++; continue;
                default: break;
            }
            break;
        }
        
        // Parse width
        int width = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }
        
        // Parse precision
        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }
        
        // Parse length modifiers
        int is_long = 0;
        int is_long_long = 0;
        int is_size_t = 0;
        if (*format == 'l') {
            format++;
            is_long = 1;
            if (*format == 'l') {
                format++;
                is_long_long = 1;
            }
        } else if (*format == 'z') {
            format++;
            is_size_t = 1;
        }
        
        // Handle format specifiers
        switch (*format) {
            case '%':
                if (sb) {
                    string_putchar('%', sb);
                } else {
                    console_putchar_unlocked('%');
                }
                count++;
                break;
                
            case 'c': {
                char c = (char)va_arg(args, int);
                if (sb) {
                    string_putchar(c, sb);
                } else {
                    console_putchar_unlocked(c);
                }
                count++;
                break;
            }
            
            case 's': {
                char* str = va_arg(args, char*);
                if (!str) str = "(null)";
                
                int len = kstrlen(str);
                if (precision >= 0 && len > precision) {
                    len = precision;
                }
                
                // Handle padding
                if (!left_align && width > len) {
                    for (int i = 0; i < width - len; i++) {
                        if (sb) {
                            string_putchar(' ', sb);
                        } else {
                            console_putchar_unlocked(' ');
                        }
                        count++;
                    }
                }
                
                // Print string
                for (int i = 0; i < len; i++) {
                    if (sb) {
                        string_putchar(str[i], sb);
                    } else {
                        console_putchar_unlocked(str[i]);
                    }
                    count++;
                }
                
                // Right padding
                if (left_align && width > len) {
                    for (int i = 0; i < width - len; i++) {
                        if (sb) {
                            string_putchar(' ', sb);
                        } else {
                            console_putchar_unlocked(' ');
                        }
                        count++;
                    }
                }
                break;
            }
            
            case 'd':
            case 'i': {
                int64_t value;
                if (is_long_long) {
                    value = va_arg(args, int64_t);
                } else if (is_long) {
                    value = va_arg(args, long);
                } else if (is_size_t) {
                    value = (int64_t)va_arg(args, size_t);
                } else {
                    value = va_arg(args, int);
                }
                
                char buffer[32];
                itoa(value, buffer, 10, 1);
                
                int len = kstrlen(buffer);
                
                // Handle padding
                if (!left_align && width > len) {
                    char pad_char = pad_zero ? '0' : ' ';
                    for (int i = 0; i < width - len; i++) {
                        if (sb) {
                            string_putchar(pad_char, sb);
                        } else {
                            console_putchar_unlocked(pad_char);
                        }
                        count++;
                    }
                }
                
                // Print number
                for (int i = 0; i < len; i++) {
                    if (sb) {
                        string_putchar(buffer[i], sb);
                    } else {
                        console_putchar_unlocked(buffer[i]);
                    }
                    count++;
                }
                
                // Right padding
                if (left_align && width > len) {
                    for (int i = 0; i < width - len; i++) {
                        if (sb) {
                            string_putchar(' ', sb);
                        } else {
                            console_putchar_unlocked(' ');
                        }
                        count++;
                    }
                }
                break;
            }
            
            case 'u': {
                uint64_t value;
                if (is_long_long) {
                    value = va_arg(args, uint64_t);
                } else if (is_long) {
                    value = va_arg(args, unsigned long);
                } else if (is_size_t) {
                    value = va_arg(args, size_t);
                } else {
                    value = va_arg(args, unsigned int);
                }
                
                char buffer[32];
                utoa(value, buffer, 10);
                
                int len = kstrlen(buffer);
                for (int i = 0; i < len; i++) {
                    if (sb) {
                        string_putchar(buffer[i], sb);
                    } else {
                        console_putchar_unlocked(buffer[i]);
                    }
                    count++;
                }
                break;
            }
            
            case 'x': {
                uint64_t value;
                if (is_long_long) {
                    value = va_arg(args, uint64_t);
                } else if (is_long) {
                    value = va_arg(args, unsigned long);
                } else if (is_size_t) {
                    value = va_arg(args, size_t);
                } else {
                    value = va_arg(args, unsigned int);
                }
                
                char buffer[32];
                utoa(value, buffer, 16);
                
                int len = kstrlen(buffer);
                for (int i = 0; i < len; i++) {
                    char c = buffer[i];
                    // Convert to lowercase for %x
                    if (c >= 'A' && c <= 'F') c = c - 'A' + 'a';
                    if (sb) {
                        string_putchar(c, sb);
                    } else {
                        console_putchar_unlocked(c);
                    }
                    count++;
                }
                break;
            }
            
            case 'X': {
                uint64_t value;
                if (is_long_long) {
                    value = va_arg(args, uint64_t);
                } else if (is_long) {
                    value = va_arg(args, unsigned long);
                } else if (is_size_t) {
                    value = va_arg(args, size_t);
                } else {
                    value = va_arg(args, unsigned int);
                }
                
                char buffer[32];
                utoa(value, buffer, 16);
                
                int len = kstrlen(buffer);
                for (int i = 0; i < len; i++) {
                    char c = buffer[i];
                    if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
                    if (sb) {
                        string_putchar(c, sb);
                    } else {
                        console_putchar_unlocked(c);
                    }
                    count++;
                }
                break;
            }
            
            case 'o': {
                uint64_t value;
                if (is_long_long) {
                    value = va_arg(args, uint64_t);
                } else if (is_long) {
                    value = va_arg(args, unsigned long);
                } else if (is_size_t) {
                    value = va_arg(args, size_t);
                } else {
                    value = va_arg(args, unsigned int);
                }
                
                char buffer[32];
                utoa(value, buffer, 8);
                
                int len = kstrlen(buffer);
                for (int i = 0; i < len; i++) {
                    if (sb) {
                        string_putchar(buffer[i], sb);
                    } else {
                        console_putchar_unlocked(buffer[i]);
                    }
                    count++;
                }
                break;
            }
            
            case 'p': {
                void* ptr = va_arg(args, void*);
                uint64_t value = (uint64_t)ptr;
                
                // Print "0x" prefix
                if (sb) {
                    string_putchar('0', sb);
                    string_putchar('x', sb);
                } else {
                    console_putchar_unlocked('0');
                    console_putchar_unlocked('x');
                }
                count += 2;
                
                char buffer[32];
                utoa(value, buffer, 16);
                
                int len = kstrlen(buffer);
                for (int i = 0; i < len; i++) {
                    char c = buffer[i];
                    // Convert to lowercase for pointers
                    if (c >= 'A' && c <= 'F') c = c - 'A' + 'a';
                    if (sb) {
                        string_putchar(c, sb);
                    } else {
                        console_putchar_unlocked(c);
                    }
                    count++;
                }
                break;
            }
            
            default:
                // Unknown format specifier, just print it
                if (sb) {
                    string_putchar('%', sb);
                    string_putchar(*format, sb);
                } else {
                    console_putchar_unlocked('%');
                    console_putchar_unlocked(*format);
                }
                count += 2;
                break;
        }
        
        format++;
    }
    
    return count;
}

// Main printf function
int kprintf(const char* format, ...) {
    uint64_t flags;
    char klog_tmp[1024];
    int klog_len = 0;
    int result;

    spin_lock_irqsave(&console_lock, &flags);
    
    va_list args;
    va_start(args, format);
    result = kvprintf(format, args);
    va_end(args);
    
    // Flush all dirty regions once after entire formatted string is written
    fb_flush_dirty_regions();

    spin_unlock_irqrestore(&console_lock, flags);

    if (result > 0) {
        va_list args2;
        va_start(args2, format);
        string_buffer_t sb2 = {klog_tmp, sizeof(klog_tmp), 0};
        kvprintf_to_buffer(format, args2, &sb2);
        klog_tmp[sb2.pos] = '\0';
        va_end(args2);

        klog_len = (int)sb2.pos;
        klog_append(klog_tmp, klog_len);
        usbserial_log_write(klog_tmp, (uint32_t)klog_len);
    }

    return result;
}

// String printf
int ksprintf(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    string_buffer_t sb = {buffer, SIZE_MAX, 0};
    int result = kvprintf_to_buffer(format, args, &sb);
    buffer[sb.pos] = '\0';
    
    va_end(args);
    return result;
}

// String printf with size limit
int ksnprintf(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    if (size == 0) {
        va_end(args);
        return 0;
    }
    
    string_buffer_t sb = {buffer, size, 0};
    int result = kvprintf_to_buffer(format, args, &sb);
    buffer[sb.pos] = '\0';
    
    va_end(args);
    return result;
}

// ============================================================================
// KERNEL LOG RING BUFFER (for dmesg)
// ============================================================================

static char g_klog_buffer[KLOG_BUF_SIZE];
static uint32_t g_klog_head = 0;      // Next write position
static uint32_t g_klog_count = 0;     // Number of valid characters
static spinlock_t g_klog_lock = SPINLOCK_INIT("klog");

void klog_append(const char *str, int len) {
    // Note: caller may already hold console_lock; klog_lock must be separate
    uint64_t flags;
    spin_lock_irqsave(&g_klog_lock, &flags);
    for (int i = 0; i < len; i++) {
        g_klog_buffer[g_klog_head] = str[i];
        g_klog_head = (g_klog_head + 1) % KLOG_BUF_SIZE;
        if (g_klog_count < KLOG_BUF_SIZE)
            g_klog_count++;
    }
    spin_unlock_irqrestore(&g_klog_lock, flags);
}

int klog_read(char *buf, int size) {
    uint64_t flags;
    spin_lock_irqsave(&g_klog_lock, &flags);
    int count = (int)g_klog_count;
    if (size < count)
        count = size;
    // Calculate start position in ring buffer
    uint32_t start;
    if (g_klog_count < KLOG_BUF_SIZE) {
        start = 0;
    } else {
        start = g_klog_head;  // oldest data is at head (was overwritten)
    }
    // Skip to read only the last 'count' chars
    uint32_t skip = g_klog_count - (uint32_t)count;
    uint32_t pos = (start + skip) % KLOG_BUF_SIZE;
    for (int i = 0; i < count; i++) {
        buf[i] = g_klog_buffer[pos];
        pos = (pos + 1) % KLOG_BUF_SIZE;
    }
    spin_unlock_irqrestore(&g_klog_lock, flags);
    return count;
}

int klog_read_clear(char *buf, int size) {
    uint64_t flags;
    spin_lock_irqsave(&g_klog_lock, &flags);
    int count = (int)g_klog_count;
    if (size < count)
        count = size;
    uint32_t start;
    if (g_klog_count < KLOG_BUF_SIZE) {
        start = 0;
    } else {
        start = g_klog_head;
    }
    uint32_t skip = g_klog_count - (uint32_t)count;
    uint32_t pos = (start + skip) % KLOG_BUF_SIZE;
    for (int i = 0; i < count; i++) {
        buf[i] = g_klog_buffer[pos];
        pos = (pos + 1) % KLOG_BUF_SIZE;
    }
    g_klog_count = 0;
    g_klog_head = 0;
    spin_unlock_irqrestore(&g_klog_lock, flags);
    return count;
}

void klog_clear(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_klog_lock, &flags);
    g_klog_count = 0;
    g_klog_head = 0;
    spin_unlock_irqrestore(&g_klog_lock, flags);
}

int klog_size(void) {
    return (int)g_klog_count;
}
