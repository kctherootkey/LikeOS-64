// LikeOS -- File Explorer: Home and This PC.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-home-page.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-bookmarks.h"
#include "backend/ex-recent.h"
#include "backend/ex-util.h"
#include "backend/ex-volumes.h"
#include "services/ex-settings.h"

#include <string.h>

static void destroy_child(GtkWidget *w, gpointer data)
{
	gtk_widget_destroy(w);
}

#define EX_RECENT_SHOWN 20
#define EX_FREQUENT_SHOWN 4

struct _ExHomePage {
	GtkScrolledWindow parent;
	gboolean this_pc;
	GtkWidget *box;
	GCancellable *cancel;
	GFileMonitor *recent_mon;
	guint reload_id;
};

G_DEFINE_TYPE(ExHomePage, ex_home_page, GTK_TYPE_SCROLLED_WINDOW)

enum { SIG_OPEN, SIG_OPEN_FILE, SIG_POPUP, SIG_POPUP_LOCATION, N_SIGS };
static guint sigs[N_SIGS];

/* ---- pieces ---------------------------------------------------------------- */

static GtkWidget *section_title(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	ex_style_add(l, EX_CLASS_SECTION_TITLE);
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

static void tile_clicked(GtkButton *b, ExHomePage *p)
{
	ExLocation *l = g_object_get_data(G_OBJECT(b), "ex-location");

	g_signal_emit(p, sigs[SIG_OPEN], 0, l, FALSE);
}

static gboolean tile_button(GtkWidget *w, GdkEventButton *ev, ExHomePage *p)
{
	ExLocation *l = g_object_get_data(G_OBJECT(w), "ex-location");

	if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
		g_signal_emit(p, sigs[SIG_OPEN], 0, l, TRUE);
		return TRUE;
	}
	if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
		g_signal_emit(p, sigs[SIG_POPUP_LOCATION], 0, l, ev);
		return TRUE;
	}
	return FALSE;
}

/* The menu key (or Shift+F10) on a focused tile. */
static gboolean tile_menu_key(GtkWidget *w, ExHomePage *p)
{
	g_signal_emit(p, sigs[SIG_POPUP_LOCATION], 0,
		      g_object_get_data(G_OBJECT(w), "ex-location"), NULL);
	return TRUE;
}

/* A tile: icon, name, a line under it, and optionally a usage bar. */
static GtkWidget *tile(ExHomePage *p, ExLocation *loc, const char *icon,
		       const char *name, const char *sub, double fill)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *nl = gtk_label_new(name);
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(p));
	/* 48: a size the icon theme has as a picture, drawn unscaled. */
	GdkPixbuf *pb = ex_icons_named(icon, 48, scale);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);

	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	gtk_label_set_xalign(GTK_LABEL(nl), 0);
	gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(nl), 22);
	gtk_box_pack_start(GTK_BOX(text), nl, FALSE, FALSE, 0);
	if (fill >= 0) {
		GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);

		gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), fill);
		gtk_widget_set_size_request(bar, 160, 6);
		gtk_box_pack_start(GTK_BOX(text), bar, FALSE, FALSE, 2);
	}
	if (sub) {
		GtkWidget *sl = gtk_label_new(sub);

		gtk_label_set_xalign(GTK_LABEL(sl), 0);
		gtk_label_set_ellipsize(GTK_LABEL(sl), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(sl), 26);
		ex_style_add(sl, EX_CLASS_DIM);
		gtk_box_pack_start(GTK_BOX(text), sl, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(b), row);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	ex_style_add(b, EX_CLASS_TILE);
	gtk_widget_set_tooltip_text(b, name);
	g_object_set_data_full(G_OBJECT(b), "ex-location", ex_location_ref(loc),
			       (GDestroyNotify)ex_location_unref);
	g_signal_connect(b, "clicked", G_CALLBACK(tile_clicked), p);
	g_signal_connect(b, "button-press-event", G_CALLBACK(tile_button), p);
	g_signal_connect(b, "popup-menu", G_CALLBACK(tile_menu_key), p);
	return b;
}

static GtkWidget *flow(void)
{
	GtkWidget *f = gtk_flow_box_new();

	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(f), GTK_SELECTION_NONE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(f), TRUE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(f), 6);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(f), 6);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(f), 6);
	return f;
}

