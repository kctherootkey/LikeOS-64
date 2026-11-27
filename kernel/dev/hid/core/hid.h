// LikeOS HID core - transport-neutral Human Interface Device support
//
// A HID device describes itself with a report descriptor: a byte-coded list
// of the reports it sends (input), accepts (output) and exchanges on request
// (feature), and of every control inside them -- its usage, its bit offset
// and size, its logical and physical range, its unit.  The HID core parses
// that descriptor into reports and fields (struct hid_report, struct
// hid_field), extracts the values of every incoming report, and hands them to
// the HID driver bound to the device: hid-multitouch for touch devices, the
// generic driver for everything else.  Both build their input devices with
// hid-input, which maps HID usages to input event codes.
//
//     transport (I2C-HID, later USB)            hid_device + hid_ll_driver
//         |  hid_input_report()                     ^ raw_request/output_report
//         v                                         |
//     HID core: parse, extract, dispatch  --->  HID driver (hid-multitouch,
//         |                                      hid-generic) --> hid-input
//         v                                         |
//     input core (input.c, evdev)  <----------------+
//
// The transport owns the bus: it reads the HID and report descriptors,
// moves report bytes, and implements struct hid_ll_driver.  It never parses
// a report.  The HID core never touches the bus except through the
// ll_driver operations.
//
// ---------------------------------------------------------------------------
// The transport contract (what an I2C-HID or USB-HID transport does)
// ---------------------------------------------------------------------------
//
//   hid = hid_allocate_device();            // process context; NULL: -ENOMEM
//   hid->bus = BUS_I2C;  hid->vendor = ...; hid->product = ...;
//   hid->version = bcdVersion;              // shows as Version= in /proc
//   ksnprintf(hid->name, sizeof(hid->name), "%s %04X:%04X", acpi_name, v, p);
//   ksnprintf(hid->phys, sizeof(hid->phys), "i2c-%s", acpi_name);
//   ksnprintf(hid->parent_path, ...);       // sysfs-style path of the bus
//                                           // client, for the S: line
//   hid->ll_driver = &i2c_hid_ll_driver;
//   hid->driver_data = client;              // the transport's own pointer
//   rc = hid_add_device(hid);               // calls ll_driver->parse(), then
//                                           // binds and probes a driver
//   ...
//   hid_destroy_device(hid);                // driver removed, device freed
//
// ll_driver->parse() reads the report descriptor from the device and hands
// it to hid_parse_report(hid, bytes, size), which copies it (the transport
// keeps ownership of its buffer).
//
// ll_driver->start() is called when a driver binds (hid_hw_start); from
// then on the transport may deliver input reports.  ll_driver->stop() is
// called when it unbinds; the transport must not return from stop() while a
// hid_input_report() call is still running, and must not start new ones
// afterwards (free/disable its interrupt first).  open()/close() bracket the
// time any input node of the device is open (reference counted by the
// core); power() is a PM_HINT_* hint.
//
// Input reports: hid_input_report(hid, HID_INPUT_REPORT, buf, len, 1) from
// the transport's threaded interrupt handler or any other PROCESS context
// (it takes a mutex with trylock and never sleeps on it, but must not be
// called from a hard interrupt handler or with a spinlock held).  `buf'
// holds the report as the device sent it, starting with the report ID byte
// when the device uses numbered reports; the transport strips its own
// framing (the I2C-HID length prefix).  The core may modify the buffer
// contents (drivers' raw_event hooks may), but never writes past `len' and
// never keeps the pointer.  A report shorter than the descriptor says is
// zero-padded in a private buffer (with a rate-limited message); a report
// with an unknown ID is dropped.  Returns 0, or a negative errno: -EBUSY
// while a driver is being bound or unbound (the report is dropped, as the
// device is not ready for it), -ENODEV with no driver bound.
//
// Report buffers in both directions between the core and the transport
// ALWAYS begin with the report ID byte -- 0 for a device without numbered
// reports -- and `len' counts that byte:
//
//   raw_request(hid, reportnum, buf, len, rtype, reqtype)
//       rtype   HID_INPUT_REPORT / HID_OUTPUT_REPORT / HID_FEATURE_REPORT
//       reqtype HID_REQ_GET_REPORT: fill buf[0] = reportnum and the report
//               data from buf[1], at most `len' bytes in all; return the
//               number of bytes stored (including buf[0]) or -errno.
//               HID_REQ_SET_REPORT: send buf[1..len-1] as report
//               `reportnum' (buf[0] == reportnum); return bytes sent
//               (including buf[0]) or -errno.
//   output_report(hid, buf, len)
//       send buf[1..len-1] as output report buf[0] over the interrupt /
//       output channel; return bytes sent or -errno, -ENOSYS when the
//       transport has no such channel (the core then uses raw_request).
//
// The caller owns `buf' in both; the transport must not keep it.  Both are
// called in process context and may sleep.  `len' never exceeds
// ll_driver->max_buffer_size (HID_MAX_BUFFER_SIZE when 0).
//
// Built-in HID drivers are registered by hid_core_init(), which the
// transport's initialisation calls once before adding its first device
// (calling it again is harmless).
//
// Locking:
//   hid_bus_lock (global mutex)    driver/device lists, binding
//   hid->driver_input_lock (mutex) held while a driver is probed or removed
//                                  and around every report delivery
//                                  (trylock there); orders reports against
//                                  bind/unbind
//   hid->ll_open_lock (mutex)      the open count
// Order: hid_bus_lock -> driver_input_lock -> (input core locks).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_HID_CORE_HID_H_
#define _KERNEL_DEV_HID_CORE_HID_H_

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/list.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/workqueue.h>
#include <kernel/dev/input/input.h>
#include <kernel/dev/hid/core/hid-usages.h>
#include <kernel/dev/hid/core/hid-ids.h>

