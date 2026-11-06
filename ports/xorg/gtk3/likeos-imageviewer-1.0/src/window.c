// LikeOS -- LikeOS Image Viewer: the window.
//
// The window draws its own title row, as LikeOS Explorer does: the window
// manager is asked for the border alone (the Motif decoration hint), the
// empty part of the row moves the window (a drag through the window
// manager's move request; a double click does nothing, the row being full
// of controls), a right click opens the window menu, and the three caption buttons sit inset at the
// right, 36 by 26 pixels with rounded corners -- here in the dark palette
// the viewer is drawn in.  There are two rows: the viewer's (the picture's
// name and place in the folder, and its commands) and the editor's (the
// way back and what is being edited).
//
// Below is a stack of the two modes.  In full screen the title row goes
// and the viewer's floating chrome hides itself when the pointer rests.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>

#include "canvas.h"
#include "editor.h"

#define IV_CHROME_HIDE_MS 2500

/* ---- small things ------------------------------------------------------------- */

GtkWindow *iv_window_gtk(IvWindow *w)
{
	return GTK_WINDOW(w->window);
}

GtkOverlay *iv_window_overlay(IvWindow *w)
{
	return GTK_OVERLAY(w->view_overlay);
}

void iv_window_toast(IvWindow *w, const char *text)
{
	if (w->editing)
		iv_editor_toast(w->editor, text);
	else
		iv_toast_show(w->toast, text, NULL, NULL, NULL, NULL, 0);
}

void iv_window_error(IvWindow *w, const char *what, const GError *e)
{
	char *msg = e && e->message ? g_strdup_printf("%s: %s", what, e->message) :
				      g_strdup(what);

	iv_window_toast(w, msg);
	g_free(msg);
}

void iv_window_set_title_text(IvWindow *w)
{
	int n = iv_folder_count(w->folder);
	char *title;

	if (w->path) {
		char *name = g_filename_display_basename(w->path);

		gtk_label_set_text(GTK_LABEL(w->title_name), name);
		title = g_strdup_printf("%s – %s", name, IV_APP_NAME);
		g_free(name);
	} else {
		gtk_label_set_text(GTK_LABEL(w->title_name), IV_APP_NAME);
		title = g_strdup(IV_APP_NAME);
	}
	if (n > 1 && w->index >= 0) {
		char *c = g_strdup_printf("%d of %d", w->index + 1, n);

		gtk_label_set_text(GTK_LABEL(w->title_count), c);
		g_free(c);
		gtk_widget_show(w->title_count);
	} else {
		gtk_widget_hide(w->title_count);
	}
	gtk_window_set_title(GTK_WINDOW(w->window), title);
	g_free(title);
	if (w->editing)
		gtk_label_set_text(GTK_LABEL(w->edit_title),
				   iv_editor_title(w->editor));
}

void iv_window_update_commands(IvWindow *w)
{
	gboolean shown = w->pic && !w->pic->error;
	gboolean have = w->path != NULL;

	gtk_widget_set_sensitive(w->btn_edit, shown);
	gtk_widget_set_sensitive(w->btn_rotate, shown && iv_picture_writable(w->pic));
	gtk_widget_set_sensitive(w->btn_delete, have);
	gtk_widget_set_sensitive(w->btn_open_with, have);
	gtk_widget_set_sensitive(w->btn_more, TRUE);
	iv_strip_update(w->strip);
}

/* ---- full screen and the chrome that hides in it ------------------------------ */

static void update_insets(IvWindow *w)
{
	GtkWidget *chrome = w->chrome_box;
	int strips = gtk_widget_get_mapped(chrome) ?
			     gtk_widget_get_allocated_height(chrome) :
			     0;

	/* The picture fits above the floating strips -- unless they hide
	 * themselves (full screen), in which case it uses the whole
	 * screen and they float over it.  Toasts always sit above them. */
	iv_canvas_set_insets(w->canvas, 0, 0, w->fullscreen ? 0 : strips, 0);
	iv_toast_set_margin(w->toast, MAX(strips, 8) + 12);
}

/* The strips' height decides the canvas's insets -- set once the
 * allocation is over, never during it. */
static gboolean insets_later(gpointer data)
{
	IvWindow *w = data;

	w->insets_idle = 0;
	update_insets(w);
	return G_SOURCE_REMOVE;
}

