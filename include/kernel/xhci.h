// LikeOS-64 - xHCI (USB 3.0) Host Controller Driver
// Interrupt-driven implementation with synchronous transfer support
#ifndef LIKEOS_XHCI_H
#define LIKEOS_XHCI_H

#include "types.h"
#include "status.h"
#include "pci.h"

// xHCI Capability Register offsets
#define XHCI_CAP_CAPLENGTH      0x00
#define XHCI_CAP_HCSPARAMS1     0x04
#define XHCI_CAP_HCSPARAMS2     0x08
#define XHCI_CAP_HCSPARAMS3     0x0C
#define XHCI_CAP_HCCPARAMS1     0x10
#define XHCI_CAP_DBOFF          0x14
#define XHCI_CAP_RTSOFF         0x18

// xHCI Operational Register offsets
#define XHCI_OP_USBCMD          0x00
#define XHCI_OP_USBSTS          0x04
#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_DNCTRL          0x14
#define XHCI_OP_CRCR            0x18
#define XHCI_OP_DCBAAP          0x30
#define XHCI_OP_CONFIG          0x38
#define XHCI_OP_PORTSC_BASE     0x400

// USBCMD bits
#define XHCI_CMD_RUN            (1 << 0)
#define XHCI_CMD_HCRST          (1 << 1)
#define XHCI_CMD_INTE           (1 << 2)
#define XHCI_CMD_HSEE           (1 << 3)

// USBSTS bits
#define XHCI_STS_HCH            (1 << 0)
#define XHCI_STS_HSE            (1 << 2)
#define XHCI_STS_EINT           (1 << 3)
#define XHCI_STS_PCD            (1 << 4)
#define XHCI_STS_CNR            (1 << 11)

// PORTSC bits
#define XHCI_PORTSC_CCS         (1 << 0)
#define XHCI_PORTSC_PED         (1 << 1)
#define XHCI_PORTSC_OCA         (1 << 3)
#define XHCI_PORTSC_PR          (1 << 4)
#define XHCI_PORTSC_PLS_MASK    (0xF << 5)
#define XHCI_PORTSC_PLS_U0      (0 << 5)
#define XHCI_PORTSC_PP          (1 << 9)
#define XHCI_PORTSC_SPEED_MASK  (0xF << 10)
#define XHCI_PORTSC_LWS         (1 << 16)
#define XHCI_PORTSC_CSC         (1 << 17)
#define XHCI_PORTSC_PEC         (1 << 18)
#define XHCI_PORTSC_WRC         (1 << 19)
#define XHCI_PORTSC_OCC         (1 << 20)
#define XHCI_PORTSC_PRC         (1 << 21)
#define XHCI_PORTSC_PLC         (1 << 22)
#define XHCI_PORTSC_CEC         (1 << 23)
#define XHCI_PORTSC_WPR_MASK    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
                                 XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

// Port speeds
#define XHCI_SPEED_FULL         1
#define XHCI_SPEED_LOW          2
#define XHCI_SPEED_HIGH         3
#define XHCI_SPEED_SUPER        4

// TRB types
#define TRB_TYPE_NORMAL         1
#define TRB_TYPE_SETUP          2
#define TRB_TYPE_DATA           3
#define TRB_TYPE_STATUS         4
#define TRB_TYPE_ISOCH          5
#define TRB_TYPE_LINK           6
#define TRB_TYPE_EVENT_DATA     7
#define TRB_TYPE_NOOP           8
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_DISABLE_SLOT   10
#define TRB_TYPE_ADDRESS_DEV    11
#define TRB_TYPE_CONFIG_EP      12
#define TRB_TYPE_EVAL_CTX       13
#define TRB_TYPE_RESET_EP       14
#define TRB_TYPE_STOP_EP        15
#define TRB_TYPE_SET_TR_DEQ     16
#define TRB_TYPE_RESET_DEV      17
#define TRB_TYPE_NOOP_CMD       23
#define TRB_TYPE_TRANSFER       32
#define TRB_TYPE_CMD_COMPLETE   33
#define TRB_TYPE_PORT_STATUS    34
#define TRB_TYPE_HOST_CTRL      37

// TRB completion codes
#define TRB_CC_INVALID          0
#define TRB_CC_SUCCESS          1
#define TRB_CC_DATA_BUFFER      2
#define TRB_CC_BABBLE           3
#define TRB_CC_USB_XACT         4
#define TRB_CC_TRB              5
#define TRB_CC_STALL            6
#define TRB_CC_SHORT_PACKET     13
#define TRB_CC_CMD_RING_STOPPED 24

