// libudev - the LikeOS device-information library behind the libudev API.
//
// Input libraries (libinput above all) describe devices through the libudev
// interface: a device is a node in a tree with a system path, a subsystem, a
// device node and a set of KEY=VALUE properties -- ID_INPUT_TOUCHPAD=1,
// NAME="...", PRODUCT=18/4f3/3169/100 and so on.  LikeOS has no device
// manager database; this library computes the same answers on demand from
// what the kernel publishes:
//
//   /proc/bus/input/devices   identity, names and capability bitmaps of
//                             every input device and its event node
//   /dev/input/eventN         the node itself (number, absolute axis sizes)
//   /dev/devevents            add/remove notifications (udev_monitor)
//   /sys/...                  any other directory with a uevent file
//
// The ID_INPUT_* classification follows the rules of udev's input_id
// builtin, so a program sees the same tags it would see elsewhere.
//
// Only the subset of the interface below is provided.  Ownership follows
// libudev: *_ref/*_unref count references; strings and list entries returned
// by a getter belong to the object and live as long as it; a device returned
// by udev_device_get_parent*() belongs to its child (take a reference to keep
// it longer).  Pointer functions return NULL and set errno on failure; int
// functions return 0 or a negative errno value.
//
// Thread safety: an object must not be used from two threads at once;
// different objects are independent.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_LIBUDEV_H
#define LIKEOS_LIBUDEV_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct udev;
struct udev_list_entry;
struct udev_device;
struct udev_monitor;
struct udev_enumerate;

/* ---- library context ----------------------------------------------------- */

struct udev *udev_new(void);
struct udev *udev_ref(struct udev *udev);
struct udev *udev_unref(struct udev *udev);
void *udev_get_userdata(struct udev *udev);
void udev_set_userdata(struct udev *udev, void *userdata);

/* ---- lists ----------------------------------------------------------------- */

struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *list_entry);
struct udev_list_entry *udev_list_entry_get_by_name(struct udev_list_entry *list_entry,
						    const char *name);
const char *udev_list_entry_get_name(struct udev_list_entry *list_entry);
const char *udev_list_entry_get_value(struct udev_list_entry *list_entry);

#define udev_list_entry_foreach(list_entry, first_entry) \
	for (list_entry = first_entry; list_entry; \
	     list_entry = udev_list_entry_get_next(list_entry))

/* ---- devices --------------------------------------------------------------- */

struct udev_device *udev_device_ref(struct udev_device *udev_device);
struct udev_device *udev_device_unref(struct udev_device *udev_device);
struct udev *udev_device_get_udev(struct udev_device *udev_device);

struct udev_device *udev_device_new_from_syspath(struct udev *udev,
						 const char *syspath);
struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type,
						dev_t devnum);
struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev,
							   const char *subsystem,
							   const char *sysname);

struct udev_device *udev_device_get_parent(struct udev_device *udev_device);
struct udev_device *
udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device,
					      const char *subsystem,
					      const char *devtype);

const char *udev_device_get_devpath(struct udev_device *udev_device);
const char *udev_device_get_subsystem(struct udev_device *udev_device);
const char *udev_device_get_devtype(struct udev_device *udev_device);
const char *udev_device_get_syspath(struct udev_device *udev_device);
const char *udev_device_get_sysname(struct udev_device *udev_device);
const char *udev_device_get_sysnum(struct udev_device *udev_device);
const char *udev_device_get_devnode(struct udev_device *udev_device);
const char *udev_device_get_driver(struct udev_device *udev_device);
dev_t udev_device_get_devnum(struct udev_device *udev_device);
const char *udev_device_get_action(struct udev_device *udev_device);
unsigned long long int udev_device_get_seqnum(struct udev_device *udev_device);
unsigned long long int
udev_device_get_usec_since_initialized(struct udev_device *udev_device);
int udev_device_get_is_initialized(struct udev_device *udev_device);

struct udev_list_entry *
udev_device_get_devlinks_list_entry(struct udev_device *udev_device);
struct udev_list_entry *
udev_device_get_properties_list_entry(struct udev_device *udev_device);
struct udev_list_entry *
udev_device_get_tags_list_entry(struct udev_device *udev_device);
struct udev_list_entry *
udev_device_get_sysattr_list_entry(struct udev_device *udev_device);
const char *udev_device_get_property_value(struct udev_device *udev_device,
					   const char *key);
const char *udev_device_get_sysattr_value(struct udev_device *udev_device,
					  const char *sysattr);
int udev_device_has_tag(struct udev_device *udev_device, const char *tag);

/* ---- hotplug monitor ------------------------------------------------------- */

struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
						   const char *name);
struct udev_monitor *udev_monitor_ref(struct udev_monitor *udev_monitor);
struct udev_monitor *udev_monitor_unref(struct udev_monitor *udev_monitor);
struct udev *udev_monitor_get_udev(struct udev_monitor *udev_monitor);
int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor);
int udev_monitor_set_receive_buffer_size(struct udev_monitor *udev_monitor,
					 int size);
int udev_monitor_get_fd(struct udev_monitor *udev_monitor);
struct udev_device *udev_monitor_receive_device(struct udev_monitor *udev_monitor);
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *udev_monitor,
						    const char *subsystem,
						    const char *devtype);
int udev_monitor_filter_add_match_tag(struct udev_monitor *udev_monitor,
				      const char *tag);
int udev_monitor_filter_update(struct udev_monitor *udev_monitor);
int udev_monitor_filter_remove(struct udev_monitor *udev_monitor);

/* ---- enumeration ----------------------------------------------------------- */

struct udev_enumerate *udev_enumerate_new(struct udev *udev);
struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *udev_enumerate);
struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *udev_enumerate);
struct udev *udev_enumerate_get_udev(struct udev_enumerate *udev_enumerate);
int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate,
				       const char *subsystem);
int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *udev_enumerate,
					 const char *subsystem);
int udev_enumerate_add_match_property(struct udev_enumerate *udev_enumerate,
				      const char *property, const char *value);
int udev_enumerate_add_match_sysname(struct udev_enumerate *udev_enumerate,
				     const char *sysname);
int udev_enumerate_add_match_is_initialized(struct udev_enumerate *udev_enumerate);
int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate);
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate);

#ifdef __cplusplus
}
#endif

#endif /* LIKEOS_LIBUDEV_H */
