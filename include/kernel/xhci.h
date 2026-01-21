// LikeOS-64 - XHCI skeleton (polling only)
#ifndef LIKEOS_XHCI_H
#define LIKEOS_XHCI_H

#include "status.h"
#include "pci.h"
#include "console.h" /* for kprintf used by debug macros */

// Minimal subset of XHCI register layout (polling, single controller)
// NOTE: This intentionally omits full spec accuracy; offsets suffice for early bring-up.

#define XHCI_USBCMD_RS      (1u << 0)
#define XHCI_USBCMD_HCRST   (1u << 1)
#define XHCI_USBCMD_INTE    (1u << 2)

#define XHCI_USBSTS_HCH     (1u << 0)

#define XHCI_PORTSC_CCS     (1u << 0)
#define XHCI_PORTSC_PED     (1u << 1)
#define XHCI_PORTSC_PR      (1u << 4)

// Operational register relative offsets
#define XHCI_OP_USBCMD      0x00
#define XHCI_OP_USBSTS      0x04
#define XHCI_OP_PAGESIZE    0x08
#define XHCI_OP_DNCTRL      0x10
// Operational register offsets relative to op_base (spec Rev 1.1):
// 0x00 USBCMD, 0x04 USBSTS, 0x08 PAGESIZE, 0x0C Reserved, 0x10 DNCTRL,
// 0x14 CRCR (64-bit), 0x1C Reserved, 0x20 DCBAAP (64-bit), 0x28 CONFIG
// (Earlier temporary edit to 0x18/0x30/0x38 was incorrect.)
#define XHCI_OP_CRCR        0x18 // 64-bit (low dword at 0x18, high at 0x1C)
#define XHCI_OP_DCBAAP      0x30 // 64-bit (low 0x30, high 0x34)
#define XHCI_OP_CONFIG      0x38

// Simplified assumption: port registers start at 0x400 from operational base
#define XHCI_PORT_REG_BASE  0x400
#define XHCI_PORT_REG_STRIDE 0x10

// Runtime / interrupter registers (interrupter 0) relative to runtime base (simplified)
#define XHCI_RT_IR0_IMAN    0x20
#define XHCI_RT_IR0_IMOD    0x24
#define XHCI_RT_IR0_ERSTSZ  0x28
#define XHCI_RT_IR0_ERSTBA  0x30 // 64-bit
#define XHCI_RT_IR0_ERDP    0x38 // 64-bit

typedef struct {
    const pci_device_t* pci;
    unsigned long mmio_base;          // Physical base from BAR
    unsigned long capability_length;  // CAPLENGTH
    unsigned long op_base;            // Operational registers base (mmio_base + caplen)
    unsigned int hcsparams1;          // Cached capability params
    unsigned int hcsparams2;          // Scratchpad etc
    unsigned int hcsparams3;          // Optional params 3
    unsigned int hccparams1;          // Capability params 1
    unsigned int dboff;               // Doorbell offset (from base)
    unsigned int rtsoff;              // Runtime register space offset (from base)
    unsigned int max_ports;           // Derived from HCSPARAMS1
    unsigned int max_slots;           // Derived from HCSPARAMS1 (bit 0-7)
    // Minimal command ring
    void* cmd_ring_virt;              // virtual address of ring segment
    unsigned long cmd_ring_phys;      // physical (assuming identity for now)
    unsigned int cmd_ring_size;       // number of TRBs
    unsigned int cmd_ring_enqueue;    // index
    unsigned int cmd_cycle_state;     // current cycle (0/1)
    // Device context base array (DCBAA) placeholder
    void* dcbaa_virt;
    unsigned long dcbaa_phys;
    // Event ring (single segment)
    void* event_ring_virt;
    unsigned long event_ring_phys;
    unsigned int event_ring_size;
    unsigned int event_ring_dequeue;
    unsigned int event_ring_cycle;
    void* erst_virt;                 // Single ERST entry (16 bytes)
    unsigned long erst_phys;
    unsigned long doorbell_array;    // virtual address of doorbell array base
    unsigned long runtime_base;      // runtime register base (mmio_base + rtsoff)
    // Pending command tracking (single outstanding simple command)
    void* pending_device;            // usb_device_t* awaiting completion
    unsigned int pending_cmd_type;   // TRB type
    void* pending_cmd_trb;           // address of command TRB enqueued
    unsigned long pending_cmd_trb_phys; // physical address of that TRB for event correlation
    unsigned long last_noop_trb_phys;    // physical address of initial NO-OP (to ignore its completion)
    unsigned int cmd_ring_stall_ticks; // polls since last progress for debug
        unsigned long device_address;    // Device address state
        void* ep0_ring;                  // EP0 ring
        unsigned int ep0_ring_index;
        unsigned int ep0_ring_cycle;
        unsigned long ep0_ring_phys;
        unsigned int ep0_stage_start; // index of first TRB of current control transfer
        // Pending control transfer tracking
        void* pending_xfer_buf;
        unsigned int pending_xfer_len;
        unsigned int pending_xfer_stage; // 0=none,1=desc8,2=desc18
    // Extended stages (planned): 3=config9,4=config_full,5=set_config
    unsigned int enable_slot_retries;    // retry counter for ENABLE_SLOT when slot id returns 0
    // Slot to usb_device pointer map (simple, sized to max_slots at init time via separate alloc maybe later)
    void* slot_device_map[64];
    // Bulk endpoint rings (single segment each)
    void* bulk_in_ring;
    void* bulk_out_ring;
    unsigned long bulk_in_ring_phys;
    unsigned long bulk_out_ring_phys;
    unsigned int bulk_in_cycle;
    unsigned int bulk_out_cycle;
    unsigned int bulk_in_enqueue;
    unsigned int bulk_out_enqueue;
    // Mass storage BOT simple state machine
    unsigned int msd_state; // 0 idle,1 cbw out,2 data in,3 csw in,4 done
    void* msd_cbw_buf;
    void* msd_data_buf;
    void* msd_csw_buf;
    // TRB pointers for BOT stage correlation
    void* msd_cbw_trb;
    void* msd_data_trb;
    void* msd_csw_trb;
    unsigned int msd_expected_data_len;
    unsigned int msd_op; // 0 none,1 inquiry,2 read_capacity,3 read
    unsigned int msd_ready; // set when capacity known
    unsigned long msd_capacity_blocks;
    unsigned int msd_block_size;
    // Pending read operation fields
    void* msd_read_buf;
    unsigned int msd_read_buf_len;
    unsigned long msd_read_lba;
    unsigned int msd_read_blocks;
    int msd_read_result; // 0 pending, >0 success bytes, <0 error
    // Pending write operation fields
    void* msd_write_buf;
    unsigned int msd_write_buf_len;
    unsigned long msd_write_lba;
    unsigned int msd_write_blocks;
    int msd_write_result; // 0 pending, >0 success bytes, <0 error
    // Recovery / error handling
    unsigned int msd_retry_count;
    unsigned int msd_reset_count;
    unsigned int msd_last_sense_key;
    unsigned int msd_last_sense_asc;
    unsigned int msd_last_sense_ascq;
    // CBW/CSW tag tracking for validation
    unsigned int msd_tag_counter;
    unsigned int msd_expected_tag;
    // Timing / timeout & backoff (simple poll counters)
    unsigned long msd_poll_counter;      // increments each poll
    unsigned long msd_op_start_tick;     // poll counter at op start
    unsigned long msd_timeout_ticks;     // threshold before declaring timeout
    unsigned long msd_backoff_until;     // poll counter until which we delay re-issuing TUR
    // Deferred BOT queuing (queue only CBW first; DATA+CSW after CBW completion)
    void* msd_pending_data_buf;          // buffer to use for data phase (if any)
    unsigned int msd_pending_data_len;   // length of data phase
    int msd_need_csw;                    // set if a CSW IN transfer must be queued after DATA (or immediately if no DATA)
    // Instrumentation (lightweight): event counters & last event metadata
    unsigned int msd_cbw_events;
    unsigned int msd_data_events;
    unsigned int msd_csw_events;
    unsigned int msd_last_event_cc;
    unsigned int msd_last_event_epid;
    unsigned long msd_cbw_phys;
    unsigned long msd_data_phys;
    unsigned long msd_csw_phys;
    unsigned long msd_last_event_ptr;
    // Extended instrumentation: total transfer events observed and bulk-specific
    unsigned int msd_transfer_events;      // all transfer events processed
    unsigned int msd_bulk_transfer_events; // transfer events with epid>1 (bulk/interrupt)
} xhci_controller_t;

