/* Host test: the cache-control tables (appended to the driver's own
 * table code by host/test-mocs.sh). */
#include <stdio.h>

static int fails;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fails++;                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

int main(void)
{
	/* Gen9 has 64 entries; anything else has none yet. */
	CHECK(i915_mocs_entries(9) == 64, "gen9 entries %u", i915_mocs_entries(9));
	CHECK(i915_mocs_entries(8) == 0, "gen8 has no table yet");
	CHECK(i915_mocs_entries(11) == 0, "gen11 has no table yet");

	/* Entry 0 caches in neither the shared cache nor L3 (uncached, target
	 * both caches, no age); entry 1 defers to the page table with the
	 * longest age and L3 write-back; entry 2 is write-back everywhere. */
	CHECK(i915_mocs_control_value(9, 0) == 0x09, "entry 0 control %08x",
	      i915_mocs_control_value(9, 0));
	CHECK(i915_mocs_control_value(9, 1) == 0x38, "entry 1 control %08x",
	      i915_mocs_control_value(9, 1));
	CHECK(i915_mocs_control_value(9, 2) == 0x3b, "entry 2 control %08x",
	      i915_mocs_control_value(9, 2));
	/* Every entry the stack does not name behaves like entry 1, except
	 * the last, which the L3 uses for its evictions: write-back in the
	 * shared cache only, uncached in L3. */
	for (unsigned i = 3; i < 63; i++)
		CHECK(i915_mocs_control_value(9, i) == 0x38, "entry %u control %08x", i,
		      i915_mocs_control_value(9, i));
	CHECK(i915_mocs_control_value(9, 63) == 0x37, "entry 63 control %08x",
	      i915_mocs_control_value(9, 63));
	/* The L3 half: two entries a register, entry 0 uncached (1 << 4),
	 * the others write-back (3 << 4). */
	CHECK(i915_mocs_l3cc_value(9, 0) == 0x00300010, "L3 register 0 %08x",
	      i915_mocs_l3cc_value(9, 0));
	CHECK(i915_mocs_l3cc_value(9, 1) == 0x00300030, "L3 register 1 %08x",
	      i915_mocs_l3cc_value(9, 1));
	for (unsigned i = 2; i < 31; i++)
		CHECK(i915_mocs_l3cc_value(9, i) == 0x00300030, "L3 register %u %08x", i,
		      i915_mocs_l3cc_value(9, i));
	CHECK(i915_mocs_l3cc_value(9, 31) == 0x00100030, "L3 register 31 %08x",
	      i915_mocs_l3cc_value(9, 31));
	/* Nothing for a generation without a table. */
	CHECK(i915_mocs_control_value(8, 2) == 0 && i915_mocs_l3cc_value(8, 0) == 0,
	      "no values without a table");

	/* The table as register loads for a context's ring.  One load
	 * command whose length field counts 32 register/value pairs, then
	 * the pairs, then a filler so the total is even -- 66 words.  A
	 * length that disagrees with the pairs makes the engine read the
	 * next command out of the middle of this one. */
	static uint32_t ring[128];
	struct intel_device_info info = { 9 };
	struct i915_device dev = { &info };
	uint32_t n = i915_mocs_l3cc_emit(&dev, 0, ring, 128);

	CHECK(n == 66, "the table is 66 words, got %u", n);
	CHECK(ring[0] == ((0x22u << 23) | (2u * 32u - 1u)),
	      "the load command counts 32 pairs: %08x", ring[0]);
	CHECK(ring[1] == 0xb020 && ring[2] == 0x00300010, "the first pair: %08x %08x",
	      ring[1], ring[2]);
	CHECK(ring[63] == 0xb020 + 31 * 4 && ring[64] == 0x00100030,
	      "the last pair: %08x %08x", ring[63], ring[64]);
	CHECK(ring[65] == 0, "a no-op fills the odd word out: %08x", ring[65]);
	/* Nothing at all for another engine, or without room. */
	CHECK(i915_mocs_l3cc_emit(&dev, 1, ring, 128) == 0, "the copy engine carries none");
	CHECK(i915_mocs_l3cc_emit(&dev, 0, ring, 65) == 0, "nothing is emitted half-way");

	if (fails) {
		printf("test-mocs: %d failure(s)\n", fails);
		return 1;
	}
	printf("test-mocs: all checks passed\n");
	return 0;
}
