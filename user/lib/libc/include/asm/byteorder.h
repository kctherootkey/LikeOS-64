/*
 * <asm/byteorder.h> -- the byte order of this machine and the conversions
 * between it and a fixed little- or big-endian representation, in the
 * spelling of the kernel interface headers (__cpu_to_le32() and friends).
 *
 * x86-64 is little-endian: the little-endian conversions are identities
 * and the big-endian ones swap.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _ASM_BYTEORDER_H
#define _ASM_BYTEORDER_H

#include <linux/types.h>

#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __LITTLE_ENDIAN_BITFIELD
#define __LITTLE_ENDIAN_BITFIELD
#endif

#define __swab16(x) ((__u16)__builtin_bswap16((__u16)(x)))
#define __swab32(x) ((__u32)__builtin_bswap32((__u32)(x)))
#define __swab64(x) ((__u64)__builtin_bswap64((__u64)(x)))

#define __cpu_to_le16(x) ((__force __le16)(__u16)(x))
#define __cpu_to_le32(x) ((__force __le32)(__u32)(x))
#define __cpu_to_le64(x) ((__force __le64)(__u64)(x))
#define __le16_to_cpu(x) ((__force __u16)(__le16)(x))
#define __le32_to_cpu(x) ((__force __u32)(__le32)(x))
#define __le64_to_cpu(x) ((__force __u64)(__le64)(x))

#define __cpu_to_be16(x) ((__force __be16)__swab16(x))
#define __cpu_to_be32(x) ((__force __be32)__swab32(x))
#define __cpu_to_be64(x) ((__force __be64)__swab64(x))
#define __be16_to_cpu(x) __swab16((__force __u16)(__be16)(x))
#define __be32_to_cpu(x) __swab32((__force __u32)(__be32)(x))
#define __be64_to_cpu(x) __swab64((__force __u64)(__be64)(x))

#endif /* _ASM_BYTEORDER_H */
