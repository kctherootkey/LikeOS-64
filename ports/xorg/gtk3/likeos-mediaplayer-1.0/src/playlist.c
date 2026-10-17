// LikeOS -- Media Player: the play queue and its play order.
//
// Rows live in a GtkListStore; a hash table maps each entry's id to its
// iterator.  That is safe because a GtkListStore's iterators stay valid for
// as long as their row exists, whatever happens to the rows around it
// (GTK_TREE_MODEL_ITERS_PERSIST), and because every removal and every move
// goes through this file -- the queue view's drag and drop is implemented in
// window.c on top of mp_playlist_move_*, not with GtkTreeView's built-in
// reordering, which deletes and re-inserts rows behind our back.
//
// Shuffle keeps a separate order: an array of ids and an index `order_next'
// pointing at the entry that plays next.  Everything before it has been
// played this round, everything from it on has not.  Entries added while
// shuffling are scattered among the unplayed part, so a long shuffle session
// never repeats a track before the others have had their turn.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include <gst/gst.h>

#include "playlist.h"
#include "util.h"

struct MpPlaylist {
	GtkListStore *store;
	GHashTable *rows;	/* id -> GtkTreeIter* */
	guint next_id;
	guint current;
	/* Where the current entry WAS when it was removed, so "next" carries
	 * on from that place rather than jumping back to the start. */
	gint removed_hint;
	gboolean shuffle;
	MpRepeat repeat;
	GArray *order;		/* guint ids, the shuffle order */
	guint order_next;
	MpPlaylistChanged changed;
	gpointer changed_data;
	guint freeze;		/* >0 while a bulk change is in progress */
	gboolean change_pending;
};

static void notify_changed(MpPlaylist *pl)
{
	if (pl->freeze > 0) {
		pl->change_pending = TRUE;
		return;
	}
	pl->change_pending = FALSE;
	if (pl->changed != NULL)
		pl->changed(pl, pl->changed_data);
}

MpPlaylist *mp_playlist_new(void)
{
	MpPlaylist *pl = g_new0(MpPlaylist, 1);
	pl->store = gtk_list_store_new(MP_N_COLS,
				       G_TYPE_UINT,	/* ID */
				       G_TYPE_STRING,	/* URI */
				       G_TYPE_STRING,	/* TITLE */
				       G_TYPE_STRING,	/* ARTIST */
				       G_TYPE_STRING,	/* ALBUM */
				       G_TYPE_INT64,	/* DURATION */
				       G_TYPE_STRING,	/* DURATION_TEXT */
				       G_TYPE_STRING,	/* ICON */
				       G_TYPE_INT,	/* WEIGHT */
				       G_TYPE_STRING,	/* TOOLTIP */
				       G_TYPE_BOOLEAN,	/* TAGGED */
				       G_TYPE_BOOLEAN);	/* FAILED */
	pl->rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
					 (GDestroyNotify)gtk_tree_iter_free);
	pl->next_id = 1;
	pl->removed_hint = -1;
	pl->order = g_array_new(FALSE, FALSE, sizeof(guint));
	pl->repeat = MP_REPEAT_NONE;
	return pl;
}

void mp_playlist_free(MpPlaylist *pl)
{
	if (pl == NULL)
		return;
	g_hash_table_destroy(pl->rows);
	g_array_free(pl->order, TRUE);
	g_object_unref(pl->store);
	g_free(pl);
}

GtkTreeModel *mp_playlist_model(MpPlaylist *pl)
{
	return GTK_TREE_MODEL(pl->store);
}

void mp_playlist_freeze_notify(MpPlaylist *pl)
{
	pl->freeze++;
}

void mp_playlist_thaw_notify(MpPlaylist *pl)
{
	if (pl->freeze > 0 && --pl->freeze == 0 && pl->change_pending)
		notify_changed(pl);
}

void mp_playlist_set_changed_func(MpPlaylist *pl, MpPlaylistChanged func,
				  gpointer user_data)
{
	pl->changed = func;
	pl->changed_data = user_data;
}

