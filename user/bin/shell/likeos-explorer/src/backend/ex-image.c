// LikeOS Explorer: turning pictures.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-image.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#define TAG_ORIENTATION 0x0112
#define TYPE_SHORT 3

gboolean ex_image_can_rotate(const char *ct)
{
	return ct && (g_content_type_is_a(ct, "image/jpeg") ||
		      g_content_type_is_a(ct, "image/png") ||
		      g_content_type_is_a(ct, "image/bmp") ||
		      g_content_type_is_a(ct, "image/x-bmp") ||
		      g_content_type_is_a(ct, "image/tiff"));
}

/* Each orientation turned a quarter clockwise: the picture as shown is
 * turned, whatever mirroring the tag already asks for. */
int ex_image_orientation_turn(int o, gboolean clockwise)
{
	static const int cw[9] = { 1, 6, 7, 8, 5, 2, 3, 4, 1 };
	int turns = clockwise ? 1 : 3;

	if (o < 1 || o > 8)
		o = 1;
	while (turns--)
		o = cw[o];
	return o;
}

/* ---- the Exif data of a JPEG ---------------------------------------------------- */

static guint rd16(const guchar *p, gboolean be)
{
	return be ? (guint)(p[0] << 8 | p[1]) : (guint)(p[1] << 8 | p[0]);
}

static guint32 rd32(const guchar *p, gboolean be)
{
	return be ? (guint32)p[0] << 24 | (guint32)p[1] << 16 |
			    (guint32)p[2] << 8 | p[3] :
		    (guint32)p[3] << 24 | (guint32)p[2] << 16 |
			    (guint32)p[1] << 8 | p[0];
}

typedef struct {
	gsize after_soi; /* where a new Exif segment goes */
	gboolean has_exif;
	gsize exif_pos, exif_seg; /* the Exif segment: its marker, its length */
	gsize orient; /* offset of the Orientation value, 0 when none */
	gboolean be; /* the Exif data's byte order */
} JpegScan;

/* Walk the segments before the picture data, looking for the Exif one and
 * the Orientation tag in its first directory. */
static gboolean scan_jpeg(const guchar *d, gsize len, JpegScan *s)
{
	gsize pos = 2;

	memset(s, 0, sizeof(*s));
	if (len < 4 || d[0] != 0xff || d[1] != 0xd8)
		return FALSE;
	s->after_soi = 2;
	while (pos + 4 <= len) {
		guint marker, seg;
		gsize avail;

		if (d[pos] != 0xff)
			return FALSE;
		marker = d[pos + 1];
		if (marker == 0xff) {
			pos++;
			continue;
		}
		if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) {
			pos += 2;
			continue;
		}
		if (marker == 0xda || marker == 0xd9)
			break;
		seg = rd16(d + pos + 2, TRUE);
		if (seg < 2)
			return FALSE;
		/* Only the start of the file may have been read: a segment
		 * running past it is looked into as far as it goes. */
		avail = MIN((gsize)seg, len - pos - 2);
		/* A new Exif segment goes after a JFIF one, which must stay
		 * first. */
		if (marker == 0xe0 && pos == 2)
			s->after_soi = pos + 2 + seg;
		if (marker == 0xe1 && !s->has_exif && avail >= 2 + 6 + 8 &&
		    memcmp(d + pos + 4, "Exif\0\0", 6) == 0) {
			const guchar *t = d + pos + 10;
			gsize tl = avail - 8;
			gboolean be = t[0] == 'M';
			guint32 ifd;
			guint n;

			s->has_exif = TRUE;
			s->exif_pos = pos;
			s->exif_seg = seg;
			s->be = be;
			if ((t[0] != 'M' && t[0] != 'I') || t[1] != t[0] ||
			    rd16(t + 2, be) != 42)
				return TRUE;
			ifd = rd32(t + 4, be);
			if ((gsize)ifd + 2 > tl)
				return TRUE;
			n = rd16(t + ifd, be);
			for (guint i = 0; i < n; i++) {
				gsize e = (gsize)ifd + 2 + 12 * (gsize)i;

				if (e + 12 > tl)
					break;
				if (rd16(t + e, be) == TAG_ORIENTATION &&
				    rd16(t + e + 2, be) == TYPE_SHORT) {
					s->orient = (gsize)(t - d) + e + 8;
					break;
				}
			}
		}
		if (pos + 2 + seg > len)
			break;
		pos += 2 + seg;
	}
	return TRUE;
}