#define HID_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------
// Report descriptor items
// ---------------------------------------------------------------------------
// A short item is one prefix byte -- bTag (bits 7-4), bType (3-2), bSize
// (1-0; 3 means 4 bytes) -- followed by 0, 1, 2 or 4 data bytes.  A long
// item is the prefix 0xfe, a data size byte, a tag byte and up to 255 data
// bytes; the specification defines no long items, so a descriptor that uses
// one is rejected.
struct hid_item {
	unsigned int format; // HID_ITEM_FORMAT_*
	uint8_t size; // data bytes (0, 1, 2, 4; up to 255 for long items)
	uint8_t type; // HID_ITEM_TYPE_*
	uint8_t tag;
	union {
		uint8_t u8;
		int8_t s8;
		uint16_t u16;
		int16_t s16;
		uint32_t u32;
		int32_t s32;
		const uint8_t *longdata;
	} data;
};

#define HID_ITEM_FORMAT_SHORT 0
#define HID_ITEM_FORMAT_LONG 1
#define HID_ITEM_TAG_LONG 15

#define HID_ITEM_TYPE_MAIN 0
#define HID_ITEM_TYPE_GLOBAL 1
#define HID_ITEM_TYPE_LOCAL 2
#define HID_ITEM_TYPE_RESERVED 3

#define HID_MAIN_ITEM_TAG_INPUT 8
#define HID_MAIN_ITEM_TAG_OUTPUT 9
#define HID_MAIN_ITEM_TAG_FEATURE 11
#define HID_MAIN_ITEM_TAG_BEGIN_COLLECTION 10
#define HID_MAIN_ITEM_TAG_END_COLLECTION 12
#define HID_MAIN_ITEM_TAG_RESERVED_MIN 13
#define HID_MAIN_ITEM_TAG_RESERVED_MAX 15

#define HID_GLOBAL_ITEM_TAG_USAGE_PAGE 0
#define HID_GLOBAL_ITEM_TAG_LOGICAL_MINIMUM 1
#define HID_GLOBAL_ITEM_TAG_LOGICAL_MAXIMUM 2
#define HID_GLOBAL_ITEM_TAG_PHYSICAL_MINIMUM 3
#define HID_GLOBAL_ITEM_TAG_PHYSICAL_MAXIMUM 4
#define HID_GLOBAL_ITEM_TAG_UNIT_EXPONENT 5
#define HID_GLOBAL_ITEM_TAG_UNIT 6
#define HID_GLOBAL_ITEM_TAG_REPORT_SIZE 7
#define HID_GLOBAL_ITEM_TAG_REPORT_ID 8
#define HID_GLOBAL_ITEM_TAG_REPORT_COUNT 9
#define HID_GLOBAL_ITEM_TAG_PUSH 10
#define HID_GLOBAL_ITEM_TAG_POP 11

#define HID_LOCAL_ITEM_TAG_USAGE 0
#define HID_LOCAL_ITEM_TAG_USAGE_MINIMUM 1
#define HID_LOCAL_ITEM_TAG_USAGE_MAXIMUM 2
#define HID_LOCAL_ITEM_TAG_DESIGNATOR_INDEX 3
#define HID_LOCAL_ITEM_TAG_DESIGNATOR_MINIMUM 4
#define HID_LOCAL_ITEM_TAG_DESIGNATOR_MAXIMUM 5
#define HID_LOCAL_ITEM_TAG_STRING_INDEX 7
#define HID_LOCAL_ITEM_TAG_STRING_MINIMUM 8
#define HID_LOCAL_ITEM_TAG_STRING_MAXIMUM 9
#define HID_LOCAL_ITEM_TAG_DELIMITER 10

