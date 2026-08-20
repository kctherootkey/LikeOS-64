/*
 * test-search.c - check libc's <search.h> against the host's glibc.
 *
 * The tree is the reason this exists.  tsearch's interface allows a plain
 * binary search tree, and a plain one becomes a linked list the moment the
 * keys arrive in order -- which is the common case, not a corner one.  So the
 * implementation balances, and balancing code has rotations, and a rotation
 * that is subtly wrong still produces a tree that answers most lookups
 * correctly.  Testing "does tfind find it" would not catch that; what is
 * checked here is the ORDER a walk produces, the HEIGHT the tree reaches, and
 * that both survive tens of thousands of insertions and deletions.
 *
 * The hash table is checked the same way: not that a lookup works, but that a
 * table filled to its stated capacity still finds every key -- which is where
 * a probe sequence that fails to cover the table shows up.
 *
 * The two implementations coexist by the same trick as the other host tests:
 * search.c is compiled on its own and objcopy renames every symbol it defines
 * or calls, so glibc's tsearch and this one can be called from one program.
 */
/* tdestroy is a GNU extension and the host header hides it otherwise. */
#define _GNU_SOURCE
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The renamed copies. */
void *lk_tsearch(const void *, void **, int (*)(const void *, const void *));
void *lk_tfind(const void *, void *const *, int (*)(const void *, const void *));
void *lk_tdelete(const void *, void **, int (*)(const void *, const void *));
void lk_twalk(const void *, void (*)(const void *, VISIT, int));
void lk_tdestroy(void *, void (*)(void *));
int lk_hcreate(size_t);
void lk_hdestroy(void);
ENTRY *lk_hsearch(ENTRY, ACTION);
void *lk_lfind(const void *, const void *, size_t *, size_t,
	       int (*)(const void *, const void *));
void *lk_lsearch(const void *, void *, size_t *, size_t,
		 int (*)(const void *, const void *));
void lk_insque(void *, void *);
void lk_remque(void *);

static int failures;

static void fail(const char *what, const char *detail)
{
	if (++failures <= 20)
		printf("  FAIL %s: %s\n", what, detail);
}

/* glibc's tdestroy calls the free function without checking it for NULL, so
 * the reference tree is freed with a no-op rather than with NULL.  (This
 * libc's tdestroy does check, and accepts NULL.) */
static void no_free(void *p)
{
	(void)p;
}

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

/* Collected by the walk below, to compare against the reference. */
static int walked[200000];
static int nwalked;
static int max_depth;

static void collect(const void *node, VISIT which, int depth)
{
	if (which == postorder || which == leaf) {
		walked[nwalked++] = **(int *const *)node;
		if (depth > max_depth)
			max_depth = depth;
	}
}

/* ceil(log2(n)) + 1, the tightest bound a balanced tree could meet. */
static int log2ceil(int n)
{
	int b = 0;

	while ((1 << b) < n)
		b++;
	return b;
}

static void test_tree(const char *what, int n, int sorted)
{
	void *mine = NULL, *ref = NULL;
	int *keys = malloc((size_t)n * sizeof *keys);
	int i;

	for (i = 0; i < n; i++)
		keys[i] = sorted ? i : (int)((unsigned)i * 2654435761u % 1000003u);

	for (i = 0; i < n; i++) {
		if (!lk_tsearch(&keys[i], &mine, cmp_int))
			fail(what, "tsearch returned NULL");
		tsearch(&keys[i], &ref, cmp_int);
	}

	/* Every key is findable, in both. */
	for (i = 0; i < n; i++) {
		void *a = lk_tfind(&keys[i], &mine, cmp_int);
		void *b = tfind(&keys[i], &ref, cmp_int);

		if (!a != !b) {
			fail(what, "tfind disagrees with the reference");
			break;
		}
		if (a && **(int **)a != keys[i]) {
			fail(what, "tfind returned the wrong key");
			break;
		}
	}

	/* A key that was never inserted is not found. */
	{
		int absent = -12345;

		if (lk_tfind(&absent, &mine, cmp_int))
			fail(what, "tfind found a key that is not there");
	}

	/* In-order traversal must be sorted -- this is what proves the
	 * rotations preserved the ordering, which "does tfind work" does
	 * not. */
	nwalked = 0;
	max_depth = 0;
	lk_twalk(mine, collect);
	if (nwalked != n) {
		char buf[128];

		snprintf(buf, sizeof buf, "walk visited %d of %d nodes",
			 nwalked, n);
		fail(what, buf);
	}
	for (i = 1; i < nwalked; i++) {
		if (walked[i - 1] >= walked[i]) {
			fail(what, "walk is not in order");
			break;
		}
	}

	/* And the tree is actually balanced.  An AVL tree's height is at most
	 * 1.44*log2(n+2); the bound below is looser than that and still far
	 * under what an unbalanced tree reaches -- inserting 20,000 sorted
	 * keys into a plain BST gives a depth of 20,000. */
	{
		int bound = 2 * log2ceil(n + 2) + 2;

		if (max_depth > bound) {
			char buf[128];

			snprintf(buf, sizeof buf,
				 "depth %d exceeds %d for %d keys (unbalanced)",
				 max_depth, bound, n);
			fail(what, buf);
		}
	}

	/* Delete half the keys and check what is left, both for presence and
	 * for order -- deletion rebalances too, and its rotations are a
	 * separate piece of code from insertion's. */
	for (i = 0; i < n; i += 2) {
		void *a = lk_tdelete(&keys[i], &mine, cmp_int);

		if (!a)
			fail(what, "tdelete did not find a key that is there");
	}
	for (i = 0; i < n; i += 2) {
		if (lk_tfind(&keys[i], &mine, cmp_int))
			fail(what, "a deleted key is still findable");
	}
	for (i = 1; i < n; i += 2) {
		if (!lk_tfind(&keys[i], &mine, cmp_int)) {
			fail(what, "deletion lost a key it should have kept");
			break;
		}
	}
	nwalked = 0;
	max_depth = 0;
	lk_twalk(mine, collect);
	for (i = 1; i < nwalked; i++) {
		if (walked[i - 1] >= walked[i]) {
			fail(what, "walk is not in order after deletion");
			break;
		}
	}
	{
		int left = nwalked;
		int bound = 2 * log2ceil(left + 2) + 2;

		if (left > 0 && max_depth > bound)
			fail(what, "unbalanced after deletion");
	}

	/* Deleting everything empties the tree. */
	for (i = 1; i < n; i += 2)
		lk_tdelete(&keys[i], &mine, cmp_int);
	if (mine != NULL)
		fail(what, "tree is not empty after deleting every key");

	tdestroy(ref, no_free);
	free(keys);
}