static void chrome_allocated(GtkWidget *wid, GdkRectangle *a, IvWindow *w)
{
	if (!w->insets_idle)
		w->insets_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 15,
						 insets_later, w, NULL);
}

static void set_chrome_hidden(IvWindow *w, gboolean hidden)
{
	GdkWindow *gw = gtk_widget_get_window(IV_CANVAS_GTK(w->canvas));

	if (w->chrome_hidden == hidden)
		return;
	w->chrome_hidden = hidden;
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->strip_revealer), !hidden);
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->film_revealer),
				      !hidden && w->film_visible);
	/* The pointer goes with the chrome. */
	if (gw) {
		GdkCursor *c = hidden ? gdk_cursor_new_from_name(
						gdk_window_get_display(gw), "none") :
					NULL;

		gdk_window_set_cursor(gw, c);
		g_clear_object(&c);
	}
}

static gboolean chrome_timeout(gpointer data)
{
	IvWindow *w = data;

	w->chrome_timer = 0;
	if (w->fullscreen && !w->editing)
		set_chrome_hidden(w, TRUE);
	return G_SOURCE_REMOVE;
}

void iv_window_show_chrome(IvWindow *w)
{
	set_chrome_hidden(w, FALSE);
	if (w->chrome_timer)
		g_source_remove(w->chrome_timer);
	w->chrome_timer = 0;
	if (w->fullscreen && !w->editing)
		w->chrome_timer = g_timeout_add(w->slideshow ? 1200 : IV_CHROME_HIDE_MS,
						chrome_timeout, w);
}

static gboolean view_motion(GtkWidget *wid, GdkEventMotion *ev, IvWindow *w)
{
	/* Real movement only: the hidden cursor's own events are not. */
	if (fabs(ev->x_root - w->last_motion_x) < 2 &&
	    fabs(ev->y_root - w->last_motion_y) < 2)
		return FALSE;
	w->last_motion_x = ev->x_root;
	w->last_motion_y = ev->y_root;
	if (w->fullscreen)
		iv_window_show_chrome(w);
	return FALSE;
}

void iv_window_set_fullscreen(IvWindow *w, gboolean on)
{
	if (on)
		gtk_window_fullscreen(GTK_WINDOW(w->window));
	else
		gtk_window_unfullscreen(GTK_WINDOW(w->window));
}

void iv_window_toggle_fullscreen(IvWindow *w)
{
	iv_window_set_fullscreen(w, !w->fullscreen);
}

/* ---- modes --------------------------------------------------------------------- */

void iv_window_enter_editor(IvWindow *w)
{
	if (w->editing || !w->pic || w->pic->error)
		return;
	if (w->slideshow)
		iv_viewer_slideshow(w, FALSE);
	iv_viewer_flush_rotation(w);
	if (!w->editor) {
		w->editor = iv_editor_new(w);
		gtk_stack_add_named(GTK_STACK(w->body_stack),
				    iv_editor_widget(w->editor), "editor");
	}
	w->editing = TRUE;
	set_chrome_hidden(w, FALSE);
	if (w->chrome_timer) {
		g_source_remove(w->chrome_timer);
		w->chrome_timer = 0;
	}
	iv_canvas_set_playing(w->canvas, FALSE);
	iv_editor_open(w->editor, w->pic);
	gtk_label_set_text(GTK_LABEL(w->edit_title), iv_editor_title(w->editor));
	gtk_stack_set_visible_child_name(GTK_STACK(w->title_stack), "editor");
	gtk_stack_set_visible_child_name(GTK_STACK(w->body_stack), "editor");
	/* The editor wants its title row even in full screen. */
	gtk_widget_show(w->title_stack);
}

void iv_window_leave_editor(IvWindow *w)
{
	if (!w->editing)
		return;
	w->editing = FALSE;
	gtk_stack_set_visible_child_name(GTK_STACK(w->title_stack), "viewer");
	gtk_stack_set_visible_child_name(GTK_STACK(w->body_stack), "viewer");
	if (w->fullscreen)
		gtk_widget_hide(w->title_stack);
	iv_canvas_set_playing(w->canvas, TRUE);
	gtk_widget_grab_focus(IV_CANVAS_GTK(w->canvas));
	iv_window_set_title_text(w);
	iv_window_show_chrome(w);
}

static void leave_done(gboolean go, gpointer data)
{
	IvWindow *w = data;

	if (go)
		iv_window_leave_editor(w);
}

