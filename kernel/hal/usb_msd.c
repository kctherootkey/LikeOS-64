/* LikeOS-64 - USB Mass Storage evolving implementation */
#include "../../include/kernel/usb_msd.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/usb.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/xhci_trb.h"
#include "../../include/kernel/block.h"

/* Disable MSD debug logging by default for normal runs */
#undef XHCI_MSD_DEBUG
#define XHCI_MSD_DEBUG 0
#undef XHCI_MSD_LOG
#define XHCI_MSD_LOG(...) do { } while (0)

static block_device_t g_msd_block_dev; /* single device instance */
static void msd_issue_inquiry(xhci_controller_t *ctrl, usb_device_t *dev);
static void msd_issue_read_capacity(xhci_controller_t *ctrl, usb_device_t *dev);
static int msd_issue_read10(xhci_controller_t *ctrl, usb_device_t *dev,
        unsigned long lba, unsigned blocks);
static int msd_issue_write10(xhci_controller_t *ctrl, usb_device_t *dev,
    unsigned long lba, unsigned blocks, const void* buf);
static void msd_issue_request_sense(xhci_controller_t *ctrl, usb_device_t *dev);
static void msd_recover_reset(xhci_controller_t *ctrl, usb_device_t *dev);
static void msd_recover_clear_stalls(xhci_controller_t *ctrl, usb_device_t *dev);
static void msd_progress(xhci_controller_t *ctrl); /* forward declaration */
static void msd_wait_cmd_clear(xhci_controller_t *ctrl, unsigned max_iters);


static usb_device_t *find_msd(xhci_controller_t *ctrl)
{
    int s;
    for (s = 1; s < 64; ++s) {
        usb_device_t *dev = (usb_device_t *)ctrl->slot_device_map[s];
        if (!dev)
            continue;
        if (dev->configured && dev->class_code == USB_CLASS_MASS_STORAGE &&
            dev->bulk_in_ep && dev->bulk_out_ep)
            return dev;
    }
    return 0;
}

static int bot_send_cbw(xhci_controller_t *ctrl, usb_device_t *dev, usb_msd_cbw_t *cbw)
{
    unsigned i;
    if (!dev->endpoints_configured) {
        xhci_configure_mass_storage_endpoints(ctrl, dev);
        if (!dev->endpoints_configured)
            return -1;
    }
    if (!ctrl->msd_cbw_buf)
        ctrl->msd_cbw_buf = kcalloc(1, 64);
    if (!ctrl->msd_cbw_buf)
        return -1;
    if (sizeof(usb_msd_cbw_t) > 64) {
        kprintf("MSD: CBW size unexpected %u\n",
            (unsigned)sizeof(usb_msd_cbw_t));
        return -1;
    }
    for (i = 0; i < sizeof(usb_msd_cbw_t); ++i)
        ((uint8_t *)ctrl->msd_cbw_buf)[i] = ((uint8_t *)cbw)[i];
    /* Pre-track TRB slot BEFORE ringing doorbell to avoid race where event fires first */
    /* Handle wrapping: if enqueue_idx >= 15, we'll wrap to 0 inside xhci_enqueue_bulk_out */
    if (ctrl->bulk_out_ring) {
        unsigned pre_idx = ctrl->bulk_out_enqueue;
        if (pre_idx >= 15) {
            pre_idx = 0; /* Will wrap to index 0 */
        }
        ctrl->msd_cbw_trb = &((xhci_trb_t *)ctrl->bulk_out_ring)[pre_idx];
        ctrl->msd_cbw_phys = ctrl->bulk_out_ring_phys + pre_idx * sizeof(xhci_trb_t);
    }
    if (xhci_enqueue_bulk_out(ctrl, dev, ctrl->msd_cbw_buf, sizeof(usb_msd_cbw_t)) != ST_OK)
        return -1;
    XHCI_MSD_LOG("MSD: CBW queued tag=%u opcode=0x%02x data_len=%u flags=0x%02x cbw_trb=%p trb_phys=%p\n",
        cbw->tag, cbw->cb[0], cbw->data_len, cbw->flags,
        ctrl->msd_cbw_trb, (void *)ctrl->msd_cbw_phys);
    ctrl->msd_data_trb = 0;
    ctrl->msd_csw_trb = 0;
    ctrl->msd_data_phys = 0;
    ctrl->msd_csw_phys = 0;
    ctrl->msd_state = 1;
    ctrl->msd_pending_data_buf = 0;
    ctrl->msd_pending_data_len = 0;
    ctrl->msd_need_csw = 0;
    return 0;
}

// Ensure the bulk read buffer is large enough for the requested transfer.
static int msd_ensure_read_buf(xhci_controller_t *ctrl, unsigned bytes)
{
    if (ctrl->msd_read_buf && ctrl->msd_read_buf_len >= bytes)
        return ST_OK;
    if (ctrl->msd_read_buf)
        kfree(ctrl->msd_read_buf);
    ctrl->msd_read_buf = kcalloc(1, bytes);
    if (!ctrl->msd_read_buf) {
        ctrl->msd_read_buf_len = 0;
        return ST_NOMEM;
    }
    ctrl->msd_read_buf_len = bytes;
    return ST_OK;
}

