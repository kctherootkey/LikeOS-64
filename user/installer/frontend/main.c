/* LikeOS installer -- likeos-installer, the graphical setup program.
 *
 * Usage: likeos-installer [--mock] [--window] [--page=N] [--substep=N]
 *                         [--size=WxH] [--fast] [--no-animation]
 *                         [--simulate=IMG ...] [--source=FILE]
 *                         [--target=DIR] [--log=FILE] [--verbose]
 *                         [--autopilot=FILE]
 *
 *   --mock          use the built-in stand-in backend (made-up disks, a
 *                   simulated installation, nothing is written); the only
 *                   backend of a build without libinstaller
 *   --simulate=IMG  install onto image files instead of disks (repeat for
 *                   more than one); libinstaller's simulate mode
 *   --source=FILE   the system image (default
 *                   /install/likeos-rootfs.tar.zst)
 *   --target=DIR    where the new system is mounted (default /mnt/likeos)
 *   --log=FILE      the installation log (default /tmp/likeos-install.log)
 *   --verbose       debug lines in the log, and everything on stderr
 *   --autopilot=FILE  answer every page from an answer file
 *                   (likeos-install.conf(5)) and install without a person:
 *                   for tests (also LIKEOS_INSTALLER_AUTOPILOT=FILE)
 *   --window        a normal 1024x768 window instead of the full screen
 *   --page=N        start on page N (0 splash ... 13 error), with made-up
 *                   answers for the pages before it -- for screenshots
 *   --substep=N     with --page: go N steps into a page with steps
 *   --size=WxH      pretend the screen is W x H pixels
 *   --fast          no minimum time on the start and waiting screens
 *   --no-animation  still illustrations and spinners
 *
 * The program runs full screen without a window manager (the installer
 * session starts it straight from xinit), so it places and sizes its one
 * window itself and draws its dialogs inside it.
 *
 * Exit status (what the installer session acts on): 0 restart the
 * computer, 10 switch it off, 2 setup was left without installing, 3 an
 * autopilot run failed, 1 the program could not start.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "app.h"
#include "autopilot.h"
#include "backend-bridge.h"
#include "keyboard.h"
#include "layout.h"
#include "pages/pages.h"
#include "sheet.h"
#include "wizard.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

static gboolean opt_mock;
static gboolean opt_window;
static int opt_page = -1;
static int opt_substep;
static char *opt_size;
static gboolean opt_fast;
static gboolean opt_no_anim;
static gboolean opt_version;
static char **opt_simulate;
static char *opt_source;
static char *opt_target;
static char *opt_log;
static gboolean opt_verbose;
static char *opt_autopilot;

static GOptionEntry g_options[] = {
	{ "mock", 0, 0, G_OPTION_ARG_NONE, &opt_mock,
	  "Use the stand-in backend: made-up disks, nothing is written", NULL },
	{ "window", 0, 0, G_OPTION_ARG_NONE, &opt_window,
	  "Run in a window instead of the full screen", NULL },
	{ "page", 0, 0, G_OPTION_ARG_INT, &opt_page,
	  "Start on page N (0-13), with made-up answers before it", "N" },
	{ "substep", 0, 0, G_OPTION_ARG_INT, &opt_substep,
	  "With --page: go N steps into the page", "N" },
	{ "size", 0, 0, G_OPTION_ARG_STRING, &opt_size,
	  "Pretend the screen is W x H pixels", "WxH" },
	{ "fast", 0, 0, G_OPTION_ARG_NONE, &opt_fast,
	  "No minimum time on the waiting screens", NULL },
	{ "no-animation", 0, 0, G_OPTION_ARG_NONE, &opt_no_anim,
	  "Do not animate illustrations and spinners", NULL },
	{ "simulate", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_simulate,
	  "Install onto this image file instead of a disk (repeatable)",
	  "IMG" },
	{ "source", 0, 0, G_OPTION_ARG_FILENAME, &opt_source,
	  "The system image (default " IL_DEFAULT_SOURCE ")", "FILE" },
	{ "target", 0, 0, G_OPTION_ARG_FILENAME, &opt_target,
	  "Where the new system is mounted (default " IL_DEFAULT_TARGET ")",
	  "DIR" },
	{ "log", 0, 0, G_OPTION_ARG_FILENAME, &opt_log,
	  "The installation log (default " IL_DEFAULT_LOG ")", "FILE" },
	{ "verbose", 0, 0, G_OPTION_ARG_NONE, &opt_verbose,
	  "Debug lines in the log, everything also on stderr", NULL },
	{ "autopilot", 0, 0, G_OPTION_ARG_FILENAME, &opt_autopilot,
	  "Answer every page from this answer file and install (tests)",
	  "FILE" },
	{ "version", 0, 0, G_OPTION_ARG_NONE, &opt_version,
	  "Print the version and exit", NULL },
	{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL },
};

/* ---- the window ---------------------------------------------------- */

static gboolean relayout_idle(gpointer data)
{
	App *app = data;

	g_object_set_data(G_OBJECT(app->window), "relayout-pending", NULL);
	wizard_relayout(app->wiz);
	return G_SOURCE_REMOVE;
}

