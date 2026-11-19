/* LikeOS installer -- the wizard (see wizard.h).
 *
 * The window holds one stack of scenes; each scene has its own backdrop,
 * its own frame (panel, card, or the whole screen) and an inner stack of
 * the pages drawn in that style.  Moving to a page in another scene
 * cross-fades the scenes; moving inside a scene slides (Setup panel) or
 * fades (card) the inner stack, so the frame itself stays put.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "wizard.h"
#include "autopilot.h"
#include "keyboard.h"
#include "layout.h"
#include "sheet.h"
#include "widgets/widgets.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TRANSITION_MS 260

struct Wizard {
	App *app;
	GtkWidget *root;                    /* stack of scenes */
	GtkWidget *scenes[SCENE_COUNT];
	GtkWidget *inner[SCENE_COUNT];
	GtkWidget *frames[SCENE_COUNT];     /* FrameBin per scene (or NULL) */
	Page pages[PAGE_COUNT];
	int current;
	GArray *history;

	/* Setup panel */
	GtkWidget *setup_back, *setup_next, *setup_close;

	/* card */
	GtkWidget *card_art_col, *card_art;
	GtkWidget *card_back, *card_next, *card_secondary;
	GtkWidget *tray_kbd_label, *tray_kbd, *tray_power;
};

static Wizard *g_wiz;

/* ---- backdrops ----------------------------------------------------- */

static void glow(cairo_t *cr, double x, double y, double r, double red,
		 double green, double blue, double alpha)
{
	cairo_pattern_t *p = cairo_pattern_create_radial(x, y, 0, x, y, r);

	cairo_pattern_add_color_stop_rgba(p, 0, red, green, blue, alpha);
	cairo_pattern_add_color_stop_rgba(p, 0.55, red, green, blue, alpha * 0.4);
	cairo_pattern_add_color_stop_rgba(p, 1, red, green, blue, 0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
}

static gboolean draw_setup_backdrop(GtkWidget *w, cairo_t *cr, gpointer data)
{
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	cairo_pattern_t *p;

	(void)data;
	p = cairo_pattern_create_linear(0, 0, W, H);
	cairo_pattern_add_color_stop_rgb(p, 0.0, 0x0a / 255.0, 0x13 / 255.0,
					 0x2c / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 0.55, 0x0f / 255.0, 0x26 / 255.0,
					 0x55 / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 1.0, 0x0b / 255.0, 0x37 / 255.0,
					 0x66 / 255.0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);

	glow(cr, W * 0.82, H * 0.12, MAX(W, H) * 0.55, 0.12, 0.70, 0.78, 0.16);
	glow(cr, W * 0.10, H * 0.92, MAX(W, H) * 0.60, 0.23, 0.45, 0.94, 0.22);

	/* the ring of the mark, very faint, in the lower right */
	cairo_set_line_width(cr, H * 0.09);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.025);
	cairo_arc(cr, W * 0.88, H * 0.86, H * 0.36, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_set_line_width(cr, 1.0);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.04);
	cairo_arc(cr, W * 0.88, H * 0.86, H * 0.52, 0, 2 * M_PI);
	cairo_stroke(cr);
	return FALSE;
}

static gboolean draw_card_backdrop(GtkWidget *w, cairo_t *cr, gpointer data)
{
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	double M = MAX(W, H);
	cairo_pattern_t *p;

	(void)data;
	p = cairo_pattern_create_linear(0, 0, W, H);
	cairo_pattern_add_color_stop_rgb(p, 0, 0xee / 255.0, 0xf3 / 255.0,
					 0xfb / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 1, 0xe2 / 255.0, 0xe9 / 255.0,
					 0xf6 / 255.0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);

	glow(cr, W * 0.06, H * 0.95, M * 0.48, 0.66, 0.78, 0.99, 0.85);
	glow(cr, W * 0.95, H * 0.08, M * 0.40, 0.72, 0.93, 0.89, 0.75);
	glow(cr, W * 0.70, H * 1.05, M * 0.38, 0.80, 0.76, 0.98, 0.65);
	glow(cr, W * 0.40, H * -0.05, M * 0.35, 1.0, 1.0, 1.0, 0.7);
	return FALSE;
}

