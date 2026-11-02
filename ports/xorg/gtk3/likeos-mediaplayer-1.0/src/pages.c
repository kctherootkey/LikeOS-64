// LikeOS -- LikeOS Media Player: the sidebar and the pages.
//
// The window is a sidebar at the left -- the search box and the places in
// three groups: the library (Recent, Music, Videos), playback (Now playing,
// Queue) and the playlists, with Settings at its foot -- and the page the
// sidebar selects beside it, over the dark transport strip.  This file
// builds the sidebar and every page except the queue and the "now playing"
// stage, which window.c builds because they are part of playback.
//
//   home        "Recent": what was played lately, new albums
//   music       the music library: songs, albums, artists
//   videos      the video library
//   collection  one album or one artist, opened from the library
//   search      what the search box found
//   playlists   saved playlists
//   settings    library folders, playback options, about
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>

#include <glib/gstdio.h>

#include "mediaplayer.h"
#include "ui.h"
#include "util.h"

#define TILE_ALBUM 160
#define TILE_ARTIST 140
#define TILE_VIDEO 240
#define RECENT_SHOWN 12

enum {
	SORT_TITLE,
	SORT_ARTIST,
	SORT_ALBUM,
	SORT_ADDED,
	N_SORTS
};

static const char *const sort_names[N_SORTS] = {
	"A – Z", "Artist", "Album", "Date added",
};

typedef struct {
	MpApp *app;

	/* navigation */
	GtkWidget *nav_top;	/* list: Home .. Playlists */
	GtkWidget *nav_bottom;	/* list: Settings */
	GtkWidget *search;
	GtkWidget *search_button; /* stands in for the box when folded */
	GtkWidget *nav_toggle;
	gboolean syncing_nav;
	gchar *page;
	gchar *prev_page;

	/* home */
	GtkWidget *recent_grid;
	GtkWidget *recent_empty;
	GtkWidget *recent_clear;
	GtkWidget *added_title;
	GtkWidget *added_grid;

	/* music */
	GtkWidget *music_outer;	/* "empty" or "library" */
	GtkWidget *music_stack;	/* songs, albums, artists */
	GtkWidget *views[3];	/* the segments of the view switch */
	GtkWidget *sort_label;
	GtkWidget *songs_view;
	GtkTreeModel *songs_filter;
	GtkTreeModel *songs_sort;
	GtkWidget *albums_grid;
	GtkWidget *artists_grid;
	GtkWidget *music_header;	/* its status line: counts */
	int sort;

	/* videos */
	GtkWidget *videos_header;
	GtkWidget *videos_outer;
	GtkWidget *videos_grid;

	/* collection */
	GtkWidget *coll_kind;
	GtkWidget *coll_title;
	GtkWidget *coll_sub;
	GtkWidget *coll_art;
	GtkWidget *coll_view;
	GtkTreeModel *coll_filter;
	GtkTreeModel *coll_sort;
	gchar *coll_album;	/* album key, or NULL */
	gchar *coll_artist;	/* artist key, or NULL */

	/* search */
	GtkWidget *search_title;
	GtkWidget *search_header;
	GtkWidget *search_view;
	GtkTreeModel *search_filter;
	gchar *query;		/* casefolded */

	/* playlists */
	GtkWidget *pl_outer;
	GtkWidget *pl_grid;
	GtkWidget *pl_menu;
	gchar *pl_menu_path;

	/* settings */
	GtkWidget *music_dirs_box;
	GtkWidget *video_dirs_box;
	GtkWidget *lib_status;
} Pages;

static Pages *pages_of(MpApp *app)
{
	return g_object_get_data(G_OBJECT(app->window), "mp-pages");
}

static void refresh_library_views(Pages *pg);
static void refresh_playlists(Pages *pg);
static void refresh_settings_dirs(Pages *pg);
static void refresh_home(Pages *pg);
static gboolean refresh_dirs_idle(gpointer d);

/* ---- helpers ---------------------------------------------------------- */

static GtkWidget *scrolled(GtkWidget *child)
{
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_container_add(GTK_CONTAINER(sw), child);
	return sw;
}

static GtkWidget *page_box(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mp_add_class(box, "mp-page");
	return box;
}

/* What an empty page says, with an optional button under it. */
static GtkWidget *empty_state(const char *likeos, const char *fallback,
			      const char *title, const char *text,
			      GtkWidget *button)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	mp_add_class(box, "mp-empty");
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	GtkWidget *g = mp_glyph_new(likeos, fallback, 48);
	mp_add_class(g, "mp-empty-glyph");
	gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
	GtkWidget *t = gtk_label_new(title);
	mp_add_class(t, "mp-empty-title");
	gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
	mp_add_class(l, "mp-empty-text");
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	if (button != NULL) {
		gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
		gtk_widget_set_margin_top(button, 8);
		gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
	}
	return box;
}

static gchar *count_text(guint n, const char *one, const char *many)
{
	return g_strdup_printf("%u %s", n, n == 1 ? one : many);
}

static gchar *minutes_text(gint64 ns)
{
	if (ns < 60 * GST_SECOND)
		return g_strdup_printf("%lld sec",
				       (long long)((ns + GST_SECOND / 2) /
						   GST_SECOND));
	gint64 min = (ns + 30 * GST_SECOND) / (60 * GST_SECOND);
	if (min >= 60)
		return g_strdup_printf("%lld h %lld min", (long long)(min / 60),
				       (long long)(min % 60));
	return g_strdup_printf("%lld min", (long long)min);
}

/* Every URI of a list model, in its order. */
static GPtrArray *model_uris(GtkTreeModel *m)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
	GtkTreeIter it;
	for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
	     ok = gtk_tree_model_iter_next(m, &it)) {
		gchar *uri = NULL;
		gtk_tree_model_get(m, &it, MP_LIB_URI, &uri, -1);
		g_ptr_array_add(a, uri);
	}
	return a;
}

/* ---- folders ---------------------------------------------------------- */

static gchar *choose_folder(MpApp *app, const char *title)
{
	GtkWidget *d = gtk_file_chooser_dialog_new(
		title, GTK_WINDOW(app->window),
		GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancel",
		GTK_RESPONSE_CANCEL, "_Add this folder", GTK_RESPONSE_ACCEPT,
		NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d),
					    g_get_home_dir());
	gchar *path = NULL;
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
		path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
	gtk_widget_destroy(d);
	return path;
}

static void add_library_folder(MpApp *app, gboolean video)
{
	gchar *path = choose_folder(app, video ? "Add a Folder to the Video "
						 "Library"
					       : "Add a Folder to the Music "
						 "Library");
	if (path == NULL)
		return;
	gchar ***dirs = video ? &app->settings.video_dirs
			      : &app->settings.music_dirs;
	if (*dirs == NULL || !g_strv_contains((const gchar *const *)*dirs,
					      path)) {
		guint n = *dirs ? g_strv_length(*dirs) : 0;
		*dirs = g_renew(gchar *, *dirs, n + 2);
		(*dirs)[n] = path;
		(*dirs)[n + 1] = NULL;
		path = NULL;
		mp_app_schedule_save(app);
		mp_pages_rescan(app, FALSE);
		refresh_settings_dirs(pages_of(app));
	}
	g_free(path);
}

static void add_music_folder(MpApp *app)
{
	add_library_folder(app, FALSE);
}

static void add_video_folder(MpApp *app)
{
	add_library_folder(app, TRUE);
}

void mp_pages_rescan(MpApp *app, gboolean retag)
{
	mp_library_scan(app->library, app->settings.music_dirs,
			app->settings.video_dirs, retag);
}

/* ---- list columns ----------------------------------------------------- */

static gboolean row_is_current(GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
	MpApp *app = d;
	const char *cur = mp_app_current_uri(app);
	if (cur == NULL)
		return FALSE;
	gchar *uri = NULL;
	gtk_tree_model_get(m, it, MP_LIB_URI, &uri, -1);
	gboolean same = uri != NULL && strcmp(uri, cur) == 0;
	g_free(uri);
	return same;
}

/* Text cells: the playing row in the accent colour (through the
 * "mp-row-current" class the painter gives it, read back here), unknown
 * artists and albums spelt out. */
typedef struct {
	MpApp *app;
	int col;
	gboolean secondary;
} CellInfo;

static void text_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
		      GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	CellInfo *ci = data;
	(void)col;
	gchar *s = NULL;
	gtk_tree_model_get(m, it, ci->col, &s, -1);
	const char *shown = s;
	if (s == NULL || *s == '\0') {
		if (ci->col == MP_LIB_ARTIST)
			shown = "Unknown artist";
		else if (ci->col == MP_LIB_ALBUM)
			shown = "Unknown album";
	}
	gboolean cur = row_is_current(m, it, ci->app);
	GtkWidget *tv = gtk_tree_view_column_get_tree_view(col);
	GtkStyleContext *ctx = gtk_widget_get_style_context(tv);
	GdkRGBA c;
	gtk_style_context_save(ctx);
	/* The row card shows whether the row is selected or under the
	 * pointer; the text keeps its own colour in every state. */
	gtk_style_context_set_state(ctx, GTK_STATE_FLAG_NORMAL);
	gtk_style_context_add_class(ctx, cur ? "mp-cell-current"
					     : ci->secondary
						       ? "mp-cell-secondary"
						       : "mp-cell");
	gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &c);
	gtk_style_context_restore(ctx);
	mp_cell_set_text(cell, shown, &c, cur);
	g_free(s);
}

static void track_cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
		       GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	(void)col;
	(void)data;
	guint n = 0;
	gtk_tree_model_get(m, it, MP_LIB_TRACK, &n, -1);
	gchar *s = n > 0 ? g_strdup_printf("%u", n) : g_strdup("");
	g_object_set(cell, "text", s, NULL);
	g_free(s);
}

static void add_text_column(GtkTreeView *tv, MpApp *app, int col,
			    const char *title, gboolean secondary,
			    gboolean expand, int width, float xalign)
{
	CellInfo *ci = g_new0(CellInfo, 1);
	ci->app = app;
	ci->col = col;
	ci->secondary = secondary;
	GtkCellRenderer *r = gtk_cell_renderer_text_new();
	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 11,
		     "xpad", 12, "xalign", xalign, NULL);
	GtkTreeViewColumn *c = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(c, title);
	gtk_tree_view_column_set_alignment(c, xalign);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_set_cell_data_func(c, r, text_cell, ci, g_free);
	gtk_tree_view_column_set_expand(c, expand);
	if (width > 0) {
		gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width(c, width);
	}
	gtk_tree_view_append_column(tv, c);
}

/* A list of library entries: title, artist, album and length, or (for an
 * album) number, title, artist and length. */
