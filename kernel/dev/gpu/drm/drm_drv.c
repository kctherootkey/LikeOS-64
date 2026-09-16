// LikeOS -- display-manager core: devices, files, ioctl dispatch,
// events, the primary/render nodes.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/ioctl.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/file.h>
#include <kernel/fs/sysfs.h>
#include <kernel/fs/pseudofs.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/dev/rand/random.h>

#define DRM_MAJOR 226
#define DRM_RENDER_MINOR_BASE 128
/* DEVFS_GID_VIDEO / DEVFS_GID_RENDER: <kernel/dev/device.h>, via drm.h. */

int drm_copy_from_user(void *dst, const void *user, size_t n)
{
	return copy_from_user(dst, user, n) == 0 ? 0 : -EFAULT;
}

int drm_copy_to_user(void *user, const void *src, size_t n)
{
	return copy_to_user(user, src, n) == 0 ? 0 : -EFAULT;
}

/* ---- files ---------------------------------------------------------- */

static struct drm_file *drm_file_of(vfs_file_t *f)
{
	return (struct drm_file *)device_file_priv(f);
}

static int drm_open(struct devfs_node *node, vfs_file_t *file, int flags,
		    task_t *cur)
{
	struct drm_device *dev = node->priv;
	struct drm_file *fp = kalloc(sizeof(*fp));
	uint64_t fl;
	(void)flags;

	if (!fp)
		return -ENOMEM;
	mm_memset(fp, 0, sizeof(*fp));
	fp->dev = dev;
	/* Every handle this file hands out carries this number, so an id
	 * names one object across the whole device rather than one per file.
	 * Never zero, so a valid handle is never a small integer that some
	 * other file could also produce. */
	static uint32_t next_file_id;
	fp->file_id = 1 + (__atomic_fetch_add(&next_file_id, 1, __ATOMIC_RELAXED) &
			   0x7FFEu);
	fp->is_render = (node == &dev->node_render);
	fp->uid = cur ? cur->cred.euid : 0;
	spinlock_init(&fp->lock, "drm_file");
	wq_head_init(&fp->wq, "drm_file");
	if (dev->drv->open) {
		int rc = dev->drv->open(dev, fp);
		if (rc) {
			kfree(fp);
			return rc;
		}
	}
	spin_lock_irqsave(&dev->lock, &fl);
	fp->next = dev->files;
	dev->files = fp;
	spin_unlock_irqrestore(&dev->lock, fl);
	device_file_set_priv(file, fp);

	/* Opening the primary node while nobody holds the display makes this
	 * file its master.  A display server counts on it: it opens the node
	 * and starts issuing mode-setting calls straight away, and only asks
	 * for the rights explicitly (SET_MASTER) when re-acquiring them after
	 * having dropped them.  Without this its very first call -- adding a
	 * framebuffer, while probing what the device can scan out -- came
	 * back EACCES, from which it concluded the device cannot do 32bpp at
	 * all and fell back to a 24bpp packed framebuffer, which this driver
	 * has no way to allocate.  The screen initialization then failed with
	 * nothing in the log to say why.
	 *
	 * Render nodes never carry the display and never become master. */
	if (!fp->is_render && !dev->master)
		(void)drm_master_set(dev, fp);
	return 0;
}

static void drm_release(vfs_file_t *file)
{
	struct drm_file *fp = drm_file_of(file);
	struct drm_device *dev;
	uint64_t fl;

	if (!fp)
		return;
	dev = fp->dev;
	if (fp->is_master)
		drm_master_drop(dev, fp);
	/* Every handle this file held. */
	for (uint32_t h = 1; h < fp->nhandles; h++) {
		struct drm_gem_object **cell = drm_handle_slot(fp, h);

		if (*cell) {
			struct drm_gem_object *o = *cell;
			*cell = NULL;
			drm_gem_put(o);
		}
	}
	for (uint32_t k = 0; k < fp->nhandles / DRM_HANDLE_CHUNK; k++)
		kfree(fp->handles[k]);
	fp->nhandles = 0;
	drm_fence_handles_release(fp);
	drm_syncobj_file_release(fp);
	drm_kms_file_release(dev, fp);
	if (dev->drv->postclose)
		dev->drv->postclose(dev, fp);
	spin_lock_irqsave(&dev->lock, &fl);
	struct drm_file **pp = &dev->files;
	while (*pp) {
		if (*pp == fp) {
			*pp = fp->next;
			break;
		}
		pp = &(*pp)->next;
	}
	spin_unlock_irqrestore(&dev->lock, fl);
	while (fp->events) {
		struct drm_pending_event *e = fp->events;
		fp->events = e->next;
		kfree(e);
	}
	kfree(fp);
}

/* ---- master / auth ------------------------------------------------------ */

