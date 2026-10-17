// LikeOS -- Media Player: the secondary windows.
//
// File choosers for media, folders and playlists, the location dialog, the
// media information window, the keyboard reference, the about box and the
// error dialog.
//
// Errors arrive from the GStreamer bus, i.e. from inside a main-loop
// callback, so the error dialog is never run with gtk_dialog_run(): a
// nested main loop there would deliver the next bus message while the
// handler of the previous one is still on the stack.  It is shown and left
// to answer through its "response" signal.  The choosers are opened from
// button clicks and key presses, where a nested loop is harmless.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include <gst/tag/tag.h>

#include "mediaplayer.h"
#include "util.h"

/* ---- generic messages ------------------------------------------------- */

void mp_dialog_message(GtkWindow *parent, GtkMessageType type,
		       const char *primary, const char *secondary)
{
	GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL |
						      GTK_DIALOG_DESTROY_WITH_PARENT,
					      type, GTK_BUTTONS_CLOSE, "%s",
					      primary);
	if (secondary != NULL)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", secondary);
	gtk_window_set_title(GTK_WINDOW(d), MP_APP_NAME);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/* ---- the error dialog ------------------------------------------------- */

enum { RESPONSE_PLAY_NEXT = 1 };

static void on_error_response(GtkDialog *d, gint response, gpointer data)
{
	MpApp *app = data;
	if (app->error_dialog == GTK_WIDGET(d))
		app->error_dialog = NULL;
	gtk_widget_destroy(GTK_WIDGET(d));
	if (response == RESPONSE_PLAY_NEXT && !app->quitting)
		mp_app_next(app, TRUE);
}

void mp_dialog_error(MpApp *app, const char *primary, const char *secondary,
		     const char *details, gboolean offer_next)
{
	/* One at a time: a newer error replaces the one still showing. */
	if (app->error_dialog != NULL) {
		gtk_widget_destroy(app->error_dialog);
		app->error_dialog = NULL;
	}

	GtkWidget *d = gtk_message_dialog_new(
		GTK_WINDOW(app->window), GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_NONE, "%s", primary);
	gtk_window_set_title(GTK_WINDOW(d), MP_APP_NAME);
	if (secondary != NULL)
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
							 "%s", secondary);

	if (details != NULL && *details) {
		/* The technical text, folded away: it is for a bug report,
		 * not for reading. */
		GtkWidget *area = gtk_message_dialog_get_message_area(
			GTK_MESSAGE_DIALOG(d));
		GtkWidget *exp = gtk_expander_new_with_mnemonic(
			"Technical _details");
		GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
					       GTK_POLICY_AUTOMATIC,
					       GTK_POLICY_AUTOMATIC);
		gtk_scrolled_window_set_min_content_height(
			GTK_SCROLLED_WINDOW(sw), 140);
		gtk_scrolled_window_set_min_content_width(
			GTK_SCROLLED_WINDOW(sw), 420);
		gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
						    GTK_SHADOW_IN);
		GtkWidget *tv = gtk_text_view_new();
		gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
		gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv),
					    GTK_WRAP_WORD_CHAR);
		gchar *valid = g_utf8_make_valid(details, -1);
		gtk_text_buffer_set_text(
			gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), valid,
			-1);
		g_free(valid);
		gtk_container_add(GTK_CONTAINER(sw), tv);
		gtk_container_add(GTK_CONTAINER(exp), sw);
		gtk_box_pack_start(GTK_BOX(area), exp, TRUE, TRUE, 0);
		gtk_widget_show_all(exp);
	}

	if (offer_next)
		gtk_dialog_add_button(GTK_DIALOG(d), "Play _Next",
				      RESPONSE_PLAY_NEXT);
	gtk_dialog_add_button(GTK_DIALOG(d), "_Close", GTK_RESPONSE_CLOSE);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE);
	g_signal_connect(d, "response", G_CALLBACK(on_error_response), app);

	/* Over a fullscreen film the dialog would hide behind it. */
	if (app->fullscreen)
		mp_app_set_fullscreen(app, FALSE);
	app->error_dialog = d;
	gtk_window_present(GTK_WINDOW(d));
}

/* ---- file choosers ---------------------------------------------------- */

