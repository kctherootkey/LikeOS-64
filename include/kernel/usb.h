// LikeOS-64 - Minimal USB core scaffolding (pre-control transfer)
#ifndef LIKEOS_USB_H
#define LIKEOS_USB_H

#include "types.h"
#include "status.h"
#include "xhci.h"

typedef enum {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,
    USB_SPEED_FULL,
    USB_SPEED_HIGH,
    USB_SPEED_SUPER,
} usb_speed_t;

typedef struct usb_device {
    uint8_t  address;      uint8_t  port_number;  usb_speed_t speed; uint8_t slot_id; uint16_t vid; uint16_t pid;
    uint8_t  class_code;   uint8_t  subclass;     uint8_t  protocol; uint8_t configured; void* input_ctx; void* device_ctx;
    uint8_t  dev_desc8[8]; uint8_t  dev_desc18[18]; uint8_t have_desc8; uint8_t have_desc18; uint8_t* config_desc;
    uint16_t config_desc_len; uint8_t have_config9; uint8_t have_config_full;
    uint8_t  bulk_in_ep; uint8_t bulk_out_ep; uint16_t bulk_in_mps; uint16_t bulk_out_mps;
    uint8_t  endpoints_configured;
} usb_device_t;

// USB standard descriptor types
#define USB_DESC_DEVICE        1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_INTERFACE     4

// Standard requests
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQTYPE_HOST_TO_DEVICE 0x00

// Mass Storage class code
#define USB_CLASS_MASS_STORAGE 0x08

// (single definition of usb_device_t above)

#define USB_MAX_DEVICES 16

typedef struct {
    usb_device_t devices[USB_MAX_DEVICES];
    int count;
} usb_device_table_t;

void usb_core_init(void);
usb_device_t* usb_alloc_device(uint8_t port);

#endif // LIKEOS_USB_H
