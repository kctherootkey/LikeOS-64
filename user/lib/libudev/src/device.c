// libudev - devices.
//
// Three kinds of device are modelled:
//
//   event   /dev/input/eventN, syspath  /sys<Sysfs>/eventN
//   input   its input device,  syspath  /sys<Sysfs>         (sysname inputM)
//   sysfs   any other /sys directory, described by its uevent file
//
// <Sysfs> is the S: line of /proc/bus/input/devices.  The event device is the
// child of the input device, as in the kernel's device tree; both carry the
// ID_INPUT_* tags.  The input device carries the identity (PRODUCT, NAME,
// PHYS, UNIQ) and the capability bitmaps as properties, and answers the
// usual attributes (name, phys, id/vendor, capabilities/key ...).
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "ludev.h"

#define EVDEV_MAJOR_NUMBER 13u
#define EVDEV_MINOR_FIRST 64u

// Longest uevent or attribute file read.
#define SYSFS_FILE_MAX 65536u

// ---------------------------------------------------------------------------
// Construction helpers
// ---------------------------------------------------------------------------

static struct udev_device *device_alloc(struct udev *udev, enum ludev_kind kind)
{
	struct udev_device *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->refcount = 1;
	d->udev = udev_ref(udev);
	d->kind = kind;
	return d;
}

static void device_free(struct udev_device *d)
{
	free(d->syspath);
	free(d->devpath);
	free(d->sysname);
	free(d->sysnum);
	free(d->subsystem);
	free(d->devtype);
	free(d->devnode);
	free(d->driver);
	free(d->action);
	ludev_list_clear(&d->properties);
	ludev_list_clear(&d->sysattrs);
	ludev_list_clear(&d->sysattr_names);
	if (d->parent)
		udev_device_unref(d->parent);
	if (d->input) {
		free(d->input->name);
		free(d->input->phys);
		free(d->input->uniq);
		free(d->input->sysfs);
		for (int i = 0; i < LUDEV_CAP_COUNT; i++)
			free(d->input->caps_text[i]);
		free(d->input);
	}
	udev_unref(d->udev);
	free(d);
}

static struct ludev_input *input_clone(const struct ludev_input *in)
{
	struct ludev_input *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	*c = *in;
	c->name = ludev_strdup_bounded(in->name);
	c->phys = ludev_strdup_bounded(in->phys);
	c->uniq = ludev_strdup_bounded(in->uniq);
	c->sysfs = ludev_strdup_bounded(in->sysfs);
	for (int i = 0; i < LUDEV_CAP_COUNT; i++)
		c->caps_text[i] = in->caps_text[i] ?
					  ludev_strdup_bounded(in->caps_text[i]) :
					  NULL;
	bool ok = c->name && c->phys && c->uniq && c->sysfs;

	for (int i = 0; i < LUDEV_CAP_COUNT; i++)
		if (in->caps_text[i] && !c->caps_text[i])
			ok = false;
	if (!ok) {
		free(c->name);
		free(c->phys);
		free(c->uniq);
		free(c->sysfs);
		for (int i = 0; i < LUDEV_CAP_COUNT; i++)
			free(c->caps_text[i]);
		free(c);
		return NULL;
	}
	return c;
}

static int set_prop(struct udev_device *d, const char *name, const char *value)
{
	return ludev_list_set(&d->properties, name, value, true);
}

static int set_prop_fmt(struct udev_device *d, const char *name,
			const char *fmt, unsigned int v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), fmt, v);
	return set_prop(d, name, buf);
}

// The last path component, and the digits it ends in.
static int set_names_from_syspath(struct udev_device *d)
{
	const char *base = strrchr(d->syspath, '/');
	const char *num;

	base = base ? base + 1 : d->syspath;
	d->sysname = ludev_strdup_bounded(base);
	if (!d->sysname)
		return -ENOMEM;
	num = base + strlen(base);
	while (num > base && num[-1] >= '0' && num[-1] <= '9')
		num--;
	if (*num) {
		d->sysnum = ludev_strdup_bounded(num);
		if (!d->sysnum)
			return -ENOMEM;
	}
	d->devpath = ludev_strdup_bounded(d->syspath + 4); // after "/sys"
	return d->devpath ? 0 : -ENOMEM;
}

bool ludev_input_syspath(const struct ludev_input *in, char *buf, size_t size)
{
	int n = snprintf(buf, size, "/sys%s", in->sysfs);

	return n > 0 && (size_t)n < size;
}

bool ludev_event_syspath(const struct ludev_input *in, char *buf, size_t size)
{
	int n;

	if (in->event < 0)
		return false;
	n = snprintf(buf, size, "/sys%s/event%d", in->sysfs, in->event);
	return n > 0 && (size_t)n < size;
}

