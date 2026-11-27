/* Test scenarios of the input core harness (host/test-input.sh).
 *
 * Kernel-side unit: compiled with the prelude against the kernel headers,
 * linked with the REAL kernel/dev/input/input.c, input-mt.c and evdev.c.
 * The "user buffers" handed to the event-device calls are ordinary host
 * memory (they lie in the range the kernel's user-pointer check accepts);
 * the bad pointers are ones it rejects.
 *
 * Groups:
 *   builtin     the keyboard (event0) and mouse (event1) streams the
 *               keyboard/mouse drivers produce, unchanged
 *   filter      key/abs/rel/fuzz filtering, empty packets, packet overflow
 *   mt          a simulated five-slot touchpad: slot events, tracking ids,
 *               BTN_TOOL_*, pointer emulation, palms, wrap, assignment
 *   ioctl       every EVIOC* request, sizes, bad pointers, errors
 *   clients     per-client queues, grab, revoke, masks, clocks, overflow
 *               and resynchronisation, state-read queue flush
 *   proc        /proc/bus/input/devices against the dumps in kernel/spec
 *   lifecycle   numbering, open/close callbacks, unregister with readers,
 *               device events
 *   race        threads: blocking readers, producers in "interrupt
 *               context", register/unregister and open/close loops
 *   handles     in-kernel listeners: handler connect for devices before
 *               and after it, kernel vs user open counting, users_changed,
 *               packet delivery only while open, disconnect on removal and
 *               on handler unregistration, a detach racing a producer
 *   fidelity    software autorepeat, the device callback only while open
 *               (with the state replay), key code tables, total-force
 *               pressure, slot assignment without a distance limit and on
 *               random contacts, more slots than one copy pass, buffer
 *               sizing, write limit, SYN_DROPPED time, split-packet time,
 *               open() reports, input_copy_abs, grab vs in-kernel handles
 *   keyboard    the console's share of the keyboard (keyboard.c, keymap.c):
 *               no key reaches the console in K_OFF or under the grab,
 *               event0 gets every key, no modifier stays held across the
 *               hand-over, the hotkeys stay quiet, the LEDs follow whoever
 *               has the keyboard; KD_GRAPHICS; the restore when the file
 *               that set the modes closes (and only then); permissions;
 *               keys racing mode changes and closes; the console's output
 *               gate (console_owner.h): KD_GRAPHICS or a DRM master owns
 *               the display, whichever is withdrawn last repaints, once
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/dev/input/input.h>
#include <kernel/dev/input/input-mt.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/dev/input/keyboard.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/dev/video/console_owner.h>
#include <kernel/uapi/kd.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/sched.h>
#include <kernel/io/tty.h>
#include "kstubs.h"
#include "../hid/wqshim.h"

static long g_checks, g_fails;

#define CHECK(cond, ...)                                                  \
	do {                                                              \
		__atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED);       \
		if (!(cond)) {                                            \
			__atomic_fetch_add(&g_fails, 1, __ATOMIC_RELAXED); \
			h_printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
			h_printf(__VA_ARGS__);                            \
			h_printf("\n");                                   \
		}                                                         \
	} while (0)

#define EV_SZ ((long)sizeof(struct input_event))
#define BAD_PTR ((void *)8UL) // below the lowest user address
#define KERNEL_PTR ((void *)0xffff800000001000UL)

struct ev {
	uint16_t type, code;
	int32_t value;
};

#define SYN \
	{ EV_SYN, SYN_REPORT, 0 }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static const char *type_name(unsigned t)
{
	switch (t) {
	case EV_SYN:
		return "SYN";
	case EV_KEY:
		return "KEY";
	case EV_REL:
		return "REL";
	case EV_ABS:
		return "ABS";
	case EV_MSC:
		return "MSC";
	case EV_LED:
		return "LED";
	case EV_REP:
		return "REP";
	default:
		return "?";
	}
}

/* Everything readable right now (nonblocking). */
static int drain(struct evdev_client *c, struct input_event *out, int max)
{
	long r = evdev_read(c, out, max * EV_SZ, 1);

	if (r == -EAGAIN)
		return 0;
	if (r < 0)
		return (int)r;
	return (int)(r / EV_SZ);
}

static void dump_stream(const char *label, const struct input_event *got,
			int n)
{
	h_printf("  %s (%d):", label, n);
	for (int i = 0; i < n; i++)
		h_printf(" %s/%#x/%d", type_name(got[i].type), got[i].code,
			 got[i].value);
	h_printf("\n");
}

static int expect_stream_(const char *what, int line, struct evdev_client *c,
			  const struct ev *exp, int n)
{
	struct input_event got[1100];
	int k = drain(c, got, 1100), ok = k == n;

	for (int i = 0; ok && i < n; i++)
		ok = got[i].type == exp[i].type && got[i].code == exp[i].code &&
		     got[i].value == exp[i].value;
	__atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED);
	if (!ok) {
		struct input_event want[256];

		__atomic_fetch_add(&g_fails, 1, __ATOMIC_RELAXED);
		h_printf("FAIL %s:%d: stream of %s\n", __FILE__, line, what);
		for (int i = 0; i < n && i < 256; i++) {
			want[i].type = exp[i].type;
			want[i].code = exp[i].code;
			want[i].value = exp[i].value;
		}
		dump_stream("want", want, n < 256 ? n : 256);
		dump_stream("got ", got, k < 0 ? 0 : k);
	}
	return ok;
}

#define expect_stream(what, c, ...)                                         \
	do {                                                                \
		static const struct ev exp_[] = { __VA_ARGS__ };            \
		expect_stream_(what, __LINE__, c, exp_,                     \
			       (int)(sizeof(exp_) / sizeof(exp_[0])));      \
	} while (0)

#define expect_empty(what, c) expect_stream_(what, __LINE__, c, NULL, 0)

static struct evdev_client *open_unit(int unit)
{
	struct evdev_client *c = NULL;
	int rc = evdev_open(unit, &c);

	CHECK(rc == 0 && c, "open event%d: %d", unit, rc);
	return rc ? NULL : c;
}

static int unit_of(struct input_dev *dev)
{
	return evdev_minor_of(dev->evdev);
}

/* What hid-multitouch builds for a Windows precision touchpad. */
static struct input_dev *mk_touchpad(const char *name, uint16_t vendor,
				     uint16_t product, const char *phys,
				     const char *devpath, unsigned int flags,
				     unsigned int slots)
{
	struct input_dev *dev = input_allocate_device();

	CHECK(dev != NULL, "allocate touchpad");
	if (!dev)
		h_abort();
	dev->name = name;
	dev->phys = phys;
	dev->devpath = devpath;
	dev->id.bustype = BUS_I2C;
	dev->id.vendor = vendor;
	dev->id.product = product;
	dev->id.version = 0x0100;
	input_set_abs_params(dev, ABS_MT_POSITION_X, 0, 3209, 0, 0);
	input_abs_set_res(dev, ABS_MT_POSITION_X, 31);
	input_set_abs_params(dev, ABS_MT_POSITION_Y, 0, 2097, 0, 0);
	input_abs_set_res(dev, ABS_MT_POSITION_Y, 31);
	input_set_abs_params(dev, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER,
			     MT_TOOL_PALM, 0, 0);
	input_set_capability(dev, EV_KEY, BTN_LEFT);
	input_set_capability(dev, EV_MSC, MSC_TIMESTAMP);
	input_set_bit(INPUT_PROP_BUTTONPAD, dev->propbit);
	CHECK(input_mt_init_slots(dev, slots,
				  INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED |
					  flags) == 0,
	      "init slots");
	return dev;
}

static struct input_dev *mk_hp_touchpad(void)
{
	return mk_touchpad(
		"ELAN073B:00 04F3:3169 Touchpad", 0x04f3, 0x3169,
		"i2c-ELAN073B:00",
		"/devices/pci0000:00/0000:00:15.0/i2c_designware.0/i2c-0/"
		"i2c-ELAN073B:00/0018:04F3:3169.0001",
		0, 5);
}

struct contact {
	int slot, x, y, tool, active;
};

/* One hardware frame the way hid-multitouch reports it. */
static void tp_frame(struct input_dev *dev, const struct contact *c, int n,
		     int ts, int button)
{
	for (int i = 0; i < n; i++) {
		input_mt_slot(dev, c[i].slot);
		if (input_mt_report_slot_state(dev, (unsigned)c[i].tool,
					       c[i].active)) {
			input_report_abs(dev, ABS_MT_POSITION_X, c[i].x);
			input_report_abs(dev, ABS_MT_POSITION_Y, c[i].y);
		}
	}
	input_mt_sync_frame(dev);
	input_event(dev, EV_MSC, MSC_TIMESTAMP, ts);
	input_report_key(dev, BTN_LEFT, button);
	input_sync(dev);
}

static int ioc(struct evdev_client *c, unsigned long req, void *arg)
{
	return evdev_ioctl(c, req, arg);
}

// ---------------------------------------------------------------------------
// builtin: event0 / event1 streams
// ---------------------------------------------------------------------------

static void test_builtin(void)
{
	struct evdev_client *kc, *mc;
	char name[64];
	struct input_id id;

	h_printf("== builtin\n");
	CHECK(evdev_unit_exists(0) && evdev_unit_exists(1),
	      "event0/event1 exist");
	CHECK(!evdev_unit_exists(2), "event2 does not exist yet");
	CHECK(evdev_next_unit(0) == 0 && evdev_next_unit(1) == 1 &&
		      evdev_next_unit(2) == -1,
	      "listing is exactly event0 event1");

	kc = open_unit(0);
	mc = open_unit(1);
	CHECK(evdev_client_unit(kc) == 0 && evdev_client_unit(mc) == 1,
	      "client units");
	CHECK(ioc(kc, EVIOCGNAME(sizeof(name)), name) == 16 &&
		      h_strcmp(name, "LikeOS Keyboard") == 0,
	      "keyboard name");
	CHECK(ioc(kc, EVIOCGID, &id) == 0 && id.bustype == BUS_I8042 &&
		      id.vendor == 1 && id.product == 1 &&
		      id.version == 0x0100,
	      "keyboard id");
	CHECK(ioc(mc, EVIOCGID, &id) == 0 && id.product == 2, "mouse id");

	/* Keys: press, typematic repeat, release, release again (dropped),
	 * an extended key, Pause, Hangul (press without release code). */
	CHECK(evdev_feed_keyboard(0x1E) == 0, "not grabbed");
	evdev_feed_keyboard(0x1E);
	evdev_feed_keyboard(0x9E);
	evdev_feed_keyboard(0x9E);
	evdev_feed_keyboard(0xE0);
	evdev_feed_keyboard(0x48);
	evdev_feed_keyboard(0xE0);
	evdev_feed_keyboard(0xC8);
	evdev_feed_keyboard(0xE1);
	evdev_feed_keyboard(0x1D);
	evdev_feed_keyboard(0x45);
	evdev_feed_keyboard(0xE1);
	evdev_feed_keyboard(0x9D);
	evdev_feed_keyboard(0xC5);
	evdev_feed_keyboard(0xF2);
	evdev_feed_keyboard(0x00); // no key
	expect_stream("keyboard", kc, { EV_KEY, KEY_A, 1 }, SYN,
		      { EV_KEY, KEY_A, 2 }, SYN, { EV_KEY, KEY_A, 0 }, SYN,
		      { EV_KEY, KEY_UP, 1 }, SYN, { EV_KEY, KEY_UP, 0 }, SYN,
		      { EV_KEY, KEY_PAUSE, 1 }, SYN, { EV_KEY, KEY_PAUSE, 0 },
		      SYN, { EV_KEY, KEY_HANGEUL, 1 }, SYN,
		      { EV_KEY, KEY_HANGEUL, 0 }, SYN);

	/* Mouse: button edges each in their own packet, then motion. */
	evdev_feed_mouse(5, -3, 0x01, 1, 0);
	evdev_feed_mouse(0, 0, 0x01, 0, 0); // nothing at all
	evdev_feed_mouse(0, 2, 0x03, 0, -1);
	evdev_feed_mouse(0, 0, 0x00, 0, 0);
	expect_stream("mouse", mc, { EV_KEY, BTN_LEFT, 1 }, SYN,
		      { EV_REL, REL_X, 5 }, { EV_REL, REL_Y, -3 },
		      { EV_REL, REL_WHEEL, 1 }, SYN, { EV_KEY, BTN_RIGHT, 1 },
		      SYN, { EV_REL, REL_Y, 2 }, { EV_REL, REL_HWHEEL, -1 },
		      SYN, { EV_KEY, BTN_LEFT, 0 }, SYN,
		      { EV_KEY, BTN_RIGHT, 0 }, SYN);

	/* The keyboard's LEDs: what a display server writes comes back to
	 * every reader, LEDs the device lacks are ignored. */
	{
		struct input_event w[4];
		unsigned long leds = 0;

		h_memset(w, 0, sizeof(w));
		w[0].type = EV_LED, w[0].code = LED_CAPSL, w[0].value = 1;
		w[1].type = EV_LED, w[1].code = LED_KANA, w[1].value = 1;
		w[2].type = EV_SYN, w[2].code = SYN_REPORT;
		CHECK(evdev_write(kc, w, 3 * EV_SZ) == 3 * EV_SZ, "LED write");
		expect_stream("LED echo", kc, { EV_LED, LED_CAPSL, 1 }, SYN);
		CHECK(ioc(kc, EVIOCGLED(sizeof(leds)), &leds) == 8 &&
			      leds == (1UL << LED_CAPSL),
		      "LED state %lx", leds);
		CHECK(evdev_write(kc, w, 3 * EV_SZ) == 3 * EV_SZ, "LED again");
		expect_empty("unchanged LED writes nothing", kc);
		w[0].value = 0;
		CHECK(evdev_write(kc, w, 3 * EV_SZ) == 3 * EV_SZ, "LED off");
		expect_stream("LED off", kc, { EV_LED, LED_CAPSL, 0 }, SYN);
		CHECK(evdev_write(kc, w, 0) == 0, "zero-length write");
		CHECK(evdev_write(kc, w, EV_SZ - 1) == -EINVAL, "short write");
		CHECK(evdev_write(kc, BAD_PTR, EV_SZ) == -EFAULT, "bad write");
	}

	/* Grab: the tty path is suppressed, the grabbing reader still reads,
	 * another reader gets nothing. */
	{
		struct evdev_client *k2 = open_unit(0);

		CHECK(ioc(kc, EVIOCGRAB, (void *)1) == 0, "grab");
		CHECK(evdev_kbd_grabbed() && !evdev_mouse_grabbed(),
		      "grab state");
		CHECK(ioc(k2, EVIOCGRAB, (void *)1) == -EBUSY, "second grab");
		CHECK(ioc(k2, EVIOCGRAB, (void *)0) == -EINVAL,
		      "ungrab by non-holder");
		CHECK(evdev_feed_keyboard(0x30) == 1, "grabbed: skip tty");
		evdev_feed_keyboard(0xB0);
		expect_stream("grabber", kc, { EV_KEY, KEY_B, 1 }, SYN,
			      { EV_KEY, KEY_B, 0 }, SYN);
		expect_empty("not the grabber", k2);
		/* Closing the grabbing descriptor releases the grab. */
		evdev_release(kc);
		CHECK(!evdev_kbd_grabbed(), "close releases the grab");
		evdev_feed_keyboard(0x2E);
		evdev_feed_keyboard(0xAE);
		expect_stream("after grab", k2, { EV_KEY, KEY_C, 1 }, SYN,
			      { EV_KEY, KEY_C, 0 }, SYN);
		kc = k2;
	}

	/* Absolute mode (VMware pointer): ABS_X/ABS_Y replace REL_X/REL_Y,
	 * clamped, unchanged positions dropped, relative motion from another
	 * source scaled by the screen size. */
	{
		struct input_absinfo ai;
		unsigned long relb = 0, props = 0;

		evdev_mouse_set_absolute(0xffff, 1024, 768);
		CHECK(ioc(mc, EVIOCGABS(ABS_X), &ai) == 0 &&
			      ai.maximum == 0xffff && ai.value == 0x7fff,
		      "absolute range");
		CHECK(ioc(mc, EVIOCGBIT(EV_REL, sizeof(relb)), &relb) == 8 &&
			      relb == ((1UL << REL_WHEEL) |
				       (1UL << REL_HWHEEL)),
		      "rel bits %lx", relb);
		CHECK(ioc(mc, EVIOCGPROP(sizeof(props)), &props) == 8 &&
			      props == 0,
		      "no pointer property");
		evdev_feed_mouse_abs(100, 200, 0, 0);
		evdev_feed_mouse_abs(100, 200, 0, 0);
		evdev_feed_mouse_abs(70000, 200, 0, -1);
		evdev_feed_mouse(1024, 0, 0, 0, 0); // a screen width right
		evdev_feed_mouse(-1, 0, 0, 0, 0);
		expect_stream("absolute", mc, { EV_ABS, ABS_X, 100 },
			      { EV_ABS, ABS_Y, 200 }, SYN,
			      { EV_ABS, ABS_X, 0xffff },
			      { EV_REL, REL_WHEEL, -1 }, SYN,
			      { EV_ABS, ABS_X, 0xffff - 64 }, SYN);
	}
	evdev_release(kc);
	evdev_release(mc);
}

// ---------------------------------------------------------------------------
// filter: the core's event filtering and packet handling
// ---------------------------------------------------------------------------

