/* Host test: the mmap address selection in kernel/mm/mmap.c.
 *
 * The rule under test, stated apart from the kernel's plumbing: given the
 * region table, a mapping with no address of its own is placed at the
 * HIGHEST page-aligned start below the ceiling such that [start, start +
 * length) lies at or above the floor and overlaps no in-use record.  When
 * no such start exists the answer is 0.
 *
 * What it replaced was a cursor that only ever moved down and never looked
 * at the table, so after enough map/unmap churn it walked into the shared
 * libraries the loader had placed with MAP_FIXED and recorded new mappings
 * on top of them.  The third test below is that exact shape: a library, and
 * a 6.5 MB scratch mapped and unmapped two hundred thousand times.
 *
 * The code under test is cut out of the live kernel source by
 * host/test-mmap-gap.sh, so there is no copy to drift.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* The three fields the search reads; the kernel's record has many more. */
typedef struct {
	uint64_t start;
	uint64_t length;
	int in_use;
} mmap_region_t;
#define PAGE_SIZE 4096ULL

#include "mmap-gap-under-test.h"

#define NREG 512
static mmap_region_t tab[NREG];

static void clear(void)
{
	memset(tab, 0, sizeof(tab));
}

static int add(uint64_t start, uint64_t length)
{
	for (int i = 0; i < NREG; i++)
		if (!tab[i].in_use) {
			tab[i].start = start;
			tab[i].length = length;
			tab[i].in_use = 1;
			return i;
		}
	return -1;
}

static int overlaps_any(uint64_t start, uint64_t length)
{
	for (int i = 0; i < NREG; i++)
		/* A record of no length occupies nothing (the kernel's
		 * searches skip it the same way). */
		if (tab[i].in_use && tab[i].length &&
		    tab[i].start < start + length &&
		    start < tab[i].start + tab[i].length)
			return 1;
	return 0;
}

/* The specification, computed the slow way: walk down a page at a time. */
static uint64_t brute(uint64_t ceiling, uint64_t floor, uint64_t length)
{
	if (ceiling < floor || ceiling - floor < length)
		return 0;
	for (uint64_t s = ceiling - length;; s -= PAGE_SIZE) {
		if (!overlaps_any(s, length))
			return s;
		if (s < floor + PAGE_SIZE)
			return 0;
	}
}

static const uint64_t CEIL = 0x7fffffb00000ULL; /* USER_STACK_TOP - 4 MB */
static const uint64_t FLOOR = 0x2000000ULL;     /* a heap end + 4 MB */

