// LikeOS Task Viewer -- Start new task.
//
// What is typed is, in this order:
//
//   a web address or other URI ("https://...", "mailto:...", "file:///...",
//                   or "www...." without its scheme) opened by the program
//                   the desktop has for it;
//   a folder        opened in LikeOS Explorer;
//   a document      (an existing file that is not a program) opened by the
//                   program the desktop has for its type;
//   a command line  split into words the way a shell splits them (quotes
//                   and backslashes), with no further shell processing --
//                   no variables, globs, pipes or redirections -- and run.
//
// A program runs where it can be seen.  One the desktop has an entry for
// (by the entry's name, "mousepad" for org.xfce.mousepad.desktop, or by the
// program its Exec line runs) runs the way the entry says: in the terminal
// when it has Terminal=true.  Typed alone, it is started through the entry
// itself.  Any other program runs in the terminal when it is a terminal
// program -- an ELF program that needs no windowing library, or a script --
// and on its own otherwise (tv-launch.h).
//
// A leading ~ is the home folder.  Programs start in the home folder,
// detached from Task Viewer.  The last commands that worked are offered
// again in the field's list.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <gio/gdesktopappinfo.h>

#include "tv-app.h"
#include "tv-glyphs.h"
#include "tv-launch.h"
#include "tv-model.h"

#define HISTORY_KEY "run-history"
#define HISTORY_MAX 20
#define RESPONSE_BROWSE 1

static char *expand_home(const char *s)
{
	if (s[0] == '~' && (s[1] == '\0' || s[1] == '/'))
		return g_build_filename(g_get_home_dir(), s + 1, NULL);
	return g_strdup(s);
}

/* "scheme:" with a scheme of letters, digits, + - . and at least two
 * characters (one letter would be a drive, which this system does not
 * have, but is not a URI either). */
static gboolean is_uri(const char *s)
{
	char *scheme = g_uri_parse_scheme(s);
	gboolean yes = scheme && strlen(scheme) > 1 && !strchr(s, ' ');

	g_free(scheme);
	return yes;
}

static gboolean open_uri(const char *uri, GError **error)
{
	GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(
		gdk_display_get_default());
	gboolean ok = g_app_info_launch_default_for_uri(
		uri, G_APP_LAUNCH_CONTEXT(ctx), error);

	g_object_unref(ctx);
	return ok;
}

static gboolean spawn_in(const char *dir, char **argv, GError **error)
{
	return g_spawn_async(dir && *dir ? dir : g_get_home_dir(), argv, NULL,
			     G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
}

static gboolean spawn(char **argv, GError **error)
{
	return spawn_in(NULL, argv, error);
}

/* ---- programs and their desktop entries ----------------------------------- */

typedef enum { BY_NONE, BY_EXEC, BY_ID } Match;

/* The desktop entry for a typed program name: one whose name is the word
 * ("org.xfce.mousepad", "mousepad"), else one whose Exec line runs a
 * program of that name.  The entries the menus show win over hidden helper
 * entries for the same program. */
static GDesktopAppInfo *find_entry(const char *word, Match *how)
{
	GList *all = g_app_info_get_all();
	char *base = g_path_get_basename(word);
	char *key = g_ascii_strdown(base, -1);
	GDesktopAppInfo *found = NULL;

	*how = BY_NONE;
	if (g_str_has_suffix(key, ".desktop"))
		key[strlen(key) - strlen(".desktop")] = '\0';
	/* A name only when typed as a name, not as a path. */
	for (int pass = 0; pass < 4 && !found; pass++) {
		gboolean shown = pass % 2 == 0;
		gboolean by_id = pass < 2;

		if (by_id && strchr(word, '/'))
			continue;
		for (GList *l = all; l && !found; l = l->next) {
			GAppInfo *a = l->data;
			gboolean hit = FALSE;

			if (!G_IS_DESKTOP_APP_INFO(a) ||
			    g_app_info_should_show(a) != shown)
				continue;
			if (by_id) {
				char **keys =
					tv_desktop_id_keys(g_app_info_get_id(a));

				for (char **k = keys; *k && !hit; k++)
					hit = strcmp(*k, key) == 0;
				g_strfreev(keys);
			} else {
				char *bin = tv_exec_basename(
					g_app_info_get_commandline(a));

				hit = bin && strcmp(bin, key) == 0;
				g_free(bin);
			}
			if (hit) {
				found = g_object_ref(G_DESKTOP_APP_INFO(a));
				*how = by_id ? BY_ID : BY_EXEC;
			}
		}
	}
	g_list_free_full(all, g_object_unref);
	g_free(key);
	g_free(base);
	return found;
}

/* The entry's own command, run in the terminal; `extra' after it. */
static gboolean launch_entry_in_terminal(GDesktopAppInfo *info,
					 char **extra, GError **error)
{
	char *exec = g_desktop_app_info_get_string(info, "Exec");
	char *dir = g_desktop_app_info_get_string(info, "Path");
	char **words = tv_exec_argv(exec);
	GPtrArray *cmd = g_ptr_array_new();
	char **argv;
	gboolean ok;

	if (!words) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "The program’s desktop entry names no program.");
		g_ptr_array_unref(cmd);
		g_free(dir);
		g_free(exec);
		return FALSE;
	}
	for (char **w = words; *w; w++)
		g_ptr_array_add(cmd, *w);
	for (char **w = extra; w && *w; w++)
		g_ptr_array_add(cmd, *w);
	g_ptr_array_add(cmd, NULL);
	argv = tv_terminal_argv((const char *const *)cmd->pdata);
	ok = spawn_in(dir, argv, error);
	g_strfreev(argv);
	g_ptr_array_unref(cmd);
	g_strfreev(words);
	g_free(dir);
	g_free(exec);
	return ok;
}

