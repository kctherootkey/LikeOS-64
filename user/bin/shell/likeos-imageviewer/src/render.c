// LikeOS -- LikeOS Image Viewer: the render pipeline.
//
// See render.h for the order of the steps.  Two things make it fast
// enough to run on every slider movement: the preview works on a proxy of
// the source, never on the full picture, and the slow retouch steps are
// kept -- the result after each inpainted mask is remembered per size, so
// changing a slider after retouching ten spots repeats none of them, and
// undoing the last spot starts from the result of the one before.
//
// A step that cannot get the memory it needs stops the render: the result
// would otherwise be a picture with that step quietly missing, and the
// editor must never save one (iv_render_ex says which step it was).
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "render.h"

/* Retouch results kept per size: beyond this the older ones in the
 * middle are let go (they can always be recomputed from an earlier one). */
#define IV_CHAIN_KEEP 16
#define IV_SCALED_KEEP 24

/* ---- what a render shows ------------------------------------------------------ */

typedef enum {
	SHOW_SOURCE,
	SHOW_TURNED,
	SHOW_FRAME,
	SHOW_STEADY,	/* the FRAME centred in a TURNED-sized canvas */
	SHOW_OUTPUT
} Shown;

static IvRenderFlags normalise(IvRenderFlags f)
{
	if (f & IV_RENDER_NO_STRAIGHTEN)
		f |= IV_RENDER_NO_CROP;
	if (f & (IV_RENDER_NO_GEOMETRY | IV_RENDER_NO_STRAIGHTEN))
		f |= IV_RENDER_NO_DRAW;
	/* The steady canvas is a way of showing the uncut, straightened
	 * picture; without one it means nothing. */
	if (!(f & IV_RENDER_NO_CROP) ||
	    (f & (IV_RENDER_NO_GEOMETRY | IV_RENDER_NO_STRAIGHTEN)))
		f &= ~IV_RENDER_STEADY;
	return f;
}

static Shown shown(IvRenderFlags f)
{
	f = normalise(f);
	if (f & IV_RENDER_NO_GEOMETRY)
		return SHOW_SOURCE;
	if (f & IV_RENDER_NO_STRAIGHTEN)
		return SHOW_TURNED;
	if (f & IV_RENDER_STEADY)
		return SHOW_STEADY;
	if (f & IV_RENDER_NO_CROP)
		return SHOW_FRAME;
	return SHOW_OUTPUT;
}

/* Where the FRAME sits in the steady canvas: centred, as the straightened
 * picture is. */
static void steady_origin(const IvGeometry *g, int sw, int sh, double *x,
			  double *y)
{
	int tw, th;
	double fw, fh;

	iv_geometry_turned_size(g, sw, sh, &tw, &th);
	iv_geometry_frame_size(g, sw, sh, &fw, &fh);
	*x = (tw - fw) / 2.0;
	*y = (th - fh) / 2.0;
}

void iv_render_frame_origin(const IvEditState *st, int sw, int sh,
			    IvRenderFlags flags, double *x, double *y)
{
	if (shown(flags) == SHOW_STEADY) {
		steady_origin(&st->geo, sw, sh, x, y);
		return;
	}
	*x = 0;
	*y = 0;
}

void iv_render_display_size(const IvEditState *st, int sw, int sh,
			    IvRenderFlags flags, int *dw, int *dh)
{
	double fw, fh;

	switch (shown(flags)) {
	case SHOW_SOURCE:
		*dw = sw;
		*dh = sh;
		break;
	case SHOW_TURNED:
	case SHOW_STEADY:
		iv_geometry_turned_size(&st->geo, sw, sh, dw, dh);
		break;
	case SHOW_FRAME:
		iv_geometry_frame_size(&st->geo, sw, sh, &fw, &fh);
		*dw = (int)fw;
		*dh = (int)fh;
		break;
	case SHOW_OUTPUT:
		iv_geometry_output_size(&st->geo, sw, sh, dw, dh);
		break;
	}
}