// Where the node of event N is opened and stat'ed.  On the system that is
// the device node itself; a host test points it elsewhere.
static bool event_node_path(int event, char *buf, size_t size)
{
	int n = snprintf(buf, size, "%s/event%d", ludev_paths.dev_input, event);

	return n > 0 && (size_t)n < size;
}

static dev_t event_devnum(int event)
{
	char path[LUDEV_PATH_MAX];
	struct stat st;

	if (event_node_path(event, path, sizeof(path)) && stat(path, &st) == 0 &&
	    S_ISCHR(st.st_mode))
		return st.st_rdev;
	// The node numbers the kernel assigns (see <sys/devevent.h>).
	return makedev(EVDEV_MAJOR_NUMBER, EVDEV_MINOR_FIRST + (unsigned int)event);
}

// ID_INPUT_WIDTH_MM / ID_INPUT_HEIGHT_MM: the size of the surface, from the
// absolute axes' range and resolution (units per millimetre).  Needs the
// node; quietly left out when it cannot be opened (no permission) or the
// axes have no resolution.
//
// Only when they are asked for (ludev_device_resolve_size), never when the
// device is built: opening an event node is not free -- the kernel's console
// pointer bridge steps back for as long as a user-space client has a
// touchpad open, and lets go of a click or drag it was holding.  A program
// that only enumerates devices or watches for new ones (likeos-input-devices
// does both on every hotplug event) must not do that to every touchpad
// each time.
static const char *const size_props[] = { "ID_INPUT_WIDTH_MM",
					  "ID_INPUT_HEIGHT_MM" };

static int add_size_props(struct udev_device *d, const struct ludev_input *in)
{
	struct input_absinfo ax, ay;
	char path[LUDEV_PATH_MAX];
	int fd, ok;

	if (!ludev_test_bit(in, LUDEV_CAP_ABS, ABS_X) ||
	    !ludev_test_bit(in, LUDEV_CAP_ABS, ABS_Y))
		return 0;
	if (!event_node_path(in->event, path, sizeof(path)))
		return 0;
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
	if (fd < 0)
		return 0;
#ifdef LUDEV_TESTING
	ludev_test_size_opens++;
#endif
	memset(&ax, 0, sizeof(ax));
	memset(&ay, 0, sizeof(ay));
	ok = ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 &&
	     ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0;
	close(fd);
	if (!ok || ax.resolution <= 0 || ay.resolution <= 0)
		return 0;
	if (set_prop_fmt(d, size_props[0], "%u",
			 (unsigned int)((ax.maximum - ax.minimum) / ax.resolution)) ||
	    set_prop_fmt(d, size_props[1], "%u",
			 (unsigned int)((ay.maximum - ay.minimum) / ay.resolution)))
		return -ENOMEM;
	return 0;
}

int ludev_device_resolve_size(struct udev_device *d)
{
	if (!d || !d->size_pending)
		return 0;
	d->size_pending = false;
	return add_size_props(d, d->input);
}

bool ludev_pattern_matches_size(const char *pattern)
{
	for (size_t i = 0; i < sizeof(size_props) / sizeof(size_props[0]); i++)
		if (pattern && fnmatch(pattern, size_props[i], 0) == 0)
			return true;
	return false;
}

// The tags and identity both kinds of input device carry.
static int add_id_props(struct udev_device *d, const struct ludev_input *in)
{
	const char *bus = ludev_bus_name(in->bustype);

	for (size_t i = 0; i < ludev_id_names_count; i++)
		if ((d->id_flags & ludev_id_names[i].flag) &&
		    set_prop(d, ludev_id_names[i].property, "1"))
			return -ENOMEM;
	if (bus && set_prop(d, "ID_BUS", bus))
		return -ENOMEM;
	if (in->vendor || in->product) {
		if (set_prop_fmt(d, "ID_VENDOR_ID", "%04x", in->vendor) ||
		    set_prop_fmt(d, "ID_MODEL_ID", "%04x", in->product))
			return -ENOMEM;
	}
	return 0;
}

