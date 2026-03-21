// LikeOS-64 - xHCI Boot Integration
// High-level interface for boot-time USB initialization and polling

#include "../../include/kernel/xhci_boot.h"
#include "../../include/kernel/usbhid.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/interrupt.h"

// Boot state tracking
static int g_init_attempted = 0;
static usb_msd_device_t g_msd_device;

// Boot-time initialization timeout (in milliseconds)
#define XHCI_BOOT_INIT_TIMEOUT_MS  5000
#define XHCI_BOOT_POLL_INTERVAL_MS 10

// Verify controller is in expected state after initialization
static int xhci_verify_controller_state(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->base) {
        return ST_ERR;
    }
    
    // Check if controller is running (USBSTS.HCH should be 0 after start)
    uint32_t usbsts = xhci_op_read32(ctrl, XHCI_OP_USBSTS);
    if (usbsts & XHCI_STS_HCH) {
        kprintf("[XHCI BOOT] Warning: Controller halted unexpectedly\n");
        return ST_ERR;
    }
    
    // Check for HSE (Host System Error) - indicates DMA failure
    if (usbsts & XHCI_STS_HSE) {
        kprintf("[XHCI BOOT] Warning: Host System Error (HSE) set - DMA problem!\n");
        return ST_ERR;
    }
    
    // Verify DCBAA is properly set
    uint64_t dcbaap = xhci_op_read64(ctrl, XHCI_OP_DCBAAP);
    if (dcbaap != ctrl->dcbaa_phys) {
        kprintf("[XHCI BOOT] Warning: DCBAA mismatch (expected 0x%llx, got 0x%llx)\n",
                ctrl->dcbaa_phys, dcbaap);
        return ST_ERR;
    }
    
    // Note: Per xHCI spec 5.4.5, CRCR Command Ring Pointer always reads as 0
    // We can only verify CRR (Command Ring Running) bit which should be 0 initially
    uint64_t crcr = xhci_op_read64(ctrl, XHCI_OP_CRCR);
    if (crcr & (1 << 3)) {
        // CRR=1 means command ring is unexpectedly running
        kprintf("[XHCI BOOT] Note: CRR=1, command ring already running\n");
    }
    // Cannot verify CRP since it always reads as 0 per spec
    
    return ST_OK;
}

void xhci_boot_init(xhci_boot_state_t* state) {
    if (!state) return;
    
    state->ctrl = NULL;
    state->ctrl_hid = NULL;
    state->enum_complete = 0;
    state->msd_ready = 0;
    
    if (g_init_attempted) return;
    g_init_attempted = 1;
    
    kprintf("[XHCI BOOT] Starting USB initialization...\n");
    
    // Find first xHCI controller via PCI
    const pci_device_t* xhci_pci = pci_get_xhci(0);
    if (!xhci_pci) {
        kprintf("[XHCI BOOT] No xHCI controller found\n");
        return;
    }
    
    // Check if 64-bit BAR
    uint64_t bar0_full;
    if ((xhci_pci->bar[0] & 0x6) == 0x4) {
        bar0_full = ((uint64_t)xhci_pci->bar[1] << 32) | (xhci_pci->bar[0] & ~0xFULL);
    } else {
        bar0_full = xhci_pci->bar[0] & ~0xFULL;
    }
    
    kprintf("[XHCI BOOT] Controller 0 at PCI %02x:%02x.%x (0x%04x:0x%04x), BAR0=0x%llx\n",
            xhci_pci->bus, xhci_pci->device, xhci_pci->function,
            xhci_pci->vendor_id, xhci_pci->device_id, bar0_full);
    
    // Initialize the primary xHCI controller
    int st = xhci_init(&g_xhci, xhci_pci, XHCI_MSI_VECTOR);
    if (st != ST_OK) {
        kprintf("[XHCI BOOT] Controller 0 initialization failed: %d\n", st);
        return;
    }
    
    state->ctrl = &g_xhci;
    
    // Power up all ports - required for VirtualBox and some real hardware
    xhci_power_ports(&g_xhci);
    
    // Verify controller state after initialization
    if (xhci_verify_controller_state(&g_xhci) != ST_OK) {
        kprintf("[XHCI BOOT] Warning: Controller 0 state verification failed\n");
    }
    
    kprintf("[XHCI BOOT] Controller 0 ready (version %x.%02x, %d ports)\n",
            g_xhci.hci_version >> 8, g_xhci.hci_version & 0xFF,
            g_xhci.max_ports);
    
    // Check for a second xHCI controller (common on laptops where internal
    // HID devices like keyboard/touchpad are on a separate controller)
    const pci_device_t* xhci_pci2 = pci_get_xhci(1);
    if (xhci_pci2) {
        uint64_t bar0_full2;
        if ((xhci_pci2->bar[0] & 0x6) == 0x4) {
            bar0_full2 = ((uint64_t)xhci_pci2->bar[1] << 32) | (xhci_pci2->bar[0] & ~0xFULL);
        } else {
            bar0_full2 = xhci_pci2->bar[0] & ~0xFULL;
        }
        
        kprintf("[XHCI BOOT] Controller 1 at PCI %02x:%02x.%x (0x%04x:0x%04x), BAR0=0x%llx\n",
                xhci_pci2->bus, xhci_pci2->device, xhci_pci2->function,
                xhci_pci2->vendor_id, xhci_pci2->device_id, bar0_full2);
        
        st = xhci_init(&g_xhci_hid, xhci_pci2, XHCI_MSI_VECTOR_2);
        if (st == ST_OK) {
            state->ctrl_hid = &g_xhci_hid;
            xhci_power_ports(&g_xhci_hid);
            
            if (xhci_verify_controller_state(&g_xhci_hid) != ST_OK) {
                kprintf("[XHCI BOOT] Warning: Controller 1 state verification failed\n");
            }
            
            kprintf("[XHCI BOOT] Controller 1 ready (version %x.%02x, %d ports)\n",
                    g_xhci_hid.hci_version >> 8, g_xhci_hid.hci_version & 0xFF,
                    g_xhci_hid.max_ports);
        } else {
            kprintf("[XHCI BOOT] Controller 1 initialization failed: %d\n", st);
        }
    }
}


