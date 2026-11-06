// LikeOS -- LikeOS Image Viewer: the info panel.
//
// A dark card at the right of the picture with everything known about the
// file: its name (edited in place: Enter renames the file, Esc leaves it
// as it was), when it was taken and last changed (the change time can be
// set, from a calendar or from the time the camera wrote), the picture's
// own facts (size, resolution, depth, colours, format), where it is (the
// folder, which opens in LikeOS Explorer), and under "More details" what
// the camera recorded: make and model, lens, exposure, aperture,
// sensitivity, focal length, flash, orientation, software, position and
// colour space -- from the EXIF block, read by ivproc.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"

struct IvInfoPanel {
	IvWindow *w;
	GtkWidget *root, *content;
	IvPicture *pic;
	char *path;

	GtkWidget *name_entry;
	gboolean renaming, cancelled;

	GtkWidget *taken_box, *taken_value, *use_taken;
	GtkWidget *modified_value, *date_button, *date_popover;
	GtkWidget *calendar, *hour, *minute;

	GtkWidget *image_grid;
	GtkWidget *location;
	GtkWidget *camera_expander, *camera_grid, *camera_empty;
};

/* ---- building blocks ------------------------------------------------------------ */

static GtkWidget *section(IvInfoPanel *ip, IvGlyph g, const char *title)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *l = gtk_label_new(title);

	iv_add_class(row, "iv-info-section");
	gtk_box_pack_start(GTK_BOX(row), iv_glyph_new(g, 16), FALSE, FALSE, 0);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	iv_add_class(l, "iv-info-section-title");
	gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(ip->content), row, FALSE, FALSE, 0);
	return row;
}

static GtkWidget *new_grid(void)
{
	GtkWidget *g = gtk_grid_new();

	gtk_grid_set_column_spacing(GTK_GRID(g), 12);
	gtk_grid_set_row_spacing(GTK_GRID(g), 6);
	iv_add_class(g, "iv-info-grid");
	return g;
}

/* One fact: the name in secondary text at the left, the value at the
 * right (selectable, so it can be copied). */
static void fact(GtkWidget *grid, const char *key, const char *value)
{
	GtkWidget *k, *v;
	int row = 0;
	GList *kids = gtk_container_get_children(GTK_CONTAINER(grid));

	row = (int)g_list_length(kids) / 2;
	g_list_free(kids);
	k = gtk_label_new(key);
	gtk_label_set_xalign(GTK_LABEL(k), 0);
	gtk_label_set_yalign(GTK_LABEL(k), 0);
	/* One column width for every section, so the values line up. */
	gtk_widget_set_size_request(k, 104, -1);
	iv_add_class(k, "iv-info-key");
	v = gtk_label_new(value);
	gtk_label_set_xalign(GTK_LABEL(v), 0);
	gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 24);
	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_widget_set_can_focus(v, FALSE);
	gtk_widget_set_hexpand(v, TRUE);
	iv_add_class(v, "iv-info-value");
	gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
	gtk_widget_show(k);
	gtk_widget_show(v);
}

static void clear_grid(GtkWidget *grid)
{
	GList *kids = gtk_container_get_children(GTK_CONTAINER(grid));

	for (GList *l = kids; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);
}

static GtkWidget *quick(IvGlyph g, const char *label, const char *tip)
{
	GtkWidget *b = gtk_button_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	GtkWidget *l = gtk_label_new(label);

	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(box), iv_glyph_new(g, 18), FALSE, FALSE, 0);
	iv_add_class(l, "iv-quick-label");
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(b), box);
	gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
	gtk_widget_set_can_focus(b, FALSE);
	gtk_widget_set_tooltip_text(b, tip);
	iv_add_class(b, "iv-quick");
	return b;
}

/* ---- the name ------------------------------------------------------------------- */

static void select_stem(GtkEntry *e)
{
	const char *t = gtk_entry_get_text(e);
	const char *dot = strrchr(t, '.');

	if (dot && dot != t)
		gtk_editable_select_region(GTK_EDITABLE(e), 0,
					   (int)g_utf8_pointer_to_offset(t, dot));
	else
		gtk_editable_select_region(GTK_EDITABLE(e), 0, -1);
}

