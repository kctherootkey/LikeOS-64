// LikeOS-64 - xHCI (USB 3.0) Host Controller Driver
// Interrupt-driven implementation with synchronous transfer API
// 
// Design principles:
// 1. Pre-compute all physical addresses BEFORE enqueueing TRBs
// 2. Properly handle ring wraparound with link TRBs
// 3. Use interrupt-driven completion with synchronous wait API
// 4. Clear, simple state machine for transfers

#include "../../include/kernel/xhci.h"
#include "../../include/kernel/usb.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/ioapic.h"

// Debug output control
#define XHCI_DEBUG 0
#if XHCI_DEBUG
    #define xhci_dbg(fmt, ...) kprintf("[XHCI] " fmt, ##__VA_ARGS__)
#else
    #define xhci_dbg(fmt, ...) ((void)0)
#endif

// Global controller instance
xhci_controller_t g_xhci;

// Internal helper prototypes
static void xhci_setup_scratchpad(xhci_controller_t* ctrl);
static int xhci_wait_ready(xhci_controller_t* ctrl, uint32_t timeout_ms);
static void xhci_setup_event_ring(xhci_controller_t* ctrl);
static void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* trb);
static void xhci_handle_command_event(xhci_controller_t* ctrl, xhci_trb_t* trb);
static void xhci_handle_port_event(xhci_controller_t* ctrl, xhci_trb_t* trb);
static void delay_us(uint32_t us);
static void delay_ms(uint32_t ms);

// Memory barrier
static inline void xhci_mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

// Simple delay (calibrated approximately)
static void delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us * 100; i++) {
        __asm__ volatile ("pause");
    }
}

static void delay_ms(uint32_t ms) {
    delay_us(ms * 1000);
}

