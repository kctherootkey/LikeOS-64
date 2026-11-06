// LikeOS -- LikeOS Image Viewer: the image processing library (ivproc).
//
// Everything the editor does to pixels lives behind this header: scaling,
// the Adjust sliders, the Styles presets, auto enhance, geometry, blur,
// retouch inpainting, background estimation and its masks, histograms, the
// EXIF reader, JPEG metadata carry-over and WebP load/save.
//
// The library is plain C11 with <math.h>; it does not know GTK.  The only
// external dependency is libwebp (and, when present, libwebpdemux for the
// first frame of animated files).
//
// Conventions used by every function below:
//  - IvImage pixels are RGBA, 8 bits per channel, STRAIGHT (not
//    premultiplied) alpha, rows `stride` bytes apart (stride >= 4 * w).
//    Filters that mix neighbouring pixels premultiply internally, so
//    transparent pixels never leak their colour into visible ones.
//  - Masks are one byte per pixel, w * h, rows tightly packed (no stride).
//  - Functions returning IvImage * return NULL only when memory runs out or
//    the arguments are invalid; the input is never modified by those.
//  - "In place" functions keep w, h and stride.
//  - Nothing here keeps global mutable state; every function is safe to call
//    from any thread as long as two threads do not share one IvImage/mask.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef IVPROC_H
#define IVPROC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Images
 * --------------------------------------------------------------------- */

typedef struct {
	int w, h, stride;
	uint8_t *px;
} IvImage; /* RGBA8, straight alpha */

/* New image, all pixels transparent black (0,0,0,0).  stride = 4 * w. */
IvImage *iv_image_new(int w, int h);
void iv_image_free(IvImage *img);
IvImage *iv_image_copy(const IvImage *img);

/* Resample to w x h.  quality: 0 bilinear, 1 bicubic (Catmull-Rom),
 * 2 Lanczos3.  All three are separable and widen their kernel when
 * shrinking, so a large downscale averages every source pixel instead of
 * skipping rows (no aliasing).  Alpha is handled premultiplied. */
IvImage *iv_image_scale(const IvImage *img, int w, int h, int quality);

/* ------------------------------------------------------------------------
 * Adjust
 * --------------------------------------------------------------------- */

typedef struct {
	double brightness, exposure, contrast, highlights, shadows, whites,
		blacks, vignette, saturation, vibrance, warmth, tint, clarity,
		sharpness, denoise, fade, grain;
} IvAdjust;
/* every field -100..100, 0 = neutral.  Meaning of the signs:
 *   brightness   midtone lift (+) / lower (-); black and white stay put
 *   exposure     +-2 stops on linear light, highlights roll off softly
 *   contrast     S-curve (+) / flatter (-) around mid-grey
 *   highlights   recover (-) / boost (+) the bright areas (local)
 *   shadows      lift (+) / deepen (-) the dark areas (local)
 *   whites       move the white end up (+) / down (-)
 *   blacks       lift (+) / crush (-) the black end
 *   vignette     darken (-) / lighten (+) the corners, elliptical, feathered
 *   saturation   all colours more (+) / less (-), -100 = grey
 *   vibrance     muted colours more (+) / less (-); skin and already
 *                saturated colours are protected
 *   warmth       amber (+) / blue (-) white balance
 *   tint         magenta (+) / green (-) white balance
 *   clarity      local (mid-frequency) contrast, (-) softens
 *   sharpness    unsharp mask (+) / gentle softening (-)
 *   denoise      edge preserving noise reduction (+); (-) does nothing
 *   fade         washed-out matte look (+) / deeper blacks (-)
 *   grain        film grain; (+) coarse, (-) fine, |value| = strength
 */

void iv_adjust_reset(IvAdjust *a);
int iv_adjust_is_neutral(const IvAdjust *a);
void iv_adjust_apply(IvImage *img, const IvAdjust *a); /* in place */

/* ------------------------------------------------------------------------
 * Styles
 * --------------------------------------------------------------------- */

