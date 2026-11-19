// LikeOS -- LikeOS Image Viewer: Gaussian and box blurs, the guided
// filter, unsharp masking and the premultiplied 16-bit working copy the
// RGBA filters share.
//
// Every blur here is separable and runs one 1-D pass per axis over float
// line buffers.  The vertical pass gathers a strip of columns at a time so
// it reads memory row by row; the image itself can stay in its compact
// 8- or 16-bit layout, which keeps a 24-megapixel photo affordable.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

/* Columns gathered together by the vertical pass. */
#define STRIP 16

/* A 1-D smoothing kernel: either sampled Gaussian weights or three box
 * passes whose combined variance equals sigma^2. */
typedef struct {
	int is_box;
	int radius;		/* Gaussian: taps on each side */
	float *weights;		/* Gaussian: radius + 1 weights, centre first */
	int box[3];		/* box radii */
} Kernel;

static int kernel_init(Kernel *k, double sigma)
{
	memset(k, 0, sizeof(*k));
	if (!(sigma > 0.0))
		return -1;
	if (sigma < 2.5) {
		/* A true kernel is both exact and cheap at this size. */
		int r = (int)ceil(sigma * 3.0);
		if (r < 1)
			r = 1;
		k->radius = r;
		k->weights = malloc(sizeof(float) * (size_t)(r + 1));
		if (k->weights == NULL)
			return -1;
		double sum = 0.0;
		for (int i = 0; i <= r; i++) {
			double w = exp(-(double)(i * i) / (2.0 * sigma * sigma));
			k->weights[i] = (float)w;
			sum += i ? 2.0 * w : w;
		}
		for (int i = 0; i <= r; i++)
			k->weights[i] = (float)(k->weights[i] / sum);
		return 0;
	}
	/* Three successive boxes approach a Gaussian closely; choose widths
	 * wl and wl + 2 (both odd) so the summed variance matches. */
	k->is_box = 1;
	const int n = 3;
	double wideal = sqrt(12.0 * sigma * sigma / n + 1.0);
	int wl = (int)floor(wideal);
	if (wl % 2 == 0)
		wl--;
	int wu = wl + 2;
	double mideal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) /
			(-4.0 * wl - 4.0);
	int m = (int)lround(mideal);
	for (int i = 0; i < n; i++)
		k->box[i] = ((i < m ? wl : wu) - 1) / 2;
	return 0;
}

static void kernel_free(Kernel *k)
{
	free(k->weights);
	k->weights = NULL;
}

/* Box mean of radius r over line[0..n-1], edges replicated; tmp holds at
 * least n floats. */
static void box_line(float *line, int n, int r, float *tmp)
{
	if (r <= 0 || n <= 1)
		return;
	const float inv = 1.0f / (float)(2 * r + 1);
	/* Sliding window sum, accumulated in double so that adding and
	 * removing samples along a long line does not drift. */
	double acc = 0.0;
	for (int i = -r; i <= r; i++)
		acc += line[ivp_clampi(i, 0, n - 1)];
	for (int i = 0; i < n; i++) {
		tmp[i] = (float)acc * inv;
		int out = ivp_clampi(i - r, 0, n - 1);
		int in = ivp_clampi(i + r + 1, 0, n - 1);
		acc += (double)line[in] - (double)line[out];
	}
	memcpy(line, tmp, sizeof(float) * (size_t)n);
}

static void gauss_line(float *line, int n, const Kernel *k, float *tmp)
{
	const int r = k->radius;
	const float *wt = k->weights;
	if (n <= 1)
		return;
	for (int i = 0; i < n; i++) {
		float s = line[i] * wt[0];
		if (i - r >= 0 && i + r < n) {
			for (int j = 1; j <= r; j++)
				s += (line[i - j] + line[i + j]) * wt[j];
		} else {
			for (int j = 1; j <= r; j++)
				s += (line[ivp_clampi(i - j, 0, n - 1)] +
				      line[ivp_clampi(i + j, 0, n - 1)]) * wt[j];
		}
		tmp[i] = s;
	}
	memcpy(line, tmp, sizeof(float) * (size_t)n);
}

static void kernel_line(float *line, int n, const Kernel *k, float *tmp)
{
	if (k->is_box) {
		for (int i = 0; i < 3; i++)
			box_line(line, n, k->box[i], tmp);
	} else {
		gauss_line(line, n, k, tmp);
	}
}

static inline float load_v(const void *data, int type, size_t off)
{
	switch (type) {
	case IVP_U8:
		return (float)((const uint8_t *)data)[off];
	case IVP_U16:
		return (float)((const uint16_t *)data)[off];
	default:
		return ((const float *)data)[off];
	}
}

static inline void store_v(void *data, int type, size_t off, float v)
{
	switch (type) {
	case IVP_U8:
		((uint8_t *)data)[off] = ivp_u8(v);
		break;
	case IVP_U16:
		if (!(v > 0.0f))
			v = 0.0f;
		else if (v > 65535.0f)
			v = 65535.0f;
		((uint16_t *)data)[off] = (uint16_t)(v + 0.5f);
		break;
	default:
		((float *)data)[off] = v;
		break;
	}
}

