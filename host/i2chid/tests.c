/* Scenarios of the I2C-HID harness (host/test-i2chid.sh).  HID world:
 * compiled with the input harness's prelude against the kernel headers,
 * linked with the real I2C-HID transport, HID core, hid-multitouch, input
 * core, event devices and console pointer bridge -- and, through
 * kworld.h, with the real I2C core and DesignWare driver driving the
 * I2C-HID device model on the bus model.
 *
 * Groups:
 *   probe      HP- and Dell-shaped machines from ACPI enumeration to event
 *              nodes: descriptors, reset, power, input mode, names, paths,
 *              /proc/bus/input/devices blocks against kernel/spec, the
 *              interrupt's trigger; reports through the interrupt into the
 *              event stream; the SIPODEV DMI override; refused devices
 *              (excluded id, broken _DSM, bad descriptors, nothing there);
 *              deferred GPIO interrupt
 *   protocol   GET/SET_REPORT with report IDs below and above 15, output
 *              reports through the output register, refused requests
 *   faults     NACK on wake, missing / late reset acknowledgement, 0xffff
 *              lengths, oversized and short lengths, garbage, failing
 *              reads, recovery by reset, a device that resets itself, a
 *              reset storm, a device that disappears
 *   openclose  reporting switched off when the last user closes and on
 *              again (hid-multitouch's surface/button switch)
 *   pm         suspend / resume (SET_POWER, interrupt masked, wake armed,
 *              configuration sent again, reset on resume quirk), shutdown
 *   remove     driver unbinding while reports stream in
 *   mousedev   the console pointer bridge: motion, tap, two-finger tap and
 *              scroll, clickpad click, silence while a client has the
 *              node open and back after, the relative mouse collection
 *   ps2pref    the PS/2 preference decisions and the device classification
 *   stress     random faults, opens, suspends and rebinds for a while
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/dev/hid/core/hid.h>
#include <kernel/dev/hid/i2c-hid/i2c-hid.h>
#include <kernel/dev/input/input.h>
#include <kernel/dev/input/input-mt.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/dev/input/mousedev.h>
#include <kernel/ke/syscall.h>
#include "host/input/kstubs.h"
#include "host/hid/wqshim.h"
#include "host/hid/corpus.h"
#include "host/i2chid/kworld.h"
#include "host/i2chid/hworld.h"

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

/* Poll `cond' for up to `ms' milliseconds. */
#define WAIT_FOR(cond, ms)                                                \
	({                                                                \
		unsigned long long __end =                                \
			h_monotonic_ns() + (unsigned long long)(ms) * 1000000ULL; \
		int __ok = 0;                                             \
		for (;;) {                                                \
			if (cond) {                                       \
				__ok = 1;                                 \
				break;                                    \
			}                                                 \
			if (h_monotonic_ns() >= __end)                    \
				break;                                    \
			h_usleep(200);                                    \
		}                                                         \
		__ok;                                                     \
	})

#define HP_SPEC "kernel/spec/devices-ubuntu-live-hp-probook-650-p8"
#define DELL_SPEC "kernel/spec/devices-ubuntu-live-dellpro16plus"
#define HP_TP "ELAN073B:00 04F3:3169 Touchpad"
#define HP_MOUSE "ELAN073B:00 04F3:3169 Mouse"
#define DELL_TP "VEN_0488:00 0488:1085 Touchpad"
#define DELL_MOUSE "VEN_0488:00 0488:1085 Mouse"

struct contact {
	int conf, tip, id, x, y;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static unsigned int elan_tp(uint8_t *b, const struct contact *c, int n,
			    int scan, int count, int button)
{
	h_memset(b, 0, ELAN_TP_LEN);
	b[0] = 4;
	for (int i = 0; i < n && i < 5; i++) {
		uint8_t *f = b + 1 + i * 5;

		f[0] = (uint8_t)((c[i].conf & 1) | ((c[i].tip & 1) << 1) |
				 ((c[i].id & 15) << 4));
		f[1] = LO(c[i].x);
		f[2] = HI(c[i].x);
		f[3] = LO(c[i].y);
		f[4] = HI(c[i].y);
	}
	b[26] = LO(scan);
	b[27] = HI(scan);
	b[28] = (uint8_t)count;
	b[29] = (uint8_t)(button & 1);
	return ELAN_TP_LEN;
}

static unsigned int cirque_tp(uint8_t *b, const struct contact *c, int n,
			      int scan, int count, int button)
{
	h_memset(b, 0, CIRQUE_TP_LEN);
	b[0] = 4;
	for (int i = 0; i < n && i < 2; i++) {
		uint8_t *f = b + 1 + i * 6;

		f[0] = (uint8_t)((c[i].conf & 1) | ((c[i].tip & 1) << 1));
		f[1] = (uint8_t)c[i].id;
		f[2] = LO(c[i].x);
		f[3] = HI(c[i].x);
		f[4] = LO(c[i].y);
		f[5] = HI(c[i].y);
	}
	b[13] = LO(scan);
	b[14] = HI(scan);
	b[15] = (uint8_t)count;
	b[16] = (uint8_t)(button & 1);
	return CIRQUE_TP_LEN;
}

static int g_scan;
static struct contact g_last[5];
static int g_last_n;

/* One ELAN frame of `n' fingers (ids 0..), all at (x + 300 i, y).  Fingers
 * of the previous frame that are not in this one are reported lifted (tip
 * off), as a precision touchpad does. */
static void elan_frame(int n, int x, int y, int button)
{
	struct contact c[5];
	uint8_t b[64];
	int k = 0;

	for (int i = 0; i < n && i < 5; i++) {
		c[k].conf = 1;
		c[k].tip = 1;
		c[k].id = i;
		c[k].x = x + 300 * i;
		c[k].y = y;
		k++;
	}
	for (int i = n; i < g_last_n && k < 5; i++) {
		c[k] = g_last[i];
		c[k].tip = 0;
		k++;
	}
	for (int i = 0; i < n && i < 5; i++)
		g_last[i] = c[i];
	g_last_n = n < 5 ? n : 5;
	g_scan += 80;
	hidm_push_report(b, (int)elan_tp(b, c, k, g_scan & 0xffff, k, button));
}

static struct input_dev *find_input(struct hid_device *hdev, const char *name)
{
	struct hid_input *hi;

	if (!hdev)
		return NULL;
	list_for_each_entry(hi, &hdev->inputs, list)
		if (hi->input->name && !h_strcmp(hi->input->name, name))
			return hi->input;
	return NULL;
}

static int count_inputs(struct hid_device *hdev)
{
	struct hid_input *hi;
	int n = 0;

	if (!hdev)
		return 0;
	list_for_each_entry(hi, &hdev->inputs, list)
		n++;
	return n;
}

static struct evdev_client *open_input(struct input_dev *dev)
{
	struct evdev_client *c = NULL;
	int rc;

