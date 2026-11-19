// LikeOS -- LikeOS Image Viewer: writing pictures.
//
// Every save goes through a temporary file in the target's own folder and
// a rename() over the target, so a full disk or a crash half-way leaves the
// old file intact.  The new file keeps the old one's permissions (and, when
// asked, its modification time).  JPEG files can carry the camera data of
// the picture they were made from: the source's EXIF block is copied into
// the new file by ivproc, with its orientation reset (the pixels are saved
// the way they are shown) and its sizes corrected.
//
// Rotating a file: a JPEG with an orientation tag is turned without
// touching its pixels -- only the tag changes, so nothing is lost however
// often it is turned.  One without any EXIF block gets a small one with the
// tag.  Everything else is decoded, turned and written again.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "imageviewer.h"

void iv_save_options_init(IvSaveOptions *o, IvFormat f)
{
	IvSettings *s = iv_settings_get();

	memset(o, 0, sizeof *o);
	o->format = f;
	o->quality = s->jpeg_quality;
	o->keep_exif = s->keep_exif;
}

void iv_save_options_clear(IvSaveOptions *o)
{
	g_free(o->exif_from);
	o->exif_from = NULL;
}

static void copy_options(IvSaveOptions *dst, const IvSaveOptions *src)
{
	*dst = *src;
	dst->exif_from = g_strdup(src->exif_from);
}

/* The picture over white, for formats without transparency. */
static GdkPixbuf *flatten(GdkPixbuf *pb)
{
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	GdkPixbuf *out;

	if (!gdk_pixbuf_get_has_alpha(pb))
		return g_object_ref(pb);
	out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
	if (!out)
		return NULL;
	gdk_pixbuf_fill(out, 0xffffffff);
	gdk_pixbuf_composite(pb, out, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_NEAREST,
			     255);
	return out;
}

static gboolean encode_to(GdkPixbuf *pb, const char *path,
			  const IvSaveOptions *o, GError **error)
{
	char q[8];
	gboolean ok = FALSE;
	GdkPixbuf *flat;

	g_snprintf(q, sizeof q, "%d", CLAMP(o->quality, 1, 100));
	switch (o->format) {
	case IV_FORMAT_PNG:
		return gdk_pixbuf_save(pb, path, "png", error, "compression", "6",
				       NULL);
	case IV_FORMAT_JPEG:
		flat = flatten(pb);
		if (!flat) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
					    "Out of memory");
			return FALSE;
		}
		ok = gdk_pixbuf_save(flat, path, "jpeg", error, "quality", q, NULL);
		g_object_unref(flat);
		if (ok && o->keep_exif && o->exif_from) {
			uint8_t *app1 = NULL;
			size_t len = 0;

			/* No EXIF in the source is not an error: there is
			 * just nothing to keep. */
			if (iv_jpeg_get_app1(o->exif_from, &app1, &len) == 0) {
				if (iv_jpeg_write_with_app1(path, app1, len, 1) != 0)
					g_warning("the camera data could not be "
						  "copied to %s", path);
				free(app1);
			}
		}
		return ok;
	case IV_FORMAT_BMP:
		flat = flatten(pb);
		if (!flat)
			return FALSE;
		ok = gdk_pixbuf_save(flat, path, "bmp", error, NULL);
		g_object_unref(flat);
		return ok;
	case IV_FORMAT_TIFF:
		return gdk_pixbuf_save(pb, path, "tiff", error, NULL);
	case IV_FORMAT_WEBP: {
		IvImage *img = iv_image_from_pixbuf(pb);
		int quality = o->quality > 100 ? 101 : CLAMP(o->quality, 1, 100);

		ok = img && iv_webp_save(img, path, quality) == 0;
		iv_image_free(img);
		if (!ok)
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
					    "The WebP picture could not be "
					    "written");
		return ok;
	}
	default:
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "Pictures cannot be saved in this format");
		return FALSE;
	}
}

gboolean iv_save_pixbuf(GdkPixbuf *pb, const char *path,
			const IvSaveOptions *o, GError **error)
{
	char *dir = g_path_get_dirname(path);
	char *base = g_path_get_basename(path);
	char *tmpl = g_strdup_printf("%s/.%s.XXXXXX", dir, base);
	struct stat old;
	gboolean had_old = stat(path, &old) == 0;
	gboolean ok = FALSE;
	int fd;

	fd = g_mkstemp_full(tmpl, O_WRONLY, 0600);
	if (fd < 0) {
		int err = errno;

		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err),
			    "Cannot write in “%s”: %s", dir, g_strerror(err));
		goto out;
	}
	close(fd);
	if (!encode_to(pb, tmpl, o, error)) {
		unlink(tmpl);
		goto out;
	}
	/* The new file gets the old one's permissions, or the usual ones
	 * for a new file (mkstemp made it private). */
	if (had_old) {
		chmod(tmpl, old.st_mode & 07777);
	} else {
		mode_t mask = umask(0);

		umask(mask);
		chmod(tmpl, 0666 & ~mask);
	}
	if (o->keep_mtime && had_old) {
		struct utimbuf t = { .actime = old.st_atime,
				     .modtime = old.st_mtime };

		utime(tmpl, &t);
	}
	if (rename(tmpl, path) != 0) {
		int err = errno;

		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err),
			    "Cannot replace “%s”: %s", base, g_strerror(err));
		unlink(tmpl);
		goto out;
	}
	ok = TRUE;
