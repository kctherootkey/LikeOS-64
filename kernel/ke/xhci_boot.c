#include "../../include/kernel/xhci_boot.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/usb.h"
#include "../../include/kernel/usb_msd.h"

extern xhci_controller_t g_xhci; // defined in hal/xhci.c

void xhci_boot_init(xhci_boot_state_t* state) {
    if (!state) {
        return;
    }
    state->available = 0;
    state->controller = 0;

    const pci_device_t* xhci_dev = pci_get_first_xhci();
    if (!xhci_dev) {
        kprintf("No XHCI controller found (USB mass storage unavailable yet)\n");
        usb_core_init();
        return;
    }

    kprintf("Found XHCI: bus=%u dev=%u func=%u vendor=%04x device=%04x\n",
            xhci_dev->bus, xhci_dev->device, xhci_dev->function,
            xhci_dev->vendor_id, xhci_dev->device_id);
    int xr = xhci_init(&g_xhci, xhci_dev);
    if (xr == ST_OK) {
        kprintf("XHCI controller running (mmio=%p)\n", (void*)g_xhci.mmio_base);
        state->available = 1;
        state->controller = &g_xhci;
        if (xhci_dev->interrupt_line != 0xFF) {
            irq_enable(xhci_dev->interrupt_line);
            kprintf("XHCI: enabled IRQ line %u\n", xhci_dev->interrupt_line);
        } else {
            kprintf("XHCI: no valid interrupt line, remaining in polling mode\n");
        }
    } else {
        kprintf("XHCI initialization failed (status=%d)\n", xr);
    }

    // Initialize USB core scaffolding after attempting controller bring-up
    usb_core_init();
}

void xhci_boot_poll(xhci_boot_state_t* state) {
    if (!state || !state->available || !state->controller) {
        return;
    }

    xhci_controller_t* ctrl = state->controller;

#ifdef XHCI_USE_INTERRUPTS
    static unsigned poll_div = 0;
    if ((poll_div++ & 0xF) == 0) {
        xhci_poll_ports(ctrl);
    }
    if ((poll_div & 0x1F) == 0) {
        xhci_process_events(ctrl);
    }
#else
    xhci_poll_ports(ctrl);
    xhci_process_events(ctrl);
#endif
    usb_msd_poll(ctrl);

    static int dbg_counter = 0;
    if (((dbg_counter++) & 0x3F) == 0) {
        extern void xhci_debug_state(xhci_controller_t*);
        xhci_debug_state(ctrl);
    }

    if (ctrl->pending_cmd_type) {
        ctrl->cmd_ring_stall_ticks++;
        if ((ctrl->cmd_ring_stall_ticks & 0xFF) == 0) {
            volatile uint32_t* db0 = (volatile uint32_t*)(ctrl->doorbell_array + 0x00);
            *db0 = 0;
        }
        if ((ctrl->cmd_ring_stall_ticks & 0x3FF) == 0) {
            volatile unsigned long* crcr = (volatile unsigned long*)(ctrl->op_base + XHCI_OP_CRCR);
            unsigned long val = (ctrl->cmd_ring_phys & ~0x3FUL) | (ctrl->cmd_cycle_state & 1);
            *crcr = val;
            volatile uint32_t* db0 = (volatile uint32_t*)(ctrl->doorbell_array + 0x00);
            *db0 = 0;
        }
    } else {
        ctrl->cmd_ring_stall_ticks = 0;
    }
}