gboolean mp_playlist_get_iter(MpPlaylist *pl, guint id, GtkTreeIter *iter)
{
	if (id == 0)
		return FALSE;
	GtkTreeIter *it = g_hash_table_lookup(pl->rows, GUINT_TO_POINTER(id));
	if (it == NULL)
		return FALSE;
	if (iter != NULL)
		*iter = *it;
	return TRUE;
}

guint mp_playlist_id_at(MpPlaylist *pl, GtkTreeIter *iter)
{
	guint id = 0;
	gtk_tree_model_get(GTK_TREE_MODEL(pl->store), iter, MP_COL_ID, &id,
			   -1);
	return id;
}

guint mp_playlist_count(MpPlaylist *pl)
{
	return g_hash_table_size(pl->rows);
}

guint mp_playlist_first(MpPlaylist *pl)
{
	GtkTreeIter it;
	if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(pl->store), &it))
		return 0;
	return mp_playlist_id_at(pl, &it);
}

static guint playlist_last(MpPlaylist *pl)
{
	gint n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(pl->store),
						NULL);
	GtkTreeIter it;
	if (n <= 0 || !gtk_tree_model_iter_nth_child(
			      GTK_TREE_MODEL(pl->store), &it, NULL, n - 1))
		return 0;
	return mp_playlist_id_at(pl, &it);
}

gint mp_playlist_index_of(MpPlaylist *pl, guint id)
{
	GtkTreeIter it;
	if (!mp_playlist_get_iter(pl, id, &it))
		return -1;
	GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(pl->store),
						 &it);
	gint idx = gtk_tree_path_get_indices(p)[0];
	gtk_tree_path_free(p);
	return idx;
}

gchar *mp_playlist_get_uri(MpPlaylist *pl, guint id)
{
	GtkTreeIter it;
	gchar *uri = NULL;
	if (mp_playlist_get_iter(pl, id, &it))
		gtk_tree_model_get(GTK_TREE_MODEL(pl->store), &it, MP_COL_URI,
				   &uri, -1);
	return uri;
}

gchar *mp_playlist_get_title(MpPlaylist *pl, guint id)
{
	GtkTreeIter it;
	gchar *t = NULL;
	if (mp_playlist_get_iter(pl, id, &it))
		gtk_tree_model_get(GTK_TREE_MODEL(pl->store), &it,
				   MP_COL_TITLE, &t, -1);
	return t;
}

gboolean mp_playlist_get_tagged(MpPlaylist *pl, guint id)
{
	GtkTreeIter it;
	gboolean tagged = FALSE;
	if (mp_playlist_get_iter(pl, id, &it))
		gtk_tree_model_get(GTK_TREE_MODEL(pl->store), &it,
				   MP_COL_TAGGED, &tagged, -1);
	return tagged;
}

/* ---- the row's derived columns ---------------------------------------- */

static void update_tooltip(MpPlaylist *pl, GtkTreeIter *it)
{
	gchar *uri = NULL, *title = NULL, *artist = NULL, *album = NULL;
	gboolean failed = FALSE;
	gtk_tree_model_get(GTK_TREE_MODEL(pl->store), it, MP_COL_URI, &uri,
			   MP_COL_TITLE, &title, MP_COL_ARTIST, &artist,
			   MP_COL_ALBUM, &album, MP_COL_FAILED, &failed, -1);

	gchar *where = mp_uri_to_path(uri);
	gchar *where_disp = where ? g_filename_display_name(where)
				  : g_strdup(uri ? uri : "");
	GString *tip = g_string_new(NULL);
	gchar *e = g_markup_escape_text(title ? title : "", -1);
	g_string_append_printf(tip, "<b>%s</b>", e);
	g_free(e);
	if (artist != NULL || album != NULL) {
		gchar *ea = g_markup_escape_text(artist ? artist : "", -1);
		gchar *eb = g_markup_escape_text(album ? album : "", -1);
		g_string_append_printf(tip, "\n%s%s%s", ea,
				       (artist && album) ? " — " : "",
				       eb);
		g_free(ea);
		g_free(eb);
	}
	gchar *ew = g_markup_escape_text(where_disp, -1);
	g_string_append_printf(tip, "\n<small>%s</small>", ew);
	g_free(ew);
	if (failed)
		g_string_append(tip, "\n<i>Could not be played</i>");

	gtk_list_store_set(pl->store, it, MP_COL_TOOLTIP, tip->str, -1);
	g_string_free(tip, TRUE);
	g_free(where);
	g_free(where_disp);
	g_free(uri);
	g_free(title);
	g_free(artist);
	g_free(album);
}

