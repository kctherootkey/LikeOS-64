/* Host test: the ranges an address space has given out (appended to the
 * driver's own list logic by host/test-vma.sh).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
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

static unsigned count(struct i915_vma *head)
{
	unsigned n = 0;
	for (; head; head = head->vm_next)
		n++;
	return n;
}

int main(void)
{
	struct i915_vma *head = NULL;
	struct i915_vma a, b, c;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	memset(&c, 0, sizeof(c));
	a.addr = 0x1000; a.npages = 4; /* [0x1000, 0x5000) */
	b.addr = 0x8000; b.npages = 2; /* [0x8000, 0xa000) */
	i915_vma_list_attach(&head, &a);
	i915_vma_list_attach(&head, &b);
	CHECK(a.attached && b.attached && count(head) == 2, "two bindings attached");

	/* A range nothing holds takes nothing. */
	CHECK(i915_vma_list_supersede(&head, 0x5000, 3, NULL) == 0, "the gap is free");
	CHECK(i915_vma_list_supersede(&head, 0xa000, 1, NULL) == 0, "after b is free");
	CHECK(count(head) == 2, "nothing lost");

	/* The client hands a's range to a new object c: a is superseded,
	 * b untouched. */
	c.addr = 0x2000; c.npages = 1;
	CHECK(i915_vma_list_supersede(&head, c.addr, c.npages, &c) == 1, "a taken over");
	i915_vma_list_attach(&head, &c);
	CHECK(!a.attached, "a no longer owns its range");
	CHECK(b.attached && c.attached && count(head) == 2, "b and c own theirs");
	/* a's teardown must not clear anything now */
	CHECK(i915_vma_list_detach(&head, &a) == 0, "a's release clears no translations");
	CHECK(count(head) == 2, "a was not on the list to remove");
	/* c's teardown does */
	CHECK(i915_vma_list_detach(&head, &c) == 1, "c's release clears its translations");
	CHECK(count(head) == 1 && head == &b, "only b left");

	/* A binding never takes itself over. */
	CHECK(i915_vma_list_supersede(&head, b.addr, b.npages, &b) == 0, "b keeps its range");
	CHECK(b.attached, "b still attached");
	/* Overlap at the edges: one page in, both sides. */
	CHECK(i915_vma_list_supersede(&head, 0x7000, 2, NULL) == 1, "an overlap of one page counts");
	CHECK(!b.attached && count(head) == 0, "b gone");
	i915_vma_list_attach(&head, &b);
	CHECK(i915_vma_list_supersede(&head, 0x9000, 4, NULL) == 1, "an overlap at the end counts");
	CHECK(count(head) == 0, "list empty");

	/* The gap finder: bindings at [0x2000,0x4000) and [0x8000,0xa000);
	 * the first page-sized gap from 0 is 0, from 0x2000 it is 0x4000,
	 * a 4-page gap from 0x2000 lands after both, 64 KB alignment hops
	 * accordingly, and a limit is honoured. */
	a.addr = 0x2000; a.npages = 2; b.addr = 0x8000; b.npages = 2;
	head = NULL;
	i915_vma_list_attach(&head, &a);
	i915_vma_list_attach(&head, &b);
	CHECK(i915_vma_list_find_gap(head, 0, 0x100000, 1, 4096) == 0, "gap at zero is a gap (reported as none)");
	CHECK(i915_vma_list_find_gap(head, 0x1000, 0x100000, 1, 4096) == 0x1000, "one page before a");
	CHECK(i915_vma_list_find_gap(head, 0x2000, 0x100000, 1, 4096) == 0x4000, "hops over a");
	CHECK(i915_vma_list_find_gap(head, 0x2000, 0x100000, 4, 4096) == 0x4000, "four pages fit between a and b");
	CHECK(i915_vma_list_find_gap(head, 0x2000, 0x100000, 5, 4096) == 0xa000, "five pages land after b");
	CHECK(i915_vma_list_find_gap(head, 0x2000, 0x100000, 1, 0x10000) == 0x10000, "64 KB alignment");
	CHECK(i915_vma_list_find_gap(head, 0x2000, 0x9000, 5, 4096) == 0, "limit refuses");
	CHECK(i915_vma_list_find_gap(head, 0x2000, 0x100000, 1, 0x3000) == 0, "a non-power-of-two alignment is refused");
	CHECK(i915_vma_list_find_gap(NULL, 0x7000, 0x100000, 3, 4096) == 0x7000, "an empty list gives the start");
	i915_vma_list_detach(&head, &a);
	i915_vma_list_detach(&head, &b);

	if (fails) {
		printf("test-vma: %d failure(s)\n", fails);
		return 1;
	}
	printf("test-vma: all checks passed\n");
	return 0;
}
