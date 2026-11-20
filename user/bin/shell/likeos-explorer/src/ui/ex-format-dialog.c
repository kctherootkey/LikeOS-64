// LikeOS Explorer: formatting a USB drive.
//
// Copyright (C) 2026 The LikeOS Project

#include "ui/ex-format-dialog.h"
#include "ui/ex-icons.h"
#include "ui/ex-style.h"
#include "backend/ex-devd.h"
#include "backend/ex-util.h"
#include "backend/ex-volumes.h"

#include <string.h>

enum { R_START = 1 };

typedef struct {
	ExWindow *win;
	GtkWidget *dialog;
	char *device; /* "sdb1" */
	char *name; /* "USB Drive", when the window opened */
	GtkWidget *capacity, *fs, *cluster, *label, *restore, *quick;
	GtkWidget *bar, *status, *start, *close;
	GtkWidget *label_note; /* why the label does not suit, under it */
	ExDevdFormat *job; /* while formatting */
	gboolean filling; /* the combos are being refilled */
	gboolean present; /* the drive is there */
} Fmt;

/* The open windows, by device. */
static GHashTable *g_open;

static void fmt_free(Fmt *f)
{
	if (f->job) {
		/* The window went with its parent while formatting: the job
		 * finishes on its own, unwatched. */
		g_signal_handlers_disconnect_by_data(f->job, f);
		g_object_unref(f->job);
	}
	g_signal_handlers_disconnect_by_data(ex_volumes_get_default(), f);
	g_signal_handlers_disconnect_by_data(ex_devd_get_default(), f);
	if (g_open && g_hash_table_lookup(g_open, f->device) == f->dialog)
		g_hash_table_remove(g_open, f->device);
	g_object_unref(f->win);
	g_free(f->device);
	g_free(f->name);
	g_free(f);
}

/* ---- pieces ------------------------------------------------------------------ */

static void row(GtkWidget *grid, int *r, const char *mnemonic, GtkWidget *value)
{
	GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_mnemonic_widget(GTK_LABEL(l), value);
	ex_style_add(l, EX_CLASS_DIM);
	ex_style_add(l, "ex-props-key");
	gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(value, TRUE);
	gtk_grid_attach(GTK_GRID(grid), l, 0, *r, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), value, 1, *r, 1, 1);
	(*r)++;
}

static GtkWidget *card(GtkWidget *box)
{
	GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *grid = gtk_grid_new();

	gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
	ex_style_add(c, "ex-props-card");
	gtk_box_pack_start(GTK_BOX(c), grid, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
	return grid;
}

/* "4096 bytes", "16 KB", "1 MB": an allocation unit. */
static char *unit_text(guint64 n)
{
	if (n >= 1024 * 1024 && n % (1024 * 1024) == 0)
		return g_strdup_printf("%" G_GUINT64_FORMAT " MB", n / (1024 * 1024));
	if (n > 8192 && n % 1024 == 0)
		return g_strdup_printf("%" G_GUINT64_FORMAT " KB", n / 1024);
	return g_strdup_printf("%" G_GUINT64_FORMAT " bytes", n);
}

/* "16,384". */
static char *grouped(guint64 n)
{
	char digits[32];
	GString *s = g_string_new(NULL);
	int len = g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, n);

	for (int i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0)
			g_string_append_c(s, ',');
		g_string_append_c(s, digits[i]);
	}
	return g_string_free(s, FALSE);
}

static const ExDevdFs *chosen_fs(Fmt *f)
{
	const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->fs));

	return id ? ex_devd_find_fs(ex_devd_get_default(), id) : NULL;
}

/* The label as long as the file system allows, cut at a character. */
static void clip_label(Fmt *f, guint max)
{
	const char *t = gtk_entry_get_text(GTK_ENTRY(f->label));

	if (strlen(t) > max) {
		const char *end = t + max;
		char *cut;

		while (end > t && (((guchar)*end) & 0xc0) == 0x80)
			end--;
		cut = g_strndup(t, (gsize)(end - t));
		gtk_entry_set_text(GTK_ENTRY(f->label), cut);
		g_free(cut);
	}
}

