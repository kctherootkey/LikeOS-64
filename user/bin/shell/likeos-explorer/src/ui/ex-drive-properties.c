// LikeOS Explorer: a drive's properties.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-drive-properties.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-devd.h"
#include "backend/ex-util.h"

#include <math.h>
#include <string.h>

/* The ring chart, in pixels: its size and the width of the ring. */
#define EX_RING_SIZE 132
#define EX_RING_WIDTH 22
/* The colour marks beside Used and Free. */
#define EX_SWATCH 12

typedef struct {
	GtkWidget *dialog;
	GCancellable *cancel;
	gboolean known; /* used and free are known */
	guint64 used, free, capacity;
	GtkWidget *used_bytes, *used_short, *free_bytes, *free_short;
	GtkWidget *cap_bytes, *cap_short, *fs_label, *ring;
	gboolean fs_known;
} Drive;

/* Used and free as the file system reports them.  Used is its own figure
 * where it gives one (the file system's own structures and the space kept
 * back for the system count as neither), else what free leaves of the
 * capacity. */
static void space_split(Drive *d, guint64 size, guint64 fr, guint64 us)
{
	d->free = MIN(fr, size);
	if (us != G_MAXUINT64)
		d->used = MIN(us, size - d->free);
	else
		d->used = size - d->free;
}

static void drive_free(Drive *d)
{
	g_cancellable_cancel(d->cancel);
	g_object_unref(d->cancel);
	g_free(d);
}

/* ---- colours ---------------------------------------------------------------- */

/* The theme's named colour, or `fallback' under a theme without it. */
static void color(GtkWidget *w, const char *name, const char *fallback,
		  GdkRGBA *out)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);

	if (!gtk_style_context_lookup_color(sc, name, out))
		gdk_rgba_parse(out, fallback);
}

#define USED_COLOR "ex_drive_used", "#3b74f0"
#define FREE_COLOR "ex_drive_free", "#e1e6ef"

static gboolean swatch_draw(GtkWidget *w, cairo_t *cr, gpointer which)
{
	GdkRGBA c;
	double r = 3, s = EX_SWATCH;
	int h = gtk_widget_get_allocated_height(w);
	double y = (h - s) / 2.0;

	if (GPOINTER_TO_INT(which))
		color(w, USED_COLOR, &c);
	else
		color(w, FREE_COLOR, &c);
	cairo_new_sub_path(cr);
	cairo_arc(cr, s - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, s - r, y + s - r, r, 0, G_PI / 2);
	cairo_arc(cr, r, y + s - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, r, y + r, r, G_PI, 3 * G_PI / 2);
	cairo_close_path(cr);
	gdk_cairo_set_source_rgba(cr, &c);
	cairo_fill(cr);
	return TRUE;
}

static GtkWidget *swatch(gboolean used)
{
	GtkWidget *a = gtk_drawing_area_new();

	gtk_widget_set_size_request(a, EX_SWATCH, EX_SWATCH);
	gtk_widget_set_valign(a, GTK_ALIGN_FILL);
	g_signal_connect(a, "draw", G_CALLBACK(swatch_draw),
			 GINT_TO_POINTER(used));
	return a;
}

/* The ring: the free part in the track colour all the way round, the used
 * part over it from twelve o'clock clockwise, round ends where it is
 * neither empty nor full. */
static gboolean ring_draw(GtkWidget *w, cairo_t *cr, Drive *d)
{
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	double cx = aw / 2.0, cy = ah / 2.0;
	double rad = (MIN(aw, ah) - EX_RING_WIDTH) / 2.0 - 1;
	double total = (double)(d->used + d->free);
	double frac = d->known && total > 0 ? (double)d->used / total : 0;
	GdkRGBA used, free;

	color(w, USED_COLOR, &used);
	color(w, FREE_COLOR, &free);
	cairo_set_line_width(cr, EX_RING_WIDTH);
	cairo_arc(cr, cx, cy, rad, 0, 2 * G_PI);
	gdk_cairo_set_source_rgba(cr, &free);
	cairo_stroke(cr);
	if (frac <= 0)
		return TRUE;
	/* A sliver still shows. */
	frac = CLAMP(frac, 0.01, 1.0);
	cairo_arc(cr, cx, cy, rad, -G_PI / 2, -G_PI / 2 + frac * 2 * G_PI);
	gdk_cairo_set_source_rgba(cr, &used);
	cairo_stroke(cr);
	return TRUE;
}

/* ---- the figures --------------------------------------------------------------- */

/* "15,931,539,456 bytes". */
static char *exact_bytes(guint64 n)
{
	char digits[32];
	GString *s = g_string_new(NULL);
	int len = g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, n);

	for (int i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0)
			g_string_append_c(s, ',');
		g_string_append_c(s, digits[i]);
	}
	g_string_append(s, n == 1 ? " byte" : " bytes");
	return g_string_free(s, FALSE);
}

static void set_pair(GtkWidget *exact, GtkWidget *shrt, gboolean known,
		     guint64 n)
{
	char *a = known ? exact_bytes(n) : g_strdup("Unknown");
	char *b = known ? ex_format_size(n) : g_strdup("");

	gtk_label_set_text(GTK_LABEL(exact), a);
	gtk_label_set_text(GTK_LABEL(shrt), b);
	g_free(a);
	g_free(b);
}

