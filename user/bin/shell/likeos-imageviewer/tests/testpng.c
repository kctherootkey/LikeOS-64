// LikeOS -- LikeOS Image Viewer: PNG output (and, with libpng, input) for
// the processing library's unit tests.
//
// The writer emits valid PNG files with "stored" (uncompressed) deflate
// blocks: larger than a real encoder's output, but needing no zlib, and
// any viewer opens them.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef IV_TEST_HAVE_PNG
#include <png.h>
#endif

#include "testpng.h"

uint32_t tp_crc32(const uint8_t *data, size_t len, uint32_t crc)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
	uint8_t hdr[8];
	put32(hdr, (uint32_t)len);
	memcpy(hdr + 4, type, 4);
	uint32_t crc = tp_crc32(hdr + 4, 4, 0);
	crc = tp_crc32(data, len, crc);
	uint8_t c[4];
	put32(c, crc);
	return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
			       fwrite(c, 1, 4, f) == 4 ? 0 : -1;
}

static int write_raw(const char *path, const uint8_t *pix, int w, int h,
		     int channels, int stride)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return -1;
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	fwrite(sig, 1, 8, f);
	uint8_t ihdr[13];
	put32(ihdr, (uint32_t)w);
	put32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 8;
	ihdr[9] = channels == 4 ? 6 : 0;
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);

	/* Filtered scanlines (filter 0), then zlib with stored blocks. */
	size_t row = (size_t)w * channels + 1;
	size_t raw_len = row * h;
	uint8_t *raw = malloc(raw_len);
	size_t nblocks = raw_len / 65535 + 1;
	uint8_t *z = malloc(raw_len + nblocks * 5 + 6);
	if (raw == NULL || z == NULL) {
		free(raw);
		free(z);
		fclose(f);
		return -1;
	}
	for (int y = 0; y < h; y++) {
		raw[row * y] = 0;
		memcpy(raw + row * y + 1, pix + (size_t)y * stride, (size_t)w * channels);
	}
	size_t o = 0;
	z[o++] = 0x78;
	z[o++] = 0x01;
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < raw_len; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t off = 0; off < raw_len || off == 0; ) {
		size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
		int last = off + n >= raw_len;
		z[o++] = (uint8_t)last;
		z[o++] = (uint8_t)n;
		z[o++] = (uint8_t)(n >> 8);
		z[o++] = (uint8_t)~n;
		z[o++] = (uint8_t)(~n >> 8);
		memcpy(z + o, raw + off, n);
		o += n;
		off += n;
		if (last)
			break;
	}
	put32(z + o, (b << 16) | a);
	o += 4;
	chunk(f, "IDAT", z, o);
	chunk(f, "IEND", NULL, 0);
	free(raw);
	free(z);
	return fclose(f) == 0 ? 0 : -1;
}

int tp_write_png(const char *path, const IvImage *img)
{
	return write_raw(path, img->px, img->w, img->h, 4, img->stride);
}

int tp_write_mask_png(const char *path, const uint8_t *mask, int w, int h)
{
	return write_raw(path, mask, w, h, 1, w);
}

IvImage *tp_read_png(const char *path)
{
#ifdef IV_TEST_HAVE_PNG
	png_image im;
	memset(&im, 0, sizeof(im));
	im.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&im, path))
		return NULL;
	im.format = PNG_FORMAT_RGBA;
	IvImage *img = iv_image_new((int)im.width, (int)im.height);
	if (img == NULL) {
		png_image_free(&im);
		return NULL;
	}
	if (!png_image_finish_read(&im, NULL, img->px, img->stride, NULL)) {
		iv_image_free(img);
		png_image_free(&im);
		return NULL;
	}
	return img;
#else
	(void)path;
	return NULL;
#endif
}
