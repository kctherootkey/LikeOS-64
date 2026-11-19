// LikeOS -- LikeOS Image Viewer: the dialogs.
//
// Save as (the file chooser with the format, its quality and whether the
// camera data is kept), Resize (presets by the long edge, a size of one's
// own with the proportions locked or not, or a percentage; the format, the
// quality and an estimate of the file's size; a copy next to the original
// or the original replaced), Settings, the keyboard shortcuts and About.
// They are ordinary windows of their own in the light LikeOS look, like
// every other dialog on the system; only the cards inside the viewer
// (toasts, confirmations) are dark.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "canvas.h"

static const IvFormat writable[] = { IV_FORMAT_PNG, IV_FORMAT_JPEG,
				     IV_FORMAT_WEBP, IV_FORMAT_TIFF,
				     IV_FORMAT_BMP };

static GtkWidget *format_combo(IvFormat active)
{
	GtkWidget *c = gtk_combo_box_text_new();

	for (gsize i = 0; i < G_N_ELEMENTS(writable); i++) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c),
					  iv_format_extension(writable[i]),
					  iv_format_label(writable[i]));
		if (writable[i] == active)
			gtk_combo_box_set_active(GTK_COMBO_BOX(c), (int)i);
	}
	if (gtk_combo_box_get_active(GTK_COMBO_BOX(c)) < 0)
		gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
	return c;
}

static IvFormat combo_format(GtkWidget *c)
{
	int i = gtk_combo_box_get_active(GTK_COMBO_BOX(c));

	return i >= 0 && i < (int)G_N_ELEMENTS(writable) ? writable[i] :
							     IV_FORMAT_PNG;
}

static GtkWidget *row_label(const char *text)
{
	GtkWidget *l = gtk_label_new_with_mnemonic(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	return l;
}

/* ---- Save as ------------------------------------------------------------------- */

typedef struct {
	GtkWidget *dialog, *format, *quality, *quality_label, *quality_value;
	GtkWidget *lossless, *exif;
	IvSaveChosen done;
	gpointer data;
} SaveAs;

static void save_as_sync(SaveAs *s, gboolean rename)
{
	IvFormat f = combo_format(s->format);
	gboolean lossy = f == IV_FORMAT_JPEG ||
			 (f == IV_FORMAT_WEBP &&
			  !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->lossless)));

	gtk_widget_set_visible(s->quality, lossy);
	gtk_widget_set_visible(s->quality_label, lossy);
	gtk_widget_set_visible(s->quality_value, lossy);
	gtk_widget_set_visible(s->lossless, f == IV_FORMAT_WEBP);
	gtk_widget_set_visible(s->exif, f == IV_FORMAT_JPEG);
	if (rename) {
		char *name = gtk_file_chooser_get_current_name(
			GTK_FILE_CHOOSER(s->dialog));

		if (name) {
			char *renamed = iv_replace_extension(name,
							     iv_format_extension(f));

			gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(s->dialog),
							  renamed);
			g_free(renamed);
			g_free(name);
		}
	}
}

static void save_as_format(GtkComboBox *c, SaveAs *s)
{
	save_as_sync(s, TRUE);
}

static void save_as_lossless(GtkToggleButton *b, SaveAs *s)
{
	save_as_sync(s, FALSE);
}

static void quality_shown(GtkRange *r, GtkWidget *label)
{
	char buf[8];

	g_snprintf(buf, sizeof buf, "%d", (int)gtk_range_get_value(r));
	gtk_label_set_text(GTK_LABEL(label), buf);
}

static void save_as_response(GtkDialog *d, int r, SaveAs *s)
{
	IvSaveOptions o;
	char *path = NULL;

	if (r == GTK_RESPONSE_ACCEPT)
		path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
	if (!path) {
		gtk_widget_destroy(GTK_WIDGET(d));
		if (s->done)
			s->done(NULL, NULL, s->data);
		g_free(s);
		return;
	}
	/* A name typed with a known extension says the format; the menu
	 * otherwise. */
	IvFormat f = iv_format_for_path(path);

	if (f == IV_FORMAT_NONE) {
		char *with;

		f = combo_format(s->format);
		with = g_strconcat(path, ".", iv_format_extension(f), NULL);
		g_free(path);
		path = with;
	}
	iv_save_options_init(&o, f);
	o.quality = (int)gtk_range_get_value(GTK_RANGE(s->quality));
	if (f == IV_FORMAT_WEBP &&
	    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->lossless)))
		o.quality = 101;
	o.keep_exif = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->exif));
	gtk_widget_destroy(GTK_WIDGET(d));
	if (s->done)
		s->done(path, &o, s->data);
	iv_save_options_clear(&o);
	g_free(path);
	g_free(s);
}

