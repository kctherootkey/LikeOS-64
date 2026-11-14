// LikeOS - xHCI Boot Integration
// High-level interface for boot-time USB initialization
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_XHCI_BOOT_H
#define LIKEOS_XHCI_BOOT_H

#include <kernel/dev/usb/xhci.h>
#include <kernel/dev/usb/usb_msd.h>

// Boot state tracking
typedef struct {
	xhci_controller_t *ctrl; // Primary controller (mass storage)
	xhci_controller_t *ctrl_hid; // Secondary controller (HID), may be NULL
	int enum_complete;
	int msd_ready;
	/* The boot enumeration phase is over (a mass-storage device is
	 * ready, or the root was mounted from another disk); from then on
	 * xhci_boot_poll() leaves the controllers to the hot-plug path. */
	int boot_done;
	/* First poll seen, and timer_ticks() at that poll */
	int started;
	uint64_t start_tick;
	/* The USB HID status last reported (reported once per change) */
	int hid_count;
	int hid_kbd;
	int hid_mouse;
} xhci_boot_state_t;

// Boot-time initialization
void xhci_boot_init(xhci_boot_state_t *state);
void xhci_boot_poll(xhci_boot_state_t *state);
int xhci_boot_is_ready(xhci_boot_state_t *state);

#endif // LIKEOS_XHCI_BOOT_H
