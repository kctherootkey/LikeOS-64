// LikeOS -- LikeOS Image Viewer: the viewer.
//
// Going through the folder, picture by picture: the current one is asked
// of the cache first, its neighbours after it, so the next picture is
// usually decoded before it is asked for.  Turning a picture shows the
// turn at once and writes it to the file a moment later, so pressing
// Rotate three times writes once; moving a picture to the Trash shows the
// next one and offers Undo for as long as the toast is up.  The slideshow
// steps through the folder in full screen with a cross-fade.
//
// Copyright (C) 2026 The LikeOS Project

#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "canvas.h"
#include "editor.h"

#define IV_ROTATE_DELAY_MS 900
#define IV_SELF_WRITE_GRACE_US (4 * G_USEC_PER_SEC)

static void show_index(IvWindow *w, int i, gboolean fade);

/* ---- what is shown ------------------------------------------------------------- */

static void prefetch_ready(IvPicture *p, gpointer data)
{
	/* Nothing to do: the picture is in the cache now. */
}

static gboolean prefetch(gpointer data)
{
	IvWindow *w = data;
	int n = iv_folder_count(w->folder);
	const char *keep[6] = { NULL };
	int k = 0;

	w->prefetch_source = 0;
	if (n == 0 || w->index < 0)
		return G_SOURCE_REMOVE;
	keep[k++] = w->path;
	/* Forward first (that is where people go), then back, then one
	 * more forward. */
	static const int order[] = { 1, -1, 2, -2 };

	for (gsize i = 0; i < G_N_ELEMENTS(order); i++) {
		int j = w->index + order[i];
		const char *p;

		if (iv_settings_get()->loop || w->slideshow)
			j = ((j % n) + n) % n;
		if (j < 0 || j >= n || j == w->index)
			continue;
		p = iv_folder_path(w->folder, j);
		keep[k++] = p;
		if (!iv_cache_peek(w->cache, p))
			iv_cache_request(w->cache, p, 1 + (int)i, prefetch_ready, w);
	}
	keep[k] = NULL;
	iv_cache_trim(w->cache, keep);
	return G_SOURCE_REMOVE;
}

static void schedule_prefetch(IvWindow *w)
{
	if (w->prefetch_source)
		g_source_remove(w->prefetch_source);
	w->prefetch_source = g_timeout_add(60, prefetch, w);
}

static void display(IvWindow *w, IvPicture *p, gboolean fade, gboolean keep_view)
{
	if (p != w->pic) {
		if (w->pic)
			iv_picture_unref(w->pic);
		w->pic = iv_picture_ref(p);
	}
	iv_canvas_set_loading(w->canvas, FALSE);
	iv_canvas_set_picture(w->canvas, p, fade, keep_view);
	iv_canvas_set_playing(w->canvas, !w->editing);
	iv_info_set_picture(w->info, p);
	iv_window_set_title_text(w);
	iv_window_update_commands(w);
	w->first_shown = TRUE;
	schedule_prefetch(w);
}

static void picture_ready(IvPicture *p, gpointer data)
{
	IvWindow *w = data;

	w->ticket = 0;
	if (!w->path || strcmp(p->path, w->path) != 0)
		return; /* the user has moved on */
	display(w, p, w->slideshow, FALSE);
}

static void show_empty(IvWindow *w)
{
	g_clear_pointer(&w->path, g_free);
	if (w->pic) {
		iv_picture_unref(w->pic);
		w->pic = NULL;
	}
	w->index = -1;
	iv_canvas_set_loading(w->canvas, FALSE);
	if (w->folder && iv_folder_dir(w->folder))
		iv_canvas_set_message(w->canvas, IV_GLYPH_FOLDER,
				      "No pictures here",
				      "This folder has no pictures left to show.");
	else
		iv_canvas_set_message(w->canvas, IV_GLYPH_IMAGE, "No picture open",
				      "Open a picture from LikeOS Explorer, or "
				      "press Ctrl+O.");
	iv_info_set_picture(w->info, NULL);
	iv_filmstrip_reload(w->film);
	iv_window_set_title_text(w);
	iv_window_update_commands(w);
}

