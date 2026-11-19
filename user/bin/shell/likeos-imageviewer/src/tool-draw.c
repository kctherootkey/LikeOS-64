// LikeOS -- LikeOS Image Viewer: the Draw tool.
//
// Strokes drawn on top of the picture with five tools:
//
//   Pen          a solid round line
//   Marker       a broad translucent highlighter with flat ends: it
//                multiplies with the picture like ink on paper and is
//                screened over it as well, which lifts dark parts towards
//                its colour, so it reads on light and dark pictures alike
//   Calligraphy  a broad nib held at an angle: the line is wide where it
//                runs across the nib and thin where it runs along it
//   Rectangle    a filled box, dragged out from corner to corner, for
//                covering what must not be seen (a name, a number in a
//                screenshot) or simply marking an area; a rectangle is
//                chosen by clicking it, moved by dragging it, resized by its
//                eight handles, recoloured by a swatch and removed by Delete
//   Eraser       takes away every stroke its path touches, as a whole
//
// A stroke is a list of points in SOURCE coordinates (normalised 0..1), a
// colour and a width (a fraction of the SOURCE's long edge), smoothed into
// a Catmull-Rom spline when it is drawn (iv_render_draw_stroke, the same
// painter the pipeline uses when saving).  Being on the source, a stroke
// stays on what it was drawn on: a crop, a turn or straightening made
// afterwards moves it with the picture instead of stretching it over the
// new frame.  While this tool is current the preview render leaves the
// strokes out (IV_RENDER_NO_DRAW) and the tool draws all of them itself
// over the picture, as vectors at the canvas's own resolution: a stroke
// being drawn costs no render at all, the eraser shows its effect at once,
// and what is seen is exactly what is saved.  Every stroke, every erasing
// drag and Clear all are one undo step each.
//
// Each drawing tool remembers its own colour and size, also from one run
// to the next (settings.c, group [Draw]).  The panel shows
// the four tools as tiles, the colours, the size with a sample stroke at
// the scale the picture is shown at, and the drawing's stroke count with
// Clear all.  Keys: P, M, K, X, E pick the tools ([E] goes back to the
// last pen from the eraser), [ and ] change the size, Shift while drawing
// makes the stroke a straight line and a new rectangle a square.
//
// A rectangle is four corners on the SOURCE like any stroke's points
// (IV_STROKE_RECT), laid out along the screen's axes when it is drawn.  It
// is handled as a box -- a centre, two unit axes and two half sizes in
// SOURCE pixels -- so moving and resizing work along its own edges however
// the picture has been turned or straightened since.  Every new, moved,
// resized, recoloured or deleted rectangle is one undo step.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "tool-brush.h"

typedef enum {
	DRAW_PEN = 0,
	DRAW_MARKER,
	DRAW_CALLIGRAPHY,
	DRAW_RECT,
	DRAW_ERASER,
	DRAW_N
} DrawTool;

typedef struct {
	const char *label;
	const char *tooltip;
	IvGlyph glyph;
	guint key;
	IvStrokeKind kind;
	guint8 alpha;		/* the tool's own opacity, 255 = opaque */
	guint32 rgba;		/* the colour picked for it (opaque) */
	double size;		/* line width in pixels per 1000 of the long edge */
	const char *setting;	/* its values in the settings: "Pen" */
} DrawOpts;

/* The sizes are relative to the picture (pixels per 1000 pixels of its long
 * edge) so that a tool draws the same on a small and on a large picture;
 * the panel shows what they come to in the picture's own pixels. */
#define WIDTH_MIN 1.0
#define WIDTH_MAX 80.0

static DrawOpts opts[DRAW_N] = {
	[DRAW_PEN] = { "Pen", "Pen (P)", IV_GLYPH_PEN, GDK_KEY_p,
		       IV_STROKE_PEN, 255, 0xd63a3aff, 4, "Pen" },
	[DRAW_MARKER] = { "Marker", "Marker (M)", IV_GLYPH_MARKER, GDK_KEY_m,
			  IV_STROKE_MARKER, 170, 0xf2c230ff, 24, "Marker" },
	[DRAW_CALLIGRAPHY] = { "Calligraphy", "Calligraphy pen (K)",
			       IV_GLYPH_CALLIGRAPHY, GDK_KEY_k,
			       IV_STROKE_CALLIGRAPHY, 255, 0x3b74f0ff, 10,
			       "Calligraphy" },
	/* Ink by default: the colour a box over text is usually wanted in.
	 * It has no width. */
	[DRAW_RECT] = { "Rectangle", "Filled rectangle (X)", IV_GLYPH_RECT,
			GDK_KEY_x, IV_STROKE_RECT, 255, 0x141a24ff, 0,
			"Rectangle" },
	[DRAW_ERASER] = { "Eraser", "Eraser: removes whole strokes (E)",
			  IV_GLYPH_ERASER, GDK_KEY_e, IV_STROKE_PEN, 0, 0, 24,
			  "Eraser" },
};

#define SETTINGS_GROUP "Draw"


/* The colours offered: ink and paper, and six of the LikeOS palette's
 * signal colours, bright enough for marks on pictures. */
static const guint32 palette[] = {
	0xffffffff, /* white */
	0x141a24ff, /* ink */
	0xd63a3aff, /* red */
	0xe8743bff, /* orange */
	0xf2c230ff, /* yellow */
	0x1f9d55ff, /* green */
	0x3b74f0ff, /* blue */
	0x7a4fd6ff, /* violet */
};

/* The calligraphy nib's angle on the screen: rising at 45 degrees.  Each
 * stroke keeps it in SOURCE space (IvStroke.angle), so it is the same on
 * the screen however the picture is turned when the stroke is drawn. */
#define NIB_ANGLE (-G_PI / 4)

/* ---- the tool's state --------------------------------------------------------- */

static IvEditor *editor;
static DrawTool current = DRAW_PEN;
static DrawTool last_pen = DRAW_PEN;	/* where E goes back to */

/* The stroke being drawn (not yet in any state). */
static IvStroke *drawing;
static gboolean straight;		/* Shift: start and end only */
static double smooth_x, smooth_y;	/* the stabilised pointer, SOURCE px */
static double last_wx, last_wy;		/* the last point taken, widget px */

/* Erasing: the live edit is open, the path so far in SOURCE px. */
static gboolean erasing;
static double erase_x, erase_y;
static int erased;

/* The pointer, for the brush outline. */
static gboolean pointer_in;
static double pointer_wx, pointer_wy;

/* Rectangles as boxes: the centre, the unit vectors along the width and
 * the height (the screen's x and y when it was drawn) and half the width
 * and height, all in SOURCE pixels. */
typedef struct {
	double cx, cy;
	double ux, uy;
	double vx, vy;
	double hw, hh;
} Box;

typedef enum {
	BOX_IDLE = 0,
	BOX_NEW,		/* dragging out a new one */
	BOX_MOVE,		/* dragging the chosen one */
	BOX_RESIZE,		/* dragging one of its handles */
} BoxDrag;

/* The chosen rectangle (held; one of the committed state's strokes) and
 * the drag going on. */
static IvStroke *chosen;
static BoxDrag box_drag;
static int box_handle;			/* BOX_RESIZE: index in handles[] */
static gboolean box_moved;		/* past the drag threshold */
static Box box_start, box_now;		/* at the press, and as dragged */
static double press_sx, press_sy;	/* the press, SOURCE px */
static double press_wx, press_wy;	/* the press, widget px */

/* Flattened strokes for the eraser's hit test: IvStroke* (held) ->
 * GArray of double x, y in SOURCE px of the size they were made for. */
typedef struct {
	int sw, sh;
	GArray *pts;
} Flat;
static GHashTable *flats;

/* The panel. */
static GtkWidget *tiles[DRAW_N];
static GtkWidget *colour_box, *swatches, *size_row, *preview, *eraser_note;
static GtkWidget *size_head, *rect_note;
static GtkWidget *count_label, *clear_button;
static gboolean syncing;
static gboolean loaded;			/* opts read from the settings */