static void set_duration_cols(MpPlaylist *pl, GtkTreeIter *it,
			      gint64 duration)
{
	gchar *text = duration >= 0 ? mp_format_time(duration) : g_strdup("");
	gtk_list_store_set(pl->store, it, MP_COL_DURATION, duration,
			   MP_COL_DURATION_TEXT, text, -1);
	g_free(text);
}

/* ---- shuffle order ---------------------------------------------------- */

static gint order_find(MpPlaylist *pl, guint id)
{
	for (guint i = 0; i < pl->order->len; i++) {
		if (g_array_index(pl->order, guint, i) == id)
			return (gint)i;
	}
	return -1;
}

static void order_remove_at(MpPlaylist *pl, guint idx)
{
	g_array_remove_index(pl->order, idx);
	if (idx < pl->order_next)
		pl->order_next--;
}

/* Scatter a new entry somewhere among the not-yet-played part. */
static void order_add(MpPlaylist *pl, guint id)
{
	guint lo = pl->order_next;
	guint hi = pl->order->len;
	guint pos = lo + (guint)g_random_int_range(0, (gint32)(hi - lo + 1));
	g_array_insert_val(pl->order, pos, id);
}

static void shuffle_array(GArray *a, guint from)
{
	/* Fisher-Yates over a[from..len). */
	for (guint i = a->len; i > from + 1; i--) {
		guint j = from + (guint)g_random_int_range(0, (gint32)(i - from));
		guint t = g_array_index(a, guint, i - 1);
		g_array_index(a, guint, i - 1) = g_array_index(a, guint, j);
		g_array_index(a, guint, j) = t;
	}
}

/* A fresh round: every entry, in random order, with the current one (if
 * any) in front as already played. */
static void order_rebuild(MpPlaylist *pl)
{
	g_array_set_size(pl->order, 0);
	GtkTreeIter it;
	gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(pl->store),
						    &it);
	while (ok) {
		guint id = mp_playlist_id_at(pl, &it);
		if (id != pl->current)
			g_array_append_val(pl->order, id);
		ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(pl->store), &it);
	}
	shuffle_array(pl->order, 0);
	if (pl->current != 0 && mp_playlist_get_iter(pl, pl->current, NULL)) {
		g_array_prepend_val(pl->order, pl->current);
		pl->order_next = 1;
	} else {
		pl->order_next = 0;
	}
}

/* The next round after the last one ran out: reshuffled, but never starting
 * with the entry that just finished (that would sound like a repeat). */
static void order_new_round(MpPlaylist *pl)
{
	guint last = pl->current;
	shuffle_array(pl->order, 0);
	if (pl->order->len > 1 && g_array_index(pl->order, guint, 0) == last) {
		guint j = 1 + (guint)g_random_int_range(
				      0, (gint32)(pl->order->len - 1));
		g_array_index(pl->order, guint, 0) =
			g_array_index(pl->order, guint, j);
		g_array_index(pl->order, guint, j) = last;
	}
	pl->order_next = 0;
}

/* ---- adding and removing ---------------------------------------------- */

