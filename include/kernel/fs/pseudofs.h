// LikeOS-64 -- in-memory pseudo filesystems (sysfs-style and proc-style).
//
// A tree of directories, text files and symlinks that exists only as
// kernel data: file contents are produced by a callback when the file is
// opened, directory listings are the children (plus, for a dynamic
// directory, whatever its enumerate callback adds).  Two instances are
// mounted: /sys, describing devices, and /proc, describing processes.
#ifndef KERNEL_FS_PSEUDOFS_H
#define KERNEL_FS_PSEUDOFS_H

#include <kernel/fs/vfs.h>

struct pfs_node;
struct pfs;

/* Produce the contents of a file into buf (cap bytes); return the length
 * (may exceed cap: the caller then retries with a bigger buffer), or a
 * negative errno. */
typedef long (*pfs_show_t)(struct pfs_node *n, char *buf, long cap);
/* Dynamic directory: list child `index' into name (return 1) or return 0
 * at the end; `type' receives DT_DIR / DT_REG / DT_LNK. */
typedef int (*pfs_list_t)(struct pfs_node *dir, unsigned index, char *name,
			  long cap, int *type);
/* Dynamic directory: resolve one child name to a node that the caller
 * owns until pfs_node_put(); NULL if there is no such child. */
typedef struct pfs_node *(*pfs_lookup_t)(struct pfs_node *dir,
					 const char *name);

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
	/* Files too large, or too changeable, to be materialised whole at
	 * open: this is called per read with the offset the caller is at, and
	 * takes precedence over `show'.  /proc/<pid>/mem is the reason it
	 * exists -- a process's address space is a hundred megabytes and does
	 * not hold still. */
	long (*read_at)(struct pfs_node *n, uint64_t off, char *buf, long cap);
	void *arg;
	uint64_t arg2;
	/* symlinks: target, absolute or relative to the link's directory */
	char link[128];
	/* dynamic directories */
	pfs_list_t list;
	pfs_lookup_t lookup;
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

void pfs_init(struct pfs *fs, const char *mount);
const vfs_ops_t *pfs_ops(struct pfs *fs);

/* Build the static tree.  `path' is relative to the mount ("bus/pci"). */
struct pfs_node *pfs_mkdir(struct pfs *fs, const char *path);
struct pfs_node *pfs_add_file(struct pfs *fs, const char *path,
			      pfs_show_t show, void *arg, uint64_t arg2);
struct pfs_node *pfs_add_link(struct pfs *fs, const char *path,
			      const char *target);
/* Make a directory dynamic. */
void pfs_set_dynamic(struct pfs_node *dir, pfs_list_t list,
		     pfs_lookup_t lookup);
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