/* Data flags of Input / Output / Feature main items. */
#define HID_MAIN_ITEM_CONSTANT 0x001 // constant (padding) vs data
#define HID_MAIN_ITEM_VARIABLE 0x002 // one value per usage vs array
#define HID_MAIN_ITEM_RELATIVE 0x004
#define HID_MAIN_ITEM_WRAP 0x008
#define HID_MAIN_ITEM_NONLINEAR 0x010
#define HID_MAIN_ITEM_NO_PREFERRED 0x020
#define HID_MAIN_ITEM_NULL_STATE 0x040
#define HID_MAIN_ITEM_VOLATILE 0x080
#define HID_MAIN_ITEM_BUFFERED_BYTE 0x100

// ---------------------------------------------------------------------------
// Limits.  Every count and size the device gives us is checked against
// these before it is used.
// ---------------------------------------------------------------------------
#define HID_MAX_IDS 256 // report IDs 1..255 (0: unnumbered)
#define HID_MAX_FIELDS 256 // fields (main items) per report
#define HID_MAX_USAGES 12288 // usages per main item, Report Count limit
#define HID_MAX_REPORT_SIZE 256 // Report Size (bits per value) limit
#define HID_GLOBAL_STACK_SIZE 4 // Push depth
#define HID_MAX_COLLECTION_DEPTH 64 // nesting of collections
#define HID_DEFAULT_NUM_COLLECTIONS 16 // initial collection table
#define HID_MAX_BUFFER_SIZE 16384 // bytes in one report, ID included
/* Report descriptors a transport reads from a device are at most this long
 * (the I2C-HID and USB transports reject longer ones). */
#define HID_MAX_DESCRIPTOR_SIZE 4096
/* What the core accepts at all (a fixed-up descriptor may grow). */
#define HID_MAX_RDESC_SIZE 65536
/* Usage slots of all fields of one device together.  Real devices need a
 * few thousand; the cap bounds the memory a hostile descriptor can make
 * the parser allocate (zero-bit fields with Report Count 12288 cost no
 * report space but a lot of field memory). */
#define HID_MAX_TOTAL_USAGES (1u << 17)

// ---------------------------------------------------------------------------
// Report types and class requests
// ---------------------------------------------------------------------------
#define HID_INPUT_REPORT 0
#define HID_OUTPUT_REPORT 1
#define HID_FEATURE_REPORT 2
#define HID_REPORT_TYPES 3

#define HID_REQ_GET_REPORT 0x01
#define HID_REQ_GET_IDLE 0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT 0x09
#define HID_REQ_SET_IDLE 0x0a
#define HID_REQ_SET_PROTOCOL 0x0b

/* ll_driver->power() levels. */
#define PM_HINT_FULLON (1 << 5)
#define PM_HINT_NORMAL (1 << 1)

/* Driver suspend events (hid_driver_suspend). */
#define HID_PM_EVENT_SUSPEND 0x0002
#define HID_PM_EVENT_FREEZE 0x0001
#define HID_PM_EVENT_AUTO_SUSPEND 0x0402

// ---------------------------------------------------------------------------
// Device groups: what kind of device the descriptor says it is, found by a
// quick scan before any driver is chosen (driver id tables match on it).
// ---------------------------------------------------------------------------
#define HID_GROUP_ANY 0x0000
#define HID_GROUP_GENERIC 0x0001
#define HID_GROUP_MULTITOUCH 0x0002
#define HID_GROUP_SENSOR_HUB 0x0003
#define HID_GROUP_MULTITOUCH_WIN_8 0x0004
#define HID_GROUP_RMI 0x0100
#define HID_GROUP_WACOM 0x0101
#define HID_GROUP_LOGITECH_DJ_DEVICE 0x0102
#define HID_GROUP_STEAM 0x0103
#define HID_GROUP_LOGITECH_27MHZ_DEVICE 0x0104
#define HID_GROUP_VIVALDI 0x0105

#define HID_BUS_ANY 0xffff
#define HID_ANY_ID (~0u)

