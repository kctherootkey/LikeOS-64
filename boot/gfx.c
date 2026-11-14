/*
 * LikeOS boot loader -- the boot menu's renderer (see gfx.h).
 *
 * Shapes are drawn from signed distance functions evaluated at each pixel
 * centre: the distance d from the pixel centre to the shape's edge
 * (negative inside) gives the coverage clamp(0.5 - d, 0, 1), which is
 * exact for straight edges and very close for curves -- the anti-aliasing
 * costs nothing beyond the distance itself.  Text comes from the glyph
 * atlas in bootfont.h (8-bit coverage per pixel), the logo from
 * bootlogo.h, both generated on the build host.
 *
 * Floating point is the SSE unit's, which UEFI hands to applications
 * enabled and configured; the one function a C library would provide,
 * the square root, is the instruction itself.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "gfx.h"

static EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop;
static UINT32 *g_buf;          /* back buffer, g_w * g_h words */
static UINT32 *g_bg;           /* background layer, the same size */
static INT32 g_w, g_h;
static UINT32 g_fb_format;     /* EFI_GRAPHICS_PIXEL_FORMAT */
static UINT32 g_fb_stride;     /* pixels per scan line */
static UINT32 g_mask_shift[3]; /* PixelBitMask: shifts for R, G, B */
static UINT32 g_mask_bits[3];  /* ... and widths */
static UINT32 g_fb_bytes;      /* bytes per framebuffer pixel: 2, 3 or 4;
				* 0 for a layout not drawn directly */
static BOOLEAN g_blt_broken;   /* Blt failed once: write the framebuffer */
static UINT8 g_text_gamma[256];

/* ---- helpers ------------------------------------------------------------ */

static inline float gfx_sqrtf(float x)
{
	float r;

	if (x <= 0.0f)
		return 0.0f;
	__asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
	return r;
}

static inline float gfx_absf(float x)
{
	return x < 0.0f ? -x : x;
}

static inline float gfx_clamp01(float x)
{
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static inline UINT32 div255(UINT32 x)
{
	x += 128;
	return (x + (x >> 8)) >> 8;
}

/* Blend colour c over *d with an extra coverage `cov' (0..255). */
static inline void blend(UINT32 *d, gfx_color c, UINT32 cov)
{
	UINT32 a = div255(((c >> 24) & 0xff) * cov);
	UINT32 dv, ia, r, g, b;

	if (a == 0)
		return;
	if (a >= 255) {
		*d = c & 0x00ffffffu;
		return;
	}
	dv = *d;
	ia = 255 - a;
	r = div255(((c >> 16) & 0xff) * a + ((dv >> 16) & 0xff) * ia);
	g = div255(((c >> 8) & 0xff) * a + ((dv >> 8) & 0xff) * ia);
	b = div255((c & 0xff) * a + (dv & 0xff) * ia);
	*d = (r << 16) | (g << 8) | b;
}

static inline void blendf(UINT32 *d, gfx_color c, float cov)
{
	if (cov <= 0.0f)
		return;
	blend(d, c, cov >= 1.0f ? 255u : (UINT32)(cov * 255.0f + 0.5f));
}

/* Clip [x, x + w) x [y, y + h) to the screen; FALSE when nothing is left. */
static BOOLEAN clip(INT32 *x, INT32 *y, INT32 *w, INT32 *h)
{
	INT32 x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > g_w)
		x1 = g_w;
	if (y1 > g_h)
		y1 = g_h;
	if (x1 <= x0 || y1 <= y0)
		return FALSE;
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return TRUE;
}

/* Distance from (px, py) to a rounded rectangle centred at (cx, cy) with
 * half extents hw, hh and corner radius r; negative inside. */
static float rr_dist(float px, float py, float cx, float cy, float hw, float hh,
		     float r)
{
	float qx = gfx_absf(px - cx) - (hw - r);
	float qy = gfx_absf(py - cy) - (hh - r);
	float mx = qx > 0.0f ? qx : 0.0f;
	float my = qy > 0.0f ? qy : 0.0f;
	float in = qx > qy ? qx : qy;

	if (in > 0.0f)
		in = 0.0f;
	return gfx_sqrtf(mx * mx + my * my) + in - r;
}

