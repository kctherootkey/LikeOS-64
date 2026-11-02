// LikeOS -- LikeOS Media Player: the player's own line glyphs.
//
// Every glyph of the transport strip, the sidebar, the page commands and the
// placeholders is drawn here as vector geometry, so it is sharp at any size
// and takes whatever colour the caller has set as the cairo source.  The
// drawings are LikeOS's own: a 16-unit box, strokes 1.5 units wide (thinner
// in proportion on large placeholders), round caps and round joins, and solid
// shapes only where a shape reads better filled -- the play triangle, the
// pause bars, the skip marks, the dots of "more".
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "ui.h"

typedef void (*Painter)(cairo_t *cr);

/* ---- helpers, in glyph units ------------------------------------------ */

static void line(cairo_t *cr, double x0, double y0, double x1, double y1)
{
	cairo_move_to(cr, x0, y0);
	cairo_line_to(cr, x1, y1);
	cairo_stroke(cr);
}

/* An open polyline of `n' points. */
static void poly(cairo_t *cr, const double *p, int n)
{
	cairo_move_to(cr, p[0], p[1]);
	for (int i = 1; i < n; i++)
		cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
	cairo_stroke(cr);
}

static void circle(cairo_t *cr, double cx, double cy, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
	cairo_stroke(cr);
}

static void dot(cairo_t *cr, double cx, double cy, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
	cairo_fill(cr);
}

static void rrect_path(cairo_t *cr, double x0, double y0, double x1,
		       double y1, double r)
{
	mp_rounded_rect(cr, x0, y0, x1 - x0, y1 - y0, r);
}

static void rrect(cairo_t *cr, double x0, double y0, double x1, double y1,
		  double r)
{
	rrect_path(cr, x0, y0, x1, y1, r);
	cairo_stroke(cr);
}

static void rrect_fill(cairo_t *cr, double x0, double y0, double x1,
		       double y1, double r)
{
	rrect_path(cr, x0, y0, x1, y1, r);
	cairo_fill(cr);
}

/* A closed shape filled and outlined with the round join, which rounds
 * its corners by half a stroke. */
static void solid(cairo_t *cr, const double *p, int n)
{
	cairo_move_to(cr, p[0], p[1]);
	for (int i = 1; i < n; i++)
		cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
	cairo_close_path(cr);
	cairo_fill_preserve(cr);
	cairo_stroke(cr);
}

/* An open arrow head with its tip at (x, y), pointing along (dx, dy). */
static void arrow_head(cairo_t *cr, double x, double y, double dx, double dy,
		       double len)
{
	double l = hypot(dx, dy);
	if (l <= 0)
		return;
	dx /= l;
	dy /= l;
	/* The two wings: the way back, turned 45 degrees each side. */
	const double c = cos(G_PI / 4), s = sin(G_PI / 4);
	double ux = -dx, uy = -dy;
	double ax = ux * c - uy * s, ay = ux * s + uy * c;
	double bx = ux * c + uy * s, by = -ux * s + uy * c;
	cairo_move_to(cr, x + ax * len, y + ay * len);
	cairo_line_to(cr, x, y);
	cairo_line_to(cr, x + bx * len, y + by * len);
	cairo_stroke(cr);
}

/* ---- transport -------------------------------------------------------- */

static void gl_play(cairo_t *cr)
{
	static const double p[] = { 5.25, 3.5, 12.75, 8, 5.25, 12.5 };
	solid(cr, p, 3);
}

static void gl_pause(cairo_t *cr)
{
	rrect_fill(cr, 4, 3, 6.75, 13, 1.1);
	rrect_fill(cr, 9.25, 3, 12, 13, 1.1);
}

static void gl_next(cairo_t *cr)
{
	static const double p[] = { 3.75, 4, 9.5, 8, 3.75, 12 };
	solid(cr, p, 3);
	rrect_fill(cr, 11, 3.5, 12.75, 12.5, 0.85);
}

static void gl_previous(cairo_t *cr)
{
	static const double p[] = { 12.25, 4, 6.5, 8, 12.25, 12 };
	solid(cr, p, 3);
	rrect_fill(cr, 3.25, 3.5, 5, 12.5, 0.85);
}

