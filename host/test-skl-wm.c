/* Host test: the display watermark levels (appended to the driver's own
 * computation by host/test-skl-wm.sh). */
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
	/* The panel of the machine this was written for: 1920x1080 at
	 * 142 MHz, 2100 total columns, and the first memory latency the
	 * firmware reports, 2 us. */
	const uint32_t rate = 142000, htotal = 2100, width = 1920;
	const uint32_t latency = 2;

	struct skl_wm lin = skl_wm_level(latency, rate, htotal, width, 4, 0, 0);
	struct skl_wm tiled = skl_wm_level(latency, rate, htotal, width, 4, 1, 0);
	struct skl_wm xtiled = skl_wm_level(latency, rate, htotal, width, 4, 0, 1);

	printf("  linear: %u blocks, %u lines; X tiled: %u blocks, %u lines; Y tiled: %u blocks, %u lines\n",
	       lin.blocks, lin.lines, xtiled.blocks, xtiled.lines, tiled.blocks, tiled.lines);

	CHECK(lin.valid && tiled.valid, "both levels are usable");
	/* A linear surface needs a fraction of a line buffered. */
	CHECK(lin.blocks >= 16, "a linear line of 1920 pixels is 16 blocks, got %u",
	      lin.blocks);
	CHECK(lin.lines <= 2, "linear lines %u", lin.lines);
	/* The layout the display server uses on this machine.  Its lines
	 * are whole blocks, so one line is fifteen -- and never the four
	 * the bare latency arithmetic produces, which starves the plane
	 * and underruns every line. */
	CHECK(xtiled.valid, "the X tiled level is usable");
	CHECK(xtiled.blocks == 15, "an X tiled line of 1920 pixels is 15 blocks, got %u",
	      xtiled.blocks);
	CHECK(xtiled.blocks > 4, "and must not be the four the latency alone gives");
	/* A tiled one is fetched four scan lines at a time: 1920 pixels of
	 * four bytes is fifteen blocks a line, so four lines plus the same
	 * again as the tile minimum. */
	CHECK(tiled.blocks >= 100, "a tiled plane needs far more blocks, got %u",
	      tiled.blocks);
	CHECK(tiled.blocks > lin.blocks * 4,
	      "a Y tiled plane (%u) needs far more than a linear one (%u); computing it with the linear formula is the bug this pins",
	      tiled.blocks, lin.blocks);
	CHECK(tiled.lines >= 4, "a tiled plane buffers whole tile rows, got %u lines",
	      tiled.lines);

	/* Both must fit the buffer a pipe gets. */
	CHECK(tiled.blocks < 888, "and still fit the data buffer (%u)", tiled.blocks);

	/* The cursor: 64 pixels wide, linear, tiny. */
	struct skl_wm cur = skl_wm_level(latency, rate, htotal, 64, 4, 0, 0);
	CHECK(cur.valid && cur.blocks <= 8, "cursor %u blocks", cur.blocks);

	/* Sixteen-bit colour halves the bytes per line. */
	struct skl_wm t16 = skl_wm_level(latency, rate, htotal, width, 2, 1, 0);
	CHECK(t16.valid, "16-bit tiled is usable");
	CHECK(t16.blocks >= 100, "16-bit tiled blocks %u", t16.blocks);

	/* Degenerate inputs are refused rather than programmed. */
	struct skl_wm bad = skl_wm_level(0, rate, htotal, width, 4, 1, 0);
	CHECK(!bad.valid, "no latency means no usable level");
	bad = skl_wm_level(latency, 0, htotal, width, 4, 1, 0);
	CHECK(!bad.valid, "no pixel rate means no usable level");

	/* A very wide tiled surface exceeds what one pipe's buffer holds,
	 * and must report itself unusable rather than silently truncate. */
	struct skl_wm huge = skl_wm_level(30, rate, htotal, 7680, 4, 1, 0);
	CHECK(huge.blocks > 400 || !huge.valid, "a huge tiled surface is demanding (%u, valid %d)",
	      huge.blocks, huge.valid);

	if (fails) {
		printf("skl watermarks: %d failures\n", fails);
		return 1;
	}
	printf("skl watermarks: all tests passed\n");
	return 0;
}
