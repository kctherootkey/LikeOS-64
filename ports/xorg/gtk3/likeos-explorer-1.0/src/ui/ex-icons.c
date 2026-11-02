// LikeOS -- File Explorer: icons for items.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-icons.h"
#include "backend/ex-thumbs.h"

static GHashTable *g_cache; /* "key@size@scale" -> GdkPixbuf */

static void theme_changed(GtkIconTheme *t, gpointer data)
{
	if (g_cache)
		g_hash_table_remove_all(g_cache);
}

static GHashTable *cache(void)
{
	if (!g_cache) {
		g_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
						g_object_unref);
		g_signal_connect(gtk_icon_theme_get_default(), "changed",
				 G_CALLBACK(theme_changed), NULL);
	}
	return g_cache;
}

GdkPixbuf *ex_icons_gicon(GIcon *icon, int size, int scale)
{
	char *s = icon ? g_icon_to_string(icon) : NULL;
	char *key = g_strdup_printf("%s@%d@%d", s ? s : "-", size, scale);
	GdkPixbuf *pb = g_hash_table_lookup(cache(), key);

	if (pb) {
		g_free(key);
		g_free(s);
		return g_object_ref(pb);
	}
	GtkIconInfo *ii = icon ? gtk_icon_theme_lookup_by_gicon_for_scale(
					 gtk_icon_theme_get_default(), icon,
					 size, scale, GTK_ICON_LOOKUP_FORCE_SIZE) :
				 NULL;
	if (!ii)
		ii = gtk_icon_theme_lookup_icon_for_scale(
			gtk_icon_theme_get_default(), "text-x-generic", size,
			scale, GTK_ICON_LOOKUP_FORCE_SIZE);
	if (ii) {
		pb = gtk_icon_info_load_icon(ii, NULL);
		g_object_unref(ii);
	}
	if (pb)
		g_hash_table_insert(cache(), key, g_object_ref(pb));
	else
		g_free(key);
	g_free(s);
	return pb;
}

GdkPixbuf *ex_icons_named(const char *name, int size, int scale)
{
	GIcon *i = g_themed_icon_new_with_default_fallbacks(name);
	GdkPixbuf *pb = ex_icons_gicon(i, size, scale);

	g_object_unref(i);
	return pb;
}

static GdkPixbuf *dim(GdkPixbuf *src)
{
	GdkPixbuf *out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
					gdk_pixbuf_get_width(src),
					gdk_pixbuf_get_height(src));

	gdk_pixbuf_fill(out, 0);
	gdk_pixbuf_composite(src, out, 0, 0, gdk_pixbuf_get_width(src),
			     gdk_pixbuf_get_height(src), 0, 0, 1, 1,
			     GDK_INTERP_NEAREST, 128);
	return out;
}

GdkPixbuf *ex_icons_for_file(ExFile *f, int size, int scale, gboolean dimmed,
			     gboolean thumbnails)
{
	GdkPixbuf *pb = NULL;

	if (thumbnails && size >= 48) {
		GdkPixbuf *th = ex_file_get_thumbnail(f);

		if (th) {
			int w = gdk_pixbuf_get_width(th);
			int h = gdk_pixbuf_get_height(th);
			int box = size * scale;
			double s = (double)box / MAX(w, h);

			pb = s >= 1.0 ? g_object_ref(th) :
					gdk_pixbuf_scale_simple(
						th, MAX(1, (int)(w * s)),
						MAX(1, (int)(h * s)),
						GDK_INTERP_BILINEAR);
		} else if (!ex_file_thumbnail_failed(f)) {
			ex_thumbs_request(f, size > 128 ? EX_THUMB_LARGE :
							  EX_THUMB_NORMAL);
		}
	}
	if (!pb)
		pb = ex_icons_gicon(ex_file_get_icon(f), size, scale);
	if (pb && dimmed) {
		GdkPixbuf *d = dim(pb);

		g_object_unref(pb);
		pb = d;
	}
	return pb;
}
