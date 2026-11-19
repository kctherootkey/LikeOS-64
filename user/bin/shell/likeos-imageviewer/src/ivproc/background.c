// LikeOS -- LikeOS Image Viewer: background estimation and the
// Background tool's operations (blur, remove, replace).
//
// Estimation works on a copy about 320 px across, then carries the result
// back to full resolution:
//
//  1. Colour model of the border: the pixels of a thin band along the
//     four edges are clustered (k-means in CIE Lab).  A cluster counts as
//     background when it covers a fair share of the band AND shows up on
//     at least two sides -- a subject cut off by one edge (a portrait's
//     shoulders) must not be mistaken for background.
//  2. Region growing as a geodesic distance: starting from the border
//     pixels that match the model, a shortest-path search spreads into
//     the image; each step costs the colour difference beyond the
//     background's own noise level, the edge strength (Sobel) beyond its
//     noise level, and a little for straying from the colour model.  Flat
//     or smoothly graded background is reached almost for free; crossing
//     the subject's outline is expensive.  Pixels within a threshold are
//     background.
//  3. Clean-up: morphological open and close, only the large foreground
//     components are kept (the largest and any of comparable size), and
//     background specks enclosed by the subject are filled.
//  4. Confidence: no usable border model, a subject covering almost
//     nothing or almost everything, or a weak outline (little contrast
//     between subject and background) mean "no clear background".
//  5. Soft edge: the small binary mask is turned into coefficients of a
//     guided filter (guide = luma) and applied at full resolution, so the
//     edge follows the real image edges with sub-pixel softness.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

#define WORK_EDGE 320
#define K 5

typedef struct {
	IvProgress cb;
	void *user;
} BgProg;

static int bg_prog(const BgProg *p, double f)
{
	return p->cb != NULL && p->cb(f, p->user) != 0;
}

/* ------------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

static inline float lab_dist(const float *a, const float *b)
{
	float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
	return sqrtf(d0 * d0 + d1 * d1 + d2 * d2);
}

/* Binary min-heap of (distance, index) for the geodesic search. */
typedef struct {
	float *d;
	int *i;
	int n, cap;
} Heap;

static int heap_push(Heap *h, float d, int idx)
{
	if (h->n == h->cap) {
		int nc = h->cap ? h->cap * 2 : 1024;
		float *nd = realloc(h->d, sizeof(float) * (size_t)nc);
		if (nd == NULL)
			return -1;
		h->d = nd;
		int *ni = realloc(h->i, sizeof(int) * (size_t)nc);
		if (ni == NULL)
			return -1;
		h->i = ni;
		h->cap = nc;
	}
	int k = h->n++;
	while (k > 0) {
		int p = (k - 1) / 2;
		if (h->d[p] <= d)
			break;
		h->d[k] = h->d[p];
		h->i[k] = h->i[p];
		k = p;
	}
	h->d[k] = d;
	h->i[k] = idx;
	return 0;
}

static void heap_pop(Heap *h, float *d, int *idx)
{
	*d = h->d[0];
	*idx = h->i[0];
	float ld = h->d[--h->n];
	int li = h->i[h->n];
	int k = 0;
	for (;;) {
		int c = 2 * k + 1;
		if (c >= h->n)
			break;
		if (c + 1 < h->n && h->d[c + 1] < h->d[c])
			c++;
		if (h->d[c] >= ld)
			break;
		h->d[k] = h->d[c];
		h->i[k] = h->i[c];
		k = c;
	}
	h->d[k] = ld;
	h->i[k] = li;
}

/* Binary erosion / dilation with a (2r+1)^2 square (separable: a row
 * pass and a column pass of running min/max). value 1 = set. */
