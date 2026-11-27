/* Report descriptor corpus of the HID harness (host/test-hid.sh).
 *
 * Written for these tests from the HID specification and the Windows
 * precision touchpad / touchscreen descriptor requirements, shaped like the
 * devices LikeOS runs on:
 *
 *   elan_ptp      a precision touchpad like the ELAN 04F3:3169 of the HP
 *                 ProBook 650 G8: Mouse application (report 1, relative X/Y,
 *                 two buttons), Touch Pad application (report 4, five
 *                 fingers of Confidence/Tip/Contact ID/X/Y with centimetre
 *                 units and exponent -2, Scan Time, Contact Count, Button 1;
 *                 features 2 = Contact Count Maximum + Pad Type and 6 = the
 *                 256-byte certification blob), Configuration application
 *                 (feature 3 = Input Mode, feature 7 = Surface/Button
 *                 Switch)
 *   cirque_ptp    a touchpad like the Dell's VEN_0488 0488:1085: two fingers
 *                 per report (hybrid mode for more contacts), a three-button
 *                 mouse collection with a wheel and a Resolution Multiplier
 *                 feature, Latency Mode
 *   boot_mouse    a three-button wheel mouse, unnumbered reports
 *   keyboard      a keyboard with modifier bits, a 6-key array, LED output
 *                 and a Consumer Control application
 *   touchscreen   a certified two-finger touchscreen with In Range, Width,
 *                 Height
 *   malformed     descriptors every parser must reject
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_HID_CORPUS_H
#define HOST_HID_CORPUS_H

#define LO(x) ((x) & 0xff)
#define HI(x) (((x) >> 8) & 0xff)

/* Short items. */
#define UPAGE(p) 0x05, (p)
#define UPAGE2(p) 0x06, LO(p), HI(p)
#define USAGE(u) 0x09, (u)
#define USAGE2(u) 0x0a, LO(u), HI(u)
#define USAGE4(u) 0x0b, LO(u), HI(u), LO((u) >> 16), HI((u) >> 16)
#define UMIN(u) 0x19, (u)
#define UMAX(u) 0x29, (u)
#define UMAX2(u) 0x2a, LO(u), HI(u)
#define COLL(t) 0xa1, (t)
#define ENDC 0xc0
#define LMIN(v) 0x15, (v)
#define LMAX(v) 0x25, (v)
#define LMAX2(v) 0x26, LO(v), HI(v)
#define PMIN(v) 0x35, (v)
#define PMAX(v) 0x45, (v)
#define PMAX2(v) 0x46, LO(v), HI(v)
#define UEXP(v) 0x55, (v)
#define UNIT(v) 0x65, (v)
#define UNIT2(v) 0x66, LO(v), HI(v)
#define RSIZE(v) 0x75, (v)
#define RSIZE2(v) 0x76, LO(v), HI(v)
#define RCOUNT(v) 0x95, (v)
#define RCOUNT2(v) 0x96, LO(v), HI(v)
#define RID(v) 0x85, (v)
#define INPUT(f) 0x81, (f)
#define OUTPUT(f) 0x91, (f)
#define FEATURE(f) 0xb1, (f)
#define PUSH 0xa4
#define POP 0xb4
#define DELIM(v) 0xa9, (v)

#define C_PHYSICAL 0
#define C_APPLICATION 1
#define C_LOGICAL 2

// ---------------------------------------------------------------------------
// ELAN-like precision touchpad
// ---------------------------------------------------------------------------
#define ELAN_FINGER                                                        \
	UPAGE(0x0d), USAGE(0x22), COLL(C_LOGICAL),                         \
		USAGE(0x47), USAGE(0x42), LMIN(0), LMAX(1), RSIZE(1),      \
		RCOUNT(2), INPUT(0x02), RCOUNT(2), INPUT(0x03),            \
		USAGE(0x51), LMAX(15), RSIZE(4), RCOUNT(1), INPUT(0x02),   \
		UPAGE(0x01), PUSH, UNIT(0x11), UEXP(0x0e), RSIZE(16),       \
		USAGE(0x30), LMAX2(3209), PMAX2(1035), INPUT(0x02),        \
		USAGE(0x31), LMAX2(2097), PMAX2(677), INPUT(0x02), POP,     \
		ENDC

