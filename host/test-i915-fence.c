/* Host test of the fence register encoding and the tiled address
 * arithmetic (include/kernel/dev/gpu/i915/i915_fence_layout.h).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <stdio.h>
#include <kernel/dev/gpu/i915/i915_fence_layout.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
	/* a 64 KB Y-tiled object at 1 MB with a 512-byte stride */
	uint64_t v = i915_fence_value(0x100000, 0x10000, 512, 1);
	CHECK((v & 0xffffffffu) == (0x100000 | 1 | 2), "low word: start, valid, Y");
	CHECK((v >> 32) == (0x10f000u | 3), "high word: last page, pitch 512/128-1");
	v = i915_fence_value(0x100000, 0x10000, 512, 0);
	CHECK((v & 2) == 0, "X tiling has no Y bit");
	CHECK(i915_fence_value(0x100000, 0x10000, 100, 1) == 0, "a stride not a multiple of 128 is refused");
	CHECK(i915_fence_value(0x100000, 0x10000, 0, 1) == 0, "a zero stride is refused");
	CHECK(i915_fence_value(0x100800, 0x10000, 512, 1) == 0, "an unaligned start is refused");
	CHECK(i915_fence_value(0x100000, 0x10000, 4096 * 128, 1) != 0, "the largest stride fits");
	CHECK(i915_fence_value(0x100000, 0x10000, 4096 * 128 + 128, 1) == 0, "one more does not");
	/* Y tiles: 128 bytes by 32 rows, 16-byte columns */
	CHECK(i915_tiled_offset(0, 0, 512, 1) == 0, "origin");
	CHECK(i915_tiled_offset(15, 0, 512, 1) == 15, "first column, first row");
	CHECK(i915_tiled_offset(16, 0, 512, 1) == 512, "second column starts 512 in");
	CHECK(i915_tiled_offset(0, 1, 512, 1) == 16, "second row is 16 in");
	CHECK(i915_tiled_offset(127, 31, 512, 1) == 7 * 512 + 31 * 16 + 15, "last byte of the first tile");
	CHECK(i915_tiled_offset(128, 0, 512, 1) == 4096, "second tile of the row");
	CHECK(i915_tiled_offset(0, 32, 512, 1) == 4 * 4096, "second tile row (4 tiles per row)");
	/* X tiles: 512 bytes by 8 rows */
	CHECK(i915_tiled_offset(0, 1, 1024, 0) == 512, "X: second row is 512 in");
	CHECK(i915_tiled_offset(512, 0, 1024, 0) == 4096, "X: second tile of the row");
	CHECK(i915_tiled_offset(0, 8, 1024, 0) == 2 * 4096, "X: second tile row (2 per row)");
	if (fails) {
		printf("test-i915-fence: %d failure(s)\n", fails);
		return 1;
	}
	printf("test-i915-fence: all checks passed\n");
	return 0;
}