static void add_media_patterns(GtkFileFilter *f)
{
	const char *const *exts = mp_media_extensions();
	for (int i = 0; exts[i] != NULL; i++) {
		gchar *lo = g_strdup_printf("*.%s", exts[i]);
		gchar *up = g_ascii_strup(lo, -1);
		gtk_file_filter_add_pattern(f, lo);
		gtk_file_filter_add_pattern(f, up);
		g_free(lo);
		g_free(up);
	}
}

static void add_playlist_patterns(GtkFileFilter *f)
{
	static const char *const pats[] = { "*.m3u", "*.M3U", "*.m3u8",
					    "*.M3U8", "*.pls", "*.PLS",
					    NULL };
	for (int i = 0; pats[i] != NULL; i++)
		gtk_file_filter_add_pattern(f, pats[i]);
	gtk_file_filter_add_mime_type(f, "audio/x-mpegurl");
	gtk_file_filter_add_mime_type(f, "audio/x-scpls");
}

static void add_filters(GtkFileChooser *fc, gboolean media)
{
	GtkFileFilter *f;

	if (media) {
		f = gtk_file_filter_new();
		gtk_file_filter_set_name(f, "Music, videos and playlists");
		gtk_file_filter_add_mime_type(f, "audio/*");
		gtk_file_filter_add_mime_type(f, "video/*");
		add_media_patterns(f);
		add_playlist_patterns(f);
		gtk_file_chooser_add_filter(fc, f);

		f = gtk_file_filter_new();
		gtk_file_filter_set_name(f, "Music");
		gtk_file_filter_add_mime_type(f, "audio/*");
		gtk_file_chooser_add_filter(fc, f);

		f = gtk_file_filter_new();
		gtk_file_filter_set_name(f, "Videos");
		gtk_file_filter_add_mime_type(f, "video/*");
		gtk_file_chooser_add_filter(fc, f);
	}

	f = gtk_file_filter_new();
	gtk_file_filter_set_name(f, "Playlists (M3U, PLS)");
	add_playlist_patterns(f);
	gtk_file_chooser_add_filter(fc, f);

	f = gtk_file_filter_new();
	gtk_file_filter_set_name(f, "All files");
	gtk_file_filter_add_pattern(f, "*");
	gtk_file_chooser_add_filter(fc, f);
}

static GtkWidget *chooser_new(MpApp *app, const char *title,
			      GtkFileChooserAction action,
			      const char *accept_label)
{
	GtkWidget *d = gtk_file_chooser_dialog_new(
		title, GTK_WINDOW(app->window), action, "_Cancel",
		GTK_RESPONSE_CANCEL, accept_label, GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(d), FALSE);
	if (app->settings.last_dir != NULL)
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d),
						    app->settings.last_dir);
	else
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d),
						    g_get_home_dir());
	return d;
}

static void remember_folder(MpApp *app, GtkFileChooser *fc,
			    gboolean is_folder_choice)
{
	gchar *dir = NULL;
	if (is_folder_choice) {
		/* The folder that was chosen; next time, open its parent so
		 * a sibling is one click away. */
		gchar *chosen = gtk_file_chooser_get_filename(fc);
		if (chosen != NULL)
			dir = g_path_get_dirname(chosen);
		g_free(chosen);
	} else {
		dir = gtk_file_chooser_get_current_folder(fc);
	}
	if (dir != NULL && g_path_is_absolute(dir)) {
		g_free(app->settings.last_dir);
		app->settings.last_dir = dir;
		mp_app_schedule_save(app);
	} else {
		g_free(dir);
	}
}

static gchar **chooser_uris(GtkFileChooser *fc)
{
	GSList *list = gtk_file_chooser_get_uris(fc);
	guint n = g_slist_length(list);
	gchar **uris = g_new0(gchar *, n + 1);
	guint i = 0;
	for (GSList *l = list; l != NULL; l = l->next)
		uris[i++] = l->data;	/* ownership moves to the array */
	g_slist_free(list);
	return uris;
}

