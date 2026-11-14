/* LikeOS installer -- keyboard layouts (see keyboard.h).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "keyboard.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Used only when the catalogue file cannot be read, so the page is never
 * empty: the most common layouts. */
static struct kbd_layout g_fallback[] = {
	{ "us", "English (US)", "English", "us", "" },
	{ "gb", "English (UK)", "English", "gb", "" },
	{ "de", "German", "German", "de", "" },
	{ "de-nodeadkeys", "German (no dead keys)", "German", "de", "nodeadkeys" },
	{ "fr", "French", "French", "fr", "" },
	{ "es", "Spanish", "Spanish", "es", "" },
	{ "it", "Italian", "Italian", "it", "" },
	{ "ch", "Swiss German", "German", "ch", "" },
	{ "at", "German (Austria)", "German", "at", "" },
	{ "nl", "Dutch", "Dutch", "nl", "" },
	{ "se", "Swedish", "Swedish", "se", "" },
	{ "no", "Norwegian", "Norwegian", "no", "" },
	{ "dk", "Danish", "Danish", "dk", "" },
	{ "fi", "Finnish", "Finnish", "fi", "" },
	{ "pl", "Polish", "Polish", "pl", "" },
	{ "pt", "Portuguese", "Portuguese", "pt", "" },
	{ "br", "Portuguese (Brazil)", "Portuguese", "br", "" },
	{ "ru", "Russian", "Russian", "ru", "" },
	{ "jp", "Japanese", "Japanese", "jp", "" },
};

void keyboard_load(App *app)
{
	if (app->kbd_cat)
		return;
	app->kbd_cat = kbd_catalogue_load();
	if (app->kbd_cat->len == 0)
		g_message("keyboard catalogue %s not readable; using the "
			  "built-in list", KBD_CATALOGUE);
}

int keyboard_count(App *app)
{
	keyboard_load(app);
	if (app->kbd_cat->len)
		return (int)app->kbd_cat->len;
	return (int)G_N_ELEMENTS(g_fallback);
}

const struct kbd_layout *keyboard_get(App *app, int i)
{
	keyboard_load(app);
	if (app->kbd_cat->len)
		return i >= 0 && i < (int)app->kbd_cat->len ?
			g_ptr_array_index(app->kbd_cat, i) : NULL;
	return i >= 0 && i < (int)G_N_ELEMENTS(g_fallback) ? &g_fallback[i] : NULL;
}

const struct kbd_layout *keyboard_find(App *app, const char *layout,
				       const char *variant)
{
	int n = keyboard_count(app);

	if (!layout)
		return NULL;
	if (!variant)
		variant = "";
	for (int i = 0; i < n; i++) {
		const struct kbd_layout *l = keyboard_get(app, i);

		if (strcmp(l->layout, layout) == 0 && strcmp(l->variant, variant) == 0)
			return l;
	}
	return NULL;
}

char *keyboard_key(const struct kbd_layout *l)
{
	if (!l)
		return g_strdup("");
	if (l->variant && *l->variant)
		return g_strdup_printf("%s:%s", l->layout, l->variant);
	return g_strdup(l->layout);
}

const struct kbd_layout *keyboard_find_key(App *app, const char *key)
{
	const struct kbd_layout *l;
	char **parts;

	if (!key || !*key)
		return NULL;
	parts = g_strsplit(key, ":", 2);
	l = keyboard_find(app, parts[0], parts[1] ? parts[1] : "");
	g_strfreev(parts);
	return l;
}

const char *keyboard_default_for_locale(const char *locale)
{
	if (locale && g_str_has_prefix(locale, "en_GB"))
		return "gb";
	return "us";
}

/* In the installer session the layout is switched for real.  The
 * stand-in backend and a simulation on image files run on someone's
 * desktop (or a test's X server): they leave that keyboard alone unless
 * LIKEOS_INSTALLER_XKB=1 asks for the real thing. */
static gboolean keyboard_live(App *app)
{
	const char *e = g_getenv("LIKEOS_INSTALLER_XKB");

	if (!app->mock && !app->simulate)
		return TRUE;
	return e && *e == '1';
}

