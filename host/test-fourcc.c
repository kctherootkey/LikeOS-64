// Host test: the pixel format table (kernel/dev/gpu/drm/drm_fourcc.c).
//
// The block size, minimum pitch and lookup vectors of the format test
// suite, run against the kernel's table as it is, plus the checks the
// framebuffer code depends on: the formats ADDFB/ADDFB2 accepted before
// the table existed keep their bits per pixel and depth, the legacy
// bpp/depth pairs map as before, and the table itself is consistent.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Maíra Canal's code: GPL-2.0
// Portions Copyright (c) 2022 Maíra Canal <mairacanal@riseup.net>

#include <kernel/dev/gpu/drm_fourcc.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>

/* What drm_fourcc.c needs from the kernel: the warning printer. */
int kprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}

void panic(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	__builtin_abort();
}

/* A minimal stand-in for the unit-test macros the vectors are written in. */
struct kunit {
	const char *name;
};

static int fails, checks;

#define KUNIT_EXPECT_EQ(test, a, b)                                                   \
	do {                                                                          \
		unsigned long long __a = (unsigned long long)(a);                     \
		unsigned long long __b = (unsigned long long)(b);                     \
		checks++;                                                             \
		if (__a != __b) {                                                     \
			fails++;                                                      \
			printf("FAIL %s:%d (%s): %s == %llu, expected %s == %llu\n", \
			       __FILE__, __LINE__, (test)->name, #a, __a, #b, __b);   \
		}                                                                     \
	} while (0)
#define KUNIT_EXPECT_TRUE(test, c) KUNIT_EXPECT_EQ(test, !!(c), 1)
#define KUNIT_ASSERT_NOT_NULL(test, p)                                         \
	do {                                                                   \
		checks++;                                                      \
		if (!(p)) {                                                    \
			fails++;                                               \
			printf("FAIL %s:%d (%s): %s is NULL\n", __FILE__,      \
			       __LINE__, (test)->name, #p);                    \
			return;                                                \
		}                                                              \
	} while (0)

/* ---- the format test suite's vectors ------------------------------------ */

static void drm_test_format_block_width_invalid(struct kunit *test)
{
	const struct drm_format_info *info = NULL;

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 0);
}

static void drm_test_format_block_width_one_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_XRGB4444);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

static void drm_test_format_block_width_two_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_NV12);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 2), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

static void drm_test_format_block_width_three_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_YUV422);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 2), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 3), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

static void drm_test_format_block_width_tiled(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_X0L0);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 0), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, 1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_width(info, -1), 0);
}

static void drm_test_format_block_height_invalid(struct kunit *test)
{
	const struct drm_format_info *info = NULL;

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 0);
}

static void drm_test_format_block_height_one_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_XRGB4444);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 0);
}

static void drm_test_format_block_height_two_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_NV12);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 2), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
}

static void drm_test_format_block_height_three_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_YUV422);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 2), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 3), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
}

static void drm_test_format_block_height_tiled(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_X0L0);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 0), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, 1), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_block_height(info, -1), 0);
}

static void drm_test_format_min_pitch_invalid(struct kunit *test)
{
	const struct drm_format_info *info = NULL;

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);
}

static void drm_test_format_min_pitch_one_plane_8bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_RGB332);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 671);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1)),
			(uint64_t)(UINT_MAX - 1));
}

static void drm_test_format_min_pitch_one_plane_16bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_XRGB4444);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 1280);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 3840);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 8192);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 1342);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1)),
			(uint64_t)(UINT_MAX - 1) * 2);
}

static void drm_test_format_min_pitch_one_plane_24bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_RGB888);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 3);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 6);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 3072);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 5760);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 12288);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 2013);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 3);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX - 1),
			(uint64_t)(UINT_MAX - 1) * 3);
}

