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
    // Use DMA-safe allocation for low physical addresses required by XHCI
    size_t ring_size = sizeof(xhci_ring_t);
    size_t alloc_size = ring_size + 4096;  // Extra page for alignment
    
    uint8_t* raw = (uint8_t*)kcalloc_dma(1, alloc_size);
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
// Expandable Ring Management (Dynamic Ring Expansion)
//=============================================================================

// Allocate a single ring segment (page-aligned for DMA)
xhci_ring_segment_t* xhci_segment_alloc(void) {
    // Ring segments need page alignment for DMA
    // Use DMA-safe allocation for low physical addresses
    size_t seg_size = sizeof(xhci_ring_segment_t);
    size_t alloc_size = seg_size + 4096;
    
    uint8_t* raw = (uint8_t*)kcalloc_dma(1, alloc_size);
    if (!raw) return NULL;
    
    // Align to 4KB page boundary
    uint64_t raw_addr = (uint64_t)raw;
    uint64_t aligned_addr = (raw_addr + 4095) & ~4095ULL;
    xhci_ring_segment_t* seg = (xhci_ring_segment_t*)aligned_addr;
    
    xhci_memset(seg, 0, seg_size);
    seg->dma = mm_get_physical_address(aligned_addr);
    seg->next = NULL;
    seg->num = 0;
    
    return seg;
}

void xhci_segment_free(xhci_ring_segment_t* seg) {
    // Note: Similar to xhci_free_ring, we don't track raw pointer
    (void)seg;
}

// Set up link TRB at end of segment to point to next segment
static void xhci_set_link_trb(xhci_ring_segment_t* seg, int chain_links) {
    if (!seg || !seg->next) return;
    
    xhci_trb_t* link = &seg->trbs[XHCI_RING_SIZE - 1];
    link->param = seg->next->dma;
    link->status = 0;
    
    uint32_t ctrl = (TRB_TYPE_LINK << 10);
    if (chain_links) ctrl |= TRB_FLAG_CHAIN;
    link->control = ctrl;
}

// Allocate an expandable ring with initial segments
xhci_ring_ex_t* xhci_ring_ex_alloc(uint32_t num_segs) {
    if (num_segs == 0) num_segs = 1;
    if (num_segs > XHCI_MAX_RING_SEGMENTS) num_segs = XHCI_MAX_RING_SEGMENTS;
    
    xhci_ring_ex_t* ring = (xhci_ring_ex_t*)kcalloc_dma(1, sizeof(xhci_ring_ex_t));
    if (!ring) return NULL;
    
    // Allocate segments
    xhci_ring_segment_t* prev = NULL;
    for (uint32_t i = 0; i < num_segs; i++) {
        xhci_ring_segment_t* seg = xhci_segment_alloc();
        if (!seg) {
            // Cleanup on failure
            xhci_ring_ex_free(ring);
            return NULL;
        }
        seg->num = i;
        
        if (i == 0) {
            ring->first_seg = seg;
        }
        if (prev) {
            prev->next = seg;
            xhci_set_link_trb(prev, 0);
        }
        prev = seg;
    }
    
    // Complete the circular list
    ring->last_seg = prev;
    prev->next = ring->first_seg;
    xhci_set_link_trb(prev, 0);
    
    // Set toggle cycle on last segment's link TRB
    ring->last_seg->trbs[XHCI_RING_SIZE - 1].control |= TRB_FLAG_TC;
    
    // Initialize ring state
    ring->enq_seg = ring->first_seg;
    ring->deq_seg = ring->first_seg;
    ring->enqueue = ring->first_seg->trbs;
    ring->dequeue = ring->first_seg->trbs;
    ring->num_segs = num_segs;
    ring->cycle_state = 1;
    ring->num_trbs_free = num_segs * XHCI_TRBS_PER_SEGMENT - 1;  // -1 for safety
    
    return ring;
}

void xhci_ring_ex_free(xhci_ring_ex_t* ring) {
    if (!ring) return;
    
    if (ring->first_seg) {
        // Break the circular link before freeing
        if (ring->last_seg) {
            ring->last_seg->next = NULL;
        }
        
        xhci_ring_segment_t* seg = ring->first_seg;
        while (seg) {
            xhci_ring_segment_t* next = seg->next;
            xhci_segment_free(seg);
            seg = next;
        }
    }
    
    kfree_dma(ring);
}

