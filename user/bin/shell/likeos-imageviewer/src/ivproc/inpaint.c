// LikeOS -- LikeOS Image Viewer: Retouch inpainting.
//
// Removes what the user painted over by rebuilding it from the rest of the
// picture, in three steps:
//
//  1. Exemplar-based filling (priority = confidence x data term).  The
//     hole is filled patch by patch from its border inwards; the next
//     patch is the one with the most known pixels (confidence) lying where
//     a strong edge (isophote) runs into the hole (data term), so lines
//     and boundaries are continued before flat areas.  Each patch is
//     copied from the most similar fully known patch (sum of squared
//     differences over its known pixels) found in a window around it.
//
//  2. Multi-scale refinement for large holes.  The exemplar fill runs on a
//     downscaled pyramid level where the hole is small; every finer level
//     starts from the upsampled result and refines it by
//     expectation-maximisation: for each patch overlapping the hole the
//     nearest source patch is found (randomised nearest-neighbour search
//     with propagation), then every hole pixel becomes the
//     similarity-weighted vote of all patches covering it.  The same
//     refinement also runs once at the base level, which dissolves the
//     seams between the copied patches.
//
//  3. Seam blend.  A membrane correction removes any remaining brightness
//     or colour step along the hole border: the low-frequency mismatch
//     measured on the border is interpolated smoothly across the fill and
//     added to it, so the copied texture meets its surroundings.
//
// Separate holes are solved one after the other in their own region of
// interest, so a handful of dust spots costs no more than their size.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

enum { ST_KNOWN = 0, ST_FILL = 1, ST_BLOCKED = 2 };

/* Holes larger than this (in pixels, at a level) go one pyramid level down. */
#define COARSE_HOLE 40
#define MAX_LEVELS 6

typedef struct {
	int w, h;
	float *c;		/* 4 floats (r, g, b, a: 0..255) per pixel */
	uint8_t *st;		/* ST_* per pixel */
} Lvl;

typedef struct {
	IvProgress cb;
	void *user;
	double base, span;	/* progress range of the current hole */
	int cancelled;
	int calls;
} Prog;

static int prog(Prog *p, double f)
{
	if (p->cancelled)
		return 1;
	if (p->cb == NULL)
		return 0;
	if (p->cb(p->base + p->span * ivp_clampd(f, 0.0, 1.0), p->user)) {
		p->cancelled = 1;
		return 1;
	}
	return 0;
}

static void lvl_free(Lvl *l)
{
	free(l->c);
	free(l->st);
	l->c = NULL;
	l->st = NULL;
}

static int lvl_alloc(Lvl *l, int w, int h)
{
	l->w = w;
	l->h = h;
	l->c = ivp_alloc((size_t)w, (size_t)h, 4 * sizeof(float));
	l->st = ivp_alloc((size_t)w, (size_t)h, 1);
	if (l->c == NULL || l->st == NULL) {
		lvl_free(l);
		return -1;
	}
	return 0;
}

/* Half-size level: a coarse pixel is to be filled when any of its
 * children is, blocked when any is blocked, and its colour is the mean of
 * its known children. */
static int lvl_down(const Lvl *f, Lvl *c)
{
	int w = (f->w + 1) / 2, h = (f->h + 1) / 2;
	if (lvl_alloc(c, w, h) < 0)
		return -1;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float acc[4] = { 0, 0, 0, 0 }, all[4] = { 0, 0, 0, 0 };
			int nk = 0, na = 0, fill = 0, blocked = 0;
			for (int j = 0; j < 2; j++) {
				int yy = 2 * y + j;
				if (yy >= f->h)
					continue;
				for (int i = 0; i < 2; i++) {
					int xx = 2 * x + i;
					if (xx >= f->w)
						continue;
					size_t k = (size_t)yy * f->w + xx;
					const float *p = f->c + k * 4;
					for (int q = 0; q < 4; q++)
						all[q] += p[q];
					na++;
					if (f->st[k] == ST_FILL)
						fill = 1;
					else if (f->st[k] == ST_BLOCKED)
						blocked = 1;
					else {
						for (int q = 0; q < 4; q++)
							acc[q] += p[q];
						nk++;
					}
				}
			}
			size_t k = (size_t)y * w + x;
			float *d = c->c + k * 4;
			for (int q = 0; q < 4; q++)
				d[q] = nk ? acc[q] / nk : (na ? all[q] / na : 0.0f);
			c->st[k] = fill ? ST_FILL : (blocked ? ST_BLOCKED : ST_KNOWN);
		}
	}
	return 0;
}

static int hole_extent(const Lvl *l)
{
	int x0 = l->w, y0 = l->h, x1 = -1, y1 = -1;
	for (int y = 0; y < l->h; y++)
		for (int x = 0; x < l->w; x++)
			if (l->st[(size_t)y * l->w + x] == ST_FILL) {
				if (x < x0)
					x0 = x;
				if (x > x1)
					x1 = x;
				if (y < y0)
					y0 = y;
				y1 = y;
			}
	if (x1 < 0)
		return 0;
	int a = x1 - x0 + 1, b = y1 - y0 + 1;
	return a > b ? a : b;
}

/* ------------------------------------------------------------------------
 * Source validity
 * --------------------------------------------------------------------- */

/* valid[k] = 1 when the patch of half size p centred on k lies inside
 * the level and every pixel of it is ST_KNOWN.  Also returns the list of
 * valid centres for random sampling. */
static uint8_t *valid_map(const Lvl *l, int p, int **list, int *nlist)
{
	const int w = l->w, h = l->h;
	*list = NULL;
	*nlist = 0;
	uint32_t *I = ivp_zalloc((size_t)w + 1, (size_t)h + 1, sizeof(uint32_t));
	uint8_t *v = ivp_zalloc((size_t)w, (size_t)h, 1);
	if (I == NULL || v == NULL) {
		free(I);
		free(v);
		return NULL;
	}
	/* Integral image of "not usable". */
	for (int y = 0; y < h; y++) {
		uint32_t run = 0;
		for (int x = 0; x < w; x++) {
			run += l->st[(size_t)y * w + x] != ST_KNOWN;
			I[(size_t)(y + 1) * (w + 1) + x + 1] = I[(size_t)y * (w + 1) + x + 1] + run;
		}
	}
	int n = 0;
	for (int y = p; y < h - p; y++) {
		for (int x = p; x < w - p; x++) {
			int x0 = x - p, y0 = y - p, x1 = x + p + 1, y1 = y + p + 1;
			uint32_t s = I[(size_t)y1 * (w + 1) + x1] - I[(size_t)y0 * (w + 1) + x1] -
				     I[(size_t)y1 * (w + 1) + x0] + I[(size_t)y0 * (w + 1) + x0];
			if (s == 0) {
				v[(size_t)y * w + x] = 1;
				n++;
			}
		}
	}
	free(I);
	if (n > 0) {
		*list = malloc(sizeof(int) * (size_t)n);
		if (*list == NULL) {
			free(v);
			return NULL;
		}
		int k = 0;
		for (int i = 0; i < w * h; i++)
			if (v[i])
				(*list)[k++] = i;
		*nlist = n;
	}
	return v;
}