static void wr16(guchar *p, guint v, gboolean be)
{
	p[be ? 0 : 1] = (guchar)(v >> 8);
	p[be ? 1 : 0] = (guchar)v;
}

static void wr32(guchar *p, guint32 v, gboolean be)
{
	for (int i = 0; i < 4; i++)
		p[be ? i : 3 - i] = (guchar)(v >> (24 - 8 * i));
}

/* ---- adding the tag to Exif data that lacks it ------------------------------------ */

/* The size of one value of a TIFF field type, 0 for a type not known. */
static guint type_size(guint type)
{
	switch (type) {
	case 1: case 2: case 6: case 7: /* BYTE, ASCII, SBYTE, UNDEFINED */
		return 1;
	case 3: case 8: /* SHORT, SSHORT */
		return 2;
	case 4: case 9: case 11: case 13: /* LONG, SLONG, FLOAT, IFD */
		return 4;
	case 5: case 10: case 12: /* RATIONAL, SRATIONAL, DOUBLE */
		return 8;
	}
	return 0;
}

#define TAG_EXIF_IFD 0x8769
#define TAG_GPS_IFD 0x8825
#define TAG_INTEROP_IFD 0xa005
#define TAG_STRIP_OFFSETS 0x0111
#define TAG_JPEG_THUMB 0x0201
#define MAX_IFDS 16

/* Every place in the TIFF data that holds an offset into it: where it is,
 * and whether it is 2 or 4 bytes wide. */
typedef struct {
	const guchar *t;
	gsize tl;
	gboolean be;
	GArray *fields; /* Field */
	guint32 ifds[MAX_IFDS];
	int n_ifds;
} Walk;

typedef struct {
	gsize pos;
	guint width;
} Field;

static void add_field(Walk *w, gsize pos, guint width)
{
	Field f = { pos, width };

	g_array_append_val(w->fields, f);
}

/* Record the offsets of one directory -- its entries' values stored
 * elsewhere, the sub-directories it points to (walked in turn), the
 * thumbnail's place -- and its pointer to the next one.  FALSE when the
 * data is not what the TIFF format says it must be. */
static gboolean walk_ifd(Walk *w, guint32 ifd, gboolean follow_next)
{
	guint n;

	if (ifd == 0)
		return TRUE;
	for (int i = 0; i < w->n_ifds; i++)
		if (w->ifds[i] == ifd)
			return FALSE; /* a directory reached twice: a loop */
	if (w->n_ifds == MAX_IFDS || (gsize)ifd + 2 > w->tl || (ifd & 1))
		return FALSE;
	w->ifds[w->n_ifds++] = ifd;
	n = rd16(w->t + ifd, w->be);
	if ((gsize)ifd + 2 + 12 * (gsize)n + 4 > w->tl)
		return FALSE;
	for (guint i = 0; i < n; i++) {
		gsize e = (gsize)ifd + 2 + 12 * (gsize)i;
		guint tag = rd16(w->t + e, w->be);
		guint type = rd16(w->t + e + 2, w->be);
		guint32 count = rd32(w->t + e + 4, w->be);
		guint size = type_size(type);
		guint64 bytes = (guint64)size * count;

		if (!size)
			return FALSE;
		if (bytes > 4) {
			guint32 off = rd32(w->t + e + 8, w->be);

			if ((guint64)off + bytes > w->tl)
				return FALSE;
			add_field(w, e + 8, 4);
			/* Strip offsets stored elsewhere: each one too. */
			if (tag == TAG_STRIP_OFFSETS && (size == 2 || size == 4))
				for (guint32 k = 0; k < count; k++)
					add_field(w, off + (gsize)k * size, size);
		} else if ((tag == TAG_EXIF_IFD || tag == TAG_GPS_IFD ||
			    tag == TAG_INTEROP_IFD) &&
			   count == 1 && size == 4) {
			add_field(w, e + 8, 4);
			if (!walk_ifd(w, rd32(w->t + e + 8, w->be), FALSE))
				return FALSE;
		} else if ((tag == TAG_JPEG_THUMB || tag == TAG_STRIP_OFFSETS) &&
			   (size == 2 || size == 4)) {
			for (guint32 k = 0; k < count; k++)
				add_field(w, e + 8 + (gsize)k * size, size);
		}
	}
	/* The pointer to the next directory (IFD0 to IFD1, the thumbnail's). */
	{
		gsize next = (gsize)ifd + 2 + 12 * (gsize)n;
		guint32 nx = rd32(w->t + next, w->be);

		add_field(w, next, 4);
		if (follow_next && nx && !walk_ifd(w, nx, FALSE))
			return FALSE;
	}
	return TRUE;
}

