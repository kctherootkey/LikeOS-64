// LikeOS input core - device registration and event processing
//
// Every input device in the system -- the PS/2 keyboard, the merged mouse,
// an I2C-HID multitouch touchpad, and whatever comes later -- is described
// by one struct input_dev.  A driver allocates it, declares what the device
// can report (event types, codes, absolute axis ranges, properties,
// multitouch slots), registers it, and from then on reports what happens
// with input_event() / input_report_*() / input_sync().
//
// The core keeps the device's state (which keys are down, where every axis
// is, the contents of every multitouch slot), drops events that would not
// change anything or that the device never declared, batches the rest into
// packets ended by SYN_REPORT, and hands each complete packet to the event
// device layer (evdev.c), which gives every open /dev/input/eventN
// descriptor its own copy.
//
// In-kernel listeners.  Besides the event nodes, kernel code can listen to
// a device: an input_handler is told about every device that registers
// (connect), and attaches an input_handle to the devices it wants.  An open
// handle receives every packet the device's readers receive, through a
// callback that runs under the device's event lock (any context, must not
// sleep), and keeps the device open just as a user-space reader does.  The
// core counts the two kinds of users separately: dev->users is the number
// of event nodes user space has open (0 or 1 -- the node counts its own
// descriptors), dev->kernel_users the number of open handles; the driver's
// open()/close() run when their sum leaves / reaches zero, and every handle
// of the device is told when the user-space count goes from zero to
// non-zero and back (users_changed).  The built-in keyboard and mouse
// (event0/event1) are never offered to handlers: kernel listeners feed the
// built-in mouse, and listening to it would be a loop.
//
// Context rules:
//   - input_allocate_device(), input_register_device(),
//     input_unregister_device(), input_free_device(), input_mt_init_slots()
//     and input_set_abs_params() on a registered device: process context.
//   - input_event() and every input_report_*() / input_sync() / input_mt_*()
//     reporting helper: ANY context, including hard interrupt handlers.
//     They take only the device's event spinlock (irqsave) and never sleep.
//   - open()/close() callbacks of a driver run in process context, with
//     the device's mutex held; event() runs under the event spinlock.
//
// Lifetime: input_allocate_device() returns a device holding one reference.
// Before registration, input_free_device() drops it.  After a successful
// input_register_device() the driver must call input_unregister_device()
// instead (which drops the same reference) and must NOT also call
// input_free_device().  Open event-device descriptors keep their own
// references, so the memory stays valid until the last one is closed; the
// driver's own data (name strings, private state) only has to outlive
// input_unregister_device().
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 1999-2002 Vojtech Pavlik

#ifndef _KERNEL_DEV_INPUT_INPUT_H_
#define _KERNEL_DEV_INPUT_INPUT_H_

#include <kernel/uapi/types.h>
#include <kernel/uapi/input.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/workqueue.h>

// ---------------------------------------------------------------------------
// Bitmaps.  Capability and state bitmaps are arrays of unsigned long with
// bit N of the map in word N / 64, bit N % 64 -- the layout the EVIOCGBIT
// family copies to user space unchanged (on a little-endian machine the
// bytes come out in the same order as a byte-wise bitmap would).
// ---------------------------------------------------------------------------
#define INPUT_BITS_PER_LONG 64
#define INPUT_BITS_TO_LONGS(nbits) \
	(((nbits) + INPUT_BITS_PER_LONG - 1) / INPUT_BITS_PER_LONG)

static inline int input_test_bit(unsigned int nr, const unsigned long *map)
{
	return (int)((map[nr / INPUT_BITS_PER_LONG] >>
		      (nr % INPUT_BITS_PER_LONG)) & 1UL);
}

static inline void input_set_bit(unsigned int nr, unsigned long *map)
{
	map[nr / INPUT_BITS_PER_LONG] |= 1UL << (nr % INPUT_BITS_PER_LONG);
}

static inline void input_clear_bit(unsigned int nr, unsigned long *map)
{
	map[nr / INPUT_BITS_PER_LONG] &= ~(1UL << (nr % INPUT_BITS_PER_LONG));
}

