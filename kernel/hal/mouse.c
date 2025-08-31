// LikeOS-64 I/O Subsystem - Mouse Driver
// PS/2 mouse input handling and cursor management

#include "../../include/kernel/mouse.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"

// Global mouse state
static mouse_state_t mouse_state = {0};

// How many top rows of the arrow remain visible at the bottom edge
#define TIP_VISIBLE_ROWS 3
// Sentinel color stored in background buffer for "not saved" entries
static const uint32_t BG_SENTINEL = 0xDEADBEEF;

static inline int clampi(int v, int lo, int hi)
{
    if(v < lo) {
        return lo;
    }
    if(v > hi) {
        return hi;
    }
    return v;
}

// Mouse cursor bitmap (11x19 Windows-style arrow cursor)
static const uint32_t cursor_bitmap[CURSOR_HEIGHT][CURSOR_WIDTH] = {
    // Classic Windows cursor: white arrow with black outline
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
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000},
    {0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000},
    {0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000},
    {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000},
    {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000},
    {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}
};

// Cursor background buffer for restoration
static uint32_t* cursor_background = NULL;

// Wait for PS/2 controller input buffer to be ready
static void mouse_wait_input(void)
{
    int timeout = 100000;
    while((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) && timeout-- > 0) {
        // Wait for input buffer to be empty
    }
}

// Wait for PS/2 controller output buffer to have data
static void mouse_wait_output(void)
{
    int timeout = 100000;
    while(!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && timeout-- > 0) {
        // Wait for output buffer to have data
    }
}

// Read data from PS/2 controller
static uint8_t mouse_read_data(void)
{
    mouse_wait_output();
    return inb(PS2_DATA_PORT);
}

// Write command to mouse via PS/2 controller
static void mouse_write_command(uint8_t cmd, uint8_t data)
{
    mouse_wait_input();
    outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_PORT2);

    mouse_wait_input();
    outb(PS2_DATA_PORT, cmd);

    if(data != 0xFF) { // 0xFF means no data byte
        mouse_wait_input();
        outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_PORT2);

        mouse_wait_input();
        outb(PS2_DATA_PORT, data);
    }
}