static void sync_panel(void);

/* ---- sizes and colours ---------------------------------------------------------- */

static double width_fraction(DrawTool t)
{
	return opts[t].size / 1000.0;
}

/* The width of tool `t' in SOURCE pixels. */
static double width_px(IvEditor *ed, DrawTool t)
{
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	return width_fraction(t) * MAX(sw, sh);
}

/* ---- remembered between runs ------------------------------------------------------ */

static void load_opts(void)
{
	char key[64];

	if (loaded)
		return;
	loaded = TRUE;
	for (int i = 0; i < DRAW_N; i++) {
		if (i != DRAW_ERASER) {
			g_snprintf(key, sizeof key, "%sColor", opts[i].setting);
			/* The tool's opacity is its own: the colour is kept
			 * opaque unless the chooser gave it an alpha. */
			opts[i].rgba = iv_settings_tool_rgba(SETTINGS_GROUP, key,
							     opts[i].rgba);
		}
		if (i == DRAW_RECT)
			continue;
		g_snprintf(key, sizeof key, "%sWidth", opts[i].setting);
		opts[i].size = iv_settings_tool_double(SETTINGS_GROUP, key,
						       opts[i].size, WIDTH_MIN,
						       WIDTH_MAX);
	}
}

static void store_opts(DrawTool t)
{
	char key[64];

	if (t != DRAW_ERASER) {
		g_snprintf(key, sizeof key, "%sColor", opts[t].setting);
		iv_settings_set_tool_rgba(SETTINGS_GROUP, key, opts[t].rgba);
	}
	if (t == DRAW_RECT)
		return;
	g_snprintf(key, sizeof key, "%sWidth", opts[t].setting);
	iv_settings_set_tool_double(SETTINGS_GROUP, key, opts[t].size);
}

static guint32 stroke_rgba(DrawTool t)
{
	guint32 c = opts[t].rgba;
	guint a = (c & 0xff) * opts[t].alpha / 255;

	return (c & 0xffffff00u) | (a & 0xff);
}

/* ---- the eraser's hit test ------------------------------------------------------------ */

static void flat_free(gpointer p)
{
	Flat *f = p;

	g_array_unref(f->pts);
	g_free(f);
}

static void flat_key_free(gpointer p)
{
	iv_stroke_unref(p);
}

/* The stroke's spline as a polyline, the same curve render.c draws: each
 * span between two points is a cubic Bezier piece with Catmull-Rom
 * control points, cut into pieces about two pixels long. */
static GArray *flatten(const IvStroke *s, double sw, double sh)
{
	GArray *a = g_array_new(FALSE, FALSE, sizeof(double));
	const double *q = s->pts;
	int n = s->n;

#define PX(i) (q[2 * (i)] * sw)
#define PY(i) (q[2 * (i) + 1] * sh)
	if (n <= 0)
		return a;
	g_array_append_val(a, (double){ PX(0) });
	g_array_append_val(a, (double){ PY(0) });
	for (int i = 0; i < n - 1; i++) {
		int i0 = MAX(i - 1, 0), i3 = MIN(i + 2, n - 1);
		double p0x = PX(i), p0y = PY(i);
		double p3x = PX(i + 1), p3y = PY(i + 1);
		double c1x = p0x + (p3x - PX(i0)) / 6;
		double c1y = p0y + (p3y - PY(i0)) / 6;
		double c2x = p3x - (PX(i3) - p0x) / 6;
		double c2y = p3y - (PY(i3) - p0y) / 6;
		/* The control polygon is never shorter than the curve. */
		double len = hypot(c1x - p0x, c1y - p0y) +
			     hypot(c2x - c1x, c2y - c1y) +
			     hypot(p3x - c2x, p3y - c2y);
		int steps = CLAMP((int)ceil(len / 2.0), 1, 64);

		for (int k = 1; k <= steps; k++) {
			double t = (double)k / steps, u = 1 - t;
			double x = u * u * u * p0x + 3 * u * u * t * c1x +
				   3 * u * t * t * c2x + t * t * t * p3x;
			double y = u * u * u * p0y + 3 * u * u * t * c1y +
				   3 * u * t * t * c2y + t * t * t * p3y;

			g_array_append_val(a, x);
			g_array_append_val(a, y);
		}
	}
#undef PX
#undef PY
	return a;
}

static const Flat *flat_for(IvStroke *s, int sw, int sh)
{
	Flat *f;

	if (!flats)
		flats = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					      flat_key_free, flat_free);
	f = g_hash_table_lookup(flats, s);
	if (f && f->sw == sw && f->sh == sh)
		return f;
	f = g_new0(Flat, 1);
	f->sw = sw;
	f->sh = sh;
	f->pts = flatten(s, sw, sh);
	/* The table holds the stroke, so its address is never reused for
	 * another stroke while the entry exists. */
	g_hash_table_replace(flats, iv_stroke_ref(s), f);
	return f;
}

static double point_segment_dist2(double px, double py, double ax, double ay,
				  double bx, double by)
{
	double dx = bx - ax, dy = by - ay;
	double l2 = dx * dx + dy * dy;
	double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;

	t = CLAMP(t, 0, 1);
	dx = ax + t * dx - px;
	dy = ay + t * dy - py;
	return dx * dx + dy * dy;
}

/* Which side of a -> b point p lies on (twice the triangle's area). */
static double orient(double ax, double ay, double bx, double by, double px,
		     double py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* The squared distance between segments a-b and c-d: zero when they
 * cross, else the least of the four point-to-segment distances. */
static double segment_dist2(double ax, double ay, double bx, double by,
			    double cx, double cy, double dx, double dy)
{
	double o1 = orient(ax, ay, bx, by, cx, cy);
	double o2 = orient(ax, ay, bx, by, dx, dy);
	double o3 = orient(cx, cy, dx, dy, ax, ay);
	double o4 = orient(cx, cy, dx, dy, bx, by);
	double d;

	if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) &&
	    ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0)))
		return 0;
	d = point_segment_dist2(ax, ay, cx, cy, dx, dy);
	d = MIN(d, point_segment_dist2(bx, by, cx, cy, dx, dy));
	d = MIN(d, point_segment_dist2(cx, cy, ax, ay, bx, by));
	d = MIN(d, point_segment_dist2(dx, dy, ax, ay, bx, by));
	return d;
}

static void box_from_stroke(const IvStroke *s, int sw, int sh, Box *b);
static gboolean box_contains(const Box *b, double x, double y, double margin);
static void box_point(const Box *b, double su, double sv, double *x,
		      double *y);

/* Does the eraser's move from (x0, y0) to (x1, y1) with radius r touch
 * rectangle `s'?  Either end inside it (grown by r), or the move passing
 * within r of one of its sides. */
static gboolean rect_hit(const IvStroke *s, int sw, int sh, double x0,
			 double y0, double x1, double y1, double r)
{
	static const double corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 },
					     { -1, 1 } };
	Box b;

	box_from_stroke(s, sw, sh, &b);
	if (box_contains(&b, x0, y0, r) || box_contains(&b, x1, y1, r))
		return TRUE;
	for (int i = 0; i < 4; i++) {
		double ax, ay, bx, by;

		box_point(&b, corner[i][0], corner[i][1], &ax, &ay);
		box_point(&b, corner[(i + 1) % 4][0], corner[(i + 1) % 4][1], &bx,
			  &by);
		if (segment_dist2(x0, y0, x1, y1, ax, ay, bx, by) <= r * r)
			return TRUE;
	}
	return FALSE;
}

/* Does the eraser's move from (x0, y0) to (x1, y1) with radius r (SOURCE
 * px) touch stroke `s'?  The stroke counts with its whole width: for the
 * calligraphy pen that is the nib's full breadth.  Strokes and eraser are
 * both on the SOURCE, so the test holds whatever the geometry is. */