static void show_index(IvWindow *w, int i, gboolean fade)
{
	const char *path = iv_folder_path(w->folder, i);
	IvPicture *p;

	if (!path) {
		show_empty(w);
		return;
	}
	if (w->rotate_base && g_strcmp0(w->pending_rotate_path, path) != 0)
		iv_viewer_flush_rotation(w);
	w->index = i;
	if (g_strcmp0(w->path, path) != 0) {
		g_free(w->path);
		w->path = g_strdup(path);
	}
	if (w->ticket) {
		iv_cache_cancel(w->cache, w->ticket);
		w->ticket = 0;
	}
	iv_info_set_path(w->info, path);
	iv_filmstrip_set_current(w->film, i, TRUE);
	iv_window_set_title_text(w);
	p = iv_cache_peek(w->cache, path);
	if (p) {
		display(w, p, fade, FALSE);
		return;
	}
	/* The old picture stays up under the spinner until the new one is
	 * there: a blank flash between two pictures is worse than either. */
	if (w->pic) {
		iv_picture_unref(w->pic);
		w->pic = NULL;
	}
	iv_canvas_set_loading(w->canvas, TRUE);
	iv_window_update_commands(w);
	w->ticket = iv_cache_request(w->cache, path, 0, picture_ready, w);
}

static void folder_changed(IvFolder *f, gpointer data)
{
	IvWindow *w = data;
	int n = iv_folder_count(f);
	int idx = w->path ? iv_folder_index(f, w->path) : -1;

	iv_filmstrip_reload(w->film);
	if (idx >= 0) {
		struct stat st;
		gboolean mine = w->self_written && !strcmp(w->self_written, w->path) &&
				g_get_monotonic_time() - w->self_written_at <
					IV_SELF_WRITE_GRACE_US;

		w->index = idx;
		iv_filmstrip_set_current(w->film, idx, FALSE);
		/* Changed by another program: show the new pixels. */
		if (w->pic && !mine && !w->rotate_base && stat(w->path, &st) == 0 &&
		    (st.st_mtime != w->pic->mtime || (goffset)st.st_size != w->pic->file_size)) {
			iv_cache_forget(w->cache, w->path);
			iv_filmstrip_forget(w->film, w->path);
			if (w->ticket)
				iv_cache_cancel(w->cache, w->ticket);
			w->ticket = iv_cache_request(w->cache, w->path, 0,
						     picture_ready, w);
		}
		iv_window_set_title_text(w);
		iv_strip_update(w->strip);
		return;
	}
	if (!w->path)
		return;
	/* The current picture went away (deleted or moved by another
	 * program): the one that took its place in the list. */
	if (n == 0) {
		show_empty(w);
		return;
	}
	show_index(w, CLAMP(w->index, 0, n - 1), FALSE);
}

/* ---- opening -------------------------------------------------------------------- */

static void set_folder(IvWindow *w, IvFolder *f)
{
	if (w->folder)
		iv_folder_free(w->folder);
	w->folder = f;
	iv_folder_set_changed(f, folder_changed, w);
	iv_filmstrip_reload(w->film);
}

void iv_viewer_open(IvWindow *w, const char *path)
{
	char *abs = g_canonicalize_filename(path, NULL);
	IvSort sort = iv_sort_parse(w->opt ? w->opt->sort : NULL, NULL);
	int i;

	if (g_file_test(abs, G_FILE_TEST_IS_DIR)) {
		set_folder(w, iv_folder_new(abs, sort));
		g_free(abs);
		if (iv_folder_count(w->folder) > 0)
			show_index(w, 0, FALSE);
		else
			show_empty(w);
		return;
	}
	/* Another file of the folder already browsed: the same list. */
	if (w->folder && iv_folder_dir(w->folder)) {
		char *dir = g_path_get_dirname(abs);

		if (!strcmp(dir, iv_folder_dir(w->folder)) &&
		    g_file_test(abs, G_FILE_TEST_IS_REGULAR))
			iv_folder_add(w->folder, abs);
		g_free(dir);
	}
	i = iv_folder_index(w->folder, abs);
	if (i < 0) {
		set_folder(w, iv_folder_new(abs, sort));
		i = iv_folder_index(w->folder, abs);
	} else {
		iv_filmstrip_reload(w->film);
	}
	g_free(abs);
	if (i >= 0)
		show_index(w, i, FALSE);
	else
		show_empty(w);
}

