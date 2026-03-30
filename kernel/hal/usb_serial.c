// LikeOS-64 - USB serial logging backend

#include "../../include/kernel/usb_serial.h"

#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/usb.h"

#ifndef USB_SERIAL_ENABLED

void usbserial_init(void) {}

int usbserial_probe(xhci_controller_t* ctrl, usb_device_t* dev,
                    uint8_t* config_desc, uint16_t config_len)
{
    (void)ctrl;
    (void)dev;
    (void)config_desc;
    (void)config_len;
    return ST_UNSUPPORTED;
}

void usbserial_disconnect(usb_device_t* dev)
{
    (void)dev;
}

void usbserial_log_write(const char* data, uint32_t len)
{
    (void)data;
    (void)len;
}

int usbserial_is_active(void)
{
    return 0;
}

#else

#define USB_CLASS_COMMUNICATIONS      0x02
#define USB_CLASS_CDC_DATA            0x0A

#define USB_CDC_DESC_CS_INTERFACE     0x24
#define USB_CDC_DESC_SUBTYPE_ACM      0x02
#define USB_CDC_DESC_SUBTYPE_UNION    0x06

#define USB_CDC_REQ_SET_LINE_CODING        0x20
#define USB_CDC_REQ_SET_CONTROL_LINE_STATE 0x22

#define USB_CDC_LINE_CODING_BAUD 115200U
#define USB_CDC_TX_CHUNK         64U

// Prolific PL2303 vendor-specific definitions
#define PL2303_VENDOR_ID              0x067b
#define PL2303_PRODUCT_ID_CLASSIC     0x2303
#define PL2303_PRODUCT_ID_HXD         0x23a3
#define PL2303_PRODUCT_ID_EA          0x23b3
#define PL2303_PRODUCT_ID_TA          0x23c3
#define PL2303_PRODUCT_ID_TB          0x23d3
#define PL2303_PRODUCT_ID_SA          0x23e3
#define PL2303_PRODUCT_ID_GB          0x23f3

#define PL2303_VENDOR_WRITE_REQUEST   0x01
#define PL2303_VENDOR_READ_REQUEST    0x01

#define PL2303_SET_LINE_CODING        0x20
#define PL2303_GET_LINE_CODING        0x21
#define PL2303_SET_CONTROL_LINE_STATE 0x22

#define PL2303_CTRL_DTR               0x01
#define PL2303_CTRL_RTS               0x02

typedef struct __attribute__((packed)) {
    uint32_t baud;
    uint8_t stop_bits;
    uint8_t parity;
    uint8_t data_bits;
} usb_cdc_line_coding_t;

typedef struct {
    uint8_t comm_if;
    uint8_t data_if;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint16_t bulk_in_max_pkt;
    uint16_t bulk_out_max_pkt;
    uint8_t has_acm_desc;
    uint8_t valid;
} usbserial_match_t;

typedef struct {
    xhci_controller_t* ctrl;
    usb_device_t* dev;
    uint8_t comm_if;
    uint8_t data_if;
    uint8_t initializing;
    uint8_t active;
    uint8_t mirror_enabled;
    uint8_t write_busy;
} usbserial_state_t;

static spinlock_t g_usbserial_lock = SPINLOCK_INIT("usbserial");
static usbserial_state_t g_usbserial;

// Forward declarations
static int usbserial_activate_pl2303(xhci_controller_t* ctrl, usb_device_t* dev,
                                     const usbserial_match_t* match);

static void usbserial_clear_state(void)
{
    g_usbserial.ctrl = NULL;
    g_usbserial.dev = NULL;
    g_usbserial.comm_if = 0xFF;
    g_usbserial.data_if = 0xFF;
    g_usbserial.initializing = 0;
    g_usbserial.active = 0;
    g_usbserial.mirror_enabled = 0;
    g_usbserial.write_busy = 0;
}

void usbserial_init(void)
{
    uint64_t flags;

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    usbserial_clear_state();
    spin_unlock_irqrestore(&g_usbserial_lock, flags);
}

// ==========================================================================
// Prolific PL2303 support
// ==========================================================================

static int usbserial_is_pl2303(usb_device_t* dev)
{
    if (dev->vendor_id != PL2303_VENDOR_ID) {
        return 0;
    }
    switch (dev->product_id) {
    case PL2303_PRODUCT_ID_CLASSIC:
    case PL2303_PRODUCT_ID_HXD:
    case PL2303_PRODUCT_ID_EA:
    case PL2303_PRODUCT_ID_TA:
    case PL2303_PRODUCT_ID_TB:
    case PL2303_PRODUCT_ID_SA:
    case PL2303_PRODUCT_ID_GB:
        return 1;
    default:
        return 0;
    }
}

