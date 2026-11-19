// LikeOS -- LikeOS Image Viewer: EXIF and basic file information.
//
// One TIFF/IFD parser serves every container: a JPEG's APP1 segment, a
// PNG eXIf chunk, a WebP EXIF chunk and TIFF files themselves.  It reads
// through a small source abstraction (memory or a seekable file with a
// block cache), so a TIFF file whose directories sit behind hundreds of
// megabytes of strips is parsed without reading it whole.
//
// Every read is bounds-checked against the TIFF block it belongs to,
// directory chains are followed at most a few levels deep and never twice
// to the same offset, entry counts and value sizes are validated before
// use.  Malformed input yields fewer fields, never a crash -- the unit
// tests feed it mutated blobs to hold it to that.
//
// The container walkers also fill what the info panel shows besides EXIF:
// pixel size, bits per pixel, channel layout (JPEG SOF, PNG IHDR, WebP
// VP8/VP8L/VP8X, GIF, BMP, TIFF), DPI fallbacks (JFIF density, PNG pHYs,
// BMP pixels-per-metre) and whether the file is animated.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

/* ------------------------------------------------------------------------
 * Byte sources
 * --------------------------------------------------------------------- */

#define CACHE_BLOCK 4096

typedef struct {
	const uint8_t *mem;	/* memory source, or NULL */
	FILE *f;		/* file source */
	uint64_t size;
	/* One-block read cache for the file source. */
	uint8_t cache[CACHE_BLOCK];
	uint64_t cache_off;
	size_t cache_len;
	int cache_valid;
} Src;

/* Read n bytes at off; 0 on success, -1 when out of range or on error. */
static int src_read(Src *s, uint64_t off, void *buf, size_t n)
{
	if (n == 0)
		return 0;
	if (off > s->size || (uint64_t)n > s->size - off)
		return -1;
	if (s->mem != NULL) {
		memcpy(buf, s->mem + off, n);
		return 0;
	}
	uint8_t *out = buf;
	while (n > 0) {
		if (!s->cache_valid || off < s->cache_off ||
		    off >= s->cache_off + s->cache_len) {
			uint64_t blk = off - off % CACHE_BLOCK;
			if (fseek(s->f, (long)blk, SEEK_SET) != 0)
				return -1;
			size_t want = CACHE_BLOCK;
			if (s->size - blk < want)
				want = (size_t)(s->size - blk);
			size_t got = fread(s->cache, 1, want, s->f);
			if (got == 0)
				return -1;
			s->cache_off = blk;
			s->cache_len = got;
			s->cache_valid = 1;
			if (off >= blk + got)
				return -1;
		}
		size_t in = (size_t)(off - s->cache_off);
		size_t take = s->cache_len - in;
		if (take > n)
			take = n;
		memcpy(out, s->cache + in, take);
		out += take;
		off += take;
		n -= take;
	}
	return 0;
}

static int src_u8(Src *s, uint64_t off, unsigned *v)
{
	uint8_t b;
	if (src_read(s, off, &b, 1) < 0)
		return -1;
	*v = b;
	return 0;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[1] << 8) | p[0];
}

static unsigned be16(const uint8_t *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

static unsigned le16(const uint8_t *p)
{
	return ((unsigned)p[1] << 8) | p[0];
}

/* ------------------------------------------------------------------------
 * TIFF / IFD parser
 * --------------------------------------------------------------------- */

enum {
	T_BYTE = 1, T_ASCII, T_SHORT, T_LONG, T_RATIONAL, T_SBYTE,
	T_UNDEFINED, T_SSHORT, T_SLONG, T_SRATIONAL, T_FLOAT, T_DOUBLE,
};

static const unsigned type_size[13] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8 };

typedef struct {
	Src *s;
	uint64_t base;		/* TIFF header position in the source */
	uint64_t len;		/* TIFF block length */
	int le;			/* little endian ("II") */
	int container_tiff;	/* a TIFF file: IFD0 also gives pixel size */
	uint32_t visited[16];
	int nvisited;
	/* Values gathered while walking, resolved at the end. */
	double xres, yres;
	int res_unit;
	int have_res;
	double shutter_apex, aperture_apex;
	int have_shutter, have_aperture;
	int photometric, samples, bits_sum;
} Tiff;

typedef struct {
	unsigned tag, type;
	uint32_t count;
	uint64_t voff;		/* value position relative to the TIFF base */
} Entry;

static int t_read(Tiff *t, uint64_t off, void *buf, size_t n)
{
	if (off > t->len || (uint64_t)n > t->len - off)
		return -1;
	return src_read(t->s, t->base + off, buf, n);
}

static int t_u16(Tiff *t, uint64_t off, unsigned *v)
{
	uint8_t b[2];
	if (t_read(t, off, b, 2) < 0)
		return -1;
	*v = t->le ? le16(b) : be16(b);
	return 0;
}

static int t_u32(Tiff *t, uint64_t off, uint32_t *v)
{
	uint8_t b[4];
	if (t_read(t, off, b, 4) < 0)
		return -1;
	*v = t->le ? le32(b) : be32(b);
	return 0;
}