void iv_render_display_to_source(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double dx, double dy,
				 double *sx, double *sy)
{
	double tx, ty, ox, oy;
	const IvGeometry *g = &st->geo;

	switch (shown(flags)) {
	case SHOW_SOURCE:
		*sx = dx;
		*sy = dy;
		break;
	case SHOW_TURNED:
		iv_geometry_turned_to_source(g, sw, sh, dx, dy, sx, sy);
		break;
	case SHOW_STEADY:
		steady_origin(g, sw, sh, &ox, &oy);
		/* Now FRAME pixels. */
		dx -= ox;
		dy -= oy;
		G_GNUC_FALLTHROUGH;
	case SHOW_FRAME:
		iv_geometry_frame_to_turned(g, sw, sh, dx, dy, &tx, &ty);
		iv_geometry_turned_to_source(g, sw, sh, tx, ty, sx, sy);
		break;
	case SHOW_OUTPUT:
		iv_geometry_output_to_source(g, sw, sh, dx, dy, sx, sy);
		break;
	}
}

void iv_render_source_to_display(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double sx, double sy,
				 double *dx, double *dy)
{
	double tx, ty, ox, oy;
	const IvGeometry *g = &st->geo;

	switch (shown(flags)) {
	case SHOW_SOURCE:
		*dx = sx;
		*dy = sy;
		break;
	case SHOW_TURNED:
		iv_geometry_source_to_turned(g, sw, sh, sx, sy, dx, dy);
		break;
	case SHOW_FRAME:
		iv_geometry_source_to_turned(g, sw, sh, sx, sy, &tx, &ty);
		iv_geometry_turned_to_frame(g, sw, sh, tx, ty, dx, dy);
		break;
	case SHOW_STEADY:
		iv_geometry_source_to_turned(g, sw, sh, sx, sy, &tx, &ty);
		iv_geometry_turned_to_frame(g, sw, sh, tx, ty, dx, dy);
		steady_origin(g, sw, sh, &ox, &oy);
		*dx += ox;
		*dy += oy;
		break;
	case SHOW_OUTPUT:
		iv_geometry_source_to_output(g, sw, sh, sx, sy, dx, dy);
		break;
	}
}

void iv_render_display_to_output(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double dx, double dy,
				 double *ox, double *oy)
{
	double fx, fy, sx, sy, x0, y0;
	const IvGeometry *g = &st->geo;

	switch (shown(flags)) {
	case SHOW_OUTPUT:
		*ox = dx;
		*oy = dy;
		break;
	case SHOW_FRAME:
		iv_geometry_frame_to_output(g, sw, sh, dx, dy, ox, oy);
		break;
	case SHOW_STEADY:
		steady_origin(g, sw, sh, &x0, &y0);
		iv_geometry_frame_to_output(g, sw, sh, dx - x0, dy - y0, ox, oy);
		break;
	case SHOW_TURNED:
		iv_geometry_turned_to_frame(g, sw, sh, dx, dy, &fx, &fy);
		iv_geometry_frame_to_output(g, sw, sh, fx, fy, ox, oy);
		break;
	case SHOW_SOURCE:
		sx = dx;
		sy = dy;
		iv_geometry_source_to_output(g, sw, sh, sx, sy, ox, oy);
		break;
	}
}

void iv_render_output_to_display(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, double ox, double oy,
				 double *dx, double *dy)
{
	double fx, fy, x0, y0;
	const IvGeometry *g = &st->geo;

	switch (shown(flags)) {
	case SHOW_OUTPUT:
		*dx = ox;
		*dy = oy;
		break;
	case SHOW_FRAME:
		iv_geometry_output_to_frame(g, sw, sh, ox, oy, dx, dy);
		break;
	case SHOW_STEADY:
		iv_geometry_output_to_frame(g, sw, sh, ox, oy, dx, dy);
		steady_origin(g, sw, sh, &x0, &y0);
		*dx += x0;
		*dy += y0;
		break;
	case SHOW_TURNED:
		iv_geometry_output_to_frame(g, sw, sh, ox, oy, &fx, &fy);
		iv_geometry_frame_to_turned(g, sw, sh, fx, fy, dx, dy);
		break;
	case SHOW_SOURCE:
		iv_geometry_output_to_source(g, sw, sh, ox, oy, dx, dy);
		break;
	}
}