	if (!dev || !dev->evdev)
		return NULL;
	rc = evdev_open(evdev_minor_of(dev->evdev), &c);
	CHECK(rc == 0 && c, "open %s: %d", dev->name, rc);
	return rc ? NULL : c;
}

static int drain(struct evdev_client *c, struct input_event *e, int max)
{
	int n = 0;

	while (c && n < max) {
		long r = evdev_read(c, e + n,
				    (long)((max - n) * (int)sizeof(*e)), 1);

		if (r <= 0)
			break;
		n += (int)(r / (long)sizeof(*e));
	}
	return n;
}

/* Drain into e[] until something satisfying `has' (type, code, value) is
 * there or `ms' passed; returns the count. */
static int drain_until(struct evdev_client *c, struct input_event *e, int max,
		       unsigned type, unsigned code, int value, int ms)
{
	unsigned long long end = h_monotonic_ns() + (unsigned long long)ms *
							    1000000ULL;
	int n = 0;

	for (;;) {
		n += drain(c, e + n, max - n);
		for (int i = 0; i < n; i++)
			if (e[i].type == type && e[i].code == code &&
			    e[i].value == value)
				return n;
		if (h_monotonic_ns() >= end || n >= max)
			return n;
		h_usleep(200);
	}
}

static int stream_has(const struct input_event *e, int n, unsigned type,
		      unsigned code, int value)
{
	for (int i = 0; i < n; i++)
		if (e[i].type == type && e[i].code == code &&
		    e[i].value == value)
			return 1;
	return 0;
}

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

static int spec_block(const char *file, const char *name, char lines[][256],
		      int max)
{
	unsigned long len;
	char *text = (char *)h_read_file(file, &len);
	char want[300], line[256];
	const char *p, *start = NULL;
	int n = 0;

	CHECK(text != NULL, "read %s", file);
	if (!text)
		return 0;
	h_snprintf(want, sizeof(want), "N: Name=\"%s\"", name);
	p = h_strstr(text, want);
	if (p) {
		start = p;
		while (start > text && !(start[-1] == '\n' && start[0] == 'I'))
			start--;
	}
	CHECK(start != NULL, "%s: no block for %s", file, name);
	p = start;
	while (p && *p && n < max) {
		p = get_line(p, line, sizeof(line));
		if (!line[0])
			break;
		h_snprintf(lines[n++], 256, "%s", line);
	}
	h_free(text);
	return n;
}

/* The /proc/bus/input/devices block of `dev' against the dump of the real
 * machine: every line equal, but for the HID instance number and inputN
 * (they depend on what registered before) and the handler list. */
static void compare_with_spec(struct input_dev *dev, const char *file)
{
	char want[16][256], got[16][256];
	char out[4096];
	const char *p = out;
	int nw, ng = 0;
	long len;

	CHECK(dev != NULL, "device for %s", file);
	if (!dev)
		return;
	nw = spec_block(file, dev->name, want, 16);
	len = input_format_device(dev, evdev_minor_of(dev->evdev), out,
				  sizeof(out), 0);
	CHECK(len > 0 && len < (long)sizeof(out), "format length");
	out[len > 0 && len < (long)sizeof(out) ? len : 0] = 0;
	while (*p && ng < 16) {
		p = get_line(p, got[ng], 256);
		if (!got[ng][0])
			break;
		ng++;
	}
	CHECK(ng == nw, "%s: %d lines, the dump has %d", dev->name, ng, nw);
	for (int i = 0; i < ng && i < nw; i++) {
		if (!h_strncmp(want[i], "S: ", 3)) {
			const char *w = h_strstr(want[i], "/input/input");
			const char *g = h_strstr(got[i], "/input/input");
			unsigned long pre = w ? (unsigned long)(w - want[i]) - 4 :
						0;

			CHECK(w && g && g - got[i] == w - want[i] &&
				      !h_strncmp(got[i], want[i], pre),
			      "%s: '%s' want '%s'", dev->name, got[i], want[i]);
			continue;
		}
		if (!h_strncmp(want[i], "H: ", 3)) {
			CHECK(h_strstr(got[i], "event") != NULL,
			      "%s: handlers '%s'", dev->name, got[i]);
			continue;
		}
		CHECK(!h_strcmp(got[i], want[i]), "%s: '%s' want '%s'",
		      dev->name, got[i], want[i]);
	}
}

static struct hid_device *cur_hid(void)
{
	return i2c_hid_get_hid(kw_client());
}

static struct i2c_hid_stats stats(void)
{
	struct i2c_hid_stats s;

