// LikeOS-64 HAL - 16550 UART Serial Console (COM1)
// Minimal polled serial for logging to QEMU -serial stdio

#ifndef _KERNEL_SERIAL_H_
#define _KERNEL_SERIAL_H_

// Use the kernel's typedefs from console.h style
typedef unsigned int uint32_t;

// Initialize COM1; detect presence; safe to call multiple times
void serial_init(void);

// Write one character (blocking, polled). Adds no translation.
void serial_write_char(char c);

// Write a buffer of length len
void serial_write(const char* s, uint32_t len);

// Query availability (1 if COM1 detected and initialized)
int serial_is_available(void);

#endif // _KERNEL_SERIAL_H_
