/*
 * Tests for the Intel display's plane arithmetic
 * (kernel/dev/gpu/i915/display/intel_plane.c): PLANE_CTL rotation and flip
 * encodings, tile-aligned surface offsets, the rotated GGTT view layout,
 * and the clipping of rotated planes with the rectangle helpers.  See
 * test-intel-plane.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

/* The kernel's fixed-width types come first and stand in for stdint.h,
 * whose typedefs differ in spelling (long vs long long) on the host. */
#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/uapi/drm/drm_mode.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define PLANE_CTL_ROTATE_180 0x2u
#include <kernel/dev/gpu/i915/skl_universal_plane_regs.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- what the cut-out code needs ----------------------------------------- */

static int warnings;

int kprintf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (!strncmp(buf, "WARNING", 7))
		warnings++;
	return n;
}

void panic(const char *fmt, ...)
{
	(void)fmt;
	printf("panic\n");
	__builtin_trap();
}

/* the framebuffer fields the layout reads */
struct drm_framebuffer {
	uint32_t width, height, pitch, offset, cpp;
};

static uint32_t fb_cpp(const struct drm_framebuffer *fb)
{
	return fb->cpp;
}

#include "plane_cut.h"

/* ---- tests --------------------------------------------------------------- */

static void test_ctl_bits(void)
{
	CHECK(skl_plane_ctl_rotate(DRM_MODE_ROTATE_0) == 0, "rotate 0");
	/* counter-clockwise property, clockwise hardware */
	CHECK(skl_plane_ctl_rotate(DRM_MODE_ROTATE_90) == PLANE_CTL_ROTATE_270, "rotate 90");
	CHECK(skl_plane_ctl_rotate(DRM_MODE_ROTATE_180) == PLANE_CTL_ROTATE_180, "rotate 180");
	CHECK(skl_plane_ctl_rotate(DRM_MODE_ROTATE_270) == PLANE_CTL_ROTATE_90, "rotate 270");
	CHECK(icl_plane_ctl_flip(0) == 0, "no flip");
	CHECK(icl_plane_ctl_flip(DRM_MODE_REFLECT_X) == PLANE_CTL_FLIP_HORIZONTAL, "flip x");
	CHECK(icl_plane_ctl_flip(DRM_MODE_REFLECT_Y) == 0, "flip y unsupported");
}

static void test_aligned_offset(void)
{
	uint32_t x, y, off;

	/* Y tiled, 4 bytes per pixel, 32 tiles per row (4096-byte pitch):
	 * tiles of 32x32 pixels */
	x = 100;
	y = 70;
	off = compute_aligned_offset_tiled(&x, &y, 4, 4096, 0, 4096);
	CHECK(off == (2 * 32 + 3) * 4096u, "tile offset %u", off);
	CHECK(x == 4 && y == 6, "in-tile x/y %u/%u", x, y);

	/* a 1 MiB alignment moves every tile back into x/y */
	x = 100;
	y = 70;
	off = compute_aligned_offset_tiled(&x, &y, 4, 4096, 0, 1024 * 1024);
	CHECK(off == 0, "aligned offset %u", off);
	CHECK(x == 100 && y == 70, "x/y back %u/%u", x, y);

	/* rotated: the pitch counts tile heights, the tiles are swapped */
	x = 40;
	y = 300;
	off = compute_aligned_offset_tiled(&x, &y, 4, 34 * 32, 1, 4096);
	CHECK(off == (300 / 32 * 34 + 40 / 32) * 4096u, "rotated tile offset %u", off);
	CHECK(x == 40 % 32 && y == 300 % 32, "rotated in-tile x/y %u/%u", x, y);
	CHECK(warnings == 0, "no warnings");
}

