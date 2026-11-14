/* LikeOS installer -- a labelled input and small UI helpers.
 *
 * A field is a label above an input and a line of text below it.  The line
 * is help while all is well, and turns into the reason when the input is
 * not acceptable; the input's underline takes the same colour.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "widgets.h"

typedef struct {
	GtkWidget *input;
	GtkWidget *msg;
	GtkWidget *msg_icon;
	GtkWidget *msg_box;
	char *hint;
} Field;

static void field_free(gpointer p)
{
	Field *f = p;

	g_free(f->hint);
	g_free(f);
}

GtkWidget *field_new(const char *label, GtkWidget *input)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	Field *f = g_new0(Field, 1);

	if (label) {
		GtkWidget *l = gtk_label_new_with_mnemonic(label);
		GtkWidget *target = input;

		if (g_object_get_data(G_OBJECT(input), "password-entry"))
			target = password_entry_get_entry(input);
		gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_style_context_add_class(gtk_widget_get_style_context(l),
					    "il-field-label");
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	}
	f->input = input;
	gtk_box_pack_start(GTK_BOX(box), input, FALSE, FALSE, 0);

	f->msg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	f->msg_icon = icon_new(ICON_ERROR, 14);
	gtk_widget_set_valign(f->msg_icon, GTK_ALIGN_START);
	gtk_widget_set_margin_top(f->msg_icon, 2);
	f->msg = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(f->msg), 0);
	gtk_label_set_line_wrap(GTK_LABEL(f->msg), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(f->msg), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(f->msg), 60);
	gtk_box_pack_start(GTK_BOX(f->msg_box), f->msg_icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(f->msg_box), f->msg, TRUE, TRUE, 0);
	gtk_widget_set_no_show_all(f->msg_icon, TRUE);
	gtk_box_pack_start(GTK_BOX(box), f->msg_box, FALSE, FALSE, 0);

	gtk_style_context_add_class(gtk_widget_get_style_context(box), "il-field");
	g_object_set_data_full(G_OBJECT(box), "field", f, field_free);
	return box;
}

static void field_message(GtkWidget *w, const char *text, const char *cls,
			  IconKind icon)
{
	static const char *const all[] = {
		"il-field-hint", "il-field-error", "il-field-warning", "il-field-ok",
	};
	Field *f = g_object_get_data(G_OBJECT(w), "field");
	GtkStyleContext *sc;
	GtkWidget *input;

	if (!f)
		return;
	sc = gtk_widget_get_style_context(f->msg_box);
	for (int i = 0; i < 4; i++)
		gtk_style_context_remove_class(sc, all[i]);
	gtk_style_context_add_class(sc, cls);
	gtk_label_set_text(GTK_LABEL(f->msg), text ? text : "");
	gtk_widget_set_visible(f->msg, text && *text);
	if (icon != ICON_NONE && text && *text) {
		icon_set(f->msg_icon, icon);
		gtk_widget_show(f->msg_icon);
	} else {
		gtk_widget_hide(f->msg_icon);
	}

	input = f->input;
	if (g_object_get_data(G_OBJECT(input), "password-entry"))
		input = password_entry_get_entry(input);
	sc = gtk_widget_get_style_context(input);
	if (g_strcmp0(cls, "il-field-error") == 0)
		gtk_style_context_add_class(sc, "il-invalid");
	else
		gtk_style_context_remove_class(sc, "il-invalid");
}

void field_set_hint(GtkWidget *w, const char *text)
{
	Field *f = g_object_get_data(G_OBJECT(w), "field");

	if (f && text != f->hint) {
		g_free(f->hint);
		f->hint = g_strdup(text);
	}
	field_message(w, text, "il-field-hint", ICON_NONE);
}

void field_set_error(GtkWidget *w, const char *text)
{
	Field *f = g_object_get_data(G_OBJECT(w), "field");

	if (!text || !*text) {
		field_set_hint(w, f ? f->hint : NULL);
		return;
	}
	field_message(w, text, "il-field-error", ICON_ERROR);
}

void field_set_warning(GtkWidget *w, const char *text)
{
	field_message(w, text, "il-field-warning", ICON_WARNING);
}

void field_set_ok(GtkWidget *w, const char *text)
{
	field_message(w, text, "il-field-ok", ICON_CHECK);
}

GtkWidget *field_get_input(GtkWidget *w)
{
	Field *f = g_object_get_data(G_OBJECT(w), "field");

	return f ? f->input : NULL;
}

/* ---- helpers ------------------------------------------------------- */

void ui_add_class(GtkWidget *w, const char *css_class)
{
	if (w && css_class)
		gtk_style_context_add_class(gtk_widget_get_style_context(w),
					    css_class);
}

void ui_remove_class(GtkWidget *w, const char *css_class)
{
	if (w && css_class)
		gtk_style_context_remove_class(gtk_widget_get_style_context(w),
					       css_class);
}

void ui_toggle_class(GtkWidget *w, const char *css_class, gboolean on)
{
	if (on)
		ui_add_class(w, css_class);
	else
		ui_remove_class(w, css_class);
}

GtkWidget *ui_label(const char *text, const char *css_class)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 72);
	ui_add_class(l, css_class);
	return l;
}

GtkWidget *ui_markup(const char *markup, const char *css_class)
{
	GtkWidget *l = ui_label(NULL, css_class);

	gtk_label_set_markup(GTK_LABEL(l), markup);
	return l;
}

GtkWidget *ui_heading(const char *text)
{
	GtkWidget *l = ui_label(text, "il-heading");

	gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
	return l;
}

GtkWidget *ui_button(const char *mnemonic_label, const char *css_class)
{
	GtkWidget *b = gtk_button_new_with_mnemonic(mnemonic_label);

	ui_add_class(b, "il-button");
	ui_add_class(b, css_class);
	return b;
}

GtkWidget *ui_scroller(GtkWidget *child)
{
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw),
							 FALSE);
	gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(sw), FALSE);
	gtk_container_add(GTK_CONTAINER(sw), child);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_widget_set_hexpand(sw, TRUE);
	ui_add_class(sw, "il-body-scroller");
	return sw;
}

GtkWidget *ui_box(GtkOrientation o, int spacing, const char *css_class)
{
	GtkWidget *b = gtk_box_new(o, spacing);

	ui_add_class(b, css_class);
	return b;
}

GtkWidget *ui_check_row(const char *markup, IconKind icon)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *i = icon_new(icon, 18);
	GtkWidget *l = ui_markup(markup, "il-check-label");

	gtk_widget_set_valign(i, GTK_ALIGN_START);
	gtk_widget_set_margin_top(i, 1);
	ui_add_class(i, "il-check-icon");
	gtk_box_pack_start(GTK_BOX(row), i, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
	ui_add_class(row, "il-check-row");
	return row;
}
