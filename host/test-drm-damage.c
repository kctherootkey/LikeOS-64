/*
 * Tests for the display-manager core's damage iterator
 * (kernel/dev/gpu/drm/drm_damage_helper.c) and plane-state checks
 * (kernel/dev/gpu/drm/drm_atomic_helper.c): clipping damage to the plane
 * source and the full-update fallbacks; clipping a plane to its crtc with
 * scaling limits, positioning and rotation; dropping damage on a mode set;
 * the primary-plane check; the committed plane as an old state.  See
 * test-drm-damage.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 * SPDX-License-Identifier for the portions derived from Maíra Canal's code: GPL-2.0
 * Portions Copyright (c) 2022 Maíra Canal <mairacanal@riseup.net>
 */

/* The kernel's fixed-width types come first and stand in for stdint.h,
 * whose typedefs differ in spelling (long vs long long) on the host. */
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_damage_helper.h>
#include <kernel/dev/gpu/drm_atomic_helper.h>
#include <kernel/dev/gpu/drm_rect.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- what the helpers need from the kernel ------------------------------- */

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

void mm_memset(void *dest, int val, size_t len)
{
	memset(dest, val, len);
}

static struct drm_device *fb_dev;
static struct drm_framebuffer *fb_table;
static unsigned fb_count;

struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t id)
{
	if (dev != fb_dev || !id)
		return NULL;
	for (unsigned i = 0; i < fb_count; i++)
		if (fb_table[i].id == id)
			return &fb_table[i];
	return NULL;
}

/* ---- damage iterator ----------------------------------------------------- */

static struct drm_framebuffer mock_fb;
static struct drm_plane mock_plane;
static struct drm_plane_state state, old_state;

static void damage_init(void)
{
	memset(&mock_fb, 0, sizeof(mock_fb));
	memset(&mock_plane, 0, sizeof(mock_plane));
	memset(&state, 0, sizeof(state));
	memset(&old_state, 0, sizeof(old_state));
	mock_fb.width = 2048;
	mock_fb.height = 2048;
	state.crtc = 0;
	state.fb = &mock_fb;
	state.visible = true;
	old_state.plane = &mock_plane;
	state.plane = &mock_plane;
	old_state.crtc = 0;
}

static void set_plane_src(struct drm_plane_state *s, int x1, int y1, int x2, int y2)
{
	s->src_x = (uint32_t)x1;
	s->src_y = (uint32_t)y1;
	s->src_w = (uint32_t)(x2 - x1);
	s->src_h = (uint32_t)(y2 - y1);
	s->src.x1 = x1;
	s->src.y1 = y1;
	s->src.x2 = x2;
	s->src.y2 = y2;
}

static void set_damage_clip(struct drm_mode_rect *r, int x1, int y1, int x2, int y2)
{
	r->x1 = x1;
	r->y1 = y1;
	r->x2 = x2;
	r->y2 = y2;
}

static void set_plane_damage(struct drm_plane_state *s, struct drm_mode_rect *r, unsigned n)
{
	s->fb_damage_clips = 1;
	s->damage_clips = r;
	s->num_damage_clips = n;
}

static void check_damage_clip(const char *name, const struct drm_rect *r,
			      int x1, int y1, int x2, int y2)
{
	/* damage never lies outside the source rounded out to whole pixels */
	int src_x1 = state.src.x1 >> 16;
	int src_y1 = state.src.y1 >> 16;
	int src_x2 = (state.src.x2 >> 16) + !!(state.src.x2 & 0xFFFF);
	int src_y2 = (state.src.y2 >> 16) + !!(state.src.y2 & 0xFFFF);

	CHECK(x1 < x2 && y1 < y2, "%s: expected clip without area", name);
	CHECK(!(x1 < src_x1 || y1 < src_y1 || x2 > src_x2 || y2 > src_y2),
	      "%s: expected damage outside the rounded source", name);
	CHECK(r->x1 == x1 && r->y1 == y1 && r->x2 == x2 && r->y2 == y2,
	      "%s: damage = %d %d %d %d, want = %d %d %d %d", name,
	      r->x1, r->y1, r->x2, r->y2, x1, y1, x2, y2);
}

static unsigned run_iter(struct drm_rect *last)
{
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;
	unsigned hits = 0;

	drm_atomic_helper_damage_iter_init(&iter, &old_state, &state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		*last = clip;
		hits++;
	}
	return hits;
}