typedef enum {
	IV_STYLE_NONE = 0,
	IV_STYLE_NATURAL,	/* "Natural": gentle clarity of tone and colour */
	IV_STYLE_CRISP,		/* "Crisp": firm contrast, clean colour */
	IV_STYLE_AMBER,		/* "Amber": warm late-afternoon light */
	IV_STYLE_GLOW,		/* "Glow": soft lifted highlights, warm skin */
	IV_STYLE_VELVET,	/* "Velvet": deep, rich, slightly dark */
	IV_STYLE_FROST,		/* "Frost": cool, bright, low saturation */
	IV_STYLE_AZURE,		/* "Azure": clear blue skies and water */
	IV_STYLE_NOIR,		/* "Noir": hard black and white */
	IV_STYLE_SILVER,	/* "Silver": soft black and white */
	IV_STYLE_SEPIA_PRINT,	/* "Sepia Print": toned monochrome print */
	IV_STYLE_FILM,		/* "Film": print-film curve and grain */
	IV_STYLE_HAZE,		/* "Haze": faded matte with cream highlights */
	IV_STYLE_NEON_NIGHT,	/* "Neon Night": magenta/cyan split, deep blacks */
	IV_STYLE_HARBOR,	/* "Harbor": teal shadows, warm highlights */
	IV_STYLE_BLOOM,		/* "Bloom": bright pastel spring colours */
	IV_STYLE_MEADOW,	/* "Meadow": fresh greens, soft warmth */
	IV_STYLE_EMBER,		/* "Ember": glowing reds and oranges */
	IV_STYLE_DUSK,		/* "Dusk": violet shadows, rose highlights */
	IV_STYLE_MATTE,		/* "Matte": lifted blacks, muted colour */
	IV_STYLE_LAGOON,	/* "Lagoon": turquoise and sand */
	IV_STYLE_COUNT
} IvStyle;

/* Display name ("Natural", ...); "Original" for IV_STYLE_NONE, "" for out
 * of range values. */
const char *iv_style_name(IvStyle s);
/* intensity 0..1 blends between the original (0) and the full style (1). */
void iv_style_apply(IvImage *img, IvStyle s, double intensity);

/* Histogram-based suggestion: levels (blacks/whites), exposure/brightness,
 * gentle contrast, white balance (warmth/tint), shadow lift for
 * under-exposed images.  Every other field of *out is 0. */
void iv_auto_enhance(const IvImage *img, IvAdjust *out);

/* ------------------------------------------------------------------------
 * Geometry
 * --------------------------------------------------------------------- */

/* quarter_turns_cw: any integer; it is taken modulo 4 (-1 = 90 deg left). */
IvImage *iv_rotate90(const IvImage *img, int quarter_turns_cw);
/* horizontal != 0: mirror left<->right; else top<->bottom.  In place. */
void iv_flip(IvImage *img, int horizontal);
/* Rotate by `degrees` clockwise about the centre, bicubic.
 *   crop_to_fit 0: the result is the bounding box of the rotated image,
 *                  uncovered corners transparent.
 *   crop_to_fit 1: the result is the largest axis-aligned rectangle inside
 *                  the rotated image (iv_straighten_inner_rect), centred.
 *   crop_to_fit 2: like 1 but keeping the original aspect ratio
 *                  (iv_straighten_inner_rect_aspect). */
IvImage *iv_rotate_free(const IvImage *img, double degrees, int crop_to_fit);
/* Size of the largest-area axis-aligned rectangle that fits inside a w x h
 * rectangle rotated by `degrees` (exact closed form). */
void iv_straighten_inner_rect(int w, int h, double degrees, double *out_w,
			      double *out_h);
IvImage *iv_crop(const IvImage *img, int x, int y, int w, int h);

/* ------------------------------------------------------------------------
 * Blur / sharpen
 * --------------------------------------------------------------------- */

/* Gaussian blur, in place.  sigma in pixels; small sigma uses a true
 * Gaussian kernel, large sigma three box passes.  sigma <= 0: no-op. */
void iv_gaussian_blur(IvImage *img, double sigma);
/* Unsharp mask: img += amount * (img - blur(img, sigma)), in place. */
void iv_unsharp(IvImage *img, double sigma, double amount);

/* ------------------------------------------------------------------------
 * Retouch and background
 * --------------------------------------------------------------------- */

typedef int (*IvProgress)(double fraction, void *user); /* nonzero = cancel */

/* Fill every pixel whose mask byte is >= 128 from the surrounding image
 * (exemplar-based, multi-scale for large holes, seam blended).
 * Returns 0 when done, 1 when cancelled through the callback (img is then
 * left untouched), -1 on bad arguments / out of memory. */
int iv_inpaint(IvImage *img, const uint8_t *mask /*w*h, 255 = fill*/,
	       IvProgress cb, void *user);

/* Estimate the background.  mask_out (w*h) always receives the best
 * estimate (255 = background, soft edge).  Returns 1 when a clear
 * background was found, 0 when not (mask_out then holds the weak guess or
 * zeros -- the UI should offer the refine brush), -1 when cancelled or on
 * bad arguments / out of memory. */
int iv_bg_estimate(const IvImage *img, uint8_t *mask_out /*w*h, 255 = background*/,
		   IvProgress cb, void *user);