static void show_figures(Drive *d)
{
	set_pair(d->used_bytes, d->used_short, d->known, d->used);
	set_pair(d->free_bytes, d->free_short, d->known, d->free);
	set_pair(d->cap_bytes, d->cap_short, d->capacity > 0, d->capacity);
	gtk_widget_queue_draw(d->ring);
}

typedef struct {
	Drive *d;
	GCancellable *cancel; /* cancelled when the window closes */
} Ask;

static void got_space(GObject *src, GAsyncResult *res, gpointer data)
{
	Ask *a = data;
	Drive *d = a->d;
	GFileInfo *i = g_file_query_filesystem_info_finish(G_FILE(src), res,
							   NULL);

	if (g_cancellable_is_cancelled(a->cancel)) {
		/* The window closed: `d' is gone. */
		g_clear_object(&i);
		g_object_unref(a->cancel);
		g_free(a);
		return;
	}
	g_object_unref(a->cancel);
	g_free(a);
	if (i && g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
		guint64 size = g_file_info_get_attribute_uint64(
			i, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
		guint64 fr = g_file_info_get_attribute_uint64(
			i, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
		guint64 us = g_file_info_has_attribute(
				     i, G_FILE_ATTRIBUTE_FILESYSTEM_USED) ?
				     g_file_info_get_attribute_uint64(
					     i, G_FILE_ATTRIBUTE_FILESYSTEM_USED) :
				     G_MAXUINT64;

		d->capacity = size;
		space_split(d, size, fr, us);
		d->known = TRUE;
		if (!d->fs_known) {
			const char *t = g_file_info_get_attribute_string(
				i, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);

			if (t && *t)
				gtk_label_set_text(GTK_LABEL(d->fs_label), t);
		}
	}
	g_clear_object(&i);
	show_figures(d);
}

/* ---- the window ------------------------------------------------------------------- */

static GtkWidget *text_label(const char *text, gboolean right)
{
	GtkWidget *l = gtk_label_new(text ? text : "");

	gtk_label_set_xalign(GTK_LABEL(l), right ? 1 : 0);
	gtk_label_set_selectable(GTK_LABEL(l), TRUE);
	gtk_widget_set_can_focus(l, FALSE);
	return l;
}

static GtkWidget *key_label(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	ex_style_add(l, EX_CLASS_DIM);
	ex_style_add(l, "ex-props-key");
	return l;
}

static GtkWidget *card(GtkWidget *box)
{
	GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *grid = gtk_grid_new();

	gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
	ex_style_add(c, "ex-props-card");
	gtk_box_pack_start(GTK_BOX(c), grid, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
	return grid;
}

/* A space row: the colour mark (or nothing), the name, the exact bytes
 * and the short figure. */
static void space_row(GtkWidget *grid, int r, GtkWidget *mark, const char *key,
		      GtkWidget **exact, GtkWidget **shrt)
{
	GtkWidget *k = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	if (mark)
		gtk_box_pack_start(GTK_BOX(k), mark, FALSE, FALSE, 0);
	else
		gtk_widget_set_margin_start(k, EX_SWATCH + 8);
	gtk_box_pack_start(GTK_BOX(k), key_label(key), FALSE, FALSE, 0);
	*exact = text_label("…", TRUE);
	*shrt = text_label("", TRUE);
	gtk_widget_set_hexpand(*exact, TRUE);
	ex_style_add(*shrt, "ex-drive-short");
	gtk_grid_attach(GTK_GRID(grid), k, 0, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), *exact, 1, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), *shrt, 2, r, 1, 1);
}

static GtkWidget *header(ExWindow *w, const ExVolume *v)
{
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(w));
	GdkPixbuf *pb = ex_icons_named(v->icon_name, 48, scale);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);
	GtkWidget *name = gtk_label_new(v->name);
	char *where = v->root ? g_file_get_parse_name(v->root) :
			  g_strdup_printf("%s, not opened", v->device);
	GtkWidget *sub = gtk_label_new(where);

	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	ex_style_add(h, "ex-props-header");
	gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(name), 32);
	gtk_label_set_selectable(GTK_LABEL(name), TRUE);
	gtk_widget_set_can_focus(name, FALSE);
	ex_style_add(name, "ex-props-title");
	gtk_label_set_xalign(GTK_LABEL(sub), 0);
	gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(sub), 40);
	ex_style_add(sub, EX_CLASS_DIM);
	gtk_box_pack_start(GTK_BOX(col), name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), sub, FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(h), col, TRUE, TRUE, 0);
	g_free(where);
	return h;
}

static void response(GtkDialog *dlg, int r, gpointer data)
{
	gtk_widget_destroy(GTK_WIDGET(dlg));
}

