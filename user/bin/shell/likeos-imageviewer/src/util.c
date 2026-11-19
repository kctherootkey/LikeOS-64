// LikeOS -- LikeOS Image Viewer: small things everything uses.
//
// File types and the formats the program writes, names and their natural
// order, sizes and dates in words, pixels between GdkPixbuf, cairo and the
// processing library, EXIF orientation, and the few places the program
// hands work to the rest of the desktop: the wallpaper (the PCManFM
// desktop), mail (the mail program's attachment line), the file manager.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#include <gio/gdesktopappinfo.h>

#include "imageviewer.h"

/* ---- the formats written ----------------------------------------------- */

static const struct {
	const char *label, *ext, *mime;
	gboolean alpha;
} formats[IV_FORMAT_COUNT] = {
	[IV_FORMAT_NONE] = { "", "", "", FALSE },
	[IV_FORMAT_PNG] = { "PNG", "png", "image/png", TRUE },
	[IV_FORMAT_JPEG] = { "JPEG", "jpg", "image/jpeg", FALSE },
	[IV_FORMAT_BMP] = { "BMP", "bmp", "image/bmp", FALSE },
	[IV_FORMAT_TIFF] = { "TIFF", "tif", "image/tiff", TRUE },
	[IV_FORMAT_WEBP] = { "WebP", "webp", "image/webp", TRUE },
};

const char *iv_format_label(IvFormat f)
{
	return f > IV_FORMAT_NONE && f < IV_FORMAT_COUNT ? formats[f].label :
							    "";
}

const char *iv_format_extension(IvFormat f)
{
	return f > IV_FORMAT_NONE && f < IV_FORMAT_COUNT ? formats[f].ext : "";
}

const char *iv_format_mime(IvFormat f)
{
	return f > IV_FORMAT_NONE && f < IV_FORMAT_COUNT ? formats[f].mime :
							    "";
}

gboolean iv_format_has_alpha(IvFormat f)
{
	return f > IV_FORMAT_NONE && f < IV_FORMAT_COUNT && formats[f].alpha;
}

IvFormat iv_format_for_mime(const char *mime)
{
	if (!mime)
		return IV_FORMAT_NONE;
	if (!strcmp(mime, "image/png"))
		return IV_FORMAT_PNG;
	if (!strcmp(mime, "image/jpeg") || !strcmp(mime, "image/pjpeg") ||
	    !strcmp(mime, "image/jpg"))
		return IV_FORMAT_JPEG;
	if (!strcmp(mime, "image/bmp") || !strcmp(mime, "image/x-bmp") ||
	    !g_ascii_strcasecmp(mime, "image/x-ms-bmp"))
		return IV_FORMAT_BMP;
	if (!strcmp(mime, "image/tiff"))
		return IV_FORMAT_TIFF;
	if (!strcmp(mime, "image/webp"))
		return IV_FORMAT_WEBP;
	return IV_FORMAT_NONE;
}

IvFormat iv_format_for_path(const char *path)
{
	const char *dot = path ? strrchr(path, '.') : NULL;

	if (!dot || strchr(dot, '/'))
		return IV_FORMAT_NONE;
	dot++;
	if (!g_ascii_strcasecmp(dot, "png"))
		return IV_FORMAT_PNG;
	if (!g_ascii_strcasecmp(dot, "jpg") || !g_ascii_strcasecmp(dot, "jpeg") ||
	    !g_ascii_strcasecmp(dot, "jpe") || !g_ascii_strcasecmp(dot, "jfif"))
		return IV_FORMAT_JPEG;
	if (!g_ascii_strcasecmp(dot, "bmp"))
		return IV_FORMAT_BMP;
	if (!g_ascii_strcasecmp(dot, "tif") || !g_ascii_strcasecmp(dot, "tiff"))
		return IV_FORMAT_TIFF;
	if (!g_ascii_strcasecmp(dot, "webp"))
		return IV_FORMAT_WEBP;
	return IV_FORMAT_NONE;
}

/* ---- file types ---------------------------------------------------------- */

/* What the image library reads, collected once: its MIME types and file
 * name extensions, and WebP, which the program decodes itself. */
static GHashTable *readable_mimes, *readable_exts;