static gboolean stroke_hit(IvStroke *s, int sw, int sh, double x0, double y0,
			   double x1, double y1, double r)
{
	double bx0, by0, bx1, by1, reach, reach2;
	double long_edge = MAX(sw, sh);
	const Flat *f;
	const double *p;
	guint n;

	if (s->n <= 0 || sw <= 0 || sh <= 0)
		return FALSE;
	reach = r + MAX(0.5, s->width * long_edge) / 2;
	/* The bounding box first: most strokes are nowhere near. */
	iv_stroke_bounds(s, (double)sw / sh, &bx0, &by0, &bx1, &by1);
	if (MAX(x0, x1) < bx0 * sw - r || MIN(x0, x1) > bx1 * sw + r ||
	    MAX(y0, y1) < by0 * sh - r || MIN(y0, y1) > by1 * sh + r)
		return FALSE;
	if (s->kind == IV_STROKE_RECT)
		return rect_hit(s, sw, sh, x0, y0, x1, y1, r);
	f = flat_for(s, sw, sh);
	p = (const double *)(gpointer)f->pts->data;
	n = f->pts->len / 2;
	reach2 = reach * reach;
	if (n == 1)
		return point_segment_dist2(p[0], p[1], x0, y0, x1, y1) <= reach2;
	for (guint i = 0; i + 1 < n; i++)
		if (segment_dist2(x0, y0, x1, y1, p[2 * i], p[2 * i + 1],
				  p[2 * i + 2], p[2 * i + 3]) <= reach2)
			return TRUE;
	return FALSE;
}

/* Erases along (x0, y0) -> (x1, y1) in the live edit. */
static void erase_along(IvEditor *ed, double x0, double y0, double x1,
			double y1)
{
	IvEditState *s;
	int sw, sh;
	double r = width_px(ed, DRAW_ERASER) / 2;

	iv_editor_source_size(ed, &sw, &sh);
	s = iv_editor_begin(ed);
	for (guint i = s->strokes->len; i > 0; i--) {
		IvStroke *st = g_ptr_array_index(s->strokes, i - 1);

		if (stroke_hit(st, sw, sh, x0, y0, x1, y1, r)) {
			iv_edit_state_remove_stroke(s, i - 1);
			erased++;
		}
	}
}

/* ---- drawing a stroke ------------------------------------------------------------- */

static void to_source(IvEditor *ed, double dx, double dy, double *sx,
		      double *sy)
{
	iv_editor_display_to_source(ed, dx, dy, sx, sy);
}

static void stroke_add(IvEditor *ed, double sx, double sy)
{
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	if (sw <= 0 || sh <= 0)
		return;
	iv_stroke_add_point(drawing, sx / sw, sy / sh);
}

/* A straight stroke: its first point and (sx, sy). */
static void stroke_straight(IvEditor *ed, double sx, double sy)
{
	drawing->n = MIN(drawing->n, 1);
	stroke_add(ed, sx, sy);
}

/* The nib's angle in SOURCE space that shows as NIB_ANGLE on the screen:
 * the screen direction taken back through the turn and mirror of the
 * geometry shown (the inverse of the source matrix's linear part). */
static double nib_angle_in_source(IvEditor *ed)
{
	cairo_matrix_t m;
	double dx = cos(NIB_ANGLE), dy = sin(NIB_ANGLE);

	if (!iv_brush_source_matrix(ed, &m) ||
	    cairo_matrix_invert(&m) != CAIRO_STATUS_SUCCESS)
		return NIB_ANGLE;
	cairo_matrix_transform_distance(&m, &dx, &dy);
	return atan2(dy, dx);
}

static void stroke_begin(IvEditor *ed, double dx, double dy, double wx,
			 double wy)
{
	double ox, oy;

	if (drawing)
		iv_stroke_unref(drawing);
	drawing = iv_stroke_new(opts[current].kind, stroke_rgba(current),
				width_fraction(current));
	drawing->angle = nib_angle_in_source(ed);
	straight = FALSE;
	to_source(ed, dx, dy, &ox, &oy);
	smooth_x = ox;
	smooth_y = oy;
	last_wx = wx;
	last_wy = wy;
	stroke_add(ed, ox, oy);
}

static void stroke_motion(IvEditor *ed, double dx, double dy, double wx,
			  double wy, gboolean shift)
{
	double ox, oy;

	to_source(ed, dx, dy, &ox, &oy);
	if (shift || straight) {
		/* Once straight, the stroke stays straight: taking Shift
		 * off half way should not turn a ruler line into a
		 * scribble. */
		straight = TRUE;
		stroke_straight(ed, ox, oy);
		return;
	}
	/* Points closer than a pixel and a half on the screen only add
	 * jitter to the spline. */
	if (hypot(wx - last_wx, wy - last_wy) < 1.5)
		return;
	last_wx = wx;
	last_wy = wy;
	/* A light stabiliser: each point moves two thirds of the way from
	 * the previous one to the pointer, which takes the shake out of a
	 * mouse hand without a noticeable lag (the true end point is added
	 * when the button goes up). */
	smooth_x += (ox - smooth_x) * 0.67;
	smooth_y += (oy - smooth_y) * 0.67;
	stroke_add(ed, smooth_x, smooth_y);
}

static void stroke_end(IvEditor *ed, double dx, double dy)
{
	double sx, sy;
	IvStroke *s = drawing;
	int sw, sh;

	drawing = NULL;
	to_source(ed, dx, dy, &sx, &sy);
	/* The stroke ends where the pointer is, not where the stabiliser
	 * had got to; a straight one is its start and this point. */
	if (straight)
		s->n = MIN(s->n, 1);
	iv_editor_source_size(ed, &sw, &sh);
	if (sw > 0 && sh > 0)
		iv_stroke_add_point(s, sx / sw, sy / sh);
	iv_edit_state_add_stroke(iv_editor_begin(ed), s);
	iv_stroke_unref(s);
	/* The render leaves the strokes out while this tool is current, so
	 * the new step needs no render, only the undo history. */
	iv_editor_commit(ed, opts[current].label);
	sync_panel();
	iv_editor_redraw_overlay(ed);
}

/* ---- rectangles ------------------------------------------------------------------- */

/* The handles of the chosen rectangle, as the sides they move: -1, 0 or 1
 * along its width and its height.  The corners first, so that where a
 * corner and a side's handle overlap on a small box the corner wins. */
static const struct {
	int su, sv;
} handles[8] = {
	{ -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 },
	{ 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 },
};

#define HANDLE_HALF 4.5		/* half a handle's side, widget px */
#define HANDLE_REACH 9.0	/* how near a press must be, widget px */
#define BOX_MIN_SCREEN 3.0	/* a new box smaller than this is a click */

static void box_point(const Box *b, double su, double sv, double *x, double *y)
{
	*x = b->cx + su * b->hw * b->ux + sv * b->hh * b->vx;
	*y = b->cy + su * b->hw * b->uy + sv * b->hh * b->vy;
}

/* (x, y) in the box's own frame: along its width and along its height. */
static void box_local(const Box *b, double x, double y, double *lu,
		      double *lv)
{
	*lu = (x - b->cx) * b->ux + (y - b->cy) * b->uy;
	*lv = (x - b->cx) * b->vx + (y - b->cy) * b->vy;
}

static gboolean box_contains(const Box *b, double x, double y, double margin)
{
	double lu, lv;

	box_local(b, x, y, &lu, &lv);
	return fabs(lu) <= b->hw + margin && fabs(lv) <= b->hh + margin;
}

static gboolean unit(double *x, double *y)
{
	double l = hypot(*x, *y);

	if (l < 1e-12)
		return FALSE;
	*x /= l;
	*y /= l;
	return TRUE;
}

/* The box of a rectangle stroke: its corners in SOURCE px give the centre,
 * the first side the width's direction, the last the height's. */