static void choose_media(MpApp *app, gboolean folder, gboolean play)
{
	GtkWidget *d = chooser_new(
		app,
		folder ? (play ? "Open Folder" : "Add Folder to the Queue")
		       : (play ? "Open Files" : "Add Files to the Queue"),
		folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER
		       : GTK_FILE_CHOOSER_ACTION_OPEN,
		play ? "_Open" : "_Add");
	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(d), TRUE);
	if (!folder)
		add_filters(GTK_FILE_CHOOSER(d), TRUE);

	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
		remember_folder(app, GTK_FILE_CHOOSER(d), folder);
		gchar **uris = chooser_uris(GTK_FILE_CHOOSER(d));
		gtk_widget_destroy(d);
		mp_app_add_uris(app, uris, play, -1);
		g_strfreev(uris);
		return;
	}
	gtk_widget_destroy(d);
}

void mp_dialog_open_files(MpApp *app)
{
	choose_media(app, FALSE, TRUE);
}

void mp_dialog_open_folder(MpApp *app)
{
	choose_media(app, TRUE, TRUE);
}

void mp_dialog_add_files(MpApp *app)
{
	choose_media(app, FALSE, FALSE);
}

void mp_dialog_add_folder(MpApp *app)
{
	choose_media(app, TRUE, FALSE);
}

void mp_dialog_open_playlist(MpApp *app)
{
	GtkWidget *d = chooser_new(app, "Open Playlist",
				   GTK_FILE_CHOOSER_ACTION_OPEN, "_Open");
	add_filters(GTK_FILE_CHOOSER(d), FALSE);
	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(d), TRUE);

	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
		remember_folder(app, GTK_FILE_CHOOSER(d), FALSE);
		gchar *uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(d));
		gtk_widget_destroy(d);
		if (uri != NULL) {
			/* Opening a playlist replaces the queue, the way
			 * opening a document replaces what was on screen. */
			mp_app_stop(app);
			mp_app_clear_queue(app);
			gchar *one[] = { uri, NULL };
			mp_app_add_uris(app, one, TRUE, -1);
		}
		g_free(uri);
		return;
	}
	gtk_widget_destroy(d);
}

void mp_dialog_save_playlist(MpApp *app)
{
	if (mp_playlist_count(app->playlist) == 0) {
		mp_dialog_message(GTK_WINDOW(app->window), GTK_MESSAGE_INFO,
				  "The play queue is empty",
				  "Add something to it first.");
		return;
	}
	GtkWidget *d = chooser_new(app, "Save Playlist",
				   GTK_FILE_CHOOSER_ACTION_SAVE, "_Save");
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d),
						       TRUE);
	gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(d), TRUE);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d),
					  "Playlist.m3u8");

	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
		gchar *path = gtk_file_chooser_get_filename(
			GTK_FILE_CHOOSER(d));
		if (path == NULL)
			break;
		/* No extension given: make it an M3U8 so other programs
		 * recognise it. */
		if (strchr(strrchr(path, '/') ? strrchr(path, '/') : path,
			   '.') == NULL) {
			gchar *p2 = g_strconcat(path, ".m3u8", NULL);
			g_free(path);
			path = p2;
		}
		GError *err = NULL;
		gboolean ok = mp_playlist_file_save(app->playlist, path, &err);
		if (ok) {
			remember_folder(app, GTK_FILE_CHOOSER(d), FALSE);
			g_free(path);
			break;
		}
		gchar *disp = g_filename_display_name(path);
		gchar *msg = g_strdup_printf("Could not save \u201c%s\u201d",
					     disp);
		mp_dialog_message(GTK_WINDOW(d), GTK_MESSAGE_ERROR, msg,
				  err->message);
		g_free(msg);
		g_free(disp);
		g_clear_error(&err);
		g_free(path);
	}
	gtk_widget_destroy(d);
}

/* ---- open location ---------------------------------------------------- */

static gboolean looks_like_url(const char *s)
{
	return s != NULL && strstr(s, "://") != NULL && strlen(s) < 4096 &&
	       strchr(s, '\n') == NULL && strchr(s, ' ') == NULL;
}

static void on_url_changed(GtkEditable *e, gpointer data)
{
	GtkDialog *d = data;
	const char *t = gtk_entry_get_text(GTK_ENTRY(e));
	gtk_dialog_set_response_sensitive(d, GTK_RESPONSE_ACCEPT,
					  t != NULL && *t != '\0');
}

