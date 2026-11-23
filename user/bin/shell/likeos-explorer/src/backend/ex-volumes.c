// LikeOS Explorer: drives and other places that hold files.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-volumes.h"
#include "backend/ex-devd.h"

#include <string.h>

struct _ExVolumes {
	GObject parent;
	GVolumeMonitor *mon;
	GPtrArray *list; /* ExVolume */
	guint rebuild_id;
	gboolean have_network;
};

G_DEFINE_TYPE(ExVolumes, ex_volumes, G_TYPE_OBJECT)

enum { SIG_CHANGED, SIG_REMOVED, N_SIGS };
static guint sigs[N_SIGS];

static void volume_free(ExVolume *v)
{
	g_free(v->name);
	g_free(v->icon_name);
	g_clear_object(&v->root);
	g_free(v->fs_type);
	g_clear_object(&v->mount);
	g_free(v->device);
	g_free(v->disk);
	g_free(v->label);
	g_free(v->uuid);
	g_free(v->state);
	g_free(v);
}

static void ex_volumes_dispose(GObject *o)
{
	ExVolumes *v = EX_VOLUMES(o);

	if (v->rebuild_id) {
		g_source_remove(v->rebuild_id);
		v->rebuild_id = 0;
	}
	if (v->mon)
		g_signal_handlers_disconnect_by_data(v->mon, v);
	g_clear_object(&v->mon);
	g_signal_handlers_disconnect_by_data(ex_devd_get_default(), v);
	g_clear_pointer(&v->list, g_ptr_array_unref);
	G_OBJECT_CLASS(ex_volumes_parent_class)->dispose(o);
}

static void ex_volumes_class_init(ExVolumesClass *k)
{
	G_OBJECT_CLASS(k)->dispose = ex_volumes_dispose;
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
	sigs[SIG_REMOVED] = g_signal_new("removed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 2, G_TYPE_FILE,
					 G_TYPE_STRING);
}

static void ex_volumes_init(ExVolumes *v)
{
}

/* The figures for one volume, in the background. */
typedef struct {
	ExVolumes *owner;
	GFile *root;
} SpaceReq;

