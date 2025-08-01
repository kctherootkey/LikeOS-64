#ifndef MINIMAL_CONSOLE_H
#define MINIMAL_CONSOLE_H

// Minimal console interface for UEFI kernel
// Simple VGA text mode output functions

// VGA text mode constants
#define VGA_MEMORY 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_COLOR_WHITE 0x0F

// Basic types
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

// Console functions
void console_init(void);
void print_char(char c);
void print_string(const char* str);
void clear_screen(void);

#endif // MINIMAL_CONSOLE_H
