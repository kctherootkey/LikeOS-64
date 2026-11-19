// LikeOS Explorer: Gallery.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-gallery-page.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-gallery.h"
#include "backend/ex-thumbs.h"
#include "backend/ex-util.h"
#include "services/ex-settings.h"

#include <math.h>
#include <string.h>

/* The proportions: rows about 160 pixels tall, six pixels between
 * pictures, a heading of 44 pixels over each month. */
#define ROW_HEIGHT 160
#define GAP 6
#define SIDE 20
#define TOP 8
#define HEADING 44

typedef struct {
	ExFile *f;
	GdkRectangle r;
} Cell;

typedef struct {
	int y;
	int year;
	char *text;
} Heading;

struct _ExGalleryPage {
	GtkBox parent;
	ExSelection *sel;
	GtkWidget *sw, *canvas, *years, *empty;
	GCancellable *cancel;
	GPtrArray *items; /* ExFile, newest first */
	GArray *cells; /* Cell, one per item, laid out for laid_width */
	GPtrArray *headings; /* Heading */
	GHashTable *selected; /* ExFile set */
	int laid_width, height;
	int focus, anchor, hover;
	guint relayout_id;
	gboolean loading, updating;
};

G_DEFINE_TYPE(ExGalleryPage, ex_gallery_page, GTK_TYPE_BOX)

enum { SIG_ACTIVATE, SIG_POPUP, SIG_LOADED, N_SIGS };
static guint sigs[N_SIGS];

static void heading_free(gpointer data)
{
	Heading *h = data;

	g_free(h->text);
	g_free(h);
}

static Cell *cell(ExGalleryPage *p, int i)
{
	return &g_array_index(p->cells, Cell, i);
}

static int n_cells(ExGalleryPage *p)
{
	return p->cells ? (int)p->cells->len : 0;
}

/* ---- layout ------------------------------------------------------------------- */

/* The picture's proportions: its thumbnail's once there is one (it is
 * turned as the picture is shown), else what the collection read. */
static double aspect(ExFile *f)
{
	GdkPixbuf *th = ex_file_get_thumbnail(f);

	if (th && gdk_pixbuf_get_height(th) > 0)
		return CLAMP((double)gdk_pixbuf_get_width(th) /
				     gdk_pixbuf_get_height(th),
			     0.2, 5.0);
	return CLAMP(ex_gallery_aspect(f), 0.2, 5.0);
}

/* Lay `n' pictures from `first' out as one row at `y', `h' tall, the last
 * one taking what rounding left over when `fill'. */
static void place_row(ExGalleryPage *p, int first, int n, int y, double h,
		      int width, gboolean fill)
{
	int x = SIDE;

	for (int i = 0; i < n; i++) {
		Cell *c = cell(p, first + i);
		int w = (int)(aspect(c->f) * h + 0.5);

		if (fill && i == n - 1)
			w = SIDE + width - x;
		c->r.x = x;
		c->r.y = y;
		c->r.width = MAX(w, 8);
		c->r.height = (int)(h + 0.5);
		x += c->r.width + GAP;
	}
}

static void month_of(ExFile *f, int *year, int *month)
{
	GDateTime *d = g_date_time_new_from_unix_local(ex_file_get_mtime(f));

	*year = d ? g_date_time_get_year(d) : 1970;
	*month = d ? g_date_time_get_month(d) : 1;
	if (d)
		g_date_time_unref(d);
}

/* The rows of each month fill the width exactly: pictures are added to a
 * row at the standard height until it is full, then the row is scaled to
 * fit.  A month's last row keeps the standard height. */
