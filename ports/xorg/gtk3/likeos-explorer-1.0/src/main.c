// LikeOS Explorer: the program.
//
// One process per session: a second "likeos-explorer" hands its arguments
// to the running one over the session bus and exits, so every window shares
// one operation queue, one undo history and one clipboard view.  Without a
// session bus each invocation is a process of its own.
//
//     likeos-explorer [LOCATION...]
//     likeos-explorer --copy-to=FOLDER | --move-to=FOLDER | --link-to=FOLDER
//                     | --trash | --delete  FILE...
//     likeos-explorer --empty-trash
//
// Each invocation opens a window -- with a tab per location given, or the
// start location.  A location is a folder (path, URI or ~/...), a file (its
// folder opens with it selected), or trash:///, computer:///, likeos:home.
//
// With one of the operation options it opens no window: the operation runs
// in the operations window, queued with every other one, and the program
// keeps running until it is done.  The desktop hands its copies, moves and
// deletions over this way, so they show the same window as those started
// in Explorer.  Nothing is asked before --delete or --empty-trash: the
// caller has asked.
//
// Copyright (C) 2026 The LikeOS Project

#include <gtk/gtk.h>

#include "backend/ex-fileops.h"
#include "backend/ex-index.h"
#include "backend/ex-location.h"
#include "services/ex-settings.h"
#include "ui/ex-actions.h"
#include "ui/ex-operation-center.h"
#include "ui/ex-style.h"
#include "ui/ex-tab.h"
#include "ui/ex-window.h"

#include <string.h>

/* Exit statuses.  1 is what GLib reports for a command line that never
 * reached the running program (see main()); a command line the program
 * refused is 2. */
#define EX_STATUS_LOST 1
#define EX_STATUS_USAGE 2
/* How often a lost command line is tried again. */
#define EX_LOST_RETRIES 3

/* Whether this process is the running program, not one handing its
 * command line to it. */
static gboolean primary;

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

/* The operation options: what each one makes of the files, and whether
 * it names a folder. */
static const struct {
	const char *name;
	gboolean to_folder;
} op_options[] = {
	{ "copy-to", TRUE },  { "move-to", TRUE }, { "link-to", TRUE },
	{ "trash", FALSE },   { "delete", FALSE }, { "empty-trash", FALSE },
};

/* An operation option on the command line: the operation queued, no
 * window opened.  0 when it was, EX_STATUS_USAGE (with a message) when it
 * could not be; -1 when the command line asks for no operation. */
static int run_operation(GApplicationCommandLine *cl, GVariantDict *opts,
			 int argc, char **argv)
{
	const char *which = NULL, *folder = NULL;
	GPtrArray *files;
	GFile *dest = NULL;
	ExOperation *op = NULL;
	gboolean undoable = TRUE;

	for (guint i = 0; i < G_N_ELEMENTS(op_options); i++) {
		if (!g_variant_dict_contains(opts, op_options[i].name))
			continue;
		if (which) {
			g_application_command_line_printerr(
				cl, "likeos-explorer: --%s and --%s cannot be "
				    "given together\n",
				which, op_options[i].name);
			return EX_STATUS_USAGE;
		}
		which = op_options[i].name;
		if (op_options[i].to_folder)
			g_variant_dict_lookup(opts, which, "^&ay", &folder);
	}
	if (!which)
		return -1;

	files = g_ptr_array_new_with_free_func(g_object_unref);
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--"))
			g_ptr_array_add(files,
					g_application_command_line_create_file_for_arg(
						cl, argv[i]));
	if (folder) {
		dest = g_application_command_line_create_file_for_arg(cl, folder);
		if (g_file_query_file_type(dest, G_FILE_QUERY_INFO_NONE, NULL) !=
		    G_FILE_TYPE_DIRECTORY) {
			g_application_command_line_printerr(
				cl, "likeos-explorer: “%s” is not a folder\n",
				folder);
			goto out;
		}
	}
	if (!strcmp(which, "empty-trash")) {
		op = ex_operation_new_empty_trash();
		undoable = FALSE;
	} else if (!files->len) {
		g_application_command_line_printerr(
			cl, "likeos-explorer: --%s needs the files to work on\n",
			which);
		goto out;
	} else if (!strcmp(which, "copy-to")) {
		op = ex_operation_new_copy(files, dest);
	} else if (!strcmp(which, "move-to")) {
		op = ex_operation_new_move(files, dest);
	} else if (!strcmp(which, "link-to")) {
		op = ex_operation_new_link(files, dest);
	} else if (!strcmp(which, "trash")) {
		op = ex_operation_new_trash(files);
	} else {
		op = ex_operation_new_delete(files);
		undoable = FALSE;
	}
	ex_operation_center_run(op, undoable);
	g_object_unref(op);
out:
	g_clear_object(&dest);
	g_ptr_array_unref(files);
	return op ? 0 : EX_STATUS_USAGE;
}