/* Typing stops where the file system's label is full (counted in bytes,
 * as the file system stores it). */
static void label_insert(GtkEditable *e, const char *text, int len, int *pos,
			 Fmt *f)
{
	const ExDevdFs *fs = chosen_fs(f);
	gsize have = strlen(gtk_entry_get_text(GTK_ENTRY(e)));
	gsize add = len < 0 ? strlen(text) : (gsize)len;
	gsize max = fs ? fs->labelmax : 0;
	gsize room = have >= max ? 0 : max - have;
	const char *end;

	if (add <= room)
		return;
	g_signal_stop_emission_by_name(e, "insert-text");
	end = text + room;
	while (end > text && (((guchar)*end) & 0xc0) == 0x80)
		end--;
	if (end > text) {
		g_signal_handlers_block_by_func(e, label_insert, f);
		gtk_editable_insert_text(e, text, (int)(end - text), pos);
		g_signal_handlers_unblock_by_func(e, label_insert, f);
	}
	gtk_widget_error_bell(GTK_WIDGET(e));
}

/* ---- the choices ---------------------------------------------------------------- */

static void fill_clusters(Fmt *f)
{
	const ExDevdFs *fs = chosen_fs(f);
	char *dflt;

	f->filling = TRUE;
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(f->cluster));
	if (fs && fs->defcluster) {
		char *u = grouped(fs->defcluster);

		dflt = g_strdup_printf("Default (%s bytes)", u);
		g_free(u);
	} else {
		dflt = g_strdup("Default");
	}
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(f->cluster), "0", dflt);
	g_free(dflt);
	for (guint i = 0; fs && i < fs->clusters->len; i++) {
		guint64 n = g_array_index(fs->clusters, guint64, i);
		char *id = g_strdup_printf("%" G_GUINT64_FORMAT, n);
		char *t = unit_text(n);

		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(f->cluster), id, t);
		g_free(t);
		g_free(id);
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(f->cluster), 0);
	f->filling = FALSE;
	if (fs) {
		char *tip = fs->labelmax ?
				    g_strdup_printf("Up to %u characters", fs->labelmax) :
				    g_strdup("This file system has no label");

		clip_label(f, fs->labelmax);
		gtk_widget_set_tooltip_text(f->label, tip);
		g_free(tip);
	}
}

/* NULL when `label' suits the file system, else a sentence saying why
 * not -- the rules the device service applies, checked before Start. */
static const char *label_problem(const ExDevdFs *fs, const char *label)
{
	if (!fs || !*label)
		return NULL;
	if (strlen(label) > fs->labelmax)
		return "The label is too long for this file system.";
	if (strcmp(fs->id, "vfat") == 0) {
		for (const guchar *c = (const guchar *)label; *c; c++) {
			if (*c < 0x20 || *c > 0x7e)
				return "A FAT32 label can hold only letters A to Z, "
				       "digits, spaces and simple punctuation.";
			if (strchr("\"*+,./:;<=>?[\\]|", *c))
				return "A FAT32 label can't contain any of "
				       "\" * + , . / : ; < = > ? [ \\ ] |";
		}
		return NULL;
	}
	for (const guchar *c = (const guchar *)label; *c; c++)
		if (*c < 0x20 || *c == 0x7f || *c == '/')
			return "A label can't contain “/” or control characters.";
	return NULL;
}

