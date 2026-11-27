// libudev - input devices: /proc/bus/input/devices and their classification.
//
// The kernel describes every input device in /proc/bus/input/devices, one
// block per device:
//
//   I: Bus=0018 Vendor=04f3 Product=3169 Version=0100
//   N: Name="ELAN073B:00 04F3:3169 Touchpad"
//   P: Phys=i2c-ELAN073B:00
//   S: Sysfs=/devices/pci0000:00/.../input/input10
//   U: Uniq=
//   H: Handlers=event6
//   B: PROP=5
//   B: EV=1b
//   B: KEY=e520 10000 0 0 0 0
//   B: ABS=2e0800000000003
//   B: MSC=20
//
// A bitmap line lists the words of the bitmap, the most significant first,
// as hexadecimal unsigned longs.  These are the same lines a device manager
// would see as the PROP/EV/KEY/... properties of the input device, and the
// same bitmaps the EVIOCGBIT/EVIOCGPROP requests return on the node.
//
// The classification below assigns the ID_INPUT_* tags by the rules of
// udev's input_id builtin, so programs that select devices by those tags
// (libinput most of all, whose model quirks also match on them) see what
// they would see on any other desktop system.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ludev.h"

// Upper bound on the size of /proc/bus/input/devices.  Each device takes well
// under 1 KiB; this allows a thousand of them.
#define PROC_INPUT_MAX (1024u * 1024u)
// Upper bound on the words of one bitmap line (the key bitmap has 12).
#define BITMAP_WORDS_MAX 64

static const struct {
	const char *name;
	unsigned int count;
} cap_info[LUDEV_CAP_COUNT] = {
	[LUDEV_CAP_PROP] = { "PROP", INPUT_PROP_CNT },
	[LUDEV_CAP_EV] = { "EV", EV_CNT },
	[LUDEV_CAP_KEY] = { "KEY", KEY_CNT },
	[LUDEV_CAP_REL] = { "REL", REL_CNT },
	[LUDEV_CAP_ABS] = { "ABS", ABS_CNT },
	[LUDEV_CAP_MSC] = { "MSC", MSC_CNT },
	[LUDEV_CAP_LED] = { "LED", LED_CNT },
	[LUDEV_CAP_SND] = { "SND", SND_CNT },
	[LUDEV_CAP_FF] = { "FF", FF_CNT },
	[LUDEV_CAP_SW] = { "SW", SW_CNT },
};

const char *ludev_cap_name(enum ludev_cap cap)
{
	return (unsigned int)cap < LUDEV_CAP_COUNT ? cap_info[cap].name : NULL;
}

unsigned int ludev_cap_count(enum ludev_cap cap)
{
	return (unsigned int)cap < LUDEV_CAP_COUNT ? cap_info[cap].count : 0;
}