void iv_viewer_open_list(IvWindow *w, char **paths)
{
	IvSort sort = iv_sort_parse(w->opt ? w->opt->sort : NULL, NULL);

	if (!paths || !paths[0]) {
		show_empty(w);
		return;
	}
	if (!paths[1]) {
		iv_viewer_open(w, paths[0]);
		return;
	}
	set_folder(w, iv_folder_new_for_list(paths, sort));
	if (iv_folder_count(w->folder) > 0)
		show_index(w, 0, FALSE);
	else
		show_empty(w);
}

void iv_viewer_reload(IvWindow *w)
{
	if (!w->path)
		return;
	iv_cache_forget(w->cache, w->path);
	if (w->ticket)
		iv_cache_cancel(w->cache, w->ticket);
	w->ticket = iv_cache_request(w->cache, w->path, 0, picture_ready, w);
}

void iv_viewer_replace(IvWindow *w, IvPicture *p)
{
	iv_cache_put(w->cache, p);
	display(w, p, FALSE, TRUE);
}

void iv_viewer_mark_self_written(IvWindow *w, const char *path)
{
	g_free(w->self_written);
	w->self_written = g_strdup(path);
	w->self_written_at = g_get_monotonic_time();
	iv_cache_forget(w->cache, path);
	iv_filmstrip_forget(w->film, path);
}

void iv_viewer_renamed(IvWindow *w, const char *from, const char *to)
{
	iv_folder_renamed(w->folder, from, to);
	iv_cache_forget(w->cache, from);
	iv_filmstrip_forget(w->film, from);
	if (g_strcmp0(w->path, from) == 0) {
		g_free(w->path);
		w->path = g_strdup(to);
		if (w->pic) {
			g_free(w->pic->path);
			g_free(w->pic->name);
			w->pic->path = g_strdup(to);
			w->pic->name = g_filename_display_basename(to);
			iv_cache_put(w->cache, w->pic);
		}
	}
	w->index = iv_folder_index(w->folder, w->path);
	iv_filmstrip_reload(w->film);
	iv_filmstrip_set_current(w->film, w->index, FALSE);
	iv_window_set_title_text(w);
	iv_info_set_picture(w->info, w->pic);
}

/* ---- going through the folder ---------------------------------------------------- */

gboolean iv_viewer_can_go(IvWindow *w, int delta)
{
	int n = iv_folder_count(w->folder);

	if (n <= 1 || w->index < 0)
		return FALSE;
	if (iv_settings_get()->loop)
		return TRUE;
	return w->index + delta >= 0 && w->index + delta < n;
}

static void restart_slideshow_timer(IvWindow *w);

void iv_viewer_go(IvWindow *w, int delta)
{
	int n = iv_folder_count(w->folder);
	int i;

	if (n == 0 || w->editing)
		return;
	i = w->index + delta;
	if (i < 0 || i >= n) {
		if (!iv_settings_get()->loop && !w->slideshow) {
			iv_window_toast(w, i < 0 ? "This is the first picture" :
						   "This is the last picture");
			return;
		}
		i = ((i % n) + n) % n;
	}
	if (i == w->index)
		return;
	show_index(w, i, w->slideshow);
	if (w->slideshow)
		restart_slideshow_timer(w);
}

void iv_viewer_go_to(IvWindow *w, int index)
{
	if (index < 0 || index >= iv_folder_count(w->folder) || w->editing)
		return;
	if (index != w->index)
		show_index(w, index, FALSE);
}

void iv_viewer_first(IvWindow *w)
{
	iv_viewer_go_to(w, 0);
}

void iv_viewer_last(IvWindow *w)
{
	iv_viewer_go_to(w, iv_folder_count(w->folder) - 1);
}

/* ---- turning ------------------------------------------------------------------------ */

typedef struct {
	IvWindow *w;
	char *path;
	IvPicture *base;
	int turns;
	GError *error;
} RotateJob;

static void rotate_job_free(gpointer p)
{
	RotateJob *j = p;

	g_free(j->path);
	iv_picture_unref(j->base);
	g_clear_error(&j->error);
	g_free(j);
}

static void rotate_thread(GTask *t, gpointer src, gpointer data,
			  GCancellable *c)
{
	RotateJob *j = data;

	g_task_return_boolean(t, iv_rotate_file(j->path, j->base, j->turns,
						&j->error));
}

