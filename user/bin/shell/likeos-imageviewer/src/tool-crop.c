// LikeOS -- LikeOS Image Viewer: the Crop tool.
//
// Cutting, straightening, turning and mirroring the picture.  While the
// tool is current the canvas shows the whole straightened picture, uncut,
// on a canvas the size of the turned picture (IV_RENDER_NO_CROP |
// IV_RENDER_STEADY), and the crop is drawn over it as an overlay: the
// parts that are cut away darkened, a light frame with round knobs at the
// corners and short bars on the edges, the rule-of-thirds lines while it
// is dragged (a finer grid while the straighten dial turns) and the size
// of the result.  The canvas keeps its size whatever the angle, so the
// view stays at the same scale while the dial turns: the picture turns in
// place and the crop shrinks inside it, instead of the view zooming in on
// an ever smaller frame.
//
// The crop rectangle lives in the edit state as fractions of the FRAME
// (geo.crop_*), so it survives every other edit.  Straightening keeps it
// inside the turned picture by itself: the FRAME is the largest rectangle
// of the picture's own proportions that the straightened picture still
// covers (editstate.h), and the crop is a part of the FRAME.
//
// Pointer:
//   corner knob     resizes from the opposite corner
//   edge bar        moves that edge (with a ratio: the size follows,
//                   centred across)
//   inside          moves the crop
//   outside         drags a new crop from that point
//   Shift           while resizing freely: keeps the current proportions
//   double-click    inside: Apply
// Keys (with the canvas focused):
//   arrows          move the crop by a screen pixel; Shift: ten
//   Ctrl+arrows     grow or shrink it at the right and bottom edges
//   [  ]            turn left / right
//   Return          Apply
//   Escape          while dragging: put the crop back
//
// Every gesture -- one drag, one run of arrow presses, one turn of the
// dial, one button -- is a single undo step.  "Apply" commits and shows
// the result as it will be saved; touching the crop again goes back to
// the whole frame.  Leaving the tool commits whatever is still open.
//
// Turning and mirroring keep what is on the screen consistent: the crop
// turns and mirrors with the picture, the straighten angle changes sign
// when the picture is mirrored, and the ratio turns with it (16:9 becomes
// 9:16).  A quarter turn of a mirrored picture swaps which mirror is set,
// because the mirrors act after the turns (editstate.h): turning a
// left/right mirrored picture by 90 degrees is the same as turning it and
// mirroring it top/bottom.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "editor.h"
#include "tool-crop-dial.h"

/* Sizes on the screen, in widget pixels. */
#define KNOB_R 6.5		/* corner knob radius */
#define BAR_LEN 22.0		/* edge bar length */
#define BAR_W 6.0		/* edge bar thickness */
#define HIT_CORNER 14.0		/* how near a corner counts as on it */
#define HIT_EDGE 10.0		/* how near an edge counts as on it */
#define MIN_CROP_PX 32.0	/* smallest crop on the screen */
#define NUDGE_REST_MS 700	/* arrow presses end their step after this */

/* What a pointer press grabs: a set of edges, the whole crop, or a new
 * crop. */
enum {
	HIT_NONE = 0,
	HIT_L = 1 << 0,
	HIT_R = 1 << 1,
	HIT_T = 1 << 2,
	HIT_B = 1 << 3,
	HIT_MOVE = 1 << 4,
	HIT_NEW = 1 << 5,
};

/* A crop rectangle in fractions of the FRAME. */
typedef struct {
	double x, y, w, h;
} Rect;

static struct {
	IvEditor *ed;

	/* the panel */
	GtkWidget *panel;
	GtkWidget *chips, *swap;
	GtkWidget *dial, *straighten_reset;
	GtkWidget *size_value, *size_detail;
	GtkWidget *reset, *apply;

	/* the gesture under way */
	int drag;		/* HIT_*, 0 = none */
	Rect drag_from;		/* the crop when it began */
	double press_nx, press_ny;	/* where it began, fractions */
	gboolean drag_moved;
	gboolean straightening;	/* the dial is turning: show a fine grid */

	/* the pointer at rest */
	int hover;

	/* arrow keys */
	guint nudge_rest;

	/* Apply: the result is shown until the crop is touched again */
	gboolean showing_result;
} C;

static void sync_panel(IvEditor *ed);

/* What the canvas shows while the crop is being made. */
#define FRAME_FLAGS (IV_RENDER_NO_CROP | IV_RENDER_STEADY)

/* ---- the frame and the crop ---------------------------------------------------- */

/* Where the FRAME's top-left corner is on the canvas (DISPLAY pixels):
 * the steady canvas has it centred. */
static void frame_origin(IvEditor *ed, const IvEditState *st, double *x,
			 double *y)
{
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	iv_render_frame_origin(st, sw, sh, iv_editor_render_flags(ed), x, y);
}

static void frame_size(IvEditor *ed, const IvEditState *st, double *fw,
		       double *fh)
{
	int sw, sh;

	iv_editor_source_size(ed, &sw, &sh);
	iv_geometry_frame_size(&st->geo, sw, sh, fw, fh);
	/* What a NO_CROP render shows: whole pixels. */
	*fw = MAX(1, floor(*fw));
	*fh = MAX(1, floor(*fh));
}

static Rect crop_of(const IvEditState *st)
{
	Rect r = { st->geo.crop_x, st->geo.crop_y, st->geo.crop_w,
		   st->geo.crop_h };

	return r;
}

static void set_crop(IvEditState *st, const Rect *r)
{
	st->geo.crop_x = CLAMP(r->x, 0, 1);
	st->geo.crop_y = CLAMP(r->y, 0, 1);
	st->geo.crop_w = CLAMP(r->w, 0, 1 - st->geo.crop_x);
	st->geo.crop_h = CLAMP(r->h, 0, 1 - st->geo.crop_y);
	/* Rounding must not leave a whole crop that is "almost" whole. */
	if (fabs(st->geo.crop_x) < 1e-9 && fabs(st->geo.crop_w - 1) < 1e-9)
		st->geo.crop_w = 1;
	if (fabs(st->geo.crop_y) < 1e-9 && fabs(st->geo.crop_h - 1) < 1e-9)
		st->geo.crop_h = 1;
}

