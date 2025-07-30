// LikeOS-64 Kernel Printf Header
// Full-featured printf implementation for 64-bit kernel

#ifndef KPRINTF_H
#define KPRINTF_H

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

// VGA console interface
void console_init(void);
void console_clear(void);
void console_putchar(char c);
void console_puts(const char* str);
void console_set_color(uint8_t fg, uint8_t bg);
void console_scroll(void);

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

#endif // KPRINTF_H
