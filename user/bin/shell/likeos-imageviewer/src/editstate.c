// LikeOS -- LikeOS Image Viewer: the edit state, its geometry and its
// history.
//
// See editstate.h for the frames and the ownership rules.  The geometry
// conversions here are exact inverses of each other and follow the render
// pipeline's order of operations (turn, mirror, straighten, crop), so a
// point a tool picks on the screen lands on the same source pixel the
// pipeline reads from.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "editstate.h"

/* ---- aspect ratios ---------------------------------------------------------- */

static const struct {
	const char *label;
	double w, h;
} aspects[IV_ASPECT_COUNT] = {
	[IV_ASPECT_FREE] = { "Free", 0, 0 },
	[IV_ASPECT_ORIGINAL] = { "Original", 0, 0 },
	[IV_ASPECT_SQUARE] = { "Square", 1, 1 },
	[IV_ASPECT_16_9] = { "16:9", 16, 9 },
	[IV_ASPECT_9_16] = { "9:16", 9, 16 },
	[IV_ASPECT_4_3] = { "4:3", 4, 3 },
	[IV_ASPECT_3_4] = { "3:4", 3, 4 },
	[IV_ASPECT_3_2] = { "3:2", 3, 2 },
	[IV_ASPECT_2_3] = { "2:3", 2, 3 },
	[IV_ASPECT_5_4] = { "5:4", 5, 4 },
	[IV_ASPECT_4_5] = { "4:5", 4, 5 },
	[IV_ASPECT_7_5] = { "7:5", 7, 5 },
	[IV_ASPECT_5_7] = { "5:7", 5, 7 },
	[IV_ASPECT_2_1] = { "2:1", 2, 1 },
	[IV_ASPECT_1_2] = { "1:2", 1, 2 },
};

const char *iv_aspect_label(IvAspect a)
{
	return (int)a >= 0 && a < IV_ASPECT_COUNT ? aspects[a].label : "";
}

double iv_aspect_ratio(IvAspect a, double fw, double fh)
{
	if ((int)a < 0 || a >= IV_ASPECT_COUNT || a == IV_ASPECT_FREE)
		return 0;
	if (a == IV_ASPECT_ORIGINAL)
		return fh > 0 ? fw / fh : 0;
	return aspects[a].w / aspects[a].h;
}

IvAspect iv_aspect_swapped(IvAspect a)
{
	if ((int)a < 0 || a >= IV_ASPECT_COUNT || aspects[a].w == aspects[a].h)
		return a;
	for (int i = 0; i < IV_ASPECT_COUNT; i++)
		if (aspects[i].w == aspects[a].h && aspects[i].h == aspects[a].w)
			return (IvAspect)i;
	return a;
}

/* ---- geometry ------------------------------------------------------------------- */

void iv_geometry_reset(IvGeometry *g)
{
	memset(g, 0, sizeof *g);
	g->crop_w = 1;
	g->crop_h = 1;
	g->aspect = IV_ASPECT_FREE;
}

gboolean iv_geometry_has_crop(const IvGeometry *g)
{
	return fabs(g->crop_x) > 1e-6 || fabs(g->crop_y) > 1e-6 ||
	       fabs(g->crop_w - 1) > 1e-6 || fabs(g->crop_h - 1) > 1e-6;
}

gboolean iv_geometry_is_identity(const IvGeometry *g)
{
	return (g->quarter_turns & 3) == 0 && !g->flip_h && !g->flip_v &&
	       fabs(g->straighten) < 1e-6 && !iv_geometry_has_crop(g);
}

gboolean iv_geometry_equal(const IvGeometry *a, const IvGeometry *b)
{
	return (a->quarter_turns & 3) == (b->quarter_turns & 3) &&
	       !a->flip_h == !b->flip_h && !a->flip_v == !b->flip_v &&
	       fabs(a->straighten - b->straighten) < 1e-9 &&
	       fabs(a->crop_x - b->crop_x) < 1e-9 &&
	       fabs(a->crop_y - b->crop_y) < 1e-9 &&
	       fabs(a->crop_w - b->crop_w) < 1e-9 &&
	       fabs(a->crop_h - b->crop_h) < 1e-9 && a->aspect == b->aspect;
}

static int turns(const IvGeometry *g)
{
	return ((g->quarter_turns % 4) + 4) % 4;
}

void iv_geometry_turned_size(const IvGeometry *g, int sw, int sh, int *tw,
			     int *th)
{
	gboolean across = turns(g) & 1;

	*tw = across ? sh : sw;
	*th = across ? sw : sh;
}

