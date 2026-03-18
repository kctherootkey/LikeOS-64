// LikeOS-64 - USB HID (Human Interface Device) Class Driver
//
// Implements the USB HID specification 1.11 for Boot Protocol keyboards and mice.
// This driver uses Interrupt IN transfers via the xHCI controller to receive
// periodic input reports from HID devices.
//
// Architecture:
//   1. During USB enumeration, xhci_enumerate_device() calls usbhid_probe()
//      for any device/interface with class code 0x03 (HID).
//   2. The probe function parses the configuration descriptor to find HID
//      interfaces and their Interrupt IN endpoints.
//   3. For Boot Protocol devices (subclass=1), the driver sends SET_PROTOCOL(Boot)
//      and SET_IDLE(0) requests per the USB HID spec Section 7.2.6/7.2.4.
//   4. An interrupt IN endpoint is configured and a transfer ring is allocated.
//   5. PRIMARY path (interrupt-driven, lowest latency):
//      xhci_handle_transfer_event() → usbhid_irq_completion()
//      Reports are processed and the next interrupt IN transfer is re-submitted
//      entirely in IRQ context.  No main-loop involvement needed.
//   6. FALLBACK path (polling):
//      usbhid_poll() is called from the main loop to catch any transfers
//      that were not re-submitted from IRQ context (e.g. initial arming
//      or error recovery requiring control transfers).
//
// References:
//   - USB Device Class Definition for HID 1.11 (June 27, 2001)
//   - USB HID Usage Tables 1.12
//   - Intel xHCI specification 1.2 (for interrupt endpoint handling)
//   - Intel Integrated Sensor Hub - detected and skipped gracefully

#include "../../include/kernel/usbhid.h"
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/usb.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/sched.h"

// Debug output control
#define USBHID_DEBUG 0
#if USBHID_DEBUG
    #define hid_dbg(fmt, ...) kprintf("[USBHID] " fmt, ##__VA_ARGS__)
#else
    #define hid_dbg(fmt, ...) ((void)0)
#endif

// ============================================================================
// Internal State
// ============================================================================

// Array of HID device instances
static usbhid_device_t g_hid_devices[USBHID_MAX_DEVICES];
static int g_hid_device_count = 0;
static int g_hid_initialized = 0;

// Spinlock for HID state protection
static spinlock_t hid_lock = SPINLOCK_INIT("usbhid");

// ============================================================================
// Utility Functions
// ============================================================================

static void hid_memset(void* dst, int val, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void hid_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

static void hid_delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms * 100000; i++) {
        __asm__ volatile("pause");
    }
}

// Memory barrier
static inline void hid_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}

// ============================================================================
// HID Usage Code to PS/2 Scan Code Translation
// (USB HID Usage Tables 1.12, Section 10 - Keyboard/Keypad Page 0x07)
// ============================================================================

// Maps HID keyboard usage codes (0x00-0x67) to PS/2 Set 1 make scan codes.
// Index = HID usage code, value = PS/2 scan code (0 = no mapping).
// This table converts USB HID key codes to the same scan codes that the
// existing PS/2 keyboard driver uses, allowing seamless integration.
static const uint8_t hid_to_scancode[256] = {
    // 0x00-0x03: No Event, ErrorRollOver, POSTFail, ErrorUndefined
    [0x00] = 0x00, [0x01] = 0x00, [0x02] = 0x00, [0x03] = 0x00,
    // 0x04-0x1D: Letters a-z → PS/2 scan codes
    [0x04] = 0x1E,  // a → 0x1E
    [0x05] = 0x30,  // b → 0x30
    [0x06] = 0x2E,  // c → 0x2E
    [0x07] = 0x20,  // d → 0x20
    [0x08] = 0x12,  // e → 0x12
    [0x09] = 0x21,  // f → 0x21
    [0x0A] = 0x22,  // g → 0x22
    [0x0B] = 0x23,  // h → 0x23
    [0x0C] = 0x17,  // i → 0x17
    [0x0D] = 0x24,  // j → 0x24
    [0x0E] = 0x25,  // k → 0x25
    [0x0F] = 0x26,  // l → 0x26
    [0x10] = 0x32,  // m → 0x32
    [0x11] = 0x31,  // n → 0x31
    [0x12] = 0x18,  // o → 0x18
    [0x13] = 0x19,  // p → 0x19
    [0x14] = 0x10,  // q → 0x10
    [0x15] = 0x13,  // r → 0x13
    [0x16] = 0x1F,  // s → 0x1F
    [0x17] = 0x14,  // t → 0x14
    [0x18] = 0x16,  // u → 0x16
    [0x19] = 0x2F,  // v → 0x2F
    [0x1A] = 0x11,  // w → 0x11
    [0x1B] = 0x2D,  // x → 0x2D
    [0x1C] = 0x15,  // y → 0x15
    [0x1D] = 0x2C,  // z → 0x2C
    // 0x1E-0x27: Digits 1-0
    [0x1E] = 0x02,  // 1 → 0x02
    [0x1F] = 0x03,  // 2 → 0x03
    [0x20] = 0x04,  // 3 → 0x04
    [0x21] = 0x05,  // 4 → 0x05
    [0x22] = 0x06,  // 5 → 0x06
    [0x23] = 0x07,  // 6 → 0x07
    [0x24] = 0x08,  // 7 → 0x08
    [0x25] = 0x09,  // 8 → 0x09
    [0x26] = 0x0A,  // 9 → 0x0A
    [0x27] = 0x0B,  // 0 → 0x0B
    // 0x28-0x38: Special keys
    [0x28] = 0x1C,  // Enter → 0x1C
    [0x29] = 0x01,  // Escape → 0x01
    [0x2A] = 0x0E,  // Backspace → 0x0E
    [0x2B] = 0x0F,  // Tab → 0x0F
    [0x2C] = 0x39,  // Space → 0x39
    [0x2D] = 0x0C,  // - → 0x0C
    [0x2E] = 0x0D,  // = → 0x0D
    [0x2F] = 0x1A,  // [ → 0x1A
    [0x30] = 0x1B,  // ] → 0x1B
    [0x31] = 0x2B,  // \ → 0x2B
    [0x32] = 0x2B,  // # (non-US) → 0x2B
    [0x33] = 0x27,  // ; → 0x27
    [0x34] = 0x28,  // ' → 0x28
    [0x35] = 0x29,  // ` → 0x29
    [0x36] = 0x33,  // , → 0x33
    [0x37] = 0x34,  // . → 0x34
    [0x38] = 0x35,  // / → 0x35
    // 0x39: Caps Lock
    [0x39] = 0x3A,
    // 0x3A-0x45: F1-F12
    [0x3A] = 0x3B,  // F1
    [0x3B] = 0x3C,  // F2
    [0x3C] = 0x3D,  // F3
    [0x3D] = 0x3E,  // F4
    [0x3E] = 0x3F,  // F5
    [0x3F] = 0x40,  // F6
    [0x40] = 0x41,  // F7
    [0x41] = 0x42,  // F8
    [0x42] = 0x43,  // F9
    [0x43] = 0x44,  // F10
    [0x44] = 0x57,  // F11
    [0x45] = 0x58,  // F12
    // 0x46-0x48: PrintScreen, ScrollLock, Pause
    [0x46] = 0x00,  // PrintScreen (complex E0 sequence, handled specially)
    [0x47] = 0x46,  // ScrollLock
    [0x48] = 0x00,  // Pause (complex sequence, handled specially)
    // 0x49-0x52: Navigation keys (these generate E0-prefixed scan codes)
    [0x49] = 0x52,  // Insert (E0 52)
    [0x4A] = 0x47,  // Home (E0 47)
    [0x4B] = 0x49,  // PageUp (E0 49)
    [0x4C] = 0x53,  // Delete (E0 53)
    [0x4D] = 0x4F,  // End (E0 4F)
    [0x4E] = 0x51,  // PageDown (E0 51)
    [0x4F] = 0x4D,  // Right Arrow (E0 4D)
    [0x50] = 0x4B,  // Left Arrow (E0 4B)
    [0x51] = 0x50,  // Down Arrow (E0 50)
    [0x52] = 0x48,  // Up Arrow (E0 48)
    // 0x53: NumLock
    [0x53] = 0x45,
    // 0x54-0x63: Keypad keys
    [0x54] = 0x35,  // KP /
    [0x55] = 0x37,  // KP *
    [0x56] = 0x4A,  // KP -
    [0x57] = 0x4E,  // KP +
    [0x58] = 0x1C,  // KP Enter (E0 1C)
    [0x59] = 0x4F,  // KP 1
    [0x5A] = 0x50,  // KP 2
    [0x5B] = 0x51,  // KP 3
    [0x5C] = 0x4B,  // KP 4
    [0x5D] = 0x4C,  // KP 5
    [0x5E] = 0x4D,  // KP 6
    [0x5F] = 0x47,  // KP 7
    [0x60] = 0x48,  // KP 8
    [0x61] = 0x49,  // KP 9
    [0x62] = 0x52,  // KP 0
    [0x63] = 0x53,  // KP .
};

