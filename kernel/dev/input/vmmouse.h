// LikeOS -- VMware absolute pointing device ("vmmouse").
//
// A VMware guest can ask the hypervisor for the pointer's absolute position
// instead of the relative motion of the emulated PS/2 or USB mouse.  The
// host then no longer has to capture its own pointer for the guest: it
// shows the guest's cursor image as its own cursor wherever its pointer is
// over the guest's screen, and the guest reads where that is.  The data
// comes over the backdoor port (kernel/hal/vmware_backdoor.h); the emulated
// PS/2 mouse still raises its interrupt as the doorbell, its bytes carry
// nothing.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_INPUT_VMMOUSE_H_
#define _KERNEL_DEV_INPUT_VMMOUSE_H_

#include <kernel/uapi/types.h>

// Positions run from 0 to VMMOUSE_MAX across the guest's screen.
#define VMMOUSE_MAX 0xFFFFU

// struct vmmouse_packet.status
#define VMMOUSE_LEFT_BUTTON 0x20U
#define VMMOUSE_RIGHT_BUTTON 0x10U
#define VMMOUSE_MIDDLE_BUTTON 0x08U
// x and y are signed deltas (y up), not a position: the host is in
// relative mode.
#define VMMOUSE_RELATIVE_PACKET 0x00010000U

struct vmmouse_packet {
	uint32_t status; // buttons and flags
	uint32_t x, y; // position, or deltas with VMMOUSE_RELATIVE_PACKET
	uint32_t z; // wheel: a signed byte, positive towards the user
};

// Switch the host to absolute pointing.  0 when it did; negative when this
// is not VMware or the host offers no absolute pointer (then nothing has
// changed).  May be called again to re-enable after an error.
int vmmouse_enable(void);
// Back to the emulated mouse alone.
void vmmouse_disable(void);
// Take one packet off the host's queue: 1 with a packet, 0 when the queue
// is empty, negative when the device failed or lost sync (re-enable it).
// Any context; the caller serialises.
int vmmouse_read(struct vmmouse_packet *p);

#endif // _KERNEL_DEV_INPUT_VMMOUSE_H_
