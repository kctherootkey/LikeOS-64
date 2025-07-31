// LikeOS-64 I/O Subsystem - Keyboard Driver Interface
// PS/2 keyboard device management and input processing

#ifndef _KERNEL_KEYBOARD_H_
#define _KERNEL_KEYBOARD_H_

#include "console.h"

// Keyboard ports
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

// Keyboard scan codes (US QWERTY layout)
#define KEY_ESC 0x01
#define KEY_BACKSPACE 0x0E
#define KEY_TAB 0x0F
#define KEY_ENTER 0x1C
#define KEY_CTRL 0x1D
#define KEY_LSHIFT 0x2A
#define KEY_RSHIFT 0x36
#define KEY_ALT 0x38
#define KEY_CAPS 0x3A
#define KEY_F1 0x3B
#define KEY_F2 0x3C
#define KEY_F3 0x3D
#define KEY_F4 0x3E
#define KEY_F5 0x3F
#define KEY_F6 0x40
#define KEY_F7 0x41
#define KEY_F8 0x42
#define KEY_F9 0x43
#define KEY_F10 0x44
#define KEY_F11 0x57
#define KEY_F12 0x58

// Key release flag
#define KEY_RELEASE 0x80

// Input buffer size
#define KEYBOARD_BUFFER_SIZE 256

// Keyboard state
typedef struct {
    uint8_t shift_pressed;
    uint8_t ctrl_pressed;
    uint8_t alt_pressed;
    uint8_t caps_lock;
    uint8_t buffer[KEYBOARD_BUFFER_SIZE];
    uint8_t buffer_start;
    uint8_t buffer_end;
    uint8_t buffer_count;
} keyboard_state_t;

// Function prototypes
void keyboard_init(void);
void keyboard_handler(void);
uint8_t keyboard_read_scan_code(void);
char scan_code_to_ascii(uint8_t scan_code, uint8_t shift);
void keyboard_buffer_add(uint8_t scan_code);
uint8_t keyboard_buffer_get(void);
uint8_t keyboard_buffer_has_data(void);
char keyboard_get_char(void);
void keyboard_wait_for_key(void);

#endif // _KERNEL_KEYBOARD_H_