int drm_master_set(struct drm_device *dev, struct drm_file *fp)
{
	uint64_t fl;

	spin_lock_irqsave(&dev->lock, &fl);
	if (dev->master && dev->master != fp) {
		spin_unlock_irqrestore(&dev->lock, fl);
		return -EBUSY;
	}
	if (dev->master == fp) {
		/* Already the master: a display server asks again (SET_MASTER
		 * on entering its VT) after having become master by opening
		 * the node.  Nothing changes hands, so nothing below runs
		 * again -- the hand-over below is counted (fbdev_opened), and
		 * a second count with one release on exit left the console
		 * neither painting nor listening after X had gone. */
		spin_unlock_irqrestore(&dev->lock, fl);
		return 0;
	}
	dev->master = fp;
	fp->is_master = 1;
	fp->authenticated = 1;
	spin_unlock_irqrestore(&dev->lock, fl);
	/* The screen belongs to the master now; the console stops pushing. */
	drm_console_suspend(dev);
	if (dev->drv->master_set)
		dev->drv->master_set(dev, fp);
	/* ...and stops LISTENING.  The console's output is silenced above,
	 * but its input -- the pointer driving the scrollbar, the scrollback
	 * and the terminal's mouse reporting -- is gated on the display being
	 * owned (fbdev_display_owned), which a display manager on this node
	 * never signals by itself: it opens /dev/dri/cardN, not /dev/fb0.
	 * Said here, for every driver, rather than by one driver's hook: with
	 * the Intel driver every scroll in an X window was also scrolling the
	 * console underneath, unseen until X exited. */
	fbdev_opened();
	return 0;
}

void drm_master_drop(struct drm_device *dev, struct drm_file *fp)
{
	uint64_t fl;

	spin_lock_irqsave(&dev->lock, &fl);
	if (dev->master != fp) {
		spin_unlock_irqrestore(&dev->lock, fl);
		return;
	}
	dev->master = NULL;
	fp->is_master = 0;
	spin_unlock_irqrestore(&dev->lock, fl);
	if (dev->drv->master_drop)
		dev->drv->master_drop(dev, fp);
	/* The display is the console's again: input is forwarded to it once
	 * more and its screen and pointer are repainted in full (see
	 * fbdev_closed) -- after the driver has handed the hardware back and
	 * before the console's mode is set again below. */
	fbdev_closed();
	/* ...and back, which means setting the mode again: the CRTC is on a
	 * framebuffer that is going away with the client that made it. */
	drm_console_resume(dev);
}

/* ---- events ---------------------------------------------------------- */

void drm_event_queue(struct drm_file *fp, const void *data, uint32_t length)
{
	struct drm_pending_event *e = kalloc(sizeof(*e));
	uint64_t fl;

	if (!e || length > sizeof(e->data)) {
		if (e)
			kfree(e);
		return;
	}
	e->next = NULL;
	e->length = length;
	mm_memcpy(e->data, data, length);
	spin_lock_irqsave(&fp->lock, &fl);
	if (fp->events_tail)
		fp->events_tail->next = e;
	else
		fp->events = e;
	fp->events_tail = e;
	spin_unlock_irqrestore(&fp->lock, fl);
	poll_notify_wq(&fp->wq);
}

static long drm_read(vfs_file_t *file, void *buf, long bytes, int nonblock)
{
	struct drm_file *fp = drm_file_of(file);
	task_t *cur = sched_current();
	long done = 0;
	uint64_t fl;

	for (;;) {
		spin_lock_irqsave(&fp->lock, &fl);
		while (fp->events && fp->events->length <= (uint32_t)(bytes - done)) {
			struct drm_pending_event *e = fp->events;
			fp->events = e->next;
			if (!fp->events)
				fp->events_tail = NULL;
			spin_unlock_irqrestore(&fp->lock, fl);
			if (copy_to_user((char *)buf + done, e->data, e->length) != 0) {
				kfree(e);
				return done ? done : -EFAULT;
			}
			done += e->length;
			kfree(e);
			spin_lock_irqsave(&fp->lock, &fl);
		}
		if (done || (fp->events && (uint32_t)bytes < fp->events->length)) {
			spin_unlock_irqrestore(&fp->lock, fl);
			return done ? done : -EINVAL;
		}
		spin_unlock_irqrestore(&fp->lock, fl);
		if (nonblock)
			return -EAGAIN;
		/* Resumable: control only reaches here with `done' still zero,
		 * so not one byte has been handed to the caller and running the
		 * read again is indistinguishable from never having stopped. */
		if (signal_pending(cur))
			return -ERESTARTSYS;
		struct wait_queue_entry we;
		fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(&fp->wq, &we);
		if (!fp->events) {
			cur->wait_channel = fp;
			cur->state = TASK_BLOCKED;
			local_irq_restore(fl);
			sched_schedule();
		} else {
			local_irq_restore(fl);
		}
		wq_remove(&fp->wq, &we);
	}
}

static short drm_poll(vfs_file_t *file, short events, struct poll_table *pt)
{
	struct drm_file *fp = drm_file_of(file);

	poll_wait(pt, file, &fp->wq);
	return (events & POLLIN) && fp->events ? POLLIN : 0;
}

/* ---- mmap: objects by offset ------------------------------------------- */

static uint64_t drm_mmap_page_phys(void *obj, uint64_t index)
{
	struct drm_gem_object *o = obj;
	return o->dev->drv->gem_page_phys(o, index);
}

/* These are only ever the region records' get/put (the address space
 * calls them as records are held for splits and forked copies, and
 * dropped at unmap and exit), so they double as the mapping census the
 * dirty tracker checks its sweeps against -- see drm_dirty.c. */
static void drm_mmap_get(void *obj)
{
	drm_gem_get(obj);
}

static void drm_mmap_put(void *obj)
{
	drm_gem_put(obj);
}

/* A mapping that does not start at the object's first page, or that is
 * of a particular kind, carries this in place of the object: the page
 * index is shifted and the object reference is held by it.  Such
 * mappings are never dirty-tracked (the tracker addresses the object
 * from byte zero), which is right for the drivers that use them -- the
 * tracking exists for a device that cannot see the pages, and a device
 * with an address space of its own can. */
