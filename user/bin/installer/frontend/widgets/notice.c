/* LikeOS installer -- notices and the installation step list.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "widgets.h"

/* ---- notice -------------------------------------------------------- */

static const char *const notice_classes[] = {
	"il-notice-info", "il-notice-warning", "il-notice-error",
	"il-notice-success",
};

static const IconKind notice_icons[] = {
	ICON_INFO, ICON_WARNING, ICON_ERROR, ICON_CHECK,
};

GtkWidget *notice_new(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *icon = icon_new(ICON_INFO, 18);
	GtkWidget *label = gtk_label_new("");

	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 80);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
	g_object_set_data(G_OBJECT(box), "notice-icon", icon);
	g_object_set_data(G_OBJECT(box), "notice-label", label);
	gtk_style_context_add_class(gtk_widget_get_style_context(box), "il-notice");
	gtk_widget_set_no_show_all(box, TRUE);
	gtk_widget_show(icon);
	gtk_widget_show(label);
	return box;
}

void notice_set(GtkWidget *n, NoticeKind kind, const char *markup)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(n);
	GtkWidget *icon = g_object_get_data(G_OBJECT(n), "notice-icon");
	GtkWidget *label = g_object_get_data(G_OBJECT(n), "notice-label");

	if (!markup || !*markup) {
		gtk_widget_hide(n);
		return;
	}
	for (int i = 0; i < 4; i++)
		gtk_style_context_remove_class(sc, notice_classes[i]);
	gtk_style_context_add_class(sc, notice_classes[kind]);
	icon_set(icon, notice_icons[kind]);
	gtk_label_set_markup(GTK_LABEL(label), markup);
	gtk_widget_show(n);
}

void notice_hide(GtkWidget *n)
{
	gtk_widget_hide(n);
}

/* ---- step list ----------------------------------------------------- */

typedef struct {
	GtkWidget *mark_stack;   /* "pending", "running", "done", "failed" */
	GtkWidget *label;
	StepState state;
} StepRow;

static void step_rows_free(gpointer p)
{
	g_ptr_array_unref(p);
}

GtkWidget *step_list_new(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);

	gtk_style_context_add_class(gtk_widget_get_style_context(box), "il-steps");
	g_object_set_data_full(G_OBJECT(box), "step-rows", rows, step_rows_free);
	return box;
}

void step_list_add(GtkWidget *list, const char *title)
{
	GPtrArray *rows = g_object_get_data(G_OBJECT(list), "step-rows");
	StepRow *r = g_new0(StepRow, 1);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *pending = icon_new(ICON_DOT, 18);
	GtkWidget *running = ring_spinner_new(16);
	GtkWidget *done = icon_new(ICON_CHECK, 18);
	GtkWidget *failed = icon_new(ICON_CLOSE, 18);

	r->mark_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(r->mark_stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(r->mark_stack), 200);
	gtk_stack_add_named(GTK_STACK(r->mark_stack), pending, "pending");
	gtk_stack_add_named(GTK_STACK(r->mark_stack), running, "running");
	gtk_stack_add_named(GTK_STACK(r->mark_stack), done, "done");
	gtk_stack_add_named(GTK_STACK(r->mark_stack), failed, "failed");
	gtk_widget_set_size_request(r->mark_stack, 22, 22);
	gtk_style_context_add_class(gtk_widget_get_style_context(r->mark_stack),
				    "il-step-mark");

	r->label = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(r->label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(r->label), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(row), r->mark_stack, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), r->label, TRUE, TRUE, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(row), "il-step");
	gtk_style_context_add_class(gtk_widget_get_style_context(row),
				    "il-step-pending");
	g_object_set_data(G_OBJECT(r->mark_stack), "step-row-box", row);
	gtk_widget_show_all(row);
	gtk_box_pack_start(GTK_BOX(list), row, FALSE, FALSE, 0);
	g_ptr_array_add(rows, r);
}

void step_list_set_state(GtkWidget *list, int index, StepState state)
{
	static const char *const names[] = { "pending", "running", "done", "failed" };
	static const char *const classes[] = {
		"il-step-pending", "il-step-running", "il-step-done",
		"il-step-failed",
	};
	GPtrArray *rows = g_object_get_data(G_OBJECT(list), "step-rows");
	StepRow *r;
	GtkWidget *row;
	GtkStyleContext *sc;

	if (!rows || index < 0 || index >= (int)rows->len)
		return;
	r = g_ptr_array_index(rows, index);
	if (r->state == state && gtk_stack_get_visible_child_name(
		    GTK_STACK(r->mark_stack)) &&
	    g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(r->mark_stack)),
		      names[state]) == 0)
		return;
	r->state = state;
	gtk_stack_set_visible_child_name(GTK_STACK(r->mark_stack), names[state]);
	row = g_object_get_data(G_OBJECT(r->mark_stack), "step-row-box");
	sc = gtk_widget_get_style_context(row);
	for (int i = 0; i < 4; i++)
		gtk_style_context_remove_class(sc, classes[i]);
	gtk_style_context_add_class(sc, classes[state]);
}

void step_list_reset(GtkWidget *list)
{
	GPtrArray *rows = g_object_get_data(G_OBJECT(list), "step-rows");

	for (guint i = 0; rows && i < rows->len; i++)
		step_list_set_state(list, (int)i, STEP_PENDING);
}

int step_list_count(GtkWidget *list)
{
	GPtrArray *rows = g_object_get_data(G_OBJECT(list), "step-rows");

	return rows ? (int)rows->len : 0;
}
