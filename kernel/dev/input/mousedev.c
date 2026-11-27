// LikeOS input - the console pointer bridge (see mousedev.h)
//
// One struct mousedev per attached device.  Its state is changed in two
// places only: the events callback, which the input core calls with the
// device's event lock held, and the users_changed callback, which takes
// that lock itself -- so the event lock is the bridge's lock too.  Both
// may inject into the merged mouse (mouse_inject_usb_movement takes the
// mouse layer's lock and the merged mouse's event lock: the order is
// always this device's event lock -> mouse lock -> merged mouse's lock,
// and nothing takes them the other way round).
//
// Opening: the handle is opened from a work item, not from connect(),
// because connect() runs inside the registration of the device -- for a
// HID device inside its driver's probe -- and opening a HID device talks
// to it (hid-multitouch switches reporting on).  The work item opens it
// once the registration is over.
//
// The touchpad algorithm is the classic one for touchpads in absolute mode:
// motion from the single-touch position (ABS_X/ABS_Y, which the multitouch
// core keeps on the oldest contact), each packet's motion the difference to
// the position two packets before (a short average that smooths the
// sensor's noise), nothing for the first two packets of a touch (the
// landing finger settles), scaled by the pad's X span on both axes with the
// remainder carried over, a touch shorter than the tap time becoming a
// click.  It adds what a console needs and the classic one lacks: no jump
// when the number of fingers changes (the average starts over), two-finger
// wheel scrolling, two-finger taps and clicks as the right button, a
// movement limit for taps, no tap for a touch that clicked the pad.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 1999-2002 Vojtech Pavlik, 2004 Dmitry Torokhov

#include <kernel/dev/input/mousedev.h>
#include <kernel/dev/input/input.h>
#include <kernel/dev/input/mouse.h>
#include <kernel/ke/workqueue.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h> /* errno values */

/* Fixed-point fraction of the motion carried from packet to packet. */
#define MOUSEDEV_FRAC 128
/* A tap may move at most 1/MOUSEDEV_TAP_MOVE_DIV of the pad's X span. */
#define MOUSEDEV_TAP_MOVE_DIV 25

#define MOUSEDEV_BTN_LEFT 0x01
#define MOUSEDEV_BTN_RIGHT 0x02
#define MOUSEDEV_BTN_MIDDLE 0x04

struct mousedev {
	struct input_handle handle;
	struct work_struct open_work;
	int kind; // MOUSEDEV_KIND_*
	int i2c_touchpad; // takes part in the PS/2 preference
	const char *name; // the device's (valid while attached)

	// ---- state: the device's event lock ----
	int suppressed; // user space has the event node open
	/* touchpad */
	int touch; // a finger is down (BTN_TOUCH)
	uint64_t touch_ns; // when this touch began
	int fingers; // contacts by BTN_TOOL_*
	int max_fingers; // the most during this touch
	/* The device's ABS_X / ABS_Y (the core passes changes only, so the
	 * current values start from the device's state at attach time). */
	int x, y;
	int x_seen, y_seen; // reported in the packet in progress
	/* Positions of the last packets of this touch: old_x[pkt_count & 3]
	 * is this packet's, motion is measured from the one two packets back. */
	int old_x[4], old_y[4];
	unsigned int pkt_count; // packets of this touch (of this finger count)
	int last_fingers; // finger count at the previous packet
	int frac_x, frac_y; // motion remainder, 1/MOUSEDEV_FRAC units
	int scroll_acc; // wheel travel not yet a step
	unsigned int moved; // device units moved during this touch
	int button_in_touch; // the pad was clicked during this touch
	uint8_t pad_click; // the clickpad button, as left or right
	int pad_down; // BTN_LEFT of the pad is down
	/* mouse */
	uint8_t mouse_buttons;
	int rel_x, rel_y, rel_wheel; // this packet's
	/* what went to the merged mouse */
	uint8_t sent_buttons;
	unsigned long injected; // reports injected (diagnostics)
};

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