// HID usage codes that map to extended (E0-prefixed) PS/2 scan codes
// Navigation keys, multimedia keys, etc.
static int hid_is_extended_key(uint8_t usage) {
    switch (usage) {
        case HID_KEY_INSERT:
        case HID_KEY_HOME:
        case HID_KEY_PAGEUP:
        case HID_KEY_DELETE:
        case HID_KEY_END:
        case HID_KEY_PAGEDOWN:
        case HID_KEY_RIGHT:
        case HID_KEY_LEFT:
        case HID_KEY_DOWN:
        case HID_KEY_UP:
        case HID_KEY_KP_SLASH:
        case HID_KEY_KP_ENTER:
            return 1;
        default:
            return 0;
    }
}

// ============================================================================
// HID Keyboard Report Processing
// (USB HID 1.11, Appendix B.1 - Boot Keyboard Input Report)
// ============================================================================

// Process a Boot Protocol keyboard input report.
// Report format (8 bytes):
//   Byte 0: Modifier keys (bitmap of Ctrl/Shift/Alt/GUI)
//   Byte 1: Reserved (OEM use)
//   Byte 2-7: Key codes (up to 6 simultaneous keys)
//
// We translate HID usage codes to PS/2 scan codes and inject them into
// the existing keyboard subsystem via keyboard_irq_handler() path.
static void hid_process_keyboard_report(usbhid_device_t* hdev,
                                         const uint8_t* report, uint8_t len) {
    if (len < HID_BOOT_KBD_REPORT_SIZE) return;

    uint8_t modifiers = report[0];
    uint8_t prev_modifiers = hdev->kbd_modifiers;
    const uint8_t* keys = &report[2];
    const uint8_t* prev_keys = &hdev->prev_report[2];

    tty_t* tty = tty_get_console();
    if (!tty) return;

    // --- Process modifier key changes ---
    // Each modifier bit maps to a virtual key press/release.
    // We translate them to the appropriate keyboard_buffer_add() calls.

    // Check for Shift changes
    uint8_t shift_pressed = (modifiers & (HID_KBD_MOD_LSHIFT | HID_KBD_MOD_RSHIFT)) ? 1 : 0;
    uint8_t prev_shift = (prev_modifiers & (HID_KBD_MOD_LSHIFT | HID_KBD_MOD_RSHIFT)) ? 1 : 0;

    if (shift_pressed && !prev_shift) {
        // Shift pressed - inject Left Shift make code
        keyboard_buffer_add(0x2A);  // KEY_LSHIFT make
    } else if (!shift_pressed && prev_shift) {
        // Shift released - inject Left Shift break code
        keyboard_buffer_add(0x2A | 0x80);  // KEY_LSHIFT break
    }

    // Check for Ctrl changes
    uint8_t ctrl_pressed = (modifiers & (HID_KBD_MOD_LCTRL | HID_KBD_MOD_RCTRL)) ? 1 : 0;
    uint8_t prev_ctrl = (prev_modifiers & (HID_KBD_MOD_LCTRL | HID_KBD_MOD_RCTRL)) ? 1 : 0;

    if (ctrl_pressed && !prev_ctrl) {
        keyboard_buffer_add(0x1D);  // KEY_CTRL make
    } else if (!ctrl_pressed && prev_ctrl) {
        keyboard_buffer_add(0x1D | 0x80);  // KEY_CTRL break
    }

    // Check for Alt changes
    uint8_t alt_pressed = (modifiers & (HID_KBD_MOD_LALT | HID_KBD_MOD_RALT)) ? 1 : 0;
    uint8_t prev_alt = (prev_modifiers & (HID_KBD_MOD_LALT | HID_KBD_MOD_RALT)) ? 1 : 0;

    if (alt_pressed && !prev_alt) {
        keyboard_buffer_add(0x38);  // KEY_ALT make
    } else if (!alt_pressed && prev_alt) {
        keyboard_buffer_add(0x38 | 0x80);  // KEY_ALT break
    }

    hdev->kbd_modifiers = modifiers;

    // --- Process key releases ---
    // Find keys in previous report that are NOT in current report → released
    for (int i = 0; i < HID_BOOT_KBD_MAX_KEYS; i++) {
        uint8_t prev_key = prev_keys[i];
        if (prev_key == 0 || prev_key >= 0x04) {
            if (prev_key == 0) continue;

            // Check if this key is still in the current report
            int still_pressed = 0;
            for (int j = 0; j < HID_BOOT_KBD_MAX_KEYS; j++) {
                if (keys[j] == prev_key) {
                    still_pressed = 1;
                    break;
                }
            }

            if (!still_pressed && prev_key < sizeof(hid_to_scancode)) {
                uint8_t sc = hid_to_scancode[prev_key];
                if (sc != 0) {
                    if (hid_is_extended_key(prev_key)) {
                        // Inject E0 prefix then break code
                        keyboard_buffer_add(0xE0);
                    }
                    keyboard_buffer_add(sc | 0x80);  // Break code
                    // Also feed the release through the keyboard IRQ handler path
                    // for proper modifier tracking
                }
            }
        }
    }

    // --- Process key presses ---
    // Find keys in current report that are NOT in previous report → newly pressed
    for (int i = 0; i < HID_BOOT_KBD_MAX_KEYS; i++) {
        uint8_t key = keys[i];
        if (key == 0 || key <= HID_KEY_ERR_UNDEFINED) continue;

        // Check if this key was already in the previous report
        int was_pressed = 0;
        for (int j = 0; j < HID_BOOT_KBD_MAX_KEYS; j++) {
            if (prev_keys[j] == key) {
                was_pressed = 1;
                break;
            }
        }

        if (!was_pressed) {
            // Newly pressed key - translate and inject

            // Handle navigation and special keys that generate escape sequences
            // directly through the TTY, similar to what keyboard_irq_handler does
            // for E0-prefixed keys.
            if (hid_is_extended_key(key)) {
                // Generate the ANSI escape sequence directly
                switch (key) {
                    case HID_KEY_UP:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, 'A', 0);
                        continue;
                    case HID_KEY_DOWN:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, 'B', 0);
                        continue;
                    case HID_KEY_RIGHT:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, 'C', 0);
                        continue;
                    case HID_KEY_LEFT:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, 'D', 0);
                        continue;
                    case HID_KEY_HOME:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, 'H', 0);
                        continue;
                    case HID_KEY_END:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, 'F', 0);
                        continue;
                    case HID_KEY_PAGEUP:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, '5', 0);
                        tty_input_char(tty, '~', 0);
                        continue;
                    case HID_KEY_PAGEDOWN:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, '6', 0);
                        tty_input_char(tty, '~', 0);
                        continue;
                    case HID_KEY_INSERT:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, '2', 0);
                        tty_input_char(tty, '~', 0);
                        continue;
                    case HID_KEY_DELETE:
                        tty_input_char(tty, 27, 0);
                        tty_input_char(tty, '[', 0);
                        tty_input_char(tty, '3', 0);
                        tty_input_char(tty, '~', 0);
                        continue;
                }
            }

            // For regular keys, translate HID usage to PS/2 scan code
            // and inject into the keyboard subsystem
            if (key < sizeof(hid_to_scancode)) {
                uint8_t sc = hid_to_scancode[key];
                if (sc != 0) {
                    // Feed the scan code to the keyboard handler
                    // This will handle conversion to ASCII and TTY input
                    keyboard_buffer_add(sc);

                    // Also directly convert to ASCII and feed TTY
                    // (keyboard_irq_handler processes the buffer, but we
                    //  feed TTY directly for immediate response)
                    char ch = scan_code_to_ascii(sc, shift_pressed);
                    if (ch) {
                        if (alt_pressed) {
                            tty_input_char(tty, 27, 0);  // ESC prefix for Alt
                        }
                        tty_input_char(tty, ch, ctrl_pressed);
                    }
                }
            }

            // Handle F1-F12 keys (generate xterm escape sequences)
            if (key >= HID_KEY_F1 && key <= HID_KEY_F12) {
                int fnum = key - HID_KEY_F1 + 1;
                tty_input_char(tty, 27, 0);  // ESC
                if (fnum <= 4) {
                    // F1-F4: \033OP .. \033OS
                    tty_input_char(tty, 'O', 0);
                    tty_input_char(tty, (char)('P' + fnum - 1), 0);
                } else {
                    // F5-F12: \033[N~ where N varies
                    static const char* fseq[] = {
                        "15","17","18","19","20","21","23","24"
                    };
                    const char* s = fseq[fnum - 5];
                    tty_input_char(tty, '[', 0);
                    while (*s) tty_input_char(tty, *s++, 0);
                    tty_input_char(tty, '~', 0);
                }
            }
        }
    }

    // Save report for next comparison
    hid_memcpy(hdev->prev_report, report, HID_BOOT_KBD_REPORT_SIZE);
}