/* ---- the cache ------------------------------------------------------------------ */

typedef struct {
	int w, h;
	GPtrArray *masks;	/* IvMask*, the steps done */
	GPtrArray *results;	/* IvImage* after each step, or NULL */
} Chain;

typedef struct {
	IvMask *src;		/* held: the pointer stays unique */
	IvMask *scaled;
} Scaled;

struct IvRenderCache {
	GMutex lock;
	GPtrArray *chains;	/* Chain* */
	GPtrArray *scaled;	/* Scaled*, oldest first */
};

static void free_result(gpointer p)
{
	if (p)
		iv_image_free(p);
}

static void chain_free(gpointer p)
{
	Chain *c = p;

	g_ptr_array_unref(c->masks);
	g_ptr_array_unref(c->results);
	g_free(c);
}

static void scaled_free(gpointer p)
{
	Scaled *s = p;

	iv_mask_unref(s->src);
	iv_mask_unref(s->scaled);
	g_free(s);
}

IvRenderCache *iv_render_cache_new(void)
{
	IvRenderCache *c = g_new0(IvRenderCache, 1);

	g_mutex_init(&c->lock);
	c->chains = g_ptr_array_new_with_free_func(chain_free);
	c->scaled = g_ptr_array_new_with_free_func(scaled_free);
	return c;
}

void iv_render_cache_clear(IvRenderCache *c)
{
	if (!c)
		return;
	g_mutex_lock(&c->lock);
	/* The chains themselves stay: a render in another thread may be
	 * working along one of them right now (it finds it emptied and
	 * keeps nothing). */
	for (guint i = 0; i < c->chains->len; i++) {
		Chain *ch = g_ptr_array_index(c->chains, i);

		g_ptr_array_set_size(ch->masks, 0);
		g_ptr_array_set_size(ch->results, 0);
	}
	g_ptr_array_set_size(c->scaled, 0);
	g_mutex_unlock(&c->lock);
}

void iv_render_cache_free(IvRenderCache *c)
{
	if (!c)
		return;
	g_ptr_array_unref(c->chains);
	g_ptr_array_unref(c->scaled);
	g_mutex_clear(&c->lock);
	g_free(c);
}

/* The mask at w x h: the mask itself when it has that size, a scaled copy
 * (kept) otherwise.  The caller owns what is returned. */
static IvMask *mask_at(IvRenderCache *c, IvMask *m, int w, int h)
{
	IvMask *out = NULL;

	if (m->w == w && m->h == h)
		return iv_mask_ref(m);
	if (c) {
		g_mutex_lock(&c->lock);
		for (guint i = 0; i < c->scaled->len; i++) {
			Scaled *s = g_ptr_array_index(c->scaled, i);

			if (s->src == m && s->scaled->w == w && s->scaled->h == h) {
				out = iv_mask_ref(s->scaled);
				break;
			}
		}
		g_mutex_unlock(&c->lock);
		if (out)
			return out;
	}
	out = iv_mask_resized(m, w, h);
	if (out && c) {
		Scaled *s = g_new0(Scaled, 1);

		s->src = iv_mask_ref(m);
		s->scaled = iv_mask_ref(out);
		g_mutex_lock(&c->lock);
		g_ptr_array_add(c->scaled, s);
		while (c->scaled->len > IV_SCALED_KEEP)
			g_ptr_array_remove_index(c->scaled, 0);
		g_mutex_unlock(&c->lock);
	}
	return out;
}

