// LikeOS hid-multitouch - touchpads and touchscreens that report contacts
//
// A multitouch HID device sends, per frame, a set of contacts: each one a
// group of fields (Tip Switch, Confidence, Contact Identifier, X, Y, maybe
// Width/Height/Azimuth/Pressure) inside a Finger logical collection, plus
// frame-wide fields (Contact Count, Scan Time, the physical buttons).  This
// driver finds those groups when hid-input maps the touch application's
// reports, keeps pointers to the field values of every contact, and on each
// report turns them into the input core's multitouch slot protocol:
// one slot per contact, a tracking id per touch, MT_TOOL_PALM for contacts
// the device does not trust, BTN_LEFT for the clickpad click, MSC_TIMESTAMP
// from the device's scan time.  Other applications of the same device (the
// touchpad's legacy Mouse collection, a keyboard's consumer keys) are left
// to hid-input and become input devices of their own.
//
// Devices differ in how they mark a contact as valid, how they number
// contacts, whether a frame fits one report, and more.  Those differences
// are classes of quirks (struct mt_class), chosen by the device table; a
// "Windows 8 certified" device (the 0xff0000c5 certification blob in its
// descriptor) gets the strict class those devices promise to follow.
//
// Device modes: a precision touchpad starts in mouse mode and reports only
// its Mouse collection.  The Input Mode feature (Digitizer 0x52) switches
// it to touchpad mode (3), touchscreens to multi-input mode (2); the
// Surface and Button Switch features turn contact and button reporting on
// and off; Latency Mode selects normal or power-saving report rates.
//
// Hybrid devices send more contacts than fit one report over several
// reports: the first carries the contact count of the whole frame, the
// rest a count of zero.  The frame is reported once all contacts arrived.
//
// Sticky fingers: a device that loses a "contact lifted" report would leave
// a finger down for ever.  Certified devices report at least every few
// milliseconds while a contact is down, so with contacts down and no report
// for 100 ms every contact is released.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2009-2013 Stephane Chatty, (C) 2010-2013 Benjamin
// Tissoires, (C) 2010 Henrik Rydberg, (C) 2010-2012 Ecole Nationale de
// l'Aviation Civile, (C) 2010 Canonical Ltd., (C) 2012-2013 Red Hat, Inc.
// (device class and device tables, quirk semantics)

#include <kernel/dev/hid/core/hid.h>
#include <kernel/dev/input/input-mt.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/ke/timer.h>

// ---------------------------------------------------------------------------
// Quirks
// ---------------------------------------------------------------------------
#define MT_QUIRK_NOT_SEEN_MEANS_UP (1u << 0) // contacts not reported are up
#define MT_QUIRK_SLOT_IS_CONTACTID (1u << 1)
#define MT_QUIRK_CYPRESS (1u << 2)
#define MT_QUIRK_SLOT_IS_CONTACTNUMBER (1u << 3)
#define MT_QUIRK_ALWAYS_VALID (1u << 4)
#define MT_QUIRK_VALID_IS_INRANGE (1u << 5)
#define MT_QUIRK_VALID_IS_CONFIDENCE (1u << 6)
#define MT_QUIRK_CONFIDENCE (1u << 7) // !confidence -> MT_TOOL_PALM
#define MT_QUIRK_SLOT_IS_CONTACTID_MINUS_ONE (1u << 8)
#define MT_QUIRK_NO_AREA (1u << 9)
#define MT_QUIRK_IGNORE_DUPLICATES (1u << 10)
#define MT_QUIRK_HOVERING (1u << 11)
#define MT_QUIRK_CONTACT_CNT_ACCURATE (1u << 12)
#define MT_QUIRK_FORCE_GET_FEATURE (1u << 13)
#define MT_QUIRK_FIX_CONST_CONTACT_ID (1u << 14)
#define MT_QUIRK_TOUCH_SIZE_SCALING (1u << 15)
#define MT_QUIRK_STICKY_FINGERS (1u << 16)
#define MT_QUIRK_ASUS_CUSTOM_UP (1u << 17)
#define MT_QUIRK_WIN8_PTP_BUTTONS (1u << 18)
#define MT_QUIRK_SEPARATE_APP_REPORT (1u << 19)
#define MT_QUIRK_FORCE_MULTI_INPUT (1u << 20)
#define MT_QUIRK_DISABLE_WAKEUP (1u << 21)
#define MT_QUIRK_ORIENTATION_INVERT (1u << 22)
#define MT_QUIRK_APPLE_TOUCHBAR (1u << 23) // tip state in Touch, id in index
#define MT_QUIRK_YOGABOOK9I (1u << 24)
#define MT_QUIRK_KEEP_LATENCY_ON_CLOSE (1u << 25)
#define MT_QUIRK_IGNORE_FEATURE_ID_MISMATCH (1u << 26)

#define MT_INPUTMODE_TOUCHSCREEN 0x02
#define MT_INPUTMODE_TOUCHPAD 0x03

#define MT_BUTTONTYPE_CLICKPAD 0
#define MT_BUTTONTYPE_PRESSUREPAD 1

enum latency_mode {
	HID_LATENCY_NORMAL = 0,
	HID_LATENCY_HIGH = 1,
};

#define TOUCHPAD_REPORT_NONE 0
#define TOUCHPAD_REPORT_BUTTONS (1u << 0)
#define TOUCHPAD_REPORT_CONTACTS (1u << 1)
#define TOUCHPAD_REPORT_ALL (TOUCHPAD_REPORT_BUTTONS | TOUCHPAD_REPORT_CONTACTS)

/* td->mt_io_flags bit: a report or the sticky release is being processed. */
#define MT_IO_FLAGS_RUNNING 0

#define MT_DEFAULT_MAXCONTACT 10
#define MT_MAX_MAXCONTACT 250
/* The input core's slot limit. */
#define MT_INPUT_MAX_SLOTS 64

/* Device and local timestamps resynchronise after this long without a
 * report (microseconds). */
#define MAX_TIMESTAMP_INTERVAL 1000000

#define MT_STICKY_RELEASE_MS 100

/* Defaults the usage pointers start with, so the report path never meets
 * NULL: distinct objects, compared by address. */
static int32_t mt_default_true = 1;
static int32_t mt_default_false;
static int32_t mt_default_zero;
#define DEFAULT_TRUE ((const int32_t *)&mt_default_true)
#define DEFAULT_FALSE ((const int32_t *)&mt_default_false)
#define DEFAULT_ZERO ((const int32_t *)&mt_default_zero)

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

/* One contact of a report: pointers to the field values that describe it
 * (they live as long as the parsed descriptor). */
struct mt_usages {
	struct list_head list;
	const int32_t *x, *y, *cx, *cy, *p, *w, *h, *a;
	const int32_t *contactid; // the device's identifier of the contact
	const int32_t *tip_state; // touching
	const int32_t *inrange_state; // in proximity
	const int32_t *confidence_state; // a finger, not a palm
};

/* One touch application (a touchpad, a touchscreen) of the device. */
struct mt_application {
	struct list_head list;
	unsigned int application;
	unsigned int report_id;
	struct list_head mt_usages; // struct mt_usages, one per contact
	uint32_t quirks;

	const int32_t *scantime; // the frame's scan time
	int32_t scantime_logical_max;
	const int32_t *raw_cc; // the contact count field
	int left_button_state;
	unsigned int mt_flags; // INPUT_MT_* for input_mt_init_slots
	unsigned long *pending_palm_slots; // palms to release next frame
	unsigned int palm_bits;

	uint8_t num_received; // contacts of the frame so far
	uint8_t num_expected; // contacts the frame announced
	uint8_t buttons_count;
	uint8_t touches_by_report; // contacts one report can carry

	uint64_t last_report_ns; // local time of the previous report
	int timestamp; // the MSC_TIMESTAMP of the frame
	int prev_scantime;
	bool have_contact_count;
};

struct mt_class {
	int32_t name; // MT_CLS_*
	uint32_t quirks;
	int32_t sn_move; // signal/noise ratio for movement (fuzz)
	int32_t sn_width;
	int32_t sn_height;
	int32_t sn_pressure;
	uint8_t maxcontacts;
	bool is_indirect; // a touchpad
	bool export_all_inputs; // map the other applications too
};

/* What the driver knows about one report of the device. */
struct mt_report_data {
	struct list_head list;
	struct hid_report *report;
	struct mt_application *application;
	bool is_mt_collection; // carries contacts
};

struct mt_device {
	struct mt_class mtclass;
	struct delayed_work release_work; // sticky finger release
	struct hid_device *hdev;
	unsigned long mt_io_flags;
	/* Slots holding an active contact (maxcontacts bits): contacts that
	 * a lost lift report would leave down. */
	unsigned long *active_slots;
	uint8_t inputmode_value;
	uint8_t maxcontacts;
	bool is_buttonpad; // one physical button under the surface
	bool is_pressurepad;
	bool serial_maybe; // generic match: maybe one contact per report
	struct list_head applications;
	struct list_head reports;
};

// ---------------------------------------------------------------------------
// Classes: what a kind of device needs.  The table below decides a device's
// class; certified devices without an entry of their own get MT_CLS_WIN_8,
// other contact devices MT_CLS_DEFAULT.
// ---------------------------------------------------------------------------
/* classes of device behavior */
#define MT_CLS_DEFAULT				0x0001

#define MT_CLS_SERIAL				0x0002
#define MT_CLS_CONFIDENCE			0x0003
#define MT_CLS_CONFIDENCE_CONTACT_ID		0x0004
#define MT_CLS_CONFIDENCE_MINUS_ONE		0x0005
#define MT_CLS_DUAL_INRANGE_CONTACTID		0x0006
#define MT_CLS_DUAL_INRANGE_CONTACTNUMBER	0x0007
/* reserved					0x0008 */
#define MT_CLS_INRANGE_CONTACTNUMBER		0x0009
#define MT_CLS_NSMU				0x000a
/* reserved					0x0010 */
/* reserved					0x0011 */
#define MT_CLS_WIN_8				0x0012
#define MT_CLS_EXPORT_ALL_INPUTS		0x0013
/* reserved					0x0014 */
#define MT_CLS_WIN_8_FORCE_MULTI_INPUT		0x0015
#define MT_CLS_WIN_8_DISABLE_WAKEUP		0x0016
#define MT_CLS_WIN_8_NO_STICKY_FINGERS		0x0017
#define MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU	0x0018
#define MT_CLS_WIN_8_KEEP_LATENCY_ON_CLOSE	0x0019

/* vendor specific classes */
#define MT_CLS_3M				0x0101
/* reserved					0x0102 */
#define MT_CLS_EGALAX				0x0103
#define MT_CLS_EGALAX_SERIAL			0x0104
#define MT_CLS_TOPSEED				0x0105
#define MT_CLS_PANASONIC			0x0106
#define MT_CLS_FLATFROG				0x0107
#define MT_CLS_GENERALTOUCH_TWOFINGERS		0x0108
#define MT_CLS_GENERALTOUCH_PWT_TENFINGERS	0x0109
#define MT_CLS_LG				0x010a
#define MT_CLS_ASUS				0x010b
#define MT_CLS_VTL				0x0110
#define MT_CLS_GOOGLE				0x0111
#define MT_CLS_RAZER_BLADE_STEALTH		0x0112
#define MT_CLS_SMART_TECH			0x0113
#define MT_CLS_APPLE_TOUCHBAR			0x0114
#define MT_CLS_YOGABOOK9I			0x0115
#define MT_CLS_EGALAX_P80H84			0x0116
#define MT_CLS_ASUS_ROG_Z13_FOLIO		0x0117
#define MT_CLS_SIS				0x0457

