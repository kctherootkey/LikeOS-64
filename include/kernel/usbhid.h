// LikeOS-64 - USB HID (Human Interface Device) Driver
// Implements USB HID Class specification 1.11
// Supports Boot Protocol keyboards and mice per USB HID spec Chapter 7/8
// Also handles Intel HID-ISHTP sensor hub discovery (graceful skip)
//
// References:
//   - USB Device Class Definition for Human Interface Devices (HID) 1.11
//   - USB HID Usage Tables 1.12
//   - Intel Integrated Sensor Hub (ISH) / HID-over-ISHTP (skip/ignore)

#ifndef LIKEOS_USBHID_H
#define LIKEOS_USBHID_H

#include "types.h"
#include "status.h"
#include "xhci.h"
#include "usb.h"

// ============================================================================
// USB HID Class Codes (USB HID 1.11, Section 4.1)
// ============================================================================

#define USB_CLASS_HID               0x03

// HID Subclass codes (USB HID 1.11, Section 4.2)
#define USB_HID_SUBCLASS_NONE       0x00    // No subclass
#define USB_HID_SUBCLASS_BOOT       0x01    // Boot Interface Subclass

// HID Protocol codes (USB HID 1.11, Section 4.3)
#define USB_HID_PROTOCOL_NONE       0x00    // None
#define USB_HID_PROTOCOL_KEYBOARD   0x01    // Keyboard
#define USB_HID_PROTOCOL_MOUSE      0x02    // Mouse

// ============================================================================
// HID Descriptor Types (USB HID 1.11, Section 7.1)
// ============================================================================

#define USB_DESC_HID                0x21    // HID class descriptor
#define USB_DESC_HID_REPORT         0x22    // Report descriptor
#define USB_DESC_HID_PHYSICAL       0x23    // Physical descriptor

// ============================================================================
// HID Class-Specific Requests (USB HID 1.11, Section 7.2)
// ============================================================================

#define HID_REQ_GET_REPORT          0x01
#define HID_REQ_GET_IDLE            0x02
#define HID_REQ_GET_PROTOCOL        0x03
#define HID_REQ_SET_REPORT          0x09
#define HID_REQ_SET_IDLE            0x0A
#define HID_REQ_SET_PROTOCOL        0x0B

// HID Report Types (USB HID 1.11, Section 7.2.1)
#define HID_REPORT_TYPE_INPUT       0x01
#define HID_REPORT_TYPE_OUTPUT      0x02
#define HID_REPORT_TYPE_FEATURE     0x03

// HID Protocol values (USB HID 1.11, Section 7.2.6)
#define HID_PROTOCOL_BOOT           0x00
#define HID_PROTOCOL_REPORT         0x01

// ============================================================================
// HID Boot Protocol - Keyboard Report (USB HID 1.11, Appendix B.1)
// ============================================================================

// Boot keyboard report: 8 bytes
// Byte 0: Modifier keys bitmap
// Byte 1: Reserved (OEM)
// Byte 2-7: Key codes (up to 6 simultaneous keys)
#define HID_BOOT_KBD_REPORT_SIZE    8
#define HID_BOOT_KBD_MAX_KEYS      6

// Modifier key bits (Byte 0 of boot keyboard report)
#define HID_KBD_MOD_LCTRL          (1 << 0)
#define HID_KBD_MOD_LSHIFT         (1 << 1)
#define HID_KBD_MOD_LALT           (1 << 2)
#define HID_KBD_MOD_LGUI           (1 << 3)
#define HID_KBD_MOD_RCTRL          (1 << 4)
#define HID_KBD_MOD_RSHIFT         (1 << 5)
#define HID_KBD_MOD_RALT           (1 << 6)
#define HID_KBD_MOD_RGUI           (1 << 7)