/* ------------------------------------------------------------------------
 * Fallback: onion-peel diffusion (no source patch anywhere)
 * --------------------------------------------------------------------- */

static void diffuse_fill(Lvl *l)
{
	const int w = l->w, h = l->h;
	uint8_t *known = malloc((size_t)w * h);
	if (known == NULL)
		return;
	for (int i = 0; i < w * h; i++)
		known[i] = l->st[i] == ST_KNOWN;
	for (int pass = 0; pass < w + h; pass++) {
		int changed = 0;
		uint8_t *next = malloc((size_t)w * h);
		if (next == NULL)
			break;
		memcpy(next, known, (size_t)w * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				size_t k = (size_t)y * w + x;
				if (known[k] || l->st[k] != ST_FILL)
					continue;
				float acc[4] = { 0, 0, 0, 0 };
				int n = 0;
				for (int j = -1; j <= 1; j++)
					for (int i = -1; i <= 1; i++) {
						int xx = x + i, yy = y + j;
						if (xx < 0 || yy < 0 || xx >= w || yy >= h)
							continue;
						size_t q = (size_t)yy * w + xx;
						if (!known[q])
							continue;
						for (int c = 0; c < 4; c++)
							acc[c] += l->c[q * 4 + c];
						n++;
					}
				if (n) {
					for (int c = 0; c < 4; c++)
						l->c[k * 4 + c] = acc[c] / n;
					next[k] = 1;
					changed = 1;
				}
			}
		memcpy(known, next, (size_t)w * h);
		free(next);
		if (!changed)
			break;
	}
	free(known);
}

/* ------------------------------------------------------------------------
 * Step 1: exemplar-based filling
 * --------------------------------------------------------------------- */

static inline float lum4(const float *p)
{
	return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

/* SSD between the target patch at t (known pixels only) and the source
 * patch at s; stops early once above `best`. */
static float ssd_known(const Lvl *l, const uint8_t *known, int tx, int ty,
		       int sx, int sy, int p, float best)
{
	const int w = l->w, h = l->h;
	float sum = 0.0f;
	for (int j = -p; j <= p; j++) {
		int ty2 = ty + j;
		if (ty2 < 0 || ty2 >= h)
			continue;
		const float *srow = l->c + ((size_t)(sy + j) * w + sx) * 4;
		const float *trow = l->c + ((size_t)ty2 * w + tx) * 4;
		const uint8_t *krow = known + (size_t)ty2 * w + tx;
		for (int i = -p; i <= p; i++) {
			int tx2 = tx + i;
			if (tx2 < 0 || tx2 >= w || !krow[i])
				continue;
			const float *a = trow + i * 4, *b = srow + i * 4;
			float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2],
			      d3 = a[3] - b[3];
			sum += d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3;
		}
		if (sum > best)
			return sum;
	}
	return sum;
}