// Ensure the bulk write buffer is large enough for the requested transfer.
static int msd_ensure_write_buf(xhci_controller_t *ctrl, unsigned bytes)
{
    if (ctrl->msd_write_buf && ctrl->msd_write_buf_len >= bytes)
        return ST_OK;
    if (ctrl->msd_write_buf)
        kfree(ctrl->msd_write_buf);
    ctrl->msd_write_buf = kcalloc(1, bytes);
    if (!ctrl->msd_write_buf) {
        ctrl->msd_write_buf_len = 0;
        return ST_NOMEM;
    }
    ctrl->msd_write_buf_len = bytes;
    return ST_OK;
}


static unsigned next_tag(xhci_controller_t *ctrl)
{
    if (ctrl->msd_tag_counter == 0 || ctrl->msd_tag_counter == 0xFFFFFFFFu)
        ctrl->msd_tag_counter = 1;
    else
        ctrl->msd_tag_counter++;
    return ctrl->msd_tag_counter;
}

static void msd_issue_inquiry(xhci_controller_t *ctrl, usb_device_t *dev)
{
    unsigned tag;
    int i;
    usb_msd_cbw_t cbw;
    if (ctrl->msd_op)
        return;
    for (i = 0; i < 16; i++)
        cbw.cb[i] = 0;
    tag = next_tag(ctrl);
    ctrl->msd_expected_tag = tag;
    cbw.signature = 0x43425355;
    cbw.tag = tag;
    cbw.data_len = 36;
    cbw.flags = 0x80;

    cbw.lun = 0;
    cbw.cb_len = 6;
    cbw.cb[0] = SCSI_INQUIRY;
    cbw.cb[4] = 36;
    if (!ctrl->msd_data_buf)
        ctrl->msd_data_buf = kcalloc(1, 512);
    if (!ctrl->msd_csw_buf)
        ctrl->msd_csw_buf = kcalloc(1, 64);
    if (bot_send_cbw(ctrl, dev, &cbw) == 0) {
        /* Pre-track data TRB before enqueue */
        if (ctrl->bulk_in_ring) {
            unsigned pre_idx = ctrl->bulk_in_enqueue % 16;
            ctrl->msd_data_trb = &((xhci_trb_t *)ctrl->bulk_in_ring)[pre_idx];
            ctrl->msd_data_phys = ctrl->bulk_in_ring_phys + pre_idx * sizeof(xhci_trb_t);
        }
        /* Set operation state BEFORE ringing doorbell so completion ISR sees correct state */
        ctrl->msd_expected_data_len = 36;
        ctrl->msd_state = 2; /* transition to data stage queued */
        ctrl->msd_op = 1;    /* INQUIRY */
        ctrl->msd_need_csw = 1;
        if (xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_data_buf, 36) == ST_OK) {
            XHCI_MSD_LOG("MSD: INQUIRY data IN queued len=%u data_trb=%p trb_phys=%p\n", 36, ctrl->msd_data_trb, (void *)ctrl->msd_data_phys);
        } else {
            /* Revert on failure */
            ctrl->msd_state = 1;
            ctrl->msd_op = 0;
            ctrl->msd_need_csw = 0;
            ctrl->msd_expected_data_len = 0;
        }
    }
}


static void msd_issue_request_sense(xhci_controller_t *ctrl, usb_device_t *dev)
{
    unsigned tag;
    int i;
    usb_msd_cbw_t cbw;
    if (ctrl->msd_op)
        return;
    for (i = 0; i < 16; i++)
        cbw.cb[i] = 0;
    tag = next_tag(ctrl);
    ctrl->msd_expected_tag = tag;
    cbw.signature = 0x43425355;
    cbw.tag = tag;
    cbw.data_len = 18;
    cbw.flags = 0x80;
    cbw.lun = 0;
    cbw.cb_len = 6;
    cbw.cb[0] = 0x03;
    cbw.cb[4] = 18;
    if (!ctrl->msd_data_buf)
        ctrl->msd_data_buf = kcalloc(1, 512);
    if (bot_send_cbw(ctrl, dev, &cbw) == 0) {
        if (ctrl->bulk_in_ring) {
            unsigned pre_idx = ctrl->bulk_in_enqueue % 16;
            ctrl->msd_data_trb = &((xhci_trb_t *)ctrl->bulk_in_ring)[pre_idx];
            ctrl->msd_data_phys = ctrl->bulk_in_ring_phys + pre_idx * sizeof(xhci_trb_t);
        }
        ctrl->msd_expected_data_len = 18;
        ctrl->msd_state = 2;
        ctrl->msd_op = 5; /* REQUEST SENSE */
        ctrl->msd_need_csw = 1;
        if (xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_data_buf, 18) == ST_OK) {
            XHCI_MSD_LOG("MSD: REQUEST SENSE data IN queued len=%u data_trb=%p trb_phys=%p\n", 18, ctrl->msd_data_trb, (void *)ctrl->msd_data_phys);
        } else {
            ctrl->msd_state = 1;
            ctrl->msd_op = 0;
            ctrl->msd_need_csw = 0;
            ctrl->msd_expected_data_len = 0;
        }
    }
}