/* The ratio the crop keeps, as width / height of the FRACTIONS (not of
 * the pixels): a 16:9 crop of a 4:3 frame is (16/9) / (4/3) wide per
 * high in fractions.  0: free. */
static double fraction_ratio(IvEditor *ed, const IvEditState *st)
{
	double fw, fh, px;

	frame_size(ed, st, &fw, &fh);
	px = iv_aspect_ratio(st->geo.aspect, fw, fh);
	return px > 0 ? px * fh / fw : 0;
}

/* The smallest crop, in fractions: MIN_CROP_PX on the screen, but never
 * less than a few pixels of the result nor more than the whole frame. */
static void min_size(IvEditor *ed, const IvEditState *st, double *mw,
		     double *mh)
{
	double fw, fh, scale = MAX(1e-3, iv_editor_view_scale(ed));
	double px = MAX(8.0, MIN_CROP_PX / scale);

	frame_size(ed, st, &fw, &fh);
	*mw = MIN(1.0, px / fw);
	*mh = MIN(1.0, px / fh);
}

/* The largest crop of ratio `rn' (fractions) centred on (cx, cy) as far
 * as the frame allows. */
static Rect largest_with_ratio(double rn, double cx, double cy)
{
	Rect r;

	r.w = 1;
	r.h = 1 / rn;
	if (r.h > 1) {
		r.h = 1;
		r.w = rn;
	}
	r.x = CLAMP(cx - r.w / 2, 0, 1 - r.w);
	r.y = CLAMP(cy - r.h / 2, 0, 1 - r.h);
	return r;
}

/* The crop a drag makes.  `from' is the crop when the drag began, (dx,
 * dy) how far the pointer has gone since, in fractions; `rn' the ratio to
 * keep (0: free); (mw, mh) the smallest size. */
static Rect drag_result(const Rect *from, int hit, double dx, double dy,
			double rn, double mw, double mh)
{
	double x0 = from->x, y0 = from->y;
	double x1 = from->x + from->w, y1 = from->y + from->h;
	gboolean horiz = (hit & (HIT_L | HIT_R)) != 0;
	gboolean vert = (hit & (HIT_T | HIT_B)) != 0;
	Rect r = *from;

	if (hit & HIT_MOVE) {
		r.x = CLAMP(from->x + dx, 0, 1 - from->w);
		r.y = CLAMP(from->y + dy, 0, 1 - from->h);
		return r;
	}
	if (rn <= 0) {
		/* Free: each grabbed edge follows the pointer, as far as the
		 * frame and the smallest size let it. */
		if (hit & HIT_L)
			x0 = CLAMP(x0 + dx, 0, x1 - mw);
		if (hit & HIT_R)
			x1 = CLAMP(x1 + dx, x0 + mw, 1);
		if (hit & HIT_T)
			y0 = CLAMP(y0 + dy, 0, y1 - mh);
		if (hit & HIT_B)
			y1 = CLAMP(y1 + dy, y0 + mh, 1);
		r.x = x0;
		r.y = y0;
		r.w = x1 - x0;
		r.h = y1 - y0;
		return r;
	}
	if (horiz && vert) {
		/* A corner with a ratio: the opposite corner stays, the size
		 * follows whichever way the pointer went further. */
		double ax = (hit & HIT_L) ? x1 : x0;
		double ay = (hit & HIT_T) ? y1 : y0;
		double px = ((hit & HIT_L) ? x0 : x1) + dx;
		double py = ((hit & HIT_T) ? y0 : y1) + dy;
		double w = (hit & HIT_L) ? ax - px : px - ax;
		double h = (hit & HIT_T) ? ay - py : py - ay;
		double maxw = (hit & HIT_L) ? ax : 1 - ax;
		double maxh = (hit & HIT_T) ? ay : 1 - ay;

		w = MAX(w, mw);
		h = MAX(h, mh);
		if (w / rn > h)
			h = w / rn;
		else
			w = h * rn;
		if (w > maxw) {
			w = maxw;
			h = w / rn;
		}
		if (h > maxh) {
			h = maxh;
			w = h * rn;
		}
		r.x = (hit & HIT_L) ? ax - w : ax;
		r.y = (hit & HIT_T) ? ay - h : ay;
		r.w = w;
		r.h = h;
		return r;
	}
	if (horiz) {
		/* A side edge with a ratio: the other side stays, the height
		 * follows, centred where it was (moved in if it must). */
		double ax = (hit & HIT_L) ? x1 : x0;
		double px = ((hit & HIT_L) ? x0 : x1) + dx;
		double w = MAX((hit & HIT_L) ? ax - px : px - ax, mw);
		double maxw = (hit & HIT_L) ? ax : 1 - ax;
		double cy = from->y + from->h / 2;
		double h;

		w = MIN(w, maxw);
		h = w / rn;
		if (h > 1) {
			h = 1;
			w = h * rn;
		}
		r.x = (hit & HIT_L) ? ax - w : ax;
		r.w = w;
		r.h = h;
		r.y = CLAMP(cy - h / 2, 0, 1 - h);
		return r;
	}
	if (vert) {
		double ay = (hit & HIT_T) ? y1 : y0;
		double py = ((hit & HIT_T) ? y0 : y1) + dy;
		double h = MAX((hit & HIT_T) ? ay - py : py - ay, mh);
		double maxh = (hit & HIT_T) ? ay : 1 - ay;
		double cx = from->x + from->w / 2;
		double w;

		h = MIN(h, maxh);
		w = h * rn;
		if (w > 1) {
			w = 1;
			h = w / rn;
		}
		r.y = (hit & HIT_T) ? ay - h : ay;
		r.h = h;
		r.w = w;
		r.x = CLAMP(cx - w / 2, 0, 1 - w);
		return r;
	}
	return r;
}

/* ---- the crop on the screen ---------------------------------------------------- */