int mousedev_device_kind(const struct input_dev *dev)
{
	if (!dev || dev->static_storage)
		return MOUSEDEV_KIND_NONE;
	if (input_test_bit(EV_ABS, dev->evbit) &&
	    input_test_bit(EV_KEY, dev->evbit) &&
	    input_test_bit(ABS_X, dev->absbit) &&
	    input_test_bit(ABS_Y, dev->absbit) &&
	    input_test_bit(BTN_TOUCH, dev->keybit) &&
	    input_test_bit(BTN_TOOL_FINGER, dev->keybit) &&
	    !input_test_bit(INPUT_PROP_DIRECT, dev->propbit) &&
	    !input_test_bit(BTN_TOOL_PEN, dev->keybit))
		return MOUSEDEV_KIND_TOUCHPAD;
	if (input_test_bit(EV_REL, dev->evbit) &&
	    input_test_bit(EV_KEY, dev->evbit) &&
	    input_test_bit(REL_X, dev->relbit) &&
	    input_test_bit(REL_Y, dev->relbit) &&
	    input_test_bit(BTN_LEFT, dev->keybit))
		return MOUSEDEV_KIND_MOUSE;
	return MOUSEDEV_KIND_NONE;
}

int mousedev_is_i2c_touchpad(const struct input_dev *dev)
{
	return dev && dev->id.bustype == BUS_I2C &&
	       input_test_bit(INPUT_PROP_POINTER, dev->propbit) &&
	       input_test_bit(EV_ABS, dev->evbit) &&
	       input_test_bit(ABS_MT_SLOT, dev->absbit) &&
	       !input_test_bit(INPUT_PROP_DIRECT, dev->propbit);
}

// ---------------------------------------------------------------------------
// PS/2 preference (pure: the PS/2 driver keeps the state under its lock)
// ---------------------------------------------------------------------------

void mousedev_ps2_pref_init(struct mousedev_ps2_pref *p, int aux_present,
			    int aux_trackpoint)
{
	kmemset(p, 0, sizeof(*p));
	p->aux_present = aux_present;
	p->aux_trackpoint = aux_present && aux_trackpoint;
}

int mousedev_ps2_pref_touchpad(struct mousedev_ps2_pref *p, int contact)
{
	if (p->disabled || !p->aux_present || p->aux_trackpoint)
		return MOUSEDEV_PS2_PASS;
	/* The touchpad works: the PS/2 device is its emulation. */
	if (!contact)
		return MOUSEDEV_PS2_PASS;
	p->disabled = 1;
	return MOUSEDEV_PS2_DISABLE;
}

// ---------------------------------------------------------------------------
// Injection (event lock held)
// ---------------------------------------------------------------------------