#define FRAC(a) (a), (a), (a) + (1024 << 16), (a) + (768 << 16)

static void test_damage(void)
{
	struct drm_rect clip;
	struct drm_mode_rect damage[2];
	unsigned n;

	/* no_damage: the plane source as damage */
	damage_init();
	set_plane_src(&old_state, 0, 0, 2048 << 16, 2048 << 16);
	set_plane_src(&state, 0, 0, 2048 << 16, 2048 << 16);
	n = run_iter(&clip);
	CHECK(n == 1, "no_damage: %u hits", n);
	check_damage_clip("no_damage", &clip, 0, 0, 2048, 2048);

	/* no_damage_fractional_src: the source rounded out */
	damage_init();
	set_plane_src(&old_state, FRAC(0x3fffe));
	set_plane_src(&state, FRAC(0x3fffe));
	n = run_iter(&clip);
	CHECK(n == 1, "no_damage_fractional_src: %u hits", n);
	check_damage_clip("no_damage_fractional_src", &clip, 3, 3, 1028, 772);

	/* no_damage_src_moved */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 10 << 16, 10 << 16, (10 + 1024) << 16, (10 + 768) << 16);
	n = run_iter(&clip);
	CHECK(n == 1, "no_damage_src_moved: %u hits", n);
	check_damage_clip("no_damage_src_moved", &clip, 10, 10, 1034, 778);

	/* no_damage_fractional_src_moved */
	damage_init();
	set_plane_src(&old_state, FRAC(0x3fffe));
	set_plane_src(&state, FRAC(0x40002));
	n = run_iter(&clip);
	CHECK(n == 1, "no_damage_fractional_src_moved: %u hits", n);
	check_damage_clip("no_damage_fractional_src_moved", &clip, 4, 4, 1029, 773);

	/* no_damage_not_visible / no_crtc / no_fb: nothing at all */
	damage_init();
	state.visible = false;
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	n = run_iter(&clip);
	CHECK(n == 0, "no_damage_not_visible: %u hits", n);

	damage_init();
	state.crtc = -1;
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	n = run_iter(&clip);
	CHECK(n == 0, "no_damage_no_crtc: %u hits", n);

	damage_init();
	state.fb = NULL;
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	n = run_iter(&clip);
	CHECK(n == 0, "no_damage_no_fb: %u hits", n);

	/* simple_damage: damage = the source */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_damage_clip(&damage[0], 0, 0, 1024, 768);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 1, "simple_damage: %u hits", n);
	check_damage_clip("simple_damage", &clip, 0, 0, 1024, 768);

	/* single_damage */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_damage_clip(&damage[0], 256, 192, 768, 576);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 1, "single_damage: %u hits", n);
	check_damage_clip("single_damage", &clip, 256, 192, 768, 576);

	/* single_damage_intersect_src */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_damage_clip(&damage[0], 256, 192, 1360, 768);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 1, "single_damage_intersect_src: %u hits", n);
	check_damage_clip("single_damage_intersect_src", &clip, 256, 192, 1024, 768);

	/* single_damage_outside_src */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_damage_clip(&damage[0], 1360, 1360, 1380, 1380);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 0, "single_damage_outside_src: %u hits", n);

	/* single_damage_fractional_src */
	damage_init();
	set_plane_src(&old_state, FRAC(0x40002));
	set_plane_src(&state, FRAC(0x40002));
	set_damage_clip(&damage[0], 10, 10, 256, 330);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 1, "single_damage_fractional_src: %u hits", n);
	check_damage_clip("single_damage_fractional_src", &clip, 10, 10, 256, 330);

	/* single_damage_intersect_fractional_src */
	damage_init();
	set_plane_src(&old_state, FRAC(0x40002));
	set_plane_src(&state, FRAC(0x40002));
	set_damage_clip(&damage[0], 10, 1, 1360, 330);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 1, "single_damage_intersect_fractional_src: %u hits", n);
	check_damage_clip("single_damage_intersect_fractional_src", &clip, 10, 4, 1029, 330);

	/* single_damage_outside_fractional_src */
	damage_init();
	set_plane_src(&old_state, FRAC(0x40002));
	set_plane_src(&state, FRAC(0x40002));
	set_damage_clip(&damage[0], 1360, 1360, 1380, 1380);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 0, "single_damage_outside_fractional_src: %u hits", n);

	/* single_damage_src_moved: the whole source */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 10 << 16, 10 << 16, (10 + 1024) << 16, (10 + 768) << 16);
	set_damage_clip(&damage[0], 20, 30, 256, 256);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 1, "single_damage_src_moved: %u hits", n);
	check_damage_clip("single_damage_src_moved", &clip, 10, 10, 1034, 778);

	/* single_damage_fractional_src_moved */
	damage_init();
	set_plane_src(&old_state, FRAC(0x3fffe));
	set_plane_src(&state, FRAC(0x40002));
	set_damage_clip(&damage[0], 20, 30, 1360, 256);
	set_plane_damage(&state, damage, 1);
	n = run_iter(&clip);
	CHECK(n == 1, "single_damage_fractional_src_moved: %u hits", n);
	check_damage_clip("single_damage_fractional_src_moved", &clip, 4, 4, 1029, 773);

	/* damage: two clips, in order */
	{
		struct drm_atomic_helper_damage_iter iter;
		unsigned hits = 0;

		damage_init();
		set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
		set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
		set_damage_clip(&damage[0], 20, 30, 200, 180);
		set_damage_clip(&damage[1], 240, 200, 280, 250);
		set_plane_damage(&state, damage, 2);
		drm_atomic_helper_damage_iter_init(&iter, &old_state, &state);
		drm_atomic_for_each_plane_damage(&iter, &clip) {
			if (hits == 0)
				check_damage_clip("damage[0]", &clip, 20, 30, 200, 180);
			if (hits == 1)
				check_damage_clip("damage[1]", &clip, 240, 200, 280, 250);
			hits++;
		}
		CHECK(hits == 2, "damage: %u hits", hits);
	}

	/* damage_one_intersect */
	{
		struct drm_atomic_helper_damage_iter iter;
		unsigned hits = 0;

		damage_init();
		set_plane_src(&old_state, FRAC(0x40002));
		set_plane_src(&state, FRAC(0x40002));
		set_damage_clip(&damage[0], 20, 30, 200, 180);
		set_damage_clip(&damage[1], 2, 2, 1360, 1360);
		set_plane_damage(&state, damage, 2);
		drm_atomic_helper_damage_iter_init(&iter, &old_state, &state);
		drm_atomic_for_each_plane_damage(&iter, &clip) {
			if (hits == 0)
				check_damage_clip("one_intersect[0]", &clip, 20, 30, 200, 180);
			if (hits == 1)
				check_damage_clip("one_intersect[1]", &clip, 4, 4, 1029, 773);
			hits++;
		}
		CHECK(hits == 2, "damage_one_intersect: %u hits", hits);
	}

	/* damage_one_outside */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_damage_clip(&damage[0], 1360, 1360, 1380, 1380);
	set_damage_clip(&damage[1], 240, 200, 280, 250);
	set_plane_damage(&state, damage, 2);
	n = run_iter(&clip);
	CHECK(n == 1, "damage_one_outside: %u hits", n);
	check_damage_clip("damage_one_outside", &clip, 240, 200, 280, 250);

	/* damage_src_moved */
	damage_init();
	set_plane_src(&old_state, FRAC(0x40002));
	set_plane_src(&state, FRAC(0x3fffe));
	set_damage_clip(&damage[0], 1360, 1360, 1380, 1380);
	set_damage_clip(&damage[1], 240, 200, 280, 250);
	set_plane_damage(&state, damage, 2);
	n = run_iter(&clip);
	CHECK(n == 1, "damage_src_moved: %u hits", n);
	check_damage_clip("damage_src_moved", &clip, 3, 3, 1028, 772);

	/* damage_not_visible */
	damage_init();
	state.visible = false;
	set_plane_src(&old_state, FRAC(0x40002));
	set_plane_src(&state, FRAC(0x3fffe));
	set_damage_clip(&damage[0], 1360, 1360, 1380, 1380);
	set_damage_clip(&damage[1], 240, 200, 280, 250);
	set_plane_damage(&state, damage, 2);
	n = run_iter(&clip);
	CHECK(n == 0, "damage_not_visible: %u hits", n);

	/* LikeOS additions: ignore_damage_clips and no old state give the
	 * whole source; an empty clip array is "nothing damaged". */
	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_damage_clip(&damage[0], 20, 30, 200, 180);
	set_plane_damage(&state, damage, 1);
	state.ignore_damage_clips = 1;
	n = run_iter(&clip);
	CHECK(n == 1, "ignore_damage_clips: %u hits", n);
	check_damage_clip("ignore_damage_clips", &clip, 0, 0, 1024, 768);

	{
		struct drm_atomic_helper_damage_iter iter;

		damage_init();
		set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
		set_damage_clip(&damage[0], 20, 30, 200, 180);
		set_plane_damage(&state, damage, 1);
		drm_atomic_helper_damage_iter_init(&iter, NULL, &state);
		CHECK(drm_atomic_helper_damage_iter_next(&iter, &clip), "no old state: no damage");
		check_damage_clip("no old state", &clip, 0, 0, 1024, 768);
		CHECK(!drm_atomic_helper_damage_iter_next(&iter, &clip), "no old state: second hit");
	}

	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_damage(&state, damage, 0);
	n = run_iter(&clip);
	CHECK(n == 0, "empty clip array: %u hits", n);
}