static GtkWidget *song_list(MpApp *app, GtkTreeModel *model, gboolean album)
{
	GtkWidget *w = gtk_tree_view_new_with_model(model);
	GtkTreeView *tv = GTK_TREE_VIEW(w);
	/* Column titles in small capitals over the rows; they do not sort
	 * (the page's sort menu does). */
	gtk_tree_view_set_headers_visible(tv, TRUE);
	gtk_tree_view_set_headers_clickable(tv, FALSE);
	gtk_tree_view_set_enable_search(tv, FALSE);
	gtk_tree_view_set_fixed_height_mode(tv, FALSE);
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv),
				    GTK_SELECTION_MULTIPLE);
	if (album) {
		GtkCellRenderer *r = gtk_cell_renderer_text_new();
		g_object_set(r, "xalign", 1.0, "xpad", 12, "ypad", 11, NULL);
		GtkTreeViewColumn *c = gtk_tree_view_column_new();
		gtk_tree_view_column_set_title(c, "#");
		gtk_tree_view_column_set_alignment(c, 1.0);
		gtk_tree_view_column_pack_start(c, r, TRUE);
		gtk_tree_view_column_set_cell_data_func(c, r, track_cell, NULL,
							NULL);
		gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_fixed_width(c, 48);
		gtk_tree_view_append_column(tv, c);
	}
	add_text_column(tv, app, MP_LIB_TITLE, "TITLE", FALSE, TRUE, 0, 0.0f);
	add_text_column(tv, app, MP_LIB_ARTIST, "ARTIST", TRUE, FALSE, 200,
			0.0f);
	if (!album)
		add_text_column(tv, app, MP_LIB_ALBUM, "ALBUM", TRUE, FALSE, 200,
				0.0f);
	add_text_column(tv, app, MP_LIB_DURATION_TEXT, "LENGTH", TRUE, FALSE,
			80, 1.0f);
	mp_list_style(tv, row_is_current, app);
	return w;
}

/* Activating a row plays the whole list from that row on, the way the
 * library pages of every music player do. */
static void on_song_activated(GtkTreeView *tv, GtkTreePath *path,
			      GtkTreeViewColumn *col, gpointer data)
{
	MpApp *app = data;
	(void)col;
	GPtrArray *uris = model_uris(gtk_tree_view_get_model(tv));
	gint idx = gtk_tree_path_get_indices(path)[0];
	if (uris->len > 0)
		mp_app_play_uris(app, uris, (guint)idx, FALSE);
	g_ptr_array_free(uris, TRUE);
}

static GPtrArray *selected_uris(GtkTreeView *tv)
{
	GtkTreeModel *m = NULL;
	GList *rows = gtk_tree_selection_get_selected_rows(
		gtk_tree_view_get_selection(tv), &m);
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
	for (GList *l = rows; l != NULL; l = l->next) {
		GtkTreeIter it;
		gchar *uri = NULL;
		if (gtk_tree_model_get_iter(m, &it, l->data)) {
			gtk_tree_model_get(m, &it, MP_LIB_URI, &uri, -1);
			g_ptr_array_add(a, uri);
		}
	}
	g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
	return a;
}

typedef struct {
	MpApp *app;
	GtkTreeView *tv;
	int what;	/* 0 play, 1 play next, 2 add to queue */
} SongAction;

static void song_action(GtkMenuItem *item, gpointer data)
{
	SongAction *sa = data;
	(void)item;
	GPtrArray *uris = selected_uris(sa->tv);
	if (uris->len > 0) {
		if (sa->what == 0)
			mp_app_play_uris(sa->app, uris, 0, FALSE);
		else
			mp_app_queue_uris(sa->app, uris, sa->what == 1);
	}
	g_ptr_array_free(uris, TRUE);
}

static gboolean on_song_button(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	MpApp *app = d;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	GtkTreeView *tv = GTK_TREE_VIEW(w);
	GtkTreePath *path = NULL;
	if (!gtk_tree_view_get_path_at_pos(tv, (gint)ev->x, (gint)ev->y, &path,
					   NULL, NULL, NULL))
		return TRUE;
	GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
	if (!gtk_tree_selection_path_is_selected(sel, path)) {
		gtk_tree_selection_unselect_all(sel);
		gtk_tree_selection_select_path(sel, path);
	}
	gtk_tree_path_free(path);

	static const char *const labels[] = { "_Play", "Play _next",
					      "_Add to queue" };
	GtkWidget *menu = gtk_menu_new();
	for (int i = 0; i < 3; i++) {
		GtkWidget *it = gtk_menu_item_new_with_mnemonic(labels[i]);
		SongAction *sa = g_new0(SongAction, 1);
		sa->app = app;
		sa->tv = tv;
		sa->what = i;
		g_signal_connect_data(it, "activate", G_CALLBACK(song_action),
				      sa, (GClosureNotify)(void (*)(void))g_free,
				      0);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
	}
	gtk_widget_show_all(menu);
	gtk_menu_attach_to_widget(GTK_MENU(menu), w, NULL);
	g_signal_connect(menu, "selection-done",
			 G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
	return TRUE;
}

static void connect_song_list(GtkWidget *view, MpApp *app)
{
	g_signal_connect(view, "row-activated", G_CALLBACK(on_song_activated),
			 app);
	g_signal_connect(view, "button-press-event", G_CALLBACK(on_song_button),
			 app);
}

/* ---- sorting ---------------------------------------------------------- */

static gint cmp_col(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, int col)
{
	gchar *x = NULL, *y = NULL;
	gtk_tree_model_get(m, a, col, &x, -1);
	gtk_tree_model_get(m, b, col, &y, -1);
	gint r = g_strcmp0(x, y);
	g_free(x);
	g_free(y);
	return r;
}

static gint cmp_track(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b)
{
	guint x = 0, y = 0;
	gtk_tree_model_get(m, a, MP_LIB_TRACK, &x, -1);
	gtk_tree_model_get(m, b, MP_LIB_TRACK, &y, -1);
	return x < y ? -1 : x > y ? 1 : 0;
}

static gint sort_songs(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b,
		       gpointer data)
{
	Pages *pg = data;
	gint r = 0;
	switch (pg->sort) {
	case SORT_ARTIST:
		r = cmp_col(m, a, b, MP_LIB_ARTIST_KEY);
		if (r == 0)
			r = cmp_col(m, a, b, MP_LIB_ALBUM_KEY);
		if (r == 0)
			r = cmp_track(m, a, b);
		break;
	case SORT_ALBUM:
		r = cmp_col(m, a, b, MP_LIB_ALBUM_KEY);
		if (r == 0)
			r = cmp_track(m, a, b);
		break;
	case SORT_ADDED: {
		gint64 x = 0, y = 0;
		gtk_tree_model_get(m, a, MP_LIB_ADDED, &x, -1);
		gtk_tree_model_get(m, b, MP_LIB_ADDED, &y, -1);
		r = x > y ? -1 : x < y ? 1 : 0;
		break;
	}
	default:
		break;
	}
	if (r == 0)
		r = cmp_col(m, a, b, MP_LIB_TITLE_KEY);
	return r;
}

/* An album's or an artist's songs: by album, then track number. */
static gint sort_collection(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b,
			    gpointer data)
{
	(void)data;
	gint r = cmp_col(m, a, b, MP_LIB_ALBUM_KEY);
	if (r == 0)
		r = cmp_track(m, a, b);
	if (r == 0)
		r = cmp_col(m, a, b, MP_LIB_TITLE_KEY);
	return r;
}

static gboolean visible_audio(GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
	(void)d;
	gboolean video = FALSE;
	gtk_tree_model_get(m, it, MP_LIB_VIDEO, &video, -1);
	return !video;
}

/* ---- navigation ------------------------------------------------------- */

/* The sidebar: a search box, then the places in three groups under small
 * headers -- the library, what is playing, the playlists -- and Settings
 * at the foot.  A row with no page is a group header. */
typedef struct {
	const char *page;
	const char *likeos;
	const char *fallback;
	const char *label;
} NavItem;

static const NavItem nav_top_items[] = {
	{ NULL, NULL, NULL, "Library" },
	{ "home", "likeos-recent", "document-open-recent-symbolic", "Recent" },
	{ "music", "likeos-music", "folder-music-symbolic", "Music" },
	{ "videos", "likeos-videos", "folder-videos-symbolic", "Videos" },
	{ NULL, NULL, NULL, "Playback" },
	{ "now", "likeos-now-playing", "media-playback-start-symbolic",
	  "Now playing" },
	{ "queue", "likeos-play-queue", "view-list-symbolic", "Queue" },
	{ NULL, NULL, NULL, "Playlists" },
	{ "playlists", "likeos-playlist", "view-list-bullet-symbolic",
	  "All playlists" },
};

static const NavItem nav_bottom_items[] = {
	{ "settings", "likeos-settings", "emblem-system-symbolic",
	  "Settings" },
};

static void new_playlist(Pages *pg);

static GtkWidget *nav_header(Pages *pg, const NavItem *n)
{
	GtkWidget *row = gtk_list_box_row_new();
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_widget_set_can_focus(row, FALSE);
	mp_add_class(row, "mp-nav-header");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gchar *up = g_utf8_strup(n->label, -1);
	GtkWidget *l = gtk_label_new(up);
	g_free(up);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0);
	mp_add_class(l, "mp-nav-header-label");
	gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
	/* Folded, a header is a short rule between the groups. */
	GtkWidget *rule = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_valign(rule, GTK_ALIGN_CENTER);
	gtk_widget_set_no_show_all(rule, TRUE);
	gtk_box_pack_start(GTK_BOX(box), rule, TRUE, TRUE, 0);
	if (strcmp(n->label, "Playlists") == 0) {
		GtkWidget *add = mp_glyph_button("likeos-add",
						 "list-add-symbolic", 12,
						 "New playlist",
						 "mp-nav-header-button",
						 FALSE);
		g_signal_connect_swapped(add, "clicked",
					 G_CALLBACK(new_playlist), pg);
		gtk_box_pack_end(GTK_BOX(box), add, FALSE, FALSE, 0);
		g_object_set_data(G_OBJECT(row), "mp-extra", add);
	}
	gtk_container_add(GTK_CONTAINER(row), box);
	g_object_set_data(G_OBJECT(row), "mp-label", l);
	g_object_set_data(G_OBJECT(row), "mp-rule", rule);
	return row;
}

static GtkWidget *nav_row(Pages *pg, const NavItem *n)
{
	if (n->page == NULL)
		return nav_header(pg, n);
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *g = mp_glyph_new(n->likeos, n->fallback, 16);
	mp_add_class(g, "mp-nav-glyph");
	gtk_box_pack_start(GTK_BOX(box), g, FALSE, FALSE, 0);
	GtkWidget *l = gtk_label_new(n->label);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	mp_add_class(l, "mp-nav-label");
	gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
	if (strcmp(n->page, "queue") == 0) {
		/* How many entries the queue holds (window.c keeps it up to
		 * date). */
		GtkWidget *c = gtk_label_new(NULL);
		mp_add_class(c, "mp-nav-count");
		gtk_widget_set_valign(c, GTK_ALIGN_CENTER);
		gtk_widget_set_no_show_all(c, TRUE);
		gtk_box_pack_end(GTK_BOX(box), c, FALSE, FALSE, 0);
		pg->app->nav_queue_count = c;
		g_object_set_data(G_OBJECT(row), "mp-extra", c);
	}
	gtk_container_add(GTK_CONTAINER(row), box);
	g_object_set_data(G_OBJECT(row), "mp-page", (gpointer)n->page);
	g_object_set_data(G_OBJECT(row), "mp-label", l);
	gtk_widget_set_tooltip_text(row, n->label);
	mp_add_class(row, "mp-nav-item");
	return row;
}

