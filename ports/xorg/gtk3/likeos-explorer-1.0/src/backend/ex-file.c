// LikeOS -- File Explorer: one item in a listing.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-file.h"
#include "backend/ex-util.h"

#include <string.h>

struct _ExFile {
	GObject parent;
	GFile *file;
	GFileInfo *info;
	char *collate_key;
	ExFileCaps caps;
	char *trash_orig;
	gint64 trash_deleted;
	GdkPixbuf *thumb;
	gboolean thumb_failed;
};

G_DEFINE_TYPE(ExFile, ex_file, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGS };
static guint sigs[N_SIGS];

static void ex_file_finalize(GObject *o)
{
	ExFile *f = EX_FILE(o);

	g_clear_object(&f->file);
	g_clear_object(&f->info);
	g_clear_object(&f->thumb);
	g_free(f->collate_key);
	g_free(f->trash_orig);
	G_OBJECT_CLASS(ex_file_parent_class)->finalize(o);
}

static void ex_file_class_init(ExFileClass *k)
{
	G_OBJECT_CLASS(k)->finalize = ex_file_finalize;
	sigs[SIG_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
					 G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					 NULL, G_TYPE_NONE, 0);
}

static void ex_file_init(ExFile *f)
{
}

/* An attribute GIO did not report reads as "allowed": a backend that
 * cannot tell is not a reason to grey out every command. */
static gboolean access_bit(GFileInfo *i, const char *attr)
{
	if (!g_file_info_has_attribute(i, attr))
		return TRUE;
	return g_file_info_get_attribute_boolean(i, attr);
}

