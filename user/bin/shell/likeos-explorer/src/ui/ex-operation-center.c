// LikeOS Explorer: the operation center.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-operation-center.h"
#include "ui/ex-conflict-dialog.h"
#include "ui/ex-progress-item.h"
#include "ui/ex-style.h"
#include "services/ex-opqueue.h"

/* An operation still running after this long opens the center. */
#define EX_CENTER_DELAY_MS 800
/* After everything finished cleanly, an automatically opened center closes
 * this much later. */
#define EX_CENTER_LINGER_MS 1500
/* The window's width inside its frame. */
#define EX_CENTER_WIDTH 460
/* Taller than this, the operations scroll. */
#define EX_CENTER_MAX_HEIGHT 640

typedef struct {
	GtkWidget *win, *list, *empty;
	GtkWidget *more, *more_img, *more_label, *clear;
	/* ExOperation* (a reference of its own: the queue lets go of finished
	 * operations when it clears or trims its history) -> GtkListBoxRow */
	GHashTable *rows;
	gboolean auto_shown;
	guint close_id;
	/* More details (the graph) or fewer (a bar), for every operation. */
	gboolean details;
	/* Order of arrival, and of finishing. */
	guint seq;
	/* What keeps the program running (see hold()): the operations not
	 * finished yet, and the window while it shows. */
	GHashTable *holding;
	gboolean window_held;
} Center;

static Center *C;

static ExOperation *row_op(GtkListBoxRow *r)
{
	return ex_progress_item_get_operation(gtk_bin_get_child(GTK_BIN(r)));
}

static gboolean is_active(ExOperation *op)
{
	ExOpState st = ex_operation_get_state(op);

	return st == EX_OP_RUNNING || st == EX_OP_QUEUED;
}

/* ---- keeping the program running ----------------------------------------- */

/* The program stays while an operation is not finished and while this
 * window shows, with no Explorer window open as well: this window is not
 * one of the application's own, so without a hold the program would end
 * when the last Explorer window closes -- in the middle of a copy -- and
 * an operation handed over on the command line would never run. */
static void hold(gboolean on)
{
	GApplication *app = g_application_get_default();

	if (!app)
		return;
	if (on)
		g_application_hold(app);
	else
		g_application_release(app);
}

static void window_shown(GtkWidget *w, gpointer data)
{
	if (!C->window_held) {
		C->window_held = TRUE;
		hold(TRUE);
	}
}

static void set_details(gboolean more);

static void window_hidden(GtkWidget *w, gpointer data)
{
	if (!C->details)
		set_details(TRUE);
	if (C->window_held) {
		C->window_held = FALSE;
		hold(FALSE);
	}
}

/* ---- the window's title and footer ------------------------------------------ */

/* "33% complete" for one operation; "2 operations running" for more. */
static void update_title(void)
{
	GPtrArray *a = ex_opqueue_active(ex_opqueue_get());
	char *t;

	if (a->len == 1)
		t = ex_progress_status_text(g_ptr_array_index(a, 0));
	else if (a->len > 1)
		t = g_strdup_printf("%u operations running", a->len);
	else
		t = g_strdup("File operations");
	if (g_strcmp0(t, gtk_window_get_title(GTK_WINDOW(C->win))))
		gtk_window_set_title(GTK_WINDOW(C->win), t);
	g_free(t);
	g_ptr_array_unref(a);
}

static void update_footer(void)
{
	GHashTableIter it;
	gpointer k;
	gboolean any_done = FALSE, any_active = FALSE;

	g_hash_table_iter_init(&it, C->rows);
	while (g_hash_table_iter_next(&it, &k, NULL)) {
		if (is_active(k))
			any_active = TRUE;
		else
			any_done = TRUE;
	}
	gtk_widget_set_visible(C->clear, any_done);
	gtk_widget_set_visible(C->more, any_active);
	gtk_image_set_from_icon_name(
		GTK_IMAGE(C->more_img),
		C->details ? (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(),
						      "likeos-chevron-up") ?
				      "likeos-chevron-up" :
				      "pan-up-symbolic") :
			     (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(),
						      "likeos-chevron-down") ?
				      "likeos-chevron-down" :
				      "pan-down-symbolic"),
		GTK_ICON_SIZE_MENU);
	gtk_label_set_text(GTK_LABEL(C->more_label),
			   C->details ? "Fewer details" : "More details");
}

