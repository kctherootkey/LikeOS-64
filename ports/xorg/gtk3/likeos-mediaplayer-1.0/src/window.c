// LikeOS -- Media Player: the main window.
//
// Layout, in the manner of Windows 11's own Media Player:
//
//   +--------------------------------------------+-----------------+
//   |                                            | Play queue   12 |
//   |        video, or the cover art page        |  > Track one    |
//   |                                            |    Track two    |
//   |                                            |    ...          |
//   +--------------------------------------------+                 |
//   | 0:42 ============o------------------- 3:51 |                 |
//   | [art] Title   (shuf) (<<) (>) (>>) (rep)   | [+][dir][-] ... |
//   |       Artist        (mute)--o (queue)(fs)(=)                 |
//   +--------------------------------------------+-----------------+
//
// The queue sits in a GtkPaned so its width is the user's; fullscreen hides
// it and turns the control bar into a dark strip that slides away while the
// pointer rests.
//
// Keyboard handling is done once, at the window, before GTK hands the key
// to the focused widget -- so Space always means play/pause, not "press the
// button that happens to have focus".  The transport buttons never take the
// focus; the queue does, for its own arrow-key navigation, only after it
// has been clicked.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include <gdk/gdkkeysyms.h>

#include "mediaplayer.h"
#include "scan.h"
#include "util.h"

enum {
	TARGET_URI_LIST,
	TARGET_TEXT,
	TARGET_QUEUE_ROWS,
};

#define QUEUE_ROWS_TARGET "application/x-mediaplayer-queue-rows"

static const double speed_steps[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
#define N_SPEEDS G_N_ELEMENTS(speed_steps)

/* Things window.c alone keeps, beside the shared MpApp. */
typedef struct {
	guint gapless_next_id;
	gboolean auto_advanced;	/* current entry started without the user */
	guint consecutive_errors;
	gboolean cover_embedded;
	guint count_timer;
	double scroll_accum;
	GdkCursor *blank_cursor;
	gboolean queue_was_visible;
	GtkWidget *queue_menu;
	GtkWidget *speed_items[N_SPEEDS];
	gboolean building_menu;
	double drag_value;	/* seconds: where the dragged bar is */
	gboolean drag_moved;	/* the bar moved since the button went down */
	guint drag_seek_timer;
} MpPriv;

static MpPriv *priv_of(MpApp *app)
{
	return g_object_get_data(G_OBJECT(app->window), "mp-priv");
}

static void update_play_button(MpApp *app);
static void update_title(MpApp *app);
static void update_now_playing(MpApp *app);
static void update_volume_ui(MpApp *app);
static void update_gapless_next(MpApp *app);
static void show_controls(MpApp *app);
static void infobar_show(MpApp *app, GtkMessageType type, const char *text);
static void on_fullscreen_clicked(GtkButton *b, gpointer data);

/* ---- small widget helpers --------------------------------------------- */

static GtkWidget *icon_button(const char *icon, const char *tooltip,
			      const char *css_class, GtkIconSize size,
			      gboolean toggle)
{
	GtkWidget *b = toggle ? gtk_toggle_button_new() : gtk_button_new();
	GtkWidget *img = gtk_image_new_from_icon_name(icon, size);
	gtk_container_add(GTK_CONTAINER(b), img);
	gtk_widget_set_tooltip_text(b, tooltip);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_focus_on_click(b, FALSE);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	if (css_class != NULL)
		gtk_style_context_add_class(gtk_widget_get_style_context(b),
					    css_class);
	return b;
}

static void set_button_icon(GtkWidget *b, const char *icon)
{
	GtkWidget *img = gtk_bin_get_child(GTK_BIN(b));
	GtkIconSize size = GTK_ICON_SIZE_BUTTON;
	if (GTK_IS_IMAGE(img)) {
		gtk_image_get_icon_name(GTK_IMAGE(img), NULL, &size);
		gtk_image_set_from_icon_name(GTK_IMAGE(img), icon, size);
	}
}

static void add_class(GtkWidget *w, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label,
			    guint key, GdkModifierType mods,
			    GCallback cb, gpointer data)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	GtkWidget *child = gtk_bin_get_child(GTK_BIN(item));
	/* The shortcut is shown for reference; the window's key handler is
	 * what acts on it. */
	if (key != 0 && GTK_IS_ACCEL_LABEL(child))
		gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), key, mods);
	if (cb != NULL)
		g_signal_connect_swapped(item, "activate", cb, data);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

static void menu_separator(GtkWidget *menu)
{
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
			      gtk_separator_menu_item_new());
}

/* ---- settings --------------------------------------------------------- */

static gboolean save_timeout(gpointer data)
{
	MpApp *app = data;
	app->save_timer = 0;
	mp_settings_save(&app->settings);
	return G_SOURCE_REMOVE;
}

void mp_app_schedule_save(MpApp *app)
{
	if (app->save_timer != 0)
		g_source_remove(app->save_timer);
	app->save_timer = g_timeout_add_seconds(2, save_timeout, app);
}

static void capture_geometry(MpApp *app)
{
	GdkWindow *w = gtk_widget_get_window(app->window);
	if (w == NULL)
		return;
	GdkWindowState st = gdk_window_get_state(w);
	if (!(st & (GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_MAXIMIZED))) {
		int ww = 0, wh = 0;
		gtk_window_get_size(GTK_WINDOW(app->window), &ww, &wh);
		if (ww >= 320 && wh >= 240) {
			app->settings.width = ww;
			app->settings.height = wh;
		}
	}
	if (gtk_widget_get_visible(app->queue_panel) && !app->fullscreen) {
		int qw = gtk_widget_get_allocated_width(app->queue_panel);
		if (qw >= 160)
			app->settings.queue_width = qw;
	}
}

/* ---- play queue view -------------------------------------------------- */

static gboolean count_timeout(gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	pv->count_timer = 0;

	GtkTreeModel *m = mp_playlist_model(app->playlist);
	guint n = 0;
	gint64 total = 0;
	gboolean all_known = TRUE;
	GtkTreeIter it;
	gboolean ok = gtk_tree_model_get_iter_first(m, &it);
	while (ok) {
		gint64 d = -1;
		gtk_tree_model_get(m, &it, MP_COL_DURATION, &d, -1);
		if (d >= 0)
			total += d;
		else
			all_known = FALSE;
		n++;
		ok = gtk_tree_model_iter_next(m, &it);
	}

	gchar *text;
	if (n == 0) {
		text = g_strdup("Empty");
	} else {
		gchar *t = mp_format_time(total);
		text = g_strdup_printf("%u %s · %s%s", n,
				       n == 1 ? "item" : "items",
				       all_known ? "" : "at least ",
				       total > 0 ? t : "--:--");
		g_free(t);
	}
	gtk_label_set_text(GTK_LABEL(app->queue_count), text);
	g_free(text);
	gtk_stack_set_visible_child_name(GTK_STACK(app->queue_stack),
					 n > 0 ? "list" : "empty");

	gboolean any = n > 0;
	gtk_widget_set_sensitive(app->btn_next, any);
	gtk_widget_set_sensitive(app->btn_prev, any);
	return G_SOURCE_REMOVE;
}

void mp_app_update_queue_count(MpApp *app)
{
	MpPriv *pv = priv_of(app);
	if (pv->count_timer == 0)
		pv->count_timer = g_timeout_add(250, count_timeout, app);
}

static void on_playlist_changed(MpPlaylist *pl, gpointer data)
{
	MpApp *app = data;
	(void)pl;
	mp_app_update_queue_count(app);
	update_gapless_next(app);
}

static void title_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell,
			    GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	(void)col;
	(void)data;
	gchar *title = NULL, *artist = NULL;
	gint weight = PANGO_WEIGHT_NORMAL;
	gboolean failed = FALSE;
	gtk_tree_model_get(m, it, MP_COL_TITLE, &title, MP_COL_ARTIST,
			   &artist, MP_COL_WEIGHT, &weight, MP_COL_FAILED,
			   &failed, -1);
	gchar *et = g_markup_escape_text(title ? title : "", -1);
	gchar *markup;
	const char *open = failed ? "<span foreground=\"#a0a0a0\">" : "";
	const char *close = failed ? "</span>" : "";
	if (artist != NULL && *artist) {
		gchar *ea = g_markup_escape_text(artist, -1);
		markup = g_strdup_printf(
			"%s<span weight=\"%d\">%s</span>\n"
			"<span size=\"small\" foreground=\"#616161\">%s</span>%s",
			open, weight, et, ea, close);
		g_free(ea);
	} else {
		markup = g_strdup_printf("%s<span weight=\"%d\">%s</span>%s",
					 open, weight, et, close);
	}
	g_object_set(cell, "markup", markup, NULL);
	g_free(markup);
	g_free(et);
	g_free(title);
	g_free(artist);
}

static GList *selected_ids(MpApp *app)
{
	GtkTreeSelection *sel =
		gtk_tree_view_get_selection(GTK_TREE_VIEW(app->queue_view));
	GtkTreeModel *m = NULL;
	GList *rows = gtk_tree_selection_get_selected_rows(sel, &m);
	GList *ids = NULL;
	for (GList *l = rows; l != NULL; l = l->next) {
		GtkTreeIter it;
		if (gtk_tree_model_get_iter(m, &it, l->data))
			ids = g_list_prepend(
				ids, GUINT_TO_POINTER(mp_playlist_id_at(
					     app->playlist, &it)));
	}
	g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
	return g_list_reverse(ids);
}

static void queue_remove_selected(MpApp *app)
{
	GList *ids = selected_ids(app);
	if (ids == NULL)
		return;

	/* Select the row after the removed block, so repeated Delete walks
	 * down the list the way a file manager's does. */
	guint last = GPOINTER_TO_UINT(g_list_last(ids)->data);
	GtkTreeIter it;
	guint after = 0;
	if (mp_playlist_get_iter(app->playlist, last, &it) &&
	    gtk_tree_model_iter_next(mp_playlist_model(app->playlist), &it))
		after = mp_playlist_id_at(app->playlist, &it);

	mp_playlist_freeze_notify(app->playlist);
	for (GList *l = ids; l != NULL; l = l->next) {
		guint id = GPOINTER_TO_UINT(l->data);
		mp_discover_forget(app->discover, id);
		if (id == app->current_id)
			/* What is playing keeps playing; it is just no longer
			 * in the queue, and "next" carries on from its
			 * place. */
			app->current_id = 0;
		mp_playlist_remove(app->playlist, id);
	}
	mp_playlist_thaw_notify(app->playlist);
	g_list_free(ids);

	if (after != 0 && mp_playlist_get_iter(app->playlist, after, &it)) {
		GtkTreeSelection *sel = gtk_tree_view_get_selection(
			GTK_TREE_VIEW(app->queue_view));
		gtk_tree_selection_unselect_all(sel);
		gtk_tree_selection_select_iter(sel, &it);
	}
}

static void queue_move_selected(MpApp *app, int delta)
{
	GList *ids = selected_ids(app);
	if (ids == NULL)
		return;
	/* Moving down, the lowest row goes first so the rows do not leap
	 * over each other. */
	if (delta > 0)
		ids = g_list_reverse(ids);
	for (GList *l = ids; l != NULL; l = l->next)
		mp_playlist_move(app->playlist, GPOINTER_TO_UINT(l->data),
				 delta);
	guint first = GPOINTER_TO_UINT(ids->data);
	GtkTreeIter it;
	if (mp_playlist_get_iter(app->playlist, first, &it)) {
		GtkTreePath *p = gtk_tree_model_get_path(
			mp_playlist_model(app->playlist), &it);
		gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(app->queue_view), p,
					     NULL, FALSE, 0, 0);
		gtk_tree_path_free(p);
	}
	g_list_free(ids);
}

static void queue_move_up(MpApp *app)
{
	queue_move_selected(app, -1);
}

static void queue_move_down(MpApp *app)
{
	queue_move_selected(app, 1);
}

void mp_app_clear_queue(MpApp *app)
{
	mp_discover_clear(app->discover);
	mp_playlist_clear(app->playlist);
	app->current_id = 0;
	update_gapless_next(app);
}

static void queue_clear(MpApp *app)
{
	mp_app_clear_queue(app);
}

static void queue_sort(MpApp *app)
{
	mp_playlist_sort_by_title(app->playlist);
}

static void queue_play_selected(MpApp *app)
{
	GList *ids = selected_ids(app);
	if (ids != NULL)
		mp_app_play_id(app, GPOINTER_TO_UINT(ids->data));
	g_list_free(ids);
}

static void scroll_to_current(MpApp *app)
{
	GtkTreeIter it;
	if (!mp_playlist_get_iter(app->playlist, app->current_id, &it))
		return;
	GtkTreePath *p = gtk_tree_model_get_path(
		mp_playlist_model(app->playlist), &it);
	gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(app->queue_view), p, NULL,
				     FALSE, 0, 0);
	gtk_tree_path_free(p);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
			     GtkTreeViewColumn *col, gpointer data)
{
	MpApp *app = data;
	(void)col;
	GtkTreeIter it;
	if (gtk_tree_model_get_iter(gtk_tree_view_get_model(tv), &it, path))
		mp_app_play_id(app, mp_playlist_id_at(app->playlist, &it));
}

