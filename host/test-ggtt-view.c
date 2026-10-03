/*
 * Host test for the page order of a rotated GGTT view
 * (kernel/dev/gpu/i915/i915_ggtt_view.c), run by host/test-ggtt-view.sh,
 * which puts the file's pure functions in front of this one.
 *
 * Each view is compared page by page with a model that builds the view
 * the way a list of mapping runs is built: for every tile column, one
 * run per tile from the bottom tile row up, then one padding run up to
 * the column stride, the runs then expanded to pages.  Then the
 * geometry: read as a tiled surface with dst_stride tiles per row, tile
 * (x, y) of the view must be the source tile (y, height - 1 - x) -- the
 * surface turned on its side.
 *
 * Copyright (C) 2026 The LikeOS Project
 * SPDX-License-Identifier for the portions derived from Intel's code: MIT
 * Portions Copyright (C) 2016 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...)                                                          \
	do {                                                                   \
		if (!(c)) {                                                    \
			failures++;                                            \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

/* ---- the model: runs of (first page, pages), padding runs marked ------------- */

struct run {
	uint64_t first; /* object page, or I915_ROTATION_PADDING */
	uint64_t pages;
};

static unsigned model_plane(const struct i915_rotation_plane *p, struct run *runs, unsigned n)
{
	unsigned column, row;
	uint64_t src_idx;

	for (column = 0; column < p->width; column++) {
		unsigned left;

		src_idx = (uint64_t)p->src_stride * (p->height - 1) + column + p->offset;
		for (row = 0; row < p->height; row++) {
			runs[n].first = src_idx;
			runs[n].pages = 1;
			n++;
			src_idx -= p->src_stride;
		}
		left = p->dst_stride - p->height;
		if (!left)
			continue;
		runs[n].first = I915_ROTATION_PADDING;
		runs[n].pages = left;
		n++;
	}
	return n;
}

static uint64_t *model_view(const struct i915_rotation_info *r, uint64_t *npages)
{
	uint64_t size = i915_rotation_info_size(r), k = 0;
	struct run *runs = calloc(size + 1, sizeof(*runs));
	uint64_t *pages = calloc(size + 1, sizeof(*pages));
	unsigned n = 0;

	for (unsigned i = 0; i < I915_ROTATION_PLANES; i++)
		n = model_plane(&r->plane[i], runs, n);
	for (unsigned i = 0; i < n; i++)
		for (uint64_t j = 0; j < runs[i].pages; j++)
			pages[k++] = runs[i].first;
	free(runs);
	*npages = k;
	return pages;
}

static void check_view(const char *what, const struct i915_rotation_info *r)
{
	uint64_t n, size = i915_rotation_info_size(r);
	uint64_t *want = model_view(r, &n);
	unsigned bad = 0;

	CHECK(n == size, "%s: model has %llu pages, the view %llu", what,
	      (unsigned long long)n, (unsigned long long)size);
	for (uint64_t i = 0; i < n && i < size; i++) {
		uint64_t got = i915_rotation_view_page(r, i);
		if (got != want[i] && bad++ < 4)
			CHECK(0, "%s: page %llu maps %lld, expected %lld", what,
			      (unsigned long long)i, (long long)got, (long long)want[i]);
	}
	CHECK(i915_rotation_view_page(r, size) == I915_ROTATION_PADDING,
	      "%s: page past the view", what);
	free(want);
}

/* The view of plane 0 read as a surface of dst_stride tiles per tile row:
 * tile (x, y) is source tile (column y, row height - 1 - x). */
static void check_geometry(const char *what, const struct i915_rotation_info *r)
{
	const struct i915_rotation_plane *p = &r->plane[0];

	for (uint32_t y = 0; y < p->width; y++)
		for (uint32_t x = 0; x < p->height; x++) {
			uint64_t got = i915_rotation_view_page(r, (uint64_t)y * p->dst_stride + x);
			uint64_t src = p->offset + (uint64_t)(p->height - 1 - x) * p->src_stride + y;
			CHECK(got == src, "%s: view tile %u,%u maps %llu, the turned surface has %llu",
			      what, x, y, (unsigned long long)got, (unsigned long long)src);
		}
}