/* ---- set-up --------------------------------------------------------------- */

static void mask_info(UINT32 mask, UINT32 *shift, UINT32 *bits)
{
	UINT32 s = 0, b = 0;

	if (mask) {
		while (!(mask & 1)) {
			mask >>= 1;
			s++;
		}
		while (mask & 1) {
			mask >>= 1;
			b++;
		}
	}
	*shift = s;
	*bits = b;
}

int gfx_init(void)
{
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
	EFI_STATUS st;
	UINTN bytes;

	g_gop = NULL;
	st = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol,
			       NULL, (VOID **)&g_gop);
	if (EFI_ERROR(st) || !g_gop || !g_gop->Mode || !g_gop->Mode->Info)
		return -1;
	mi = g_gop->Mode->Info;
	if (mi->HorizontalResolution < 320 || mi->VerticalResolution < 200 ||
	    mi->HorizontalResolution > 16384 || mi->VerticalResolution > 16384)
		return -1;
	g_w = (INT32)mi->HorizontalResolution;
	g_h = (INT32)mi->VerticalResolution;
	g_fb_format = (UINT32)mi->PixelFormat;
	g_fb_stride = mi->PixelsPerScanLine;
	g_blt_broken = FALSE;
	g_fb_bytes = 4;
	if (g_fb_format == PixelBitMask) {
		UINT32 all = mi->PixelInformation.RedMask |
			     mi->PixelInformation.GreenMask |
			     mi->PixelInformation.BlueMask |
			     mi->PixelInformation.ReservedMask;
		UINT32 top = 0;

		mask_info(mi->PixelInformation.RedMask, &g_mask_shift[0],
			  &g_mask_bits[0]);
		mask_info(mi->PixelInformation.GreenMask, &g_mask_shift[1],
			  &g_mask_bits[1]);
		mask_info(mi->PixelInformation.BlueMask, &g_mask_shift[2],
			  &g_mask_bits[2]);
		/* The pixel is as wide as its highest mask bit reaches. */
		while (all) {
			all >>= 1;
			top++;
		}
		g_fb_bytes = top > 24 ? 4 : top > 16 ? 3 : top > 8 ? 2 : 0;
	}
	bytes = (UINTN)g_w * (UINTN)g_h * 4;
	g_buf = NULL;
	st = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, bytes,
			       (VOID **)&g_buf);
	if (EFI_ERROR(st) || !g_buf)
		return -1;
	uefi_call_wrapper(BS->SetMem, 3, g_buf, bytes, 0);

	/* Text coverage is linear; light text on a dark ground reads thin
	 * that way.  Lift the mid tones a little (about gamma 1.4). */
	for (UINT32 i = 0; i < 256; i++) {
		float lin = (float)i / 255.0f;
		float v = 0.5f * (lin + gfx_sqrtf(lin));

		g_text_gamma[i] = (UINT8)(v * 255.0f + 0.5f);
	}
	return 0;
}

void gfx_shutdown(void)
{
	if (g_buf)
		uefi_call_wrapper(BS->FreePool, 1, g_buf);
	if (g_bg)
		uefi_call_wrapper(BS->FreePool, 1, g_bg);
	g_buf = NULL;
	g_bg = NULL;
	g_gop = NULL;
}

BOOLEAN gfx_save_background(void)
{
	UINTN bytes = (UINTN)g_w * (UINTN)g_h * 4;

	if (!g_buf)
		return FALSE;
	if (!g_bg &&
	    EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
					bytes, (VOID **)&g_bg)))
		g_bg = NULL;
	if (!g_bg)
		return FALSE;
	uefi_call_wrapper(BS->CopyMem, 3, g_bg, g_buf, bytes);
	return TRUE;
}