static void test_rotated_view(void)
{
	struct drm_framebuffer fb = { 1920, 1080, 7680, 0, 4 };
	struct intel_rotated_view_info info;
	uint32_t vx, vy, stride;

	rotated_view_layout(&fb, &info, &vx, &vy, &stride);
	CHECK(info.offset == 0, "offset %u", info.offset);
	CHECK(info.src_stride == 60, "src stride %u", info.src_stride);
	CHECK(info.width == 60 && info.height == 34, "tiles %ux%u", info.width, info.height);
	CHECK(info.dst_stride == 34, "dst stride %u", info.dst_stride);
	/* the 8 rows of padding of the last tile row end up on the left */
	CHECK(vx == 34 * 32 - 1080 && vy == 0, "view x/y %u/%u", vx, vy);
	CHECK(stride == 34 * 32, "mapping stride %u", stride);

	/* a framebuffer two tile rows into its object */
	struct drm_framebuffer fb2 = { 1920, 1080, 7680, 2 * 60 * 4096, 4 };
	rotated_view_layout(&fb2, &info, &vx, &vy, &stride);
	CHECK(info.offset == 120, "offset 2 rows %u", info.offset);
	CHECK(info.width == 60 && info.height == 34, "tiles 2 rows %ux%u", info.width, info.height);
	CHECK(warnings == 0, "no warnings");
}

/* The plane check's clipping: rotate the source into the shown
 * orientation, clip, rotate back. */
static int clip_plane(struct drm_rect *src, struct drm_rect *dst, int fbw, int fbh,
		      unsigned int rotation, int mw, int mh)
{
	struct drm_rect clip = { 0, 0, mw, mh };
	int visible;

	drm_rect_rotate(src, fbw << 16, fbh << 16, rotation);
	visible = drm_rect_clip_scaled(src, dst, &clip);
	drm_rect_rotate_inv(src, fbw << 16, fbh << 16, rotation);
	return visible;
}

static void test_clipping(void)
{
	struct drm_rect src, dst;

	/* unrotated, 100 pixels off the left edge: the source loses its
	 * left 100 columns */
	drm_rect_init(&src, 0, 0, 1920 << 16, 1080 << 16);
	drm_rect_init(&dst, -100, 0, 1920, 1080);
	CHECK(clip_plane(&src, &dst, 1920, 1080, DRM_MODE_ROTATE_0, 1920, 1080), "visible");
	CHECK(src.x1 == 100 << 16 && drm_rect_width(&src) == 1820 << 16, "src %d+%d",
	      src.x1 >> 16, drm_rect_width(&src) >> 16);
	CHECK(dst.x1 == 0 && drm_rect_width(&dst) == 1820, "dst %d+%d", dst.x1,
	      drm_rect_width(&dst));

	/* 180 degrees, the same position: the left of the screen shows the
	 * right of the framebuffer, so the source loses its RIGHT columns */
	drm_rect_init(&src, 0, 0, 1920 << 16, 1080 << 16);
	drm_rect_init(&dst, -100, 0, 1920, 1080);
	CHECK(clip_plane(&src, &dst, 1920, 1080, DRM_MODE_ROTATE_180, 1920, 1080), "visible 180");
	CHECK(src.x1 == 0 && drm_rect_width(&src) == 1820 << 16, "src 180 %d+%d",
	      src.x1 >> 16, drm_rect_width(&src) >> 16);

	/* reflect-x, the same: also the right columns go */
	drm_rect_init(&src, 0, 0, 1920 << 16, 1080 << 16);
	drm_rect_init(&dst, -100, 0, 1920, 1080);
	CHECK(clip_plane(&src, &dst, 1920, 1080, DRM_MODE_ROTATE_0 | DRM_MODE_REFLECT_X,
			 1920, 1080), "visible reflect");
	CHECK(src.x1 == 0 && drm_rect_width(&src) == 1820 << 16, "src reflect %d+%d",
	      src.x1 >> 16, drm_rect_width(&src) >> 16);

	/* wholly off the pipe: not visible */
	drm_rect_init(&src, 0, 0, 64 << 16, 64 << 16);
	drm_rect_init(&dst, 2000, 0, 64, 64);
	CHECK(!clip_plane(&src, &dst, 64, 64, DRM_MODE_ROTATE_0, 1920, 1080), "invisible");
}

int main(void)
{
	test_ctl_bits();
	test_aligned_offset();
	test_rotated_view();
	test_clipping();
	printf("intel_plane: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