/* The crop and the whole canvas in widget pixels. */
static void widget_rects(IvEditor *ed, double *cx0, double *cy0, double *cx1,
			 double *cy1, double *fx0, double *fy0, double *fx1,
			 double *fy1)
{
	const IvEditState *st = iv_editor_state(ed);
	double fw, fh, ox, oy;
	int dw, dh;
	Rect r = crop_of(st);

	frame_size(ed, st, &fw, &fh);
	frame_origin(ed, st, &ox, &oy);
	iv_editor_display_to_widget(ed, ox + r.x * fw, oy + r.y * fh, cx0, cy0);
	iv_editor_display_to_widget(ed, ox + (r.x + r.w) * fw,
				    oy + (r.y + r.h) * fh, cx1, cy1);
	/* The whole canvas: on the steady one that is more than the FRAME,
	 * and all of it outside the crop is cut away. */
	iv_editor_display_size(ed, &dw, &dh);
	if (fx0)
		iv_editor_display_to_widget(ed, 0, 0, fx0, fy0);
	if (fx1)
		iv_editor_display_to_widget(ed, dw, dh, fx1, fy1);
}

/* What the pointer at widget (wx, wy) would grab. */
static int hit_test(IvEditor *ed, double wx, double wy)
{
	double x0, y0, x1, y1;
	double tc, te;
	gboolean near_l, near_r, near_t, near_b, within_x, within_y;

	widget_rects(ed, &x0, &y0, &x1, &y1, NULL, NULL, NULL, NULL);
	/* On a tiny crop the zones must not swallow each other: the
	 * corners win, but leave the middle for moving. */
	tc = MIN(HIT_CORNER, MAX(4.0, (x1 - x0) / 3));
	tc = MIN(tc, MAX(4.0, (y1 - y0) / 3));
	te = MIN(HIT_EDGE, tc);
	if (fabs(wx - x0) <= tc && fabs(wy - y0) <= tc)
		return HIT_L | HIT_T;
	if (fabs(wx - x1) <= tc && fabs(wy - y0) <= tc)
		return HIT_R | HIT_T;
	if (fabs(wx - x0) <= tc && fabs(wy - y1) <= tc)
		return HIT_L | HIT_B;
	if (fabs(wx - x1) <= tc && fabs(wy - y1) <= tc)
		return HIT_R | HIT_B;
	within_x = wx > x0 && wx < x1;
	within_y = wy > y0 && wy < y1;
	near_l = fabs(wx - x0) <= te && within_y;
	near_r = fabs(wx - x1) <= te && within_y;
	near_t = fabs(wy - y0) <= te && within_x;
	near_b = fabs(wy - y1) <= te && within_x;
	if (near_l)
		return HIT_L;
	if (near_r)
		return HIT_R;
	if (near_t)
		return HIT_T;
	if (near_b)
		return HIT_B;
	if (within_x && within_y)
		return HIT_MOVE;
	return HIT_NEW;
}

static const char *cursor_for(int hit)
{
	switch (hit) {
	case HIT_L | HIT_T:
	case HIT_R | HIT_B:
		return "nwse-resize";
	case HIT_R | HIT_T:
	case HIT_L | HIT_B:
		return "nesw-resize";
	case HIT_L:
	case HIT_R:
		return "ew-resize";
	case HIT_T:
	case HIT_B:
		return "ns-resize";
	case HIT_MOVE:
		return "move";
	case HIT_NEW:
		return "crosshair";
	default:
		return NULL;
	}
}

static void accent_color(IvEditor *ed, GdkRGBA *c)
{
	GtkStyleContext *sc =
		gtk_widget_get_style_context(IV_CANVAS_GTK(iv_editor_canvas(ed)));

	if (!gtk_style_context_lookup_color(sc, "likeos_shell_accent", c))
		gdk_rgba_parse(c, "#5b8ff9");
}

/* A capsule, lying or standing. */
static void pill(cairo_t *cr, double x, double y, double w, double h)
{
	cairo_new_path(cr);
	iv_rounded_rect(cr, x, y, w, h, MIN(w, h) / 2);
}

/* Lines dividing the crop in `n' parts each way. */
static void grid_lines(cairo_t *cr, double x0, double y0, double x1, double y1,
		       int n)
{
	for (int i = 1; i < n; i++) {
		double x = round(x0 + (x1 - x0) * i / n) + 0.5;
		double y = round(y0 + (y1 - y0) * i / n) + 0.5;

		cairo_move_to(cr, x, y0);
		cairo_line_to(cr, x, y1);
		cairo_move_to(cr, x0, y);
		cairo_line_to(cr, x1, y);
	}
}

/* The size of the result, in a dark pill at the bottom of the crop. */
static void draw_size_label(IvEditor *ed, cairo_t *cr, double x0, double y0,
			    double x1, double y1)
{
	const IvEditState *st = iv_editor_state(ed);
	PangoLayout *l;
	char buf[64];
	int sw, sh, ow, oh, tw, th;
	double bx, by, bw, bh;

	iv_editor_source_size(ed, &sw, &sh);
	iv_geometry_output_size(&st->geo, sw, sh, &ow, &oh);
	g_snprintf(buf, sizeof buf, "%d × %d", ow, oh);
	l = gtk_widget_create_pango_layout(IV_CANVAS_GTK(iv_editor_canvas(ed)),
					   buf);
	pango_layout_get_pixel_size(l, &tw, &th);
	bw = tw + 20;
	bh = th + 8;
	if (bw > x1 - x0 - 8 || bh > y1 - y0 - 8) {
		/* No room inside: under the crop. */
		bx = (x0 + x1) / 2 - bw / 2;
		by = y1 + 8;
	} else {
		bx = (x0 + x1) / 2 - bw / 2;
		by = y1 - bh - 10;
	}
	bx = round(bx);
	by = round(by);
	cairo_set_source_rgba(cr, 0.06, 0.08, 0.12, 0.82);
	pill(cr, bx, by, bw, bh);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 0.93, 0.95, 0.98, 1);
	cairo_move_to(cr, bx + 10, by + 4);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

