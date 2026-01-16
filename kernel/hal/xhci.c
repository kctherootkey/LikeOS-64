// LikeOS-64 - XHCI early bring-up (polling + optional interrupts)
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/status.h"
#include "../../include/kernel/usb.h"
#include "../../include/kernel/xhci_trb.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/usb_msd.h"

// Global controller instance (single-controller bring-up scenario)
xhci_controller_t g_xhci; // referenced via extern in init.c / interrupt.c

// Debug verbosity controls (set to 1 to re-enable selective logging)
#ifndef XHCI_DEBUG
#define XHCI_DEBUG 0          // general one-off init messages currently suppressed
#endif
#ifndef XHCI_EVT_DEBUG
#define XHCI_EVT_DEBUG 0     // enable verbose transfer event logging
#endif
#ifndef XHCI_BULK_DEBUG
#define XHCI_BULK_DEBUG 0      // enable bulk enqueue logging
#endif
#ifndef XHCI_MSD_DEBUG
/* XHCI_MSD_DEBUG (and XHCI_MSD_LOG) now centrally defined in xhci.h so other modules can reuse.
    Keep a fallback here only if the header was included without its guard (should not happen). */
#define XHCI_MSD_DEBUG 0
#endif

#define XHCI_LOG(fmt, ...)      do { if(XHCI_DEBUG)      kprintf(fmt, ##__VA_ARGS__); } while(0)
#define XHCI_EVT_LOG(fmt, ...)  do { if(XHCI_EVT_DEBUG)  kprintf(fmt, ##__VA_ARGS__); } while(0)
#define XHCI_BULK_LOG(fmt, ...) do { if(XHCI_BULK_DEBUG) kprintf(fmt, ##__VA_ARGS__); } while(0)
/* XHCI_MSD_LOG provided by header (avoid local redefinition) */

// Forward declarations for new control transfer helpers
static void xhci_post_ep0_doorbell(xhci_controller_t* ctrl, unsigned slot_id);
static void xhci_ep0_build_setup(xhci_controller_t* ctrl, xhci_trb_t* ring, uint8_t bm, uint8_t req, uint16_t wValue, uint16_t wIndex, uint16_t wLength, int in_dir);
void xhci_start_enumeration(xhci_controller_t* ctrl, struct usb_device* dev);
void xhci_control_xfer_event(xhci_controller_t* ctrl, struct usb_device* dev, unsigned cc);
void xhci_issue_reset_endpoint(xhci_controller_t* ctrl, usb_device_t* dev, unsigned epid);
void xhci_issue_set_tr_dequeue(xhci_controller_t* ctrl, usb_device_t* dev, unsigned epid, uint64_t ring_phys, unsigned cycle);
static xhci_trb_t* xhci_cmd_enqueue_nodoorbell(xhci_controller_t* ctrl, unsigned int type, unsigned long param);
// Issue Evaluate Context for a subset of endpoints (add_mask should include the endpoint IDs; slot bit added here)
static void xhci_issue_eval_context_endpoints(xhci_controller_t* ctrl, usb_device_t* dev, unsigned add_mask)
{
    if(!ctrl || !dev || !dev->input_ctx || !add_mask) {
        return;
    }
    volatile uint32_t *icc = (volatile uint32_t *)dev->input_ctx;
    /* Drop Context Flags */
    icc[0] = 0;
    /* Add Context Flags (include slot) */
    icc[1] = (add_mask | 0x1u);
    unsigned long phys = (unsigned long)mm_get_physical_address((uint64_t)dev->input_ctx);
    xhci_trb_t *trb = xhci_cmd_enqueue_nodoorbell(ctrl, XHCI_TRB_TYPE_EVAL_CONTEXT, phys);
    if(!trb) {
        return;
    }
    trb->control |= (dev->slot_id & 0xFF) << 24;
    __asm__ __volatile__("mfence":::"memory");
    volatile uint32_t *db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0);
    *db0 = 0;
    XHCI_EVT_LOG("xhci: eval_context (bulk refresh) slot=%u add_mask=0x%08x\n", dev->slot_id, icc[1]);
}

static inline void io_delay(void)
{
    volatile int i;
    for (i = 0; i < 10000; i++)
        ; /* busy wait */
}

void xhci_irq_service(xhci_controller_t *ctrl)
{
#if 1
    if(!ctrl || !ctrl->runtime_base) {
        return;
    }
    volatile uint32_t *iman = (volatile uint32_t *)(ctrl->runtime_base + XHCI_RT_IR0_IMAN);
    uint32_t v = *iman;
    if(v & 0x1) {
        /* Clear interrupt pending (write 1 to IP) while preserving IE */
        *iman = v;
    }
    XHCI_LOG("XHCI: IRQ iman=%08x dequeue=%u cycle=%u\n", v, ctrl->event_ring_dequeue, ctrl->event_ring_cycle);
    /* Process any available events */
    xhci_process_events(ctrl);
    /* Advance ERDP to current dequeue pointer (set EHB=1) */
    if(ctrl->event_ring_virt) {
        volatile uint64_t *erdp = (volatile uint64_t *)(ctrl->runtime_base + XHCI_RT_IR0_ERDP);
        uint64_t newp = ctrl->event_ring_phys + (ctrl->event_ring_dequeue % ctrl->event_ring_size) * sizeof(xhci_trb_t);
        *erdp = newp | 0x1ULL; /* EHB */
        XHCI_LOG("XHCI: IRQ ERDP advanced to %p\n", (void*)newp);
    }
    else {
        /* If event ring missing, poke IMAN to test if write is visible */
        *iman |= 0x2; /* ensure IE stays set */
    }
#endif
}

// Robustly program CRCR with validation
static uint64_t xhci_program_crcr(xhci_controller_t *ctrl)
{
    volatile uint32_t *crcr_lo;
    volatile uint32_t *crcr_hi;
    uint64_t desired;
    uint64_t readback = 0;
    int attempt;

    if (!ctrl)
        return 0;

    crcr_lo = (volatile uint32_t *)(ctrl->op_base + XHCI_OP_CRCR);
    crcr_hi = (volatile uint32_t *)(ctrl->op_base + XHCI_OP_CRCR + 4);
    desired = (ctrl->cmd_ring_phys & ~0x3FULL) | (ctrl->cmd_cycle_state & 1);

    for (attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 0) {
            volatile uint64_t *cr64 = (volatile uint64_t *)(ctrl->op_base + XHCI_OP_CRCR);
            *cr64 = desired;
        }
        __asm__ __volatile__("mfence" ::: "memory");
        {
            uint32_t lo = *crcr_lo;
            uint32_t hi = *crcr_hi;
            readback = ((uint64_t)hi << 32) | lo;
        }
        if ((readback & ~0x3FULL) == (desired & ~0x3FULL)) {
            XHCI_LOG("XHCI: CRCR atomic64 attempt succeeded rb=%p\n", (void *)readback);
            return readback;
        }
    }
    for (attempt = 0; attempt < 4; ++attempt) {
        if (attempt & 1) {
            *crcr_lo = (uint32_t)(desired & 0xFFFFFFFFu);
            *crcr_hi = (uint32_t)(desired >> 32);
        } else {
            *crcr_hi = (uint32_t)(desired >> 32);
            *crcr_lo = (uint32_t)(desired & 0xFFFFFFFFu);
        }
        __asm__ __volatile__("mfence" ::: "memory");
        {
            uint32_t lo = *crcr_lo;
            uint32_t hi = *crcr_hi;
            readback = ((uint64_t)hi << 32) | lo;
        }
        if ((readback & ~0x3FULL) == (desired & ~0x3FULL))
            break;
        io_delay();
    }
    XHCI_LOG("XHCI: CRCR programmed desired=%p rb=%p\n",
        (void *)(((unsigned long)ctrl->cmd_ring_phys & ~0x3FULL) |
        (ctrl->cmd_cycle_state & 1)), (void *)readback);
    return readback;
}

static void xhci_cmd_enqueue(xhci_controller_t *ctrl, unsigned int type,
    unsigned long param)
{
    xhci_trb_t *ring;
    unsigned int idx;
    xhci_trb_t *trb;
    unsigned long trb_phys;
    volatile uint32_t *db0;

    if (!ctrl || !ctrl->cmd_ring_virt)
        return;

    ring = (xhci_trb_t *)ctrl->cmd_ring_virt;
    idx = ctrl->cmd_ring_enqueue % ctrl->cmd_ring_size;
    if (idx == ctrl->cmd_ring_size - 1) {
        ctrl->cmd_ring_enqueue++;
        idx = ctrl->cmd_ring_enqueue % ctrl->cmd_ring_size;
    }
    trb = &ring[idx];
    trb->param_lo = (uint32_t)(param & 0xFFFFFFFFu);
    trb->param_hi = (uint32_t)((param >> 32) & 0xFFFFFFFFu);
    trb->status = 0;
    trb->control = XHCI_TRB_SET_TYPE(type) | (ctrl->cmd_cycle_state & 1);
    ctrl->cmd_ring_enqueue++;
    ctrl->pending_cmd_type = type;
    ctrl->pending_cmd_trb = trb;
    trb_phys = ctrl->cmd_ring_phys +
        ((unsigned long)((uint8_t *)trb - (uint8_t *)ctrl->cmd_ring_virt));
    ctrl->pending_cmd_trb_phys = trb_phys;
    XHCI_EVT_LOG("XHCI: Enqueue CMD type=%u idx=%u trb_virt=%p trb_phys=%p param=%p ctl=%08x\n",
        type, idx, trb, (void *)trb_phys, (void *)param, trb->control);
    __asm__ __volatile__("mfence" ::: "memory");
    db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0x00);
    *db0 = 0;
}

// Build command TRB without ringing the doorbell (used to avoid races when we must set extra bits before doorbell)
static xhci_trb_t *xhci_cmd_enqueue_nodoorbell(xhci_controller_t *ctrl,
    unsigned int type, unsigned long param)
{
    xhci_trb_t *ring;
    unsigned int idx;
    xhci_trb_t *trb;
    unsigned long trb_phys;

    if (!ctrl || !ctrl->cmd_ring_virt)
        return 0;

    ring = (xhci_trb_t *)ctrl->cmd_ring_virt;
    idx = ctrl->cmd_ring_enqueue % ctrl->cmd_ring_size;
    if (idx == ctrl->cmd_ring_size - 1) {
        ctrl->cmd_ring_enqueue++;
        idx = ctrl->cmd_ring_enqueue % ctrl->cmd_ring_size;
    }
    trb = &ring[idx];
    trb->param_lo = (uint32_t)(param & 0xFFFFFFFFu);
    trb->param_hi = (uint32_t)((param >> 32) & 0xFFFFFFFFu);
    trb->status = 0;
    trb->control = XHCI_TRB_SET_TYPE(type) | (ctrl->cmd_cycle_state & 1);
    ctrl->cmd_ring_enqueue++;
    ctrl->pending_cmd_type = type;
    ctrl->pending_cmd_trb = trb;
    trb_phys = ctrl->cmd_ring_phys +
        ((unsigned long)((uint8_t *)trb - (uint8_t *)ctrl->cmd_ring_virt));
    ctrl->pending_cmd_trb_phys = trb_phys;
    XHCI_EVT_LOG("XHCI: Build CMD(noDB) type=%u idx=%u trb_virt=%p trb_phys=%p param=%p ctl=%08x\n",
        type, idx, trb, (void *)trb_phys, (void *)param, trb->control);
    return trb;
}

static unsigned long extract_mmio_base(unsigned int bar_low, unsigned int bar_high)
{
    if((bar_low & 0x04) && bar_high) {
        return ((unsigned long)bar_high << 32) | (bar_low & ~0xFUL);
    }
    return (unsigned long)(bar_low & ~0xFUL);
}