static void sync_sensitive(Fmt *f)
{
	gboolean idle = f->job == NULL;
	const ExDevdFs *fs = chosen_fs(f);
	const char *problem = label_problem(
		fs, gtk_entry_get_text(GTK_ENTRY(f->label)));
	gboolean can = idle && f->present && fs != NULL && !problem;

	gtk_label_set_text(GTK_LABEL(f->label_note), problem ? problem : "");
	gtk_widget_set_visible(f->label_note, problem != NULL);
	gtk_widget_set_sensitive(f->fs, idle);
	gtk_widget_set_sensitive(f->cluster, idle);
	gtk_widget_set_sensitive(f->label, idle && fs && fs->labelmax > 0);
	gtk_widget_set_sensitive(f->restore, idle);
	gtk_widget_set_sensitive(f->quick, idle);
	gtk_widget_set_sensitive(f->start, can);
	gtk_widget_set_sensitive(f->close, idle);
	gtk_window_set_deletable(GTK_WINDOW(f->dialog), idle);
}

static void fs_changed(GtkComboBox *c, Fmt *f)
{
	if (f->filling)
		return;
	fill_clusters(f);
	sync_sensitive(f);
}

/* The file systems the service formats, its default one first and
 * chosen. */
static void fill_fstypes(Fmt *f)
{
	ExDevd *devd = ex_devd_get_default();
	GPtrArray *all = ex_devd_fstypes(devd);
	char *keep = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->fs)));
	const char *first = NULL;

	f->filling = TRUE;
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(f->fs));
	for (guint i = 0; i < all->len; i++) {
		const ExDevdFs *fs = g_ptr_array_index(all, i);

		if (!fs->can_format)
			continue;
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(f->fs), fs->id,
					  fs->name);
		/* The list comes with the default first. */
		if (!first)
			first = fs->id;
	}
	f->filling = FALSE;
	if (!keep || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(f->fs), keep)) {
		if (first)
			gtk_combo_box_set_active_id(GTK_COMBO_BOX(f->fs), first);
		else
			fill_clusters(f);
	}
	g_free(keep);
	if (!first && !f->job)
		gtk_label_set_text(GTK_LABEL(f->status),
				   ex_devd_is_connected(devd) ?
					   "The device service offers no file "
					   "system to format with." :
					   ex_devd_error_explain(
						   EX_DEVD_ERROR_NOT_RUNNING));
	else if (!f->job && f->present)
		gtk_label_set_text(GTK_LABEL(f->status), "");
	sync_sensitive(f);
}

static void restore_defaults(GtkButton *b, Fmt *f)
{
	GPtrArray *all = ex_devd_fstypes(ex_devd_get_default());

	for (guint i = 0; i < all->len; i++) {
		const ExDevdFs *fs = g_ptr_array_index(all, i);

		if (fs->is_default && fs->can_format) {
			gtk_combo_box_set_active_id(GTK_COMBO_BOX(f->fs), fs->id);
			break;
		}
	}
	gtk_combo_box_set_active(GTK_COMBO_BOX(f->cluster), 0);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f->quick), TRUE);
}

/* ---- the drive coming and going --------------------------------------------- */

static void volumes_changed(ExVolumes *v, Fmt *f)
{
	const ExVolume *vol = ex_volumes_find_device(v, f->device);

	if (f->job)
		return; /* the drive comes and goes while it is formatted */
	f->present = vol != NULL && strcmp(vol->state, "formatting") != 0;
	if (!vol)
		gtk_label_set_text(GTK_LABEL(f->status),
				   "The drive is no longer connected.");
	else if (!f->present)
		gtk_label_set_text(GTK_LABEL(f->status),
				   "The drive is being formatted.");
	else if (strcmp(gtk_label_get_text(GTK_LABEL(f->status)),
			"The drive is no longer connected.") == 0 ||
		 strcmp(gtk_label_get_text(GTK_LABEL(f->status)),
			"The drive is being formatted.") == 0)
		gtk_label_set_text(GTK_LABEL(f->status), "");
	sync_sensitive(f);
}

static void fstypes_changed(ExDevd *d, Fmt *f)
{
	if (!f->job)
		fill_fstypes(f);
}

/* ---- formatting --------------------------------------------------------------------- */

