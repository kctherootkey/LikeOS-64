// LikeOS Explorer: Home.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-home-page.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-bookmarks.h"
#include "backend/ex-recent.h"
#include "backend/ex-util.h"
#include "services/ex-settings.h"

#include <string.h>

#define EX_RECENT_SHOWN 20
#define EX_FREQUENT_SHOWN 4
/* The file lists' columns, in pixels: Last opened; the name takes the
 * rest. */
#define EX_COL_WHEN 200
/* A file's picture in the lists, and a folder's on a card. */
#define EX_LIST_ICON 32
#define EX_TILE_ICON 48
/* A folder card's width. */
#define EX_CARD_WIDTH 120

struct _ExHomePage {
	GtkScrolledWindow parent;
	GtkWidget *box;
	GCancellable *cancel;
	GFileMonitor *recent_mon;
	guint reload_id;
	char *filter; /* the kind Recent and Favorites show, NULL for all */
};

G_DEFINE_TYPE(ExHomePage, ex_home_page, GTK_TYPE_SCROLLED_WINDOW)

enum { SIG_OPEN, SIG_OPEN_FILE, SIG_POPUP, SIG_POPUP_LOCATION, N_SIGS };
static guint sigs[N_SIGS];

/* What the person chose here, for the life of the program: a reload (the
 * recent list changed, a folder was pinned) keeps it. */
static gboolean g_pinned_folded, g_files_folded, g_show_favorites;

static void destroy_child(GtkWidget *w, gpointer data)
{
	gtk_widget_destroy(w);
}

/* An icon of the LikeOS theme, with a standard one to stand in where the
 * theme has none. */
static GtkWidget *themed_image(const char *name, const char *fallback,
			       int size)
{
	const char *names[] = { name, fallback, NULL };
	GIcon *gi = g_themed_icon_new_from_names((char **)names, -1);
	GtkWidget *img = gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_BUTTON);

	gtk_image_set_pixel_size(GTK_IMAGE(img), size);
	g_object_unref(gi);
	return img;
}

static GtkWidget *pixbuf_image(GdkPixbuf *pb, int scale)
{
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);

	if (s)
		cairo_surface_destroy(s);
	return img;
}

static GtkWidget *dim_label(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	ex_style_add(l, EX_CLASS_DIM);
	return l;
}

static GtkWidget *placeholder(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	ex_style_add(l, EX_CLASS_PLACEHOLDER);
	return l;
}

/* Where a file or folder is: below the home folder relative to it
 * ("Pictures/Screenshots"), the home folder itself "Home", elsewhere the
 * whole path. */
static char *where_text(GFile *f)
{
	GFile *home = ex_home_dir();
	char *rel = g_file_get_relative_path(home, f);
	char *out;

	if (rel)
		out = rel;
	else if (g_file_equal(home, f))
		out = g_strdup("Home");
	else
		out = g_file_get_parse_name(f);
	g_object_unref(home);
	return out;
}

/* ---- folding headings ----------------------------------------------------- */

static GtkWidget *chevron(gboolean folded)
{
	return folded ? themed_image("likeos-chevron-right", "pan-end-symbolic",
				     12) :
			themed_image("likeos-chevron-down", "pan-down-symbolic",
				     12);
}

static void fold_toggled(GtkButton *b, GtkWidget *revealer)
{
	gboolean open = !gtk_revealer_get_reveal_child(GTK_REVEALER(revealer));
	GtkWidget *row = g_object_get_data(G_OBJECT(b), "ex-row");
	GtkWidget *chev = g_object_get_data(G_OBJECT(b), "ex-chevron");
	gboolean *folded = g_object_get_data(G_OBJECT(b), "ex-folded");

	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), open);
	gtk_widget_destroy(chev);
	chev = chevron(!open);
	ex_style_add(chev, "ex-fold-chevron");
	gtk_box_pack_end(GTK_BOX(row), chev, FALSE, FALSE, 0);
	gtk_widget_show(chev);
	g_object_set_data(G_OBJECT(b), "ex-chevron", chev);
	*folded = !open;
	atk_object_set_description(gtk_widget_get_accessible(GTK_WIDGET(b)),
				   open ? "Expanded" : "Collapsed");
}

