// LikeOS -- LikeOS Image Viewer: what the brush tools share.
//
// See tool-brush.h.  Nothing here keeps state of its own except the
// hatching tile, which is made once.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tool-brush.h"

/* ---- where things are on the canvas -------------------------------------- */

/* The matrix through three points: p0 the image of the origin, px that of
 * (ux, 0), py that of (0, uy). */
static void matrix_from_points(cairo_matrix_t *m, double ux, double uy,
			       double p0x, double p0y, double pxx, double pxy,
			       double pyx, double pyy)
{
	cairo_matrix_init(m, (pxx - p0x) / ux, (pxy - p0y) / ux,
			  (pyx - p0x) / uy, (pyy - p0y) / uy, p0x, p0y);
}

gboolean iv_brush_source_matrix(IvEditor *ed, cairo_matrix_t *m)
{
	double ax, ay, bx, by, cx, cy;
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	if (sw <= 0 || sh <= 0) {
		cairo_matrix_init_identity(m);
		return FALSE;
	}
	iv_editor_source_to_display(ed, 0, 0, &ax, &ay);
	iv_editor_display_to_widget(ed, ax, ay, &ax, &ay);
	iv_editor_source_to_display(ed, sw, 0, &bx, &by);
	iv_editor_display_to_widget(ed, bx, by, &bx, &by);
	iv_editor_source_to_display(ed, 0, sh, &cx, &cy);
	iv_editor_display_to_widget(ed, cx, cy, &cx, &cy);
	matrix_from_points(m, sw, sh, ax, ay, bx, by, cx, cy);
	return TRUE;
}

void iv_brush_output_size(IvEditor *ed, int *ow, int *oh)
{
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	if (sw <= 0 || sh <= 0) {
		*ow = *oh = 0;
		return;
	}
	iv_geometry_output_size(&iv_editor_state(ed)->geo, sw, sh, ow, oh);
}

gboolean iv_brush_output_matrix(IvEditor *ed, cairo_matrix_t *m)
{
	double ax, ay, bx, by, cx, cy;
	int ow, oh;

	iv_brush_output_size(ed, &ow, &oh);
	if (ow <= 0 || oh <= 0) {
		cairo_matrix_init_identity(m);
		return FALSE;
	}
	iv_editor_output_to_display(ed, 0, 0, &ax, &ay);
	iv_editor_display_to_widget(ed, ax, ay, &ax, &ay);
	iv_editor_output_to_display(ed, ow, 0, &bx, &by);
	iv_editor_display_to_widget(ed, bx, by, &bx, &by);
	iv_editor_output_to_display(ed, 0, oh, &cx, &cy);
	iv_editor_display_to_widget(ed, cx, cy, &cx, &cy);
	matrix_from_points(m, ow, oh, ax, ay, bx, by, cx, cy);
	return TRUE;
}

double iv_brush_matrix_scale(const cairo_matrix_t *m)
{
	return sqrt(fabs(m->xx * m->yy - m->xy * m->yx));
}

/* ---- masks ---------------------------------------------------------------- */

double iv_brush_mask_scale(IvEditor *ed)
{
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	if (MAX(sw, sh) <= IV_BRUSH_MASK_EDGE)
		return 1.0;
	return (double)IV_BRUSH_MASK_EDGE / MAX(sw, sh);
}

void iv_brush_mask_size(IvEditor *ed, int *mw, int *mh)
{
	int sw, sh;
	double k = iv_brush_mask_scale(ed);

	iv_editor_source_size(ed, &sw, &sh);
	*mw = sw > 0 ? MAX(1, (int)lround(sw * k)) : 0;
	*mh = sh > 0 ? MAX(1, (int)lround(sh * k)) : 0;
}

IvMask *iv_brush_mask_new(IvEditor *ed)
{
	int mw, mh;

	iv_brush_mask_size(ed, &mw, &mh);
	return mw > 0 ? iv_mask_new(mw, mh) : NULL;
}