static void back_clicked(GtkButton *b, IvWindow *w)
{
	iv_editor_request_leave(w->editor, leave_done, w);
}

static void edit_clicked(GtkButton *b, IvWindow *w)
{
	iv_window_enter_editor(w);
}

static void rotate_clicked(GtkButton *b, IvWindow *w)
{
	GdkEvent *ev = gtk_get_current_event();
	GdkModifierType mods = 0;

	if (ev) {
		gdk_event_get_state(ev, &mods);
		gdk_event_free(ev);
	}
	iv_viewer_rotate(w, (mods & GDK_SHIFT_MASK) ? -1 : 1);
}

static void delete_clicked(GtkButton *b, IvWindow *w)
{
	iv_viewer_delete(w);
}

static void open_with_clicked(GtkButton *b, IvWindow *w)
{
	iv_menu_open_with(w, GTK_WIDGET(b));
}

static void more_clicked(GtkButton *b, IvWindow *w)
{
	iv_menu_more(w, GTK_WIDGET(b));
}

void iv_window_settings_changed(IvWindow *w)
{
	IvSettings *s = iv_settings_get();

	iv_canvas_set_backdrop(w->canvas, s->backdrop);
	if (w->editor)
		iv_canvas_set_backdrop(iv_editor_canvas(w->editor), s->backdrop);
	iv_strip_update(w->strip);
}

/* ---- the title row ------------------------------------------------------------- */

static void toggle_maximized(IvWindow *w)
{
	if (w->maximized)
		gtk_window_unmaximize(GTK_WINDOW(w->window));
	else
		gtk_window_maximize(GTK_WINDOW(w->window));
}

static void minimize_clicked(GtkButton *b, IvWindow *w)
{
	gtk_window_iconify(GTK_WINDOW(w->window));
}

static void maximize_clicked(GtkButton *b, IvWindow *w)
{
	toggle_maximized(w);
}

static void close_clicked(GtkButton *b, IvWindow *w)
{
	gtk_window_close(GTK_WINDOW(w->window));
}

static void menu_restore(IvWindow *w)
{
	gtk_window_unmaximize(GTK_WINDOW(w->window));
}

static void menu_minimize(IvWindow *w)
{
	gtk_window_iconify(GTK_WINDOW(w->window));
}

static void menu_maximize(IvWindow *w)
{
	gtk_window_maximize(GTK_WINDOW(w->window));
}

static void menu_close(IvWindow *w)
{
	gtk_window_close(GTK_WINDOW(w->window));
}

/* The window menu a right click on the title row opens, as on the title
 * bar of any window. */
static void window_menu(IvWindow *w, GdkEventButton *ev)
{
	GtkWidget *m = iv_menu_new();
	GtkWidget *mi;

	mi = iv_menu_item(m, IV_GLYPH_RESTORE, "_Restore", NULL,
			  G_CALLBACK(menu_restore), w);
	gtk_widget_set_sensitive(mi, w->maximized);
	iv_menu_item(m, IV_GLYPH_MINIMIZE, "Mi_nimize", NULL,
		     G_CALLBACK(menu_minimize), w);
	mi = iv_menu_item(m, IV_GLYPH_MAXIMIZE, "Ma_ximize", NULL,
			  G_CALLBACK(menu_maximize), w);
	gtk_widget_set_sensitive(mi, !w->maximized);
	iv_menu_separator(m);
	iv_menu_item(m, IV_GLYPH_CLOSE, "_Close", "Alt+F4",
		     G_CALLBACK(menu_close), w);
	iv_menu_popup_at_pointer(m, w->window, (GdkEvent *)ev);
}

/* The empty part of the title row is the window's handle: dragging it
 * moves the window once the pointer has moved far enough to be a drag.  A
 * double click does nothing here: the row is full of controls, and two quick
 * clicks on one of them (zooming in twice, say) arrive as a double click on
 * the row too -- they must not maximise or restore the window under them. */
static gboolean title_press(GtkWidget *wid, GdkEventButton *ev, IvWindow *w)
{
	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (ev->button == 3) {
		window_menu(w, ev);
		return TRUE;
	}
	if (ev->button == 1) {
		w->press_pending = TRUE;
		w->press_x = ev->x_root;
		w->press_y = ev->y_root;
		return TRUE;
	}
	return FALSE;
}