/* A new Exif segment like the one at s->exif_pos with an Orientation
 * entry added to its first directory, in tag order: the 12 bytes go into
 * the directory, so every offset pointing past them moves on by 12.  The
 * compressed picture is not part of the segment and stays as it is.  NULL
 * when the data is malformed or the segment would grow past what a JPEG
 * segment can hold.  (A maker note's private offsets, if a camera wrote
 * any relative to the TIFF data, cannot be known and are left as they are,
 * as every Exif editor leaves them.) */
static GByteArray *exif_with_orientation(const guchar *d, const JpegScan *s,
					 int orientation)
{
	const guchar *t = d + s->exif_pos + 10;
	gsize tl = s->exif_seg - 8;
	gboolean be = s->be;
	Walk w = { t, tl, be, NULL, { 0 }, 0 };
	guint32 ifd0;
	guint n, at = 0;
	gsize ins;
	GByteArray *out = NULL;

	if ((t[0] != 'M' && t[0] != 'I') || t[1] != t[0] || rd16(t + 2, be) != 42 ||
	    s->exif_seg + 12 > 0xffff)
		return NULL;
	ifd0 = rd32(t + 4, be);
	w.fields = g_array_new(FALSE, FALSE, sizeof(Field));
	if (ifd0 < 8 || !walk_ifd(&w, ifd0, TRUE))
		goto out;
	n = rd16(t + ifd0, be);
	while (at < n && rd16(t + ifd0 + 2 + 12 * at, be) < TAG_ORIENTATION)
		at++;
	/* A tag of that number in a form not understood: left alone. */
	if (at < n && rd16(t + ifd0 + 2 + 12 * at, be) == TAG_ORIENTATION)
		goto out;
	ins = (gsize)ifd0 + 2 + 12 * (gsize)at;

	out = g_byte_array_sized_new((guint)(s->exif_seg + 2 + 12));
	{
		guchar head[4] = { 0xff, 0xe1, 0, 0 };
		guchar entry[12] = { 0 };

		head[2] = (guchar)((s->exif_seg + 12) >> 8);
		head[3] = (guchar)(s->exif_seg + 12);
		g_byte_array_append(out, head, 4);
		g_byte_array_append(out, d + s->exif_pos + 4, 6); /* "Exif\0\0" */
		g_byte_array_append(out, t, (guint)ins);
		wr16(entry, TAG_ORIENTATION, be);
		wr16(entry + 2, TYPE_SHORT, be);
		wr32(entry + 4, 1, be);
		wr16(entry + 8, (guint)orientation, be);
		g_byte_array_append(out, entry, 12);
		g_byte_array_append(out, t + ins, (guint)(tl - ins));
	}
	{
		guchar *nt = out->data + 10;

		wr16(nt + ifd0, n + 1, be);
		for (guint i = 0; i < w.fields->len; i++) {
			Field *f = &g_array_index(w.fields, Field, i);
			gsize np = f->pos >= ins ? f->pos + 12 : f->pos;
			guint32 v = f->width == 4 ? rd32(nt + np, be) :
						    rd16(nt + np, be);

			if (v < ins)
				continue;
			v += 12;
			if (f->width == 4)
				wr32(nt + np, v, be);
			else if (v <= 0xffff)
				wr16(nt + np, v, be);
			else {
				g_byte_array_unref(out);
				out = NULL;
				goto out;
			}
		}
	}
out:
	g_array_unref(w.fields);
	return out;
}