static void show_name(IvInfoPanel *ip)
{
	char *name = ip->path ? g_filename_display_basename(ip->path) : g_strdup("");

	gtk_entry_set_text(GTK_ENTRY(ip->name_entry), name);
	g_free(name);
}

static void finish_rename(IvInfoPanel *ip)
{
	const char *text;
	char *old;

	if (!ip->renaming)
		return;
	ip->renaming = FALSE;
	if (ip->cancelled || !ip->path) {
		ip->cancelled = FALSE;
		show_name(ip);
		return;
	}
	text = gtk_entry_get_text(GTK_ENTRY(ip->name_entry));
	old = g_filename_display_basename(ip->path);
	if (strcmp(text, old) != 0 && !iv_cmd_rename_to(ip->w, text))
		show_name(ip);
	g_free(old);
}

static void name_activated(GtkEntry *e, IvInfoPanel *ip)
{
	finish_rename(ip);
	gtk_widget_grab_focus(IV_CANVAS_GTK(ip->w->canvas));
}

static gboolean name_key(GtkWidget *e, GdkEventKey *ev, IvInfoPanel *ip)
{
	if (ev->keyval == GDK_KEY_Escape) {
		ip->cancelled = TRUE;
		finish_rename(ip);
		gtk_widget_grab_focus(IV_CANVAS_GTK(ip->w->canvas));
		return TRUE;
	}
	return FALSE;
}

static gboolean name_focus_in(GtkWidget *e, GdkEventFocus *ev, IvInfoPanel *ip)
{
	ip->renaming = TRUE;
	ip->cancelled = FALSE;
	select_stem(GTK_ENTRY(e));
	return FALSE;
}

static gboolean name_focus_out(GtkWidget *e, GdkEventFocus *ev, IvInfoPanel *ip)
{
	finish_rename(ip);
	return FALSE;
}

/* ---- the dates ----------------------------------------------------------------- */

static void use_taken(GtkButton *b, IvInfoPanel *ip)
{
	gint64 t;

	if (!ip->pic)
		return;
	t = iv_parse_exif_datetime(ip->pic->exif.datetime_original[0] ?
					   ip->pic->exif.datetime_original :
					   ip->pic->exif.datetime);
	if (t > 0 && iv_cmd_set_mtime(ip->w, t))
		iv_window_toast(ip->w, "The file date is now the date taken");
}

static void date_set(GtkButton *b, IvInfoPanel *ip)
{
	guint y, m, d;
	GDateTime *dt;

	gtk_calendar_get_date(GTK_CALENDAR(ip->calendar), &y, &m, &d);
	dt = g_date_time_new_local((int)y, (int)m + 1, (int)d,
				   gtk_spin_button_get_value_as_int(
					   GTK_SPIN_BUTTON(ip->hour)),
				   gtk_spin_button_get_value_as_int(
					   GTK_SPIN_BUTTON(ip->minute)),
				   0);
	gtk_popover_popdown(GTK_POPOVER(ip->date_popover));
	if (!dt)
		return;
	if (iv_cmd_set_mtime(ip->w, g_date_time_to_unix(dt)))
		iv_window_toast(ip->w, "The file date was changed");
	g_date_time_unref(dt);
}

static void date_clicked(GtkButton *b, IvInfoPanel *ip)
{
	GDateTime *dt;

	if (!ip->pic)
		return;
	dt = g_date_time_new_from_unix_local(ip->pic->mtime);
	if (dt) {
		gtk_calendar_select_month(GTK_CALENDAR(ip->calendar),
					  (guint)g_date_time_get_month(dt) - 1,
					  (guint)g_date_time_get_year(dt));
		gtk_calendar_select_day(GTK_CALENDAR(ip->calendar),
					(guint)g_date_time_get_day_of_month(dt));
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(ip->hour),
					  g_date_time_get_hour(dt));
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(ip->minute),
					  g_date_time_get_minute(dt));
		g_date_time_unref(dt);
	}
	gtk_popover_popup(GTK_POPOVER(ip->date_popover));
}

