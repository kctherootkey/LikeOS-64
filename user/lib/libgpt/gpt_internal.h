/* libgpt -- helpers shared by the library's modules (not installed).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_GPT_INTERNAL_H
#define LIKEOS_GPT_INTERNAL_H

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "gpt.h"

/* Little-endian loads and stores, byte by byte: the on-disk structures are
 * little-endian whatever the CPU is, and the buffers need not be aligned. */
static inline uint16_t gpt_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t gpt_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t gpt_le64(const uint8_t *p)
{
	return (uint64_t)gpt_le32(p) | ((uint64_t)gpt_le32(p + 4) << 32);
}

static inline void gpt_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void gpt_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void gpt_put64(uint8_t *p, uint64_t v)
{
	gpt_put32(p, (uint32_t)v);
	gpt_put32(p + 4, (uint32_t)(v >> 32));
}

/* Byte offsets inside the 92-byte header. */
#define GH_SIGNATURE     0
#define GH_REVISION      8
#define GH_HEADER_SIZE   12
#define GH_HEADER_CRC    16
#define GH_RESERVED      20
#define GH_MY_LBA        24
#define GH_ALT_LBA       32
#define GH_FIRST_USABLE  40
#define GH_LAST_USABLE   48
#define GH_DISK_GUID     56
#define GH_ENTRIES_LBA   72
#define GH_NUM_ENTRIES   80
#define GH_ENTRY_SIZE    84
#define GH_ENTRIES_CRC   88

/* Byte offsets inside a partition entry. */
#define GE_TYPE          0
#define GE_UUID          16
#define GE_FIRST         32
#define GE_LAST          40
#define GE_ATTRS         48
#define GE_NAME          56

/* The number of sectors an entry array of n entries of size esz takes. */
static inline uint64_t gpt_entry_sectors(uint32_t n, uint32_t esz,
					 uint32_t sector_size)
{
	uint64_t bytes = (uint64_t)n * esz;

	return (bytes + sector_size - 1) / sector_size;
}

/* Writes a sentence into t->errmsg; returns -err. */
int gpt_fail(struct gpt_table *t, int err, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* Decodes one raw entry into a gpt_part (layout.c). */
void gpt_entry_decode(const uint8_t *raw, struct gpt_part *p);
/* Encodes a gpt_part into a raw entry of entry_size bytes (zero padded). */
void gpt_entry_encode(const struct gpt_part *p, uint8_t *raw,
		      uint32_t entry_size);

#endif /* LIKEOS_GPT_INTERNAL_H */