static void test_damage_merged(void)
{
	struct drm_mode_rect damage[3];
	struct drm_rect r;

	damage_init();
	set_plane_src(&old_state, 0, 0, 1024 << 16, 768 << 16);
	set_plane_src(&state, 0, 0, 1024 << 16, 768 << 16);
	set_damage_clip(&damage[0], 20, 30, 200, 180);
	set_damage_clip(&damage[1], 1360, 1360, 1380, 1380); /* outside */
	set_damage_clip(&damage[2], 240, 10, 280, 250);
	set_plane_damage(&state, damage, 3);
	CHECK(drm_atomic_helper_damage_merged(&old_state, &state, &r), "merged: none");
	CHECK(r.x1 == 20 && r.y1 == 10 && r.x2 == 280 && r.y2 == 250,
	      "merged: %d %d %d %d", r.x1, r.y1, r.x2, r.y2);

	state.visible = false;
	CHECK(!drm_atomic_helper_damage_merged(&old_state, &state, &r), "merged: invisible plane damaged");
}

static void test_damage_accessors(void)
{
	struct drm_mode_rect damage[2];

	damage_init();
	CHECK(drm_plane_get_damage_clips_count(NULL) == 0, "count(NULL)");
	CHECK(drm_plane_get_damage_clips(NULL) == NULL, "clips(NULL)");
	CHECK(drm_plane_get_damage_clips_count(&state) == 0, "count without clips");
	state.num_damage_clips = 5; /* stale count without an array */
	CHECK(drm_plane_get_damage_clips_count(&state) == 0, "count without array");
	set_plane_damage(&state, damage, 2);
	CHECK(drm_plane_get_damage_clips_count(&state) == 2, "count");
	CHECK(drm_plane_get_damage_clips(&state) == damage, "clips");
}