int xhci_init(xhci_controller_t* ctrl, const pci_device_t* dev)
{
    if(!ctrl || !dev) {
        return ST_INVALID;
    }
    ctrl->pci = dev;
    pci_enable_busmaster_mem(dev);
    unsigned int bar0 = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x10);
    unsigned int bar1 = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x14);
    XHCI_LOG("XHCI: BAR0=%08x BAR1=%08x\n", bar0, bar1);
    ctrl->mmio_base = extract_mmio_base(bar0, bar1);
    if((ctrl->mmio_base & 0xFFFFFFFFULL) < 0x1000) {
        return ST_ERR;
    }
    if(ctrl->mmio_base >= 0x100000000ULL) {
        return ST_UNSUPPORTED;
    }
    volatile unsigned char *base8 = (volatile unsigned char *)ctrl->mmio_base;
    ctrl->capability_length = base8[0];
    if(ctrl->capability_length == 0 || ctrl->capability_length == 0xFF) {
        return ST_ERR;
    }
    XHCI_LOG("XHCI: CAP dump @%p\n", (void *)ctrl->mmio_base);
    if(XHCI_DEBUG) {
        for(int off = 0; off < 0x80; off += 16) {
            XHCI_LOG("  %02x: ", off);
            for(int b = 0; b < 16; b++) {
                XHCI_LOG("%02x ", base8[off + b]);
            }
            XHCI_LOG("\n");
        }
    }
    volatile unsigned int *cap32 = (volatile unsigned int *)ctrl->mmio_base;
    ctrl->hcsparams1 = cap32[1];
    ctrl->hcsparams2 = cap32[2];
    ctrl->hcsparams3 = cap32[3];
    ctrl->hccparams1 = cap32[4];
    ctrl->dboff = cap32[5];
    ctrl->rtsoff = cap32[6];
    ctrl->op_base = ctrl->mmio_base + ctrl->capability_length;
    unsigned long candidate_offsets[4];
    int cand_count = 0;
    unsigned long chosen_op = 0;
    unsigned int chosen_usbsts = 0;
    candidate_offsets[cand_count++] = ctrl->capability_length;
    if(ctrl->capability_length != 0x20) {
        candidate_offsets[cand_count++] = 0x20;
    }
    if(ctrl->capability_length != 0x40) {
        candidate_offsets[cand_count++] = 0x40;
    }
    candidate_offsets[cand_count++] = ctrl->capability_length + 0x20;
    XHCI_LOG("XHCI: Probing operational base candidates\n");
    for(int i = 0; i < cand_count; i++) {
        unsigned long test_off = candidate_offsets[i];
        unsigned long test_base = ctrl->mmio_base + test_off;
        volatile unsigned int *test_usbsts = (volatile unsigned int *)(test_base + XHCI_OP_USBSTS);
        volatile unsigned int *test_usbcmd = (volatile unsigned int *)(test_base + XHCI_OP_USBCMD);
        unsigned int v_before = *test_usbsts;
        unsigned int cmdv = *test_usbcmd;
        *test_usbcmd = cmdv & ~1u;
        unsigned int v_after = *test_usbsts; /* retained for debug parity with previous state (unused) */
        (void)v_after;
        XHCI_LOG("  off=0x%lx USBSTS before=%08x\n", test_off, v_before);
        if(!chosen_op && v_before) {
            chosen_op = test_base;
            chosen_usbsts = v_before;
        }
    }
    if(chosen_op && chosen_op != ctrl->op_base) {
        XHCI_LOG("XHCI: Using probed operational base %p (was %p) USBSTS=%08x\n", (void *)chosen_op, (void *)ctrl->op_base, chosen_usbsts);
        ctrl->op_base = chosen_op;
    } else if(chosen_op) {
        XHCI_LOG("XHCI: Using CAPLENGTH-derived operational base %p (USBSTS=%08x)\n", (void *)ctrl->op_base, chosen_usbsts);
    } else {
        XHCI_LOG("XHCI: No responsive operational base detected; using %p\n", (void *)ctrl->op_base);
    }
    ctrl->max_slots = (ctrl->hcsparams1 & 0xFF);
    ctrl->max_ports = (ctrl->hcsparams1 >> 24) & 0xFF;
    XHCI_LOG("XHCI: mmio=%p caplen=%u slots=%u ports=%u hcs1=%08x hcs2=%08x hcs3=%08x hcc1=%08x dboff=0x%x rtsoff=0x%x\n",
        (void *)ctrl->mmio_base, (unsigned)ctrl->capability_length, ctrl->max_slots, ctrl->max_ports, ctrl->hcsparams1, ctrl->hcsparams2, ctrl->hcsparams3, ctrl->hccparams1, ctrl->dboff, ctrl->rtsoff);
    volatile unsigned int *usbcmd = (volatile unsigned int *)(ctrl->op_base + XHCI_OP_USBCMD);
    volatile unsigned int *usbsts = (volatile unsigned int *)(ctrl->op_base + XHCI_OP_USBSTS);
    volatile unsigned int *pagesize = (volatile unsigned int *)(ctrl->op_base + XHCI_OP_PAGESIZE);
    ctrl->cmd_cycle_state = 1;
    ctrl->cmd_ring_size = 16;
    void *cmd_ring_raw = kcalloc(1, 4096 + 64);
    if(!cmd_ring_raw) {
        return ST_NOMEM;
    }
    unsigned long raw_phys = (unsigned long)mm_get_physical_address((uint64_t)cmd_ring_raw);
    unsigned long aligned_virt = ((unsigned long)cmd_ring_raw + 63) & ~63UL;
    unsigned long aligned_phys = raw_phys + (aligned_virt - (unsigned long)cmd_ring_raw);
    if(aligned_phys & 0x3F) {
        return ST_ERR;
    }
    ctrl->cmd_ring_virt = (void *)aligned_virt;
    ctrl->cmd_ring_phys = aligned_phys;
    ctrl->cmd_ring_enqueue = 0;
    ctrl->cmd_ring_stall_ticks = 0;
    xhci_trb_t *cr = (xhci_trb_t *)ctrl->cmd_ring_virt;
    xhci_trb_t *link = &cr[ctrl->cmd_ring_size - 1];
    link->param_lo = (uint32_t)(ctrl->cmd_ring_phys & 0xFFFFFFFFu);
    link->param_hi = (uint32_t)(ctrl->cmd_ring_phys >> 32);
    link->status = 0;
    link->control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_LINK) | (ctrl->cmd_cycle_state & 1) | (1u << 1);
    XHCI_LOG("XHCI: Command ring initial cycle=%u phys=%p\n", ctrl->cmd_cycle_state & 1, (void *)ctrl->cmd_ring_phys);
    /* DCBAA */
    ctrl->dcbaa_virt = kcalloc(ctrl->max_slots + 1, sizeof(unsigned long));
    ctrl->dcbaa_phys = (unsigned long)mm_get_physical_address((uint64_t)ctrl->dcbaa_virt);
    /* Scratchpads */
    unsigned int sp_lo = ctrl->hcsparams2 & 0x1F;
    unsigned int sp_hi = (ctrl->hcsparams2 >> 27) & 0x1F;
    unsigned int scratchpad_count = (sp_hi << 5) | sp_lo;
    if(scratchpad_count) {
        unsigned int array_bytes = scratchpad_count * 8;
        void *spa = kcalloc(1, array_bytes + 64);
        unsigned long spa_aligned = ((unsigned long)spa + 63) & ~63UL;
        unsigned long *entries = (unsigned long *)spa_aligned;
        for(unsigned i = 0; i < scratchpad_count; i++) {
            void *page = kcalloc(1, 4096);
            entries[i] = (unsigned long)mm_get_physical_address((uint64_t)page);
        }
        unsigned long spa_phys = (unsigned long)mm_get_physical_address((uint64_t)entries);
        ((unsigned long *)ctrl->dcbaa_virt)[0] = spa_phys;
        XHCI_LOG("XHCI: scratchpads=%u array_phys=%p\n", scratchpad_count, (void *)spa_phys);
    }
    /* Event ring */
    ctrl->event_ring_size = 16;
    ctrl->event_ring_virt = kcalloc(1, 4096);
    ctrl->event_ring_phys = (unsigned long)mm_get_physical_address((uint64_t)ctrl->event_ring_virt);
    ctrl->event_ring_dequeue = 0;
    ctrl->event_ring_cycle = 1;
    ctrl->erst_virt = kcalloc(1, 16);
    ctrl->erst_phys = (unsigned long)mm_get_physical_address((uint64_t)ctrl->erst_virt);
    uint64_t *erst = (uint64_t *)ctrl->erst_virt;
    erst[0] = ctrl->event_ring_phys;
    ((uint32_t *)erst)[2] = ctrl->event_ring_size;
    ((uint32_t *)erst)[3] = 0;
    /* Halt then reset */
    unsigned int cmd = *usbcmd;
    if(cmd & XHCI_USBCMD_RS) {
        *usbcmd = cmd & ~XHCI_USBCMD_RS;
    }
    for(int t = 0; t < 200000; t++) {
        if((*usbsts & XHCI_USBSTS_HCH)) {
            break;
        }
    }
    *usbcmd |= XHCI_USBCMD_HCRST;
    for(int t = 0; t < 500000; t++) {
        if(!(*usbcmd & XHCI_USBCMD_HCRST)) {
            break;
        }
    }
    *usbsts = *usbsts;
    *pagesize = 0x1;
    for(int t = 0; t < 500000; t++) {
        if(((*usbsts) & (1u << 11)) == 0) {
            break;
        }
    }
    /* Program DCBAAP & CRCR */
    volatile uint32_t *dcbaap_lo = (volatile uint32_t *)(ctrl->op_base + XHCI_OP_DCBAAP);
    volatile uint32_t *dcbaap_hi = (volatile uint32_t *)(ctrl->op_base + XHCI_OP_DCBAAP + 4);
    *dcbaap_lo = (uint32_t)(ctrl->dcbaa_phys & 0xFFFFFFFFu);
    *dcbaap_hi = (uint32_t)(ctrl->dcbaa_phys >> 32);
    uint64_t dcbaa_rb = ((uint64_t)(*dcbaap_hi) << 32) | (*dcbaap_lo);
    XHCI_LOG("XHCI: DCBAAP set to %p rb=%p\n", (void *)ctrl->dcbaa_phys, (void *)dcbaa_rb);
    uint64_t crcr_rb = xhci_program_crcr(ctrl);
    uint64_t desired_crcr = (ctrl->cmd_ring_phys & ~0x3FULL) | (ctrl->cmd_cycle_state & 1);
    if((crcr_rb & ~0x3FULL) != (desired_crcr & ~0x3FULL)) {
        return ST_ERR;
    }
    volatile uint32_t *config_reg = (volatile uint32_t *)(ctrl->op_base + XHCI_OP_CONFIG);
    *config_reg = 1;
    unsigned long masked_rtsoff = (unsigned long)(ctrl->rtsoff & ~0x1Ful);
    unsigned long masked_dboff = (unsigned long)(ctrl->dboff & ~0x3ul);
    ctrl->runtime_base = ctrl->mmio_base + masked_rtsoff;
    ctrl->doorbell_array = ctrl->mmio_base + masked_dboff;
    volatile uint32_t *erstsz = (volatile uint32_t *)(ctrl->runtime_base + XHCI_RT_IR0_ERSTSZ);
    volatile uint64_t *erstba = (volatile uint64_t *)(ctrl->runtime_base + XHCI_RT_IR0_ERSTBA);
    volatile uint64_t *erdp  = (volatile uint64_t *)(ctrl->runtime_base + XHCI_RT_IR0_ERDP);
    *erstsz = 1;
    *erstba = ctrl->erst_phys;
    *erdp = ctrl->event_ring_phys;
    volatile uint32_t *iman = (volatile uint32_t *)(ctrl->runtime_base + XHCI_RT_IR0_IMAN);
    uint32_t iman_val = *iman;
    /* Clear any pending IP (bit0) and set IE (bit1) */
    *iman = (iman_val | 0x2) & ~0x1u; /* write back with IE=1, IP cleared */
    uint32_t iman_rb = *iman;
    XHCI_LOG("XHCI: IMAN programmed val=%08x\n", iman_rb);
    *usbcmd |= XHCI_USBCMD_INTE; /* enable interrupts before run */
    XHCI_LOG("XHCI: USBCMD after INTE=%08x\n", *usbcmd);
    *usbcmd |= XHCI_USBCMD_RS;   /* run */
    XHCI_LOG("XHCI: USBCMD after RS=%08x\n", *usbcmd);
    for(int t = 0; t < 500000; t++) {
        if(!(*usbsts & XHCI_USBSTS_HCH)) {
            break;
        }
    }
    XHCI_LOG("XHCI: controller running USBSTS=%08x\n", *usbsts);
    /* Prime ring with NO-OP (don't track as pending to avoid stale completion confusing state machine) */
    xhci_cmd_enqueue(ctrl, XHCI_TRB_TYPE_NO_OP_CMD, 0);
    XHCI_LOG("XHCI: queued initial NO-OP command (expect silent completion)\n");
    ctrl->last_noop_trb_phys = ctrl->pending_cmd_trb_phys;
    ctrl->pending_cmd_type = 0; /* do not track NO-OP */
    ctrl->pending_cmd_trb = 0;
    ctrl->pending_cmd_trb_phys = 0;
    return ST_OK;
}