bool ludev_test_bit(const struct ludev_input *in, enum ludev_cap cap,
		    unsigned int code)
{
	if ((unsigned int)cap >= LUDEV_CAP_COUNT || code >= cap_info[cap].count)
		return false;
	return (in->caps[cap].w[code / LUDEV_LONG_BITS] >>
		(code % LUDEV_LONG_BITS)) & 1ul;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

static char *dup_range(const char *s, const char *e)
{
	size_t n;
	char *r;

	if (e < s)
		e = s;
	n = (size_t)(e - s);
	if (n > LUDEV_VALUE_MAX)
		n = LUDEV_VALUE_MAX;
	r = malloc(n + 1);
	if (!r)
		return NULL;
	memcpy(r, s, n);
	r[n] = '\0';
	return r;
}

static void input_free(struct ludev_input *in)
{
	free(in->name);
	free(in->phys);
	free(in->uniq);
	free(in->sysfs);
	for (int i = 0; i < LUDEV_CAP_COUNT; i++)
		free(in->caps_text[i]);
	memset(in, 0, sizeof(*in));
}

void ludev_input_table_free(struct ludev_input_table *t)
{
	if (!t)
		return;
	for (size_t i = 0; i < t->n; i++)
		input_free(&t->v[i]);
	free(t->v);
	t->v = NULL;
	t->n = 0;
}

// "Key=hhhh" anywhere in [s, e): the hexadecimal value, or 0.
static unsigned int field_hex(const char *s, const char *e, const char *key)
{
	size_t kl = strlen(key);
	unsigned long v = 0;

	for (const char *p = s; p + kl <= e; p++) {
		if (memcmp(p, key, kl) != 0)
			continue;
		if (p > s && p[-1] != ' ')
			continue;
		p += kl;
		for (int digits = 0; p < e && digits < 8; p++, digits++) {
			int d;

			if (*p >= '0' && *p <= '9')
				d = *p - '0';
			else if (*p >= 'a' && *p <= 'f')
				d = *p - 'a' + 10;
			else if (*p >= 'A' && *p <= 'F')
				d = *p - 'A' + 10;
			else
				break;
			v = v * 16 + (unsigned long)d;
		}
		return (unsigned int)(v & 0xffffu);
	}
	return 0;
}

// The words of a bitmap line, most significant first, into `map'.  Words
// beyond the type's size and bits past its last code are dropped.
static int parse_bitmap(const char *s, const char *e, enum ludev_cap cap,
			struct ludev_bitmap *map)
{
	unsigned long words[BITMAP_WORDS_MAX];
	unsigned int nwords = 0, count = cap_info[cap].count;
	unsigned int maxw = LUDEV_NLONGS(count);
	const char *p = s;

	memset(map, 0, sizeof(*map));
	while (p < e) {
		unsigned long v = 0;
		int digits = 0;

		while (p < e && *p == ' ')
			p++;
		if (p >= e)
			break;
		for (; p < e && *p != ' '; p++) {
			int d;

			if (*p >= '0' && *p <= '9')
				d = *p - '0';
			else if (*p >= 'a' && *p <= 'f')
				d = *p - 'a' + 10;
			else if (*p >= 'A' && *p <= 'F')
				d = *p - 'A' + 10;
			else
				return -EINVAL;
			if (++digits > (int)(sizeof(unsigned long) * 2))
				return -EINVAL;
			v = (v << 4) | (unsigned long)d;
		}
		if (nwords == BITMAP_WORDS_MAX)
			return -EINVAL;
		words[nwords++] = v;
	}
	// words[nwords - 1] is word 0.
	for (unsigned int i = 0; i < nwords; i++) {
		unsigned int w = nwords - 1 - i;

		if (w < maxw)
			map->w[w] = words[i];
	}
	// Clear the bits of codes the type does not have.
	if (count % LUDEV_LONG_BITS && maxw > 0)
		map->w[maxw - 1] &= (1ul << (count % LUDEV_LONG_BITS)) - 1;
	return 0;
}

static bool line_is(const char *s, const char *e, const char *prefix,
		    const char **rest)
{
	size_t n = strlen(prefix);

	if ((size_t)(e - s) < n || memcmp(s, prefix, n) != 0)
		return false;
	*rest = s + n;
	return true;
}

static int finish_record(struct ludev_input_table *t, struct ludev_input *cur,
			 bool *open)
{
	struct ludev_input *nv;

	if (!*open)
		return 0;
	*open = false;
	if (!cur->name)
		cur->name = dup_range("", "");
	if (!cur->phys)
		cur->phys = dup_range("", "");
	if (!cur->uniq)
		cur->uniq = dup_range("", "");
	if (!cur->sysfs || cur->sysfs[0] != '/') {
		char buf[64];

		free(cur->sysfs);

		// Every kernel block carries one; a description without a
		// usable one still gets a name of its own, so it remains
		// addressable.
		snprintf(buf, sizeof(buf), "/devices/virtual/input/input-noname%zu",
			 t->n);
		cur->sysfs = dup_range(buf, buf + strlen(buf));
	}
	if (!cur->name || !cur->phys || !cur->uniq || !cur->sysfs) {
		input_free(cur);
		return -ENOMEM;
	}
	nv = realloc(t->v, (t->n + 1) * sizeof(*nv));
	if (!nv) {
		input_free(cur);
		return -ENOMEM;
	}
	t->v = nv;
	t->v[t->n++] = *cur;
	memset(cur, 0, sizeof(*cur));
	cur->event = -1;
	return 0;
}

int ludev_input_parse(const char *text, size_t len, struct ludev_input_table *out)
{
	struct ludev_input cur;
	const char *p = text, *end = text + len;
	bool open = false;
	int rc = 0;

	out->v = NULL;
	out->n = 0;
	memset(&cur, 0, sizeof(cur));
	cur.event = -1;

	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *le = nl ? nl : end;
		const char *r;

		// Tolerate a carriage return before the newline.
		const char *ce = (le > p && le[-1] == '\r') ? le - 1 : le;

		if (ce == p) {
			rc = finish_record(out, &cur, &open);
			if (rc)
				goto fail;
		} else if (line_is(p, ce, "I:", &r)) {
			rc = finish_record(out, &cur, &open);
			if (rc)
				goto fail;
			open = true;
			cur.bustype = field_hex(r, ce, "Bus=");
			cur.vendor = field_hex(r, ce, "Vendor=");
			cur.product = field_hex(r, ce, "Product=");
			cur.version = field_hex(r, ce, "Version=");
		} else if (line_is(p, ce, "N: Name=", &r)) {
			const char *q1 = memchr(r, '"', (size_t)(ce - r));
			const char *q2 = ce;

			open = true;
			if (q1) {
				// Up to the LAST quote: a name may itself
				// contain one.
				while (q2 > q1 + 1 && q2[-1] != '"')
					q2--;
				if (q2 > q1 + 1)
					q2--;
				else
					q2 = ce;
				r = q1 + 1;
			}
			free(cur.name);
			cur.name = dup_range(r, q2 < r ? r : q2);
			if (!cur.name) {
				rc = -ENOMEM;
				goto fail;
			}
		} else if (line_is(p, ce, "P: Phys=", &r)) {
			open = true;
			free(cur.phys);
			cur.phys = dup_range(r, ce);
			if (!cur.phys) {
				rc = -ENOMEM;
				goto fail;
			}
		} else if (line_is(p, ce, "S: Sysfs=", &r)) {
			open = true;
			free(cur.sysfs);
			cur.sysfs = dup_range(r, ce);
			if (!cur.sysfs) {
				rc = -ENOMEM;
				goto fail;
			}
		} else if (line_is(p, ce, "U: Uniq=", &r)) {
			open = true;
			free(cur.uniq);
			cur.uniq = dup_range(r, ce);
			if (!cur.uniq) {
				rc = -ENOMEM;
				goto fail;
			}
		} else if (line_is(p, ce, "H: Handlers=", &r)) {
			open = true;
			while (r < ce) {
				const char *ts, *te;
				char tok[24];

				while (r < ce && *r == ' ')
					r++;
				ts = r;
				while (r < ce && *r != ' ')
					r++;
				te = r;
				if (te > ts && (size_t)(te - ts) < sizeof(tok)) {
					memcpy(tok, ts, (size_t)(te - ts));
					tok[te - ts] = '\0';
					int ev = ludev_parse_event_name(tok);

					if (ev >= 0)
						cur.event = ev;
				}
			}
		} else if (line_is(p, ce, "B: ", &r)) {
			const char *eq = memchr(r, '=', (size_t)(ce - r));

			open = true;
			if (eq) {
				for (int c = 0; c < LUDEV_CAP_COUNT; c++) {
					size_t nl2 = strlen(cap_info[c].name);

					if ((size_t)(eq - r) != nl2 ||
					    memcmp(r, cap_info[c].name, nl2) != 0)
						continue;
					// A malformed line leaves the type
					// empty rather than half-filled.
					if (parse_bitmap(eq + 1, ce, (enum ludev_cap)c,
							 &cur.caps[c]) != 0)
						memset(&cur.caps[c], 0,
						       sizeof(cur.caps[c]));
					free(cur.caps_text[c]);
					cur.caps_text[c] = dup_range(eq + 1, ce);
					if (!cur.caps_text[c]) {
						rc = -ENOMEM;
						goto fail;
					}
					break;
				}
			}
		}
		// Lines of any other kind are ignored.
		p = nl ? nl + 1 : end;
	}
	rc = finish_record(out, &cur, &open);
	if (rc)
		goto fail;
	return 0;

fail:
	input_free(&cur);
	ludev_input_table_free(out);
	return rc;
}