/* Is `code' (<= max) set in `map'?  Out-of-range codes never are. */
static inline int input_code_supported(unsigned int code,
				       const unsigned long *map,
				       unsigned int max)
{
	return code <= max && input_test_bit(code, map);
}

// ---------------------------------------------------------------------------
// One event inside a packet, before it is timestamped for a reader.
// ---------------------------------------------------------------------------
struct input_value {
	uint16_t type;
	uint16_t code;
	int32_t value;
};

struct input_mt; // multitouch slot state, see input-mt.h
struct evdev; // the event-device node of a registered device (evdev.c)
struct input_handle; // an in-kernel listener attached to a device

// ---------------------------------------------------------------------------
// The device.
// ---------------------------------------------------------------------------
struct input_dev {
	// ---- identity, set by the driver before registration ----
	const char *name; // "ELAN073B:00 04F3:3169 Touchpad"
	const char *phys; // "i2c-ELAN073B:00"; NULL: none
	const char *uniq; // serial number; NULL: none
	struct input_id id; // bus, vendor, product, version
	/* Where the device hangs in the device tree, for the S: line of
	 * /proc/bus/input/devices ("<devpath>/input/inputN").  NULL: the
	 * device is listed under /devices/virtual/input. */
	const char *devpath;

	// ---- capabilities, set by the driver before registration ----
	unsigned long propbit[INPUT_BITS_TO_LONGS(INPUT_PROP_CNT)];
	unsigned long evbit[INPUT_BITS_TO_LONGS(EV_CNT)];
	unsigned long keybit[INPUT_BITS_TO_LONGS(KEY_CNT)];
	unsigned long relbit[INPUT_BITS_TO_LONGS(REL_CNT)];
	unsigned long absbit[INPUT_BITS_TO_LONGS(ABS_CNT)];
	unsigned long mscbit[INPUT_BITS_TO_LONGS(MSC_CNT)];
	unsigned long ledbit[INPUT_BITS_TO_LONGS(LED_CNT)];
	unsigned long sndbit[INPUT_BITS_TO_LONGS(SND_CNT)];
	unsigned long ffbit[INPUT_BITS_TO_LONGS(FF_CNT)];
	unsigned long swbit[INPUT_BITS_TO_LONGS(SW_CNT)];

	/* Average number of events in one packet, as the driver knows it.
	 * The core raises it to its own estimate from the capabilities; it
	 * sizes the packet buffer and every reader's queue. */
	unsigned int hint_events_per_packet;

	/* Autorepeat parameters (REP_DELAY ms, REP_PERIOD ms).  A driver
	 * that repeats keys itself presets them; otherwise registration sets
	 * 250/33 and the core repeats the last key pressed on a device with
	 * EV_REP (software autorepeat: KEY value 2 every period after the
	 * delay, each in a packet of its own).  EVIOCGREP reads them,
	 * EVIOCSREP changes them as EV_REP events. */
	int rep[REP_CNT];

	/* Scancode -> key code table (optional): keycodemax entries of
	 * keycodesize bytes (1, 2 or 4).  EVIOCGKEYCODE / EVIOCSKEYCODE go
	 * through getkeycode / setkeycode, which default to plain lookups in
	 * this table; a device without one answers -EINVAL.  Both callbacks
	 * run under the event spinlock. */
	void *keycode;
	unsigned int keycodemax;
	unsigned int keycodesize;
	int (*setkeycode)(struct input_dev *dev,
			  const struct input_keymap_entry *ke,
			  unsigned int *old_keycode);
	int (*getkeycode)(struct input_dev *dev, struct input_keymap_entry *ke);

	// ---- current state, owned by the core (event_lock) ----
	unsigned long key[INPUT_BITS_TO_LONGS(KEY_CNT)];
	unsigned long led[INPUT_BITS_TO_LONGS(LED_CNT)];
	unsigned long snd[INPUT_BITS_TO_LONGS(SND_CNT)];
	unsigned long sw[INPUT_BITS_TO_LONGS(SW_CNT)];
	/* Range and current value of every absolute axis.  Meaningful for
	 * the axes in absbit; the rest read as zero. */
	struct input_absinfo absinfo[ABS_CNT];
	struct input_mt *mt; // multitouch slots (input_mt_init_slots)

