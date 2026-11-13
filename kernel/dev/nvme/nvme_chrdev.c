// LikeOS - NVMe character devices /dev/nvmeX and /dev/ngXnY
//
// Three kinds of character node, as the established driver has them:
//   /dev/nvmeX   the controller (major nvme_char_major "nvme", minor =
//                controller instance): admin pass-through, reset, rescan.
//                Opens only while the controller is LIVE (-EWOULDBLOCK
//                otherwise); each open holds a controller reference.
//   /dev/ngXnY   the generic node of a namespace (major nvme_ns_chr_major
//                "nvme-generic", minor from nvme_ns_chr_minor_ida;
//                X = controller instance, Y = namespace head instance):
//                pass-through to that namespace even when it has no block
//                device (unsupported format, zoned without block support).
//                Each open holds a namespace reference.
//   /dev/ngXnY   of a multipath head (X = subsystem instance): the same
//                through the head's current path; each open holds a head
//                reference.
// Every node is 0600 root (the established default for the nvme and
// nvme-generic classes) and appears in sysfs with its device number
// (/sys/dev/char/M:m, /sys/class/<class>/<name>/{dev,uevent,device}).
//
// Removal while open: devfs keeps a pointer to the node, which is
// embedded in its object, and calls ->open after looking the node up.
// So ->open never trusts the node: it looks it up in this file's registry
// (g_cdev_lock) and takes the object reference UNDER that lock;
// nvme_cdev_del_*() marks the entry dead under the same lock before the
// node is unregistered.  An open that loses the race gets -ENXIO and
// never touches the object; an open that wins holds a reference, so the
// object (and the node in it) outlives every open file.  The generic
// node's minor goes back to the ida when the node is deleted AND its
// last open file closed, as with the established device release.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/fs/device.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/fs/sysfs.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

enum nvme_cdev_kind {
	NVME_CDEV_CTRL = 1,
	NVME_CDEV_NS = 2,
	NVME_CDEV_HEAD = 3,
};

struct nvme_cdev_entry {
	struct list_head link; /* g_cdev_list */
	struct devfs_node *node; /* the object's embedded node */
	enum nvme_cdev_kind kind;
	void *obj; /* struct nvme_ctrl / nvme_ns / nvme_ns_head */
	uint32_t major, minor;
	int opens; /* open files */
	bool dead; /* deleted: no new opens */
	bool ida_minor; /* minor from nvme_ns_chr_minor_ida */
};

static LIST_HEAD(g_cdev_list);
static spinlock_t g_cdev_lock = SPINLOCK_INIT("nvme_cdev");

static struct nvme_cdev_entry *cdev_find_node_locked(struct devfs_node *node)
{
	struct nvme_cdev_entry *e;

	/* A dead entry may share the node with a newer registration of the
	 * same object: only the live one counts. */
	list_for_each_entry(e, &g_cdev_list, link)
		if (e->node == node && !e->dead)
			return e;
	return NULL;
}

static struct nvme_cdev_entry *cdev_find_obj_locked(void *obj,
						    enum nvme_cdev_kind kind)
{
	struct nvme_cdev_entry *e;

	list_for_each_entry(e, &g_cdev_list, link)
		if (e->obj == obj && e->kind == kind && !e->dead)
			return e;
	return NULL;
}

/* ==========================================================================
 * File operations
 * ========================================================================== */

static int nvme_cdev_open(struct devfs_node *node, vfs_file_t *file, int flags,
			  struct task *cur)
{
	struct nvme_cdev_entry *e;
	uint64_t fl;
	int ret = 0;

	(void)flags;
	(void)cur;
	spin_lock_irqsave(&g_cdev_lock, &fl);
	e = cdev_find_node_locked(node);
	if (!e) {
		spin_unlock_irqrestore(&g_cdev_lock, fl);
		return -ENXIO;
	}
	switch (e->kind) {
	case NVME_CDEV_CTRL: {
		struct nvme_ctrl *ctrl = e->obj;

		/* Only a working controller can be opened. */
		if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE) {
			ret = -EWOULDBLOCK;
			break;
		}
		nvme_get_ctrl(ctrl);
		break;
	}
	case NVME_CDEV_NS: {
		struct nvme_ns *ns = e->obj;

		/* A path of a multipath head is reached through the head's
		 * node, never its own. */
		if (WARN_ON_ONCE(ns->head && nvme_ns_head_multipath(ns->head))) {
			ret = -ENXIO;
			break;
		}
		if (!nvme_get_ns(ns))
			ret = -ENXIO;
		break;
	}
	case NVME_CDEV_HEAD:
		if (!nvme_tryget_ns_head((struct nvme_ns_head *)e->obj))
			ret = -ENXIO;
		break;
	default:
		ret = -ENXIO;
		break;
	}
	if (!ret)
		e->opens++;
	spin_unlock_irqrestore(&g_cdev_lock, fl);
	if (!ret)
		device_file_set_priv(file, e);
	return ret;
}