static size_t elem_size(int type)
{
	return type == IVP_U8 ? 1 : (type == IVP_U16 ? 2 : 4);
}

/* Generic separable pass engine; kx / ky may be NULL to skip an axis. */
static void blur_passes(void *data, int type, int w, int h, int nc,
			size_t row_stride, const Kernel *kx, const Kernel *ky)
{
	const size_t es = elem_size(type);
	const size_t row_elems = row_stride / es;
	int maxn = w > h ? w : h;
	float *tmp = malloc(sizeof(float) * ((size_t)maxn + 16));
	float *lines = malloc(sizeof(float) * (size_t)maxn * (size_t)(STRIP * nc));
	if (tmp == NULL || lines == NULL) {
		free(tmp);
		free(lines);
		return;
	}

	if (kx != NULL && w > 1) {
		for (int y = 0; y < h; y++) {
			size_t base = (size_t)y * row_elems;
			for (int c = 0; c < nc; c++) {
				float *line = lines;
				for (int x = 0; x < w; x++)
					line[x] = load_v(data, type, base + (size_t)x * nc + c);
				kernel_line(line, w, kx, tmp);
				for (int x = 0; x < w; x++)
					store_v(data, type, base + (size_t)x * nc + c, line[x]);
			}
		}
	}

	if (ky != NULL && h > 1) {
		for (int x0 = 0; x0 < w; x0 += STRIP) {
			int sw = w - x0 < STRIP ? w - x0 : STRIP;
			int nl = sw * nc;
			/* Gather: line l = (column, channel), h samples each. */
			for (int y = 0; y < h; y++) {
				size_t base = (size_t)y * row_elems + (size_t)x0 * nc;
				for (int l = 0; l < nl; l++)
					lines[(size_t)l * h + y] = load_v(data, type, base + l);
			}
			for (int l = 0; l < nl; l++)
				kernel_line(lines + (size_t)l * h, h, ky, tmp);
			for (int y = 0; y < h; y++) {
				size_t base = (size_t)y * row_elems + (size_t)x0 * nc;
				for (int l = 0; l < nl; l++)
					store_v(data, type, base + l, lines[(size_t)l * h + y]);
			}
		}
	}
	free(tmp);
	free(lines);
}

/* Keep box radii at most the line length: beyond that the replicated
 * edges dominate anyway and the result approaches the line mean.  (The
 * small Gaussian kernels need no clamp; their edge taps are clamped per
 * sample.) */
static void kernel_clamp(Kernel *k, int n)
{
	if (!k->is_box)
		return;
	for (int i = 0; i < 3; i++)
		if (k->box[i] > n)
			k->box[i] = n;
}

void ivp_blur_generic(void *data, int type, int w, int h, int nc,
		      size_t row_stride, double sigma_x, double sigma_y)
{
	if (data == NULL || w <= 0 || h <= 0 || nc <= 0)
		return;
	Kernel kx, ky;
	int hx = kernel_init(&kx, sigma_x) == 0;
	int hy = kernel_init(&ky, sigma_y) == 0;
	if (hx)
		kernel_clamp(&kx, w);
	if (hy)
		kernel_clamp(&ky, h);
	if (hx || hy)
		blur_passes(data, type, w, h, nc, row_stride, hx ? &kx : NULL,
			    hy ? &ky : NULL);
	kernel_free(&kx);
	kernel_free(&ky);
}

void ivp_blur_plane(float *p, int w, int h, double sigma)
{
	ivp_blur_generic(p, IVP_F32, w, h, 1, (size_t)w * sizeof(float), sigma,
			 sigma);
}

void ivp_box_plane(float *p, int w, int h, int r)
{
	if (p == NULL || r <= 0)
		return;
	Kernel k;
	memset(&k, 0, sizeof(k));
	k.is_box = 1;
	/* One box pass of radius r and two of radius 0 (identity). */
	Kernel kx = k, ky = k;
	kx.box[0] = r < w ? r : w;
	ky.box[0] = r < h ? r : h;
	blur_passes(p, IVP_F32, w, h, 1, (size_t)w * sizeof(float), &kx, &ky);
}

int ivp_guided_self(const float *I, int w, int h, int r, float eps, float *a,
		    float *b)
{
	size_t n = (size_t)w * (size_t)h;
	float *mean = malloc(n * sizeof(float));
	float *sq = malloc(n * sizeof(float));
	if (mean == NULL || sq == NULL) {
		free(mean);
		free(sq);
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		mean[i] = I[i];
		sq[i] = I[i] * I[i];
	}
	ivp_box_plane(mean, w, h, r);
	ivp_box_plane(sq, w, h, r);
	for (size_t i = 0; i < n; i++) {
		float var = sq[i] - mean[i] * mean[i];
		if (var < 0.0f)
			var = 0.0f;
		float ai = var / (var + eps);
		a[i] = ai;
		b[i] = mean[i] * (1.0f - ai);
	}
	ivp_box_plane(a, w, h, r);
	ivp_box_plane(b, w, h, r);
	free(mean);
	free(sq);
	return 0;
}

