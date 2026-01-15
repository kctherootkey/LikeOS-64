// LikeOS-64 - Minimal XHCI TRB definitions (skeleton)
#ifndef LIKEOS_XHCI_TRB_H
#define LIKEOS_XHCI_TRB_H

#include "types.h"

// Generic 16-byte TRB
typedef struct __attribute__((packed)) {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control; // type + flags
} xhci_trb_t;

// TRB Type field (bits 10:15 of control word)
#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_TYPE_MASK  (0x3F << XHCI_TRB_TYPE_SHIFT)

// Selected TRB types (command ring)
#define XHCI_TRB_TYPE_ENABLE_SLOT    9
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11
#define XHCI_TRB_TYPE_CONFIG_ENDPOINT 12
#define XHCI_TRB_TYPE_RESET_ENDPOINT 14
#define XHCI_TRB_TYPE_EVAL_CONTEXT    13
#define XHCI_TRB_TYPE_LINK            6
#define XHCI_TRB_TYPE_NO_OP_CMD       23
#define XHCI_TRB_TYPE_SET_TR_DEQUEUE_POINTER 16

// Transfer TRB types (subset)
#define XHCI_TRB_TYPE_NORMAL          1
#define XHCI_TRB_TYPE_SETUP_STAGE     2
#define XHCI_TRB_TYPE_DATA_STAGE      3
#define XHCI_TRB_TYPE_STATUS_STAGE    4
// Event (transfer) type for data completions
#define XHCI_TRB_TYPE_TRANSFER_EVENT  32
// Event TRB types (subset)
#define XHCI_TRB_TYPE_COMMAND_COMPLETION 33
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE 34

// Control word helpers
#define XHCI_TRB_SET_TYPE(t)   ((t) << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_CYCLE         (1u << 0)
#define XHCI_TRB_IOC           (1u << 5)
#define XHCI_TRB_IDT           (1u << 6)

// Setup Stage TRB Transfer Type (TRT) bits (17:16)
#define XHCI_SETUP_TRT_NO_DATA   (0u << 16)
#define XHCI_SETUP_TRT_OUT_DATA  (2u << 16)
#define XHCI_SETUP_TRT_IN_DATA   (3u << 16)

// Command Completion Event layout helpers
// param_lo: pointer to command TRB that completed (low 32 bits)
// status: completion code bits 23:16, slot ID bits 7:0 for some events (including Enable Slot)
// For Command Completion Event TRB:
// status DW: bits 31:24 = Completion Code, bits 23:16 = Slot ID (when applicable)
#define XHCI_CCE_COMPLETION_CODE(status) (((status) >> 24) & 0xFF)
#define XHCI_CCE_SLOT_ID(status)        (((status) >> 16) & 0xFF)

// Setup Stage TRB request type fields
#define XHCI_SETUP_DIR_IN   (1u<<7)
// bmRequestType for standard GET_DESCRIPTOR(Device)
#define USB_REQTYPE_DEVICE_TO_HOST (0x80)
#define USB_REQ_GET_DESCRIPTOR     0x06


#endif // LIKEOS_XHCI_TRB_H