// Expand ring by adding new segments after the current enqueue segment
// This can be called when ring is running low on space
int xhci_ring_expansion(xhci_ring_ex_t* ring, uint32_t num_new_segs) {
    if (!ring || num_new_segs == 0) return ST_INVALID;
    if (ring->num_segs + num_new_segs > XHCI_MAX_RING_SEGMENTS) {
        num_new_segs = XHCI_MAX_RING_SEGMENTS - ring->num_segs;
        if (num_new_segs == 0) return ST_NOMEM;
    }
    
    // Allocate new segments
    xhci_ring_segment_t* new_first = NULL;
    xhci_ring_segment_t* new_last = NULL;
    xhci_ring_segment_t* prev = NULL;
    
    for (uint32_t i = 0; i < num_new_segs; i++) {
        xhci_ring_segment_t* seg = xhci_segment_alloc();
        if (!seg) {
            // Cleanup on failure
            xhci_ring_segment_t* s = new_first;
            while (s) {
                xhci_ring_segment_t* next = s->next;
                xhci_segment_free(s);
                s = next;
            }
            return ST_NOMEM;
        }
        seg->num = ring->num_segs + i;
        
        if (!new_first) new_first = seg;
        if (prev) {
            prev->next = seg;
            xhci_set_link_trb(prev, 0);
        }
        prev = seg;
        new_last = seg;
    }
    
    // Insert new segments after enq_seg
    // The new_last->next points to where enq_seg->next was pointing
    xhci_ring_segment_t* old_next = ring->enq_seg->next;
    
    // Link new segments into the ring
    ring->enq_seg->next = new_first;
    xhci_set_link_trb(ring->enq_seg, 0);
    
    new_last->next = old_next;
    xhci_set_link_trb(new_last, 0);
    
    // Update last_seg if we inserted after it
    if (ring->enq_seg == ring->last_seg) {
        // Move toggle cycle bit to new last segment
        ring->last_seg->trbs[XHCI_RING_SIZE - 1].control &= ~TRB_FLAG_TC;
        ring->last_seg = new_last;
        ring->last_seg->trbs[XHCI_RING_SIZE - 1].control |= TRB_FLAG_TC;
    }
    
    ring->num_segs += num_new_segs;
    ring->num_trbs_free += num_new_segs * XHCI_TRBS_PER_SEGMENT;
    
    xhci_dbg("Ring expanded: now %d segments, %d TRBs free\n", 
             ring->num_segs, ring->num_trbs_free);
    
    return ST_OK;
}

// Get number of free TRBs in expandable ring
uint32_t xhci_ring_ex_num_trbs_free(xhci_ring_ex_t* ring) {
    if (!ring) return 0;
    return ring->num_trbs_free;
}

// Check if a TRB is a link TRB
static inline int trb_is_link(xhci_trb_t* trb) {
    return ((trb->control >> 10) & 0x3F) == TRB_TYPE_LINK;
}

// Check if TRB is last on segment
static inline int last_trb_on_seg(xhci_ring_segment_t* seg, xhci_trb_t* trb) {
    return trb == &seg->trbs[XHCI_RING_SIZE - 1];
}

// Enqueue a TRB to expandable ring
int xhci_ring_ex_enqueue(xhci_ring_ex_t* ring, uint64_t param, uint32_t status, uint32_t control) {
    if (!ring || ring->num_trbs_free == 0) return -1;
    
    // Check if we're at the link TRB - need to follow it
    while (trb_is_link(ring->enqueue)) {
        // Update link TRB with current cycle
        uint32_t link_ctrl = ring->enqueue->control;
        link_ctrl = (link_ctrl & ~TRB_FLAG_CYCLE) | (ring->cycle_state ? TRB_FLAG_CYCLE : 0);
        ring->enqueue->control = link_ctrl;
        xhci_mb();
        
        // Toggle cycle if link has TC bit
        if (link_ctrl & TRB_FLAG_TC) {
            ring->cycle_state ^= 1;
        }
        
        // Move to next segment
        ring->enq_seg = ring->enq_seg->next;
        ring->enqueue = ring->enq_seg->trbs;
    }
    
    // Write the TRB
    ring->enqueue->param = param;
    ring->enqueue->status = status;
    ring->enqueue->control = (control & ~TRB_FLAG_CYCLE) | (ring->cycle_state ? TRB_FLAG_CYCLE : 0);
    xhci_mb();
    
    // Advance enqueue pointer
    ring->enqueue++;
    ring->num_trbs_free--;
    
    // Check if we landed on link TRB - handle it on next enqueue
    
    return 0;
}

//=============================================================================
// Scatter-Gather List Support
//=============================================================================

// Initialize a scatter-gather list
void xhci_sg_init(xhci_sg_list_t* sg) {
    if (!sg) return;
    xhci_memset(sg, 0, sizeof(xhci_sg_list_t));
}

// Add an entry to scatter-gather list
int xhci_sg_add(xhci_sg_list_t* sg, uint64_t phys_addr, uint32_t length) {
    if (!sg || sg->num_entries >= XHCI_MAX_SG_ENTRIES) return ST_NOMEM;
    if (length == 0) return ST_OK;  // Skip empty entries
    
    xhci_sg_entry_t* entry = &sg->entries[sg->num_entries];
    entry->phys_addr = phys_addr;
    entry->length = length;
    
    sg->num_entries++;
    sg->total_len += length;
    
    return ST_OK;
}

// Count number of TRBs needed for scatter-gather list
// Accounts for 64KB boundary crossings within each segment
uint32_t xhci_sg_count_trbs(xhci_sg_list_t* sg) {
    if (!sg) return 0;
    
    uint32_t num_trbs = 0;
    for (uint32_t i = 0; i < sg->num_entries; i++) {
        uint64_t addr = sg->entries[i].phys_addr;
        uint32_t remaining = sg->entries[i].length;
        
        while (remaining > 0) {
            uint32_t trb_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
            if (trb_len > remaining) trb_len = remaining;
            if (trb_len == 0) trb_len = remaining;  // Safety
            
            num_trbs++;
            addr += trb_len;
            remaining -= trb_len;
        }
    }
    
    return num_trbs > 0 ? num_trbs : 1;  // At least 1 for zero-length
}