/* ---- dropping damage on a mode set ---------------------------------------- */

static void test_check_plane_damage(void)
{
	static struct drm_atomic_state st;
	static struct drm_crtc crtc0;
	struct drm_mode_rect damage[1];

	memset(&st, 0, sizeof(st));
	crtc0.index = 0;
	st.crtcs[0].crtc = &crtc0;

	damage_init();
	set_plane_damage(&state, damage, 1);
	drm_atomic_helper_check_plane_damage(&st, &state);
	CHECK(state.damage_clips == damage && state.num_damage_clips == 1 &&
	      state.fb_damage_clips == 1, "no mode set: damage dropped");

	st.crtcs[0].mode_changed = 1;
	drm_atomic_helper_check_plane_damage(&st, &state);
	CHECK(!state.damage_clips && !state.num_damage_clips && !state.fb_damage_clips,
	      "mode set: damage kept");

	/* a plane that is off has no crtc to ask */
	damage_init();
	state.crtc = -1;
	set_plane_damage(&state, damage, 1);
	drm_atomic_helper_check_plane_damage(&st, &state);
	CHECK(state.damage_clips == damage, "plane off: damage dropped");

	st.crtcs[0].mode_changed = 0;
	st.crtcs[0].connectors_changed = 1;
	damage_init();
	set_plane_damage(&state, damage, 1);
	drm_atomic_helper_check_plane_damage(&st, &state);
	CHECK(!state.damage_clips, "connectors changed: damage kept");
}