static void box_from_stroke(const IvStroke *s, int sw, int sh, Box *b)
{
	double x[4], y[4];

	memset(b, 0, sizeof *b);
	b->ux = 1;
	b->vy = 1;
	if (s->n < 4)
		return;
	for (int i = 0; i < 4; i++) {
		x[i] = s->pts[2 * i] * sw;
		y[i] = s->pts[2 * i + 1] * sh;
		b->cx += x[i] / 4;
		b->cy += y[i] / 4;
	}
	b->ux = x[1] - x[0];
	b->uy = y[1] - y[0];
	b->hw = hypot(b->ux, b->uy) / 2;
	b->vx = x[3] - x[0];
	b->vy = y[3] - y[0];
	b->hh = hypot(b->vx, b->vy) / 2;
	if (!unit(&b->ux, &b->uy)) {
		b->ux = 1;
		b->uy = 0;
	}
	if (!unit(&b->vx, &b->vy)) {
		b->vx = -b->uy;
		b->vy = b->ux;
	}
}

/* A new rectangle stroke (not yet in any state) for box `b'. */
static IvStroke *box_to_stroke(const Box *b, int sw, int sh, guint32 rgba)
{
	static const double corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 },
					     { -1, 1 } };
	IvStroke *s = iv_stroke_new(IV_STROKE_RECT, rgba, 0);

	for (int i = 0; i < 4; i++) {
		double x, y;

		box_point(b, corner[i][0], corner[i][1], &x, &y);
		iv_stroke_add_point(s, x / sw, y / sh);
	}
	return s;
}

/* The directions of the screen's x and y axes on the SOURCE, as the box of
 * a rectangle drawn now is laid out: through the turn, the mirror and the
 * straightening shown. */
static void screen_axes(IvEditor *ed, Box *b)
{
	cairo_matrix_t m;

	b->ux = 1;
	b->uy = 0;
	b->vx = 0;
	b->vy = 1;
	if (!iv_brush_source_matrix(ed, &m) ||
	    cairo_matrix_invert(&m) != CAIRO_STATUS_SUCCESS)
		return;
	cairo_matrix_transform_distance(&m, &b->ux, &b->uy);
	cairo_matrix_transform_distance(&m, &b->vx, &b->vy);
	if (!unit(&b->ux, &b->uy) || !unit(&b->vx, &b->vy)) {
		b->ux = 1;
		b->uy = 0;
		b->vx = 0;
		b->vy = 1;
	}
}

/* Widget pixels per SOURCE pixel, as the picture is shown now. */
static double view_scale(IvEditor *ed)
{
	cairo_matrix_t m;

	return iv_brush_source_matrix(ed, &m) ? iv_brush_matrix_scale(&m) : 1;
}

/* The chosen rectangle, if it is still in the drawing (an undo may have
 * taken it away); its box in `b'. */
static gboolean chosen_box(IvEditor *ed, Box *b)
{
	int sw, sh;

	if (!chosen || iv_edit_state_find_stroke(iv_editor_committed(ed),
						 chosen) < 0)
		return FALSE;
	iv_editor_source_size(ed, &sw, &sh);
	if (sw <= 0 || sh <= 0)
		return FALSE;
	box_from_stroke(chosen, sw, sh, b);
	return TRUE;
}

static void choose(IvStroke *s)
{
	if (s == chosen)
		return;
	if (s)
		iv_stroke_ref(s);
	if (chosen)
		iv_stroke_unref(chosen);
	chosen = s;
}

/* The handle of box `b' at widget (wx, wy), or -1.  The sides' handles
 * only where the box is large enough on the screen to tell them apart. */
static int handle_at(IvEditor *ed, const Box *b, double wx, double wy)
{
	cairo_matrix_t m;
	double scale, best = HANDLE_REACH;
	gboolean sides;
	int hit = -1;

	if (!iv_brush_source_matrix(ed, &m))
		return -1;
	scale = iv_brush_matrix_scale(&m);
	sides = 2 * b->hw * scale > 6 * HANDLE_HALF &&
		2 * b->hh * scale > 6 * HANDLE_HALF;
	for (int i = 0; i < (sides ? 8 : 4); i++) {
		double x, y, d;

		box_point(b, handles[i].su, handles[i].sv, &x, &y);
		cairo_matrix_transform_point(&m, &x, &y);
		d = hypot(x - wx, y - wy);
		/* Strictly nearer: a corner keeps a tie. */
		if (d < best) {
			best = d;
			hit = i;
		}
	}
	return hit;
}

/* The topmost rectangle under SOURCE (sx, sy), or NULL. */
static IvStroke *rect_at(IvEditor *ed, double sx, double sy)
{
	const IvEditState *st = iv_editor_committed(ed);
	double margin = 2 / view_scale(ed);
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	if (sw <= 0 || sh <= 0)
		return NULL;
	for (guint i = st->strokes->len; i > 0; i--) {
		IvStroke *s = g_ptr_array_index(st->strokes, i - 1);
		Box b;

		if (s->kind != IV_STROKE_RECT)
			continue;
		box_from_stroke(s, sw, sh, &b);
		if (box_contains(&b, sx, sy, margin))
			return s;
	}
	return NULL;
}

/* The resize cursor for handle `h' of box `b': by the way the handle
 * points from the centre on the screen, so a turned picture's box still
 * shows the arrows its edges move along. */
static const char *handle_cursor(IvEditor *ed, const Box *b, int h)
{
	cairo_matrix_t m;
	double dx = handles[h].su * b->hw * b->ux + handles[h].sv * b->hh * b->vx;
	double dy = handles[h].su * b->hw * b->uy + handles[h].sv * b->hh * b->vy;
	double a;

	if (iv_brush_source_matrix(ed, &m))
		cairo_matrix_transform_distance(&m, &dx, &dy);
	/* The angle folded into 0..180 degrees: a line has no direction. */
	a = fmod(atan2(dy, dx) * 180 / G_PI + 360, 180);
	if (a < 22.5 || a >= 157.5)
		return "ew-resize";
	if (a < 67.5)
		return "nwse-resize";
	if (a < 112.5)
		return "ns-resize";
	return "nesw-resize";
}

/* What the pointer at widget (wx, wy), SOURCE (sx, sy) would do. */
static void box_cursor(IvEditor *ed, double wx, double wy, double sx,
		       double sy)
{
	Box b;
	int h;

	switch (box_drag) {
	case BOX_NEW:
		iv_editor_set_cursor(ed, "crosshair");
		return;
	case BOX_MOVE:
		iv_editor_set_cursor(ed, "move");
		return;
	case BOX_RESIZE:
		iv_editor_set_cursor(ed, handle_cursor(ed, &box_start, box_handle));
		return;
	case BOX_IDLE:
		break;
	}
	if (chosen_box(ed, &b) && (h = handle_at(ed, &b, wx, wy)) >= 0)
		iv_editor_set_cursor(ed, handle_cursor(ed, &b, h));
	else if (rect_at(ed, sx, sy))
		iv_editor_set_cursor(ed, "move");
	else
		iv_editor_set_cursor(ed, "crosshair");
}

static void box_press(IvEditor *ed, double dx, double dy, double wx, double wy)
{
	IvStroke *hit;
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	to_source(ed, dx, dy, &press_sx, &press_sy);
	press_wx = wx;
	press_wy = wy;
	box_moved = FALSE;
	if (chosen_box(ed, &box_start)) {
		int h = handle_at(ed, &box_start, wx, wy);

		if (h >= 0) {
			box_drag = BOX_RESIZE;
			box_handle = h;
			box_now = box_start;
			return;
		}
	}
	hit = rect_at(ed, press_sx, press_sy);
	if (hit) {
		choose(hit);
		box_from_stroke(hit, sw, sh, &box_start);
		box_now = box_start;
		box_drag = BOX_MOVE;
		return;
	}
	/* Elsewhere: a new rectangle, from here. */
	choose(NULL);
	memset(&box_start, 0, sizeof box_start);
	screen_axes(ed, &box_start);
	box_start.cx = press_sx;
	box_start.cy = press_sy;
	box_now = box_start;
	box_drag = BOX_NEW;
}