// Detect mouse type and capabilities
static uint8_t mouse_detect_type(void)
{
    uint8_t response;

    kprintf("  Attempting mouse reset...\n");
    // Reset mouse
    mouse_write_command(MOUSE_CMD_RESET, 0xFF);
    response = mouse_read_data(); // Should be ACK
    kprintf("  Reset ACK: 0x%02X\n", response);
    response = mouse_read_data(); // Should be 0xAA (self-test passed)
    kprintf("  Self-test: 0x%02X\n", response);
    response = mouse_read_data(); // Should be 0x00 (device ID)
    kprintf("  Device ID: 0x%02X\n", response);

    if(response != 0x00) {
        kprintf("  Mouse reset failed or not standard mouse\n");
        return MOUSE_TYPE_STANDARD;
    }

    kprintf("  Attempting IntelliMouse detection sequence...\n");
    // Try to enable scroll wheel (IntelliMouse protocol)
    mouse_write_command(MOUSE_CMD_SET_SAMPLE_RATE, 200);
    response = mouse_read_data(); // ACK
    kprintf("  Sample rate 200 ACK: 0x%02X\n", response);

    mouse_write_command(MOUSE_CMD_SET_SAMPLE_RATE, 100);
    response = mouse_read_data(); // ACK
    kprintf("  Sample rate 100 ACK: 0x%02X\n", response);

    mouse_write_command(MOUSE_CMD_SET_SAMPLE_RATE, 80);
    response = mouse_read_data(); // ACK
    kprintf("  Sample rate 80 ACK: 0x%02X\n", response);

    // Get device ID after the sequence
    kprintf("  Getting device ID after sequence...\n");
    mouse_write_command(MOUSE_CMD_GET_DEVICE_ID, 0xFF);
    response = mouse_read_data(); // ACK
    kprintf("  Get ID ACK: 0x%02X\n", response);
    response = mouse_read_data(); // Device ID
    kprintf("  New Device ID: 0x%02X\n", response);

    if(response == MOUSE_TYPE_INTELLIMOUSE) {
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
static void mouse_draw_cursor_full(int x, int y)
{
    if(!mouse_state.cursor_visible) {
        return;
    }
    if(!cursor_background) {
        return;
    }
    int cw = mouse_state.cursor_w;
    int ch = mouse_state.cursor_h;
    if(x >= mouse_state.screen_width || y >= mouse_state.screen_height || x + cw - 1 < 0 || y + ch - 1 < 0) {
        return;
    }
    for(int cy = 0; cy < ch; cy++) {
        for(int cx = 0; cx < cw; cx++) {
            int bg_index = cy * cw + cx;
            if(bg_index < 0 || bg_index >= cw * ch) {
                continue;
            }
            int screen_x = x + cx;
            int screen_y = y + cy;
            if(screen_x < 0 || screen_y < 0 || screen_x >= mouse_state.screen_width || screen_y >= mouse_state.screen_height) {
                continue;
            }
            cursor_background[bg_index] = fb_get_pixel((uint32_t)screen_x, (uint32_t)screen_y);
            uint32_t pix = cursor_bitmap[cy][cx];
            if(pix != 0x00000000) {
                fb_set_pixel((uint32_t)screen_x, (uint32_t)screen_y, pix);
            }
        }
    }
    int x1 = clampi(x, 0, mouse_state.screen_width - 1);
    int y1 = clampi(y, 0, mouse_state.screen_height - 1);
    int x2 = clampi(x + cw - 1, 0, mouse_state.screen_width - 1);
    int y2 = clampi(y + ch - 1, 0, mouse_state.screen_height - 1);
    fb_mark_dirty((uint32_t)x1, (uint32_t)y1, (uint32_t)x2, (uint32_t)y2);
}

// Clear cursor at specified position
static void mouse_clear_cursor_full(int x, int y)
{
    if(!mouse_state.cursor_visible) {
        return;
    }
    if(!cursor_background) {
        return;
    }
    int cw = mouse_state.cursor_w;
    int ch = mouse_state.cursor_h;
    if(x >= mouse_state.screen_width || y >= mouse_state.screen_height || x + cw - 1 < 0 || y + ch - 1 < 0) {
        return;
    }
    for(int cy = 0; cy < ch; cy++) {
        for(int cx = 0; cx < cw; cx++) {
            int bg_index = cy * cw + cx;
            if(bg_index < 0 || bg_index >= cw * ch) {
                continue;
            }
            int screen_x = x + cx;
            int screen_y = y + cy;
            if(screen_x < 0 || screen_y < 0 || screen_x >= mouse_state.screen_width || screen_y >= mouse_state.screen_height) {
                continue;
            }
            uint32_t bg = cursor_background[bg_index];
            if(bg != BG_SENTINEL) {
                fb_set_pixel((uint32_t)screen_x, (uint32_t)screen_y, bg);
            }
            // Reset background entry so we never reuse stale data
            cursor_background[bg_index] = BG_SENTINEL;
        }
    }
    int x1 = clampi(x, 0, mouse_state.screen_width - 1);
    int y1 = clampi(y, 0, mouse_state.screen_height - 1);
    int x2 = clampi(x + cw - 1, 0, mouse_state.screen_width - 1);
    int y2 = clampi(y + ch - 1, 0, mouse_state.screen_height - 1);
    fb_mark_dirty((uint32_t)x1, (uint32_t)y1, (uint32_t)x2, (uint32_t)y2);
}

static void mouse_draw_cursor_partial(int x, int y, int visible_w, int visible_h)
{
    if(!mouse_state.cursor_visible) {
        return;
    }
    if(!cursor_background) {
        return;
    }
    int cw = mouse_state.cursor_w;
    int ch = mouse_state.cursor_h;
    if(visible_w <= 0 || visible_h <= 0) {
        return;
    }
    if(x >= mouse_state.screen_width || y >= mouse_state.screen_height) {
        return;
    }
    if(visible_w > cw) {
        visible_w = cw;
    }
    if(visible_h > ch) {
        visible_h = ch;
    }

    for(int cy = 0; cy < visible_h; cy++) {
        for(int cx = 0; cx < visible_w; cx++) {
            if(cx < 0 || cy < 0 || cx >= cw || cy >= ch) {
                continue;
            }
            int bg_index = cy * cw + cx;
            if(bg_index < 0 || bg_index >= cw * ch) {
                continue;
            }
            int screen_x = x + cx;
            int screen_y = y + cy;
            if(screen_x < 0 || screen_y < 0 || screen_x >= mouse_state.screen_width || screen_y >= mouse_state.screen_height) {
                continue;
            }
            cursor_background[bg_index] = fb_get_pixel((uint32_t)screen_x, (uint32_t)screen_y);
            uint32_t pix = cursor_bitmap[cy][cx];
            if(pix != 0x00000000) {
                fb_set_pixel((uint32_t)screen_x, (uint32_t)screen_y, pix);
            }
        }
    }

    int x1 = clampi(x, 0, mouse_state.screen_width - 1);
    int y1 = clampi(y, 0, mouse_state.screen_height - 1);
    int x2 = clampi(x + visible_w - 1, 0, mouse_state.screen_width - 1);
    int y2 = clampi(y + visible_h - 1, 0, mouse_state.screen_height - 1);
    fb_mark_dirty((uint32_t)x1, (uint32_t)y1, (uint32_t)x2, (uint32_t)y2);
}

static void mouse_clear_cursor_partial(int x, int y, int visible_w, int visible_h)
{
    if(!mouse_state.cursor_visible) {
        return;
    }
    if(!cursor_background) {
        return;
    }
    int cw = mouse_state.cursor_w;
    int ch = mouse_state.cursor_h;
    if(visible_w <= 0 || visible_h <= 0) {
        return;
    }
    if(x >= mouse_state.screen_width || y >= mouse_state.screen_height) {
        return;
    }
    if(visible_w > cw) {
        visible_w = cw;
    }
    if(visible_h > ch) {
        visible_h = ch;
    }

    for(int cy = 0; cy < visible_h; cy++) {
        for(int cx = 0; cx < visible_w; cx++) {
            if(cx < 0 || cy < 0 || cx >= cw || cy >= ch) {
                continue;
            }
            int bg_index = cy * cw + cx;
            if(bg_index < 0 || bg_index >= cw * ch) {
                continue;
            }
            int screen_x = x + cx;
            int screen_y = y + cy;
            if(screen_x < 0 || screen_y < 0 || screen_x >= mouse_state.screen_width || screen_y >= mouse_state.screen_height) {
                continue;
            }
            uint32_t bg = cursor_background[bg_index];
            if(bg != BG_SENTINEL) {
                fb_set_pixel((uint32_t)screen_x, (uint32_t)screen_y, bg);
            }
            cursor_background[bg_index] = BG_SENTINEL;
        }
    }

    int x1 = clampi(x, 0, mouse_state.screen_width - 1);
    int y1 = clampi(y, 0, mouse_state.screen_height - 1);
    int x2 = clampi(x + visible_w - 1, 0, mouse_state.screen_width - 1);
    int y2 = clampi(y + visible_h - 1, 0, mouse_state.screen_height - 1);
    fb_mark_dirty((uint32_t)x1, (uint32_t)y1, (uint32_t)x2, (uint32_t)y2);
}

// Process mouse packet
static void mouse_process_packet(void)
{
    uint8_t flags = mouse_state.packet_buffer[0];
    int8_t raw_x = (int8_t)mouse_state.packet_buffer[1]; // Cast to signed 8-bit
    int8_t raw_y = (int8_t)mouse_state.packet_buffer[2]; // Cast to signed 8-bit
    int8_t raw_z = 0;

    // Check if this is a valid packet
    if(!(flags & 0x08)) {
        // Bit 3 should always be set in the first byte
        // Try to resynchronize by looking for a valid first byte in the buffer
        int sync_found = 0;
        for(int i = 1; i < mouse_state.packet_size; i++) {
            if(mouse_state.packet_buffer[i] & 0x08) {
                // Shift buffer left to align with the valid flags byte
                for(int j = 0; j < mouse_state.packet_size - i; j++) {
                    mouse_state.packet_buffer[j] = mouse_state.packet_buffer[j + i];
                }
                mouse_state.packet_index = mouse_state.packet_size - i;
                sync_found = 1;
                break;
            }
        }
        if(!sync_found) {
            mouse_state.packet_index = 0;
        }
        return;
    }

    // Handle scroll wheel for IntelliMouse
    if(mouse_state.has_scroll_wheel && mouse_state.packet_size == 4) {
        raw_z = (int8_t)mouse_state.packet_buffer[3];
        // For IntelliMouse, use the full Z byte without masking
        mouse_state.scroll_delta = raw_z;
        if(raw_z != 0) {
            // Forward wheel to console immediately
            console_handle_mouse_wheel((int)raw_z);
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
    if(flags & (MOUSE_X_OVERFLOW | MOUSE_Y_OVERFLOW)) {
        mouse_state.packet_index = 0;
        return;
    }

    // Apply sensitivity and update position
    mouse_state.delta_x = (raw_x * mouse_state.sensitivity) / 2; // Less division for IntelliMouse
    mouse_state.delta_y = -(raw_y * mouse_state.sensitivity) / 2; // Less division for IntelliMouse

    // Store last position for cursor clearing
    mouse_state.last_x = mouse_state.x;
    mouse_state.last_y = mouse_state.y;

    // Update mouse position with bounds checking
    mouse_state.x += mouse_state.delta_x;
    mouse_state.y += mouse_state.delta_y;

    // Clamp origin to screen boundaries with partial visibility policy
    if(mouse_state.x < 0) {
        mouse_state.x = 0;
    }
    if(mouse_state.y < 0) {
        mouse_state.y = 0;
    }
    int max_x_for_partial_visibility = mouse_state.screen_width - 2;
    int max_y_for_tip_visibility = mouse_state.screen_height - TIP_VISIBLE_ROWS;
    if(max_x_for_partial_visibility < 0) {
        max_x_for_partial_visibility = 0;
    }
    if(max_y_for_tip_visibility < 0) {
        max_y_for_tip_visibility = 0;
    }
    // Keep at least 2px (right) and TIP_VISIBLE_ROWS (bottom) visible
    if(mouse_state.x > max_x_for_partial_visibility) {
        mouse_state.x = max_x_for_partial_visibility;
    }
    if(mouse_state.y > max_y_for_tip_visibility) {
        mouse_state.y = max_y_for_tip_visibility;
    }

    // Update cursor if position changed
    if(mouse_state.x != mouse_state.last_x || mouse_state.y != mouse_state.last_y) {
        mouse_update_cursor();
    }

    // Forward button/position events to console for scrollbar interactions
    console_handle_mouse_event(mouse_state.x, mouse_state.y, mouse_state.left_button ? 1 : 0);

    // Reset packet index for next packet
    mouse_state.packet_index = 0;
}

// Initialize mouse system
void mouse_init(void)
{
    kprintf("Initializing PS/2 mouse...\n");

    // Initialize mouse state
    mouse_state.x = 400; // Start in center of screen (assuming 800x600)
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
    mouse_state.sensitivity = 4; // Default sensitivity

    // Get screen dimensions from framebuffer optimization system
    fb_double_buffer_t* fb_buffer = get_fb_double_buffer();
    mouse_state.screen_width = fb_buffer->width;
    mouse_state.screen_height = fb_buffer->height;

    // Set runtime cursor dimensions
    mouse_state.cursor_w = CURSOR_WIDTH;
    mouse_state.cursor_h = CURSOR_HEIGHT;

    // Allocate cursor background buffer sized to runtime dimensions
    size_t bg_count = (size_t)mouse_state.cursor_w * (size_t)mouse_state.cursor_h;
    cursor_background = (uint32_t*)kalloc(bg_count * sizeof(uint32_t));
    if(!cursor_background) {
        kprintf("ERROR: Failed to allocate cursor background buffer\n");
        return;
    }
    for(size_t i = 0; i < bg_count; ++i) {
        cursor_background[i] = BG_SENTINEL;
    }
    (void)bg_count; // count reserved for potential diagnostics

    // Enable PS/2 mouse port
    mouse_wait_input();
    outb(PS2_COMMAND_PORT, PS2_CMD_ENABLE_PORT2);

    // Test if mouse port is available
    mouse_wait_input();
    outb(PS2_COMMAND_PORT, PS2_CMD_TEST_PORT2);
    uint8_t test_result = mouse_read_data();

    if(test_result != 0x00) {
        kprintf("ERROR: PS/2 mouse port test failed (result: 0x%02X)\n", test_result);
        return;
    }

    // Detect mouse type and capabilities
    mouse_state.mouse_type = mouse_detect_type();

    // Enable mouse data reporting
    mouse_write_command(MOUSE_CMD_ENABLE_REPORTING, 0xFF);
    uint8_t response = mouse_read_data();

    if(response != MOUSE_ACK) {
        kprintf("ERROR: Mouse failed to enable reporting (response: 0x%02X)\n", response);
        return;
    }

    mouse_state.enabled = 1;

    kprintf("Mouse initialized successfully\n");
    kprintf("  Position: (%d, %d)\n", mouse_state.x, mouse_state.y);
    kprintf("  Screen size: %dx%d\n", mouse_state.screen_width, mouse_state.screen_height);
    kprintf("  Mouse type: %s\n", mouse_state.has_scroll_wheel ? "IntelliMouse" : "Standard");
    kprintf("  Cursor size: %dx%d\n", mouse_state.cursor_w, mouse_state.cursor_h);
}

// Mouse IRQ handler (called from interrupt.c)
void mouse_irq_handler(void)
{
    if(!mouse_state.enabled) {
        // Clear the data port to prevent buffer overflow
        inb(PS2_DATA_PORT);
        return;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    // Handle ACK responses
    if(mouse_state.expecting_ack) {
        if(data == MOUSE_ACK) {
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
    if(mouse_state.packet_index >= mouse_state.packet_size) {
        mouse_process_packet();
    }
}

// Update cursor position on screen
void mouse_update_cursor(void)
{
    if(!mouse_state.enabled || !mouse_state.cursor_visible) {
        return;
    }

    int max_x_for_partial_visibility = mouse_state.screen_width - 2;
    int max_y_for_tip_visibility = mouse_state.screen_height - TIP_VISIBLE_ROWS;

    // Compute prev visible region
    int prev_w = mouse_state.cursor_w;
    int prev_h = mouse_state.cursor_h;
    if(mouse_state.last_x >= max_x_for_partial_visibility) {
        int rem = mouse_state.screen_width - mouse_state.last_x;
        prev_w = rem > 0 ? (rem < 2 ? rem : 2) : 0;
    }
    if(mouse_state.last_y >= max_y_for_tip_visibility) {
        int rem = mouse_state.screen_height - mouse_state.last_y;
        int tip = TIP_VISIBLE_ROWS;
        if(rem < tip) {
            tip = rem < 0 ? 0 : rem;
        }
        if(tip > mouse_state.cursor_h) {
            tip = mouse_state.cursor_h;
        }
        prev_h = tip;
    }
    if(prev_w > mouse_state.cursor_w) {
        prev_w = mouse_state.cursor_w;
    }
    if(prev_h > mouse_state.cursor_h) {
        prev_h = mouse_state.cursor_h;
    }

    // Clear prev
    if(prev_w > 0 && prev_h > 0 && (mouse_state.last_x >= max_x_for_partial_visibility || mouse_state.last_y >= max_y_for_tip_visibility)) {
        mouse_clear_cursor_partial(mouse_state.last_x, mouse_state.last_y, prev_w, prev_h);
    } else {
        mouse_clear_cursor_full(mouse_state.last_x, mouse_state.last_y);
    }

    // Draw new
    int visible_w = mouse_state.cursor_w;
    int visible_h = mouse_state.cursor_h;
    if(mouse_state.x >= max_x_for_partial_visibility) {
        int rem = mouse_state.screen_width - mouse_state.x;
        visible_w = rem > 0 ? (rem < 2 ? rem : 2) : 0;
    }
    if(mouse_state.y >= max_y_for_tip_visibility) {
        int rem = mouse_state.screen_height - mouse_state.y;
        int tip = TIP_VISIBLE_ROWS;
        if(rem < tip) {
            tip = rem < 0 ? 0 : rem;
        }
        if(tip > mouse_state.cursor_h) {
            tip = mouse_state.cursor_h;
        }
        visible_h = tip;
    }
    if(visible_w > mouse_state.cursor_w) {
        visible_w = mouse_state.cursor_w;
    }
    if(visible_h > mouse_state.cursor_h) {
        visible_h = mouse_state.cursor_h;
    }
    if(visible_w > 0 && visible_h > 0 && (mouse_state.x >= max_x_for_partial_visibility || mouse_state.y >= max_y_for_tip_visibility)) {
        mouse_draw_cursor_partial(mouse_state.x, mouse_state.y, visible_w, visible_h);
    } else {
        mouse_draw_cursor_full(mouse_state.x, mouse_state.y);
    }

    // Flush dirty regions to display changes
    fb_flush_dirty_regions();
}

// Get current mouse X position
int mouse_get_x(void)
{
    return mouse_state.x;
}

// Get current mouse Y position
int mouse_get_y(void)
{
    return mouse_state.y;
}

// Get left button state
int mouse_button_left(void)
{
    return mouse_state.left_button;
}

// Get right button state
int mouse_button_right(void)
{
    return mouse_state.right_button;
}

// Get middle button state
int mouse_button_middle(void)
{
    return mouse_state.middle_button;
}

// Get scroll wheel delta
int mouse_scroll_delta(void)
{
    int delta = mouse_state.scroll_delta;
    mouse_state.scroll_delta = 0; // Reset after reading
    return delta;
}

// Set mouse sensitivity
void mouse_set_sensitivity(int sensitivity)
{
    if(sensitivity >= 1 && sensitivity <= 10) {
        mouse_state.sensitivity = sensitivity;
    }
}

// Show or hide cursor
void mouse_show_cursor(int show)
{
    if(show && !mouse_state.cursor_visible) {
        mouse_state.cursor_visible = 1;
        mouse_draw_cursor_full(mouse_state.x, mouse_state.y);
        fb_flush_dirty_regions();
    } else if(!show && mouse_state.cursor_visible) {
        mouse_clear_cursor_full(mouse_state.x, mouse_state.y);
        mouse_state.cursor_visible = 0;
        fb_flush_dirty_regions();
    }
}
