// LikeOS -- Media Player: the play queue.
//
// A GtkListStore the queue view shows directly, plus the play order.  Every
// entry carries a unique id that never changes and is never reused, so the
// "current" entry survives the user reordering, sorting or deleting rows
// around it -- a row path or an iterator would not.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_PLAYLIST_H
#define MP_PLAYLIST_H

#include <gtk/gtk.h>

typedef enum {
	MP_REPEAT_NONE = 0,
	MP_REPEAT_ALL = 1,
	MP_REPEAT_ONE = 2,
} MpRepeat;

enum {
	MP_COL_ID,		/* guint, unique */
	MP_COL_URI,		/* gchararray */
	MP_COL_TITLE,		/* gchararray, what the row shows */
	MP_COL_ARTIST,		/* gchararray, may be NULL */
	MP_COL_ALBUM,		/* gchararray, may be NULL */
	MP_COL_DURATION,	/* gint64 ns, -1 when unknown */
	MP_COL_DURATION_TEXT,	/* gchararray */
	MP_COL_ICON,		/* gchararray icon name: playing/paused mark */
	MP_COL_WEIGHT,		/* gint PangoWeight, bold for the current */
	MP_COL_TOOLTIP,		/* gchararray, markup */
	MP_COL_TAGGED,		/* gboolean: title came from tags */
	MP_COL_FAILED,		/* gboolean: last attempt to play failed */
	MP_N_COLS
};

typedef enum {
	MP_MARK_NONE,
	MP_MARK_PLAYING,
	MP_MARK_PAUSED,
	MP_MARK_ERROR,
} MpMark;

/* Hard ceiling on queue length: a folder of a hundred thousand files is a
 * mistake, not a playlist, and every entry costs memory and a row. */
#define MP_PLAYLIST_MAX 50000

typedef struct MpPlaylist MpPlaylist;

typedef void (*MpPlaylistChanged)(MpPlaylist *pl, gpointer user_data);

MpPlaylist *mp_playlist_new(void);
void mp_playlist_free(MpPlaylist *pl);
GtkTreeModel *mp_playlist_model(MpPlaylist *pl);

/* Called after anything that changes what "next" would return: entries
 * added, removed or moved, shuffle or repeat changed. */
void mp_playlist_set_changed_func(MpPlaylist *pl, MpPlaylistChanged func,
				  gpointer user_data);

/* Hold back change notifications during a bulk insert; the thaw sends one
 * if anything changed in between.  Nests. */
void mp_playlist_freeze_notify(MpPlaylist *pl);
void mp_playlist_thaw_notify(MpPlaylist *pl);

/* Insert an entry at `position' (-1 appends).  `title' may be NULL, in which
 * case the URI's display name is shown until tags arrive.  Returns the new
 * id, or 0 when the queue is full. */
guint mp_playlist_insert(MpPlaylist *pl, gint position, const char *uri,
			 const char *title, gint64 duration);
void mp_playlist_remove(MpPlaylist *pl, guint id);
void mp_playlist_clear(MpPlaylist *pl);
/* Move one entry up (-1) or down (+1). */
void mp_playlist_move(MpPlaylist *pl, guint id, int delta);
/* Move a group of entries, keeping their relative order, so that they sit
 * before what is now row `index' (-1 or past the end: to the end). */
void mp_playlist_move_to(MpPlaylist *pl, const guint *ids, guint n,
			 gint index);
void mp_playlist_sort_by_title(MpPlaylist *pl);

guint mp_playlist_count(MpPlaylist *pl);
gboolean mp_playlist_get_iter(MpPlaylist *pl, guint id, GtkTreeIter *iter);
guint mp_playlist_id_at(MpPlaylist *pl, GtkTreeIter *iter);
guint mp_playlist_first(MpPlaylist *pl);
/* Newly allocated; NULL for an unknown id. */
gchar *mp_playlist_get_uri(MpPlaylist *pl, guint id);
gchar *mp_playlist_get_title(MpPlaylist *pl, guint id);
gint mp_playlist_index_of(MpPlaylist *pl, guint id);
/* Every entry in queue order, as a NULL-terminated array of URIs. */
gchar **mp_playlist_get_uris(MpPlaylist *pl, gchar ***titles,
			     gint64 **durations);

/* The entry being played; the order and the "playing" mark follow it. */
void mp_playlist_set_current(MpPlaylist *pl, guint id);
guint mp_playlist_get_current(MpPlaylist *pl);
void mp_playlist_set_mark(MpPlaylist *pl, guint id, MpMark mark);

/* What comes after the current entry.  `user' is TRUE for the Next button,
 * which moves on even under repeat-one; FALSE when a track ended by itself.
 * Moves the shuffle position; returns 0 when playback should stop. */
guint mp_playlist_next(MpPlaylist *pl, gboolean user);
guint mp_playlist_prev(MpPlaylist *pl);
/* What mp_playlist_next(pl, FALSE) WOULD return, without moving anything --
 * for gapless playback, which has to name the next stream before the current
 * one ends.  Returns 0 where the answer depends on a reshuffle. */
guint mp_playlist_peek_next(MpPlaylist *pl);

void mp_playlist_set_shuffle(MpPlaylist *pl, gboolean shuffle);
gboolean mp_playlist_get_shuffle(MpPlaylist *pl);
void mp_playlist_set_repeat(MpPlaylist *pl, MpRepeat repeat);
MpRepeat mp_playlist_get_repeat(MpPlaylist *pl);

/* Fill in what tags or the discoverer found.  NULL / -1 leave a field. */
void mp_playlist_set_info(MpPlaylist *pl, guint id, const char *title,
			  const char *artist, const char *album,
			  gint64 duration);
void mp_playlist_set_failed(MpPlaylist *pl, guint id, gboolean failed);
gboolean mp_playlist_get_tagged(MpPlaylist *pl, guint id);

/* playlist_io.c: M3U/M3U8/PLS.  Load returns the URIs (and titles where
 * the file has them) in order; save writes an extended M3U in UTF-8. */
gboolean mp_playlist_file_load(const char *path, GPtrArray **uris,
			       GPtrArray **titles, GError **error);
gboolean mp_playlist_file_save(MpPlaylist *pl, const char *path,
			       GError **error);

#endif