static int pl2303_vendor_read(xhci_controller_t* ctrl, usb_device_t* dev,
                              uint16_t value, uint8_t* buf)
{
    return xhci_control_transfer(ctrl, dev,
                                USB_RT_D2H | USB_RT_VENDOR | USB_RT_DEV,
                                PL2303_VENDOR_READ_REQUEST,
                                value, 0, 1, buf);
}

static int pl2303_vendor_write(xhci_controller_t* ctrl, usb_device_t* dev,
                               uint16_t value, uint16_t index)
{
    return xhci_control_transfer(ctrl, dev,
                                USB_RT_H2D | USB_RT_VENDOR | USB_RT_DEV,
                                PL2303_VENDOR_WRITE_REQUEST,
                                value, index, 0, NULL);
}

static int pl2303_init_device(xhci_controller_t* ctrl, usb_device_t* dev)
{
    // Allocate DMA-safe page-aligned buffer for vendor reads and line coding
    uint8_t* raw_buf = (uint8_t*)kcalloc_dma(1, 4096 + 4096);
    if (!raw_buf) {
        kprintf("[USBSER] PL2303: DMA alloc failed\n");
        return ST_ERR;
    }
    uint8_t* dma_buf = (uint8_t*)(((uint64_t)raw_buf + 4095) & ~4095ULL);

    // Legacy vendor init sequence - only for classic PL2303 (PID 0x2303)
    // HXN variants (PID 0x23a3+) do NOT support these and will stall EP0
    if (dev->product_id == PL2303_PRODUCT_ID_CLASSIC) {
        pl2303_vendor_read(ctrl, dev, 0x8484, dma_buf);
        pl2303_vendor_write(ctrl, dev, 0x0404, 0);
        pl2303_vendor_read(ctrl, dev, 0x8484, dma_buf);
        pl2303_vendor_read(ctrl, dev, 0x8383, dma_buf);
        pl2303_vendor_read(ctrl, dev, 0x8484, dma_buf);
        pl2303_vendor_write(ctrl, dev, 0x0404, 1);
        pl2303_vendor_read(ctrl, dev, 0x8484, dma_buf);
        pl2303_vendor_read(ctrl, dev, 0x8383, dma_buf);
        pl2303_vendor_write(ctrl, dev, 0, 1);
        pl2303_vendor_write(ctrl, dev, 1, 0);
        pl2303_vendor_write(ctrl, dev, 2, 0x24);
    }

    // Set line coding: 115200 8N1 (using DMA-safe buffer)
    usb_cdc_line_coding_t* lc = (usb_cdc_line_coding_t*)dma_buf;
    kmemset(dma_buf, 0, sizeof(usb_cdc_line_coding_t));
    lc->baud = USB_CDC_LINE_CODING_BAUD;
    lc->stop_bits = 0;
    lc->parity = 0;
    lc->data_bits = 8;

    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_H2D | USB_RT_CLASS | USB_RT_IFACE,
                              PL2303_SET_LINE_CODING,
                              0, 0,
                              sizeof(usb_cdc_line_coding_t), lc) != ST_OK) {
        kprintf("[USBSER] PL2303: SET_LINE_CODING failed\n");
        kfree_dma(raw_buf);
        return ST_ERR;
    }

    // Set control line state: DTR + RTS
    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_H2D | USB_RT_CLASS | USB_RT_IFACE,
                              PL2303_SET_CONTROL_LINE_STATE,
                              PL2303_CTRL_DTR | PL2303_CTRL_RTS, 0,
                              0, NULL) != ST_OK) {
        kprintf("[USBSER] PL2303: SET_CONTROL_LINE_STATE failed\n");
        kfree_dma(raw_buf);
        return ST_ERR;
    }

    kfree_dma(raw_buf);
    return ST_OK;
}

