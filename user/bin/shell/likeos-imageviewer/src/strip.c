// LikeOS -- LikeOS Image Viewer: the bottom strip.
//
// A dark card floating over the bottom of the picture, inset from the
// window's edges.  At the left what the picture is -- the filmstrip and
// slideshow switches, its format as a chip, its size in pixels and on
// disk; in the middle the way to the previous and the next picture; at the
// right how it is shown -- the info panel, fit or actual size, the zoom as
// a menu of steps and as a slider, and full screen.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "canvas.h"

/* The slider runs over the zoom's logarithm, so every doubling is the same
 * distance. */
#define IV_SLIDER_MIN 0.02
#define IV_SLIDER_MAX 8.0
#define IV_SLIDER_STEPS 1000.0

struct IvStrip {
	IvWindow *w;
	GtkWidget *root;
	GtkWidget *b_film, *b_show, *chip, *dims, *size;
	GtkWidget *b_prev, *b_next;
	GtkWidget *b_info, *b_fit, *b_zoom, *b_zoom_out, *scale, *b_zoom_in;
	GtkWidget *b_full;
	gboolean syncing;
};

/* ---- the zoom menu ---------------------------------------------------------- */

static void zoom_item(GtkMenuItem *mi, IvCanvas *c)
{
	double z = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "iv-zoom")) /
		   1000.0;

	if (z <= 0)
		iv_canvas_zoom_fit(c, TRUE);
	else
		iv_canvas_zoom_to(c, z, -1, -1, TRUE);
}

static void add_zoom(GtkWidget *m, IvGlyph g, const char *label, double z,
		     IvCanvas *c, gboolean current)
{
	GtkWidget *mi = iv_menu_item(m, current ? IV_GLYPH_CHECK : g, label,
				     NULL, NULL, NULL);

	g_object_set_data(G_OBJECT(mi), "iv-zoom",
			  GINT_TO_POINTER((int)lround(z * 1000)));
	g_signal_connect(mi, "activate", G_CALLBACK(zoom_item), c);
}

void iv_zoom_menu_popup(IvCanvas *c, GtkWidget *button)
{
	static const int steps[] = { 25, 33, 50, 67, 100, 150, 200, 400, 800 };
	GtkWidget *m = iv_menu_new();
	double z = iv_canvas_get_zoom(c);
	gboolean fit = iv_canvas_is_fit(c);

	add_zoom(m, IV_GLYPH_FIT, "Fit to window", 0, c, fit);
	add_zoom(m, IV_GLYPH_ACTUAL_SIZE, "Actual size", 1.0, c,
		 !fit && fabs(z - 1.0) < 1e-3);
	iv_menu_separator(m);
	for (gsize i = 0; i < G_N_ELEMENTS(steps); i++) {
		char label[16];

		g_snprintf(label, sizeof label, "%d %%", steps[i]);
		add_zoom(m, IV_GLYPH_NONE, label, steps[i] / 100.0, c,
			 !fit && fabs(z * 100 - steps[i]) < 0.6);
	}
	/* Above a button in the bottom strip, below one in a top bar. */
	{
		GtkWidget *top = gtk_widget_get_toplevel(button);
		int x, y;

		gtk_widget_translate_coordinates(button, top, 0, 0, &x, &y);
		if (y > gtk_widget_get_allocated_height(top) / 2)
			iv_menu_popup_above(m, button);
		else
			iv_menu_popup_below(m, button);
	}
}

/* ---- updating ---------------------------------------------------------------- */

static double slider_to_zoom(double v)
{
	return exp(log(IV_SLIDER_MIN) +
		   v / IV_SLIDER_STEPS * (log(IV_SLIDER_MAX) - log(IV_SLIDER_MIN)));
}

static double zoom_to_slider(double z)
{
	z = CLAMP(z, IV_SLIDER_MIN, IV_SLIDER_MAX);
	return (log(z) - log(IV_SLIDER_MIN)) /
	       (log(IV_SLIDER_MAX) - log(IV_SLIDER_MIN)) * IV_SLIDER_STEPS;
}

void iv_strip_update_zoom(IvStrip *s)
{
	IvCanvas *c;
	double z;
	char buf[16];
	gboolean have;

	if (!s)
		return;
	c = s->w->canvas;
	z = iv_canvas_get_zoom(c);
	have = iv_canvas_has_image(c);
	g_snprintf(buf, sizeof buf, "%d %%", (int)lround(z * 100));
	gtk_label_set_text(GTK_LABEL(iv_text_button_label(s->b_zoom)),
			   have ? buf : "– %");
	s->syncing = TRUE;
	gtk_range_set_value(GTK_RANGE(s->scale), zoom_to_slider(z));
	s->syncing = FALSE;
	/* Fit shows the way to actual size and the other way round. */
	iv_glyph_button_set(s->b_fit, iv_canvas_is_fit(c) ? IV_GLYPH_ACTUAL_SIZE :
							    IV_GLYPH_FIT);
	gtk_widget_set_tooltip_text(s->b_fit, iv_canvas_is_fit(c) ?
						      "Actual size (1)" :
						      "Fit to window (0)");
	gtk_widget_set_sensitive(s->b_zoom, have);
	gtk_widget_set_sensitive(s->scale, have);
	gtk_widget_set_sensitive(s->b_zoom_in, have && z < IV_ZOOM_MAX * 0.99);
	gtk_widget_set_sensitive(s->b_zoom_out, have && z > IV_ZOOM_MIN * 1.01);
	gtk_widget_set_sensitive(s->b_fit, have);
}

