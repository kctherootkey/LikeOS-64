// LikeOS - byte order of on-wire and on-device data
//
// Device structures (NVMe queues and identify data, PCI configuration
// space, GPT, ATA identify) are defined by their specifications as
// little-endian or big-endian byte sequences.  LikeOS runs on x86-64,
// which is little-endian, so a little-endian field reads as a plain integer
// and the conversions below compile to nothing; the big-endian ones swap.
// They exist so a structure says what its fields ARE (__le32 rather than
// uint32_t) and every access says what it DOES (le32_to_cpu), which keeps
// a driver honest about which values come from a device.
//
// The __le and __be types are plain integer typedefs: the compiler does not
// stop a mix-up, the names and the conversions document it.
//
// Unaligned accessors: device data at an odd offset (a field inside a log
// page or a SCSI CDB) is read byte by byte, never through a cast pointer
// that the compiler could assume aligned.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_BYTEORDER_H
#define KERNEL_KE_BYTEORDER_H

#include <kernel/uapi/types.h>

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;

static inline uint16_t swab16(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t swab32(uint32_t v)
{
	return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
	       ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

static inline uint64_t swab64(uint64_t v)
{
	return ((uint64_t)swab32((uint32_t)v) << 32) |
	       (uint64_t)swab32((uint32_t)(v >> 32));
}

/* Little-endian: the native order. */
static inline __le16 cpu_to_le16(uint16_t v)
{
	return v;
}
static inline __le32 cpu_to_le32(uint32_t v)
{
	return v;
}
static inline __le64 cpu_to_le64(uint64_t v)
{
	return v;
}
static inline uint16_t le16_to_cpu(__le16 v)
{
	return v;
}
static inline uint32_t le32_to_cpu(__le32 v)
{
	return v;
}
static inline uint64_t le64_to_cpu(__le64 v)
{
	return v;
}

/* Big-endian: swapped. */
static inline __be16 cpu_to_be16(uint16_t v)
{
	return swab16(v);
}
static inline __be32 cpu_to_be32(uint32_t v)
{
	return swab32(v);
}
static inline __be64 cpu_to_be64(uint64_t v)
{
	return swab64(v);
}
static inline uint16_t be16_to_cpu(__be16 v)
{
	return swab16(v);
}
static inline uint32_t be32_to_cpu(__be32 v)
{
	return swab32(v);
}
static inline uint64_t be64_to_cpu(__be64 v)
{
	return swab64(v);
}

/* In-place forms for a field updated by read-modify-write. */
static inline void le16_add_cpu(__le16 *var, uint16_t val)
{
	*var = cpu_to_le16((uint16_t)(le16_to_cpu(*var) + val));
}
static inline void le32_add_cpu(__le32 *var, uint32_t val)
{
	*var = cpu_to_le32(le32_to_cpu(*var) + val);
}
static inline void le64_add_cpu(__le64 *var, uint64_t val)
{
	*var = cpu_to_le64(le64_to_cpu(*var) + val);
}

/* Unaligned little-/big-endian loads and stores, byte by byte. */
static inline uint16_t get_unaligned_le16(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return (uint16_t)(b[0] | (b[1] << 8));
}
static inline uint32_t get_unaligned_le32(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline uint64_t get_unaligned_le64(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return (uint64_t)get_unaligned_le32(b) |
	       ((uint64_t)get_unaligned_le32(b + 4) << 32);
}
static inline uint16_t get_unaligned_be16(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return (uint16_t)((b[0] << 8) | b[1]);
}
static inline uint32_t get_unaligned_be32(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}
static inline uint64_t get_unaligned_be64(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return ((uint64_t)get_unaligned_be32(b) << 32) |
	       (uint64_t)get_unaligned_be32(b + 4);
}
static inline void put_unaligned_le16(uint16_t v, void *p)
{
	uint8_t *b = (uint8_t *)p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}
static inline void put_unaligned_le32(uint32_t v, void *p)
{
	uint8_t *b = (uint8_t *)p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}
static inline void put_unaligned_le64(uint64_t v, void *p)
{
	put_unaligned_le32((uint32_t)v, p);
	put_unaligned_le32((uint32_t)(v >> 32), (uint8_t *)p + 4);
}
static inline void put_unaligned_be16(uint16_t v, void *p)
{
	uint8_t *b = (uint8_t *)p;

	b[0] = (uint8_t)(v >> 8);
	b[1] = (uint8_t)v;
}
static inline void put_unaligned_be32(uint32_t v, void *p)
{
	uint8_t *b = (uint8_t *)p;

	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
}
static inline void put_unaligned_be64(uint64_t v, void *p)
{
	put_unaligned_be32((uint32_t)(v >> 32), p);
	put_unaligned_be32((uint32_t)v, (uint8_t *)p + 4);
}

#endif /* KERNEL_KE_BYTEORDER_H */
