/*
 * Tests for the display-manager core's rectangle helpers
 * (kernel/dev/gpu/drm/drm_rect.c): intersection, scaled clipping with its
 * rounding toward 1.0, scale factors and their limits, rotation and its
 * inverse, and the log line of drm_rect_debug_print.  See test-drm-rect.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 * SPDX-License-Identifier for the portions derived from Maíra Canal's code: GPL-2.0
 * Portions Copyright (c) 2022 Maíra Canal <mairacanal@riseup.net>
 */

/* The kernel's fixed-width types come first and stand in for stdint.h,
 * whose typedefs differ in spelling (long vs long long) on the host. */
#include <kernel/dev/gpu/drm_rect.h>
#include <kernel/uapi/drm/drm_mode.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define INT_MAX 0x7fffffff

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- what drm_rect.c needs from the kernel ------------------------------- */

static char logbuf[512];
static int warnings;

int kprintf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(logbuf, sizeof(logbuf), fmt, ap);
	va_end(ap);
	if (!strncmp(logbuf, "WARNING", 7))
		warnings++;
	return n;
}

void panic(const char *fmt, ...)
{
	(void)fmt;
	printf("panic\n");
	__builtin_trap();
}

/* ---- helpers ------------------------------------------------------------- */

static int rect_is(const struct drm_rect *r, int x1, int y1, int x2, int y2)
{
	return r->x1 == x1 && r->y1 == y1 && r->x2 == x2 && r->y2 == y2;
}

/* position and size equal (what the vectors compare) */
static int rect_same(const struct drm_rect *r, const struct drm_rect *e)
{
	return r->x1 == e->x1 && r->y1 == e->y1 &&
		drm_rect_width(r) == drm_rect_width(e) &&
		drm_rect_height(r) == drm_rect_height(e);
}

#define R(r) (r).x1, (r).y1, (r).x2, (r).y2

/* ---- scaled clipping ----------------------------------------------------- */

static void test_clip_scaled_div_by_zero(void)
{
	struct drm_rect src, dst, clip;
	bool visible;

	/* no division by zero when dst is empty and misses the clip */
	drm_rect_init(&src, 0, 0, 0, 0);
	drm_rect_init(&dst, 0, 0, 0, 0);
	drm_rect_init(&clip, 1, 1, 1, 1);
	visible = drm_rect_clip_scaled(&src, &dst, &clip);
	CHECK(!visible, "div0 a: dst should not be visible");
	CHECK(!drm_rect_visible(&src), "div0 a: src should not be visible");

	drm_rect_init(&src, 0, 0, 0, 0);
	drm_rect_init(&dst, 3, 3, 0, 0);
	drm_rect_init(&clip, 1, 1, 1, 1);
	visible = drm_rect_clip_scaled(&src, &dst, &clip);
	CHECK(!visible, "div0 b: dst should not be visible");
	CHECK(!drm_rect_visible(&src), "div0 b: src should not be visible");
}

struct clip_case {
	const char *name;
	struct drm_rect src, dst, clip;
	struct drm_rect src_exp, dst_exp;
};

static const struct clip_case clip_cases[] = {
	/* not clipped */
	{ "1:1", DRM_RECT_INIT(0, 0, 1 << 16, 1 << 16), DRM_RECT_INIT(0, 0, 1, 1),
	  DRM_RECT_INIT(0, 0, 1, 1),
	  { 0, 0, 1 << 16, 1 << 16 }, { 0, 0, 1, 1 } },
	{ "2:1", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 1, 1),
	  DRM_RECT_INIT(0, 0, 1, 1),
	  { 0, 0, 2 << 16, 2 << 16 }, { 0, 0, 1, 1 } },
	{ "1:2", DRM_RECT_INIT(0, 0, 1 << 16, 1 << 16), DRM_RECT_INIT(0, 0, 2, 2),
	  DRM_RECT_INIT(0, 0, 2, 2),
	  { 0, 0, 1 << 16, 1 << 16 }, { 0, 0, 2, 2 } },
	/* clipped */
	{ "1:1 top/left", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 2, 2),
	  DRM_RECT_INIT(0, 0, 1, 1),
	  { 0, 0, 1 << 16, 1 << 16 }, { 0, 0, 1, 1 } },
	{ "1:1 bottom/right", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 2, 2),
	  DRM_RECT_INIT(1, 1, 1, 1),
	  { 1 << 16, 1 << 16, 2 << 16, 2 << 16 }, { 1, 1, 2, 2 } },
	{ "2:1 top/left", DRM_RECT_INIT(0, 0, 4 << 16, 4 << 16), DRM_RECT_INIT(0, 0, 2, 2),
	  DRM_RECT_INIT(0, 0, 1, 1),
	  { 0, 0, 2 << 16, 2 << 16 }, { 0, 0, 1, 1 } },
	{ "2:1 bottom/right", DRM_RECT_INIT(0, 0, 4 << 16, 4 << 16), DRM_RECT_INIT(0, 0, 2, 2),
	  DRM_RECT_INIT(1, 1, 1, 1),
	  { 2 << 16, 2 << 16, 4 << 16, 4 << 16 }, { 1, 1, 2, 2 } },
	{ "1:2 top/left", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 4, 4),
	  DRM_RECT_INIT(0, 0, 2, 2),
	  { 0, 0, 1 << 16, 1 << 16 }, { 0, 0, 2, 2 } },
	{ "1:2 bottom/right", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 4, 4),
	  DRM_RECT_INIT(2, 2, 2, 2),
	  { 1 << 16, 1 << 16, 2 << 16, 2 << 16 }, { 2, 2, 4, 4 } },
};