static gboolean on_queue_button(GtkWidget *w, GdkEventButton *ev,
				gpointer data)
{
	MpApp *app = data;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
		return FALSE;

	/* Right-click on an unselected row selects just that row first, as
	 * every file list does. */
	GtkTreePath *path = NULL;
	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (gint)ev->x,
					  (gint)ev->y, &path, NULL, NULL,
					  NULL)) {
		GtkTreeSelection *sel =
			gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
		if (!gtk_tree_selection_path_is_selected(sel, path)) {
			gtk_tree_selection_unselect_all(sel);
			gtk_tree_selection_select_path(sel, path);
		}
		gtk_tree_path_free(path);
	}
	gtk_menu_popup_at_pointer(GTK_MENU(priv_of(app)->queue_menu),
				  (GdkEvent *)ev);
	return TRUE;
}

/* ---- drag and drop ---------------------------------------------------- */

typedef struct {
	MpApp *app;
	gint position;
	gboolean play_first;
} ScanTarget;

static void scan_done(MpScanResult *r, gpointer data)
{
	ScanTarget *t = data;
	MpApp *app = t->app;
	app->scans_running--;

	guint first = 0;
	gint pos = t->position;
	mp_playlist_freeze_notify(app->playlist);
	for (guint i = 0; i < r->uris->len; i++) {
		const char *uri = g_ptr_array_index(r->uris, i);
		const char *title = g_ptr_array_index(r->titles, i);
		guint id = mp_playlist_insert(app->playlist, pos, uri, title,
					      -1);
		if (id == 0)
			break;
		if (pos >= 0)
			pos++;
		if (first == 0)
			first = id;
		if (app->settings.discover && title == NULL)
			mp_discover_queue(app->discover, id, uri);
	}
	mp_playlist_thaw_notify(app->playlist);

	if (r->problems->len > 0 || r->truncated) {
		GString *s = g_string_new(NULL);
		if (r->truncated)
			g_string_append_printf(
				s, "The play queue holds at most %d entries; "
				   "the rest were not added.",
				MP_PLAYLIST_MAX);
		for (guint i = 0; i < r->problems->len && i < 3; i++) {
			gchar *clean = mp_sanitize_text(
				g_ptr_array_index(r->problems, i), 300);
			g_string_append_printf(s, "%s%s", s->len ? "\n" : "",
					       clean);
			g_free(clean);
		}
		if (r->problems->len > 3)
			g_string_append_printf(s, "\n…and %u more",
					       r->problems->len - 3);
		infobar_show(app, GTK_MESSAGE_WARNING, s->str);
		g_string_free(s, TRUE);
	} else if (r->uris->len == 0) {
		infobar_show(app, GTK_MESSAGE_INFO,
			     "No playable files were found there.");
	}

	MpPlayerState st = mp_player_get_state(app->player);
	if (first != 0 && (t->play_first || st == MP_PLAYER_STOPPED))
		mp_app_play_id(app, first);
	g_free(t);
}

static void add_inputs(MpApp *app, gchar **uris, gint position,
		       gboolean play_first)
{
	if (uris == NULL || uris[0] == NULL)
		return;
	ScanTarget *t = g_new0(ScanTarget, 1);
	t->app = app;
	t->position = position;
	t->play_first = play_first;
	app->scans_running++;
	mp_scan_async(uris, app->cancel, scan_done, t);
}

void mp_app_add_paths_async(MpApp *app, gchar **args, gboolean play_first)
{
	GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
	for (gchar **a = args; a != NULL && *a != NULL; a++) {
		gchar *u = mp_uri_from_arg(*a, NULL);
		if (u != NULL)
			g_ptr_array_add(uris, u);
	}
	g_ptr_array_add(uris, NULL);
	add_inputs(app, (gchar **)uris->pdata, -1, play_first);
	g_ptr_array_free(uris, TRUE);
}

void mp_app_add_uris(MpApp *app, gchar **uris, gboolean play_first,
		     gint position)
{
	add_inputs(app, uris, position, play_first);
}

/* URIs out of a drop: a uri-list from a file manager, or text (a link
 * dragged out of a browser, one per line). */
static gchar **uris_from_selection(GtkSelectionData *sel, guint info)
{
	if (info == TARGET_URI_LIST) {
		gchar **u = gtk_selection_data_get_uris(sel);
		if (u != NULL)
			return u;
	}
	guchar *text = gtk_selection_data_get_text(sel);
	if (text == NULL)
		return NULL;
	GPtrArray *out = g_ptr_array_new();
	gchar **lines = g_strsplit((const gchar *)text, "\n", 1000);
	for (gchar **l = lines; *l != NULL; l++) {
		gchar *s = g_strstrip(*l);
		if (*s == '\0' || *s == '#')
			continue;
		if (strstr(s, "://") == NULL && !g_path_is_absolute(s))
			continue;
		gchar *u = mp_uri_from_arg(s, NULL);
		if (u != NULL)
			g_ptr_array_add(out, u);
	}
	g_strfreev(lines);
	g_free(text);
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

static gint drop_index(MpApp *app, gint x, gint y)
{
	GtkTreePath *path = NULL;
	GtkTreeViewDropPosition dp;
	if (!gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(app->queue_view),
					       x, y, &path, &dp))
		return -1;
	gint idx = gtk_tree_path_get_indices(path)[0];
	if (dp == GTK_TREE_VIEW_DROP_AFTER ||
	    dp == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)
		idx++;
	gtk_tree_path_free(path);
	return idx;
}

static void on_drop_received(GtkWidget *w, GdkDragContext *ctx, gint x,
			     gint y, GtkSelectionData *sel, guint info,
			     guint time, gpointer data)
{
	MpApp *app = data;
	(void)ctx;
	(void)time;

	if (info == TARGET_QUEUE_ROWS && w == app->queue_view) {
		/* Rows dragged inside the queue: the selection data is the
		 * list of their ids. */
		const guchar *raw = gtk_selection_data_get_data(sel);
		gint len = gtk_selection_data_get_length(sel);
		if (raw != NULL && len > 0 && len < 1024 * 1024) {
			gchar *s = g_strndup((const gchar *)raw, (gsize)len);
			gchar **parts = g_strsplit(s, ",", -1);
			GArray *ids = g_array_new(FALSE, FALSE, sizeof(guint));
			for (gchar **p = parts; *p != NULL; p++) {
				guint64 v = g_ascii_strtoull(*p, NULL, 10);
				if (v > 0 && v <= G_MAXUINT) {
					guint id = (guint)v;
					g_array_append_val(ids, id);
				}
			}
			mp_playlist_move_to(app->playlist,
					    (guint *)(void *)ids->data,
					    ids->len, drop_index(app, x, y));
			g_array_free(ids, TRUE);
			g_strfreev(parts);
			g_free(s);
		}
		g_signal_stop_emission_by_name(w, "drag-data-received");
		return;
	}

	gchar **uris = uris_from_selection(sel, info);
	if (uris != NULL && uris[0] != NULL) {
		gint pos = w == app->queue_view ? drop_index(app, x, y) : -1;
		/* Dropped on the picture: play it now.  Dropped on the queue:
		 * just add it where it fell. */
		add_inputs(app, uris, pos, w != app->queue_view);
	}
	g_strfreev(uris);
	if (w == app->queue_view)
		g_signal_stop_emission_by_name(w, "drag-data-received");
}

static gboolean on_queue_drag_motion(GtkWidget *w, GdkDragContext *ctx,
				     gint x, gint y, guint time,
				     gpointer data)
{
	(void)ctx;
	(void)time;
	(void)data;
	/* Show where the drop would land.  Runs after GtkTreeView's own
	 * handler, which clears the mark because the view is not a model
	 * drag destination in its own sense. */
	GtkTreePath *path = NULL;
	GtkTreeViewDropPosition dp;
	if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(w), x, y, &path,
					      &dp)) {
		if (dp == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE)
			dp = GTK_TREE_VIEW_DROP_BEFORE;
		else if (dp == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)
			dp = GTK_TREE_VIEW_DROP_AFTER;
		gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), path, dp);
		gtk_tree_path_free(path);
	} else {
		gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), NULL, 0);
	}
	return FALSE;
}

static void on_queue_drag_leave(GtkWidget *w, GdkDragContext *ctx,
				guint time, gpointer data)
{
	(void)ctx;
	(void)time;
	(void)data;
	gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), NULL, 0);
}

static void on_queue_drag_get(GtkWidget *w, GdkDragContext *ctx,
			      GtkSelectionData *sel, guint info, guint time,
			      gpointer data)
{
	MpApp *app = data;
	(void)ctx;
	(void)time;
	if (info != TARGET_QUEUE_ROWS)
		return;
	GList *ids = selected_ids(app);
	GString *s = g_string_new(NULL);
	for (GList *l = ids; l != NULL; l = l->next)
		g_string_append_printf(s, "%s%u", s->len ? "," : "",
				       GPOINTER_TO_UINT(l->data));
	g_list_free(ids);
	gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8,
			       (const guchar *)s->str, (gint)s->len);
	g_string_free(s, TRUE);
	g_signal_stop_emission_by_name(w, "drag-data-get");
}

static void on_queue_drag_delete(GtkWidget *w, GdkDragContext *ctx,
				 gpointer data)
{
	(void)ctx;
	(void)data;
	/* Never let GtkTreeView delete the source row: moves are done by
	 * mp_playlist_move_to(), which keeps the rows (and their ids). */
	g_signal_stop_emission_by_name(w, "drag-data-delete");
}

static void setup_drop_target(GtkWidget *w, MpApp *app, gboolean rows)
{
	gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, NULL, 0,
			  GDK_ACTION_COPY | GDK_ACTION_MOVE |
				  GDK_ACTION_LINK);
	GtkTargetList *tl = gtk_target_list_new(NULL, 0);
	gtk_target_list_add_uri_targets(tl, TARGET_URI_LIST);
	gtk_target_list_add_text_targets(tl, TARGET_TEXT);
	if (rows)
		gtk_target_list_add(tl,
				    gdk_atom_intern_static_string(
					    QUEUE_ROWS_TARGET),
				    GTK_TARGET_SAME_WIDGET, TARGET_QUEUE_ROWS);
	gtk_drag_dest_set_target_list(w, tl);
	gtk_target_list_unref(tl);
	g_signal_connect(w, "drag-data-received",
			 G_CALLBACK(on_drop_received), app);
}

/* ---- the media area --------------------------------------------------- */

static gboolean on_art_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	MpApp *app = data;
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	double side = MIN(MIN(aw, ah), 420);
	gboolean video = app->current_id != 0 &&
			 mp_player_has_video(app->player);
	mp_cover_draw(cr, app->cover, (aw - side) / 2, (ah - side) / 2, side,
		      side, 10,
		      video ? "video-x-generic-symbolic"
			    : "audio-x-generic-symbolic",
		      w);
	return FALSE;
}

static gboolean on_thumb_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	MpApp *app = data;
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	if (app->current_id == 0 && app->cover == NULL &&
	    mp_player_get_state(app->player) == MP_PLAYER_STOPPED)
		return FALSE;
	gboolean video = mp_player_has_video(app->player);
	mp_cover_draw(cr, app->cover, 0, 0, aw, ah, 4,
		      video ? "video-x-generic-symbolic"
			    : "audio-x-generic-symbolic",
		      w);
	return FALSE;
}

static void set_cursor_visible(MpApp *app, gboolean visible)
{
	MpPriv *pv = priv_of(app);
	GdkCursor *c = visible ? NULL : pv->blank_cursor;
	GdkWindow *w = gtk_widget_get_window(app->media_events);
	if (w != NULL)
		gdk_window_set_cursor(w, c);
	if (app->video_widget != NULL) {
		w = gtk_widget_get_window(app->video_widget);
		if (w != NULL)
			gdk_window_set_cursor(w, c);
	}
}

static gboolean pointer_over_controls(MpApp *app)
{
	GdkWindow *win = gtk_widget_get_window(app->controls);
	if (win == NULL || !gtk_widget_get_mapped(app->controls))
		return FALSE;
	GdkSeat *seat = gdk_display_get_default_seat(
		gtk_widget_get_display(app->window));
	GdkDevice *ptr = gdk_seat_get_pointer(seat);
	int x = 0, y = 0;
	gdk_window_get_device_position(gtk_widget_get_window(app->window), ptr,
				       &x, &y, NULL);
	GtkAllocation a;
	gtk_widget_get_allocation(app->controls, &a);
	int cx = 0, cy = 0;
	gtk_widget_translate_coordinates(app->controls, app->window, 0, 0,
					 &cx, &cy);
	return x >= cx && x < cx + a.width && y >= cy && y < cy + a.height;
}

static gboolean hide_timeout(gpointer data)
{
	MpApp *app = data;
	app->hide_timer = 0;
	if (!app->fullscreen)
		return G_SOURCE_REMOVE;
	/* Keep them while the pointer is on them, and while nothing plays
	 * (a paused film with no controls looks like a hung program). */
	if (pointer_over_controls(app) ||
	    mp_player_get_state(app->player) != MP_PLAYER_PLAYING) {
		app->hide_timer = g_timeout_add(MP_FULLSCREEN_HIDE_MS,
						hide_timeout, app);
		return G_SOURCE_REMOVE;
	}
	gtk_revealer_set_reveal_child(GTK_REVEALER(app->controls_revealer),
				      FALSE);
	app->controls_hidden = TRUE;
	set_cursor_visible(app, FALSE);
	return G_SOURCE_REMOVE;
}

