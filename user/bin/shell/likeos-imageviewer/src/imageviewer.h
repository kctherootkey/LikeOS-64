// LikeOS -- LikeOS Image Viewer: the program's shared declarations.
//
// One window per invocation (main.c).  The window (window.c) is a
// client-drawn title row over a stack of two modes: the viewer (viewer.c:
// the canvas with the floating strip, the filmstrip and the info panel)
// and the editor (editor.c, with its tools in tool-*.c).  What the modes
// share -- the picture loader and its cache (loader.c), the folder being
// browsed (folder.c), saving (saver.c), the Trash (trash.c), toasts and
// confirm cards (toast.c), the settings (settings.c) -- is declared here.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IMAGEVIEWER_H
#define IMAGEVIEWER_H

#include <gtk/gtk.h>

#include "glyphs.h"
#include "ivproc.h"

#define IV_APP_NAME "LikeOS Image Viewer"
#define IV_APP_ID "imageviewer"
#define IV_ICON_NAME "image-viewer"
#define IV_DESKTOP_ID "likeos-imageviewer.desktop"

/* GdkPixbufAnimation is marked deprecated by newer gdk-pixbuf releases,
 * yet GTK3 has no other way to decode and play an animated GIF: the calls
 * of loader.c and canvas.c that play animations sit between these two. */
#define IV_BEGIN_ANIMATION_API G_GNUC_BEGIN_IGNORE_DEPRECATIONS
#define IV_END_ANIMATION_API G_GNUC_END_IGNORE_DEPRECATIONS

/* Style classes the theme (_imageviewer.scss) styles. */
#define IV_CLASS_WINDOW "iv-window"

typedef struct IvWindow IvWindow;
typedef struct _IvCanvas IvCanvas;	/* a GtkWidget, canvas.h */
typedef struct IvEditor IvEditor;
typedef struct IvStrip IvStrip;
typedef struct IvFilmstrip IvFilmstrip;
typedef struct IvInfoPanel IvInfoPanel;
typedef struct IvToast IvToast;
typedef struct IvThumbs IvThumbs;
typedef struct IvFolder IvFolder;
typedef struct IvPicture IvPicture;
typedef struct IvPictureCache IvPictureCache;
typedef struct IvTrashRecord IvTrashRecord;

/* ---- command line (main.c) ------------------------------------------- */

typedef enum {
	IV_SORT_NAME = 0,	/* natural order of the names, as Explorer */
	IV_SORT_NAME_DESC,
	IV_SORT_MTIME,		/* oldest first */
	IV_SORT_MTIME_DESC,
	IV_SORT_SIZE,
	IV_SORT_SIZE_DESC,
	IV_SORT_TYPE,
} IvSort;

typedef struct {
	char **files;		/* the files (or one folder) named */
	char *sort;		/* --sort=name|name-desc|date|date-desc|size... */
	gboolean fullscreen;	/* --fullscreen */
	gboolean slideshow;	/* --slideshow */
	gboolean edit;		/* --edit: open the first file in the editor */
} IvOptions;

IvSort iv_sort_parse(const char *name, gboolean *ok);

/* ---- settings (settings.c) ------------------------------------------- */

typedef enum {
	IV_BACKDROP_DARK = 0,
	IV_BACKDROP_DARKER,
	IV_BACKDROP_CHECKER,
} IvBackdrop;

typedef struct {
	IvBackdrop backdrop;		/* behind the picture */
	gboolean wheel_zooms;		/* TRUE: wheel zooms; FALSE: browses */
	int slideshow_seconds;		/* 3, 5 or 10 (any 1..60 accepted) */
	gboolean confirm_delete;	/* ask before moving to the Trash */
	int jpeg_quality;		/* 10..100 */
	gboolean keep_exif;		/* keep camera data when saving JPEG */
	gboolean show_filmstrip;	/* filmstrip shown at start */
	gboolean loop;			/* past the last picture to the first */
	gboolean show_info;		/* info panel shown at start */
	gboolean smooth_zoom;		/* animate zoom steps */
	/* window geometry, remembered */
	int width, height;
	gboolean maximized;
} IvSettings;