static void on_nav_selected(GtkListBox *box, GtkListBoxRow *row, gpointer d)
{
	Pages *pg = d;
	if (row == NULL || pg->syncing_nav)
		return;
	const char *page = g_object_get_data(G_OBJECT(row), "mp-page");
	GtkWidget *other = GTK_WIDGET(box) == pg->nav_top ? pg->nav_bottom
							   : pg->nav_top;
	pg->syncing_nav = TRUE;
	gtk_list_box_unselect_all(GTK_LIST_BOX(other));
	pg->syncing_nav = FALSE;
	if (page != NULL)
		mp_pages_show(pg->app, page);
}

/* The sidebar's selection follows the page shown; pages without a row of
 * their own light up the row they belong under. */
static void sync_nav(Pages *pg, const char *page)
{
	const char *row_page = page;
	if (strcmp(page, "collection") == 0)
		row_page = "music";
	pg->syncing_nav = TRUE;
	GtkWidget *lists[] = { pg->nav_top, pg->nav_bottom };
	for (int i = 0; i < 2; i++) {
		GList *kids = gtk_container_get_children(
			GTK_CONTAINER(lists[i]));
		GtkListBoxRow *hit = NULL;
		for (GList *l = kids; l != NULL; l = l->next) {
			const char *p = g_object_get_data(G_OBJECT(l->data),
							  "mp-page");
			if (p != NULL && strcmp(p, row_page) == 0)
				hit = l->data;
		}
		g_list_free(kids);
		if (hit != NULL)
			gtk_list_box_select_row(GTK_LIST_BOX(lists[i]), hit);
		else
			gtk_list_box_unselect_all(GTK_LIST_BOX(lists[i]));
	}
	pg->syncing_nav = FALSE;
}

/* Folded, the sidebar is a rail of glyphs: no search box (a button stands
 * in for it), no names (they become tooltips), rules for the headers. */
static void set_compact(Pages *pg, gboolean compact)
{
	MpApp *app = pg->app;
	app->settings.nav_compact = compact;
	GtkStyleContext *sc = gtk_widget_get_style_context(app->nav_pane);
	if (compact)
		gtk_style_context_add_class(sc, "mp-nav-compact");
	else
		gtk_style_context_remove_class(sc, "mp-nav-compact");
	gtk_widget_set_visible(pg->search, !compact);
	gtk_widget_set_visible(pg->search_button, compact);
	/* Folded, the fold button stands alone in the middle of the rail. */
	gtk_box_set_child_packing(
		GTK_BOX(gtk_widget_get_parent(pg->nav_toggle)), pg->nav_toggle,
		compact, compact, 0, GTK_PACK_END);
	gtk_widget_set_halign(pg->nav_toggle,
			      compact ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
	GtkWidget *lists[] = { pg->nav_top, pg->nav_bottom };
	for (int i = 0; i < 2; i++) {
		GList *kids = gtk_container_get_children(
			GTK_CONTAINER(lists[i]));
		for (GList *l = kids; l != NULL; l = l->next) {
			GtkWidget *lab = g_object_get_data(G_OBJECT(l->data),
							   "mp-label");
			GtkWidget *rule = g_object_get_data(G_OBJECT(l->data),
							    "mp-rule");
			GtkWidget *extra = g_object_get_data(G_OBJECT(l->data),
							     "mp-extra");
			if (lab != NULL)
				gtk_widget_set_visible(lab, !compact);
			if (rule != NULL)
				gtk_widget_set_visible(rule, compact);
			if (extra != NULL)
				gtk_widget_set_child_visible(extra, !compact);
			if (lab != NULL && rule == NULL)
				gtk_box_set_spacing(
					GTK_BOX(gtk_widget_get_parent(lab)),
					compact ? 0 : 12);
			if (rule == NULL)
				gtk_widget_set_has_tooltip(l->data, compact);
		}
		g_list_free(kids);
	}
	gtk_widget_set_size_request(app->nav_pane, compact ? 56 : 236, -1);
}

static void on_nav_toggle(GtkButton *b, gpointer d)
{
	Pages *pg = d;
	(void)b;
	set_compact(pg, !pg->app->settings.nav_compact);
	mp_app_schedule_save(pg->app);
}

static void on_search_button(GtkButton *b, gpointer d)
{
	Pages *pg = d;
	(void)b;
	set_compact(pg, FALSE);
	gtk_widget_grab_focus(pg->search);
}

/* ---- search ----------------------------------------------------------- */

static gboolean contains_folded(GtkTreeModel *m, GtkTreeIter *it, int col,
				const char *q)
{
	gchar *s = NULL;
	gtk_tree_model_get(m, it, col, &s, -1);
	gboolean hit = FALSE;
	if (s != NULL) {
		gchar *f = g_utf8_casefold(s, -1);
		hit = strstr(f, q) != NULL;
		g_free(f);
	}
	g_free(s);
	return hit;
}

static gboolean visible_search(GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
	Pages *pg = d;
	if (pg->query == NULL || *pg->query == '\0')
		return FALSE;
	return contains_folded(m, it, MP_LIB_TITLE, pg->query) ||
	       contains_folded(m, it, MP_LIB_ARTIST, pg->query) ||
	       contains_folded(m, it, MP_LIB_ALBUM, pg->query);
}

static void run_search(Pages *pg, const char *text)
{
	gchar *clean = g_strstrip(g_strdup(text != NULL ? text : ""));
	g_free(pg->query);
	pg->query = g_utf8_casefold(clean, -1);
	gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(pg->search_filter));
	gchar *title = g_strdup_printf("Results for “%s”", clean);
	gtk_label_set_text(GTK_LABEL(pg->search_title), title);
	g_free(title);
	guint n = (guint)gtk_tree_model_iter_n_children(pg->search_filter,
							 NULL);
	gchar *c = n == 0 ? g_strdup("Nothing in the library matches.")
			  : count_text(n, "item", "items");
	mp_page_set_subtitle(pg->search_header, c);
	g_free(c);
	g_free(clean);
	if (*pg->query != '\0')
		mp_pages_show(pg->app, "search");
}

static void on_search_activate(GtkEntry *e, gpointer d)
{
	run_search(d, gtk_entry_get_text(e));
}

static void on_search_changed(GtkSearchEntry *e, gpointer d)
{
	Pages *pg = d;
	const char *t = gtk_entry_get_text(GTK_ENTRY(e));
	/* Searching as one types, once there is something to look for. */
	if (t != NULL && g_utf8_strlen(t, -1) >= 2)
		run_search(pg, t);
}

static GtkWidget *build_nav(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mp_add_class(nav, "mp-nav");
	app->nav_pane = nav;

	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	mp_add_class(top, "mp-nav-top");
	pg->search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(pg->search),
				       "Search the library");
	mp_add_class(pg->search, "mp-nav-search");
	g_signal_connect(pg->search, "activate",
			 G_CALLBACK(on_search_activate), pg);
	g_signal_connect(pg->search, "search-changed",
			 G_CALLBACK(on_search_changed), pg);
	gtk_box_pack_start(GTK_BOX(top), pg->search, TRUE, TRUE, 0);
	GtkWidget *toggle = mp_glyph_button("likeos-sidebar",
					    "sidebar-show-symbolic", 16,
					    "Fold or unfold the sidebar",
					    "mp-nav-button", FALSE);
	g_signal_connect(toggle, "clicked", G_CALLBACK(on_nav_toggle), pg);
	gtk_box_pack_end(GTK_BOX(top), toggle, FALSE, FALSE, 0);
	pg->nav_toggle = toggle;
	gtk_box_pack_start(GTK_BOX(nav), top, FALSE, FALSE, 0);
	pg->search_button = mp_glyph_button("likeos-search",
					    "edit-find-symbolic", 16, "Search",
					    "mp-nav-button", FALSE);
	gtk_widget_set_halign(pg->search_button, GTK_ALIGN_CENTER);
	g_signal_connect(pg->search_button, "clicked",
			 G_CALLBACK(on_search_button), pg);
	gtk_box_pack_start(GTK_BOX(nav), pg->search_button, FALSE, FALSE, 0);

	pg->nav_top = gtk_list_box_new();
	mp_add_class(pg->nav_top, "mp-nav-list");
	for (gsize i = 0; i < G_N_ELEMENTS(nav_top_items); i++)
		gtk_container_add(GTK_CONTAINER(pg->nav_top),
				  nav_row(pg, &nav_top_items[i]));
	g_signal_connect(pg->nav_top, "row-selected",
			 G_CALLBACK(on_nav_selected), pg);
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_container_add(GTK_CONTAINER(sw), pg->nav_top);
	gtk_box_pack_start(GTK_BOX(nav), sw, TRUE, TRUE, 0);

	pg->nav_bottom = gtk_list_box_new();
	mp_add_class(pg->nav_bottom, "mp-nav-list");
	mp_add_class(pg->nav_bottom, "mp-nav-foot");
	for (gsize i = 0; i < G_N_ELEMENTS(nav_bottom_items); i++)
		gtk_container_add(GTK_CONTAINER(pg->nav_bottom),
				  nav_row(pg, &nav_bottom_items[i]));
	g_signal_connect(pg->nav_bottom, "row-selected",
			 G_CALLBACK(on_nav_selected), pg);
	gtk_box_pack_end(GTK_BOX(nav), pg->nav_bottom, FALSE, FALSE, 0);
	return nav;
}

/* ---- tiles ------------------------------------------------------------ */

static GdkPixbuf *album_art(MpTile *t, gpointer d)
{
	MpApp *app = d;
	return mp_library_album_cover(app->library,
				      mp_library_album(app->library, t->key));
}

static GdkPixbuf *artist_art(MpTile *t, gpointer d)
{
	MpApp *app = d;
	MpArtist *ar = mp_library_artist(app->library, t->key);
	if (ar == NULL)
		return NULL;
	return mp_library_album_cover(app->library,
				      mp_library_album(app->library,
						       ar->album));
}

static GdkPixbuf *uri_art(MpTile *t, gpointer d)
{
	MpApp *app = d;
	return mp_library_picture_for(app->library, t->key);
}

static void open_collection(MpApp *app, const char *album,
			    const char *artist);