static void layout(ExGalleryPage *p, int alloc_width)
{
	static const char *const months[] = {
		"January", "February", "March",	    "April",   "May",	   "June",
		"July",	   "August",   "September", "October", "November", "December",
	};
	int width = MAX(alloc_width - 2 * SIDE, 100);
	int y = TOP, first = 0, n = 0, cur = -1;
	double sum = 0;

	p->laid_width = alloc_width;
	g_ptr_array_set_size(p->headings, 0);
	g_array_set_size(p->cells, p->items ? p->items->len : 0);
	for (guint i = 0; p->items && i < p->items->len; i++) {
		ExFile *f = g_ptr_array_index(p->items, i);
		int year, month;

		cell(p, (int)i)->f = f;
		month_of(f, &year, &month);
		if (year * 12 + month != cur) {
			Heading *h = g_new0(Heading, 1);

			if (n) {
				place_row(p, first, n, y, ROW_HEIGHT, width, FALSE);
				y += ROW_HEIGHT + GAP;
			}
			cur = year * 12 + month;
			h->y = y;
			h->year = year;
			h->text = g_strdup_printf("%s %d", months[month - 1], year);
			g_ptr_array_add(p->headings, h);
			y += HEADING;
			first = (int)i;
			n = 0;
			sum = 0;
		}
		sum += aspect(f);
		n++;
		if (sum * ROW_HEIGHT + GAP * (n - 1) >= width) {
			double h = (width - GAP * (n - 1)) / sum;

			place_row(p, first, n, y, h, width, TRUE);
			y += (int)(h + 0.5) + GAP;
			first = (int)i + 1;
			n = 0;
			sum = 0;
		}
	}
	if (n) {
		place_row(p, first, n, y, ROW_HEIGHT, width, FALSE);
		y += ROW_HEIGHT + GAP;
	}
	p->height = y + TOP;
	gtk_widget_set_size_request(p->canvas, -1, p->height);
	gtk_widget_queue_draw(p->canvas);
}

static gboolean relayout_now(gpointer data)
{
	ExGalleryPage *p = data;

	p->relayout_id = 0;
	layout(p, gtk_widget_get_allocated_width(p->canvas));
	return G_SOURCE_REMOVE;
}

static void relayout_later(ExGalleryPage *p)
{
	if (!p->relayout_id)
		p->relayout_id = g_idle_add(relayout_now, p);
}

static void canvas_allocated(GtkWidget *w, GdkRectangle *a, ExGalleryPage *p)
{
	if (a->width != p->laid_width)
		relayout_later(p);
}

/* ---- the years at the side ------------------------------------------------------ */

static void scroll_to_y(ExGalleryPage *p, int y)
{
	GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(p->sw));

	gtk_adjustment_set_value(a, y);
}

static void year_clicked(GtkButton *b, ExGalleryPage *p)
{
	int year = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "ex-year"));

	for (guint i = 0; i < p->headings->len; i++) {
		Heading *h = g_ptr_array_index(p->headings, i);

		if (h->year == year) {
			scroll_to_y(p, h->y - TOP);
			return;
		}
	}
}

static void destroy_child(GtkWidget *w, gpointer data)
{
	gtk_widget_destroy(w);
}

static void fill_years(ExGalleryPage *p)
{
	int last = 0;

	gtk_container_foreach(GTK_CONTAINER(p->years), destroy_child, NULL);
	for (guint i = 0; p->items && i < p->items->len; i++) {
		int year, month;

		month_of(g_ptr_array_index(p->items, i), &year, &month);
		if (year != last) {
			char *t = g_strdup_printf("%d", year);
			GtkWidget *b = gtk_button_new_with_label(t);

			gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
			gtk_widget_set_focus_on_click(b, FALSE);
			ex_style_add(b, "ex-gallery-year");
			gtk_widget_set_tooltip_text(b, t);
			g_object_set_data(G_OBJECT(b), "ex-year", GINT_TO_POINTER(year));
			g_signal_connect(b, "clicked", G_CALLBACK(year_clicked), p);
			gtk_box_pack_start(GTK_BOX(p->years), b, FALSE, FALSE, 0);
			g_free(t);
			last = year;
		}
	}
	gtk_widget_show_all(p->years);
}

/* ---- drawing ------------------------------------------------------------------- */

static void rounded(cairo_t *cr, double x, double y, double w, double h,
		    double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
	cairo_arc(cr, x + w - r, y + r, r, 1.5 * G_PI, 2 * G_PI);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 0.5 * G_PI);
	cairo_arc(cr, x + r, y + h - r, r, 0.5 * G_PI, G_PI);
	cairo_close_path(cr);
}