void gfx_restore_background(INT32 x, INT32 y, INT32 w, INT32 h)
{
	if (!g_buf || !g_bg || !clip(&x, &y, &w, &h))
		return;
	for (INT32 j = y; j < y + h; j++)
		uefi_call_wrapper(BS->CopyMem, 3,
				  g_buf + (UINTN)j * (UINTN)g_w + x,
				  g_bg + (UINTN)j * (UINTN)g_w + x, (UINTN)w * 4);
}

INT32 gfx_width(void)
{
	return g_w;
}

INT32 gfx_height(void)
{
	return g_h;
}

/* ---- the screen ------------------------------------------------------------ */

/* Without a working Blt(): straight into the framebuffer, converting to the
 * display's pixel format and size (2, 3 or 4 bytes for PixelBitMask).
 * Never for a BLT-only mode (no framebuffer) or a pixel narrower than
 * 16 bits. */
static void present_direct(INT32 x, INT32 y, INT32 w, INT32 h)
{
	volatile UINT8 *fb;
	UINTN pitch;

	if (!g_gop->Mode->FrameBufferBase || g_fb_format == PixelBltOnly ||
	    g_fb_format >= PixelFormatMax || !g_fb_bytes)
		return;
	fb = (volatile UINT8 *)(UINTN)g_gop->Mode->FrameBufferBase;
	pitch = (UINTN)g_fb_stride * g_fb_bytes;
	for (INT32 j = y; j < y + h; j++) {
		const UINT32 *s = g_buf + (UINTN)j * (UINTN)g_w + x;
		volatile UINT8 *d = fb + (UINTN)j * pitch + (UINTN)x * g_fb_bytes;

		for (INT32 i = 0; i < w; i++, d += g_fb_bytes) {
			UINT32 p = s[i];
			UINT32 r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
			UINT32 v;

			switch (g_fb_format) {
			case PixelBlueGreenRedReserved8BitPerColor:
				v = p;
				break;
			case PixelRedGreenBlueReserved8BitPerColor:
				v = (b << 16) | (g << 8) | r;
				break;
			default: { /* PixelBitMask */
				UINT32 c[3] = { r, g, b };

				v = 0;
				for (int k = 0; k < 3; k++) {
					UINT32 bits = g_mask_bits[k];
					UINT32 cv = bits >= 8 ? c[k] << (bits - 8) :
								c[k] >> (8 - bits);

					v |= cv << g_mask_shift[k];
				}
				break;
			}
			}
			switch (g_fb_bytes) {
			case 4:
				*(volatile UINT32 *)d = v;
				break;
			case 2:
				*(volatile UINT16 *)d = (UINT16)v;
				break;
			default: /* 3, little-endian */
				d[0] = (UINT8)v;
				d[1] = (UINT8)(v >> 8);
				d[2] = (UINT8)(v >> 16);
				break;
			}
		}
	}
}

void gfx_present(INT32 x, INT32 y, INT32 w, INT32 h)
{
	EFI_STATUS st;

	if (!g_buf || !g_gop || !clip(&x, &y, &w, &h))
		return;
	if (!g_blt_broken) {
		st = uefi_call_wrapper(g_gop->Blt, 10, g_gop,
				       (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)g_buf,
				       EfiBltBufferToVideo, (UINTN)x, (UINTN)y,
				       (UINTN)x, (UINTN)y, (UINTN)w, (UINTN)h,
				       (UINTN)g_w * 4);
		if (!EFI_ERROR(st))
			return;
		g_blt_broken = TRUE;
	}
	present_direct(x, y, w, h);
}

void gfx_clear_screen(void)
{
	if (!g_buf)
		return;
	uefi_call_wrapper(BS->SetMem, 3, g_buf, (UINTN)g_w * (UINTN)g_h * 4, 0);
	gfx_present(0, 0, g_w, g_h);
}

/* ---- fills ---------------------------------------------------------------- */

void gfx_fill(INT32 x, INT32 y, INT32 w, INT32 h, gfx_color c)
{
	if (!g_buf || !clip(&x, &y, &w, &h))
		return;
	for (INT32 j = y; j < y + h; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;

		for (INT32 i = x; i < x + w; i++)
			blend(&row[i], c, 255);
	}
}