int xhci_poll_ports(xhci_controller_t* ctrl)
{
    if(!ctrl) {
        return ST_INVALID;
    }
    static unsigned char prev_ccs[64];
    unsigned int new_devices = 0;
    for(unsigned int p = 0; p < ctrl->max_ports && p < 64; p++) {
        volatile unsigned int *portsc = (volatile unsigned int *)(ctrl->op_base + XHCI_PORT_REG_BASE + p * XHCI_PORT_REG_STRIDE);
        unsigned int v = *portsc;
        int ccs = (v & XHCI_PORTSC_CCS) ? 1 : 0;
        if(ccs && !prev_ccs[p]) {
            volatile unsigned int *portscw = portsc;
            *portscw = v | XHCI_PORTSC_PR;
            for(int t = 0; t < 100000; t++) {
                if(((*portscw) & XHCI_PORTSC_PR) == 0) {
                    break;
                }
            }
            v = *portscw;
            if(!(v & XHCI_PORTSC_PED)) {
                XHCI_LOG("USB: Port %u reset done but PED not set (v=0x%x)\n", p + 1, v);
            } else {
                XHCI_LOG("USB: Port %u reset OK (v=0x%x)\n", p + 1, v);
            }
            usb_device_t *dev = usb_alloc_device(p + 1);
            if(dev && (v & XHCI_PORTSC_PED)) {
                unsigned int sp = (v >> 10) & 0xF;
                switch(sp) {
                case 1: dev->speed = USB_SPEED_FULL; break;
                case 2: dev->speed = USB_SPEED_LOW; break;
                case 3: dev->speed = USB_SPEED_HIGH; break;
                case 4: dev->speed = USB_SPEED_SUPER; break;
                default: dev->speed = USB_SPEED_UNKNOWN; break;
                }
                ctrl->pending_device = dev;
                xhci_cmd_enqueue(ctrl, XHCI_TRB_TYPE_ENABLE_SLOT, 0);
            }
            XHCI_LOG("USB: Device connect on port %u (raw=0x%x)\n", p + 1, v);
            new_devices++;
        }
        prev_ccs[p] = ccs;
    }
    return new_devices ? new_devices : ST_OK;
}

