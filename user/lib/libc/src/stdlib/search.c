/*
 * search.c - the POSIX search tables of <search.h>.
 *
 * Four collections that share a header and nothing else.  See search.h for
 * what each one is; this file is about how they are built.
 *
 * The tree is an AVL tree rather than the plain binary search tree the
 * interface would allow.  A plain one is conformant and is a linked list the
 * moment the keys arrive in order -- which is the common case, not a corner
 * one, since programs feed these from sorted files and from counters.  The
 * balancing is fifty lines and turns that back into O(log n).
 *
 * The hash table is open-addressed with double hashing, over a table whose
 * size is prime.  Both of those matter together: the second hash is taken
 * modulo size-1 and incremented, so it is in [1, size-1], and with size prime
 * that step visits every slot before repeating.  A power-of-two size with the
 * same probe would revisit a fraction of the table and loop for ever on a full
 * one.
 */
#include <search.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------ *
 * Doubly linked lists.
 * ------------------------------------------------------------------ */

/* The caller's structures, of which only the first two members are ours. */
struct qelem {
	struct qelem *q_forw;
	struct qelem *q_back;
};

void insque(void *elem, void *pred)
{
	struct qelem *e = elem;
	struct qelem *p = pred;

	if (!p) {
		/* A null predecessor starts a new list.  Not in POSIX, which
		 * leaves it undefined, but it is what every implementation
		 * does and what callers written against them expect. */
		e->q_forw = NULL;
		e->q_back = NULL;
		return;
	}
	e->q_forw = p->q_forw;
	e->q_back = p;
	if (p->q_forw)
		p->q_forw->q_back = e;
	p->q_forw = e;
}

void remque(void *elem)
{
	struct qelem *e = elem;

	if (e->q_forw)
		e->q_forw->q_back = e->q_back;
	if (e->q_back)
		e->q_back->q_forw = e->q_forw;
}

/* ------------------------------------------------------------------ *
 * Linear search.
 * ------------------------------------------------------------------ */

void *lfind(const void *key, const void *base, size_t *nelp, size_t width,
	    int (*compar)(const void *, const void *))
{
	const char *p = base;
	size_t n = *nelp;

	while (n--) {
		if (compar(key, p) == 0)
			return (void *)p;
		p += width;
	}
	return NULL;
}

void *lsearch(const void *key, void *base, size_t *nelp, size_t width,
	      int (*compar)(const void *, const void *))
{
	void *p = lfind(key, base, nelp, width, compar);

	if (p)
		return p;

	/* Not found: append.  The caller is responsible for the array having
	 * room -- there is nothing in the interface that could tell us how
	 * much there is, which is why lsearch is the odd one out here. */
	p = (char *)base + (*nelp)++ * width;
	memcpy(p, key, width);
	return p;
}

/* ------------------------------------------------------------------ *
 * Hash table.
 * ------------------------------------------------------------------ */

/* A slot.  `used' is the first hash value, kept so a lookup can reject a slot
 * without calling strcmp -- and zero means empty, which is why the hash is
 * forced non-zero below. */
struct _ENTRY {
	unsigned int used;
	ENTRY entry;
};

/* The table hcreate/hsearch/hdestroy share.  One per process, which is the
 * whole reason the _r forms exist. */
static struct hsearch_data htab;

static int is_prime(unsigned int n)
{
	if (n < 2)
		return 0;
	if (n % 2 == 0)
		return n == 2;
	for (unsigned int d = 3; d * d <= n; d += 2)
		if (n % d == 0)
			return 0;
	return 1;
}

/* djb2, which is what this family has traditionally used: cheap, and good
 * enough for the short identifier-like keys these tables hold. */
static unsigned int hash_key(const char *s)
{
	unsigned int h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	/* Zero marks an empty slot, so the hash must never be zero. */
	return h ? h : 1;
}

int hcreate_r(size_t nel, struct hsearch_data *tab)
{
	unsigned int size;

	if (!tab) {
		errno = EINVAL;
		return 0;
	}
	if (tab->table) {
		/* Already in use.  Creating over the top would leak it. */
		errno = EINVAL;
		return 0;
	}

	/* Room to spare, then rounded up to a prime.  Open addressing degrades
	 * sharply as a table fills, so the usual 25% headroom; the prime is
	 * what makes the double-hash probe cover every slot. */
	nel += nel / 4 + 1;
	if (nel < 3)
		nel = 3;
	if (nel > 0x7FFFFFFF) {
		errno = ENOMEM;
		return 0;
	}
	for (size = (unsigned int)nel; !is_prime(size); size++)
		;

	tab->table = calloc(size + 1, sizeof *tab->table);
	if (!tab->table) {
		errno = ENOMEM;
		return 0;
	}
	tab->size = size;
	tab->filled = 0;
	return 1;
}