guint mp_playlist_insert(MpPlaylist *pl, gint position, const char *uri,
			 const char *title, gint64 duration)
{
	if (uri == NULL || *uri == '\0')
		return 0;
	if (mp_playlist_count(pl) >= MP_PLAYLIST_MAX) {
		g_warning("play queue is full (%d entries); %s not added",
			  MP_PLAYLIST_MAX, uri);
		return 0;
	}

	gchar *shown = (title != NULL && *title != '\0')
			       ? mp_sanitize_text(title, 512)
			       : mp_uri_display_name(uri);
	guint id = pl->next_id++;
	if (pl->next_id == 0)	/* 4 billion entries later */
		pl->next_id = 1;

	GtkTreeIter it;
	gtk_list_store_insert(pl->store, &it, position);
	gtk_list_store_set(pl->store, &it, MP_COL_ID, id, MP_COL_URI, uri,
			   MP_COL_TITLE, shown, MP_COL_ARTIST, NULL,
			   MP_COL_ALBUM, NULL, MP_COL_ICON, NULL,
			   MP_COL_WEIGHT, PANGO_WEIGHT_NORMAL, MP_COL_TAGGED,
			   FALSE, MP_COL_FAILED, FALSE, -1);
	set_duration_cols(pl, &it, duration);
	g_hash_table_insert(pl->rows, GUINT_TO_POINTER(id),
			    gtk_tree_iter_copy(&it));
	update_tooltip(pl, &it);
	g_free(shown);

	if (pl->shuffle)
		order_add(pl, id);
	/* An entry inserted above the removed current's old place moves that
	 * place down by one. */
	if (pl->removed_hint >= 0 && position >= 0 &&
	    position <= pl->removed_hint)
		pl->removed_hint++;

	notify_changed(pl);
	return id;
}

void mp_playlist_remove(MpPlaylist *pl, guint id)
{
	GtkTreeIter it;
	if (!mp_playlist_get_iter(pl, id, &it))
		return;

	gint idx = mp_playlist_index_of(pl, id);
	if (id == pl->current) {
		pl->current = 0;
		pl->removed_hint = idx;
	} else if (pl->removed_hint >= 0 && idx >= 0 &&
		   idx < pl->removed_hint) {
		pl->removed_hint--;
	}

	gint o = order_find(pl, id);
	if (o >= 0)
		order_remove_at(pl, (guint)o);

	/* The hash entry owns a copy of the iterator; drop it before the row
	 * goes, so nothing can look up a dead iterator in between. */
	g_hash_table_remove(pl->rows, GUINT_TO_POINTER(id));
	gtk_list_store_remove(pl->store, &it);
	notify_changed(pl);
}

void mp_playlist_clear(MpPlaylist *pl)
{
	g_hash_table_remove_all(pl->rows);
	gtk_list_store_clear(pl->store);
	g_array_set_size(pl->order, 0);
	pl->order_next = 0;
	pl->current = 0;
	pl->removed_hint = -1;
	notify_changed(pl);
}

void mp_playlist_move(MpPlaylist *pl, guint id, int delta)
{
	GtkTreeIter it, other;
	if (!mp_playlist_get_iter(pl, id, &it))
		return;
	other = it;
	if (delta < 0) {
		if (!gtk_tree_model_iter_previous(GTK_TREE_MODEL(pl->store),
						  &other))
			return;
		gtk_list_store_move_before(pl->store, &it, &other);
	} else {
		if (!gtk_tree_model_iter_next(GTK_TREE_MODEL(pl->store),
					      &other))
			return;
		gtk_list_store_move_after(pl->store, &it, &other);
	}
	/* Moving rows keeps their iterators (the store moves the sequence
	 * node, it does not copy it), so the hash stays valid. */
	notify_changed(pl);
}

void mp_playlist_move_to(MpPlaylist *pl, const guint *ids, guint n,
			 gint index)
{
	GtkTreeModel *m = GTK_TREE_MODEL(pl->store);
	GHashTable *moving = g_hash_table_new(g_direct_hash, g_direct_equal);
	for (guint i = 0; i < n; i++)
		g_hash_table_add(moving, GUINT_TO_POINTER(ids[i]));

	/* The anchor is the first row at or after `index' that is not itself
	 * being moved; everything goes in front of it, in the given order. */
	GtkTreeIter anchor;
	gboolean have_anchor = FALSE;
	if (index >= 0 &&
	    gtk_tree_model_iter_nth_child(m, &anchor, NULL, index)) {
		do {
			guint id = mp_playlist_id_at(pl, &anchor);
			if (!g_hash_table_contains(moving,
						   GUINT_TO_POINTER(id))) {
				have_anchor = TRUE;
				break;
			}
		} while (gtk_tree_model_iter_next(m, &anchor));
	}

	for (guint i = 0; i < n; i++) {
		GtkTreeIter it;
		if (!mp_playlist_get_iter(pl, ids[i], &it))
			continue;
		/* NULL moves to the end of the list. */
		gtk_list_store_move_before(pl->store, &it,
					   have_anchor ? &anchor : NULL);
	}
	g_hash_table_destroy(moving);
	pl->removed_hint = -1;
	notify_changed(pl);
}