IvSettings *iv_settings_get(void);
void iv_settings_save(void);
char *iv_settings_path(void);
/* Writes what is still waiting to be written (at exit). */
void iv_settings_flush(void);

/* Values the editor's tools keep between runs, in a group per tool
 * ("Draw", "Retouch", "Background").  A value missing or out of range
 * reads as `def'.  Setting one writes the file a moment later; changes in
 * a row are written once. */
double iv_settings_tool_double(const char *tool, const char *key, double def,
			       double lo, double hi);
void iv_settings_set_tool_double(const char *tool, const char *key, double v);
gboolean iv_settings_tool_bool(const char *tool, const char *key, gboolean def);
void iv_settings_set_tool_bool(const char *tool, const char *key, gboolean v);
/* 0xRRGGBBAA, stored as #rrggbb (#rrggbbaa when not opaque). */
guint32 iv_settings_tool_rgba(const char *tool, const char *key, guint32 def);
void iv_settings_set_tool_rgba(const char *tool, const char *key,
			       guint32 rgba);

/* ---- utilities (util.c) ---------------------------------------------- */

/* Formats that can be written. */
typedef enum {
	IV_FORMAT_NONE = 0,
	IV_FORMAT_PNG,
	IV_FORMAT_JPEG,
	IV_FORMAT_BMP,
	IV_FORMAT_TIFF,
	IV_FORMAT_WEBP,
	IV_FORMAT_COUNT
} IvFormat;

const char *iv_format_label(IvFormat f);	/* "JPEG" */
const char *iv_format_extension(IvFormat f);	/* "jpg" */
const char *iv_format_mime(IvFormat f);
IvFormat iv_format_for_mime(const char *mime);	/* NONE: not writable */
IvFormat iv_format_for_path(const char *path);	/* by extension */
gboolean iv_format_has_alpha(IvFormat f);

/* The content type of a file, from its first bytes and its name. */
char *iv_mime_for_path(const char *path);
/* A short label for a type: "JPEG", "PNG", "WebP", "GIF" ... */
char *iv_mime_label(const char *mime);
/* Whether the viewer can show files of this type / this file. */
gboolean iv_mime_is_image(const char *mime);
gboolean iv_path_is_image(const char *path);

/* Natural-order key of a file name (digits by value, case folded), the
 * order Explorer lists names in. */
char *iv_name_sort_key(const char *name);

char *iv_format_size(goffset bytes);			/* "142.4 kB" */
char *iv_format_datetime(gint64 unix_time);		/* "3 Nov 2026, 11:08" */
char *iv_format_exif_datetime(const char *exif);	/* EXIF "YYYY:MM:DD.." */
gint64 iv_parse_exif_datetime(const char *exif);	/* 0 when unparsable */

/* "name (resized).jpg" next to `path', numbered until it does not exist. */
char *iv_unique_sibling(const char *path, const char *suffix,
			const char *ext);
/* `path' with its extension replaced. */
char *iv_replace_extension(const char *path, const char *ext);

/* Pixels between GdkPixbuf and ivproc (both straight-alpha RGBA). */
IvImage *iv_image_from_pixbuf(const GdkPixbuf *pb);
GdkPixbuf *iv_pixbuf_from_image(const IvImage *img);
/* A cairo image surface (premultiplied ARGB32) from a pixbuf, safe to
 * call from a worker thread. */
cairo_surface_t *iv_surface_from_pixbuf(const GdkPixbuf *pb);
cairo_surface_t *iv_surface_from_image(const IvImage *img);
/* The pixbuf turned as EXIF orientation 1..8 says (a new ref). */
GdkPixbuf *iv_pixbuf_orient(GdkPixbuf *pb, int orientation);
/* Orientation after turning a picture shown as `o' by quarter turns. */
int iv_orientation_rotate(int o, int quarter_turns_cw);

