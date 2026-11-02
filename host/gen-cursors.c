/*
 * gen-cursors -- generate the LikeOS X cursor theme.
 *
 * Writes Xcursor-format files for the BUSY cursors: the LikeOS busy ring, a
 * white ring with a blue arc running round it, and the pointer with a small
 * ring beside it that means "starting, but you can still click".  And for the
 * RESIZE and LINK pointers, in the core font's own black-on-white style (see
 * build_bitmap).
 *
 * Why this exists: with no cursor theme installed, libXcursor falls back to the
 * X core cursor font, whose "watch" is a 1-bit black-and-white wristwatch from
 * the 1980s.  Xcursor's format is ARGB and supports animation, so the busy
 * cursor can be something a person recognises.
 *
 * The resize and link pointers exist for a different reason: GTK asks for
 * them by CSS name ("col-resize", "nwse-resize") and, when a theme does not
 * have that name, by an old theme name ("h_double_arrow", "bd_double_arrow")
 * -- neither of which is in the core font.  With nothing answering, GDK
 * settles for the plain arrow, so the divider between two panes and a list's
 * column edge looked like nothing could be done there.
 *
 * The link pointer is the plain arrow: every name a hand is asked for by --
 * "pointer" and "hand" from GTK and WebKit, the core font's "hand1" and
 * "hand2", which libX11 also looks up in the theme -- gets the core arrow
 * itself, so pointing at a link, a button or an attachment does not change
 * the pointer.  The core font's hand looked out of place over a web page.
 *
 * Why GENERATED rather than committed as binaries: the artwork is geometry --
 * a ring and an arc at an angle that moves -- so it is shorter and far more
 * editable as the code that draws it than as a directory of binary files
 * nobody can diff.  Same reasoning as host/gen-unicode-tables.c.  No image
 * library is involved: every shape is a predicate over the plane, sampled 4x4
 * per pixel for antialiasing.
 *
 * The file format is taken from the ported libXcursor's own header
 * (ports/xorg/libXcursor-*\/include/X11/Xcursor/Xcursor.h), not guessed:
 *
 *   file header : magic, header length, version, table-of-contents count
 *   toc entry   : type, subtype (nominal size), absolute file position
 *   image chunk : chunk header, then width, height, xhot, yhot, delay,
 *                 then width*height ARGB pixels, little-endian
 *
 * Pixels are PREMULTIPLIED alpha, which is what Xcursor and the Render
 * extension expect; straight alpha renders with bright fringes on the edges.
 *
 * The plain ring is written at several nominal sizes in one file.  Xcursor
 * picks the size closest to what the client asked for, so shipping 24/32/48
 * means the cursor stays sharp instead of being scaled from a single bitmap.
 * The pointer variant is written at ONE size -- see build_ptr_busy for why it
 * must not scale.
 *
 * Usage: gen-cursors <output-directory>
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Xcursor file format ------------------------------------------------ */

#define XCURSOR_MAGIC 0x72756358u /* "Xcur", little-endian */
#define XCURSOR_FILE_VERSION 0x00010000u
#define XCURSOR_FILE_HEADER_LEN (4 * 4)
#define XCURSOR_FILE_TOC_LEN (3 * 4)
#define XCURSOR_CHUNK_HEADER_LEN (4 * 4)
#define XCURSOR_IMAGE_TYPE 0xfffd0002u
#define XCURSOR_IMAGE_VERSION 1u
#define XCURSOR_IMAGE_HEADER_LEN (XCURSOR_CHUNK_HEADER_LEN + (5 * 4))

/* ---- Canvas ------------------------------------------------------------- */

#define SS 4 /* subsamples per axis */

typedef struct {
	double r, g, b, a; /* 0..1, r/g/b PREMULTIPLIED by a */
} pixel;

typedef struct {
	int w, h;
	pixel *p;
} canvas;

typedef struct {
	double r, g, b, a; /* 0..1, straight */
} colour;

static canvas *canvas_new(int w, int h)
{
	canvas *c = calloc(1, sizeof *c);

	c->w = w;
	c->h = h;
	c->p = calloc((size_t)w * h, sizeof *c->p);
	return c;
}