static int exemplar_fill(Lvl *l, int p, Prog *pr)
{
	const int w = l->w, h = l->h;
	int *vlist = NULL, nv = 0;
	uint8_t *valid = valid_map(l, p, &vlist, &nv);
	if (valid == NULL)
		return -1;
	free(vlist);
	if (nv == 0) {
		free(valid);
		diffuse_fill(l);
		return 0;
	}
	uint8_t *known = malloc((size_t)w * h);
	float *conf = malloc(sizeof(float) * (size_t)w * h);
	float *lum = malloc(sizeof(float) * (size_t)w * h);
	if (known == NULL || conf == NULL || lum == NULL) {
		free(valid);
		free(known);
		free(conf);
		free(lum);
		return -1;
	}
	int total = 0;
	int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			size_t k = (size_t)y * w + x;
			/* Blocked pixels (other, not yet treated holes) are
			 * neither copied from nor compared against. */
			known[k] = l->st[k] == ST_KNOWN;
			conf[k] = known[k] ? 1.0f : 0.0f;
			lum[k] = lum4(l->c + k * 4);
			if (l->st[k] == ST_FILL) {
				total++;
				if (x < bx0)
					bx0 = x;
				if (x > bx1)
					bx1 = x;
				if (y < by0)
					by0 = y;
				by1 = y;
			}
		}
	/* Search window: the hole size times three, at least 24 pixels; the
	 * search over it is done on a stride-2 grid and refined around the
	 * winner. */
	int ext = (bx1 - bx0 > by1 - by0 ? bx1 - bx0 : by1 - by0) + 1;
	int R = ext * 3;
	if (R < 24)
		R = 24;
	const float area = (float)((2 * p + 1) * (2 * p + 1));
	/* Distance penalty in SSD units: 0.01 level^2 per channel and pixel
	 * for every squared pixel of distance, over about half a patch of
	 * known pixels -- 30 px away costs as much as a 3-level mismatch. */
	const float dist_w = 0.01f * 4.0f * area * 0.5f;
	int remaining = total, steps = 0;

	while (remaining > 0) {
		/* -- pick the front pixel with the highest priority -------- */
		float bestP = -1.0f;
		int bpx = -1, bpy = -1;
		for (int y = by0; y <= by1; y++) {
			for (int x = bx0; x <= bx1; x++) {
				size_t k = (size_t)y * w + x;
				if (known[k] || l->st[k] != ST_FILL)
					continue;
				int front = (x > 0 && known[k - 1]) || (x < w - 1 && known[k + 1]) ||
					    (y > 0 && known[k - w]) || (y < h - 1 && known[k + w]);
				if (!front)
					continue;
				/* Confidence: mean confidence over the patch. */
				float C = 0.0f;
				float gbest = 0.0f, gx = 0.0f, gy = 0.0f;
				for (int j = -p; j <= p; j++) {
					int yy = y + j;
					if (yy < 1 || yy >= h - 1)
						continue;
					for (int i = -p; i <= p; i++) {
						int xx = x + i;
						if (xx < 1 || xx >= w - 1)
							continue;
						size_t q = (size_t)yy * w + xx;
						if (!known[q])
							continue;
						C += conf[q];
						/* Isophote: strongest known gradient in the
						 * patch (central differences over known
						 * neighbours only). */
						if (known[q - 1] && known[q + 1] && known[q - w] &&
						    known[q + w]) {
							float dx = (lum[q + 1] - lum[q - 1]) * 0.5f;
							float dy = (lum[q + w] - lum[q - w]) * 0.5f;
							float m = dx * dx + dy * dy;
							if (m > gbest) {
								gbest = m;
								gx = dx;
								gy = dy;
							}
						}
					}
				}
				C /= area;
				/* Normal of the fill front from the known map. */
				float nx = 0.0f, ny = 0.0f;
				for (int j = -1; j <= 1; j++)
					for (int i = -1; i <= 1; i++) {
						int xx = ivp_clampi(x + i, 0, w - 1);
						int yy = ivp_clampi(y + j, 0, h - 1);
						float kv = known[(size_t)yy * w + xx] ? 1.0f : 0.0f;
						float wgt = (i == 0 || j == 0) ? 2.0f : 1.0f;
						nx += kv * (float)i * wgt;
						ny += kv * (float)j * wgt;
					}
				float nl = sqrtf(nx * nx + ny * ny);
				float D = 0.0f;
				if (nl > 1e-6f) {
					/* |isophote . n|, isophote = (-gy, gx). */
					D = fabsf(-gy * nx + gx * ny) / nl / 255.0f;
				}
				float P = C * (D + 0.02f);
				if (P > bestP) {
					bestP = P;
					bpx = x;
					bpy = y;
				}
			}
		}
		if (bpx < 0)
			break;	/* unreachable pixels (enclosed by blocked ones) */

		/* -- best source patch ------------------------------------- */
		float best = INFINITY;
		int bsx = -1, bsy = -1;
		int sx0 = ivp_clampi(bpx - R, p, w - 1 - p), sx1 = ivp_clampi(bpx + R, p, w - 1 - p);
		int sy0 = ivp_clampi(bpy - R, p, h - 1 - p), sy1 = ivp_clampi(bpy + R, p, h - 1 - p);
		for (int pass = 0; pass < 2 && bsx < 0; pass++) {
			for (int sy = sy0; sy <= sy1; sy += 2) {
				for (int sx = sx0 + ((sy >> 1) & 1); sx <= sx1; sx += 2) {
					if (!valid[(size_t)sy * w + sx])
						continue;
					float d = ssd_known(l, known, bpx, bpy, sx, sy, p, best);
					/* A preference for nearby sources: they share
					 * the local light and colour, so where several
					 * patches match equally well the fill stays
					 * coherent instead of mixing far-off tones. */
					float dx = (float)(sx - bpx), dy = (float)(sy - bpy);
					d += (dx * dx + dy * dy) * dist_w;
					if (d < best) {
						best = d;
						bsx = sx;
						bsy = sy;
					}
				}
			}
			if (bsx < 0) {
				/* Nothing valid in the window: search everywhere. */
				sx0 = p;
				sx1 = w - 1 - p;
				sy0 = p;
				sy1 = h - 1 - p;
			}
		}
		if (bsx < 0)
			break;
		/* Refine on the full grid around the winner. */
		int cx = bsx, cy = bsy;
		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++) {
				int sx = cx + i, sy = cy + j;
				if ((i == 0 && j == 0) || sx < p || sy < p || sx > w - 1 - p ||
				    sy > h - 1 - p || !valid[(size_t)sy * w + sx])
					continue;
				float d = ssd_known(l, known, bpx, bpy, sx, sy, p, best);
				float dx = (float)(sx - bpx), dy = (float)(sy - bpy);
				d += (dx * dx + dy * dy) * dist_w;
				if (d < best) {
					best = d;
					bsx = sx;
					bsy = sy;
				}
			}

		/* -- copy the unknown pixels, propagate confidence --------- */
		float Cp = 0.0f;
		for (int j = -p; j <= p; j++)
			for (int i = -p; i <= p; i++) {
				int xx = bpx + i, yy = bpy + j;
				if (xx >= 0 && yy >= 0 && xx < w && yy < h &&
				    known[(size_t)yy * w + xx])
					Cp += conf[(size_t)yy * w + xx];
			}
		Cp /= area;
		for (int j = -p; j <= p; j++) {
			int yy = bpy + j;
			if (yy < 0 || yy >= h)
				continue;
			for (int i = -p; i <= p; i++) {
				int xx = bpx + i;
				if (xx < 0 || xx >= w)
					continue;
				size_t k = (size_t)yy * w + xx;
				if (known[k] || l->st[k] != ST_FILL)
					continue;
				size_t s = (size_t)(bsy + j) * w + (bsx + i);
				memcpy(l->c + k * 4, l->c + s * 4, 4 * sizeof(float));
				lum[k] = lum[s];
				known[k] = 1;
				conf[k] = Cp;
				remaining--;
			}
		}
		if ((++steps & 7) == 0 && prog(pr, 1.0 - (double)remaining / total))
			break;
	}
	free(valid);
	free(known);
	free(conf);
	free(lum);
	if (pr->cancelled)
		return 1;
	if (remaining > 0)
		diffuse_fill(l);
	return 0;
}

/* ------------------------------------------------------------------------
 * Step 2: nearest-neighbour field + voting
 * --------------------------------------------------------------------- */

typedef struct {
	int n;			/* number of target centres */
	int *t;			/* target centre index */
	int *s;			/* matched source centre index */
	float *d;		/* its distance */
	int *map;		/* pixel -> target slot, or -1 */
} Nnf;

static void nnf_free(Nnf *f)
{
	free(f->t);
	free(f->s);
	free(f->d);
	free(f->map);
	memset(f, 0, sizeof(*f));
}

