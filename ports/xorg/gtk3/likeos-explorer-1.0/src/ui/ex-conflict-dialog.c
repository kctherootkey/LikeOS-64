// LikeOS Explorer: questions an operation asks.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-conflict-dialog.h"
#include "ui/ex-progress-item.h"
#include "ui/ex-style.h"
#include "backend/ex-location.h"
#include "backend/ex-util.h"

/* The dialogs' responses. */
enum {
	R_REPLACE = 1,
	R_SKIP,
	R_KEEP_BOTH,
	R_MERGE,
	R_RETRY,
	R_DELETE,
	R_CANCEL = GTK_RESPONSE_CANCEL,
};

/* The message boxes' width, inside the frame. */
#define EX_ASK_WIDTH 500

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

/* ---- pieces ---------------------------------------------------------------- */

static void add_class(GtkWidget *w, const char *klass)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), klass);
}

static const char *icon_name(const char *want, const char *fallback)
{
	return gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), want) ?
		       want :
		       fallback;
}

static GtkWidget *label(const char *text, const char *klass, gboolean wrap)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	if (wrap) {
		gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
		/* As wide as the dialog, never wider. */
		gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
	} else {
		gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
	}
	if (klass)
		add_class(l, klass);
	return l;
}

/* The picture of an item: its type's icon, guessed from the name (the
 * item itself is not read here). */
static GtkWidget *item_icon(GFile *f, gboolean is_dir)
{
	GtkWidget *img;

	if (is_dir) {
		img = gtk_image_new_from_icon_name("folder", GTK_ICON_SIZE_DIALOG);
	} else {
		char *name = g_file_get_basename(f);
		char *type = g_content_type_guess(name, NULL, 0, NULL);
		GIcon *gi = g_content_type_get_icon(type);

		img = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_DIALOG);
		g_object_unref(gi);
		g_free(type);
		g_free(name);
	}
	gtk_image_set_pixel_size(GTK_IMAGE(img), 40);
	gtk_widget_set_valign(img, GTK_ALIGN_START);
	return img;
}

/* "3.92 GB" over "Modified Today, 14:02" -- a folder's date alone. */
static char *item_facts(gboolean is_dir, guint64 size, gint64 mtime)
{
	char *t = ex_format_time(mtime);
	char *s;

	if (is_dir) {
		s = g_strdup_printf("Modified %s", t);
	} else {
		char *sz = ex_format_size(size);

		s = g_strdup_printf("%s\nModified %s", sz, t);
		g_free(sz);
	}
	g_free(t);
	return s;
}

/* The item a question is about: icon, name in bold (unless `with_name' is
 * FALSE), facts. */
static GtkWidget *item_row(GFile *f, gboolean is_dir, gboolean with_name,
			   const char *facts)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	char *name = g_file_get_basename(f);

	add_class(row, "ex-ask-item");
	gtk_box_pack_start(GTK_BOX(row), item_icon(f, is_dir), FALSE, FALSE, 0);
	if (with_name)
		gtk_box_pack_start(GTK_BOX(col),
				   label(name, "ex-ask-item-name", FALSE), FALSE,
				   FALSE, 0);
	if (facts)
		gtk_box_pack_start(GTK_BOX(col),
				   label(facts, "ex-ask-item-detail", TRUE), FALSE,
				   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
	g_free(name);
	return row;
}

static void button_clicked(GtkButton *b, GtkDialog *d)
{
	gtk_dialog_response(d, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),
								 "ex-response")));
}

/* A push button of the footer's row, packed from the right: the default
 * answer goes in first. */
static GtkWidget *push(GtkWidget *d, GtkWidget *row, const char *mnemonic,
		       int response)
{
	GtkWidget *b = gtk_button_new_with_mnemonic(mnemonic);

	g_object_set_data(G_OBJECT(b), "ex-response", GINT_TO_POINTER(response));
	g_signal_connect(b, "clicked", G_CALLBACK(button_clicked), d);
	gtk_box_pack_end(GTK_BOX(row), b, FALSE, FALSE, 0);
	return b;
}

static void more_toggled(GtkButton *b, GtkWidget *text)
{
	gboolean show = !gtk_widget_get_visible(text);
	GtkWidget *img = g_object_get_data(G_OBJECT(b), "ex-img");
	GtkWidget *l = g_object_get_data(G_OBJECT(b), "ex-label");

	gtk_widget_set_visible(text, show);
	gtk_image_set_from_icon_name(
		GTK_IMAGE(img),
		show ? icon_name("likeos-chevron-up-symbolic", "pan-up-symbolic") :
		       icon_name("likeos-chevron-down-symbolic",
				 "pan-down-symbolic"),
		GTK_ICON_SIZE_MENU);
	gtk_label_set_text(GTK_LABEL(l), show ? "Hide details" : "Details");
}

