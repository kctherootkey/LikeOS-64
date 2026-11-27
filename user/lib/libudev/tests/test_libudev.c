// libudev host tests: classification, devices, attributes, enumeration and
// the monitor, against fixture copies of real /proc/bus/input/devices
// output (kernel/spec) and LikeOS-format descriptions.
//
//   test_libudev <fixture directory>
//
// Built with -DLUDEV_TESTING, which lets the library read the fixtures in
// place of /proc, /dev/input, /dev/devevents and /sys.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/devevent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "ludev.h"

static int g_fail, g_pass;
static const char *g_dir;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (cond) {                                                    \
			g_pass++;                                              \
		} else {                                                       \
			g_fail++;                                              \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static char g_path[4][1024];

static const char *fixture(int slot, const char *name)
{
	snprintf(g_path[slot], sizeof(g_path[slot]), "%s/%s", g_dir, name);
	return g_path[slot];
}

static void use_proc(const char *name)
{
	ludev_test_set_paths(fixture(0, name), NULL, NULL, NULL);
}

static bool streq_or_null(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	return strcmp(a, b) == 0;
}

static const char *prop(struct udev_device *d, const char *k)
{
	return udev_device_get_property_value(d, k);
}

// The ID_INPUT_* flags of /dev/input/eventN as properties, as a string like
// "KEY,KEYBOARD" for readable failures.
static void id_string(struct udev_device *d, char *buf, size_t size)
{
	static const char *const names[] = {
		"ACCELEROMETER", "POINTINGSTICK", "MOUSE", "TOUCHPAD",
		"TOUCHSCREEN",   "JOYSTICK",      "TABLET", "TABLET_PAD",
		"KEY",           "KEYBOARD",      "SWITCH",
	};
	size_t n = 0;

	buf[0] = '\0';
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char k[64];
		const char *v;

		snprintf(k, sizeof(k), "ID_INPUT_%s", names[i]);
		v = prop(d, k);
		if (v && strcmp(v, "1") == 0) {
			int w = snprintf(buf + n, size - n, "%s%s", n ? "," : "",
					 names[i]);
			if (w > 0 && (size_t)w < size - n)
				n += (size_t)w;
		}
	}
}

static void expect_class(struct udev *u, int event, const char *want)
{
	char sp[64], got[256];
	struct udev_device *d;

	snprintf(sp, sizeof(sp), "/sys/class/input/event%d", event);
	d = udev_device_new_from_syspath(u, sp);
	CHECK(d != NULL, "event%d exists", event);
	if (!d)
		return;
	CHECK(streq_or_null(prop(d, "ID_INPUT"), "1"), "event%d ID_INPUT=1", event);
	id_string(d, got, sizeof(got));
	CHECK(strcmp(got, want) == 0, "event%d: got [%s] want [%s]", event, got, want);
	// The input device above it carries the same tags.
	{
		struct udev_device *p = udev_device_get_parent(d);
		char pgot[256];

		CHECK(p != NULL, "event%d has a parent", event);
		if (p) {
			id_string(p, pgot, sizeof(pgot));
			CHECK(strcmp(pgot, want) == 0, "input of event%d: [%s]", event,
			      pgot);
		}
	}
	udev_device_unref(d);
}

// ---------------------------------------------------------------------------

static void test_classification(void)
{
	struct udev *u = udev_new();

	use_proc("hp-probook-650-g8.devices");
	expect_class(u, 0, "KEY");                 // Sleep Button
	expect_class(u, 1, "SWITCH");              // Lid Switch
	expect_class(u, 2, "KEY");                 // Power Button
	expect_class(u, 4, "KEY,KEYBOARD");        // AT keyboard
	expect_class(u, 5, "POINTINGSTICK,MOUSE"); // ELAN Mouse (i2c)
	expect_class(u, 6, "TOUCHPAD");            // ELAN Touchpad
	expect_class(u, 7, "KEY");                 // Wireless hotkeys
	expect_class(u, 8, "KEY");                 // Intel HID events
	expect_class(u, 9, "KEY,SWITCH");          // HP WMI hotkeys
	expect_class(u, 10, "KEY");                // Video Bus
	expect_class(u, 11, "SWITCH");             // jack

	use_proc("dell-pro16-plus.devices");
	expect_class(u, 0, "SWITCH");              // Lid (no I: line)
	expect_class(u, 3, "KEY,KEYBOARD");
	expect_class(u, 4, "POINTINGSTICK,MOUSE"); // VEN_0488 Mouse
	expect_class(u, 5, "TOUCHPAD");
	expect_class(u, 6, "MOUSE");               // PS/2 Generic Mouse
	expect_class(u, 8, "KEY");                 // 5 button array
	expect_class(u, 12, "KEY,SWITCH");         // soundwire jack

	use_proc("likeos-hp-probook.devices");
	expect_class(u, 0, "KEY,KEYBOARD");
	expect_class(u, 1, "MOUSE");
	expect_class(u, 2, "POINTINGSTICK,MOUSE");
	expect_class(u, 3, "TOUCHPAD");

	use_proc("thinkpad-p50.devices");
	expect_class(u, 3, "KEY,KEYBOARD");
	expect_class(u, 5, "KEY,SWITCH");
	expect_class(u, 6, "TOUCHPAD");
	expect_class(u, 7, "POINTINGSTICK,MOUSE");

	// A virtual machine's absolute pointer is a mouse, not a touchpad.
	use_proc("vm-absolute-mouse.devices");
	expect_class(u, 0, "KEY,KEYBOARD");
	expect_class(u, 1, "MOUSE");

	udev_unref(u);
}

