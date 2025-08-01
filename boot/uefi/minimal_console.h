#ifndef MINIMAL_CONSOLE_H
#define MINIMAL_CONSOLE_H

// Minimal console interface for UEFI kernel
// Framebuffer-based graphics output

// Basic types
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

// Framebuffer information structure (must match bootloader)
typedef struct {
    void* framebuffer_base;
    uint32_t framebuffer_size;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixels_per_scanline;
    uint32_t bytes_per_pixel;
} framebuffer_info_t;

// Console functions
void console_init(framebuffer_info_t* fb_info);
void print_char(char c);
void print_string(const char* str);
void clear_screen(void);

#endif // MINIMAL_CONSOLE_H