int xhci_process_events(xhci_controller_t *ctrl)
{
    if(!ctrl || !ctrl->event_ring_virt) {
        return ST_INVALID;
    }
    int processed = 0;
    xhci_trb_t *ring = (xhci_trb_t *)ctrl->event_ring_virt;
    for(;;) {
        unsigned int idx = ctrl->event_ring_dequeue % ctrl->event_ring_size;
        xhci_trb_t *trb = &ring[idx];
        unsigned int control = trb->control;
        if(control == 0) {
            break;
        }
        unsigned int cycle = control & 1;
        if(cycle != (ctrl->event_ring_cycle & 1)) {
            break;
        }
        unsigned int type = (control >> 10) & 0x3F;
        if(type == XHCI_TRB_TYPE_COMMAND_COMPLETION) {
            unsigned int status = trb->status;
            unsigned int cc = (status >> 24) & 0xFF; /* completion code */
            const char *cc_name = "Unknown";
            switch(cc) {
            case 1: cc_name = "Success"; break;
            case 5: cc_name = "TRBError"; break;
            case 9: cc_name = "SlotNotEnabled"; break;
            case 19: cc_name = "ContextError"; break; /* tentative label */
            default: break;
            }
            unsigned int slot = (status >> 16) & 0xFF; /* spec-defined slot bits 23:16 */
            unsigned int ctrl_slot = (trb->control >> 24) & 0xFF; /* observed alternative */
            if(slot == 0 && ctrl_slot && (ctrl->pending_cmd_type == XHCI_TRB_TYPE_ENABLE_SLOT || ctrl->pending_cmd_type == XHCI_TRB_TYPE_ADDRESS_DEVICE)) {
                slot = ctrl_slot; /* fallback for non-compliant status field */
            }
            uint64_t cmd_trb_ptr = ((uint64_t)trb->param_hi << 32) | trb->param_lo;
            int ptr_match = (ctrl->pending_cmd_trb && ctrl->pending_cmd_trb_phys == cmd_trb_ptr);
            if(cmd_trb_ptr == ctrl->last_noop_trb_phys && ctrl->pending_cmd_type == 0) {
                /* Silent ignore of initial NO-OP completion */
                goto cmd_done;
            }
            XHCI_EVT_LOG("xhci: cmd cc=%u(%s) slot=%u type=%u match=%c\n", cc, cc_name, slot, ctrl->pending_cmd_type, ptr_match ? 'Y' : 'N');
            if(!ptr_match) {
                XHCI_EVT_LOG("xhci: stale cmd ignored type=%u\n", ctrl->pending_cmd_type);
                goto cmd_done;
            }
            if(cc == 1 && slot == 0 && ctrl->pending_cmd_type == XHCI_TRB_TYPE_ADDRESS_DEVICE && ctrl->pending_device) {
                usb_device_t *dev = (usb_device_t *)ctrl->pending_device;
                if(dev->slot_id) {
                    slot = dev->slot_id;
                    XHCI_LOG("XHCI: WARNING recovered missing slot id -> %u for ADDRESS_DEVICE\n", slot);
                }
            }
            if(ctrl->pending_cmd_type == XHCI_TRB_TYPE_ENABLE_SLOT) {
                if(cc == 1 && slot) {
                    usb_device_t *dev = (usb_device_t *)ctrl->pending_device;
                    dev->slot_id = slot;
                    if(slot < 64) {
                        ctrl->slot_device_map[slot] = dev;
                    }
                    XHCI_LOG("xhci: enable_slot slot=%u port=%u\n", slot, dev->port_number);
                    /* Allocate and 64-byte align Input and Device Contexts (spec requires 64B alignment) */
                    void *in_raw = kcalloc(1, 4096 + 64);
                    void *dc_raw = kcalloc(1, 4096 + 64);
                    if(in_raw) {
                        unsigned long in_aligned = (((unsigned long)in_raw) + 63) & ~63UL;
                        dev->input_ctx = (void *)in_aligned;
                    } else {
                        dev->input_ctx = 0;
                    }
                    if(dc_raw) {
                        unsigned long dc_aligned = (((unsigned long)dc_raw) + 63) & ~63UL;
                        dev->device_ctx = (void *)dc_aligned;
                    } else {
                        dev->device_ctx = 0;
                    }
                    if(dev->input_ctx && dev->device_ctx) {
                        /* Ensure aligned regions fully zeroed (alignment offset may skip initial zeroed bytes) */
                        for(unsigned i = 0; i < 4096 / 8; i++) {
                            ((volatile uint64_t *)dev->input_ctx)[i] = 0;
                        }
                        for(unsigned i = 0; i < 4096 / 8; i++) {
                            ((volatile uint64_t *)dev->device_ctx)[i] = 0;
                        }
                        unsigned long in_phys = (unsigned long)mm_get_physical_address((uint64_t)dev->input_ctx);
                        unsigned long dc_phys = (unsigned long)mm_get_physical_address((uint64_t)dev->device_ctx);
                        if((in_phys & 0x3F) || (dc_phys & 0x3F)) {
                            XHCI_LOG("XHCI: WARN context phys alignment in=%p dev=%p\n", (void *)in_phys, (void *)dc_phys);
                        } else {
                            XHCI_LOG("xhci: ctx aligned in=%p dev=%p\n", (void *)in_phys, (void *)dc_phys);
                        }
                    }
                    if(!dev->input_ctx || !dev->device_ctx) {
                        XHCI_LOG("XHCI: context alloc failed slot %u\n", slot);
                        ctrl->pending_cmd_type = 0; /* drop pending, we won't send ADDRESS_DEVICE */
                        ctrl->pending_device = 0;
                        ctrl->pending_cmd_trb = 0;
                        ctrl->pending_cmd_trb_phys = 0;
                    } else {
                        volatile uint32_t *icc = (volatile uint32_t *)dev->input_ctx; /* Input Control Context */
                        icc[0] = 0x0;       /* Drop Context Flags (none) */
                        icc[1] = 0x3;       /* Add Context Flags: bit0=slot, bit1=ep0 */
                        unsigned speed_code = 0;
                        switch(dev->speed) {
                        case USB_SPEED_LOW: speed_code = 2; break;
                        case USB_SPEED_FULL: speed_code = 1; break;
                        case USB_SPEED_HIGH: speed_code = 3; break;
                        case USB_SPEED_SUPER: speed_code = 4; break;
                        default: break;
                        }
                        unsigned ctx_stride = (ctrl->hccparams1 & (1u << 2)) ? 64u : 32u; /* CSZ bit */
                        XHCI_EVT_LOG("xhci: ctx_stride=%u (hcc1=%08x)\n", ctx_stride, ctrl->hccparams1);
                        volatile uint32_t *slot_ctx = (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride); /* Slot Context */
                        /* Dword0: RouteString=0, Speed(23:20), Context Entries (31:27)=1 (only EP0 present) */
                        slot_ctx[0] = ((1u & 0x1F) << 27) | ((speed_code & 0xF) << 20);
                        /* Dword1: Root Hub Port Number (23:16) */
                        slot_ctx[1] = (dev->port_number & 0xFF) << 16;
                        /* Initialize remaining required Slot Context dwords (minimal) */
                        slot_ctx[2] = 0; /* Interrupter Target (31:22)=0, TTT=0, etc. */
                        slot_ctx[3] = 0; /* Max Exit Latency=0, Root Hub Port Number already in dword1 */
                        volatile uint32_t *ep0_ctx = (volatile uint32_t *)((uint8_t *)dev->input_ctx + 2 * ctx_stride); /* EP0 Context */
                        /* Spec: initial control EP0 Max Packet Size must be 8 until first 8 bytes of device descriptor read */
                        unsigned mps = 8; /* will adjust after reading bMaxPacketSize0 */
                        /* EP0 Endpoint Context (Input): */
                        /* ep0_ctx[0] (EP Info): bits 10:8 Max Burst (0), bits 7:0 Interval (0) */
                        ep0_ctx[0] = 0; /* Leave EP state disabled; controller sets running after address; no invalid state bits */
                        ep0_ctx[1] = 0;
                        ep0_ctx[1] |= 0x3;              /* Error Count = 3 */
                        ep0_ctx[1] |= (4u << 3);        /* Endpoint Type = Control (per spec 4 for EP0) */
                        ep0_ctx[1] |= (mps & 0xFFFF) << 16; /* Max Packet Size */
                        ep0_ctx[4] = (8u << 16); /* Average TRB Length = 8 bytes (for control transfers first stage) */
                        /* Allocate EP0 ring now so EP0 context has valid TR Dequeue Pointer before ADDRESS_DEVICE */
                        if(!ctrl->ep0_ring) {
                            void *ring_raw = kcalloc(1, 16 * sizeof(xhci_trb_t) + 64);
                            if(ring_raw) {
                                unsigned long raw_phys = (unsigned long)mm_get_physical_address((uint64_t)ring_raw);
                                unsigned long aligned_virt = ((unsigned long)ring_raw + 63) & ~63UL; /* 64-byte align */
                                unsigned long aligned_phys = raw_phys + (aligned_virt - (unsigned long)ring_raw);
                                ctrl->ep0_ring = (void *)aligned_virt;
                                ctrl->ep0_ring_index = 0;
                                ctrl->ep0_ring_cycle = 1;
                                xhci_trb_t *ring = (xhci_trb_t *)ctrl->ep0_ring;
                                xhci_trb_t *link_trb = &ring[15];
                                unsigned long ring_phys = aligned_phys;
                                ctrl->ep0_ring_phys = ring_phys;
                                link_trb->param_lo = (uint32_t)(ring_phys & 0xFFFFFFFFu);
                                link_trb->param_hi = (uint32_t)(ring_phys >> 32);
                                link_trb->status = 0;
                                link_trb->control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_LINK) | (ctrl->ep0_ring_cycle & 1) | (1u << 1);
                                /* TR Dequeue Pointer: bits 3:0 must be 0 except bit0 for DCS */
                                uint64_t dq = ((uint64_t)ring_phys & ~0xFULL) | (ctrl->ep0_ring_cycle & 1);
                                ep0_ctx[2] = (uint32_t)(dq & 0xFFFFFFFFu);
                                ep0_ctx[3] = (uint32_t)(dq >> 32);
                                XHCI_EVT_LOG("xhci: ep0 ring phys=%p virt=%p dq_lo=%08x dq_hi=%08x\n", (void *)ring_phys, ctrl->ep0_ring, ep0_ctx[2], ep0_ctx[3]);
                            } else {
                                XHCI_LOG("xhci: warn ep0 ring alloc failed\n");
                            }
                        }
                        if(slot < ctrl->max_slots + 1) {
                            unsigned long *dcbaa = (unsigned long *)ctrl->dcbaa_virt;
                            unsigned long dev_ctx_phys = (unsigned long)mm_get_physical_address((uint64_t)dev->device_ctx);
                            dcbaa[slot] = dev_ctx_phys;
                        }
                        // Queue ADDRESS_DEVICE
                        // Debug: dump first 8 dwords of Input Context and DCBAA entry
                        /* reduced debug: remove large context dump */
                        unsigned long input_ctx_phys = (unsigned long)mm_get_physical_address((uint64_t)dev->input_ctx);
                        __asm__ __volatile__("mfence":::"memory");
                        xhci_trb_t *ad = xhci_cmd_enqueue_nodoorbell(ctrl, XHCI_TRB_TYPE_ADDRESS_DEVICE, input_ctx_phys);
                        if(ad) {
                            /* Insert Slot ID bits before doorbell per spec (control dword bits 31:24) */
                            ad->control |= (slot & 0xFF) << 24;
                            __asm__ __volatile__("mfence":::"memory");
                            /* Ring doorbell after fully populated TRB */
                            volatile uint32_t *db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0x00);
                            *db0 = 0;
                            ctrl->pending_device = dev;
                            XHCI_EVT_LOG("xhci: address_device slot=%u\n", slot);
                        } else {
                            XHCI_LOG("xhci: failed build address_device\n");
                            ctrl->pending_cmd_type = 0;
                            ctrl->pending_cmd_trb = 0;
                            ctrl->pending_cmd_trb_phys = 0;
                            ctrl->pending_device = 0;
                        }
                    }
                } else {
                    XHCI_LOG("xhci: enable_slot failed cc=%u\n", cc);
                    ctrl->pending_cmd_type = 0;
                    ctrl->pending_cmd_trb = 0;
                    ctrl->pending_cmd_trb_phys = 0;
                    ctrl->pending_device = 0;
                }
            } else if(ctrl->pending_cmd_type == XHCI_TRB_TYPE_EVAL_CONTEXT && ctrl->pending_device) {
                /* After successful Evaluate Context, ring EP0 doorbell if we were waiting */
                if(cc == 1) {
                    usb_device_t *dev = (usb_device_t *)ctrl->pending_device;
                    XHCI_EVT_LOG("xhci: eval_context complete slot=%u\n", slot);
                    ctrl->pending_cmd_type = 0;
                    ctrl->pending_cmd_trb = 0;
                    ctrl->pending_cmd_trb_phys = 0;
                    /* Now ring for pending transfer if stage==1 not yet started (it may already have been rung) */
                } else {
                    XHCI_LOG("xhci: eval_context failed cc=%u\n", cc);
                    ctrl->pending_cmd_type = 0;
                    ctrl->pending_cmd_trb = 0;
                    ctrl->pending_cmd_trb_phys = 0;
                }
            } else if(ctrl->pending_cmd_type == XHCI_TRB_TYPE_ADDRESS_DEVICE && ctrl->pending_device) {
                if(cc == 1) {
                    usb_device_t *dev = (usb_device_t *)ctrl->pending_device;
                    if(dev->slot_id && slot && slot != dev->slot_id) {
                        XHCI_LOG("xhci: warn addrdev slot mismatch event=%u dev=%u\n", slot, dev->slot_id);
                        slot = dev->slot_id;
                    }
                    if(!dev->slot_id && slot) {
                        dev->slot_id = slot;
                    }
                    if(!slot) {
                        XHCI_LOG("xhci: err addrdev slot=0\n");
                        ctrl->pending_device = 0;
                        ctrl->pending_cmd_type = 0;
                        ctrl->pending_cmd_trb = 0;
                        ctrl->pending_cmd_trb_phys = 0;
                    } else {
                        XHCI_EVT_LOG("xhci: address_device complete slot=%u\n", slot);
                        /* Dump device context first two contexts (slot + ep0) post-address for debug */
                        unsigned ctx_stride = (ctrl->hccparams1 & (1u << 2)) ? 64u : 32u;
                        volatile uint32_t *dev_slotc = (volatile uint32_t *)dev->device_ctx;
                        volatile uint32_t *dev_ep0c = (volatile uint32_t *)((uint8_t *)dev->device_ctx + ctx_stride);
                        XHCI_EVT_LOG("xhci: devctx after addr SLOT=%08x %08x %08x %08x EP0=%08x %08x %08x %08x\n",
                            dev_slotc[0], dev_slotc[1], dev_slotc[2], dev_slotc[3],
                            dev_ep0c[0], dev_ep0c[1], dev_ep0c[2], dev_ep0c[3]);
                        ctrl->pending_device = 0;
                        ctrl->device_address = dev->address = 1;
                        if(!ctrl->ep0_ring) {
                            XHCI_LOG("xhci: err ep0 ring missing post addrdev\n");
                        } else {
                            xhci_start_enumeration(ctrl, dev);
                        }
                    }
                    ctrl->pending_cmd_type = 0;
                    ctrl->pending_cmd_trb = 0;
                    ctrl->pending_cmd_trb_phys = 0;
                } else {
                    XHCI_LOG("xhci: address_device failed cc=%u\n", cc);
                    if(cc == 5) { /* TRBError debug */
                        usb_device_t *dev = (usb_device_t *)ctrl->pending_device;
                        unsigned slot_dbg = slot;
                        if(!slot_dbg && dev && dev->slot_id) {
                            slot_dbg = dev->slot_id;
                        }
                        if(slot_dbg && slot_dbg < 65 && dev) {
                            unsigned long *dcbaa = (unsigned long *)ctrl->dcbaa_virt;
                            volatile uint32_t *icdump = (volatile uint32_t *)dev->input_ctx;
                            unsigned ctx_stride = (ctrl->hccparams1 & (1u << 2)) ? 64u : 32u;
                            volatile uint32_t *slotc = (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride);
                            volatile uint32_t *ep0c = (volatile uint32_t *)((uint8_t *)dev->input_ctx + 2 * ctx_stride);
                            XHCI_LOG("xhci: trberr slot=%u dcbaa=%p ICC=%08x %08x %08x %08x SLOT=%08x %08x %08x %08x EP0=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                                slot_dbg, slot_dbg, (void *)dcbaa[slot_dbg],
                                icdump[0], icdump[1], icdump[2], icdump[3],
                                slotc[0], slotc[1], slotc[2], slotc[3],
                                ep0c[0], ep0c[1], ep0c[2], ep0c[3], ep0c[4], ep0c[5], ep0c[6], ep0c[7]);
                            volatile uint32_t *dev_slotc = (volatile uint32_t *)dev->device_ctx;
                            volatile uint32_t *dev_ep0c = (volatile uint32_t *)((uint8_t *)dev->device_ctx + ctx_stride);
                            XHCI_LOG("xhci: devctx SLOT=%08x %08x %08x %08x EP0=%08x %08x %08x %08x\n",
                                dev_slotc[0], dev_slotc[1], dev_slotc[2], dev_slotc[3],
                                dev_ep0c[0], dev_ep0c[1], dev_ep0c[2], dev_ep0c[3]);
                        }
                    }
                    ctrl->pending_cmd_type = 0;
                    ctrl->pending_cmd_trb = 0;
                    ctrl->pending_cmd_trb_phys = 0;
                    ctrl->pending_device = 0;
                }
            } else if(ctrl->pending_cmd_type == XHCI_TRB_TYPE_RESET_ENDPOINT || ctrl->pending_cmd_type == XHCI_TRB_TYPE_SET_TR_DEQUEUE_POINTER) {
                /* Clear pending for endpoint maintenance commands regardless of CC */
                ctrl->pending_cmd_type = 0;
                ctrl->pending_cmd_trb = 0;
                ctrl->pending_cmd_trb_phys = 0;
                ctrl->pending_device = 0;
                if(cc != 1) {
                    XHCI_EVT_LOG("xhci: endpoint cmd type=%u cc=%u\n", type, cc);
                }
            } else if(ctrl->pending_cmd_type == XHCI_TRB_TYPE_CONFIG_ENDPOINT && ctrl->pending_device) {
                usb_device_t *dev = (usb_device_t *)ctrl->pending_device;
                if(cc == 1) {
                    dev->endpoints_configured = 1;
                    dev->configured = 1;
                    XHCI_EVT_LOG("xhci: configure_endpoint complete slot=%u bulk in ep=%u out ep=%u\n", dev->slot_id, dev->bulk_in_ep, dev->bulk_out_ep);
                    unsigned ctx_stride2 = (ctrl->hccparams1 & (1u << 2)) ? 64u : 32u;
                    /* Revert EPID computation to spec: EPID = (EpNum << 1) | Dir (1=IN); EP0=1. */
                    unsigned epid_out2 = dev->bulk_out_ep ? ((dev->bulk_out_ep << 1) | 0) : 0;
                    unsigned epid_in2 = dev->bulk_in_ep ? ((dev->bulk_in_ep << 1) | 1) : 0;
                    /* Device Context indexing per spec: Slot Context at index 0, EP0 at 1, EPID n at index n */
                    volatile uint32_t *ep_out_ctx2 = epid_out2 ? (volatile uint32_t *)((uint8_t *)dev->device_ctx + ctx_stride2 * epid_out2) : 0;
                    volatile uint32_t *ep_in_ctx2 = epid_in2 ? (volatile uint32_t *)((uint8_t *)dev->device_ctx + ctx_stride2 * epid_in2) : 0;
                    if(ep_out_ctx2) {
                        XHCI_EVT_LOG("xhci: OUT epctx dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n", ep_out_ctx2[0], ep_out_ctx2[1], ep_out_ctx2[2], ep_out_ctx2[3]);
                    }
                    if(ep_in_ctx2) {
                        XHCI_EVT_LOG("xhci: IN  epctx dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n", ep_in_ctx2[0], ep_in_ctx2[1], ep_in_ctx2[2], ep_in_ctx2[3]);
                    }
                    /* Extra sanity: dump slot context dword0/1 and EPIDs used */
                    volatile uint32_t *slotc_dbg = (volatile uint32_t *)dev->device_ctx;
                    XHCI_EVT_LOG("xhci: DC slotc0=%08x slotc1=%08x (epid_out=%u epid_in=%u)\n", slotc_dbg[0], slotc_dbg[1], epid_out2, epid_in2);
                    /* If endpoint contexts look uninitialized, request Evaluate Context to re-supply them to controller */
                    unsigned add_mask_eval = 0;
                    if(ep_out_ctx2) {
                        unsigned cur_type = (ep_out_ctx2[1] >> 3) & 0x7;
                        uint32_t dq = ep_out_ctx2[2];
                        if(cur_type != 2u || dq == 0) {
                            XHCI_EVT_LOG("xhci: OUT epctx invalid type=%u dq=%08x -> scheduling eval\n", cur_type, dq);
                            add_mask_eval |= (1u << epid_out2);
                        }
                    }
                    if(ep_in_ctx2) {
                        unsigned cur_type = (ep_in_ctx2[1] >> 3) & 0x7;
                        uint32_t dq = ep_in_ctx2[2];
                        if(cur_type != 6u || dq == 0) {
                            XHCI_EVT_LOG("xhci: IN epctx invalid type=%u dq=%08x -> scheduling eval\n", cur_type, dq);
                            add_mask_eval |= (1u << epid_in2);
                        }
                    }
                    if(add_mask_eval) {
                        /* With corrected Input Context indexing this path shouldn't trigger; keep minimal diagnostic. */
                        add_mask_eval |= (1u << 1); /* include EP0 */
                        xhci_issue_eval_context_endpoints(ctrl, dev, add_mask_eval);
                    }
                } else {
                    XHCI_LOG("xhci: configure_endpoint failed cc=%u slot=%u\n", cc, dev->slot_id);
                    ctrl->pending_cmd_type = 0; ctrl->pending_cmd_trb = 0; ctrl->pending_cmd_trb_phys = 0; ctrl->pending_device = 0;
                    goto cmd_done;
                }
                ctrl->pending_cmd_type = 0; ctrl->pending_cmd_trb = 0; ctrl->pending_cmd_trb_phys = 0; ctrl->pending_device = 0;
            } else {
                XHCI_EVT_LOG("xhci: untracked cmd type=%u cc=%u\n", ctrl->pending_cmd_type, cc);
                ctrl->pending_cmd_type = 0;
                ctrl->pending_cmd_trb = 0;
                ctrl->pending_cmd_trb_phys = 0;
                ctrl->pending_device = 0;
            }
cmd_done:;
        } else if(type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
            unsigned int cc = (trb->status >> 24) & 0xFF;
            uint64_t ep_trb_ptr_dbg = ((uint64_t)trb->param_hi << 32) | trb->param_lo;
            unsigned epid_dbg = (trb->control >> 16) & 0x1F; /* bits 20:16 per spec */
            XHCI_EVT_LOG("xhci: transfer event cc=%u epid=%u ptr=%p rawsts=%08x rawctl=%08x\n", cc, epid_dbg, (void *)ep_trb_ptr_dbg, trb->status, trb->control);
            ctrl->msd_transfer_events++; /* count all transfer events */
            /* Determine device: prefer endpoint ID match for bulk traffic */
            usb_device_t *dev = 0;
            unsigned epid = (trb->control >> 16) & 0x1F; /* Endpoint ID bits 20:16 */
            if(epid > 1) { /* bulk or interrupt endpoints (EP0 is 1) */
                for(unsigned s = 1; s < 64; s++) {
                    usb_device_t *cand = (usb_device_t *)ctrl->slot_device_map[s];
                    if(!cand) continue;
                    if(cand->bulk_in_ep || cand->bulk_out_ep) {
                        unsigned epid_out = cand->bulk_out_ep ? ((cand->bulk_out_ep << 1) | 0) : 0;
                        unsigned epid_in  = cand->bulk_in_ep  ? ((cand->bulk_in_ep  << 1) | 1) : 0;
                        if(epid == epid_out || epid == epid_in) { dev = cand; break; }
                    }
                }
                ctrl->msd_bulk_transfer_events++;
            }
            if(!dev) {
                /* Fallback: first present device */
                for(unsigned s = 1; s < 64 && !dev; s++) {
                    if(ctrl->slot_device_map[s]) dev = (usb_device_t *)ctrl->slot_device_map[s];
                }
            }
            uint64_t ep_trb_ptr = ((uint64_t)trb->param_hi << 32) | trb->param_lo;
            /* Identify which TRB in ring this event refers to by physical address */
            if(dev && ctrl->ep0_ring) {
                for(unsigned i = 0; i < 16; i++) {
                    uint64_t phys = ctrl->ep0_ring_phys + i * sizeof(xhci_trb_t);
                    if(phys == ep_trb_ptr) {
                        XHCI_EVT_LOG("xhci: transfer_event cc=%u trb_idx=%u\n", cc, i);
                        /* Data stage completion (index = stage_start+1) triggers advancement for first 8 bytes */
                        unsigned data_idx = (ctrl->ep0_stage_start + 1) % 16;
                        unsigned status_idx = (ctrl->ep0_stage_start + 2) % 16;
                        if(i == data_idx && (ctrl->pending_xfer_stage >= 1 && ctrl->pending_xfer_stage <= 4)) {
                            /* capture data then wait for status */
                        } else if(i == status_idx) {
                            /* status completion -> advance */
                            if(ctrl->pending_xfer_stage >= 1 && ctrl->pending_xfer_stage <= 4) {
                                xhci_control_xfer_event(ctrl, dev, cc);
                            }
                        }
                        break;
                    }
                }
            } else {
                XHCI_EVT_LOG("xhci: transfer_event cc=%u (no dev)\n", cc);
            }
            /* Bulk ring correlation for MSD BOT (compare to tracked TRB virtual pointers -> phys) */
            if(dev && (ctrl->msd_cbw_trb || ctrl->msd_data_trb || ctrl->msd_csw_trb)) {
                /* Use cached physical TRB addresses (avoid repeated mm_get_physical_address calls which may differ) */
                uint64_t cbw_phys = ctrl->msd_cbw_phys;
                uint64_t data_phys = ctrl->msd_data_phys;
                uint64_t csw_phys = ctrl->msd_csw_phys;
                /* Compute expected EPIDs for bulk IN/OUT for this device */
                unsigned epid_out_expected = dev->bulk_out_ep ? ((dev->bulk_out_ep << 1) | 0) : 0;
                unsigned epid_in_expected  = dev->bulk_in_ep  ? ((dev->bulk_in_ep  << 1) | 1) : 0;
                /* Unconditional diagnostic log (lightweight) for bulk endpoints */
                if(epid > 1) {
                    XHCI_MSD_LOG("MSDDBG: TE cc=%u epid=%u ptr=%p cbw=%p data=%p csw=%p state=%u op=%u outEP=%u(in=%u) bulk_ev=%u total_ev=%u\n",
                        cc, epid, (void*)ep_trb_ptr, (void*)cbw_phys, (void*)data_phys, (void*)csw_phys,
                        ctrl->msd_state, ctrl->msd_op, epid_out_expected, epid_in_expected,
                        ctrl->msd_bulk_transfer_events, ctrl->msd_transfer_events);
                }
                int matched = 0;
                if(ep_trb_ptr == cbw_phys) {
                    /* Always record CBW event */
                    ctrl->msd_cbw_events++;
                    ctrl->msd_last_event_cc = cc;
                    ctrl->msd_last_event_epid = epid;
                    ctrl->msd_last_event_ptr = ep_trb_ptr;
                    if(cc != 1) {
                        XHCI_MSD_LOG("MSD: CBW completion error cc=%u -> abort\n", cc);
                        ctrl->msd_state = 0;
                        ctrl->msd_op = 0; /* host BOT reset logic will see timeout/idle and recover */
                    } else if(ctrl->msd_state == 1 || ctrl->msd_state == 2) {
                        if(ctrl->msd_state == 1) {
                            ctrl->msd_state = 2;
                            XHCI_MSD_LOG("MSD: CBW complete (state->2)\n");
                        } else {
                            XHCI_MSD_LOG("MSD: CBW complete (state already 2)\n");
                        }
                        /* If we deferred queuing DATA stage, queue it now */
                        if(ctrl->msd_pending_data_len && ctrl->msd_pending_data_buf) {
                            if(xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_pending_data_buf, ctrl->msd_pending_data_len) == ST_OK) {
                                unsigned didx = (ctrl->bulk_in_enqueue - 1) % 16;
                                ctrl->msd_data_trb = &((xhci_trb_t *)ctrl->bulk_in_ring)[didx];
                                ctrl->msd_data_phys = (unsigned long)mm_get_physical_address((uint64_t)ctrl->msd_data_trb);
                            }
                            ctrl->msd_pending_data_buf = 0;
                            ctrl->msd_pending_data_len = 0;
                        } else if(ctrl->msd_need_csw) {
                            /* No data: queue CSW now */
                            if(!ctrl->msd_csw_buf) {
                                ctrl->msd_csw_buf = kcalloc(1, 64);
                            }
                            if(xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_csw_buf, sizeof(usb_msd_csw_t)) == ST_OK) {
                                unsigned cidx = (ctrl->bulk_in_enqueue - 1) % 16;
                                ctrl->msd_csw_trb = &((xhci_trb_t *)ctrl->bulk_in_ring)[cidx];
                                ctrl->msd_csw_phys = ctrl->bulk_in_ring_phys + ((uint8_t *)ctrl->msd_csw_trb - (uint8_t *)ctrl->bulk_in_ring);
                                XHCI_MSD_LOG("MSD: CSW queued csw_trb=%p trb_phys=%p\n", ctrl->msd_csw_trb, (void *)ctrl->msd_csw_phys);
                                ctrl->msd_state = 3;
                                ctrl->msd_need_csw = 0;
                            }
                        }
                    }
                    matched = 1;
                } else if(ep_trb_ptr == data_phys) {
                    if(ctrl->msd_state == 2) {
                        ctrl->msd_data_events++;
                        ctrl->msd_last_event_cc = cc;
                        ctrl->msd_last_event_epid = epid;
                        ctrl->msd_last_event_ptr = ep_trb_ptr;
                        if(cc != 1) {
                            XHCI_MSD_LOG("MSD: DATA phase error cc=%u -> abort\n", cc);
                            ctrl->msd_state = 0;
                            ctrl->msd_op = 0;
                        } else {
                            ctrl->msd_state = 3;
                            XHCI_MSD_LOG("MSD: DATA phase complete (queue CSW)\n");
                            if(ctrl->msd_op) {
                                if(!ctrl->msd_csw_buf) {
                                    ctrl->msd_csw_buf = kcalloc(1, 64);
                                }
                                if(xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_csw_buf, sizeof(usb_msd_csw_t)) == ST_OK) {
                                    unsigned cidx = (ctrl->bulk_in_enqueue - 1) % 16;
                                    ctrl->msd_csw_trb = &((xhci_trb_t *)ctrl->bulk_in_ring)[cidx];
                                    ctrl->msd_csw_phys = ctrl->bulk_in_ring_phys + ((uint8_t *)ctrl->msd_csw_trb - (uint8_t *)ctrl->bulk_in_ring);
                                    XHCI_MSD_LOG("MSD: CSW queued (post DATA) csw_trb=%p trb_phys=%p\n", ctrl->msd_csw_trb, (void *)ctrl->msd_csw_phys);
                                }
                            }
                        }
                    }
                    matched = 1;
                } else if(ep_trb_ptr == csw_phys) {
                    if(ctrl->msd_state == 3) {
                        ctrl->msd_csw_events++;
                        ctrl->msd_last_event_cc = cc;
                        ctrl->msd_last_event_epid = epid;
                        ctrl->msd_last_event_ptr = ep_trb_ptr;
                        if(cc == 1) {
                            ctrl->msd_state = 4;
                            XHCI_MSD_LOG("MSD: CSW complete (success)\n");
                        } else {
                            XHCI_MSD_LOG("MSD: CSW error cc=%u -> reset pending\n", cc);
                            ctrl->msd_state = 0;
                            ctrl->msd_op = 0; /* trigger retry logic outside */
                        }
                    }
                    matched = 1;
                }
                /* Fallback: if pointer mismatch but epid matches expected and we are in appropriate state, advance state */
                if(!matched && ctrl->msd_op) {
                    if(epid == epid_out_expected && (ctrl->msd_state == 1 || ctrl->msd_state == 2)) {
                        XHCI_MSD_LOG("MSD: Fallback CBW event epid=%u ptr=%p expected_ptr=%p\n", epid, (void*)ep_trb_ptr, (void*)cbw_phys);
                        if(ctrl->msd_state == 1) ctrl->msd_state = 2;
                        matched = 1;
                    } else if(epid == epid_in_expected && ctrl->msd_state == 2) {
                        XHCI_MSD_LOG("MSD: Fallback DATA event epid=%u ptr=%p expected_ptr=%p\n", epid, (void*)ep_trb_ptr, (void*)data_phys);
                        ctrl->msd_state = 3;
                        matched = 1;
                    } else if(epid == epid_in_expected && ctrl->msd_state == 3) {
                        XHCI_MSD_LOG("MSD: Fallback CSW event epid=%u ptr=%p expected_ptr=%p\n", epid, (void*)ep_trb_ptr, (void*)csw_phys);
                        ctrl->msd_state = (cc == 1) ? 4 : 0;
                        matched = 1;
                    }
                    if(!matched) {
                        XHCI_MSD_LOG("MSD: Unmatched transfer event (epid=%u ptr=%p cbw=%p data=%p csw=%p state=%u op=%u)\n", epid, (void*)ep_trb_ptr, (void*)cbw_phys, (void*)data_phys, (void*)csw_phys, ctrl->msd_state, ctrl->msd_op);
                    }
                }
            }
        }
        ctrl->event_ring_dequeue++;
        if(ctrl->event_ring_dequeue % ctrl->event_ring_size == 0) {
            ctrl->event_ring_cycle ^= 1;
        }
        processed++;
    }
    if(processed) {
        volatile uint64_t *erdp = (volatile uint64_t *)(ctrl->runtime_base + XHCI_RT_IR0_ERDP);
        uint64_t val = ctrl->event_ring_phys + ((ctrl->event_ring_dequeue % ctrl->event_ring_size) * sizeof(xhci_trb_t));
        *erdp = val | (1ull << 3);
    }
    /* removed verbose idle ring snapshot */
    return processed ? processed : ST_OK;
}

