// LikeOS Explorer: the details and preview panes.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-preview-pane.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-media.h"
#include "backend/ex-thumbs.h"
#include "backend/ex-util.h"

#include <string.h>

static void destroy_child(GtkWidget *w, gpointer data)
{
	gtk_widget_destroy(w);
}

#define EX_PANE_WIDTH 300
#define EX_PANE_PICTURE 268
#define EX_TEXT_PREVIEW_BYTES (32 * 1024)

typedef struct {
	GtkWidget *root; /* scrolled window */
	GtkWidget *picture, *name, *kind, *heading, *grid, *text_sw, *text;
	ExSelection *sel;
	ExLocation *loc;
	char *mode;
	GCancellable *cancel;
	guint update_id;
	ExFile *shown; /* whose thumbnail we wait for */
	gulong shown_changed;
	int grid_rows;
} Pane;

static Pane *pane_of(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "ex-pane");
}

static void set_picture(Pane *p, GdkPixbuf *pb)
{
	int scale = gtk_widget_get_scale_factor(p->root);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;

	gtk_image_set_from_surface(GTK_IMAGE(p->picture), s);
	if (s)
		cairo_surface_destroy(s);
}

static void clear_grid(Pane *p)
{
	gtk_container_foreach(GTK_CONTAINER(p->grid),
			      destroy_child, NULL);
	p->grid_rows = 0;
}

static void add_fact(Pane *p, const char *label, const char *value)
{
	GtkWidget *l = gtk_label_new(label), *v = gtk_label_new(value);

	if (!value || !*value) {
		gtk_widget_destroy(l);
		gtk_widget_destroy(v);
		return;
	}
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_yalign(GTK_LABEL(l), 0);
	ex_style_add(l, EX_CLASS_DIM);
	gtk_label_set_xalign(GTK_LABEL(v), 0);
	gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_grid_attach(GTK_GRID(p->grid), l, 0, p->grid_rows, 1, 1);
	gtk_grid_attach(GTK_GRID(p->grid), v, 1, p->grid_rows, 1, 1);
	p->grid_rows++;
	gtk_widget_show(l);
	gtk_widget_show(v);
}

static void forget_shown(Pane *p)
{
	if (p->shown) {
		g_signal_handler_disconnect(p->shown, p->shown_changed);
		g_clear_object(&p->shown);
	}
}

static void thumb_arrived(ExFile *f, Pane *p)
{
	GdkPixbuf *th = ex_file_get_thumbnail(f);

	if (th) {
		int w = gdk_pixbuf_get_width(th), h = gdk_pixbuf_get_height(th);
		int box = EX_PANE_PICTURE * gtk_widget_get_scale_factor(p->root);
		double s = (double)box / MAX(w, h);
		GdkPixbuf *sc = s < 1.0 ? gdk_pixbuf_scale_simple(th, (int)(w * s),
								  (int)(h * s),
								  GDK_INTERP_BILINEAR) :
					  g_object_ref(th);

		set_picture(p, sc);
		g_object_unref(sc);
	}
}

static void got_media(GObject *src, GAsyncResult *res, gpointer data)
{
	Pane *p = data;
	ExMediaInfo *m = ex_media_describe_finish(res, NULL);

	if (!m)
		return; /* cancelled, or not media: nothing to add */
	if (m->duration_ms) {
		char *d = ex_media_format_duration(m->duration_ms);

		add_fact(p, "Length", d);
		g_free(d);
	}
	if (m->width) {
		char *d = g_strdup_printf("%d × %d", m->width, m->height);

		add_fact(p, "Frame size", d);
		g_free(d);
	}
	add_fact(p, "Title", m->title);
	add_fact(p, "Artist", m->artist);
	add_fact(p, "Album", m->album);
	ex_media_info_free(m);
}

static void got_image(GObject *src, GAsyncResult *res, gpointer data)
{
	Pane *p = data;
	GdkPixbuf *pb = gdk_pixbuf_new_from_stream_finish(res, NULL);

	if (pb) {
		GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation(pb);

		set_picture(p, rot);
		char *dim = g_strdup_printf("%d × %d",
					    gdk_pixbuf_get_width(rot),
					    gdk_pixbuf_get_height(rot));
		g_object_unref(rot);
		g_object_unref(pb);
		g_free(dim);
	}
	g_object_unref(src);
}

static void image_opened(GObject *src, GAsyncResult *res, gpointer data)
{
	Pane *p = data;
	GFileInputStream *in = g_file_read_finish(G_FILE(src), res, NULL);
	int box = EX_PANE_PICTURE * gtk_widget_get_scale_factor(p->root);

	if (!in)
		return;
	gdk_pixbuf_new_from_stream_at_scale_async(G_INPUT_STREAM(in), box, box,
						  TRUE, p->cancel, got_image, p);
}