// TRB flags
#define TRB_FLAG_CYCLE          (1 << 0)
#define TRB_FLAG_TC             (1 << 1)   // Toggle Cycle
#define TRB_FLAG_ISP            (1 << 2)   // Interrupt on Short Packet
#define TRB_FLAG_CH             (1 << 4)   // Chain
#define TRB_FLAG_IOC            (1 << 5)   // Interrupt on Completion
#define TRB_FLAG_IDT            (1 << 6)   // Immediate Data
#define TRB_FLAG_BSR            (1 << 9)   // Block Set Address Request

// Ring sizes (power of 2, last entry is link TRB)
#define XHCI_RING_SIZE          32
#define XHCI_MAX_SLOTS          16
#define XHCI_MAX_ENDPOINTS      32
#define XHCI_MAX_PORTS          8

// Context sizes
#define XHCI_SLOT_CTX_SIZE      32
#define XHCI_EP_CTX_SIZE        32
#define XHCI_INPUT_CTX_SIZE     (33 * 32)  // Input control + slot + 31 endpoints
#define XHCI_DEV_CTX_SIZE       (32 * 32)  // Slot + 31 endpoints

// Endpoint types
#define EP_TYPE_ISOCH_OUT       1
#define EP_TYPE_BULK_OUT        2
#define EP_TYPE_INTERRUPT_OUT   3
#define EP_TYPE_CONTROL         4
#define EP_TYPE_ISOCH_IN        5
#define EP_TYPE_BULK_IN         6
#define EP_TYPE_INTERRUPT_IN    7

