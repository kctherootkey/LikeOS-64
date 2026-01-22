// LikeOS-64 - xHCI Boot Integration
// High-level interface for boot-time USB initialization
#ifndef LIKEOS_XHCI_BOOT_H
#define LIKEOS_XHCI_BOOT_H

#include "xhci.h"
#include "usb_msd.h"

// Boot state tracking
typedef struct {
    xhci_controller_t* ctrl;
    int enum_complete;
    int msd_ready;
} xhci_boot_state_t;

// Boot-time initialization
void xhci_boot_init(xhci_boot_state_t* state);
void xhci_boot_poll(xhci_boot_state_t* state);
int xhci_boot_is_ready(xhci_boot_state_t* state);

#endif // LIKEOS_XHCI_BOOT_H