static void test_filter(void)
{
	struct input_dev *dev = input_allocate_device();
	struct evdev_client *c;

	h_printf("== filter\n");
	dev->name = "filter test";
	input_set_capability(dev, EV_KEY, BTN_LEFT);
	input_set_capability(dev, EV_REL, REL_X);
	input_set_capability(dev, EV_MSC, MSC_SCAN);
	input_set_capability(dev, EV_SW, SW_LID);
	input_set_abs_params(dev, ABS_PRESSURE, 0, 255, 4, 0);
	CHECK(input_register_device(dev) == 0, "register");
	c = open_unit(unit_of(dev));

	input_report_key(dev, BTN_LEFT, 1);
	input_report_key(dev, BTN_LEFT, 1); // no change
	input_event(dev, EV_KEY, BTN_LEFT, 2); // repeat while down
	input_report_key(dev, BTN_RIGHT, 1); // not declared
	input_report_rel(dev, REL_X, 0); // no motion
	input_report_rel(dev, REL_Y, 4); // not declared
	input_event(dev, EV_LED, LED_NUML, 1); // type not declared
	input_report_switch(dev, SW_LID, 1);
	input_sync(dev);
	input_sync(dev); // empty packet
	input_event(dev, EV_SYN, SYN_DROPPED, 0); // never from a driver
	input_report_key(dev, BTN_LEFT, 0);
	/* Autorepeat changes no state and passes for any key the device
	 * has, down or not. */
	input_event(dev, EV_KEY, BTN_LEFT, 2); // repeat while up
	input_event(dev, EV_KEY, BTN_RIGHT, 2); // not declared
	input_report_rel(dev, REL_X, -7);
	input_sync(dev);
	expect_stream("filter", c, { EV_KEY, BTN_LEFT, 1 },
		      { EV_KEY, BTN_LEFT, 2 }, { EV_SW, SW_LID, 1 }, SYN,
		      { EV_KEY, BTN_LEFT, 0 }, { EV_KEY, BTN_LEFT, 2 },
		      { EV_REL, REL_X, -7 }, SYN);
	CHECK(!(dev->key[BTN_LEFT / 64] & (1UL << (BTN_LEFT % 64))),
	      "a repeat does not press the key");

	/* Fuzz 4: within 2 is noise, within 4 a quarter step, within 8 a
	 * half step. */
	input_report_abs(dev, ABS_PRESSURE, 100);
	input_sync(dev);
	input_report_abs(dev, ABS_PRESSURE, 101);
	input_sync(dev);
	input_report_abs(dev, ABS_PRESSURE, 103);
	input_sync(dev);
	input_report_abs(dev, ABS_PRESSURE, 107);
	input_sync(dev);
	input_report_abs(dev, ABS_PRESSURE, 200);
	input_sync(dev);
	expect_stream("fuzz", c, { EV_ABS, ABS_PRESSURE, 100 }, SYN,
		      { EV_ABS, ABS_PRESSURE, 103 }, SYN,
		      { EV_ABS, ABS_PRESSURE, 200 }, SYN);

	/* More events than the packet buffer holds: the core ends packets
	 * itself (SYN_REPORT value 1) instead of losing events. */
	{
		struct input_event got[64];
		int n, i, msc = 0, auto_syn = 0;

		CHECK(dev->max_vals == dev->hint_events_per_packet + 2,
		      "packet buffer size");
		for (i = 0; i < 25; i++)
			input_event(dev, EV_MSC, MSC_SCAN, i);
		input_sync(dev);
		n = drain(c, got, 64);
		for (i = 0; i < n; i++) {
			if (got[i].type == EV_MSC)
				CHECK(got[i].value == msc++, "msc order");
			if (got[i].type == EV_SYN && got[i].value == 1)
				auto_syn++;
		}
		CHECK(msc == 25 && auto_syn >= 1 && got[n - 1].type == EV_SYN &&
			      got[n - 1].value == 0,
		      "packet overflow: %d msc, %d core syncs", msc, auto_syn);
	}

	/* State bitmaps follow the events. */
	{
		unsigned long sw = 0, key[12];

		CHECK(ioc(c, EVIOCGSW(sizeof(sw)), &sw) == 8 &&
			      sw == (1UL << SW_LID),
		      "switch state");
		input_report_key(dev, BTN_LEFT, 1);
		input_sync(dev);
		CHECK(ioc(c, EVIOCGKEY(sizeof(key)), key) == 96 &&
			      (key[BTN_LEFT / 64] >> (BTN_LEFT % 64)) & 1,
		      "key state");
	}
	evdev_release(c);
	input_unregister_device(dev);
}

// ---------------------------------------------------------------------------
// mt: the simulated touchpad
// ---------------------------------------------------------------------------

