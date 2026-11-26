// LikeOS — intrusive red-black tree (see rbtree.h)
//
// The five properties: every node is red or black; the root is black;
// NULL leaves count as black; a red node has no red child; every path
// from a node down to a leaf passes the same number of black nodes.
// Insertion and deletion restore them with at most three rotations.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/rbtree.h>

static inline void rb_set_parent(struct rb_node *n, struct rb_node *p)
{
	n->rb_parent_color = (unsigned long)p | (n->rb_parent_color & 1u);
}

static inline void rb_set_color(struct rb_node *n, unsigned color)
{
	n->rb_parent_color = (n->rb_parent_color & ~(unsigned long)1) | color;
}

static inline void rb_set_parent_color(struct rb_node *n, struct rb_node *p,
				       unsigned color)
{
	n->rb_parent_color = (unsigned long)p | color;
}

/* Make `newn' take `old''s place under `parent' (or as the root). */
static inline void rb_change_child(struct rb_node *old, struct rb_node *newn,
				   struct rb_node *parent, struct rb_root *root)
{
	if (!parent)
		root->rb_node = newn;
	else if (parent->rb_left == old)
		parent->rb_left = newn;
	else
		parent->rb_right = newn;
}

/*        x                y
 *       / \              / \
 *      a   y     =>     x   c
 *         / \          / \
 *        b   c        a   b        (colors unchanged) */
static void rb_rotate_left(struct rb_node *x, struct rb_root *root)
{
	struct rb_node *y = x->rb_right;
	struct rb_node *p = rb_parent(x);

	x->rb_right = y->rb_left;
	if (y->rb_left)
		rb_set_parent(y->rb_left, x);
	rb_set_parent(y, p);
	rb_change_child(x, y, p, root);
	y->rb_left = x;
	rb_set_parent(x, y);
}

static void rb_rotate_right(struct rb_node *x, struct rb_root *root)
{
	struct rb_node *y = x->rb_left;
	struct rb_node *p = rb_parent(x);

	x->rb_left = y->rb_right;
	if (y->rb_right)
		rb_set_parent(y->rb_right, x);
	rb_set_parent(y, p);
	rb_change_child(x, y, p, root);
	y->rb_right = x;
	rb_set_parent(x, y);
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *parent, *gparent, *uncle;

	while ((parent = rb_parent(node)) && rb_is_red(parent)) {
		/* A red parent is never the root: the grandparent exists. */
		gparent = rb_parent(parent);
		if (parent == gparent->rb_left) {
			uncle = gparent->rb_right;
			if (rb_is_red(uncle)) {
				// recolor and continue two levels up
				rb_set_color(parent, RB_BLACK);
				rb_set_color(uncle, RB_BLACK);
				rb_set_color(gparent, RB_RED);
				node = gparent;
				continue;
			}
			if (node == parent->rb_right) {
				rb_rotate_left(parent, root);
				node = parent;
				parent = rb_parent(node);
			}
			rb_set_color(parent, RB_BLACK);
			rb_set_color(gparent, RB_RED);
			rb_rotate_right(gparent, root);
		} else {
			uncle = gparent->rb_left;
			if (rb_is_red(uncle)) {
				rb_set_color(parent, RB_BLACK);
				rb_set_color(uncle, RB_BLACK);
				rb_set_color(gparent, RB_RED);
				node = gparent;
				continue;
			}
			if (node == parent->rb_left) {
				rb_rotate_right(parent, root);
				node = parent;
				parent = rb_parent(node);
			}
			rb_set_color(parent, RB_BLACK);
			rb_set_color(gparent, RB_RED);
			rb_rotate_left(gparent, root);
		}
	}
	rb_set_color(root->rb_node, RB_BLACK);
}

/* A black node was removed above `node' (possibly NULL) whose parent is
 * `parent': that side lacks one black.  Restore the black heights. */
static void rb_erase_fixup(struct rb_node *node, struct rb_node *parent,
			   struct rb_root *root)
{
	struct rb_node *w;

