/*
 * gen-cursors -- generate the LikeOS X cursor theme.
 *
 * Writes Xcursor-format files for the BUSY cursors: an animated hourglass in
 * the style of the one Windows XP used, and the pointer-with-hourglass that
 * means "starting, but you can still click".
 *
 * Why this exists: with no cursor theme installed, libXcursor falls back to the
 * X core cursor font, whose "watch" is a 1-bit black-and-white wristwatch from
 * the 1980s.  Xcursor's format is ARGB and supports animation, so the busy
 * cursor can be something a person recognises.
 *
 * Why GENERATED rather than committed as binaries: the artwork is geometry --
 * two funnels, a neck, and a sand level that moves -- so it is shorter and far
 * more editable as the code that draws it than as a directory of binary files
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
 * The plain hourglass is written at several nominal sizes in one file.
 * Xcursor picks the size closest to what the client asked for, so shipping
 * 24/32/48 means the cursor stays sharp instead of being scaled from a single
 * bitmap.  The pointer variant is written at ONE size -- see build_ptr_watch
 * for why it must not scale.
 *
 * Usage: gen-cursors <output-directory>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
 * the hourglass on its own and tucked beside an arrow, at any size, with no
 * second copy of the geometry.
 *
 * The hourglass TURNS OVER at the end of its cycle, which is what an
 * hourglass does and what the cursor it imitates did, and there are two ways
 * to show that, both about the centre (rcx, rcy): `rot' spins the drawing in
 * the plane, and `sy' squashes it vertically, through zero, to the mirror
 * image -- a turn about the horizontal axis, seen edge-on in the middle
 * frame.  The spin is the better animation when there is room for it; the
 * turn is what the small glass beside the pointer uses, because a nine-pixel
 * drawing spun through 60 degrees is a smear, and it would sweep across the
 * pointer while it turned.
 */
typedef struct {
	double scale;
	double ox, oy;
	double rot;
	double sy;
	double rcx, rcy;
} xform;

/*
 * The pixel being rasterised, for shapes defined on the PIXEL grid rather
 * than the design grid (the sand's checkerboard).  fill_shape sets it before
 * sampling a pixel, so every subsample of that pixel sees the same value and
 * such a shape is wholly in or wholly out of a pixel -- never antialiased
 * into a grey smear, whatever the scale.
 */
static int cur_px, cur_py;

