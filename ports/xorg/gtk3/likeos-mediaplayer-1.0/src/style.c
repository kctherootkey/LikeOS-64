// LikeOS -- Media Player: the look.
//
// The desktop theme (LikeOS, Windows 11's light palette on Adwaita's widget
// structure) supplies everything general.  This sheet only adds what a
// player has and a dialog does not: round transport buttons with the big
// accent-filled play button in the middle, a thin seek bar, the black video
// stage, the cover-art page and a dark control bar in fullscreen.  Every
// rule is scoped to a class of ours, so nothing leaks into the file chooser
// or the dialogs.
//
// Copyright (C) 2026 The LikeOS Project

#include "mediaplayer.h"

static const char css[] =
	/* The stage: black under video, the window's Mica grey under art. */
	".mp-stage { background-color: #000000; }\n"
	".mp-art { background-color: #f3f3f3; }\n"
	".mp-art-title { font-size: 18pt; font-weight: 600; color: #1b1b1b; }\n"
	".mp-art-sub { font-size: 11pt; color: #616161; }\n"
	".mp-art-status { font-size: 9pt; color: #616161; }\n"

	/* The control bar: a raised layer above the stage. */
	".mp-controls {\n"
	"  background-color: #fbfbfb;\n"
	"  border-top: 1px solid #e5e5e5;\n"
	"  padding: 6px 14px 10px 14px;\n"
	"}\n"
	".mp-time { color: #616161;\n"
	"  font-size: 8pt; min-width: 48px; }\n"
	".mp-now-title { font-weight: 600; }\n"
	".mp-now-artist { color: #616161; font-size: 8pt; }\n"

	/* Round, flat transport buttons that only show a fill when pointed
	 * at -- Windows 11's "subtle" button. */
	"button.mp-round {\n"
	"  border-radius: 9999px;\n"
	"  min-width: 32px; min-height: 32px;\n"
	"  padding: 0;\n"
	"  border: none;\n"
	"  background-image: none;\n"
	"  background-color: transparent;\n"
	"  box-shadow: none;\n"
	"  color: #1b1b1b;\n"
	"}\n"
	"button.mp-round:hover { background-color: rgba(0,0,0,0.0373); }\n"
	"button.mp-round:active { background-color: rgba(0,0,0,0.0241);\n"
	"  color: #5d5d5d; }\n"
	"button.mp-round:checked { background-color: rgba(0,95,184,0.10);\n"
	"  color: #005fb8; }\n"
	"button.mp-round:checked:hover {\n"
	"  background-color: rgba(0,95,184,0.16); }\n"
	"button.mp-round:disabled { color: #a0a0a0; }\n"

	/* The play button: the accent circle. */
	"button.mp-play {\n"
	"  border-radius: 9999px;\n"
	"  min-width: 44px; min-height: 44px;\n"
	"  padding: 0;\n"
	"  border: none;\n"
	"  background-image: none;\n"
	"  background-color: #005fb8;\n"
	"  color: #ffffff;\n"
	"  box-shadow: 0 1px 2px rgba(0,0,0,0.18);\n"
	"}\n"
	"button.mp-play:hover { background-color: #1a6ebd; }\n"
	"button.mp-play:active { background-color: #337fc6; }\n"
	"button.mp-play:disabled { background-color: #c5c5c5; }\n"

	/* A thin seek bar whose knob only matters when grabbed. */
	"scale.mp-seek trough { min-height: 4px; border-radius: 2px; }\n"
	"scale.mp-seek highlight { border-radius: 2px;\n"
	"  background-color: #005fb8; }\n"
	"scale.mp-seek slider { min-width: 14px; min-height: 14px; }\n"
	"scale.mp-volume { min-width: 96px; }\n"
	"scale.mp-volume highlight { background-color: #005fb8; }\n"

	/* The play queue panel. */
	".mp-queue { background-color: #ffffff;\n"
	"  border-left: 1px solid #e5e5e5; }\n"
	".mp-queue-header { padding: 8px 10px 6px 12px; }\n"
	".mp-queue-title { font-size: 11pt; font-weight: 600; }\n"
	".mp-queue-count { color: #616161; font-size: 8pt; }\n"
	".mp-queue-tools { padding: 4px 8px; border-top: 1px solid #e5e5e5; }\n"
	".mp-queue treeview.view { padding: 3px 4px; }\n"
	".mp-queue-hint { color: #616161; }\n"

	/* Fullscreen: the control bar turns dark so it does not glare over
	 * a film. */
	".mp-fullscreen .mp-controls {\n"
	"  background-color: #202020;\n"
	"  border-top-color: #2b2b2b;\n"
	"  color: #ffffff;\n"
	"}\n"
	".mp-fullscreen .mp-time, .mp-fullscreen .mp-now-artist {\n"
	"  color: #c5c5c5; }\n"
	".mp-fullscreen button.mp-round { color: #ffffff; }\n"
	".mp-fullscreen button.mp-round:hover {\n"
	"  background-color: rgba(255,255,255,0.08); }\n"
	".mp-fullscreen button.mp-round:checked { color: #60cdff;\n"
	"  background-color: rgba(96,205,255,0.12); }\n"
	".mp-fullscreen button.mp-play { background-color: #60cdff;\n"
	"  color: #000000; }\n"
	".mp-fullscreen .mp-art { background-color: #000000; }\n"
	".mp-fullscreen .mp-art-title { color: #ffffff; }\n"
	".mp-fullscreen .mp-art-sub, .mp-fullscreen .mp-art-status {\n"
	"  color: #c5c5c5; }\n"

	/* Information dialog: the keys quieter than the values. */
	".mp-info-key { color: #616161; }\n"
	".mp-shortcut-key { font-family: monospace; font-weight: 600;\n"
	"  padding: 1px 6px; border: 1px solid #d0d0d0; border-radius: 4px;\n"
	"  background-color: #fbfbfb; }\n";

void mp_style_install(void)
{
	GtkCssProvider *p = gtk_css_provider_new();
	GError *err = NULL;
	if (!gtk_css_provider_load_from_data(p, css, -1, &err)) {
		/* The program works without it; it just looks plainer. */
		g_warning("style sheet: %s", err->message);
		g_clear_error(&err);
	}
	gtk_style_context_add_provider_for_screen(
		gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

void mp_style_set_fullscreen(MpApp *app, gboolean on)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(app->window);
	if (on)
		gtk_style_context_add_class(sc, "mp-fullscreen");
	else
		gtk_style_context_remove_class(sc, "mp-fullscreen");
}