static void msd_recover_clear_stalls(xhci_controller_t *ctrl, usb_device_t *dev)
{
    uint8_t eps[2];
    int epcount = 0;
    int i;
    if (!dev)
        return;
    if (dev->bulk_in_ep)
        eps[epcount++] = dev->bulk_in_ep;
    if (dev->bulk_out_ep)
        eps[epcount++] = dev->bulk_out_ep;
    for (i = 0; i < epcount; i++) {
        uint8_t epaddr = eps[i];
        uint8_t bmRequestType = 0x02;
        uint8_t bRequest = 1;
        uint16_t wValue = 0;
        uint16_t wIndex = epaddr;
        uint16_t wLength = 0;
        uint64_t setup_pkt = (uint64_t)bmRequestType |
            ((uint64_t)bRequest << 8) |
            ((uint64_t)wValue << 16) |
            ((uint64_t)wIndex << 32) |
            ((uint64_t)wLength << 48);
        if (ctrl->ep0_ring) {
            xhci_trb_t *trb = (xhci_trb_t *)ctrl->ep0_ring;
            trb[0].param_lo = (uint32_t)(setup_pkt & 0xFFFFFFFFu);
            trb[0].param_hi = (uint32_t)(setup_pkt >> 32);
            trb[0].status = 0;
            trb[0].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_SETUP_STAGE) | XHCI_TRB_CYCLE;
            trb[1].param_lo = 0;
            trb[1].param_hi = 0;
            trb[1].status = 0;
            trb[1].control = 0;
            trb[2].param_lo = 0;
            trb[2].param_hi = 0;
            trb[2].status = 0;
            trb[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) | XHCI_TRB_CYCLE;
            {
                volatile uint32_t *db_slot = (volatile uint32_t *)(ctrl->doorbell_array + 1 * 4);
                *db_slot = 0;
            }
            XHCI_MSD_LOG("MSD: CLEAR_FEATURE(HALT) ep=0x%02x issued\n", epaddr);
        }
    }
}

static void msd_recover_reset(xhci_controller_t *ctrl, usb_device_t *dev)
{
    uint8_t bmRequestType = 0x21;
    uint8_t bRequest = 0xFF;
    uint16_t wValue = 0;
    uint16_t wIndex = 0;
    uint16_t wLength = 0;
    uint64_t setup_pkt;
    if (!dev)
        return;
    setup_pkt = (uint64_t)bmRequestType |
        ((uint64_t)bRequest << 8) |
        ((uint64_t)wValue << 16) |
        ((uint64_t)wIndex << 32) |
        ((uint64_t)wLength << 48);
    if (ctrl->ep0_ring) {
        xhci_trb_t *trb = (xhci_trb_t *)ctrl->ep0_ring;
        trb[0].param_lo = (uint32_t)(setup_pkt & 0xFFFFFFFFu);
        trb[0].param_hi = (uint32_t)(setup_pkt >> 32);
        trb[0].status = 0;
        trb[0].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_SETUP_STAGE) | XHCI_TRB_CYCLE;
        trb[1].param_lo = 0;
        trb[1].param_hi = 0;
        trb[1].status = 0;
        trb[1].control = 0;
        trb[2].param_lo = 0;
        trb[2].param_hi = 0;
        trb[2].status = 0;
        trb[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) | XHCI_TRB_CYCLE;
        {
            volatile uint32_t *db_slot = (volatile uint32_t *)(ctrl->doorbell_array + 1 * 4);
            *db_slot = 0;
        }
    XHCI_MSD_LOG("MSD: BOT RESET issued\n");
    }
    msd_recover_clear_stalls(ctrl, dev);
    /* Don't touch the rings or endpoint configuration */
    /* Just clear the BOT protocol state - the retry logic will handle re-issuing commands */
    XHCI_MSD_LOG("MSD: BOT reset complete\n");
    
    /* Clear instrumentation so future attempts count fresh events */
    ctrl->msd_cbw_events = 0;
    ctrl->msd_data_events = 0;
    ctrl->msd_csw_events = 0;
    ctrl->msd_transfer_events = 0;
    ctrl->msd_bulk_transfer_events = 0;
    ctrl->msd_last_event_cc = 0;
    ctrl->msd_last_event_epid = 0;
    ctrl->msd_last_event_ptr = 0;
    /* Give the controller a moment to process reconfiguration before reissuing commands */
    msd_wait_cmd_clear(ctrl, 2048);
    /* After reset, drop stale TRB tracking to force fresh enqueue/use */
    ctrl->msd_cbw_trb = 0;
    ctrl->msd_data_trb = 0;
    ctrl->msd_csw_trb = 0;
    ctrl->msd_cbw_phys = 0;
    ctrl->msd_data_phys = 0;
    ctrl->msd_csw_phys = 0;
}

/* Poll command completions until pending_cmd_type clears or max_iters exhausted */
static void msd_wait_cmd_clear(xhci_controller_t *ctrl, unsigned max_iters)
{
    for (unsigned i = 0; i < max_iters; ++i) {
        if (!ctrl->pending_cmd_type)
            break;
        xhci_process_events(ctrl);
    }
}