static const uint8_t elan_ptp[] = {
	/* Mouse */
	UPAGE(0x01), USAGE(0x02), COLL(C_APPLICATION),
		RID(1), USAGE(0x01), COLL(C_PHYSICAL),
			UPAGE(0x09), UMIN(1), UMAX(2), LMIN(0), LMAX(1),
			RSIZE(1), RCOUNT(2), INPUT(0x02),
			RCOUNT(6), INPUT(0x03),
			UPAGE(0x01), USAGE(0x30), USAGE(0x31), LMIN(0x81),
			LMAX(0x7f), RSIZE(8), RCOUNT(2), INPUT(0x06),
		ENDC,
	ENDC,
	/* Touch Pad */
	UPAGE(0x0d), USAGE(0x05), COLL(C_APPLICATION),
		RID(4),
		ELAN_FINGER, ELAN_FINGER, ELAN_FINGER, ELAN_FINGER, ELAN_FINGER,
		UPAGE(0x0d), UEXP(0x0c), UNIT2(0x1001), LMIN(0), LMAX2(0xffff),
		RSIZE(16), RCOUNT(1), USAGE(0x56), INPUT(0x02),
		UEXP(0), UNIT(0),
		USAGE(0x54), LMAX(127), RSIZE(8), INPUT(0x02),
		UPAGE(0x09), USAGE(0x01), LMAX(1), RSIZE(1), RCOUNT(1),
		INPUT(0x02),
		RCOUNT(7), INPUT(0x03),
		UPAGE(0x0d), RID(2), USAGE(0x55), USAGE(0x59), LMAX(15),
		RSIZE(4), RCOUNT(2), FEATURE(0x02),
		UPAGE2(0xff00), RID(6), USAGE(0xc5), LMAX2(255), RSIZE(8),
		RCOUNT2(256), FEATURE(0x02),
	ENDC,
	/* Configuration */
	UPAGE(0x0d), USAGE(0x0e), COLL(C_APPLICATION),
		RID(3), USAGE(0x22), COLL(C_LOGICAL),
			USAGE(0x52), LMIN(0), LMAX(10), RSIZE(8), RCOUNT(1),
			FEATURE(0x02),
		ENDC,
		USAGE(0x22), COLL(C_PHYSICAL),
			RID(7), USAGE(0x57), USAGE(0x58), LMAX(1), RSIZE(1),
			RCOUNT(2), FEATURE(0x02),
			RCOUNT(6), FEATURE(0x03),
		ENDC,
	ENDC,
};

/* Report 4: 1 + 5 * 5 + 2 + 1 + 1 bytes. */
#define ELAN_TP_LEN 30

// ---------------------------------------------------------------------------
// Cirque-like touchpad: two fingers per report, hi-res wheel mouse
// ---------------------------------------------------------------------------
#define CIRQUE_FINGER                                                      \
	UPAGE(0x0d), USAGE(0x22), COLL(C_LOGICAL),                         \
		USAGE(0x47), USAGE(0x42), LMIN(0), LMAX(1), RSIZE(1),      \
		RCOUNT(2), INPUT(0x02), RCOUNT(6), INPUT(0x03),            \
		USAGE(0x51), LMAX2(255), RSIZE(8), RCOUNT(1), INPUT(0x02), \
		UPAGE(0x01), UNIT(0x13), UEXP(0x0d), RSIZE(16), PMIN(0),    \
		USAGE(0x30), LMAX2(4448), PMAX2(4380), INPUT(0x02),        \
		USAGE(0x31), LMAX2(2840), PMAX2(2797), INPUT(0x02),        \
		UNIT(0), UEXP(0), PMAX(0),                                 \
		ENDC