/* 4x4 ordered dither thresholds, in 1/16 of a level. */
static const UINT8 g_bayer[4][4] = {
	{ 0, 8, 2, 10 },
	{ 12, 4, 14, 6 },
	{ 3, 11, 1, 9 },
	{ 15, 7, 13, 5 },
};

void gfx_vgradient(INT32 x, INT32 y, INT32 w, INT32 h, gfx_color top,
		   gfx_color bottom)
{
	INT32 y0 = y, h0 = h;

	if (!g_buf || h <= 0 || !clip(&x, &y, &w, &h))
		return;
	for (INT32 j = y; j < y + h; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;
		/* channel values in 1/16 levels */
		INT32 t = h0 > 1 ? (INT32)(((j - y0) * 4096) / (h0 - 1)) : 0;
		INT32 c16[3];

		for (int k = 0; k < 3; k++) {
			INT32 a = (INT32)((top >> (16 - 8 * k)) & 0xff);
			INT32 b = (INT32)((bottom >> (16 - 8 * k)) & 0xff);

			c16[k] = a * 16 + ((b - a) * 16 * t) / 4096;
		}
		for (INT32 i = x; i < x + w; i++) {
			INT32 th = g_bayer[j & 3][i & 3];
			UINT32 v = 0;

			for (int k = 0; k < 3; k++) {
				INT32 q = (c16[k] + th) >> 4;

				if (q > 255)
					q = 255;
				if (q < 0)
					q = 0;
				v |= (UINT32)q << (16 - 8 * k);
			}
			row[i] = v;
		}
	}
}

void gfx_glow(INT32 cx, INT32 cy, INT32 rx, INT32 ry, gfx_color c)
{
	INT32 x = cx - rx, y = cy - ry, w = 2 * rx, h = 2 * ry;

	if (!g_buf || rx <= 0 || ry <= 0 || !clip(&x, &y, &w, &h))
		return;
	for (INT32 j = y; j < y + h; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;
		float dy = ((float)j + 0.5f - (float)cy) / (float)ry;

		for (INT32 i = x; i < x + w; i++) {
			float dx = ((float)i + 0.5f - (float)cx) / (float)rx;
			float q = dx * dx + dy * dy;

			if (q >= 1.0f)
				continue;
			q = 1.0f - q;
			/* dithered so the faint edge does not band */
			blendf(&row[i], c,
			       q * q * q + ((float)g_bayer[j & 3][i & 3] - 7.5f) /
						   (16.0f * 255.0f));
		}
	}
}

void gfx_round_rect(INT32 x, INT32 y, INT32 w, INT32 h, INT32 r, gfx_color c)
{
	INT32 cx0 = x, cy0 = y, cw = w, ch = h;
	float hw = (float)w * 0.5f, hh = (float)h * 0.5f;
	float cx = (float)x + hw, cy = (float)y + hh, rad = (float)r;

	if (!g_buf || w <= 0 || h <= 0 || !clip(&cx0, &cy0, &cw, &ch))
		return;
	if (rad > hw)
		rad = hw;
	if (rad > hh)
		rad = hh;
	for (INT32 j = cy0; j < cy0 + ch; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;
		BOOLEAN straight = j >= y + r && j < y + h - r;

		for (INT32 i = cx0; i < cx0 + cw; i++) {
			if (straight && i > x && i < x + w - 1) {
				blend(&row[i], c, 255);
				continue;
			}
			blendf(&row[i], c,
			       gfx_clamp01(0.5f - rr_dist((float)i + 0.5f,
							  (float)j + 0.5f, cx, cy,
							  hw, hh, rad)));
		}
	}
}

