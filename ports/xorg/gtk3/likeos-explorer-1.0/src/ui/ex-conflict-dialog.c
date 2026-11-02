// LikeOS -- File Explorer: questions an operation asks.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-conflict-dialog.h"
#include "ui/ex-style.h"
#include "backend/ex-util.h"

enum { R_REPLACE = 1, R_SKIP, R_KEEP_BOTH, R_MERGE, R_CANCEL = GTK_RESPONSE_CANCEL };

typedef struct {
	ExOperation *op;
	GtkWidget *all;
	gboolean answered;
	gboolean is_issue;
} Ask;

/* Runs when the dialog goes, answered or not: a dialog destroyed with its
 * window must not leave the operation waiting for an answer forever. */
static void ask_free(gpointer data, GClosure *closure)
{
	Ask *a = data;

	if (!a->answered) {
		if (a->is_issue)
			ex_issue_resolve(a->op, EX_ISSUE_CANCEL, FALSE);
		else
			ex_conflict_resolve(a->op, EX_CONFLICT_CANCEL, FALSE);
	}
	g_object_unref(a->op);
	g_free(a);
}

static char *describe(GFile *f, gboolean is_dir, guint64 size, gint64 mtime)
{
	char *t = ex_format_time(mtime);
	char *s;

	if (is_dir)
		s = g_strdup_printf("Folder · modified %s", t);
	else {
		char *sz = ex_format_size(size);

		s = g_strdup_printf("%s · modified %s", sz, t);
		g_free(sz);
	}
	g_free(t);
	return s;
}

static GtkWidget *choice(GtkWidget *d, int response, const char *title,
			 const char *detail)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *t = gtk_label_new(NULL);
	char *m = g_markup_printf_escaped("<b>%s</b>", title);

	gtk_label_set_markup(GTK_LABEL(t), m);
	g_free(m);
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	if (detail) {
		GtkWidget *dl = gtk_label_new(detail);

		gtk_label_set_xalign(GTK_LABEL(dl), 0);
		gtk_label_set_ellipsize(GTK_LABEL(dl), PANGO_ELLIPSIZE_MIDDLE);
		ex_style_add(dl, EX_CLASS_DIM);
		gtk_box_pack_start(GTK_BOX(box), dl, FALSE, FALSE, 0);
	}
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	atk_object_set_name(gtk_widget_get_accessible(b), title);
	g_object_set_data(G_OBJECT(b), "ex-response", GINT_TO_POINTER(response));
	return b;
}

static void choice_clicked(GtkButton *b, GtkDialog *d)
{
	gtk_dialog_response(d, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),
								 "ex-response")));
}

static void conflict_response(GtkDialog *d, int r, Ask *a)
{
	gboolean all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->all));
	ExConflictChoice c;

	switch (r) {
	case R_REPLACE:
		c = EX_CONFLICT_REPLACE;
		break;
	case R_SKIP:
		c = EX_CONFLICT_SKIP;
		break;
	case R_KEEP_BOTH:
		c = EX_CONFLICT_KEEP_BOTH;
		break;
	case R_MERGE:
		c = EX_CONFLICT_MERGE;
		break;
	default:
		c = EX_CONFLICT_CANCEL;
		break;
	}
	a->answered = TRUE;
	ex_conflict_resolve(a->op, c, all);
	gtk_widget_destroy(GTK_WIDGET(d));
}