/* One soft round dab centred at (x, y), radius r pixels: moves the mask
 * towards `value` (0..255) by the brush coverage.  hardness 0..1: the
 * fraction of the radius painted at full strength; the rest falls off
 * smoothly. */
void iv_mask_dab(uint8_t *mask, int w, int h, double x, double y, double r,
		 int value, double hardness);
/* Gaussian feather of a mask, radius ~ 2 sigma, in place. */
void iv_mask_feather(uint8_t *mask, int w, int h, double radius);

typedef enum { IV_BG_BLUR, IV_BG_REMOVE, IV_BG_REPLACE } IvBgOp;
/* mask 255 = background.
 *   IV_BG_BLUR     strength 0..100 = blur radius up to 3 % of the long edge;
 *                  the blur only gathers background pixels, so the subject
 *                  never bleeds into it.
 *   IV_BG_REMOVE   alpha *= 1 - mask; strength ignored.
 *   IV_BG_REPLACE  composite rgba (0xRRGGBBAA) under the subject;
 *                  strength ignored. */
void iv_bg_apply(IvImage *img, const uint8_t *mask, IvBgOp op, double strength,
		 uint32_t rgba);

/* ------------------------------------------------------------------------
 * Histogram
 * --------------------------------------------------------------------- */

/* out[0..2] = R, G, B counts, out[3] = luma (Rec. 709); pixels with alpha 0
 * are not counted. */
void iv_histogram(const IvImage *img, uint32_t out[4][256]);

/* ------------------------------------------------------------------------
 * EXIF / file information
 * --------------------------------------------------------------------- */

typedef struct {
	char make[64], model[64], lens[96], software[96], datetime[32],
		datetime_original[32];
	double exposure_time, fnumber, focal_length, gps_lat, gps_lon,
		gps_alt, x_dpi, y_dpi;
	int iso, flash, orientation, color_space, has_gps, width, height, bits;
	/* ---- additions (append only) ---- */
	char lens_make[64], artist[64], copyright[96], datetime_digitized[32];
	char color_type[32];	/* "RGB", "RGBA", "Greyscale", "Palette", "CMYK" ... */
	char format[16];	/* "JPEG", "PNG", "WebP", "TIFF", "GIF", "BMP" */
	double exposure_bias;	/* EV */
	int focal_length_35mm;	/* mm, 0 = unknown */
	int exposure_program, metering_mode, white_balance; /* raw tag values, -1 = none */
	int channels;		/* samples per pixel, 0 = unknown */
	int has_exif;		/* 1 when an EXIF block was parsed */
	int has_alt;		/* gps_alt is valid */
	int animated;		/* GIF / WebP with more than one frame (best effort) */
} IvExif;
/* width/height: the real pixel size from the container (JPEG SOF, PNG
 * IHDR, WebP, GIF, BMP, TIFF IFD0); bits: bits per PIXEL (24 for 8-bit
 * RGB, 32 for RGBA, 8 for greyscale); x/y_dpi from EXIF, else JFIF
 * density / PNG pHYs / BMP header.
 * Absent values: strings "", doubles 0, iso/color_space 0, flash and the
 * other raw tag values -1, orientation 1, width/height/bits 0.
 * Returns 1 when an EXIF block was found and parsed, 0 when the file was
 * recognised but has no EXIF (basic fields may still be filled), -1 when
 * the file cannot be read or its format is unknown. */
int iv_exif_read_file(const char *path, IvExif *out); /* JPEG APP1, PNG eXIf, TIFF, WebP EXIF */

/* The EXIF APP1 payload of a JPEG file ("Exif\0\0" + TIFF data, without the
 * FF E1 marker and the length field).  *data is malloc()ed; free() it.
 * Returns 0 on success, -1 when the file has none or cannot be read. */
int iv_jpeg_get_app1(const char *path, uint8_t **data, size_t *len);
/* Insert an APP1 payload (as returned above) into the JPEG file jpeg_path,
 * right after SOI and any JFIF APP0, replacing an EXIF APP1 already there.
 * The copy that is written gets PixelX/YDimension set to the file's real
 * size and its thumbnail (IFD1) unlinked and zeroed, since the pixels
 * changed (the old pixels must not survive in the copy);
 * reset_orientation != 0 also sets the Orientation tag to 1.  The file is
 * rewritten through a temporary file and rename().  Returns 0 / -1. */
int iv_jpeg_write_with_app1(const char *jpeg_path, const uint8_t *app1,
			    size_t len, int reset_orientation);