void gfx_round_rect_line(INT32 x, INT32 y, INT32 w, INT32 h, INT32 r, INT32 t,
			 gfx_color c)
{
	INT32 cx0 = x, cy0 = y, cw = w, ch = h;
	float hw = (float)w * 0.5f, hh = (float)h * 0.5f;
	float cx = (float)x + hw, cy = (float)y + hh, rad = (float)r;
	float th = (float)t, irad;

	if (!g_buf || w <= 2 * t || h <= 2 * t || t <= 0 ||
	    !clip(&cx0, &cy0, &cw, &ch))
		return;
	if (rad > hw)
		rad = hw;
	if (rad > hh)
		rad = hh;
	irad = rad - th > 0.0f ? rad - th : 0.0f;
	for (INT32 j = cy0; j < cy0 + ch; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;

		for (INT32 i = cx0; i < cx0 + cw; i++) {
			float px = (float)i + 0.5f, py = (float)j + 0.5f;
			float outer = gfx_clamp01(
				0.5f - rr_dist(px, py, cx, cy, hw, hh, rad));
			float inner;

			if (outer <= 0.0f)
				continue;
			inner = gfx_clamp01(0.5f - rr_dist(px, py, cx, cy,
							   hw - th, hh - th,
							   irad));
			blendf(&row[i], c, outer - inner);
		}
	}
}

void gfx_shadow(INT32 x, INT32 y, INT32 w, INT32 h, INT32 r, INT32 blur,
		gfx_color c)
{
	INT32 cx0 = x - blur, cy0 = y - blur, cw = w + 2 * blur, ch = h + 2 * blur;
	float hw = (float)w * 0.5f, hh = (float)h * 0.5f;
	float cx = (float)x + hw, cy = (float)y + hh, rad = (float)r;
	float b = (float)(blur > 0 ? blur : 1);

	if (!g_buf || w <= 0 || h <= 0 || !clip(&cx0, &cy0, &cw, &ch))
		return;
	if (rad > hw)
		rad = hw;
	if (rad > hh)
		rad = hh;
	for (INT32 j = cy0; j < cy0 + ch; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;

		for (INT32 i = cx0; i < cx0 + cw; i++) {
			float d = rr_dist((float)i + 0.5f, (float)j + 0.5f, cx,
					  cy, hw, hh, rad);
			float a;

			/* falls from full at half a blur inside the edge to
			 * nothing at a blur outside it */
			a = gfx_clamp01((b - d) / (1.5f * b));
			blendf(&row[i], c, a * a * (3.0f - 2.0f * a));
		}
	}
}

void gfx_polygon(const float *xy, int n, gfx_color c)
{
	float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f, area = 0.0f;
	float nx[16], ny[16], nd[16];
	INT32 x, y, w, h;

	if (!g_buf || n < 3 || n > 16)
		return;
	for (int k = 0; k < n; k++) {
		float x0 = xy[2 * k], y0 = xy[2 * k + 1];
		float x1 = xy[2 * ((k + 1) % n)], y1 = xy[2 * ((k + 1) % n) + 1];

		area += x0 * y1 - x1 * y0;
		if (x0 < minx)
			minx = x0;
		if (x0 > maxx)
			maxx = x0;
		if (y0 < miny)
			miny = y0;
		if (y0 > maxy)
			maxy = y0;
	}
	/* inward unit normals of the edges */
	for (int k = 0; k < n; k++) {
		float x0 = xy[2 * k], y0 = xy[2 * k + 1];
		float x1 = xy[2 * ((k + 1) % n)], y1 = xy[2 * ((k + 1) % n) + 1];
		float ex = x1 - x0, ey = y1 - y0;
		float len = gfx_sqrtf(ex * ex + ey * ey);

		if (len <= 0.0f)
			len = 1.0f;
		/* screen coordinates (y down): for a positive area the
		 * inside is on the right of each edge */
		if (area > 0.0f) {
			nx[k] = -ey / len;
			ny[k] = ex / len;
		} else {
			nx[k] = ey / len;
			ny[k] = -ex / len;
		}
		nd[k] = -(nx[k] * x0 + ny[k] * y0);
	}
	x = (INT32)minx - 1;
	y = (INT32)miny - 1;
	w = (INT32)maxx - x + 2;
	h = (INT32)maxy - y + 2;
	if (!clip(&x, &y, &w, &h))
		return;
	for (INT32 j = y; j < y + h; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;
		float py = (float)j + 0.5f;

		for (INT32 i = x; i < x + w; i++) {
			float px = (float)i + 0.5f, m = 1e9f;

			for (int k = 0; k < n; k++) {
				float d = nx[k] * px + ny[k] * py + nd[k];

				if (d < m)
					m = d;
			}
			blendf(&row[i], c, gfx_clamp01(0.5f + m));
		}
	}
}