void iv_dialog_save_as(GtkWindow *parent, const char *suggest_path,
		       IvFormat suggest, gboolean has_alpha, IvSaveChosen done,
		       gpointer data)
{
	SaveAs *s = g_new0(SaveAs, 1);
	IvSettings *st = iv_settings_get();
	GtkWidget *grid = gtk_grid_new();
	char *dir = suggest_path ? g_path_get_dirname(suggest_path) : NULL;
	char *base = suggest_path ? g_path_get_basename(suggest_path) : NULL;

	s->done = done;
	s->data = data;
	s->dialog = gtk_file_chooser_dialog_new("Save a copy", parent,
						GTK_FILE_CHOOSER_ACTION_SAVE,
						"_Cancel", GTK_RESPONSE_CANCEL,
						"_Save", GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(s->dialog), GTK_RESPONSE_ACCEPT);
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(s->dialog),
						       TRUE);
	if (dir)
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(s->dialog), dir);
	if (base)
		gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(s->dialog), base);

	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	s->format = format_combo(suggest);
	gtk_grid_attach(GTK_GRID(grid), row_label("_Format"), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), s->format, 1, 0, 1, 1);
	s->quality_label = row_label("_Quality");
	s->quality = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10, 100,
					      1);
	gtk_scale_set_draw_value(GTK_SCALE(s->quality), FALSE);
	gtk_range_set_value(GTK_RANGE(s->quality), st->jpeg_quality);
	gtk_widget_set_size_request(s->quality, 200, -1);
	s->quality_value = gtk_label_new(NULL);
	g_signal_connect(s->quality, "value-changed", G_CALLBACK(quality_shown),
			 s->quality_value);
	quality_shown(GTK_RANGE(s->quality), s->quality_value);
	gtk_grid_attach(GTK_GRID(grid), s->quality_label, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), s->quality, 1, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), s->quality_value, 2, 1, 1, 1);
	s->lossless = gtk_check_button_new_with_mnemonic("_Lossless (larger file)");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->lossless), has_alpha);
	gtk_grid_attach(GTK_GRID(grid), s->lossless, 1, 2, 2, 1);
	s->exif = gtk_check_button_new_with_mnemonic(
		"_Keep the camera data (date, camera, position)");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->exif), st->keep_exif);
	gtk_grid_attach(GTK_GRID(grid), s->exif, 1, 3, 2, 1);
	gtk_widget_show_all(grid);
	gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(s->dialog), grid);
	g_signal_connect(s->format, "changed", G_CALLBACK(save_as_format), s);
	g_signal_connect(s->lossless, "toggled", G_CALLBACK(save_as_lossless), s);
	g_signal_connect(s->dialog, "response", G_CALLBACK(save_as_response), s);
	save_as_sync(s, FALSE);
	gtk_window_set_modal(GTK_WINDOW(s->dialog), TRUE);
	gtk_widget_show(s->dialog);
	g_free(dir);
	g_free(base);
}

/* ---- Resize -------------------------------------------------------------------- */

enum { R_SMALL, R_MEDIUM, R_LARGE, R_CUSTOM, R_PERCENT, R_COUNT };

static const int preset_edge[] = { 640, 1280, 1920 };

typedef struct {
	IvWindow *w;
	IvPicture *pic;
	GtkWidget *dialog;
	GtkWidget *radio[R_COUNT];
	GtkWidget *width, *height, *lock, *percent;
	GtkWidget *format, *quality, *quality_label, *quality_value;
	GtkWidget *estimate, *copy, *replace;
	guint est_timer;
	gboolean syncing;
} Resize;

static void resize_free(Resize *r)
{
	if (r->est_timer)
		g_source_remove(r->est_timer);
	iv_picture_unref(r->pic);
	g_free(r);
}

static int chosen(Resize *r)
{
	for (int i = 0; i < R_COUNT; i++)
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r->radio[i])))
			return i;
	return R_MEDIUM;
}

static void target_size(Resize *r, int *tw, int *th)
{
	int w = r->pic->width, h = r->pic->height;
	int c = chosen(r);
	double k = 1;

	switch (c) {
	case R_SMALL:
	case R_MEDIUM:
	case R_LARGE:
		k = MIN(1.0, (double)preset_edge[c] / MAX(w, h));
		*tw = MAX(1, (int)lround(w * k));
		*th = MAX(1, (int)lround(h * k));
		return;
	case R_PERCENT:
		k = gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->percent)) / 100.0;
		*tw = MAX(1, (int)lround(w * k));
		*th = MAX(1, (int)lround(h * k));
		return;
	default:
		*tw = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->width));
		*th = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(r->height));
		return;
	}
}