static void error_box(Fmt *f, const char *primary, const char *secondary)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(f->dialog),
					      GTK_DIALOG_DESTROY_WITH_PARENT |
						      GTK_DIALOG_MODAL,
					      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
					      "%s", primary);

	gtk_window_set_title(GTK_WINDOW(d), "Format");
	if (secondary)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", secondary);
	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	ex_style_fixed_dialog(d);
	gtk_widget_show(d);
}

static void job_progress(ExDevdFormat *job, int pct, Fmt *f)
{
	char *s = g_strdup_printf("Formatting… %d %%", pct);

	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar), pct / 100.0);
	gtk_label_set_text(GTK_LABEL(f->status), s);
	g_free(s);
}

static void mounted_after(GObject *src, GAsyncResult *res, gpointer data)
{
	/* Not mounted (the drive went, or its new file system cannot be):
	 * it is shown as it is, nothing to report. */
	g_free(ex_devd_mount_finish(EX_DEVD(src), res, NULL));
}

static void label_changed(GtkEditable *e, Fmt *f)
{
	sync_sensitive(f);
}

static void job_finished(ExDevdFormat *job, GError *e, Fmt *f)
{
	g_signal_handlers_disconnect_by_data(job, f);
	g_clear_object(&f->job);
	if (e) {
		char *what = g_strdup_printf("%s could not be formatted", f->name);

		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar), 0);
		gtk_label_set_text(GTK_LABEL(f->status), "Formatting did not finish.");
		error_box(f, what, e->message);
		g_free(what);
	} else {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar), 1.0);
		gtk_label_set_text(GTK_LABEL(f->status), "Formatting is complete.");
		/* The service leaves a formatted drive unmounted: mounted
		 * now, the new empty drive is there to open. */
		ex_devd_mount_async(ex_devd_get_default(), f->device, NULL,
				    mounted_after, NULL);
	}
	volumes_changed(ex_volumes_get_default(), f);
}

static void start_format(Fmt *f)
{
	const ExDevdFs *fs = chosen_fs(f);
	const char *cl = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->cluster));
	ExVolumes *vols = ex_volumes_get_default();
	const ExVolume *vol = ex_volumes_find_device(vols, f->device);
	guint64 cluster = 0;

	if (!fs || !vol)
		return;
	if (cl)
		g_ascii_string_to_unsigned(cl, 10, 0, G_MAXUINT64, &cluster, NULL);
	/* Tabs showing the drive leave it: the service unmounts it first. */
	ex_volumes_release(vols, vol);
	f->job = ex_devd_format_start(
		f->device, fs->id, gtk_entry_get_text(GTK_ENTRY(f->label)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->quick)), cluster);
	g_signal_connect(f->job, "progress", G_CALLBACK(job_progress), f);
	g_signal_connect(f->job, "finished", G_CALLBACK(job_finished), f);
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(f->bar), 0);
	gtk_label_set_text(GTK_LABEL(f->status), "Formatting…");
	sync_sensitive(f);
}

static void confirm_response(GtkDialog *d, int r, Fmt *f)
{
	gtk_widget_destroy(GTK_WIDGET(d));
	if (r == GTK_RESPONSE_ACCEPT && !f->job)
		start_format(f);
}

/* Start: formatting erases everything, so it asks first. */
static void ask_first(Fmt *f)
{
	GtkWidget *d = gtk_message_dialog_new(
		GTK_WINDOW(f->dialog),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
		"Formatting will erase ALL data on this drive.");

	gtk_message_dialog_format_secondary_text(
		GTK_MESSAGE_DIALOG(d),
		"Every file and folder on %s (%s) will be gone for good. Format "
		"it anyway?",
		f->name, f->device);
	gtk_window_set_title(GTK_WINDOW(d), "Format");
	gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL,
			       "_Format", GTK_RESPONSE_ACCEPT, NULL);
	/* Erasing is never what a stray Enter does. */
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
	g_signal_connect(d, "response", G_CALLBACK(confirm_response), f);
	ex_style_fixed_dialog(d);
	gtk_widget_show(d);
}