/* More details or fewer, for every operation shown. */
static void set_details(gboolean more)
{
	GHashTableIter it;
	gpointer v;

	C->details = more;
	g_hash_table_iter_init(&it, C->rows);
	while (g_hash_table_iter_next(&it, NULL, &v))
		ex_progress_item_set_details(gtk_bin_get_child(GTK_BIN(v)),
					     C->details);
	update_footer();
}

/* Fewer details lasts while the window stays open: each time it opens
 * again it shows the speed graph. */
static void more_clicked(GtkButton *b, gpointer data)
{
	set_details(!C->details);
}

/* ---- the list ------------------------------------------------------------- */

/* Running and waiting operations first, in the order they came; finished
 * ones below them, the latest first. */
static int sort_rows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data)
{
	gboolean aa = is_active(row_op(a)), ba = is_active(row_op(b));
	guint as = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(a), "ex-seq"));
	guint bs = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "ex-seq"));

	if (aa != ba)
		return aa ? -1 : 1;
	if (aa)
		return as < bs ? -1 : as > bs;
	return as > bs ? -1 : as < bs;
}

static void op_progress(ExOperation *op, gpointer data)
{
	update_title();
}

static GtkWidget *make_row(ExOperation *op)
{
	GtkWidget *rw = gtk_list_box_row_new();
	GtkWidget *item = ex_progress_item_new(op);

	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(rw), FALSE);
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(rw), FALSE);
	ex_progress_item_set_details(item, C->details);
	gtk_container_add(GTK_CONTAINER(rw), item);
	g_object_set_data(G_OBJECT(rw), "ex-seq", GUINT_TO_POINTER(++C->seq));
	gtk_widget_show(rw);
	g_signal_connect_object(op, "progress", G_CALLBACK(op_progress), rw, 0);
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

	if (!g_hash_table_contains(C->holding, op)) {
		g_hash_table_add(C->holding, g_object_ref(op));
		hold(TRUE);
	}

	gtk_container_add(GTK_CONTAINER(C->list), rw);
	g_hash_table_insert(C->rows, g_object_ref(op), rw);
	if (C->close_id) {
		g_source_remove(C->close_id);
		C->close_id = 0;
	}
	update_title();
	update_footer();
	g_timeout_add(EX_CENTER_DELAY_MS, show_if_still_running, g_object_ref(op));
}

static void started(ExOpQueue *q, ExOperation *op, gpointer data)
{
	update_title();
}

static gboolean close_later(gpointer data)
{
	C->close_id = 0;
	if (!any_active() && C->auto_shown)
		gtk_widget_hide(C->win);
	C->auto_shown = FALSE;
	return G_SOURCE_REMOVE;
}

static void row_finished(GtkWidget *rw, ExOperation *op, GPtrArray *errs)
{
	ex_progress_item_finish(gtk_bin_get_child(GTK_BIN(rw)));
	/* Among the finished ones it goes first. */
	g_object_set_data(G_OBJECT(rw), "ex-seq", GUINT_TO_POINTER(++C->seq));
	gtk_list_box_invalidate_sort(GTK_LIST_BOX(C->list));
	update_title();
	update_footer();
	if (errs->len) {
		/* Failures stay on screen until looked at. */
		C->auto_shown = FALSE;
		if (!gtk_widget_get_visible(C->win))
			ex_operation_center_present(NULL);
	} else if (!any_active() && C->auto_shown && !C->close_id) {
		C->close_id = g_timeout_add(EX_CENTER_LINGER_MS, close_later, NULL);
	}
}

