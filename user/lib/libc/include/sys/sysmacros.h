/*
 * sys/sysmacros.h - taking a device number apart, and putting one together.
 *
 * A dev_t packs a major number (which driver) and a minor number (which device
 * that driver owns) into one integer, and how they are packed is a property of
 * the kernel, not of the caller.  These macros are the only sanctioned way to
 * get at either half; code that shifts and masks by hand breaks the day the
 * encoding changes.
 *
 * The encoding is the established 64-bit one, and it must agree with the
 * kernel's kdev_encode() (kernel/fs/kdev_t.h), which writes st_dev and
 * st_rdev:
 *
 *   bits  0- 7   minor bits 0-7
 *   bits  8-19   major bits 0-11
 *   bits 20-31   minor bits 8-19
 *   bits 32-43   minor bits 20-31
 *   bits 44-63   major bits 12-31
 *
 * A device with major < 4096 and minor < 256 therefore has exactly the
 * value of the older 8:8 packing (major << 8 | minor); the extended block
 * major 259 and dynamically allocated character majors (NVMe) use the
 * wider fields.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

static __inline__ unsigned int gnu_dev_major(dev_t __dev)
{
	unsigned int __major;

	__major = (unsigned int)(((unsigned long long)__dev &
				  0x00000000000fff00ULL) >> 8);
	__major |= (unsigned int)(((unsigned long long)__dev &
				   0xfffff00000000000ULL) >> 32);
	return __major;
}

static __inline__ unsigned int gnu_dev_minor(dev_t __dev)
{
	unsigned int __minor;

	__minor = (unsigned int)(((unsigned long long)__dev &
				  0x00000000000000ffULL) >> 0);
	__minor |= (unsigned int)(((unsigned long long)__dev &
				   0x00000ffffff00000ULL) >> 12);
	return __minor;
}

static __inline__ dev_t gnu_dev_makedev(unsigned int __major,
					unsigned int __minor)
{
	unsigned long long __dev;

	__dev = ((unsigned long long)(__major & 0x00000fffu)) << 8;
	__dev |= ((unsigned long long)(__major & 0xfffff000u)) << 32;
	__dev |= ((unsigned long long)(__minor & 0x000000ffu)) << 0;
	__dev |= ((unsigned long long)(__minor & 0xffffff00u)) << 12;
	return (dev_t)__dev;
}

/* The short spellings are macros over the functions, so a local variable
 * or function called major/minor can still be shielded with parentheses:
 * (major)(x) calls a function of that name, not this. */
#define major(dev) gnu_dev_major(dev)
#define minor(dev) gnu_dev_minor(dev)
#define makedev(maj, min) gnu_dev_makedev(maj, min)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSMACROS_H */