/* Targets: every centre whose patch touches a pixel to fill. */
static int nnf_targets(const Lvl *l, int p, Nnf *f)
{
	const int w = l->w, h = l->h;
	memset(f, 0, sizeof(*f));
	f->map = malloc(sizeof(int) * (size_t)w * h);
	uint8_t *mark = ivp_zalloc((size_t)w, (size_t)h, 1);
	if (f->map == NULL || mark == NULL) {
		free(mark);
		nnf_free(f);
		return -1;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			if (l->st[(size_t)y * w + x] != ST_FILL)
				continue;
			for (int j = -p; j <= p; j++) {
				int yy = y + j;
				if (yy < 0 || yy >= h)
					continue;
				for (int i = -p; i <= p; i++) {
					int xx = x + i;
					if (xx >= 0 && xx < w)
						mark[(size_t)yy * w + xx] = 1;
				}
			}
		}
	int n = 0;
	for (int i = 0; i < w * h; i++)
		n += mark[i];
	f->t = malloc(sizeof(int) * (size_t)(n ? n : 1));
	f->s = malloc(sizeof(int) * (size_t)(n ? n : 1));
	f->d = malloc(sizeof(float) * (size_t)(n ? n : 1));
	if (f->t == NULL || f->s == NULL || f->d == NULL) {
		free(mark);
		nnf_free(f);
		return -1;
	}
	int k = 0;
	for (int i = 0; i < w * h; i++) {
		f->map[i] = -1;
		if (mark[i]) {
			f->map[i] = k;
			f->t[k++] = i;
		}
	}
	f->n = n;
	free(mark);
	return 0;
}

/* SSD over the whole target patch (current estimates included; blocked
 * and out-of-range pixels skipped). */
/*
 * Patch distance for the refinement, tolerant of a brightness/colour
 * offset: with d the differences over the n compared pixels and o their
 * mean per channel, cost = sum d^2 - K n |o|^2.  A source with the right
 * texture under slightly different light then still matches; the vote
 * (below) adds the offset back, so the refinement takes texture from the
 * sources but keeps the local brightness of the current estimate -- this
 * is what lets a fill follow a gradient that exists nowhere else in the
 * picture.  Since n |o|^2 <= sum d^2, cost >= (1 - K) sum d^2, which
 * bounds the early exit.
 */
#define EM_OFFSET_K 0.8f
#define VOTE_OFFSET_K 1.0f

static float ssd_full(const Lvl *l, int t, int s, int p, float best)
{
	const int w = l->w, h = l->h;
	int tx = t % w, ty = t / w, sx = s % w, sy = s / w;
	float sum = 0.0f, sd[4] = { 0, 0, 0, 0 };
	int n = 0;
	const float bound = 1.0f - EM_OFFSET_K;
	for (int j = -p; j <= p; j++) {
		int ty2 = ty + j;
		if (ty2 < 0 || ty2 >= h)
			continue;
		const float *trow = l->c + ((size_t)ty2 * w + tx) * 4;
		const float *srow = l->c + ((size_t)(sy + j) * w + sx) * 4;
		const uint8_t *strow = l->st + (size_t)ty2 * w + tx;
		for (int i = -p; i <= p; i++) {
			int tx2 = tx + i;
			if (tx2 < 0 || tx2 >= w || strow[i] == ST_BLOCKED)
				continue;
			const float *a = trow + i * 4, *b = srow + i * 4;
			for (int c = 0; c < 4; c++) {
				float d = a[c] - b[c];
				sum += d * d;
				sd[c] += d;
			}
			n++;
		}
		if (sum * bound > best)
			return sum;
	}
	if (n > 0) {
		float o2 = 0.0f;
		for (int c = 0; c < 4; c++)
			o2 += sd[c] * sd[c];
		sum -= EM_OFFSET_K * o2 / (float)n;
	}
	return sum;
}

static void nnf_try(const Lvl *l, Nnf *f, int k, int s, int p,
		    const uint8_t *valid)
{
	if (s < 0 || s >= l->w * l->h || !valid[s] || s == f->s[k])
		return;
	float d = ssd_full(l, f->t[k], s, p, f->d[k]);
	if (d < f->d[k]) {
		f->d[k] = d;
		f->s[k] = s;
	}
}

/* Randomised nearest-neighbour search: propagation from the scan-order
 * neighbours plus a shrinking random search around the current match. */
static void nnf_search(const Lvl *l, Nnf *f, int p, const uint8_t *valid,
		       int sweeps, uint32_t seed)
{
	const int w = l->w, h = l->h;
	int maxr = w > h ? w : h;
	for (int sw = 0; sw < sweeps; sw++) {
		int fwd = (sw & 1) == 0;
		int dir = fwd ? 1 : -1;
		for (int ii = 0; ii < f->n; ii++) {
			int k = fwd ? ii : f->n - 1 - ii;
			int t = f->t[k];
			int tx = t % w, ty = t / w;
			/* Propagation: the neighbour's match, shifted. */
			int nb[2] = { tx - dir >= 0 && tx - dir < w ? t - dir : -1,
				      ty - dir >= 0 && ty - dir < h ? t - dir * w : -1 };
			for (int q = 0; q < 2; q++) {
				if (nb[q] < 0)
					continue;
				int kn = f->map[nb[q]];
				if (kn < 0)
					continue;
				int s = f->s[kn];
				int sx = s % w + (q == 0 ? dir : 0);
				int sy = s / w + (q == 1 ? dir : 0);
				if (sx < 0 || sy < 0 || sx >= w || sy >= h)
					continue;
				nnf_try(l, f, k, sy * w + sx, p, valid);
			}
			/* Random search. */
			int bx = f->s[k] % w, by = f->s[k] / w;
			uint32_t hs = ivp_hash32((uint32_t)t * 2654435761u + seed + (uint32_t)sw * 7919u);
			for (int r = maxr; r >= 1; r /= 2) {
				hs = ivp_hash32(hs);
				int rx = bx + (int)(hs % (uint32_t)(2 * r + 1)) - r;
				hs = ivp_hash32(hs);
				int ry = by + (int)(hs % (uint32_t)(2 * r + 1)) - r;
				if (rx < 0 || ry < 0 || rx >= w || ry >= h)
					continue;
				nnf_try(l, f, k, ry * w + rx, p, valid);
			}
		}
	}
}

/* Fill pixels = similarity-weighted average of the overlapping source
 * patches. */