// ============================================================================
// HID Mouse Report Processing
// (USB HID 1.11, Appendix B.2 - Boot Mouse Input Report)
// ============================================================================

// Process a Boot Protocol mouse input report.
// Report format (3-4 bytes):
//   Byte 0: Buttons (bit 0=left, bit 1=right, bit 2=middle)
//   Byte 1: X displacement (signed 8-bit, -127 to +127)
//   Byte 2: Y displacement (signed 8-bit, -127 to +127)
//   Byte 3: Wheel (optional, signed 8-bit)
//
// We translate this into the same format the PS/2 mouse driver uses
// and call the mouse movement/button processing functions.
static void hid_process_mouse_report(usbhid_device_t* hdev,
                                      const uint8_t* report, uint8_t len) {
    if (len < HID_BOOT_MOUSE_MIN_SIZE) return;

    uint8_t buttons = report[0];
    int8_t dx = (int8_t)report[1];
    int8_t dy = (int8_t)report[2];
    int8_t wheel = 0;
    if (len >= 4) {
        wheel = (int8_t)report[3];
    }

    // Inject the movement/button/scroll event into the mouse subsystem.
    // mouse_inject_usb_movement() handles position update, clamping,
    // cursor redraw, scrollbar interaction, and TTY mouse reporting —
    // the same pipeline the PS/2 mouse driver uses.
    mouse_inject_usb_movement((int)dx, (int)dy, buttons, wheel);

    // Save current report for future delta comparisons
    hid_memcpy(hdev->prev_report, report,
               len > USBHID_REPORT_BUF_SIZE ? USBHID_REPORT_BUF_SIZE : len);
}

// ============================================================================
// Interrupt Transfer Management
// ============================================================================

// Submit an interrupt IN transfer for a HID device.
// This queues a Normal TRB on the device's interrupt IN transfer ring
// and rings the doorbell. The xHCI controller will complete this transfer
// when the device sends its next interrupt report.
static int hid_submit_interrupt_in(usbhid_device_t* hdev) {
    if (!hdev || !hdev->int_in_ring || !hdev->report_buf) return ST_INVALID;

    xhci_controller_t* ctrl = hdev->ctrl;
    usb_device_t* dev = hdev->usb_dev;
    xhci_ring_t* ring = hdev->int_in_ring;
    uint8_t slot = dev->slot_id;
    // DCI for IN endpoint = endpoint_number * 2 + 1
    uint8_t dci = hdev->int_in_ep * 2 + 1;
    uint16_t max_pkt = hdev->int_in_max_pkt;

    // Reset transfer state
    hdev->xfer.completed = 0;
    hdev->xfer.cc = 0;
    hdev->xfer.bytes_transferred = 0;

    // Register the pending transfer for the IRQ handler
    ctrl->pending_xfer[slot - 1][dci] = &hdev->xfer;

    // Queue a Normal TRB for interrupt IN
    // Per xHCI spec, interrupt transfers use Normal TRBs on the endpoint's
    // transfer ring. The controller schedules them based on the endpoint's
    // interval configured in the Endpoint Context.
    uint32_t status = TRB_LEN(max_pkt) | TRB_TD_SIZE(0);
    uint32_t control = (TRB_TYPE_NORMAL << 10) | TRB_FLAG_IOC | TRB_FLAG_ISP;

    xhci_ring_enqueue(ring, hdev->report_buf_phys, status, control);

    // Ring the doorbell to notify the controller
    xhci_ring_doorbell(ctrl, slot, dci);

    hdev->xfer_pending = 1;
    hdev->needs_resubmit = 0;

    hid_dbg("Submitted interrupt IN: slot=%d ep=%d dci=%d max_pkt=%d\n",
            slot, hdev->int_in_ep, dci, max_pkt);

    return ST_OK;
}

