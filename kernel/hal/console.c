// LikeOS-64 Hardware Abstraction Layer - Console
// Framebuffer-based console and printf implementation for 64-bit kernel

#include "../../include/kernel/console.h"

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

// Draw a single pixel (32-bit BGRA format)
static void set_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    if (x >= fb_info->horizontal_resolution || y >= fb_info->vertical_resolution) return;
    
    uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
    uint32_t pixel_offset = y * fb_info->pixels_per_scanline + x;
    
    framebuffer[pixel_offset] = color;
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
    
    if (fb_info) {
        max_cols = fb_info->horizontal_resolution / CHAR_WIDTH;
        max_rows = fb_info->vertical_resolution / CHAR_HEIGHT;
    } else {
        // Fallback values
        max_cols = 80;
        max_rows = 25;
    }
    
    console_clear();
}

// Clear the entire screen
void console_clear(void) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    
    // Clear framebuffer to background color
    uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
    uint32_t total_pixels = fb_info->horizontal_resolution * fb_info->vertical_resolution;
    
    for (uint32_t i = 0; i < total_pixels; i++) {
        framebuffer[i] = bg_color;
    }
    
    cursor_x = 0;
    cursor_y = 0;
}

// Scroll the screen up by one line
static void console_scroll_up(void) {
    if (!fb_info || !fb_info->framebuffer_base) return;
    
    uint32_t* framebuffer = (uint32_t*)fb_info->framebuffer_base;
    uint32_t width = fb_info->horizontal_resolution;
    uint32_t height = fb_info->vertical_resolution;
    uint32_t pixels_per_line = fb_info->pixels_per_scanline;
    
    // Calculate how many pixels to scroll (one character line)
    uint32_t scroll_lines = CHAR_HEIGHT;
    
    // Move all scan lines up by scroll_lines using memory copy for efficiency
    for (uint32_t y = scroll_lines; y < height; y++) {
        uint32_t* src_line = &framebuffer[y * pixels_per_line];
        uint32_t* dst_line = &framebuffer[(y - scroll_lines) * pixels_per_line];
        kmemcpy(dst_line, src_line, width * sizeof(uint32_t));
    }
    
    // Clear the bottom scroll_lines rows with background color
    for (uint32_t y = height - scroll_lines; y < height; y++) {
        uint32_t* line = &framebuffer[y * pixels_per_line];
        for (uint32_t x = 0; x < width; x++) {
            line[x] = bg_color;
        }
    }
    
    // Move cursor to last line
    cursor_y = max_rows - 1;
    cursor_x = 0;
}

// Set console colors (VGA compatibility)
void console_set_color(uint8_t fg, uint8_t bg) {
    fg_color = vga_to_rgb(fg);
    bg_color = vga_to_rgb(bg);
}

// Print a single character
void console_putchar(char c) {
    if (!fb_info) return;
    
    // Handle special characters
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= max_rows) {
            console_scroll_up();
        }
        return;
    }
    
    if (c == '\r') {
        cursor_x = 0;
        return;
    }
    
    if (c == '\b') {
        console_backspace();
        return;
    }
    
    if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~7; // Tab to next 8-character boundary
        if (cursor_x >= max_cols) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= max_rows) {
                console_scroll_up();
            }
        }
        return;
    }
    
    // Draw the character
    uint32_t pixel_x = cursor_x * CHAR_WIDTH;
    uint32_t pixel_y = cursor_y * CHAR_HEIGHT;
    
    draw_char(c, pixel_x, pixel_y, fg_color, bg_color);
    
    // Advance cursor
    cursor_x++;
    if (cursor_x >= max_cols) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= max_rows) {
            console_scroll_up();
        }
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
    console_scroll_up();
}

// Handle backspace - move cursor back and erase character
void console_backspace(void) {
    if (!fb_info) return;
    
    // Can't backspace if we're at the beginning of the first line
    if (cursor_x == 0 && cursor_y == 0) return;
    
    // Move cursor back
    if (cursor_x > 0) {
        cursor_x--;
    } else {
        // Wrap to previous line
        cursor_y--;
        cursor_x = max_cols - 1;
    }
    
    // Erase the character at the current cursor position
    uint32_t pixel_x = cursor_x * CHAR_WIDTH;
    uint32_t pixel_y = cursor_y * CHAR_HEIGHT;
    
    draw_char(' ', pixel_x, pixel_y, fg_color, bg_color);
}

// String utilities for printf
size_t kstrlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
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
        if (*format == 'l') {
            format++;
            is_long = 1;
            if (*format == 'l') {
                format++;
                is_long_long = 1;
            }
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
