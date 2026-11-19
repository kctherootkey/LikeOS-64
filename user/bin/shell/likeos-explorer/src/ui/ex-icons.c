// LikeOS Explorer: icons for items.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-icons.h"
#include "backend/ex-thumbs.h"
#include "backend/ex-util.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>

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

/* A thumbnail made ready for one view size: scaled into the box and, for a
 * picture or a video, framed.  Kept on the item, one per box size, so a
 * view that redraws does not scale the same picture again each time; the
 * thumbnail it was made from tells when it is stale. */
typedef struct {
	GdkPixbuf *src;
	GdkPixbuf *out;
} Framed;

static void framed_free(gpointer data)
{
	Framed *fr = data;

	g_object_unref(fr->src);
	g_object_unref(fr->out);
	g_free(fr);
}

/* The hairline round a picture: a light grey line round a thumbnail, with
 * the picture on white inside it where it is clear. */
#define EX_FRAME_RGBA 0xd0d0d0ffu

static GdkPixbuf *frame(GdkPixbuf *src, int line)
{
	int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
	GdkPixbuf *out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
					w + 2 * line, h + 2 * line);
	GdkPixbuf *inner;

	gdk_pixbuf_fill(out, EX_FRAME_RGBA);
	inner = gdk_pixbuf_new_subpixbuf(out, line, line, w, h);
	gdk_pixbuf_fill(inner, 0xffffffffu);
	g_object_unref(inner);
	gdk_pixbuf_composite(src, out, line, line, w, h, line, line, 1, 1,
			     GDK_INTERP_NEAREST, 255);
	return out;
}

static GdkPixbuf *thumbnail_for_box(ExFile *f, GdkPixbuf *th, int box,
				    int scale)
{
	const char *kind = ex_file_get_kind(f);
	gboolean framed = !strcmp(kind, "image") || !strcmp(kind, "video");
	int line = framed ? scale : 0;
	char key[48];
	Framed *fr;
	int w = gdk_pixbuf_get_width(th), h = gdk_pixbuf_get_height(th);
	double s;
	GdkPixbuf *scaled;

	g_snprintf(key, sizeof(key), "ex-thumb-%d", box);
	fr = g_object_get_data(G_OBJECT(f), key);
	if (fr && fr->src == th)
		return g_object_ref(fr->out);
	/* Room for the frame inside the box, so framed and unframed items
	 * line up. */
	s = (double)(box - 2 * line) / MAX(w, h);
	scaled = s >= 1.0 ? g_object_ref(th) :
			    gdk_pixbuf_scale_simple(th, MAX(1, (int)(w * s)),
						    MAX(1, (int)(h * s)),
						    GDK_INTERP_BILINEAR);
	fr = g_new0(Framed, 1);
	fr->src = g_object_ref(th);
	if (framed) {
		fr->out = frame(scaled, line);
		g_object_unref(scaled);
	} else {
		fr->out = scaled;
	}
	g_object_set_data_full(G_OBJECT(f), key, fr, framed_free);
	return g_object_ref(fr->out);
}

/* ---- shortcuts ----------------------------------------------------------------- */

gboolean ex_icons_is_shortcut(ExFile *f)
{
	return !ex_file_is_dir(f) &&
	       g_str_has_suffix(ex_file_get_name(f), ".desktop");
}

/* A shortcut (a .desktop file) shows the picture of what it starts: the
 * Icon= of the file, read the first time the item is drawn and kept on
 * it. */
static GIcon *shortcut_icon(ExFile *f)
{
	GIcon *icon = g_object_get_data(G_OBJECT(f), "ex-shortcut-icon");
	char *path;

	if (icon || g_object_get_data(G_OBJECT(f), "ex-shortcut-none"))
		return icon;
	path = g_file_get_path(ex_file_get_file(f));
	if (path) {
		GKeyFile *kf = g_key_file_new();
		char *name = NULL;

		/* The key file alone: an application that is not installed
		 * (or is hidden) still has its picture. */
		if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
			name = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
						     G_KEY_FILE_DESKTOP_KEY_ICON,
						     NULL);
		if (name && *name) {
			GFile *file = g_path_is_absolute(name) ?
					      g_file_new_for_path(name) :
					      NULL;

			icon = file ? g_file_icon_new(file) :
				      g_themed_icon_new(name);
			g_clear_object(&file);
		}
		g_free(name);
		g_key_file_unref(kf);
		g_free(path);
	}
	if (icon)
		g_object_set_data_full(G_OBJECT(f), "ex-shortcut-icon", icon,
				       g_object_unref);
	else
		g_object_set_data(G_OBJECT(f), "ex-shortcut-none",
				  GINT_TO_POINTER(1));
	return icon;
}

/* The shortcut arrow in the lower left corner: a white tile with a hairline
 * and a blue arrow bending up to the right, about a third of the icon. */