// Re-submit an interrupt IN transfer from IRQ context.
// This is a stripped-down version of hid_submit_interrupt_in() that assumes
// the caller already holds xhci_lock (i.e., we are inside the xHCI IRQ
// handler).  No extra locks are acquired.
static void hid_resubmit_irq(usbhid_device_t* hdev) {
    xhci_controller_t* ctrl = hdev->ctrl;
    usb_device_t* dev = hdev->usb_dev;
    xhci_ring_t* ring = hdev->int_in_ring;
    uint8_t slot = dev->slot_id;
    uint8_t dci = hdev->int_in_ep * 2 + 1;
    uint16_t max_pkt = hdev->int_in_max_pkt;

    // Reset transfer state for the next report
    hdev->xfer.completed = 0;
    hdev->xfer.cc = 0;
    hdev->xfer.bytes_transferred = 0;

    // Re-register pending transfer (the old pointer was already consumed)
    ctrl->pending_xfer[slot - 1][dci] = &hdev->xfer;

    // Queue Normal TRB
    uint32_t status = TRB_LEN(max_pkt) | TRB_TD_SIZE(0);
    uint32_t control = (TRB_TYPE_NORMAL << 10) | TRB_FLAG_IOC | TRB_FLAG_ISP;
    xhci_ring_enqueue(ring, hdev->report_buf_phys, status, control);

    // Ring doorbell
    xhci_ring_doorbell(ctrl, slot, dci);

    hdev->xfer_pending = 1;
    hdev->needs_resubmit = 0;
}

// ============================================================================
// IRQ-Context Completion Handler
// ============================================================================

// Called directly from xhci_handle_transfer_event() in hard-IRQ context.
// The xhci_lock is already held.  We must NOT acquire hid_lock here
// (IRQ-save on hid_lock disables interrupts, so if the main loop held it
// on this CPU the IRQ would not be delivered – but lock ordering must
// still be respected across CPUs).
//
// Returns 1 if the (slot, ep_id) belongs to a HID device and was consumed,
// 0 otherwise (fall through to the generic pending_xfer path).
int usbhid_irq_completion(xhci_controller_t* ctrl, uint8_t slot, uint8_t ep_id,
                           uint8_t cc, uint32_t residue) {
    if (!g_hid_initialized || g_hid_device_count == 0)
        return 0;

    // Identify which HID device this transfer belongs to.
    // We match on controller pointer, slot ID, and endpoint DCI.
    usbhid_device_t* hdev = NULL;
    for (int i = 0; i < g_hid_device_count; i++) {
        usbhid_device_t* h = &g_hid_devices[i];
        if (!h->active || !h->configured) continue;
        if (h->ctrl != ctrl) continue;
        if (h->usb_dev->slot_id != slot) continue;
        uint8_t dci = h->int_in_ep * 2 + 1;
        if (dci != ep_id) continue;
        hdev = h;
        break;
    }

    if (!hdev)
        return 0;   // Not a HID endpoint; let generic handler deal with it

    // Clear the pending_xfer pointer – we consume this transfer here
    ctrl->pending_xfer[slot - 1][ep_id] = NULL;
    hdev->xfer_pending = 0;

    if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PACKET) {
        // Compiler barrier: ensure we see the DMA data written by the device
        __asm__ volatile("" ::: "memory");

        uint32_t received = hdev->int_in_max_pkt - residue;
        if (received > USBHID_REPORT_BUF_SIZE) received = USBHID_REPORT_BUF_SIZE;

        hid_dbg("IRQ HID[slot=%d] report: %d bytes, cc=%d\n", slot, received, cc);

        // Process the report immediately in IRQ context.
        // keyboard_buffer_add() and tty_input_char() are safe to call here –
        // they use their own irqsave spinlocks internally.
        if (hdev->type == USBHID_TYPE_KEYBOARD && received >= HID_BOOT_KBD_REPORT_SIZE) {
            hid_process_keyboard_report(hdev, hdev->report_buf, (uint8_t)received);
        } else if (hdev->type == USBHID_TYPE_MOUSE && received >= HID_BOOT_MOUSE_MIN_SIZE) {
            hid_process_mouse_report(hdev, hdev->report_buf, (uint8_t)received);
        }

        // Re-submit the next interrupt IN transfer, still in IRQ context.
        hid_resubmit_irq(hdev);
    } else if (cc == TRB_CC_STALL) {
        // Stall recovery requires a control transfer (CLEAR_FEATURE) which
        // cannot be done safely inside the IRQ handler because the xHCI lock
        // is already held and xhci_control_transfer() would deadlock.
        // Flag it for the main-loop fallback in usbhid_poll().
        hid_dbg("IRQ HID[slot=%d] STALL – deferring to poll\n", slot);
        hdev->needs_resubmit = 1;
    } else {
        // Transaction error / babble / etc.  Just re-submit and hope for
        // the best.  If it keeps failing the device is probably disconnected.
        hid_dbg("IRQ HID[slot=%d] error cc=%d – re-submitting\n", slot, cc);
        hid_resubmit_irq(hdev);
    }

    return 1;   // Transfer consumed by HID driver
}

// ============================================================================
// HID Device Configuration
// ============================================================================

// Send SET_PROTOCOL(Boot Protocol) request to a HID device.
// USB HID 1.11, Section 7.2.6:
//   bmRequestType = 0x21 (Host-to-Device, Class, Interface)
//   bRequest = SET_PROTOCOL (0x0B)
//   wValue = 0 (Boot Protocol) or 1 (Report Protocol)
//   wIndex = Interface number
//   wLength = 0
static int hid_set_protocol(xhci_controller_t* ctrl, usb_device_t* dev,
                             uint8_t interface_num, uint8_t protocol) {
    return xhci_control_transfer(ctrl, dev,
                                  USB_RT_H2D | USB_RT_CLASS | USB_RT_IFACE,
                                  HID_REQ_SET_PROTOCOL,
                                  protocol,          // wValue: 0=Boot, 1=Report
                                  interface_num,     // wIndex: Interface number
                                  0, NULL);          // No data stage
}

