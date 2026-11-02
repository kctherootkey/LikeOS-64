// LikeOS -- File Explorer: the program.
//
// One process per session: a second "likeos-explorer" hands its arguments
// to the running one over the session bus and exits, so every window shares
// one operation queue, one undo history and one clipboard view.  Without a
// session bus each invocation is a process of its own.
//
//     likeos-explorer [LOCATION...]
//
// Each invocation opens a window -- with a tab per location given, or the
// start location.  A location is a folder (path, URI or ~/...), a file (its
// folder opens with it selected), or trash:///, computer:///, likeos:home.
//
// Copyright (C) 2026 The LikeOS Project

#include <gtk/gtk.h>

#include "backend/ex-index.h"
#include "backend/ex-location.h"
#include "services/ex-settings.h"
#include "ui/ex-actions.h"
#include "ui/ex-operation-center.h"
#include "ui/ex-style.h"
#include "ui/ex-tab.h"
#include "ui/ex-window.h"

#include <string.h>

static gboolean special(const char *arg)
{
	return g_str_has_prefix(arg, "likeos:") || g_str_has_prefix(arg, "trash:") ||
	       g_str_has_prefix(arg, "computer:") || strstr(arg, "://");
}

/* A window for the command line's locations. */
static void open_window(GtkApplication *app, GApplicationCommandLine *cl,
			int argc, char **argv)
{
	ExWindow *w = NULL;
	GPtrArray *select = g_ptr_array_new_with_free_func(g_object_unref);

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		ExLocation *l = NULL;

		if (!strcmp(a, "--new-window") || !strcmp(a, "--"))
			continue;
		if (special(a)) {
			l = ex_location_parse(a);
		} else {
			GFile *f = g_application_command_line_create_file_for_arg(cl, a);
			GFileType t = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE,
							     NULL);

			/* A file: its folder, with it selected. */
			if (t != G_FILE_TYPE_DIRECTORY && t != G_FILE_TYPE_UNKNOWN) {
				GFile *p = g_file_get_parent(f);

				if (p) {
					l = ex_location_new_folder(p);
					g_ptr_array_add(select, g_object_ref(f));
					g_object_unref(p);
				}
			} else {
				l = ex_location_new_folder(f);
			}
			g_object_unref(f);
		}
		if (!l) {
			g_application_command_line_printerr(
				cl, "likeos-explorer: “%s” is not a location\n", a);
			continue;
		}
		if (!w)
			w = ex_window_new(app, l);
		else
			ex_window_add_tab(w, l, FALSE);
		ex_location_unref(l);
	}
	if (!w) {
		char *start = ex_settings_get_string(ex_settings_get(), "start-location");
		ExLocation *l = ex_location_parse(start);

		if (!l)
			l = ex_location_new_special(EX_LOC_HOME);
		w = ex_window_new(app, l);
		ex_location_unref(l);
		g_free(start);
	}
	if (select->len)
		ex_tab_select_files(ex_window_current_tab(w), select);
	g_ptr_array_unref(select);
	gtk_window_present(GTK_WINDOW(w));
}

static int command_line(GApplication *app, GApplicationCommandLine *cl,
			gpointer data)
{
	int argc;
	char **argv = g_application_command_line_get_arguments(cl, &argc);
	GVariantDict *opts = g_application_command_line_get_options_dict(cl);

	if (g_variant_dict_contains(opts, "version"))
		g_application_command_line_print(cl, "likeos-explorer %s\n",
						 EX_VERSION);
	else
		open_window(GTK_APPLICATION(app), cl, argc, argv);
	g_strfreev(argv);
	return 0;
}

static void startup(GApplication *app, gpointer data)
{
	ExSettings *s = ex_settings_get();

	gtk_window_set_default_icon_name("system-file-manager");
	ex_style_init();
	ex_actions_set_accels(GTK_APPLICATION(app));
	ex_operation_center_init();
	ex_index_set_enabled(ex_index_get_default(),
			     ex_settings_get_bool(s, "index-home"));
	ex_index_ensure(ex_index_get_default());
}

int main(int argc, char **argv)
{
	GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE;
	GtkApplication *app;
	int status;

	g_set_prgname("likeos-explorer");
	g_set_application_name("File Explorer");
	/* No session bus: no single instance, rather than no program. */
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
		flags |= G_APPLICATION_NON_UNIQUE;
	static const GOptionEntry options[] = {
		{ "new-window", 'n', 0, G_OPTION_ARG_NONE, NULL,
		  "Open a new window (what every invocation does)", NULL },
		{ "version", 0, 0, G_OPTION_ARG_NONE, NULL,
		  "Show the version and exit", NULL },
		{ NULL },
	};

	app = gtk_application_new("org.likeos.Explorer", flags);
	g_application_add_main_option_entries(G_APPLICATION(app), options);
	g_application_set_option_context_parameter_string(G_APPLICATION(app),
							  "[LOCATION...]");
	g_application_set_option_context_summary(
		G_APPLICATION(app),
		"Open a File Explorer window, with a tab for each location: a "
		"folder,\na file (shown selected in its folder), trash:///, "
		"computer:/// or likeos:home.");
	g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