/* Value i of an integer entry (BYTE/SHORT/LONG and their signed forms). */
static int e_int(Tiff *t, const Entry *e, uint32_t i, long *v)
{
	if (i >= e->count)
		return -1;
	unsigned u16;
	uint32_t u32;
	unsigned u8;
	switch (e->type) {
	case T_BYTE:
	case T_UNDEFINED:
		if (src_u8(t->s, t->base + e->voff + i, &u8) < 0 || e->voff + i >= t->len)
			return -1;
		*v = (long)u8;
		return 0;
	case T_SBYTE:
		if (src_u8(t->s, t->base + e->voff + i, &u8) < 0 || e->voff + i >= t->len)
			return -1;
		*v = (long)(int8_t)u8;
		return 0;
	case T_SHORT:
		if (t_u16(t, e->voff + (uint64_t)i * 2, &u16) < 0)
			return -1;
		*v = (long)u16;
		return 0;
	case T_SSHORT:
		if (t_u16(t, e->voff + (uint64_t)i * 2, &u16) < 0)
			return -1;
		*v = (long)(int16_t)u16;
		return 0;
	case T_LONG:
		if (t_u32(t, e->voff + (uint64_t)i * 4, &u32) < 0)
			return -1;
		*v = (long)u32;
		return 0;
	case T_SLONG:
		if (t_u32(t, e->voff + (uint64_t)i * 4, &u32) < 0)
			return -1;
		*v = (long)(int32_t)u32;
		return 0;
	default:
		return -1;
	}
}

/* Value i of a numeric entry as double (rationals divided out; a zero
 * denominator reads as 0). */
static int e_num(Tiff *t, const Entry *e, uint32_t i, double *v)
{
	if (i >= e->count)
		return -1;
	uint32_t a, b;
	long l;
	switch (e->type) {
	case T_RATIONAL:
		if (t_u32(t, e->voff + (uint64_t)i * 8, &a) < 0 ||
		    t_u32(t, e->voff + (uint64_t)i * 8 + 4, &b) < 0)
			return -1;
		*v = b ? (double)a / (double)b : 0.0;
		return 0;
	case T_SRATIONAL:
		if (t_u32(t, e->voff + (uint64_t)i * 8, &a) < 0 ||
		    t_u32(t, e->voff + (uint64_t)i * 8 + 4, &b) < 0)
			return -1;
		*v = b ? (double)(int32_t)a / (double)(int32_t)b : 0.0;
		return 0;
	case T_FLOAT: {
		if (t_u32(t, e->voff + (uint64_t)i * 4, &a) < 0)
			return -1;
		float f;
		memcpy(&f, &a, 4);
		*v = isfinite(f) ? (double)f : 0.0;
		return 0;
	}
	case T_DOUBLE: {
		uint8_t raw[8];
		if (t_read(t, e->voff + (uint64_t)i * 8, raw, 8) < 0)
			return -1;
		uint64_t bits = 0;
		for (int k = 0; k < 8; k++)
			bits |= (uint64_t)raw[t->le ? k : 7 - k] << (8 * k);
		double d;
		memcpy(&d, &bits, 8);
		*v = isfinite(d) ? d : 0.0;
		return 0;
	}
	default:
		if (e_int(t, e, i, &l) < 0)
			return -1;
		*v = (double)l;
		return 0;
	}
}

/* Copy an ASCII (or UNDEFINED text) entry into dst: stops at the first
 * NUL, control characters become spaces, trailing spaces are trimmed. */
static void e_str(Tiff *t, const Entry *e, char *dst, size_t cap)
{
	if (cap == 0)
		return;
	dst[0] = '\0';
	if (e->type != T_ASCII && e->type != T_UNDEFINED && e->type != T_BYTE)
		return;
	size_t n = e->count;
	if (n > cap - 1)
		n = cap - 1;
	uint8_t tmp[256];
	if (n > sizeof(tmp))
		n = sizeof(tmp);
	if (t_read(t, e->voff, tmp, n) < 0)
		return;
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		uint8_t c = tmp[i];
		if (c == 0)
			break;
		dst[o++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
	}
	while (o > 0 && dst[o - 1] == ' ')
		o--;
	dst[o] = '\0';
	/* Leading blanks too ("    Canon" exists in the wild). */
	size_t lead = 0;
	while (dst[lead] == ' ')
		lead++;
	if (lead)
		memmove(dst, dst + lead, o - lead + 1);
}

/* Decode the 12-byte entry at off. */
static int read_entry(Tiff *t, uint64_t off, Entry *e)
{
	unsigned tag, type;
	uint32_t count, val;
	if (t_u16(t, off, &tag) < 0 || t_u16(t, off + 2, &type) < 0 ||
	    t_u32(t, off + 4, &count) < 0 || t_u32(t, off + 8, &val) < 0)
		return -1;
	e->tag = tag;
	e->type = type;
	e->count = count;
	if (type == 0 || type > T_DOUBLE)
		return -1;	/* unknown type: skip the entry */
	uint64_t total = (uint64_t)count * type_size[type];
	if (total <= 4)
		e->voff = off + 8;
	else
		e->voff = val;
	/* The whole value must lie inside the TIFF block. */
	if (e->voff > t->len || total > t->len - e->voff)
		return -1;
	return 0;
}

enum { IFD_0, IFD_EXIF, IFD_GPS, IFD_1 };