/* The desktop and the rest of the system. */
gboolean iv_set_wallpaper(const char *path, GError **error);
gboolean iv_mail_available(void);
gboolean iv_mail_files(const char *const *paths, GError **error);
gboolean iv_show_in_explorer(const char *path, GError **error);
gboolean iv_open_folder(const char *dir, GError **error);

/* The 8-square checkerboard behind transparent pictures. */
cairo_pattern_t *iv_checker_pattern(int square);

/* Rounded rectangle path. */
void iv_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		     double r);

/* ---- style (style.c) --------------------------------------------------- */

void iv_style_install(void);
void iv_add_class(GtkWidget *w, const char *cls);
void iv_remove_class(GtkWidget *w, const char *cls);
void iv_set_class(GtkWidget *w, const char *cls, gboolean on);
/* A flat button showing a glyph; never takes the keyboard focus. */
GtkWidget *iv_glyph_button(IvGlyph g, int size, const char *tooltip,
			   const char *css_class);
GtkWidget *iv_glyph_toggle(IvGlyph g, int size, const char *tooltip,
			   const char *css_class);
void iv_glyph_button_set(GtkWidget *button, IvGlyph g);
/* A button with a glyph and a label (and a trailing chevron when
 * `chevron'). */
GtkWidget *iv_text_button(IvGlyph g, const char *label, gboolean chevron,
			  const char *css_class);
GtkWidget *iv_text_button_label(GtkWidget *button);
/* A menu item with a glyph, a label and the accelerator text at the
 * right; `cb' runs on activate with `data'. */
GtkWidget *iv_menu_item(GtkWidget *menu, IvGlyph g, const char *label,
			const char *accel, GCallback cb, gpointer data);
GtkWidget *iv_menu_separator(GtkWidget *menu);
/* A new menu in the dark palette of the window. */
GtkWidget *iv_menu_new(void);
void iv_menu_popup_below(GtkWidget *menu, GtkWidget *button);
void iv_menu_popup_below_end(GtkWidget *menu, GtkWidget *button);
void iv_menu_popup_above(GtkWidget *menu, GtkWidget *button);
/* A menu at the pointer for the click `ev', belonging to `top'. */
void iv_menu_popup_at_pointer(GtkWidget *menu, GtkWidget *top, GdkEvent *ev);
/* The fixed-size dialog look (Move and Close only). */
void iv_fixed_dialog(GtkWidget *dialog);
/* A heading label of the dark panels. */
GtkWidget *iv_section_label(const char *text);

/* ---- folders (folder.c) -------------------------------------------------- */

typedef void (*IvFolderChanged)(IvFolder *f, gpointer data);

/* The pictures in the folder of `path' (or the folder `path'), sorted.
 * Watches the folder; `changed' runs after the list was refreshed. */
IvFolder *iv_folder_new(const char *path, IvSort sort);
/* A fixed list (several files named on the command line). */
IvFolder *iv_folder_new_for_list(char **paths, IvSort sort);
void iv_folder_free(IvFolder *f);
void iv_folder_set_changed(IvFolder *f, IvFolderChanged cb, gpointer data);
int iv_folder_count(IvFolder *f);
const char *iv_folder_path(IvFolder *f, int i);
int iv_folder_index(IvFolder *f, const char *path);
const char *iv_folder_dir(IvFolder *f);
void iv_folder_set_sort(IvFolder *f, IvSort sort);
/* Bookkeeping for changes the program made itself, without waiting for
 * the monitor. */
void iv_folder_remove(IvFolder *f, const char *path);
void iv_folder_add(IvFolder *f, const char *path);
void iv_folder_renamed(IvFolder *f, const char *from, const char *to);

/* ---- pictures (loader.c) ------------------------------------------------- */

#define IV_MIP_MAX 8