/* Debug macro access (allow other modules like usb_msd.c to reuse logging gate) */
#ifndef XHCI_MSD_DEBUG
#define XHCI_MSD_DEBUG 0
#endif
#ifndef XHCI_MSD_LOG
#define XHCI_MSD_LOG(fmt, ...) do { if (XHCI_MSD_DEBUG) kprintf(fmt, ##__VA_ARGS__); } while(0)
#endif

int xhci_init(xhci_controller_t* ctrl, const pci_device_t* dev);
int xhci_poll_ports(xhci_controller_t* ctrl); // log simple connect status
int xhci_process_events(xhci_controller_t* ctrl); // poll event ring
// Interrupt service routine hook (called from generic IRQ dispatch)
void xhci_irq_service(xhci_controller_t* ctrl);
struct usb_device; // forward declare

// Simple control transfer state machine (device enumeration)
typedef enum {
    XHCI_CTRL_IDLE = 0,
    XHCI_CTRL_GET_DEV_DESC8_SETUP,
    XHCI_CTRL_GET_DEV_DESC8_DATA,
    XHCI_CTRL_GET_DEV_DESC8_STATUS,
    XHCI_CTRL_GET_DEV_DESC18_SETUP,
    XHCI_CTRL_GET_DEV_DESC18_DATA,
    XHCI_CTRL_GET_DEV_DESC18_STATUS,
    XHCI_CTRL_SET_ADDRESS_PENDING,
    XHCI_CTRL_COMPLETE
} xhci_ctrl_state_t;

// Kick enumeration sequence after ADDRESS_DEVICE completion
void xhci_start_enumeration(xhci_controller_t* ctrl, struct usb_device* dev);
// Advance control transfer state machine on transfer events
void xhci_control_xfer_event(xhci_controller_t* ctrl, struct usb_device* dev, unsigned cc);
int xhci_configure_mass_storage_endpoints(xhci_controller_t* ctrl, struct usb_device* dev);
int xhci_enqueue_bulk_out(xhci_controller_t* ctrl, struct usb_device* dev, void* buf, unsigned len);
int xhci_enqueue_bulk_in(xhci_controller_t* ctrl, struct usb_device* dev, void* buf, unsigned len);
void xhci_issue_reset_endpoint(xhci_controller_t* ctrl, struct usb_device* dev, unsigned epid);
void xhci_issue_set_tr_dequeue(xhci_controller_t* ctrl, struct usb_device* dev, unsigned epid, uint64_t ring_phys, unsigned cycle);
void xhci_debug_state(xhci_controller_t* ctrl);

#endif // LIKEOS_XHCI_H
