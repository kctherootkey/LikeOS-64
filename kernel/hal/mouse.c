// LikeOS-64 I/O Subsystem - Mouse Driver
// PS/2 mouse input handling and cursor management

#include "../../include/kernel/mouse.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/memory.h"

// Global mouse state
static mouse_state_t mouse_state = {0};

// Mouse cursor bitmap (11x19 arrow cursor)
static const uint32_t cursor_bitmap[CURSOR_HEIGHT][CURSOR_WIDTH] = {
    {0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}
};

// Cursor background buffer for restoration
static uint32_t* cursor_background = NULL;

// Wait for PS/2 controller input buffer to be ready
static void mouse_wait_input(void) {
    int timeout = 100000;
    while ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) && timeout-- > 0) {
        // Wait for input buffer to be empty
    }
}

// Wait for PS/2 controller output buffer to have data
static void mouse_wait_output(void) {
    int timeout = 100000;
    while (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && timeout-- > 0) {
        // Wait for output buffer to have data
    }
}

// Read data from PS/2 controller
static uint8_t mouse_read_data(void) {
    mouse_wait_output();
    return inb(PS2_DATA_PORT);
}

// Write command to mouse via PS/2 controller
static void mouse_write_command(uint8_t cmd, uint8_t data) {
    mouse_wait_input();
    outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_PORT2);
    
    mouse_wait_input();
    outb(PS2_DATA_PORT, cmd);
    
    if (data != 0xFF) {  // 0xFF means no data byte
        mouse_wait_input();
        outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_PORT2);
        
        mouse_wait_input();
        outb(PS2_DATA_PORT, data);
    }
}

// Detect mouse type and capabilities
static uint8_t mouse_detect_type(void) {
    uint8_t response;
    
    kprintf("  Attempting mouse reset...\n");
    // Reset mouse
    mouse_write_command(MOUSE_CMD_RESET, 0xFF);
    response = mouse_read_data();  // Should be ACK
    kprintf("  Reset ACK: 0x%02X\n", response);
    response = mouse_read_data();  // Should be 0xAA (self-test passed)
    kprintf("  Self-test: 0x%02X\n", response);
    response = mouse_read_data();  // Should be 0x00 (device ID)
    kprintf("  Device ID: 0x%02X\n", response);
    
    if (response != 0x00) {
        kprintf("  Mouse reset failed or not standard mouse\n");
        return MOUSE_TYPE_STANDARD;
    }
    
    kprintf("  Attempting IntelliMouse detection sequence...\n");
    // Try to enable scroll wheel (IntelliMouse protocol)
    mouse_write_command(MOUSE_CMD_SET_SAMPLE_RATE, 200);
    response = mouse_read_data();  // ACK
    kprintf("  Sample rate 200 ACK: 0x%02X\n", response);
    
    mouse_write_command(MOUSE_CMD_SET_SAMPLE_RATE, 100);
    response = mouse_read_data();  // ACK
    kprintf("  Sample rate 100 ACK: 0x%02X\n", response);
    
    mouse_write_command(MOUSE_CMD_SET_SAMPLE_RATE, 80);
    response = mouse_read_data();  // ACK
    kprintf("  Sample rate 80 ACK: 0x%02X\n", response);
    
    // Get device ID after the sequence
    kprintf("  Getting device ID after sequence...\n");
    mouse_write_command(MOUSE_CMD_GET_DEVICE_ID, 0xFF);
    response = mouse_read_data();  // ACK
    kprintf("  Get ID ACK: 0x%02X\n", response);
    response = mouse_read_data();  // Device ID
    kprintf("  New Device ID: 0x%02X\n", response);
    
    if (response == MOUSE_TYPE_INTELLIMOUSE) {
        kprintf("  IntelliMouse detected (scroll wheel supported)\n");
        mouse_state.has_scroll_wheel = 1;
        mouse_state.packet_size = 4;
        return MOUSE_TYPE_INTELLIMOUSE;
    } else {
        kprintf("  Standard mouse detected\n");
        mouse_state.has_scroll_wheel = 0;
        mouse_state.packet_size = 3;
        return MOUSE_TYPE_STANDARD;
    }
}

// Draw cursor at specified position
static void mouse_draw_cursor(int x, int y) {
    if (!mouse_state.cursor_visible) return;
    
    // Save background before drawing cursor
    for (int cy = 0; cy < CURSOR_HEIGHT; cy++) {
        for (int cx = 0; cx < CURSOR_WIDTH; cx++) {
            int screen_x = x + cx;
            int screen_y = y + cy;
            
            // Check bounds
            if (screen_x >= 0 && screen_x < mouse_state.screen_width &&
                screen_y >= 0 && screen_y < mouse_state.screen_height) {
                
                // Save background pixel
                cursor_background[cy * CURSOR_WIDTH + cx] = fb_get_pixel(screen_x, screen_y);
                
                // Draw cursor pixel if not transparent
                if (cursor_bitmap[cy][cx] != 0x00000000) {
                    fb_set_pixel(screen_x, screen_y, cursor_bitmap[cy][cx]);
                }
            }
        }
    }
    
    // Mark cursor area as dirty
    fb_mark_dirty(x, y, x + CURSOR_WIDTH - 1, y + CURSOR_HEIGHT - 1);
}

