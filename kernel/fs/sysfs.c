// LikeOS-64 -- /sys: PCI devices and character device classes.
#include <kernel/fs/sysfs.h>
#include <kernel/fs/pseudofs.h>
#include <kernel/fs/vfs.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

static struct pfs g_sysfs;

void sysfs_pci_path(const pci_device_t *dev, char *out, size_t cap)
{
	ksnprintf(out, cap, "/sys/bus/pci/devices/0000:%02x:%02x.%x", dev->bus,
		  dev->device, dev->function);
}

static void pci_relpath(const pci_device_t *dev, char *out, size_t cap)
{
	ksnprintf(out, cap, "bus/pci/devices/0000:%02x:%02x.%x", dev->bus,
		  dev->device, dev->function);
}

/* ---- PCI attribute files ---- */

static long pci_show_hex16(struct pfs_node *n, char *buf, long cap)
{
	const pci_device_t *d = n->arg;
	uint32_t v;

	switch (n->arg2) {
	case 0:
		v = d->vendor_id;
		break;
	case 1:
		v = d->device_id;
		break;
	case 2: /* subsystem vendor */
		v = pci_cfg_read32(d->bus, d->device, d->function, 0x2C) & 0xFFFF;
		break;
	case 3: /* subsystem device */
		v = pci_cfg_read32(d->bus, d->device, d->function, 0x2C) >> 16;
		break;
	case 4: /* revision */
		return pfs_printf(buf, cap, 0, "0x%02x\n",
				  pci_cfg_read32(d->bus, d->device, d->function,
						 0x08) & 0xFF);
	case 5: /* class: base<<16 | sub<<8 | prog_if */
		return pfs_printf(buf, cap, 0, "0x%06x\n",
				  ((uint32_t)d->class_code << 16) |
					  ((uint32_t)d->subclass << 8) |
					  d->prog_if);
	default:
		v = 0;
	}
	return pfs_printf(buf, cap, 0, "0x%04x\n", v);
}

static long pci_show_config(struct pfs_node *n, char *buf, long cap)
{
	const pci_device_t *d = n->arg;

	/* The 256-byte header space, raw. */
	for (int off = 0; off < 256; off += 4) {
		uint32_t v = pci_cfg_read32(d->bus, d->device, d->function,
					    (unsigned char)off);
		for (int b = 0; b < 4; b++)
			if (off + b < cap)
				buf[off + b] = (char)((v >> (8 * b)) & 0xFF);
	}
	return 256;
}

static long pci_show_uevent(struct pfs_node *n, char *buf, long cap)
{
	const pci_device_t *d = n->arg;
	long p = 0;

	p = pfs_printf(buf, cap, p, "DRIVER=%s\n", (const char *)n->parent->arg2 ? (const char *)n->parent->arg2 : "");
	p = pfs_printf(buf, cap, p, "PCI_CLASS=%X\n",
		       ((uint32_t)d->class_code << 16) |
			       ((uint32_t)d->subclass << 8) | d->prog_if);
	p = pfs_printf(buf, cap, p, "PCI_ID=%04X:%04X\n", d->vendor_id,
		       d->device_id);
	uint32_t sub = pci_cfg_read32(d->bus, d->device, d->function, 0x2C);
	p = pfs_printf(buf, cap, p, "PCI_SUBSYS_ID=%04X:%04X\n", sub & 0xFFFF,
		       sub >> 16);
	p = pfs_printf(buf, cap, p, "PCI_SLOT_NAME=0000:%02x:%02x.%x\n", d->bus,
		       d->device, d->function);
	p = pfs_printf(buf, cap, p, "MODALIAS=pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X\n",
		       d->vendor_id, d->device_id, sub & 0xFFFF, sub >> 16,
		       d->class_code, d->subclass, d->prog_if);
	return p;
}

static long pci_show_resource(struct pfs_node *n, char *buf, long cap)
{
	const pci_device_t *d = n->arg;
	long p = 0;

	for (int i = 0; i < 6; i++) {
		uint64_t bar = d->bar[i];
		uint64_t start = 0, end = 0, flags = 0;

		if (bar & 1) {
			start = bar & ~3ULL;
			flags = 0x101; /* I/O */
		} else if (bar) {
			start = bar & ~0xFULL;
			flags = 0x200; /* memory */
			if (((bar >> 1) & 3) == 2 && i < 5)
				start |= (uint64_t)d->bar[i + 1] << 32;
		}
		/* Sizes are not recorded; report a one-page span so the line
		 * is well-formed. */
		if (start)
			end = start + 0xFFF;
		p = pfs_printf(buf, cap, p, "0x%016llx 0x%016llx 0x%016llx\n",
			       (unsigned long long)start,
			       (unsigned long long)end,
			       (unsigned long long)flags);
	}
	return p;
}