static void add_folder_tile(ExHomePage *p, GtkWidget *fb, GFile *f,
			    const char *label, const char *sub)
{
	ExLocation *l = ex_location_new_folder(f);
	char *name = label ? g_strdup(label) : ex_location_display_name(l);

	gtk_container_add(GTK_CONTAINER(fb),
			  tile(p, l, ex_location_icon_name(l), name, sub, -1));
	g_free(name);
	ex_location_unref(l);
}

static const GUserDirectory personal[] = {
	G_USER_DIRECTORY_DESKTOP,  G_USER_DIRECTORY_DOWNLOAD,
	G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_PICTURES,
	G_USER_DIRECTORY_MUSIC,	   G_USER_DIRECTORY_VIDEOS,
};

static void add_personal(ExHomePage *p, GtkWidget *fb, const char *sub)
{
	for (gsize i = 0; i < G_N_ELEMENTS(personal); i++) {
		const char *path = ex_user_dir(personal[i]);

		if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR))
			continue;
		GFile *f = g_file_new_for_path(path);
		add_folder_tile(p, fb, f, NULL, sub);
		g_object_unref(f);
	}
}

/* A row of the file lists (Favorites, Recent). */
static GtkWidget *file_row(ExHomePage *p, ExFile *f)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *name = gtk_label_new(ex_file_get_name(f));
	GFile *parent = g_file_get_parent(ex_file_get_file(f));
	char *where = parent ? g_file_get_parse_name(parent) : g_strdup("");
	char *when = ex_format_time(ex_file_get_mtime(f));
	GtkWidget *wl = gtk_label_new(where), *tl = gtk_label_new(when);
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(p));
	GdkPixbuf *pb = ex_icons_for_file(f, 24, scale, FALSE, FALSE);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;

	gtk_box_pack_start(GTK_BOX(h), gtk_image_new_from_surface(s), FALSE, FALSE,
			   0);
	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_size_request(name, 280, -1);
	gtk_label_set_xalign(GTK_LABEL(wl), 0);
	gtk_label_set_ellipsize(GTK_LABEL(wl), PANGO_ELLIPSIZE_START);
	ex_style_add(wl, EX_CLASS_DIM);
	ex_style_add(tl, EX_CLASS_DIM);
	gtk_box_pack_start(GTK_BOX(h), name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(h), wl, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(h), tl, FALSE, FALSE, 0);
	gtk_container_set_border_width(GTK_CONTAINER(h), 4);
	gtk_container_add(GTK_CONTAINER(row), h);
	g_object_set_data_full(G_OBJECT(row), "ex-file", g_object_ref(f),
			       g_object_unref);
	gtk_widget_set_tooltip_text(row, where);
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
		g_signal_emit(p, sigs[SIG_POPUP], 0, f, ev);
	}
	return TRUE;
}

static GtkWidget *file_list(ExHomePage *p, GPtrArray *files, const char *kind)
{
	GtkWidget *lb = gtk_list_box_new();

	gtk_list_box_set_selection_mode(GTK_LIST_BOX(lb), GTK_SELECTION_SINGLE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(lb), FALSE);
	for (guint i = 0; i < files->len; i++)
		gtk_container_add(GTK_CONTAINER(lb),
				  file_row(p, g_ptr_array_index(files, i)));
	g_object_set_data(G_OBJECT(lb), "ex-kind", (gpointer)kind);
	g_signal_connect(lb, "row-activated", G_CALLBACK(row_activated), p);
	g_signal_connect(lb, "button-press-event", G_CALLBACK(list_button), p);
	atk_object_set_name(gtk_widget_get_accessible(lb), kind);
	return lb;
}

/* ---- Home ------------------------------------------------------------------ */

typedef struct {
	ExHomePage *p;
	GtkWidget *slot; /* a box the section fills */
	GtkWidget *frequent_slot;
	GPtrArray *pinned; /* GFile already in Quick access */
} Fill;

static void fill_free(Fill *f)
{
	g_object_unref(f->p);
	g_clear_object(&f->slot);
	g_clear_object(&f->frequent_slot);
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
		add_folder_tile(fl->p, fl->frequent_slot, l->data, NULL,
				"Frequent folder");
		shown++;
	}
	g_list_free(keys);
	g_hash_table_unref(count);
	g_object_unref(home);
	gtk_widget_show_all(fl->frequent_slot);
}