/* A heading button: `title' when there is one, and after it the chevron
 * that tells whether the group is open.  It folds `revealer'. */
static GtkWidget *fold_button(const char *title, GtkWidget *revealer,
			      gboolean *folded)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *chev = chevron(*folded);

	if (title)
		gtk_box_pack_start(GTK_BOX(row), gtk_label_new(title), FALSE,
				   FALSE, 0);
	ex_style_add(chev, "ex-fold-chevron");
	gtk_box_pack_end(GTK_BOX(row), chev, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_halign(b, GTK_ALIGN_START);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	ex_style_add(b, "ex-fold");
	g_object_set_data(G_OBJECT(b), "ex-row", row);
	g_object_set_data(G_OBJECT(b), "ex-chevron", chev);
	g_object_set_data(G_OBJECT(b), "ex-folded", folded);
	atk_object_set_name(gtk_widget_get_accessible(b),
			    title ? title : "Recent files and Favorites");
	atk_object_set_description(gtk_widget_get_accessible(b),
				   *folded ? "Collapsed" : "Expanded");
	gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
					 GTK_REVEALER_TRANSITION_TYPE_NONE);
	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), !*folded);
	g_signal_connect(b, "clicked", G_CALLBACK(fold_toggled), revealer);
	return b;
}

/* ---- Pinned folder cards --------------------------------------------------- */

static void tile_clicked(GtkButton *b, ExHomePage *p)
{
	g_signal_emit(p, sigs[SIG_OPEN], 0,
		      g_object_get_data(G_OBJECT(b), "ex-location"), FALSE);
}

static gboolean tile_button(GtkWidget *w, GdkEventButton *ev, ExHomePage *p)
{
	ExLocation *l = g_object_get_data(G_OBJECT(w), "ex-location");

	if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
		g_signal_emit(p, sigs[SIG_OPEN], 0, l, TRUE);
		return TRUE;
	}
	if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
		gtk_widget_grab_focus(w);
		g_signal_emit(p, sigs[SIG_POPUP_LOCATION], 0, l, ev);
		return TRUE;
	}
	return FALSE;
}

/* The menu key (or Shift+F10) on a focused card. */
static gboolean tile_menu_key(GtkWidget *w, ExHomePage *p)
{
	g_signal_emit(p, sigs[SIG_POPUP_LOCATION], 0,
		      g_object_get_data(G_OBJECT(w), "ex-location"), NULL);
	return TRUE;
}

static const GUserDirectory personal[] = {
	G_USER_DIRECTORY_DESKTOP,  G_USER_DIRECTORY_DOWNLOAD,
	G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_PICTURES,
	G_USER_DIRECTORY_MUSIC,	   G_USER_DIRECTORY_VIDEOS,
};

/* A folder card: its picture large over its name, under the name where it
 * is, and for a pinned folder a small pin in the card's corner. */