static void options_of(Resize *r, IvSaveOptions *o)
{
	iv_save_options_init(o, combo_format(r->format));
	o->quality = (int)gtk_range_get_value(GTK_RANGE(r->quality));
	o->exif_from = g_strdup(r->pic->path);
}

static gboolean estimate_now(gpointer data)
{
	Resize *r = data;
	IvSaveOptions o;
	int tw, th;
	goffset est;
	char *size, *text;

	r->est_timer = 0;
	target_size(r, &tw, &th);
	options_of(r, &o);
	est = iv_save_estimate(r->pic->pixbuf, tw, th, &o);
	iv_save_options_clear(&o);
	size = iv_format_size(est);
	text = est > 0 ? g_strdup_printf("%d × %d pixels, about %s", tw, th, size) :
			 g_strdup_printf("%d × %d pixels", tw, th);
	gtk_label_set_text(GTK_LABEL(r->estimate), text);
	g_free(text);
	g_free(size);
	return G_SOURCE_REMOVE;
}

static void resize_sync(Resize *r)
{
	IvFormat f;
	gboolean lossy;
	int c;

	/* The radios answer while the dialog is still being built. */
	if (!r->format || !r->estimate)
		return;
	f = combo_format(r->format);
	lossy = f == IV_FORMAT_JPEG || f == IV_FORMAT_WEBP;
	c = chosen(r);

	gtk_widget_set_sensitive(r->width, c == R_CUSTOM);
	gtk_widget_set_sensitive(r->height, c == R_CUSTOM);
	gtk_widget_set_sensitive(r->lock, c == R_CUSTOM);
	gtk_widget_set_sensitive(r->percent, c == R_PERCENT);
	gtk_widget_set_visible(r->quality, lossy);
	gtk_widget_set_visible(r->quality_label, lossy);
	gtk_widget_set_visible(r->quality_value, lossy);
	gtk_label_set_text(GTK_LABEL(r->estimate), "Estimating the size…");
	if (r->est_timer)
		g_source_remove(r->est_timer);
	r->est_timer = g_timeout_add(250, estimate_now, r);
}

static void radio_toggled(GtkToggleButton *b, Resize *r)
{
	if (gtk_toggle_button_get_active(b))
		resize_sync(r);
}

/* With the proportions locked, either side follows the other. */
static void side_changed(GtkSpinButton *sb, Resize *r)
{
	double aspect = (double)r->pic->width / r->pic->height;

	if (r->syncing)
		return;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r->lock))) {
		r->syncing = TRUE;
		if (GTK_WIDGET(sb) == r->width)
			gtk_spin_button_set_value(
				GTK_SPIN_BUTTON(r->height),
				MAX(1, lround(gtk_spin_button_get_value(sb) / aspect)));
		else
			gtk_spin_button_set_value(
				GTK_SPIN_BUTTON(r->width),
				MAX(1, lround(gtk_spin_button_get_value(sb) * aspect)));
		r->syncing = FALSE;
	}
	resize_sync(r);
}

static void lock_toggled(GtkToggleButton *b, Resize *r)
{
	GtkWidget *g = gtk_bin_get_child(GTK_BIN(b));

	iv_glyph_set(g, gtk_toggle_button_get_active(b) ? IV_GLYPH_LOCK :
							  IV_GLYPH_UNLOCK);
	if (gtk_toggle_button_get_active(b))
		side_changed(GTK_SPIN_BUTTON(r->width), r);
}

static void anything_changed(GtkWidget *wid, Resize *r)
{
	resize_sync(r);
}

typedef struct {
	IvWindow *w;
	GdkPixbuf *src;
	int tw, th;
	char *path;
	IvSaveOptions o;
	gboolean replace;
	GError *error;
} ResizeJob;

static void resize_job_free(gpointer p)
{
	ResizeJob *j = p;

	g_object_unref(j->src);
	g_free(j->path);
	iv_save_options_clear(&j->o);
	g_clear_error(&j->error);
	g_free(j);
}