static void show_controls(MpApp *app)
{
	if (app->controls_hidden) {
		gtk_revealer_set_reveal_child(
			GTK_REVEALER(app->controls_revealer), TRUE);
		app->controls_hidden = FALSE;
		set_cursor_visible(app, TRUE);
	}
	if (app->hide_timer != 0) {
		g_source_remove(app->hide_timer);
		app->hide_timer = 0;
	}
	if (app->fullscreen)
		app->hide_timer = g_timeout_add(MP_FULLSCREEN_HIDE_MS,
						hide_timeout, app);
}

static gboolean on_media_motion(GtkWidget *w, GdkEventMotion *ev,
				gpointer data)
{
	(void)w;
	(void)ev;
	MpApp *app = data;
	if (app->fullscreen)
		show_controls(app);
	return FALSE;
}

static gboolean on_media_button(GtkWidget *w, GdkEventButton *ev,
				gpointer data)
{
	MpApp *app = data;
	(void)w;

	/* A click on the picture takes the keyboard back from the queue, so
	 * the arrow keys seek and change the volume again. */
	gtk_window_set_focus(GTK_WINDOW(app->window), NULL);

	if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
		mp_app_set_fullscreen(app, !app->fullscreen);
		return TRUE;
	}
	if (ev->type == GDK_BUTTON_PRESS && ev->button == GDK_BUTTON_MIDDLE) {
		mp_app_toggle_play(app);
		return TRUE;
	}
	if (ev->type == GDK_BUTTON_PRESS &&
	    ev->button == GDK_BUTTON_SECONDARY) {
		gtk_menu_popup_at_pointer(GTK_MENU(app->menu), (GdkEvent *)ev);
		return TRUE;
	}
	if (app->fullscreen)
		show_controls(app);
	return FALSE;
}

static gboolean on_media_scroll(GtkWidget *w, GdkEventScroll *ev,
				gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	(void)w;
	double steps = 0;

	switch (ev->direction) {
	case GDK_SCROLL_UP:
		steps = 1;
		break;
	case GDK_SCROLL_DOWN:
		steps = -1;
		break;
	case GDK_SCROLL_SMOOTH:
		/* Touchpads deliver fractions; add them up to whole steps. */
		pv->scroll_accum -= ev->delta_y;
		if (fabs(pv->scroll_accum) >= 1.0) {
			steps = trunc(pv->scroll_accum);
			pv->scroll_accum -= steps;
		}
		break;
	default:
		return FALSE;
	}
	if (steps != 0)
		mp_app_set_volume(app, app->settings.volume +
					       steps * MP_VOLUME_STEP);
	return TRUE;
}

static void connect_media_events(MpApp *app, GtkWidget *w)
{
	gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK |
					 GDK_POINTER_MOTION_MASK |
					 GDK_SCROLL_MASK |
					 GDK_SMOOTH_SCROLL_MASK);
	g_signal_connect(w, "motion-notify-event",
			 G_CALLBACK(on_media_motion), app);
	g_signal_connect(w, "button-press-event", G_CALLBACK(on_media_button),
			 app);
	g_signal_connect(w, "scroll-event", G_CALLBACK(on_media_scroll), app);
}

static void show_media_page(MpApp *app)
{
	gboolean video = app->video_widget != NULL && app->current_id != 0 &&
			 mp_player_has_video(app->player) &&
			 mp_player_get_state(app->player) != MP_PLAYER_STOPPED;
	/* Keep showing the picture while the same entry is merely being
	 * re-loaded or sought. */
	gtk_stack_set_visible_child_name(GTK_STACK(app->media_stack),
					 video ? "video" : "art");
	gtk_widget_queue_draw(app->art_area);
	gtk_widget_queue_draw(app->thumb_area);
}

/* ---- now playing ------------------------------------------------------ */

static gchar *current_tag(MpApp *app, const char *tag)
{
	const GstTagList *tl = mp_player_get_tags(app->player);
	gchar *s = NULL;
	if (tl != NULL && gst_tag_list_get_string(tl, tag, &s)) {
		gchar *clean = mp_sanitize_text(s, 300);
		g_free(s);
		return clean;
	}
	return NULL;
}

static gchar *current_display_title(MpApp *app)
{
	gchar *t = current_tag(app, GST_TAG_TITLE);
	if (t != NULL && *t)
		return t;
	g_free(t);
	if (app->current_id != 0) {
		gchar *pt = mp_playlist_get_title(app->playlist,
						  app->current_id);
		if (pt != NULL)
			return pt;
	}
	const char *uri = mp_player_get_uri(app->player);
	return uri ? mp_uri_display_name(uri) : NULL;
}

static void update_title(MpApp *app)
{
	gchar *t = app->current_id != 0 ||
				   mp_player_get_uri(app->player) != NULL
			   ? current_display_title(app)
			   : NULL;
	if (t != NULL && *t) {
		gchar *full = g_strdup_printf("%s – %s", t, MP_APP_NAME);
		gtk_window_set_title(GTK_WINDOW(app->window), full);
		g_free(full);
	} else {
		gtk_window_set_title(GTK_WINDOW(app->window), MP_APP_NAME);
	}
	g_free(t);
}

static void update_now_playing(MpApp *app)
{
	gboolean loaded = mp_player_get_uri(app->player) != NULL;
	gchar *title = loaded ? current_display_title(app) : NULL;
	gchar *artist = loaded ? current_tag(app, GST_TAG_ARTIST) : NULL;
	gchar *album = loaded ? current_tag(app, GST_TAG_ALBUM) : NULL;
	if (artist == NULL && loaded)
		artist = current_tag(app, GST_TAG_ALBUM_ARTIST);

	gtk_label_set_text(GTK_LABEL(app->now_title), title ? title : "");
	gtk_label_set_text(GTK_LABEL(app->now_artist), artist ? artist : "");
	gtk_label_set_text(GTK_LABEL(app->art_title),
			   title ? title : "Nothing playing");

	gchar *sub;
	if (artist != NULL && album != NULL)
		sub = g_strdup_printf("%s — %s", artist, album);
	else
		sub = g_strdup(artist ? artist : album ? album : "");
	gtk_label_set_text(GTK_LABEL(app->art_artist), sub);
	g_free(sub);

	if (title != NULL && *title)
		gtk_widget_set_tooltip_text(app->now_title, title);
	update_title(app);
	g_free(title);
	g_free(artist);
	g_free(album);
}

static void set_cover(MpApp *app, GdkPixbuf *pb, gchar *source)
{
	g_clear_object(&app->cover);
	g_free(app->cover_source);
	app->cover = pb;
	app->cover_source = source;
	gtk_widget_queue_draw(app->art_area);
	gtk_widget_queue_draw(app->thumb_area);
}

static void update_status(MpApp *app, int buffering)
{
	MpPlayerState st = mp_player_get_state(app->player);
	const char *text = "";
	gchar *own = NULL;

	if (buffering >= 0 && buffering < 100)
		text = own = g_strdup_printf("Buffering… %d %%",
					     buffering);
	else if (mp_player_get_uri(app->player) == NULL ||
		 (app->current_id == 0 && st == MP_PLAYER_STOPPED))
		text = mp_playlist_count(app->playlist) == 0
			       ? "Open files or drop them here to start playing"
			       : "Double-click an entry in the queue to play it";
	else if (st == MP_PLAYER_LOADING)
		text = "Loading…";
	else if (st == MP_PLAYER_PAUSED)
		text = "Paused";
	else if (st == MP_PLAYER_STOPPED)
		text = "Stopped";

	if (st == MP_PLAYER_PLAYING && app->speed != 1.0)
		text = own = g_strdup_printf("Playing at %.2g×",
					     app->speed);
	gtk_label_set_text(GTK_LABEL(app->art_status), text);
	g_free(own);
}

static void update_play_button(MpApp *app)
{
	gboolean playing = mp_player_wants_playing(app->player) &&
			   mp_player_get_state(app->player) != MP_PLAYER_STOPPED;
	set_button_icon(app->btn_play, playing ? "media-playback-pause-symbolic"
					       : "media-playback-start-symbolic");
	gtk_widget_set_tooltip_text(app->btn_play,
				    playing ? "Pause (Space)" : "Play (Space)");
	gtk_widget_set_sensitive(app->btn_stop,
				 mp_player_get_state(app->player) !=
					 MP_PLAYER_STOPPED);
}

/* ---- position --------------------------------------------------------- */

static void set_time_labels(MpApp *app, gint64 pos)
{
	gchar *a = mp_format_time(pos);
	gchar *b = app->show_remaining
			   ? mp_format_remaining(pos, app->duration)
			   : mp_format_time(app->duration);
	gtk_label_set_text(GTK_LABEL(app->time_label), a);
	gtk_label_set_text(GTK_LABEL(app->duration_label), b);
	g_free(a);
	g_free(b);
}

/* Move the bar and the times to `pos' -- unless the user holds the bar. */
static void show_position(MpApp *app, gint64 pos)
{
	if (app->user_seeking)
		return;
	if (app->duration > 0 && pos >= 0) {
		app->updating_seek = TRUE;
		gtk_adjustment_set_value(app->seek_adj,
					 (double)pos / GST_SECOND);
		app->updating_seek = FALSE;
	}
	set_time_labels(app, pos);
}

static gboolean position_tick(gpointer data)
{
	MpApp *app = data;
	show_position(app, mp_player_get_position(app->player));
	return G_SOURCE_CONTINUE;
}

static void start_position_timer(MpApp *app, gboolean on)
{
	if (on && app->position_timer == 0) {
		app->position_timer = g_timeout_add(MP_POSITION_INTERVAL_MS,
						    position_tick, app);
	} else if (!on && app->position_timer != 0) {
		g_source_remove(app->position_timer);
		app->position_timer = 0;
	}
}

static void set_duration(MpApp *app, gint64 d)
{
	app->duration = d;
	gboolean seekable = d > 0 && mp_player_is_seekable(app->player);
	app->updating_seek = TRUE;
	gtk_adjustment_configure(app->seek_adj,
				 seekable ? gtk_adjustment_get_value(
						    app->seek_adj)
					  : 0,
				 0, d > 0 ? (double)d / GST_SECOND : 1, 1, 10,
				 0);
	app->updating_seek = FALSE;
	gtk_widget_set_sensitive(app->seek_scale, seekable);
	set_time_labels(app, mp_player_get_position(app->player));
}

static gboolean drag_seek_timeout(gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	pv->drag_seek_timer = 0;
	mp_player_seek(app->player, (gint64)(pv->drag_value * GST_SECOND),
		       FALSE);
	return G_SOURCE_REMOVE;
}

/* The button that held the bar went up (or GTK lost the grab): one exact
 * seek to where the bar was let go, if it moved at all. */
static void end_seek_drag(MpApp *app)
{
	MpPriv *pv = priv_of(app);
	if (!app->user_seeking)
		return;
	app->user_seeking = FALSE;
	if (pv->drag_seek_timer != 0) {
		g_source_remove(pv->drag_seek_timer);
		pv->drag_seek_timer = 0;
	}
	if (!pv->drag_moved || app->duration <= 0)
		return;
	pv->drag_moved = FALSE;
	gint64 target = (gint64)(pv->drag_value * GST_SECOND);
	mp_player_seek(app->player, target, TRUE);
	show_position(app, target);
}

static gboolean on_seek_change_value(GtkRange *range, GtkScrollType scroll,
				     gdouble value, gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	(void)range;
	(void)scroll;

	if (app->duration <= 0)
		return TRUE;
	double max = (double)app->duration / GST_SECOND;
	if (value < 0)
		value = 0;
	if (value > max)
		value = max;
	pv->drag_value = value;
	set_time_labels(app, (gint64)(value * GST_SECOND));

	if (app->user_seeking) {
		pv->drag_moved = TRUE;
		/* Dragging: seek to key frames, at most a few times a
		 * second, so the picture follows without the decoder being
		 * flushed on every pixel of movement. */
		if (pv->drag_seek_timer == 0)
			pv->drag_seek_timer =
				g_timeout_add(120, drag_seek_timeout, app);
	} else {
		mp_player_seek(app->player, (gint64)(value * GST_SECOND),
			       TRUE);
	}
	return FALSE;
}

/* A primary press on the bar starts a drag: a click on the trough jumps
 * the slider there (gtk-primary-button-warps-slider) and is the start of a
 * drag as well.  The handlers run before GtkRange's own gestures, which
 * still get the events. */
static gboolean on_seek_press(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
	MpApp *app = d;
	(void)w;
	if (ev->type == GDK_BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY) {
		app->user_seeking = TRUE;
		priv_of(app)->drag_moved = FALSE;
	}
	return FALSE;
}

static gboolean on_seek_release(GtkWidget *w, GdkEventButton *ev,
				gpointer d)
{
	MpApp *app = d;
	(void)w;
	if (ev->button == GDK_BUTTON_PRIMARY)
		end_seek_drag(app);
	return FALSE;
}

static gboolean on_seek_grab_broken(GtkWidget *w, GdkEventGrabBroken *ev,
				    gpointer d)
{
	(void)w;
	(void)ev;
	/* No release will come: do not leave the bar frozen. */
	end_seek_drag(d);
	return FALSE;
}

static gboolean on_duration_click(GtkWidget *w, GdkEventButton *ev,
				  gpointer d)
{
	MpApp *app = d;
	(void)w;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_PRIMARY)
		return FALSE;
	app->show_remaining = !app->show_remaining;
	app->settings.show_remaining = app->show_remaining;
	set_time_labels(app, mp_player_get_position(app->player));
	mp_app_schedule_save(app);
	return TRUE;
}