static GtkWidget *folder_tile(ExHomePage *p, GFile *f, const char *label,
			      gboolean pinned)
{
	ExLocation *loc = ex_location_new_folder(f);
	char *name = label ? g_strdup(label) : ex_location_display_name(loc);
	GtkWidget *b = gtk_button_new();
	GtkWidget *ov = gtk_overlay_new();
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *nl = gtk_label_new(name);
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(p));
	GdkPixbuf *pb = ex_icons_named(ex_location_icon_name(loc), EX_TILE_ICON,
				       scale);
	GtkWidget *img = pixbuf_image(pb, scale);
	GFile *parent = g_file_get_parent(f);
	char *sub = parent ? where_text(parent) : g_strdup("");
	GtkWidget *sl = dim_label(sub);
	char *tip;

	g_clear_object(&pb);
	g_clear_object(&parent);
	gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_bottom(img, 6);
	gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(nl), 14);
	gtk_label_set_xalign(GTK_LABEL(sl), 0.5);
	gtk_label_set_max_width_chars(GTK_LABEL(sl), 16);
	ex_style_add(sl, "ex-caption");
	gtk_box_pack_start(GTK_BOX(col), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), nl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), sl, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(ov), col);
	if (pinned) {
		GtkWidget *pin = themed_image("likeos-pin-symbolic",
					      "view-pin-symbolic", 12);

		gtk_widget_set_halign(pin, GTK_ALIGN_END);
		gtk_widget_set_valign(pin, GTK_ALIGN_START);
		ex_style_add(pin, "ex-pin");
		gtk_overlay_add_overlay(GTK_OVERLAY(ov), pin);
		gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(ov), pin, TRUE);
	}
	gtk_container_add(GTK_CONTAINER(b), ov);
	gtk_widget_set_size_request(b, EX_CARD_WIDTH, -1);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	ex_style_add(b, "ex-folder-card");
	tip = pinned ? g_strdup_printf("%s\n%s\nPinned", name, sub) :
		       g_strdup_printf("%s\n%s", name, sub);
	gtk_widget_set_tooltip_text(b, tip);
	atk_object_set_name(gtk_widget_get_accessible(b), name);
	atk_object_set_description(gtk_widget_get_accessible(b), sub);
	g_object_set_data_full(G_OBJECT(b), "ex-location", loc,
			       (GDestroyNotify)ex_location_unref);
	g_signal_connect(b, "clicked", G_CALLBACK(tile_clicked), p);
	g_signal_connect(b, "button-press-event", G_CALLBACK(tile_button), p);
	g_signal_connect(b, "popup-menu", G_CALLBACK(tile_menu_key), p);
	g_free(tip);
	g_free(sub);
	g_free(name);
	return b;
}

static GtkWidget *flow(void)
{
	GtkWidget *f = gtk_flow_box_new();

	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(f), GTK_SELECTION_NONE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(f), TRUE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(f), 1);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(f), 12);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(f), 8);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(f), 8);
	gtk_widget_set_halign(f, GTK_ALIGN_START);
	gtk_widget_set_valign(f, GTK_ALIGN_START);
	return f;
}

/* ---- the file lists -------------------------------------------------------- */

/* A picture that follows its file: a thumbnail made after the row was
 * built replaces the type's icon. */
static void row_image_update(ExFile *f, GtkWidget *img)
{
	int scale = gtk_widget_get_scale_factor(img);
	GdkPixbuf *pb = ex_icons_for_file(f, EX_LIST_ICON, scale, FALSE, TRUE);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;

	gtk_image_set_from_surface(GTK_IMAGE(img), s);
	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
}

/* When it was last opened: the access time the system keeps, or the time
 * it was changed when that is later (a disk mounted without access
 * times). */
static gint64 last_opened(ExFile *f)
{
	GFileInfo *i = ex_file_get_info(f);
	gint64 t = ex_file_get_mtime(f);

	if (i && g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_TIME_ACCESS))
		t = MAX(t, (gint64)g_file_info_get_attribute_uint64(
				   i, G_FILE_ATTRIBUTE_TIME_ACCESS));
	return t;
}

static GtkWidget *file_row(ExHomePage *p, ExFile *f)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *name = gtk_label_new(ex_file_get_name(f));
	GtkWidget *img = gtk_image_new();
	GFile *parent = g_file_get_parent(ex_file_get_file(f));
	char *where = parent ? where_text(parent) : g_strdup("");
	char *when = ex_format_time(last_opened(f));
	GtkWidget *wl = dim_label(where), *tl = dim_label(when);
	char *tip;

	gtk_widget_set_size_request(img, EX_LIST_ICON, EX_LIST_ICON);
	row_image_update(f, img);
	/* A thumbnail made later replaces the icon, while the row lives. */
	g_signal_connect_object(f, "changed", G_CALLBACK(row_image_update), img,
				0);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
	ex_style_add(wl, "ex-caption");
	gtk_box_pack_start(GTK_BOX(names), name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(names), wl, FALSE, FALSE, 0);
	gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
	gtk_widget_set_size_request(tl, EX_COL_WHEN, -1);
	ex_style_add(tl, "ex-caption");
	gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(h), names, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(h), tl, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(row), h);
	g_object_set_data_full(G_OBJECT(row), "ex-file", g_object_ref(f),
			       g_object_unref);
	tip = g_strdup_printf("%s\nLocation: %s\nLast opened: %s",
			      ex_file_get_name(f), where, when);
	gtk_widget_set_tooltip_text(row, tip);
	atk_object_set_name(gtk_widget_get_accessible(row), ex_file_get_name(f));
	g_free(tip);
	g_free(where);
	g_free(when);
	g_clear_object(&parent);
	return row;
}

