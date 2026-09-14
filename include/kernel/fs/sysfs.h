// LikeOS-64 -- the sysfs-style device attribute tree at /sys.
//
// What userspace device libraries read to find out what a descriptor is:
// given a character device's major:minor, /sys/dev/char/MAJ:MIN/ leads to
// the PCI device behind it (vendor, device, revision, config space) and to
// the driver's other nodes.  Built from the kernel's own PCI table at boot;
// drivers add their nodes as they register them.
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
/* An attribute file in such a directory; `store' NULL makes it read-only. */
struct pfs_node *sysfs_add_attr(const char *base, const char *name,
				pfs_show_t show, pfs_store_t store, void *arg,
				uint64_t arg2);

#endif