int ex_image_jpeg_orientation(const char *path)
{
	/* The Exif data sits in the first 64 KB of the file. */
	guchar buf[65536];
	JpegScan s;
	FILE *fp = fopen(path, "rb");
	gsize len;
	int o;

	if (!fp)
		return 1;
	len = fread(buf, 1, sizeof(buf), fp);
	fclose(fp);
	if (!scan_jpeg(buf, len, &s) || !s.orient)
		return 1;
	o = (int)rd16(buf + s.orient, s.be);
	return o >= 1 && o <= 8 ? o : 1;
}

/* A minimal Exif segment: the TIFF header and one directory holding the
 * Orientation alone. */
static GBytes *exif_segment(int orientation)
{
	static const guchar head[] = {
		0xff, 0xe1, 0x00, 0x22, /* APP1, length 34 */
		'E', 'x', 'i', 'f', 0, 0,
		'M', 'M', 0x00, 0x2a, 0x00, 0x00, 0x00, 0x08, /* TIFF, IFD at 8 */
		0x00, 0x01, /* one entry */
		0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, /* Orientation */
	};
	guchar seg[sizeof(head) + 8];

	memcpy(seg, head, sizeof(head));
	seg[sizeof(head)] = 0;
	seg[sizeof(head) + 1] = (guchar)orientation;
	memset(seg + sizeof(head) + 2, 0, 6); /* padding, no next IFD */
	return g_bytes_new(seg, sizeof(seg));
}

static gboolean rotate_jpeg(GFile *file, gboolean clockwise, GCancellable *c,
			    GError **error)
{
	char *data = NULL;
	gsize len = 0;
	JpegScan s;
	gboolean ok;

	if (!g_file_load_contents(file, c, &data, &len, NULL, error))
		return FALSE;
	if (!scan_jpeg((guchar *)data, len, &s)) {
		g_free(data);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				    "The picture is not a valid JPEG file.");
		return FALSE;
	}
	if (s.orient) {
		guchar *v = (guchar *)data + s.orient;
		int o = ex_image_orientation_turn((int)rd16(v, s.be), clockwise);

		v[s.be ? 0 : 1] = 0;
		v[s.be ? 1 : 0] = (guchar)o;
		ok = g_file_replace_contents(file, data, len, NULL, FALSE,
					     G_FILE_CREATE_NONE, NULL, c, error);
	} else if (!s.has_exif) {
		GBytes *seg = exif_segment(ex_image_orientation_turn(1, clockwise));
		gsize sl;
		const guchar *sd = g_bytes_get_data(seg, &sl);
		GByteArray *out = g_byte_array_sized_new((guint)(len + sl));

		g_byte_array_append(out, (guchar *)data, (guint)s.after_soi);
		g_byte_array_append(out, sd, (guint)sl);
		g_byte_array_append(out, (guchar *)data + s.after_soi,
				    (guint)(len - s.after_soi));
		ok = g_file_replace_contents(file, (char *)out->data, out->len,
					     NULL, FALSE, G_FILE_CREATE_NONE,
					     NULL, c, error);
		g_byte_array_unref(out);
		g_bytes_unref(seg);
	} else {
		/* Exif data without the tag: the segment is written again with
		 * the tag added. */
		GByteArray *seg = s.exif_pos + 2 + s.exif_seg <= len ?
					  exif_with_orientation(
						  (guchar *)data, &s,
						  ex_image_orientation_turn(1, clockwise)) :
					  NULL;

		if (seg) {
			GByteArray *out = g_byte_array_sized_new(
				(guint)(len + seg->len));
			gsize after = s.exif_pos + 2 + s.exif_seg;

			g_byte_array_append(out, (guchar *)data, (guint)s.exif_pos);
			g_byte_array_append(out, seg->data, seg->len);
			g_byte_array_append(out, (guchar *)data + after,
					    (guint)(len - after));
			ok = g_file_replace_contents(file, (char *)out->data,
						     out->len, NULL, FALSE,
						     G_FILE_CREATE_NONE, NULL, c,
						     error);
			g_byte_array_unref(out);
			g_byte_array_unref(seg);
		} else {
			g_set_error_literal(error, G_IO_ERROR,
					    G_IO_ERROR_NOT_SUPPORTED,
					    "This picture's information cannot "
					    "be read, so it is not rotated.");
			ok = FALSE;
		}
	}
	g_free(data);
	return ok;
}