/* ---- the plane check ------------------------------------------------------ */

/* 1024x768@60 (VESA): 65 MHz, 1344 x 806 total */
static const struct drm_mode_modeinfo mode_1024 = {
	.clock = 65000,
	.hdisplay = 1024, .hsync_start = 1048, .hsync_end = 1184, .htotal = 1344,
	.vdisplay = 768, .vsync_start = 771, .vsync_end = 777, .vtotal = 806,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.name = "1024x768",
};

struct plane_case {
	const char *name;
	struct { unsigned int x, y, w, h; } src, src_expected;
	struct { int x, y; unsigned int w, h; } crtc, crtc_expected;
	unsigned int rotation;
	int min_scale, max_scale;
	bool can_position;
};

static const struct plane_case valid_cases[] = {
	{ "clipping_simple", { 0, 0, 2048 << 16, 2048 << 16 }, { 0, 0, 1024 << 16, 768 << 16 },
	  { 0, 0, 2048, 2048 }, { 0, 0, 1024, 768 },
	  DRM_MODE_ROTATE_0, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false },
	{ "clipping_rotate_reflect", { 0, 0, 2048 << 16, 2048 << 16 }, { 0, 0, 768 << 16, 1024 << 16 },
	  { 0, 0, 2048, 2048 }, { 0, 0, 1024, 768 },
	  DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_X, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false },
	{ "positioning_simple", { 0, 0, 1023 << 16, 767 << 16 }, { 0, 0, 1023 << 16, 767 << 16 },
	  { 0, 0, 1023, 767 }, { 0, 0, 1023, 767 },
	  DRM_MODE_ROTATE_0, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, true },
	{ "upscaling", { 0, 0, 512 << 16, 384 << 16 }, { 0, 0, 512 << 16, 384 << 16 },
	  { 0, 0, 1024, 768 }, { 0, 0, 1024, 768 },
	  DRM_MODE_ROTATE_0, 0x8000, DRM_PLANE_NO_SCALING, false },
	{ "downscaling", { 0, 0, 2048 << 16, 1536 << 16 }, { 0, 0, 2048 << 16, 1536 << 16 },
	  { 0, 0, 1024, 768 }, { 0, 0, 1024, 768 },
	  DRM_MODE_ROTATE_0, DRM_PLANE_NO_SCALING, 0x20000, false },
	{ "rounding1", { 0, 0, 0x40001, 0x40001 }, { 0, 0, 2 << 16, 2 << 16 },
	  { 1022, 766, 4, 4 }, { 1022, 766, 2, 2 },
	  DRM_MODE_ROTATE_0, DRM_PLANE_NO_SCALING, 0x10001, true },
	{ "rounding2", { 0x20001, 0x20001, 0x4040001, 0x3040001 }, { 0x40002, 0x40002, 1024 << 16, 768 << 16 },
	  { -2, -2, 1028, 772 }, { 0, 0, 1024, 768 },
	  DRM_MODE_ROTATE_0, DRM_PLANE_NO_SCALING, 0x10001, false },
	/* not rounded to 0x20001, which would be upscaling */
	{ "rounding3", { 0, 0, 0x3ffff, 0x3ffff }, { 0, 0, 2 << 16, 2 << 16 },
	  { 1022, 766, 4, 4 }, { 1022, 766, 2, 2 },
	  DRM_MODE_ROTATE_0, 0xffff, DRM_PLANE_NO_SCALING, true },
	{ "rounding4", { 0x1ffff, 0x1ffff, 0x403ffff, 0x303ffff }, { 0x3fffe, 0x3fffe, 1024 << 16, 768 << 16 },
	  { -2, -2, 1028, 772 }, { 0, 0, 1024, 768 },
	  DRM_MODE_ROTATE_0, 0xffff, DRM_PLANE_NO_SCALING, false },
};

static const struct plane_case invalid_cases[] = {
	{ "positioning_invalid", { 0, 0, 1023 << 16, 767 << 16 }, { 0, 0, 0, 0 },
	  { 0, 0, 1023, 767 }, { 0, 0, 0, 0 },
	  DRM_MODE_ROTATE_0, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false },
	{ "upscaling_invalid", { 0, 0, 512 << 16, 384 << 16 }, { 0, 0, 0, 0 },
	  { 0, 0, 1024, 768 }, { 0, 0, 0, 0 },
	  DRM_MODE_ROTATE_0, 0x8001, DRM_PLANE_NO_SCALING, false },
	{ "downscaling_invalid", { 0, 0, 2048 << 16, 1536 << 16 }, { 0, 0, 0, 0 },
	  { 0, 0, 1024, 768 }, { 0, 0, 0, 0 },
	  DRM_MODE_ROTATE_0, DRM_PLANE_NO_SCALING, 0x1ffff, false },
};