void hdestroy_r(struct hsearch_data *tab)
{
	if (!tab)
		return;
	free(tab->table);
	tab->table = NULL;
	tab->size = 0;
	tab->filled = 0;
}

int hsearch_r(ENTRY item, ACTION action, ENTRY **retval,
	      struct hsearch_data *tab)
{
	unsigned int h, idx, step;
	unsigned int tries;

	if (!tab || !tab->table) {
		errno = EINVAL;
		if (retval)
			*retval = NULL;
		return 0;
	}

	h = hash_key(item.key);
	idx = h % tab->size;
	/* The second hash, in [1, size-1].  With size prime, stepping by it
	 * reaches every slot before coming back to the first. */
	step = 1 + h % (tab->size - 1);

	for (tries = 0; tries < tab->size; tries++) {
		struct _ENTRY *slot = &tab->table[idx];

		if (slot->used == 0)
			break; /* not present: the probe ends at the first gap */
		if (slot->used == h &&
		    strcmp(slot->entry.key, item.key) == 0) {
			if (retval)
				*retval = &slot->entry;
			return 1;
		}
		idx += step;
		if (idx >= tab->size)
			idx -= tab->size;
	}

	if (action == FIND) {
		errno = ESRCH;
		if (retval)
			*retval = NULL;
		return 0;
	}

	/* ENTER, and the probe stopped at an empty slot.  A full table has no
	 * gap to stop at, and the loop above ran out of tries instead. */
	if (tries == tab->size || tab->filled == tab->size) {
		errno = ENOMEM;
		if (retval)
			*retval = NULL;
		return 0;
	}

	tab->table[idx].used = h;
	tab->table[idx].entry = item;
	tab->filled++;
	if (retval)
		*retval = &tab->table[idx].entry;
	return 1;
}

int hcreate(size_t nel)
{
	return hcreate_r(nel, &htab);
}

void hdestroy(void)
{
	hdestroy_r(&htab);
}

ENTRY *hsearch(ENTRY item, ACTION action)
{
	ENTRY *r = NULL;

	hsearch_r(item, action, &r, &htab);
	return r;
}

/* ------------------------------------------------------------------ *
 * Sorted tree (AVL).
 * ------------------------------------------------------------------ */

/* The key pointer is FIRST, and that is part of the interface: tsearch and
 * tfind hand back a pointer to the node, and callers dereference it as a
 * `void **' to read the key they stored. */
struct tnode {
	const void *key;
	struct tnode *left;
	struct tnode *right;
	int height;
};

static int t_height(struct tnode *n)
{
	return n ? n->height : 0;
}

static void t_fix_height(struct tnode *n)
{
	int l = t_height(n->left);
	int r = t_height(n->right);

	n->height = (l > r ? l : r) + 1;
}

static int t_balance(struct tnode *n)
{
	return t_height(n->left) - t_height(n->right);
}

static struct tnode *t_rot_right(struct tnode *n)
{
	struct tnode *l = n->left;

	n->left = l->right;
	l->right = n;
	t_fix_height(n);
	t_fix_height(l);
	return l;
}

static struct tnode *t_rot_left(struct tnode *n)
{
	struct tnode *r = n->right;

	n->right = r->left;
	r->left = n;
	t_fix_height(n);
	t_fix_height(r);
	return r;
}

/* Restore the AVL invariant at one node, after a change below it.  The four
 * cases are the textbook ones: a subtree two taller than its sibling is
 * rotated, and if the offending grandchild is on the inside it is rotated out
 * first. */
static struct tnode *t_rebalance(struct tnode *n)
{
	int b;

	t_fix_height(n);
	b = t_balance(n);

	if (b > 1) {
		if (t_balance(n->left) < 0)
			n->left = t_rot_left(n->left);
		return t_rot_right(n);
	}
	if (b < -1) {
		if (t_balance(n->right) > 0)
			n->right = t_rot_right(n->right);
		return t_rot_left(n);
	}
	return n;
}

/* Insert, or find.  `*found' is set to the node holding the key either way,
 * and `*failed' when a node was needed and could not be allocated. */
static struct tnode *t_insert(struct tnode *n, const void *key,
			      int (*compar)(const void *, const void *),
			      struct tnode **found, int *failed)
{
	int c;

	if (!n) {
		struct tnode *fresh = malloc(sizeof *fresh);

		if (!fresh) {
			*failed = 1;
			return NULL;
		}
		fresh->key = key;
		fresh->left = NULL;
		fresh->right = NULL;
		fresh->height = 1;
		*found = fresh;
		return fresh;
	}