static void finished(ExOpQueue *q, ExOperation *op, gpointer data)
{
	GtkWidget *rw = g_hash_table_lookup(C->rows, op);
	GPtrArray *errs = ex_operation_get_errors(op);

	if (rw)
		row_finished(rw, op, errs);
	/* Last: a window that opens for the errors holds the program first. */
	if (g_hash_table_remove(C->holding, op))
		hold(FALSE);
}

static void clear_clicked(GtkButton *b, gpointer data)
{
	GHashTableIter it;
	gpointer k, v;

	ex_opqueue_clear_finished(ex_opqueue_get());
	g_hash_table_iter_init(&it, C->rows);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		if (!is_active(k)) {
			gtk_widget_destroy(v);
			g_hash_table_iter_remove(&it);
		}
	}
	update_footer();
}

/* ---- the window ------------------------------------------------------------ */

/* Minimise and close, but no maximise -- the window keeps the size of what
 * it shows, which also makes the window manager grey Maximize.  Unlike a
 * dialog it can be minimised: a long copy is put out of the way, not
 * waited on. */
static void center_realized(GtkWidget *w, gpointer data)
{
	gdk_window_set_functions(gtk_widget_get_window(w),
				 GDK_FUNC_MOVE | GDK_FUNC_MINIMIZE |
					 GDK_FUNC_CLOSE);
}

static GtkWidget *footer_new(void)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	gtk_style_context_add_class(gtk_widget_get_style_context(bar),
				    "ex-op-footer");
	C->more = gtk_button_new();
	C->more_img = gtk_image_new();
	C->more_label = gtk_label_new("");
	gtk_style_context_add_class(gtk_widget_get_style_context(C->more_img),
				    "ex-op-chevron");
	gtk_box_pack_start(GTK_BOX(inner), C->more_img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(inner), C->more_label, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(C->more), inner);
	gtk_button_set_relief(GTK_BUTTON(C->more), GTK_RELIEF_NONE);
	gtk_style_context_add_class(gtk_widget_get_style_context(C->more),
				    "ex-op-more");
	g_signal_connect(C->more, "clicked", G_CALLBACK(more_clicked), NULL);

	C->clear = gtk_button_new_with_mnemonic("_Clear finished");
	gtk_button_set_relief(GTK_BUTTON(C->clear), GTK_RELIEF_NONE);
	gtk_style_context_add_class(gtk_widget_get_style_context(C->clear),
				    "ex-op-clear");
	g_signal_connect(C->clear, "clicked", G_CALLBACK(clear_clicked), NULL);
	gtk_box_pack_start(GTK_BOX(bar), C->more, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(bar), C->clear, FALSE, FALSE, 0);
	gtk_widget_show_all(bar);
	return bar;
}

void ex_operation_center_init(void)
{
	GtkWidget *box, *sw;

	if (C)
		return;
	C = g_new0(Center, 1);
	C->details = TRUE;
	C->rows = g_hash_table_new_full(NULL, NULL, g_object_unref, NULL);
	C->holding = g_hash_table_new_full(NULL, NULL, g_object_unref, NULL);
	C->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(C->win), "File operations");
	gtk_window_set_resizable(GTK_WINDOW(C->win), FALSE);
	gtk_window_set_icon_name(GTK_WINDOW(C->win), "likeos-clock");
	g_signal_connect(C->win, "delete-event",
			 G_CALLBACK(gtk_widget_hide_on_delete), NULL);
	g_signal_connect(C->win, "realize", G_CALLBACK(center_realized), NULL);
	g_signal_connect(C->win, "show", G_CALLBACK(window_shown), NULL);
	g_signal_connect(C->win, "hide", G_CALLBACK(window_hidden), NULL);
	ex_style_add(C->win, EX_CLASS_WINDOW);
	ex_style_add(C->win, "ex-ops-window");

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_size_request(box, EX_CENTER_WIDTH, -1);
	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw),
							 TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw),
						   EX_CENTER_MAX_HEIGHT);
	C->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(C->list), GTK_SELECTION_NONE);
	gtk_list_box_set_sort_func(GTK_LIST_BOX(C->list), sort_rows, NULL, NULL);
	gtk_style_context_add_class(gtk_widget_get_style_context(C->list),
				    "ex-op-list");
	atk_object_set_name(gtk_widget_get_accessible(C->list), "Operations");
	C->empty = gtk_label_new("No file operations in this session.");
	ex_style_add(C->empty, EX_CLASS_PLACEHOLDER);
	gtk_style_context_add_class(gtk_widget_get_style_context(C->empty),
				    "ex-op-empty");
	gtk_widget_show(C->empty);
	gtk_list_box_set_placeholder(GTK_LIST_BOX(C->list), C->empty);
	gtk_container_add(GTK_CONTAINER(sw), C->list);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(box), footer_new(), FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(C->win), box);
	gtk_widget_show_all(box);
	update_footer();

	g_signal_connect(ex_opqueue_get(), "added", G_CALLBACK(added), NULL);
	g_signal_connect(ex_opqueue_get(), "started", G_CALLBACK(started), NULL);
	g_signal_connect(ex_opqueue_get(), "finished", G_CALLBACK(finished), NULL);
}