/* ---- transport -------------------------------------------------------- */

static void update_gapless_next(MpApp *app)
{
	MpPriv *pv = priv_of(app);
	pv->gapless_next_id = 0;
	if (!app->settings.gapless || app->current_id == 0 ||
	    mp_player_get_state(app->player) == MP_PLAYER_STOPPED) {
		mp_player_set_next_uri(app->player, NULL);
		return;
	}
	guint id = mp_playlist_peek_next(app->playlist);
	gchar *uri = id ? mp_playlist_get_uri(app->playlist, id) : NULL;
	mp_player_set_next_uri(app->player, uri);
	pv->gapless_next_id = uri ? id : 0;
	g_free(uri);
}

/* The new entry is loaded (or gapless playback moved into it): reset what
 * belonged to the old one. */
static void entry_started(MpApp *app, guint id)
{
	MpPriv *pv = priv_of(app);
	app->current_id = id;
	mp_playlist_set_current(app->playlist, id);
	mp_playlist_set_mark(app->playlist, id, MP_MARK_PLAYING);
	mp_discover_forget(app->discover, id);
	pv->cover_embedded = FALSE;

	gchar *uri = mp_playlist_get_uri(app->playlist, id);
	gchar *src = NULL;
	GdkPixbuf *folder = uri ? mp_cover_from_folder(uri, &src) : NULL;
	set_cover(app, folder, src);
	g_free(uri);

	/* -1 after a load; after a gapless switch the player may know the
	 * new entry's duration already. */
	set_duration(app, mp_player_get_duration(app->player));
	update_now_playing(app);
	update_status(app, -1);
	show_media_page(app);
	scroll_to_current(app);
	update_gapless_next(app);
	if (app->info_dialog != NULL)
		mp_dialog_media_info_refresh(app);
}

void mp_app_play_id(MpApp *app, guint id)
{
	gchar *uri = mp_playlist_get_uri(app->playlist, id);
	if (uri == NULL)
		return;
	priv_of(app)->auto_advanced = FALSE;
	mp_playlist_set_failed(app->playlist, id, FALSE);
	/* Current before loading: the state callbacks the load triggers
	 * mark the queue row, and must mark this one. */
	app->current_id = id;
	mp_playlist_set_current(app->playlist, id);
	mp_player_load(app->player, uri, TRUE);
	entry_started(app, id);
	g_free(uri);
}

static void play_id_auto(MpApp *app, guint id)
{
	mp_app_play_id(app, id);
	priv_of(app)->auto_advanced = TRUE;
}

void mp_app_next(MpApp *app, gboolean user)
{
	guint id = mp_playlist_next(app->playlist, user);
	if (id != 0) {
		if (user)
			mp_app_play_id(app, id);
		else
			play_id_auto(app, id);
	} else if (!user) {
		mp_app_stop(app);
	}
}

void mp_app_prev(MpApp *app)
{
	gint64 pos = mp_player_get_position(app->player);
	if (pos > MP_PREV_RESTART_NS && mp_player_is_seekable(app->player)) {
		mp_player_seek(app->player, 0, TRUE);
		return;
	}
	guint id = mp_playlist_prev(app->playlist);
	if (id != 0)
		mp_app_play_id(app, id);
}

void mp_app_toggle_play(MpApp *app)
{
	MpPlayerState st = mp_player_get_state(app->player);

	if (st != MP_PLAYER_STOPPED && mp_player_wants_playing(app->player)) {
		mp_player_pause(app->player);
		return;
	}
	if (st != MP_PLAYER_STOPPED) {
		mp_player_play(app->player);
		return;
	}
	/* Stopped: start the current entry again, else the selected one,
	 * else the first. */
	guint id = app->current_id;
	if (id == 0) {
		GList *sel = selected_ids(app);
		if (sel != NULL)
			id = GPOINTER_TO_UINT(sel->data);
		g_list_free(sel);
	}
	if (id == 0)
		id = mp_playlist_next(app->playlist, TRUE);
	if (id != 0)
		mp_app_play_id(app, id);
	else
		mp_dialog_open_files(app);
}

void mp_app_stop(MpApp *app)
{
	mp_player_stop(app->player);
	if (app->current_id != 0)
		mp_playlist_set_mark(app->playlist, app->current_id,
				     MP_MARK_NONE);
	app->updating_seek = TRUE;
	gtk_adjustment_set_value(app->seek_adj, 0);
	app->updating_seek = FALSE;
	set_time_labels(app, 0);
	update_status(app, -1);
	show_media_page(app);
	update_gapless_next(app);
}

void mp_app_seek_relative(MpApp *app, gint64 delta)
{
	if (!mp_player_is_seekable(app->player))
		return;
	gint64 pos = mp_player_get_position(app->player);
	if (pos < 0)
		return;
	gint64 target = pos + delta;
	if (target < 0)
		target = 0;
	/* Never seek onto the very end: that would end the entry, which is
	 * what Next is for. */
	if (app->duration > 0 && target > app->duration - GST_SECOND / 2)
		target = MAX(0, app->duration - GST_SECOND / 2);
	/* Exact: a step to the nearest key frame can land where it started
	 * (or behind it) when key frames are further apart than the step.
	 * Repeated steps add up, from the target of the one before. */
	mp_player_seek(app->player, target, TRUE);
	show_position(app, target);
	if (app->fullscreen)
		show_controls(app);
}

static void seek_fraction(MpApp *app, double f)
{
	if (app->duration <= 0 || !mp_player_is_seekable(app->player))
		return;
	gint64 target = (gint64)(f * (double)app->duration);
	mp_player_seek(app->player, target, TRUE);
	show_position(app, target);
}

void mp_app_set_speed(MpApp *app, double rate)
{
	MpPriv *pv = priv_of(app);
	if (!(rate >= 0.25 && rate <= 4.0))
		return;
	app->speed = rate;
	mp_player_set_rate(app->player, rate);
	pv->building_menu = TRUE;
	for (guint i = 0; i < N_SPEEDS; i++)
		gtk_check_menu_item_set_active(
			GTK_CHECK_MENU_ITEM(pv->speed_items[i]),
			fabs(speed_steps[i] - rate) < 0.01);
	pv->building_menu = FALSE;
	update_status(app, -1);
}

static void speed_step(MpApp *app, int dir)
{
	guint cur = 2;
	for (guint i = 0; i < N_SPEEDS; i++) {
		if (fabs(speed_steps[i] - app->speed) < 0.01)
			cur = i;
	}
	gint next = (gint)cur + dir;
	if (next < 0 || next >= (gint)N_SPEEDS)
		return;
	mp_app_set_speed(app, speed_steps[next]);
	gchar *msg = g_strdup_printf("Playback speed %.2g×",
				     speed_steps[next]);
	infobar_show(app, GTK_MESSAGE_INFO, msg);
	g_free(msg);
}

/* ---- volume ----------------------------------------------------------- */

static void update_volume_ui(MpApp *app)
{
	double v = app->settings.volume;
	const char *icon;
	if (app->settings.muted || v <= 0.0)
		icon = "audio-volume-muted-symbolic";
	else if (v < 0.34)
		icon = "audio-volume-low-symbolic";
	else if (v < 0.67)
		icon = "audio-volume-medium-symbolic";
	else
		icon = "audio-volume-high-symbolic";
	set_button_icon(app->btn_mute, icon);

	gchar *tip = g_strdup_printf("%s (M) — volume %d %%",
				     app->settings.muted ? "Unmute" : "Mute",
				     (int)lround(v * 100));
	gtk_widget_set_tooltip_text(app->btn_mute, tip);
	g_free(tip);

	app->updating_volume = TRUE;
	gtk_adjustment_set_value(app->volume_adj, v * 100.0);
	app->updating_volume = FALSE;
}

void mp_app_set_volume(MpApp *app, double v)
{
	if (!(v >= 0.0))
		v = 0.0;
	if (v > 1.0)
		v = 1.0;
	app->settings.volume = v;
	mp_player_set_volume(app->player, v);
	/* Turning the volume up is a request to hear something. */
	if (app->settings.muted && v > 0.0) {
		app->settings.muted = FALSE;
		mp_player_set_mute(app->player, FALSE);
	}
	update_volume_ui(app);
	mp_app_schedule_save(app);
}

void mp_app_toggle_mute(MpApp *app)
{
	app->settings.muted = !app->settings.muted;
	mp_player_set_mute(app->player, app->settings.muted);
	update_volume_ui(app);
	mp_app_schedule_save(app);
}

static void on_volume_changed(GtkAdjustment *adj, gpointer data)
{
	MpApp *app = data;
	if (app->updating_volume)
		return;
	mp_app_set_volume(app, gtk_adjustment_get_value(adj) / 100.0);
}

/* ---- shuffle and repeat ----------------------------------------------- */

static void update_mode_buttons(MpApp *app)
{
	MpRepeat r = mp_playlist_get_repeat(app->playlist);
	app->updating_toggles = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->btn_shuffle),
				     mp_playlist_get_shuffle(app->playlist));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->btn_repeat),
				     r != MP_REPEAT_NONE);
	app->updating_toggles = FALSE;
	set_button_icon(app->btn_repeat,
			r == MP_REPEAT_ONE ? "media-playlist-repeat-song-symbolic"
					   : "media-playlist-repeat-symbolic");
	gtk_widget_set_tooltip_text(
		app->btn_repeat,
		r == MP_REPEAT_NONE  ? "Repeat: off (R)"
		: r == MP_REPEAT_ALL ? "Repeat: all (R)"
				     : "Repeat: this one (R)");
	gtk_widget_set_tooltip_text(
		app->btn_shuffle,
		mp_playlist_get_shuffle(app->playlist) ? "Shuffle: on (S)"
						       : "Shuffle: off (S)");
}

static void toggle_shuffle(MpApp *app)
{
	gboolean on = !mp_playlist_get_shuffle(app->playlist);
	mp_playlist_set_shuffle(app->playlist, on);
	app->settings.shuffle = on;
	update_mode_buttons(app);
	mp_app_schedule_save(app);
}

static void cycle_repeat(MpApp *app)
{
	MpRepeat r = mp_playlist_get_repeat(app->playlist);
	r = r == MP_REPEAT_NONE ? MP_REPEAT_ALL
	    : r == MP_REPEAT_ALL ? MP_REPEAT_ONE
				 : MP_REPEAT_NONE;
	mp_playlist_set_repeat(app->playlist, r);
	app->settings.repeat = r;
	update_mode_buttons(app);
	mp_app_schedule_save(app);
}

static void on_shuffle_toggled(GtkToggleButton *b, gpointer data)
{
	MpApp *app = data;
	(void)b;
	if (!app->updating_toggles)
		toggle_shuffle(app);
}

static void on_repeat_toggled(GtkToggleButton *b, gpointer data)
{
	MpApp *app = data;
	(void)b;
	if (!app->updating_toggles)
		cycle_repeat(app);
}

/* ---- queue visibility and fullscreen ---------------------------------- */

static void set_queue_visible(MpApp *app, gboolean visible)
{
	if (gtk_widget_get_visible(app->queue_panel) && !visible)
		capture_geometry(app);
	gtk_widget_set_visible(app->queue_panel, visible);
	if (visible) {
		int ww = gtk_widget_get_allocated_width(app->paned);
		if (ww > app->settings.queue_width + 200)
			gtk_paned_set_position(GTK_PANED(app->paned),
					       ww - app->settings.queue_width);
	}
	app->updating_toggles = TRUE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->btn_playlist),
				     visible);
	gtk_check_menu_item_set_active(
		GTK_CHECK_MENU_ITEM(app->menu_show_playlist), visible);
	app->updating_toggles = FALSE;
}

void mp_app_toggle_playlist(MpApp *app)
{
	if (app->fullscreen)
		mp_app_set_fullscreen(app, FALSE);
	gboolean v = !gtk_widget_get_visible(app->queue_panel);
	set_queue_visible(app, v);
	app->settings.show_playlist = v;
	mp_app_schedule_save(app);
}

static void on_playlist_button(GtkToggleButton *b, gpointer data)
{
	MpApp *app = data;
	(void)b;
	if (!app->updating_toggles)
		mp_app_toggle_playlist(app);
}

static void on_playlist_menu(GtkCheckMenuItem *i, gpointer data)
{
	MpApp *app = data;
	(void)i;
	if (!app->updating_toggles)
		mp_app_toggle_playlist(app);
}

void mp_app_set_fullscreen(MpApp *app, gboolean on)
{
	if (on)
		gtk_window_fullscreen(GTK_WINDOW(app->window));
	else
		gtk_window_unfullscreen(GTK_WINDOW(app->window));
}

/* The window manager has the last word on fullscreen (a key binding of its
 * own, or a refusal), so the layout follows the state it reports rather
 * than the request. */