static void resize_thread(GTask *t, gpointer src, gpointer data,
			  GCancellable *c)
{
	ResizeJob *j = data;
	IvImage *img = iv_image_from_pixbuf(j->src);
	IvImage *scaled = img ? iv_image_scale(img, j->tw, j->th, 2) : NULL;
	GdkPixbuf *pb = scaled ? iv_pixbuf_from_image(scaled) : NULL;
	gboolean ok = FALSE;

	iv_image_free(img);
	iv_image_free(scaled);
	if (!pb)
		g_set_error_literal(&j->error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
				    "Not enough memory");
	else
		ok = iv_save_pixbuf(pb, j->path, &j->o, &j->error);
	g_clear_object(&pb);
	g_task_return_boolean(t, ok);
}

static void resize_done(GObject *o, GAsyncResult *res, gpointer data)
{
	ResizeJob *j = g_task_get_task_data(G_TASK(res));
	IvWindow *w = j->w;
	char *base, *msg;

	iv_toast_hide(w->toast);
	if (!g_task_propagate_boolean(G_TASK(res), NULL)) {
		iv_window_error(w, "The resized picture could not be saved",
				j->error);
		return;
	}
	base = g_filename_display_basename(j->path);
	iv_viewer_mark_self_written(w, j->path);
	if (j->replace) {
		msg = g_strdup_printf("“%s” was resized", base);
		if (w->path && !strcmp(w->path, j->path))
			iv_viewer_reload(w);
	} else {
		msg = g_strdup_printf("Saved a resized copy as “%s”", base);
		if (w->folder) {
			iv_folder_add(w->folder, j->path);
			iv_filmstrip_reload(w->film);
			iv_window_set_title_text(w);
		}
	}
	iv_window_toast(w, msg);
	g_free(msg);
	g_free(base);
}

static void start_resize(Resize *r, gboolean replace)
{
	ResizeJob *j = g_new0(ResizeJob, 1);
	GTask *t;
	IvFormat f = combo_format(r->format);

	j->w = r->w;
	j->src = g_object_ref(r->pic->pixbuf);
	target_size(r, &j->tw, &j->th);
	options_of(r, &j->o);
	j->replace = replace;
	if (replace)
		j->path = iv_format_for_path(r->pic->path) == f ?
				  g_strdup(r->pic->path) :
				  iv_replace_extension(r->pic->path,
						       iv_format_extension(f));
	else
		j->path = iv_unique_sibling(r->pic->path, " (resized)",
					    iv_format_extension(f));
	iv_toast_progress(r->w->toast, "Resizing", -1);
	t = g_task_new(NULL, NULL, resize_done, NULL);
	g_task_set_task_data(t, j, resize_job_free);
	g_task_run_in_thread(t, resize_thread);
	g_object_unref(t);
}

static void resize_response(GtkDialog *d, int response, Resize *r)
{
	if (response == GTK_RESPONSE_OK) {
		gboolean replace = gtk_toggle_button_get_active(
			GTK_TOGGLE_BUTTON(r->replace));

		if (replace) {
			GtkWidget *q = gtk_message_dialog_new(
				GTK_WINDOW(d), GTK_DIALOG_MODAL,
				GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
				"Replace the original picture?");

			gtk_message_dialog_format_secondary_text(
				GTK_MESSAGE_DIALOG(q),
				"The original is overwritten with the smaller "
				"picture; its full size cannot be brought back.");
			gtk_dialog_add_buttons(GTK_DIALOG(q), "_Cancel",
					       GTK_RESPONSE_CANCEL, "_Replace",
					       GTK_RESPONSE_ACCEPT, NULL);
			iv_add_class(gtk_dialog_get_widget_for_response(
					     GTK_DIALOG(q), GTK_RESPONSE_ACCEPT),
				     "destructive-action");
			response = gtk_dialog_run(GTK_DIALOG(q));
			gtk_widget_destroy(q);
			if (response != GTK_RESPONSE_ACCEPT)
				return; /* back to the dialog */
		}
		start_resize(r, replace);
	}
	gtk_widget_destroy(GTK_WIDGET(d));
	resize_free(r);
}