static gboolean draw_accent_backdrop(GtkWidget *w, cairo_t *cr, gpointer data)
{
	double W = gtk_widget_get_allocated_width(w);
	double H = gtk_widget_get_allocated_height(w);
	cairo_pattern_t *p;

	(void)data;
	p = cairo_pattern_create_linear(0, 0, W, H);
	cairo_pattern_add_color_stop_rgb(p, 0, 0x14 / 255.0, 0x36 / 255.0,
					 0x8a / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 1, 0x22 / 255.0, 0x52 / 255.0,
					 0xc6 / 255.0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
	glow(cr, W * 0.92, H * 0.10, MAX(W, H) * 0.5, 0.36, 0.56, 0.98, 0.35);
	glow(cr, W * 0.05, H * 1.0, MAX(W, H) * 0.5, 0.05, 0.16, 0.42, 0.45);
	return FALSE;
}

static gboolean draw_splash_backdrop(GtkWidget *w, cairo_t *cr, gpointer data)
{
	(void)data;
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 0, 0, gtk_widget_get_allocated_width(w),
			gtk_widget_get_allocated_height(w));
	cairo_fill(cr);
	return FALSE;
}

static GtkWidget *scene_box(const char *css_class,
			    gboolean (*painter)(GtkWidget *, cairo_t *, gpointer))
{
	GtkWidget *ov = gtk_overlay_new();

	gtk_widget_set_app_paintable(ov, TRUE);
	g_signal_connect(ov, "draw", G_CALLBACK(painter), NULL);
	ui_add_class(ov, "il-scene");
	ui_add_class(ov, css_class);
	return ov;
}

static GtkWidget *inner_stack(GtkStackTransitionType type)
{
	GtkWidget *s = gtk_stack_new();

	gtk_stack_set_transition_type(GTK_STACK(s), type);
	gtk_stack_set_transition_duration(GTK_STACK(s), TRANSITION_MS);
	gtk_stack_set_homogeneous(GTK_STACK(s), FALSE);
	gtk_widget_set_vexpand(s, TRUE);
	gtk_widget_set_hexpand(s, TRUE);
	return s;
}

/* ---- the Setup panel ----------------------------------------------- */

static void on_next_clicked(GtkButton *b, gpointer data)
{
	(void)b;
	wizard_next(data);
}

static void on_back_clicked(GtkButton *b, gpointer data)
{
	(void)b;
	wizard_back(data);
}

static void on_secondary_clicked(GtkButton *b, gpointer data)
{
	Wizard *w = data;
	Page *p = wizard_current(w);

	(void)b;
	if (p && p->secondary)
		p->secondary(p);
}

static void on_close_clicked(GtkButton *b, gpointer data)
{
	Wizard *w = data;

	(void)b;
	sheet_power(w->app);
}

static void on_help_clicked(GtkButton *b, gpointer data)
{
	Wizard *w = data;
	GBytes *bytes;

	(void)b;
	bytes = g_resources_lookup_data("/org/likeos/installer/help.txt", 0, NULL);
	if (bytes) {
		/* the file's own comment lines are not part of the text */
		char **lines = g_strsplit(g_bytes_get_data(bytes, NULL), "\n", -1);
		GString *text = g_string_new(NULL);

		for (char **l = lines; *l; l++) {
			if (**l == '#')
				continue;
			g_string_append(text, *l);
			g_string_append_c(text, '\n');
		}
		sheet_show_text(w->app, "Help with LikeOS Setup", text->str, FALSE);
		g_string_free(text, TRUE);
		g_strfreev(lines);
		g_bytes_unref(bytes);
	} else {
		sheet_show_text(w->app, "Help with LikeOS Setup",
				"Help is not available.", FALSE);
	}
}