void ex_drive_properties_show(ExWindow *w, const ExVolume *v)
{
	Drive *d = g_new0(Drive, 1);
	char *title = g_strdup_printf("Properties of %s", v->name);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *grid, *chart, *under, *sep;
	char *fs;
	int r = 0;

	d->cancel = g_cancellable_new();
	d->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w),
						GTK_DIALOG_DESTROY_WITH_PARENT,
						"_OK", GTK_RESPONSE_OK, NULL);
	g_free(title);
	gtk_dialog_set_default_response(GTK_DIALOG(d->dialog), GTK_RESPONSE_OK);
	gtk_box_set_spacing(GTK_BOX(gtk_dialog_get_content_area(
				    GTK_DIALOG(d->dialog))),
			    0);
	gtk_box_pack_start(GTK_BOX(box), header(w, v), FALSE, FALSE, 0);
	ex_style_add(page, "ex-props-page");
	gtk_box_pack_start(GTK_BOX(box), page, TRUE, TRUE, 0);

	/* What it is. */
	grid = card(page);
	gtk_grid_attach(GTK_GRID(grid), key_label("Type"), 0, r, 1, 1);
	{
		/* "CD/DVD drive", "Local disk", "USB drive" -- and a note
		 * when nothing can be written to it */
		char *type = v->kind == EX_VOLUME_DRIVE && v->readonly ?
				     g_strdup_printf("%s (read-only)",
						     ex_volume_type_text(v)) :
				     g_strdup(ex_volume_type_text(v));

		gtk_grid_attach(GTK_GRID(grid), text_label(type, FALSE), 1,
				r++, 1, 1);
		g_free(type);
	}
	if (v->kind == EX_VOLUME_DRIVE) {
		fs = ex_devd_volume_fs_name(ex_devd_get_default(), v->fs_type,
					    v->supported);
		d->fs_known = TRUE;
	} else {
		fs = g_strdup(v->fs_type && *v->fs_type ? v->fs_type : "…");
		d->fs_known = v->fs_type && *v->fs_type;
	}
	d->fs_label = text_label(fs, FALSE);
	gtk_widget_set_hexpand(d->fs_label, TRUE);
	gtk_grid_attach(GTK_GRID(grid), key_label("File system"), 0, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), d->fs_label, 1, r++, 1, 1);
	g_free(fs);

	/* How full: the figures, the ring under them with the name. */
	grid = card(page);
	r = 0;
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	space_row(grid, r++, swatch(TRUE), "Used space", &d->used_bytes,
		  &d->used_short);
	space_row(grid, r++, swatch(FALSE), "Free space", &d->free_bytes,
		  &d->free_short);
	sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	ex_style_add(sep, "ex-settings-separator");
	gtk_grid_attach(GTK_GRID(grid), sep, 0, r++, 3, 1);
	space_row(grid, r++, NULL, "Capacity", &d->cap_bytes, &d->cap_short);

	chart = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	ex_style_add(chart, "ex-drive-chart");
	d->ring = gtk_drawing_area_new();
	gtk_widget_set_size_request(d->ring, EX_RING_SIZE, EX_RING_SIZE);
	gtk_widget_set_halign(d->ring, GTK_ALIGN_CENTER);
	g_signal_connect(d->ring, "draw", G_CALLBACK(ring_draw), d);
	gtk_box_pack_start(GTK_BOX(chart), d->ring, FALSE, FALSE, 0);
	under = gtk_label_new(v->name);
	gtk_label_set_ellipsize(GTK_LABEL(under), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(under), 32);
	ex_style_add(under, "ex-drive-name");
	gtk_box_pack_start(GTK_BOX(chart), under, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), chart, 0, r++, 3, 1);
	atk_object_set_name(gtk_widget_get_accessible(d->ring),
			    "Used and free space");

	/* The figures known now; those of a mounted drive asked again. */
	if (v->root) {
		Ask *a = g_new0(Ask, 1);

		a->d = d;
		a->cancel = g_object_ref(d->cancel);
		if (v->size) {
			d->capacity = v->size;
			space_split(d, v->size, v->free, v->used);
			d->known = TRUE;
		}
		g_file_query_filesystem_info_async(
			v->root,
			G_FILE_ATTRIBUTE_FILESYSTEM_SIZE
			"," G_FILE_ATTRIBUTE_FILESYSTEM_FREE
			"," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
			G_PRIORITY_DEFAULT, d->cancel, got_space, a);
	} else {
		d->capacity = v->capacity;
	}
	show_figures(d);
	if (v->root && !d->known) {
		/* Asked, not answered yet. */
		gtk_label_set_text(GTK_LABEL(d->used_bytes), "…");
		gtk_label_set_text(GTK_LABEL(d->free_bytes), "…");
		gtk_label_set_text(GTK_LABEL(d->cap_bytes), "…");
	}

	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(
				  GTK_DIALOG(d->dialog))),
			  box);
	g_signal_connect(d->dialog, "response", G_CALLBACK(response), NULL);
	g_object_set_data_full(G_OBJECT(d->dialog), "ex-drive", d,
			       (GDestroyNotify)drive_free);
	ex_style_add(d->dialog, "ex-properties");
	ex_style_add(d->dialog, "ex-drive-properties");
	ex_style_fixed_dialog(d->dialog);
	gtk_widget_show_all(d->dialog);
}
