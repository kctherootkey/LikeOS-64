// LikeOS HAL - 16550 UART Serial Console (COM1)
// Minimal polled serial for logging to QEMU -serial stdio
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_SERIAL_H_
#define _KERNEL_SERIAL_H_

// Use the kernel's typedefs from console.h style
typedef unsigned int uint32_t;

// Initialize COM1; detect presence; safe to call multiple times
void serial_init(void);

// Queue one character ('\n' becomes CRLF); never waits for the UART.
void serial_write_char(char c);

// Write a buffer of length len
void serial_write(const char *s, uint32_t len);

// Move queued output to the UART as far as it takes it now (timer tick).
void serial_poll(void);
// Start the kserial thread (scheduler running): from then on it alone does
// the port I/O and console output never waits for the UART.
void serial_start_thread(void);
// The system is going down (oops, panic, reboot): write output out at once
// again, queued bytes first.  Cheap; safe in any context.
void serial_set_sync(void);
// Query availability (1 if COM1 detected and initialized)
int serial_is_available(void);

#endif // _KERNEL_SERIAL_H_
