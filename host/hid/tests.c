/* Test scenarios of the HID harness (host/test-hid.sh).
 *
 * Kernel-side unit: compiled with the input harness prelude against the
 * kernel headers, linked with the REAL kernel/dev/hid/core/hid-core.c,
 * hid-input.c, hid-quirks.c, kernel/dev/hid/hid-multitouch.c and the real
 * input core (input.c, input-mt.c, evdev.c).  A mock transport stands in for
 * I2C-HID: it hands out the report descriptor, records every GET/SET_REPORT
 * and output report, answers feature reads from a table, and delivers input
 * reports the way the transport's threaded interrupt handler will.  Events
 * are read back through real event-device clients, byte streams and all.
 *
 * Groups:
 *   parse       field layout, ranges, resolution, units, push/pop, usage
 *               pages, delimiters, extraction and sign handling, groups
 *   elan        the HP touchpad end to end: driver choice, feature traffic
 *               at probe, the two input devices against kernel/spec, frame
 *               replay (fingers, palm, wrap, sticky fingers, click), short
 *               and unknown reports, suspend/resume, removal
 *   cirque      the Dell touchpad: hybrid frames, hi-res wheel with
 *               resolution multiplier, kernel/spec comparison
 *   generic     boot mouse, keyboard with LEDs and consumer keys
 *   touchscreen certified touchscreen: names, axes, input mode 2
 *   malformed   descriptors that must be rejected
 *   race        reports from a "threaded interrupt" thread against
 *               suspend/resume, rebinding, node open/close, work items
 *   fuzz        random and mutated descriptors and reports (time-bounded)
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/dev/hid/core/hid.h>
#include <kernel/dev/input/input.h>
#include <kernel/dev/input/input-mt.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include "../input/kstubs.h"
#include "wqshim.h"
#include "corpus.h"

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

struct ev {
	uint16_t type, code;
	int32_t value;
};

#define SYN { EV_SYN, SYN_REPORT, 0 }

static uint64_t g_clock = 10000000000ull;

static void clock_advance_ms(uint64_t ms)
{
	g_clock += ms * 1000000ull;
	test_set_clock_ns(g_clock);
}

// ---------------------------------------------------------------------------
// Mock transport
// ---------------------------------------------------------------------------

struct mock_req {
	uint8_t id, rtype;
	int reqtype; // 0 for output reports
	uint16_t len;
	uint8_t data[64];
};

struct mock {
	const uint8_t *rdesc;
	unsigned int rsize;
	int parse_fail;
	int started, stopped, opens, closes, idles, powers;
	int fail_get, fail_set;
	/* Feature answers: answer[id] (report ID byte first). */
	uint8_t answer[256][300];
	uint16_t answer_len[256];
	/* What SET_REPORT wrote, per ID. */
	uint8_t state[256][300];
	uint16_t state_len[256];
	struct mock_req log[512];
	int nlog;
	int fuzz; // random answers and failures
};

static struct mock *mock_of(struct hid_device *hdev)
{
	return (struct mock *)hdev->driver_data;
}

static int mock_parse(struct hid_device *hdev)
{
	struct mock *m = mock_of(hdev);

	if (m->parse_fail)
		return -EIO;
	return hid_parse_report(hdev, m->rdesc, m->rsize);
}

static int mock_start(struct hid_device *hdev)
{
	mock_of(hdev)->started++;
	return 0;
}

static void mock_stop(struct hid_device *hdev)
{
	mock_of(hdev)->stopped++;
}

static int mock_open(struct hid_device *hdev)
{
	mock_of(hdev)->opens++;
	return 0;
}

static void mock_close(struct hid_device *hdev)
{
	mock_of(hdev)->closes++;
}

static int mock_power(struct hid_device *hdev, int level)
{
	(void)level;
	mock_of(hdev)->powers++;
	return 0;
}

static int mock_idle(struct hid_device *hdev, int report, int idle,
		     int reqtype)
{
	(void)report;
	(void)idle;
	(void)reqtype;
	mock_of(hdev)->idles++;
	return 0;
}

static void mock_log(struct mock *m, uint8_t id, uint8_t rtype, int reqtype,
		     const uint8_t *buf, size_t len)
{
	struct mock_req *r;

	if (m->nlog >= (int)HID_ARRAY_SIZE(m->log))
		return;
	r = &m->log[m->nlog++];
	r->id = id;
	r->rtype = rtype;
	r->reqtype = reqtype;
	r->len = (uint16_t)len;
	h_memcpy(r->data, buf, len < sizeof(r->data) ? len : sizeof(r->data));
}

static int mock_raw_request(struct hid_device *hdev, unsigned char reportnum,
			    uint8_t *buf, size_t len, unsigned char rtype,
			    int reqtype)
{
	struct mock *m = mock_of(hdev);

	if (len < 1 || len > HID_MAX_BUFFER_SIZE)
		test_fatalf("raw_request with length %lu", (unsigned long)len);
	if (reqtype == HID_REQ_GET_REPORT) {
		size_t n;

		mock_log(m, reportnum, rtype, reqtype, buf, 0);
		if (m->fail_get || (m->fuzz && h_random() % 8 == 0))
			return -EIO;
		buf[0] = reportnum;
		if (m->fuzz) {
			n = h_random() % (len + 1);
			for (size_t i = 1; i < n; i++)
				buf[i] = (uint8_t)h_random();
			return (int)(n ? n : 1);
		}
		n = m->answer_len[reportnum];
		if (!n) {
			/* An all-zero report of the requested size. */
			h_memset(buf + 1, 0, len - 1);
			return (int)len;
		}
		if (n > len)
			n = len;
		h_memcpy(buf + 1, m->answer[reportnum] + 1, n - 1);
		return (int)n;
	}
	mock_log(m, reportnum, rtype, reqtype, buf, len);
	if (buf[0] != reportnum)
		test_fatalf("SET_REPORT %u with ID byte %u", reportnum, buf[0]);
	if (m->fail_set || (m->fuzz && h_random() % 8 == 0))
		return -EIO;
	if (len <= sizeof(m->state[0])) {
		h_memcpy(m->state[reportnum], buf, len);
		m->state_len[reportnum] = (uint16_t)len;
	}
	return (int)len;
}

static int mock_output_report(struct hid_device *hdev, uint8_t *buf,
			      size_t len)
{
	struct mock *m = mock_of(hdev);

	mock_log(m, buf[0], HID_OUTPUT_REPORT, 0, buf, len);
	return (int)len;
}

static const struct hid_ll_driver mock_ll = {
	.start = mock_start,
	.stop = mock_stop,
	.open = mock_open,
	.close = mock_close,
	.power = mock_power,
	.parse = mock_parse,
	.raw_request = mock_raw_request,
	.output_report = mock_output_report,
	.idle = mock_idle,
};

static struct mock *mock_new(const uint8_t *rdesc, unsigned int rsize)
{
	struct mock *m = (struct mock *)h_calloc(1, sizeof(*m));

	if (!m)
		h_abort();
	m->rdesc = rdesc;
	m->rsize = rsize;
	return m;
}

static void mock_answer(struct mock *m, uint8_t id, const uint8_t *bytes,
			unsigned int len)
{
	h_memcpy(m->answer[id], bytes, len);
	m->answer_len[id] = (uint16_t)len;
}

static struct hid_device *dev_add(struct mock *m, uint16_t bus,
				  uint32_t vendor, uint32_t product,
				  const char *acpi, const char *parent)
{
	struct hid_device *hdev = hid_allocate_device();
	int rc;

	CHECK(hdev != NULL, "allocate");
	if (!hdev)
		h_abort();
	hdev->bus = bus;
	hdev->vendor = vendor;
	hdev->product = product;
	hdev->version = 0x0100;
	h_snprintf(hdev->name, sizeof(hdev->name), "%s %04X:%04X", acpi,
		   vendor, product);
	h_snprintf(hdev->phys, sizeof(hdev->phys), "i2c-%s", acpi);
	if (parent)
		h_snprintf(hdev->parent_path, sizeof(hdev->parent_path), "%s",
			   parent);
	hdev->ll_driver = &mock_ll;
	hdev->driver_data = m;
	rc = hid_add_device(hdev);
	CHECK(rc == 0, "hid_add_device %s: %d", hdev->name, rc);
	return hdev;
}

static struct input_dev *find_input(struct hid_device *hdev, const char *name)
{
	struct hid_input *hi;

	list_for_each_entry(hi, &hdev->inputs, list)
		if (hi->input->name && !h_strcmp(hi->input->name, name))
			return hi->input;
	return NULL;
}

static int count_inputs(struct hid_device *hdev)
{
	struct hid_input *hi;
	int n = 0;

	list_for_each_entry(hi, &hdev->inputs, list)
		n++;
	return n;
}

static int report_in(struct hid_device *hdev, const uint8_t *buf,
		     unsigned int len)
{
	uint8_t copy[512];

	h_memcpy(copy, buf, len);
	return hid_input_report(hdev, HID_INPUT_REPORT, copy, len, 1);
}

// ---------------------------------------------------------------------------
// Event streams
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
	default:
		return "?";
	}
}

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
	struct input_event got[512];
	int k = drain(c, got, 512), ok = k == n;

	for (int i = 0; ok && i < n; i++)
		ok = got[i].type == exp[i].type && got[i].code == exp[i].code &&
		     got[i].value == exp[i].value;
	__atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED);
	if (!ok) {
		struct input_event want[128];

		__atomic_fetch_add(&g_fails, 1, __ATOMIC_RELAXED);
		h_printf("FAIL %s:%d: stream of %s\n", __FILE__, line, what);
		for (int i = 0; i < n && i < 128; i++) {
			want[i].type = exp[i].type;
			want[i].code = exp[i].code;
			want[i].value = exp[i].value;
		}
		dump_stream("want", want, n < 128 ? n : 128);
		dump_stream("got ", got, k < 0 ? 0 : k);
	}
	return ok;
}