static const struct mt_class mt_classes[] = {
	{ .name = MT_CLS_DEFAULT,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE },
	{ .name = MT_CLS_NSMU,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP },
	{ .name = MT_CLS_SERIAL,
		.quirks = MT_QUIRK_ALWAYS_VALID},
	{ .name = MT_CLS_CONFIDENCE,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE },
	{ .name = MT_CLS_CONFIDENCE_CONTACT_ID,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE |
			MT_QUIRK_SLOT_IS_CONTACTID },
	{ .name = MT_CLS_CONFIDENCE_MINUS_ONE,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE |
			MT_QUIRK_SLOT_IS_CONTACTID_MINUS_ONE },
	{ .name = MT_CLS_DUAL_INRANGE_CONTACTID,
		.quirks = MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTID,
		.maxcontacts = 2 },
	{ .name = MT_CLS_DUAL_INRANGE_CONTACTNUMBER,
		.quirks = MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTNUMBER,
		.maxcontacts = 2 },
	{ .name = MT_CLS_INRANGE_CONTACTNUMBER,
		.quirks = MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTNUMBER },
	{ .name = MT_CLS_WIN_8,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS |
			MT_QUIRK_WIN8_PTP_BUTTONS,
		.export_all_inputs = true },
	{ .name = MT_CLS_EXPORT_ALL_INPUTS,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE,
		.export_all_inputs = true },
	{ .name = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS |
			MT_QUIRK_WIN8_PTP_BUTTONS |
			MT_QUIRK_FORCE_MULTI_INPUT,
		.export_all_inputs = true },
	{ .name = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
		.quirks = MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS |
			MT_QUIRK_WIN8_PTP_BUTTONS |
			MT_QUIRK_FORCE_MULTI_INPUT |
			MT_QUIRK_NOT_SEEN_MEANS_UP,
		.export_all_inputs = true },
	{ .name = MT_CLS_WIN_8_DISABLE_WAKEUP,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS |
			MT_QUIRK_WIN8_PTP_BUTTONS |
			MT_QUIRK_DISABLE_WAKEUP,
		.export_all_inputs = true },
	{ .name = MT_CLS_WIN_8_NO_STICKY_FINGERS,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_WIN8_PTP_BUTTONS,
		.export_all_inputs = true },
	{ .name = MT_CLS_WIN_8_KEEP_LATENCY_ON_CLOSE,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS |
			MT_QUIRK_WIN8_PTP_BUTTONS |
			MT_QUIRK_KEEP_LATENCY_ON_CLOSE,
		.export_all_inputs = true },

	/*
	 * vendor specific classes
	 */
	{ .name = MT_CLS_3M,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE |
			MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_TOUCH_SIZE_SCALING,
		.sn_move = 2048,
		.sn_width = 128,
		.sn_height = 128,
		.maxcontacts = 60,
	},
	{ .name = MT_CLS_EGALAX,
		.quirks =  MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_VALID_IS_INRANGE,
		.sn_move = 4096,
		.sn_pressure = 32,
	},
	{ .name = MT_CLS_EGALAX_SERIAL,
		.quirks =  MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_ALWAYS_VALID,
		.sn_move = 4096,
		.sn_pressure = 32,
	},
	{ .name = MT_CLS_TOPSEED,
		.quirks = MT_QUIRK_ALWAYS_VALID,
		.is_indirect = true,
		.maxcontacts = 2,
	},
	{ .name = MT_CLS_PANASONIC,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP,
		.maxcontacts = 4 },
	{ .name	= MT_CLS_GENERALTOUCH_TWOFINGERS,
		.quirks	= MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTID,
		.maxcontacts = 2
	},
	{ .name	= MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		.quirks	= MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_SLOT_IS_CONTACTID
	},

	{ .name = MT_CLS_FLATFROG,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_NO_AREA,
		.sn_move = 2048,
		.maxcontacts = 40,
	},
	{ .name = MT_CLS_LG,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_FIX_CONST_CONTACT_ID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE },
	{ .name = MT_CLS_ASUS,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_ASUS_CUSTOM_UP },
	{ .name = MT_CLS_ASUS_ROG_Z13_FOLIO,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS |
			MT_QUIRK_WIN8_PTP_BUTTONS |
			MT_QUIRK_CONFIDENCE |
			MT_QUIRK_IGNORE_FEATURE_ID_MISMATCH,
		.export_all_inputs = true },
	{ .name = MT_CLS_VTL,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS |
			MT_QUIRK_FORCE_GET_FEATURE,
	},
	{ .name = MT_CLS_GOOGLE,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_HOVERING
	},
	{ .name = MT_CLS_RAZER_BLADE_STEALTH,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_WIN8_PTP_BUTTONS,
	},
	{ .name = MT_CLS_SMART_TECH,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_SEPARATE_APP_REPORT,
	},
	{ .name = MT_CLS_APPLE_TOUCHBAR,
		.quirks = MT_QUIRK_HOVERING |
			MT_QUIRK_SLOT_IS_CONTACTID_MINUS_ONE |
			MT_QUIRK_APPLE_TOUCHBAR,
		.maxcontacts = 11,
	},
	{ .name = MT_CLS_SIS,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE,
	},
		{ .name = MT_CLS_YOGABOOK9I,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_FORCE_MULTI_INPUT |
			MT_QUIRK_SEPARATE_APP_REPORT |
			MT_QUIRK_HOVERING |
			MT_QUIRK_YOGABOOK9I,
		.maxcontacts = 10,
		.export_all_inputs = true
	},
	{ .name = MT_CLS_EGALAX_P80H84,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_CONTACT_CNT_ACCURATE,
	},
	{ }
};

// ---------------------------------------------------------------------------
// Devices.  Only devices that need something other than the generic
// handling, and the two generic rules at the end.
// ---------------------------------------------------------------------------
#define MT_USB_DEVICE(v, p) HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH, v, p)
#define MT_BT_DEVICE(v, p) HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_MULTITOUCH, v, p)

