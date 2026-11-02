// LikeOS -- File Explorer: the Properties window.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-properties.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-apps.h"
#include "backend/ex-util.h"

#include <string.h>
#include <unistd.h>

typedef struct {
	ExWindow *win;
	GtkWidget *dialog;
	GPtrArray *files; /* ExFile */
	GtkWidget *name_entry;
	GtkWidget *size_label, *contains_label;
	GtkWidget *perm[9];
	GtkWidget *opens_with;
	GCancellable *cancel;
	guint32 mode;
	gboolean mode_known;
	/* What Apply already did: the item's name and place as they are now,
	 * so OK after Apply does not do it all again (on a name that no longer
	 * exists). */
	char *name;
	GFile *file;
} Props;

static void props_free(Props *p)
{
	g_free(p->name);
	g_clear_object(&p->file);
	g_cancellable_cancel(p->cancel);
	g_object_unref(p->cancel);
	g_ptr_array_unref(p->files);
	g_object_unref(p->win);
	g_free(p);
}

static void row(GtkWidget *grid, int *r, const char *label, GtkWidget *value)
{
	GtkWidget *l = gtk_label_new(label);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_yalign(GTK_LABEL(l), 0);
	ex_style_add(l, EX_CLASS_DIM);
	gtk_grid_attach(GTK_GRID(grid), l, 0, *r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), value, 1, *r, 1, 1);
	(*r)++;
}

static GtkWidget *value(const char *text)
{
	GtkWidget *v = gtk_label_new(text ? text : "");

	gtk_label_set_xalign(GTK_LABEL(v), 0);
	/* Text to select with the mouse and copy, not a stop of the keyboard:
	 * a click on a tab carries the focus into the page, and a focusable
	 * label taking it shows all of itself selected. */
	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_widget_set_can_focus(v, FALSE);
	gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 48);
	return v;
}

/* A heading of the permission grid: text to read, not to copy. */
static GtkWidget *heading(const char *text)
{
	GtkWidget *h = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(h), 0);
	return h;
}

static void separator(GtkWidget *grid, int *r)
{
	gtk_grid_attach(GTK_GRID(grid),
			gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, *r, 2, 1);
	(*r)++;
}

/* ---- size of folders, counted in the background -------------------------- */

typedef struct {
	guint64 bytes;
	guint files, folders;
} Count;

static void count(GFile *f, Count *c, GCancellable *cancel, int depth)
{
	GFileInfo *i = g_file_query_info(
		f, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, NULL);

	if (!i)
		return;
	if (g_file_info_get_file_type(i) == G_FILE_TYPE_DIRECTORY && depth < 128) {
		GFileEnumerator *en;
		GFileInfo *ci;

		if (depth > 0)
			c->folders++;
		en = g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME,
					       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					       cancel, NULL);
		while (en && (ci = g_file_enumerator_next_file(en, cancel, NULL))) {
			GFile *child = g_file_enumerator_get_child(en, ci);

			count(child, c, cancel, depth + 1);
			g_object_unref(child);
			g_object_unref(ci);
		}
		g_clear_object(&en);
	} else {
		c->files++;
		c->bytes += (guint64)g_file_info_get_size(i);
	}
	g_object_unref(i);
}

static void count_thread(GTask *task, gpointer src, gpointer data,
			 GCancellable *cancel)
{
	GPtrArray *files = data;
	Count *c = g_new0(Count, 1);

	for (guint i = 0; i < files->len && !g_cancellable_is_cancelled(cancel); i++)
		count(ex_file_get_file(g_ptr_array_index(files, i)), c, cancel,
		      files->len == 1 ? 0 : 1);
	g_task_return_pointer(task, c, g_free);
}

static void counted(GObject *src, GAsyncResult *res, gpointer data)
{
	Props *p = data;
	Count *c = g_task_propagate_pointer(G_TASK(res), NULL);

	if (!c)
		return; /* the window closed */
	char *sz = ex_format_size(c->bytes);
	char *s = g_strdup_printf("%s (%" G_GUINT64_FORMAT " bytes)", sz, c->bytes);
	char *n = g_strdup_printf("%u files, %u folders", c->files, c->folders);

	gtk_label_set_text(GTK_LABEL(p->size_label), s);
	if (p->contains_label)
		gtk_label_set_text(GTK_LABEL(p->contains_label), n);
	g_free(n);
	g_free(s);
	g_free(sz);
	g_free(c);
}