/* A picture fills its cell, cut at the sides or the top and bottom as the
 * proportions need. */
static void draw_thumb(cairo_t *cr, GdkPixbuf *th, const GdkRectangle *r)
{
	double w = gdk_pixbuf_get_width(th), h = gdk_pixbuf_get_height(th);
	double s = MAX(r->width / w, r->height / h);

	cairo_save(cr);
	cairo_rectangle(cr, r->x, r->y, r->width, r->height);
	cairo_clip(cr);
	cairo_translate(cr, r->x + (r->width - w * s) / 2,
			r->y + (r->height - h * s) / 2);
	cairo_scale(cr, s, s);
	gdk_cairo_set_source_pixbuf(cr, th, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void draw_play(cairo_t *cr, const GdkRectangle *r)
{
	double cx = r->x + 22, cy = r->y + r->height - 22;

	cairo_arc(cr, cx, cy, 13, 0, 2 * G_PI);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
	cairo_fill(cr);
	cairo_move_to(cr, cx - 4, cy - 6.5);
	cairo_line_to(cr, cx + 7, cy);
	cairo_line_to(cr, cx - 4, cy + 6.5);
	cairo_close_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);
}

/* Selected: the accent frame and the ticked box in the corner. */
static void draw_selected(cairo_t *cr, const GdkRectangle *r)
{
	double bx = r->x + r->width - 26, by = r->y + 6;

	cairo_set_source_rgba(cr, 0, 0.47, 0.83, 0.12);
	cairo_rectangle(cr, r->x, r->y, r->width, r->height);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0.47, 0.83);
	cairo_set_line_width(cr, 3);
	cairo_rectangle(cr, r->x + 1.5, r->y + 1.5, r->width - 3, r->height - 3);
	cairo_stroke(cr);
	rounded(cr, bx, by, 20, 20, 4);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 1.6);
	cairo_move_to(cr, bx + 5, by + 10.5);
	cairo_line_to(cr, bx + 8.5, by + 14);
	cairo_line_to(cr, bx + 15, by + 6.5);
	cairo_stroke(cr);
}