static struct drm_crtc check_crtc;
static struct drm_crtc_state check_crtc_state;
static struct drm_framebuffer check_fb;
static struct drm_plane check_plane;

static void plane_init(struct drm_plane_state *ps, const struct plane_case *c)
{
	memset(&check_fb, 0, sizeof(check_fb));
	check_fb.width = 2048;
	check_fb.height = 2048;
	memset(&check_crtc, 0, sizeof(check_crtc));
	check_crtc.index = 0;
	check_crtc.id = 40;
	check_crtc.active = 1;
	check_crtc.mode = mode_1024;
	memset(&check_crtc_state, 0, sizeof(check_crtc_state));
	check_crtc_state.crtc = &check_crtc;
	check_crtc_state.mode_valid = 1;
	check_crtc_state.active = 1;
	check_crtc_state.mode = mode_1024;

	memset(ps, 0, sizeof(*ps));
	ps->plane = &check_plane;
	ps->crtc = 0;
	ps->fb = &check_fb;
	ps->rotation = c->rotation;
	ps->src_x = c->src.x;
	ps->src_y = c->src.y;
	ps->src_w = c->src.w;
	ps->src_h = c->src.h;
	ps->crtc_x = c->crtc.x;
	ps->crtc_y = c->crtc.y;
	ps->crtc_w = c->crtc.w;
	ps->crtc_h = c->crtc.h;
}