struct drm_mmap_ctx {
	int refs;
	struct drm_gem_object *obj;
	uint64_t first_page;
};

static uint64_t drm_mmap_ctx_page_phys(void *arg, uint64_t index)
{
	struct drm_mmap_ctx *c = arg;
	struct drm_gem_object *o = c->obj;
	return o->dev->drv->gem_page_phys(o, c->first_page + index);
}

static void drm_mmap_ctx_get(void *arg)
{
	struct drm_mmap_ctx *c = arg;
	__atomic_fetch_add(&c->refs, 1, __ATOMIC_ACQ_REL);
}

static void drm_mmap_ctx_put(void *arg)
{
	struct drm_mmap_ctx *c = arg;
	if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) == 0) {
		drm_gem_put(c->obj);
		kfree(c);
	}
}

static int drm_mmap(vfs_file_t *file, struct device_mmap *m)
{
	struct drm_file *fp = drm_file_of(file);
	struct drm_device *dev = fp->dev;
	struct drm_gem_object *o = drm_gem_by_offset(dev, m->offset);
	unsigned kind = drm_gem_mmap_kind_of(m->offset);

	if (!o)
		return -EINVAL;
	if (!dev->drv->gem_page_phys) {
		drm_gem_put(o);
		return -ENODEV;
	}
	/* The offset within the window is the offset within the object. */
	uint64_t inner = m->offset & 0xFFFFFFFFULL;
	if (inner + m->length > o->size) {
		drm_gem_put(o);
		return -EINVAL;
	}
	uint64_t pte = dev->drv->gem_mmap_pte ?
			       dev->drv->gem_mmap_pte(o, kind) :
			       dev->drv->gem_mmap_pte_extra;
	if (inner || kind) {
		struct drm_mmap_ctx *c = kalloc(sizeof(*c));
		if (!c) {
			drm_gem_put(o);
			return -ENOMEM;
		}
		c->refs = 1;
		c->obj = o; /* the reference taken above */
		c->first_page = inner / PAGE_SIZE;
		m->page_phys = drm_mmap_ctx_page_phys;
		m->obj = c;
		m->get = drm_mmap_ctx_get;
		m->put = drm_mmap_ctx_put;
		m->pte_extra = pte;
		m->dirty_ops = NULL;
		return 0;
	}
	m->page_phys = drm_mmap_page_phys;
	m->obj = o; /* the reference taken above is the mapping's */
	m->get = drm_mmap_get;
	m->put = drm_mmap_put;
	m->pte_extra = pte;
	m->dirty_ops = &drm_gem_dirty_mmap_ops;
	/* The census entry is NOT made here: it depends on the mapping's
	 * protection, which only the address space knows, so mmap makes it
	 * once the record exists (mm_region_census). */
	return 0;
}

/* ---- ioctl dispatch --------------------------------------------------- */

static int drm_is_render_only_ok(unsigned nr)
{
	switch (nr) {
	case 0x00: /* VERSION */
	case 0x09: /* GEM_CLOSE */
	case 0x0c: /* GET_CAP */
	case 0x0d: /* SET_CLIENT_CAP */
	case 0x2d: /* PRIME_HANDLE_TO_FD */
	case 0x2e: /* PRIME_FD_TO_HANDLE */
		return 1;
	default:
		return drm_syncobj_is_ioctl(nr);
	}
}

