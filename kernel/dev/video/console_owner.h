// LikeOS -- who owns the display: the console's output gate, as a state
// machine.
//
// The console stops putting pixels on the screen while another program has
// taken the display, and paints all of it again when that program lets go.
// Two statements take the display, and either is enough:
//
//   - the console's display mode is KD_GRAPHICS (KDSETMODE; a display server
//     sets it at start and puts KD_TEXT back on exit, and the kernel puts it
//     back when the file that set it is closed -- keyboard.c);
//   - a DRM device has a master: a display manager scans out of buffers of
//     its own, from the moment it becomes master (opening the primary node
//     while nobody holds it, or SET_MASTER) until it drops it or closes.
//     One count per device with a master.
//
// Merely opening or mapping /dev/fb0 takes nothing: such a program draws
// alongside the console, which goes on drawing too, unless it also says
// KD_GRAPHICS.
//
// All of it is one word, changed with single atomic operations, so every
// change knows the state before and after it exactly: the change that turns
// "nobody" into "owned" is the one that takes the display (the hardware
// cursor goes down), and the change that turns "owned" into "nobody" is the
// one that gives it back (the full repaint).  Whichever of the two
// statements is withdrawn LAST repaints; withdrawing the first does nothing
// while the other still holds.  Read from interrupt context (every console
// flush and every pointer event asks), changed from process context.
//
// Kept free of everything else so the host harness runs the same code.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_VIDEO_CONSOLE_OWNER_H_
#define _KERNEL_DEV_VIDEO_CONSOLE_OWNER_H_

#define CONSOLE_OWNER_KD 1u /* the console is in KD_GRAPHICS */
#define CONSOLE_OWNER_MASTER 2u /* added once per DRM device with a master */

/* What a change did to the ownership as a whole. */
enum console_owner_change {
	CONSOLE_OWNER_UNCHANGED = 0, /* owned before and after, or neither */
	CONSOLE_OWNER_TAKEN = 1, /* nobody -> owned */
	CONSOLE_OWNER_RELEASED = 2, /* owned -> nobody */
};

static inline int console_owner_is_owned(const unsigned *w)
{
	return __atomic_load_n(w, __ATOMIC_ACQUIRE) != 0;
}

/* The console's display mode is now KD_GRAPHICS (graphics != 0) or not.
 * Idempotent: setting the mode it already has changes nothing. */
static inline enum console_owner_change console_owner_set_kd(unsigned *w,
							     int graphics)
{
	unsigned old;

	if (graphics) {
		old = __atomic_fetch_or(w, CONSOLE_OWNER_KD, __ATOMIC_ACQ_REL);
		return old == 0 ? CONSOLE_OWNER_TAKEN : CONSOLE_OWNER_UNCHANGED;
	}
	old = __atomic_fetch_and(w, ~CONSOLE_OWNER_KD, __ATOMIC_ACQ_REL);
	return old == CONSOLE_OWNER_KD ? CONSOLE_OWNER_RELEASED :
					 CONSOLE_OWNER_UNCHANGED;
}

/* A DRM device got a master. */
static inline enum console_owner_change console_owner_master_set(unsigned *w)
{
	unsigned old = __atomic_fetch_add(w, CONSOLE_OWNER_MASTER,
					  __ATOMIC_ACQ_REL);

	return old == 0 ? CONSOLE_OWNER_TAKEN : CONSOLE_OWNER_UNCHANGED;
}

/* A DRM device's master went.  Never below zero: a drop without a set
 * (which the DRM core does not do) changes nothing.  *underflow is set
 * then, for the caller to complain about. */
static inline enum console_owner_change console_owner_master_drop(
	unsigned *w, int *underflow)
{
	unsigned old = __atomic_load_n(w, __ATOMIC_ACQUIRE);

	*underflow = 0;
	do {
		if (old < CONSOLE_OWNER_MASTER) {
			*underflow = 1;
			return CONSOLE_OWNER_UNCHANGED;
		}
	} while (!__atomic_compare_exchange_n(w, &old,
					      old - CONSOLE_OWNER_MASTER, 0,
					      __ATOMIC_ACQ_REL,
					      __ATOMIC_ACQUIRE));
	return old == CONSOLE_OWNER_MASTER ? CONSOLE_OWNER_RELEASED :
					     CONSOLE_OWNER_UNCHANGED;
}

#endif /* _KERNEL_DEV_VIDEO_CONSOLE_OWNER_H_ */