static void rotate_written(GObject *o, GAsyncResult *res, gpointer data)
{
	RotateJob *j = g_task_get_task_data(G_TASK(res));
	IvWindow *w = j->w;

	if (!g_task_propagate_boolean(G_TASK(res), NULL)) {
		iv_window_error(w, "The picture could not be turned", j->error);
		/* Back to what the file really is. */
		iv_cache_forget(w->cache, j->path);
		if (w->path && !strcmp(w->path, j->path))
			iv_viewer_reload(w);
		return;
	}
	iv_viewer_mark_self_written(w, j->path);
	/* The file's own data (orientation tag, size, time) as it is now,
	 * quietly, keeping the view. */
	if (w->path && !strcmp(w->path, j->path) && !w->rotate_base) {
		if (w->ticket)
			iv_cache_cancel(w->cache, w->ticket);
		w->ticket = iv_cache_request(w->cache, w->path, 0, picture_ready, w);
	}
}

static void flush(IvWindow *w, gboolean wait)
{
	RotateJob *j;

	if (w->rotate_timer) {
		g_source_remove(w->rotate_timer);
		w->rotate_timer = 0;
	}
	if (!w->rotate_base)
		return;
	j = g_new0(RotateJob, 1);
	j->w = w;
	j->path = w->pending_rotate_path;
	j->base = w->rotate_base;
	j->turns = ((w->pending_turns % 4) + 4) % 4;
	w->pending_rotate_path = NULL;
	w->rotate_base = NULL;
	w->pending_turns = 0;
	if (j->turns == 0) {
		rotate_job_free(j);
		return;
	}
	g_free(w->self_written);
	w->self_written = g_strdup(j->path);
	w->self_written_at = g_get_monotonic_time();
	if (wait) {
		if (!iv_rotate_file(j->path, j->base, j->turns, &j->error))
			g_warning("%s: %s", j->path, j->error->message);
		rotate_job_free(j);
		return;
	}
	GTask *t = g_task_new(NULL, NULL, rotate_written, NULL);

	g_task_set_task_data(t, j, rotate_job_free);
	g_task_run_in_thread(t, rotate_thread);
	g_object_unref(t);
}

void iv_viewer_flush_rotation(IvWindow *w)
{
	flush(w, FALSE);
}

void iv_viewer_flush_rotation_now(IvWindow *w)
{
	flush(w, TRUE);
}

static gboolean rotate_later(gpointer data)
{
	IvWindow *w = data;

	w->rotate_timer = 0;
	flush(w, FALSE);
	return G_SOURCE_REMOVE;
}

void iv_viewer_rotate(IvWindow *w, int quarter_turns)
{
	IvPicture *turned;

	if (!w->pic || w->pic->error || w->editing)
		return;
	if (!iv_picture_writable(w->pic)) {
		iv_window_toast(w, iv_picture_is_animated(w->pic) ?
					   "Animated pictures cannot be turned" :
					   "Pictures in this format cannot be turned; "
					   "Save as keeps a turned copy");
		return;
	}
	if (access(w->path, W_OK) != 0) {
		iv_window_toast(w, "This picture cannot be changed (it is "
				   "read-only)");
		return;
	}
	if (w->rotate_base && g_strcmp0(w->pending_rotate_path, w->path) != 0)
		flush(w, FALSE);
	if (!w->rotate_base) {
		w->rotate_base = iv_picture_ref(w->pic);
		w->pending_rotate_path = g_strdup(w->path);
		w->pending_turns = 0;
	}
	w->pending_turns += quarter_turns;
	turned = iv_picture_new_rotated(w->rotate_base, w->pending_turns);
	if (!turned)
		return;
	iv_cache_put(w->cache, turned);
	display(w, turned, FALSE, FALSE);
	iv_picture_unref(turned);
	if (w->rotate_timer)
		g_source_remove(w->rotate_timer);
	w->rotate_timer = g_timeout_add(IV_ROTATE_DELAY_MS, rotate_later, w);
}

/* ---- the Trash ---------------------------------------------------------------------- */