static int command_line(GApplication *app, GApplicationCommandLine *cl,
			gpointer data)
{
	int argc, status = 0;
	char **argv = g_application_command_line_get_arguments(cl, &argc);
	GVariantDict *opts = g_application_command_line_get_options_dict(cl);

	if (g_variant_dict_contains(opts, "version"))
		g_application_command_line_print(cl, "likeos-explorer %s\n",
						 EX_VERSION);
	else if ((status = run_operation(cl, opts, argc, argv)) < 0) {
		open_window(GTK_APPLICATION(app), cl, argc, argv);
		status = 0;
	}
	g_strfreev(argv);
	return status;
}

static void startup(GApplication *app, gpointer data)
{
	ExSettings *s = ex_settings_get();

	primary = TRUE;
	gtk_window_set_default_icon_name("system-file-manager");
	ex_style_init();
	ex_actions_set_accels(GTK_APPLICATION(app));
	ex_operation_center_init();
	ex_index_set_enabled(ex_index_get_default(),
			     ex_settings_get_bool(s, "index-home"));
	ex_index_ensure(ex_index_get_default());
}

static const GOptionEntry options[] = {
	{ "new-window", 'n', 0, G_OPTION_ARG_NONE, NULL,
	  "Open a new window (what every invocation does)", NULL },
	{ "version", 0, 0, G_OPTION_ARG_NONE, NULL, "Show the version and exit",
	  NULL },
	{ "copy-to", 0, 0, G_OPTION_ARG_FILENAME, NULL,
	  "Copy the FILEs into FOLDER", "FOLDER" },
	{ "move-to", 0, 0, G_OPTION_ARG_FILENAME, NULL,
	  "Move the FILEs into FOLDER", "FOLDER" },
	{ "link-to", 0, 0, G_OPTION_ARG_FILENAME, NULL,
	  "Make links to the FILEs in FOLDER", "FOLDER" },
	{ "trash", 0, 0, G_OPTION_ARG_NONE, NULL, "Move the FILEs to the Trash",
	  NULL },
	{ "delete", 0, 0, G_OPTION_ARG_NONE, NULL,
	  "Delete the FILEs permanently, without asking", NULL },
	{ "empty-trash", 0, 0, G_OPTION_ARG_NONE, NULL,
	  "Empty the Trash, without asking", NULL },
	{ NULL },
};

static int run(int argc, char **argv, GApplicationFlags flags)
{
	GtkApplication *app = gtk_application_new("org.likeos.Explorer", flags);
	int status;

	g_application_add_main_option_entries(G_APPLICATION(app), options);
	g_application_set_option_context_parameter_string(
		G_APPLICATION(app), "[LOCATION...] | OPERATION FILE...");
	g_application_set_option_context_summary(
		G_APPLICATION(app),
		"Open a " EX_APP_NAME " window, with a tab for each location: a "
		"folder,\na file (shown selected in its folder), trash:///, "
		"computer:///, likeos:computer or likeos:home.\n\n"
		"With an operation option, run that operation on the FILEs in "
		"the file\noperations window instead, and open no window.");
	g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
	g_signal_connect(app, "command-line", G_CALLBACK(command_line), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}

int main(int argc, char **argv)
{
	GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE;
	int status;

	g_set_prgname("likeos-explorer");
	g_set_application_name(EX_APP_NAME);
	/* No session bus: no single instance, rather than no program. */
	if (!g_getenv("DBUS_SESSION_BUS_ADDRESS"))
		flags |= G_APPLICATION_NON_UNIQUE;
	/* A command line handed to the running program just as that ends --
	 * its last operation done, its last window closed -- is never read:
	 * the call comes back unanswered, and this process starts over, by
	 * then the running program itself or handing over to a new one. */
	for (int tries = 0;; tries++) {
		status = run(argc, argv, flags);
		if (primary || status != EX_STATUS_LOST ||
		    tries >= EX_LOST_RETRIES)
			break;
	}
	return status;
}