static gboolean canvas_draw(GtkWidget *w, cairo_t *cr, ExGalleryPage *p)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(w);
	int scale = gtk_widget_get_scale_factor(w);
	double x1, y1, x2, y2;
	GdkRGBA fg;

	gtk_render_background(sc, cr, 0, 0, gtk_widget_get_allocated_width(w),
			      gtk_widget_get_allocated_height(w));
	gtk_style_context_get_color(sc, gtk_widget_get_state_flags(w), &fg);
	cairo_clip_extents(cr, &x1, &y1, &x2, &y2);

	for (guint i = 0; i < p->headings->len; i++) {
		Heading *h = g_ptr_array_index(p->headings, i);
		PangoLayout *pl;
		PangoFontDescription *fd;

		if (h->y + HEADING < y1 || h->y > y2)
			continue;
		pl = gtk_widget_create_pango_layout(w, h->text);
		fd = pango_font_description_copy(
			pango_context_get_font_description(
				pango_layout_get_context(pl)));
		pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
		pango_font_description_set_size(fd, 11 * PANGO_SCALE);
		pango_layout_set_font_description(pl, fd);
		gdk_cairo_set_source_rgba(cr, &fg);
		cairo_move_to(cr, SIDE, h->y + 14);
		pango_cairo_show_layout(cr, pl);
		pango_font_description_free(fd);
		g_object_unref(pl);
	}
	for (int i = 0; i < n_cells(p); i++) {
		Cell *c = cell(p, i);
		GdkPixbuf *th;

		if (c->r.y + c->r.height < y1 || c->r.y > y2)
			continue;
		th = ex_file_get_thumbnail(c->f);
		if (th) {
			draw_thumb(cr, th, &c->r);
			/* A thumbnail much smaller than its cell: the larger
			 * one is asked for, once. */
			if (MAX(gdk_pixbuf_get_width(th), gdk_pixbuf_get_height(th)) <
				    MAX(c->r.width, c->r.height) * scale * 3 / 4 &&
			    !g_object_get_data(G_OBJECT(c->f), "ex-gallery-big")) {
				int big = MAX(c->r.width, c->r.height) * scale;

				g_object_set_data(G_OBJECT(c->f), "ex-gallery-big",
						  GINT_TO_POINTER(1));
				ex_thumbs_request(c->f, big > 256 ? EX_THUMB_XLARGE :
								    EX_THUMB_LARGE);
			}
		} else {
			GdkPixbuf *icon = ex_icons_for_file(c->f, 48, scale, FALSE,
							    FALSE);

			cairo_set_source_rgb(cr, 0.953, 0.953, 0.953);
			cairo_rectangle(cr, c->r.x, c->r.y, c->r.width, c->r.height);
			cairo_fill(cr);
			if (icon) {
				cairo_save(cr);
				cairo_translate(cr, c->r.x + (c->r.width - 48) / 2.0,
						c->r.y + (c->r.height - 48) / 2.0);
				cairo_scale(cr, 1.0 / scale, 1.0 / scale);
				gdk_cairo_set_source_pixbuf(cr, icon, 0, 0);
				cairo_paint(cr);
				cairo_restore(cr);
				g_object_unref(icon);
			}
			/* Asked for once; it arrives through "changed". */
			if (!ex_file_thumbnail_failed(c->f) &&
			    !g_object_get_data(G_OBJECT(c->f), "ex-gallery-asked")) {
				g_object_set_data(G_OBJECT(c->f), "ex-gallery-asked",
						  GINT_TO_POINTER(1));
				int big = MAX(c->r.width, c->r.height) * scale;

				ex_thumbs_request(c->f, big > 256 ? EX_THUMB_XLARGE :
							big > 128 ? EX_THUMB_LARGE :
								    EX_THUMB_NORMAL);
			}
		}
		if (!strcmp(ex_file_get_kind(c->f), "video"))
			draw_play(cr, &c->r);
		if (g_hash_table_contains(p->selected, c->f)) {
			draw_selected(cr, &c->r);
		} else if (i == p->hover) {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
			cairo_rectangle(cr, c->r.x, c->r.y, c->r.width, c->r.height);
			cairo_fill(cr);
		}
		if (i == p->focus && gtk_widget_has_visible_focus(w)) {
			gtk_render_focus(sc, cr, c->r.x - 2, c->r.y - 2,
					 c->r.width + 4, c->r.height + 4);
		}
	}
	return TRUE;
}

static void thumb_changed(ExFile *f, ExGalleryPage *p)
{
	/* A thumbnail of other proportions than read moves the rows. */
	relayout_later(p);
}

/* ---- selection ------------------------------------------------------------------- */

static void publish(ExGalleryPage *p)
{
	GPtrArray *a = g_ptr_array_new();

	for (guint i = 0; p->items && i < p->items->len; i++) {
		ExFile *f = g_ptr_array_index(p->items, i);

		if (g_hash_table_contains(p->selected, f))
			g_ptr_array_add(a, f);
	}
	p->updating = TRUE;
	ex_selection_set(p->sel, a);
	p->updating = FALSE;
	g_ptr_array_unref(a);
	gtk_widget_queue_draw(p->canvas);
}

/* The tab changed the selection (cleared it on leaving, say). */
static void selection_changed(ExSelection *s, ExGalleryPage *p)
{
	GPtrArray *a;

	if (p->updating)
		return;
	g_hash_table_remove_all(p->selected);
	a = ex_selection_get(s);
	for (guint i = 0; a && i < a->len; i++)
		g_hash_table_add(p->selected, g_ptr_array_index(a, i));
	gtk_widget_queue_draw(p->canvas);
}

static void select_only(ExGalleryPage *p, int i)
{
	g_hash_table_remove_all(p->selected);
	if (i >= 0 && i < n_cells(p))
		g_hash_table_add(p->selected, cell(p, i)->f);
}

static void select_range(ExGalleryPage *p, int a, int b, gboolean add)
{
	if (!add)
		g_hash_table_remove_all(p->selected);
	for (int i = MIN(a, b); i <= MAX(a, b); i++)
		if (i >= 0 && i < n_cells(p))
			g_hash_table_add(p->selected, cell(p, i)->f);
}