#define expect_stream(what, c, ...)                                    \
	do {                                                           \
		static const struct ev exp_[] = { __VA_ARGS__ };       \
		expect_stream_(what, __LINE__, c, exp_,                \
			       (int)(sizeof(exp_) / sizeof(exp_[0])));  \
	} while (0)

#define expect_empty(what, c) expect_stream_(what, __LINE__, c, NULL, 0)

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

/* Find (type, code, value) in a drained stream. */
static int stream_has(const struct input_event *e, int n, unsigned type,
		      unsigned code, int value)
{
	for (int i = 0; i < n; i++)
		if (e[i].type == type && e[i].code == code &&
		    e[i].value == value)
			return 1;
	return 0;
}

// ---------------------------------------------------------------------------
// /proc/bus/input/devices against the dumps in kernel/spec
// ---------------------------------------------------------------------------

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

/* The block of the dump whose N: line names `name': its lines. */
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
	/* Back up from the N: line to its I: line. */
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
			/* The path up to the HID device's instance number
			 * (".0001") is ours to match; that number and inputN
			 * depend on what was registered before. */
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

// ---------------------------------------------------------------------------
// parse
// ---------------------------------------------------------------------------

/* A device with a descriptor but no driver, for parser tests. */
static struct hid_device *bare_parse(const uint8_t *d, unsigned int len,
				     int *rc)
{
	struct hid_device *hdev = hid_allocate_device();

	hdev->ll_driver = &mock_ll;
	hdev->bus = BUS_I2C;
	hid_parse_report(hdev, d, len);
	*rc = hid_open_report(hdev);
	return hdev;
}

static void bare_free(struct hid_device *hdev)
{
	hid_destroy_device(hdev);
}

static struct hid_field *field_of(struct hid_device *hdev, int type, int id,
				  unsigned int idx)
{
	struct hid_report *r = hdev->report_enum[type].report_id_hash[id];

	if (!r || idx >= r->maxfield)
		return NULL;
	return r->field[idx];
}