void iv_geometry_frame_size(const IvGeometry *g, int sw, int sh, double *fw,
			    double *fh)
{
	int tw, th;

	iv_geometry_turned_size(g, sw, sh, &tw, &th);
	if (fabs(g->straighten) < 1e-6) {
		*fw = tw;
		*fh = th;
		return;
	}
	iv_straighten_inner_rect_aspect(tw, th, g->straighten, fw, fh);
	/* Whole pixels, as the pipeline's straighten produces them. */
	*fw = MAX(1, floor(*fw + 1e-6));
	*fh = MAX(1, floor(*fh + 1e-6));
}

void iv_geometry_crop_rect(const IvGeometry *g, int sw, int sh, double *x,
			   double *y, double *w, double *h)
{
	double fw, fh;

	iv_geometry_frame_size(g, sw, sh, &fw, &fh);
	*x = g->crop_x * fw;
	*y = g->crop_y * fh;
	*w = g->crop_w * fw;
	*h = g->crop_h * fh;
}

void iv_geometry_output_size(const IvGeometry *g, int sw, int sh, int *ow,
			     int *oh)
{
	double fw, fh;

	iv_geometry_frame_size(g, sw, sh, &fw, &fh);
	*ow = MAX(1, (int)lround(g->crop_w * fw));
	*oh = MAX(1, (int)lround(g->crop_h * fh));
}

void iv_geometry_source_to_turned(const IvGeometry *g, int sw, int sh,
				  double sx, double sy, double *tx, double *ty)
{
	int tw, th;
	double x, y;

	switch (turns(g)) {
	case 1:
		x = sh - sy;
		y = sx;
		break;
	case 2:
		x = sw - sx;
		y = sh - sy;
		break;
	case 3:
		x = sy;
		y = sw - sx;
		break;
	default:
		x = sx;
		y = sy;
		break;
	}
	iv_geometry_turned_size(g, sw, sh, &tw, &th);
	if (g->flip_h)
		x = tw - x;
	if (g->flip_v)
		y = th - y;
	*tx = x;
	*ty = y;
}

void iv_geometry_turned_to_source(const IvGeometry *g, int sw, int sh,
				  double tx, double ty, double *sx, double *sy)
{
	int tw, th;

	iv_geometry_turned_size(g, sw, sh, &tw, &th);
	if (g->flip_h)
		tx = tw - tx;
	if (g->flip_v)
		ty = th - ty;
	switch (turns(g)) {
	case 1:
		*sx = ty;
		*sy = sh - tx;
		break;
	case 2:
		*sx = sw - tx;
		*sy = sh - ty;
		break;
	case 3:
		*sx = sw - ty;
		*sy = tx;
		break;
	default:
		*sx = tx;
		*sy = ty;
		break;
	}
}

/* Straightening turns about the centre: clockwise on the screen (y down)
 * is the ordinary rotation matrix. */
void iv_geometry_turned_to_frame(const IvGeometry *g, int sw, int sh,
				 double tx, double ty, double *fx, double *fy)
{
	int tw, th;
	double fw, fh, a = g->straighten * G_PI / 180.0;
	double dx, dy;

	iv_geometry_turned_size(g, sw, sh, &tw, &th);
	iv_geometry_frame_size(g, sw, sh, &fw, &fh);
	dx = tx - tw / 2.0;
	dy = ty - th / 2.0;
	*fx = dx * cos(a) - dy * sin(a) + fw / 2.0;
	*fy = dx * sin(a) + dy * cos(a) + fh / 2.0;
}

void iv_geometry_frame_to_turned(const IvGeometry *g, int sw, int sh,
				 double fx, double fy, double *tx, double *ty)
{
	int tw, th;
	double fw, fh, a = g->straighten * G_PI / 180.0;
	double dx, dy;

	iv_geometry_turned_size(g, sw, sh, &tw, &th);
	iv_geometry_frame_size(g, sw, sh, &fw, &fh);
	dx = fx - fw / 2.0;
	dy = fy - fh / 2.0;
	*tx = dx * cos(a) + dy * sin(a) + tw / 2.0;
	*ty = -dx * sin(a) + dy * cos(a) + th / 2.0;
}

void iv_geometry_frame_to_output(const IvGeometry *g, int sw, int sh,
				 double fx, double fy, double *ox, double *oy)
{
	double x, y, w, h;

	iv_geometry_crop_rect(g, sw, sh, &x, &y, &w, &h);
	*ox = fx - x;
	*oy = fy - y;
}