static int visited(Tiff *t, uint32_t off)
{
	for (int i = 0; i < t->nvisited; i++)
		if (t->visited[i] == off)
			return 1;
	if (t->nvisited < (int)(sizeof(t->visited) / sizeof(t->visited[0])))
		t->visited[t->nvisited++] = off;
	else
		return 1;	/* too many directories: stop */
	return 0;
}

static double gps_coord(Tiff *t, const Entry *e)
{
	double d = 0, m = 0, s = 0;
	if (e_num(t, e, 0, &d) < 0)
		return NAN;
	if (e->count >= 2)
		e_num(t, e, 1, &m);
	if (e->count >= 3)
		e_num(t, e, 2, &s);
	return d + m / 60.0 + s / 3600.0;
}

static void parse_ifd(Tiff *t, uint32_t off, int kind, int depth, IvExif *x);

static void handle_entry(Tiff *t, const Entry *e, int kind, int depth, IvExif *x,
			 double *gps_lat, double *gps_lon, char *lat_ref,
			 char *lon_ref, int *alt_ref)
{
	long l;
	double d;
	char ref[4];

	if (kind == IFD_0 || kind == IFD_1) {
		switch (e->tag) {
		case 0x8769:	/* ExifIFD pointer */
			if (kind == IFD_0 && e_int(t, e, 0, &l) == 0 && l > 0)
				parse_ifd(t, (uint32_t)l, IFD_EXIF, depth + 1, x);
			return;
		case 0x8825:	/* GPS IFD pointer */
			if (kind == IFD_0 && e_int(t, e, 0, &l) == 0 && l > 0)
				parse_ifd(t, (uint32_t)l, IFD_GPS, depth + 1, x);
			return;
		}
		if (kind == IFD_1)
			return;	/* the thumbnail directory adds nothing */
		switch (e->tag) {
		case 0x010F:
			e_str(t, e, x->make, sizeof(x->make));
			break;
		case 0x0110:
			e_str(t, e, x->model, sizeof(x->model));
			break;
		case 0x0131:
			e_str(t, e, x->software, sizeof(x->software));
			break;
		case 0x0132:
			e_str(t, e, x->datetime, sizeof(x->datetime));
			break;
		case 0x013B:
			e_str(t, e, x->artist, sizeof(x->artist));
			break;
		case 0x8298:
			e_str(t, e, x->copyright, sizeof(x->copyright));
			break;
		case 0x0112:
			if (e_int(t, e, 0, &l) == 0 && l >= 1 && l <= 8)
				x->orientation = (int)l;
			break;
		case 0x011A:
			if (e_num(t, e, 0, &d) == 0 && d > 0) {
				t->xres = d;
				t->have_res = 1;
			}
			break;
		case 0x011B:
			if (e_num(t, e, 0, &d) == 0 && d > 0)
				t->yres = d;
			break;
		case 0x0128:
			if (e_int(t, e, 0, &l) == 0)
				t->res_unit = (int)l;
			break;
		case 0x0100:
			if (t->container_tiff && e_int(t, e, 0, &l) == 0 && l > 0)
				x->width = (int)(l > 0x7FFFFFFF ? 0 : l);
			break;
		case 0x0101:
			if (t->container_tiff && e_int(t, e, 0, &l) == 0 && l > 0)
				x->height = (int)(l > 0x7FFFFFFF ? 0 : l);
			break;
		case 0x0102:
			if (t->container_tiff) {
				int sum = 0;
				for (uint32_t i = 0; i < e->count && i < 8; i++)
					if (e_int(t, e, i, &l) == 0 && l > 0 && l <= 64)
						sum += (int)l;
				t->bits_sum = sum;
			}
			break;
		case 0x0106:
			if (e_int(t, e, 0, &l) == 0)
				t->photometric = (int)l;
			break;
		case 0x0115:
			if (e_int(t, e, 0, &l) == 0 && l > 0 && l <= 16)
				t->samples = (int)l;
			break;
		}
		return;
	}

	if (kind == IFD_EXIF) {
		switch (e->tag) {
		case 0x829A:
			if (e_num(t, e, 0, &d) == 0 && d > 0)
				x->exposure_time = d;
			break;
		case 0x829D:
			if (e_num(t, e, 0, &d) == 0 && d > 0)
				x->fnumber = d;
			break;
		case 0x8822:
			if (e_int(t, e, 0, &l) == 0)
				x->exposure_program = (int)l;
			break;
		case 0x8827:	/* ISOSpeedRatings / PhotographicSensitivity */
			if (e_int(t, e, 0, &l) == 0 && l > 0)
				x->iso = (int)l;
			break;
		case 0x8833:	/* ISOSpeed, when the short field saturated */
			if (e_int(t, e, 0, &l) == 0 && l > 0 &&
			    (x->iso == 0 || x->iso == 65535))
				x->iso = (int)(l > 0x7FFFFFFF ? 0x7FFFFFFF : l);
			break;
		case 0x9003:
			e_str(t, e, x->datetime_original, sizeof(x->datetime_original));
			break;
		case 0x9004:
			e_str(t, e, x->datetime_digitized, sizeof(x->datetime_digitized));
			break;
		case 0x9201:
			if (e_num(t, e, 0, &d) == 0) {
				t->shutter_apex = d;
				t->have_shutter = 1;
			}
			break;
		case 0x9202:
			if (e_num(t, e, 0, &d) == 0) {
				t->aperture_apex = d;
				t->have_aperture = 1;
			}
			break;
		case 0x9204:
			if (e_num(t, e, 0, &d) == 0)
				x->exposure_bias = d;
			break;
		case 0x9207:
			if (e_int(t, e, 0, &l) == 0)
				x->metering_mode = (int)l;
			break;
		case 0x9209:
			if (e_int(t, e, 0, &l) == 0)
				x->flash = (int)l;
			break;
		case 0x920A:
			if (e_num(t, e, 0, &d) == 0 && d > 0)
				x->focal_length = d;
			break;
		case 0xA001:
			if (e_int(t, e, 0, &l) == 0)
				x->color_space = (int)l;
			break;
		case 0xA403:
			if (e_int(t, e, 0, &l) == 0)
				x->white_balance = (int)l;
			break;
		case 0xA405:
			if (e_int(t, e, 0, &l) == 0 && l > 0)
				x->focal_length_35mm = (int)l;
			break;
		case 0xA433:
			e_str(t, e, x->lens_make, sizeof(x->lens_make));
			break;
		case 0xA434:
			e_str(t, e, x->lens, sizeof(x->lens));
			break;
		}
		return;
	}

	/* GPS */
	switch (e->tag) {
	case 0x0001:
		e_str(t, e, ref, sizeof(ref));
		*lat_ref = ref[0];
		break;
	case 0x0002:
		*gps_lat = gps_coord(t, e);
		break;
	case 0x0003:
		e_str(t, e, ref, sizeof(ref));
		*lon_ref = ref[0];
		break;
	case 0x0004:
		*gps_lon = gps_coord(t, e);
		break;
	case 0x0005:
		if (e_int(t, e, 0, &l) == 0)
			*alt_ref = (int)l;
		break;
	case 0x0006:
		if (e_num(t, e, 0, &d) == 0) {
			x->gps_alt = d;
			x->has_alt = 1;
		}
		break;
	}
}

