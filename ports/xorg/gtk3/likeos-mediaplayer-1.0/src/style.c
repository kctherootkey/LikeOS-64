// LikeOS -- LikeOS Media Player: the look.
//
// The look lives in the LikeOS desktop theme (res/xorg/gtk3/theme, partial
// _mediaplayer.scss): the light sidebar with its group headers, the pages
// with their tile cards and plain lists, and the dark rounded transport
// strip.  Under any other theme this sheet stands in for it: the same
// structure -- spacing, sizes, the round play button, the dark strips -- in
// that theme's own colours, so the program stays usable and recognisable.
// Every rule is scoped to a class of ours.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "mediaplayer.h"

static const char fallback_css[] =
	".mp-content { border-left: 1px solid @borders; }\n"
	".mp-nav { padding: 8px; }\n"
	"list.mp-nav-list { background: transparent; }\n"
	"list.mp-nav-list row.mp-nav-item { min-height: 32px;"
	"  padding: 0 8px 0 12px; border-radius: 6px; }\n"
	"list.mp-nav-list row.mp-nav-header { min-height: 28px;"
	"  margin-top: 12px; padding: 0 2px 0 12px; }\n"
	".mp-nav-header-label { font-size: 7.5pt; font-weight: bold;"
	"  opacity: 0.7; }\n"
	".mp-nav-count { font-size: 7.5pt; opacity: 0.7; }\n"
	".mp-page { padding: 24px 0 0 32px; }\n"
	".mp-page-body { padding: 0 32px 24px 0; }\n"
	".mp-page-header { margin: 0 32px 16px 0; }\n"
	".mp-view-bar { margin: 0 32px 12px 0; }\n"
	".mp-list-scroller { margin: 0 24px 12px 0; }\n"
	".mp-grid-scroller flowbox.mp-grid { margin: 0 24px 24px -4px; }\n"
	".mp-page-title { font-size: 17pt; font-weight: bold; }\n"
	".mp-section-title { font-size: 12pt; font-weight: bold;"
	"  margin: 24px 0 8px 0; }\n"
	".mp-hint, .mp-page-subtitle, .mp-tile-sub, .mp-coll-sub,"
	" .mp-coll-kind, .mp-time, .mp-now-artist, .mp-art-sub,"
	" .mp-art-status, .mp-setting-desc, .mp-empty-text { opacity: 0.7; }\n"
	"button.mp-segment:checked { font-weight: bold; }\n"
	"treeview.mp-list, treeview.mp-list:selected { background: transparent;"
	"  color: @theme_text_color; }\n"
	"treeview.mp-list.mp-row-hover { background-color:"
	"  alpha(@theme_fg_color, 0.07); border-radius: 6px; }\n"
	"treeview.mp-list.mp-row-selected { background-color:"
	"  alpha(@theme_selected_bg_color, 0.25); border-radius: 6px; }\n"
	"treeview.mp-list.mp-cell-secondary { color:"
	"  alpha(@theme_text_color, 0.7); }\n"
	"treeview.mp-list.mp-cell-current { color: @theme_selected_bg_color; }\n"
	"treeview.mp-list.mp-cell-failed { color: @insensitive_fg_color; }\n"
	"flowboxchild.mp-tile { padding: 6px 6px 10px 6px; margin: 2px;"
	"  border-radius: 8px; border: 1px solid @borders; }\n"
	"flowboxchild.mp-tile.mp-tile-round { border-color: transparent; }\n"
	".mp-tile-title { margin-top: 8px; font-weight: bold; }\n"
	".mp-tile-art { background-image: linear-gradient(160deg,"
	"  shade(@theme_selected_bg_color, 1.3), @theme_selected_bg_color);"
	"  border-radius: 6px; color: white; }\n"
	".mp-tile-art.mp-tile-art-wide { background-image: none;"
	"  background-color: #1f2636; }\n"
	".mp-tile-art.mp-tile-art-round { border-radius: 9999px; }\n"
	".mp-tile-art.mp-tile-play { background-image: none;"
	"  background-color: @theme_selected_bg_color; border-radius: 9999px;"
	"  border: 2px solid white; color: @theme_selected_fg_color; }\n"
	".mp-empty-title, .mp-coll-title, .mp-art-title { font-size: 18pt;"
	"  font-weight: bold; }\n"
	".mp-setting-card { border: 1px solid @borders; border-radius: 8px; }\n"
	".mp-setting-card > .mp-setting-row + .mp-setting-row,"
	" .mp-setting-card > .mp-setting-group + .mp-setting-row,"
	" .mp-setting-card > .mp-setting-group + .mp-setting-group,"
	" .mp-setting-card > .mp-setting-row + .mp-setting-group"
	" { border-top: 1px solid @borders; }\n"
	".mp-setting-row { padding: 0 16px; min-height: 56px; }\n"
	".mp-setting-list > .mp-setting-subrow { padding: 0 16px 0 52px;"
	"  min-height: 40px; border-top: 1px solid @borders; }\n"
	".mp-now-wash { background-color: @theme_base_color; }\n"
	".mp-now-wash.mp-wash-over-cover { background-color:"
	"  alpha(@theme_base_color, 0.7); }\n"
	".mp-art { padding: 16px 24px 24px 24px; }\n"
	".mp-video { background-color: black; }\n"
	".mp-bar, .mp-top-bar { margin: 8px; border-radius: 14px;"
	"  background-color: #161c2a; color: #eef2fa; }\n"
	".mp-bar { padding: 8px 14px 8px 8px; }\n"
	".mp-top-bar { margin-bottom: 0; padding: 4px 12px 4px 4px; }\n"
	".mp-now-title, .mp-top-title { font-weight: bold; color: #eef2fa; }\n"
	".mp-time, .mp-now-artist { color: #a4adbf; }\n"
	"button.mp-bar-button, button.mp-now-button { min-width: 34px;"
	"  min-height: 34px; padding: 0; color: #eef2fa; background: none;"
	"  border-color: transparent; box-shadow: none; }\n"
	"button.mp-bar-button { border-radius: 9999px; }\n"
	"button.mp-bar-button:hover, button.mp-now-button:hover {"
	"  background-color: rgba(255,255,255,0.07); }\n"
	"button.mp-bar-button:checked { color: #5b8ff9; }\n"
	"button.mp-play { min-width: 40px; min-height: 40px; padding: 0;"
	"  margin: 0 8px; border-radius: 9999px; background-image: none;"
	"  border: none; background-color: #3f74f2; color: white; }\n"
	"scale.mp-seek highlight, scale.mp-volume highlight {"
	"  background-color: #5b8ff9; }\n"
	".mp-immersive, .mp-fullscreen { background-color: black; }\n"
	".mp-immersive .mp-content { background-color: black; border: none; }\n"
	".mp-immersive .mp-now-wash { background-color: #101010; }\n"
	".mp-immersive .mp-art-title { color: white; }\n"
	".mp-info-key { opacity: 0.7; }\n"
	".mp-shortcut-key { font-family: monospace; font-weight: bold;"
	"  padding: 1px 6px; border: 1px solid @borders; border-radius: 6px; }\n";

