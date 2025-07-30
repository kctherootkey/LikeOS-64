// LikeOS-64 Keyboard Driver Implementation
// PS/2 keyboard input handling

#include "keyboard.h"
#include "interrupts.h"

// Global keyboard state
static keyboard_state_t kb_state = {0};

// US QWERTY scan code to ASCII conversion table
static char scan_code_to_ascii_table[] = {
    0,   0,   '1', '2', '3', '4', '5', '6',    // 0x00-0x07
    '7', '8', '9', '0', '-', '=', '\b', '\t', // 0x08-0x0F
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',  // 0x10-0x17
    'o', 'p', '[', ']', '\n', 0,  'a', 's',  // 0x18-0x1F
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  // 0x20-0x27
    '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',  // 0x28-0x2F
    'b', 'n', 'm', ',', '.', '/', 0,   '*',  // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,    // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',  // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',  // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,    // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0     // 0x58-0x5F
};

// Shifted characters
static char scan_code_to_ascii_shifted[] = {
    0,   0,   '!', '@', '#', '$', '%', '^',    // 0x00-0x07
    '&', '*', '(', ')', '_', '+', '\b', '\t', // 0x08-0x0F
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',  // 0x10-0x17
    'O', 'P', '{', '}', '\n', 0,  'A', 'S',  // 0x18-0x1F
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',  // 0x20-0x27
    '"', '~', 0, '|', 'Z', 'X', 'C', 'V',    // 0x28-0x2F
    'B', 'N', 'M', '<', '>', '?', 0,   '*',  // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,    // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',  // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',  // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,    // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0     // 0x58-0x5F
};

// Keyboard initialization

// Initialize keyboard
void keyboard_init(void) {
    // Clear keyboard state
    kb_state.shift_pressed = 0;
    kb_state.ctrl_pressed = 0;
    kb_state.alt_pressed = 0;
    kb_state.caps_lock = 0;
    kb_state.buffer_start = 0;
    kb_state.buffer_end = 0;
    kb_state.buffer_count = 0;
    
    // Enable keyboard IRQ (will be done in main kernel)
    // irq_enable(IRQ_KEYBOARD);
    
    kprintf("Keyboard initialized\n");
}

// Read scan code from keyboard
uint8_t keyboard_read_scan_code(void) {
    return inb(KEYBOARD_DATA_PORT);
}

// Convert scan code to ASCII
char scan_code_to_ascii(uint8_t scan_code, uint8_t shift) {
    if (scan_code >= sizeof(scan_code_to_ascii_table)) {
        return 0;
    }
    
    if (shift) {
        return scan_code_to_ascii_shifted[scan_code];
    } else {
        kprintf("shifted[0x1e]=%d, normal[0x1e]=%d\n", (int)scan_code_to_ascii_shifted[0x1e], (int)scan_code_to_ascii_table[0x1e]);
        return scan_code_to_ascii_table[scan_code];
    }
}

// Add character to keyboard buffer
void keyboard_buffer_add(uint8_t scan_code) {
    if (kb_state.buffer_count < KEYBOARD_BUFFER_SIZE) {
        kb_state.buffer[kb_state.buffer_end] = scan_code;
        kb_state.buffer_end = (kb_state.buffer_end + 1) % KEYBOARD_BUFFER_SIZE;
        kb_state.buffer_count++;
    }
}

// Get character from keyboard buffer
uint8_t keyboard_buffer_get(void) {
    if (kb_state.buffer_count > 0) {
        uint8_t scan_code = kb_state.buffer[kb_state.buffer_start];
        kb_state.buffer_start = (kb_state.buffer_start + 1) % KEYBOARD_BUFFER_SIZE;
        kb_state.buffer_count--;
        return scan_code;
    }
    return 0;
}

// Check if keyboard buffer has data
uint8_t keyboard_buffer_has_data(void) {
    return kb_state.buffer_count > 0;
}

// Keyboard interrupt handler
void keyboard_irq_handler(void) {
    uint8_t scan_code = keyboard_read_scan_code();
    
    // Handle key release
    if (scan_code & KEY_RELEASE) {
        scan_code &= ~KEY_RELEASE;
        
        // Handle modifier key releases
        switch (scan_code) {
            case KEY_LSHIFT:
            case KEY_RSHIFT:
                kb_state.shift_pressed = 0;
                break;
            case KEY_CTRL:
                kb_state.ctrl_pressed = 0;
                break;
            case KEY_ALT:
                kb_state.alt_pressed = 0;
                break;
        }
        return;
    }
    
    // Handle modifier key presses
    switch (scan_code) {
        case KEY_LSHIFT:
        case KEY_RSHIFT:
            kb_state.shift_pressed = 1;
            return;
        case KEY_CTRL:
            kb_state.ctrl_pressed = 1;
            return;
        case KEY_ALT:
            kb_state.alt_pressed = 1;
            return;
        case KEY_CAPS:
            kb_state.caps_lock = !kb_state.caps_lock;
            return;
    }
    
    // Add to buffer for processing
    keyboard_buffer_add(scan_code);
}

// Get processed character from keyboard
char keyboard_get_char(void) {
    if (!keyboard_buffer_has_data()) {
        return 0;
    }
    
    uint8_t scan_code = keyboard_buffer_get();
    uint8_t shift = kb_state.shift_pressed || kb_state.caps_lock;

    return scan_code_to_ascii(scan_code, shift);
}

// Wait for key press
void keyboard_wait_for_key(void) {
    while (!keyboard_buffer_has_data()) {
        __asm__ volatile ("hlt");  // Wait for interrupt
    }
}