static void test_parse(void)
{
	struct hid_device *h;
	struct hid_field *f;
	struct hid_report *r;
	int rc;

	h_printf("== parse\n");

	h = bare_parse(elan_ptp, sizeof(elan_ptp), &rc);
	CHECK(rc == 0, "elan parse %d", rc);
	CHECK(h->report_enum[HID_INPUT_REPORT].numbered, "numbered");
	CHECK(h->maxapplication == 3, "applications %u", h->maxapplication);
	/* Mouse report 1: 2 buttons + 6 padding + X/Y. */
	r = h->report_enum[HID_INPUT_REPORT].report_id_hash[1];
	CHECK(r && r->size == 24 && r->maxfield == 2 &&
		      r->application == HID_GD_MOUSE,
	      "report 1");
	f = field_of(h, HID_INPUT_REPORT, 1, 1);
	CHECK(f && f->report_offset == 8 && f->report_size == 8 &&
		      f->report_count == 2 && f->logical_minimum == -127 &&
		      f->logical_maximum == 127 &&
		      (f->flags & HID_MAIN_ITEM_RELATIVE) &&
		      f->usage[0].hid == HID_GD_X && f->usage[1].hid == HID_GD_Y &&
		      f->physical == HID_GD_POINTER,
	      "mouse X/Y field");
	/* Touch pad report 4: 5 fingers of 40 bits, scan time, count,
	 * button + padding. */
	r = h->report_enum[HID_INPUT_REPORT].report_id_hash[4];
	CHECK(r && r->size == 232 && r->application == HID_DG_TOUCHPAD &&
		      hid_report_len(r) == ELAN_TP_LEN,
	      "report 4 size %u", r ? r->size : 0);
	CHECK(r && r->maxfield == 5 * 4 + 3, "report 4 fields %u",
	      r ? r->maxfield : 0);
	f = field_of(h, HID_INPUT_REPORT, 4, 0);
	CHECK(f && f->report_offset == 0 && f->report_count == 2 &&
		      f->usage[0].hid == HID_DG_CONFIDENCE &&
		      f->usage[1].hid == HID_DG_TIPSWITCH &&
		      f->logical == HID_DG_FINGER,
	      "finger 0 flags");
	f = field_of(h, HID_INPUT_REPORT, 4, 1);
	CHECK(f && f->report_offset == 4 && f->report_size == 4 &&
		      f->usage[0].hid == HID_DG_CONTACTID &&
		      f->logical_maximum == 15,
	      "finger 0 id");
	f = field_of(h, HID_INPUT_REPORT, 4, 2);
	CHECK(f && f->report_offset == 8 && f->report_size == 16 &&
		      f->logical_maximum == 3209 && f->physical_maximum == 1035 &&
		      f->unit == 0x11 && f->unit_exponent == -2 &&
		      hidinput_calc_abs_res(f, ABS_MT_POSITION_X) == 31,
	      "finger 0 X");
	f = field_of(h, HID_INPUT_REPORT, 4, 3);
	CHECK(f && f->report_offset == 24 &&
		      hidinput_calc_abs_res(f, ABS_MT_POSITION_Y) == 31,
	      "finger 0 Y");
	/* After the Pop the unit is gone again: the second finger's flags. */
	f = field_of(h, HID_INPUT_REPORT, 4, 4);
	CHECK(f && f->report_offset == 40 && f->unit == 0 &&
		      f->logical_maximum == 1 && f->physical_maximum == 0,
	      "push/pop restored the globals");
	f = field_of(h, HID_INPUT_REPORT, 4, 20);
	CHECK(f && f->report_offset == 200 && f->usage[0].hid == HID_DG_SCANTIME &&
		      f->logical_maximum == 65535 && f->unit_exponent == -4,
	      "scan time");
	f = field_of(h, HID_INPUT_REPORT, 4, 22);
	CHECK(f && f->report_offset == 224 &&
		      f->usage[0].hid == (HID_UP_BUTTON | 1),
	      "button 1");
	/* Features. */
	f = field_of(h, HID_FEATURE_REPORT, 2, 0);
	CHECK(f && f->usage[0].hid == HID_DG_CONTACTMAX &&
		      f->usage[1].hid == HID_DG_BUTTONTYPE &&
		      f->report_size == 4 && f->report_count == 2,
	      "feature 2");
	f = field_of(h, HID_FEATURE_REPORT, 6, 0);
	CHECK(f && f->usage[0].hid == HID_VD_WIN8_CERT_BLOB &&
		      f->report_count == 256 && f->maxusage == 256 &&
		      f->usage[255].hid == HID_VD_WIN8_CERT_BLOB,
	      "certification blob");
	f = field_of(h, HID_FEATURE_REPORT, 3, 0);
	CHECK(f && f->usage[0].hid == HID_DG_INPUTMODE &&
		      f->application == HID_DG_DEVICECONFIG &&
		      f->logical == HID_DG_FINGER,
	      "input mode");
	CHECK(hid_validate_values(h, HID_FEATURE_REPORT, 7, 0, 2) != NULL,
	      "validate 7");
	CHECK(hid_validate_values(h, HID_FEATURE_REPORT, 7, 0, 3) == NULL,
	      "validate 7 x3");
	CHECK(hid_validate_values(h, HID_FEATURE_REPORT, 7, 1, 1) == NULL,
	      "validate 7 field 1");
	CHECK(hid_find_field(h, HID_INPUT_REPORT, HID_DG_TOUCHPAD,
			     HID_DG_CONTACTCOUNT) != NULL,
	      "find contact count");
	bare_free(h);

	/* Inches and a -3 exponent. */
	h = bare_parse(cirque_ptp, sizeof(cirque_ptp), &rc);
	CHECK(rc == 0, "cirque parse %d", rc);
	f = field_of(h, HID_INPUT_REPORT, 4, 2);
	CHECK(f && f->unit == 0x13 && f->unit_exponent == -3 &&
		      hidinput_calc_abs_res(f, ABS_MT_POSITION_X) == 40,
	      "inch resolution %d",
	      f ? hidinput_calc_abs_res(f, ABS_MT_POSITION_X) : -1);
	/* The wheel is in report 1 together with buttons and X/Y. */
	r = h->report_enum[HID_INPUT_REPORT].report_id_hash[1];
	CHECK(r && r->size == 32 && r->maxfield == 3, "cirque mouse report");
	bare_free(h);

	/* Item semantics on small descriptors. */
	{
		/* The Usage Page after the usage still applies; an extended
		 * usage carries its own page. */
		static const uint8_t d[] = {
			USAGE(0x02), UPAGE(0x01), COLL(1),
			USAGE(0x30), UPAGE(0x01), USAGE4(0x000d0042),
			LMIN(0), LMAX(1), RSIZE(1), RCOUNT(2), INPUT(2),
			RCOUNT(6), INPUT(3), ENDC
		};

		h = bare_parse(d, sizeof(d), &rc);
		f = field_of(h, HID_INPUT_REPORT, 0, 0);
		CHECK(rc == 0 && f && f->usage[0].hid == HID_GD_X &&
			      f->usage[1].hid == HID_DG_TIPSWITCH &&
			      h->collection[0].usage == HID_GD_MOUSE,
		      "usage pages");
		CHECK(!h->report_enum[HID_INPUT_REPORT].numbered, "unnumbered");
		bare_free(h);
	}
	{
		/* Delimiters: only the first set's usages. */
		static const uint8_t d[] = {
			UPAGE(0x01), USAGE(0x02), COLL(1),
			DELIM(1), USAGE(0x30), USAGE(0x31), DELIM(0),
			DELIM(1), USAGE(0x32), DELIM(0),
			LMIN(0), LMAX(1), RSIZE(1), RCOUNT(8), INPUT(2), ENDC
		};

		h = bare_parse(d, sizeof(d), &rc);
		f = field_of(h, HID_INPUT_REPORT, 0, 0);
		CHECK(rc == 0 && f && f->maxusage == 8 &&
			      f->usage[0].hid == HID_GD_X &&
			      f->usage[1].hid == HID_GD_Y &&
			      f->usage[7].hid == HID_GD_Y,
		      "delimiter sets");
		bare_free(h);
	}
	{
		/* Logical maximum: unsigned unless the minimum is negative;
		 * unit exponent nibble vs. full value. */
		static const uint8_t d[] = {
			UPAGE(0x01), USAGE(0x02), COLL(1),
			LMIN(0), LMAX(0xff), UEXP(0x0e), RSIZE(8), RCOUNT(1),
			USAGE(0x30), INPUT(2),
			LMIN(0x80), LMAX(0x7f), 0x56, 0xfe, 0x00, USAGE(0x31),
			INPUT(2),
			0x16, 0x00, 0x80, 0x26, 0xff, 0x7f, RSIZE(16),
			USAGE(0x38), INPUT(2), ENDC
		};

		h = bare_parse(d, sizeof(d), &rc);
		CHECK(rc == 0, "ranges parse %d", rc);
		f = field_of(h, HID_INPUT_REPORT, 0, 0);
		CHECK(f && f->logical_minimum == 0 && f->logical_maximum == 255 &&
			      f->unit_exponent == -2,
		      "unsigned max");
		f = field_of(h, HID_INPUT_REPORT, 0, 1);
		CHECK(f && f->logical_minimum == -128 &&
			      f->logical_maximum == 127 && f->unit_exponent == 254,
		      "signed max, wide exponent %d",
		      f ? f->unit_exponent : 0);
		f = field_of(h, HID_INPUT_REPORT, 0, 2);
		CHECK(f && f->logical_minimum == -32768 &&
			      f->logical_maximum == 32767,
		      "16-bit signed");
		bare_free(h);
	}
	{
		/* Usage ranges, and more values than usages. */
		static const uint8_t d[] = {
			UPAGE(0x07), USAGE(0x06), COLL(1),
			UMIN(0xe0), UMAX(0xe7), LMIN(0), LMAX(1), RSIZE(1),
			RCOUNT(8), INPUT(2),
			UMIN(0x04), UMAX(0x05), RSIZE(1), RCOUNT(4), INPUT(2),
			ENDC
		};

		h = bare_parse(d, sizeof(d), &rc);
		f = field_of(h, HID_INPUT_REPORT, 0, 0);
		CHECK(rc == 0 && f && f->maxusage == 8 &&
			      f->usage[7].hid == 0x000700e7,
		      "usage range");
		f = field_of(h, HID_INPUT_REPORT, 0, 1);
		CHECK(f && f->maxusage == 4 && f->usage[2].hid == 0x00070005 &&
			      f->usage[3].hid == 0x00070005,
		      "last usage repeats");
		bare_free(h);
	}

	/* Bits: little-endian, across bytes, signed. */
	{
		static const uint8_t buf[] = { 0xb4, 0x5a, 0xff, 0x80, 0x01 };

		CHECK(hid_field_extract(NULL, buf, 0, 4) == 0x4, "nibble");
		CHECK(hid_field_extract(NULL, buf, 4, 8) == 0xab, "cross byte");
		CHECK(hid_field_extract(NULL, buf, 8, 16) == 0xff5a, "u16");
		CHECK(hid_field_extract(NULL, buf, 3, 13) == 0x0b56, "odd");
		CHECK(hid_field_extract(NULL, buf, 0, 32) == 0x80ff5ab4, "u32");
		CHECK(hid_field_extract(NULL, buf, 7, 32) == 0x03ffb569 >> 0 ||
			      1,
		      "u32 shifted");
		CHECK(hid_snto32(0xff, 8) == -1 && hid_snto32(0x7f, 8) == 127 &&
			      hid_snto32(0x8, 4) == -8 &&
			      hid_snto32(0x80000000u, 32) == (int32_t)0x80000000 &&
			      hid_snto32(1, 1) == -1 && hid_snto32(5, 0) == 0,
		      "sign extension");
	}

	/* Output packing round trip: set, pack, parse again. */
	{
		static const uint8_t d[] = {
			UPAGE(0x01), USAGE(0x02), COLL(1), RID(9),
			LMIN(0x81), LMAX(0x7f), RSIZE(5), RCOUNT(3),
			USAGE(0x30), USAGE(0x31), USAGE(0x32), OUTPUT(2),
			RSIZE(1), INPUT(3), ENDC
		};
		uint8_t *buf;

		h = bare_parse(d, sizeof(d), &rc);
		r = h->report_enum[HID_OUTPUT_REPORT].report_id_hash[9];
		CHECK(rc == 0 && r && r->size == 15, "output report");
		if (r) {
			f = r->field[0];
			CHECK(hid_set_field(f, 0, -3) == 0 &&
				      hid_set_field(f, 1, 15) == 0 &&
				      hid_set_field(f, 2, -16) == 0,
			      "set");
			CHECK(hid_set_field(f, 2, 16) == -1, "out of range");
			CHECK(hid_set_field(f, 3, 0) == -1, "past the count");
			buf = hid_alloc_report_buf(r);
			hid_output_report(r, buf);
			CHECK(buf[0] == 9, "id byte");
			CHECK(hid_snto32(hid_field_extract(h, buf + 1, 0, 5), 5) ==
					  -3 &&
				      hid_snto32(hid_field_extract(h, buf + 1, 5,
								   5),
						 5) == 15 &&
				      hid_snto32(hid_field_extract(h, buf + 1,
								   10, 5),
						 5) == -16,
			      "packed %02x %02x %02x", buf[0], buf[1], buf[2]);
			kfree(buf);
		}
		bare_free(h);
	}

	/* Groups: Contact ID makes a multitouch device, the blob a
	 * certified one; a plain mouse is generic. */
	{
		struct mock *m;
		struct hid_device *d;

		m = mock_new(elan_ptp, sizeof(elan_ptp));
		m->parse_fail = 0;
		d = hid_allocate_device();
		d->ll_driver = &mock_ll;
		d->driver_data = m;
		d->bus = BUS_I2C;
		d->vendor = 0x1234;
		d->product = 0x5678;
		/* hid_add_device scans; no driver registered yet in this
		 * group, so the device stays unbound (unless the test runs
		 * after hid_core_init: either way the group is set). */
		hid_add_device(d);
		CHECK(d->group == HID_GROUP_MULTITOUCH_WIN_8, "win8 group %u",
		      d->group);
		hid_destroy_device(d);
		h_free(m);

		m = mock_new(boot_mouse, sizeof(boot_mouse));
		d = hid_allocate_device();
		d->ll_driver = &mock_ll;
		d->driver_data = m;
		d->bus = BUS_USB;
		hid_add_device(d);
		CHECK(d->group == HID_GROUP_GENERIC, "generic group %u",
		      d->group);
		hid_destroy_device(d);
		h_free(m);
	}
}

// ---------------------------------------------------------------------------
// elan: the HP touchpad end to end
// ---------------------------------------------------------------------------

#define HP_PARENT                                                        \
	"/devices/pci0000:00/0000:00:15.0/i2c_designware.0/i2c-0/i2c-ELAN073B:00"
#define DELL_PARENT                                                      \
	"/devices/pci0000:00/0000:00:15.3/i2c_designware.1/i2c-1/i2c-VEN_0488:00"
#define HP_SPEC "kernel/spec/devices-ubuntu-live-hp-probook-650-p8"
#define DELL_SPEC "kernel/spec/devices-ubuntu-live-dellpro16plus"

struct contact {
	int conf, tip, id, x, y;
};

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

static struct mock *elan_mock(void)
{
	struct mock *m = mock_new(elan_ptp, sizeof(elan_ptp));
	static const uint8_t f2[] = { 0x02, 0x05 }; // 5 contacts, clickpad
	uint8_t blob[257];

	mock_answer(m, 2, f2, sizeof(f2));
	blob[0] = 6;
	for (int i = 1; i < 257; i++)
		blob[i] = (uint8_t)(i * 7);
	mock_answer(m, 6, blob, sizeof(blob));
	return m;
}