static void got_text(GObject *src, GAsyncResult *res, gpointer data)
{
	Pane *p = data;
	GBytes *b = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res,
						     NULL);

	if (b) {
		gsize len;
		const char *d = g_bytes_get_data(b, &len);
		const char *end = NULL;

		/* Text only: a binary file shows no preview. */
		if (!g_utf8_validate(d, (gssize)len, &end) &&
		    (gsize)(end - d) < len - MIN(len, 4)) {
			g_bytes_unref(b);
			g_object_unref(src);
			return;
		}
		char *txt = g_strndup(d, (gsize)(end ? end - d : (gssize)len));
		GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->text));

		gtk_text_buffer_set_text(buf, txt, -1);
		gtk_widget_show(p->text_sw);
		g_free(txt);
		g_bytes_unref(b);
	}
	g_object_unref(src);
}

static void text_opened(GObject *src, GAsyncResult *res, gpointer data)
{
	Pane *p = data;
	GFileInputStream *in = g_file_read_finish(G_FILE(src), res, NULL);

	if (in)
		g_input_stream_read_bytes_async(G_INPUT_STREAM(in),
						EX_TEXT_PREVIEW_BYTES,
						G_PRIORITY_DEFAULT, p->cancel,
						got_text, p);
}

static void show_file(Pane *p, ExFile *f, gboolean preview)
{
	int scale = gtk_widget_get_scale_factor(p->root);
	GdkPixbuf *icon = ex_icons_for_file(f, 96, scale, FALSE, FALSE);
	const char *ct = ex_file_get_content_type(f);
	char *kind = ex_icons_kind_label(f);
	GFile *gf = ex_file_get_file(f);
	gboolean is_text = !ex_file_is_dir(f) &&
			   (g_content_type_is_a(ct, "text/plain") ||
			    g_str_has_prefix(ct, "text/")) &&
			   (ex_file_get_caps(f) & EX_CAP_READ);

	set_picture(p, icon);
	g_clear_object(&icon);
	gtk_label_set_text(GTK_LABEL(p->name), ex_file_get_name(f));
	gtk_label_set_text(GTK_LABEL(p->kind), kind);
	g_free(kind);

	if (!ex_file_is_dir(f)) {
		char *sz = ex_format_size(ex_file_get_size(f));

		add_fact(p, "Size", sz);
		g_free(sz);
	}
	char *mt = ex_format_time(ex_file_get_mtime(f));
	add_fact(p, "Date modified", mt);
	g_free(mt);
	if (ex_file_get_trash_orig_path(f)) {
		char *dt = ex_format_time(ex_file_get_trash_deleted(f));

		add_fact(p, "Original location", ex_file_get_trash_orig_path(f));
		add_fact(p, "Date deleted", dt);
		g_free(dt);
	} else {
		GFile *parent = g_file_get_parent(gf);
		char *where = parent ? g_file_get_parse_name(parent) : NULL;

		add_fact(p, "Location", where);
		g_free(where);
		g_clear_object(&parent);
	}
	if (ex_file_is_symlink(f)) {
		const char *t = g_file_info_get_symlink_target(ex_file_get_info(f));

		add_fact(p, "Link to", t);
	}

	/* The picture: an image's own pixels, else a thumbnail when one can be
	 * made, in the background. */
	if (g_str_has_prefix(ct, "image/") && ex_thumbs_supported(f)) {
		g_file_read_async(gf, G_PRIORITY_DEFAULT, p->cancel, image_opened, p);
	} else if (ex_thumbs_supported(f)) {
		p->shown = g_object_ref(f);
		p->shown_changed = g_signal_connect(f, "changed",
						    G_CALLBACK(thumb_arrived), p);
		if (ex_file_get_thumbnail(f))
			thumb_arrived(f, p);
		else
			ex_thumbs_request(f, EX_THUMB_LARGE);
	}
	if (g_str_has_prefix(ct, "audio/") || g_str_has_prefix(ct, "video/"))
		ex_media_describe_async(gf, p->cancel, got_media, p);
	if (preview && is_text)
		g_file_read_async(gf, G_PRIORITY_DEFAULT, p->cancel, text_opened, p);
	/* The preview pane is the item's contents alone:
	 * the picture or the text, and where there is neither, a word saying
	 * so -- the facts are the details pane's. */
	if (preview) {
		gtk_widget_hide(p->name);
		gtk_widget_hide(p->heading);
		gtk_widget_hide(p->grid);
		if (is_text || ex_thumbs_supported(f)) {
			gtk_widget_hide(p->kind);
			if (is_text)
				gtk_widget_hide(p->picture);
		} else {
			gtk_widget_hide(p->picture);
			gtk_label_set_text(GTK_LABEL(p->kind),
					   "No preview available.");
		}
	}
}

