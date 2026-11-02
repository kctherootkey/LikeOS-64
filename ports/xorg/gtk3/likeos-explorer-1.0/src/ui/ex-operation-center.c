// LikeOS -- File Explorer: the operation center.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-operation-center.h"
#include "ui/ex-style.h"
#include "backend/ex-util.h"
#include "services/ex-opqueue.h"

/* An operation still running after this long opens the center. */
#define EX_CENTER_DELAY_MS 800
/* After everything finished cleanly, an automatically opened center closes
 * this much later. */
#define EX_CENTER_LINGER_MS 1500

typedef struct {
	GtkWidget *win, *list, *empty;
	/* ExOperation* (a reference of its own: the queue lets go of finished
	 * operations when it clears or trims its history) -> GtkWidget row */
	GHashTable *rows;
	gboolean auto_shown;
	guint close_id;
} Center;

static Center *C;

typedef struct {
	GtkWidget *title, *bar, *detail, *cancel, *errors;
} Row;

static Row *row_of(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "ex-row");
}

static char *time_left(double rate, guint64 left)
{
	if (rate <= 1.0)
		return NULL;
	guint64 s = (guint64)((double)left / rate);

	if (s < 60)
		return g_strdup_printf("%u seconds left", (unsigned)s);
	if (s < 3600)
		return g_strdup_printf("%u minutes left", (unsigned)((s + 59) / 60));
	return g_strdup_printf("%u hours left", (unsigned)((s + 1799) / 3600));
}

static void refresh(ExOperation *op, GtkWidget *rw)
{
	Row *r = row_of(rw);
	ExOpState st = ex_operation_get_state(op);
	guint64 bd, bt;
	guint id, it;
	double f = ex_operation_get_fraction(op);
	char *cur = ex_operation_get_current(op);
	char *detail = NULL;

	ex_operation_get_counts(op, &bd, &bt, &id, &it);
	if (f < 0)
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(r->bar));
	else
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(r->bar), f);

	switch (st) {
	case EX_OP_QUEUED:
		detail = g_strdup("Waiting for another operation to finish");
		break;
	case EX_OP_RUNNING: {
		double rate = ex_operation_get_rate(op);

		if (f < 0) {
			detail = g_strdup("Counting items…");
			break;
		}
		/* "3 of 12 items · 18.2 MB/s · 2 minutes left · photo.jpg" */
		GString *d = g_string_new(NULL);
		char *left = bt > bd ? time_left(rate, bt - bd) : NULL;

		g_string_append_printf(d, "%u of %u items", id, it);
		if (rate > 1.0) {
			char *sp = ex_format_size((guint64)rate);

			g_string_append_printf(d, " · %s/s", sp);
			g_free(sp);
		}
		if (left)
			g_string_append_printf(d, " · %s", left);
		if (cur)
			g_string_append_printf(d, " · %s", cur);
		g_free(left);
		detail = g_string_free(d, FALSE);
		break;
	}
	case EX_OP_DONE:
		detail = g_strdup("Done");
		break;
	case EX_OP_CANCELLED:
		detail = g_strdup("Cancelled");
		break;
	case EX_OP_FAILED: {
		GPtrArray *errs = ex_operation_get_errors(op);

		detail = g_strdup_printf("Finished, %u item%s could not be done",
					 errs->len, errs->len == 1 ? "" : "s");
		break;
	}
	}
	gtk_label_set_text(GTK_LABEL(r->detail), detail);
	gtk_widget_set_visible(r->cancel,
			       st == EX_OP_RUNNING || st == EX_OP_QUEUED);
	g_free(detail);
	g_free(cur);
}

static void progress(ExOperation *op, GtkWidget *rw)
{
	refresh(op, rw);
}

static void cancel_clicked(GtkButton *b, ExOperation *op)
{
	ex_operation_cancel(op);
}

static GtkWidget *make_row(ExOperation *op)
{
	GtkWidget *rw = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	Row *r = g_new0(Row, 1);

	r->title = gtk_label_new(ex_operation_get_title(op));
	gtk_label_set_xalign(GTK_LABEL(r->title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(r->title), PANGO_ELLIPSIZE_MIDDLE);
	r->cancel = gtk_button_new_from_icon_name("process-stop",
						  GTK_ICON_SIZE_BUTTON);
	gtk_button_set_relief(GTK_BUTTON(r->cancel), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(r->cancel, "Cancel");
	atk_object_set_name(gtk_widget_get_accessible(r->cancel), "Cancel");
	g_signal_connect_object(r->cancel, "clicked", G_CALLBACK(cancel_clicked),
				op, 0);
	gtk_box_pack_start(GTK_BOX(top), r->title, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(top), r->cancel, FALSE, FALSE, 0);
	r->bar = gtk_progress_bar_new();
	r->detail = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(r->detail), 0);
	gtk_label_set_ellipsize(GTK_LABEL(r->detail), PANGO_ELLIPSIZE_MIDDLE);
	ex_style_add(r->detail, EX_CLASS_DIM);
	r->errors = gtk_expander_new("Details");
	gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), r->bar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), r->detail, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), r->errors, FALSE, FALSE, 0);
	gtk_container_set_border_width(GTK_CONTAINER(box), 10);
	gtk_container_add(GTK_CONTAINER(rw), box);
	g_object_set_data_full(G_OBJECT(rw), "ex-row", r, g_free);
	gtk_widget_show_all(rw);
	gtk_widget_hide(r->errors);
	g_signal_connect_object(op, "progress", G_CALLBACK(progress), rw, 0);
	refresh(op, rw);
	return rw;
}