static void box_motion(IvEditor *ed, double dx, double dy, double wx,
		       double wy, gboolean shift)
{
	double sx, sy, du, dv;
	Box b = box_start;

	/* A press that has not moved yet is a click: it chooses a
	 * rectangle (or none) without moving anything. */
	if (!box_moved && hypot(wx - press_wx, wy - press_wy) < 3)
		return;
	box_moved = TRUE;
	to_source(ed, dx, dy, &sx, &sy);
	switch (box_drag) {
	case BOX_NEW:
		/* From the press to the pointer along the screen's axes as
		 * they lie on the source. */
		du = (sx - press_sx) * b.ux + (sy - press_sy) * b.uy;
		dv = (sx - press_sx) * b.vx + (sy - press_sy) * b.vy;
		if (shift) {
			double m = MAX(fabs(du), fabs(dv));

			du = copysign(m, du);
			dv = copysign(m, dv);
		}
		b.cx = press_sx + du / 2 * b.ux + dv / 2 * b.vx;
		b.cy = press_sy + du / 2 * b.uy + dv / 2 * b.vy;
		b.hw = fabs(du) / 2;
		b.hh = fabs(dv) / 2;
		break;
	case BOX_MOVE:
		b.cx += sx - press_sx;
		b.cy += sy - press_sy;
		break;
	case BOX_RESIZE: {
		int su = handles[box_handle].su, sv = handles[box_handle].sv;
		double l = -b.hw, r = b.hw, t = -b.hh, bot = b.hh;
		double plu, plv, lu, lv, nl, nr, nt, nb;

		/* The handle follows the pointer by the way it moved, not
		 * by where it is: a press a few pixels off the handle does
		 * not make the side jump. */
		box_local(&box_start, press_sx, press_sy, &plu, &plv);
		box_local(&box_start, sx, sy, &lu, &lv);
		lu += su * b.hw - plu;
		lv += sv * b.hh - plv;
		if (su > 0)
			r = lu;
		else if (su < 0)
			l = lu;
		if (sv > 0)
			bot = lv;
		else if (sv < 0)
			t = lv;
		/* Dragged over the opposite side, the box turns inside out
		 * rather than vanishing. */
		nl = MIN(l, r);
		nr = MAX(l, r);
		nt = MIN(t, bot);
		nb = MAX(t, bot);
		b.cx = box_start.cx + (nl + nr) / 2 * b.ux + (nt + nb) / 2 * b.vx;
		b.cy = box_start.cy + (nl + nr) / 2 * b.uy + (nt + nb) / 2 * b.vy;
		b.hw = (nr - nl) / 2;
		b.hh = (nb - nt) / 2;
		break;
	}
	case BOX_IDLE:
		return;
	}
	box_now = b;
}

static void box_release(IvEditor *ed)
{
	BoxDrag d = box_drag;
	double scale = view_scale(ed);
	IvEditState *live;
	IvStroke *s;
	int sw, sh, i;

	box_drag = BOX_IDLE;
	iv_editor_source_size(ed, &sw, &sh);
	if (!box_moved || sw <= 0 || sh <= 0)
		return;
	/* Too small to see on the screen: no rectangle. */
	if (2 * box_now.hw * scale < BOX_MIN_SCREEN ||
	    2 * box_now.hh * scale < BOX_MIN_SCREEN) {
		if (d == BOX_NEW)
			return;
		/* A resize keeps at least that much. */
		box_now.hw = MAX(box_now.hw, BOX_MIN_SCREEN / 2 / scale);
		box_now.hh = MAX(box_now.hh, BOX_MIN_SCREEN / 2 / scale);
	}
	s = box_to_stroke(&box_now, sw, sh,
			  d == BOX_NEW || !chosen ? stroke_rgba(DRAW_RECT) :
						    chosen->rgba);
	live = iv_editor_begin(ed);
	if (d == BOX_NEW) {
		iv_edit_state_add_stroke(live, s);
	} else {
		i = iv_edit_state_find_stroke(live, chosen);
		if (i < 0) {
			iv_stroke_unref(s);
			iv_editor_cancel(ed);
			return;
		}
		iv_edit_state_replace_stroke(live, (guint)i, s);
	}
	choose(s);
	iv_stroke_unref(s);
	iv_editor_commit(ed, d == BOX_NEW  ? "Rectangle" :
			     d == BOX_MOVE ? "Move rectangle" :
					     "Resize rectangle");
	sync_panel();
}

/* The chosen rectangle in another colour, as its own step. */
static void recolour_chosen(IvEditor *ed, guint32 rgba)
{
	IvEditState *live;
	IvStroke *s;
	int i;

	if (!chosen || chosen->rgba == rgba)
		return;
	i = iv_edit_state_find_stroke(iv_editor_committed(ed), chosen);
	if (i < 0)
		return;
	s = iv_stroke_new(IV_STROKE_RECT, rgba, 0);
	for (int k = 0; k < chosen->n; k++)
		iv_stroke_add_point(s, chosen->pts[2 * k], chosen->pts[2 * k + 1]);
	live = iv_editor_begin(ed);
	iv_edit_state_replace_stroke(live, (guint)i, s);
	choose(s);
	iv_stroke_unref(s);
	iv_editor_commit(ed, "Rectangle colour");
}

static void delete_chosen(IvEditor *ed)
{
	int i = chosen ? iv_edit_state_find_stroke(iv_editor_committed(ed),
						   chosen) :
			 -1;

	choose(NULL);
	if (i < 0)
		return;
	iv_edit_state_remove_stroke(iv_editor_begin(ed), (guint)i);
	iv_editor_commit(ed, "Delete rectangle");
	sync_panel();
}

/* The outline and the handles of the box being worked on, in widget
 * pixels (cairo's matrix is the widget's). */