static void test_mt(void)
{
	struct input_dev *dev = mk_hp_touchpad();
	struct evdev_client *c;

	h_printf("== mt\n");
	CHECK(input_register_device(dev) == 0, "register touchpad");
	c = open_unit(unit_of(dev));

	{
		static const struct contact a[] = { { 0, 100, 200, 0, 1 } };
		tp_frame(dev, a, 1, 100, 0);
	}
	expect_stream("one finger", c, { EV_ABS, ABS_MT_TRACKING_ID, 0 },
		      { EV_ABS, ABS_MT_POSITION_X, 100 },
		      { EV_ABS, ABS_MT_POSITION_Y, 200 },
		      { EV_KEY, BTN_TOUCH, 1 }, { EV_KEY, BTN_TOOL_FINGER, 1 },
		      { EV_ABS, ABS_X, 100 }, { EV_ABS, ABS_Y, 200 },
		      { EV_MSC, MSC_TIMESTAMP, 100 }, SYN);
	{
		static const struct contact b[] = { { 0, 110, 200, 0, 1 },
						    { 1, 500, 600, 0, 1 } };
		tp_frame(dev, b, 2, 200, 0);
	}
	expect_stream("second finger", c, { EV_ABS, ABS_MT_POSITION_X, 110 },
		      { EV_ABS, ABS_MT_SLOT, 1 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 1 },
		      { EV_ABS, ABS_MT_POSITION_X, 500 },
		      { EV_ABS, ABS_MT_POSITION_Y, 600 },
		      { EV_KEY, BTN_TOOL_FINGER, 0 },
		      { EV_KEY, BTN_TOOL_DOUBLETAP, 1 },
		      { EV_ABS, ABS_X, 110 }, { EV_MSC, MSC_TIMESTAMP, 200 },
		      SYN);
	{
		/* The first finger is not reported: dropped as unused. */
		static const struct contact cc[] = { { 1, 500, 600, 0, 1 } };
		tp_frame(dev, cc, 1, 300, 0);
	}
	expect_stream("first lifted", c, { EV_ABS, ABS_MT_SLOT, 0 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_KEY, BTN_TOOL_FINGER, 1 },
		      { EV_KEY, BTN_TOOL_DOUBLETAP, 0 },
		      { EV_ABS, ABS_X, 500 }, { EV_ABS, ABS_Y, 600 },
		      { EV_MSC, MSC_TIMESTAMP, 300 }, SYN);
	{
		static const struct contact d[] = { { 1, 0, 0, 0, 0 } };
		tp_frame(dev, d, 1, 400, 0);
	}
	expect_stream("all lifted", c, { EV_ABS, ABS_MT_SLOT, 1 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_KEY, BTN_TOUCH, 0 }, { EV_KEY, BTN_TOOL_FINGER, 0 },
		      { EV_MSC, MSC_TIMESTAMP, 400 }, SYN);
	{
		static const struct contact e[] = {
			{ 0, 1000, 1000, 0, 1 }, { 1, 1010, 1020, 0, 1 },
			{ 2, 1020, 1040, 0, 1 }, { 3, 1030, 1060, 0, 1 },
			{ 4, 1040, 1080, 0, 1 }
		};
		tp_frame(dev, e, 5, 500, 1);
	}
	expect_stream("five fingers and a click", c, { EV_ABS, ABS_MT_SLOT, 0 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 2 },
		      { EV_ABS, ABS_MT_POSITION_X, 1000 },
		      { EV_ABS, ABS_MT_POSITION_Y, 1000 },
		      { EV_ABS, ABS_MT_SLOT, 1 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 3 },
		      { EV_ABS, ABS_MT_POSITION_X, 1010 },
		      { EV_ABS, ABS_MT_POSITION_Y, 1020 },
		      { EV_ABS, ABS_MT_SLOT, 2 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 4 },
		      { EV_ABS, ABS_MT_POSITION_X, 1020 },
		      { EV_ABS, ABS_MT_POSITION_Y, 1040 },
		      { EV_ABS, ABS_MT_SLOT, 3 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 5 },
		      { EV_ABS, ABS_MT_POSITION_X, 1030 },
		      { EV_ABS, ABS_MT_POSITION_Y, 1060 },
		      { EV_ABS, ABS_MT_SLOT, 4 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 6 },
		      { EV_ABS, ABS_MT_POSITION_X, 1040 },
		      { EV_ABS, ABS_MT_POSITION_Y, 1080 },
		      { EV_KEY, BTN_TOUCH, 1 },
		      { EV_KEY, BTN_TOOL_QUINTTAP, 1 },
		      { EV_ABS, ABS_X, 1000 }, { EV_ABS, ABS_Y, 1000 },
		      { EV_MSC, MSC_TIMESTAMP, 500 }, { EV_KEY, BTN_LEFT, 1 },
		      SYN);
	{
		/* The same frame again changes nothing but the timestamp;
		 * then the middle contact turns out to be a palm. */
		static const struct contact f[] = {
			{ 0, 1000, 1000, 0, 1 }, { 1, 1010, 1020, 0, 1 },
			{ 2, 1020, 1040, MT_TOOL_PALM, 1 },
			{ 3, 1030, 1060, 0, 1 }, { 4, 1040, 1080, 0, 1 }
		};
		static const struct contact same[] = {
			{ 0, 1000, 1000, 0, 1 }, { 1, 1010, 1020, 0, 1 },
			{ 2, 1020, 1040, 0, 1 }, { 3, 1030, 1060, 0, 1 },
			{ 4, 1040, 1080, 0, 1 }
		};
		tp_frame(dev, same, 5, 550, 1);
		tp_frame(dev, f, 5, 600, 1);
	}
	expect_stream("repeat frame, palm", c, { EV_MSC, MSC_TIMESTAMP, 550 },
		      SYN, { EV_ABS, ABS_MT_SLOT, 2 },
		      { EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM },
		      { EV_MSC, MSC_TIMESTAMP, 600 }, SYN);

	/* EVIOCGMTSLOTS sees the slots as the stream left them. */
	{
		INPUT_MT_REQUEST_LAYOUT(req5, 5) r;
		int32_t big[8];

		h_memset(&r, 0x55, sizeof(r));
		r.code = ABS_MT_TRACKING_ID;
		CHECK(ioc(c, EVIOCGMTSLOTS(sizeof(r)), &r) == 0 &&
			      r.values[0] == 2 && r.values[1] == 3 &&
			      r.values[2] == 4 && r.values[3] == 5 &&
			      r.values[4] == 6,
		      "MTSLOTS tracking ids");
		r.code = ABS_MT_TOOL_TYPE;
		CHECK(ioc(c, EVIOCGMTSLOTS(sizeof(r)), &r) == 0 &&
			      r.values[2] == MT_TOOL_PALM && r.values[1] == 0,
		      "MTSLOTS tool types");
		/* Room for three slots: exactly three written. */
		h_memset(big, 0x55, sizeof(big));
		big[0] = ABS_MT_POSITION_X;
		CHECK(ioc(c, EVIOCGMTSLOTS(4 + 3 * 4), big) == 0 &&
			      big[1] == 1000 && big[2] == 1010 &&
			      big[3] == 1020 && big[4] == 0x55555555,
		      "MTSLOTS cut to the buffer");
		/* A buffer larger than the slot count: the rest untouched. */
		h_memset(big, 0x55, sizeof(big));
		big[0] = ABS_MT_POSITION_Y;
		CHECK(ioc(c, EVIOCGMTSLOTS(sizeof(big)), big) == 0 &&
			      big[5] == 1080 && big[6] == 0x55555555,
		      "MTSLOTS larger buffer");
		big[0] = ABS_MT_SLOT;
		CHECK(ioc(c, EVIOCGMTSLOTS(sizeof(big)), big) == -EINVAL,
		      "MTSLOTS of ABS_MT_SLOT");
		big[0] = ABS_X;
		CHECK(ioc(c, EVIOCGMTSLOTS(sizeof(big)), big) == -EINVAL,
		      "MTSLOTS of a single-touch axis");
		CHECK(ioc(c, EVIOCGMTSLOTS(2), big) == -EINVAL,
		      "MTSLOTS without room for the code");
		CHECK(ioc(c, EVIOCGMTSLOTS(sizeof(big)), BAD_PTR) == -EFAULT,
		      "MTSLOTS bad pointer");
		CHECK(ioc(c, EVIOCGMTSLOTS(sizeof(big)), KERNEL_PTR) ==
			      -EFAULT,
		      "MTSLOTS kernel pointer");
	}
	{
		struct input_absinfo ai;

		/* The slot readers last heard about (the palm's). */
		CHECK(ioc(c, EVIOCGABS(ABS_MT_SLOT), &ai) == 0 &&
			      ai.value == 2 && ai.maximum == 4,
		      "current slot %d", ai.value);
		CHECK(ioc(c, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 &&
			      ai.maximum == 3209 && ai.resolution == 31,
		      "position axis");
		CHECK(ioc(c, EVIOCGABS(ABS_X), &ai) == 0 &&
			      ai.maximum == 3209 && ai.value == 1000 &&
			      ai.fuzz == 0 && ai.resolution == 31,
		      "emulated axis");
		CHECK(ioc(c, EVIOCGABS(ABS_MT_TRACKING_ID), &ai) == 0 &&
			      ai.maximum == INPUT_MT_TRKID_MAX,
		      "tracking id range");
	}

	/* Everything lifts at once (DROP_UNUSED with an empty frame). */
	tp_frame(dev, NULL, 0, 700, 0);
	expect_stream("lift all", c, { EV_ABS, ABS_MT_SLOT, 0 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_ABS, ABS_MT_SLOT, 1 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_ABS, ABS_MT_SLOT, 2 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_ABS, ABS_MT_SLOT, 3 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_ABS, ABS_MT_SLOT, 4 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_KEY, BTN_TOUCH, 0 },
		      { EV_KEY, BTN_TOOL_QUINTTAP, 0 },
		      { EV_MSC, MSC_TIMESTAMP, 700 }, { EV_KEY, BTN_LEFT, 0 },
		      SYN);

	/* Tracking-id wrap: the oldest contact is the one furthest behind
	 * in the wrapping id space. */
	dev->mt->trkid = 0xfffe;
	{
		static const struct contact w1[] = { { 0, 10, 10, 0, 1 },
						     { 1, 20, 20, 0, 1 } };
		static const struct contact w2[] = { { 1, 20, 20, 0, 1 } };
		static const struct contact w3[] = { { 0, 30, 30, 0, 1 },
						     { 1, 20, 20, 0, 1 } };
		tp_frame(dev, w1, 2, 800, 0);
		tp_frame(dev, w2, 1, 801, 0);
		tp_frame(dev, w3, 2, 802, 0);
	}
	expect_stream("tracking id wrap", c, { EV_ABS, ABS_MT_SLOT, 0 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 0xfffe },
		      { EV_ABS, ABS_MT_POSITION_X, 10 },
		      { EV_ABS, ABS_MT_POSITION_Y, 10 },
		      { EV_ABS, ABS_MT_SLOT, 1 },
		      { EV_ABS, ABS_MT_TRACKING_ID, 0xffff },
		      { EV_ABS, ABS_MT_POSITION_X, 20 },
		      { EV_ABS, ABS_MT_POSITION_Y, 20 },
		      { EV_KEY, BTN_TOUCH, 1 },
		      { EV_KEY, BTN_TOOL_DOUBLETAP, 1 },
		      { EV_ABS, ABS_X, 10 }, { EV_ABS, ABS_Y, 10 },
		      { EV_MSC, MSC_TIMESTAMP, 800 }, SYN,
		      /* slot 0 lifted: slot 1 (0xffff) is the oldest */
		      { EV_ABS, ABS_MT_SLOT, 0 },
		      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
		      { EV_KEY, BTN_TOOL_FINGER, 1 },
		      { EV_KEY, BTN_TOOL_DOUBLETAP, 0 },
		      { EV_ABS, ABS_X, 20 }, { EV_ABS, ABS_Y, 20 },
		      { EV_MSC, MSC_TIMESTAMP, 801 }, SYN,
		      /* a new contact gets id 0 after the wrap; 0xffff is
		       * still the oldest */
		      { EV_ABS, ABS_MT_TRACKING_ID, 0 },
		      { EV_ABS, ABS_MT_POSITION_X, 30 },
		      { EV_ABS, ABS_MT_POSITION_Y, 30 },
		      { EV_KEY, BTN_TOOL_FINGER, 0 },
		      { EV_KEY, BTN_TOOL_DOUBLETAP, 1 },
		      { EV_MSC, MSC_TIMESTAMP, 802 }, SYN);
	CHECK(test_warning_count() == 0, "no warnings so far");
	evdev_release(c);
	input_unregister_device(dev);

	/* In-core tracking: contacts without ids matched to slots. */
	{
		struct input_dev *t = mk_touchpad("track", 1, 2, NULL, NULL,
						  INPUT_MT_TRACK, 4);
		struct input_mt_pos p[4];
		int slots[4];

		CHECK(input_register_device(t) == 0, "register tracking pad");
		CHECK(input_mt_assign_slots(t, slots, p, 5, 10) == -EINVAL,
		      "more contacts than slots");
		p[0].x = 100, p[0].y = 100;
		p[1].x = 500, p[1].y = 500;
		CHECK(input_mt_assign_slots(t, slots, p, 2, 50) == 0 &&
			      slots[0] == 0 && slots[1] == 1,
		      "new contacts take free slots in order");
		for (int i = 0; i < 2; i++) {
			input_mt_slot(t, slots[i]);
			input_mt_report_slot_state(t, MT_TOOL_FINGER, 1);
			input_report_abs(t, ABS_MT_POSITION_X, p[i].x);
			input_report_abs(t, ABS_MT_POSITION_Y, p[i].y);
		}
		input_mt_sync_frame(t);
		input_sync(t);
		/* Reported in the other order and moved a little: each keeps
		 * its slot; a third, far away, gets a new one. */
		p[0].x = 505, p[0].y = 498;
		p[1].x = 103, p[1].y = 99;
		p[2].x = 2000, p[2].y = 1500;
		CHECK(input_mt_assign_slots(t, slots, p, 3, 50) == 0 &&
			      slots[0] == 1 && slots[1] == 0 && slots[2] == 2,
		      "tracking keeps slots: %d %d %d", slots[0], slots[1],
		      slots[2]);
		/* A jump beyond dmax is a new contact, not the old one. */
		p[0].x = 3000, p[0].y = 100;
		CHECK(input_mt_assign_slots(t, slots, p, 1, 50) == 0 &&
			      slots[0] == 2,
		      "beyond dmax: new slot (got %d)", slots[0]);
		/* With no tracking work space it refuses. */
		{
			struct input_dev *nt = mk_touchpad("notrack", 1, 3, NULL,
							   NULL, 0, 2);

			CHECK(input_mt_assign_slots(nt, slots, p, 1, 50) ==
				      -ENXIO,
			      "assign without TRACK");
			CHECK(input_mt_get_slot_by_key(nt, 77) == 0 &&
				      input_mt_get_slot_by_key(nt, 88) == 0,
			      "free slot by key (unused until reported)");
			input_free_device(nt);
		}
		input_unregister_device(t);
	}
}

// ---------------------------------------------------------------------------
// ioctl: sizes, errors, bad pointers
// ---------------------------------------------------------------------------

static void test_ioctl(void)
{
	struct input_dev *dev = mk_hp_touchpad();
	struct evdev_client *c, *kc;
	uint8_t buf[256];
	int v;

	h_printf("== ioctl\n");
	dev->uniq = "SN-1234";
	CHECK(input_register_device(dev) == 0, "register");
	c = open_unit(unit_of(dev));
	kc = open_unit(0);

	CHECK(ioc(c, EVIOCGVERSION, &v) == 0 && v == EV_VERSION, "version");
	CHECK(ioc(c, EVIOCGVERSION, BAD_PTR) == -EFAULT, "version bad ptr");
	CHECK(ioc(c, EVIOCGID, KERNEL_PTR) == -EFAULT, "id kernel ptr");

	/* Names: the whole string with its NUL, or cut to the buffer. */
	h_memset(buf, 0x7e, sizeof(buf));
	CHECK(ioc(c, EVIOCGNAME(sizeof(buf)), buf) == 31 &&
		      h_strcmp((char *)buf, "ELAN073B:00 04F3:3169 Touchpad") ==
			      0,
	      "name");
	h_memset(buf, 0x7e, sizeof(buf));
	CHECK(ioc(c, EVIOCGNAME(5), buf) == 5 &&
		      h_memcmp(buf, "ELAN0", 5) == 0 && buf[5] == 0x7e,
	      "name cut");
	CHECK(ioc(c, EVIOCGNAME(0), BAD_PTR) == 0, "zero-length name");
	CHECK(ioc(c, EVIOCGNAME(16), BAD_PTR) == -EFAULT, "name bad ptr");
	CHECK(ioc(c, EVIOCGPHYS(sizeof(buf)), buf) == 16 &&
		      h_strcmp((char *)buf, "i2c-ELAN073B:00") == 0,
	      "phys");
	CHECK(ioc(c, EVIOCGUNIQ(sizeof(buf)), buf) == 8 &&
		      h_strcmp((char *)buf, "SN-1234") == 0,
	      "uniq");
	CHECK(ioc(kc, EVIOCGUNIQ(sizeof(buf)), buf) == -ENOENT,
	      "no uniq -> ENOENT");

	/* EVIOCGBIT: whole words of the bitmap, cut to the buffer, the rest
	 * of a larger buffer untouched. */
	{
		unsigned long abs = 0, ev = 0, prop = 0, msc = 0;

		CHECK(ioc(c, EVIOCGBIT(0, sizeof(ev)), &ev) == 8 && ev == 0x1b,
		      "EV=%lx", ev);
		CHECK(ioc(c, EVIOCGBIT(EV_ABS, sizeof(abs)), &abs) == 8 &&
			      abs == 0x2e0800000000003UL,
		      "ABS=%lx", abs);
		CHECK(ioc(c, EVIOCGPROP(sizeof(prop)), &prop) == 8 &&
			      prop == 5,
		      "PROP=%lx", prop);
		CHECK(ioc(c, EVIOCGBIT(EV_MSC, sizeof(msc)), &msc) == 8 &&
			      msc == 0x20,
		      "MSC=%lx", msc);
		h_memset(buf, 0x7e, sizeof(buf));
		CHECK(ioc(c, EVIOCGBIT(EV_KEY, 200), buf) == 96 &&
			      buf[96] == 0x7e && buf[199] == 0x7e,
		      "KEY bitmap is 96 bytes, tail untouched");
		CHECK(buf[BTN_LEFT / 8] & (1 << (BTN_LEFT % 8)), "BTN_LEFT");
		h_memset(buf, 0x7e, sizeof(buf));
		CHECK(ioc(c, EVIOCGBIT(EV_KEY, 10), buf) == 10 &&
			      buf[10] == 0x7e,
		      "KEY bitmap cut");
		CHECK(ioc(c, EVIOCGBIT(0, 0), BAD_PTR) == 0, "zero length");
		CHECK(ioc(c, EVIOCGBIT(EV_KEY, 8), BAD_PTR) == -EFAULT,
		      "GBIT bad ptr");
		CHECK(ioc(c, EVIOCGBIT(EV_REP, 8), buf) == -EINVAL,
		      "GBIT of EV_REP");
		CHECK(ioc(c, EVIOCGBIT(EV_PWR, 8), buf) == -EINVAL,
		      "GBIT of EV_PWR");
		CHECK(ioc(c, EVIOCGBIT(EV_SW, 8), buf) == 8 && buf[0] == 0,
		      "GBIT of EV_SW (none)");
		CHECK(ioc(c, EVIOCGBIT(EV_FF, 32), buf) == 16, "FF is 16 bytes");
	}

	/* EVIOCGABS / EVIOCSABS. */
	{
		struct input_absinfo ai, set;

		CHECK(ioc(kc, EVIOCGABS(ABS_X), &ai) == -EINVAL,
		      "GABS without absolute axes");
		CHECK(ioc(c, EVIOCGABS(ABS_RZ), &ai) == 0 && ai.maximum == 0 &&
			      ai.minimum == 0,
		      "GABS of an axis the device lacks: zeros");
		CHECK(ioc(c, EVIOCGABS(ABS_X), BAD_PTR) == -EFAULT,
		      "GABS bad ptr");
		h_memset(&set, 0, sizeof(set));
		set.minimum = -5;
		set.maximum = 4000;
		set.resolution = 40;
		CHECK(ioc(c, EVIOCSABS(ABS_X), &set) == 0 &&
			      ioc(c, EVIOCGABS(ABS_X), &ai) == 0 &&
			      ai.minimum == -5 && ai.maximum == 4000 &&
			      ai.resolution == 40,
		      "SABS round trip");
		CHECK(ioc(c, EVIOCSABS(ABS_MT_SLOT), &set) == -EINVAL,
		      "SABS of ABS_MT_SLOT");
		CHECK(ioc(c, EVIOCSABS(ABS_Y), BAD_PTR) == -EFAULT,
		      "SABS bad ptr");
		CHECK(ioc(kc, EVIOCSABS(ABS_X), &set) == -EINVAL,
		      "SABS without absolute axes");
	}

	/* Fixed requests. */
	{
		unsigned int rep[2] = { 0, 0 };
		struct input_mask m;
		struct ff_effect eff;
		int effects = -1;

		CHECK(ioc(c, EVIOCGREP, rep) == -ENOSYS, "GREP without EV_REP");
		CHECK(ioc(c, EVIOCSREP, rep) == -ENOSYS, "SREP without EV_REP");
		CHECK(ioc(c, EVIOCGEFFECTS, &effects) == 0 && effects == 0,
		      "no effects");
		CHECK(ioc(c, EVIOCRMFF, (void *)3) == -ENOSYS, "RMFF");
		h_memset(&eff, 0, sizeof(eff));
		CHECK(sizeof(eff) == 48, "ff_effect is 48 bytes");
		CHECK(ioc(c, EVIOCSFF, &eff) == -ENOSYS, "SFF");
		/* An effect of another layout cannot be read at all. */
		CHECK(ioc(c, _INPUT_IOC(_INPUT_IOC_WRITE, 'E', 0x80, 40),
			  &eff) == -EFAULT,
		      "SFF of another size");
		CHECK(ioc(c, EVIOCSFF, BAD_PTR) == -EFAULT, "SFF bad ptr");
		CHECK(ioc(c, EVIOCGKEYCODE, rep) == -EINVAL, "no keymap");
		CHECK(ioc(c, EVIOCGKEYCODE, BAD_PTR) == -EFAULT,
		      "GKEYCODE bad ptr");
		h_memset(&m, 0, sizeof(m));
		CHECK(ioc(c, EVIOCGMASK, BAD_PTR) == -EFAULT, "GMASK bad ptr");
		CHECK(ioc(c, EVIOCREVOKE, (void *)1) == -EINVAL,
		      "REVOKE with an argument");
		v = 99;
		CHECK(ioc(c, EVIOCSCLOCKID, &v) == -EINVAL, "unknown clock");
		CHECK(ioc(c, EVIOCSCLOCKID, BAD_PTR) == -EFAULT,
		      "SCLOCKID bad ptr");
		CHECK(ioc(c, _INPUT_IOC(_INPUT_IOC_READ, 'E', 0x0f, 4), &v) ==
			      -EINVAL,
		      "unknown 'E' request");
		CHECK(ioc(c, 0x5401 /* a terminal request */, &v) == -EINVAL,
		      "foreign request");
	}

	/* A device with autorepeat. */
	{
		struct input_dev *k = input_allocate_device();
		struct evdev_client *rc;
		unsigned int rep[2];

		k->name = "repeat keyboard";
		input_set_capability(k, EV_KEY, KEY_A);
		input_set_capability(k, EV_REP, 0);
		CHECK(input_register_device(k) == 0, "register");
		rc = open_unit(unit_of(k));
		CHECK(ioc(rc, EVIOCGREP, rep) == 0 && rep[0] == 250 &&
			      rep[1] == 33,
		      "default repeat timing");
		rep[0] = 500, rep[1] = 20;
		CHECK(ioc(rc, EVIOCSREP, rep) == 0, "set repeat");
		/* The changes travel as EV_REP events in the next packet. */
		input_sync(k);
		CHECK(ioc(rc, EVIOCGREP, rep) == 0 && rep[0] == 500 &&
			      rep[1] == 20,
		      "repeat timing set");
		expect_stream("repeat events", rc, { EV_REP, REP_DELAY, 500 },
			      { EV_REP, REP_PERIOD, 20 }, SYN);
		evdev_release(rc);
		input_unregister_device(k);
	}

	/* A state read that cannot be delivered leaves the client knowing
	 * its view is incomplete: SYN_DROPPED in front of the next packet. */
	{
		static const struct contact one[] = { { 0, 9, 9, 0, 1 } };
		struct input_event e[16];
		int n;

		CHECK(ioc(c, EVIOCGKEY(96), BAD_PTR) == -EFAULT,
		      "GKEY bad ptr");
		CHECK(evdev_read(c, e, EV_SZ, 1) == -EAGAIN,
		      "SYN_DROPPED waits for a packet end");
		tp_frame(dev, one, 1, 1, 0);
		n = drain(c, e, 16);
		CHECK(n == 10 && e[0].code == SYN_DROPPED && e[0].type == EV_SYN &&
			      e[9].code == SYN_REPORT,
		      "SYN_DROPPED after a failed state read (%d)", n);
		CHECK(ioc(c, EVIOCGSND(8), e) == 8, "GSND");
	}

	/* Short and bad reads. */
	{
		struct input_event e;

		static const struct contact one[] = { { 0, 19, 19, 0, 1 } };
		struct input_event q[16];

		CHECK(evdev_read(c, &e, EV_SZ - 1, 1) == -EINVAL, "short read");
		CHECK(evdev_read(c, &e, 0, 0) == 0, "zero-length read");
		CHECK(evdev_read(c, BAD_PTR, EV_SZ, 1) == -EAGAIN,
		      "nonblocking read to a bad pointer, nothing queued");
		tp_frame(dev, one, 1, 2, 0);
		CHECK(evdev_read(c, BAD_PTR, EV_SZ, 1) == -EFAULT,
		      "read to a bad pointer");
		CHECK(evdev_read(c, KERNEL_PTR, 4 * EV_SZ, 0) == -EFAULT,
		      "read to a kernel pointer");
		CHECK(drain(c, q, 16) == 6 && q[0].code == ABS_MT_POSITION_X &&
			      q[5].code == SYN_REPORT,
		      "a failed read loses no events");
		CHECK(evdev_read(c, &e, EV_SZ, 1) == -EAGAIN, "nothing queued");
		CHECK(evdev_poll(c, 0x0001 | 0x0004) == 0x0004,
		      "poll: writable, not readable");
	}
	evdev_release(kc);
	evdev_release(c);
	input_unregister_device(dev);
}

// ---------------------------------------------------------------------------
// clients: queues, grab, revoke, masks, clocks, overflow
// ---------------------------------------------------------------------------

static void test_clients(void)
{
	struct input_dev *dev = mk_hp_touchpad();
	struct evdev_client *a, *b;
	static const struct contact one[] = { { 0, 100, 100, 0, 1 } };
	static const struct contact moved[] = { { 0, 200, 100, 0, 1 } };

	h_printf("== clients\n");
	CHECK(input_register_device(dev) == 0, "register");
	a = open_unit(unit_of(dev));
	b = open_unit(unit_of(dev));

	/* Two readers get every event each. */
	tp_frame(dev, one, 1, 1, 0);
	{
		struct input_event ea[32], eb[32];
		int na = drain(a, ea, 32), nb = drain(b, eb, 32);

		CHECK(na == 9 && nb == 9 && h_memcmp(ea, eb, sizeof(ea[0]) * 9) ==
						     0,
		      "both readers: %d %d", na, nb);
	}
	/* One reader's backlog is its own. */
	tp_frame(dev, moved, 1, 2, 0);
	expect_stream("reader a", a, { EV_ABS, ABS_MT_POSITION_X, 200 },
		      { EV_ABS, ABS_X, 200 }, { EV_MSC, MSC_TIMESTAMP, 2 }, SYN);
	CHECK(evdev_poll(b, 0x0001) == 0x0001, "b still readable");
	expect_stream("reader b", b, { EV_ABS, ABS_MT_POSITION_X, 200 },
		      { EV_ABS, ABS_X, 200 }, { EV_MSC, MSC_TIMESTAMP, 2 }, SYN);

	/* Grab: only the grabber hears the device. */
	CHECK(ioc(a, EVIOCGRAB, (void *)1) == 0, "grab");
	CHECK(ioc(a, EVIOCGRAB, (void *)1) == -EBUSY, "grab twice");
	tp_frame(dev, one, 1, 3, 0);
	expect_stream("grabber", a, { EV_ABS, ABS_MT_POSITION_X, 100 },
		      { EV_ABS, ABS_X, 100 }, { EV_MSC, MSC_TIMESTAMP, 3 }, SYN);
	expect_empty("grabbed away", b);
	CHECK(ioc(a, EVIOCGRAB, (void *)0) == 0, "ungrab");
	tp_frame(dev, moved, 1, 4, 0);
	CHECK(drain(a, NULL, 0) == 0, "zero-size drain");
	{
		struct input_event e[8];

		CHECK(drain(a, e, 8) == 4 && drain(b, e, 8) == 4,
		      "both again after ungrab");
	}

	/* Masks: types and codes. */
	{
		unsigned long types = ~0UL, codes = 0;
		struct input_mask m;

		types &= ~(1UL << EV_MSC);
		m.type = 0;
		m.codes_size = sizeof(types);
		m.codes_ptr = (uint64_t)(unsigned long)&types;
		CHECK(ioc(a, EVIOCSMASK, &m) == 0, "mask out EV_MSC");
		codes = 1UL << ABS_X;
		m.type = EV_ABS;
		m.codes_ptr = (uint64_t)(unsigned long)&codes;
		CHECK(ioc(a, EVIOCSMASK, &m) == 0, "only ABS_X");
		tp_frame(dev, one, 1, 5, 0);
		expect_stream("masked", a, { EV_ABS, ABS_X, 100 }, SYN);
		/* Masking everything of a packet drops its SYN_REPORT. */
		tp_frame(dev, one, 1, 6, 0);
		expect_empty("masked to nothing", a);
		expect_stream("unmasked reader", b,
			      { EV_ABS, ABS_MT_POSITION_X, 100 },
			      { EV_ABS, ABS_X, 100 },
			      { EV_MSC, MSC_TIMESTAMP, 5 }, SYN,
			      { EV_MSC, MSC_TIMESTAMP, 6 }, SYN);
		{
			unsigned long got[3] = { 7, 7, 7 };

			m.type = EV_ABS;
			m.codes_size = sizeof(got);
			m.codes_ptr = (uint64_t)(unsigned long)got;
			CHECK(ioc(a, EVIOCGMASK, &m) == 0 &&
				      got[0] == (1UL << ABS_X) && got[1] == 0 &&
				      got[2] == 0,
			      "GMASK: mask, zero-filled tail");
			m.type = EV_KEY;
			m.codes_size = 8;
			CHECK(ioc(a, EVIOCGMASK, &m) == 0 && got[0] == ~0UL,
			      "GMASK of an unmasked type: all set");
			m.type = EV_REP;
			got[0] = 7;
			CHECK(ioc(a, EVIOCGMASK, &m) == 0 && got[0] == 0,
			      "GMASK of an unknown type: zeros");
			/* Unknown types are accepted and ignored (forward
			 * compatibility). */
			CHECK(ioc(a, EVIOCSMASK, &m) == 0,
			      "SMASK of an unknown type");
			m.type = EV_REL;
			got[0] = 0;
			CHECK(ioc(a, EVIOCGMASK, &m) == 0 && got[0] == ~0UL,
			      "GMASK of an unmasked short type: all bytes set");
			m.type = 0;
			got[0] = 0;
			CHECK(ioc(b, EVIOCGMASK, &m) == 0 && got[0] == ~0UL,
			      "GMASK of the unmasked types: all bytes set");
			m.type = EV_KEY;
			m.codes_size = 4;
			CHECK(ioc(a, EVIOCSMASK, &m) == -EINVAL,
			      "SMASK of a part of a word");
			m.codes_size = 8;
			m.type = EV_KEY;
			m.codes_ptr = 8;
			CHECK(ioc(a, EVIOCSMASK, &m) == -EFAULT,
			      "SMASK bad codes pointer");
		}
		types = ~0UL;
		m.type = 0;
		m.codes_size = sizeof(types);
		m.codes_ptr = (uint64_t)(unsigned long)&types;
		codes = ~0UL;
		CHECK(ioc(a, EVIOCSMASK, &m) == 0, "unmask types");
		m.type = EV_ABS;
		m.codes_ptr = (uint64_t)(unsigned long)&codes;
		CHECK(ioc(a, EVIOCSMASK, &m) == 0, "unmask abs");
	}

	/* Clocks: per client; switching drops what was queued. */
	{
		struct input_event e[16];
		int clk = CLOCK_MONOTONIC, n;

		test_set_clock_ns(5123456789ULL);
		tp_frame(dev, moved, 1, 7, 0);
		CHECK(ioc(a, EVIOCSCLOCKID, &clk) == 0, "monotonic clock");
		CHECK(drain(a, e, 16) == 0, "switch drops the queue");
		n = drain(b, e, 16);
		CHECK(n == 4 && e[0].time.tv_sec == 1700000005 &&
			      e[0].time.tv_usec == 123456,
		      "realtime stamp %lld.%06lld", (long long)e[0].time.tv_sec,
		      (long long)e[0].time.tv_usec);
		test_set_clock_ns(6000001000ULL);
		tp_frame(dev, one, 1, 8, 0);
		n = drain(a, e, 16);
		CHECK(n == 5 && e[0].type == EV_SYN &&
			      e[0].code == SYN_DROPPED &&
			      e[1].time.tv_sec == 6 && e[1].time.tv_usec == 1,
		      "SYN_DROPPED then monotonic stamps (%d)", n);
		clk = CLOCK_BOOTTIME;
		CHECK(ioc(a, EVIOCSCLOCKID, &clk) == 0, "boottime clock");
		clk = CLOCK_REALTIME;
		CHECK(ioc(a, EVIOCSCLOCKID, &clk) == 0, "realtime clock");
		drain(b, e, 16);
		test_set_clock_ns(0);
	}

	/* State reads remove that type from the queue. */
	{
		unsigned long key[12];

		tp_frame(dev, one, 1, 9, 1); // click
		tp_frame(dev, one, 1, 10, 0); // release
		CHECK(ioc(a, EVIOCGKEY(sizeof(key)), key) == 96,
		      "GKEY after the click");
		expect_stream("queue without key events", a,
			      { EV_MSC, MSC_TIMESTAMP, 9 }, SYN,
			      { EV_MSC, MSC_TIMESTAMP, 10 }, SYN);
		tp_frame(dev, one, 1, 11, 1);
		/* Only key events and timestamps: the masked packets lose
		 * everything but the timestamp. */
		{
			unsigned long types = ~0UL & ~(1UL << EV_MSC);
			struct input_mask m;

			m.type = 0;
			m.codes_size = sizeof(types);
			m.codes_ptr = (uint64_t)(unsigned long)&types;
			CHECK(ioc(a, EVIOCSMASK, &m) == 0, "mask msc");
			tp_frame(dev, one, 1, 12, 0);
			CHECK(ioc(a, EVIOCGKEY(sizeof(key)), key) == 96 &&
				      key[BTN_LEFT / 64] == 0,
			      "GKEY: released");
			expect_stream("packets of only keys vanish", a,
				      { EV_MSC, MSC_TIMESTAMP, 11 }, SYN);
			types = ~0UL;
			CHECK(ioc(a, EVIOCSMASK, &m) == 0, "unmask");
		}
		drain(b, NULL, 0);
		{
			struct input_event e[64];

			while (drain(b, e, 64) > 0)
				;
		}
	}

	/* Overflow: a reader that does not keep up loses everything unread,
	 * gets SYN_DROPPED, and resynchronises from the state ioctls. */
	{
		struct input_event e[1100];
		int n, i, frames = 0, last_x = 0;
		INPUT_MT_REQUEST_LAYOUT(req5, 5) r;
		struct input_absinfo ai;

		while (frames < 400) {
			struct contact cx[2] = { { 0, 0, 100, 0, 1 },
						 { 1, 0, 900, 0, 1 } };

			cx[0].x = 100 + frames;
			cx[1].x = 2000 - frames;
			tp_frame(dev, cx, 2, 1000 + frames, frames & 1);
			last_x = cx[0].x;
			frames++;
		}
		n = drain(a, e, 1100);
		CHECK(n > 0 && e[0].type == EV_SYN && e[0].code == SYN_DROPPED,
		      "overflow: SYN_DROPPED first (%d events)", n);
		CHECK(n > 1 && e[n - 1].type == EV_SYN &&
			      e[n - 1].code == SYN_REPORT,
		      "overflow: ends with a whole packet");
		for (i = 1; i < n; i++)
			CHECK(!(e[i].type == EV_SYN && e[i].code == SYN_DROPPED),
			      "one SYN_DROPPED only");
		r.code = ABS_MT_POSITION_X;
		CHECK(ioc(a, EVIOCGMTSLOTS(sizeof(r)), &r) == 0 &&
			      r.values[0] == last_x &&
			      r.values[1] == 2000 - 399,
		      "resync positions");
		CHECK(ioc(a, EVIOCGABS(ABS_X), &ai) == 0 && ai.value == last_x,
		      "resync ABS_X");
		n = drain(b, e, 1100);
		CHECK(n > 0 && e[0].code == SYN_DROPPED, "b overflowed too");
	}

	/* Revoke: the descriptor is dead, the other one is not. */
	CHECK(ioc(a, EVIOCGRAB, (void *)1) == 0, "grab before revoke");
	CHECK(ioc(a, EVIOCREVOKE, NULL) == 0, "revoke");
	{
		struct input_event e[8];
		int v;

		CHECK(evdev_read(a, e, EV_SZ, 1) == -ENODEV, "revoked read");
		CHECK(evdev_read(a, e, EV_SZ, 0) == -ENODEV,
		      "revoked blocking read");
		CHECK(ioc(a, EVIOCGVERSION, &v) == -ENODEV, "revoked ioctl");
		CHECK(evdev_write(a, e, EV_SZ) == -ENODEV, "revoked write");
		CHECK(evdev_poll(a, 0x0001) == (0x0008 | 0x0010),
		      "revoked poll: ERR|HUP");
		tp_frame(dev, one, 1, 20, 0);
		CHECK(drain(b, e, 8) > 0, "revoke released the grab");
	}
	evdev_release(a);
	evdev_release(b);
	input_unregister_device(dev);
}

// ---------------------------------------------------------------------------
// proc: /proc/bus/input/devices
// ---------------------------------------------------------------------------

/* Parse "B: NAME=w_hi ... w_0" into a bitmap. */
static void parse_bitmap(const char *s, unsigned long *map, int nlongs)
{
	unsigned long words[16];
	int n = 0;

	while (*s && *s != '\n' && n < 16) {
		char *end;

		words[n++] = (unsigned long)h_strtoull(s, &end, 16);
		s = end;
		while (*s == ' ')
			s++;
	}
	for (int i = 0; i < n && i < nlongs; i++)
		map[i] = words[n - 1 - i];
}

/* Copy one line (without \r\n) into out. */
static const char *get_line(const char *p, char *out, int cap)
{
	int n = 0;

	while (*p && *p != '\n') {
		if (*p != '\r' && n < cap - 1)
			out[n++] = *p;
		p++;
	}
	out[n] = 0;
	return *p ? p + 1 : p;
}

/* One block of a dump: its lines. */
struct block {
	char lines[16][384];
	int n;
};

static int block_line(const struct block *b, const char *prefix)
{
	for (int i = 0; i < b->n; i++)
		if (h_strncmp(b->lines[i], prefix, h_strlen(prefix)) == 0)
			return i;
	return -1;
}

/* Build a device with exactly the identity and bitmaps of the block. */
static struct input_dev *dev_from_block(const struct block *b, char *name,
					char *phys)
{
	struct input_dev *dev = input_allocate_device();
	int i = block_line(b, "I: ");
	const char *p;

	if (i >= 0) {
		p = h_strstr(b->lines[i], "Bus=");
		dev->id.bustype = (uint16_t)h_strtoull(p + 4, NULL, 16);
		p = h_strstr(b->lines[i], "Vendor=");
		dev->id.vendor = (uint16_t)h_strtoull(p + 7, NULL, 16);
		p = h_strstr(b->lines[i], "Product=");
		dev->id.product = (uint16_t)h_strtoull(p + 8, NULL, 16);
		p = h_strstr(b->lines[i], "Version=");
		dev->id.version = (uint16_t)h_strtoull(p + 8, NULL, 16);
	}
	i = block_line(b, "N: Name=\"");
	if (i >= 0) {
		h_snprintf(name, 256, "%s", b->lines[i] + 9);
		name[h_strlen(name) - 1] = 0; // closing quote
		dev->name = name;
	}
	i = block_line(b, "P: Phys=");
	if (i >= 0 && b->lines[i][8]) {
		h_snprintf(phys, 256, "%s", b->lines[i] + 8);
		dev->phys = phys;
	}
#define BITS(tag, map)                                                       \
	do {                                                                 \
		int l = block_line(b, "B: " tag "=");                        \
		if (l >= 0)                                                  \
			parse_bitmap(b->lines[l] + 4 + h_strlen(tag), dev->map, \
				     (int)(sizeof(dev->map) /                \
					   sizeof(dev->map[0])));            \
	} while (0)
	BITS("PROP", propbit);
	BITS("EV", evbit);
	BITS("KEY", keybit);
	BITS("REL", relbit);
	BITS("ABS", absbit);
	BITS("MSC", mscbit);
	BITS("LED", ledbit);
	BITS("SND", sndbit);
	BITS("FF", ffbit);
	BITS("SW", swbit);
#undef BITS
	return dev;
}

/* Our block for `dev' must equal the dump's, line by line, except for the
 * numbers that depend on registration order (inputN in S:, the handler
 * list in H:, which here is only eventN). */
static void compare_block(const struct block *want, struct input_dev *dev,
			  const char *file)
{
	char out[4096];
	long len = input_format_device(dev, unit_of(dev), out, sizeof(out), 0);
	struct block got;
	const char *p = out;

	CHECK(len > 0 && len < (long)sizeof(out), "format length");
	out[len < (long)sizeof(out) ? len : (long)sizeof(out) - 1] = 0;
	got.n = 0;
	while (*p && got.n < 16) {
		p = get_line(p, got.lines[got.n], sizeof(got.lines[0]));
		if (!got.lines[got.n][0])
			break;
		got.n++;
	}
	CHECK(got.n == want->n, "%s: %s: %d lines, want %d", file,
	      dev->name ? dev->name : "?", got.n, want->n);
	for (int i = 0; i < got.n && i < want->n; i++) {
		const char *g = got.lines[i], *w = want->lines[i];

		if (h_strncmp(w, "S: ", 3) == 0) {
			/* The device path is the driver's (here: none, so
			 * the virtual tree), the inputN number ours. */
			CHECK(h_strncmp(g, "S: Sysfs=", 9) == 0 &&
				      h_strstr(g, "/input/input") != NULL,
			      "%s: sysfs line '%s'", file, g);
			continue;
		}
		if (h_strncmp(w, "H: ", 3) == 0) {
			char ev[24];

			h_snprintf(ev, sizeof(ev), "event%d ", unit_of(dev));
			CHECK(h_strncmp(g, "H: Handlers=", 12) == 0 &&
				      h_strstr(g, ev) &&
				      h_strstr(w, "event") != NULL,
			      "%s: handlers line '%s'", file, g);
			continue;
		}
		CHECK(h_strcmp(g, w) == 0, "%s: '%s' want '%s'", file, g, w);
	}
}

static void proc_roundtrip(const char *file)
{
	unsigned long len;
	char *text = (char *)h_read_file(file, &len);
	const char *p;
	static struct block blocks[40];
	struct input_dev *devs[40];
	static char names[40][256], physs[40][256];
	int nb = 0, nd = 0;

	CHECK(text != NULL, "read %s", file);
	if (!text)
		return;
	p = text;
	{
		struct block *b = &blocks[0];

		b->n = 0;
		while (*p && nb < 40) {
			char line[384];

			p = get_line(p, line, sizeof(line));
			if (!line[0]) {
				if (b->n) {
					nb++;
					b = &blocks[nb];
					b->n = 0;
				}
				continue;
			}
			if (b->n == 0 && line[0] != 'I' && line[0] != 'N')
				continue; // a heading
			if (b->n < 16)
				h_snprintf(b->lines[b->n++], 384, "%s", line);
		}
		if (b->n && nb < 40)
			nb++;
	}
	for (int i = 0; i < nb; i++) {
		/* A block cut off in the dump (no I: line) is not a device. */
		if (block_line(&blocks[i], "I: ") != 0)
			continue;
		devs[nd] = dev_from_block(&blocks[i], names[nd], physs[nd]);
		CHECK(input_register_device(devs[nd]) == 0, "register %s",
		      names[nd]);
		compare_block(&blocks[i], devs[nd], file);
		nd++;
	}
	CHECK(nd >= 14, "%s: %d devices", file, nd);
	for (int i = 0; i < nd; i++)
		input_unregister_device(devs[i]);
	h_free(text);
}

static void test_proc(void)
{
	h_printf("== proc\n");
	proc_roundtrip("kernel/spec/devices-ubuntu-live-hp-probook-650-p8");
	proc_roundtrip("kernel/spec/devices-ubuntu-live-dellpro16plus");

	/* The touchpad built the way the driver will build it gives the
	 * dump's block, and the whole file lists the built-in devices
	 * first. */
	{
		struct input_dev *tp = mk_hp_touchpad();
		char *out = (char *)h_calloc(1, 8192);
		long len;

		CHECK(input_register_device(tp) == 0, "register");
		len = input_proc_devices_show(NULL, out, 8192);
		CHECK(len > 0 && len < 8192, "proc length %ld", len);
		static const char kbd[] =
			"I: Bus=0011 Vendor=0001 Product=0001 Version=0100\n"
			"N: Name=\"LikeOS Keyboard\"\n"
			"P: Phys=isa0060/serio0/input0\n"
			"S: Sysfs=/devices/platform/i8042/serio0/input/input0\n"
			"U: Uniq=\nH: Handlers=event0 \n"
			"B: PROP=0\nB: EV=20003\n"
			"B: KEY=fea0ffff73ffffff fffffffffffffffe\n"
			"B: LED=7\n\n"
			"I: Bus=0011 Vendor=0001 Product=0002 Version=0100\n"
			"N: Name=\"LikeOS Mouse\"\n";

		CHECK(h_strncmp(out, kbd, h_strlen(kbd)) == 0,
		      "keyboard block:\n%.600s", out);
		CHECK(h_strstr(out, "N: Name=\"ELAN073B:00 04F3:3169 Touchpad\"\n"
				    "P: Phys=i2c-ELAN073B:00\n"
				    "S: Sysfs=/devices/pci0000:00/0000:00:15.0/"
				    "i2c_designware.0/i2c-0/i2c-ELAN073B:00/"
				    "0018:04F3:3169.0001/input/input2\n"
				    "U: Uniq=\nH: Handlers=event2 \n"
				    "B: PROP=5\nB: EV=1b\n"
				    "B: KEY=e520 10000 0 0 0 0\n"
				    "B: ABS=2e0800000000003\nB: MSC=20\n\n") !=
			      NULL,
		      "touchpad block:\n%s", out);
		/* A buffer that is too small: the length needed comes back
		 * and nothing is written past the end. */
		h_memset(out, 0x7e, 8192);
		CHECK(input_proc_devices_show(NULL, out, 10) == len &&
			      out[10] == 0x7e,
		      "short buffer");
		h_free(out);
		input_unregister_device(tp);
	}
}

// ---------------------------------------------------------------------------
// lifecycle: numbering, callbacks, removal, device events
// ---------------------------------------------------------------------------

static int g_open_calls, g_close_calls, g_fail_open;

static int cb_open(struct input_dev *dev)
{
	(void)dev;
	g_open_calls++;
	return g_fail_open ? -EIO : 0;
}

static void cb_close(struct input_dev *dev)
{
	(void)dev;
	g_close_calls++;
}

static void test_lifecycle(void)
{
	struct input_dev *devs[EVDEV_MINORS];
	struct test_devevent de;
	struct evdev_client *c1, *c2, *c3;
	struct input_dev *dev;
	unsigned before;
	int n = 0;

	h_printf("== lifecycle\n");
	/* Numbering: lowest free from 2, reused after removal. */
	for (n = 0; n < EVDEV_MINORS - EVDEV_BUILTIN_UNITS; n++) {
		devs[n] = mk_hp_touchpad();
		CHECK(input_register_device(devs[n]) == 0, "register %d", n);
		CHECK(unit_of(devs[n]) == n + 2, "unit %d", unit_of(devs[n]));
	}
	dev = mk_hp_touchpad();
	CHECK(input_register_device(dev) == -ENFILE, "no free node");
	input_free_device(dev);
	CHECK(evdev_next_unit(0) == 0 && evdev_next_unit(31) == 31 &&
		      evdev_next_unit(32) == -1,
	      "listing of a full table");
	input_unregister_device(devs[3]); // event5
	input_unregister_device(devs[1]); // event3
	CHECK(!evdev_unit_exists(3) && !evdev_unit_exists(5) &&
		      evdev_next_unit(3) == 4,
	      "removed nodes are gone");
	devs[1] = mk_hp_touchpad();
	devs[3] = mk_hp_touchpad();
	CHECK(input_register_device(devs[1]) == 0 && unit_of(devs[1]) == 3,
	      "lowest free reused");
	CHECK(input_register_device(devs[3]) == 0 && unit_of(devs[3]) == 5,
	      "next free");
	for (int i = 0; i < n; i++)
		input_unregister_device(devs[i]);
	CHECK(evdev_next_unit(2) == -1, "all dynamic nodes gone");
	{
		struct evdev_client *x = NULL;

		CHECK(evdev_open(2, &x) == -ENODEV && x == NULL,
		      "open of a missing node");
		CHECK(evdev_open(40, &x) == -ENODEV, "open out of range");
		CHECK(evdev_open(-1, &x) == -ENODEV, "open negative");
	}

	/* open()/close() of the driver: first user, last user. */
	before = test_devevent_count();
	dev = mk_hp_touchpad();
	dev->open = cb_open;
	dev->close = cb_close;
	g_open_calls = g_close_calls = 0;
	CHECK(input_register_device(dev) == 0, "register");
	CHECK(test_devevent_get(before, &de) && de.type == DEVICE_EVENT_ADD &&
		      de.device_class == DEVICE_CLASS_INPUT &&
		      de.major == 13 && de.minor == 66 &&
		      h_strcmp(de.name, "event2") == 0 &&
		      de.vendor == 0x04f3 && de.product == 0x3169 &&
		      (de.flags & DEVICE_EVENT_F_INPUT_TOUCHPAD) &&
		      !(de.flags & DEVICE_EVENT_F_INPUT_MOUSE) &&
		      de.device_id == (((uint64_t)BUS_I2C << 48) |
				       (0x0100ULL << 32) | 2),
	      "add event: class %u name %s flags %x", de.device_class,
	      de.name, de.flags);
	CHECK(!input_device_enabled(dev), "not enabled before an open");
	c1 = open_unit(2);
	c2 = open_unit(2);
	CHECK(g_open_calls == 1 && input_device_enabled(dev),
	      "one open callback");
	evdev_release(c1);
	CHECK(g_close_calls == 0, "still a user");
	evdev_release(c2);
	CHECK(g_close_calls == 1 && !input_device_enabled(dev),
	      "close callback on the last close");
	g_fail_open = 1;
	{
		struct evdev_client *x = NULL;

		CHECK(evdev_open(2, &x) == -EIO && !x,
		      "driver open error fails the open");
	}
	g_fail_open = 0;

	/* Removal with open descriptors: they stay valid but dead; the
	 * device is closed once; the node is free for the next device. */
	c1 = open_unit(2);
	c2 = open_unit(2);
	c3 = open_unit(0);
	input_report_key(dev, BTN_LEFT, 1);
	input_sync(dev);
	before = test_devevent_count();
	input_unregister_device(dev);
	CHECK(g_close_calls == 2, "closed at removal");
	CHECK(test_devevent_get(before, &de) &&
		      de.type == DEVICE_EVENT_REMOVE &&
		      h_strcmp(de.name, "event2") == 0,
	      "remove event");
	{
		struct input_event e[4];
		int v;

		CHECK(evdev_read(c1, e, EV_SZ, 1) == -ENODEV, "read: ENODEV");
		CHECK(evdev_read(c1, e, EV_SZ, 0) == -ENODEV,
		      "blocking read: ENODEV");
		CHECK(ioc(c1, EVIOCGVERSION, &v) == -ENODEV, "ioctl: ENODEV");
		CHECK(evdev_write(c1, e, EV_SZ) == -ENODEV, "write: ENODEV");
		CHECK((evdev_poll(c1, 0x0001) & 0x0018) == 0x0018,
		      "poll: ERR|HUP");
		CHECK(ioc(c3, EVIOCGVERSION, &v) == 0, "event0 unaffected");
	}
	CHECK(!evdev_unit_exists(2), "node gone");
	dev = mk_hp_touchpad();
	CHECK(input_register_device(dev) == 0 && unit_of(dev) == 2,
	      "number reused while old descriptors are open");
	{
		struct evdev_client *c4 = open_unit(2);
		struct input_event e[16];
		static const struct contact one[] = { { 0, 1, 1, 0, 1 } };

		tp_frame(dev, one, 1, 1, 0);
		CHECK(drain(c4, e, 16) > 0, "new device reads");
		CHECK(evdev_read(c2, e, EV_SZ, 1) == -ENODEV,
		      "old descriptor still dead");
		evdev_release(c4);
	}
	evdev_release(c1);
	evdev_release(c2); // last reference to the old device
	evdev_release(c3);
	CHECK(g_close_calls == 2, "no second close for the dead node");
	input_unregister_device(dev);

	/* Classification of the built-in devices. */
	CHECK(test_devevent_count() > 0, "events recorded");
}

// ---------------------------------------------------------------------------
// race: threads
// ---------------------------------------------------------------------------

struct reader_arg {
	struct evdev_client *c;
	long result;
	int events;
	volatile int stop;
};

static void *blocking_reader(void *p)
{
	struct reader_arg *ra = (struct reader_arg *)p;
	struct input_event e[64];

	for (;;) {
		long r = evdev_read(ra->c, e, sizeof(e), 0);

		if (r <= 0) {
			ra->result = r;
			break;
		}
		ra->events += (int)(r / EV_SZ);
		if (ra->stop) {
			ra->result = 0;
			break;
		}
	}
	test_thread_done();
	return NULL;
}

static void wait_sleepers(int n)
{
	for (int i = 0; i < 20000 && test_sleepers() < n; i++)
		h_usleep(100);
}

struct producer_arg {
	struct input_dev *dev;
	volatile int stop;
	int frames;
	unsigned seed;
};

/* A device's interrupt handler: frames with interrupts "off", so any
 * sleeping call inside the core aborts the test. */
static void *producer(void *p)
{
	struct producer_arg *pa = (struct producer_arg *)p;

	h_srandom(pa->seed);
	while (!__atomic_load_n(&pa->stop, __ATOMIC_ACQUIRE)) {
		struct contact c[5];
		int n = (int)(h_random() % 6);
		uint64_t f = test_irq_flag;

		for (int i = 0; i < n; i++) {
			c[i].slot = i;
			c[i].x = (int)(h_random() % 3200);
			c[i].y = (int)(h_random() % 2000);
			c[i].tool = (h_random() % 8) ? 0 : MT_TOOL_PALM;
			c[i].active = 1;
		}
		test_irq_flag = 0;
		tp_frame(pa->dev, c, n, pa->frames, (int)(h_random() & 1));
		test_irq_flag = f;
		pa->frames++;
		if ((pa->frames & 63) == 0)
			h_yield();
	}
	test_thread_done();
	return NULL;
}

static void race_blocking_readers(void)
{
	struct input_dev *dev = mk_hp_touchpad();
	struct reader_arg ra[3];
	unsigned long th[3];
	static const struct contact one[] = { { 0, 7, 7, 0, 1 } };

	CHECK(input_register_device(dev) == 0, "register");
	/* A sleeping reader wakes for a packet. */
	ra[0].c = open_unit(unit_of(dev));
	ra[0].events = 0;
	ra[0].stop = 1;
	h_thread_start(blocking_reader, &ra[0], &th[0]);
	wait_sleepers(1);
	tp_frame(dev, one, 1, 1, 0);
	h_thread_join(th[0]);
	CHECK(ra[0].result == 0 && ra[0].events == 9,
	      "woken reader got the packet (%ld, %d)", ra[0].result,
	      ra[0].events);

	/* A signal ends a sleep with EINTR. */
	{
		struct input_event e;

		test_set_signal_pending(1);
		CHECK(evdev_read(ra[0].c, &e, EV_SZ, 0) == -EINTR,
		      "signal: EINTR");
		CHECK(evdev_ioctl(ra[0].c, EVIOCGRAB, (void *)1) == -EINTR,
		      "signal: interrupted mutex");
		test_set_signal_pending(0);
	}

	/* Sleeping readers wake with ENODEV when the device goes. */
	ra[1].c = open_unit(unit_of(dev));
	ra[2].c = open_unit(unit_of(dev));
	for (int i = 0; i < 3; i++) {
		ra[i].events = 0;
		ra[i].stop = 0;
		h_thread_start(blocking_reader, &ra[i], &th[i]);
	}
	wait_sleepers(3);
	input_unregister_device(dev);
	for (int i = 0; i < 3; i++) {
		h_thread_join(th[i]);
		CHECK(ra[i].result == -ENODEV, "reader %d woke with %ld", i,
		      ra[i].result);
		evdev_release(ra[i].c);
	}
}

struct churn_arg {
	volatile int stop;
	unsigned seed;
	long opens, reads;
};

/* Opens whatever node number comes up, reads, queries, closes. */
static void *churn_opener(void *p)
{
	struct churn_arg *ca = (struct churn_arg *)p;
	struct input_event e[32];
	char name[64];

	h_srandom(ca->seed);
	while (!__atomic_load_n(&ca->stop, __ATOMIC_ACQUIRE)) {
		struct evdev_client *c = NULL;
		int unit = 2 + (int)(h_random() % 6);
		INPUT_MT_REQUEST_LAYOUT(req5, 5) r;
		unsigned long key[12];

		if (evdev_open(unit, &c) != 0)
			continue;
		ca->opens++;
		for (int i = 0; i < 4; i++) {
			long n = evdev_read(c, e, sizeof(e), 1);

			if (n > 0)
				ca->reads += n / EV_SZ;
			r.code = ABS_MT_POSITION_X;
			evdev_ioctl(c, EVIOCGMTSLOTS(sizeof(r)), &r);
			evdev_ioctl(c, EVIOCGNAME(sizeof(name)), name);
			evdev_ioctl(c, EVIOCGKEY(sizeof(key)), key);
			evdev_poll(c, 0x0001);
			if (h_random() % 4 == 0)
				evdev_ioctl(c, EVIOCGRAB, (void *)1);
		}
		if (h_random() % 3 == 0)
			evdev_ioctl(c, EVIOCREVOKE, NULL);
		evdev_release(c);
	}
	test_thread_done();
	return NULL;
}

/* Registers a device, feeds it from a producer thread, reads it with a
 * blocking reader, and takes it away again -- over and over. */
static void *churn_device(void *p)
{
	struct churn_arg *ca = (struct churn_arg *)p;
	int rounds = 0;

	h_srandom(ca->seed);
	while (!__atomic_load_n(&ca->stop, __ATOMIC_ACQUIRE)) {
		struct input_dev *dev = mk_hp_touchpad();
		struct producer_arg pa;
		struct reader_arg ra;
		unsigned long tp, tr;

		if (input_register_device(dev) != 0) {
			input_free_device(dev);
			h_usleep(100);
			continue;
		}
		/* The test's own reference: the producer may still be in
		 * the core while the device is being unregistered. */
		input_get_device(dev);
		ra.c = NULL;
		if (evdev_open(unit_of(dev), &ra.c) == 0) {
			ra.events = 0;
			ra.stop = 0;
			h_thread_start(blocking_reader, &ra, &tr);
		}
		pa.dev = dev;
		pa.stop = 0;
		pa.frames = 0;
		pa.seed = (unsigned)h_random();
		h_thread_start(producer, &pa, &tp);
		h_usleep((unsigned)(h_random() % 3000));
		input_unregister_device(dev);
		__atomic_store_n(&pa.stop, 1, __ATOMIC_RELEASE);
		h_thread_join(tp);
		if (ra.c) {
			h_thread_join(tr);
			CHECK(ra.result == -ENODEV,
			      "churn reader ended with %ld", ra.result);
			evdev_release(ra.c);
		}
		input_put_device(dev);
		rounds++;
	}
	ca->opens += rounds;
	test_thread_done();
	return NULL;
}

static void test_race(void)
{
	struct churn_arg dev_args[4], open_args[4];
	unsigned long dth[4], oth[4];
	int secs = 4;
	char *e = h_getenv("TESTINPUT_RACE_SECS");

	h_printf("== race\n");
	if (e)
		secs = (int)h_strtoull(e, NULL, 10);
	race_blocking_readers();

	for (int i = 0; i < 4; i++) {
		dev_args[i].stop = 0;
		dev_args[i].seed = 100 + (unsigned)i;
		dev_args[i].opens = 0;
		open_args[i].stop = 0;
		open_args[i].seed = 200 + (unsigned)i;
		open_args[i].opens = open_args[i].reads = 0;
		h_thread_start(churn_device, &dev_args[i], &dth[i]);
		h_thread_start(churn_opener, &open_args[i], &oth[i]);
	}
	/* Meanwhile, this thread hammers the built-in mouse as its
	 * interrupt would, with a reader of its own. */
	{
		struct evdev_client *mc = open_unit(1);
		unsigned long long end = h_monotonic_ns() +
					 (unsigned long long)secs * 1000000000ULL;
		struct input_event ev[64];
		long got = 0;

		while (h_monotonic_ns() < end) {
			uint64_t f = test_irq_flag;

			test_irq_flag = 0;
			evdev_feed_mouse(1, 1, 0, 0, 0);
			evdev_feed_keyboard(0x1E);
			evdev_feed_keyboard(0x9E);
			test_irq_flag = f;
			got += drain(mc, ev, 64);
			h_usleep(200);
		}
		CHECK(got > 0, "mouse reader got events");
		evdev_release(mc);
	}
	for (int i = 0; i < 4; i++) {
		__atomic_store_n(&dev_args[i].stop, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&open_args[i].stop, 1, __ATOMIC_RELEASE);
	}
	{
		long rounds = 0, opens = 0;

		for (int i = 0; i < 4; i++) {
			h_thread_join(dth[i]);
			h_thread_join(oth[i]);
			rounds += dev_args[i].opens;
			opens += open_args[i].opens;
		}
		h_printf("   %ld register/unregister rounds, %ld racing opens\n",
			 rounds, opens);
		CHECK(rounds > 10, "device churn ran");
	}
	CHECK(evdev_next_unit(2) == -1, "everything unregistered");
}

// ---------------------------------------------------------------------------
// handles: in-kernel listeners
// ---------------------------------------------------------------------------

struct tl {
	struct input_handle h;
	volatile long packets, values, syns, btn_left;
	volatile int dead; // set after the handle was detached
	int users_calls, last_users;
	int disconnects;
	int open_on_connect;
	struct tl *next;
};

static struct tl *g_tl_list;
static int g_tl_connects;
static int g_tl_want_static; // a test: the built-ins must never be offered

static void tl_events(struct input_handle *h, const struct input_value *v,
		      unsigned int n, uint64_t mono)
{
	struct tl *t = (struct tl *)h->private;

	(void)mono;
	if (t->dead)
		test_fatal("packet for a detached handle", h->name);
	if (!irqs_disabled() || !test_spins_held)
		test_fatal("handle callback outside the event lock", h->name);
	__atomic_fetch_add(&t->packets, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&t->values, (long)n, __ATOMIC_RELAXED);
	if (n && v[n - 1].type == EV_SYN && v[n - 1].code == SYN_REPORT)
		__atomic_fetch_add(&t->syns, 1, __ATOMIC_RELAXED);
	for (unsigned int i = 0; i < n; i++)
		if (v[i].type == EV_KEY && v[i].code == BTN_LEFT)
			t->btn_left = v[i].value;
}

static void tl_users(struct input_handle *h, unsigned int users)
{
	struct tl *t = (struct tl *)h->private;

	t->users_calls++;
	t->last_users = (int)users;
}

static int tl_connect(struct input_handler *hd, struct input_dev *dev)
{
	struct tl *t;

	if (dev->static_storage)
		g_tl_want_static++;
	if (!input_test_bit(EV_ABS, dev->evbit))
		return -ENODEV;
	t = (struct tl *)h_calloc(1, sizeof(*t));
	t->h.name = "test-listener";
	t->h.handler = hd;
	t->h.private = t;
	t->h.events = tl_events;
	t->h.users_changed = tl_users;
	if (input_register_handle(dev, &t->h)) {
		h_free(t);
		return -ENODEV;
	}
	t->next = g_tl_list;
	g_tl_list = t;
	g_tl_connects++;
	return 0;
}

static void tl_disconnect(struct input_handle *h)
{
	struct tl *t = (struct tl *)h->private;

	t->disconnects++;
	input_unregister_handle(h);
	t->dead = 1;
	/* Kept on the list (and freed at the end) so the test can look. */
}

static struct input_handler g_tl_handler = {
	.name = "test-listener",
	.connect = tl_connect,
	.disconnect = tl_disconnect,
};

static struct tl *tl_find(struct input_dev *dev)
{
	for (struct tl *t = g_tl_list; t; t = t->next)
		if (t->h.dev == dev)
			return t;
	return NULL;
}

static void tl_free_all(void)
{
	while (g_tl_list) {
		struct tl *t = g_tl_list;

		g_tl_list = t->next;
		CHECK(t->h.dev == NULL, "listener still attached at the end");
		h_free(t);
	}
}

struct detach_race {
	struct input_dev *dev;
	volatile int stop;
	long rounds;
};

static void *detach_racer(void *p)
{
	struct detach_race *r = (struct detach_race *)p;

	while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
		struct tl *t = (struct tl *)h_calloc(1, sizeof(*t));

		t->h.name = "racer";
		t->h.handler = &g_tl_handler;
		t->h.private = t;
		t->h.events = tl_events;
		if (input_register_handle(r->dev, &t->h) == 0) {
			if (input_handle_open(&t->h) == 0) {
				h_yield();
				input_handle_close(&t->h);
			}
			input_unregister_handle(&t->h);
			/* From here a callback would be a use after free. */
			t->dead = 1;
		}
		h_free(t);
		r->rounds++;
	}
	test_thread_done();
	return NULL;
}

