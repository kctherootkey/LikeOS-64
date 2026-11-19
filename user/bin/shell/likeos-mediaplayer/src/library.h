// LikeOS -- LikeOS Media Player: the music and video library.
//
// Every media file under the library folders (Music and Videos unless the
// user chose others), with what its tags say about it, in one list store
// the library pages filter and sort.  Albums and artists are derived from
// it.  What was read is remembered in ~/.cache/mediaplayer/library, so a
// start-up shows the library at once and only new or changed files are
// read again.
//
// The "recent media" list Home shows lives here as well.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_LIBRARY_H
#define MP_LIBRARY_H

#include <gtk/gtk.h>

enum {
	MP_LIB_ID,		/* guint, unique for the run */
	MP_LIB_URI,		/* gchararray */
	MP_LIB_TITLE,		/* gchararray */
	MP_LIB_ARTIST,		/* gchararray, "" when unknown */
	MP_LIB_ALBUM,		/* gchararray, "" when unknown */
	MP_LIB_DURATION,	/* gint64 ns, -1 when unknown */
	MP_LIB_DURATION_TEXT,	/* gchararray */
	MP_LIB_VIDEO,		/* gboolean */
	MP_LIB_TITLE_KEY,	/* gchararray: collation keys, for sorting */
	MP_LIB_ARTIST_KEY,
	MP_LIB_ALBUM_KEY,
	MP_LIB_GROUP,		/* gchararray: the album this belongs to */
	MP_LIB_MTIME,		/* gint64, seconds */
	MP_LIB_ADDED,		/* gint64, when it was first seen */
	MP_LIB_TRACK,		/* guint, number on the album, 0 unknown */
	MP_LIB_N_COLS
};

typedef struct {
	gchar *key;		/* MP_LIB_GROUP of its tracks */
	gchar *title;		/* "Unknown album" when the tags say nothing */
	gchar *artist;
	gchar *first_uri;	/* a track, for the folder cover */
	guint tracks;
	gint64 total;		/* ns */
	gint64 added;		/* newest track's MP_LIB_ADDED */
	GdkPixbuf *cover;
	gboolean cover_tried;
} MpAlbum;

typedef struct {
	gchar *key;		/* MP_LIB_ARTIST_KEY */
	gchar *name;
	guint tracks;
	gchar *album;		/* an album key of theirs, for a picture */
} MpArtist;

typedef struct {
	gchar *uri;
	gchar *title;
	gchar *artist;
	gboolean video;
} MpRecent;

typedef struct MpLibrary MpLibrary;

/* `changed' runs (debounced) when entries were added, removed or got their
 * tags; `picture' when a cover or a video picture arrived for `uri' (NULL:
 * an album cover). */
typedef void (*MpLibraryChanged)(MpLibrary *lib, gpointer data);
typedef void (*MpLibraryPicture)(MpLibrary *lib, const char *uri,
				 gpointer data);

MpLibrary *mp_library_new(MpLibraryChanged changed,
			  MpLibraryPicture picture, gpointer data);
void mp_library_free(MpLibrary *lib);

/* Read the folders (again).  `retag' reads every file's tags again, not
 * just those of new or changed files. */
void mp_library_scan(MpLibrary *lib, gchar **music_dirs, gchar **video_dirs,
		     gboolean retag);
gboolean mp_library_busy(MpLibrary *lib);

GtkListStore *mp_library_store(MpLibrary *lib);
guint mp_library_count(MpLibrary *lib, gboolean video);

/* Albums sorted by title, artists by name; arrays of borrowed pointers,
 * valid until the next `changed'. */
GPtrArray *mp_library_albums(MpLibrary *lib);
GPtrArray *mp_library_artists(MpLibrary *lib);
MpAlbum *mp_library_album(MpLibrary *lib, const char *key);
MpArtist *mp_library_artist(MpLibrary *lib, const char *key);

/* Pictures: borrowed, NULL when there is none (yet). */
GdkPixbuf *mp_library_album_cover(MpLibrary *lib, MpAlbum *album);
GdkPixbuf *mp_library_video_picture(MpLibrary *lib, const char *uri);
/* The picture that stands for a URI: its album's cover or its video
 * picture. */
GdkPixbuf *mp_library_picture_for(MpLibrary *lib, const char *uri);
/* What the library knows about a URI; FALSE when it is not in it.  The
 * strings are new copies. */
gboolean mp_library_lookup(MpLibrary *lib, const char *uri, gchar **title,
			   gchar **artist, gchar **album, gint64 *duration);

/* Recently played, newest first (borrowed). */
GPtrArray *mp_library_recent(MpLibrary *lib);
void mp_library_add_recent(MpLibrary *lib, const char *uri,
			   const char *title, const char *artist,
			   gboolean video);
void mp_library_clear_recent(MpLibrary *lib);

#endif