/* ---- the opener -------------------------------------------------------------- */

static void opener_text(Props *p, ExFile *f)
{
	GAppInfo *a = ex_apps_default_for(ex_file_get_content_type(f));

	gtk_button_set_label(GTK_BUTTON(p->opens_with),
			     a ? g_app_info_get_display_name(a) : "Choose an app…");
	g_clear_object(&a);
}

static void chooser_done(GtkDialog *d, int r, Props *p)
{
	ExFile *f = g_ptr_array_index(p->files, 0);

	if (r == GTK_RESPONSE_OK) {
		GAppInfo *a = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(d));
		GError *e = NULL;

		if (a && !ex_apps_set_default(a, ex_file_get_content_type(f), &e)) {
			ex_window_show_error(p->win,
					     "The default app could not be changed",
					     e->message);
			g_error_free(e);
		}
		g_clear_object(&a);
		opener_text(p, f);
	}
	gtk_widget_destroy(GTK_WIDGET(d));
}

static void change_opener(GtkButton *b, Props *p)
{
	ExFile *f = g_ptr_array_index(p->files, 0);
	GtkWidget *d = gtk_app_chooser_dialog_new_for_content_type(
		GTK_WINDOW(p->dialog), GTK_DIALOG_MODAL,
		ex_file_get_content_type(f));
	char *desc = ex_file_get_type_description(f);
	char *head = g_strdup_printf("Always open %s files with:", desc);

	gtk_app_chooser_dialog_set_heading(GTK_APP_CHOOSER_DIALOG(d), head);
	g_free(head);
	g_free(desc);
	g_signal_connect(d, "response", G_CALLBACK(chooser_done), p);
	gtk_widget_show(d);
}

/* ---- pages ---------------------------------------------------------------------- */

static GtkWidget *general_page(Props *p)
{
	GtkWidget *grid = gtk_grid_new();
	int r = 0;
	ExFile *f = p->files->len == 1 ? g_ptr_array_index(p->files, 0) : NULL;
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(p->win));

	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);

	GdkPixbuf *pb = f ? ex_icons_for_file(f, 48, scale, FALSE, TRUE) :
			    ex_icons_named("edit-select-all", 48, scale);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);
	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	gtk_grid_attach(GTK_GRID(grid), img, 0, r, 1, 1);
	if (f) {
		p->name_entry = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(p->name_entry), ex_file_get_edit_name(f));
		gtk_widget_set_sensitive(p->name_entry,
					 (ex_file_get_caps(f) & EX_CAP_RENAME) != 0);
		atk_object_set_name(gtk_widget_get_accessible(p->name_entry), "Name");
		gtk_widget_set_hexpand(p->name_entry, TRUE);
		gtk_grid_attach(GTK_GRID(grid), p->name_entry, 1, r, 1, 1);
	} else {
		char *n = g_strdup_printf("%u items", p->files->len);

		gtk_grid_attach(GTK_GRID(grid), value(n), 1, r, 1, 1);
		g_free(n);
	}
	r++;
	separator(grid, &r);

	if (f) {
		char *desc = ex_file_get_type_description(f);

		row(grid, &r, "Type", value(desc));
		g_free(desc);
		if (!ex_file_is_dir(f)) {
			p->opens_with = gtk_button_new_with_label("");
			gtk_widget_set_halign(p->opens_with, GTK_ALIGN_START);
			gtk_widget_set_tooltip_text(p->opens_with,
						    "Change which app opens files of "
						    "this type");
			g_signal_connect(p->opens_with, "clicked",
					 G_CALLBACK(change_opener), p);
			opener_text(p, f);
			row(grid, &r, "Opens with", p->opens_with);
		}
		GFile *parent = g_file_get_parent(ex_file_get_file(f));
		char *where = parent ? g_file_get_parse_name(parent) : g_strdup("");
		row(grid, &r, "Location", value(where));
		g_free(where);
		g_clear_object(&parent);
		if (ex_file_is_symlink(f))
			row(grid, &r, "Link to",
			    value(g_file_info_get_symlink_target(ex_file_get_info(f))));
	}
	p->size_label = value("Calculating…");
	row(grid, &r, "Size", p->size_label);
	if (!f || ex_file_is_dir(f)) {
		p->contains_label = value("");
		row(grid, &r, "Contains", p->contains_label);
	}
	separator(grid, &r);
	if (f) {
		GFileInfo *i = ex_file_get_info(f);
		struct {
			const char *label, *attr;
		} times[] = {
			{ "Created", G_FILE_ATTRIBUTE_TIME_CREATED },
			{ "Modified", G_FILE_ATTRIBUTE_TIME_MODIFIED },
			{ "Accessed", G_FILE_ATTRIBUTE_TIME_ACCESS },
		};

		for (gsize k = 0; k < G_N_ELEMENTS(times); k++) {
			if (!g_file_info_has_attribute(i, times[k].attr))
				continue;
			char *t = ex_format_time((gint64)g_file_info_get_attribute_uint64(
				i, times[k].attr));
			row(grid, &r, times[k].label, value(t));
			g_free(t);
		}
	}

	GTask *t = g_task_new(NULL, p->cancel, counted, p);
	g_task_set_task_data(t, g_ptr_array_ref(p->files),
			     (GDestroyNotify)g_ptr_array_unref);
	g_task_run_in_thread(t, count_thread);
	g_object_unref(t);
	return grid;
}

