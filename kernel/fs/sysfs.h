// LikeOS -- the sysfs-style device attribute tree at /sys.
//
// What userspace device libraries read to find out what a descriptor is:
// given a character device's major:minor, /sys/dev/char/MAJ:MIN/ leads to
// the PCI device behind it (vendor, device, revision, config space) and to
// the driver's other nodes.  Built from the kernel's own PCI table at boot;
// drivers add their nodes as they register them.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_SYSFS_H
#define KERNEL_FS_SYSFS_H

#include <kernel/uapi/types.h>
#include <kernel/hal/pci.h>
#include <kernel/fs/pseudofs.h>

void sysfs_init(void);

/* Register a character device node `name' (e.g. "dri/card0", as it
 * appears under /dev) with major:minor, belonging to class `class'
 * ("drm", "input", ...) and, if it is a PCI function's, to that PCI
 * device.  Creates:
 *   /sys/dev/char/MAJ:MIN/           {uevent, device -> PCI dir, subsystem}
 *   /sys/class/<class>/<basename>    -> the same directory
 *   <PCI dir>/<class>/<basename>     -> the same directory
 */
int sysfs_add_char_device(const char *name, uint32_t major, uint32_t minor,
			  const char *class, const pci_device_t *pci);

/* A PCI function's directory /sys/bus/pci/devices/SSSS:BB:DD.F with its
 * attribute files: every function present at boot gets one from
 * sysfs_init; the PCI core calls this for functions found later (hot-plug,
 * VMD domains).  A function found again at the same address reuses its
 * directory.  No-op before sysfs_init. */
void sysfs_add_pci(const pci_device_t *d);

/* Record which driver bound a PCI function: the uevent's DRIVER= line,
 * a `driver' link in the device's directory and the device's entry under
 * /sys/bus/pci/drivers/<name>/.  `name' must outlive the call (a driver's
 * static name). */
int sysfs_pci_set_driver(const pci_device_t *dev, const char *name);

/* Path of a PCI function's directory, "/sys/bus/pci/devices/0000:00:0f.0". */
void sysfs_pci_path(const pci_device_t *dev, char *out, size_t cap);

/* A device of a class without a device node (a backlight, a thermal
 * zone): its directory under the PCI parent (or /sys/devices/virtual),
 * the /sys/class/<class>/<name> link, `device' and `subsystem' links.
 * `base' receives the directory's path relative to /sys, for
 * sysfs_add_attr(). */
int sysfs_add_class_device(const char *class, const char *name,
			   const pci_device_t *pci, char *base, size_t cap);
/* /sys/class/hwmon/hwmon<N>: register a hardware monitor whose directory
 * is <parent>/hwmon<N> (`parent' an absolute /sys path; the caller serves
 * that directory under the name it is given).  Returns N (lowest free)
 * or a negative errno; unregister with N when the monitor goes.  Any
 * context. */
int sysfs_hwmon_register(const char *parent);
void sysfs_hwmon_unregister(int id);
/* A directory under /sys (created with its parents, or the existing
 * one), for a subsystem that makes it dynamic (pfs_set_dynamic) and lists
 * its own entries -- the block layer's /sys/block. */
struct pfs_node *sysfs_mkdir(const char *path);
/* An attribute file in such a directory; `store' NULL makes it read-only. */
struct pfs_node *sysfs_add_attr(const char *base, const char *name,
				pfs_show_t show, pfs_store_t store, void *arg,
				uint64_t arg2);
/* A node of `type' at `path' (relative to /sys) carrying the fields of
 * `tmpl' from the moment it is reachable (pfs_add_node): a dynamic
 * directory with its list/lookup/create callbacks, a file with a mode of
 * its own. */
struct pfs_node *sysfs_add_node(const char *path, enum pfs_type type,
				const struct pfs_node *tmpl);

#endif
