// likeos-input-devices - finding the touchpads and writing their X
// configuration.  See xconf.h.
//
// Copyright (C) 2026 The LikeOS Project

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __LIKEOS__
#include <sys/input.h>
#else
#include <linux/input.h>
#endif

#include <libudev.h>

#include "xconf.h"

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

void lid_options_defaults(struct lid_options *o)
{
	memset(o, 0, sizeof(*o));
	o->tapping = 1;
	o->tapping_drag = 1;
	o->tapping_drag_lock = -1;
	o->natural_scrolling = 0;
	o->disable_while_typing = 1;
	o->left_handed = -1;
	o->middle_emulation = -1;
	o->horizontal_scrolling = -1;
	snprintf(o->accel_profile, sizeof(o->accel_profile), "adaptive");
}

static int parse_bool(const char *v)
{
	static const char *const yes[] = { "on", "true", "yes", "1" };
	static const char *const no[] = { "off", "false", "no", "0" };

	for (size_t i = 0; i < 4; i++) {
		if (strcasecmp(v, yes[i]) == 0)
			return 1;
		if (strcasecmp(v, no[i]) == 0)
			return 0;
	}
	return -1;
}

static bool one_of(const char *v, const char *const *set, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(v, set[i]) == 0)
			return true;
	return false;
}

// "-1" .. "1" with an optional fraction, no exponent, no sign tricks.
static bool valid_speed(const char *v)
{
	const char *p = v;
	double d;
	char *end;

	if (*p == '-' || *p == '+')
		p++;
	if (!isdigit((unsigned char)*p))
		return false;
	while (isdigit((unsigned char)*p))
		p++;
	if (*p == '.') {
		p++;
		if (!isdigit((unsigned char)*p))
			return false;
		while (isdigit((unsigned char)*p))
			p++;
	}
	if (*p || strlen(v) >= 16)
		return false;
	d = strtod(v, &end);
	return *end == '\0' && d >= -1.0 && d <= 1.0;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

int lid_options_parse(struct lid_options *o, const char *text, char *err,
		      size_t errlen)
{
	static const struct {
		const char *key;
		size_t off;
	} bools[] = {
		{ "Tapping", offsetof(struct lid_options, tapping) },
		{ "TappingDrag", offsetof(struct lid_options, tapping_drag) },
		{ "TappingDragLock", offsetof(struct lid_options, tapping_drag_lock) },
		{ "NaturalScrolling", offsetof(struct lid_options, natural_scrolling) },
		{ "DisableWhileTyping", offsetof(struct lid_options, disable_while_typing) },
		{ "LeftHanded", offsetof(struct lid_options, left_handed) },
		{ "MiddleEmulation", offsetof(struct lid_options, middle_emulation) },
		{ "HorizontalScrolling", offsetof(struct lid_options, horizontal_scrolling) },
	};
	static const char *const scroll[] = { "twofinger", "edge", "none" };
	static const char *const click[] = { "clickfinger", "buttonareas", "none" };
	static const char *const profile[] = { "adaptive", "flat" };
	char *copy, *line, *save = NULL;
	int bad = 0, lineno = 0;

	if (err && errlen)
		err[0] = '\0';
	copy = strdup(text ? text : "");
	if (!copy)
		return -ENOMEM;
	for (line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *k, *v, *eq;
		bool ok = false;

		lineno++;
		k = trim(line);
		if (!*k || *k == '#')
			continue;
		eq = strchr(k, '=');
		if (eq) {
			*eq = '\0';
			v = trim(eq + 1);
			k = trim(k);
			for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
				if (strcmp(k, bools[i].key) != 0)
					continue;
				int b = parse_bool(v);

				if (b >= 0) {
					*(int *)((char *)o + bools[i].off) = b;
					ok = true;
				}
				break;
			}
			if (strcmp(k, "ScrollMethod") == 0 && one_of(v, scroll, 3)) {
				snprintf(o->scroll_method, sizeof(o->scroll_method), "%s", v);
				ok = true;
			} else if (strcmp(k, "ClickMethod") == 0 && one_of(v, click, 3)) {
				snprintf(o->click_method, sizeof(o->click_method), "%s", v);
				ok = true;
			} else if (strcmp(k, "AccelProfile") == 0 && one_of(v, profile, 2)) {
				snprintf(o->accel_profile, sizeof(o->accel_profile), "%s", v);
				ok = true;
			} else if (strcmp(k, "AccelSpeed") == 0 && valid_speed(v)) {
				snprintf(o->accel_speed, sizeof(o->accel_speed), "%s", v);
				ok = true;
			}
		}
		if (!ok) {
			if (!bad && err && errlen)
				snprintf(err, errlen, "line %d: not a known setting", lineno);
			bad++;
		}
	}
	free(copy);
	return bad;
}