	c = compar(key, n->key);
	if (c == 0) {
		*found = n;
		return n; /* already there: the tree is unchanged */
	}
	if (c < 0)
		n->left = t_insert(n->left, key, compar, found, failed);
	else
		n->right = t_insert(n->right, key, compar, found, failed);

	if (*failed)
		return n;
	return t_rebalance(n);
}

void *tsearch(const void *key, void **rootp,
	      int (*compar)(const void *, const void *))
{
	struct tnode *found = NULL;
	int failed = 0;

	if (!rootp)
		return NULL;
	*rootp = t_insert(*rootp, key, compar, &found, &failed);
	if (failed)
		return NULL;
	return found;
}

void *tfind(const void *key, void *const *rootp,
	    int (*compar)(const void *, const void *))
{
	struct tnode *n;

	if (!rootp)
		return NULL;
	n = *rootp;
	while (n) {
		int c = compar(key, n->key);

		if (c == 0)
			return n;
		n = c < 0 ? n->left : n->right;
	}
	return NULL;
}

/* Remove `key' from the subtree at n.  `*removed' is set when something was.
 *
 * The two-child case takes the in-order successor's KEY into this node and
 * then deletes the successor, which is the standard trick: it keeps the tree
 * ordered without moving any node, so no pointer the caller might hold to a
 * node changes meaning -- except the successor's, which is going away anyway.
 */
static struct tnode *t_remove(struct tnode *n, const void *key,
			      int (*compar)(const void *, const void *),
			      int *removed)
{
	int c;

	if (!n)
		return NULL;

	c = compar(key, n->key);
	if (c < 0) {
		n->left = t_remove(n->left, key, compar, removed);
	} else if (c > 0) {
		n->right = t_remove(n->right, key, compar, removed);
	} else {
		*removed = 1;
		if (!n->left || !n->right) {
			struct tnode *child = n->left ? n->left : n->right;

			free(n);
			return child;
		}
		{
			struct tnode *succ = n->right;
			int gone = 0;

			while (succ->left)
				succ = succ->left;
			n->key = succ->key;
			n->right = t_remove(n->right, succ->key, compar,
					    &gone);
		}
	}
	return t_rebalance(n);
}

void *tdelete(const void *key, void **rootp,
	      int (*compar)(const void *, const void *))
{
	struct tnode *parent = NULL;
	struct tnode *n;
	int removed = 0;

	if (!rootp || !*rootp)
		return NULL;

	/* The parent is found BEFORE the removal, because afterwards the node
	 * is gone and so, possibly, is the shape of the tree around it.  This
	 * is what the interface asks to be returned -- a pointer to the parent
	 * of the deleted node -- and callers use it only to tell "removed"
	 * from "was not there". */
	n = *rootp;
	while (n) {
		int c = compar(key, n->key);

		if (c == 0)
			break;
		parent = n;
		n = c < 0 ? n->left : n->right;
	}
	if (!n)
		return NULL;

	*rootp = t_remove(*rootp, key, compar, &removed);

	/* Deleting the root leaves no parent to name.  A non-null result still
	 * has to distinguish "removed" from "not found", so the new root
	 * stands in -- and when the tree is now empty, a non-null value that
	 * is not a node, exactly as the traditional implementations do. */
	if (!parent)
		return *rootp ? *rootp : (void *)rootp;
	return parent;
}

static void t_walk(const struct tnode *n,
		   void (*action)(const void *, VISIT, int), int depth)
{
	if (!n)
		return;
	if (!n->left && !n->right) {
		action(n, leaf, depth);
		return;
	}
	action(n, preorder, depth);
	t_walk(n->left, action, depth + 1);
	action(n, postorder, depth);
	t_walk(n->right, action, depth + 1);
	action(n, endorder, depth);
}

void twalk(const void *root, void (*action)(const void *, VISIT, int))
{
	if (root && action)
		t_walk(root, action, 0);
}

static void t_walk_r(const struct tnode *n,
		     void (*action)(const void *, VISIT, void *), void *closure)
{
	if (!n)
		return;
	if (!n->left && !n->right) {
		action(n, leaf, closure);
		return;
	}
	action(n, preorder, closure);
	t_walk_r(n->left, action, closure);
	action(n, postorder, closure);
	t_walk_r(n->right, action, closure);
	action(n, endorder, closure);
}

void twalk_r(const void *root, void (*action)(const void *, VISIT, void *),
	     void *closure)
{
	if (root && action)
		t_walk_r(root, action, closure);
}

void tdestroy(void *root, void (*free_node)(void *))
{
	struct tnode *n = root;

	if (!n)
		return;
	tdestroy(n->left, free_node);
	tdestroy(n->right, free_node);
	if (free_node)
		free_node((void *)n->key);
	free(n);
}