static GtkWidget *build_date_popover(IvInfoPanel *ip, GtkWidget *relative)
{
	GtkWidget *pop = gtk_popover_new(relative);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *time = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *set = gtk_button_new_with_mnemonic("_Set date");
	GtkWidget *l = gtk_label_new("Time");

	gtk_container_set_border_width(GTK_CONTAINER(box), 10);
	ip->calendar = gtk_calendar_new();
	gtk_box_pack_start(GTK_BOX(box), ip->calendar, FALSE, FALSE, 0);
	ip->hour = gtk_spin_button_new_with_range(0, 23, 1);
	ip->minute = gtk_spin_button_new_with_range(0, 59, 1);
	gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(ip->hour), TRUE);
	gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(ip->minute), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(time), l, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(time), ip->hour, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(time), gtk_label_new(":"), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(time), ip->minute, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), time, FALSE, FALSE, 0);
	iv_add_class(set, "suggested-action");
	g_signal_connect(set, "clicked", G_CALLBACK(date_set), ip);
	gtk_box_pack_start(GTK_BOX(box), set, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(pop), box);
	gtk_widget_show_all(box);
	iv_add_class(pop, "iv-date-popover");
	return pop;
}

/* ---- the rest ------------------------------------------------------------------- */

static void close_clicked(GtkButton *b, IvInfoPanel *ip)
{
	iv_viewer_set_info(ip->w, FALSE);
}

static void q_resize(GtkButton *b, IvInfoPanel *ip)
{
	iv_cmd_resize(ip->w);
}

static void q_copy_path(GtkButton *b, IvInfoPanel *ip)
{
	iv_cmd_copy_path(ip->w);
}

static void q_open_folder(GtkButton *b, IvInfoPanel *ip)
{
	iv_cmd_show_in_explorer(ip->w);
}

static void q_wallpaper(GtkButton *b, IvInfoPanel *ip)
{
	iv_cmd_wallpaper(ip->w);
}

static void location_clicked(GtkButton *b, IvInfoPanel *ip)
{
	iv_cmd_open_folder(ip->w);
}

static void fill_image(IvInfoPanel *ip, IvPicture *p)
{
	GtkWidget *g = ip->image_grid;
	char buf[128];
	char *s;

	clear_grid(g);
	if (!p) {
		return;
	}
	if (!p->error) {
		g_snprintf(buf, sizeof buf, "%d × %d pixels%s", p->width, p->height,
			   p->reduced ? " (shown reduced)" : "");
		fact(g, "Dimensions", buf);
		g_snprintf(buf, sizeof buf, "%.1f megapixels",
			   (double)p->width * p->height / 1e6);
		fact(g, "Resolution", buf);
	}
	s = iv_format_size(p->file_size);
	g_snprintf(buf, sizeof buf, "%s (%" G_GINT64_FORMAT " bytes)", s,
		   (gint64)p->file_size);
	g_free(s);
	fact(g, "File size", buf);
	if (p->exif.x_dpi > 0) {
		if (p->exif.y_dpi > 0 && fabs(p->exif.y_dpi - p->exif.x_dpi) > 0.5)
			g_snprintf(buf, sizeof buf, "%.0f × %.0f dpi", p->exif.x_dpi,
				   p->exif.y_dpi);
		else
			g_snprintf(buf, sizeof buf, "%.0f dpi", p->exif.x_dpi);
		fact(g, "Print resolution", buf);
	}
	if (p->bits > 0) {
		if (p->channels > 1 && p->bits % p->channels == 0)
			g_snprintf(buf, sizeof buf, "%d bits per pixel (%d per channel)",
				   p->bits, p->bits / p->channels);
		else
			g_snprintf(buf, sizeof buf, "%d bits per pixel", p->bits);
		fact(g, "Bit depth", buf);
	}
	if (p->exif.color_type[0])
		fact(g, "Colour", p->exif.color_type);
	else if (!p->error)
		fact(g, "Colour", p->has_alpha ? "RGB with transparency" : "RGB");
	if (p->anim)
		fact(g, "Animation", "Several frames (plays in the viewer)");
	g_snprintf(buf, sizeof buf, "%s (%s)", p->format, p->mime);
	fact(g, "Format", buf);
}