static Chain *chain_for(IvRenderCache *c, int w, int h)
{
	Chain *ch;

	for (guint i = 0; i < c->chains->len; i++) {
		ch = g_ptr_array_index(c->chains, i);
		if (ch->w == w && ch->h == h)
			return ch;
	}
	ch = g_new0(Chain, 1);
	ch->w = w;
	ch->h = h;
	ch->masks = g_ptr_array_new_with_free_func((GDestroyNotify)iv_mask_unref);
	ch->results = g_ptr_array_new_with_free_func(free_result);
	g_ptr_array_add(c->chains, ch);
	return ch;
}

/* ---- progress, cancelling and failing --------------------------------------------- */

typedef struct {
	GCancellable *cancel;
	IvProgress progress;
	void *user;
	double base, span;	/* this step's share of the whole */
	const char *failed;	/* the step that ran out of memory */
} Prog;

static int prog_cb(double f, void *data)
{
	Prog *p = data;

	if (p->progress && p->progress(p->base + p->span * CLAMP(f, 0, 1),
				       p->user))
		return 1;
	return g_cancellable_is_cancelled(p->cancel) ? 1 : 0;
}

static gboolean cancelled(Prog *p)
{
	return g_cancellable_is_cancelled(p->cancel);
}

/* A step begins: the library's failures are counted from here. */
static void step_begin(void)
{
	iv_alloc_failures_reset();
}

/* A step is over: did everything in it get its memory?  When not, the
 * render is a failure under the step's name. */
static gboolean step_failed(Prog *p, const char *what)
{
	if (!iv_alloc_failures())
		return FALSE;
	p->failed = what;
	return TRUE;
}

/* ---- the steps ------------------------------------------------------------------- */

/* The source with the retouch steps applied: from the longest run of
 * steps already done, the rest inpainted now.  NULL when cancelled or out
 * of memory (p->failed is then set). */
static IvImage *retouch(const IvImage *src, GPtrArray *steps,
			IvRenderCache *c, Prog *p)
{
	guint n = steps ? steps->len : 0, start = 0;
	IvImage *img = NULL;
	Chain *ch = NULL;

	step_begin();
	if (n == 0) {
		img = iv_image_copy(src);
		if (!img)
			p->failed = "Your edits";
		return img;
	}
	if (c) {
		guint same = 0;

		g_mutex_lock(&c->lock);
		ch = chain_for(c, src->w, src->h);
		while (same < n && same < ch->masks->len &&
		       g_ptr_array_index(ch->masks, same) ==
			       g_ptr_array_index(steps, same))
			same++;
		/* Steps past the common run belong to another history. */
		g_ptr_array_set_size(ch->masks, same);
		g_ptr_array_set_size(ch->results, same);
		start = same;
		while (start > 0 && !g_ptr_array_index(ch->results, start - 1))
			start--;
		if (start > 0)
			img = iv_image_copy(g_ptr_array_index(ch->results, start - 1));
		g_mutex_unlock(&c->lock);
		if (start > 0 && !img) {
			p->failed = "Retouching";
			return NULL;
		}
	}
	if (!img)
		img = iv_image_copy(src);
	if (!img) {
		p->failed = "Retouching";
		return NULL;
	}
	for (guint i = start; i < n; i++) {
		IvMask *m = mask_at(c, g_ptr_array_index(steps, i), img->w, img->h);
		int r;

		if (!m) {
			/* No room for the mask at this size: the spot cannot
			 * be retouched, and leaving it out would be a
			 * different picture. */
			iv_image_free(img);
			p->failed = "Retouching";
			return NULL;
		}
		p->base = 0.9 * (i - start) / (double)(n - start);
		p->span = 0.9 / (double)(n - start);
		step_begin();
		r = iv_inpaint(img, m->data, prog_cb, p);
		iv_mask_unref(m);
		if (r == 1 || cancelled(p)) {
			iv_image_free(img);
			return NULL;
		}
		if (r < 0 || step_failed(p, "Retouching")) {
			p->failed = "Retouching";
			iv_image_free(img);
			return NULL;
		}
		if (c) {
			IvImage *keep = iv_image_copy(img);

			g_mutex_lock(&c->lock);
			/* Still the same history up to here (another thread
			 * may have rendered a different one meanwhile).  A
			 * result that could not be copied is simply not
			 * kept: it can always be made again. */
			if (keep && ch->masks->len == i) {
				g_ptr_array_add(ch->masks,
						iv_mask_ref(g_ptr_array_index(steps, i)));
				g_ptr_array_add(ch->results, keep);
				keep = NULL;
				/* Let the older results in the middle go. */
				guint kept = 0;

				for (guint k = ch->results->len; k > 0; k--)
					if (g_ptr_array_index(ch->results, k - 1) &&
					    ++kept > IV_CHAIN_KEEP && (k % 2)) {
						iv_image_free(g_ptr_array_index(ch->results,
										k - 1));
						g_ptr_array_index(ch->results, k - 1) =
							NULL;
					}
			}
			g_mutex_unlock(&c->lock);
			if (keep)
				iv_image_free(keep);
		}
	}
	return img;
}