/* Starts the entry itself, with `files' (words typed after its name) as
 * the files it opens. */
static gboolean launch_entry(GDesktopAppInfo *info, char **files,
			     GError **error)
{
	GdkAppLaunchContext *ctx;
	GList *list = NULL;
	gboolean ok;

	if (g_desktop_app_info_get_boolean(info, "Terminal"))
		return launch_entry_in_terminal(info, files, error);
	for (char **f = files; f && *f; f++)
		list = g_list_append(list, g_file_new_for_commandline_arg_and_cwd(
						   *f, g_get_home_dir()));
	ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
	ok = g_app_info_launch(G_APP_INFO(info), list,
			       G_APP_LAUNCH_CONTEXT(ctx), error);
	g_object_unref(ctx);
	g_list_free_full(list, g_object_unref);
	return ok;
}

/* Whether the entry's Exec line is its program alone: started through the
 * entry, it then runs what was typed. */
static gboolean entry_runs_program_alone(GDesktopAppInfo *info)
{
	char *exec = g_desktop_app_info_get_string(info, "Exec");
	char **words = tv_exec_argv(exec);
	gboolean alone = words && words[0] && !words[1];

	g_strfreev(words);
	g_free(exec);
	return alone;
}

/* The program file a typed word names: the word itself when it is a path,
 * else found on PATH.  NULL when there is none. */
static char *program_file(const char *word)
{
	if (strchr(word, '/'))
		return g_file_test(word, G_FILE_TEST_IS_EXECUTABLE) &&
				       !g_file_test(word, G_FILE_TEST_IS_DIR) ?
			       g_strdup(word) :
			       NULL;
	return g_find_program_in_path(word);
}

/* Runs a command: argv[0] as typed, the rest its arguments, passed on as
 * they are. */
static gboolean run_program(char **argv, GError **error)
{
	Match how;
	GDesktopAppInfo *entry = find_entry(argv[0], &how);
	char *prog = program_file(argv[0]);
	gboolean terminal, ok;

	if (entry && (argv[1] == NULL || !prog) &&
	    (how == BY_ID || entry_runs_program_alone(entry))) {
		/* Typed alone (or with files for it): the entry runs. */
		ok = launch_entry(entry, argv + 1, error);
		g_object_unref(entry);
		g_free(prog);
		return ok;
	}
	if (!prog) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			    "“%s” cannot be found.  Check that the "
			    "name is typed correctly, then try again.",
			    argv[0]);
		g_clear_object(&entry);
		return FALSE;
	}
	g_free(argv[0]);
	argv[0] = prog;
	/* The entry knows best; without one, the program file tells. */
	terminal = entry ? g_desktop_app_info_get_boolean(entry, "Terminal") :
			   tv_program_kind_needs_terminal(tv_program_kind(prog));
	g_clear_object(&entry);
	if (terminal) {
		char **cmd = tv_terminal_argv((const char *const *)argv);

		ok = spawn(cmd, error);
		g_strfreev(cmd);
		return ok;
	}
	return spawn(argv, error);
}

