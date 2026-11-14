/* LikeOS installer -- in-window dialogs (see sheet.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "sheet.h"
#include "layout.h"
#include "widgets/widgets.h"

struct Sheet {
	App *app;
	GtkWidget *scrim;      /* the dimming layer: an overlay child */
	GtkWidget *frame;      /* FrameBin */
	GtkWidget *card;
	GtkWidget *art;
	GtkWidget *title;
	GtkWidget *body_box;
	GtkWidget *buttons;
	GtkWidget *default_button;
	SheetCb cb;
	gpointer data;
	gboolean wide;
	gboolean closing;
	gboolean body_packed;
};

static Sheet *g_open;   /* at most one sheet at a time */

static gboolean scrim_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	(void)data;
	cairo_set_source_rgba(cr, 0.03, 0.05, 0.10, 0.52);
	cairo_rectangle(cr, 0, 0, gtk_widget_get_allocated_width(w),
			gtk_widget_get_allocated_height(w));
	cairo_fill(cr);
	return FALSE;
}

static gboolean scrim_swallow(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	(void)w;
	(void)ev;
	(void)data;
	return TRUE;
}

Sheet *sheet_new(App *app, const char *title, ArtKind art)
{
	Sheet *s = g_new0(Sheet, 1);
	GtkWidget *head;

	s->app = app;
	s->scrim = gtk_event_box_new();
	gtk_widget_set_app_paintable(s->scrim, TRUE);
	gtk_widget_add_events(s->scrim, GDK_BUTTON_PRESS_MASK |
			      GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK);
	g_signal_connect(s->scrim, "draw", G_CALLBACK(scrim_draw), NULL);
	g_signal_connect(s->scrim, "button-press-event",
			 G_CALLBACK(scrim_swallow), NULL);
	g_signal_connect(s->scrim, "scroll-event", G_CALLBACK(scrim_swallow), NULL);
	ui_add_class(s->scrim, "il-scrim");

	s->frame = frame_bin_new(0.5, 0.8, 600, 640);
	frame_bin_set_margins(FRAME_BIN(s->frame), 24, 0);
	frame_bin_set_fit_height(FRAME_BIN(s->frame), TRUE);
	frame_bin_set_compact(FRAME_BIN(s->frame), app->tier == TIER_COMPACT);
	gtk_container_add(GTK_CONTAINER(s->scrim), s->frame);

	s->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	ui_add_class(s->card, "il-sheet");
	gtk_container_add(GTK_CONTAINER(s->frame), s->card);

	head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	ui_add_class(head, "il-sheet-head");
	if (art != ART_NONE) {
		s->art = art_view_new(art);
		art_view_set_animated(s->art, FALSE);
		gtk_widget_set_size_request(s->art, 48, 48);
		gtk_widget_set_valign(s->art, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(head), s->art, FALSE, FALSE, 0);
	}
	s->title = ui_label(title, "il-sheet-title");
	gtk_widget_set_valign(s->title, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(head), s->title, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(s->card), head, FALSE, FALSE, 0);

	s->body_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	ui_add_class(s->body_box, "il-sheet-body");
	/* the body goes in when the sheet is shown: straight into the card
	 * for a question, in a scrolled window for a long text */
	g_object_ref_sink(s->body_box);

	{
		/* the bar spans the sheet, the buttons sit at its right */
		GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

		ui_add_class(bar, "il-sheet-buttons");
		s->buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_halign(s->buttons, GTK_ALIGN_END);
		gtk_widget_set_hexpand(s->buttons, TRUE);
		gtk_box_pack_start(GTK_BOX(bar), s->buttons, TRUE, TRUE, 0);
		gtk_box_pack_end(GTK_BOX(s->card), bar, FALSE, FALSE, 0);
	}
	return s;
}

void sheet_set_body(Sheet *s, GtkWidget *body)
{
	gtk_box_pack_start(GTK_BOX(s->body_box), body, FALSE, FALSE, 0);
}