static void parse_ifd(Tiff *t, uint32_t off, int kind, int depth, IvExif *x)
{
	if (depth > 4 || off < 8 || visited(t, off))
		return;
	unsigned n;
	if (t_u16(t, off, &n) < 0 || n == 0 || n > 1024)
		return;
	/* Entries beyond the block end are dropped individually by
	 * read_entry(); a directory claiming more than fits is clipped. */
	uint64_t avail = (t->len - off - 2) / 12;
	if (n > avail)
		n = (unsigned)avail;

	double lat = NAN, lon = NAN;
	char lat_ref = 0, lon_ref = 0;
	int alt_ref = 0;
	for (unsigned i = 0; i < n; i++) {
		Entry e;
		if (read_entry(t, (uint64_t)off + 2 + (uint64_t)i * 12, &e) < 0)
			continue;
		handle_entry(t, &e, kind, depth, x, &lat, &lon, &lat_ref,
			     &lon_ref, &alt_ref);
	}
	if (kind == IFD_GPS) {
		if (isfinite(lat) && isfinite(lon) && fabs(lat) <= 90.0 &&
		    fabs(lon) <= 180.0) {
			x->gps_lat = (lat_ref == 'S' || lat_ref == 's') ? -lat : lat;
			x->gps_lon = (lon_ref == 'W' || lon_ref == 'w') ? -lon : lon;
			x->has_gps = 1;
		}
		if (x->has_alt && alt_ref == 1)
			x->gps_alt = -fabs(x->gps_alt);
	}
	if (kind == IFD_0) {
		/* IFD1 (thumbnail) follows; walked only for completeness of
		 * the loop guard, nothing is taken from it. */
		uint32_t next;
		if (t_u32(t, (uint64_t)off + 2 + (uint64_t)n * 12, &next) == 0 && next)
			parse_ifd(t, next, IFD_1, depth + 1, x);
	}
}

static void exif_defaults(IvExif *x)
{
	memset(x, 0, sizeof(*x));
	x->orientation = 1;
	x->flash = -1;
	x->exposure_program = -1;
	x->metering_mode = -1;
	x->white_balance = -1;
}