static void canvas_free(canvas *c)
{
	free(c->p);
	free(c);
}

/* Source-over one pixel, premultiplied; `cov' is the coverage 0..1. */
static void blend(pixel *d, colour col, double cov)
{
	double sa = cov * col.a;
	double inv = 1.0 - sa;

	d->r = col.r * sa + d->r * inv;
	d->g = col.g * sa + d->g * inv;
	d->b = col.b * sa + d->b * inv;
	d->a = sa + d->a * inv;
}

/* A shape is a predicate over DESIGN coordinates. */
typedef int (*shape_fn)(double x, double y, const void *ctx);

/*
 * How design coordinates land on the canvas.  `scale' maps design units to
 * pixels and `ox'/`oy' place the design origin, so the same predicates draw
 * the ring on its own and tucked beside an arrow, at any size, with no second
 * copy of the geometry.
 */
typedef struct {
	double scale;
	double ox, oy;
} xform;

static void fill_shape(canvas *c, shape_fn in, const void *ctx, colour col,
		       const xform *xf)
{
	const double step = 1.0 / SS;
	const double unit = 1.0 / (SS * SS);

	for (int py = 0; py < c->h; py++) {
		for (int px = 0; px < c->w; px++) {
			double cov = 0.0;

			for (int sy = 0; sy < SS; sy++)
				for (int sx = 0; sx < SS; sx++) {
					double x = (px + (sx + 0.5) * step -
						    xf->ox) / xf->scale;
					double y = (py + (sy + 0.5) * step -
						    xf->oy) / xf->scale;

					if (in(x, y, ctx))
						cov += unit;
				}
			if (cov > 0.0)
				blend(&c->p[(size_t)py * c->w + px], col, cov);
		}
	}
}

static unsigned int *canvas_argb(const canvas *c)
{
	unsigned int *out = calloc((size_t)c->w * c->h, sizeof *out);

	for (int i = 0; i < c->w * c->h; i++) {
		const pixel *p = &c->p[i];
		int A = (int)(p->a * 255.0 + 0.5);
		int R = (int)(p->r * 255.0 + 0.5);
		int G = (int)(p->g * 255.0 + 0.5);
		int B = (int)(p->b * 255.0 + 0.5);

		if (A < 0)
			A = 0;
		if (A > 255)
			A = 255;
		/* Premultiplied invariant: no channel may exceed alpha, or the
		 * compositor produces colours brighter than the source. */
		if (R < 0)
			R = 0;
		if (R > A)
			R = A;
		if (G < 0)
			G = 0;
		if (G > A)
			G = A;
		if (B < 0)
			B = 0;
		if (B > A)
			B = A;

		out[i] = ((unsigned)A << 24) | ((unsigned)R << 16) |
			 ((unsigned)G << 8) | (unsigned)B;
	}
	return out;
}

/* ---- Geometry: the busy ring -------------------------------------------- */

/*
 * The ring is an annulus around (cx, cy) between radii `inner' and `outer',
 * white, with a dark one-pixel line along both edges so it reads on a white
 * page and on a dark panel alike.  On it runs the ARC: a stretch of the ring
 * in the accent blue, its head opaque and its tail fading, that turns once a
 * cycle.
 *
 * The dimensions are a parameter, not constants, because the same drawing is
 * made at two very different sizes: on its own it fills a 32-unit design grid
 * that is scaled to the cursor size; beside the pointer it is eleven pixels
 * across, drawn at scale 1, where a line thinner than a pixel is just grey.
 */
typedef struct {
	double cx, cy;
	double outer, inner;
} ring;

typedef struct {
	const ring *r;
	double grow; /* added to the outer and taken from the inner radius */
} ring_ctx;

static int in_ring(double x, double y, const void *vctx)
{
	const ring_ctx *c = vctx;
	double d = hypot(x - c->r->cx, y - c->r->cy);

	return d <= c->r->outer + c->grow && d >= c->r->inner - c->grow;
}

/*
 * How much of the arc covers the point (x, y): 0 off the arc, 1 at its head,
 * falling towards its tail, which is ARC_SPAN behind the head (angles run
 * clockwise from twelve o'clock, in radians).  A weight rather than a
 * predicate, so the fade is smooth instead of a row of stripes.
 */