void xhci_debug_state(xhci_controller_t* ctrl)
{
    (void)ctrl;
}

// Forward declarations
static void xhci_configure_bulk_endpoints(xhci_controller_t* ctrl, usb_device_t* dev);
// Minimal mass storage bulk endpoint configuration wrapper
int xhci_configure_mass_storage_endpoints(xhci_controller_t *ctrl, struct usb_device *dev)
{
    if(!ctrl || !dev) {
        return ST_INVALID;
    }
    if(dev->endpoints_configured) {
        return ST_OK;
    }
    /* We expect bulk_in_ep and bulk_out_ep already discovered during descriptor parsing (not implemented yet) */
    /* For now, assume fixed EP numbers 1 OUT, 2 IN if none set (QEMU typical for usb-storage) */
    if(!dev->bulk_out_ep) {
        dev->bulk_out_ep = 1; /* EP1 OUT */
    }
    if(!dev->bulk_in_ep) {
        dev->bulk_in_ep = 2;  /* EP2 IN (avoid sharing EP number) */
    }
    if(dev->bulk_in_ep & 0x80) {
        dev->bulk_in_ep &= 0x0F; /* ensure pure number */
    }
    /* Provide plausible max packet sizes (512 for high speed / bulk) */
    if(!dev->bulk_in_mps) {
        dev->bulk_in_mps = 512;
    }
    if(!dev->bulk_out_mps) {
        dev->bulk_out_mps = 512;
    }
    /* Build contexts + issue configure endpoint */
    xhci_configure_bulk_endpoints(ctrl, dev);
    /* Do NOT mark endpoints_configured yet; wait for CONFIG_ENDPOINT completion event */
    XHCI_LOG("xhci: MSD endpoint configuration command queued (IN ep=%u OUT ep=%u)\n", dev->bulk_in_ep, dev->bulk_out_ep);
    return ST_OK;
}