static void test_hash(void)
{
	enum { N = 500 };
	static char keys[N][16];
	int i;

	if (!lk_hcreate(N)) {
		fail("hcreate", "returned 0");
		return;
	}
	for (i = 0; i < N; i++) {
		ENTRY e;

		snprintf(keys[i], sizeof keys[i], "key%d", i);
		e.key = keys[i];
		e.data = (void *)(long)i;
		if (!lk_hsearch(e, ENTER)) {
			fail("hsearch ENTER", keys[i]);
			break;
		}
	}
	/* Every key is still findable with the table at its stated capacity.
	 * This is what a probe sequence that cannot reach every slot fails. */
	for (i = 0; i < N; i++) {
		ENTRY q = { keys[i], NULL };
		ENTRY *r = lk_hsearch(q, FIND);

		if (!r) {
			fail("hsearch FIND", keys[i]);
			break;
		}
		if ((long)r->data != i) {
			fail("hsearch FIND", "wrong data");
			break;
		}
	}
	/* Re-entering an existing key returns the existing entry rather than
	 * adding a second one. */
	{
		ENTRY e = { keys[0], (void *)999L };
		ENTRY *r = lk_hsearch(e, ENTER);

		if (!r || (long)r->data != 0)
			fail("hsearch ENTER existing",
			     "replaced the data instead of returning it");
	}
	/* A key that is not there is not found. */
	{
		char absent[] = "nope";
		ENTRY q = { absent, NULL };

		if (lk_hsearch(q, FIND))
			fail("hsearch FIND", "found a key that is not there");
	}
	lk_hdestroy();
}

static void test_linear(void)
{
	int arr[16];
	size_t n = 0;
	int i;

	for (i = 0; i < 8; i++) {
		int v = i * 3;

		if (!lk_lsearch(&v, arr, &n, sizeof v, cmp_int))
			fail("lsearch", "returned NULL");
	}
	if (n != 8)
		fail("lsearch", "did not append eight elements");

	/* lsearch on a key already present must NOT append. */
	{
		int v = 3;

		lk_lsearch(&v, arr, &n, sizeof v, cmp_int);
		if (n != 8)
			fail("lsearch", "appended a key that was already there");
	}
	for (i = 0; i < 8; i++) {
		int v = i * 3;

		if (!lk_lfind(&v, arr, &n, sizeof v, cmp_int))
			fail("lfind", "did not find a key that is there");
	}
	{
		int v = 1;

		if (lk_lfind(&v, arr, &n, sizeof v, cmp_int))
			fail("lfind", "found a key that is not there");
	}
}

struct qe {
	struct qe *forw;
	struct qe *back;
	int v;
};

static void test_queue(void)
{
	struct qe a = { 0, 0, 1 }, b = { 0, 0, 2 }, c = { 0, 0, 3 };
	struct qe *p;
	int seen[3], n = 0;

	lk_insque(&a, NULL); /* start the list */
	lk_insque(&b, &a);   /* a b */
	lk_insque(&c, &a);   /* a c b */

	for (p = &a; p; p = p->forw)
		if (n < 3)
			seen[n++] = p->v;
	if (n != 3 || seen[0] != 1 || seen[1] != 3 || seen[2] != 2)
		fail("insque", "list is not a c b");

	lk_remque(&c);
	n = 0;
	for (p = &a; p; p = p->forw)
		if (n < 3)
			seen[n++] = p->v;
	if (n != 2 || seen[0] != 1 || seen[1] != 2)
		fail("remque", "did not unlink the middle element");
}

int main(void)
{
	printf("tree: random keys\n");
	test_tree("tree/random", 20000, 0);

	/* The case that separates a balanced tree from a list. */
	printf("tree: sorted keys (the case a plain BST degenerates on)\n");
	test_tree("tree/sorted", 20000, 1);

	printf("tree: small\n");
	test_tree("tree/small", 1, 0);
	test_tree("tree/small", 2, 0);
	test_tree("tree/small", 3, 1);

	printf("hash table\n");
	test_hash();

	printf("lsearch / lfind\n");
	test_linear();

	printf("insque / remque\n");
	test_queue();

	printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
