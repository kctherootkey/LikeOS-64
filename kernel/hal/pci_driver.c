// LikeOS - the PCI driver model: registration, ID matching,
// probe/remove/shutdown and device removal
//
// A struct pci_driver is bound to a device when its ID table matches and
// probe() returns 0 (a positive return is treated as success with a
// warning, a negative one leaves the device unbound).  Binding happens
// for the devices present when the driver registers, and for every device
// that appears later (VMD domains).  remove() runs on
// unregistration and when the device goes away; shutdown() of every bound
// device runs, in reverse probe order, on system shutdown.
//
// Locks: g_pci_rescan_mutex serialises topology changes and driver
// (un)registration; every driver callback runs under the device's own
// lock (pci_dev_lock), so a remove() never overlaps the device's probe(),
// error handler or a reset.  Order: rescan mutex -> device lock.
//
// The drivers that predate this model find their devices themselves
// (pci_get_devices) and are not bound here; a new driver must not claim
// one of theirs (their ID tables do not overlap).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/hal/pci.h>
#include <kernel/hal/pci_regs.h>
#include <kernel/hal/pci_internal.h>
#include <kernel/fs/sysfs.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>
#include <kernel/fs/devevent.h>

static struct pci_driver *g_pci_drivers; /* registration order */
static int g_probe_seq;

static int pci_id_is_terminator(const struct pci_device_id *id)
{
	return !id->vendor && !id->device && !id->subvendor &&
	       !id->subdevice && !id->class_mask;
}

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
					 const pci_device_t *dev)
{
	if (!ids || !dev)
		return NULL;
	for (; !pci_id_is_terminator(ids); ids++) {
		if (ids->vendor != PCI_ANY_ID && ids->vendor != dev->vendor_id)
			continue;
		if (ids->device != PCI_ANY_ID && ids->device != dev->device_id)
			continue;
		if (ids->subvendor != PCI_ANY_ID &&
		    ids->subvendor != dev->subsystem_vendor)
			continue;
		if (ids->subdevice != PCI_ANY_ID &&
		    ids->subdevice != dev->subsystem_device)
			continue;
		if ((ids->class ^ dev->class_full) & ids->class_mask)
			continue;
		return ids;
	}
	return NULL;
}

/* Probe `drv' on `dev' (rescan mutex held, device unbound). */
static int pci_device_probe(struct pci_driver *drv, pci_device_t *dev)
{
	const struct pci_device_id *id;
	int rc;

	lockdep_assert_held(&g_pci_rescan_mutex);
	if (dev->driver || dev->is_removed || pci_dev_is_disconnected(dev))
		return -ENODEV;
	id = pci_match_id(drv->id_table, dev);
	if (!id || !drv->probe)
		return -ENODEV;
	pci_dev_lock(dev);
	/* Boot-time functions are sized here, before the driver owns them
	 * (the boot enumeration never sizes). */
	pci_bars_ensure(dev);
	dev->driver = drv;
	rc = drv->probe(dev, id);
	if (rc < 0) {
		dev->driver = NULL;
		dev->driver_data = NULL;
	} else {
		if (rc > 0) {
			pci_warn(dev, "driver %s: probe returned %d, taken as success\n",
				 drv->name ? drv->name : "?", rc);
			rc = 0;
		}
		pci_priv(dev)->probe_order = ++g_probe_seq;
	}
	pci_dev_unlock(dev);
	if (rc == 0) {
		pci_info(dev, "bound to %s\n", drv->name ? drv->name : "?");
		/* Every function has its /sys/bus/pci directory (boot-time
		 * ones from sysfs_init, later ones from the scan). */
		if (drv->name)
			(void)sysfs_pci_set_driver(dev, drv->name);
	}
	return rc;
}

/* Call remove() (rescan mutex held). */
static void pci_device_remove_locked(pci_device_t *dev)
{
	struct pci_driver *drv;

	lockdep_assert_held(&g_pci_rescan_mutex);
	pci_dev_lock(dev);
	drv = dev->driver;
	if (drv) {
		if (drv->remove)
			drv->remove(dev);
		/* The device keeps the power state it was left in, but its
		 * driver-level state is gone. */
		dev->driver = NULL;
		dev->driver_data = NULL;
		pci_priv(dev)->probe_order = 0;
		if (dev->irq_type)
			pci_warn(dev, "driver %s left interrupt vectors allocated\n",
				 drv->name ? drv->name : "?");
	}
	pci_dev_unlock(dev);
}

void pci_device_release_driver(pci_device_t *dev)
{
	int held;

	might_sleep();
	if (!dev)
		return;
	held = mutex_held(&g_pci_rescan_mutex);
	if (!held)
		mutex_lock(&g_pci_rescan_mutex);
	pci_device_remove_locked(dev);
	if (!held)
		mutex_unlock(&g_pci_rescan_mutex);
}

/* Publish a device event (/dev/devevents) for a PCI function, named
 * "<segment>:<bus>:<device>.<function>".  Any context. */
static void pci_devevent(const pci_device_t *dev, uint32_t type)
{
	char name[32];

	ksnprintf(name, sizeof(name), "%04x:%02x:%02x.%x", dev->segment,
		  dev->bus, dev->device, dev->function);
	devevent_publish(type, DEVICE_CLASS_PCI,
			 ((uint64_t)dev->segment << 16) |
				 ((uint64_t)dev->bus << 8) |
				 ((uint64_t)(dev->device & 0x1f) << 3) |
				 (dev->function & 7),
			 0, 0, dev->vendor_id, dev->device_id, 0, 0, name);
}