// Special HID keyboard key codes (USB HID Usage Tables, Section 10)
#define HID_KEY_NONE               0x00
#define HID_KEY_ERR_ROLLOVER       0x01    // ErrorRollOver
#define HID_KEY_POST_FAIL          0x02    // POSTFail
#define HID_KEY_ERR_UNDEFINED      0x03    // ErrorUndefined
#define HID_KEY_A                  0x04
#define HID_KEY_Z                  0x1D
#define HID_KEY_1                  0x1E
#define HID_KEY_0                  0x27
#define HID_KEY_ENTER              0x28
#define HID_KEY_ESC                0x29
#define HID_KEY_BACKSPACE          0x2A
#define HID_KEY_TAB                0x2B
#define HID_KEY_SPACE              0x2C
#define HID_KEY_MINUS              0x2D
#define HID_KEY_EQUAL              0x2E
#define HID_KEY_LBRACKET           0x2F
#define HID_KEY_RBRACKET           0x30
#define HID_KEY_BACKSLASH          0x31
#define HID_KEY_SEMICOLON          0x33
#define HID_KEY_APOSTROPHE         0x34
#define HID_KEY_GRAVE              0x35
#define HID_KEY_COMMA              0x36
#define HID_KEY_DOT                0x37
#define HID_KEY_SLASH              0x38
#define HID_KEY_CAPSLOCK           0x39
#define HID_KEY_F1                 0x3A
#define HID_KEY_F2                 0x3B
#define HID_KEY_F3                 0x3C
#define HID_KEY_F4                 0x3D
#define HID_KEY_F5                 0x3E
#define HID_KEY_F6                 0x3F
#define HID_KEY_F7                 0x40
#define HID_KEY_F8                 0x41
#define HID_KEY_F9                 0x42
#define HID_KEY_F10                0x43
#define HID_KEY_F11                0x44
#define HID_KEY_F12                0x45
#define HID_KEY_PRINTSCREEN        0x46
#define HID_KEY_SCROLLLOCK         0x47
#define HID_KEY_PAUSE              0x48
#define HID_KEY_INSERT             0x49
#define HID_KEY_HOME               0x4A
#define HID_KEY_PAGEUP             0x4B
#define HID_KEY_DELETE             0x4C
#define HID_KEY_END                0x4D
#define HID_KEY_PAGEDOWN           0x4E
#define HID_KEY_RIGHT              0x4F
#define HID_KEY_LEFT               0x50
#define HID_KEY_DOWN               0x51
#define HID_KEY_UP                 0x52
#define HID_KEY_NUMLOCK            0x53
#define HID_KEY_KP_SLASH           0x54
#define HID_KEY_KP_ASTERISK        0x55
#define HID_KEY_KP_MINUS           0x56
#define HID_KEY_KP_PLUS            0x57
#define HID_KEY_KP_ENTER           0x58
#define HID_KEY_KP_1               0x59
#define HID_KEY_KP_2               0x5A
#define HID_KEY_KP_3               0x5B
#define HID_KEY_KP_4               0x5C
#define HID_KEY_KP_5               0x5D
#define HID_KEY_KP_6               0x5E
#define HID_KEY_KP_7               0x5F
#define HID_KEY_KP_8               0x60
#define HID_KEY_KP_9               0x61
#define HID_KEY_KP_0               0x62
#define HID_KEY_KP_DOT             0x63

// ============================================================================
// HID Boot Protocol - Mouse Report (USB HID 1.11, Appendix B.2)
// ============================================================================

// Boot mouse report: 3-4 bytes
// Byte 0: Button bitmap (bits 0-2 = left, right, middle)
// Byte 1: X displacement (signed 8-bit)
// Byte 2: Y displacement (signed 8-bit)
// Byte 3: Wheel (optional, signed 8-bit, IntelliMouse extension)
#define HID_BOOT_MOUSE_REPORT_SIZE  4   // 3 standard + 1 wheel
#define HID_BOOT_MOUSE_MIN_SIZE     3   // Minimum without wheel

// Mouse button bits (Byte 0 of boot mouse report)
#define HID_MOUSE_BTN_LEFT         (1 << 0)
#define HID_MOUSE_BTN_RIGHT        (1 << 1)
#define HID_MOUSE_BTN_MIDDLE       (1 << 2)

// ============================================================================
// HID Class Descriptor (USB HID 1.11, Section 6.2.1)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  length;            // Size of this descriptor
    uint8_t  type;              // USB_DESC_HID (0x21)
    uint16_t hid_version;       // HID spec release (BCD, e.g. 0x0111 for 1.11)
    uint8_t  country_code;      // Country code (0 = not localized)
    uint8_t  num_descriptors;   // Number of class descriptors (at least 1)
    uint8_t  desc_type;         // Type of first class descriptor (0x22 = Report)
    uint16_t desc_length;       // Length of first class descriptor
} usb_hid_desc_t;

// ============================================================================
// USB HID Device Instance
// ============================================================================

#define USBHID_MAX_DEVICES      8       // Maximum simultaneous HID devices
#define USBHID_REPORT_BUF_SIZE  64      // Max report buffer size
#define USBHID_POLL_INTERVAL_MS 10      // Default polling interval (ms)

// HID device type
typedef enum {
    USBHID_TYPE_UNKNOWN = 0,
    USBHID_TYPE_KEYBOARD,
    USBHID_TYPE_MOUSE,
    USBHID_TYPE_OTHER
} usbhid_type_t;