#define ARC_SPAN (0.36 * 2.0 * M_PI)

static double arc_weight(const ring *r, double head, double x, double y)
{
	double dx = x - r->cx, dy = y - r->cy;
	double d = hypot(dx, dy);
	double a, behind;

	if (d > r->outer || d < r->inner)
		return 0.0;
	a = atan2(dx, -dy);
	behind = fmod(head - a + 4.0 * M_PI, 2.0 * M_PI);
	if (behind > ARC_SPAN)
		return 0.0;
	return 0.3 + 0.7 * pow(1.0 - behind / ARC_SPAN, 1.5);
}

static void fill_arc(canvas *c, const ring *r, double head, colour col,
		     const xform *xf)
{
	const double step = 1.0 / SS;
	const double unit = 1.0 / (SS * SS);

	for (int py = 0; py < c->h; py++) {
		for (int px = 0; px < c->w; px++) {
			double cov = 0.0;

			for (int sy = 0; sy < SS; sy++)
				for (int sx = 0; sx < SS; sx++) {
					double x = (px + (sx + 0.5) * step -
						    xf->ox) / xf->scale;
					double y = (py + (sy + 0.5) * step -
						    xf->oy) / xf->scale;

					cov += unit * arc_weight(r, head, x, y);
				}
			if (cov > 0.0)
				blend(&c->p[(size_t)py * c->w + px], col, cov);
		}
	}
}

/* ---- Geometry: the pointer ---------------------------------------------- */

/*
 * The pointer this cursor is a variant of is NOT drawn here.  It is the X core
 * cursor font's left_ptr, exactly as the server shows it over every other
 * window: this theme defines no left_ptr, so XcursorLibraryLoadCursor falls
 * back to XCreateGlyphCursor on glyphs 68/69 of cursor.pcf, black on white.
 * A pointer that changes size or colour the moment it goes busy looks like a
 * different pointer, and the switch back and forth is what the eye sees; so
 * the busy variant carries those same pixels, and only adds to them.
 *
 * Transcribed from ports/xorg/font-cursor-misc-1.0.4/cursor.bdf: the 10x16
 * mask glyph (BBX 10 16 -1 -15) is the white shape, and the 8x14 source
 * glyph (BBX 8 14 0 -14) is black on top of it, both placed at their shared
 * origin -- which is where the hot spot goes, one pixel in from the corner.
 */
#define PTR_W 10
#define PTR_H 16
#define PTR_XHOT 1
#define PTR_YHOT 1

static const char CORE_PTR[PTR_H][PTR_W + 1] = {
	"..        ",
	".#.       ",
	".##.      ",
	".###.     ",
	".####.    ",
	".#####.   ",
	".######.  ",
	".#######. ",
	".########.",
	".#####....",
	".##.##.   ",
	".#. .##.  ",
	"..  .##.  ",
	"     .##. ",
	"     .##. ",
	"      ..  ",
};

/* The same rows, in the form the one-image cursors below take. */
static const char *const CORE_PTR_ROWS[PTR_H] = {
	CORE_PTR[0], CORE_PTR[1], CORE_PTR[2], CORE_PTR[3],
	CORE_PTR[4], CORE_PTR[5], CORE_PTR[6], CORE_PTR[7],
	CORE_PTR[8], CORE_PTR[9], CORE_PTR[10], CORE_PTR[11],
	CORE_PTR[12], CORE_PTR[13], CORE_PTR[14], CORE_PTR[15],
};

/* ---- Palette ------------------------------------------------------------ */

/*
 * Black and white for the core-font construction of the bitmap pointers, and
 * the ring's own colours: white with an ink-coloured edge, and the LikeOS
 * highlight blue for the arc.
 */
static const colour OUTLINE = { 0.0, 0.0, 0.0, 1.0 };	   /* #000000 */
static const colour HILITE = { 1.0, 1.0, 1.0, 1.0 };	   /* #ffffff */
static const colour RING_EDGE = { 0.078, 0.102, 0.141, 0.92 }; /* #141a24 */
static const colour RING = { 1.0, 1.0, 1.0, 1.0 };	   /* #ffffff */
static const colour ARC = { 0.231, 0.455, 0.941, 1.0 };	   /* #3b74f0 */

