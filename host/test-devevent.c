/*
 * Tests for the device event ring behind /dev/devevents
 * (kernel/fs/devevent_ring.h): sequence numbers, readers that start at the
 * tail, batching, the wrap of the ring, and the single OVERFLOW record a
 * reader gets when it fell behind.  See test-devevent.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

/* The kernel's fixed-width types come first and stand in for stdint.h. */
#include <kernel/fs/devevent_ring.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, ...)                                                  \
	do {                                                              \
		checks++;                                                 \
		if (!(cond)) {                                            \
			fails++;                                          \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
			printf(__VA_ARGS__);                              \
			printf("\n");                                     \
		}                                                         \
	} while (0)

static struct devevent_ring ring; /* zeroed, as the kernel's (bss) */

static void publish(uint32_t id)
{
	struct device_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.size = sizeof(ev);
	ev.version = DEVEVENT_VERSION;
	ev.type = DEVICE_EVENT_ADD;
	ev.device_class = DEVICE_CLASS_PCI;
	ev.device_id = id;
	ev.seq = 0xdeadbeef; /* must be replaced */
	snprintf(ev.name, sizeof(ev.name), "dev%u", id);
	devevent_ring_put(&ring, &ev);
}

static int all_zero(const void *p, size_t n)
{
	const unsigned char *c = p;

	for (size_t i = 0; i < n; i++)
		if (c[i])
			return 0;
	return 1;
}