static void collect_readable(void)
{
	if (readable_mimes)
		return;
	readable_mimes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					       NULL);
	readable_exts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					      NULL);
	GSList *fmts = gdk_pixbuf_get_formats();

	for (GSList *l = fmts; l; l = l->next) {
		GdkPixbufFormat *f = l->data;
		char **m = gdk_pixbuf_format_get_mime_types(f);
		char **e = gdk_pixbuf_format_get_extensions(f);
		char *name = gdk_pixbuf_format_get_name(f);

		/* SVG needs a renderer that is not part of this system and
		 * "ani" cursors are not pictures; neither is offered. */
		if (name && (!strcmp(name, "svg") || !strcmp(name, "ani"))) {
			g_strfreev(m);
			g_strfreev(e);
			g_free(name);
			continue;
		}
		for (int i = 0; m && m[i]; i++)
			g_hash_table_add(readable_mimes, g_ascii_strdown(m[i], -1));
		for (int i = 0; e && e[i]; i++)
			g_hash_table_add(readable_exts, g_ascii_strdown(e[i], -1));
		g_strfreev(m);
		g_strfreev(e);
		g_free(name);
	}
	g_slist_free(fmts);
	g_hash_table_add(readable_mimes, g_strdup("image/webp"));
	g_hash_table_add(readable_exts, g_strdup("webp"));
}

char *iv_mime_for_path(const char *path)
{
	guchar head[512];
	gsize n = 0;
	gboolean uncertain = FALSE;
	FILE *fp = fopen(path, "rb");

	if (fp) {
		n = fread(head, 1, sizeof head, fp);
		fclose(fp);
	}
	/* WebP first: RIFF....WEBP, which older type databases name
	 * after the RIFF container alone. */
	if (n >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4))
		return g_strdup("image/webp");
	char *ct = g_content_type_guess(path, n ? head : NULL, n, &uncertain);
	char *mime = ct ? g_content_type_get_mime_type(ct) : NULL;

	g_free(ct);
	if (!mime)
		mime = g_strdup("application/octet-stream");
	/* An unknown name with picture bytes in it: ask the image library. */
	if (!iv_mime_is_image(mime)) {
		GdkPixbufFormat *f = gdk_pixbuf_get_file_info(path, NULL, NULL);
		char **m = f ? gdk_pixbuf_format_get_mime_types(f) : NULL;

		if (m && m[0]) {
			g_free(mime);
			mime = g_strdup(m[0]);
		}
		g_strfreev(m);
	}
	return mime;
}

char *iv_mime_label(const char *mime)
{
	static const struct {
		const char *mime, *label;
	} known[] = {
		{ "image/jpeg", "JPEG" }, { "image/pjpeg", "JPEG" },
		{ "image/png", "PNG" }, { "image/gif", "GIF" },
		{ "image/webp", "WebP" }, { "image/bmp", "BMP" },
		{ "image/x-bmp", "BMP" }, { "image/x-ms-bmp", "BMP" },
		{ "image/tiff", "TIFF" }, { "image/x-icon", "ICO" },
		{ "image/vnd.microsoft.icon", "ICO" },
		{ "image/x-tga", "TGA" }, { "image/x-portable-pixmap", "PPM" },
		{ "image/x-portable-graymap", "PGM" },
		{ "image/x-portable-bitmap", "PBM" },
		{ "image/x-portable-anymap", "PNM" },
		{ "image/x-xpixmap", "XPM" }, { "image/x-xbitmap", "XBM" },
		{ "image/x-icns", "ICNS" }, { "image/x-quicktime", "QTIF" },
		{ "image/qtif", "QTIF" },
	};

	if (!mime)
		return g_strdup("");
	for (gsize i = 0; i < G_N_ELEMENTS(known); i++)
		if (!g_ascii_strcasecmp(known[i].mime, mime))
			return g_strdup(known[i].label);
	const char *slash = strchr(mime, '/');
	const char *sub = slash ? slash + 1 : mime;

	if (!strncmp(sub, "x-", 2))
		sub += 2;
	return g_ascii_strup(sub, -1);
}

gboolean iv_mime_is_image(const char *mime)
{
	char *low;
	gboolean ok;

	if (!mime)
		return FALSE;
	collect_readable();
	low = g_ascii_strdown(mime, -1);
	ok = g_hash_table_contains(readable_mimes, low);
	g_free(low);
	return ok;
}

gboolean iv_path_is_image(const char *path)
{
	const char *base = strrchr(path, '/');
	const char *dot = strrchr(base ? base : path, '.');
	char *low;
	gboolean ok;

	if (!dot || !dot[1])
		return FALSE;
	collect_readable();
	low = g_ascii_strdown(dot + 1, -1);
	ok = g_hash_table_contains(readable_exts, low);
	g_free(low);
	return ok;
}

/* ---- names, sizes, dates ------------------------------------------------- */