static void got_recent(GObject *src, GAsyncResult *res, gpointer data)
{
	Fill *fl = data;
	GPtrArray *files = ex_recent_list_finish(res, NULL);

	if (files && !g_cancellable_is_cancelled(fl->p->cancel)) {
		if (fl->frequent_slot)
			add_frequent(fl, files);
		if (fl->slot) {
			if (files->len)
				gtk_container_add(GTK_CONTAINER(fl->slot),
						  file_list(fl->p, files, "Recent"));
			else
				gtk_container_add(GTK_CONTAINER(fl->slot),
						  placeholder("Files you open will "
							      "show up here."));
			gtk_widget_show_all(fl->slot);
		}
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

	if (files && files->len && !g_cancellable_is_cancelled(fl->p->cancel)) {
		gtk_container_add(GTK_CONTAINER(fl->slot), section_title("Favorites"));
		gtk_container_add(GTK_CONTAINER(fl->slot),
				  file_list(fl->p, files, "Favorites"));
		gtk_widget_show_all(fl->slot);
	}
	if (files)
		g_ptr_array_unref(files);
	fill_free(fl);
}

static GtkWidget *slot(GtkWidget *box)
{
	GtkWidget *s = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

	gtk_box_pack_start(GTK_BOX(box), s, FALSE, FALSE, 0);
	return s;
}

static void build_home(ExHomePage *p)
{
	ExSettings *st = ex_settings_get();
	GtkWidget *qa = flow();
	GPtrArray *bm = ex_bookmarks_list(ex_bookmarks_get_default());
	GPtrArray *pinned = g_ptr_array_new_with_free_func(g_object_unref);
	gboolean frequent = ex_settings_get_bool(st, "show-frequent");
	gboolean recent = ex_settings_get_bool(st, "show-recent");

	gtk_box_pack_start(GTK_BOX(p->box), section_title("Quick access"), FALSE,
			   FALSE, 0);
	if (bm->len) {
		for (guint i = 0; i < bm->len; i++) {
			ExBookmark *b = g_ptr_array_index(bm, i);

			add_folder_tile(p, qa, b->file, b->label, "Pinned");
			g_ptr_array_add(pinned, g_object_ref(b->file));
		}
	} else {
		/* Nothing pinned yet: the personal folders stand in. */
		add_personal(p, qa, "Folder");
	}
	gtk_box_pack_start(GTK_BOX(p->box), qa, FALSE, FALSE, 0);
	GtkWidget *fq = frequent ? flow() : NULL;
	if (fq)
		gtk_box_pack_start(GTK_BOX(p->box), fq, FALSE, FALSE, 0);

	/* Favorites, read in the background. */
	char **fav = ex_settings_get_strv(st, "favorites");
	if (fav[0]) {
		Fill *fl = g_new0(Fill, 1);
		GTask *t;

		fl->p = g_object_ref(p);
		fl->slot = g_object_ref(slot(p->box));
		t = g_task_new(NULL, p->cancel, got_favorites, fl);
		g_task_set_task_data(t, fav, (GDestroyNotify)g_strfreev);
		g_task_run_in_thread(t, favorites_thread);
		g_object_unref(t);
	} else {
		g_strfreev(fav);
	}

	if (recent || fq) {
		Fill *fl = g_new0(Fill, 1);

		fl->p = g_object_ref(p);
		fl->pinned = g_ptr_array_ref(pinned);
		if (fq)
			fl->frequent_slot = g_object_ref(fq);
		if (recent) {
			gtk_box_pack_start(GTK_BOX(p->box), section_title("Recent"),
					   FALSE, FALSE, 0);
			fl->slot = g_object_ref(slot(p->box));
		}
		ex_recent_list_async(EX_RECENT_SHOWN, FALSE, p->cancel, got_recent,
				     fl);
	}
	g_ptr_array_unref(pinned);

	/* The other places worth a click. */
	GtkWidget *loc = flow();
	ExLocation *pc = ex_location_new_special(EX_LOC_THISPC);
	ExLocation *tr = ex_location_new_special(EX_LOC_TRASH);

	gtk_box_pack_start(GTK_BOX(p->box), section_title("Locations"), FALSE,
			   FALSE, 0);
	gtk_container_add(GTK_CONTAINER(loc),
			  tile(p, pc, "computer", "This PC", "Drives and folders",
			       -1));
	gtk_container_add(GTK_CONTAINER(loc),
			  tile(p, tr, "user-trash", "Trash", "Deleted items", -1));
	ex_location_unref(pc);
	ex_location_unref(tr);
	gtk_box_pack_start(GTK_BOX(p->box), loc, FALSE, FALSE, 0);
}

/* ---- This PC ---------------------------------------------------------------- */

static void build_thispc(ExHomePage *p)
{
	GtkWidget *folders = flow(), *drives = flow();
	GPtrArray *vl = ex_volumes_list(ex_volumes_get_default());

	gtk_box_pack_start(GTK_BOX(p->box), section_title("Folders"), FALSE, FALSE,
			   0);
	add_personal(p, folders, NULL);
	gtk_box_pack_start(GTK_BOX(p->box), folders, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p->box), section_title("Devices and drives"),
			   FALSE, FALSE, 0);
	for (guint i = 0; vl && i < vl->len; i++) {
		ExVolume *v = g_ptr_array_index(vl, i);
		ExLocation *l = ex_location_new_folder(v->root);
		char *sub = NULL;
		double fill = -1;

		if (v->size) {
			char *fr = ex_format_size(v->free);
			char *sz = ex_format_size(v->size);

			sub = g_strdup_printf("%s free of %s", fr, sz);
			fill = 1.0 - (double)v->free / (double)v->size;
			g_free(fr);
			g_free(sz);
		}
		gtk_container_add(GTK_CONTAINER(drives),
				  tile(p, l, v->icon_name, v->name, sub, fill));
		g_free(sub);
		ex_location_unref(l);
	}
	gtk_box_pack_start(GTK_BOX(p->box), drives, FALSE, FALSE, 0);
}