/* The result of a step that makes a new picture: `next' in place of
 * `old', or, when there is none, the render fails under `what'. */
static IvImage *replace(IvImage *old, IvImage *next, Prog *p, const char *what)
{
	iv_image_free(old);
	if (!next)
		p->failed = what;
	return next;
}

static void rgba_of(guint32 c, double *r, double *g, double *b, double *a)
{
	*r = ((c >> 24) & 0xff) / 255.0;
	*g = ((c >> 16) & 0xff) / 255.0;
	*b = ((c >> 8) & 0xff) / 255.0;
	*a = (c & 0xff) / 255.0;
}

/* The points of a stroke as a smooth path: a Catmull-Rom spline through
 * them, as cubic Bezier pieces.  (The geometry maps strokes by turning,
 * mirroring and scaling only, and a spline is the same curve whichever
 * frame it is made in, so making it in SOURCE space is exact.) */
static void stroke_path(cairo_t *cr, const IvStroke *s, double ow, double oh)
{
	int n = s->n;
	const double *q = s->pts;

	if (n <= 0)
		return;
#define PX(i) (q[2 * (i)] * ow)
#define PY(i) (q[2 * (i) + 1] * oh)
	cairo_move_to(cr, PX(0), PY(0));
	if (n == 1) {
		cairo_line_to(cr, PX(0), PY(0));
		return;
	}
	for (int i = 0; i < n - 1; i++) {
		int i0 = MAX(i - 1, 0), i3 = MIN(i + 2, n - 1);
		double c1x = PX(i) + (PX(i + 1) - PX(i0)) / 6;
		double c1y = PY(i) + (PY(i + 1) - PY(i0)) / 6;
		double c2x = PX(i + 1) - (PX(i3) - PX(i)) / 6;
		double c2y = PY(i + 1) - (PY(i3) - PY(i)) / 6;

		cairo_curve_to(cr, c1x, c1y, c2x, c2y, PX(i + 1), PY(i + 1));
	}
#undef PX
#undef PY
}

/* A broad nib held at the stroke's angle: the mark is everything the
 * nib's edge passes over, so the line is thick across the nib and thin
 * along it. */