static void undo_trash(gpointer data)
{
	IvWindow *w = data;
	GError *e = NULL;
	char *path;

	if (!w->trashed)
		return;
	path = g_strdup(iv_trash_record_path(w->trashed));
	if (!iv_trash_restore(w->trashed, &e)) {
		iv_window_error(w, "The picture could not be restored", e);
		g_clear_error(&e);
		g_free(path);
		return;
	}
	iv_trash_record_free(w->trashed);
	w->trashed = NULL;
	iv_folder_add(w->folder, path);
	iv_viewer_open(w, path);
	iv_window_toast(w, "Restored from the Trash");
	g_free(path);
}

static void do_delete(IvWindow *w)
{
	GError *e = NULL;
	IvTrashRecord *r;
	int old = w->index, n;
	char *path, *msg, *name;

	if (!w->path)
		return;
	/* A turn not written yet does not matter any more. */
	if (w->rotate_timer) {
		g_source_remove(w->rotate_timer);
		w->rotate_timer = 0;
	}
	if (w->rotate_base) {
		iv_picture_unref(w->rotate_base);
		w->rotate_base = NULL;
		g_clear_pointer(&w->pending_rotate_path, g_free);
		w->pending_turns = 0;
	}
	path = g_strdup(w->path);
	r = iv_trash_file(path, &e);
	if (!r) {
		iv_window_error(w, "The picture could not be moved to the Trash", e);
		g_clear_error(&e);
		g_free(path);
		return;
	}
	iv_trash_record_free(w->trashed);
	w->trashed = r;
	w->trashed_index = old;
	iv_folder_remove(w->folder, path);
	iv_cache_forget(w->cache, path);
	iv_filmstrip_forget(w->film, path);
	iv_filmstrip_reload(w->film);
	n = iv_folder_count(w->folder);
	g_clear_pointer(&w->path, g_free);
	if (n == 0)
		show_empty(w);
	else
		show_index(w, MIN(old, n - 1), FALSE);
	name = g_filename_display_basename(path);
	msg = g_strdup_printf("“%s” moved to the Trash", name);
	iv_toast_show(w->toast, msg, "Undo", undo_trash, w, NULL, 7);
	g_free(msg);
	g_free(name);
	g_free(path);
}

static void delete_answered(gboolean yes, gpointer data)
{
	if (yes)
		do_delete(data);
}

void iv_viewer_delete(IvWindow *w)
{
	char *name, *text;

	if (!w->path || w->editing)
		return;
	if (!iv_settings_get()->confirm_delete) {
		do_delete(w);
		return;
	}
	name = g_filename_display_basename(w->path);
	text = g_strdup_printf("“%s” goes to the Trash, where it can be "
			       "restored from.", name);
	iv_confirm(GTK_OVERLAY(w->view_overlay), "Move this picture to the Trash?",
		   text, "_Move to Trash", "_Cancel", TRUE, delete_answered, w);
	g_free(text);
	g_free(name);
}

/* ---- the slideshow ----------------------------------------------------------------- */

static gboolean slideshow_tick(gpointer data)
{
	IvWindow *w = data;
	int n = iv_folder_count(w->folder);

	if (!w->slideshow) {
		w->slideshow_timer = 0;
		return G_SOURCE_REMOVE;
	}
	if (w->slideshow_paused || n < 2)
		return G_SOURCE_CONTINUE;
	if (w->index + 1 >= n && !iv_settings_get()->loop) {
		w->slideshow_timer = 0;
		iv_viewer_slideshow(w, FALSE);
		iv_window_toast(w, "The slideshow has ended");
		return G_SOURCE_REMOVE;
	}
	show_index(w, (w->index + 1) % n, TRUE);
	return G_SOURCE_CONTINUE;
}

static void restart_slideshow_timer(IvWindow *w)
{
	if (w->slideshow_timer)
		g_source_remove(w->slideshow_timer);
	w->slideshow_timer = g_timeout_add_seconds(
		(guint)CLAMP(iv_settings_get()->slideshow_seconds, 1, 60),
		slideshow_tick, w);
}