static const struct hid_device_id mt_devices[] = {

	/* 3M panels */
	{ .driver_data = MT_CLS_3M,
		MT_USB_DEVICE(USB_VENDOR_ID_3M,
			USB_DEVICE_ID_3M1968) },
	{ .driver_data = MT_CLS_3M,
		MT_USB_DEVICE(USB_VENDOR_ID_3M,
			USB_DEVICE_ID_3M2256) },
	{ .driver_data = MT_CLS_3M,
		MT_USB_DEVICE(USB_VENDOR_ID_3M,
			USB_DEVICE_ID_3M3266) },

	/* Anton devices */
	{ .driver_data = MT_CLS_EXPORT_ALL_INPUTS,
		MT_USB_DEVICE(USB_VENDOR_ID_ANTON,
			USB_DEVICE_ID_ANTON_TOUCH_PAD) },

	/* Asus T101HA */
	{ .driver_data = MT_CLS_WIN_8_DISABLE_WAKEUP,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_ASUSTEK,
			   USB_DEVICE_ID_ASUSTEK_T101HA_KEYBOARD) },

	/* Asus T304UA */
	{ .driver_data = MT_CLS_ASUS,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ASUSTEK,
			USB_DEVICE_ID_ASUSTEK_T304_KEYBOARD) },

	/* Atmel panels */
	{ .driver_data = MT_CLS_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_ATMEL,
			USB_DEVICE_ID_ATMEL_MXT_DIGITIZER) },

	/* Baanto multitouch devices */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_BAANTO,
			USB_DEVICE_ID_BAANTO_MT_190W2) },

	/* Cando panels */
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_MULTI_TOUCH) },
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_MULTI_TOUCH_15_6) },

	/* Chunghwa Telecom touch panels */
	{  .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CHUNGHWAT,
			USB_DEVICE_ID_CHUNGHWAT_MULTITOUCH) },

	/* CJTouch panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CJTOUCH,
			USB_DEVICE_ID_CJTOUCH_MULTI_TOUCH_0020) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CJTOUCH,
			USB_DEVICE_ID_CJTOUCH_MULTI_TOUCH_0040) },

	/* CVTouch panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CVTOUCH,
			USB_DEVICE_ID_CVTOUCH_SCREEN) },

	/* eGalax devices (SAW) */
	{ .driver_data = MT_CLS_EXPORT_ALL_INPUTS,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_EGALAX_TOUCHCONTROLLER) },

	/* eGalax devices (resistive) */
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_480D) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_480E) },

	/* eGalax devices (capacitive) */
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7207) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_720C) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7224) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_722A) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_725E) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7262) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_726B) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72A1) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72AA) },
	{ .driver_data = MT_CLS_EGALAX,
		HID_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72C4) },
	{ .driver_data = MT_CLS_EGALAX,
		HID_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72D0) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72FA) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7302) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7349) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_73F7) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_A001) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_C000) },
	{ .driver_data = MT_CLS_EGALAX_P80H84,
		HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_C002) },

	/* Elan devices */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ELAN, 0x313a) },

	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ELAN, 0x3148) },

	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ELAN, 0x32ae) },

	/* Elitegroup panel */
	{ .driver_data = MT_CLS_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_ELITEGROUP,
			USB_DEVICE_ID_ELITEGROUP_05D8) },

	/* Flatfrog Panels */
	{ .driver_data = MT_CLS_FLATFROG,
		MT_USB_DEVICE(USB_VENDOR_ID_FLATFROG,
			USB_DEVICE_ID_MULTITOUCH_3200) },

	/* FocalTech Panels */
	{ .driver_data = MT_CLS_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_CYGNAL,
			USB_DEVICE_ID_FOCALTECH_FTXXXX_MULTITOUCH) },

	/* GeneralTouch panel */
	{ .driver_data = MT_CLS_GENERALTOUCH_TWOFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN7_TWOFINGERS) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PWT_TENFINGERS) },
	{ .driver_data = MT_CLS_GENERALTOUCH_TWOFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_0101) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_0102) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_0106) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_010A) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_E100) },

	/* Gametel game controller */
	{ .driver_data = MT_CLS_NSMU,
		MT_BT_DEVICE(USB_VENDOR_ID_FRUCTEL,
			USB_DEVICE_ID_GAMETEL_MT_MODE) },

	/* Goodix GT7868Q devices */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
	  HID_DEVICE(BUS_I2C, HID_GROUP_ANY, I2C_VENDOR_ID_GOODIX,
		     I2C_DEVICE_ID_GOODIX_01E8) },
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
	  HID_DEVICE(BUS_I2C, HID_GROUP_ANY, I2C_VENDOR_ID_GOODIX,
		     I2C_DEVICE_ID_GOODIX_01E9) },

	/* GoodTouch panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_GOODTOUCH,
			USB_DEVICE_ID_GOODTOUCH_000f) },

	/* Hanvon panels */
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTID,
		MT_USB_DEVICE(USB_VENDOR_ID_HANVON_ALT,
			USB_DEVICE_ID_HANVON_ALT_MULTITOUCH) },

	/* HONOR GLO-GXXX panel */
	{ .driver_data = MT_CLS_VTL,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			0x347d, 0x7853) },

	/* HONOR MagicBook Art 14 touchpad */
	{ .driver_data = MT_CLS_VTL,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			0x35cc, 0x0104) },

	/* Ilitek dual touch panel */
	{  .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_ILITEK,
			USB_DEVICE_ID_ILITEK_MULTITOUCH) },

	/* LG Melfas panel */
	{ .driver_data = MT_CLS_LG,
		HID_USB_DEVICE(USB_VENDOR_ID_LG,
			USB_DEVICE_ID_LG_MELFAS_MT) },
	{ .driver_data = MT_CLS_LG,
		HID_DEVICE(BUS_I2C, HID_GROUP_GENERIC,
			USB_VENDOR_ID_LG, I2C_DEVICE_ID_LG_7010) },

	/* Lenovo X1 TAB Gen 1 */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_LENOVO,
			   USB_DEVICE_ID_LENOVO_X1_TAB) },

	/* Lenovo X1 TAB Gen 2 */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_LENOVO,
			   USB_DEVICE_ID_LENOVO_X1_TAB2) },

	/* Lenovo X1 TAB Gen 3 */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_LENOVO,
			   USB_DEVICE_ID_LENOVO_X1_TAB3) },

	/* Lenovo X12 TAB Gen 1 */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_LENOVO,
			   USB_DEVICE_ID_LENOVO_X12_TAB) },

	/* Lenovo X12 TAB Gen 2 */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_LENOVO,
			   USB_DEVICE_ID_LENOVO_X12_TAB2) },

	/* Lenovo Yoga Book 9i */
	{ .driver_data = MT_CLS_YOGABOOK9I,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_LENOVO,
			   USB_DEVICE_ID_LENOVO_YOGABOOK9I) },

	/* Logitech devices */
	{ .driver_data = MT_CLS_NSMU,
		HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_LOGITECH,
			USB_DEVICE_ID_LOGITECH_CASA_TOUCHPAD) },
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_LOGITECH,
			USB_DEVICE_ID_LOGITECH_BOLT_RECEIVER) },

	/* MosArt panels */
	{ .driver_data = MT_CLS_CONFIDENCE_MINUS_ONE,
		MT_USB_DEVICE(USB_VENDOR_ID_ASUS,
			USB_DEVICE_ID_ASUS_T91MT)},
	{ .driver_data = MT_CLS_CONFIDENCE_MINUS_ONE,
		MT_USB_DEVICE(USB_VENDOR_ID_ASUS,
			USB_DEVICE_ID_ASUSTEK_MULTITOUCH_YFO) },
	{ .driver_data = MT_CLS_CONFIDENCE_MINUS_ONE,
		MT_USB_DEVICE(USB_VENDOR_ID_TURBOX,
			USB_DEVICE_ID_TURBOX_TOUCHSCREEN_MOSART) },

	/* Novatek Panel */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_NOVATEK,
			USB_DEVICE_ID_NOVATEK_PCT) },

	/* Ntrig Panel */
	{ .driver_data = MT_CLS_NSMU,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_NTRIG, 0x1b05) },

	/* Panasonic panels */
	{ .driver_data = MT_CLS_PANASONIC,
		MT_USB_DEVICE(USB_VENDOR_ID_PANASONIC,
			USB_DEVICE_ID_PANABOARD_UBT780) },
	{ .driver_data = MT_CLS_PANASONIC,
		MT_USB_DEVICE(USB_VENDOR_ID_PANASONIC,
			USB_DEVICE_ID_PANABOARD_UBT880) },

	/* PixArt optical touch screen */
	{ .driver_data = MT_CLS_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_PIXART,
			USB_DEVICE_ID_PIXART_OPTICAL_TOUCH_SCREEN) },
	{ .driver_data = MT_CLS_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_PIXART,
			USB_DEVICE_ID_PIXART_OPTICAL_TOUCH_SCREEN1) },
	{ .driver_data = MT_CLS_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_PIXART,
			USB_DEVICE_ID_PIXART_OPTICAL_TOUCH_SCREEN2) },

	/* PixCir-based panels */
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTID,
		MT_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_PIXCIR_MULTI_TOUCH) },

	/* Quanta-based panels */
	{ .driver_data = MT_CLS_CONFIDENCE_CONTACT_ID,
		MT_USB_DEVICE(USB_VENDOR_ID_QUANTA,
			USB_DEVICE_ID_QUANTA_OPTICAL_TOUCH_3001) },

	/* Razer touchpads */
	{ .driver_data = MT_CLS_RAZER_BLADE_STEALTH,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_SYNAPTICS, 0x8323) },

	/* Smart Tech panels */
	{ .driver_data = MT_CLS_SMART_TECH,
		MT_USB_DEVICE(0x0b8c, 0x0092)},

	/* Stantum panels */
	{ .driver_data = MT_CLS_CONFIDENCE,
		MT_USB_DEVICE(USB_VENDOR_ID_STANTUM_STM,
			USB_DEVICE_ID_MTP_STM)},

	/* Synaptics devices */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_SYNAPTICS, 0xcd7e) },

	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_SYNAPTICS, 0xcddc) },

	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_SYNAPTICS, 0xce08) },

	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_SYNAPTICS, 0xce09) },

	/* TopSeed panels */
	{ .driver_data = MT_CLS_TOPSEED,
		MT_USB_DEVICE(USB_VENDOR_ID_TOPSEED2,
			USB_DEVICE_ID_TOPSEED2_PERIPAD_701) },

	/* Touch International panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_TOUCH_INTL,
			USB_DEVICE_ID_TOUCH_INTL_MULTI_TOUCH) },

	/* Unitec panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_UNITEC,
			USB_DEVICE_ID_UNITEC_USB_TOUCH_0709) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_UNITEC,
			USB_DEVICE_ID_UNITEC_USB_TOUCH_0A19) },

	/* Uniwill touchpads */
	{ .driver_data = MT_CLS_WIN_8_KEEP_LATENCY_ON_CLOSE,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_PIXART, 0x0255) },
	{ .driver_data = MT_CLS_WIN_8_KEEP_LATENCY_ON_CLOSE,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_PIXART, 0x0274) },

	/* VTL panels */
	{ .driver_data = MT_CLS_VTL,
		MT_USB_DEVICE(USB_VENDOR_ID_VTL,
			USB_DEVICE_ID_VTL_MULTITOUCH_FF3F) },

	/* Winbond Electronics Corp. */
	{ .driver_data = MT_CLS_WIN_8_NO_STICKY_FINGERS,
		HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_WINBOND, USB_DEVICE_ID_TSTP_MTOUCH) },

	/* Wistron panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_WISTRON,
			USB_DEVICE_ID_WISTRON_OPTICAL_TOUCH) },

	/* XAT */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XAT,
			USB_DEVICE_ID_XAT_CSR) },

	/* Xiroku */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_SPX) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_MPX) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_CSR) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_SPX1) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_MPX1) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_CSR1) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_SPX2) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_MPX2) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_CSR2) },

	/* Apple Touch Bar */
	{ .driver_data = MT_CLS_APPLE_TOUCHBAR,
		HID_USB_DEVICE(USB_VENDOR_ID_APPLE,
			USB_DEVICE_ID_APPLE_TOUCHBAR_DISPLAY) },

	/* Google MT devices */
	{ .driver_data = MT_CLS_GOOGLE,
		HID_DEVICE(HID_BUS_ANY, HID_GROUP_ANY, USB_VENDOR_ID_GOOGLE,
			USB_DEVICE_ID_GOOGLE_TOUCH_ROSE) },
	{ .driver_data = MT_CLS_GOOGLE,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8, USB_VENDOR_ID_GOOGLE,
			USB_DEVICE_ID_GOOGLE_WHISKERS) },

	/* sis */
	{ .driver_data = MT_CLS_SIS,
		HID_DEVICE(HID_BUS_ANY, HID_GROUP_ANY, USB_VENDOR_ID_SIS_TOUCH,
			HID_ANY_ID) },

	/* Hantick */
	{ .driver_data = MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			   I2C_VENDOR_ID_HANTICK, I2C_PRODUCT_ID_HANTICK_5288) },

	/* Asus ROG Flow Z13 (2025) GZ302EA keyboard-cover touchpad */
	{ .driver_data = MT_CLS_ASUS_ROG_Z13_FOLIO,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ASUSTEK,
			USB_DEVICE_ID_ASUSTEK_ROG_Z13_FOLIO) },

	/* Generic MT device */
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH, HID_ANY_ID, HID_ANY_ID) },

	/* Generic Win 8 certified MT device */
	{  .driver_data = MT_CLS_WIN_8,
		HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH_WIN_8,
			HID_ANY_ID, HID_ANY_ID) },
	{ }
};

// ---------------------------------------------------------------------------
// Atomic flag bits
// ---------------------------------------------------------------------------
static inline bool mt_test_and_set_bit_lock(unsigned int nr,
					    unsigned long *flags)
{
	return (__atomic_fetch_or(flags, 1ul << nr, __ATOMIC_ACQUIRE) >> nr) &
	       1;
}

static inline void mt_clear_bit_unlock(unsigned int nr, unsigned long *flags)
{
	__atomic_fetch_and(flags, ~(1ul << nr), __ATOMIC_RELEASE);
}

/* The active-slot bitmap (maxcontacts bits; no bitmap: nothing to
 * track). */
static void mt_set_slot_bit(unsigned long *map, unsigned int slot)
{
	if (map)
		__atomic_fetch_or(&map[slot / INPUT_BITS_PER_LONG],
				  1ul << (slot % INPUT_BITS_PER_LONG),
				  __ATOMIC_RELAXED);
}

static void mt_clear_slot_bit(unsigned long *map, unsigned int slot)
{
	if (map && slot < MT_INPUT_MAX_SLOTS)
		__atomic_fetch_and(&map[slot / INPUT_BITS_PER_LONG],
				   ~(1ul << (slot % INPUT_BITS_PER_LONG)),
				   __ATOMIC_RELAXED);
}

static bool mt_any_slot_active(struct mt_device *td)
{
	if (!td->active_slots)
		return false;
	for (int i = 0; i < INPUT_BITS_TO_LONGS(td->maxcontacts); i++)
		if (__atomic_load_n(&td->active_slots[i], __ATOMIC_RELAXED))
			return true;
	return false;
}

// ---------------------------------------------------------------------------
// Report data, applications, contacts
// ---------------------------------------------------------------------------

static struct mt_application *mt_allocate_application(struct mt_device *td,
						      struct hid_report *report)
{
	unsigned int application = report->application;
	struct mt_application *app = kcalloc(1, sizeof(*app));

	if (!app)
		return NULL;
	app->application = application;
	INIT_LIST_HEAD(&app->mt_usages);

