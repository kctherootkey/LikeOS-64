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

// A client of /dev/input/event0 changed the lock LEDs (EV_LED): shown while
// the console does not have the keyboard.  Called by the input core under
// the device's event lock (interrupts off).
void keyboard_client_leds(int num, int caps, int scroll);

// The KD* ioctls (<kernel/uapi/kd.h>) of the console terminal.  -ENOTTY for
// a request that is not one of them.  `token' identifies the open file of
// the console the request came through (keyboard_console_file_token()), 0
// for none.
int keyboard_kd_ioctl(unsigned long req, void *argp, struct task *cur,
		      uint64_t token);

// A token for a new open file of the console terminal: never 0, never
// reused.  The file that switched the console to KD_GRAPHICS or K_OFF is
// remembered by it.
uint64_t keyboard_console_file_token(void);

// The final close of the console file with that token: if it was the last
// to switch the console away from KD_TEXT / its keyboard mode, both are put
// back (keys released, console LEDs, redraw).  Process context.
void keyboard_console_file_closed(uint64_t token);

// The console is in KD_GRAPHICS: another program draws on the screen, and
// the console's pointer (scrollbar, wheel scrollback, terminal mouse
// reports) stays out of its way.  Any context.
int keyboard_kd_graphics(void);

#endif // _KERNEL_KEYBOARD_H_