void iv_dialog_resize(IvWindow *w)
{
	Resize *r = g_new0(Resize, 1);
	GtkWidget *content, *grid, *sizes, *box;
	IvPicture *p = w->pic;
	IvFormat f = iv_format_for_mime(p->mime);
	char buf[128];
	int row = 0;

	r->w = w;
	r->pic = iv_picture_ref(p);
	r->dialog = gtk_dialog_new_with_buttons("Resize picture",
						GTK_WINDOW(w->window),
						GTK_DIALOG_MODAL |
							GTK_DIALOG_DESTROY_WITH_PARENT,
						"_Cancel", GTK_RESPONSE_CANCEL,
						"_Save", GTK_RESPONSE_OK, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(r->dialog), GTK_RESPONSE_OK);
	iv_add_class(gtk_dialog_get_widget_for_response(GTK_DIALOG(r->dialog),
							GTK_RESPONSE_OK),
		     "suggested-action");
	content = gtk_dialog_get_content_area(GTK_DIALOG(r->dialog));
	grid = gtk_grid_new();
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

	g_snprintf(buf, sizeof buf, "Now %d × %d pixels", p->width, p->height);
	box = gtk_label_new(buf);
	gtk_label_set_xalign(GTK_LABEL(box), 0);
	iv_add_class(box, "dim-label");
	gtk_grid_attach(GTK_GRID(grid), box, 0, row++, 3, 1);

	static const char *const names[R_COUNT] = {
		"_Small (640 pixels on the long side)",
		"_Medium (1280 pixels on the long side)",
		"_Large (1920 pixels on the long side)",
		"Size of my _own",
		"_Percentage",
	};
	for (int i = 0; i < R_COUNT; i++) {
		r->radio[i] = gtk_radio_button_new_with_mnemonic_from_widget(
			i ? GTK_RADIO_BUTTON(r->radio[0]) : NULL, names[i]);
		g_signal_connect(r->radio[i], "toggled", G_CALLBACK(radio_toggled), r);
		if (i <= R_LARGE)
			gtk_widget_set_sensitive(r->radio[i],
						 MAX(p->width, p->height) >
							 preset_edge[i]);
		gtk_grid_attach(GTK_GRID(grid), r->radio[i], 0, row++, 3, 1);
		if (i == R_CUSTOM) {
			sizes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			gtk_widget_set_margin_start(sizes, 28);
			r->width = gtk_spin_button_new_with_range(1, 65535, 1);
			r->height = gtk_spin_button_new_with_range(1, 65535, 1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->width), p->width);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->height), p->height);
			r->lock = gtk_toggle_button_new();
			gtk_container_add(GTK_CONTAINER(r->lock),
					  iv_glyph_new(IV_GLYPH_LOCK, 16));
			gtk_widget_set_tooltip_text(r->lock, "Keep the proportions");
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->lock), TRUE);
			g_signal_connect(r->width, "value-changed",
					 G_CALLBACK(side_changed), r);
			g_signal_connect(r->height, "value-changed",
					 G_CALLBACK(side_changed), r);
			g_signal_connect(r->lock, "toggled", G_CALLBACK(lock_toggled),
					 r);
			gtk_box_pack_start(GTK_BOX(sizes), row_label("Width"), FALSE,
					   FALSE, 0);
			gtk_box_pack_start(GTK_BOX(sizes), r->width, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(sizes), r->lock, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(sizes), row_label("Height"), FALSE,
					   FALSE, 0);
			gtk_box_pack_start(GTK_BOX(sizes), r->height, FALSE, FALSE, 0);
			gtk_grid_attach(GTK_GRID(grid), sizes, 0, row++, 3, 1);
		} else if (i == R_PERCENT) {
			sizes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			gtk_widget_set_margin_start(sizes, 28);
			r->percent = gtk_spin_button_new_with_range(1, 400, 1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(r->percent), 50);
			g_signal_connect(r->percent, "value-changed",
					 G_CALLBACK(anything_changed), r);
			gtk_box_pack_start(GTK_BOX(sizes), r->percent, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(sizes), gtk_label_new("%"), FALSE,
					   FALSE, 0);
			gtk_grid_attach(GTK_GRID(grid), sizes, 0, row++, 3, 1);
		}
	}
	/* The largest preset smaller than the picture to start with. */
	{
		int start = R_PERCENT;

		for (int i = R_LARGE; i >= R_SMALL; i--)
			if (gtk_widget_get_sensitive(r->radio[i])) {
				start = i;
				break;
			}
		if (start == R_LARGE && gtk_widget_get_sensitive(r->radio[R_MEDIUM]))
			start = R_MEDIUM;
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->radio[start]), TRUE);
	}

	gtk_grid_attach(GTK_GRID(grid), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
			0, row++, 3, 1);
	r->format = format_combo(f != IV_FORMAT_NONE ? f :
				 p->has_alpha	     ? IV_FORMAT_PNG :
						       IV_FORMAT_JPEG);
	g_signal_connect(r->format, "changed", G_CALLBACK(anything_changed), r);
	gtk_grid_attach(GTK_GRID(grid), row_label("_Format"), 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), r->format, 1, row++, 2, 1);
	r->quality_label = row_label("_Quality");
	r->quality = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(r->quality), FALSE);
	gtk_range_set_value(GTK_RANGE(r->quality), iv_settings_get()->jpeg_quality);
	gtk_widget_set_hexpand(r->quality, TRUE);
	r->quality_value = gtk_label_new(NULL);
	g_signal_connect(r->quality, "value-changed", G_CALLBACK(quality_shown),
			 r->quality_value);
	g_signal_connect(r->quality, "value-changed", G_CALLBACK(anything_changed),
			 r);
	quality_shown(GTK_RANGE(r->quality), r->quality_value);
	gtk_grid_attach(GTK_GRID(grid), r->quality_label, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), r->quality, 1, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), r->quality_value, 2, row++, 1, 1);
	r->estimate = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(r->estimate), 0);
	iv_add_class(r->estimate, "dim-label");
	gtk_grid_attach(GTK_GRID(grid), r->estimate, 0, row++, 3, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
			0, row++, 3, 1);
	r->copy = gtk_radio_button_new_with_mnemonic(NULL,
						     "Save as a _copy "
						     "(“name (resized)”)");
	r->replace = gtk_radio_button_new_with_mnemonic_from_widget(
		GTK_RADIO_BUTTON(r->copy), "_Replace the original");
	gtk_widget_set_sensitive(r->replace, iv_picture_writable(p) &&
						     !p->reduced);
	gtk_grid_attach(GTK_GRID(grid), r->copy, 0, row++, 3, 1);
	gtk_grid_attach(GTK_GRID(grid), r->replace, 0, row++, 3, 1);

	gtk_container_add(GTK_CONTAINER(content), grid);
	g_signal_connect(r->dialog, "response", G_CALLBACK(resize_response), r);
	iv_fixed_dialog(r->dialog);
	gtk_widget_show_all(r->dialog);
	resize_sync(r);
}

