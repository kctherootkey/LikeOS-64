// LikeOS-64 -- tmpfs: a filesystem kept entirely in RAM, mounted at /ram.
#ifndef LIKEOS_TMPFS_H
#define LIKEOS_TMPFS_H

/* Where it is mounted.  Anything created below this path lives in memory
 * and is gone at the next boot. */
#define TMPFS_MOUNT "/ram"

/* Create the (empty) tree and register the mount.  Called once at boot,
 * after vfs_init(); needs the physical allocator, nothing else. */
void tmpfs_init(void);

#endif /* LIKEOS_TMPFS_H */