/* ---- Frame assembly ----------------------------------------------------- */

/* The ring, then the arc on it with its head at angle `head'. */
static void draw_ring(canvas *c, const ring *r, double head, const xform *xf)
{
	const double one = 1.0 / xf->scale; /* one pixel, in design units */
	ring_ctx edge = { r, one };
	ring_ctx body = { r, 0.0 };

	fill_shape(c, in_ring, &edge, RING_EDGE, xf);
	fill_shape(c, in_ring, &body, RING, xf);
	fill_arc(c, r, head, ARC, xf);
}

/* The core pointer, pixel for pixel, its corner at (x0, y0).  Drawn last so
 * that nothing the ring does touches it. */
static void draw_core_ptr(canvas *c, int x0, int y0)
{
	for (int y = 0; y < PTR_H; y++)
		for (int x = 0; x < PTR_W; x++) {
			char ch = CORE_PTR[y][x];
			pixel *d;

			if (ch == ' ')
				continue;
			d = &c->p[(size_t)(y0 + y) * c->w + (x0 + x)];
			blend(d, ch == '#' ? OUTLINE : HILITE, 1.0);
		}
}

/* ---- Xcursor writing ---------------------------------------------------- */

typedef struct {
	int size, w, h, xhot, yhot, delay;
	unsigned int *px;
} frame;

static void put32(FILE *f, unsigned int v)
{
	fputc((int)(v & 0xff), f);
	fputc((int)((v >> 8) & 0xff), f);
	fputc((int)((v >> 16) & 0xff), f);
	fputc((int)((v >> 24) & 0xff), f);
}

static void write_xcursor(const char *path, const frame *fr, int n)
{
	FILE *f = fopen(path, "wb");
	unsigned int pos;

	if (!f) {
		perror(path);
		exit(1);
	}

	put32(f, XCURSOR_MAGIC);
	put32(f, XCURSOR_FILE_HEADER_LEN);
	put32(f, XCURSOR_FILE_VERSION);
	put32(f, (unsigned)n);

	/* Chunk positions are absolute and computable up front: header, then
	 * the whole table, then the chunks in table order. */
	pos = XCURSOR_FILE_HEADER_LEN + (unsigned)n * XCURSOR_FILE_TOC_LEN;
	for (int i = 0; i < n; i++) {
		put32(f, XCURSOR_IMAGE_TYPE);
		put32(f, (unsigned)fr[i].size);
		put32(f, pos);
		pos += XCURSOR_IMAGE_HEADER_LEN +
		       (unsigned)(fr[i].w * fr[i].h) * 4;
	}

	for (int i = 0; i < n; i++) {
		put32(f, XCURSOR_IMAGE_HEADER_LEN);
		put32(f, XCURSOR_IMAGE_TYPE);
		put32(f, (unsigned)fr[i].size);
		put32(f, XCURSOR_IMAGE_VERSION);
		put32(f, (unsigned)fr[i].w);
		put32(f, (unsigned)fr[i].h);
		put32(f, (unsigned)fr[i].xhot);
		put32(f, (unsigned)fr[i].yhot);
		put32(f, (unsigned)fr[i].delay);
		for (int k = 0; k < fr[i].w * fr[i].h; k++)
			put32(f, fr[i].px[k]);
	}
	fclose(f);
}

/* ---- Cursor construction ------------------------------------------------ */

/*
 * The cycle: the arc goes once round the ring in NFRAMES steps, clockwise,
 * about a second per turn.
 */
#define NFRAMES 16
#define FRAME_MS 60

/* Where the head of the arc is in frame `i'. */
static double frame_head(int i)
{
	return 2.0 * M_PI * (double)i / NFRAMES;
}