void ex_gallery_page_select_all(ExGalleryPage *p)
{
	select_range(p, 0, n_cells(p) - 1, FALSE);
	publish(p);
}

void ex_gallery_page_select_none(ExGalleryPage *p)
{
	g_hash_table_remove_all(p->selected);
	publish(p);
}

void ex_gallery_page_invert_selection(ExGalleryPage *p)
{
	for (int i = 0; i < n_cells(p); i++) {
		ExFile *f = cell(p, i)->f;

		if (!g_hash_table_remove(p->selected, f))
			g_hash_table_add(p->selected, f);
	}
	publish(p);
}

static void activate_selection(ExGalleryPage *p)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);

	for (guint i = 0; p->items && i < p->items->len; i++) {
		ExFile *f = g_ptr_array_index(p->items, i);

		if (g_hash_table_contains(p->selected, f))
			g_ptr_array_add(a, g_object_ref(f));
	}
	if (a->len)
		g_signal_emit(p, sigs[SIG_ACTIVATE], 0, a);
	g_ptr_array_unref(a);
}

/* ---- the pointer and the keys ------------------------------------------------------ */

static int hit(ExGalleryPage *p, double x, double y)
{
	for (int i = 0; i < n_cells(p); i++) {
		GdkRectangle *r = &cell(p, i)->r;

		if (x >= r->x && x < r->x + r->width && y >= r->y &&
		    y < r->y + r->height)
			return i;
	}
	return -1;
}

static void show_cell(ExGalleryPage *p, int i)
{
	GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(p->sw));
	double top = gtk_adjustment_get_value(a);
	double page = gtk_adjustment_get_page_size(a);
	GdkRectangle *r;

	if (i < 0 || i >= n_cells(p))
		return;
	r = &cell(p, i)->r;
	if (r->y < top)
		gtk_adjustment_set_value(a, r->y - GAP);
	else if (r->y + r->height > top + page)
		gtk_adjustment_set_value(a, r->y + r->height + GAP - page);
}

static gboolean canvas_press(GtkWidget *w, GdkEventButton *ev, ExGalleryPage *p)
{
	int i = hit(p, ev->x, ev->y);
	gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
	gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;

	gtk_widget_grab_focus(w);
	if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
		if (i >= 0)
			activate_selection(p);
		return TRUE;
	}
	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (ev->button == 1) {
		if (i < 0) {
			if (!ctrl)
				g_hash_table_remove_all(p->selected);
		} else if (ctrl) {
			if (!g_hash_table_remove(p->selected, cell(p, i)->f))
				g_hash_table_add(p->selected, cell(p, i)->f);
			p->anchor = i;
		} else if (shift && p->anchor >= 0) {
			select_range(p, p->anchor, i, FALSE);
		} else {
			select_only(p, i);
			p->anchor = i;
		}
		if (i >= 0)
			p->focus = i;
		publish(p);
		return TRUE;
	}
	if (ev->button == 3) {
		if (i >= 0 && !g_hash_table_contains(p->selected, cell(p, i)->f)) {
			select_only(p, i);
			p->anchor = p->focus = i;
		} else if (i < 0) {
			g_hash_table_remove_all(p->selected);
		}
		publish(p);
		g_signal_emit(p, sigs[SIG_POPUP], 0, ev, i >= 0);
		return TRUE;
	}
	return FALSE;
}

static gboolean canvas_motion(GtkWidget *w, GdkEventMotion *ev, ExGalleryPage *p)
{
	int i = hit(p, ev->x, ev->y);

	if (i != p->hover) {
		p->hover = i;
		gtk_widget_set_tooltip_text(
			w, i >= 0 ? ex_file_get_name(cell(p, i)->f) : NULL);
		gtk_widget_queue_draw(w);
	}
	return FALSE;
}

static gboolean canvas_leave(GtkWidget *w, GdkEventCrossing *ev,
			     ExGalleryPage *p)
{
	if (p->hover >= 0) {
		p->hover = -1;
		gtk_widget_queue_draw(w);
	}
	return FALSE;
}

