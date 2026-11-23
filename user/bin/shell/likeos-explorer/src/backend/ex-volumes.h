// LikeOS Explorer: drives and other places that hold files.
//
// Three sources, merged: the device service (devd, see ex-devd.h) for USB
// drives, CD/DVD drives and the partitions of built-in disks that the
// system does not use -- mounted or not, with their filesystem, label and
// state; GIO's
// volume monitor (mounts it knows -- network shares, where a backend
// provides them); and the system's own table of mounted filesystems,
// /proc/mounts, of which the ones a person keeps files on are shown: the
// root disk and the RAM disk.  The table is read again whenever the device
// service reports a change.  Each mounted volume comes with its capacity
// and free space, asked for in the background.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_VOLUMES_H
#define EX_VOLUMES_H

#include <gio/gio.h>
#include "backend/ex-location.h"

G_BEGIN_DECLS

/* The name of the disk the system runs from (the root folder's). */
#define EX_SYSTEM_DISK_NAME "System disk"

/* The name of a USB drive without a label. */
#define EX_USB_DRIVE_NAME "USB Drive"
/* The name of a CD/DVD drive whose disc has no label (or no disc). */
#define EX_OPTICAL_DRIVE_NAME "CD/DVD Drive"
/* A partition of a built-in disk without a label: "Local Disk (sda3)". */
#define EX_LOCAL_DISK_NAME "Local Disk"

typedef enum {
	EX_VOLUME_SYSTEM, /* the disk the system runs from */
	EX_VOLUME_RAM, /* the RAM disk */
	EX_VOLUME_LOCAL, /* another mounted filesystem */
	EX_VOLUME_DRIVE, /* a drive the device service reports (USB, CD/DVD,
			    a built-in disk's partition) */
	EX_VOLUME_NETWORK, /* a network share */
	EX_VOLUME_OTHER, /* removable media known to GIO */
} ExVolumeKind;

/* What kind of drive an EX_VOLUME_DRIVE is. */
typedef enum {
	EX_DRIVE_USB, /* mount, unmount, eject, format */
	EX_DRIVE_OPTICAL, /* mount (read-only), unmount, eject (the tray) */
	EX_DRIVE_INTERNAL, /* a built-in disk: mount and unmount only */
} ExDriveClass;

typedef struct {
	char *name; /* "System disk", "RAM Disk", "USB Drive", a label */
	char *icon_name;
	GFile *root; /* where it is mounted; NULL for a USB drive that is not */
	char *fs_type; /* "ext4", "tmpfs", "vfat", "" */
	guint64 size, free; /* 0 when not known (yet) */
	/* What the file system reports in use (its own structures and the
	 * space kept for the system not counted); G_MAXUINT64 when it does
	 * not say. */
	guint64 used;
	gboolean removable; /* can be ejected */
	gboolean network;
	GMount *mount; /* when it came from GIO, else NULL */
	ExVolumeKind kind;
	/* Drives of the device service (EX_VOLUME_DRIVE) only: */
	char *device; /* the volume, "sdb1" (NULL for the other kinds) */
	char *disk; /* its disk, "sdb" */
	char *label, *uuid; /* "" when none */
	guint64 capacity; /* the device's size in bytes */
	gboolean supported; /* LikeOS can open its filesystem */
	char *state; /* "ready", "busy", "formatting" */
	ExDriveClass drive;
	gboolean readonly; /* write-protected, or a disc */
} ExVolume;

#define EX_TYPE_VOLUMES (ex_volumes_get_type())
G_DECLARE_FINAL_TYPE(ExVolumes, ex_volumes, EX, VOLUMES, GObject)

/* The one list ("changed" when a volume came, went or its space is
 * known; "removed" (GFile *root, const char *device) when a USB drive went
 * or was unmounted -- `root' its old mount folder or NULL, `device' its
 * device name when the drive itself is gone, else NULL). */
ExVolumes *ex_volumes_get_default(void);
/* ExVolume*, owned by the list; valid until the next "changed". */
GPtrArray *ex_volumes_list(ExVolumes *v);
/* Whether anything provides network locations (the sidebar's Network
 * entry shows only then). */
gboolean ex_volumes_have_network(ExVolumes *v);
/* The volume holding `file', or NULL. */
const ExVolume *ex_volumes_find(ExVolumes *v, GFile *file);
/* The USB volume the device service calls `device' ("sdb1"), or NULL. */
const ExVolume *ex_volumes_find_device(ExVolumes *v, const char *device);
/* The drive a location is: a USB drive's device location, or a folder
 * that is a volume's root.  NULL for anything else. */
const ExVolume *ex_volumes_for_location(ExVolumes *v, ExLocation *loc);
/* Where a volume opens: its root folder when it is mounted, else its
 * device location (a USB drive to mount first).  A new reference. */
ExLocation *ex_volume_location(const ExVolume *vol);
/* The tabs inside a drive about to go away (ejected, formatted) leave it
 * now: "removed" is emitted for it at once. */
void ex_volumes_release(ExVolumes *v, const ExVolume *vol);
/* Ask for the space figures again. */
void ex_volumes_refresh(ExVolumes *v);
/* What may be done with a drive of the device service: Eject (USB and
 * CD/DVD drives), Format (USB drives that are not write-protected),
 * Unmount (a built-in disk's partition that is mounted). */
gboolean ex_volume_can_eject(const ExVolume *vol);
gboolean ex_volume_can_format(const ExVolume *vol);
gboolean ex_volume_can_unmount(const ExVolume *vol);
/* The icon of a drive of the device service (a name that stays valid). */
const char *ex_volume_drive_icon(const ExVolume *vol);
/* A CD/DVD drive without a disc. */
gboolean ex_volume_no_disc(const ExVolume *vol);
/* "USB drive", "CD/DVD drive", "Local disk", ... */
const char *ex_volume_type_text(const ExVolume *vol);
/* A field of the mount table as it was written: the table escapes a space,
 * a tab, a newline and a backslash as three octal digits ("\040"). */
char *ex_volumes_unescape(const char *field);

/* Unmount / eject a GIO volume. */
void ex_volume_eject_async(const ExVolume *vol, GMountOperation *mop,
			   GCancellable *cancel, GAsyncReadyCallback done,
			   gpointer user_data);
gboolean ex_volume_eject_finish(GAsyncResult *res, GError **error);

G_END_DECLS

#endif