static void test_check_plane_state(void)
{
	struct drm_plane_state ps;

	for (unsigned i = 0; i < sizeof(valid_cases) / sizeof(valid_cases[0]); i++) {
		const struct plane_case *c = &valid_cases[i];
		struct drm_rect se = DRM_RECT_INIT((int)c->src_expected.x, (int)c->src_expected.y,
						   (int)c->src_expected.w, (int)c->src_expected.h);
		struct drm_rect de = DRM_RECT_INIT(c->crtc_expected.x, c->crtc_expected.y,
						   (int)c->crtc_expected.w, (int)c->crtc_expected.h);
		int ret;

		plane_init(&ps, c);
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state, c->min_scale,
							  c->max_scale, c->can_position, false);
		CHECK(ret == 0, "%s: returned %d", c->name, ret);
		CHECK(ps.visible, "%s: not visible", c->name);
		CHECK(ps.src.x1 >= 0 && ps.src.y1 >= 0, "%s: src below 0", c->name);
		CHECK(drm_rect_equals(&ps.src, &se), "%s: src " DRM_RECT_FP_FMT ", want " DRM_RECT_FP_FMT,
		      c->name, DRM_RECT_FP_ARG(&ps.src), DRM_RECT_FP_ARG(&se));
		CHECK(drm_rect_equals(&ps.dst, &de), "%s: dst " DRM_RECT_FMT ", want " DRM_RECT_FMT,
		      c->name, DRM_RECT_ARG(&ps.dst), DRM_RECT_ARG(&de));

		/* the legacy form of the same check agrees */
		{
			struct drm_rect src = DRM_RECT_INIT((int)c->src.x, (int)c->src.y,
							    (int)c->src.w, (int)c->src.h);
			struct drm_rect dst = DRM_RECT_INIT(c->crtc.x, c->crtc.y,
							    (int)c->crtc.w, (int)c->crtc.h);
			bool visible = false;

			ret = drm_plane_helper_check_update(&check_plane, &check_crtc, &check_fb,
							    &src, &dst, c->rotation, c->min_scale,
							    c->max_scale, c->can_position, false,
							    &visible);
			CHECK(ret == 0 && visible && drm_rect_equals(&src, &se) &&
			      drm_rect_equals(&dst, &de), "%s: legacy check differs (%d)", c->name, ret);
		}
	}

	for (unsigned i = 0; i < sizeof(invalid_cases) / sizeof(invalid_cases[0]); i++) {
		const struct plane_case *c = &invalid_cases[i];
		int ret;

		plane_init(&ps, c);
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state, c->min_scale,
							  c->max_scale, c->can_position, false);
		CHECK(ret < 0, "%s: returned %d", c->name, ret);
	}

	/* The errors themselves, and the cases around a disabled crtc. */
	{
		int ret;

		plane_init(&ps, &invalid_cases[1]);
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state, 0x8001,
							  DRM_PLANE_NO_SCALING, false, false);
		CHECK(ret == -ERANGE, "scale error %d", ret);
		plane_init(&ps, &invalid_cases[0]);
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state,
							  DRM_PLANE_NO_SCALING,
							  DRM_PLANE_NO_SCALING, false, false);
		CHECK(ret == -EINVAL, "position error %d", ret);

		/* no framebuffer: invisible, fine, crtc state may be absent */
		plane_init(&ps, &valid_cases[0]);
		ps.fb = NULL;
		ps.crtc = -1;
		ps.visible = 1;
		ret = drm_atomic_helper_check_plane_state(&ps, NULL, DRM_PLANE_NO_SCALING,
							  DRM_PLANE_NO_SCALING, false, false);
		CHECK(ret == 0 && !ps.visible, "no fb: %d visible %d", ret, ps.visible);

		/* a crtc without a mode */
		plane_init(&ps, &valid_cases[2]);
		check_crtc_state.mode_valid = 0;
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state,
							  DRM_PLANE_NO_SCALING,
							  DRM_PLANE_NO_SCALING, true, false);
		CHECK(ret == -EINVAL, "disabled crtc: %d", ret);
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state,
							  DRM_PLANE_NO_SCALING,
							  DRM_PLANE_NO_SCALING, true, true);
		CHECK(ret == 0 && !ps.visible, "disabled crtc, can update: %d visible %d",
		      ret, ps.visible);

		/* wholly off the crtc: not visible, not an error */
		plane_init(&ps, &valid_cases[2]);
		ps.crtc_x = 2000;
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state,
							  DRM_PLANE_NO_SCALING,
							  DRM_PLANE_NO_SCALING, true, false);
		CHECK(ret == 0 && !ps.visible, "off the crtc: %d visible %d", ret, ps.visible);

		/* a frame-packed stereo mode is vdisplay + vtotal lines tall */
		plane_init(&ps, &valid_cases[0]);
		check_crtc_state.mode.flags |= DRM_MODE_FLAG_3D_FRAME_PACKING;
		ret = drm_atomic_helper_check_plane_state(&ps, &check_crtc_state,
							  DRM_PLANE_NO_SCALING,
							  DRM_PLANE_NO_SCALING, false, false);
		CHECK(ret == 0 && ps.dst.y2 == 768 + 806, "frame packing: %d dst.y2 %d",
		      ret, ps.dst.y2);

		/* a crtc state of another crtc is a caller bug: warned about */
		{
			int w0 = warnings;

			plane_init(&ps, &valid_cases[0]);
			ps.crtc = 1;
			drm_atomic_helper_check_plane_state(&ps, &check_crtc_state,
							    DRM_PLANE_NO_SCALING,
							    DRM_PLANE_NO_SCALING, false, false);
			CHECK(warnings == w0 + 1, "wrong crtc state not warned about");
		}
	}
}

/* ---- primary plane, plane mask, committed state ---------------------------- */