static void test_clip_scaled(void)
{
	for (unsigned i = 0; i < sizeof(clip_cases) / sizeof(clip_cases[0]); i++) {
		const struct clip_case *c = &clip_cases[i];
		struct drm_rect src = c->src, dst = c->dst;
		bool visible = drm_rect_clip_scaled(&src, &dst, &c->clip);

		CHECK(rect_is(&src, R(c->src_exp)), "clip %s: src %d,%d,%d,%d", c->name, R(src));
		CHECK(rect_is(&dst, R(c->dst_exp)), "clip %s: dst %d,%d,%d,%d", c->name, R(dst));
		CHECK(visible, "clip %s: dst should be visible", c->name);
		CHECK(drm_rect_visible(&src), "clip %s: src should be visible", c->name);
	}
}

static void test_clip_scaled_signed_vs_unsigned(void)
{
	struct drm_rect src, dst, clip;
	bool visible;

	/* a clip beyond dst must not give a negative (huge unsigned) src */
	drm_rect_init(&src, 0, 0, INT_MAX, INT_MAX);
	drm_rect_init(&dst, 0, 0, 2, 2);
	drm_rect_init(&clip, 3, 3, 1, 1);
	visible = drm_rect_clip_scaled(&src, &dst, &clip);
	CHECK(!visible, "signed: dst should not be visible");
	CHECK(!drm_rect_visible(&src), "signed: src should not be visible");
}

/* ---- intersection -------------------------------------------------------- */

struct intersect_case {
	const char *name;
	struct drm_rect r1, r2;
	bool visible;
	struct drm_rect expected;
};

static const struct intersect_case intersect_cases[] = {
	{ "top-left x bottom-right", DRM_RECT_INIT(1, 1, 2, 2), DRM_RECT_INIT(0, 0, 2, 2), true, DRM_RECT_INIT(1, 1, 1, 1) },
	{ "top-right x bottom-left", DRM_RECT_INIT(0, 0, 2, 2), DRM_RECT_INIT(1, -1, 2, 2), true, DRM_RECT_INIT(1, 0, 1, 1) },
	{ "bottom-left x top-right", DRM_RECT_INIT(1, -1, 2, 2), DRM_RECT_INIT(0, 0, 2, 2), true, DRM_RECT_INIT(1, 0, 1, 1) },
	{ "bottom-right x top-left", DRM_RECT_INIT(0, 0, 2, 2), DRM_RECT_INIT(1, 1, 2, 2), true, DRM_RECT_INIT(1, 1, 1, 1) },
	{ "right x left", DRM_RECT_INIT(0, 0, 2, 1), DRM_RECT_INIT(1, 0, 3, 1), true, DRM_RECT_INIT(1, 0, 1, 1) },
	{ "left x right", DRM_RECT_INIT(1, 0, 3, 1), DRM_RECT_INIT(0, 0, 2, 1), true, DRM_RECT_INIT(1, 0, 1, 1) },
	{ "up x bottom", DRM_RECT_INIT(0, 0, 1, 2), DRM_RECT_INIT(0, -1, 1, 3), true, DRM_RECT_INIT(0, 0, 1, 2) },
	{ "bottom x up", DRM_RECT_INIT(0, -1, 1, 3), DRM_RECT_INIT(0, 0, 1, 2), true, DRM_RECT_INIT(0, 0, 1, 2) },
	{ "touching corner", DRM_RECT_INIT(0, 0, 1, 1), DRM_RECT_INIT(1, 1, 2, 2), false, DRM_RECT_INIT(1, 1, 0, 0) },
	{ "touching side", DRM_RECT_INIT(0, 0, 1, 1), DRM_RECT_INIT(1, 0, 1, 1), false, DRM_RECT_INIT(1, 0, 0, 1) },
	{ "equal rects", DRM_RECT_INIT(0, 0, 2, 2), DRM_RECT_INIT(0, 0, 2, 2), true, DRM_RECT_INIT(0, 0, 2, 2) },
	{ "inside another", DRM_RECT_INIT(0, 0, 2, 2), DRM_RECT_INIT(1, 1, 1, 1), true, DRM_RECT_INIT(1, 1, 1, 1) },
	{ "far away", DRM_RECT_INIT(0, 0, 1, 1), DRM_RECT_INIT(3, 6, 1, 1), false, DRM_RECT_INIT(3, 6, -2, -5) },
	{ "points intersecting", DRM_RECT_INIT(5, 10, 0, 0), DRM_RECT_INIT(5, 10, 0, 0), false, DRM_RECT_INIT(5, 10, 0, 0) },
	{ "points not intersecting", DRM_RECT_INIT(0, 0, 0, 0), DRM_RECT_INIT(5, 10, 0, 0), false, DRM_RECT_INIT(5, 10, -5, -10) },
};