/* Over the middle of `parent', the first time it is shown.  The window
 * manager places a window where it likes unless the position is the
 * user's (USPosition), so it is given as that. */
static void place_over(GtkWindow *parent)
{
	int px, py, pw, ph;
	GtkRequisition nat;
	GdkGeometry geo = { 0 };

	gtk_window_get_position(parent, &px, &py);
	gtk_window_get_size(parent, &pw, &ph);
	gtk_widget_get_preferred_size(C->win, NULL, &nat);
	gtk_window_set_geometry_hints(GTK_WINDOW(C->win), NULL, &geo,
				      GDK_HINT_USER_POS);
	gtk_window_move(GTK_WINDOW(C->win), px + (pw - nat.width) / 2,
			py + (ph - nat.height) / 2);
}

/* The Explorer window with the focus; when none has it (it moved to a
 * question, or to another program), one of Explorer's windows all the
 * same; NULL when none is open. */
static GtkWindow *front_window(void)
{
	GList *tops = gtk_window_list_toplevels();
	GtkWindow *parent = NULL, *any = NULL;

	for (GList *l = tops; l && !parent; l = l->next) {
		if (l->data == C->win || !GTK_IS_APPLICATION_WINDOW(l->data) ||
		    !gtk_widget_get_visible(l->data))
			continue;
		if (gtk_window_is_active(l->data))
			parent = l->data;
		else if (!any)
			any = l->data;
	}
	g_list_free(tops);
	return parent ? parent : any;
}

void ex_operation_center_present(GtkWindow *parent)
{
	ex_operation_center_init();
	if (!parent)
		parent = front_window();
	/* Its own window in the taskbar, not one belonging to an Explorer
	 * window: placed over the window it came from, not tied to it. */
	if (parent && parent != GTK_WINDOW(C->win) &&
	    !gtk_widget_get_visible(C->win))
		place_over(parent);
	update_title();
	gtk_window_present(GTK_WINDOW(C->win));
}

/* ---- operations no window started ------------------------------------------ */

/* A question such an operation asks goes over the Explorer window in
 * front, or, with none open, over this window, shown for it. */
static GtkWindow *question_parent(void)
{
	GtkWindow *parent = front_window();

	if (!parent) {
		ex_operation_center_present(NULL);
		parent = GTK_WINDOW(C->win);
	}
	return parent;
}

static void ask_conflict(ExOperation *op, ExConflict *c, gpointer data)
{
	ex_conflict_dialog_show(question_parent(), op, c);
}

static void ask_issue(ExOperation *op, ExIssue *i, gpointer data)
{
	ex_issue_dialog_show(question_parent(), op, i);
}

void ex_operation_center_run(ExOperation *op, gboolean undoable)
{
	ex_operation_center_init();
	/* Connected before the queue can start it. */
	g_signal_connect(op, "conflict", G_CALLBACK(ask_conflict), NULL);
	g_signal_connect(op, "issue", G_CALLBACK(ask_issue), NULL);
	ex_opqueue_add(ex_opqueue_get(), op, undoable);
}
