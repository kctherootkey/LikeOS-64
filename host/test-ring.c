/* Host test: the ring reservation rule of kernel/dev/gpu/i915/i915_request.c.
 *
 * The rule under test, stated independently of the driver's plumbing:
 * with a head H (the point the engine has read up to) and a tail T (the
 * point the driver has written up to), a reservation of N bytes may only
 * use space that is not in [H, T), may not split a command across the
 * end of the ring, and may never leave T equal to H, which the engine
 * reads as an empty ring.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdint.h>
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

#define RING_SIZE 32768

struct ring {
	uint32_t head, tail, size;
};

/* The driver's rule, transcribed. Returns 0 when the space is there
 * (wrapping the tail if it had to), non-zero when the caller must wait
 * for the engine to retire work first. */
static int reserve(struct ring *r, uint32_t bytes)
{
	uint32_t need = (bytes + 7) & ~7u;
	uint32_t head = r->head, tail = r->tail;

	if (tail >= head) {
		uint32_t to_end = r->size - tail;
		if (to_end > need + (head == 0 ? 8u : 0u))
			return 0;
		if (head > need + 8) {
			r->tail = 0;
			return 0;
		}
	} else if (head - tail > need + 8) {
		return 0;
	}
	return -1;
}

/* Is [from, from+len) free, given the engine has read up to head? */
static int range_is_free(uint32_t head, uint32_t tail, uint32_t from, uint32_t len,
			 uint32_t size)
{
	for (uint32_t i = 0; i < len; i++) {
		uint32_t at = (from + i) % size;
		int pending = (tail >= head) ? (at >= head && at < tail)
					     : (at >= head || at < tail);
		if (pending)
			return 0;
	}
	return 1;
}

int main(void)
{
	struct ring r = { 0, 0, RING_SIZE };

	/* An empty ring takes a request. */
	CHECK(reserve(&r, 128) == 0, "an empty ring has room");

	/* Fill it without ever retiring: the head stays at the start, so
	 * once the end is reached there is nowhere to go and the caller
	 * must wait.  This is the case the driver got wrong: it wrapped
	 * onto commands the engine had not read. */
	r.head = 0;
	r.tail = 0;
	int wrote = 0;
	while (reserve(&r, 128) == 0) {
		uint32_t before = r.tail;
		CHECK(range_is_free(r.head, r.tail, r.tail, 128, r.size),
		      "the space handed out at %u is not pending work", before);
		r.tail += 128;
		if (++wrote > RING_SIZE / 128 + 4)
			break;
	}
	CHECK(wrote <= RING_SIZE / 128,
	      "a ring that never retires must fill and then refuse, wrote %d", wrote);
	CHECK(reserve(&r, 128) != 0, "and refuse once full");

	/* Retire half of it: the head moves, and the tail may now wrap. */
	r.head = RING_SIZE / 2;
	CHECK(reserve(&r, 128) == 0, "space appears once the engine has read half");
	CHECK(r.tail == 0, "and the tail wrapped to the start, at %u", r.tail);
	CHECK(range_is_free(r.head, r.tail, 0, 128, r.size), "the start is free to write");

	/* With the tail behind the head, the gap between them is the limit. */
	r.head = 1000;
	r.tail = 900;
	CHECK(reserve(&r, 64) == 0, "a 100 byte gap takes a 64 byte request... ");
	r.head = 1000;
	r.tail = 900;
	CHECK(reserve(&r, 96) != 0, "...but not one that would meet the head");

	/* The tail must never land exactly on the head. */
	r.head = 0;
	r.tail = RING_SIZE - 128;
	CHECK(reserve(&r, 120) != 0,
	      "filling to the very end with the head at the start is refused");

	/* A request is never split across the end. */
	r.head = 4096;
	r.tail = RING_SIZE - 64;
	CHECK(reserve(&r, 128) == 0, "a request that does not fit before the end wraps");
	CHECK(r.tail == 0, "to the start, at %u", r.tail);

	/* Steady state: submit and retire in lockstep, many laps. */
	r.head = 0;
	r.tail = 0;
	uint32_t retired = 0;
	for (int i = 0; i < 10000; i++) {
		if (reserve(&r, 96) != 0) {
			/* the engine catches up */
			r.head = r.tail;
			retired++;
			continue;
		}
		CHECK(range_is_free(r.head, r.tail, r.tail, 96, r.size),
		      "lap %d handed out pending space at %u", i, r.tail);
		r.tail += 96;
		if (r.tail > r.size) {
			printf("FAIL: tail %u past the end\n", r.tail);
			fails++;
			break;
		}
		if ((i % 7) == 0)
			r.head = r.tail; /* work completes */
	}
	CHECK(retired < 10000, "the ring kept flowing");

	if (fails) {
		printf("ring: %d failures\n", fails);
		return 1;
	}
	printf("ring: all tests passed\n");
	return 0;
}