static void gl_shuffle(cairo_t *cr)
{
	/* Two lanes that swap sides on the way across. */
	cairo_move_to(cr, 2, 4.75);
	cairo_line_to(cr, 4.25, 4.75);
	cairo_curve_to(cr, 8, 4.75, 8, 11.25, 11.75, 11.25);
	cairo_line_to(cr, 13.75, 11.25);
	cairo_stroke(cr);
	cairo_move_to(cr, 2, 11.25);
	cairo_line_to(cr, 4.25, 11.25);
	cairo_curve_to(cr, 8, 11.25, 8, 4.75, 11.75, 4.75);
	cairo_line_to(cr, 13.75, 4.75);
	cairo_stroke(cr);
	arrow_head(cr, 13.75, 4.75, 1, 0, 2.4);
	arrow_head(cr, 13.75, 11.25, 1, 0, 2.4);
}

static void repeat_loop(cairo_t *cr)
{
	/* Two halves of a rounded loop, each ending in an arrow. */
	cairo_move_to(cr, 2.75, 8.75);
	cairo_line_to(cr, 2.75, 7.5);
	cairo_arc(cr, 6, 7.5, 3.25, G_PI, 1.5 * G_PI);
	cairo_line_to(cr, 13, 4.25);
	cairo_stroke(cr);
	arrow_head(cr, 13, 4.25, 1, 0, 2.3);
	cairo_move_to(cr, 13.25, 7.25);
	cairo_line_to(cr, 13.25, 8.5);
	cairo_arc(cr, 10, 8.5, 3.25, 0, 0.5 * G_PI);
	cairo_line_to(cr, 3, 11.75);
	cairo_stroke(cr);
	arrow_head(cr, 3, 11.75, -1, 0, 2.3);
}

static void gl_repeat(cairo_t *cr)
{
	repeat_loop(cr);
}

static void gl_repeat_one(cairo_t *cr)
{
	repeat_loop(cr);
	/* "Once more": a small solid dot in the loop. */
	dot(cr, 8, 8, 1.35);
}

/* The speaker: a box and a cone, with `waves' arcs in front of it. */
static void speaker(cairo_t *cr, int waves)
{
	static const double p[] = { 2.25, 6.25, 4.75, 6.25, 8, 3.25,
				    8,	  12.75, 4.75, 9.75, 2.25, 9.75 };
	cairo_move_to(cr, p[0], p[1]);
	for (int i = 1; i < 6; i++)
		cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
	cairo_close_path(cr);
	cairo_stroke(cr);
	for (int i = 0; i < waves; i++) {
		double r = 2.5 + 2.25 * i;
		double a = i == 0 ? 0.85 : 0.75;
		cairo_new_sub_path(cr);
		cairo_arc(cr, 8.25, 8, r, -a, a);
		cairo_stroke(cr);
	}
}

static void gl_volume(cairo_t *cr)
{
	speaker(cr, 3);
}

static void gl_volume_medium(cairo_t *cr)
{
	speaker(cr, 2);
}

static void gl_volume_low(cairo_t *cr)
{
	speaker(cr, 1);
}

static void gl_volume_mute(cairo_t *cr)
{
	speaker(cr, 0);
	line(cr, 10.75, 6.25, 14.25, 9.75);
	line(cr, 14.25, 6.25, 10.75, 9.75);
}

/* ---- places of the sidebar -------------------------------------------- */

static void gl_music(cairo_t *cr)
{
	/* Two notes on one beam. */
	cairo_save(cr);
	cairo_translate(cr, 4.6, 11.9);
	cairo_scale(cr, 1.0, 0.8);
	dot(cr, 0, 0, 2.1);
	cairo_restore(cr);
	cairo_save(cr);
	cairo_translate(cr, 11.1, 10.4);
	cairo_scale(cr, 1.0, 0.8);
	dot(cr, 0, 0, 2.1);
	cairo_restore(cr);
	line(cr, 6.45, 11.6, 6.45, 4.4);
	line(cr, 12.95, 10.1, 12.95, 2.9);
	line(cr, 6.45, 4.4, 12.95, 2.9);
}

static void gl_videos(cairo_t *cr)
{
	/* A frame of film: the picture between two perforated edges. */
	rrect(cr, 2.25, 3, 13.75, 13, 2);
	line(cr, 5.25, 3, 5.25, 13);
	line(cr, 10.75, 3, 10.75, 13);
	line(cr, 2.25, 8, 5.25, 8);
	line(cr, 10.75, 8, 13.75, 8);
}

static void gl_recent(cairo_t *cr)
{
	circle(cr, 8, 8, 5.75);
	static const double p[] = { 8, 4.75, 8, 8, 10.5, 9.5 };
	poly(cr, p, 3);
}