static void test_handles(void)
{
	static const struct contact one[] = { { 0, 100, 100, 0, 1 } };
	static const struct contact none[] = { { 0, 0, 0, 0, 0 } };
	struct input_dev *a, *b;
	struct evdev_client *c;
	struct tl *ta, *tb;
	long before;

	h_printf("== handles\n");
	g_open_calls = g_close_calls = g_fail_open = 0;
	g_tl_connects = g_tl_want_static = 0;

	/* A device that exists before the handler. */
	a = mk_hp_touchpad();
	a->open = cb_open;
	a->close = cb_close;
	CHECK(input_register_device(a) == 0, "register a");
	CHECK(input_register_handler(&g_tl_handler) == 0, "register handler");
	CHECK(input_register_handler(&g_tl_handler) == -EBUSY,
	      "double registration refused");
	CHECK(g_tl_connects == 1, "offered the existing device (%d)",
	      g_tl_connects);
	CHECK(g_tl_want_static == 0, "built-in devices offered");
	ta = tl_find(a);
	CHECK(ta != NULL, "handle attached to a");
	if (!ta)
		return;

	/* Closed handle: no packets, device not opened. */
	tp_frame(a, one, 1, 1, 0);
	CHECK(ta->packets == 0, "closed handle got %ld packets", ta->packets);
	CHECK(g_open_calls == 0, "device opened without users");
	CHECK(!input_device_enabled(a), "enabled without users");

	/* Kernel open: driver open once; packets flow. */
	CHECK(input_handle_open(&ta->h) == 0, "handle open");
	CHECK(g_open_calls == 1, "driver open for the kernel user");
	CHECK(input_device_enabled(a) && !input_device_user_open(a),
	      "kernel user counts as enabled, not as user space");
	tp_frame(a, none, 1, 2, 1);
	CHECK(ta->packets == 1 && ta->syns == 1, "one packet (%ld)",
	      ta->packets);
	CHECK(ta->btn_left == 1, "button seen");

	/* A user reader: no second driver open, users_changed 0 -> 1. */
	c = open_unit(unit_of(a));
	CHECK(g_open_calls == 1, "second open did not reach the driver");
	CHECK(ta->users_calls == 1 && ta->last_users == 1,
	      "users_changed on the first reader (%d, %d)", ta->users_calls,
	      ta->last_users);
	before = ta->packets;
	tp_frame(a, one, 1, 3, 0);
	CHECK(ta->packets == before + 1, "handle still gets packets");
	{
		struct input_event ev[32];

		CHECK(drain(c, ev, 32) > 0, "reader gets the packet too");
	}
	evdev_release(c);
	CHECK(ta->users_calls == 2 && ta->last_users == 0,
	      "users_changed on the last reader");
	CHECK(g_close_calls == 0, "kernel user keeps the device open");

	/* Kernel close: the driver's close. */
	input_handle_close(&ta->h);
	CHECK(g_close_calls == 1, "driver close after the last user");
	before = ta->packets;
	tp_frame(a, none, 1, 4, 0);
	CHECK(ta->packets == before, "closed handle got a packet");

	/* A device registered after the handler is offered at once; a
	 * keyboard-like device without EV_ABS is refused by connect. */
	b = mk_hp_touchpad();
	CHECK(input_register_device(b) == 0, "register b");
	CHECK(g_tl_connects == 2, "offered the new device");
	tb = tl_find(b);
	CHECK(tb != NULL, "handle attached to b");
	CHECK(tb && input_handle_open(&tb->h) == 0, "open b");
	/* Removal with a held key: the handle sees the release, then is
	 * detached. */
	tp_frame(b, one, 1, 5, 1);
	CHECK(tb && tb->btn_left == 1, "b button down");
	input_unregister_device(b);
	CHECK(tb && tb->disconnects == 1 && tb->dead,
	      "disconnect on removal");
	CHECK(tb && tb->btn_left == 0, "release seen before the detach");

	/* Detach racing a producer: no callback after unregister returns. */
	{
		struct producer_arg pa = { .dev = a, .stop = 0, .frames = 0,
					   .seed = 77 };
		struct detach_race dr = { .dev = a, .stop = 0, .rounds = 0 };
		unsigned long pt, rt;
		unsigned long long end = h_monotonic_ns() + 1000000000ULL;

		h_thread_start(producer, &pa, &pt);
		h_thread_start(detach_racer, &dr, &rt);
		while (h_monotonic_ns() < end)
			h_usleep(1000);
		__atomic_store_n(&dr.stop, 1, __ATOMIC_RELEASE);
		h_thread_join(rt);
		__atomic_store_n(&pa.stop, 1, __ATOMIC_RELEASE);
		h_thread_join(pt);
		h_printf("   %ld attach/open/detach rounds against %d frames\n",
			 dr.rounds, pa.frames);
		CHECK(dr.rounds > 10, "detach race ran");
	}
	CHECK(a->kernel_users == 0, "kernel users balanced (%u)",
	      a->kernel_users);

	/* Handler unregistration detaches its handles. */
	input_unregister_handler(&g_tl_handler);
	CHECK(ta->disconnects == 1 && ta->dead && ta->h.dev == NULL,
	      "handler unregistration detached a");
	CHECK(!a->handles, "no handles left on a");
	input_unregister_device(a);
	tl_free_all();
	CHECK(evdev_next_unit(2) == -1, "everything unregistered");
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// fidelity: behaviour of the core in detail
// ---------------------------------------------------------------------------

#define MS (1000000ULL)

struct cbrec {
	int n;
	struct ev ev[32];
};
static struct cbrec g_cb;

static int rec_event(struct input_dev *dev, unsigned int type,
		     unsigned int code, int value)
{
	(void)dev;
	if (g_cb.n < 32) {
		g_cb.ev[g_cb.n].type = (uint16_t)type;
		g_cb.ev[g_cb.n].code = (uint16_t)code;
		g_cb.ev[g_cb.n].value = value;
	}
	g_cb.n++;
	return 0;
}

static int cb_has(unsigned type, unsigned code, int value)
{
	for (int i = 0; i < g_cb.n && i < 32; i++)
		if (g_cb.ev[i].type == type && g_cb.ev[i].code == code &&
		    g_cb.ev[i].value == value)
			return 1;
	return 0;
}

static int g_open_reports;

static int report_on_open(struct input_dev *dev)
{
	g_open_reports++;
	input_report_key(dev, KEY_A, 1);
	input_sync(dev);
	return 0;
}

static struct input_dev *mk_keys(const char *name)
{
	struct input_dev *k = input_allocate_device();

	k->name = name;
	input_set_capability(k, EV_KEY, KEY_A);
	input_set_capability(k, EV_KEY, KEY_B);
	input_set_capability(k, EV_REP, 0);
	return k;
}

static void fid_softrepeat(void)
{
	struct input_dev *k = mk_keys("soft repeat");
	struct input_dev *p = mk_keys("driver repeat");
	struct evdev_client *c, *pc;
	struct input_event e[16];
	int n;

	p->rep[REP_DELAY] = 500;
	p->rep[REP_PERIOD] = 100;
	CHECK(input_register_device(k) == 0 && input_register_device(p) == 0,
	      "register keyboards");
	CHECK(k->softrepeat && !p->softrepeat && k->rep[REP_DELAY] == 250 &&
		      k->rep[REP_PERIOD] == 33,
	      "software autorepeat only without preset timings");
	c = open_unit(unit_of(k));
	pc = open_unit(unit_of(p));

	test_set_clock_ns(1000 * MS);
	input_report_key(k, KEY_A, 1);
	input_sync(k);
	input_report_key(p, KEY_A, 1);
	input_sync(p);
	CHECK(test_wq_pending() == 1, "one repeat armed (%d)",
	      test_wq_pending());
	test_set_clock_ns(1249 * MS);
	CHECK(test_wq_run() == 0, "nothing before the delay");
	test_set_clock_ns(1250 * MS);
	CHECK(test_wq_run() == 1, "the first repeat after the delay");
	test_set_clock_ns(1282 * MS);
	CHECK(test_wq_run() == 0, "nothing before the period");
	test_set_clock_ns(1283 * MS);
	CHECK(test_wq_run() == 1, "the next one a period later");
	n = drain(c, e, 16);
	CHECK(n == 6 && e[0].code == KEY_A && e[0].value == 1 &&
		      e[1].type == EV_SYN && e[1].value == 0 &&
		      e[2].type == EV_KEY && e[2].code == KEY_A &&
		      e[2].value == 2 && e[3].type == EV_SYN &&
		      e[3].code == SYN_REPORT && e[3].value == 1 &&
		      e[2].time.tv_sec == 1700000001 &&
		      e[2].time.tv_usec == 250000 && e[4].value == 2 &&
		      e[4].time.tv_usec == 283000,
	      "repeat stream (%d)", n);
	/* Another key takes the repeat over; any release stops it. */
	input_report_key(k, KEY_B, 1);
	input_sync(k);
	test_set_clock_ns(1532 * MS);
	CHECK(test_wq_run() == 0, "the delay starts again");
	test_set_clock_ns(1533 * MS);
	CHECK(test_wq_run() == 1, "repeat of the newest key");
	input_report_key(k, KEY_A, 0);
	input_sync(k);
	CHECK(test_wq_pending() == 0, "a release stops the repeat");
	test_set_clock_ns(2000 * MS);
	CHECK(test_wq_run() == 0, "nothing after the release");
	expect_stream("newest key repeats", c, { EV_KEY, KEY_B, 1 }, SYN,
		      { EV_KEY, KEY_B, 2 }, { EV_SYN, SYN_REPORT, 1 },
		      { EV_KEY, KEY_A, 0 }, SYN);
	expect_stream("the driver's own repeat", pc, { EV_KEY, KEY_A, 1 }, SYN);
	/* A period of 0: one repeat, no more. */
	{
		unsigned int rep[2] = { 100, 0 };

		CHECK(ioc(c, EVIOCSREP, rep) == 0, "period 0");
		input_report_key(k, KEY_A, 1);
		input_sync(k);
		test_set_clock_ns(2100 * MS);
		/* The REP events and the press: delay 100 ms but a period of 0
		 * arms nothing at all. */
		CHECK(test_wq_pending() == 0, "no repeat without a period");
		rep[1] = 40;
		CHECK(ioc(c, EVIOCSREP, rep) == 0, "period 40");
		input_report_key(k, KEY_B, 0);
		input_report_key(k, KEY_B, 1);
		input_sync(k);
		CHECK(test_wq_pending() == 1, "armed again");
	}
	/* Removal with a key down and a repeat armed: released, disarmed. */
	input_unregister_device(k);
	CHECK(test_wq_pending() == 0, "removal disarms the repeat");
	test_set_clock_ns(5000 * MS);
	CHECK(test_wq_run() == 0, "nothing runs for a removed device");
	evdev_release(c);
	evdev_release(pc);
	input_unregister_device(p);
	test_set_clock_ns(0);
}

static void fid_ready(void)
{
	struct input_dev *d = input_allocate_device();
	struct evdev_client *c;
	struct input_event w;

	d->name = "led keyboard";
	input_set_capability(d, EV_KEY, KEY_A);
	input_set_capability(d, EV_LED, LED_NUML);
	input_set_capability(d, EV_LED, LED_CAPSL);
	d->event = rec_event;
	CHECK(input_register_device(d) == 0, "register");
	g_cb.n = 0;
	/* Not open: the state changes, the device hears nothing. */
	input_event(d, EV_LED, LED_CAPSL, 1);
	CHECK(g_cb.n == 0 && (d->led[0] & (1UL << LED_CAPSL)),
	      "no callback while closed (%d)", g_cb.n);
	/* The first open replays the state: every LED, and nothing else. */
	c = open_unit(unit_of(d));
	CHECK(g_cb.n == 2 && cb_has(EV_LED, LED_NUML, 0) &&
		      cb_has(EV_LED, LED_CAPSL, 1),
	      "state replayed at open (%d)", g_cb.n);
	g_cb.n = 0;
	h_memset(&w, 0, sizeof(w));
	w.type = EV_LED;
	w.code = LED_NUML;
	w.value = 1;
	CHECK(evdev_write(c, &w, EV_SZ) == EV_SZ && g_cb.n == 1 &&
		      cb_has(EV_LED, LED_NUML, 1),
	      "a client's LED write reaches the device");
	/* The last close switches the lit ones off. */
	g_cb.n = 0;
	evdev_release(c);
	CHECK(g_cb.n == 2 && cb_has(EV_LED, LED_NUML, 0) &&
		      cb_has(EV_LED, LED_CAPSL, 0),
	      "LEDs off at the last close (%d)", g_cb.n);
	g_cb.n = 0;
	input_event(d, EV_LED, LED_CAPSL, 0);
	CHECK(g_cb.n == 0, "closed again: no callback");
	input_unregister_device(d);
}

static void fid_keycode(void)
{
	static uint8_t table[4] = { KEY_A, KEY_B, KEY_C, KEY_RESERVED };
	struct input_dev *d = input_allocate_device();
	struct evdev_client *c;
	struct input_keymap_entry ke;
	unsigned int kc[2];

	d->name = "keymap keyboard";
	d->keycode = table;
	d->keycodesize = 1;
	d->keycodemax = 4;
	for (int i = 0; i < 3; i++)
		input_set_capability(d, EV_KEY, table[i]);
	CHECK(input_register_device(d) == 0, "register");
	c = open_unit(unit_of(d));

	kc[0] = 1, kc[1] = 0;
	CHECK(ioc(c, EVIOCGKEYCODE, kc) == 0 && kc[1] == KEY_B,
	      "GKEYCODE %u", kc[1]);
	kc[0] = 4;
	CHECK(ioc(c, EVIOCGKEYCODE, kc) == -EINVAL, "scancode out of range");
	h_memset(&ke, 0, sizeof(ke));
	ke.flags = INPUT_KEYMAP_BY_INDEX;
	ke.index = 2;
	CHECK(ioc(c, EVIOCGKEYCODE_V2, &ke) == 0 && ke.keycode == KEY_C &&
		      ke.len == 4 && ke.scancode[0] == 2 && ke.index == 2,
	      "GKEYCODE_V2 by index");
	h_memset(&ke, 0, sizeof(ke));
	ke.len = 2;
	ke.scancode[0] = 0;
	CHECK(ioc(c, EVIOCGKEYCODE_V2, &ke) == 0 && ke.keycode == KEY_A,
	      "GKEYCODE_V2 by a 2-byte scancode");
	ke.len = 3;
	CHECK(ioc(c, EVIOCGKEYCODE_V2, &ke) == -EINVAL, "3-byte scancode");

	/* Remapping a key that is down releases it. */
	input_report_key(d, KEY_A, 1);
	input_sync(d);
	kc[0] = 0, kc[1] = KEY_Z;
	CHECK(ioc(c, EVIOCSKEYCODE, kc) == 0 && table[0] == KEY_Z,
	      "SKEYCODE");
	CHECK(input_test_bit(KEY_Z, d->keybit) &&
		      !input_test_bit(KEY_A, d->keybit) &&
		      !input_test_bit(KEY_A, d->key),
	      "key bits follow the table");
	expect_stream("remapped key released", c, { EV_KEY, KEY_A, 1 }, SYN,
		      { EV_KEY, KEY_A, 0 }, { EV_SYN, SYN_REPORT, 1 });
	/* A code still produced elsewhere stays declared. */
	kc[0] = 3, kc[1] = KEY_B;
	CHECK(ioc(c, EVIOCSKEYCODE, kc) == 0, "second KEY_B");
	kc[0] = 1, kc[1] = KEY_X;
	CHECK(ioc(c, EVIOCSKEYCODE, kc) == 0 &&
		      input_test_bit(KEY_B, d->keybit) &&
		      input_test_bit(KEY_X, d->keybit),
	      "KEY_B still declared");
	kc[0] = 2, kc[1] = KEY_MAX + 1;
	CHECK(ioc(c, EVIOCSKEYCODE, kc) == -EINVAL, "key code beyond KEY_MAX");
	kc[1] = 0x1ff;
	CHECK(ioc(c, EVIOCSKEYCODE, kc) == -EINVAL,
	      "key code wider than the table");
	h_memset(&ke, 0, sizeof(ke));
	ke.len = 33;
	CHECK(ioc(c, EVIOCSKEYCODE_V2, &ke) == -EINVAL, "oversized scancode");
	ke.len = 0;
	ke.flags = INPUT_KEYMAP_BY_INDEX;
	ke.index = 2;
	ke.keycode = KEY_RESERVED;
	CHECK(ioc(c, EVIOCSKEYCODE_V2, &ke) == 0 &&
		      !input_test_bit(KEY_RESERVED, d->keybit) &&
		      !input_test_bit(KEY_C, d->keybit),
	      "KEY_RESERVED never declared");
	CHECK(ioc(c, EVIOCSKEYCODE, BAD_PTR) == -EFAULT, "SKEYCODE bad ptr");
	evdev_release(c);
	input_unregister_device(d);
}

static void fid_mt(void)
{
	/* Total force: ABS_PRESSURE is the sum of the contacts'. */
	for (int total = 0; total < 2; total++) {
		struct input_dev *d = input_allocate_device();
		struct evdev_client *c;
		struct input_event e[64];
		int n, p = -1;

		d->name = total ? "total force" : "oldest force";
		input_set_abs_params(d, ABS_MT_POSITION_X, 0, 1000, 0, 0);
		input_set_abs_params(d, ABS_MT_POSITION_Y, 0, 1000, 0, 0);
		input_set_abs_params(d, ABS_MT_PRESSURE, 0, 255, 0, 0);
		CHECK(input_mt_init_slots(d, 2, INPUT_MT_DIRECT |
						       (total ? INPUT_MT_TOTAL_FORCE :
							        0)) == 0,
		      "slots");
		CHECK(input_test_bit(ABS_PRESSURE, d->absbit) &&
			      d->absinfo[ABS_PRESSURE].maximum == 255,
		      "pressure axis copied");
		CHECK(input_register_device(d) == 0, "register");
		c = open_unit(unit_of(d));
		for (int i = 0; i < 2; i++) {
			input_mt_slot(d, i);
			input_mt_report_slot_state(d, MT_TOOL_FINGER, 1);
			input_report_abs(d, ABS_MT_POSITION_X, 100 * (i + 1));
			input_report_abs(d, ABS_MT_PRESSURE, 10 * (i + 1));
		}
		input_mt_sync_frame(d);
		input_sync(d);
		n = drain(c, e, 64);
		for (int i = 0; i < n; i++)
			if (e[i].type == EV_ABS && e[i].code == ABS_PRESSURE)
				p = e[i].value;
		CHECK(p == (total ? 30 : 10), "%s: ABS_PRESSURE %d", d->name,
		      p);
		evdev_release(c);
		input_unregister_device(d);
	}

	/* Slot counts: up to 1024. */
	{
		struct input_dev *d = input_allocate_device();

		CHECK(input_mt_init_slots(d, 1025, 0) == -EINVAL,
		      "1025 slots refused");
		CHECK(input_mt_init_slots(d, 1024, 0) == 0 &&
			      d->mt->num_slots == 1024 &&
			      d->absinfo[ABS_MT_SLOT].maximum == 1023,
		      "1024 slots");
		input_free_device(d);
	}

	/* More slots than one copy pass of EVIOCGMTSLOTS. */
	{
		struct input_dev *d = input_allocate_device();
		struct evdev_client *c;
		int32_t *buf = (int32_t *)h_calloc(1 + 110, sizeof(int32_t));
		int ok = 1;

		d->name = "hundred slots";
		input_set_abs_params(d, ABS_MT_POSITION_X, 0, 10000, 0, 0);
		input_set_abs_params(d, ABS_MT_POSITION_Y, 0, 10000, 0, 0);
		CHECK(input_mt_init_slots(d, 100, INPUT_MT_DIRECT) == 0,
		      "100 slots");
		CHECK(input_register_device(d) == 0, "register");
		c = open_unit(unit_of(d));
		for (int i = 0; i < 100; i++) {
			input_mt_slot(d, i);
			input_mt_report_slot_state(d, MT_TOOL_FINGER, 1);
			input_report_abs(d, ABS_MT_POSITION_X, 7 * i + 3);
		}
		input_mt_sync_frame(d);
		input_sync(d);
		for (int i = 0; i < 111; i++)
			buf[i] = 0x55555555;
		buf[0] = ABS_MT_POSITION_X;
		CHECK(ioc(c, EVIOCGMTSLOTS(4 * 111), buf) == 0, "MTSLOTS 100");
		for (int i = 0; i < 100; i++)
			ok &= buf[1 + i] == 7 * i + 3;
		CHECK(ok && buf[101] == 0x55555555, "all 100 values, no more");
		for (int i = 0; i < 111; i++)
			buf[i] = 0x55555555;
		buf[0] = ABS_MT_POSITION_X;
		CHECK(ioc(c, EVIOCGMTSLOTS(4 * 71), buf) == 0 &&
			      buf[70] == 7 * 69 + 3 &&
			      buf[71] == 0x55555555,
		      "70 values for a 70-slot buffer");
		h_free(buf);
		evdev_release(c);
		input_unregister_device(d);
	}

	/* Assignment: dmax 0 is no limit; random frames keep the rules. */
	{
		struct input_dev *t = mk_touchpad("track nolimit", 1, 2, NULL,
						  NULL, INPUT_MT_TRACK, 5);
		struct input_mt_pos p[5];
		int slots[5];

		CHECK(input_register_device(t) == 0, "register");
		p[0].x = 100, p[0].y = 100;
		CHECK(input_mt_assign_slots(t, slots, p, 1, 0) == 0 &&
			      slots[0] == 0,
		      "first contact");
		input_mt_slot(t, 0);
		input_mt_report_slot_state(t, MT_TOOL_FINGER, 1);
		input_report_abs(t, ABS_MT_POSITION_X, 100);
		input_report_abs(t, ABS_MT_POSITION_Y, 100);
		input_mt_sync_frame(t);
		input_sync(t);
		p[0].x = 3000, p[0].y = 2000;
		CHECK(input_mt_assign_slots(t, slots, p, 1, 0) == 0 &&
			      slots[0] == 0,
		      "no distance limit: the contact keeps its slot (%d)",
		      slots[0]);
		CHECK(input_mt_assign_slots(t, slots, p, 1, 50) == 0 &&
			      slots[0] == 1,
		      "with a limit: a new slot (%d)", slots[0]);

		h_srandom(4242);
		for (int round = 0; round < 2000; round++) {
			int n = 1 + (int)(h_random() % 5), used[5] = { 0 };
			int good = 1, active = 0;

			/* Contacts at least 100 units apart (2.5 dmax): the
			 * best match of an unmoved set is unambiguous. */
			for (int i = 0; i < n; i++) {
				int far;

				do {
					p[i].x = (int16_t)(h_random() % 3200);
					p[i].y = (int16_t)(h_random() % 2100);
					far = 1;
					for (int j = 0; j < i; j++)
						if ((p[i].x - p[j].x) * (p[i].x - p[j].x) +
							    (p[i].y - p[j].y) * (p[i].y - p[j].y) <
						    100 * 100)
							far = 0;
				} while (!far);
			}
			for (int i = 0; i < 5; i++)
				active += input_mt_is_active(&t->mt->slots[i]);
			if (input_mt_assign_slots(t, slots, p, n, 40) != 0)
				good = 0;
			/* Distinct slots.  A contact that matches no active
			 * slot gets an empty one while there are any, else -1
			 * (the slots of contacts that went away are released
			 * only by the frame that follows). */
			{
				int assigned = 0, empty = 5 - active;

				for (int i = 0; i < n && good; i++) {
					if (slots[i] == -1)
						continue;
					if (slots[i] < 0 || slots[i] >= 5 ||
					    used[slots[i]]++)
						good = 0;
					assigned++;
				}
				if (assigned < (n < empty ? n : empty))
					good = 0;
			}
			CHECK(good, "round %d: distinct valid slots", round);
			if (!good) {
				for (int i = 0; i < 5; i++)
					h_printf("   slot %d: id %d at %d,%d\n", i,
						 input_mt_get_value(&t->mt->slots[i], ABS_MT_TRACKING_ID),
						 input_mt_get_value(&t->mt->slots[i], ABS_MT_POSITION_X),
						 input_mt_get_value(&t->mt->slots[i], ABS_MT_POSITION_Y));
				for (int i = 0; i < n; i++)
					h_printf("   contact %d at %d,%d -> %d\n", i,
						 p[i].x, p[i].y, slots[i]);
				break;
			}
			/* Report them, so the next round tracks against
			 * these; a contact exactly where it was keeps its
			 * slot. */
			for (int i = 0; i < n; i++) {
				if (slots[i] < 0)
					continue;
				input_mt_slot(t, slots[i]);
				input_mt_report_slot_state(t, MT_TOOL_FINGER,
							   1);
				input_report_abs(t, ABS_MT_POSITION_X, p[i].x);
				input_report_abs(t, ABS_MT_POSITION_Y, p[i].y);
			}
			input_mt_sync_frame(t);
			input_sync(t);
			{
				int again[5];

				CHECK(input_mt_assign_slots(t, again, p, n,
							    40) == 0,
				      "again");
				for (int i = 0; i < n; i++)
					good &= slots[i] < 0 ||
						again[i] == slots[i];
				CHECK(good, "round %d: unmoved contacts keep "
					    "their slots", round);
				if (!good)
					break;
			}
		}
		input_unregister_device(t);
	}
}

static void fid_evdev(void)
{
	struct input_dev *d = input_allocate_device();
	struct evdev_client *c;
	struct input_event e[256];
	int n, drops;

	/* Queue size: 8 packets of the estimate, at least 64, a power of two:
	 * 1 + REL_X + 7 = 9 per packet -> 72 -> 128 events. */
	d->name = "small mouse";
	input_set_capability(d, EV_REL, REL_X);
	input_set_capability(d, EV_KEY, BTN_LEFT);
	input_set_capability(d, EV_MSC, MSC_SCAN);
	CHECK(input_register_device(d) == 0, "register");
	CHECK(d->hint_events_per_packet == 9, "estimate %u",
	      d->hint_events_per_packet);
	c = open_unit(unit_of(d));
	for (int i = 1; i <= 63; i++) {
		input_report_rel(d, REL_X, i);
		input_sync(d);
	}
	n = drain(c, e, 256);
	CHECK(n == 126 && e[0].type == EV_REL, "126 events fit (%d)", n);
	for (int i = 1; i <= 64; i++) {
		input_report_rel(d, REL_X, i);
		input_sync(d);
	}
	n = drain(c, e, 256);
	drops = 0;
	for (int i = 0; i < n; i++)
		drops += e[i].type == EV_SYN && e[i].code == SYN_DROPPED;
	CHECK(drops == 1 && e[0].code == SYN_DROPPED, "128 events do not (%d)",
	      n);

	/* A write injects at most 4096 bytes. */
	{
		struct input_event *w =
			(struct input_event *)h_calloc(200, sizeof(*w));

		for (int i = 0; i < 200; i++) {
			w[i].type = EV_MSC;
			w[i].code = MSC_SCAN;
			w[i].value = i;
		}
		CHECK(evdev_write(c, w, 200 * EV_SZ) == 170 * EV_SZ,
		      "write cut to 170 events");
		h_free(w);
		input_sync(d);
		while (drain(c, e, 256) > 0)
			;
	}

	/* SYN_DROPPED after a failed state read carries the time of the
	 * packet in progress, or now. */
	test_set_clock_ns(9000 * MS);
	input_set_timestamp(d, 8000 * MS + 5000);
	{
		int clk = CLOCK_MONOTONIC;

		CHECK(ioc(c, EVIOCSCLOCKID, &clk) == 0, "monotonic");
	}
	CHECK(ioc(c, EVIOCGKEY(96), BAD_PTR) == -EFAULT, "GKEY bad ptr");
	input_report_rel(d, REL_X, 5);
	input_sync(d);
	n = drain(c, e, 16);
	CHECK(n == 3 && e[0].code == SYN_DROPPED && e[0].time.tv_sec == 8 &&
		      e[0].time.tv_usec == 5 && e[1].time.tv_usec == 5,
	      "SYN_DROPPED at the packet's time (%d: %lld.%06lld)", n,
	      (long long)e[0].time.tv_sec, (long long)e[0].time.tv_usec);
	CHECK(ioc(c, EVIOCGKEY(96), BAD_PTR) == -EFAULT, "GKEY bad ptr");
	input_report_rel(d, REL_X, 6);
	input_sync(d);
	n = drain(c, e, 16);
	CHECK(n == 3 && e[0].code == SYN_DROPPED && e[0].time.tv_sec == 9 &&
		      e[0].time.tv_usec == 0,
	      "SYN_DROPPED now without one (%d)", n);

	/* Both parts of a packet split for lack of room share its time. */
	test_set_clock_ns(0);
	{
		int same = 1;

		for (int i = 0; i < 25; i++) {
			input_event(d, EV_MSC, MSC_SCAN, i);
			h_usleep(100);
		}
		input_sync(d);
		n = drain(c, e, 64);
		for (int i = 1; i < n; i++)
			same &= e[i].time.tv_sec == e[0].time.tv_sec &&
				e[i].time.tv_usec == e[0].time.tv_usec;
		CHECK(n > 25 && same, "split packet: one time (%d)", n);
	}
	evdev_release(c);
	input_unregister_device(d);

	/* A client sees what the device reports from its open() on. */
	d = mk_keys("reports on open");
	d->open = report_on_open;
	CHECK(input_register_device(d) == 0, "register");
	g_open_reports = 0;
	c = open_unit(unit_of(d));
	CHECK(g_open_reports == 1, "open ran");
	expect_stream("report from open()", c, { EV_KEY, KEY_A, 1 }, SYN);
	evdev_release(c);
	input_unregister_device(d);
}

static void fid_copy_abs(void)
{
	struct input_dev *tp = mk_hp_touchpad();
	struct input_dev *pen = input_allocate_device();

	tp->absinfo[ABS_MT_POSITION_X].fuzz = 4;
	input_copy_abs(pen, ABS_X, tp, ABS_MT_POSITION_X);
	CHECK(input_test_bit(EV_ABS, pen->evbit) &&
		      input_test_bit(ABS_X, pen->absbit) &&
		      pen->absinfo[ABS_X].maximum == 3209 &&
		      pen->absinfo[ABS_X].resolution == 31 &&
		      pen->absinfo[ABS_X].fuzz == 4,
	      "copied with its fuzz");
	CHECK(tp->absinfo[ABS_X].fuzz == 0, "the MT copy has none");
	input_free_device(pen);
	input_free_device(tp);
}

static void fid_grab_handles(void)
{
	static const struct contact one[] = { { 0, 100, 100, 0, 1 } };
	static const struct contact two[] = { { 0, 120, 100, 0, 1 } };
	struct input_dev *a;
	struct evdev_client *c, *g;
	struct tl *t;
	long before;

	g_tl_connects = 0;
	CHECK(input_register_handler(&g_tl_handler) == 0, "handler");
	a = mk_hp_touchpad();
	CHECK(input_register_device(a) == 0, "register");
	t = tl_find(a);
	CHECK(t && input_handle_open(&t->h) == 0, "handle open");
	c = open_unit(unit_of(a));
	g = open_unit(unit_of(a));
	if (t) {
		before = t->packets;
		tp_frame(a, one, 1, 1, 0);
		CHECK(t->packets == before + 1, "handle hears the device");
		CHECK(ioc(g, EVIOCGRAB, (void *)1) == 0, "grab");
		before = t->packets;
		tp_frame(a, two, 1, 2, 0);
		CHECK(t->packets == before, "a grab keeps the handles out too");
		CHECK(ioc(g, EVIOCGRAB, (void *)0) == 0, "ungrab");
		tp_frame(a, one, 1, 3, 0);
		CHECK(t->packets == before + 1, "handle hears it again");
		input_handle_close(&t->h);
	}
	expect_stream("grabber", g, { EV_ABS, ABS_MT_TRACKING_ID, 0 },
		      { EV_ABS, ABS_MT_POSITION_X, 100 },
		      { EV_ABS, ABS_MT_POSITION_Y, 100 },
		      { EV_KEY, BTN_TOUCH, 1 }, { EV_KEY, BTN_TOOL_FINGER, 1 },
		      { EV_ABS, ABS_X, 100 }, { EV_ABS, ABS_Y, 100 },
		      { EV_MSC, MSC_TIMESTAMP, 1 }, SYN,
		      { EV_ABS, ABS_MT_POSITION_X, 120 }, { EV_ABS, ABS_X, 120 },
		      { EV_MSC, MSC_TIMESTAMP, 2 }, SYN,
		      { EV_ABS, ABS_MT_POSITION_X, 100 }, { EV_ABS, ABS_X, 100 },
		      { EV_MSC, MSC_TIMESTAMP, 3 }, SYN);
	evdev_release(g);
	evdev_release(c);
	input_unregister_device(a);
	input_unregister_handler(&g_tl_handler);
	tl_free_all();
}

static void test_fidelity(void)
{
	h_printf("== fidelity\n");
	fid_softrepeat();
	fid_ready();
	fid_keycode();
	fid_mt();
	fid_evdev();
	fid_copy_abs();
	fid_grab_handles();
	CHECK(evdev_next_unit(2) == -1, "everything unregistered");
}

// ---------------------------------------------------------------------------
// keyboard: the console's share of the keyboard
// ---------------------------------------------------------------------------

/* Set-1 make codes; the break code is the make code | 0x80. */
#define SC_A 0x1E
#define SC_B 0x30
#define SC_D 0x20
#define SC_LSHIFT 0x2A
#define SC_LCTRL 0x1D
#define SC_CAPS 0x3A

static void kb_tap(uint8_t sc)
{
	keyboard_buffer_add(sc);
	keyboard_buffer_add((uint8_t)(sc | 0x80));
}

static void expect_typed_(int line, const char *want)
{
	char got[256];

	test_console_take(got, sizeof(got));
	__atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED);
	if (h_strcmp(got, want) != 0) {
		__atomic_fetch_add(&g_fails, 1, __ATOMIC_RELAXED);
		h_printf("FAIL %s:%d: console typed \"%s\", want \"%s\"\n",
			 __FILE__, line, got, want);
	}
}