static void camera_fact(IvInfoPanel *ip, const char *key, const char *value)
{
	if (value && *value)
		fact(ip->camera_grid, key, value);
}

static void fill_camera(IvInfoPanel *ip, IvPicture *p)
{
	const IvExif *x;
	char buf[192];

	clear_grid(ip->camera_grid);
	if (!p || p->exif_status <= 0 || !p->exif.has_exif) {
		gtk_widget_show(ip->camera_empty);
		return;
	}
	gtk_widget_hide(ip->camera_empty);
	x = &p->exif;
	if (x->make[0] || x->model[0]) {
		/* The model often repeats the make ("Canon" "Canon EOS R6"). */
		if (x->make[0] && x->model[0] &&
		    g_ascii_strncasecmp(x->model, x->make, strlen(x->make)) != 0)
			g_snprintf(buf, sizeof buf, "%s %s", x->make, x->model);
		else
			g_snprintf(buf, sizeof buf, "%s", x->model[0] ? x->model : x->make);
		camera_fact(ip, "Camera", buf);
	}
	if (x->lens[0]) {
		if (x->lens_make[0] && !strstr(x->lens, x->lens_make))
			g_snprintf(buf, sizeof buf, "%s %s", x->lens_make, x->lens);
		else
			g_snprintf(buf, sizeof buf, "%s", x->lens);
		camera_fact(ip, "Lens", buf);
	}
	if (x->exposure_time > 0) {
		iv_exif_format_exposure(x->exposure_time, buf, sizeof buf);
		camera_fact(ip, "Exposure", buf);
	}
	if (x->fnumber > 0) {
		g_snprintf(buf, sizeof buf, "f/%.1f", x->fnumber);
		camera_fact(ip, "Aperture", buf);
	}
	if (x->iso > 0) {
		g_snprintf(buf, sizeof buf, "ISO %d", x->iso);
		camera_fact(ip, "Sensitivity", buf);
	}
	if (x->focal_length > 0) {
		if (x->focal_length_35mm > 0)
			g_snprintf(buf, sizeof buf, "%.0f mm (%d mm full frame)",
				   x->focal_length, x->focal_length_35mm);
		else
			g_snprintf(buf, sizeof buf, "%.0f mm", x->focal_length);
		camera_fact(ip, "Focal length", buf);
	}
	if (fabs(x->exposure_bias) > 0.01) {
		g_snprintf(buf, sizeof buf, "%+.1f EV", x->exposure_bias);
		camera_fact(ip, "Exposure bias", buf);
	}
	if (x->exposure_program > 0)
		camera_fact(ip, "Program", iv_exif_exposure_program_string(
						   x->exposure_program));
	if (x->metering_mode > 0)
		camera_fact(ip, "Metering", iv_exif_metering_mode_string(
						    x->metering_mode));
	if (x->white_balance >= 0)
		camera_fact(ip, "White balance",
			    x->white_balance == 1 ? "Manual" : "Automatic");
	if (x->flash >= 0)
		camera_fact(ip, "Flash", iv_exif_flash_string(x->flash));
	if (x->orientation > 1)
		camera_fact(ip, "Orientation",
			    iv_exif_orientation_string(x->orientation));
	if (x->color_space > 0)
		camera_fact(ip, "Colour space",
			    iv_exif_color_space_string(x->color_space));
	camera_fact(ip, "Software", x->software);
	if (x->has_gps) {
		char lat[48], lon[48];

		g_snprintf(buf, sizeof buf, "%.6f, %.6f", x->gps_lat, x->gps_lon);
		camera_fact(ip, "Position", buf);
		iv_exif_format_dms(x->gps_lat, 1, lat, sizeof lat);
		iv_exif_format_dms(x->gps_lon, 0, lon, sizeof lon);
		g_snprintf(buf, sizeof buf, "%s\n%s", lat, lon);
		camera_fact(ip, "", buf);
		if (x->has_alt) {
			g_snprintf(buf, sizeof buf, "%.0f m", x->gps_alt);
			camera_fact(ip, "Altitude", buf);
		}
	}
	camera_fact(ip, "Artist", x->artist);
	camera_fact(ip, "Copyright", x->copyright);
	{
		char *d = iv_format_exif_datetime(x->datetime_digitized);

		camera_fact(ip, "Digitised", d);
		g_free(d);
	}
}