struct udev_device *ludev_device_new_event(struct udev *udev,
					   const struct ludev_input *in)
{
	char path[LUDEV_PATH_MAX], group[LUDEV_PATH_MAX];
	struct udev_device *d;

	if (!udev || !in || in->event < 0) {
		errno = EINVAL;
		return NULL;
	}
	if (!ludev_event_syspath(in, path, sizeof(path))) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	d = device_alloc(udev, LUDEV_KIND_EVENT);
	if (!d)
		goto nomem;
	d->input = input_clone(in);
	d->syspath = ludev_strdup_bounded(path);
	d->subsystem = ludev_strdup_bounded("input");
	snprintf(path, sizeof(path), "/dev/input/event%d", in->event);
	d->devnode = ludev_strdup_bounded(path);
	if (!d->input || !d->syspath || !d->subsystem || !d->devnode ||
	    set_names_from_syspath(d))
		goto nomem;
	d->devnum = event_devnum(in->event);
	d->id_flags = ludev_input_classify(in);

	if (set_prop(d, "DEVPATH", d->devpath) || set_prop(d, "DEVNAME", d->devnode) ||
	    set_prop_fmt(d, "MAJOR", "%u", major(d->devnum)) ||
	    set_prop_fmt(d, "MINOR", "%u", minor(d->devnum)) ||
	    set_prop(d, "SUBSYSTEM", "input") || add_id_props(d, in))
		goto nomem;
	// The size: on demand (add_size_props).
	d->size_pending = true;
	// Which input devices are one physical device (a touchpad and the
	// pointing stick on the same module, say).
	if (ludev_device_group(in, group, sizeof(group)) &&
	    set_prop(d, "LIBINPUT_DEVICE_GROUP", group))
		goto nomem;
	return d;

nomem:
	if (d)
		device_free(d);
	errno = ENOMEM;
	return NULL;
}

struct udev_device *ludev_device_new_input(struct udev *udev,
					   const struct ludev_input *in)
{
	char path[LUDEV_PATH_MAX], product[64];
	struct udev_device *d;

	if (!udev || !in) {
		errno = EINVAL;
		return NULL;
	}
	if (!ludev_input_syspath(in, path, sizeof(path))) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	d = device_alloc(udev, LUDEV_KIND_INPUT);
	if (!d)
		goto nomem;
	d->input = input_clone(in);
	d->syspath = ludev_strdup_bounded(path);
	d->subsystem = ludev_strdup_bounded("input");
	if (!d->input || !d->syspath || !d->subsystem || set_names_from_syspath(d))
		goto nomem;
	d->id_flags = ludev_input_classify(in);

	snprintf(product, sizeof(product), "%x/%x/%x/%x", in->bustype, in->vendor,
		 in->product, in->version);
	if (set_prop(d, "DEVPATH", d->devpath) || set_prop(d, "SUBSYSTEM", "input") ||
	    set_prop(d, "PRODUCT", product))
		goto nomem;
	// NAME, PHYS and UNIQ are quoted, as the kernel publishes them.
	{
		static const char *const keys[] = { "NAME", "PHYS", "UNIQ" };
		const char *vals[] = { in->name, in->phys, in->uniq };

		for (int i = 0; i < 3; i++) {
			size_t n = strlen(vals[i]);
			char *q = malloc(n + 3);
			int rc;

			if (!q)
				goto nomem;
			q[0] = '"';
			memcpy(q + 1, vals[i], n);
			q[n + 1] = '"';
			q[n + 2] = '\0';
			rc = set_prop(d, keys[i], q);
			free(q);
			if (rc)
				goto nomem;
		}
	}
	for (int c = 0; c < LUDEV_CAP_COUNT; c++)
		if (in->caps_text[c] &&
		    set_prop(d, ludev_cap_name((enum ludev_cap)c), in->caps_text[c]))
			goto nomem;
	if (add_id_props(d, in))
		goto nomem;
	return d;

nomem:
	if (d)
		device_free(d);
	errno = ENOMEM;
	return NULL;
}

struct udev_device *ludev_device_new_event_removed(struct udev *udev, int event,
						   const char *syspath,
						   dev_t devnum)
{
	char path[64];
	struct udev_device *d;

	if (!udev || event < 0 || !syspath || strncmp(syspath, "/sys/", 5) != 0) {
		errno = EINVAL;
		return NULL;
	}
	d = device_alloc(udev, LUDEV_KIND_EVENT);
	if (!d)
		goto nomem;
	d->syspath = ludev_strdup_bounded(syspath);
	d->subsystem = ludev_strdup_bounded("input");
	snprintf(path, sizeof(path), "/dev/input/event%d", event);
	d->devnode = ludev_strdup_bounded(path);
	if (!d->syspath || !d->subsystem || !d->devnode || set_names_from_syspath(d))
		goto nomem;
	d->devnum = devnum ? devnum :
			     makedev(EVDEV_MAJOR_NUMBER,
				     EVDEV_MINOR_FIRST + (unsigned int)event);
	d->parent_done = true; // nothing left to describe it by
	if (set_prop(d, "DEVPATH", d->devpath) || set_prop(d, "DEVNAME", d->devnode) ||
	    set_prop_fmt(d, "MAJOR", "%u", major(d->devnum)) ||
	    set_prop_fmt(d, "MINOR", "%u", minor(d->devnum)) ||
	    set_prop(d, "SUBSYSTEM", "input"))
		goto nomem;
	return d;

nomem:
	if (d)
		device_free(d);
	errno = ENOMEM;
	return NULL;
}