static long drm_core_ioctl(struct drm_device *dev, struct drm_file *fp,
			   unsigned nr, void *kb, unsigned size, int *handled)
{
	(void)size; /* every core request is a fixed-size structure */
	*handled = 1;
	switch (nr) {
	case 0x00: { /* VERSION */
		struct drm_version *v = kb;
		const char *name = dev->drv->name, *date = dev->drv->date,
			   *desc = dev->drv->desc;
		size_t nl = 0, dl = 0, sl = 0;
		while (name[nl])
			nl++;
		while (date[dl])
			dl++;
		while (desc[sl])
			sl++;
		v->version_major = dev->drv->major;
		v->version_minor = dev->drv->minor;
		v->version_patchlevel = dev->drv->patch;
		if (v->name && v->name_len >= nl &&
		    copy_to_user(v->name, name, nl) != 0)
			return -EFAULT;
		if (v->date && v->date_len >= dl &&
		    copy_to_user(v->date, date, dl) != 0)
			return -EFAULT;
		if (v->desc && v->desc_len >= sl &&
		    copy_to_user(v->desc, desc, sl) != 0)
			return -EFAULT;
		v->name_len = nl;
		v->date_len = dl;
		v->desc_len = sl;
		return 0;
	}
	case 0x01: { /* GET_UNIQUE */
		struct drm_unique *u = kb;
		size_t l = 0;
		while (dev->unique[l])
			l++;
		if (u->unique && u->unique_len >= l &&
		    copy_to_user(u->unique, dev->unique, l) != 0)
			return -EFAULT;
		u->unique_len = l;
		return 0;
	}
	case 0x02: { /* GET_MAGIC */
		struct drm_auth *a = kb;
		if (fp->is_render)
			return -EACCES;
		if (!fp->magic) {
			uint32_t m;
			do {
				random_get_bytes(&m, sizeof(m), 0);
			} while (m == 0);
			fp->magic = m;
		}
		a->magic = fp->magic;
		return 0;
	}
	case 0x11: { /* AUTH_MAGIC */
		struct drm_auth *a = kb;
		uint64_t fl;
		int ok = 0;
		if (!fp->is_master)
			return -EACCES;
		spin_lock_irqsave(&dev->lock, &fl);
		for (struct drm_file *o = dev->files; o; o = o->next) {
			if (o->magic && o->magic == a->magic) {
				o->authenticated = 1;
				ok = 1;
				break;
			}
		}
		spin_unlock_irqrestore(&dev->lock, fl);
		return ok ? 0 : -EINVAL;
	}
	case 0x07: { /* SET_VERSION */
		struct drm_set_version *sv = kb;
		sv->drm_di_major = 1;
		sv->drm_di_minor = 4;
		sv->drm_dd_major = dev->drv->major;
		sv->drm_dd_minor = dev->drv->minor;
		return 0;
	}
	case 0x1e: /* SET_MASTER */
		if (fp->is_render)
			return -EACCES;
		return drm_master_set(dev, fp);
	case 0x1f: /* DROP_MASTER */
		if (!fp->is_master)
			return -EINVAL;
		drm_master_drop(dev, fp);
		return 0;
	case 0x0c: { /* GET_CAP */
		struct drm_get_cap *c = kb;
		switch (c->capability) {
		case DRM_CAP_DUMB_BUFFER:
			c->value = 1;
			return 0;
		case DRM_CAP_VBLANK_HIGH_CRTC:
			c->value = 1;
			return 0;
		case DRM_CAP_DUMB_PREFERRED_DEPTH:
			c->value = 24;
			return 0;
		case DRM_CAP_DUMB_PREFER_SHADOW:
			c->value = 0;
			return 0;
		case DRM_CAP_PRIME:
			c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
			return 0;
		case DRM_CAP_TIMESTAMP_MONOTONIC:
			c->value = 1;
			return 0;
		case DRM_CAP_ASYNC_PAGE_FLIP:
			c->value = 0;
			return 0;
		case DRM_CAP_CURSOR_WIDTH:
			c->value = dev->drv->cursor_w ? dev->drv->cursor_w : 64;
			return 0;
		case DRM_CAP_CURSOR_HEIGHT:
			c->value = dev->drv->cursor_h ? dev->drv->cursor_h : 64;
			return 0;
		case DRM_CAP_ADDFB2_MODIFIERS:
			c->value = 1;
			return 0;
		case DRM_CAP_PAGE_FLIP_TARGET:
			c->value = 0;
			return 0;
		case DRM_CAP_CRTC_IN_VBLANK_EVENT:
			c->value = 1;
			return 0;
		case DRM_CAP_SYNCOBJ:
		case DRM_CAP_SYNCOBJ_TIMELINE:
			c->value = 1;
			return 0;
		default:
			if (dev->drv->get_cap &&
			    dev->drv->get_cap(dev, c->capability, &c->value) == 0)
				return 0;
			return -EINVAL;
		}
	}
	case 0x0d: { /* SET_CLIENT_CAP */
		struct drm_set_client_cap *c = kb;
		switch (c->capability) {
		case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
		case DRM_CLIENT_CAP_ATOMIC:
		case DRM_CLIENT_CAP_ASPECT_RATIO:
		case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
		case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT:
			if (c->value)
				fp->client_caps |= 1ULL << c->capability;
			else
				fp->client_caps &= ~(1ULL << c->capability);
			if (c->capability == DRM_CLIENT_CAP_ATOMIC && c->value)
				fp->client_caps |= 1ULL << DRM_CLIENT_CAP_UNIVERSAL_PLANES;
			return 0;
		default:
			return -EINVAL;
		}
	}
	case 0x09: { /* GEM_CLOSE */
		struct drm_gem_close *g = kb;
		return drm_gem_handle_delete(fp, g->handle);
	}
	case 0x0a: { /* GEM_FLINK */
		struct drm_gem_flink *g = kb;
		struct drm_gem_object *o = drm_gem_lookup(fp, g->handle);
		if (!o)
			return -ENOENT;
		uint64_t fl;
		spin_lock_irqsave(&dev->lock, &fl);
		if (!o->flink_name)
			o->flink_name = (int)++dev->next_flink;
		g->name = (uint32_t)o->flink_name;
		spin_unlock_irqrestore(&dev->lock, fl);
		drm_gem_put(o);
		return 0;
	}
	case 0x0b: { /* GEM_OPEN */
		struct drm_gem_open *g = kb;
		struct drm_gem_object *found = NULL;
		uint64_t fl;
		spin_lock_irqsave(&dev->lock, &fl);
		for (struct drm_gem_object *o = dev->objects; o; o = o->next) {
			if (o->flink_name && (uint32_t)o->flink_name == g->name &&
			    drm_gem_get_unless_zero(o)) {
				found = o;
				break;
			}
		}
		spin_unlock_irqrestore(&dev->lock, fl);
		if (!found)
			return -ENOENT;
		uint32_t h;
		int rc = drm_gem_handle_create(fp, found, &h);
		g->handle = h;
		g->size = found->size;
		drm_gem_put(found);
		return rc;
	}
	case 0x2d: { /* PRIME_HANDLE_TO_FD */
		struct drm_prime_handle *p = kb;
		struct drm_gem_object *o = drm_gem_lookup(fp, p->handle);
		if (!o)
			return -ENOENT;
		int fd = drm_prime_export(fp, o, (int)p->flags);
		drm_gem_put(o);
		if (fd < 0)
			return fd;
		p->fd = fd;
		return 0;
	}
	case 0x2e: { /* PRIME_FD_TO_HANDLE */
		struct drm_prime_handle *p = kb;
		struct drm_gem_object *o = drm_prime_import(p->fd);
		if (!o)
			return -EINVAL;
		uint32_t h = 0;
		/* Dedup: the same object already has a handle in this file?
		 * The slot has to be turned back into a handle -- handles
		 * name their file, and returning a bare slot would hand the
		 * client an id that names a different file's object. */
		uint64_t fl;
		spin_lock_irqsave(&fp->lock, &fl);
		for (uint32_t i = 1; i < fp->nhandles; i++)
			if (*drm_handle_slot(fp, i) == o) {
				h = drm_gem_handle_of_slot(fp, i);
				break;
			}
		spin_unlock_irqrestore(&fp->lock, fl);
		int rc = 0;
		if (!h)
			rc = drm_gem_handle_create(fp, o, &h);
		drm_gem_put(o);
		p->handle = h;
		return rc;
	}
	default:
		*handled = 0;
		return -ENOTTY;
	}
}