struct IvPicture {
	gint ref;
	char *path;
	char *name;		/* display name */
	char *mime;
	char *format;		/* "JPEG" */
	goffset file_size;
	gint64 mtime;		/* seconds */
	int width, height;	/* as shown: after orientation */
	int file_width, file_height; /* as stored */
	int orientation;	/* EXIF 1..8, already applied */
	gboolean has_alpha;
	int bits;		/* per pixel, as stored */
	int channels;		/* as stored */
	gboolean reduced;	/* too large: loaded at a reduced size */
	GdkPixbuf *pixbuf;	/* full picture, oriented (first frame) */
	GdkPixbufAnimation *anim; /* animated pictures only */
	/* Prescaled copies for drawing: [0] the full picture, [k] 1/2^k. */
	cairo_surface_t *levels[IV_MIP_MAX];
	int n_levels;
	IvExif exif;
	int exif_status;	/* iv_exif_read_file()'s result */
	char *error;		/* set when the file could not be read */
};

IvPicture *iv_picture_ref(IvPicture *p);
void iv_picture_unref(IvPicture *p);
/* Load synchronously (any thread). */
IvPicture *iv_picture_load(const char *path, GCancellable *c, GError **e);
/* The full-size surface (created on first use). */
cairo_surface_t *iv_picture_surface(IvPicture *p, int level);
/* The level to draw at `scale' (screen pixels per picture pixel). */
int iv_picture_level_for(IvPicture *p, double scale);
gboolean iv_picture_is_animated(IvPicture *p);
/* A copy of a still picture turned by quarter turns clockwise, as it will
 * be once the file is turned (orientation, sizes and levels updated). */
IvPicture *iv_picture_new_rotated(IvPicture *p, int quarter_turns);
gboolean iv_picture_writable(IvPicture *p);

typedef void (*IvPictureReady)(IvPicture *p, gpointer data);

/* Loads in worker threads and keeps the pictures around the current one.
 * A request is answered on the main loop (synchronously never); the
 * returned ticket cancels the answer (the load itself still finishes into
 * the cache).  `priority' 0 is the picture being looked at, higher numbers
 * prefetches. */
IvPictureCache *iv_cache_new(void);
void iv_cache_free(IvPictureCache *c);
guint iv_cache_request(IvPictureCache *c, const char *path, int priority,
		       IvPictureReady cb, gpointer data);
void iv_cache_cancel(IvPictureCache *c, guint ticket);
IvPicture *iv_cache_peek(IvPictureCache *c, const char *path);
void iv_cache_forget(IvPictureCache *c, const char *path);
void iv_cache_put(IvPictureCache *c, IvPicture *p);
/* Keep only the pictures named (and what is in flight). */
void iv_cache_trim(IvPictureCache *c, const char *const *keep);

/* ---- thumbnails (thumbs.c) ----------------------------------------------- */

typedef void (*IvThumbReady)(const char *path, GdkPixbuf *thumb,
			     gpointer data);

IvThumbs *iv_thumbs_new(int size, IvThumbReady ready, gpointer data);
void iv_thumbs_free(IvThumbs *t);
/* The thumbnail if it is in memory (borrowed); otherwise NULL and a
 * request is queued (the most recent requests first). */
GdkPixbuf *iv_thumbs_get(IvThumbs *t, const char *path);
void iv_thumbs_forget(IvThumbs *t, const char *path);
void iv_thumbs_clear_queue(IvThumbs *t);

/* ---- the Trash (trash.c) --------------------------------------------------- */

/* Moves the file to the Trash and returns what restores it. */
IvTrashRecord *iv_trash_file(const char *path, GError **error);
gboolean iv_trash_restore(IvTrashRecord *r, GError **error);
const char *iv_trash_record_path(IvTrashRecord *r);
void iv_trash_record_free(IvTrashRecord *r);

/* ---- saving (saver.c) -------------------------------------------------------- */

typedef struct {
	IvFormat format;
	int quality;		/* JPEG / WebP 1..100; WebP 101 = lossless */
	gboolean keep_exif;	/* JPEG: carry the source's EXIF over */
	char *exif_from;	/* the file whose EXIF is carried */
	gboolean keep_mtime;	/* keep the target's modification time */
} IvSaveOptions;