out:
	g_free(tmpl);
	g_free(base);
	g_free(dir);
	return ok;
}

/* ---- off the main thread --------------------------------------------------- */

typedef struct {
	GdkPixbuf *pb;
	char *path;
	IvSaveOptions o;
} SaveJob;

static void save_job_free(gpointer data)
{
	SaveJob *j = data;

	g_object_unref(j->pb);
	g_free(j->path);
	iv_save_options_clear(&j->o);
	g_free(j);
}

static void save_thread(GTask *task, gpointer src, gpointer data,
			GCancellable *c)
{
	SaveJob *j = data;
	GError *e = NULL;

	if (g_task_return_error_if_cancelled(task))
		return;
	if (iv_save_pixbuf(j->pb, j->path, &j->o, &e))
		g_task_return_boolean(task, TRUE);
	else
		g_task_return_error(task, e);
}

void iv_save_pixbuf_async(GdkPixbuf *pb, const char *path,
			  const IvSaveOptions *o, GCancellable *c,
			  GAsyncReadyCallback cb, gpointer data)
{
	GTask *t = g_task_new(NULL, c, cb, data);
	SaveJob *j = g_new0(SaveJob, 1);

	j->pb = g_object_ref(pb);
	j->path = g_strdup(path);
	copy_options(&j->o, o);
	g_task_set_task_data(t, j, save_job_free);
	g_task_run_in_thread(t, save_thread);
	g_object_unref(t);
}

gboolean iv_save_finish(GAsyncResult *res, GError **error)
{
	return g_task_propagate_boolean(G_TASK(res), error);
}

/* ---- estimating ----------------------------------------------------------- */

goffset iv_save_estimate(GdkPixbuf *pb, int w, int h, const IvSaveOptions *o)
{
	/* A sample of at most a megapixel, encoded in memory; the size
	 * scales with the pixel count. */
	const double sample_px = 1000000.0;
	double k = 1.0;
	int sw = w, sh = h;
	GdkPixbuf *sample;
	gchar *buf = NULL;
	gsize len = 0;
	goffset est = 0;
	char q[8];

	if (!pb || w <= 0 || h <= 0)
		return 0;
	if ((double)w * h > sample_px) {
		k = sqrt(sample_px / ((double)w * h));
		sw = MAX(1, (int)(w * k));
		sh = MAX(1, (int)(h * k));
	}
	sample = gdk_pixbuf_scale_simple(pb, sw, sh, GDK_INTERP_BILINEAR);
	if (!sample)
		return 0;
	g_snprintf(q, sizeof q, "%d", CLAMP(o->quality, 1, 100));
	switch (o->format) {
	case IV_FORMAT_PNG:
		gdk_pixbuf_save_to_buffer(sample, &buf, &len, "png", NULL,
					  "compression", "6", NULL);
		break;
	case IV_FORMAT_JPEG: {
		GdkPixbuf *flat = flatten(sample);

		if (flat)
			gdk_pixbuf_save_to_buffer(flat, &buf, &len, "jpeg", NULL,
						  "quality", q, NULL);
		g_clear_object(&flat);
		break;
	}
	case IV_FORMAT_BMP:
		/* Uncompressed: known exactly. */
		est = (goffset)54 + (goffset)((w * 3 + 3) & ~3) * h;
		break;
	case IV_FORMAT_TIFF:
		gdk_pixbuf_save_to_buffer(sample, &buf, &len, "tiff", NULL, NULL);
		break;
	case IV_FORMAT_WEBP: {
		IvImage *img = iv_image_from_pixbuf(sample);
		uint8_t *out = NULL;
		size_t n = 0;

		if (img && iv_webp_encode_mem(img, o->quality > 100 ? 101 :
								    o->quality,
					      &out, &n) == 0)
			len = n;
		free(out);
		iv_image_free(img);
		break;
	}
	default:
		break;
	}
	if (!est && len)
		est = (goffset)(len / (k * k));
	g_free(buf);
	g_object_unref(sample);
	return est;
}

/* ---- rotating files ----------------------------------------------------- */

static guint16 rd16(const uint8_t *p, gboolean le)
{
	return le ? (guint16)(p[0] | p[1] << 8) : (guint16)(p[0] << 8 | p[1]);
}

