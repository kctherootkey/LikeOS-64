// LikeOS Explorer: the Properties window.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-properties.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "ui/widgets/lx-segmented.h"
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
	GtkWidget *stack;
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

/* A labelled row of a card: the name of the fact in the secondary colour,
 * the fact beside it. */
static void row(GtkWidget *grid, int *r, const char *label, GtkWidget *value)
{
	GtkWidget *l = gtk_label_new(label);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_yalign(GTK_LABEL(l), 0);
	ex_style_add(l, EX_CLASS_DIM);
	ex_style_add(l, "ex-props-key");
	gtk_widget_set_hexpand(value, TRUE);
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

/* A heading of the access grid: text to read, not to copy. */
static GtkWidget *heading(const char *text)
{
	GtkWidget *h = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(h), 0);
	ex_style_add(h, EX_CLASS_DIM);
	ex_style_add(h, "ex-props-key");
	return h;
}

/* A white card holding a grid of rows; the grid is returned, the card
 * packed into `box'. */
static GtkWidget *card(GtkWidget *box)
{
	GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *grid = gtk_grid_new();

	gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
	ex_style_add(c, "ex-props-card");
	gtk_box_pack_start(GTK_BOX(c), grid, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
	return grid;
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
	char *s = g_strdup_printf("%s  ·  %" G_GUINT64_FORMAT " bytes", sz,
				  c->bytes);
	char *n = g_strdup_printf("%u file%s and %u folder%s", c->files,
				  c->files == 1 ? "" : "s", c->folders,
				  c->folders == 1 ? "" : "s");

	gtk_label_set_text(GTK_LABEL(p->size_label), s);
	if (p->contains_label)
		gtk_label_set_text(GTK_LABEL(p->contains_label), n);
	g_free(n);
	g_free(s);
	g_free(sz);
	g_free(c);
}

/* ---- the opener -------------------------------------------------------------- */

/* "Opens with": the app's picture and name, with "Choose…" beside them. */
static void opener_text(Props *p, ExFile *f)
{
	GAppInfo *a = ex_apps_default_for(ex_file_get_content_type(f));
	GtkWidget *img = g_object_get_data(G_OBJECT(p->opens_with), "ex-img");
	GtkWidget *name = g_object_get_data(G_OBJECT(p->opens_with), "ex-name");
	GIcon *icon = a ? g_app_info_get_icon(a) : NULL;
	int scale = gtk_widget_get_scale_factor(p->opens_with);
	GdkPixbuf *pb = icon ? ex_icons_gicon(icon, 16, scale) : NULL;
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;

	gtk_image_set_from_surface(GTK_IMAGE(img), s);
	gtk_label_set_text(GTK_LABEL(name),
			   a ? g_app_info_get_display_name(a) : "No app chosen");
	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
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
					     "The app for this type could not be set",
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
	char *head = g_strdup_printf("Which app opens every %s?", desc);

	gtk_app_chooser_dialog_set_heading(GTK_APP_CHOOSER_DIALOG(d), head);
	g_free(head);
	g_free(desc);
	g_signal_connect(d, "response", G_CALLBACK(chooser_done), p);
	gtk_widget_show(d);
}

/* ---- pages ---------------------------------------------------------------------- */

/* The header: the picture large, the name in a field to edit (or how many
 * items there are), and under it the folder it is in. */
static GtkWidget *header(Props *p)
{
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	ExFile *f = p->files->len == 1 ? g_ptr_array_index(p->files, 0) : NULL;
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(p->win));
	/* The type's picture (not a thumbnail): the window is about the
	 * item, whatever is in it. */
	GdkPixbuf *pb = f ? ex_icons_for_file(f, 48, scale, FALSE, FALSE) :
			    ex_icons_named("edit-select-all", 48, scale);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);
	GtkWidget *kind;

	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	ex_style_add(h, "ex-props-header");
	gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);
	if (f) {
		GFile *parent = g_file_get_parent(ex_file_get_file(f));
		ExLocation *pl = parent ? ex_location_new_folder(parent) : NULL;
		char *pn = pl ? ex_location_display_name(pl) : NULL;
		char *desc = pn ? g_strdup_printf("In %s", pn) : g_strdup("");

		g_free(pn);
		if (pl)
			ex_location_unref(pl);
		g_clear_object(&parent);
		p->name_entry = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(p->name_entry), ex_file_get_edit_name(f));
		gtk_widget_set_sensitive(p->name_entry,
					 (ex_file_get_caps(f) & EX_CAP_RENAME) != 0);
		atk_object_set_name(gtk_widget_get_accessible(p->name_entry), "Name");
		gtk_widget_set_hexpand(p->name_entry, TRUE);
		ex_style_add(p->name_entry, "ex-props-name");
		gtk_box_pack_start(GTK_BOX(col), p->name_entry, FALSE, FALSE, 0);
		kind = gtk_label_new(desc);
		g_free(desc);
	} else {
		char *n = g_strdup_printf("%u items", p->files->len);
		GtkWidget *l = gtk_label_new(n);

		gtk_label_set_xalign(GTK_LABEL(l), 0);
		ex_style_add(l, "ex-props-title");
		gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
		kind = gtk_label_new("Several items");
		g_free(n);
	}
	gtk_label_set_xalign(GTK_LABEL(kind), 0);
	ex_style_add(kind, EX_CLASS_DIM);
	gtk_box_pack_start(GTK_BOX(col), kind, FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(h), col, TRUE, TRUE, 0);
	return h;
}