/* Parse a TIFF structure at [base, base + len) of the source. */
static int tiff_parse(Src *s, uint64_t base, uint64_t len, int container_tiff,
		      IvExif *x)
{
	uint8_t hdr[8];
	if (len < 8 || src_read(s, base, hdr, 8) < 0)
		return -1;
	Tiff t;
	memset(&t, 0, sizeof(t));
	t.s = s;
	t.base = base;
	t.len = len;
	t.container_tiff = container_tiff;
	t.res_unit = 2;
	if (hdr[0] == 'I' && hdr[1] == 'I')
		t.le = 1;
	else if (hdr[0] == 'M' && hdr[1] == 'M')
		t.le = 0;
	else
		return -1;
	unsigned magic = t.le ? le16(hdr + 2) : be16(hdr + 2);
	if (magic != 42)
		return -1;
	uint32_t ifd0 = t.le ? le32(hdr + 4) : be32(hdr + 4);
	parse_ifd(&t, ifd0, IFD_0, 0, x);

	/* Resolve the values that depend on several tags. */
	if (t.have_res) {
		double f = t.res_unit == 3 ? 2.54 : 1.0;
		if (t.res_unit != 1) {	/* 1 = no absolute unit */
			x->x_dpi = t.xres * f;
			x->y_dpi = (t.yres > 0 ? t.yres : t.xres) * f;
		}
	}
	if (x->exposure_time <= 0.0 && t.have_shutter &&
	    fabs(t.shutter_apex) < 40.0)
		x->exposure_time = pow(2.0, -t.shutter_apex);
	if (x->fnumber <= 0.0 && t.have_aperture && fabs(t.aperture_apex) < 40.0)
		x->fnumber = pow(2.0, t.aperture_apex / 2.0);
	if (container_tiff) {
		if (t.samples > 0)
			x->channels = t.samples;
		if (t.bits_sum > 0)
			x->bits = t.bits_sum;
		const char *ct = NULL;
		switch (t.photometric) {
		case 0:
		case 1:
			ct = t.samples >= 2 ? "Greyscale + alpha" : "Greyscale";
			break;
		case 2:
			ct = t.samples >= 4 ? "RGBA" : "RGB";
			break;
		case 3:
			ct = "Palette";
			break;
		case 5:
			ct = "CMYK";
			break;
		case 6:
			ct = "YCbCr";
			break;
		}
		if (ct)
			snprintf(x->color_type, sizeof(x->color_type), "%s", ct);
	}
	x->has_exif = 1;
	return 0;
}

/* ------------------------------------------------------------------------
 * Containers
 * --------------------------------------------------------------------- */

/* An EXIF payload may come with or without the "Exif\0\0" prefix. */
static int exif_block(Src *s, uint64_t off, uint64_t len, IvExif *x)
{
	uint8_t p[6];
	if (len >= 6 && src_read(s, off, p, 6) == 0 && memcmp(p, "Exif\0\0", 6) == 0) {
		off += 6;
		len -= 6;
	}
	return tiff_parse(s, off, len, 0, x);
}

static int is_sof(unsigned m)
{
	return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

static int parse_jpeg(Src *s, IvExif *x)
{
	snprintf(x->format, sizeof(x->format), "JPEG");
	uint64_t off = 2;
	int got_exif = 0, have_jfif_dpi = 0;
	double jx = 0, jy = 0;
	for (int guard = 0; guard < 4096; guard++) {
		unsigned b;
		if (src_u8(s, off, &b) < 0)
			break;
		if (b != 0xFF) {
			/* Garbage between segments: resynchronise on the next
			 * marker, but not forever. */
			off++;
			continue;
		}
		unsigned m;
		do {
			off++;
			if (src_u8(s, off, &m) < 0)
				goto done;
		} while (m == 0xFF);
		off++;
		if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7))
			continue;
		if (m == 0xD9 || m == 0xDA)
			break;
		uint8_t lb[2];
		if (src_read(s, off, lb, 2) < 0)
			break;
		unsigned seglen = be16(lb);
		if (seglen < 2)
			break;
		uint64_t pay = off + 2, plen = seglen - 2;
		if (pay > s->size || plen > s->size - pay)
			plen = s->size > pay ? s->size - pay : 0;
		if (m == 0xE1 && !got_exif && plen >= 14) {
			uint8_t sig[6];
			if (src_read(s, pay, sig, 6) == 0 && memcmp(sig, "Exif\0\0", 6) == 0 &&
			    tiff_parse(s, pay + 6, plen - 6, 0, x) == 0)
				got_exif = 1;
		} else if (m == 0xE0 && plen >= 12) {
			uint8_t j[12];
			if (src_read(s, pay, j, 12) == 0 && memcmp(j, "JFIF\0", 5) == 0) {
				unsigned unit = j[7];
				unsigned dx = be16(j + 8), dy = be16(j + 10);
				if ((unit == 1 || unit == 2) && dx > 0 && dy > 0) {
					double f = unit == 2 ? 2.54 : 1.0;
					jx = dx * f;
					jy = dy * f;
					have_jfif_dpi = 1;
				}
			}
		} else if (is_sof(m) && plen >= 6) {
			uint8_t f[6];
			if (src_read(s, pay, f, 6) == 0) {
				unsigned prec = f[0], ncomp = f[5];
				x->height = (int)be16(f + 1);
				x->width = (int)be16(f + 3);
				x->channels = (int)ncomp;
				x->bits = (int)(prec * ncomp);
				const char *ct = ncomp == 1 ? "Greyscale" :
						 ncomp == 3 ? "RGB" :
						 ncomp == 4 ? "CMYK" : "";
				snprintf(x->color_type, sizeof(x->color_type), "%s", ct);
			}
		}
		off = pay + (seglen - 2);
	}
done:
	if (x->x_dpi <= 0.0 && have_jfif_dpi) {
		x->x_dpi = jx;
		x->y_dpi = jy;
	}
	return got_exif ? 1 : 0;
}

