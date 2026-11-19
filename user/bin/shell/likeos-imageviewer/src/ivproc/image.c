// LikeOS -- LikeOS Image Viewer: IvImage allocation, copies and the
// conversions to and from other pixel layouts.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

/* Largest edge accepted anywhere in the library.  Far beyond any camera,
 * small enough that w * h * 16 bytes (the widest intermediate any filter
 * uses per pixel) stays representable in size_t on every platform. */
#define IVP_MAX_EDGE 65535

size_t ivp_size3(size_t a, size_t b, size_t c)
{
	if (a == 0 || b == 0 || c == 0)
		return 0;
	if (a > SIZE_MAX / b)
		return 0;
	size_t ab = a * b;
	if (ab > SIZE_MAX / c)
		return 0;
	return ab * c;
}

void *ivp_alloc(size_t a, size_t b, size_t c)
{
	size_t n = ivp_size3(a, b, c);
	if (n == 0) {
		/* An impossible size counts as running out. */
		ivp_note_alloc_failure();
		return NULL;
	}
	return malloc(n);
}

void *ivp_zalloc(size_t a, size_t b, size_t c)
{
	size_t n = ivp_size3(a, b, c);
	if (n == 0) {
		ivp_note_alloc_failure();
		return NULL;
	}
	return calloc(1, n);
}

IvImage *iv_image_new(int w, int h)
{
	if (w <= 0 || h <= 0 || w > IVP_MAX_EDGE || h > IVP_MAX_EDGE)
		return NULL;
	IvImage *img = malloc(sizeof(*img));
	if (img == NULL)
		return NULL;
	img->w = w;
	img->h = h;
	img->stride = w * 4;
	img->px = ivp_zalloc((size_t)w, (size_t)h, 4);
	if (img->px == NULL) {
		free(img);
		return NULL;
	}
	return img;
}

void iv_image_free(IvImage *img)
{
	if (img == NULL)
		return;
	free(img->px);
	free(img);
}

IvImage *iv_image_copy(const IvImage *img)
{
	if (img == NULL || img->px == NULL)
		return NULL;
	IvImage *c = iv_image_new(img->w, img->h);
	if (c == NULL)
		return NULL;
	for (int y = 0; y < img->h; y++)
		memcpy(ivp_px(c, 0, y), ivp_px(img, 0, y), (size_t)img->w * 4);
	return c;
}

IvImage *iv_image_new_from_data(const uint8_t *data, int w, int h, int stride,
				int channels)
{
	if (data == NULL || (channels != 3 && channels != 4) ||
	    stride < w * channels)
		return NULL;
	IvImage *img = iv_image_new(w, h);
	if (img == NULL)
		return NULL;
	for (int y = 0; y < h; y++) {
		const uint8_t *s = data + (size_t)y * (size_t)stride;
		uint8_t *d = ivp_px(img, 0, y);
		if (channels == 4) {
			memcpy(d, s, (size_t)w * 4);
			continue;
		}
		for (int x = 0; x < w; x++, s += 3, d += 4) {
			d[0] = s[0];
			d[1] = s[1];
			d[2] = s[2];
			d[3] = 255;
		}
	}
	return img;
}

void iv_image_to_cairo(const IvImage *img, uint8_t *dst, int dst_stride)
{
	if (img == NULL || dst == NULL || dst_stride < img->w * 4)
		return;
	for (int y = 0; y < img->h; y++) {
		const uint8_t *s = ivp_px(img, 0, y);
		uint32_t *d = (uint32_t *)(void *)(dst + (size_t)y * (size_t)dst_stride);
		for (int x = 0; x < img->w; x++, s += 4) {
			uint32_t a = s[3];
			/* Premultiply with correct rounding: (c * a + 127) / 255. */
			uint32_t r = (s[0] * a + 127) / 255;
			uint32_t g = (s[1] * a + 127) / 255;
			uint32_t b = (s[2] * a + 127) / 255;
			d[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
}

IvImage *iv_image_from_cairo(const uint8_t *src, int w, int h, int stride)
{
	if (src == NULL || stride < w * 4)
		return NULL;
	IvImage *img = iv_image_new(w, h);
	if (img == NULL)
		return NULL;
	for (int y = 0; y < h; y++) {
		const uint32_t *s = (const uint32_t *)(const void *)(src + (size_t)y * (size_t)stride);
		uint8_t *d = ivp_px(img, 0, y);
		for (int x = 0; x < w; x++, d += 4) {
			uint32_t v = s[x];
			uint32_t a = v >> 24;
			if (a == 0) {
				d[0] = d[1] = d[2] = d[3] = 0;
				continue;
			}
			uint32_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
			/* Undo the premultiplication, rounding to nearest. */
			r = (r * 255 + a / 2) / a;
			g = (g * 255 + a / 2) / a;
			b = (b * 255 + a / 2) / a;
			d[0] = (uint8_t)(r > 255 ? 255 : r);
			d[1] = (uint8_t)(g > 255 ? 255 : g);
			d[2] = (uint8_t)(b > 255 ? 255 : b);
			d[3] = (uint8_t)a;
		}
	}
	return img;
}

int iv_image_is_opaque(const IvImage *img)
{
	if (img == NULL)
		return 1;
	for (int y = 0; y < img->h; y++) {
		const uint8_t *p = ivp_px(img, 0, y);
		for (int x = 0; x < img->w; x++)
			if (p[x * 4 + 3] != 255)
				return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------------
 * File helpers shared by the EXIF, JPEG and WebP code
 * --------------------------------------------------------------------- */

uint8_t *ivp_read_file(const char *path, size_t max, size_t *len)
{
	*len = 0;
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	size_t cap = 65536, n = 0;
	uint8_t *buf = malloc(cap);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	for (;;) {
		if (n == cap) {
			if (cap >= max) {
				/* Too large: refuse rather than truncate. */
				free(buf);
				fclose(f);
				return NULL;
			}
			size_t ncap = cap * 2 > max ? max : cap * 2;
			uint8_t *nb = realloc(buf, ncap);
			if (nb == NULL) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = nb;
			cap = ncap;
		}
		size_t got = fread(buf + n, 1, cap - n, f);
		n += got;
		if (got == 0)
			break;
	}
	int err = ferror(f);
	fclose(f);
	if (err) {
		free(buf);
		return NULL;
	}
	*len = n;
	return buf;
}

int ivp_write_file_atomic(const char *path, const uint8_t *data, size_t len)
{
	size_t pl = strlen(path);
	char *tmp = malloc(pl + 16);
	if (tmp == NULL)
		return -1;
	memcpy(tmp, path, pl);
	memcpy(tmp + pl, ".ivproc-tmp", 12);
	FILE *f = fopen(tmp, "wb");
	if (f == NULL) {
		free(tmp);
		return -1;
	}
	int ok = fwrite(data, 1, len, f) == len;
	if (fflush(f) != 0)
		ok = 0;
	if (fclose(f) != 0)
		ok = 0;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}