// ---------------------------------------------------------------------------
// /sys directories
// ---------------------------------------------------------------------------

// The file system path of a /sys path (the same on the system; a host test
// moves the root).
static bool sys_fs_path(const char *syspath, const char *rel, char *buf,
			size_t size)
{
	int n;

	if (strncmp(syspath, "/sys", 4) != 0 || (syspath[4] && syspath[4] != '/'))
		return false;
	n = snprintf(buf, size, "%s%s%s%s", ludev_paths.sys, syspath + 4,
		     rel ? "/" : "", rel ? rel : "");
	return n > 0 && (size_t)n < size;
}

// A syspath this library accepts: absolute under /sys, no "." or ".."
// component, no empty component; trailing slashes are removed.
static int normalize_syspath(const char *in, char *out, size_t size)
{
	size_t n = strnlen(in, LUDEV_PATH_MAX);
	const char *p;

	if (n >= LUDEV_PATH_MAX || n >= size)
		return -ENAMETOOLONG;
	if (strncmp(in, "/sys/", 5) != 0)
		return -EINVAL;
	memcpy(out, in, n + 1);
	while (n > 5 && out[n - 1] == '/')
		out[--n] = '\0';
	for (p = out + 1; *p;) {
		const char *s = strchr(p, '/');
		size_t len = s ? (size_t)(s - p) : strlen(p);

		if (len == 0 || (len == 1 && p[0] == '.') ||
		    (len == 2 && p[0] == '.' && p[1] == '.'))
			return -EINVAL;
		p += len;
		if (*p == '/')
			p++;
	}
	return 0;
}

static int read_sys_value(const char *syspath, const char *rel, char **out)
{
	char path[LUDEV_PATH_MAX];
	struct stat st;
	size_t len;
	int rc;

	*out = NULL;
	if (!sys_fs_path(syspath, rel, path, sizeof(path)))
		return -ENAMETOOLONG;
	if (stat(path, &st) != 0)
		return -errno;
	if (!S_ISREG(st.st_mode))
		return -EISDIR;
	rc = ludev_read_file(path, out, &len, SYSFS_FILE_MAX);
	if (rc)
		return rc;
	while (len > 0 && ((*out)[len - 1] == '\n' || (*out)[len - 1] == ' ' ||
			   (*out)[len - 1] == '\t' || (*out)[len - 1] == '\r'))
		(*out)[--len] = '\0';
	return 0;
}

static int parse_uevent(struct udev_device *d)
{
	char *text = NULL, *line, *save = NULL;
	unsigned int maj = 0, min = 0;
	bool have_maj = false, have_min = false;
	int rc = read_sys_value(d->syspath, "uevent", &text);

	if (rc)
		return rc == -ENOMEM ? rc : 0; // no uevent: no properties
	for (line = strtok_r(text, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '=');

		if (!eq || eq == line)
			continue;
		*eq = '\0';
		const char *k = line, *v = eq + 1;

		if (strcmp(k, "DEVTYPE") == 0) {
			free(d->devtype);
			d->devtype = ludev_strdup_bounded(v);
		} else if (strcmp(k, "DRIVER") == 0) {
			free(d->driver);
			d->driver = ludev_strdup_bounded(v);
		} else if (strcmp(k, "DEVNAME") == 0) {
			char node[LUDEV_PATH_MAX];

			if (strstr(v, "..") == NULL) {
				snprintf(node, sizeof(node), "%s%s",
					 v[0] == '/' ? "" : "/dev/", v);
				free(d->devnode);
				d->devnode = ludev_strdup_bounded(node);
				v = d->devnode ? d->devnode : v;
			}
		} else if (strcmp(k, "MAJOR") == 0) {
			maj = (unsigned int)strtoul(v, NULL, 10);
			have_maj = true;
		} else if (strcmp(k, "MINOR") == 0) {
			min = (unsigned int)strtoul(v, NULL, 10);
			have_min = true;
		}
		if (set_prop(d, k, v)) {
			free(text);
			return -ENOMEM;
		}
	}
	free(text);
	if (have_maj && have_min)
		d->devnum = makedev(maj, min);
	return 0;
}

struct udev_device *ludev_device_new_sysfs(struct udev *udev, const char *syspath)
{
	char norm[LUDEV_PATH_MAX], path[LUDEV_PATH_MAX], link[LUDEV_PATH_MAX];
	struct udev_device *d;
	struct stat st;
	int rc;
	ssize_t n;