void iv_geometry_output_to_frame(const IvGeometry *g, int sw, int sh,
				 double ox, double oy, double *fx, double *fy)
{
	double x, y, w, h;

	iv_geometry_crop_rect(g, sw, sh, &x, &y, &w, &h);
	*fx = ox + x;
	*fy = oy + y;
}

void iv_geometry_source_to_output(const IvGeometry *g, int sw, int sh,
				  double sx, double sy, double *ox, double *oy)
{
	double tx, ty, fx, fy;

	iv_geometry_source_to_turned(g, sw, sh, sx, sy, &tx, &ty);
	iv_geometry_turned_to_frame(g, sw, sh, tx, ty, &fx, &fy);
	iv_geometry_frame_to_output(g, sw, sh, fx, fy, ox, oy);
}

void iv_geometry_output_to_source(const IvGeometry *g, int sw, int sh,
				  double ox, double oy, double *sx, double *sy)
{
	double fx, fy, tx, ty;

	iv_geometry_output_to_frame(g, sw, sh, ox, oy, &fx, &fy);
	iv_geometry_frame_to_turned(g, sw, sh, fx, fy, &tx, &ty);
	iv_geometry_turned_to_source(g, sw, sh, tx, ty, sx, sy);
}

/* ---- masks ---------------------------------------------------------------------- */

IvMask *iv_mask_new(int w, int h)
{
	IvMask *m;

	if (w <= 0 || h <= 0)
		return NULL;
	m = g_new0(IvMask, 1);
	m->ref = 1;
	m->w = w;
	m->h = h;
	m->data = g_try_malloc0((gsize)w * (gsize)h);
	if (!m->data) {
		g_free(m);
		return NULL;
	}
	return m;
}

IvMask *iv_mask_ref(IvMask *m)
{
	if (m)
		g_atomic_int_inc(&m->ref);
	return m;
}

void iv_mask_unref(IvMask *m)
{
	if (!m || !g_atomic_int_dec_and_test(&m->ref))
		return;
	g_free(m->data);
	g_free(m);
}

IvMask *iv_mask_copy(const IvMask *m)
{
	IvMask *c;

	if (!m)
		return NULL;
	c = iv_mask_new(m->w, m->h);
	if (c)
		memcpy(c->data, m->data, (gsize)m->w * (gsize)m->h);
	return c;
}

IvMask *iv_mask_resized(const IvMask *m, int w, int h)
{
	IvMask *c;

	if (!m)
		return NULL;
	if (m->w == w && m->h == h)
		return iv_mask_copy(m);
	c = iv_mask_new(w, h);
	if (c)
		iv_mask_scale(m->data, m->w, m->h, c->data, w, h);
	return c;
}

gboolean iv_mask_is_empty(const IvMask *m)
{
	if (!m)
		return TRUE;
	gsize n = (gsize)m->w * (gsize)m->h;

	for (gsize i = 0; i < n; i++)
		if (m->data[i])
			return FALSE;
	return TRUE;
}

/* ---- strokes ------------------------------------------------------------------- */

IvStroke *iv_stroke_new(IvStrokeKind kind, guint32 rgba, double width)
{
	IvStroke *s = g_new0(IvStroke, 1);

	s->ref = 1;
	s->kind = kind;
	s->rgba = rgba;
	s->width = width;
	s->angle = -G_PI / 4;
	return s;
}

IvStroke *iv_stroke_ref(IvStroke *s)
{
	if (s)
		g_atomic_int_inc(&s->ref);
	return s;
}

void iv_stroke_unref(IvStroke *s)
{
	if (!s || !g_atomic_int_dec_and_test(&s->ref))
		return;
	g_free(s->pts);
	g_free(s);
}

void iv_stroke_add_point(IvStroke *s, double x, double y)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 64;
		s->pts = g_renew(double, s->pts, (gsize)s->cap * 2);
	}
	s->pts[2 * s->n] = x;
	s->pts[2 * s->n + 1] = y;
	s->n++;
}

void iv_stroke_bounds(const IvStroke *s, double aspect, double *x0,
		      double *y0, double *x1, double *y1)
{
	double hw, hh;

	*x0 = *y0 = 1e9;
	*x1 = *y1 = -1e9;
	for (int i = 0; i < s->n; i++) {
		*x0 = MIN(*x0, s->pts[2 * i]);
		*y0 = MIN(*y0, s->pts[2 * i + 1]);
		*x1 = MAX(*x1, s->pts[2 * i]);
		*y1 = MAX(*y1, s->pts[2 * i + 1]);
	}
	/* The width is a fraction of the long edge: in normalised units it
	 * is larger across the short one. */
	aspect = aspect > 0 ? aspect : 1;
	hw = s->width / 2 * (aspect >= 1 ? 1 : 1 / aspect);
	hh = s->width / 2 * (aspect >= 1 ? aspect : 1);
	*x0 -= hw;
	*x1 += hw;
	*y0 -= hh;
	*y1 += hh;
}