// ---------------------------------------------------------------------------
// Device quirks (hid->quirks)
// ---------------------------------------------------------------------------
#define HID_QUIRK_INVERT (1ul << 0)
#define HID_QUIRK_NOTOUCH (1ul << 1)
#define HID_QUIRK_IGNORE (1ul << 2)
#define HID_QUIRK_NOGET (1ul << 3)
#define HID_QUIRK_HIDDEV_FORCE (1ul << 4)
#define HID_QUIRK_BADPAD (1ul << 5)
#define HID_QUIRK_MULTI_INPUT (1ul << 6) // one input device per report
#define HID_QUIRK_HIDINPUT_FORCE (1ul << 7)
#define HID_QUIRK_ALWAYS_POLL (1ul << 10)
#define HID_QUIRK_INPUT_PER_APP (1ul << 11) // one input device per application
#define HID_QUIRK_X_INVERT (1ul << 12)
#define HID_QUIRK_Y_INVERT (1ul << 13)
#define HID_QUIRK_IGNORE_MOUSE (1ul << 14)
#define HID_QUIRK_SKIP_OUTPUT_REPORTS (1ul << 16)
#define HID_QUIRK_SKIP_OUTPUT_REPORT_ID (1ul << 17)
#define HID_QUIRK_NO_OUTPUT_REPORTS_ON_INTR_EP (1ul << 18)
#define HID_QUIRK_HAVE_SPECIAL_DRIVER (1ul << 19)
#define HID_QUIRK_INCREMENT_USAGE_ON_DUPLICATE (1ul << 20)
#define HID_QUIRK_NOINVERT (1ul << 21)
#define HID_QUIRK_IGNORE_SPECIAL_DRIVER (1ul << 22)
#define HID_QUIRK_POWER_ON_AFTER_BACKLIGHT (1ul << 23)
#define HID_QUIRK_FULLSPEED_INTERVAL (1ul << 28)
#define HID_QUIRK_NO_INIT_REPORTS (1ul << 29) // never GET_REPORT at setup
#define HID_QUIRK_NO_IGNORE (1ul << 30)
#define HID_QUIRK_NO_INPUT_SYNC (1ul << 31) // the driver sends SYN_REPORT

// ---------------------------------------------------------------------------
// hid_hw_start() connect mask, hid->claimed, hid->status
// ---------------------------------------------------------------------------
#define HID_CONNECT_HIDINPUT (1u << 0)
#define HID_CONNECT_HIDINPUT_FORCE (1u << 1)
#define HID_CONNECT_HIDRAW (1u << 2)
#define HID_CONNECT_HIDDEV (1u << 3)
#define HID_CONNECT_HIDDEV_FORCE (1u << 4)
#define HID_CONNECT_FF (1u << 5)
#define HID_CONNECT_DRIVER (1u << 6)
#define HID_CONNECT_DEFAULT                                         \
	(HID_CONNECT_HIDINPUT | HID_CONNECT_HIDRAW | HID_CONNECT_HIDDEV | \
	 HID_CONNECT_FF)

#define HID_CLAIMED_INPUT (1u << 0)
#define HID_CLAIMED_HIDDEV (1u << 1)
#define HID_CLAIMED_HIDRAW (1u << 2)
#define HID_CLAIMED_DRIVER (1u << 3)

#define HID_STAT_ADDED (1u << 0)
#define HID_STAT_PARSED (1u << 1)
#define HID_STAT_DUP_DETECTED (1u << 2)
#define HID_STAT_REPROBED (1u << 3)

// ---------------------------------------------------------------------------
// Parsed descriptor
// ---------------------------------------------------------------------------
struct hid_collection {
	int parent_idx; // index in hid->collection[], -1 for top level
	unsigned int type; // HID_COLLECTION_*
	unsigned int usage; // the usage in front of Collection
	unsigned int level; // nesting depth, 0 for top level
};

struct hid_input;
struct hid_report;
struct hid_device;

/* One usage of a field. */
struct hid_usage {
	unsigned int hid; // full 32-bit usage
	unsigned int collection_index; // innermost collection around it
	unsigned int usage_index; // its index within the field
	/* Resolution Multiplier feature in effect (Generic Desktop 0x48):
	 * hi-resolution wheels report 1/multiplier of a notch. */
	int resolution_multiplier;
	int wheel_accumulated; // hi-res wheel remainder, 1/120 notches
	// ---- hid-input mapping ----
	uint16_t code; // input event code
	uint8_t type; // input event type, 0: not mapped
	int16_t hat_min; // hat switch logical range
	int16_t hat_max;
	int16_t hat_dir;
};

/* One main item: report_count values of report_size bits each, starting
 * report_offset bits into the report (after the ID byte). */