static void sysfs_add_pci(const pci_device_t *d)
{
	char base[64], path[96];

	pci_relpath(d, base, sizeof(base));
	struct pfs_node *dir = pfs_mkdir(&g_sysfs, base);
	if (!dir)
		return;
	static const char *names[] = { "vendor", "device", "subsystem_vendor",
				       "subsystem_device", "revision", "class" };
	for (int i = 0; i < 6; i++) {
		ksnprintf(path, sizeof(path), "%s/%s", base, names[i]);
		pfs_add_file(&g_sysfs, path, pci_show_hex16, (void *)d, (uint64_t)i);
	}
	ksnprintf(path, sizeof(path), "%s/config", base);
	pfs_add_file(&g_sysfs, path, pci_show_config, (void *)d, 0);
	ksnprintf(path, sizeof(path), "%s/uevent", base);
	pfs_add_file(&g_sysfs, path, pci_show_uevent, (void *)d, 0);
	ksnprintf(path, sizeof(path), "%s/resource", base);
	pfs_add_file(&g_sysfs, path, pci_show_resource, (void *)d, 0);
	ksnprintf(path, sizeof(path), "%s/subsystem", base);
	pfs_add_link(&g_sysfs, path, "../../../bus/pci");
	dir->arg = (void *)d;
}

/* ---- character devices ---- */

static long chr_show_uevent(struct pfs_node *n, char *buf, long cap)
{
	long p = 0;

	p = pfs_printf(buf, cap, p, "MAJOR=%u\n", (unsigned)(n->arg2 >> 20));
	p = pfs_printf(buf, cap, p, "MINOR=%u\n", (unsigned)(n->arg2 & 0xFFFFF));
	p = pfs_printf(buf, cap, p, "DEVNAME=%s\n", (const char *)n->arg);
	return p;
}

static long chr_show_dev(struct pfs_node *n, char *buf, long cap)
{
	return pfs_printf(buf, cap, 0, "%u:%u\n", (unsigned)(n->arg2 >> 20),
			  (unsigned)(n->arg2 & 0xFFFFF));
}

int sysfs_add_char_device(const char *name, uint32_t major, uint32_t minor,
			  const char *class, const pci_device_t *pci)
{
	char base[64], path[128], target[128];
	const char *bn = name;

	for (const char *q = name; *q; q++)
		if (*q == '/')
			bn = q + 1;

	/* The device's own directory lives with its PCI parent (or under
	 * /sys/devices/virtual when it has none); everything else links to
	 * it, which is how the consumers walk it: realpath() of
	 * /sys/dev/char/M:m/device must end in the PCI slot name. */
	if (pci) {
		char pcirel[64];
		pci_relpath(pci, pcirel, sizeof(pcirel));
		ksnprintf(base, sizeof(base), "%s/%s/%s", pcirel, class, bn);
	} else {
		ksnprintf(base, sizeof(base), "devices/virtual/%s/%s", class, bn);
	}
	if (!pfs_mkdir(&g_sysfs, base))
		return -ENOMEM;

	char *devname = kalloc(64);
	if (!devname)
		return -ENOMEM;
	ksnprintf(devname, 64, "%s", name);
	uint64_t devnum = ((uint64_t)major << 20) | minor;

	ksnprintf(path, sizeof(path), "%s/uevent", base);
	pfs_add_file(&g_sysfs, path, chr_show_uevent, devname, devnum);
	ksnprintf(path, sizeof(path), "%s/dev", base);
	pfs_add_file(&g_sysfs, path, chr_show_dev, devname, devnum);
	if (pci) {
		ksnprintf(path, sizeof(path), "%s/device", base);
		pfs_add_link(&g_sysfs, path, "../..");
	}
	ksnprintf(path, sizeof(path), "%s/subsystem", base);
	ksnprintf(target, sizeof(target), "/sys/class/%s", class);
	pfs_add_link(&g_sysfs, path, target);

	/* /sys/dev/char/MAJ:MIN -> the directory */
	ksnprintf(path, sizeof(path), "dev/char/%u:%u", major, minor);
	ksnprintf(target, sizeof(target), "/sys/%s", base);
	pfs_add_link(&g_sysfs, path, target);
	/* /sys/class/<class>/<name> -> the directory */
	ksnprintf(path, sizeof(path), "class/%s/%s", class, bn);
	pfs_add_link(&g_sysfs, path, target);
	return 0;
}

void sysfs_init(void)
{
	int count = 0;
	const pci_device_t *devs = pci_get_devices(&count);

	pfs_init(&g_sysfs, "/sys");
	pfs_mkdir(&g_sysfs, "bus/pci/devices");
	pfs_mkdir(&g_sysfs, "dev/char");
	pfs_mkdir(&g_sysfs, "class");
	pfs_mkdir(&g_sysfs, "devices/virtual");
	for (int i = 0; i < count; i++)
		sysfs_add_pci(&devs[i]);
	vfs_register_mount("/sys", pfs_ops(&g_sysfs));
}