/* ---- the state ---------------------------------------------------------------- */

static GPtrArray *mask_list(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)iv_mask_unref);
}

static GPtrArray *stroke_list(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)iv_stroke_unref);
}

IvEditState *iv_edit_state_new(void)
{
	IvEditState *s = g_new0(IvEditState, 1);

	iv_geometry_reset(&s->geo);
	iv_adjust_reset(&s->adjust);
	s->style = IV_STYLE_NONE;
	s->style_intensity = 1.0;
	s->retouch = mask_list();
	s->strokes = stroke_list();
	s->bg.op = IV_BG_BLUR;
	s->bg.strength = 50;
	s->bg.rgba = 0xffffffffu;
	return s;
}

static void copy_lists(IvEditState *dst, const IvEditState *src)
{
	dst->retouch = mask_list();
	for (guint i = 0; i < src->retouch->len; i++)
		g_ptr_array_add(dst->retouch,
				iv_mask_ref(g_ptr_array_index(src->retouch, i)));
	dst->strokes = stroke_list();
	for (guint i = 0; i < src->strokes->len; i++)
		g_ptr_array_add(dst->strokes,
				iv_stroke_ref(g_ptr_array_index(src->strokes, i)));
	dst->bg.mask = iv_mask_ref(src->bg.mask);
}

IvEditState *iv_edit_state_copy(const IvEditState *s)
{
	IvEditState *c;

	if (!s)
		return iv_edit_state_new();
	c = g_new0(IvEditState, 1);
	*c = *s;
	copy_lists(c, s);
	return c;
}

void iv_edit_state_free(IvEditState *s)
{
	if (!s)
		return;
	g_ptr_array_unref(s->retouch);
	g_ptr_array_unref(s->strokes);
	iv_mask_unref(s->bg.mask);
	g_free(s);
}

void iv_edit_state_assign(IvEditState *dst, const IvEditState *src)
{
	GPtrArray *r = dst->retouch, *st = dst->strokes;
	IvMask *m = dst->bg.mask;

	*dst = *src;
	copy_lists(dst, src);
	g_ptr_array_unref(r);
	g_ptr_array_unref(st);
	iv_mask_unref(m);
}

static gboolean same_ptrs(GPtrArray *a, GPtrArray *b)
{
	if (a->len != b->len)
		return FALSE;
	for (guint i = 0; i < a->len; i++)
		if (g_ptr_array_index(a, i) != g_ptr_array_index(b, i))
			return FALSE;
	return TRUE;
}

static gboolean adjust_equal(const IvAdjust *a, const IvAdjust *b)
{
	const double *pa = (const double *)a, *pb = (const double *)b;

	for (gsize i = 0; i < sizeof(IvAdjust) / sizeof(double); i++)
		if (fabs(pa[i] - pb[i]) > 1e-9)
			return FALSE;
	return TRUE;
}

static gboolean bg_equal(const IvBackgroundEdit *a, const IvBackgroundEdit *b)
{
	if (!a->active && !b->active)
		return a->mask == b->mask;
	return a->active == b->active && a->mask == b->mask && a->op == b->op &&
	       fabs(a->strength - b->strength) < 1e-9 && a->rgba == b->rgba;
}

gboolean iv_edit_state_equal(const IvEditState *a, const IvEditState *b)
{
	if (a == b)
		return TRUE;
	if (!a || !b)
		return FALSE;
	return iv_geometry_equal(&a->geo, &b->geo) &&
	       adjust_equal(&a->adjust, &b->adjust) && a->style == b->style &&
	       (a->style == IV_STYLE_NONE ||
		fabs(a->style_intensity - b->style_intensity) < 1e-9) &&
	       same_ptrs(a->retouch, b->retouch) &&
	       same_ptrs(a->strokes, b->strokes) && bg_equal(&a->bg, &b->bg);
}

gboolean iv_edit_state_is_identity(const IvEditState *s)
{
	return iv_geometry_is_identity(&s->geo) &&
	       iv_adjust_is_neutral(&s->adjust) &&
	       (s->style == IV_STYLE_NONE || s->style_intensity <= 0) &&
	       s->retouch->len == 0 && s->strokes->len == 0 &&
	       !(s->bg.active && s->bg.mask);
}