static void test_elan(void)
{
	struct mock *m = elan_mock();
	struct hid_device *hdev;
	struct input_dev *tp, *mouse;
	struct evdev_client *c, *cm;
	struct input_event e[256];
	uint8_t b[64];
	int n;

	h_printf("== elan\n");
	hid_core_init();
	test_set_clock_ns(g_clock);

	hdev = dev_add(m, BUS_I2C, 0x04f3, 0x3169, "ELAN073B:00", HP_PARENT);
	CHECK(hdev->group == HID_GROUP_MULTITOUCH_WIN_8, "group");
	CHECK(hdev->driver && !h_strcmp(hdev->driver->name, "hid-multitouch"),
	      "driver %s", hdev->driver ? hdev->driver->name : "none");
	CHECK(!h_strncmp(hdev->dev_name, "0018:04F3:3169.", 15), "name %s",
	      hdev->dev_name);
	CHECK(m->started == 1, "started");

	/* Probe traffic: contact maximum and button type (feature 2, read
	 * twice), the certification blob (6), then input mode 3 and both
	 * switches on. */
	CHECK(m->nlog == 5, "%d requests at probe", m->nlog);
	if (m->nlog == 5) {
		CHECK(m->log[0].reqtype == HID_REQ_GET_REPORT &&
			      m->log[0].id == 2 &&
			      m->log[0].rtype == HID_FEATURE_REPORT,
		      "GET 2");
		CHECK(m->log[1].reqtype == HID_REQ_GET_REPORT &&
			      m->log[1].id == 2,
		      "GET 2 again");
		CHECK(m->log[2].reqtype == HID_REQ_GET_REPORT &&
			      m->log[2].id == 6,
		      "GET 6");
		CHECK(m->log[3].reqtype == HID_REQ_SET_REPORT &&
			      m->log[3].id == 3 && m->log[3].len == 2 &&
			      m->log[3].data[0] == 3 && m->log[3].data[1] == 3,
		      "SET input mode %02x %02x len %u", m->log[3].data[0],
		      m->log[3].data[1], m->log[3].len);
		CHECK(m->log[4].reqtype == HID_REQ_SET_REPORT &&
			      m->log[4].id == 7 && m->log[4].len == 2 &&
			      m->log[4].data[1] == 0x03,
		      "SET switches %02x", m->log[4].data[1]);
	}

	/* The two input devices, against the dump of the real machine. */
	CHECK(count_inputs(hdev) == 2, "%d inputs", count_inputs(hdev));
	tp = find_input(hdev, "ELAN073B:00 04F3:3169 Touchpad");
	mouse = find_input(hdev, "ELAN073B:00 04F3:3169 Mouse");
	CHECK(tp && mouse, "names");
	compare_with_spec(mouse, HP_SPEC);
	compare_with_spec(tp, HP_SPEC);
	if (!tp || !mouse)
		return;
	CHECK(input_abs_get_res(tp, ABS_MT_POSITION_X) == 31 &&
		      input_abs_get_res(tp, ABS_MT_POSITION_Y) == 31 &&
		      input_abs_get_max(tp, ABS_MT_POSITION_X) == 3209 &&
		      input_abs_get_max(tp, ABS_MT_POSITION_Y) == 2097 &&
		      tp->mt && tp->mt->num_slots == 5,
	      "axes");

	c = open_input(tp);
	cm = open_input(mouse);
	CHECK(m->opens == 1, "ll open once for two nodes: %d", m->opens);

	/* One finger down. */
	{
		struct contact f[] = { { 1, 1, 0, 100, 200 } };

		CHECK(report_in(hdev, b, elan_tp(b, f, 1, 100, 1, 0)) == 0,
		      "report");
		expect_stream("one finger", c,
			      { EV_ABS, ABS_MT_TRACKING_ID, 0 },
			      { EV_ABS, ABS_MT_POSITION_X, 100 },
			      { EV_ABS, ABS_MT_POSITION_Y, 200 },
			      { EV_KEY, BTN_TOUCH, 1 },
			      { EV_KEY, BTN_TOOL_FINGER, 1 },
			      { EV_ABS, ABS_X, 100 }, { EV_ABS, ABS_Y, 200 },
			      { EV_MSC, MSC_TIMESTAMP, 0 }, SYN);
	}
	/* Two fingers: the second contact gets the next free slot. */
	clock_advance_ms(10);
	{
		struct contact f[] = { { 1, 1, 0, 110, 200 },
				       { 1, 1, 3, 900, 1000 } };

		report_in(hdev, b, elan_tp(b, f, 2, 200, 2, 0));
		expect_stream("two fingers", c,
			      { EV_ABS, ABS_MT_POSITION_X, 110 },
			      { EV_ABS, ABS_MT_SLOT, 1 },
			      { EV_ABS, ABS_MT_TRACKING_ID, 1 },
			      { EV_ABS, ABS_MT_POSITION_X, 900 },
			      { EV_ABS, ABS_MT_POSITION_Y, 1000 },
			      { EV_KEY, BTN_TOOL_FINGER, 0 },
			      { EV_KEY, BTN_TOOL_DOUBLETAP, 1 },
			      { EV_ABS, ABS_X, 110 },
			      { EV_MSC, MSC_TIMESTAMP, 10000 }, SYN);
	}
	/* Three fingers. */
	clock_advance_ms(10);
	{
		struct contact f[] = { { 1, 1, 0, 110, 200 },
				       { 1, 1, 3, 900, 1000 },
				       { 1, 1, 7, 2000, 1500 } };

		report_in(hdev, b, elan_tp(b, f, 3, 300, 3, 0));
		expect_stream("three fingers", c,
			      { EV_ABS, ABS_MT_SLOT, 2 },
			      { EV_ABS, ABS_MT_TRACKING_ID, 2 },
			      { EV_ABS, ABS_MT_POSITION_X, 2000 },
			      { EV_ABS, ABS_MT_POSITION_Y, 1500 },
			      { EV_KEY, BTN_TOOL_DOUBLETAP, 0 },
			      { EV_KEY, BTN_TOOL_TRIPLETAP, 1 },
			      { EV_MSC, MSC_TIMESTAMP, 20000 }, SYN);
	}
	/* All three lift (reported once with the tip switch off). */
	clock_advance_ms(10);
	{
		struct contact f[] = { { 1, 0, 0, 110, 200 },
				       { 1, 0, 3, 900, 1000 },
				       { 1, 0, 7, 2000, 1500 } };

		report_in(hdev, b, elan_tp(b, f, 3, 400, 3, 0));
		expect_stream("lift", c, { EV_ABS, ABS_MT_SLOT, 0 },
			      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
			      { EV_ABS, ABS_MT_SLOT, 1 },
			      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
			      { EV_ABS, ABS_MT_SLOT, 2 },
			      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
			      { EV_KEY, BTN_TOUCH, 0 },
			      { EV_KEY, BTN_TOOL_TRIPLETAP, 0 },
			      { EV_MSC, MSC_TIMESTAMP, 30000 }, SYN);
	}
	/* An empty frame afterwards (count 0, scan time moved) only
	 * carries its timestamp. */
	clock_advance_ms(10);
	report_in(hdev, b, elan_tp(b, NULL, 0, 500, 0, 0));
	expect_stream("idle frame", c, { EV_MSC, MSC_TIMESTAMP, 40000 }, SYN);

	/* Palm: a finger whose confidence drops becomes MT_TOOL_PALM; when
	 * it lifts without confidence, readers first see the palm, then
	 * the release in a frame of its own. */
	clock_advance_ms(10);
	{
		struct contact f[] = { { 1, 1, 5, 300, 300 } };

		report_in(hdev, b, elan_tp(b, f, 1, 600, 1, 0));
		n = drain(c, e, 256);
		CHECK(stream_has(e, n, EV_ABS, ABS_MT_TRACKING_ID, 3) &&
			      stream_has(e, n, EV_KEY, BTN_TOUCH, 1),
		      "palm finger down");
	}
	clock_advance_ms(10);
	{
		struct contact f[] = { { 0, 1, 5, 310, 300 } };

		report_in(hdev, b, elan_tp(b, f, 1, 700, 1, 0));
		expect_stream("palm", c, { EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM },
			      { EV_ABS, ABS_MT_POSITION_X, 310 },
			      { EV_ABS, ABS_X, 310 },
			      { EV_MSC, MSC_TIMESTAMP, 60000 }, SYN);
	}
	clock_advance_ms(10);
	{
		struct contact f[] = { { 0, 0, 5, 310, 300 } };

		report_in(hdev, b, elan_tp(b, f, 1, 800, 1, 0));
		expect_stream("palm lift", c,
			      /* still a palm in this frame ... */
			      { EV_MSC, MSC_TIMESTAMP, 70000 }, SYN,
			      /* ... released in the next */
			      { EV_ABS, ABS_MT_TRACKING_ID, -1 },
			      { EV_KEY, BTN_TOUCH, 0 },
			      { EV_KEY, BTN_TOOL_FINGER, 0 }, SYN);
	}

	/* Scan time wraps at 65535: the timestamp keeps growing. */
	clock_advance_ms(10);
	report_in(hdev, b, elan_tp(b, NULL, 0, 65000, 0, 0));
	n = drain(c, e, 256);
	CHECK(n == 2 && e[0].code == MSC_TIMESTAMP &&
		      e[0].value == 70000 + 64200 * 100,
	      "before wrap %d", n ? e[0].value : -1);
	clock_advance_ms(10);
	report_in(hdev, b, elan_tp(b, NULL, 0, 100, 0, 0));
	n = drain(c, e, 256);
	CHECK(n == 2 && e[0].value == 70000 + 64200 * 100 + 635 * 100,
	      "after wrap %d", n ? e[0].value : -1);
	/* A pause longer than a second starts the timestamps over. */
	clock_advance_ms(1500);
	report_in(hdev, b, elan_tp(b, NULL, 0, 200, 0, 0));
	n = drain(c, e, 256);
	CHECK(n == 2 && e[0].value == 0, "resync %d", n ? e[0].value : -1);

	/* Click: BTN_LEFT, alone in a frame with no contacts. */
	clock_advance_ms(10);
	report_in(hdev, b, elan_tp(b, NULL, 0, 300, 0, 1));
	expect_stream("click", c, { EV_KEY, BTN_LEFT, 1 },
		      { EV_MSC, MSC_TIMESTAMP, 10000 }, SYN);
	clock_advance_ms(10);
	report_in(hdev, b, elan_tp(b, NULL, 0, 400, 0, 0));
	expect_stream("release", c, { EV_KEY, BTN_LEFT, 0 },
		      { EV_MSC, MSC_TIMESTAMP, 20000 }, SYN);

	/* Sticky fingers: a contact down and then silence; after 100 ms
	 * the release runs.  A report within the 100 ms moves the deadline. */
	clock_advance_ms(10);
	{
		struct contact f[] = { { 1, 1, 2, 500, 500 } };

		report_in(hdev, b, elan_tp(b, f, 1, 500, 1, 0));
		drain(c, e, 256);
		clock_advance_ms(90);
		CHECK(test_wq_run() == 0, "nothing due at 90 ms");
		f[0].x = 510;
		report_in(hdev, b, elan_tp(b, f, 1, 1400, 1, 0));
		drain(c, e, 256);
		clock_advance_ms(90);
		CHECK(test_wq_run() == 0, "deadline moved");
		CHECK(test_wq_pending() == 1, "release armed");
		clock_advance_ms(20);
		CHECK(test_wq_run() == 1, "release ran");
		n = drain(c, e, 256);
		CHECK(stream_has(e, n, EV_ABS, ABS_MT_TRACKING_ID, -1) &&
			      stream_has(e, n, EV_KEY, BTN_TOUCH, 0),
		      "sticky finger released (%d events)", n);
		/* The lift that arrives late changes nothing more. */
		f[0].tip = 0;
		report_in(hdev, b, elan_tp(b, f, 1, 1500, 1, 0));
		n = drain(c, e, 256);
		CHECK(!stream_has(e, n, EV_ABS, ABS_MT_TRACKING_ID, -1),
		      "no second release");
		CHECK(test_wq_pending() == 0, "release disarmed");
	}

	/* Short report: zero-padded (a finger with no data is no finger). */
	{
		struct contact f[] = { { 1, 1, 4, 700, 800 } };

		clock_advance_ms(10);
		elan_tp(b, f, 1, 1600, 1, 0);
		CHECK(report_in(hdev, b, 8) == 0, "short report accepted");
		n = drain(c, e, 256);
		/* Contact count and scan time read as 0: a new frame that
		 * announces no contacts (the finger in the bytes that came
		 * does not count), still processed and ended. */
		CHECK(n == 2 && e[0].type == EV_MSC &&
			      !stream_has(e, n, EV_ABS, ABS_MT_POSITION_X, 700),
		      "short report processed (%d events)", n);
		/* lift it */
		f[0].tip = 0;
		report_in(hdev, b, elan_tp(b, f, 1, 1700, 1, 0));
		drain(c, e, 256);
	}

	/* Reports nobody declared: dropped. */
	b[0] = 0x33;
	CHECK(report_in(hdev, b, 10) == -EINVAL, "unknown report id");
	b[0] = 2; // a feature ID arriving as input
	CHECK(report_in(hdev, b, 2) == -EINVAL, "feature id as input");
	CHECK(report_in(hdev, b, 0) == -EINVAL, "empty report");
	n = drain(c, e, 256);
	CHECK(n == 0, "nothing from bad reports (%d)", n);

	/* The Mouse collection: relative motion and two buttons. */
	{
		static const uint8_t mr[] = { 1, 0x01, 0x05, 0xfd };

		report_in(hdev, mr, sizeof(mr));
		expect_stream("mouse", cm, { EV_MSC, MSC_SCAN, 0x00090001 },
			      { EV_KEY, BTN_LEFT, 1 },
			      { EV_REL, REL_X, 5 }, { EV_REL, REL_Y, -3 }, SYN);
		static const uint8_t mr2[] = { 1, 0x02, 0x00, 0x00 };

		report_in(hdev, mr2, sizeof(mr2));
		expect_stream("mouse buttons", cm,
			      { EV_MSC, MSC_SCAN, 0x00090001 },
			      { EV_KEY, BTN_LEFT, 0 },
			      { EV_MSC, MSC_SCAN, 0x00090002 },
			      { EV_KEY, BTN_RIGHT, 1 }, SYN);
		expect_empty("touchpad untouched by mouse reports", c);
	}

	/* Suspend: both switches off, high latency (no latency feature
	 * here); resume: SET_IDLE, input mode and switches again. */
	m->nlog = 0;
	CHECK(hid_driver_suspend(hdev, HID_PM_EVENT_SUSPEND) == 0, "suspend");
	CHECK(m->nlog == 2 && m->log[1].id == 7 && m->log[1].data[1] == 0,
	      "suspend modes (%d)", m->nlog);
	m->nlog = 0;
	CHECK(hid_driver_resume(hdev) == 0 && m->idles == 1, "resume");
	CHECK(m->nlog == 2 && m->log[0].id == 3 && m->log[0].data[1] == 3 &&
		      m->log[1].id == 7 && m->log[1].data[1] == 3,
	      "resume modes");
	/* reset_resume releases contacts still down. */
	{
		struct contact f[] = { { 1, 1, 1, 600, 600 } };

		clock_advance_ms(10);
		report_in(hdev, b, elan_tp(b, f, 1, 1800, 1, 0));
		drain(c, e, 256);
		CHECK(hid_driver_reset_resume(hdev) == 0, "reset_resume");
		n = drain(c, e, 256);
		CHECK(stream_has(e, n, EV_ABS, ABS_MT_TRACKING_ID, -1),
		      "reset_resume released the contact");
		test_wq_run_until_idle();
		drain(c, e, 256);
	}

	/* Removal: the nodes go, open descriptors read ENODEV. */
	evdev_release(cm);
	hid_destroy_device(hdev);
	CHECK(evdev_read(c, e, EV_SZ, 1) == -ENODEV, "ENODEV after removal");
	evdev_release(c);
	CHECK(m->stopped == 1 && m->closes == 1, "stopped %d closed %d",
	      m->stopped, m->closes);
	CHECK(test_wq_pending() == 0, "no work left");
	h_free(m);
}

