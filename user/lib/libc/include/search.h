/*
 * search.h - the POSIX search tables.
 *
 * Four unrelated collections that share a header for historical reasons:
 *
 *   insque/remque      splice into and out of a doubly linked list
 *   lsearch/lfind      linear search of a flat array, with lsearch appending
 *                      the key when it is not there
 *   hsearch and co.    a hash table of string keys
 *   tsearch and co.    a sorted tree with a caller-supplied comparison
 *
 * The tree is the one worth knowing about: it is kept balanced (an AVL tree),
 * so a program that inserts already-sorted keys -- which is most programs
 * reading a sorted file -- gets O(log n) lookups rather than a linked list.
 */
#ifndef _SEARCH_H
#define _SEARCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- doubly linked lists ------------------------------------------------
 *
 * The elements are the caller's own structures, whose first two members must
 * be forward and backward pointers.  Nothing here allocates.
 */
void insque(void *__elem, void *__pred);
void remque(void *__elem);

/* ---- linear search of an array ------------------------------------------
 *
 * `base' is an array of `*nelp' elements of `width' bytes.  lfind returns the
 * matching element or NULL; lsearch APPENDS the key and returns the new
 * element instead -- which means base must have room for one more, and *nelp
 * is incremented.
 */
void *lfind(const void *__key, const void *__base, size_t *__nelp,
	    size_t __width, int (*__compar)(const void *, const void *));
void *lsearch(const void *__key, void *__base, size_t *__nelp, size_t __width,
	      int (*__compar)(const void *, const void *));

/* ---- hash table ---------------------------------------------------------
 *
 * One entry: a string key and whatever the caller wants to hang off it.  The
 * table does not copy the key, so it must outlive the entry.
 */
typedef struct entry {
	char *key;
	void *data;
} ENTRY;

typedef enum { FIND, ENTER } ACTION;

/* Whether hsearch should look only, or insert when it finds nothing. */
int hcreate(size_t __nel);
void hdestroy(void);
ENTRY *hsearch(ENTRY __item, ACTION __action);

/* The reentrant forms, which take the table instead of using a hidden one.
 * Not POSIX, but universal, and the only way to have two tables at once.
 * The struct must be zeroed before hcreate_r is called on it. */
struct hsearch_data {
	struct _ENTRY *table;
	unsigned int size;
	unsigned int filled;
};

int hcreate_r(size_t __nel, struct hsearch_data *__tab);
void hdestroy_r(struct hsearch_data *__tab);
int hsearch_r(ENTRY __item, ACTION __action, ENTRY **__retval,
	      struct hsearch_data *__tab);

/* ---- sorted tree --------------------------------------------------------
 *
 * `rootp' points at the caller's root pointer, which starts as NULL and is
 * updated as the tree grows.  The tree stores the key POINTER, not a copy, so
 * the key must outlive the entry -- and the value returned by tsearch and
 * tfind is a pointer to that stored pointer, which is how a caller reads back
 * what it inserted.
 */
typedef enum { preorder, postorder, endorder, leaf } VISIT;

/* Find `key', inserting it if it is not there.  Returns a pointer to the
 * stored key pointer, or NULL if a new node was needed and could not be
 * allocated. */
void *tsearch(const void *__key, void **__rootp,
	      int (*__compar)(const void *, const void *));

/* Find `key' without inserting.  NULL if it is not there. */
void *tfind(const void *__key, void *const *__rootp,
	    int (*__compar)(const void *, const void *));

/* Remove `key'.  Returns a pointer to the parent of the removed node, or NULL
 * if there was no such key.  Removing the last node leaves *rootp NULL. */
void *tdelete(const void *__key, void **__rootp,
	      int (*__compar)(const void *, const void *));

/* Walk the tree in order.  The action is called three times for an internal
 * node (preorder, postorder, endorder) and once for a leaf, with the depth
 * from the root.  The first argument is the node, from which the key pointer
 * can be read -- it is the first member. */
void twalk(const void *__root, void (*__action)(const void *, VISIT, int));

/* twalk with a caller closure, and free the whole tree.  Both are GNU
 * extensions; tdestroy in particular has no portable equivalent, and without
 * it there is no way to free a tree without knowing its shape. */
void twalk_r(const void *__root,
	     void (*__action)(const void *, VISIT, void *), void *__closure);
void tdestroy(void *__root, void (*__free_node)(void *));

#ifdef __cplusplus
}
#endif

#endif /* _SEARCH_H */
