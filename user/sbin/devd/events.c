// devd: the kernel's device events (/dev/devevents).
//
// Every record only tells devd where to look again: the device table is
// always rebuilt from /sys/block, so records may come in any order, may be
// for devices devd never saw, and a lost stretch (OVERFLOW) costs one full
// rescan.  Without /dev/devevents devd rescans every few seconds instead.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/devevent.h>
#include <unistd.h>

#include "devd.h"

int events_open(void)
{
	int fd = open(DEVD_EVENTS_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		log_warn("cannot open " DEVD_EVENTS_PATH " (%s); looking at "
			 "/sys/block every %d s instead",
			 strerror(errno), DEVD_POLL_RESCAN_MS / 1000);
	return fd;
}

/* The disk a block device belongs to, from /sys/class/block/<name>
 * ("../../block/sdb/sdb1"); 0 when it cannot be told. */
static int disk_of(const char *name, char *out, size_t cap)
{
	char path[DEVD_PATH_MAX], target[DEVD_PATH_MAX];
	const char *p, *e;
	ssize_t n;
	size_t len;

	if (!devname_ok(name))
		return 0;
	snprintf(path, sizeof(path), "/sys/class/block/%s", name);
	n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0) {
		struct dev *d = tab_find(name);

		if (!d)
			return 0;
		strlcpy_d(out, d->disk, cap);
		return 1;
	}
	target[n] = '\0';
	p = strstr(target, "block/");
	if (!p)
		return 0;
	p += 6;
	e = strchr(p, '/');
	len = e ? (size_t)(e - p) : strlen(p);
	if (len == 0 || len >= cap || len >= DEVD_NAME_MAX)
		return 0;
	memcpy(out, p, len);
	out[len] = '\0';
	return devname_ok(out);
}

int events_read(int fd, int *all, char *force, size_t force_cap)
{
	struct device_event ev[16];
	int rescan = 0;

	for (int rounds = 0; rounds < 64; rounds++) {
		ssize_t n = read(fd, ev, sizeof(ev));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				log_warn("reading " DEVD_EVENTS_PATH ": %s",
					 strerror(errno));
			break;
		}
		if (n == 0)
			break;
		for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
			struct device_event *e = &ev[i];
			char name[DEVD_NAME_MAX], disk[DEVD_NAME_MAX];

			if (e->size != sizeof(*e) || e->version != DEVEVENT_VERSION)
				continue;
			if (e->type == DEVICE_EVENT_OVERFLOW) {
				log_info("device events were lost; looking at "
					 "everything again");
				*all = 1;
				rescan = 1;
				continue;
			}
			if (e->device_class != DEVICE_CLASS_BLOCK_DISK &&
			    e->device_class != DEVICE_CLASS_BLOCK_PARTITION)
				continue;
			memcpy(name, e->name, sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
			rescan = 1;
			log_debug("event %u class %u %s", e->type,
				  e->device_class, devname_ok(name) ? name : "?");
			if (!disk_of(name, disk, sizeof(disk))) {
				/* gone already, or never seen: the rescan
				 * finds out what is left */
				continue;
			}
			if ((e->flags & DEVICE_EVENT_F_USB) &&
			    e->type != DEVICE_EVENT_REMOVE)
				tab_note_usb(disk);
			if (e->type == DEVICE_EVENT_CHANGE ||
			    e->type == DEVICE_EVENT_ADD) {
				/* one disk to probe again; more than one in
				 * a batch: all of them */
				if (force[0] && strcmp(force, disk) != 0)
					*all = 1;
				else
					strlcpy_d(force, disk, force_cap);
			}
		}
		if ((size_t)n < sizeof(ev))
			break;
	}
	return rescan;
}
