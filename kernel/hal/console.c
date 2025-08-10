// LikeOS-64 Hardware Abstraction Layer - Console
// Framebuffer-based console and printf implementation for 64-bit kernel

#include "../../include/kernel/console.h"
#include "../../include/kernel/serial.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/scrollbar.h"

#define SIZE_MAX ((size_t)-1)
#define NULL ((void*)0)

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
static framebuffer_info_t* fb_info = 0;
static uint32_t fg_color = 0xFFFFFFFF; // White
static uint32_t bg_color = 0x00000000; // Black

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

// Character dimensions
#define CHAR_WIDTH 8
#define CHAR_HEIGHT 16

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
static uint32_t vga_to_rgb(uint8_t vga_color) {
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

static inline uint32_t sb_capacity(void) { return CONSOLE_SCROLLBACK_LINES; }

static inline uint32_t sb_visible_lines(void) { return max_rows; }

static uint32_t sb_effective_total(void) {
    // effective total lines available to view (min(total_filled, capacity))
    uint32_t cap = sb_capacity();
    return (g_sb.total_filled_lines < cap) ? g_sb.total_filled_lines : cap;
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
    // Zero first line
    for (uint32_t i = 0; i < sb_capacity(); ++i) {
        g_lines[i].length = 0;
        g_lines[i].text[0] = '\0';
        g_lines[i].fg = VGA_COLOR_WHITE;
        g_lines[i].bg = VGA_COLOR_BLACK;
    }
}

static console_line_t* sb_current_line(void) { return &g_sb.lines[g_sb.head]; }

static void sb_new_line(void) {
    // Advance head in ring
    g_sb.head = (g_sb.head + 1) % sb_capacity();
    if (g_sb.total_filled_lines < 0xFFFFFFFFu) g_sb.total_filled_lines++;
    // Clear the new line
    g_sb.lines[g_sb.head].length = 0;
    g_sb.lines[g_sb.head].text[0] = '\0';
    g_sb.lines[g_sb.head].fg = VGA_COLOR_WHITE;
    g_sb.lines[g_sb.head].bg = VGA_COLOR_BLACK;
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
        // carriage return: reset line length to 0 (simple behavior)
        line->length = 0;
        line->text[0] = '\0';
        return;
    }
    if (c == '\t') {
        // simple tab expansion: up to next 8-char boundary using spaces
        uint16_t next = (uint16_t)(((line->length + 8) & ~7) - line->length);
        for (uint16_t i = 0; i < next && line->length < CONSOLE_MAX_LINE_LENGTH - 1; ++i) {
            line->text[line->length++] = ' ';
        }
        line->text[line->length] = '\0';
        return;
    }
    if (c == '\b') {
        if (line->length > 0) {
            line->length--;
            line->text[line->length] = '\0';
        }
        return;
    }
    if (line->length < CONSOLE_MAX_LINE_LENGTH - 1) {
        line->text[line->length++] = c;
        line->text[line->length] = '\0';
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
            draw_char(line->text[col], col * CHAR_WIDTH, y, fg_color, bg_color);
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

// Draw a character at specific position
static void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) uc = '?'; // Default for unknown chars
    
    const uint8_t* char_bitmap = font_8x16[uc];
    
    for (int row = 0; row < CHAR_HEIGHT; row++) {
        uint8_t line = char_bitmap[row];
        for (int col = 0; col < CHAR_WIDTH; col++) {
            uint32_t pixel_x = x + col;
            uint32_t pixel_y = y + row;
            
            if (line & (0x80 >> col)) {
                set_pixel(pixel_x, pixel_y, fg_color);
            } else {
                set_pixel(pixel_x, pixel_y, bg_color);
            }
        }
    }
}