// Clear cursor at specified position
static void mouse_clear_cursor(int x, int y) {
    if (!mouse_state.cursor_visible) return;
    
    // Restore background
    for (int cy = 0; cy < CURSOR_HEIGHT; cy++) {
        for (int cx = 0; cx < CURSOR_WIDTH; cx++) {
            int screen_x = x + cx;
            int screen_y = y + cy;
            
            // Check bounds
            if (screen_x >= 0 && screen_x < mouse_state.screen_width &&
                screen_y >= 0 && screen_y < mouse_state.screen_height) {
                
                // Restore background pixel
                fb_set_pixel(screen_x, screen_y, cursor_background[cy * CURSOR_WIDTH + cx]);
            }
        }
    }
    
    // Mark cursor area as dirty
    fb_mark_dirty(x, y, x + CURSOR_WIDTH - 1, y + CURSOR_HEIGHT - 1);
}

// Process mouse packet
static void mouse_process_packet(void) {
    uint8_t flags = mouse_state.packet_buffer[0];
    int8_t raw_x = (int8_t)mouse_state.packet_buffer[1];  // Cast to signed 8-bit
    int8_t raw_y = (int8_t)mouse_state.packet_buffer[2];  // Cast to signed 8-bit
    int8_t raw_z = 0;
        
    // Check if this is a valid packet
    if (!(flags & 0x08)) {
        // Bit 3 should always be set in the first byte
        
        // Try to resynchronize by looking for a valid first byte in the buffer
        int sync_found = 0;
        for (int i = 1; i < mouse_state.packet_size; i++) {
            if (mouse_state.packet_buffer[i] & 0x08) {
                // Shift buffer left to align with the valid flags byte
                for (int j = 0; j < mouse_state.packet_size - i; j++) {
                    mouse_state.packet_buffer[j] = mouse_state.packet_buffer[j + i];
                }
                mouse_state.packet_index = mouse_state.packet_size - i;
                sync_found = 1;
                break;
            }
        }
        
        if (!sync_found) {
            mouse_state.packet_index = 0;
        }
        return;
    }
    
    // Handle scroll wheel for IntelliMouse
    if (mouse_state.has_scroll_wheel && mouse_state.packet_size == 4) {
        raw_z = (int8_t)mouse_state.packet_buffer[3];
        // For IntelliMouse, use the full Z byte without masking
        mouse_state.scroll_delta = raw_z;
        if (raw_z != 0) {
            kprintf("Scroll wheel: raw_z=%d\n", raw_z);
        }
    }
    
    // Process button states
    mouse_state.last_buttons = (mouse_state.left_button ? MOUSE_LEFT_BUTTON : 0) |
                              (mouse_state.right_button ? MOUSE_RIGHT_BUTTON : 0) |
                              (mouse_state.middle_button ? MOUSE_MIDDLE_BUTTON : 0);
    
    mouse_state.left_button = (flags & MOUSE_LEFT_BUTTON) ? 1 : 0;
    mouse_state.right_button = (flags & MOUSE_RIGHT_BUTTON) ? 1 : 0;
    mouse_state.middle_button = (flags & MOUSE_MIDDLE_BUTTON) ? 1 : 0;
    
    // Skip if overflow occurred (check flags, not the data values)
    if (flags & (MOUSE_X_OVERFLOW | MOUSE_Y_OVERFLOW)) {
        mouse_state.packet_index = 0;
        return;
    }
    
    // Apply sensitivity and update position
    mouse_state.delta_x = (raw_x * mouse_state.sensitivity) / 2;  // Less division for IntelliMouse
    mouse_state.delta_y = -(raw_y * mouse_state.sensitivity) / 2;  // Less division for IntelliMouse
        
    // Store last position for cursor clearing
    mouse_state.last_x = mouse_state.x;
    mouse_state.last_y = mouse_state.y;
    
    // Update mouse position with bounds checking
    mouse_state.x += mouse_state.delta_x;
    mouse_state.y += mouse_state.delta_y;
    
    // Clamp to screen boundaries
    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (mouse_state.x >= mouse_state.screen_width - CURSOR_WIDTH) {
        mouse_state.x = mouse_state.screen_width - CURSOR_WIDTH - 1;
    }
    if (mouse_state.y >= mouse_state.screen_height - CURSOR_HEIGHT) {
        mouse_state.y = mouse_state.screen_height - CURSOR_HEIGHT - 1;
    }
    
    // Update cursor if position changed
    if (mouse_state.x != mouse_state.last_x || mouse_state.y != mouse_state.last_y) {
        mouse_update_cursor();
    }
    
    // Reset packet index for next packet
    mouse_state.packet_index = 0;
}

