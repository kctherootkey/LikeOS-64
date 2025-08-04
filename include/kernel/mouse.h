// LikeOS-64 I/O Subsystem - Mouse Driver Interface
// PS/2 mouse device management and input processing

#ifndef _KERNEL_MOUSE_H_
#define _KERNEL_MOUSE_H_

#include "console.h"

// PS/2 Controller ports (shared with keyboard)
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

// PS/2 Controller commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT2    0xA8
#define PS2_CMD_TEST_PORT2      0xA9
#define PS2_CMD_WRITE_PORT2     0xD4

// PS/2 Status register bits
#define PS2_STATUS_OUTPUT_FULL  0x01
#define PS2_STATUS_INPUT_FULL   0x02
#define PS2_STATUS_TIMEOUT      0x40
#define PS2_STATUS_PARITY_ERROR 0x80

// Mouse commands
#define MOUSE_CMD_ENABLE_REPORTING  0xF4
#define MOUSE_CMD_DISABLE_REPORTING 0xF5
#define MOUSE_CMD_SET_DEFAULTS      0xF6
#define MOUSE_CMD_RESEND            0xFE
#define MOUSE_CMD_RESET             0xFF
#define MOUSE_CMD_SET_SAMPLE_RATE   0xF3
#define MOUSE_CMD_GET_DEVICE_ID     0xF2
#define MOUSE_CMD_SET_REMOTE_MODE   0xF0
#define MOUSE_CMD_SET_STREAM_MODE   0xEA

// Mouse responses
#define MOUSE_ACK       0xFA
#define MOUSE_NACK      0xFE
#define MOUSE_ERROR     0xFC

// Mouse button flags
#define MOUSE_LEFT_BUTTON   0x01
#define MOUSE_RIGHT_BUTTON  0x02
#define MOUSE_MIDDLE_BUTTON 0x04

// Mouse packet flags
#define MOUSE_X_OVERFLOW    0x40
#define MOUSE_Y_OVERFLOW    0x80
#define MOUSE_X_SIGN        0x10
#define MOUSE_Y_SIGN        0x20

// Mouse buffer size
#define MOUSE_BUFFER_SIZE 256

// Mouse types
#define MOUSE_TYPE_STANDARD     0x00
#define MOUSE_TYPE_INTELLIMOUSE 0x03
#define MOUSE_TYPE_EXPLORER     0x04

// Mouse cursor size
#define CURSOR_WIDTH  11
#define CURSOR_HEIGHT 19

// Mouse state structure
typedef struct {
    // Position
    int x, y;
    int last_x, last_y;
    
    // Button states
    uint8_t left_button;
    uint8_t right_button;
    uint8_t middle_button;
    uint8_t last_buttons;
    
    // Scroll wheel
    int8_t scroll_delta;
    
    // Movement deltas
    int16_t delta_x;
    int16_t delta_y;
    
    // Mouse type and capabilities
    uint8_t mouse_type;
    uint8_t has_scroll_wheel;
    uint8_t packet_size;  // 3 for standard, 4 for IntelliMouse
    
    // Packet processing
    uint8_t packet_buffer[4];
    uint8_t packet_index;
    uint8_t expecting_ack;
    
    // Configuration
    uint8_t enabled;
    uint8_t cursor_visible;
    uint8_t sensitivity;
    
    // Screen bounds
    int screen_width;
    int screen_height;
} mouse_state_t;

// Core functions
void mouse_init(void);
void mouse_irq_handler(void);
void mouse_update_cursor(void);

// State queries
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_button_left(void);
int mouse_button_right(void);
int mouse_button_middle(void);
int mouse_scroll_delta(void);

// Configuration
void mouse_set_sensitivity(int sensitivity);
void mouse_show_cursor(int show);

#endif // _KERNEL_MOUSE_H_
