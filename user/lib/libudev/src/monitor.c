// libudev - the hotplug monitor, on /dev/devevents.
//
// The kernel publishes a 128-byte record (struct device_event,
// <sys/devevent.h>) whenever a device appears or goes away; for input event
// nodes the record names the node ("event6").  The monitor's descriptor is
// that device file, so it can be polled like any udev monitor socket, and
// udev_monitor_receive_device() turns each record about an input node into
// a device with an action ("add", "remove", "change").
//
// Two details make the result consistent with what an enumeration returned:
//
//   - A "remove" carries the syspath the node had when it was added, kept in
//     a table from the monitor's creation onward (the kernel's description
//     is gone by the time the record is read).
//   - When the kernel reports that records were lost (an overflow record),
//     the monitor compares its table with the devices that exist now and
//     delivers the difference as remove and add events.
//
// /dev/devevents is root's.  For anyone else the monitor is created on an
// idle descriptor that never becomes readable: enumeration still works, only
// hotplug notifications are not delivered.  (Such a caller could not open
// the input nodes either, unless it is in the input group.)
//
// Records about other kinds of device (disks, USB, PCI) are not translated;
// a filter on another subsystem therefore receives nothing.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/devevent.h>
#include <sys/eventfd.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "ludev.h"

struct mon_node {
	struct mon_node *next;
	int event;
	char *syspath;
};

// A synthesized event, queued after an overflow.
struct mon_pending {
	struct mon_pending *next;
	bool add;
	int event;
	char *syspath; // for a remove
};

struct udev_monitor {
	int refcount;
	struct udev *udev;
	int fd;
	bool devevents; // fd is /dev/devevents (else an idle eventfd)
	bool receiving;
	struct ludev_list subsystems; // name = subsystem, value = devtype
	struct ludev_list tags;
	struct mon_node *known;
	struct mon_pending *pending, *pending_tail;
};

// ---------------------------------------------------------------------------
// The table of known event nodes
// ---------------------------------------------------------------------------

static struct mon_node *known_find(struct udev_monitor *m, int event)
{
	for (struct mon_node *n = m->known; n; n = n->next)
		if (n->event == event)
			return n;
	return NULL;
}

static void known_remove(struct udev_monitor *m, int event)
{
	struct mon_node **pp = &m->known;

	while (*pp) {
		struct mon_node *n = *pp;

		if (n->event == event) {
			*pp = n->next;
			free(n->syspath);
			free(n);
			return;
		}
		pp = &n->next;
	}
}

static int known_set(struct udev_monitor *m, int event, const char *syspath)
{
	struct mon_node *n = known_find(m, event);
	char *s = ludev_strdup_bounded(syspath);

	if (!s)
		return -ENOMEM;
	if (n) {
		free(n->syspath);
		n->syspath = s;
		return 0;
	}
	n = calloc(1, sizeof(*n));
	if (!n) {
		free(s);
		return -ENOMEM;
	}
	n->event = event;
	n->syspath = s;
	n->next = m->known;
	m->known = n;
	return 0;
}

static void known_clear(struct udev_monitor *m)
{
	while (m->known) {
		struct mon_node *n = m->known;

		m->known = n->next;
		free(n->syspath);
		free(n);
	}
}

static void pending_clear(struct udev_monitor *m)
{
	while (m->pending) {
		struct mon_pending *p = m->pending;

		m->pending = p->next;
		free(p->syspath);
		free(p);
	}
	m->pending_tail = NULL;
}

static int pending_push(struct udev_monitor *m, bool add, int event,
			const char *syspath)
{
	struct mon_pending *p = calloc(1, sizeof(*p));

	if (!p)
		return -ENOMEM;
	p->add = add;
	p->event = event;
	if (syspath) {
		p->syspath = ludev_strdup_bounded(syspath);
		if (!p->syspath) {
			free(p);
			return -ENOMEM;
		}
	}
	if (m->pending_tail)
		m->pending_tail->next = p;
	else
		m->pending = p;
	m->pending_tail = p;
	return 0;
}

// Fill the table from the devices that exist now.
static int known_load(struct udev_monitor *m)
{
	struct ludev_input_table t;
	char path[LUDEV_PATH_MAX];
	int rc = ludev_input_load(&t);

	if (rc)
		return rc;
	for (size_t i = 0; i < t.n && !rc; i++)
		if (ludev_event_syspath(&t.v[i], path, sizeof(path)))
			rc = known_set(m, t.v[i].event, path);
	ludev_input_table_free(&t);
	return rc;
}