/* ---- Settings ------------------------------------------------------------------ */

typedef struct {
	IvWindow *w;
	GtkWidget *backdrop, *wheel, *interval, *confirm, *quality, *exif;
	GtkWidget *film, *loop, *smooth, *info;
} Settings;

static void settings_changed(GtkWidget *wid, Settings *st)
{
	IvSettings *s = iv_settings_get();
	static const int intervals[] = { 3, 5, 10 };
	int i;

	s->backdrop = (IvBackdrop)gtk_combo_box_get_active(GTK_COMBO_BOX(st->backdrop));
	s->wheel_zooms = gtk_combo_box_get_active(GTK_COMBO_BOX(st->wheel)) == 0;
	i = gtk_combo_box_get_active(GTK_COMBO_BOX(st->interval));
	if (i >= 0 && i < 3)
		s->slideshow_seconds = intervals[i];
	s->confirm_delete = gtk_switch_get_active(GTK_SWITCH(st->confirm));
	s->jpeg_quality = (int)gtk_range_get_value(GTK_RANGE(st->quality));
	s->keep_exif = gtk_switch_get_active(GTK_SWITCH(st->exif));
	s->show_filmstrip = gtk_switch_get_active(GTK_SWITCH(st->film));
	s->show_info = gtk_switch_get_active(GTK_SWITCH(st->info));
	s->loop = gtk_switch_get_active(GTK_SWITCH(st->loop));
	s->smooth_zoom = gtk_switch_get_active(GTK_SWITCH(st->smooth));
	iv_settings_save();
	iv_window_settings_changed(st->w);
}

static void switch_changed(GObject *o, GParamSpec *ps, Settings *st)
{
	settings_changed(GTK_WIDGET(o), st);
}

static GtkWidget *settings_switch(gboolean on, Settings *st)
{
	GtkWidget *sw = gtk_switch_new();

	gtk_switch_set_active(GTK_SWITCH(sw), on);
	gtk_widget_set_halign(sw, GTK_ALIGN_END);
	gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
	g_signal_connect(sw, "notify::active", G_CALLBACK(switch_changed), st);
	return sw;
}

static GtkWidget *combo(const char *const *items, int n, int active)
{
	GtkWidget *c = gtk_combo_box_text_new();

	for (int i = 0; i < n; i++)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), items[i]);
	gtk_combo_box_set_active(GTK_COMBO_BOX(c), active);
	gtk_widget_set_halign(c, GTK_ALIGN_END);
	return c;
}