/* One setxkbmap at a time: two running side by side may finish in either
 * order, and the layout of the older request would then be the one the
 * person types with.  A request made while one runs waits (only the
 * newest), and the end of the running one is seen by polling, so no
 * SIGCHLD handler can interrupt the backend's calls on the worker. */
static GPid g_kbd_child;
static char **g_kbd_pending;
static App *g_kbd_app;

static void spawn_setxkbmap(App *app, char **argv);

static gboolean kbd_child_poll(gpointer data)
{
	int status;
	pid_t r;
	char **next;

	(void)data;
	r = waitpid(g_kbd_child, &status, WNOHANG);
	if (r == 0 || (r < 0 && errno == EINTR))
		return G_SOURCE_CONTINUE;
	/* ended (or reaped by someone else: ECHILD) */
	g_spawn_close_pid(g_kbd_child);
	g_kbd_child = 0;
	next = g_kbd_pending;
	g_kbd_pending = NULL;
	if (next) {
		spawn_setxkbmap(g_kbd_app, next);
		g_strfreev(next);
	}
	return G_SOURCE_REMOVE;
}

static void spawn_setxkbmap(App *app, char **argv)
{
	char *cmd;
	GError *err = NULL;
	GPid pid;

	if (g_kbd_child) {
		g_strfreev(g_kbd_pending);
		g_kbd_pending = g_strdupv(argv);
		g_kbd_app = app;
		return;
	}
	cmd = g_strjoinv(" ", argv);
	app_log(app, IL_LOG_DEBUG, "ui: running %s", cmd);
	g_free(cmd);
	if (!g_spawn_async(NULL, argv, NULL,
			   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
			   G_SPAWN_STDOUT_TO_DEV_NULL |
			   G_SPAWN_STDERR_TO_DEV_NULL,
			   NULL, NULL, &pid, &err)) {
		app_log(app, IL_LOG_WARN, "ui: setxkbmap did not start: %s",
			err->message);
		g_error_free(err);
		return;
	}
	g_kbd_child = pid;
	g_kbd_app = app;
	g_timeout_add(20, kbd_child_poll, NULL);
}

void keyboard_apply(App *app)
{
	GString *layouts = g_string_new(NULL), *variants = g_string_new(NULL);
	GPtrArray *argv = g_ptr_array_new();

	for (int i = 0; i < app->cfg.nlayouts; i++) {
		if (i) {
			g_string_append_c(layouts, ',');
			g_string_append_c(variants, ',');
		}
		g_string_append(layouts, app->cfg.layouts[i]);
		g_string_append(variants, app->cfg.variants[i]);
	}
	g_ptr_array_add(argv, (char *)"setxkbmap");
	g_ptr_array_add(argv, (char *)"-layout");
	g_ptr_array_add(argv, layouts->str);
	g_ptr_array_add(argv, (char *)"-variant");
	g_ptr_array_add(argv, variants->str);
	/* the options of the new system: cleared, then the group switch for
	 * two layouts or more */
	app_finish_keyboard(app);
	g_ptr_array_add(argv, (char *)"-option");
	g_ptr_array_add(argv, (char *)"");
	if (app->cfg.xkb_options[0]) {
		g_ptr_array_add(argv, (char *)"-option");
		g_ptr_array_add(argv, app->cfg.xkb_options);
	}
	g_ptr_array_add(argv, NULL);
	if (keyboard_live(app))
		spawn_setxkbmap(app, (char **)argv->pdata);
	g_ptr_array_free(argv, TRUE);
	g_string_free(layouts, TRUE);
	g_string_free(variants, TRUE);
}

void keyboard_apply_one(App *app, const char *layout, const char *variant)
{
	char *argv[] = {
		(char *)"setxkbmap", (char *)"-layout", (char *)layout,
		(char *)"-variant", (char *)(variant ? variant : ""), NULL,
	};

	if (keyboard_live(app))
		spawn_setxkbmap(app, argv);
}

char *keyboard_short_code(const char *layout)
{
	return g_ascii_strup(layout && *layout ? layout : "us", 3);
}
