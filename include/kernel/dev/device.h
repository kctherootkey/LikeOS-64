// LikeOS-64 -- character devices with driver-supplied operations.
//
// The original device nodes (ttys, /dev/fb0, /dev/input/event*, /dev/shm)
// are wired into devfs by name, with their ioctl, mmap and poll behaviour
// dispatched from type switches in the syscall layer.  That does not scale
// to a driver that brings its own node -- a GPU with two device files, a
// dozen ioctls, buffers to map and events to poll -- so this is the
// generic form: a driver REGISTERS a node with a table of operations, and
// devfs, mmap() and poll() call the table.
//
// The same table also backs ANONYMOUS device files: a file that has no
// name in /dev and exists only as a descriptor -- an exported buffer, a
// fence, an event counter, a timer.  Because it is an ordinary vfs_file_t
// from the descriptor layer's point of view, dup(), fork(), exec(),
// close() refcounting, fstat(), poll() and descriptor passing over AF_UNIX
// all work on it with no special cases.
#ifndef KERNEL_DEV_DEVICE_H
#define KERNEL_DEV_DEVICE_H

#include <kernel/uapi/types.h>
#include <kernel/fs/vfs.h>

struct task;
struct poll_table;
struct kstat;
struct devfs_node;

/* Describes what a driver's ->mmap wants mapped.  The driver fills it in;
 * the memory manager builds the page tables from it.  Pages named here are
 * OWNED BY THE DRIVER'S OBJECT: they are never handed to the physical
 * allocator by the address space, and fork shares rather than copies them.
 * The object is pinned for the life of the mapping through get/put. */
struct mm_dirty_ops;
struct device_mmap {
	/* In: what the caller asked for. */
	uint64_t offset; /* page-aligned file offset */
	uint64_t length; /* page-aligned */
	uint64_t prot; /* PROT_* */
	uint64_t flags; /* MAP_* */
	/* Out: the pages.  page_phys returns the physical address of page
	 * `index' of the mapping (index 0 = `offset'), or 0 for "no such
	 * page" which fails the mmap. */
	uint64_t (*page_phys)(void *obj, uint64_t index);
	void *obj;
	void (*get)(void *obj); /* one more mapping references obj */
	void (*put)(void *obj); /* a mapping went away */
	/* Extra PTE bits: PAGE_WRITE_THROUGH / PAGE_CACHE_DISABLE for device
	 * memory that must not be cached normally.  0 for ordinary RAM. */
	uint64_t pte_extra;
	/* Dirty tracking for the mapping, when the driver wants the
	 * processor's writes through it watched: stored on every region
	 * record made of the mapping and consulted by the write-fault and
	 * unmap paths.  NULL for the ordinary case.  See mm_dirty_ops. */
	const struct mm_dirty_ops *dirty_ops;
};

/* Every operation is optional; a NULL slot answers the conventional way
 * (read/write: -EINVAL, ioctl: -ENOTTY, mmap: -ENODEV, poll: always
 * ready, seek: -ESPIPE). */
struct device_ops {
	/* A descriptor is being opened on the node.  `file' is the new
	 * handle; set device_file_priv(file, ...) for per-open state.  The
	 * caller's credentials have already been checked against the node's
	 * mode.  Not called for anonymous files. */
	int (*open)(struct devfs_node *node, vfs_file_t *file, int flags,
		    struct task *cur);
	/* The last reference to the handle is gone. */
	void (*release)(vfs_file_t *file);
	long (*read)(vfs_file_t *file, void *buf, long bytes, int nonblock);
	long (*write)(vfs_file_t *file, const void *buf, long bytes,
		      int nonblock);
	long (*seek)(vfs_file_t *file, long offset, int whence);
	/* `argp' is the raw user pointer; the driver validates and copies. */
	long (*ioctl)(vfs_file_t *file, unsigned long req, void *argp,
		      struct task *cur);
	/* Fill `m' (offset/length/prot/flags are already set) or return a
	 * negative errno. */
	int (*mmap)(vfs_file_t *file, struct device_mmap *m);
	/* Register on the file's wait queue(s) with poll_wait(pt, file, wq)
	 * and return the ready POLL* bits. */
	short (*poll)(vfs_file_t *file, short events, struct poll_table *pt);
	/* Optional: refine the fstat answer (size, times) after the node's
	 * identity has been filled in. */
	int (*fstat)(vfs_file_t *file, struct kstat *st);
};

/* A registered node under /dev. */
struct devfs_node {
	char path[48]; /* full path, "/dev/dri/card0" */
	uint32_t mode; /* permission bits; S_IFCHR is implied */
	uint32_t uid, gid;
	uint32_t major, minor; /* st_rdev */
	const struct device_ops *ops;
	void *priv; /* driver's node context */
	int is_dir; /* a directory (ops unused) */
	int registered;
};

/* Register / unregister.  A path whose parent directory is not one of the
 * built-in ones must have been registered as a directory first. */
int device_register(struct devfs_node *node);
int device_unregister(struct devfs_node *node);
int device_register_dir(struct devfs_node *node);

/* Per-open private state. */
void *device_file_priv(vfs_file_t *file);
void device_file_set_priv(vfs_file_t *file, void *priv);
/* The node an open handle belongs to (NULL for an anonymous file). */
struct devfs_node *device_file_node(vfs_file_t *file);
/* The operations behind a handle, or NULL if it is not one of ours. */
const struct device_ops *device_file_ops(vfs_file_t *file);

/* Create an anonymous device file: no name, just a handle whose
 * behaviour is `ops' with `priv' as its per-open state.  `name' is what
 * /dev/fd/N reports and what shows in a listing ("anon_inode:[eventfd]").
 * Returns the file (refcount 1) or NULL.  The caller installs it with
 * fd_install(). */
vfs_file_t *device_anon_file(const struct device_ops *ops, void *priv,
			     const char *name, int flags);

/* For drivers' ->poll: register the calling task on `h', owned by `file',
 * so a later poll_notify_wq(h) wakes the poller.  (kernel/net/poll.c) */
struct wait_queue_head;
void poll_wait(struct poll_table *pt, void *owner, struct wait_queue_head *h);
void poll_notify_wq(struct wait_queue_head *h);

#endif