/* All songs of an album or an artist, in album order. */
static GPtrArray *collection_uris(MpApp *app, const char *album,
				  const char *artist)
{
	GtkTreeModel *m = GTK_TREE_MODEL(mp_library_store(app->library));
	GtkTreeIter it;
	GPtrArray *rows = g_ptr_array_new();
	for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
	     ok = gtk_tree_model_iter_next(m, &it)) {
		gboolean video;
		gchar *g, *ak;
		gtk_tree_model_get(m, &it, MP_LIB_VIDEO, &video, MP_LIB_GROUP,
				   &g, MP_LIB_ARTIST_KEY, &ak, -1);
		if (!video && ((album != NULL && strcmp(g, album) == 0) ||
			       (artist != NULL && strcmp(ak, artist) == 0)))
			g_ptr_array_add(rows, gtk_tree_iter_copy(&it));
		g_free(g);
		g_free(ak);
	}
	/* Insertion sort by album and track: albums are short. */
	for (guint i = 1; i < rows->len; i++) {
		for (guint j = i; j > 0; j--) {
			GtkTreeIter *a = g_ptr_array_index(rows, j - 1);
			GtkTreeIter *b = g_ptr_array_index(rows, j);
			if (sort_collection(m, a, b, NULL) <= 0)
				break;
			rows->pdata[j - 1] = b;
			rows->pdata[j] = a;
		}
	}
	GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < rows->len; i++) {
		gchar *uri = NULL;
		gtk_tree_model_get(m, g_ptr_array_index(rows, i), MP_LIB_URI,
				   &uri, -1);
		g_ptr_array_add(uris, uri);
		gtk_tree_iter_free(g_ptr_array_index(rows, i));
	}
	g_ptr_array_free(rows, TRUE);
	return uris;
}

static void album_tile_action(MpTile *t, gboolean play, gpointer d)
{
	MpApp *app = d;
	if (!play) {
		open_collection(app, t->key, NULL);
		return;
	}
	GPtrArray *uris = collection_uris(app, t->key, NULL);
	if (uris->len > 0)
		mp_app_play_uris(app, uris, 0, FALSE);
	g_ptr_array_free(uris, TRUE);
}

static void artist_tile_action(MpTile *t, gboolean play, gpointer d)
{
	MpApp *app = d;
	if (!play) {
		open_collection(app, NULL, t->key);
		return;
	}
	GPtrArray *uris = collection_uris(app, NULL, t->key);
	if (uris->len > 0)
		mp_app_play_uris(app, uris, 0, FALSE);
	g_ptr_array_free(uris, TRUE);
}

/* A video tile, or a recent one: plays that one entry (a video), or the
 * entry within its album (music from the library). */
static void uri_tile_action(MpTile *t, gboolean play, gpointer d)
{
	MpApp *app = d;
	(void)play;
	GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(uris, g_strdup(t->key));
	mp_app_play_uris(app, uris, 0, FALSE);
	g_ptr_array_free(uris, TRUE);
}

static GtkWidget *album_tile(MpApp *app, MpAlbum *a, int width)
{
	return mp_tile_new(MP_TILE_SQUARE, width, a->key, a->title, a->artist,
			   "likeos-music", "audio-x-generic-symbolic",
			   album_art, album_tile_action, app);
}

/* ---- home ------------------------------------------------------------- */

static gint cmp_added(gconstpointer pa, gconstpointer pb)
{
	const MpAlbum *a = *(MpAlbum *const *)pa, *b = *(MpAlbum *const *)pb;
	return a->added > b->added ? -1 : a->added < b->added ? 1 : 0;
}

static void refresh_home(Pages *pg)
{
	MpApp *app = pg->app;
	mp_tile_grid_clear(pg->recent_grid);
	GPtrArray *rec = mp_library_recent(app->library);
	for (guint i = 0; i < rec->len && i < RECENT_SHOWN; i++) {
		MpRecent *r = g_ptr_array_index(rec, i);
		GtkWidget *t = mp_tile_new(
			MP_TILE_SQUARE, TILE_ALBUM, r->uri, r->title,
			r->artist && *r->artist ? r->artist
			: r->video		? "Video"
						: "",
			r->video ? "likeos-videos" : "likeos-music",
			r->video ? "video-x-generic-symbolic"
				 : "audio-x-generic-symbolic",
			uri_art, uri_tile_action, app);
		gtk_container_add(GTK_CONTAINER(pg->recent_grid), t);
	}
	gtk_widget_set_visible(pg->recent_grid, rec->len > 0);
	gtk_widget_set_visible(pg->recent_empty, rec->len == 0);
	gtk_widget_set_visible(pg->recent_clear, rec->len > 0);

	mp_tile_grid_clear(pg->added_grid);
	GPtrArray *albums = g_ptr_array_copy(mp_library_albums(app->library),
					     NULL, NULL);
	g_ptr_array_sort(albums, cmp_added);
	for (guint i = 0; i < albums->len && i < RECENT_SHOWN; i++)
		gtk_container_add(GTK_CONTAINER(pg->added_grid),
				  album_tile(app, g_ptr_array_index(albums, i),
					     TILE_ALBUM));
	gtk_widget_set_visible(pg->added_title, albums->len > 0);
	gtk_widget_set_visible(pg->added_grid, albums->len > 0);
	g_ptr_array_free(albums, TRUE);
}

static void clear_recent(Pages *pg)
{
	mp_library_clear_recent(pg->app->library);
	refresh_home(pg);
}

static GtkWidget *build_home(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *box = page_box();
	GtkWidget *actions = NULL;
	gtk_box_pack_start(GTK_BOX(box), mp_page_header("Recent", &actions),
			   FALSE, FALSE, 0);
	GtkWidget *b = mp_text_button("likeos-open", "document-open-symbolic",
				      "Open files", FALSE);
	g_signal_connect_swapped(b, "clicked",
				 G_CALLBACK(mp_dialog_open_files), app);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);
	b = mp_text_button("likeos-open-folder", "folder-open-symbolic",
			   "Open folder", FALSE);
	g_signal_connect_swapped(b, "clicked",
				 G_CALLBACK(mp_dialog_open_folder), app);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);
	b = mp_text_button("likeos-network", "insert-link-symbolic",
			   "Open address", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(mp_dialog_open_url),
				 app);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);

	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mp_add_class(inner, "mp-page-body");
	GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start(GTK_BOX(hrow), mp_section_label("Played lately"),
			   FALSE, FALSE, 0);
	GtkWidget *clear = gtk_button_new_with_label("Clear");
	pg->recent_clear = clear;
	gtk_widget_set_no_show_all(clear, TRUE);
	mp_add_class(clear, "mp-link-button");
	gtk_widget_set_tooltip_text(clear, "Clear the list of recent media");
	gtk_widget_set_focus_on_click(clear, FALSE);
	gtk_widget_set_valign(clear, GTK_ALIGN_END);
	g_signal_connect_swapped(clear, "clicked", G_CALLBACK(clear_recent),
				 pg);
	gtk_box_pack_end(GTK_BOX(hrow), clear, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(inner), hrow, FALSE, FALSE, 0);
	pg->recent_grid = mp_tile_grid_new();
	gtk_box_pack_start(GTK_BOX(inner), pg->recent_grid, FALSE, FALSE, 0);
	pg->recent_empty = gtk_label_new(
		"The music and videos you play will show up here.");
	gtk_label_set_xalign(GTK_LABEL(pg->recent_empty), 0.0);
	mp_add_class(pg->recent_empty, "mp-hint");
	gtk_widget_set_no_show_all(pg->recent_empty, TRUE);
	gtk_box_pack_start(GTK_BOX(inner), pg->recent_empty, FALSE, FALSE, 0);

	pg->added_title = mp_section_label("New in the library");
	gtk_widget_set_no_show_all(pg->added_title, TRUE);
	gtk_box_pack_start(GTK_BOX(inner), pg->added_title, FALSE, FALSE, 0);
	pg->added_grid = mp_tile_grid_new();
	gtk_widget_set_no_show_all(pg->added_grid, TRUE);
	gtk_box_pack_start(GTK_BOX(inner), pg->added_grid, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(box), scrolled(inner), TRUE, TRUE, 0);
	return box;
}

/* ---- music ------------------------------------------------------------ */

static void apply_sort(Pages *pg)
{
	gchar *t = g_strdup_printf("Sort: %s", sort_names[pg->sort]);
	gtk_label_set_text(GTK_LABEL(pg->sort_label), t);
	g_free(t);
	/* Setting the function again sorts again. */
	gtk_tree_sortable_set_default_sort_func(
		GTK_TREE_SORTABLE(pg->songs_sort), sort_songs, pg, NULL);
	refresh_library_views(pg);
}

static void on_sort_item(GtkCheckMenuItem *item, gpointer d)
{
	Pages *pg = d;
	if (!gtk_check_menu_item_get_active(item))
		return;
	pg->sort = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item),
						     "mp-sort"));
	apply_sort(pg);
}

static GtkWidget *sort_button(Pages *pg)
{
	GtkWidget *mb = gtk_menu_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	pg->sort_label = gtk_label_new(NULL);
	gtk_box_pack_start(GTK_BOX(box), pg->sort_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box),
			   mp_glyph_new("likeos-chevron-down",
					"pan-down-symbolic", 16),
			   FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(mb), box);
	mp_add_class(mb, "mp-subtle-button");
	gtk_widget_set_focus_on_click(mb, FALSE);
	gtk_widget_set_valign(mb, GTK_ALIGN_CENTER);
	GtkWidget *menu = gtk_menu_new();
	GSList *group = NULL;
	for (int i = 0; i < N_SORTS; i++) {
		GtkWidget *it = gtk_radio_menu_item_new_with_label(
			group, sort_names[i]);
		group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
		g_object_set_data(G_OBJECT(it), "mp-sort", GINT_TO_POINTER(i));
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it),
					       i == pg->sort);
		g_signal_connect(it, "toggled", G_CALLBACK(on_sort_item), pg);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
	}
	gtk_widget_show_all(menu);
	gtk_menu_button_set_popup(GTK_MENU_BUTTON(mb), menu);
	return mb;
}

static const char *const view_pages[3] = { "songs", "albums", "artists" };

static void on_view_switch(GtkToggleButton *b, gpointer d)
{
	Pages *pg = d;
	if (!gtk_toggle_button_get_active(b))
		return;
	for (int i = 0; i < 3; i++) {
		if (pg->views[i] == GTK_WIDGET(b))
			gtk_stack_set_visible_child_name(
				GTK_STACK(pg->music_stack), view_pages[i]);
	}
}

static void shuffle_music(MpApp *app)
{
	Pages *pg = pages_of(app);
	GPtrArray *uris = model_uris(pg->songs_sort);
	if (uris->len > 0)
		mp_app_play_uris(app, uris, 0, TRUE);
	g_ptr_array_free(uris, TRUE);
}

