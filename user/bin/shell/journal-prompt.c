// LikeOS -- likeos-journal-prompt, the question before a journal is
// replayed.
//
// A drive whose file system was not closed properly (pulled out, or the
// computer turned off, before everything was written; for NTFS also a
// Windows that was not shut down normally) still holds changes in its
// journal.  devd does not replay it by itself: the mount is refused with
// EUCLEAN, and the session's volume agent or the file manager runs this
// dialog to ask the user.
//
//   likeos-journal-prompt --device sdb4 --fstype ext4 [--label TEXT]
//                         [--size BYTES] [--drive "MODEL"]
//
// Exit status: 0 the user chose to replay the journal, 1 declined (or
// closed the window, or there is no display), 2 bad usage.
//
// Every text in the arguments comes from the drive and is treated as
// untrusted: made valid UTF-8, control and formatting characters dropped,
// cut to a bounded length, and only ever shown as plain text (no markup).
//
// Copyright (C) 2026 The LikeOS Project

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_REPLAY 0
#define EXIT_DECLINE 1
#define EXIT_USAGE 2

#define DEVICE_MAX 31  /* bytes of a device name */
#define FSTYPE_MAX 15  /* bytes of a file system id */
#define LABEL_CHARS 64 /* characters of a label shown */
#define DRIVE_CHARS 64 /* characters of a drive model shown */
#define ARG_BYTES 1024 /* longest argument taken at all */

static void usage(void)
{
	fprintf(stderr,
		"usage: likeos-journal-prompt --device NAME --fstype TYPE "
		"[--label TEXT] [--size BYTES] [--drive MODEL]\n");
	exit(EXIT_USAGE);
}

/* A device name as devd writes it: a-z, A-Z, 0-9, '_' and '-'. */
static gboolean device_ok(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n > DEVICE_MAX)
		return FALSE;
	for (const char *p = s; *p; p++)
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
			return FALSE;
	return TRUE;
}

/* A file system id: a-z and 0-9. */
static gboolean fstype_ok(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n > FSTYPE_MAX)
		return FALSE;
	for (const char *p = s; *p; p++)
		if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p))
			return FALSE;
	return TRUE;
}

/* Untrusted text made fit to show: valid UTF-8, no control or formatting
 * characters (no line breaks, no direction overrides), blanks at the ends
 * gone, at most `max' characters (the rest replaced by an ellipsis). */
static char *clean_text(const char *in, glong max)
{
	char *valid = g_utf8_make_valid(in, -1);
	GString *out = g_string_new(NULL);
	glong chars = 0;
	char *res;

	for (const char *p = valid; *p; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		GUnicodeType t = g_unichar_type(c);

		if (t == G_UNICODE_CONTROL || t == G_UNICODE_FORMAT ||
		    t == G_UNICODE_LINE_SEPARATOR ||
		    t == G_UNICODE_PARAGRAPH_SEPARATOR ||
		    t == G_UNICODE_SURROGATE || t == G_UNICODE_UNASSIGNED ||
		    t == G_UNICODE_PRIVATE_USE)
			continue;
		if (chars == max) {
			g_string_append(out, "\xe2\x80\xa6"); /* … */
			break;
		}
		g_string_append_unichar(out, c);
		chars++;
	}
	g_free(valid);
	res = g_strstrip(g_string_free(out, FALSE));
	return res;
}

/* Bytes in decimal units, one decimal: "62.7 GB". */
static char *size_text(guint64 b)
{
	if (b >= 1000000000000ull)
		return g_strdup_printf("%.1f TB", (double)b / 1e12);
	if (b >= 1000000000ull)
		return g_strdup_printf("%.1f GB", (double)b / 1e9);
	if (b >= 1000000ull)
		return g_strdup_printf("%.1f MB", (double)b / 1e6);
	return g_strdup_printf("%.1f kB", (double)b / 1e3);
}

static const char *fs_name(const char *fstype)
{
	if (strcmp(fstype, "ntfs") == 0)
		return "NTFS";
	if (strcmp(fstype, "vfat") == 0)
		return "FAT32";
	if (strcmp(fstype, "exfat") == 0)
		return "exFAT";
	return fstype; /* "ext4" */
}

/* A label the text wraps by itself; plain text, never markup. */
static GtkWidget *text_label(const char *text)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 58);
	gtk_label_set_width_chars(GTK_LABEL(l), 50);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_selectable(GTK_LABEL(l), FALSE);
	gtk_widget_set_halign(l, GTK_ALIGN_FILL);
	return l;
}