static void msd_issue_read_capacity(xhci_controller_t *ctrl, usb_device_t *dev)
{
    unsigned tag;
    int i;
    usb_msd_cbw_t cbw;
    if (ctrl->msd_op)
        return;
    for (i = 0; i < 16; i++)
        cbw.cb[i] = 0;
    tag = next_tag(ctrl);
    ctrl->msd_expected_tag = tag;
    cbw.signature = 0x43425355;
    cbw.tag = tag;
    cbw.data_len = 8;
    cbw.flags = 0x80;
    cbw.lun = 0;
    cbw.cb_len = 10;
    cbw.cb[0] = SCSI_READ_CAPACITY_10;
    if (!ctrl->msd_data_buf)
        ctrl->msd_data_buf = kcalloc(1, 512);
    if (bot_send_cbw(ctrl, dev, &cbw) == 0) {
        if (ctrl->bulk_in_ring) {
            unsigned pre_idx = ctrl->bulk_in_enqueue % 16;
            ctrl->msd_data_trb = &((xhci_trb_t *)ctrl->bulk_in_ring)[pre_idx];
            ctrl->msd_data_phys = ctrl->bulk_in_ring_phys + pre_idx * sizeof(xhci_trb_t);
        }
        ctrl->msd_expected_data_len = 8;
        ctrl->msd_state = 2;
        ctrl->msd_op = 2; /* READ CAPACITY(10) */
        ctrl->msd_need_csw = 1;
        if (xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_data_buf, 8) == ST_OK) {
            XHCI_MSD_LOG("MSD: READ CAPACITY(10) data IN queued len=%u data_trb=%p trb_phys=%p\n", 8, ctrl->msd_data_trb, (void *)ctrl->msd_data_phys);
            XHCI_MSD_LOG("MSD: READ CAPACITY (10) issued\n");
        } else {
            ctrl->msd_state = 1;
            ctrl->msd_op = 0;
            ctrl->msd_need_csw = 0;
            ctrl->msd_expected_data_len = 0;
        }
    }
}

static int msd_issue_read10(xhci_controller_t *ctrl, usb_device_t *dev,
    unsigned long lba, unsigned blocks)
{
    unsigned bytes;
    unsigned tag;
    int i;
    usb_msd_cbw_t cbw;
    if (ctrl->msd_op || !ctrl->msd_ready)
        return ST_BUSY;
    bytes = blocks * ctrl->msd_block_size;
    if (msd_ensure_read_buf(ctrl, bytes) != ST_OK)
        return ST_NOMEM;
    for (i = 0; i < 16; i++)
        cbw.cb[i] = 0;
    tag = next_tag(ctrl);
    ctrl->msd_expected_tag = tag;
    cbw.signature = 0x43425355;
    cbw.tag = tag;
    cbw.data_len = bytes;
    cbw.flags = 0x80;
    cbw.lun = 0;
    cbw.cb_len = 10;
    cbw.cb[0] = 0x28;
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    cbw.cb[7] = (blocks >> 8) & 0xFF;
    cbw.cb[8] = blocks & 0xFF;
    if (bot_send_cbw(ctrl, dev, &cbw) == 0) {
        if (ctrl->bulk_in_ring) {
            unsigned pre_idx = ctrl->bulk_in_enqueue;
            if (pre_idx >= 15) {
                pre_idx = 0; /* Will wrap to index 0 */
            }
            ctrl->msd_data_trb = &((xhci_trb_t *)ctrl->bulk_in_ring)[pre_idx];
            ctrl->msd_data_phys = ctrl->bulk_in_ring_phys + pre_idx * sizeof(xhci_trb_t);
        }
        ctrl->msd_expected_data_len = bytes;
        ctrl->msd_state = 2;
        ctrl->msd_op = 3; /* READ(10) */
        ctrl->msd_need_csw = 1;
        ctrl->msd_read_lba = lba;
        ctrl->msd_read_blocks = blocks;
        ctrl->msd_read_result = 0;
        if (xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_read_buf, bytes) == ST_OK) {
            XHCI_MSD_LOG("MSD: READ(10) data IN queued len=%u data_trb=%p trb_phys=%p\n", bytes, ctrl->msd_data_trb, (void *)ctrl->msd_data_phys);
            return ST_OK;
        } else {
            ctrl->msd_state = 1;
            ctrl->msd_op = 0;
            ctrl->msd_need_csw = 0;
            ctrl->msd_expected_data_len = 0;
        }
    }
    return ST_ERR;
}

static int msd_issue_write10(xhci_controller_t *ctrl, usb_device_t *dev,
    unsigned long lba, unsigned blocks, const void* buf)
{
    unsigned bytes;
    unsigned tag;
    int i;
    usb_msd_cbw_t cbw;
    if (ctrl->msd_op || !ctrl->msd_ready)
        return ST_BUSY;
    bytes = blocks * ctrl->msd_block_size;
    if (msd_ensure_write_buf(ctrl, bytes) != ST_OK)
        return ST_NOMEM;
    if (buf && bytes) {
        mm_memcpy(ctrl->msd_write_buf, buf, bytes);
    }
    for (i = 0; i < 16; i++)
        cbw.cb[i] = 0;
    tag = next_tag(ctrl);
    ctrl->msd_expected_tag = tag;
    cbw.signature = 0x43425355;
    cbw.tag = tag;
    cbw.data_len = bytes;
    cbw.flags = 0x00; // OUT
    cbw.lun = 0;
    cbw.cb_len = 10;
    cbw.cb[0] = 0x2A; // WRITE(10)
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    cbw.cb[7] = (blocks >> 8) & 0xFF;
    cbw.cb[8] = blocks & 0xFF;
    if (bot_send_cbw(ctrl, dev, &cbw) == 0) {
        if (ctrl->bulk_out_ring) {
            unsigned pre_idx = ctrl->bulk_out_enqueue;
            if (pre_idx >= 15) {
                pre_idx = 0; /* Will wrap to index 0 */
            }
            ctrl->msd_data_trb = &((xhci_trb_t *)ctrl->bulk_out_ring)[pre_idx];
            ctrl->msd_data_phys = ctrl->bulk_out_ring_phys + pre_idx * sizeof(xhci_trb_t);
        }
        ctrl->msd_expected_data_len = bytes;
        ctrl->msd_state = 2;
        ctrl->msd_op = 6; /* WRITE(10) */
        ctrl->msd_need_csw = 1;
        ctrl->msd_write_lba = lba;
        ctrl->msd_write_blocks = blocks;
        ctrl->msd_write_result = 0;
        if (xhci_enqueue_bulk_out(ctrl, dev, ctrl->msd_write_buf, bytes) == ST_OK) {
            XHCI_MSD_LOG("MSD: WRITE(10) data OUT queued len=%u data_trb=%p trb_phys=%p\n", bytes, ctrl->msd_data_trb, (void *)ctrl->msd_data_phys);
            return ST_OK;
        } else {
            ctrl->msd_state = 1;
            ctrl->msd_op = 0;
            ctrl->msd_need_csw = 0;
            ctrl->msd_expected_data_len = 0;
        }
    }
    return ST_ERR;
}