static void row_activated(GtkListBox *lb, GtkListBoxRow *row, ExHomePage *p)
{
	ExFile *f = g_object_get_data(G_OBJECT(row), "ex-file");

	if (f)
		g_signal_emit(p, sigs[SIG_OPEN_FILE], 0, f);
}

static gboolean list_button(GtkWidget *w, GdkEventButton *ev, ExHomePage *p)
{
	GtkListBoxRow *row;
	ExFile *f;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return FALSE;
	row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(w), (int)ev->y);
	f = row ? g_object_get_data(G_OBJECT(row), "ex-file") : NULL;
	if (f) {
		gtk_list_box_select_row(GTK_LIST_BOX(w), row);
		gtk_widget_grab_focus(GTK_WIDGET(row));
		g_signal_emit(p, sigs[SIG_POPUP], 0, f, ev);
	}
	return TRUE;
}

/* The menu key (or Shift+F10) in a list: the menu of the focused row. */
static gboolean list_menu_key(GtkWidget *w, ExHomePage *p)
{
	GtkWidget *focus = gtk_container_get_focus_child(GTK_CONTAINER(w));
	ExFile *f = focus ? g_object_get_data(G_OBJECT(focus), "ex-file") : NULL;

	if (!f) {
		GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(w));

		f = row ? g_object_get_data(G_OBJECT(row), "ex-file") : NULL;
	}
	if (f)
		g_signal_emit(p, sigs[SIG_POPUP], 0, f, NULL);
	return f != NULL;
}

/* The titles over a list's columns. */
static GtkWidget *list_header(void)
{
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *name = gtk_label_new("Name");
	GtkWidget *when = gtk_label_new("Last opened");

	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_xalign(GTK_LABEL(when), 0);
	/* The name over the names, past the pictures. */
	gtk_widget_set_margin_start(name, EX_LIST_ICON + 12);
	gtk_widget_set_size_request(when, EX_COL_WHEN, -1);
	gtk_box_pack_start(GTK_BOX(h), name, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(h), when, FALSE, FALSE, 0);
	ex_style_add(h, "ex-list-header");
	return h;
}

static GtkWidget *file_list(ExHomePage *p, GPtrArray *files, const char *kind)
{
	GtkWidget *lb = gtk_list_box_new();

	gtk_list_box_set_selection_mode(GTK_LIST_BOX(lb), GTK_SELECTION_SINGLE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(lb), FALSE);
	for (guint i = 0; i < files->len; i++)
		gtk_container_add(GTK_CONTAINER(lb),
				  file_row(p, g_ptr_array_index(files, i)));
	g_signal_connect(lb, "row-activated", G_CALLBACK(row_activated), p);
	g_signal_connect(lb, "button-press-event", G_CALLBACK(list_button), p);
	g_signal_connect(lb, "popup-menu", G_CALLBACK(list_menu_key), p);
	ex_style_add(lb, "ex-file-list");
	atk_object_set_name(gtk_widget_get_accessible(lb), kind);
	return lb;
}

/* A list's place: filled when its files are known. */
static void fill_slot(GtkWidget *slot, ExHomePage *p, GPtrArray *all,
		      const char *kind, const char *empty)
{
	GPtrArray *files = all;

	/* The command bar's Filter: only the items of one kind. */
	if (all && p->filter) {
		files = g_ptr_array_new();
		for (guint i = 0; i < all->len; i++) {
			ExFile *f = g_ptr_array_index(all, i);

			if (!strcmp(ex_file_get_kind(f), p->filter))
				g_ptr_array_add(files, f);
		}
		if (all->len && !files->len)
			empty = "No items match the filter.";
	}
	gtk_container_foreach(GTK_CONTAINER(slot), destroy_child, NULL);
	if (files && files->len) {
		gtk_box_pack_start(GTK_BOX(slot), list_header(), FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(slot), file_list(p, files, kind), FALSE,
				   FALSE, 0);
	} else {
		gtk_box_pack_start(GTK_BOX(slot), placeholder(empty), FALSE, FALSE,
				   0);
	}
	gtk_widget_show_all(slot);
	if (files != all)
		g_ptr_array_unref(files);
}