/* ---- life cycle --------------------------------------------------------------- */

void ex_home_page_reload(ExHomePage *p)
{
	if (p->cancel) {
		g_cancellable_cancel(p->cancel);
		g_object_unref(p->cancel);
	}
	p->cancel = g_cancellable_new();
	gtk_container_foreach(GTK_CONTAINER(p->box), destroy_child,
			      NULL);
	if (p->this_pc)
		build_thispc(p);
	else
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
	g_signal_handlers_disconnect_by_data(ex_volumes_get_default(), p);
	g_signal_handlers_disconnect_by_data(ex_settings_get(), p);
	if (p->recent_mon) {
		g_signal_handlers_disconnect_by_data(p->recent_mon, p);
		g_clear_object(&p->recent_mon);
	}
	G_OBJECT_CLASS(ex_home_page_parent_class)->dispose(o);
}

static void ex_home_page_class_init(ExHomePageClass *k)
{
	GType t = G_TYPE_FROM_CLASS(k);

	G_OBJECT_CLASS(k)->dispose = ex_home_page_dispose;
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

	p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(p->box), 16);
	gtk_box_pack_start(GTK_BOX(clamp), p->box, FALSE, FALSE, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(p), clamp);
	ex_style_add(GTK_WIDGET(p), EX_CLASS_HOME);
}

GtkWidget *ex_home_page_new(gboolean this_pc)
{
	ExHomePage *p = g_object_new(EX_TYPE_HOME_PAGE, NULL);
	GFile *rf;

	p->this_pc = this_pc;
	g_signal_connect(ex_volumes_get_default(), "changed",
			 G_CALLBACK(something_changed), p);
	if (!this_pc) {
		g_signal_connect(ex_bookmarks_get_default(), "changed",
				 G_CALLBACK(something_changed), p);
		g_signal_connect(ex_settings_get(), "changed",
				 G_CALLBACK(setting_changed), p);
		rf = ex_recent_file();
		p->recent_mon = g_file_monitor_file(rf, G_FILE_MONITOR_NONE, NULL,
						    NULL);
		g_object_unref(rf);
		if (p->recent_mon)
			g_signal_connect(p->recent_mon, "changed",
					 G_CALLBACK(recent_changed), p);
	}
	ex_home_page_reload(p);
	return GTK_WIDGET(p);
}
