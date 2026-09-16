/* Host test: the address form of the submission interface (appended to
 * the driver's own two helpers by host/test-execaddr.sh).
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
	const uint64_t size = 1ULL << 48;

	/* The lower half of the space passes through untouched. */
	CHECK(addr_from_user(0x1000) == 0x1000, "a low address is itself");
	CHECK(addr_to_user(0x1000) == 0x1000, "and stays itself on the way out");
	CHECK(addr_from_user(0x0000300000000000ULL) == 0x0000300000000000ULL,
	      "48 TB is below the sign bit");

	/* The upper half arrives with every top bit set.  This is where the
	 * graphics driver of the display server puts most of its buffers. */
	uint64_t high = 0xFFFFFFFFFFFFF000ULL; /* the last page of the space */
	CHECK(addr_from_user(high) == 0x0000FFFFFFFFF000ULL, "the sign comes off: %llx",
	      (unsigned long long)addr_from_user(high));
	CHECK(addr_from_user(high) + 4096 <= size, "and the result fits the space");
	CHECK(addr_to_user(addr_from_user(high)) == high, "and goes back on unchanged");

	/* A buffer just above the halfway mark: bit 47 set, so the top bits
	 * are set too. */
	uint64_t mid = 0xFFFF800000000000ULL;
	CHECK(addr_from_user(mid) == 0x0000800000000000ULL, "halfway: %llx",
	      (unsigned long long)addr_from_user(mid));
	CHECK(addr_to_user(0x0000800000000000ULL) == mid, "and back");

	/* The address the display server's graphics driver actually uses
	 * first: the top of its largest zone, four gigabytes below the end
	 * of a 48-bit space. */
	uint64_t top_zone = (1ULL << 48) - (4ULL << 30) - 0x10000;
	CHECK((top_zone & (1ULL << 47)) != 0, "it is in the upper half");
	CHECK(addr_from_user(addr_to_user(top_zone)) == top_zone,
	      "a round trip leaves it alone");
	CHECK(addr_from_user(addr_to_user(top_zone)) + 0x10000 <= size,
	      "and it fits the space with room for the buffer");

	/* Nothing below the sign bit gains top bits. */
	for (int b = 0; b < 47; b++) {
		uint64_t a = 1ULL << b;
		CHECK(addr_to_user(a) == a, "bit %d must not sign-extend", b);
	}

	if (fails) {
		printf("execaddr: %d failures\n", fails);
		return 1;
	}
	printf("execaddr: all tests passed\n");
	return 0;
}