	// ---- driver callbacks (all optional) ----
	/* First user opened an event node of the device / last one closed
	 * it.  Process context, the device mutex held.  open() returning a
	 * negative errno fails the open(2). */
	int (*open)(struct input_dev *dev);
	void (*close)(struct input_dev *dev);
	/* An event for the device itself: LED, sound, autorepeat parameter,
	 * force feedback -- what a client writes to the node.  Runs under
	 * the event spinlock: it must not sleep.  Called only while the
	 * device is open; at the first open the current LED / sound /
	 * autorepeat state is replayed through it, and at the last close
	 * the LEDs and sounds that are on are switched off through it
	 * (registered devices; the built-in keyboard and mouse get every
	 * event and no replay). */
	int (*event)(struct input_dev *dev, unsigned int type,
		     unsigned int code, int value);
	void *driver_data;

	// ---- core-private ----
	spinlock_t event_lock; // state, packet buffer, evdev link
	struct mutex mutex; // users / open / close
	refcount_t refcount;
	/* Event nodes user space has open (mutex).  An event node opens its
	 * device once for all its descriptors, so this is 0 or 1. */
	unsigned int users;
	/* Open in-kernel handles (mutex). */
	unsigned int kernel_users;
	/* Attached in-kernel handles: changed with the mutex AND the event
	 * lock held, walked by the event path under the event lock. */
	struct input_handle *handles;
	/* The handlers were offered the device (g_input_handler_lock). */
	int handlers_attached;
	int registered; // between register and unregister
	/* The event() callback may be called: the device is open (built-in
	 * devices: always).  event_lock. */
	int ready;
	/* Software autorepeat (input_enable_softrepeat()): repeat_work is
	 * initialised; repeat_live (event_lock) allows arming it, from a
	 * successful registration until unregistration starts. */
	int softrepeat;
	int repeat_live;
	unsigned int repeat_key;
	struct delayed_work repeat_work;
	int going_away; // unregistration started: no new users
	int static_storage; // the core never frees it (built-in devices)
	/* The packet being assembled: events since the last SYN_REPORT. */
	struct input_value *vals;
	unsigned int num_vals;
	unsigned int max_vals;
	/* Monotonic time of the packet in progress (input_set_timestamp()
	 * or the first flush), valid while timestamp_set. */
	uint64_t timestamp_ns;
	int timestamp_set;
	struct evdev *evdev; // the node while registered (event_lock)
	int input_no; // N of inputN, -1 while unregistered
	struct input_dev *next; // registered-device list (input_list_lock)
};

// ---------------------------------------------------------------------------
// Allocation and registration
// ---------------------------------------------------------------------------

/* A zeroed device holding one reference, or NULL.  Process context. */
struct input_dev *input_allocate_device(void);
/* Drop the allocation reference of a device that was never registered (or
 * whose registration failed).  NULL is ignored.  Process context. */
void input_free_device(struct input_dev *dev);
/* Make the device live: it gets an inputN number and an event node
 * /dev/input/eventN (the lowest free number from 2 up; 0 and 1 belong to
 * the built-in keyboard and mouse), appears in /proc/bus/input/devices, and
 * a DEVICE_CLASS_INPUT add event is published.  EV_SYN is added, KEY_RESERVED
 * removed, and code bitmaps of types the device does not have are cleared;
 * without preset rep[] timings the core's software autorepeat is enabled
 * (input_enable_softrepeat(dev, 250, 33)), and without getkeycode /
 * setkeycode the default table methods are used.
 * 0, -EINVAL (bad description), -ENOMEM, or -ENFILE (no free event node).
 * Process context. */
int input_register_device(struct input_dev *dev);
/* Take the device away: keys still down are released to readers, every
 * open descriptor reports ENODEV / POLLHUP|POLLERR from now on, the node
 * disappears and a remove event is published.  Drops the device reference
 * taken by input_allocate_device(); the driver must not touch `dev'
 * afterwards.  Process context. */