static void test_crtc_primary_and_old_state(void)
{
	static struct drm_device dev;
	static struct drm_atomic_state st;
	static struct drm_framebuffer fbs[1];
	struct drm_plane_state os;
	int ret;

	memset(&dev, 0, sizeof(dev));
	memset(&st, 0, sizeof(st));
	dev.ncrtc = 2;
	dev.nplanes = 4;
	for (int i = 0; i < 2; i++) {
		dev.crtc[i].index = i;
		dev.crtc[i].id = 30 + (uint32_t)i;
		st.crtcs[i].crtc = &dev.crtc[i];
	}
	/* planes: 0 primary(crtc0) 1 cursor(crtc0) 2 primary(crtc1) 3 overlay */
	static const uint32_t types[4] = { DRM_PLANE_TYPE_PRIMARY, DRM_PLANE_TYPE_CURSOR,
					   DRM_PLANE_TYPE_PRIMARY, DRM_PLANE_TYPE_OVERLAY };
	for (int i = 0; i < 4; i++) {
		dev.planes[i].index = i;
		dev.planes[i].type = types[i];
		dev.planes[i].crtc = -1;
		st.planes[i].plane = &dev.planes[i];
		st.planes[i].crtc = -1;
	}
	st.dev = &dev;

	st.planes[1].crtc = 0;
	st.planes[3].crtc = 0;
	CHECK(drm_atomic_helper_crtc_plane_mask(&st, 0) == 0xa, "plane mask 0x%x",
	      drm_atomic_helper_crtc_plane_mask(&st, 0));
	ret = drm_atomic_helper_check_crtc_primary_plane(&st, &st.crtcs[0]);
	CHECK(ret == -EINVAL, "no primary: %d", ret);
	st.planes[0].crtc = 0;
	ret = drm_atomic_helper_check_crtc_primary_plane(&st, &st.crtcs[0]);
	CHECK(ret == 0, "primary on: %d", ret);
	/* another crtc's primary does not count */
	ret = drm_atomic_helper_check_crtc_primary_plane(&st, &st.crtcs[1]);
	CHECK(ret == -EINVAL, "crtc1 without its primary: %d", ret);
	st.planes[2].crtc = 1;
	ret = drm_atomic_helper_check_crtc_primary_plane(&st, &st.crtcs[1]);
	CHECK(ret == 0, "crtc1 primary on: %d", ret);

	/* the committed plane as an old state: a 2048x2048 source on the
	 * 1024x768 crtc 0, clipped as the check clips it */
	memset(fbs, 0, sizeof(fbs));
	fbs[0].id = 77;
	fbs[0].width = 2048;
	fbs[0].height = 2048;
	fb_dev = &dev;
	fb_table = fbs;
	fb_count = 1;
	dev.crtc[0].active = 1;
	dev.crtc[0].mode = mode_1024;
	dev.planes[0].crtc = 0;
	dev.planes[0].fb_id = 77;
	dev.planes[0].src_w = 2048u << 16;
	dev.planes[0].src_h = 2048u << 16;
	dev.planes[0].crtc_w = 2048;
	dev.planes[0].crtc_h = 2048;
	drm_atomic_helper_old_plane_state(&dev, &dev.planes[0], &os);
	CHECK(os.fb == &fbs[0] && os.crtc == 0 && os.visible, "old state: fb %p crtc %d visible %d",
	      (void *)os.fb, os.crtc, os.visible);
	CHECK(os.src.x1 == 0 && os.src.y1 == 0 && os.src.x2 == 1024 << 16 && os.src.y2 == 768 << 16,
	      "old state src " DRM_RECT_FP_FMT, DRM_RECT_FP_ARG(&os.src));
	CHECK(os.dst.x2 == 1024 && os.dst.y2 == 768, "old state dst " DRM_RECT_FMT, DRM_RECT_ARG(&os.dst));

	/* ... and the damage of an unmoved update against it is the clips */
	{
		struct drm_plane_state ns = os;
		struct drm_mode_rect damage[1];
		struct drm_rect r;

		set_damage_clip(&damage[0], 5, 6, 7, 8);
		ns.damage_clips = damage;
		ns.num_damage_clips = 1;
		CHECK(drm_atomic_helper_damage_merged(&os, &ns, &r) &&
		      r.x1 == 5 && r.y1 == 6 && r.x2 == 7 && r.y2 == 8,
		      "old state damage %d %d %d %d", r.x1, r.y1, r.x2, r.y2);
	}

	/* crtc off: nothing visible */
	dev.crtc[0].active = 0;
	drm_atomic_helper_old_plane_state(&dev, &dev.planes[0], &os);
	CHECK(!os.visible, "old state on a crtc that is off is visible");
	/* plane off */
	dev.planes[0].crtc = -1;
	drm_atomic_helper_old_plane_state(&dev, &dev.planes[0], &os);
	CHECK(!os.visible && os.crtc == -1, "old state of a plane that is off");
}

int main(void)
{
	test_damage();
	test_damage_merged();
	test_damage_accessors();
	test_check_plane_damage();
	test_check_plane_state();
	test_crtc_primary_and_old_state();

	if (fails) {
		printf("drm_damage/atomic_helper: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("drm_damage/atomic_helper: all %d checks passed\n", checks);
	return 0;
}
