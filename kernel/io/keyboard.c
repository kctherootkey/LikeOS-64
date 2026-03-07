// LikeOS-64 I/O Subsystem - Keyboard Driver
// PS/2 keyboard input handling and device management

#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/sched.h"

// Global keyboard state
static keyboard_state_t kb_state = {0};

// Spinlock for keyboard buffer protection
static spinlock_t kb_lock = SPINLOCK_INIT("keyboard");

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
        return scan_code_to_ascii_table[scan_code];
    }
}

// Add character to keyboard buffer
void keyboard_buffer_add(uint8_t scan_code) {
    uint64_t flags;
    spin_lock_irqsave(&kb_lock, &flags);
    if (kb_state.buffer_count < KEYBOARD_BUFFER_SIZE) {
        kb_state.buffer[kb_state.buffer_end] = scan_code;
        kb_state.buffer_end = (kb_state.buffer_end + 1) % KEYBOARD_BUFFER_SIZE;
        kb_state.buffer_count++;
    }
    spin_unlock_irqrestore(&kb_lock, flags);
}

// Get character from keyboard buffer
uint8_t keyboard_buffer_get(void) {
    uint64_t flags;
    spin_lock_irqsave(&kb_lock, &flags);
    uint8_t scan_code = 0;
    if (kb_state.buffer_count > 0) {
        scan_code = kb_state.buffer[kb_state.buffer_start];
        kb_state.buffer_start = (kb_state.buffer_start + 1) % KEYBOARD_BUFFER_SIZE;
        kb_state.buffer_count--;
    }
    spin_unlock_irqrestore(&kb_lock, flags);
    return scan_code;
}

// Check if keyboard buffer has data
uint8_t keyboard_buffer_has_data(void) {
    // Simple read of volatile count - no lock needed for single read
    return kb_state.buffer_count > 0;
}

// Keyboard interrupt handler
void keyboard_irq_handler(void) {
    uint8_t scan_code = keyboard_read_scan_code();

    // Handle extended key prefix (0xE0)
    if (scan_code == KEY_EXTENDED_PREFIX) {
        kb_state.e0_prefix = 1;
        return;
    }

    // Handle extended keys (arrow, pgup, pgdn, home, end, insert, delete)
    if (kb_state.e0_prefix) {
        kb_state.e0_prefix = 0;

        // Ignore extended key releases
        if (scan_code & KEY_RELEASE)
            return;

        // Map extended scan codes to ANSI escape sequences
        tty_t *tty = tty_get_console();
        switch (scan_code) {
            case KEY_EXT_UP:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'A', 0);
                return;
            case KEY_EXT_DOWN:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'B', 0);
                return;
            case KEY_EXT_RIGHT:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'C', 0);
                return;
            case KEY_EXT_LEFT:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'D', 0);
                return;
            case KEY_EXT_HOME:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'H', 0);
                return;
            case KEY_EXT_END:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'F', 0);
                return;
            case KEY_EXT_PGUP:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '5', 0);
                tty_input_char(tty, '~', 0);
                return;
            case KEY_EXT_PGDN:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '6', 0);
                tty_input_char(tty, '~', 0);
                return;
            case KEY_EXT_INSERT:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '2', 0);
                tty_input_char(tty, '~', 0);
                return;
            case KEY_EXT_DELETE:
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '3', 0);
                tty_input_char(tty, '~', 0);
                return;
            default:
                return;
        }
    }
    
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

    // Debug hotkey: Ctrl+D dumps all task states
    if (kb_state.ctrl_pressed && !kb_state.alt_pressed && scan_code == 0x20) {  // 0x20 = 'd'
        sched_dump_tasks();
        return;
    }

    // Numpad navigation keys (same scan codes as extended keys, but without E0 prefix).
    // QEMU and some hardware send these for PgUp/PgDn/Home/End/arrows/Ins/Del.
    // Handle them as navigation keys, generating ANSI escape sequences.
    {
        tty_t *tty = tty_get_console();
        switch (scan_code) {
            case 0x47: /* Numpad 7 / Home */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'H', 0);
                return;
            case 0x48: /* Numpad 8 / Up */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'A', 0);
                return;
            case 0x49: /* Numpad 9 / PgUp */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '5', 0);
                tty_input_char(tty, '~', 0);
                return;
            case 0x4B: /* Numpad 4 / Left */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'D', 0);
                return;
            case 0x4D: /* Numpad 6 / Right */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'C', 0);
                return;
            case 0x4F: /* Numpad 1 / End */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'F', 0);
                return;
            case 0x50: /* Numpad 2 / Down */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, 'B', 0);
                return;
            case 0x51: /* Numpad 3 / PgDn */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '6', 0);
                tty_input_char(tty, '~', 0);
                return;
            case 0x52: /* Numpad 0 / Insert */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '2', 0);
                tty_input_char(tty, '~', 0);
                return;
            case 0x53: /* Numpad . / Delete */
                tty_input_char(tty, 27, 0);
                tty_input_char(tty, '[', 0);
                tty_input_char(tty, '3', 0);
                tty_input_char(tty, '~', 0);
                return;
        }
    }

    // Feed TTY input (console)
    uint8_t shift = kb_state.shift_pressed || kb_state.caps_lock;
    char ch = scan_code_to_ascii(scan_code, shift);
    if (ch) {
        tty_input_char(tty_get_console(), ch, kb_state.ctrl_pressed);
    }
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
