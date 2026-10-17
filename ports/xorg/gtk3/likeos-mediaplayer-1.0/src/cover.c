// LikeOS -- Media Player: cover art.
//
// Two sources, in order: a picture embedded in the file's tags (ID3 APIC,
// MP4 covr, Vorbis METADATA_BLOCK_PICTURE, Matroska attachments -- GStreamer
// presents them all as an image tag), then a picture file next to the track
// with one of the names rippers and stores use ("cover.jpg", "folder.jpg").
//
// Both come from files the user did not necessarily make, so the decoded
// size is bounded before any pixels are allocated: the loader is told the
// target size as soon as it knows the real one.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <glib/gstdio.h>
#include <gst/tag/tag.h>

#include "mediaplayer.h"
#include "util.h"

#define COVER_MAX_BYTES (16 * 1024 * 1024)
#define COVER_MAX_EDGE 1024
#define FOLDER_SCAN_MAX 2048

static void on_size_prepared(GdkPixbufLoader *loader, gint w, gint h,
			     gpointer data)
{
	(void)data;
	if (w <= 0 || h <= 0)
		return;
	if (w <= COVER_MAX_EDGE && h <= COVER_MAX_EDGE)
		return;
	double s = (double)COVER_MAX_EDGE / (w > h ? w : h);
	gint nw = (gint)(w * s + 0.5), nh = (gint)(h * s + 0.5);
	gdk_pixbuf_loader_set_size(loader, nw > 0 ? nw : 1, nh > 0 ? nh : 1);
}

static GdkPixbuf *pixbuf_from_sample(GstSample *sample)
{
	GstBuffer *buf = gst_sample_get_buffer(sample);
	if (buf == NULL)
		return NULL;
	gsize size = gst_buffer_get_size(buf);
	if (size == 0 || size > COVER_MAX_BYTES)
		return NULL;

	GstMapInfo map;
	if (!gst_buffer_map(buf, &map, GST_MAP_READ))
		return NULL;

	GdkPixbuf *pb = NULL;
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	g_signal_connect(loader, "size-prepared", G_CALLBACK(on_size_prepared),
			 NULL);
	gboolean ok = gdk_pixbuf_loader_write(loader, map.data, map.size,
					      NULL);
	/* close() must run even after a failed write, or the loader warns
	 * about being finalised while open. */
	ok = gdk_pixbuf_loader_close(loader, NULL) && ok;
	if (ok) {
		pb = gdk_pixbuf_loader_get_pixbuf(loader);
		if (pb != NULL)
			g_object_ref(pb);
	}
	g_object_unref(loader);
	gst_buffer_unmap(buf, &map);
	return pb;
}

static int image_type_of(GstSample *sample)
{
	const GstStructure *info = gst_sample_get_info(sample);
	int type = GST_TAG_IMAGE_TYPE_UNDEFINED;
	if (info != NULL)
		gst_structure_get_enum(info, "image-type",
				       GST_TYPE_TAG_IMAGE_TYPE, &type);
	return type;
}

GdkPixbuf *mp_cover_from_tags(const GstTagList *tags)
{
	if (tags == NULL)
		return NULL;

	/* The front cover when the file says which picture that is; the
	 * first picture otherwise; the preview image last. */
	guint n = gst_tag_list_get_tag_size(tags, GST_TAG_IMAGE);
	gint best = -1;
	for (guint i = 0; i < n && i < 32; i++) {
		GstSample *s = NULL;
		if (!gst_tag_list_get_sample_index(tags, GST_TAG_IMAGE, i, &s))
			continue;
		int t = image_type_of(s);
		gst_sample_unref(s);
		if (t == GST_TAG_IMAGE_TYPE_FRONT_COVER) {
			best = (gint)i;
			break;
		}
		if (best < 0)
			best = (gint)i;
	}
	if (best >= 0) {
		GstSample *s = NULL;
		if (gst_tag_list_get_sample_index(tags, GST_TAG_IMAGE,
						  (guint)best, &s)) {
			GdkPixbuf *pb = pixbuf_from_sample(s);
			gst_sample_unref(s);
			if (pb != NULL)
				return pb;
		}
	}

	GstSample *s = NULL;
	if (gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &s)) {
		GdkPixbuf *pb = pixbuf_from_sample(s);
		gst_sample_unref(s);
		return pb;
	}
	return NULL;
}