// Queue TRBs for scatter-gather list (internal helper)
static int xhci_queue_sg_trbs(xhci_ring_t* ring, xhci_sg_list_t* sg, int is_in, uint16_t max_pkt) {
    if (!ring || !sg) return 0;
    
    int num_trbs = 0;
    uint32_t total_len = sg->total_len;
    uint32_t transferred = 0;
    
    // Handle zero-length transfer
    if (sg->num_entries == 0 || total_len == 0) {
        uint32_t control = (TRB_TYPE_NORMAL << 10) | TRB_FLAG_IOC;
        if (is_in) control |= TRB_FLAG_ISP;
        xhci_ring_enqueue(ring, 0, 0, control);
        return 1;
    }
    
    // Process each scatter-gather entry
    for (uint32_t i = 0; i < sg->num_entries; i++) {
        uint64_t addr = sg->entries[i].phys_addr;
        uint32_t remaining = sg->entries[i].length;
        
        while (remaining > 0) {
            // Calculate bytes until 64KB boundary
            uint32_t trb_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
            if (trb_len > remaining) trb_len = remaining;
            if (trb_len == 0) trb_len = remaining;  // Safety
            
            uint32_t control = (TRB_TYPE_NORMAL << 10);
            
            // Calculate if more TRBs coming
            uint32_t bytes_after = (sg->total_len - transferred - trb_len);
            int more_trbs = (bytes_after > 0);
            
            if (more_trbs) {
                control |= TRB_FLAG_CHAIN;
            } else {
                control |= TRB_FLAG_IOC;
            }
            
            if (is_in) {
                control |= TRB_FLAG_ISP;
            }
            
            // Calculate TD_SIZE
            uint32_t td_size = 0;
            if (more_trbs && max_pkt > 0) {
                uint32_t total_pkts = (total_len + max_pkt - 1) / max_pkt;
                uint32_t pkts_done = (transferred + trb_len) / max_pkt;
                td_size = total_pkts > pkts_done ? (total_pkts - pkts_done) : 0;
                if (td_size > 31) td_size = 31;
            }
            
            uint32_t status = TRB_LEN(trb_len) | TRB_TD_SIZE(td_size) | TRB_INTR_TARGET(0);
            xhci_ring_enqueue(ring, addr, status, control);
            
            addr += trb_len;
            remaining -= trb_len;
            transferred += trb_len;
            num_trbs++;
        }
    }
    
    return num_trbs;
}

//=============================================================================
// Extended Capabilities and BIOS Handoff
//=============================================================================

// Find an extended capability by ID, starting from a given offset
// Returns offset of capability or 0 if not found
uint32_t xhci_find_ext_cap(xhci_controller_t* ctrl, uint32_t cap_id, uint32_t start_offset) {
    uint32_t offset;
    uint32_t val;
    
    // If no start offset, begin from HCCPARAMS1 XECP pointer
    if (start_offset == 0) {
        uint32_t hccparams1 = xhci_cap_read32(ctrl, XHCI_CAP_HCCPARAMS1);
        offset = ((hccparams1 & XHCI_HCC_XECP_MASK) >> XHCI_HCC_XECP_SHIFT) << 2;
    } else {
        // Read next pointer from previous capability
        val = *(volatile uint32_t*)(ctrl->base + start_offset);
        offset = start_offset + (((val & XHCI_EXT_CAP_NEXT_MASK) >> XHCI_EXT_CAP_NEXT_SHIFT) << 2);
    }
    
    if (offset == 0) return 0;
    
    // Search for capability
    while (offset) {
        val = *(volatile uint32_t*)(ctrl->base + offset);
        if ((val & XHCI_EXT_CAP_ID_MASK) == cap_id) {
            return offset;
        }
        
        // Get next capability offset
        uint32_t next = ((val & XHCI_EXT_CAP_NEXT_MASK) >> XHCI_EXT_CAP_NEXT_SHIFT) << 2;
        if (next == 0) break;
        offset += next;
    }
    
    return 0;
}

// Take ownership of xHCI controller from BIOS
// This is critical for controllers that were being used by BIOS USB support
int xhci_bios_handoff(xhci_controller_t* ctrl) {
    uint32_t offset;
    uint32_t val;
    int timeout;
    
    // Find USB Legacy Support extended capability
    offset = xhci_find_ext_cap(ctrl, XHCI_EXT_CAP_LEGACY, 0);
    if (offset == 0) {
        xhci_dbg("No USB Legacy Support capability found\n");
        return ST_OK;  // No legacy support, nothing to do
    }
    
    xhci_dbg("Found USB Legacy Support capability at offset 0x%x\n", offset);
    
    // Read USBLEGSUP register
    val = *(volatile uint32_t*)(ctrl->base + offset);
    
    // Check if BIOS owns the controller
    if (!(val & XHCI_USBLEGSUP_BIOS_OWNED)) {
        xhci_dbg("BIOS does not own xHCI controller\n");
        return ST_OK;
    }
    
    kprintf("[XHCI] Requesting ownership from BIOS...\n");
    
    // Request ownership by setting OS Owned bit
    val |= XHCI_USBLEGSUP_OS_OWNED;
    *(volatile uint32_t*)(ctrl->base + offset) = val;
    
    // Wait for BIOS to release ownership (up to 1 second)
    timeout = XHCI_LEGACY_TIMEOUT_USEC / 10;  // 10us increments
    while (timeout > 0) {
        val = *(volatile uint32_t*)(ctrl->base + offset);
        if ((val & XHCI_USBLEGSUP_OS_OWNED) && !(val & XHCI_USBLEGSUP_BIOS_OWNED)) {
            break;
        }
        delay_us(10);
        timeout--;
    }
    
    if (timeout <= 0) {
        kprintf("[XHCI] WARNING: BIOS handoff timeout, forcing ownership\n");
        // Force ownership anyway
        val = XHCI_USBLEGSUP_OS_OWNED;
        *(volatile uint32_t*)(ctrl->base + offset) = val;
    } else {
        kprintf("[XHCI] BIOS handoff successful\n");
    }
    
    // Disable SMI events on the controller (USBLEGCTLSTS register at offset + 4)
    // This prevents BIOS SMI handlers from interfering with USB operation
    uint32_t legctl_offset = offset + 4;
    val = *(volatile uint32_t*)(ctrl->base + legctl_offset);
    
    // Clear all SMI enable bits and status bits
    // Bits 31:29 are SMI enables, clear them; bits 28:16 are write-1-to-clear status
    val &= ~XHCI_USBLEGCTLSTS_DISABLE_SMI;  // Clear SMI enables
    val |= 0x00FF0000;  // Clear all status bits by writing 1
    *(volatile uint32_t*)(ctrl->base + legctl_offset) = val;
    
    return ST_OK;
}

