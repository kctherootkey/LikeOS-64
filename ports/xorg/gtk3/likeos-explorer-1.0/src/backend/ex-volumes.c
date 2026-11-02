// LikeOS -- File Explorer: drives and other places that hold files.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-volumes.h"

#include <string.h>

struct _ExVolumes {
	GObject parent;
	GVolumeMonitor *mon;
	GPtrArray *list; /* ExVolume */
	guint rebuild_id;
	gboolean have_network;
};

G_DEFINE_TYPE(ExVolumes, ex_volumes, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void volume_free(ExVolume *v)
{
	g_free(v->name);
	g_free(v->icon_name);
	g_clear_object(&v->root);
	g_free(v->fs_type);
	g_clear_object(&v->mount);
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
	g_clear_pointer(&v->list, g_ptr_array_unref);
	G_OBJECT_CLASS(ex_volumes_parent_class)->dispose(o);
}

static void ex_volumes_class_init(ExVolumesClass *k)
{
	G_OBJECT_CLASS(k)->dispose = ex_volumes_dispose;
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
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

			if (g_file_equal(v->root, r->root)) {
				v->size = g_file_info_get_attribute_uint64(
					i, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
				v->free = g_file_info_get_attribute_uint64(
					i, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
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
	SpaceReq *r = g_new0(SpaceReq, 1);

	r->owner = g_object_ref(v);
	r->root = g_object_ref(vol->root);
	g_file_query_filesystem_info_async(
		vol->root,
		G_FILE_ATTRIBUTE_FILESYSTEM_SIZE
		"," G_FILE_ATTRIBUTE_FILESYSTEM_FREE
		"," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
		G_PRIORITY_LOW, NULL, got_space, r);
}

static gboolean have_root(GPtrArray *list, GFile *root)
{
	for (guint i = 0; i < list->len; i++)
		if (g_file_equal(((ExVolume *)g_ptr_array_index(list, i))->root,
				 root))
			return TRUE;
	return FALSE;
}

/* The filesystems a person keeps files on, from the mount table.  The
 * kernel's own (proc, sysfs, devfs) are skipped. */
static void add_system_mounts(GPtrArray *list)
{
	char *data = NULL;
	gboolean root_seen = FALSE;

	if (g_file_get_contents("/proc/mounts", &data, NULL, NULL)) {
		char **lines = g_strsplit(data, "\n", -1);

		for (int i = 0; lines[i]; i++) {
			char **f = g_strsplit_set(lines[i], " \t", -1);

			if (g_strv_length(f) >= 3) {
				const char *dir = f[1], *type = f[2];

				if (strcmp(type, "proc") && strcmp(type, "sysfs") &&
				    strcmp(type, "devfs") && strcmp(type, "devtmpfs") &&
				    strcmp(type, "devpts") &&
				    (strcmp(dir, "/") == 0 ||
				     g_str_has_prefix(dir, "/media") ||
				     g_str_has_prefix(dir, "/mnt") ||
				     strcmp(dir, "/ram") == 0)) {
					GFile *root = g_file_new_for_path(dir);

					if (!have_root(list, root)) {
						ExVolume *v = g_new0(ExVolume, 1);
						char *b = g_path_get_basename(dir);

						v->root = root;
						v->fs_type = g_strdup(type);
						if (strcmp(dir, "/") == 0) {
							v->name = g_strdup("Local Disk");
							v->icon_name = g_strdup("drive-harddisk");
							root_seen = TRUE;
						} else if (strcmp(type, "tmpfs") == 0) {
							v->name = g_strdup("RAM Disk");
							v->icon_name = g_strdup("media-flash");
						} else {
							v->name = g_strdup(b);
							v->icon_name = g_strdup("drive-harddisk");
						}
						g_free(b);
						g_ptr_array_add(list, v);
					} else {
						g_object_unref(root);
					}
				}
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
			v->name = g_strdup("Local Disk");
			v->icon_name = g_strdup("drive-harddisk");
			v->fs_type = g_strdup("");
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

static void rebuild(ExVolumes *v)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(
		(GDestroyNotify)volume_free);
	GList *mounts = v->mon ? g_volume_monitor_get_mounts(v->mon) : NULL;

	add_system_mounts(list);
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
		if (vol->network)
			v->have_network = TRUE;
		g_clear_object(&icon);
		g_ptr_array_add(list, vol);
	}
	g_list_free_full(mounts, g_object_unref);
	g_clear_pointer(&v->list, g_ptr_array_unref);
	v->list = list;
	for (guint i = 0; i < list->len; i++)
		ask_space(v, g_ptr_array_index(list, i));
	g_signal_emit(v, sigs[SIG_CHANGED], 0);
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
		char *uri = g_file_get_uri(vol->root);
		size_t len = strlen(uri);

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

void ex_volumes_refresh(ExVolumes *v)
{
	for (guint i = 0; v->list && i < v->list->len; i++)
		ask_space(v, g_ptr_array_index(v->list, i));
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
