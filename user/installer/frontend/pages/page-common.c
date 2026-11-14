/* LikeOS installer -- page building blocks (see pages.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

static Wizard *g_wizard;

Wizard *pages_wizard(void)
{
	return g_wizard;
}

void pages_set_wizard(Wizard *w)
{
	g_wizard = w;
}

char *pages_escape(const char *text)
{
	return g_markup_escape_text(text ? text : "", -1);
}

static GtkWidget *page_scaffold(const char *heading, const char *subtitle,
				GtkWidget **body, const char *page_class)
{
	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *head;
	GtkWidget *sub = NULL;
	GtkWidget *scroller;

	ui_add_class(outer, "il-page");
	ui_add_class(outer, page_class);
	head = ui_heading(heading);
	gtk_box_pack_start(GTK_BOX(outer), head, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(outer), "page-heading", head);
	sub = ui_label(subtitle ? subtitle : "", "il-subtitle");
	gtk_box_pack_start(GTK_BOX(outer), sub, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(outer), "page-subtitle", sub);
	gtk_widget_set_no_show_all(sub, subtitle == NULL);
	gtk_widget_set_visible(sub, subtitle != NULL);

	*body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
	ui_add_class(*body, "il-page-body");
	gtk_box_pack_start(GTK_BOX(outer), *body, TRUE, TRUE, 0);

	scroller = ui_scroller(outer);
	ui_add_class(scroller, "il-page-scroller");
	g_object_set_data(G_OBJECT(scroller), "page-heading", head);
	g_object_set_data(G_OBJECT(scroller), "page-subtitle", sub);
	return scroller;
}

GtkWidget *setup_page_new(const char *heading, GtkWidget **body)
{
	return page_scaffold(heading, NULL, body, "il-setup-page");
}

GtkWidget *card_page_new(const char *heading, const char *subtitle,
			 GtkWidget **body)
{
	return page_scaffold(heading, subtitle, body, "il-card-page");
}

void page_set_heading(GtkWidget *root, const char *heading, const char *subtitle)
{
	GtkWidget *h = g_object_get_data(G_OBJECT(root), "page-heading");
	GtkWidget *s = g_object_get_data(G_OBJECT(root), "page-subtitle");

	if (h && heading)
		gtk_label_set_text(GTK_LABEL(h), heading);
	if (s) {
		gtk_label_set_text(GTK_LABEL(s), subtitle ? subtitle : "");
		gtk_widget_set_no_show_all(s, FALSE);
		gtk_widget_set_visible(s, subtitle && *subtitle);
	}
}

GtkWidget *form_row(const char *mnemonic_label, GtkWidget *control)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic_label);

	gtk_label_set_mnemonic_widget(GTK_LABEL(l), control);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_widget_set_size_request(l, 220, -1);
	ui_add_class(l, "il-form-label");
	gtk_widget_set_hexpand(control, FALSE);
	gtk_widget_set_halign(control, GTK_ALIGN_START);
	gtk_widget_set_size_request(control, 300, -1);
	gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
	ui_add_class(row, "il-form-row");
	g_object_set_data(G_OBJECT(row), "form-label", l);
	g_object_set_data(G_OBJECT(row), "form-control", control);
	return row;
}

void form_relayout(GtkWidget *container, gboolean stacked)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(container));

	for (GList *c = children; c; c = c->next) {
		GtkWidget *row = c->data;
		GtkWidget *l = g_object_get_data(G_OBJECT(row), "form-label");
		GtkWidget *ctl = g_object_get_data(G_OBJECT(row), "form-control");

		if (!l)
			continue;
		gtk_orientable_set_orientation(GTK_ORIENTABLE(row), stacked ?
			GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
		gtk_box_set_spacing(GTK_BOX(row), stacked ? 6 : 16);
		gtk_widget_set_size_request(l, stacked ? -1 : 220, -1);
		gtk_widget_set_size_request(ctl, stacked ? -1 : 300, -1);
		gtk_widget_set_halign(ctl, stacked ? GTK_ALIGN_FILL : GTK_ALIGN_START);
	}
	g_list_free(children);
}

static void on_search_changed(GtkSearchEntry *e, gpointer data)
{
	GtkWidget *list = data;

	choice_list_filter(list, gtk_entry_get_text(GTK_ENTRY(e)));
	/* a selection the search hides gives way to the best match */
	if (*gtk_entry_get_text(GTK_ENTRY(e))) {
		const char *sel = choice_list_selected(list);

		if (!sel || !choice_list_is_shown(list, sel))
			choice_list_select_first_visible(list);
	}
}

static gboolean on_search_key(GtkWidget *e, GdkEventKey *ev, gpointer data)
{
	GtkWidget *list = data;

	(void)e;
	/* Down arrow from the search field into the list */
	if (ev->keyval == GDK_KEY_Down || ev->keyval == GDK_KEY_KP_Down) {
		GtkListBoxRow *row =
			gtk_list_box_get_selected_row(GTK_LIST_BOX(list));

		if (row && gtk_widget_get_child_visible(GTK_WIDGET(row)))
			gtk_widget_grab_focus(GTK_WIDGET(row));
		else
			gtk_widget_child_focus(list, GTK_DIR_TAB_FORWARD);
		return TRUE;
	}
	return FALSE;
}

/* Enter in the search field takes the first row found, then the
 * window's default button moves on with it. */
static void on_search_activate(GtkEntry *e, gpointer data)
{
	(void)e;
	choice_list_select_first_visible(data);
}

GtkWidget *search_for_list(GtkWidget *list, const char *placeholder)
{
	GtkWidget *e = gtk_search_entry_new();

	gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
	ui_add_class(e, "il-entry");
	ui_add_class(e, "il-search");
	g_signal_connect(e, "search-changed", G_CALLBACK(on_search_changed), list);
	g_signal_connect(e, "key-press-event", G_CALLBACK(on_search_key), list);
	g_signal_connect(e, "activate", G_CALLBACK(on_search_activate), list);
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	return e;
}