// Disable interrupts and prepare for halt
void xhci_quiesce(xhci_controller_t* ctrl) {
    uint32_t cmd;
    uint32_t halted;
    uint32_t mask;
    
    // Disable interrupts
    mask = ~(XHCI_CMD_INTE | XHCI_CMD_HSEE);
    halted = xhci_op_read32(ctrl, XHCI_OP_USBSTS) & XHCI_STS_HCH;
    
    if (!halted) {
        mask &= ~XHCI_CMD_RUN;
    }
    
    cmd = xhci_op_read32(ctrl, XHCI_OP_USBCMD);
    cmd &= mask;
    xhci_op_write32(ctrl, XHCI_OP_USBCMD, cmd);
}

// Halt the controller
int xhci_halt(xhci_controller_t* ctrl) {
    xhci_dbg("Halting controller\n");
    
    // Quiesce first
    xhci_quiesce(ctrl);
    
    // Wait for halt
    for (int i = 0; i < XHCI_MAX_HALT_USEC / 100; i++) {
        if (xhci_op_read32(ctrl, XHCI_OP_USBSTS) & XHCI_STS_HCH) {
            ctrl->running = 0;
            xhci_dbg("Controller halted\n");
            return ST_OK;
        }
        delay_us(100);
    }
    
    kprintf("[XHCI] WARNING: Halt timeout\n");
    return ST_TIMEOUT;
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
    
    // Read and cache capability registers
    uint32_t cap_base = xhci_cap_read32(ctrl, XHCI_CAP_CAPLENGTH);
    uint8_t cap_len = cap_base & 0xFF;
    ctrl->hci_version = (cap_base >> 16) & 0xFFFF;
    
    ctrl->hcs_params1 = xhci_cap_read32(ctrl, XHCI_CAP_HCSPARAMS1);
    ctrl->hcs_params2 = xhci_cap_read32(ctrl, XHCI_CAP_HCSPARAMS2);
    ctrl->hcs_params3 = xhci_cap_read32(ctrl, XHCI_CAP_HCSPARAMS3);
    ctrl->hcc_params1 = xhci_cap_read32(ctrl, XHCI_CAP_HCCPARAMS1);
    ctrl->hcc_params2 = xhci_cap_read32(ctrl, XHCI_CAP_HCCPARAMS2);
    
    uint32_t dboff = xhci_cap_read32(ctrl, XHCI_CAP_DBOFF) & ~0x3;
    uint32_t rtsoff = xhci_cap_read32(ctrl, XHCI_CAP_RTSOFF) & ~0x1F;
    
    // Extract parameters from cached registers
    ctrl->max_slots = ctrl->hcs_params1 & 0xFF;
    ctrl->max_ports = (ctrl->hcs_params1 >> 24) & 0xFF;
    ctrl->max_intrs = (ctrl->hcs_params1 >> 8) & 0x7FF;
    ctrl->context_size = (ctrl->hcc_params1 & XHCI_HCC_CSZ) ? 64 : 32;
    ctrl->num_scratchpads = ((ctrl->hcs_params2 >> 21) & 0x1F) | (((ctrl->hcs_params2 >> 27) & 0x1F) << 5);
    
    // Calculate register base addresses
    ctrl->op_base = ctrl->base + cap_len;
    ctrl->db_base = ctrl->base + dboff;
    ctrl->rt_base = ctrl->base + rtsoff;
    
    // Calculate extended capabilities base
    uint32_t xecp = ((ctrl->hcc_params1 & XHCI_HCC_XECP_MASK) >> XHCI_HCC_XECP_SHIFT) << 2;
    ctrl->ext_caps_base = xecp ? (ctrl->base + xecp) : 0;
    
    kprintf("[XHCI] xHCI version %x.%02x at 0x%llx\n", 
            ctrl->hci_version >> 8, ctrl->hci_version & 0xFF, ctrl->base);
    kprintf("[XHCI] MaxSlots=%d, MaxPorts=%d, CtxSize=%d, Scratchpads=%d\n",
            ctrl->max_slots, ctrl->max_ports, ctrl->context_size, ctrl->num_scratchpads);
    
    xhci_dbg("Base=0x%llx, OpBase=0x%llx, DBBase=0x%llx, RTBase=0x%llx\n",
             ctrl->base, ctrl->op_base, ctrl->db_base, ctrl->rt_base);
    xhci_dbg("MaxSlots=%d, MaxPorts=%d, CtxSize=%d, Scratchpads=%d\n",
             ctrl->max_slots, ctrl->max_ports, ctrl->context_size, ctrl->num_scratchpads);
    
    // Limit slots to our maximum
    if (ctrl->max_slots > XHCI_MAX_SLOTS) ctrl->max_slots = XHCI_MAX_SLOTS;
    if (ctrl->max_ports > XHCI_MAX_PORTS) ctrl->max_ports = XHCI_MAX_PORTS;
    
    // Take ownership from BIOS
    if (ctrl->ext_caps_base) {
        xhci_bios_handoff(ctrl);
    }
    
    // Halt controller first before reset
    if (xhci_halt(ctrl) != ST_OK) {
        kprintf("[XHCI] Warning: Controller halt timeout, continuing anyway\n");
    }
    
    // Reset controller
    if (xhci_reset(ctrl) != ST_OK) {
        kprintf("[XHCI] Reset failed\n");
        return ST_ERR;
    }
    
    // Allocate DCBAA (Device Context Base Address Array) - DMA-safe
    ctrl->dcbaa = (uint64_t*)kcalloc_dma(1, (ctrl->max_slots + 1) * sizeof(uint64_t) + 64);
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
    
    // Allocate input context - DMA-safe
    ctrl->input_ctx = (xhci_input_ctx_t*)kcalloc_dma(1, sizeof(xhci_input_ctx_t) + 64);
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
    
    // Allocate scratchpad array (DMA-safe for low physical addresses)
    ctrl->scratchpad_array = (uint64_t*)kcalloc_dma(ctrl->num_scratchpads, sizeof(uint64_t));
    ctrl->scratchpad_pages = (void**)kcalloc_dma(ctrl->num_scratchpads, sizeof(void*));
    
    if (!ctrl->scratchpad_array || !ctrl->scratchpad_pages) {
        kprintf("[XHCI] Failed to allocate scratchpad\n");
        return;
    }
    
    // Allocate pages for scratchpad (must be page-aligned for DMA)
    for (uint16_t i = 0; i < ctrl->num_scratchpads; i++) {
        // Allocate extra for alignment, DMA-safe for low physical addresses
        void* raw = kcalloc_dma(1, PAGE_SIZE + PAGE_SIZE);
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
    
    // Allocate ERST (Event Ring Segment Table) - DMA-safe
    ctrl->erst = (xhci_erst_entry_t*)kcalloc_dma(1, sizeof(xhci_erst_entry_t) * 2 + 64);
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

int xhci_power_ports(xhci_controller_t* ctrl) {
    if (!ctrl) return ST_INVALID;
    
    // Power up all ports
    for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
        uint32_t off = XHCI_OP_PORTSC_BASE + (port - 1) * 0x10;
        uint32_t portsc = xhci_op_read32(ctrl, off);
        
        if (!(portsc & XHCI_PORTSC_PP)) {
            // Set Port Power (PP) bit
            xhci_op_write32(ctrl, off, XHCI_PORTSC_PP);
        } else if (portsc & XHCI_PORTSC_WPR_MASK) {
            // Clear any pending change bits
            xhci_op_write32(ctrl, off, (portsc & XHCI_PORTSC_PP) | XHCI_PORTSC_WPR_MASK);
        }
    }
    
    // Wait for port power stabilization (USB spec: 100ms, use 200ms for compatibility)
    delay_ms(200);
    
    // Clear any port status change bits that appeared during power-up
    for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
        uint32_t off = XHCI_OP_PORTSC_BASE + (port - 1) * 0x10;
        uint32_t portsc = xhci_op_read32(ctrl, off);
        
        if (portsc & XHCI_PORTSC_WPR_MASK) {
            xhci_op_write32(ctrl, off, (portsc & (XHCI_PORTSC_PP | XHCI_PORTSC_PED)) | XHCI_PORTSC_WPR_MASK);
        }
    }
    
    return ST_OK;
}

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
    
    // Allocate device context - DMA-safe for low physical addresses
    xhci_dev_ctx_t* dev_ctx = (xhci_dev_ctx_t*)kcalloc_dma(1, sizeof(xhci_dev_ctx_t) + 64);
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
    // Uses legacy heap for low physical addresses required by XHCI DMA
    uint8_t* raw_buf = (uint8_t*)kcalloc_dma(1, 4096 + 4096);
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
    kfree_dma(raw_buf);
    
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
// Endpoint Reset (for error recovery)
//=============================================================================

