/* Host test: the Skylake-and-later watermark levels (appended to the
 * driver's own computation by host/test-skl-wm.sh).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>

static int fails, checks;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			fails++;                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

/* One plane's level `level' at memory latency `latency' us (the
 * driver's correction of it included), the level below it all zero. */
static struct skl_wm_level level_for(int level, uint32_t latency, uint32_t rate,
				     uint32_t htotal, uint32_t width, int cpp,
				     uint64_t modifier, int is_cursor)
{
	struct intel_wm_pipe_cfg c = { .htotal = htotal };
	struct skl_wm_params wp;
	struct skl_wm_level prev, res;

	memset(&prev, 0, sizeof(prev));
	memset(&res, 0, sizeof(res));
	skl_compute_wm_params(&c, width, cpp, modifier, rate, &wp, 0);
	g_wm.latency[level] = (uint16_t)latency;
	skl_compute_plane_wm(is_cursor, level, skl_wm_latency(level, wp.x_tiled, 1), &wp, &prev,
			     &res);
	return res;
}

static void show(const char *what, const struct skl_wm_level *l)
{
	printf("  %-34s %3u blocks, %2u lines, min %5u, %s\n", what, l->blocks, l->lines,
	       l->min_ddb_alloc, l->enable ? "on" : "off");
}

int main(void)
{
	/* The panel of the machine this was written for (display version 9):
	 * 1920x1080 at 142 MHz, 2100 total columns; level 0 at the 2 us
	 * the firmware reports, level 1 at 19 us. */
	const uint32_t rate = 142000, htotal = 2100, width = 1920;
	struct skl_wm_level l;

	g_wm.ver = 9;
	g_wm.num_levels = 8;

	/* Linear, level 0: method 1 (2 us is under the 15 us line time),
	 * 2*142000*4/512000 = 2.22 blocks, rounded up and one more. */
	l = level_for(0, 2, rate, htotal, width, 4, DRM_FORMAT_MOD_LINEAR, 0);
	show("linear, level 0", &l);
	CHECK(l.enable && l.blocks == 4 && l.lines == 0 && l.min_ddb_alloc == 5,
	      "linear level 0: 4 blocks, no lines, min 5; got %u/%u/%u", l.blocks, l.lines,
	      l.min_ddb_alloc);

	/* X tiled, level 0: the bandwidth workaround adds 15 us (17 us is
	 * over the line time: the smaller of 18.86 and 2 lines of 15
	 * blocks), so never the four blocks the bare latency gives. */
	l = level_for(0, 2, rate, htotal, width, 4, I915_FORMAT_MOD_X_TILED, 0);
	show("X tiled, level 0", &l);
	CHECK(l.enable && l.blocks == 20, "X tiled level 0: 20 blocks, got %u", l.blocks);
	CHECK(l.blocks > 4, "and not the four of the bare latency");

	/* Y tiled, level 0: eight lines a tile row on display version 9,
	 * 120 blocks a tile row, the tile row minimum wins. */
	l = level_for(0, 2, rate, htotal, width, 4, I915_FORMAT_MOD_Y_TILED, 0);
	show("Y tiled, level 0", &l);
	CHECK(l.enable && l.blocks == 121, "Y tiled level 0: 121 blocks, got %u", l.blocks);
	CHECK(l.blocks > 4 * 4, "a Y tiled plane needs far more than a linear one");

	/* Level 1: lines count; Display WA #1126 adds a tile row (Y) or a
	 * block (linear). */
	l = level_for(1, 19, rate, htotal, width, 4, I915_FORMAT_MOD_Y_TILED, 0);
	show("Y tiled, level 1", &l);
	CHECK(l.enable && l.blocks == 241 && l.lines == 16,
	      "Y tiled level 1: 241 blocks, 16 lines; got %u/%u", l.blocks, l.lines);
	l = level_for(1, 19, rate, htotal, width, 4, DRM_FORMAT_MOD_LINEAR, 0);
	show("linear, level 1", &l);
	CHECK(l.enable && l.blocks == 24 && l.lines == 2,
	      "linear level 1: 24 blocks, 2 lines; got %u/%u", l.blocks, l.lines);

	/* The cursor: 64 pixels, linear, tiny. */
	l = level_for(0, 2, rate, htotal, 64, 4, DRM_FORMAT_MOD_LINEAR, 1);
	show("cursor 64, level 0", &l);
	CHECK(l.enable && l.blocks == 4, "cursor level 0: 4 blocks, got %u", l.blocks);

	/* A level without a latency is refused, not programmed. */
	l = level_for(1, 0, rate, htotal, width, 4, I915_FORMAT_MOD_Y_TILED, 0);
	CHECK(!l.enable && l.min_ddb_alloc == U16_MAX_, "no latency: level refused");

	/* More lines than the register holds (31 here) is refused too:
	 * 500 us is 34 lines of a linear 1920 plane. */
	l = level_for(1, 500, rate, htotal, width, 4, DRM_FORMAT_MOD_LINEAR, 0);
	show("linear, level 1 at 500 us", &l);
	CHECK(!l.enable && l.min_ddb_alloc == U16_MAX_, "34 lines on display 9: refused");
	g_wm.ver = 13; /* 255 lines from version 13 */
	l = level_for(1, 500, rate, htotal, width, 4, DRM_FORMAT_MOD_LINEAR, 0);
	CHECK(l.enable && l.lines == 34, "34 lines on display 13: kept, got %u", l.lines);

	/* Display version 12, Y tiled, level 0: four lines a tile row, one
	 * block more per row, and from version 11 the allocation must hold
	 * the lines and another tile row: 8 lines of 15.25 blocks. */
	g_wm.ver = 12;
	l = level_for(0, 2, rate, htotal, width, 4, I915_FORMAT_MOD_Y_TILED, 0);
	show("Y tiled, level 0, display 12", &l);
	CHECK(l.enable && l.blocks == 62 && l.lines == 4 && l.min_ddb_alloc == 123,
	      "display 12 Y tiled level 0: 62 blocks, 4 lines, min 123; got %u/%u/%u",
	      l.blocks, l.lines, l.min_ddb_alloc);

	if (fails) {
		printf("skl watermarks: %d of %d checks failed\n", fails, checks);
		return 1;
	}
	printf("skl watermarks: all %d checks passed\n", checks);
	return 0;
}