IvMask *iv_brush_mask_conform(IvEditor *ed, IvMask *m)
{
	int mw, mh;

	if (!m)
		return NULL;
	iv_brush_mask_size(ed, &mw, &mh);
	if (m->w == mw && m->h == mh)
		return iv_mask_ref(m);
	return iv_mask_resized(m, mw, mh);
}

static void dirty_add(GdkRectangle *dirty, double x, double y, double r, int w,
		      int h)
{
	GdkRectangle d;

	if (!dirty)
		return;
	d.x = CLAMP((int)floor(x - r) - 1, 0, w);
	d.y = CLAMP((int)floor(y - r) - 1, 0, h);
	d.width = CLAMP((int)ceil(x + r) + 2, 0, w) - d.x;
	d.height = CLAMP((int)ceil(y + r) + 2, 0, h) - d.y;
	if (d.width <= 0 || d.height <= 0)
		return;
	if (dirty->width <= 0 || dirty->height <= 0)
		*dirty = d;
	else
		gdk_rectangle_union(dirty, &d, dirty);
}

void iv_brush_line(uint8_t *mask, int stride, int w, int h, double x0,
		   double y0, double x1, double y1, double r, int value,
		   double hardness, GdkRectangle *dirty)
{
	double len = hypot(x1 - x0, y1 - y0);
	/* A quarter of the radius apart: closer would only cost time, wider
	 * apart and a soft brush shows the single dabs as beads. */
	double step = MAX(0.75, r * 0.25);
	int n = MAX(1, (int)ceil(len / step));

	if (!mask || w <= 0 || h <= 0 || !(r > 0))
		return;
	/* The first dab of a line is the last one of the line before (a
	 * stroke is drawn as a chain of them), so it is skipped -- except
	 * for a single dab, where both ends are the same point. */
	for (int i = len > 0 ? 1 : 0; i <= n; i++) {
		double t = (double)i / n;
		double x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;

		/* `stride' as the width: a row's padding bytes past the
		 * last real column may be painted too, which is harmless
		 * (nothing reads them), and the dab stays exact otherwise. */
		iv_mask_dab(mask, stride, h, x, y, r, value, hardness);
	}
	dirty_add(dirty, MIN(x0, x1) + fabs(x1 - x0) / 2,
		  MIN(y0, y1) + fabs(y1 - y0) / 2,
		  r + MAX(fabs(x1 - x0), fabs(y1 - y0)) / 2, w, h);
}

/* ---- the mask shown over the picture ---------------------------------------- */

IvMaskView *iv_mask_view_new(int mask_w, int mask_h)
{
	IvMaskView *v;
	double k;

	if (mask_w <= 0 || mask_h <= 0)
		return NULL;
	v = g_new0(IvMaskView, 1);
	v->mw = mask_w;
	v->mh = mask_h;
	k = MIN(1.0, (double)IV_BRUSH_VIEW_EDGE / MAX(mask_w, mask_h));
	v->w = MAX(1, (int)lround(mask_w * k));
	v->h = MAX(1, (int)lround(mask_h * k));
	v->k = (double)v->w / mask_w;
	v->stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, v->w);
	v->data = g_try_malloc0((gsize)v->stride * (gsize)v->h);
	if (!v->data) {
		g_free(v);
		return NULL;
	}
	v->surface = cairo_image_surface_create_for_data(v->data, CAIRO_FORMAT_A8,
							 v->w, v->h, v->stride);
	v->empty = TRUE;
	return v;
}

void iv_mask_view_free(IvMaskView *v)
{
	if (!v)
		return;
	cairo_surface_destroy(v->surface);
	g_free(v->data);
	g_free(v);
}

void iv_mask_view_clear(IvMaskView *v)
{
	if (!v)
		return;
	cairo_surface_flush(v->surface);
	memset(v->data, 0, (gsize)v->stride * (gsize)v->h);
	cairo_surface_mark_dirty(v->surface);
	v->empty = TRUE;
}