	rc = normalize_syspath(syspath, norm, sizeof(norm));
	if (rc) {
		errno = -rc;
		return NULL;
	}
	if (!sys_fs_path(norm, NULL, path, sizeof(path))) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	if (stat(path, &st) != 0)
		return NULL; // errno from stat
	if (!S_ISDIR(st.st_mode)) {
		errno = ENODEV;
		return NULL;
	}
	d = device_alloc(udev, LUDEV_KIND_SYSFS);
	if (!d)
		goto nomem;
	d->syspath = ludev_strdup_bounded(norm);
	if (!d->syspath || set_names_from_syspath(d))
		goto nomem;
	// The subsystem is the name of what the "subsystem" link points at.
	if (sys_fs_path(norm, "subsystem", path, sizeof(path))) {
		n = readlink(path, link, sizeof(link) - 1);
		if (n > 0) {
			const char *b;

			link[n] = '\0';
			b = strrchr(link, '/');
			d->subsystem = ludev_strdup_bounded(b ? b + 1 : link);
			if (!d->subsystem)
				goto nomem;
		}
	}
	if (set_prop(d, "DEVPATH", d->devpath))
		goto nomem;
	if (d->subsystem && set_prop(d, "SUBSYSTEM", d->subsystem))
		goto nomem;
	if (parse_uevent(d))
		goto nomem;
	return d;

nomem:
	if (d)
		device_free(d);
	errno = ENOMEM;
	return NULL;
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

// The input device whose sysname is `name' ("input10").
static const struct ludev_input *find_input_sysname(const struct ludev_input_table *t,
						    const char *name)
{
	for (size_t i = 0; i < t->n; i++) {
		const char *b = strrchr(t->v[i].sysfs, '/');

		b = b ? b + 1 : t->v[i].sysfs;
		if (strcmp(b, name) == 0)
			return &t->v[i];
	}
	return NULL;
}

static struct udev_device *event_device_by_number(struct udev *udev, int event)
{
	struct ludev_input_table t;
	const struct ludev_input *in;
	struct udev_device *d;
	int rc = ludev_input_load(&t);

	if (rc) {
		errno = -rc;
		return NULL;
	}
	in = ludev_input_find_event(&t, event);
	if (!in) {
		ludev_input_table_free(&t);
		errno = ENODEV;
		return NULL;
	}
	d = ludev_device_new_event(udev, in);
	rc = errno;
	ludev_input_table_free(&t);
	errno = rc;
	return d;
}

LUDEV_EXPORT struct udev_device *udev_device_new_from_syspath(struct udev *udev,
							       const char *syspath)
{
	char norm[LUDEV_PATH_MAX], buf[LUDEV_PATH_MAX];
	struct ludev_input_table t;
	struct udev_device *d = NULL;
	int rc;

	if (!udev || !syspath) {
		errno = EINVAL;
		return NULL;
	}
	rc = normalize_syspath(syspath, norm, sizeof(norm));
	if (rc) {
		errno = -rc;
		return NULL;
	}

	// The class directory names.
	if (strncmp(norm, "/sys/class/input/", 17) == 0) {
		const char *name = norm + 17;

		if (strchr(name, '/') == NULL) {
			int ev = ludev_parse_event_name(name);

			if (ev >= 0)
				return event_device_by_number(udev, ev);
			if (strncmp(name, "input", 5) == 0) {
				const struct ludev_input *in;

				rc = ludev_input_load(&t);
				if (rc) {
					errno = -rc;
					return NULL;
				}
				in = find_input_sysname(&t, name);
				d = in ? ludev_device_new_input(udev, in) : NULL;
				rc = in ? errno : ENODEV;
				ludev_input_table_free(&t);
				errno = rc;
				return d;
			}
		}
	}

	// The input devices' own paths.
	if (ludev_input_load(&t) == 0) {
		for (size_t i = 0; i < t.n && !d; i++) {
			const struct ludev_input *in = &t.v[i];

			if (ludev_input_syspath(in, buf, sizeof(buf)) &&
			    strcmp(buf, norm) == 0) {
				d = ludev_device_new_input(udev, in);
				rc = errno;
				ludev_input_table_free(&t);
				errno = rc;
				return d;
			}
			if (ludev_event_syspath(in, buf, sizeof(buf)) &&
			    strcmp(buf, norm) == 0) {
				d = ludev_device_new_event(udev, in);
				rc = errno;
				ludev_input_table_free(&t);
				errno = rc;
				return d;
			}
		}
		ludev_input_table_free(&t);
	}

	return ludev_device_new_sysfs(udev, norm);
}

LUDEV_EXPORT struct udev_device *udev_device_new_from_devnum(struct udev *udev,
							      char type,
							      dev_t devnum)
{
	DIR *dir;
	struct dirent *de;
	int found = -1;

	if (!udev || (type != 'c' && type != 'b')) {
		errno = EINVAL;
		return NULL;
	}
	// Input nodes are the only character devices described here.
	if (type != 'c') {
		errno = ENOENT;
		return NULL;
	}
	dir = opendir(ludev_paths.dev_input);
	if (!dir)
		return NULL;
	while ((de = readdir(dir)) != NULL && found < 0) {
		char path[LUDEV_PATH_MAX];
		struct stat st;
		int ev = ludev_parse_event_name(de->d_name);

		if (ev < 0 || !event_node_path(ev, path, sizeof(path)))
			continue;
		if (stat(path, &st) != 0)
			continue;
#ifdef LUDEV_TESTING
		if (ludev_test_any_node_type) {
			if (makedev(EVDEV_MAJOR_NUMBER,
				    EVDEV_MINOR_FIRST + (unsigned int)ev) == devnum)
				found = ev;
			continue;
		}
#endif
		if (S_ISCHR(st.st_mode) && st.st_rdev == devnum)
			found = ev;
	}
	closedir(dir);
	if (found < 0) {
		errno = ENOENT;
		return NULL;
	}
	return event_device_by_number(udev, found);
}

LUDEV_EXPORT struct udev_device *
udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
				       const char *sysname)
{
	char path[LUDEV_PATH_MAX];
	int n;

	if (!udev || !subsystem || !sysname || !*subsystem || !*sysname ||
	    strchr(subsystem, '/') || strchr(sysname, '/')) {
		errno = EINVAL;
		return NULL;
	}
	if (strcmp(subsystem, "input") == 0) {
		n = snprintf(path, sizeof(path), "/sys/class/input/%s", sysname);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			errno = ENAMETOOLONG;
			return NULL;
		}
		return udev_device_new_from_syspath(udev, path);
	}
	n = snprintf(path, sizeof(path), "/sys/class/%s/%s", subsystem, sysname);
	if (n > 0 && (size_t)n < sizeof(path)) {
		struct udev_device *d = ludev_device_new_sysfs(udev, path);

		if (d)
			return d;
	}
	n = snprintf(path, sizeof(path), "/sys/bus/%s/devices/%s", subsystem, sysname);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	return ludev_device_new_sysfs(udev, path);
}