/* The picture above or below `from': in the nearest row that way, the one
 * whose middle is closest. */
static int vertical(ExGalleryPage *p, int from, gboolean down)
{
	GdkRectangle *f = &cell(p, from)->r;
	int row_y = -1, best = -1;
	double fx = f->x + f->width / 2.0, bd = G_MAXDOUBLE;

	for (int i = 0; i < n_cells(p); i++) {
		GdkRectangle *r = &cell(p, i)->r;

		if (down ? r->y > f->y && (row_y < 0 || r->y < row_y) :
			   r->y < f->y && (row_y < 0 || r->y > row_y))
			row_y = r->y;
	}
	if (row_y < 0)
		return from;
	for (int i = 0; i < n_cells(p); i++) {
		GdkRectangle *r = &cell(p, i)->r;
		double d = fabs(r->x + r->width / 2.0 - fx);

		if (r->y == row_y && d < bd) {
			bd = d;
			best = i;
		}
	}
	return best >= 0 ? best : from;
}

static gboolean canvas_key(GtkWidget *w, GdkEventKey *ev, ExGalleryPage *p)
{
	int n = n_cells(p), to = p->focus;
	gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;
	gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;

	if (!n)
		return FALSE;
	if (to < 0 || to >= n)
		to = 0;
	switch (ev->keyval) {
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		activate_selection(p);
		return TRUE;
	case GDK_KEY_space:
		if (ctrl) {
			if (!g_hash_table_remove(p->selected, cell(p, to)->f))
				g_hash_table_add(p->selected, cell(p, to)->f);
		} else {
			select_only(p, to);
		}
		publish(p);
		return TRUE;
	case GDK_KEY_Left:
		to = MAX(to - 1, 0);
		break;
	case GDK_KEY_Right:
		to = MIN(to + 1, n - 1);
		break;
	case GDK_KEY_Up:
		to = vertical(p, to, FALSE);
		break;
	case GDK_KEY_Down:
		to = vertical(p, to, TRUE);
		break;
	case GDK_KEY_Home:
		to = 0;
		break;
	case GDK_KEY_End:
		to = n - 1;
		break;
	default:
		return FALSE;
	}
	p->focus = to;
	if (shift && p->anchor >= 0) {
		select_range(p, p->anchor, to, FALSE);
	} else if (!ctrl) {
		select_only(p, to);
		p->anchor = to;
	}
	publish(p);
	show_cell(p, to);
	return TRUE;
}

static gboolean canvas_menu_key(GtkWidget *w, ExGalleryPage *p)
{
	g_signal_emit(p, sigs[SIG_POPUP], 0, NULL,
		      g_hash_table_size(p->selected) > 0);
	return TRUE;
}

/* ---- loading ------------------------------------------------------------------------- */

static void set_items(ExGalleryPage *p, GPtrArray *items)
{
	if (p->items)
		for (guint i = 0; i < p->items->len; i++)
			g_signal_handlers_disconnect_by_data(
				g_ptr_array_index(p->items, i), p);
	g_clear_pointer(&p->items, g_ptr_array_unref);
	g_hash_table_remove_all(p->selected);
	p->items = items;
	p->focus = p->anchor = p->hover = -1;
	for (guint i = 0; items && i < items->len; i++)
		g_signal_connect(g_ptr_array_index(items, i), "changed",
				 G_CALLBACK(thumb_changed), p);
	gtk_widget_set_visible(p->empty, items && !items->len);
	layout(p, gtk_widget_get_allocated_width(p->canvas));
	fill_years(p);
}

static void collected(GObject *src, GAsyncResult *res, gpointer data)
{
	ExGalleryPage *p = data;
	GPtrArray *items = ex_gallery_collect_finish(res, NULL);

	if (items) {
		p->loading = FALSE;
		set_items(p, items);
		scroll_to_y(p, 0);
		g_signal_emit(p, sigs[SIG_LOADED], 0);
	}
	g_object_unref(p);
}