static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *ev,
				gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	(void)w;
	if (!(ev->changed_mask & GDK_WINDOW_STATE_FULLSCREEN))
		return FALSE;
	gboolean on = (ev->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
	if (on == app->fullscreen)
		return FALSE;

	if (on) {
		capture_geometry(app);
		pv->queue_was_visible = gtk_widget_get_visible(app->queue_panel);
		app->fullscreen = TRUE;
		gtk_widget_hide(app->queue_panel);
		gtk_widget_hide(app->infobar);
		set_button_icon(app->btn_fullscreen, "view-restore-symbolic");
		gtk_widget_set_tooltip_text(app->btn_fullscreen,
					    "Leave full screen (Esc)");
		mp_style_set_fullscreen(app, TRUE);
		show_controls(app);
	} else {
		app->fullscreen = FALSE;
		if (app->hide_timer != 0) {
			g_source_remove(app->hide_timer);
			app->hide_timer = 0;
		}
		gtk_revealer_set_reveal_child(
			GTK_REVEALER(app->controls_revealer), TRUE);
		app->controls_hidden = FALSE;
		set_cursor_visible(app, TRUE);
		mp_style_set_fullscreen(app, FALSE);
		set_button_icon(app->btn_fullscreen, "view-fullscreen-symbolic");
		gtk_widget_set_tooltip_text(app->btn_fullscreen,
					    "Full screen (F)");
		if (pv->queue_was_visible)
			set_queue_visible(app, TRUE);
	}
	mp_player_expose(app->player);
	return FALSE;
}

/* ---- info bar --------------------------------------------------------- */

static void infobar_show(MpApp *app, GtkMessageType type, const char *text)
{
	gtk_info_bar_set_message_type(GTK_INFO_BAR(app->infobar), type);
	gtk_label_set_text(GTK_LABEL(app->infobar_label), text);
	if (!app->fullscreen)
		gtk_widget_show(app->infobar);
}

static void on_infobar_response(GtkInfoBar *bar, gint response, gpointer d)
{
	(void)response;
	(void)d;
	gtk_widget_hide(GTK_WIDGET(bar));
}

/* ---- player callbacks ------------------------------------------------- */

static void cb_state(MpPlayer *p, MpPlayerState st, gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	(void)p;

	update_play_button(app);
	update_status(app, -1);
	start_position_timer(app, st != MP_PLAYER_STOPPED);

	if (app->current_id != 0) {
		MpMark m = st == MP_PLAYER_PLAYING ? MP_MARK_PLAYING
			   : st == MP_PLAYER_STOPPED ? MP_MARK_NONE
						     : MP_MARK_PAUSED;
		if (st == MP_PLAYER_LOADING &&
		    mp_player_wants_playing(app->player))
			m = MP_MARK_PLAYING;
		mp_playlist_set_mark(app->playlist, app->current_id, m);
	}
	if (st == MP_PLAYER_PLAYING) {
		pv->consecutive_errors = 0;
		gtk_widget_queue_draw(app->thumb_area);
	}
	if (app->fullscreen && st != MP_PLAYER_PLAYING)
		show_controls(app);
	show_media_page(app);
	update_gapless_next(app);
}

static void cb_eos(MpPlayer *p, gpointer data)
{
	MpApp *app = data;
	(void)p;
	mp_app_next(app, FALSE);
}

static gchar *join_missing(const char *const *missing)
{
	GString *s = g_string_new(NULL);
	for (int i = 0; missing != NULL && missing[i] != NULL; i++)
		g_string_append_printf(s, "%s• %s", i ? "\n" : "",
				       missing[i]);
	return g_string_free(s, FALSE);
}

static const char *explain_error(const GError *e)
{
	if (e == NULL)
		return "An unknown error occurred.";
	if (e->domain == GST_RESOURCE_ERROR) {
		switch (e->code) {
		case GST_RESOURCE_ERROR_NOT_FOUND:
			return "The file or address does not exist.";
		case GST_RESOURCE_ERROR_NOT_AUTHORIZED:
			return "You are not allowed to open it.";
		case GST_RESOURCE_ERROR_OPEN_READ:
		case GST_RESOURCE_ERROR_OPEN_READ_WRITE:
		case GST_RESOURCE_ERROR_READ:
			return "It could not be read.";
		case GST_RESOURCE_ERROR_BUSY:
			return "The output device is busy.";
		case GST_RESOURCE_ERROR_OPEN_WRITE:
		case GST_RESOURCE_ERROR_WRITE:
			return "The sound or video output could not be "
			       "opened.";
		default:
			break;
		}
	} else if (e->domain == GST_STREAM_ERROR) {
		switch (e->code) {
		case GST_STREAM_ERROR_TYPE_NOT_FOUND:
		case GST_STREAM_ERROR_WRONG_TYPE:
			return "It is not in a format this player "
			       "recognises.";
		case GST_STREAM_ERROR_CODEC_NOT_FOUND:
			return "No decoder for its format is installed.";
		case GST_STREAM_ERROR_DECODE:
		case GST_STREAM_ERROR_DEMUX:
		case GST_STREAM_ERROR_FORMAT:
			return "It is damaged, or uses a variant of its "
			       "format that cannot be decoded.";
		case GST_STREAM_ERROR_DECRYPT:
		case GST_STREAM_ERROR_DECRYPT_NOKEY:
			return "It is encrypted (copy protected).";
		default:
			break;
		}
	} else if (e->domain == GST_CORE_ERROR &&
		   e->code == GST_CORE_ERROR_MISSING_PLUGIN) {
		return "A GStreamer plug-in it needs is not installed.";
	}
	return NULL;
}

static void cb_error(MpPlayer *p, const GError *err, const char *details,
		     const char *const *missing, gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	(void)p;

	guint failed_id = app->current_id;
	if (failed_id != 0)
		mp_playlist_set_failed(app->playlist, failed_id, TRUE);
	pv->consecutive_errors++;

	gchar *title = failed_id ? mp_playlist_get_title(app->playlist,
							 failed_id)
				 : NULL;
	gchar *primary = g_strdup_printf(
		"“%s” cannot be played", title ? title : "This");

	gchar *secondary;
	if (missing != NULL && missing[0] != NULL) {
		gchar *list = join_missing(missing);
		secondary = g_strdup_printf(
			"The following GStreamer plug-ins are needed and are "
			"not installed:\n\n%s",
			list);
		g_free(list);
	} else {
		const char *why = explain_error(err);
		gchar *clean = mp_sanitize_text(err ? err->message : "", 400);
		secondary = why ? g_strdup_printf("%s\n\n%s", why, clean)
				: g_strdup(clean);
		g_free(clean);
	}

	/* An entry reached by itself, in the middle of a queue that goes on,
	 * is skipped with a note instead of stopping everything behind a
	 * dialog -- unless every entry has failed in turn, which means the
	 * problem is not the files. */
	guint count = mp_playlist_count(app->playlist);
	guint next = mp_playlist_peek_next(app->playlist);
	gboolean can_go_on = next != 0 && next != failed_id;
	if (pv->auto_advanced && can_go_on &&
	    pv->consecutive_errors < MAX(count, 1u) &&
	    pv->consecutive_errors < 50) {
		gchar *note = g_strdup_printf("Skipped: %s", primary);
		infobar_show(app, GTK_MESSAGE_WARNING, note);
		g_free(note);
		mp_app_next(app, FALSE);
	} else {
		mp_dialog_error(app, primary, secondary, details,
				can_go_on || count > 1);
		update_status(app, -1);
		show_media_page(app);
	}
	g_free(primary);
	g_free(secondary);
	g_free(title);
}

static void cb_warning(MpPlayer *p, const char *message,
		       const char *const *missing, gpointer data)
{
	MpApp *app = data;
	(void)p;
	(void)message;
	/* Most GStreamer warnings are for developers.  The one a user can act
	 * on is a stream that is skipped for want of a plug-in -- a film
	 * that plays without sound, say. */
	if (missing == NULL || missing[0] == NULL)
		return;
	gchar *list = join_missing(missing);
	gchar *text = g_strdup_printf(
		"Part of this file cannot be played; missing:\n%s", list);
	infobar_show(app, GTK_MESSAGE_WARNING, text);
	g_free(text);
	g_free(list);
}

static void cb_tags(MpPlayer *p, gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	const GstTagList *tl = mp_player_get_tags(p);

	if (app->current_id != 0 && tl != NULL) {
		gchar *title = current_tag(app, GST_TAG_TITLE);
		gchar *artist = current_tag(app, GST_TAG_ARTIST);
		gchar *album = current_tag(app, GST_TAG_ALBUM);
		/* A radio stream's "title" tag is the song on air; the entry
		 * keeps its station name, the now-playing line shows both. */
		if (!mp_player_is_live(p) ||
		    !mp_playlist_get_tagged(app->playlist, app->current_id))
			mp_playlist_set_info(app->playlist, app->current_id,
					     title, artist, album, -1);
		g_free(title);
		g_free(artist);
		g_free(album);
	}

	if (!pv->cover_embedded && tl != NULL &&
	    (gst_tag_list_get_tag_size(tl, GST_TAG_IMAGE) > 0 ||
	     gst_tag_list_get_tag_size(tl, GST_TAG_PREVIEW_IMAGE) > 0)) {
		GdkPixbuf *pb = mp_cover_from_tags(tl);
		if (pb != NULL) {
			pv->cover_embedded = TRUE;
			set_cover(app, pb, g_strdup("embedded in the file"));
		}
	}
	update_now_playing(app);
	if (app->info_dialog != NULL)
		mp_dialog_media_info_refresh(app);
}

static void cb_duration(MpPlayer *p, gint64 d, gpointer data)
{
	MpApp *app = data;
	(void)p;
	set_duration(app, d);
	if (app->current_id != 0 && d > 0)
		mp_playlist_set_info(app->playlist, app->current_id, NULL,
				     NULL, NULL, d);
	mp_app_update_queue_count(app);
}

static void cb_buffering(MpPlayer *p, int pct, gpointer data)
{
	MpApp *app = data;
	(void)p;
	update_status(app, pct);
	update_play_button(app);
}

static void cb_streams(MpPlayer *p, gpointer data)
{
	MpApp *app = data;
	(void)p;
	show_media_page(app);
	if (app->info_dialog != NULL)
		mp_dialog_media_info_refresh(app);
}

static void cb_track_advanced(MpPlayer *p, const char *uri, gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	(void)p;

	/* Move the queue along the same way an end-of-stream would, so the
	 * shuffle position stays right; then check it named what the player
	 * is actually playing. */
	guint id = mp_playlist_next(app->playlist, FALSE);
	gchar *u = id ? mp_playlist_get_uri(app->playlist, id) : NULL;
	if (u == NULL || strcmp(u, uri) != 0) {
		id = pv->gapless_next_id;
		g_free(u);
		u = id ? mp_playlist_get_uri(app->playlist, id) : NULL;
		if (u == NULL || strcmp(u, uri) != 0)
			id = 0;
	}
	g_free(u);
	if (id == 0) {
		/* The entry went away while it was queued for gapless
		 * playback; carry on, just without a queue row. */
		app->current_id = 0;
		mp_playlist_set_current(app->playlist, 0);
		set_cover(app, NULL, NULL);
		update_now_playing(app);
		return;
	}
	pv->auto_advanced = TRUE;
	entry_started(app, id);
}

/* ---- keyboard --------------------------------------------------------- */