static void response(GtkDialog *d, int r, Fmt *f)
{
	if (r == R_START) {
		if (!f->job)
			ask_first(f);
		return;
	}
	/* Close, Escape, the window's close button: not while formatting. */
	if (!f->job)
		gtk_widget_destroy(GTK_WIDGET(d));
}

static gboolean delete_event(GtkWidget *w, GdkEvent *ev, Fmt *f)
{
	return f->job != NULL;
}

/* ---- the window ----------------------------------------------------------------- */

static GtkWidget *header(Fmt *f, const ExVolume *v)
{
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(f->win));
	GdkPixbuf *pb = ex_icons_named(v->icon_name, 48, scale);
	cairo_surface_t *s = pb ? gdk_cairo_surface_create_from_pixbuf(pb, scale,
								       NULL) :
				  NULL;
	GtkWidget *img = gtk_image_new_from_surface(s);
	char *title = g_strdup_printf("Format %s (%s)", v->name, v->device);
	char *cap = ex_format_size(v->capacity);
	char *fs = ex_devd_volume_fs_name(ex_devd_get_default(), v->fs_type,
					  v->supported);
	char *sub = g_strdup_printf("%s USB drive  ·  %s", cap, fs);
	GtkWidget *t = gtk_label_new(title);
	GtkWidget *k = gtk_label_new(sub);

	if (s)
		cairo_surface_destroy(s);
	g_clear_object(&pb);
	ex_style_add(h, "ex-props-header");
	gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(t), 36);
	ex_style_add(t, "ex-props-title");
	gtk_label_set_xalign(GTK_LABEL(k), 0);
	ex_style_add(k, EX_CLASS_DIM);
	gtk_box_pack_start(GTK_BOX(col), t, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(col), k, FALSE, FALSE, 0);
	gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(h), col, TRUE, TRUE, 0);
	g_free(sub);
	g_free(fs);
	g_free(cap);
	g_free(title);
	return h;
}