static int usbserial_probe_pl2303(xhci_controller_t* ctrl, usb_device_t* dev)
{
    usbserial_match_t match;

    if (!usbserial_is_pl2303(dev)) {
        return ST_UNSUPPORTED;
    }

    kprintf("[USBSER] PL2303 detected (VID=%04x PID=%04x)\n",
            dev->vendor_id, dev->product_id);

    // PL2303 uses interface 0 with vendor-specific class
    kmemset(&match, 0, sizeof(match));
    match.comm_if = 0;
    match.data_if = 0;
    match.bulk_in_ep = dev->bulk_in_ep;
    match.bulk_out_ep = dev->bulk_out_ep;
    match.bulk_in_max_pkt = dev->bulk_in_max_pkt;
    match.bulk_out_max_pkt = dev->bulk_out_max_pkt;
    match.valid = 1;

    if (!match.bulk_in_ep || !match.bulk_out_ep) {
        kprintf("[USBSER] PL2303: missing bulk endpoints (in=%d out=%d)\n",
                match.bulk_in_ep, match.bulk_out_ep);
        return ST_UNSUPPORTED;
    }

    if (!dev->bulk_in_ring || !dev->bulk_out_ring) {
        kprintf("[USBSER] PL2303: bulk rings not configured\n");
        return ST_AGAIN;
    }

    kprintf("[USBSER] PL2303: initializing (EP in=%d out=%d)\n",
            match.bulk_in_ep, match.bulk_out_ep);

    if (pl2303_init_device(ctrl, dev) != ST_OK) {
        kprintf("[USBSER] PL2303: init failed\n");
        return ST_ERR;
    }

    kprintf("[USBSER] PL2303: init OK, activating serial logging\n");
    return usbserial_activate_pl2303(ctrl, dev, &match);
}

// ==========================================================================
// CDC ACM support
// ==========================================================================

static int usbserial_parse_cdc_acm(uint8_t* config_desc, uint16_t config_len,
                                   usbserial_match_t* match)
{
    uint8_t first_comm = 0xFF;
    uint8_t first_data = 0xFF;
    uint8_t union_master = 0xFF;
    uint8_t union_slave = 0xFF;
    uint8_t acm_iface = 0xFF;
    uint8_t current_iface = 0xFF;
    uint8_t current_class = 0xFF;
    uint8_t* ptr = config_desc;
    uint8_t* end = config_desc + config_len;

    while (ptr + 1 < end) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];

        if (len == 0 || ptr + len > end) {
            break;
        }

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
            usb_interface_desc_t* iface = (usb_interface_desc_t*)ptr;
            current_iface = iface->interface_num;
            current_class = iface->class_code;
            if (iface->class_code == USB_CLASS_COMMUNICATIONS && first_comm == 0xFF) {
                first_comm = iface->interface_num;
            }
            if (iface->class_code == USB_CLASS_CDC_DATA && first_data == 0xFF) {
                first_data = iface->interface_num;
            }
        } else if (type == USB_CDC_DESC_CS_INTERFACE && len >= 3 && current_iface != 0xFF) {
            if (ptr[2] == USB_CDC_DESC_SUBTYPE_ACM && current_class == USB_CLASS_COMMUNICATIONS) {
                acm_iface = current_iface;
            } else if (ptr[2] == USB_CDC_DESC_SUBTYPE_UNION && len >= 5) {
                union_master = ptr[3];
                union_slave = ptr[4];
            }
        }

        ptr += len;
    }

    if (acm_iface != 0xFF) {
        match->has_acm_desc = 1;
    }

    match->comm_if = (union_master != 0xFF) ? union_master : first_comm;
    match->data_if = (union_slave != 0xFF) ? union_slave : first_data;

    if (match->comm_if == 0xFF || match->data_if == 0xFF) {
        return ST_UNSUPPORTED;
    }

    ptr = config_desc;
    current_iface = 0xFF;
    while (ptr + 1 < end) {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];

        if (len == 0 || ptr + len > end) {
            break;
        }

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_desc_t)) {
            usb_interface_desc_t* iface = (usb_interface_desc_t*)ptr;
            current_iface = iface->interface_num;
        } else if (type == USB_DESC_ENDPOINT && len >= sizeof(usb_endpoint_desc_t)) {
            usb_endpoint_desc_t* ep = (usb_endpoint_desc_t*)ptr;
            uint8_t ep_num = ep->address & USB_EP_NUM_MASK;
            uint8_t ep_in = (ep->address & USB_EP_DIR_IN) ? 1 : 0;
            uint8_t ep_type = ep->attributes & USB_EP_TYPE_MASK;

            if (current_iface == match->data_if && ep_type == USB_EP_TYPE_BULK) {
                if (ep_in) {
                    match->bulk_in_ep = ep_num;
                    match->bulk_in_max_pkt = ep->max_packet;
                } else {
                    match->bulk_out_ep = ep_num;
                    match->bulk_out_max_pkt = ep->max_packet;
                }
            }
        }

        ptr += len;
    }

    if (!match->bulk_in_ep || !match->bulk_out_ep) {
        return ST_UNSUPPORTED;
    }

    match->valid = 1;
    return ST_OK;
}

