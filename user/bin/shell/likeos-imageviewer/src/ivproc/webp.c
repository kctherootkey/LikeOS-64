// LikeOS -- LikeOS Image Viewer: WebP load and save through libwebp.
//
// Still images decode with WebPDecodeRGBA.  Animated files need the demux
// library's animation decoder, which composites frames onto the canvas;
// when it was found at build time the first frame is returned, otherwise
// an animated file cannot be decoded here.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include <webp/decode.h>
#include <webp/encode.h>
#ifdef IV_HAVE_WEBPDEMUX
#include <webp/demux.h>
#endif

#include "ivpriv.h"

/* Upper bound for a WebP file read into memory. */
#define IVP_WEBP_MAX ((size_t)256 << 20)

int iv_webp_probe(const uint8_t *data, size_t len, int *w, int *h,
		  int *has_alpha, int *animated)
{
	if (data == NULL || len < 12)
		return -1;
	WebPBitstreamFeatures ft;
	if (WebPGetFeatures(data, len, &ft) != VP8_STATUS_OK)
		return -1;
	if (w)
		*w = ft.width;
	if (h)
		*h = ft.height;
	if (has_alpha)
		*has_alpha = ft.has_alpha ? 1 : 0;
	if (animated)
		*animated = ft.has_animation ? 1 : 0;
	return 0;
}

#ifdef IV_HAVE_WEBPDEMUX
static IvImage *decode_first_frame(const uint8_t *data, size_t len)
{
	WebPAnimDecoderOptions opt;
	if (!WebPAnimDecoderOptionsInit(&opt))
		return NULL;
	opt.color_mode = MODE_RGBA;
	opt.use_threads = 0;
	WebPData wd = { data, len };
	WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opt);
	if (dec == NULL)
		return NULL;
	WebPAnimInfo info;
	IvImage *img = NULL;
	uint8_t *frame = NULL;
	int ts = 0;
	if (WebPAnimDecoderGetInfo(dec, &info) &&
	    WebPAnimDecoderGetNext(dec, &frame, &ts) && frame != NULL) {
		img = iv_image_new_from_data(frame, (int)info.canvas_width,
					     (int)info.canvas_height,
					     (int)info.canvas_width * 4, 4);
	}
	WebPAnimDecoderDelete(dec);
	return img;
}
#endif

IvImage *iv_webp_decode_mem(const uint8_t *data, size_t len, int *has_alpha)
{
	int w = 0, h = 0, alpha = 0, anim = 0;
	if (iv_webp_probe(data, len, &w, &h, &alpha, &anim) < 0)
		return NULL;
	if (has_alpha)
		*has_alpha = alpha;
	if (w <= 0 || h <= 0)
		return NULL;
	if (anim) {
#ifdef IV_HAVE_WEBPDEMUX
		return decode_first_frame(data, len);
#else
		return NULL;
#endif
	}
	IvImage *img = iv_image_new(w, h);
	if (img == NULL)
		return NULL;
	/* Decode straight into our buffer: same layout (RGBA, straight). */
	if (WebPDecodeRGBAInto(data, len, img->px,
			       (size_t)img->stride * (size_t)h, img->stride) == NULL) {
		iv_image_free(img);
		return NULL;
	}
	return img;
}

IvImage *iv_webp_load(const char *path, int *has_alpha)
{
	if (path == NULL)
		return NULL;
	size_t len = 0;
	uint8_t *data = ivp_read_file(path, IVP_WEBP_MAX, &len);
	if (data == NULL)
		return NULL;
	IvImage *img = iv_webp_decode_mem(data, len, has_alpha);
	free(data);
	return img;
}

int iv_webp_encode_mem(const IvImage *img, int quality, uint8_t **out,
		       size_t *out_len)
{
	if (img == NULL || img->px == NULL || out == NULL || out_len == NULL)
		return -1;
	*out = NULL;
	*out_len = 0;
	/* libwebp's limit. */
	if (img->w > WEBP_MAX_DIMENSION || img->h > WEBP_MAX_DIMENSION)
		return -1;
	uint8_t *enc = NULL;
	size_t n;
	if (quality > 100)
		n = WebPEncodeLosslessRGBA(img->px, img->w, img->h, img->stride, &enc);
	else
		n = WebPEncodeRGBA(img->px, img->w, img->h, img->stride,
				   (float)ivp_clampi(quality, 0, 100), &enc);
	if (n == 0 || enc == NULL) {
		WebPFree(enc);
		return -1;
	}
	/* Hand out plain malloc() memory so callers free() it. */
	*out = malloc(n);
	if (*out == NULL) {
		WebPFree(enc);
		return -1;
	}
	memcpy(*out, enc, n);
	*out_len = n;
	WebPFree(enc);
	return 0;
}

int iv_webp_save(const IvImage *img, const char *path, int quality)
{
	if (path == NULL)
		return -1;
	uint8_t *data = NULL;
	size_t len = 0;
	if (iv_webp_encode_mem(img, quality, &data, &len) < 0)
		return -1;
	int r = ivp_write_file_atomic(path, data, len);
	free(data);
	return r;
}