static GtkWidget *build_music(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *box = page_box();
	GtkWidget *actions = NULL;
	pg->music_header = mp_page_header("Music", &actions);
	gtk_box_pack_start(GTK_BOX(box), pg->music_header, FALSE, FALSE, 0);
	GtkWidget *b = mp_text_button("likeos-shuffle",
				      "media-playlist-shuffle-symbolic",
				      "Shuffle all", TRUE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(shuffle_music), app);
	gtk_box_pack_end(GTK_BOX(actions), b, FALSE, FALSE, 0);
	b = mp_text_button("likeos-folder-add", "folder-new-symbolic",
			   "Add folder", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(add_music_folder),
				 app);
	gtk_box_pack_end(GTK_BOX(actions), b, FALSE, FALSE, 0);

	/* The view bar: a segmented switch between Songs, Albums and
	 * Artists, and the sort order at the right. */
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	mp_add_class(bar, "mp-view-bar");
	GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	mp_add_class(seg, "mp-segmented");
	gtk_widget_set_valign(seg, GTK_ALIGN_CENTER);
	static const char *const labels[3] = { "Songs", "Albums", "Artists" };
	GSList *group = NULL;
	for (int i = 0; i < 3; i++) {
		pg->views[i] = gtk_radio_button_new_with_label(group, labels[i]);
		group = gtk_radio_button_get_group(
			GTK_RADIO_BUTTON(pg->views[i]));
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(pg->views[i]),
					   FALSE);
		gtk_widget_set_focus_on_click(pg->views[i], FALSE);
		gtk_widget_set_can_focus(pg->views[i], FALSE);
		mp_add_class(pg->views[i], "mp-segment");
		g_signal_connect(pg->views[i], "toggled", G_CALLBACK(on_view_switch),
				 pg);
		gtk_box_pack_start(GTK_BOX(seg), pg->views[i], FALSE, FALSE,
				   0);
	}
	gtk_box_pack_start(GTK_BOX(bar), seg, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(bar), sort_button(pg), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

	pg->songs_filter = gtk_tree_model_filter_new(
		GTK_TREE_MODEL(mp_library_store(app->library)), NULL);
	gtk_tree_model_filter_set_visible_func(
		GTK_TREE_MODEL_FILTER(pg->songs_filter), visible_audio, NULL,
		NULL);
	pg->songs_sort = gtk_tree_model_sort_new_with_model(pg->songs_filter);
	gtk_tree_sortable_set_default_sort_func(
		GTK_TREE_SORTABLE(pg->songs_sort), sort_songs, pg, NULL);
	pg->songs_view = song_list(app, pg->songs_sort, FALSE);
	connect_song_list(pg->songs_view, app);

	pg->music_stack = gtk_stack_new();
	GtkWidget *songs_sw = scrolled(pg->songs_view);
	mp_add_class(songs_sw, "mp-list-scroller");
	gtk_stack_add_named(GTK_STACK(pg->music_stack), songs_sw, "songs");
	pg->albums_grid = mp_tile_grid_new();
	GtkWidget *sw = scrolled(pg->albums_grid);
	mp_add_class(sw, "mp-grid-scroller");
	gtk_stack_add_named(GTK_STACK(pg->music_stack), sw, "albums");
	pg->artists_grid = mp_tile_grid_new();
	sw = scrolled(pg->artists_grid);
	mp_add_class(sw, "mp-grid-scroller");
	gtk_stack_add_named(GTK_STACK(pg->music_stack), sw, "artists");

	GtkWidget *add = mp_text_button("likeos-folder-add",
					"folder-new-symbolic", "Add folder",
					TRUE);
	g_signal_connect_swapped(add, "clicked", G_CALLBACK(add_music_folder),
				 app);
	pg->music_outer = gtk_stack_new();
	gtk_stack_add_named(
		GTK_STACK(pg->music_outer),
		empty_state("likeos-music", "folder-music-symbolic",
			    "Your music library is empty",
			    "Music in the folders of your library shows up "
			    "here. Add the folder your music is in.",
			    add),
		"empty");
	gtk_stack_add_named(GTK_STACK(pg->music_outer), pg->music_stack,
			    "library");
	gtk_box_pack_start(GTK_BOX(box), pg->music_outer, TRUE, TRUE, 0);
	return box;
}

/* ---- videos ----------------------------------------------------------- */

static GtkWidget *build_videos(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *box = page_box();
	GtkWidget *actions = NULL;
	pg->videos_header = mp_page_header("Videos", &actions);
	gtk_box_pack_start(GTK_BOX(box), pg->videos_header, FALSE, FALSE, 0);
	GtkWidget *b = mp_text_button("likeos-folder-add",
				      "folder-new-symbolic", "Add folder",
				      FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(add_video_folder),
				 app);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);

	pg->videos_grid = mp_tile_grid_new();
	GtkWidget *sw = scrolled(pg->videos_grid);
	mp_add_class(sw, "mp-grid-scroller");
	GtkWidget *add = mp_text_button("likeos-folder-add",
					"folder-new-symbolic", "Add folder",
					TRUE);
	g_signal_connect_swapped(add, "clicked", G_CALLBACK(add_video_folder),
				 app);
	pg->videos_outer = gtk_stack_new();
	gtk_stack_add_named(
		GTK_STACK(pg->videos_outer),
		empty_state("likeos-videos", "folder-videos-symbolic",
			    "Your video library is empty",
			    "Videos in the folders of your library show up "
			    "here. Add the folder your videos are in.",
			    add),
		"empty");
	gtk_stack_add_named(GTK_STACK(pg->videos_outer), sw, "library");
	gtk_box_pack_start(GTK_BOX(box), pg->videos_outer, TRUE, TRUE, 0);
	return box;
}

/* ---- collection: one album or one artist ------------------------------ */

static gboolean visible_collection(GtkTreeModel *m, GtkTreeIter *it,
				   gpointer d)
{
	Pages *pg = d;
	gboolean video = FALSE;
	gchar *g = NULL, *ak = NULL;
	gtk_tree_model_get(m, it, MP_LIB_VIDEO, &video, MP_LIB_GROUP, &g,
			   MP_LIB_ARTIST_KEY, &ak, -1);
	gboolean vis = !video &&
		       ((pg->coll_album != NULL && g_strcmp0(g, pg->coll_album) == 0) ||
			(pg->coll_artist != NULL &&
			 g_strcmp0(ak, pg->coll_artist) == 0));
	g_free(g);
	g_free(ak);
	return vis;
}

static gboolean coll_art_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
	Pages *pg = d;
	MpApp *app = pg->app;
	int side = gtk_widget_get_allocated_width(w);
	GdkPixbuf *pb = NULL;
	if (pg->coll_album != NULL) {
		pb = mp_library_album_cover(
			app->library,
			mp_library_album(app->library, pg->coll_album));
	} else {
		MpArtist *ar = mp_library_artist(app->library, pg->coll_artist);
		if (ar != NULL)
			pb = mp_library_album_cover(
				app->library,
				mp_library_album(app->library, ar->album));
	}
	double radius = pg->coll_artist != NULL ? side / 2.0 : 8;
	if (pb != NULL) {
		mp_draw_picture(cr, pb, 0, 0, side, side, radius);
		return FALSE;
	}
	GtkStyleContext *ctx = gtk_widget_get_style_context(w);
	gtk_render_background(ctx, cr, 0, 0, side, side);
	return FALSE;
}

static void update_collection_header(Pages *pg)
{
	MpApp *app = pg->app;
	guint n = (guint)gtk_tree_model_iter_n_children(pg->coll_sort, NULL);
	gint64 total = 0;
	GtkTreeIter it;
	for (gboolean ok = gtk_tree_model_get_iter_first(pg->coll_sort, &it);
	     ok; ok = gtk_tree_model_iter_next(pg->coll_sort, &it)) {
		gint64 d = -1;
		gtk_tree_model_get(pg->coll_sort, &it, MP_LIB_DURATION, &d, -1);
		if (d > 0)
			total += d;
	}
	gchar *songs = count_text(n, "song", "songs");
	gchar *mins = minutes_text(total);
	gchar *sub;
	if (pg->coll_album != NULL) {
		MpAlbum *a = mp_library_album(app->library, pg->coll_album);
		gtk_label_set_text(GTK_LABEL(pg->coll_kind), "ALBUM");
		gtk_label_set_text(GTK_LABEL(pg->coll_title),
				   a ? a->title : "Album");
		sub = g_strdup_printf("%s · %s · %s",
				      a ? a->artist : "", songs, mins);
	} else {
		MpArtist *ar = mp_library_artist(app->library, pg->coll_artist);
		gtk_label_set_text(GTK_LABEL(pg->coll_kind), "ARTIST");
		gtk_label_set_text(GTK_LABEL(pg->coll_title),
				   ar ? ar->name : "Artist");
		sub = g_strdup_printf("%s · %s", songs, mins);
	}
	gtk_label_set_text(GTK_LABEL(pg->coll_sub), sub);
	GtkStyleContext *sc = gtk_widget_get_style_context(pg->coll_art);
	if (pg->coll_artist != NULL)
		gtk_style_context_add_class(sc, "mp-tile-art-round");
	else
		gtk_style_context_remove_class(sc, "mp-tile-art-round");
	gtk_widget_queue_draw(pg->coll_art);
	g_free(sub);
	g_free(songs);
	g_free(mins);
}

static void open_collection(MpApp *app, const char *album,
			    const char *artist)
{
	Pages *pg = pages_of(app);
	g_free(pg->coll_album);
	g_free(pg->coll_artist);
	pg->coll_album = g_strdup(album);
	pg->coll_artist = g_strdup(artist);
	gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(pg->coll_filter));
	update_collection_header(pg);
	mp_pages_show(app, "collection");
}

static void coll_play(Pages *pg, gboolean shuffle)
{
	GPtrArray *uris = model_uris(pg->coll_sort);
	if (uris->len > 0)
		mp_app_play_uris(pg->app, uris, 0, shuffle);
	g_ptr_array_free(uris, TRUE);
}

static void coll_play_all(Pages *pg)
{
	coll_play(pg, FALSE);
}

static void coll_shuffle(Pages *pg)
{
	coll_play(pg, TRUE);
}

static void coll_queue(Pages *pg)
{
	GPtrArray *uris = model_uris(pg->coll_sort);
	if (uris->len > 0)
		mp_app_queue_uris(pg->app, uris, FALSE);
	g_ptr_array_free(uris, TRUE);
}