/* "Details" at the footer's left, opening the full error text above the
 * footer. */
static void more_toggle(GtkWidget *box, GtkWidget *footer, const char *text)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *img = gtk_image_new_from_icon_name(
		icon_name("likeos-chevron-down-symbolic", "pan-down-symbolic"),
		GTK_ICON_SIZE_MENU);
	GtkWidget *l = gtk_label_new("Details");
	GtkWidget *t = label(text, "ex-ask-more-text", TRUE);

	gtk_label_set_selectable(GTK_LABEL(t), TRUE);
	gtk_box_pack_start(GTK_BOX(inner), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(inner), img, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), inner);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	add_class(b, "ex-ask-more");
	g_object_set_data(G_OBJECT(b), "ex-img", img);
	g_object_set_data(G_OBJECT(b), "ex-label", l);
	g_signal_connect(b, "clicked", G_CALLBACK(more_toggled), t);
	gtk_box_pack_start(GTK_BOX(footer), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(t, TRUE);
}

/* A question window: white, its buttons in the footer band of the content
 * (the dialog's own button area stays empty), fixed in size. */
static GtkWidget *ask_dialog(GtkWindow *parent, const char *title)
{
	GtkWidget *d = gtk_dialog_new_with_buttons(title, parent,
						   GTK_DIALOG_DESTROY_WITH_PARENT,
						   NULL, NULL);

	/* The button box stays empty; the theme gives it no size. */
	add_class(d, "ex-ask");
	gtk_box_set_spacing(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			    0);
	ex_style_fixed_dialog(d);
	return d;
}

/* The message: the badge with `icon' (a symbolic name) at the left; at the
 * right the question, the operation's line under it, and room for the
 * rest -- the box returned. */
static GtkWidget *body_new(GtkWidget *d, ExOperation *op, const char *icon,
			   const char *fallback, const char *instruction,
			   gboolean warn)
{
	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *img = gtk_image_new_from_icon_name(icon_name(icon, fallback),
						      GTK_ICON_SIZE_LARGE_TOOLBAR);
	GtkWidget *head = ex_progress_header_new(op);

	add_class(body, "ex-ask-body");
	gtk_widget_set_size_request(body, EX_ASK_WIDTH, -1);
	gtk_image_set_pixel_size(GTK_IMAGE(img), 20);
	gtk_box_pack_start(GTK_BOX(badge), img, TRUE, TRUE, 0);
	gtk_widget_set_size_request(badge, 40, 40);
	gtk_widget_set_valign(badge, GTK_ALIGN_START);
	add_class(badge, "ex-ask-badge");
	if (warn)
		add_class(badge, "warning");
	gtk_box_pack_start(GTK_BOX(body), badge, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col),
			   label(instruction, "ex-ask-instruction", TRUE), FALSE,
			   FALSE, 0);
	add_class(head, "ex-ask-context");
	gtk_box_pack_start(GTK_BOX(col), head, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), col, TRUE, TRUE, 0);
	g_object_set_data(G_OBJECT(body), "ex-column", col);
	return body;
}

static GtkWidget *body_column(GtkWidget *body)
{
	return g_object_get_data(G_OBJECT(body), "ex-column");
}

/* The band along the bottom: `all' (a check button, or NULL) at the left,
 * the answers at the right. */
static GtkWidget *footer_new(GtkWidget *all)
{
	GtkWidget *f = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	add_class(f, "ex-ask-footer");
	if (all) {
		add_class(all, "ex-ask-all");
		gtk_widget_set_valign(all, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(f), all, FALSE, FALSE, 0);
	}
	return f;
}

/* The dialog's content: the message (packed into `box' first) over the
 * footer. */
static void ask_finish(GtkWidget *d, GtkWidget *box, GtkWidget *footer)
{
	gtk_box_pack_end(GTK_BOX(box), footer, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			  box);
}

/* ---- a name taken --------------------------------------------------------------- */

static void conflict_answer(GtkDialog *d, Ask *a, ExConflictChoice c,
			    gboolean all)
{
	a->answered = TRUE;
	ex_conflict_resolve(a->op, c, all);
	gtk_widget_destroy(GTK_WIDGET(d));
}

static void conflict_response(GtkDialog *d, int r, Ask *a)
{
	gboolean all = a->all &&
		       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->all));

	switch (r) {
	case R_REPLACE:
		conflict_answer(d, a, EX_CONFLICT_REPLACE, all);
		break;
	case R_SKIP:
		conflict_answer(d, a, EX_CONFLICT_SKIP, all);
		break;
	case R_KEEP_BOTH:
		conflict_answer(d, a, EX_CONFLICT_KEEP_BOTH, all);
		break;
	case R_MERGE:
		conflict_answer(d, a, EX_CONFLICT_MERGE, all);
		break;
	default:
		conflict_answer(d, a, EX_CONFLICT_CANCEL, FALSE);
		break;
	}
}

