// LikeOS-64 evdev input core - /dev/input/eventN
//
// Standard event-device interface for display servers (xf86-input-evdev):
// input_event records via read(), capability/identity ioctls, exclusive
// grabs.  Two fixed devices: unit 0 = keyboard, unit 1 = mouse.  The
// keyboard/mouse drivers feed raw events here in parallel with the cooked
// tty path; a grab suppresses the tty path entirely.

#ifndef _KERNEL_DEV_INPUT_EVDEV_H_
#define _KERNEL_DEV_INPUT_EVDEV_H_

#include <kernel/uapi/types.h>

struct task;

#define EVDEV_UNIT_KEYBOARD 0
#define EVDEV_UNIT_MOUSE 1
#define EVDEV_NUM_UNITS 2

// Device-file interface (devfs dispatch)
long evdev_read(int unit, void *user_buf, long bytes, int nonblock);
int evdev_ioctl(int unit, unsigned long req, void *argp, struct task *cur,
		void *owner);
/* Release a grab taken through a particular descriptor.  Keyed on the handle
 * rather than the task, because the closer is not always the opener. */
void evdev_release_grab_by_owner(int unit, void *owner);
short evdev_poll(int unit, short events);
void evdev_release_grab_for(int unit, int64_t task_id); // close/exit hook

// Producer interface (interrupt context safe)
// Feed one raw set-1 scancode byte (0xE0 prefixes included).
// Returns 1 when the keyboard is grabbed (caller must skip tty delivery).
int evdev_feed_keyboard(uint8_t scan_code);
// Feed one mouse report (raw pre-sensitivity deltas, evdev orientation:
// dy positive = down; wheel positive = away from user).  buttons bit0=left,
// bit1=right, bit2=middle.  Returns 1 when the mouse is grabbed.
int evdev_feed_mouse(int dx, int dy, uint8_t buttons, int wheel, int hwheel);

// Grab state (drivers use these to gate cursor drawing etc.)
int evdev_kbd_grabbed(void);
int evdev_mouse_grabbed(void);

#endif // _KERNEL_DEV_INPUT_EVDEV_H_