static void settings_row(GtkWidget *grid, int row, const char *title,
			 const char *detail, GtkWidget *control)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *t = gtk_label_new(title);

	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	if (detail) {
		GtkWidget *d = gtk_label_new(detail);

		gtk_label_set_xalign(GTK_LABEL(d), 0);
		gtk_label_set_line_wrap(GTK_LABEL(d), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(d), 44);
		iv_add_class(d, "dim-label");
		gtk_box_pack_start(GTK_BOX(box), d, FALSE, FALSE, 0);
	}
	gtk_widget_set_hexpand(box, TRUE);
	gtk_grid_attach(GTK_GRID(grid), box, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
}

static GtkWidget *settings_heading(const char *text)
{
	GtkWidget *l = gtk_label_new(NULL);
	char *m = g_markup_printf_escaped("<b>%s</b>", text);

	gtk_label_set_markup(GTK_LABEL(l), m);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_margin_top(l, 8);
	g_free(m);
	return l;
}

static void settings_response(GtkDialog *d, int r, Settings *st)
{
	gtk_widget_destroy(GTK_WIDGET(d));
	g_free(st);
}

void iv_dialog_settings(IvWindow *w)
{
	static const char *const backdrops[] = { "Dark", "Darker", "Checkerboard" };
	static const char *const wheels[] = { "Zooms", "Goes to the next picture" };
	static const char *const intervals[] = { "3 seconds", "5 seconds",
						 "10 seconds" };
	IvSettings *s = iv_settings_get();
	Settings *st = g_new0(Settings, 1);
	GtkWidget *d, *grid;
	int row = 0, iv = s->slideshow_seconds <= 3 ? 0 : s->slideshow_seconds <= 5 ? 1 : 2;

	st->w = w;
	d = gtk_dialog_new_with_buttons("Settings", GTK_WINDOW(w->window),
					GTK_DIALOG_DESTROY_WITH_PARENT, "_Close",
					GTK_RESPONSE_CLOSE, NULL);
	grid = gtk_grid_new();
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 24);

	gtk_grid_attach(GTK_GRID(grid), settings_heading("Viewing"), 0, row++, 2, 1);
	st->backdrop = combo(backdrops, 3, s->backdrop);
	settings_row(grid, row++, "Background", "What is behind the picture",
		     st->backdrop);
	st->wheel = combo(wheels, 2, s->wheel_zooms ? 0 : 1);
	settings_row(grid, row++, "Mouse wheel",
		     "With Ctrl held the wheel always zooms", st->wheel);
	st->smooth = settings_switch(s->smooth_zoom, st);
	settings_row(grid, row++, "Smooth zoom", NULL, st->smooth);
	st->loop = settings_switch(s->loop, st);
	settings_row(grid, row++, "Go round the folder",
		     "After the last picture comes the first", st->loop);
	st->film = settings_switch(s->show_filmstrip, st);
	settings_row(grid, row++, "Show the filmstrip at start", NULL, st->film);
	st->info = settings_switch(s->show_info, st);
	settings_row(grid, row++, "Show the info panel at start", NULL, st->info);
	st->interval = combo(intervals, 3, iv);
	settings_row(grid, row++, "Slideshow", "How long each picture is shown",
		     st->interval);

	gtk_grid_attach(GTK_GRID(grid), settings_heading("Files"), 0, row++, 2, 1);
	st->confirm = settings_switch(s->confirm_delete, st);
	settings_row(grid, row++, "Ask before moving to the Trash",
		     "Moving to the Trash can always be undone", st->confirm);
	st->quality = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 10, 100, 1);
	gtk_scale_set_value_pos(GTK_SCALE(st->quality), GTK_POS_RIGHT);
	gtk_scale_set_digits(GTK_SCALE(st->quality), 0);
	gtk_range_set_value(GTK_RANGE(st->quality), s->jpeg_quality);
	gtk_widget_set_size_request(st->quality, 180, -1);
	settings_row(grid, row++, "JPEG quality",
		     "For saving, turning and resizing JPEG pictures", st->quality);
	st->exif = settings_switch(s->keep_exif, st);
	settings_row(grid, row++, "Keep the camera data",
		     "Date, camera and position stay in saved JPEG files",
		     st->exif);

	g_signal_connect(st->backdrop, "changed", G_CALLBACK(settings_changed), st);
	g_signal_connect(st->wheel, "changed", G_CALLBACK(settings_changed), st);
	g_signal_connect(st->interval, "changed", G_CALLBACK(settings_changed), st);
	g_signal_connect(st->quality, "value-changed", G_CALLBACK(settings_changed),
			 st);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			  grid);
	g_signal_connect(d, "response", G_CALLBACK(settings_response), st);
	iv_fixed_dialog(d);
	gtk_widget_show_all(d);
}