static void morph(uint8_t *m, int w, int h, int r, int dilate)
{
	uint8_t *t = malloc((size_t)w * h);
	if (t == NULL)
		return;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int v = dilate ? 0 : 1;
			for (int i = -r; i <= r; i++) {
				int xx = ivp_clampi(x + i, 0, w - 1);
				int s = m[(size_t)y * w + xx];
				if (dilate ? s : !s) {
					v = dilate;
					break;
				}
			}
			t[(size_t)y * w + x] = (uint8_t)v;
		}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int v = dilate ? 0 : 1;
			for (int j = -r; j <= r; j++) {
				int yy = ivp_clampi(y + j, 0, h - 1);
				int s = t[(size_t)yy * w + x];
				if (dilate ? s : !s) {
					v = dilate;
					break;
				}
			}
			m[(size_t)y * w + x] = (uint8_t)v;
		}
	free(t);
}

/* Label 4-connected components of pixels equal to `val`; returns the
 * count, sizes in *sizes (index = label - 1) and per-label "touches the
 * border" flags in *border. */
static int components(const uint8_t *m, int w, int h, uint8_t val, int *lab,
		      int **sizes, uint8_t **border)
{
	int *stack = malloc(sizeof(int) * (size_t)w * h);
	int cap = 64, n = 0;
	int *sz = malloc(sizeof(int) * (size_t)cap);
	uint8_t *bd = malloc((size_t)cap);
	if (stack == NULL || sz == NULL || bd == NULL) {
		free(stack);
		free(sz);
		free(bd);
		return -1;
	}
	for (int i = 0; i < w * h; i++)
		lab[i] = 0;
	for (int i = 0; i < w * h; i++) {
		if (m[i] != val || lab[i])
			continue;
		if (n == cap) {
			cap *= 2;
			int *ns = realloc(sz, sizeof(int) * (size_t)cap);
			uint8_t *nb = ns ? realloc(bd, (size_t)cap) : NULL;
			if (ns)
				sz = ns;
			if (nb == NULL) {
				free(stack);
				free(sz);
				free(bd);
				return -1;
			}
			bd = nb;
		}
		int id = ++n;
		int count = 0, touch = 0, sp = 0;
		stack[sp++] = i;
		lab[i] = id;
		while (sp) {
			int k = stack[--sp];
			int x = k % w, y = k / w;
			count++;
			if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
				touch = 1;
			const int nb4[4] = { x > 0 ? k - 1 : -1, x < w - 1 ? k + 1 : -1,
					     y > 0 ? k - w : -1, y < h - 1 ? k + w : -1 };
			for (int q = 0; q < 4; q++) {
				int j = nb4[q];
				if (j >= 0 && m[j] == val && !lab[j]) {
					lab[j] = id;
					stack[sp++] = j;
				}
			}
		}
		sz[id - 1] = count;
		bd[id - 1] = (uint8_t)touch;
	}
	free(stack);
	*sizes = sz;
	*border = bd;
	return n;
}

/* ------------------------------------------------------------------------
 * Border colour model
 * --------------------------------------------------------------------- */

typedef struct {
	float c[K][3];
	float spread[K];	/* RMS distance of members */
	int keep[K];
	int nk;
} Model;