static int vote(Lvl *l, const Nnf *f, int p)
{
	const int w = l->w, h = l->h;
	float *acc = ivp_zalloc((size_t)w, (size_t)h, 5 * sizeof(float));
	if (acc == NULL)
		return -1;
	/* Weight scale from the median per-pixel distance. */
	const float npx = (float)((2 * p + 1) * (2 * p + 1));
	enum { NS = 512 };
	float samp[NS];
	int ns = 0;
	for (int i = 0; i < f->n && ns < NS; i += (f->n / NS) + 1)
		samp[ns++] = f->d[i] / npx;
	float med = 1.0f;
	if (ns > 0) {
		/* Insertion sort is fine for 512 values. */
		for (int i = 1; i < ns; i++) {
			float v = samp[i];
			int j = i - 1;
			while (j >= 0 && samp[j] > v) {
				samp[j + 1] = samp[j];
				j--;
			}
			samp[j + 1] = v;
		}
		med = samp[ns / 2];
	}
	const float inv2s2 = 1.0f / (2.0f * (med + 4.0f));
	for (int k = 0; k < f->n; k++) {
		int t = f->t[k], s = f->s[k];
		int tx = t % w, ty = t / w, sx = s % w, sy = s / w;
		float wt = expf(-(f->d[k] / npx) * inv2s2);
		/* The patch's offset (see ssd_full). */
		float off[4] = { 0, 0, 0, 0 };
		{
			int n = 0;
			for (int j = -p; j <= p; j++) {
				int yy = ty + j;
				if (yy < 0 || yy >= h)
					continue;
				for (int i = -p; i <= p; i++) {
					int xx = tx + i;
					if (xx < 0 || xx >= w || l->st[(size_t)yy * w + xx] == ST_BLOCKED)
						continue;
					const float *a = l->c + ((size_t)yy * w + xx) * 4;
					const float *b = l->c + ((size_t)(sy + j) * w + (sx + i)) * 4;
					for (int c = 0; c < 4; c++)
						off[c] += a[c] - b[c];
					n++;
				}
			}
			for (int c = 0; c < 4; c++)
				off[c] = n ? VOTE_OFFSET_K * off[c] / (float)n : 0.0f;
		}
		for (int j = -p; j <= p; j++) {
			int yy = ty + j;
			if (yy < 0 || yy >= h)
				continue;
			for (int i = -p; i <= p; i++) {
				int xx = tx + i;
				if (xx < 0 || xx >= w)
					continue;
				size_t q = (size_t)yy * w + xx;
				if (l->st[q] != ST_FILL)
					continue;
				const float *src = l->c + ((size_t)(sy + j) * w + (sx + i)) * 4;
				float *a = acc + q * 5;
				a[0] += (src[0] + off[0]) * wt;
				a[1] += (src[1] + off[1]) * wt;
				a[2] += (src[2] + off[2]) * wt;
				a[3] += (src[3] + off[3]) * wt;
				a[4] += wt;
			}
		}
	}
	for (size_t q = 0; q < (size_t)w * h; q++) {
		if (l->st[q] != ST_FILL || acc[q * 5 + 4] <= 0.0f)
			continue;
		float inv = 1.0f / acc[q * 5 + 4];
		for (int c = 0; c < 4; c++)
			l->c[q * 4 + c] = acc[q * 5 + c] * inv;
	}
	free(acc);
	return 0;
}

/*
 * Final reconstruction at full resolution.  Voting averages many
 * overlapping patches: that is what makes the levels converge, but it
 * also smooths fine texture (grain, foliage, fabric).  Copying for every
 * pixel the centre of the source patch matched to the patch centred on it
 * keeps the texture, but leaves steps where neighbouring pixels were
 * matched to differently lit sources.  So the two are combined: the
 * result is the centre copy with its local mean (a (2R+1)^2 box) replaced
 * by the local mean of the voted estimate -- detail from the copy, the
 * smooth part from the vote.
 */
#define DETAIL_R 3

static int reconstruct_detail(Lvl *l, const Nnf *f)
{
	const int w = l->w, h = l->h;
	const size_t n = (size_t)w * h;
	float *cp = malloc(sizeof(float) * 4 * n);
	if (cp == NULL)
		return -1;
	memcpy(cp, l->c, sizeof(float) * 4 * n);
	for (int k = 0; k < f->n; k++) {
		int t = f->t[k];
		if (l->st[t] == ST_FILL)
			memcpy(cp + (size_t)t * 4, l->c + (size_t)f->s[k] * 4, 4 * sizeof(float));
	}
	/* Local means of the copy and of the vote, per channel. */
	float *mc = malloc(sizeof(float) * n);
	float *mv = malloc(sizeof(float) * n);
	if (mc == NULL || mv == NULL) {
		free(mc);
		free(mv);
		free(cp);
		return -1;
	}
	for (int ch = 0; ch < 4; ch++) {
		for (size_t k = 0; k < n; k++) {
			mc[k] = cp[k * 4 + ch];
			mv[k] = l->c[k * 4 + ch];
		}
		ivp_box_plane(mc, w, h, DETAIL_R);
		ivp_box_plane(mv, w, h, DETAIL_R);
		for (size_t k = 0; k < n; k++)
			if (l->st[k] == ST_FILL)
				l->c[k * 4 + ch] = ivp_clampf(cp[k * 4 + ch] - mc[k] + mv[k],
							      0.0f, 255.0f);
	}
	free(mc);
	free(mv);
	free(cp);
	return 0;
}

/* Initialise f from the coarse field (offsets doubled), random where the
 * coarse field has nothing, then compute the distances. */
static void nnf_init(const Lvl *l, Nnf *f, int p, const uint8_t *valid,
		     const int *vlist, int nv, const Nnf *coarse, int cw,
		     uint32_t seed)
{
	const int w = l->w;
	for (int k = 0; k < f->n; k++) {
		int t = f->t[k];
		int tx = t % w, ty = t / w;
		int s = -1;
		if (coarse != NULL) {
			int cx = tx / 2, cy = ty / 2;
			int ci = cy * cw + cx;
			int ck = coarse->map[ci];
			if (ck >= 0) {
				int cs = coarse->s[ck];
				int sx = (cs % cw) * 2 + (tx - 2 * cx);
				int sy = (cs / cw) * 2 + (ty - 2 * cy);
				if (sx >= 0 && sy >= 0 && sx < l->w && sy < l->h &&
				    valid[sy * w + sx])
					s = sy * w + sx;
			}
		}
		if (s < 0)
			s = vlist[ivp_hash32((uint32_t)t ^ seed) % (uint32_t)nv];
		f->s[k] = s;
		f->d[k] = ssd_full(l, t, s, p, INFINITY);
	}
}

/* One level of EM refinement.  coarse may be NULL. */
static int refine_level(Lvl *l, int p, Nnf *f, const Nnf *coarse, int cw,
			int iters, Prog *pr, uint32_t seed)
{
	int *vlist = NULL, nv = 0;
	uint8_t *valid = valid_map(l, p, &vlist, &nv);
	if (valid == NULL)
		return -1;
	if (nv == 0) {
		free(valid);
		free(vlist);
		diffuse_fill(l);
		return 0;
	}
	if (nnf_targets(l, p, f) < 0) {
		free(valid);
		free(vlist);
		return -1;
	}
	nnf_init(l, f, p, valid, vlist, nv, coarse, cw, seed);
	int r = 0;
	for (int it = 0; it < iters; it++) {
		nnf_search(l, f, p, valid, 2, seed + (uint32_t)it * 101u);
		/* Distances were measured against the previous estimate; the
		 * vote makes the new one. */
		if (vote(l, f, p) < 0) {
			r = -1;
			break;
		}
		for (int k = 0; k < f->n; k++)
			f->d[k] = ssd_full(l, f->t[k], f->s[k], p, INFINITY);
		if (prog(pr, (double)(it + 1) / iters)) {
			r = 1;
			break;
		}
	}
	free(valid);
	free(vlist);
	return r;
}

