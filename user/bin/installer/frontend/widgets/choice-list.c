/* LikeOS installer -- large selectable rows.
 *
 * The card pages ask their questions with a list of big rows: one line of
 * text each (sometimes a second, smaller one), the chosen row filled in the
 * accent colour.  A filter narrows the rows while the person types in a
 * search field above the list.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "widgets.h"

#include <string.h>

typedef struct {
	GtkWidget *scroller;
	char *filter;          /* casefolded, NULL = all */
	ChoiceChanged changed;
	gpointer changed_data;
	ChoiceChanged activated;
	gpointer activated_data;
	gboolean quiet;        /* no "changed" while selecting in code */
	guint scroll_idle;
} ChoiceList;

typedef struct {
	char *id;
	char *label;           /* casefolded label */
	char *haystack;        /* casefolded label + sublabel */
} ChoiceRow;

static ChoiceList *cl_data(GtkWidget *list)
{
	return g_object_get_data(G_OBJECT(list), "choice-list");
}

static void row_data_free(gpointer p)
{
	ChoiceRow *r = p;

	g_free(r->id);
	g_free(r->label);
	g_free(r->haystack);
	g_free(r);
}

static gboolean filter_func(GtkListBoxRow *row, gpointer data)
{
	ChoiceList *cl = data;
	ChoiceRow *r = g_object_get_data(G_OBJECT(row), "choice-row");

	if (!cl->filter || !*cl->filter || !r)
		return TRUE;
	return strstr(r->haystack, cl->filter) != NULL;
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	ChoiceList *cl = data;
	ChoiceRow *r = row ? g_object_get_data(G_OBJECT(row), "choice-row") : NULL;

	if (cl->quiet || !cl->changed)
		return;
	cl->changed(GTK_WIDGET(box), r ? r->id : NULL, cl->changed_data);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
	ChoiceList *cl = data;
	ChoiceRow *r = row ? g_object_get_data(G_OBJECT(row), "choice-row") : NULL;

	if (cl->activated && r)
		cl->activated(GTK_WIDGET(box), r->id, cl->activated_data);
}

static void cl_free(gpointer p)
{
	ChoiceList *cl = p;

	if (cl->scroll_idle)
		g_source_remove(cl->scroll_idle);
	g_free(cl->filter);
	g_free(cl);
}

GtkWidget *choice_list_new(void)
{
	GtkWidget *box = gtk_list_box_new();
	ChoiceList *cl = g_new0(ChoiceList, 1);

	gtk_list_box_set_selection_mode(GTK_LIST_BOX(box), GTK_SELECTION_SINGLE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(box), FALSE);
	gtk_list_box_set_filter_func(GTK_LIST_BOX(box), filter_func, cl, NULL);
	gtk_style_context_add_class(gtk_widget_get_style_context(box), "il-choice");
	g_object_set_data_full(G_OBJECT(box), "choice-list", cl, cl_free);
	g_signal_connect(box, "row-selected", G_CALLBACK(on_row_selected), cl);
	g_signal_connect(box, "row-activated", G_CALLBACK(on_row_activated), cl);

	cl->scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cl->scroller),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(cl->scroller),
						  FALSE);
	gtk_style_context_add_class(gtk_widget_get_style_context(cl->scroller),
				    "il-choice-scroller");
	gtk_container_add(GTK_CONTAINER(cl->scroller), box);
	gtk_widget_set_vexpand(cl->scroller, TRUE);
	return box;
}

GtkWidget *choice_list_scroller(GtkWidget *list)
{
	ChoiceList *cl = cl_data(list);

	return cl ? cl->scroller : list;
}

void choice_list_add(GtkWidget *list, const char *id, const char *label,
		     const char *sublabel)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	GtkWidget *l = gtk_label_new(label);
	ChoiceRow *r = g_new0(ChoiceRow, 1);
	char *hay;

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_style_context_add_class(gtk_widget_get_style_context(l),
				    "il-choice-label");
	gtk_box_pack_start(GTK_BOX(vbox), l, FALSE, FALSE, 0);
	if (sublabel && *sublabel) {
		GtkWidget *s = gtk_label_new(sublabel);

		gtk_label_set_xalign(GTK_LABEL(s), 0);
		gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
		gtk_style_context_add_class(gtk_widget_get_style_context(s),
					    "il-choice-sublabel");
		gtk_box_pack_start(GTK_BOX(vbox), s, FALSE, FALSE, 0);
	}
	gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
	gtk_container_add(GTK_CONTAINER(row), vbox);

	r->id = g_strdup(id);
	r->label = g_utf8_casefold(label, -1);
	hay = g_strconcat(label, " ", sublabel ? sublabel : "", " ", id, NULL);
	r->haystack = g_utf8_casefold(hay, -1);
	g_free(hay);
	g_object_set_data_full(G_OBJECT(row), "choice-row", r, row_data_free);
	gtk_widget_show_all(row);
	gtk_container_add(GTK_CONTAINER(list), row);
}

void choice_list_clear(GtkWidget *list)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(list));

	for (GList *l = children; l; l = l->next)
		gtk_widget_destroy(GTK_WIDGET(l->data));
	g_list_free(children);
}

static GtkListBoxRow *find_row(GtkWidget *list, const char *id)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(list));
	GtkListBoxRow *found = NULL;

	for (GList *l = children; l && id; l = l->next) {
		ChoiceRow *r = g_object_get_data(G_OBJECT(l->data), "choice-row");

		if (r && strcmp(r->id, id) == 0) {
			found = l->data;
			break;
		}
	}
	g_list_free(children);
	return found;
}