// Send SET_IDLE(0) request to a HID device.
// USB HID 1.11, Section 7.2.4:
//   bmRequestType = 0x21 (Host-to-Device, Class, Interface)
//   bRequest = SET_IDLE (0x0A)
//   wValue = 0 (Duration=0 → report only on change; ReportID=0 → all reports)
//   wIndex = Interface number
//   wLength = 0
//
// Setting idle to 0 means the device only sends reports when input changes,
// which is ideal for interrupt-driven polling.
static int hid_set_idle(xhci_controller_t* ctrl, usb_device_t* dev,
                         uint8_t interface_num, uint8_t duration, uint8_t report_id) {
    uint16_t wValue = ((uint16_t)duration << 8) | report_id;
    return xhci_control_transfer(ctrl, dev,
                                  USB_RT_H2D | USB_RT_CLASS | USB_RT_IFACE,
                                  HID_REQ_SET_IDLE,
                                  wValue,            // wValue: Duration | ReportID
                                  interface_num,     // wIndex: Interface number
                                  0, NULL);          // No data stage
}

// Configure an interrupt IN endpoint for a HID device via xHCI.
// This sets up the Endpoint Context in the device's output context
// with the proper type (Interrupt IN) and interval.
static int hid_configure_interrupt_ep(usbhid_device_t* hdev) {
    xhci_controller_t* ctrl = hdev->ctrl;
    usb_device_t* dev = hdev->usb_dev;
    uint8_t slot = dev->slot_id;
    uint8_t ep_num = hdev->int_in_ep;
    uint16_t max_pkt = hdev->int_in_max_pkt;
    uint8_t interval = hdev->int_in_interval;

    // Allocate transfer ring for this endpoint
    xhci_ring_t* ring = xhci_alloc_ring();
    if (!ring) {
        kprintf("[USBHID] Failed to allocate interrupt IN ring\n");
        return ST_NOMEM;
    }

    uint64_t ring_phys = mm_get_physical_address((uint64_t)ring->trbs);
    xhci_ring_init(ring, ring_phys);
    hdev->int_in_ring = ring;

    // Configure the endpoint through xHCI
    // For interrupt endpoints, the interval in the Endpoint Context is encoded as:
    //   Interval = 2^(value-1) * 125 µs (for HS/SS)
    //   Interval = value ms (for FS/LS)
    // The USB endpoint descriptor interval field meaning:
    //   FS/LS: interval in ms (1-255)
    //   HS/SS: interval = 2^(bInterval-1) * 125 µs
    //
    // For the xHCI Endpoint Context interval field (bits 23:16):
    //   The value represents 2^(Interval) * 125 µs for all speeds.
    //   FS/LS: convert ms to this encoding
    //   HS/SS: use bInterval directly

    // Convert USB descriptor interval to xHCI endpoint context interval
    uint8_t xhci_interval;
    if (dev->speed <= XHCI_SPEED_FULL) {
        // Full/Low speed: bInterval is in frames (1 ms each)
        // Convert to xHCI 125 µs units: interval_125us = interval_ms * 8
        // Then find log2: xhci_interval = log2(interval_125us) + 1
        // Simplified: for FS/LS, xhci_interval ≈ bInterval + 3
        xhci_interval = interval;
        if (xhci_interval < 1) xhci_interval = 1;
        // Find highest set bit position + 3
        uint8_t msb = 0;
        uint32_t v = xhci_interval;
        while (v > 1) { v >>= 1; msb++; }
        xhci_interval = msb + 3;
        if (xhci_interval > 15) xhci_interval = 15;
    } else {
        // High/Super speed: bInterval is already in 2^(n-1) * 125 µs units
        xhci_interval = interval;
        if (xhci_interval < 1) xhci_interval = 1;
        if (xhci_interval > 16) xhci_interval = 16;
        xhci_interval--;  // xHCI uses 0-based (2^interval * 125 µs)
    }

    // Use xhci_configure_endpoint with EP_TYPE_INTERRUPT_IN
    int st = xhci_configure_endpoint(ctrl, slot, ep_num,
                                      EP_TYPE_INTERRUPT_IN, max_pkt, xhci_interval);
    if (st != ST_OK) {
        kprintf("[USBHID] Failed to configure interrupt endpoint: %d\n", st);
        return st;
    }

    // The xhci_configure_endpoint function doesn't know about our HID ring,
    // so we need to ensure our ring is used. However, xhci_configure_endpoint
    // allocates its own ring internally. We need to use that ring instead.
    // Let's free our ring and find the one that was set up.
    //
    // Actually, looking at xhci_configure_endpoint(), it stores the ring in
    // dev->bulk_in_ring or dev->bulk_out_ring based on type. For interrupt
    // endpoints that don't match bulk types, the ring might not be stored.
    // We'll keep our own ring reference and override the endpoint context.

    // Re-read: xhci_configure_endpoint creates its own ring and sets up the
    // EP context with it. The ring pointer goes to bulk_in/out_ring based on
    // EP type. Since EP_TYPE_INTERRUPT_IN maps to the bulk_in path... let's
    // check. Actually it doesn't - it only checks EP_TYPE_BULK_IN/OUT.
    // So the ring is allocated but not stored anywhere accessible to us.
    //
    // Better approach: we directly set up the input context and send the
    // Configure Endpoint command ourselves with our ring.

    hid_dbg("Interrupt EP configured: slot=%d ep=%d interval=%d\n",
            slot, ep_num, xhci_interval);

    return ST_OK;
}