static void on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer data)
{
	App *app = data;

	(void)w;
	if (layout_update(app, a->width, a->height) &&
	    !g_object_get_data(G_OBJECT(app->window), "relayout-pending")) {
		g_object_set_data(G_OBJECT(app->window), "relayout-pending",
				  GINT_TO_POINTER(1));
		g_idle_add(relayout_idle, app);
	}
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	App *app = data;

	(void)w;
	return wizard_key_press(app->wiz, ev);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	App *app = data;

	(void)w;
	(void)ev;
	/* a debugging window closes; the real thing asks */
	if (app->windowed && app->mock) {
		app->exit_code = APP_EXIT_QUIT;
		return FALSE;
	}
	if (!bridge_busy(app->bridge) && !app->installed)
		sheet_power(app);
	return TRUE;
}

static void on_screen_changed(GdkScreen *screen, gpointer data)
{
	App *app = data;

	(void)screen;
	layout_cover_screen(app);
}

static void load_css(void)
{
	GtkCssProvider *css = gtk_css_provider_new();

	gtk_css_provider_load_from_resource(css,
					    "/org/likeos/installer/installer.css");
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
						  GTK_STYLE_PROVIDER(css),
						  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
}

static void build_window(App *app)
{
	GtkSettings *settings = gtk_settings_get_default();

	g_object_set(settings, "gtk-application-prefer-dark-theme", FALSE, NULL);

	app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app->window), "LikeOS Setup");
	gtk_window_set_icon_name(GTK_WINDOW(app->window), "likeos-logo");
	gtk_style_context_add_class(gtk_widget_get_style_context(app->window),
				    "il-window");
	gtk_window_set_decorated(GTK_WINDOW(app->window), app->windowed);
	gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);

	app->overlay = gtk_overlay_new();
	gtk_container_add(GTK_CONTAINER(app->window), app->overlay);

	app->wiz = wizard_new(app);
	pages_set_wizard(app->wiz);
	gtk_container_add(GTK_CONTAINER(app->overlay), wizard_widget(app->wiz));

	g_signal_connect(app->window, "size-allocate", G_CALLBACK(on_size_allocate),
			 app);
	g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key_press),
			 app);
	g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete), app);
	g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(gtk_widget_get_screen(app->window), "size-changed",
			 G_CALLBACK(on_screen_changed), app);

	layout_cover_screen(app);
	{
		int w, h;

		gtk_window_get_default_size(GTK_WINDOW(app->window), &w, &h);
		layout_update(app, w, h);
	}
	gtk_widget_show_all(app->window);
	wizard_relayout(app->wiz);
}

/* ---- made-up answers for --page ------------------------------------ */

static void seed_answers(App *app, int page)
{
	if (page >= PAGE_DISK) {
		app->src_ok = il_source_probe(app->ctx, &app->src) == 0 &&
			app->src.present;
		if (!app->src_ok)
			g_strlcpy(app->src_error, il_last_error(app->ctx),
				  sizeof(app->src_error));
		app->disks = g_new0(struct il_disk_list, 1);
		app->disks_ok = il_disks_scan(app->ctx, app->disks) == 0;
		if (!app->disks_ok)
			g_strlcpy(app->disks_error, il_last_error(app->ctx),
				  sizeof(app->disks_error));
	}
	if (page > PAGE_DISK && app->disks) {
		for (int i = 0; i < app->disks->n; i++) {
			PlanStep whole = { PLAN_STEP_WHOLE, 0, 0, 0 };

			if (!plans_get(app, i))
				continue;
			if (plans_apply(app, i, &whole, NULL) == 0) {
				app->target_disk = i;
				break;
			}
		}
	}
	if (page > PAGE_REGION)
		g_strlcpy(app->cfg.timezone, "Europe/Berlin", sizeof(app->cfg.timezone));
	if (page > PAGE_LAYOUT) {
		app->cfg.nlayouts = 2;
		g_strlcpy(app->cfg.layouts[0], "de", sizeof(app->cfg.layouts[0]));
		g_strlcpy(app->cfg.variants[0], "", sizeof(app->cfg.variants[0]));
		g_strlcpy(app->cfg.layouts[1], "us", sizeof(app->cfg.layouts[1]));
		g_strlcpy(app->cfg.variants[1], "", sizeof(app->cfg.variants[1]));
		app_finish_keyboard(app);
	}
	if (page > PAGE_ROOT)
		g_strlcpy(app->cfg.root_password, "Root-Example-2026",
			  sizeof(app->cfg.root_password));
	if (page >= PAGE_ACCOUNT) {
		g_strlcpy(app->cfg.user.fullname, "Alex Example",
			  sizeof(app->cfg.user.fullname));
		g_strlcpy(app->cfg.user.username, "alex",
			  sizeof(app->cfg.user.username));
		g_strlcpy(app->cfg.user.password, "Example-Pass-77",
			  sizeof(app->cfg.user.password));
	}
	if (page == PAGE_ERROR) {
		app->failed_step = IL_STEP_EXTRACT;
		app->install_rc = -ENOSPC;
		g_strlcpy(app->install_error, "Could not write /mnt/likeos/usr/lib/"
			  "libwebkit2gtk-4.1.so.0: No space left on device.",
			  sizeof(app->install_error));
		app_log(app, IL_LOG_ERROR, "%s", app->install_error);
	}
	if (page == PAGE_FINISHED)
		app->installed = TRUE;
}

