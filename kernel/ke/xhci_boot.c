// LikeOS-64 - xHCI Boot Integration
// High-level interface for boot-time USB initialization and polling

#include "../../include/kernel/xhci_boot.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"

// Boot state tracking
static int g_init_attempted = 0;
static usb_msd_device_t g_msd_device;

void xhci_boot_init(xhci_boot_state_t* state) {
    if (!state) return;
    
    state->ctrl = NULL;
    state->enum_complete = 0;
    state->msd_ready = 0;
    
    if (g_init_attempted) return;
    g_init_attempted = 1;
    
    kprintf("[XHCI BOOT] Starting USB initialization...\n");
    
    // Find xHCI controller via PCI
    const pci_device_t* xhci_pci = pci_get_first_xhci();
    if (!xhci_pci) {
        kprintf("[XHCI BOOT] No xHCI controller found\n");
        return;
    }
    
    kprintf("[XHCI BOOT] Found xHCI at PCI %02x:%02x.%x, BAR0=0x%08x\n",
            xhci_pci->bus, xhci_pci->device, xhci_pci->function,
            xhci_pci->bar[0]);
    
    // Initialize the global xHCI controller
    int st = xhci_init(&g_xhci, xhci_pci);
    if (st != ST_OK) {
        kprintf("[XHCI BOOT] Controller initialization failed: %d\n", st);
        return;
    }
    
    state->ctrl = &g_xhci;
    
    kprintf("[XHCI BOOT] Controller initialized successfully\n");
}

void xhci_boot_poll(xhci_boot_state_t* state) {
    if (!state || !state->ctrl) return;
    
    xhci_controller_t* ctrl = state->ctrl;
    
    // Process any pending events
    xhci_process_events(ctrl);
    
    // Keep polling ports until we have a mass storage device or reach max devices
    if (!state->msd_ready) {
        int connected = xhci_poll_ports(ctrl);
        
        // Check if we have new devices to examine
        if (ctrl->num_devices > 0) {
            // Check for mass storage device
            for (int i = 0; i < ctrl->num_devices; i++) {
                usb_device_t* dev = &ctrl->devices[i];
                if (dev->configured && dev->class_code == USB_CLASS_MASS_STORAGE) {
                    if (!state->enum_complete) {
                        state->enum_complete = 1;
                        kprintf("[XHCI BOOT] Device enumeration complete (%d devices)\n", ctrl->num_devices);
                    }
                    kprintf("[XHCI BOOT] Found USB Mass Storage device on port %d\n", dev->port);
                    
                    // Initialize MSD
                    int st = usb_msd_init(&g_msd_device, dev, ctrl);
                    if (st == ST_OK) {
                        state->msd_ready = 1;
                        kprintf("[XHCI BOOT] USB Mass Storage ready\n");
                    } else {
                        kprintf("[XHCI BOOT] MSD init failed: %d\n", st);
                    }
                    return;
                }
            }
        }
    }
}

int xhci_boot_is_ready(xhci_boot_state_t* state) {
    return (state && state->msd_ready);
}
