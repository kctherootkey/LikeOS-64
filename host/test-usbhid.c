/*
 * Host test for the USB HID mouse report descriptor parser.
 *
 * Runs on the BUILD machine.  host/test-usbhid.sh cuts the parser and the
 * Report Protocol decoder out of kernel/dev/hid/usb_hid.c (and the layout
 * typedefs out of the header) into two headers, and this file feeds
 * them real mouse report descriptors and reports:
 *
 *   - the classic 3-button + wheel mouse (no report IDs, 4-byte report)
 *   - a report-ID mouse with 12-bit X/Y, 5 buttons, AC Pan, plus a Consumer
 *     Control collection on the same endpoint
 *   - a vendor collection declaring X/Y BEFORE the Mouse collection (the
 *     Mouse collection must win)
 *   - the HID 1.11 example boot mouse without a wheel, and a keyboard:
 *     both must be rejected so the driver stays in Boot Protocol
 *   - truncated and random descriptors: must not crash
 *
 * Build and run:  ./host/test-usbhid.sh
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the environment the cut-out code expects -------------------------- */

#define USBHID_REPORT_BUF_SIZE 64

static void hid_memset(void *dst, int val, size_t n)
{
	memset(dst, val, n);
}

#include "usbhid-types-under-test.h"

/* The decoder only touches mouse_layout; a minimal device is enough. */
typedef struct {
	usbhid_mouse_layout_t mouse_layout;
} usbhid_device_t;

/* Captured by the stubbed mouse subsystem entry point. */
static int g_inject_calls;
static int g_dx, g_dy, g_wheel;
static uint8_t g_buttons;

void mouse_inject_usb_movement(int dx, int dy, uint8_t buttons, int8_t wheel);

#include "usbhid-code-under-test.h"

void mouse_inject_usb_movement(int dx, int dy, uint8_t buttons, int8_t wheel)
{
	g_inject_calls++;
	g_dx = dx;
	g_dy = dy;
	g_buttons = buttons;
	g_wheel = wheel;
}

/* ---- descriptors ------------------------------------------------------- */

/* Classic wheel mouse: Usage Page GD, Mouse, Application, Pointer, Physical,
 * 3 buttons + 5 bits padding, X/Y/Wheel as 8-bit relative. */
static const uint8_t desc_classic[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05,
	0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
	0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05,
	0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
	0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0
};

/* Report-ID mouse (ID 2): 5 buttons + 3 pad bits, X/Y 12-bit relative,
 * Wheel 8-bit, AC Pan 8-bit; then a Consumer Control collection (ID 3). */
static const uint8_t desc_reportid[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1,
	0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81,
	0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0xF8, 0x26,
	0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x81, 0x06, 0x09, 0x38, 0x15,
	0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x05, 0x0C,
	0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
	/* Consumer Control, ID 3, one 16-bit array item */
	0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03, 0x19, 0x00, 0x2A,
	0x3C, 0x02, 0x15, 0x00, 0x26, 0x3C, 0x02, 0x95, 0x01, 0x75, 0x10,
	0x81, 0x00, 0xC0
};

/* Vendor collection (ID 0x10) that declares GD X/Y first, followed by a
 * classic wheel mouse under report ID 1.  The Mouse collection must win. */
static const uint8_t desc_vendor_first[] = {
	0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x10, 0x05, 0x01,
	0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95,
	0x02, 0x81, 0x02, 0xC0,
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1,
	0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
	0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81,
	0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81,
	0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0
};

/* USB HID 1.11 Appendix E.10: boot mouse, no wheel. */
static const uint8_t desc_boot_nowheel[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05,
	0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
	0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05,
	0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
	0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0
};

/* USB HID 1.11 Appendix E.6: boot keyboard. */
static const uint8_t desc_keyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29,
	0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05,
	0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
	0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05,
	0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

/* ---- harness ----------------------------------------------------------- */

static int g_failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++;                                        \
		}                                                            \
	} while (0)

static void check_field(const char *name, const usbhid_field_t *f,
			unsigned off, unsigned size, unsigned is_signed)
{
	if (f->bit_offset != off || f->bit_size != size ||
	    f->is_signed != is_signed) {
		printf("FAIL %s: got @%u/%u signed=%u, want @%u/%u signed=%u\n",
		       name, f->bit_offset, f->bit_size, f->is_signed, off,
		       size, is_signed);
		g_failures++;
	}
}

static void decode(const usbhid_mouse_layout_t *l, const uint8_t *rep,
		   uint32_t len)
{
	usbhid_device_t dev;
	memset(&dev, 0, sizeof(dev));
	dev.mouse_layout = *l;
	dev.mouse_layout.valid = 1;
	g_inject_calls = 0;
	hid_process_mouse_report_desc(&dev, rep, len);
}