static long drm_ioctl(vfs_file_t *file, unsigned long req, void *argp,
		      struct task *cur)
{
	struct drm_file *fp = drm_file_of(file);
	struct drm_device *dev = fp->dev;
	unsigned nr = _IOC_NR(req);
	unsigned dir = _IOC_DIR(req);
	unsigned size = _IOC_SIZE(req);
	uint8_t kbuf[512];
	long rc;
	int handled = 0;
	(void)cur;

	if (_IOC_TYPE(req) != DRM_IOCTL_BASE)
		return -ENOTTY;
	if (size > sizeof(kbuf))
		return -EINVAL;
	mm_memset(kbuf, 0, sizeof(kbuf));
	if ((dir & _IOC_WRITE) && size) {
		if (!argp || !validate_user_ptr((uint64_t)argp, size))
			return -EFAULT;
		if (copy_from_user(kbuf, argp, size) != 0)
			return -EFAULT;
	}

	/* Whose ioctl this is.
	 *
	 * The driver's own commands occupy a WINDOW in the number space,
	 * [DRM_COMMAND_BASE, DRM_COMMAND_END) = [0x40, 0xa0) -- not
	 * everything from 0x40 upwards.  The core owns 0x00-0x3f below it
	 * and, above the window, the whole mode-setting interface from 0xa0
	 * on: GETRESOURCES, GETCONNECTOR, SETCRTC, the dumb-buffer calls,
	 * PAGE_FLIP, ATOMIC, all of it.
	 *
	 * Testing only the lower bound handed every one of those to the
	 * driver with 0x40 subtracted from its number, which matches no
	 * driver command, so the driver's default arm answered -- and the
	 * mode-setting interface was unreachable through this node.  The
	 * X server's modesetting driver opened /dev/dri/card0, asked for
	 * the resources, got the driver's error back and concluded there
	 * was no display device: "no screens found", with nothing in the
	 * log to say why. */
	int is_driver_ioctl = (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END);

	/* Render nodes: rendering only. */
	if (fp->is_render) {
		int ok = 0;
		if (is_driver_ioctl)
			ok = dev->drv->render_allowed ?
				     dev->drv->render_allowed(nr - DRM_COMMAND_BASE) :
				     1;
		else
			ok = drm_is_render_only_ok(nr);
		if (!ok)
			return -EACCES;
	}

	if (is_driver_ioctl) {
		rc = dev->drv->ioctl ? dev->drv->ioctl(dev, fp,
						       nr - DRM_COMMAND_BASE, dir,
						       kbuf, size, &handled) :
				       -ENOTTY;
		if (!handled)
			rc = -ENOTTY;
	} else if (drm_syncobj_is_ioctl(nr)) {
		rc = drm_syncobj_ioctl(dev, fp, nr, kbuf, size, &handled);
		if (!handled)
			rc = -ENOTTY;
	} else {
		rc = drm_core_ioctl(dev, fp, nr, kbuf, size, &handled);
		if (!handled)
			rc = drm_kms_ioctl(dev, fp, nr, kbuf, size, &handled);
		if (!handled)
			rc = -ENOTTY;
	}

	/* A refused ioctl is a client error and belongs in the return value,
	 * not on the console -- except that the client here is a driver stack
	 * that does not always report what it was told.  Mesa answers some
	 * failures by abandoning a teardown part-way, which leaves objects it
	 * later walks again, and the crash that follows names neither the
	 * ioctl nor the error.  So say it once per distinct command, which
	 * costs a line per problem rather than a line per call.
	 *
	 * ENOTTY is left out on purpose: probing for an ioctl that does not
	 * exist is how userspace tests for optional features, and answering
	 * that is not a failure.  Nor is a call cut short by a signal: the
	 * caller resumes or retries it. */
	if (rc < 0 && rc != -ENOTTY && rc != -ERESTARTSYS && rc != -EINTR &&
	    rc != -EAGAIN) {
		/* Once per command, error AND process: a client that dies
		 * of a refused call is usually not the one that first made
		 * it, and a line hidden behind another process's is a
		 * death with no cause on record. */
		static uint64_t seen[128];
		static unsigned nseen;
		unsigned i;
		int pid = sched_current() ? (int)sched_current()->tgid : -1;
		uint64_t key = ((uint64_t)(uint32_t)pid << 32) | ((uint32_t)nr << 16) |
			       (uint32_t)(-rc & 0xFFFF);

		for (i = 0; i < nseen && i < 128; i++)
			if (seen[i] == key)
				break;
		if (i >= nseen && nseen < 128) {
			seen[nseen++] = key;
			kprintf("drm: ioctl nr=0x%02x (%s) returned %d for pid %d\n",
				(unsigned)nr,
				is_driver_ioctl ? "driver" : "core", (int)rc, pid);
		}
	}