static GtkWidget *permissions_page(Props *p)
{
	static const char *who[] = { "Owner", "Group", "Others" };
	static const char *what[] = { "Read", "Write", "Execute" };
	ExFile *f = g_ptr_array_index(p->files, 0);
	GFileInfo *i = ex_file_get_info(f);
	GtkWidget *grid = gtk_grid_new();
	gboolean mine;
	int r = 0;

	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	p->mode_known = g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_UNIX_MODE);
	p->mode = p->mode_known ?
			  g_file_info_get_attribute_uint32(i, G_FILE_ATTRIBUTE_UNIX_MODE) :
			  0;
	mine = g_file_info_get_attribute_uint32(i, G_FILE_ATTRIBUTE_UNIX_UID) ==
		       (guint32)geteuid() ||
	       geteuid() == 0;

	gtk_grid_attach(GTK_GRID(grid), heading("Owner"), 0, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid),
			value(g_file_info_get_attribute_string(
				i, G_FILE_ATTRIBUTE_OWNER_USER)),
			1, r++, 3, 1);
	gtk_grid_attach(GTK_GRID(grid), heading("Group"), 0, r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid),
			value(g_file_info_get_attribute_string(
				i, G_FILE_ATTRIBUTE_OWNER_GROUP)),
			1, r++, 3, 1);
	gtk_grid_attach(GTK_GRID(grid),
			gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, r++, 4, 1);
	for (int c = 0; c < 3; c++)
		gtk_grid_attach(GTK_GRID(grid), heading(what[c]), c + 1, r, 1, 1);
	r++;
	for (int w = 0; w < 3; w++) {
		gtk_grid_attach(GTK_GRID(grid), heading(who[w]), 0, r, 1, 1);
		for (int c = 0; c < 3; c++) {
			GtkWidget *cb = gtk_check_button_new();
			guint32 bit = 1u << (8 - (w * 3 + c));
			char *name = g_strdup_printf("%s may %s", who[w], what[c]);

			/* The box alone, under its heading: its keyboard ring
			 * then goes round the box, not the whole column. */
			gtk_widget_set_halign(cb, GTK_ALIGN_START);
			gtk_widget_set_valign(cb, GTK_ALIGN_CENTER);
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb),
						     (p->mode & bit) != 0);
			gtk_widget_set_sensitive(cb, mine && p->mode_known);
			atk_object_set_name(gtk_widget_get_accessible(cb), name);
			g_free(name);
			p->perm[w * 3 + c] = cb;
			gtk_grid_attach(GTK_GRID(grid), cb, c + 1, r, 1, 1);
		}
		r++;
	}
	if (!mine) {
		gtk_grid_attach(GTK_GRID(grid),
				value("Only the owner can change the permissions."),
				0, r++, 4, 1);
	}
	return grid;
}