static GtkWidget *general_page(Props *p)
{
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *grid;
	int r = 0;
	ExFile *f = p->files->len == 1 ? g_ptr_array_index(p->files, 0) : NULL;

	ex_style_add(page, "ex-props-page");
	grid = card(page);
	if (f) {
		char *kind = ex_icons_kind_label(f);
		const char *ext = ex_name_extension(ex_file_get_name(f));
		/* "PNG file  ·  .png": the type in words, then its
		 * extension. */
		char *desc = !ex_file_is_dir(f) && *ext ?
				     g_strdup_printf("%s  ·  %s", kind, ext) :
				     g_strdup(kind);

		row(grid, &r, "Type", value(desc));
		g_free(desc);
		g_free(kind);
		if (!ex_file_is_dir(f)) {
			GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
			GtkWidget *img = gtk_image_new();
			GtkWidget *name = gtk_label_new("");
			GtkWidget *choose = gtk_button_new_with_mnemonic("C_hoose…");

			gtk_label_set_xalign(GTK_LABEL(name), 0);
			gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
			gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(box), name, TRUE, TRUE, 0);
			gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
			gtk_widget_set_tooltip_text(choose,
						    "Choose the app that opens every "
						    "file of this type");
			ex_style_add(choose, "ex-props-choose");
			gtk_box_pack_end(GTK_BOX(box), choose, FALSE, FALSE, 0);
			g_signal_connect(choose, "clicked",
					 G_CALLBACK(change_opener), p);
			p->opens_with = box;
			g_object_set_data(G_OBJECT(box), "ex-img", img);
			g_object_set_data(G_OBJECT(box), "ex-name", name);
			opener_text(p, f);
			row(grid, &r, "Opens with", p->opens_with);
		}
		GFile *parent = g_file_get_parent(ex_file_get_file(f));
		char *where = parent ? g_file_get_parse_name(parent) : g_strdup("");
		row(grid, &r, "Location", value(where));
		g_free(where);
		g_clear_object(&parent);
		if (ex_file_is_symlink(f))
			row(grid, &r, "Points to",
			    value(g_file_info_get_symlink_target(ex_file_get_info(f))));
	}
	p->size_label = value("Counting…");
	row(grid, &r, "Size", p->size_label);
	if (!f || ex_file_is_dir(f)) {
		p->contains_label = value("");
		row(grid, &r, "Contains", p->contains_label);
	}
	if (f) {
		GFileInfo *i = ex_file_get_info(f);
		struct {
			const char *label, *attr;
		} times[] = {
			{ "Created", G_FILE_ATTRIBUTE_TIME_CREATED },
			{ "Modified", G_FILE_ATTRIBUTE_TIME_MODIFIED },
			{ "Last opened", G_FILE_ATTRIBUTE_TIME_ACCESS },
		};
		GtkWidget *dates = NULL;
		int dr = 0;

		for (gsize k = 0; k < G_N_ELEMENTS(times); k++) {
			if (!g_file_info_has_attribute(i, times[k].attr))
				continue;
			char *t = ex_format_time((gint64)g_file_info_get_attribute_uint64(
				i, times[k].attr));
			if (!dates)
				dates = card(page);
			row(dates, &dr, times[k].label, value(t));
			g_free(t);
		}
	}

	GTask *t = g_task_new(NULL, p->cancel, counted, p);
	g_task_set_task_data(t, g_ptr_array_ref(p->files),
			     (GDestroyNotify)g_ptr_array_unref);
	g_task_run_in_thread(t, count_thread);
	g_object_unref(t);
	return page;
}