void input_unregister_device(struct input_dev *dev);

/* Extra references, for code that keeps a device pointer around. */
void input_get_device(struct input_dev *dev);
void input_put_device(struct input_dev *dev);

static inline void input_set_drvdata(struct input_dev *dev, void *data)
{
	dev->driver_data = data;
}

static inline void *input_get_drvdata(struct input_dev *dev)
{
	return dev->driver_data;
}

/* Whether anybody -- a user-space reader of the event node or an in-kernel
 * handle -- has the device open right now (a racy snapshot: a driver uses
 * it to decide whether a fallback path should run, not for correctness). */
static inline int input_device_enabled(const struct input_dev *dev)
{
	return __atomic_load_n(&dev->users, __ATOMIC_RELAXED) != 0 ||
	       __atomic_load_n(&dev->kernel_users, __ATOMIC_RELAXED) != 0;
}

/* Whether user space has the event node open (same snapshot rules). */
static inline int input_device_user_open(const struct input_dev *dev)
{
	return __atomic_load_n(&dev->users, __ATOMIC_RELAXED) != 0;
}

// ---------------------------------------------------------------------------
// In-kernel listeners (see the top of this file)
// ---------------------------------------------------------------------------

struct input_handler;

struct input_handle {
	// ---- set by the owner before input_register_handle() ----
	const char *name;
	struct input_handler *handler; // who owns it (disconnect callback)
	void *private;
	/* A complete packet -- `count' values ending with the SYN_REPORT --
	 * exactly as the event nodes get it, with its timestamp.  Called
	 * with the device's event lock held, interrupts off, in whatever
	 * context the driver reported in: must not sleep, must not report
	 * events on THIS device (other devices are fine). */
	void (*events)(struct input_handle *handle,
		       const struct input_value *vals, unsigned int count,
		       uint64_t mono_ns);
	/* The number of user-space readers went from zero to non-zero
	 * (users != 0) or back to zero; also called by
	 * input_register_handle() when user space has the node open
	 * already.  Process context, the device's mutex held: must not open
	 * or close handles of the device. */
	void (*users_changed)(struct input_handle *handle,
			      unsigned int users);

	// ---- the core's ----
	struct input_dev *dev; // set by input_register_handle()
	int open; // input_handle_open() count (dev->mutex)
	struct input_handle *next; // dev->handles
};

struct input_handler {
	const char *name;
	/* A device was registered (or the handler was, and the device
	 * already exists).  The handler looks at the capabilities and, when
	 * it wants the device, attaches a handle with
	 * input_register_handle().  Process context; it must not register
	 * or unregister input devices.  The return value is informational
	 * (0: attached, -ENODEV: not interested). */
	int (*connect)(struct input_handler *handler, struct input_dev *dev);
	/* The device is going away: detach the handle (and free it).  The
	 * handler must call input_unregister_handle(handle) here.  Process
	 * context, the device is still registered but takes no new users. */
	void (*disconnect)(struct input_handle *handle);

	// ---- the core's ----
	struct input_handler *next;
	int registered;
};

/* Register a handler: it is offered every registered device (except the
 * built-in keyboard and mouse) now, and every device that registers later.
 * 0, -EINVAL, -EBUSY (already registered).  Process context. */
int input_register_handler(struct input_handler *handler);
/* Detach the handler from every device (disconnect for each of its
 * handles) and forget it.  Process context. */
void input_unregister_handler(struct input_handler *handler);
/* Attach a handle to a device (closed: it gets no packets until opened).
 * 0, -EINVAL, -ENODEV (the device is going away).  Process context. */
int input_register_handle(struct input_dev *dev, struct input_handle *handle);
/* Detach: closed first if it is open.  When this returns, the events
 * callback is not running and will not run again, so the handle may be
 * freed.  Process context. */