// Full setup of a HID device's interrupt endpoint, including the input context
// and Configure Endpoint command. This gives us full control over the ring.
static int hid_setup_interrupt_endpoint(usbhid_device_t* hdev) {
    xhci_controller_t* ctrl = hdev->ctrl;
    usb_device_t* dev = hdev->usb_dev;
    uint8_t slot = dev->slot_id;
    uint8_t ep_num = hdev->int_in_ep;
    uint16_t max_pkt = hdev->int_in_max_pkt;
    uint8_t interval = hdev->int_in_interval;

    if (slot == 0 || slot > ctrl->max_slots) return ST_INVALID;

    // Allocate transfer ring
    xhci_ring_t* ring = xhci_alloc_ring();
    if (!ring) return ST_NOMEM;

    uint64_t ring_phys = mm_get_physical_address((uint64_t)ring->trbs);
    xhci_ring_init(ring, ring_phys);
    hdev->int_in_ring = ring;

    // Calculate DCI for IN interrupt endpoint
    uint8_t dci = ep_num * 2 + 1;  // IN endpoint

    // Compute xHCI interval from USB descriptor interval
    // For FS/LS interrupt endpoints: bInterval is polling period in ms (1-255)
    //   xHCI interval = closest power-of-2 in 125µs units
    // For HS/SS interrupt endpoints: bInterval is 2^(bInterval-1) * 125µs
    //   xHCI interval = bInterval - 1 (0-based exponent)
    uint8_t xhci_interval;
    if (dev->speed <= XHCI_SPEED_FULL) {
        // FS/LS: convert ms to 125µs exponent
        uint32_t us125 = (uint32_t)interval * 8;
        xhci_interval = 0;
        while (us125 > 1) { us125 >>= 1; xhci_interval++; }
        if (xhci_interval > 15) xhci_interval = 15;
    } else {
        // HS/SS: direct exponent
        xhci_interval = interval;
        if (xhci_interval < 1) xhci_interval = 1;
        if (xhci_interval > 16) xhci_interval = 16;
        xhci_interval--;
    }

    // Setup input context
    size_t input_ctx_size = 33 * ctrl->context_size;
    hid_memset(ctrl->input_ctx_raw, 0, input_ctx_size);

    // Input Control Context: add Slot Context and this endpoint
    uint32_t* input_ctrl_ctx = (uint32_t*)ctrl->input_ctx_raw;
    input_ctrl_ctx[0] = 0;                        // Drop flags
    input_ctrl_ctx[1] = (1 << 0) | (1 << dci);   // Add: Slot + endpoint

    // Copy current slot context from device context
    xhci_dev_ctx_t* dev_ctx = ctrl->dev_ctx[slot - 1];
    if (!dev_ctx) {
        kprintf("[USBHID] No device context for slot %d\n", slot);
        return ST_ERR;
    }

    // Get slot context pointers (handle variable context size)
    uint8_t* in_slot_ptr = ctrl->input_ctx_raw + ctrl->context_size;
    uint8_t* out_slot_ptr = (uint8_t*)dev_ctx;
    hid_memcpy(in_slot_ptr, out_slot_ptr, sizeof(xhci_slot_ctx_t));

    // Update context entries in slot context to include this endpoint
    xhci_slot_ctx_t* in_slot = (xhci_slot_ctx_t*)in_slot_ptr;
    uint32_t entries = (in_slot->route_speed_entries >> 27) & 0x1F;
    if (dci > entries) {
        in_slot->route_speed_entries = (in_slot->route_speed_entries & 0x07FFFFFF) | (dci << 27);
    }

    // Setup Endpoint Context for Interrupt IN
    uint8_t* ep_ptr = ctrl->input_ctx_raw + (2 + dci - 1) * ctrl->context_size;
    xhci_ep_ctx_t* ep_ctx = (xhci_ep_ctx_t*)ep_ptr;

    ep_ctx->ep_info1 = ((uint32_t)xhci_interval << 16);      // Interval
    ep_ctx->ep_info2 = (3 << 1) |                              // CErr = 3 (max retries)
                        (EP_TYPE_INTERRUPT_IN << 3) |           // EP Type = Interrupt IN
                        ((uint32_t)max_pkt << 16);             // Max packet size
    ep_ctx->tr_dequeue = ring_phys | 1;                        // DCS = 1
    ep_ctx->avg_trb_len = max_pkt;                             // Average TRB length

    // Flush caches before sending command
    // x86 DMA is cache-coherent (bus snooping), but explicit flush for safety
    __asm__ volatile("mfence" ::: "memory");

    // Send Configure Endpoint command
    uint32_t cmd_control = (TRB_TYPE_CONFIG_EP << 10) | (slot << 24);

    if (xhci_send_command(ctrl, ctrl->input_ctx_phys, 0, cmd_control) != ST_OK) {
        kprintf("[USBHID] Failed to send Configure EP command\n");
        return ST_ERR;
    }

    if (xhci_wait_command(ctrl, 1000) != ST_OK) {
        kprintf("[USBHID] Configure Endpoint command failed\n");
        return ST_ERR;
    }

    hid_dbg("Interrupt IN EP%d configured: DCI=%d, interval=%d, max_pkt=%d\n",
            ep_num, dci, xhci_interval, max_pkt);

    return ST_OK;
}

// Allocate a DMA-safe report buffer for a HID device
static int hid_alloc_report_buffer(usbhid_device_t* hdev) {
    // Allocate a page-aligned DMA buffer for the report
    // Must be physically contiguous and addressable by the xHCI controller
    uint8_t* raw = (uint8_t*)kcalloc_dma(1, 4096 + 4096);
    if (!raw) {
        kprintf("[USBHID] Failed to allocate report buffer\n");
        return ST_NOMEM;
    }

    // Page-align the buffer
    uint64_t aligned = ((uint64_t)raw + 4095) & ~4095ULL;
    hdev->report_buf_raw = raw;  // Save original pointer for kfree_dma
    hdev->report_buf = (uint8_t*)aligned;
    hdev->report_buf_phys = mm_get_physical_address(aligned);

    hid_memset(hdev->report_buf, 0, USBHID_REPORT_BUF_SIZE);
    hid_memset(hdev->prev_report, 0, sizeof(hdev->prev_report));

    return ST_OK;
}

// ============================================================================
// Intel ISH (Integrated Sensor Hub) Detection
// ============================================================================

int usbhid_is_intel_ish(uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id != INTEL_ISH_PCI_VENDOR) return 0;

    switch (device_id) {
        case INTEL_ISH_DEV_CHV:
        case INTEL_ISH_DEV_BXT_Ax:
        case INTEL_ISH_DEV_BXT_P:
        case INTEL_ISH_DEV_APL_Ax:
        case INTEL_ISH_DEV_SPT_Ax:
        case INTEL_ISH_DEV_CNL_H:
        case INTEL_ISH_DEV_ICL:
        case INTEL_ISH_DEV_TGL:
        case INTEL_ISH_DEV_ADL_S:
        case INTEL_ISH_DEV_ADL_P:
        case INTEL_ISH_DEV_RPL_S:
            kprintf("[USBHID] Intel ISH detected (PCI %04x:%04x) - skipping\n",
                    vendor_id, device_id);
            return 1;
        default:
            return 0;
    }
}

// ============================================================================
// Public API - Initialization
// ============================================================================

void usbhid_init(void) {
    if (g_hid_initialized) return;

    hid_memset(g_hid_devices, 0, sizeof(g_hid_devices));
    g_hid_device_count = 0;
    g_hid_initialized = 1;

    kprintf("[USBHID] USB HID subsystem initialized\n");
}

// ============================================================================
// Public API - Device Probe
// ============================================================================