	if (application == HID_DG_TOUCHSCREEN)
		app->mt_flags |= INPUT_MT_DIRECT;
	/* A touchpad: a pointer device, and the device goes to touchpad
	 * mode. */
	if (application == HID_DG_TOUCHPAD) {
		app->mt_flags |= INPUT_MT_POINTER;
		td->inputmode_value = MT_INPUTMODE_TOUCHPAD;
	}

	app->scantime = DEFAULT_ZERO;
	app->raw_cc = DEFAULT_ZERO;
	app->quirks = td->mtclass.quirks;
	app->report_id = report->id;
	list_add_tail(&app->list, &td->applications);
	return app;
}

static struct mt_application *mt_find_application(struct mt_device *td,
						  struct hid_report *report)
{
	struct mt_application *tmp;

	list_for_each_entry(tmp, &td->applications, list)
		if (report->application == tmp->application &&
		    (!(td->mtclass.quirks & MT_QUIRK_SEPARATE_APP_REPORT) ||
		     tmp->report_id == report->id))
			return tmp;
	return mt_allocate_application(td, report);
}

static struct mt_report_data *mt_allocate_report_data(struct mt_device *td,
						      struct hid_report *report)
{
	struct mt_report_data *rdata = kcalloc(1, sizeof(*rdata));

	if (!rdata)
		return NULL;
	rdata->report = report;
	rdata->application = mt_find_application(td, report);
	if (!rdata->application) {
		kfree(rdata);
		return NULL;
	}

	/* A report with a Contact Identifier carries contacts -- on a
	 * certified device only when it sits in a Finger collection. */
	for (unsigned int r = 0; r < report->maxfield; r++) {
		struct hid_field *field = report->field[r];

		if (!(field->flags & HID_MAIN_ITEM_VARIABLE))
			continue;
		if (field->logical == HID_DG_FINGER ||
		    td->hdev->group != HID_GROUP_MULTITOUCH_WIN_8) {
			for (unsigned int n = 0; n < field->report_count; n++) {
				unsigned int hid = field->usage[n].hid;

				/* (A touch bar numbers contacts with the
				 * transducer index.) */
				if (hid == HID_DG_CONTACTID ||
				    ((td->mtclass.quirks &
				      MT_QUIRK_APPLE_TOUCHBAR) &&
				     hid == HID_DG_TRANSDUCER_INDEX)) {
					rdata->is_mt_collection = true;
					break;
				}
			}
		}
	}
	list_add_tail(&rdata->list, &td->reports);
	return rdata;
}

/* Report path: never allocates. */
static struct mt_report_data *mt_lookup_report_data(struct mt_device *td,
						    struct hid_report *report)
{
	struct mt_report_data *tmp;

	list_for_each_entry(tmp, &td->reports, list)
		if (tmp->report == report)
			return tmp;
	return NULL;
}

/* Setup path: creates the data at first sight. */
static struct mt_report_data *mt_find_report_data(struct mt_device *td,
						  struct hid_report *report)
{
	struct mt_report_data *rdata = mt_lookup_report_data(td, report);

	return rdata ? rdata : mt_allocate_report_data(td, report);
}

static struct mt_usages *mt_allocate_usage(struct mt_application *app)
{
	struct mt_usages *usage = kcalloc(1, sizeof(*usage));

	if (!usage)
		return NULL;
	usage->x = DEFAULT_ZERO;
	usage->y = DEFAULT_ZERO;
	usage->cx = DEFAULT_ZERO;
	usage->cy = DEFAULT_ZERO;
	usage->p = DEFAULT_ZERO;
	usage->w = DEFAULT_ZERO;
	usage->h = DEFAULT_ZERO;
	usage->a = DEFAULT_ZERO;
	usage->contactid = DEFAULT_ZERO;
	usage->tip_state = DEFAULT_FALSE;
	usage->inrange_state = DEFAULT_FALSE;
	usage->confidence_state = DEFAULT_TRUE;
	list_add_tail(&usage->list, &app->mt_usages);
	return usage;
}

static bool mt_is_default(const int32_t *p)
{
	return p == DEFAULT_TRUE || p == DEFAULT_FALSE || p == DEFAULT_ZERO;
}

/* Point member `offset' of the current contact at `value'.  A member that
 * is already set starts the next contact -- unless the current one is
 * still incomplete (no id or position yet), which is a duplicate usage and
 * ignored.  Out of memory, the usage just stays unknown. */
static void mt_store_field(struct mt_application *app, const int32_t *value,
			  size_t offset)
{
	struct mt_usages *usage;
	const int32_t **target;

	if (list_empty(&app->mt_usages))
		usage = mt_allocate_usage(app);
	else
		usage = list_last_entry(&app->mt_usages, struct mt_usages,
					list);
	if (!usage)
		return;

	target = (const int32_t **)((char *)usage + offset);
	if (!mt_is_default(*target)) {
		if (usage->contactid == DEFAULT_ZERO ||
		    usage->x == DEFAULT_ZERO || usage->y == DEFAULT_ZERO)
			return;
		usage = mt_allocate_usage(app);
		if (!usage)
			return;
		target = (const int32_t **)((char *)usage + offset);
	}
	*target = value;
}

#define MT_STORE_FIELD(member)                                    \
	mt_store_field(app, &field->value[usage->usage_index],    \
		       __builtin_offsetof(struct mt_usages, member))

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------

/* Read a feature report into its fields (setup only: the probe holds the
 * device's input lock, which hid_report_raw_event wants). */
static void mt_get_feature(struct hid_device *hdev, struct hid_report *report)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	uint32_t size;
	uint8_t *buf;
	int ret;

	if (hdev->quirks & HID_QUIRK_NO_INIT_REPORTS)
		return;

	buf = hid_alloc_report_buf(report);
	if (!buf)
		return;
	/* The buffer starts with the report ID byte, also when the report
	 * is unnumbered. */
	size = hid_report_len(report) + (report->id == 0);
	ret = hid_hw_raw_request(hdev, (unsigned char)report->id, buf, size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_warn(hdev, "failed to fetch feature %u\n", report->id);
	} else if (report->id != buf[0] &&
		   !(td->mtclass.quirks & MT_QUIRK_IGNORE_FEATURE_ID_MISMATCH)) {
		/* An answer for another report is no answer. */
		hid_err(hdev, "returned feature report did not match the "
			      "request\n");
	} else {
		/* The whole report length: what the device did not send
		 * reads as zero (the buffer is zeroed). */
		if (hdev->report_enum[HID_FEATURE_REPORT].numbered)
			ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT,
						   buf, size, 0);
		else
			ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT,
						   buf + 1, size - 1, 0);
		if (ret)
			hid_warn(hdev, "failed to report feature\n");
	}
	kfree(buf);
}

static void mt_feature_mapping(struct hid_device *hdev,
			       struct hid_field *field, struct hid_usage *usage)
{
	struct mt_device *td = hid_get_drvdata(hdev);

	switch (usage->hid) {
	case HID_DG_CONTACTMAX:
		mt_get_feature(hdev, field->report);
		td->maxcontacts = (uint8_t)field->value[0];
		if (!td->maxcontacts &&
		    field->logical_maximum <= MT_MAX_MAXCONTACT)
			td->maxcontacts = (uint8_t)field->logical_maximum;
		if (td->mtclass.maxcontacts)
			/* the class knows better */
			td->maxcontacts = td->mtclass.maxcontacts;
		break;

	case HID_DG_BUTTONTYPE:
		if (usage->usage_index >= field->report_count) {
			hid_err(hdev, "HID_DG_BUTTONTYPE out of range\n");
			break;
		}
		mt_get_feature(hdev, field->report);
		switch (field->value[usage->usage_index]) {
		case MT_BUTTONTYPE_CLICKPAD:
			td->is_buttonpad = true;
			break;
		case MT_BUTTONTYPE_PRESSUREPAD:
			td->is_pressurepad = true;
			break;
		}
		break;

	case HID_VD_WIN8_CERT_BLOB:
		/* Some devices only work after their certification blob
		 * was read once. */
		if (usage->usage_index == 0)
			mt_get_feature(hdev, field->report);
		break;
	}
}

/* Set the value a mode feature should have; true when the report must be
 * sent. */
static bool mt_need_to_apply_feature(struct hid_device *hdev,
				     struct hid_field *field,
				     struct hid_usage *usage,
				     enum latency_mode latency,
				     unsigned int report_mode,
				     bool *inputmode_found)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_class *cls = &td->mtclass;
	struct hid_report *report = field->report;
	unsigned int index = usage->usage_index;
	uint8_t *buf;
	int max;

	switch (usage->hid) {
	case HID_DG_INPUTMODE:
		/* Some panels declare two Input Mode features and ignore
		 * the second: only the first is set. */
		if (*inputmode_found)
			return false;
		if (cls->quirks & MT_QUIRK_FORCE_GET_FEATURE) {
			uint32_t len = hid_report_len(report) +
				       (report->id == 0);

			buf = hid_alloc_report_buf(report);
			if (!buf) {
				hid_err(hdev, "failed to allocate buffer for "
					      "report\n");
				return false;
			}
			hid_hw_raw_request(hdev, (unsigned char)report->id, buf,
					   len, HID_FEATURE_REPORT,
					   HID_REQ_GET_REPORT);
			kfree(buf);
		}
		field->value[index] = td->inputmode_value;
		*inputmode_found = true;
		return true;

	case HID_DG_CONTACTMAX:
		if (cls->maxcontacts) {
			max = field->logical_maximum < cls->maxcontacts ?
				      field->logical_maximum :
				      cls->maxcontacts;
			if (field->value[index] != max) {
				field->value[index] = max;
				return true;
			}
		}
		break;

	case HID_DG_LATENCYMODE:
		field->value[index] = latency;
		return true;

	case HID_DG_SURFACESWITCH:
		field->value[index] = !!(report_mode & TOUCHPAD_REPORT_CONTACTS);
		return true;

	case HID_DG_BUTTONSWITCH:
		field->value[index] = !!(report_mode & TOUCHPAD_REPORT_BUTTONS);
		return true;
	}
	return false;
}