/* ------------------------------------------------------------------------
 * Step 3: seam blend
 * --------------------------------------------------------------------- */

/*
 * The mismatch along the border: for every known pixel next to the fill,
 * the difference between it and the mean of its filled neighbours.  It is
 * smoothed along the border (it contains texture as well as the step we
 * are after), then carried into the fill by membrane interpolation --
 * Shepard weights 1 / d^2 over the border samples, which approximates the
 * smooth (harmonic) correction field -- and added.  The copied texture is
 * kept; only its overall brightness and colour are bent to meet the
 * surroundings.
 */
static void seam_blend(Lvl *l)
{
	const int w = l->w, h = l->h;
	static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
	/* Border samples. */
	int cap = 1024, nb = 0;
	int *bx = malloc(sizeof(int) * (size_t)cap);
	float *be = malloc(sizeof(float) * 4 * (size_t)cap);
	if (bx == NULL || be == NULL) {
		free(bx);
		free(be);
		return;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			size_t k = (size_t)y * w + x;
			if (l->st[k] != ST_KNOWN)
				continue;
			float acc[4] = { 0, 0, 0, 0 };
			int c = 0;
			for (int q = 0; q < 4; q++) {
				int xx = x + dx4[q], yy = y + dy4[q];
				if (xx < 0 || yy < 0 || xx >= w || yy >= h)
					continue;
				size_t j = (size_t)yy * w + xx;
				if (l->st[j] != ST_FILL)
					continue;
				for (int ch = 0; ch < 4; ch++)
					acc[ch] += l->c[j * 4 + ch];
				c++;
			}
			if (!c)
				continue;
			if (nb == cap) {
				cap *= 2;
				int *nbx = realloc(bx, sizeof(int) * (size_t)cap);
				float *nbe = nbx ? realloc(be, sizeof(float) * 4 * (size_t)cap) : NULL;
				if (nbx)
					bx = nbx;
				if (nbe == NULL) {
					free(bx);
					free(be);
					return;
				}
				be = nbe;
			}
			bx[nb] = (int)k;
			for (int ch = 0; ch < 4; ch++)
				be[nb * 4 + ch] = l->c[k * 4 + ch] - acc[ch] / c;
			nb++;
		}
	if (nb == 0) {
		free(bx);
		free(be);
		return;
	}
	/* Smooth along the border: mean of the samples within 4 px. */
	float *bs = malloc(sizeof(float) * 4 * (size_t)nb);
	if (bs == NULL) {
		free(bx);
		free(be);
		return;
	}
	for (int i = 0; i < nb; i++) {
		int xi = bx[i] % w, yi = bx[i] / w;
		float acc[4] = { 0, 0, 0, 0 };
		int c = 0;
		for (int j = 0; j < nb; j++) {
			int dxj = bx[j] % w - xi, dyj = bx[j] / w - yi;
			if (dxj * dxj + dyj * dyj > 16)
				continue;
			for (int ch = 0; ch < 4; ch++)
				acc[ch] += be[j * 4 + ch];
			c++;
		}
		for (int ch = 0; ch < 4; ch++)
			bs[i * 4 + ch] = ivp_clampf(acc[ch] / c, -60.0f, 60.0f);
	}
	/* At most ~600 samples are needed for a smooth field. */
	int stride = nb / 600 + 1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			size_t k = (size_t)y * w + x;
			if (l->st[k] != ST_FILL)
				continue;
			double acc[4] = { 0, 0, 0, 0 }, ws = 0.0;
			for (int i = 0; i < nb; i += stride) {
				double ddx = bx[i] % w - x, ddy = bx[i] / w - y;
				double wt = 1.0 / (ddx * ddx + ddy * ddy + 0.25);
				for (int ch = 0; ch < 4; ch++)
					acc[ch] += wt * bs[i * 4 + ch];
				ws += wt;
			}
			for (int ch = 0; ch < 4; ch++)
				l->c[k * 4 + ch] = ivp_clampf(l->c[k * 4 + ch] + (float)(acc[ch] / ws),
							      0.0f, 255.0f);
		}
	free(bx);
	free(be);
	free(bs);
}

/* ------------------------------------------------------------------------
 * One hole: pyramid, fill, refine, blend
 * --------------------------------------------------------------------- */

/* Smooth (harmonic) fill: onion-peel start, then Jacobi relaxation of
 * every fill pixel towards the mean of its 4 neighbours. */
static void membrane_fill(Lvl *l)
{
	const int w = l->w, h = l->h;
	diffuse_fill(l);
	int ext = hole_extent(l);
	int iters = ext * ext / 4 + 50;
	if (iters > 2000)
		iters = 2000;
	static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
	for (int it = 0; it < iters; it++) {
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				size_t k = (size_t)y * w + x;
				if (l->st[k] != ST_FILL)
					continue;
				float acc[4] = { 0, 0, 0, 0 };
				int c = 0;
				for (int q = 0; q < 4; q++) {
					int xx = x + dx4[q], yy = y + dy4[q];
					if (xx < 0 || yy < 0 || xx >= w || yy >= h)
						continue;
					size_t j = (size_t)yy * w + xx;
					if (l->st[j] == ST_BLOCKED)
						continue;
					for (int ch = 0; ch < 4; ch++)
						acc[ch] += l->c[j * 4 + ch];
					c++;
				}
				if (c)
					for (int ch = 0; ch < 4; ch++)
						l->c[k * 4 + ch] = acc[ch] / (float)c;
			}
	}
}

static double nnf_energy(const Nnf *f)
{
	double s = 0.0;
	for (int k = 0; k < f->n; k++)
		s += f->d[k];
	return f->n ? s / f->n : 0.0;
}

static int patch_half(int img_long, int hole_ext)
{
	int p = img_long > 2500 ? 5 : 4;	/* 11x11 / 9x9 */
	if (hole_ext <= 10)
		p = 3;				/* 7x7 for specks */
	return p;
}