// Zero memory
static void xhci_memset(void* dst, int val, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void xhci_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

//=============================================================================
// Ring Management
//=============================================================================

xhci_ring_t* xhci_alloc_ring(void) {
    // For xHCI rings, we need 64-byte alignment minimum, but page alignment is safer for DMA
    // kalloc doesn't guarantee alignment, so allocate extra and align manually
    size_t ring_size = sizeof(xhci_ring_t);
    size_t alloc_size = ring_size + 4096;  // Extra page for alignment
    
    uint8_t* raw = (uint8_t*)kcalloc(1, alloc_size);
    if (!raw) return NULL;
    
    // Align to 4KB page boundary
    uint64_t raw_addr = (uint64_t)raw;
    uint64_t aligned_addr = (raw_addr + 4095) & ~4095ULL;
    xhci_ring_t* ring = (xhci_ring_t*)aligned_addr;
    
    xhci_memset(ring, 0, ring_size);
    return ring;
}

void xhci_free_ring(xhci_ring_t* ring) {
    // Note: We can't easily free the original raw pointer, but for kernel this is OK
    (void)ring;
}

void xhci_ring_init(xhci_ring_t* ring, uint64_t phys) {
    xhci_memset(ring->trbs, 0, sizeof(ring->trbs));
    ring->enqueue = 0;
    ring->dequeue = 0;
    ring->cycle = 1;
    
    // Setup link TRB at the last entry to loop back
    uint32_t link_idx = XHCI_RING_SIZE - 1;
    ring->trbs[link_idx].param = phys;  // Point back to start
    ring->trbs[link_idx].status = 0;
    ring->trbs[link_idx].control = (TRB_TYPE_LINK << 10) | TRB_FLAG_TC | TRB_FLAG_CYCLE;
}

// Enqueue a TRB to the ring, handling link TRB wraparound
// Returns index of enqueued TRB, or -1 on error
int xhci_ring_enqueue(xhci_ring_t* ring, uint64_t param, uint32_t status, uint32_t control) {
    static int wrap_count = 0;
    
    // Check if we're at the link TRB
    if (ring->enqueue >= XHCI_RING_SIZE - 1) {
        // We need to process the Link TRB. The Link TRB has TC (Toggle Cycle) set,
        // so the controller will toggle its cycle state when it follows the link.
        // We need to:
        // 1. Make sure the Link TRB has the current cycle bit (so controller processes it)
        // 2. Toggle OUR cycle to match what controller will use after the link
        // 3. Reset enqueue to 0
        
        // Set Link TRB cycle to current cycle (make it valid)
        uint32_t link_ctrl = ring->trbs[XHCI_RING_SIZE - 1].control;
        link_ctrl = (link_ctrl & ~TRB_FLAG_CYCLE) | (ring->cycle ? TRB_FLAG_CYCLE : 0);
        ring->trbs[XHCI_RING_SIZE - 1].control = link_ctrl;
        xhci_mb();
        
        // Toggle our cycle to match post-link state
        ring->cycle ^= 1;
        ring->enqueue = 0;
    }
    
    uint32_t idx = ring->enqueue;
    
    // Set up TRB with current cycle bit
    ring->trbs[idx].param = param;
    ring->trbs[idx].status = status;
    ring->trbs[idx].control = (control & ~TRB_FLAG_CYCLE) | (ring->cycle ? TRB_FLAG_CYCLE : 0);
    
    xhci_mb();
    
    ring->enqueue++;
    
    return (int)idx;
}

// Ring doorbell for a slot/endpoint
void xhci_ring_doorbell(xhci_controller_t* ctrl, uint8_t slot, uint8_t ep) {
    xhci_mb();
    xhci_db_write32(ctrl, slot, ep);
    xhci_mb();
}

//=============================================================================
// Controller Initialization
//=============================================================================

int xhci_init(xhci_controller_t* ctrl, const pci_device_t* dev) {
    xhci_memset(ctrl, 0, sizeof(*ctrl));
    
    // Get BAR0 (MMIO base)
    ctrl->base = dev->bar[0] & ~0xFULL;
    if (ctrl->base == 0) {
        kprintf("[XHCI] ERROR: BAR0 not configured\n");
        return ST_ERR;
    }
    
    // Enable bus mastering and memory access
    pci_enable_busmaster_mem(dev);
    
    // Read capability registers
    uint8_t cap_len = xhci_cap_read32(ctrl, XHCI_CAP_CAPLENGTH) & 0xFF;
    uint32_t hcsparams1 = xhci_cap_read32(ctrl, XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2 = xhci_cap_read32(ctrl, XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1 = xhci_cap_read32(ctrl, XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = xhci_cap_read32(ctrl, XHCI_CAP_DBOFF) & ~0x3;
    uint32_t rtsoff = xhci_cap_read32(ctrl, XHCI_CAP_RTSOFF) & ~0x1F;
    
    ctrl->max_slots = hcsparams1 & 0xFF;
    ctrl->max_ports = (hcsparams1 >> 24) & 0xFF;
    ctrl->max_intrs = (hcsparams1 >> 8) & 0x7FF;
    ctrl->context_size = (hccparams1 & (1 << 2)) ? 64 : 32;
    ctrl->num_scratchpads = ((hcsparams2 >> 21) & 0x1F) | (((hcsparams2 >> 27) & 0x1F) << 5);
    
    // Calculate register base addresses
    ctrl->op_base = ctrl->base + cap_len;
    ctrl->db_base = ctrl->base + dboff;
    ctrl->rt_base = ctrl->base + rtsoff;
    
    xhci_dbg("Base=0x%llx, OpBase=0x%llx, DBBase=0x%llx, RTBase=0x%llx\n",
             ctrl->base, ctrl->op_base, ctrl->db_base, ctrl->rt_base);
    xhci_dbg("MaxSlots=%d, MaxPorts=%d, CtxSize=%d, Scratchpads=%d\n",
             ctrl->max_slots, ctrl->max_ports, ctrl->context_size, ctrl->num_scratchpads);
    
    // Limit slots to our maximum
    if (ctrl->max_slots > XHCI_MAX_SLOTS) ctrl->max_slots = XHCI_MAX_SLOTS;
    if (ctrl->max_ports > XHCI_MAX_PORTS) ctrl->max_ports = XHCI_MAX_PORTS;
    
    // Reset controller
    if (xhci_reset(ctrl) != ST_OK) {
        kprintf("[XHCI] Reset failed\n");
        return ST_ERR;
    }
    
    // Allocate DCBAA (Device Context Base Address Array)
    ctrl->dcbaa = (uint64_t*)kcalloc(1, (ctrl->max_slots + 1) * sizeof(uint64_t) + 64);
    if (!ctrl->dcbaa) {
        kprintf("[XHCI] Failed to allocate DCBAA\n");
        return ST_NOMEM;
    }
    // Align to 64 bytes
    uint64_t dcbaa_addr = (uint64_t)ctrl->dcbaa;
    if (dcbaa_addr & 0x3F) {
        dcbaa_addr = (dcbaa_addr + 63) & ~63ULL;
        ctrl->dcbaa = (uint64_t*)dcbaa_addr;
    }
    ctrl->dcbaa_phys = mm_get_physical_address((uint64_t)ctrl->dcbaa);
    
    // Setup scratchpad if needed
    if (ctrl->num_scratchpads > 0) {
        xhci_setup_scratchpad(ctrl);
    }
    
    // Allocate command ring
    // Allocate command ring
    ctrl->cmd_ring = xhci_alloc_ring();
    if (!ctrl->cmd_ring) {
        kprintf("[XHCI] Failed to allocate command ring\n");
        return ST_NOMEM;
    }
    ctrl->cmd_ring_phys = mm_get_physical_address((uint64_t)ctrl->cmd_ring->trbs);
    xhci_ring_init(ctrl->cmd_ring, ctrl->cmd_ring_phys);
    
    // Allocate input context
    ctrl->input_ctx = (xhci_input_ctx_t*)kcalloc(1, sizeof(xhci_input_ctx_t) + 64);
    if (!ctrl->input_ctx) {
        kprintf("[XHCI] Failed to allocate input context\n");
        return ST_NOMEM;
    }
    uint64_t ictx_addr = (uint64_t)ctrl->input_ctx;
    if (ictx_addr & 0x3F) {
        ictx_addr = (ictx_addr + 63) & ~63ULL;
        ctrl->input_ctx = (xhci_input_ctx_t*)ictx_addr;
    }
    ctrl->input_ctx_phys = mm_get_physical_address((uint64_t)ctrl->input_ctx);
    
    // Setup event ring
    xhci_setup_event_ring(ctrl);
    
    // Configure operational registers
    xhci_op_write32(ctrl, XHCI_OP_CONFIG, ctrl->max_slots);
    xhci_op_write64(ctrl, XHCI_OP_DCBAAP, ctrl->dcbaa_phys);
    xhci_op_write64(ctrl, XHCI_OP_CRCR, ctrl->cmd_ring_phys | TRB_FLAG_CYCLE);
    
    // Start controller
    if (xhci_start(ctrl) != ST_OK) {
        kprintf("[XHCI] Failed to start controller\n");
        return ST_ERR;
    }
    
    // Enable interrupts
#if XHCI_USE_INTERRUPTS
    ctrl->irq = dev->interrupt_line;
    if (ctrl->irq != 0xFF && ctrl->irq < 16) {
        // Configure interrupter 0
        uint64_t erdp = mm_get_physical_address((uint64_t)ctrl->event_ring->trbs);
        xhci_rt_write64(ctrl, 0x38, erdp | (1 << 3)); // ERDP with EHB
        
        // Set IMOD (interrupt moderation) to 0 for immediate interrupts
        xhci_rt_write32(ctrl, 0x24, 0);
        
        // Enable interrupter
        xhci_rt_write32(ctrl, 0x20, (1 << 1) | (1 << 0)); // IE | IP
        
        // Enable interrupts in USBCMD
        uint32_t cmd = xhci_op_read32(ctrl, XHCI_OP_USBCMD);
        xhci_op_write32(ctrl, XHCI_OP_USBCMD, cmd | XHCI_CMD_INTE);
        
        irq_enable(ctrl->irq);
        ctrl->irq_enabled = 1;
    }
#endif
    
    ctrl->initialized = 1;
    
    return ST_OK;
}

static void xhci_setup_scratchpad(xhci_controller_t* ctrl) {
    if (ctrl->num_scratchpads == 0) return;
    
    // Allocate scratchpad array
    ctrl->scratchpad_array = (uint64_t*)kcalloc(ctrl->num_scratchpads, sizeof(uint64_t));
    ctrl->scratchpad_pages = (void**)kcalloc(ctrl->num_scratchpads, sizeof(void*));
    
    if (!ctrl->scratchpad_array || !ctrl->scratchpad_pages) {
        kprintf("[XHCI] Failed to allocate scratchpad\n");
        return;
    }
    
    // Allocate pages for scratchpad (must be page-aligned for DMA)
    for (uint16_t i = 0; i < ctrl->num_scratchpads; i++) {
        // Allocate extra for alignment since kcalloc doesn't guarantee page alignment
        void* raw = kcalloc(1, PAGE_SIZE + PAGE_SIZE);
        if (!raw) {
            kprintf("[XHCI] Failed to allocate scratchpad page %d\n", i);
            return;
        }
        // Align to page boundary
        void* page = (void*)(((uint64_t)raw + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        ctrl->scratchpad_pages[i] = raw;  // Store raw for freeing later
        ctrl->scratchpad_array[i] = mm_get_physical_address((uint64_t)page);
    }
    
    // Point DCBAA[0] to scratchpad array
    ctrl->dcbaa[0] = mm_get_physical_address((uint64_t)ctrl->scratchpad_array);
    xhci_dbg("Scratchpad: %d pages configured\n", ctrl->num_scratchpads);
}

static void xhci_setup_event_ring(xhci_controller_t* ctrl) {
    // Allocate event ring
    ctrl->event_ring = xhci_alloc_ring();
    if (!ctrl->event_ring) {
        kprintf("[XHCI] Failed to allocate event ring\n");
        return;
    }
    ctrl->event_ring_phys = mm_get_physical_address((uint64_t)ctrl->event_ring->trbs);
    
    // Initialize event ring (no link TRB, just zero it)
    xhci_memset(ctrl->event_ring->trbs, 0, sizeof(ctrl->event_ring->trbs));
    ctrl->event_ring->enqueue = 0;
    ctrl->event_ring->dequeue = 0;
    ctrl->event_ring->cycle = 1;
    
    // Allocate ERST (Event Ring Segment Table)
    ctrl->erst = (xhci_erst_entry_t*)kcalloc(1, sizeof(xhci_erst_entry_t) * 2 + 64);
    if (!ctrl->erst) {
        kprintf("[XHCI] Failed to allocate ERST\n");
        return;
    }
    // Align to 64 bytes
    uint64_t erst_addr = (uint64_t)ctrl->erst;
    if (erst_addr & 0x3F) {
        erst_addr = (erst_addr + 63) & ~63ULL;
        ctrl->erst = (xhci_erst_entry_t*)erst_addr;
    }
    ctrl->erst_phys = mm_get_physical_address((uint64_t)ctrl->erst);
    
    // Configure ERST entry 0
    ctrl->erst[0].base = ctrl->event_ring_phys;
    ctrl->erst[0].size = XHCI_RING_SIZE;
    ctrl->erst[0].reserved = 0;
    
    // Configure interrupter 0 runtime registers
    // ERSTSZ = 1 (one segment)
    xhci_rt_write32(ctrl, 0x28, 1);
    // ERDP = physical address of event ring
    xhci_rt_write64(ctrl, 0x38, ctrl->event_ring_phys);
    // ERSTBA = physical address of ERST
    xhci_rt_write64(ctrl, 0x30, ctrl->erst_phys);
    
    xhci_dbg("Event ring configured at 0x%llx\n", ctrl->event_ring_phys);
}

int xhci_reset(xhci_controller_t* ctrl) {
    // Stop controller first
    uint32_t cmd = xhci_op_read32(ctrl, XHCI_OP_USBCMD);
    xhci_op_write32(ctrl, XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);
    
    // Wait for halt
    for (int i = 0; i < 100; i++) {
        if (xhci_op_read32(ctrl, XHCI_OP_USBSTS) & XHCI_STS_HCH) break;
        delay_ms(1);
    }
    
    // Issue reset
    xhci_op_write32(ctrl, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    
    // Wait for reset to complete
    for (int i = 0; i < 1000; i++) {
        uint32_t cmd_val = xhci_op_read32(ctrl, XHCI_OP_USBCMD);
        uint32_t sts_val = xhci_op_read32(ctrl, XHCI_OP_USBSTS);
        if (!(cmd_val & XHCI_CMD_HCRST) && !(sts_val & XHCI_STS_CNR)) {
            xhci_dbg("Reset complete\n");
            return ST_OK;
        }
        delay_ms(1);
    }
    
    kprintf("[XHCI] Reset timeout\n");
    return ST_TIMEOUT;
}

int xhci_start(xhci_controller_t* ctrl) {
    // Wait for controller not ready to clear
    if (xhci_wait_ready(ctrl, 100) != ST_OK) {
        return ST_ERR;
    }
    
    // Start controller
    uint32_t cmd = xhci_op_read32(ctrl, XHCI_OP_USBCMD);
    xhci_op_write32(ctrl, XHCI_OP_USBCMD, cmd | XHCI_CMD_RUN);
    
    // Wait for running
    for (int i = 0; i < 100; i++) {
        if (!(xhci_op_read32(ctrl, XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
            ctrl->running = 1;
            xhci_dbg("Controller running\n");
            return ST_OK;
        }
        delay_ms(1);
    }
    
    kprintf("[XHCI] Failed to start\n");
    return ST_TIMEOUT;
}

void xhci_stop(xhci_controller_t* ctrl) {
    uint32_t cmd = xhci_op_read32(ctrl, XHCI_OP_USBCMD);
    xhci_op_write32(ctrl, XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);
    ctrl->running = 0;
}

static int xhci_wait_ready(xhci_controller_t* ctrl, uint32_t timeout_ms) {
    for (uint32_t i = 0; i < timeout_ms; i++) {
        if (!(xhci_op_read32(ctrl, XHCI_OP_USBSTS) & XHCI_STS_CNR)) {
            return ST_OK;
        }
        delay_ms(1);
    }
    return ST_TIMEOUT;
}

//=============================================================================
// Command Ring Operations
//=============================================================================

// Pending command state
static volatile uint8_t g_cmd_complete = 0;
static volatile uint8_t g_cmd_cc = 0;
static volatile uint32_t g_cmd_slot = 0;

int xhci_send_command(xhci_controller_t* ctrl, uint64_t param, uint32_t status, uint32_t control) {
    g_cmd_complete = 0;
    g_cmd_cc = TRB_CC_INVALID;
    g_cmd_slot = 0;
    
    int idx = xhci_ring_enqueue(ctrl->cmd_ring, param, status, control);
    if (idx < 0) return ST_ERR;
    
    // Ring command doorbell (slot 0, target 0)
    xhci_ring_doorbell(ctrl, 0, 0);
    
    return ST_OK;
}

int xhci_wait_command(xhci_controller_t* ctrl, uint32_t timeout_ms) {
    for (uint32_t i = 0; i < timeout_ms * 10; i++) {  // 10x iterations with 100us delays
        // Process events (either from interrupt or polling)
        xhci_process_events(ctrl);
        
        if (g_cmd_complete) {
            if (g_cmd_cc == TRB_CC_SUCCESS) {
                return ST_OK;
            }
            xhci_dbg("Command failed with CC=%d\n", g_cmd_cc);
            return ST_ERR;
        }
        
        delay_us(100);  // 100 microsecond delay for faster response
    }
    
    return ST_TIMEOUT;
}

//=============================================================================
// Event Processing
//=============================================================================

void xhci_irq_service(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->initialized) return;
    
    // Check if we have an interrupt pending
    uint32_t sts = xhci_op_read32(ctrl, XHCI_OP_USBSTS);
    if (!(sts & XHCI_STS_EINT)) return;
    
    // Clear interrupt
    xhci_op_write32(ctrl, XHCI_OP_USBSTS, XHCI_STS_EINT);
    
    // Clear interrupter pending
    uint32_t iman = xhci_rt_read32(ctrl, 0x20);
    xhci_rt_write32(ctrl, 0x20, iman | (1 << 0)); // Clear IP
    
    // Process events
    xhci_process_events(ctrl);
}

void xhci_process_events(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->event_ring) return;
    
    xhci_ring_t* ring = ctrl->event_ring;
    int processed = 0;
    
    while (processed < XHCI_RING_SIZE) {
        xhci_trb_t* trb = &ring->trbs[ring->dequeue];
        
        // Check cycle bit matches expected
        uint8_t trb_cycle = (trb->control & TRB_FLAG_CYCLE) ? 1 : 0;
        if (trb_cycle != ring->cycle) {
            break;  // No more events
        }
        
        uint8_t trb_type = (trb->control >> 10) & 0x3F;
        
        switch (trb_type) {
            case TRB_TYPE_TRANSFER:
                xhci_handle_transfer_event(ctrl, trb);
                break;
            case TRB_TYPE_CMD_COMPLETE:
                xhci_handle_command_event(ctrl, trb);
                break;
            case TRB_TYPE_PORT_STATUS:
                xhci_handle_port_event(ctrl, trb);
                break;
            case TRB_TYPE_HOST_CTRL:
                xhci_dbg("Host controller event\n");
                break;
            default:
                xhci_dbg("Unknown event type %d\n", trb_type);
                break;
        }
        
        // Advance dequeue
        ring->dequeue++;
        if (ring->dequeue >= XHCI_RING_SIZE) {
            ring->dequeue = 0;
            ring->cycle ^= 1;
        }
        
        processed++;
    }
    
    // Update ERDP to acknowledge processed events
    if (processed > 0) {
        uint64_t erdp = mm_get_physical_address((uint64_t)&ring->trbs[ring->dequeue]);
        xhci_rt_write64(ctrl, 0x38, erdp | (1 << 3)); // Set EHB
    }
}

static void xhci_handle_command_event(xhci_controller_t* ctrl, xhci_trb_t* trb) {
    (void)ctrl;
    g_cmd_cc = (trb->status >> 24) & 0xFF;
    g_cmd_slot = (trb->control >> 24) & 0xFF;
    g_cmd_complete = 1;
    xhci_dbg("Command complete: CC=%d, Slot=%d\n", g_cmd_cc, g_cmd_slot);
}

static void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* trb) {
    uint8_t cc = (trb->status >> 24) & 0xFF;
    uint32_t residue = trb->status & 0xFFFFFF;
    uint8_t slot = (trb->control >> 24) & 0xFF;
    uint8_t ep_id = (trb->control >> 16) & 0x1F;
    
    xhci_dbg("Transfer event: Slot=%d, EP=%d, CC=%d, Residue=%d\n", slot, ep_id, cc, residue);
    
    // Find pending transfer
    if (slot > 0 && slot <= XHCI_MAX_SLOTS && ep_id < XHCI_MAX_ENDPOINTS) {
        xhci_transfer_t* xfer = ctrl->pending_xfer[slot - 1][ep_id];
        if (xfer) {
            xfer->cc = cc;
            xfer->bytes_transferred = residue;  // Note: this is residue, not transferred
            xfer->completed = 1;
        }
    }
}

static void xhci_handle_port_event(xhci_controller_t* ctrl, xhci_trb_t* trb) {
    uint8_t port = ((trb->param >> 24) & 0xFF);
    xhci_dbg("Port status change: Port %d\n", port);
    
    // Clear port status change bits
    if (port > 0 && port <= ctrl->max_ports) {
        uint32_t portsc = xhci_op_read32(ctrl, XHCI_OP_PORTSC_BASE + (port - 1) * 0x10);
        // Write 1 to clear change bits, preserve power and enabled
        xhci_op_write32(ctrl, XHCI_OP_PORTSC_BASE + (port - 1) * 0x10,
                        (portsc & (XHCI_PORTSC_PP | XHCI_PORTSC_PED)) | XHCI_PORTSC_WPR_MASK);
    }
}

//=============================================================================
// Port Management
//=============================================================================

int xhci_poll_ports(xhci_controller_t* ctrl) {
    int connected = 0;
    
    for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
        uint32_t portsc = xhci_op_read32(ctrl, XHCI_OP_PORTSC_BASE + (port - 1) * 0x10);
        
        if (portsc & XHCI_PORTSC_CCS) {
            xhci_dbg("Port %d: Connected, PORTSC=0x%08x\n", port, portsc);
            connected++;
            
            // Check if this port is already enumerated
            int already_enumerated = 0;
            for (int i = 0; i < ctrl->num_devices; i++) {
                if (ctrl->devices[i].port == port && ctrl->devices[i].configured) {
                    already_enumerated = 1;
                    break;
                }
            }
            
            // Try to enumerate if not already done for this port
            if (!already_enumerated && ctrl->num_devices < XHCI_MAX_SLOTS) {
                xhci_dbg("Port %d: Attempting enumeration...\n", port);
                if (xhci_enumerate_device(ctrl, port) == ST_OK) {
                    xhci_dbg("Device enumerated on port %d\n", port);
                } else {
                    xhci_dbg("Port %d: Enumeration failed\n", port);
                }
            }
        }
    }
    
    return connected;
}

int xhci_port_reset(xhci_controller_t* ctrl, uint8_t port) {
    if (port < 1 || port > ctrl->max_ports) return ST_INVALID;
    
    uint32_t off = XHCI_OP_PORTSC_BASE + (port - 1) * 0x10;
    uint32_t portsc = xhci_op_read32(ctrl, off);
    
    xhci_dbg("Port %d before reset: PORTSC=0x%08x\n", port, portsc);
    
    // Check if port is already enabled (SuperSpeed devices may not need reset)
    if (portsc & XHCI_PORTSC_PED) {
        xhci_dbg("Port %d already enabled, skipping reset\n", port);
        return ST_OK;
    }
    
    // For USB 3.0 ports, use warm reset if needed
    // For USB 2.0 ports, use regular reset
    uint8_t speed = (portsc >> 10) & 0xF;
    
    if (speed == XHCI_SPEED_SUPER) {
        // SuperSpeed: set warm reset (bit 31)
        portsc = (portsc & XHCI_PORTSC_PP) | (1 << 31);  // WPR
    } else {
        // Full/High Speed: set regular reset
        portsc = (portsc & XHCI_PORTSC_PP) | XHCI_PORTSC_PR;
    }
    
    xhci_op_write32(ctrl, off, portsc);
    
    // Wait for reset to complete (PRC or WRC will be set)
    for (int i = 0; i < 500; i++) {
        delay_ms(1);
        portsc = xhci_op_read32(ctrl, off);
        
        // Check for Port Reset Change or Warm Port Reset Change
        if ((portsc & XHCI_PORTSC_PRC) || (portsc & XHCI_PORTSC_WRC)) {
            // Clear the change bits
            xhci_op_write32(ctrl, off, (portsc & (XHCI_PORTSC_PP | XHCI_PORTSC_PED)) | 
                            (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC));
            xhci_dbg("Port %d reset complete, PORTSC=0x%08x\n", port, portsc);
            return ST_OK;
        }
        
        // Check if port became enabled without explicit PRC
        if (portsc & XHCI_PORTSC_PED) {
            xhci_dbg("Port %d now enabled, PORTSC=0x%08x\n", port, portsc);
            return ST_OK;
        }
    }
    
    // Even if timeout, check if port is now enabled
    portsc = xhci_op_read32(ctrl, off);
    if (portsc & XHCI_PORTSC_PED) {
        xhci_dbg("Port %d enabled after timeout, PORTSC=0x%08x\n", port, portsc);
        return ST_OK;
    }
    
    kprintf("[XHCI] Port %d reset timeout, PORTSC=0x%08x\n", port, portsc);
    return ST_TIMEOUT;
}

uint8_t xhci_port_speed(xhci_controller_t* ctrl, uint8_t port) {
    if (port < 1 || port > ctrl->max_ports) return 0;
    uint32_t portsc = xhci_op_read32(ctrl, XHCI_OP_PORTSC_BASE + (port - 1) * 0x10);
    return (portsc >> 10) & 0xF;
}

//=============================================================================
// Device Enumeration
//=============================================================================

int xhci_enable_slot(xhci_controller_t* ctrl) {
    // Send Enable Slot command
    uint32_t control = (TRB_TYPE_ENABLE_SLOT << 10);
    
    if (xhci_send_command(ctrl, 0, 0, control) != ST_OK) {
        return -1;
    }
    
    if (xhci_wait_command(ctrl, 1000) != ST_OK) {
        return -1;
    }
    
    return g_cmd_slot;  // Slot ID assigned by controller
}

int xhci_address_device(xhci_controller_t* ctrl, uint8_t slot, uint8_t port, uint8_t speed) {
    if (slot == 0 || slot > ctrl->max_slots) return ST_INVALID;
    
    // Allocate device context
    xhci_dev_ctx_t* dev_ctx = (xhci_dev_ctx_t*)kcalloc(1, sizeof(xhci_dev_ctx_t) + 64);
    if (!dev_ctx) return ST_NOMEM;
    
    // Align to 64 bytes
    uint64_t ctx_addr = (uint64_t)dev_ctx;
    if (ctx_addr & 0x3F) {
        ctx_addr = (ctx_addr + 63) & ~63ULL;
        dev_ctx = (xhci_dev_ctx_t*)ctx_addr;
    }
    
    ctrl->dev_ctx[slot - 1] = dev_ctx;
    uint64_t dev_ctx_phys = mm_get_physical_address((uint64_t)dev_ctx);
    ctrl->dcbaa[slot] = dev_ctx_phys;
    
    // Allocate transfer ring for EP0 (control endpoint)
    usb_device_t* udev = &ctrl->devices[slot - 1];
    udev->slot_id = slot;
    udev->port = port;
    udev->speed = speed;
    udev->controller = ctrl;
    udev->bulk_in_ring = xhci_alloc_ring();
    
    if (!udev->bulk_in_ring) {
        kprintf("[XHCI] Failed to allocate EP0 ring\n");
        return ST_NOMEM;
    }
    
    uint64_t ep0_ring_phys = mm_get_physical_address((uint64_t)udev->bulk_in_ring->trbs);
    xhci_ring_init(udev->bulk_in_ring, ep0_ring_phys);
    
    // Determine max packet size for EP0 based on speed
    uint16_t max_pkt_ep0;
    switch (speed) {
        case XHCI_SPEED_LOW:    max_pkt_ep0 = 8; break;
        case XHCI_SPEED_FULL:   max_pkt_ep0 = 8; break;  // Start with 8, will update
        case XHCI_SPEED_HIGH:   max_pkt_ep0 = 64; break;
        case XHCI_SPEED_SUPER:  max_pkt_ep0 = 512; break;
        default:                max_pkt_ep0 = 8; break;
    }
    udev->max_packet_ep0 = max_pkt_ep0;
    
    // Setup input context
    xhci_input_ctx_t* ictx = ctrl->input_ctx;
    xhci_memset(ictx, 0, sizeof(*ictx));
    
    // Add flags: Slot context (bit 0) and EP0 (bit 1)
    ictx->add_flags = (1 << 0) | (1 << 1);
    ictx->drop_flags = 0;
    
    // Slot context
    uint32_t route = 0;  // No route string for directly attached device
    ictx->slot.route_speed_entries = route | (speed << 20) | (1 << 27); // 1 context entry
    ictx->slot.latency_hub_ports = (port << 16);  // Root hub port number
    ictx->slot.tt_info = 0;
    ictx->slot.slot_state = 0;
    
    // EP0 context (endpoint 1 in context = EP0, DCI = 1)
    xhci_ep_ctx_t* ep0 = &ictx->endpoints[0];
    ep0->ep_info1 = 0;  // Interval = 0 for control
    ep0->ep_info2 = (3 << 1) |            // CErr = 3
                    (EP_TYPE_CONTROL << 3) |   // EP Type
                    (max_pkt_ep0 << 16);       // Max packet size
    ep0->tr_dequeue = ep0_ring_phys | 1;  // DCS = 1
    ep0->avg_trb_len = 8;  // Average for control
    
    // Send Address Device command (with BSR=1 first to set address without requesting)
    uint32_t control = (TRB_TYPE_ADDRESS_DEV << 10) | (slot << 24) | TRB_FLAG_BSR;
    
    if (xhci_send_command(ctrl, ctrl->input_ctx_phys, 0, control) != ST_OK) {
        return ST_ERR;
    }
    
    if (xhci_wait_command(ctrl, 1000) != ST_OK) {
        kprintf("[XHCI] Address Device (BSR) failed\n");
        return ST_ERR;
    }
    
    // Now send Address Device without BSR to complete addressing
    control = (TRB_TYPE_ADDRESS_DEV << 10) | (slot << 24);
    
    if (xhci_send_command(ctrl, ctrl->input_ctx_phys, 0, control) != ST_OK) {
        return ST_ERR;
    }
    
    if (xhci_wait_command(ctrl, 1000) != ST_OK) {
        kprintf("[XHCI] Address Device failed\n");
        return ST_ERR;
    }
    
    // Read back assigned address from device context
    udev->address = dev_ctx->slot.slot_state & 0xFF;
    xhci_dbg("Device addressed: Slot=%d, Address=%d\n", slot, udev->address);
    
    return ST_OK;
}

int xhci_enumerate_device(xhci_controller_t* ctrl, uint8_t port) {
    // Reset port
    if (xhci_port_reset(ctrl, port) != ST_OK) {
        return ST_ERR;
    }
    
    delay_ms(50);  // Debounce
    
    // Get port speed
    uint8_t speed = xhci_port_speed(ctrl, port);
    xhci_dbg("Port %d speed: %d\n", port, speed);
    
    // Enable slot
    int slot = xhci_enable_slot(ctrl);
    if (slot <= 0) {
        kprintf("[XHCI] Failed to enable slot\n");
        return ST_ERR;
    }
    xhci_dbg("Slot enabled: %d\n", slot);
    
    // Address device
    if (xhci_address_device(ctrl, slot, port, speed) != ST_OK) {
        kprintf("[XHCI] Failed to address device\n");
        return ST_ERR;
    }
    
    usb_device_t* dev = &ctrl->devices[slot - 1];
    
    // Allocate DMA-safe PAGE-ALIGNED buffer for descriptors
    // kcalloc only provides 8-byte alignment, so allocate extra and align manually
    uint8_t* raw_buf = (uint8_t*)kcalloc(1, 4096 + 4096);
    if (!raw_buf) {
        kprintf("[XHCI] Failed to allocate DMA buffer\n");
        return ST_NOMEM;
    }
    uint8_t* dma_buf = (uint8_t*)(((uint64_t)raw_buf + 4095) & ~4095ULL);
    usb_device_desc_t* desc = (usb_device_desc_t*)dma_buf;
    
    // Get device descriptor (first 8 bytes to get max packet size)
    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_D2H | USB_RT_STD | USB_RT_DEV,
                              USB_REQ_GET_DESCRIPTOR,
                              (USB_DESC_DEVICE << 8) | 0,
                              0, 8, desc) != ST_OK) {
        kprintf("[XHCI] Failed to get device descriptor (8 bytes)\n");
        // Try to continue anyway
    } else {
        dev->max_packet_ep0 = desc->max_pkt_ep0;
        xhci_dbg("MaxPacketEP0: %d (raw bytes: %02x %02x %02x %02x)\n", 
                 dev->max_packet_ep0,
                 dma_buf[0], dma_buf[1], dma_buf[2], dma_buf[3]);
    }
    
    // Get full device descriptor
    xhci_memset(dma_buf, 0, 256);
    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_D2H | USB_RT_STD | USB_RT_DEV,
                              USB_REQ_GET_DESCRIPTOR,
                              (USB_DESC_DEVICE << 8) | 0,
                              0, 18, desc) == ST_OK) {
        dev->vendor_id = desc->vendor_id;
        dev->product_id = desc->product_id;
        dev->class_code = desc->class_code;
        dev->subclass = desc->subclass;
        dev->protocol = desc->protocol;
        dev->num_configs = desc->num_configs;
        
        xhci_dbg("Device: VID=%04x PID=%04x Class=%02x/%02x/%02x\n",
                 dev->vendor_id, dev->product_id,
                 dev->class_code, dev->subclass, dev->protocol);
    }
    
    // Get configuration descriptor to find interfaces and endpoints
    // Use the same DMA buffer we already allocated
    xhci_memset(dma_buf, 0, 256);
    
    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_D2H | USB_RT_STD | USB_RT_DEV,
                              USB_REQ_GET_DESCRIPTOR,
                              (USB_DESC_CONFIG << 8) | 0,
                              0, 256, dma_buf) == ST_OK) {
        // Parse configuration descriptor
        usb_config_desc_t* cfg = (usb_config_desc_t*)dma_buf;
        uint8_t* ptr = dma_buf + cfg->length;
        uint8_t* end = dma_buf + cfg->total_length;
        
        while (ptr < end) {
            uint8_t len = ptr[0];
            uint8_t type = ptr[1];
            
            if (len == 0) break;
            
            if (type == USB_DESC_INTERFACE) {
                usb_interface_desc_t* iface = (usb_interface_desc_t*)ptr;
                xhci_dbg("Interface: %d/%d/%d, EPs=%d\n",
                         iface->class_code, iface->subclass, iface->protocol,
                         iface->num_endpoints);
                
                // Check for Mass Storage
                if (iface->class_code == USB_CLASS_MASS_STORAGE &&
                    iface->subclass == 0x06 &&
                    iface->protocol == 0x50) {
                    dev->class_code = iface->class_code;
                    dev->subclass = iface->subclass;
                    dev->protocol = iface->protocol;
                }
            } else if (type == USB_DESC_ENDPOINT) {
                usb_endpoint_desc_t* ep = (usb_endpoint_desc_t*)ptr;
                uint8_t ep_type = ep->attributes & USB_EP_TYPE_MASK;
                uint8_t ep_num = ep->address & USB_EP_NUM_MASK;
                uint8_t ep_in = (ep->address & USB_EP_DIR_IN) ? 1 : 0;
                
                xhci_dbg("Endpoint: 0x%02x, Type=%d, MaxPkt=%d\n",
                         ep->address, ep_type, ep->max_packet);
                
                if (ep_type == USB_EP_TYPE_BULK) {
                    if (ep_in) {
                        dev->bulk_in_ep = ep_num;
                        dev->bulk_in_max_pkt = ep->max_packet;
                    } else {
                        dev->bulk_out_ep = ep_num;
                        dev->bulk_out_max_pkt = ep->max_packet;
                    }
                }
            }
            
            ptr += len;
        }
    }
    
    // Set configuration
    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_H2D | USB_RT_STD | USB_RT_DEV,
                              USB_REQ_SET_CONFIG,
                              1, 0, 0, NULL) != ST_OK) {
        kprintf("[XHCI] Failed to set configuration\n");
        // Continue anyway, some devices work without this
    }
    
    // Free DMA buffer (free the raw allocation, not the aligned pointer)
    kfree(raw_buf);
    
    // Configure bulk endpoints if found
    if (dev->bulk_in_ep && dev->bulk_out_ep) {
        if (xhci_configure_endpoint(ctrl, slot, dev->bulk_in_ep,
                                    EP_TYPE_BULK_IN, dev->bulk_in_max_pkt, 0) != ST_OK) {
            kprintf("[XHCI] Failed to configure bulk IN endpoint\n");
        }
        
        if (xhci_configure_endpoint(ctrl, slot, dev->bulk_out_ep,
                                    EP_TYPE_BULK_OUT, dev->bulk_out_max_pkt, 0) != ST_OK) {
            kprintf("[XHCI] Failed to configure bulk OUT endpoint\n");
        }
    }
    
    dev->configured = 1;
    ctrl->num_devices++;
    
    return ST_OK;
}