// USB HID device state
typedef struct {
    // USB device back-reference
    usb_device_t*        usb_dev;
    xhci_controller_t*   ctrl;

    // HID device identification
    usbhid_type_t        type;
    uint8_t              interface_num;
    uint8_t              subclass;       // 0=no subclass, 1=boot
    uint8_t              protocol;       // 0=none, 1=keyboard, 2=mouse

    // Interrupt IN endpoint for input reports
    uint8_t              int_in_ep;      // Endpoint number (1-15)
    uint16_t             int_in_max_pkt; // Max packet size
    uint8_t              int_in_interval;// Polling interval (ms)
    xhci_ring_t*         int_in_ring;    // Transfer ring for interrupt IN

    // Report buffers (DMA-safe)
    uint8_t*             report_buf_raw; // Original kcalloc_dma pointer (for kfree_dma)
    uint8_t*             report_buf;     // Current report (DMA buffer, page-aligned)
    uint64_t             report_buf_phys;// Physical address of report buffer
    uint8_t              report_size;    // Size of reports for this device
    uint8_t              prev_report[USBHID_REPORT_BUF_SIZE]; // Previous report for delta

    // Keyboard-specific state
    uint8_t              kbd_modifiers;  // Current modifier key state
    uint8_t              kbd_leds;       // LED state (Num/Caps/Scroll)

    // State tracking
    uint8_t              active;         // Device is active and polling
    uint8_t              configured;     // Device has been configured
    uint8_t              boot_protocol;  // Using boot protocol (vs report protocol)
    uint8_t              needs_resubmit; // Transfer completed, needs re-arm

    // Pending interrupt transfer
    xhci_transfer_t      xfer;          // Transfer completion tracking
    uint8_t              xfer_pending;   // Transfer is in-flight
} usbhid_device_t;

// ============================================================================
// Intel Sensor Hub / HID-ISHTP Detection
// ============================================================================

// Intel ISH (Integrated Sensor Hub) PCI class/vendor IDs
// The ISH appears as a PCI device with class 0x1180 (Signal processing controller)
// Linux handles it through intel-ish-hid driver; we just detect and skip it.
#define INTEL_ISH_PCI_VENDOR       0x8086
#define INTEL_ISH_PCI_CLASS        0x1180  // Signal processing controller

// Known Intel ISH device IDs (from Linux intel-ish-hid driver)
#define INTEL_ISH_DEV_CHV          0x22D8  // Cherry View
#define INTEL_ISH_DEV_BXT_Ax      0x0AA2  // Broxton A-step
#define INTEL_ISH_DEV_BXT_P       0x1AA2  // Broxton P
#define INTEL_ISH_DEV_APL_Ax      0x5AA2  // Apollo Lake
#define INTEL_ISH_DEV_SPT_Ax      0xA135  // Sunrise Point
#define INTEL_ISH_DEV_KBL         0xA135  // Kaby Lake
#define INTEL_ISH_DEV_CNL_H       0xA37C  // Cannon Lake H
#define INTEL_ISH_DEV_ICL         0x34FC  // Ice Lake
#define INTEL_ISH_DEV_TGL         0xA0FC  // Tiger Lake
#define INTEL_ISH_DEV_ADL_S       0x7AF8  // Alder Lake S
#define INTEL_ISH_DEV_ADL_P       0x51FC  // Alder Lake P
#define INTEL_ISH_DEV_RPL_S       0x7A78  // Raptor Lake S

// ============================================================================
// Public API
// ============================================================================

// Initialize the USB HID subsystem
void usbhid_init(void);

// Probe a newly enumerated USB device for HID interfaces
// Called from xhci_enumerate_device after configuration is set
// Returns ST_OK if a HID device was found and configured
int usbhid_probe(xhci_controller_t* ctrl, usb_device_t* dev,
                  uint8_t* config_desc, uint16_t config_len);

// IRQ-context completion handler.
// Called directly from xhci_handle_transfer_event() when an interrupt
// transfer completes on a HID endpoint.  Processes the report and
// re-submits the next transfer entirely in IRQ context for minimum
// latency.  Returns 1 if the transfer was consumed by a HID device.
int usbhid_irq_completion(xhci_controller_t* ctrl, uint8_t slot, uint8_t ep_id,
                          uint8_t cc, uint32_t residue);

// Poll all active HID devices for new input (fallback path).
// Called from the main loop; only needed if an IRQ-context re-submit
// failed or for initial arming.
void usbhid_poll(void);

// Disconnect a USB HID device (called when the USB device is unplugged).
// Deactivates any HID device instance associated with the given usb_device_t.
void usbhid_disconnect(usb_device_t* dev);

// Check if USB HID keyboard/mouse is active (for status reporting)
int usbhid_has_keyboard(void);
int usbhid_has_mouse(void);

// Get number of active HID devices
int usbhid_device_count(void);

// Check if a PCI device is an Intel ISH (to skip during USB enumeration)
int usbhid_is_intel_ish(uint16_t vendor_id, uint16_t device_id);

#endif // LIKEOS_USBHID_H
