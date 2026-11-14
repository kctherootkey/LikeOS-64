// LikeOS -- /sys: PCI devices and character device classes.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/sysfs.h>
#include <kernel/fs/pseudofs.h>
#include <kernel/fs/vfs.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/bug.h>

void blk_sysfs_init(void); /* kernel/dev/block/blk_sysfs.c */

static struct pfs g_sysfs;

static struct pfs_node *g_sysfs_pci_dir; /* bus/pci/devices, once set up */

/* "SSSS:BB:DD.F": the segment (domain) is part of the name -- a VMD
 * domain's functions repeat the bus numbers of segment 0. */
void sysfs_pci_path(const pci_device_t *dev, char *out, size_t cap)
{
	ksnprintf(out, cap, "/sys/bus/pci/devices/%04x:%02x:%02x.%x",
		  dev->segment, dev->bus, dev->device, dev->function);
}

static void pci_relpath(const pci_device_t *dev, char *out, size_t cap)
{
	ksnprintf(out, cap, "bus/pci/devices/%04x:%02x:%02x.%x", dev->segment,
		  dev->bus, dev->device, dev->function);
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
		v = pci_cfg_read32_dev(d, 0x2C) & 0xFFFF;
		break;
	case 3: /* subsystem device */
		v = pci_cfg_read32_dev(d, 0x2C) >> 16;
		break;
	case 4: /* revision */
		return pfs_printf(buf, cap, 0, "0x%02x\n",
				  pci_cfg_read32_dev(d, 0x08) & 0xFF);
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
		uint32_t v = pci_cfg_read32_dev(d, (unsigned char)off);
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
	uint32_t sub = pci_cfg_read32_dev(d, 0x2C);
	p = pfs_printf(buf, cap, p, "PCI_SUBSYS_ID=%04X:%04X\n", sub & 0xFFFF,
		       sub >> 16);
	p = pfs_printf(buf, cap, p, "PCI_SLOT_NAME=%04x:%02x:%02x.%x\n",
		       d->segment, d->bus, d->device, d->function);
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

void sysfs_add_pci(const pci_device_t *d)
{
	char base[64], path[96];

	/* Before sysfs_init the boot-time walk below adds it. */
	if (!d || !__atomic_load_n(&g_sysfs_pci_dir, __ATOMIC_ACQUIRE))
		return;

	pci_relpath(d, base, sizeof(base));
	/* The directory carries its function from the moment it is visible
	 * (published complete, see pfs_add_node). */
	struct pfs_node dtmpl;

	mm_memset(&dtmpl, 0, sizeof(dtmpl));
	dtmpl.arg = (void *)d;
	struct pfs_node *dir = pfs_add_node(&g_sysfs, base, PFS_DIR, &dtmpl);
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
}

int sysfs_pci_set_driver(const pci_device_t *dev, const char *name)
{
	char base[64], path[128], target[128];

	if (!dev || !name)
		return -EINVAL;
	pci_relpath(dev, base, sizeof(base));
	struct pfs_node *dir = pfs_lookup(&g_sysfs, base);
	if (!dir)
		return -ENOENT;
	dir->arg2 = (uint64_t)(uintptr_t)name;
	ksnprintf(path, sizeof(path), "bus/pci/drivers/%s", name);
	if (!pfs_mkdir(&g_sysfs, path))
		return -ENOMEM;
	ksnprintf(path, sizeof(path), "bus/pci/drivers/%s/%04x:%02x:%02x.%x",
		  name, dev->segment, dev->bus, dev->device, dev->function);
	ksnprintf(target, sizeof(target), "/sys/%s", base);
	pfs_add_link(&g_sysfs, path, target);
	ksnprintf(path, sizeof(path), "%s/driver", base);
	ksnprintf(target, sizeof(target), "../../../bus/pci/drivers/%s", name);
	pfs_add_link(&g_sysfs, path, target);
	return 0;
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

	/* Static nodes are never removed: a device registered again under
	 * the same name (a hot re-plug) gets the same nodes back, refreshed,
	 * and keeps its name buffer. */
	char *devname = NULL;
	ksnprintf(path, sizeof(path), "%s/uevent", base);
	struct pfs_node *old = pfs_lookup(&g_sysfs, path);
	if (old) {
		if (!old->transient && old->show == chr_show_uevent)
			devname = (char *)old->arg;
		pfs_node_put(old);
	}
	if (!devname) {
		devname = kalloc(64);
		if (!devname)
			return -ENOMEM;
	}
	ksnprintf(devname, 64, "%s", name);
	uint64_t devnum = ((uint64_t)major << 20) | minor;

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

int sysfs_add_class_device(const char *class, const char *name,
			   const pci_device_t *pci, char *base, size_t cap)
{
	char path[128], target[128];

	if (pci) {
		char pcirel[64];
		pci_relpath(pci, pcirel, sizeof(pcirel));
		ksnprintf(base, cap, "%s/%s/%s", pcirel, class, name);
	} else {
		ksnprintf(base, cap, "devices/virtual/%s/%s", class, name);
	}
	if (!pfs_mkdir(&g_sysfs, base))
		return -ENOMEM;
	if (pci) {
		ksnprintf(path, sizeof(path), "%s/device", base);
		pfs_add_link(&g_sysfs, path, "../..");
	}
	ksnprintf(path, sizeof(path), "%s/subsystem", base);
	ksnprintf(target, sizeof(target), "/sys/class/%s", class);
	pfs_add_link(&g_sysfs, path, target);
	ksnprintf(path, sizeof(path), "class/%s", class);
	pfs_mkdir(&g_sysfs, path);
	ksnprintf(path, sizeof(path), "class/%s/%s", class, name);
	ksnprintf(target, sizeof(target), "/sys/%s", base);
	pfs_add_link(&g_sysfs, path, target);
	return 0;
}

/* ---- /sys/class/hwmon ---- */

/*
 * The class directory of hardware monitors: hwmon<N> for every monitor a
 * driver registered, each a link to the monitor's own directory (which
 * the driver serves, e.g. under its device).  N comes from one number
 * space for all drivers, lowest free first, as tools expect.  Dynamic:
 * a monitor that goes away disappears from the listing at once.
 */
#define SYSFS_HWMON_MAX 128
#define SYSFS_HWMON_TARGET 112
static char g_hwmon_target[SYSFS_HWMON_MAX][SYSFS_HWMON_TARGET];
static uint8_t g_hwmon_used[SYSFS_HWMON_MAX];
static spinlock_t g_hwmon_lock = SPINLOCK_INIT("sysfs_hwmon");

int sysfs_hwmon_register(const char *parent)
{
	uint64_t fl;
	int id = -ENOSPC;

	/* Room for "/hwmon<NNN>" after the parent. */
	if (!parent || parent[0] != '/' ||
	    kstrlen(parent) + 10 >= SYSFS_HWMON_TARGET)
		return -EINVAL;
	spin_lock_irqsave(&g_hwmon_lock, &fl);
	for (int i = 0; i < SYSFS_HWMON_MAX; i++) {
		if (g_hwmon_used[i])
			continue;
		ksnprintf(g_hwmon_target[i], SYSFS_HWMON_TARGET, "%s/hwmon%d",
			  parent, i);
		g_hwmon_used[i] = 1;
		id = i;
		break;
	}
	spin_unlock_irqrestore(&g_hwmon_lock, fl);
	return id;
}

void sysfs_hwmon_unregister(int id)
{
	uint64_t fl;

	if (id < 0 || id >= SYSFS_HWMON_MAX)
		return;
	spin_lock_irqsave(&g_hwmon_lock, &fl);
	WARN_ON(!g_hwmon_used[id]);
	g_hwmon_used[id] = 0;
	g_hwmon_target[id][0] = 0;
	spin_unlock_irqrestore(&g_hwmon_lock, fl);
}

static int sysfs_hwmon_list(struct pfs_node *dir, unsigned *cursor,
			    char *name, long cap, int *type)
{
	uint64_t fl;
	int found = 0;

	(void)dir;
	spin_lock_irqsave(&g_hwmon_lock, &fl);
	for (unsigned i = *cursor; i < SYSFS_HWMON_MAX; i++) {
		if (!g_hwmon_used[i])
			continue;
		ksnprintf(name, (size_t)cap, "hwmon%u", i);
		*type = DT_LNK;
		*cursor = i + 1;
		found = 1;
		break;
	}
	spin_unlock_irqrestore(&g_hwmon_lock, fl);
	return found;
}

static struct pfs_node *sysfs_hwmon_lookup(struct pfs_node *dir,
					   const char *name)
{
	char target[SYSFS_HWMON_TARGET];
	struct pfs_node *n;
	unsigned id = 0;
	const char *p;
	uint64_t fl;

	if (kstrncmp(name, "hwmon", 5) != 0)
		return NULL;
	p = name + 5;
	if (*p < '0' || *p > '9' || (*p == '0' && p[1]))
		return NULL;
	while (*p >= '0' && *p <= '9') {
		id = id * 10 + (unsigned)(*p++ - '0');
		if (id >= SYSFS_HWMON_MAX)
			return NULL;
	}
	if (*p)
		return NULL;
	target[0] = 0;
	spin_lock_irqsave(&g_hwmon_lock, &fl);
	if (g_hwmon_used[id])
		ksnprintf(target, sizeof(target), "%s", g_hwmon_target[id]);
	spin_unlock_irqrestore(&g_hwmon_lock, fl);
	if (!target[0])
		return NULL;
	n = pfs_node_new(dir->fs, dir, name, PFS_LINK);
	if (n)
		ksnprintf(n->link, sizeof(n->link), "%s", target);
	return n;
}

struct pfs_node *sysfs_add_attr(const char *base, const char *name,
				pfs_show_t show, pfs_store_t store, void *arg,
				uint64_t arg2)
{
	char path[192];
	ksnprintf(path, sizeof(path), "%s/%s", base, name);
	if (store)
		return pfs_add_file_rw(&g_sysfs, path, show, store, arg, arg2);
	return pfs_add_file(&g_sysfs, path, show, arg, arg2);
}

struct pfs_node *sysfs_mkdir(const char *path)
{
	return pfs_mkdir(&g_sysfs, path);
}

struct pfs_node *sysfs_add_node(const char *path, enum pfs_type type,
				const struct pfs_node *tmpl)
{
	return pfs_add_node(&g_sysfs, path, type, tmpl);
}

void sysfs_init(void)
{
	int count = 0;
	const pci_device_t *devs = pci_get_devices(&count);

	pfs_init(&g_sysfs, "/sys");
	__atomic_store_n(&g_sysfs_pci_dir, pfs_mkdir(&g_sysfs, "bus/pci/devices"),
			 __ATOMIC_RELEASE);
	/* Physical slots: none are registered (there is no hot-plug slot
	 * driver), but the directory is part of the PCI bus layout and
	 * tools list it to name a device's slot. */
	pfs_mkdir(&g_sysfs, "bus/pci/slots");
	pfs_mkdir(&g_sysfs, "dev/char");
	pfs_mkdir(&g_sysfs, "class");
	{
		/* dynamic from the moment it exists */
		struct pfs_node hw;

		mm_memset(&hw, 0, sizeof(hw));
		hw.list = sysfs_hwmon_list;
		hw.lookup = sysfs_hwmon_lookup;
		pfs_add_node(&g_sysfs, "class/hwmon", PFS_DIR, &hw);
	}
	pfs_mkdir(&g_sysfs, "devices/virtual");
	for (int i = 0; i < count; i++)
		sysfs_add_pci(&devs[i]);
	/* /sys/block, /sys/class/block, /sys/dev/block and the disk power
	 * trigger: directories the block layer fills from its registry. */
	blk_sysfs_init();
	vfs_register_mount("/sys", pfs_ops(&g_sysfs), "sysfs", &g_sysfs,
			   /* Shown as the conventional options, but not
			    * enforced: a program re-executes itself through
			    * /proc/self/exe, which lexically is below here. */
			   0, "ro,nosuid,nodev,noexec", 1);
}