// ---------------------------------------------------------------------------
// References and getters
// ---------------------------------------------------------------------------

LUDEV_EXPORT struct udev_device *udev_device_ref(struct udev_device *udev_device)
{
	if (udev_device)
		udev_device->refcount++;
	return udev_device;
}

LUDEV_EXPORT struct udev_device *udev_device_unref(struct udev_device *udev_device)
{
	if (!udev_device)
		return NULL;
	if (--udev_device->refcount > 0)
		return NULL;
	device_free(udev_device);
	return NULL;
}

LUDEV_EXPORT struct udev *udev_device_get_udev(struct udev_device *udev_device)
{
	return udev_device ? udev_device->udev : NULL;
}

LUDEV_EXPORT struct udev_device *udev_device_get_parent(struct udev_device *udev_device)
{
	struct udev_device *d = udev_device;

	if (!d) {
		errno = EINVAL;
		return NULL;
	}
	if (d->parent_done) {
		if (!d->parent)
			errno = ENOENT;
		return d->parent;
	}
	d->parent_done = true;
	switch (d->kind) {
	case LUDEV_KIND_EVENT:
		if (d->input)
			d->parent = ludev_device_new_input(d->udev, d->input);
		break;
	case LUDEV_KIND_INPUT:
		// The bus devices above are not described.
		break;
	case LUDEV_KIND_SYSFS: {
		char up[LUDEV_PATH_MAX], path[LUDEV_PATH_MAX];
		struct stat st;

		snprintf(up, sizeof(up), "%s", d->syspath);
		// Walk up through /sys/devices to the nearest directory
		// that is a device (has a uevent file).
		for (;;) {
			char *slash = strrchr(up, '/');

			if (!slash || (size_t)(slash - up) <= strlen("/sys/devices"))
				break;
			*slash = '\0';
			if (strncmp(up, "/sys/devices/", 13) != 0)
				break;
			if (sys_fs_path(up, "uevent", path, sizeof(path)) &&
			    stat(path, &st) == 0) {
				d->parent = ludev_device_new_sysfs(d->udev, up);
				break;
			}
		}
		break;
	}
	}
	if (!d->parent)
		errno = ENOENT;
	return d->parent;
}

LUDEV_EXPORT struct udev_device *
udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device,
					      const char *subsystem,
					      const char *devtype)
{
	struct udev_device *p;

	if (!udev_device || !subsystem) {
		errno = EINVAL;
		return NULL;
	}
	for (p = udev_device_get_parent(udev_device); p;
	     p = udev_device_get_parent(p)) {
		if (!p->subsystem || strcmp(p->subsystem, subsystem) != 0)
			continue;
		if (!devtype || (p->devtype && strcmp(p->devtype, devtype) == 0))
			return p;
	}
	errno = ENOENT;
	return NULL;
}