void iv_save_options_init(IvSaveOptions *o, IvFormat f);
void iv_save_options_clear(IvSaveOptions *o);
/* Writes `pb' to `path' through a temporary file in the same folder and
 * rename(), so a failure never leaves half a file. */
gboolean iv_save_pixbuf(GdkPixbuf *pb, const char *path,
			const IvSaveOptions *o, GError **error);
/* The same off the main thread. */
void iv_save_pixbuf_async(GdkPixbuf *pb, const char *path,
			  const IvSaveOptions *o, GCancellable *c,
			  GAsyncReadyCallback cb, gpointer data);
gboolean iv_save_finish(GAsyncResult *res, GError **error);
/* An estimate of the encoded size, by encoding a small sample. */
goffset iv_save_estimate(GdkPixbuf *pb, int w, int h, const IvSaveOptions *o);

/* Rotates the FILE by quarter turns: JPEG losslessly through the EXIF
 * orientation where the file has the tag; everything else re-encoded.
 * `shown' is the picture as currently shown (oriented). */
gboolean iv_rotate_file(const char *path, IvPicture *shown, int quarter_turns,
			GError **error);

/* ---- toasts and confirm cards (toast.c) ------------------------------------ */

typedef void (*IvToastAction)(gpointer data);
typedef void (*IvConfirmDone)(gboolean accepted, gpointer data);

/* Toasts float at the bottom centre of `overlay', above whatever strip
 * the caller says it keeps there (`bottom_margin'). */
IvToast *iv_toast_new(GtkOverlay *overlay);
void iv_toast_free(IvToast *t);
void iv_toast_set_margin(IvToast *t, int bottom_margin);
/* A message, optionally with an action button ("Undo"); `seconds' 0 =
 * default (5).  A new toast replaces the one shown; the replaced one's
 * `destroy' runs. */
void iv_toast_show(IvToast *t, const char *text, const char *action,
		   IvToastAction cb, gpointer data, GDestroyNotify destroy,
		   int seconds);
/* A toast with a progress bar that stays until iv_toast_hide(). */
void iv_toast_progress(IvToast *t, const char *text, double fraction);
/* The same with an action button beside the bar ("Cancel"); later
 * iv_toast_progress() calls keep it. */
void iv_toast_progress_action(IvToast *t, const char *text, double fraction,
			      const char *action, IvToastAction cb,
			      gpointer data);
void iv_toast_hide(IvToast *t);
/* A card in the middle of the overlay: title, text, an accept and a
 * cancel button; the rest of the overlay is dimmed and takes no input
 * until it is answered.  Esc cancels, Enter accepts. */
void iv_confirm(GtkOverlay *overlay, const char *title, const char *text,
		const char *accept, const char *cancel, gboolean destructive,
		IvConfirmDone done, gpointer data);
/* A three-way card: accept, alternative, cancel.  `done' gets 1, 2 or 0. */
typedef void (*IvChoiceDone)(int choice, gpointer data);
void iv_choice(GtkOverlay *overlay, const char *title, const char *text,
	       const char *first, const char *second, const char *cancel,
	       IvChoiceDone done, gpointer data);
gboolean iv_confirm_is_open(GtkOverlay *overlay);

/* ---- the window (window.c) ---------------------------------------------- */

struct IvWindow {
	GtkWidget *window;
	IvOptions *opt;
	GtkAccelGroup *accel;

	/* The title row: one for viewing, one for editing. */
	GtkWidget *title_stack;
	GtkWidget *title_icon, *title_name, *title_count;
	GtkWidget *edit_title;
	GtkWidget *btn_edit, *btn_rotate, *btn_delete, *btn_open_with;
	GtkWidget *btn_more;
	GtkWidget *max_btn[2], *max_glyph[2];
	GtkWidget *title_rows[2];
	gboolean press_pending;
	double press_x, press_y;