char *iv_name_sort_key(const char *name)
{
	return g_utf8_collate_key_for_filename(name ? name : "", -1);
}

char *iv_format_size(goffset bytes)
{
	if (bytes < 0)
		return g_strdup("");
	return g_format_size((guint64)bytes);
}

char *iv_format_datetime(gint64 unix_time)
{
	GDateTime *dt = g_date_time_new_from_unix_local(unix_time);
	char *s;

	if (!dt)
		return g_strdup("");
	s = g_date_time_format(dt, "%-d %b %Y, %H:%M");
	g_date_time_unref(dt);
	return s;
}

gint64 iv_parse_exif_datetime(const char *exif)
{
	int y, mo, d, h = 0, mi = 0, s = 0;

	if (!exif || !*exif)
		return 0;
	if (sscanf(exif, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3 &&
	    sscanf(exif, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3)
		return 0;
	if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31)
		return 0;
	/* EXIF times are the camera's local time without a zone. */
	GDateTime *dt = g_date_time_new_local(y, mo, d, h, mi, s);

	if (!dt)
		return 0;
	gint64 t = g_date_time_to_unix(dt);

	g_date_time_unref(dt);
	return t;
}

char *iv_format_exif_datetime(const char *exif)
{
	gint64 t = iv_parse_exif_datetime(exif);

	return t ? iv_format_datetime(t) : g_strdup("");
}

char *iv_replace_extension(const char *path, const char *ext)
{
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(slash ? slash : path, '.');
	gsize stem = dot && dot != (slash ? slash + 1 : path) ?
			     (gsize)(dot - path) :
			     strlen(path);

	return g_strdup_printf("%.*s.%s", (int)stem, path, ext);
}

char *iv_unique_sibling(const char *path, const char *suffix, const char *ext)
{
	char *dir = g_path_get_dirname(path);
	char *base = g_path_get_basename(path);
	char *dot = strrchr(base, '.');
	char *out = NULL;

	if (dot && dot != base) {
		if (!ext)
			ext = dot + 1;
		*dot = '\0';
	}
	if (!ext || !*ext)
		ext = "png";
	char *e = g_strdup(ext);

	for (int n = 1; n < 10000; n++) {
		char *name = n == 1 ?
				     g_strdup_printf("%s%s.%s", base, suffix, e) :
				     g_strdup_printf("%s%s %d.%s", base, suffix,
						     n, e);
		char *p = g_build_filename(dir, name, NULL);

		g_free(name);
		if (!g_file_test(p, G_FILE_TEST_EXISTS)) {
			out = p;
			break;
		}
		g_free(p);
	}
	g_free(e);
	g_free(base);
	g_free(dir);
	return out;
}

/* ---- pixels -------------------------------------------------------------- */

IvImage *iv_image_from_pixbuf(const GdkPixbuf *pb)
{
	if (!pb)
		return NULL;
	return iv_image_new_from_data(gdk_pixbuf_read_pixels(pb),
				      gdk_pixbuf_get_width(pb),
				      gdk_pixbuf_get_height(pb),
				      gdk_pixbuf_get_rowstride(pb),
				      gdk_pixbuf_get_n_channels(pb));
}

static void free_image_pixels(guchar *px, gpointer data)
{
	iv_image_free(data);
}

GdkPixbuf *iv_pixbuf_from_image(const IvImage *img)
{
	if (!img)
		return NULL;
	IvImage *copy = iv_image_copy(img);

	if (!copy)
		return NULL;
	return gdk_pixbuf_new_from_data(copy->px, GDK_COLORSPACE_RGB, TRUE, 8,
					copy->w, copy->h, copy->stride,
					free_image_pixels, copy);
}

/* Straight RGB(A) bytes to cairo's premultiplied native-endian words. */
static cairo_surface_t *surface_from_rows(const guchar *px, int w, int h,
					  int stride, int channels)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w,
							h);

	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		return NULL;
	}
	cairo_surface_flush(s);
	guchar *dst = cairo_image_surface_get_data(s);
	int ds = cairo_image_surface_get_stride(s);

	for (int y = 0; y < h; y++) {
		const guchar *src = px + (gsize)y * stride;
		guint32 *out = (guint32 *)(dst + (gsize)y * ds);

		if (channels == 3) {
			for (int x = 0; x < w; x++, src += 3)
				out[x] = 0xff000000u | ((guint32)src[0] << 16) |
					 ((guint32)src[1] << 8) | src[2];
		} else {
			for (int x = 0; x < w; x++, src += 4) {
				guint a = src[3];

				if (a == 255) {
					out[x] = 0xff000000u |
						 ((guint32)src[0] << 16) |
						 ((guint32)src[1] << 8) | src[2];
				} else if (a == 0) {
					out[x] = 0;
				} else {
					/* x * a / 255, rounded */
					guint r = src[0] * a + 0x80;
					guint g = src[1] * a + 0x80;
					guint b = src[2] * a + 0x80;

					r = (r + (r >> 8)) >> 8;
					g = (g + (g >> 8)) >> 8;
					b = (b + (b >> 8)) >> 8;
					out[x] = ((guint32)a << 24) | (r << 16) |
						 (g << 8) | b;
				}
			}
		}
	}
	cairo_surface_mark_dirty(s);
	return s;
}