static void test_intersect(void)
{
	for (unsigned i = 0; i < sizeof(intersect_cases) / sizeof(intersect_cases[0]); i++) {
		const struct intersect_case *c = &intersect_cases[i];
		struct drm_rect r = c->r1;
		bool visible = drm_rect_intersect(&r, &c->r2);

		CHECK(visible == c->visible, "intersect %s: visible %d", c->name, visible);
		CHECK(rect_same(&r, &c->expected), "intersect %s: got %d,%d,%d,%d", c->name, R(r));
	}
}

/* ---- scale factors ------------------------------------------------------- */

struct scale_case {
	const char *name;
	struct drm_rect src, dst;
	int min_range, max_range;
	int expected;
};

static const struct scale_case scale_cases[] = {
	{ "normal use", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 1 << 16, 1 << 16), 0, INT_MAX, 2 },
	{ "out of max range", DRM_RECT_INIT(0, 0, 10 << 16, 10 << 16), DRM_RECT_INIT(0, 0, 1 << 16, 1 << 16), 3, 5, -ERANGE },
	{ "out of min range", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 1 << 16, 1 << 16), 3, 5, -ERANGE },
	{ "zero dst", DRM_RECT_INIT(0, 0, 2 << 16, 2 << 16), DRM_RECT_INIT(0, 0, 0 << 16, 0 << 16), 0, INT_MAX, 0 },
	{ "negative src", DRM_RECT_INIT(0, 0, -(1 << 16), -(1 << 16)), DRM_RECT_INIT(0, 0, 1 << 16, 1 << 16), 0, INT_MAX, -EINVAL },
	{ "negative dst", DRM_RECT_INIT(0, 0, 1 << 16, 1 << 16), DRM_RECT_INIT(0, 0, -(1 << 16), -(1 << 16)), 0, INT_MAX, -EINVAL },
	/* the rounding: below 1.0 down, above 1.0 up */
	{ "upscale rounds down", DRM_RECT_INIT(0, 0, 0x3ffff, 0x3ffff), DRM_RECT_INIT(0, 0, 4, 4), 0, INT_MAX, 0xffff },
	{ "downscale rounds up", DRM_RECT_INIT(0, 0, 0x40001, 0x40001), DRM_RECT_INIT(0, 0, 4, 4), 0, INT_MAX, 0x10001 },
};

static void test_calc_scale(void)
{
	for (unsigned i = 0; i < sizeof(scale_cases) / sizeof(scale_cases[0]); i++) {
		const struct scale_case *c = &scale_cases[i];
		int expected_warnings = c->expected == -EINVAL;
		int w0 = warnings;
		int h = drm_rect_calc_hscale(&c->src, &c->dst, c->min_range, c->max_range);

		CHECK(h == c->expected, "hscale %s: %d, want %d", c->name, h, c->expected);
		CHECK(warnings - w0 == expected_warnings, "hscale %s: %d warnings", c->name, warnings - w0);
		w0 = warnings;
		int v = drm_rect_calc_vscale(&c->src, &c->dst, c->min_range, c->max_range);
		CHECK(v == c->expected, "vscale %s: %d, want %d", c->name, v, c->expected);
		CHECK(warnings - w0 == expected_warnings, "vscale %s: %d warnings", c->name, warnings - w0);
	}
}