void iv_mask_view_load(IvMaskView *v, const uint8_t *mask)
{
	uint8_t *tmp;

	if (!v)
		return;
	if (!mask) {
		iv_mask_view_clear(v);
		return;
	}
	cairo_surface_flush(v->surface);
	if (v->stride == v->w) {
		iv_mask_scale(mask, v->mw, v->mh, v->data, v->w, v->h);
	} else {
		/* The surface's rows are padded: scale into a packed copy and
		 * move the rows over. */
		tmp = g_try_malloc((gsize)v->w * (gsize)v->h);
		if (!tmp) {
			memset(v->data, 0, (gsize)v->stride * (gsize)v->h);
		} else {
			iv_mask_scale(mask, v->mw, v->mh, tmp, v->w, v->h);
			for (int y = 0; y < v->h; y++) {
				memcpy(v->data + (gsize)y * v->stride,
				       tmp + (gsize)y * v->w, v->w);
				memset(v->data + (gsize)y * v->stride + v->w, 0,
				       v->stride - v->w);
			}
			g_free(tmp);
		}
	}
	cairo_surface_mark_dirty(v->surface);
	v->empty = FALSE;
}

void iv_mask_view_line(IvMaskView *v, double x0, double y0, double x1,
		       double y1, double r, int value, double hardness)
{
	if (!v)
		return;
	cairo_surface_flush(v->surface);
	/* At least a pixel of radius: a thin brush still shows. */
	iv_brush_line(v->data, v->stride, v->w, v->h, x0 * v->k, y0 * v->k,
		      x1 * v->k, y1 * v->k, MAX(1.0, r * v->k), value, hardness,
		      NULL);
	cairo_surface_mark_dirty(v->surface);
	if (value > 0)
		v->empty = FALSE;
}

/* The hatching: one tile of 45-degree lines that repeats seamlessly. */
#define HATCH_TILE 10

static cairo_surface_t *hatch_tile(void)
{
	static cairo_surface_t *tile;
	cairo_t *cr;

	if (tile)
		return tile;
	tile = cairo_image_surface_create(CAIRO_FORMAT_A8, HATCH_TILE, HATCH_TILE);
	cr = cairo_create(tile);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	cairo_set_line_width(cr, 1.5);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
	/* x + y = c for three values of c: the middle line and the two
	 * corners it continues into on the neighbouring tiles. */
	for (int c = 0; c <= 2 * HATCH_TILE; c += HATCH_TILE) {
		cairo_move_to(cr, c + 1, -1);
		cairo_line_to(cr, -1, c + 1);
	}
	cairo_stroke(cr);
	cairo_destroy(cr);
	return tile;
}

void iv_mask_view_paint(IvMaskView *v, cairo_t *cr,
			const cairo_matrix_t *src_to_widget, double mask_scale,
			IvMaskLook look, guint32 accent)
{
	double r = ((accent >> 24) & 0xff) / 255.0;
	double g = ((accent >> 16) & 0xff) / 255.0;
	double b = ((accent >> 8) & 0xff) / 255.0;
	double a = (accent & 0xff) / 255.0;
	cairo_pattern_t *mask;
	cairo_matrix_t pm;

	if (!v || v->empty)
		return;
	/* The mask pattern maps widget pixels to its own pixels: view <-
	 * MASK <- SOURCE <- widget. */
	pm = *src_to_widget;
	if (cairo_matrix_invert(&pm) != CAIRO_STATUS_SUCCESS)
		return;
	{
		cairo_matrix_t to_view;

		cairo_matrix_init_scale(&to_view, mask_scale * v->k,
					mask_scale * v->k);
		cairo_matrix_multiply(&pm, &pm, &to_view);
	}
	cairo_surface_flush(v->surface);
	mask = cairo_pattern_create_for_surface(v->surface);
	cairo_pattern_set_matrix(mask, &pm);
	cairo_pattern_set_filter(mask, CAIRO_FILTER_BILINEAR);
	cairo_pattern_set_extend(mask, CAIRO_EXTEND_NONE);

	cairo_save(cr);
	if (look == IV_MASK_HATCH) {
		cairo_pattern_t *lines = cairo_pattern_create_for_surface(hatch_tile());
		cairo_surface_t *group;
		cairo_pattern_t *gp;
		cairo_t *gc;
		double x1, y1, x2, y2;

		/* A faint wash, so the inside reads as one area ... */
		cairo_set_source_rgba(cr, r, g, b, a * 0.22);
		cairo_mask(cr, mask);
		/* ... and the lines, in widget space (they stay 2 px apart
		 * at any zoom), masked by the selection.  Lines times mask
		 * is a mask of its own: made in a group the size of what is
		 * being drawn. */
		cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
		group = cairo_surface_create_similar(cairo_get_target(cr),
						     CAIRO_CONTENT_ALPHA,
						     MAX(1, (int)ceil(x2 - x1)),
						     MAX(1, (int)ceil(y2 - y1)));
		gc = cairo_create(group);
		cairo_translate(gc, -floor(x1), -floor(y1));
		cairo_pattern_set_extend(lines, CAIRO_EXTEND_REPEAT);
		cairo_set_source(gc, lines);
		cairo_mask(gc, mask);
		cairo_destroy(gc);
		gp = cairo_pattern_create_for_surface(group);
		{
			cairo_matrix_t gm;

			cairo_matrix_init_translate(&gm, -floor(x1), -floor(y1));
			cairo_pattern_set_matrix(gp, &gm);
		}
		cairo_set_source_rgba(cr, r, g, b, MIN(1.0, a * 1.1));
		cairo_mask(cr, gp);
		cairo_pattern_destroy(gp);
		cairo_surface_destroy(group);
		cairo_pattern_destroy(lines);
	} else {
		cairo_set_source_rgba(cr, r, g, b, a);
		cairo_mask(cr, mask);
	}
	cairo_restore(cr);
	cairo_pattern_destroy(mask);
}

