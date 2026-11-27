// libudev - enumeration.
//
// Lists the input devices the kernel describes: for each one its input
// device (inputM) and, when it has one, its event node (eventN), in the
// order the kernel registered them.  Devices of other subsystems are not
// enumerated.
//
// Matches combine as in libudev: a device must pass the subsystem matches,
// any one of the sysname patterns and any one of the property matches (when
// there are any of each).  Patterns are shell globs.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

#include "ludev.h"

struct udev_enumerate {
	int refcount;
	struct udev *udev;
	struct ludev_list match_subsystem;
	struct ludev_list nomatch_subsystem;
	struct ludev_list match_sysname;
	struct ludev_list match_property; // name = property glob, value = value glob
	struct ludev_list devices;        // name = syspath
};

LUDEV_EXPORT struct udev_enumerate *udev_enumerate_new(struct udev *udev)
{
	struct udev_enumerate *e;

	if (!udev) {
		errno = EINVAL;
		return NULL;
	}
	e = calloc(1, sizeof(*e));
	if (!e) {
		errno = ENOMEM;
		return NULL;
	}
	e->refcount = 1;
	e->udev = udev_ref(udev);
	return e;
}

LUDEV_EXPORT struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *udev_enumerate)
{
	if (udev_enumerate)
		udev_enumerate->refcount++;
	return udev_enumerate;
}

LUDEV_EXPORT struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *udev_enumerate)
{
	struct udev_enumerate *e = udev_enumerate;

	if (!e)
		return NULL;
	if (--e->refcount > 0)
		return NULL;
	ludev_list_clear(&e->match_subsystem);
	ludev_list_clear(&e->nomatch_subsystem);
	ludev_list_clear(&e->match_sysname);
	ludev_list_clear(&e->match_property);
	ludev_list_clear(&e->devices);
	udev_unref(e->udev);
	free(e);
	return NULL;
}

LUDEV_EXPORT struct udev *udev_enumerate_get_udev(struct udev_enumerate *udev_enumerate)
{
	return udev_enumerate ? udev_enumerate->udev : NULL;
}

LUDEV_EXPORT int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate,
						    const char *subsystem)
{
	if (!udev_enumerate)
		return -EINVAL;
	if (!subsystem)
		return 0;
	return ludev_list_set(&udev_enumerate->match_subsystem, subsystem, NULL, true);
}

LUDEV_EXPORT int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *udev_enumerate,
						      const char *subsystem)
{
	if (!udev_enumerate)
		return -EINVAL;
	if (!subsystem)
		return 0;
	return ludev_list_set(&udev_enumerate->nomatch_subsystem, subsystem, NULL, true);
}

LUDEV_EXPORT int udev_enumerate_add_match_property(struct udev_enumerate *udev_enumerate,
						   const char *property,
						   const char *value)
{
	if (!udev_enumerate)
		return -EINVAL;
	if (!property)
		return 0;
	return ludev_list_set(&udev_enumerate->match_property, property, value, false);
}

LUDEV_EXPORT int udev_enumerate_add_match_sysname(struct udev_enumerate *udev_enumerate,
						  const char *sysname)
{
	if (!udev_enumerate)
		return -EINVAL;
	if (!sysname)
		return 0;
	return ludev_list_set(&udev_enumerate->match_sysname, sysname, NULL, true);
}

// Every device is initialized when it is described.
LUDEV_EXPORT int udev_enumerate_add_match_is_initialized(struct udev_enumerate *udev_enumerate)
{
	return udev_enumerate ? 0 : -EINVAL;
}

static bool subsystem_wanted(const struct udev_enumerate *e, const char *subsystem)
{
	struct udev_list_entry *l;
	bool ok = e->match_subsystem.head == NULL;

	for (l = e->match_subsystem.head; l && !ok; l = l->next)
		if (fnmatch(l->name, subsystem, 0) == 0)
			ok = true;
	for (l = e->nomatch_subsystem.head; l && ok; l = l->next)
		if (fnmatch(l->name, subsystem, 0) == 0)
			ok = false;
	return ok;
}

static bool sysname_wanted(const struct udev_enumerate *e, const char *syspath)
{
	const char *name = strrchr(syspath, '/');
	struct udev_list_entry *l;

	if (!e->match_sysname.head)
		return true;
	name = name ? name + 1 : syspath;
	for (l = e->match_sysname.head; l; l = l->next)
		if (fnmatch(l->name, name, 0) == 0)
			return true;
	return false;
}

// Any one property match is enough.  -ENOMEM when the device could not be
// built to be looked at.
static int properties_wanted(const struct udev_enumerate *e,
			     const struct ludev_input *in, bool event_node)
{
	struct udev_list_entry *m, *p;
	struct udev_device *d;
	int found = 0;

	if (!e->match_property.head)
		return 1;
	d = event_node ? ludev_device_new_event(e->udev, in) :
			 ludev_device_new_input(e->udev, in);
	if (!d)
		return -ENOMEM;
	for (m = e->match_property.head; m && !found; m = m->next) {
		// The size properties open the node: only for a match that
		// can be about them.
		if (ludev_pattern_matches_size(m->name) &&
		    ludev_device_resolve_size(d)) {
			udev_device_unref(d);
			return -ENOMEM;
		}
		for (p = d->properties.head; p && !found; p = p->next)
			if (fnmatch(m->name, p->name, 0) == 0 &&
			    (!m->value ||
			     fnmatch(m->value, p->value ? p->value : "", 0) == 0))
				found = 1;
	}
	udev_device_unref(d);
	return found;
}

static int consider(struct udev_enumerate *e, const struct ludev_input *in,
		    bool event_node)
{
	char path[LUDEV_PATH_MAX];
	int rc;

	if (event_node ? !ludev_event_syspath(in, path, sizeof(path)) :
			 !ludev_input_syspath(in, path, sizeof(path)))
		return 0;
	if (!sysname_wanted(e, path))
		return 0;
	rc = properties_wanted(e, in, event_node);
	if (rc <= 0)
		return rc;
	return ludev_list_set(&e->devices, path, NULL, true);
}

LUDEV_EXPORT int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate)
{
	struct udev_enumerate *e = udev_enumerate;
	struct ludev_input_table t;
	int rc;

	if (!e)
		return -EINVAL;
	ludev_list_clear(&e->devices);
	if (!subsystem_wanted(e, "input"))
		return 0;
	rc = ludev_input_load(&t);
	if (rc)
		return rc;
	for (size_t i = 0; i < t.n && !rc; i++) {
		rc = consider(e, &t.v[i], false);
		if (!rc && t.v[i].event >= 0)
			rc = consider(e, &t.v[i], true);
	}
	ludev_input_table_free(&t);
	if (rc)
		ludev_list_clear(&e->devices);
	return rc;
}

LUDEV_EXPORT struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate)
{
	return udev_enumerate ? udev_enumerate->devices.head : NULL;
}