static void pci_bus_add_device_locked(pci_device_t *dev)
{
	lockdep_assert_held(&g_pci_rescan_mutex);
	for (struct pci_driver *drv = g_pci_drivers; drv; drv = drv->next) {
		if (pci_device_probe(drv, dev) == 0)
			return;
	}
}

void pci_bus_add_device(pci_device_t *dev)
{
	int held;

	might_sleep();
	if (!dev)
		return;
	held = mutex_held(&g_pci_rescan_mutex);
	if (!held)
		mutex_lock(&g_pci_rescan_mutex);
	pci_devevent(dev, DEVICE_EVENT_ADD);
	pci_bus_add_device_locked(dev);
	if (!held)
		mutex_unlock(&g_pci_rescan_mutex);
}

int pci_register_driver(struct pci_driver *drv)
{
	struct pci_driver **pp;
	pci_device_t *devs;
	int count = 0;

	might_sleep();
	if (!drv || !drv->probe || !drv->id_table)
		return -EINVAL;
	mutex_lock(&g_pci_rescan_mutex);
	for (pp = &g_pci_drivers; *pp; pp = &(*pp)->next) {
		if (*pp == drv) {
			mutex_unlock(&g_pci_rescan_mutex);
			return -EBUSY;
		}
	}
	drv->next = NULL;
	*pp = drv;
	devs = pci_dev_table(&count);
	for (int i = 0; i < count; i++)
		(void)pci_device_probe(drv, &devs[i]);
	mutex_unlock(&g_pci_rescan_mutex);
	return 0;
}

void pci_unregister_driver(struct pci_driver *drv)
{
	struct pci_driver **pp;
	pci_device_t *devs;
	int count = 0;

	might_sleep();
	if (!drv)
		return;
	mutex_lock(&g_pci_rescan_mutex);
	devs = pci_dev_table(&count);
	/* Reverse table order: functions after the bridges above them were
	 * found, so children go first. */
	for (int i = count - 1; i >= 0; i--)
		if (devs[i].driver == drv)
			pci_device_remove_locked(&devs[i]);
	for (pp = &g_pci_drivers; *pp; pp = &(*pp)->next) {
		if (*pp == drv) {
			*pp = drv->next;
			break;
		}
	}
	drv->next = NULL;
	mutex_unlock(&g_pci_rescan_mutex);
}

void pci_shutdown_drivers(void)
{
	pci_device_t *devs;
	int count = 0;

	might_sleep();
	mutex_lock(&g_pci_rescan_mutex);
	devs = pci_dev_table(&count);
	/* Reverse probe order: the last bound goes first. */
	for (;;) {
		pci_device_t *last = NULL;
		int best = 0;

		for (int i = 0; i < count; i++) {
			struct pci_dev_priv *pp = pci_priv(&devs[i]);

			if (devs[i].driver && pp && pp->probe_order > best) {
				best = pp->probe_order;
				last = &devs[i];
			}
		}
		if (!last)
			break;
		pci_dev_lock(last);
		if (last->driver && last->driver->shutdown)
			last->driver->shutdown(last);
		pci_priv(last)->probe_order = 0; /* done (stays bound) */
		pci_dev_unlock(last);
	}
	mutex_unlock(&g_pci_rescan_mutex);
}

/* ==========================================================================
 * Walking, removal, rescan
 * ========================================================================== */

static int pci_walk_one(pci_device_t *dev, int post_order, int depth,
			int (*fn)(pci_device_t *dev, void *data), void *data)
{
	pci_device_t *kids[64];
	int n, rc;

	if (depth > 32)
		return 0;
	if (!post_order) {
		rc = fn(dev, data);
		if (rc)
			return rc;
	}
	if (pci_is_bridge(dev)) {
		n = pci_bridge_children(dev, kids, 64);
		for (int i = 0; i < n; i++) {
			rc = pci_walk_one(kids[i], post_order, depth + 1, fn,
					  data);
			if (rc)
				return rc;
		}
	}
	if (post_order)
		return fn(dev, data);
	return 0;
}

void pci_walk_bridge(pci_device_t *bridge, int self, int post_order,
		     int (*fn)(pci_device_t *dev, void *data), void *data)
{
	pci_device_t *kids[64];
	int n;

	if (!bridge || !fn)
		return;
	if (self) {
		(void)pci_walk_one(bridge, post_order, 0, fn, data);
		return;
	}
	n = pci_bridge_children(bridge, kids, 64);
	for (int i = 0; i < n; i++)
		if (pci_walk_one(kids[i], post_order, 1, fn, data))
			return;
}

static int pci_remove_one(pci_device_t *dev, void *data)
{
	(void)data;
	pci_device_remove_locked(dev);
	pci_msi_dev_removed(dev);
	pci_info(dev, "removed\n");
	pci_devevent(dev, DEVICE_EVENT_REMOVE);
	pci_dev_forget(dev);
	return 0;
}

void pci_stop_and_remove_bus_device(pci_device_t *dev)
{
	int held;

	might_sleep();
	if (!dev || dev->is_removed)
		return;
	held = mutex_held(&g_pci_rescan_mutex);
	if (!held)
		mutex_lock(&g_pci_rescan_mutex);
	/* Removed while this waited for the mutex (by whoever held it). */
	if (__atomic_load_n(&dev->is_removed, __ATOMIC_ACQUIRE))
		goto out;
	/* Children first (post order): a bridge goes last. */
	pci_walk_bridge(dev, 1, 1, pci_remove_one, NULL);
out:
	if (!held)
		mutex_unlock(&g_pci_rescan_mutex);
}