static void got_space(GObject *src, GAsyncResult *res, gpointer data)
{
	SpaceReq *r = data;
	GFileInfo *i = g_file_query_filesystem_info_finish(G_FILE(src), res,
							   NULL);

	if (i && r->owner->list) {
		for (guint n = 0; n < r->owner->list->len; n++) {
			ExVolume *v = g_ptr_array_index(r->owner->list, n);

			if (v->root && g_file_equal(v->root, r->root)) {
				v->size = g_file_info_get_attribute_uint64(
					i, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
				v->free = g_file_info_get_attribute_uint64(
					i, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
				v->used = g_file_info_has_attribute(
						  i, G_FILE_ATTRIBUTE_FILESYSTEM_USED) ?
						  g_file_info_get_attribute_uint64(
							  i,
							  G_FILE_ATTRIBUTE_FILESYSTEM_USED) :
						  G_MAXUINT64;
				if (!v->fs_type || !*v->fs_type) {
					g_free(v->fs_type);
					v->fs_type = g_strdup(
						g_file_info_get_attribute_string(
							i,
							G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
				}
				g_signal_emit(r->owner, sigs[SIG_CHANGED], 0);
				break;
			}
		}
	}
	g_clear_object(&i);
	g_object_unref(r->root);
	g_object_unref(r->owner);
	g_free(r);
}

static void ask_space(ExVolumes *v, ExVolume *vol)
{
	SpaceReq *r;

	if (!vol->root)
		return;
	r = g_new0(SpaceReq, 1);
	r->owner = g_object_ref(v);
	r->root = g_object_ref(vol->root);
	g_file_query_filesystem_info_async(
		vol->root,
		G_FILE_ATTRIBUTE_FILESYSTEM_SIZE
		"," G_FILE_ATTRIBUTE_FILESYSTEM_FREE
		"," G_FILE_ATTRIBUTE_FILESYSTEM_USED
		"," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
		G_PRIORITY_LOW, NULL, got_space, r);
}

static gboolean have_root(GPtrArray *list, GFile *root)
{
	for (guint i = 0; list && i < list->len; i++) {
		GFile *r = ((ExVolume *)g_ptr_array_index(list, i))->root;

		if (r && g_file_equal(r, root))
			return TRUE;
	}
	return FALSE;
}

char *ex_volumes_unescape(const char *field)
{
	GString *out = g_string_sized_new(strlen(field));

	for (const char *c = field; *c; c++) {
		if (c[0] == '\\' && c[1] >= '0' && c[1] <= '3' && c[2] >= '0' &&
		    c[2] <= '7' && c[3] >= '0' && c[3] <= '7') {
			g_string_append_c(out, (char)(((c[1] - '0') << 6) |
						      ((c[2] - '0') << 3) |
						      (c[3] - '0')));
			c += 3;
		} else {
			g_string_append_c(out, *c);
		}
	}
	return g_string_free(out, FALSE);
}

/* The filesystems a person keeps files on, from the mount table.  The
 * kernel's own (proc, sysfs, devfs) are skipped. */
static void add_system_mounts(GPtrArray *list, GPtrArray *usb)
{
	char *data = NULL;
	gboolean root_seen = FALSE;

	if (g_file_get_contents("/proc/mounts", &data, NULL, NULL)) {
		char **lines = g_strsplit(data, "\n", -1);

		for (int i = 0; lines[i]; i++) {
			char **f = g_strsplit_set(lines[i], " \t", -1);

			if (g_strv_length(f) >= 3) {
				char *dir = ex_volumes_unescape(f[1]);
				const char *type = f[2];

				if (strcmp(type, "proc") && strcmp(type, "sysfs") &&
				    strcmp(type, "devfs") && strcmp(type, "devtmpfs") &&
				    strcmp(type, "devpts") &&
				    (strcmp(dir, "/") == 0 ||
				     g_str_has_prefix(dir, "/media") ||
				     g_str_has_prefix(dir, "/mnt") ||
				     strcmp(dir, "/ram") == 0)) {
					GFile *root = g_file_new_for_path(dir);

					/* A drive the device service mounted
					 * is shown as the service describes it. */
					if (!have_root(list, root) &&
					    !have_root(usb, root)) {
						ExVolume *v = g_new0(ExVolume, 1);
						char *b = g_path_get_basename(dir);

						v->root = root;
						v->fs_type = g_strdup(type);
						if (strcmp(dir, "/") == 0) {
							v->name = g_strdup(EX_SYSTEM_DISK_NAME);
							v->icon_name = g_strdup("drive-harddisk");
							v->kind = EX_VOLUME_SYSTEM;
							root_seen = TRUE;
						} else if (strcmp(type, "tmpfs") == 0) {
							v->name = g_strdup("RAM Disk");
							v->icon_name = g_strdup("media-flash");
							v->kind = EX_VOLUME_RAM;
						} else {
							v->name = g_strdup(b);
							v->icon_name = g_strdup("drive-harddisk");
							v->kind = EX_VOLUME_LOCAL;
						}
						g_free(b);
						g_ptr_array_add(list, v);
					} else {
						g_object_unref(root);
					}
				}
				g_free(dir);
			}
			g_strfreev(f);
		}
		g_strfreev(lines);
		g_free(data);
	}
	if (!root_seen) {
		GFile *root = g_file_new_for_path("/");

		if (!have_root(list, root)) {
			ExVolume *v = g_new0(ExVolume, 1);

			v->root = root;
			v->name = g_strdup(EX_SYSTEM_DISK_NAME);
			v->icon_name = g_strdup("drive-harddisk");
			v->fs_type = g_strdup("");
			v->kind = EX_VOLUME_SYSTEM;
			g_ptr_array_insert(list, 0, v);
		} else {
			g_object_unref(root);
		}
	}
	/* The RAM disk is not in the table on every kernel; it is a volume
	 * wherever it exists. */
	if (g_file_test("/ram", G_FILE_TEST_IS_DIR)) {
		GFile *ram = g_file_new_for_path("/ram");

		if (!have_root(list, ram)) {
			ExVolume *v = g_new0(ExVolume, 1);

			v->root = ram;
			v->name = g_strdup("RAM Disk");
			v->icon_name = g_strdup("media-flash");
			v->fs_type = g_strdup("tmpfs");
			v->kind = EX_VOLUME_RAM;
			g_ptr_array_add(list, v);
		} else {
			g_object_unref(ram);
		}
	}
}

static char *icon_name_of(GIcon *icon)
{
	if (G_IS_THEMED_ICON(icon)) {
		const char *const *names =
			g_themed_icon_get_names(G_THEMED_ICON(icon));

		if (names && names[0])
			return g_strdup(names[0]);
	}
	return g_strdup("drive-removable-media");
}

/* Whether a device of the service's table is a volume to show:
 *  - a USB drive's partition, or the whole disk when there is a filesystem
 *    directly on it -- or nothing on it at all, so that it can be
 *    formatted;
 *  - a CD/DVD drive, with a disc or without one;
 *  - a partition of a built-in disk with a filesystem LikeOS can open.
 * A system device (the stick LikeOS runs from, the disk with the root) is
 * the System disk already, and an ejected one is safe to unplug: neither
 * is shown. */
static gboolean is_drive_volume(GPtrArray *devs, const ExDevdDevice *d)
{
	if (d->system || d->ejected)
		return FALSE;
	if (d->optical)
		return d->is_disk;
	if (d->internal)
		return d->supported && d->size > 0;
	if (!d->usb || d->size == 0)
		return FALSE;
	if (!d->is_disk || strcmp(d->fstype, "none") != 0)
		return TRUE;
	for (guint i = 0; i < devs->len; i++) {
		const ExDevdDevice *p = g_ptr_array_index(devs, i);

		if (!p->is_disk && strcmp(p->disk, d->name) == 0)
			return FALSE;
	}
	return TRUE;
}

static ExDriveClass drive_class(const ExDevdDevice *d)
{
	if (d->optical)
		return EX_DRIVE_OPTICAL;
	if (d->internal)
		return EX_DRIVE_INTERNAL;
	return EX_DRIVE_USB;
}

static char *drive_name(const ExDevdDevice *d, const char *label)
{
	if (*label)
		return g_strdup(label);
	switch (drive_class(d)) {
	case EX_DRIVE_OPTICAL:
		return g_strdup(EX_OPTICAL_DRIVE_NAME);
	case EX_DRIVE_INTERNAL:
		return g_strdup_printf(EX_LOCAL_DISK_NAME " (%s)", d->name);
	case EX_DRIVE_USB:
	default:
		return g_strdup(EX_USB_DRIVE_NAME);
	}
}

static GPtrArray *drive_volumes(void)
{
	GPtrArray *devs = ex_devd_devices(ex_devd_get_default());
	GPtrArray *out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)volume_free);

	for (guint i = 0; i < devs->len; i++) {
		const ExDevdDevice *d = g_ptr_array_index(devs, i);
		ExVolume *v;
		char *label;

		if (!is_drive_volume(devs, d))
			continue;
		v = g_new0(ExVolume, 1);
		label = g_strstrip(g_strdup(d->label));
		v->kind = EX_VOLUME_DRIVE;
		v->drive = drive_class(d);
		v->readonly = d->readonly || d->optical;
		v->name = drive_name(d, label);
		v->root = *d->mounted ? g_file_new_for_path(d->mounted) : NULL;
		v->fs_type = g_strdup(d->fstype);
		v->removable = v->drive != EX_DRIVE_INTERNAL;
		v->device = g_strdup(d->name);
		v->disk = g_strdup(d->disk);
		v->label = label;
		v->uuid = g_strdup(d->uuid);
		v->capacity = d->size;
		v->supported = d->supported;
		v->state = g_strdup(d->state);
		v->icon_name = g_strdup(ex_volume_drive_icon(v));
		g_ptr_array_add(out, v);
	}
	/* Two drives of the same name are told apart by their device. */
	for (guint i = 0; i < out->len; i++) {
		ExVolume *a = g_ptr_array_index(out, i);
		gboolean twin = FALSE;

		for (guint j = 0; j < out->len && !twin; j++)
			twin = j != i && strcmp(a->name, ((ExVolume *)g_ptr_array_index(
								 out, j))->name) == 0;
		if (twin) {
			char *n = g_strdup_printf("%s (%s)", a->name, a->device);

			g_free(a->name);
			a->name = n;
		}
	}
	return out;
}

/* The drives of the device service that went, or were unmounted,
 * between two lists. */
static void announce_removals(ExVolumes *v, GPtrArray *old)
{
	for (guint i = 0; old && i < old->len; i++) {
		ExVolume *o = g_ptr_array_index(old, i);
		const ExVolume *n;

		if (o->kind != EX_VOLUME_DRIVE)
			continue;
		n = ex_volumes_find_device(v, o->device);
		if (!n)
			g_signal_emit(v, sigs[SIG_REMOVED], 0, o->root, o->device);
		else if (o->root && (!n->root || !g_file_equal(n->root, o->root)))
			g_signal_emit(v, sigs[SIG_REMOVED], 0, o->root, NULL);
	}
}

static void rebuild(ExVolumes *v)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(
		(GDestroyNotify)volume_free);
	GPtrArray *usb = drive_volumes();
	GPtrArray *old;
	GList *mounts = v->mon ? g_volume_monitor_get_mounts(v->mon) : NULL;

	add_system_mounts(list, usb);
	for (guint i = 0; i < usb->len; i++)
		g_ptr_array_add(list, g_ptr_array_steal_index(usb, i--));
	g_ptr_array_unref(usb);
	v->have_network = FALSE;
	for (GList *l = mounts; l; l = l->next) {
		GMount *m = l->data;
		GFile *root = g_mount_get_root(m);

		if (g_mount_is_shadowed(m) || have_root(list, root)) {
			g_object_unref(root);
			continue;
		}
		ExVolume *vol = g_new0(ExVolume, 1);
		GIcon *icon = g_mount_get_icon(m);

		vol->mount = g_object_ref(m);
		vol->root = root;
		vol->name = g_mount_get_name(m);
		vol->icon_name = icon_name_of(icon);
		vol->fs_type = g_strdup("");
		vol->removable = g_mount_can_eject(m) || g_mount_can_unmount(m);
		vol->network = !g_file_is_native(root);
		vol->kind = vol->network ? EX_VOLUME_NETWORK : EX_VOLUME_OTHER;
		if (vol->network)
			v->have_network = TRUE;
		g_clear_object(&icon);
		g_ptr_array_add(list, vol);
	}
	g_list_free_full(mounts, g_object_unref);
	old = v->list;
	v->list = list;
	for (guint i = 0; i < list->len; i++)
		ask_space(v, g_ptr_array_index(list, i));
	g_signal_emit(v, sigs[SIG_CHANGED], 0);
	announce_removals(v, old);
	if (old)
		g_ptr_array_unref(old);
}

static gboolean rebuild_idle(gpointer data)
{
	ExVolumes *v = data;

	v->rebuild_id = 0;
	rebuild(v);
	return G_SOURCE_REMOVE;
}

static void mounts_changed(ExVolumes *v)
{
	if (!v->rebuild_id)
		v->rebuild_id = g_idle_add(rebuild_idle, v);
}

ExVolumes *ex_volumes_get_default(void)
{
	static ExVolumes *the;

	if (!the) {
		the = g_object_new(EX_TYPE_VOLUMES, NULL);
		the->mon = g_volume_monitor_get();
		g_signal_connect_swapped(the->mon, "mount-added",
					 G_CALLBACK(mounts_changed), the);
		g_signal_connect_swapped(the->mon, "mount-removed",
					 G_CALLBACK(mounts_changed), the);
		g_signal_connect_swapped(the->mon, "mount-changed",
					 G_CALLBACK(mounts_changed), the);
		/* The device service's events are what says a drive came,
		 * went, was mounted or unmounted: the mount table is read
		 * again on each. */
		g_signal_connect_swapped(ex_devd_get_default(), "changed",
					 G_CALLBACK(mounts_changed), the);
		rebuild(the);
		/* GVfs' network:// backend, when installed, is what makes a
		 * Network place exist at all. */
		const char *const *schemes = g_vfs_get_supported_uri_schemes(
			g_vfs_get_default());
		for (int i = 0; schemes && schemes[i]; i++)
			if (strcmp(schemes[i], "network") == 0 ||
			    strcmp(schemes[i], "smb") == 0)
				the->have_network = TRUE;
	}
	return the;
}

GPtrArray *ex_volumes_list(ExVolumes *v)
{
	return v->list;
}

gboolean ex_volumes_have_network(ExVolumes *v)
{
	return v->have_network;
}

const ExVolume *ex_volumes_find(ExVolumes *v, GFile *file)
{
	const ExVolume *best = NULL;
	size_t best_len = 0;

	for (guint i = 0; v->list && i < v->list->len; i++) {
		ExVolume *vol = g_ptr_array_index(v->list, i);
		char *uri;
		size_t len;

		if (!vol->root)
			continue;
		uri = g_file_get_uri(vol->root);
		len = strlen(uri);

		if ((g_file_equal(file, vol->root) ||
		     g_file_has_prefix(file, vol->root)) &&
		    len >= best_len) {
			best = vol;
			best_len = len;
		}
		g_free(uri);
	}
	return best;
}

const ExVolume *ex_volumes_find_device(ExVolumes *v, const char *device)
{
	for (guint i = 0; device && v->list && i < v->list->len; i++) {
		ExVolume *vol = g_ptr_array_index(v->list, i);

		if (vol->device && strcmp(vol->device, device) == 0)
			return vol;
	}
	return NULL;
}

const ExVolume *ex_volumes_for_location(ExVolumes *v, ExLocation *loc)
{
	GFile *f;

	if (!loc)
		return NULL;
	if (ex_location_kind(loc) == EX_LOC_DEVICE)
		return ex_volumes_find_device(v, ex_location_device(loc));
	if (ex_location_kind(loc) != EX_LOC_FOLDER)
		return NULL;
	f = ex_location_file(loc);
	for (guint i = 0; v->list && i < v->list->len; i++) {
		ExVolume *vol = g_ptr_array_index(v->list, i);

		if (vol->root && g_file_equal(vol->root, f))
			return vol;
	}
	return NULL;
}

ExLocation *ex_volume_location(const ExVolume *vol)
{
	if (vol->root)
		return ex_location_new_folder(vol->root);
	return ex_location_new_device(vol->device);
}

void ex_volumes_release(ExVolumes *v, const ExVolume *vol)
{
	GFile *root;

	if (!vol->root)
		return;
	root = g_object_ref(vol->root);
	g_signal_emit(v, sigs[SIG_REMOVED], 0, root, NULL);
	g_object_unref(root);
}

void ex_volumes_refresh(ExVolumes *v)
{
	for (guint i = 0; v->list && i < v->list->len; i++)
		ask_space(v, g_ptr_array_index(v->list, i));
}

gboolean ex_volume_can_eject(const ExVolume *vol)
{
	return vol && vol->kind == EX_VOLUME_DRIVE &&
	       vol->drive != EX_DRIVE_INTERNAL;
}

gboolean ex_volume_can_format(const ExVolume *vol)
{
	return vol && vol->kind == EX_VOLUME_DRIVE &&
	       vol->drive == EX_DRIVE_USB && !vol->readonly;
}

gboolean ex_volume_can_unmount(const ExVolume *vol)
{
	return vol && vol->kind == EX_VOLUME_DRIVE &&
	       vol->drive == EX_DRIVE_INTERNAL && vol->root;
}

const char *ex_volume_drive_icon(const ExVolume *vol)
{
	switch (vol->drive) {
	case EX_DRIVE_OPTICAL:
		return vol->capacity > 0 ? "media-optical" : "drive-optical";
	case EX_DRIVE_INTERNAL:
		return "drive-harddisk";
	case EX_DRIVE_USB:
	default:
		return "drive-removable-media-usb";
	}
}

gboolean ex_volume_no_disc(const ExVolume *vol)
{
	return vol && vol->kind == EX_VOLUME_DRIVE &&
	       vol->drive == EX_DRIVE_OPTICAL && vol->capacity == 0;
}

const char *ex_volume_type_text(const ExVolume *vol)
{
	switch (vol->kind) {
	case EX_VOLUME_DRIVE:
		switch (vol->drive) {
		case EX_DRIVE_OPTICAL:
			return "CD/DVD drive";
		case EX_DRIVE_INTERNAL:
			return "Local disk";
		case EX_DRIVE_USB:
		default:
			return "USB drive";
		}
	case EX_VOLUME_RAM:
		return "RAM disk";
	case EX_VOLUME_NETWORK:
		return "Network share";
	case EX_VOLUME_OTHER:
		return "Removable drive";
	case EX_VOLUME_SYSTEM:
	case EX_VOLUME_LOCAL:
	default:
		return "Local disk";
	}
}

void ex_volume_eject_async(const ExVolume *vol, GMountOperation *mop,
			   GCancellable *cancel, GAsyncReadyCallback done,
			   gpointer user_data)
{
	g_return_if_fail(vol->mount != NULL);
	if (g_mount_can_eject(vol->mount))
		g_mount_eject_with_operation(vol->mount, G_MOUNT_UNMOUNT_NONE,
					     mop, cancel, done, user_data);
	else
		g_mount_unmount_with_operation(vol->mount, G_MOUNT_UNMOUNT_NONE,
					       mop, cancel, done, user_data);
}

gboolean ex_volume_eject_finish(GAsyncResult *res, GError **error)
{
	GMount *m = G_MOUNT(g_async_result_get_source_object(res));
	gboolean ok;

	if (g_mount_can_eject(m))
		ok = g_mount_eject_with_operation_finish(m, res, error);
	else
		ok = g_mount_unmount_with_operation_finish(m, res, error);
	g_object_unref(m);
	return ok;
}