static gboolean open_path(const char *path, GError **error)
{
	if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
		char *argv[] = { "likeos-explorer", (char *)path, NULL };

		return spawn(argv, error);
	}
	if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) {
		char *argv[] = { g_strdup(path), NULL };
		gboolean ok = run_program(argv, error);

		g_free(argv[0]);
		return ok;
	}
	{
		GFile *f = g_file_new_for_path(path);
		char *uri = g_file_get_uri(f);
		gboolean ok = open_uri(uri, error);

		g_free(uri);
		g_object_unref(f);
		return ok;
	}
}

static gboolean run(const char *cmdline, GError **error)
{
	char *text = g_strstrip(g_strdup(cmdline ? cmdline : ""));
	char *path = NULL;
	char **argv = NULL;
	int argc = 0;
	gboolean ok = FALSE;

	if (!*text) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			    "Nothing was typed.");
		goto out;
	}
	if (is_uri(text)) {
		ok = open_uri(text, error);
		goto out;
	}
	/* A web address typed the short way. */
	if (g_str_has_prefix(text, "www.") && !strchr(text, ' ')) {
		char *uri = g_strconcat("https://", text, NULL);

		ok = open_uri(uri, error);
		g_free(uri);
		goto out;
	}
	/* The whole text naming something that exists, spaces and all. */
	path = expand_home(text);
	if (g_file_test(path, G_FILE_TEST_EXISTS) &&
	    (g_path_is_absolute(path) || strchr(path, '/'))) {
		ok = open_path(path, error);
		goto out;
	}
	g_clear_pointer(&path, g_free);

	if (!g_shell_parse_argv(text, &argc, &argv, error))
		goto out;
	for (int i = 0; i < argc; i++) {
		char *e = expand_home(argv[i]);

		g_free(argv[i]);
		argv[i] = e;
	}
	/* One word that is a folder or a document, quoted or not. */
	if (argc == 1 && strchr(argv[0], '/') &&
	    g_file_test(argv[0], G_FILE_TEST_EXISTS)) {
		ok = open_path(argv[0], error);
		goto out;
	}
	ok = run_program(argv, error);
out:
	g_strfreev(argv);
	g_free(path);
	g_free(text);
	return ok;
}

/* With `error', a failure is reported there; without, in a dialog. */
gboolean tv_run_command(const char *cmdline, GError **error)
{
	GError *err = NULL;

	if (run(cmdline, error ? error : &err))
		return TRUE;
	if (!error) {
		tv_show_error("The task could not be started.", err);
		g_clear_error(&err);
	}
	return FALSE;
}

/* ---- the dialog ------------------------------------------------------------ */

typedef struct {
	GtkWidget *dialog;
	GtkWidget *combo;
	GtkWidget *entry;
} Run;

static void run_error(GtkWidget *parent, const GError *err)
{
	GtkWidget *d = gtk_message_dialog_new(
		GTK_WINDOW(parent), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
		"The task could not be started.");

	if (err)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", err->message);
	gtk_window_set_title(GTK_WINDOW(d), "Start new task");
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static void history_add(const char *cmd)
{
	char **old = tv_settings_get_strv(HISTORY_KEY);
	GPtrArray *h = g_ptr_array_new();

	g_ptr_array_add(h, (gpointer)cmd);
	for (char **o = old; *o && h->len < HISTORY_MAX; o++)
		if (strcmp(*o, cmd) != 0)
			g_ptr_array_add(h, *o);
	g_ptr_array_add(h, NULL);
	tv_settings_set_strv(HISTORY_KEY, (const char *const *)h->pdata);
	tv_settings_save();
	g_ptr_array_unref(h);
	g_strfreev(old);
}

static void entry_changed(GtkEditable *e, gpointer data)
{
	Run *r = data;
	char *t = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));

	gtk_dialog_set_response_sensitive(GTK_DIALOG(r->dialog),
					  GTK_RESPONSE_OK, *t != '\0');
	g_free(t);
}