void ex_home_page_set_filter(ExHomePage *p, const char *kind)
{
	char *k = kind && *kind && strcmp(kind, "all") ? g_strdup(kind) : NULL;

	if (!g_strcmp0(k, p->filter)) {
		g_free(k);
		return;
	}
	g_free(p->filter);
	p->filter = k;
	ex_home_page_reload(p);
}

const char *ex_home_page_get_filter(ExHomePage *p)
{
	return p->filter ? p->filter : "all";
}

#define EX_RECENT_EMPTY "No recent files yet. Files you open appear here."
#define EX_FAVORITES_EMPTY \
	"No favorites yet. Use Add to Favorites on a file to keep it here."

/* ---- filling in the background ----------------------------------------------- */

typedef struct {
	ExHomePage *p;
	GtkWidget *slot; /* the list's box */
	GtkWidget *frequent; /* the Pinned grid, for frequent folders */
	GPtrArray *pinned; /* GFile already in Pinned */
} Fill;

static void fill_free(Fill *f)
{
	g_object_unref(f->p);
	g_clear_object(&f->slot);
	g_clear_object(&f->frequent);
	if (f->pinned)
		g_ptr_array_unref(f->pinned);
	g_free(f);
}

/* The folders the recent files are in, by how many there are. */
static void add_frequent(Fill *fl, GPtrArray *recent)
{
	GHashTable *count = g_hash_table_new_full(g_file_hash,
						  (GEqualFunc)g_file_equal,
						  g_object_unref, NULL);
	GList *keys;
	GFile *home = ex_home_dir();
	int shown = 0;

	for (guint i = 0; i < recent->len; i++) {
		GFile *parent = g_file_get_parent(
			ex_file_get_file(g_ptr_array_index(recent, i)));

		if (!parent)
			continue;
		g_hash_table_replace(count, parent,
				     GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(
							     count, parent)) +
						     1));
	}
	keys = g_hash_table_get_keys(count);
	/* Most used first: a small insertion pass, there are few. */
	for (GList *a = keys; a; a = a->next)
		for (GList *b = a->next; b; b = b->next)
			if (GPOINTER_TO_INT(g_hash_table_lookup(count, b->data)) >
			    GPOINTER_TO_INT(g_hash_table_lookup(count, a->data))) {
				gpointer t = a->data;

				a->data = b->data;
				b->data = t;
			}
	for (GList *l = keys; l && shown < EX_FREQUENT_SHOWN; l = l->next) {
		gboolean dup = FALSE;

		for (guint i = 0; i < fl->pinned->len; i++)
			if (g_file_equal(g_ptr_array_index(fl->pinned, i), l->data))
				dup = TRUE;
		if (dup || g_file_equal(l->data, home))
			continue;
		gtk_container_add(GTK_CONTAINER(fl->frequent),
				  folder_tile(fl->p, l->data, NULL, FALSE));
		shown++;
	}
	g_list_free(keys);
	g_hash_table_unref(count);
	g_object_unref(home);
	gtk_widget_show_all(fl->frequent);
}

static void got_recent(GObject *src, GAsyncResult *res, gpointer data)
{
	Fill *fl = data;
	GPtrArray *files = ex_recent_list_finish(res, NULL);

	if (!g_cancellable_is_cancelled(fl->p->cancel)) {
		if (fl->frequent && files)
			add_frequent(fl, files);
		if (fl->slot)
			fill_slot(fl->slot, fl->p, files, "Recent",
				  EX_RECENT_EMPTY);
	}
	if (files)
		g_ptr_array_unref(files);
	fill_free(fl);
}