static gboolean queue_has_focus(MpApp *app)
{
	return gtk_widget_has_focus(app->queue_view);
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	MpApp *app = data;
	(void)w;
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
	gboolean shift = (mods & GDK_SHIFT_MASK) != 0;
	gboolean alt = (mods & GDK_MOD1_MASK) != 0;
	guint key = gdk_keyval_to_lower(ev->keyval);

	if (alt)
		return FALSE;

	if (ctrl) {
		switch (key) {
		case GDK_KEY_o:
			if (shift)
				mp_dialog_open_folder(app);
			else
				mp_dialog_open_files(app);
			return TRUE;
		case GDK_KEY_l:
			mp_dialog_open_url(app);
			return TRUE;
		case GDK_KEY_s:
			mp_dialog_save_playlist(app);
			return TRUE;
		case GDK_KEY_i:
			mp_dialog_media_info(app);
			return TRUE;
		case GDK_KEY_q:
		case GDK_KEY_w:
			gtk_window_close(GTK_WINDOW(app->window));
			return TRUE;
		case GDK_KEY_Left:
			mp_app_prev(app);
			return TRUE;
		case GDK_KEY_Right:
			mp_app_next(app, TRUE);
			return TRUE;
		case GDK_KEY_a:
			if (queue_has_focus(app))
				return FALSE;	/* select all */
			break;
		default:
			break;
		}
		return FALSE;
	}

	/* Keys the queue needs for itself while it has the focus. */
	if (queue_has_focus(app)) {
		switch (ev->keyval) {
		case GDK_KEY_Up:
		case GDK_KEY_Down:
		case GDK_KEY_Page_Up:
		case GDK_KEY_Page_Down:
		case GDK_KEY_Home:
		case GDK_KEY_End:
		case GDK_KEY_Return:
		case GDK_KEY_KP_Enter:
			return FALSE;
		case GDK_KEY_Delete:
		case GDK_KEY_KP_Delete:
			queue_remove_selected(app);
			return TRUE;
		default:
			break;
		}
	}

	switch (ev->keyval) {
	case GDK_KEY_space:
	case GDK_KEY_AudioPlay:
	case GDK_KEY_AudioPause:
		mp_app_toggle_play(app);
		return TRUE;
	case GDK_KEY_AudioStop:
		mp_app_stop(app);
		return TRUE;
	case GDK_KEY_AudioNext:
		mp_app_next(app, TRUE);
		return TRUE;
	case GDK_KEY_AudioPrev:
		mp_app_prev(app);
		return TRUE;
	case GDK_KEY_AudioMute:
		mp_app_toggle_mute(app);
		return TRUE;
	case GDK_KEY_AudioRaiseVolume:
		mp_app_set_volume(app, app->settings.volume + MP_VOLUME_STEP);
		return TRUE;
	case GDK_KEY_AudioLowerVolume:
		mp_app_set_volume(app, app->settings.volume - MP_VOLUME_STEP);
		return TRUE;
	case GDK_KEY_Left:
	case GDK_KEY_KP_Left:
		mp_app_seek_relative(app, -(shift ? MP_SEEK_LARGE_S
						  : MP_SEEK_SMALL_S) *
						  GST_SECOND);
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_KP_Right:
		mp_app_seek_relative(app, (shift ? MP_SEEK_LARGE_S
						 : MP_SEEK_SMALL_S) *
						  GST_SECOND);
		return TRUE;
	case GDK_KEY_Up:
	case GDK_KEY_KP_Up:
	case GDK_KEY_plus:
	case GDK_KEY_equal:
	case GDK_KEY_KP_Add:
		mp_app_set_volume(app, app->settings.volume + MP_VOLUME_STEP);
		if (app->fullscreen)
			show_controls(app);
		return TRUE;
	case GDK_KEY_Down:
	case GDK_KEY_KP_Down:
	case GDK_KEY_minus:
	case GDK_KEY_KP_Subtract:
		mp_app_set_volume(app, app->settings.volume - MP_VOLUME_STEP);
		if (app->fullscreen)
			show_controls(app);
		return TRUE;
	case GDK_KEY_Home:
		seek_fraction(app, 0.0);
		return TRUE;
	case GDK_KEY_Escape:
		if (app->fullscreen) {
			mp_app_set_fullscreen(app, FALSE);
			return TRUE;
		}
		return FALSE;
	case GDK_KEY_F11:
		mp_app_set_fullscreen(app, !app->fullscreen);
		return TRUE;
	case GDK_KEY_F9:
		mp_app_toggle_playlist(app);
		return TRUE;
	case GDK_KEY_F1:
	case GDK_KEY_question:
		mp_dialog_shortcuts(app);
		return TRUE;
	case GDK_KEY_bracketleft:
		speed_step(app, -1);
		return TRUE;
	case GDK_KEY_bracketright:
		speed_step(app, 1);
		return TRUE;
	case GDK_KEY_BackSpace:
		if (app->speed != 1.0)
			mp_app_set_speed(app, 1.0);
		return TRUE;
	default:
		break;
	}

	/* Letters, case-insensitive. */
	switch (key) {
	case GDK_KEY_n:
		mp_app_next(app, TRUE);
		return TRUE;
	case GDK_KEY_p:
		mp_app_prev(app);
		return TRUE;
	case GDK_KEY_m:
		mp_app_toggle_mute(app);
		return TRUE;
	case GDK_KEY_f:
		mp_app_set_fullscreen(app, !app->fullscreen);
		return TRUE;
	case GDK_KEY_s:
		toggle_shuffle(app);
		return TRUE;
	case GDK_KEY_r:
		cycle_repeat(app);
		return TRUE;
	case GDK_KEY_i:
		mp_dialog_media_info(app);
		return TRUE;
	case GDK_KEY_t:
		app->show_remaining = !app->show_remaining;
		app->settings.show_remaining = app->show_remaining;
		set_time_labels(app, mp_player_get_position(app->player));
		mp_app_schedule_save(app);
		return TRUE;
	default:
		break;
	}

	/* 0..9: jump to that tenth of the entry. */
	if (ev->keyval >= GDK_KEY_0 && ev->keyval <= GDK_KEY_9) {
		seek_fraction(app, (ev->keyval - GDK_KEY_0) / 10.0);
		return TRUE;
	}
	if (ev->keyval >= GDK_KEY_KP_0 && ev->keyval <= GDK_KEY_KP_9) {
		seek_fraction(app, (ev->keyval - GDK_KEY_KP_0) / 10.0);
		return TRUE;
	}
	return FALSE;
}

/* ---- the main menu ---------------------------------------------------- */

typedef struct {
	MpApp *app;
	MpTrackType type;
	int index;
} TrackChoice;

static void on_track_chosen(GtkCheckMenuItem *item, gpointer data)
{
	TrackChoice *c = data;
	if (priv_of(c->app)->building_menu ||
	    !gtk_check_menu_item_get_active(item))
		return;
	if (c->type == MP_TRACK_TEXT && !mp_player_get_subtitles(c->app->player)) {
		mp_player_set_subtitles(c->app->player, TRUE);
		c->app->settings.subtitles = TRUE;
		mp_app_schedule_save(c->app);
	}
	mp_player_set_current_track(c->app->player, c->type, c->index);
}

static void on_subtitles_toggled(GtkCheckMenuItem *item, gpointer data)
{
	MpApp *app = data;
	if (priv_of(app)->building_menu)
		return;
	gboolean on = gtk_check_menu_item_get_active(item);
	mp_player_set_subtitles(app->player, on);
	app->settings.subtitles = on;
	mp_app_schedule_save(app);
}

static GtkWidget *track_submenu(MpApp *app, MpTrackType type,
				GtkWidget *extra_first)
{
	GtkWidget *sub = gtk_menu_new();
	if (extra_first != NULL) {
		gtk_menu_shell_append(GTK_MENU_SHELL(sub), extra_first);
		menu_separator(sub);
	}
	int n = mp_player_get_n_tracks(app->player, type);
	int cur = mp_player_get_current_track(app->player, type);
	GSList *group = NULL;
	if (n == 0) {
		GtkWidget *none = gtk_menu_item_new_with_label(
			type == MP_TRACK_TEXT ? "No subtitles in this file"
					      : "No tracks");
		gtk_widget_set_sensitive(none, FALSE);
		gtk_menu_shell_append(GTK_MENU_SHELL(sub), none);
	}
	for (int i = 0; i < n && i < 64; i++) {
		gchar *label = mp_player_get_track_label(app->player, type, i);
		gchar *clean = mp_sanitize_text(label, 120);
		GtkWidget *item = gtk_radio_menu_item_new_with_label(group,
								     clean);
		group = gtk_radio_menu_item_get_group(
			GTK_RADIO_MENU_ITEM(item));
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
					       i == cur);
		TrackChoice *c = g_new0(TrackChoice, 1);
		c->app = app;
		c->type = type;
		c->index = i;
		g_signal_connect_data(item, "toggled",
				      G_CALLBACK(on_track_chosen), c,
				      (GClosureNotify)(void (*)(void))g_free,
				      0);
		gtk_menu_shell_append(GTK_MENU_SHELL(sub), item);
		g_free(label);
		g_free(clean);
	}
	gtk_widget_show_all(sub);
	return sub;
}

/* A submenu replaced is destroyed outright: detaching alone would leave it
 * (and the per-item data its handlers own) alive as a hidden toplevel. */
static void replace_submenu(GtkWidget *item, GtkWidget *sub)
{
	GtkWidget *old = gtk_menu_item_get_submenu(GTK_MENU_ITEM(item));
	if (old != NULL)
		g_object_ref(old);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
	if (old != NULL) {
		gtk_widget_destroy(old);
		g_object_unref(old);
	}
}

/* The track menus depend on what is loaded, so they are rebuilt each time
 * the menu opens. */
static void on_menu_show(GtkWidget *menu, gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	(void)menu;
	pv->building_menu = TRUE;

	replace_submenu(app->menu_audio_tracks,
			track_submenu(app, MP_TRACK_AUDIO, NULL));

	GtkWidget *show = gtk_check_menu_item_new_with_mnemonic(
		"_Show Subtitles");
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(show),
				       mp_player_get_subtitles(app->player));
	g_signal_connect(show, "toggled", G_CALLBACK(on_subtitles_toggled),
			 app);
	replace_submenu(app->menu_subtitle_tracks,
			track_submenu(app, MP_TRACK_TEXT, show));

	gboolean loaded = mp_player_get_state(app->player) != MP_PLAYER_STOPPED;
	gtk_widget_set_sensitive(app->menu_audio_tracks, loaded);
	gtk_widget_set_sensitive(app->menu_subtitle_tracks,
				 loaded && !app->options->no_video);
	gtk_widget_set_sensitive(app->menu_speed,
				 loaded && mp_player_is_seekable(app->player));
	app->updating_toggles = TRUE;
	gtk_check_menu_item_set_active(
		GTK_CHECK_MENU_ITEM(app->menu_show_playlist),
		gtk_widget_get_visible(app->queue_panel));
	app->updating_toggles = FALSE;
	pv->building_menu = FALSE;
}

static void on_speed_item(GtkCheckMenuItem *item, gpointer data)
{
	MpApp *app = data;
	MpPriv *pv = priv_of(app);
	if (pv->building_menu || !gtk_check_menu_item_get_active(item))
		return;
	for (guint i = 0; i < N_SPEEDS; i++) {
		if (pv->speed_items[i] == GTK_WIDGET(item))
			mp_app_set_speed(app, speed_steps[i]);
	}
}

static void menu_quit(MpApp *app)
{
	gtk_window_close(GTK_WINDOW(app->window));
}

static GtkWidget *build_main_menu(MpApp *app)
{
	MpPriv *pv = priv_of(app);
	GtkWidget *m = gtk_menu_new();

	menu_item(m, "_Open Files…", GDK_KEY_o, GDK_CONTROL_MASK,
		  G_CALLBACK(mp_dialog_open_files), app);
	menu_item(m, "Open _Folder…", GDK_KEY_o,
		  GDK_CONTROL_MASK | GDK_SHIFT_MASK,
		  G_CALLBACK(mp_dialog_open_folder), app);
	menu_item(m, "Open _Location…", GDK_KEY_l, GDK_CONTROL_MASK,
		  G_CALLBACK(mp_dialog_open_url), app);
	menu_separator(m);
	menu_item(m, "Open _Playlist…", 0, 0,
		  G_CALLBACK(mp_dialog_open_playlist), app);
	menu_item(m, "_Save Playlist…", GDK_KEY_s, GDK_CONTROL_MASK,
		  G_CALLBACK(mp_dialog_save_playlist), app);
	menu_separator(m);

	app->menu_audio_tracks = menu_item(m, "_Audio Track", 0, 0, NULL,
					   NULL);
	app->menu_subtitle_tracks = menu_item(m, "S_ubtitles", 0, 0, NULL,
					      NULL);
	app->menu_speed = menu_item(m, "Playback Sp_eed", 0, 0, NULL, NULL);
	GtkWidget *speed = gtk_menu_new();
	GSList *group = NULL;
	for (guint i = 0; i < N_SPEEDS; i++) {
		gchar *label = speed_steps[i] == 1.0
				       ? g_strdup("Normal")
				       : g_strdup_printf("%.2g×",
							 speed_steps[i]);
		GtkWidget *it = gtk_radio_menu_item_new_with_label(group, label);
		group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
		pv->speed_items[i] = it;
		if (speed_steps[i] == 1.0)
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it),
						       TRUE);
		g_signal_connect(it, "toggled", G_CALLBACK(on_speed_item),
				 app);
		gtk_menu_shell_append(GTK_MENU_SHELL(speed), it);
		g_free(label);
	}
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(app->menu_speed), speed);
	menu_separator(m);

	app->menu_show_playlist =
		gtk_check_menu_item_new_with_mnemonic("Show Play _Queue");
	GtkWidget *child = gtk_bin_get_child(GTK_BIN(app->menu_show_playlist));
	if (GTK_IS_ACCEL_LABEL(child))
		gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), GDK_KEY_F9,
					  0);
	g_signal_connect(app->menu_show_playlist, "toggled",
			 G_CALLBACK(on_playlist_menu), app);
	gtk_menu_shell_append(GTK_MENU_SHELL(m), app->menu_show_playlist);
	menu_item(m, "Media _Information", GDK_KEY_i, GDK_CONTROL_MASK,
		  G_CALLBACK(mp_dialog_media_info), app);
	menu_separator(m);
	menu_item(m, "_Keyboard Shortcuts", GDK_KEY_F1, 0,
		  G_CALLBACK(mp_dialog_shortcuts), app);
	menu_item(m, "A_bout Media Player", 0, 0,
		  G_CALLBACK(mp_dialog_about), app);
	menu_separator(m);
	menu_item(m, "_Quit", GDK_KEY_q, GDK_CONTROL_MASK,
		  G_CALLBACK(menu_quit), app);

	g_signal_connect(m, "show", G_CALLBACK(on_menu_show), app);
	gtk_widget_show_all(m);
	return m;
}

static GtkWidget *build_queue_menu(MpApp *app)
{
	GtkWidget *m = gtk_menu_new();
	menu_item(m, "_Play", 0, 0, G_CALLBACK(queue_play_selected), app);
	menu_item(m, "_Remove", GDK_KEY_Delete, 0,
		  G_CALLBACK(queue_remove_selected), app);
	menu_separator(m);
	menu_item(m, "Move _Up", 0, 0, G_CALLBACK(queue_move_up), app);
	menu_item(m, "Move _Down", 0, 0, G_CALLBACK(queue_move_down), app);
	menu_item(m, "_Sort by Title", 0, 0, G_CALLBACK(queue_sort), app);
	menu_separator(m);
	menu_item(m, "_Clear Queue", 0, 0, G_CALLBACK(queue_clear), app);
	gtk_widget_show_all(m);
	return m;
}

/* ---- building the window ---------------------------------------------- */