static int parse_png(Src *s, IvExif *x)
{
	snprintf(x->format, sizeof(x->format), "PNG");
	uint64_t off = 8;
	int got_exif = 0;
	for (int guard = 0; guard < 100000; guard++) {
		uint8_t h[8];
		if (src_read(s, off, h, 8) < 0)
			break;
		uint32_t len = be32(h);
		if (len > 0x7FFFFFFF)
			break;
		uint64_t data = off + 8;
		if (data > s->size || (uint64_t)len > s->size - data)
			break;
		if (memcmp(h + 4, "IHDR", 4) == 0 && len >= 13) {
			uint8_t d[13];
			if (src_read(s, data, d, 13) == 0) {
				x->width = (int)(be32(d) & 0x7FFFFFFF);
				x->height = (int)(be32(d + 4) & 0x7FFFFFFF);
				unsigned depth = d[8], ctype = d[9];
				static const struct { unsigned t, ch; const char *name; } ct[] = {
					{ 0, 1, "Greyscale" }, { 2, 3, "RGB" },
					{ 3, 1, "Palette" }, { 4, 2, "Greyscale + alpha" },
					{ 6, 4, "RGBA" },
				};
				for (size_t i = 0; i < sizeof(ct) / sizeof(ct[0]); i++) {
					if (ct[i].t == ctype) {
						x->channels = (int)ct[i].ch;
						x->bits = (int)(depth * ct[i].ch);
						snprintf(x->color_type, sizeof(x->color_type),
							 "%s", ct[i].name);
					}
				}
			}
		} else if (memcmp(h + 4, "pHYs", 4) == 0 && len >= 9) {
			uint8_t d[9];
			if (src_read(s, data, d, 9) == 0 && d[8] == 1) {
				double px = be32(d), py = be32(d + 4);
				if (x->x_dpi <= 0.0 && px > 0 && py > 0) {
					x->x_dpi = px * 0.0254;
					x->y_dpi = py * 0.0254;
				}
			}
		} else if (memcmp(h + 4, "eXIf", 4) == 0 && !got_exif) {
			/* Keep any DPI found from pHYs only if EXIF has none. */
			double kx = x->x_dpi, ky = x->y_dpi;
			x->x_dpi = x->y_dpi = 0;
			if (exif_block(s, data, len, x) == 0)
				got_exif = 1;
			if (x->x_dpi <= 0.0) {
				x->x_dpi = kx;
				x->y_dpi = ky;
			}
		} else if (memcmp(h + 4, "acTL", 4) == 0) {
			x->animated = 1;
		} else if (memcmp(h + 4, "IEND", 4) == 0) {
			break;
		}
		off = data + len + 4;	/* skip CRC */
	}
	return got_exif ? 1 : 0;
}

static int parse_webp(Src *s, IvExif *x)
{
	snprintf(x->format, sizeof(x->format), "WebP");
	uint8_t h[12];
	if (src_read(s, 0, h, 12) < 0)
		return -1;
	uint64_t end = (uint64_t)le32(h + 4) + 8;
	if (end > s->size)
		end = s->size;
	uint64_t off = 12;
	int got_exif = 0, alpha = 0;
	for (int guard = 0; guard < 100000 && off + 8 <= end; guard++) {
		uint8_t c[8];
		if (src_read(s, off, c, 8) < 0)
			break;
		uint32_t len = le32(c + 4);
		uint64_t data = off + 8;
		if ((uint64_t)len > end - data)
			break;
		if (memcmp(c, "VP8X", 4) == 0 && len >= 10) {
			uint8_t d[10];
			if (src_read(s, data, d, 10) == 0) {
				if (d[0] & 0x10)
					alpha = 1;
				if (d[0] & 0x02)
					x->animated = 1;
				x->width = (int)(1 + (d[4] | (d[5] << 8) | ((uint32_t)d[6] << 16)));
				x->height = (int)(1 + (d[7] | (d[8] << 8) | ((uint32_t)d[9] << 16)));
			}
		} else if (memcmp(c, "VP8 ", 4) == 0 && len >= 10) {
			uint8_t d[10];
			if (src_read(s, data, d, 10) == 0 && d[3] == 0x9D && d[4] == 0x01 &&
			    d[5] == 0x2A && x->width == 0) {
				x->width = (int)(le16(d + 6) & 0x3FFF);
				x->height = (int)(le16(d + 8) & 0x3FFF);
			}
		} else if (memcmp(c, "VP8L", 4) == 0 && len >= 5) {
			uint8_t d[5];
			if (src_read(s, data, d, 5) == 0 && d[0] == 0x2F) {
				uint32_t v = le32(d + 1);
				if (x->width == 0) {
					x->width = (int)((v & 0x3FFF) + 1);
					x->height = (int)(((v >> 14) & 0x3FFF) + 1);
				}
				if ((v >> 28) & 1)
					alpha = 1;
			}
		} else if (memcmp(c, "ALPH", 4) == 0) {
			alpha = 1;
		} else if (memcmp(c, "ANIM", 4) == 0) {
			x->animated = 1;
		} else if (memcmp(c, "EXIF", 4) == 0 && !got_exif) {
			if (exif_block(s, data, len, x) == 0)
				got_exif = 1;
		}
		off = data + len + (len & 1);
	}
	x->channels = alpha ? 4 : 3;
	x->bits = alpha ? 32 : 24;
	snprintf(x->color_type, sizeof(x->color_type), "%s", alpha ? "RGBA" : "RGB");
	return got_exif ? 1 : 0;
}