#define expect_typed(want) expect_typed_(__LINE__, want)

static void led_write(struct evdev_client *c, unsigned code, int value)
{
	struct input_event w[2];

	h_memset(w, 0, sizeof(w));
	w[0].type = EV_LED, w[0].code = (uint16_t)code, w[0].value = value;
	w[1].type = EV_SYN, w[1].code = SYN_REPORT;
	CHECK(evdev_write(c, w, 2 * EV_SZ) == 2 * EV_SZ, "LED write");
}

static void kb_drain(struct evdev_client *c)
{
	struct input_event junk[64];

	while (drain(c, junk, 64) > 0)
		;
}

/* The console's KD* requests as a console file with that token makes them;
 * `cur' NULL is the kernel itself (always permitted). */
static int kd(task_t *cur, uint64_t token, unsigned long req, long arg)
{
	return keyboard_kd_ioctl(req, (void *)arg, cur, token);
}

static int kd_kbdmode(void)
{
	int m = -1;

	CHECK(kd(NULL, 0, KDGKBMODE, (long)&m) == 0, "KDGKBMODE");
	return m;
}

static int kd_dispmode(void)
{
	int m = -1;

	CHECK(kd(NULL, 0, KDGETMODE, (long)&m) == 0, "KDGETMODE");
	return m;
}