/* ------------------------------------------------------------------------
 * WebP
 * --------------------------------------------------------------------- */

/* Decode a WebP file (first frame of an animation).  *has_alpha (may be
 * NULL) receives whether the file carries alpha. */
IvImage *iv_webp_load(const char *path, int *has_alpha);
/* quality 0..100 lossy, 101 = lossless.  Returns 0 / -1.  The file is
 * written through a temporary file and rename(). */
int iv_webp_save(const IvImage *img, const char *path, int quality /*0..100, 101 = lossless*/);

/* ========================================================================
 * Additions to the agreed API (append only)
 * ===================================================================== */

/* Import from / export to other pixel layouts.
 *   iv_image_new_from_data: rows of 3 (RGB) or 4 (RGBA, straight alpha)
 *   bytes per pixel, e.g. a GdkPixbuf's pixels.
 *   iv_image_to_cairo / iv_image_from_cairo: cairo's CAIRO_FORMAT_ARGB32
 *   (native-endian 32-bit words, PREMULTIPLIED), e.g. to draw strokes on
 *   top of the image with cairo and come back. */
IvImage *iv_image_new_from_data(const uint8_t *data, int w, int h, int stride,
				int channels);
void iv_image_to_cairo(const IvImage *img, uint8_t *dst, int dst_stride);
IvImage *iv_image_from_cairo(const uint8_t *src, int w, int h, int stride);
/* 1 when every pixel has alpha 255. */
int iv_image_is_opaque(const IvImage *img);

/* Like iv_adjust_apply, for previews: detail_scale = preview width / full
 * resolution width (1.0 = full size).  The radius-based operations
 * (sharpness, denoise, grain size) are scaled by it so that a proxy
 * preview looks like the full-resolution result; the large-scale ones
 * (highlights/shadows, clarity, vignette) are relative to the image size
 * anyway. */
void iv_adjust_apply_ex(IvImage *img, const IvAdjust *a, double detail_scale);

/* Like iv_straighten_inner_rect, keeping the w:h aspect ratio. */
void iv_straighten_inner_rect_aspect(int w, int h, double degrees,
				     double *out_w, double *out_h);

/* Resample a mask (bilinear when enlarging, box average when shrinking). */
void iv_mask_scale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw,
		   int dh);
/* Bounding box of the mask bytes >= threshold.  Returns 0 when empty. */
int iv_mask_bbox(const uint8_t *mask, int w, int h, int threshold, int *x,
		 int *y, int *bw, int *bh);

/* EXIF from memory: a whole file image (format sniffed), or a bare TIFF
 * structure ("II*\0" / "MM\0*"), or an APP1 payload starting with
 * "Exif\0\0".  Same return values as iv_exif_read_file. */
int iv_exif_read_mem(const uint8_t *data, size_t len, IvExif *out);
int iv_exif_parse_tiff(const uint8_t *tiff, size_t len, IvExif *out);
/* Human-readable texts for the info panel ("Flash fired, auto mode",
 * "Rotate 90 deg clockwise", "Manual", ...).  Never NULL. */
const char *iv_exif_flash_string(int flash);
const char *iv_exif_orientation_string(int orientation);
const char *iv_exif_exposure_program_string(int program);
const char *iv_exif_metering_mode_string(int mode);
const char *iv_exif_color_space_string(int color_space);
/* 48 deg 51' 29.6" N style text for a decimal coordinate. */
void iv_exif_format_dms(double deg, int is_latitude, char *buf, size_t len);
/* "1/250 s", "2.5 s" */
void iv_exif_format_exposure(double seconds, char *buf, size_t len);

/* WebP from memory, and the header-only probe (any argument may be NULL).
 * iv_webp_probe returns 0 when the data is WebP. */
IvImage *iv_webp_decode_mem(const uint8_t *data, size_t len, int *has_alpha);
int iv_webp_probe(const uint8_t *data, size_t len, int *w, int *h,
		  int *has_alpha, int *animated);
/* Encode to memory; *out is malloc()ed.  Returns 0 / -1. */
int iv_webp_encode_mem(const IvImage *img, int quality, uint8_t **out,
		       size_t *out_len);

/* Allocation failures inside the library on the calling thread since the
 * last iv_alloc_failures_reset().  The in-place operations above return
 * nothing and leave the picture as it is (or only partly done) when a
 * buffer cannot be had; a caller that must not pass on such a result (the
 * editor saving at full size) resets the count before a step and checks
 * it after. */
void iv_alloc_failures_reset(void);
unsigned iv_alloc_failures(void);

#ifdef __cplusplus
}
#endif

#endif /* IVPROC_H */