static int parse_gif(Src *s, IvExif *x)
{
	snprintf(x->format, sizeof(x->format), "GIF");
	uint8_t h[13];
	if (src_read(s, 0, h, 13) < 0)
		return 0;
	x->width = (int)le16(h + 6);
	x->height = (int)le16(h + 8);
	x->bits = (h[10] & 0x80) ? (h[10] & 7) + 1 : 8;
	x->channels = 1;
	snprintf(x->color_type, sizeof(x->color_type), "Palette");
	/* Best-effort animation check: a looping application extension near
	 * the start of the file. */
	uint8_t buf[2048];
	size_t n = s->size < sizeof(buf) ? (size_t)s->size : sizeof(buf);
	if (src_read(s, 0, buf, n) == 0) {
		for (size_t i = 0; i + 11 <= n; i++)
			if (memcmp(buf + i, "NETSCAPE2.0", 11) == 0 ||
			    memcmp(buf + i, "ANIMEXTS1.0", 11) == 0) {
				x->animated = 1;
				break;
			}
	}
	return 0;
}

static int parse_bmp(Src *s, IvExif *x)
{
	snprintf(x->format, sizeof(x->format), "BMP");
	uint8_t h[46];
	if (src_read(s, 0, h, 30) < 0)
		return 0;
	uint32_t dib = le32(h + 14);
	if (dib == 12) {	/* OS/2 core header: 16-bit sizes */
		x->width = (int)le16(h + 18);
		x->height = (int)le16(h + 20);
		x->bits = (int)le16(h + 24);
	} else if (dib >= 40) {
		int32_t w = (int32_t)le32(h + 18), hh = (int32_t)le32(h + 22);
		x->width = w < 0 ? 0 : w;
		x->height = hh == INT32_MIN ? 0 : (hh < 0 ? -hh : hh);
		x->bits = (int)le16(h + 28);
		if (src_read(s, 0, h, 46) == 0) {
			double ppm_x = (int32_t)le32(h + 38), ppm_y = (int32_t)le32(h + 42);
			if (ppm_x > 0 && ppm_y > 0) {
				x->x_dpi = ppm_x * 0.0254;
				x->y_dpi = ppm_y * 0.0254;
			}
		}
	}
	x->channels = x->bits == 32 ? 4 : (x->bits <= 8 ? 1 : 3);
	snprintf(x->color_type, sizeof(x->color_type), "%s",
		 x->bits <= 8 ? "Palette" : (x->bits == 32 ? "RGBA" : "RGB"));
	return 0;
}

/* Sniff the container and dispatch. */
static int parse_src(Src *s, IvExif *x)
{
	uint8_t m[12];
	size_t n = s->size < 12 ? (size_t)s->size : 12;
	memset(m, 0, sizeof(m));
	if (n < 2 || src_read(s, 0, m, n) < 0)
		return -1;
	if (m[0] == 0xFF && m[1] == 0xD8)
		return parse_jpeg(s, x);
	if (n >= 8 && memcmp(m, "\x89PNG\r\n\x1a\n", 8) == 0)
		return parse_png(s, x);
	if (n >= 12 && memcmp(m, "RIFF", 4) == 0 && memcmp(m + 8, "WEBP", 4) == 0)
		return parse_webp(s, x);
	if (n >= 4 && (memcmp(m, "II*\0", 4) == 0 || memcmp(m, "MM\0*", 4) == 0)) {
		snprintf(x->format, sizeof(x->format), "TIFF");
		return tiff_parse(s, 0, s->size, 1, x) == 0 ? 1 : 0;
	}
	if (n >= 6 && memcmp(m, "Exif\0\0", 6) == 0)
		return exif_block(s, 0, s->size, x) == 0 ? 1 : 0;
	if (n >= 6 && (memcmp(m, "GIF87a", 6) == 0 || memcmp(m, "GIF89a", 6) == 0))
		return parse_gif(s, x);
	if (m[0] == 'B' && m[1] == 'M')
		return parse_bmp(s, x);
	return -1;
}

/* ------------------------------------------------------------------------
 * Public entry points
 * --------------------------------------------------------------------- */

int iv_exif_read_mem(const uint8_t *data, size_t len, IvExif *out)
{
	if (out == NULL)
		return -1;
	exif_defaults(out);
	if (data == NULL)
		return -1;
	Src *s = calloc(1, sizeof(*s));
	if (s == NULL)
		return -1;
	s->mem = data;
	s->size = len;
	int r = parse_src(s, out);
	free(s);
	return r;
}

int iv_exif_parse_tiff(const uint8_t *tiff, size_t len, IvExif *out)
{
	if (out == NULL)
		return -1;
	exif_defaults(out);
	if (tiff == NULL)
		return -1;
	Src *s = calloc(1, sizeof(*s));
	if (s == NULL)
		return -1;
	s->mem = tiff;
	s->size = len;
	int r = exif_block(s, 0, len, out) == 0 ? 1 : -1;
	free(s);
	return r;
}

int iv_exif_read_file(const char *path, IvExif *out)
{
	if (out == NULL)
		return -1;
	exif_defaults(out);
	if (path == NULL)
		return -1;
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	Src *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		fclose(f);
		return -1;
	}
	s->f = f;
	long sz = -1;
	if (fseek(f, 0, SEEK_END) == 0)
		sz = ftell(f);
	int r = -1;
	if (sz > 0) {
		s->size = (uint64_t)sz;
		r = parse_src(s, out);
	}
	free(s);
	fclose(f);
	return r;
}

