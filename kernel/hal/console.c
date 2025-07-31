// LikeOS-64 Hardware Abstraction Layer - Console
// VGA text mode console and printf implementation for 64-bit kernel

#include "../../include/kernel/console.h"

#define SIZE_MAX ((size_t)-1)
#define NULL ((void*)0)

// I/O port functions for VGA cursor control
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// VGA text mode buffer
#define VGA_BUFFER ((volatile uint16_t*)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

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

// Console state
static struct {
    int cursor_x;
    int cursor_y;
    uint8_t color;
} console = {0, 0, 0x0F}; // White on black

// Update hardware cursor position
static void console_update_cursor(void) {
    uint16_t pos = console.cursor_y * VGA_WIDTH + console.cursor_x;
    
    // Cursor low byte to VGA register
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    
    // Cursor high byte to VGA register  
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

// Enable hardware cursor
static void console_enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | cursor_start);
    
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | cursor_end);
}

// Disable hardware cursor
static void console_disable_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

// Initialize console
void console_init(void) {
    console.cursor_x = 0;
    console.cursor_y = 0;
    console.color = 0x0F; // White on black
    console_clear();
    
    // Enable hardware cursor (underline style: start=14, end=15)
    console_enable_cursor(14, 15);
    console_update_cursor();
}

// Clear screen
void console_clear(void) {
    uint16_t blank = (console.color << 8) | ' ';
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = blank;
    }
    console.cursor_x = 0;
    console.cursor_y = 0;
    console_update_cursor();
}

// Set console colors
void console_set_color(uint8_t fg, uint8_t bg) {
    console.color = (bg << 4) | (fg & 0x0F);
}

// Scroll screen up one line
void console_scroll(void) {
    uint16_t blank = (console.color << 8) | ' ';
    
    // Move all lines up
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        VGA_BUFFER[i] = VGA_BUFFER[i + VGA_WIDTH];
    }
    
    // Clear bottom line
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        VGA_BUFFER[i] = blank;
    }
    
    console.cursor_y = VGA_HEIGHT - 1;
    console_update_cursor();
}

// Put character to console
void console_putchar(char c) {
    if (c == '\n') {
        console.cursor_x = 0;
        console.cursor_y++;
    } else if (c == '\r') {
        console.cursor_x = 0;
    } else if (c == '\t') {
        console.cursor_x = (console.cursor_x + 8) & ~7;
    } else if (c == '\b') {
        if (console.cursor_x > 0) {
            console.cursor_x--;
        }
    } else if (c >= ' ') {
        uint16_t attr_char = (console.color << 8) | c;
        VGA_BUFFER[console.cursor_y * VGA_WIDTH + console.cursor_x] = attr_char;
        console.cursor_x++;
    }
    
    // Handle line wrapping
    if (console.cursor_x >= VGA_WIDTH) {
        console.cursor_x = 0;
        console.cursor_y++;
    }
    
    // Handle scrolling
    if (console.cursor_y >= VGA_HEIGHT) {
        console_scroll();
    }
    
    // Update hardware cursor position
    console_update_cursor();
}

// Put string to console
void console_puts(const char* str) {
    while (*str) {
        console_putchar(*str++);
    }
}

// String length
size_t kstrlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

// String copy
char* kstrcpy(char* dest, const char* src) {
    char* orig_dest = dest;
    while ((*dest++ = *src++));
    return orig_dest;
}

// String copy with limit
char* kstrncpy(char* dest, const char* src, size_t n) {
    char* orig_dest = dest;
    while (n-- && (*dest++ = *src++));
    while (n-- > 0) *dest++ = '\0';
    return orig_dest;
}

// String compare
int kstrcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

// String compare with limit
int kstrncmp(const char* s1, const char* s2, size_t n) {
    while (n-- && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
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