// Synthetic bitmaps for the branches the fixtures do not reach.
static void test_classify_synthetic(void)
{
	struct ludev_input in;
	unsigned int f;

#define SET(cap, code) \
	(in.caps[LUDEV_CAP_##cap].w[(code) / LUDEV_LONG_BITS] |= 1ul << ((code) % LUDEV_LONG_BITS))

	// Touchscreen: direct, MT, BTN_TOUCH.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_KEY); SET(EV, EV_ABS); SET(PROP, INPUT_PROP_DIRECT);
	SET(KEY, BTN_TOUCH); SET(ABS, ABS_X); SET(ABS, ABS_Y);
	SET(ABS, ABS_MT_POSITION_X); SET(ABS, ABS_MT_POSITION_Y);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_TOUCHSCREEN), "touchscreen %#x", f);

	// Tablet: pen.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_KEY); SET(EV, EV_ABS); SET(KEY, BTN_TOOL_PEN); SET(KEY, BTN_TOUCH);
	SET(ABS, ABS_X); SET(ABS, ABS_Y);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_TABLET), "tablet %#x", f);

	// Gamepad: sticks and face buttons.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_KEY); SET(EV, EV_ABS); SET(KEY, BTN_A); SET(KEY, BTN_B);
	SET(ABS, ABS_X); SET(ABS, ABS_Y); SET(ABS, ABS_RX); SET(ABS, ABS_RY);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_JOYSTICK), "joystick %#x", f);

	// A keyboard with one stray joystick button is no joystick.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_KEY);
	for (unsigned int k = KEY_ESC; k <= KEY_D; k++)
		SET(KEY, k);
	SET(KEY, KEY_LEFTCTRL); SET(KEY, KEY_CAPSLOCK); SET(KEY, KEY_NUMLOCK);
	SET(KEY, KEY_INSERT); SET(KEY, BTN_TRIGGER);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_KEY | LUDEV_ID_KEYBOARD),
	      "keyboard with joystick button %#x", f);

	// Accelerometer: three absolute axes, no keys.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_ABS); SET(ABS, ABS_X); SET(ABS, ABS_Y); SET(ABS, ABS_Z);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_ACCELEROMETER), "accelerometer %#x", f);

	// Wheel-only device: keys.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_REL); SET(REL, REL_WHEEL);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_KEY), "wheel only %#x", f);

	// A touchpad with a pen tool is a tablet, not a touchpad.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_KEY); SET(EV, EV_ABS); SET(KEY, BTN_TOOL_FINGER);
	SET(KEY, BTN_TOOL_PEN); SET(ABS, ABS_X); SET(ABS, ABS_Y);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_TABLET), "finger+pen %#x", f);

	// High key block only (KEY_OK and up): keys.
	memset(&in, 0, sizeof(in));
	SET(EV, EV_KEY); SET(KEY, KEY_OK);
	f = ludev_input_classify(&in);
	CHECK(f == (LUDEV_ID_INPUT | LUDEV_ID_KEY), "KEY_OK %#x", f);

	// Nothing at all: only ID_INPUT.
	memset(&in, 0, sizeof(in));
	f = ludev_input_classify(&in);
	CHECK(f == LUDEV_ID_INPUT, "empty %#x", f);
#undef SET
}

