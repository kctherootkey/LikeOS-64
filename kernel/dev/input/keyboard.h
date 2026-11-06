// LikeOS I/O Subsystem - Keyboard Driver Interface
// PS/2 keyboard device management and the console keyboard
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_KEYBOARD_H_
#define _KERNEL_KEYBOARD_H_

#include <kernel/io/console.h>

// Keyboard ports
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_STATUS_OUTPUT_FULL 0x01
#define KEYBOARD_STATUS_AUXDATA 0x20

// Input buffer full: the controller has not taken the last byte yet.
#define KEYBOARD_STATUS_INPUT_FULL 0x02

struct task;

// Function prototypes
void keyboard_init(void);
void keyboard_activate(void);
void keyboard_reset_state(void);
uint8_t keyboard_read_scan_code(void);

// One byte of the set-1 scancode stream from a keyboard other than the PS/2
// one (USB HID translates its reports into the stream).  The name is
// historical: the byte is processed at once -- evdev, then the console.
void keyboard_buffer_add(uint8_t scan_code);

// The lock LEDs the console wants lit (LED_SCR | LED_NUM | LED_CAP), for
// keyboards whose LEDs are set from elsewhere (USB HID).
uint8_t keyboard_led_state(void);

// The KD* ioctls (<kernel/uapi/kd.h>) of the console terminal.  -ENOTTY for
// a request that is not one of them.
int keyboard_kd_ioctl(unsigned long req, void *argp, struct task *cur);

#endif // _KERNEL_KEYBOARD_H_