typedef struct {
	gint old_pos;
	gchar *key;
} SortItem;

static gint sort_item_cmp(gconstpointer a, gconstpointer b)
{
	const SortItem *x = a, *y = b;
	gint r = strcmp(x->key, y->key);
	return r != 0 ? r : x->old_pos - y->old_pos;
}

void mp_playlist_sort_by_title(MpPlaylist *pl)
{
	GtkTreeModel *m = GTK_TREE_MODEL(pl->store);
	gint n = gtk_tree_model_iter_n_children(m, NULL);
	if (n < 2)
		return;

	SortItem *items = g_new0(SortItem, n);
	GtkTreeIter it;
	gint i = 0;
	gboolean ok = gtk_tree_model_get_iter_first(m, &it);
	while (ok && i < n) {
		gchar *title = NULL, *artist = NULL;
		gtk_tree_model_get(m, &it, MP_COL_TITLE, &title,
				   MP_COL_ARTIST, &artist, -1);
		gchar *joined = g_strdup_printf("%s\x01%s", artist ? artist : "",
						title ? title : "");
		items[i].old_pos = i;
		items[i].key = g_utf8_collate_key_for_filename(joined, -1);
		g_free(joined);
		g_free(title);
		g_free(artist);
		i++;
		ok = gtk_tree_model_iter_next(m, &it);
	}
	n = i;
	qsort(items, (size_t)n, sizeof(SortItem),
	      (int (*)(const void *, const void *))sort_item_cmp);
	gint *new_order = g_new(gint, n);
	for (i = 0; i < n; i++) {
		new_order[i] = items[i].old_pos;
		g_free(items[i].key);
	}
	/* Reordering keeps every iterator valid, as moving does. */
	gtk_list_store_reorder(pl->store, new_order);
	g_free(new_order);
	g_free(items);
	pl->removed_hint = -1;
	notify_changed(pl);
}

gchar **mp_playlist_get_uris(MpPlaylist *pl, gchar ***titles,
			     gint64 **durations)
{
	GtkTreeModel *m = GTK_TREE_MODEL(pl->store);
	gint n = gtk_tree_model_iter_n_children(m, NULL);
	gchar **uris = g_new0(gchar *, n + 1);
	if (titles != NULL)
		*titles = g_new0(gchar *, n + 1);
	if (durations != NULL)
		*durations = g_new0(gint64, n + 1);

	GtkTreeIter it;
	gint i = 0;
	gboolean ok = gtk_tree_model_get_iter_first(m, &it);
	while (ok && i < n) {
		gchar *uri = NULL, *title = NULL;
		gint64 dur = -1;
		gtk_tree_model_get(m, &it, MP_COL_URI, &uri, MP_COL_TITLE,
				   &title, MP_COL_DURATION, &dur, -1);
		uris[i] = uri;
		if (titles != NULL)
			(*titles)[i] = title;
		else
			g_free(title);
		if (durations != NULL)
			(*durations)[i] = dur;
		i++;
		ok = gtk_tree_model_iter_next(m, &it);
	}
	return uris;
}

/* ---- current entry and marks ------------------------------------------ */

static const char *mark_icon(MpMark mark)
{
	switch (mark) {
	case MP_MARK_PLAYING:
		return "media-playback-start-symbolic";
	case MP_MARK_PAUSED:
		return "media-playback-pause-symbolic";
	case MP_MARK_ERROR:
		return "dialog-warning-symbolic";
	case MP_MARK_NONE:
	default:
		return NULL;
	}
}