static void browse(Run *r)
{
	GtkWidget *fc = gtk_file_chooser_dialog_new(
		"Browse", GTK_WINDOW(r->dialog), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT,
		NULL);
	GtkFileFilter *progs = gtk_file_filter_new();
	GtkFileFilter *all = gtk_file_filter_new();

	gtk_file_filter_set_name(progs, "Programs");
	gtk_file_filter_add_mime_type(progs, "application/x-executable");
	gtk_file_filter_add_mime_type(progs, "application/x-sharedlib");
	gtk_file_filter_add_mime_type(progs, "application/x-shellscript");
	gtk_file_filter_add_mime_type(progs, "application/x-desktop");
	gtk_file_filter_set_name(all, "All files");
	gtk_file_filter_add_pattern(all, "*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), progs);
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), all);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), "/usr/bin");
	if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
		char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));

		if (file) {
			/* Quoted when it has to be, so the words split back
			 * into the same path. */
			char *q = strpbrk(file, " \t'\"\\$`") ?
					  g_shell_quote(file) :
					  g_strdup(file);

			gtk_entry_set_text(GTK_ENTRY(r->entry), q);
			gtk_editable_set_position(GTK_EDITABLE(r->entry), -1);
			g_free(q);
			g_free(file);
		}
	}
	gtk_widget_destroy(fc);
	gtk_widget_grab_focus(r->entry);
}

void tv_run_dialog(GtkWindow *parent)
{
	static GtkWidget *open_dialog;
	Run r = { NULL, NULL, NULL };
	GtkWidget *box, *top, *text, *row, *label;
	char **hist;
	int resp;

	if (open_dialog) {
		gtk_window_present(GTK_WINDOW(open_dialog));
		return;
	}
	r.dialog = gtk_dialog_new_with_buttons(
		"Start new task", parent,
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_OK",
		GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, "_Browse…",
		RESPONSE_BROWSE, NULL);
	open_dialog = r.dialog;
	g_object_add_weak_pointer(G_OBJECT(r.dialog), (gpointer *)&open_dialog);
	gtk_window_set_resizable(GTK_WINDOW(r.dialog), FALSE);
	gtk_dialog_set_default_response(GTK_DIALOG(r.dialog), GTK_RESPONSE_OK);

	box = gtk_dialog_get_content_area(GTK_DIALOG(r.dialog));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 14);

	top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
	gtk_box_pack_start(GTK_BOX(top), tv_glyph_new(TV_GLYPH_RUN, 32), FALSE,
			   FALSE, 0);
	text = gtk_label_new(
		"Enter a program, file, folder or web address to open.");
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(text), 48);
	gtk_box_pack_start(GTK_BOX(top), text, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	label = gtk_label_new_with_mnemonic("_Open:");
	r.combo = gtk_combo_box_text_new_with_entry();
	r.entry = gtk_bin_get_child(GTK_BIN(r.combo));
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), r.entry);
	gtk_entry_set_activates_default(GTK_ENTRY(r.entry), TRUE);
	gtk_entry_set_width_chars(GTK_ENTRY(r.entry), 40);
	hist = tv_settings_get_strv(HISTORY_KEY);
	for (char **h = hist; *h; h++)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(r.combo), *h);
	/* The last command, selected, ready to be run again or typed over. */
	if (hist[0]) {
		gtk_entry_set_text(GTK_ENTRY(r.entry), hist[0]);
		gtk_editable_select_region(GTK_EDITABLE(r.entry), 0, -1);
	}
	g_strfreev(hist);
	gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), r.combo, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	g_signal_connect(r.entry, "changed", G_CALLBACK(entry_changed), &r);

	gtk_widget_show_all(r.dialog);
	entry_changed(GTK_EDITABLE(r.entry), &r);
	gtk_widget_grab_focus(r.entry);

	while ((resp = gtk_dialog_run(GTK_DIALOG(r.dialog))) ==
		       RESPONSE_BROWSE ||
	       resp == GTK_RESPONSE_OK) {
		char *cmd;
		GError *err = NULL;

		if (resp == RESPONSE_BROWSE) {
			browse(&r);
			continue;
		}
		cmd = g_strstrip(
			g_strdup(gtk_entry_get_text(GTK_ENTRY(r.entry))));
		if (!*cmd) {
			g_free(cmd);
			continue;
		}
		if (tv_run_command(cmd, &err)) {
			history_add(cmd);
			g_free(cmd);
			break;
		}
		run_error(r.dialog, err);
		g_clear_error(&err);
		g_free(cmd);
		gtk_widget_grab_focus(r.entry);
	}
	if (open_dialog)
		gtk_widget_destroy(r.dialog);
}
