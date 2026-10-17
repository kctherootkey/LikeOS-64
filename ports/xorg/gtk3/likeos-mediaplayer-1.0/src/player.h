// LikeOS -- Media Player: the GStreamer engine.
//
// One playbin for the life of the program, reused for every entry.  The
// interface below hides GStreamer's asynchronous state machine behind a
// handful of calls and callbacks; every callback runs on the GTK main loop,
// never on a streaming thread.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_PLAYER_H
#define MP_PLAYER_H

#include <gtk/gtk.h>
#include <gst/gst.h>

typedef struct MpPlayer MpPlayer;

typedef enum {
	MP_PLAYER_STOPPED,	/* nothing loaded, or stopped */
	MP_PLAYER_LOADING,	/* opening, prerolling or buffering */
	MP_PLAYER_PAUSED,
	MP_PLAYER_PLAYING,
} MpPlayerState;

typedef enum {
	MP_TRACK_AUDIO,
	MP_TRACK_VIDEO,
	MP_TRACK_TEXT,
} MpTrackType;

typedef struct {
	void (*state_changed)(MpPlayer *p, MpPlayerState state, gpointer data);
	void (*eos)(MpPlayer *p, gpointer data);
	/* `missing' lists plug-ins GStreamer asked for and did not find, as
	 * human-readable descriptions; NULL-terminated, may be empty. */
	void (*error)(MpPlayer *p, const GError *error, const char *details,
		      const char *const *missing, gpointer data);
	void (*warning)(MpPlayer *p, const char *message,
			const char *const *missing, gpointer data);
	void (*tags)(MpPlayer *p, gpointer data);
	/* The duration or whether the entry can be seeked changed; the
	 * latter is mp_player_is_seekable().  The two are learnt at different
	 * moments, so a seek bar must follow both through this callback. */
	void (*duration)(MpPlayer *p, gint64 duration, gpointer data);
	void (*buffering)(MpPlayer *p, int percent, gpointer data);
	/* The number or choice of audio/video/subtitle streams changed. */
	void (*streams)(MpPlayer *p, gpointer data);
	/* Gapless playback moved on to the URI set with
	 * mp_player_set_next_uri() without stopping. */
	void (*track_advanced)(MpPlayer *p, const char *uri, gpointer data);
} MpPlayerCallbacks;

typedef struct {
	int width, height;	/* 0 when there is no video */
	int fps_n, fps_d;
	int par_n, par_d;
	int audio_rate, audio_channels;
	gchar *video_codec;
	gchar *audio_codec;
	gchar *container;
	gchar *subtitle_codec;
	guint bitrate;		/* bits per second, 0 when unknown */
	gchar *video_sink;	/* element the video goes to */
	gchar *audio_sink;	/* element the audio goes to */
} MpStreamInfo;

/* `video_sink' is "auto", "xv", "gtk", "x11" or "gl"; `no_video' turns
 * video decoding off altogether, `no_audio' the sound.  Returns NULL (and sets `error') when
 * playbin itself is missing, which means GStreamer's base plug-ins are. */
MpPlayer *mp_player_new(const MpPlayerCallbacks *cb, gpointer data,
			const char *video_sink, gboolean no_video,
			gboolean no_audio, GError **error);
void mp_player_free(MpPlayer *p);

/* The widget the video appears in; NULL with no_video.  It belongs to the
 * player and must be packed exactly once. */
GtkWidget *mp_player_get_video_widget(MpPlayer *p);
const char *mp_player_get_video_sink_name(MpPlayer *p);

void mp_player_load(MpPlayer *p, const char *uri, gboolean play);
const char *mp_player_get_uri(MpPlayer *p);
void mp_player_play(MpPlayer *p);
void mp_player_pause(MpPlayer *p);
void mp_player_stop(MpPlayer *p);
MpPlayerState mp_player_get_state(MpPlayer *p);
/* TRUE when the player is playing or on its way to (loading, buffering):
 * what a play/pause button should offer to undo. */
gboolean mp_player_wants_playing(MpPlayer *p);

/* For gapless playback: what to switch to when the current stream is
 * about to end.  NULL clears it (the stream then ends with an EOS). */
void mp_player_set_next_uri(MpPlayer *p, const char *uri);
void mp_player_set_gapless(MpPlayer *p, gboolean gapless);

/* Seek to `position' (ns).  `accurate' lands exactly there; otherwise on
 * the nearest key frame, which is cheap and meant for following a dragged
 * seek bar.  Seeks are coalesced: while one is being carried out, a newer
 * request replaces any waiting one and is issued when the first is done,
 * so a burst of requests never queues up work in the pipeline. */
gboolean mp_player_seek(MpPlayer *p, gint64 position, gboolean accurate);
/* The playback position (ns), or -1.  While a seek is under way this is
 * the position asked for, so the display does not jump back meanwhile and
 * relative seeks add up. */
gint64 mp_player_get_position(MpPlayer *p);
gint64 mp_player_get_duration(MpPlayer *p);
gboolean mp_player_is_seekable(MpPlayer *p);
gboolean mp_player_is_live(MpPlayer *p);
void mp_player_set_rate(MpPlayer *p, double rate);
double mp_player_get_rate(MpPlayer *p);

/* Volume on the cubic scale (what a slider should show), 0 .. 1. */
void mp_player_set_volume(MpPlayer *p, double volume);
double mp_player_get_volume(MpPlayer *p);
void mp_player_set_mute(MpPlayer *p, gboolean mute);
gboolean mp_player_get_mute(MpPlayer *p);

gboolean mp_player_has_video(MpPlayer *p);
gboolean mp_player_has_audio(MpPlayer *p);
/* The merged tags of the current entry (container and streams), or NULL.
 * Not a new reference. */
const GstTagList *mp_player_get_tags(MpPlayer *p);
void mp_player_get_stream_info(MpPlayer *p, MpStreamInfo *info);
void mp_stream_info_clear(MpStreamInfo *info);

int mp_player_get_n_tracks(MpPlayer *p, MpTrackType type);
int mp_player_get_current_track(MpPlayer *p, MpTrackType type);
gchar *mp_player_get_track_label(MpPlayer *p, MpTrackType type, int index);
void mp_player_set_current_track(MpPlayer *p, MpTrackType type, int index);
void mp_player_set_subtitles(MpPlayer *p, gboolean on);
gboolean mp_player_get_subtitles(MpPlayer *p);

/* Redraw the last video frame (the window was exposed or resized while
 * paused). */
void mp_player_expose(MpPlayer *p);

#endif