static const uint8_t cirque_ptp[] = {
	/* Mouse with wheel and resolution multiplier */
	UPAGE(0x01), USAGE(0x02), COLL(C_APPLICATION),
		RID(1), USAGE(0x01), COLL(C_PHYSICAL),
			UPAGE(0x09), UMIN(1), UMAX(3), LMIN(0), LMAX(1),
			RSIZE(1), RCOUNT(3), INPUT(0x02),
			RCOUNT(5), INPUT(0x03),
			UPAGE(0x01), USAGE(0x30), USAGE(0x31), LMIN(0x81),
			LMAX(0x7f), RSIZE(8), RCOUNT(2), INPUT(0x06),
			COLL(C_LOGICAL),
				RID(0x0a), USAGE(0x48), LMIN(0), LMAX(1),
				PMIN(1), PMAX(8), RSIZE(2), RCOUNT(1),
				FEATURE(0x02),
				RSIZE(6), FEATURE(0x03),
				RID(1), PMIN(0), PMAX(0), USAGE(0x38),
				LMIN(0x81), LMAX(0x7f), RSIZE(8), RCOUNT(1),
				INPUT(0x06),
			ENDC,
		ENDC,
	ENDC,
	/* Touch Pad */
	UPAGE(0x0d), USAGE(0x05), COLL(C_APPLICATION),
		RID(4),
		CIRQUE_FINGER, CIRQUE_FINGER,
		UPAGE(0x0d), UEXP(0x0c), UNIT2(0x1001), LMIN(0), LMAX2(0xffff),
		RSIZE(16), RCOUNT(1), USAGE(0x56), INPUT(0x02),
		UEXP(0), UNIT(0),
		USAGE(0x54), LMAX(127), RSIZE(8), INPUT(0x02),
		UPAGE(0x09), USAGE(0x01), LMAX(1), RSIZE(1), RCOUNT(1),
		INPUT(0x02),
		RCOUNT(7), INPUT(0x03),
		UPAGE(0x0d), RID(2), USAGE(0x55), USAGE(0x59), LMAX(15),
		RSIZE(4), RCOUNT(2), FEATURE(0x02),
		UPAGE2(0xff00), RID(6), USAGE(0xc5), LMAX2(255), RSIZE(8),
		RCOUNT2(256), FEATURE(0x02),
	ENDC,
	/* Configuration */
	UPAGE(0x0d), USAGE(0x0e), COLL(C_APPLICATION),
		RID(3), USAGE(0x22), COLL(C_LOGICAL),
			USAGE(0x52), LMIN(0), LMAX(10), RSIZE(8), RCOUNT(1),
			FEATURE(0x02),
		ENDC,
		USAGE(0x22), COLL(C_PHYSICAL),
			RID(7), USAGE(0x57), USAGE(0x58), LMAX(1), RSIZE(1),
			RCOUNT(2), FEATURE(0x02),
			RCOUNT(6), FEATURE(0x03),
			RID(5), USAGE(0x60), RCOUNT(1), FEATURE(0x02),
			RCOUNT(7), FEATURE(0x03),
		ENDC,
	ENDC,
};

/* Report 4: 1 + 2 * 6 + 2 + 1 + 1 bytes. */
#define CIRQUE_TP_LEN 17

// ---------------------------------------------------------------------------
// Boot mouse
// ---------------------------------------------------------------------------
static const uint8_t boot_mouse[] = {
	UPAGE(0x01), USAGE(0x02), COLL(C_APPLICATION),
		USAGE(0x01), COLL(C_PHYSICAL),
			UPAGE(0x09), UMIN(1), UMAX(3), LMIN(0), LMAX(1),
			RCOUNT(3), RSIZE(1), INPUT(0x02),
			RCOUNT(1), RSIZE(5), INPUT(0x03),
			UPAGE(0x01), USAGE(0x30), USAGE(0x31), USAGE(0x38),
			LMIN(0x81), LMAX(0x7f), RSIZE(8), RCOUNT(3),
			INPUT(0x06),
		ENDC,
	ENDC,
};

// ---------------------------------------------------------------------------
// Keyboard + LEDs + consumer control
// ---------------------------------------------------------------------------
static const uint8_t keyboard[] = {
	UPAGE(0x01), USAGE(0x06), COLL(C_APPLICATION),
		RID(1),
		UPAGE(0x07), UMIN(0xe0), UMAX(0xe7), LMIN(0), LMAX(1),
		RSIZE(1), RCOUNT(8), INPUT(0x02),
		RCOUNT(1), RSIZE(8), INPUT(0x03),
		RCOUNT(5), RSIZE(1), UPAGE(0x08), UMIN(1), UMAX(5),
		OUTPUT(0x02),
		RCOUNT(1), RSIZE(3), OUTPUT(0x03),
		RCOUNT(6), RSIZE(8), LMIN(0), LMAX(0x65), UPAGE(0x07),
		UMIN(0), UMAX(0x65), INPUT(0x00),
	ENDC,
	UPAGE(0x0c), USAGE(0x01), COLL(C_APPLICATION),
		RID(2), LMIN(0), LMAX2(0x29c), UMIN(0), UMAX2(0x29c),
		RSIZE(16), RCOUNT(1), INPUT(0x00),
	ENDC,
};

// ---------------------------------------------------------------------------
// Certified touchscreen
// ---------------------------------------------------------------------------
#define TS_FINGER                                                          \
	UPAGE(0x0d), USAGE(0x22), COLL(C_LOGICAL),                         \
		USAGE(0x42), USAGE(0x32), LMIN(0), LMAX(1), RSIZE(1),      \
		RCOUNT(2), INPUT(0x02), RCOUNT(6), INPUT(0x03),            \
		USAGE(0x51), LMAX2(255), RSIZE(8), RCOUNT(1), INPUT(0x02), \
		UPAGE(0x01), UNIT(0x11), UEXP(0x0e), RSIZE(16),             \
		USAGE(0x30), LMAX2(4095), PMAX2(2000), INPUT(0x02),        \
		USAGE(0x31), LMAX2(4095), PMAX2(1200), INPUT(0x02),        \
		UNIT(0), UEXP(0), PMAX(0),                                 \
		UPAGE(0x0d), USAGE(0x48), USAGE(0x49), LMAX2(255),          \
		RSIZE(8), RCOUNT(2), INPUT(0x02),                          \
		ENDC