static void fill_dates(IvInfoPanel *ip, IvPicture *p)
{
	char *mod, *taken = NULL;
	const char *raw = NULL;

	if (!p) {
		gtk_label_set_text(GTK_LABEL(ip->modified_value), "");
		gtk_widget_hide(ip->taken_box);
		gtk_widget_set_sensitive(ip->date_button, FALSE);
		return;
	}
	mod = iv_format_datetime(p->mtime);
	gtk_label_set_text(GTK_LABEL(ip->modified_value), mod);
	gtk_widget_set_sensitive(ip->date_button, TRUE);
	if (p->exif.datetime_original[0])
		raw = p->exif.datetime_original;
	else if (p->exif.datetime[0])
		raw = p->exif.datetime;
	if (raw)
		taken = iv_format_exif_datetime(raw);
	if (taken && *taken) {
		gint64 t = iv_parse_exif_datetime(raw);

		gtk_label_set_text(GTK_LABEL(ip->taken_value), taken);
		gtk_widget_show(ip->taken_box);
		/* Offered only where it would change something. */
		gtk_widget_set_visible(ip->use_taken, llabs(t - p->mtime) > 60);
	} else {
		gtk_widget_hide(ip->taken_box);
	}
	g_free(taken);
	g_free(mod);
}

static void fill_location(IvInfoPanel *ip)
{
	char *dir = ip->path ? g_path_get_dirname(ip->path) : NULL;
	GtkWidget *l = gtk_bin_get_child(GTK_BIN(ip->location));

	gtk_label_set_text(GTK_LABEL(l), dir ? dir : "");
	gtk_widget_set_tooltip_text(ip->location, dir ? "Open the folder" : NULL);
	gtk_widget_set_sensitive(ip->location, dir != NULL);
	g_free(dir);
}

void iv_info_set_path(IvInfoPanel *ip, const char *path)
{
	if (!ip)
		return;
	if (g_strcmp0(path, ip->path) != 0) {
		g_free(ip->path);
		ip->path = g_strdup(path);
	}
	if (!ip->renaming)
		show_name(ip);
	fill_location(ip);
}

void iv_info_set_picture(IvInfoPanel *ip, IvPicture *p)
{
	if (!ip)
		return;
	if (p != ip->pic) {
		if (ip->pic)
			iv_picture_unref(ip->pic);
		ip->pic = p ? iv_picture_ref(p) : NULL;
	}
	iv_info_set_path(ip, p ? p->path : ip->w->path);
	/* Hidden, it is filled when it is shown. */
	if (!ip->w->info_visible)
		return;
	fill_dates(ip, p);
	fill_image(ip, p);
	fill_camera(ip, p);
	gtk_widget_set_sensitive(ip->name_entry, ip->path != NULL);
}

void iv_info_start_rename(IvInfoPanel *ip)
{
	if (!ip->path)
		return;
	gtk_widget_grab_focus(ip->name_entry);
	select_stem(GTK_ENTRY(ip->name_entry));
}

gboolean iv_info_is_editing(IvInfoPanel *ip)
{
	return ip && ip->renaming;
}