/* Deterministic k-means: farthest-point initialisation, 12 iterations. */
static void kmeans(const float *pts, int n, Model *m, int k)
{
	m->nk = k;
	memcpy(m->c[0], pts + (size_t)(n / 2) * 3, sizeof(float) * 3);
	float *dmin = malloc(sizeof(float) * (size_t)n);
	int *as = malloc(sizeof(int) * (size_t)n);
	if (dmin == NULL || as == NULL) {
		free(dmin);
		free(as);
		m->nk = 1;
		return;
	}
	for (int i = 0; i < n; i++)
		dmin[i] = lab_dist(pts + (size_t)i * 3, m->c[0]);
	for (int c = 1; c < k; c++) {
		int far = 0;
		for (int i = 1; i < n; i++)
			if (dmin[i] > dmin[far])
				far = i;
		memcpy(m->c[c], pts + (size_t)far * 3, sizeof(float) * 3);
		for (int i = 0; i < n; i++) {
			float d = lab_dist(pts + (size_t)i * 3, m->c[c]);
			if (d < dmin[i])
				dmin[i] = d;
		}
	}
	for (int it = 0; it < 12; it++) {
		double acc[K][3];
		int cnt[K];
		memset(acc, 0, sizeof(acc));
		memset(cnt, 0, sizeof(cnt));
		for (int i = 0; i < n; i++) {
			const float *p = pts + (size_t)i * 3;
			int best = 0;
			float bd = lab_dist(p, m->c[0]);
			for (int c = 1; c < k; c++) {
				float d = lab_dist(p, m->c[c]);
				if (d < bd) {
					bd = d;
					best = c;
				}
			}
			as[i] = best;
			for (int q = 0; q < 3; q++)
				acc[best][q] += p[q];
			cnt[best]++;
		}
		for (int c = 0; c < k; c++)
			if (cnt[c])
				for (int q = 0; q < 3; q++)
					m->c[c][q] = (float)(acc[c][q] / cnt[c]);
	}
	double sp[K];
	int cnt[K];
	memset(sp, 0, sizeof(sp));
	memset(cnt, 0, sizeof(cnt));
	for (int i = 0; i < n; i++) {
		float d = lab_dist(pts + (size_t)i * 3, m->c[as[i]]);
		sp[as[i]] += (double)d * d;
		cnt[as[i]]++;
	}
	for (int c = 0; c < k; c++)
		m->spread[c] = cnt[c] ? (float)sqrt(sp[c] / cnt[c]) : 0.0f;
	free(dmin);
	free(as);
}

/* Distance of a colour to the nearest kept cluster, in units relative to
 * that cluster's spread. */
static float model_dist(const Model *m, const float *lab, float *raw)
{
	float best = INFINITY, braw = INFINITY;
	for (int c = 0; c < m->nk; c++) {
		if (!m->keep[c])
			continue;
		float d = lab_dist(lab, m->c[c]);
		float rel = d / (m->spread[c] + 4.0f);
		if (rel < best) {
			best = rel;
			braw = d;
		}
	}
	if (raw)
		*raw = braw;
	return best;
}

/* ------------------------------------------------------------------------
 * Estimation
 * --------------------------------------------------------------------- */