int xhci_configure_endpoint(xhci_controller_t* ctrl, uint8_t slot, uint8_t ep_num,
                            uint8_t ep_type, uint16_t max_packet, uint8_t interval) {
    if (slot == 0 || slot > ctrl->max_slots) return ST_INVALID;
    
    usb_device_t* dev = &ctrl->devices[slot - 1];
    
    // Calculate endpoint index (DCI)
    // DCI = endpoint number * 2 + direction (0=out, 1=in)
    uint8_t dci;
    if (ep_type == EP_TYPE_BULK_IN || ep_type == EP_TYPE_INTERRUPT_IN || ep_type == EP_TYPE_ISOCH_IN) {
        dci = ep_num * 2 + 1;
    } else {
        dci = ep_num * 2;
    }
    
    // Allocate transfer ring for this endpoint
    xhci_ring_t* ring = xhci_alloc_ring();
    if (!ring) return ST_NOMEM;
    
    uint64_t ring_phys = mm_get_physical_address((uint64_t)ring->trbs);
    xhci_ring_init(ring, ring_phys);
    
    // Store ring pointer
    if (ep_type == EP_TYPE_BULK_IN) {
        dev->bulk_in_ring = ring;
    } else if (ep_type == EP_TYPE_BULK_OUT) {
        dev->bulk_out_ring = ring;
    }
    
    // Setup input context
    xhci_input_ctx_t* ictx = ctrl->input_ctx;
    xhci_memset(ictx, 0, sizeof(*ictx));
    
    // Add flags: Slot context and the endpoint
    ictx->add_flags = (1 << 0) | (1 << dci);
    ictx->drop_flags = 0;
    
    // Copy slot context from device context and update
    xhci_dev_ctx_t* dev_ctx = ctrl->dev_ctx[slot - 1];
    xhci_memcpy(&ictx->slot, &dev_ctx->slot, sizeof(xhci_slot_ctx_t));
    
    // Update context entries to include this endpoint
    uint32_t entries = (ictx->slot.route_speed_entries >> 27) & 0x1F;
    if (dci > entries) {
        ictx->slot.route_speed_entries = (ictx->slot.route_speed_entries & 0x07FFFFFF) | (dci << 27);
    }
    
    // Setup endpoint context
    xhci_ep_ctx_t* ep = &ictx->endpoints[dci - 1];
    ep->ep_info1 = (interval << 16);  // Interval
    ep->ep_info2 = (3 << 1) |                    // CErr = 3
                   (ep_type << 3) |               // EP Type
                   (max_packet << 16);            // Max packet size
    ep->tr_dequeue = ring_phys | 1;  // DCS = 1
    ep->avg_trb_len = max_packet;
    
    // Send Configure Endpoint command
    uint32_t control = (TRB_TYPE_CONFIG_EP << 10) | (slot << 24);
    
    if (xhci_send_command(ctrl, ctrl->input_ctx_phys, 0, control) != ST_OK) {
        return ST_ERR;
    }
    
    if (xhci_wait_command(ctrl, 1000) != ST_OK) {
        kprintf("[XHCI] Configure Endpoint failed\n");
        return ST_ERR;
    }
    
    xhci_dbg("Endpoint configured: Slot=%d, DCI=%d, Type=%d\n", slot, dci, ep_type);
    return ST_OK;
}