static void mt_set_modes(struct hid_device *hdev, enum latency_mode latency,
			 unsigned int report_mode)
{
	struct hid_report *rep;
	bool inputmode_found = false;

	list_for_each_entry(rep,
			    &hdev->report_enum[HID_FEATURE_REPORT].report_list,
			    list) {
		bool update_report = false;

		for (unsigned int i = 0; i < rep->maxfield; i++) {
			struct hid_field *field = rep->field[i];

			if (field->report_count < 1)
				continue;
			for (unsigned int j = 0; j < field->maxusage; j++)
				if (mt_need_to_apply_feature(
					    hdev, field, &field->usage[j],
					    latency, report_mode,
					    &inputmode_found))
					update_report = true;
		}
		if (update_report)
			hid_hw_request(hdev, rep, HID_REQ_SET_REPORT);
	}
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------

static void set_abs(struct input_dev *input, unsigned int code,
		    struct hid_field *field, int snratio)
{
	int fmin = field->logical_minimum;
	int fmax = field->logical_maximum;
	int fuzz = snratio ? (int)(((int64_t)fmax - fmin) / snratio) : 0;

	input_set_abs_params(input, code, fmin, fmax, fuzz, 0);
	input_abs_set_res(input, code, hidinput_calc_abs_res(field,
							     (uint16_t)code));
}

static int mt_touch_input_mapping(struct hid_device *hdev,
				  struct hid_input *hi, struct hid_field *field,
				  struct hid_usage *usage, unsigned long **bit,
				  int *max, struct mt_application *app)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_class *cls = &td->mtclass;
	struct hid_usage *prev_usage = NULL;
	unsigned int code;

	/* A touchscreen with buttons is modelled as a touchpad. */
	if (field->application == HID_DG_TOUCHSCREEN &&
	    (usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON) {
		app->mt_flags |= INPUT_MT_POINTER;
		td->inputmode_value = MT_INPUTMODE_TOUCHPAD;
	}

	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON)
		app->buttons_count++;

	if (usage->usage_index)
		prev_usage = &field->usage[usage->usage_index - 1];

	switch (usage->hid & HID_USAGE_PAGE) {
	case HID_UP_GENDESK:
		switch (usage->hid) {
		case HID_GD_X:
			/* X twice in a row: the second is the tool's. */
			if (prev_usage && prev_usage->hid == usage->hid) {
				code = ABS_MT_TOOL_X;
				MT_STORE_FIELD(cx);
			} else {
				code = ABS_MT_POSITION_X;
				MT_STORE_FIELD(x);
			}
			set_abs(hi->input, code, field, cls->sn_move);
			/* A multi-axis device exporting X and Y works on a
			 * surface. */
			if (field->application == HID_GD_SYSTEM_MULTIAXIS) {
				input_set_bit(INPUT_PROP_DIRECT,
					      hi->input->propbit);
				input_set_abs_params(hi->input, ABS_MT_TOOL_TYPE,
						     MT_TOOL_DIAL, MT_TOOL_DIAL,
						     0, 0);
			}
			return 1;
		case HID_GD_Y:
			if (prev_usage && prev_usage->hid == usage->hid) {
				code = ABS_MT_TOOL_Y;
				MT_STORE_FIELD(cy);
			} else {
				code = ABS_MT_POSITION_Y;
				MT_STORE_FIELD(y);
			}
			set_abs(hi->input, code, field, cls->sn_move);
			return 1;
		}
		return 0;

	case HID_UP_DIGITIZER:
		switch (usage->hid) {
		case HID_DG_INRANGE:
			if (app->quirks & MT_QUIRK_HOVERING)
				input_set_abs_params(hi->input, ABS_MT_DISTANCE,
						     0, 1, 0, 0);
			MT_STORE_FIELD(inrange_state);
			return 1;
		case HID_DG_CONFIDENCE:
			/* Certified touch devices mark palms by clearing
			 * Confidence: report those as MT_TOOL_PALM. */
			if ((cls->name == MT_CLS_WIN_8 ||
			     cls->name == MT_CLS_WIN_8_FORCE_MULTI_INPUT ||
			     cls->name == MT_CLS_WIN_8_FORCE_MULTI_INPUT_NSMU ||
			     cls->name == MT_CLS_WIN_8_DISABLE_WAKEUP ||
			     cls->name == MT_CLS_WIN_8_KEEP_LATENCY_ON_CLOSE) &&
			    (field->application == HID_DG_TOUCHPAD ||
			     field->application == HID_DG_TOUCHSCREEN))
				app->quirks |= MT_QUIRK_CONFIDENCE;
			if (app->quirks & MT_QUIRK_CONFIDENCE)
				input_set_abs_params(hi->input, ABS_MT_TOOL_TYPE,
						     MT_TOOL_FINGER,
						     MT_TOOL_PALM, 0, 0);
			MT_STORE_FIELD(confidence_state);
			return 1;
		case HID_DG_TOUCH:
			/* Legacy usage, ignored -- except on touch bars,
			 * where it is the tip state. */
			if (!(cls->quirks & MT_QUIRK_APPLE_TOUCHBAR))
				return -1;
			__attribute__((fallthrough));
		case HID_DG_TIPSWITCH:
			if (field->application != HID_GD_SYSTEM_MULTIAXIS)
				input_set_capability(hi->input, EV_KEY,
						     BTN_TOUCH);
			MT_STORE_FIELD(tip_state);
			return 1;
		case HID_DG_TRANSDUCER_INDEX:
			if (!(cls->quirks & MT_QUIRK_APPLE_TOUCHBAR))
				return 0;
			/* the contact number is there */
			__attribute__((fallthrough));
		case HID_DG_CONTACTID:
			MT_STORE_FIELD(contactid);
			app->touches_by_report++;
			return 1;
		case HID_DG_WIDTH:
			if (!(app->quirks & MT_QUIRK_NO_AREA))
				set_abs(hi->input, ABS_MT_TOUCH_MAJOR, field,
					cls->sn_width);
			MT_STORE_FIELD(w);
			return 1;
		case HID_DG_HEIGHT:
			if (!(app->quirks & MT_QUIRK_NO_AREA)) {
				set_abs(hi->input, ABS_MT_TOUCH_MINOR, field,
					cls->sn_height);
				/* Unless Azimuth already declared it. */
				if (!input_test_bit(ABS_MT_ORIENTATION,
						    hi->input->absbit))
					input_set_abs_params(hi->input,
							     ABS_MT_ORIENTATION,
							     0, 1, 0, 0);
			}
			MT_STORE_FIELD(h);
			return 1;
		case HID_DG_TIPPRESSURE:
			set_abs(hi->input, ABS_MT_PRESSURE, field,
				cls->sn_pressure);
			MT_STORE_FIELD(p);
			return 1;
		case HID_DG_SCANTIME:
			input_set_capability(hi->input, EV_MSC, MSC_TIMESTAMP);
			app->scantime = &field->value[usage->usage_index];
			app->scantime_logical_max = field->logical_maximum;
			return 1;
		case HID_DG_CONTACTCOUNT:
			app->have_contact_count = true;
			app->raw_cc = &field->value[usage->usage_index];
			return 1;
		case HID_DG_AZIMUTH: {
			/* A full turn is [0, max): orientation covers a
			 * quarter turn each way. */
			int quarter = field->logical_maximum / 4;

			input_set_abs_params(hi->input, ABS_MT_ORIENTATION,
					     -quarter, quarter,
					     cls->sn_move ?
						     field->logical_maximum /
							     cls->sn_move :
						     0,
					     0);
			MT_STORE_FIELD(a);
			return 1;
		}
		case HID_DG_CONTACTMAX:
			/* a feature of the whole report */
			return -1;
		}
		/* the rest is hid-input's */
		return 0;

	case HID_UP_BUTTON:
		code = BTN_MOUSE + ((usage->hid - 1) & HID_USAGE);
		/* A certified touchpad's external left/right buttons are
		 * usages 2 and 3; usage 1 is the click of the pad. */
		if ((app->quirks & MT_QUIRK_WIN8_PTP_BUTTONS) &&
		    field->application == HID_DG_TOUCHPAD &&
		    (usage->hid & HID_USAGE) > 1)
			code--;
		if (field->application == HID_GD_SYSTEM_MULTIAXIS)
			code = BTN_0 + ((usage->hid - 1) & HID_USAGE);
		hid_map_usage(hi, usage, bit, max, EV_KEY, code);
		if (!*bit)
			return -1;
		input_set_capability(hi->input, EV_KEY, code);
		return 1;

	case 0xff000000:
		/* vendor data: nothing for the input layer */
		return -1;
	}
	return 0;
}

static int mt_input_mapping(struct hid_device *hdev, struct hid_input *hi,
			    struct hid_field *field, struct hid_usage *usage,
			    unsigned long **bit, int *max)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_report_data *rdata;
	struct mt_application *application;

	rdata = mt_find_report_data(td, field->report);
	if (!rdata) {
		hid_err(hdev, "failed to allocate data for report\n");
		return 0;
	}
	application = rdata->application;

	/* Without export_all_inputs only touch, keyboard-like and system
	 * applications are mapped: a Mouse collection with the same
	 * Generic Desktop usages would duplicate the touch device. */
	if (!td->mtclass.export_all_inputs &&
	    field->application != HID_DG_TOUCHSCREEN &&
	    field->application != HID_DG_PEN &&
	    field->application != HID_DG_TOUCHPAD &&
	    field->application != HID_GD_KEYBOARD &&
	    field->application != HID_GD_SYSTEM_CONTROL &&
	    field->application != HID_CP_CONSUMER_CONTROL &&
	    field->application != HID_GD_WIRELESS_RADIO_CTLS &&
	    field->application != HID_GD_SYSTEM_MULTIAXIS &&
	    !(field->application == HID_VD_ASUS_CUSTOM_MEDIA_KEYS &&
	      (application->quirks & MT_QUIRK_ASUS_CUSTOM_UP)))
		return -1;

	/* ASUS keyboards' hot keys in the touchpad descriptor: an array. */
	if (field->application == HID_VD_ASUS_CUSTOM_MEDIA_KEYS &&
	    (application->quirks & MT_QUIRK_ASUS_CUSTOM_UP) &&
	    (usage->hid & HID_USAGE_PAGE) == HID_UP_CUSTOM) {
		unsigned int key;

		input_set_bit(EV_REP, hi->input->evbit);
		field->flags &= ~HID_MAIN_ITEM_VARIABLE;
		switch (usage->hid & HID_USAGE) {
		case 0x10:
			key = KEY_BRIGHTNESSDOWN;
			break;
		case 0x20:
			key = KEY_BRIGHTNESSUP;
			break;
		case 0x35:
			key = KEY_DISPLAY_OFF;
			break;
		case 0x6b:
			key = KEY_F21;
			break;
		case 0x6c:
			key = KEY_SLEEP;
			break;
		default:
			return -1;
		}
		hid_map_usage_clear(hi, usage, bit, max, EV_KEY, key);
		return 1;
	}

	if (rdata->is_mt_collection)
		return mt_touch_input_mapping(hdev, hi, field, usage, bit, max,
					      application);

	/* Some panels put their stylus in the touchscreen application. */
	if (field->physical == HID_DG_STYLUS)
		hi->application = HID_DG_STYLUS;

	return 0;
}

static int mt_input_mapped(struct hid_device *hdev, struct hid_input *hi,
			   struct hid_field *field, struct hid_usage *usage,
			   unsigned long **bit, int *max)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_report_data *rdata;

	(void)hi;
	(void)usage;
	(void)bit;
	(void)max;
	rdata = mt_find_report_data(td, field->report);
	if (rdata && rdata->is_mt_collection)
		return -1; // ours: hid-input must not touch it
	return 0;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static void mt_post_parse_default_settings(struct mt_device *td,
					   struct mt_application *app)
{
	uint32_t quirks = app->quirks;