/* The folders of the collection, by their personal-folder names. */
static GPtrArray *collection(void)
{
	static const struct {
		const char *name;
		GUserDirectory dir;
	} known[] = {
		{ "pictures", G_USER_DIRECTORY_PICTURES },
		{ "videos", G_USER_DIRECTORY_VIDEOS },
		{ "desktop", G_USER_DIRECTORY_DESKTOP },
		{ "download", G_USER_DIRECTORY_DOWNLOAD },
		{ "documents", G_USER_DIRECTORY_DOCUMENTS },
		{ "music", G_USER_DIRECTORY_MUSIC },
	};
	char **names = ex_settings_get_strv(ex_settings_get(), "gallery-folders");
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);

	for (gsize i = 0; i < G_N_ELEMENTS(known); i++) {
		const char *path = ex_user_dir(known[i].dir);

		if (path && names &&
		    g_strv_contains((const char *const *)names, known[i].name) &&
		    strcmp(path, g_get_home_dir()) != 0)
			g_ptr_array_add(out, g_file_new_for_path(path));
	}
	g_strfreev(names);
	return out;
}

void ex_gallery_page_reload(ExGalleryPage *p)
{
	GPtrArray *folders = collection();

	if (p->cancel)
		g_cancellable_cancel(p->cancel);
	g_clear_object(&p->cancel);
	p->cancel = g_cancellable_new();
	p->loading = TRUE;
	ex_gallery_collect_async(folders, p->cancel, collected, g_object_ref(p));
	g_ptr_array_unref(folders);
}

void ex_gallery_page_clear(ExGalleryPage *p)
{
	if (p->cancel)
		g_cancellable_cancel(p->cancel);
	g_clear_object(&p->cancel);
	p->loading = FALSE;
	set_items(p, NULL);
}

gboolean ex_gallery_page_loading(ExGalleryPage *p)
{
	return p->loading;
}

guint ex_gallery_page_count(ExGalleryPage *p)
{
	return p->items ? p->items->len : 0;
}

GPtrArray *ex_gallery_page_list(ExGalleryPage *p)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);

	for (guint i = 0; p->items && i < p->items->len; i++)
		g_ptr_array_add(a, g_object_ref(g_ptr_array_index(p->items, i)));
	return a;
}

void ex_gallery_page_focus(ExGalleryPage *p)
{
	gtk_widget_grab_focus(p->canvas);
}

static void setting_changed(ExSettings *s, const char *key, ExGalleryPage *p)
{
	/* The collection changed: read it again, if it is being shown. */
	if (!strcmp(key, "gallery-folders") && (p->items || p->loading))
		ex_gallery_page_reload(p);
}

/* ---- life cycle ------------------------------------------------------------------- */

static void ex_gallery_page_dispose(GObject *o)
{
	ExGalleryPage *p = EX_GALLERY_PAGE(o);

	if (p->relayout_id) {
		g_source_remove(p->relayout_id);
		p->relayout_id = 0;
	}
	if (p->cancel) {
		g_cancellable_cancel(p->cancel);
		g_clear_object(&p->cancel);
	}
	if (p->items) {
		for (guint i = 0; i < p->items->len; i++)
			g_signal_handlers_disconnect_by_data(
				g_ptr_array_index(p->items, i), p);
		g_clear_pointer(&p->items, g_ptr_array_unref);
	}
	g_signal_handlers_disconnect_by_data(ex_settings_get(), p);
	if (p->sel)
		g_signal_handlers_disconnect_by_data(p->sel, p);
	g_clear_object(&p->sel);
	G_OBJECT_CLASS(ex_gallery_page_parent_class)->dispose(o);
}

static void ex_gallery_page_finalize(GObject *o)
{
	ExGalleryPage *p = EX_GALLERY_PAGE(o);

	g_array_unref(p->cells);
	g_ptr_array_unref(p->headings);
	g_hash_table_unref(p->selected);
	G_OBJECT_CLASS(ex_gallery_page_parent_class)->finalize(o);
}

