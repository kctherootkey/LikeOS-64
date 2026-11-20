// LikeOS -- device events: drivers tell userspace what came and went.
//
// A driver calls devevent_publish() when a device appears, goes away or
// changes; the record goes into a ring that /dev/devevents (root only)
// hands out.  The record layout and its meaning are the ABI in
// <kernel/uapi/devevent.h>.  The kernel only reports: what to do about a
// device is userspace's decision.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_DEVEVENT_H
#define KERNEL_FS_DEVEVENT_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/devevent.h>

/* Publish one event.  `type' is a DEVICE_EVENT_* (not OVERFLOW),
 * `device_class' a DEVICE_CLASS_*, `flags' DEVICE_EVENT_F_*; ids, numbers
 * and `name' (NULL for none; cut to 31 bytes) as <kernel/uapi/devevent.h>
 * describes them.
 *
 * Any context: process context, a spinlock or mutex held, interrupts off,
 * an interrupt handler.  It never sleeps, allocates nothing and takes only
 * leaf locks (its ring lock, then -- after dropping that -- the readers'
 * wait queue and the run queues of the readers it wakes, exactly what an
 * interrupt handler's wake-up takes). */
void devevent_publish(uint32_t type, uint32_t device_class,
		      uint64_t device_id, uint64_t parent_id, uint32_t flags,
		      uint16_t vendor, uint16_t product, uint32_t major,
		      uint32_t minor, const char *name);

/* Register /dev/devevents (devfs initialisation).  Events published before
 * this are kept in the ring like any others. */
void devevent_init(void);

#endif /* KERNEL_FS_DEVEVENT_H */