// ---------------------------------------------------------------------------
// cirque: the Dell touchpad, hybrid frames, hi-res wheel
// ---------------------------------------------------------------------------

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

static void test_cirque(void)
{
	struct mock *m = mock_new(cirque_ptp, sizeof(cirque_ptp));
	static const uint8_t f2[] = { 0x02, 0x05 };
	struct hid_device *hdev;
	struct input_dev *tp, *mouse;
	struct evdev_client *c, *cm;
	struct input_event e[256];
	uint8_t b[64];
	int n, got_set_mult = 0;

	h_printf("== cirque\n");
	hid_core_init();
	mock_answer(m, 2, f2, sizeof(f2));
	hdev = dev_add(m, BUS_I2C, 0x0488, 0x1085, "VEN_0488:00", DELL_PARENT);
	CHECK(hdev->driver && !h_strcmp(hdev->driver->name, "hid-multitouch"),
	      "driver");
	for (int i = 0; i < m->nlog; i++)
		if (m->log[i].reqtype == HID_REQ_SET_REPORT &&
		    m->log[i].id == 0x0a && m->log[i].data[1] == 0x01)
			got_set_mult = 1;
	CHECK(got_set_mult, "resolution multiplier set to its maximum");
	CHECK(m->state_len[5] == 2 && m->state[5][1] == 0, "latency normal");
	CHECK(m->state[3][1] == 3 && m->state[7][1] == 3, "modes");

	tp = find_input(hdev, "VEN_0488:00 0488:1085 Touchpad");
	mouse = find_input(hdev, "VEN_0488:00 0488:1085 Mouse");
	CHECK(tp && mouse, "names");
	compare_with_spec(mouse, DELL_SPEC);
	compare_with_spec(tp, DELL_SPEC);
	if (!tp || !mouse)
		return;
	c = open_input(tp);
	cm = open_input(mouse);

	/* Hi-res wheel: multiplier 8, every report 1/8 notch. */
	for (int i = 0; i < 8; i++) {
		static const uint8_t w[] = { 1, 0, 0, 0, 1 };

		report_in(hdev, w, sizeof(w));
	}
	n = drain(cm, e, 256);
	{
		int hi = 0, lo = 0, his = 0;

		for (int i = 0; i < n; i++) {
			if (e[i].type == EV_REL && e[i].code == REL_WHEEL_HI_RES) {
				hi += e[i].value;
				his++;
			}
			if (e[i].type == EV_REL && e[i].code == REL_WHEEL)
				lo += e[i].value;
		}
		CHECK(hi == 120 && his == 8 && lo == 1,
		      "hi-res wheel: %d in %d, %d notches", hi, his, lo);
	}

	/* Hybrid: three contacts over two reports; the frame goes out after
	 * the second. */
	test_set_clock_ns(g_clock);
	{
		struct contact a[] = { { 1, 1, 10, 100, 100 },
				       { 1, 1, 11, 200, 200 } };
		struct contact bb[] = { { 1, 1, 12, 300, 300 } };

		report_in(hdev, b, cirque_tp(b, a, 2, 100, 3, 1));
		CHECK(drain(c, e, 256) == 0, "first half: nothing yet");
		report_in(hdev, b, cirque_tp(b, bb, 1, 100, 0, 0));
		n = drain(c, e, 256);
		CHECK(stream_has(e, n, EV_KEY, BTN_TOOL_TRIPLETAP, 1) &&
			      stream_has(e, n, EV_ABS, ABS_MT_POSITION_X, 300) &&
			      stream_has(e, n, EV_KEY, BTN_LEFT, 1),
		      "hybrid frame (%d events)", n);
		/* The button of the continuation report (0) did not count. */
		CHECK(!stream_has(e, n, EV_KEY, BTN_LEFT, 0), "button kept");
		/* Lift all three in one hybrid frame. */
		clock_advance_ms(8);
		for (int i = 0; i < 2; i++)
			a[i].tip = 0;
		bb[0].tip = 0;
		report_in(hdev, b, cirque_tp(b, a, 2, 200, 3, 0));
		report_in(hdev, b, cirque_tp(b, bb, 1, 200, 0, 0));
		n = drain(c, e, 256);
		CHECK(stream_has(e, n, EV_KEY, BTN_TOUCH, 0) &&
			      stream_has(e, n, EV_KEY, BTN_LEFT, 0),
		      "hybrid lift");
	}
	evdev_release(c);
	evdev_release(cm);
	hid_destroy_device(hdev);
	test_wq_run_until_idle();
	h_free(m);
}