static void gl_home(cairo_t *cr)
{
	static const double roof[] = { 2.25, 7.75, 8, 2.75, 13.75, 7.75 };
	poly(cr, roof, 3);
	static const double body[] = { 4, 6.5, 4, 13.25, 12, 13.25, 12, 6.5 };
	poly(cr, body, 4);
}

static void gl_now(cairo_t *cr)
{
	/* Level bars. */
	line(cr, 3.5, 12.5, 3.5, 8);
	line(cr, 6.5, 12.5, 6.5, 4);
	line(cr, 9.5, 12.5, 9.5, 6.5);
	line(cr, 12.5, 12.5, 12.5, 9.5);
}

static void gl_play_queue(cairo_t *cr)
{
	line(cr, 2.5, 4, 13.5, 4);
	line(cr, 2.5, 8, 8.5, 8);
	line(cr, 2.5, 12, 8.5, 12);
	static const double p[] = { 11, 8.75, 14, 10.5, 11, 12.25 };
	solid(cr, p, 3);
}

static void gl_playlist(cairo_t *cr)
{
	line(cr, 2.5, 4, 13.5, 4);
	line(cr, 2.5, 8, 8.5, 8);
	line(cr, 2.5, 12, 7, 12);
	cairo_save(cr);
	cairo_translate(cr, 11, 12.25);
	cairo_scale(cr, 1.0, 0.8);
	dot(cr, 0, 0, 1.75);
	cairo_restore(cr);
	static const double stem[] = { 12.6, 12, 12.6, 6.75, 14.25, 7.75 };
	poly(cr, stem, 3);
}

static void gl_settings(cairo_t *cr)
{
	/* Two sliders. */
	line(cr, 2.25, 4.75, 7.75, 4.75);
	line(cr, 12.25, 4.75, 13.75, 4.75);
	circle(cr, 10, 4.75, 2.1);
	line(cr, 2.25, 11.25, 3.75, 11.25);
	line(cr, 8.25, 11.25, 13.75, 11.25);
	circle(cr, 6, 11.25, 2.1);
}

static void gl_search(cairo_t *cr)
{
	circle(cr, 7, 7, 4.25);
	line(cr, 10.25, 10.25, 13.5, 13.5);
}

/* ---- window and page commands ----------------------------------------- */

static void gl_fullscreen(cairo_t *cr)
{
	static const double a[] = { 2.5, 6, 2.5, 2.5, 6, 2.5 };
	static const double b[] = { 10, 2.5, 13.5, 2.5, 13.5, 6 };
	static const double c[] = { 13.5, 10, 13.5, 13.5, 10, 13.5 };
	static const double d[] = { 6, 13.5, 2.5, 13.5, 2.5, 10 };
	poly(cr, a, 3);
	poly(cr, b, 3);
	poly(cr, c, 3);
	poly(cr, d, 3);
}

static void gl_leave_fullscreen(cairo_t *cr)
{
	static const double a[] = { 2.5, 6, 6, 6, 6, 2.5 };
	static const double b[] = { 10, 2.5, 10, 6, 13.5, 6 };
	static const double c[] = { 13.5, 10, 10, 10, 10, 13.5 };
	static const double d[] = { 6, 13.5, 6, 10, 2.5, 10 };
	poly(cr, a, 3);
	poly(cr, b, 3);
	poly(cr, c, 3);
	poly(cr, d, 3);
}

static void gl_mini_player(cairo_t *cr)
{
	rrect(cr, 1.75, 3, 14.25, 13, 2);
	rrect_fill(cr, 8, 8.25, 12.25, 11, 0.9);
}

static void gl_window_restore(cairo_t *cr)
{
	rrect(cr, 1.75, 3, 14.25, 13, 2);
	/* An arrow out of the corner: back to the full window. */
	line(cr, 6, 10, 10.5, 5.5);
	static const double p[] = { 7.25, 5.5, 10.5, 5.5, 10.5, 8.75 };
	poly(cr, p, 3);
}

static void gl_more(cairo_t *cr)
{
	dot(cr, 3.5, 8, 1.3);
	dot(cr, 8, 8, 1.3);
	dot(cr, 12.5, 8, 1.3);
}

static void gl_menu(cairo_t *cr)
{
	line(cr, 2.5, 4.5, 13.5, 4.5);
	line(cr, 2.5, 8, 13.5, 8);
	line(cr, 2.5, 11.5, 13.5, 11.5);
}