void gfx_disc(float cx, float cy, float r, gfx_color c)
{
	INT32 x = (INT32)(cx - r) - 1, y = (INT32)(cy - r) - 1;
	INT32 w = (INT32)(2.0f * r) + 3, h = w;

	if (!g_buf || r <= 0.0f || !clip(&x, &y, &w, &h))
		return;
	for (INT32 j = y; j < y + h; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;
		float dy = (float)j + 0.5f - cy;

		for (INT32 i = x; i < x + w; i++) {
			float dx = (float)i + 0.5f - cx;

			blendf(&row[i], c,
			       gfx_clamp01(0.5f - (gfx_sqrtf(dx * dx + dy * dy) - r)));
		}
	}
}

void gfx_ring(float cx, float cy, float r, float t, gfx_color c)
{
	INT32 x = (INT32)(cx - r) - 1, y = (INT32)(cy - r) - 1;
	INT32 w = (INT32)(2.0f * r) + 3, h = w;
	float mid = r - t * 0.5f;

	if (!g_buf || r <= 0.0f || t <= 0.0f || !clip(&x, &y, &w, &h))
		return;
	for (INT32 j = y; j < y + h; j++) {
		UINT32 *row = g_buf + (UINTN)j * (UINTN)g_w;
		float dy = (float)j + 0.5f - cy;

		for (INT32 i = x; i < x + w; i++) {
			float dx = (float)i + 0.5f - cx;
			float d = gfx_absf(gfx_sqrtf(dx * dx + dy * dy) - mid) -
				  t * 0.5f;

			blendf(&row[i], c, gfx_clamp01(0.5f - d));
		}
	}
}

/* ---- images ------------------------------------------------------------------ */

/* Composite one premultiplied pixel (components in 1/256 units over 255). */
static inline void put_premul(UINT32 *d, UINT32 a, UINT32 r, UINT32 g, UINT32 b)
{
	UINT32 dv = *d, ia;

	if (a == 0)
		return;
	if (a > 255)
		a = 255;
	ia = 255 - a;
	r += div255(((dv >> 16) & 0xff) * ia);
	g += div255(((dv >> 8) & 0xff) * ia);
	b += div255((dv & 0xff) * ia);
	*d = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) |
	     (b > 255 ? 255 : b);
}