	if (rc == 0 && (dir & _IOC_READ) && size) {
		if (!argp || !validate_user_ptr((uint64_t)argp, size))
			return -EFAULT;
		if (copy_to_user(argp, kbuf, size) != 0)
			return -EFAULT;
	}
	return rc;
}

static const struct device_ops drm_node_ops = {
	.open = drm_open,
	.release = drm_release,
	.read = drm_read,
	.poll = drm_poll,
	.mmap = drm_mmap,
	.ioctl = drm_ioctl,
};

/* ---- registration ------------------------------------------------------ */

static struct devfs_node g_dri_dir;
static int g_dri_dir_registered;
static struct drm_device *g_drm_devices;

void drm_late_init(void)
{
	for (struct drm_device *dev = g_drm_devices; dev; dev = dev->next_dev) {
		if (dev->late_init_done)
			continue;
		dev->late_init_done = 1;
		if (dev->drv->late_init) {
			int rc = dev->drv->late_init(dev);
			if (rc)
				kprintf("[drm] %s: late init failed (%d)\n",
					dev->drv->name, rc);
		}
	}
}

static long drm_show_hotplug(struct pfs_node *n, char *buf, long cap)
{
	struct drm_device *dev = n->arg;
	return ksnprintf(buf, (size_t)cap, "%u\n", dev->hotplug_epoch);
}

/* Where the GPU's pages are, read before and after a run.
 *
 * `memstat -o' can say that drm_gem_alloc_pages() holds the memory, but not
 * WHICH objects hold it or whether anything still references them -- and
 * those are different bugs.  A client that has exited should leave neither
 * live objects nor a dead queue behind, so this separates the two:
 *
 *   objects-live    still referenced by somebody (a handle, a mapping, the
 *                   scanout, a driver structure).  Memory here means a
 *                   reference was not dropped.
 *   dead-queued     unreferenced, waiting for the device to finish reading
 *                   them (see drm_gem_put / gem_reap).  Memory here means
 *                   fences are not being retired, not that a ref leaked.
 *   borrowed-pages  an object over a client's own memory (gem_userptr):
 *                   its pages are counted here but were never ours to free.
 *
 * Counted under the device lock, formatted after it: ksnprintf with
 * interrupts disabled is not worth the shorter function.  The walk is
 * bounded so a corrupt list cannot hold the lock forever. */
#define DRM_GEM_STATS_MAX 100000u