static void test_device_properties(void)
{
	struct udev *u = udev_new();
	struct udev_device *d, *p, *p2;
	struct udev_list_entry *e;
	int n;

	use_proc("hp-probook-650-g8.devices");
	d = udev_device_new_from_syspath(u, "/sys/class/input/event6");
	CHECK(d != NULL, "touchpad device");
	if (!d)
		goto out;
	CHECK(streq_or_null(udev_device_get_sysname(d), "event6"), "sysname");
	CHECK(streq_or_null(udev_device_get_sysnum(d), "6"), "sysnum");
	CHECK(streq_or_null(udev_device_get_subsystem(d), "input"), "subsystem");
	CHECK(streq_or_null(udev_device_get_devnode(d), "/dev/input/event6"), "devnode");
	CHECK(streq_or_null(udev_device_get_syspath(d),
			    "/sys/devices/pci0000:00/0000:00:15.0/i2c_designware.0/"
			    "i2c-0/i2c-ELAN073B:00/0018:04F3:3169.0001/input/input10/event6"),
	      "syspath %s", udev_device_get_syspath(d));
	CHECK(major(udev_device_get_devnum(d)) == 13 &&
		      minor(udev_device_get_devnum(d)) == 70,
	      "devnum 13:70");
	CHECK(udev_device_get_is_initialized(d) == 1, "initialized");
	CHECK(udev_device_get_action(d) == NULL, "no action outside a monitor");
	CHECK(streq_or_null(prop(d, "DEVNAME"), "/dev/input/event6"), "DEVNAME");
	CHECK(streq_or_null(prop(d, "MAJOR"), "13") && streq_or_null(prop(d, "MINOR"), "70"),
	      "MAJOR/MINOR");
	CHECK(streq_or_null(prop(d, "ID_BUS"), "i2c"), "ID_BUS");
	CHECK(streq_or_null(prop(d, "ID_VENDOR_ID"), "04f3"), "ID_VENDOR_ID");
	CHECK(streq_or_null(prop(d, "ID_MODEL_ID"), "3169"), "ID_MODEL_ID");
	CHECK(streq_or_null(prop(d, "LIBINPUT_DEVICE_GROUP"), "18/4f3/3169:i2c-ELAN073B:00"),
	      "LIBINPUT_DEVICE_GROUP %s", prop(d, "LIBINPUT_DEVICE_GROUP"));
	// No node in the fixtures: no size.
	CHECK(prop(d, "ID_INPUT_WIDTH_MM") == NULL, "no width without a node");
	CHECK(prop(d, "NO_SUCH") == NULL && errno == ENOENT, "missing property");
	CHECK(udev_device_get_property_value(d, NULL) == NULL, "NULL key");
	CHECK(streq_or_null(udev_device_get_sysattr_value(d, "dev"), "13:70"), "dev attr");
	CHECK(udev_device_get_sysattr_value(d, "name") == NULL, "event node has no name attr");

	n = 0;
	udev_list_entry_foreach(e, udev_device_get_properties_list_entry(d))
		n++;
	CHECK(n >= 8, "property list (%d)", n);
	CHECK(udev_list_entry_get_by_name(udev_device_get_properties_list_entry(d),
					  "ID_INPUT_TOUCHPAD") != NULL,
	      "list lookup by name");

	p = udev_device_get_parent(d);
	CHECK(p != NULL, "parent");
	if (p) {
		CHECK(streq_or_null(udev_device_get_sysname(p), "input10"), "parent sysname");
		CHECK(udev_device_get_devnode(p) == NULL, "parent has no node");
		CHECK(streq_or_null(prop(p, "PRODUCT"), "18/4f3/3169/100"), "PRODUCT %s",
		      prop(p, "PRODUCT"));
		CHECK(streq_or_null(prop(p, "NAME"), "\"ELAN073B:00 04F3:3169 Touchpad\""),
		      "NAME");
		CHECK(streq_or_null(prop(p, "PHYS"), "\"i2c-ELAN073B:00\""), "PHYS");
		CHECK(streq_or_null(prop(p, "UNIQ"), "\"\""), "UNIQ");
		CHECK(streq_or_null(prop(p, "PROP"), "5"), "PROP");
		CHECK(streq_or_null(prop(p, "KEY"), "e520 10000 0 0 0 0"), "KEY");
		CHECK(streq_or_null(prop(p, "ABS"), "2e0800000000003"), "ABS");
		CHECK(prop(p, "REL") == NULL, "no REL");
		CHECK(streq_or_null(udev_device_get_sysattr_value(p, "name"),
				    "ELAN073B:00 04F3:3169 Touchpad"),
		      "name attr");
		CHECK(streq_or_null(udev_device_get_sysattr_value(p, "id/vendor"), "04f3"),
		      "id/vendor");
		CHECK(streq_or_null(udev_device_get_sysattr_value(p, "id/bustype"), "0018"),
		      "id/bustype");
		CHECK(streq_or_null(udev_device_get_sysattr_value(p, "capabilities/rel"), "0"),
		      "empty capability reads 0");
		CHECK(streq_or_null(udev_device_get_sysattr_value(p, "properties"), "5"),
		      "properties attr");
		CHECK(udev_device_get_sysattr_value(p, "bogus") == NULL && errno == ENOENT,
		      "unknown attr");
		// Repeated lookups return the same storage.
		CHECK(udev_device_get_sysattr_value(p, "name") ==
			      udev_device_get_sysattr_value(p, "name"),
		      "attr cached");
		CHECK(udev_device_get_parent(p) == NULL, "input device is the top");
		p2 = udev_device_get_parent_with_subsystem_devtype(d, "input", NULL);
		CHECK(p2 == p, "parent with subsystem input");
		CHECK(udev_device_get_parent_with_subsystem_devtype(d, "hid", NULL) == NULL,
		      "no hid parent");
		n = 0;
		udev_list_entry_foreach(e, udev_device_get_sysattr_list_entry(p))
			n++;
		CHECK(n == 17, "input attrs listed (%d)", n);

		// Keep the parent beyond the child, as libinput's device group
		// helper does.
		udev_device_ref(p);
		udev_device_unref(d);
		d = NULL;
		CHECK(streq_or_null(udev_device_get_sysname(p), "input10"),
		      "parent outlives child");
		udev_device_unref(p);
	}
	if (d)
		udev_device_unref(d);

	// The same device by its real syspath, and by subsystem + sysname.
	d = udev_device_new_from_syspath(
		u, "/sys/devices/pci0000:00/0000:00:15.0/i2c_designware.0/i2c-0/"
		   "i2c-ELAN073B:00/0018:04F3:3169.0001/input/input10/event6/");
	CHECK(d && streq_or_null(udev_device_get_sysname(d), "event6"),
	      "by syspath (trailing slash)");
	udev_device_unref(d);
	d = udev_device_new_from_syspath(
		u, "/sys/devices/pci0000:00/0000:00:15.0/i2c_designware.0/i2c-0/"
		   "i2c-ELAN073B:00/0018:04F3:3169.0001/input/input10");
	CHECK(d && streq_or_null(udev_device_get_sysname(d), "input10"), "input by syspath");
	udev_device_unref(d);
	d = udev_device_new_from_subsystem_sysname(u, "input", "event4");
	CHECK(d && streq_or_null(prop(d, "ID_INPUT_KEYBOARD"), "1"), "subsystem+sysname");
	udev_device_unref(d);
	d = udev_device_new_from_subsystem_sysname(u, "input", "input4");
	CHECK(d && streq_or_null(udev_device_get_sysname(d), "input4"), "inputM by name");
	udev_device_unref(d);
	d = udev_device_new_from_subsystem_sysname(u, "input", "event99");
	CHECK(d == NULL, "missing event node");
	d = udev_device_new_from_subsystem_sysname(u, "input", "../event4");
	CHECK(d == NULL && errno == EINVAL, "sysname with a slash");

	// Bad syspaths.
	CHECK(udev_device_new_from_syspath(u, "/etc/passwd") == NULL && errno == EINVAL,
	      "outside /sys");
	CHECK(udev_device_new_from_syspath(u, "/sys/../etc") == NULL && errno == EINVAL,
	      "dot-dot");
	CHECK(udev_device_new_from_syspath(u, "/sys/class//input") == NULL, "empty component");
	CHECK(udev_device_new_from_syspath(u, "relative") == NULL, "relative");
	CHECK(udev_device_new_from_syspath(NULL, "/sys/class/input/event6") == NULL,
	      "NULL udev");
	CHECK(udev_device_new_from_syspath(u, NULL) == NULL, "NULL syspath");
	{
		char *longp = malloc(LUDEV_PATH_MAX + 16);

		memset(longp, 'a', LUDEV_PATH_MAX + 15);
		memcpy(longp, "/sys/", 5);
		longp[LUDEV_PATH_MAX + 15] = '\0';
		CHECK(udev_device_new_from_syspath(u, longp) == NULL, "overlong syspath");
		free(longp);
	}

	// NULL safety of the getters.
	CHECK(udev_device_get_syspath(NULL) == NULL, "NULL getter");
	CHECK(udev_device_unref(NULL) == NULL, "unref NULL");
	CHECK(udev_device_get_parent(NULL) == NULL, "parent of NULL");
	CHECK(udev_device_get_sysattr_value(NULL, "x") == NULL, "attr of NULL");
	CHECK(udev_list_entry_get_next(NULL) == NULL && udev_list_entry_get_name(NULL) == NULL,
	      "NULL list");

out:
	udev_unref(u);
}

