/*
 * sys/sysmacros.h - taking a device number apart, and putting one together.
 *
 * A dev_t packs a major number (which driver) and a minor number (which device
 * that driver owns) into one integer, and how they are packed is a property of
 * the kernel, not of the caller.  These macros are the only sanctioned way to
 * get at either half; code that shifts and masks by hand breaks the day the
 * encoding changes.
 *
 * The encoding here is the classic one: minor in the low 8 bits, major above
 * it.  That is exactly what the kernel writes into st_rdev (see the st_rdev
 * assignments in kernel/fs/devfs.c), and these macros must agree with it --
 * they are not free to pick a roomier layout, because the two would then
 * disagree about every device number in the system.
 *
 * The practical consequence is that minor numbers run 0..255.  That is the
 * kernel's limit, not this header's.
 */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* The casts matter: dev_t is wider than int, and without them a major number
 * with the top bit set would sign-extend when shifted back. */
#define major(dev)        ((unsigned int)(((dev_t)(dev) >> 8) & 0xffu))
#define minor(dev)        ((unsigned int)((dev_t)(dev) & 0xffu))
#define makedev(maj, min) ((dev_t)((((dev_t)(maj) & 0xffu) << 8) | \
				   ((dev_t)(min) & 0xffu)))

/* Underscored spellings.  Some code uses these to sidestep a clash with a
 * local variable or function called major/minor -- the macros above have no
 * scope and will happily eat an identifier. */
#define gnu_dev_major(dev)        major(dev)
#define gnu_dev_minor(dev)        minor(dev)
#define gnu_dev_makedev(maj, min) makedev(maj, min)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSMACROS_H */