/* ---- Keyboard shortcuts -------------------------------------------------------- */

static const struct {
	const char *keys, *what;
} shortcut_list[] = {
	{ NULL, "Viewing" },
	{ "← → · Page Up/Down", "Previous / next picture" },
	{ "Home · End", "First / last picture" },
	{ "+ · − · Ctrl+wheel", "Zoom in / out" },
	{ "0 · 1", "Fit to window / actual size" },
	{ "Double click", "Fit ↔ actual size" },
	{ "F11 · F", "Full screen" },
	{ "F5 · S", "Slideshow" },
	{ "Space", "Pause the slideshow (else next picture)" },
	{ "T", "Filmstrip" },
	{ "I · Alt+Enter", "Info panel" },
	{ "Esc", "Leave full screen, close a panel" },
	{ NULL, "The picture" },
	{ "E · Ctrl+E", "Edit" },
	{ "R · Shift+R", "Rotate right / left" },
	{ "Delete", "Move to the Trash" },
	{ "F2", "Rename" },
	{ "Ctrl+S", "Save as" },
	{ "Ctrl+C", "Copy the picture" },
	{ "Ctrl+Shift+C", "Copy the path" },
	{ "Ctrl+O", "Open another picture" },
	{ NULL, "Editing" },
	{ "Ctrl+Z", "Undo" },
	{ "Ctrl+Y · Ctrl+Shift+Z", "Redo" },
	{ "\\ (hold)", "Compare with the original" },
	{ "C · A · S · D · R · B", "Crop, Adjust, Styles, Draw, Retouch, Background" },
	{ "P · M · K · X · E", "Draw: pen, marker, calligraphy, rectangle, eraser" },
	{ "Delete", "Draw: remove the chosen rectangle" },
	{ "Ctrl+S · Ctrl+Shift+S", "Save / save a copy" },
	{ "Esc", "Back to the viewer" },
	{ NULL, "Window" },
	{ "F1", "This list" },
	{ "Ctrl+,", "Settings" },
	{ "Ctrl+W · Ctrl+Q", "Close" },
};

static void shortcuts_response(GtkDialog *d, int r, gpointer data)
{
	gtk_widget_destroy(GTK_WIDGET(d));
}

void iv_dialog_shortcuts(IvWindow *w)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Keyboard shortcuts",
						   GTK_WINDOW(w->window),
						   GTK_DIALOG_DESTROY_WITH_PARENT,
						   "_Close", GTK_RESPONSE_CLOSE,
						   NULL);
	GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
	GtkWidget *grid = gtk_grid_new();
	int row = 0;

	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
	for (gsize i = 0; i < G_N_ELEMENTS(shortcut_list); i++) {
		if (!shortcut_list[i].keys) {
			gtk_grid_attach(GTK_GRID(grid),
					settings_heading(shortcut_list[i].what), 0,
					row++, 2, 1);
			continue;
		}
		GtkWidget *k = gtk_label_new(shortcut_list[i].keys);
		GtkWidget *v = gtk_label_new(shortcut_list[i].what);

		gtk_label_set_xalign(GTK_LABEL(k), 0);
		gtk_label_set_xalign(GTK_LABEL(v), 0);
		iv_add_class(k, "iv-shortcut-keys");
		gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), v, 1, row++, 1, 1);
	}
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller),
						   440);
	gtk_scrolled_window_set_propagate_natural_width(
		GTK_SCROLLED_WINDOW(scroller), TRUE);
	gtk_container_add(GTK_CONTAINER(scroller), grid);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			   scroller, TRUE, TRUE, 0);
	g_signal_connect(d, "response", G_CALLBACK(shortcuts_response), NULL);
	gtk_widget_show_all(d);
}

/* ---- About ------------------------------------------------------------------------ */

void iv_dialog_about(IvWindow *w)
{
	static const char *const authors[] = { "The LikeOS Project", NULL };

	gtk_show_about_dialog(GTK_WINDOW(w->window), "program-name", IV_APP_NAME,
			      "version", IV_VERSION, "logo-icon-name", IV_ICON_NAME,
			      "comments",
			      "View, browse and edit pictures: crop and "
			      "straighten, adjust light and colour, apply "
			      "styles, draw, retouch spots and change the "
			      "background.",
			      "copyright", "Copyright (C) 2026 The LikeOS Project",
			      "license-type", GTK_LICENSE_GPL_2_0_ONLY, "authors",
			      authors, NULL);
}