static void compute(ExFile *f)
{
	GFileInfo *i = f->info;
	ExFileCaps c = 0;

	g_free(f->collate_key);
	f->collate_key = ex_collate_key(ex_file_get_name(f));
	if (access_bit(i, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
		c |= EX_CAP_READ | EX_CAP_OPEN;
	if (access_bit(i, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
		c |= EX_CAP_WRITE;
	if (access_bit(i, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME))
		c |= EX_CAP_RENAME;
	if (access_bit(i, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE))
		c |= EX_CAP_DELETE;
	/* Trashing is the one GIO decides by location (no trash on a system
	 * mount); trust only a yes. */
	if (g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH) &&
	    g_file_info_get_attribute_boolean(i,
					      G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH))
		c |= EX_CAP_TRASH;
	if (g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE) &&
	    g_file_info_get_attribute_boolean(
		    i, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
		c |= EX_CAP_EXECUTE;
	if (f->trash_orig)
		c = (c & ~(EX_CAP_TRASH | EX_CAP_RENAME)) | EX_CAP_RESTORE;
	f->caps = c;
}

ExFile *ex_file_new(GFile *file, GFileInfo *info)
{
	ExFile *f = g_object_new(EX_TYPE_FILE, NULL);

	f->file = g_object_ref(file);
	f->info = info ? g_object_ref(info) : g_file_info_new();
	compute(f);
	return f;
}

ExFile *ex_file_new_query(GFile *file, GCancellable *cancellable,
			  GError **error)
{
	/* Followed, as a listing does (ex-dir-loader.c); a broken link is
	 * then asked about without following, so it is still an item. */
	GFileInfo *info = g_file_query_info(file, EX_FILE_ATTRS,
					    G_FILE_QUERY_INFO_NONE,
					    cancellable, NULL);
	if (!info)
		info = g_file_query_info(file, EX_FILE_ATTRS,
					 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					 cancellable, error);
	ExFile *f;

	if (!info)
		return NULL;
	f = ex_file_new(file, info);
	g_object_unref(info);
	return f;
}

GFile *ex_file_get_file(ExFile *f)
{
	return f->file;
}

GFileInfo *ex_file_get_info(ExFile *f)
{
	return f->info;
}

void ex_file_set_info(ExFile *f, GFileInfo *info)
{
	g_return_if_fail(G_IS_FILE_INFO(info));
	g_set_object(&f->info, info);
	/* What the old thumbnail showed may be gone. */
	g_clear_object(&f->thumb);
	f->thumb_failed = FALSE;
	compute(f);
	g_signal_emit(f, sigs[SIG_CHANGED], 0);
}

const char *ex_file_get_name(ExFile *f)
{
	const char *n = NULL;

	if (g_file_info_has_attribute(f->info,
				      G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME))
		n = g_file_info_get_display_name(f->info);
	if (!n || !*n) {
		if (g_file_info_has_attribute(f->info,
					      G_FILE_ATTRIBUTE_STANDARD_NAME))
			n = g_file_info_get_name(f->info);
	}
	return n ? n : "";
}

const char *ex_file_get_edit_name(ExFile *f)
{
	if (g_file_info_has_attribute(f->info,
				      G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME)) {
		const char *n = g_file_info_get_edit_name(f->info);

		if (n && *n)
			return n;
	}
	return ex_file_get_name(f);
}

const char *ex_file_get_collate_key(ExFile *f)
{
	return f->collate_key;
}

const char *ex_file_get_content_type(ExFile *f)
{
	const char *t = NULL;

	if (g_file_info_has_attribute(f->info,
				      G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE))
		t = g_file_info_get_content_type(f->info);
	if (!t && g_file_info_has_attribute(
			  f->info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE))
		t = g_file_info_get_attribute_string(
			f->info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
	if (!t)
		t = ex_file_is_dir(f) ? "inode/directory" :
					"application/octet-stream";
	return t;
}

char *ex_file_get_type_description(ExFile *f)
{
	return g_content_type_get_description(ex_file_get_content_type(f));
}

GIcon *ex_file_get_icon(ExFile *f)
{
	if (g_file_info_has_attribute(f->info, G_FILE_ATTRIBUTE_STANDARD_ICON))
		return g_file_info_get_icon(f->info);
	return NULL;
}

guint64 ex_file_get_size(ExFile *f)
{
	if (!g_file_info_has_attribute(f->info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
		return 0;
	return (guint64)g_file_info_get_size(f->info);
}

gint64 ex_file_get_mtime(ExFile *f)
{
	if (!g_file_info_has_attribute(f->info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
		return 0;
	return (gint64)g_file_info_get_attribute_uint64(
		f->info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
}

static GFileType file_type(ExFile *f)
{
	if (!g_file_info_has_attribute(f->info, G_FILE_ATTRIBUTE_STANDARD_TYPE))
		return G_FILE_TYPE_UNKNOWN;
	return g_file_info_get_file_type(f->info);
}

gboolean ex_file_is_dir(ExFile *f)
{
	GFileType t = file_type(f);

	/* Listings follow links, so a link to a folder reports the folder's
	 * type here. */
	return t == G_FILE_TYPE_DIRECTORY || t == G_FILE_TYPE_MOUNTABLE;
}

gboolean ex_file_is_hidden(ExFile *f)
{
	if (g_file_info_has_attribute(f->info,
				      G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) &&
	    g_file_info_get_is_hidden(f->info))
		return TRUE;
	if (g_file_info_has_attribute(f->info,
				      G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP) &&
	    g_file_info_get_is_backup(f->info))
		return TRUE;
	return ex_name_is_hidden(g_file_info_get_name(f->info));
}

gboolean ex_file_is_symlink(ExFile *f)
{
	return g_file_info_has_attribute(f->info,
					 G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) &&
	       g_file_info_get_is_symlink(f->info);
}

ExFileCaps ex_file_get_caps(ExFile *f)
{
	return f->caps;
}

char *ex_file_get_kind_label(ExFile *f)
{
	if (ex_file_is_dir(f))
		return g_strdup("File folder");
	const char *ext = ex_name_extension(ex_file_get_name(f));
	if (*ext) {
		char *up = g_utf8_strup(ext + 1, -1);
		char *s = g_strdup_printf("%s file", up);

		g_free(up);
		return s;
	}
	return ex_file_get_type_description(f);
}

const char *ex_file_get_kind(ExFile *f)
{
	const char *ct;

	if (ex_file_is_dir(f))
		return "folder";
	ct = ex_file_get_content_type(f);
	if (g_str_has_prefix(ct, "image/"))
		return "image";
	if (g_str_has_prefix(ct, "video/"))
		return "video";
	if (g_str_has_prefix(ct, "audio/"))
		return "audio";
	if (g_str_has_prefix(ct, "text/") ||
	    g_content_type_is_a(ct, "application/pdf") ||
	    strstr(ct, "document") || strstr(ct, "opendocument") ||
	    strstr(ct, "msword") || strstr(ct, "spreadsheet") ||
	    strstr(ct, "presentation"))
		return "document";
	if (strstr(ct, "zip") || strstr(ct, "tar") ||
	    strstr(ct, "compressed") || strstr(ct, "archive") ||
	    strstr(ct, "x-7z") || strstr(ct, "rar"))
		return "archive";
	if (g_content_type_can_be_executable(ct) &&
	    (f->caps & EX_CAP_EXECUTE))
		return "program";
	return "other";
}

void ex_file_set_trash_info(ExFile *f, const char *orig_path, gint64 deleted)
{
	g_free(f->trash_orig);
	f->trash_orig = g_strdup(orig_path);
	f->trash_deleted = deleted;
	compute(f);
}

const char *ex_file_get_trash_orig_path(ExFile *f)
{
	return f->trash_orig;
}

gint64 ex_file_get_trash_deleted(ExFile *f)
{
	return f->trash_deleted;
}

GdkPixbuf *ex_file_get_thumbnail(ExFile *f)
{
	return f->thumb;
}

void ex_file_set_thumbnail(ExFile *f, GdkPixbuf *thumb)
{
	g_set_object(&f->thumb, thumb);
	g_signal_emit(f, sigs[SIG_CHANGED], 0);
}

gboolean ex_file_thumbnail_failed(ExFile *f)
{
	return f->thumb_failed;
}

void ex_file_set_thumbnail_failed(ExFile *f)
{
	f->thumb_failed = TRUE;
}