static void favorites_thread(GTask *task, gpointer src, gpointer data,
			     GCancellable *c)
{
	char **uris = data;
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);

	for (int i = 0; uris[i]; i++) {
		GFile *f = g_file_new_for_uri(uris[i]);
		ExFile *ef = ex_file_new_query(f, c, NULL);

		if (ef && (ex_file_get_caps(ef) & EX_CAP_READ))
			g_ptr_array_add(out, ef);
		else if (ef)
			g_object_unref(ef);
		g_object_unref(f);
	}
	g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
}

static void got_favorites(GObject *src, GAsyncResult *res, gpointer data)
{
	Fill *fl = data;
	GPtrArray *files = g_task_propagate_pointer(G_TASK(res), NULL);

	if (!g_cancellable_is_cancelled(fl->p->cancel))
		fill_slot(fl->slot, fl->p, files, "Favorites", EX_FAVORITES_EMPTY);
	if (files)
		g_ptr_array_unref(files);
	fill_free(fl);
}

/* ---- building -------------------------------------------------------------------- */

static void pinned_cards(ExHomePage *p, gboolean frequent, GPtrArray *pinned,
			 GtkWidget **grid)
{
	GtkWidget *rev = gtk_revealer_new();
	GtkWidget *qa = flow();
	GPtrArray *bm = ex_bookmarks_list(ex_bookmarks_get_default());

	gtk_box_pack_start(GTK_BOX(p->box),
			   fold_button("Pinned", rev, &g_pinned_folded),
			   FALSE, FALSE, 0);
	if (bm->len) {
		for (guint i = 0; i < bm->len; i++) {
			ExBookmark *b = g_ptr_array_index(bm, i);

			gtk_container_add(GTK_CONTAINER(qa),
					  folder_tile(p, b->file, b->label, TRUE));
			g_ptr_array_add(pinned, g_object_ref(b->file));
		}
	} else {
		/* Nothing pinned yet: the personal folders stand in, shown
		 * pinned, as the navigation pane shows them. */
		for (gsize i = 0; i < G_N_ELEMENTS(personal); i++) {
			const char *path = ex_user_dir(personal[i]);
			GFile *f;

			if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR))
				continue;
			f = g_file_new_for_path(path);
			gtk_container_add(GTK_CONTAINER(qa),
					  folder_tile(p, f, NULL, TRUE));
			g_ptr_array_add(pinned, f);
		}
	}
	ex_style_add(qa, "ex-pinned-grid");
	gtk_container_add(GTK_CONTAINER(rev), qa);
	gtk_box_pack_start(GTK_BOX(p->box), rev, FALSE, FALSE, 0);
	*grid = frequent ? qa : NULL;
}

static void pill_toggled(GtkToggleButton *b, GtkStack *stack)
{
	if (!gtk_toggle_button_get_active(b))
		return;
	g_show_favorites = !strcmp(gtk_widget_get_name(GTK_WIDGET(b)),
				   "favorites");
	gtk_stack_set_visible_child_name(stack, g_show_favorites ? "favorites" :
								    "recent");
}

static GtkWidget *pill(GtkWidget *group_with, const char *name,
		       const char *label, const char *icon, GtkStack *stack)
{
	GtkWidget *b = group_with ?
			       gtk_radio_button_new_from_widget(
				       GTK_RADIO_BUTTON(group_with)) :
			       gtk_radio_button_new(NULL);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
	gtk_box_pack_start(GTK_BOX(row), themed_image(icon, NULL, 16), FALSE,
			   FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), gtk_label_new(label), FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_widget_set_name(b, name);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	ex_style_add(b, "ex-switch-button");
	atk_object_set_name(gtk_widget_get_accessible(b), label);
	g_signal_connect(b, "toggled", G_CALLBACK(pill_toggled), stack);
	return b;
}

static GtkWidget *slot(void)
{
	return gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
}