static void test_devnum(void)
{
	struct udev *u = udev_new();
	struct udev_device *d;
	char dir[256], f[300];
	int fd;

	snprintf(dir, sizeof(dir), "/tmp/libudev-test-dev.%d", (int)getpid());
	mkdir(dir, 0700);
	snprintf(f, sizeof(f), "%s/event3", dir);
	fd = open(f, O_CREAT | O_WRONLY, 0600);
	if (fd >= 0)
		close(fd);
	snprintf(f, sizeof(f), "%s/mouse0", dir);
	fd = open(f, O_CREAT | O_WRONLY, 0600);
	if (fd >= 0)
		close(fd);

	use_proc("likeos-hp-probook.devices");
	ludev_test_set_paths(NULL, dir, NULL, NULL);
	ludev_test_any_node_type = true;

	// Building, enumerating and reading other properties never opens the
	// node: only asking for the size does (once).
	ludev_test_size_opens = 0;
	d = udev_device_new_from_devnum(u, 'c', makedev(13, 67));
	CHECK(d && streq_or_null(udev_device_get_sysname(d), "event3") &&
		      streq_or_null(prop(d, "ID_INPUT_TOUCHPAD"), "1"),
	      "devnum 13:67 -> touchpad");
	CHECK(ludev_test_size_opens == 0, "node not opened for other properties (%u)",
	      ludev_test_size_opens);
	{
		struct udev_enumerate *en = udev_enumerate_new(u);
		int found = 0;

		CHECK(en && udev_enumerate_add_match_subsystem(en, "input") == 0 &&
			      udev_enumerate_add_match_sysname(en, "event*") == 0 &&
			      udev_enumerate_add_match_property(en, "ID_INPUT_TOUCHPAD", "1") == 0 &&
			      udev_enumerate_scan_devices(en) == 0,
		      "touchpad enumeration");
		for (struct udev_list_entry *l = en ? udev_enumerate_get_list_entry(en) : NULL;
		     l; l = udev_list_entry_get_next(l))
			found++;
		CHECK(found == 1, "one touchpad enumerated (%d)", found);
		CHECK(ludev_test_size_opens == 0, "enumeration opens no node (%u)",
		      ludev_test_size_opens);
		udev_enumerate_unref(en);
	}
	// The node exists but cannot be opened for its axes: no size.
	CHECK(d && prop(d, "ID_INPUT_WIDTH_MM") == NULL, "no width from a plain file");
	CHECK(ludev_test_size_opens == 1, "the size opens the node (%u)",
	      ludev_test_size_opens);
	CHECK(d && prop(d, "ID_INPUT_HEIGHT_MM") == NULL &&
		      udev_device_get_properties_list_entry(d) != NULL &&
		      ludev_test_size_opens == 1,
	      "and only once (%u)", ludev_test_size_opens);
	udev_device_unref(d);
	// The whole list includes the size, so it opens the node.
	d = udev_device_new_from_devnum(u, 'c', makedev(13, 67));
	CHECK(d && udev_device_get_properties_list_entry(d) != NULL &&
		      ludev_test_size_opens == 2,
	      "property list resolves the size (%u)", ludev_test_size_opens);
	udev_device_unref(d);
	CHECK(udev_device_new_from_devnum(u, 'c', makedev(13, 99)) == NULL && errno == ENOENT,
	      "unknown devnum");
	CHECK(udev_device_new_from_devnum(u, 'b', makedev(8, 0)) == NULL, "block devnum");
	CHECK(udev_device_new_from_devnum(u, 'x', makedev(13, 67)) == NULL && errno == EINVAL,
	      "bad type");

	ludev_test_any_node_type = false;
	ludev_test_set_paths(NULL, "/nonexistent-libudev-test", NULL, NULL);
	snprintf(f, sizeof(f), "%s/event3", dir);
	unlink(f);
	snprintf(f, sizeof(f), "%s/mouse0", dir);
	unlink(f);
	rmdir(dir);
	udev_unref(u);
}