struct hid_field {
	unsigned int physical; // usage of the innermost physical collection
	unsigned int logical; // ... logical collection
	unsigned int application; // ... application collection
	struct hid_usage *usage; // maxusage entries
	unsigned int maxusage; // max(usages declared, report_count)
	unsigned int flags; // HID_MAIN_ITEM_*
	unsigned int report_offset; // bits
	unsigned int report_size; // bits per value (0..256)
	unsigned int report_count; // values
	unsigned int report_type; // HID_*_REPORT
	/* Current values (maxusage entries; indices < report_count carry
	 * data).  Drivers may keep pointers into this array: it lives as long
	 * as the parsed descriptor. */
	int32_t *value;
	int32_t *new_value; // values of the report being processed
	int32_t logical_minimum;
	int32_t logical_maximum;
	int32_t physical_minimum;
	int32_t physical_maximum;
	int32_t unit_exponent;
	unsigned int unit;
	bool ignored; // array field reported ErrorRollOver this time
	struct hid_report *report;
	unsigned int index; // in report->field[]
	struct hid_input *hidinput; // the input device it was mapped to
	uint16_t dpad; // hat code of the field's D-pad directions, 0: none
};

struct hid_report {
	struct list_head list; // on report_enum->report_list
	struct list_head hidinput_list; // on hidinput->reports
	unsigned int id; // 0 for unnumbered reports
	unsigned int type; // HID_*_REPORT
	unsigned int application; // usage of its application collection
	struct hid_field *field[HID_MAX_FIELDS];
	unsigned int maxfield;
	unsigned int size; // bits, without the ID byte
	struct hid_device *device;
	// ---- tool tracking of hid-input (pens) ----
	bool tool_active;
	unsigned int tool;
};

struct hid_report_enum {
	unsigned int numbered; // the device uses report IDs
	struct list_head report_list; // in descriptor order
	struct hid_report *report_id_hash[HID_MAX_IDS];
};

/* An input device hid-input made of some of the device's reports. */
struct hid_input {
	struct list_head list; // on hid->inputs
	struct hid_report *report; // HID_QUIRK_MULTI_INPUT: its report
	struct input_dev *input;
	char *name; // owned name string, NULL: input->name = hid->name
	struct list_head reports; // reports mapped into it
	unsigned int application;
	bool registered;
};

// ---------------------------------------------------------------------------
// Drivers
// ---------------------------------------------------------------------------
struct hid_device_id {
	uint16_t bus; // BUS_*, HID_BUS_ANY; 0 ends a table
	uint16_t group; // HID_GROUP_*, HID_GROUP_ANY
	uint32_t vendor; // HID_ANY_ID matches all
	uint32_t product;
	unsigned long driver_data;
};

#define HID_DEVICE(b, g, ven, prod) \
	.bus = (b), .group = (g), .vendor = (ven), .product = (prod)
#define HID_USB_DEVICE(ven, prod) HID_DEVICE(BUS_USB, HID_GROUP_ANY, ven, prod)
#define HID_BLUETOOTH_DEVICE(ven, prod) \
	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_ANY, ven, prod)
#define HID_I2C_DEVICE(ven, prod) HID_DEVICE(BUS_I2C, HID_GROUP_ANY, ven, prod)

struct hid_driver {
	const char *name;
	const struct hid_device_id *id_table;
	/* Optional: refuse a device the table matched (the generic driver
	 * refuses what a specific driver wants). */
	bool (*match)(struct hid_device *hdev, bool ignore_special_driver);
	/* Bind.  Without probe the core parses and starts the device with
	 * HID_CONNECT_DEFAULT.  Process context; reports are held off until
	 * it returns (or the driver calls hid_device_io_start()). */
	int (*probe)(struct hid_device *hdev, const struct hid_device_id *id);
	/* Unbind.  Without remove the core calls hid_hw_stop(). */
	void (*remove)(struct hid_device *hdev);
	/* Raw report bytes before the core parses them; < 0 drops the
	 * report.  Report delivery context (process, no sleeping locks held
	 * by the caller except driver_input_lock). */
	int (*raw_event)(struct hid_device *hdev, struct hid_report *report,
			 uint8_t *data, int size);
	/* One usage's value; non-zero: the driver consumed it (negative
	 * values are logged as errors). */
	int (*event)(struct hid_device *hdev, struct hid_field *field,
		     struct hid_usage *usage, int32_t value);
	/* After all fields of a report were processed. */
	void (*report)(struct hid_device *hdev, struct hid_report *report);
	/* Rewrite the descriptor before it is parsed: return `buf' (edited
	 * in place, *size updated, never grown) or a buffer of the driver's
	 * that lives as long as the device. */
	const uint8_t *(*report_fixup)(struct hid_device *hdev, uint8_t *buf,
				       unsigned int *size);
	/* hid-input hooks: > 0 the driver mapped the usage itself, < 0
	 * ignore it, 0 let hid-input decide.  input_mapped() runs after
	 * hid-input's mapping and can still veto it (< 0). */
	int (*input_mapping)(struct hid_device *hdev, struct hid_input *hidinput,
			     struct hid_field *field, struct hid_usage *usage,
			     unsigned long **bit, int *max);
	int (*input_mapped)(struct hid_device *hdev, struct hid_input *hidinput,
			    struct hid_field *field, struct hid_usage *usage,
			    unsigned long **bit, int *max);
	/* Every input device before registration (name, MT slots, ...). */
	int (*input_configured)(struct hid_device *hdev,
				struct hid_input *hidinput);
	/* Every usage of every feature report, before the input reports are
	 * mapped. */
	void (*feature_mapping)(struct hid_device *hdev, struct hid_field *field,
				struct hid_usage *usage);
	int (*suspend)(struct hid_device *hdev, int pm_event);
	int (*resume)(struct hid_device *hdev);
	int (*reset_resume)(struct hid_device *hdev);
	/* The first user opened an input node of the device / the last one
	 * closed it (after the transport's open/close; ll_open_lock held). */
	void (*on_hid_hw_open)(struct hid_device *hdev);
	void (*on_hid_hw_close)(struct hid_device *hdev);