static void build_home(ExHomePage *p)
{
	ExSettings *st = ex_settings_get();
	GPtrArray *pinned = g_ptr_array_new_with_free_func(g_object_unref);
	gboolean frequent = ex_settings_get_bool(st, "show-frequent");
	gboolean recent = ex_settings_get_bool(st, "show-recent");
	GtkWidget *grid = NULL, *rev, *head, *sw, *stack, *rpill = NULL, *fpill;
	GtkWidget *recent_slot = NULL, *fav_slot;
	char **fav;

	pinned_cards(p, frequent, pinned, &grid);

	/* Recent files | Favorites: a two-way switch, and after it the
	 * chevron that folds the list away. */
	rev = gtk_revealer_new();
	head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	sw = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	ex_style_add(sw, "ex-switch");
	stack = gtk_stack_new();
	gtk_stack_set_homogeneous(GTK_STACK(stack), FALSE);
	if (recent) {
		rpill = pill(NULL, "recent", "Recent files",
			     "document-open-recent-symbolic", GTK_STACK(stack));
		gtk_box_pack_start(GTK_BOX(sw), rpill, FALSE, FALSE, 0);
		recent_slot = slot();
		gtk_stack_add_named(GTK_STACK(stack), recent_slot, "recent");
	}
	fpill = pill(rpill, "favorites", "Favorites", "starred-symbolic",
		     GTK_STACK(stack));
	gtk_box_pack_start(GTK_BOX(sw), fpill, FALSE, FALSE, 0);
	gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(head), sw, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(head),
			   fold_button(NULL, rev, &g_files_folded), FALSE, FALSE,
			   0);
	fav_slot = slot();
	gtk_stack_add_named(GTK_STACK(stack), fav_slot, "favorites");
	ex_style_add(head, "ex-files-head");
	gtk_box_pack_start(GTK_BOX(p->box), head, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(rev), stack);
	gtk_box_pack_start(GTK_BOX(p->box), rev, FALSE, FALSE, 0);
	gtk_widget_show_all(stack);
	if (g_show_favorites || !recent) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fpill), TRUE);
		gtk_stack_set_visible_child(GTK_STACK(stack), fav_slot);
	} else {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rpill), TRUE);
		gtk_stack_set_visible_child(GTK_STACK(stack), recent_slot);
	}

	/* Favorites, read in the background. */
	fav = ex_settings_get_strv(st, "favorites");
	if (fav[0]) {
		Fill *fl = g_new0(Fill, 1);
		GTask *t;

		fl->p = g_object_ref(p);
		fl->slot = g_object_ref(fav_slot);
		t = g_task_new(NULL, p->cancel, got_favorites, fl);
		g_task_set_task_data(t, fav, (GDestroyNotify)g_strfreev);
		g_task_run_in_thread(t, favorites_thread);
		g_object_unref(t);
	} else {
		fill_slot(fav_slot, p, NULL, "Favorites", EX_FAVORITES_EMPTY);
		g_strfreev(fav);
	}

	/* Recent files, and the folders used most, read in the
	 * background. */
	if (recent || grid) {
		Fill *fl = g_new0(Fill, 1);

		fl->p = g_object_ref(p);
		fl->pinned = g_ptr_array_ref(pinned);
		if (grid)
			fl->frequent = g_object_ref(grid);
		if (recent_slot)
			fl->slot = g_object_ref(recent_slot);
		ex_recent_list_async(EX_RECENT_SHOWN, FALSE, p->cancel, got_recent,
				     fl);
	}
	g_ptr_array_unref(pinned);
}

/* ---- life cycle --------------------------------------------------------------- */

void ex_home_page_reload(ExHomePage *p)
{
	if (p->cancel) {
		g_cancellable_cancel(p->cancel);
		g_object_unref(p->cancel);
	}
	p->cancel = g_cancellable_new();
	gtk_container_foreach(GTK_CONTAINER(p->box), destroy_child, NULL);
	build_home(p);
	gtk_widget_show_all(p->box);
}

static gboolean reload_idle(gpointer data)
{
	ExHomePage *p = data;

	p->reload_id = 0;
	ex_home_page_reload(p);
	return G_SOURCE_REMOVE;
}

static void schedule_reload(ExHomePage *p)
{
	if (!p->reload_id)
		p->reload_id = g_timeout_add(300, reload_idle, p);
}

