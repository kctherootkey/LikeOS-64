// libudev - internal interfaces shared by the library's source files.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_LIBUDEV_INTERNAL_H
#define LIKEOS_LIBUDEV_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __LIKEOS__
#include <sys/input.h>
#else
#include <linux/input.h>
#endif

#include <libudev.h>

#define LUDEV_EXPORT __attribute__((visibility("default")))

// ---------------------------------------------------------------------------
// Where the kernel's information is read from.  Fixed paths on the system; a
// host test build (-DLUDEV_TESTING) can point them at fixture files instead.
// There is deliberately no run-time override in the library proper: a
// privileged program (the X server) links it, and nothing its caller's
// environment says may change what a device is taken to be.
// ---------------------------------------------------------------------------
struct ludev_paths {
	const char *proc_input_devices; // "/proc/bus/input/devices"
	const char *dev_input;          // "/dev/input"
	const char *devevents;          // "/dev/devevents"
	const char *sys;                // "/sys"
};

extern struct ludev_paths ludev_paths;

#ifdef LUDEV_TESTING
void ludev_test_set_paths(const char *proc_input_devices, const char *dev_input,
			  const char *devevents, const char *sys);
// Host tests have no character devices: when set, a /dev/input entry of any
// file type counts as a device node, numbered from its eventN name.
extern bool ludev_test_any_node_type;
// Times an event node was opened to read its axes (the size properties).
extern unsigned int ludev_test_size_opens;
#endif

// Longest string read from the kernel or the file system for one value.
#define LUDEV_VALUE_MAX 4096
// Longest system path accepted.
#define LUDEV_PATH_MAX 4096

// ---------------------------------------------------------------------------
// Lists (properties, attributes, enumeration results)
// ---------------------------------------------------------------------------
struct udev_list_entry {
	struct udev_list_entry *next;
	char *name;
	char *value; // may be NULL
};

struct ludev_list {
	struct udev_list_entry *head;
	struct udev_list_entry *tail;
};

// Set NAME to VALUE (copied; VALUE may be NULL).  An existing entry of that
// name is replaced in place when `unique' is true, otherwise appended.
// 0 or -ENOMEM.
int ludev_list_set(struct ludev_list *list, const char *name, const char *value,
		   bool unique);
struct udev_list_entry *ludev_list_find(const struct ludev_list *list,
					const char *name);
void ludev_list_clear(struct ludev_list *list);

// ---------------------------------------------------------------------------
// Input devices, as /proc/bus/input/devices describes them
// ---------------------------------------------------------------------------
#define LUDEV_LONG_BITS (8u * (unsigned int)sizeof(unsigned long))
#define LUDEV_NLONGS(n) (((n) + LUDEV_LONG_BITS - 1) / LUDEV_LONG_BITS)

enum ludev_cap {
	LUDEV_CAP_PROP,
	LUDEV_CAP_EV,
	LUDEV_CAP_KEY,
	LUDEV_CAP_REL,
	LUDEV_CAP_ABS,
	LUDEV_CAP_MSC,
	LUDEV_CAP_LED,
	LUDEV_CAP_SND,
	LUDEV_CAP_FF,
	LUDEV_CAP_SW,
	LUDEV_CAP_COUNT
};

// The largest bitmap is the key one.
struct ludev_bitmap {
	unsigned long w[LUDEV_NLONGS(KEY_CNT)];
};

struct ludev_input {
	unsigned int bustype, vendor, product, version;
	char *name;  // "ELAN073B:00 04F3:3169 Touchpad"
	char *phys;  // "i2c-ELAN073B:00", "" when none
	char *uniq;
	char *sysfs; // "/devices/.../input/input10"
	int event;   // N of /dev/input/eventN, -1 when it has none
	struct ludev_bitmap caps[LUDEV_CAP_COUNT];
	// The bitmap lines as published ("e520 10000 0 0 0 0"); NULL for a
	// type the device does not have.
	char *caps_text[LUDEV_CAP_COUNT];
};

struct ludev_input_table {
	struct ludev_input *v;
	size_t n;
};

// The name of each capability line ("PROP", "EV", "KEY" ...) and the number
// of codes of that type.
const char *ludev_cap_name(enum ludev_cap cap);
unsigned int ludev_cap_count(enum ludev_cap cap);

// Parse the text of /proc/bus/input/devices.  0 or a negative errno.
int ludev_input_parse(const char *text, size_t len, struct ludev_input_table *out);
// Read and parse ludev_paths.proc_input_devices.
int ludev_input_load(struct ludev_input_table *out);
void ludev_input_table_free(struct ludev_input_table *t);
const struct ludev_input *ludev_input_find_event(const struct ludev_input_table *t,
						 int event);