static void nvme_cdev_release(vfs_file_t *file)
{
	struct nvme_cdev_entry *e = device_file_priv(file);
	enum nvme_cdev_kind kind;
	void *obj;
	bool last = false;
	uint64_t fl;

	if (WARN_ON_ONCE(!e))
		return;
	device_file_set_priv(file, NULL);
	spin_lock_irqsave(&g_cdev_lock, &fl);
	BUG_ON(e->opens <= 0); /* open count underflow: entry corrupt */
	e->opens--;
	kind = e->kind;
	obj = e->obj;
	if (e->dead && e->opens == 0) {
		list_del(&e->link);
		last = true;
	}
	spin_unlock_irqrestore(&g_cdev_lock, fl);

	if (last) {
		if (e->ida_minor)
			ida_free(&nvme_ns_chr_minor_ida, e->minor);
		kfree(e);
	}
	/* Last: the object (and the node inside it) may go now. */
	switch (kind) {
	case NVME_CDEV_CTRL:
		nvme_put_ctrl((struct nvme_ctrl *)obj);
		break;
	case NVME_CDEV_NS:
		nvme_put_ns((struct nvme_ns *)obj);
		break;
	case NVME_CDEV_HEAD:
		nvme_put_ns_head((struct nvme_ns_head *)obj);
		break;
	}
}

static long nvme_cdev_ioctl(vfs_file_t *file, unsigned long req, void *argp,
			    struct task *cur)
{
	struct nvme_cdev_entry *e = device_file_priv(file);
	/* A character node is never a partition. */
	int open_flags = file->flags & ~NVME_IOCTL_OPEN_PARTITION;

	if (!e)
		return -ENXIO;
	switch (e->kind) {
	case NVME_CDEV_CTRL:
		return nvme_dev_ioctl((struct nvme_ctrl *)e->obj, req, argp, cur,
				      open_flags);
	case NVME_CDEV_NS:
		return nvme_ns_ioctl((struct nvme_ns *)e->obj, req, argp, cur,
				     open_flags);
	case NVME_CDEV_HEAD:
		return nvme_ns_head_ioctl((struct nvme_ns_head *)e->obj, req,
					  argp, cur, open_flags);
	default:
		return -ENOTTY;
	}
}

static const struct device_ops nvme_cdev_ops = {
	.open = nvme_cdev_open,
	.release = nvme_cdev_release,
	.ioctl = nvme_cdev_ioctl,
};

/* ==========================================================================
 * Registration
 * ========================================================================== */

static int nvme_cdev_register(struct devfs_node *node, const char *name,
			      uint32_t major, uint32_t minor,
			      enum nvme_cdev_kind kind, void *obj,
			      bool ida_minor, const char *class,
			      const pci_device_t *pdev)
{
	struct nvme_cdev_entry *e;
	uint64_t fl;
	int ret;

	if (!major || !kdev_valid(major, minor))
		return -EINVAL;
	e = kalloc(sizeof(*e));
	if (!e)
		return -ENOMEM;
	mm_memset(e, 0, sizeof(*e));
	e->node = node;
	e->kind = kind;
	e->obj = obj;
	e->major = major;
	e->minor = minor;
	e->ida_minor = ida_minor;

	mm_memset(node, 0, sizeof(*node));
	ksnprintf(node->path, sizeof(node->path), "/dev/%s", name);
	node->mode = 0600;
	node->uid = 0;
	node->gid = 0;
	node->major = major;
	node->minor = minor;
	node->ops = &nvme_cdev_ops;
	node->priv = obj;

	/* The registry first: an open that finds the node finds the entry. */
	spin_lock_irqsave(&g_cdev_lock, &fl);
	if (cdev_find_obj_locked(obj, kind)) {
		spin_unlock_irqrestore(&g_cdev_lock, fl);
		kfree(e);
		return -EEXIST;
	}
	list_add_tail(&e->link, &g_cdev_list);
	spin_unlock_irqrestore(&g_cdev_lock, fl);

	ret = device_register(node);
	if (ret) {
		spin_lock_irqsave(&g_cdev_lock, &fl);
		list_del(&e->link);
		spin_unlock_irqrestore(&g_cdev_lock, fl);
		kfree(e);
		kprintf(NVME_LOG_PREFIX "%s: cannot register the device node "
					"(%d)\n",
			node->path, ret);
		return ret;
	}
	/* /sys/dev/char/M:m and /sys/class/<class>/<name>: name and number
	 * only, no pointer to the object (sysfs nodes are permanent). */
	ret = sysfs_add_char_device(name, major, minor, class, pdev);
	if (ret)
		kprintf(NVME_LOG_PREFIX "%s: no sysfs entry (%d)\n", name, ret);
	return 0;
}