static GtkWidget *build_media_area(MpApp *app)
{
	app->media_events = gtk_event_box_new();
	gtk_widget_set_hexpand(app->media_events, TRUE);
	gtk_widget_set_vexpand(app->media_events, TRUE);
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(app->media_events),
					 TRUE);
	connect_media_events(app, app->media_events);
	setup_drop_target(app->media_events, app, FALSE);

	app->media_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(app->media_stack),
				      GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_container_add(GTK_CONTAINER(app->media_events), app->media_stack);

	/* The art page. */
	GtkWidget *art = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	add_class(art, "mp-art");
	gtk_widget_set_margin_top(art, 24);
	gtk_widget_set_margin_bottom(art, 16);
	app->art_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(app->art_area, 160, 160);
	gtk_widget_set_vexpand(app->art_area, TRUE);
	g_signal_connect(app->art_area, "draw", G_CALLBACK(on_art_draw), app);
	gtk_box_pack_start(GTK_BOX(art), app->art_area, TRUE, TRUE, 0);

	app->art_title = gtk_label_new("Nothing playing");
	add_class(app->art_title, "mp-art-title");
	gtk_label_set_ellipsize(GTK_LABEL(app->art_title), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(app->art_title), 60);
	gtk_widget_set_margin_top(app->art_title, 12);
	gtk_box_pack_start(GTK_BOX(art), app->art_title, FALSE, FALSE, 0);

	app->art_artist = gtk_label_new("");
	add_class(app->art_artist, "mp-art-sub");
	gtk_label_set_ellipsize(GTK_LABEL(app->art_artist), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(app->art_artist), 80);
	gtk_box_pack_start(GTK_BOX(art), app->art_artist, FALSE, FALSE, 0);

	app->art_status = gtk_label_new("");
	add_class(app->art_status, "mp-art-status");
	gtk_widget_set_margin_top(app->art_status, 6);
	gtk_box_pack_start(GTK_BOX(art), app->art_status, FALSE, FALSE, 0);
	app->art_album = NULL;

	/* The art page needs its own background: the event box's window is
	 * painted by the theme, and the stack draws nothing. */
	GtkWidget *art_bg = gtk_event_box_new();
	add_class(art_bg, "mp-art");
	gtk_container_add(GTK_CONTAINER(art_bg), art);
	gtk_stack_add_named(GTK_STACK(app->media_stack), art_bg, "art");

	app->video_widget = mp_player_get_video_widget(app->player);
	if (app->video_widget != NULL) {
		add_class(app->video_widget, "mp-stage");
		connect_media_events(app, app->video_widget);
		gtk_stack_add_named(GTK_STACK(app->media_stack),
				    app->video_widget, "video");
	}
	return app->media_events;
}

static GtkWidget *build_controls(MpApp *app)
{
	app->controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	add_class(app->controls, "mp-controls");

	/* Row one: the seek bar between the two times. */
	GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	app->time_label = gtk_label_new("0:00");
	add_class(app->time_label, "mp-time");
	gtk_label_set_xalign(GTK_LABEL(app->time_label), 1.0);
	gtk_label_set_width_chars(GTK_LABEL(app->time_label), 7);

	app->seek_adj = gtk_adjustment_new(0, 0, 1, 1, 10, 0);
	app->seek_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,
					app->seek_adj);
	gtk_scale_set_draw_value(GTK_SCALE(app->seek_scale), FALSE);
	gtk_widget_set_hexpand(app->seek_scale, TRUE);
	gtk_widget_set_can_focus(app->seek_scale, FALSE);
	gtk_widget_set_sensitive(app->seek_scale, FALSE);
	add_class(app->seek_scale, "mp-seek");
	g_signal_connect(app->seek_scale, "change-value",
			 G_CALLBACK(on_seek_change_value), app);
	g_signal_connect(app->seek_scale, "button-press-event",
			 G_CALLBACK(on_seek_press), app);
	g_signal_connect(app->seek_scale, "button-release-event",
			 G_CALLBACK(on_seek_release), app);
	g_signal_connect(app->seek_scale, "grab-broken-event",
			 G_CALLBACK(on_seek_grab_broken), app);

	GtkWidget *dur_box = gtk_event_box_new();
	app->duration_label = gtk_label_new("--:--");
	add_class(app->duration_label, "mp-time");
	gtk_label_set_xalign(GTK_LABEL(app->duration_label), 0.0);
	gtk_label_set_width_chars(GTK_LABEL(app->duration_label), 8);
	gtk_container_add(GTK_CONTAINER(dur_box), app->duration_label);
	gtk_widget_set_tooltip_text(dur_box,
				    "Click to show the time remaining (T)");
	g_signal_connect(dur_box, "button-press-event",
			 G_CALLBACK(on_duration_click), app);

	gtk_box_pack_start(GTK_BOX(row1), app->time_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row1), app->seek_scale, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row1), dur_box, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(app->controls), row1, FALSE, FALSE, 0);

	/* Row two: what is playing | transport | volume and view. */
	GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

	GtkWidget *now = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	app->thumb_area = gtk_drawing_area_new();
	gtk_widget_set_size_request(app->thumb_area, 44, 44);
	gtk_widget_set_valign(app->thumb_area, GTK_ALIGN_CENTER);
	g_signal_connect(app->thumb_area, "draw", G_CALLBACK(on_thumb_draw),
			 app);
	GtkWidget *now_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(now_text, GTK_ALIGN_CENTER);
	app->now_title = gtk_label_new("");
	add_class(app->now_title, "mp-now-title");
	app->now_artist = gtk_label_new("");
	add_class(app->now_artist, "mp-now-artist");
	GtkWidget *labels[] = { app->now_title, app->now_artist };
	for (int i = 0; i < 2; i++) {
		gtk_label_set_xalign(GTK_LABEL(labels[i]), 0.0);
		gtk_label_set_ellipsize(GTK_LABEL(labels[i]),
					PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(labels[i]), 28);
		gtk_label_set_width_chars(GTK_LABEL(labels[i]), 12);
		gtk_box_pack_start(GTK_BOX(now_text), labels[i], FALSE, FALSE,
				   0);
	}
	gtk_box_pack_start(GTK_BOX(now), app->thumb_area, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(now), now_text, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row2), now, TRUE, TRUE, 0);

	GtkWidget *transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(transport, 12);
	gtk_widget_set_margin_end(transport, 12);
	app->btn_shuffle = icon_button("media-playlist-shuffle-symbolic",
				       "Shuffle (S)", "mp-round",
				       GTK_ICON_SIZE_BUTTON, TRUE);
	app->btn_prev = icon_button("media-skip-backward-symbolic",
				    "Previous (P)", "mp-round",
				    GTK_ICON_SIZE_BUTTON, FALSE);
	app->btn_play = icon_button("media-playback-start-symbolic",
				    "Play (Space)", "mp-play",
				    GTK_ICON_SIZE_LARGE_TOOLBAR, FALSE);
	app->btn_next = icon_button("media-skip-forward-symbolic", "Next (N)",
				    "mp-round", GTK_ICON_SIZE_BUTTON, FALSE);
	app->btn_repeat = icon_button("media-playlist-repeat-symbolic",
				      "Repeat (R)", "mp-round",
				      GTK_ICON_SIZE_BUTTON, TRUE);
	app->btn_stop = icon_button("media-playback-stop-symbolic", "Stop",
				    "mp-round", GTK_ICON_SIZE_BUTTON, FALSE);
	g_signal_connect(app->btn_shuffle, "toggled",
			 G_CALLBACK(on_shuffle_toggled), app);
	g_signal_connect_swapped(app->btn_prev, "clicked",
				 G_CALLBACK(mp_app_prev), app);
	g_signal_connect_swapped(app->btn_play, "clicked",
				 G_CALLBACK(mp_app_toggle_play), app);
	g_signal_connect_swapped(app->btn_stop, "clicked",
				 G_CALLBACK(mp_app_stop), app);
	g_signal_connect(app->btn_repeat, "toggled",
			 G_CALLBACK(on_repeat_toggled), app);
	gtk_box_pack_start(GTK_BOX(transport), app->btn_shuffle, FALSE, FALSE,
			   0);
	gtk_box_pack_start(GTK_BOX(transport), app->btn_prev, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(transport), app->btn_play, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(transport), app->btn_next, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(transport), app->btn_repeat, FALSE, FALSE,
			   0);
	gtk_box_pack_start(GTK_BOX(transport), app->btn_stop, FALSE, FALSE, 0);
	gtk_box_set_center_widget(GTK_BOX(row2), transport);

	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_halign(right, GTK_ALIGN_END);
	app->btn_mute = icon_button("audio-volume-high-symbolic", "Mute (M)",
				    "mp-round", GTK_ICON_SIZE_BUTTON, FALSE);
	g_signal_connect_swapped(app->btn_mute, "clicked",
				 G_CALLBACK(mp_app_toggle_mute), app);
	app->volume_adj = gtk_adjustment_new(80, 0, 100, 5, 10, 0);
	app->volume_scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL,
					  app->volume_adj);
	gtk_scale_set_draw_value(GTK_SCALE(app->volume_scale), FALSE);
	gtk_widget_set_can_focus(app->volume_scale, FALSE);
	gtk_widget_set_size_request(app->volume_scale, 100, -1);
	gtk_widget_set_valign(app->volume_scale, GTK_ALIGN_CENTER);
	add_class(app->volume_scale, "mp-volume");
	g_signal_connect(app->volume_adj, "value-changed",
			 G_CALLBACK(on_volume_changed), app);

	app->btn_playlist = icon_button("view-list-symbolic",
					"Play queue (F9)", "mp-round",
					GTK_ICON_SIZE_BUTTON, TRUE);
	g_signal_connect(app->btn_playlist, "toggled",
			 G_CALLBACK(on_playlist_button), app);
	app->btn_fullscreen = icon_button("view-fullscreen-symbolic",
					  "Full screen (F)", "mp-round",
					  GTK_ICON_SIZE_BUTTON, FALSE);
	g_signal_connect(app->btn_fullscreen, "clicked",
			 G_CALLBACK(on_fullscreen_clicked), app);

	app->btn_menu = gtk_menu_button_new();
	gtk_container_add(GTK_CONTAINER(app->btn_menu),
			  gtk_image_new_from_icon_name("open-menu-symbolic",
						       GTK_ICON_SIZE_BUTTON));
	gtk_widget_set_tooltip_text(app->btn_menu, "Menu");
	gtk_widget_set_can_focus(app->btn_menu, FALSE);
	gtk_widget_set_focus_on_click(app->btn_menu, FALSE);
	gtk_widget_set_valign(app->btn_menu, GTK_ALIGN_CENTER);
	add_class(app->btn_menu, "mp-round");
	app->menu = build_main_menu(app);
	gtk_menu_button_set_popup(GTK_MENU_BUTTON(app->btn_menu), app->menu);
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(app->btn_menu),
				      GTK_ARROW_UP);

	gtk_box_pack_start(GTK_BOX(right), app->btn_mute, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), app->volume_scale, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), app->btn_playlist, FALSE, FALSE,
			   0);
	gtk_box_pack_start(GTK_BOX(right), app->btn_fullscreen, FALSE, FALSE,
			   0);
	gtk_box_pack_start(GTK_BOX(right), app->btn_menu, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row2), right, TRUE, TRUE, 0);

	gtk_box_pack_start(GTK_BOX(app->controls), row2, FALSE, FALSE, 0);

	app->controls_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(
		GTK_REVEALER(app->controls_revealer),
		GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_revealer_set_transition_duration(
		GTK_REVEALER(app->controls_revealer), 180);
	gtk_container_add(GTK_CONTAINER(app->controls_revealer),
			  app->controls);
	gtk_revealer_set_reveal_child(GTK_REVEALER(app->controls_revealer),
				      TRUE);
	/* Motion over the bar counts as activity in fullscreen too. */
	GtkWidget *ev = gtk_event_box_new();
	gtk_widget_add_events(ev, GDK_POINTER_MOTION_MASK);
	g_signal_connect(ev, "motion-notify-event",
			 G_CALLBACK(on_media_motion), app);
	gtk_container_add(GTK_CONTAINER(ev), app->controls_revealer);
	return ev;
}

static GtkWidget *queue_tool(const char *icon, const char *tip,
			     GCallback cb, MpApp *app)
{
	GtkWidget *b = icon_button(icon, tip, "mp-round", GTK_ICON_SIZE_BUTTON,
				   FALSE);
	g_signal_connect_swapped(b, "clicked", cb, app);
	return b;
}