// ---------------------------------------------------------------------------
// Collecting
// ---------------------------------------------------------------------------

// Is `code' set in a bitmap attribute ("e520 10000 0 0 0 0": hexadecimal
// words, most significant first)?
static bool attr_bit(const char *text, unsigned int code)
{
	const unsigned int bits = 8u * (unsigned int)sizeof(unsigned long);
	const char *words[64];
	unsigned int n = 0, want = code / bits;
	const char *p = text;

	if (!text)
		return false;
	while (*p && n < 64) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		words[n++] = p;
		while (*p && *p != ' ')
			p++;
	}
	if (want >= n)
		return false;
	{
		const char *w = words[n - 1 - want];
		unsigned long v = 0;
		int digits = 0;

		for (; *w && *w != ' '; w++) {
			int d;

			if (*w >= '0' && *w <= '9')
				d = *w - '0';
			else if (*w >= 'a' && *w <= 'f')
				d = *w - 'a' + 10;
			else if (*w >= 'A' && *w <= 'F')
				d = *w - 'A' + 10;
			else
				return false;
			if (++digits > (int)(sizeof(unsigned long) * 2))
				return false;
			v = (v << 4) | (unsigned long)d;
		}
		return (v >> (code % bits)) & 1ul;
	}
}

static unsigned int attr_hex(const char *text)
{
	unsigned long v;
	char *end;

	if (!text || !*text)
		return 0;
	v = strtoul(text, &end, 16);
	return *end ? 0 : (unsigned int)(v & 0xffffu);
}

