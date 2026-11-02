// LikeOS -- File Explorer: drives and other places that hold files.
//
// Two sources, merged: GIO's volume monitor (mounts it knows -- removable
// media and network shares, where a backend provides them) and the system's
// own table of mounted filesystems, /proc/mounts, of which the ones a person
// keeps files on are shown: the root disk and the RAM disk.  Each comes with
// its capacity and free space, asked for in the background.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_VOLUMES_H
#define EX_VOLUMES_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
	char *name; /* "Local Disk", "RAM Disk", "USB stick" */
	char *icon_name;
	GFile *root;
	char *fs_type; /* "ext4", "tmpfs", "" */
	guint64 size, free; /* 0 when not known (yet) */
	gboolean removable; /* can be ejected */
	gboolean network;
	GMount *mount; /* when it came from GIO, else NULL */
} ExVolume;

#define EX_TYPE_VOLUMES (ex_volumes_get_type())
G_DECLARE_FINAL_TYPE(ExVolumes, ex_volumes, EX, VOLUMES, GObject)

/* The one list ("changed" when a volume came, went or its space is
 * known). */
ExVolumes *ex_volumes_get_default(void);
/* ExVolume*, owned by the list; valid until the next "changed". */
GPtrArray *ex_volumes_list(ExVolumes *v);
/* Whether anything provides network locations (the sidebar's Network
 * entry shows only then). */
gboolean ex_volumes_have_network(ExVolumes *v);
/* The volume holding `file', or NULL. */
const ExVolume *ex_volumes_find(ExVolumes *v, GFile *file);
/* Ask for the space figures again. */
void ex_volumes_refresh(ExVolumes *v);

/* Unmount / eject a GIO volume. */
void ex_volume_eject_async(const ExVolume *vol, GMountOperation *mop,
			   GCancellable *cancel, GAsyncReadyCallback done,
			   gpointer user_data);
gboolean ex_volume_eject_finish(GAsyncResult *res, GError **error);

G_END_DECLS

#endif