void gfx_image(const UINT32 *img, INT32 sw, INT32 sh, INT32 dx, INT32 dy,
	       INT32 dw, INT32 dh)
{
	if (!g_buf || !img || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	for (INT32 j = 0; j < dh; j++) {
		INT32 ty = dy + j;
		UINT32 *row;

		if (ty < 0 || ty >= g_h)
			continue;
		row = g_buf + (UINTN)ty * (UINTN)g_w;
		for (INT32 i = 0; i < dw; i++) {
			INT32 tx = dx + i;
			float acc[4] = { 0, 0, 0, 0 }, wsum = 0.0f;

			if (tx < 0 || tx >= g_w)
				continue;
			if (dw <= sw && dh <= sh) {
				/* area average over the source footprint */
				float fx0 = (float)i * (float)sw / (float)dw;
				float fx1 = (float)(i + 1) * (float)sw / (float)dw;
				float fy0 = (float)j * (float)sh / (float)dh;
				float fy1 = (float)(j + 1) * (float)sh / (float)dh;

				for (INT32 sy = (INT32)fy0; sy < sh && (float)sy < fy1;
				     sy++) {
					float wy = 1.0f;

					if ((float)sy < fy0)
						wy -= fy0 - (float)sy;
					if ((float)(sy + 1) > fy1)
						wy -= (float)(sy + 1) - fy1;
					for (INT32 sx = (INT32)fx0;
					     sx < sw && (float)sx < fx1; sx++) {
						float wx = 1.0f, wgt;
						UINT32 p = img[(UINTN)sy * (UINTN)sw +
							       (UINTN)sx];

						if ((float)sx < fx0)
							wx -= fx0 - (float)sx;
						if ((float)(sx + 1) > fx1)
							wx -= (float)(sx + 1) - fx1;
						wgt = wx * wy;
						acc[0] += wgt * (float)(p >> 24);
						acc[1] += wgt * (float)((p >> 16) & 0xff);
						acc[2] += wgt * (float)((p >> 8) & 0xff);
						acc[3] += wgt * (float)(p & 0xff);
						wsum += wgt;
					}
				}
			} else {
				/* bilinear */
				float fx = ((float)i + 0.5f) * (float)sw / (float)dw - 0.5f;
				float fy = ((float)j + 0.5f) * (float)sh / (float)dh - 0.5f;
				INT32 x0 = (INT32)(fx < 0 ? -1 : fx);
				INT32 y0 = (INT32)(fy < 0 ? -1 : fy);
				float ax = fx - (float)x0, ay = fy - (float)y0;

				for (int k = 0; k < 4; k++) {
					INT32 sx = x0 + (k & 1), sy = y0 + (k >> 1);
					float wgt = ((k & 1) ? ax : 1.0f - ax) *
						    ((k >> 1) ? ay : 1.0f - ay);
					UINT32 p;

					if (sx < 0)
						sx = 0;
					if (sy < 0)
						sy = 0;
					if (sx >= sw)
						sx = sw - 1;
					if (sy >= sh)
						sy = sh - 1;
					p = img[(UINTN)sy * (UINTN)sw + (UINTN)sx];
					acc[0] += wgt * (float)(p >> 24);
					acc[1] += wgt * (float)((p >> 16) & 0xff);
					acc[2] += wgt * (float)((p >> 8) & 0xff);
					acc[3] += wgt * (float)(p & 0xff);
					wsum += wgt;
				}
			}
			if (wsum <= 0.0f)
				continue;
			put_premul(&row[tx], (UINT32)(acc[0] / wsum + 0.5f),
				   (UINT32)(acc[1] / wsum + 0.5f),
				   (UINT32)(acc[2] / wsum + 0.5f),
				   (UINT32)(acc[3] / wsum + 0.5f));
		}
	}
}

/* ---- text ------------------------------------------------------------------- */

const struct bootfont_face *gfx_face(int weight, int px)
{
	const struct bootfont_face *best = NULL;
	int bestd = 1 << 30;

	for (int i = 0; i < BOOTFONT_NFACES; i++) {
		const struct bootfont_face *f = &bootfont_faces[i];
		int d = (int)f->px - px;

		if (f->weight != weight)
			continue;
		if (d < 0)
			d = -d;
		/* on a tie the smaller face: text must fit */
		if (d < bestd || (d == bestd && best && f->px < best->px)) {
			best = f;
			bestd = d;
		}
	}
	return best ? best : &bootfont_faces[0];
}

/* Next code point of UTF-8 text; malformed bytes give U+FFFD. */
static UINT32 utf8_next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	UINT32 c = s[0], need, cp;

	if (c < 0x80) {
		*ps += 1;
		return c;
	}
	if ((c & 0xe0) == 0xc0) {
		need = 1;
		cp = c & 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		need = 2;
		cp = c & 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		need = 3;
		cp = c & 0x07;
	} else {
		*ps += 1;
		return 0xfffd;
	}
	for (UINT32 k = 1; k <= need; k++) {
		if ((s[k] & 0xc0) != 0x80) {
			*ps += k;
			return 0xfffd;
		}
		cp = (cp << 6) | (s[k] & 0x3f);
	}
	*ps += need + 1;
	return cp;
}

