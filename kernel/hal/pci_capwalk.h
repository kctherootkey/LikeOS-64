// LikeOS - PCI capability list walkers and ECAM address math (pure, no
// kernel dependencies)
//
// The classic list: a byte pointer at 0x34 (0x14 for a CardBus bridge),
// each entry [id, next], next pointers dword aligned, at least 0x40; at
// most 48 entries (the space below 256 holds no more), id 0xff ends it.
// The extended list: from 0x100, each header [id:16, version:4,
// next:12], next dword aligned and >= 0x100, at most (4096-256)/8
// entries; a header of 0 (or all-ones) means no extended capabilities.
// Both stop at a loop or a broken pointer instead of running forever.
//
// The reader returns 0 and a dword at a dword-aligned offset, or nonzero
// on failure.  Used by kernel/hal/pci.c and by the host test.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_HAL_PCI_CAPWALK_H
#define KERNEL_HAL_PCI_CAPWALK_H

#include <kernel/uapi/types.h>

typedef int (*pci_capwalk_read_fn)(void *ctx, int where, uint32_t *val);

#define PCI_CAPWALK_TTL 48
#define PCI_CAPWALK_EXT_TTL ((4096 - 256) / 8)

static inline uint8_t pci_capwalk_byte(pci_capwalk_read_fn rd, void *ctx,
				       int where, int *err)
{
	uint32_t v = 0xffffffffu;

	if (rd(ctx, where & ~3, &v))
		*err = 1;
	return (uint8_t)(v >> (8 * (where & 3)));
}

/* Classic capability `cap' after `pos' (0: from the head, given the
 * pointer register `head_reg').  0 when absent. */
static inline uint8_t pci_capwalk_std(pci_capwalk_read_fn rd, void *ctx,
				      int head_reg, uint8_t pos, int cap)
{
	int err = 0, ttl = PCI_CAPWALK_TTL;

	if (pos == 0)
		pos = pci_capwalk_byte(rd, ctx, head_reg, &err);
	else
		pos = pci_capwalk_byte(rd, ctx, pos + 1, &err);
	while (!err && ttl--) {
		uint8_t id;

		if (pos < 0x40)
			break;
		pos &= ~3;
		id = pci_capwalk_byte(rd, ctx, pos, &err);
		if (err || id == 0xff)
			break;
		if (id == cap)
			return pos;
		pos = pci_capwalk_byte(rd, ctx, pos + 1, &err);
	}
	return 0;
}

/* Extended capability `cap' after `start' (0: from 0x100).  0 when absent
 * or when the space is only 256 bytes. */
static inline uint16_t pci_capwalk_ext(pci_capwalk_read_fn rd, void *ctx,
				       uint16_t cfg_size, uint16_t start,
				       int cap)
{
	uint32_t header = 0;
	int ttl = PCI_CAPWALK_EXT_TTL;
	uint16_t pos = start ? start : 256;

	if (cfg_size <= 256 || pos < 256 || pos >= cfg_size)
		return 0;
	if (rd(ctx, pos, &header))
		return 0;
	if (header == 0 || header == 0xffffffffu)
		return 0;
	while (ttl-- > 0) {
		if ((int)(header & 0xffff) == cap && pos != start)
			return pos;
		pos = (uint16_t)((header >> 20) & 0xffc);
		if (pos < 256 || pos >= cfg_size)
			break;
		if (rd(ctx, pos, &header))
			break;
		if (header == 0xffffffffu)
			break;
	}
	return 0;
}

/* ECAM byte offset of (bus, devfn, where) inside a region whose first
 * bus is `bus_start': 1 MiB per bus, 4 KiB per function. */
static inline uint64_t pci_ecam_offset(uint8_t bus_start, uint8_t bus,
				       uint8_t devfn, int where)
{
	return ((uint64_t)(uint8_t)(bus - bus_start) << 20) |
	       ((uint64_t)devfn << 12) | ((uint64_t)where & 0xfff);
}

#endif /* KERNEL_HAL_PCI_CAPWALK_H */
