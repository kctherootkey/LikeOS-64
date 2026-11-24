// LikeOS Explorer: the device service.
//
// devd, the system's device service, knows the disks and the partitions on
// them, mounts, unmounts, ejects and formats USB drives, mounts and ejects
// CDs and DVDs, mounts the partitions of built-in disks the system does not
// use (never formatting or ejecting those), and tells its
// clients when a device came, changed or went.  This is Explorer's client:
// one connection to the service's socket, opened in the background, with
// HELLO, SUBSCRIBE, LIST and FSTYPES sent first; then the table of devices
// kept current from the service's events, and requests answered in the
// order they were made.  When the service is not running, or the
// connection breaks, the table is empty and the client tries again, at
// growing intervals up to a few seconds, so the drives come back by
// themselves when the service does.
//
// Formatting runs on a connection of its own (ExDevdFormat), so the
// progress of one long job never holds up the answers to anything else.
//
// Signals of ExDevd:
//   "changed"          ()  -- the table of devices changed, or the
//                             connection came or went
//   "fstypes-changed"  ()
// Signals of ExDevdFormat:
//   "progress"  (int percent)  -- -1: a step without progress of its own
//                                 (writing the file system) has started
//   "finished"  (GError *error)  -- NULL when the drive was formatted;
//                                 EX_DEVD_ERROR_CANCELED after a cancel
//                                 (ex_devd_cancel_format_async()), with
//                                 the service's message saying whether
//                                 the drive was changed
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_DEVD_H
#define EX_DEVD_H

#include <gio/gio.h>
#include "backend/ex-devd-proto.h"

G_BEGIN_DECLS

#define EX_TYPE_DEVD (ex_devd_get_type())
G_DECLARE_FINAL_TYPE(ExDevd, ex_devd, EX, DEVD, GObject)

/* The one client; connects on first use. */
ExDevd *ex_devd_get_default(void);
/* A client of its own for a service listening at `socket_path' (the test
 * program's stand-in service). */
ExDevd *ex_devd_new_for_path(const char *socket_path);
/* Whether the service answered and the table is its own. */
gboolean ex_devd_is_connected(ExDevd *d);
/* ExDevdDevice, owned by the client; valid until the next "changed". */
GPtrArray *ex_devd_devices(ExDevd *d);
const ExDevdDevice *ex_devd_find(ExDevd *d, const char *name);
/* ExDevdFs, owned by the client, the default one first. */
GPtrArray *ex_devd_fstypes(ExDevd *d);
const ExDevdFs *ex_devd_find_fs(ExDevd *d, const char *id);
/* What a filesystem type is called: the service's name for it ("FAT32"),
 * or a known name for the types it only recognises ("NTFS", "exFAT"). */
char *ex_devd_fs_display_name(ExDevd *d, const char *fstype);
/* The same for a volume, which may carry a variant of the type the service
 * cannot open (an older FAT reported as vfat, not supported). */
char *ex_devd_volume_fs_name(ExDevd *d, const char *fstype, gboolean supported);
/* Ask for the filesystem list again (when it is empty). */
void ex_devd_refresh_fstypes(ExDevd *d);

/* MOUNT: the mount path on success. */
void ex_devd_mount_async(ExDevd *d, const char *name, GCancellable *cancel,
			 GAsyncReadyCallback done, gpointer user_data);
char *ex_devd_mount_finish(ExDevd *d, GAsyncResult *res, GError **error);
/* MOUNT with recovery: for a volume whose mount failed with
 * EX_DEVD_ERROR_UNCLEAN and whose user agreed to have the journal
 * replayed.  Finished with ex_devd_mount_finish(). */
void ex_devd_mount_recover_async(ExDevd *d, const char *name,
				 GCancellable *cancel, GAsyncReadyCallback done,
				 gpointer user_data);
/* UNMOUNT one volume. */
void ex_devd_unmount_async(ExDevd *d, const char *name, GCancellable *cancel,
			   GAsyncReadyCallback done, gpointer user_data);
gboolean ex_devd_unmount_finish(ExDevd *d, GAsyncResult *res, GError **error);
/* EJECT a whole disk: every volume of it unmounted, the disk safe to
 * unplug. */
void ex_devd_eject_async(ExDevd *d, const char *disk, GCancellable *cancel,
			 GAsyncReadyCallback done, gpointer user_data);
gboolean ex_devd_eject_finish(ExDevd *d, GAsyncResult *res, GError **error);
/* CANCEL: stop the format of volume `name' (sent on this connection: the
 * format's own takes nothing until its job ends).  Answered once the job
 * has stopped; the job's "finished" then carries EX_DEVD_ERROR_CANCELED.
 * EX_DEVD_ERROR_NOENT when no format of it runs -- it may have just
 * finished, with its own "finished". */
void ex_devd_cancel_format_async(ExDevd *d, const char *name,
				 GCancellable *cancel, GAsyncReadyCallback done,
				 gpointer user_data);
gboolean ex_devd_cancel_format_finish(ExDevd *d, GAsyncResult *res,
				      GError **error);

#define EX_TYPE_DEVD_FORMAT (ex_devd_format_get_type())
G_DECLARE_FINAL_TYPE(ExDevdFormat, ex_devd_format, EX, DEVD_FORMAT, GObject)

/* Start formatting volume `name' with filesystem `fs' (an FSTYPES id),
 * `label' (any text; the service checks it), a quick or a full format, and
 * an allocation unit of `cluster' bytes (0: the filesystem's default).
 * The job keeps itself alive until "finished".  One job at a time: a
 * second one finishes at once with EX_DEVD_ERROR_AGAIN. */
ExDevdFormat *ex_devd_format_start(const char *name, const char *fs,
				   const char *label, gboolean quick,
				   guint64 cluster);
/* Whether "finished" was emitted. */
gboolean ex_devd_format_done(ExDevdFormat *f);

G_END_DECLS

#endif