static int count_list(struct udev_list_entry *e)
{
	int n = 0;

	for (; e; e = udev_list_entry_get_next(e))
		n++;
	return n;
}

static void test_enumerate(void)
{
	struct udev *u = udev_new();
	struct udev_enumerate *e;
	struct udev_list_entry *l;

	use_proc("hp-probook-650-g8.devices");
	e = udev_enumerate_new(u);
	CHECK(udev_enumerate_add_match_subsystem(e, "input") == 0, "match subsystem");
	CHECK(udev_enumerate_scan_devices(e) == 0, "scan");
	// 16 input devices, each with an event node.
	CHECK(count_list(udev_enumerate_get_list_entry(e)) == 32, "all (%d)",
	      count_list(udev_enumerate_get_list_entry(e)));
	udev_enumerate_unref(e);

	e = udev_enumerate_new(u);
	udev_enumerate_add_match_subsystem(e, "input");
	udev_enumerate_add_match_sysname(e, "event*");
	udev_enumerate_add_match_property(e, "ID_INPUT_TOUCHPAD", "1");
	udev_enumerate_add_match_is_initialized(e);
	CHECK(udev_enumerate_scan_devices(e) == 0, "scan touchpads");
	l = udev_enumerate_get_list_entry(e);
	CHECK(count_list(l) == 1 && strstr(udev_list_entry_get_name(l), "/event6"),
	      "only the touchpad node");
	udev_enumerate_unref(e);

	e = udev_enumerate_new(u);
	udev_enumerate_add_match_subsystem(e, "block");
	CHECK(udev_enumerate_scan_devices(e) == 0 &&
		      udev_enumerate_get_list_entry(e) == NULL,
	      "other subsystems enumerate nothing");
	udev_enumerate_unref(e);

	e = udev_enumerate_new(u);
	udev_enumerate_add_nomatch_subsystem(e, "input");
	CHECK(udev_enumerate_scan_devices(e) == 0 &&
		      udev_enumerate_get_list_entry(e) == NULL,
	      "nomatch input");
	udev_enumerate_unref(e);

	// Rescan replaces, ref/unref balance.
	e = udev_enumerate_new(u);
	udev_enumerate_add_match_sysname(e, "input1?");
	udev_enumerate_scan_devices(e);
	udev_enumerate_scan_devices(e);
	CHECK(count_list(udev_enumerate_get_list_entry(e)) == 10, "input1? twice (%d)",
	      count_list(udev_enumerate_get_list_entry(e)));
	udev_enumerate_ref(e);
	udev_enumerate_unref(e);
	CHECK(udev_enumerate_get_udev(e) == u, "still alive");
	udev_enumerate_unref(e);

	// A missing /proc file is an error, not an empty list.
	use_proc("does-not-exist.devices");
	e = udev_enumerate_new(u);
	CHECK(udev_enumerate_scan_devices(e) == -ENOENT, "missing proc file");
	udev_enumerate_unref(e);
	CHECK(udev_enumerate_new(NULL) == NULL, "enumerate NULL udev");
	udev_unref(u);
}