	/* The modes. */
	GtkWidget *body_stack;		/* "viewer", "editor" */
	GtkWidget *view_overlay;
	GtkWidget *view_box;		/* canvas column | info panel */
	IvCanvas *canvas;
	IvStrip *strip;
	GtkWidget *strip_revealer;
	IvFilmstrip *film;
	GtkWidget *film_revealer;
	GtkWidget *chrome_box;		/* filmstrip over strip */
	IvInfoPanel *info;
	GtkWidget *info_revealer;
	IvToast *toast;
	IvEditor *editor;
	gboolean editing;

	/* What is shown. */
	IvFolder *folder;
	int index;
	char *path;			/* the current file */
	IvPicture *pic;			/* NULL while loading or failed */
	IvPictureCache *cache;
	guint ticket;			/* the current load */
	guint prefetch_source;
	guint spinner_source;
	gboolean first_shown;

	/* Full screen and the chrome that hides in it. */
	gboolean fullscreen;
	gboolean maximized;
	gboolean chrome_hidden;
	guint chrome_timer;
	guint insets_idle;
	double last_motion_x, last_motion_y;

	/* The slideshow. */
	gboolean slideshow;
	gboolean slideshow_paused;
	gboolean slideshow_own_fullscreen;
	guint slideshow_timer;

	gboolean film_visible, info_visible;

	/* A rotation the file has not been told about yet: the picture as it
	 * was before, and the turns since. */
	IvPicture *rotate_base;
	int pending_turns;
	char *pending_rotate_path;
	guint rotate_timer;

	/* The last move to the Trash, while its Undo is offered. */
	IvTrashRecord *trashed;
	int trashed_index;
	/* Path of a file the program itself just rewrote, so the folder
	 * monitor's change does not reload what is already shown. */
	char *self_written;
	gint64 self_written_at;
};

IvWindow *iv_window_new(IvOptions *opt);
void iv_window_destroy(IvWindow *w);
GtkWindow *iv_window_gtk(IvWindow *w);
void iv_window_set_title_text(IvWindow *w);
void iv_window_set_fullscreen(IvWindow *w, gboolean on);
void iv_window_toggle_fullscreen(IvWindow *w);
void iv_window_show_chrome(IvWindow *w);	/* and restart the hide timer */
void iv_window_enter_editor(IvWindow *w);
void iv_window_leave_editor(IvWindow *w);	/* the editor has finished */
void iv_window_error(IvWindow *w, const char *what, const GError *e);
void iv_window_toast(IvWindow *w, const char *text);
void iv_window_update_commands(IvWindow *w);	/* sensitivity */
void iv_window_settings_changed(IvWindow *w);
GtkOverlay *iv_window_overlay(IvWindow *w);

/* ---- the viewer (viewer.c) --------------------------------------------------- */

void iv_viewer_init(IvWindow *w);
void iv_viewer_open(IvWindow *w, const char *path);
void iv_viewer_open_list(IvWindow *w, char **paths);
void iv_viewer_go(IvWindow *w, int delta);	/* +1 next, -1 previous */
void iv_viewer_go_to(IvWindow *w, int index);
void iv_viewer_first(IvWindow *w);
void iv_viewer_last(IvWindow *w);
void iv_viewer_reload(IvWindow *w);
/* The current picture changed on disk or was replaced by a save. */
void iv_viewer_replace(IvWindow *w, IvPicture *p);
gboolean iv_viewer_can_go(IvWindow *w, int delta);
void iv_viewer_rotate(IvWindow *w, int quarter_turns);
/* Writes a pending rotation to the file now (in the background unless
 * `wait'). */
void iv_viewer_flush_rotation(IvWindow *w);
void iv_viewer_flush_rotation_now(IvWindow *w);
void iv_viewer_delete(IvWindow *w);
void iv_viewer_slideshow(IvWindow *w, gboolean on);
void iv_viewer_slideshow_toggle_pause(IvWindow *w);
void iv_viewer_toggle_filmstrip(IvWindow *w);
void iv_viewer_toggle_info(IvWindow *w);
void iv_viewer_set_info(IvWindow *w, gboolean on);
gboolean iv_viewer_key(IvWindow *w, GdkEventKey *ev);
/* After the file was renamed by the program. */
void iv_viewer_renamed(IvWindow *w, const char *from, const char *to);
void iv_viewer_mark_self_written(IvWindow *w, const char *path);