void iv_edit_state_add_retouch(IvEditState *s, IvMask *m)
{
	if (m)
		g_ptr_array_add(s->retouch, iv_mask_ref(m));
}

void iv_edit_state_add_stroke(IvEditState *s, IvStroke *st)
{
	if (st)
		g_ptr_array_add(s->strokes, iv_stroke_ref(st));
}

void iv_edit_state_remove_stroke(IvEditState *s, guint index)
{
	if (index < s->strokes->len)
		g_ptr_array_remove_index(s->strokes, index);
}

void iv_edit_state_replace_stroke(IvEditState *s, guint index, IvStroke *st)
{
	if (!st || index >= s->strokes->len)
		return;
	iv_stroke_unref(g_ptr_array_index(s->strokes, index));
	g_ptr_array_index(s->strokes, index) = iv_stroke_ref(st);
}

int iv_edit_state_find_stroke(const IvEditState *s, const IvStroke *st)
{
	for (guint i = 0; st && i < s->strokes->len; i++)
		if (g_ptr_array_index(s->strokes, i) == st)
			return (int)i;
	return -1;
}

void iv_edit_state_clear_strokes(IvEditState *s)
{
	g_ptr_array_set_size(s->strokes, 0);
}

void iv_edit_state_set_bg_mask(IvEditState *s, IvMask *m)
{
	IvMask *old = s->bg.mask;

	s->bg.mask = iv_mask_ref(m);
	iv_mask_unref(old);
	if (!m)
		s->bg.active = FALSE;
}

/* ---- history ------------------------------------------------------------------ */

typedef struct {
	IvEditState *state;
	char *label;
} Entry;

struct IvHistory {
	GQueue undo;	/* Entry*, newest at the head */
	GQueue redo;
};

static void entry_free(gpointer p)
{
	Entry *e = p;

	iv_edit_state_free(e->state);
	g_free(e->label);
	g_free(e);
}

IvHistory *iv_history_new(void)
{
	IvHistory *h = g_new0(IvHistory, 1);

	g_queue_init(&h->undo);
	g_queue_init(&h->redo);
	return h;
}

void iv_history_clear(IvHistory *h)
{
	g_queue_clear_full(&h->undo, entry_free);
	g_queue_clear_full(&h->redo, entry_free);
	g_queue_init(&h->undo);
	g_queue_init(&h->redo);
}

void iv_history_free(IvHistory *h)
{
	if (!h)
		return;
	iv_history_clear(h);
	g_free(h);
}

static Entry *entry_new(IvEditState *s, const char *label)
{
	Entry *e = g_new0(Entry, 1);

	e->state = s;
	e->label = g_strdup(label ? label : "Edit");
	return e;
}

void iv_history_push(IvHistory *h, IvEditState *before, const char *label)
{
	g_queue_push_head(&h->undo, entry_new(before, label));
	while (h->undo.length > IV_HISTORY_MAX)
		entry_free(g_queue_pop_tail(&h->undo));
	g_queue_clear_full(&h->redo, entry_free);
	g_queue_init(&h->redo);
}

IvEditState *iv_history_undo(IvHistory *h, IvEditState *current)
{
	Entry *e = g_queue_pop_head(&h->undo);
	IvEditState *s;

	if (!e)
		return NULL;
	/* The label travels with the step: redo says what it redoes. */
	g_queue_push_head(&h->redo, entry_new(current, e->label));
	s = e->state;
	g_free(e->label);
	g_free(e);
	return s;
}

IvEditState *iv_history_redo(IvHistory *h, IvEditState *current)
{
	Entry *e = g_queue_pop_head(&h->redo);
	IvEditState *s;

	if (!e)
		return NULL;
	g_queue_push_head(&h->undo, entry_new(current, e->label));
	s = e->state;
	g_free(e->label);
	g_free(e);
	return s;
}

gboolean iv_history_can_undo(IvHistory *h)
{
	return h && h->undo.length > 0;
}

gboolean iv_history_can_redo(IvHistory *h)
{
	return h && h->redo.length > 0;
}

const char *iv_history_undo_label(IvHistory *h)
{
	Entry *e = h ? g_queue_peek_head(&h->undo) : NULL;

	return e ? e->label : NULL;
}

const char *iv_history_redo_label(IvHistory *h)
{
	Entry *e = h ? g_queue_peek_head(&h->redo) : NULL;

	return e ? e->label : NULL;
}

guint iv_history_depth(IvHistory *h)
{
	return h ? h->undo.length : 0;
}
