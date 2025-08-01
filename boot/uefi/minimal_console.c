#include "minimal_console.h"

// Global console state
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;

// Initialize the console
void console_init(void) {
    cursor_x = 0;
    cursor_y = 0;
    clear_screen();
}

// Clear the entire screen
void clear_screen(void) {
    uint16_t blank = (VGA_COLOR_WHITE << 8) | ' ';
    
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = blank;
    }
    
    cursor_x = 0;
    cursor_y = 0;
}

// Scroll the screen up by one line
static void scroll_up(void) {
    uint16_t blank = (VGA_COLOR_WHITE << 8) | ' ';
    
    // Move all lines up
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        vga_buffer[i] = vga_buffer[i + VGA_WIDTH];
    }
    
    // Clear the last line
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        vga_buffer[i] = blank;
    }
    
    cursor_y = VGA_HEIGHT - 1;
}

// Print a single character to the screen
void print_char(char c) {
    if (c == '\n') {
        // Newline
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        // Carriage return
        cursor_x = 0;
    } else if (c == '\t') {
        // Tab (4 spaces)
        cursor_x = (cursor_x + 4) & ~3;
    } else if (c >= ' ') {
        // Printable character
        uint16_t position = cursor_y * VGA_WIDTH + cursor_x;
        vga_buffer[position] = (VGA_COLOR_WHITE << 8) | c;
        cursor_x++;
    }
    
    // Handle line wrap
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }
    
    // Handle screen scroll
    if (cursor_y >= VGA_HEIGHT) {
        scroll_up();
    }
}

// Print a null-terminated string
void print_string(const char* str) {
    if (!str) return;
    
    while (*str) {
        print_char(*str);
        str++;
    }
}