static void on_license_clicked(GtkButton *b, gpointer data)
{
	Wizard *w = data;
	GBytes *bytes;
	char *text;

	(void)b;
	bytes = g_resources_lookup_data("/org/likeos/installer/COPYING", 0, NULL);
	text = g_strdup_printf(
		"LikeOS is free software. You can redistribute it and/or modify "
		"it under the terms of the GNU General Public License, version 2, "
		"as published by the Free Software Foundation. The programs that "
		"come with LikeOS are covered by their own licences, which are "
		"installed with them.\n\n%s",
		bytes ? (const char *)g_bytes_get_data(bytes, NULL) : "");
	sheet_show_text(w->app, "License terms", text, TRUE);
	g_free(text);
	if (bytes)
		g_bytes_unref(bytes);
}

static GtkWidget *logo_image(App *app, int size)
{
	GdkPixbuf *pb = app_logo(app, size);
	GtkWidget *img;

	if (pb) {
		img = gtk_image_new_from_pixbuf(pb);
		g_object_unref(pb);
	} else {
		img = art_view_new(ART_LOGO);
		art_view_set_animated(img, FALSE);
		gtk_widget_set_size_request(img, size, size);
	}
	return img;
}

static GtkWidget *build_setup_scene(Wizard *w)
{
	GtkWidget *scene = scene_box("il-scene-setup", draw_setup_backdrop);
	GtkWidget *frame = frame_bin_new(0.72, 0.90, 980, 760);
	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *l, *b;

	frame_bin_set_margins(FRAME_BIN(frame), 32, 0);
	ui_add_class(panel, "il-setup-panel");

	/* title bar */
	ui_add_class(title, "il-setup-titlebar");
	gtk_box_pack_start(GTK_BOX(title), logo_image(w->app, 16), FALSE, FALSE, 0);
	l = gtk_label_new("LikeOS Setup");
	ui_add_class(l, "il-setup-title");
	gtk_box_pack_start(GTK_BOX(title), l, FALSE, FALSE, 0);
	w->setup_close = gtk_button_new();
	gtk_container_add(GTK_CONTAINER(w->setup_close), icon_new(ICON_CLOSE, 14));
	gtk_button_set_relief(GTK_BUTTON(w->setup_close), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(w->setup_close, "Leave setup");
	gtk_widget_set_can_focus(w->setup_close, FALSE);
	ui_add_class(w->setup_close, "il-caption-close");
	g_signal_connect(w->setup_close, "clicked", G_CALLBACK(on_close_clicked), w);
	gtk_box_pack_end(GTK_BOX(title), w->setup_close, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(panel), title, FALSE, FALSE, 0);

	/* pages */
	w->inner[SCENE_SETUP] = inner_stack(GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	ui_add_class(w->inner[SCENE_SETUP], "il-setup-pages");
	gtk_box_pack_start(GTK_BOX(panel), w->inner[SCENE_SETUP], TRUE, TRUE, 0);

	/* footer: brand, links, buttons */
	ui_add_class(footer, "il-setup-footer");
	gtk_box_pack_start(GTK_BOX(brand), logo_image(w->app, 22), FALSE, FALSE, 0);
	l = gtk_label_new("LikeOS");
	ui_add_class(l, "il-brand");
	gtk_box_pack_start(GTK_BOX(brand), l, FALSE, FALSE, 0);
	ui_add_class(brand, "il-brand-box");
	gtk_box_pack_start(GTK_BOX(footer), brand, FALSE, FALSE, 0);
	b = link_button_new("_Help", ICON_NONE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_help_clicked), w);
	gtk_box_pack_start(GTK_BOX(footer), b, FALSE, FALSE, 0);
	b = link_button_new("_License", ICON_NONE);
	g_signal_connect(b, "clicked", G_CALLBACK(on_license_clicked), w);
	gtk_box_pack_start(GTK_BOX(footer), b, FALSE, FALSE, 0);

	w->setup_next = ui_button("_Next", "il-primary");
	gtk_widget_set_can_default(w->setup_next, TRUE);
	g_signal_connect(w->setup_next, "clicked", G_CALLBACK(on_next_clicked), w);
	gtk_box_pack_end(GTK_BOX(footer), w->setup_next, FALSE, FALSE, 0);
	w->setup_back = ui_button("_Back", NULL);
	g_signal_connect(w->setup_back, "clicked", G_CALLBACK(on_back_clicked), w);
	gtk_box_pack_end(GTK_BOX(footer), w->setup_back, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(panel), footer, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(frame), panel);
	gtk_container_add(GTK_CONTAINER(scene), frame);
	w->frames[SCENE_SETUP] = frame;
	return scene;
}

/* ---- the card ------------------------------------------------------ */

static void kbd_switch(GtkButton *b, gpointer data)
{
	Wizard *w = data;
	int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "layout-index"));
	GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
	char *code;

	keyboard_apply_one(w->app, w->app->cfg.layouts[i], w->app->cfg.variants[i]);
	code = keyboard_short_code(w->app->cfg.layouts[i]);
	gtk_label_set_text(GTK_LABEL(w->tray_kbd_label), code);
	g_free(code);
	if (pop)
		gtk_popover_popdown(GTK_POPOVER(pop));
}