static gboolean title_motion(GtkWidget *wid, GdkEventMotion *ev, IvWindow *w)
{
	if (!w->press_pending || !(ev->state & GDK_BUTTON1_MASK))
		return FALSE;
	if (!gtk_drag_check_threshold(wid, (int)w->press_x, (int)w->press_y,
				      (int)ev->x_root, (int)ev->y_root))
		return TRUE;
	w->press_pending = FALSE;
	gtk_window_begin_move_drag(GTK_WINDOW(w->window), 1, (int)w->press_x,
				   (int)w->press_y, ev->time);
	return TRUE;
}

static gboolean title_release(GtkWidget *wid, GdkEventButton *ev, IvWindow *w)
{
	if (ev->button == 1)
		w->press_pending = FALSE;
	return FALSE;
}

/* The caption buttons: inset rounded buttons 36 by 26 pixels, 4 apart and
 * 6 from the right edge, centred in the row (sizes and colours in the
 * theme). */
static GtkWidget *caption_button(IvGlyph g, const char *tip, const char *klass,
				 GCallback cb, IvWindow *w, GtkWidget **glyph_out)
{
	GtkWidget *b = iv_glyph_button(g, 10, tip, "iv-caption");

	iv_add_class(b, klass);
	g_signal_connect(b, "clicked", cb, w);
	if (glyph_out)
		*glyph_out = g_object_get_data(G_OBJECT(b), "iv-glyph-widget");
	return b;
}

static void add_captions(IvWindow *w, GtkWidget *row, int which)
{
	gtk_box_pack_end(GTK_BOX(row),
			 caption_button(IV_GLYPH_CLOSE, "Close", "close",
					G_CALLBACK(close_clicked), w, NULL),
			 FALSE, FALSE, 0);
	w->max_btn[which] = caption_button(IV_GLYPH_MAXIMIZE, "Maximize",
					   "maximize", G_CALLBACK(maximize_clicked),
					   w, &w->max_glyph[which]);
	gtk_box_pack_end(GTK_BOX(row), w->max_btn[which], FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row),
			 caption_button(IV_GLYPH_MINIMIZE, "Minimize", "minimize",
					G_CALLBACK(minimize_clicked), w, NULL),
			 FALSE, FALSE, 0);
}

static GtkWidget *title_box(IvWindow *w, GtkWidget **row_out)
{
	GtkWidget *eb = gtk_event_box_new();
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

	iv_add_class(row, "iv-titlebar");
	gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
					  GDK_BUTTON_MOTION_MASK);
	g_signal_connect(eb, "button-press-event", G_CALLBACK(title_press), w);
	g_signal_connect(eb, "motion-notify-event", G_CALLBACK(title_motion), w);
	g_signal_connect(eb, "button-release-event", G_CALLBACK(title_release), w);
	gtk_container_add(GTK_CONTAINER(eb), row);
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
	*row_out = row;
	return eb;
}