	(void)td;
	/* One contact per report, of an unknown device: serial. */
	if (list_is_singular(&app->mt_usages)) {
		quirks |= MT_QUIRK_ALWAYS_VALID;
		quirks &= ~MT_QUIRK_NOT_SEEN_MEANS_UP;
		quirks &= ~MT_QUIRK_VALID_IS_INRANGE;
		quirks &= ~MT_QUIRK_VALID_IS_CONFIDENCE;
		quirks &= ~MT_QUIRK_CONTACT_CNT_ACCURATE;
	}
	app->quirks = quirks;
}

static void mt_post_parse(struct mt_device *td, struct mt_application *app)
{
	(void)td;
	if (!app->have_contact_count)
		app->quirks &= ~MT_QUIRK_CONTACT_CNT_ACCURATE;
}

static int mt_touch_input_configured(struct hid_device *hdev,
				     struct hid_input *hi,
				     struct mt_application *app)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_class *cls = &td->mtclass;
	struct input_dev *input = hi->input;
	int ret;

	/* Touch bars have no Contact Count Maximum but more contacts than
	 * the default. */
	if ((cls->quirks & MT_QUIRK_APPLE_TOUCHBAR) && cls->maxcontacts)
		td->maxcontacts = cls->maxcontacts;
	/* Old devices have no Contact Count Maximum feature. */
	if (!td->maxcontacts)
		td->maxcontacts = MT_DEFAULT_MAXCONTACT;
	if (td->maxcontacts > MT_INPUT_MAX_SLOTS) {
		hid_warn(hdev, "%u contacts, tracking the first %u\n",
			 td->maxcontacts, MT_INPUT_MAX_SLOTS);
		td->maxcontacts = MT_INPUT_MAX_SLOTS;
	}

	mt_post_parse(td, app);
	if (td->serial_maybe)
		mt_post_parse_default_settings(td, app);

	/* A touch bar's application is a touch pad, but it is direct. */
	if (cls->quirks & MT_QUIRK_APPLE_TOUCHBAR)
		app->mt_flags |= INPUT_MT_DIRECT;
	if (cls->is_indirect)
		app->mt_flags |= INPUT_MT_POINTER;
	if (app->quirks & MT_QUIRK_NOT_SEEN_MEANS_UP)
		app->mt_flags |= INPUT_MT_DROP_UNUSED;

	/* A touchpad with exactly one button: the click of the pad. */
	if ((app->mt_flags & INPUT_MT_POINTER) && app->buttons_count == 1)
		td->is_buttonpad = true;
	if (td->is_buttonpad)
		input_set_bit(INPUT_PROP_BUTTONPAD, input->propbit);
	if (td->is_pressurepad)
		input_set_bit(INPUT_PROP_PRESSUREPAD, input->propbit);

	if (!td->active_slots) {
		td->active_slots = kcalloc(INPUT_BITS_TO_LONGS(td->maxcontacts),
					   sizeof(unsigned long));
		if (!td->active_slots)
			return -ENOMEM;
	}
	if (!app->pending_palm_slots) {
		app->palm_bits = td->maxcontacts;
		app->pending_palm_slots =
			kcalloc(INPUT_BITS_TO_LONGS(td->maxcontacts),
				sizeof(unsigned long));
		if (!app->pending_palm_slots)
			return -ENOMEM;
	}

	ret = input_mt_init_slots(input, td->maxcontacts, app->mt_flags);
	if (ret)
		return ret;
	app->mt_flags = 0;
	return 0;
}

static int mt_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_report_data *rdata;
	struct hid_report *report;
	const char *suffix = NULL;
	int ret;

	list_for_each_entry(report, &hi->reports, hidinput_list) {
		rdata = mt_find_report_data(td, report);
		if (!rdata) {
			hid_err(hdev, "failed to allocate data for report\n");
			return -ENOMEM;
		}
		if (rdata->is_mt_collection) {
			ret = mt_touch_input_configured(hdev, hi,
							rdata->application);
			if (ret)
				return ret;
		}
	}

	switch (hi->application) {
	case HID_GD_KEYBOARD:
	case HID_GD_KEYPAD:
	case HID_GD_MOUSE:
	case HID_DG_TOUCHPAD:
	case HID_GD_SYSTEM_CONTROL:
	case HID_CP_CONSUMER_CONTROL:
	case HID_GD_WIRELESS_RADIO_CTLS:
	case HID_GD_SYSTEM_MULTIAXIS:
	case HID_DG_PEN:
		/* hid-input named it already */
		break;
	case HID_DG_TOUCHSCREEN:
		/* a touchscreen carries the device name alone */
		kfree(hi->name);
		hi->name = NULL;
		hi->input->name = hdev->name;
		break;
	case HID_VD_ASUS_CUSTOM_MEDIA_KEYS:
		suffix = "Custom Media Keys";
		break;
	case HID_DG_STYLUS:
		/* BTN_STYLUS lets user space recognise the tablet */
		input_set_bit(BTN_STYLUS, hi->input->keybit);
		break;
	default:
		suffix = "UNKNOWN";
		break;
	}

	/* The Yoga Book 9i's two screens and pens are told apart by
	 * name. */
	if (hi->report && (td->mtclass.quirks & MT_QUIRK_YOGABOOK9I)) {
		switch (hi->report->id) {
		case 48:
			suffix = "Touchscreen Top";
			break;
		case 56:
			suffix = "Touchscreen Bottom";
			break;
		case 20:
			suffix = "Stylus Top";
			break;
		case 40:
			suffix = "Stylus Bottom";
			break;
		case 80:
			suffix = "Emulated Touchpad";
			break;
		default:
			suffix = "";
			break;
		}
	}
	if (suffix)
		return hidinput_set_name(hi, hdev->name, suffix);
	return 0;
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

/* Microseconds of device time, from the scan time (in 100 us units,
 * wrapping at its logical maximum), resynchronised after a pause. */
static int mt_compute_timestamp(struct mt_application *app, int32_t value)
{
	int64_t delta = (int64_t)value - app->prev_scantime;
	uint64_t now = timer_get_precise_ns();
	uint64_t jdelta = (now - app->last_report_ns) / 1000;

	app->last_report_ns = now;
	if (delta < 0)
		delta += app->scantime_logical_max;
	delta *= 100;
	if (jdelta > MAX_TIMESTAMP_INTERVAL)
		return 0; // no data for a while: start over
	return (int)(uint32_t)((uint32_t)app->timestamp + (uint32_t)delta);
}

static int mt_compute_slot(struct mt_device *td, struct mt_application *app,
			   struct mt_usages *slot, struct input_dev *input)
{
	uint32_t quirks = app->quirks;

	(void)td;
	if (quirks & MT_QUIRK_SLOT_IS_CONTACTID)
		return *slot->contactid;
	if (quirks & MT_QUIRK_CYPRESS) {
		if (*slot->contactid != 0 || app->num_received == 0)
			return *slot->contactid;
		return -1;
	}
	if (quirks & MT_QUIRK_SLOT_IS_CONTACTNUMBER)
		return app->num_received;
	if (quirks & MT_QUIRK_SLOT_IS_CONTACTID_MINUS_ONE)
		return (int)((int64_t)*slot->contactid - 1);
	return input_mt_get_slot_by_key(input, *slot->contactid);
}

static int32_t mt_clamp32(int64_t v)
{
	if (v > 2147483647LL)
		return 2147483647;
	if (v < -2147483648LL)
		return (int32_t)(-2147483647 - 1);
	return (int32_t)v;
}

/* One contact; 0 when it was taken (or deliberately skipped), negative
 * when it does not count towards the frame. */
static int mt_process_slot(struct mt_device *td, struct input_dev *input,
			   struct mt_application *app, struct mt_usages *slot)
{
	struct input_mt *mt = input->mt;
	struct hid_device *hdev = td->hdev;
	uint32_t quirks = app->quirks;
	bool valid = true;
	bool confidence_state = true;
	bool inrange_state = false;
	int active, slotnum;
	unsigned int tool = MT_TOOL_FINGER;

	if (!slot)
		return -EINVAL;

	/* The frame is complete: more contacts are leftovers. */
	if ((quirks & MT_QUIRK_CONTACT_CNT_ACCURATE) &&
	    app->num_received >= app->num_expected)
		return -EAGAIN;

	if (!(quirks & MT_QUIRK_ALWAYS_VALID)) {
		if (quirks & MT_QUIRK_VALID_IS_INRANGE)
			valid = *slot->inrange_state != 0;
		if (quirks & MT_QUIRK_NOT_SEEN_MEANS_UP)
			valid = *slot->tip_state != 0;
		if (quirks & MT_QUIRK_VALID_IS_CONFIDENCE)
			valid = *slot->confidence_state != 0;
		if (!valid)
			return 0;
	}

	slotnum = mt_compute_slot(td, app, slot, input);
	if (slotnum < 0 || slotnum >= td->maxcontacts)
		return 0;
	if (!mt || slotnum >= mt->num_slots)
		return 0;

	/* The same contact twice in one frame: the first one counts. */
	if (quirks & MT_QUIRK_IGNORE_DUPLICATES) {
		struct input_mt_slot *i_slot = &mt->slots[slotnum];

		if (input_mt_is_active(i_slot) && input_mt_is_used(mt, i_slot))
			return -EAGAIN;
	}

	if (quirks & MT_QUIRK_CONFIDENCE)
		confidence_state = *slot->confidence_state != 0;
	if (quirks & MT_QUIRK_HOVERING)
		inrange_state = *slot->inrange_state != 0;

	active = *slot->tip_state || inrange_state;

	if (app->application == HID_GD_SYSTEM_MULTIAXIS) {
		tool = MT_TOOL_DIAL;
	} else if (!confidence_state) {
		tool = MT_TOOL_PALM;
		if (!active && input_mt_is_active(&mt->slots[slotnum])) {
			/* A contact that turns into a palm as it lifts:
			 * readers must see the palm before the lift, or they
			 * take the lift for a tap.  Report it active as a
			 * palm now, release it after the frame. */
			active = true;
			if ((unsigned int)slotnum < app->palm_bits)
				app->pending_palm_slots[slotnum /
							INPUT_BITS_PER_LONG] |=
					1ul << (slotnum % INPUT_BITS_PER_LONG);
		}
	}