const struct ludev_input *ludev_input_find_sysfs(const struct ludev_input_table *t,
						 const char *sysfs);

bool ludev_test_bit(const struct ludev_input *in, enum ludev_cap cap,
		    unsigned int code);

// ---------------------------------------------------------------------------
// Classification (ID_INPUT_*), after udev's input_id builtin
// ---------------------------------------------------------------------------
enum {
	LUDEV_ID_INPUT = 1u << 0,
	LUDEV_ID_KEY = 1u << 1,
	LUDEV_ID_KEYBOARD = 1u << 2,
	LUDEV_ID_MOUSE = 1u << 3,
	LUDEV_ID_TOUCHPAD = 1u << 4,
	LUDEV_ID_TOUCHSCREEN = 1u << 5,
	LUDEV_ID_TABLET = 1u << 6,
	LUDEV_ID_TABLET_PAD = 1u << 7,
	LUDEV_ID_JOYSTICK = 1u << 8,
	LUDEV_ID_ACCELEROMETER = 1u << 9,
	LUDEV_ID_POINTINGSTICK = 1u << 10,
	LUDEV_ID_SWITCH = 1u << 11,
};

unsigned int ludev_input_classify(const struct ludev_input *in);

// The ID_INPUT_* property names, in the order they are published, with the
// flag each one stands for.
struct ludev_id_name {
	unsigned int flag;
	const char *property;
};
extern const struct ludev_id_name ludev_id_names[];
extern const size_t ludev_id_names_count;

// ID_BUS for a bus type, or NULL when there is no conventional name.
const char *ludev_bus_name(unsigned int bustype);

// The LIBINPUT_DEVICE_GROUP value of an input device: "bus/vid/pid:phys",
// cut at "/input" and at a '.' after the last '-'.  Writes into buf;
// false when it does not fit.
bool ludev_device_group(const struct ludev_input *in, char *buf, size_t size);

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------
enum ludev_kind {
	LUDEV_KIND_EVENT, // an input event node, /dev/input/eventN
	LUDEV_KIND_INPUT, // its input device (inputM): identity + capabilities
	LUDEV_KIND_SYSFS, // any other /sys directory, read through its uevent
};

struct udev {
	int refcount;
	void *userdata;
};

struct udev_device {
	int refcount;
	struct udev *udev;
	enum ludev_kind kind;
	char *syspath;   // "/sys/devices/.../input/input10/event6"
	char *devpath;   // syspath without the "/sys"
	char *sysname;   // "event6"
	char *sysnum;    // "6", NULL when the name ends in no digit
	char *subsystem; // "input", NULL when unknown
	char *devtype;
	char *devnode;   // "/dev/input/event6", NULL when none
	char *driver;
	char *action;    // monitor events only
	dev_t devnum;
	unsigned long long seqnum;
	struct ludev_list properties;
	struct ludev_list sysattrs;  // cache of values read so far
	struct ludev_list sysattr_names;
	bool sysattr_names_done;
	struct udev_device *parent;  // a reference held by this device
	bool parent_done;
	// What an input-kind device was made from, for its parent and sysattrs.
	struct ludev_input *input;
	unsigned int id_flags;
	// ID_INPUT_WIDTH_MM / ID_INPUT_HEIGHT_MM not looked up yet: they need
	// the event node opened, which is done only when they are asked for
	// (ludev_device_resolve_size).
	bool size_pending;
};

// The size properties of an event device, if still pending: opens the node
// once.  Called by every lookup that may return them.  0 or -ENOMEM.
int ludev_device_resolve_size(struct udev_device *d);
// Whether a property name pattern (fnmatch) can match a size property.
bool ludev_pattern_matches_size(const char *pattern);

// Build devices.  The returned device holds one reference.
struct udev_device *ludev_device_new_event(struct udev *udev,
					   const struct ludev_input *in);
struct udev_device *ludev_device_new_input(struct udev *udev,
					   const struct ludev_input *in);
// A device for an event node that is gone (a monitor "remove"): only its
// names and numbers.
struct udev_device *ludev_device_new_event_removed(struct udev *udev, int event,
						   const char *syspath,
						   dev_t devnum);
struct udev_device *ludev_device_new_sysfs(struct udev *udev, const char *syspath);

// "/sys" + sysfs + "/eventN" into buf.  false when it does not fit.
bool ludev_event_syspath(const struct ludev_input *in, char *buf, size_t size);
bool ludev_input_syspath(const struct ludev_input *in, char *buf, size_t size);

// Parse "eventN" -> N (>= 0), or -1.
int ludev_parse_event_name(const char *s);

// Small helpers.
char *ludev_strdup_bounded(const char *s);
int ludev_read_file(const char *path, char **out, size_t *len, size_t max);

#endif /* LIKEOS_LIBUDEV_INTERNAL_H */