int main(void)
{
	/* 1. Nothing mapped: right under the ceiling. */
	clear();
	CHECK(mmap_gap_search(tab, NREG, CEIL, FLOOR, 0x10000) ==
		      CEIL - 0x10000,
	      "empty table");

	/* 2. The top is taken: the next gap down. */
	clear();
	add(CEIL - 0x10000, 0x10000);
	CHECK(mmap_gap_search(tab, NREG, CEIL, FLOOR, 0x10000) ==
		      CEIL - 0x20000,
	      "below an occupied top");

	/* 3. The shape of the crash.  A 64 MB library placed by the loader,
	 * then a 6.5 MB scratch mapped and unmapped per frame for two hundred
	 * thousand frames -- an hour at 55 Hz.  The cursor this replaced
	 * spent 6.5 MB of address space per frame and reached the library
	 * after ~170k frames.  Here every placement must avoid the library,
	 * succeed, and -- because the range is freed each time -- land on
	 * the same top address again. */
	clear();
	const uint64_t lib = 0x7f0000000000ULL, libsz = 64ULL << 20;
	add(lib, libsz);
	const uint64_t scratch = 0x641000;
	uint64_t first = 0;
	int bad = 0;
	for (int frame = 0; frame < 200000 && bad < 3; frame++) {
		uint64_t a = mmap_gap_search(tab, NREG, CEIL, FLOOR, scratch);
		if (!a) {
			CHECK(0, "frame %d: no address", frame);
			bad++;
			break;
		}
		if (a < lib + libsz && lib < a + scratch) {
			CHECK(0, "frame %d: placed inside the library at %llx",
			      frame, (unsigned long long)a);
			bad++;
		}
		if (!first)
			first = a;
		else if (a != first) {
			CHECK(0, "frame %d: %llx, not the reused %llx", frame,
			      (unsigned long long)a, (unsigned long long)first);
			bad++;
		}
		int slot = add(a, scratch);
		tab[slot].in_use = 0; /* the frame's munmap */
	}
	/* And with fifty of them kept alive (a browser's other mappings). */
	for (int i = 0; i < 50; i++) {
		uint64_t a = mmap_gap_search(tab, NREG, CEIL, FLOOR, scratch);
		CHECK(a != 0, "live %d: no address", i);
		CHECK(!(a < lib + libsz && lib < a + scratch),
		      "live %d: inside the library", i);
		CHECK(!overlaps_any(a, scratch), "live %d: overlaps", i);
		add(a, scratch);
	}

	/* 4. The top candidate straddles two records: the answer is the gap
	 * below the LOWER of them, whichever the table lists first. */
	clear();
	const uint64_t x = CEIL - 0x10000;
	add(x + 0x8000, 0x1000); /* higher, found first in the table */
	add(x, 0x1000);          /* lower */
	uint64_t a = mmap_gap_search(tab, NREG, CEIL, x - 0x100000, 0x10000);
	CHECK(a == x - 0x10000, "straddle: got %llx want %llx",
	      (unsigned long long)a, (unsigned long long)(x - 0x10000));
	CHECK(!overlaps_any(a, 0x10000), "straddle: overlaps");

	/* 5. No room between floor and ceiling. */
	clear();
	CHECK(mmap_gap_search(tab, NREG, FLOOR + 0x8000, FLOOR, 0x10000) == 0,
	      "too small a window");
	CHECK(mmap_gap_search(tab, NREG, FLOOR, FLOOR + 0x1000, 0x1000) == 0,
	      "ceiling below floor");
	CHECK(mmap_gap_search(tab, NREG, CEIL, FLOOR, 0) == 0, "zero length");

	/* 6. A record BELOW the floor must neither be chosen past nor spin
	 * the search: the answer is the same as with an empty table. */
	clear();
	add(FLOOR - 0x8000, 0x4000);
	CHECK(mmap_gap_search(tab, NREG, CEIL, FLOOR, 0x10000) ==
		      CEIL - 0x10000,
	      "record below the floor");
	/* ...and one straddling the floor from below, with the window
	 * otherwise full, gives up rather than dipping under. */
	clear();
	add(FLOOR - 0x1000, 0x2000);
	add(FLOOR + 0x1000, CEIL - FLOOR - 0x1000);
	CHECK(mmap_gap_search(tab, NREG, CEIL, FLOOR, 0x2000) == 0,
	      "no dipping under the floor");

	/* 7. Against the specification: random unsorted tables in a small
	 * window, compared with the page-by-page walk. */
	srand(12345);
	const uint64_t sc = 0x400000, sf = 0x10000; /* 4 MB window */
	for (int trial = 0; trial < 3000; trial++) {
		clear();
		int n = rand() % 24;
		for (int i = 0; i < n; i++) {
			uint64_t s = sf - 0x8000 +
				     (uint64_t)(rand() % 0x400) * PAGE_SIZE;
			uint64_t l = (uint64_t)(1 + rand() % 12) * PAGE_SIZE;
			add(s, l);
		}
		uint64_t len = (uint64_t)(1 + rand() % 16) * PAGE_SIZE;
		uint64_t got = mmap_gap_search(tab, NREG, sc, sf, len);
		uint64_t want = brute(sc, sf, len);
		CHECK(got == want, "trial %d: got %llx want %llx (n=%d len=%llx)",
		      trial, (unsigned long long)got, (unsigned long long)want,
		      n, (unsigned long long)len);
	}

	/* 8. The ordered pass must give the SAME answer as the plain search
	 * -- it replaces it wherever the plain one would take more than a
	 * few walks -- and both must match the specification.  Same random
	 * tables as above, plus what the plain search meets in a real
	 * process: records straddling the ceiling, records above it, records
	 * under the floor, starts that are not page aligned (an executable's
	 * lazy ranges), zero-length and unused slots. */
	{
		static uint16_t order[NREG];

		for (int trial = 0; trial < 20000; trial++) {
			clear();
			int n = rand() % 40;
			int aligned = 1;
			for (int i = 0; i < n; i++) {
				uint64_t s = sf - 0x8000 +
					     (uint64_t)(rand() % 0x420) * PAGE_SIZE;
				uint64_t l = (uint64_t)(1 + rand() % 12) * PAGE_SIZE;

				if (rand() % 8 == 0) {
					s += 0x123; /* unaligned start */
					aligned = 0;
				}
				if (rand() % 16 == 0)
					l = 0;
				/* keep the table what the kernel keeps it:
				 * records do not overlap one another */
				if (l && overlaps_any(s, l))
					continue;
				int slot = add(s, l);
				if (slot >= 0 && rand() % 16 == 0)
					tab[slot].in_use = 0;
			}
			uint64_t len = (uint64_t)(1 + rand() % 16) * PAGE_SIZE;
			uint64_t plain = mmap_gap_search(tab, NREG, sc, sf, len);
			uint64_t sorted = mmap_gap_search_sorted(tab, NREG, sc,
								 sf, len, order);
			CHECK(sorted == plain,
			      "ordered trial %d: got %llx, plain search %llx (n=%d len=%llx)",
			      trial, (unsigned long long)sorted,
			      (unsigned long long)plain, n,
			      (unsigned long long)len);
			/* ...and the specification, which walks whole pages
			 * and so speaks only for page-aligned tables. */
			if (aligned)
				CHECK(sorted == brute(sc, sf, len),
				      "ordered trial %d: got %llx, specification %llx",
				      trial, (unsigned long long)sorted,
				      (unsigned long long)brute(sc, sf, len));
			if (sorted) {
				CHECK(!overlaps_any(sorted, len),
				      "ordered trial %d: %llx overlaps", trial,
				      (unsigned long long)sorted);
				CHECK(sorted >= sf && sorted + len <= sc,
				      "ordered trial %d: %llx outside the window",
				      trial, (unsigned long long)sorted);
				CHECK(mmap_gap_window_free(tab, NREG, sorted,
							   sorted + len),
				      "ordered trial %d: window check disagrees",
				      trial);
			}
			/* The bounded search: the plain answer when it
			 * finishes, and says so when it does not. */
			int gave_up = 0;
			uint64_t few = mmap_gap_search_steps(tab, NREG, sc, sf,
							     len, 4, &gave_up);
			CHECK(gave_up ? few == 0 : few == plain,
			      "bounded trial %d: got %llx gave_up %d, plain %llx",
			      trial, (unsigned long long)few, gave_up,
			      (unsigned long long)plain);
		}
		/* No scratch is an answer of 0, never a wild write. */
		clear();
		CHECK(mmap_gap_search_sorted(tab, NREG, CEIL, FLOOR, 0x10000,
					     NULL) == 0,
		      "ordered pass without scratch");
		/* Every slot in use: the order array is filled to the brim. */
		clear();
		for (int i = 0; i < NREG; i++)
			add(CEIL - (uint64_t)(i + 1) * 0x3000, 0x2000);
		CHECK(mmap_gap_search_sorted(tab, NREG, CEIL, FLOOR, 0x2000,
					     order) ==
			      mmap_gap_search(tab, NREG, CEIL, FLOOR, 0x2000),
		      "full table");
	}

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("test-mmap-gap: all checks passed\n");
	return 0;
}
