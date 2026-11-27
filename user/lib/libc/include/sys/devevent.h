// LikeOS -- <sys/devevent.h>: the record read() returns from /dev/devevents.
//
// The kernel publishes one record whenever a device appears, goes away or
// changes (a disk and each of its partitions, a USB device, a USB input
// device, a PCI function).  Userspace -- the device manager, devd -- reads
// them to keep its own list of devices current; deciding what to do about a
// device (mounting a USB stick, for one) is userspace's business, never the
// driver's.
//
// Reading: open /dev/devevents (root only) and read() whole records; a buffer
// smaller than one record gets EINVAL.  A new descriptor starts at the next
// event published after the open, so the usual pattern is: open, enumerate
// what exists (/sys/block ...), then read -- an event for something already
// enumerated is harmless.  poll() reports POLLIN while records are queued;
// O_NONBLOCK gives EAGAIN when none are.  A reader that falls further behind
// than the kernel keeps gets one DEVICE_EVENT_OVERFLOW record (seq is the
// first sequence number it missed) and continues with the oldest record still
// kept: it should enumerate again.
//
// The userspace copy of <kernel/uapi/devevent.h>; keep both in step.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _SYS_DEVEVENT_H
#define _SYS_DEVEVENT_H

#include <stdint.h>

#define DEVEVENT_VERSION 1

enum device_event_type {
	DEVICE_EVENT_ADD = 1,
	DEVICE_EVENT_REMOVE = 2,
	DEVICE_EVENT_CHANGE = 3,    /* medium changed, partitions re-read */
	DEVICE_EVENT_OVERFLOW = 4,  /* events were lost: enumerate again */
};

enum device_event_class {
	DEVICE_CLASS_BLOCK_DISK = 1,      /* name "sdb", "nvme0n1" */
	DEVICE_CLASS_BLOCK_PARTITION = 2, /* name "sdb1"; parent = the disk */
	DEVICE_CLASS_USB_DEVICE = 3,      /* any enumerated USB device */
	DEVICE_CLASS_USB_INPUT = 4,       /* a USB keyboard, mouse, ... */
	DEVICE_CLASS_PCI = 5,             /* name "0000:00:14.0" */
	DEVICE_CLASS_INPUT = 6,           /* an event node, name "event6" */
};

/* flags */
#define DEVICE_EVENT_F_REMOVABLE 0x0001u  /* removable medium or device */
#define DEVICE_EVENT_F_HOTPLUGGED 0x0002u /* appeared after boot */
#define DEVICE_EVENT_F_USB 0x0004u        /* reached through USB */
/* Input devices: what kind of device it is, judged from its capabilities
 * (several can be set; a touchpad with physical keys is TOUCHPAD|KEY). */
#define DEVICE_EVENT_F_INPUT_KEYBOARD 0x0100u      /* a full keyboard */
#define DEVICE_EVENT_F_INPUT_KEY 0x0200u           /* has some keys */
#define DEVICE_EVENT_F_INPUT_MOUSE 0x0400u         /* relative/absolute pointer */
#define DEVICE_EVENT_F_INPUT_TOUCHPAD 0x0800u
#define DEVICE_EVENT_F_INPUT_TOUCHSCREEN 0x1000u
#define DEVICE_EVENT_F_INPUT_TABLET 0x2000u        /* pen / stylus */
#define DEVICE_EVENT_F_INPUT_JOYSTICK 0x4000u      /* joystick, gamepad */
#define DEVICE_EVENT_F_INPUT_SWITCH 0x8000u        /* lid, tablet mode, ... */
#define DEVICE_EVENT_F_INPUT_ACCELEROMETER 0x10000u
#define DEVICE_EVENT_F_INPUT_POINTINGSTICK 0x20000u

/* device_id: block -- (major << 32) | minor, the numbers of the /dev node;
 * PCI -- (segment << 16) | (bus << 8) | (device << 3) | function;
 * USB -- (controller << 32) | (root port << 8) | slot id;
 * USB input -- the USB device's id with the interface number in bits 16..23;
 * input -- (bus type << 48) | (version << 32) | N of /dev/input/eventN, with
 * major/minor the node's numbers (13, 64 + N) and vendor/product the
 * device's input_id.
 * parent_id: a partition's disk, a USB input's USB device (same encodings);
 * 0 otherwise.
 * name: block -- "sdb", "sdb1"; USB -- "<controller+1>-<root port>", e.g.
 * "1-3"; USB input -- "keyboard" or "mouse"; PCI -- "SSSS:BB:DD.F";
 * input -- "event6" (the device's own name is EVIOCGNAME on the node). */
struct device_event {
	uint32_t size;          /* sizeof(struct device_event): 128 */
	uint32_t version;       /* DEVEVENT_VERSION */
	uint32_t type;          /* enum device_event_type */
	uint32_t device_class;  /* enum device_event_class */
	uint64_t device_id;
	uint64_t parent_id;
	uint64_t seq;           /* 1, 2, 3 ... in publication order */
	uint64_t timestamp_ns;  /* monotonic clock */
	uint32_t flags;         /* DEVICE_EVENT_F_* */
	uint16_t vendor;        /* USB/PCI vendor id, else 0 */
	uint16_t product;       /* USB/PCI product id, else 0 */
	uint32_t major;         /* block: /dev node numbers, else 0 */
	uint32_t minor;
	char name[32];          /* NUL-terminated; empty when none */
	uint8_t reserved[32];
};

_Static_assert(sizeof(struct device_event) == 128, "device_event ABI");

#endif