static int xhci_bulk_enqueue(xhci_controller_t *ctrl, void *ring_base, unsigned *enqueue_idx, unsigned *cycle,
                   void **out_trb, void *buf, unsigned len)
{
    if(!ring_base || !enqueue_idx || !cycle || !buf || !len) {
        return ST_INVALID;
    }
    if(*enqueue_idx >= 15) {
        /* Update link TRB cycle to current PCS before wrapping, then toggle PCS */
        xhci_trb_t *ring = (xhci_trb_t *)ring_base;
        xhci_trb_t *link = &ring[15];
        unsigned int ctl = link->control;
        ctl &= ~1u;
        ctl |= (*cycle & 1u);
        link->control = ctl; /* preserve type+toggle bit programmed at init */
        *enqueue_idx = 0;
        *cycle ^= 1;
    }
    xhci_trb_t *ring = (xhci_trb_t *)ring_base;
    xhci_trb_t *trb = &ring[*enqueue_idx];
    unsigned long phys = (unsigned long)mm_get_physical_address((uint64_t)buf);
    trb->param_lo = (uint32_t)(phys & 0xFFFFFFFFu);
    trb->param_hi = (uint32_t)(phys >> 32);
    trb->status = len;
    trb->control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_NORMAL) | (*cycle & 1) | XHCI_TRB_IOC;
    __asm__ __volatile__("mfence":::"memory");
    XHCI_BULK_LOG("xhci: bulk enqueue len=%u buf_phys=%p trb=%p idx=%u cyc=%u\n", len, (void *)phys, trb, *enqueue_idx, *cycle & 1);
    if(out_trb) {
        *out_trb = trb;
    }
    (*enqueue_idx)++;
    return ST_OK;
}

int xhci_enqueue_bulk_out(xhci_controller_t *ctrl, struct usb_device *dev, void *buf, unsigned len)
{
    if(!ctrl || !dev) {
        return ST_INVALID;
    }
    void *trb = 0;
    int st = xhci_bulk_enqueue(ctrl, ctrl->bulk_out_ring, &ctrl->bulk_out_enqueue, &ctrl->bulk_out_cycle, &trb, buf, len);
    if(st != ST_OK) {
        return st;
    }
    uint64_t trb_phys = ctrl->bulk_out_ring_phys + ((uint8_t *)trb - (uint8_t *)ctrl->bulk_out_ring);
    volatile uint32_t *db = (volatile uint32_t *)(ctrl->doorbell_array + dev->slot_id * 4);
    unsigned epid = (dev->bulk_out_ep << 1); /* EPID per spec (OUT dir=0). EP0 is 1; ep1 OUT =>2. */
    *db = epid;
    static int once_out = 0;
    if(!once_out) {
        once_out = 1;
        XHCI_LOG("xhci: bulk OUT first enqueue idx=%u cyc=%u trb_phys=%p\n", (ctrl->bulk_out_enqueue - 1) % 16, ctrl->bulk_out_cycle & 1, (void *)trb_phys);
    }
    return ST_OK;
}

int xhci_enqueue_bulk_in(xhci_controller_t *ctrl, struct usb_device *dev, void *buf, unsigned len)
{
    if(!ctrl || !dev) {
        return ST_INVALID;
    }
    void *trb = 0;
    int st = xhci_bulk_enqueue(ctrl, ctrl->bulk_in_ring, &ctrl->bulk_in_enqueue, &ctrl->bulk_in_cycle, &trb, buf, len);
    if(st != ST_OK) {
        return st;
    }
    uint64_t trb_phys = ctrl->bulk_in_ring_phys + ((uint8_t *)trb - (uint8_t *)ctrl->bulk_in_ring);
    volatile uint32_t *db = (volatile uint32_t *)(ctrl->doorbell_array + dev->slot_id * 4);
    unsigned epid = ((dev->bulk_in_ep << 1) | 1); /* EPID per spec (IN dir=1). ep1 IN =>3, ep2 IN =>5. */
    *db = epid;
    static int once_in = 0;
    if(!once_in) {
        once_in = 1;
        XHCI_LOG("xhci: bulk IN first enqueue idx=%u cyc=%u trb_phys=%p\n", (ctrl->bulk_in_enqueue - 1) % 16, ctrl->bulk_in_cycle & 1, (void *)trb_phys);
    }
    return ST_OK;
}

// ------- Control transfer helpers -------
static void xhci_ep0_build_setup(xhci_controller_t *ctrl, xhci_trb_t *ring,
    uint8_t bm, uint8_t req, uint16_t wValue, uint16_t wIndex,
    uint16_t wLength, int in_dir)
{
    uint64_t pkt;
    uint32_t trt;

    pkt = (uint64_t)bm |
        ((uint64_t)req << 8) |
        ((uint64_t)wValue << 16) |
        ((uint64_t)wIndex << 32) |
        ((uint64_t)wLength << 48);

    ring[0].param_lo = (uint32_t)(pkt & 0xFFFFFFFFu);
    ring[0].param_hi = (uint32_t)(pkt >> 32);
    ring[0].status = 8; /* setup length */

    trt = in_dir ? XHCI_SETUP_TRT_IN_DATA :
        (wLength ? XHCI_SETUP_TRT_OUT_DATA : XHCI_SETUP_TRT_NO_DATA);
    ring[0].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_SETUP_STAGE) |
        (ctrl->ep0_ring_cycle & 1) | trt | XHCI_TRB_IOC | XHCI_TRB_IDT;
    (void)ctrl;
    (void)in_dir;
}

static void xhci_init_ep_ctx(volatile uint32_t *ep_ctx, unsigned mps, int in)
{
    if(!ep_ctx) {
        return;
    }
    for(int i = 0; i < 8; i++) {
        ep_ctx[i] = 0;
    }
    if(mps > 1024) {
        mps = 1024; /* allow SuperSpeed bulk 1024 max packet */
    }
    unsigned ep_type = in ? 6u : 2u; /* Bulk IN=6 OUT=2 */
    ep_ctx[0] = 0; /* EP State = Disabled (0), leave other bits 7:0 (Mult, MaxPStreams, LSA) zero */
    /* DW1:
     *  Bits 2:0 CErr = 3 (allow up to 3 retries)
     *  Bits 5:3 Endpoint Type
     *  Bit  7:6 Reserved, Bit 8: Host Initiate Disable (0), Bit 9: Max Burst Size bits[7:0] ??? (for SS) kept 0
     *  Bits 15:10 Reserved
     *  Bits 31:16 Max Packet Size
     */
    ep_ctx[1] = 0x3u | (ep_type << 3) | ((mps & 0xFFFF) << 16);
    /* DW4: bits 31:16 Average TRB Length (set to MPS), bits 7:0 Max Burst (leave 0 for now) */
    ep_ctx[4] = ((mps & 0xFFFF) << 16);
}