static void something_changed(gpointer src, ExHomePage *p)
{
	schedule_reload(p);
}

static void setting_changed(ExSettings *s, const char *key, ExHomePage *p)
{
	if (!strcmp(key, "favorites") || !strcmp(key, "show-recent") ||
	    !strcmp(key, "show-frequent"))
		schedule_reload(p);
}

static void recent_changed(GFileMonitor *m, GFile *f, GFile *o,
			   GFileMonitorEvent ev, ExHomePage *p)
{
	if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
	    ev == G_FILE_MONITOR_EVENT_CREATED ||
	    ev == G_FILE_MONITOR_EVENT_RENAMED || ev == G_FILE_MONITOR_EVENT_MOVED_IN)
		schedule_reload(p);
}

static void ex_home_page_dispose(GObject *o)
{
	ExHomePage *p = EX_HOME_PAGE(o);

	if (p->reload_id) {
		g_source_remove(p->reload_id);
		p->reload_id = 0;
	}
	if (p->cancel) {
		g_cancellable_cancel(p->cancel);
		g_clear_object(&p->cancel);
	}
	g_signal_handlers_disconnect_by_data(ex_bookmarks_get_default(), p);
	g_signal_handlers_disconnect_by_data(ex_settings_get(), p);
	if (p->recent_mon) {
		g_signal_handlers_disconnect_by_data(p->recent_mon, p);
		g_clear_object(&p->recent_mon);
	}
	G_OBJECT_CLASS(ex_home_page_parent_class)->dispose(o);
}

static void ex_home_page_finalize(GObject *o)
{
	g_free(EX_HOME_PAGE(o)->filter);
	G_OBJECT_CLASS(ex_home_page_parent_class)->finalize(o);
}

static void ex_home_page_class_init(ExHomePageClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->dispose = ex_home_page_dispose;
	G_OBJECT_CLASS(k)->finalize = ex_home_page_finalize;
	sigs[SIG_OPEN] = g_signal_new("open", t, G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				      NULL, G_TYPE_NONE, 2, EX_TYPE_LOCATION,
				      G_TYPE_BOOLEAN);
	sigs[SIG_OPEN_FILE] = g_signal_new("open-file", t, G_SIGNAL_RUN_LAST, 0,
					   NULL, NULL, NULL, G_TYPE_NONE, 1,
					   EX_TYPE_FILE);
	sigs[SIG_POPUP] = g_signal_new("popup", t, G_SIGNAL_RUN_LAST, 0, NULL,
				       NULL, NULL, G_TYPE_NONE, 2, EX_TYPE_FILE,
				       G_TYPE_POINTER);
	sigs[SIG_POPUP_LOCATION] = g_signal_new("popup-location", t,
						G_SIGNAL_RUN_LAST, 0, NULL, NULL,
						NULL, G_TYPE_NONE, 2,
						EX_TYPE_LOCATION, G_TYPE_POINTER);
}

static void ex_home_page_init(ExHomePage *p)
{
	GtkWidget *clamp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	ex_style_add(p->box, "ex-page-body");
	gtk_box_pack_start(GTK_BOX(clamp), p->box, FALSE, FALSE, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(p), clamp);
	ex_style_add(GTK_WIDGET(p), EX_CLASS_HOME);
}

GtkWidget *ex_home_page_new(void)
{
	ExHomePage *p = g_object_new(EX_TYPE_HOME_PAGE, NULL);
	GFile *rf;

	g_signal_connect(ex_bookmarks_get_default(), "changed",
			 G_CALLBACK(something_changed), p);
	g_signal_connect(ex_settings_get(), "changed",
			 G_CALLBACK(setting_changed), p);
	rf = ex_recent_file();
	p->recent_mon = g_file_monitor_file(rf, G_FILE_MONITOR_NONE, NULL, NULL);
	g_object_unref(rf);
	if (p->recent_mon)
		g_signal_connect(p->recent_mon, "changed",
				 G_CALLBACK(recent_changed), p);
	ex_home_page_reload(p);
	return GTK_WIDGET(p);
}