void mp_dialog_open_url(MpApp *app)
{
	GtkWidget *d = gtk_dialog_new_with_buttons(
		"Open Location", GTK_WINDOW(app->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel",
		GTK_RESPONSE_CANCEL, "_Play", GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
	gtk_window_set_default_size(GTK_WINDOW(d), 520, -1);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	GtkWidget *lbl = gtk_label_new(
		"Enter the address of a stream or a file on the web:");
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
				       "https://example.org/stream.mp3");
	gtk_entry_set_max_length(GTK_ENTRY(entry), 4000);
	gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_URL);
	GtkWidget *hint = gtk_label_new(
		"HTTP and HTTPS work wherever a source for them is "
		"installed; HLS and DASH streams play too.");
	gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
	gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(hint),
				    "dim-label");
	gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(
				  GTK_DIALOG(d))),
			  box);

	/* Offer what is on the clipboard when it is an address. */
	GtkClipboard *cb = gtk_clipboard_get_for_display(
		gtk_widget_get_display(app->window), GDK_SELECTION_CLIPBOARD);
	gchar *clip = gtk_clipboard_wait_for_text(cb);
	if (clip != NULL) {
		g_strstrip(clip);
		if (looks_like_url(clip)) {
			gtk_entry_set_text(GTK_ENTRY(entry), clip);
			gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
		}
		g_free(clip);
	}
	g_signal_connect(entry, "changed", G_CALLBACK(on_url_changed), d);
	on_url_changed(GTK_EDITABLE(entry), d);
	gtk_widget_show_all(d);

	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
		gchar *text = g_strstrip(
			g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
		gchar *uri = mp_uri_from_arg(text, NULL);
		g_free(text);
		if (uri != NULL && (mp_uri_is_playable_scheme(uri) ||
				    mp_uri_is_local(uri))) {
			gtk_widget_destroy(d);
			gchar *one[] = { uri, NULL };
			mp_app_add_uris(app, one, TRUE, -1);
			g_free(uri);
			return;
		}
		gchar *proto = uri ? gst_uri_get_protocol(uri) : NULL;
		gchar *msg = proto ? g_strdup_printf(
					     "No GStreamer source element "
					     "handles \u201c%s:\u201d "
					     "addresses on this system.",
					     proto)
				   : g_strdup("Enter a complete address, "
					      "starting with http:// or "
					      "https://.");
		mp_dialog_message(GTK_WINDOW(d), GTK_MESSAGE_WARNING,
				  "This address cannot be opened", msg);
		g_free(msg);
		g_free(proto);
		g_free(uri);
	}
	gtk_widget_destroy(d);
}

/* ---- media information ------------------------------------------------ */

typedef struct {
	GtkWidget *grid;
	GtkWidget *cover;
	int row;
} InfoWidgets;

static void info_section(InfoWidgets *iw, const char *title)
{
	GtkWidget *l = gtk_label_new(NULL);
	gchar *m = g_markup_printf_escaped("<b>%s</b>", title);
	gtk_label_set_markup(GTK_LABEL(l), m);
	g_free(m);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0);
	if (iw->row > 0)
		gtk_widget_set_margin_top(l, 10);
	gtk_grid_attach(GTK_GRID(iw->grid), l, 0, iw->row++, 2, 1);
}

static void info_row(InfoWidgets *iw, const char *key, const char *value)
{
	if (value == NULL || *value == '\0')
		return;
	GtkWidget *k = gtk_label_new(key);
	gtk_label_set_xalign(GTK_LABEL(k), 1.0);
	gtk_widget_set_valign(k, GTK_ALIGN_START);
	gtk_style_context_add_class(gtk_widget_get_style_context(k),
				    "mp-info-key");
	gchar *clean = mp_sanitize_text(value, 2000);
	GtkWidget *v = gtk_label_new(clean);
	g_free(clean);
	gtk_label_set_xalign(GTK_LABEL(v), 0.0);
	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 52);
	gtk_widget_set_hexpand(v, TRUE);
	gtk_widget_set_can_focus(v, FALSE);
	gtk_grid_attach(GTK_GRID(iw->grid), k, 0, iw->row, 1, 1);
	gtk_grid_attach(GTK_GRID(iw->grid), v, 1, iw->row, 1, 1);
	iw->row++;
}

