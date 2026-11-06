// LikeOS -- LikeOS Image Viewer: the settings.
//
// A key file, ~/.config/likeos/imageviewer.ini, read once at start-up and
// written whenever something changes.  Unknown keys are kept as they are,
// so a newer version's settings survive an older version being run, and a
// value out of range falls back to the default instead of being taken.
//
//   [Viewer]  Backdrop=dark|darker|checker  WheelZooms=true
//             ConfirmDelete=false  Loop=true  ShowFilmstrip=false
//             ShowInfo=false  SmoothZoom=true
//   [Slideshow]  Interval=5
//   [Saving]  JpegQuality=90  KeepExif=true
//   [Window]  Width=1100  Height=760  Maximized=false
//
// and what the editor's tools keep between runs, a group per tool:
//
//   [Draw]  PenColor=#d63a3a  PenWidth=4  MarkerColor=#f2c230 ...
//   [Retouch]  BrushSize=40  AutoApply=true
//   [Background]  BrushSize=60  Softness=35
//
// Tools change their values while they are used (a slider being dragged),
// so those are written a moment after the last change, not on each one.
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "imageviewer.h"

static IvSettings settings;
static GKeyFile *keyfile;
static gboolean loaded;
static guint write_timer;	/* a tool's value waits to be written */

/* How long after a tool's last change its values are written. */
#define IV_SETTINGS_WRITE_DELAY 2

static const char *const backdrop_names[] = { "dark", "darker", "checker" };

char *iv_settings_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "likeos",
				"imageviewer.ini", NULL);
}

static gboolean get_bool(const char *group, const char *key, gboolean def)
{
	GError *e = NULL;
	gboolean v = g_key_file_get_boolean(keyfile, group, key, &e);

	if (e) {
		g_error_free(e);
		return def;
	}
	return v;
}

static int get_int(const char *group, const char *key, int def, int lo,
		   int hi)
{
	GError *e = NULL;
	int v = g_key_file_get_integer(keyfile, group, key, &e);

	if (e) {
		g_error_free(e);
		return def;
	}
	return v < lo || v > hi ? def : v;
}

static void defaults(IvSettings *s)
{
	s->backdrop = IV_BACKDROP_DARK;
	s->wheel_zooms = TRUE;
	s->slideshow_seconds = 5;
	s->confirm_delete = FALSE;
	s->jpeg_quality = 90;
	s->keep_exif = TRUE;
	s->show_filmstrip = FALSE;
	s->loop = TRUE;
	s->show_info = FALSE;
	s->smooth_zoom = TRUE;
	s->width = 1100;
	s->height = 760;
	s->maximized = FALSE;
}

static void load(void)
{
	char *path = iv_settings_path();
	char *bd;

	loaded = TRUE;
	defaults(&settings);
	keyfile = g_key_file_new();
	if (!g_key_file_load_from_file(keyfile, path,
				       G_KEY_FILE_KEEP_COMMENTS, NULL)) {
		g_free(path);
		return;
	}
	g_free(path);

	bd = g_key_file_get_string(keyfile, "Viewer", "Backdrop", NULL);
	for (gsize i = 0; bd && i < G_N_ELEMENTS(backdrop_names); i++)
		if (!g_ascii_strcasecmp(bd, backdrop_names[i]))
			settings.backdrop = (IvBackdrop)i;
	g_free(bd);
	settings.wheel_zooms = get_bool("Viewer", "WheelZooms", TRUE);
	settings.confirm_delete = get_bool("Viewer", "ConfirmDelete", FALSE);
	settings.loop = get_bool("Viewer", "Loop", TRUE);
	settings.show_filmstrip = get_bool("Viewer", "ShowFilmstrip", FALSE);
	settings.show_info = get_bool("Viewer", "ShowInfo", FALSE);
	settings.smooth_zoom = get_bool("Viewer", "SmoothZoom", TRUE);
	settings.slideshow_seconds = get_int("Slideshow", "Interval", 5, 1, 60);
	settings.jpeg_quality = get_int("Saving", "JpegQuality", 90, 10, 100);
	settings.keep_exif = get_bool("Saving", "KeepExif", TRUE);
	settings.width = get_int("Window", "Width", 1100, 400, 16384);
	settings.height = get_int("Window", "Height", 760, 300, 16384);
	settings.maximized = get_bool("Window", "Maximized", FALSE);
}

IvSettings *iv_settings_get(void)
{
	if (!loaded)
		load();
	return &settings;
}

static void write_file(void)
{
	char *path, *dir, *data;
	gsize len;
	GError *e = NULL;

	if (write_timer) {
		g_source_remove(write_timer);
		write_timer = 0;
	}
	path = iv_settings_path();
	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	data = g_key_file_to_data(keyfile, &len, NULL);
	if (!g_file_set_contents(path, data, (gssize)len, &e)) {
		g_warning("cannot write %s: %s", path, e->message);
		g_error_free(e);
	}
	g_free(data);
	g_free(dir);
	g_free(path);
}