// Initialize the console with framebuffer info
void console_init(framebuffer_info_t* fb) {
    fb_info = fb;
    cursor_x = 0;
    cursor_y = 0;
    fg_color = 0xFFFFFFFF; // White
    bg_color = 0x00000000; // Black
    // Initialize serial console early so kprintf mirrors to QEMU if present
    serial_init();
    
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

// Initialize framebuffer optimization system (call after console_init)
void console_init_fb_optimization(void) {
    if (!fb_info) {
        kprintf("Console: No framebuffer available for optimization\n");
        return;
    }
    
    // Initialize framebuffer optimization system
    if (fb_optimize_init(fb_info) == 0) {
        kprintf("Console: Framebuffer optimization enabled\n");
    } else {
        kprintf("Console: Using direct framebuffer (no optimization)\n");
    }
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
    // reset viewport to top as well
    g_sb.viewport_top = 0;
    g_sb.at_bottom = 1;
}

// Scroll the screen up by one line
static void console_scroll_up(void) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    
    // Hide mouse cursor before scrolling to prevent artifacts
    mouse_show_cursor(0);
    
    uint32_t screen_width = fb_info->horizontal_resolution;
    uint32_t height = fb_info->vertical_resolution;
    uint32_t scroll_lines = CHAR_HEIGHT;
    
    // Calculate text area width (exclude scrollbar area)
    uint32_t scrollbar_total_width = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
    uint32_t text_area_width = screen_width - scrollbar_total_width;
    
    fb_double_buffer_t* fb_opt = get_fb_double_buffer();
    if (fb_opt) {
        // Use optimized copy operation - only scroll the text area
        fb_copy_rect(0, 0, 0, scroll_lines, text_area_width, height - scroll_lines);
        
        // Clear the bottom area of text region only
        fb_fill_rect(0, height - scroll_lines, text_area_width, scroll_lines, bg_color);
        
        // Flush changes to screen
        fb_flush_dirty_regions();
    } else {
        // Fallback to direct framebuffer access
        uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
        uint32_t pixels_per_line = fb_info->pixels_per_scanline;
        
        // Move all scan lines up by scroll_lines using memory copy for efficiency
        // Only copy the text area, not the scrollbar area
        for (uint32_t y = scroll_lines; y < height; y++) {
            uint32_t* src_line = &framebuffer[y * pixels_per_line];
            uint32_t* dst_line = &framebuffer[(y - scroll_lines) * pixels_per_line];
            kmemcpy(dst_line, src_line, text_area_width * sizeof(uint32_t));
        }
        
        // Clear the bottom scroll_lines rows with background color (text area only)
        for (uint32_t y = height - scroll_lines; y < height; y++) {
            uint32_t* line = &framebuffer[y * pixels_per_line];
            for (uint32_t x = 0; x < text_area_width; x++) {
                line[x] = bg_color;
            }
        }
    }
    
    // Move cursor to last line
    cursor_y = max_rows - 1;
    cursor_x = 0;
    
    // Show mouse cursor again after scrolling is complete
    mouse_show_cursor(1);
}

// Set console colors (VGA compatibility)
void console_set_color(uint8_t fg, uint8_t bg) {
    fg_color = vga_to_rgb(fg);
    bg_color = vga_to_rgb(bg);
}

// Print a single character
void console_putchar(char c) {
    if (!fb_info) return;
    // Mirror to serial (if present) before screen updates
    if (serial_is_available()) {
        serial_write_char(c);
    }
    // Fast path drawing when at bottom; otherwise only update scrollback/scrollbar
    if (c == '\n') {
        sb_append_char(c);
        if (g_sb.at_bottom) {
            cursor_x = 0;
            // Hide cursor, scroll text area, sync scrollbar, show cursor
            mouse_show_cursor(0);
            console_scroll_up();
            console_sync_scrollbar();
            fb_flush_dirty_regions();
            mouse_show_cursor(1);
        } else {
            console_sync_scrollbar();
        }
        return;
    }
    if (c == '\r') {
        sb_append_char(c);
        if (g_sb.at_bottom) {
            // Clear current line and move to column 0
            uint32_t sb_total_w = SCROLLBAR_DEFAULT_WIDTH + SCROLLBAR_MARGIN;
            uint32_t text_w = (fb_info->horizontal_resolution > sb_total_w) ? (fb_info->horizontal_resolution - sb_total_w) : 0;
            uint32_t y = cursor_y * CHAR_HEIGHT;
            fb_fill_rect(0, y, text_w, CHAR_HEIGHT, bg_color);
            fb_mark_dirty(0, y, text_w ? (text_w - 1) : 0, y + CHAR_HEIGHT - 1);
            fb_flush_dirty_regions();
            cursor_x = 0;
        }
        return;
    }
    if (c == '\t') {
        // Expand to spaces in buffer and draw spaces on screen
        // Determine spaces to next tab stop (8)
        console_line_t* line = sb_current_line();
        uint16_t current_len = line->length;
        sb_append_char(c);
        if (g_sb.at_bottom) {
            uint32_t spaces = ((current_len + 8) & ~7) - current_len;
            for (uint32_t i = 0; i < spaces; ++i) {
                if (cursor_x >= max_cols) {
                    // wrap
                    cursor_x = 0;
                    console_scroll_up();
                }
                draw_char(' ', cursor_x * CHAR_WIDTH, cursor_y * CHAR_HEIGHT, fg_color, bg_color);
                cursor_x++;
            }
            fb_flush_dirty_regions();
        } else {
        }
        return;
    }
    if (c == '\b') {
        // Backspace handled by console_backspace
        console_backspace();
        return;
    }
    // Normal printable character
    sb_append_char(c);
    if (g_sb.at_bottom) {
        // Draw at current cursor cell
        uint32_t px = cursor_x * CHAR_WIDTH;
        uint32_t py = cursor_y * CHAR_HEIGHT;
        draw_char(c, px, py, fg_color, bg_color);
        fb_flush_dirty_regions();
        cursor_x++;
        if (cursor_x >= max_cols) {
            // Wrap to new line: scroll up and reset cursor_x
            cursor_x = 0;
            console_scroll_up();
            fb_flush_dirty_regions();
        }
    } else {
    // Not at bottom: do nothing visually here
    }
}

