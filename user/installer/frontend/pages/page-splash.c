/* LikeOS installer -- page 0: the start screen.
 *
 * Black, the LikeOS logo in the middle and the spinner under it, shown
 * while the backend looks at the installation medium.  It stays at least a
 * moment, so it reads as a start screen and not as a flicker.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#define SPLASH_MIN_MS 1800

typedef struct {
	GtkWidget *logo;
	GtkWidget *spinner;
	GtkWidget *status;
	gboolean probe_done;
	gboolean timer_done;
	gboolean left;
	guint timer;
} Splash;

static void splash_maybe_leave(Page *p)
{
	Splash *s = p->priv;

	if (s->left || !s->probe_done || !s->timer_done)
		return;
	s->left = TRUE;
	wizard_go(pages_wizard(), PAGE_LANGUAGE, +1);
}

static gboolean splash_timer(gpointer data)
{
	Page *p = data;
	Splash *s = p->priv;

	s->timer = 0;
	s->timer_done = TRUE;
	splash_maybe_leave(p);
	return G_SOURCE_REMOVE;
}

static void probe_done(App *app, int rc, gpointer data)
{
	Page *p = data;
	Splash *s = p->priv;

	(void)app;
	(void)rc;
	s->probe_done = TRUE;
	splash_maybe_leave(p);
}

static GtkWidget *splash_build(Page *p)
{
	Splash *s = p->priv;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GdkPixbuf *pb = app_logo(p->app, 128);

	ui_add_class(box, "il-splash");
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	if (pb) {
		s->logo = gtk_image_new_from_pixbuf(pb);
		g_object_unref(pb);
	} else {
		s->logo = art_view_new(ART_LOGO);
		gtk_widget_set_size_request(s->logo, 128, 128);
	}
	ui_add_class(s->logo, "il-splash-logo");
	gtk_box_pack_start(GTK_BOX(box), s->logo, FALSE, FALSE, 0);

	s->spinner = ring_spinner_new(40);
	gtk_widget_set_margin_top(s->spinner, 72);
	ui_add_class(s->spinner, "il-splash-spinner");
	gtk_box_pack_start(GTK_BOX(box), s->spinner, FALSE, FALSE, 0);

	s->status = ui_label("Starting LikeOS Setup", "il-splash-status");
	gtk_label_set_xalign(GTK_LABEL(s->status), 0.5);
	gtk_box_pack_start(GTK_BOX(box), s->status, FALSE, FALSE, 0);
	return box;
}

static void splash_enter(Page *p, int direction)
{
	Splash *s = p->priv;

	(void)direction;
	s->left = FALSE;
	s->probe_done = FALSE;
	s->timer_done = FALSE;
	if (s->timer)
		g_source_remove(s->timer);
	s->timer = g_timeout_add(p->app->no_splash_delay ? 200 : SPLASH_MIN_MS,
				 splash_timer, p);
	if (!bridge_probe_source(p->app->bridge, probe_done, p))
		s->probe_done = TRUE;
}

static void splash_leave(Page *p)
{
	Splash *s = p->priv;

	if (s->timer) {
		g_source_remove(s->timer);
		s->timer = 0;
	}
}

void page_splash_init(Page *p)
{
	p->id = "splash";
	p->scene = SCENE_SPLASH;
	p->transient = TRUE;
	p->build = splash_build;
	p->enter = splash_enter;
	p->leave = splash_leave;
	p->next_visible = FALSE;
	p->back_ok = FALSE;
	p->priv = g_new0(Splash, 1);
}
