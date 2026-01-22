// LikeOS-64 - USB Device Definitions
#ifndef LIKEOS_USB_H
#define LIKEOS_USB_H

#include "types.h"

// USB descriptor types
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIG         2
#define USB_DESC_STRING         3
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5

// USB request types
#define USB_REQ_GET_STATUS      0
#define USB_REQ_CLEAR_FEATURE   1
#define USB_REQ_SET_FEATURE     3
#define USB_REQ_SET_ADDRESS     5
#define USB_REQ_GET_DESCRIPTOR  6
#define USB_REQ_SET_CONFIG      9

// USB request type flags
#define USB_RT_D2H              0x80
#define USB_RT_H2D              0x00
#define USB_RT_STD              0x00
#define USB_RT_CLASS            0x20
#define USB_RT_VENDOR           0x40
#define USB_RT_DEV              0x00
#define USB_RT_IFACE            0x01
#define USB_RT_EP               0x02

// USB class codes
#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09
#define USB_CLASS_HID           0x03

// USB device descriptor (18 bytes)
typedef struct __attribute__((packed)) {
    uint8_t length;
    uint8_t type;
    uint16_t usb_ver;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t max_pkt_ep0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_ver;
    uint8_t manufacturer_str;
    uint8_t product_str;
    uint8_t serial_str;
    uint8_t num_configs;
} usb_device_desc_t;

// USB configuration descriptor (9 bytes)
typedef struct __attribute__((packed)) {
    uint8_t length;
    uint8_t type;
    uint16_t total_length;
    uint8_t num_interfaces;
    uint8_t config_value;
    uint8_t config_str;
    uint8_t attributes;
    uint8_t max_power;
} usb_config_desc_t;

// USB interface descriptor (9 bytes)
typedef struct __attribute__((packed)) {
    uint8_t length;
    uint8_t type;
    uint8_t interface_num;
    uint8_t alt_setting;
    uint8_t num_endpoints;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t interface_str;
} usb_interface_desc_t;

// USB endpoint descriptor (7 bytes)
typedef struct __attribute__((packed)) {
    uint8_t length;
    uint8_t type;
    uint8_t address;        // EP number + direction (0x80 = IN)
    uint8_t attributes;     // Transfer type
    uint16_t max_packet;
    uint8_t interval;
} usb_endpoint_desc_t;

// Endpoint direction and type masks
#define USB_EP_DIR_IN           0x80
#define USB_EP_DIR_OUT          0x00
#define USB_EP_NUM_MASK         0x0F
#define USB_EP_TYPE_MASK        0x03
#define USB_EP_TYPE_CONTROL     0
#define USB_EP_TYPE_ISOCH       1
#define USB_EP_TYPE_BULK        2
#define USB_EP_TYPE_INTERRUPT   3

#endif // LIKEOS_USB_H