static GtkWidget *build_queue(MpApp *app)
{
	MpPriv *pv = priv_of(app);
	app->queue_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	add_class(app->queue_panel, "mp-queue");
	gtk_widget_set_size_request(app->queue_panel, 200, -1);

	GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	add_class(hdr, "mp-queue-header");
	GtkWidget *title = gtk_label_new("Play queue");
	add_class(title, "mp-queue-title");
	gtk_label_set_xalign(GTK_LABEL(title), 0.0);
	app->queue_count = gtk_label_new("Empty");
	add_class(app->queue_count, "mp-queue-count");
	gtk_label_set_xalign(GTK_LABEL(app->queue_count), 0.0);
	gtk_box_pack_start(GTK_BOX(hdr), title, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hdr), app->queue_count, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(app->queue_panel), hdr, FALSE, FALSE, 0);

	/* The list. */
	app->queue_view = gtk_tree_view_new_with_model(
		mp_playlist_model(app->playlist));
	GtkTreeView *tv = GTK_TREE_VIEW(app->queue_view);
	gtk_tree_view_set_headers_visible(tv, FALSE);
	/* Letters are player commands (N, P, M, ...) everywhere in the
	 * window, so typing in the list must not start a search. */
	gtk_tree_view_set_enable_search(tv, FALSE);
	gtk_tree_view_set_tooltip_column(tv, MP_COL_TOOLTIP);
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv),
				    GTK_SELECTION_MULTIPLE);

	GtkCellRenderer *r_icon = gtk_cell_renderer_pixbuf_new();
	g_object_set(r_icon, "xpad", 4, NULL);
	gtk_cell_renderer_set_fixed_size(r_icon, 22, -1);
	GtkTreeViewColumn *c_icon = gtk_tree_view_column_new_with_attributes(
		"", r_icon, "icon-name", MP_COL_ICON, NULL);
	gtk_tree_view_append_column(tv, c_icon);

	GtkCellRenderer *r_title = gtk_cell_renderer_text_new();
	g_object_set(r_title, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 3,
		     NULL);
	GtkTreeViewColumn *c_title = gtk_tree_view_column_new();
	gtk_tree_view_column_pack_start(c_title, r_title, TRUE);
	gtk_tree_view_column_set_cell_data_func(c_title, r_title,
						title_cell_data, NULL, NULL);
	gtk_tree_view_column_set_expand(c_title, TRUE);
	gtk_tree_view_append_column(tv, c_title);

	GtkCellRenderer *r_dur = gtk_cell_renderer_text_new();
	g_object_set(r_dur, "xalign", 1.0, "foreground", "#616161",
		     "xpad", 6, NULL);
	GtkTreeViewColumn *c_dur = gtk_tree_view_column_new_with_attributes(
		"", r_dur, "text", MP_COL_DURATION_TEXT, NULL);
	gtk_tree_view_append_column(tv, c_dur);

	g_signal_connect(app->queue_view, "row-activated",
			 G_CALLBACK(on_row_activated), app);
	g_signal_connect(app->queue_view, "button-press-event",
			 G_CALLBACK(on_queue_button), app);

	/* Rows can be dragged within the queue, and files dropped on it. */
	static const GtkTargetEntry row_target[] = {
		{ (gchar *)QUEUE_ROWS_TARGET, GTK_TARGET_SAME_WIDGET,
		  TARGET_QUEUE_ROWS },
	};
	gtk_tree_view_enable_model_drag_source(tv, GDK_BUTTON1_MASK,
					       row_target, 1, GDK_ACTION_COPY);
	setup_drop_target(app->queue_view, app, TRUE);
	g_signal_connect(app->queue_view, "drag-data-get",
			 G_CALLBACK(on_queue_drag_get), app);
	g_signal_connect(app->queue_view, "drag-data-delete",
			 G_CALLBACK(on_queue_drag_delete), app);
	g_signal_connect_after(app->queue_view, "drag-motion",
			       G_CALLBACK(on_queue_drag_motion), app);
	g_signal_connect(app->queue_view, "drag-leave",
			 G_CALLBACK(on_queue_drag_leave), app);

	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(sw), app->queue_view);

	/* What an empty queue shows instead. */
	GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
	GtkWidget *icon = gtk_image_new_from_icon_name("folder-music-symbolic",
						       GTK_ICON_SIZE_DIALOG);
	gtk_widget_set_opacity(icon, 0.5);
	GtkWidget *hint = gtk_label_new(
		"Drop music and videos here,\nor use the buttons below.");
	gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
	add_class(hint, "mp-queue-hint");
	gtk_box_pack_start(GTK_BOX(empty), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(empty), hint, FALSE, FALSE, 0);
	GtkWidget *empty_ev = gtk_event_box_new();
	gtk_container_add(GTK_CONTAINER(empty_ev), empty);
	setup_drop_target(empty_ev, app, FALSE);
	app->queue_empty = empty_ev;

	app->queue_stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(app->queue_stack), empty_ev, "empty");
	gtk_stack_add_named(GTK_STACK(app->queue_stack), sw, "list");
	gtk_box_pack_start(GTK_BOX(app->queue_panel), app->queue_stack, TRUE,
			   TRUE, 0);

	GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	add_class(tools, "mp-queue-tools");
	gtk_box_pack_start(GTK_BOX(tools),
			   queue_tool("list-add-symbolic", "Add files",
				      G_CALLBACK(mp_dialog_add_files), app),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tools),
			   queue_tool("folder-open-symbolic", "Add a folder",
				      G_CALLBACK(mp_dialog_add_folder), app),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tools),
			   queue_tool("insert-link-symbolic",
				      "Add a location (Ctrl+L)",
				      G_CALLBACK(mp_dialog_open_url), app),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tools),
			   queue_tool("list-remove-symbolic",
				      "Remove the selected entries (Delete)",
				      G_CALLBACK(queue_remove_selected), app),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tools),
			   queue_tool("go-up-symbolic", "Move up",
				      G_CALLBACK(queue_move_up), app),
			   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tools),
			   queue_tool("go-down-symbolic", "Move down",
				      G_CALLBACK(queue_move_down), app),
			   FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(tools),
			 queue_tool("edit-clear-all-symbolic", "Clear the queue",
				    G_CALLBACK(queue_clear), app),
			 FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(tools),
			 queue_tool("document-save-symbolic",
				    "Save as a playlist (Ctrl+S)",
				    G_CALLBACK(mp_dialog_save_playlist), app),
			 FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(tools),
			 queue_tool("view-sort-ascending-symbolic",
				    "Sort by title", G_CALLBACK(queue_sort),
				    app),
			 FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(app->queue_panel), tools, FALSE, FALSE, 0);

	pv->queue_menu = build_queue_menu(app);
	gtk_menu_attach_to_widget(GTK_MENU(pv->queue_menu), app->queue_view,
				  NULL);
	return app->queue_panel;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	MpApp *app = data;
	(void)w;
	(void)ev;
	app->quitting = TRUE;
	capture_geometry(app);
	if (app->save_timer != 0) {
		g_source_remove(app->save_timer);
		app->save_timer = 0;
	}
	mp_settings_save(&app->settings);
	/* Stop the sound now, not when the process is finally torn down. */
	mp_player_stop(app->player);
	gtk_main_quit();
	return TRUE;
}

static void discover_done(guint id, const char *title, const char *artist,
			  const char *album, gint64 duration, gpointer data)
{
	MpApp *app = data;
	if (id == 0 || !mp_playlist_get_iter(app->playlist, id, NULL))
		return;
	/* Playback's own tags are fresher; do not overwrite them. */
	if (id == app->current_id &&
	    mp_player_get_state(app->player) != MP_PLAYER_STOPPED)
		return;
	mp_playlist_set_info(app->playlist, id, title, artist, album,
			     duration);
	if (duration >= 0)
		mp_app_update_queue_count(app);
}

static void on_fullscreen_clicked(GtkButton *b, gpointer data)
{
	MpApp *app = data;
	(void)b;
	mp_app_set_fullscreen(app, !app->fullscreen);
}

static void priv_free(gpointer p)
{
	MpPriv *pv = p;
	if (pv->count_timer != 0)
		g_source_remove(pv->count_timer);
	if (pv->drag_seek_timer != 0)
		g_source_remove(pv->drag_seek_timer);
	g_clear_object(&pv->blank_cursor);
	g_free(pv);
}

MpApp *mp_window_create(MpOptions *options)
{
	MpApp *app = g_new0(MpApp, 1);
	app->options = options;
	app->speed = 1.0;
	app->duration = -1;
	app->cancel = g_cancellable_new();
	mp_settings_load(&app->settings);

	/* Command-line choices override the remembered ones for this run;
	 * the shuffle and repeat ones are remembered, as if clicked. */
	if (options->shuffle)
		app->settings.shuffle = TRUE;
	if (options->repeat != NULL) {
		if (g_ascii_strcasecmp(options->repeat, "all") == 0)
			app->settings.repeat = MP_REPEAT_ALL;
		else if (g_ascii_strcasecmp(options->repeat, "one") == 0)
			app->settings.repeat = MP_REPEAT_ONE;
		else
			app->settings.repeat = MP_REPEAT_NONE;
	}
	const char *sink = options->video_sink ? options->video_sink
					       : app->settings.video_sink;
	const char *env_sink = g_getenv("MEDIAPLAYER_VIDEO_SINK");
	if (options->video_sink == NULL && env_sink != NULL && *env_sink)
		sink = env_sink;

	app->playlist = mp_playlist_new();
	mp_playlist_set_shuffle(app->playlist, app->settings.shuffle);
	mp_playlist_set_repeat(app->playlist, app->settings.repeat);

	app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	MpPriv *pv = g_new0(MpPriv, 1);
	g_object_set_data_full(G_OBJECT(app->window), "mp-priv", pv,
			       priv_free);
	pv->blank_cursor = gdk_cursor_new_for_display(
		gtk_widget_get_display(app->window), GDK_BLANK_CURSOR);
	gtk_window_set_title(GTK_WINDOW(app->window), MP_APP_NAME);
	gtk_window_set_icon_name(GTK_WINDOW(app->window), MP_ICON_NAME);
	gtk_window_set_default_size(GTK_WINDOW(app->window),
				    app->settings.width, app->settings.height);

	static const MpPlayerCallbacks cb = {
		.state_changed = cb_state,
		.eos = cb_eos,
		.error = cb_error,
		.warning = cb_warning,
		.tags = cb_tags,
		.duration = cb_duration,
		.buffering = cb_buffering,
		.streams = cb_streams,
		.track_advanced = cb_track_advanced,
	};
	GError *err = NULL;
	app->player = mp_player_new(&cb, app, sink, options->no_video,
				    options->no_audio, &err);
	if (app->player == NULL) {
		mp_dialog_message(NULL, GTK_MESSAGE_ERROR,
				  "Media Player cannot start",
				  err ? err->message : "GStreamer failed.");
		g_clear_error(&err);
		gtk_widget_destroy(app->window);
		mp_playlist_free(app->playlist);
		mp_settings_clear(&app->settings);
		g_object_unref(app->cancel);
		g_free(app);
		return NULL;
	}
	mp_player_set_gapless(app->player, app->settings.gapless);
	mp_player_set_volume(app->player, app->settings.volume);
	mp_player_set_mute(app->player, app->settings.muted);
	mp_player_set_subtitles(app->player, app->settings.subtitles);
	app->show_remaining = app->settings.show_remaining;

	if (app->settings.discover)
		app->discover = mp_discover_new(discover_done, app);
	mp_playlist_set_changed_func(app->playlist, on_playlist_changed, app);

	/* Assemble. */
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	app->infobar = gtk_info_bar_new();
	gtk_info_bar_set_show_close_button(GTK_INFO_BAR(app->infobar), TRUE);
	app->infobar_label = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(app->infobar_label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(app->infobar_label), 0.0);
	gtk_label_set_selectable(GTK_LABEL(app->infobar_label), TRUE);
	gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(
				  GTK_INFO_BAR(app->infobar))),
			  app->infobar_label);
	g_signal_connect(app->infobar, "response",
			 G_CALLBACK(on_infobar_response), app);
	gtk_box_pack_start(GTK_BOX(vbox), app->infobar, FALSE, FALSE, 0);

	app->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(left), build_media_area(app), TRUE, TRUE,
			   0);
	gtk_box_pack_start(GTK_BOX(left), build_controls(app), FALSE, FALSE,
			   0);
	gtk_paned_pack1(GTK_PANED(app->paned), left, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(app->paned), build_queue(app), FALSE, FALSE);
	gtk_paned_set_position(GTK_PANED(app->paned),
			       MAX(320, app->settings.width -
						app->settings.queue_width));
	gtk_box_pack_start(GTK_BOX(vbox), app->paned, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(app->window), vbox);

	g_signal_connect(app->window, "key-press-event",
			 G_CALLBACK(on_key_press), app);
	g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete),
			 app);
	g_signal_connect(app->window, "window-state-event",
			 G_CALLBACK(on_window_state), app);

	gtk_widget_show_all(app->window);
	gtk_widget_hide(app->infobar);
	set_queue_visible(app, app->settings.show_playlist);
	gtk_stack_set_visible_child_name(GTK_STACK(app->media_stack), "art");
	gtk_window_set_focus(GTK_WINDOW(app->window), NULL);

	update_mode_buttons(app);
	update_volume_ui(app);
	update_play_button(app);
	update_status(app, -1);
	count_timeout(app);
	return app;
}

void mp_window_destroy(MpApp *app)
{
	if (app == NULL)
		return;
	g_cancellable_cancel(app->cancel);
	start_position_timer(app, FALSE);
	if (app->hide_timer != 0)
		g_source_remove(app->hide_timer);
	if (app->save_timer != 0)
		g_source_remove(app->save_timer);
	if (app->info_dialog != NULL)
		gtk_widget_destroy(app->info_dialog);
	if (app->error_dialog != NULL)
		gtk_widget_destroy(app->error_dialog);
	mp_discover_free(app->discover);
	/* The player goes before the window: its video widget lives inside
	 * the window, and the sink must stop drawing into it first. */
	mp_player_stop(app->player);
	MpPriv *pv = priv_of(app);
	if (pv->queue_menu != NULL) {
		gtk_widget_destroy(pv->queue_menu);
		pv->queue_menu = NULL;
	}
	gtk_widget_destroy(app->menu);
	gtk_widget_destroy(app->window);
	mp_player_free(app->player);
	mp_playlist_free(app->playlist);
	g_clear_object(&app->cover);
	g_free(app->cover_source);
	mp_settings_clear(&app->settings);
	g_object_unref(app->cancel);
	g_free(app);
}