static struct i915_rotation_info one(uint32_t offset, uint32_t w, uint32_t h, uint32_t src_stride,
				     uint32_t dst_stride)
{
	struct i915_rotation_info r;

	memset(&r, 0, sizeof(r));
	r.plane[0].offset = offset;
	r.plane[0].width = w;
	r.plane[0].height = h;
	r.plane[0].src_stride = src_stride;
	r.plane[0].dst_stride = dst_stride;
	return r;
}

int main(void)
{
	struct i915_rotation_info r;

	/* the smallest: one tile */
	r = one(0, 1, 1, 1, 1);
	check_view("1x1", &r);
	CHECK(i915_rotation_view_page(&r, 0) == 0, "1x1 maps tile 0");

	/* 1920x1080 XRGB8888 Y tiled: 32-pixel tiles across (60), 32 rows
	 * down (34 tile rows, the last partial) */
	r = one(0, 60, 34, 60, 34);
	check_view("1920x1080", &r);
	check_geometry("1920x1080", &r);
	/* the first page of the view is the bottom left tile */
	CHECK(i915_rotation_view_page(&r, 0) == 33 * 60, "bottom left first");
	CHECK(i915_rotation_view_page(&r, 33) == 0, "top left ends the first column");
	CHECK(i915_rotation_view_page(&r, 34) == 33 * 60 + 1, "second column starts at the bottom");

	/* a framebuffer at an offset in a wider object, padded columns */
	r = one(7, 5, 3, 16, 8);
	check_view("offset+padding", &r);
	check_geometry("offset+padding", &r);
	CHECK(i915_rotation_view_page(&r, 3) == I915_ROTATION_PADDING, "padding after the column");
	CHECK(i915_rotation_view_page(&r, 8) == 7 + 2 * 16 + 1, "column 1 after the padding");

	/* 3840x2160 RGB565 (64 pixels a tile): 60 x 68 tiles */
	r = one(128, 60, 68, 64, 68);
	check_view("3840x2160 565", &r);
	check_geometry("3840x2160 565", &r);

	/* two planes, laid out one after the other */
	r = one(0, 4, 3, 4, 3);
	r.plane[1].offset = 12;
	r.plane[1].width = 2;
	r.plane[1].height = 2;
	r.plane[1].src_stride = 2;
	r.plane[1].dst_stride = 4;
	check_view("two planes", &r);
	CHECK(i915_rotation_info_size(&r) == 4 * 3 + 2 * 4, "two planes' size");
	CHECK(i915_rotation_view_page(&r, 12) == 12 + 2, "second plane's first page");

	/* validity: inside the object, not taller than a column, not empty */
	r = one(0, 60, 34, 60, 34);
	CHECK(i915_rotation_info_valid(&r, 60 * 34), "exact fit is valid");
	CHECK(!i915_rotation_info_valid(&r, 60 * 34 - 1), "one page short");
	r = one(7, 5, 3, 16, 8);
	CHECK(i915_rotation_info_valid(&r, 7 + 2 * 16 + 5), "offset fit");
	CHECK(!i915_rotation_info_valid(&r, 7 + 2 * 16 + 4), "offset one short");
	r = one(0, 5, 9, 5, 8);
	CHECK(!i915_rotation_info_valid(&r, 1000), "column shorter than the plane");
	r = one(0, 0, 0, 0, 0);
	CHECK(!i915_rotation_info_valid(&r, 1000), "empty view");
	r = one(0, 1u << 16, 1u << 16, 1u << 16, 1u << 16);
	CHECK(!i915_rotation_info_valid(&r, ~0ULL), "beyond the global GTT");

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("ggtt view: all tests passed\n");
	return 0;
}