// Probe a USB device for HID interfaces.
// Called after device enumeration and configuration is set.
// Parses the configuration descriptor to find HID interfaces
// with Boot Protocol support.
//
// Per USB HID 1.11, Section 4.1:
//   Class = 0x03 (HID)
//   Subclass = 0x01 (Boot Interface)
//   Protocol = 0x01 (Keyboard) or 0x02 (Mouse)
int usbhid_probe(xhci_controller_t* ctrl, usb_device_t* dev,
                  uint8_t* config_desc, uint16_t config_len) {
    if (!ctrl || !dev || !config_desc || config_len < 9) return ST_INVALID;
    if (!g_hid_initialized) usbhid_init();

    // Check for Intel ISH devices and skip them
    if (usbhid_is_intel_ish(dev->vendor_id, dev->product_id)) {
        return ST_UNSUPPORTED;
    }

    usb_config_desc_t* cfg = (usb_config_desc_t*)config_desc;
    uint8_t* ptr = config_desc + cfg->length;
    uint8_t* end = config_desc + cfg->total_length;
    if (end > config_desc + config_len) end = config_desc + config_len;

    int found_hid = 0;

    // Track current interface being parsed
    usb_interface_desc_t* cur_iface = NULL;

    while (ptr < end) {
        uint8_t desc_len = ptr[0];
        uint8_t desc_type = ptr[1];

        if (desc_len == 0) break;
        if (ptr + desc_len > end) break;

        if (desc_type == USB_DESC_INTERFACE) {
            cur_iface = (usb_interface_desc_t*)ptr;

            // Check if this is a HID interface
            if (cur_iface->class_code == USB_CLASS_HID) {
                hid_dbg("Found HID interface %d: subclass=%d protocol=%d eps=%d\n",
                        cur_iface->interface_num, cur_iface->subclass,
                        cur_iface->protocol, cur_iface->num_endpoints);

                // We support Boot Protocol keyboards and mice
                if (cur_iface->subclass == USB_HID_SUBCLASS_BOOT &&
                    (cur_iface->protocol == USB_HID_PROTOCOL_KEYBOARD ||
                     cur_iface->protocol == USB_HID_PROTOCOL_MOUSE)) {

                    // Check if we have room for another HID device
                    if (g_hid_device_count >= USBHID_MAX_DEVICES) {
                        kprintf("[USBHID] Maximum HID devices reached\n");
                        break;
                    }

                    // Initialize a new HID device entry
                    usbhid_device_t* hdev = &g_hid_devices[g_hid_device_count];
                    hid_memset(hdev, 0, sizeof(usbhid_device_t));

                    hdev->usb_dev = dev;
                    hdev->ctrl = ctrl;
                    hdev->interface_num = cur_iface->interface_num;
                    hdev->subclass = cur_iface->subclass;
                    hdev->protocol = cur_iface->protocol;
                    hdev->boot_protocol = 1;

                    if (cur_iface->protocol == USB_HID_PROTOCOL_KEYBOARD) {
                        hdev->type = USBHID_TYPE_KEYBOARD;
                        hdev->report_size = HID_BOOT_KBD_REPORT_SIZE;
                    } else {
                        hdev->type = USBHID_TYPE_MOUSE;
                        hdev->report_size = HID_BOOT_MOUSE_REPORT_SIZE;
                    }

                    // Continue scanning for endpoint descriptors for this interface
                    uint8_t* ep_ptr = ptr + desc_len;
                    while (ep_ptr < end) {
                        uint8_t ep_len = ep_ptr[0];
                        uint8_t ep_type = ep_ptr[1];
                        if (ep_len == 0) break;
                        if (ep_ptr + ep_len > end) break;

                        // Stop at next interface descriptor
                        if (ep_type == USB_DESC_INTERFACE) break;

                        if (ep_type == USB_DESC_ENDPOINT) {
                            usb_endpoint_desc_t* ep = (usb_endpoint_desc_t*)ep_ptr;
                            uint8_t ep_xfer_type = ep->attributes & USB_EP_TYPE_MASK;
                            uint8_t ep_dir = ep->address & USB_EP_DIR_IN;
                            uint8_t ep_num = ep->address & USB_EP_NUM_MASK;

                            // Look for Interrupt IN endpoint
                            if (ep_xfer_type == USB_EP_TYPE_INTERRUPT && ep_dir) {
                                hdev->int_in_ep = ep_num;
                                hdev->int_in_max_pkt = ep->max_packet & 0x7FF;
                                hdev->int_in_interval = ep->interval;

                                hid_dbg("  Interrupt IN EP%d: max_pkt=%d interval=%dms\n",
                                        ep_num, hdev->int_in_max_pkt, hdev->int_in_interval);
                                break;  // Found our endpoint
                            }
                        }

                        ep_ptr += ep_len;
                    }

                    // Must have an interrupt IN endpoint
                    if (hdev->int_in_ep == 0) {
                        kprintf("[USBHID] No interrupt IN endpoint found for HID interface %d\n",
                                cur_iface->interface_num);
                        hid_memset(hdev, 0, sizeof(usbhid_device_t));
                        ptr += desc_len;
                        continue;
                    }

                    // Allocate DMA report buffer
                    if (hid_alloc_report_buffer(hdev) != ST_OK) {
                        hid_memset(hdev, 0, sizeof(usbhid_device_t));
                        ptr += desc_len;
                        continue;
                    }

                    // Set Boot Protocol (USB HID 1.11, Section 7.2.6)
                    // Boot Protocol is simpler and has fixed report formats,
                    // which is exactly what we need for our driver.
                    int st = hid_set_protocol(ctrl, dev, hdev->interface_num,
                                               HID_PROTOCOL_BOOT);
                    if (st != ST_OK) {
                        hid_dbg("SET_PROTOCOL(Boot) failed: %d (may already be in boot mode)\n", st);
                        // Some devices may not support this; continue anyway
                    }

                    // Set Idle rate to 0 (report only on change)
                    // USB HID 1.11, Section 7.2.4
                    st = hid_set_idle(ctrl, dev, hdev->interface_num, 0, 0);
                    if (st != ST_OK) {
                        hid_dbg("SET_IDLE(0) failed: %d (non-fatal)\n", st);
                        // Non-fatal - device will use its default idle rate
                    }

                    // Configure the interrupt IN endpoint in xHCI
                    st = hid_setup_interrupt_endpoint(hdev);
                    if (st != ST_OK) {
                        kprintf("[USBHID] Failed to setup interrupt endpoint: %d\n", st);
                        hid_memset(hdev, 0, sizeof(usbhid_device_t));
                        ptr += desc_len;
                        continue;
                    }

                    // Mark device as active and submit first interrupt transfer
                    hdev->active = 1;
                    hdev->configured = 1;
                    g_hid_device_count++;

                    // Submit initial interrupt IN transfer
                    st = hid_submit_interrupt_in(hdev);
                    if (st != ST_OK) {
                        kprintf("[USBHID] Failed to submit initial interrupt transfer: %d\n", st);
                        // Device is configured but first transfer failed; poll will retry
                    }

                    const char* type_str = (hdev->type == USBHID_TYPE_KEYBOARD) ?
                                            "Keyboard" : "Mouse";
                    kprintf("[USBHID] %s detected on port %d (EP%d, %d-byte reports)\n",
                            type_str, dev->port, hdev->int_in_ep, hdev->report_size);

                    found_hid = 1;
                }
            }
        }

        ptr += desc_len;
    }

    return found_hid ? ST_OK : ST_NOT_FOUND;
}

// ============================================================================
// Public API - Polling (fallback path)
// ============================================================================