void ex_format_dialog_show(ExWindow *w, const char *device)
{
	const ExVolume *v = ex_volumes_find_device(ex_volumes_get_default(),
						   device);
	GtkWidget *open, *box, *page, *grid, *opts, *h, *cbox;
	Fmt *f;
	char *title, *cap;
	int r = 0;

	if (!v)
		return;
	if (!g_open)
		g_open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					       NULL);
	open = g_hash_table_lookup(g_open, device);
	if (open) {
		gtk_window_present(GTK_WINDOW(open));
		return;
	}
	f = g_new0(Fmt, 1);
	f->win = g_object_ref(w);
	f->device = g_strdup(device);
	f->name = g_strdup(v->name);
	f->present = TRUE;
	title = g_strdup_printf("Format %s (%s)", v->name, v->device);
	f->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(w),
						GTK_DIALOG_DESTROY_WITH_PARENT,
						NULL, NULL);
	g_free(title);
	f->close = gtk_dialog_add_button(GTK_DIALOG(f->dialog), "_Close",
					 GTK_RESPONSE_CLOSE);
	f->start = gtk_dialog_add_button(GTK_DIALOG(f->dialog), "_Start", R_START);
	ex_style_add(f->start, "suggested-action");
	gtk_box_set_spacing(GTK_BOX(gtk_dialog_get_content_area(
				    GTK_DIALOG(f->dialog))),
			    0);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(box), header(f, v), FALSE, FALSE, 0);
	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	ex_style_add(page, "ex-props-page");
	gtk_box_pack_start(GTK_BOX(box), page, TRUE, TRUE, 0);

	grid = card(page);
	f->capacity = gtk_combo_box_text_new();
	cap = ex_format_size(v->capacity);
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(f->capacity), "0", cap);
	gtk_combo_box_set_active(GTK_COMBO_BOX(f->capacity), 0);
	g_free(cap);
	row(grid, &r, "Ca_pacity", f->capacity);
	f->fs = gtk_combo_box_text_new();
	row(grid, &r, "File _system", f->fs);
	f->cluster = gtk_combo_box_text_new();
	row(grid, &r, "_Allocation unit size", f->cluster);
	f->label = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(f->label), v->label ? v->label : "");
	gtk_entry_set_activates_default(GTK_ENTRY(f->label), FALSE);
	row(grid, &r, "Volume _label", f->label);
	f->label_note = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(f->label_note), 0);
	gtk_label_set_line_wrap(GTK_LABEL(f->label_note), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(f->label_note), 30);
	gtk_widget_set_no_show_all(f->label_note, TRUE);
	ex_style_add(f->label_note, "ex-format-label-note");
	gtk_grid_attach(GTK_GRID(grid), f->label_note, 1, r++, 1, 1);
	f->restore = gtk_button_new_with_mnemonic("_Restore device defaults");
	gtk_widget_set_halign(f->restore, GTK_ALIGN_START);
	ex_style_add(f->restore, "ex-format-restore");
	gtk_grid_attach(GTK_GRID(grid), f->restore, 1, r++, 1, 1);

	h = gtk_label_new("Format options");
	gtk_label_set_xalign(GTK_LABEL(h), 0);
	ex_style_add(h, "ex-settings-heading");
	gtk_box_pack_start(GTK_BOX(page), h, FALSE, FALSE, 0);
	opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	ex_style_add(opts, "ex-props-card");
	ex_style_add(opts, "ex-settings-card");
	cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	ex_style_add(cbox, "ex-settings-row");
	f->quick = gtk_check_button_new_with_mnemonic("_Quick format");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f->quick), TRUE);
	gtk_widget_set_tooltip_text(f->quick,
				    "Off: every byte of the drive is overwritten "
				    "first, which takes much longer");
	gtk_box_pack_start(GTK_BOX(cbox), f->quick, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(opts), cbox, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(page), opts, FALSE, FALSE, 0);

	f->bar = gtk_progress_bar_new();
	ex_style_add(f->bar, "ex-format-progress");
	gtk_box_pack_start(GTK_BOX(page), f->bar, FALSE, FALSE, 0);
	f->status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(f->status), 0);
	gtk_label_set_line_wrap(GTK_LABEL(f->status), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(f->status), 48);
	ex_style_add(f->status, EX_CLASS_DIM);
	ex_style_add(f->status, "ex-format-status");
	gtk_box_pack_start(GTK_BOX(page), f->status, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(
				  GTK_DIALOG(f->dialog))),
			  box);
	g_signal_connect(f->fs, "changed", G_CALLBACK(fs_changed), f);
	g_signal_connect(f->label, "insert-text", G_CALLBACK(label_insert), f);
	g_signal_connect(f->label, "changed", G_CALLBACK(label_changed), f);
	g_signal_connect(f->restore, "clicked", G_CALLBACK(restore_defaults), f);
	g_signal_connect(f->dialog, "response", G_CALLBACK(response), f);
	g_signal_connect(f->dialog, "delete-event", G_CALLBACK(delete_event), f);
	g_signal_connect(ex_volumes_get_default(), "changed",
			 G_CALLBACK(volumes_changed), f);
	g_signal_connect(ex_devd_get_default(), "fstypes-changed",
			 G_CALLBACK(fstypes_changed), f);
	g_object_set_data_full(G_OBJECT(f->dialog), "ex-format", f,
			       (GDestroyNotify)fmt_free);
	g_hash_table_insert(g_open, g_strdup(device), f->dialog);

	if (ex_devd_fstypes(ex_devd_get_default())->len == 0)
		ex_devd_refresh_fstypes(ex_devd_get_default());
	fill_fstypes(f);
	volumes_changed(ex_volumes_get_default(), f);
	ex_style_add(f->dialog, "ex-properties");
	ex_style_add(f->dialog, "ex-format");
	ex_style_fixed_dialog(f->dialog);
	gtk_widget_show_all(f->dialog);
	gtk_widget_grab_focus(f->fs);
}