static void calligraphy(cairo_t *cr, const IvStroke *s, double ow, double oh,
			double lw)
{
	cairo_path_t *flat;
	double nx = cos(s->angle) * lw / 2, ny = sin(s->angle) * lw / 2;
	double px = 0, py = 0;
	gboolean have = FALSE;

	cairo_new_path(cr);
	stroke_path(cr, s, ow, oh);
	flat = cairo_copy_path_flat(cr);
	cairo_new_path(cr);
	for (int i = 0; i < flat->num_data; i += flat->data[i].header.length) {
		cairo_path_data_t *d = &flat->data[i];
		double x, y;

		if (d->header.type != CAIRO_PATH_MOVE_TO &&
		    d->header.type != CAIRO_PATH_LINE_TO)
			continue;
		x = d[1].point.x;
		y = d[1].point.y;
		if (have && d->header.type == CAIRO_PATH_LINE_TO) {
			cairo_move_to(cr, px - nx, py - ny);
			cairo_line_to(cr, px + nx, py + ny);
			cairo_line_to(cr, x + nx, y + ny);
			cairo_line_to(cr, x - nx, y - ny);
			cairo_close_path(cr);
		}
		px = x;
		py = y;
		have = TRUE;
	}
	cairo_path_destroy(flat);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	cairo_fill(cr);
	/* A hairline along the path, so the thin parts never vanish. */
	cairo_set_line_width(cr, MAX(1, lw * 0.14));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	stroke_path(cr, s, ow, oh);
	cairo_stroke(cr);
}

/* How the marker lays its colour down: first multiplied with the picture
 * (dark lines under it stay dark and light areas take its colour, as ink
 * on paper), then screened over it.  Screening lifts dark pixels towards
 * the marker's colour and leaves light ones almost as they are, so the
 * band shows on the dark parts of a picture, where a multiply alone would
 * leave nothing to see, without washing out the light parts. */
#define MARKER_LIFT 0.9

void iv_render_draw_stroke(cairo_t *cr, const IvStroke *s, double w,
			   double h)
{
	double r, g, b, a, lw;
	cairo_pattern_t *ink;

	if (!s || s->n == 0)
		return;
	lw = MAX(0.5, s->width * MAX(w, h));
	rgba_of(s->rgba, &r, &g, &b, &a);
	cairo_save(cr);
	cairo_new_path(cr);
	/* Each stroke as one group, so where it crosses itself the colour
	 * does not build up (the marker especially). */
	cairo_push_group(cr);
	cairo_set_source_rgb(cr, r, g, b);
	switch (s->kind) {
	case IV_STROKE_MARKER:
		cairo_set_line_width(cr, lw);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		stroke_path(cr, s, w, h);
		cairo_stroke(cr);
		break;
	case IV_STROKE_CALLIGRAPHY:
		calligraphy(cr, s, w, h, lw);
		break;
	case IV_STROKE_RECT:
		/* Filled, without an outline: it covers exactly what was
		 * drawn, which is what a box laid over text is for. */
		for (int i = 0; i < s->n; i++)
			cairo_line_to(cr, s->pts[2 * i] * w, s->pts[2 * i + 1] * h);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case IV_STROKE_PEN:
	default:
		cairo_set_line_width(cr, lw);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		stroke_path(cr, s, w, h);
		cairo_stroke(cr);
		break;
	}
	ink = cairo_pop_group(cr);
	cairo_set_source(cr, ink);
	if (s->kind == IV_STROKE_MARKER) {
		cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
		cairo_paint_with_alpha(cr, a);
		cairo_set_operator(cr, CAIRO_OPERATOR_SCREEN);
		cairo_paint_with_alpha(cr, a * MARKER_LIFT);
	} else {
		cairo_paint_with_alpha(cr, a);
	}
	cairo_pattern_destroy(ink);
	cairo_restore(cr);
}

void iv_render_draw_strokes(cairo_t *cr, GPtrArray *strokes, double w,
			    double h)
{
	for (guint i = 0; strokes && i < strokes->len; i++)
		iv_render_draw_stroke(cr, g_ptr_array_index(strokes, i), w, h);
}

gboolean iv_render_source_matrix(const IvEditState *st, int sw, int sh,
				 IvRenderFlags flags, cairo_matrix_t *m)
{
	double ax, ay, bx, by, cx, cy;

	if (sw <= 0 || sh <= 0) {
		cairo_matrix_init_identity(m);
		return FALSE;
	}
	/* Every geometry step is affine: three points settle the map. */
	iv_render_source_to_display(st, sw, sh, flags, 0, 0, &ax, &ay);
	iv_render_source_to_display(st, sw, sh, flags, sw, 0, &bx, &by);
	iv_render_source_to_display(st, sw, sh, flags, 0, sh, &cx, &cy);
	cairo_matrix_init(m, (bx - ax) / sw, (by - ay) / sw, (cx - ax) / sh,
			  (cy - ay) / sh, ax, ay);
	return TRUE;
}