int ivp_guided(const float *I, const float *p, int w, int h, int r, float eps,
	       float *q)
{
	size_t n = (size_t)w * (size_t)h;
	float *mI = malloc(n * sizeof(float));
	float *mp = malloc(n * sizeof(float));
	float *cIp = malloc(n * sizeof(float));
	float *cII = malloc(n * sizeof(float));
	if (mI == NULL || mp == NULL || cIp == NULL || cII == NULL) {
		free(mI);
		free(mp);
		free(cIp);
		free(cII);
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		mI[i] = I[i];
		mp[i] = p[i];
		cIp[i] = I[i] * p[i];
		cII[i] = I[i] * I[i];
	}
	ivp_box_plane(mI, w, h, r);
	ivp_box_plane(mp, w, h, r);
	ivp_box_plane(cIp, w, h, r);
	ivp_box_plane(cII, w, h, r);
	/* Reuse cIp for a and cII for b. */
	for (size_t i = 0; i < n; i++) {
		float cov = cIp[i] - mI[i] * mp[i];
		float var = cII[i] - mI[i] * mI[i];
		if (var < 0.0f)
			var = 0.0f;
		float ai = cov / (var + eps);
		cIp[i] = ai;
		cII[i] = mp[i] - ai * mI[i];
	}
	ivp_box_plane(cIp, w, h, r);
	ivp_box_plane(cII, w, h, r);
	for (size_t i = 0; i < n; i++)
		q[i] = cIp[i] * I[i] + cII[i];
	free(mI);
	free(mp);
	free(cIp);
	free(cII);
	return 0;
}

uint16_t *ivp_premul16(const IvImage *img)
{
	uint16_t *buf = ivp_alloc((size_t)img->w, (size_t)img->h, 8);
	if (buf == NULL)
		return NULL;
	for (int y = 0; y < img->h; y++) {
		const uint8_t *s = ivp_px(img, 0, y);
		uint16_t *d = buf + (size_t)y * (size_t)img->w * 4;
		for (int x = 0; x < img->w; x++, s += 4, d += 4) {
			uint32_t a = s[3];
			d[0] = (uint16_t)(s[0] * a);
			d[1] = (uint16_t)(s[1] * a);
			d[2] = (uint16_t)(s[2] * a);
			d[3] = (uint16_t)(a * 255);
		}
	}
	return buf;
}

void ivp_unpremul16(const uint16_t *src, IvImage *img)
{
	for (int y = 0; y < img->h; y++) {
		const uint16_t *s = src + (size_t)y * (size_t)img->w * 4;
		uint8_t *d = ivp_px(img, 0, y);
		for (int x = 0; x < img->w; x++, s += 4, d += 4) {
			uint32_t A = s[3];
			if (A < 128) {	/* alpha rounds to 0 */
				d[0] = d[1] = d[2] = d[3] = 0;
				continue;
			}
			for (int c = 0; c < 3; c++) {
				uint32_t v = ((uint32_t)s[c] * 255 + A / 2) / A;
				d[c] = (uint8_t)(v > 255 ? 255 : v);
			}
			d[3] = (uint8_t)((A + 127) / 255);
		}
	}
}

void iv_gaussian_blur(IvImage *img, double sigma)
{
	if (img == NULL || !(sigma > 0.0))
		return;
	uint16_t *buf = ivp_premul16(img);
	if (buf == NULL)
		return;
	ivp_blur_generic(buf, IVP_U16, img->w, img->h, 4,
			 (size_t)img->w * 8, sigma, sigma);
	ivp_unpremul16(buf, img);
	free(buf);
}

void iv_unsharp(IvImage *img, double sigma, double amount)
{
	if (img == NULL || !(sigma > 0.0) || amount == 0.0)
		return;
	uint16_t *buf = ivp_premul16(img);
	if (buf == NULL)
		return;
	ivp_blur_generic(buf, IVP_U16, img->w, img->h, 4,
			 (size_t)img->w * 8, sigma, sigma);
	const float amt = (float)amount;
	for (int y = 0; y < img->h; y++) {
		const uint16_t *s = buf + (size_t)y * (size_t)img->w * 4;
		uint8_t *d = ivp_px(img, 0, y);
		for (int x = 0; x < img->w; x++, s += 4, d += 4) {
			if (d[3] == 0 || s[3] < 128)
				continue;
			float A = (float)s[3];
			for (int c = 0; c < 3; c++) {
				float blurred = (float)s[c] * 255.0f / A;
				float v = (float)d[c] + amt * ((float)d[c] - blurred);
				d[c] = ivp_u8(v);
			}
		}
	}
	free(buf);
}