static const int SIZES[] = { 24, 32, 48 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

/* The ring on its own, on the 32-unit design grid. */
static const ring BIG_RING = {
	.cx = 16.0, .cy = 16.0, .outer = 10.5, .inner = 6.0,
};

/*
 * The plain busy ring.  Hot spot at the centre of the ring, which is where a
 * user reads the pointer as being while it is busy.
 */
static void build_busy(const char *dir, const char *name)
{
	frame fr[NSIZES * NFRAMES];
	int n = 0;

	for (int s = 0; s < NSIZES; s++) {
		int px = SIZES[s];
		xform xf = { .scale = px / 32.0, .ox = 0, .oy = 0 };

		for (int i = 0; i < NFRAMES; i++) {
			canvas *c = canvas_new(px, px);

			draw_ring(c, &BIG_RING, frame_head(i), &xf);
			fr[n].size = px;
			fr[n].w = fr[n].h = px;
			fr[n].xhot = (int)(BIG_RING.cx * xf.scale);
			fr[n].yhot = (int)(BIG_RING.cy * xf.scale);
			fr[n].delay = FRAME_MS;
			fr[n].px = canvas_argb(c);
			canvas_free(c);
			n++;
		}
	}

	char path[1024];
	snprintf(path, sizeof path, "%s/cursors/%s", dir, name);
	write_xcursor(path, fr, n);
	for (int i = 0; i < n; i++)
		free(fr[i].px);
}

/*
 * The pointer plus a small ring: "working, but the interface still responds".
 * The pointer is the core font's, pixel for pixel (see CORE_PTR), and the
 * ring sits to its lower right, one pixel clear of the widest row.  The hot
 * spot is the pointer's, because that is what the user is pointing with.
 *
 * ONE image, at ONE nominal size.  The core pointer is a bitmap the server
 * shows at a fixed size whatever XCURSOR_SIZE says, and this cursor has to
 * match it exactly, so it must not come in sizes either: Xcursor takes the
 * one image on offer for any size a client asks.
 */
#define SMALL_X (PTR_W + 1) /* left edge of the ring's box */
#define SMALL_Y 5	     /* top edge: the ring hangs beside the tail */
#define SMALL_D 11	     /* the box is SMALL_D pixels square */

static const ring SMALL_RING = {
	.cx = SMALL_D / 2.0, .cy = SMALL_D / 2.0,
	.outer = SMALL_D / 2.0 - 1.0, .inner = SMALL_D / 2.0 - 3.6,
};

static void build_ptr_busy(const char *dir, const char *name)
{
	const int W = SMALL_X + SMALL_D;
	const int H = SMALL_Y + SMALL_D > PTR_H ? SMALL_Y + SMALL_D : PTR_H;
	frame fr[NFRAMES];
	xform xf = { .scale = 1.0, .ox = SMALL_X, .oy = SMALL_Y };

	for (int i = 0; i < NFRAMES; i++) {
		canvas *c = canvas_new(W, H);

		draw_ring(c, &SMALL_RING, frame_head(i), &xf);
		draw_core_ptr(c, 0, 0);
		fr[i].size = 32;
		fr[i].w = W;
		fr[i].h = H;
		fr[i].xhot = PTR_XHOT;
		fr[i].yhot = PTR_YHOT;
		fr[i].delay = FRAME_MS;
		fr[i].px = canvas_argb(c);
		canvas_free(c);
	}

	char path[1024];
	snprintf(path, sizeof path, "%s/cursors/%s", dir, name);
	write_xcursor(path, fr, NFRAMES);
	for (int i = 0; i < NFRAMES; i++)
		free(fr[i].px);
}

/* ---- The resize and link pointers ----------------------------------------- */

/*
 * One-image cursors in the core font's construction: black body ('#') on a
 * one-pixel white surround ('.'), shown at one fixed size like the core arrow
 * they sit beside (see build_ptr_busy on why such a cursor must not come in
 * sizes).
 *
 * The horizontal and vertical double arrows are the core glyphs, transcribed
 * from ports/xorg/font-cursor-misc-1.0.4/cursor.bdf as CORE_PTR is
 * (sb_h_double_arrow and sb_v_double_arrow, each over its mask).  The core font has no diagonal double arrow -- its corner cursors
 * are an arrow into a bracket -- so that one is drawn here in the same
 * weight: two solid heads with five-pixel legs, a two-pixel stair-step shaft
 * between them, and the white surround grown around the lot (diag_arrow).
 */
typedef struct {
	int w, h, xhot, yhot;
	const char *const *rows;
} bitmap_cursor;

static const char *const H_ARROW_ROWS[] = {
	"   ..     ..   ",
	"  .#.     .#.  ",
	" .##.......##. ",
	".#############.",
	"####.......####",
	".#############.",
	" .##.......##. ",
	"  .#.     .#.  ",
	"   ..     ..   ",
};
static const bitmap_cursor H_ARROW = { 15, 9, 7, 4, H_ARROW_ROWS };

static const char *const V_ARROW_ROWS[] = {
	"   .#.   ",
	"  .###.  ",
	" .#####. ",
	".#######.",
	"...#.#...",
	"  .#.#.  ",
	"  .#.#.  ",
	"  .#.#.  ",
	"  .#.#.  ",
	"  .#.#.  ",
	"...#.#...",
	".#######.",
	" .#####. ",
	"  .###.  ",
	"   .#.   ",
};
static const bitmap_cursor V_ARROW = { 9, 15, 4, 7, V_ARROW_ROWS };

/* The core arrow, for the names a link pointer goes by. */
static const bitmap_cursor LINK_ARROW = { PTR_W, PTR_H, PTR_XHOT, PTR_YHOT,
					  CORE_PTR_ROWS };

#define DIAG_N 15

/* The diagonal double arrow from the top-left corner to the bottom-right one
 * ("\"), or mirrored left to right ("/").  Rows are written into `buf'. */
static bitmap_cursor diag_arrow(char buf[DIAG_N][DIAG_N + 1],
				const char *rows[DIAG_N], int mirror)
{
	const int n = DIAG_N, leg = 5;
	static int body[DIAG_N][DIAG_N];

	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			int X = n - 1 - x, Y = n - 1 - y;

			body[y][x] =
				(x >= 1 && y >= 1 && x + y <= leg + 1) ||
				(X >= 1 && Y >= 1 && X + Y <= leg + 1) ||
				((x - y == 0 || x - y == 1) && x >= 1 &&
				 x <= n - 2 && y >= 1 && y <= n - 2);
		}
	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++) {
			int edge = 0;

			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int yy = y + dy, xx = x + dx;

					if (yy >= 0 && yy < n && xx >= 0 &&
					    xx < n && body[yy][xx])
						edge = 1;
				}
			buf[y][mirror ? n - 1 - x : x] =
				body[y][x] ? '#' : (edge ? '.' : ' ');
		}
		buf[y][n] = '\0';
		rows[y] = buf[y];
	}
	return (bitmap_cursor){ n, n, n / 2, n / 2, rows };
}

