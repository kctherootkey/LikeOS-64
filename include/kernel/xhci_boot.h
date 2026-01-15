// LikeOS-64 XHCI bootstrap helper
// Encapsulates controller discovery, initialization, and periodic polling

#ifndef _KERNEL_XHCI_BOOT_H_
#define _KERNEL_XHCI_BOOT_H_

#include "xhci.h"

typedef struct {
    int available;
    xhci_controller_t* controller;
} xhci_boot_state_t;

// Discover and initialize the first XHCI controller
void xhci_boot_init(xhci_boot_state_t* state);

// Poll ports/events and MSD helpers once; safe no-op if unavailable
void xhci_boot_poll(xhci_boot_state_t* state);

#endif // _KERNEL_XHCI_BOOT_H_