static void draw_overlay(IvEditor *ed, cairo_t *cr)
{
	double x0, y0, x1, y1, fx0, fy0, fx1, fy1;
	GdkRGBA accent;
	gboolean dragging = C.drag != 0 && C.drag_moved;

	if (C.showing_result || !iv_editor_source(ed))
		return;
	widget_rects(ed, &x0, &y0, &x1, &y1, &fx0, &fy0, &fx1, &fy1);
	accent_color(ed, &accent);
	/* Whole pixels, so the hairlines are crisp. */
	x0 = round(x0);
	y0 = round(y0);
	x1 = round(x1);
	y1 = round(y1);

	cairo_save(cr);

	/* What is cut away: darkened. */
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_rectangle(cr, fx0, fy0, fx1 - fx0, fy1 - fy0);
	cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
	cairo_set_source_rgba(cr, 0.04, 0.05, 0.09, dragging ? 0.5 : 0.62);
	cairo_fill(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);

	/* The guides: thirds while the crop moves, a fine grid while the
	 * picture is being straightened (to line up horizons and walls). */
	cairo_set_line_width(cr, 1);
	if (C.straightening) {
		cairo_set_source_rgba(cr, 1, 1, 1, 0.28);
		grid_lines(cr, x0, y0, x1, y1, 9);
		cairo_stroke(cr);
	} else if (dragging) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
		cairo_save(cr);
		cairo_translate(cr, 1, 1);
		grid_lines(cr, x0, y0, x1, y1, 3);
		cairo_stroke(cr);
		cairo_restore(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
		grid_lines(cr, x0, y0, x1, y1, 3);
		cairo_stroke(cr);
	}

	/* The frame: a dark line outside a light one, so it shows on
	 * bright and dark pictures alike. */
	cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
	cairo_rectangle(cr, x0 - 0.5, y0 - 0.5, x1 - x0 + 1, y1 - y0 + 1);
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
	cairo_rectangle(cr, x0 + 0.5, y0 + 0.5, x1 - x0 - 1, y1 - y0 - 1);
	cairo_stroke(cr);

	/* The handles: bars on the edges, knobs at the corners.  The one
	 * under the pointer (or being dragged) is drawn in the accent. */
	{
		int active = C.drag ? C.drag : C.hover;
		double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
		gboolean room_x = x1 - x0 > BAR_LEN + 4 * KNOB_R;
		gboolean room_y = y1 - y0 > BAR_LEN + 4 * KNOB_R;
		struct {
			int hit;
			double x, y;
			gboolean across;	/* the bar lies across */
		} bars[] = {
			{ HIT_T, mx, y0, TRUE },
			{ HIT_B, mx, y1, TRUE },
			{ HIT_L, x0, my, FALSE },
			{ HIT_R, x1, my, FALSE },
		};
		struct {
			int hit;
			double x, y;
		} knobs[] = {
			{ HIT_L | HIT_T, x0, y0 },
			{ HIT_R | HIT_T, x1, y0 },
			{ HIT_L | HIT_B, x0, y1 },
			{ HIT_R | HIT_B, x1, y1 },
		};

		for (guint i = 0; i < G_N_ELEMENTS(bars); i++) {
			double w = bars[i].across ? BAR_LEN : BAR_W;
			double h = bars[i].across ? BAR_W : BAR_LEN;

			if ((bars[i].across && !room_x) || (!bars[i].across && !room_y))
				continue;
			pill(cr, bars[i].x - w / 2, bars[i].y - h / 2, w, h);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
			cairo_set_line_width(cr, 2);
			cairo_stroke_preserve(cr);
			if (active == bars[i].hit)
				gdk_cairo_set_source_rgba(cr, &accent);
			else
				cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_fill(cr);
		}
		for (guint i = 0; i < G_N_ELEMENTS(knobs); i++) {
			cairo_arc(cr, knobs[i].x, knobs[i].y, KNOB_R, 0, 2 * G_PI);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
			cairo_set_line_width(cr, 2);
			cairo_stroke_preserve(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_fill(cr);
			/* A dot in the middle: accent when grabbed. */
			cairo_arc(cr, knobs[i].x, knobs[i].y, KNOB_R - 3, 0,
				  2 * G_PI);
			if (active == knobs[i].hit)
				gdk_cairo_set_source_rgba(cr, &accent);
			else
				cairo_set_source_rgba(cr, accent.red, accent.green,
						      accent.blue, 0.0);
			cairo_fill(cr);
		}
	}

	if (dragging || C.straightening || C.nudge_rest)
		draw_size_label(ed, cr, x0, y0, x1, y1);
	cairo_restore(cr);
}

/* ---- the live edit -------------------------------------------------------------- */

/* Ends what arrow presses or the dial still hold open, as their own step,
 * before something else begins. */
static void flush_pending(IvEditor *ed)
{
	if (C.nudge_rest) {
		g_source_remove(C.nudge_rest);
		C.nudge_rest = 0;
		iv_editor_commit(ed, "Crop");
	}
	if (C.dial && iv_dial_busy(C.dial))
		iv_dial_flush(C.dial);
}

/* Back from the Apply view to the whole frame. */
static void show_frame(IvEditor *ed)
{
	if (!C.showing_result)
		return;
	C.showing_result = FALSE;
	iv_editor_set_render_flags(ed, FRAME_FLAGS);
	if (C.apply)
		gtk_widget_set_sensitive(C.apply, TRUE);
	iv_editor_redraw_overlay(ed);
}

/* One finished step of geometry: `s' (taken) becomes the state. */
static void apply_geometry(IvEditor *ed, IvEditState *s, const char *label)
{
	flush_pending(ed);
	show_frame(ed);
	iv_editor_apply(ed, s, label);
}

/* ---- turning and mirroring ----------------------------------------------------- */

static Rect rect_turned(const Rect *r, int dir)
{
	Rect o;

	if (dir > 0) {
		/* clockwise: (u, v) -> (1 - v, u) */
		o.x = 1 - r->y - r->h;
		o.y = r->x;
	} else {
		/* counter-clockwise: (u, v) -> (v, 1 - u) */
		o.x = r->y;
		o.y = 1 - r->x - r->w;
	}
	o.w = r->h;
	o.h = r->w;
	return o;
}

static void turn(IvEditor *ed, int dir)
{
	IvEditState *s;
	Rect r;
	gboolean fh;

	flush_pending(ed);
	s = iv_edit_state_copy(iv_editor_committed(ed));
	r = crop_of(s);
	s->geo.quarter_turns = ((s->geo.quarter_turns + dir) % 4 + 4) % 4;
	/* Mirrors act after the turns: a quarter turn swaps them. */
	fh = s->geo.flip_h;
	s->geo.flip_h = s->geo.flip_v;
	s->geo.flip_v = fh;
	r = rect_turned(&r, dir);
	set_crop(s, &r);
	s->geo.aspect = iv_aspect_swapped(s->geo.aspect);
	apply_geometry(ed, s, dir > 0 ? "Rotate right" : "Rotate left");
}

static void mirror(IvEditor *ed, gboolean horizontal)
{
	IvEditState *s;
	Rect r;

	flush_pending(ed);
	s = iv_edit_state_copy(iv_editor_committed(ed));
	r = crop_of(s);
	if (horizontal) {
		s->geo.flip_h = !s->geo.flip_h;
		r.x = 1 - r.x - r.w;
	} else {
		s->geo.flip_v = !s->geo.flip_v;
		r.y = 1 - r.y - r.h;
	}
	/* A mirrored tilt leans the other way. */
	s->geo.straighten = -s->geo.straighten;
	set_crop(s, &r);
	apply_geometry(ed, s, horizontal ? "Flip horizontally" : "Flip vertically");
}

static void rotate_left(GtkButton *b, IvEditor *ed)
{
	turn(ed, -1);
}

static void rotate_right(GtkButton *b, IvEditor *ed)
{
	turn(ed, 1);
}

static void flip_h(GtkButton *b, IvEditor *ed)
{
	mirror(ed, TRUE);
}

static void flip_v(GtkButton *b, IvEditor *ed)
{
	mirror(ed, FALSE);
}

/* ---- the aspect ratio ------------------------------------------------------------ */

/* `a' becomes the ratio: the largest crop of it, centred where the crop
 * is now. */
static void choose_aspect(IvEditor *ed, IvAspect a, const char *label)
{
	IvEditState *s;
	Rect r;
	double rn;
	char *step;

	flush_pending(ed);
	s = iv_edit_state_copy(iv_editor_committed(ed));
	s->geo.aspect = a;
	rn = fraction_ratio(ed, s);
	r = crop_of(s);
	if (rn > 0) {
		r = largest_with_ratio(rn, r.x + r.w / 2, r.y + r.h / 2);
		set_crop(s, &r);
	}
	step = g_strdup_printf("Ratio %s", label ? label : iv_aspect_label(a));
	apply_geometry(ed, s, step);
	g_free(step);
}

static void aspect_chosen(int index, gpointer data)
{
	choose_aspect(data, (IvAspect)index, NULL);
}

/* The other orientation: 16:9 <-> 9:16.  Free and Original have no other
 * ratio of their own; for them the crop itself turns on its centre
 * (Original then becomes Free, since the turned crop is no longer the
 * picture's own ratio). */
static void swap_clicked(GtkButton *b, IvEditor *ed)
{
	const IvEditState *st = iv_editor_committed(ed);
	IvAspect a = st->geo.aspect;
	IvAspect sw = iv_aspect_swapped(a);

	if (a == IV_ASPECT_SQUARE)
		return;
	if (sw != a) {
		choose_aspect(ed, sw, NULL);
		return;
	} else {
		IvEditState *s = iv_edit_state_copy(st);
		double fw, fh, pw, ph, cx, cy;
		Rect r = crop_of(s);

		flush_pending(ed);
		frame_size(ed, s, &fw, &fh);
		/* The crop's size in pixels, turned, scaled down to fit. */
		pw = r.h * fh;
		ph = r.w * fw;
		if (pw > fw) {
			ph *= fw / pw;
			pw = fw;
		}
		if (ph > fh) {
			pw *= fh / ph;
			ph = fh;
		}
		cx = r.x + r.w / 2;
		cy = r.y + r.h / 2;
		r.w = pw / fw;
		r.h = ph / fh;
		r.x = CLAMP(cx - r.w / 2, 0, 1 - r.w);
		r.y = CLAMP(cy - r.h / 2, 0, 1 - r.h);
		set_crop(s, &r);
		s->geo.aspect = IV_ASPECT_FREE;
		apply_geometry(ed, s, "Turn the crop");
	}
}

/* ---- straightening ---------------------------------------------------------------- */

static void dial_changed(GtkWidget *dial, double value, gboolean final,
			 gpointer data)
{
	IvEditor *ed = data;
	IvEditState *s;

	if (C.nudge_rest) {
		g_source_remove(C.nudge_rest);
		C.nudge_rest = 0;
		iv_editor_commit(ed, "Crop");
	}
	show_frame(ed);
	if (!final) {
		s = iv_editor_begin(ed);
		s->geo.straighten = value;
		C.straightening = TRUE;
		iv_editor_changed(ed);
		sync_panel(ed);
		return;
	}
	C.straightening = FALSE;
	if (iv_editor_live(ed)) {
		s = iv_editor_begin(ed);
		s->geo.straighten = value;
		iv_editor_changed(ed);
		iv_editor_commit(ed, "Straighten");
	}
	iv_editor_redraw_overlay(ed);
	sync_panel(ed);
}

static void straighten_reset(GtkButton *b, IvEditor *ed)
{
	IvEditState *s = iv_edit_state_copy(iv_editor_committed(ed));

	s->geo.straighten = 0;
	apply_geometry(ed, s, "Level");
}

/* ---- reset and apply ------------------------------------------------------------- */

static void reset_clicked(GtkButton *b, IvEditor *ed)
{
	IvEditState *s = iv_edit_state_copy(iv_editor_committed(ed));

	iv_geometry_reset(&s->geo);
	apply_geometry(ed, s, "Reset crop");
}

static void do_apply(IvEditor *ed)
{
	flush_pending(ed);
	if (iv_editor_live(ed))
		iv_editor_commit(ed, "Crop");
	C.showing_result = TRUE;
	C.hover = HIT_NONE;
	iv_editor_set_cursor(ed, NULL);
	iv_editor_set_render_flags(ed, IV_RENDER_ALL);
	gtk_widget_set_sensitive(C.apply, FALSE);
	iv_editor_redraw_overlay(ed);
	if (!iv_geometry_is_identity(&iv_editor_committed(ed)->geo))
		iv_editor_toast(ed, "Crop applied — click the picture to change it");
}

static void apply_clicked(GtkButton *b, IvEditor *ed)
{
	do_apply(ed);
}

/* ---- the pointer ----------------------------------------------------------------- */

static void set_hover(IvEditor *ed, int hit)
{
	if (hit == C.hover)
		return;
	C.hover = hit;
	iv_editor_set_cursor(ed, cursor_for(hit));
	iv_editor_redraw_overlay(ed);
}

static gboolean pointer_press(IvEditor *ed, GdkEventButton *ev, double dx,
			      double dy)
{
	const IvEditState *st;
	double fw, fh, ox, oy;

	if (ev->button != 1)
		return FALSE;
	if (C.showing_result) {
		/* Back to the whole frame to change the crop. */
		show_frame(ed);
		return TRUE;
	}
	if (ev->type == GDK_2BUTTON_PRESS) {
		C.drag = 0;
		if (hit_test(ed, ev->x, ev->y) == HIT_MOVE)
			do_apply(ed);
		return TRUE;
	}
	if (ev->type != GDK_BUTTON_PRESS)
		return TRUE;
	flush_pending(ed);
	st = iv_editor_state(ed);
	frame_size(ed, st, &fw, &fh);
	frame_origin(ed, st, &ox, &oy);
	C.drag = hit_test(ed, ev->x, ev->y);
	C.drag_from = crop_of(st);
	C.press_nx = (dx - ox) / fw;
	C.press_ny = (dy - oy) / fh;
	C.drag_moved = FALSE;
	if (C.drag == HIT_NEW) {
		/* A new crop grows from the press point (inside the frame). */
		C.press_nx = CLAMP(C.press_nx, 0, 1);
		C.press_ny = CLAMP(C.press_ny, 0, 1);
		C.drag_from.x = C.press_nx;
		C.drag_from.y = C.press_ny;
		C.drag_from.w = 0;
		C.drag_from.h = 0;
	}
	iv_editor_set_cursor(ed, cursor_for(C.drag));
	return TRUE;
}

static gboolean pointer_motion(IvEditor *ed, GdkEventMotion *ev, double dx,
			       double dy)
{
	const IvEditState *st = iv_editor_state(ed);
	double fw, fh, ox, oy, nx, ny, mw, mh, rn;
	int hit = C.drag;
	Rect r;
	IvEditState *s;

	if (C.showing_result)
		return FALSE;
	if (!C.drag) {
		set_hover(ed, hit_test(ed, ev->x, ev->y));
		return FALSE;
	}
	if (!(ev->state & GDK_BUTTON1_MASK))
		return TRUE;
	frame_size(ed, st, &fw, &fh);
	frame_origin(ed, st, &ox, &oy);
	nx = (dx - ox) / fw - C.press_nx;
	ny = (dy - oy) / fh - C.press_ny;
	if (!C.drag_moved) {
		double scale = iv_editor_view_scale(ed);

		/* A few screen pixels before it counts as a drag. */
		if (hypot(nx * fw * scale, ny * fh * scale) < 3)
			return TRUE;
		C.drag_moved = TRUE;
	}
	min_size(ed, st, &mw, &mh);
	rn = fraction_ratio(ed, st);
	if (rn <= 0 && (ev->state & GDK_SHIFT_MASK) && C.drag != HIT_NEW &&
	    C.drag_from.h > 0)
		rn = C.drag_from.w / C.drag_from.h;
	if (hit == HIT_NEW) {
		/* Grows towards wherever the pointer is. */
		hit = (nx < 0 ? HIT_L : HIT_R) | (ny < 0 ? HIT_T : HIT_B);
	}
	r = drag_result(&C.drag_from, hit, nx, ny, rn, mw, mh);
	s = iv_editor_begin(ed);
	set_crop(s, &r);
	iv_editor_changed(ed);
	sync_panel(ed);
	return TRUE;
}

static gboolean pointer_release(IvEditor *ed, GdkEventButton *ev)
{
	if (ev->button != 1 || !C.drag)
		return C.showing_result ? FALSE : ev->button == 1;
	C.drag = 0;
	if (C.drag_moved)
		iv_editor_commit(ed, "Crop");
	C.drag_moved = FALSE;
	set_hover(ed, hit_test(ed, ev->x, ev->y));
	iv_editor_set_cursor(ed, cursor_for(C.hover));
	iv_editor_redraw_overlay(ed);
	sync_panel(ed);
	return TRUE;
}

static gboolean canvas_event(IvEditor *ed, GdkEvent *ev, double x, double y)
{
	switch (ev->type) {
	case GDK_BUTTON_PRESS:
	case GDK_2BUTTON_PRESS:
	case GDK_3BUTTON_PRESS:
		return pointer_press(ed, &ev->button, x, y);
	case GDK_MOTION_NOTIFY:
		return pointer_motion(ed, &ev->motion, x, y);
	case GDK_BUTTON_RELEASE:
		return pointer_release(ed, &ev->button);
	case GDK_LEAVE_NOTIFY:
		if (!C.drag)
			set_hover(ed, HIT_NONE);
		return FALSE;
	default:
		/* The wheel zooms as everywhere. */
		return FALSE;
	}
}

/* ---- keys -------------------------------------------------------------------------- */

static gboolean nudge_over(gpointer data)
{
	IvEditor *ed = data;

	C.nudge_rest = 0;
	iv_editor_commit(ed, "Crop");
	iv_editor_redraw_overlay(ed);
	sync_panel(ed);
	return G_SOURCE_REMOVE;
}

/* Arrow keys: move the crop, or with `resize' grow/shrink it at the right
 * and bottom, by `step_px' screen pixels. */
static void nudge(IvEditor *ed, int kx, int ky, gboolean resize, double step_px)
{
	const IvEditState *st;
	double fw, fh, scale = MAX(1e-3, iv_editor_view_scale(ed));
	double mw, mh, rn, dx, dy;
	Rect from, r;
	IvEditState *s;

	show_frame(ed);
	st = iv_editor_state(ed);
	frame_size(ed, st, &fw, &fh);
	dx = kx * step_px / scale / fw;
	dy = ky * step_px / scale / fh;
	from = crop_of(st);
	min_size(ed, st, &mw, &mh);
	rn = fraction_ratio(ed, st);
	if (resize) {
		int hit = (kx ? HIT_R : 0) | (ky ? HIT_B : 0);

		r = drag_result(&from, hit, dx, dy, rn, mw, mh);
	} else {
		r = drag_result(&from, HIT_MOVE, dx, dy, 0, mw, mh);
	}
	s = iv_editor_begin(ed);
	set_crop(s, &r);
	iv_editor_changed(ed);
	if (C.nudge_rest)
		g_source_remove(C.nudge_rest);
	C.nudge_rest = g_timeout_add(NUDGE_REST_MS, nudge_over, ed);
	sync_panel(ed);
}

/* Keys belong to a focused control of the panel first (the dial, the
 * chips): only with the canvas (or nothing) focused do they move the
 * crop. */
static gboolean key_press(IvEditor *ed, GdkEventKey *ev)
{
	GtkWidget *focus = gtk_window_get_focus(iv_editor_window(ed));
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
	gboolean shift = (mods & GDK_SHIFT_MASK) != 0;
	double step = shift ? 10 : 1;

	if (focus && focus == C.dial)
		return iv_dial_key(C.dial, ev);
	if (focus && focus != IV_CANVAS_GTK(iv_editor_canvas(ed)))
		return FALSE;
	if (mods & GDK_MOD1_MASK)
		return FALSE;
	switch (ev->keyval) {
	case GDK_KEY_Left:
	case GDK_KEY_KP_Left:
		nudge(ed, -1, 0, ctrl, step);
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_KP_Right:
		nudge(ed, 1, 0, ctrl, step);
		return TRUE;
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
		nudge(ed, 0, -1, ctrl, step);
		return TRUE;
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
		nudge(ed, 0, 1, ctrl, step);
		return TRUE;
	case GDK_KEY_bracketleft:
		if (ctrl)
			return FALSE;
		turn(ed, -1);
		return TRUE;
	case GDK_KEY_bracketright:
		if (ctrl)
			return FALSE;
		turn(ed, 1);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		if (ctrl)
			return FALSE;
		do_apply(ed);
		return TRUE;
	case GDK_KEY_Escape:
		if (C.drag) {
			/* The drag is put back; the button release that
			 * follows does nothing. */
			C.drag = 0;
			C.drag_moved = FALSE;
			iv_editor_cancel(ed);
			return TRUE;
		}
		return FALSE;
	default:
		return FALSE;
	}
}

/* ---- the panel ------------------------------------------------------------------- */

static void sync_panel(IvEditor *ed)
{
	const IvEditState *st = iv_editor_state(ed);
	const IvGeometry *g;
	int sw, sh, ow, oh;
	char buf[96];

	if (!C.panel || !st)
		return;
	g = &st->geo;
	iv_panel_chips_set(C.chips, g->aspect);
	gtk_widget_set_sensitive(C.swap, g->aspect != IV_ASPECT_SQUARE);
	if (!iv_dial_busy(C.dial))
		iv_dial_set_value(C.dial, g->straighten);
	gtk_widget_set_sensitive(C.straighten_reset, fabs(g->straighten) > 1e-6);
	gtk_widget_set_sensitive(C.reset, !iv_geometry_is_identity(g));
	gtk_widget_set_sensitive(C.apply, !C.showing_result);

	iv_editor_source_size(ed, &sw, &sh);
	if (sw > 0) {
		iv_geometry_output_size(g, sw, sh, &ow, &oh);
		g_snprintf(buf, sizeof buf, "%d × %d px", ow, oh);
		gtk_label_set_text(GTK_LABEL(C.size_value), buf);
		if (ow == sw && oh == sh && iv_geometry_is_identity(g))
			g_snprintf(buf, sizeof buf, "The whole picture");
		else
			g_snprintf(buf, sizeof buf, "%.0f %% of the picture · from %d × %d",
				   100.0 * ow * oh / ((double)sw * sh), sw, sh);
		gtk_label_set_text(GTK_LABEL(C.size_detail), buf);
	} else {
		gtk_label_set_text(GTK_LABEL(C.size_value), "");
		gtk_label_set_text(GTK_LABEL(C.size_detail), "");
	}
}

/* A section header with a glyph before its title; `extra' (may be NULL)
 * goes at its right end, before the reset button if there is one. */
static GtkWidget *section(GtkWidget *panel, IvGlyph g, const char *title,
			  GtkWidget **reset, GtkWidget *extra)
{
	GtkWidget *row = iv_panel_section(panel, title, reset);
	GtkWidget *glyph = iv_glyph_new(g, 14);

	iv_add_class(glyph, "iv-section-glyph");
	gtk_box_pack_start(GTK_BOX(row), glyph, FALSE, FALSE, 0);
	gtk_box_reorder_child(GTK_BOX(row), glyph, 0);
	if (extra)
		gtk_box_pack_end(GTK_BOX(row), extra, FALSE, FALSE, 0);
	return row;
}

/* A square tile: the glyph over a short caption. */
static GtkWidget *tile(IvGlyph g, const char *caption, const char *tip,
		       GCallback cb, IvEditor *ed)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *l = gtk_label_new(caption);

	gtk_box_pack_start(GTK_BOX(box), iv_glyph_new(g, 18), FALSE, FALSE, 0);
	iv_add_class(l, "iv-crop-tile-label");
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_hexpand(b, TRUE);
	gtk_widget_set_tooltip_text(b, tip);
	iv_add_class(b, "iv-crop-tile");
	g_signal_connect(b, "clicked", cb, ed);
	return b;
}