/* ---- the bottom strip (strip.c) ------------------------------------------------ */

IvStrip *iv_strip_new(IvWindow *w);
void iv_strip_free(IvStrip *s);
GtkWidget *iv_strip_widget(IvStrip *s);
void iv_strip_update(IvStrip *s);	/* picture, navigation, toggles */
void iv_strip_update_zoom(IvStrip *s);
/* The zoom menu, also on the editor's zoom button. */
void iv_zoom_menu_popup(IvCanvas *canvas, GtkWidget *button);

/* ---- the filmstrip (filmstrip.c) --------------------------------------------- */

IvFilmstrip *iv_filmstrip_new(IvWindow *w);
void iv_filmstrip_free(IvFilmstrip *f);
GtkWidget *iv_filmstrip_widget(IvFilmstrip *f);
void iv_filmstrip_reload(IvFilmstrip *f);	/* the folder's list changed */
void iv_filmstrip_set_current(IvFilmstrip *f, int index, gboolean animate);
void iv_filmstrip_forget(IvFilmstrip *f, const char *path);

/* ---- the info panel (infopanel.c) ------------------------------------------ */

IvInfoPanel *iv_info_new(IvWindow *w);
void iv_info_free(IvInfoPanel *i);
GtkWidget *iv_info_widget(IvInfoPanel *i);
void iv_info_set_picture(IvInfoPanel *i, IvPicture *p);
void iv_info_set_path(IvInfoPanel *i, const char *path); /* while loading */
void iv_info_start_rename(IvInfoPanel *i);
gboolean iv_info_is_editing(IvInfoPanel *i);

/* ---- menus (menus.c) ----------------------------------------------------- */

void iv_menu_open_with(IvWindow *w, GtkWidget *button);
void iv_menu_more(IvWindow *w, GtkWidget *button);
void iv_menu_context(IvWindow *w, GdkEvent *ev);

/* ---- commands (actions.c) ------------------------------------------------ */

void iv_cmd_save_as(IvWindow *w);
void iv_cmd_copy_image(IvWindow *w);
void iv_cmd_copy_path(IvWindow *w);
void iv_cmd_resize(IvWindow *w);
void iv_cmd_wallpaper(IvWindow *w);
void iv_cmd_mail(IvWindow *w);
void iv_cmd_rename(IvWindow *w);
void iv_cmd_settings(IvWindow *w);
void iv_cmd_shortcuts(IvWindow *w);
void iv_cmd_about(IvWindow *w);
void iv_cmd_show_in_explorer(IvWindow *w);
void iv_cmd_open_folder(IvWindow *w);
void iv_cmd_open_with(IvWindow *w, GAppInfo *app);
void iv_cmd_choose_app(IvWindow *w);
void iv_cmd_open_file(IvWindow *w);
/* Renames the current file to `new_name' (a name, not a path).  FALSE
 * with a message shown when it cannot. */
gboolean iv_cmd_rename_to(IvWindow *w, const char *new_name);
gboolean iv_cmd_set_mtime(IvWindow *w, gint64 when);

/* ---- dialogs (dialogs.c) ------------------------------------------------- */

typedef void (*IvSaveChosen)(const char *path, const IvSaveOptions *o,
			     gpointer data);

/* Save as: a file chooser with the format, the quality and Keep EXIF. */
void iv_dialog_save_as(GtkWindow *parent, const char *suggest_path,
		       IvFormat suggest, gboolean has_alpha, IvSaveChosen done,
		       gpointer data);
void iv_dialog_resize(IvWindow *w);
void iv_dialog_settings(IvWindow *w);
void iv_dialog_shortcuts(IvWindow *w);
void iv_dialog_about(IvWindow *w);

#endif