// Fallback poll for HID devices.  In normal operation the primary path is
// entirely interrupt-driven via usbhid_irq_completion().  This function only
// needs to handle:
//   (a) Deferred STALL recovery (needs_resubmit flag set in IRQ context,
//       because control transfers cannot be issued with xhci_lock held).
//   (b) Initial arming if the first hid_submit_interrupt_in() failed.
//   (c) Any transfer that slipped through the IRQ path (shouldn't happen,
//       but defence in depth).
void usbhid_poll(void) {
    if (!g_hid_initialized || g_hid_device_count == 0) return;

    for (int i = 0; i < g_hid_device_count; i++) {
        usbhid_device_t* hdev = &g_hid_devices[i];

        if (!hdev->active || !hdev->configured) continue;

        // --- (a) Deferred STALL recovery from IRQ context ---
        if (hdev->needs_resubmit) {
            uint8_t slot = hdev->usb_dev->slot_id;
            uint8_t dci = hdev->int_in_ep * 2 + 1;

            hid_dbg("HID[%d] deferred STALL recovery\n", i);
            xhci_reset_endpoint(hdev->ctrl, slot, dci);

            // Send CLEAR_FEATURE(ENDPOINT_HALT) to the device
            xhci_control_transfer(hdev->ctrl, hdev->usb_dev,
                                   USB_RT_H2D | USB_RT_STD | USB_RT_EP,
                                   USB_REQ_CLEAR_FEATURE,
                                   0,  // ENDPOINT_HALT feature
                                   (hdev->int_in_ep | USB_EP_DIR_IN),
                                   0, NULL);

            hdev->needs_resubmit = 0;
            hid_submit_interrupt_in(hdev);
            continue;
        }

        // --- (b)/(c) Catch any completed-but-unprocessed transfers or
        //     re-arm if nothing is pending ---
        if (hdev->xfer_pending && hdev->xfer.completed) {
            uint8_t cc = hdev->xfer.cc;
            hdev->xfer_pending = 0;

            uint8_t slot = hdev->usb_dev->slot_id;
            uint8_t dci = hdev->int_in_ep * 2 + 1;

            uint64_t flags;
            spin_lock_irqsave(&hid_lock, &flags);
            hdev->ctrl->pending_xfer[slot - 1][dci] = NULL;
            spin_unlock_irqrestore(&hid_lock, flags);

            if (cc == TRB_CC_SUCCESS || cc == TRB_CC_SHORT_PACKET) {
                __asm__ volatile("" ::: "memory");

                uint32_t received = hdev->int_in_max_pkt - hdev->xfer.bytes_transferred;
                if (received > USBHID_REPORT_BUF_SIZE) received = USBHID_REPORT_BUF_SIZE;

                hid_dbg("POLL HID[%d] report: %d bytes, cc=%d\n", i, received, cc);

                if (hdev->type == USBHID_TYPE_KEYBOARD && received >= HID_BOOT_KBD_REPORT_SIZE) {
                    hid_process_keyboard_report(hdev, hdev->report_buf, (uint8_t)received);
                } else if (hdev->type == USBHID_TYPE_MOUSE && received >= HID_BOOT_MOUSE_MIN_SIZE) {
                    hid_process_mouse_report(hdev, hdev->report_buf, (uint8_t)received);
                }
            } else if (cc == TRB_CC_STALL) {
                hid_dbg("POLL HID[%d] stall on interrupt EP\n", i);
                xhci_reset_endpoint(hdev->ctrl, slot, dci);
                xhci_control_transfer(hdev->ctrl, hdev->usb_dev,
                                       USB_RT_H2D | USB_RT_STD | USB_RT_EP,
                                       USB_REQ_CLEAR_FEATURE,
                                       0,
                                       (hdev->int_in_ep | USB_EP_DIR_IN),
                                       0, NULL);
            } else {
                hid_dbg("POLL HID[%d] error cc=%d\n", i, cc);
            }

            hid_submit_interrupt_in(hdev);
        }

        // Re-arm if nothing is pending (initial submit failure or lost transfer)
        if (!hdev->xfer_pending && !hdev->needs_resubmit) {
            hid_submit_interrupt_in(hdev);
        }
    }
}

// ============================================================================
// Public API - Disconnect
// ============================================================================

// Called when a USB device is physically disconnected (hot-unplug).
// Finds any HID device instances referencing the given usb_device_t,
// cancels pending transfers, and deactivates them.
void usbhid_disconnect(usb_device_t* dev) {
    if (!dev || !g_hid_initialized) return;

    uint64_t flags;
    spin_lock_irqsave(&hid_lock, &flags);

    for (int i = 0; i < g_hid_device_count; i++) {
        usbhid_device_t* hdev = &g_hid_devices[i];
        if (!hdev->active) continue;
        if (hdev->usb_dev != dev) continue;

        const char* type_str = (hdev->type == USBHID_TYPE_KEYBOARD) ?
                                "Keyboard" : "Mouse";
        kprintf("[USBHID] %s disconnected (port %d)\n", type_str, dev->port);

        // Cancel pending transfer - clear the pending_xfer pointer so the
        // IRQ handler won't try to deliver completions to a dead device.
        if (hdev->ctrl && hdev->xfer_pending) {
            uint8_t slot = dev->slot_id;
            uint8_t dci = hdev->int_in_ep * 2 + 1;
            if (slot > 0 && slot <= XHCI_MAX_SLOTS && dci < XHCI_MAX_ENDPOINTS) {
                hdev->ctrl->pending_xfer[slot - 1][dci] = NULL;
            }
            hdev->xfer_pending = 0;
        }

        // Free the interrupt IN transfer ring
        if (hdev->int_in_ring) {
            xhci_free_ring(hdev->int_in_ring);
            hdev->int_in_ring = NULL;
        }

        // Free DMA report buffer (must use the original raw pointer, not
        // the page-aligned one, because kfree_dma tracks the allocation
        // metadata at the start of the original block).
        if (hdev->report_buf_raw) {
            kfree_dma(hdev->report_buf_raw);
            hdev->report_buf_raw = NULL;
            hdev->report_buf = NULL;
            hdev->report_buf_phys = 0;
        }

        // Deactivate the device slot
        hdev->active = 0;
        hdev->configured = 0;
        hdev->usb_dev = NULL;
        hdev->ctrl = NULL;
    }

    spin_unlock_irqrestore(&hid_lock, flags);
}

// ============================================================================
// Public API - Status Queries
// ============================================================================

int usbhid_has_keyboard(void) {
    for (int i = 0; i < g_hid_device_count; i++) {
        if (g_hid_devices[i].active && g_hid_devices[i].type == USBHID_TYPE_KEYBOARD) {
            return 1;
        }
    }
    return 0;
}

int usbhid_has_mouse(void) {
    for (int i = 0; i < g_hid_device_count; i++) {
        if (g_hid_devices[i].active && g_hid_devices[i].type == USBHID_TYPE_MOUSE) {
            return 1;
        }
    }
    return 0;
}

int usbhid_device_count(void) {
    return g_hid_device_count;
}