static GtkWidget *build_view_title(IvWindow *w)
{
	GtkWidget *row;
	GtkWidget *eb = title_box(w, &row);
	GtkWidget *names = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *grip = gtk_label_new(NULL);
	GtkWidget *cmds = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

	w->title_icon = gtk_image_new_from_icon_name(IV_ICON_NAME,
						     GTK_ICON_SIZE_MENU);
	gtk_image_set_pixel_size(GTK_IMAGE(w->title_icon), 16);
	iv_add_class(w->title_icon, "iv-title-icon");
	gtk_box_pack_start(GTK_BOX(row), w->title_icon, FALSE, FALSE, 0);

	w->title_name = gtk_label_new(IV_APP_NAME);
	gtk_label_set_ellipsize(GTK_LABEL(w->title_name), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(w->title_name), 48);
	gtk_label_set_xalign(GTK_LABEL(w->title_name), 0);
	iv_add_class(w->title_name, "iv-title-name");
	w->title_count = gtk_label_new(NULL);
	iv_add_class(w->title_count, "iv-title-count");
	gtk_widget_set_no_show_all(w->title_count, TRUE);
	gtk_box_pack_start(GTK_BOX(names), w->title_name, FALSE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(names), w->title_count, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), names, FALSE, TRUE, 0);

	/* Room to grab the window by. */
	gtk_widget_set_size_request(grip, 32, -1);
	gtk_box_pack_start(GTK_BOX(row), grip, TRUE, TRUE, 0);

	w->btn_edit = iv_text_button(IV_GLYPH_EDIT, "Edit", FALSE,
				     "iv-edit-button");
	gtk_widget_set_tooltip_text(w->btn_edit, "Edit the picture (E)");
	g_signal_connect(w->btn_edit, "clicked", G_CALLBACK(edit_clicked), w);
	w->btn_rotate = iv_glyph_button(IV_GLYPH_ROTATE_RIGHT, 16,
					"Rotate right (R) · Shift: left",
					"iv-title-button");
	g_signal_connect(w->btn_rotate, "clicked", G_CALLBACK(rotate_clicked), w);
	w->btn_delete = iv_glyph_button(IV_GLYPH_DELETE, 16,
					"Move to Trash (Delete)",
					"iv-title-button");
	g_signal_connect(w->btn_delete, "clicked", G_CALLBACK(delete_clicked), w);
	w->btn_open_with = iv_text_button(IV_GLYPH_NONE, "Open with", TRUE,
					  "iv-title-button");
	gtk_widget_set_tooltip_text(w->btn_open_with, "Open in another app");
	g_signal_connect(w->btn_open_with, "clicked",
			 G_CALLBACK(open_with_clicked), w);
	w->btn_more = iv_glyph_button(IV_GLYPH_MORE, 16, "More", "iv-title-button");
	g_signal_connect(w->btn_more, "clicked", G_CALLBACK(more_clicked), w);
	gtk_box_pack_start(GTK_BOX(cmds), w->btn_edit, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(cmds), w->btn_rotate, FALSE, FALSE, 2);
	gtk_box_pack_start(GTK_BOX(cmds), w->btn_delete, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(cmds), w->btn_open_with, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(cmds), w->btn_more, FALSE, FALSE, 0);
	iv_add_class(cmds, "iv-title-commands");

	add_captions(w, row, 0);
	gtk_box_pack_end(GTK_BOX(row), cmds, FALSE, FALSE, 0);
	w->title_rows[0] = row;
	return eb;
}