static GtkWidget *build_collection(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *box = page_box();

	GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
	mp_add_class(head, "mp-coll-header");
	GtkWidget *back = mp_glyph_button("likeos-back", "go-previous-symbolic",
					  16, "Back", "mp-back-button", FALSE);
	gtk_widget_set_valign(back, GTK_ALIGN_START);
	g_signal_connect_swapped(back, "clicked", G_CALLBACK(mp_pages_back),
				 app);
	gtk_box_pack_start(GTK_BOX(head), back, FALSE, FALSE, 0);
	pg->coll_art = gtk_drawing_area_new();
	gtk_widget_set_size_request(pg->coll_art, 176, 176);
	mp_add_class(pg->coll_art, "mp-tile-art");
	g_signal_connect(pg->coll_art, "draw", G_CALLBACK(coll_art_draw), pg);
	gtk_box_pack_start(GTK_BOX(head), pg->coll_art, FALSE, FALSE, 0);

	GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(text, GTK_ALIGN_END);
	pg->coll_kind = gtk_label_new(NULL);
	mp_add_class(pg->coll_kind, "mp-coll-kind");
	pg->coll_title = gtk_label_new(NULL);
	mp_add_class(pg->coll_title, "mp-coll-title");
	pg->coll_sub = gtk_label_new(NULL);
	mp_add_class(pg->coll_sub, "mp-coll-sub");
	GtkWidget *ls[] = { pg->coll_kind, pg->coll_title, pg->coll_sub };
	for (int i = 0; i < 3; i++) {
		gtk_label_set_xalign(GTK_LABEL(ls[i]), 0.0);
		gtk_label_set_ellipsize(GTK_LABEL(ls[i]), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(text), ls[i], FALSE, FALSE, 0);
	}
	GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	mp_add_class(actions, "mp-page-actions");
	GtkWidget *b = mp_text_button("likeos-play",
				      "media-playback-start-symbolic",
				      "Play all", TRUE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(coll_play_all), pg);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);
	b = mp_text_button("likeos-shuffle", "media-playlist-shuffle-symbolic",
			   "Shuffle", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(coll_shuffle), pg);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);
	b = mp_text_button("likeos-add", "list-add-symbolic",
			   "Add to queue", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(coll_queue), pg);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(text), actions, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(head), text, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

	pg->coll_filter = gtk_tree_model_filter_new(
		GTK_TREE_MODEL(mp_library_store(app->library)), NULL);
	gtk_tree_model_filter_set_visible_func(
		GTK_TREE_MODEL_FILTER(pg->coll_filter), visible_collection, pg,
		NULL);
	pg->coll_sort = gtk_tree_model_sort_new_with_model(pg->coll_filter);
	gtk_tree_sortable_set_default_sort_func(
		GTK_TREE_SORTABLE(pg->coll_sort), sort_collection, NULL, NULL);
	pg->coll_view = song_list(app, pg->coll_sort, TRUE);
	connect_song_list(pg->coll_view, app);
	GtkWidget *sw = scrolled(pg->coll_view);
	mp_add_class(sw, "mp-list-scroller");
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	return box;
}

/* ---- search results --------------------------------------------------- */

static GtkWidget *build_search(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *box = page_box();
	GtkWidget *actions = NULL;
	GtkWidget *head = mp_page_header("Results", &actions);
	pg->search_title = g_object_get_data(G_OBJECT(head), "mp-title");
	pg->search_header = head;
	gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);

	pg->search_filter = gtk_tree_model_filter_new(
		GTK_TREE_MODEL(mp_library_store(app->library)), NULL);
	gtk_tree_model_filter_set_visible_func(
		GTK_TREE_MODEL_FILTER(pg->search_filter), visible_search, pg,
		NULL);
	pg->search_view = song_list(app, pg->search_filter, FALSE);
	connect_song_list(pg->search_view, app);
	GtkWidget *sw = scrolled(pg->search_view);
	mp_add_class(sw, "mp-list-scroller");
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	return box;
}

/* ---- playlists -------------------------------------------------------- */

static gchar *playlists_dir(void)
{
	return g_build_filename(g_get_user_data_dir(), "mediaplayer",
				"playlists", NULL);
}

static void playlist_tile_action(MpTile *t, gboolean play, gpointer d)
{
	(void)play;
	mp_app_play_playlist_file(d, t->key, FALSE);
}

static void pl_menu_play(Pages *pg)
{
	if (pg->pl_menu_path != NULL)
		mp_app_play_playlist_file(pg->app, pg->pl_menu_path, FALSE);
}

static void pl_menu_add(Pages *pg)
{
	if (pg->pl_menu_path != NULL)
		mp_app_play_playlist_file(pg->app, pg->pl_menu_path, TRUE);
}

static void pl_menu_delete(Pages *pg)
{
	if (pg->pl_menu_path == NULL)
		return;
	gchar *name = g_path_get_basename(pg->pl_menu_path);
	GtkWidget *d = gtk_message_dialog_new(
		GTK_WINDOW(pg->app->window), GTK_DIALOG_MODAL,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
		"Delete the playlist “%s”?", name);
	gtk_message_dialog_format_secondary_text(
		GTK_MESSAGE_DIALOG(d),
		"The music and videos in it are not deleted.");
	gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL,
			       "_Delete", GTK_RESPONSE_ACCEPT, NULL);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT &&
	    g_unlink(pg->pl_menu_path) != 0)
		mp_dialog_message(GTK_WINDOW(pg->app->window),
				  GTK_MESSAGE_ERROR,
				  "The playlist could not be deleted",
				  g_strerror(errno));
	gtk_widget_destroy(d);
	g_free(name);
	refresh_playlists(pg);
}

/* A right click on a playlist tile.  The press may come from the tile's
 * picture or from the grid itself (under a label), so the tile is found
 * from where the pointer is over the grid. */
static gboolean on_pl_grid_button(GtkWidget *w, GdkEventButton *ev,
				  gpointer d)
{
	Pages *pg = d;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;
	int x = 0, y = 0;
	GtkAllocation a;
	gtk_widget_get_allocation(w, &a);
	gdk_window_get_device_position(gtk_widget_get_window(w), ev->device,
				       &x, &y, NULL);
	if (!gtk_widget_get_has_window(w)) {
		x -= a.x;
		y -= a.y;
	}
	GtkFlowBoxChild *child =
		gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(w), x, y);
	MpTile *t = child != NULL ? mp_tile_of(GTK_WIDGET(child)) : NULL;
	if (t == NULL)
		return FALSE;
	g_free(pg->pl_menu_path);
	pg->pl_menu_path = g_strdup(t->key);
	gtk_menu_popup_at_pointer(GTK_MENU(pg->pl_menu), (GdkEvent *)ev);
	return TRUE;
}

static gint cmp_names(gconstpointer a, gconstpointer b)
{
	return mp_compare_filenames(*(const char *const *)a,
				    *(const char *const *)b);
}

static void refresh_playlists(Pages *pg)
{
	MpApp *app = pg->app;
	mp_tile_grid_clear(pg->pl_grid);
	gchar *dir = playlists_dir();
	GDir *d = g_dir_open(dir, 0, NULL);
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	const char *n;
	while (d != NULL && (n = g_dir_read_name(d)) != NULL) {
		if (mp_is_playlist_name(n))
			g_ptr_array_add(names, g_strdup(n));
	}
	if (d != NULL)
		g_dir_close(d);
	g_ptr_array_sort(names, cmp_names);
	for (guint i = 0; i < names->len; i++) {
		const char *name = g_ptr_array_index(names, i);
		gchar *path = g_build_filename(dir, name, NULL);
		GPtrArray *uris = NULL, *titles = NULL;
		gchar *sub = NULL;
		if (mp_playlist_file_load(path, &uris, &titles, NULL)) {
			sub = count_text(uris->len, "item", "items");
			g_ptr_array_free(uris, TRUE);
			g_ptr_array_free(titles, TRUE);
		}
		gchar *title = g_strdup(name);
		char *dot = strrchr(title, '.');
		if (dot != NULL && dot != title)
			*dot = '\0';
		gchar *disp = g_filename_display_name(title);
		GtkWidget *t = mp_tile_new(MP_TILE_SQUARE, TILE_ALBUM, path,
					   disp, sub, "likeos-playlist",
					   "view-list-bullet-symbolic", NULL,
					   playlist_tile_action, app);
		gtk_container_add(GTK_CONTAINER(pg->pl_grid), t);
		g_free(disp);
		g_free(title);
		g_free(sub);
		g_free(path);
	}
	gtk_stack_set_visible_child_name(GTK_STACK(pg->pl_outer),
					 names->len > 0 ? "list" : "empty");
	g_ptr_array_free(names, TRUE);
	g_free(dir);
}

/* "New playlist": what is in the play queue now, under a name. */
static void new_playlist(Pages *pg)
{
	MpApp *app = pg->app;
	if (mp_playlist_count(app->playlist) == 0) {
		mp_dialog_message(GTK_WINDOW(app->window), GTK_MESSAGE_INFO,
				  "The queue is empty",
				  "A new playlist is made of what is in the "
				  "queue. Add music or videos to it "
				  "first.");
		return;
	}
	GtkWidget *d = gtk_dialog_new_with_buttons(
		"New Playlist", GTK_WINDOW(app->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel",
		GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(area), 16);
	gtk_box_set_spacing(GTK_BOX(area), 8);
	GtkWidget *l = gtk_label_new("Name the playlist. It holds what is in "
				     "the queue now.");
	gtk_label_set_xalign(GTK_LABEL(l), 0.0);
	gtk_box_pack_start(GTK_BOX(area), l, FALSE, FALSE, 0);
	GtkWidget *e = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(e), "Untitled playlist");
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	gtk_box_pack_start(GTK_BOX(area), e, FALSE, FALSE, 0);
	gtk_widget_show_all(d);

	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
		gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(
			GTK_ENTRY(e))));
		if (*name == '\0' || strchr(name, '/') != NULL ||
		    name[0] == '.') {
			mp_dialog_message(GTK_WINDOW(d), GTK_MESSAGE_WARNING,
					  "That name cannot be used",
					  "A name must not be empty, start "
					  "with a dot or contain “/”.");
			g_free(name);
			continue;
		}
		gchar *dir = playlists_dir();
		gchar *file = g_strconcat(name, ".m3u8", NULL);
		gchar *path = g_build_filename(dir, file, NULL);
		GError *err = NULL;
		gboolean ok = g_mkdir_with_parents(dir, 0700) == 0 &&
			      mp_playlist_file_save(app->playlist, path, &err);
		if (!ok)
			mp_dialog_message(GTK_WINDOW(d), GTK_MESSAGE_ERROR,
					  "The playlist could not be saved",
					  err ? err->message
					      : g_strerror(errno));
		g_clear_error(&err);
		g_free(path);
		g_free(file);
		g_free(dir);
		g_free(name);
		if (ok)
			break;
	}
	gtk_widget_destroy(d);
	refresh_playlists(pg);
}

static GtkWidget *build_playlists(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *box = page_box();
	GtkWidget *actions = NULL;
	gtk_box_pack_start(GTK_BOX(box), mp_page_header("Playlists", &actions),
			   FALSE, FALSE, 0);
	GtkWidget *b = mp_text_button("likeos-new", "list-add-symbolic",
				      "New playlist", TRUE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(new_playlist), pg);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);
	b = mp_text_button("likeos-open", "document-open-symbolic",
			   "Open playlist file", FALSE);
	g_signal_connect_swapped(b, "clicked",
				 G_CALLBACK(mp_dialog_open_playlist), app);
	gtk_box_pack_start(GTK_BOX(actions), b, FALSE, FALSE, 0);

	pg->pl_grid = mp_tile_grid_new();
	gtk_widget_add_events(pg->pl_grid, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(pg->pl_grid, "button-press-event",
			 G_CALLBACK(on_pl_grid_button), pg);
	GtkWidget *sw = scrolled(pg->pl_grid);
	mp_add_class(sw, "mp-grid-scroller");
	pg->pl_outer = gtk_stack_new();
	gtk_stack_add_named(
		GTK_STACK(pg->pl_outer),
		empty_state("likeos-playlist", "view-list-bullet-symbolic",
			    "No playlists yet",
			    "Put together a queue, then make it a "
			    "playlist with “New playlist”.",
			    NULL),
		"empty");
	gtk_stack_add_named(GTK_STACK(pg->pl_outer), sw, "list");
	gtk_box_pack_start(GTK_BOX(box), pg->pl_outer, TRUE, TRUE, 0);

	pg->pl_menu = gtk_menu_new();
	struct {
		const char *label;
		void (*fn)(Pages *);
	} items[] = {
		{ "_Play", pl_menu_play },
		{ "_Add to queue", pl_menu_add },
		{ "_Delete", pl_menu_delete },
	};
	for (gsize i = 0; i < G_N_ELEMENTS(items); i++) {
		GtkWidget *it = gtk_menu_item_new_with_mnemonic(items[i].label);
		g_signal_connect_swapped(it, "activate", G_CALLBACK(items[i].fn),
					 pg);
		gtk_menu_shell_append(GTK_MENU_SHELL(pg->pl_menu), it);
	}
	gtk_widget_show_all(pg->pl_menu);
	gtk_menu_attach_to_widget(GTK_MENU(pg->pl_menu), box, NULL);
	return box;
}