static void update(Pane *p)
{
	GPtrArray *items = ex_selection_get(p->sel);
	gboolean preview = !g_strcmp0(p->mode, "preview");

	if (p->cancel) {
		g_cancellable_cancel(p->cancel);
		g_object_unref(p->cancel);
	}
	p->cancel = g_cancellable_new();
	forget_shown(p);
	clear_grid(p);
	gtk_widget_show(p->picture);
	gtk_widget_show(p->name);
	gtk_widget_show(p->kind);
	gtk_widget_show(p->grid);
	gtk_widget_hide(p->heading);
	gtk_widget_hide(p->text_sw);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->text)),
				 "", 0);

	if (items->len == 1) {
		gtk_widget_show(p->heading);
		show_file(p, g_ptr_array_index(items, 0), preview);
	} else if (items->len > 1) {
		char *n = g_strdup_printf("%u items selected", items->len);
		char *sz = ex_format_size(ex_selection_total_size(p->sel));

		set_picture(p, NULL);
		{
			GdkPixbuf *pb = ex_icons_named("edit-select-all", 96,
						       gtk_widget_get_scale_factor(
							       p->root));
			set_picture(p, pb);
			g_clear_object(&pb);
		}
		gtk_label_set_text(GTK_LABEL(p->name), n);
		gtk_label_set_text(GTK_LABEL(p->kind), "");
		add_fact(p, "Total size", sz);
		g_free(sz);
		g_free(n);
	} else {
		/* Nothing selected: the place itself. */
		char *name = p->loc ? ex_location_display_name(p->loc) :
				      g_strdup("");
		GdkPixbuf *pb = ex_icons_named(p->loc ? ex_location_icon_name(p->loc) :
							"folder",
					       96, gtk_widget_get_scale_factor(p->root));

		set_picture(p, pb);
		g_clear_object(&pb);
		gtk_label_set_text(GTK_LABEL(p->name), name);
		gtk_label_set_text(GTK_LABEL(p->kind),
				   preview ? "Select a file to preview." :
					     "Select an item to see its details.");
		g_free(name);
	}
}

static gboolean update_idle(gpointer data)
{
	Pane *p = data;

	p->update_id = 0;
	update(p);
	return G_SOURCE_REMOVE;
}

static void sel_changed(ExSelection *s, Pane *p)
{
	/* A moment's delay: arrowing through a folder should not start a
	 * read for every item passed over. */
	if (p->update_id)
		g_source_remove(p->update_id);
	p->update_id = g_timeout_add(120, update_idle, p);
}

static void pane_free(Pane *p)
{
	if (p->update_id)
		g_source_remove(p->update_id);
	if (p->cancel) {
		g_cancellable_cancel(p->cancel);
		g_object_unref(p->cancel);
	}
	forget_shown(p);
	g_signal_handlers_disconnect_by_data(p->sel, p);
	g_object_unref(p->sel);
	if (p->loc)
		ex_location_unref(p->loc);
	g_free(p->mode);
	g_free(p);
}

GtkWidget *ex_preview_pane_new(ExSelection *sel)
{
	Pane *p = g_new0(Pane, 1);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

	p->sel = g_object_ref(sel);
	p->mode = g_strdup("details");
	p->root = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->root),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(p->root, EX_PANE_WIDTH, -1);
	ex_style_add(p->root, EX_CLASS_PANE);
	gtk_container_set_border_width(GTK_CONTAINER(box), 14);

	p->picture = gtk_image_new();
	gtk_widget_set_size_request(p->picture, -1, 140);
	p->name = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(p->name), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(p->name), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(p->name), TRUE);
	gtk_label_set_xalign(GTK_LABEL(p->name), 0);
	ex_style_add(p->name, "ex-pane-title");
	p->kind = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(p->kind), 0);
	gtk_label_set_line_wrap(GTK_LABEL(p->kind), TRUE);
	ex_style_add(p->kind, EX_CLASS_DIM);
	/* The heading over an item's facts. */
	p->heading = gtk_label_new("Details");
	gtk_label_set_xalign(GTK_LABEL(p->heading), 0);
	ex_style_add(p->heading, "ex-pane-heading");
	p->grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(p->grid), 12);
	gtk_grid_set_row_spacing(GTK_GRID(p->grid), 6);
	p->text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(p->text), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(p->text), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->text), GTK_WRAP_WORD_CHAR);
	p->text_sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_set_size_request(p->text_sw, -1, 320);
	gtk_container_add(GTK_CONTAINER(p->text_sw), p->text);

	gtk_box_pack_start(GTK_BOX(box), p->picture, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), p->name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), p->kind, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), p->heading, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), p->grid, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), p->text_sw, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(p->root), box);
	gtk_widget_show_all(box);
	gtk_widget_hide(p->text_sw);
	gtk_widget_hide(p->heading);

	g_object_set_data_full(G_OBJECT(p->root), "ex-pane", p,
			       (GDestroyNotify)pane_free);
	g_signal_connect(sel, "changed", G_CALLBACK(sel_changed), p);
	update(p);
	return p->root;
}

void ex_preview_pane_set_mode(GtkWidget *pane, const char *mode)
{
	Pane *p = pane_of(pane);

	if (!g_strcmp0(p->mode, mode))
		return;
	g_free(p->mode);
	p->mode = g_strdup(mode);
	update(p);
}

void ex_preview_pane_set_location(GtkWidget *pane, ExLocation *loc)
{
	Pane *p = pane_of(pane);

	if (p->loc)
		ex_location_unref(p->loc);
	p->loc = ex_location_ref(loc);
	update(p);
}