static const uint8_t touchscreen[] = {
	UPAGE(0x0d), USAGE(0x04), COLL(C_APPLICATION),
		RID(1),
		TS_FINGER, TS_FINGER,
		UPAGE(0x0d), USAGE(0x54), LMAX(10), RSIZE(8), RCOUNT(1),
		INPUT(0x02),
		RID(2), USAGE(0x55), LMAX(10), RSIZE(8), RCOUNT(1),
		FEATURE(0x02),
		UPAGE2(0xff00), RID(6), USAGE(0xc5), LMAX2(255), RSIZE(8),
		RCOUNT2(256), FEATURE(0x02),
	ENDC,
	UPAGE(0x0d), USAGE(0x0e), COLL(C_APPLICATION),
		RID(3), USAGE(0x22), COLL(C_LOGICAL),
			USAGE(0x52), USAGE(0x53), LMIN(0), LMAX(10), RSIZE(8),
			RCOUNT(2), FEATURE(0x02),
		ENDC,
	ENDC,
};

/* Report 1: 1 + 2 * 8 + 1 bytes. */
#define TS_LEN 18

// ---------------------------------------------------------------------------
// Malformed descriptors: each must be rejected (and nothing leak).
// ---------------------------------------------------------------------------
static const uint8_t bad_truncated_long[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), 0xfe, 0x05, 0x10, 0x01
};
static const uint8_t bad_long_item[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), 0xfe, 0x01, 0x10, 0x00, ENDC
};
static const uint8_t bad_truncated_short[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), RSIZE(8), RCOUNT(1), USAGE(0x30),
	INPUT(2), ENDC, 0x27, 0x01, 0x02
};
static const uint8_t bad_push_overflow[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), PUSH, PUSH, PUSH, PUSH, PUSH, ENDC
};
static const uint8_t bad_pop_underflow[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), POP, ENDC
};
static const uint8_t bad_report_size[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), RSIZE2(257), RCOUNT(1), USAGE(0x30),
	INPUT(2), ENDC
};
static const uint8_t bad_usage_range[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), UMIN(0), UMAX2(0xffff), UMIN(0),
	UMAX2(0xffff), RSIZE(1), RCOUNT(1), INPUT(2), ENDC
};
static const uint8_t bad_collection_underflow[] = {
	UPAGE(0x01), USAGE(0x02), ENDC, ENDC
};
static const uint8_t bad_unbalanced[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), USAGE(0x01), COLL(0), RSIZE(8),
	RCOUNT(1), USAGE(0x30), INPUT(2), ENDC
};
static const uint8_t bad_report_count[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), RSIZE(8), RCOUNT2(12289),
	USAGE(0x30), INPUT(2), ENDC
};
static const uint8_t bad_report_id_zero[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), RID(0), ENDC
};
static const uint8_t bad_report_too_long[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), RSIZE2(256), RCOUNT2(600),
	USAGE(0x30), INPUT(2), ENDC
};
static const uint8_t bad_logical_range[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), LMIN(0x10), LMAX(0x05), RSIZE(8),
	RCOUNT(1), USAGE(0x30), INPUT(2), ENDC
};
static const uint8_t bad_nested_delimiter[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), DELIM(1), DELIM(1), ENDC
};
static const uint8_t bad_close_delimiter[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), DELIM(0), ENDC
};
static const uint8_t bad_open_delimiter[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), DELIM(1), USAGE(0x30)
};
static const uint8_t bad_unknown_global[] = {
	UPAGE(0x01), USAGE(0x02), COLL(1), 0xc4, 0x01, ENDC
};
struct bad_desc {
	const char *name;
	const uint8_t *bytes;
	unsigned int len;
};

#define BAD(x) { #x, x, sizeof(x) }
static const struct bad_desc bad_descs[] = {
	BAD(bad_truncated_long),	BAD(bad_long_item),
	BAD(bad_truncated_short),	BAD(bad_push_overflow),
	BAD(bad_pop_underflow),		BAD(bad_report_size),
	BAD(bad_usage_range),		BAD(bad_collection_underflow),
	BAD(bad_unbalanced),		BAD(bad_report_count),
	BAD(bad_report_id_zero),	BAD(bad_report_too_long),
	BAD(bad_logical_range),		BAD(bad_nested_delimiter),
	BAD(bad_close_delimiter),	BAD(bad_open_delimiter),
	BAD(bad_unknown_global),
};
#undef BAD

#endif