//=============================================================================
// Control Transfers
//=============================================================================

int xhci_control_transfer(xhci_controller_t* ctrl, usb_device_t* dev,
                         uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                         void* data) {
    if (!dev || !dev->bulk_in_ring) return ST_INVALID;
    
    xhci_ring_t* ring = dev->bulk_in_ring;  // EP0 uses bulk_in_ring
    uint8_t slot = dev->slot_id;
    uint8_t direction = (bmRequestType & USB_RT_D2H) ? 1 : 0;
    
    // Determine number of TRBs we'll enqueue
    int num_trbs = 2;  // Setup + Status
    if (wLength > 0 && data) {
        num_trbs = 3;  // Setup + Data + Status
    }
    
    // Setup TRB - NO IOC, chain to next
    uint64_t setup_data = (uint64_t)bmRequestType |
                          ((uint64_t)bRequest << 8) |
                          ((uint64_t)wValue << 16) |
                          ((uint64_t)wIndex << 32) |
                          ((uint64_t)wLength << 48);
    
    uint32_t setup_status = 8;  // TRB transfer length = 8 bytes
    uint32_t setup_control = (TRB_TYPE_SETUP << 10) | TRB_FLAG_IDT;
    
    if (wLength > 0) {
        // TRT: 3 = IN data, 2 = OUT data
        setup_control |= (direction ? 3 : 2) << 16;
    }
    
    xhci_transfer_t xfer = {0, 0, 0};
    ctrl->pending_xfer[slot - 1][1] = &xfer;  // EP0 = DCI 1
    
    // Enqueue Setup TRB
    xhci_ring_enqueue(ring, setup_data, setup_status, setup_control);
    
    // Data TRB if needed - NO IOC, chain to status
    if (wLength > 0 && data) {
        uint64_t data_phys = mm_get_physical_address((uint64_t)data);
        uint32_t data_status = wLength;
        uint32_t data_control = (TRB_TYPE_DATA << 10);
        
        if (direction) {
            data_control |= (1 << 16);  // DIR = IN
        }
        
        xhci_ring_enqueue(ring, data_phys, data_status, data_control);
    }
    
    // Status TRB - IOC only here to get one completion for entire transfer
    uint32_t status_control = (TRB_TYPE_STATUS << 10) | TRB_FLAG_IOC;
    if (!direction && wLength > 0) {
        status_control |= (1 << 16);  // DIR = IN for status phase of OUT transfer
    } else if (direction && wLength > 0) {
        // For IN transfers, status phase is OUT direction (no DIR bit)
    }
    
    xhci_ring_enqueue(ring, 0, 0, status_control);
    
    // Ring doorbell for EP0 (DCI = 1)
    xhci_ring_doorbell(ctrl, slot, 1);
    
    // Wait for completion (only Status TRB has IOC, so one event expected)
    for (int i = 0; i < 1000; i++) {
        xhci_process_events(ctrl);
        
        if (xfer.completed) {
            ctrl->pending_xfer[slot - 1][1] = NULL;
            
            if (xfer.cc == TRB_CC_SUCCESS || xfer.cc == TRB_CC_SHORT_PACKET) {
                return ST_OK;
            }
            
            xhci_dbg("Control transfer failed: CC=%d\n", xfer.cc);
            return ST_IO;
        }
        
        delay_ms(1);
    }
    
    ctrl->pending_xfer[slot - 1][1] = NULL;
    return ST_TIMEOUT;
}