// Records were lost: queue the difference between the table and what
// exists, removes first.
static void resync(struct udev_monitor *m)
{
	struct ludev_input_table t;
	char path[LUDEV_PATH_MAX];

	if (ludev_input_load(&t))
		return;
	for (struct mon_node *n = m->known; n; n = n->next) {
		const struct ludev_input *in = ludev_input_find_event(&t, n->event);

		if (!in || !ludev_event_syspath(in, path, sizeof(path)) ||
		    strcmp(path, n->syspath) != 0)
			pending_push(m, false, n->event, n->syspath);
	}
	for (size_t i = 0; i < t.n; i++) {
		struct mon_node *n;

		if (!ludev_event_syspath(&t.v[i], path, sizeof(path)))
			continue;
		n = known_find(m, t.v[i].event);
		if (!n || strcmp(n->syspath, path) != 0)
			pending_push(m, true, t.v[i].event, NULL);
	}
	ludev_input_table_free(&t);
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

static bool passes_filters(struct udev_monitor *m, const char *subsystem,
			   const char *devtype)
{
	struct udev_list_entry *e;

	// No device here carries a tag.
	if (m->tags.head)
		return false;
	if (!m->subsystems.head)
		return true;
	for (e = m->subsystems.head; e; e = e->next) {
		if (strcmp(e->name, subsystem) != 0)
			continue;
		if (!e->value || (devtype && strcmp(e->value, devtype) == 0))
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Turning a record into a device
// ---------------------------------------------------------------------------

static struct udev_device *make_added(struct udev_monitor *m, int event,
				      const char *action, unsigned long long seq)
{
	struct ludev_input_table t;
	const struct ludev_input *in;
	struct udev_device *d = NULL;
	char path[LUDEV_PATH_MAX];

	if (ludev_input_load(&t))
		return NULL;
	in = ludev_input_find_event(&t, event);
	// Gone again before the record was read: a remove follows.
	if (in && ludev_event_syspath(in, path, sizeof(path))) {
		d = ludev_device_new_event(m->udev, in);
		if (d) {
			d->action = ludev_strdup_bounded(action);
			d->seqnum = seq;
			if (!d->action || known_set(m, event, path)) {
				udev_device_unref(d);
				d = NULL;
			}
		}
	}
	ludev_input_table_free(&t);
	return d;
}

static struct udev_device *make_removed(struct udev_monitor *m, int event,
					const char *syspath, dev_t devnum,
					unsigned long long seq)
{
	char fallback[64];
	struct mon_node *n = known_find(m, event);
	struct udev_device *d;

	if (!syspath) {
		if (n) {
			syspath = n->syspath;
		} else {
			snprintf(fallback, sizeof(fallback),
				 "/sys/class/input/event%d", event);
			syspath = fallback;
		}
	}
	d = ludev_device_new_event_removed(m->udev, event, syspath, devnum);
	if (d) {
		d->action = ludev_strdup_bounded("remove");
		d->seqnum = seq;
		if (!d->action) {
			udev_device_unref(d);
			return NULL;
		}
	}
	known_remove(m, event);
	return d;
}

LUDEV_EXPORT struct udev_device *udev_monitor_receive_device(struct udev_monitor *udev_monitor)
{
	struct udev_monitor *m = udev_monitor;

	if (!m) {
		errno = EINVAL;
		return NULL;
	}
	if (!passes_filters(m, "input", NULL)) {
		// Nothing this monitor can deliver is wanted; still drain
		// what is queued so the descriptor stops polling readable.
		pending_clear(m);
		if (m->devevents) {
			struct device_event ev;

			while (read(m->fd, &ev, sizeof(ev)) > 0)
				;
		}
		errno = EAGAIN;
		return NULL;
	}

	for (;;) {
		struct device_event ev;
		struct udev_device *d;
		ssize_t n;
		int event;

		while (m->pending) {
			struct mon_pending *p = m->pending;

			m->pending = p->next;
			if (!m->pending)
				m->pending_tail = NULL;
			d = p->add ? make_added(m, p->event, "add", 0) :
				     make_removed(m, p->event, p->syspath, 0, 0);
			free(p->syspath);
			free(p);
			if (d)
				return d;
		}

		if (!m->devevents) {
			errno = EAGAIN;
			return NULL;
		}
		n = read(m->fd, &ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return NULL;
		}
		if (n != (ssize_t)sizeof(ev) || ev.size != sizeof(ev) ||
		    ev.version != DEVEVENT_VERSION) {
			if (n == 0) {
				errno = EAGAIN;
				return NULL;
			}
			continue; // not a record this library understands
		}
		if (ev.type == DEVICE_EVENT_OVERFLOW) {
			resync(m);
			continue;
		}
		if (ev.device_class != DEVICE_CLASS_INPUT)
			continue;
		ev.name[sizeof(ev.name) - 1] = '\0';
		event = ludev_parse_event_name(ev.name);
		if (event < 0)
			continue;
		switch (ev.type) {
		case DEVICE_EVENT_ADD:
			d = make_added(m, event, "add", ev.seq);
			break;
		case DEVICE_EVENT_CHANGE:
			d = make_added(m, event, "change", ev.seq);
			break;
		case DEVICE_EVENT_REMOVE:
			d = make_removed(m, event, NULL,
					 ev.major ? makedev(ev.major, ev.minor) : 0,
					 ev.seq);
			break;
		default:
			d = NULL;
			break;
		}
		if (d)
			return d;
	}
}

// ---------------------------------------------------------------------------
// The object
// ---------------------------------------------------------------------------

LUDEV_EXPORT struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev,
								 const char *name)
{
	struct udev_monitor *m;

	if (!udev || (name && strcmp(name, "udev") != 0 && strcmp(name, "kernel") != 0)) {
		errno = EINVAL;
		return NULL;
	}
	m = calloc(1, sizeof(*m));
	if (!m) {
		errno = ENOMEM;
		return NULL;
	}
	m->refcount = 1;
	m->fd = open(ludev_paths.devevents, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (m->fd >= 0) {
		m->devevents = true;
	} else {
		m->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (m->fd < 0) {
			int err = errno;

			free(m);
			errno = err;
			return NULL;
		}
	}
	// What exists already, so a later remove names the right device.  A
	// table that cannot be read starts empty.
	if (known_load(m) == -ENOMEM) {
		known_clear(m);
		close(m->fd);
		free(m);
		errno = ENOMEM;
		return NULL;
	}
	m->udev = udev_ref(udev);
	return m;
}

LUDEV_EXPORT struct udev_monitor *udev_monitor_ref(struct udev_monitor *udev_monitor)
{
	if (udev_monitor)
		udev_monitor->refcount++;
	return udev_monitor;
}

LUDEV_EXPORT struct udev_monitor *udev_monitor_unref(struct udev_monitor *udev_monitor)
{
	struct udev_monitor *m = udev_monitor;

	if (!m)
		return NULL;
	if (--m->refcount > 0)
		return NULL;
	if (m->fd >= 0)
		close(m->fd);
	ludev_list_clear(&m->subsystems);
	ludev_list_clear(&m->tags);
	known_clear(m);
	pending_clear(m);
	udev_unref(m->udev);
	free(m);
	return NULL;
}

LUDEV_EXPORT struct udev *udev_monitor_get_udev(struct udev_monitor *udev_monitor)
{
	return udev_monitor ? udev_monitor->udev : NULL;
}

LUDEV_EXPORT int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor)
{
	if (!udev_monitor)
		return -EINVAL;
	udev_monitor->receiving = true;
	return 0;
}

// The kernel keeps its own queue of records; there is no buffer to size.
LUDEV_EXPORT int udev_monitor_set_receive_buffer_size(struct udev_monitor *udev_monitor,
						      int size)
{
	if (!udev_monitor || size <= 0)
		return -EINVAL;
	return 0;
}

LUDEV_EXPORT int udev_monitor_get_fd(struct udev_monitor *udev_monitor)
{
	if (!udev_monitor)
		return -EINVAL;
	return udev_monitor->fd;
}

LUDEV_EXPORT int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *udev_monitor,
								 const char *subsystem,
								 const char *devtype)
{
	if (!udev_monitor || !subsystem)
		return -EINVAL;
	return ludev_list_set(&udev_monitor->subsystems, subsystem, devtype, false);
}

LUDEV_EXPORT int udev_monitor_filter_add_match_tag(struct udev_monitor *udev_monitor,
						   const char *tag)
{
	if (!udev_monitor || !tag)
		return -EINVAL;
	return ludev_list_set(&udev_monitor->tags, tag, NULL, false);
}

// Filters are applied as records are read; nothing to install.
LUDEV_EXPORT int udev_monitor_filter_update(struct udev_monitor *udev_monitor)
{
	return udev_monitor ? 0 : -EINVAL;
}

LUDEV_EXPORT int udev_monitor_filter_remove(struct udev_monitor *udev_monitor)
{
	if (!udev_monitor)
		return -EINVAL;
	ludev_list_clear(&udev_monitor->subsystems);
	ludev_list_clear(&udev_monitor->tags);
	return 0;
}