static void on_tray_kbd(GtkButton *button, gpointer data)
{
	Wizard *w = data;
	GtkWidget *pop = gtk_popover_new(GTK_WIDGET(button));
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *l = ui_label("Type with", "il-popover-title");

	ui_add_class(pop, "il-popover");
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	for (int i = 0; i < w->app->cfg.nlayouts; i++) {
		char *name = app_layout_label(w->app, w->app->cfg.layouts[i],
					      w->app->cfg.variants[i]);
		GtkWidget *b = gtk_button_new_with_label(name);

		gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
		gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(b)), GTK_ALIGN_START);
		ui_add_class(b, "il-popover-item");
		g_object_set_data(G_OBJECT(b), "layout-index", GINT_TO_POINTER(i));
		g_signal_connect(b, "clicked", G_CALLBACK(kbd_switch), w);
		gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
		g_free(name);
	}
	if (w->app->cfg.nlayouts > 1) {
		l = ui_label("Alt+Shift switches between them.", "il-popover-hint");
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(pop), box);
	gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_TOP);
	gtk_widget_show_all(box);
	g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_tray_power(GtkButton *b, gpointer data)
{
	Wizard *w = data;

	(void)b;
	sheet_power(w->app);
}

static GtkWidget *build_tray(Wizard *w)
{
	GtkWidget *tray = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	ui_add_class(tray, "il-tray");
	w->tray_kbd = gtk_button_new();
	gtk_button_set_relief(GTK_BUTTON(w->tray_kbd), GTK_RELIEF_NONE);
	gtk_box_pack_start(GTK_BOX(box), icon_new(ICON_KEYBOARD, 18), FALSE, FALSE, 0);
	w->tray_kbd_label = gtk_label_new("US");
	gtk_box_pack_start(GTK_BOX(box), w->tray_kbd_label, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(w->tray_kbd), box);
	gtk_widget_set_tooltip_text(w->tray_kbd, "Keyboard layout");
	ui_add_class(w->tray_kbd, "il-tray-button");
	g_signal_connect(w->tray_kbd, "clicked", G_CALLBACK(on_tray_kbd), w);
	gtk_box_pack_start(GTK_BOX(tray), w->tray_kbd, FALSE, FALSE, 0);

	w->tray_power = gtk_button_new();
	gtk_button_set_relief(GTK_BUTTON(w->tray_power), GTK_RELIEF_NONE);
	gtk_container_add(GTK_CONTAINER(w->tray_power), icon_new(ICON_POWER, 18));
	gtk_widget_set_tooltip_text(w->tray_power, "Restart or shut down");
	ui_add_class(w->tray_power, "il-tray-button");
	g_signal_connect(w->tray_power, "clicked", G_CALLBACK(on_tray_power), w);
	gtk_box_pack_start(GTK_BOX(tray), w->tray_power, FALSE, FALSE, 0);

	gtk_widget_set_halign(tray, GTK_ALIGN_END);
	gtk_widget_set_valign(tray, GTK_ALIGN_END);
	return tray;
}