IvInfoPanel *iv_info_new(IvWindow *w)
{
	IvInfoPanel *ip = g_new0(IvInfoPanel, 1);
	GtkWidget *card, *scroller, *head, *t, *close, *quicks, *b, *row, *col;

	ip->w = w;
	card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	iv_add_class(card, "iv-info");
	gtk_widget_set_size_request(card, 340, -1);
	/* A fixed width: the facts wrap inside it, and their own wish to
	 * expand must not reach the window's row. */
	gtk_widget_set_hexpand(card, FALSE);
	ip->root = card;

	head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	iv_add_class(head, "iv-info-head");
	t = gtk_label_new("Info");
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	iv_add_class(t, "iv-info-title");
	gtk_box_pack_start(GTK_BOX(head), t, TRUE, TRUE, 0);
	close = iv_glyph_button(IV_GLYPH_CLOSE, 10, "Close (Esc)", "iv-info-close");
	g_signal_connect(close, "clicked", G_CALLBACK(close_clicked), ip);
	gtk_box_pack_end(GTK_BOX(head), close, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(card), head, FALSE, FALSE, 0);

	scroller = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
				       GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
	ip->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	iv_add_class(ip->content, "iv-info-content");
	gtk_container_add(GTK_CONTAINER(scroller), ip->content);
	gtk_box_pack_start(GTK_BOX(card), scroller, TRUE, TRUE, 0);

	/* Quick actions. */
	quicks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_set_homogeneous(GTK_BOX(quicks), TRUE);
	iv_add_class(quicks, "iv-quicks");
	b = quick(IV_GLYPH_RESIZE, "Resize", "Resize the picture");
	g_signal_connect(b, "clicked", G_CALLBACK(q_resize), ip);
	gtk_box_pack_start(GTK_BOX(quicks), b, TRUE, TRUE, 0);
	b = quick(IV_GLYPH_COPY_PATH, "Copy path", "Copy the file's path");
	g_signal_connect(b, "clicked", G_CALLBACK(q_copy_path), ip);
	gtk_box_pack_start(GTK_BOX(quicks), b, TRUE, TRUE, 0);
	b = quick(IV_GLYPH_FOLDER_OPEN, "Show", "Show in LikeOS Explorer");
	g_signal_connect(b, "clicked", G_CALLBACK(q_open_folder), ip);
	gtk_box_pack_start(GTK_BOX(quicks), b, TRUE, TRUE, 0);
	b = quick(IV_GLYPH_WALLPAPER, "Wallpaper", "Set as desktop wallpaper");
	g_signal_connect(b, "clicked", G_CALLBACK(q_wallpaper), ip);
	gtk_box_pack_start(GTK_BOX(quicks), b, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(ip->content), quicks, FALSE, FALSE, 0);

	/* The name. */
	section(ip, IV_GLYPH_FILE, "File name");
	ip->name_entry = gtk_entry_new();
	iv_add_class(ip->name_entry, "iv-info-name");
	gtk_widget_set_tooltip_text(ip->name_entry,
				    "Type a new name and press Enter to rename "
				    "the file (F2)");
	g_signal_connect(ip->name_entry, "activate", G_CALLBACK(name_activated), ip);
	g_signal_connect(ip->name_entry, "key-press-event", G_CALLBACK(name_key), ip);
	g_signal_connect(ip->name_entry, "focus-in-event",
			 G_CALLBACK(name_focus_in), ip);
	g_signal_connect(ip->name_entry, "focus-out-event",
			 G_CALLBACK(name_focus_out), ip);
	gtk_box_pack_start(GTK_BOX(ip->content), ip->name_entry, FALSE, FALSE, 0);

	/* The dates. */
	section(ip, IV_GLYPH_CALENDAR, "Dates");
	ip->taken_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	t = gtk_label_new("Taken");
	iv_add_class(t, "iv-info-key");
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_widget_set_size_request(t, 104, -1);
	ip->taken_value = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(ip->taken_value), 0);
	iv_add_class(ip->taken_value, "iv-info-value");
	gtk_box_pack_start(GTK_BOX(row), t, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), ip->taken_value, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(ip->taken_box), row, FALSE, FALSE, 0);
	ip->use_taken = gtk_button_new_with_label("Use as file date");
	iv_add_class(ip->use_taken, "iv-link");
	gtk_widget_set_halign(ip->use_taken, GTK_ALIGN_START);
	gtk_widget_set_margin_start(ip->use_taken, 108);
	gtk_widget_set_can_focus(ip->use_taken, FALSE);
	g_signal_connect(ip->use_taken, "clicked", G_CALLBACK(use_taken), ip);
	gtk_box_pack_start(GTK_BOX(ip->taken_box), ip->use_taken, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(ip->taken_box, TRUE);
	gtk_widget_show(row);
	gtk_widget_show_all(row);
	gtk_widget_show(ip->use_taken);
	gtk_box_pack_start(GTK_BOX(ip->content), ip->taken_box, FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	t = gtk_label_new("Modified");
	iv_add_class(t, "iv-info-key");
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_widget_set_size_request(t, 104, -1);
	ip->modified_value = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(ip->modified_value), 0);
	iv_add_class(ip->modified_value, "iv-info-value");
	ip->date_button = iv_glyph_button(IV_GLYPH_EDIT, 14, "Change the file date",
					  "iv-info-edit");
	g_signal_connect(ip->date_button, "clicked", G_CALLBACK(date_clicked), ip);
	gtk_box_pack_start(GTK_BOX(row), t, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), ip->modified_value, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), ip->date_button, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(ip->content), row, FALSE, FALSE, 0);
	ip->date_popover = build_date_popover(ip, ip->date_button);

	/* The picture. */
	section(ip, IV_GLYPH_IMAGE, "Picture");
	ip->image_grid = new_grid();
	gtk_box_pack_start(GTK_BOX(ip->content), ip->image_grid, FALSE, FALSE, 0);

	/* Where. */
	section(ip, IV_GLYPH_LOCATION, "Location");
	ip->location = gtk_button_new_with_label("");
	iv_add_class(ip->location, "iv-link");
	gtk_widget_set_can_focus(ip->location, FALSE);
	gtk_widget_set_halign(ip->location, GTK_ALIGN_START);
	{
		GtkWidget *l = gtk_bin_get_child(GTK_BIN(ip->location));

		gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_START);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 34);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
	}
	g_signal_connect(ip->location, "clicked", G_CALLBACK(location_clicked), ip);
	gtk_box_pack_start(GTK_BOX(ip->content), ip->location, FALSE, FALSE, 0);

	/* The camera's record. */
	section(ip, IV_GLYPH_CAMERA, "Camera");
	col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	ip->camera_empty = gtk_label_new("No camera data in this file.");
	gtk_label_set_xalign(GTK_LABEL(ip->camera_empty), 0);
	iv_add_class(ip->camera_empty, "iv-info-key");
	gtk_box_pack_start(GTK_BOX(col), ip->camera_empty, FALSE, FALSE, 0);
	ip->camera_expander = gtk_expander_new_with_mnemonic("_More details");
	iv_add_class(ip->camera_expander, "iv-info-expander");
	gtk_expander_set_expanded(GTK_EXPANDER(ip->camera_expander), TRUE);
	ip->camera_grid = new_grid();
	gtk_widget_set_margin_top(ip->camera_grid, 6);
	gtk_container_add(GTK_CONTAINER(ip->camera_expander), ip->camera_grid);
	gtk_box_pack_start(GTK_BOX(col), ip->camera_expander, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(ip->content), col, FALSE, FALSE, 0);

	gtk_widget_show_all(card);
	gtk_widget_hide(ip->camera_empty);
	return ip;
}

void iv_info_free(IvInfoPanel *ip)
{
	if (!ip)
		return;
	if (ip->pic)
		iv_picture_unref(ip->pic);
	g_free(ip->path);
	g_free(ip);
}

GtkWidget *iv_info_widget(IvInfoPanel *ip)
{
	return ip->root;
}