static char *place_name(GFile *dir)
{
	ExLocation *l;
	char *s;

	if (!dir)
		return g_strdup("the source");
	l = ex_location_new_folder(dir);
	s = ex_location_display_name(l);
	ex_location_unref(l);
	return s;
}

/* One side of the comparison: a small heading ("Coming in from
 * Downloads"), and the item with its facts, in a card. */
static GtkWidget *side(const char *heading, GFile *f, gboolean is_dir,
		       guint64 size, gint64 mtime)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	char *facts = item_facts(is_dir, size, mtime);

	add_class(box, "ex-ask-side");
	gtk_box_pack_start(GTK_BOX(box),
			   label(heading, "ex-ask-side-title", FALSE), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), item_row(f, is_dir, FALSE, facts),
			   FALSE, FALSE, 0);
	g_free(facts);
	return box;
}

/* Two folders of one name: merge them, or leave this one out. */
static void folder_conflict(GtkWindow *parent, ExOperation *op,
			    const ExConflict *c, Ask *a)
{
	GtkWidget *d = ask_dialog(parent, "Merge Folders");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GFile *dst_dir = g_file_get_parent(c->dest);
	char *dn = place_name(dst_dir);
	char *name = g_file_get_basename(c->dest);
	char *head = g_strdup_printf("“%s” already exists in %s. Merge the two "
				     "folders?",
				     name, dn);
	GtkWidget *body = body_new(d, op, "likeos-new-folder-symbolic",
				   "folder-new-symbolic", head, FALSE);
	GtkWidget *col = body_column(body);
	char *facts = item_facts(TRUE, 0, c->dest_mtime);
	GtkWidget *footer, *yes;

	gtk_box_pack_start(GTK_BOX(box), body, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col),
			   label("Merging puts the contents of both folders "
				 "together in one. Items with the same name are "
				 "asked about one at a time.",
				 "ex-ask-text", TRUE),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), item_row(c->dest, TRUE, TRUE, facts),
			   FALSE, FALSE, 0);
	a->all = gtk_check_button_new_with_mnemonic("Apply to _all folders");
	footer = footer_new(a->all);
	yes = push(d, footer, "_Merge", R_MERGE);
	add_class(yes, "suggested-action");
	push(d, footer, "_Skip", R_SKIP);
	push(d, footer, "_Cancel", R_CANCEL);
	ask_finish(d, box, footer);
	g_signal_connect_data(d, "response", G_CALLBACK(conflict_response), a,
			      ask_free, 0);
	gtk_widget_show_all(d);
	gtk_widget_grab_focus(yes);
	g_free(facts);
	g_free(head);
	g_free(name);
	g_free(dn);
	g_clear_object(&dst_dir);
}

void ex_conflict_dialog_show(GtkWindow *parent, ExOperation *op,
			     const ExConflict *c)
{
	Ask *a = g_new0(Ask, 1);
	GtkWidget *d, *box, *body, *col, *sides, *footer, *def;
	GFile *src_dir, *dst_dir;
	char *name, *head, *sn, *dn, *from, *there;

	a->op = g_object_ref(op);
	if (c->source_is_dir && c->dest_is_dir) {
		folder_conflict(parent, op, c, a);
		return;
	}

	d = ask_dialog(parent, "Name Already Taken");
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	src_dir = g_file_get_parent(c->source);
	dst_dir = g_file_get_parent(c->dest);
	sn = place_name(src_dir);
	dn = place_name(dst_dir);
	name = g_file_get_basename(c->dest);
	head = g_strdup_printf("“%s” already exists in %s", name, dn);
	body = body_new(d, op, "likeos-copy-symbolic", "edit-copy-symbolic", head,
			FALSE);
	gtk_box_pack_start(GTK_BOX(box), body, FALSE, FALSE, 0);
	col = body_column(body);
	gtk_box_pack_start(GTK_BOX(col),
			   label("Replace it with the item coming in, skip this "
				 "item, or keep both – the new one then gets a "
				 "number added to its name.",
				 "ex-ask-text", TRUE),
			   FALSE, FALSE, 0);

	/* The two items, side by side. */
	from = g_strdup_printf("Coming in from %s", sn);
	there = g_strdup_printf("Already in %s", dn);
	sides = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_set_homogeneous(GTK_BOX(sides), TRUE);
	gtk_box_pack_start(GTK_BOX(sides),
			   side(from, c->source, c->source_is_dir, c->source_size,
				c->source_mtime),
			   TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(sides),
			   side(there, c->dest, c->dest_is_dir, c->dest_size,
				c->dest_mtime),
			   TRUE, TRUE, 0);
	add_class(sides, "ex-ask-sides");
	gtk_box_pack_start(GTK_BOX(col), sides, FALSE, FALSE, 0);

	a->all = gtk_check_button_new_with_mnemonic("Apply to _all conflicts");
	footer = footer_new(a->all);
	def = push(d, footer, "_Replace", R_REPLACE);
	add_class(def, "suggested-action");
	push(d, footer, "_Keep both", R_KEEP_BOTH);
	push(d, footer, "_Skip", R_SKIP);
	ask_finish(d, box, footer);
	g_signal_connect_data(d, "response", G_CALLBACK(conflict_response), a,
			      ask_free, 0);
	gtk_widget_show_all(d);
	gtk_widget_grab_focus(def);
	g_free(from);
	g_free(there);
	g_free(head);
	g_free(name);
	g_free(sn);
	g_free(dn);
	g_clear_object(&src_dir);
	g_clear_object(&dst_dir);
}