static void set_toggle(IvStrip *s, GtkWidget *b, gboolean on)
{
	s->syncing = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), on);
	s->syncing = FALSE;
}

void iv_strip_update(IvStrip *s)
{
	IvWindow *w;
	IvPicture *p;

	if (!s)
		return;
	w = s->w;
	p = w->pic;
	if (p && !p->error) {
		char *dims = g_strdup_printf("%d × %d", p->width, p->height);
		char *size = iv_format_size(p->file_size);

		gtk_label_set_text(GTK_LABEL(s->chip), p->format);
		gtk_label_set_text(GTK_LABEL(s->dims), dims);
		gtk_label_set_text(GTK_LABEL(s->size), size);
		gtk_widget_show(s->chip);
		g_free(dims);
		g_free(size);
	} else {
		gtk_label_set_text(GTK_LABEL(s->dims), "");
		gtk_label_set_text(GTK_LABEL(s->size), "");
		gtk_widget_hide(s->chip);
	}
	gtk_widget_set_sensitive(s->b_prev, iv_viewer_can_go(w, -1));
	gtk_widget_set_sensitive(s->b_next, iv_viewer_can_go(w, 1));
	gtk_widget_set_sensitive(s->b_show, iv_folder_count(w->folder) > 0);
	set_toggle(s, s->b_film, w->film_visible);
	set_toggle(s, s->b_info, w->info_visible);
	iv_glyph_button_set(s->b_show, w->slideshow && !w->slideshow_paused ?
					       IV_GLYPH_PAUSE :
				       w->slideshow ? IV_GLYPH_PLAY :
						      IV_GLYPH_SLIDESHOW);
	gtk_widget_set_tooltip_text(s->b_show, !w->slideshow ? "Slideshow (F5)" :
					       w->slideshow_paused ?
					       "Go on (Space)" :
					       "Pause (Space)");
	iv_glyph_button_set(s->b_full, w->fullscreen ? IV_GLYPH_LEAVE_FULLSCREEN :
						       IV_GLYPH_FULLSCREEN);
	gtk_widget_set_tooltip_text(s->b_full, w->fullscreen ?
						       "Leave full screen (F11)" :
						       "Full screen (F11)");
	iv_strip_update_zoom(s);
}

/* ---- the controls ------------------------------------------------------------- */

static void film_toggled(GtkToggleButton *b, IvStrip *s)
{
	if (!s->syncing)
		iv_viewer_toggle_filmstrip(s->w);
}

static void info_toggled(GtkToggleButton *b, IvStrip *s)
{
	if (!s->syncing)
		iv_viewer_set_info(s->w, gtk_toggle_button_get_active(b));
}

static void show_clicked(GtkButton *b, IvStrip *s)
{
	if (s->w->slideshow)
		iv_viewer_slideshow_toggle_pause(s->w);
	else
		iv_viewer_slideshow(s->w, TRUE);
}

static void prev_clicked(GtkButton *b, IvStrip *s)
{
	iv_viewer_go(s->w, -1);
}

static void next_clicked(GtkButton *b, IvStrip *s)
{
	iv_viewer_go(s->w, 1);
}

static void fit_clicked(GtkButton *b, IvStrip *s)
{
	iv_canvas_toggle_fit(s->w->canvas, -1, -1);
}

static void zoom_clicked(GtkButton *b, IvStrip *s)
{
	iv_zoom_menu_popup(s->w->canvas, GTK_WIDGET(b));
}

static void zoom_in_clicked(GtkButton *b, IvStrip *s)
{
	iv_canvas_zoom_step(s->w->canvas, 1, -1, -1);
}

static void zoom_out_clicked(GtkButton *b, IvStrip *s)
{
	iv_canvas_zoom_step(s->w->canvas, -1, -1, -1);
}

static void scale_moved(GtkRange *r, IvStrip *s)
{
	if (s->syncing)
		return;
	iv_canvas_zoom_to(s->w->canvas, slider_to_zoom(gtk_range_get_value(r)),
			  -1, -1, FALSE);
}

static void full_clicked(GtkButton *b, IvStrip *s)
{
	iv_window_toggle_fullscreen(s->w);
}