/* The strokes on the picture: through a cairo surface and back.  `m' maps
 * SOURCE pixels to the picture's pixels.  NULL when out of memory. */
static IvImage *draw_on(IvImage *img, GPtrArray *strokes,
			const cairo_matrix_t *m, int sw, int sh)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
							img->w, img->h);
	cairo_t *cr;
	IvImage *out;

	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		return NULL;
	}
	cairo_surface_flush(s);
	iv_image_to_cairo(img, cairo_image_surface_get_data(s),
			  cairo_image_surface_get_stride(s));
	cairo_surface_mark_dirty(s);
	cr = cairo_create(s);
	cairo_set_matrix(cr, m);
	iv_render_draw_strokes(cr, strokes, sw, sh);
	if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
		cairo_destroy(cr);
		cairo_surface_destroy(s);
		return NULL;
	}
	cairo_destroy(cr);
	cairo_surface_flush(s);
	out = iv_image_from_cairo(cairo_image_surface_get_data(s), img->w,
				  img->h, cairo_image_surface_get_stride(s));
	cairo_surface_destroy(s);
	return out;
}

/* The straightened picture on a canvas the size of `img' (the TURNED
 * frame at this render's scale), centred: IV_RENDER_STEADY. */
static IvImage *straighten_steady(const IvImage *img, double degrees)
{
	IvImage *whole = iv_rotate_free(img, degrees, 0), *out;
	int x, y;

	if (!whole)
		return NULL;
	x = (whole->w - img->w) / 2;
	y = (whole->h - img->h) / 2;
	out = iv_crop(whole, MAX(0, x), MAX(0, y), MIN(img->w, whole->w),
		      MIN(img->h, whole->h));
	iv_image_free(whole);
	return out;
}

IvImage *iv_render(const IvImage *src, int sw, int sh, const IvEditState *st,
		   IvRenderFlags flags, IvRenderCache *cache, GCancellable *cancel,
		   IvProgress progress, void *progress_user)
{
	return iv_render_ex(src, sw, sh, st, flags, cache, cancel, progress,
			    progress_user, NULL);
}