static void drm_test_format_min_pitch_one_plane_32bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_ABGR8888);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 8);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 2560);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 7680);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 16384);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 2684);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX - 1),
			(uint64_t)(UINT_MAX - 1) * 4);
}

static void drm_test_format_min_pitch_two_plane(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_NV12);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 320), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 512), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 960), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 2048), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 671);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 336), 672);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, UINT_MAX / 2 + 1),
			(uint64_t)UINT_MAX + 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1)),
			(uint64_t)(UINT_MAX - 1));
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, (UINT_MAX - 1) /  2),
			(uint64_t)(UINT_MAX - 1));
}

static void drm_test_format_min_pitch_three_plane_8bpp(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_YUV422);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 3, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 1), 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 2), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 640);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 320), 320);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 320), 320);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 1024);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 512), 512);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 512), 512);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 1920);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 960), 960);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 960), 960);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 4096);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 2048), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 2048), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 671);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 336), 336);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 336), 336);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, UINT_MAX / 2 + 1),
			(uint64_t)UINT_MAX / 2 + 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, UINT_MAX / 2 + 1),
			(uint64_t)UINT_MAX / 2 + 1);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, (UINT_MAX - 1) / 2),
			(uint64_t)(UINT_MAX - 1) / 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, (UINT_MAX - 1) / 2),
			(uint64_t)(UINT_MAX - 1) / 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, (UINT_MAX - 1) / 2),
			(uint64_t)(UINT_MAX - 1) / 2);
}

static void drm_test_format_min_pitch_tiled(struct kunit *test)
{
	const struct drm_format_info *info = drm_format_info(DRM_FORMAT_X0L2);

	KUNIT_ASSERT_NOT_NULL(test, info);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, -1, 0), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 0), 0);

	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1), 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 2), 4);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 640), 1280);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1024), 2048);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 3840);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 4096), 8192);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 671), 1342);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX),
			(uint64_t)UINT_MAX * 2);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, UINT_MAX - 1),
			(uint64_t)(UINT_MAX - 1) * 2);
}

#define RUN(fn) do { struct kunit t = { #fn }; fn(&t); } while (0)

/* ---- what the framebuffer code relies on --------------------------------- */

/* The formats ADDFB2 took when they were a hard-coded list: the bits per
 * pixel and depth a framebuffer reports (GETFB) must not change. */
static void likeos_test_old_formats(struct kunit *test)
{
	static const struct {
		uint32_t format, bpp, depth;
	} old[] = {
		{ DRM_FORMAT_XRGB8888, 32, 24 },    { DRM_FORMAT_XBGR8888, 32, 24 },
		{ DRM_FORMAT_ARGB8888, 32, 32 },    { DRM_FORMAT_ABGR8888, 32, 32 },
		{ DRM_FORMAT_XRGB2101010, 32, 30 }, { DRM_FORMAT_XBGR2101010, 32, 30 },
		{ DRM_FORMAT_RGB565, 16, 16 },
	};

	for (unsigned i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
		const struct drm_format_info *info = __drm_format_info(old[i].format);

		KUNIT_ASSERT_NOT_NULL(test, info);
		KUNIT_EXPECT_EQ(test, info->num_planes, 1);
		KUNIT_EXPECT_EQ(test, drm_format_info_bpp(info, 0), old[i].bpp);
		KUNIT_EXPECT_EQ(test, info->depth, old[i].depth);
		KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920),
				1920 * old[i].bpp / 8);
		KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1366),
				1366 * old[i].bpp / 8);
		KUNIT_EXPECT_EQ(test, drm_format_info_plane_width(info, 1366, 0), 1366);
		KUNIT_EXPECT_EQ(test, drm_format_info_plane_width(info, 1366, 1), 0);
	}
}

