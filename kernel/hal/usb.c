/* LikeOS-64 - Minimal USB core implementation */
#include "../../include/kernel/usb.h"
#include "../../include/kernel/console.h"

static usb_device_table_t g_usb_table;

void usb_core_init(void)
{
    int i;
    g_usb_table.count = 0;
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        g_usb_table.devices[i].address = 0;
        g_usb_table.devices[i].port_number = 0;
        g_usb_table.devices[i].speed = USB_SPEED_UNKNOWN;
        g_usb_table.devices[i].vid = 0;
        g_usb_table.devices[i].pid = 0;
        g_usb_table.devices[i].class_code = 0;
        g_usb_table.devices[i].subclass = 0;
        g_usb_table.devices[i].protocol = 0;
        g_usb_table.devices[i].configured = 0;
    }
    kprintf("USB core initialized (slots=%d)\n", USB_MAX_DEVICES);
}

usb_device_t *usb_alloc_device(uint8_t port)
{
    usb_device_t *d;
    if (g_usb_table.count >= USB_MAX_DEVICES)
        return 0;
    d = &g_usb_table.devices[g_usb_table.count++];
    d->address = 0;
    d->port_number = port;
    d->speed = USB_SPEED_UNKNOWN;
    d->slot_id = 0;
    d->vid = 0;
    d->pid = 0;
    d->class_code = 0;
    d->subclass = 0;
    d->protocol = 0;
    d->configured = 0;
    d->input_ctx = 0;
    d->device_ctx = 0;
    d->have_desc8 = 0;
    d->have_desc18 = 0;
    d->config_desc = 0;
    d->config_desc_len = 0;
    d->have_config9 = 0;
    d->have_config_full = 0;
    d->bulk_in_ep = 0;
    d->bulk_out_ep = 0;
    d->bulk_in_mps = 0;
    d->bulk_out_mps = 0;
    d->endpoints_configured = 0;
    kprintf("USB: allocated device slot %d for port %u\n", g_usb_table.count - 1,
        port);
    return d;
}