static GtkWidget *build_edit_title(IvWindow *w)
{
	GtkWidget *row;
	GtkWidget *eb = title_box(w, &row);
	GtkWidget *back = iv_glyph_button(IV_GLYPH_BACK, 16, "Back to the viewer (Esc)",
					  "iv-title-button");
	GtkWidget *grip = gtk_label_new(NULL);

	iv_add_class(back, "iv-back");
	g_signal_connect(back, "clicked", G_CALLBACK(back_clicked), w);
	gtk_box_pack_start(GTK_BOX(row), back, FALSE, FALSE, 0);
	w->edit_title = gtk_label_new("Editing");
	gtk_label_set_ellipsize(GTK_LABEL(w->edit_title), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign(GTK_LABEL(w->edit_title), 0);
	iv_add_class(w->edit_title, "iv-title-name");
	gtk_box_pack_start(GTK_BOX(row), w->edit_title, FALSE, TRUE, 4);
	gtk_box_pack_start(GTK_BOX(row), grip, TRUE, TRUE, 0);
	add_captions(w, row, 1);
	w->title_rows[1] = row;
	return eb;
}

/* ---- window events ------------------------------------------------------------- */

static gboolean window_state(GtkWidget *wid, GdkEventWindowState *ev,
			     IvWindow *w)
{
	if (ev->changed_mask & GDK_WINDOW_STATE_MAXIMIZED)
		w->maximized = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
	if (ev->changed_mask & (GDK_WINDOW_STATE_MAXIMIZED |
				GDK_WINDOW_STATE_FULLSCREEN)) {
		gboolean big = (ev->new_window_state &
				(GDK_WINDOW_STATE_MAXIMIZED |
				 GDK_WINDOW_STATE_FULLSCREEN)) != 0;
		const char *tip = big ? "Restore Down" : "Maximize";

		for (int i = 0; i < 2; i++) {
			iv_glyph_set(w->max_glyph[i],
				     big ? IV_GLYPH_RESTORE : IV_GLYPH_MAXIMIZE);
			gtk_widget_set_tooltip_text(w->max_btn[i], tip);
		}
	}
	if (ev->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) {
		w->fullscreen = (ev->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
		iv_set_class(w->window, "iv-fullscreen", w->fullscreen);
		if (w->fullscreen && !w->editing) {
			gtk_widget_hide(w->title_stack);
			iv_window_show_chrome(w);
		} else {
			gtk_widget_show(w->title_stack);
			set_chrome_hidden(w, FALSE);
			if (w->chrome_timer) {
				g_source_remove(w->chrome_timer);
				w->chrome_timer = 0;
			}
			if (w->slideshow)
				iv_viewer_slideshow(w, FALSE);
		}
		update_insets(w);
		iv_strip_update(w->strip);
	}
	return FALSE;
}

static gboolean key_press(GtkWidget *wid, GdkEventKey *ev, IvWindow *w)
{
	GtkOverlay *ov = w->editing ? NULL : GTK_OVERLAY(w->view_overlay);

	/* A confirm card answers its own keys. */
	if ((ov && iv_confirm_is_open(ov)))
		return FALSE;
	if (w->editing) {
		if (iv_editor_key(w->editor, ev))
			return TRUE;
		if (ev->keyval == GDK_KEY_Escape) {
			iv_editor_request_leave(w->editor, leave_done, w);
			return TRUE;
		}
		return FALSE;
	}
	if (w->fullscreen)
		iv_window_show_chrome(w);
	return iv_viewer_key(w, ev);
}

static gboolean key_release(GtkWidget *wid, GdkEventKey *ev, IvWindow *w)
{
	if (w->editing)
		return iv_editor_key_release(w->editor, ev);
	return FALSE;
}

static void close_after_leave(gboolean go, gpointer data)
{
	IvWindow *w = data;

	if (go)
		gtk_widget_destroy(w->window);
}

static gboolean delete_event(GtkWidget *wid, GdkEvent *ev, IvWindow *w)
{
	IvSettings *s = iv_settings_get();

	if (!w->fullscreen) {
		s->maximized = w->maximized;
		if (!w->maximized)
			gtk_window_get_size(GTK_WINDOW(w->window), &s->width,
					    &s->height);
		iv_settings_save();
	}
	if (w->editing && w->editor && iv_editor_dirty(w->editor)) {
		iv_editor_request_leave(w->editor, close_after_leave, w);
		return TRUE;
	}
	/* The window goes now: a turn not written yet is written first. */
	iv_viewer_flush_rotation_now(w);
	return FALSE;
}

static void destroyed(GtkWidget *wid, IvWindow *w)
{
	gtk_main_quit();
}

/* The window draws its own title row: the window manager is asked for the
 * border alone (it keeps the frame, its corners and its resize edges). */
static void realized(GtkWidget *wid, IvWindow *w)
{
	gdk_window_set_decorations(gtk_widget_get_window(wid), GDK_DECOR_BORDER);
}

/* ---- building -------------------------------------------------------------------- */

static GtkWidget *build_viewer(IvWindow *w)
{
	GtkWidget *film_card;

	w->view_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	w->view_overlay = gtk_overlay_new();
	w->canvas = iv_canvas_new();
	gtk_container_add(GTK_CONTAINER(w->view_overlay), IV_CANVAS_GTK(w->canvas));

	/* The floating chrome at the bottom: the filmstrip over the strip. */
	w->chrome_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign(w->chrome_box, GTK_ALIGN_END);
	gtk_widget_set_halign(w->chrome_box, GTK_ALIGN_FILL);
	iv_add_class(w->chrome_box, "iv-chrome");

	w->film = iv_filmstrip_new(w);
	w->film_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(w->film_revealer),
					 GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_revealer_set_transition_duration(GTK_REVEALER(w->film_revealer), 160);
	film_card = iv_filmstrip_widget(w->film);
	gtk_container_add(GTK_CONTAINER(w->film_revealer), film_card);
	gtk_box_pack_start(GTK_BOX(w->chrome_box), w->film_revealer, FALSE, FALSE, 0);

	w->strip = iv_strip_new(w);
	w->strip_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(w->strip_revealer),
					 GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
	gtk_revealer_set_transition_duration(GTK_REVEALER(w->strip_revealer), 200);
	gtk_container_add(GTK_CONTAINER(w->strip_revealer), iv_strip_widget(w->strip));
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->strip_revealer), TRUE);
	gtk_box_pack_start(GTK_BOX(w->chrome_box), w->strip_revealer, FALSE, FALSE, 0);
	g_signal_connect(w->chrome_box, "size-allocate",
			 G_CALLBACK(chrome_allocated), w);

	gtk_overlay_add_overlay(GTK_OVERLAY(w->view_overlay), w->chrome_box);
	w->toast = iv_toast_new(GTK_OVERLAY(w->view_overlay));
	gtk_widget_add_events(w->view_overlay, GDK_POINTER_MOTION_MASK);
	g_signal_connect(w->view_overlay, "motion-notify-event",
			 G_CALLBACK(view_motion), w);
	gtk_box_pack_start(GTK_BOX(w->view_box), w->view_overlay, TRUE, TRUE, 0);

	w->info = iv_info_new(w);
	w->info_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(w->info_revealer),
					 GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
	gtk_revealer_set_transition_duration(GTK_REVEALER(w->info_revealer), 180);
	gtk_container_add(GTK_CONTAINER(w->info_revealer), iv_info_widget(w->info));
	gtk_widget_set_hexpand(w->info_revealer, FALSE);
	gtk_box_pack_start(GTK_BOX(w->view_box), w->info_revealer, FALSE, FALSE, 0);
	return w->view_box;
}