static int msd_block_read(block_device_t *bdev, unsigned long lba,
    unsigned long count, void *buf)
{
    xhci_controller_t *ctrl = (xhci_controller_t *)bdev->driver_data;
    usb_device_t *dev = find_msd(ctrl);
    unsigned long start_poll;
    unsigned long start_overall;
    int st;
    if (!dev)
        return ST_INVALID;
    XHCI_MSD_LOG("MSD: block_read lba=%lu count=%lu\n", lba, count);
    start_overall = ctrl->msd_poll_counter;
    for (int attempt = 0; attempt < 3; ++attempt) {
        XHCI_MSD_LOG("MSD: read attempt %d state=%u op=%u\n", attempt + 1, ctrl->msd_state, ctrl->msd_op);
        st = msd_issue_read10(ctrl, dev, lba, count);
        if (st == ST_BUSY || st == ST_AGAIN) {
            /* Another operation in flight; process events and retry until a reasonable deadline */
            for (int i = 0; i < 512 && (ctrl->msd_op || ctrl->msd_state); ++i) {
                xhci_process_events(ctrl);
                msd_progress(ctrl);
            }
            XHCI_MSD_LOG("MSD: busy/again lba=%lu attempt=%d state=%u op=%u cbw_ev=%u data_ev=%u csw_ev=%u\n",
                lba, attempt + 1, ctrl->msd_state, ctrl->msd_op,
                ctrl->msd_cbw_events, ctrl->msd_data_events, ctrl->msd_csw_events);
            if (ctrl->msd_poll_counter - start_overall > 60000)
                return ST_IO;
            attempt--; /* don't count this as a full attempt */
            continue;
        } else if (st != ST_OK) {
            return st; /* propagate fatal conditions */
        }

        start_poll = ctrl->msd_poll_counter;
        while (1) {
            if (ctrl->msd_read_result > 0)
                break; /* success */
            if ((ctrl->msd_op != 3 && ctrl->msd_read_result == 0) || (ctrl->msd_op == 3 && ctrl->msd_state == 0)) {
                /* Operation ended without setting result (unexpected) */
                break;
            }
            if (ctrl->msd_op == 3 && ctrl->msd_state == 4 && ctrl->msd_read_result == 0) {
                /* CSW arrived; parse via msd_progress to set result */
                msd_progress(ctrl);
                if (ctrl->msd_read_result > 0)
                    break;
            }
            xhci_process_events(ctrl);
            msd_progress(ctrl);
            /* light backoff to avoid starving controller (acts like previous kprintf latency) */
            for (volatile int spin = 0; spin < 200; ++spin) __asm__ __volatile__("pause");
            if (ctrl->msd_poll_counter - start_poll > 25000) { /* extended timeout */
                XHCI_MSD_LOG("MSD: read timeout lba=%lu count=%lu (cbw_ev=%u data_ev=%u csw_ev=%u state=%u op=%u)\n",
                    lba, count, ctrl->msd_cbw_events, ctrl->msd_data_events,
                    ctrl->msd_csw_events, ctrl->msd_state, ctrl->msd_op);
                break;
            }
        }

        if (ctrl->msd_read_result > 0) {
            unsigned copy = (unsigned)ctrl->msd_read_result;
            __asm__ __volatile__("mfence" ::: "memory"); /* ensure data visible before copy */
            for (unsigned i = 0; i < copy; i++)
                ((uint8_t *)buf)[i] = ((uint8_t *)ctrl->msd_read_buf)[i];
            XHCI_MSD_LOG("MSD: read success lba=%lu count=%lu bytes=%u attempt=%d cbw_ev=%u data_ev=%u csw_ev=%u\n",
                lba, count, copy, attempt + 1,
                ctrl->msd_cbw_events, ctrl->msd_data_events, ctrl->msd_csw_events);
            return ST_OK;
        }

        /* failed this attempt; reset BOT state and retry */
        XHCI_MSD_LOG("MSD: read attempt %d failed result=%d state=%u op=%u cbw_ev=%u data_ev=%u csw_ev=%u\n",
            attempt + 1, ctrl->msd_read_result, ctrl->msd_state, ctrl->msd_op,
            ctrl->msd_cbw_events, ctrl->msd_data_events, ctrl->msd_csw_events);
        ctrl->msd_state = 0;
        ctrl->msd_op = 0;
        ctrl->msd_timeout_ticks = 0;
        ctrl->msd_read_result = 0;
        msd_recover_reset(ctrl, dev);
    }
    XHCI_MSD_LOG("MSD: read give up lba=%lu count=%lu\n", lba, count);
    return ST_IO;
}