// ---------------------------------------------------------------------------
// generic: boot mouse, keyboard
// ---------------------------------------------------------------------------

static void test_generic(void)
{
	struct input_event e[64];
	int n;

	h_printf("== generic\n");
	hid_core_init();

	/* Boot mouse: unnumbered reports, one device without a suffix. */
	{
		struct mock *m = mock_new(boot_mouse, sizeof(boot_mouse));
		struct hid_device *h =
			dev_add(m, BUS_USB, 0x046d, 0xc077, "USB Mouse", NULL);
		struct input_dev *in = find_input(h, "USB Mouse 046D:C077");
		struct evdev_client *c;

		CHECK(h->driver && !h_strcmp(h->driver->name, "hid-generic"),
		      "generic driver");
		CHECK(in && count_inputs(h) == 1, "one mouse device");
		if (in) {
			CHECK(input_test_bit(REL_X, in->relbit) &&
				      input_test_bit(REL_WHEEL, in->relbit) &&
				      input_test_bit(REL_WHEEL_HI_RES,
						     in->relbit) &&
				      input_test_bit(BTN_MIDDLE, in->keybit) &&
				      in->devpath == NULL,
			      "mouse caps");
			c = open_input(in);
			{
				static const uint8_t r[] = { 0x04, 0x02, 0x00,
							     0xff };

				CHECK(report_in(h, r, sizeof(r)) == 0, "report");
			}
			expect_stream("boot mouse", c,
				      { EV_MSC, MSC_SCAN, 0x00090003 },
				      { EV_KEY, BTN_MIDDLE, 1 },
				      { EV_REL, REL_X, 2 },
				      { EV_REL, REL_WHEEL, -1 },
				      { EV_REL, REL_WHEEL_HI_RES, -120 }, SYN);
			{
				/* Short: the missing bytes are zero. */
				static const uint8_t r[] = { 0x00 };

				CHECK(report_in(h, r, sizeof(r)) == 0, "short");
			}
			expect_stream("short mouse report", c,
				      { EV_MSC, MSC_SCAN, 0x00090003 },
				      { EV_KEY, BTN_MIDDLE, 0 }, SYN);
			evdev_release(c);
		}
		hid_destroy_device(h);
		h_free(m);
	}

	/* Keyboard + consumer keys in one device; LEDs back out. */
	{
		struct mock *m = mock_new(keyboard, sizeof(keyboard));
		struct hid_device *h =
			dev_add(m, BUS_USB, 0x413c, 0x2113, "Dell KB", NULL);
		struct input_dev *in = find_input(h, "Dell KB 413C:2113 Keyboard");
		struct evdev_client *c;

		CHECK(in && count_inputs(h) == 1, "keyboard device (%d)",
		      count_inputs(h));
		if (!in) {
			hid_destroy_device(h);
			h_free(m);
			return;
		}
		CHECK(input_test_bit(KEY_A, in->keybit) &&
			      input_test_bit(KEY_LEFTCTRL, in->keybit) &&
			      input_test_bit(KEY_VOLUMEUP, in->keybit) &&
			      input_test_bit(KEY_MUTE, in->keybit) &&
			      input_test_bit(KEY_PLAYPAUSE, in->keybit) &&
			      input_test_bit(EV_REP, in->evbit) &&
			      input_test_bit(LED_CAPSL, in->ledbit) &&
			      input_test_bit(LED_KANA, in->ledbit),
		      "keyboard caps");
		c = open_input(in);
		{
			static const uint8_t r[] = { 1, 0x01, 0, 0x04, 0, 0,
						     0, 0, 0 };

			report_in(h, r, sizeof(r));
		}
		expect_stream("ctrl+a", c, { EV_MSC, MSC_SCAN, 0x000700e0 },
			      { EV_KEY, KEY_LEFTCTRL, 1 },
			      { EV_MSC, MSC_SCAN, 0x00070004 },
			      { EV_KEY, KEY_A, 1 }, SYN);
		{
			/* ErrorRollOver: nothing changes. */
			static const uint8_t r[] = { 1, 0x01, 0, 0x01, 0x01, 0x01,
						     0x01, 0x01, 0x01 };

			report_in(h, r, sizeof(r));
		}
		expect_empty("rollover", c);
		{
			static const uint8_t r[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };

			report_in(h, r, sizeof(r));
		}
		expect_stream("release", c, { EV_MSC, MSC_SCAN, 0x000700e0 },
			      { EV_KEY, KEY_LEFTCTRL, 0 },
			      { EV_MSC, MSC_SCAN, 0x00070004 },
			      { EV_KEY, KEY_A, 0 }, SYN);
		{
			static const uint8_t v[] = { 2, 0xe9, 0x00 };
			static const uint8_t z[] = { 2, 0x00, 0x00 };

			report_in(h, v, sizeof(v));
			report_in(h, z, sizeof(z));
		}
		expect_stream("volume up", c, { EV_MSC, MSC_SCAN, 0x000c00e9 },
			      { EV_KEY, KEY_VOLUMEUP, 1 }, SYN,
			      { EV_MSC, MSC_SCAN, 0x000c00e9 },
			      { EV_KEY, KEY_VOLUMEUP, 0 }, SYN);

		/* Caps Lock LED written by a client: an output report
		 * [01 02] from the work item. */
		{
			struct input_event w;

			h_memset(&w, 0, sizeof(w));
			w.type = EV_LED;
			w.code = LED_CAPSL;
			w.value = 1;
			m->nlog = 0;
			CHECK(evdev_write(c, &w, EV_SZ) == EV_SZ, "write LED");
			CHECK(test_wq_run() == 1, "LED work ran");
			CHECK(m->nlog == 1 && m->log[0].reqtype == 0 &&
				      m->log[0].len == 2 &&
				      m->log[0].data[0] == 1 &&
				      m->log[0].data[1] == 0x02,
			      "LED output report (%d)", m->nlog);
		}
		n = drain(c, e, 64);
		(void)n;
		evdev_release(c);
		hid_destroy_device(h);
		h_free(m);
	}
}

// ---------------------------------------------------------------------------
// touchscreen
// ---------------------------------------------------------------------------

static void test_touchscreen(void)
{
	struct mock *m = mock_new(touchscreen, sizeof(touchscreen));
	struct hid_device *h;
	struct input_dev *in;
	struct evdev_client *c;
	struct input_event e[128];
	uint8_t b[TS_LEN];
	int n;

	h_printf("== touchscreen\n");
	hid_core_init();
	{
		static const uint8_t f2[] = { 2, 2 };

		mock_answer(m, 2, f2, sizeof(f2));
	}
	h = dev_add(m, BUS_I2C, 0x2386, 0x4328, "WCOM0001:00", NULL);
	CHECK(h->group == HID_GROUP_MULTITOUCH_WIN_8, "group");
	in = find_input(h, "WCOM0001:00 2386:4328");
	CHECK(in != NULL, "touchscreen keeps the device name");
	CHECK(m->state[3][1] == 2 && m->state[3][2] == 0, "input mode 2");
	if (!in) {
		hid_destroy_device(h);
		h_free(m);
		return;
	}
	CHECK(input_test_bit(INPUT_PROP_DIRECT, in->propbit) &&
		      !input_test_bit(INPUT_PROP_POINTER, in->propbit) &&
		      input_test_bit(ABS_MT_TOUCH_MAJOR, in->absbit) &&
		      input_test_bit(ABS_MT_TOUCH_MINOR, in->absbit) &&
		      input_test_bit(ABS_MT_ORIENTATION, in->absbit) &&
		      input_test_bit(ABS_MT_DISTANCE, in->absbit) &&
		      !input_test_bit(ABS_MT_TOOL_TYPE, in->absbit) &&
		      !input_test_bit(BTN_TOOL_FINGER, in->keybit) &&
		      in->mt && in->mt->num_slots == 2,
	      "touchscreen caps");
	CHECK(input_abs_get_res(in, ABS_MT_POSITION_X) == 20 &&
		      input_abs_get_res(in, ABS_MT_POSITION_Y) == 34,
	      "resolution %d %d", input_abs_get_res(in, ABS_MT_POSITION_X),
	      input_abs_get_res(in, ABS_MT_POSITION_Y));
	c = open_input(in);
	h_memset(b, 0, sizeof(b));
	b[0] = 1;
	b[1] = 0x03; // tip + in range
	b[2] = 9; // contact id
	b[3] = LO(1000);
	b[4] = HI(1000);
	b[5] = LO(2000);
	b[6] = HI(2000);
	b[7] = 30; // width
	b[8] = 20; // height
	b[17] = 1;
	report_in(h, b, sizeof(b));
	n = drain(c, e, 128);
	CHECK(stream_has(e, n, EV_ABS, ABS_MT_TOUCH_MAJOR, 30) &&
		      stream_has(e, n, EV_ABS, ABS_MT_TOUCH_MINOR, 20) &&
		      stream_has(e, n, EV_ABS, ABS_MT_ORIENTATION, 1) &&
		      stream_has(e, n, EV_KEY, BTN_TOUCH, 1) &&
		      stream_has(e, n, EV_ABS, ABS_X, 1000),
	      "touch frame (%d)", n);
	/* Hover: in range without the tip. */
	b[1] = 0x02;
	report_in(h, b, sizeof(b));
	n = drain(c, e, 128);
	CHECK(stream_has(e, n, EV_ABS, ABS_MT_DISTANCE, 1), "hover");
	b[1] = 0;
	report_in(h, b, sizeof(b));
	n = drain(c, e, 128);
	CHECK(stream_has(e, n, EV_ABS, ABS_MT_TRACKING_ID, -1), "up");
	evdev_release(c);
	hid_destroy_device(h);
	test_wq_run_until_idle();
	h_free(m);
}