static void test_classic(void)
{
	usbhid_mouse_layout_t l;
	CHECK(hid_parse_mouse_report_desc(desc_classic, sizeof(desc_classic),
					  &l) == 1);
	CHECK(l.has_report_id == 0);
	CHECK(l.report_bytes == 4);
	check_field("classic.buttons", &l.buttons, 0, 3, 0);
	check_field("classic.x", &l.x, 8, 8, 1);
	check_field("classic.y", &l.y, 16, 8, 1);
	check_field("classic.wheel", &l.wheel, 24, 8, 1);

	/* middle button, dx=5, dy=-5, wheel=+1 */
	const uint8_t rep[] = { 0x04, 0x05, 0xFB, 0x01 };
	decode(&l, rep, sizeof(rep));
	CHECK(g_inject_calls == 1);
	CHECK(g_buttons == 0x04 && g_dx == 5 && g_dy == -5 && g_wheel == 1);

	/* wheel down */
	const uint8_t rep2[] = { 0x00, 0x00, 0x00, 0xFF };
	decode(&l, rep2, sizeof(rep2));
	CHECK(g_inject_calls == 1 && g_wheel == -1 && g_dx == 0 && g_dy == 0);

	/* short report: nothing delivered */
	decode(&l, rep, 3);
	CHECK(g_inject_calls == 0);
}

static void test_reportid(void)
{
	usbhid_mouse_layout_t l;
	CHECK(hid_parse_mouse_report_desc(desc_reportid, sizeof(desc_reportid),
					  &l) == 1);
	CHECK(l.has_report_id == 1);
	CHECK(l.report_id == 2);
	CHECK(l.report_bytes == 5);
	check_field("reportid.buttons", &l.buttons, 0, 5, 0);
	check_field("reportid.x", &l.x, 8, 12, 1);
	check_field("reportid.y", &l.y, 20, 12, 1);
	check_field("reportid.wheel", &l.wheel, 32, 8, 1);

	/* ID 2, left+right, X=-1 (0xFFF), Y=-2048 (0x800), wheel=-1, pan=0 */
	const uint8_t rep[] = { 0x02, 0x03, 0xFF, 0x0F, 0x80, 0xFF, 0x00 };
	decode(&l, rep, sizeof(rep));
	CHECK(g_inject_calls == 1);
	CHECK(g_buttons == 0x03 && g_dx == -1 && g_dy == -2048 &&
	      g_wheel == -1);

	/* X=+2047 (0x7FF), Y=+1, wheel=+127 */
	const uint8_t rep2[] = { 0x02, 0x00, 0xFF, 0x17, 0x00, 0x7F, 0x00 };
	decode(&l, rep2, sizeof(rep2));
	CHECK(g_inject_calls == 1 && g_dx == 2047 && g_dy == 1 &&
	      g_wheel == 127 && g_buttons == 0);

	/* Consumer Control report on the same endpoint: ignored */
	const uint8_t cc[] = { 0x03, 0xE9, 0x00 };
	decode(&l, cc, sizeof(cc));
	CHECK(g_inject_calls == 0);

	/* Truncated mouse report: ignored */
	decode(&l, rep, 5);
	CHECK(g_inject_calls == 0);
}

static void test_vendor_first(void)
{
	usbhid_mouse_layout_t l;
	CHECK(hid_parse_mouse_report_desc(desc_vendor_first,
					  sizeof(desc_vendor_first), &l) == 1);
	CHECK(l.has_report_id == 1);
	CHECK(l.report_id == 1);
	CHECK(l.report_bytes == 4);
	check_field("vendor_first.x", &l.x, 8, 8, 1);
	check_field("vendor_first.wheel", &l.wheel, 24, 8, 1);
}

static void test_rejected(void)
{
	usbhid_mouse_layout_t l;
	CHECK(hid_parse_mouse_report_desc(desc_boot_nowheel,
					  sizeof(desc_boot_nowheel), &l) == 0);
	CHECK(hid_parse_mouse_report_desc(desc_keyboard, sizeof(desc_keyboard),
					  &l) == 0);
	CHECK(hid_parse_mouse_report_desc(desc_classic, 0, &l) == 0);
}

static void test_robustness(void)
{
	usbhid_mouse_layout_t l;

	/* Every prefix of a valid descriptor parses without crashing. */
	for (size_t n = 0; n < sizeof(desc_reportid); n++)
		(void)hid_parse_mouse_report_desc(desc_reportid, (uint16_t)n,
						  &l);

	/* Random descriptors: no crash, and any accepted layout is sane. */
	uint8_t buf[512];
	uint32_t seed = 12345;
	for (int iter = 0; iter < 20000; iter++) {
		size_t n = 1 + (seed >> 8) % sizeof(buf);
		for (size_t i = 0; i < n; i++) {
			seed = seed * 1103515245u + 12345u;
			buf[i] = (uint8_t)(seed >> 16);
		}
		if (hid_parse_mouse_report_desc(buf, (uint16_t)n, &l)) {
			CHECK(l.report_bytes > 0 &&
			      l.report_bytes < USBHID_REPORT_BUF_SIZE);
			CHECK(l.x.bit_size <= 32 && l.wheel.bit_size <= 32 &&
			      l.buttons.bit_size <= 8);
			/* decoding a maximal report must stay in bounds */
			uint8_t rep[USBHID_REPORT_BUF_SIZE];
			memset(rep, 0xA5, sizeof(rep));
			if (l.has_report_id)
				rep[0] = l.report_id;
			decode(&l, rep, sizeof(rep));
		}
	}
}

int main(void)
{
	test_classic();
	test_reportid();
	test_vendor_first();
	test_rejected();
	test_robustness();

	if (g_failures) {
		printf("usbhid: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("usbhid: all tests passed\n");
	return 0;
}
