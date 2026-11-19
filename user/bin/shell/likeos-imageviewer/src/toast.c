// LikeOS -- LikeOS Image Viewer: toasts and confirm cards.
//
// A toast is a short message on a dark card that floats at the bottom of
// the picture for a few seconds -- "Moved to Trash" with its Undo, "Path
// copied", a save in progress with its bar -- and goes by itself.  A
// confirm card asks a question in the middle of the window ("Revert all
// edits?") with the rest of it dimmed; nothing else takes input until it is
// answered, Enter accepts and Esc cancels.  Both are children of a
// GtkOverlay: they belong to the window and move with it, instead of being
// windows of their own.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "imageviewer.h"

#define IV_TOAST_SECONDS 5

struct IvToast {
	GtkOverlay *overlay;
	GtkWidget *revealer, *card, *label, *action, *close, *bar;
	guint timer;
	IvToastAction cb;
	gpointer data;
	GDestroyNotify destroy;
	gboolean progress_action;	/* the action stays with the bar */
};

static void drop_action(IvToast *t)
{
	if (t->destroy && t->data)
		t->destroy(t->data);
	t->cb = NULL;
	t->data = NULL;
	t->destroy = NULL;
}

void iv_toast_hide(IvToast *t)
{
	if (!t)
		return;
	if (t->timer) {
		g_source_remove(t->timer);
		t->timer = 0;
	}
	gtk_revealer_set_reveal_child(GTK_REVEALER(t->revealer), FALSE);
	drop_action(t);
	t->progress_action = FALSE;
}

static gboolean timed_out(gpointer data)
{
	IvToast *t = data;

	t->timer = 0;
	iv_toast_hide(t);
	return G_SOURCE_REMOVE;
}

static void action_clicked(GtkButton *b, IvToast *t)
{
	IvToastAction cb = t->cb;
	gpointer data = t->data;
	GDestroyNotify destroy = t->destroy;

	/* Taken first: the action may well show another toast. */
	t->cb = NULL;
	t->data = NULL;
	t->destroy = NULL;
	iv_toast_hide(t);
	if (cb)
		cb(data);
	if (destroy && data)
		destroy(data);
}

static void close_clicked(GtkButton *b, IvToast *t)
{
	iv_toast_hide(t);
}

/* While the pointer is on the toast it stays. */
static gboolean card_enter(GtkWidget *w, GdkEventCrossing *ev, IvToast *t)
{
	if (t->timer) {
		g_source_remove(t->timer);
		t->timer = 0;
	}
	return FALSE;
}

static gboolean card_leave(GtkWidget *w, GdkEventCrossing *ev, IvToast *t)
{
	if (ev->detail == GDK_NOTIFY_INFERIOR)
		return FALSE;
	if (!t->timer && !gtk_widget_get_visible(t->bar) &&
	    gtk_revealer_get_reveal_child(GTK_REVEALER(t->revealer)))
		t->timer = g_timeout_add_seconds(2, timed_out, t);
	return FALSE;
}