/* Access: whose the item is, and a grid of who may do what -- in the words
 * that fit a folder or a file. */
static GtkWidget *permissions_page(Props *p)
{
	static const char *who[] = { "Owner", "Group", "Everyone else" };
	static const char *file_what[] = { "Read", "Change", "Run" };
	static const char *dir_what[] = { "See inside", "Add & remove", "Open" };
	ExFile *f = g_ptr_array_index(p->files, 0);
	const char *const *what = ex_file_is_dir(f) ? dir_what : file_what;
	GFileInfo *i = ex_file_get_info(f);
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *grid;
	gboolean mine;
	int r = 0;

	ex_style_add(page, "ex-props-page");
	p->mode_known = g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_UNIX_MODE);
	p->mode = p->mode_known ?
			  g_file_info_get_attribute_uint32(i, G_FILE_ATTRIBUTE_UNIX_MODE) :
			  0;
	mine = g_file_info_get_attribute_uint32(i, G_FILE_ATTRIBUTE_UNIX_UID) ==
		       (guint32)geteuid() ||
	       geteuid() == 0;

	grid = card(page);
	row(grid, &r, "Owner",
	    value(g_file_info_get_attribute_string(i, G_FILE_ATTRIBUTE_OWNER_USER)));
	row(grid, &r, "Group",
	    value(g_file_info_get_attribute_string(i, G_FILE_ATTRIBUTE_OWNER_GROUP)));

	grid = card(page);
	r = 0;
	gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
	for (int c = 0; c < 3; c++)
		gtk_grid_attach(GTK_GRID(grid), heading(what[c]), c + 1, r, 1, 1);
	r++;
	for (int w = 0; w < 3; w++) {
		GtkWidget *l = gtk_label_new(who[w]);

		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
		for (int c = 0; c < 3; c++) {
			GtkWidget *cb = gtk_check_button_new();
			guint32 bit = 1u << (8 - (w * 3 + c));
			char *name = g_strdup_printf("%s: %s", who[w], what[c]);

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
		GtkWidget *note = value("Only the owner can change who may do "
					"what.");

		ex_style_add(note, EX_CLASS_DIM);
		gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 0);
	}
	return page;
}

static void page_changed(LxSegmented *seg, int idx, Props *p)
{
	gtk_stack_set_visible_child_name(GTK_STACK(p->stack),
					 idx == 0 ? "general" : "access");
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
	char *title = f ? g_strdup_printf("Properties of “%s”", ex_file_get_name(f)) :
			  g_strdup_printf("Properties of %u items", files->len);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

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
	gtk_box_set_spacing(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->dialog))),
			    0);
	gtk_box_pack_start(GTK_BOX(box), header(p), FALSE, FALSE, 0);

	/* General | Access, when there is an Access page. */
	p->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(p->stack), general_page(p), "general");
	if (f && g_file_is_native(ex_file_get_file(f))) {
		GtkWidget *seg = lx_segmented_new();

		lx_segmented_append_label(LX_SEGMENTED(seg), "General");
		lx_segmented_append_label(LX_SEGMENTED(seg), "Access");
		lx_segmented_set_active(LX_SEGMENTED(seg), 0);
		g_signal_connect(seg, "changed", G_CALLBACK(page_changed), p);
		gtk_widget_set_halign(seg, GTK_ALIGN_START);
		ex_style_add(seg, "ex-props-switch");
		gtk_box_pack_start(GTK_BOX(box), seg, FALSE, FALSE, 0);
		gtk_stack_add_named(GTK_STACK(p->stack), permissions_page(p),
				    "access");
	}
	gtk_box_pack_start(GTK_BOX(box), p->stack, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(
				  GTK_DIALOG(p->dialog))),
			  box);
	g_signal_connect(p->dialog, "response", G_CALLBACK(response), p);
	g_object_set_data_full(G_OBJECT(p->dialog), "ex-props", p,
			       (GDestroyNotify)props_free);
	ex_style_add(p->dialog, "ex-properties");
	ex_style_fixed_dialog(p->dialog);
	gtk_widget_show_all(p->dialog);
	gtk_stack_set_visible_child_name(GTK_STACK(p->stack), "general");
	/* The name is what is edited here most: it starts with the focus,
	 * selected. */
	if (p->name_entry && gtk_widget_get_sensitive(p->name_entry))
		gtk_widget_grab_focus(p->name_entry);
}
