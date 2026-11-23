// LikeOS -- in-memory pseudo filesystems (sysfs-style and proc-style).
//
// A tree of directories, text files and symlinks that exists only as
// kernel data: file contents are produced by a callback when the file is
// opened, directory listings are the children (plus, for a dynamic
// directory, whatever its enumerate callback adds).  Two instances are
// mounted: /sys, describing devices, and /proc, describing processes.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_PSEUDOFS_H
#define KERNEL_FS_PSEUDOFS_H

#include <kernel/fs/vfs.h>

struct pfs_node;
struct pfs;

/* Produce the contents of a file into buf (cap bytes); return the length
 * (may exceed cap: the caller then retries with a bigger buffer), or a
 * negative errno. */
typedef long (*pfs_show_t)(struct pfs_node *n, char *buf, long cap);
/* Accept a write of `len' bytes (a copy in kernel memory, not NUL
 * terminated) to a file that takes input -- a sysfs attribute that sets
 * something.  Return the bytes consumed or a negative errno. */
typedef long (*pfs_store_t)(struct pfs_node *n, const char *buf, long len);
/* Dynamic directory: list the first child whose key is >= *cursor into name,
 * set *cursor to that key + 1 and return 1; return 0 at the end.  `type'
 * receives DT_DIR / DT_REG / DT_LNK.
 *
 * The key is what readdir resumes from between getdents calls, so it must
 * name the entry itself (a pid, a descriptor number), not its place in the
 * list: an entry that goes away before the next call would otherwise shift
 * every later one down by one, and one of them would never be listed. */
typedef int (*pfs_list_t)(struct pfs_node *dir, unsigned *cursor, char *name,
			  long cap, int *type);
/* Dynamic directory listed from a snapshot that belongs to one open
 * directory handle -- for a source that can only be enumerated as a whole
 * ("the entry after this one") and must not shift under a reader that is
 * part way through.  *snap is the handle's: NULL when it was opened, and
 * again after a rewind (lseek to 0) when `refresh' is set too; the callback
 * takes its snapshot when it finds NULL there (freeing an old one first
 * when refreshing) and keeps it.  Otherwise as pfs_list_t, with *cursor an
 * index into the snapshot; a negative errno when no snapshot can be had.
 * `list_free' releases a snapshot: at close, and nowhere else. */
typedef int (*pfs_list_snap_t)(struct pfs_node *dir, void **snap, int refresh,
			       unsigned *cursor, char *name, long cap,
			       int *type);
/* Dynamic directory: resolve one child name to a node that the caller
 * owns until pfs_node_put(); NULL if there is no such child. */
typedef struct pfs_node *(*pfs_lookup_t)(struct pfs_node *dir,
					 const char *name);
/* Dynamic directory that takes new files: the node for `name', which does
 * not exist yet and is about to be created by an open() with O_CREAT -- a
 * transient file node with a store callback, owned by the caller until
 * pfs_node_put(); NULL refuses the name.  Nothing exists until the first
 * write reaches `store'.  The directory must be a static node (the new
 * node keeps a pointer to it). */
typedef struct pfs_node *(*pfs_create_t)(struct pfs_node *dir,
					 const char *name);

/* The longest name a dynamic directory's list/lookup/create callbacks are
 * handed or may report (including the terminating NUL).  Static nodes keep
 * the shorter pfs_node.name. */
#define PFS_NAME_MAX 256

enum pfs_type { PFS_DIR = 1, PFS_FILE = 2, PFS_LINK = 3 };