/* ---- applying -------------------------------------------------------------------- */

static void apply(Props *p)
{
	if (!p->file)
		return;
	if (p->name_entry) {
		const char *n = gtk_entry_get_text(GTK_ENTRY(p->name_entry));

		if (*n && strcmp(n, p->name) != 0) {
			ExOperation *op = ex_operation_new_rename(p->file, n);
			GFile *parent = g_file_get_parent(p->file);

			ex_window_run(p->win, op, TRUE, FALSE);
			g_object_unref(op);
			/* From now on the item is the renamed one: a later
			 * Apply or OK, and the permissions below, act on it. */
			g_free(p->name);
			p->name = g_strdup(n);
			if (parent) {
				g_object_unref(p->file);
				p->file = g_file_get_child_for_display_name(parent, n,
									    NULL);
				g_object_unref(parent);
			}
		}
	}
	if (p->mode_known && p->perm[0] && p->file) {
		guint32 m = p->mode & ~0777u;

		for (int k = 0; k < 9; k++)
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->perm[k])))
				m |= 1u << (8 - k);
		if (m != p->mode) {
			ExOperation *op = ex_operation_new_chmod(p->file, m);

			ex_window_run(p->win, op, FALSE, FALSE);
			g_object_unref(op);
			p->mode = m;
		}
	}
}

static void response(GtkDialog *d, int r, Props *p)
{
	if (r == GTK_RESPONSE_OK || r == GTK_RESPONSE_APPLY)
		apply(p);
	if (r != GTK_RESPONSE_APPLY)
		gtk_widget_destroy(GTK_WIDGET(d));
}

void ex_properties_show(ExWindow *w, GPtrArray *files)
{
	Props *p = g_new0(Props, 1);
	ExFile *f = files->len == 1 ? g_ptr_array_index(files, 0) : NULL;
	char *title = f ? g_strdup_printf("%s Properties", ex_file_get_name(f)) :
			  g_strdup_printf("%u items Properties", files->len);
	GtkWidget *nb = gtk_notebook_new();

	p->win = g_object_ref(w);
	p->cancel = g_cancellable_new();
	if (f) {
		p->name = g_strdup(ex_file_get_edit_name(f));
		p->file = g_object_ref(ex_file_get_file(f));
	}
	p->files = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint i = 0; i < files->len; i++)
		g_ptr_array_add(p->files, g_object_ref(g_ptr_array_index(files, i)));

	p->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w),
						GTK_DIALOG_DESTROY_WITH_PARENT,
						"_Cancel", GTK_RESPONSE_CANCEL,
						"_Apply", GTK_RESPONSE_APPLY, "_OK",
						GTK_RESPONSE_OK, NULL);
	g_free(title);
	gtk_dialog_set_default_response(GTK_DIALOG(p->dialog), GTK_RESPONSE_OK);
	gtk_notebook_append_page(GTK_NOTEBOOK(nb), general_page(p),
				 gtk_label_new("General"));
	if (f && g_file_is_native(ex_file_get_file(f)))
		gtk_notebook_append_page(GTK_NOTEBOOK(nb), permissions_page(p),
					 gtk_label_new("Permissions"));
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(
				  GTK_DIALOG(p->dialog))),
			  nb);
	g_signal_connect(p->dialog, "response", G_CALLBACK(response), p);
	g_object_set_data_full(G_OBJECT(p->dialog), "ex-props", p,
			       (GDestroyNotify)props_free);
	ex_style_fixed_dialog(p->dialog);
	gtk_widget_show_all(p->dialog);
	/* The name is what is edited here most: it starts with the focus,
	 * selected, as in Windows. */
	if (p->name_entry && gtk_widget_get_sensitive(p->name_entry))
		gtk_widget_grab_focus(p->name_entry);
}