static GtkWidget *build_panel(IvEditor *ed)
{
	GtkWidget *p = iv_panel_new("Crop", "Cut, straighten, turn and mirror the "
					    "picture.");
	GtkWidget *row, *box;
	GtkWidget *reset = NULL;
	const char *aspect_labels[IV_ASPECT_COUNT];

	C.ed = ed;
	C.panel = p;
	iv_add_class(p, "iv-crop-panel");

	/* Aspect ratio, with the orientation swap at the header's end. */
	C.swap = iv_glyph_button(IV_GLYPH_SWAP, 14,
				 "Other orientation (portrait ↔ landscape)",
				 "iv-panel-reset");
	g_signal_connect(C.swap, "clicked", G_CALLBACK(swap_clicked), ed);
	section(p, IV_GLYPH_ASPECT, "ASPECT RATIO", NULL, C.swap);
	for (int i = 0; i < IV_ASPECT_COUNT; i++)
		aspect_labels[i] = iv_aspect_label((IvAspect)i);
	C.chips = iv_panel_chips(aspect_labels, IV_ASPECT_COUNT, IV_ASPECT_FREE,
				 aspect_chosen, ed);
	iv_add_class(C.chips, "iv-aspect-chips");
	gtk_box_pack_start(GTK_BOX(p), C.chips, FALSE, FALSE, 0);

	/* Straighten. */
	section(p, IV_GLYPH_STRAIGHTEN, "STRAIGHTEN", &reset, NULL);
	C.straighten_reset = reset;
	gtk_widget_set_tooltip_text(reset, "Level (0°)");
	g_signal_connect(reset, "clicked", G_CALLBACK(straighten_reset), ed);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	iv_add_class(box, "iv-dial-card");
	C.dial = iv_dial_new(dial_changed, ed);
	gtk_box_pack_start(GTK_BOX(box), C.dial, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), box, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p),
			   iv_panel_note("Drag or scroll the ruler, or use the "
					 "arrow keys (Shift: tenths). The corners "
					 "are trimmed so none stay empty."),
			   FALSE, FALSE, 0);

	/* Turn and mirror. */
	section(p, IV_GLYPH_ROTATE_RIGHT, "ROTATE AND FLIP", NULL, NULL);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	gtk_box_pack_start(GTK_BOX(row),
			   tile(IV_GLYPH_ROTATE_LEFT, "Left", "Rotate left ([)",
				G_CALLBACK(rotate_left), ed),
			   TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row),
			   tile(IV_GLYPH_ROTATE_RIGHT, "Right", "Rotate right (])",
				G_CALLBACK(rotate_right), ed),
			   TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row),
			   tile(IV_GLYPH_FLIP_H, "Mirror",
				"Flip horizontally (left and right swap)",
				G_CALLBACK(flip_h), ed),
			   TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row),
			   tile(IV_GLYPH_FLIP_V, "Flip",
				"Flip vertically (upside down)",
				G_CALLBACK(flip_v), ed),
			   TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	/* The result. */
	section(p, IV_GLYPH_IMAGE, "RESULT", NULL, NULL);
	C.size_value = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(C.size_value), 0);
	iv_add_class(C.size_value, "iv-crop-size");
	gtk_box_pack_start(GTK_BOX(p), C.size_value, FALSE, FALSE, 0);
	C.size_detail = iv_panel_note("");
	gtk_box_pack_start(GTK_BOX(p), C.size_detail, FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	iv_add_class(row, "iv-crop-actions");
	C.reset = iv_text_button(IV_GLYPH_RESET, "Reset crop", FALSE,
				 "iv-outline-button");
	gtk_widget_set_tooltip_text(C.reset, "Undo the crop, the straightening, "
					     "the turns and the mirrors");
	gtk_widget_set_hexpand(C.reset, TRUE);
	g_signal_connect(C.reset, "clicked", G_CALLBACK(reset_clicked), ed);
	C.apply = iv_text_button(IV_GLYPH_CHECK, "Apply", FALSE, "iv-accent-button");
	gtk_widget_set_tooltip_text(C.apply, "Show the result (Return)");
	gtk_widget_set_hexpand(C.apply, TRUE);
	g_signal_connect(C.apply, "clicked", G_CALLBACK(apply_clicked), ed);
	gtk_box_pack_start(GTK_BOX(row), C.reset, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), C.apply, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	sync_panel(ed);
	return p;
}