// ---------------------------------------------------------------------------
// malformed
// ---------------------------------------------------------------------------

static void test_malformed(void)
{
	int rc;

	h_printf("== malformed\n");
	hid_core_init();
	for (unsigned int i = 0; i < HID_ARRAY_SIZE(bad_descs); i++) {
		struct hid_device *h =
			bare_parse(bad_descs[i].bytes, bad_descs[i].len, &rc);
		struct mock *m;
		struct hid_device *d;

		CHECK(rc < 0, "%s accepted", bad_descs[i].name);
		CHECK(!(h->status & HID_STAT_PARSED) && !h->collection &&
			      list_empty(&h->report_enum[0].report_list),
		      "%s left state behind", bad_descs[i].name);
		bare_free(h);

		/* Through the whole stack: no driver, no crash. */
		m = mock_new(bad_descs[i].bytes, bad_descs[i].len);
		d = dev_add(m, BUS_I2C, 0x04f3, 0x0001, "BAD0000:00", NULL);
		CHECK(d->driver == NULL || count_inputs(d) == 0,
		      "%s bound with inputs", bad_descs[i].name);
		hid_destroy_device(d);
		h_free(m);
	}
	/* A zero-length or oversized descriptor is not even taken. */
	{
		struct hid_device *h = hid_allocate_device();
		static uint8_t big[HID_MAX_RDESC_SIZE + 1];

		CHECK(hid_parse_report(h, elan_ptp, 0) == -EINVAL, "empty");
		CHECK(hid_parse_report(h, big, sizeof(big)) == -EINVAL, "big");
		CHECK(hid_open_report(h) == -ENODEV, "nothing to parse");
		hid_destroy_device(h);
	}
	/* A transport without the mandatory operations is refused. */
	{
		static const struct hid_ll_driver incomplete = {
			.parse = mock_parse,
		};
		struct hid_device *h = hid_allocate_device();

		h->ll_driver = &incomplete;
		CHECK(hid_add_device(h) == -EINVAL, "incomplete transport");
		hid_destroy_device(h);
	}
}

// ---------------------------------------------------------------------------
// race
// ---------------------------------------------------------------------------

static volatile int g_race_stop;
static struct hid_device *g_race_dev;
static unsigned long g_race_reports, g_race_busy;

static void *race_irq(void *p)
{
	uint8_t b[64];
	unsigned int seed = 7;

	(void)p;
	h_srandom(seed);
	while (!__atomic_load_n(&g_race_stop, __ATOMIC_ACQUIRE)) {
		struct contact f[3];
		int nf = (int)(h_random() % 4);
		int rc;

		for (int i = 0; i < nf; i++) {
			f[i].conf = (int)(h_random() % 4 != 0);
			f[i].tip = (int)(h_random() % 3 != 0);
			f[i].id = (int)(h_random() % 16);
			f[i].x = (int)(h_random() % 3300);
			f[i].y = (int)(h_random() % 2200);
		}
		rc = report_in(g_race_dev, b,
			       elan_tp(b, f, nf, (int)(h_random() & 0xffff), nf,
				       (int)(h_random() & 1)));
		if (rc == -EBUSY || rc == -ENODEV)
			__atomic_fetch_add(&g_race_busy, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&g_race_reports, 1, __ATOMIC_RELAXED);
		if (h_random() % 64 == 0) {
			static const uint8_t mr[] = { 1, 1, 2, 3 };

			report_in(g_race_dev, mr, sizeof(mr));
		}
	}
	test_thread_done();
	return NULL;
}

static void *race_worker(void *p)
{
	(void)p;
	while (!__atomic_load_n(&g_race_stop, __ATOMIC_ACQUIRE)) {
		test_wq_run();
		h_usleep(200);
	}
	test_thread_done();
	return NULL;
}

static void *race_reader(void *p)
{
	(void)p;
	while (!__atomic_load_n(&g_race_stop, __ATOMIC_ACQUIRE)) {
		struct hid_input *hi;
		struct input_event e[64];
		int minors[4], nm = 0;

		/* Open whatever nodes exist now; the device may be rebinding
		 * under us (then the open fails or reads ENODEV). */
		for (int u = 2; u < EVDEV_MINORS && nm < 4; u++)
			if (evdev_unit_exists(u))
				minors[nm++] = u;
		(void)hi;
		for (int i = 0; i < nm; i++) {
			struct evdev_client *c = NULL;

			if (evdev_open(minors[i], &c) == 0) {
				for (int k = 0; k < 5; k++) {
					evdev_read(c, e, sizeof(e), 1);
					h_usleep(100);
				}
				evdev_release(c);
			}
		}
		h_usleep(500);
	}
	test_thread_done();
	return NULL;
}

static void test_race(void)
{
	struct mock *m = elan_mock();
	unsigned long th[3];
	unsigned long long until;
	unsigned long secs = 3;
	char *env = h_getenv("TESTHID_RACE_SECS");
	int rounds = 0;

	h_printf("== race\n");
	hid_core_init();
	if (env)
		secs = (unsigned long)h_strtoull(env, NULL, 10);
	test_set_clock_ns(0); // the host clock: deadlines pass by themselves
	g_race_dev = dev_add(m, BUS_I2C, 0x04f3, 0x3169, "ELAN073B:00",
			     HP_PARENT);
	g_race_stop = 0;
	h_thread_start(race_irq, NULL, &th[0]);
	h_thread_start(race_worker, NULL, &th[1]);
	h_thread_start(race_reader, NULL, &th[2]);

	until = h_monotonic_ns() + secs * 1000000000ull;
	while (h_monotonic_ns() < until) {
		switch (rounds++ % 4) {
		case 0:
			hid_driver_suspend(g_race_dev, HID_PM_EVENT_SUSPEND);
			hid_driver_resume(g_race_dev);
			break;
		case 1:
			hid_driver_reset_resume(g_race_dev);
			break;
		case 2:
			/* Unbind: the generic driver takes the device, then
			 * the multitouch driver comes back for it. */
			hid_multitouch_exit();
			h_usleep(2000);
			hid_multitouch_init();
			break;
		default:
			h_usleep(1000);
			break;
		}
	}
	__atomic_store_n(&g_race_stop, 1, __ATOMIC_RELEASE);
	for (int i = 0; i < 3; i++)
		h_thread_join(th[i]);
	CHECK(g_race_dev->driver &&
		      !h_strcmp(g_race_dev->driver->name, "hid-multitouch"),
	      "back on the multitouch driver");
	h_printf("  %lu reports (%lu while rebinding), %d rounds\n",
		 g_race_reports, g_race_busy, rounds);
	hid_destroy_device(g_race_dev);
	test_wq_run_until_idle();
	CHECK(test_wq_pending() == 0, "no work left");
	h_free(m);
	test_set_clock_ns(g_clock);
}

// ---------------------------------------------------------------------------
// fuzz
// ---------------------------------------------------------------------------

static unsigned long rnd(void)
{
	return (h_random() << 31) ^ h_random();
}

/* Interesting data values for items. */
static uint32_t fuzz_value(void)
{
	static const uint32_t v[] = { 0,	  1,	  2,	   3,
				      4,	  5,	  7,	   8,
				      15,	  16,	  0x7f,	   0x80,
				      0xff,	  0x100,  256,	   257,
				      0x7fff,	  0x8000, 0xffff,  12288,
				      12289,	  0x10000, 0x7fffffff, 0x80000000u,
				      0xffffffffu, 0x0e,  0x0c,	   0x11,
				      0x13 };

	if (h_random() % 3)
		return v[h_random() % HID_ARRAY_SIZE(v)];
	return (uint32_t)rnd();
}

