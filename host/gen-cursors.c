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
 * Each cursor is written at several nominal sizes in one file.  Xcursor picks
 * the size closest to what the client asked for, so shipping 24/32/48 means the
 * cursor stays sharp instead of being scaled from a single bitmap.
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

/* A shape is a predicate over DESIGN coordinates (the 32x32 grid below). */
typedef int (*shape_fn)(double x, double y, const void *ctx);

/*
 * Rasterise one shape.  `scale' maps design units to pixels and `ox'/`oy'
 * shift the shape within the image, so the same predicates draw the hourglass
 * on its own and tucked beside an arrow, at any size, with no second copy of
 * the geometry.
 */
static void fill_shape(canvas *c, shape_fn in, const void *ctx, colour col,
		       double scale, double ox, double oy, double rot)
{
	const double step = 1.0 / SS;
	const double unit = 1.0 / (SS * SS);
	const double cs = cos(-rot), sn = sin(-rot);

	for (int py = 0; py < c->h; py++) {
		for (int px = 0; px < c->w; px++) {
			double cov = 0.0;

			for (int sy = 0; sy < SS; sy++)
				for (int sx = 0; sx < SS; sx++) {
					double x = (px + (sx + 0.5) * step - ox) / scale;
					double y = (py + (sy + 0.5) * step - oy) / scale;

					if (rot != 0.0) {
						/* Rotate about the design centre.
						 * The hourglass TURNS OVER at the
						 * end of its cycle, which is what
						 * an hourglass does and what the
						 * cursor it imitates did. */
						double rx = x - 16.0;
						double ry = y - 16.0;

						x = 16.0 + rx * cs - ry * sn;
						y = 16.0 + rx * sn + ry * cs;
					}
					if (in(x, y, ctx))
						cov += unit;
				}
			if (cov <= 0.0)
				continue;

			/* Source-over, premultiplied. */
			pixel *d = &c->p[(size_t)py * c->w + px];
			double sa = cov * col.a;
			double inv = 1.0 - sa;

			d->r = col.r * sa + d->r * inv;
			d->g = col.g * sa + d->g * inv;
			d->b = col.b * sa + d->b * inv;
			d->a = sa + d->a * inv;
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
 * Design grid is 32x32.  The glass is two funnels meeting at a neck, capped
 * top and bottom:
 *
 *        7            25
 *    3   +------------+     cap
 *          \        /       upper funnel
 *   16         ><           neck
 *          /        \       lower funnel
 *   29   +------------+     cap
 */
#define G_LEFT 9.5
#define G_RIGHT 22.5
#define G_TOP 4.0
#define G_BOT 28.0
#define G_CAP 2.0
#define G_NECK_Y 16.0
#define G_NECK_HW 1.0

static double glass_halfwidth(double y)
{
	const double full = (G_RIGHT - G_LEFT) / 2.0;
	const double top_in = G_TOP + G_CAP;
	const double bot_in = G_BOT - G_CAP;

	if (y <= top_in || y >= bot_in)
		return full;
	if (y < G_NECK_Y) {
		double t = (y - top_in) / (G_NECK_Y - top_in);
		return full + (G_NECK_HW - full) * t;
	}
	{
		double t = (y - G_NECK_Y) / (bot_in - G_NECK_Y);
		return G_NECK_HW + (full - G_NECK_HW) * t;
	}
}

#define G_CX ((G_LEFT + G_RIGHT) / 2.0)

typedef struct {
	double inset;
} inset_ctx;

static int in_glass(double x, double y, const void *vctx)
{
	const inset_ctx *c = vctx;
	double d = c->inset;

	if (y < G_TOP + d || y > G_BOT - d)
		return 0;
	return fabs(x - G_CX) <= glass_halfwidth(y) - d;
}

/*
 * Sand.  `f' runs 0..1 through the animation: the upper chamber drains and the
 * lower fills, both measured from the neck, so the two levels move in step and
 * the sand looks conserved.
 */
typedef struct {
	double f;
	double inset;
} sand_ctx;

/*
 * Grains, not a fill.  A checkerboard over the DESIGN grid, so at the nominal
 * 32px size every other pixel is set exactly as the cursor this imitates does
 * it; at other sizes the same pattern scales with the rest of the drawing.
 * Without it the sand reads as a solid block of colour, which is the single
 * biggest thing that made an earlier attempt look wrong.
 */
static int sand_dither(double x, double y)
{
	int ix = (int)floor(x), iy = (int)floor(y);

	return ((ix + iy) & 1) == 0;
}

static int in_sand_top(double x, double y, const void *vctx)
{
	const sand_ctx *s = vctx;
	inset_ctx ic = { s->inset };
	double top_in = G_TOP + G_CAP;
	double surface = top_in + (G_NECK_Y - top_in) * s->f;

	if (y < surface || y > G_NECK_Y)
		return 0;
	if (!sand_dither(x, y))
		return 0;
	return in_glass(x, y, &ic);
}

static int in_sand_bottom(double x, double y, const void *vctx)
{
	const sand_ctx *s = vctx;
	inset_ctx ic = { s->inset };
	double bot_in = G_BOT - G_CAP;
	double surface = bot_in - (bot_in - G_NECK_Y) * s->f;

	if (y < surface || y > bot_in)
		return 0;
	if (!sand_dither(x, y))
		return 0;
	return in_glass(x, y, &ic);
}

static int in_stream(double x, double y, const void *vctx)
{
	const sand_ctx *s = vctx;
	double bot_in = G_BOT - G_CAP;
	double surface = bot_in - (bot_in - G_NECK_Y) * s->f;

	/* No stream at the very start or end of the cycle -- an hourglass that
	 * is full or empty is not pouring. */
	if (s->f <= 0.05 || s->f >= 0.95)
		return 0;
	if (y < G_NECK_Y || y > surface)
		return 0;
	return fabs(x - G_CX) <= 0.6;
}

/* ---- Geometry: the arrow ------------------------------------------------ */

/*
 * The conventional X left_ptr outline, hot spot at (0,0), as a polygon in
 * design units.  Point-in-polygon by ray crossing -- exact, and it keeps the
 * shape readable as a list of corners instead of a stack of half-planes.
 */
static const double ARROW[][2] = {
	{ 0.0, 0.0 },	{ 0.0, 16.4 }, { 4.1, 12.6 },  { 6.7, 18.6 },
	{ 9.6, 17.3 },	{ 7.0, 11.5 }, { 12.0, 11.3 },
};
#define ARROW_N ((int)(sizeof ARROW / sizeof ARROW[0]))

static int point_in_poly(double x, double y, const double poly[][2], int n)
{
	int inside = 0;

	for (int i = 0, j = n - 1; i < n; j = i++) {
		double xi = poly[i][0], yi = poly[i][1];
		double xj = poly[j][0], yj = poly[j][1];

		if ((yi > y) != (yj > y) &&
		    x < (xj - xi) * (y - yi) / (yj - yi) + xi)
			inside = !inside;
	}
	return inside;
}

static int in_arrow(double x, double y, const void *ctx)
{
	(void)ctx;
	return point_in_poly(x, y, ARROW, ARROW_N);
}

/* The white body: the same polygon shrunk towards its centroid, which gives a
 * uniform dark border without needing a second hand-drawn outline. */
static int in_arrow_body(double x, double y, const void *ctx)
{
	static double inner[ARROW_N][2];
	static int built;
	(void)ctx;

	if (!built) {
		double cx = 0, cy = 0;

		for (int i = 0; i < ARROW_N; i++) {
			cx += ARROW[i][0];
			cy += ARROW[i][1];
		}
		cx /= ARROW_N;
		cy /= ARROW_N;
		for (int i = 0; i < ARROW_N; i++) {
			double dx = ARROW[i][0] - cx, dy = ARROW[i][1] - cy;
			double len = sqrt(dx * dx + dy * dy);
			double k = len > 0 ? (len - 1.15) / len : 0;

			inner[i][0] = cx + dx * k;
			inner[i][1] = cy + dy * k;
		}
		built = 1;
	}
	return point_in_poly(x, y, inner, ARROW_N);
}

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

/* The end caps, drawn solid.  On the cursor this imitates, the frame is a
 * filled dark bar across the top and the bottom -- not a hollow outline -- and
 * that is most of what makes the silhouette recognisable at 32 pixels. */
static int in_caps(double x, double y, const void *ctx)
{
	(void)ctx;
	if (fabs(x - G_CX) > (G_RIGHT - G_LEFT) / 2.0)
		return 0;
	return (y >= G_TOP && y <= G_TOP + G_CAP) ||
	       (y >= G_BOT - G_CAP && y <= G_BOT);
}

/* The lit edge: a two-pixel white stripe just inside the left wall of each
 * chamber.  Together with the darker BEVEL laid under the body this gives the
 * glass the shaded, three-dimensional look the original has -- flat silver on
 * its own looks like a sticker. */
static int in_highlight(double x, double y, const void *ctx)
{
	inset_ctx in2 = { 2.0 }, in4 = { 4.0 };
	(void)ctx;

	if (y < G_TOP + G_CAP + 0.5 || y > G_BOT - G_CAP - 0.5)
		return 0;
	if (x > G_CX)
		return 0; /* left half only */
	return in_glass(x, y, &in2) && !in_glass(x, y, &in4);
}

static void draw_hourglass(canvas *c, double f, double scale, double ox,
			   double oy, double rot)
{
	inset_ctx outer = { 0.0 };
	inset_ctx bevel = { 0.9 };
	inset_ctx inner = { 1.6 };
	sand_ctx sand = { f, 1.6 };

	fill_shape(c, in_glass, &outer, OUTLINE, scale, ox, oy, rot);
	fill_shape(c, in_glass, &bevel, BEVEL, scale, ox, oy, rot);
	fill_shape(c, in_glass, &inner, GLASS, scale, ox, oy, rot);
	fill_shape(c, in_highlight, NULL, HILITE, scale, ox, oy, rot);
	fill_shape(c, in_sand_top, &sand, SAND, scale, ox, oy, rot);
	fill_shape(c, in_sand_bottom, &sand, SAND, scale, ox, oy, rot);
	fill_shape(c, in_stream, &sand, SAND, scale, ox, oy, rot);
	fill_shape(c, in_caps, NULL, OUTLINE, scale, ox, oy, rot);
}

static void draw_arrow(canvas *c, double scale, double ox, double oy)
{
	fill_shape(c, in_arrow, NULL, OUTLINE, scale, ox, oy, 0.0);
	fill_shape(c, in_arrow_body, NULL, GLASS, scale, ox, oy, 0.0);
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
 * DRAIN_FRAMES show the sand running out; FLIP_FRAMES rotate the whole thing
 * through half a turn, which is why fill_shape takes a rotation at all.
 */
#define DRAIN_FRAMES 10
#define FLIP_FRAMES 3
#define NFRAMES (DRAIN_FRAMES + FLIP_FRAMES)
#define FRAME_MS 110

/* Fill level and rotation for frame `i' of the cycle. */
static void frame_state(int i, double *f, double *rot)
{
	if (i < DRAIN_FRAMES) {
		*f = (double)i / (DRAIN_FRAMES - 1);
		*rot = 0.0;
	} else {
		/* Turning over: drained, so the sand is all in the bottom --
		 * which the rotation carries up to the top for the next pass. */
		*f = 1.0;
		*rot = 3.14159265358979 * (double)(i - DRAIN_FRAMES + 1) /
		       (double)FLIP_FRAMES;
	}
}
static const int SIZES[] = { 24, 32, 48 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

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
		double scale = px / 32.0;

		for (int i = 0; i < NFRAMES; i++) {
			canvas *c = canvas_new(px, px);
			double f, rot;

			frame_state(i, &f, &rot);
			draw_hourglass(c, f, scale, 0, 0, rot);
			fr[n].size = px;
			fr[n].w = fr[n].h = px;
			fr[n].xhot = (int)(G_CX * scale);
			fr[n].yhot = (int)(G_NECK_Y * scale);
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
 * Arrow plus a small hourglass: "working, but the interface still responds".
 * The hourglass is drawn at 60% and tucked to the lower right of the arrow, and
 * the hot spot stays at the arrow's tip because that is what the user is
 * pointing with.
 */
static void build_ptr_watch(const char *dir, const char *name)
{
	frame fr[NSIZES * NFRAMES];
	int n = 0;

	for (int s = 0; s < NSIZES; s++) {
		int px = SIZES[s];
		double scale = px / 32.0;

		for (int i = 0; i < NFRAMES; i++) {
			canvas *c = canvas_new(px, px);
			double f, rot;

			frame_state(i, &f, &rot);
			draw_hourglass(c, f, scale * 0.58,
				       px * 0.40, px * 0.34, rot);
			draw_arrow(c, scale, 0.5 * scale, 0.5 * scale);
			fr[n].size = px;
			fr[n].w = fr[n].h = px;
			fr[n].xhot = 0;
			fr[n].yhot = 0;
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