static void draw_box_handles(IvEditor *ed, cairo_t *cr, const Box *b)
{
	static const double corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 },
					     { -1, 1 } };
	GtkStyleContext *sc =
		gtk_widget_get_style_context(IV_CANVAS_GTK(iv_editor_canvas(ed)));
	cairo_matrix_t m;
	GdkRGBA accent;
	double scale;
	gboolean sides;

	if (!iv_brush_source_matrix(ed, &m))
		return;
	if (!gtk_style_context_lookup_color(sc, "likeos_shell_accent", &accent))
		gdk_rgba_parse(&accent, "#5b8ff9");
	scale = iv_brush_matrix_scale(&m);
	sides = 2 * b->hw * scale > 6 * HANDLE_HALF &&
		2 * b->hh * scale > 6 * HANDLE_HALF;
	cairo_save(cr);
	cairo_new_path(cr);
	for (int i = 0; i < 4; i++) {
		double x, y;

		box_point(b, corner[i][0], corner[i][1], &x, &y);
		cairo_matrix_transform_point(&m, &x, &y);
		cairo_line_to(cr, x, y);
	}
	cairo_close_path(cr);
	/* A dark line under the accent one, so the outline shows on any
	 * picture and on a box of any colour. */
	cairo_set_line_width(cr, 3);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
	cairo_stroke_preserve(cr);
	cairo_set_line_width(cr, 1.5);
	gdk_cairo_set_source_rgba(cr, &accent);
	cairo_stroke(cr);
	for (int i = 0; i < (sides ? 8 : 4); i++) {
		double x, y;

		box_point(b, handles[i].su, handles[i].sv, &x, &y);
		cairo_matrix_transform_point(&m, &x, &y);
		iv_rounded_rect(cr, x - HANDLE_HALF, y - HANDLE_HALF,
				2 * HANDLE_HALF, 2 * HANDLE_HALF, 2);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
		cairo_set_line_width(cr, 2);
		cairo_stroke_preserve(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill_preserve(cr);
		gdk_cairo_set_source_rgba(cr, &accent);
		cairo_set_line_width(cr, 1.25);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

/* ---- the canvas -------------------------------------------------------------------- */

static void widget_point(IvEditor *ed, double dx, double dy, double *wx,
			 double *wy)
{
	iv_editor_display_to_widget(ed, dx, dy, wx, wy);
}

static gboolean canvas_event(IvEditor *ed, GdkEvent *ev, double x, double y)
{
	double wx, wy, ox, oy;

	switch (ev->type) {
	case GDK_BUTTON_PRESS:
		if (ev->button.button != 1)
			return FALSE;
		widget_point(ed, x, y, &wx, &wy);
		pointer_in = TRUE;
		pointer_wx = wx;
		pointer_wy = wy;
		if (current == DRAW_RECT) {
			box_press(ed, x, y, wx, wy);
			to_source(ed, x, y, &ox, &oy);
			box_cursor(ed, wx, wy, ox, oy);
		} else if (current == DRAW_ERASER) {
			erasing = TRUE;
			erased = 0;
			to_source(ed, x, y, &erase_x, &erase_y);
			erase_along(ed, erase_x, erase_y, erase_x, erase_y);
		} else {
			stroke_begin(ed, x, y, wx, wy);
		}
		iv_editor_redraw_overlay(ed);
		return TRUE;
	case GDK_2BUTTON_PRESS:
	case GDK_3BUTTON_PRESS:
		/* A quick second click is the start of another stroke, not
		 * the canvas's zoom toggle. */
		return ev->button.button == 1;
	case GDK_MOTION_NOTIFY:
		widget_point(ed, x, y, &wx, &wy);
		pointer_in = TRUE;
		pointer_wx = wx;
		pointer_wy = wy;
		if (current == DRAW_RECT) {
			if (box_drag != BOX_IDLE)
				box_motion(ed, x, y, wx, wy,
					   (ev->motion.state & GDK_SHIFT_MASK) != 0);
			to_source(ed, x, y, &ox, &oy);
			box_cursor(ed, wx, wy, ox, oy);
			iv_editor_redraw_overlay(ed);
			return box_drag != BOX_IDLE;
		}
		if (erasing) {
			to_source(ed, x, y, &ox, &oy);
			erase_along(ed, erase_x, erase_y, ox, oy);
			erase_x = ox;
			erase_y = oy;
		} else if (drawing) {
			stroke_motion(ed, x, y, wx, wy,
				      (ev->motion.state & GDK_SHIFT_MASK) != 0);
		}
		iv_editor_redraw_overlay(ed);
		return drawing || erasing;
	case GDK_BUTTON_RELEASE:
		if (ev->button.button != 1)
			return FALSE;
		if (box_drag != BOX_IDLE) {
			box_release(ed);
			widget_point(ed, x, y, &wx, &wy);
			to_source(ed, x, y, &ox, &oy);
			box_cursor(ed, wx, wy, ox, oy);
			iv_editor_redraw_overlay(ed);
			return TRUE;
		}
		if (erasing) {
			erasing = FALSE;
			iv_editor_commit(ed, "Erase");
			sync_panel();
			iv_editor_redraw_overlay(ed);
			return TRUE;
		}
		if (!drawing)
			return FALSE;
		stroke_end(ed, x, y);
		return TRUE;
	case GDK_LEAVE_NOTIFY:
		/* While a button is held the stroke goes on past the edge. */
		if (!drawing && !erasing && box_drag == BOX_IDLE) {
			pointer_in = FALSE;
			iv_editor_redraw_overlay(ed);
		}
		return FALSE;
	default:
		return FALSE;
	}
}

/* ---- the overlay -------------------------------------------------------------------- */

/* One stroke (cairo's user space SOURCE pixels), clipped to its own
 * bounds so the group the painter draws it in stays small (a canvas-sized
 * group per stroke would make a busy drawing slow to show). */
static void draw_one(cairo_t *cr, const IvStroke *s, int sw, int sh)
{
	double x0, y0, x1, y1, cx0, cy0, cx1, cy1;

	if (s->n <= 0)
		return;
	iv_stroke_bounds(s, (double)sw / sh, &x0, &y0, &x1, &y1);
	cairo_save(cr);
	cairo_rectangle(cr, floor(x0 * sw) - 2, floor(y0 * sh) - 2,
			ceil((x1 - x0) * sw) + 4, ceil((y1 - y0) * sh) + 4);
	cairo_clip(cr);
	cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1);
	if (cx1 > cx0 && cy1 > cy0)
		iv_render_draw_stroke(cr, s, sw, sh);
	cairo_restore(cr);
}

static void draw_cursor(IvEditor *ed, cairo_t *cr, double scale)
{
	double lw = width_px(ed, current) * scale;

	switch (current) {
	case DRAW_ERASER:
		iv_brush_draw_cursor(cr, pointer_wx, pointer_wy, lw / 2,
				     IV_CURSOR_ERASER, 0);
		break;
	case DRAW_CALLIGRAPHY:
		iv_brush_draw_nib(cr, pointer_wx, pointer_wy, lw, NIB_ANGLE,
				  opts[current].rgba);
		break;
	case DRAW_MARKER:
	case DRAW_PEN:
	default:
		iv_brush_draw_cursor(cr, pointer_wx, pointer_wy, lw / 2,
				     IV_CURSOR_RING,
				     drawing ? 0 : opts[current].rgba);
		break;
	}
}

static void draw_overlay(IvEditor *ed, cairo_t *cr)
{
	const IvEditState *st = iv_editor_state(ed);
	cairo_matrix_t om, sm, base;
	int ow, oh, sw, sh;

	if (!iv_brush_output_matrix(ed, &om) || !iv_brush_source_matrix(ed, &sm))
		return;
	iv_brush_output_size(ed, &ow, &oh);
	iv_editor_source_size(ed, &sw, &sh);
	cairo_save(cr);
	cairo_get_matrix(cr, &base);
	/* The saved picture ends at its edges; so do the strokes here. */
	cairo_transform(cr, &om);
	cairo_rectangle(cr, 0, 0, ow, oh);
	cairo_clip(cr);
	/* The strokes are on the source. */
	cairo_set_matrix(cr, &base);
	cairo_transform(cr, &sm);
	for (guint i = 0; i < st->strokes->len; i++) {
		IvStroke *s = g_ptr_array_index(st->strokes, i);

		/* The rectangle being moved or resized shows where it is
		 * going, not where it was. */
		if (s == chosen && box_moved &&
		    (box_drag == BOX_MOVE || box_drag == BOX_RESIZE))
			continue;
		draw_one(cr, s, sw, sh);
	}
	if (drawing)
		draw_one(cr, drawing, sw, sh);
	if (box_drag != BOX_IDLE && box_moved && sw > 0 && sh > 0) {
		IvStroke *b = box_to_stroke(&box_now, sw, sh,
					    box_drag == BOX_NEW || !chosen ?
						    stroke_rgba(DRAW_RECT) :
						    chosen->rgba);

		draw_one(cr, b, sw, sh);
		iv_stroke_unref(b);
	}
	cairo_restore(cr);
	if (current == DRAW_RECT) {
		Box b;

		if (box_drag != BOX_IDLE && box_moved)
			draw_box_handles(ed, cr, &box_now);
		else if (box_drag == BOX_IDLE && chosen_box(ed, &b))
			draw_box_handles(ed, cr, &b);
		return;
	}
	if (pointer_in)
		draw_cursor(ed, cr, iv_brush_matrix_scale(&sm));
}

/* ---- the panel -------------------------------------------------------------------- */

static void select_draw_tool(DrawTool t)
{
	if (t < 0 || t >= DRAW_N)
		return;
	if (drawing || erasing || box_drag != BOX_IDLE)
		return;
	current = t;
	if (t != DRAW_ERASER)
		last_pen = t;
	/* The handles belong to the rectangle tool; the others draw their
	 * own pointer. */
	if (t != DRAW_RECT)
		choose(NULL);
	if (editor)
		iv_editor_set_cursor(editor, t == DRAW_RECT ? "crosshair" : "none");
	sync_panel();
	if (editor)
		iv_editor_redraw_overlay(editor);
}

static void tile_toggled(GtkToggleButton *b, gpointer data)
{
	if (syncing)
		return;
	if (!gtk_toggle_button_get_active(b)) {
		/* A tile stays pressed until another is chosen. */
		syncing = TRUE;
		gtk_toggle_button_set_active(b, TRUE);
		syncing = FALSE;
		return;
	}
	select_draw_tool((DrawTool)GPOINTER_TO_INT(data));
}

static void colour_picked(guint32 rgba, gpointer data)
{
	if (current == DRAW_ERASER)
		return;
	/* The tool's opacity is its own; a colour with alpha from the
	 * chooser makes it lighter still. */
	opts[current].rgba = rgba;
	store_opts(current);
	gtk_widget_queue_draw(preview);
	/* With a rectangle chosen, the colour is for it as well. */
	if (editor && current == DRAW_RECT && !syncing && box_drag == BOX_IDLE)
		recolour_chosen(editor, stroke_rgba(DRAW_RECT));
	if (editor)
		iv_editor_redraw_overlay(editor);
}

static void size_text(void)
{
	char buf[48];
	double px = editor ? width_px(editor, current) : 0;

	if (px <= 0)
		return;
	if (px < 10)
		g_snprintf(buf, sizeof buf, "%.1f px", px);
	else
		g_snprintf(buf, sizeof buf, "%d px", (int)lround(px));
	iv_brush_slider_set_text(size_row, buf);
}

static void size_changed(double v, gboolean final, gpointer data)
{
	opts[current].size = CLAMP(v, WIDTH_MIN, WIDTH_MAX);
	if (final)
		store_opts(current);
	size_text();
	gtk_widget_queue_draw(preview);
	if (editor)
		iv_editor_redraw_overlay(editor);
}

static void change_size(double factor)
{
	if (current == DRAW_RECT)
		return;
	opts[current].size = CLAMP(opts[current].size * factor, WIDTH_MIN, WIDTH_MAX);
	store_opts(current);
	sync_panel();
	if (editor)
		iv_editor_redraw_overlay(editor);
}

/* The sample stroke: a gentle wave across a box that is half paper, half
 * dark, at the width the tool draws with on the picture as it is shown
 * now -- a dark ink needs the light half, a white pen the dark one, and
 * the marker shows how it reads on both. */
static gboolean preview_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	double scale = 1, lw;
	IvStroke *s;
	cairo_matrix_t m;

	cairo_save(cr);
	iv_rounded_rect(cr, 0.5, 0.5, W - 1, H - 1, 8);
	cairo_clip_preserve(cr);
	cairo_set_source_rgb(cr, 0x16 / 255.0, 0x1c / 255.0, 0x2a / 255.0);
	cairo_fill(cr);
	cairo_rectangle(cr, 0, 0, W / 2, H);
	cairo_set_source_rgb(cr, 0xf5 / 255.0, 0xf7 / 255.0, 0xfb / 255.0);
	cairo_fill(cr);

	if (editor && iv_brush_source_matrix(editor, &m))
		scale = iv_brush_matrix_scale(&m);
	lw = (editor ? width_px(editor, current) : opts[current].size) * scale;
	if (current == DRAW_ERASER) {
		/* The eraser's own size, where the two halves meet. */
		iv_brush_draw_cursor(cr, W / 2, H / 2, MIN(lw / 2, H / 2 - 4),
				     IV_CURSOR_ERASER, 0);
	} else {
		/* Never wider than the box, never thinner than a hairline. */
		lw = CLAMP(lw, 1, H * 0.7);
		s = iv_stroke_new(opts[current].kind, stroke_rgba(current),
				  lw / W);
		for (int i = 0; i <= 24; i++) {
			double t = i / 24.0;

			iv_stroke_add_point(s, 0.08 + 0.84 * t,
					    0.5 - 0.22 * sin(t * 2 * G_PI));
		}
		iv_render_draw_stroke(cr, s, W, H);
		iv_stroke_unref(s);
	}
	cairo_restore(cr);
	/* A hairline round it. */
	iv_rounded_rect(cr, 0.5, 0.5, W - 1, H - 1, 8);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	return TRUE;
}

