// LikeOS — intrusive red-black tree
//
// A node is embedded in the object it orders (an sk_buff, through the
// union with its queue links).  The tree owns no memory and takes no lock:
// the user serialises every operation on one tree (TCP: the connection's
// lock).  The color is kept in the low bit of the parent pointer, so a node
// must be at least 2-byte aligned (every struct with pointers is).
//
// Insertion is two steps, like everywhere this structure is used: the
// caller walks down from the root comparing its own keys, then links the
// node at the empty child slot it found (rb_link_node) and rebalances
// (rb_insert_color).  Erase, first/last and next/prev are O(log n).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_RBTREE_H
#define LIKEOS_RBTREE_H

#include <kernel/uapi/types.h>

#define RB_RED 0u
#define RB_BLACK 1u

struct rb_node {
	unsigned long rb_parent_color; // parent pointer | color bit
	struct rb_node *rb_right;
	struct rb_node *rb_left;
};

struct rb_root {
	struct rb_node *rb_node;
};

#define RB_ROOT_INIT { NULL }

static inline struct rb_node *rb_parent(const struct rb_node *n)
{
	return (struct rb_node *)(n->rb_parent_color & ~(unsigned long)3);
}

static inline unsigned rb_color(const struct rb_node *n)
{
	return (unsigned)(n->rb_parent_color & 1u);
}

static inline int rb_is_red(const struct rb_node *n)
{
	return n && rb_color(n) == RB_RED;
}

static inline int rb_is_black(const struct rb_node *n)
{
	return !n || rb_color(n) == RB_BLACK;
}

static inline int rb_empty_root(const struct rb_root *r)
{
	return r->rb_node == NULL;
}

/* A node that is in no tree points at itself (RB_CLEAR_NODE). */
static inline void rb_clear_node(struct rb_node *n)
{
	n->rb_parent_color = (unsigned long)n;
}

static inline int rb_empty_node(const struct rb_node *n)
{
	return n->rb_parent_color == (unsigned long)n;
}

// Link `node' as a (red) child of `parent' at *link (a NULL child slot of
// parent, or &root->rb_node for an empty tree); rb_insert_color next.
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
				struct rb_node **link)
{
	node->rb_parent_color = (unsigned long)parent; // red
	node->rb_left = NULL;
	node->rb_right = NULL;
	*link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
// Put `newn' in `old''s place (same position in the order), O(1).
void rb_replace_node(struct rb_node *old, struct rb_node *newn,
		     struct rb_root *root);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

// Check every red-black property; returns the black height, or -1 when a
// property is violated (tests and assertions).
int rb_validate(const struct rb_root *root);

#define rb_entry(ptr, type, member) \
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

#endif // LIKEOS_RBTREE_H
