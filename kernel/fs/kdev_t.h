// LikeOS - device numbers: major/minor <-> the 64-bit dev_t userland sees
//
// A device number names a driver (major) and one device of it (minor).
// Inside the kernel they are kept apart as two uint32_t (devfs nodes,
// block devices); they are only packed where they cross into userland --
// st_dev / st_rdev of stat(), mknod()'s argument -- and the packing must
// be exactly what libc's <sys/sysmacros.h> unpacks.
//
// The encoding is the established 64-bit one: a 12-bit major and a 20-bit
// minor in the low 32 bits, with the rest of a (larger) major and minor
// above them:
//
//   bits  0- 7   minor bits 0-7
//   bits  8-19   major bits 0-11
//   bits 20-31   minor bits 8-19
//   bits 32-43   minor bits 20-31   (not used: minors stay below 2^20)
//   bits 44-63   major bits 12-31   (not used: majors stay below 2^12)
//
// So every number with major < 4096 and minor < 256 encodes to exactly the
// old 8:8 value (major << 8 | minor): existing devices keep their st_rdev.
// The extended block major (259) and dynamically allocated character
// majors (NVMe controllers, generic namespace nodes) need the wider fields.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_FS_KDEV_T_H
#define KERNEL_FS_KDEV_T_H

#include <kernel/uapi/types.h>

/* The ranges the system hands out (what the 32-bit part can carry). */
#define KDEV_MAJOR_BITS 12
#define KDEV_MINOR_BITS 20
#define KDEV_MAJOR_MAX ((1u << KDEV_MAJOR_BITS) - 1) /* 4095 */
#define KDEV_MINOR_MAX ((1u << KDEV_MINOR_BITS) - 1) /* 1048575 */

/* Pack major/minor into the 64-bit userland dev_t. */
static inline uint64_t kdev_encode(uint32_t major, uint32_t minor)
{
	uint64_t dev;

	dev = ((uint64_t)(major & 0x00000fffu)) << 8;
	dev |= ((uint64_t)(major & 0xfffff000u)) << 32;
	dev |= ((uint64_t)(minor & 0x000000ffu)) << 0;
	dev |= ((uint64_t)(minor & 0xffffff00u)) << 12;
	return dev;
}

static inline uint32_t kdev_major(uint64_t dev)
{
	uint32_t major;

	major = (uint32_t)((dev & 0x00000000000fff00ULL) >> 8);
	major |= (uint32_t)((dev & 0xfffff00000000000ULL) >> 32);
	return major;
}

static inline uint32_t kdev_minor(uint64_t dev)
{
	uint32_t minor;

	minor = (uint32_t)((dev & 0x00000000000000ffULL) >> 0);
	minor |= (uint32_t)((dev & 0x00000ffffff00000ULL) >> 12);
	return minor;
}

/* Are major/minor inside the ranges the 32-bit form carries?  Every
 * number the kernel registers must be. */
static inline int kdev_valid(uint32_t major, uint32_t minor)
{
	return major <= KDEV_MAJOR_MAX && minor <= KDEV_MINOR_MAX;
}

#endif /* KERNEL_FS_KDEV_T_H */
