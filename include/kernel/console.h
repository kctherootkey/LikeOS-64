// LikeOS-64 Hardware Abstraction Layer - Console Interface
// Framebuffer-based console and printf services for kernel subsystems

#ifndef _KERNEL_CONSOLE_H_
#define _KERNEL_CONSOLE_H_

// Standard types for 64-bit kernel
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef char int8_t;
typedef unsigned long size_t;

// Framebuffer information structure (must match bootloader)
typedef struct {
    void* framebuffer_base;
    uint32_t framebuffer_size;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixels_per_scanline;
    uint32_t bytes_per_pixel;
} framebuffer_info_t;

// ================= Scrollback configuration and API =================
// Capacity: 10,000 lines. Each line has a fixed character capacity to avoid heap use.
#define CONSOLE_SCROLLBACK_LINES 10000
#define CONSOLE_MAX_LINE_LENGTH  256

// Ring-buffer line
typedef struct {
    char     text[CONSOLE_MAX_LINE_LENGTH];
    uint16_t length;     // visible length: highest write position ever reached
    uint16_t write_pos;  // current write offset (reset to 0 by CR for overwrite)
    uint8_t  fg;         // legacy line-level fg (first char or last written)
    uint8_t  bg;         // legacy line-level bg
    // Per-character attributes to preserve color across scrolling
    uint8_t  fg_attrs[CONSOLE_MAX_LINE_LENGTH];
    uint8_t  bg_attrs[CONSOLE_MAX_LINE_LENGTH];
} console_line_t;

// Scrollback state
typedef struct {
    console_line_t* lines;        // storage (pre-allocated, capacity CONSOLE_SCROLLBACK_LINES)
    uint32_t head;                // index of the current write line in ring [0..LINES-1]
    uint32_t total_filled_lines;  // total lines ever written (monotonic)

    // Viewport
    uint32_t viewport_top;        // first visible logical line (0 = oldest)
    uint32_t visible_lines;       // count of lines visible on screen (rows)
    uint8_t  at_bottom;           // 1 if viewport is pinned to bottom

    // Dragging state for scrollbar thumb
    uint8_t  dragging_thumb;      // 1 while dragging thumb
    int      drag_start_y;        // mouse Y at drag start
    uint32_t drag_start_viewport; // viewport_top at drag start
} console_scrollback_t;

// Public APIs for scrollback-driven viewport control
void console_scroll_up_line(void);
void console_scroll_down_line(void);
void console_set_viewport_top(uint32_t line);
void console_scroll_to_bottom(void);
void console_handle_mouse_event(int x, int y, uint8_t left_pressed);
void console_handle_mouse_wheel(int delta);

// Console interface (now framebuffer-based with optimization)
void console_init(framebuffer_info_t* fb_info);
void console_init_fb_optimization(void);
void console_apply_sysfont(void);  // Apply loaded system font and redraw screen
void console_clear(void);
void console_putchar(char c);
void console_putchar_batch(char c);  // No VRAM flush — call console_flush() when done
void console_flush(void);            // Rate-limited VRAM flush for batch output
void console_batch_begin(void);      // Begin batch mode (suppresses cursor updates)
void console_batch_end(void);        // End batch mode (unconditional final flush)
uint32_t console_get_sb_total(void); // Snapshot scrollback line count (for alt-screen save)
void console_restore_alt_screen(uint32_t saved_total); // Restore pre-alt-screen viewport
void console_puts(const char* str);
void console_set_color(uint8_t fg, uint8_t bg);
void console_scroll(void);
void console_backspace(void);
void console_set_prompt_guard(void);
void console_clear_prompt_guard(void);

// ANSI CSI support: cursor positioning and erase operations
void console_set_cursor_pos(uint32_t row, uint32_t col);
void console_get_cursor_pos(uint32_t *row, uint32_t *col);
void console_erase_display(int mode);   // 0=below, 1=above, 2=all
void console_erase_line(int mode);      // 0=to end, 1=to start, 2=whole line

// VT-style line / character editing (used by terminal emulators like tmux).
void console_erase_chars(int n);                    // ECH:    erase N chars from cursor (in place)
void console_insert_lines(int n, int top, int bot); // IL:     scroll [cursor..bot] down by N
void console_delete_lines(int n, int top, int bot); // DL:     scroll [cursor..bot] up by N
void console_insert_chars(int n);                   // ICH:    shift chars right from cursor
void console_delete_chars(int n);                   // DCH:    shift chars left at cursor
void console_scroll_region_up(int n, int top, int bot);   // SU:  scroll region [top..bot] up
void console_scroll_region_down(int n, int top, int bot); // SD:  scroll region [top..bot] down

// Active scroll region (DECSTBM).  Pass top<0 or bot<0 to clear it.
// Coordinates are 0-based, inclusive.  Honored by the implicit \\n / wrap
// scroll path so that pinned status rows below the region are preserved.
void console_set_scroll_region(int top, int bot);
void console_get_scroll_region(int *top, int *bot);

// Set color using direct RGB values (used for 24-bit truecolor SGR).
void console_set_color_rgb(uint32_t fg_rgb, uint32_t bg_rgb);

// Query console dimensions (actual rows/cols based on framebuffer and font)
void console_get_dimensions(uint32_t *rows, uint32_t *cols);

// Cursor control functions
void console_cursor_enable(void);
void console_cursor_disable(void);
void console_cursor_update(void);  // Call periodically to handle blinking

// Framebuffer optimization interface
void console_show_fb_status(void);
void console_show_fb_stats(void);

// Printf family functions
typedef __builtin_va_list va_list;
int kprintf(const char* format, ...);
int ksprintf(char* buffer, const char* format, ...);
int ksnprintf(char* buffer, size_t size, const char* format, ...);
int kvprintf(const char* format, va_list args);

// String utility functions
size_t kstrlen(const char* str);
char* kstrcpy(char* dest, const char* src);
char* kstrncpy(char* dest, const char* src, size_t n);
int kstrcmp(const char* s1, const char* s2);
int kstrncmp(const char* s1, const char* s2, size_t n);

// Memory utility functions
void* kmemset(void* ptr, int value, size_t size);
void* kmemcpy(void* dest, const void* src, size_t size);
int kmemcmp(const void* s1, const void* s2, size_t n);

// Remap framebuffer to use direct map (call before removing identity mapping)
void console_remap_to_direct_map(void);

// VGA colors
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

// ============================================================================
// Kernel log ring buffer (for dmesg)
// ============================================================================
#define KLOG_BUF_SIZE  (256 * 1024)  // 256 KB ring buffer

void klog_append(const char *str, int len);
int klog_read(char *buf, int size);
int klog_read_clear(char *buf, int size);
void klog_clear(void);
int klog_size(void);

#endif // _KERNEL_CONSOLE_H_