/* ---- the brush outline ---------------------------------------------------- */

void iv_brush_set_rgba(cairo_t *cr, guint32 c, double alpha_scale)
{
	cairo_set_source_rgba(cr, ((c >> 24) & 0xff) / 255.0,
			      ((c >> 16) & 0xff) / 255.0,
			      ((c >> 8) & 0xff) / 255.0,
			      (c & 0xff) / 255.0 * alpha_scale);
}

static void hot_spot_cross(cairo_t *cr, double x, double y)
{
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 0) {
			cairo_set_source_rgba(cr, 0.04, 0.06, 0.09, 0.7);
			cairo_set_line_width(cr, 3);
		} else {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
			cairo_set_line_width(cr, 1.25);
		}
		cairo_move_to(cr, x - 6, y);
		cairo_line_to(cr, x - 2.5, y);
		cairo_move_to(cr, x + 2.5, y);
		cairo_line_to(cr, x + 6, y);
		cairo_move_to(cr, x, y - 6);
		cairo_line_to(cr, x, y - 2.5);
		cairo_move_to(cr, x, y + 2.5);
		cairo_line_to(cr, x, y + 6);
		cairo_stroke(cr);
	}
}

void iv_brush_draw_cursor(cairo_t *cr, double x, double y, double r,
			  IvCursorLook look, guint32 fill)
{
	static const double dash[] = { 4.0, 3.0 };

	cairo_save(cr);
	cairo_new_path(cr);
	r = MAX(r, 1.0);
	if (fill & 0xff) {
		cairo_arc(cr, x, y, r, 0, 2 * G_PI);
		iv_brush_set_rgba(cr, fill, 0.3);
		cairo_fill(cr);
	}
	if (look == IV_CURSOR_ERASER) {
		cairo_arc(cr, x, y, r, 0, 2 * G_PI);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
		cairo_fill(cr);
	}
	/* Dark under light: one of the two shows on any picture. */
	cairo_arc(cr, x, y, r, 0, 2 * G_PI);
	cairo_set_source_rgba(cr, 0.04, 0.06, 0.09, 0.6);
	cairo_set_line_width(cr, 3);
	cairo_stroke(cr);
	cairo_arc(cr, x, y, r, 0, 2 * G_PI);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
	cairo_set_line_width(cr, 1.25);
	if (look == IV_CURSOR_DASHED)
		cairo_set_dash(cr, dash, G_N_ELEMENTS(dash), 0);
	cairo_stroke(cr);
	cairo_set_dash(cr, NULL, 0, 0);
	if (r < 7)
		hot_spot_cross(cr, x, y);
	cairo_restore(cr);
}