static void jump(App *app, int page, int substep)
{
	Page *p;

	seed_answers(app, page);
	wizard_seed_history(app->wiz, page);
	wizard_go(app->wiz, page, 0);
	p = wizard_current(app->wiz);
	for (int s = 0; s < substep; s++) {
		if (p->index == PAGE_LAYOUT && s == 1 && p->secondary)
			p->secondary(p);
		else
			wizard_next(app->wiz);
	}
}

/* ---- main ---------------------------------------------------------- */

int main(int argc, char **argv)
{
	App *app = g_new0(App, 1);
	GError *err = NULL;

	the_app = app;
	g_set_prgname("likeos-installer");
	g_set_application_name("LikeOS Setup");

	if (!gtk_init_with_args(&argc, &argv, "- install LikeOS on this computer",
				g_options, NULL, &err)) {
		g_printerr("likeos-installer: %s\n", err ? err->message :
			   "cannot open the display");
		g_clear_error(&err);
		return 1;
	}
	/* numbers in the program's text are written the one way */
	setlocale(LC_NUMERIC, "C");

	/* The backend: libinstaller where it is linked in, unless --mock. */
#ifdef INSTALLER_HAVE_REAL
	app->mock = opt_mock;
#else
	app->mock = TRUE;
	if (opt_simulate) {
		g_printerr("likeos-installer: --simulate needs libinstaller, "
			   "which this build does not have; it only has the "
			   "stand-in backend (--mock)\n");
		return 1;
	}
#endif
	if (app->mock && opt_simulate) {
		g_printerr("likeos-installer: --simulate and --mock exclude each "
			   "other\n");
		return 1;
	}
#ifdef INSTALLER_HAVE_REAL
	il_be = app->mock ? &il_backend_mock : &il_backend_real;
#else
	il_be = &il_backend_mock;
#endif
	if (opt_version) {
		g_print("likeos-installer (%s %s)\n", il_be->name, il_version());
		return 0;
	}
	app->exit_code = APP_EXIT_QUIT;
	app->simulate = opt_simulate != NULL;
	app->sim_disks = opt_simulate;
	app->source_path = opt_source;
	app->target_path = opt_target;
	app->log_path = opt_log;
	app->verbose = opt_verbose;
	app->windowed = opt_window;
	app->no_splash_delay = opt_fast;
	app->start_page = opt_page;
	if (opt_no_anim)
		art_set_animations(FALSE);
	if (opt_size && sscanf(opt_size, "%dx%d", &app->req_width,
			       &app->req_height) != 2) {
		g_printerr("likeos-installer: --size wants WxH, like 800x600\n");
		return 1;
	}
	if (app->req_width > 0 && app->req_width < 320)
		app->req_width = 320;
	if (app->req_height > 0 && app->req_height < 240)
		app->req_height = 240;

	app->bridge = bridge_new(app);
	app->ctx = bridge_create_context(app->bridge);
	if (!app->ctx) {
		g_printerr("likeos-installer: the installer backend did not start\n");
		return 1;
	}
	app_init_config(app);
	app_log(app, IL_LOG_INFO, "ui: likeos-installer started (%s%s%s)",
		il_be->name, app->simulate ? ", on image files" : "",
		app->windowed ? ", in a window" : "");
	keyboard_load(app);
	load_css();
	build_window(app);
	{
		const char *ap = opt_autopilot ? opt_autopilot :
			g_getenv("LIKEOS_INSTALLER_AUTOPILOT");

		if (ap && *ap && !autopilot_start(app, ap)) {
			il_ctx_free(app->ctx);
			return APP_EXIT_AUTOPILOT;
		}
	}

	if (app->start_page > 0 && app->start_page < PAGE_COUNT)
		jump(app, app->start_page, opt_substep);
	else
		wizard_go(app->wiz, PAGE_SPLASH, 0);

	gtk_main();

	app_log(app, IL_LOG_INFO, "ui: likeos-installer ends with status %d",
		app->exit_code);
	autopilot_free(app);
	bridge_shutdown(app->bridge);
	plans_free_all(app);
	g_free(app->disks);
	g_free(app->disk_message);
	if (app->kbd_cat)
		g_ptr_array_unref(app->kbd_cat);
	if (app->logo)
		g_object_unref(app->logo);
	il_ctx_free(app->ctx);
	bridge_free(app->bridge);
	{
		int code = app->exit_code;

		/* the choices hold the passwords */
		explicit_bzero(&app->cfg, sizeof(app->cfg));
		g_free(app);
		return code;
	}
}