void iv_viewer_slideshow(IvWindow *w, gboolean on)
{
	if (on == w->slideshow)
		return;
	if (on) {
		if (iv_folder_count(w->folder) == 0 || w->editing)
			return;
		w->slideshow = TRUE;
		w->slideshow_paused = FALSE;
		if (!w->fullscreen) {
			w->slideshow_own_fullscreen = TRUE;
			iv_window_set_fullscreen(w, TRUE);
		}
		iv_canvas_zoom_fit(w->canvas, FALSE);
		restart_slideshow_timer(w);
		iv_window_show_chrome(w);
	} else {
		w->slideshow = FALSE;
		if (w->slideshow_timer) {
			g_source_remove(w->slideshow_timer);
			w->slideshow_timer = 0;
		}
		if (w->slideshow_own_fullscreen) {
			w->slideshow_own_fullscreen = FALSE;
			iv_window_set_fullscreen(w, FALSE);
		}
	}
	iv_strip_update(w->strip);
}

void iv_viewer_slideshow_toggle_pause(IvWindow *w)
{
	if (!w->slideshow)
		return;
	w->slideshow_paused = !w->slideshow_paused;
	iv_window_toast(w, w->slideshow_paused ? "Slideshow paused" :
						 "Slideshow playing");
	if (!w->slideshow_paused)
		restart_slideshow_timer(w);
	iv_strip_update(w->strip);
}

/* ---- the panels ----------------------------------------------------------------------- */

void iv_viewer_toggle_filmstrip(IvWindow *w)
{
	w->film_visible = !w->film_visible;
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->film_revealer),
				      w->film_visible && !w->chrome_hidden);
	if (w->film_visible)
		iv_filmstrip_set_current(w->film, w->index, FALSE);
	iv_strip_update(w->strip);
}

void iv_viewer_set_info(IvWindow *w, gboolean on)
{
	w->info_visible = on;
	gtk_revealer_set_reveal_child(GTK_REVEALER(w->info_revealer), on);
	if (on)
		iv_info_set_picture(w->info, w->pic);
	iv_strip_update(w->strip);
}

void iv_viewer_toggle_info(IvWindow *w)
{
	iv_viewer_set_info(w, !w->info_visible);
}

/* ---- keys ----------------------------------------------------------------------------- */