static int solve(Lvl *base, int img_long, Prog *pr)
{
	Lvl lv[MAX_LEVELS];
	memset(lv, 0, sizeof(lv));
	lv[0] = *base;
	int nl = 1;
	while (nl < MAX_LEVELS) {
		int ext = hole_extent(&lv[nl - 1]);
		if (ext <= COARSE_HOLE || lv[nl - 1].w < 48 || lv[nl - 1].h < 48)
			break;
		if (lvl_down(&lv[nl - 1], &lv[nl]) < 0) {
			for (int i = 1; i < nl; i++)
				lvl_free(&lv[i]);
			return -1;
		}
		nl++;
	}
	int r = 0;
	Nnf prev;
	memset(&prev, 0, sizeof(prev));
	int prev_w = 0;
	/* Progress slots: exemplar fill, its refinement, the membrane
	 * candidate's refinement (multi-level only), then one per finer
	 * level. */
	const int slots = nl > 1 ? nl + 2 : 2;
	const double share = 1.0 / (double)slots;
	double pbase = pr->base, pspan = pr->span;

	/* The coarsest level.  The exemplar fill continues structure (edges,
	 * lines) best; on large holes in smoothly varying content, though,
	 * fronts growing from opposite sides may meet with a step.  So for a
	 * multi-level solve a second candidate starts from the smooth
	 * membrane interpolation; both are refined, and the one whose patches
	 * are more consistent with the rest of the image (lower mean
	 * nearest-patch distance) wins. */
	Lvl *top = &lv[nl - 1];
	int p_top = patch_half(img_long >> (nl - 1), hole_extent(top));
	pr->base = pbase;
	pr->span = pspan * share;
	Lvl alt;
	memset(&alt, 0, sizeof(alt));
	if (nl > 1) {
		if (lvl_alloc(&alt, top->w, top->h) < 0) {
			for (int i = 1; i < nl; i++)
				lvl_free(&lv[i]);
			return -1;
		}
		memcpy(alt.c, top->c, sizeof(float) * 4 * (size_t)top->w * top->h);
		memcpy(alt.st, top->st, (size_t)top->w * top->h);
	}
	r = exemplar_fill(top, p_top, pr);
	pr->base = pbase + pspan * share;
	if (r == 0)
		r = refine_level(top, p_top, &prev, NULL, 0, 5, pr, 0x1234u);
	if (r == 0 && nl > 1) {
		Nnf alt_f;
		memset(&alt_f, 0, sizeof(alt_f));
		membrane_fill(&alt);
		pr->base = pbase + pspan * share * 2.0;
		r = refine_level(&alt, p_top, &alt_f, NULL, 0, 5, pr, 0x4321u);
		double ea = nnf_energy(&prev), eb = nnf_energy(&alt_f);

		if (r == 0 && eb < ea) {
			memcpy(top->c, alt.c, sizeof(float) * 4 * (size_t)top->w * top->h);
			nnf_free(&prev);
			prev = alt_f;
		} else {
			nnf_free(&alt_f);
		}
	}
	lvl_free(&alt);
	prev_w = top->w;

	for (int li = nl - 2; li >= 0 && r == 0; li--) {
		Lvl *l = &lv[li];
		{
			/* Upsample the coarser result into the fill pixels. */
			const Lvl *c = &lv[li + 1];
			for (int y = 0; y < l->h; y++)
				for (int x = 0; x < l->w; x++) {
					size_t k = (size_t)y * l->w + x;
					if (l->st[k] != ST_FILL)
						continue;
					float fx = ((float)x + 0.5f) * 0.5f - 0.5f;
					float fy = ((float)y + 0.5f) * 0.5f - 0.5f;
					fx = ivp_clampf(fx, 0.0f, (float)(c->w - 1));
					fy = ivp_clampf(fy, 0.0f, (float)(c->h - 1));
					int x0 = (int)fx, y0 = (int)fy;
					int x1 = x0 + 1 < c->w ? x0 + 1 : x0;
					int y1 = y0 + 1 < c->h ? y0 + 1 : y0;
					float tx = fx - x0, ty = fy - y0;
					for (int ch = 0; ch < 4; ch++) {
						float a = c->c[((size_t)y0 * c->w + x0) * 4 + ch];
						float b = c->c[((size_t)y0 * c->w + x1) * 4 + ch];
						float cc = c->c[((size_t)y1 * c->w + x0) * 4 + ch];
						float d = c->c[((size_t)y1 * c->w + x1) * 4 + ch];
						float top2 = a + (b - a) * tx, bot = cc + (d - cc) * tx;
						l->c[k * 4 + ch] = top2 + (bot - top2) * ty;
					}
				}
		}
		/* One patch size on every level: the coarse field then
		 * predicts the fine one directly. */
		int p = p_top;
		Nnf cur;
		memset(&cur, 0, sizeof(cur));
		/* Fewer iterations on the big fine levels, more on the
		 * cheap coarse ones. */
		int iters = li == 0 ? 3 : 5;
		pr->base = pbase + pspan * share * (double)(slots - 1 - li);
		pr->span = pspan * share;
		r = refine_level(l, p, &cur, prev.n ? &prev : NULL, prev_w, iters,
				 pr, 0x1234u + (uint32_t)li * 977u);
		nnf_free(&prev);
		prev = cur;
		prev_w = l->w;
	}
	if (r == 0 && prev.n > 0) {
		/* One more search against the converged estimate, then the
		 * texture-preserving reconstruction. */
		int *vlist = NULL, nv = 0;
		uint8_t *valid = valid_map(&lv[0], p_top, &vlist, &nv);
		if (valid != NULL && nv > 0) {
			nnf_search(&lv[0], &prev, p_top, valid, 2, 0xBEEFu);
			if (vote(&lv[0], &prev, p_top) < 0 ||
			    reconstruct_detail(&lv[0], &prev) < 0)
				r = -1;
		}
		free(valid);
		free(vlist);
	}
	nnf_free(&prev);
	pr->base = pbase;
	pr->span = pspan;
	if (r == 0)
		seam_blend(&lv[0]);
	for (int i = 1; i < nl; i++)
		lvl_free(&lv[i]);
	return r;
}

/* ------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------- */

/* 8-connected components of the hole; returns their count, labels in lab
 * (0 = none), bounding boxes in *boxes (4 ints each, malloc()ed). */