int iv_bg_estimate(const IvImage *img, uint8_t *mask_out, IvProgress cb,
		   void *user)
{
	if (img == NULL || img->px == NULL || mask_out == NULL)
		return -1;
	const int W = img->w, H = img->h;
	memset(mask_out, 0, (size_t)W * H);
	if (W < 8 || H < 8)
		return 0;
	BgProg pr = { cb, user };

	/* -- working copy ---------------------------------------------------- */
	int lng = W > H ? W : H;
	double sc = lng > WORK_EDGE ? (double)WORK_EDGE / lng : 1.0;
	int w = (int)lround(W * sc), h = (int)lround(H * sc);
	if (w < 8)
		w = 8;
	if (h < 8)
		h = 8;
	IvImage *sm = iv_image_scale(img, w, h, 1);
	if (sm == NULL)
		return -1;
	iv_gaussian_blur(sm, 0.7);
	const size_t n = (size_t)w * h;
	float *lab = malloc(sizeof(float) * 3 * n);
	float *grad = malloc(sizeof(float) * n);
	uint8_t *bg = calloc(n, 1);
	uint8_t *transp = calloc(n, 1);
	float *dist = malloc(sizeof(float) * n);
	int *lbl = malloc(sizeof(int) * n);
	float *pts = NULL;
	Heap heap = { NULL, NULL, 0, 0 };
	int result = 0;
	if (lab == NULL || grad == NULL || bg == NULL || transp == NULL ||
	    dist == NULL || lbl == NULL) {
		result = -1;
		goto out;
	}
	float lut[256];
	ivp_build_srgb_lut(lut);
	int any_transp = 0;
	for (int y = 0; y < h; y++) {
		const uint8_t *p = ivp_px(sm, 0, y);
		for (int x = 0; x < w; x++, p += 4) {
			size_t k = (size_t)y * w + x;
			ivp_rgb8_to_lab(lut, p[0], p[1], p[2], lab + k * 3);
			if (p[3] < 32) {
				transp[k] = 1;
				any_transp = 1;
			}
		}
	}
	/* Sobel magnitude over the three Lab channels. */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float g2 = 0.0f;
			for (int c = 0; c < 3; c++) {
#define LB(xx, yy) lab[((size_t)ivp_clampi(yy, 0, h - 1) * w + ivp_clampi(xx, 0, w - 1)) * 3 + c]
				float gx = (LB(x + 1, y - 1) + 2 * LB(x + 1, y) + LB(x + 1, y + 1)) -
					   (LB(x - 1, y - 1) + 2 * LB(x - 1, y) + LB(x - 1, y + 1));
				float gy = (LB(x - 1, y + 1) + 2 * LB(x, y + 1) + LB(x + 1, y + 1)) -
					   (LB(x - 1, y - 1) + 2 * LB(x, y - 1) + LB(x + 1, y - 1));
#undef LB
				g2 += gx * gx + gy * gy;
			}
			grad[(size_t)y * w + x] = sqrtf(g2) * 0.125f;
		}
	if (bg_prog(&pr, 0.1)) {
		result = -1;
		goto out;
	}

	/* -- border model ---------------------------------------------------- */
	int band = (int)lround(0.02 * (w > h ? w : h));
	if (band < 2)
		band = 2;
	int nb = 0;
	pts = malloc(sizeof(float) * 3 * n);
	int *side_of = malloc(sizeof(int) * n);
	if (pts == NULL || side_of == NULL) {
		free(side_of);
		result = -1;
		goto out;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int side = y < band ? 0 : (y >= h - band ? 1 : (x < band ? 2 : (x >= w - band ? 3 : -1)));
			if (side < 0)
				continue;
			size_t k = (size_t)y * w + x;
			if (transp[k])
				continue;
			memcpy(pts + (size_t)nb * 3, lab + k * 3, sizeof(float) * 3);
			side_of[nb] = side;
			nb++;
		}
	Model model;
	memset(&model, 0, sizeof(model));
	if (nb >= 16) {
		kmeans(pts, nb, &model, K);
		/* Shares per cluster and side. */
		int cnt[K] = { 0 }, side_cnt[K][4], side_tot[4] = { 0 };
		memset(side_cnt, 0, sizeof(side_cnt));
		for (int i = 0; i < nb; i++) {
			const float *p = pts + (size_t)i * 3;
			int best = 0;
			float bd = lab_dist(p, model.c[0]);
			for (int c = 1; c < model.nk; c++) {
				float d = lab_dist(p, model.c[c]);
				if (d < bd) {
					bd = d;
					best = c;
				}
			}
			cnt[best]++;
			side_cnt[best][side_of[i]]++;
			side_tot[side_of[i]]++;
		}
		for (int c = 0; c < model.nk; c++) {
			int sides = 0;
			for (int s = 0; s < 4; s++)
				if (side_tot[s] && side_cnt[c][s] * 10 >= side_tot[s])
					sides++;
			model.keep[c] = cnt[c] * 100 >= nb * 10 && sides >= 2;
		}
		/* Small clusters very close to a kept one belong to it. */
		for (int c = 0; c < model.nk; c++) {
			if (model.keep[c] || cnt[c] == 0)
				continue;
			for (int d = 0; d < model.nk; d++)
				if (model.keep[d] && lab_dist(model.c[c], model.c[d]) < 10.0f)
					model.keep[c] = 1;
		}
	}
	free(side_of);
	int kept = 0;
	for (int c = 0; c < model.nk; c++)
		kept += model.keep[c];
	if (kept == 0 && !any_transp)
		goto out;	/* result 0: no clear background */

	/* -- noise levels of the background --------------------------------- */
	/* Median neighbour difference and median edge strength over border
	 * pixels that match the model. */
	float *samp_d = malloc(sizeof(float) * (size_t)(nb + 1));
	float *samp_g = malloc(sizeof(float) * (size_t)(nb + 1));
	if (samp_d == NULL || samp_g == NULL) {
		free(samp_d);
		free(samp_g);
		result = -1;
		goto out;
	}
	int ns = 0, nseed = 0, nborder = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int in_band = y < band || y >= h - band || x < band || x >= w - band;
			if (!in_band)
				continue;
			size_t k = (size_t)y * w + x;
			nborder++;
			if (transp[k]) {
				nseed++;
				continue;
			}
			if (kept && model_dist(&model, lab + k * 3, NULL) < 2.5f) {
				nseed++;
				if (x + 1 < w) {
					samp_d[ns] = lab_dist(lab + k * 3, lab + (k + 1) * 3);
					samp_g[ns] = grad[k];
					ns++;
				}
			}
		}
	float noise = 1.0f, gnoise = 1.0f;
	if (ns > 0) {
		/* Partial selection would do; a sort of a few thousand floats
		 * is cheap enough. */
		for (int pass = 0; pass < 2; pass++) {
			float *a = pass ? samp_g : samp_d;
			for (int i = 1; i < ns; i++) {
				float v = a[i];
				int j = i - 1;
				while (j >= 0 && a[j] > v) {
					a[j + 1] = a[j];
					j--;
				}
				a[j + 1] = v;
			}
		}
		/* The 75th percentile: textured backgrounds have a wide spread
		 * of small steps that must all count as "noise". */
		noise = samp_d[(ns * 3) / 4] + 0.5f;
		gnoise = samp_g[(ns * 3) / 4] + 0.5f;
	}
	free(samp_d);
	free(samp_g);
	if (nborder == 0 || nseed * 100 < nborder * 35) {
		/* Most of the border does not look like a background. */
		goto out;
	}
	if (bg_prog(&pr, 0.3)) {
		result = -1;
		goto out;
	}

	/* -- geodesic region growing ----------------------------------------- */
	for (size_t k = 0; k < n; k++)
		dist[k] = INFINITY;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int edge = y == 0 || x == 0 || y == h - 1 || x == w - 1;
			size_t k = (size_t)y * w + x;
			if (transp[k] || (edge && kept &&
					  model_dist(&model, lab + k * 3, NULL) < 2.5f)) {
				dist[k] = 0.0f;
				if (heap_push(&heap, 0.0f, (int)k) < 0) {
					result = -1;
					goto out;
				}
			}
		}
	/* Path costs per step.  Scaled to the background's own noise, so a
	 * busy background needs a stronger outline to stop the growth. */
	const float cw_col = 1.0f, cw_edge = 0.6f, cw_model = 0.35f;
	const float T = 14.0f + 1.5f * noise;
	static const int ndx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int ndy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	int popped = 0;
	while (heap.n > 0) {
		float d;
		int k;
		heap_pop(&heap, &d, &k);
		if (d > dist[k])
			continue;
		if (d > T * 1.5f)
			break;	/* everything left is far beyond the threshold */
		if ((++popped & 16383) == 0 && bg_prog(&pr, 0.3 + 0.4 * (double)popped / n)) {
			result = -1;
			goto out;
		}
		int x = k % w, y = k / w;
		for (int q = 0; q < 8; q++) {
			int xx = x + ndx[q], yy = y + ndy[q];
			if (xx < 0 || yy < 0 || xx >= w || yy >= h)
				continue;
			int j = yy * w + xx;
			float step;
			if (transp[j]) {
				step = 0.0f;
			} else {
				float dc = lab_dist(lab + (size_t)k * 3, lab + (size_t)j * 3);
				float de = grad[j];
				step = cw_col * fmaxf(0.0f, dc - noise) +
				       cw_edge * fmaxf(0.0f, de - 1.5f * gnoise);
				if (kept) {
					float md = model_dist(&model, lab + (size_t)j * 3, NULL);
					step += cw_model * fmaxf(0.0f, md - 2.5f);
				}
				if (q >= 4)
					step *= 1.41421356f;
			}
			float nd = d + step;
			if (nd < dist[j]) {
				dist[j] = nd;
				if (heap_push(&heap, nd, j) < 0) {
					result = -1;
					goto out;
				}
			}
		}
	}
	for (size_t k = 0; k < n; k++)
		bg[k] = dist[k] <= T;

	/* -- clean-up -------------------------------------------------------- */
	morph(bg, w, h, 1, 0);	/* open: erode ... */
	morph(bg, w, h, 1, 1);	/* ... then dilate */
	morph(bg, w, h, 1, 1);	/* close: dilate ... */
	morph(bg, w, h, 1, 0);	/* ... then erode */
	for (size_t k = 0; k < n; k++)
		if (transp[k])
			bg[k] = 1;

	/* Foreground components: keep the largest and those at least 15 % of
	 * its size; smaller islands are background clutter. */
	int *sizes = NULL;
	uint8_t *touch = NULL;
	int nc = components(bg, w, h, 0, lbl, &sizes, &touch);
	if (nc < 0) {
		result = -1;
		goto out;
	}
	int largest = 0;
	for (int c = 0; c < nc; c++)
		if (sizes[c] > largest)
			largest = sizes[c];
	for (size_t k = 0; k < n; k++)
		if (!bg[k] && sizes[lbl[k] - 1] * 100 < largest * 15)
			bg[k] = 1;
	free(sizes);
	free(touch);
	/* Background specks enclosed by the subject (not reaching the image
	 * border) are holes in the subject. */
	nc = components(bg, w, h, 1, lbl, &sizes, &touch);
	if (nc < 0) {
		result = -1;
		goto out;
	}
	for (size_t k = 0; k < n; k++)
		if (bg[k] && !touch[lbl[k] - 1] && !transp[k] &&
		    (size_t)sizes[lbl[k] - 1] * 100 < n * 2)
			bg[k] = 0;
	free(sizes);
	free(touch);

	/* -- confidence ------------------------------------------------------ */
	size_t nfg = 0;
	for (size_t k = 0; k < n; k++)
		nfg += !bg[k];
	double fg_frac = (double)nfg / (double)n;
	/* Contrast along the outline: mean edge strength on fg pixels that
	 * touch the background. */
	double edge_sum = 0.0;
	size_t edge_n = 0;
	for (int y = 1; y < h - 1; y++)
		for (int x = 1; x < w - 1; x++) {
			size_t k = (size_t)y * w + x;
			if (bg[k])
				continue;
			if (bg[k - 1] || bg[k + 1] || bg[k - w] || bg[k + w]) {
				edge_sum += grad[k];
				edge_n++;
			}
		}
	double edge_mean = edge_n ? edge_sum / (double)edge_n : 0.0;
	int clear = fg_frac > 0.02 && fg_frac < 0.95 &&
		    (any_transp || edge_mean > 2.0 * gnoise + 3.0);
	if (bg_prog(&pr, 0.8)) {
		result = -1;
		goto out;
	}

	/* -- soft full-resolution mask -------------------------------------- */
	{
		float *I = malloc(sizeof(float) * n);
		float *P = malloc(sizeof(float) * n);
		float *A = malloc(sizeof(float) * n);
		float *B = malloc(sizeof(float) * n);
		if (I == NULL || P == NULL || A == NULL || B == NULL) {
			free(I);
			free(P);
			free(A);
			free(B);
			result = -1;
			goto out;
		}
		for (int y = 0; y < h; y++) {
			const uint8_t *p = ivp_px(sm, 0, y);
			for (int x = 0; x < w; x++, p += 4) {
				size_t k = (size_t)y * w + x;
				I[k] = ivp_luma(p[0], p[1], p[2]) * (1.0f / 255.0f);
				P[k] = bg[k] ? 1.0f : 0.0f;
			}
		}
		/* Guided-filter coefficients of P with guide I: q = a I + b,
		 * a and b box-averaged; computed small, applied large. */
		const int r = 2;
		const float eps = 2e-3f;
		float *mI = malloc(sizeof(float) * n), *mP = malloc(sizeof(float) * n);
		float *cIP = malloc(sizeof(float) * n), *cII = malloc(sizeof(float) * n);
		if (mI == NULL || mP == NULL || cIP == NULL || cII == NULL) {
			free(mI);
			free(mP);
			free(cIP);
			free(cII);
			free(I);
			free(P);
			free(A);
			free(B);
			result = -1;
			goto out;
		}
		for (size_t k = 0; k < n; k++) {
			mI[k] = I[k];
			mP[k] = P[k];
			cIP[k] = I[k] * P[k];
			cII[k] = I[k] * I[k];
		}
		ivp_box_plane(mI, w, h, r);
		ivp_box_plane(mP, w, h, r);
		ivp_box_plane(cIP, w, h, r);
		ivp_box_plane(cII, w, h, r);
		for (size_t k = 0; k < n; k++) {
			float var = fmaxf(0.0f, cII[k] - mI[k] * mI[k]);
			float cov = cIP[k] - mI[k] * mP[k];
			A[k] = cov / (var + eps);
			B[k] = mP[k] - A[k] * mI[k];
		}
		ivp_box_plane(A, w, h, r);
		ivp_box_plane(B, w, h, r);
		free(mI);
		free(mP);
		free(cIP);
		free(cII);
		free(I);
		free(P);

		/* Full resolution: sample a, b bilinearly at each pixel's
		 * position in the small image, apply to that pixel's luma.
		 * Then a gentle contrast on the matte keeps the edge crisp
		 * without stair steps. */
		const float sx = (float)w / (float)W, sy = (float)h / (float)H;
		for (int y = 0; y < H; y++) {
			const uint8_t *p = ivp_px(img, 0, y);
			uint8_t *mo = mask_out + (size_t)y * W;
			float fy = ((float)y + 0.5f) * sy - 0.5f;
			for (int x = 0; x < W; x++, p += 4) {
				float fx = ((float)x + 0.5f) * sx - 0.5f;
				float a = ivp_plane_sample(A, w, h, fx, fy);
				float b = ivp_plane_sample(B, w, h, fx, fy);
				float l = ivp_luma(p[0], p[1], p[2]) * (1.0f / 255.0f);
				float q = a * l + b;
				q = ivp_smoothstep(0.2f, 0.8f, q);
				if (p[3] < 32)
					q = 1.0f;
				mo[x] = ivp_to_u8(q);
			}
			if ((y & 255) == 0 && bg_prog(&pr, 0.8 + 0.2 * (double)y / H)) {
				free(A);
				free(B);
				result = -1;
				goto out;
			}
		}
		free(A);
		free(B);
	}
	result = clear ? 1 : 0;
	if (cb)
		cb(1.0, user);