void sheet_set_text(Sheet *s, const char *markup)
{
	GtkWidget *l = ui_markup(markup, "il-sheet-text");

	gtk_label_set_selectable(GTK_LABEL(l), FALSE);
	sheet_set_body(s, l);
}

static void on_button(GtkButton *b, gpointer data)
{
	Sheet *s = data;

	sheet_close(s, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),
							 "sheet-response")));
}

GtkWidget *sheet_add_button(Sheet *s, const char *label, int response,
			    const char *style)
{
	GtkWidget *b = gtk_button_new_with_mnemonic(label);

	ui_add_class(b, "il-button");
	if (style) {
		char cls[32];

		g_snprintf(cls, sizeof(cls), "il-%s", style);
		ui_add_class(b, cls);
	}
	g_object_set_data(G_OBJECT(b), "sheet-response", GINT_TO_POINTER(response));
	g_signal_connect(b, "clicked", G_CALLBACK(on_button), s);
	gtk_box_pack_start(GTK_BOX(s->buttons), b, FALSE, FALSE, 0);
	if (style && (g_str_equal(style, "primary") || g_str_equal(style, "danger")))
		s->default_button = b;
	else if (!s->default_button && response != SHEET_CANCEL)
		s->default_button = b;
	return b;
}

void sheet_set_wide(Sheet *s, gboolean wide)
{
	s->wide = wide;
	/* a long text fills the room it gets and scrolls */
	frame_bin_set_fit_height(FRAME_BIN(s->frame), !wide);
	frame_bin_set_share(FRAME_BIN(s->frame), wide ? 0.8 : 0.5, wide ? 0.86 : 0.8,
			    wide ? 900 : 600, wide ? 760 : 640);
}

static gboolean focus_default(gpointer data)
{
	GtkWidget *b = data;

	if (gtk_widget_get_mapped(b)) {
		gtk_widget_grab_focus(b);
		gtk_widget_grab_default(b);
	}
	g_object_unref(b);
	return G_SOURCE_REMOVE;
}

void sheet_present(Sheet *s, SheetCb cb, gpointer data)
{
	GtkWidget *under = gtk_bin_get_child(GTK_BIN(s->app->overlay));

	if (g_open)
		sheet_close(g_open, SHEET_CANCEL);
	if (!s->body_packed) {
		GList *kids = gtk_container_get_children(GTK_CONTAINER(s->card));
		GtkWidget *bar = g_list_last(kids)->data;

		g_list_free(kids);
		if (s->wide) {
			GtkWidget *sw = ui_scroller(s->body_box);

			gtk_box_pack_start(GTK_BOX(s->card), sw, TRUE, TRUE, 0);
		} else {
			gtk_box_pack_start(GTK_BOX(s->card), s->body_box, TRUE, TRUE, 0);
		}
		/* keep the button bar last */
		gtk_box_reorder_child(GTK_BOX(s->card), bar, -1);
		g_object_unref(s->body_box);
		s->body_packed = TRUE;
	}
	s->cb = cb;
	s->data = data;
	{
		GList *kids = gtk_container_get_children(GTK_CONTAINER(s->buttons));

		if (!kids)
			sheet_add_button(s, "_Close", SHEET_OK, "primary");
		g_list_free(kids);
	}
	gtk_overlay_add_overlay(GTK_OVERLAY(s->app->overlay), s->scrim);
	if (under)
		gtk_widget_set_sensitive(under, FALSE);
	gtk_widget_show_all(s->scrim);
	g_open = s;
	if (s->default_button) {
		gtk_widget_set_can_default(s->default_button, TRUE);
		g_idle_add(focus_default, g_object_ref(s->default_button));
	}
}

static gboolean sheet_destroy_idle(gpointer data)
{
	Sheet *s = data;

	gtk_widget_destroy(s->scrim);
	g_free(s);
	return G_SOURCE_REMOVE;
}

