// LikeOS-64 - xHCI Boot Integration
// High-level interface for boot-time USB initialization and polling

#include "../../include/kernel/xhci_boot.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"

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

// Log extended capability information (for debugging)
static void xhci_log_ext_caps(xhci_controller_t* ctrl) {
    if (!ctrl->ext_caps_base) {
        kprintf("[XHCI BOOT] No extended capabilities\n");
        return;
    }
    
    // Look for ALL Supported Protocol capabilities (ID=2)
    // There can be multiple: one for USB 2.0 ports and one for USB 3.0 ports
    uint32_t protocol_offset = 0;
    while ((protocol_offset = xhci_find_ext_cap(ctrl, XHCI_EXT_CAP_PROTOCOL, protocol_offset)) != 0) {
        // xhci_find_ext_cap returns offset from ctrl->base, not absolute address
        volatile uint32_t* cap_ptr = (volatile uint32_t*)(ctrl->base + protocol_offset);
        uint32_t cap_data = cap_ptr[0];
        uint8_t minor_rev = (cap_data >> 16) & 0xFF;
        uint8_t major_rev = (cap_data >> 24) & 0xFF;
        
        // Port offset and count are in DWORD 2 (offset +8 bytes)
        uint32_t port_info = cap_ptr[2];
        uint8_t port_offset = port_info & 0xFF;
        uint8_t port_count = (port_info >> 8) & 0xFF;
        
        kprintf("[XHCI BOOT] Protocol: USB %d.%d, ports %d-%d\n",
                major_rev, minor_rev, port_offset, port_offset + port_count - 1);
        
        // Move to next capability (offset is in DWORDs)
        protocol_offset += 4;  // Move past current capability to search for next
    }
}

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
    
    // Check if 64-bit BAR
    uint64_t bar0_full;
    if ((xhci_pci->bar[0] & 0x6) == 0x4) {
        bar0_full = ((uint64_t)xhci_pci->bar[1] << 32) | (xhci_pci->bar[0] & ~0xFULL);
    } else {
        bar0_full = xhci_pci->bar[0] & ~0xFULL;
    }
    
    kprintf("[XHCI BOOT] Found xHCI at PCI %02x:%02x.%x, BAR0=0x%llx\n",
            xhci_pci->bus, xhci_pci->device, xhci_pci->function,
            bar0_full);
    
    // Check vendor/device ID for known quirks
    uint16_t vendor_id = xhci_pci->vendor_id;
    uint16_t device_id = xhci_pci->device_id;
    kprintf("[XHCI BOOT] Vendor: 0x%04x, Device: 0x%04x\n", vendor_id, device_id);
    
    // Initialize the global xHCI controller
    // This now includes BIOS handoff
    int st = xhci_init(&g_xhci, xhci_pci);
    if (st != ST_OK) {
        kprintf("[XHCI BOOT] Controller initialization failed: %d\n", st);
        return;
    }
    
    state->ctrl = &g_xhci;
    
    // Log extended capabilities (debug)
    xhci_log_ext_caps(&g_xhci);
    
    // Power up all ports - required for VirtualBox and some real hardware
    xhci_power_ports(&g_xhci);
    
    // Verify controller state after initialization
    if (xhci_verify_controller_state(&g_xhci) != ST_OK) {
        kprintf("[XHCI BOOT] Warning: Controller state verification failed\n");
        // Don't fail - some controllers may have quirks
    }
    
    kprintf("[XHCI BOOT] Controller initialized successfully (version %x.%02x)\n",
            g_xhci.hci_version >> 8, g_xhci.hci_version & 0xFF);
}


void xhci_boot_poll(xhci_boot_state_t* state) {
    if (!state || !state->ctrl) return;
    
    xhci_controller_t* ctrl = state->ctrl;
    
    // Process any pending events
    xhci_process_events(ctrl);
    
    // Keep polling ports until we have a mass storage device or reach max devices
    if (!state->msd_ready) {
        xhci_poll_ports(ctrl);
        
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