out:
	iv_image_free(sm);
	free(lab);
	free(grad);
	free(bg);
	free(transp);
	free(dist);
	free(lbl);
	free(pts);
	free(heap.d);
	free(heap.i);
	if (result < 0)
		memset(mask_out, 0, (size_t)W * H);
	return result;
}

/* ------------------------------------------------------------------------
 * Operations
 * --------------------------------------------------------------------- */

/*
 * Background blur by normalised convolution: the image premultiplied by
 * the background weight and the weight itself are blurred alike, and
 * their ratio is the blur of the background ALONE -- the subject never
 * smears into it, even right at its outline.  The result is blended back
 * by the mask.
 */
static void bg_blur(IvImage *img, const uint8_t *mask, double strength)
{
	const int w = img->w, h = img->h;
	int lng = w > h ? w : h;
	double radius = ivp_clampd(strength, 0.0, 100.0) / 100.0 * 0.03 * lng;
	double sigma = radius * 0.5;
	if (sigma < 0.5)
		return;
	uint16_t *buf = ivp_premul16(img);
	float *wt = ivp_alloc((size_t)w, (size_t)h, sizeof(float));
	if (buf == NULL || wt == NULL) {
		free(buf);
		free(wt);
		return;
	}
	for (size_t k = 0; k < (size_t)w * h; k++) {
		uint32_t m = mask[k];
		for (int c = 0; c < 4; c++)
			buf[k * 4 + c] = (uint16_t)(((uint32_t)buf[k * 4 + c] * m + 127) / 255);
		wt[k] = (float)m / 255.0f;
	}
	ivp_blur_generic(buf, IVP_U16, w, h, 4, (size_t)w * 8, sigma, sigma);
	ivp_blur_plane(wt, w, h, sigma);
	for (int y = 0; y < h; y++) {
		uint8_t *p = ivp_px(img, 0, y);
		for (int x = 0; x < w; x++, p += 4) {
			size_t k = (size_t)y * w + x;
			float m = (float)mask[k] / 255.0f;
			if (m <= 0.0f || wt[k] < 1e-3f)
				continue;
			const uint16_t *b = buf + k * 4;
			/* Undo the weight: b / wt is the background-only blur,
			 * still premultiplied by alpha. */
			float A = (float)b[3] / wt[k];	/* alpha * 255 */
			float out[4];
			if (A < 128.0f) {
				out[0] = out[1] = out[2] = out[3] = 0.0f;
			} else {
				for (int c = 0; c < 3; c++)
					out[c] = ivp_clampf((float)b[c] / wt[k] * 255.0f / A, 0.0f, 255.0f);
				out[3] = ivp_clampf(A / 255.0f, 0.0f, 255.0f);
			}
			for (int c = 0; c < 4; c++)
				p[c] = ivp_u8((float)p[c] + (out[c] - (float)p[c]) * m);
		}
	}
	free(buf);
	free(wt);
}