#define expect_modes(kbd, disp)                                            \
	do {                                                               \
		int k_ = kd_kbdmode(), d_ = kd_dispmode();                 \
		CHECK(k_ == (kbd) && d_ == (disp),                         \
		      "modes kbd %d disp %d, want %d %d", k_, d_, (kbd),   \
		      (disp));                                             \
		CHECK(keyboard_kd_graphics() == ((disp) == KD_GRAPHICS),   \
		      "keyboard_kd_graphics %d", keyboard_kd_graphics());  \
	} while (0)

/* Race: keys from "interrupt context" against mode changes and owner
 * closes from another thread. */
struct kb_race {
	int stop;
	long keys;
};

static void *kb_race_keys(void *arg)
{
	struct kb_race *r = (struct kb_race *)arg;
	static const uint8_t keys[] = { SC_A, SC_B, SC_LSHIFT, SC_LCTRL,
					SC_D, 0x48 /* E0: Up */, SC_CAPS };
	unsigned i = 0;

	while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
		uint8_t sc = keys[i++ % sizeof(keys)];

		if (sc == 0x48)
			keyboard_buffer_add(0xE0);
		keyboard_buffer_add(sc);
		if (sc == 0x48)
			keyboard_buffer_add(0xE0);
		keyboard_buffer_add((uint8_t)(sc | 0x80));
		__atomic_fetch_add(&r->keys, 1, __ATOMIC_RELAXED);
	}
	test_thread_done();
	return NULL;
}