/* ---- the tool --------------------------------------------------------------------- */

static void activate(IvEditor *ed)
{
	C.ed = ed;
	C.drag = 0;
	C.hover = HIT_NONE;
	C.showing_result = FALSE;
	C.straightening = FALSE;
	iv_editor_set_render_flags(ed, FRAME_FLAGS);
	/* The first button is the crop's; the middle one and the wheel
	 * still move and zoom the view. */
	iv_editor_set_drag_pan(ed, FALSE);
	sync_panel(ed);
}

static void deactivate(IvEditor *ed)
{
	/* What is still open becomes its step. */
	if (C.dial && iv_dial_busy(C.dial))
		iv_dial_flush(C.dial);
	if (C.nudge_rest) {
		g_source_remove(C.nudge_rest);
		C.nudge_rest = 0;
	}
	if (iv_editor_live(ed))
		iv_editor_commit(ed, "Crop");
	C.drag = 0;
	C.drag_moved = FALSE;
	C.hover = HIT_NONE;
	C.straightening = FALSE;
	C.showing_result = FALSE;
}

static void state_changed(IvEditor *ed)
{
	/* Undo or redo in the middle of a gesture ends it. */
	if (!iv_editor_live(ed)) {
		C.drag = 0;
		C.drag_moved = FALSE;
		if (C.nudge_rest) {
			g_source_remove(C.nudge_rest);
			C.nudge_rest = 0;
		}
	}
	sync_panel(ed);
}

const IvTool iv_tool_crop = {
	.id = "crop",
	.label = "Crop",
	.glyph = IV_GLYPH_CROP,
	.tooltip = "Crop and rotate (C)",
	.key = GDK_KEY_c,
	.build_panel = build_panel,
	.activate = activate,
	.deactivate = deactivate,
	.canvas_event = canvas_event,
	.draw_overlay = draw_overlay,
	.state_changed = state_changed,
	.key_press = key_press,
};