int xhci_reset_endpoint(xhci_controller_t* ctrl, uint8_t slot, uint8_t dci) {
    if (slot == 0 || slot > ctrl->max_slots) return ST_INVALID;
    if (dci == 0 || dci > 31) return ST_INVALID;
    
    xhci_dbg("Resetting endpoint: slot=%d, dci=%d\n", slot, dci);
    
    // Get the ring for this endpoint
    usb_device_t* dev = &ctrl->devices[slot - 1];
    xhci_ring_t* ring = NULL;
    
    if (dci == dev->bulk_in_ep * 2 + 1) {
        ring = dev->bulk_in_ring;
    } else if (dci == dev->bulk_out_ep * 2) {
        ring = dev->bulk_out_ring;
    }
    
    // Step 1: Queue Reset Endpoint command
    uint32_t reset_ctrl = (TRB_TYPE_RESET_EP << 10) | (slot << 24) | (dci << 16);
    
    if (xhci_send_command(ctrl, 0, 0, reset_ctrl) != ST_OK) {
        kprintf("[XHCI] Failed to send Reset EP command\n");
        return ST_ERR;
    }
    
    if (xhci_wait_command(ctrl, 1000) != ST_OK) {
        kprintf("[XHCI] Reset EP command timeout\n");
        return ST_TIMEOUT;
    }
    
    // Step 2: Queue Set TR Dequeue Pointer command to reset ring position
    if (ring) {
        // Calculate the dequeue pointer (point to current enqueue position)
        uint64_t deq_ptr = mm_get_physical_address((uint64_t)&ring->trbs[ring->enqueue]);
        deq_ptr |= ring->cycle;  // Include current cycle state (DCS)
        
        uint32_t set_deq_ctrl = (TRB_TYPE_SET_TR_DEQ << 10) | (slot << 24) | (dci << 16);
        
        if (xhci_send_command(ctrl, deq_ptr, 0, set_deq_ctrl) != ST_OK) {
            kprintf("[XHCI] Failed to send Set TR Dequeue command\n");
            return ST_ERR;
        }
        
        if (xhci_wait_command(ctrl, 1000) != ST_OK) {
            kprintf("[XHCI] Set TR Dequeue command timeout\n");
            return ST_TIMEOUT;
        }
    }
    
    xhci_dbg("Endpoint reset complete: slot=%d, dci=%d\n", slot, dci);
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
// Bulk Transfers (with 64KB boundary handling and retry logic)
//=============================================================================

// Helper: Count number of TRBs needed for a transfer respecting 64KB boundaries
static int xhci_count_trbs(uint64_t addr, uint32_t len) {
    int num_trbs = 0;
    while (len > 0) {
        // Calculate max bytes until 64KB boundary
        uint32_t trb_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
        if (trb_len > len) trb_len = len;
        if (trb_len == 0) trb_len = len;  // Safety for aligned addresses
        
        num_trbs++;
        addr += trb_len;
        len -= trb_len;
    }
    return num_trbs ? num_trbs : 1;  // At least 1 TRB for zero-length
}

// Calculate TD_SIZE (packets remaining) for xHCI 1.0+
// Returns number of packets remaining after this TRB, capped at 31
static uint32_t xhci_td_remainder(uint32_t transferred, uint32_t trb_len, 
                                   uint32_t td_total_len, uint16_t max_pkt) {
    uint32_t total_packets, packets_transferred;
    
    // Zero length or last TRB
    if (trb_len == 0 || transferred + trb_len >= td_total_len) {
        return 0;
    }
    
    // Total packets in TD
    total_packets = (td_total_len + max_pkt - 1) / max_pkt;
    
    // Packets transferred after this TRB
    packets_transferred = (transferred + trb_len) / max_pkt;
    
    // Remaining packets (capped at 31 for 5-bit field)
    uint32_t remaining = total_packets > packets_transferred ? 
                         total_packets - packets_transferred : 0;
    return remaining > 31 ? 31 : remaining;
}

// Helper: Enqueue multiple chained TRBs for a bulk transfer with TD_SIZE
// Returns number of TRBs enqueued
static int xhci_queue_bulk_trbs_ex(xhci_ring_t* ring, uint64_t buf_phys, 
                                    uint32_t len, int is_in, uint16_t max_pkt) {
    int num_trbs = 0;
    uint32_t remaining = len;
    uint32_t transferred = 0;
    uint64_t addr = buf_phys;
    
    // Handle zero-length transfer
    if (len == 0) {
        uint32_t status = TRB_LEN(0) | TRB_TD_SIZE(0);
        uint32_t control = (TRB_TYPE_NORMAL << 10) | TRB_FLAG_IOC;
        if (is_in) control |= TRB_FLAG_ISP;
        xhci_ring_enqueue(ring, 0, status, control);
        return 1;
    }
    
    while (remaining > 0) {
        // Calculate max bytes until 64KB boundary
        uint32_t trb_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
        if (trb_len > remaining) trb_len = remaining;
        if (trb_len == 0) trb_len = remaining;  // Safety
        
        // Calculate TD_SIZE for this TRB
        uint32_t td_size = xhci_td_remainder(transferred, trb_len, len, max_pkt);
        uint32_t status = TRB_LEN(trb_len) | TRB_TD_SIZE(td_size);
        
        uint32_t control = (TRB_TYPE_NORMAL << 10);
        
        // Chain all but the last TRB
        if (remaining > trb_len) {
            control |= TRB_FLAG_CHAIN;
        } else {
            // Last TRB gets IOC (Interrupt On Completion)
            control |= TRB_FLAG_IOC;
        }
        
        // For IN endpoints, set ISP (Interrupt on Short Packet)
        if (is_in) {
            control |= TRB_FLAG_ISP;
        }
        
        xhci_ring_enqueue(ring, addr, status, control);
        
        addr += trb_len;
        transferred += trb_len;
        remaining -= trb_len;
        num_trbs++;
    }
    
    return num_trbs;
}

int xhci_bulk_transfer_in(xhci_controller_t* ctrl, usb_device_t* dev,
                          void* buf, uint32_t len, uint32_t* transferred) {
    if (!dev || !dev->bulk_in_ring || !dev->bulk_in_ep) return ST_INVALID;
    
    xhci_ring_t* ring = dev->bulk_in_ring;
    uint8_t slot = dev->slot_id;
    uint8_t dci = dev->bulk_in_ep * 2 + 1;  // IN endpoint DCI
    uint16_t max_pkt = dev->bulk_in_max_pkt ? dev->bulk_in_max_pkt : 512;
    
    // Pre-calculate physical address BEFORE enqueueing
    uint64_t buf_phys = mm_get_physical_address((uint64_t)buf);
    
    int num_trbs = xhci_count_trbs(buf_phys, len);
    xhci_dbg("Bulk IN: slot=%d, dci=%d, len=%d, trbs=%d, ring enq=%d\n", 
             slot, dci, len, num_trbs, ring->enqueue);
    
    xhci_transfer_t xfer = {0, 0, 0};
    ctrl->pending_xfer[slot - 1][dci] = &xfer;
    
    // Enqueue TRBs with 64KB boundary handling and TD_SIZE
    xhci_queue_bulk_trbs_ex(ring, buf_phys, len, 1, max_pkt);
    
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
                dev->err_count_in = 0;  // Reset error count on success
                return ST_OK;
            }
            
            // Handle transaction errors with retry
            if (xfer.cc == TRB_CC_USB_XACT && dev->err_count_in < MAX_SOFT_RETRY) {
                dev->err_count_in++;
                xhci_dbg("Bulk IN retry %d after CC=%d\n", dev->err_count_in, xfer.cc);
                
                // Reset transfer state and retry
                xfer.completed = 0;
                xfer.cc = 0;
                xfer.bytes_transferred = 0;
                ctrl->pending_xfer[slot - 1][dci] = &xfer;
                
                // Re-enqueue and ring doorbell
                xhci_queue_bulk_trbs_ex(ring, buf_phys, len, 1, max_pkt);
                xhci_ring_doorbell(ctrl, slot, dci);
                continue;
            }
            
            // Too many errors or non-retryable error - reset endpoint
            if (xfer.cc == TRB_CC_STALL || dev->err_count_in >= MAX_SOFT_RETRY) {
                xhci_dbg("Resetting IN endpoint after CC=%d, err_count=%d\n", 
                         xfer.cc, dev->err_count_in);
                xhci_reset_endpoint(ctrl, slot, dci);
                dev->err_count_in = 0;
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
    uint16_t max_pkt = dev->bulk_out_max_pkt ? dev->bulk_out_max_pkt : 512;
    
    // Pre-calculate physical address BEFORE enqueueing
    uint64_t buf_phys = mm_get_physical_address((uint64_t)buf);
    
    int num_trbs = xhci_count_trbs(buf_phys, len);
    xhci_dbg("Bulk OUT: slot=%d, dci=%d, len=%d, trbs=%d\n", slot, dci, len, num_trbs);
    
    xhci_transfer_t xfer = {0, 0, 0};
    ctrl->pending_xfer[slot - 1][dci] = &xfer;
    
    // Enqueue TRBs with 64KB boundary handling and TD_SIZE
    xhci_queue_bulk_trbs_ex(ring, buf_phys, len, 0, max_pkt);
    
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
                dev->err_count_out = 0;  // Reset error count on success
                return ST_OK;
            }
            
            // Handle transaction errors with retry
            if (xfer.cc == TRB_CC_USB_XACT && dev->err_count_out < MAX_SOFT_RETRY) {
                dev->err_count_out++;
                xhci_dbg("Bulk OUT retry %d after CC=%d\n", dev->err_count_out, xfer.cc);
                
                // Reset transfer state and retry
                xfer.completed = 0;
                xfer.cc = 0;
                xfer.bytes_transferred = 0;
                ctrl->pending_xfer[slot - 1][dci] = &xfer;
                
                // Re-enqueue and ring doorbell
                xhci_queue_bulk_trbs_ex(ring, buf_phys, len, 0, max_pkt);
                xhci_ring_doorbell(ctrl, slot, dci);
                continue;
            }
            
            // Too many errors or stall - reset endpoint
            if (xfer.cc == TRB_CC_STALL || dev->err_count_out >= MAX_SOFT_RETRY) {
                xhci_dbg("Resetting OUT endpoint after CC=%d, err_count=%d\n", 
                         xfer.cc, dev->err_count_out);
                xhci_reset_endpoint(ctrl, slot, dci);
                dev->err_count_out = 0;
            }
            
            return ST_IO;
        }
        
        delay_us(100);  // 100 microsecond delay for ultra-fast response
    }
    
    ctrl->pending_xfer[slot - 1][dci] = NULL;
    return ST_TIMEOUT;
}