void mp_playlist_set_mark(MpPlaylist *pl, guint id, MpMark mark)
{
	GtkTreeIter it;
	if (!mp_playlist_get_iter(pl, id, &it))
		return;
	gtk_list_store_set(pl->store, &it, MP_COL_ICON, mark_icon(mark), -1);
}

void mp_playlist_set_current(MpPlaylist *pl, guint id)
{
	GtkTreeIter it;

	if (pl->current != 0 && mp_playlist_get_iter(pl, pl->current, &it)) {
		gboolean failed = FALSE;
		gtk_tree_model_get(GTK_TREE_MODEL(pl->store), &it,
				   MP_COL_FAILED, &failed, -1);
		gtk_list_store_set(pl->store, &it, MP_COL_WEIGHT,
				   PANGO_WEIGHT_NORMAL, MP_COL_ICON,
				   failed ? mark_icon(MP_MARK_ERROR) : NULL,
				   -1);
	}

	if (id != 0 && !mp_playlist_get_iter(pl, id, &it))
		id = 0;
	pl->current = id;
	pl->removed_hint = -1;
	if (id == 0) {
		notify_changed(pl);
		return;
	}
	gtk_list_store_set(pl->store, &it, MP_COL_WEIGHT, PANGO_WEIGHT_BOLD,
			   -1);

	if (pl->shuffle) {
		/* Make the chosen entry the latest played one: take it out of
		 * wherever it was and put it right before order_next. */
		gint o = order_find(pl, id);
		if (o >= 0)
			order_remove_at(pl, (guint)o);
		g_array_insert_val(pl->order, pl->order_next, id);
		pl->order_next++;
	}
	notify_changed(pl);
}

guint mp_playlist_get_current(MpPlaylist *pl)
{
	return pl->current;
}

/* ---- play order ------------------------------------------------------- */

/* Sequential order: the row after `id', or the row at the removed current's
 * old place.  0 at the end. */
static guint sequential_after_current(MpPlaylist *pl)
{
	GtkTreeModel *m = GTK_TREE_MODEL(pl->store);
	GtkTreeIter it;

	if (pl->current != 0 && mp_playlist_get_iter(pl, pl->current, &it)) {
		if (gtk_tree_model_iter_next(m, &it))
			return mp_playlist_id_at(pl, &it);
		return 0;
	}
	if (pl->removed_hint >= 0) {
		if (gtk_tree_model_iter_nth_child(m, &it, NULL,
						  pl->removed_hint))
			return mp_playlist_id_at(pl, &it);
		return 0;
	}
	return mp_playlist_first(pl);
}

guint mp_playlist_peek_next(MpPlaylist *pl)
{
	if (mp_playlist_count(pl) == 0)
		return 0;
	if (pl->repeat == MP_REPEAT_ONE && pl->current != 0)
		return pl->current;

	if (pl->shuffle) {
		if (pl->order_next < pl->order->len)
			return g_array_index(pl->order, guint, pl->order_next);
		return 0;	/* a new round would be shuffled first */
	}

	guint id = sequential_after_current(pl);
	if (id == 0 && pl->repeat == MP_REPEAT_ALL)
		id = mp_playlist_first(pl);
	return id;
}

guint mp_playlist_next(MpPlaylist *pl, gboolean user)
{
	if (mp_playlist_count(pl) == 0)
		return 0;
	if (!user && pl->repeat == MP_REPEAT_ONE && pl->current != 0)
		return pl->current;

	if (pl->shuffle) {
		if (pl->order->len == 0)
			order_rebuild(pl);
		if (pl->order_next >= pl->order->len) {
			/* Round over.  Repeat-all starts a new one; so does
			 * the Next button with repeat-one, which would
			 * otherwise have nowhere to go. */
			if (pl->repeat == MP_REPEAT_NONE)
				return 0;
			order_new_round(pl);
		}
		if (pl->order_next >= pl->order->len)
			return 0;
		/* set_current() will move it before order_next again, which
		 * for this entry is where it already is. */
		return g_array_index(pl->order, guint, pl->order_next);
	}

	guint id = sequential_after_current(pl);
	if (id == 0 && pl->repeat != MP_REPEAT_NONE)
		id = mp_playlist_first(pl);
	return id;
}