// The kernel's device name as it may stand in a quoted configuration string
// and a comment: printable ASCII without quotes or backslashes, no
// surrounding blanks, at most 64 characters (room is left for the
// disambiguating suffix).
static void safe_name(const char *in, char *out, size_t size)
{
	size_t n = 0;

	if (size == 0)
		return;
	for (; in && *in && n + 1 < size && n < 64; in++) {
		unsigned char c = (unsigned char)*in;

		if (c < 0x20 || c > 0x7e || c == '"' || c == '\\')
			c = '_';
		if (n == 0 && c == ' ')
			continue;
		out[n++] = (char)c;
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

// The X server compares identifiers ignoring case, blanks and underscores.
static bool same_identifier(const char *a, const char *b)
{
	for (;;) {
		while (*a == ' ' || *a == '\t' || *a == '_')
			a++;
		while (*b == ' ' || *b == '\t' || *b == '_')
			b++;
		if (!*a || !*b)
			return *a == *b;
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
		a++;
		b++;
	}
}

static int by_event(const void *a, const void *b)
{
	const struct lid_touchpad *x = a, *y = b;

	return (x->event > y->event) - (x->event < y->event);
}

int lid_collect(struct udev *udev, struct lid_touchpad **out, size_t *count)
{
	struct udev_enumerate *e;
	struct udev_list_entry *l;
	struct lid_touchpad *v = NULL;
	size_t n = 0;
	int rc;

	*out = NULL;
	*count = 0;
	e = udev_enumerate_new(udev);
	if (!e)
		return -errno;
	if ((rc = udev_enumerate_add_match_subsystem(e, "input")) ||
	    (rc = udev_enumerate_add_match_sysname(e, "event*")) ||
	    (rc = udev_enumerate_add_match_property(e, "ID_INPUT_TOUCHPAD", "1")) ||
	    (rc = udev_enumerate_scan_devices(e))) {
		udev_enumerate_unref(e);
		return rc;
	}
	udev_list_entry_foreach(l, udev_enumerate_get_list_entry(e)) {
		struct udev_device *d, *p;
		struct lid_touchpad *nv, *t;
		const char *sysname, *num;

		d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(l));
		if (!d)
			continue; // gone since the scan
		sysname = udev_device_get_sysname(d);
		num = udev_device_get_sysnum(d);
		p = udev_device_get_parent_with_subsystem_devtype(d, "input", NULL);
		if (!sysname || strncmp(sysname, "event", 5) != 0 || !num || !p ||
		    !udev_device_get_devnode(d)) {
			udev_device_unref(d);
			continue;
		}
		nv = realloc(v, (n + 1) * sizeof(*v));
		if (!nv) {
			udev_device_unref(d);
			free(v);
			udev_enumerate_unref(e);
			return -ENOMEM;
		}
		v = nv;
		t = &v[n++];
		memset(t, 0, sizeof(*t));
		t->event = atoi(num);
		safe_name(udev_device_get_sysattr_value(p, "name"), t->name,
			  sizeof(t->name));
		t->bustype = attr_hex(udev_device_get_sysattr_value(p, "id/bustype"));
		t->vendor = attr_hex(udev_device_get_sysattr_value(p, "id/vendor"));
		t->product = attr_hex(udev_device_get_sysattr_value(p, "id/product"));
		t->buttonpad = attr_bit(udev_device_get_sysattr_value(p, "properties"),
					INPUT_PROP_BUTTONPAD);
		t->multifinger =
			attr_bit(udev_device_get_sysattr_value(p, "capabilities/key"),
				 BTN_TOOL_DOUBLETAP) ||
			attr_bit(udev_device_get_sysattr_value(p, "capabilities/abs"),
				 ABS_MT_SLOT);
		udev_device_unref(d);
	}
	udev_enumerate_unref(e);
	if (n > 1)
		qsort(v, n, sizeof(*v), by_event);
	*out = v;
	*count = n;
	return 0;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

struct sbuf {
	char *p;
	size_t len, cap;
	bool failed;
};

static void sb_printf(struct sbuf *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void sb_printf(struct sbuf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (b->failed)
		return;
	for (;;) {
		size_t room = b->cap - b->len;

		va_start(ap, fmt);
		n = vsnprintf(b->p ? b->p + b->len : NULL, room, fmt, ap);
		va_end(ap);
		if (n < 0) {
			b->failed = true;
			return;
		}
		if ((size_t)n < room) {
			b->len += (size_t)n;
			return;
		}
		size_t ncap = b->cap ? b->cap * 2 : 1024;

		while (ncap - b->len <= (size_t)n)
			ncap *= 2;
		char *np = realloc(b->p, ncap);

		if (!np) {
			b->failed = true;
			return;
		}
		b->p = np;
		b->cap = ncap;
	}
}

static const char *bus_word(unsigned int bus)
{
	switch (bus) {
	case BUS_USB:
		return "usb";
	case BUS_BLUETOOTH:
		return "bluetooth";
	case BUS_I8042:
		return "ps/2";
	case BUS_I2C:
		return "i2c";
	case BUS_SPI:
		return "spi";
	case BUS_RMI:
		return "rmi";
	default:
		return "bus";
	}
}

static void opt_line(struct sbuf *b, const char *name, const char *value)
{
	// Tab-aligned like the shipped configurations.
	size_t len = strlen(name) + 2;

	sb_printf(b, "\tOption\t\t\"%s\"%s\"%s\"\n", name,
		  len < 8 ? "\t\t\t" : len < 16 ? "\t\t" : "\t", value);
}

static void opt_bool(struct sbuf *b, const char *name, int v)
{
	if (v >= 0)
		opt_line(b, name, v ? "on" : "off");
}

char *lid_render(const struct lid_touchpad *tp, size_t count,
		 const struct lid_options *o)
{
	struct sbuf b = { 0 };
	char (*ids)[LID_NAME_MAX + 24];

	if (count == 0) {
		errno = EINVAL;
		return NULL;
	}
	ids = calloc(count, sizeof(*ids));
	if (!ids) {
		errno = ENOMEM;
		return NULL;
	}

	sb_printf(&b,
		  "# The touchpads of this machine, for the X server.\n"
		  "#\n"
		  "# Written by likeos-input-devices, which init keeps running: it\n"
		  "# rewrites this file whenever a touchpad appears or goes away and\n"
		  "# removes it when there is none.  The X server reads it when it\n"
		  "# starts.  Do not edit it -- put settings in %s\n"
		  "# and run likeos-input-devices as root instead.\n"
		  "#\n"
		  "# \"AutoServerLayout\" adds each touchpad to whichever configuration\n"
		  "# the server runs with, beside its keyboard and mouse.  The touchpad\n"
		  "# itself is handled by libinput: tapping, scrolling, clickpad\n"
		  "# buttons, palm and thumb detection, acceleration and gestures.\n",
		  LID_DEFAULT_SETTINGS);

	for (size_t i = 0; i < count; i++) {
		const struct lid_touchpad *t = &tp[i];
		const char *scroll, *click = NULL;
		bool clash = false;

		// The identifier is the kernel's name, as the device is
		// called everywhere else; made unique when two touchpads (or
		// a touchpad and the shipped Keyboard0/Mouse0) share one.
		if (!t->name[0])
			snprintf(ids[i], sizeof(ids[i]), "Touchpad event%d", t->event);
		else
			snprintf(ids[i], sizeof(ids[i]), "%s", t->name);
		if (same_identifier(ids[i], "Keyboard0") || same_identifier(ids[i], "Mouse0"))
			clash = true;
		for (size_t j = 0; j < count && !clash; j++)
			if (j != i && same_identifier(t->name, tp[j].name))
				clash = true;
		if (clash)
			snprintf(ids[i], sizeof(ids[i]), "%s (event%d)",
				 t->name[0] ? t->name : "Touchpad", t->event);

		// Two-finger scrolling where the touchpad can tell two
		// fingers from one; otherwise the right edge scrolls.
		if (o->scroll_method[0])
			scroll = o->scroll_method;
		else
			scroll = t->multifinger ? "twofinger" : "edge";
		if (strcmp(scroll, "twofinger") == 0 && !t->multifinger)
			scroll = "edge";
		// Click methods exist on clickpads only.  Without a setting
		// libinput picks the model's own default: the bottom edge split
		// into left and right buttons on most clickpads, the number of
		// fingers on the few models built for that.
		if (t->buttonpad && o->click_method[0])
			click = o->click_method;

		sb_printf(&b, "\n# %s: %s %04x:%04x, %s\n", ids[i], bus_word(t->bustype),
			  t->vendor, t->product,
			  t->buttonpad ? "clickpad" : "touchpad with buttons");
		sb_printf(&b, "Section \"InputDevice\"\n");
		sb_printf(&b, "\tIdentifier\t\"%s\"\n", ids[i]);
		sb_printf(&b, "\tDriver\t\t\"libinput\"\n");
		{
			char dev[48];

			snprintf(dev, sizeof(dev), "/dev/input/event%d", t->event);
			opt_line(&b, "Device", dev);
		}
		opt_line(&b, "AutoServerLayout", "on");
		opt_bool(&b, "Tapping", o->tapping);
		opt_bool(&b, "TappingDrag", o->tapping_drag);
		opt_bool(&b, "TappingDragLock", o->tapping_drag_lock);
		opt_bool(&b, "NaturalScrolling", o->natural_scrolling);
		opt_line(&b, "ScrollMethod", scroll);
		opt_bool(&b, "HorizontalScrolling", o->horizontal_scrolling);
		if (click)
			opt_line(&b, "ClickMethod", click);
		opt_bool(&b, "DisableWhileTyping", o->disable_while_typing);
		opt_bool(&b, "MiddleEmulation", o->middle_emulation);
		opt_bool(&b, "LeftHanded", o->left_handed);
		if (o->accel_profile[0])
			opt_line(&b, "AccelProfile", o->accel_profile);
		if (o->accel_speed[0])
			opt_line(&b, "AccelSpeed", o->accel_speed);
		sb_printf(&b, "EndSection\n");
	}
	free(ids);
	if (b.failed) {
		free(b.p);
		errno = ENOMEM;
		return NULL;
	}
	return b.p;
}