static void build_bitmap(const char *dir, const char *name,
			 const bitmap_cursor *bc)
{
	canvas *c = canvas_new(bc->w, bc->h);
	frame fr;

	for (int y = 0; y < bc->h; y++)
		for (int x = 0; x < bc->w; x++) {
			char ch = bc->rows[y][x];

			if (ch != ' ')
				blend(&c->p[(size_t)y * c->w + x],
				      ch == '#' ? OUTLINE : HILITE, 1.0);
		}
	fr.size = 32;
	fr.w = bc->w;
	fr.h = bc->h;
	fr.xhot = bc->xhot;
	fr.yhot = bc->yhot;
	fr.delay = 0;
	fr.px = canvas_argb(c);
	canvas_free(c);

	char path[1024];
	snprintf(path, sizeof path, "%s/cursors/%s", dir, name);
	write_xcursor(path, &fr, 1);
	free(fr.px);
}

/* Aliases are written as copies rather than symlinks: the image is built into
 * an ext4 filesystem by a tool that need not preserve links, and the files are
 * small. */
static void copy_file(const char *dir, const char *from, const char *to)
{
	char src[1024], dst[1024];
	FILE *a, *b;
	char buf[8192];
	size_t n;

	snprintf(src, sizeof src, "%s/cursors/%s", dir, from);
	snprintf(dst, sizeof dst, "%s/cursors/%s", dir, to);
	a = fopen(src, "rb");
	if (!a) {
		perror(src);
		exit(1);
	}
	b = fopen(dst, "wb");
	if (!b) {
		perror(dst);
		exit(1);
	}
	while ((n = fread(buf, 1, sizeof buf, a)) > 0)
		fwrite(buf, 1, n, b);
	fclose(a);
	fclose(b);
}