static GtkCssProvider *fallback;

/* The fallback is wanted exactly while the theme is not LikeOS; the theme
 * can change while the program runs. */
static void theme_changed(GtkSettings *s, GParamSpec *ps, gpointer data)
{
	(void)ps;
	(void)data;
	gchar *name = NULL;
	g_object_get(s, "gtk-theme-name", &name, NULL);
	const char *env = g_getenv("GTK_THEME");
	gboolean ours = (env != NULL && *env)
				? g_str_has_prefix(env, "LikeOS")
				: name != NULL && strcmp(name, "LikeOS") == 0;
	GdkScreen *scr = gdk_screen_get_default();

	if (!ours && fallback == NULL) {
		fallback = gtk_css_provider_new();
		GError *err = NULL;
		if (!gtk_css_provider_load_from_data(fallback, fallback_css, -1,
						     &err)) {
			/* The program works without it; it just looks
			 * plainer. */
			g_warning("style sheet: %s", err->message);
			g_clear_error(&err);
		}
		gtk_style_context_add_provider_for_screen(
			scr, GTK_STYLE_PROVIDER(fallback),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	} else if (ours && fallback != NULL) {
		gtk_style_context_remove_provider_for_screen(
			scr, GTK_STYLE_PROVIDER(fallback));
		g_clear_object(&fallback);
	}
	g_free(name);
}

void mp_style_install(void)
{
	GtkSettings *s = gtk_settings_get_default();
	if (s == NULL)
		return;
	g_signal_connect(s, "notify::gtk-theme-name", G_CALLBACK(theme_changed),
			 NULL);
	theme_changed(s, NULL, NULL);
}

void mp_style_set_fullscreen(MpApp *app, gboolean on)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(app->window);
	if (on)
		gtk_style_context_add_class(sc, "mp-fullscreen");
	else
		gtk_style_context_remove_class(sc, "mp-fullscreen");
}