int ludev_input_load(struct ludev_input_table *out)
{
	char *text;
	size_t len;
	int rc;

	out->v = NULL;
	out->n = 0;
	rc = ludev_read_file(ludev_paths.proc_input_devices, &text, &len,
			     PROC_INPUT_MAX);
	if (rc)
		return rc;
	rc = ludev_input_parse(text, len, out);
	free(text);
	return rc;
}

const struct ludev_input *ludev_input_find_event(const struct ludev_input_table *t,
						 int event)
{
	if (event < 0)
		return NULL;
	for (size_t i = 0; i < t->n; i++)
		if (t->v[i].event == event)
			return &t->v[i];
	return NULL;
}

const struct ludev_input *ludev_input_find_sysfs(const struct ludev_input_table *t,
						 const char *sysfs)
{
	if (!sysfs)
		return NULL;
	for (size_t i = 0; i < t->n; i++)
		if (strcmp(t->v[i].sysfs, sysfs) == 0)
			return &t->v[i];
	return NULL;
}

// ---------------------------------------------------------------------------
// Classification
//
// The rules of udev's input_id builtin: what the device can report decides
// what it is.  Pointer-like devices first (test_pointers), then keys
// (test_key), then the leftovers (wheel-only devices count as keys, any
// switch makes a switch).
// ---------------------------------------------------------------------------