static int8_t mousedev_clamp_wheel(int v)
{
	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

static void mousedev_inject(struct mousedev *md, int dx, int dy,
			    uint8_t buttons, int wheel)
{
	if (!dx && !dy && !wheel && buttons == md->sent_buttons)
		return;
	md->sent_buttons = buttons;
	md->injected++;
	mouse_inject_usb_movement(dx, dy, buttons, mousedev_clamp_wheel(wheel));
}

/* Let go of whatever the bridge holds down on the merged mouse. */
static void mousedev_release(struct mousedev *md)
{
	if (md->sent_buttons)
		mousedev_inject(md, 0, 0, 0, 0);
}

static void mousedev_touch_reset(struct mousedev *md)
{
	md->pkt_count = 0;
	md->frac_x = md->frac_y = 0;
	md->scroll_acc = 0;
	md->moved = 0;
	md->max_fingers = 0;
	md->button_in_touch = 0;
}

// ---------------------------------------------------------------------------
// Touchpad
// ---------------------------------------------------------------------------

static int mousedev_x_span(const struct input_dev *dev)
{
	int span = input_abs_get_max(dev, ABS_X) - input_abs_get_min(dev, ABS_X);

	/* A device without a range moves as a 512-unit wide pad would. */
	return span > 0 ? span : 512;
}

/* Device units -> pointer units with the remainder kept in *frac (the
 * division truncates toward zero, the remainder keeps the sign). */
static int mousedev_scale(int64_t delta, int span, int *frac)
{
	int64_t t = delta * MOUSEDEV_TP_UNITS * MOUSEDEV_FRAC / span +
		    *frac;
	int out;

	/* A jump of more than the whole pad is not a movement. */
	if (t > (int64_t)MOUSEDEV_TP_UNITS * MOUSEDEV_FRAC)
		t = (int64_t)MOUSEDEV_TP_UNITS * MOUSEDEV_FRAC;
	if (t < -(int64_t)MOUSEDEV_TP_UNITS * MOUSEDEV_FRAC)
		t = -(int64_t)MOUSEDEV_TP_UNITS * MOUSEDEV_FRAC;
	out = (int)(t / MOUSEDEV_FRAC);
	*frac = (int)(t - (int64_t)out * MOUSEDEV_FRAC);
	return out;
}

static void mousedev_tp_touch(struct mousedev *md, int down, uint64_t now_ns)
{
	if (down) {
		if (!md->touch) {
			md->touch = 1;
			md->touch_ns = now_ns;
			mousedev_touch_reset(md);
		}
		return;
	}
	if (!md->touch)
		return;
	md->touch = 0;
	/* A tap: short, hardly moved, not a click of the pad itself. */
	if (!md->suppressed && !md->button_in_touch &&
	    now_ns - md->touch_ns < (uint64_t)MOUSEDEV_TAP_TIME_MS * 1000000ull &&
	    md->moved <= (unsigned int)(mousedev_x_span(md->handle.dev) /
					MOUSEDEV_TAP_MOVE_DIV)) {
		uint8_t b = md->max_fingers >= 2 ? MOUSEDEV_BTN_RIGHT :
						   MOUSEDEV_BTN_LEFT;

		/* Press and release: two reports, as a click is. */
		mousedev_inject(md, 0, 0, (uint8_t)(md->sent_buttons | b), 0);
		mousedev_inject(md, 0, 0, (uint8_t)(md->sent_buttons & ~b), 0);
	}
	mousedev_touch_reset(md);
}

#define MOUSEDEV_FX(md, i) ((md)->old_x[((md)->pkt_count - (i)) & 3])
#define MOUSEDEV_FY(md, i) ((md)->old_y[((md)->pkt_count - (i)) & 3])

static unsigned int mousedev_absdiff(int a, int b)
{
	return a > b ? (unsigned int)a - (unsigned int)b :
		       (unsigned int)b - (unsigned int)a;
}

static void mousedev_tp_frame(struct mousedev *md)
{
	struct input_dev *dev = md->handle.dev;
	int dx = 0, dy = 0, wheel = 0;
	uint8_t buttons;

	if (md->fingers > md->max_fingers)
		md->max_fingers = md->fingers;
	/* A finger more or fewer moves the single-touch position to another
	 * contact: the average starts over from there. */
	if (md->fingers != md->last_fingers) {
		md->pkt_count = 0;
		md->frac_x = md->frac_y = 0;
		md->last_fingers = md->fingers;
	}

	if (md->touch) {
		int span = mousedev_x_span(dev);

		/* This packet's position: the reported one, or the last one
		 * when the axis did not change (the core drops repeats). */
		MOUSEDEV_FX(md, 0) = md->x;
		MOUSEDEV_FY(md, 0) = md->y;
		if (md->pkt_count >= 1) {
			unsigned int step =
				mousedev_absdiff(md->x, MOUSEDEV_FX(md, 1)) +
				mousedev_absdiff(md->y, MOUSEDEV_FY(md, 1));

			if (md->moved < 0x40000000u)
				md->moved += step < 0x40000000u ? step :
								  0x40000000u;
		}
		if (md->pkt_count >= 2) {
			if (md->fingers >= 2) {
				/* Two fingers: the wheel, up moves it away from
				 * the user (positive), as a wheel turned up. */
				if (md->y_seen) {
					int units = mousedev_scale(
						(int64_t)MOUSEDEV_FY(md, 2) -
							md->y,
						span, &md->frac_y);

					md->scroll_acc += units;
					wheel = md->scroll_acc /
						MOUSEDEV_SCROLL_UNITS;
					md->scroll_acc -=
						wheel * MOUSEDEV_SCROLL_UNITS;
				}
			} else {
				/* The same scale on both axes: the X span's. */
				if (md->x_seen)
					dx = mousedev_scale(
						(int64_t)md->x -
							MOUSEDEV_FX(md, 2),
						span, &md->frac_x);
				if (md->y_seen)
					dy = mousedev_scale(
						(int64_t)md->y -
							MOUSEDEV_FY(md, 2),
						span, &md->frac_y);
			}
		}
		md->pkt_count++;
	}
	md->x_seen = md->y_seen = 0;

	if (md->suppressed)
		return;
	buttons = md->pad_click;
	mousedev_inject(md, dx, dy, buttons, wheel);
}

static void mousedev_tp_value(struct mousedev *md, const struct input_value *v,
			      uint64_t now_ns)
{
	switch (v->type) {
	case EV_KEY:
		if (v->value == 2)
			return; // autorepeat
		switch (v->code) {
		case BTN_TOUCH:
			mousedev_tp_touch(md, v->value != 0, now_ns);
			break;
		case BTN_TOOL_FINGER:
			md->fingers = v->value ? 1 : (md->fingers == 1 ? 0 : md->fingers);
			break;
		case BTN_TOOL_DOUBLETAP:
			md->fingers = v->value ? 2 : (md->fingers == 2 ? 0 : md->fingers);
			break;
		case BTN_TOOL_TRIPLETAP:
			md->fingers = v->value ? 3 : (md->fingers == 3 ? 0 : md->fingers);
			break;
		case BTN_TOOL_QUADTAP:
			md->fingers = v->value ? 4 : (md->fingers == 4 ? 0 : md->fingers);
			break;
		case BTN_TOOL_QUINTTAP:
			md->fingers = v->value ? 5 : (md->fingers == 5 ? 0 : md->fingers);
			break;
		case BTN_LEFT:
			/* The clickpad's button: the right one when two
			 * fingers rest on the pad as it goes down. */
			md->pad_down = v->value != 0;
			if (md->pad_down) {
				md->pad_click = md->fingers >= 2 ?
							MOUSEDEV_BTN_RIGHT :
							MOUSEDEV_BTN_LEFT;
				md->button_in_touch = 1;
			} else {
				md->pad_click = 0;
			}
			break;
		case BTN_RIGHT:
			md->pad_click = v->value ?
						(uint8_t)(md->pad_click | MOUSEDEV_BTN_RIGHT) :
						(uint8_t)(md->pad_click & ~MOUSEDEV_BTN_RIGHT);
			if (v->value)
				md->button_in_touch = 1;
			break;
		case BTN_MIDDLE:
			md->pad_click = v->value ?
						(uint8_t)(md->pad_click | MOUSEDEV_BTN_MIDDLE) :
						(uint8_t)(md->pad_click & ~MOUSEDEV_BTN_MIDDLE);
			if (v->value)
				md->button_in_touch = 1;
			break;
		}
		break;
	case EV_ABS:
		if (v->code == ABS_X) {
			md->x = v->value;
			md->x_seen = 1;
		} else if (v->code == ABS_Y) {
			md->y = v->value;
			md->y_seen = 1;
		}
		break;
	}
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

static void mousedev_mouse_value(struct mousedev *md,
				 const struct input_value *v)
{
	uint8_t bit = 0;

	switch (v->type) {
	case EV_REL:
		if (v->code == REL_X)
			md->rel_x += v->value;
		else if (v->code == REL_Y)
			md->rel_y += v->value;
		else if (v->code == REL_WHEEL)
			md->rel_wheel += v->value;
		break;
	case EV_KEY:
		if (v->value == 2)
			return;
		if (v->code == BTN_LEFT)
			bit = MOUSEDEV_BTN_LEFT;
		else if (v->code == BTN_RIGHT)
			bit = MOUSEDEV_BTN_RIGHT;
		else if (v->code == BTN_MIDDLE)
			bit = MOUSEDEV_BTN_MIDDLE;
		if (bit)
			md->mouse_buttons = v->value ?
						    (uint8_t)(md->mouse_buttons | bit) :
						    (uint8_t)(md->mouse_buttons & ~bit);
		break;
	}
}

static void mousedev_mouse_frame(struct mousedev *md)
{
	int dx = md->rel_x, dy = md->rel_y, wheel = md->rel_wheel;

	md->rel_x = md->rel_y = md->rel_wheel = 0;
	if (md->suppressed)
		return;
	mousedev_inject(md, dx, dy, md->mouse_buttons, wheel);
}

// ---------------------------------------------------------------------------
// Input core callbacks
// ---------------------------------------------------------------------------

/* A packet of the device: event lock held, any context. */
static void mousedev_events(struct input_handle *handle,
			    const struct input_value *vals, unsigned int count,
			    uint64_t mono_ns)
{
	struct mousedev *md = handle->private;

	for (unsigned int i = 0; i < count; i++) {
		const struct input_value *v = &vals[i];

		if (v->type == EV_SYN) {
			if (v->code != SYN_REPORT)
				continue;
			if (md->kind == MOUSEDEV_KIND_TOUCHPAD) {
				mousedev_tp_frame(md);
				if (md->i2c_touchpad)
					mouse_i2c_touchpad_report(
						md->name,
						md->touch || md->pad_down);
			} else {
				mousedev_mouse_frame(md);
			}
			continue;
		}
		if (md->kind == MOUSEDEV_KIND_TOUCHPAD)
			mousedev_tp_value(md, v, mono_ns);
		else
			mousedev_mouse_value(md, v);
	}
}

/* User space opened the event node (users != 0) or closed it.  Process
 * context, the device's mutex held. */
static void mousedev_users_changed(struct input_handle *handle,
				   unsigned int users)
{
	struct mousedev *md = handle->private;
	struct input_dev *dev = handle->dev;
	uint64_t f;

	spin_lock_irqsave(&dev->event_lock, &f);
	md->suppressed = users != 0;
	/* Whoever takes over starts from a clean state: nothing held on the
	 * merged mouse, no half-made gesture. */
	mousedev_release(md);
	md->touch = 0;
	md->pad_click = 0;
	md->pad_down = 0;
	md->mouse_buttons = 0;
	md->rel_x = md->rel_y = md->rel_wheel = 0;
	md->x_seen = md->y_seen = 0;
	mousedev_touch_reset(md);
	spin_unlock_irqrestore(&dev->event_lock, f);
}

static void mousedev_open_work(struct work_struct *work)
{
	struct mousedev *md =
		(struct mousedev *)((char *)work -
				    __builtin_offsetof(struct mousedev,
						       open_work));
	int rc;

	rc = input_handle_open(&md->handle);
	if (rc && rc != -ENODEV)
		kprintf("mousedev: cannot open %s: %d\n", md->name, rc);
}

static int mousedev_connect(struct input_handler *handler,
			    struct input_dev *dev)
{
	struct mousedev *md;
	int kind = mousedev_device_kind(dev);
	int rc;

	might_sleep();
	if (kind == MOUSEDEV_KIND_NONE)
		return -ENODEV;
	md = kcalloc(1, sizeof(*md));
	if (!md)
		return -ENOMEM;
	md->kind = kind;
	md->i2c_touchpad = kind == MOUSEDEV_KIND_TOUCHPAD &&
			   mousedev_is_i2c_touchpad(dev);
	md->name = dev->name ? dev->name : "touchpad";
	md->handle.name = "mousedev";
	md->handle.handler = handler;
	md->handle.private = md;
	md->handle.events = mousedev_events;
	md->handle.users_changed = mousedev_users_changed;
	if (kind == MOUSEDEV_KIND_TOUCHPAD) {
		md->x = input_abs_get_val(dev, ABS_X);
		md->y = input_abs_get_val(dev, ABS_Y);
	}
	INIT_WORK(&md->open_work, mousedev_open_work);
	rc = input_register_handle(dev, &md->handle);
	if (rc) {
		kfree(md);
		return rc;
	}
	kprintf("mousedev: %s %s feeds the console pointer until a client opens it\n",
		kind == MOUSEDEV_KIND_TOUCHPAD ? "touchpad" : "mouse",
		md->name);
	queue_work(system_wq, &md->open_work);
	return 0;
}

static void mousedev_disconnect(struct input_handle *handle)
{
	struct mousedev *md = handle->private;
	struct input_dev *dev = handle->dev;
	uint64_t f;

	might_sleep();
	cancel_work_sync(&md->open_work);
	/* Buttons the device held stay down on the merged mouse otherwise. */
	spin_lock_irqsave(&dev->event_lock, &f);
	mousedev_release(md);
	spin_unlock_irqrestore(&dev->event_lock, f);
	input_unregister_handle(handle);
	kfree(md);
}

static struct input_handler mousedev_handler = {
	.name = "mousedev",
	.connect = mousedev_connect,
	.disconnect = mousedev_disconnect,
};

void mousedev_init(void)
{
	int rc;

	might_sleep();
	rc = input_register_handler(&mousedev_handler);
	if (rc && rc != -EBUSY)
		kprintf("mousedev: registration failed: %d\n", rc);
}

void mousedev_exit(void)
{
	might_sleep();
	input_unregister_handler(&mousedev_handler);
}
