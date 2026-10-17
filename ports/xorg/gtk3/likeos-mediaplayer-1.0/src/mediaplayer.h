// LikeOS -- Media Player: the application state every module shares.
//
// The program is split by concern -- player.c drives GStreamer, playlist.c
// owns the play queue, window.c builds and runs the interface, dialogs.c holds
// the secondary windows -- and they all meet in one MpApp.  There is exactly
// one per process, created by main() and torn down when the window closes.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MEDIAPLAYER_H
#define MEDIAPLAYER_H

#include <gtk/gtk.h>
#include <gst/gst.h>

#include "discover.h"
#include "player.h"
#include "playlist.h"
#include "settings.h"

#define MP_APP_NAME "Media Player"
#define MP_APP_ID "mediaplayer"
#define MP_ICON_NAME "multimedia-player"

#ifndef MP_VERSION
#define MP_VERSION "1.0"
#endif

/* How often the seek bar and the time labels follow the stream. */
#define MP_POSITION_INTERVAL_MS 200
/* How long the pointer has to rest before fullscreen hides the controls. */
#define MP_FULLSCREEN_HIDE_MS 2500
/* Seek and volume steps for the keyboard and the scroll wheel. */
#define MP_SEEK_SMALL_S 5
#define MP_SEEK_LARGE_S 60
#define MP_VOLUME_STEP 0.05
/* "Previous" restarts the current track when it has played this long. */
#define MP_PREV_RESTART_NS (3 * GST_SECOND)

typedef struct {
	/* Options from the command line, applied once at start-up. */
	gboolean start_fullscreen;
	gboolean no_video;
	gboolean no_audio;	/* --no-audio: no sound decoded or played */
	gboolean shuffle;	/* --shuffle given */
	gchar *repeat;		/* --repeat=MODE, NULL when not given */
	gchar *video_sink;	/* --video-sink=NAME, NULL when not given */
	gchar **files;		/* the remaining arguments */
} MpOptions;

typedef struct {
	/* ---- top level ---- */
	GtkWidget *window;
	GtkWidget *paned;	/* media area | play queue */
	GtkWidget *infobar;
	GtkWidget *infobar_label;

	/* ---- media area ---- */
	GtkWidget *media_events; /* event box around the stack */
	GtkWidget *media_stack;	 /* "video" or "art" */
	GtkWidget *video_widget; /* whatever the player renders into */
	GtkWidget *art_area;	 /* cover art, or the placeholder */
	GtkWidget *art_title;
	GtkWidget *art_artist;
	GtkWidget *art_album;
	GtkWidget *art_status;	 /* "Buffering 40 %", "Stopped" */

	/* ---- control bar ---- */
	GtkWidget *controls_revealer;
	GtkWidget *controls;
	GtkWidget *seek_scale;
	GtkAdjustment *seek_adj;
	GtkWidget *time_label;
	GtkWidget *duration_label;
	GtkWidget *thumb_area;	/* small cover beside the title */
	GtkWidget *now_title;
	GtkWidget *now_artist;
	GtkWidget *btn_shuffle;
	GtkWidget *btn_prev;
	GtkWidget *btn_play;
	GtkWidget *btn_stop;
	GtkWidget *btn_next;
	GtkWidget *btn_repeat;
	GtkWidget *btn_mute;
	GtkWidget *volume_scale;
	GtkAdjustment *volume_adj;
	GtkWidget *btn_playlist;
	GtkWidget *btn_fullscreen;
	GtkWidget *btn_menu;
	GtkWidget *menu;
	GtkWidget *menu_show_playlist;
	GtkWidget *menu_subtitles;
	GtkWidget *menu_audio_tracks;	 /* submenu item */
	GtkWidget *menu_subtitle_tracks; /* submenu item */
	GtkWidget *menu_speed;		 /* submenu item */

	/* ---- play queue ---- */
	GtkWidget *queue_panel;
	GtkWidget *queue_view;
	GtkWidget *queue_count;
	GtkWidget *queue_empty;	/* "Drop files here" hint */
	GtkWidget *queue_stack;

	/* ---- engine and model ---- */
	MpPlayer *player;
	MpPlaylist *playlist;
	MpSettings settings;
	MpOptions *options;
	MpDiscover *discover;
	GCancellable *cancel;	/* cancelled when the window goes */
	guint scans_running;

	/* ---- state ---- */
	guint current_id;	/* playlist entry that is loaded, 0 = none */
	gint64 duration;	/* of the loaded entry, -1 when unknown */
	gboolean fullscreen;
	gboolean user_seeking;	/* the seek bar is being dragged */
	gboolean updating_seek;	/* we are moving the seek bar ourselves */
	gboolean updating_volume;
	gboolean updating_toggles;
	gboolean controls_hidden;
	gboolean show_remaining;
	gboolean quitting;
	gint64 last_drag_seek;	/* monotonic time of the last seek sent while
				 * dragging, to keep the stream from drowning */
	guint position_timer;
	guint hide_timer;
	guint save_timer;
	guint title_retry;
	GdkPixbuf *cover;	/* full-size cover of the current entry */
	gchar *cover_source;	/* where it came from, for the info dialog */
	GtkWidget *info_dialog;	/* open media information window, if any */
	GtkWidget *error_dialog;
	double speed;
} MpApp;