	while (node != root->rb_node && rb_is_black(node)) {
		if (parent->rb_left == node) {
			w = parent->rb_right;
			if (rb_is_red(w)) {
				rb_set_color(w, RB_BLACK);
				rb_set_color(parent, RB_RED);
				rb_rotate_left(parent, root);
				w = parent->rb_right;
			}
			if (rb_is_black(w->rb_left) &&
			    rb_is_black(w->rb_right)) {
				rb_set_color(w, RB_RED);
				node = parent;
				parent = rb_parent(node);
			} else {
				if (rb_is_black(w->rb_right)) {
					rb_set_color(w->rb_left, RB_BLACK);
					rb_set_color(w, RB_RED);
					rb_rotate_right(w, root);
					w = parent->rb_right;
				}
				rb_set_color(w, rb_color(parent));
				rb_set_color(parent, RB_BLACK);
				if (w->rb_right)
					rb_set_color(w->rb_right, RB_BLACK);
				rb_rotate_left(parent, root);
				node = root->rb_node;
				break;
			}
		} else {
			w = parent->rb_left;
			if (rb_is_red(w)) {
				rb_set_color(w, RB_BLACK);
				rb_set_color(parent, RB_RED);
				rb_rotate_right(parent, root);
				w = parent->rb_left;
			}
			if (rb_is_black(w->rb_left) &&
			    rb_is_black(w->rb_right)) {
				rb_set_color(w, RB_RED);
				node = parent;
				parent = rb_parent(node);
			} else {
				if (rb_is_black(w->rb_left)) {
					rb_set_color(w->rb_right, RB_BLACK);
					rb_set_color(w, RB_RED);
					rb_rotate_left(w, root);
					w = parent->rb_left;
				}
				rb_set_color(w, rb_color(parent));
				rb_set_color(parent, RB_BLACK);
				if (w->rb_left)
					rb_set_color(w->rb_left, RB_BLACK);
				rb_rotate_right(parent, root);
				node = root->rb_node;
				break;
			}
		}
	}
	if (node)
		rb_set_color(node, RB_BLACK);
}

void rb_erase(struct rb_node *z, struct rb_root *root)
{
	struct rb_node *child, *parent;
	unsigned color;

	if (z->rb_left && z->rb_right) {
		/* Two children: the in-order successor y (leftmost of the
		 * right subtree, no left child) takes z's place and color;
		 * the color that leaves the tree is y's own. */
		struct rb_node *y = z->rb_right;

		while (y->rb_left)
			y = y->rb_left;
		child = y->rb_right;
		color = rb_color(y);
		if (rb_parent(y) == z) {
			parent = y; // y keeps its right subtree
		} else {
			parent = rb_parent(y);
			if (child)
				rb_set_parent(child, parent);
			parent->rb_left = child;
			y->rb_right = z->rb_right;
			rb_set_parent(z->rb_right, y);
		}
		rb_change_child(z, y, rb_parent(z), root);
		y->rb_parent_color = z->rb_parent_color; // parent and color
		y->rb_left = z->rb_left;
		rb_set_parent(z->rb_left, y);
		if (color == RB_BLACK)
			rb_erase_fixup(child, parent, root);
		rb_clear_node(z);
		return;
	}
	child = z->rb_left ? z->rb_left : z->rb_right;
	parent = rb_parent(z);
	color = rb_color(z);
	if (child)
		rb_set_parent_color(child, parent, rb_color(child));
	rb_change_child(z, child, parent, root);
	if (color == RB_BLACK)
		rb_erase_fixup(child, parent, root);
	rb_clear_node(z);
}

void rb_replace_node(struct rb_node *old, struct rb_node *newn,
		     struct rb_root *root)
{
	struct rb_node *parent = rb_parent(old);

	*newn = *old;
	if (old->rb_left)
		rb_set_parent(old->rb_left, newn);
	if (old->rb_right)
		rb_set_parent(old->rb_right, newn);
	rb_change_child(old, newn, parent, root);
	rb_clear_node(old);
}

struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;

	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}

struct rb_node *rb_last(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;

	if (!n)
		return NULL;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}

struct rb_node *rb_next(const struct rb_node *node)
{
	struct rb_node *parent;

	if (node->rb_right) {
		node = node->rb_right;
		while (node->rb_left)
			node = node->rb_left;
		return (struct rb_node *)node;
	}
	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;
	return parent;
}

struct rb_node *rb_prev(const struct rb_node *node)
{
	struct rb_node *parent;

	if (node->rb_left) {
		node = node->rb_left;
		while (node->rb_right)
			node = node->rb_right;
		return (struct rb_node *)node;
	}
	while ((parent = rb_parent(node)) && node == parent->rb_left)
		node = parent;
	return parent;
}

static int rb_validate_sub(const struct rb_node *n, const struct rb_node *p)
{
	int l, r;

	if (!n)
		return 1;
	if (rb_parent(n) != p)
		return -1;
	if (rb_is_red(n) && (rb_is_red(n->rb_left) || rb_is_red(n->rb_right)))
		return -1;
	l = rb_validate_sub(n->rb_left, n);
	r = rb_validate_sub(n->rb_right, n);
	if (l < 0 || r < 0 || l != r)
		return -1;
	return l + (rb_is_black(n) ? 1 : 0);
}

int rb_validate(const struct rb_root *root)
{
	if (root->rb_node && rb_is_red(root->rb_node))
		return -1;
	return rb_validate_sub(root->rb_node, NULL);
}