#define GETTER(field)                                                          \
	LUDEV_EXPORT const char *udev_device_get_##field(                     \
		struct udev_device *udev_device)                               \
	{                                                                      \
		if (!udev_device) {                                            \
			errno = EINVAL;                                        \
			return NULL;                                           \
		}                                                              \
		return udev_device->field;                                     \
	}

GETTER(devpath)
GETTER(subsystem)
GETTER(devtype)
GETTER(syspath)
GETTER(sysname)
GETTER(sysnum)
GETTER(devnode)
GETTER(driver)
GETTER(action)

#undef GETTER

LUDEV_EXPORT dev_t udev_device_get_devnum(struct udev_device *udev_device)
{
	if (!udev_device) {
		errno = EINVAL;
		return makedev(0, 0);
	}
	return udev_device->devnum;
}

LUDEV_EXPORT unsigned long long int udev_device_get_seqnum(struct udev_device *udev_device)
{
	return udev_device ? udev_device->seqnum : 0;
}

LUDEV_EXPORT unsigned long long int
udev_device_get_usec_since_initialized(struct udev_device *udev_device)
{
	(void)udev_device;
	return 0;
}

// Everything is described the moment it is asked about: there is no rule
// processing that could still be under way.
LUDEV_EXPORT int udev_device_get_is_initialized(struct udev_device *udev_device)
{
	if (!udev_device) {
		errno = EINVAL;
		return -EINVAL;
	}
	return 1;
}

LUDEV_EXPORT struct udev_list_entry *
udev_device_get_devlinks_list_entry(struct udev_device *udev_device)
{
	(void)udev_device;
	return NULL;
}

LUDEV_EXPORT struct udev_list_entry *
udev_device_get_tags_list_entry(struct udev_device *udev_device)
{
	(void)udev_device;
	return NULL;
}

LUDEV_EXPORT int udev_device_has_tag(struct udev_device *udev_device, const char *tag)
{
	(void)udev_device;
	(void)tag;
	return 0;
}

LUDEV_EXPORT struct udev_list_entry *
udev_device_get_properties_list_entry(struct udev_device *udev_device)
{
	if (!udev_device)
		return NULL;
	// Every property, so the size too.  Out of memory leaves it out.
	(void)ludev_device_resolve_size(udev_device);
	return udev_device->properties.head;
}

LUDEV_EXPORT const char *udev_device_get_property_value(struct udev_device *udev_device,
							const char *key)
{
	struct udev_list_entry *e;

	if (!udev_device || !key) {
		errno = EINVAL;
		return NULL;
	}
	if (udev_device->size_pending &&
	    (strcmp(key, size_props[0]) == 0 || strcmp(key, size_props[1]) == 0) &&
	    ludev_device_resolve_size(udev_device)) {
		errno = ENOMEM;
		return NULL;
	}
	e = ludev_list_find(&udev_device->properties, key);
	if (!e) {
		errno = ENOENT;
		return NULL;
	}
	return e->value;
}

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

static const char *const input_attrs[] = {
	"name",
	"phys",
	"uniq",
	"properties",
	"id/bustype",
	"id/vendor",
	"id/product",
	"id/version",
	"capabilities/ev",
	"capabilities/key",
	"capabilities/rel",
	"capabilities/abs",
	"capabilities/msc",
	"capabilities/led",
	"capabilities/snd",
	"capabilities/ff",
	"capabilities/sw",
};

// The value of an attribute of an input device, into buf.
static bool input_attr(const struct ludev_input *in, const char *attr, char *buf,
		       size_t size)
{
	static const struct {
		const char *name;
		enum ludev_cap cap;
	} caps[] = {
		{ "properties", LUDEV_CAP_PROP },     { "capabilities/ev", LUDEV_CAP_EV },
		{ "capabilities/key", LUDEV_CAP_KEY }, { "capabilities/rel", LUDEV_CAP_REL },
		{ "capabilities/abs", LUDEV_CAP_ABS }, { "capabilities/msc", LUDEV_CAP_MSC },
		{ "capabilities/led", LUDEV_CAP_LED }, { "capabilities/snd", LUDEV_CAP_SND },
		{ "capabilities/ff", LUDEV_CAP_FF },   { "capabilities/sw", LUDEV_CAP_SW },
	};
	const char *s = NULL;
	unsigned int v = 0;
	bool hex = false;

	if (strcmp(attr, "name") == 0)
		s = in->name;
	else if (strcmp(attr, "phys") == 0)
		s = in->phys;
	else if (strcmp(attr, "uniq") == 0)
		s = in->uniq;
	else if (strcmp(attr, "id/bustype") == 0)
		v = in->bustype, hex = true;
	else if (strcmp(attr, "id/vendor") == 0)
		v = in->vendor, hex = true;
	else if (strcmp(attr, "id/product") == 0)
		v = in->product, hex = true;
	else if (strcmp(attr, "id/version") == 0)
		v = in->version, hex = true;
	else {
		for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
			if (strcmp(attr, caps[i].name) == 0) {
				// Every type has its file; an empty one reads 0.
				s = in->caps_text[caps[i].cap] ?
					    in->caps_text[caps[i].cap] :
					    "0";
				break;
			}
		if (!s)
			return false;
	}
	if (hex)
		snprintf(buf, size, "%04x", v);
	else
		snprintf(buf, size, "%s", s);
	return true;
}

