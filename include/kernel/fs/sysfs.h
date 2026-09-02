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

/* Path of a PCI function's directory, "/sys/bus/pci/devices/0000:00:0f.0". */
void sysfs_pci_path(const pci_device_t *dev, char *out, size_t cap);

#endif