int main(int argc, char **argv)
{
	const char *dir;
	char path[1024];
	FILE *f;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <theme-directory>\n", argv[0]);
		return 2;
	}
	dir = argv[1];

	build_busy(dir, "watch");
	build_ptr_busy(dir, "left_ptr_watch");

	/* The names different toolkits ask for.  GTK's GDK_WATCH and the CSS
	 * "wait" both mean the plain busy ring; "progress" and "half-busy" mean
	 * the pointer variant.  A name a theme does not provide falls back to
	 * the core cursor font, so every spelling has to be present. */
	copy_file(dir, "watch", "wait");
	copy_file(dir, "left_ptr_watch", "progress");
	copy_file(dir, "left_ptr_watch", "half-busy");

	/* The resize pointers, under every name they are asked for by: the
	 * core font's (which libX11 looks up in the theme too, so ctwm's
	 * window-edge cursors are these), GTK's CSS names, and the older
	 * theme names GTK falls back to.  Edges get the double arrow across
	 * them and corners the diagonal one, which says "drag to resize"
	 * more plainly than the core font's arrow-into-a-bracket. */
	static const char *const h_names[] = {
		"h_double_arrow", "col-resize", "ew-resize", "e-resize",
		"w-resize", "left_side", "right_side", "size_hor", NULL
	};
	static const char *const v_names[] = {
		"v_double_arrow", "row-resize", "ns-resize", "n-resize",
		"s-resize", "top_side", "bottom_side", "size_ver", NULL
	};
	static const char *const nwse_names[] = {
		"nwse-resize", "nw-resize", "se-resize", "top_left_corner",
		"bottom_right_corner", "size_fdiag", NULL
	};
	static const char *const nesw_names[] = {
		"nesw-resize", "ne-resize", "sw-resize", "top_right_corner",
		"bottom_left_corner", "size_bdiag", NULL
	};
	static const char *const link_names[] = {
		"pointer", "pointing_hand", "hand", "hand1", "hand2", NULL
	};
	char diag_buf[DIAG_N][DIAG_N + 1];
	const char *diag_rows[DIAG_N];
	bitmap_cursor diag;

	build_bitmap(dir, "sb_h_double_arrow", &H_ARROW);
	for (int i = 0; h_names[i]; i++)
		copy_file(dir, "sb_h_double_arrow", h_names[i]);
	build_bitmap(dir, "sb_v_double_arrow", &V_ARROW);
	for (int i = 0; v_names[i]; i++)
		copy_file(dir, "sb_v_double_arrow", v_names[i]);
	diag = diag_arrow(diag_buf, diag_rows, 0);
	build_bitmap(dir, "bd_double_arrow", &diag);
	for (int i = 0; nwse_names[i]; i++)
		copy_file(dir, "bd_double_arrow", nwse_names[i]);
	diag = diag_arrow(diag_buf, diag_rows, 1);
	build_bitmap(dir, "fd_double_arrow", &diag);
	for (int i = 0; nesw_names[i]; i++)
		copy_file(dir, "fd_double_arrow", nesw_names[i]);
	build_bitmap(dir, link_names[0], &LINK_ARROW);
	for (int i = 1; link_names[i]; i++)
		copy_file(dir, link_names[0], link_names[i]);

	/* The directory is also the LikeOS icon theme (res/xorg/icons), whose
	 * own index.theme lists its icon directories and inherits Adwaita too
	 * -- libXcursor reads nothing but Inherits.  Whichever is staged
	 * first, that file must win: writing ours over it would hide every
	 * icon in the theme. */
	snprintf(path, sizeof path, "%s/index.theme", dir);
	if (access(path, F_OK) == 0) {
		printf("cursor theme written to %s (index.theme kept)\n", dir);
		return 0;
	}
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return 1;
	}
	/* Inherits from Adwaita so any cursor this theme does NOT define is
	 * looked for there before falling back to the core font. */
	fprintf(f, "[Icon Theme]\n"
		   "Name=LikeOS\n"
		   "Comment=LikeOS cursors\n"
		   "Inherits=Adwaita\n");
	fclose(f);

	printf("cursor theme written to %s\n", dir);
	return 0;
}