static void xhci_configure_bulk_endpoints(xhci_controller_t *ctrl, usb_device_t *dev)
{
    if(!ctrl || !dev || !dev->input_ctx || !dev->device_ctx) {
        return;
    }
    if(dev->endpoints_configured || ctrl->pending_cmd_type == XHCI_TRB_TYPE_CONFIG_ENDPOINT) {
        return;
    }
    if(!dev->bulk_in_ep && !dev->bulk_out_ep) {
        return;
    }
    unsigned ctx_stride = (ctrl->hccparams1 & (1u << 2)) ? 64u : 32u;
    volatile uint32_t *icc = (volatile uint32_t *)dev->input_ctx;
    /* Prepare Input Control Context flags */
    uint32_t add = 0x1; /* include slot only; we'll add bulk endpoints below (exclude EP0 to avoid reconfig of control) */
    /* Compute EPIDs per spec: EPID = (EpNum << 1) | Dir (1=IN). EP0 = 1. */
    unsigned epid_out = dev->bulk_out_ep ? ((dev->bulk_out_ep << 1) | 0) : 0; /* OUT dir=0 */
    unsigned epid_in  = dev->bulk_in_ep  ? ((dev->bulk_in_ep  << 1) | 1) : 0; /* IN dir=1 */
    unsigned max_epid = 1; /* at least EP0 */
    if(epid_out) {
        add |= (1u << epid_out);
        if(epid_out > max_epid) {
            max_epid = epid_out;
        }
    }
    if(epid_in) {
        add |= (1u << epid_in);
        if(epid_in > max_epid) {
            max_epid = epid_in;
        }
    }
    icc[0] = 0;
    icc[1] = add; /* drop=0 add=set */
    volatile uint32_t *slot_ctx = (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride);
    /* Build fresh Slot Context (do not rely on possibly sparse device context copy) */
    unsigned speed_code = 0;
    switch(dev->speed) {
    case USB_SPEED_LOW: speed_code = 2; break;
    case USB_SPEED_FULL: speed_code = 1; break;
    case USB_SPEED_HIGH: speed_code = 3; break;
    case USB_SPEED_SUPER: speed_code = 4; break;
    default: break;
    }
    for(int i = 0; i < 4; i++) {
        slot_ctx[i] = 0;
    }
    slot_ctx[0] = ((max_epid & 0x1F) << 27) | ((speed_code & 0xF) << 20);
    slot_ctx[1] = (dev->port_number & 0xFF) << 16; /* Root Hub Port Number */
    /* Leave slot_ctx[2]=0 (TT info not used) slot_ctx[3]=0 (Max Exit Latency, Interrupter etc.) */
    XHCI_EVT_LOG("xhci: slot_ctx build sc0=%08x sc1=%08x max_epid=%u speed=%u\n", slot_ctx[0], slot_ctx[1], max_epid, speed_code);
    /* OUT endpoint context */
    if(epid_out) {
        /* In Input Context layout, after Input Control (2 contexts?), spec: EPID index plus offset */
        volatile uint32_t *ep_out_ctx = (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride * (epid_out + 1));
        unsigned out_mps = dev->bulk_out_mps;
        if(dev->speed != USB_SPEED_SUPER && out_mps > 512) {
            out_mps = 512;
        }
        if(dev->speed == USB_SPEED_SUPER && out_mps > 1024) {
            out_mps = 1024;
        }
        xhci_init_ep_ctx(ep_out_ctx, out_mps, 0);
        /* Allocate ring */
        if(!ctrl->bulk_out_ring) {
            void *raw = kcalloc(1, 16 * sizeof(xhci_trb_t) + 64);
            if(raw) {
                unsigned long raw_phys = (unsigned long)mm_get_physical_address((uint64_t)raw);
                unsigned long av = ((unsigned long)raw + 63) & ~63UL;
                unsigned long phys = raw_phys + (av - (unsigned long)raw);
                ctrl->bulk_out_ring = (void *)av;
                ctrl->bulk_out_ring_phys = phys;
                ctrl->bulk_out_cycle = 1;
                ctrl->bulk_out_enqueue = 0;
                xhci_trb_t *ring = (xhci_trb_t *)ctrl->bulk_out_ring;
                xhci_trb_t *link = &ring[15];
                link->param_lo = (uint32_t)(phys & 0xFFFFFFFFu);
                link->param_hi = (uint32_t)(phys >> 32);
                link->status = 0;
                link->control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_LINK) | (1 /*cycle*/) | (1u << 1);
                uint64_t dq = (phys & ~0xFULL) | 1ull; /* DCS=1 initial */
                ep_out_ctx[2] = (uint32_t)(dq & 0xFFFFFFFFu);
                ep_out_ctx[3] = (uint32_t)(dq >> 32);
            }
        }
    }
    /* IN endpoint context */
    if(epid_in) {
        volatile uint32_t *ep_in_ctx = (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride * (epid_in + 1));
        unsigned in_mps = dev->bulk_in_mps;
        if(dev->speed != USB_SPEED_SUPER && in_mps > 512) {
            in_mps = 512;
        }
        if(dev->speed == USB_SPEED_SUPER && in_mps > 1024) {
            in_mps = 1024;
        }
        xhci_init_ep_ctx(ep_in_ctx, in_mps, 1);
        if(!ctrl->bulk_in_ring) {
            void *raw = kcalloc(1, 16 * sizeof(xhci_trb_t) + 64);
            if(raw) {
                unsigned long raw_phys = (unsigned long)mm_get_physical_address((uint64_t)raw);
                unsigned long av = ((unsigned long)raw + 63) & ~63UL;
                unsigned long phys = raw_phys + (av - (unsigned long)raw);
                ctrl->bulk_in_ring = (void *)av;
                ctrl->bulk_in_ring_phys = phys;
                ctrl->bulk_in_cycle = 1;
                ctrl->bulk_in_enqueue = 0;
                xhci_trb_t *ring = (xhci_trb_t *)ctrl->bulk_in_ring;
                xhci_trb_t *link = &ring[15];
                link->param_lo = (uint32_t)(phys & 0xFFFFFFFFu);
                link->param_hi = (uint32_t)(phys >> 32);
                link->status = 0;
                link->control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_LINK) | (1 /*cycle*/) | (1u << 1);
                uint64_t dq = (phys & ~0xFULL) | 1ull; /* DCS=1 */
                ep_in_ctx[2] = (uint32_t)(dq & 0xFFFFFFFFu);
                ep_in_ctx[3] = (uint32_t)(dq >> 32);
            }
        }
    }
    /* Issue Configure Endpoint command */
    unsigned long ic_phys = (unsigned long)mm_get_physical_address((uint64_t)dev->input_ctx);
    xhci_trb_t *cfg = xhci_cmd_enqueue_nodoorbell(ctrl, XHCI_TRB_TYPE_CONFIG_ENDPOINT, ic_phys);
    if(cfg) {
        cfg->control |= (dev->slot_id & 0xFF) << 24;
        ctrl->pending_cmd_type = XHCI_TRB_TYPE_CONFIG_ENDPOINT;
        ctrl->pending_device = dev;
        ctrl->pending_cmd_trb = cfg;
        ctrl->pending_cmd_trb_phys = ctrl->cmd_ring_phys + ((unsigned long)cfg - (unsigned long)ctrl->cmd_ring_virt);
        /* Debug: dump input endpoint contexts just before ringing the doorbell */
        if(epid_out) {
            volatile uint32_t *ep_out_ctx_dbg2 = (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride * (epid_out + 1));
            XHCI_EVT_LOG("xhci: pre-CFG OUT inctx dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n", ep_out_ctx_dbg2[0], ep_out_ctx_dbg2[1], ep_out_ctx_dbg2[2], ep_out_ctx_dbg2[3]);
        }
        if(epid_in) {
            volatile uint32_t *ep_in_ctx_dbg2 = (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride * (epid_in + 1));
            XHCI_EVT_LOG("xhci: pre-CFG IN  inctx dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n", ep_in_ctx_dbg2[0], ep_in_ctx_dbg2[1], ep_in_ctx_dbg2[2], ep_in_ctx_dbg2[3]);
        }
        __asm__ __volatile__("mfence":::"memory");
        volatile uint32_t *db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0);
        *db0 = 0;
        /* Debug dump a few context dwords for failure analysis */
        volatile uint32_t *ep_out_ctx_dbg = epid_out ? (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride * epid_out) : 0;
        volatile uint32_t *ep_in_ctx_dbg  = epid_in  ? (volatile uint32_t *)((uint8_t *)dev->input_ctx + ctx_stride * epid_in) : 0;
        XHCI_EVT_LOG("xhci: configure_endpoint slot=%u epid_out=%u epid_in=%u add_mask=0x%08x sc0=%08x sc1=%08x sc2=%08x sc3=%08x out0=%08x out1=%08x out2=%08x in0=%08x in1=%08x in2=%08x\n",
            dev->slot_id, epid_out, epid_in, icc[1], slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3],
            ep_out_ctx_dbg ? ep_out_ctx_dbg[0] : 0, ep_out_ctx_dbg ? ep_out_ctx_dbg[1] : 0, ep_out_ctx_dbg ? ep_out_ctx_dbg[2] : 0,
            ep_in_ctx_dbg ? ep_in_ctx_dbg[0] : 0, ep_in_ctx_dbg ? ep_in_ctx_dbg[1] : 0, ep_in_ctx_dbg ? ep_in_ctx_dbg[2] : 0);
    }
}

static void xhci_post_ep0_doorbell(xhci_controller_t* ctrl, unsigned slot_id){
    // Doorbell target 1 selects control endpoint context (EPID=1 for EP0 per spec)
    volatile uint32_t* db = (volatile uint32_t*)(ctrl->doorbell_array + slot_id*4);
    *db = 1;
}


void xhci_start_enumeration(xhci_controller_t *ctrl, struct usb_device *dev)
{
    uint8_t *buf8;
    unsigned long buf8_phys;
    xhci_trb_t *base;
    xhci_trb_t *t;

    if (!ctrl || !dev || !ctrl->ep0_ring)
        return;

    if (ctrl->ep0_ring_index >= 15) {
        ctrl->ep0_ring_index = 0;
        ctrl->ep0_ring_cycle ^= 1;
    }
    base = (xhci_trb_t *)ctrl->ep0_ring;
    t = &base[ctrl->ep0_ring_index];
    ctrl->ep0_stage_start = ctrl->ep0_ring_index;
    buf8 = (uint8_t *)kcalloc(1, 64);
    ctrl->pending_xfer_buf = buf8;
    ctrl->pending_xfer_len = 8;
    ctrl->pending_xfer_stage = 1;
    xhci_ep0_build_setup(ctrl, t, USB_REQTYPE_DEVICE_TO_HOST,
        USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8) | 0, 0, 8, 1);
    buf8_phys = (unsigned long)mm_get_physical_address((uint64_t)buf8);
    t[1].param_lo = (uint32_t)(buf8_phys & 0xFFFFFFFFu);
    t[1].param_hi = (uint32_t)(buf8_phys >> 32);
    t[1].status = 8;
    t[1].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_DATA_STAGE) |
        (ctrl->ep0_ring_cycle & 1) | (1 << 16);
    t[2].param_lo = 0;
    t[2].param_hi = 0;
    t[2].status = 0;
    t[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) |
        (ctrl->ep0_ring_cycle & 1) | XHCI_TRB_IOC;
    ctrl->ep0_ring_index += 3;
    __asm__ __volatile__("mfence" ::: "memory");
    if (dev->input_ctx) {
        volatile uint32_t *icc = (volatile uint32_t *)dev->input_ctx;
        unsigned long input_ctx_phys;
        xhci_trb_t *eval;
        icc[0] = 0;
        icc[1] = 0x3;
        input_ctx_phys = (unsigned long)mm_get_physical_address((uint64_t)dev->input_ctx);
        eval = xhci_cmd_enqueue_nodoorbell(ctrl, XHCI_TRB_TYPE_EVAL_CONTEXT,
            input_ctx_phys);
        if (eval) {
            eval->control |= (dev->slot_id & 0xFF) << 24;
            __asm__ __volatile__("mfence" ::: "memory");
            {
                volatile uint32_t *db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0);
                *db0 = 0;
            }
            XHCI_EVT_LOG("xhci: eval_context slot=%u\n", dev->slot_id);
        }
    }
    xhci_post_ep0_doorbell(ctrl, dev->slot_id ? dev->slot_id : 1);
    XHCI_EVT_LOG("xhci: ctrl get dev desc 8 bytes (doorbell=1)\n");
}