/* ---- rotation ------------------------------------------------------------ */

struct rotate_case {
	const char *name;
	unsigned int rotation;
	struct drm_rect rect;
	int width, height;
	struct drm_rect expected;
};

static const struct rotate_case rotate_cases[] = {
	{ "reflect-x", DRM_MODE_REFLECT_X, DRM_RECT_INIT(0, 0, 5, 5), 5, 10, DRM_RECT_INIT(0, 0, 5, 5) },
	{ "reflect-y", DRM_MODE_REFLECT_Y, DRM_RECT_INIT(2, 0, 5, 5), 5, 10, DRM_RECT_INIT(2, 5, 5, 5) },
	{ "rotate-0", DRM_MODE_ROTATE_0, DRM_RECT_INIT(0, 2, 5, 5), 5, 10, DRM_RECT_INIT(0, 2, 5, 5) },
	{ "rotate-90", DRM_MODE_ROTATE_90, DRM_RECT_INIT(0, 0, 5, 10), 5, 10, DRM_RECT_INIT(0, 0, 10, 5) },
	{ "rotate-180", DRM_MODE_ROTATE_180, DRM_RECT_INIT(11, 3, 5, 10), 5, 10, DRM_RECT_INIT(-11, -3, 5, 10) },
	{ "rotate-270", DRM_MODE_ROTATE_270, DRM_RECT_INIT(6, 3, 5, 10), 5, 10, DRM_RECT_INIT(-3, 6, 10, 5) },
};

static void test_rotate(void)
{
	for (unsigned i = 0; i < sizeof(rotate_cases) / sizeof(rotate_cases[0]); i++) {
		const struct rotate_case *c = &rotate_cases[i];
		struct drm_rect r = c->rect;

		drm_rect_rotate(&r, c->width, c->height, c->rotation);
		CHECK(rect_same(&r, &c->expected), "rotate %s: got %d,%d,%d,%d", c->name, R(r));

		r = c->expected;
		drm_rect_rotate_inv(&r, c->width, c->height, c->rotation);
		CHECK(rect_same(&r, &c->rect), "rotate_inv %s: got %d,%d,%d,%d", c->name, R(r));
	}

	/* every rotation with every reflection undoes itself */
	for (unsigned rot = 0; rot < 4; rot++) {
		for (unsigned refl = 0; refl < 4; refl++) {
			unsigned int rotation = (1u << rot) | (refl << 4);
			struct drm_rect r = DRM_RECT_INIT(3, 7, 11, 2);

			drm_rect_rotate(&r, 40, 30, rotation);
			drm_rect_rotate_inv(&r, 40, 30, rotation);
			CHECK(rect_is(&r, 3, 7, 14, 9), "round trip 0x%x: got %d,%d,%d,%d", rotation, R(r));
		}
	}
}

/* ---- the log line -------------------------------------------------------- */

static void test_debug_print(void)
{
	static const struct drm_rect rs[] = {
		DRM_RECT_INIT(10, 20, 640, 480),
		DRM_RECT_INIT(-3, -7, 5, 9),
		DRM_RECT_INIT(0x18000, 0x4000, 0x20000, 0x10001),
		DRM_RECT_INIT(-(5 << 16), 3 << 16, 1 << 16, 1 << 16),
	};
	char want[256];

	for (unsigned i = 0; i < sizeof(rs) / sizeof(rs[0]); i++) {
		const struct drm_rect *r = &rs[i];

		drm_rect_debug_print("p: ", r, false);
		snprintf(want, sizeof(want), "[drm] p: " DRM_RECT_FMT "\n", DRM_RECT_ARG(r));
		CHECK(!strcmp(logbuf, want), "debug_print int: \"%s\" want \"%s\"", logbuf, want);

		drm_rect_debug_print("p: ", r, true);
		snprintf(want, sizeof(want), "[drm] p: " DRM_RECT_FP_FMT "\n", DRM_RECT_FP_ARG(r));
		CHECK(!strcmp(logbuf, want), "debug_print fp: \"%s\" want \"%s\"", logbuf, want);
	}
}

int main(void)
{
	test_clip_scaled_div_by_zero();
	test_clip_scaled();
	test_clip_scaled_signed_vs_unsigned();
	test_intersect();
	test_calc_scale();
	test_rotate();
	test_debug_print();

	if (fails) {
		printf("drm_rect: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("drm_rect: all %d checks passed\n", checks);
	return 0;
}