//=============================================================================
// Bulk Transfers
//=============================================================================

int xhci_bulk_transfer_in(xhci_controller_t* ctrl, usb_device_t* dev,
                          void* buf, uint32_t len, uint32_t* transferred) {
    if (!dev || !dev->bulk_in_ring || !dev->bulk_in_ep) return ST_INVALID;
    
    xhci_ring_t* ring = dev->bulk_in_ring;
    uint8_t slot = dev->slot_id;
    uint8_t dci = dev->bulk_in_ep * 2 + 1;  // IN endpoint DCI
    
    xhci_dbg("Bulk IN: slot=%d, dci=%d, len=%d, ring enq=%d\n", slot, dci, len, ring->enqueue);
    
    // Pre-calculate physical address BEFORE enqueueing
    uint64_t buf_phys = mm_get_physical_address((uint64_t)buf);
    
    xhci_transfer_t xfer = {0, 0, 0};
    ctrl->pending_xfer[slot - 1][dci] = &xfer;
    
    // Enqueue Normal TRB
    uint32_t status = len;
    uint32_t control = (TRB_TYPE_NORMAL << 10) | TRB_FLAG_IOC | TRB_FLAG_ISP;
    
    xhci_ring_enqueue(ring, buf_phys, status, control);
    
    // Ring doorbell
    xhci_ring_doorbell(ctrl, slot, dci);
    
    // Wait for completion (ultra-fast polling with 100us delays)
    for (int i = 0; i < 50000; i++) {  // 5 second timeout total
        xhci_process_events(ctrl);
        
        if (xfer.completed) {
            ctrl->pending_xfer[slot - 1][dci] = NULL;
            
            // Memory barrier to ensure CPU sees DMA-written data
            __asm__ volatile("mfence" ::: "memory");
            
            if (xfer.cc == TRB_CC_SUCCESS || xfer.cc == TRB_CC_SHORT_PACKET) {
                if (transferred) {
                    // bytes_transferred is actually residue
                    *transferred = len - xfer.bytes_transferred;
                }
                return ST_OK;
            }
            
            return ST_IO;
        }
        
        delay_us(100);  // 100 microsecond delay for ultra-fast response
    }
    
    ctrl->pending_xfer[slot - 1][dci] = NULL;
    return ST_TIMEOUT;
}

