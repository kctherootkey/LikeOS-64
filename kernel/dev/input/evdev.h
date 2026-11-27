// LikeOS evdev - /dev/input/eventN
//
// The event-device interface of the input core: every registered input
// device gets a node /dev/input/eventN, and every descriptor opened on it is
// a client with its own event queue -- two readers of one device each get
// every event.  Clients read whole input_event records, query the device's
// capabilities and state with the EVIOC* ioctls, may grab the device
// (exclusive delivery), set their timestamp clock and event masks, and
// write events back (LED changes, autorepeat settings).
//
// Node numbers: event0 is the built-in keyboard and event1 the merged mouse
// (PS/2, USB HID boot mouse, VMware absolute pointer), always, whatever else
// registers first.  Further devices get the lowest free number from 2 up to
// EVDEV_MINORS - 1; a number is reused once its device is gone.
// Device numbers are char major 13, minor 64 + N.
//
// The keyboard and mouse drivers feed raw events here through the
// evdev_feed_*() producer functions, in parallel with the cooked tty path;
// a grab of event0 / event1 suppresses the tty path entirely.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_INPUT_EVDEV_H_
#define _KERNEL_DEV_INPUT_EVDEV_H_

#include <kernel/uapi/types.h>

/* Only a pointer to it is used here; the definition is in <kernel/ke/waitq.h>. */
struct wait_queue_head;

struct task;
struct input_dev;
struct input_value;
struct evdev;
struct evdev_client;

#define EVDEV_UNIT_KEYBOARD 0
#define EVDEV_UNIT_MOUSE 1
#define EVDEV_BUILTIN_UNITS 2 // event0/event1 are reserved for these
#define EVDEV_MINORS 32 // event0 .. event31
#define EVDEV_MAJOR 13
#define EVDEV_MINOR_BASE 64

// ---------------------------------------------------------------------------
// Device-file interface (devfs / poll dispatch).  Process context.
// ---------------------------------------------------------------------------

/* Is /dev/input/event<unit> there (registered and not being removed)? */
int evdev_unit_exists(int unit);
/* The lowest existing unit >= from, or -1: for listing /dev/input. */
int evdev_next_unit(int from);
/* Open a client on the unit: 0 and *out set, -ENODEV (no such node, or
 * it is going away), -ENOMEM, or the device driver's open() error. */
int evdev_open(int unit, struct evdev_client **out);
/* The descriptor's last reference is gone: release its grab and queue. */
void evdev_release(struct evdev_client *client);
/* The unit number the client was opened on. */
int evdev_client_unit(const struct evdev_client *client);
long evdev_read(struct evdev_client *client, void *user_buf, long bytes,
		int nonblock);
/* Events a client injects (whole input_event records), processed exactly
 * like the device's own reports: LED changes, autorepeat settings, ... */
long evdev_write(struct evdev_client *client, const void *user_buf,
		 long bytes);
int evdev_ioctl(struct evdev_client *client, unsigned long req, void *argp);
short evdev_poll(struct evdev_client *client, short events);
/* The wait queue poll()/select()/epoll register on.  It lives as long as
 * the client does. */
struct wait_queue_head *evdev_pollq(struct evdev_client *client);

// ---------------------------------------------------------------------------
// Producer interface for the built-in devices (interrupt context safe)
// ---------------------------------------------------------------------------

// Feed one raw set-1 scancode byte (0xE0 prefixes included).
// Returns 1 when the keyboard is grabbed (caller must skip tty delivery).
int evdev_feed_keyboard(uint8_t scan_code);
// Feed one mouse report (raw pre-sensitivity deltas, evdev orientation:
// dy positive = down; wheel positive = away from user).  buttons bit0=left,
// bit1=right, bit2=middle.  Returns 1 when the mouse is grabbed.
int evdev_feed_mouse(int dx, int dy, uint8_t buttons, int wheel, int hwheel);
// The mouse reports absolute positions from now on (an absolute pointing
// device was found at boot, before any client opened the device): ABS_X and
// ABS_Y from 0 to `max' replace REL_X and REL_Y.  Relative motion still fed
// through evdev_feed_mouse() moves the position as on a screen_w x
// screen_h screen.
void evdev_mouse_set_absolute(int32_t max, int screen_w, int screen_h);
void evdev_mouse_set_abs_screen(int screen_w, int screen_h);
// Feed one absolute report: position on the axes, buttons as above, wheel
// positive away from the user.  Returns 1 when the mouse is grabbed.
int evdev_feed_mouse_abs(uint32_t x, uint32_t y, uint8_t buttons, int wheel);

// Grab state (drivers use these to gate cursor drawing etc.)
int evdev_kbd_grabbed(void);
int evdev_mouse_grabbed(void);

// ---------------------------------------------------------------------------
// Input-core side (input.c only)
// ---------------------------------------------------------------------------

/* Register the built-in keyboard (event0) and mouse (event1) if that has
 * not happened yet.  Any context; idempotent. */
void evdev_builtin_init(void);
/* Give a registering device its node: 0, -ENOMEM or -ENFILE. */
int evdev_connect(struct input_dev *dev);
/* The same for a built-in device: static node, fixed minor, no allocation;
 * any context. */
int evdev_connect_builtin(struct input_dev *dev, struct evdev *node,
			  int minor);
/* Take the node of an unregistering device away (see input.c). */
void evdev_disconnect(struct input_dev *dev);
/* N of the node's eventN. */
int evdev_minor_of(const struct evdev *evdev);
/* A complete packet for the node's clients.  Called with the device's
 * event lock held.  Returns 1 when a client holds a grab: the device is
 * that client's alone, the in-kernel handles get nothing either. */
int evdev_pass_values(struct evdev *evdev, const struct input_value *vals,
		      unsigned int count, uint64_t mono_ns);

#endif // _KERNEL_DEV_INPUT_EVDEV_H_