void sheet_close(Sheet *s, int response)
{
	GtkWidget *under;

	if (!s || s->closing)
		return;
	s->closing = TRUE;
	under = gtk_bin_get_child(GTK_BIN(s->app->overlay));
	if (g_open == s)
		g_open = NULL;
	gtk_widget_hide(s->scrim);
	if (under && !g_open)
		gtk_widget_set_sensitive(under, TRUE);
	if (s->cb)
		s->cb(s, response, s->data);
	g_idle_add(sheet_destroy_idle, s);
}

void sheet_set_busy(Sheet *s, gboolean busy)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(s->buttons));

	for (GList *l = children; l; l = l->next)
		gtk_widget_set_sensitive(l->data, !busy);
	g_list_free(children);
}

gboolean sheet_is_open(App *app)
{
	(void)app;
	return g_open != NULL;
}

gboolean sheet_escape(App *app)
{
	(void)app;
	if (!g_open)
		return FALSE;
	{
		/* a busy sheet (its buttons insensitive) stays */
		GList *children = gtk_container_get_children(
			GTK_CONTAINER(g_open->buttons));
		gboolean busy = children && !gtk_widget_get_sensitive(children->data);

		g_list_free(children);
		if (busy)
			return TRUE;
	}
	sheet_close(g_open, SHEET_CANCEL);
	return TRUE;
}

void sheet_confirm(App *app, const char *title, const char *markup,
		   const char *ok_label, gboolean danger, ArtKind art,
		   SheetCb cb, gpointer data)
{
	Sheet *s = sheet_new(app, title, art);

	sheet_set_text(s, markup);
	sheet_add_button(s, "_Cancel", SHEET_CANCEL, NULL);
	sheet_add_button(s, ok_label, SHEET_OK, danger ? "danger" : "primary");
	sheet_present(s, cb, data);
}

void sheet_show_text(App *app, const char *title, const char *text,
		     gboolean monospace)
{
	Sheet *s = sheet_new(app, title, ART_NONE);
	GtkWidget *l = monospace ? ui_label(text, "il-sheet-mono") :
		ui_markup(text, "il-sheet-text");

	if (monospace)
		gtk_label_set_max_width_chars(GTK_LABEL(l), 80);
	gtk_label_set_selectable(GTK_LABEL(l), TRUE);
	gtk_widget_set_can_focus(l, FALSE);
	sheet_set_wide(s, TRUE);
	sheet_set_body(s, l);
	sheet_add_button(s, "_Close", SHEET_OK, "primary");
	sheet_present(s, NULL, NULL);
}

static void power_done(Sheet *s, int response, gpointer data)
{
	App *app = data;

	(void)s;
	if (response == SHEET_OK)
		app_power_action(app, TRUE);
	else if (response == SHEET_OTHER)
		app_power_action(app, FALSE);
	else if (response == SHEET_THIRD)
		app_exit_setup(app);
	else
		app_log(app, IL_LOG_INFO, "ui: setup continues");
}

void sheet_power(App *app)
{
	Sheet *s = sheet_new(app, "Leave setup?", ART_WARNING);

	app_log(app, IL_LOG_INFO, "ui: asked whether to leave setup");
	if (app->installed)
		sheet_set_text(s, "LikeOS is installed. Restart the computer to "
			       "start it, or shut the computer down.");
	else
		sheet_set_text(s, "LikeOS is not installed yet. If you leave "
			       "now, nothing on your disks is changed and you "
			       "can start setup again.\n\n<b>Exit setup</b> "
			       "ends LikeOS Setup and shows a menu on the "
			       "screen: the text-mode installer, a console "
			       "login, restarting and switching off.");
	sheet_add_button(s, "_Continue setup", SHEET_CANCEL, NULL);
	if (!app->installed)
		sheet_add_button(s, "E_xit setup", SHEET_THIRD, NULL);
	sheet_add_button(s, "_Shut down", SHEET_OTHER, NULL);
	sheet_add_button(s, "_Restart", SHEET_OK, "danger");
	sheet_present(s, power_done, app);
}

gboolean sheet_respond(App *app, int response)
{
	(void)app;
	if (!g_open || g_open->closing)
		return FALSE;
	sheet_close(g_open, response);
	return TRUE;
}