	input_mt_slot(input, slotnum);
	input_mt_report_slot_state(input, tool, active);
	if (active) {
		int64_t w = *slot->w, h = *slot->h;
		int wide = w > h;
		int major = (int)(w > h ? w : h);
		int minor = (int)(w > h ? h : w);
		int orientation = wide;
		int x, y, cx, cy;

		if (slot->a != DEFAULT_ZERO) {
			/* Azimuth: counter-clockwise over [0, max), made
			 * clockwise over [-max/2, max/2] (beyond the
			 * declared quarter turn for an upside-down
			 * ellipse). */
			int64_t azimuth = *slot->a;
			int64_t max_azimuth =
				input_abs_get_max(input, ABS_MT_ORIENTATION);

			if (azimuth > max_azimuth * 2)
				azimuth -= max_azimuth * 4;
			orientation = mt_clamp32(-azimuth);
			if (quirks & MT_QUIRK_ORIENTATION_INVERT)
				orientation = mt_clamp32(-(int64_t)orientation);
		}

		if (quirks & MT_QUIRK_TOUCH_SIZE_SCALING) {
			/* half size matches what these panels mean */
			major >>= 1;
			minor >>= 1;
		}

		x = hdev->quirks & HID_QUIRK_X_INVERT ?
			    mt_clamp32((int64_t)input_abs_get_max(
					       input, ABS_MT_POSITION_X) -
				       *slot->x) :
			    *slot->x;
		y = hdev->quirks & HID_QUIRK_Y_INVERT ?
			    mt_clamp32((int64_t)input_abs_get_max(
					       input, ABS_MT_POSITION_Y) -
				       *slot->y) :
			    *slot->y;
		cx = hdev->quirks & HID_QUIRK_X_INVERT ?
			     mt_clamp32((int64_t)input_abs_get_max(
						input, ABS_MT_POSITION_X) -
					*slot->cx) :
			     *slot->cx;
		cy = hdev->quirks & HID_QUIRK_Y_INVERT ?
			     mt_clamp32((int64_t)input_abs_get_max(
						input, ABS_MT_POSITION_Y) -
					*slot->cy) :
			     *slot->cy;

		input_event(input, EV_ABS, ABS_MT_POSITION_X, x);
		input_event(input, EV_ABS, ABS_MT_POSITION_Y, y);
		input_event(input, EV_ABS, ABS_MT_TOOL_X, cx);
		input_event(input, EV_ABS, ABS_MT_TOOL_Y, cy);
		input_event(input, EV_ABS, ABS_MT_DISTANCE, !*slot->tip_state);
		input_event(input, EV_ABS, ABS_MT_ORIENTATION, orientation);
		input_event(input, EV_ABS, ABS_MT_PRESSURE, *slot->p);
		input_event(input, EV_ABS, ABS_MT_TOUCH_MAJOR, major);
		input_event(input, EV_ABS, ABS_MT_TOUCH_MINOR, minor);

		mt_set_slot_bit(td->active_slots, (unsigned int)slotnum);
	} else {
		mt_clear_slot_bit(td->active_slots, (unsigned int)slotnum);
	}
	return 0;
}

/* Frame-wide values (buttons) of a contact report. */
static void mt_process_mt_event(struct hid_device *hid,
				struct mt_application *app,
				struct hid_field *field,
				struct hid_usage *usage, int32_t value,
				bool first_packet)
{
	uint32_t quirks = app->quirks;
	struct input_dev *input;

	if (!usage->type || !(hid->claimed & HID_CLAIMED_INPUT))
		return;
	if (!field->hidinput)
		return;
	input = field->hidinput->input;

	if (quirks & MT_QUIRK_WIN8_PTP_BUTTONS) {
		/* In a frame spread over several reports only the first
		 * one's buttons count. */
		if (!first_packet)
			return;
		/* The pad's click and an external left button are both
		 * BTN_LEFT: down while either is. */
		if (usage->type == EV_KEY && usage->code == BTN_LEFT) {
			app->left_button_state |= value;
			return;
		}
	}
	input_event(input, usage->type, usage->code, value);
}

static void mt_release_pending_palms(struct mt_device *td,
				     struct mt_application *app,
				     struct input_dev *input)
{
	bool need_sync = false;

	for (unsigned int slotnum = 0; slotnum < app->palm_bits; slotnum++) {
		unsigned long *word =
			&app->pending_palm_slots[slotnum / INPUT_BITS_PER_LONG];
		unsigned long bit = 1ul << (slotnum % INPUT_BITS_PER_LONG);

		if (!(*word & bit))
			continue;
		*word &= ~bit;
		mt_clear_slot_bit(td->active_slots, slotnum);
		input_mt_slot(input, (int)slotnum);
		input_mt_report_slot_inactive(input);
		need_sync = true;
	}
	if (need_sync) {
		input_mt_sync_frame(input);
		input_sync(input);
	}
}

static void mt_sync_frame(struct mt_device *td, struct mt_application *app,
			  struct input_dev *input)
{
	if (app->quirks & MT_QUIRK_WIN8_PTP_BUTTONS)
		input_event(input, EV_KEY, BTN_LEFT, app->left_button_state);

	input_mt_sync_frame(input);
	input_event(input, EV_MSC, MSC_TIMESTAMP, app->timestamp);
	input_sync(input);

	mt_release_pending_palms(td, app, input);

	app->num_received = 0;
	app->left_button_state = 0;
}

static void mt_touch_report(struct hid_device *hid,
			    struct mt_report_data *rdata)
{
	struct mt_device *td = hid_get_drvdata(hid);
	struct hid_report *report = rdata->report;
	struct mt_application *app = rdata->application;
	struct input_dev *input;
	struct mt_usages *slot;
	bool first_packet;
	int scantime, contact_count = -1;

	/* The sticky finger release is running: it handles this state. */
	if (mt_test_and_set_bit_lock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags))
		return;

	if (!report->maxfield || !report->field[0]->hidinput)
		goto out;
	input = report->field[0]->hidinput->input;

	scantime = *app->scantime;
	app->timestamp = mt_compute_timestamp(app, scantime);
	if (app->raw_cc != DEFAULT_ZERO)
		contact_count = *app->raw_cc;

	/* A frame may be spread over several reports: the first one
	 * carries the contact count, the others zero.  A count of zero at
	 * the start of a frame (the scan time moved) is a frame of its own:
	 * a certified touchpad's button-only report, or a device's "all
	 * contacts up". */
	if (contact_count > 0)
		app->num_expected = (uint8_t)contact_count;
	else if (app->num_received == 0 && app->prev_scantime != scantime)
		app->num_expected = 0;
	app->prev_scantime = scantime;

	first_packet = app->num_received == 0;

	list_for_each_entry(slot, &app->mt_usages, list)
		if (!mt_process_slot(td, input, app, slot))
			app->num_received++;

	for (unsigned int r = 0; r < report->maxfield; r++) {
		struct hid_field *field = report->field[r];

		if (!(field->flags & HID_MAIN_ITEM_VARIABLE))
			continue;
		for (unsigned int n = 0; n < field->report_count; n++)
			mt_process_mt_event(hid, app, field, &field->usage[n],
					    field->value[n], first_packet);
	}

	if (app->num_received >= app->num_expected)
		mt_sync_frame(td, app, input);

	/* Certified devices report at least every few milliseconds while a
	 * contact is down; 100 ms of silence with contacts down means the
	 * lift got lost. */
	if (app->quirks & MT_QUIRK_STICKY_FINGERS) {
		if (mt_any_slot_active(td))
			mod_delayed_work(system_wq, &td->release_work,
					 msecs_to_jiffies(MT_STICKY_RELEASE_MS));
		else
			cancel_delayed_work(&td->release_work);
	}
out:
	mt_clear_bit_unlock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags);
}

/* Release every contact of every input device. */
static void mt_release_contacts(struct hid_device *hid)
{
	struct mt_device *td = hid_get_drvdata(hid);
	struct mt_application *application;
	struct hid_input *hidinput;

	list_for_each_entry(hidinput, &hid->inputs, list) {
		struct input_dev *input_dev = hidinput->input;
		struct input_mt *mt = input_dev->mt;

		if (!mt)
			continue;
		for (int i = 0; i < mt->num_slots; i++) {
			input_mt_slot(input_dev, i);
			input_mt_report_slot_inactive(input_dev);
			mt_clear_slot_bit(td->active_slots, (unsigned int)i);
		}
		input_mt_sync_frame(input_dev);
		input_sync(input_dev);
	}
	list_for_each_entry(application, &td->applications, list)
		application->num_received = 0;
}

static void mt_expired_timeout(struct work_struct *work)
{
	struct mt_device *td =
		container_of(to_delayed_work(work), struct mt_device,
			     release_work);
	struct hid_device *hdev = td->hdev;

	/* A report that just came in takes care of the contacts. */
	if (mt_test_and_set_bit_lock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags))
		return;
	if (mt_any_slot_active(td))
		mt_release_contacts(hdev);
	mt_clear_bit_unlock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags);
}

static int mt_event(struct hid_device *hid, struct hid_field *field,
		    struct hid_usage *usage, int32_t value)
{
	struct mt_device *td = hid_get_drvdata(hid);
	struct mt_report_data *rdata;

	(void)usage;
	(void)value;
	rdata = mt_lookup_report_data(td, field->report);
	if (rdata && rdata->is_mt_collection)
		return 1; // the contacts are reported per frame, in mt_report
	return 0;
}

static void mt_report(struct hid_device *hid, struct hid_report *report)
{
	struct mt_device *td = hid_get_drvdata(hid);
	struct mt_report_data *rdata;
	struct hid_field *field = report->maxfield ? report->field[0] : NULL;

	if (!(hid->claimed & HID_CLAIMED_INPUT))
		return;
	rdata = mt_lookup_report_data(td, report);
	if (rdata && rdata->is_mt_collection) {
		mt_touch_report(hid, rdata);
		return;
	}

	/* The Yoga Book 9i sends reports with every pen and contact state
	 * zero that mean nothing: consume them. */
	if (rdata && rdata->application &&
	    (rdata->application->quirks & MT_QUIRK_YOGABOOK9I)) {
		bool all_zero_report = true;

		for (unsigned int f = 0;
		     f < report->maxfield && all_zero_report; f++) {
			struct hid_field *fld = report->field[f];

			for (unsigned int i = 0; i < fld->report_count; i++) {
				unsigned int usage = fld->usage[i].hid;

				if ((usage == HID_DG_INRANGE ||
				     usage == HID_DG_TIPSWITCH ||
				     usage == HID_DG_BARRELSWITCH ||
				     usage == HID_DG_BARRELSWITCH2 ||
				     usage == HID_DG_CONTACTID ||
				     usage == HID_DG_TILT_X ||
				     usage == HID_DG_TILT_Y) &&
				    fld->value[i] != 0) {
					all_zero_report = false;
					break;
				}
			}
		}
		if (all_zero_report)
			return;
	}
	/* Everything else: hid-input reported it, end the packet. */
	if (field && field->hidinput && field->hidinput->input)
		input_sync(field->hidinput->input);
}

// ---------------------------------------------------------------------------
// Probe, remove, power
// ---------------------------------------------------------------------------

/* Lenovo Yoga Book 9i: its descriptor carries a Touch Pad application meant
 * for another operating system's touchpad emulation, next to the real
 * touchscreens.  It is cut out, so the touchscreens are not taken for
 * button pads.  The firmware also resets when it gets control requests
 * while its serial interface starts: the certification blob, the contact
 * maximum and the surface/button switch usages are neutralised so that no
 * GET/SET_REPORT goes out for them (the class gives the contact count;
 * Input Mode stays, sent once at probe, which also clears a ghost contact
 * the firmware keeps). */