void iv_settings_save(void)
{
	if (!loaded)
		load();
	g_key_file_set_string(keyfile, "Viewer", "Backdrop",
			      backdrop_names[CLAMP(settings.backdrop, 0, 2)]);
	g_key_file_set_boolean(keyfile, "Viewer", "WheelZooms",
			       settings.wheel_zooms);
	g_key_file_set_boolean(keyfile, "Viewer", "ConfirmDelete",
			       settings.confirm_delete);
	g_key_file_set_boolean(keyfile, "Viewer", "Loop", settings.loop);
	g_key_file_set_boolean(keyfile, "Viewer", "ShowFilmstrip",
			       settings.show_filmstrip);
	g_key_file_set_boolean(keyfile, "Viewer", "ShowInfo", settings.show_info);
	g_key_file_set_boolean(keyfile, "Viewer", "SmoothZoom",
			       settings.smooth_zoom);
	g_key_file_set_integer(keyfile, "Slideshow", "Interval",
			       settings.slideshow_seconds);
	g_key_file_set_integer(keyfile, "Saving", "JpegQuality",
			       settings.jpeg_quality);
	g_key_file_set_boolean(keyfile, "Saving", "KeepExif", settings.keep_exif);
	g_key_file_set_integer(keyfile, "Window", "Width", settings.width);
	g_key_file_set_integer(keyfile, "Window", "Height", settings.height);
	g_key_file_set_boolean(keyfile, "Window", "Maximized",
			       settings.maximized);
	write_file();
}

void iv_settings_flush(void)
{
	if (write_timer)
		write_file();
}

/* ---- the tools' values ------------------------------------------------------- */

static gboolean write_later(gpointer data)
{
	write_timer = 0;
	write_file();
	return G_SOURCE_REMOVE;
}

static void changed_later(void)
{
	if (write_timer)
		g_source_remove(write_timer);
	write_timer = g_timeout_add_seconds(IV_SETTINGS_WRITE_DELAY, write_later,
					    NULL);
}

/* A tool's value as text; nothing is written when it is what the file
 * has already. */
static void set_value(const char *tool, const char *key, const char *text)
{
	char *old = g_key_file_get_value(keyfile, tool, key, NULL);
	gboolean same = !g_strcmp0(old, text);

	g_free(old);
	if (same)
		return;
	g_key_file_set_value(keyfile, tool, key, text);
	changed_later();
}

double iv_settings_tool_double(const char *tool, const char *key, double def,
			       double lo, double hi)
{
	GError *e = NULL;
	double v;

	if (!loaded)
		load();
	v = g_key_file_get_double(keyfile, tool, key, &e);
	if (e) {
		g_error_free(e);
		return def;
	}
	return isfinite(v) && v >= lo && v <= hi ? v : def;
}

void iv_settings_set_tool_double(const char *tool, const char *key, double v)
{
	char buf[G_ASCII_DTOSTR_BUF_SIZE];

	if (!loaded)
		load();
	/* Two decimals are all a slider shows; the file stays readable. */
	g_ascii_formatd(buf, sizeof buf, "%.2f", v);
	set_value(tool, key, buf);
}

gboolean iv_settings_tool_bool(const char *tool, const char *key, gboolean def)
{
	if (!loaded)
		load();
	return get_bool(tool, key, def);
}

void iv_settings_set_tool_bool(const char *tool, const char *key, gboolean v)
{
	if (!loaded)
		load();
	set_value(tool, key, v ? "true" : "false");
}

/* Colours as #rrggbb, with the alpha after when it is not opaque. */
guint32 iv_settings_tool_rgba(const char *tool, const char *key, guint32 def)
{
	char *text, *end = NULL;
	guint64 v;
	size_t len;

	if (!loaded)
		load();
	text = g_key_file_get_string(keyfile, tool, key, NULL);
	if (!text)
		return def;
	g_strstrip(text);
	len = strlen(text);
	if (text[0] != '#' || (len != 7 && len != 9)) {
		g_free(text);
		return def;
	}
	v = g_ascii_strtoull(text + 1, &end, 16);
	if (!end || *end) {
		g_free(text);
		return def;
	}
	g_free(text);
	return len == 7 ? (guint32)(v << 8 | 0xff) : (guint32)v;
}

void iv_settings_set_tool_rgba(const char *tool, const char *key, guint32 rgba)
{
	char buf[16];

	if (!loaded)
		load();
	if ((rgba & 0xff) == 0xff)
		g_snprintf(buf, sizeof buf, "#%06x", rgba >> 8);
	else
		g_snprintf(buf, sizeof buf, "#%08x", rgba);
	set_value(tool, key, buf);
}