static int usbserial_send_control_setup(xhci_controller_t* ctrl, usb_device_t* dev,
                                        uint8_t comm_if)
{
    usb_cdc_line_coding_t line_coding;

    line_coding.baud = USB_CDC_LINE_CODING_BAUD;
    line_coding.stop_bits = 0;
    line_coding.parity = 0;
    line_coding.data_bits = 8;

    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_H2D | USB_RT_CLASS | USB_RT_IFACE,
                              USB_CDC_REQ_SET_LINE_CODING,
                              0, comm_if,
                              sizeof(line_coding), &line_coding) != ST_OK) {
        return ST_ERR;
    }

    if (xhci_control_transfer(ctrl, dev,
                              USB_RT_H2D | USB_RT_CLASS | USB_RT_IFACE,
                              USB_CDC_REQ_SET_CONTROL_LINE_STATE,
                              0x0003, comm_if,
                              0, NULL) != ST_OK) {
        return ST_ERR;
    }

    return ST_OK;
}

static int usbserial_write_raw(xhci_controller_t* ctrl, usb_device_t* dev,
                               const char* data, uint32_t len)
{
    uint32_t transferred = 0;
    uint32_t offset = 0;

    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > USB_CDC_TX_CHUNK) {
            chunk = USB_CDC_TX_CHUNK;
        }

        if (xhci_bulk_transfer_out(ctrl, dev, data + offset, chunk, &transferred) != ST_OK) {
            return ST_IO;
        }

        offset += chunk;
    }

    return ST_OK;
}

static int usbserial_write_text(xhci_controller_t* ctrl, usb_device_t* dev,
                                const char* data, uint32_t len)
{
    char txbuf[USB_CDC_TX_CHUNK * 2];
    uint32_t txlen = 0;

    for (uint32_t index = 0; index < len; ++index) {
        char ch = data[index];

        if (ch == '\n') {
            if (txlen == sizeof(txbuf)) {
                if (usbserial_write_raw(ctrl, dev, txbuf, txlen) != ST_OK) {
                    return ST_IO;
                }
                txlen = 0;
            }
            txbuf[txlen++] = '\r';
        }

        if (txlen == sizeof(txbuf)) {
            if (usbserial_write_raw(ctrl, dev, txbuf, txlen) != ST_OK) {
                return ST_IO;
            }
            txlen = 0;
        }

        txbuf[txlen++] = ch;
    }

    if (txlen != 0) {
        return usbserial_write_raw(ctrl, dev, txbuf, txlen);
    }

    return ST_OK;
}

static void usbserial_send_existing_klog(xhci_controller_t* ctrl, usb_device_t* dev)
{
    int size = klog_size();
    if (size <= 0) {
        return;
    }

    char* buffer = (char*)kalloc((size_t)size);
    if (!buffer) {
        return;
    }

    int got = klog_read(buffer, size);
    if (got > 0) {
        usbserial_write_text(ctrl, dev, buffer, (uint32_t)got);
    }

    kfree(buffer);
}

// ==========================================================================
// Common activation path (shared by CDC ACM and PL2303)
// ==========================================================================

static int usbserial_activate_common(xhci_controller_t* ctrl, usb_device_t* dev,
                                     const usbserial_match_t* match)
{
    uint64_t flags;

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    if (g_usbserial.active || g_usbserial.initializing) {
        spin_unlock_irqrestore(&g_usbserial_lock, flags);
        return ST_EXISTS;
    }

    g_usbserial.ctrl = ctrl;
    g_usbserial.dev = dev;
    g_usbserial.comm_if = match->comm_if;
    g_usbserial.data_if = match->data_if;
    g_usbserial.initializing = 1;
    g_usbserial.active = 0;
    g_usbserial.mirror_enabled = 0;
    g_usbserial.write_busy = 1;
    spin_unlock_irqrestore(&g_usbserial_lock, flags);

    return ST_OK;
}