void input_unregister_handle(struct input_handle *handle);
/* Open the device for the handle: packets reach its events callback from
 * now on, and the device is open (the driver's open() runs if this is
 * its first user of either kind).  0, -ENODEV (going away), or the
 * driver's open() error.  Process context. */
int input_handle_open(struct input_handle *handle);
/* Close it again (the driver's close() after the last user).  Process
 * context. */
void input_handle_close(struct input_handle *handle);

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

/* Declare that the device reports `code' of `type' (sets the type bit too).
 * EV_ABS axes need input_set_abs_params() for their range instead. */
void input_set_capability(struct input_dev *dev, unsigned int type,
			  unsigned int code);
/* Declare an absolute axis and its range: EV_ABS and the axis bit are set,
 * value starts at 0, resolution is left as it was (input_abs_set_res()). */
void input_set_abs_params(struct input_dev *dev, unsigned int axis, int min,
			  int max, int fuzz, int flat);
/* Give `dst' axis `dst_axis' the parameters of axis `src_axis' of `src'
 * (which must have it): e.g. a pen device sharing a touchscreen's
 * coordinates.  Process context, before registration. */
void input_copy_abs(struct input_dev *dst, unsigned int dst_axis,
		    const struct input_dev *src, unsigned int src_axis);
/* Software autorepeat with these timings (ms), for a driver that wants it
 * although it set rep[] itself. */
void input_enable_softrepeat(struct input_dev *dev, int delay, int period);

#define INPUT_ABS_ACCESSORS(field)                                         \
	static inline int input_abs_get_##field(const struct input_dev *dev, \
						unsigned int axis)         \
	{                                                                  \
		return axis < ABS_CNT ? dev->absinfo[axis].field : 0;      \
	}                                                                  \
	static inline void input_abs_set_##field(struct input_dev *dev,    \
						 unsigned int axis, int v) \
	{                                                                  \
		if (axis < ABS_CNT)                                        \
			dev->absinfo[axis].field = v;                      \
	}

INPUT_ABS_ACCESSORS(value)
INPUT_ABS_ACCESSORS(minimum)
INPUT_ABS_ACCESSORS(maximum)
INPUT_ABS_ACCESSORS(fuzz)
INPUT_ABS_ACCESSORS(flat)
INPUT_ABS_ACCESSORS(resolution)

#undef INPUT_ABS_ACCESSORS

/* The usual short names. */
static inline int input_abs_get_val(const struct input_dev *dev,
				    unsigned int axis)
{
	return input_abs_get_value(dev, axis);
}

static inline void input_abs_set_val(struct input_dev *dev, unsigned int axis,
				     int v)
{
	input_abs_set_value(dev, axis, v);
}

static inline int input_abs_get_min(const struct input_dev *dev,
				    unsigned int axis)
{
	return input_abs_get_minimum(dev, axis);
}

static inline int input_abs_get_max(const struct input_dev *dev,
				    unsigned int axis)
{
	return input_abs_get_maximum(dev, axis);
}

static inline int input_abs_get_res(const struct input_dev *dev,
				    unsigned int axis)
{
	return input_abs_get_resolution(dev, axis);
}

static inline void input_abs_set_res(struct input_dev *dev, unsigned int axis,
				     int res)
{
	input_abs_set_resolution(dev, axis, res);
}

// ---------------------------------------------------------------------------
// Reporting (any context)
// ---------------------------------------------------------------------------

/* Report one event.  What the device did not declare is dropped; so is what
 * would not change the device's state (a key already down reported down
 * again, an axis at the value it already has, relative motion of zero) --
 * except a key's autorepeat (value 2), which changes no state and always
 * passes.  Accepted events collect into a packet that SYN_REPORT hands to
 * readers.  EV_SYN needs no declaration. */
void input_event(struct input_dev *dev, unsigned int type, unsigned int code,
		 int value);

/* The time of the packet in progress, monotonic nanoseconds -- for a driver
 * that captured the moment of the interrupt.  Without it the packet is
 * stamped when it is handed to the readers. */
void input_set_timestamp(struct input_dev *dev, uint64_t mono_ns);