void ex_conflict_dialog_show(GtkWindow *parent, ExOperation *op,
			     const ExConflict *c)
{
	char *name = g_file_get_basename(c->dest);
	gboolean dirs = c->source_is_dir && c->dest_is_dir;
	char *title = dirs ? g_strdup_printf("The destination already has a folder "
					     "named “%s”",
					     name) :
			     g_strdup_printf("The destination already has a file "
					     "named “%s”",
					     name);
	GtkWidget *d = gtk_dialog_new_with_buttons(
		dirs ? "Merge folders" : "Replace or skip files", parent,
		GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel", GTK_RESPONSE_CANCEL,
		NULL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *head = gtk_label_new(title);
	char *src_d = describe(c->source, c->source_is_dir, c->source_size,
			       c->source_mtime);
	char *dst_d = describe(c->dest, c->dest_is_dir, c->dest_size, c->dest_mtime);
	char *src_line = g_strdup_printf("New: %s", src_d);
	char *dst_line = g_strdup_printf("Existing: %s", dst_d);
	Ask *a = g_new0(Ask, 1);
	GtkWidget *btns[4];
	int n = 0;

	a->op = g_object_ref(op);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_label_set_line_wrap(GTK_LABEL(head), TRUE);
	gtk_label_set_xalign(GTK_LABEL(head), 0);
	ex_style_add(head, EX_CLASS_SECTION_TITLE);
	gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
	if (dirs) {
		btns[n++] = choice(d, R_MERGE, "Merge the folders",
				   "Items with the same name will be asked about "
				   "one by one");
	} else {
		btns[n++] = choice(d, R_REPLACE, "Replace the file in the destination",
				   src_line);
	}
	btns[n++] = choice(d, R_SKIP, dirs ? "Skip this folder" : "Skip this file",
			   dst_line);
	btns[n++] = choice(d, R_KEEP_BOTH, "Keep both",
			   "The new item gets a number added to its name");
	for (int i = 0; i < n; i++) {
		g_signal_connect(btns[i], "clicked", G_CALLBACK(choice_clicked), d);
		gtk_box_pack_start(GTK_BOX(box), btns[i], FALSE, FALSE, 0);
	}
	a->all = gtk_check_button_new_with_mnemonic("Do this for _all conflicts");
	gtk_box_pack_start(GTK_BOX(box), a->all, FALSE, FALSE, 6);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			  box);
	g_signal_connect_data(d, "response", G_CALLBACK(conflict_response), a,
			      ask_free, 0);
	ex_style_fixed_dialog(d);
	gtk_widget_show_all(d);
	gtk_widget_grab_focus(btns[0]);
	g_free(src_line);
	g_free(dst_line);
	g_free(src_d);
	g_free(dst_d);
	g_free(title);
	g_free(name);
}

enum { I_RETRY = 1, I_SKIP, I_DELETE };

static void issue_response(GtkDialog *d, int r, Ask *a)
{
	gboolean all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->all));
	ExIssueChoice c;

	switch (r) {
	case I_RETRY:
		c = EX_ISSUE_RETRY;
		break;
	case I_SKIP:
		c = EX_ISSUE_SKIP;
		break;
	case I_DELETE:
		c = EX_ISSUE_DELETE_INSTEAD;
		break;
	default:
		c = EX_ISSUE_CANCEL;
		break;
	}
	a->answered = TRUE;
	ex_issue_resolve(a->op, c, all);
	gtk_widget_destroy(GTK_WIDGET(d));
}

void ex_issue_dialog_show(GtkWindow *parent, ExOperation *op, const ExIssue *i)
{
	GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
					      "%s", i->message);
	Ask *a = g_new0(Ask, 1);
	GtkWidget *area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(d));

	a->op = g_object_ref(op);
	a->is_issue = TRUE;
	/* The operation names the window ("Deleting “x”"); the text says what
	 * went wrong. */
	gtk_window_set_title(GTK_WINDOW(d), ex_operation_get_title(op));
	gtk_dialog_add_button(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL);
	gtk_dialog_add_button(GTK_DIALOG(d), "_Skip", I_SKIP);
	if (i->can_retry)
		gtk_dialog_add_button(GTK_DIALOG(d), "_Try again", I_RETRY);
	if (i->can_delete_instead)
		gtk_dialog_add_button(GTK_DIALOG(d), "_Delete permanently", I_DELETE);
	gtk_dialog_set_default_response(GTK_DIALOG(d),
					i->can_retry ? I_RETRY : I_SKIP);
	a->all = gtk_check_button_new_with_mnemonic("Do this for _all items");
	gtk_box_pack_start(GTK_BOX(area), a->all, FALSE, FALSE, 0);
	g_signal_connect_data(d, "response", G_CALLBACK(issue_response), a,
			      ask_free, 0);
	ex_style_fixed_dialog(d);
	gtk_widget_show_all(d);
}