/* window.c */
MpApp *mp_window_create(MpOptions *options);
void mp_window_destroy(MpApp *app);
void mp_app_add_uris(MpApp *app, gchar **uris, gboolean play_first,
		     gint position);
void mp_app_add_paths_async(MpApp *app, gchar **args, gboolean play_first);
void mp_app_play_id(MpApp *app, guint id);
void mp_app_next(MpApp *app, gboolean user);
void mp_app_prev(MpApp *app);
void mp_app_toggle_play(MpApp *app);
void mp_app_stop(MpApp *app);
void mp_app_seek_relative(MpApp *app, gint64 delta_ns);
void mp_app_set_volume(MpApp *app, double volume);
void mp_app_toggle_mute(MpApp *app);
void mp_app_set_fullscreen(MpApp *app, gboolean fullscreen);
void mp_app_toggle_playlist(MpApp *app);
void mp_app_schedule_save(MpApp *app);
void mp_app_set_speed(MpApp *app, double rate);
void mp_app_update_queue_count(MpApp *app);
void mp_app_clear_queue(MpApp *app);

/* dialogs.c */
/* "Open" plays what was chosen at once; "Add" only queues it (and plays it
 * when nothing else is playing). */
void mp_dialog_open_files(MpApp *app);
void mp_dialog_open_folder(MpApp *app);
void mp_dialog_add_files(MpApp *app);
void mp_dialog_add_folder(MpApp *app);
void mp_dialog_open_url(MpApp *app);
void mp_dialog_open_playlist(MpApp *app);
void mp_dialog_save_playlist(MpApp *app);
void mp_dialog_media_info(MpApp *app);
void mp_dialog_media_info_refresh(MpApp *app);
void mp_dialog_shortcuts(MpApp *app);
void mp_dialog_about(MpApp *app);
void mp_dialog_error(MpApp *app, const char *primary, const char *secondary,
		     const char *details, gboolean offer_next);
void mp_dialog_message(GtkWindow *parent, GtkMessageType type,
		       const char *primary, const char *secondary);

/* style.c */
void mp_style_install(void);
void mp_style_set_fullscreen(MpApp *app, gboolean on);

/* cover.c */
GdkPixbuf *mp_cover_from_tags(const GstTagList *tags);
GdkPixbuf *mp_cover_from_folder(const char *uri, gchar **source_out);
void mp_cover_draw(cairo_t *cr, GdkPixbuf *cover, double x, double y,
		   double w, double h, double radius, const char *fallback_icon,
		   GtkWidget *widget);

#endif