static void set_attrs(GtkWidget *l, double scale, gboolean bold)
{
	PangoAttrList *a = pango_attr_list_new();

	pango_attr_list_insert(a, pango_attr_scale_new(scale));
	if (bold)
		pango_attr_list_insert(a, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
	gtk_label_set_attributes(GTK_LABEL(l), a);
	pango_attr_list_unref(a);
}

int main(int argc, char **argv)
{
	const char *device = NULL, *fstype = NULL, *label_arg = "";
	const char *size_arg = NULL, *drive_arg = "";
	guint64 size = 0;
	char *label, *drive, *name, *sz = NULL, *heading, *body1;
	const char *fs;
	GString *details;
	GtkWidget *dlg, *content, *row, *icon, *col, *w, *replay;
	gint response;

	for (int i = 1; i < argc; i++) {
		const char *opt = argv[i], *val;

		if (i + 1 >= argc)
			usage();
		val = argv[++i];
		if (strlen(val) > ARG_BYTES)
			usage();
		if (strcmp(opt, "--device") == 0)
			device = val;
		else if (strcmp(opt, "--fstype") == 0)
			fstype = val;
		else if (strcmp(opt, "--label") == 0)
			label_arg = val;
		else if (strcmp(opt, "--size") == 0)
			size_arg = val;
		else if (strcmp(opt, "--drive") == 0)
			drive_arg = val;
		else
			usage();
	}
	if (!device || !fstype || !device_ok(device) || !fstype_ok(fstype))
		usage();
	if (size_arg) {
		if (!*size_arg || strlen(size_arg) > 20 ||
		    strspn(size_arg, "0123456789") != strlen(size_arg) ||
		    !g_ascii_string_to_unsigned(size_arg, 10, 0, G_MAXUINT64,
						&size, NULL))
			usage();
	}

	/* No display: nobody can be asked, so nothing is replayed. */
	if (!gtk_init_check(NULL, NULL))
		return EXIT_DECLINE;

	label = clean_text(label_arg, LABEL_CHARS);
	drive = clean_text(drive_arg, DRIVE_CHARS);
	fs = fs_name(fstype);
	if (size)
		sz = size_text(size);
	/* What the volume is called: its label, else "62.7 GB Volume", else
	 * its device name. */
	if (*label)
		name = g_strdup(label);
	else if (sz)
		name = g_strdup_printf("%s Volume", sz);
	else
		name = g_strdup(device);

	heading = g_strdup_printf("Replay the journal of \xe2\x80\x9c%s\xe2\x80\x9d?",
				  name);
	if (strcmp(fstype, "ntfs") == 0)
		body1 = g_strdup_printf(
			"The %s file system on \xe2\x80\x9c%s\xe2\x80\x9d was not "
			"closed properly. Windows was probably not shut down "
			"normally, or the drive was removed before all changes "
			"were written to it.",
			fs, name);
	else
		body1 = g_strdup_printf(
			"The %s file system on \xe2\x80\x9c%s\xe2\x80\x9d was not "
			"closed properly. The drive was probably removed, or the "
			"computer was turned off, before all changes were "
			"written to it.",
			fs, name);

	details = g_string_new(NULL);
	g_string_append_printf(details, "Device: /dev/%s", device);
	g_string_append_printf(details, " \xc2\xb7 File system: %s", fs);
	if (*drive)
		g_string_append_printf(details, " \xc2\xb7 Drive: %s", drive);
	if (sz)
		g_string_append_printf(details, " \xc2\xb7 Size: %s", sz);

	dlg = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(dlg), "Drive Needs Recovery");
	gtk_window_set_icon_name(GTK_WINDOW(dlg), "dialog-warning");
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
	gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
	gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);

	gtk_dialog_add_button(GTK_DIALOG(dlg), "Don\xe2\x80\x99t Replay",
			      GTK_RESPONSE_CANCEL);
	replay = gtk_dialog_add_button(GTK_DIALOG(dlg), "Replay Journal",
				       GTK_RESPONSE_ACCEPT);
	gtk_style_context_add_class(gtk_widget_get_style_context(replay),
				    "suggested-action");
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

	content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	gtk_container_set_border_width(GTK_CONTAINER(content), 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	gtk_container_set_border_width(GTK_CONTAINER(row), 18);
	icon = gtk_image_new_from_icon_name("dialog-warning",
					    GTK_ICON_SIZE_DIALOG);
	gtk_widget_set_valign(icon, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

	col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	w = text_label(heading);
	set_attrs(w, PANGO_SCALE_LARGE, TRUE);
	gtk_box_pack_start(GTK_BOX(col), w, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), text_label(body1), FALSE, FALSE, 0);
	gtk_box_pack_start(
		GTK_BOX(col),
		text_label("Its journal still holds these changes. Replaying "
			   "the journal writes them to the drive so that the "
			   "file system is consistent again. This is normally "
			   "harmless and is what any operating system does "
			   "before using such a drive, but it does write to "
			   "the drive."),
		FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col),
			   text_label("If you do not replay the journal, the "
				      "drive is not opened."),
			   FALSE, FALSE, 0);
	w = text_label(details->str);
	set_attrs(w, PANGO_SCALE_SMALL, FALSE);
	gtk_style_context_add_class(gtk_widget_get_style_context(w),
				    "dim-label");
	gtk_widget_set_margin_top(w, 4);
	gtk_box_pack_start(GTK_BOX(col), w, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(row), col, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), row, TRUE, TRUE, 0);

	gtk_widget_show_all(dlg);
	gtk_widget_grab_default(replay);
	gtk_widget_grab_focus(replay);
	gtk_window_present(GTK_WINDOW(dlg));

	/* Escape and the window's close button answer "don't". */
	response = gtk_dialog_run(GTK_DIALOG(dlg));
	gtk_widget_destroy(dlg);

	g_string_free(details, TRUE);
	g_free(body1);
	g_free(heading);
	g_free(name);
	g_free(sz);
	g_free(drive);
	g_free(label);
	return response == GTK_RESPONSE_ACCEPT ? EXIT_REPLAY : EXIT_DECLINE;
}