//=============================================================================
// Scatter-Gather Bulk Transfers
//=============================================================================

int xhci_bulk_transfer_in_sg(xhci_controller_t* ctrl, usb_device_t* dev,
                             xhci_sg_list_t* sg_list, uint32_t* transferred) {
    if (!dev || !dev->bulk_in_ring || !dev->bulk_in_ep || !sg_list) return ST_INVALID;
    
    xhci_ring_t* ring = dev->bulk_in_ring;
    uint8_t slot = dev->slot_id;
    uint8_t dci = dev->bulk_in_ep * 2 + 1;  // IN endpoint DCI
    uint16_t max_pkt = dev->bulk_in_max_pkt ? dev->bulk_in_max_pkt : 512;
    uint32_t len = sg_list->total_len;
    
    int num_trbs = xhci_sg_count_trbs(sg_list);
    xhci_dbg("Bulk IN SG: slot=%d, dci=%d, entries=%d, len=%d, trbs=%d\n", 
             slot, dci, sg_list->num_entries, len, num_trbs);
    
    xhci_transfer_t xfer = {0, 0, 0};
    ctrl->pending_xfer[slot - 1][dci] = &xfer;
    
    // Enqueue TRBs for all scatter-gather entries
    xhci_queue_sg_trbs(ring, sg_list, 1, max_pkt);
    
    // Ring doorbell
    xhci_ring_doorbell(ctrl, slot, dci);
    
    // Wait for completion
    for (int i = 0; i < 50000; i++) {  // 5 second timeout
        xhci_process_events(ctrl);
        
        if (xfer.completed) {
            ctrl->pending_xfer[slot - 1][dci] = NULL;
            
            __asm__ volatile("mfence" ::: "memory");
            
            if (xfer.cc == TRB_CC_SUCCESS || xfer.cc == TRB_CC_SHORT_PACKET) {
                if (transferred) {
                    *transferred = len - xfer.bytes_transferred;
                }
                dev->err_count_in = 0;
                return ST_OK;
            }
            
            // Handle transaction errors with retry
            if (xfer.cc == TRB_CC_USB_XACT && dev->err_count_in < MAX_SOFT_RETRY) {
                dev->err_count_in++;
                xhci_dbg("Bulk IN SG retry %d after CC=%d\n", dev->err_count_in, xfer.cc);
                
                xfer.completed = 0;
                xfer.cc = 0;
                xfer.bytes_transferred = 0;
                ctrl->pending_xfer[slot - 1][dci] = &xfer;
                
                xhci_queue_sg_trbs(ring, sg_list, 1, max_pkt);
                xhci_ring_doorbell(ctrl, slot, dci);
                continue;
            }
            
            // Too many errors or stall - reset endpoint
            if (xfer.cc == TRB_CC_STALL || dev->err_count_in >= MAX_SOFT_RETRY) {
                xhci_dbg("Resetting IN endpoint after SG transfer CC=%d\n", xfer.cc);
                xhci_reset_endpoint(ctrl, slot, dci);
                dev->err_count_in = 0;
            }
            
            return ST_IO;
        }
        
        delay_us(100);
    }
    
    ctrl->pending_xfer[slot - 1][dci] = NULL;
    return ST_TIMEOUT;
}