static void fill_shape(canvas *c, shape_fn in, const void *ctx, colour col,
		       const xform *xf)
{
	const double step = 1.0 / SS;
	const double unit = 1.0 / (SS * SS);
	const double cs = cos(-xf->rot), sn = sin(-xf->rot);

	for (int py = 0; py < c->h; py++) {
		for (int px = 0; px < c->w; px++) {
			double cov = 0.0;

			cur_px = px;
			cur_py = py;
			for (int sy = 0; sy < SS; sy++)
				for (int sx = 0; sx < SS; sx++) {
					double x = (px + (sx + 0.5) * step -
						    xf->ox) / xf->scale;
					double y = (py + (sy + 0.5) * step -
						    xf->oy) / xf->scale;

					if (xf->sy != 1.0)
						y = xf->rcy +
						    (y - xf->rcy) / xf->sy;
					if (xf->rot != 0.0) {
						double rx = x - xf->rcx;
						double ry = y - xf->rcy;

						x = xf->rcx + rx * cs - ry * sn;
						y = xf->rcy + rx * sn + ry * cs;
					}
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

/* ---- Geometry: the hourglass -------------------------------------------- */

/*
 * The glass is two funnels meeting at a neck, capped top and bottom:
 *
 *        left        right
 *   top   +------------+     cap
 *           \        /       upper funnel
 * neck_y        ><           neck
 *           /        \       lower funnel
 *   bot   +------------+     cap
 *
 * The dimensions are a parameter, not constants, because the same drawing is
 * made at two very different sizes: on its own it fills a 32-unit design grid
 * that is scaled to the cursor size; beside the pointer it is nine pixels
 * wide, drawn at scale 1 with every straight edge on a pixel boundary,
 * because at that size a half-pixel edge is a grey line.
 */
typedef struct {
	double left, right, top, bot; /* the outer box */
	double cap;		      /* thickness of the solid end bars */
	double neck_y, neck_hw;	      /* height and half-width of the neck */
} glass;

static double glass_cx(const glass *g)
{
	return (g->left + g->right) / 2.0;
}

static double glass_cy(const glass *g)
{
	return (g->top + g->bot) / 2.0;
}

static double glass_halfwidth(const glass *g, double y)
{
	const double full = (g->right - g->left) / 2.0;
	const double top_in = g->top + g->cap;
	const double bot_in = g->bot - g->cap;

	if (y <= top_in || y >= bot_in)
		return full;
	if (y < g->neck_y) {
		double t = (y - top_in) / (g->neck_y - top_in);
		return full + (g->neck_hw - full) * t;
	}
	{
		double t = (y - g->neck_y) / (bot_in - g->neck_y);
		return g->neck_hw + (full - g->neck_hw) * t;
	}
}

typedef struct {
	const glass *g;
	double inset;
} inset_ctx;

static int in_glass(double x, double y, const void *vctx)
{
	const inset_ctx *c = vctx;
	const glass *g = c->g;
	double d = c->inset;

	if (y < g->top + d || y > g->bot - d)
		return 0;
	return fabs(x - glass_cx(g)) <= glass_halfwidth(g, y) - d;
}

/*
 * Sand.  `f' runs 0..1 through the animation: the upper chamber drains and the
 * lower fills, both measured from the neck, so the two levels move in step and
 * the sand looks conserved.
 */
typedef struct {
	const glass *g;
	double f;
	double inset;
	double stream_hw;
} sand_ctx;

/*
 * Grains, not a fill.  A checkerboard over the PIXEL grid, so at every size
 * exactly every other pixel is set, as the cursor this imitates does it.
 * Without it the sand reads as a solid block of colour, which is the single
 * biggest thing that made an earlier attempt look wrong; and with the board
 * on the design grid instead, any size other than the nominal one blurred it
 * into a grey wash.
 */
static int sand_dither(void)
{
	return ((cur_px + cur_py) & 1) == 0;
}

static int in_sand_top(double x, double y, const void *vctx)
{
	const sand_ctx *s = vctx;
	const glass *g = s->g;
	inset_ctx ic = { g, s->inset };
	double top_in = g->top + g->cap;
	double surface = top_in + (g->neck_y - top_in) * s->f;

	if (y < surface || y > g->neck_y)
		return 0;
	if (!sand_dither())
		return 0;
	return in_glass(x, y, &ic);
}

static int in_sand_bottom(double x, double y, const void *vctx)
{
	const sand_ctx *s = vctx;
	const glass *g = s->g;
	inset_ctx ic = { g, s->inset };
	double bot_in = g->bot - g->cap;
	double surface = bot_in - (bot_in - g->neck_y) * s->f;

	if (y < surface || y > bot_in)
		return 0;
	if (!sand_dither())
		return 0;
	return in_glass(x, y, &ic);
}

static int in_stream(double x, double y, const void *vctx)
{
	const sand_ctx *s = vctx;
	const glass *g = s->g;
	double bot_in = g->bot - g->cap;
	double surface = bot_in - (bot_in - g->neck_y) * s->f;

	/* No stream at the very start or end of the cycle -- an hourglass that
	 * is full or empty is not pouring. */
	if (s->f <= 0.05 || s->f >= 0.95)
		return 0;
	if (y < g->neck_y || y > surface)
		return 0;
	return fabs(x - glass_cx(g)) <= s->stream_hw;
}

/* The end caps, drawn solid.  On the cursor this imitates, the frame is a
 * filled dark bar across the top and the bottom -- not a hollow outline -- and
 * that is most of what makes the silhouette recognisable at 32 pixels. */
static int in_caps(double x, double y, const void *vctx)
{
	const inset_ctx *c = vctx;
	const glass *g = c->g;

	if (fabs(x - glass_cx(g)) > (g->right - g->left) / 2.0)
		return 0;
	return (y >= g->top && y <= g->top + g->cap) ||
	       (y >= g->bot - g->cap && y <= g->bot);
}

/* The lit edge: a white stripe just inside the left wall of each chamber,
 * between the two insets.  Together with the darker BEVEL laid under the body
 * this gives the glass the shaded, three-dimensional look the original has --
 * flat silver on its own looks like a sticker. */
typedef struct {
	const glass *g;
	double from, to;
} hilite_ctx;

static int in_highlight(double x, double y, const void *vctx)
{
	const hilite_ctx *h = vctx;
	const glass *g = h->g;
	inset_ctx a = { g, h->from }, b = { g, h->to };

	if (y < g->top + g->cap + 0.5 || y > g->bot - g->cap - 0.5)
		return 0;
	if (x > glass_cx(g))
		return 0; /* left half only */
	return in_glass(x, y, &a) && !in_glass(x, y, &b);
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

/* ---- Palette ------------------------------------------------------------ */

/*
 * The four colours the classic hourglass cursor is built from: silver body,
 * black outline, mid-grey bevel and a white highlight.  The sand is not a
 * colour at all -- it is the black, stippled onto the body in a checkerboard,
 * which is what makes it read as grains rather than as a filled shape.
 */
static const colour OUTLINE = { 0.0, 0.0, 0.0, 1.0 };	   /* #000000 */
static const colour GLASS = { 0.753, 0.753, 0.753, 1.0 };  /* #c0c0c0 */
static const colour BEVEL = { 0.502, 0.502, 0.502, 1.0 };  /* #808080 */
static const colour HILITE = { 1.0, 1.0, 1.0, 1.0 };	   /* #ffffff */
static const colour SAND = { 0.0, 0.0, 0.0, 1.0 };

/* ---- Frame assembly ----------------------------------------------------- */

static void draw_hourglass(canvas *c, const glass *g, double f,
			   const xform *xf)
{
	const double one = 1.0 / xf->scale; /* one pixel, in design units */
	/*
	 * At the design size the outline is 0.9 units of black with 0.7 of
	 * darker grey bevelled inside it.  Where one pixel is wider than that
	 * outline -- the small glass beside the pointer -- the bevel would
	 * be a fraction of a pixel of antialiasing noise, so the outline
	 * becomes exactly one pixel of black and the body starts right
	 * inside it; the highlight likewise shrinks to one pixel.
	 */
	const int small = one > 0.9;
	const double edge = small ? one : 1.6;
	inset_ctx outer = { g, 0.0 };
	inset_ctx bevel = { g, 0.9 };
	inset_ctx inner = { g, edge };
	hilite_ctx hi = { g, small ? edge : 2.0, small ? edge + one : 4.0 };
	sand_ctx sand = { g, f, edge, small ? one / 2.0 : 0.6 };

	fill_shape(c, in_glass, &outer, OUTLINE, xf);
	if (!small)
		fill_shape(c, in_glass, &bevel, BEVEL, xf);
	fill_shape(c, in_glass, &inner, GLASS, xf);
	fill_shape(c, in_highlight, &hi, HILITE, xf);
	fill_shape(c, in_sand_top, &sand, SAND, xf);
	fill_shape(c, in_sand_bottom, &sand, SAND, xf);
	fill_shape(c, in_stream, &sand, SAND, xf);
	fill_shape(c, in_caps, &outer, OUTLINE, xf);
}

/* The core pointer, pixel for pixel, its corner at (x0, y0).  Drawn last so
 * that nothing the hourglass does -- including turning over -- touches it. */
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
 * The cycle: the sand drains, then the glass turns over and it starts again.
 * DRAIN_FRAMES show the sand running out; FLIP_FRAMES take the whole thing
 * through half a turn, by spinning it or by turning it over (see xform).
 */
#define DRAIN_FRAMES 10
#define FLIP_FRAMES 3
#define NFRAMES (DRAIN_FRAMES + FLIP_FRAMES)
#define FRAME_MS 110

enum flip { SPIN, TURN };

/* Fill level and orientation for frame `i' of the cycle. */
static void frame_state(int i, enum flip how, double *f, xform *xf)
{
	double angle;

	xf->rot = 0.0;
	xf->sy = 1.0;
	if (i < DRAIN_FRAMES) {
		*f = (double)i / (DRAIN_FRAMES - 1);
		return;
	}
	/* Turning over: drained, so the sand is all in the bottom -- which the
	 * half turn carries up to the top for the next pass. */
	*f = 1.0;
	angle = 3.14159265358979 * (double)(i - DRAIN_FRAMES + 1) /
		(double)FLIP_FRAMES;
	if (how == SPIN)
		xf->rot = angle;
	else
		xf->sy = cos(angle);
}

static const int SIZES[] = { 24, 32, 48 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

/* The glass on its own, on the 32-unit design grid. */
static const glass BIG_GLASS = {
	.left = 9.5, .right = 22.5, .top = 4.0, .bot = 28.0,
	.cap = 2.0, .neck_y = 16.0, .neck_hw = 1.0,
};

/*
 * The plain hourglass.  Hot spot at the centre of the glass, which is where a
 * user reads the pointer as being while it is busy.
 */
static void build_watch(const char *dir, const char *name)
{
	frame fr[NSIZES * NFRAMES];
	int n = 0;

	for (int s = 0; s < NSIZES; s++) {
		int px = SIZES[s];
		xform xf = { .scale = px / 32.0, .ox = 0, .oy = 0,
			     .rcx = glass_cx(&BIG_GLASS),
			     .rcy = glass_cy(&BIG_GLASS) };

		for (int i = 0; i < NFRAMES; i++) {
			canvas *c = canvas_new(px, px);
			double f;

			frame_state(i, SPIN, &f, &xf);
			draw_hourglass(c, &BIG_GLASS, f, &xf);
			fr[n].size = px;
			fr[n].w = fr[n].h = px;
			fr[n].xhot = (int)(glass_cx(&BIG_GLASS) * xf.scale);
			fr[n].yhot = (int)(BIG_GLASS.neck_y * xf.scale);
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
 * The pointer plus a small hourglass: "working, but the interface still
 * responds".  The pointer is the core font's, pixel for pixel (see CORE_PTR),
 * and the hourglass sits to its right, one pixel clear of the widest row,
 * with its top level with the pointer's tip.  The hot spot is the pointer's,
 * because that is what the user is pointing with.
 *
 * ONE image, at ONE nominal size.  The core pointer is a bitmap the server
 * shows at a fixed size whatever XCURSOR_SIZE says, and this cursor has to
 * match it exactly, so it must not come in sizes either: Xcursor takes the
 * one image on offer for any size a client asks.
 */
#define SMALL_X (PTR_W + 1)  /* left edge of the glass */
#define SMALL_Y 1	      /* level with the tip of the pointer */

static const glass SMALL_GLASS = {
	.left = 0.0, .right = 9.0, .top = 0.0, .bot = 15.0,
	.cap = 1.5, .neck_y = 7.5, .neck_hw = 0.5,
};

static void build_ptr_watch(const char *dir, const char *name)
{
	const int W = SMALL_X + (int)SMALL_GLASS.right;
	const int H = PTR_H;
	frame fr[NFRAMES];
	xform xf = { .scale = 1.0, .ox = SMALL_X, .oy = SMALL_Y,
		     .rcx = glass_cx(&SMALL_GLASS),
		     .rcy = glass_cy(&SMALL_GLASS) };

	for (int i = 0; i < NFRAMES; i++) {
		canvas *c = canvas_new(W, H);
		double f;

		frame_state(i, TURN, &f, &xf);
		draw_hourglass(c, &SMALL_GLASS, f, &xf);
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

	build_watch(dir, "watch");
	build_ptr_watch(dir, "left_ptr_watch");

	/* The names different toolkits ask for.  GTK's GDK_WATCH and the CSS
	 * "wait" both mean the plain hourglass; "progress" and "half-busy" mean
	 * the pointer variant.  A name a theme does not provide falls back to
	 * the core cursor font, so every spelling has to be present. */
	copy_file(dir, "watch", "wait");
	copy_file(dir, "left_ptr_watch", "progress");
	copy_file(dir, "left_ptr_watch", "half-busy");

	snprintf(path, sizeof path, "%s/index.theme", dir);
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return 1;
	}
	/* Inherits from Adwaita so any cursor this theme does NOT define is
	 * looked for there before falling back to the core font. */
	fprintf(f, "[Icon Theme]\n"
		   "Name=LikeOS\n"
		   "Comment=LikeOS-64 cursors\n"
		   "Inherits=Adwaita\n");
	fclose(f);

	printf("cursor theme written to %s\n", dir);
	return 0;
}