#define BIT(cap, code) ludev_test_bit(in, LUDEV_CAP_##cap, (code))

static bool test_pointers(const struct ludev_input *in, unsigned int *flags)
{
	bool has_keys = BIT(EV, EV_KEY);
	bool has_abs_coordinates = BIT(ABS, ABS_X) && BIT(ABS, ABS_Y);
	bool has_3d_coordinates = has_abs_coordinates && BIT(ABS, ABS_Z);
	bool is_accelerometer = BIT(PROP, INPUT_PROP_ACCELEROMETER);
	bool is_pointing_stick, has_stylus, has_pen, finger_but_no_pen;
	bool has_mouse_button = false, has_rel_coordinates, has_mt_coordinates;
	bool is_direct, has_touch, has_pad_buttons, has_wheel;
	bool is_tablet = false, is_touchpad = false, is_abs_mouse = false;
	bool is_touchscreen = false, is_joystick = false, is_mouse = false;
	bool is_tablet_pad = false;
	unsigned int num_joystick_buttons = 0, num_joystick_axes = 0;

	if (!has_keys && has_3d_coordinates)
		is_accelerometer = true;
	if (is_accelerometer) {
		*flags |= LUDEV_ID_ACCELEROMETER;
		return true;
	}

	is_pointing_stick = BIT(PROP, INPUT_PROP_POINTING_STICK);
	has_stylus = BIT(KEY, BTN_STYLUS);
	has_pen = BIT(KEY, BTN_TOOL_PEN);
	finger_but_no_pen = BIT(KEY, BTN_TOOL_FINGER) && !BIT(KEY, BTN_TOOL_PEN);
	for (unsigned int b = BTN_MOUSE; b < BTN_JOYSTICK && !has_mouse_button; b++)
		has_mouse_button = BIT(KEY, b);
	has_rel_coordinates = BIT(EV, EV_REL) && BIT(REL, REL_X) && BIT(REL, REL_Y);
	has_mt_coordinates = BIT(ABS, ABS_MT_POSITION_X) &&
			     BIT(ABS, ABS_MT_POSITION_Y);
	// A device claiming every absolute axis is not really multitouch.
	if (has_mt_coordinates && BIT(ABS, ABS_MT_SLOT) && BIT(ABS, ABS_MT_SLOT - 1))
		has_mt_coordinates = false;
	is_direct = BIT(PROP, INPUT_PROP_DIRECT);
	has_touch = BIT(KEY, BTN_TOUCH);
	has_pad_buttons = BIT(KEY, BTN_0) && has_stylus && !has_pen;
	has_wheel = BIT(EV, EV_REL) && (BIT(REL, REL_WHEEL) || BIT(REL, REL_HWHEEL));

	// Joysticks need not have buttons (pedals, rudders) nor axes.  The
	// joystick button range follows the mouse one, so a mouse with more
	// than sixteen buttons runs into it: such a device is not counted.
	if (!BIT(KEY, BTN_JOYSTICK - 1)) {
		for (unsigned int b = BTN_JOYSTICK; b < BTN_DIGI; b++)
			if (BIT(KEY, b))
				num_joystick_buttons++;
		for (unsigned int b = BTN_TRIGGER_HAPPY1; b <= BTN_TRIGGER_HAPPY40; b++)
			if (BIT(KEY, b))
				num_joystick_buttons++;
		for (unsigned int b = BTN_DPAD_UP; b <= BTN_DPAD_RIGHT; b++)
			if (BIT(KEY, b))
				num_joystick_buttons++;
	}
	for (unsigned int a = ABS_RX; a < ABS_PRESSURE; a++)
		if (BIT(ABS, a))
			num_joystick_axes++;

	if (has_abs_coordinates) {
		if (has_stylus || has_pen)
			is_tablet = true;
		else if (finger_but_no_pen && !is_direct)
			is_touchpad = true;
		else if (has_mouse_button)
			// Absolute pointers with buttons but no touch
			// (virtual machines' tablets) are mice.
			is_abs_mouse = true;
		else if (has_touch || is_direct)
			is_touchscreen = true;
		else if (num_joystick_buttons > 0 || num_joystick_axes > 0)
			is_joystick = true;
	} else if (num_joystick_buttons > 0 || num_joystick_axes > 0) {
		is_joystick = true;
	}

	if (has_mt_coordinates) {
		if (has_stylus || has_pen)
			is_tablet = true;
		else if (finger_but_no_pen && !is_direct)
			is_touchpad = true;
		else if (has_touch || is_direct)
			is_touchscreen = true;
	}

	if (is_tablet && has_pad_buttons)
		is_tablet_pad = true;
	if (has_pad_buttons && has_wheel && !has_rel_coordinates) {
		is_tablet = true;
		is_tablet_pad = true;
	}

	if (!is_tablet && !is_touchpad && !is_joystick && has_mouse_button &&
	    (has_rel_coordinates || !has_abs_coordinates))
		is_mouse = true;

	// There is no such thing as an I2C mouse: a relative pointer on that
	// bus is the pointing stick of a touchpad module.
	if (is_mouse && in->bustype == BUS_I2C)
		is_pointing_stick = true;

	// Keyboards with a stray joystick button, and "joysticks" with a
	// single control, are not joysticks.
	if (is_joystick) {
		static const unsigned int well_known_keyboard_keys[] = {
			KEY_LEFTCTRL, KEY_CAPSLOCK, KEY_NUMLOCK, KEY_INSERT,
			KEY_MUTE,     KEY_CALC,     KEY_FILE,    KEY_MAIL,
			KEY_PLAYPAUSE, KEY_BRIGHTNESSDOWN,
		};
		unsigned int num_well_known_keys = 0;

		if (has_keys)
			for (size_t i = 0; i < sizeof(well_known_keyboard_keys) /
						       sizeof(well_known_keyboard_keys[0]);
			     i++)
				if (BIT(KEY, well_known_keyboard_keys[i]))
					num_well_known_keys++;
		if (num_well_known_keys >= 4 ||
		    num_joystick_buttons + num_joystick_axes < 2)
			is_joystick = false;
		if (has_wheel && has_pad_buttons)
			is_joystick = false;
	}

	if (is_pointing_stick)
		*flags |= LUDEV_ID_POINTINGSTICK;
	if (is_mouse || is_abs_mouse)
		*flags |= LUDEV_ID_MOUSE;
	if (is_touchpad)
		*flags |= LUDEV_ID_TOUCHPAD;
	if (is_touchscreen)
		*flags |= LUDEV_ID_TOUCHSCREEN;
	if (is_joystick)
		*flags |= LUDEV_ID_JOYSTICK;
	if (is_tablet)
		*flags |= LUDEV_ID_TABLET;
	if (is_tablet_pad)
		*flags |= LUDEV_ID_TABLET_PAD;

	return is_tablet || is_mouse || is_abs_mouse || is_touchpad ||
	       is_touchscreen || is_joystick || is_pointing_stick;
}