// TRB structure (16 bytes, must be aligned to 16)
typedef struct __attribute__((packed, aligned(16))) {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

// Transfer ring with link TRB
typedef struct __attribute__((aligned(64))) {
    xhci_trb_t trbs[XHCI_RING_SIZE];
    uint32_t enqueue;
    uint32_t dequeue;
    uint8_t cycle;
    uint8_t pad[3];
} xhci_ring_t;

// Event ring segment table entry
typedef struct __attribute__((packed, aligned(64))) {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} xhci_erst_entry_t;

// Slot context
typedef struct __attribute__((packed)) {
    uint32_t route_speed_entries;   // Route string[19:0], Speed[23:20], Entries[31:27]
    uint32_t latency_hub_ports;     // Max exit latency, RH port num, Num ports
    uint32_t tt_info;               // TT info for FS/LS devices
    uint32_t slot_state;            // Slot state, device address
    uint32_t reserved[4];
} xhci_slot_ctx_t;

// Endpoint context
typedef struct __attribute__((packed)) {
    uint32_t ep_info1;              // EP state, mult, max streams, interval, LSA
    uint32_t ep_info2;              // Error count, EP type, HID, Max burst, Max packet size
    uint64_t tr_dequeue;            // TR dequeue pointer
    uint32_t avg_trb_len;           // Average TRB length
    uint32_t reserved[3];
} xhci_ep_ctx_t;

// Device context
typedef struct __attribute__((aligned(64))) {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t endpoints[31];
} xhci_dev_ctx_t;

// Input context
typedef struct __attribute__((aligned(64))) {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t endpoints[31];
} xhci_input_ctx_t;

// USB device structure
typedef struct usb_device {
    uint8_t slot_id;
    uint8_t port;
    uint8_t speed;
    uint8_t address;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t num_configs;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t max_packet_ep0;
    
    // Endpoint info
    uint8_t bulk_in_ep;        // Endpoint number (1-15)
    uint8_t bulk_out_ep;       // Endpoint number (1-15)
    uint16_t bulk_in_max_pkt;
    uint16_t bulk_out_max_pkt;
    
    // MSD specific
    uint8_t lun_count;
    uint8_t configured;
    
    // Transfer rings for bulk endpoints
    xhci_ring_t* bulk_in_ring;
    xhci_ring_t* bulk_out_ring;
    
    // Controller back-reference
    void* controller;
} usb_device_t;

// Transfer completion structure for interrupt-driven operation
typedef struct {
    volatile uint8_t completed;
    volatile uint8_t cc;           // Completion code
    volatile uint32_t bytes_transferred;
} xhci_transfer_t;

// xHCI controller structure
typedef struct xhci_controller {
    // Base addresses
    uint64_t base;                  // MMIO base (capability registers)
    uint64_t op_base;               // Operational registers
    uint64_t db_base;               // Doorbell registers
    uint64_t rt_base;               // Runtime registers
    
    // Controller info
    uint8_t max_slots;
    uint8_t max_ports;
    uint8_t max_intrs;
    uint8_t context_size;           // 32 or 64 bytes
    
    // DCBAA (Device Context Base Address Array)
    uint64_t* dcbaa;
    uint64_t dcbaa_phys;
    
    // Command ring
    xhci_ring_t* cmd_ring;
    uint64_t cmd_ring_phys;
    
    // Event ring
    xhci_ring_t* event_ring;
    uint64_t event_ring_phys;
    xhci_erst_entry_t* erst;
    uint64_t erst_phys;
    
    // Device contexts
    xhci_dev_ctx_t* dev_ctx[XHCI_MAX_SLOTS];
    xhci_input_ctx_t* input_ctx;
    uint64_t input_ctx_phys;
    
    // Devices
    usb_device_t devices[XHCI_MAX_SLOTS];
    uint8_t num_devices;
    
    // Pending transfers (for interrupt handling)
    xhci_transfer_t* pending_xfer[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS];
    
    // IRQ info
    uint8_t irq;
    uint8_t irq_enabled;
    
    // State
    uint8_t running;
    uint8_t initialized;
    
    // Scratchpad
    uint64_t* scratchpad_array;
    void** scratchpad_pages;
    uint16_t num_scratchpads;
} xhci_controller_t;

// Global controller instance
extern xhci_controller_t g_xhci;

// Core functions
int xhci_init(xhci_controller_t* ctrl, const pci_device_t* dev);
void xhci_shutdown(xhci_controller_t* ctrl);
int xhci_reset(xhci_controller_t* ctrl);
int xhci_start(xhci_controller_t* ctrl);
void xhci_stop(xhci_controller_t* ctrl);

// Interrupt handling
void xhci_irq_service(xhci_controller_t* ctrl);
void xhci_process_events(xhci_controller_t* ctrl);

// Port management
int xhci_poll_ports(xhci_controller_t* ctrl);
int xhci_port_reset(xhci_controller_t* ctrl, uint8_t port);
uint8_t xhci_port_speed(xhci_controller_t* ctrl, uint8_t port);

// Device management
int xhci_enable_slot(xhci_controller_t* ctrl);
int xhci_address_device(xhci_controller_t* ctrl, uint8_t slot, uint8_t port, uint8_t speed);
int xhci_configure_endpoint(xhci_controller_t* ctrl, uint8_t slot, uint8_t ep_num,
                            uint8_t ep_type, uint16_t max_packet, uint8_t interval);
int xhci_enumerate_device(xhci_controller_t* ctrl, uint8_t port);

// Control transfers
int xhci_control_transfer(xhci_controller_t* ctrl, usb_device_t* dev,
                         uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                         void* data);

// Bulk transfers
int xhci_bulk_transfer_in(xhci_controller_t* ctrl, usb_device_t* dev,
                          void* buf, uint32_t len, uint32_t* transferred);
int xhci_bulk_transfer_out(xhci_controller_t* ctrl, usb_device_t* dev,
                           const void* buf, uint32_t len, uint32_t* transferred);

// Ring management
xhci_ring_t* xhci_alloc_ring(void);
void xhci_free_ring(xhci_ring_t* ring);
void xhci_ring_init(xhci_ring_t* ring, uint64_t phys);
int xhci_ring_enqueue(xhci_ring_t* ring, uint64_t param, uint32_t status, uint32_t control);
void xhci_ring_doorbell(xhci_controller_t* ctrl, uint8_t slot, uint8_t ep);

// Command ring operations
int xhci_send_command(xhci_controller_t* ctrl, uint64_t param, uint32_t status, uint32_t control);
int xhci_wait_command(xhci_controller_t* ctrl, uint32_t timeout_ms);

// Memory helpers
static inline uint32_t xhci_cap_read32(xhci_controller_t* ctrl, uint32_t off) {
    return *(volatile uint32_t*)(ctrl->base + off);
}

static inline uint32_t xhci_op_read32(xhci_controller_t* ctrl, uint32_t off) {
    return *(volatile uint32_t*)(ctrl->op_base + off);
}

static inline void xhci_op_write32(xhci_controller_t* ctrl, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(ctrl->op_base + off) = val;
}

static inline uint64_t xhci_op_read64(xhci_controller_t* ctrl, uint32_t off) {
    return *(volatile uint64_t*)(ctrl->op_base + off);
}

static inline void xhci_op_write64(xhci_controller_t* ctrl, uint32_t off, uint64_t val) {
    *(volatile uint64_t*)(ctrl->op_base + off) = val;
}

static inline uint32_t xhci_rt_read32(xhci_controller_t* ctrl, uint32_t off) {
    return *(volatile uint32_t*)(ctrl->rt_base + off);
}

static inline void xhci_rt_write32(xhci_controller_t* ctrl, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(ctrl->rt_base + off) = val;
}

static inline void xhci_rt_write64(xhci_controller_t* ctrl, uint32_t off, uint64_t val) {
    *(volatile uint64_t*)(ctrl->rt_base + off) = val;
}

static inline void xhci_db_write32(xhci_controller_t* ctrl, uint8_t slot, uint32_t val) {
    *(volatile uint32_t*)(ctrl->db_base + slot * 4) = val;
}

#endif // LIKEOS_XHCI_H