	h_memset(&s, 0, sizeof(s));
	(void)i2c_hid_get_stats(kw_client(), &s);
	return s;
}

/* The ELAN feature answers hid-multitouch asks for at probe. */
static void elan_features(void)
{
	static const uint8_t f2[] = { 0x02, 0x05 }; // 5 contacts, clickpad
	uint8_t blob[257];

	hidm_set_feature(2, f2, sizeof(f2));
	blob[0] = 6;
	for (int i = 1; i < 257; i++)
		blob[i] = (uint8_t)(i * 7);
	hidm_set_feature(6, blob, sizeof(blob));
}

/* An HP-shaped machine with the ELAN touchpad, bound and opened by the
 * bridge. */
static void hp_up(const struct kw_cfg *over)
{
	struct kw_cfg cfg;

	h_memset(&cfg, 0, sizeof(cfg));
	if (over)
		cfg = *over;
	cfg.machine = KW_HP;
	if (!cfg.rdesc) {
		cfg.rdesc = elan_ptp;
		cfg.rdesc_len = (int)sizeof(elan_ptp);
		cfg.vendor = 0x04f3;
		cfg.product = 0x3169;
	}
	if (!cfg.max_input)
		cfg.max_input = ELAN_TP_LEN + 2;
	kw_setup(&cfg);
	elan_features();
	CHECK(i2c_hid_acpi_init() == 0, "driver registration");
}

static void machine_down(void)
{
	i2c_hid_acpi_exit();
	kw_teardown();
	CHECK(WAIT_FOR(evdev_next_unit(2) == -1, 2000),
	      "event nodes left after teardown");
}

/* Wait until the bridge holds `dev' open (its open work ran). */
static int bridge_open(struct input_dev *dev)
{
	return WAIT_FOR(dev && __atomic_load_n(&dev->kernel_users,
					       __ATOMIC_ACQUIRE) == 1,
			2000);
}

static int inj_sum(int from, int *dx, int *dy, int *wheel)
{
	static struct hw_injection inj[HW_MAX_INJ];
	int n = hw_injections(inj, HW_MAX_INJ);

	*dx = *dy = *wheel = 0;
	for (int i = from; i < n; i++) {
		*dx += inj[i].dx;
		*dy += inj[i].dy;
		*wheel += inj[i].wheel;
	}
	return n;
}

/* Did the injections since `from' press and then release `buttons'? */
static int inj_clicked(int from, uint8_t buttons)
{
	static struct hw_injection inj[HW_MAX_INJ];
	int n = hw_injections(inj, HW_MAX_INJ), pressed = 0;

	for (int i = from; i < n; i++) {
		if ((inj[i].buttons & buttons) == buttons)
			pressed = 1;
		else if (pressed && !(inj[i].buttons & buttons))
			return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// probe
// ---------------------------------------------------------------------------

static void probe_hp(void)
{
	struct hid_device *hdev;
	struct input_dev *tp, *mouse;
	struct evdev_client *c;
	struct input_event e[256];
	char name[96];
	uint8_t f[8];
	int n, irq;

	h_printf("-- HP ProBook-shaped machine\n");
	hw_pref_reset(1, 0); // the firmware's PS/2 emulation of the touchpad
	hp_up(NULL);
	hdev = cur_hid();
	CHECK(hdev != NULL, "bound");
	if (!hdev) {
		machine_down();
		return;
	}
	CHECK(!h_strcmp(hdev->name, "ELAN073B:00 04F3:3169"), "name %s",
	      hdev->name);
	CHECK(!h_strcmp(hdev->phys, "i2c-ELAN073B:00"), "phys %s", hdev->phys);
	CHECK(!h_strcmp(hdev->parent_path,
			"/devices/pci0000:00/0000:00:15.0/i2c_designware.0/"
			"i2c-0/i2c-ELAN073B:00"),
	      "parent %s", hdev->parent_path);
	CHECK(hdev->bus == BUS_I2C && hdev->version == 0x0100 &&
		      hdev->vendor == 0x04f3 && hdev->product == 0x3169,
	      "identity");
	CHECK(hdev->driver && !h_strcmp(hdev->driver->name, "hid-multitouch"),
	      "driver %s", hdev->driver ? hdev->driver->name : "none");
	CHECK(!h_strncmp(hdev->dev_name, "0018:04F3:3169.", 15), "dev %s",
	      hdev->dev_name);

	/* The device's view of the probe. */
	CHECK(hidm_get(HIDM_DESC_READS) == 1, "descriptor reads %d",
	      hidm_get(HIDM_DESC_READS));
	CHECK(hidm_get(HIDM_RDESC_READS) == 1, "report descriptor reads");
	CHECK(hidm_get(HIDM_RESETS) == 1, "resets %d", hidm_get(HIDM_RESETS));
	CHECK(hidm_get(HIDM_RESET_ACKS_SERVED) == 1, "reset acknowledged");
	/* ELAN: no SET_POWER ON after the reset (NO_WAKEUP_AFTER_RESET). */
	CHECK(hidm_get(HIDM_SET_POWER_ON) == 1, "power on %d",
	      hidm_get(HIDM_SET_POWER_ON));
	CHECK(hidm_get(HIDM_POWER) == 0, "device on");
	CHECK(hidm_get(HIDM_PROTO_ERRORS) == 0, "protocol errors %d",
	      hidm_get(HIDM_PROTO_ERRORS));
	CHECK(hidm_feature(3, f, 8) == 2 && f[0] == 3 && f[1] == 3,
	      "input mode set to touchpad");
	CHECK(hidm_feature(7, f, 8) == 2 && f[0] == 7 && f[1] == 3,
	      "surface and button switches on");
	irq = kw_irq();
	CHECK(irq > 0 && kw_irq_trigger(irq) == 0x8u /* IRQ_TYPE_LEVEL_LOW */,
	      "level-low interrupt (%u)", kw_irq_trigger(irq));
	CHECK(i2c_hid_lookup_quirk(0x04f3, 0x3169) ==
		      (I2C_HID_QUIRK_NO_WAKEUP_AFTER_RESET |
		       I2C_HID_QUIRK_BOGUS_IRQ),
	      "ELAN quirks");

	/* The input nodes, against the real machine's dump. */
	CHECK(count_inputs(hdev) == 2, "%d inputs", count_inputs(hdev));
	tp = find_input(hdev, HP_TP);
	mouse = find_input(hdev, HP_MOUSE);
	compare_with_spec(tp, HP_SPEC);
	compare_with_spec(mouse, HP_SPEC);
	CHECK(bridge_open(tp) && bridge_open(mouse), "bridge opened both");
	CHECK(tp && !input_device_user_open(tp), "no user yet");

	/* A frame through the interrupt: the bridge sees it, the PS/2
	 * preference gives the auxiliary port up (its device is no
	 * TrackPoint). */
	hw_injections_clear();
	elan_frame(1, 1000, 1000, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(hidm_wait_drained(2000), "reports read");
	CHECK(WAIT_FOR(stats().reports >= 2, 2000), "reports delivered %lu",
	      stats().reports);
	CHECK(WAIT_FOR(hw_pref_state(NULL, name, sizeof(name)) == 1, 2000),
	      "PS/2 aux verdict");
	CHECK(!h_strcmp(name, HP_TP), "verdict for %s", name);

	/* A user client: the event stream of the touchpad. */
	c = open_input(tp);
	CHECK(tp && input_device_user_open(tp), "user client counted");
	elan_frame(1, 1500, 700, 0);
	n = drain_until(c, e, 256, EV_ABS, ABS_MT_POSITION_X, 1500, 2000);
	CHECK(stream_has(e, n, EV_ABS, ABS_MT_POSITION_X, 1500) &&
		      stream_has(e, n, EV_ABS, ABS_MT_POSITION_Y, 700) &&
		      stream_has(e, n, EV_KEY, BTN_TOUCH, 1) &&
		      stream_has(e, n, EV_KEY, BTN_TOOL_FINGER, 1),
	      "touch stream (%d events)", n);
	if (g_fails)
		for (int i = 0; i < n; i++)
			h_printf("   ev %u %u %d\n", e[i].type, e[i].code,
				 e[i].value);
	elan_frame(0, 0, 0, 0);
	n = drain_until(c, e, 256, EV_KEY, BTN_TOUCH, 0, 2000);
	CHECK(stream_has(e, n, EV_ABS, ABS_MT_TRACKING_ID, -1) &&
		      stream_has(e, n, EV_KEY, BTN_TOUCH, 0),
	      "release stream");
	evdev_release(c);
	CHECK(tp && !input_device_user_open(tp), "user gone");

	machine_down();
	CHECK(hidm_get(HIDM_POWER) == 1, "asleep after removal");
	CHECK(kw_client_power_state() != 0, "ACPI device left D0 (%d)",
	      kw_client_power_state());
}

static void probe_dell(void)
{
	struct kw_cfg cfg;
	struct hid_device *hdev;
	struct input_dev *tp, *mouse;
	static const uint8_t f2[] = { 0x02, 0x05 };
	uint8_t f[8];
	int irq;

	h_printf("-- Dell Pro-shaped machine\n");
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.machine = KW_DELL;
	cfg.rdesc = cirque_ptp;
	cfg.rdesc_len = (int)sizeof(cirque_ptp);
	cfg.vendor = 0x0488;
	cfg.product = 0x1085;
	cfg.max_input = CIRQUE_TP_LEN + 2;
	kw_setup(&cfg);
	hidm_set_feature(2, f2, sizeof(f2));
	CHECK(i2c_hid_acpi_init() == 0, "driver registration");
	hdev = cur_hid();
	CHECK(hdev && !h_strcmp(hdev->name, "VEN_0488:00 0488:1085"), "name");
	CHECK(hdev && !h_strcmp(hdev->parent_path,
				"/devices/pci0000:00/0000:00:15.3/"
				"i2c_designware.1/i2c-1/i2c-VEN_0488:00"),
	      "parent %s", hdev ? hdev->parent_path : "-");
	/* Cirque: no quirk, so SET_POWER ON after the reset as well. */
	CHECK(hidm_get(HIDM_SET_POWER_ON) == 2, "power on %d",
	      hidm_get(HIDM_SET_POWER_ON));
	CHECK(hidm_feature(3, f, 8) == 2 && f[1] == 3, "input mode");
	CHECK(hidm_feature(0x0a, f, 8) == 2 && f[1] == 0x01,
	      "resolution multiplier");
	CHECK(hidm_feature(5, f, 8) == 2 && f[1] == 0, "latency normal");
	irq = kw_irq();
	CHECK(irq > 0 && kw_irq_trigger(irq) == 0x8u /* IRQ_TYPE_LEVEL_LOW */,
	      "GSI interrupt level low");
	tp = find_input(hdev, DELL_TP);
	mouse = find_input(hdev, DELL_MOUSE);
	compare_with_spec(tp, DELL_SPEC);
	compare_with_spec(mouse, DELL_SPEC);
	CHECK(bridge_open(tp), "bridge opened");
	/* Reports through the GSI line. */
	{
		struct contact a[] = { { 1, 1, 9, 2000, 1500 } };
		uint8_t b[32];
		unsigned long before = stats().reports;

		hidm_push_report(b, (int)cirque_tp(b, a, 1, 10, 1, 0));
		hidm_push_report(b, (int)cirque_tp(b, NULL, 0, 20, 0, 0));
		CHECK(WAIT_FOR(stats().reports >= before + 2, 2000),
		      "reports over the GSI");
	}
	machine_down();
}

/* The SIPODEV touchpad of a Teclast F7: no descriptors from the device;
 * the DMI table supplies them. */
static void probe_sipodev(void)
{
	struct kw_cfg cfg;
	struct hid_device *hdev;

	h_printf("-- DMI descriptor override\n");
	kw_dmi_set("TECLAST", "F7");
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.machine = KW_SIPODEV;
	cfg.desc_len = 0xffff; // garbage in the device's own descriptor
	cfg.bcd = 0x1234;
	cfg.max_input = 0x1b;
	cfg.vendor = 0x0911;
	cfg.product = 0x5288;
	kw_setup(&cfg);
	CHECK(i2c_hid_acpi_init() == 0, "driver registration");
	hdev = cur_hid();
	CHECK(hdev && !h_strcmp(hdev->name, "SYNA3602:00 0911:5288"),
	      "bound with the override: %s", hdev ? hdev->name : "none");
	CHECK(hidm_get(HIDM_DESC_READS) == 0 && hidm_get(HIDM_RDESC_READS) == 0,
	      "descriptors not read from the device");
	/* Hantick 5288: no reset acknowledgement expected. */
	CHECK(i2c_hid_lookup_quirk(0x0911, 0x5288) ==
		      (I2C_HID_QUIRK_NO_IRQ_AFTER_RESET |
		       I2C_HID_QUIRK_BAD_INPUT_SIZE),
	      "Hantick quirks");
	{
		struct hid_input *hi;
		int mt = 0;

		if (hdev)
			list_for_each_entry(hi, &hdev->inputs, list)
				if (input_test_bit(ABS_MT_SLOT,
						   hi->input->absbit))
					mt = 1;
		CHECK(mt, "a multitouch node from the override descriptor");
	}
	machine_down();
	kw_dmi_set("HP", "HP ProBook 650 G8 Notebook PC");
}

/* Devices the transport must leave alone (or cannot use). */
static void probe_refused(void)
{
	struct kw_cfg cfg;

	h_printf("-- refused devices\n");
	/* The excluded Chipone id. */
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.hid = "CHPN0001";
	hp_up(&cfg);
	CHECK(!cur_hid() && hidm_get(HIDM_DESC_READS) == 0,
	      "CHPN0001 not probed");
	machine_down();
	/* _DSM failing / out of range. */
	for (int mode = 1; mode <= 2; mode++) {
		h_memset(&cfg, 0, sizeof(cfg));
		cfg.dsm_mode = mode;
		hp_up(&cfg);
		CHECK(!cur_hid(), "_DSM mode %d refused", mode);
		CHECK(kw_irqs_in_use() > 0, "irq table");
		machine_down();
	}
	/* Descriptor version and length. */
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.bcd = 0x0200;
	hp_up(&cfg);
	CHECK(!cur_hid(), "bcdVersion 2.00 refused");
	machine_down();
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.desc_len = 32;
	hp_up(&cfg);
	CHECK(!cur_hid(), "descriptor length 32 refused");
	machine_down();
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.max_input = 1;
	hp_up(&cfg);
	CHECK(!cur_hid(), "wMaxInputLength 1 refused");
	machine_down();
	/* Nothing at the address. */
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.absent = 1;
	hp_up(&cfg);
	CHECK(!cur_hid(), "absent device refused");
	machine_down();
	/* A report descriptor longer than allowed. */
	{
		static uint8_t big[4097];

		h_memset(&cfg, 0, sizeof(cfg));
		h_memcpy(big, elan_ptp, sizeof(elan_ptp));
		cfg.rdesc = big;
		cfg.rdesc_len = (int)sizeof(big);
		cfg.vendor = 0x04f3;
		cfg.product = 0x3169;
		hp_up(&cfg);
		CHECK(!cur_hid(), "report descriptor of 4097 bytes refused");
		machine_down();
	}
	/* A touchpad declaring an Input Mode feature of 65532 bytes under
	 * report ID 20 (the three-byte command form): SET_REPORT of it would
	 * not fit the transport's 16-bit lengths.  The HID core's report
	 * size limit refuses it; nothing may assert. */
	{
		static const uint8_t huge[] = {
			UPAGE(0x0d), USAGE(0x05), COLL(C_APPLICATION),
				RID(0x01), USAGE(0x42), LMIN(0), LMAX(1),
				RSIZE(8), RCOUNT(1), INPUT(0x02),
				USAGE(0x0e), COLL(C_LOGICAL),
					RID(20), USAGE(0x52), LMIN(0),
					LMAX(10), RSIZE2(256), RCOUNT2(2047),
					FEATURE(0x02),
					RSIZE(8), RCOUNT(28), FEATURE(0x02),
				ENDC,
			ENDC,
		};
		long warns = test_warning_count();

		h_memset(&cfg, 0, sizeof(cfg));
		cfg.rdesc = huge;
		cfg.rdesc_len = (int)sizeof(huge);
		cfg.vendor = 0x04f3;
		cfg.product = 0x3169;
		hp_up(&cfg);
		CHECK(!cur_hid() || hidm_get(HIDM_PROTO_ERRORS) == 0,
		      "65532-byte feature report: no malformed command");
		CHECK(test_warning_count() == warns,
		      "65532-byte feature report: no kernel warning");
		machine_down();
	}
}

/* The GPIO controller comes after the I2C-HID driver. */
static void probe_deferred(void)
{
	struct kw_cfg cfg;

	h_printf("-- deferred interrupt\n");
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.gpio_defer = 1;
	hp_up(&cfg);
	CHECK(!cur_hid(), "deferred while the GPIO controller is missing");
	kw_gpio_arrives();
	CHECK(cur_hid() != NULL, "bound once it is there");
	machine_down();
}

static void test_probe(void)
{
	h_printf("== probe\n");
	probe_hp();
	probe_dell();
	probe_sipodev();
	probe_refused();
	probe_deferred();
}

// ---------------------------------------------------------------------------
// protocol
// ---------------------------------------------------------------------------

/* A device with a vendor feature of report ID 0x20 (beyond the 4-bit
 * field) and an output report. */
static const uint8_t proto_rdesc[] = {
	UPAGE2(0xff00), USAGE(0x01), COLL(C_APPLICATION),
		RID(0x01), USAGE(0x02), LMIN(0), LMAX2(255), RSIZE(8),
		RCOUNT(4), INPUT(0x02),
		RID(0x03), USAGE(0x03), RCOUNT(3), FEATURE(0x02),
		RID(0x20), USAGE(0x04), RCOUNT(5), FEATURE(0x02),
		RID(0x21), USAGE(0x05), RCOUNT(2), OUTPUT(0x02),
	ENDC,
};

static void test_protocol(void)
{
	struct kw_cfg cfg;
	struct hid_device *hdev;
	uint8_t buf[16], out[16];
	int rc, via = 0;

	h_printf("== protocol\n");
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.rdesc = proto_rdesc;
	cfg.rdesc_len = (int)sizeof(proto_rdesc);
	cfg.vendor = 0x1234;
	cfg.product = 0x5678;
	cfg.max_input = 7;
	hp_up(&cfg);
	/* No HID driver wants a device with nothing to map (no raw HID
	 * node here): the transport's requests work all the same. */
	hdev = cur_hid();
	CHECK(hdev != NULL, "transport bound");
	if (!hdev) {
		machine_down();
		return;
	}

	/* GET_REPORT, ID 3 (in the 4-bit field). */
	{
		static const uint8_t a3[] = { 0x03, 0xaa, 0xbb, 0xcc };

		hidm_set_feature(3, a3, sizeof(a3));
		h_memset(buf, 0, sizeof(buf));
		rc = hid_hw_raw_request(hdev, 3, buf, 4, HID_FEATURE_REPORT,
					HID_REQ_GET_REPORT);
		CHECK(rc == 4 && buf[0] == 3 && buf[1] == 0xaa &&
			      buf[3] == 0xcc,
		      "GET 3: %d", rc);
	}
	/* GET_REPORT, ID 0x20: the 0xF form. */
	{
		static const uint8_t a20[] = { 0x20, 1, 2, 3, 4, 5 };
		int before = hidm_get(HIDM_LONG_ID_CMDS);

		hidm_set_feature(0x20, a20, sizeof(a20));
		h_memset(buf, 0, sizeof(buf));
		rc = hid_hw_raw_request(hdev, 0x20, buf, 6, HID_FEATURE_REPORT,
					HID_REQ_GET_REPORT);
		CHECK(rc == 6 && buf[0] == 0x20 && buf[5] == 5, "GET 0x20: %d",
		      rc);
		CHECK(hidm_get(HIDM_LONG_ID_CMDS) == before + 1,
		      "third ID byte used");
	}
	/* A device answering with the wrong ID. */
	{
		static const uint8_t bad[] = { 0x04, 9, 9, 9 };

		hidm_set_feature(3, bad, sizeof(bad));
		rc = hid_hw_raw_request(hdev, 3, buf, 4, HID_FEATURE_REPORT,
					HID_REQ_GET_REPORT);
		CHECK(rc == -EINVAL, "wrong ID refused: %d", rc);
	}
	/* SET_REPORT, ID 0x20. */
	buf[0] = 0x20;
	for (int i = 1; i < 6; i++)
		buf[i] = (uint8_t)(0x40 + i);
	rc = hid_hw_raw_request(hdev, 0x20, buf, 6, HID_FEATURE_REPORT,
				HID_REQ_SET_REPORT);
	CHECK(rc == 6, "SET 0x20: %d", rc);
	CHECK(hidm_feature(0x20, out, 16) == 6 && out[0] == 0x20 &&
		      out[5] == 0x45,
	      "SET 0x20 data");
	/* SET_REPORT whose buffer does not start with its ID. */
	buf[0] = 0x21;
	rc = hid_hw_raw_request(hdev, 0x20, buf, 6, HID_FEATURE_REPORT,
				HID_REQ_SET_REPORT);
	CHECK(rc == -EINVAL, "SET with a mismatched ID: %d", rc);
	/* Output report through the output register. */
	buf[0] = 0x21;
	buf[1] = 0x5a;
	buf[2] = 0xa5;
	rc = hid_hw_output_report(hdev, buf, 3);
	CHECK(rc == 3, "output report: %d", rc);
	CHECK(hidm_output(out, 16, &via) == 3 && via && out[0] == 0x21 &&
		      out[2] == 0xa5,
	      "output report on the output register");
	/* SET_REPORT of an output report: the data register. */
	rc = hid_hw_raw_request(hdev, 0x21, buf, 3, HID_OUTPUT_REPORT,
				HID_REQ_SET_REPORT);
	CHECK(rc == 3 && hidm_output(out, 16, &via) == 3 && !via,
	      "SET_REPORT output: %d", rc);
	/* Requests the transport refuses. */
	CHECK(hid_hw_raw_request(hdev, 1, buf, 4, HID_INPUT_REPORT,
				 HID_REQ_SET_REPORT) == -EINVAL,
	      "SET of an input report");
	CHECK(hid_hw_raw_request(hdev, 0x21, buf, 4, HID_OUTPUT_REPORT,
				 HID_REQ_GET_REPORT) == -EINVAL,
	      "GET of an output report");
	CHECK(hid_hw_raw_request(hdev, 1, buf, 4, HID_INPUT_REPORT,
				 HID_REQ_GET_IDLE) == -EIO,
	      "other requests");
	CHECK(hidm_get(HIDM_PROTO_ERRORS) == 0, "no malformed command (%d)",
	      hidm_get(HIDM_PROTO_ERRORS));
	machine_down();
}

// ---------------------------------------------------------------------------
// faults
// ---------------------------------------------------------------------------

static void test_faults(void)
{
	struct kw_cfg cfg;
	struct i2c_hid_stats s0, s1;
	uint8_t f[8];
	unsigned long r0;

	h_printf("== faults\n");

	/* No reset acknowledgement: the probe goes on after its wait. */
	{
		unsigned long long t0;

		h_memset(&cfg, 0, sizeof(cfg));
		kw_setup(&(struct kw_cfg){ .machine = KW_HP,
					    .rdesc = elan_ptp,
					    .rdesc_len = (int)sizeof(elan_ptp),
					    .vendor = 0x04f3,
					    .product = 0x3169,
					    .max_input = ELAN_TP_LEN + 2 });
		elan_features();
		hidm_set(HIDM_NO_RESET_ACK, 1);
		t0 = h_monotonic_ns();
		CHECK(i2c_hid_acpi_init() == 0, "registration");
		CHECK(cur_hid() != NULL, "bound without a reset ack");
		CHECK(h_monotonic_ns() - t0 >= 900000000ULL,
		      "waited for the acknowledgement");
		machine_down();
	}

	hp_up(NULL);
	CHECK(cur_hid() != NULL, "bound");
	CHECK(bridge_open(find_input(cur_hid(), HP_TP)), "bridge open");

	/* 0xffff lengths of an ELAN (BOGUS_IRQ): dropped silently. */
	s0 = stats();
	hidm_set(HIDM_BOGUS, 5);
	CHECK(WAIT_FOR(stats().bogus >= s0.bogus + 5, 2000), "bogus %lu",
	      stats().bogus);
	s1 = stats();
	CHECK(s1.read_errors == s0.read_errors && s1.bad_length == s0.bad_length,
	      "bogus lengths are no errors");

	/* Lengths beyond the read, below 2: dropped, counted. */
	hidm_set(HIDM_BAD_LEN, 2);
	elan_frame(1, 100, 100, 0);
	elan_frame(1, 110, 100, 0);
	CHECK(WAIT_FOR(stats().bad_length >= s1.bad_length + 2, 2000),
	      "oversized lengths %lu", stats().bad_length);
	hidm_set(HIDM_SHORT_LEN, 1);
	elan_frame(1, 120, 100, 0);
	CHECK(WAIT_FOR(stats().bad_length >= s1.bad_length + 3, 2000),
	      "short length");
	r0 = stats().reports;
	elan_frame(0, 0, 0, 0);
	CHECK(WAIT_FOR(stats().reports == r0 + 1, 2000),
	      "a good report after the bad ones");

	/* Failing reads below the recovery threshold. */
	s0 = stats();
	hidm_set(HIDM_NACK_READS, 3);
	elan_frame(1, 200, 200, 0);
	CHECK(WAIT_FOR(stats().reports == s0.reports + 1, 3000),
	      "delivered after 3 failed reads");
	CHECK(stats().read_errors >= s0.read_errors + 3, "read errors %lu",
	      stats().read_errors);
	CHECK(stats().recoveries == s0.recoveries, "no recovery for 3");

	/* Garbage. */
	hidm_seed(77);
	hidm_set(HIDM_GARBAGE, 20);
	CHECK(WAIT_FOR(hidm_get(HIDM_LINE) == 0, 3000), "garbage consumed");
	elan_frame(0, 0, 0, 0);
	CHECK(hidm_wait_drained(3000), "drained after garbage");

	/* A run of failures: the device is reset and configured again. */
	s0 = stats();
	hidm_set(HIDM_NACK_READS, 12);
	elan_frame(1, 300, 300, 0);
	CHECK(WAIT_FOR(stats().recoveries >= s0.recoveries + 1, 5000),
	      "recovered by reset (%lu)", stats().recoveries);
	CHECK(WAIT_FOR(hidm_feature(3, f, 8) == 2 && f[1] == 3, 3000),
	      "input mode sent again");
	CHECK(WAIT_FOR(kw_irq_depth(kw_irq()) == 0, 2000),
	      "interrupt unmasked after the recovery (%d)",
	      kw_irq_depth(kw_irq()));
	r0 = stats().reports;
	elan_frame(1, 310, 300, 0);
	CHECK(WAIT_FOR(stats().reports > r0, 2000), "reports flow again");

	/* The device resets itself: configured again. */
	s0 = stats();
	hidm_set(HIDM_SPONTANEOUS_RESET, 1);
	CHECK(WAIT_FOR(stats().reinits == s0.reinits + 1, 3000),
	      "device reset answered (%lu)", stats().reinits);
	CHECK(WAIT_FOR(hidm_feature(3, f, 8) == 2 && f[1] == 3, 3000),
	      "configured after its own reset");

	machine_down();

	/* A reset storm: configured again a bounded number of times. */
	hp_up(NULL);
	s0 = stats();
	for (int i = 0; i < 10; i++) {
		hidm_set(HIDM_SPONTANEOUS_RESET, 1);
		WAIT_FOR(hidm_get(HIDM_LINE) == 0, 1000);
		h_usleep(5000);
	}
	CHECK(WAIT_FOR(test_wq_pending() == 0, 3000), "storm settled");
	CHECK(stats().reinits >= 1 && stats().reinits <= 3,
	      "re-configured %lu times for 10 resets", stats().reinits);
	CHECK(stats().spurious >= 10, "all seen (%lu)", stats().spurious);
	r0 = stats().reports;
	elan_frame(1, 400, 400, 0);
	CHECK(WAIT_FOR(stats().reports > r0, 2000), "still working");
	machine_down();

	/* Late acknowledgement: the probe waits a second, the
	 * acknowledgement then comes as a device reset. */
	h_memset(&cfg, 0, sizeof(cfg));
	kw_setup(&(struct kw_cfg){ .machine = KW_HP, .rdesc = elan_ptp,
				    .rdesc_len = (int)sizeof(elan_ptp),
				    .vendor = 0x04f3, .product = 0x3169,
				    .max_input = ELAN_TP_LEN + 2 });
	elan_features();
	hidm_set(HIDM_LATE_RESET_ACK_MS, 1300);
	CHECK(i2c_hid_acpi_init() == 0, "registration");
	CHECK(cur_hid() != NULL, "bound with a late acknowledgement");
	hidm_set(HIDM_LATE_RESET_ACK_MS, 0);
	CHECK(WAIT_FOR(hidm_get(HIDM_RESET_ACKS_SERVED) == 1, 3000),
	      "late acknowledgement read");
	machine_down();

	/* The device disappears while reporting: recovery fails, gives up,
	 * and the removal still works. */
	hp_up(NULL);
	CHECK(bridge_open(find_input(cur_hid(), HP_TP)), "bridge open");
	for (int i = 0; i < 5; i++)
		elan_frame(1, 500 + i, 500, 0);
	hidm_set(HIDM_GONE, 1);
	/* The line is low with the device gone; keep it busy with an
	 * asserted line (as a broken pad would) through the reads. */
	hidm_set(HIDM_ASLEEP_REPORTS, 1);
	s0 = stats();
	for (int i = 0; i < 40; i++) {
		hidm_set(HIDM_GONE, 0);
		elan_frame(1, 600, 600, 0);
		hidm_set(HIDM_GONE, 1);
		h_usleep(2000);
	}
	h_usleep(200000);
	CHECK(stats().read_errors > s0.read_errors, "reads failed (%lu)",
	      stats().read_errors);
	machine_down();
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

static void test_openclose(void)
{
	struct input_dev *tp;
	struct evdev_client *c;
	uint8_t f[8];
	unsigned long r0;

	h_printf("== openclose\n");
	hp_up(NULL);
	tp = find_input(cur_hid(), HP_TP);
	CHECK(bridge_open(tp), "bridge open");
	CHECK(hidm_feature(7, f, 8) == 2 && f[1] == 3, "reporting on");

	/* Without the bridge nobody has the device open: hid-multitouch
	 * turns reporting off, the transport delivers nothing. */
	mousedev_exit();
	CHECK(tp && tp->kernel_users == 0, "bridge gone");
	CHECK(hidm_feature(7, f, 8) == 2 && f[1] == 0, "reporting off (%02x)",
	      f[1]);
	r0 = stats().reports;
	elan_frame(1, 100, 100, 0);
	CHECK(hidm_wait_drained(2000), "read anyway");
	h_usleep(20000);
	CHECK(stats().reports == r0, "nothing delivered while closed");

	/* A user client opens it: on again. */
	c = open_input(tp);
	CHECK(hidm_feature(7, f, 8) == 2 && f[1] == 3, "reporting on again");
	elan_frame(1, 110, 100, 0);
	CHECK(WAIT_FOR(stats().reports == r0 + 1, 2000), "delivered");
	evdev_release(c);
	CHECK(hidm_feature(7, f, 8) == 2 && f[1] == 0, "off after close");

	/* The bridge again. */
	mousedev_init();
	CHECK(bridge_open(tp), "bridge back");
	CHECK(hidm_feature(7, f, 8) == 2 && f[1] == 3, "on with the bridge");
	machine_down();
}

// ---------------------------------------------------------------------------
// power management
// ---------------------------------------------------------------------------

static void test_pm(void)
{
	struct kw_cfg cfg;
	uint8_t f[8];
	unsigned long r0;
	int irq, resets;

	h_printf("== pm\n");
	hp_up(NULL);
	CHECK(bridge_open(find_input(cur_hid(), HP_TP)), "bridge open");
	irq = kw_irq();

	CHECK(kw_suspend_all() == 0, "suspend");
	CHECK(hidm_get(HIDM_POWER) == 1, "device asleep");
	CHECK(kw_irq_depth(irq) == 1, "interrupt masked (%d)",
	      kw_irq_depth(irq));
	CHECK(kw_irq_wake_enabled(irq), "wake armed (I2C_CLIENT_WAKE)");
	/* mt_suspend: high latency, nothing reported (no wakeup driver). */
	r0 = stats().reports;
	elan_frame(1, 100, 100, 0);
	h_usleep(20000);
	CHECK(stats().reports == r0, "nothing while suspended");

	/* Resume with a device that NACKs its first address after sleep. */
	hidm_set(HIDM_NACK_WAKE, 1);
	CHECK(kw_resume_all() == 0, "resume");
	CHECK(hidm_get(HIDM_NACKS) >= 1, "the wake-up NACK happened");
	CHECK(hidm_get(HIDM_POWER) == 0, "device on");
	CHECK(kw_irq_depth(irq) == 0, "interrupt unmasked");
	CHECK(!kw_irq_wake_enabled(irq), "wake disarmed");
	CHECK(hidm_feature(3, f, 8) == 2 && f[1] == 3, "configured after resume");
	elan_frame(1, 120, 100, 0);
	CHECK(WAIT_FOR(stats().reports > r0, 2000), "reports after resume");
	hidm_set(HIDM_NACK_WAKE, 0);
	machine_down();

	/* ALPS: reset on resume. */
	h_memset(&cfg, 0, sizeof(cfg));
	cfg.rdesc = elan_ptp;
	cfg.rdesc_len = (int)sizeof(elan_ptp);
	cfg.vendor = 0x044e;
	cfg.product = 0x1234;
	hp_up(&cfg);
	CHECK(cur_hid() != NULL, "ALPS-like bound");
	resets = hidm_get(HIDM_RESETS);
	CHECK(kw_suspend_all() == 0 && kw_resume_all() == 0, "cycle");
	CHECK(hidm_get(HIDM_RESETS) == resets + 1, "reset on resume");
	CHECK(hidm_feature(3, f, 8) == 2 && f[1] == 3, "configured");
	machine_down();

	/* Shutdown: asleep, interrupt released. */
	hp_up(NULL);
	irq = kw_irq();
	kw_shutdown_all();
	CHECK(hidm_get(HIDM_POWER) == 1, "asleep at shutdown");
	CHECK(kw_irq_depth(irq) == 0, "interrupt released");
	machine_down();
}

// ---------------------------------------------------------------------------
// the suspend test chain (/proc/i2c "suspend-test")
// ---------------------------------------------------------------------------

static void test_suspendtest(void)
{
	struct kw_pm_snap snap;
	static char buf[16384];
	unsigned long r0, gr0;
	uint8_t f[8];
	int irq, sleeps, ons;

	h_printf("== suspendtest\n");
	hp_up(NULL);
	CHECK(bridge_open(find_input(cur_hid(), HP_TP)), "bridge open");
	irq = kw_irq();
	sleeps = hidm_get(HIDM_SET_POWER_SLEEP);
	ons = hidm_get(HIDM_SET_POWER_ON);
	gr0 = kw_gpio_resumes();

	/* The whole chain: the touchpad asleep with its line masked and
	 * armed for wake, the controller suspended with its LPSS function
	 * in reset, then the GPIO controllers; back in reverse. */
	CHECK(kw_suspend_test(30, &snap) == 0, "suspend test");
	CHECK(snap.taken, "GPIO controllers suspended last");
	CHECK(snap.client_suspended == 1, "client suspended before GPIO");
	CHECK(snap.adapter_suspended == 1, "adapter suspended before GPIO");
	CHECK(snap.controller_off == 1, "controller off, LPSS in reset");
	CHECK(snap.irq_depth == 1, "line masked while asleep (%d)",
	      snap.irq_depth);
	CHECK(snap.irq_wake == 1, "wake armed while asleep");
	CHECK(snap.device_power == 1, "device asleep (SET_POWER SLEEP)");
	CHECK(hidm_get(HIDM_SET_POWER_SLEEP) == sleeps + 1, "one SLEEP");
	CHECK(hidm_get(HIDM_SET_POWER_ON) >= ons + 1, "ON again");
	CHECK(kw_gpio_resumes() == gr0 + 1, "GPIO controllers resumed");
	CHECK(hidm_get(HIDM_POWER) == 0, "device on");
	CHECK(kw_irq_depth(irq) == 0, "line unmasked");
	CHECK(!kw_irq_wake_enabled(irq), "wake disarmed");
	CHECK(hidm_feature(3, f, 8) == 2 && f[1] == 3,
	      "configured again after resume");
	r0 = stats().reports;
	elan_frame(1, 140, 120, 0);
	CHECK(WAIT_FOR(stats().reports > r0, 2000), "reports after the test");

	/* The trigger as user space writes it, and /proc/i2c afterwards. */
	CHECK(kw_proc_i2c_write("suspend-test 10\n") == 16, "write");
	CHECK(kw_proc_i2c_write("suspend-test ten") < 0, "bad argument");
	CHECK(kw_proc_i2c(buf, sizeof(buf)) > 0, "read /proc/i2c");
	CHECK(h_strstr(buf, "suspend-test: runs") != NULL, "test record");
	CHECK(h_strstr(buf, "i2c-hid: reports") != NULL,
	      "the transport's recovery counters");
	CHECK(h_strstr(buf, "reinits") != NULL, "the controller's counters");
	r0 = stats().reports;
	elan_frame(1, 150, 120, 0);
	CHECK(WAIT_FOR(stats().reports > r0, 2000), "reports after the write");
	machine_down();
}

// ---------------------------------------------------------------------------
// removal while reports stream
// ---------------------------------------------------------------------------

struct streamer {
	volatile int stop;
	int frames;
};

static void *stream_thread(void *p)
{
	struct streamer *s = p;

	while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
		elan_frame(1 + s->frames % 3, 100 + (s->frames * 7) % 2000,
			   200 + (s->frames * 3) % 1500, s->frames & 1);
		s->frames++;
		h_usleep(300);
	}
	test_thread_done();
	return NULL;
}

static void test_remove(void)
{
	h_printf("== remove\n");
	for (int round = 0; round < 4; round++) {
		struct streamer s = { .stop = 0, .frames = 0 };
		unsigned long th;
		struct evdev_client *c = NULL;

		hp_up(NULL);
		CHECK(cur_hid() != NULL, "bound");
		if (round & 1)
			c = open_input(find_input(cur_hid(), HP_TP));
		h_thread_start(stream_thread, &s, &th);
		h_usleep(20000 + round * 15000);
		/* Unbind with reports arriving; the client stays open
		 * across the removal in odd rounds. */
		i2c_hid_acpi_exit();
		CHECK(kw_client() && !cur_hid(), "unbound");
		__atomic_store_n(&s.stop, 1, __ATOMIC_RELEASE);
		h_thread_join(th);
		if (c) {
			struct input_event e[64];

			(void)drain(c, e, 64);
			CHECK(evdev_read(c, e, sizeof(e), 1) == -ENODEV,
			      "client sees the device gone");
			evdev_release(c);
		}
		kw_teardown();
		CHECK(WAIT_FOR(evdev_next_unit(2) == -1, 2000), "nodes gone");
		CHECK(s.frames > 0, "streamed %d frames", s.frames);
	}
}

// ---------------------------------------------------------------------------
// the console pointer bridge
// ---------------------------------------------------------------------------

static void test_mousedev(void)
{
	struct input_dev *tp;
	struct evdev_client *c, *mc;
	struct input_event e[512];
	int from, dx, dy, wheel, n;

	h_printf("== mousedev\n");
	hp_up(NULL);
	tp = find_input(cur_hid(), HP_TP);
	CHECK(bridge_open(tp), "bridge open");
	mc = open_input(NULL);
	{
		struct evdev_client *m1 = NULL;

		CHECK(evdev_open(EVDEV_UNIT_MOUSE, &m1) == 0, "open event1");
		mc = m1;
	}

	/* One finger moves right 100 units a packet.  The first two packets
	 * of a touch move nothing; from the third on each moves by the
	 * distance of the last two packets: 200 * 256 / 3209 = 15.95 units,
	 * 15 and a remainder after the third, 31 after the fourth. */
	from = hw_injections(NULL, 0);
	elan_frame(1, 1000, 1000, 0);
	elan_frame(1, 1100, 1000, 0);
	CHECK(hidm_wait_drained(2000), "drained");
	h_usleep(20000);
	inj_sum(from, &dx, &dy, &wheel);
	CHECK(dx == 0 && dy == 0, "two packets: no motion yet (%d %d)", dx, dy);
	elan_frame(1, 1200, 1000, 0);
	CHECK(hidm_wait_drained(2000), "drained");
	CHECK(WAIT_FOR((inj_sum(from, &dx, &dy, &wheel), dx >= 15), 2000),
	      "moved right: dx %d", dx);
	CHECK(dx == 15 && dy == 0 && wheel == 0, "dx %d dy %d wheel %d", dx,
	      dy, wheel);
	elan_frame(1, 1300, 1000, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(hidm_wait_drained(2000), "drained");
	CHECK(WAIT_FOR((inj_sum(from, &dx, &dy, &wheel), dx >= 31), 2000),
	      "moved on: dx %d", dx);
	CHECK(dx == 31 && dy == 0 && wheel == 0, "dx %d dy %d wheel %d", dx,
	      dy, wheel);
	CHECK(!inj_clicked(from, 1), "a move is no tap");
	n = drain_until(mc, e, 512, EV_REL, REL_X, 0, 300);
	{
		int sum = 0;

		for (int i = 0; i < n; i++)
			if (e[i].type == EV_REL && e[i].code == REL_X)
				sum += e[i].value;
		CHECK(sum == dx, "event1 REL_X %d = injected %d", sum, dx);
	}

	/* A tap: left click. */
	from = hw_injections(NULL, 0);
	elan_frame(1, 800, 800, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(WAIT_FOR(inj_clicked(from, 1), 2000), "tap = left click");
	n = drain_until(mc, e, 512, EV_KEY, BTN_LEFT, 0, 1000);
	CHECK(stream_has(e, n, EV_KEY, BTN_LEFT, 1) &&
		      stream_has(e, n, EV_KEY, BTN_LEFT, 0),
	      "event1 click");

	/* Two-finger tap: right click. */
	from = hw_injections(NULL, 0);
	elan_frame(2, 800, 800, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(WAIT_FOR(inj_clicked(from, 2), 2000), "two-finger tap = right");

	/* Two fingers up: the wheel turns away from the user. */
	from = hw_injections(NULL, 0);
	elan_frame(2, 800, 1600, 0);
	for (int y = 1500; y >= 900; y -= 100)
		elan_frame(2, 800, y, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(hidm_wait_drained(2000), "drained");
	CHECK(WAIT_FOR((inj_sum(from, &dx, &dy, &wheel), wheel >= 4), 2000),
	      "two-finger scroll: wheel %d", wheel);
	CHECK(dx == 0 && dy == 0, "no pointer motion while scrolling (%d %d)",
	      dx, dy);
	if (dx || dy) {
		static struct hw_injection inj[HW_MAX_INJ];
		int k = hw_injections(inj, HW_MAX_INJ);

		for (int i = from; i < k; i++)
			h_printf("   inj %d %d %u %d\n", inj[i].dx, inj[i].dy,
				 inj[i].buttons, inj[i].wheel);
	}

	/* The clickpad pressed with one finger: left; with two: right. */
	from = hw_injections(NULL, 0);
	elan_frame(1, 900, 900, 0);
	h_usleep(250000); // no tap
	elan_frame(1, 900, 900, 1);
	elan_frame(1, 900, 900, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(WAIT_FOR(inj_clicked(from, 1), 2000), "clickpad left");
	from = hw_injections(NULL, 0);
	elan_frame(2, 900, 900, 0);
	elan_frame(2, 900, 900, 1);
	elan_frame(2, 900, 900, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(WAIT_FOR(inj_clicked(from, 2), 2000), "clickpad right");

	/* A user client on the touchpad: the bridge falls silent and lets
	 * go of a held button. */
	elan_frame(1, 900, 900, 0);
	h_usleep(250000);
	elan_frame(1, 900, 900, 1); // pressed, held
	CHECK(WAIT_FOR(({
			       struct hw_injection last[HW_MAX_INJ];
			       int k = hw_injections(last, HW_MAX_INJ);
			       k > 0 && (last[k - 1].buttons & 1);
		       }),
		       2000),
	      "button held");
	c = open_input(tp);
	{
		struct hw_injection inj[HW_MAX_INJ];
		int k = hw_injections(inj, HW_MAX_INJ);

		CHECK(k > 0 && inj[k - 1].buttons == 0,
		      "released when the client came");
	}
	from = hw_injections(NULL, 0);
	elan_frame(1, 900, 900, 0);
	elan_frame(1, 1300, 900, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(hidm_wait_drained(2000), "drained");
	n = drain_until(c, e, 512, EV_KEY, BTN_TOUCH, 0, 2000);
	CHECK(stream_has(e, n, EV_ABS, ABS_MT_POSITION_X, 1300),
	      "the client gets the touchpad");
	h_usleep(20000);
	CHECK(hw_injections(NULL, 0) == from, "bridge silent (%d injections)",
	      hw_injections(NULL, 0) - from);
	evdev_release(c);

	/* Back to the bridge after the client closed. */
	from = hw_injections(NULL, 0);
	elan_frame(1, 1000, 1000, 0);
	elan_frame(1, 1150, 1000, 0);
	elan_frame(1, 1300, 1000, 0);
	elan_frame(0, 0, 0, 0);
	CHECK(WAIT_FOR((inj_sum(from, &dx, &dy, &wheel), dx > 0), 2000),
	      "bridge back: dx %d", dx);

	evdev_release(mc);
	machine_down();
}

// ---------------------------------------------------------------------------
// PS/2 preference
// ---------------------------------------------------------------------------

static struct input_dev *fake_dev(int bus, int touchpad, int direct, int rel)
{
	struct input_dev *d = input_allocate_device();

	d->id.bustype = (uint16_t)bus;
	if (touchpad || direct) {
		input_set_abs_params(d, ABS_MT_POSITION_X, 0, 3000, 0, 0);
		input_set_abs_params(d, ABS_MT_POSITION_Y, 0, 2000, 0, 0);
		input_mt_init_slots(d, 5,
				    direct ? INPUT_MT_DIRECT :
					     INPUT_MT_POINTER);
	}
	if (rel) {
		input_set_capability(d, EV_REL, REL_X);
		input_set_capability(d, EV_REL, REL_Y);
		input_set_capability(d, EV_KEY, BTN_LEFT);
	}
	return d;
}

static void test_ps2pref(void)
{
	struct mousedev_ps2_pref p;
	struct input_dev *d;
	int v;

	h_printf("== ps2pref\n");
	/* Classification. */
	d = fake_dev(BUS_I2C, 1, 0, 0);
	CHECK(mousedev_is_i2c_touchpad(d) &&
		      mousedev_device_kind(d) == MOUSEDEV_KIND_TOUCHPAD,
	      "I2C touchpad");
	input_free_device(d);
	d = fake_dev(BUS_USB, 1, 0, 0);
	CHECK(!mousedev_is_i2c_touchpad(d) &&
		      mousedev_device_kind(d) == MOUSEDEV_KIND_TOUCHPAD,
	      "USB touchpad: bridged, but no PS/2 preference");
	input_free_device(d);
	d = fake_dev(BUS_I2C, 0, 1, 0);
	CHECK(!mousedev_is_i2c_touchpad(d) &&
		      mousedev_device_kind(d) == MOUSEDEV_KIND_NONE,
	      "touchscreen: neither");
	input_free_device(d);
	d = fake_dev(BUS_I2C, 0, 0, 1);
	CHECK(!mousedev_is_i2c_touchpad(d) &&
		      mousedev_device_kind(d) == MOUSEDEV_KIND_MOUSE,
	      "HID mouse collection");
	input_free_device(d);
	d = fake_dev(BUS_I2C, 0, 0, 0);
	input_set_capability(d, EV_KEY, KEY_A);
	CHECK(mousedev_device_kind(d) == MOUSEDEV_KIND_NONE &&
		      !mousedev_is_i2c_touchpad(d),
	      "keyboard");
	input_free_device(d);

	/* No PS/2 pointing device at boot: nothing to do, ever. */
	mousedev_ps2_pref_init(&p, 0, 0);
	CHECK(mousedev_ps2_pref_touchpad(&p, 0) == MOUSEDEV_PS2_PASS &&
		      mousedev_ps2_pref_touchpad(&p, 1) == MOUSEDEV_PS2_PASS &&
		      mousedev_ps2_pref_touchpad(&p, 1) == MOUSEDEV_PS2_PASS,
	      "no PS/2 device: no verdict");
	mousedev_ps2_pref_init(&p, 0, 1);
	CHECK(mousedev_ps2_pref_touchpad(&p, 1) == MOUSEDEV_PS2_PASS,
	      "no PS/2 device (stale TrackPoint flag): no verdict");

	/* A generic PS/2 device (the firmware's emulation): disabled at the
	 * touchpad's first real report, once. */
	mousedev_ps2_pref_init(&p, 1, 0);
	CHECK(mousedev_ps2_pref_touchpad(&p, 0) == MOUSEDEV_PS2_PASS,
	      "a frame without contact is no verdict");
	CHECK(mousedev_ps2_pref_touchpad(&p, 1) == MOUSEDEV_PS2_DISABLE,
	      "first real report disables");
	CHECK(mousedev_ps2_pref_touchpad(&p, 1) == MOUSEDEV_PS2_PASS &&
		      mousedev_ps2_pref_touchpad(&p, 0) == MOUSEDEV_PS2_PASS,
	      "only once");

	/* A TrackPoint: never, whatever the touchpad does. */
	mousedev_ps2_pref_init(&p, 1, 1);
	v = MOUSEDEV_PS2_PASS;
	for (int i = 0; i < 1000; i++)
		v |= mousedev_ps2_pref_touchpad(&p, i & 1);
	CHECK(v == MOUSEDEV_PS2_PASS, "TrackPoint never disabled");
}

// ---------------------------------------------------------------------------
// stress
// ---------------------------------------------------------------------------

static void test_stress(void)
{
	char *env = h_getenv("TESTI2CHID_STRESS_SECS");
	int secs = env ? (int)h_strtoull(env, NULL, 10) : 20;
	unsigned long long end;
	struct evdev_client *c = NULL;
	unsigned long ops = 0, rebinds = 0, suspends = 0, delivered = 0;
	unsigned int seed = 0x5eed;

	h_printf("== stress (%d s)\n", secs);
	hp_up(NULL);
	h_srandom(seed);
	hidm_seed(seed);
	end = h_monotonic_ns() + (unsigned long long)secs * 1000000000ULL;
	while (h_monotonic_ns() < end) {
		int what = (int)(h_random() % 100);

		ops++;
		if (what < 50) {
			elan_frame((int)(h_random() % 6),
				   (int)(h_random() % 3200),
				   (int)(h_random() % 2100),
				   (int)(h_random() & 1));
		} else if (what < 58) {
			static const int faults[] = { HIDM_BOGUS, HIDM_BAD_LEN,
						      HIDM_SHORT_LEN,
						      HIDM_NACK_READS,
						      HIDM_GARBAGE };
			hidm_set(faults[h_random() % 5],
				 (int)(1 + h_random() % 4));
		} else if (what < 60) {
			hidm_set(HIDM_SPONTANEOUS_RESET, 1);
		} else if (what < 70) {
			if (c) {
				evdev_release(c);
				c = NULL;
			} else if (cur_hid()) {
				c = open_input(find_input(cur_hid(), HP_TP));
			}
		} else if (what < 72) {
			CHECK(kw_suspend_all() == 0, "suspend");
			elan_frame(1, 5, 5, 0);
			CHECK(kw_resume_all() == 0, "resume");
			suspends++;
		} else if (what < 74) {
			if (c) {
				evdev_release(c);
				c = NULL;
			}
			delivered += stats().reports;
			i2c_hid_acpi_exit();
			CHECK(!cur_hid(), "unbound");
			CHECK(i2c_hid_acpi_init() == 0, "rebound");
			rebinds++;
		} else if (c) {
			struct input_event e[256];

			(void)drain(c, e, 256);
		}
		h_usleep((unsigned int)(h_random() % 1500));
	}
	if (c)
		evdev_release(c);
	delivered += stats().reports;
	h_printf("   %lu operations, %lu rebinds, %lu suspend cycles, %lu "
		 "reports delivered\n",
		 ops, rebinds, suspends, delivered);
	CHECK(delivered > 0, "reports got through during the stress");
	/* Still alive and well: a clean frame gets through. */
	hidm_set(HIDM_BOGUS, 0);
	hidm_set(HIDM_BAD_LEN, 0);
	hidm_set(HIDM_SHORT_LEN, 0);
	hidm_set(HIDM_NACK_READS, 0);
	hidm_set(HIDM_GARBAGE, 0);
	if (cur_hid()) {
		struct input_dev *tp = find_input(cur_hid(), HP_TP);
		unsigned long r0;

		CHECK(bridge_open(tp), "bridge open at the end");
		CHECK(WAIT_FOR(test_wq_pending() == 0, 5000), "work settled");
		r0 = stats().reports;
		elan_frame(1, 50, 50, 0);
		CHECK(WAIT_FOR(stats().reports > r0, 4000),
		      "reports after the stress");
	}
	machine_down();
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : "all";
	int all = h_strcmp(only, "all") == 0;
	long kw0;

	test_set_clock_ns(0);
	kw_adopt();
	kw_dmi_set("HP", "HP ProBook 650 G8 Notebook PC");
	evdev_builtin_init();
	hw_wq_start();
	hid_core_init();
	mousedev_init();
	hw_pref_reset(0, 0);
	kw0 = kw_warnings();

	if (all || !h_strcmp(only, "probe"))
		test_probe();
	if (all || !h_strcmp(only, "protocol"))
		test_protocol();
	if (all || !h_strcmp(only, "faults"))
		test_faults();
	if (all || !h_strcmp(only, "openclose"))
		test_openclose();
	if (all || !h_strcmp(only, "pm"))
		test_pm();
	if (all || !h_strcmp(only, "suspendtest"))
		test_suspendtest();
	if (all || !h_strcmp(only, "remove"))
		test_remove();
	if (all || !h_strcmp(only, "mousedev"))
		test_mousedev();
	if (all || !h_strcmp(only, "ps2pref"))
		test_ps2pref();
	if (all || !h_strcmp(only, "stress"))
		test_stress();

	mousedev_exit();
	hw_wq_stop();
	CHECK(test_warning_count() == 0, "%ld kernel warnings (HID world)",
	      test_warning_count());
	CHECK(kw_warnings() == kw0, "%ld kernel warnings (bus world)",
	      kw_warnings() - kw0);
	test_thread_done();
	h_printf("%s: %ld checks, %ld failed\n", g_fails ? "FAIL" : "PASS",
		 g_checks, g_fails);
	return g_fails ? 1 : 0;
}
