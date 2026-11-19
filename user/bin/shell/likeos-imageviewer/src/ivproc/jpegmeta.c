// LikeOS -- LikeOS Image Viewer: carrying EXIF metadata over to a newly
// written JPEG.
//
// The image is saved by the toolkit's encoder, which writes no EXIF.  The
// original's APP1 segment is taken out beforehand (iv_jpeg_get_app1) and
// put back into the new file afterwards (iv_jpeg_write_with_app1), with
// the few tags that the edit made untrue patched in place: Orientation
// (the pixels are now upright), PixelX/YDimension (the size may have
// changed) and IFD1 (the embedded thumbnail shows the old pixels: it is
// unlinked and its bytes zeroed).  Nothing is re-serialised, so maker notes
// and every other tag survive byte for byte.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

#define IVP_JPEG_MAX ((size_t)1 << 30)

/* A marker segment inside a JPEG buffer. */
typedef struct {
	unsigned marker;
	size_t start;		/* position of the 0xFF */
	size_t end;		/* one past the segment */
	size_t pay, plen;	/* payload */
} Seg;

/*
 * Walk the header segments of a JPEG in memory, up to (not including)
 * SOS.  Calls fn for each one; *sos receives the position of the SOS
 * marker (or the end of the buffer when there is none).  Returns 0, or -1
 * for something that is not a well-formed JPEG header.
 */
typedef int (*SegFn)(const uint8_t *d, const Seg *s, void *user);

static int walk(const uint8_t *d, size_t n, SegFn fn, void *user, size_t *sos)
{
	if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
		return -1;
	size_t off = 2;
	while (off + 1 < n) {
		if (d[off] != 0xFF)
			return -1;
		size_t start = off;
		while (off < n && d[off] == 0xFF)
			off++;
		if (off >= n)
			return -1;
		unsigned m = d[off++];
		if (m == 0xDA || m == 0xD9) {
			*sos = start;
			return 0;
		}
		if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
			Seg s = { m, start, off, off, 0 };
			if (fn && fn(d, &s, user) < 0)
				return -1;
			continue;
		}
		if (off + 2 > n)
			return -1;
		size_t len = ((size_t)d[off] << 8) | d[off + 1];
		if (len < 2 || off + len > n)
			return -1;
		Seg s = { m, start, off + len, off + 2, len - 2 };
		if (fn && fn(d, &s, user) < 0)
			return -1;
		off += len;
	}
	*sos = n;
	return 0;
}

static int is_exif_app1(const uint8_t *d, const Seg *s)
{
	return s->marker == 0xE1 && s->plen >= 6 &&
	       memcmp(d + s->pay, "Exif\0\0", 6) == 0;
}

typedef struct {
	Seg seg;
	int have;
} FindCtx;

static int find_app1(const uint8_t *d, const Seg *s, void *user)
{
	FindCtx *c = user;
	if (!c->have && is_exif_app1(d, s)) {
		c->seg = *s;
		c->have = 1;
	}
	return 0;
}

int iv_jpeg_get_app1(const char *path, uint8_t **data, size_t *len)
{
	if (path == NULL || data == NULL || len == NULL)
		return -1;
	*data = NULL;
	*len = 0;
	size_t n = 0;
	uint8_t *buf = ivp_read_file(path, IVP_JPEG_MAX, &n);
	if (buf == NULL)
		return -1;
	FindCtx c;
	memset(&c, 0, sizeof(c));
	size_t sos = 0;
	int r = -1;
	if (walk(buf, n, find_app1, &c, &sos) == 0 && c.have) {
		*data = malloc(c.seg.plen);
		if (*data != NULL) {
			memcpy(*data, buf + c.seg.pay, c.seg.plen);
			*len = c.seg.plen;
			r = 0;
		}
	}
	free(buf);
	return r;
}

/* ------------------------------------------------------------------------
 * In-place TIFF patching
 * --------------------------------------------------------------------- */

typedef struct {
	uint8_t *t;
	size_t n;
	int le;
} Patch;

static int p_have(const Patch *p, size_t off, size_t len)
{
	return off <= p->n && len <= p->n - off;
}

static unsigned p_u16(const Patch *p, size_t off)
{
	const uint8_t *b = p->t + off;
	return p->le ? (unsigned)(b[0] | (b[1] << 8)) : (unsigned)((b[0] << 8) | b[1]);
}

