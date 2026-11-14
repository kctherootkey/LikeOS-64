/* libgpt -- CRC-32 as GPT uses it.
 *
 * The IEEE 802.3 CRC: reflected polynomial 0xEDB88320, initial value and
 * final XOR all ones.  The GPT header carries the CRC of its first
 * header_size bytes (with the CRC field itself zero) and the CRC of the
 * whole partition entry array.
 *
 * Table driven, eight bits at a time; the table is built once on first use
 * from the polynomial (no 1 KiB of literal constants to get wrong).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stddef.h>
#include <stdint.h>

#include "gpt.h"

#define GPT_CRC32_POLY 0xEDB88320u

static uint32_t g_crc_table[256];
static volatile int g_crc_ready;

static void gpt_crc32_build(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ GPT_CRC32_POLY : c >> 1;
		g_crc_table[i] = c;
	}
	/* Two threads building it at once write the same values: harmless. */
	__atomic_store_n(&g_crc_ready, 1, __ATOMIC_RELEASE);
}

uint32_t gpt_crc32(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	if (!__atomic_load_n(&g_crc_ready, __ATOMIC_ACQUIRE))
		gpt_crc32_build();
	crc = ~crc;
	while (len >= 4) {
		crc = g_crc_table[(crc ^ p[0]) & 0xff] ^ (crc >> 8);
		crc = g_crc_table[(crc ^ p[1]) & 0xff] ^ (crc >> 8);
		crc = g_crc_table[(crc ^ p[2]) & 0xff] ^ (crc >> 8);
		crc = g_crc_table[(crc ^ p[3]) & 0xff] ^ (crc >> 8);
		p += 4;
		len -= 4;
	}
	while (len--)
		crc = g_crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}