/* ---- settings --------------------------------------------------------- */

typedef struct {
	MpApp *app;
	gboolean video;
	gchar *dir;
} DirRemove;

static void dir_remove_free(gpointer p, GClosure *c)
{
	DirRemove *dr = p;
	(void)c;
	g_free(dr->dir);
	g_free(dr);
}

static void on_dir_remove(GtkButton *b, gpointer d)
{
	DirRemove *dr = d;
	(void)b;
	MpApp *app = dr->app;
	gchar ***dirs = dr->video ? &app->settings.video_dirs
				  : &app->settings.music_dirs;
	GPtrArray *keep = g_ptr_array_new();
	for (gchar **p = *dirs; p != NULL && *p != NULL; p++) {
		if (strcmp(*p, dr->dir) != 0)
			g_ptr_array_add(keep, g_strdup(*p));
	}
	g_ptr_array_add(keep, NULL);
	g_strfreev(*dirs);
	*dirs = (gchar **)g_ptr_array_free(keep, FALSE);
	mp_app_schedule_save(app);
	mp_pages_rescan(app, FALSE);
	/* The button that was clicked goes with its row; rebuild after the
	 * signal has finished. */
	g_idle_add(refresh_dirs_idle, pages_of(app));
}

static gboolean refresh_dirs_idle(gpointer d)
{
	refresh_settings_dirs(d);
	return G_SOURCE_REMOVE;
}

static void fill_dirs(Pages *pg, GtkWidget *box, gboolean video)
{
	MpApp *app = pg->app;
	GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
	for (GList *l = kids; l != NULL; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);
	gchar **dirs = video ? app->settings.video_dirs
			     : app->settings.music_dirs;
	int n = 0;
	for (gchar **p = dirs; p != NULL && *p != NULL; p++, n++) {
		GtkWidget *rm = mp_glyph_button("likeos-close",
						"window-close-symbolic", 16,
						"Remove this folder from the "
						"library",
						"mp-subtle-button", FALSE);
		DirRemove *dr = g_new0(DirRemove, 1);
		dr->app = app;
		dr->video = video;
		dr->dir = g_strdup(*p);
		g_signal_connect_data(rm, "clicked", G_CALLBACK(on_dir_remove),
				      dr, dir_remove_free, 0);
		gchar *disp = g_filename_display_name(*p);
		const char *note = g_file_test(*p, G_FILE_TEST_IS_DIR)
					   ? NULL
					   : "This folder does not exist "
					     "(yet).";
		GtkWidget *row = mp_setting_row("likeos-folder", "folder-symbolic",
						disp, note, rm);
		mp_add_class(row, "mp-setting-subrow");
		/* A long path keeps its start and its last folder. */
		gtk_label_set_ellipsize(
			GTK_LABEL(g_object_get_data(G_OBJECT(row), "mp-title")),
			PANGO_ELLIPSIZE_MIDDLE);
		gtk_widget_set_tooltip_text(row, disp);
		gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
		g_free(disp);
	}
	if (n == 0) {
		GtkWidget *l = gtk_label_new("No folders: this library is "
					     "empty.");
		gtk_label_set_xalign(GTK_LABEL(l), 0.0);
		mp_add_class(l, "mp-setting-subrow");
		mp_add_class(l, "mp-hint");
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	}
	gtk_widget_show_all(box);
}

static void refresh_settings_dirs(Pages *pg)
{
	fill_dirs(pg, pg->music_dirs_box, FALSE);
	fill_dirs(pg, pg->video_dirs_box, TRUE);
}

static GtkWidget *card(void)
{
	GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mp_add_class(c, "mp-setting-card");
	return c;
}

static GtkWidget *switch_for(gboolean on)
{
	GtkWidget *s = gtk_switch_new();
	gtk_switch_set_active(GTK_SWITCH(s), on);
	gtk_widget_set_focus_on_click(s, FALSE);
	return s;
}

static void on_gapless(GObject *o, GParamSpec *ps, gpointer d)
{
	MpApp *app = d;
	(void)ps;
	app->settings.gapless = gtk_switch_get_active(GTK_SWITCH(o));
	mp_player_set_gapless(app->player, app->settings.gapless);
	mp_app_schedule_save(app);
}

static void on_discover(GObject *o, GParamSpec *ps, gpointer d)
{
	MpApp *app = d;
	(void)ps;
	mp_app_set_discover(app, gtk_switch_get_active(GTK_SWITCH(o)));
}

static void on_remaining(GObject *o, GParamSpec *ps, gpointer d)
{
	MpApp *app = d;
	(void)ps;
	app->show_remaining = gtk_switch_get_active(GTK_SWITCH(o));
	app->settings.show_remaining = app->show_remaining;
	mp_app_schedule_save(app);
}

static void on_subtitles(GObject *o, GParamSpec *ps, gpointer d)
{
	MpApp *app = d;
	(void)ps;
	app->settings.subtitles = gtk_switch_get_active(GTK_SWITCH(o));
	mp_player_set_subtitles(app->player, app->settings.subtitles);
	mp_app_schedule_save(app);
}

static void on_sink(GtkComboBox *c, gpointer d)
{
	MpApp *app = d;
	const char *id = gtk_combo_box_get_active_id(c);
	if (id == NULL)
		return;
	g_free(app->settings.video_sink);
	app->settings.video_sink = g_strdup(id);
	mp_app_schedule_save(app);
}

static void refresh_library(MpApp *app)
{
	mp_pages_rescan(app, TRUE);
}

/* A row with a list of sub-rows under it (the library folders). */
static GtkWidget *row_with_list(const char *likeos, const char *fallback,
				const char *title, const char *desc,
				GtkWidget *button, GtkWidget **list_out)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mp_add_class(box, "mp-setting-group");
	gtk_box_pack_start(GTK_BOX(box),
			   mp_setting_row(likeos, fallback, title, desc, button),
			   FALSE, FALSE, 0);
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mp_add_class(list, "mp-setting-list");
	gtk_box_pack_start(GTK_BOX(box), list, FALSE, FALSE, 0);
	*list_out = list;
	return box;
}

/* Settings are grouped: one card per section, its rows divided by
 * hairlines. */
static GtkWidget *section(GtkWidget *inner, const char *title)
{
	gchar *up = g_utf8_strup(title, -1);
	gtk_box_pack_start(GTK_BOX(inner), mp_section_label(up), FALSE, FALSE,
			   0);
	g_free(up);
	GtkWidget *c = card();
	gtk_box_pack_start(GTK_BOX(inner), c, FALSE, FALSE, 0);
	return c;
}

static GtkWidget *build_settings(Pages *pg)
{
	MpApp *app = pg->app;
	GtkWidget *box = page_box();
	GtkWidget *head = mp_page_header("Settings", NULL);
	mp_page_set_subtitle(head, "Library folders, playback and video "
				   "output");
	gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
	GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	mp_add_class(inner, "mp-page-body");
	mp_add_class(inner, "mp-settings");

	GtkWidget *c = section(inner, "Library");
	GtkWidget *b = mp_text_button("likeos-add", "list-add-symbolic",
				      "Add folder", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(add_music_folder),
				 app);
	gtk_box_pack_start(GTK_BOX(c),
			   row_with_list("likeos-music", "folder-music-symbolic",
					 "Music folders",
					 "Where the music library comes from",
					 b, &pg->music_dirs_box),
			   FALSE, FALSE, 0);
	b = mp_text_button("likeos-add", "list-add-symbolic", "Add folder",
			   FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(add_video_folder),
				 app);
	gtk_box_pack_start(GTK_BOX(c),
			   row_with_list("likeos-videos", "folder-videos-symbolic",
					 "Video folders",
					 "Where the video library comes from",
					 b, &pg->video_dirs_box),
			   FALSE, FALSE, 0);
	b = mp_text_button("likeos-refresh", "view-refresh-symbolic",
			   "Refresh", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(refresh_library), app);
	GtkWidget *row = mp_setting_row("likeos-refresh", "view-refresh-symbolic",
					"Read the library again",
					"Every file's title, artist and album "
					"are read anew",
					b);
	pg->lib_status = gtk_label_new(NULL);
	mp_add_class(pg->lib_status, "mp-hint");
	gtk_box_pack_end(GTK_BOX(row), pg->lib_status, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(c), row, FALSE, FALSE, 0);

	c = section(inner, "Playback");
	struct {
		const char *likeos, *fallback, *title, *desc;
		gboolean on;
		GCallback cb;
	} sw[] = {
		{ "likeos-next", "media-skip-forward-symbolic",
		  "Gapless playback",
		  "Go from one track to the next without a pause",
		  app->settings.gapless, G_CALLBACK(on_gapless) },
		{ "likeos-info", "dialog-information-symbolic",
		  "Read titles ahead",
		  "Look up title, artist and length of queued files before "
		  "they play",
		  app->settings.discover, G_CALLBACK(on_discover) },
		{ "likeos-recent", "document-open-recent-symbolic",
		  "Show the time remaining",
		  "Count down to the end instead of showing the length",
		  app->settings.show_remaining, G_CALLBACK(on_remaining) },
		{ "likeos-closed-captions", "media-view-subtitles-symbolic",
		  "Subtitles", "Show the subtitles a video has",
		  app->settings.subtitles, G_CALLBACK(on_subtitles) },
	};
	for (gsize i = 0; i < G_N_ELEMENTS(sw); i++) {
		GtkWidget *s = switch_for(sw[i].on);
		g_signal_connect(s, "notify::active", sw[i].cb, app);
		gtk_box_pack_start(GTK_BOX(c),
				   mp_setting_row(sw[i].likeos, sw[i].fallback,
						  sw[i].title, sw[i].desc, s),
				   FALSE, FALSE, 0);
	}
	GtkWidget *combo = gtk_combo_box_text_new();
	static const char *const sinks[][2] = {
		{ "auto", "Automatic" },     { "xv", "X video overlay" },
		{ "x11", "X11 images" },     { "gtk", "GTK (software)" },
		{ "gl", "GTK with OpenGL" },
	};
	for (gsize i = 0; i < G_N_ELEMENTS(sinks); i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),
					  sinks[i][0], sinks[i][1]);
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo),
				    app->settings.video_sink
					    ? app->settings.video_sink
					    : "auto");
	g_signal_connect(combo, "changed", G_CALLBACK(on_sink), app);
	gtk_box_pack_start(GTK_BOX(c),
			   mp_setting_row("likeos-videos", "video-display-symbolic",
					  "Video output",
					  "How videos are drawn; takes effect "
					  "the next time the player starts",
					  combo),
			   FALSE, FALSE, 0);

	c = section(inner, "About");
	gchar *about = g_strdup_printf("Version %s · GStreamer %u.%u.%u "
				       "· © 2026 The LikeOS Project",
				       MP_VERSION, GST_VERSION_MAJOR,
				       GST_VERSION_MINOR, GST_VERSION_MICRO);
	GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	b = mp_text_button(NULL, NULL, "Keyboard shortcuts", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(mp_dialog_shortcuts),
				 app);
	gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
	b = mp_text_button(NULL, NULL, "About", FALSE);
	g_signal_connect_swapped(b, "clicked", G_CALLBACK(mp_dialog_about),
				 app);
	gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(c),
			   mp_setting_row("likeos-info",
					  "dialog-information-symbolic",
					  MP_APP_NAME, about, bb),
			   FALSE, FALSE, 0);
	g_free(about);

	gtk_box_pack_start(GTK_BOX(box), scrolled(inner), TRUE, TRUE, 0);
	refresh_settings_dirs(pg);
	return box;
}