static inline void input_report_key(struct input_dev *dev, unsigned int code,
				    int value)
{
	input_event(dev, EV_KEY, code, !!value);
}

static inline void input_report_rel(struct input_dev *dev, unsigned int code,
				    int value)
{
	input_event(dev, EV_REL, code, value);
}

static inline void input_report_abs(struct input_dev *dev, unsigned int code,
				    int value)
{
	input_event(dev, EV_ABS, code, value);
}

static inline void input_report_switch(struct input_dev *dev,
				       unsigned int code, int value)
{
	input_event(dev, EV_SW, code, !!value);
}

static inline void input_report_ff_status(struct input_dev *dev,
					  unsigned int code, int value)
{
	input_event(dev, EV_FF_STATUS, code, value);
}

/* End of a packet: everything reported since the last one goes out. */
static inline void input_sync(struct input_dev *dev)
{
	input_event(dev, EV_SYN, SYN_REPORT, 0);
}

/* End of one contact in the anonymous (slot-less) multitouch protocol. */
static inline void input_mt_sync(struct input_dev *dev)
{
	input_event(dev, EV_SYN, SYN_MT_REPORT, 0);
}

// ---------------------------------------------------------------------------
// For the event-device layer and procfs (not for drivers)
// ---------------------------------------------------------------------------

/* Key code table access (EVIOCGKEYCODE / EVIOCSKEYCODE): through the
 * device's getkeycode / setkeycode, under the event lock.  A key that is
 * down and no longer in the table after a change is released.  0 or
 * -EINVAL.  Any context. */
int input_get_keycode(struct input_dev *dev, struct input_keymap_entry *ke);
int input_set_keycode(struct input_dev *dev,
		      const struct input_keymap_entry *ke);
/* The default table methods (exposed for drivers that extend them; the
 * event lock is held when they run). */
int input_scancode_to_scalar(const struct input_keymap_entry *ke,
			     unsigned int *scancode);
int input_default_getkeycode(struct input_dev *dev,
			     struct input_keymap_entry *ke);
int input_default_setkeycode(struct input_dev *dev,
			     const struct input_keymap_entry *ke,
			     unsigned int *old_keycode);

/* An event written to an event node by a client: the same filtering as
 * input_event(), and a device callback for LEDs and the like. */
void input_inject_event(struct input_dev *dev, unsigned int type,
			unsigned int code, int value);
/* input_event() for a caller that already holds dev->event_lock (the
 * multitouch helpers, which need several events to be atomic). */
void input_handle_event_locked(struct input_dev *dev, unsigned int type,
			       unsigned int code, int value);
/* An event node opened / closed the device for user space: the first
 * user (of either kind) calls the driver's open(), the last close its
 * close(); the handles hear about user space coming and going.  Process
 * context. */
int input_open_device(struct input_dev *dev);
void input_close_device(struct input_dev *dev);

/* Register one of the two built-in devices (keyboard, mouse) whose node
 * number is fixed and whose storage -- the device, its packet buffer
 * (dev->vals / dev->max_vals preset) and its event node -- is static.
 * Callable from any context, including the interrupt that reports the
 * device's first event; never sleeps and never allocates. */
int input_register_builtin_device(struct input_dev *dev, struct evdev *node,
				  int minor);

/* A summary of what kind of device it is, DEVICE_EVENT_F_INPUT_* bits of
 * <kernel/uapi/devevent.h>: keyboard, mouse, touchpad, touchscreen, ...
 * Derived from the capabilities only. */
uint32_t input_classify(const struct input_dev *dev);

struct pfs_node;
/* /proc/bus/input/devices: one block per registered device. */
long input_proc_devices_show(struct pfs_node *n, char *buf, long cap);

/* Format the block of one device into buf at pos (pfs_printf rules: the
 * returned position may exceed cap, meaning "this much was needed").
 * `evdev_minor' is the N of eventN, -1 for none.  Exposed for the host
 * test harness. */
long input_format_device(const struct input_dev *dev, int evdev_minor,
			 char *buf, long cap, long pos);

#endif // _KERNEL_DEV_INPUT_INPUT_H_