/* ------------------------------------------------------------------------
 * Texts for the info panel
 * --------------------------------------------------------------------- */

const char *iv_exif_flash_string(int flash)
{
	static const struct { int v; const char *s; } tab[] = {
		{ 0x00, "No flash" },
		{ 0x01, "Flash fired" },
		{ 0x05, "Flash fired, return not detected" },
		{ 0x07, "Flash fired, return detected" },
		{ 0x08, "On, flash did not fire" },
		{ 0x09, "Flash fired, compulsory" },
		{ 0x0D, "Flash fired, compulsory, return not detected" },
		{ 0x0F, "Flash fired, compulsory, return detected" },
		{ 0x10, "Off, flash did not fire" },
		{ 0x14, "Off, flash did not fire, return not detected" },
		{ 0x18, "Auto, flash did not fire" },
		{ 0x19, "Flash fired, auto mode" },
		{ 0x1D, "Flash fired, auto mode, return not detected" },
		{ 0x1F, "Flash fired, auto mode, return detected" },
		{ 0x20, "No flash function" },
		{ 0x30, "Off, no flash function" },
		{ 0x41, "Flash fired, red-eye reduction" },
		{ 0x45, "Flash fired, red-eye reduction, return not detected" },
		{ 0x47, "Flash fired, red-eye reduction, return detected" },
		{ 0x49, "Flash fired, compulsory, red-eye reduction" },
		{ 0x4D, "Flash fired, compulsory, red-eye reduction, return not detected" },
		{ 0x4F, "Flash fired, compulsory, red-eye reduction, return detected" },
		{ 0x50, "Off, red-eye reduction" },
		{ 0x58, "Auto, did not fire, red-eye reduction" },
		{ 0x59, "Flash fired, auto mode, red-eye reduction" },
		{ 0x5D, "Flash fired, auto mode, return not detected, red-eye reduction" },
		{ 0x5F, "Flash fired, auto mode, return detected, red-eye reduction" },
	};
	if (flash < 0)
		return "";
	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (tab[i].v == flash)
			return tab[i].s;
	return (flash & 1) ? "Flash fired" : "No flash";
}

const char *iv_exif_orientation_string(int o)
{
	switch (o) {
	case 1:
		return "Normal";
	case 2:
		return "Mirrored horizontally";
	case 3:
		return "Rotated 180°";
	case 4:
		return "Mirrored vertically";
	case 5:
		return "Mirrored horizontally, rotated 90° counter-clockwise";
	case 6:
		return "Rotated 90° clockwise";
	case 7:
		return "Mirrored horizontally, rotated 90° clockwise";
	case 8:
		return "Rotated 90° counter-clockwise";
	default:
		return "";
	}
}

const char *iv_exif_exposure_program_string(int p)
{
	static const char *const tab[] = {
		"Not defined", "Manual", "Program", "Aperture priority",
		"Shutter priority", "Creative", "Action", "Portrait",
		"Landscape",
	};
	if (p < 0 || p >= (int)(sizeof(tab) / sizeof(tab[0])))
		return "";
	return tab[p];
}

const char *iv_exif_metering_mode_string(int m)
{
	static const char *const tab[] = {
		"Unknown", "Average", "Centre-weighted average", "Spot",
		"Multi-spot", "Pattern", "Partial",
	};
	if (m == 255)
		return "Other";
	if (m < 0 || m >= (int)(sizeof(tab) / sizeof(tab[0])))
		return "";
	return tab[m];
}

const char *iv_exif_color_space_string(int cs)
{
	switch (cs) {
	case 1:
		return "sRGB";
	case 2:
		return "Adobe RGB";
	case 0xFFFF:
		return "Uncalibrated";
	default:
		return "";
	}
}

void iv_exif_format_dms(double deg, int is_latitude, char *buf, size_t len)
{
	if (buf == NULL || len == 0)
		return;
	if (!isfinite(deg)) {
		buf[0] = '\0';
		return;
	}
	char hemi = is_latitude ? (deg < 0 ? 'S' : 'N') : (deg < 0 ? 'W' : 'E');
	double a = fabs(deg);
	int d = (int)a;
	double mf = (a - d) * 60.0;
	int m = (int)mf;
	double s = (mf - m) * 60.0;
	/* Rounding the seconds to one decimal can carry into the minutes. */
	if (s >= 59.95) {
		s = 0.0;
		if (++m == 60) {
			m = 0;
			d++;
		}
	}
	snprintf(buf, len, "%d° %d′ %.1f″ %c", d, m, s, hemi);
}

void iv_exif_format_exposure(double t, char *buf, size_t len)
{
	if (buf == NULL || len == 0)
		return;
	if (!(t > 0.0) || !isfinite(t)) {
		buf[0] = '\0';
		return;
	}
	if (t >= 1.0) {
		if (fabs(t - round(t)) < 0.05)
			snprintf(buf, len, "%.0f s", t);
		else
			snprintf(buf, len, "%.1f s", t);
		return;
	}
	double inv = 1.0 / t;
	if (inv >= 1.5 && fabs(inv - round(inv)) < 0.05 * inv)
		snprintf(buf, len, "1/%.0f s", round(inv));
	else
		snprintf(buf, len, "%.2f s", t);
}