/* How good a name is for a folder picture: higher wins, 0 is no match. */
static int folder_name_score(const char *name)
{
	static const char *const stems[] = {
		"cover", "folder", "front", "album", "albumart", "thumb",
		"albumartsmall", NULL
	};
	static const char *const exts[] = {
		"jpg", "jpeg", "png", "webp", "gif", "bmp", NULL
	};

	const char *dot = strrchr(name, '.');
	if (dot == NULL)
		return 0;
	gboolean ext_ok = FALSE;
	for (int i = 0; exts[i] != NULL; i++) {
		if (g_ascii_strcasecmp(dot + 1, exts[i]) == 0)
			ext_ok = TRUE;
	}
	if (!ext_ok)
		return 0;

	gsize stem_len = (gsize)(dot - name);
	for (int i = 0; stems[i] != NULL; i++) {
		if (strlen(stems[i]) == stem_len &&
		    g_ascii_strncasecmp(name, stems[i], stem_len) == 0)
			return 100 - i;
	}
	/* "AlbumArt_{GUID}_Large.jpg", as Windows Media Player wrote them. */
	if (g_ascii_strncasecmp(name, "albumart", 8) == 0)
		return 20;
	return 0;
}

GdkPixbuf *mp_cover_from_folder(const char *uri, gchar **source_out)
{
	gchar *path = mp_uri_to_path(uri);
	if (path == NULL)
		return NULL;
	gchar *dir = g_path_get_dirname(path);
	g_free(path);

	GDir *d = g_dir_open(dir, 0, NULL);
	if (d == NULL) {
		g_free(dir);
		return NULL;
	}

	gchar *best = NULL;
	int best_score = 0;
	const char *name;
	int seen = 0;
	while ((name = g_dir_read_name(d)) != NULL && seen++ < FOLDER_SCAN_MAX) {
		int sc = folder_name_score(name);
		if (sc > best_score) {
			best_score = sc;
			g_free(best);
			best = g_strdup(name);
		}
	}
	g_dir_close(d);

	GdkPixbuf *pb = NULL;
	if (best != NULL) {
		gchar *file = g_build_filename(dir, best, NULL);
		GStatBuf st;
		if (g_stat(file, &st) == 0 && S_ISREG(st.st_mode) &&
		    st.st_size > 0 && st.st_size <= COVER_MAX_BYTES) {
			pb = gdk_pixbuf_new_from_file_at_scale(
				file, COVER_MAX_EDGE, COVER_MAX_EDGE, TRUE,
				NULL);
			if (pb != NULL && source_out != NULL)
				*source_out = g_filename_display_name(file);
		}
		g_free(file);
		g_free(best);
	}
	g_free(dir);
	return pb;
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
			 double r)
{
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
}

void mp_cover_draw(cairo_t *cr, GdkPixbuf *cover, double x, double y,
		   double w, double h, double radius, const char *fallback_icon,
		   GtkWidget *widget)
{
	if (w < 2 || h < 2)
		return;

	cairo_save(cr);
	if (cover != NULL) {
		int pw = gdk_pixbuf_get_width(cover);
		int ph = gdk_pixbuf_get_height(cover);
		double s = MIN(w / pw, h / ph);
		double dw = pw * s, dh = ph * s;
		double dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;

		/* A soft shadow under the picture, as a card has. */
		for (int i = 4; i >= 1; i--) {
			rounded_rect(cr, dx - i, dy - i + 2, dw + 2 * i,
				     dh + 2 * i, radius + i);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.035);
			cairo_fill(cr);
		}
		rounded_rect(cr, dx, dy, dw, dh, radius);
		cairo_clip(cr);
		cairo_translate(cr, dx, dy);
		cairo_scale(cr, s, s);
		gdk_cairo_set_source_pixbuf(cr, cover, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr),
					 CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		return;
	}

	/* No picture: a square tile in the accent colour with a large
	 * symbolic note (or film strip) on it. */
	double side = MIN(w, h);
	double dx = x + (w - side) / 2, dy = y + (h - side) / 2;
	cairo_pattern_t *g = cairo_pattern_create_linear(dx, dy, dx + side,
							 dy + side);
	cairo_pattern_add_color_stop_rgb(g, 0.0, 0.0, 0.47, 0.83);
	cairo_pattern_add_color_stop_rgb(g, 1.0, 0.0, 0.30, 0.62);
	rounded_rect(cr, dx, dy, side, side, radius);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);

	if (fallback_icon != NULL && widget != NULL) {
		int isz = (int)(side * 0.45);
		if (isz >= 8) {
			GtkIconTheme *th = gtk_icon_theme_get_default();
			GtkIconInfo *ii = gtk_icon_theme_lookup_icon(
				th, fallback_icon, isz,
				GTK_ICON_LOOKUP_FORCE_SIZE);
			if (ii != NULL) {
				GdkRGBA white = { 1, 1, 1, 0.92 };
				GdkPixbuf *ip = gtk_icon_info_load_symbolic(
					ii, &white, NULL, NULL, NULL, NULL,
					NULL);
				if (ip != NULL) {
					int iw = gdk_pixbuf_get_width(ip);
					int ih = gdk_pixbuf_get_height(ip);
					gdk_cairo_set_source_pixbuf(
						cr, ip, dx + (side - iw) / 2,
						dy + (side - ih) / 2);
					cairo_paint(cr);
					g_object_unref(ip);
				}
				g_object_unref(ii);
			}
		}
	}
	cairo_restore(cr);
}
