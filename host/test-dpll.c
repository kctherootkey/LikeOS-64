/* Host test: the link-rate encoding of the display PLLs (appended to the
 * driver's own two functions by host/test-dpll.sh).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

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
	/* The three DisplayPort rates the port can be clocked at, and the
	 * three extra rates an embedded panel may use.  Each entry is half
	 * the link rate, which is what the field holds. */
	struct {
		unsigned rate;
		int code;
	} t[] = {
		{ 162000, DPLL_CTRL1_LINK_RATE_810 },
		{ 216000, DPLL_CTRL1_LINK_RATE_1080 },
		{ 270000, DPLL_CTRL1_LINK_RATE_1350 },
		{ 324000, DPLL_CTRL1_LINK_RATE_1620 },
		{ 432000, DPLL_CTRL1_LINK_RATE_2160 },
		{ 540000, DPLL_CTRL1_LINK_RATE_2700 },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		CHECK(link_rate_code(t[i].rate) == t[i].code,
		      "%u kHz encodes to %d, expected %d", t[i].rate,
		      link_rate_code(t[i].rate), t[i].code);
		CHECK(link_rate_of_code((unsigned)t[i].code) == t[i].rate,
		      "code %d decodes to %u, expected %u", t[i].code,
		      link_rate_of_code((unsigned)t[i].code), t[i].rate);
	}
	/* The panel of the machine this was written for: the firmware runs
	 * it at 2.7 GHz, which is the 1350 entry -- not the 2700 one, which
	 * would clock the port at 5.4 GHz. */
	CHECK(link_rate_code(270000) != DPLL_CTRL1_LINK_RATE_2700,
	      "2.7 GHz must not encode as the 2700 entry");
	CHECK(link_rate_of_code(DPLL_CTRL1_LINK_RATE_2700) == 540000,
	      "the 2700 entry is a 5.4 GHz link");

	/* Rates the hardware cannot be given. */
	CHECK(link_rate_code(810000) < 0, "8.1 GHz is not available here");
	CHECK(link_rate_code(243000) < 0, "2.43 GHz is not one of the six");
	CHECK(link_rate_code(0) < 0, "zero is rejected");
	CHECK(link_rate_of_code(7) == 0, "an unused code decodes to nothing");

	if (fails) {
		printf("dpll: %d failures\n", fails);
		return 1;
	}
	printf("dpll: all tests passed\n");
	return 0;
}