	// ---- core-private ----
	struct list_head node; // registered drivers (hid_bus_lock)
	bool registered;
};

/* The transport side. */
struct hid_ll_driver {
	int (*start)(struct hid_device *hdev);
	void (*stop)(struct hid_device *hdev);
	int (*open)(struct hid_device *hdev);
	void (*close)(struct hid_device *hdev);
	int (*power)(struct hid_device *hdev, int level);
	/* Read the report descriptor and call hid_parse_report(). */
	int (*parse)(struct hid_device *hdev);
	/* Optional asynchronous report request (else raw_request is used). */
	void (*request)(struct hid_device *hdev, struct hid_report *report,
			int reqtype);
	/* Optional: wait for outstanding asynchronous requests. */
	int (*wait)(struct hid_device *hdev);
	/* Mandatory.  See the contract at the top. */
	int (*raw_request)(struct hid_device *hdev, unsigned char reportnum,
			   uint8_t *buf, size_t len, unsigned char rtype,
			   int reqtype);
	int (*output_report)(struct hid_device *hdev, uint8_t *buf, size_t len);
	/* Optional SET_IDLE. */
	int (*idle)(struct hid_device *hdev, int report, int idle, int reqtype);
	/* Optional: may the device wake the system. */
	bool (*may_wakeup)(struct hid_device *hdev);
	/* Largest report buffer, ID byte included; 0: HID_MAX_BUFFER_SIZE. */
	unsigned int max_buffer_size;
};

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------
struct hid_device {
	// ---- identity: set by the transport before hid_add_device() ----
	uint16_t bus; // BUS_I2C, BUS_USB, ...
	uint16_t group; // 0: found by scanning the descriptor
	uint32_t vendor;
	uint32_t product;
	uint32_t version; // bcdVersion / bcdDevice
	uint8_t country;
	char name[128]; // "ELAN073B:00 04F3:3169"
	char phys[64]; // "i2c-ELAN073B:00"
	char uniq[64]; // serial number, "" for none
	/* Device-tree path of the transport's device, e.g.
	 * "/devices/pci0000:00/0000:00:15.0/i2c_designware.0/i2c-0/
	 *  i2c-ELAN073B:00"; "" for none (the input devices then appear under
	 * /devices/virtual).  The HID device itself is
	 * "<parent_path>/<dev_name>". */
	char parent_path[192];
	const struct hid_ll_driver *ll_driver;
	void *driver_data; // the transport's own pointer
	/* HID_QUIRK_* the transport knows the device needs (a machine's
	 * DMI quirk table, say); OR-ed into every quirk lookup. */
	unsigned long initial_quirks;

	// ---- set by the core ----
	unsigned int id; // 1, 2, ... in the order devices were added
	char dev_name[32]; // "0018:04F3:3169.0001"
	char devpath[256]; // parent_path + "/" + dev_name, input S: lines
	unsigned long quirks; // HID_QUIRK_*
	unsigned int claimed; // HID_CLAIMED_*
	unsigned int status; // HID_STAT_*
	struct hid_driver *driver; // bound driver (NULL: none)
	void *drvdata; // the HID driver's (hid_set_drvdata)

	/* The descriptor as read from the device (owned copy) and as parsed
	 * (after the driver's report_fixup; owned copy). */
	uint8_t *dev_rdesc;
	unsigned int dev_rsize;
	uint8_t *rdesc;
	unsigned int rsize;

	struct hid_collection *collection; // maxcollection used entries
	unsigned int collection_size; // allocated entries
	unsigned int maxcollection;
	unsigned int maxapplication; // application collections seen
	struct hid_report_enum report_enum[HID_REPORT_TYPES];
	unsigned int total_usages; // parse budget (HID_MAX_TOTAL_USAGES)

