/*
 * <linux/types.h> -- the fixed-width kernel interface types.
 *
 * Programs that talk to device drivers through ioctl structures (libnvme,
 * nvme-cli, smartctl, the disk utilities) spell the members of those
 * structures with the interface's own type names: __u8 .. __u64 and
 * __s8 .. __s64 for native-endian integers, __le16 .. __be64 for integers
 * stored in a fixed byte order (the order is documentation only; nothing
 * converts automatically), and __aligned_u64 for 64-bit members that must
 * sit on an 8-byte boundary whatever the surrounding structure looks like.
 * The include path is the one those programs use.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_UAPI_TYPES_H
#define _LIKEOS_UAPI_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* The widths of the established x86-64 definitions, spelled the same way
 * (64-bit members are long long), so a structure declared with them has
 * the same type wherever it is compiled. */
typedef signed char __s8;
typedef unsigned char __u8;
typedef short __s16;
typedef unsigned short __u16;
typedef int __s32;
typedef unsigned int __u32;
__extension__ typedef long long __s64;
__extension__ typedef unsigned long long __u64;

/* Annotations a static checker reads; to the compiler they mean nothing. */
#ifndef __bitwise
#define __bitwise
#endif
#ifndef __force
#define __force
#endif

typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;

typedef __u16 __bitwise __sum16;
typedef __u32 __bitwise __wsum;

#define __aligned_u64 __u64 __attribute__((aligned(8)))
#define __aligned_be64 __be64 __attribute__((aligned(8)))
#define __aligned_le64 __le64 __attribute__((aligned(8)))

typedef unsigned __bitwise __poll_t;

/* The kernel interface's own names for the basic system types (x86-64),
 * used by interface headers such as the DRM ones. */
typedef long __kernel_long_t;
typedef unsigned long __kernel_ulong_t;
typedef __kernel_ulong_t __kernel_ino_t;
typedef unsigned int __kernel_mode_t;
typedef int __kernel_pid_t;
typedef int __kernel_ipc_pid_t;
typedef unsigned int __kernel_uid_t;
typedef unsigned int __kernel_gid_t;
typedef __kernel_long_t __kernel_suseconds_t;
typedef int __kernel_daddr_t;
typedef unsigned int __kernel_uid32_t;
typedef unsigned int __kernel_gid32_t;
typedef unsigned short __kernel_old_uid_t;
typedef unsigned short __kernel_old_gid_t;
typedef unsigned long __kernel_old_dev_t;
typedef __kernel_ulong_t __kernel_size_t;
typedef __kernel_long_t __kernel_ssize_t;
typedef __kernel_long_t __kernel_ptrdiff_t;
typedef struct {
	int val[2];
} __kernel_fsid_t;
typedef __kernel_long_t __kernel_off_t;
__extension__ typedef long long __kernel_loff_t;
typedef __kernel_long_t __kernel_old_time_t;
typedef __kernel_long_t __kernel_time_t;
__extension__ typedef long long __kernel_time64_t;
typedef __kernel_long_t __kernel_clock_t;
typedef int __kernel_timer_t;
typedef int __kernel_clockid_t;
typedef char *__kernel_caddr_t;
typedef unsigned short __kernel_uid16_t;
typedef unsigned short __kernel_gid16_t;

#ifdef __cplusplus
}
#endif

/* This header stands for the whole established set (fixed-width types,
 * architecture posix types).  Mark the architecture headers as included, so
 * that an interface header that names one of them directly finds the types
 * already defined here rather than defining the same anonymous structures a
 * second time. */
#define _ASM_GENERIC_INT_LL64_H
#define _ASM_X86_TYPES_H
#define _ASM_GENERIC_TYPES_H
#define _ASM_X86_POSIX_TYPES_64_H
#define _ASM_X86_POSIX_TYPES_H
#define __ASM_GENERIC_POSIX_TYPES_H

#endif /* _LIKEOS_UAPI_TYPES_H */