void xhci_control_xfer_event(xhci_controller_t *ctrl, struct usb_device *dev,
    unsigned cc)
{
    (void)cc;
    if (!ctrl || !dev)
        return;

    if (ctrl->pending_xfer_stage == 1 && ctrl->pending_xfer_buf) {
        int i;
        for (i = 0; i < 8; i++)
            dev->dev_desc8[i] = ((uint8_t *)ctrl->pending_xfer_buf)[i];
        dev->have_desc8 = 1;
        XHCI_EVT_LOG("USB: Got first 8 bytes Device Descriptor (MPS0=%u)\n",
            dev->dev_desc8[7]);
        ctrl->pending_xfer_stage = 2;
        if (ctrl->ep0_ring) {
            unsigned ctx_stride;
            volatile uint32_t *dev_ep0c;
            uint16_t old_mps;
            uint8_t *buf18;
            unsigned long buf18_phys;
            xhci_trb_t *base;
            xhci_trb_t *t;
            ctx_stride = (ctrl->hccparams1 & (1u << 2)) ? 64u : 32u;
            dev_ep0c = (volatile uint32_t *)((uint8_t *)dev->device_ctx + ctx_stride);
            old_mps = (uint16_t)((dev_ep0c[1] >> 16) & 0xFFFF);
            dev_ep0c[1] &= ~(0xFFFFu << 16);
            dev_ep0c[1] |= (dev->dev_desc8[7] & 0xFF) << 16;
            XHCI_EVT_LOG("xhci: update EP0 MPS old=%u new=%u\n", old_mps,
                dev->dev_desc8[7]);
            if (dev->input_ctx) {
                volatile uint32_t *icc = (volatile uint32_t *)dev->input_ctx;
                unsigned long ic_phys = (unsigned long)
                    mm_get_physical_address((uint64_t)dev->input_ctx);
                xhci_trb_t *eval = xhci_cmd_enqueue_nodoorbell(ctrl,
                    XHCI_TRB_TYPE_EVAL_CONTEXT, ic_phys);
                if (eval) {
                    eval->control |= (dev->slot_id & 0xFF) << 24;
                    __asm__ __volatile__("mfence" ::: "memory");
                    {
                        volatile uint32_t *db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0);
                        *db0 = 0;
                    }
                    XHCI_EVT_LOG("xhci: eval_context (mps update) slot=%u\n",
                        dev->slot_id);
                }
            }
            if (ctrl->ep0_ring_index >= 15) {
                ctrl->ep0_ring_index = 0;
                ctrl->ep0_ring_cycle ^= 1;
            }
            base = (xhci_trb_t *)ctrl->ep0_ring;
            t = &base[ctrl->ep0_ring_index];
            ctrl->ep0_stage_start = ctrl->ep0_ring_index;
            buf18 = (uint8_t *)kcalloc(1, 64);
            ctrl->pending_xfer_buf = buf18;
            ctrl->pending_xfer_len = 18;
            xhci_ep0_build_setup(ctrl, t, USB_REQTYPE_DEVICE_TO_HOST,
                USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8) | 0, 0, 18, 1);
            buf18_phys = (unsigned long)mm_get_physical_address((uint64_t)buf18);
            t[1].param_lo = (uint32_t)(buf18_phys & 0xFFFFFFFFu);
            t[1].param_hi = (uint32_t)(buf18_phys >> 32);
            t[1].status = 18;
            t[1].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_DATA_STAGE) |
                (ctrl->ep0_ring_cycle & 1) | (1 << 16);
            t[2].param_lo = 0;
            t[2].param_hi = 0;
            t[2].status = 0;
            t[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) |
                (ctrl->ep0_ring_cycle & 1) | XHCI_TRB_IOC;
            ctrl->ep0_ring_index += 3;
            __asm__ __volatile__("mfence" ::: "memory");
            xhci_post_ep0_doorbell(ctrl, dev->slot_id ? dev->slot_id : 1);
            XHCI_EVT_LOG("XHCI: Requested full 18-byte device descriptor (doorbell=1)\n");
        }
    } else if (ctrl->pending_xfer_stage == 2 && ctrl->pending_xfer_buf) {
        int i;
        for (i = 0; i < 18; i++)
            dev->dev_desc18[i] = ((uint8_t *)ctrl->pending_xfer_buf)[i];
        dev->have_desc18 = 1;
        XHCI_EVT_LOG("USB: Got full 18-byte device descriptor VID=%02x%02x PID=%02x%02x\n",
            dev->dev_desc18[9], dev->dev_desc18[8], dev->dev_desc18[11],
            dev->dev_desc18[10]);
        ctrl->pending_xfer_stage = 3;
        if (ctrl->ep0_ring) {
            xhci_trb_t *base;
            xhci_trb_t *t;
            uint8_t *cfg9;
            unsigned long cfg9_phys;
            if (ctrl->ep0_ring_index >= 15) {
                ctrl->ep0_ring_index = 0;
                ctrl->ep0_ring_cycle ^= 1;
            }
            base = (xhci_trb_t *)ctrl->ep0_ring;
            t = &base[ctrl->ep0_ring_index];
            ctrl->ep0_stage_start = ctrl->ep0_ring_index;
            cfg9 = (uint8_t *)kcalloc(1, 64);
            ctrl->pending_xfer_buf = cfg9;
            ctrl->pending_xfer_len = 9;
            xhci_ep0_build_setup(ctrl, t, USB_REQTYPE_DEVICE_TO_HOST,
                USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIGURATION << 8) | 0, 0, 9, 1);
            cfg9_phys = (unsigned long)mm_get_physical_address((uint64_t)cfg9);
            t[1].param_lo = (uint32_t)(cfg9_phys & 0xFFFFFFFFu);
            t[1].param_hi = (uint32_t)(cfg9_phys >> 32);
            t[1].status = 9;
            t[1].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_DATA_STAGE) |
                (ctrl->ep0_ring_cycle & 1) | (1 << 16);
            t[2].param_lo = 0;
            t[2].param_hi = 0;
            t[2].status = 0;
            t[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) |
                (ctrl->ep0_ring_cycle & 1) | XHCI_TRB_IOC;
            ctrl->ep0_ring_index += 3;
            __asm__ __volatile__("mfence" ::: "memory");
            xhci_post_ep0_doorbell(ctrl, dev->slot_id ? dev->slot_id : 1);
            XHCI_EVT_LOG("XHCI: Requested 9-byte config descriptor\n");
        }
    } else if (ctrl->pending_xfer_stage == 3 && ctrl->pending_xfer_buf) {
        uint8_t *cfg9 = (uint8_t *)ctrl->pending_xfer_buf;
        uint16_t total_len = (uint16_t)cfg9[2] | ((uint16_t)cfg9[3] << 8);
        XHCI_EVT_LOG("USB: Got config desc header total_len=%u interfaces=%u\n",
            total_len, cfg9[4]);
        if (total_len > 1024)
            total_len = 1024;
        ctrl->pending_xfer_stage = 4;
        if (ctrl->ep0_ring) {
            xhci_trb_t *base;
            xhci_trb_t *t;
            uint8_t *cfg_full;
            unsigned long cfg_phys;
            if (ctrl->ep0_ring_index >= 15) {
                ctrl->ep0_ring_index = 0;
                ctrl->ep0_ring_cycle ^= 1;
            }
            base = (xhci_trb_t *)ctrl->ep0_ring;
            t = &base[ctrl->ep0_ring_index];
            ctrl->ep0_stage_start = ctrl->ep0_ring_index;
            cfg_full = (uint8_t *)kcalloc(1, total_len + 16);
            ctrl->pending_xfer_buf = cfg_full;
            ctrl->pending_xfer_len = total_len;
            xhci_ep0_build_setup(ctrl, t, USB_REQTYPE_DEVICE_TO_HOST,
                USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIGURATION << 8) | 0, 0,
                total_len, 1);
            cfg_phys = (unsigned long)mm_get_physical_address((uint64_t)cfg_full);
            t[1].param_lo = (uint32_t)(cfg_phys & 0xFFFFFFFFu);
            t[1].param_hi = (uint32_t)(cfg_phys >> 32);
            t[1].status = total_len;
            t[1].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_DATA_STAGE) |
                (ctrl->ep0_ring_cycle & 1) | (1 << 16);
            t[2].param_lo = 0;
            t[2].param_hi = 0;
            t[2].status = 0;
            t[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) |
                (ctrl->ep0_ring_cycle & 1) | XHCI_TRB_IOC;
            ctrl->ep0_ring_index += 3;
            __asm__ __volatile__("mfence" ::: "memory");
            xhci_post_ep0_doorbell(ctrl, dev->slot_id ? dev->slot_id : 1);
            XHCI_EVT_LOG("XHCI: Requested full %u-byte config descriptor\n",
                total_len);
        }
    } else if (ctrl->pending_xfer_stage == 4 && ctrl->pending_xfer_buf) {
        uint8_t *cfg_full = (uint8_t *)ctrl->pending_xfer_buf;
        uint16_t tot = ctrl->pending_xfer_len;
        uint8_t cfg_value = cfg_full[5];
        uint8_t *p = cfg_full;
        uint16_t parsed = 0;
        uint8_t bulk_in_addr = 0, bulk_out_addr = 0;
        uint16_t bulk_in_mps = 0, bulk_out_mps = 0;
        uint8_t iface_class = 0, iface_sub = 0, iface_proto = 0;
        XHCI_EVT_LOG("USB: Got full config descriptor (%u bytes) bNumInterfaces=%u\n",
            tot, cfg_full[4]);
        while (parsed + 2 <= tot) {
            uint8_t len = p[0];
            uint8_t type = p[1];
            if (len == 0)
                break;
            if (parsed + len > tot)
                break;
            if (type == 4 && len >= 9) {
                iface_class = p[5];
                iface_sub = p[6];
                iface_proto = p[7];
            } else if (type == 5 && len >= 7) {
                uint8_t epaddr = p[2];
                uint8_t attrs = p[3];
                uint16_t mps = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
                if ((attrs & 0x3) == 2) {
                    if (epaddr & 0x80) {
                        bulk_in_addr = epaddr;
                        bulk_in_mps = mps;
                    } else {
                        bulk_out_addr = epaddr;
                        bulk_out_mps = mps;
                    }
                }
            }
            parsed += len;
            p += len;
        }
        XHCI_EVT_LOG("USB: iface class=%02x sub=%02x proto=%02x bulk_in=%02x mps=%u bulk_out=%02x mps=%u\n",
            iface_class, iface_sub, iface_proto, bulk_in_addr, bulk_in_mps,
            bulk_out_addr, bulk_out_mps);
        ((usb_device_t *)dev)->class_code = iface_class;
        ((usb_device_t *)dev)->subclass = iface_sub;
        ((usb_device_t *)dev)->protocol = iface_proto;
        if (cfg_value && (bulk_in_addr || bulk_out_addr)) {
            usb_device_t *udev_cfg;
            ctrl->pending_xfer_stage = 5;
            if (ctrl->ep0_ring) {
                xhci_trb_t *base;
                xhci_trb_t *t;
                if (ctrl->ep0_ring_index >= 15) {
                    ctrl->ep0_ring_index = 0;
                    ctrl->ep0_ring_cycle ^= 1;
                }
                base = (xhci_trb_t *)ctrl->ep0_ring;
                t = &base[ctrl->ep0_ring_index];
                ctrl->ep0_stage_start = ctrl->ep0_ring_index;
                xhci_ep0_build_setup(ctrl, t, USB_REQTYPE_HOST_TO_DEVICE,
                    USB_REQ_SET_CONFIGURATION, cfg_value, 0, 0, 0);
                t[1].param_lo = 0;
                t[1].param_hi = 0;
                t[1].status = 0;
                t[1].control = 0;
                t[2].param_lo = 0;
                t[2].param_hi = 0;
                t[2].status = 0;
                t[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) |
                    (ctrl->ep0_ring_cycle & 1) | XHCI_TRB_IOC;
                ctrl->ep0_ring_index += 3;
                __asm__ __volatile__("mfence" ::: "memory");
                xhci_post_ep0_doorbell(ctrl,
                    ((usb_device_t *)dev)->slot_id ? ((usb_device_t *)dev)->slot_id : 1);
                XHCI_EVT_LOG("USB: Sent SET_CONFIGURATION %u\n", cfg_value);
                udev_cfg = (usb_device_t *)dev;
                udev_cfg->bulk_in_ep = bulk_in_addr & 0x0F;
                udev_cfg->bulk_out_ep = bulk_out_addr & 0x0F;
                udev_cfg->bulk_in_mps = bulk_in_mps;
                udev_cfg->bulk_out_mps = bulk_out_mps;
                xhci_configure_bulk_endpoints(ctrl, udev_cfg);
            }
        } else {
            ctrl->pending_xfer_stage = 0;
        }
    } else if (ctrl->pending_xfer_stage == 5) {
        usb_device_t *udev = dev;
        ctrl->pending_xfer_stage = 0;
        xhci_configure_bulk_endpoints(ctrl, udev);
    }
}

void xhci_issue_set_tr_dequeue(xhci_controller_t* ctrl, usb_device_t* dev, unsigned epid, uint64_t ring_phys, unsigned cycle)
{
    if(!ctrl || !dev || !epid || !ring_phys) {
        return;
    }
    /* TR Dequeue Ptr parameter: 64-bit pointer with DCS bit (bit0) and Stream ID (bits 31:16) zero */
    uint64_t ptr = (ring_phys & ~0xFULL) | (cycle & 1); /* ensure bits 3:1 zero, bit0=DCS */
    xhci_trb_t *trb = xhci_cmd_enqueue_nodoorbell(ctrl, XHCI_TRB_TYPE_SET_TR_DEQUEUE_POINTER, ptr);
    if(!trb) {
        return;
    }
    trb->status = 0; /* Stream ID = 0 */
    trb->control |= ((dev->slot_id & 0xFF) << 24) | ((epid & 0x1F) << 16);
    __asm__ __volatile__("mfence":::"memory");
    volatile uint32_t *db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0);
    *db0 = 0;
    XHCI_EVT_LOG("xhci: SET_TR_DEQ slot=%u epid=%u ptr=%p cycle=%u\n", dev->slot_id, epid, (void *)ptr, cycle & 1);
}

void xhci_issue_reset_endpoint(xhci_controller_t* ctrl, usb_device_t* dev, unsigned epid)
{
    if(!ctrl || !dev || !epid) {
        return;
    }
    xhci_trb_t *trb = xhci_cmd_enqueue_nodoorbell(ctrl, XHCI_TRB_TYPE_RESET_ENDPOINT, 0);
    if(!trb) {
        return;
    }
    trb->status = 0;
    trb->control |= ((dev->slot_id & 0xFF) << 24) | ((epid & 0x1F) << 16);
    __asm__ __volatile__("mfence":::"memory");
    volatile uint32_t *db0 = (volatile uint32_t *)(ctrl->doorbell_array + 0);
    *db0 = 0;
    XHCI_EVT_LOG("xhci: RESET_ENDPOINT slot=%u epid=%u\n", dev->slot_id, epid);
}