void iv_brush_draw_nib(cairo_t *cr, double x, double y, double len,
		       double angle, guint32 fill)
{
	double dx = cos(angle) * MAX(len, 4) / 2, dy = sin(angle) * MAX(len, 4) / 2;

	cairo_save(cr);
	cairo_new_path(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, x - dx, y - dy);
	cairo_line_to(cr, x + dx, y + dy);
	cairo_set_source_rgba(cr, 0.04, 0.06, 0.09, 0.6);
	cairo_set_line_width(cr, 5);
	cairo_stroke_preserve(cr);
	if (fill & 0xff)
		iv_brush_set_rgba(cr, fill | 0xff, 1.0);
	else
		cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
	cairo_set_line_width(cr, 2.25);
	cairo_stroke(cr);
	if (len < 12)
		hot_spot_cross(cr, x, y);
	cairo_restore(cr);
}

/* ---- tool-owned sliders ----------------------------------------------------- */

#define SLIDER_STEPS 1000.0

typedef struct {
	double min, max;
	gboolean log;
	char *unit;
	IvBrushSliderChanged changed;
	gpointer data;
	GtkWidget *scale, *value;
	gboolean syncing, dragging, custom_text;
} BrushSlider;

static void brush_slider_free(gpointer p)
{
	BrushSlider *s = p;

	g_free(s->unit);
	g_free(s);
}

/* Slider position (0..SLIDER_STEPS) <-> value. */
static double pos_to_value(BrushSlider *s, double pos)
{
	double t = CLAMP(pos / SLIDER_STEPS, 0, 1);

	if (s->log && s->min > 0)
		return s->min * pow(s->max / s->min, t);
	return s->min + (s->max - s->min) * t;
}

static double value_to_pos(BrushSlider *s, double v)
{
	v = CLAMP(v, s->min, s->max);
	if (s->log && s->min > 0)
		return SLIDER_STEPS * log(v / s->min) / log(s->max / s->min);
	return SLIDER_STEPS * (v - s->min) / (s->max - s->min);
}

static void brush_slider_show(BrushSlider *s, double v)
{
	char buf[32];

	if (s->custom_text)
		return;
	g_snprintf(buf, sizeof buf, "%d%s", (int)lround(v), s->unit ? s->unit : "");
	gtk_label_set_text(GTK_LABEL(s->value), buf);
}

static void brush_slider_moved(GtkRange *r, BrushSlider *s)
{
	double v = pos_to_value(s, gtk_range_get_value(r));

	brush_slider_show(s, v);
	if (!s->syncing && s->changed)
		s->changed(v, !s->dragging, s->data);
}

static gboolean brush_slider_press(GtkWidget *w, GdkEventButton *ev,
				   BrushSlider *s)
{
	if (ev->button == 1)
		s->dragging = TRUE;
	return FALSE;
}

static gboolean brush_slider_release(GtkWidget *w, GdkEventButton *ev,
				     BrushSlider *s)
{
	if (ev->button != 1)
		return FALSE;
	s->dragging = FALSE;
	if (s->changed)
		s->changed(pos_to_value(s, gtk_range_get_value(GTK_RANGE(s->scale))),
			   TRUE, s->data);
	return FALSE;
}