static long drm_show_gem_stats(struct pfs_node *n, char *buf, long cap)
{
	struct drm_device *dev = n->arg;
	unsigned long live = 0, live_pages = 0;
	unsigned long bo = 0, bo_pages = 0, surf = 0, surf_pages = 0;
	unsigned long borrowed = 0, borrowed_pages = 0;
	unsigned long waiting = 0, waiting_pages = 0;
	unsigned long dead = 0, dead_pages = 0, dead_unsignalled = 0;
	unsigned long files = 0, guard;
	unsigned long created;
	/* Which open file's handle table still names the live objects.
	 * DRM_GEM_STATS_FILES bounds what is PRINTED; the totals below count
	 * every file however many there are. */
#define DRM_GEM_STATS_FILES 8
	uint32_t f_id[DRM_GEM_STATS_FILES];
	int f_render[DRM_GEM_STATS_FILES];
	unsigned long f_handles[DRM_GEM_STATS_FILES];
	unsigned long f_pages[DRM_GEM_STATS_FILES];
	unsigned nfiles_shown = 0;
	unsigned long handles_total = 0, handle_pages_total = 0;
	unsigned long fb_owned = 0, fb_owned_pages = 0;
	unsigned long fb_internal = 0, fb_internal_pages = 0;
	int dead_n;
	uint64_t fl;

	spin_lock_irqsave(&dev->lock, &fl);
	guard = DRM_GEM_STATS_MAX;
	for (struct drm_gem_object *o = dev->objects; o && guard; o = o->next) {
		guard--;
		live++;
		live_pages += o->npages;
		if (o->kind == DRM_GEM_SURFACE) {
			surf++;
			surf_pages += o->npages;
		} else {
			bo++;
			bo_pages += o->npages;
		}
		if (o->pages_borrowed) {
			borrowed++;
			borrowed_pages += o->npages;
		}
		if (o->fence && !o->fence->signaled) {
			waiting++;
			waiting_pages += o->npages;
		}
	}
	guard = DRM_GEM_STATS_MAX;
	for (struct drm_gem_object *o = dev->dead; o && guard;
	     o = o->dead_next) {
		guard--;
		dead++;
		dead_pages += o->npages;
		if (o->fence && !o->fence->signaled)
			dead_unsignalled++;
	}
	guard = DRM_GEM_STATS_MAX;
	for (struct drm_file *f = dev->files; f && guard; f = f->next) {
		unsigned long fh = 0, fpg = 0;

		guard--;
		files++;
		/* Slots read under the DEVICE lock without the file's own,
		 * exactly as drm_gem_lookup_foreign() does: a file cannot be
		 * unlinked from dev->files while this lock is held. */
		for (uint32_t h = 1; h < f->nhandles; h++) {
			struct drm_gem_object *o = *drm_handle_slot(f, h);

			if (!o)
				continue;
			fh++;
			fpg += o->npages;
		}
		handles_total += fh;
		handle_pages_total += fpg;
		if (nfiles_shown < DRM_GEM_STATS_FILES) {
			f_id[nfiles_shown] = f->file_id;
			f_render[nfiles_shown] = f->is_render;
			f_handles[nfiles_shown] = fh;
			f_pages[nfiles_shown] = fpg;
			nfiles_shown++;
		}
	}
	/* The other reference this side can count.  An internal framebuffer
	 * (owner NULL) is the console's or a cursor wrapper's and is NOT
	 * freed when a client exits -- drm_kms_file_release() frees only
	 * those whose owner is the closing file -- so it is worth seeing
	 * separately from the ones a client owns. */
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		struct drm_framebuffer *fb = &dev->fbs[i];

		if (!fb->id || !fb->obj)
			continue;
		if (fb->owner) {
			fb_owned++;
			fb_owned_pages += fb->obj->npages;
		} else {
			fb_internal++;
			fb_internal_pages += fb->obj->npages;
		}
	}
	created = dev->next_obj_id;
	dead_n = dev->dead_n;
	spin_unlock_irqrestore(&dev->lock, fl);

	long len = ksnprintf(
		buf, (size_t)cap,
		"objects-live     %lu  pages %lu (%lu MB)\n"
		"  buffers        %lu  pages %lu (%lu MB)\n"
		"  surfaces       %lu  pages %lu (%lu MB)\n"
		"  borrowed       %lu  pages %lu  (client memory, never ours to free)\n"
		"  awaiting-fence %lu  pages %lu\n"
		"dead-queued      %lu  pages %lu (%lu MB)  dead_n %d  unsignalled %lu\n"
		"open-files       %lu\n"
		"objects-created  %lu\n",
		live, live_pages, live_pages / 256, bo, bo_pages,
		bo_pages / 256, surf, surf_pages, surf_pages / 256, borrowed,
		borrowed_pages, waiting, waiting_pages, dead, dead_pages,
		dead_pages / 256, dead_n, dead_unsignalled, files, created);

	/* What KEEPS the live objects alive.  A live object is held by a
	 * handle, a framebuffer, a mapping or a driver structure; the first
	 * two are countable from here, and between them they answer the
	 * question a leak hunt actually asks -- whether a client that has
	 * exited left something behind (handles-held well under
	 * objects-live), or whether a client still running is holding it
	 * (one file's handle count carrying the difference).
	 *
	 * An object can be counted twice, by a handle AND by a framebuffer
	 * over it: these are reference holders, not a partition of memory. */
	if (len > 0 && len < cap)
		len += ksnprintf(buf + len, (size_t)(cap - len),
				 "handles-held     %lu  pages %lu (%lu MB)\n"
				 "framebuffers     owned %lu pages %lu / internal %lu pages %lu\n",
				 handles_total, handle_pages_total,
				 handle_pages_total / 256, fb_owned,
				 fb_owned_pages, fb_internal, fb_internal_pages);
	for (unsigned i = 0; i < nfiles_shown && len > 0 && len < cap; i++)
		len += ksnprintf(buf + len, (size_t)(cap - len),
				 "  file %u (%s) handles %lu  pages %lu (%lu MB)\n",
				 f_id[i], f_render[i] ? "render" : "card",
				 f_handles[i], f_pages[i], f_pages[i] / 256);
	return len;
}

/* ---- power ---------------------------------------------------------------- */

int drm_suspend(struct drm_device *dev)
{
	int rc;

	if (!dev->drv->suspend || !dev->drv->atomic_commit)
		return -ENODEV;
	if (dev->suspended)
		return 0;
	/* The console keeps drawing into its buffer; nothing pushes it
	 * while the device is down. */
	drm_console_suspend(dev);
	rc = dev->drv->suspend(dev);
	if (rc) {
		drm_console_resume(dev);
		return rc;
	}
	dev->suspended = 1;
	kprintf("[drm] %s: suspended\n", dev->drv->name);
	return 0;
}

int drm_resume(struct drm_device *dev)
{
	int rc;

	if (!dev->drv->resume || !dev->drv->atomic_commit)
		return -ENODEV;
	if (!dev->suspended)
		return 0;
	rc = dev->drv->resume(dev);
	if (rc) {
		kprintf("[drm] %s: resume failed (%d)\n", dev->drv->name, rc);
		return rc;
	}
	dev->suspended = 0;
	/* What was shown before is shown again: the committed state, every
	 * active crtc as a fresh mode set. */
	rc = drm_atomic_replay(dev);
	if (rc)
		kprintf("[drm] %s: the display did not come back (%d)\n",
			dev->drv->name, rc);
	/* the console's pushes resume; its mode was part of the replay
	 * when it owns the screen, so no second mode set */
	drm_console_resume_pushes(dev);
	kprintf("[drm] %s: resumed\n", dev->drv->name);
	return rc;
}

int drm_suspend_all(void)
{
	int rc = 0;
	for (struct drm_device *dev = g_drm_devices; dev; dev = dev->next_dev) {
		int r = drm_suspend(dev);
		if (r && r != -ENODEV && rc == 0)
			rc = r;
	}
	return rc;
}

int drm_resume_all(void)
{
	int rc = 0;
	for (struct drm_device *dev = g_drm_devices; dev; dev = dev->next_dev) {
		int r = drm_resume(dev);
		if (r && r != -ENODEV && rc == 0)
			rc = r;
	}
	return rc;
}

