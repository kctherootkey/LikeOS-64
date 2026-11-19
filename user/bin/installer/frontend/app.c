/* LikeOS installer -- shared state helpers (see app.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "app.h"
#include "backend-bridge.h"
#include "keyboard.h"

#include <stdarg.h>
#include <string.h>
#include <unistd.h>

App *the_app;

/* The backend in use (backend-api.h); main() sets it before anything
 * calls it. */
const IlBackend *il_be = &il_backend_mock;

void app_init_config(App *app)
{
	il_config_defaults(&app->cfg);
	if (!app->cfg.locale[0])
		g_strlcpy(app->cfg.locale, "en_US.UTF-8", sizeof(app->cfg.locale));
	if (!app->cfg.time_format[0])
		g_strlcpy(app->cfg.time_format, app->cfg.locale,
			  sizeof(app->cfg.time_format));
	if (app->cfg.nlayouts < 1) {
		app->cfg.nlayouts = 1;
		g_strlcpy(app->cfg.layouts[0], "us", sizeof(app->cfg.layouts[0]));
		app->cfg.variants[0][0] = '\0';
	}
	app_finish_keyboard(app);
	if (!app->cfg.hostname[0])
		il_suggest_hostname(app->cfg.hostname, sizeof(app->cfg.hostname));
	app->target_disk = -1;
	app->failed_step = -1;
}

void app_finish_keyboard(App *app)
{
	/* What the backend writes for two layouts or more anyway, and what
	 * setxkbmap needs to switch between them on the live session. */
	if (app->cfg.nlayouts > 1)
		g_strlcpy(app->cfg.xkb_options, "grp:alt_shift_toggle",
			  sizeof(app->cfg.xkb_options));
	else
		app->cfg.xkb_options[0] = '\0';
}

const char *app_locale_label(const char *name)
{
	for (int i = 0; i < il_locale_count(); i++)
		if (g_strcmp0(il_locale_name(i), name) == 0)
			return il_locale_label(i);
	return name;
}

const char *app_timezone_label(const char *name)
{
	for (int i = 0; i < il_timezone_count(); i++)
		if (g_strcmp0(il_timezone_name(i), name) == 0)
			return il_timezone_label(i);
	return name;
}

char *app_layout_label(App *app, const char *layout, const char *variant)
{
	const struct kbd_layout *l = keyboard_find(app, layout, variant);

	if (l)
		return g_strdup(l->name);
	if (variant && *variant)
		return g_strdup_printf("%s (%s)", layout, variant);
	return g_strdup(layout);
}

char *app_layouts_summary(App *app)
{
	GString *s = g_string_new(NULL);

	for (int i = 0; i < app->cfg.nlayouts; i++) {
		char *l = app_layout_label(app, app->cfg.layouts[i],
					   app->cfg.variants[i]);

		if (i)
			g_string_append(s, ", ");
		g_string_append(s, l);
		g_free(l);
	}
	return g_string_free(s, FALSE);
}

il_plan *app_target_plan(App *app)
{
	if (!app->disks || app->target_disk < 0 ||
	    app->target_disk >= app->disks->n)
		return NULL;
	return app->plans[app->target_disk];
}

const struct il_disk *app_target_disk(App *app)
{
	if (!app->disks || app->target_disk < 0 ||
	    app->target_disk >= app->disks->n)
		return NULL;
	return &app->disks->disks[app->target_disk];
}

const char *app_log_path(App *app)
{
	return app->ctx ? il_log_path(app->ctx) : IL_DEFAULT_LOG;
}

void app_quit(App *app)
{
	(void)app;
	gtk_main_quit();
}

/* Whether this program may switch the computer off itself: libinstaller
 * on a LikeOS machine's disks, run by root.  Otherwise (the stand-in, a
 * simulation on image files, a test on another system) it only ends, with
 * the exit status that tells the installer session what to do. */
static gboolean may_power(App *app)
{
#ifdef __LIKEOS__
	return !il_be->is_mock && !app->simulate && geteuid() == 0;
#else
	(void)app;
	return FALSE;
#endif
}

void app_power_action(App *app, gboolean reboot)
{
	const char *what = reboot ? "restart" : "power off";

	if (bridge_busy(app->bridge))
		return;
	app->exit_code = reboot ? APP_EXIT_RESTART : APP_EXIT_POWEROFF;
	app_log(app, IL_LOG_INFO, "ui: %s chosen%s", what,
		app->installed ? " after the installation" : "");
	if (may_power(app)) {
		int rc = reboot ? il_reboot(app->ctx) : il_poweroff(app->ctx);

		/* A working reboot(2) does not come back.  When it does, the
		 * installer session does it on the exit status. */
		app_log(app, IL_LOG_WARN, "ui: the %s did not happen (%s); "
			"leaving it to the installer session", what,
			rc ? il_last_error(app->ctx) : "no error");
	}
	app_quit(app);
}

void app_exit_setup(App *app)
{
	if (bridge_busy(app->bridge))
		return;
	app->exit_code = APP_EXIT_QUIT;
	app_log(app, IL_LOG_INFO, "ui: setup left without %s",
		app->installed ? "restarting" : "installing");
	app_quit(app);
}

void app_log(App *app, int level, const char *fmt, ...)
{
	va_list ap;
	char *line;

	va_start(ap, fmt);
	line = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	if (!app || !app->ctx)
		g_debug("%s", line);
	else if (app->bridge && bridge_busy(app->bridge))
		bridge_defer_log(app->bridge, level, line);
	else
		il_be->log(app->ctx, level, "%s", line);
	g_free(line);
}

char *app_sentence(const char *text)
{
	size_t n;
	char *s;

	if (!text || !*text)
		return g_strdup("");
	n = strlen(text);
	s = strchr(".!?:", text[n - 1]) ? g_strdup(text) :
		g_strdup_printf("%s.", text);
	/* "the system image ... is damaged" begins a sentence here */
	if (s[0] >= 'a' && s[0] <= 'z')
		s[0] = (char)(s[0] - 'a' + 'A');
	return s;
}

GdkPixbuf *app_logo(App *app, int size)
{
	if (!app->logo) {
		GError *err = NULL;

		app->logo = gdk_pixbuf_new_from_resource(
			"/org/likeos/installer/likeos-logo.png", &err);
		if (!app->logo) {
			g_debug("logo: %s", err ? err->message : "?");
			g_clear_error(&err);
			return NULL;
		}
	}
	return gdk_pixbuf_scale_simple(app->logo, size, size, GDK_INTERP_HYPER);
}

char *app_size_text(guint64 bytes)
{
	char buf[32];

	il_format_size(bytes, buf, sizeof(buf));
	return g_strdup(buf);
}