static gboolean any_active(void)
{
	GPtrArray *a = ex_opqueue_active(ex_opqueue_get());
	gboolean any = a->len > 0;

	g_ptr_array_unref(a);
	return any;
}

static gboolean show_if_still_running(gpointer data)
{
	ExOperation *op = data;
	ExOpState st = ex_operation_get_state(op);

	/* Not over the question it is asking: ask again once answered. */
	if (st == EX_OP_RUNNING && ex_operation_is_waiting(op))
		return G_SOURCE_CONTINUE;
	if ((st == EX_OP_RUNNING || st == EX_OP_QUEUED) &&
	    !gtk_widget_get_visible(C->win)) {
		C->auto_shown = TRUE;
		ex_operation_center_present(NULL);
	}
	g_object_unref(op);
	return G_SOURCE_REMOVE;
}

static void added(ExOpQueue *q, ExOperation *op, gpointer data)
{
	GtkWidget *rw = make_row(op);

	gtk_list_box_prepend(GTK_LIST_BOX(C->list), rw);
	g_hash_table_insert(C->rows, g_object_ref(op), rw);
	gtk_widget_hide(C->empty);
	if (C->close_id) {
		g_source_remove(C->close_id);
		C->close_id = 0;
	}
	g_timeout_add(EX_CENTER_DELAY_MS, show_if_still_running, g_object_ref(op));
}

static gboolean close_later(gpointer data)
{
	C->close_id = 0;
	if (!any_active() && C->auto_shown)
		gtk_widget_hide(C->win);
	C->auto_shown = FALSE;
	return G_SOURCE_REMOVE;
}

static void finished(ExOpQueue *q, ExOperation *op, gpointer data)
{
	GtkWidget *rw = g_hash_table_lookup(C->rows, op);
	GPtrArray *errs = ex_operation_get_errors(op);

	if (!rw)
		return;
	refresh(op, rw);
	if (errs->len) {
		Row *r = row_of(rw);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

		for (guint i = 0; i < errs->len; i++) {
			GtkWidget *l = gtk_label_new(g_ptr_array_index(errs, i));

			gtk_label_set_xalign(GTK_LABEL(l), 0);
			gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
			gtk_label_set_selectable(GTK_LABEL(l), TRUE);
			gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
		}
		gtk_container_add(GTK_CONTAINER(r->errors), box);
		gtk_widget_show_all(r->errors);
		/* Failures stay on screen until looked at. */
		C->auto_shown = FALSE;
		if (!gtk_widget_get_visible(C->win))
			ex_operation_center_present(NULL);
	} else if (!any_active() && C->auto_shown && !C->close_id) {
		C->close_id = g_timeout_add(EX_CENTER_LINGER_MS, close_later, NULL);
	}
}

static void clear_clicked(GtkButton *b, gpointer data)
{
	GHashTableIter it;
	gpointer k, v;

	ex_opqueue_clear_finished(ex_opqueue_get());
	g_hash_table_iter_init(&it, C->rows);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		ExOpState st = ex_operation_get_state(k);

		if (st != EX_OP_RUNNING && st != EX_OP_QUEUED) {
			gtk_widget_destroy(v);
			g_hash_table_iter_remove(&it);
		}
	}
	if (g_hash_table_size(C->rows) == 0)
		gtk_widget_show(C->empty);
}

void ex_operation_center_init(void)
{
	GtkWidget *box, *sw, *bar, *clear;

	if (C)
		return;
	C = g_new0(Center, 1);
	C->rows = g_hash_table_new_full(NULL, NULL, g_object_unref, NULL);
	C->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(C->win), "File operations");
	gtk_window_set_default_size(GTK_WINDOW(C->win), 480, 360);
	gtk_window_set_icon_name(GTK_WINDOW(C->win), "system-file-manager");
	g_signal_connect(C->win, "delete-event",
			 G_CALLBACK(gtk_widget_hide_on_delete), NULL);
	ex_style_add(C->win, EX_CLASS_WINDOW);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	C->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(C->list), GTK_SELECTION_NONE);
	atk_object_set_name(gtk_widget_get_accessible(C->list), "Operations");
	gtk_container_add(GTK_CONTAINER(sw), C->list);
	C->empty = gtk_label_new("No file operations in this session.");
	ex_style_add(C->empty, EX_CLASS_PLACEHOLDER);
	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(bar), 8);
	clear = gtk_button_new_with_mnemonic("_Clear finished");
	g_signal_connect(clear, "clicked", G_CALLBACK(clear_clicked), NULL);
	gtk_box_pack_end(GTK_BOX(bar), clear, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), C->empty, FALSE, FALSE, 24);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(box), bar, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(C->win), box);
	gtk_widget_show_all(box);

	g_signal_connect(ex_opqueue_get(), "added", G_CALLBACK(added), NULL);
	g_signal_connect(ex_opqueue_get(), "finished", G_CALLBACK(finished), NULL);
}

void ex_operation_center_present(GtkWindow *parent)
{
	ex_operation_center_init();
	if (!parent) {
		GList *tops = gtk_window_list_toplevels();

		for (GList *l = tops; l && !parent; l = l->next)
			if (l->data != C->win && GTK_IS_APPLICATION_WINDOW(l->data) &&
			    gtk_window_is_active(l->data))
				parent = l->data;
		g_list_free(tops);
	}
	if (parent && parent != GTK_WINDOW(C->win))
		gtk_window_set_transient_for(GTK_WINDOW(C->win), parent);
	gtk_window_present(GTK_WINDOW(C->win));
}