IvImage *iv_render_ex(const IvImage *src, int sw, int sh,
		      const IvEditState *st, IvRenderFlags flags,
		      IvRenderCache *cache, GCancellable *cancel,
		      IvProgress progress, void *progress_user,
		      IvRenderOutcome *out)
{
	Prog p = { cancel, progress, progress_user, 0, 1, NULL };
	double k;
	IvImage *img;
	const IvGeometry *g = &st->geo;

	if (out) {
		out->cancelled = FALSE;
		out->failed = NULL;
	}
	if (!src || sw <= 0 || sh <= 0)
		return NULL;
	flags = normalise(flags);
	k = (double)src->w / sw;

	/* 1. retouch */
	img = retouch(src, st->retouch, cache, &p);
	if (!img || cancelled(&p))
		goto stop;

	/* 2. colour */
	if (!(flags & IV_RENDER_NO_COLOR)) {
		if (!iv_adjust_is_neutral(&st->adjust)) {
			step_begin();
			iv_adjust_apply_ex(img, &st->adjust, k);
			if (step_failed(&p, "The adjustments"))
				goto stop;
		}
		if (cancelled(&p))
			goto stop;
		if (st->style != IV_STYLE_NONE && st->style_intensity > 0) {
			step_begin();
			iv_style_apply(img, st->style, CLAMP(st->style_intensity, 0, 1));
			if (step_failed(&p, "The style"))
				goto stop;
		}
		if (cancelled(&p))
			goto stop;
	}

	/* 3. background */
	if (!(flags & IV_RENDER_NO_BACKGROUND) && st->bg.active && st->bg.mask) {
		IvMask *m = mask_at(cache, st->bg.mask, img->w, img->h);

		if (!m) {
			p.failed = "The background change";
			goto stop;
		}
		step_begin();
		iv_bg_apply(img, m->data, st->bg.op, st->bg.strength, st->bg.rgba);
		iv_mask_unref(m);
		if (step_failed(&p, "The background change") || cancelled(&p))
			goto stop;
	}
	if (progress)
		progress(0.95, progress_user);

	/* 4. geometry */
	if (!(flags & IV_RENDER_NO_GEOMETRY)) {
		int q = ((g->quarter_turns % 4) + 4) % 4;

		if (q) {
			img = replace(img, iv_rotate90(img, q), &p, "The turn");
			if (!img)
				goto stop;
		}
		step_begin();
		if (g->flip_h)
			iv_flip(img, 1);
		if (g->flip_v)
			iv_flip(img, 0);
		if (step_failed(&p, "The mirror"))
			goto stop;
		if (!(flags & IV_RENDER_NO_STRAIGHTEN) && fabs(g->straighten) > 1e-6) {
			if (flags & IV_RENDER_STEADY)
				img = replace(img, straighten_steady(img, g->straighten),
					      &p, "Straightening");
			else
				img = replace(img, iv_rotate_free(img, g->straighten, 2),
					      &p, "Straightening");
			if (!img)
				goto stop;
		}
		if (cancelled(&p))
			goto stop;
		if (!(flags & IV_RENDER_NO_CROP) && iv_geometry_has_crop(g)) {
			int x = (int)lround(g->crop_x * img->w);
			int y = (int)lround(g->crop_y * img->h);
			int w = (int)lround(g->crop_w * img->w);
			int h = (int)lround(g->crop_h * img->h);

			x = CLAMP(x, 0, img->w - 1);
			y = CLAMP(y, 0, img->h - 1);
			w = CLAMP(w, 1, img->w - x);
			h = CLAMP(h, 1, img->h - y);
			img = replace(img, iv_crop(img, x, y, w, h), &p, "The crop");
			if (!img)
				goto stop;
		}
	}

	/* 5. drawing: the strokes mapped from SOURCE onto what is shown */
	if (!(flags & IV_RENDER_NO_DRAW) && st->strokes->len) {
		cairo_matrix_t m, scale;
		int dw, dh;

		iv_render_display_size(st, sw, sh, flags, &dw, &dh);
		if (iv_render_source_matrix(st, sw, sh, flags, &m) && dw > 0 &&
		    dh > 0) {
			cairo_matrix_init_scale(&scale, (double)img->w / dw,
						(double)img->h / dh);
			cairo_matrix_multiply(&m, &m, &scale);
			img = replace(img, draw_on(img, st->strokes, &m, sw, sh), &p,
				      "The drawing");
			if (!img)
				goto stop;
		}
	}
	if (progress)
		progress(1.0, progress_user);
	return img;

stop:
	if (img)
		iv_image_free(img);
	if (out) {
		out->cancelled = cancelled(&p);
		out->failed = out->cancelled ? NULL : p.failed;
		/* Nothing came out and nobody cancelled: it can only have
		 * been memory. */
		if (!out->cancelled && !out->failed)
			out->failed = "Your edits";
	}
	return NULL;
}

IvImage *iv_render_make_proxy(const IvImage *src)
{
	double k;

	if (!src)
		return NULL;
	if (MAX(src->w, src->h) <= IV_PROXY_EDGE)
		return iv_image_copy(src);
	k = (double)IV_PROXY_EDGE / MAX(src->w, src->h);
	return iv_image_scale(src, MAX(1, (int)lround(src->w * k)),
			      MAX(1, (int)lround(src->h * k)), 1);
}