struct pfs_node {
	char name[64];
	enum pfs_type type;
	uint32_t mode;
	uint32_t uid, gid;
	struct pfs_node *parent;
	struct pfs_node *children; /* static children */
	struct pfs_node *sibling;
	struct pfs *fs;
	/* files */
	pfs_show_t show;
	pfs_store_t store; /* writable when set */
	/* Files too large, or too changeable, to be materialised whole at
	 * open: this is called per read with the offset the caller is at, and
	 * takes precedence over `show'.  /proc/<pid>/mem is the reason it
	 * exists -- a process's address space is a hundred megabytes and does
	 * not hold still. */
	long (*read_at)(struct pfs_node *n, uint64_t off, char *buf, long cap);
	void *arg;
	uint64_t arg2;
	/* symlinks: target, absolute or relative to the link's directory --
	 * in link[], or, longer (a process's working directory or open file,
	 * up to a whole path), in link_long, which a transient node owns
	 * (pfs_set_link; freed with the node).  Read it with pfs_link(). */
	char link[128];
	char *link_long;
	/* dynamic directories */
	pfs_list_snap_t list_snap; /* takes precedence over `list' */
	void (*list_free)(struct pfs_node *dir, void *snap);
	pfs_list_t list;
	pfs_lookup_t lookup;
	pfs_create_t create; /* open(O_CREAT) of a name that does not exist */
	/* Files that unlink() may remove: 0 or a negative errno.  Without it
	 * unlink() is refused with EPERM. */
	int (*unlink)(struct pfs_node *n);
	/* Called on a transient node just before it is freed, for a callback
	 * that hung private data on `arg'. */
	void (*release)(struct pfs_node *n);
	/* The most one write() hands `store'.  0: PFS_STORE_MAX, and a longer
	 * write is cut to it (attribute files take one short value).  Set: a
	 * write up to this many bytes reaches `store' whole, a longer one
	 * fails with EFBIG -- for files whose contents are one record that
	 * must arrive in a single write. */
	long store_max;
	/* nodes made by a lookup callback are freed when released */
	int transient;
	uint64_t ino;
};

struct pfs {
	const char *mount; /* "/sys" */
	struct pfs_node root;
	vfs_ops_t ops;
	uint64_t next_ino;
};

/* A link node's target. */
static inline const char *pfs_link(const struct pfs_node *n)
{
	return n->link_long ? n->link_long : n->link;
}

/* Set a (transient) link node's target, of any length: ST_OK or ST_NOMEM. */
int pfs_set_link(struct pfs_node *n, const char *target);

void pfs_init(struct pfs *fs, const char *mount);
const vfs_ops_t *pfs_ops(struct pfs *fs);

/* Build the static tree.  `path' is relative to the mount ("bus/pci"). */
struct pfs_node *pfs_mkdir(struct pfs *fs, const char *path);
struct pfs_node *pfs_add_file(struct pfs *fs, const char *path,
			      pfs_show_t show, void *arg, uint64_t arg2);
struct pfs_node *pfs_add_link(struct pfs *fs, const char *path,
			      const char *target);
/* A file that is read through `show' and written through `store'. */
struct pfs_node *pfs_add_file_rw(struct pfs *fs, const char *path,
				 pfs_show_t show, pfs_store_t store, void *arg,
				 uint64_t arg2);
/* Add a node of `type' whose fields (mode, uid/gid, show/store/read_at,
 * arg/arg2, link, list/lookup) are taken from `tmpl' before the node
 * becomes reachable -- what a caller that would otherwise set them on the
 * returned node afterwards, while lookups can already see it, should use.
 * Adding is serialised; nodes are never removed. */
struct pfs_node *pfs_add_node(struct pfs *fs, const char *path,
			      enum pfs_type type, const struct pfs_node *tmpl);
/* Make a directory dynamic. */
void pfs_set_dynamic(struct pfs_node *dir, pfs_list_t list,
		     pfs_lookup_t lookup);
/* The most a write to a file whose node does not set store_max delivers. */
#define PFS_STORE_MAX 256
/* For lookup callbacks: allocate a transient node. */
struct pfs_node *pfs_node_new(struct pfs *fs, struct pfs_node *parent,
			      const char *name, enum pfs_type type);
void pfs_node_put(struct pfs_node *n);
/* Find a node by path relative to the mount; the result must be released
 * with pfs_node_put() (a no-op for static nodes). */
struct pfs_node *pfs_lookup(struct pfs *fs, const char *relpath);

/* Helpers for show callbacks. */
long pfs_printf(char *buf, long cap, long pos, const char *fmt, ...);

#endif
