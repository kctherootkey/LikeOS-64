// LikeOS -- LikeOS Image Viewer: moving pictures to the Trash, and back.
//
// GIO moves the file into the Trash of its filesystem (the home Trash,
// ~/.local/share/Trash, for everything on the home filesystem; the
// filesystem's own .Trash-UID elsewhere) and writes the record next to it:
// a .trashinfo file with the original path and the time.  To undo, the
// record is looked up again -- the newest one for that path, since the same
// name may have been deleted before -- and the file is moved back, which
// is exactly what restoring from LikeOS Explorer's Trash does.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "imageviewer.h"

struct IvTrashRecord {
	char *orig;		/* where the file was */
	char *trash_dir;	/* the Trash it went to (with files/, info/) */
	char *name;		/* its name in files/ */
};

static char *home_trash(void)
{
	return g_build_filename(g_get_user_data_dir(), "Trash", NULL);
}

/* The mount point of the filesystem `path' is on: up the folders while
 * the device stays the same. */
static char *mount_top(const char *path)
{
	struct stat st, up;
	char *dir = g_path_get_dirname(path);

	if (stat(dir, &st) != 0)
		return dir;
	for (;;) {
		char *parent = g_path_get_dirname(dir);

		if (!strcmp(parent, dir) || stat(parent, &up) != 0 ||
		    up.st_dev != st.st_dev) {
			g_free(parent);
			return dir;
		}
		g_free(dir);
		dir = parent;
	}
}

/* The Trash directories a file at `path' may have gone to, most likely
 * first. */
static GPtrArray *candidate_trashes(const char *path)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
	char *top = mount_top(path);
	char *uid = g_strdup_printf("%u", (unsigned)getuid());

	g_ptr_array_add(a, home_trash());
	g_ptr_array_add(a, g_build_filename(top, ".Trash", uid, NULL));
	g_ptr_array_add(a, g_strdup_printf("%s/.Trash-%s", top, uid));
	g_free(uid);
	g_free(top);
	return a;
}

/* The record's Path= is relative to the trash's top directory when it does
 * not start with "/". */
static char *info_path_of(GKeyFile *kf, const char *trash_dir)
{
	char *raw = g_key_file_get_string(kf, "Trash Info", "Path", NULL);
	char *un, *out;

	if (!raw)
		return NULL;
	un = g_uri_unescape_string(raw, NULL);
	if (!un)
		un = g_strdup(raw);
	g_free(raw);
	if (un[0] == '/')
		return un;
	/* Relative to the filesystem the Trash is on: the parent of
	 * .Trash-UID, the grandparent of .Trash/UID. */
	char *parent = g_path_get_dirname(trash_dir);
	char *base = g_path_get_basename(parent);

	if (!strcmp(base, ".Trash")) {
		char *gp = g_path_get_dirname(parent);

		g_free(parent);
		parent = gp;
	}
	out = g_build_filename(parent, un, NULL);
	g_free(base);
	g_free(parent);
	g_free(un);
	return out;
}

static gboolean find_record(const char *orig, IvTrashRecord *r)
{
	GPtrArray *dirs = candidate_trashes(orig);
	char *best_name = NULL, *best_dir = NULL, *best_date = NULL;

	for (guint i = 0; i < dirs->len; i++) {
		const char *td = g_ptr_array_index(dirs, i);
		char *info = g_build_filename(td, "info", NULL);
		GDir *d = g_dir_open(info, 0, NULL);
		const char *n;

		while (d && (n = g_dir_read_name(d))) {
			if (!g_str_has_suffix(n, ".trashinfo"))
				continue;
			char *p = g_build_filename(info, n, NULL);
			GKeyFile *kf = g_key_file_new();

			if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) {
				char *path = info_path_of(kf, td);
				char *date = g_key_file_get_string(kf, "Trash Info",
								   "DeletionDate",
								   NULL);

				if (path && !strcmp(path, orig) &&
				    (!best_date || g_strcmp0(date, best_date) >= 0)) {
					g_free(best_name);
					g_free(best_dir);
					g_free(best_date);
					best_name = g_strndup(n, strlen(n) -
							      strlen(".trashinfo"));
					best_dir = g_strdup(td);
					best_date = g_strdup(date ? date : "");
				}
				g_free(date);
				g_free(path);
			}
			g_key_file_unref(kf);
			g_free(p);
		}
		if (d)
			g_dir_close(d);
		g_free(info);
		if (best_name)
			break;
	}
	g_ptr_array_unref(dirs);
	g_free(best_date);
	if (!best_name) {
		g_free(best_dir);
		return FALSE;
	}
	r->name = best_name;
	r->trash_dir = best_dir;
	return TRUE;
}

IvTrashRecord *iv_trash_file(const char *path, GError **error)
{
	GFile *f = g_file_new_for_path(path);
	IvTrashRecord *r;

	if (!g_file_trash(f, NULL, error)) {
		g_object_unref(f);
		return NULL;
	}
	g_object_unref(f);
	r = g_new0(IvTrashRecord, 1);
	r->orig = g_strdup(path);
	/* Without a record found the move still happened; Undo will say it
	 * cannot. */
	find_record(path, r);
	return r;
}

gboolean iv_trash_restore(IvTrashRecord *r, GError **error)
{
	char *from, *info;
	GFile *src, *dst;
	gboolean ok;

	if (!r->name || !r->trash_dir) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				    "The picture's Trash record was not found");
		return FALSE;
	}
	if (g_file_test(r->orig, G_FILE_TEST_EXISTS)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
				    "Another file now has the picture's name");
		return FALSE;
	}
	from = g_build_filename(r->trash_dir, "files", r->name, NULL);
	info = g_strdup_printf("%s/info/%s.trashinfo", r->trash_dir, r->name);
	src = g_file_new_for_path(from);
	dst = g_file_new_for_path(r->orig);
	ok = g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS |
				   G_FILE_COPY_ALL_METADATA,
			 NULL, NULL, NULL, error);
	if (ok)
		unlink(info);
	g_object_unref(dst);
	g_object_unref(src);
	g_free(info);
	g_free(from);
	return ok;
}

const char *iv_trash_record_path(IvTrashRecord *r)
{
	return r ? r->orig : NULL;
}

void iv_trash_record_free(IvTrashRecord *r)
{
	if (!r)
		return;
	g_free(r->orig);
	g_free(r->trash_dir);
	g_free(r->name);
	g_free(r);
}
