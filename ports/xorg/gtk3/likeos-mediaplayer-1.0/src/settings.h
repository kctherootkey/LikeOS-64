// LikeOS -- Media Player: what is remembered between runs.
//
// Kept in ~/.config/mediaplayer/settings, a GKeyFile with one [Player]
// group.  Every value is range-checked on load, so a hand-edited or damaged
// file degrades to the defaults key by key rather than wholesale.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef MP_SETTINGS_H
#define MP_SETTINGS_H

#include <glib.h>

typedef struct {
	double volume;		/* 0.0 .. 1.0, on the cubic (perceived) scale */
	gboolean muted;
	gchar *last_dir;	/* folder the file chooser opens in */
	gboolean shuffle;
	int repeat;		/* MpRepeat */
	gboolean show_playlist;
	int width, height;	/* window size */
	int queue_width;	/* width of the play queue panel */
	gchar *video_sink;	/* "auto", "xv", "gtk", "x11", "gl" */
	gboolean gapless;
	gboolean discover;	/* read tags of queued local files ahead */
	gboolean show_remaining;
	gboolean subtitles;
} MpSettings;

void mp_settings_load(MpSettings *s);
/* Returns FALSE (and logs) when the file could not be written. */
gboolean mp_settings_save(const MpSettings *s);
void mp_settings_clear(MpSettings *s);
/* ~/.config/mediaplayer */
gchar *mp_settings_dir(void);

#endif