static GtkWidget *build_card_scene(Wizard *w)
{
	GtkWidget *scene = scene_box("il-scene-card", draw_card_backdrop);
	GtkWidget *frame = frame_bin_new(0.92, 0.92, 1180, 800);
	GtkWidget *card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *card = gtk_overlay_new();
	GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

	frame_bin_set_margins(FRAME_BIN(frame), 36, 44);
	ui_add_class(card_box, "il-card");

	w->card_art_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	ui_add_class(w->card_art_col, "il-card-art");
	w->card_art = art_view_new(ART_NONE);
	gtk_widget_set_vexpand(w->card_art, TRUE);
	/* the column keeps the width wizard_relayout() gives it; the rest of
	 * the card goes to the questions */
	gtk_widget_set_hexpand(w->card_art, FALSE);
	gtk_widget_set_hexpand(w->card_art_col, FALSE);
	gtk_box_pack_start(GTK_BOX(w->card_art_col), w->card_art, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(cols), w->card_art_col, FALSE, FALSE, 0);

	ui_add_class(right, "il-card-content");
	gtk_widget_set_hexpand(right, TRUE);
	w->inner[SCENE_CARD] = inner_stack(GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_box_pack_start(GTK_BOX(right), w->inner[SCENE_CARD], TRUE, TRUE, 0);

	ui_add_class(buttons, "il-card-buttons");
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	w->card_next = ui_button("_Next", "il-primary");
	gtk_widget_set_can_default(w->card_next, TRUE);
	g_signal_connect(w->card_next, "clicked", G_CALLBACK(on_next_clicked), w);
	w->card_secondary = link_button_new("_Skip", ICON_NONE);
	ui_add_class(w->card_secondary, "il-secondary-link");
	g_signal_connect(w->card_secondary, "clicked",
			 G_CALLBACK(on_secondary_clicked), w);
	gtk_box_pack_end(GTK_BOX(buttons), w->card_next, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(buttons), w->card_secondary, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(right), buttons, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(cols), right, TRUE, TRUE, 0);

	gtk_container_add(GTK_CONTAINER(card), cols);

	w->card_back = gtk_button_new();
	gtk_container_add(GTK_CONTAINER(w->card_back), icon_new(ICON_BACK, 18));
	gtk_button_set_relief(GTK_BUTTON(w->card_back), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(w->card_back, "Back (Alt+B)");
	gtk_widget_set_halign(w->card_back, GTK_ALIGN_START);
	gtk_widget_set_valign(w->card_back, GTK_ALIGN_START);
	ui_add_class(w->card_back, "il-card-back");
	g_signal_connect(w->card_back, "clicked", G_CALLBACK(on_back_clicked), w);
	gtk_overlay_add_overlay(GTK_OVERLAY(card), w->card_back);

	gtk_box_pack_start(GTK_BOX(card_box), card, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(frame), card_box);
	gtk_container_add(GTK_CONTAINER(scene), frame);
	gtk_overlay_add_overlay(GTK_OVERLAY(scene), build_tray(w));
	w->frames[SCENE_CARD] = frame;
	return scene;
}

static GtkWidget *build_plain_scene(Wizard *w, SceneKind kind)
{
	GtkWidget *scene;

	if (kind == SCENE_SPLASH)
		scene = scene_box("il-scene-splash", draw_splash_backdrop);
	else
		scene = scene_box("il-scene-accent", draw_accent_backdrop);
	w->inner[kind] = inner_stack(GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_container_add(GTK_CONTAINER(scene), w->inner[kind]);
	return scene;
}

/* ---- pages --------------------------------------------------------- */

static void (*const g_page_init[PAGE_COUNT])(Page *) = {
	[PAGE_SPLASH] = page_splash_init,
	[PAGE_LANGUAGE] = page_language_init,
	[PAGE_KEYBOARD] = page_keyboard_init,
	[PAGE_PREPARING] = page_preparing_init,
	[PAGE_DISK] = page_disk_init,
	[PAGE_REGION] = page_region_init,
	[PAGE_LAYOUT] = page_layout_init,
	[PAGE_HOSTNAME] = page_hostname_init,
	[PAGE_ROOT] = page_root_init,
	[PAGE_ACCOUNT] = page_account_init,
	[PAGE_SUMMARY] = page_summary_init,
	[PAGE_INSTALLING] = page_installing_init,
	[PAGE_FINISHED] = page_finished_init,
	[PAGE_ERROR] = page_error_init,
};

Wizard *wizard_new(App *app)
{
	Wizard *w = g_new0(Wizard, 1);

	g_wiz = w;
	w->app = app;
	w->current = -1;
	w->history = g_array_new(FALSE, FALSE, sizeof(int));

	w->root = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(w->root),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(w->root), 380);
	ui_add_class(w->root, "il-root");

	w->scenes[SCENE_SPLASH] = build_plain_scene(w, SCENE_SPLASH);
	w->scenes[SCENE_SETUP] = build_setup_scene(w);
	w->scenes[SCENE_CARD] = build_card_scene(w);
	w->scenes[SCENE_ACCENT] = build_plain_scene(w, SCENE_ACCENT);
	gtk_stack_add_named(GTK_STACK(w->root), w->scenes[SCENE_SPLASH], "splash");
	gtk_stack_add_named(GTK_STACK(w->root), w->scenes[SCENE_SETUP], "setup");
	gtk_stack_add_named(GTK_STACK(w->root), w->scenes[SCENE_CARD], "card");
	gtk_stack_add_named(GTK_STACK(w->root), w->scenes[SCENE_ACCENT], "accent");

	for (int i = 0; i < PAGE_COUNT; i++) {
		Page *p = &w->pages[i];

		p->index = i;
		p->app = app;
		p->next_ok = TRUE;
		p->next_visible = TRUE;
		p->back_ok = TRUE;
		p->next_index = -1;
		g_page_init[i](p);
	}
	return w;
}

GtkWidget *wizard_widget(Wizard *w)
{
	return w->root;
}

Page *wizard_page(Wizard *w, int index)
{
	if (index < 0 || index >= PAGE_COUNT)
		return NULL;
	return &w->pages[index];
}

Page *wizard_current(Wizard *w)
{
	return wizard_page(w, w->current);
}

int wizard_current_index(Wizard *w)
{
	return w->current;
}

GtkWidget *wizard_scene(Wizard *w, SceneKind kind)
{
	return w->scenes[kind];
}

static gboolean history_has_back(Wizard *w)
{
	return w->history->len > 0;
}

static void refresh_chrome(Wizard *w, Page *p)
{
	const char *next = p->next_label ? p->next_label : "_Next";
	gboolean can_back = p->back_ok && (history_has_back(w) || p->substep > 0);

	switch (p->scene) {
	case SCENE_SETUP:
		gtk_button_set_label(GTK_BUTTON(w->setup_next), next);
		gtk_widget_set_sensitive(w->setup_next, p->next_ok);
		gtk_widget_set_visible(w->setup_next, p->next_visible);
		gtk_widget_set_sensitive(w->setup_back, can_back);
		gtk_widget_set_visible(w->setup_back, TRUE);
		if (p->next_ok && p->next_visible)
			gtk_widget_grab_default(w->setup_next);
		break;
	case SCENE_CARD:
		gtk_button_set_label(GTK_BUTTON(w->card_next), next);
		gtk_widget_set_sensitive(w->card_next, p->next_ok);
		gtk_widget_set_visible(w->card_next, p->next_visible);
		gtk_widget_set_visible(w->card_back, can_back);
		if (p->secondary_label) {
			link_button_set_label(w->card_secondary, p->secondary_label);
			gtk_widget_show(w->card_secondary);
		} else {
			gtk_widget_hide(w->card_secondary);
		}
		art_view_set(w->card_art, p->art);
		gtk_widget_set_visible(w->card_art_col,
				       !w->app->narrow && p->art != ART_NONE);
		if (p->next_ok && p->next_visible)
			gtk_widget_grab_default(w->card_next);
		/* the tray's keyboard code */
		{
			char *code = keyboard_short_code(w->app->cfg.layouts[0]);

			gtk_label_set_text(GTK_LABEL(w->tray_kbd_label), code);
			g_free(code);
		}
		break;
	default:
		break;
	}
}

void page_update(Page *p)
{
	if (g_wiz && g_wiz->current == p->index)
		refresh_chrome(g_wiz, p);
}

void page_set_focus(Page *p, GtkWidget *widget)
{
	if (p->root)
		g_object_set_data(G_OBJECT(p->root), "focus-widget", widget);
}

static gboolean focus_later_idle(gpointer data)
{
	GtkWidget *w = data;

	if (gtk_widget_get_mapped(w) && gtk_widget_is_sensitive(w))
		gtk_widget_grab_focus(w);
	g_object_unref(w);
	return G_SOURCE_REMOVE;
}

void page_focus_later(GtkWidget *widget)
{
	g_timeout_add(TRANSITION_MS + 140, focus_later_idle, g_object_ref(widget));
}

/* Whether a widget is on screen without scrolling its page: focusing one
 * further down would scroll the heading away on a small screen. */
static gboolean widget_in_view(GtkWidget *w, GtkWidget *root)
{
	int x, y;

	if (!gtk_widget_translate_coordinates(w, root, 0, 0, &x, &y))
		return TRUE;
	/* the top of it is enough (a long list scrolls by itself) */
	return y + MIN(gtk_widget_get_allocated_height(w), 48) <=
		gtk_widget_get_allocated_height(root);
}

static gboolean focus_idle(gpointer data)
{
	Wizard *w = data;
	Page *p = wizard_current(w);
	GtkWidget *f;

	if (!p || !p->root)
		return G_SOURCE_REMOVE;
	/* the copy screen and the splash take no keyboard focus: a focused
	 * button would scroll a small screen away from the title */
	if (p->scene == SCENE_ACCENT || p->scene == SCENE_SPLASH)
		return G_SOURCE_REMOVE;
	f = g_object_get_data(G_OBJECT(p->root), "focus-widget");
	if (f && !widget_in_view(f, p->root))
		f = NULL;
	if (f && gtk_widget_get_mapped(f) && gtk_widget_is_sensitive(f))
		gtk_widget_grab_focus(f);
	else if (p->scene == SCENE_SETUP && gtk_widget_is_sensitive(w->setup_next))
		gtk_widget_grab_focus(w->setup_next);
	else if (p->scene == SCENE_CARD && gtk_widget_get_visible(w->card_next) &&
		 gtk_widget_is_sensitive(w->card_next))
		gtk_widget_grab_focus(w->card_next);
	else
		gtk_widget_child_focus(p->root, GTK_DIR_TAB_FORWARD);
	return G_SOURCE_REMOVE;
}

void wizard_go(Wizard *w, int index, int direction)
{
	Page *old = wizard_current(w), *p = wizard_page(w, index);
	GtkStack *inner;
	char name[16];

	if (!p)
		return;
	if (old && old->leave)
		old->leave(old);
	if (direction > 0 && old && !old->transient && old != p)
		g_array_append_val(w->history, old->index);

	if (!p->root) {
		p->root = p->build(p);
		g_snprintf(name, sizeof(name), "page%d", p->index);
		gtk_widget_show_all(p->root);
		gtk_stack_add_named(GTK_STACK(w->inner[p->scene]), p->root, name);
		if (p->relayout)
			p->relayout(p);
	}
	inner = GTK_STACK(w->inner[p->scene]);
	if (p->scene == SCENE_SETUP)
		gtk_stack_set_transition_type(inner, direction < 0 ?
			GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT :
			direction > 0 ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT :
			GTK_STACK_TRANSITION_TYPE_NONE);
	if (!old || old->scene != p->scene)
		gtk_stack_set_transition_type(inner, GTK_STACK_TRANSITION_TYPE_NONE);
	else if (p->scene == SCENE_CARD)
		gtk_stack_set_transition_type(inner,
					      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_visible_child(inner, p->root);
	gtk_stack_set_visible_child(GTK_STACK(w->root), w->scenes[p->scene]);

	w->current = index;
	app_log(w->app, IL_LOG_INFO, "ui: page %d \"%s\"%s", index, p->id,
		direction < 0 ? " (back)" : "");
	if (p->enter)
		p->enter(p, direction);
	refresh_chrome(w, p);
	g_idle_add(focus_idle, w);
	autopilot_page_entered(w->app, index);
}

void wizard_next(Wizard *w)
{
	Page *p = wizard_current(w);
	int target;

	if (!p || !p->next_ok || !p->next_visible || sheet_is_open(w->app))
		return;
	if (p->go_next && !p->go_next(p))
		return;
	target = p->next_index >= 0 ? p->next_index : p->index + 1;
	if (target >= PAGE_COUNT)
		return;
	wizard_go(w, target, +1);
}

void wizard_back(Wizard *w)
{
	Page *p = wizard_current(w);
	int target;

	if (!p || !p->back_ok || sheet_is_open(w->app))
		return;
	if (p->go_back && p->go_back(p))
		return;
	if (!w->history->len)
		return;
	target = g_array_index(w->history, int, w->history->len - 1);
	g_array_set_size(w->history, w->history->len - 1);
	wizard_go(w, target, -1);
}

void wizard_seed_history(Wizard *w, int index)
{
	g_array_set_size(w->history, 0);
	for (int i = 0; i < index && i <= PAGE_SUMMARY; i++)
		if (!w->pages[i].transient)
			g_array_append_val(w->history, i);
}

void wizard_clear_history(Wizard *w)
{
	g_array_set_size(w->history, 0);
}

void wizard_history_truncate(Wizard *w, int index)
{
	for (guint i = 0; i < w->history->len; i++) {
		if (g_array_index(w->history, int, i) == index) {
			g_array_set_size(w->history, i);
			return;
		}
	}
}

void wizard_relayout(Wizard *w)
{
	gboolean compact = w->app->tier == TIER_COMPACT;
	Page *cur = wizard_current(w);

	for (int i = 0; i < SCENE_COUNT; i++)
		if (w->frames[i])
			frame_bin_set_compact(FRAME_BIN(w->frames[i]), compact);
	frame_bin_set_share(FRAME_BIN(w->frames[SCENE_SETUP]), 0.72, 0.90,
			    w->app->tier == TIER_LARGE ? 1120 : 980,
			    w->app->tier == TIER_LARGE ? 840 : 760);
	frame_bin_set_margins(FRAME_BIN(w->frames[SCENE_CARD]),
			      w->app->tier == TIER_LARGE ? 56 : 36,
			      compact ? 40 : 48);
	/* the illustration takes a good third of the card */
	{
		int margin = w->app->tier == TIER_LARGE ? 56 : 36;
		int card_w = MIN((int)((w->app->screen_w - 2 * margin) * 0.92), 1180);

		gtk_widget_set_size_request(w->card_art_col, MAX(200, card_w * 38 / 100),
					    -1);
	}
	for (int i = 0; i < PAGE_COUNT; i++) {
		Page *p = &w->pages[i];

		if (p->root && p->relayout)
			p->relayout(p);
	}
	if (cur)
		refresh_chrome(w, cur);
}

gboolean wizard_key_press(Wizard *w, GdkEventKey *ev)
{
	guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
	Page *p = wizard_current(w);

	if (ev->keyval == GDK_KEY_Escape && mods == 0) {
		if (sheet_escape(w->app))
			return TRUE;
		if (p && (p->scene == SCENE_SETUP || p->scene == SCENE_CARD)) {
			wizard_back(w);
			return TRUE;
		}
		return FALSE;
	}
	if (mods == GDK_MOD1_MASK && !sheet_is_open(w->app)) {
		switch (gdk_keyval_to_lower(ev->keyval)) {
		case GDK_KEY_n:
			wizard_next(w);
			return TRUE;
		case GDK_KEY_b:
			wizard_back(w);
			return TRUE;
		default:
			break;
		}
	}
	return FALSE;
}