/* ---- failures --------------------------------------------------------------- */

static void issue_response(GtkDialog *d, int r, Ask *a)
{
	gboolean all = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->all));
	ExIssueChoice c;

	switch (r) {
	case R_RETRY:
		c = EX_ISSUE_RETRY;
		break;
	case R_SKIP:
		c = EX_ISSUE_SKIP;
		break;
	case R_DELETE:
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

/* The window's title and the message for what went wrong. */
static void issue_words(const ExIssue *i, const char **title,
			const char **instruction)
{
	*title = "Operation Stopped";
	*instruction = "The operation cannot go on with this item";
	if (i->can_delete_instead) {
		*title = "Delete Permanently";
		*instruction = "The Trash cannot take this item here. Delete it "
			       "permanently instead?";
		return;
	}
	if (!i->error || i->error->domain != G_IO_ERROR)
		return;
	switch (i->error->code) {
	case G_IO_ERROR_PERMISSION_DENIED:
		*title = "No Permission";
		*instruction = "You do not have permission to do this here";
		break;
	case G_IO_ERROR_BUSY:
		*title = "Item Busy";
		*instruction = "Another program is using this item";
		break;
	case G_IO_ERROR_NO_SPACE:
		*title = "Destination Full";
		*instruction = "The destination has no room left for this item";
		break;
	case G_IO_ERROR_NOT_FOUND:
		*title = "Item Gone";
		*instruction = "This item is no longer there";
		break;
	case G_IO_ERROR_READ_ONLY:
		*title = "Read-Only Disk";
		*instruction = "The disk can only be read, not written";
		break;
	case G_IO_ERROR_FILENAME_TOO_LONG:
		*title = "Name Too Long";
		*instruction = "The name is too long for the destination";
		break;
	default:
		break;
	}
}

void ex_issue_dialog_show(GtkWindow *parent, ExOperation *op, const ExIssue *i)
{
	const char *title, *instruction;
	GtkWidget *d, *box, *body, *footer, *def = NULL;
	Ask *a = g_new0(Ask, 1);

	a->op = g_object_ref(op);
	a->is_issue = TRUE;
	issue_words(i, &title, &instruction);
	d = ask_dialog(parent, title);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	body = i->can_delete_instead ?
		       body_new(d, op, "likeos-delete-permanently-symbolic",
				"edit-delete-symbolic", instruction, TRUE) :
		       body_new(d, op, "dialog-warning-symbolic",
				"dialog-warning-symbolic", instruction, TRUE);
	gtk_box_pack_start(GTK_BOX(box), body, FALSE, FALSE, 0);
	/* The item, and the sentence saying what happened to it. */
	gtk_box_pack_start(GTK_BOX(body_column(body)),
			   item_row(i->file, FALSE, TRUE, i->message), FALSE,
			   FALSE, 0);

	a->all = gtk_check_button_new_with_mnemonic("Apply to _all items");
	footer = footer_new(a->all);
	/* The full error, which the message only summarises. */
	if (i->error && i->error->message)
		more_toggle(box, footer, i->error->message);
	if (i->can_delete_instead) {
		def = push(d, footer, "_Delete", R_DELETE);
		add_class(def, "destructive-action");
		push(d, footer, "_Skip", R_SKIP);
	} else {
		if (i->can_retry)
			def = push(d, footer, "_Try again", R_RETRY);
		GtkWidget *skip = push(d, footer, "_Skip", R_SKIP);

		if (!def)
			def = skip;
		add_class(def, "suggested-action");
	}
	push(d, footer, "_Cancel", R_CANCEL);
	ask_finish(d, box, footer);
	g_signal_connect_data(d, "response", G_CALLBACK(issue_response), a,
			      ask_free, 0);
	gtk_widget_show_all(d);
	gtk_widget_grab_focus(def);
}