guint mp_playlist_prev(MpPlaylist *pl)
{
	if (mp_playlist_count(pl) == 0)
		return 0;

	if (pl->shuffle) {
		/* order[order_next - 1] is the current entry; the one before
		 * it is what played last.  Step back by taking the current
		 * out of the played part again. */
		if (pl->order_next >= 2) {
			pl->order_next -= 2;
			guint id = g_array_index(pl->order, guint,
						 pl->order_next);
			return id;
		}
		return pl->current != 0 ? pl->current : mp_playlist_first(pl);
	}

	GtkTreeIter it;
	if (pl->current != 0 && mp_playlist_get_iter(pl, pl->current, &it)) {
		if (gtk_tree_model_iter_previous(GTK_TREE_MODEL(pl->store),
						 &it))
			return mp_playlist_id_at(pl, &it);
		if (pl->repeat == MP_REPEAT_ALL)
			return playlist_last(pl);
		return pl->current;
	}
	if (pl->removed_hint > 0 &&
	    gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(pl->store), &it, NULL,
					  pl->removed_hint - 1))
		return mp_playlist_id_at(pl, &it);
	return mp_playlist_first(pl);
}

void mp_playlist_set_shuffle(MpPlaylist *pl, gboolean shuffle)
{
	shuffle = !!shuffle;
	if (pl->shuffle == shuffle)
		return;
	pl->shuffle = shuffle;
	if (shuffle) {
		order_rebuild(pl);
	} else {
		g_array_set_size(pl->order, 0);
		pl->order_next = 0;
	}
	notify_changed(pl);
}

gboolean mp_playlist_get_shuffle(MpPlaylist *pl)
{
	return pl->shuffle;
}

void mp_playlist_set_repeat(MpPlaylist *pl, MpRepeat repeat)
{
	if (repeat < MP_REPEAT_NONE || repeat > MP_REPEAT_ONE)
		repeat = MP_REPEAT_NONE;
	pl->repeat = repeat;
	notify_changed(pl);
}

MpRepeat mp_playlist_get_repeat(MpPlaylist *pl)
{
	return pl->repeat;
}

/* ---- metadata --------------------------------------------------------- */

void mp_playlist_set_info(MpPlaylist *pl, guint id, const char *title,
			  const char *artist, const char *album,
			  gint64 duration)
{
	GtkTreeIter it;
	if (!mp_playlist_get_iter(pl, id, &it))
		return;

	if (title != NULL && *title != '\0') {
		gchar *t = mp_sanitize_text(title, 512);
		gtk_list_store_set(pl->store, &it, MP_COL_TITLE, t,
				   MP_COL_TAGGED, TRUE, -1);
		g_free(t);
	}
	if (artist != NULL && *artist != '\0') {
		gchar *a = mp_sanitize_text(artist, 512);
		gtk_list_store_set(pl->store, &it, MP_COL_ARTIST, a, -1);
		g_free(a);
	}
	if (album != NULL && *album != '\0') {
		gchar *a = mp_sanitize_text(album, 512);
		gtk_list_store_set(pl->store, &it, MP_COL_ALBUM, a, -1);
		g_free(a);
	}
	if (duration >= 0)
		set_duration_cols(pl, &it, duration);
	update_tooltip(pl, &it);
}

void mp_playlist_set_failed(MpPlaylist *pl, guint id, gboolean failed)
{
	GtkTreeIter it;
	if (!mp_playlist_get_iter(pl, id, &it))
		return;
	gtk_list_store_set(pl->store, &it, MP_COL_FAILED, failed, -1);
	if (failed)
		gtk_list_store_set(pl->store, &it, MP_COL_ICON,
				   mark_icon(MP_MARK_ERROR), -1);
	update_tooltip(pl, &it);
}