static void test_malformed(void)
{
	struct udev *u = udev_new();
	struct udev_device *d;
	struct ludev_input_table t;
	char *text;
	size_t len;

	use_proc("malformed.devices");
	CHECK(ludev_input_load(&t) == 0, "malformed file parses");
	CHECK(t.n == 2, "two records (%zu)", t.n);
	if (t.n == 2) {
		CHECK(t.v[0].bustype == 0 && t.v[0].product == 0x5678, "fields clipped");
		CHECK(strcmp(t.v[0].name, "unterminated") == 0, "unterminated name [%s]",
		      t.v[0].name);
		CHECK(t.v[0].event == 7, "event7 from the handler list");
		CHECK(t.v[0].sysfs[0] == '/', "a usable sysfs path");
		CHECK(!ludev_test_bit(&t.v[0], LUDEV_CAP_KEY, 0) &&
			      !ludev_test_bit(&t.v[0], LUDEV_CAP_ABS, 0) &&
			      !ludev_test_bit(&t.v[0], LUDEV_CAP_REL, 0),
		      "bad bitmaps are empty");
		CHECK(ludev_test_bit(&t.v[0], LUDEV_CAP_EV, EV_REL), "good bitmap kept");
		CHECK(strcmp(t.v[1].name, "quoted \"inner\" name") == 0, "inner quotes [%s]",
		      t.v[1].name);
	}
	ludev_input_table_free(&t);
	d = udev_device_new_from_subsystem_sysname(u, "input", "event9");
	CHECK(d && streq_or_null(prop(d, "ID_INPUT_KEY"), "1"), "malformed record 2 usable");
	udev_device_unref(d);

	// Random input: no crash, no leak (ASan).
	srand(12345);
	for (int it = 0; it < 20000; it++) {
		static const char alphabet[] = "IBNPSUH:= \n\"0123456789abcdefxevent";
		char buf[512];
		size_t n = (size_t)(rand() % (int)sizeof(buf));

		for (size_t i = 0; i < n; i++)
			buf[i] = (rand() % 4) ? alphabet[rand() % (int)(sizeof(alphabet) - 1)] :
						(char)rand();
		if (ludev_input_parse(buf, n, &t) == 0) {
			for (size_t i = 0; i < t.n; i++)
				(void)ludev_input_classify(&t.v[i]);
			ludev_input_table_free(&t);
		}
	}
	// Every fixture prefix parses.
	if (ludev_read_file(fixture(1, "hp-probook-650-g8.devices"), &text, &len, 1 << 20) == 0) {
		for (size_t cut = 0; cut <= len; cut += 7) {
			if (ludev_input_parse(text, cut, &t) == 0)
				ludev_input_table_free(&t);
		}
		free(text);
	}
	udev_unref(u);
}