static int label_holes(const uint8_t *hole, int w, int h, int *lab, int **boxes)
{
	int *stack = malloc(sizeof(int) * (size_t)w * h);
	int cap = 16, n = 0;
	int *bx = malloc(sizeof(int) * 4 * (size_t)cap);
	if (stack == NULL || bx == NULL) {
		free(stack);
		free(bx);
		return -1;
	}
	memset(lab, 0, sizeof(int) * (size_t)w * h);
	for (int i = 0; i < w * h; i++) {
		if (!hole[i] || lab[i])
			continue;
		if (n == cap) {
			cap *= 2;
			int *nb = realloc(bx, sizeof(int) * 4 * (size_t)cap);
			if (nb == NULL) {
				free(stack);
				free(bx);
				return -1;
			}
			bx = nb;
		}
		int id = ++n;
		int x0 = w, y0 = h, x1 = -1, y1 = -1;
		int sp = 0;
		stack[sp++] = i;
		lab[i] = id;
		while (sp) {
			int k = stack[--sp];
			int x = k % w, y = k / w;
			if (x < x0)
				x0 = x;
			if (x > x1)
				x1 = x;
			if (y < y0)
				y0 = y;
			if (y > y1)
				y1 = y;
			for (int j = -1; j <= 1; j++)
				for (int q = -1; q <= 1; q++) {
					int xx = x + q, yy = y + j;
					if (xx < 0 || yy < 0 || xx >= w || yy >= h)
						continue;
					int m = yy * w + xx;
					if (hole[m] && !lab[m]) {
						lab[m] = id;
						stack[sp++] = m;
					}
				}
		}
		int *b = bx + 4 * (id - 1);
		b[0] = x0;
		b[1] = y0;
		b[2] = x1;
		b[3] = y1;
	}
	free(stack);
	*boxes = bx;
	return n;
}

int iv_inpaint(IvImage *img, const uint8_t *mask, IvProgress cb, void *user)
{
	if (img == NULL || img->px == NULL || mask == NULL)
		return -1;
	const int w = img->w, h = img->h;
	const size_t n = (size_t)w * h;
	if ((size_t)w * h > (size_t)0x7FFFFFFF)
		return -1;	/* pixel indices are int */

	/* Threshold and dilate: painted masks have soft edges, and the faint
	 * rim of a spot must go too. */
	uint8_t *hole = malloc(n);
	uint8_t *tmp = malloc(n);
	int *lab = malloc(sizeof(int) * n);
	if (hole == NULL || tmp == NULL || lab == NULL) {
		free(hole);
		free(tmp);
		free(lab);
		return -1;
	}
	int any = 0;
	for (size_t i = 0; i < n; i++) {
		tmp[i] = mask[i] >= 128;
		any |= tmp[i];
	}
	if (!any) {
		free(hole);
		free(tmp);
		free(lab);
		return 0;
	}
	int dil = (w > 1500 || h > 1500) ? 2 : 1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t v = 0;
			for (int j = -dil; j <= dil && !v; j++) {
				int yy = y + j;
				if (yy < 0 || yy >= h)
					continue;
				for (int i = -dil; i <= dil; i++) {
					int xx = x + i;
					if (xx >= 0 && xx < w && tmp[(size_t)yy * w + xx]) {
						v = 1;
						break;
					}
				}
			}
			hole[(size_t)y * w + x] = v;
		}
	free(tmp);

	/* If everything is hole there is nothing to copy from. */
	size_t nh = 0;
	for (size_t i = 0; i < n; i++)
		nh += hole[i];
	if (nh == n) {
		free(hole);
		free(lab);
		return -1;
	}

	int *boxes = NULL;
	int nc = label_holes(hole, w, h, lab, &boxes);
	if (nc < 0) {
		free(hole);
		free(lab);
		return -1;
	}

	/* Work on a float copy of the whole image only where needed: each
	 * hole gets its own region, results go to `out` (so a cancel leaves
	 * img untouched). */
	IvImage *out = iv_image_copy(img);
	if (out == NULL) {
		free(hole);
		free(lab);
		free(boxes);
		return -1;
	}
	Prog pr = { cb, user, 0.0, 1.0, 0, 0 };
	int img_long = w > h ? w : h;
	int result = 0;

	for (int c = 0; c < nc && result == 0; c++) {
		int *b = boxes + 4 * c;
		int ext = (b[2] - b[0] > b[3] - b[1] ? b[2] - b[0] : b[3] - b[1]) + 1;
		int margin = (int)(ext * 1.5);
		if (margin < 40)
			margin = 40;
		if (margin > 600)
			margin = 600;
		int rx0 = b[0] - margin > 0 ? b[0] - margin : 0;
		int ry0 = b[1] - margin > 0 ? b[1] - margin : 0;
		int rx1 = b[2] + margin < w - 1 ? b[2] + margin : w - 1;
		int ry1 = b[3] + margin < h - 1 ? b[3] + margin : h - 1;
		Lvl L;
		if (lvl_alloc(&L, rx1 - rx0 + 1, ry1 - ry0 + 1) < 0) {
			result = -1;
			break;
		}
		for (int y = ry0; y <= ry1; y++) {
			const uint8_t *s = ivp_px(out, rx0, y);
			for (int x = rx0; x <= rx1; x++, s += 4) {
				size_t k = (size_t)(y - ry0) * L.w + (x - rx0);
				size_t g = (size_t)y * w + x;
				for (int ch = 0; ch < 4; ch++)
					L.c[k * 4 + ch] = s[ch];
				/* This component is filled; holes not yet
				 * treated are off limits. */
				L.st[k] = lab[g] == c + 1 ? ST_FILL :
					  (hole[g] ? ST_BLOCKED : ST_KNOWN);
			}
		}
		pr.base = (double)c / nc;
		pr.span = 1.0 / nc;
		int r = solve(&L, img_long, &pr);
		if (r != 0) {
			result = r;
			lvl_free(&L);
			break;
		}
		for (int y = ry0; y <= ry1; y++) {
			uint8_t *d = ivp_px(out, rx0, y);
			for (int x = rx0; x <= rx1; x++, d += 4) {
				size_t k = (size_t)(y - ry0) * L.w + (x - rx0);
				size_t g = (size_t)y * w + x;
				if (lab[g] != c + 1)
					continue;
				for (int ch = 0; ch < 4; ch++)
					d[ch] = ivp_u8(L.c[k * 4 + ch]);
				hole[g] = 0;	/* now usable by the next holes */
			}
		}
		lvl_free(&L);
	}
	if (result == 0) {
		for (int y = 0; y < h; y++)
			memcpy(ivp_px(img, 0, y), ivp_px(out, 0, y), (size_t)w * 4);
		if (cb)
			cb(1.0, user);
	}
	iv_image_free(out);
	free(hole);
	free(lab);
	free(boxes);
	return result < 0 ? -1 : result;
}