static void likeos_test_legacy_formats(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(32, 24), DRM_FORMAT_XRGB8888);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(32, 32), DRM_FORMAT_ARGB8888);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(32, 30), DRM_FORMAT_XRGB2101010);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(16, 16), DRM_FORMAT_RGB565);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(16, 15), DRM_FORMAT_XRGB1555);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(24, 24), DRM_FORMAT_RGB888);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(8, 8), DRM_FORMAT_C8);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(1, 1), DRM_FORMAT_C1);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(16, 0), DRM_FORMAT_INVALID);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(32, 8), DRM_FORMAT_INVALID);
	KUNIT_EXPECT_EQ(test, drm_mode_legacy_fb_format(0, 0), DRM_FORMAT_INVALID);
	/* every pair that names a format names one the table knows, with
	 * that depth */
	for (uint32_t bpp = 0; bpp <= 32; bpp++)
		for (uint32_t depth = 0; depth <= 32; depth++) {
			uint32_t f = drm_mode_legacy_fb_format(bpp, depth);
			const struct drm_format_info *info;

			if (f == DRM_FORMAT_INVALID)
				continue;
			info = __drm_format_info(f);
			KUNIT_ASSERT_NOT_NULL(test, info);
			KUNIT_EXPECT_EQ(test, info->depth, depth);
			KUNIT_EXPECT_EQ(test, drm_format_info_bpp(info, 0), bpp);
		}
}

static void likeos_test_multi_planar(struct kunit *test)
{
	const struct drm_format_info *info = __drm_format_info(DRM_FORMAT_NV12);

	KUNIT_ASSERT_NOT_NULL(test, info);
	KUNIT_EXPECT_EQ(test, info->num_planes, 2);
	KUNIT_EXPECT_TRUE(test, drm_format_info_is_yuv_semiplanar(info));
	KUNIT_EXPECT_TRUE(test, drm_format_info_is_yuv_sampling_420(info));
	KUNIT_EXPECT_EQ(test, drm_format_info_plane_width(info, 641, 0), 641);
	KUNIT_EXPECT_EQ(test, drm_format_info_plane_width(info, 641, 1), 321);
	KUNIT_EXPECT_EQ(test, drm_format_info_plane_height(info, 481, 1), 241);
	KUNIT_EXPECT_EQ(test, drm_format_info_plane_height(info, 481, 2), 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 1, 321), 642);

	info = __drm_format_info(DRM_FORMAT_YUV420);
	KUNIT_ASSERT_NOT_NULL(test, info);
	KUNIT_EXPECT_EQ(test, info->num_planes, 3);
	KUNIT_EXPECT_TRUE(test, drm_format_info_is_yuv_planar(info));
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 2, 320), 320);

	/* a format that only exists in non-linear layouts has no pitch */
	info = __drm_format_info(DRM_FORMAT_YUV420_8BIT);
	KUNIT_ASSERT_NOT_NULL(test, info);
	KUNIT_EXPECT_EQ(test, info->char_per_block[0], 0);
	KUNIT_EXPECT_EQ(test, drm_format_info_min_pitch(info, 0, 1920), 0);

	KUNIT_EXPECT_TRUE(test, __drm_format_info(DRM_FORMAT_INVALID) == NULL);
	KUNIT_EXPECT_TRUE(test, __drm_format_info(0x12345678) == NULL);
}

/* The table: every code once, 1 to 3 planes, sane subsampling, nothing
 * described for planes the format does not have. */