static void clear_answered(gboolean yes, gpointer data)
{
	IvEditor *ed = data;
	IvEditState *s;

	if (!yes || iv_editor_committed(ed)->strokes->len == 0)
		return;
	choose(NULL);
	s = iv_edit_state_copy(iv_editor_committed(ed));
	iv_edit_state_clear_strokes(s);
	iv_editor_apply(ed, s, "Clear drawing");
	iv_editor_toast(ed, "Drawing cleared");
}

static void clear_clicked(GtkButton *b, IvEditor *ed)
{
	guint n = iv_editor_committed(ed)->strokes->len;
	char *text;

	if (n == 0 || iv_editor_busy(ed))
		return;
	text = g_strdup_printf(n == 1 ? "What was drawn is removed from the "
					"picture.  Undo brings it back." :
					"All %u marks are removed from the "
					"picture.  Undo brings them back.",
			       n);
	iv_editor_confirm(ed, "Clear the drawing?", text, "_Clear all",
			  "_Keep drawing", TRUE, clear_answered, ed);
	g_free(text);
}

static GtkWidget *tile(DrawTool t)
{
	GtkWidget *b = gtk_toggle_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *l = gtk_label_new(opts[t].label);

	gtk_box_pack_start(GTK_BOX(box), iv_glyph_new(opts[t].glyph, 20), FALSE,
			   FALSE, 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	iv_add_class(l, "iv-draw-tile-label");
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_tooltip_text(b, opts[t].tooltip);
	atk_object_set_name(gtk_widget_get_accessible(b), opts[t].label);
	iv_add_class(b, "iv-draw-tile");
	g_signal_connect(b, "toggled", G_CALLBACK(tile_toggled),
			 GINT_TO_POINTER(t));
	return b;
}

static void sync_panel(void)
{
	const IvEditState *st;
	guint n;
	char *text;

	if (!size_row)
		return;
	syncing = TRUE;
	for (int i = 0; i < DRAW_N; i++)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tiles[i]),
					     i == (int)current);
	syncing = FALSE;
	gtk_widget_set_visible(colour_box, current != DRAW_ERASER);
	gtk_widget_set_visible(eraser_note, current == DRAW_ERASER);
	gtk_widget_set_visible(rect_note, current == DRAW_RECT);
	/* A rectangle has no width. */
	gtk_widget_set_visible(size_head, current != DRAW_RECT);
	gtk_widget_set_visible(size_row, current != DRAW_RECT);
	gtk_widget_set_visible(preview, current != DRAW_RECT);
	/* The chosen rectangle's colour while there is one. */
	if (current == DRAW_RECT && chosen && editor &&
	    iv_edit_state_find_stroke(iv_editor_committed(editor), chosen) >= 0)
		iv_panel_swatches_set(swatches, chosen->rgba);
	else if (current != DRAW_ERASER)
		iv_panel_swatches_set(swatches, opts[current].rgba);
	if (current != DRAW_RECT) {
		iv_brush_slider_set(size_row, opts[current].size);
		size_text();
	}
	gtk_widget_queue_draw(preview);

	st = editor ? iv_editor_committed(editor) : NULL;
	n = st ? st->strokes->len : 0;
	if (n == 0) {
		text = g_strdup("Nothing drawn yet");
	} else {
		guint boxes = 0, lines;
		char *a, *b;

		for (guint i = 0; i < n; i++)
			if (((IvStroke *)g_ptr_array_index(st->strokes, i))->kind ==
			    IV_STROKE_RECT)
				boxes++;
		lines = n - boxes;
		a = g_strdup_printf(lines == 1 ? "%u stroke" : "%u strokes", lines);
		b = g_strdup_printf(boxes == 1 ? "%u rectangle" : "%u rectangles",
				    boxes);
		text = !boxes ? g_strdup(a) :
		       !lines ? g_strdup(b) :
				g_strdup_printf("%s, %s", a, b);
		g_free(a);
		g_free(b);
	}
	gtk_label_set_text(GTK_LABEL(count_label), text);
	g_free(text);
	gtk_widget_set_sensitive(clear_button, n > 0);
}