gboolean iv_viewer_key(IvWindow *w, GdkEventKey *ev)
{
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
	gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
	gboolean shift = (mods & GDK_SHIFT_MASK) != 0;
	gboolean alt = (mods & GDK_MOD1_MASK) != 0;
	guint key = gdk_keyval_to_lower(ev->keyval);
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(w->window));

	/* Typing a new name in the info panel is the field's business. */
	if (focus && GTK_IS_EDITABLE(focus) && !ctrl &&
	    ev->keyval != GDK_KEY_F1 && ev->keyval != GDK_KEY_F11)
		return FALSE;
	if (alt) {
		if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
			iv_viewer_toggle_info(w);
			return TRUE;
		}
		return FALSE;
	}
	if (ctrl) {
		switch (key) {
		case GDK_KEY_s:
			iv_cmd_save_as(w);
			return TRUE;
		case GDK_KEY_c:
			if (shift)
				iv_cmd_copy_path(w);
			else
				iv_cmd_copy_image(w);
			return TRUE;
		case GDK_KEY_o:
			iv_cmd_open_file(w);
			return TRUE;
		case GDK_KEY_e:
			iv_window_enter_editor(w);
			return TRUE;
		case GDK_KEY_r:
			iv_viewer_rotate(w, shift ? -1 : 1);
			return TRUE;
		case GDK_KEY_l:
			iv_viewer_rotate(w, -1);
			return TRUE;
		case GDK_KEY_w:
		case GDK_KEY_q:
			gtk_window_close(GTK_WINDOW(w->window));
			return TRUE;
		case GDK_KEY_comma:
			iv_cmd_settings(w);
			return TRUE;
		case GDK_KEY_plus:
		case GDK_KEY_equal:
		case GDK_KEY_KP_Add:
			iv_canvas_zoom_step(w->canvas, 1, -1, -1);
			return TRUE;
		case GDK_KEY_minus:
		case GDK_KEY_KP_Subtract:
			iv_canvas_zoom_step(w->canvas, -1, -1, -1);
			return TRUE;
		case GDK_KEY_0:
			iv_canvas_zoom_fit(w->canvas, TRUE);
			return TRUE;
		case GDK_KEY_1:
			iv_canvas_zoom_actual(w->canvas, -1, -1);
			return TRUE;
		}
		return FALSE;
	}
	switch (ev->keyval) {
	case GDK_KEY_Left:
	case GDK_KEY_Up:
	case GDK_KEY_Page_Up:
	case GDK_KEY_KP_Page_Up:
	case GDK_KEY_BackSpace:
		iv_viewer_go(w, -1);
		return TRUE;
	case GDK_KEY_Right:
	case GDK_KEY_Down:
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Down:
		iv_viewer_go(w, 1);
		return TRUE;
	case GDK_KEY_space:
		if (w->slideshow)
			iv_viewer_slideshow_toggle_pause(w);
		else
			iv_viewer_go(w, 1);
		return TRUE;
	case GDK_KEY_Home:
		iv_viewer_first(w);
		return TRUE;
	case GDK_KEY_End:
		iv_viewer_last(w);
		return TRUE;
	case GDK_KEY_plus:
	case GDK_KEY_equal:
	case GDK_KEY_KP_Add:
		iv_canvas_zoom_step(w->canvas, 1, -1, -1);
		return TRUE;
	case GDK_KEY_minus:
	case GDK_KEY_KP_Subtract:
		iv_canvas_zoom_step(w->canvas, -1, -1, -1);
		return TRUE;
	case GDK_KEY_0:
	case GDK_KEY_KP_0:
		iv_canvas_zoom_fit(w->canvas, TRUE);
		return TRUE;
	case GDK_KEY_1:
	case GDK_KEY_KP_1:
		iv_canvas_zoom_actual(w->canvas, -1, -1);
		return TRUE;
	case GDK_KEY_F11:
		iv_window_toggle_fullscreen(w);
		return TRUE;
	case GDK_KEY_F5:
		iv_viewer_slideshow(w, !w->slideshow);
		return TRUE;
	case GDK_KEY_F2:
		iv_cmd_rename(w);
		return TRUE;
	case GDK_KEY_F1:
		iv_cmd_shortcuts(w);
		return TRUE;
	case GDK_KEY_Delete:
	case GDK_KEY_KP_Delete:
		iv_viewer_delete(w);
		return TRUE;
	case GDK_KEY_Escape:
		if (w->slideshow) {
			iv_viewer_slideshow(w, FALSE);
			return TRUE;
		}
		if (w->fullscreen) {
			iv_window_set_fullscreen(w, FALSE);
			return TRUE;
		}
		if (w->info_visible) {
			iv_viewer_set_info(w, FALSE);
			return TRUE;
		}
		if (w->film_visible) {
			iv_viewer_toggle_filmstrip(w);
			return TRUE;
		}
		return FALSE;
	}
	switch (key) {
	case GDK_KEY_f:
		iv_window_toggle_fullscreen(w);
		return TRUE;
	case GDK_KEY_e:
		iv_window_enter_editor(w);
		return TRUE;
	case GDK_KEY_r:
		iv_viewer_rotate(w, shift ? -1 : 1);
		return TRUE;
	case GDK_KEY_i:
		iv_viewer_toggle_info(w);
		return TRUE;
	case GDK_KEY_t:
		iv_viewer_toggle_filmstrip(w);
		return TRUE;
	case GDK_KEY_s:
		iv_viewer_slideshow(w, !w->slideshow);
		return TRUE;
	}
	return FALSE;
}

/* ---- the canvas ------------------------------------------------------------------------ */

static void view_changed(IvCanvas *c, IvWindow *w)
{
	iv_strip_update_zoom(w->strip);
}

static void navigate(IvCanvas *c, int dir, IvWindow *w)
{
	iv_viewer_go(w, dir);
}

static void activated(IvCanvas *c, IvWindow *w)
{
	/* A click in full screen brings the chrome back (or, when it is up,
	 * sends it away). */
	if (w->fullscreen) {
		if (w->chrome_hidden)
			iv_window_show_chrome(w);
	}
}

static void context_menu(IvCanvas *c, GdkEvent *ev, IvWindow *w)
{
	iv_menu_context(w, ev);
}

void iv_viewer_init(IvWindow *w)
{
	w->cache = iv_cache_new();
	g_signal_connect(w->canvas, "view-changed", G_CALLBACK(view_changed), w);
	g_signal_connect(w->canvas, "navigate", G_CALLBACK(navigate), w);
	g_signal_connect(w->canvas, "image-activated", G_CALLBACK(activated), w);
	g_signal_connect(w->canvas, "context-menu", G_CALLBACK(context_menu), w);
	show_empty(w);
}