static void likeos_test_table(struct kunit *test)
{
	static const uint32_t probe[] = {
		DRM_FORMAT_C1, DRM_FORMAT_C8, DRM_FORMAT_R8, DRM_FORMAT_RGB332,
		DRM_FORMAT_XRGB4444, DRM_FORMAT_ARGB1555, DRM_FORMAT_RGB565,
		DRM_FORMAT_BGR888, DRM_FORMAT_RGB565_A8, DRM_FORMAT_XRGB8888,
		DRM_FORMAT_RGBA1010102, DRM_FORMAT_RGB161616, DRM_FORMAT_ABGR16161616F,
		DRM_FORMAT_XRGB8888_A8, DRM_FORMAT_YVU410, DRM_FORMAT_Y8,
		DRM_FORMAT_NV42, DRM_FORMAT_YUYV, DRM_FORMAT_XVUY2101010,
		DRM_FORMAT_Y416, DRM_FORMAT_X0L2, DRM_FORMAT_P016, DRM_FORMAT_P210,
		DRM_FORMAT_NV30, DRM_FORMAT_Q401, DRM_FORMAT_P230, DRM_FORMAT_S416,
		DRM_FORMAT_XYYY2101010, DRM_FORMAT_T430,
	};
	unsigned i, j, n = sizeof(probe) / sizeof(probe[0]);

	for (i = 0; i < n; i++) {
		const struct drm_format_info *info = __drm_format_info(probe[i]);

		KUNIT_ASSERT_NOT_NULL(test, info);
		KUNIT_EXPECT_EQ(test, info->format, probe[i]);
		KUNIT_EXPECT_TRUE(test, info->num_planes >= 1 && info->num_planes <= 3);
		KUNIT_EXPECT_TRUE(test, info->hsub >= 1 && info->vsub >= 1);
		for (j = info->num_planes; j < DRM_FORMAT_MAX_PLANES; j++) {
			KUNIT_EXPECT_EQ(test, info->char_per_block[j], 0);
			KUNIT_EXPECT_EQ(test, info->block_w[j], 0);
			KUNIT_EXPECT_EQ(test, info->block_h[j], 0);
		}
		/* the same code twice would make the second entry dead */
		for (j = 0; j < i; j++)
			KUNIT_EXPECT_TRUE(test, probe[j] != probe[i]);
	}
	/* sweep every printable code: a hit is unique and self-consistent */
	for (uint32_t a = ' '; a <= 'Z'; a++)
		for (uint32_t b = ' '; b <= 'Z'; b++)
			for (uint32_t c = ' '; c <= 'Z'; c++)
				for (uint32_t d = ' '; d <= 'Z'; d++) {
					uint32_t f = fourcc_code(a, b, c, d);
					const struct drm_format_info *info = __drm_format_info(f);

					if (!info)
						continue;
					if (info->format != f || info->num_planes < 1 ||
					    info->num_planes > 3) {
						fails++;
						printf("FAIL: entry %08x inconsistent\n", f);
					}
				}
}

int main(void)
{
	RUN(drm_test_format_block_width_invalid);
	RUN(drm_test_format_block_width_one_plane);
	RUN(drm_test_format_block_width_two_plane);
	RUN(drm_test_format_block_width_three_plane);
	RUN(drm_test_format_block_width_tiled);
	RUN(drm_test_format_block_height_invalid);
	RUN(drm_test_format_block_height_one_plane);
	RUN(drm_test_format_block_height_two_plane);
	RUN(drm_test_format_block_height_three_plane);
	RUN(drm_test_format_block_height_tiled);
	RUN(drm_test_format_min_pitch_invalid);
	RUN(drm_test_format_min_pitch_one_plane_8bpp);
	RUN(drm_test_format_min_pitch_one_plane_16bpp);
	RUN(drm_test_format_min_pitch_one_plane_24bpp);
	RUN(drm_test_format_min_pitch_one_plane_32bpp);
	RUN(drm_test_format_min_pitch_two_plane);
	RUN(drm_test_format_min_pitch_three_plane_8bpp);
	RUN(drm_test_format_min_pitch_tiled);
	RUN(likeos_test_old_formats);
	RUN(likeos_test_legacy_formats);
	RUN(likeos_test_multi_planar);
	RUN(likeos_test_table);

	/* the warning variant reports an unknown code and still answers */
	printf("(one WARNING line expected next)\n");
	if (drm_format_info(0x12345678) != NULL) {
		fails++;
		printf("FAIL: drm_format_info of an unknown code\n");
	}

	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails ? 1 : 0;
}