/* The console's output gate (console_owner.h) on its own: one word, two
 * signals, and the change that ends the ownership is the one that repaints. */
struct gate_race {
	unsigned word;
	int stop;
	long taken, released, rounds;
};

static void gate_count(struct gate_race *g, enum console_owner_change c)
{
	if (c == CONSOLE_OWNER_TAKEN)
		__atomic_fetch_add(&g->taken, 1, __ATOMIC_RELAXED);
	else if (c == CONSOLE_OWNER_RELEASED)
		__atomic_fetch_add(&g->released, 1, __ATOMIC_RELAXED);
}

static void *gate_race_master(void *arg)
{
	struct gate_race *g = (struct gate_race *)arg;
	int uf;

	while (!__atomic_load_n(&g->stop, __ATOMIC_ACQUIRE)) {
		gate_count(g, console_owner_master_set(&g->word));
		gate_count(g, console_owner_master_drop(&g->word, &uf));
		if (uf)
			CHECK(0, "race: master underflow");
		__atomic_fetch_add(&g->rounds, 1, __ATOMIC_RELAXED);
	}
	test_thread_done();
	return NULL;
}

static void *gate_race_kd(void *arg)
{
	struct gate_race *g = (struct gate_race *)arg;

	while (!__atomic_load_n(&g->stop, __ATOMIC_ACQUIRE)) {
		gate_count(g, console_owner_set_kd(&g->word, 1));
		gate_count(g, console_owner_set_kd(&g->word, 0));
		__atomic_fetch_add(&g->rounds, 1, __ATOMIC_RELAXED);
	}
	test_thread_done();
	return NULL;
}

static void test_output_gate_machine(void)
{
	unsigned w = 0;
	int uf;

	/* KD_GRAPHICS alone. */
	CHECK(console_owner_set_kd(&w, 1) == CONSOLE_OWNER_TAKEN, "kd takes");
	CHECK(console_owner_set_kd(&w, 1) == CONSOLE_OWNER_UNCHANGED,
	      "kd again: nothing");
	CHECK(console_owner_is_owned(&w), "kd owns");
	CHECK(console_owner_set_kd(&w, 0) == CONSOLE_OWNER_RELEASED,
	      "kd text releases");
	CHECK(console_owner_set_kd(&w, 0) == CONSOLE_OWNER_UNCHANGED,
	      "text again: nothing");
	CHECK(!console_owner_is_owned(&w) && w == 0, "free");

	/* A master alone, and two devices with one each. */
	CHECK(console_owner_master_set(&w) == CONSOLE_OWNER_TAKEN, "master");
	CHECK(console_owner_master_set(&w) == CONSOLE_OWNER_UNCHANGED,
	      "second device");
	CHECK(console_owner_master_drop(&w, &uf) == CONSOLE_OWNER_UNCHANGED &&
		      !uf,
	      "one device left");
	CHECK(console_owner_master_drop(&w, &uf) == CONSOLE_OWNER_RELEASED &&
		      !uf,
	      "last master releases");

	/* Both: the last one withdrawn releases, in either order. */
	CHECK(console_owner_master_set(&w) == CONSOLE_OWNER_TAKEN, "m+kd: m");
	CHECK(console_owner_set_kd(&w, 1) == CONSOLE_OWNER_UNCHANGED,
	      "m+kd: kd");
	CHECK(console_owner_master_drop(&w, &uf) == CONSOLE_OWNER_UNCHANGED,
	      "master first: still KD_GRAPHICS");
	CHECK(console_owner_is_owned(&w), "owned by KD_GRAPHICS");
	CHECK(console_owner_set_kd(&w, 0) == CONSOLE_OWNER_RELEASED,
	      "then KD_TEXT releases");
	CHECK(console_owner_set_kd(&w, 1) == CONSOLE_OWNER_TAKEN, "kd+m: kd");
	CHECK(console_owner_master_set(&w) == CONSOLE_OWNER_UNCHANGED,
	      "kd+m: m");
	CHECK(console_owner_set_kd(&w, 0) == CONSOLE_OWNER_UNCHANGED,
	      "KD_TEXT first: still a master");
	CHECK(console_owner_master_drop(&w, &uf) == CONSOLE_OWNER_RELEASED,
	      "then the drop releases");
	CHECK(w == 0, "free");

	/* A drop with no master counted changes nothing, KD bit included. */
	CHECK(console_owner_master_drop(&w, &uf) == CONSOLE_OWNER_UNCHANGED &&
		      uf && w == 0,
	      "drop without set: underflow refused");
	(void)console_owner_set_kd(&w, 1);
	CHECK(console_owner_master_drop(&w, &uf) == CONSOLE_OWNER_UNCHANGED &&
		      uf && w == CONSOLE_OWNER_KD,
	      "drop without set under KD_GRAPHICS");
	(void)console_owner_set_kd(&w, 0);

	/* Masters coming and going against KD flips on another thread:
	 * every take is matched by exactly one release, and it ends free. */
	{
		struct gate_race g;
		unsigned long t1, t2;
		unsigned long long end = h_monotonic_ns() + 300000000ULL;

		h_memset(&g, 0, sizeof(g));
		h_thread_start(gate_race_master, &g, &t1);
		h_thread_start(gate_race_kd, &g, &t2);
		while (h_monotonic_ns() < end)
			;
		__atomic_store_n(&g.stop, 1, __ATOMIC_RELEASE);
		h_thread_join(t1);
		h_thread_join(t2);
		CHECK(g.rounds > 0 && g.taken > 0, "gate race ran");
		CHECK(g.taken == g.released && g.word == 0,
		      "gate race: %ld taken, %ld released, word %#x", g.taken,
		      g.released, g.word);
	}
}