LUDEV_EXPORT const char *udev_device_get_sysattr_value(struct udev_device *udev_device,
						       const char *sysattr)
{
	struct udev_device *d = udev_device;
	struct udev_list_entry *e;
	char *value = NULL;
	char buf[LUDEV_VALUE_MAX];

	if (!d || !sysattr || !*sysattr) {
		errno = EINVAL;
		return NULL;
	}
	e = ludev_list_find(&d->sysattrs, sysattr);
	if (e) {
		if (!e->value)
			errno = ENOENT;
		return e->value;
	}

	switch (d->kind) {
	case LUDEV_KIND_EVENT:
		if (strcmp(sysattr, "dev") == 0) {
			snprintf(buf, sizeof(buf), "%u:%u", major(d->devnum),
				 minor(d->devnum));
			value = buf;
		}
		break;
	case LUDEV_KIND_INPUT:
		if (d->input && input_attr(d->input, sysattr, buf, sizeof(buf)))
			value = buf;
		break;
	case LUDEV_KIND_SYSFS: {
		char *file = NULL;
		int rc;

		// Attribute names are paths below the device; nothing
		// above it.
		if (sysattr[0] == '/' || strstr(sysattr, "..") ||
		    strlen(sysattr) > 256) {
			errno = EINVAL;
			return NULL;
		}
		rc = read_sys_value(d->syspath, sysattr, &file);
		if (rc == -ENOMEM) {
			errno = ENOMEM;
			return NULL;
		}
		if (rc == 0) {
			// Cached as found, so the pointer stays valid.
			if (ludev_list_set(&d->sysattrs, sysattr, file, true)) {
				free(file);
				errno = ENOMEM;
				return NULL;
			}
			free(file);
			return ludev_list_find(&d->sysattrs, sysattr)->value;
		}
		break;
	}
	}

	// Remember the answer, including "no such attribute".
	if (ludev_list_set(&d->sysattrs, sysattr, value, true)) {
		errno = ENOMEM;
		return NULL;
	}
	e = ludev_list_find(&d->sysattrs, sysattr);
	if (!e->value)
		errno = ENOENT;
	return e->value;
}

LUDEV_EXPORT struct udev_list_entry *
udev_device_get_sysattr_list_entry(struct udev_device *udev_device)
{
	struct udev_device *d = udev_device;

	if (!d)
		return NULL;
	if (d->sysattr_names_done)
		return d->sysattr_names.head;
	d->sysattr_names_done = true;
	switch (d->kind) {
	case LUDEV_KIND_EVENT:
		if (ludev_list_set(&d->sysattr_names, "dev", NULL, false))
			goto nomem;
		break;
	case LUDEV_KIND_INPUT:
		for (size_t i = 0; i < sizeof(input_attrs) / sizeof(input_attrs[0]); i++)
			if (ludev_list_set(&d->sysattr_names, input_attrs[i], NULL, false))
				goto nomem;
		break;
	case LUDEV_KIND_SYSFS: {
		char path[LUDEV_PATH_MAX];
		DIR *dir;
		struct dirent *de;

		if (!sys_fs_path(d->syspath, NULL, path, sizeof(path)))
			break;
		dir = opendir(path);
		if (!dir)
			break;
		while ((de = readdir(dir)) != NULL) {
			char fp[LUDEV_PATH_MAX];
			struct stat st;

			if (de->d_name[0] == '.' || strcmp(de->d_name, "uevent") == 0)
				continue;
			if (!sys_fs_path(d->syspath, de->d_name, fp, sizeof(fp)) ||
			    lstat(fp, &st) != 0 || !S_ISREG(st.st_mode))
				continue;
			if (ludev_list_set(&d->sysattr_names, de->d_name, NULL, false)) {
				closedir(dir);
				goto nomem;
			}
		}
		closedir(dir);
		break;
	}
	}
	return d->sysattr_names.head;

nomem:
	ludev_list_clear(&d->sysattr_names);
	d->sysattr_names_done = false;
	errno = ENOMEM;
	return NULL;
}
