/*
 * LikeOS boot loader -- a small anti-aliased renderer on the UEFI Graphics
 * Output Protocol, for the boot menu (boot/menu.c).
 *
 * Everything is drawn into a back buffer in the GOP's BLT pixel layout
 * (one 32-bit word per pixel, 0x00RRGGBB) and copied to the screen a
 * rectangle at a time with gfx_present(): the screen never shows a half
 * drawn frame, and the firmware's Blt() takes care of the display's own
 * pixel format (BGR, RGB, bit masks, or a BLT-only mode).
 *
 * Colours are 0xAARRGGBB with straight (not premultiplied) alpha.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_BOOT_GFX_H
#define LIKEOS_BOOT_GFX_H

#include <efi.h>
#include <efilib.h>

#include "bootfont.h"

typedef UINT32 gfx_color;

#define GFX_RGB(r, g, b) ((gfx_color)(0xff000000u | ((r) << 16) | ((g) << 8) | (b)))
#define GFX_ARGB(a, r, g, b) \
	((gfx_color)(((UINT32)(a) << 24) | ((r) << 16) | ((g) << 8) | (b)))

/* Find the GOP and take its CURRENT mode (the menu runs before the boot
 * loader picks the kernel's mode); allocate the back buffer.  0 on
 * success, -1 when there is no usable graphics output. */
int gfx_init(void);
/* Free the back buffer. */
void gfx_shutdown(void);

INT32 gfx_width(void);
INT32 gfx_height(void);

/* Drawing, clipped to the screen. */
void gfx_fill(INT32 x, INT32 y, INT32 w, INT32 h, gfx_color c);
/* Vertical gradient from `top' to `bottom' (opaque), ordered dither so
 * long gradients do not band. */
void gfx_vgradient(INT32 x, INT32 y, INT32 w, INT32 h, gfx_color top,
		   gfx_color bottom);
/* A soft elliptical glow centred at (cx, cy): colour c at the centre,
 * fading to nothing at radii rx, ry. */
void gfx_glow(INT32 cx, INT32 cy, INT32 rx, INT32 ry, gfx_color c);
/* Anti-aliased rounded rectangle, filled. */
void gfx_round_rect(INT32 x, INT32 y, INT32 w, INT32 h, INT32 r, gfx_color c);
/* Its outline, `t' pixels wide, inside the rectangle. */
void gfx_round_rect_line(INT32 x, INT32 y, INT32 w, INT32 h, INT32 r, INT32 t,
			 gfx_color c);
/* A soft shadow of the rounded rectangle, `blur' pixels wide. */
void gfx_shadow(INT32 x, INT32 y, INT32 w, INT32 h, INT32 r, INT32 blur,
		gfx_color c);
/* Anti-aliased convex polygon (points in pixels, any winding). */
void gfx_polygon(const float *xy, int n, gfx_color c);
/* Anti-aliased disc and ring. */
void gfx_disc(float cx, float cy, float r, gfx_color c);
void gfx_ring(float cx, float cy, float r, float t, gfx_color c);
/* A premultiplied 0xAARRGGBB image scaled to dw x dh at (dx, dy) with an
 * area-averaging filter (down) or bilinear filter (up). */
void gfx_image(const UINT32 *img, INT32 sw, INT32 sh, INT32 dx, INT32 dy,
	       INT32 dw, INT32 dh);

/* Text: UTF-8, one line.  The face closest to `px' of `weight' (0
 * regular, 1 semibold). */
const struct bootfont_face *gfx_face(int weight, int px);
INT32 gfx_text_width(const struct bootfont_face *f, const char *s);
/* Draw with the baseline at `y'; returns the pen position after it. */
INT32 gfx_text(const struct bootfont_face *f, INT32 x, INT32 y, const char *s,
	       gfx_color c);
/* Text cut with an ellipsis to fit `maxw' pixels; returns its width. */
INT32 gfx_text_fit(const struct bootfont_face *f, INT32 x, INT32 y,
		   const char *s, INT32 maxw, gfx_color c);

/* Keep the back buffer as it is now as the background layer (allocated on
 * first use; FALSE when there is no memory for it), and put a rectangle of
 * that layer back into the back buffer -- what a partial redraw starts
 * from. */
BOOLEAN gfx_save_background(void);
void gfx_restore_background(INT32 x, INT32 y, INT32 w, INT32 h);

/* Copy a rectangle of the back buffer to the screen. */
void gfx_present(INT32 x, INT32 y, INT32 w, INT32 h);
/* Clear the screen to black (back buffer and screen). */
void gfx_clear_screen(void);

#endif /* LIKEOS_BOOT_GFX_H */