static void info_tag_row(InfoWidgets *iw, const GstTagList *tl,
			 const char *key, const char *tag)
{
	gchar *s = NULL;
	if (tl != NULL && gst_tag_list_get_string(tl, tag, &s)) {
		info_row(iw, key, s);
		g_free(s);
	}
}

static gboolean on_info_cover_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
	MpApp *app = d;
	int aw = gtk_widget_get_allocated_width(w);
	int ah = gtk_widget_get_allocated_height(w);
	mp_cover_draw(cr, app->cover, 0, 0, aw, ah, 8,
		      mp_player_has_video(app->player)
			      ? "video-x-generic-symbolic"
			      : "audio-x-generic-symbolic",
		      w);
	return FALSE;
}

static void fill_info(MpApp *app, InfoWidgets *iw)
{
	const GstTagList *tl = mp_player_get_tags(app->player);
	const char *uri = mp_player_get_uri(app->player);

	if (uri == NULL || mp_player_get_state(app->player) ==
				   MP_PLAYER_STOPPED) {
		info_section(iw, "Nothing is playing");
		info_row(iw, "", "Information about the current entry "
				 "appears here while it plays.");
		return;
	}

	info_section(iw, "Tags");
	info_tag_row(iw, tl, "Title", GST_TAG_TITLE);
	info_tag_row(iw, tl, "Artist", GST_TAG_ARTIST);
	info_tag_row(iw, tl, "Album", GST_TAG_ALBUM);
	info_tag_row(iw, tl, "Album artist", GST_TAG_ALBUM_ARTIST);
	info_tag_row(iw, tl, "Composer", GST_TAG_COMPOSER);
	info_tag_row(iw, tl, "Genre", GST_TAG_GENRE);
	if (tl != NULL) {
		GstDateTime *dt = NULL;
		GDate *date = NULL;
		if (gst_tag_list_get_date_time(tl, GST_TAG_DATE_TIME, &dt)) {
			gchar *s = gst_date_time_to_iso8601_string(dt);
			info_row(iw, "Date", s);
			g_free(s);
			gst_date_time_unref(dt);
		} else if (gst_tag_list_get_date(tl, GST_TAG_DATE, &date)) {
			gchar buf[32];
			if (g_date_valid(date) &&
			    g_date_strftime(buf, sizeof(buf), "%Y-%m-%d",
					    date) > 0)
				info_row(iw, "Date", buf);
			g_date_free(date);
		}
		guint n = 0, total = 0;
		if (gst_tag_list_get_uint(tl, GST_TAG_TRACK_NUMBER, &n)) {
			gchar *s = gst_tag_list_get_uint(tl, GST_TAG_TRACK_COUNT,
							 &total) &&
						   total > 0
					   ? g_strdup_printf("%u of %u", n,
							     total)
					   : g_strdup_printf("%u", n);
			info_row(iw, "Track", s);
			g_free(s);
		}
		if (gst_tag_list_get_uint(tl, GST_TAG_ALBUM_VOLUME_NUMBER, &n)) {
			gchar *s = g_strdup_printf("%u", n);
			info_row(iw, "Disc", s);
			g_free(s);
		}
	}
	info_tag_row(iw, tl, "Organization", GST_TAG_ORGANIZATION);
	info_tag_row(iw, tl, "Comment", GST_TAG_COMMENT);
	info_tag_row(iw, tl, "Copyright", GST_TAG_COPYRIGHT);

	info_section(iw, "File");
	gchar *path = mp_uri_to_path(uri);
	gchar *where = path ? g_filename_display_name(path) : g_strdup(uri);
	info_row(iw, "Location", where);
	g_free(where);
	g_free(path);
	gint64 d = mp_player_get_duration(app->player);
	if (d > 0) {
		gchar *t = mp_format_time(d);
		info_row(iw, "Length", t);
		g_free(t);
	} else if (mp_player_is_live(app->player)) {
		info_row(iw, "Length", "Live stream");
	}

	MpStreamInfo si;
	mp_player_get_stream_info(app->player, &si);
	info_row(iw, "Container", si.container);
	if (si.bitrate > 0) {
		gchar *b = g_strdup_printf("%u kbit/s", si.bitrate / 1000);
		info_row(iw, "Bit rate", b);
		g_free(b);
	}

	if (si.width > 0 || si.video_codec != NULL) {
		info_section(iw, "Video");
		info_row(iw, "Codec", si.video_codec);
		if (si.width > 0) {
			gchar *r = g_strdup_printf("%d \u00d7 %d", si.width,
						   si.height);
			info_row(iw, "Size", r);
			g_free(r);
		}
		if (si.fps_n > 0 && si.fps_d > 0) {
			gchar *f = g_strdup_printf("%.3g frames per second",
						   (double)si.fps_n / si.fps_d);
			info_row(iw, "Frame rate", f);
			g_free(f);
		}
		if (si.par_n > 0 && si.par_d > 0 && si.par_n != si.par_d) {
			gchar *p = g_strdup_printf("%d:%d", si.par_n,
						   si.par_d);
			info_row(iw, "Pixel aspect", p);
			g_free(p);
		}
		info_row(iw, "Shown with", si.video_sink);
	}

	if (si.audio_rate > 0 || si.audio_codec != NULL) {
		info_section(iw, "Audio");
		info_row(iw, "Codec", si.audio_codec);
		if (si.audio_rate > 0) {
			gchar *r = g_strdup_printf("%d Hz", si.audio_rate);
			info_row(iw, "Sample rate", r);
			g_free(r);
		}
		if (si.audio_channels > 0) {
			gchar *c = g_strdup_printf(
				"%d%s", si.audio_channels,
				si.audio_channels == 1	 ? " (mono)"
				: si.audio_channels == 2 ? " (stereo)"
							 : "");
			info_row(iw, "Channels", c);
			g_free(c);
		}
		info_row(iw, "Played through", si.audio_sink);
	}

	if (si.subtitle_codec != NULL) {
		info_section(iw, "Subtitles");
		info_row(iw, "Format", si.subtitle_codec);
	}
	if (app->cover_source != NULL) {
		info_section(iw, "Cover art");
		info_row(iw, "From", app->cover_source);
	}
	mp_stream_info_clear(&si);
}