static bool test_key(const struct ludev_input *in, unsigned int *flags)
{
	// The key codes above the button ranges.
	static const struct {
		unsigned int start, end;
	} high_key_blocks[] = {
		{ KEY_OK, BTN_DPAD_UP },
		{ KEY_ALS_TOGGLE, BTN_TRIGGER_HAPPY },
	};
	bool found = false;

	if (!BIT(EV, EV_KEY))
		return false;

	// Any KEY_* below the buttons (codes 0 .. BTN_MISC-1).
	for (unsigned int c = 0; c < BTN_MISC && !found; c += LUDEV_LONG_BITS)
		if (in->caps[LUDEV_CAP_KEY].w[c / LUDEV_LONG_BITS])
			found = true;
	for (size_t b = 0; !found && b < sizeof(high_key_blocks) /
						 sizeof(high_key_blocks[0]);
	     b++)
		for (unsigned int c = high_key_blocks[b].start;
		     c < high_key_blocks[b].end && !found; c++)
			if (BIT(KEY, c))
				found = true;

	if (found)
		*flags |= LUDEV_ID_KEY;

	// The first 32 codes are Esc, the digits and the top letter rows; a
	// device with all of them (KEY_RESERVED aside) is a full keyboard.
	if ((in->caps[LUDEV_CAP_KEY].w[0] & 0xfffffffeul) == 0xfffffffeul) {
		*flags |= LUDEV_ID_KEYBOARD;
		return true;
	}
	return found;
}