	/* Zero-padding buffer for short reports (driver_input_lock). */
	uint8_t *pad_buf;
	unsigned int pad_len;

	// ---- hid-input ----
	struct list_head inputs; // struct hid_input
	struct work_struct led_work; // sends LED output reports

	// ---- locks and lifetime ----
	struct mutex driver_input_lock;
	bool io_started; // driver released reports early in probe
	struct mutex ll_open_lock;
	unsigned int ll_open_count;
	refcount_t ref;
	struct list_head node; // all added devices (hid_bus_lock)
	/* Rate limit of the short-report message. */
	uint64_t short_warn_ns;
	unsigned int short_warn_burst;
};

static inline void *hid_get_drvdata(struct hid_device *hdev)
{
	return hdev->drvdata;
}

static inline void hid_set_drvdata(struct hid_device *hdev, void *data)
{
	hdev->drvdata = data;
}

/* Bytes of the report on the wire, ID byte included. */
static inline unsigned int hid_report_len(const struct hid_report *report)
{
	return ((report->size + 7) >> 3) + (report->id > 0);
}

// ---------------------------------------------------------------------------
// Transport API
// ---------------------------------------------------------------------------

/* Register the built-in HID drivers (hid-multitouch, generic).  Process
 * context; idempotent. */
void hid_core_init(void);

/* A zeroed device holding one reference, or NULL.  Process context. */
struct hid_device *hid_allocate_device(void);
/* Read the descriptor (ll_driver->parse), find the device group, name the
 * device, bind a driver.  0 even when no driver wanted the device (it stays
 * added, unbound); -errno when it cannot be added at all (the transport
 * then calls hid_destroy_device()).  Process context. */
int hid_add_device(struct hid_device *hdev);
/* Unbind, remove and free the device.  Process context.  The transport
 * must not touch `hdev' afterwards. */
void hid_destroy_device(struct hid_device *hdev);
/* Called from ll_driver->parse(): keep a copy of the descriptor. */
int hid_parse_report(struct hid_device *hdev, const uint8_t *start,
		     unsigned int size);
/* Deliver a report (see the contract at the top). */
int hid_input_report(struct hid_device *hdev, int type, uint8_t *data,
		     uint32_t size, int interrupt);

/* Power management, called by the transport around suspend/resume. */
int hid_driver_suspend(struct hid_device *hdev, int pm_event);
int hid_driver_resume(struct hid_device *hdev);
int hid_driver_reset_resume(struct hid_device *hdev);

/* Extra references for code that keeps a device pointer. */
void hid_get_device(struct hid_device *hdev);
void hid_put_device(struct hid_device *hdev);

// ---------------------------------------------------------------------------
// Driver API
// ---------------------------------------------------------------------------
int hid_register_driver(struct hid_driver *drv);
void hid_unregister_driver(struct hid_driver *drv);

/* Parse the descriptor (once per binding). */
int hid_open_report(struct hid_device *hdev);
static inline int hid_parse(struct hid_device *hdev)
{
	return hid_open_report(hdev);
}

int hid_hw_start(struct hid_device *hdev, unsigned int connect_mask);
void hid_hw_stop(struct hid_device *hdev);
int hid_hw_open(struct hid_device *hdev);
void hid_hw_close(struct hid_device *hdev);
int hid_hw_power(struct hid_device *hdev, int level);
int hid_hw_raw_request(struct hid_device *hdev, unsigned char reportnum,
		       uint8_t *buf, size_t len, unsigned char rtype,
		       int reqtype);
int hid_hw_output_report(struct hid_device *hdev, uint8_t *buf, size_t len);
/* SET_REPORT the report's current field values / GET_REPORT it and
 * process the answer like an incoming report. */
void hid_hw_request(struct hid_device *hdev, struct hid_report *report,
		    int reqtype);
int __hid_request(struct hid_device *hdev, struct hid_report *report,
		  int reqtype);
int hid_hw_wait(struct hid_device *hdev);
int hid_hw_idle(struct hid_device *hdev, int report, int idle, int reqtype);
bool hid_hw_may_wakeup(struct hid_device *hdev);

/* Let reports through before probe() returns / hold them off again. */
void hid_device_io_start(struct hid_device *hdev);
void hid_device_io_stop(struct hid_device *hdev);

int hid_connect(struct hid_device *hdev, unsigned int connect_mask);
void hid_disconnect(struct hid_device *hdev);

/* A zeroed buffer for the report: hid_report_len() + 7 + 1 bytes (room
 * for the ID byte of an unnumbered report and for whole-word packing). */