int main(void)
{
	struct device_event out[600];
	uint64_t cur, next;
	unsigned n;

	CHECK(sizeof(struct device_event) == 128, "record size");

	/* Empty ring: a reader at the tail has nothing. */
	cur = devevent_ring_tail(&ring);
	CHECK(cur == 1, "first tail %llu", (unsigned long long)cur);
	CHECK(!devevent_ring_pending(&ring, cur), "pending on empty ring");
	n = devevent_ring_take(&ring, cur, out, 8, 0, &next);
	CHECK(n == 0 && next == cur, "take on empty ring: %u", n);

	/* Three events: seq 1..3 in order. */
	publish(10);
	publish(11);
	publish(12);
	CHECK(devevent_ring_pending(&ring, cur), "not pending after publish");
	n = devevent_ring_take(&ring, cur, out, 8, 0, &next);
	CHECK(n == 3, "took %u, want 3", n);
	for (unsigned i = 0; i < n; i++) {
		CHECK(out[i].seq == 1 + i, "seq %llu at %u",
		      (unsigned long long)out[i].seq, i);
		CHECK(out[i].device_id == 10 + i, "id at %u", i);
	}
	CHECK(next == 4, "next cursor %llu", (unsigned long long)next);
	cur = next;
	CHECK(!devevent_ring_pending(&ring, cur), "pending after reading all");

	/* A reader opened now starts after what exists. */
	uint64_t late = devevent_ring_tail(&ring);
	CHECK(late == 4, "late reader tail %llu", (unsigned long long)late);
	CHECK(!devevent_ring_pending(&ring, late), "late reader sees old events");

	/* Batching: a buffer of 2 gets 2, the rest stays. */
	publish(13);
	publish(14);
	publish(15);
	n = devevent_ring_take(&ring, cur, out, 2, 0, &next);
	CHECK(n == 2 && out[0].seq == 4 && out[1].seq == 5 && next == 6,
	      "batch of 2: n=%u first=%llu next=%llu", n,
	      (unsigned long long)out[0].seq, (unsigned long long)next);
	/* A reader that did not commit (copy fault) can take them again. */
	n = devevent_ring_take(&ring, cur, out, 8, 0, &next);
	CHECK(n == 3 && out[0].seq == 4 && next == 7, "retake n=%u", n);
	cur = next;

	/* Exactly a full ring behind: no overflow. */
	uint64_t full = cur;
	for (unsigned i = 0; i < DEVEVENT_RING_SIZE; i++)
		publish(1000 + i);
	CHECK(devevent_ring_oldest(&ring) == full, "oldest %llu want %llu",
	      (unsigned long long)devevent_ring_oldest(&ring),
	      (unsigned long long)full);
	n = devevent_ring_take(&ring, full, out, 600, 0, &next);
	CHECK(n == DEVEVENT_RING_SIZE, "full ring took %u", n);
	CHECK(out[0].type == DEVICE_EVENT_ADD && out[0].seq == full,
	      "full ring first record type %u seq %llu", out[0].type,
	      (unsigned long long)out[0].seq);
	CHECK(out[n - 1].seq == full + DEVEVENT_RING_SIZE - 1, "full ring last");
	for (unsigned i = 1; i < n; i++)
		CHECK(out[i].seq == out[i - 1].seq + 1, "gap at %u", i);

	/* One more: that reader lost exactly one record. */
	publish(5000);
	n = devevent_ring_take(&ring, full, out, 600, 777, &next);
	CHECK(n == DEVEVENT_RING_SIZE + 1, "overflow take %u", n);
	CHECK(out[0].type == DEVICE_EVENT_OVERFLOW, "no overflow record");
	CHECK(out[0].seq == full, "overflow seq %llu want %llu",
	      (unsigned long long)out[0].seq, (unsigned long long)full);
	CHECK(out[0].size == 128 && out[0].version == DEVEVENT_VERSION &&
		      out[0].timestamp_ns == 777,
	      "overflow header");
	CHECK(out[0].device_class == 0 && out[0].device_id == 0 &&
		      out[0].parent_id == 0 && out[0].flags == 0 &&
		      all_zero(out[0].name, sizeof(out[0].name)) &&
		      all_zero(out[0].reserved, sizeof(out[0].reserved)),
	      "overflow record not zeroed");
	CHECK(out[1].seq == full + 1, "after overflow continues at %llu",
	      (unsigned long long)out[1].seq);
	CHECK(next == ring.next_seq, "cursor after overflow");

	/* Overflow with a tiny buffer: ONE overflow record, and the cursor
	 * moves to the oldest kept, so the next read does not repeat it. */
	n = devevent_ring_take(&ring, full, out, 1, 5, &next);
	CHECK(n == 1 && out[0].type == DEVICE_EVENT_OVERFLOW,
	      "tiny overflow n=%u type=%u", n, out[0].type);
	CHECK(next == devevent_ring_oldest(&ring), "tiny overflow cursor");
	uint64_t c2 = next;
	n = devevent_ring_take(&ring, c2, out, 1, 5, &next);
	CHECK(n == 1 && out[0].type == DEVICE_EVENT_ADD && out[0].seq == c2,
	      "after tiny overflow: type %u seq %llu", out[0].type,
	      (unsigned long long)out[0].seq);

	/* Far behind (many laps): still one overflow record. */
	for (unsigned i = 0; i < 5 * DEVEVENT_RING_SIZE + 3; i++)
		publish(i);
	n = devevent_ring_take(&ring, 1, out, 600, 0, &next);
	CHECK(n == DEVEVENT_RING_SIZE + 1, "laps take %u", n);
	CHECK(out[0].type == DEVICE_EVENT_OVERFLOW && out[0].seq == 1,
	      "laps overflow");
	unsigned ovf = 0;
	for (unsigned i = 0; i < n; i++)
		ovf += out[i].type == DEVICE_EVENT_OVERFLOW;
	CHECK(ovf == 1, "%u overflow records", ovf);
	CHECK(out[n - 1].seq == ring.next_seq - 1, "laps last seq");
	for (unsigned i = 2; i < n; i++)
		CHECK(out[i].seq == out[i - 1].seq + 1, "laps gap at %u", i);

	/* Two independent readers. */
	uint64_t a = devevent_ring_tail(&ring), b = a;
	publish(1);
	publish(2);
	n = devevent_ring_take(&ring, a, out, 1, 0, &a);
	CHECK(n == 1 && out[0].device_id == 1, "reader a first");
	n = devevent_ring_take(&ring, b, out, 8, 0, &b);
	CHECK(n == 2 && out[0].device_id == 1 && out[1].device_id == 2,
	      "reader b both");
	n = devevent_ring_take(&ring, a, out, 8, 0, &a);
	CHECK(n == 1 && out[0].device_id == 2, "reader a second");
	CHECK(a == b && !devevent_ring_pending(&ring, a), "readers end equal");

	/* max == 0 takes nothing, even when overflowed. */
	n = devevent_ring_take(&ring, 1, out, 0, 0, &next);
	CHECK(n == 0 && next == 1, "max 0: n=%u next=%llu", n,
	      (unsigned long long)next);

	/* Names survive the ring unchanged. */
	publish(42);
	n = devevent_ring_take(&ring, a, out, 1, 0, &next);
	CHECK(n == 1 && !strcmp(out[0].name, "dev42"), "name '%s'",
	      out[0].name);

	printf("%d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