void iv_bg_apply(IvImage *img, const uint8_t *mask, IvBgOp op, double strength,
		 uint32_t rgba)
{
	if (img == NULL || img->px == NULL || mask == NULL)
		return;
	const int w = img->w, h = img->h;
	switch (op) {
	case IV_BG_BLUR:
		bg_blur(img, mask, strength);
		return;
	case IV_BG_REMOVE:
		for (int y = 0; y < h; y++) {
			uint8_t *p = ivp_px(img, 0, y);
			const uint8_t *m = mask + (size_t)y * w;
			for (int x = 0; x < w; x++, p += 4)
				p[3] = (uint8_t)((p[3] * (255u - m[x]) + 127) / 255);
		}
		return;
	case IV_BG_REPLACE: {
		const float cr = (float)((rgba >> 24) & 0xFF), cg = (float)((rgba >> 16) & 0xFF);
		const float cb = (float)((rgba >> 8) & 0xFF), ca = (float)(rgba & 0xFF) / 255.0f;
		for (int y = 0; y < h; y++) {
			uint8_t *p = ivp_px(img, 0, y);
			const uint8_t *m = mask + (size_t)y * w;
			for (int x = 0; x < w; x++, p += 4) {
				if (m[x] == 0)
					continue;
				/* The subject (alpha reduced by the mask) over
				 * the colour layer. */
				float sa = (float)p[3] / 255.0f * (1.0f - (float)m[x] / 255.0f);
				float oa = sa + ca * (1.0f - sa);
				if (oa <= 0.0f) {
					p[0] = p[1] = p[2] = p[3] = 0;
					continue;
				}
				float k2 = ca * (1.0f - sa);
				p[0] = ivp_u8(((float)p[0] * sa + cr * k2) / oa);
				p[1] = ivp_u8(((float)p[1] * sa + cg * k2) / oa);
				p[2] = ivp_u8(((float)p[2] * sa + cb * k2) / oa);
				p[3] = ivp_to_u8(oa);
			}
		}
		return;
	}
	}
}