GtkWidget *iv_brush_slider(const char *label, double min, double max,
			   double value, gboolean logarithmic, const char *unit,
			   IvBrushSliderChanged changed, gpointer data)
{
	BrushSlider *s = g_new0(BrushSlider, 1);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *l = gtk_label_new(label);

	s->min = min;
	s->max = max > min ? max : min + 1;
	s->log = logarithmic && min > 0;
	s->unit = g_strdup(unit);
	s->changed = changed;
	s->data = data;
	iv_add_class(row, "iv-slider-row");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	iv_add_class(l, "iv-slider-label");
	s->value = gtk_label_new(NULL);
	iv_add_class(s->value, "iv-slider-value");
	gtk_box_pack_start(GTK_BOX(head), l, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(head), s->value, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), head, FALSE, FALSE, 0);

	s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0,
					    SLIDER_STEPS, 1);
	gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
	iv_add_class(s->scale, "iv-slider");
	gtk_widget_set_can_focus(s->scale, TRUE);
	g_signal_connect(s->scale, "value-changed", G_CALLBACK(brush_slider_moved),
			 s);
	g_signal_connect(s->scale, "button-press-event",
			 G_CALLBACK(brush_slider_press), s);
	g_signal_connect(s->scale, "button-release-event",
			 G_CALLBACK(brush_slider_release), s);
	gtk_box_pack_start(GTK_BOX(row), s->scale, FALSE, FALSE, 0);
	g_object_set_data_full(G_OBJECT(row), "iv-brush-slider", s,
			       brush_slider_free);
	g_object_set_data(G_OBJECT(row), "iv-scale", s->scale);
	iv_brush_slider_set(row, value);
	return row;
}

void iv_brush_slider_set(GtkWidget *row, double value)
{
	BrushSlider *s = g_object_get_data(G_OBJECT(row), "iv-brush-slider");

	if (!s)
		return;
	s->syncing = TRUE;
	gtk_range_set_value(GTK_RANGE(s->scale), value_to_pos(s, value));
	s->syncing = FALSE;
	brush_slider_show(s, CLAMP(value, s->min, s->max));
}

double iv_brush_slider_get(GtkWidget *row)
{
	BrushSlider *s = g_object_get_data(G_OBJECT(row), "iv-brush-slider");

	return s ? pos_to_value(s, gtk_range_get_value(GTK_RANGE(s->scale))) : 0;
}

void iv_brush_slider_set_text(GtkWidget *row, const char *text)
{
	BrushSlider *s = g_object_get_data(G_OBJECT(row), "iv-brush-slider");

	if (!s)
		return;
	s->custom_text = text != NULL;
	if (text)
		gtk_label_set_text(GTK_LABEL(s->value), text);
	else
		brush_slider_show(s, iv_brush_slider_get(row));
}

/* ---- small things ---------------------------------------------------------------- */

GtkWidget *iv_brush_section(GtkWidget *panel, IvGlyph g, const char *title,
			    GtkWidget **reset)
{
	GtkWidget *row = iv_panel_section(panel, title, reset);
	GtkWidget *glyph = iv_glyph_new(g, 14);

	iv_add_class(glyph, "iv-section-glyph");
	gtk_box_pack_start(GTK_BOX(row), glyph, FALSE, FALSE, 0);
	gtk_box_reorder_child(GTK_BOX(row), glyph, 0);
	return row;
}

GtkWidget *iv_brush_caption(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	iv_add_class(l, "iv-brush-caption");
	return l;
}

GtkWidget *iv_brush_tip(const char *text)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *dot = gtk_label_new("•");
	GtkWidget *l = gtk_label_new(text);

	gtk_widget_set_valign(dot, GTK_ALIGN_START);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 36);
	iv_add_class(row, "iv-brush-tip");
	gtk_box_pack_start(GTK_BOX(row), dot, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
	return row;
}

void iv_brush_paint_thumb(cairo_t *cr, cairo_surface_t *s, double x, double y,
			  double w, double h, double r)
{
	cairo_pattern_t *ch;
	double sw, sh, k;

	cairo_save(cr);
	iv_rounded_rect(cr, x, y, w, h, r);
	cairo_clip(cr);
	ch = iv_checker_pattern(5);
	cairo_set_source(cr, ch);
	cairo_paint(cr);
	cairo_pattern_destroy(ch);
	if (s) {
		sw = cairo_image_surface_get_width(s);
		sh = cairo_image_surface_get_height(s);
		if (sw > 0 && sh > 0) {
			/* Cover: the thumbnail fills the box, centred. */
			k = MAX(w / sw, h / sh);
			cairo_translate(cr, x + (w - sw * k) / 2, y + (h - sh * k) / 2);
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, s, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
		}
	}
	cairo_restore(cr);
}