IvWindow *iv_window_new(IvOptions *opt)
{
	IvWindow *w = g_new0(IvWindow, 1);
	IvSettings *s = iv_settings_get();
	GtkWidget *box;

	w->opt = opt;
	w->index = -1;
	w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	iv_add_class(w->window, IV_CLASS_WINDOW);
	gtk_window_set_icon_name(GTK_WINDOW(w->window), IV_ICON_NAME);
	gtk_window_set_title(GTK_WINDOW(w->window), IV_APP_NAME);
	gtk_window_set_default_size(GTK_WINDOW(w->window), s->width, s->height);
	gtk_widget_set_size_request(w->window, 560, 400);
	if (s->maximized)
		gtk_window_maximize(GTK_WINDOW(w->window));
	g_signal_connect(w->window, "realize", G_CALLBACK(realized), w);
	g_signal_connect(w->window, "window-state-event", G_CALLBACK(window_state), w);
	g_signal_connect(w->window, "key-press-event", G_CALLBACK(key_press), w);
	g_signal_connect(w->window, "key-release-event", G_CALLBACK(key_release), w);
	g_signal_connect(w->window, "delete-event", G_CALLBACK(delete_event), w);
	g_signal_connect(w->window, "destroy", G_CALLBACK(destroyed), w);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	w->title_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(w->title_stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(w->title_stack), 120);
	gtk_stack_add_named(GTK_STACK(w->title_stack), build_view_title(w), "viewer");
	gtk_stack_add_named(GTK_STACK(w->title_stack), build_edit_title(w), "editor");
	gtk_box_pack_start(GTK_BOX(box), w->title_stack, FALSE, FALSE, 0);

	w->body_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(w->body_stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration(GTK_STACK(w->body_stack), 140);
	gtk_stack_add_named(GTK_STACK(w->body_stack), build_viewer(w), "viewer");
	gtk_box_pack_start(GTK_BOX(box), w->body_stack, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(w->window), box);
	gtk_widget_show_all(box);
	gtk_stack_set_visible_child_name(GTK_STACK(w->title_stack), "viewer");

	iv_viewer_init(w);
	w->film_visible = s->show_filmstrip;
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->film_revealer), w->film_visible);
	iv_viewer_set_info(w, s->show_info);
	iv_window_settings_changed(w);
	iv_window_update_commands(w);
	gtk_widget_grab_focus(IV_CANVAS_GTK(w->canvas));
	return w;
}

void iv_window_destroy(IvWindow *w)
{
	if (!w)
		return;
	if (w->chrome_timer)
		g_source_remove(w->chrome_timer);
	if (w->insets_idle)
		g_source_remove(w->insets_idle);
	if (w->slideshow_timer)
		g_source_remove(w->slideshow_timer);
	if (w->rotate_timer)
		g_source_remove(w->rotate_timer);
	if (w->prefetch_source)
		g_source_remove(w->prefetch_source);
	if (w->editor)
		iv_editor_free(w->editor);
	iv_toast_free(w->toast);
	iv_strip_free(w->strip);
	iv_filmstrip_free(w->film);
	iv_info_free(w->info);
	iv_cache_free(w->cache);
	iv_folder_free(w->folder);
	if (w->pic)
		iv_picture_unref(w->pic);
	iv_trash_record_free(w->trashed);
	g_free(w->path);
	g_free(w->pending_rotate_path);
	g_free(w->self_written);
	g_free(w);
}