/* ---- keeping the pages up to date ------------------------------------- */

static void refresh_library_views(Pages *pg)
{
	MpApp *app = pg->app;
	MpLibrary *lib = app->library;

	mp_tile_grid_clear(pg->albums_grid);
	GPtrArray *albums = g_ptr_array_copy(mp_library_albums(lib), NULL,
					     NULL);
	if (pg->sort == SORT_ADDED)
		g_ptr_array_sort(albums, cmp_added);
	for (guint i = 0; i < albums->len; i++)
		gtk_container_add(GTK_CONTAINER(pg->albums_grid),
				  album_tile(app, g_ptr_array_index(albums, i),
					     TILE_ALBUM));
	g_ptr_array_free(albums, TRUE);

	mp_tile_grid_clear(pg->artists_grid);
	GPtrArray *artists = mp_library_artists(lib);
	for (guint i = 0; i < artists->len; i++) {
		MpArtist *ar = g_ptr_array_index(artists, i);
		gchar *sub = count_text(ar->tracks, "song", "songs");
		gtk_container_add(
			GTK_CONTAINER(pg->artists_grid),
			mp_tile_new(MP_TILE_ROUND, TILE_ARTIST, ar->key,
				    ar->name, sub, "likeos-user",
				    "avatar-default-symbolic", artist_art,
				    artist_tile_action, app));
		g_free(sub);
	}

	guint n_audio = mp_library_count(lib, FALSE);
	gtk_stack_set_visible_child_name(GTK_STACK(pg->music_outer),
					 n_audio > 0 ? "library" : "empty");
	gchar *st = n_audio > 0 ? count_text(n_audio, "song", "songs")
				: g_strdup("");
	if (n_audio > 0) {
		gchar *al = count_text(mp_library_albums(lib)->len, "album",
				       "albums");
		gchar *ar = count_text(artists->len, "artist", "artists");
		gchar *s2 = g_strconcat(st, " · ", al, " · ", ar, NULL);
		g_free(st);
		g_free(al);
		g_free(ar);
		st = s2;
	}
	if (mp_library_busy(lib)) {
		gchar *s2 = g_strconcat(st, *st ? " · " : "",
					"looking for music…", NULL);
		g_free(st);
		st = s2;
	}
	mp_page_set_subtitle(pg->music_header, st);
	g_free(st);

	/* Videos: by title. */
	mp_tile_grid_clear(pg->videos_grid);
	GtkTreeModel *m = GTK_TREE_MODEL(mp_library_store(lib));
	GtkTreeModel *sorted = gtk_tree_model_sort_new_with_model(m);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(sorted),
					     MP_LIB_TITLE_KEY,
					     GTK_SORT_ASCENDING);
	GtkTreeIter it;
	guint n_video = 0;
	for (gboolean ok = gtk_tree_model_get_iter_first(sorted, &it); ok;
	     ok = gtk_tree_model_iter_next(sorted, &it)) {
		gboolean video;
		gchar *uri, *title, *dur;
		gtk_tree_model_get(sorted, &it, MP_LIB_VIDEO, &video, MP_LIB_URI,
				   &uri, MP_LIB_TITLE, &title,
				   MP_LIB_DURATION_TEXT, &dur, -1);
		if (video) {
			GtkWidget *t = mp_tile_new(
				MP_TILE_WIDE, TILE_VIDEO, uri, title, NULL,
				"likeos-videos", "video-x-generic-symbolic",
				uri_art, uri_tile_action, app);
			mp_tile_set_badge(t, dur);
			gtk_container_add(GTK_CONTAINER(pg->videos_grid), t);
			n_video++;
		}
		g_free(uri);
		g_free(title);
		g_free(dur);
	}
	g_object_unref(sorted);
	gtk_stack_set_visible_child_name(GTK_STACK(pg->videos_outer),
					 n_video > 0 ? "library" : "empty");
	gchar *vs = n_video > 0 ? count_text(n_video, "video", "videos")
				: NULL;
	mp_page_set_subtitle(pg->videos_header, vs);
	g_free(vs);

	if (pg->lib_status != NULL) {
		gchar *s = g_strdup_printf(
			"%u songs · %u videos%s", n_audio, n_video,
			mp_library_busy(lib) ? " • looking…" : "");
		gtk_label_set_text(GTK_LABEL(pg->lib_status), s);
		g_free(s);
	}
	if (pg->coll_album != NULL || pg->coll_artist != NULL)
		update_collection_header(pg);
	refresh_home(pg);
}

static void on_library_changed(MpLibrary *lib, gpointer d)
{
	(void)lib;
	refresh_library_views(d);
}

static void on_library_picture(MpLibrary *lib, const char *uri, gpointer d)
{
	Pages *pg = d;
	(void)lib;
	(void)uri;
	gtk_widget_queue_draw(pg->app->pages);
	gtk_widget_queue_draw(pg->app->thumb_area);
}

void mp_pages_now_playing_changed(MpApp *app)
{
	Pages *pg = pages_of(app);
	if (pg == NULL)
		return;
	gtk_widget_queue_draw(pg->songs_view);
	gtk_widget_queue_draw(pg->coll_view);
	gtk_widget_queue_draw(pg->search_view);
	refresh_home(pg);
}

/* ---- the stack -------------------------------------------------------- */

void mp_pages_show(MpApp *app, const char *name)
{
	Pages *pg = pages_of(app);
	if (pg == NULL || name == NULL)
		return;
	if (pg->page != NULL && strcmp(pg->page, name) == 0) {
		sync_nav(pg, name);
		return;
	}
	if (pg->page != NULL && strcmp(pg->page, "now") != 0) {
		g_free(pg->prev_page);
		pg->prev_page = g_strdup(pg->page);
	}
	g_free(pg->page);
	pg->page = g_strdup(name);
	gtk_stack_set_visible_child_name(GTK_STACK(app->pages), name);
	sync_nav(pg, name);
	if (strcmp(name, "playlists") == 0)
		refresh_playlists(pg);
	mp_app_page_changed(app);
}

void mp_pages_back(MpApp *app)
{
	Pages *pg = pages_of(app);
	const char *to = pg->prev_page != NULL ? pg->prev_page : "home";
	if (strcmp(pg->page, "collection") == 0 && pg->prev_page != NULL &&
	    strcmp(pg->prev_page, "collection") == 0)
		to = "music";
	gchar *t = g_strdup(to);
	mp_pages_show(app, t);
	g_free(t);
}

const char *mp_pages_current(MpApp *app)
{
	Pages *pg = pages_of(app);
	return pg != NULL && pg->page != NULL ? pg->page : "home";
}

void mp_pages_focus_search(MpApp *app)
{
	Pages *pg = pages_of(app);
	if (app->mini)
		mp_app_set_mini(app, FALSE);
	if (app->fullscreen)
		mp_app_set_fullscreen(app, FALSE);
	set_compact(pg, FALSE);
	gtk_widget_grab_focus(pg->search);
}

gboolean mp_pages_typing(MpApp *app)
{
	GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(app->window));
	return f != NULL && GTK_IS_EDITABLE(f);
}

static void pages_free(gpointer p)
{
	Pages *pg = p;
	g_free(pg->page);
	g_free(pg->prev_page);
	g_free(pg->coll_album);
	g_free(pg->coll_artist);
	g_free(pg->query);
	g_free(pg->pl_menu_path);
	g_clear_object(&pg->songs_sort);
	g_clear_object(&pg->songs_filter);
	g_clear_object(&pg->coll_sort);
	g_clear_object(&pg->coll_filter);
	g_clear_object(&pg->search_filter);
	g_free(pg);
}

void mp_pages_free(MpApp *app)
{
	Pages *pg = pages_of(app);
	if (pg != NULL && pg->pl_menu != NULL) {
		gtk_widget_destroy(pg->pl_menu);
		pg->pl_menu = NULL;
	}
}

void mp_pages_build(MpApp *app, GtkWidget *queue_page, GtkWidget *now_page)
{
	Pages *pg = g_new0(Pages, 1);
	pg->app = app;
	pg->sort = SORT_TITLE;
	g_object_set_data_full(G_OBJECT(app->window), "mp-pages", pg,
			       pages_free);
	app->library = mp_library_new(on_library_changed, on_library_picture,
				      pg);

	build_nav(pg);
	app->pages = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(app->pages),
				      GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_stack_add_named(GTK_STACK(app->pages), build_home(pg), "home");
	gtk_stack_add_named(GTK_STACK(app->pages), build_music(pg), "music");
	gtk_stack_add_named(GTK_STACK(app->pages), build_videos(pg),
			    "videos");
	gtk_stack_add_named(GTK_STACK(app->pages), queue_page, "queue");
	gtk_stack_add_named(GTK_STACK(app->pages), build_playlists(pg),
			    "playlists");
	gtk_stack_add_named(GTK_STACK(app->pages), build_settings(pg),
			    "settings");
	gtk_stack_add_named(GTK_STACK(app->pages), build_collection(pg),
			    "collection");
	gtk_stack_add_named(GTK_STACK(app->pages), build_search(pg),
			    "search");
	gtk_stack_add_named(GTK_STACK(app->pages), now_page, "now");
	apply_sort(pg);
	set_compact(pg, app->settings.nav_compact);
}

void mp_pages_start(MpApp *app)
{
	Pages *pg = pages_of(app);
	set_compact(pg, app->settings.nav_compact);
	refresh_library_views(pg);
	refresh_playlists(pg);
	mp_pages_show(app, "home");
	mp_pages_rescan(app, FALSE);
}