int xhci_bulk_transfer_out(xhci_controller_t* ctrl, usb_device_t* dev,
                           const void* buf, uint32_t len, uint32_t* transferred) {
    if (!dev || !dev->bulk_out_ring || !dev->bulk_out_ep) return ST_INVALID;
    
    xhci_ring_t* ring = dev->bulk_out_ring;
    uint8_t slot = dev->slot_id;
    uint8_t dci = dev->bulk_out_ep * 2;  // OUT endpoint DCI
    
    // Pre-calculate physical address BEFORE enqueueing
    uint64_t buf_phys = mm_get_physical_address((uint64_t)buf);
    
    xhci_transfer_t xfer = {0, 0, 0};
    ctrl->pending_xfer[slot - 1][dci] = &xfer;
    
    // Enqueue Normal TRB
    uint32_t status = len;
    uint32_t control = (TRB_TYPE_NORMAL << 10) | TRB_FLAG_IOC;
    
    xhci_ring_enqueue(ring, buf_phys, status, control);
    
    // Ring doorbell
    xhci_ring_doorbell(ctrl, slot, dci);
    
    // Wait for completion (ultra-fast polling with 100us delays)
    for (int i = 0; i < 50000; i++) {  // 5 second timeout total
        xhci_process_events(ctrl);
        
        if (xfer.completed) {
            ctrl->pending_xfer[slot - 1][dci] = NULL;
            
            if (transferred) {
                *transferred = len - xfer.bytes_transferred;
            }
            
            if (xfer.cc == TRB_CC_SUCCESS) {
                return ST_OK;
            }
            
            return ST_IO;
        }
        
        delay_us(100);  // 100 microsecond delay for ultra-fast response
    }
    
    ctrl->pending_xfer[slot - 1][dci] = NULL;
    return ST_TIMEOUT;
}

void xhci_shutdown(xhci_controller_t* ctrl) {
    if (!ctrl) return;
    
    xhci_stop(ctrl);
    
    // Free rings
    if (ctrl->cmd_ring) xhci_free_ring(ctrl->cmd_ring);
    if (ctrl->event_ring) xhci_free_ring(ctrl->event_ring);
    
    // Free device resources
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (ctrl->dev_ctx[i]) kfree(ctrl->dev_ctx[i]);
        if (ctrl->devices[i].bulk_in_ring) xhci_free_ring(ctrl->devices[i].bulk_in_ring);
        if (ctrl->devices[i].bulk_out_ring) xhci_free_ring(ctrl->devices[i].bulk_out_ring);
    }
    
    ctrl->initialized = 0;
}