static void gl_sidebar(cairo_t *cr)
{
	/* A pane beside a page: folds the sidebar. */
	rrect(cr, 1.75, 3, 14.25, 13, 2);
	line(cr, 6, 3, 6, 13);
}

static void gl_back(cairo_t *cr)
{
	line(cr, 3, 8, 13, 8);
	static const double p[] = { 7.25, 3.75, 3, 8, 7.25, 12.25 };
	poly(cr, p, 3);
}

static void gl_chevron_down(cairo_t *cr)
{
	static const double p[] = { 4, 6, 8, 10, 12, 6 };
	poly(cr, p, 3);
}

static void gl_add(cairo_t *cr)
{
	line(cr, 8, 3, 8, 13);
	line(cr, 3, 8, 13, 8);
}

static void gl_close(cairo_t *cr)
{
	line(cr, 4, 4, 12, 12);
	line(cr, 12, 4, 4, 12);
}

static void folder_path(cairo_t *cr)
{
	static const double p[] = { 2.25, 12.75, 2.25, 3.5, 6.25, 3.5,
				    7.75, 5.25, 13.75, 5.25, 13.75, 12.75 };
	cairo_move_to(cr, p[0], p[1]);
	for (int i = 1; i < 6; i++)
		cairo_line_to(cr, p[2 * i], p[2 * i + 1]);
	cairo_close_path(cr);
	cairo_stroke(cr);
}

static void gl_folder(cairo_t *cr)
{
	folder_path(cr);
}

static void gl_folder_add(cairo_t *cr)
{
	folder_path(cr);
	line(cr, 8, 7.25, 8, 10.75);
	line(cr, 6.25, 9, 9.75, 9);
}

static void gl_open_folder(cairo_t *cr)
{
	static const double back[] = { 2.25, 12.75, 2.25, 3.5, 6.25, 3.5,
				       7.75, 5.25, 12.25, 5.25, 12.25, 7.5 };
	poly(cr, back, 6);
	static const double flap[] = { 2.25, 12.75, 4.25, 7.75, 14.5, 7.75,
				       12.5, 12.75 };
	cairo_move_to(cr, flap[0], flap[1]);
	for (int i = 1; i < 4; i++)
		cairo_line_to(cr, flap[2 * i], flap[2 * i + 1]);
	cairo_close_path(cr);
	cairo_stroke(cr);
}

static void gl_open(cairo_t *cr)
{
	/* A page with a folded corner. */
	static const double page[] = { 9.5, 2.25, 3.75, 2.25, 3.75, 13.75,
				       12.25, 13.75, 12.25, 5, 9.5, 2.25 };
	poly(cr, page, 6);
	static const double fold[] = { 9.5, 2.25, 9.5, 5, 12.25, 5 };
	poly(cr, fold, 3);
	static const double p[] = { 6.75, 7.5, 9.75, 9.25, 6.75, 11 };
	solid(cr, p, 3);
}

static void gl_network(cairo_t *cr)
{
	circle(cr, 8, 8, 5.75);
	cairo_save(cr);
	cairo_translate(cr, 8, 8);
	cairo_scale(cr, 0.45, 1.0);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 0, 0, 5.75, 0, 2 * G_PI);
	cairo_restore(cr);
	cairo_stroke(cr);
	line(cr, 2.25, 8, 13.75, 8);
}

static void gl_save_as(cairo_t *cr)
{
	line(cr, 8, 2.25, 8, 9.75);
	static const double head[] = { 4.75, 6.75, 8, 9.75, 11.25, 6.75 };
	poly(cr, head, 3);
	static const double tray[] = { 2.5, 10.25, 2.5, 13.5, 13.5, 13.5,
				       13.5, 10.25 };
	poly(cr, tray, 4);
}

static void gl_delete(cairo_t *cr)
{
	line(cr, 2.5, 4.5, 13.5, 4.5);
	static const double handle[] = { 6, 4.5, 6, 2.5, 10, 2.5, 10, 4.5 };
	poly(cr, handle, 4);
	static const double body[] = { 4, 4.5, 4.75, 13.5, 11.25, 13.5,
				       12, 4.5 };
	poly(cr, body, 4);
	line(cr, 6.75, 7.25, 6.75, 10.75);
	line(cr, 9.25, 7.25, 9.25, 10.75);
}

static void gl_refresh(cairo_t *cr)
{
	const double r = 5.5, a0 = 0.35, a1 = 1.62 * G_PI;
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 8, r, a0, a1);
	cairo_stroke(cr);
	double tx = 8 + r * cos(a1), ty = 8 + r * sin(a1);
	arrow_head(cr, tx, ty, -sin(a1), cos(a1), 2.6);
}