static int msd_block_write(block_device_t *bdev, unsigned long lba,
    unsigned long count, const void *buf)
{
    xhci_controller_t *ctrl = (xhci_controller_t *)bdev->driver_data;
    usb_device_t *dev = find_msd(ctrl);
    unsigned long start_poll;
    unsigned long start_overall;
    int st;
    if (!dev)
        return ST_INVALID;
    XHCI_MSD_LOG("MSD: block_write lba=%lu count=%lu\n", lba, count);
    start_overall = ctrl->msd_poll_counter;
    for (int attempt = 0; attempt < 3; ++attempt) {
        XHCI_MSD_LOG("MSD: write attempt %d state=%u op=%u\n", attempt + 1, ctrl->msd_state, ctrl->msd_op);
        st = msd_issue_write10(ctrl, dev, lba, count, buf);
        if (st == ST_BUSY || st == ST_AGAIN) {
            for (int i = 0; i < 512 && (ctrl->msd_op || ctrl->msd_state); ++i) {
                xhci_process_events(ctrl);
                msd_progress(ctrl);
            }
            if (ctrl->msd_poll_counter - start_overall > 60000)
                return ST_IO;
            attempt--;
            continue;
        } else if (st != ST_OK) {
            return st;
        }

        start_poll = ctrl->msd_poll_counter;
        while (1) {
            if (ctrl->msd_write_result > 0)
                break;
            if ((ctrl->msd_op != 6 && ctrl->msd_write_result == 0) || (ctrl->msd_op == 6 && ctrl->msd_state == 0)) {
                break;
            }
            if (ctrl->msd_op == 6 && ctrl->msd_state == 4 && ctrl->msd_write_result == 0) {
                msd_progress(ctrl);
                if (ctrl->msd_write_result > 0)
                    break;
            }
            xhci_process_events(ctrl);
            msd_progress(ctrl);
            for (volatile int spin = 0; spin < 200; ++spin) __asm__ __volatile__("pause");
            if (ctrl->msd_poll_counter - start_poll > 25000) {
                break;
            }
        }

        if (ctrl->msd_write_result > 0) {
            XHCI_MSD_LOG("MSD: write success lba=%lu count=%lu bytes=%u attempt=%d\n",
                lba, count, (unsigned)ctrl->msd_write_result, attempt + 1);
            return ST_OK;
        }

        ctrl->msd_state = 0;
        ctrl->msd_op = 0;
        ctrl->msd_timeout_ticks = 0;
        ctrl->msd_write_result = 0;
        msd_recover_reset(ctrl, dev);
    }
    return ST_IO;
}