cairo_surface_t *iv_surface_from_pixbuf(const GdkPixbuf *pb)
{
	if (!pb)
		return NULL;
	return surface_from_rows(gdk_pixbuf_read_pixels(pb),
				 gdk_pixbuf_get_width(pb),
				 gdk_pixbuf_get_height(pb),
				 gdk_pixbuf_get_rowstride(pb),
				 gdk_pixbuf_get_n_channels(pb));
}

cairo_surface_t *iv_surface_from_image(const IvImage *img)
{
	if (!img)
		return NULL;
	return surface_from_rows(img->px, img->w, img->h, img->stride, 4);
}

/* An orientation as a mirror (applied first) and quarter turns clockwise
 * (applied after): the eight EXIF codes are the eight combinations. */
static const struct {
	int turns, mirror;
} orient_parts[9] = {
	{ 0, 0 }, /* 0: treated as 1 */
	{ 0, 0 }, { 0, 1 }, { 2, 0 }, { 2, 1 },
	{ 3, 1 }, { 1, 0 }, { 1, 1 }, { 3, 0 },
};

static int orientation_from_parts(int turns, int mirror)
{
	for (int o = 1; o <= 8; o++)
		if (orient_parts[o].turns == turns &&
		    orient_parts[o].mirror == mirror)
			return o;
	return 1;
}

int iv_orientation_rotate(int o, int quarter_turns_cw)
{
	if (o < 1 || o > 8)
		o = 1;
	int t = ((orient_parts[o].turns + quarter_turns_cw) % 4 + 4) % 4;

	return orientation_from_parts(t, orient_parts[o].mirror);
}

GdkPixbuf *iv_pixbuf_orient(GdkPixbuf *pb, int orientation)
{
	GdkPixbuf *cur, *next;

	if (!pb)
		return NULL;
	if (orientation < 2 || orientation > 8)
		return g_object_ref(pb);
	cur = g_object_ref(pb);
	if (orient_parts[orientation].mirror) {
		next = gdk_pixbuf_flip(cur, TRUE);
		g_object_unref(cur);
		cur = next;
	}
	switch (orient_parts[orientation].turns) {
	case 1:
		next = gdk_pixbuf_rotate_simple(cur, GDK_PIXBUF_ROTATE_CLOCKWISE);
		break;
	case 2:
		next = gdk_pixbuf_rotate_simple(cur, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
		break;
	case 3:
		next = gdk_pixbuf_rotate_simple(cur,
						GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
		break;
	default:
		next = g_object_ref(cur);
		break;
	}
	g_object_unref(cur);
	return next;
}

cairo_pattern_t *iv_checker_pattern(int square)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
							2 * square, 2 * square);
	cairo_t *cr = cairo_create(s);
	cairo_pattern_t *p;
	cairo_matrix_t m;

	/* Two dark greys a step apart: visible behind a picture, quiet
	 * next to the shell colours round it. */
	cairo_set_source_rgb(cr, 0x2b / 255.0, 0x31 / 255.0, 0x3d / 255.0);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0x3a / 255.0, 0x41 / 255.0, 0x4f / 255.0);
	cairo_rectangle(cr, 0, 0, square, square);
	cairo_rectangle(cr, square, square, square, square);
	cairo_fill(cr);
	cairo_destroy(cr);
	p = cairo_pattern_create_for_surface(s);
	cairo_surface_destroy(s);
	cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
	cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
	cairo_matrix_init_identity(&m);
	cairo_pattern_set_matrix(p, &m);
	return p;
}

void iv_rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		     double r)
{
	r = MIN(r, MIN(w / 2, h / 2));
	if (r <= 0) {
		cairo_rectangle(cr, x, y, w, h);
		return;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 1.5 * G_PI);
	cairo_close_path(cr);
}

/* ---- the rest of the desktop --------------------------------------------- */