static int usbserial_activate_finish(xhci_controller_t* ctrl, usb_device_t* dev)
{
    uint64_t flags;

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    if (!g_usbserial.initializing || g_usbserial.dev != dev) {
        spin_unlock_irqrestore(&g_usbserial_lock, flags);
        return ST_ERR;
    }
    g_usbserial.active = 1;
    spin_unlock_irqrestore(&g_usbserial_lock, flags);

    usbserial_send_existing_klog(ctrl, dev);

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    if (g_usbserial.initializing && g_usbserial.dev == dev) {
        g_usbserial.mirror_enabled = 1;
        g_usbserial.write_busy = 0;
        g_usbserial.initializing = 0;
    }
    spin_unlock_irqrestore(&g_usbserial_lock, flags);

    return ST_OK;
}

static void usbserial_activate_fail(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_usbserial_lock, &flags);
    usbserial_clear_state();
    spin_unlock_irqrestore(&g_usbserial_lock, flags);
}

// CDC ACM activation: lock state, do control setup, finish
static int usbserial_activate(xhci_controller_t* ctrl, usb_device_t* dev,
                              const usbserial_match_t* match)
{
    int rc = usbserial_activate_common(ctrl, dev, match);
    if (rc != ST_OK) return rc;

    if (usbserial_send_control_setup(ctrl, dev, match->comm_if) != ST_OK) {
        usbserial_activate_fail();
        return ST_ERR;
    }

    return usbserial_activate_finish(ctrl, dev);
}

// PL2303 activation: lock state, vendor init already done, just finish
static int usbserial_activate_pl2303(xhci_controller_t* ctrl, usb_device_t* dev,
                                     const usbserial_match_t* match)
{
    int rc = usbserial_activate_common(ctrl, dev, match);
    if (rc != ST_OK) return rc;

    // PL2303 init was already done in pl2303_init_device before this call
    return usbserial_activate_finish(ctrl, dev);
}

int usbserial_probe(xhci_controller_t* ctrl, usb_device_t* dev,
                    uint8_t* config_desc, uint16_t config_len)
{
    usbserial_match_t match;

    if (!ctrl || !dev || !config_desc || config_len < sizeof(usb_config_desc_t)) {
        return ST_INVALID;
    }

    kmemset(&match, 0, sizeof(match));
    match.comm_if = 0xFF;
    match.data_if = 0xFF;

    // Try PL2303 first (vendor-specific, detected by VID/PID)
    {
        int rc = usbserial_probe_pl2303(ctrl, dev);
        if (rc != ST_UNSUPPORTED) {
            return rc;
        }
    }

    // Try CDC ACM
    if (usbserial_parse_cdc_acm(config_desc, config_len, &match) != ST_OK || !match.valid) {
        return ST_UNSUPPORTED;
    }

    kprintf("[USBSER] CDC ACM detected\n");

    dev->bulk_in_ep = match.bulk_in_ep;
    dev->bulk_out_ep = match.bulk_out_ep;
    dev->bulk_in_max_pkt = match.bulk_in_max_pkt;
    dev->bulk_out_max_pkt = match.bulk_out_max_pkt;

    if (!dev->bulk_in_ring || !dev->bulk_out_ring) {
        return ST_AGAIN;
    }

    return usbserial_activate(ctrl, dev, &match);
}

void usbserial_disconnect(usb_device_t* dev)
{
    uint64_t flags;

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    if ((g_usbserial.active || g_usbserial.initializing) && g_usbserial.dev == dev) {
        usbserial_clear_state();
    }
    spin_unlock_irqrestore(&g_usbserial_lock, flags);
}

void usbserial_log_write(const char* data, uint32_t len)
{
    uint64_t flags;
    xhci_controller_t* ctrl;
    usb_device_t* dev;

    if (!data || len == 0) {
        return;
    }

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    if (!g_usbserial.active || !g_usbserial.mirror_enabled || g_usbserial.write_busy) {
        spin_unlock_irqrestore(&g_usbserial_lock, flags);
        return;
    }

    ctrl = g_usbserial.ctrl;
    dev = g_usbserial.dev;
    g_usbserial.write_busy = 1;
    spin_unlock_irqrestore(&g_usbserial_lock, flags);

    (void)usbserial_write_text(ctrl, dev, data, len);

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    if (g_usbserial.dev == dev) {
        g_usbserial.write_busy = 0;
    }
    spin_unlock_irqrestore(&g_usbserial_lock, flags);
}

int usbserial_is_active(void)
{
    uint64_t flags;
    int active;

    spin_lock_irqsave(&g_usbserial_lock, &flags);
    active = g_usbserial.active && g_usbserial.mirror_enabled;
    spin_unlock_irqrestore(&g_usbserial_lock, flags);

    return active;
}

#endif