static GtkWidget *label(const char *cls)
{
	GtkWidget *l = gtk_label_new(NULL);

	iv_add_class(l, cls);
	gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
	return l;
}

static GtkWidget *button(IvGlyph g, const char *tip, GCallback cb, IvStrip *s,
			 gboolean toggle)
{
	GtkWidget *b = toggle ? iv_glyph_toggle(g, 16, tip, "iv-strip-button") :
				iv_glyph_button(g, 16, tip, "iv-strip-button");

	g_signal_connect(b, toggle ? "toggled" : "clicked", cb, s);
	return b;
}

IvStrip *iv_strip_new(IvWindow *w)
{
	IvStrip *s = g_new0(IvStrip, 1);
	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	GtkWidget *facts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

	s->w = w;
	s->root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	iv_add_class(s->root, "iv-strip");

	s->b_film = button(IV_GLYPH_FILMSTRIP, "Filmstrip (T)",
			   G_CALLBACK(film_toggled), s, TRUE);
	s->b_show = button(IV_GLYPH_SLIDESHOW, "Slideshow (F5)",
			   G_CALLBACK(show_clicked), s, FALSE);
	gtk_box_pack_start(GTK_BOX(left), s->b_film, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(left), s->b_show, FALSE, FALSE, 0);
	s->chip = label("iv-chip-label");
	gtk_widget_set_no_show_all(s->chip, TRUE);
	s->dims = label("iv-strip-fact");
	s->size = label("iv-strip-fact");
	gtk_widget_set_margin_start(facts, 8);
	gtk_box_pack_start(GTK_BOX(facts), s->chip, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(facts), s->dims, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(facts), s->size, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(left), facts, FALSE, FALSE, 0);

	s->b_prev = button(IV_GLYPH_PREVIOUS, "Previous picture (←)",
			   G_CALLBACK(prev_clicked), s, FALSE);
	s->b_next = button(IV_GLYPH_NEXT, "Next picture (→)",
			   G_CALLBACK(next_clicked), s, FALSE);
	iv_add_class(s->b_prev, "iv-nav-button");
	iv_add_class(s->b_next, "iv-nav-button");
	gtk_box_pack_start(GTK_BOX(mid), s->b_prev, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(mid), s->b_next, FALSE, FALSE, 0);

	s->b_info = button(IV_GLYPH_INFO, "Info (I)", G_CALLBACK(info_toggled), s,
			   TRUE);
	s->b_fit = button(IV_GLYPH_ACTUAL_SIZE, "Actual size (1)",
			  G_CALLBACK(fit_clicked), s, FALSE);
	s->b_zoom = iv_text_button(IV_GLYPH_NONE, "100 %", TRUE, "iv-strip-button");
	iv_add_class(s->b_zoom, "iv-zoom-button");
	gtk_widget_set_tooltip_text(s->b_zoom, "Zoom");
	g_signal_connect(s->b_zoom, "clicked", G_CALLBACK(zoom_clicked), s);
	s->b_zoom_out = button(IV_GLYPH_ZOOM_OUT, "Zoom out (−)",
			       G_CALLBACK(zoom_out_clicked), s, FALSE);
	s->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0,
					    IV_SLIDER_STEPS, 1);
	gtk_scale_set_draw_value(GTK_SCALE(s->scale), FALSE);
	gtk_widget_set_size_request(s->scale, 120, -1);
	gtk_widget_set_can_focus(s->scale, FALSE);
	gtk_widget_set_valign(s->scale, GTK_ALIGN_CENTER);
	iv_add_class(s->scale, "iv-zoom-scale");
	g_signal_connect(s->scale, "value-changed", G_CALLBACK(scale_moved), s);
	s->b_zoom_in = button(IV_GLYPH_ZOOM_IN, "Zoom in (+)",
			      G_CALLBACK(zoom_in_clicked), s, FALSE);
	s->b_full = button(IV_GLYPH_FULLSCREEN, "Full screen (F11)",
			   G_CALLBACK(full_clicked), s, FALSE);
	gtk_box_pack_start(GTK_BOX(right), s->b_info, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), s->b_fit, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), s->b_zoom, FALSE, FALSE, 4);
	gtk_box_pack_start(GTK_BOX(right), s->b_zoom_out, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), s->scale, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), s->b_zoom_in, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), s->b_full, FALSE, FALSE, 4);

	gtk_box_pack_start(GTK_BOX(s->root), left, FALSE, FALSE, 0);
	gtk_box_set_center_widget(GTK_BOX(s->root), mid);
	gtk_box_pack_end(GTK_BOX(s->root), right, FALSE, FALSE, 0);
	gtk_widget_show_all(s->root);
	return s;
}

void iv_strip_free(IvStrip *s)
{
	g_free(s);
}

GtkWidget *iv_strip_widget(IvStrip *s)
{
	return s->root;
}