static void mt_yogabook9_fixup(struct hid_device *hdev, uint8_t *rdesc,
			       unsigned int *size)
{
	/* Usage Page (Digitizer), Usage (Touch Pad), Collection (Application) */
	static const uint8_t tp_app_hdr[] = { 0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01 };
	/* Usage Page (0xff00): the certification blob */
	static const uint8_t win8_page[] = { 0x06, 0x00, 0xff };
	/* Usage (Contact Count Maximum) */
	static const uint8_t ccmax_usage[] = { 0x09, 0x55 };
	unsigned int i;

	(void)hdev;
	for (i = 0; i + sizeof(tp_app_hdr) <= *size; i++) {
		uint8_t *start, *p, *coll_end = NULL;
		int depth = 0;
		unsigned int drop;

		if (kmemcmp(rdesc + i, tp_app_hdr, sizeof(tp_app_hdr)))
			continue;
		/* Walk the items to the End Collection that closes it. */
		start = p = rdesc + i;
		while (p < rdesc + *size) {
			uint8_t b = *p;
			unsigned int item_len;

			if (b == 0xfe) { // long item
				if (p + 2 >= rdesc + *size)
					break;
				item_len = p[1] + 3u;
			} else {
				item_len = (b & 3) == 3 ? 5 : (b & 3) + 1u;
			}
			if (p + item_len > rdesc + *size)
				break;
			if ((b & 0xfc) == 0xa0) {
				depth++;
			} else if (b == 0xc0 && --depth == 0) {
				coll_end = p;
				break;
			}
			p += item_len;
		}
		if (!coll_end) {
			hid_err(hdev, "Yoga Book 9: Touch Pad End Collection "
				      "not found\n");
			break;
		}
		drop = (unsigned int)(coll_end - start) + 1;
		for (uint8_t *d = start; d + drop < rdesc + *size; d++)
			*d = d[drop];
		*size -= drop;
		break;
	}
	for (i = 0; i + sizeof(win8_page) <= *size; i++)
		if (!kmemcmp(rdesc + i, win8_page, sizeof(win8_page)))
			rdesc[i + 2] = 0x0f; // page 0xff00 -> 0x0f00
	for (i = 0; i + sizeof(ccmax_usage) <= *size; i++)
		if (!kmemcmp(rdesc + i, ccmax_usage, sizeof(ccmax_usage)))
			rdesc[i + 1] = 0x00;
	for (i = 0; i + 2 <= *size; i++)
		if (rdesc[i] == 0x09 &&
		    (rdesc[i + 1] == 0x57 || rdesc[i + 1] == 0x58))
			rdesc[i + 1] = 0x00;
}

/* Goodix GT7868Q touchpads declare a logical minimum where a maximum is
 * meant. */
static const uint8_t *mt_report_fixup(struct hid_device *hdev, uint8_t *rdesc,
				      unsigned int *size)
{
	if (hdev->vendor == I2C_VENDOR_ID_GOODIX &&
	    (hdev->product == I2C_DEVICE_ID_GOODIX_01E8 ||
	     hdev->product == I2C_DEVICE_ID_GOODIX_01E9)) {
		if (*size < 608) {
			hid_info(hdev, "GT7868Q fixup: report descriptor is only "
				       "%u bytes, skipping\n",
				 *size);
		} else if (rdesc[607] == 0x15) {
			rdesc[607] = 0x25;
			hid_info(hdev, "GT7868Q report descriptor fixup is "
				       "applied\n");
		} else {
			hid_info(hdev, "GT7868Q descriptor not as expected "
				       "(got %x), no fixup\n",
				 rdesc[607]);
		}
	}
	if (hdev->vendor == USB_VENDOR_ID_LENOVO &&
	    hdev->product == USB_DEVICE_ID_LENOVO_YOGABOOK9I)
		mt_yogabook9_fixup(hdev, rdesc, size);
	return rdesc;
}

/* LG panels mark their contact id constant. */
static void mt_fix_const_fields(struct hid_device *hdev, unsigned int usage)
{
	struct hid_report *report;

	list_for_each_entry(report,
			    &hdev->report_enum[HID_INPUT_REPORT].report_list,
			    list)
		for (unsigned int i = 0; i < report->maxfield; i++) {
			struct hid_field *field = report->field[i];

			if (field->maxusage < 1 ||
			    field->usage[0].hid != usage ||
			    !(field->flags & HID_MAIN_ITEM_CONSTANT))
				continue;
			field->flags &= ~HID_MAIN_ITEM_CONSTANT;
			field->flags |= HID_MAIN_ITEM_VARIABLE;
		}
}

static void mt_free(struct mt_device *td)
{
	struct mt_report_data *rdata, *rtmp;
	struct mt_application *app, *atmp;
	struct mt_usages *usage, *utmp;

	list_for_each_entry_safe(rdata, rtmp, &td->reports, list) {
		list_del(&rdata->list);
		kfree(rdata);
	}
	list_for_each_entry_safe(app, atmp, &td->applications, list) {
		list_for_each_entry_safe(usage, utmp, &app->mt_usages, list) {
			list_del(&usage->list);
			kfree(usage);
		}
		list_del(&app->list);
		kfree(app->pending_palm_slots);
		kfree(app);
	}
	kfree(td->active_slots);
	kfree(td);
}

static int mt_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	const struct mt_class *mtclass = mt_classes; // MT_CLS_DEFAULT
	struct mt_device *td;
	int ret;

	for (unsigned int i = 0; mt_classes[i].name; i++) {
		if (id->driver_data == (unsigned long)mt_classes[i].name) {
			mtclass = &mt_classes[i];
			break;
		}
	}

	td = kcalloc(1, sizeof(*td));
	if (!td) {
		hid_err(hdev, "cannot allocate multitouch data\n");
		return -ENOMEM;
	}
	td->hdev = hdev;
	td->mtclass = *mtclass;
	td->inputmode_value = MT_INPUTMODE_TOUCHSCREEN;
	INIT_LIST_HEAD(&td->applications);
	INIT_LIST_HEAD(&td->reports);
	INIT_DELAYED_WORK(&td->release_work, mt_expired_timeout);
	hid_set_drvdata(hdev, td);

	if (id->vendor == HID_ANY_ID && id->product == HID_ANY_ID)
		td->serial_maybe = true;

	/* One inverted axis turns the orientation around; both inverted
	 * is a rotation and leaves it alone. */
	if ((hdev->quirks & (HID_QUIRK_X_INVERT | HID_QUIRK_Y_INVERT)) &&
	    !((hdev->quirks & HID_QUIRK_X_INVERT) &&
	      (hdev->quirks & HID_QUIRK_Y_INVERT)))
		td->mtclass.quirks |= MT_QUIRK_ORIENTATION_INVERT;

	/* Frames can span several reports: we send SYN_REPORT. */
	hdev->quirks |= HID_QUIRK_NO_INPUT_SYNC;
	/* Touch, mouse, keys of one device: one input device each. */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;
	if (id->group != HID_GROUP_MULTITOUCH_WIN_8)
		hdev->quirks |= HID_QUIRK_MULTI_INPUT;
	if (mtclass->quirks & MT_QUIRK_FORCE_MULTI_INPUT) {
		hdev->quirks &= ~HID_QUIRK_INPUT_PER_APP;
		hdev->quirks |= HID_QUIRK_MULTI_INPUT;
	}

	ret = hid_parse(hdev);
	if (ret)
		goto fail;

	if (mtclass->name == MT_CLS_APPLE_TOUCHBAR &&
	    !hid_find_field(hdev, HID_INPUT_REPORT, HID_DG_TOUCHPAD,
			    HID_DG_TRANSDUCER_INDEX)) {
		ret = -ENODEV;
		goto fail;
	}

	if (mtclass->quirks & MT_QUIRK_FIX_CONST_CONTACT_ID)
		mt_fix_const_fields(hdev, HID_DG_CONTACTID);

	if (hdev->vendor == USB_VENDOR_ID_SIS_TOUCH)
		hdev->quirks |= HID_QUIRK_NOGET;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		goto fail;

	mt_set_modes(hdev, HID_LATENCY_NORMAL, TOUCHPAD_REPORT_ALL);
	return 0;

fail:
	cancel_delayed_work_sync(&td->release_work);
	hid_set_drvdata(hdev, NULL);
	mt_free(td);
	return ret;
}

static void mt_remove(struct hid_device *hdev)
{
	struct mt_device *td = hid_get_drvdata(hdev);

	/* Reports are held off (the core holds the input lock): nothing
	 * arms the release again once it is cancelled. */
	cancel_delayed_work_sync(&td->release_work);
	hid_hw_stop(hdev);
	hid_set_drvdata(hdev, NULL);
	mt_free(td);
}

static int mt_suspend(struct hid_device *hdev, int pm_event)
{
	struct mt_device *td = hid_get_drvdata(hdev);

	(void)pm_event;
	/* High latency saves power while suspended; a device that may wake
	 * the system keeps reporting. */
	if ((td->mtclass.quirks & MT_QUIRK_DISABLE_WAKEUP) ||
	    !hid_hw_may_wakeup(hdev))
		mt_set_modes(hdev, HID_LATENCY_HIGH, TOUCHPAD_REPORT_NONE);
	else
		mt_set_modes(hdev, HID_LATENCY_HIGH, TOUCHPAD_REPORT_ALL);
	return 0;
}

static int mt_reset_resume(struct hid_device *hdev)
{
	/* The device forgot everything, including the contacts down. */
	mt_release_contacts(hdev);
	mt_set_modes(hdev, HID_LATENCY_NORMAL, TOUCHPAD_REPORT_ALL);
	return 0;
}

static int mt_resume(struct hid_device *hdev)
{
	/* Some older panels need SET_IDLE after resume; it is harmless to
	 * the others. */
	hid_hw_idle(hdev, 0, 0, HID_REQ_SET_IDLE);
	mt_set_modes(hdev, HID_LATENCY_NORMAL, TOUCHPAD_REPORT_ALL);
	return 0;
}

/* The first input node of the device opened: full reporting. */
static void mt_on_hid_hw_open(struct hid_device *hdev)
{
	mt_set_modes(hdev, HID_LATENCY_NORMAL, TOUCHPAD_REPORT_ALL);
}

/* The last one closed: nobody listens, the device may rest. */
static void mt_on_hid_hw_close(struct hid_device *hdev)
{
	struct mt_device *td = hid_get_drvdata(hdev);

	if (td->mtclass.quirks & MT_QUIRK_KEEP_LATENCY_ON_CLOSE)
		mt_set_modes(hdev, HID_LATENCY_NORMAL, TOUCHPAD_REPORT_NONE);
	else
		mt_set_modes(hdev, HID_LATENCY_HIGH, TOUCHPAD_REPORT_NONE);
}

static struct hid_driver mt_driver = {
	.name = "hid-multitouch",
	.id_table = mt_devices,
	.probe = mt_probe,
	.remove = mt_remove,
	.input_mapping = mt_input_mapping,
	.input_mapped = mt_input_mapped,
	.input_configured = mt_input_configured,
	.feature_mapping = mt_feature_mapping,
	.event = mt_event,
	.report = mt_report,
	.report_fixup = mt_report_fixup,
	.suspend = mt_suspend,
	.resume = mt_resume,
	.reset_resume = mt_reset_resume,
	.on_hid_hw_open = mt_on_hid_hw_open,
	.on_hid_hw_close = mt_on_hid_hw_close,
};

int hid_multitouch_init(void)
{
	return hid_register_driver(&mt_driver);
}

void hid_multitouch_exit(void)
{
	hid_unregister_driver(&mt_driver);
}
