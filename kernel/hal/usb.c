// LikeOS-64 - USB Core Utilities
// Generic USB helper functions

#include "../../include/kernel/usb.h"
#include "../../include/kernel/console.h"

// USB class name lookup
const char* usb_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "Defined at interface";
        case 0x01: return "Audio";
        case 0x02: return "Communications";
        case USB_CLASS_HID: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case USB_CLASS_MASS_STORAGE: return "Mass Storage";
        case USB_CLASS_HUB: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "Smart Card";
        case 0x0D: return "Content Security";
        case 0x0E: return "Video";
        case 0x0F: return "Personal Healthcare";
        case 0xDC: return "Diagnostic";
        case 0xE0: return "Wireless Controller";
        case 0xEF: return "Miscellaneous";
        case 0xFE: return "Application Specific";
        case 0xFF: return "Vendor Specific";
        default: return "Unknown";
    }
}

// USB speed name lookup
const char* usb_speed_name(uint8_t speed) {
    switch (speed) {
        case 1: return "Full Speed (12 Mbps)";
        case 2: return "Low Speed (1.5 Mbps)";
        case 3: return "High Speed (480 Mbps)";
        case 4: return "Super Speed (5 Gbps)";
        case 5: return "Super Speed+ (10 Gbps)";
        default: return "Unknown";
    }
}