static void gl_info(cairo_t *cr)
{
	circle(cr, 8, 8, 5.75);
	line(cr, 8, 7.25, 8, 11);
	dot(cr, 8, 5, 0.95);
}

static void gl_closed_captions(cairo_t *cr)
{
	rrect(cr, 1.5, 3.5, 14.5, 12.5, 2.5);
	for (int i = 0; i < 2; i++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, 5.9 + 4.4 * i, 8, 1.9, 0.25 * G_PI,
			  1.75 * G_PI);
		cairo_stroke(cr);
	}
}

static void gl_user(cairo_t *cr)
{
	circle(cr, 8, 5.5, 2.75);
	cairo_new_sub_path(cr);
	cairo_arc(cr, 8, 14, 5.25, G_PI, 2 * G_PI);
	cairo_stroke(cr);
}

static const struct {
	const char *name;
	Painter paint;
} painters[] = {
	{ "likeos-play", gl_play },
	{ "likeos-pause", gl_pause },
	{ "likeos-next", gl_next },
	{ "likeos-previous", gl_previous },
	{ "likeos-shuffle", gl_shuffle },
	{ "likeos-repeat", gl_repeat },
	{ "likeos-repeat-one", gl_repeat_one },
	{ "likeos-volume", gl_volume },
	{ "likeos-volume-medium", gl_volume_medium },
	{ "likeos-volume-low", gl_volume_low },
	{ "likeos-volume-mute", gl_volume_mute },
	{ "likeos-music", gl_music },
	{ "likeos-videos", gl_videos },
	{ "likeos-recent", gl_recent },
	{ "likeos-home", gl_home },
	{ "likeos-now-playing", gl_now },
	{ "likeos-play-queue", gl_play_queue },
	{ "likeos-playlist", gl_playlist },
	{ "likeos-settings", gl_settings },
	{ "likeos-search", gl_search },
	{ "likeos-fullscreen", gl_fullscreen },
	{ "likeos-leave-fullscreen", gl_leave_fullscreen },
	{ "likeos-mini-player", gl_mini_player },
	{ "likeos-window-restore", gl_window_restore },
	{ "likeos-more", gl_more },
	{ "likeos-menu", gl_menu },
	{ "likeos-sidebar", gl_sidebar },
	{ "likeos-back", gl_back },
	{ "likeos-chevron-down", gl_chevron_down },
	{ "likeos-add", gl_add },
	{ "likeos-new", gl_add },
	{ "likeos-close", gl_close },
	{ "likeos-folder", gl_folder },
	{ "likeos-folder-add", gl_folder_add },
	{ "likeos-open-folder", gl_open_folder },
	{ "likeos-open", gl_open },
	{ "likeos-network", gl_network },
	{ "likeos-save-as", gl_save_as },
	{ "likeos-delete", gl_delete },
	{ "likeos-refresh", gl_refresh },
	{ "likeos-info", gl_info },
	{ "likeos-closed-captions", gl_closed_captions },
	{ "likeos-user", gl_user },
};

gboolean mp_glyph_known(const char *name)
{
	if (name == NULL)
		return FALSE;
	for (gsize i = 0; i < G_N_ELEMENTS(painters); i++)
		if (strcmp(painters[i].name, name) == 0)
			return TRUE;
	return FALSE;
}

gboolean mp_glyph_paint(cairo_t *cr, const char *name, double cx, double cy,
			double size)
{
	if (name == NULL || size < 4)
		return FALSE;
	for (gsize i = 0; i < G_N_ELEMENTS(painters); i++) {
		if (strcmp(painters[i].name, name) != 0)
			continue;
		double s = size / 16.0;
		/* 1.5 units at the small sizes; on a large placeholder the
		 * stroke grows more slowly than the glyph. */
		double px = size <= 24 ? 1.5 * s : MAX(2.25, 1.5 + size / 32.0);
		cairo_save(cr);
		/* Whole pixels for the box, so that 16-pixel strokes land on
		 * the same half pixels every time. */
		cairo_translate(cr, round(cx - size / 2.0),
				round(cy - size / 2.0));
		cairo_scale(cr, s, s);
		cairo_set_line_width(cr, px / s);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_new_path(cr);
		painters[i].paint(cr);
		cairo_restore(cr);
		return TRUE;
	}
	return FALSE;
}