static long drm_show_power_state(struct pfs_node *n, char *buf, long cap)
{
	struct drm_device *dev = n->arg;
	return ksnprintf(buf, (size_t)cap, "%s\n", dev->suspended ? "off" : "on");
}

/* "off" / "on": the whole suspend and resume path, for exercising it on
 * a machine that has no system sleep to trigger it. */
static long drm_store_power_state(struct pfs_node *n, const char *buf, long len)
{
	struct drm_device *dev = n->arg;
	int rc;

	if (len >= 3 && buf[0] == 'o' && buf[1] == 'f' && buf[2] == 'f')
		rc = drm_suspend(dev);
	else if (len >= 2 && buf[0] == 'o' && buf[1] == 'n')
		rc = drm_resume(dev);
	else
		return -EINVAL;
	return rc ? rc : len;
}

int drm_dev_register(struct drm_device *dev, const struct drm_driver *drv,
		     const pci_device_t *pci, void *priv)
{
	static int next_index;

	dev->drv = drv;
	dev->priv = priv;
	dev->pci = pci;
	dev->index = next_index++;
	spinlock_init(&dev->lock, "drm_dev");
	wq_head_init(&dev->vbl_wq, "drm_vbl");
	dev->next_mode_id = 32;
	if (pci)
		ksnprintf(dev->unique, sizeof(dev->unique), "pci:0000:%02x:%02x.%x",
			  pci->bus, pci->device, pci->function);
	else
		ksnprintf(dev->unique, sizeof(dev->unique), "platform:%s", drv->name);

	if (!g_dri_dir_registered) {
		ksnprintf(g_dri_dir.path, sizeof(g_dri_dir.path), "/dev/dri");
		g_dri_dir.mode = 0755;
		if (device_register_dir(&g_dri_dir) == 0)
			g_dri_dir_registered = 1;
	}

	struct devfs_node *n = &dev->node_card;
	ksnprintf(n->path, sizeof(n->path), "/dev/dri/card%d", dev->index);
	n->mode = 0660;
	n->uid = 0;
	n->gid = DEVFS_GID_VIDEO;
	n->major = DRM_MAJOR;
	n->minor = (uint32_t)dev->index;
	n->ops = &drm_node_ops;
	n->priv = dev;
	if (device_register(n) != 0)
		return -EEXIST;

	n = &dev->node_render;
	ksnprintf(n->path, sizeof(n->path), "/dev/dri/renderD%d",
		  DRM_RENDER_MINOR_BASE + dev->index);
	/* 0660 root:render, like the primary node is 0660 root:video.  The
	 * render node was world-readable and -writable, which made its group
	 * ownership decorative: any account could submit command buffers to
	 * the GPU and map the buffers it produced.  A client that needs
	 * off-screen rendering joins `render' now. */
	n->mode = 0660;
	n->uid = 0;
	n->gid = DEVFS_GID_RENDER;
	n->major = DRM_MAJOR;
	n->minor = (uint32_t)(DRM_RENDER_MINOR_BASE + dev->index);
	n->ops = &drm_node_ops;
	n->priv = dev;
	if (device_register(n) != 0)
		return -EEXIST;

	ksnprintf(dev->devname_card, sizeof(dev->devname_card), "dri/card%d",
		  dev->index);
	ksnprintf(dev->devname_render, sizeof(dev->devname_render),
		  "dri/renderD%d", DRM_RENDER_MINOR_BASE + dev->index);
	sysfs_add_char_device(dev->devname_card, DRM_MAJOR, (uint32_t)dev->index,
			      "drm", pci);
	/* /sys/class/drm/cardN/hotplug: a count that changes whenever a
	 * connector's status did.  No event bus carries hotplugs to clients
	 * here; a client that cares polls this and re-reads its connectors
	 * when it moved. */
	{
		char base[96];
		if (pci)
			ksnprintf(base, sizeof(base), "bus/pci/devices/0000:%02x:%02x.%x/drm/card%d",
				  pci->bus, pci->device, pci->function, dev->index);
		else
			ksnprintf(base, sizeof(base), "devices/virtual/drm/card%d", dev->index);
		sysfs_add_attr(base, "hotplug", drm_show_hotplug, NULL, dev, 0);
		/* Read it before and after a run to see whether a client left
		 * objects referenced or only left them queued: see
		 * drm_show_gem_stats(). */
		sysfs_add_attr(base, "gem_stats", drm_show_gem_stats, NULL, dev,
			       0);
		if (drv->suspend && drv->resume && drv->atomic_commit)
			sysfs_add_attr(base, "power_state", drm_show_power_state,
				       drm_store_power_state, dev, 0);
	}
	sysfs_add_char_device(dev->devname_render, DRM_MAJOR,
			      (uint32_t)(DRM_RENDER_MINOR_BASE + dev->index),
			      "drm", pci);

	drm_kms_init(dev);
	/* Object teardown runs here, not in a client's ioctl.  See
	 * drm_gem_reap_start(). */
	drm_gem_reap_start(dev);
	dev->next_dev = g_drm_devices;
	g_drm_devices = dev;
	if (pci)
		sysfs_pci_set_driver(pci, drv->name);
	kprintf("[drm] %s: /dev/dri/card%d + renderD%d (%s), version %d.%d.%d\n",
		drv->name, dev->index, DRM_RENDER_MINOR_BASE + dev->index,
		dev->unique, drv->major, drv->minor, drv->patch);
	return 0;
}