unsigned int ludev_input_classify(const struct ludev_input *in)
{
	unsigned int flags = LUDEV_ID_INPUT;
	bool is_pointer = test_pointers(in, &flags);
	bool is_key = test_key(in, &flags);

	// Some event nodes have only a scroll wheel.
	if (!is_pointer && !is_key && BIT(EV, EV_REL) &&
	    (BIT(REL, REL_WHEEL) || BIT(REL, REL_HWHEEL)))
		flags |= LUDEV_ID_KEY;
	if (BIT(EV, EV_SW))
		flags |= LUDEV_ID_SWITCH;
	return flags;
}

#undef BIT

const struct ludev_id_name ludev_id_names[] = {
	{ LUDEV_ID_INPUT, "ID_INPUT" },
	{ LUDEV_ID_ACCELEROMETER, "ID_INPUT_ACCELEROMETER" },
	{ LUDEV_ID_POINTINGSTICK, "ID_INPUT_POINTINGSTICK" },
	{ LUDEV_ID_MOUSE, "ID_INPUT_MOUSE" },
	{ LUDEV_ID_TOUCHPAD, "ID_INPUT_TOUCHPAD" },
	{ LUDEV_ID_TOUCHSCREEN, "ID_INPUT_TOUCHSCREEN" },
	{ LUDEV_ID_JOYSTICK, "ID_INPUT_JOYSTICK" },
	{ LUDEV_ID_TABLET, "ID_INPUT_TABLET" },
	{ LUDEV_ID_TABLET_PAD, "ID_INPUT_TABLET_PAD" },
	{ LUDEV_ID_KEY, "ID_INPUT_KEY" },
	{ LUDEV_ID_KEYBOARD, "ID_INPUT_KEYBOARD" },
	{ LUDEV_ID_SWITCH, "ID_INPUT_SWITCH" },
};
const size_t ludev_id_names_count = sizeof(ludev_id_names) / sizeof(ludev_id_names[0]);

const char *ludev_bus_name(unsigned int bustype)
{
	switch (bustype) {
	case BUS_PCI:
		return "pci";
	case BUS_USB:
		return "usb";
	case BUS_BLUETOOTH:
		return "bluetooth";
	case BUS_I8042:
		return "i8042";
	case BUS_I2C:
		return "i2c";
	case BUS_SPI:
		return "spi";
	case BUS_RMI:
		return "rmi";
	default:
		return NULL;
	}
}

bool ludev_device_group(const struct ludev_input *in, char *buf, size_t size)
{
	char *cut, *dot, *dash;
	int n;

	n = snprintf(buf, size, "%x/%x/%x:%s", in->bustype, in->vendor,
		     in->product, in->phys ? in->phys : "");
	if (n < 0 || (size_t)n >= size)
		return false;
	// The interface number is not part of the physical device:
	// "usb-0000:00:14.0-1/input1" groups with ".../input0".
	cut = strstr(buf, "/input");
	if (cut)
		*cut = '\0';
	// Nor is a function number after the port path: "...-6.3.1" groups
	// with "...-6.3.0".
	dot = strrchr(buf, '.');
	dash = strrchr(buf, '-');
	if (dot && (!dash || dot > dash))
		*dot = '\0';
	return true;
}