/* The desktop is PCManFM's: it is asked to show the picture filling the
 * screen, cut to fit (its "crop" mode) -- what LikeOS Explorer's "Set as
 * desktop background" asks for too. */
gboolean iv_set_wallpaper(const char *path, GError **error)
{
	char *arg = g_strdup_printf("--set-wallpaper=%s", path);
	char *argv[] = { (char *)"pcmanfm", arg, (char *)"--wallpaper-mode=crop",
			 NULL };
	gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
				    NULL, NULL, error);

	g_free(arg);
	return ok;
}

/* The mail program's way of taking attachments, from its desktop entry:
 * an Exec-like line in which %F stands for the files. */
#define IV_ATTACH_KEY "X-LikeOS-Attach"

static GDesktopAppInfo *mailer(void)
{
	GAppInfo *a = g_app_info_get_default_for_uri_scheme("mailto");

	if (a && G_IS_DESKTOP_APP_INFO(a) &&
	    g_desktop_app_info_has_key(G_DESKTOP_APP_INFO(a), IV_ATTACH_KEY))
		return G_DESKTOP_APP_INFO(a);
	g_clear_object(&a);
	return NULL;
}

gboolean iv_mail_available(void)
{
	GDesktopAppInfo *m = mailer();
	gboolean ok = m != NULL;

	g_clear_object(&m);
	return ok;
}

gboolean iv_mail_files(const char *const *paths, GError **error)
{
	GDesktopAppInfo *m = mailer();
	char *line, **argv = NULL;
	GPtrArray *args;
	gboolean ok;

	if (!m) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "No mail program that takes attachments "
				    "is set up");
		return FALSE;
	}
	line = g_desktop_app_info_get_string(m, IV_ATTACH_KEY);
	if (!line || !g_shell_parse_argv(line, NULL, &argv, error)) {
		if (line == NULL)
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
					    "The mail program's attachment "
					    "command is empty");
		g_free(line);
		g_object_unref(m);
		return FALSE;
	}
	args = g_ptr_array_new_with_free_func(g_free);
	for (int i = 0; argv[i]; i++) {
		if (!strcmp(argv[i], "%F") || !strcmp(argv[i], "%f")) {
			for (int k = 0; paths[k]; k++)
				g_ptr_array_add(args, g_strdup(paths[k]));
		} else {
			g_ptr_array_add(args, g_strdup(argv[i]));
		}
	}
	g_ptr_array_add(args, NULL);
	ok = g_spawn_async(NULL, (char **)args->pdata, NULL, G_SPAWN_SEARCH_PATH,
			   NULL, NULL, NULL, error);
	g_ptr_array_unref(args);
	g_strfreev(argv);
	g_free(line);
	g_object_unref(m);
	return ok;
}

/* LikeOS Explorer given a file opens its folder with the file selected. */
gboolean iv_show_in_explorer(const char *path, GError **error)
{
	char *argv[] = { (char *)"likeos-explorer", (char *)path, NULL };

	return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			     NULL, error);
}

/* A folder opens in whatever opens folders (Explorer, by default). */
gboolean iv_open_folder(const char *dir, GError **error)
{
	GFile *f = g_file_new_for_path(dir);
	char *uri = g_file_get_uri(f);
	gboolean ok = g_app_info_launch_default_for_uri(uri, NULL, error);

	if (!ok && error && *error) {
		/* No default for folders: Explorer by name. */
		char *argv[] = { (char *)"likeos-explorer", (char *)dir, NULL };

		g_clear_error(error);
		ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
				   NULL, NULL, error);
	}
	g_free(uri);
	g_object_unref(f);
	return ok;
}

IvSort iv_sort_parse(const char *name, gboolean *ok)
{
	static const struct {
		const char *name;
		IvSort sort;
	} names[] = {
		{ "name", IV_SORT_NAME },	  { "name-desc", IV_SORT_NAME_DESC },
		{ "date", IV_SORT_MTIME },	  { "mtime", IV_SORT_MTIME },
		{ "date-desc", IV_SORT_MTIME_DESC },
		{ "mtime-desc", IV_SORT_MTIME_DESC },
		{ "size", IV_SORT_SIZE },	  { "size-desc", IV_SORT_SIZE_DESC },
		{ "type", IV_SORT_TYPE },
	};

	if (ok)
		*ok = TRUE;
	if (!name || !*name)
		return IV_SORT_NAME;
	for (gsize i = 0; i < G_N_ELEMENTS(names); i++)
		if (!g_ascii_strcasecmp(names[i].name, name))
			return names[i].sort;
	if (ok)
		*ok = FALSE;
	return IV_SORT_NAME;
}