/* Delete: no new opens, the node goes; the entry (and an ida minor) goes
 * now or with the last open file. */
static void nvme_cdev_unregister(struct devfs_node *node)
{
	struct nvme_cdev_entry *e;
	bool free_now = false;
	uint64_t fl;

	spin_lock_irqsave(&g_cdev_lock, &fl);
	e = cdev_find_node_locked(node);
	if (e) {
		e->dead = true;
		if (e->opens == 0) {
			list_del(&e->link);
			free_now = true;
		}
	} else {
		e = NULL;
	}
	spin_unlock_irqrestore(&g_cdev_lock, fl);
	if (!e) {
		WARN_ON_ONCE(1); /* deleting a node that is not registered */
		return;
	}
	device_unregister(node);
	if (free_now) {
		if (e->ida_minor)
			ida_free(&nvme_ns_chr_minor_ida, e->minor);
		kfree(e);
	}
}

int nvme_cdev_add_ctrl(struct nvme_ctrl *ctrl)
{
	int ret;

	if (WARN_ON(!ctrl) || ctrl->cdev_registered)
		return ctrl ? 0 : -EINVAL;
	if (!nvme_char_major) {
		kprintf(NVME_LOG_PREFIX "%s: no character major\n", ctrl->name);
		return -ENODEV;
	}
	if (ctrl->instance < 0)
		return -EINVAL;
	ret = nvme_cdev_register(&ctrl->cdev_node, ctrl->name, nvme_char_major,
				 (uint32_t)ctrl->instance, NVME_CDEV_CTRL, ctrl,
				 false, "nvme", ctrl->pdev);
	if (ret)
		return ret;
	ctrl->cdev_minor = (uint32_t)ctrl->instance;
	ctrl->cdev_registered = 1;
	return 0;
}

void nvme_cdev_del_ctrl(struct nvme_ctrl *ctrl)
{
	if (!ctrl || !ctrl->cdev_registered)
		return;
	nvme_cdev_unregister(&ctrl->cdev_node);
	ctrl->cdev_registered = 0;
}

/* A generic node with a fresh minor. */
static int nvme_cdev_add_generic(struct devfs_node *node, const char *name,
				 enum nvme_cdev_kind kind, void *obj,
				 const pci_device_t *pdev, uint32_t *minor_out)
{
	int minor, ret;

	if (!nvme_ns_chr_major) {
		kprintf(NVME_LOG_PREFIX "%s: no generic character major\n",
			name);
		return -ENODEV;
	}
	minor = ida_alloc_max(&nvme_ns_chr_minor_ida, KDEV_MINOR_MAX);
	if (minor < 0)
		return minor;
	ret = nvme_cdev_register(node, name, nvme_ns_chr_major,
				 (uint32_t)minor, kind, obj, true,
				 "nvme-generic", pdev);
	if (ret) {
		ida_free(&nvme_ns_chr_minor_ida, (unsigned int)minor);
		return ret;
	}
	*minor_out = (uint32_t)minor;
	return 0;
}

int nvme_cdev_add_ns(struct nvme_ns *ns)
{
	char name[NVME_NAME_LEN];
	int ret;

	if (WARN_ON(!ns || !ns->ctrl || !ns->head))
		return -EINVAL;
	if (ns->cdev_registered)
		return 0;
	ksnprintf(name, sizeof(name), "ng%dn%d", ns->ctrl->instance,
		  ns->head->instance);
	ret = nvme_cdev_add_generic(&ns->cdev_node, name, NVME_CDEV_NS, ns,
				    ns->ctrl->pdev, &ns->cdev_minor);
	if (ret)
		return ret;
	ns->cdev_registered = 1;
	return 0;
}

void nvme_cdev_del_ns(struct nvme_ns *ns)
{
	if (!ns || !ns->cdev_registered)
		return;
	nvme_cdev_unregister(&ns->cdev_node);
	ns->cdev_registered = 0;
}

int nvme_cdev_add_head(struct nvme_ns_head *head)
{
	char name[NVME_NAME_LEN];
	int ret;

	if (WARN_ON(!head || !head->subsys))
		return -EINVAL;
	if (head->cdev_registered)
		return 0;
	ksnprintf(name, sizeof(name), "ng%dn%d", head->subsys->instance,
		  head->instance);
	ret = nvme_cdev_add_generic(&head->cdev_node, name, NVME_CDEV_HEAD,
				    head, NULL, &head->cdev_minor);
	if (ret)
		return ret;
	head->cdev_registered = 1;
	return 0;
}

void nvme_cdev_del_head(struct nvme_ns_head *head)
{
	if (!head || !head->cdev_registered)
		return;
	nvme_cdev_unregister(&head->cdev_node);
	head->cdev_registered = 0;
}