void xhci_boot_poll(xhci_boot_state_t* state) {
    if (!state || !state->ctrl) return;
    
    // Once boot enumeration is done, stop touching the event ring.
    // Transfer functions and the IRQ handler process events under
    // xhci_lock; doing it here without the lock caused a race that
    // corrupted event-ring dequeue/cycle and hung USB transfers.
    if (state->msd_ready) return;
    
    xhci_controller_t* ctrl = state->ctrl;
    
    // Process any pending events (use locked version to serialise with
    // the IRQ handler and any concurrent transfer polling on other CPUs).
    xhci_process_events_locked(ctrl);
    
    // Also process events on the secondary controller if present
    if (state->ctrl_hid) {
        xhci_process_events_locked(state->ctrl_hid);
    }
    
    // Keep polling ports until we have a mass storage device or reach max devices
    if (!state->msd_ready) {
        xhci_poll_ports(ctrl);
        
        // Also poll the secondary controller for HID devices
        if (state->ctrl_hid) {
            xhci_poll_ports(state->ctrl_hid);
        }
        
        // Check if we have new devices to examine (on either controller)
        int total_devices = ctrl->num_devices;
        if (state->ctrl_hid) total_devices += state->ctrl_hid->num_devices;
        
        if (total_devices > 0) {
            if (!state->enum_complete) {
                state->enum_complete = 1;
                // Drain any stale port status change events that accumulated
                // during boot power-up and enumeration.  Without this,
                // xhci_hotplug_poll() would re-process them and could
                // disrupt already-configured ports.
                ctrl->hotplug_ports = 0;
                if (state->ctrl_hid) state->ctrl_hid->hotplug_ports = 0;
                kprintf("[XHCI BOOT] Device enumeration complete (%d devices)\n", total_devices);
            }
            
            // Check for mass storage device on primary controller
            for (int i = 0; i < ctrl->num_devices; i++) {
                usb_device_t* dev = &ctrl->devices[i];
                if (dev->configured && dev->class_code == USB_CLASS_MASS_STORAGE) {
                    kprintf("[XHCI BOOT] Found USB Mass Storage device on port %d\n", dev->port);
                    
                    // Initialize MSD
                    int st = usb_msd_init(&g_msd_device, dev, ctrl);
                    if (st == ST_OK) {
                        state->msd_ready = 1;
                        kprintf("[XHCI BOOT] USB Mass Storage ready\n");
                    } else {
                        kprintf("[XHCI BOOT] MSD init failed: %d\n", st);
                    }
                    break;
                }
            }
            
            // Also check secondary controller for mass storage (in case
            // the USB stick is on the other controller)
            if (!state->msd_ready && state->ctrl_hid) {
                for (int i = 0; i < state->ctrl_hid->num_devices; i++) {
                    usb_device_t* dev = &state->ctrl_hid->devices[i];
                    if (dev->configured && dev->class_code == USB_CLASS_MASS_STORAGE) {
                        kprintf("[XHCI BOOT] Found USB Mass Storage device on ctrl1 port %d\n", dev->port);
                        
                        int st = usb_msd_init(&g_msd_device, dev, state->ctrl_hid);
                        if (st == ST_OK) {
                            state->msd_ready = 1;
                            kprintf("[XHCI BOOT] USB Mass Storage ready\n");
                        } else {
                            kprintf("[XHCI BOOT] MSD init failed: %d\n", st);
                        }
                        break;
                    }
                }
            }
            
            // Report HID device status
            if (usbhid_device_count() > 0) {
                kprintf("[XHCI BOOT] USB HID: %d device(s) (kbd=%d mouse=%d)\n",
                        usbhid_device_count(), usbhid_has_keyboard(), usbhid_has_mouse());
            }
        }
    }
}

int xhci_boot_is_ready(xhci_boot_state_t* state) {
    return (state && state->msd_ready);
}