uint8_t *hid_alloc_report_buf(struct hid_report *report);
/* Pack the report's current field values into `data' (ID byte first
 * when the report is numbered). */
void hid_output_report(struct hid_report *report, uint8_t *data);
/* Set value `offset' of a field (range-checked).  0 or -1. */
int hid_set_field(struct hid_field *field, unsigned int offset,
		  int32_t value);
/* Parse report bytes as if received (feature report answers).  The caller
 * holds hid->driver_input_lock (probe paths and hid_input_report do). */
int hid_report_raw_event(struct hid_device *hdev, int type, uint8_t *data,
			 uint32_t size, int interrupt);
/* The report `id' of `type' (id 0: the first one) if it has a field
 * `field_index' with at least `report_counts' values; NULL otherwise. */
struct hid_report *hid_validate_values(struct hid_device *hdev,
				       unsigned int type, unsigned int id,
				       unsigned int field_index,
				       unsigned int report_counts);
/* The first field of a `report_type' report in application `application'
 * that carries `usage'. */
struct hid_field *hid_find_field(struct hid_device *hdev,
				 unsigned int report_type,
				 unsigned int application, unsigned int usage);

/* Bit extraction / sign handling, exposed for drivers and tests. */
uint32_t hid_field_extract(const struct hid_device *hdev, const uint8_t *report,
			   unsigned int offset, unsigned int n);
int32_t hid_snto32(uint32_t value, unsigned int n);

const struct hid_device_id *hid_match_id(const struct hid_device *hdev,
					 const struct hid_device_id *id);
bool hid_match_one_id(const struct hid_device *hdev,
		      const struct hid_device_id *id);

/* The device's quirks from the static table (hid-quirks.c). */
unsigned long hid_lookup_quirk(const struct hid_device *hdev);
/* Devices no HID driver should bind (hid-quirks.c). */
bool hid_ignore(struct hid_device *hdev);

/* The built-in drivers (registered by hid_core_init). */
extern struct hid_driver hid_generic_driver;
int hid_multitouch_init(void);
void hid_multitouch_exit(void);

// ---------------------------------------------------------------------------
// hid-input (hid-input.c)
// ---------------------------------------------------------------------------
int hidinput_connect(struct hid_device *hdev, unsigned int force);
void hidinput_disconnect(struct hid_device *hdev);
void hidinput_hid_event(struct hid_device *hdev, struct hid_field *field,
			struct hid_usage *usage, int32_t value);
void hidinput_report_event(struct hid_device *hdev, struct hid_report *report);
/* Resolution of an absolute axis in units/mm (units/radian for angles)
 * from the field's unit, exponent and ranges; 0 when unknown. */
int32_t hidinput_calc_abs_res(const struct hid_field *field, uint16_t code);
/* Replace the name of an input device hid-input allocated (drivers'
 * input_configured).  0 or -ENOMEM. */
int hidinput_set_name(struct hid_input *hi, const char *base,
		      const char *suffix);
/* Initial values of every Resolution Multiplier feature (parser). */
void hid_setup_resolution_multiplier(struct hid_device *hdev);

/* Map `usage' to (type, code) on the input device of `hidinput' and point
 * *bit / *max at the matching capability bitmap (NULL when the code does
 * not exist).  For drivers' input_mapping hooks. */
void hid_map_usage(struct hid_input *hidinput, struct hid_usage *usage,
		   unsigned long **bit, int *max, uint8_t type,
		   unsigned int c);
/* The same, and clear the code bit (so a duplicate check does not trip). */
void hid_map_usage_clear(struct hid_input *hidinput, struct hid_usage *usage,
			 unsigned long **bit, int *max, uint8_t type,
			 unsigned int c);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
/* "<driver> <dev_name>: message" (the device's name before it is added). */
void hid_printk(const struct hid_device *hdev, const char *level,
		const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define hid_err(hdev, fmt, ...) hid_printk(hdev, "error: ", fmt, ##__VA_ARGS__)
#define hid_warn(hdev, fmt, ...) \
	hid_printk(hdev, "warning: ", fmt, ##__VA_ARGS__)
#define hid_info(hdev, fmt, ...) hid_printk(hdev, "", fmt, ##__VA_ARGS__)
#ifdef HID_DEBUG
#define hid_dbg(hdev, fmt, ...) hid_printk(hdev, "debug: ", fmt, ##__VA_ARGS__)
#else
#define hid_dbg(hdev, fmt, ...)                       \
	do {                                          \
		if (0)                                \
			hid_printk(hdev, "", fmt, ##__VA_ARGS__); \
	} while (0)
#endif

#endif // _KERNEL_DEV_HID_CORE_HID_H_
