// LikeOS -- Media Player: small helpers shared by every module.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_UTIL_H
#define MP_UTIL_H

#include <glib.h>
#include <gst/gst.h>

/* "1:02:03" or "2:03"; "--:--" for an unknown time. */
gchar *mp_format_time(gint64 ns);
/* The same, with a leading "-" (time remaining). */
gchar *mp_format_remaining(gint64 position, gint64 duration);

/* A command-line argument, a dropped string or a playlist line, made into a
 * URI.  Relative paths resolve against `base_dir' (the working directory when
 * NULL).  Returns NULL for an empty or unusable string. */
gchar *mp_uri_from_arg(const char *arg, const char *base_dir);

/* What to show for an entry nobody has read tags from yet: the file name
 * without its extension for a local file, the last path component (or the
 * host) for a stream. */
gchar *mp_uri_display_name(const char *uri);

/* The local file a file:// URI names, or NULL for any other scheme. */
gchar *mp_uri_to_path(const char *uri);

/* Whether a file name looks like something GStreamer could play.  Decided by
 * extension first and then by the content type the name suggests; never
 * opens the file. */
gboolean mp_is_media_name(const char *name);

/* The extensions mp_is_media_name() knows, lower case, NULL-terminated --
 * for building file chooser filters. */
const char *const *mp_media_extensions(void);

/* Whether a file name is a playlist this program reads (M3U, M3U8, PLS). */
gboolean mp_is_playlist_name(const char *name);

/* Whether a URI's scheme is one a source element is installed for. */
gboolean mp_uri_is_playable_scheme(const char *uri);

/* Whether the URI is a local file (the only kind folder covers and metadata
 * discovery are tried for). */
gboolean mp_uri_is_local(const char *uri);

/* Ordering for file names the way people read them: "Track 2" before
 * "Track 10", case folded. */
gint mp_compare_filenames(const char *a, const char *b);

/* Strip control characters and cap the length of a string that came from a
 * file or a stream before it reaches a label. */
gchar *mp_sanitize_text(const char *s, gsize max_chars);

#endif