static void msd_progress(xhci_controller_t *ctrl)
{
    usb_device_t *dev = find_msd(ctrl);
    if (!dev)
        return;
    if (!dev->endpoints_configured) {
        int cr = xhci_configure_mass_storage_endpoints(ctrl, dev);
        if (cr != ST_OK)
            return;
    }
    ctrl->msd_poll_counter++;
    if (ctrl->msd_op && ctrl->msd_timeout_ticks) {
        if (ctrl->msd_poll_counter - ctrl->msd_op_start_tick >
            ctrl->msd_timeout_ticks) {
            XHCI_MSD_LOG("MSD: Operation %u timeout (state=%u reset_count=%u) -> reset\n", ctrl->msd_op, ctrl->msd_state, ctrl->msd_reset_count);
            XHCI_MSD_LOG("  Debug: cbw_ev=%u data_ev=%u csw_ev=%u cbw_phys=%p data_phys=%p csw_phys=%p last_evt_ptr=%p last_cc=%u last_epid=%u\n",
                ctrl->msd_cbw_events, ctrl->msd_data_events, ctrl->msd_csw_events,
                (void *)ctrl->msd_cbw_phys, (void *)ctrl->msd_data_phys, (void *)ctrl->msd_csw_phys,
                (void *)ctrl->msd_last_event_ptr, ctrl->msd_last_event_cc, ctrl->msd_last_event_epid);
            if(ctrl->msd_reset_count < 5) {
                msd_recover_reset(ctrl, dev);
            } else {
                XHCI_MSD_LOG("MSD: giving up further resets\n");
            }
            ctrl->msd_state = 0;
            ctrl->msd_op = 0;
            ctrl->msd_timeout_ticks = 0;
        }
    }
    if (!ctrl->msd_op) {
        if (!ctrl->msd_ready) {
            XHCI_MSD_LOG("MSD: issuing INQUIRY (poll_counter=%lu)\n", ctrl->msd_poll_counter);
            msd_issue_inquiry(ctrl, dev);
            ctrl->msd_op_start_tick = ctrl->msd_poll_counter;
            ctrl->msd_timeout_ticks = 5000;
        } else {
            return;
        }
    }
    if (ctrl->msd_op == 1 && ctrl->msd_state == 4) {
        int i;
        char vendor[9];
        char product[17];
        for (i = 0; i < 8; i++) {
            vendor[i] = ((char *)ctrl->msd_data_buf)[8 + i];
            if (vendor[i] < ' ' || vendor[i] > '~')
                vendor[i] = ' ';
        }
        vendor[8] = '\0';
        for (i = 0; i < 16; i++) {
            product[i] = ((char *)ctrl->msd_data_buf)[16 + i];
            if (product[i] < ' ' || product[i] > '~')
                product[i] = ' ';
        }
        product[16] = '\0';
    XHCI_MSD_LOG("MSD: INQUIRY vendor='%s' product='%s'\n", vendor, product);
        ctrl->msd_state = 0;
        ctrl->msd_op = 0;
        msd_issue_read_capacity(ctrl, dev);
        ctrl->msd_op_start_tick = ctrl->msd_poll_counter;
        ctrl->msd_timeout_ticks = 500;
    }
    if (ctrl->msd_op == 2 && ctrl->msd_state == 4) {
        uint8_t *d = (uint8_t *)ctrl->msd_data_buf;
        unsigned long last_lba = ((unsigned long)d[0] << 24) |
            ((unsigned long)d[1] << 16) |
            ((unsigned long)d[2] << 8) |
            (unsigned long)d[3];
        unsigned int blen = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
        ctrl->msd_capacity_blocks = last_lba + 1;
        ctrl->msd_block_size = blen;
        ctrl->msd_ready = 1;
    XHCI_MSD_LOG("MSD: Capacity blocks=%lu block_size=%u (~%lu KB)\n",
            ctrl->msd_capacity_blocks, blen,
            (ctrl->msd_capacity_blocks * (unsigned long)blen) / 1024);
        /* Immediately read and hex dump sector 0 for diagnostics */
        if (msd_ensure_read_buf(ctrl, 512) == ST_OK) {
            /* Synchronously issue a READ(10) of first sector (1 block) */
            if (msd_issue_read10(ctrl, dev, 0, 1) == ST_OK) {
                unsigned long start_poll = ctrl->msd_poll_counter;
                while (ctrl->msd_op == 3 && ctrl->msd_state != 4 && (ctrl->msd_poll_counter - start_poll < 10000)) {
                    xhci_process_events(ctrl);
                    /* fast path; don't call msd_progress recursively for nested read until done */
                    ctrl->msd_poll_counter++; /* emulate tick */
                }
                if (ctrl->msd_state == 4 && ctrl->msd_read_result > 0) {
                    unsigned dump = ctrl->msd_read_result;
                    if (dump > 512) dump = 512;
                    XHCI_MSD_LOG("MSD: Sector0 dump (first %u bytes):\n", dump);
                    for (unsigned i = 0; i < dump; i += 16) {
                        XHCI_MSD_LOG("  %03x: ", i);
                        for (unsigned j = 0; j < 16 && (i + j) < dump; j++) {
                            XHCI_MSD_LOG("%02x ", ((uint8_t*)ctrl->msd_read_buf)[i + j]);
                        }
                        XHCI_MSD_LOG(" | ");
                        for (unsigned j = 0; j < 16 && (i + j) < dump; j++) {
                            uint8_t c = ((uint8_t*)ctrl->msd_read_buf)[i + j];
                            if (c < 32 || c > 126) c = '.';
                            XHCI_MSD_LOG("%c", c);
                        }
                        XHCI_MSD_LOG("\n");
                    }
                } else {
                    XHCI_MSD_LOG("MSD: Sector0 read failed (state=%u op=%u result=%d)\n", ctrl->msd_state, ctrl->msd_op, ctrl->msd_read_result);
                }
                /* Reset read state after diagnostic read */
                ctrl->msd_state = 0;
                ctrl->msd_op = 0;
                ctrl->msd_timeout_ticks = 0;
            }
        }
        if (!g_msd_block_dev.name) {
            g_msd_block_dev.name = "usb0";
            g_msd_block_dev.sector_size = blen;
            g_msd_block_dev.total_sectors = ctrl->msd_capacity_blocks;
            g_msd_block_dev.read = msd_block_read;
            g_msd_block_dev.write = msd_block_write;
            g_msd_block_dev.driver_data = ctrl;
            if (block_register(&g_msd_block_dev) == ST_OK)
                XHCI_MSD_LOG("MSD: Registered block device 'usb0'\n");
        }
        ctrl->msd_state = 0;
        ctrl->msd_op = 0;
        ctrl->msd_timeout_ticks = 0;
    }
    if (ctrl->msd_op == 3 && ctrl->msd_state == 4) {
        usb_msd_csw_t *csw = (usb_msd_csw_t *)ctrl->msd_csw_buf;
        int tag_ok = (csw->tag == ctrl->msd_expected_tag);
        int sig_ok = (csw->signature == 0x53425355);
        if (!sig_ok || !tag_ok) {
            XHCI_MSD_LOG("MSD: CSW invalid sig_ok=%d tag_ok=%d exp_tag=%u got=%u -> reset\n",
                sig_ok, tag_ok, ctrl->msd_expected_tag, csw->tag);
            ctrl->msd_read_result = -1;
            msd_recover_reset(ctrl, dev);
            ctrl->msd_state = 0;
            ctrl->msd_op = 0;
            return;
        }
        if (csw->status == 0) {
            if (csw->residue)
                XHCI_MSD_LOG("MSD: READ(10) residue %u (expected %u)\n",
                    csw->residue, ctrl->msd_expected_data_len);
            ctrl->msd_read_result = ctrl->msd_expected_data_len - csw->residue;
        } else {
            ctrl->msd_read_result = -1;
            XHCI_MSD_LOG("MSD: READ(10) CSW status=%u residue=%u\n",
                csw->status, csw->residue);
            if (ctrl->msd_retry_count < 3) {
                ctrl->msd_retry_count++;
                XHCI_MSD_LOG("MSD: Retrying READ(10) attempt %u\n",
                    ctrl->msd_retry_count);
                ctrl->msd_state = 0;
                ctrl->msd_op = 0;
                msd_issue_read10(ctrl, dev, ctrl->msd_read_lba,
                    ctrl->msd_read_blocks);
                ctrl->msd_op_start_tick = ctrl->msd_poll_counter;
                ctrl->msd_timeout_ticks = 2000;
                return;
            } else {
                msd_recover_reset(ctrl, dev);
            }
        }
        ctrl->msd_state = 0;
        ctrl->msd_op = 0;
        ctrl->msd_timeout_ticks = 0;
    }
    if (ctrl->msd_op == 6 && ctrl->msd_state == 4) {
        usb_msd_csw_t *csw = (usb_msd_csw_t *)ctrl->msd_csw_buf;
        int tag_ok = (csw->tag == ctrl->msd_expected_tag);
        int sig_ok = (csw->signature == 0x53425355);
        if (!sig_ok || !tag_ok) {
            XHCI_MSD_LOG("MSD: WRITE(10) CSW invalid -> reset\n");
            ctrl->msd_write_result = -1;
            msd_recover_reset(ctrl, dev);
            ctrl->msd_state = 0;
            ctrl->msd_op = 0;
            return;
        }
        if (csw->status == 0) {
            if (csw->residue)
                XHCI_MSD_LOG("MSD: WRITE(10) residue %u (expected %u)\n",
                    csw->residue, ctrl->msd_expected_data_len);
            ctrl->msd_write_result = ctrl->msd_expected_data_len - csw->residue;
        } else {
            ctrl->msd_write_result = -1;
            XHCI_MSD_LOG("MSD: WRITE(10) CSW status=%u residue=%u\n",
                csw->status, csw->residue);
        }
        ctrl->msd_state = 0;
        ctrl->msd_op = 0;
        ctrl->msd_timeout_ticks = 0;
    }
    if (ctrl->msd_op == 4 && ctrl->msd_state == 4) {
        usb_msd_csw_t *csw = (usb_msd_csw_t *)ctrl->msd_csw_buf;
        int tag_ok = (csw->tag == ctrl->msd_expected_tag);
        int sig_ok = (csw->signature == 0x53425355);
        if (!sig_ok || !tag_ok) {
            XHCI_MSD_LOG("MSD: TUR CSW invalid -> reset\n");
            msd_recover_reset(ctrl, dev);
            ctrl->msd_state = 0;
            ctrl->msd_op = 0;
            return;
        }
        if (csw->status != 0) {
            XHCI_MSD_LOG("MSD: TEST UNIT READY failed status=%u -> REQUEST SENSE\n",
                csw->status);
            ctrl->msd_state = 0;
            ctrl->msd_op = 0;
            msd_issue_request_sense(ctrl, dev);
            ctrl->msd_op_start_tick = ctrl->msd_poll_counter;
            ctrl->msd_timeout_ticks = 200;
        } else {
            ctrl->msd_state = 0;
            ctrl->msd_op = 0;
        }
    }
    if (ctrl->msd_op == 5 && ctrl->msd_state == 4) {
        uint8_t *s = (uint8_t *)ctrl->msd_data_buf;
        uint8_t key = s[2] & 0x0F;
        uint8_t asc = s[12];
        uint8_t ascq = s[13];
        ctrl->msd_last_sense_key = key;
        ctrl->msd_last_sense_asc = asc;
        ctrl->msd_last_sense_ascq = ascq;
    XHCI_MSD_LOG("MSD: SENSE key=%u asc=%02x ascq=%02x\n", key, asc, ascq);
        if (key == 0x02) {
            if (ctrl->msd_retry_count < 5) {
                ctrl->msd_retry_count++;
                XHCI_MSD_LOG("MSD: Not ready, retry #%u TUR backoff\n",
                    ctrl->msd_retry_count);
            }
            ctrl->msd_backoff_until = ctrl->msd_poll_counter +
                (500 * (ctrl->msd_retry_count ? ctrl->msd_retry_count : 1));
        } else if (key == 0x06) {
            XHCI_MSD_LOG("MSD: Unit attention, resetting retry counter\n");
            ctrl->msd_retry_count = 0;
            ctrl->msd_backoff_until = 0;
        } else if (key) {
            if (ctrl->msd_reset_count < 2) {
                ctrl->msd_reset_count++;
                msd_recover_reset(ctrl, dev);
            }
        }
        ctrl->msd_state = 0;
        ctrl->msd_op = 0;
        ctrl->msd_timeout_ticks = 0;
    }
}

void usb_msd_poll(void *ctrl_ptr)
{
    xhci_controller_t *ctrl = (xhci_controller_t *)ctrl_ptr;
    msd_progress(ctrl);
}

int usb_msd_try_init(void *ctrl_ptr)
{
    xhci_controller_t *ctrl = (xhci_controller_t *)ctrl_ptr;
    int s;
    for (s = 1; s < 64; ++s) {
        usb_device_t *dev = (usb_device_t *)ctrl->slot_device_map[s];
        if (!dev)
            continue;
        if (dev->class_code == USB_CLASS_MASS_STORAGE &&
            dev->bulk_in_ep && dev->bulk_out_ep) {
            XHCI_MSD_LOG("MSD: Ready (slot %d vid=%04x pid=%04x) - BOT not yet implemented\n",
                s, dev->vid, dev->pid);
            return 0;
        }
    }
    return -1;
}