static GdkPixbuf *with_arrow(GdkPixbuf *src, int scale)
{
	int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
	int a = MAX(8 * scale, MIN(w, h) * 3 / 8);
	cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(cs);
	double x = 0.5, y = h - a + 0.5, s = a - 1, lw = MAX(1.0, a / 10.0);
	GdkPixbuf *out;

	gdk_cairo_set_source_pixbuf(cr, src, 0, 0);
	cairo_paint(cr);
	cairo_rectangle(cr, x, y, s, s);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	/* The arrow: a curve from the bottom left to the top right, and its
	 * head. */
	cairo_set_source_rgb(cr, 0, 0.47, 0.83);
	cairo_set_line_width(cr, lw);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, x + s * 0.28, y + s * 0.8);
	cairo_curve_to(cr, x + s * 0.28, y + s * 0.45, x + s * 0.45, y + s * 0.3,
		       x + s * 0.74, y + s * 0.3);
	cairo_stroke(cr);
	cairo_move_to(cr, x + s * 0.52, y + s * 0.14);
	cairo_line_to(cr, x + s * 0.76, y + s * 0.3);
	cairo_line_to(cr, x + s * 0.52, y + s * 0.47);
	cairo_stroke(cr);
	cairo_destroy(cr);
	out = gdk_pixbuf_get_from_surface(cs, 0, 0, w, h);
	cairo_surface_destroy(cs);
	return out;
}

GdkPixbuf *ex_icons_for_file(ExFile *f, int size, int scale, gboolean dimmed,
			     gboolean thumbnails)
{
	GdkPixbuf *pb = NULL;

	if (ex_icons_is_shortcut(f)) {
		GIcon *icon = shortcut_icon(f);

		pb = ex_icons_gicon(icon ? icon : ex_file_get_icon(f), size,
				    scale);
		if (pb) {
			GdkPixbuf *a = with_arrow(pb, scale);

			g_object_unref(pb);
			pb = a;
		}
		thumbnails = FALSE;
	}

	if (thumbnails && size >= 32) {
		GdkPixbuf *th = ex_file_get_thumbnail(f);

		if (th) {
			pb = thumbnail_for_box(f, th, size * scale, scale);
			/* Extra large icons: the normal thumbnail is half the
			 * size; the large one is asked for, once. */
			if (size > 128 &&
			    MAX(gdk_pixbuf_get_width(th),
				gdk_pixbuf_get_height(th)) < size * scale * 3 / 4 &&
			    !g_object_get_data(G_OBJECT(f), "ex-thumb-large")) {
				g_object_set_data(G_OBJECT(f), "ex-thumb-large",
						  GINT_TO_POINTER(1));
				ex_thumbs_request(f, EX_THUMB_LARGE);
			}
		} else if (!ex_file_thumbnail_failed(f))
			ex_thumbs_request(f, size > 128 ? EX_THUMB_LARGE :
							  EX_THUMB_NORMAL);
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

/* ---- type names ------------------------------------------------------------ */

/* Extensions whose type is named in words rather than "XYZ file". */
static const struct {
	const char *ext, *label;
} named_types[] = {
	{ "txt", "Text document" },
	{ "log", "Text document" },
	{ "desktop", "Shortcut" },
	{ "lnk", "Shortcut" },
	{ "zip", "ZIP archive" },
	{ "htm", "HTML document" },
	{ "html", "HTML document" },
	{ "pdf", "PDF document" },
	{ "exe", "Program" },
	{ "appimage", "Program" },
	{ "rtf", "Rich text document" },
	{ "odt", "OpenDocument text" },
	{ "ods", "OpenDocument spreadsheet" },
	{ "odp", "OpenDocument presentation" },
	{ "docx", "Text document (DOCX)" },
	{ "xlsx", "Spreadsheet (XLSX)" },
	{ "pptx", "Presentation (PPTX)" },
	{ "iso", "Disc image" },
	{ "ttf", "TrueType font" },
	{ "otf", "OpenType font" },
};

char *ex_icons_kind_label(ExFile *f)
{
	const char *name, *ext;

	if (ex_file_is_dir(f))
		return g_strdup("Folder");
	name = ex_file_get_name(f);
	ext = ex_name_extension(name);
	if (*ext) {
		/* The last extension names the type: "a.tar.gz" is a GZ
		 * file. */
		const char *last = strrchr(ext, '.');
		char *low = g_ascii_strdown(last + 1, -1);
		char *out = NULL;

		for (gsize i = 0; i < G_N_ELEMENTS(named_types) && !out; i++)
			if (!strcmp(low, named_types[i].ext))
				out = g_strdup(named_types[i].label);
		if (!out) {
			char *up = g_ascii_strup(last + 1, -1);

			out = g_strdup_printf("%s file", up);
			g_free(up);
		}
		g_free(low);
		return out;
	}
	if (!strcmp(ex_file_get_kind(f), "program"))
		return g_strdup("Program");
	return g_strdup("File");
}