static uint32_t p_u32(const Patch *p, size_t off)
{
	const uint8_t *b = p->t + off;
	return p->le ? ((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
			((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)) :
		       (((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
			((uint32_t)b[2] << 8) | (uint32_t)b[3]);
}

static void p_w16(Patch *p, size_t off, unsigned v)
{
	uint8_t *b = p->t + off;
	if (p->le) {
		b[0] = (uint8_t)v;
		b[1] = (uint8_t)(v >> 8);
	} else {
		b[0] = (uint8_t)(v >> 8);
		b[1] = (uint8_t)v;
	}
}

static void p_w32(Patch *p, size_t off, uint32_t v)
{
	uint8_t *b = p->t + off;
	if (p->le) {
		b[0] = (uint8_t)v;
		b[1] = (uint8_t)(v >> 8);
		b[2] = (uint8_t)(v >> 16);
		b[3] = (uint8_t)(v >> 24);
	} else {
		b[0] = (uint8_t)(v >> 24);
		b[1] = (uint8_t)(v >> 16);
		b[2] = (uint8_t)(v >> 8);
		b[3] = (uint8_t)v;
	}
}

/* Overwrite the single inline SHORT/LONG value of an entry. */
static void patch_inline(Patch *p, size_t ent, uint32_t v)
{
	unsigned type = p_u16(p, ent + 2);
	uint32_t count = p_u32(p, ent + 4);
	if (count != 1)
		return;
	if (type == 3 && v <= 0xFFFF)
		p_w16(p, ent + 8, v);
	else if (type == 4)
		p_w32(p, ent + 8, v);
}

/* Directory at off: number of entries, or -1 when it does not fit. */
static int p_dir(const Patch *p, uint32_t off)
{
	if (off < 8 || !p_have(p, off, 2))
		return -1;
	unsigned n = p_u16(p, off);
	if (n == 0 || n > 1024 || !p_have(p, (size_t)off + 2, (size_t)n * 12 + 4))
		return -1;
	return (int)n;
}

/* The single SHORT or LONG value of an entry, or 0 for anything else. */
static uint32_t p_value(const Patch *p, size_t ent)
{
	unsigned type = p_u16(p, ent + 2);

	if (p_u32(p, ent + 4) != 1)
		return 0;
	if (type == 3)
		return p_u16(p, ent + 8);
	if (type == 4)
		return p_u32(p, ent + 8);
	return 0;
}

static void p_zero(Patch *p, uint32_t off, uint32_t len)
{
	if (off >= 8 && len && p_have(p, off, len))
		memset(p->t + off, 0, len);
}

/* The stale thumbnail goes with its link: unlinked only, its bytes would
 * stay in the block, unreferenced but readable by anyone who looks -- the
 * old pixels of a picture whose edit may have been made to hide part of it.
 * A JPEG thumbnail (JPEGInterchangeFormat and its length) and a one-strip
 * uncompressed one are zeroed, then IFD1's directory itself. */
static void wipe_ifd1(Patch *p, uint32_t ifd1)
{
	int cnt = p_dir(p, ifd1);
	uint32_t jpeg = 0, jpeg_len = 0, strip = 0, strip_len = 0;

	if (cnt < 0)
		return;
	for (int i = 0; i < cnt; i++) {
		size_t ent = (size_t)ifd1 + 2 + (size_t)i * 12;

		switch (p_u16(p, ent)) {
		case 0x0201:
			jpeg = p_value(p, ent);
			break;
		case 0x0202:
			jpeg_len = p_value(p, ent);
			break;
		case 0x0111:
			strip = p_value(p, ent);
			break;
		case 0x0117:
			strip_len = p_value(p, ent);
			break;
		}
	}
	p_zero(p, jpeg, jpeg_len);
	p_zero(p, strip, strip_len);
	p_zero(p, ifd1, 2 + (uint32_t)cnt * 12 + 4);
}

static void patch_tiff(uint8_t *tiff, size_t n, int reset_orientation, int w,
		       int h)
{
	Patch p = { tiff, n, 0 };
	if (n < 8)
		return;
	if (tiff[0] == 'I' && tiff[1] == 'I')
		p.le = 1;
	else if (!(tiff[0] == 'M' && tiff[1] == 'M'))
		return;
	if (p_u16(&p, 2) != 42)
		return;
	uint32_t ifd0 = p_u32(&p, 4);
	int cnt = p_dir(&p, ifd0);
	if (cnt < 0)
		return;
	uint32_t exif_ifd = 0;
	for (int i = 0; i < cnt; i++) {
		size_t ent = (size_t)ifd0 + 2 + (size_t)i * 12;
		unsigned tag = p_u16(&p, ent);
		if (tag == 0x0112 && reset_orientation)
			patch_inline(&p, ent, 1);
		else if (tag == 0x8769 && p_u32(&p, ent + 4) == 1)
			exif_ifd = p_u32(&p, ent + 8);
	}
	/* Wipe and unlink IFD1 (the stale thumbnail). */
	wipe_ifd1(&p, p_u32(&p, (size_t)ifd0 + 2 + (size_t)cnt * 12));
	p_w32(&p, (size_t)ifd0 + 2 + (size_t)cnt * 12, 0);

	int ecnt = exif_ifd ? p_dir(&p, exif_ifd) : -1;
	for (int i = 0; i < ecnt && w > 0 && h > 0; i++) {
		size_t ent = (size_t)exif_ifd + 2 + (size_t)i * 12;
		unsigned tag = p_u16(&p, ent);
		if (tag == 0xA002)
			patch_inline(&p, ent, (uint32_t)w);
		else if (tag == 0xA003)
			patch_inline(&p, ent, (uint32_t)h);
	}
}

/* ------------------------------------------------------------------------
 * Re-insertion
 * --------------------------------------------------------------------- */

typedef struct {
	int w, h;
} SofCtx;

static int find_sof(const uint8_t *d, const Seg *s, void *user)
{
	SofCtx *c = user;
	unsigned m = s->marker;
	if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC &&
	    s->plen >= 5) {
		c->h = (d[s->pay + 1] << 8) | d[s->pay + 2];
		c->w = (d[s->pay + 3] << 8) | d[s->pay + 4];
	}
	return 0;
}

typedef struct {
	uint8_t *out;
	size_t o;
	int pass;		/* 0: APP0 segments, 1: the others */
} CopyCtx;

static int copy_seg(const uint8_t *d, const Seg *s, void *user)
{
	CopyCtx *c = user;
	int app0 = s->marker == 0xE0;
	if (is_exif_app1(d, s))
		return 0;	/* replaced */
	if ((c->pass == 0) != app0)
		return 0;
	size_t len = s->end - s->start;
	memcpy(c->out + c->o, d + s->start, len);
	c->o += len;
	return 0;
}

int iv_jpeg_write_with_app1(const char *jpeg_path, const uint8_t *app1,
			    size_t len, int reset_orientation)
{
	if (jpeg_path == NULL || app1 == NULL || len < 14)
		return -1;
	/* Accept a bare TIFF block too, adding the identifier. */
	int add_id = memcmp(app1, "Exif\0\0", 6) != 0;
	if (add_id && !(memcmp(app1, "II*\0", 4) == 0 || memcmp(app1, "MM\0*", 4) == 0))
		return -1;
	size_t plen = len + (add_id ? 6 : 0);
	if (plen > 65533)
		return -1;	/* does not fit a segment */

	size_t n = 0;
	uint8_t *src = ivp_read_file(jpeg_path, IVP_JPEG_MAX, &n);
	if (src == NULL)
		return -1;
	size_t sos = 0;
	SofCtx sof = { 0, 0 };
	if (walk(src, n, find_sof, &sof, &sos) < 0) {
		free(src);
		return -1;
	}

	uint8_t *seg = malloc(plen);
	uint8_t *out = malloc(n + plen + 4);
	if (seg == NULL || out == NULL) {
		free(seg);
		free(out);
		free(src);
		return -1;
	}
	if (add_id) {
		memcpy(seg, "Exif\0\0", 6);
		memcpy(seg + 6, app1, len);
	} else {
		memcpy(seg, app1, len);
	}
	patch_tiff(seg + 6, plen - 6, reset_orientation, sof.w, sof.h);

	CopyCtx c = { out, 0, 0 };
	out[c.o++] = 0xFF;
	out[c.o++] = 0xD8;
	walk(src, n, copy_seg, &c, &sos);	/* APP0 (JFIF) first */
	out[c.o++] = 0xFF;
	out[c.o++] = 0xE1;
	out[c.o++] = (uint8_t)((plen + 2) >> 8);
	out[c.o++] = (uint8_t)(plen + 2);
	memcpy(out + c.o, seg, plen);
	c.o += plen;
	c.pass = 1;
	walk(src, n, copy_seg, &c, &sos);	/* every other segment */
	memcpy(out + c.o, src + sos, n - sos);	/* scan data to the end */
	c.o += n - sos;

	int r = ivp_write_file_atomic(jpeg_path, out, c.o);
	free(seg);
	free(out);
	free(src);
	return r;
}