static const struct bootfont_glyph *glyph_of(const struct bootfont_face *f,
					     UINT32 cp)
{
	INT32 lo = 0, hi = (INT32)f->nglyphs - 1;

	while (lo <= hi) {
		INT32 mid = (lo + hi) / 2;

		if (f->glyphs[mid].cp == cp)
			return &f->glyphs[mid];
		if (f->glyphs[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	if (cp != '?')
		return glyph_of(f, '?');
	return NULL;
}

static INT32 kern64(const struct bootfont_face *f, UINT32 a, UINT32 b)
{
	if (!f->kern || !f->nkern || a > 0x7f || b > 0x7f)
		return 0;
	for (UINT32 i = 0; i < f->nkern; i++)
		if (f->kern[i].a == a && f->kern[i].b == b)
			return f->kern[i].adj64;
	return 0;
}

INT32 gfx_text_width(const struct bootfont_face *f, const char *s)
{
	INT32 pen = 0;
	UINT32 prev = 0;

	while (s && *s) {
		UINT32 cp = utf8_next(&s);
		const struct bootfont_glyph *g = glyph_of(f, cp);

		if (!g)
			continue;
		pen += kern64(f, prev, cp) + g->adv64;
		prev = cp;
	}
	return (pen + 32) >> 6;
}

static void draw_glyph(const struct bootfont_face *f,
		       const struct bootfont_glyph *g, INT32 x, INT32 y,
		       gfx_color c)
{
	const unsigned char *cov = f->cov + g->off;
	INT32 gx = x + g->left, gy = y - g->top;

	for (INT32 j = 0; j < (INT32)g->h; j++) {
		INT32 ty = gy + j;
		UINT32 *row;

		if (ty < 0 || ty >= g_h)
			continue;
		row = g_buf + (UINTN)ty * (UINTN)g_w;
		for (INT32 i = 0; i < (INT32)g->w; i++) {
			INT32 tx = gx + i;
			UINT32 a = cov[(UINTN)j * g->w + (UINTN)i];

			if (!a || tx < 0 || tx >= g_w)
				continue;
			blend(&row[tx], c, g_text_gamma[a]);
		}
	}
}

INT32 gfx_text(const struct bootfont_face *f, INT32 x, INT32 y, const char *s,
	       gfx_color c)
{
	INT32 pen = x * 64;
	UINT32 prev = 0;

	if (!g_buf || !f)
		return x;
	while (s && *s) {
		UINT32 cp = utf8_next(&s);
		const struct bootfont_glyph *g = glyph_of(f, cp);

		if (!g)
			continue;
		pen += kern64(f, prev, cp);
		if (g->w && g->h)
			draw_glyph(f, g, (pen + 32) >> 6, y, c);
		pen += g->adv64;
		prev = cp;
	}
	return (pen + 32) >> 6;
}

INT32 gfx_text_fit(const struct bootfont_face *f, INT32 x, INT32 y,
		   const char *s, INT32 maxw, gfx_color c)
{
	static const char ell[] = "\xe2\x80\xa6"; /* U+2026 */
	char buf[256];
	UINTN n = 0, cut;
	INT32 ew;

	if (gfx_text_width(f, s) <= maxw)
		return gfx_text(f, x, y, s, c) - x;
	ew = gfx_text_width(f, ell);
	while (s[n] && n + 4 < sizeof(buf)) {
		buf[n] = s[n];
		n++;
	}
	buf[n] = 0;
	/* drop whole characters from the end until text + ellipsis fit */
	while (n > 0) {
		cut = n - 1;
		while (cut > 0 && ((unsigned char)buf[cut] & 0xc0) == 0x80)
			cut--;
		n = cut;
		buf[n] = 0;
		while (n > 0 && buf[n - 1] == ' ')
			buf[--n] = 0;
		if (gfx_text_width(f, buf) + ew <= maxw)
			break;
	}
	buf[n] = ell[0];
	buf[n + 1] = ell[1];
	buf[n + 2] = ell[2];
	buf[n + 3] = 0;
	return gfx_text(f, x, y, buf, c) - x;
}
