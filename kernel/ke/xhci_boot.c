// LikeOS - xHCI Boot Integration
// High-level interface for boot-time USB initialization and polling
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/xhci_boot.h>
#include <kernel/dev/hid/usb_hid.h>
#include <kernel/hal/pci.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/storage.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

// Boot state tracking
static int g_init_attempted = 0;

// Boot-time initialization timeout (in milliseconds)
#define XHCI_BOOT_INIT_TIMEOUT_MS 5000
#define XHCI_BOOT_POLL_INTERVAL_MS 10
/* How long the boot phase goes on after its first poll once the root
 * filesystem is mounted from a disk that is not USB (xhci_boot_phase_over). */
#define XHCI_BOOT_SETTLE_MS 3000

// Verify controller is in expected state after initialization
static int xhci_verify_controller_state(xhci_controller_t *ctrl)
{
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

void xhci_boot_init(xhci_boot_state_t *state)
{
	if (!state)
		return;

	state->ctrl = NULL;
	state->ctrl_hid = NULL;
	state->enum_complete = 0;
	state->msd_ready = 0;
	state->boot_done = 0;
	state->started = 0;
	state->start_tick = 0;
	state->hid_count = 0;
	state->hid_kbd = 0;
	state->hid_mouse = 0;

	if (g_init_attempted)
		return;
	g_init_attempted = 1;

	kprintf("[XHCI BOOT] Starting USB initialization...\n");

	// Find first xHCI controller via PCI
	const pci_device_t *xhci_pci = pci_get_xhci(0);
	if (!xhci_pci) {
		kprintf("[XHCI BOOT] No xHCI controller found\n");
		return;
	}

	// Check if 64-bit BAR
	uint64_t bar0_full;
	if ((xhci_pci->bar[0] & 0x6) == 0x4) {
		bar0_full = ((uint64_t)xhci_pci->bar[1] << 32) |
			    (xhci_pci->bar[0] & ~0xFULL);
	} else {
		bar0_full = xhci_pci->bar[0] & ~0xFULL;
	}
	BUG_ON(bar0_full ==
	       0); /* xHCI BAR0 is zero - PCI enumeration did not assign MMIO */
	WARN_ON(bar0_full <
		0x1000); /* xHCI BAR0 below 4K is suspicious - likely not a valid MMIO address */

	kprintf("[XHCI BOOT] Controller 0 at PCI %02x:%02x.%x (0x%04x:0x%04x), BAR0=0x%llx\n",
		xhci_pci->bus, xhci_pci->device, xhci_pci->function,
		xhci_pci->vendor_id, xhci_pci->device_id, bar0_full);

	// Initialize the primary xHCI controller
	int st = xhci_init(&g_xhci, xhci_pci);
	if (st != ST_OK) {
		kprintf("[XHCI BOOT] Controller 0 initialization failed: %d\n",
			st);
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
	const pci_device_t *xhci_pci2 = pci_get_xhci(1);
	if (xhci_pci2) {
		uint64_t bar0_full2;
		if ((xhci_pci2->bar[0] & 0x6) == 0x4) {
			bar0_full2 = ((uint64_t)xhci_pci2->bar[1] << 32) |
				     (xhci_pci2->bar[0] & ~0xFULL);
		} else {
			bar0_full2 = xhci_pci2->bar[0] & ~0xFULL;
		}

		kprintf("[XHCI BOOT] Controller 1 at PCI %02x:%02x.%x (0x%04x:0x%04x), BAR0=0x%llx\n",
			xhci_pci2->bus, xhci_pci2->device, xhci_pci2->function,
			xhci_pci2->vendor_id, xhci_pci2->device_id, bar0_full2);

		st = xhci_init(&g_xhci_hid, xhci_pci2);
		if (st == ST_OK) {
			state->ctrl_hid = &g_xhci_hid;
			xhci_power_ports(&g_xhci_hid);

			if (xhci_verify_controller_state(&g_xhci_hid) !=
			    ST_OK) {
				kprintf("[XHCI BOOT] Warning: Controller 1 state verification failed\n");
			}

			kprintf("[XHCI BOOT] Controller 1 ready (version %x.%02x, %d ports)\n",
				g_xhci_hid.hci_version >> 8,
				g_xhci_hid.hci_version & 0xFF,
				g_xhci_hid.max_ports);
		} else {
			kprintf("[XHCI BOOT] Controller 1 initialization failed: %d\n",
				st);
		}
	}
}

/* Report the USB keyboards and mice once per change, not on every poll. */
static void xhci_boot_report_hid(xhci_boot_state_t *state)
{
	int count = usbhid_device_count();
	int kbd = usbhid_has_keyboard();
	int mouse = usbhid_has_mouse();

	if (count == state->hid_count && kbd == state->hid_kbd &&
	    mouse == state->hid_mouse)
		return;
	state->hid_count = count;
	state->hid_kbd = kbd;
	state->hid_mouse = mouse;
	if (count > 0)
		kprintf("[XHCI BOOT] USB HID: %d device(s) (kbd=%d mouse=%d)\n",
			count, kbd, mouse);
}

/* Attach every configured mass-storage device of `ctrl' that is not
 * attached yet, right here: the root filesystem may be on one of them. */
static void xhci_boot_attach_storage(xhci_boot_state_t *state,
				     xhci_controller_t *ctrl, const char *which)
{
	for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
		usb_device_t *dev = &ctrl->devices[i];
		if (!dev->configured || dev->gone || !dev->msd_iface_valid)
			continue;
		int st = usb_msd_attach_boot(ctrl, dev);
		if (st == ST_OK) {
			kprintf("[XHCI BOOT] USB Mass Storage on %sport %d ready\n",
				which, dev->port);
			state->msd_ready = 1;
		} else if (st == ST_EXISTS) {
			state->msd_ready = 1;
		} else if (st != ST_NOT_FOUND && st != ST_BUSY &&
			   st != ST_NO_DEVICE) {
			kprintf("[XHCI BOOT] MSD init on %sport %d failed: %d\n",
				which, dev->port, st);
		}
	}
}

/* Is the boot enumeration phase over?  It is, once
 *   - a USB mass-storage device is ready (the phase was there to find it), or
 *   - the root filesystem is mounted and the phase has run for
 *     XHCI_BOOT_SETTLE_MS: the root is then on a disk that is not USB (a USB
 *     root needs the mass-storage device first), and a stick that is plugged
 *     in at boot has had that long to come up.
 * While the root is not mounted the search goes on, however long it takes:
 * the root may be the USB stick that is still coming up. */
static int xhci_boot_phase_over(xhci_boot_state_t *state)
{
	if (state->msd_ready)
		return 1;
	if (!storage_root_fs_type()) /* not NULL once the root is mounted */
		return 0;
	return timer_ticks() - state->start_tick >=
	       timer_ms_to_ticks(XHCI_BOOT_SETTLE_MS);
}

void xhci_boot_poll(xhci_boot_state_t *state)
{
	if (!state || !state->ctrl)
		return;

	// Once the boot phase is over, stop touching the controllers: from
	// then on the IRQ handler, the transfer functions and
	// xhci_hotplug_poll() own the event ring and the ports.  Processing
	// events here as well, without xhci_lock, once raced with them,
	// corrupted the event-ring dequeue/cycle state and hung USB
	// transfers.
	if (state->boot_done)
		return;

	if (!state->started) {
		state->started = 1;
		state->start_tick = timer_ticks();
	}

	xhci_controller_t *ctrl = state->ctrl;

	// Process any pending events (use locked version to serialise with
	// the IRQ handler and any concurrent transfer polling on other CPUs).
	xhci_process_events_locked(ctrl);

	// Also process events on the secondary controller if present
	if (state->ctrl_hid) {
		xhci_process_events_locked(state->ctrl_hid);
	}

	// Keep polling ports until we have a mass storage device or the boot
	// phase ends
	if (!state->msd_ready) {
		xhci_poll_ports(ctrl);

		// Also poll the secondary controller for HID devices
		if (state->ctrl_hid) {
			xhci_poll_ports(state->ctrl_hid);
		}

		// Check if we have new devices to examine (on either controller)
		int total_devices = ctrl->num_devices;
		if (state->ctrl_hid)
			total_devices += state->ctrl_hid->num_devices;

		if (total_devices > 0) {
			if (!state->enum_complete) {
				state->enum_complete = 1;
				// Drain any stale port status change events that accumulated
				// during boot power-up and enumeration.  Without this,
				// xhci_hotplug_poll() would re-process them and could
				// disrupt already-configured ports.
				ctrl->hotplug_ports = 0;
				if (state->ctrl_hid)
					state->ctrl_hid->hotplug_ports = 0;
				kprintf("[XHCI BOOT] Device enumeration complete (%d devices)\n",
					total_devices);
			}

			// Attach the mass-storage devices found so far, on
			// either controller (the USB stick may be on the other
			// one).  Devices are kept by slot id, which need not be
			// dense: every slot is looked at.
			xhci_boot_attach_storage(state, ctrl, "");
			if (state->ctrl_hid)
				xhci_boot_attach_storage(state, state->ctrl_hid,
							 "ctrl1 ");

			// Report HID device status (when it changed)
			xhci_boot_report_hid(state);
		}
	}

	if (xhci_boot_phase_over(state)) {
		state->boot_done = 1;
		if (!state->msd_ready)
			kprintf("[XHCI BOOT] Boot enumeration finished (root filesystem is not on USB); hot-plug handles new devices\n");
		/* The handover, on this thread -- the one that runs the
		 * hot-plug poll as well, so nothing enumerates meanwhile: a
		 * port connected without a device (its change event may have
		 * been drained above) is flagged for the hot-plug poll, and
		 * from now on the storage driver's attach thread attaches
		 * storage devices, starting with the configured ones the boot
		 * phase did not attach. */
		xhci_hotplug_rescan(ctrl);
		if (state->ctrl_hid)
			xhci_hotplug_rescan(state->ctrl_hid);
		usb_msd_boot_done();
	}
}

int xhci_boot_is_ready(xhci_boot_state_t *state)
{
	return (state && state->msd_ready);
}