static gboolean scroll_idle(gpointer data)
{
	GtkWidget *list = data;
	ChoiceList *cl = cl_data(list);
	GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	GtkAdjustment *adj;
	GtkAllocation a;
	double page, value;

	cl->scroll_idle = 0;
	if (!row || !gtk_widget_get_child_visible(GTK_WIDGET(row)))
		return G_SOURCE_REMOVE;
	gtk_widget_get_allocation(GTK_WIDGET(row), &a);
	if (a.height <= 1)
		return G_SOURCE_REMOVE;
	adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(cl->scroller));
	page = gtk_adjustment_get_page_size(adj);
	value = gtk_adjustment_get_value(adj);
	/* a row out of sight comes to the top, a little below the edge */
	if (a.y < value || a.y + a.height > value + page)
		gtk_adjustment_set_value(adj, MAX(0, a.y - 4));
	return G_SOURCE_REMOVE;
}

void choice_list_scroll_to_selected(GtkWidget *list)
{
	ChoiceList *cl = cl_data(list);

	if (cl && !cl->scroll_idle)
		cl->scroll_idle = g_timeout_add(60, scroll_idle, list);
}

void choice_list_select(GtkWidget *list, const char *id)
{
	ChoiceList *cl = cl_data(list);
	GtkListBoxRow *row = find_row(list, id);

	cl->quiet = TRUE;
	if (row)
		gtk_list_box_select_row(GTK_LIST_BOX(list), row);
	else
		gtk_list_box_unselect_all(GTK_LIST_BOX(list));
	cl->quiet = FALSE;
	choice_list_scroll_to_selected(list);
}

gboolean choice_list_choose(GtkWidget *list, const char *id)
{
	ChoiceList *cl = cl_data(list);
	GtkListBoxRow *row = find_row(list, id);

	if (!row)
		return FALSE;
	/* a filter could hide it: show everything */
	choice_list_filter(list, NULL);
	/* from nothing, so the row is newly selected and reported */
	cl->quiet = TRUE;
	gtk_list_box_unselect_all(GTK_LIST_BOX(list));
	cl->quiet = FALSE;
	gtk_list_box_select_row(GTK_LIST_BOX(list), row);
	choice_list_scroll_to_selected(list);
	return TRUE;
}

const char *choice_list_selected(GtkWidget *list)
{
	GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	ChoiceRow *r = row ? g_object_get_data(G_OBJECT(row), "choice-row") : NULL;

	return r ? r->id : NULL;
}

void choice_list_filter(GtkWidget *list, const char *text)
{
	ChoiceList *cl = cl_data(list);

	g_free(cl->filter);
	cl->filter = text && *text ? g_utf8_casefold(text, -1) : NULL;
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(list));
	choice_list_scroll_to_selected(list);
}

int choice_list_visible_count(GtkWidget *list)
{
	ChoiceList *cl = cl_data(list);
	GList *children = gtk_container_get_children(GTK_CONTAINER(list));
	int n = 0;

	for (GList *l = children; l; l = l->next)
		if (filter_func(l->data, cl))
			n++;
	g_list_free(children);
	return n;
}

void choice_list_on_changed(GtkWidget *list, ChoiceChanged cb, gpointer data)
{
	ChoiceList *cl = cl_data(list);

	cl->changed = cb;
	cl->changed_data = data;
}

void choice_list_on_activated(GtkWidget *list, ChoiceChanged cb, gpointer data)
{
	ChoiceList *cl = cl_data(list);

	cl->activated = cb;
	cl->activated_data = data;
}

gboolean choice_list_select_first_visible(GtkWidget *list)
{
	ChoiceList *cl = cl_data(list);
	GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	GList *children;
	GtkListBoxRow *best = NULL;
	int best_rank = 4;

	/* the selected row is still shown and was not just typed past */
	if (sel && filter_func(sel, cl) && (!cl->filter || !*cl->filter))
		return TRUE;
	/* rank: the label starts with the text, a word of the label starts
	 * with it, the label holds it, only the sublabel does */
	children = gtk_container_get_children(GTK_CONTAINER(list));
	for (GList *l = children; l; l = l->next) {
		ChoiceRow *r = g_object_get_data(G_OBJECT(l->data), "choice-row");
		int rank;

		if (!r || !filter_func(l->data, cl))
			continue;
		if (!cl->filter || !*cl->filter || g_str_has_prefix(r->label, cl->filter))
			rank = 0;
		else if (strstr(r->label, cl->filter)) {
			const char *hit = strstr(r->label, cl->filter);

			rank = (hit > r->label && (hit[-1] == ' ' || hit[-1] == '(' ||
						   hit[-1] == '/')) ? 1 : 2;
		} else
			rank = 3;
		if (rank < best_rank) {
			best_rank = rank;
			best = l->data;
		}
	}
	g_list_free(children);
	if (sel && best && sel != best && filter_func(sel, cl)) {
		ChoiceRow *r = g_object_get_data(G_OBJECT(sel), "choice-row");

		/* keep a selection that ranks as well as the best */
		if (r && cl->filter && g_str_has_prefix(r->label, cl->filter))
			return TRUE;
	}
	if (!best)
		return FALSE;
	gtk_list_box_select_row(GTK_LIST_BOX(list), best);
	return TRUE;
}

GtkWidget *choice_list_focus_target(GtkWidget *list)
{
	GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));

	return sel ? GTK_WIDGET(sel) : list;
}

gboolean choice_list_is_shown(GtkWidget *list, const char *id)
{
	GtkListBoxRow *row = find_row(list, id);

	return row && filter_func(row, cl_data(list));
}
