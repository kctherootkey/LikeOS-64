/* LikeOS-64 POSIX shared memory objects.
 *
 * A named, reference-counted array of physical pages.  Unlike every other
 * mapping in this system, these pages are shared between processes that are
 * NOT related by fork(): two unrelated tasks that mmap the same object see the
 * same physical memory.  That is the whole point of the thing — it is what
 * lets a display server hand a client a frame buffer without copying it.
 *
 * The objects are reached through /dev/shm, so a handle on one is an ordinary
 * vfs_file and inherits dup/fork/exec/close refcounting, fstat, ftruncate and
 * descriptor passing for free rather than needing a parallel set of hooks.
 */
#ifndef _KERNEL_MM_SHM_H_
#define _KERNEL_MM_SHM_H_

#include <kernel/uapi/types.h>

/* System-wide limits.
 *
 * SHM_MAX_OBJECTS counts POSIX and System V segments TOGETHER: shmget() names
 * its segments "sysv.<key>" and allocates them from this same table (see
 * shm_sysv_get), so the two namespaces compete for one pool.  That is what
 * makes 64 far too few for a desktop:
 *
 *   - GTK draws X11 windows through MIT-SHM, which is shmget(), and takes a
 *     segment per window buffer.  A program with a deep widget tree holds
 *     dozens before it has drawn anything.
 *   - WebKit paints every page into a ShareableBitmap backed by shm_open().
 *
 * At 64 the desktop's X11 segments were exhausting the table before WebKit
 * could allocate a single backing store.  luakit loaded, styled and laid out
 * its page -- hovering a link even showed the URL, so hit-testing worked --
 * and then painted nothing, because shm_open() answered ENOMEM forever:
 *
 *     Failed to create shared memory: Cannot allocate memory
 *
 * MiniBrowser, one plain window with a shallow widget tree, stayed under the
 * limit and rendered the same page perfectly.  That difference is the whole
 * bug; it had nothing to do with WebKit configuration.
 *
 * 1024 entries cost sizeof(shm_object_t) each in .bss and nothing else --
 * the table is an array of descriptors, not of memory.  Pages are still only
 * pinned by segments that actually exist, which is what the note below is
 * about, and SHM_MAX_PAGES still caps each one. */
#define SHM_MAX_OBJECTS 1024
#define SHM_NAME_MAX 64
#define SHM_MAX_PAGES 16384 /* 64 MB per object */

typedef struct shm_object {
	char name[SHM_NAME_MAX]; /* without any leading '/' */
	int in_use;
	/* The name has been removed but mappings or open handles remain.  The
	 * pages live until the last reference goes: POSIX requires an unlinked
	 * object to stay usable for everyone still holding it. */
	int unlinked;
	int refs; /* open handles + live mappings */
	/* Claimed by a resize in progress.  Resizing allocates, which cannot
	 * happen under the table spinlock, so the flag is what serialises two
	 * concurrent ftruncate()s on one object. */
	int resizing;
	unsigned long size; /* bytes, as set by ftruncate */
	unsigned long npages;
	uint64_t *pages; /* physical addresses, npages entries */
	unsigned mode; /* permission bits */
	unsigned uid;
	unsigned gid;
	unsigned long ino; /* stable identity for fstat */
} shm_object_t;

void shm_init(void);

/* Look up by name and take a reference; NULL if absent. */
shm_object_t *shm_lookup_get(const char *name);
/* Create (and take a reference).  Fails with NULL if the name exists. */
shm_object_t *shm_create_get(const char *name, unsigned mode);
/* Drop a reference; frees the pages when the last one goes on an unlinked
 * object. */
void shm_put(shm_object_t *obj);
/* Remove the name.  Open handles keep working. */
int shm_unlink_name(const char *name);

/* Resize.  Growing allocates zeroed pages, shrinking frees the tail.  An
 * object that is currently mapped cannot shrink (a mapping would be left
 * pointing at freed pages), which is reported as -EBUSY. */
int shm_set_size(shm_object_t *obj, unsigned long size);

/* Physical address of one page, or 0 if the index is past the end. */
uint64_t shm_page_phys(shm_object_t *obj, unsigned long index);

/* Enumeration for /dev/shm readdir: fills `name` for the idx'th live object
 * and returns 1, or returns 0 when idx is past the end. */
int shm_enumerate(unsigned idx, char *name, size_t cap);

/* ---- System V IPC view of the same objects -----------------------------
 *
 * shmget() names segments by integer key rather than by string, and hands back
 * an identifier that means the same thing in every process — which is the
 * whole point, since one process creates a segment and passes the id to
 * another (that is exactly how the MIT-SHM extension works).  Both views map
 * onto one object type so a segment is a segment however it was created.
 */
#define SHM_SYSV_KEY_PRIVATE 0 /* IPC_PRIVATE: never matched by key */

/* Find or create the segment for `key`.  `out_id` receives the identifier.
 * `create` allows creation, `excl` fails if it already exists. */
shm_object_t *shm_sysv_get(int key, unsigned long size, int create, int excl,
			   unsigned mode, int *out_id);
/* Resolve an identifier, taking a reference. */
shm_object_t *shm_by_id_get(int id);
/* The identifier for an object (>= 1), or -1. */
int shm_id_of(const shm_object_t *obj);
/* Fill `name` with the object's name; returns 0 on success. */
int shm_name_of(const shm_object_t *obj, char *name, size_t cap);

#endif /* _KERNEL_MM_SHM_H_ */