static void test_keyboard(void)
{
	struct evdev_client *kc;
	char junk[256];
	int dumps, redraws;
	uint64_t t1, t2, t3;
	task_t *user;

	h_printf("== keyboard\n");
	keyboard_init();
	test_set_capable(1);
	test_console_take(junk, sizeof(junk));
	kc = open_unit(0);
	led_write(kc, LED_NUML, 0);
	led_write(kc, LED_CAPSL, 0);
	led_write(kc, LED_SCROLLL, 0);
	kb_drain(kc);
	t1 = keyboard_console_file_token();
	t2 = keyboard_console_file_token();
	t3 = keyboard_console_file_token();
	CHECK(t1 && t2 && t3 && t1 != t2 && t2 != t3 && t1 != t3,
	      "tokens unique and nonzero");
	expect_modes(K_UNICODE, KD_TEXT);

	/* Nobody else has it: the console and event0 both get the key. */
	kb_tap(SC_A);
	expect_typed("a");
	expect_stream("free: event0", kc, { EV_KEY, KEY_A, 1 }, SYN,
		      { EV_KEY, KEY_A, 0 }, SYN);

	/* Bad values. */
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF + 1) == -EINVAL, "kbdmode range");
	CHECK(kd(NULL, t1, KDSETMODE, 2) == -EINVAL, "KD_TEXT0 refused");
	expect_modes(K_UNICODE, KD_TEXT);

	/* KD_GRAPHICS alone: the pointer is off (keyboard_kd_graphics), the
	 * keyboard still the console's. */
	CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "KD_GRAPHICS");
	expect_modes(K_UNICODE, KD_GRAPHICS);
	kb_tap(SC_B);
	expect_typed("b");
	kb_drain(kc);

	/* K_OFF: event0 only. */
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF");
	expect_modes(K_OFF, KD_GRAPHICS);
	kb_tap(SC_B);
	expect_typed("");
	expect_stream("off: event0", kc, { EV_KEY, KEY_B, 1 }, SYN,
		      { EV_KEY, KEY_B, 0 }, SYN);

	/* No console hotkey and no raw byte while off. */
	dumps = test_console_dumps();
	keyboard_buffer_add(SC_LCTRL);
	kb_tap(SC_D);
	keyboard_buffer_add(SC_LCTRL | 0x80);
	CHECK(test_console_dumps() == dumps, "off: no hotkey");
	expect_typed("");
	kb_drain(kc);

	/* The owner puts it back itself: the console has it at once, and
	 * there is nothing left to restore at its close. */
	CHECK(kd(NULL, t1, KDSKBMODE, K_UNICODE) == 0, "back to K_UNICODE");
	redraws = test_console_redraws();
	CHECK(kd(NULL, t1, KDSETMODE, KD_TEXT) == 0, "back to KD_TEXT");
	CHECK(test_console_redraws() == redraws + 1,
	      "graphics -> text repaints");
	expect_modes(K_UNICODE, KD_TEXT);
	kb_tap(SC_A);
	expect_typed("a");
	keyboard_buffer_add(SC_LCTRL);
	kb_tap(SC_D);
	keyboard_buffer_add(SC_LCTRL | 0x80);
	CHECK(test_console_dumps() == dumps + 1, "free: Ctrl+D dumps");
	expect_typed("");
	redraws = test_console_redraws();
	keyboard_console_file_closed(t1);
	CHECK(test_console_redraws() == redraws, "nothing to restore");
	expect_modes(K_UNICODE, KD_TEXT);

	/* The output gate through the console's modes.  KD_GRAPHICS takes
	 * the display once (setting it again takes nothing); KD_TEXT gives
	 * it back with one repaint. */
	{
		int takes = test_display_takes();

		CHECK(!console_display_owned(), "text: display free");
		CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "graphics");
		CHECK(console_display_owned(), "graphics owns the display");
		CHECK(test_display_takes() == takes + 1, "one take");
		CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "again");
		CHECK(test_display_takes() == takes + 1, "no second take");
		/* A keyboard mode change alone touches no display. */
		redraws = test_console_redraws();
		CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF");
		CHECK(kd(NULL, t1, KDSKBMODE, K_UNICODE) == 0, "K_UNICODE");
		CHECK(console_display_owned() &&
			      test_console_redraws() == redraws,
		      "kbdmode leaves the gate alone");
		CHECK(kd(NULL, t1, KDSETMODE, KD_TEXT) == 0, "text");
		CHECK(!console_display_owned() &&
			      test_console_redraws() == redraws + 1,
		      "text: free, one repaint");
		CHECK(kd(NULL, t1, KDSETMODE, KD_TEXT) == 0, "text again");
		CHECK(test_console_redraws() == redraws + 1,
		      "text again: no repaint");

		/* A display server on a DRM device, normal exit: master at
		 * open, KD_GRAPHICS, then the master dropped first (its file
		 * closes) and KD_TEXT last -- which is the one that
		 * repaints. */
		takes = test_display_takes();
		redraws = test_console_redraws();
		test_display_master(1);
		CHECK(console_display_owned() &&
			      test_display_takes() == takes + 1,
		      "master takes");
		CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "X graphics");
		CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "X K_OFF");
		CHECK(test_display_takes() == takes + 1, "already taken");
		test_display_master(0);
		CHECK(console_display_owned() &&
			      test_console_redraws() == redraws,
		      "master gone, KD_GRAPHICS holds: no repaint");
		CHECK(kd(NULL, t1, KDSETMODE, KD_TEXT) == 0, "X text");
		CHECK(kd(NULL, t1, KDSKBMODE, K_UNICODE) == 0, "X unicode");
		CHECK(!console_display_owned() &&
			      test_console_redraws() == redraws + 1,
		      "KD_TEXT last: one repaint");

		/* Killed: the console file closes first (the kernel restores
		 * KD_TEXT), the master still holds; its drop repaints. */
		redraws = test_console_redraws();
		test_display_master(1);
		CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "graphics");
		CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF");
		keyboard_console_file_closed(t1);
		expect_modes(K_UNICODE, KD_TEXT);
		CHECK(console_display_owned() &&
			      test_console_redraws() == redraws,
		      "restore under a master: no repaint yet");
		test_display_master(0);
		CHECK(!console_display_owned() &&
			      test_console_redraws() == redraws + 1,
		      "the drop repaints");

		/* ...and the other way round: master dropped, then the
		 * restore on close repaints. */
		redraws = test_console_redraws();
		test_display_master(1);
		CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "graphics");
		test_display_master(0);
		CHECK(test_console_redraws() == redraws, "drop: no repaint");
		keyboard_console_file_closed(t1);
		CHECK(!console_display_owned() &&
			      test_console_redraws() == redraws + 1,
		      "restore repaints once");
		expect_modes(K_UNICODE, KD_TEXT);
	}

	/* A Shift pressed on the console and released while off must not
	 * stay held; one pressed while off and released on the console is
	 * nothing to the console either. */
	keyboard_buffer_add(SC_LSHIFT);
	kb_tap(SC_A);
	expect_typed("A");
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF");
	keyboard_buffer_add(SC_LSHIFT | 0x80);
	CHECK(kd(NULL, t1, KDSKBMODE, K_UNICODE) == 0, "K_UNICODE");
	kb_tap(SC_A);
	expect_typed("a");
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF");
	keyboard_buffer_add(SC_LSHIFT);
	CHECK(kd(NULL, t1, KDSKBMODE, K_UNICODE) == 0, "K_UNICODE");
	kb_tap(SC_A);
	keyboard_buffer_add(SC_LSHIFT | 0x80);
	kb_tap(SC_B);
	expect_typed("ab");
	kb_drain(kc);

	/* LEDs: the console's lock state while it has the keyboard, the
	 * client's while it is off, the console's again after. */
	kb_tap(SC_CAPS);
	CHECK(keyboard_led_state() == LED_CAP, "console Caps Lock LED %#x",
	      keyboard_led_state());
	led_write(kc, LED_NUML, 1);
	CHECK(keyboard_led_state() == LED_CAP,
	      "client LEDs not shown while the console has it");
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF");
	CHECK(keyboard_led_state() == LED_NUM, "off: client LEDs %#x",
	      keyboard_led_state());
	led_write(kc, LED_CAPSL, 1);
	led_write(kc, LED_SCROLLL, 1);
	CHECK(keyboard_led_state() == (LED_NUM | LED_CAP | LED_SCR),
	      "off: client sets all three %#x", keyboard_led_state());
	led_write(kc, LED_NUML, 0);
	led_write(kc, LED_CAPSL, 0);
	led_write(kc, LED_SCROLLL, 0);
	CHECK(keyboard_led_state() == 0, "off: client clears %#x",
	      keyboard_led_state());
	kb_tap(SC_CAPS); // the session's Caps Lock, not the console's
	CHECK(keyboard_led_state() == 0, "off: keys leave LEDs alone");
	CHECK(kd(NULL, t1, KDSKBMODE, K_UNICODE) == 0, "K_UNICODE");
	CHECK(keyboard_led_state() == LED_CAP, "console LEDs back %#x",
	      keyboard_led_state());
	kb_tap(SC_A);
	expect_typed("A"); // the console's Caps Lock is still on
	kb_tap(SC_CAPS);
	CHECK(keyboard_led_state() == 0, "console Caps Lock off");
	kb_tap(SC_A);
	expect_typed("a");
	kb_drain(kc);

	/* The display server dies: its file's close puts everything back --
	 * KD_TEXT, the keyboard mode from before, no key held, the console's
	 * LEDs, a repaint.  Another file's close changes nothing. */
	CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "KD_GRAPHICS");
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF");
	led_write(kc, LED_NUML, 1);
	keyboard_buffer_add(SC_LSHIFT); // held in the session
	keyboard_buffer_add(SC_LCTRL);
	redraws = test_console_redraws();
	keyboard_console_file_closed(t2);
	keyboard_console_file_closed(0);
	expect_modes(K_OFF, KD_GRAPHICS);
	CHECK(test_console_redraws() == redraws, "other close: no repaint");
	keyboard_console_file_closed(t1);
	expect_modes(K_UNICODE, KD_TEXT);
	CHECK(test_console_redraws() == redraws + 1, "restore repaints");
	CHECK(keyboard_led_state() == 0, "console LEDs after restore %#x",
	      keyboard_led_state());
	kb_tap(SC_A); // neither Shift nor Ctrl is held for the console
	expect_typed("a");
	keyboard_buffer_add(SC_LSHIFT | 0x80);
	keyboard_buffer_add(SC_LCTRL | 0x80);
	kb_tap(SC_B);
	expect_typed("b");
	/* Closed a second time (cannot happen; the token is gone): nothing. */
	redraws = test_console_redraws();
	keyboard_console_file_closed(t1);
	CHECK(test_console_redraws() == redraws, "stale token");
	led_write(kc, LED_NUML, 0);
	kb_drain(kc);

	/* Each mode goes back with the file that asked for it: t1's keyboard
	 * mode with t1, t2's graphics with t2. */
	CHECK(kd(NULL, t1, KDSKBMODE, K_XLATE) == 0, "K_XLATE");
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF by t1");
	CHECK(kd(NULL, t2, KDSETMODE, KD_GRAPHICS) == 0, "KD_GRAPHICS by t2");
	keyboard_console_file_closed(t1);
	expect_modes(K_XLATE, KD_GRAPHICS);
	keyboard_console_file_closed(t2);
	expect_modes(K_XLATE, KD_TEXT);

	/* Keyboard taken over: the newest claim goes back to what it found,
	 * and an older claim's close hands its way back on. */
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF by t1");
	CHECK(kd(NULL, t2, KDSKBMODE, K_RAW) == 0, "K_RAW by t2");
	keyboard_console_file_closed(t1);
	expect_modes(K_RAW, KD_TEXT);
	keyboard_console_file_closed(t2);
	expect_modes(K_XLATE, KD_TEXT);
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF by t1");
	CHECK(kd(NULL, t2, KDSKBMODE, K_RAW) == 0, "K_RAW by t2");
	keyboard_console_file_closed(t2);
	expect_modes(K_OFF, KD_TEXT);
	keyboard_console_file_closed(t1);
	expect_modes(K_XLATE, KD_TEXT);
	kb_tap(SC_A);
	expect_typed("a");

	/* A display manager keeps the console in graphics mode for its whole
	 * graphical login.  The display servers it starts one after another
	 * set and restore the modes as always; the console stays out of sight
	 * between them, and also when a server started from a console login
	 * (which found KD_TEXT and puts it back) ends after the display
	 * manager has asked for graphics. */
	{
		uint64_t dm = keyboard_console_file_token();
		uint64_t x1 = keyboard_console_file_token();
		uint64_t x2 = keyboard_console_file_token();
		int takes = test_display_takes();
		uint8_t kb0;

		kb0 = (uint8_t)kd_kbdmode();
		redraws = test_console_redraws();
		CHECK(kd(NULL, dm, KDSETMODE, KD_GRAPHICS) == 0, "dm graphics");
		expect_modes(kb0, KD_GRAPHICS);
		CHECK(console_display_owned() &&
			      test_display_takes() == takes + 1,
		      "dm takes the display");
		/* X under the display manager: finds KD_GRAPHICS, switches the
		 * keyboard off, puts both back and closes. */
		CHECK(kd_dispmode() == KD_GRAPHICS, "x1 finds graphics");
		CHECK(kd(NULL, x1, KDSETMODE, KD_GRAPHICS) == 0, "x1 graphics");
		CHECK(kd(NULL, x1, KDSKBMODE, K_OFF) == 0, "x1 K_OFF");
		expect_modes(K_OFF, KD_GRAPHICS);
		CHECK(kd(NULL, x1, KDSETMODE, KD_GRAPHICS) == 0,
		      "x1 puts back graphics");
		CHECK(kd(NULL, x1, KDSKBMODE, kb0) == 0, "x1 puts back kbd");
		keyboard_console_file_closed(x1);
		expect_modes(kb0, KD_GRAPHICS);
		CHECK(console_display_owned() &&
			      test_console_redraws() == redraws,
		      "x1 gone: still held, nothing painted");
		/* X killed with the keyboard off: the keyboard comes back, the
		 * console stays out of sight. */
		CHECK(kd(NULL, x2, KDSETMODE, KD_GRAPHICS) == 0, "x2 graphics");
		CHECK(kd(NULL, x2, KDSKBMODE, K_OFF) == 0, "x2 K_OFF");
		keyboard_console_file_closed(x2);
		expect_modes(kb0, KD_GRAPHICS);
		CHECK(console_display_owned() &&
			      test_console_redraws() == redraws,
		      "x2 killed: still held, no repaint");
		/* The display manager gives the console back: one repaint,
		 * nothing left for its close. */
		CHECK(kd(NULL, dm, KDSETMODE, KD_TEXT) == 0, "dm text");
		expect_modes(kb0, KD_TEXT);
		CHECK(!console_display_owned() &&
			      test_console_redraws() == redraws + 1,
		      "released: one repaint");
		keyboard_console_file_closed(dm);
		CHECK(test_console_redraws() == redraws + 1,
		      "dm close: nothing left");

		/* startx from a console login: X finds KD_TEXT.  Signing out
		 * of it, the display manager asks for graphics first; X then
		 * puts back the KD_TEXT it found and exits -- the console
		 * stays out of sight until the display manager lets go. */
		redraws = test_console_redraws();
		CHECK(kd_dispmode() == KD_TEXT, "x1 finds text");
		CHECK(kd(NULL, x1, KDSETMODE, KD_GRAPHICS) == 0, "x1 graphics");
		CHECK(kd(NULL, x1, KDSKBMODE, K_OFF) == 0, "x1 K_OFF");
		CHECK(kd(NULL, dm, KDSETMODE, KD_GRAPHICS) == 0, "dm asks too");
		CHECK(kd(NULL, x1, KDSETMODE, KD_TEXT) == 0, "x1 text");
		CHECK(kd(NULL, x1, KDSKBMODE, kb0) == 0, "x1 kbd back");
		keyboard_console_file_closed(x1);
		expect_modes(kb0, KD_GRAPHICS);
		CHECK(console_display_owned() &&
			      test_console_redraws() == redraws,
		      "x1 out: the display manager still holds it");
		/* The display manager dies holding it: text again. */
		keyboard_console_file_closed(dm);
		expect_modes(kb0, KD_TEXT);
		CHECK(!console_display_owned() &&
			      test_console_redraws() == redraws + 1,
		      "dm gone: one repaint");
		kb_tap(SC_A);
		expect_typed("a");
	}

	/* A mode set without a file (stdio markers) owns nothing: a close
	 * does not undo it, and setting it back is the way out. */
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "K_OFF by t1");
	CHECK(kd(NULL, 0, KDSETMODE, KD_GRAPHICS) == 0, "graphics, no file");
	keyboard_console_file_closed(t1);
	expect_modes(K_XLATE, KD_GRAPHICS);
	CHECK(kd(NULL, 0, KDSKBMODE, K_OFF) == 0, "K_OFF, no file");
	keyboard_console_file_closed(t1);
	expect_modes(K_OFF, KD_GRAPHICS);
	CHECK(kd(NULL, 0, KDSETMODE, KD_TEXT) == 0, "text, no file");
	CHECK(kd(NULL, 0, KDSKBMODE, K_UNICODE) == 0, "unicode, no file");
	expect_modes(K_UNICODE, KD_TEXT);
	kb_tap(SC_A);
	expect_typed("a");

	/* K_RAW and K_MEDIUMRAW are not off: the console gets scancodes and
	 * key codes. */
	CHECK(kd(NULL, t3, KDSKBMODE, K_RAW) == 0, "K_RAW");
	kb_tap(SC_A);
	CHECK(test_console_take(junk, sizeof(junk)) == 2 &&
		      (uint8_t)junk[0] == SC_A && (uint8_t)junk[1] == 0x9E,
	      "K_RAW: scancodes");
	CHECK(kd(NULL, t3, KDSKBMODE, K_MEDIUMRAW) == 0, "K_MEDIUMRAW");
	kb_tap(SC_A);
	CHECK(test_console_take(junk, sizeof(junk)) == 2 &&
		      (uint8_t)junk[0] == KEY_A &&
		      (uint8_t)junk[1] == (KEY_A | 0x80),
	      "K_MEDIUMRAW: key codes");
	CHECK(kd(NULL, t3, KDSKBMODE, K_UNICODE) == 0, "K_UNICODE");
	kb_drain(kc);

	/* Permissions: an unprivileged task outside the console's session may
	 * read but not change; one in it may; the owning file may always
	 * change back (the display server drops root before it exits). */
	user = (task_t *)h_calloc(1, sizeof(task_t));
	CHECK(user != NULL, "task");
	test_set_capable(0);
	CHECK(kd(user, t1, KDSKBMODE, K_OFF) == -EPERM, "KDSKBMODE EPERM");
	CHECK(kd(user, t1, KDSETMODE, KD_GRAPHICS) == -EPERM,
	      "KDSETMODE EPERM");
	{
		int m = -1;

		CHECK(kd(user, t1, KDGKBMODE, (long)&m) == 0 && m == K_UNICODE,
		      "KDGKBMODE unprivileged");
		CHECK(kd(user, t1, KDGETMODE, (long)&m) == 0 && m == KD_TEXT,
		      "KDGETMODE unprivileged");
	}
	expect_modes(K_UNICODE, KD_TEXT);
	user->ctty = tty_get_console();
	CHECK(kd(user, t1, KDSETMODE, KD_GRAPHICS) == 0, "console session");
	CHECK(kd(user, t1, KDSETMODE, KD_TEXT) == 0, "console session back");
	user->ctty = NULL;
	test_set_capable(1);
	CHECK(kd(NULL, t1, KDSETMODE, KD_GRAPHICS) == 0, "root: graphics");
	CHECK(kd(NULL, t1, KDSKBMODE, K_OFF) == 0, "root: off");
	test_set_capable(0);
	CHECK(kd(user, t2, KDSKBMODE, K_UNICODE) == -EPERM,
	      "not the owner: EPERM");
	CHECK(kd(user, t1, KDSETMODE, KD_TEXT) == 0, "owner after the drop");
	CHECK(kd(user, t1, KDSKBMODE, K_UNICODE) == 0,
	      "owner after the drop: keyboard");
	CHECK(kd(user, t1, KDSKBMODE, K_OFF) == -EPERM,
	      "no longer the owner once back to normal");
	test_set_capable(1);
	expect_modes(K_UNICODE, KD_TEXT);
	h_free(user);

	/* The grab still keeps keys from the console by itself. */
	CHECK(ioc(kc, EVIOCGRAB, (void *)1) == 0, "grab");
	kb_tap(SC_B);
	expect_typed("");
	expect_stream("grabbed: event0", kc, { EV_KEY, KEY_B, 1 }, SYN,
		      { EV_KEY, KEY_B, 0 }, SYN);
	CHECK(ioc(kc, EVIOCGRAB, (void *)0) == 0, "ungrab");
	kb_tap(SC_B);
	expect_typed("b");
	kb_drain(kc);
	evdev_release(kc);

	/* Keys in one thread, mode changes and owner closes in another. */
	{
		struct kb_race r;
		unsigned long th;
		unsigned long long end = h_monotonic_ns() + 500000000ULL;
		long rounds = 0;

		h_memset(&r, 0, sizeof(r));
		h_thread_start(kb_race_keys, &r, &th);
		while (h_monotonic_ns() < end) {
			uint64_t t = keyboard_console_file_token();

			(void)kd(NULL, t, KDSETMODE, KD_GRAPHICS);
			(void)kd(NULL, t, KDSKBMODE, K_OFF);
			if (rounds & 1) {
				(void)kd(NULL, t, KDSKBMODE, K_UNICODE);
				(void)kd(NULL, t, KDSETMODE, KD_TEXT);
			}
			keyboard_console_file_closed(t);
			rounds++;
		}
		__atomic_store_n(&r.stop, 1, __ATOMIC_RELEASE);
		h_thread_join(th);
		CHECK(rounds > 0 && __atomic_load_n(&r.keys, __ATOMIC_RELAXED) > 0,
		      "race ran");
		expect_modes(K_UNICODE, KD_TEXT);
		CHECK(!console_display_owned(), "after the race: display free");
		keyboard_buffer_add(SC_LSHIFT | 0x80);
		keyboard_buffer_add(SC_LCTRL | 0x80);
		test_console_take(junk, sizeof(junk));
		/* Nothing stuck: a plain letter, in whichever Caps Lock state
		 * the race left (no Ctrl, no prefix). */
		kb_tap(SC_A);
		test_console_take(junk, sizeof(junk));
		CHECK(!h_strcmp(junk, "a") || !h_strcmp(junk, "A"),
		      "after the race: \"%s\"", junk);
		if (!h_strcmp(junk, "A"))
			kb_tap(SC_CAPS);
		kb_tap(SC_A);
		expect_typed("a");
	}
}

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : "all";
	int all = h_strcmp(only, "all") == 0;

	/* The kernel's built-in devices exist from the first look. */
	evdev_builtin_init();
	if (all || !h_strcmp(only, "builtin"))
		test_builtin();
	if (all || !h_strcmp(only, "filter"))
		test_filter();
	if (all || !h_strcmp(only, "mt"))
		test_mt();
	if (all || !h_strcmp(only, "ioctl"))
		test_ioctl();
	if (all || !h_strcmp(only, "clients"))
		test_clients();
	if (all || !h_strcmp(only, "proc"))
		test_proc();
	if (all || !h_strcmp(only, "lifecycle"))
		test_lifecycle();
	if (all || !h_strcmp(only, "race"))
		test_race();
	if (all || !h_strcmp(only, "handles"))
		test_handles();
	if (all || !h_strcmp(only, "fidelity"))
		test_fidelity();
	if (all || !h_strcmp(only, "keyboard")) {
		test_output_gate_machine();
		test_keyboard();
	}
	CHECK(test_warning_count() == 0, "%ld kernel warnings",
	      test_warning_count());
	test_thread_done();
	h_printf("%s: %ld checks, %ld failed\n", g_fails ? "FAIL" : "PASS",
		 g_checks, g_fails);
	return g_fails ? 1 : 0;
}