IvToast *iv_toast_new(GtkOverlay *overlay)
{
	IvToast *t = g_new0(IvToast, 1);
	GtkWidget *eb = gtk_event_box_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

	t->overlay = overlay;
	t->revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(t->revealer),
					 GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
	gtk_revealer_set_transition_duration(GTK_REVEALER(t->revealer), 180);
	gtk_widget_set_halign(t->revealer, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(t->revealer, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom(t->revealer, 72);
	gtk_widget_set_no_show_all(t->revealer, FALSE);

	t->card = row;
	iv_add_class(row, "iv-toast");
	t->label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(t->label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(t->label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(t->label), 60);
	t->bar = gtk_progress_bar_new();
	iv_add_class(t->bar, "iv-toast-bar");
	gtk_widget_set_no_show_all(t->bar, TRUE);
	gtk_box_pack_start(GTK_BOX(col), t->label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), t->bar, FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);

	t->action = gtk_button_new_with_label("");
	iv_add_class(t->action, "iv-toast-action");
	gtk_widget_set_can_focus(t->action, FALSE);
	gtk_widget_set_valign(t->action, GTK_ALIGN_CENTER);
	gtk_widget_set_no_show_all(t->action, TRUE);
	g_signal_connect(t->action, "clicked", G_CALLBACK(action_clicked), t);
	gtk_box_pack_start(GTK_BOX(row), t->action, FALSE, FALSE, 0);

	t->close = iv_glyph_button(IV_GLYPH_CLOSE, 10, "Dismiss",
				   "iv-toast-close");
	g_signal_connect(t->close, "clicked", G_CALLBACK(close_clicked), t);
	gtk_box_pack_start(GTK_BOX(row), t->close, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(eb), row);
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
	gtk_widget_add_events(eb, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
	g_signal_connect(eb, "enter-notify-event", G_CALLBACK(card_enter), t);
	g_signal_connect(eb, "leave-notify-event", G_CALLBACK(card_leave), t);
	gtk_container_add(GTK_CONTAINER(t->revealer), eb);
	gtk_overlay_add_overlay(overlay, t->revealer);
	gtk_widget_show_all(t->revealer);
	return t;
}

void iv_toast_free(IvToast *t)
{
	if (!t)
		return;
	if (t->timer)
		g_source_remove(t->timer);
	drop_action(t);
	g_free(t);
}

void iv_toast_set_margin(IvToast *t, int bottom_margin)
{
	gtk_widget_set_margin_bottom(t->revealer, bottom_margin);
}

void iv_toast_show(IvToast *t, const char *text, const char *action,
		   IvToastAction cb, gpointer data, GDestroyNotify destroy,
		   int seconds)
{
	if (t->timer) {
		g_source_remove(t->timer);
		t->timer = 0;
	}
	drop_action(t);
	t->progress_action = FALSE;
	t->cb = cb;
	t->data = data;
	t->destroy = destroy;
	gtk_label_set_text(GTK_LABEL(t->label), text);
	gtk_widget_hide(t->bar);
	if (action) {
		gtk_button_set_label(GTK_BUTTON(t->action), action);
		gtk_widget_show(t->action);
	} else {
		gtk_widget_hide(t->action);
	}
	gtk_widget_show(t->close);
	gtk_revealer_set_reveal_child(GTK_REVEALER(t->revealer), TRUE);
	t->timer = g_timeout_add_seconds(seconds > 0 ? (guint)seconds :
						       IV_TOAST_SECONDS,
					 timed_out, t);
}

void iv_toast_progress(IvToast *t, const char *text, double fraction)
{
	if (t->timer) {
		g_source_remove(t->timer);
		t->timer = 0;
	}
	gtk_label_set_text(GTK_LABEL(t->label), text);
	if (!t->progress_action)
		gtk_widget_hide(t->action);
	gtk_widget_hide(t->close);
	gtk_widget_show(t->bar);
	if (fraction < 0)
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(t->bar));
	else
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(t->bar),
					      CLAMP(fraction, 0, 1));
	gtk_revealer_set_reveal_child(GTK_REVEALER(t->revealer), TRUE);
}

/* A progress toast with a button beside the bar (added for the editor's
 * slow jobs: "Cancel").  The button stays while iv_toast_progress() moves
 * the bar, until the toast is hidden or replaced. */
void iv_toast_progress_action(IvToast *t, const char *text, double fraction,
			      const char *action, IvToastAction cb,
			      gpointer data)
{
	drop_action(t);
	t->cb = cb;
	t->data = data;
	t->destroy = NULL;
	t->progress_action = action != NULL;
	if (action) {
		gtk_button_set_label(GTK_BUTTON(t->action), action);
		gtk_widget_show(t->action);
	}
	iv_toast_progress(t, text, fraction);
}

/* ---- confirm cards ------------------------------------------------------------ */

typedef struct {
	GtkOverlay *overlay;
	GtkWidget *shade;
	IvConfirmDone done;
	IvChoiceDone choice;
	gpointer data;
	GtkWidget *first;
} Card;

static void card_answer(Card *c, int answer)
{
	GtkWidget *shade = c->shade;

	g_object_set_data(G_OBJECT(c->overlay), "iv-confirm-open", NULL);
	gtk_grab_remove(shade);
	if (c->done)
		c->done(answer == 1, c->data);
	if (c->choice)
		c->choice(answer, c->data);
	gtk_widget_destroy(shade);
	g_free(c);
}

static void answer_first(GtkButton *b, Card *c)
{
	card_answer(c, 1);
}

static void answer_second(GtkButton *b, Card *c)
{
	card_answer(c, 2);
}

static void answer_cancel(GtkButton *b, Card *c)
{
	card_answer(c, 0);
}

static gboolean card_key(GtkWidget *w, GdkEventKey *ev, Card *c)
{
	switch (ev->keyval) {
	case GDK_KEY_Escape:
		card_answer(c, 0);
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		/* Enter on a focused button presses that button. */
		if (gtk_widget_has_focus(c->first) ||
		    !GTK_IS_BUTTON(gtk_window_get_focus(
			    GTK_WINDOW(gtk_widget_get_toplevel(w))))) {
			card_answer(c, 1);
			return TRUE;
		}
		return FALSE;
	}
	return FALSE;
}

/* Clicks on the dimmed part do nothing (the card must be answered). */
static gboolean shade_press(GtkWidget *w, GdkEventButton *ev, Card *c)
{
	return TRUE;
}

static GtkWidget *card_button(const char *label, const char *cls,
			      GCallback cb, Card *c)
{
	GtkWidget *b = gtk_button_new_with_mnemonic(label);

	iv_add_class(b, "iv-card-button");
	iv_add_class(b, cls);
	gtk_widget_set_hexpand(b, TRUE);
	g_signal_connect(b, "clicked", cb, c);
	return b;
}

static void card_open(GtkOverlay *overlay, const char *title, const char *text,
		      const char *first, const char *second, const char *cancel,
		      gboolean destructive, IvConfirmDone done,
		      IvChoiceDone choice, gpointer data)
{
	Card *c = g_new0(Card, 1);
	GtkWidget *shade = gtk_event_box_new();
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *t = gtk_label_new(title);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *b;

	c->overlay = overlay;
	c->shade = shade;
	c->done = done;
	c->choice = choice;
	c->data = data;
	iv_add_class(shade, "iv-shade");
	gtk_widget_set_can_focus(shade, TRUE);
	gtk_widget_add_events(shade, GDK_KEY_PRESS_MASK | GDK_BUTTON_PRESS_MASK);
	g_signal_connect(shade, "key-press-event", G_CALLBACK(card_key), c);
	g_signal_connect(shade, "button-press-event", G_CALLBACK(shade_press), c);

	iv_add_class(card, "iv-confirm");
	gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
	gtk_widget_set_size_request(card, 420, -1);
	iv_add_class(t, "iv-confirm-title");
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_label_set_line_wrap(GTK_LABEL(t), TRUE);
	gtk_box_pack_start(GTK_BOX(card), t, FALSE, FALSE, 0);
	if (text) {
		GtkWidget *l = gtk_label_new(text);

		iv_add_class(l, "iv-confirm-text");
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 52);
		gtk_box_pack_start(GTK_BOX(card), l, FALSE, FALSE, 0);
	}
	gtk_widget_set_margin_top(buttons, 12);
	b = card_button(first, destructive ? "iv-destructive" : "iv-accent",
			G_CALLBACK(answer_first), c);
	c->first = b;
	gtk_box_pack_start(GTK_BOX(buttons), b, TRUE, TRUE, 0);
	if (second) {
		b = card_button(second, "iv-plain", G_CALLBACK(answer_second), c);
		gtk_box_pack_start(GTK_BOX(buttons), b, TRUE, TRUE, 0);
	}
	b = card_button(cancel, "iv-plain", G_CALLBACK(answer_cancel), c);
	gtk_box_pack_start(GTK_BOX(buttons), b, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(card), buttons, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(shade), card);
	gtk_overlay_add_overlay(overlay, shade);
	gtk_widget_show_all(shade);
	g_object_set_data(G_OBJECT(overlay), "iv-confirm-open", c);
	gtk_grab_add(shade);
	gtk_widget_grab_focus(c->first);
}

void iv_confirm(GtkOverlay *overlay, const char *title, const char *text,
		const char *accept, const char *cancel, gboolean destructive,
		IvConfirmDone done, gpointer data)
{
	card_open(overlay, title, text, accept, NULL, cancel, destructive, done,
		  NULL, data);
}

void iv_choice(GtkOverlay *overlay, const char *title, const char *text,
	       const char *first, const char *second, const char *cancel,
	       IvChoiceDone done, gpointer data)
{
	card_open(overlay, title, text, first, second, cancel, FALSE, NULL, done,
		  data);
}

gboolean iv_confirm_is_open(GtkOverlay *overlay)
{
	return overlay &&
	       g_object_get_data(G_OBJECT(overlay), "iv-confirm-open") != NULL;
}