static void ex_gallery_page_class_init(ExGalleryPageClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->dispose = ex_gallery_page_dispose;
	G_OBJECT_CLASS(k)->finalize = ex_gallery_page_finalize;
	sigs[SIG_ACTIVATE] = g_signal_new("activate", t, G_SIGNAL_RUN_LAST, 0,
					  NULL, NULL, NULL, G_TYPE_NONE, 1,
					  G_TYPE_PTR_ARRAY);
	sigs[SIG_POPUP] = g_signal_new("popup", t, G_SIGNAL_RUN_LAST, 0, NULL,
				       NULL, NULL, G_TYPE_NONE, 2, G_TYPE_POINTER,
				       G_TYPE_BOOLEAN);
	sigs[SIG_LOADED] = g_signal_new("loaded", t, G_SIGNAL_RUN_LAST, 0, NULL,
					NULL, NULL, G_TYPE_NONE, 0);
}

static void ex_gallery_page_init(ExGalleryPage *p)
{
	GtkWidget *overlay = gtk_overlay_new();

	p->cells = g_array_new(FALSE, TRUE, sizeof(Cell));
	p->headings = g_ptr_array_new_with_free_func(heading_free);
	p->selected = g_hash_table_new(NULL, NULL);
	p->focus = p->anchor = p->hover = -1;
	gtk_orientable_set_orientation(GTK_ORIENTABLE(p),
				       GTK_ORIENTATION_HORIZONTAL);
	ex_style_add(GTK_WIDGET(p), "ex-gallery");

	p->canvas = gtk_drawing_area_new();
	gtk_widget_set_can_focus(p->canvas, TRUE);
	gtk_widget_add_events(p->canvas, GDK_BUTTON_PRESS_MASK |
						 GDK_POINTER_MOTION_MASK |
						 GDK_LEAVE_NOTIFY_MASK |
						 GDK_KEY_PRESS_MASK);
	ex_style_add(p->canvas, "ex-gallery-canvas");
	atk_object_set_name(gtk_widget_get_accessible(p->canvas), "Gallery");
	g_signal_connect(p->canvas, "draw", G_CALLBACK(canvas_draw), p);
	g_signal_connect(p->canvas, "size-allocate", G_CALLBACK(canvas_allocated),
			 p);
	g_signal_connect(p->canvas, "button-press-event", G_CALLBACK(canvas_press),
			 p);
	g_signal_connect(p->canvas, "motion-notify-event",
			 G_CALLBACK(canvas_motion), p);
	g_signal_connect(p->canvas, "leave-notify-event", G_CALLBACK(canvas_leave),
			 p);
	g_signal_connect(p->canvas, "key-press-event", G_CALLBACK(canvas_key), p);
	g_signal_connect(p->canvas, "popup-menu", G_CALLBACK(canvas_menu_key), p);

	p->sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(p->sw), p->canvas);
	gtk_container_add(GTK_CONTAINER(overlay), p->sw);

	p->empty = gtk_label_new("The pictures and videos of your collection "
				 "will show here.\nChoose its folders under "
				 "Collection.");
	gtk_label_set_justify(GTK_LABEL(p->empty), GTK_JUSTIFY_CENTER);
	gtk_widget_set_halign(p->empty, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(p->empty, GTK_ALIGN_START);
	gtk_widget_set_margin_top(p->empty, 48);
	gtk_widget_set_no_show_all(p->empty, TRUE);
	ex_style_add(p->empty, EX_CLASS_PLACEHOLDER);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->empty);
	gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), p->empty, TRUE);
	gtk_box_pack_start(GTK_BOX(p), overlay, TRUE, TRUE, 0);

	/* The years, newest at the top, to jump between. */
	p->years = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	ex_style_add(p->years, "ex-gallery-years");
	gtk_box_pack_end(GTK_BOX(p), p->years, FALSE, FALSE, 0);

	g_signal_connect(ex_settings_get(), "changed", G_CALLBACK(setting_changed),
			 p);
	gtk_widget_show_all(GTK_WIDGET(p));
}

GtkWidget *ex_gallery_page_new(ExSelection *sel)
{
	ExGalleryPage *p = g_object_new(EX_TYPE_GALLERY_PAGE, NULL);

	p->sel = g_object_ref(sel);
	g_signal_connect(sel, "changed", G_CALLBACK(selection_changed), p);
	return GTK_WIDGET(p);
}