static void test_sysfs(void)
{
	struct udev *u = udev_new();
	struct udev_device *d;

	ludev_test_set_paths(NULL, NULL, NULL, fixture(2, "sys"));
	d = udev_device_new_from_syspath(u, "/sys/devices/virtual/dmi/id");
	CHECK(d != NULL, "dmi device");
	if (d) {
		const char *m = prop(d, "MODALIAS");

		CHECK(m && strncmp(m, "dmi:bvnHP:", 10) == 0, "MODALIAS");
		CHECK(streq_or_null(udev_device_get_subsystem(d), "dmi"), "subsystem dmi");
		CHECK(streq_or_null(udev_device_get_sysname(d), "id"), "sysname id");
		CHECK(streq_or_null(udev_device_get_sysattr_value(d, "sys_vendor"), "HP"),
		      "sys_vendor");
		CHECK(udev_device_get_sysattr_value(d, "../../../../../etc/passwd") == NULL &&
			      errno == EINVAL,
		      "attr escaping the device");
		CHECK(udev_device_get_sysattr_value(d, "missing") == NULL, "missing attr");
		CHECK(count_list(udev_device_get_sysattr_list_entry(d)) == 2,
		      "two attribute files (%d)",
		      count_list(udev_device_get_sysattr_list_entry(d)));
		CHECK(udev_device_get_parent(d) == NULL, "no device above");
		udev_device_unref(d);
	}
	CHECK(udev_device_new_from_syspath(u, "/sys/devices/virtual/dmi/nothere") == NULL,
	      "missing directory");
	CHECK(udev_device_new_from_syspath(u, "/sys/devices/virtual/dmi/id/uevent") == NULL &&
		      errno == ENODEV,
	      "a file is no device");
	ludev_test_set_paths(NULL, NULL, NULL, "/nonexistent-libudev-sys");
	udev_unref(u);
}

static void write_record(int fd, uint32_t type, uint32_t cls, const char *name,
			 uint64_t seq)
{
	struct device_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.size = sizeof(ev);
	ev.version = DEVEVENT_VERSION;
	ev.type = type;
	ev.device_class = cls;
	ev.seq = seq;
	snprintf(ev.name, sizeof(ev.name), "%s", name);
	if (cls == DEVICE_CLASS_INPUT) {
		ev.major = 13;
		ev.minor = 64 + (uint32_t)ludev_parse_event_name(name);
	}
	if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		perror("write record");
}