// Print a string
void console_puts(const char* str) {
    if (!str) return;
    
    while (*str) {
        console_putchar(*str);
        str++;
    }
}

// Dummy scroll function for compatibility
void console_scroll(void) {
    // legacy hook: scroll one line up in viewport
    console_scroll_up_line();
}

// Handle backspace - move cursor back and erase character
void console_backspace(void) {
    if (!fb_info) return;
    // Reflect backspace in scrollback current line
    sb_append_char('\b');
    if (!g_sb.at_bottom) {
        // Do not touch the screen when scrolled up
        console_sync_scrollbar();
        return;
    }
    // Visual backspace when at bottom
    if (cursor_x == 0 && cursor_y == 0) return; // nothing to erase
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
    fb_flush_dirty_regions();
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
    if (delta > 0) {
        // wheel up: scroll up a few lines
        uint32_t steps = (delta > 3) ? 3 : (uint32_t)delta;
        for (uint32_t i = 0; i < steps; ++i) console_scroll_up_line();
    } else if (delta < 0) {
        uint32_t steps = (-delta > 3) ? 3 : (uint32_t)(-delta);
        for (uint32_t i = 0; i < steps; ++i) console_scroll_down_line();
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
                console_putchar(*format);
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
                    console_putchar('%');
                }
                count++;
                break;
                
            case 'c': {
                char c = (char)va_arg(args, int);
                if (sb) {
                    string_putchar(c, sb);
                } else {
                    console_putchar(c);
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
                            console_putchar(' ');
                        }
                        count++;
                    }
                }
                
                // Print string
                for (int i = 0; i < len; i++) {
                    if (sb) {
                        string_putchar(str[i], sb);
                    } else {
                        console_putchar(str[i]);
                    }
                    count++;
                }
                
                // Right padding
                if (left_align && width > len) {
                    for (int i = 0; i < width - len; i++) {
                        if (sb) {
                            string_putchar(' ', sb);
                        } else {
                            console_putchar(' ');
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
                            console_putchar(pad_char);
                        }
                        count++;
                    }
                }
                
                // Print number
                for (int i = 0; i < len; i++) {
                    if (sb) {
                        string_putchar(buffer[i], sb);
                    } else {
                        console_putchar(buffer[i]);
                    }
                    count++;
                }
                
                // Right padding
                if (left_align && width > len) {
                    for (int i = 0; i < width - len; i++) {
                        if (sb) {
                            string_putchar(' ', sb);
                        } else {
                            console_putchar(' ');
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
                        console_putchar(buffer[i]);
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
                        console_putchar(c);
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
                        console_putchar(c);
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
                        console_putchar(buffer[i]);
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
                    console_putchar('0');
                    console_putchar('x');
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
                        console_putchar(c);
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
                    console_putchar('%');
                    console_putchar(*format);
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
    va_list args;
    va_start(args, format);
    int result = kvprintf(format, args);
    va_end(args);
    return result;
}

// Serial-only printf (does not render to framebuffer)
int kprintf_serial(const char* format, ...) {
    // If serial not available, do nothing (return bytes that would be written)
    // We still format into a temporary buffer to compute length when serial is present.
    if (!serial_is_available()) {
        // Quick path: count approximate length by formatting into a small scratch buffer
        // Without vsnprintf, we can’t compute exact length cheaply; return 0 in this case.
        return 0;
    }

    // Format into a temporary buffer using existing string formatter
    // Choose a reasonable size for kernel logs
    char buf[1024];
    string_buffer_t sb = { buf, sizeof(buf), 0 };
    va_list args;
    va_start(args, format);
    kvprintf_to_buffer(format, args, &sb);
    va_end(args);
    buf[sb.pos] = '\0';

    if (sb.pos > 0) {
        serial_write(buf, (uint32_t)sb.pos);
    }
    return (int)sb.pos;
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