void mp_dialog_media_info_refresh(MpApp *app)
{
	if (app->info_dialog == NULL)
		return;
	InfoWidgets *iw = g_object_get_data(G_OBJECT(app->info_dialog),
					    "mp-info");
	if (iw == NULL)
		return;
	GList *kids = gtk_container_get_children(GTK_CONTAINER(iw->grid));
	for (GList *l = kids; l != NULL; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(kids);
	iw->row = 0;
	fill_info(app, iw);
	gtk_widget_show_all(iw->grid);
	gtk_widget_queue_draw(iw->cover);
}

static void on_info_destroy(GtkWidget *w, gpointer data)
{
	MpApp *app = data;
	if (app->info_dialog == w)
		app->info_dialog = NULL;
}

void mp_dialog_media_info(MpApp *app)
{
	if (app->info_dialog != NULL) {
		gtk_window_present(GTK_WINDOW(app->info_dialog));
		return;
	}
	if (app->fullscreen)
		mp_app_set_fullscreen(app, FALSE);

	GtkWidget *d = gtk_dialog_new_with_buttons(
		"Media Information", GTK_WINDOW(app->window),
		GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE,
		NULL);
	gtk_window_set_default_size(GTK_WINDOW(d), 640, 460);
	g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);

	InfoWidgets *iw = g_new0(InfoWidgets, 1);
	g_object_set_data_full(G_OBJECT(d), "mp-info", iw, g_free);

	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 14);
	iw->cover = gtk_drawing_area_new();
	gtk_widget_set_size_request(iw->cover, 160, 160);
	gtk_widget_set_valign(iw->cover, GTK_ALIGN_START);
	g_signal_connect(iw->cover, "draw", G_CALLBACK(on_info_cover_draw),
			 app);
	iw->grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(iw->grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(iw->grid), 12);
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand(sw, TRUE);
	gtk_widget_set_vexpand(sw, TRUE);
	gtk_container_add(GTK_CONTAINER(sw), iw->grid);
	gtk_box_pack_start(GTK_BOX(hbox), iw->cover, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), sw, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			   hbox, TRUE, TRUE, 0);

	app->info_dialog = d;
	g_signal_connect(d, "destroy", G_CALLBACK(on_info_destroy), app);
	fill_info(app, iw);
	gtk_widget_show_all(d);
}