static GtkWidget *build_panel(IvEditor *ed)
{
	GtkWidget *p = iv_panel_new("Draw", "Mark up the picture with a pen, a "
					    "highlighter or a broad nib, or cover "
					    "parts of it with rectangles.");
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *head, *box;

	editor = ed;
	load_opts();
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	iv_add_class(row, "iv-draw-tiles");
	for (int i = 0; i < DRAW_N; i++) {
		tiles[i] = tile((DrawTool)i);
		gtk_box_pack_start(GTK_BOX(row), tiles[i], TRUE, TRUE, 0);
	}
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 6);

	/* Colour: hidden for the eraser, which has a note instead. */
	colour_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	iv_brush_section(colour_box, IV_GLYPH_COLOR, "Colour", NULL);
	swatches = iv_panel_swatches(palette, G_N_ELEMENTS(palette),
				     opts[current].rgba, colour_picked, NULL);
	/* Equal cells: the eight colours and the chooser fit one row. */
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(swatches), TRUE);
	iv_add_class(swatches, "iv-brush-swatches");
	gtk_box_pack_start(GTK_BOX(colour_box), swatches, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), colour_box, FALSE, FALSE, 0);

	eraser_note = iv_panel_note("Drag across the drawing: every stroke the "
				    "eraser touches goes, as a whole.");
	gtk_widget_set_no_show_all(eraser_note, TRUE);
	gtk_box_pack_start(GTK_BOX(p), eraser_note, FALSE, FALSE, 4);

	rect_note = iv_panel_note("Drag on the picture for a filled rectangle – "
				  "over a name or a number, say, that should "
				  "not be seen.  Drag a rectangle to move it, "
				  "its handles to resize it; Delete removes the "
				  "one chosen.");
	gtk_widget_set_no_show_all(rect_note, TRUE);
	gtk_box_pack_start(GTK_BOX(p), rect_note, FALSE, FALSE, 4);

	size_head = iv_brush_section(p, IV_GLYPH_BRUSH, "Size", NULL);
	gtk_widget_show_all(size_head);
	gtk_widget_set_no_show_all(size_head, TRUE);
	size_row = iv_brush_slider("Width", WIDTH_MIN, WIDTH_MAX, opts[current].size,
				   TRUE, " px", size_changed, NULL);
	gtk_box_pack_start(GTK_BOX(p), size_row, FALSE, FALSE, 0);
	gtk_widget_show_all(size_row);
	gtk_widget_set_no_show_all(size_row, TRUE);
	preview = gtk_drawing_area_new();
	gtk_widget_set_no_show_all(preview, TRUE);
	gtk_widget_set_size_request(preview, -1, 64);
	iv_add_class(preview, "iv-draw-preview");
	g_signal_connect(preview, "draw", G_CALLBACK(preview_draw), NULL);
	gtk_box_pack_start(GTK_BOX(p), preview, FALSE, FALSE, 6);

	iv_brush_section(p, IV_GLYPH_DRAW, "Drawing", NULL);
	head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	count_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(count_label), 0);
	iv_add_class(count_label, "iv-panel-note");
	gtk_box_pack_start(GTK_BOX(head), count_label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(p), head, FALSE, FALSE, 0);
	clear_button = iv_panel_button(IV_GLYPH_CLEAR, "Clear all");
	gtk_widget_set_tooltip_text(clear_button, "Remove every stroke");
	g_signal_connect(clear_button, "clicked", G_CALLBACK(clear_clicked), ed);
	gtk_box_pack_start(GTK_BOX(p), clear_button, FALSE, FALSE, 0);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	iv_add_class(box, "iv-brush-tips");
	gtk_box_pack_start(GTK_BOX(box),
			   iv_brush_tip("Hold Shift while drawing for a straight "
					"line, or a square."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   iv_brush_tip("[ and ] change the width; P, M, K, X and "
					"E pick the tools."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   iv_brush_tip("Strokes stay sharp at any size: they are "
					"drawn again when the picture is saved."),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), box, FALSE, FALSE, 8);

	/* The editor shows the panel and then activates the tool, whose
	 * sync_panel() sets which of the colours, the size and the notes
	 * are visible (they are kept out of show_all). */
	return p;
}

/* ---- the tool ------------------------------------------------------------------------ */

static void activate(IvEditor *ed)
{
	editor = ed;
	/* The strokes are drawn by the overlay while this tool is current. */
	iv_editor_set_render_flags(ed, IV_RENDER_NO_DRAW);
	iv_editor_set_drag_pan(ed, FALSE);
	iv_editor_set_cursor(ed, current == DRAW_RECT ? "crosshair" : "none");
	pointer_in = FALSE;
	sync_panel();
}

static void deactivate(IvEditor *ed)
{
	if (drawing) {
		iv_stroke_unref(drawing);
		drawing = NULL;
	}
	if (erasing) {
		erasing = FALSE;
		iv_editor_commit(ed, "Erase");
	}
	box_drag = BOX_IDLE;
	choose(NULL);
	pointer_in = FALSE;
	if (flats)
		g_hash_table_remove_all(flats);
}

static void state_changed(IvEditor *ed)
{
	/* Undo may have brought back strokes the eraser had dropped; the
	 * cache only keys on strokes it holds, so nothing in it is stale.
	 * A chosen rectangle the undo took away is chosen no more. */
	if (chosen && iv_edit_state_find_stroke(iv_editor_committed(ed),
						chosen) < 0)
		choose(NULL);
	sync_panel();
	iv_editor_redraw_overlay(ed);
}

static gboolean key_press(IvEditor *ed, GdkEventKey *ev)
{
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	guint key = gdk_keyval_to_lower(ev->keyval);

	if (mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK))
		return FALSE;
	if (current == DRAW_RECT && box_drag == BOX_IDLE && chosen) {
		switch (ev->keyval) {
		case GDK_KEY_Delete:
		case GDK_KEY_KP_Delete:
		case GDK_KEY_BackSpace:
			delete_chosen(ed);
			iv_editor_redraw_overlay(ed);
			return TRUE;
		case GDK_KEY_Escape:
			/* Esc lets go of the rectangle first, and only then
			 * leaves the editor. */
			choose(NULL);
			sync_panel();
			iv_editor_redraw_overlay(ed);
			return TRUE;
		}
	}
	switch (key) {
	case GDK_KEY_bracketleft:
		change_size(1 / 1.2);
		return TRUE;
	case GDK_KEY_bracketright:
		change_size(1.2);
		return TRUE;
	case GDK_KEY_e:
		select_draw_tool(current == DRAW_ERASER ? last_pen : DRAW_ERASER);
		return TRUE;
	}
	for (int i = 0; i < DRAW_N; i++)
		if (opts[i].key == key && i != DRAW_ERASER) {
			select_draw_tool((DrawTool)i);
			return TRUE;
		}
	return FALSE;
}

const IvTool iv_tool_draw = {
	.id = "draw",
	.label = "Draw",
	.glyph = IV_GLYPH_DRAW,
	.tooltip = "Draw (D)",
	.key = GDK_KEY_d,
	.build_panel = build_panel,
	.activate = activate,
	.deactivate = deactivate,
	.canvas_event = canvas_event,
	.draw_overlay = draw_overlay,
	.state_changed = state_changed,
	.key_press = key_press,
};
