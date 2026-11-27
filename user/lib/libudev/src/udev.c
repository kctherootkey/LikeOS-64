// libudev - the library context, lists and small helpers.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ludev.h"

struct ludev_paths ludev_paths = {
	.proc_input_devices = "/proc/bus/input/devices",
	.dev_input = "/dev/input",
	.devevents = "/dev/devevents",
	.sys = "/sys",
};

#ifdef LUDEV_TESTING
bool ludev_test_any_node_type;
unsigned int ludev_test_size_opens;

void ludev_test_set_paths(const char *proc_input_devices, const char *dev_input,
			  const char *devevents, const char *sys)
{
	if (proc_input_devices)
		ludev_paths.proc_input_devices = proc_input_devices;
	if (dev_input)
		ludev_paths.dev_input = dev_input;
	if (devevents)
		ludev_paths.devevents = devevents;
	if (sys)
		ludev_paths.sys = sys;
}
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

char *ludev_strdup_bounded(const char *s)
{
	size_t n;
	char *r;

	if (!s)
		return NULL;
	n = strnlen(s, LUDEV_VALUE_MAX);
	r = malloc(n + 1);
	if (!r)
		return NULL;
	memcpy(r, s, n);
	r[n] = '\0';
	return r;
}

// Read a whole file of at most `max' bytes, NUL-terminated.  Pseudo files
// report no size, so this reads until end of file rather than trusting
// st_size.  0, or a negative errno (-EFBIG when the file is larger).
int ludev_read_file(const char *path, char **out, size_t *len, size_t max)
{
	size_t cap = 4096, n = 0;
	char *buf, *nb;
	int fd, err;

	*out = NULL;
	if (len)
		*len = 0;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0)
		return -errno;
	buf = malloc(cap + 1);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	for (;;) {
		ssize_t r;

		if (n == cap) {
			if (cap >= max) {
				err = -EFBIG;
				goto fail;
			}
			cap = cap * 2 > max ? max : cap * 2;
			nb = realloc(buf, cap + 1);
			if (!nb) {
				err = -ENOMEM;
				goto fail;
			}
			buf = nb;
		}
		r = read(fd, buf + n, cap - n);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			goto fail;
		}
		if (r == 0)
			break;
		n += (size_t)r;
	}
	close(fd);
	buf[n] = '\0';
	*out = buf;
	if (len)
		*len = n;
	return 0;

fail:
	close(fd);
	free(buf);
	return err;
}

int ludev_parse_event_name(const char *s)
{
	long v = 0;

	if (!s || strncmp(s, "event", 5) != 0)
		return -1;
	s += 5;
	if (*s < '0' || *s > '9')
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		v = v * 10 + (*s - '0');
		if (v > 1000000)
			return -1;
	}
	return (int)v;
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------

struct udev_list_entry *ludev_list_find(const struct ludev_list *list,
					const char *name)
{
	struct udev_list_entry *e;

	if (!name)
		return NULL;
	for (e = list->head; e; e = e->next)
		if (strcmp(e->name, name) == 0)
			return e;
	return NULL;
}

int ludev_list_set(struct ludev_list *list, const char *name, const char *value,
		   bool unique)
{
	struct udev_list_entry *e;
	char *v = NULL;

	if (!name)
		return -EINVAL;
	if (value) {
		v = ludev_strdup_bounded(value);
		if (!v)
			return -ENOMEM;
	}
	if (unique) {
		e = ludev_list_find(list, name);
		if (e) {
			free(e->value);
			e->value = v;
			return 0;
		}
	}
	e = calloc(1, sizeof(*e));
	if (!e) {
		free(v);
		return -ENOMEM;
	}
	e->name = ludev_strdup_bounded(name);
	if (!e->name) {
		free(v);
		free(e);
		return -ENOMEM;
	}
	e->value = v;
	if (list->tail)
		list->tail->next = e;
	else
		list->head = e;
	list->tail = e;
	return 0;
}

void ludev_list_clear(struct ludev_list *list)
{
	struct udev_list_entry *e = list->head;

	while (e) {
		struct udev_list_entry *next = e->next;

		free(e->name);
		free(e->value);
		free(e);
		e = next;
	}
	list->head = list->tail = NULL;
}

LUDEV_EXPORT struct udev_list_entry *
udev_list_entry_get_next(struct udev_list_entry *list_entry)
{
	return list_entry ? list_entry->next : NULL;
}

LUDEV_EXPORT struct udev_list_entry *
udev_list_entry_get_by_name(struct udev_list_entry *list_entry, const char *name)
{
	if (!name)
		return NULL;
	for (; list_entry; list_entry = list_entry->next)
		if (strcmp(list_entry->name, name) == 0)
			return list_entry;
	return NULL;
}

LUDEV_EXPORT const char *udev_list_entry_get_name(struct udev_list_entry *list_entry)
{
	return list_entry ? list_entry->name : NULL;
}

LUDEV_EXPORT const char *udev_list_entry_get_value(struct udev_list_entry *list_entry)
{
	return list_entry ? list_entry->value : NULL;
}

// ---------------------------------------------------------------------------
// The library context
// ---------------------------------------------------------------------------

LUDEV_EXPORT struct udev *udev_new(void)
{
	struct udev *u = calloc(1, sizeof(*u));

	if (!u) {
		errno = ENOMEM;
		return NULL;
	}
	u->refcount = 1;
	return u;
}

LUDEV_EXPORT struct udev *udev_ref(struct udev *udev)
{
	if (udev)
		udev->refcount++;
	return udev;
}

LUDEV_EXPORT struct udev *udev_unref(struct udev *udev)
{
	if (!udev)
		return NULL;
	if (--udev->refcount > 0)
		return NULL;
	free(udev);
	return NULL;
}

LUDEV_EXPORT void *udev_get_userdata(struct udev *udev)
{
	return udev ? udev->userdata : NULL;
}

LUDEV_EXPORT void udev_set_userdata(struct udev *udev, void *userdata)
{
	if (udev)
		udev->userdata = userdata;
}