int xhci_bulk_transfer_out_sg(xhci_controller_t* ctrl, usb_device_t* dev,
                              xhci_sg_list_t* sg_list, uint32_t* transferred) {
    if (!dev || !dev->bulk_out_ring || !dev->bulk_out_ep || !sg_list) return ST_INVALID;
    
    xhci_ring_t* ring = dev->bulk_out_ring;
    uint8_t slot = dev->slot_id;
    uint8_t dci = dev->bulk_out_ep * 2;  // OUT endpoint DCI
    uint16_t max_pkt = dev->bulk_out_max_pkt ? dev->bulk_out_max_pkt : 512;
    uint32_t len = sg_list->total_len;
    
    int num_trbs = xhci_sg_count_trbs(sg_list);
    xhci_dbg("Bulk OUT SG: slot=%d, dci=%d, entries=%d, len=%d, trbs=%d\n", 
             slot, dci, sg_list->num_entries, len, num_trbs);
    
    xhci_transfer_t xfer = {0, 0, 0};
    ctrl->pending_xfer[slot - 1][dci] = &xfer;
    
    // Enqueue TRBs for all scatter-gather entries
    xhci_queue_sg_trbs(ring, sg_list, 0, max_pkt);
    
    // Ring doorbell
    xhci_ring_doorbell(ctrl, slot, dci);
    
    // Wait for completion
    for (int i = 0; i < 50000; i++) {  // 5 second timeout
        xhci_process_events(ctrl);
        
        if (xfer.completed) {
            ctrl->pending_xfer[slot - 1][dci] = NULL;
            
            if (transferred) {
                *transferred = len - xfer.bytes_transferred;
            }
            
            if (xfer.cc == TRB_CC_SUCCESS) {
                dev->err_count_out = 0;
                return ST_OK;
            }
            
            // Handle transaction errors with retry
            if (xfer.cc == TRB_CC_USB_XACT && dev->err_count_out < MAX_SOFT_RETRY) {
                dev->err_count_out++;
                xhci_dbg("Bulk OUT SG retry %d after CC=%d\n", dev->err_count_out, xfer.cc);
                
                xfer.completed = 0;
                xfer.cc = 0;
                xfer.bytes_transferred = 0;
                ctrl->pending_xfer[slot - 1][dci] = &xfer;
                
                xhci_queue_sg_trbs(ring, sg_list, 0, max_pkt);
                xhci_ring_doorbell(ctrl, slot, dci);
                continue;
            }
            
            // Too many errors or stall - reset endpoint
            if (xfer.cc == TRB_CC_STALL || dev->err_count_out >= MAX_SOFT_RETRY) {
                xhci_dbg("Resetting OUT endpoint after SG transfer CC=%d\n", xfer.cc);
                xhci_reset_endpoint(ctrl, slot, dci);
                dev->err_count_out = 0;
            }
            
            return ST_IO;
        }
        
        delay_us(100);
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
        if (ctrl->dev_ctx[i]) kfree_dma(ctrl->dev_ctx[i]);
        if (ctrl->devices[i].bulk_in_ring) xhci_free_ring(ctrl->devices[i].bulk_in_ring);
        if (ctrl->devices[i].bulk_out_ring) xhci_free_ring(ctrl->devices[i].bulk_out_ring);
    }
    
    ctrl->initialized = 0;
}