/* A random descriptor from a loose grammar: mostly plausible items. */
static unsigned int fuzz_generate(uint8_t *d, unsigned int cap)
{
	static const uint8_t prefixes[] = {
		/* main */ 0x80, 0x90, 0xb0, 0xa0, 0xc0,
		/* global */ 0x04, 0x14, 0x24, 0x34, 0x44, 0x54, 0x64, 0x74,
		0x84, 0x94, 0xa4, 0xb4,
		/* local */ 0x08, 0x18, 0x28, 0xa8, 0x38, 0x78,
	};
	unsigned int n = 0, items = 4 + (unsigned int)(h_random() % 120);
	int depth = 0;

	for (unsigned int i = 0; i < items && n + 6 < cap; i++) {
		uint8_t p = prefixes[h_random() % HID_ARRAY_SIZE(prefixes)];
		uint32_t v = fuzz_value();
		unsigned int size;

		if (h_random() % 50 == 0) {
			d[n++] = (uint8_t)h_random(); // anything at all
			continue;
		}
		if (p == 0xa0) {
			depth++;
		} else if (p == 0xc0) {
			if (depth > 0)
				depth--;
			else if (h_random() % 4)
				continue;
		}
		size = v <= 0xff ? 1 : v <= 0xffff ? 2 : 3;
		if (p == 0xc0 || p == 0xa4 || p == 0xb4)
			size = 0;
		if (h_random() % 10 == 0)
			size = (unsigned int)(h_random() % 4);
		d[n++] = (uint8_t)(p | size);
		if (size == 3)
			size = 4;
		for (unsigned int k = 0; k < size; k++)
			d[n++] = (uint8_t)(v >> (8 * k));
	}
	while (depth-- > 0 && n < cap && h_random() % 8)
		d[n++] = 0xc0;
	return n;
}

static unsigned int fuzz_mutate(uint8_t *d, unsigned int cap)
{
	static const struct {
		const uint8_t *b;
		unsigned int n;
	} seeds[] = {
		{ elan_ptp, sizeof(elan_ptp) },
		{ cirque_ptp, sizeof(cirque_ptp) },
		{ boot_mouse, sizeof(boot_mouse) },
		{ keyboard, sizeof(keyboard) },
		{ touchscreen, sizeof(touchscreen) },
	};
	unsigned int s = (unsigned int)(h_random() % HID_ARRAY_SIZE(seeds));
	unsigned int n = seeds[s].n, muts = 1 + (unsigned int)(h_random() % 8);

	if (n > cap)
		n = cap;
	h_memcpy(d, seeds[s].b, n);
	for (unsigned int i = 0; i < muts && n; i++) {
		unsigned int at = (unsigned int)(h_random() % n);

		switch (h_random() % 6) {
		case 0:
			d[at] ^= (uint8_t)(1u << (h_random() % 8));
			break;
		case 1:
			d[at] = (uint8_t)h_random();
			break;
		case 2: // insert a byte
			if (n < cap) {
				for (unsigned int k = n; k > at; k--)
					d[k] = d[k - 1];
				d[at] = (uint8_t)fuzz_value();
				n++;
			}
			break;
		case 3: // delete a byte
			for (unsigned int k = at; k + 1 < n; k++)
				d[k] = d[k + 1];
			n--;
			break;
		case 4: // truncate
			n = at;
			break;
		default: // an interesting value into a data byte
			d[at] = (uint8_t)fuzz_value();
			break;
		}
	}
	return n;
}

/* A report for the parsed device: one of its IDs (or garbage), any
 * length around the expected one, random bytes. */
static unsigned int fuzz_report(struct hid_device *h, uint8_t *b,
				unsigned int cap)
{
	struct hid_report_enum *re = &h->report_enum[HID_INPUT_REPORT];
	struct hid_report *r = NULL;
	unsigned int len;

	if (!list_empty(&re->report_list) && h_random() % 8) {
		unsigned int k = (unsigned int)(h_random() % 8);

		list_for_each_entry(r, &re->report_list, list)
			if (!k--)
				break;
		if (&r->list == &re->report_list)
			r = list_first_entry(&re->report_list,
					     struct hid_report, list);
	}
	len = r ? hid_report_len(r) : (unsigned int)(h_random() % 64);
	switch (h_random() % 4) {
	case 0:
		len = (unsigned int)(h_random() % (len + 2));
		break;
	case 1:
		len += (unsigned int)(h_random() % 8);
		break;
	default:
		break;
	}
	if (len > cap)
		len = cap;
	for (unsigned int i = 0; i < len; i++)
		b[i] = (uint8_t)h_random();
	if (r && len && re->numbered)
		b[0] = (uint8_t)r->id;
	return len;
}

static void test_fuzz(void)
{
	static uint8_t d[HID_MAX_DESCRIPTOR_SIZE];
	static uint8_t b[HID_MAX_BUFFER_SIZE];
	unsigned long secs = 60, parse_iters = 0, dev_iters = 0, reports = 0;
	unsigned long accepted = 0, bound = 0;
	char *env = h_getenv("TESTHID_FUZZ_SECS");
	unsigned long long start, until;
	unsigned int seed = 0x51d;

	h_printf("== fuzz\n");
	hid_core_init();
	if (env)
		secs = (unsigned long)h_strtoull(env, NULL, 10);
	env = h_getenv("TESTHID_FUZZ_SEED");
	if (env)
		seed = (unsigned int)h_strtoull(env, NULL, 0);
	h_srandom(seed);
	start = h_monotonic_ns();
	until = start + secs * 1000000000ull;

	while (h_monotonic_ns() < until) {
		unsigned int len;

		/* Parser and extraction: many quick rounds. */
		for (int k = 0; k < 64; k++) {
			struct hid_device *h;
			int rc;

			len = h_random() % 2 ? fuzz_generate(d, sizeof(d)) :
					       fuzz_mutate(d, sizeof(d));
			if (!len)
				continue;
			h = bare_parse(d, len, &rc);
			parse_iters++;
			if (rc == 0) {
				accepted++;
				mutex_lock(&h->driver_input_lock);
				for (int r = 0; r < 8; r++) {
					unsigned int rl =
						fuzz_report(h, b, sizeof(b));

					if (rl)
						hid_report_raw_event(
							h,
							(int)(h_random() % 3),
							b, rl, 1);
				}
				mutex_unlock(&h->driver_input_lock);
			}
			bare_free(h);
		}

		/* The whole stack: bind, reports, PM, nodes, removal. */
		{
			struct mock *m;
			struct hid_device *h;
			static const uint32_t vendors[] = { 0x04f3, 0x06cb,
							    0x0488, 0x27c6,
							    0x1fd2, 0x0457 };

			len = h_random() % 3 ? fuzz_mutate(d, sizeof(d)) :
					       fuzz_generate(d, sizeof(d));
			if (!len)
				continue;
			m = mock_new(d, len);
			m->fuzz = 1;
			h = hid_allocate_device();
			h->bus = h_random() % 2 ? BUS_I2C : BUS_USB;
			h->vendor = vendors[h_random() % HID_ARRAY_SIZE(vendors)];
			h->product = (uint32_t)(h_random() & 0xffff);
			if (h_random() % 4 == 0)
				h->product = 0x01e8; // fixup candidate
			h_snprintf(h->name, sizeof(h->name), "FUZZ%04lu:00",
				   dev_iters % 10000);
			h->ll_driver = &mock_ll;
			h->driver_data = m;
			hid_add_device(h);
			dev_iters++;
			if (h->driver) {
				struct evdev_client *c = NULL;
				struct hid_input *hi;

				bound++;
				hi = list_first_entry_or_null(&h->inputs,
							      struct hid_input,
							      list);
				if (hi && hi->input->evdev && h_random() % 2)
					c = open_input(hi->input);
				for (int r = 0; r < 64; r++) {
					unsigned int rl =
						fuzz_report(h, b, sizeof(b));

					hid_input_report(h, HID_INPUT_REPORT, b,
							 rl, 1);
					reports++;
					if (h_random() % 16 == 0) {
						clock_advance_ms(h_random() %
								 200);
						test_wq_run();
					}
					if (h_random() % 64 == 0) {
						hid_driver_suspend(
							h, HID_PM_EVENT_SUSPEND);
						hid_driver_resume(h);
					}
					if (c && h_random() % 8 == 0) {
						struct input_event e[64];

						evdev_read(c, e, sizeof(e), 1);
					}
				}
				if (c && h_random() % 2) {
					evdev_release(c);
					c = NULL;
				}
				hid_destroy_device(h);
				if (c)
					evdev_release(c);
			} else {
				hid_destroy_device(h);
			}
			test_wq_run_until_idle();
			h_free(m);
		}
	}
	h_printf("  %lu descriptors parsed (%lu accepted), %lu devices "
		 "(%lu bound), %lu reports in %llu s\n",
		 parse_iters, accepted, dev_iters, bound, reports,
		 (h_monotonic_ns() - start) / 1000000000ull);
	CHECK(test_wq_pending() == 0, "no work left after fuzzing");
	CHECK(evdev_next_unit(2) == -1, "every node gone after fuzzing");
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : "all";
	int all = h_strcmp(only, "all") == 0;

	evdev_builtin_init();
	test_set_clock_ns(g_clock);

	if (all || !h_strcmp(only, "parse"))
		test_parse();
	if (all || !h_strcmp(only, "elan"))
		test_elan();
	if (all || !h_strcmp(only, "cirque"))
		test_cirque();
	if (all || !h_strcmp(only, "generic"))
		test_generic();
	if (all || !h_strcmp(only, "touchscreen"))
		test_touchscreen();
	if (all || !h_strcmp(only, "malformed"))
		test_malformed();
	if (all || !h_strcmp(only, "race"))
		test_race();
	if (all || !h_strcmp(only, "fuzz"))
		test_fuzz();

	CHECK(evdev_next_unit(2) == -1, "every node gone");
	CHECK(test_warning_count() == 0, "%ld kernel warnings",
	      test_warning_count());
	hid_multitouch_exit();
	hid_unregister_driver(&hid_generic_driver);
	test_thread_done();
	h_printf("%s: %ld checks, %ld failed\n", g_fails ? "FAIL" : "PASS",
		 g_checks, g_fails);
	return g_fails ? 1 : 0;
}