static guint32 rd32(const uint8_t *p, gboolean le)
{
	return le ? (guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16 |
			    (guint32)p[3] << 24 :
		    (guint32)p[0] << 24 | (guint32)p[1] << 16 |
			    (guint32)p[2] << 8 | (guint32)p[3];
}

static void wr16(uint8_t *p, guint16 v, gboolean le)
{
	if (le) {
		p[0] = v & 0xff;
		p[1] = v >> 8;
	} else {
		p[0] = v >> 8;
		p[1] = v & 0xff;
	}
}

/* Sets the Orientation tag (0x0112) of IFD0 in an APP1 payload.  FALSE
 * when the payload has no such tag (it is not added: that would move
 * everything after it). */
static gboolean app1_set_orientation(uint8_t *app1, size_t len, int orient)
{
	const uint8_t *tiff;
	size_t tlen;
	gboolean le;
	guint32 ifd;
	guint16 n;

	if (len < 6 + 8 || memcmp(app1, "Exif\0\0", 6) != 0)
		return FALSE;
	tiff = app1 + 6;
	tlen = len - 6;
	if (!memcmp(tiff, "II*\0", 4))
		le = TRUE;
	else if (!memcmp(tiff, "MM\0*", 4))
		le = FALSE;
	else
		return FALSE;
	ifd = rd32(tiff + 4, le);
	if (ifd < 8 || (size_t)ifd + 2 > tlen)
		return FALSE;
	n = rd16(tiff + ifd, le);
	if ((size_t)ifd + 2 + (size_t)n * 12 > tlen)
		return FALSE;
	for (guint16 i = 0; i < n; i++) {
		uint8_t *e = (uint8_t *)tiff + ifd + 2 + (size_t)i * 12;

		if (rd16(e, le) != 0x0112)
			continue;
		if (rd16(e + 2, le) != 3 || rd32(e + 4, le) != 1)
			return FALSE;
		wr16(e + 8, (guint16)orient, le);
		return TRUE;
	}
	return FALSE;
}

/* The smallest EXIF block there is: IFD0 with the Orientation tag. */
static uint8_t *app1_minimal(int orient, size_t *len)
{
	static const uint8_t head[] = {
		'E', 'x', 'i', 'f', 0, 0,	/* the APP1 identifier */
		'I', 'I', 42, 0, 8, 0, 0, 0,	/* little-endian TIFF, IFD0 at 8 */
		1, 0,				/* one entry */
		0x12, 0x01, 3, 0, 1, 0, 0, 0,	/* Orientation, SHORT, 1 */
		0, 0, 0, 0,			/* its value (set below) */
		0, 0, 0, 0,			/* no IFD1 */
	};
	uint8_t *p = g_memdup2(head, sizeof head);

	p[6 + 8 + 2 + 8] = (uint8_t)orient;
	*len = sizeof head;
	return p;
}

static gboolean rotate_jpeg_tag(const char *path, IvPicture *shown, int turns,
				gboolean *done, GError **error)
{
	uint8_t *app1 = NULL;
	size_t len = 0;
	int cur = shown && shown->orientation ? shown->orientation : 1;
	int next = iv_orientation_rotate(cur, turns);
	gboolean ok;

	*done = FALSE;
	if (iv_jpeg_get_app1(path, &app1, &len) == 0) {
		if (!app1_set_orientation(app1, len, next)) {
			free(app1);
			return TRUE; /* no tag to change: re-encode instead */
		}
		ok = iv_jpeg_write_with_app1(path, app1, len, 0) == 0;
		free(app1);
	} else {
		uint8_t *mini = app1_minimal(next, &len);

		ok = iv_jpeg_write_with_app1(path, mini, len, 0) == 0;
		g_free(mini);
	}
	if (!ok) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			    "The orientation of “%s” could not be written",
			    shown ? shown->name : path);
		return FALSE;
	}
	*done = TRUE;
	return TRUE;
}

gboolean iv_rotate_file(const char *path, IvPicture *shown, int quarter_turns,
			GError **error)
{
	char *mime = iv_mime_for_path(path);
	IvFormat f = iv_format_for_mime(mime);
	int turns = ((quarter_turns % 4) + 4) % 4;
	gboolean ok = FALSE, done = FALSE;
	GdkPixbuf *turned = NULL;
	IvSaveOptions o;

	g_free(mime);
	if (turns == 0)
		return TRUE;
	if (f == IV_FORMAT_NONE || (shown && shown->anim)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "Pictures in this format cannot be turned in "
				    "place; use Save as to keep a turned copy");
		return FALSE;
	}
	if (f == IV_FORMAT_JPEG) {
		if (!rotate_jpeg_tag(path, shown, turns, &done, error))
			return FALSE;
		if (done)
			return TRUE;
	}
	if (!shown || !shown->pixbuf || shown->reduced) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "The picture is not loaded at full size");
		return FALSE;
	}
	switch (turns) {
	case 1:
		turned = gdk_pixbuf_rotate_simple(shown->pixbuf,
						  GDK_PIXBUF_ROTATE_CLOCKWISE);
		break;
	case 2:
		turned = gdk_pixbuf_rotate_simple(shown->pixbuf,
						  GDK_PIXBUF_ROTATE_UPSIDEDOWN);
		break;
	default:
		turned = gdk_pixbuf_rotate_simple(shown->pixbuf,
						  GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
		break;
	}
	iv_save_options_init(&o, f);
	o.exif_from = g_strdup(path);
	/* A lossless WebP stays lossless only if it was; the quality
	 * setting is what the user chose for lossy files. */
	ok = turned && iv_save_pixbuf(turned, path, &o, error);
	iv_save_options_clear(&o);
	g_clear_object(&turned);
	return ok;
}