// Initialize mouse system
void mouse_init(void) {
    kprintf("Initializing PS/2 mouse...\n");
    
    // Initialize mouse state
    mouse_state.x = 400;  // Start in center of screen (assuming 800x600)
    mouse_state.y = 300;
    mouse_state.last_x = mouse_state.x;
    mouse_state.last_y = mouse_state.y;
    mouse_state.left_button = 0;
    mouse_state.right_button = 0;
    mouse_state.middle_button = 0;
    mouse_state.scroll_delta = 0;
    mouse_state.packet_index = 0;
    mouse_state.expecting_ack = 0;
    mouse_state.enabled = 0;
    mouse_state.cursor_visible = 1;
    mouse_state.sensitivity = 4;  // Default sensitivity
    
    // Get screen dimensions from framebuffer optimization system
    fb_double_buffer_t* fb_buffer = get_fb_double_buffer();
    mouse_state.screen_width = fb_buffer->width;
    mouse_state.screen_height = fb_buffer->height;
    
    // Allocate cursor background buffer
    cursor_background = (uint32_t*)kalloc(CURSOR_WIDTH * CURSOR_HEIGHT * sizeof(uint32_t));
    if (!cursor_background) {
        kprintf("ERROR: Failed to allocate cursor background buffer\n");
        return;
    }
    
    // Enable PS/2 mouse port
    mouse_wait_input();
    outb(PS2_COMMAND_PORT, PS2_CMD_ENABLE_PORT2);
    
    // Test if mouse port is available
    mouse_wait_input();
    outb(PS2_COMMAND_PORT, PS2_CMD_TEST_PORT2);
    uint8_t test_result = mouse_read_data();
    
    if (test_result != 0x00) {
        kprintf("ERROR: PS/2 mouse port test failed (result: 0x%02X)\n", test_result);
        return;
    }
    
    // Detect mouse type and capabilities
    mouse_state.mouse_type = mouse_detect_type();
    
    // Enable mouse data reporting
    mouse_write_command(MOUSE_CMD_ENABLE_REPORTING, 0xFF);
    uint8_t response = mouse_read_data();
    
    if (response != MOUSE_ACK) {
        kprintf("ERROR: Mouse failed to enable reporting (response: 0x%02X)\n", response);
        return;
    }
    
    mouse_state.enabled = 1;
        
    kprintf("Mouse initialized successfully\n");
    kprintf("  Position: (%d, %d)\n", mouse_state.x, mouse_state.y);
    kprintf("  Screen size: %dx%d\n", mouse_state.screen_width, mouse_state.screen_height);
    kprintf("  Mouse type: %s\n", mouse_state.has_scroll_wheel ? "IntelliMouse" : "Standard");
}

// Mouse IRQ handler (called from interrupt.c)
void mouse_irq_handler(void) {
    if (!mouse_state.enabled) {
        // Clear the data port to prevent buffer overflow
        inb(PS2_DATA_PORT);
        return;
    }
    
    uint8_t data = inb(PS2_DATA_PORT);
    
    // Handle ACK responses
    if (mouse_state.expecting_ack) {
        if (data == MOUSE_ACK) {
            mouse_state.expecting_ack = 0;
        } else {
            // Stay silent
            // kprintf("Expected ACK, got 0x%02X\n", data);
        }
        return;
    }
    
    // Store packet data
    mouse_state.packet_buffer[mouse_state.packet_index] = data;
    mouse_state.packet_index++;
    
    // Process packet when complete
    if (mouse_state.packet_index >= mouse_state.packet_size) {
        mouse_process_packet();
    }
}

// Update cursor position on screen
void mouse_update_cursor(void) {
    if (!mouse_state.enabled || !mouse_state.cursor_visible) return;
    
    // Clear cursor at old position
    mouse_clear_cursor(mouse_state.last_x, mouse_state.last_y);
    
    // Draw cursor at new position
    mouse_draw_cursor(mouse_state.x, mouse_state.y);
    
    // Flush dirty regions to display changes
    fb_flush_dirty_regions();
}

// Get current mouse X position
int mouse_get_x(void) {
    return mouse_state.x;
}

// Get current mouse Y position
int mouse_get_y(void) {
    return mouse_state.y;
}

// Get left button state
int mouse_button_left(void) {
    return mouse_state.left_button;
}

// Get right button state
int mouse_button_right(void) {
    return mouse_state.right_button;
}

// Get middle button state
int mouse_button_middle(void) {
    return mouse_state.middle_button;
}

// Get scroll wheel delta
int mouse_scroll_delta(void) {
    int delta = mouse_state.scroll_delta;
    mouse_state.scroll_delta = 0;  // Reset after reading
    return delta;
}

// Set mouse sensitivity
void mouse_set_sensitivity(int sensitivity) {
    if (sensitivity >= 1 && sensitivity <= 10) {
        mouse_state.sensitivity = sensitivity;
    }
}

// Show or hide cursor
void mouse_show_cursor(int show) {
    if (show && !mouse_state.cursor_visible) {
        mouse_state.cursor_visible = 1;
        mouse_draw_cursor(mouse_state.x, mouse_state.y);
        fb_flush_dirty_regions();
    } else if (!show && mouse_state.cursor_visible) {
        mouse_clear_cursor(mouse_state.x, mouse_state.y);
        mouse_state.cursor_visible = 0;
        fb_flush_dirty_regions();
    }
}