static void test_monitor(void)
{
	struct udev *u = udev_new();
	struct udev_monitor *m;
	struct udev_device *d;
	char file[256], add_syspath[1024] = "";
	int fd;

	snprintf(file, sizeof(file), "/tmp/libudev-test-events.%d", (int)getpid());
	fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	CHECK(fd >= 0, "record file");
	if (fd < 0)
		goto out;
	write_record(fd, DEVICE_EVENT_ADD, DEVICE_CLASS_BLOCK_DISK, "sdb", 1);
	write_record(fd, DEVICE_EVENT_ADD, DEVICE_CLASS_INPUT, "event3", 2);
	write_record(fd, DEVICE_EVENT_ADD, DEVICE_CLASS_INPUT, "event42", 3); // gone
	write_record(fd, DEVICE_EVENT_REMOVE, DEVICE_CLASS_INPUT, "event3", 4);
	write_record(fd, DEVICE_EVENT_REMOVE, DEVICE_CLASS_INPUT, "event2", 5);
	write_record(fd, DEVICE_EVENT_REMOVE, DEVICE_CLASS_INPUT, "event17", 6);
	write_record(fd, DEVICE_EVENT_OVERFLOW, 0, "", 7);
	close(fd);

	use_proc("likeos-hp-probook.devices");
	ludev_test_set_paths(NULL, NULL, file, NULL);
	m = udev_monitor_new_from_netlink(u, "udev");
	CHECK(m != NULL, "monitor");
	if (!m)
		goto out;
	CHECK(udev_monitor_filter_add_match_subsystem_devtype(m, "input", NULL) == 0,
	      "filter");
	CHECK(udev_monitor_enable_receiving(m) == 0, "enable");
	CHECK(udev_monitor_get_fd(m) >= 0, "fd");
	CHECK(udev_monitor_set_receive_buffer_size(m, 128 * 1024) == 0, "buffer size");

	d = udev_monitor_receive_device(m);
	CHECK(d && streq_or_null(udev_device_get_action(d), "add") &&
		      streq_or_null(udev_device_get_sysname(d), "event3") &&
		      streq_or_null(prop(d, "ID_INPUT_TOUCHPAD"), "1") &&
		      udev_device_get_seqnum(d) == 2,
	      "add event3 (disk record skipped)");
	if (d)
		snprintf(add_syspath, sizeof(add_syspath), "%s", udev_device_get_syspath(d));
	udev_device_unref(d);

	d = udev_monitor_receive_device(m);
	CHECK(d && streq_or_null(udev_device_get_action(d), "remove") &&
		      streq_or_null(udev_device_get_syspath(d), add_syspath) &&
		      streq_or_null(udev_device_get_devnode(d), "/dev/input/event3") &&
		      minor(udev_device_get_devnum(d)) == 67,
	      "remove event3 with the syspath of its add (event42 skipped)");
	CHECK(d && udev_device_get_parent(d) == NULL, "removed device has no parent");
	udev_device_unref(d);

	// event2 existed before the monitor was made: its syspath is known.
	d = udev_monitor_receive_device(m);
	CHECK(d && streq_or_null(udev_device_get_action(d), "remove") &&
		      strstr(udev_device_get_syspath(d), "/input/input2/event2"),
	      "remove of a device known from the start: %s",
	      d ? udev_device_get_syspath(d) : "(null)");
	udev_device_unref(d);

	d = udev_monitor_receive_device(m);
	CHECK(d && streq_or_null(udev_device_get_syspath(d), "/sys/class/input/event17"),
	      "remove of an unknown node");
	udev_device_unref(d);

	// Overflow: the table now knows event0, event1 (event2/3 were removed);
	// the HP description has event0..event15 -- adds for the new nodes and
	// a remove for none, and changed identities show as remove + add.
	use_proc("hp-probook-650-g8.devices");
	{
		int adds = 0, removes = 0;

		while ((d = udev_monitor_receive_device(m)) != NULL) {
			if (streq_or_null(udev_device_get_action(d), "add"))
				adds++;
			else if (streq_or_null(udev_device_get_action(d), "remove"))
				removes++;
			udev_device_unref(d);
		}
		CHECK(errno == EAGAIN, "drained");
		// event0 and event1 changed identity (LikeOS keyboard/mouse ->
		// Sleep Button/Lid Switch): 2 removes; 16 adds.
		CHECK(removes == 2 && adds == 16, "resync: %d removes, %d adds", removes, adds);
	}

	udev_monitor_ref(m);
	udev_monitor_unref(m);
	CHECK(udev_monitor_get_udev(m) == u, "monitor alive after ref/unref");
	CHECK(udev_monitor_filter_add_match_tag(m, "seat") == 0, "tag filter");
	CHECK(udev_monitor_filter_update(m) == 0, "update");
	CHECK(udev_monitor_receive_device(m) == NULL, "tag filter receives nothing");
	CHECK(udev_monitor_filter_remove(m) == 0, "remove filters");
	udev_monitor_unref(m);

	// No access to the event file: an idle monitor, still usable.
	ludev_test_set_paths(NULL, NULL, "/nonexistent-libudev-devevents", NULL);
	m = udev_monitor_new_from_netlink(u, "udev");
	CHECK(m && udev_monitor_get_fd(m) >= 0 && udev_monitor_receive_device(m) == NULL,
	      "idle monitor");
	udev_monitor_unref(m);
	CHECK(udev_monitor_new_from_netlink(u, "bogus") == NULL && errno == EINVAL,
	      "bad monitor name");
	CHECK(udev_monitor_new_from_netlink(NULL, "udev") == NULL, "monitor NULL udev");
	unlink(file);
out:
	udev_unref(u);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <fixture directory>\n", argv[0]);
		return 2;
	}
	g_dir = argv[1];
	ludev_test_set_paths(NULL, "/nonexistent-libudev-test",
			     "/nonexistent-libudev-devevents", "/nonexistent-libudev-sys");

	test_classification();
	test_classify_synthetic();
	test_device_properties();
	test_devnum();
	test_enumerate();
	test_malformed();
	test_sysfs();
	test_monitor();

	printf("libudev host tests: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