/* ---- keyboard shortcuts ----------------------------------------------- */

void mp_dialog_shortcuts(MpApp *app)
{
	static const char *const keys[][2] = {
		{ NULL, "Playback" },
		{ "Space", "Play or pause" },
		{ "N  /  P", "Next or previous entry" },
		{ "\u2190  /  \u2192", "Back or forward 5 seconds" },
		{ "Shift+\u2190  /  Shift+\u2192", "Back or forward 1 minute" },
		{ "Home", "Back to the beginning" },
		{ "0 \u2026 9", "Jump to 0 % \u2026 90 % of the entry" },
		{ "[  /  ]", "Slower or faster" },
		{ "Backspace", "Normal speed" },
		{ NULL, "Sound" },
		{ "\u2191  /  \u2193", "Volume up or down" },
		{ "M", "Mute or unmute" },
		{ NULL, "Queue" },
		{ "S", "Shuffle on or off" },
		{ "R", "Repeat: off, all, this one" },
		{ "Delete", "Remove the selected entries" },
		{ "F9", "Show or hide the play queue" },
		{ NULL, "View and files" },
		{ "F  /  F11", "Full screen" },
		{ "Esc", "Leave full screen" },
		{ "Double-click", "Full screen on or off" },
		{ "T", "Total or remaining time" },
		{ "I  /  Ctrl+I", "Media information" },
		{ "Ctrl+O", "Open files" },
		{ "Ctrl+Shift+O", "Open a folder" },
		{ "Ctrl+L", "Open a location" },
		{ "Ctrl+S", "Save the queue as a playlist" },
		{ "Ctrl+Q", "Quit" },
	};

	GtkWidget *d = gtk_dialog_new_with_buttons(
		"Keyboard Shortcuts", GTK_WINDOW(app->window),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Close",
		GTK_RESPONSE_CLOSE, NULL);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
	int row = 0;
	for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
		if (keys[i][0] == NULL) {
			GtkWidget *h = gtk_label_new(NULL);
			gchar *m = g_markup_printf_escaped("<b>%s</b>",
							   keys[i][1]);
			gtk_label_set_markup(GTK_LABEL(h), m);
			g_free(m);
			gtk_label_set_xalign(GTK_LABEL(h), 0.0);
			if (row > 0)
				gtk_widget_set_margin_top(h, 8);
			gtk_grid_attach(GTK_GRID(grid), h, 0, row++, 2, 1);
			continue;
		}
		GtkWidget *k = gtk_label_new(keys[i][0]);
		gtk_style_context_add_class(gtk_widget_get_style_context(k),
					    "mp-shortcut-key");
		gtk_widget_set_halign(k, GTK_ALIGN_END);
		GtkWidget *v = gtk_label_new(keys[i][1]);
		gtk_label_set_xalign(GTK_LABEL(v), 0.0);
		gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
		row++;
	}
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw),
						   560);
	gtk_container_add(GTK_CONTAINER(sw), grid);
	gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),
			   sw, TRUE, TRUE, 0);
	gtk_widget_show_all(d);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/* ---- about ------------------------------------------------------------ */

void mp_dialog_about(MpApp *app)
{
	static const char *const authors[] = { "The LikeOS Project", NULL };
	gchar *comments = g_strdup_printf(
		"Plays music and videos with GStreamer %u.%u.\n"
		"Video output: %s",
		GST_VERSION_MAJOR, GST_VERSION_MINOR,
		mp_player_get_video_sink_name(app->player));
	gtk_show_about_dialog(GTK_WINDOW(app->window), "program-name",
			      MP_APP_NAME, "version", MP_VERSION, "comments",
			      comments, "copyright",
			      "Copyright \u00a9 2026 The LikeOS Project",
			      "license-type", GTK_LICENSE_GPL_2_0, "authors",
			      authors, "logo-icon-name", MP_ICON_NAME, NULL);
	g_free(comments);
}