/* A lossless format: turned and written again in the same format. */
static gboolean rotate_pixels(GFile *file, const char *type, gboolean clockwise,
			      GCancellable *c, GError **error)
{
	char *path = g_file_get_path(file);
	GdkPixbuf *pb, *turned;
	char *buf = NULL;
	gsize len = 0;
	gboolean ok;

	if (!path) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "Only pictures on this computer can be "
				    "rotated.");
		return FALSE;
	}
	pb = gdk_pixbuf_new_from_file(path, error);
	g_free(path);
	if (!pb)
		return FALSE;
	turned = gdk_pixbuf_rotate_simple(
		pb, clockwise ? GDK_PIXBUF_ROTATE_CLOCKWISE :
				GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
	g_object_unref(pb);
	ok = turned && gdk_pixbuf_save_to_buffer(turned, &buf, &len, type, error,
						 NULL);
	g_clear_object(&turned);
	if (ok)
		ok = g_file_replace_contents(file, buf, len, NULL, FALSE,
					     G_FILE_CREATE_NONE, NULL, c, error);
	g_free(buf);
	return ok;
}

gboolean ex_image_rotate(GFile *file, gboolean clockwise, GCancellable *c,
			 GError **error)
{
	GFileInfo *info = g_file_query_info(
		file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
		G_FILE_QUERY_INFO_NONE, c, error);
	const char *ct;
	gboolean ok;

	if (!info)
		return FALSE;
	ct = g_file_info_get_content_type(info);
	if (ct && g_content_type_is_a(ct, "image/jpeg"))
		ok = rotate_jpeg(file, clockwise, c, error);
	else if (ct && g_content_type_is_a(ct, "image/png"))
		ok = rotate_pixels(file, "png", clockwise, c, error);
	else if (ct && (g_content_type_is_a(ct, "image/bmp") ||
			g_content_type_is_a(ct, "image/x-bmp")))
		ok = rotate_pixels(file, "bmp", clockwise, c, error);
	else if (ct && g_content_type_is_a(ct, "image/tiff"))
		ok = rotate_pixels(file, "tiff", clockwise, c, error);
	else {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    "Pictures of this type cannot be rotated.");
		ok = FALSE;
	}
	g_object_unref(info);
	return ok;
}

static void rotate_thread(GTask *task, gpointer src, gpointer data,
			  GCancellable *c)
{
	GError *e = NULL;

	if (ex_image_rotate(G_FILE(src), GPOINTER_TO_INT(data), c, &e))
		g_task_return_boolean(task, TRUE);
	else
		g_task_return_error(task, e);
}

void ex_image_rotate_async(GFile *file, gboolean clockwise, GCancellable *c,
			   GAsyncReadyCallback done, gpointer user_data)
{
	GTask *task = g_task_new(file, c, done, user_data);

	g_task_set_task_data(task, GINT_TO_POINTER(clockwise), NULL);
	g_task_run_in_thread(task, rotate_thread);
	g_object_unref(task);
}

gboolean ex_image_rotate_finish(GAsyncResult *res, GError **error)
{
	return g_task_propagate_boolean(G_TASK(res), error);
}
