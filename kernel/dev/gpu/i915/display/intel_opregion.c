// LikeOS -- the ACPI OpRegion of Intel graphics.
//
// An 8 KB region in system memory, named by the ASLS config register,
// through which firmware and driver talk about the display: the VBT is
// inside it (or, when too large, at an address it names), and its
// mailboxes carry the firmware's requests (backlight, display switch)
// and the driver's declaration that it owns the display.
//
// A discrete card (DG2, Battlemage) has no OpRegion; its VBT is in the
// option ROM image in the card's SPI flash, which the display reads one
// dword at a time through a pair of registers: an address within the
// option ROM's region, then a read of the trigger register returns the
// dword there.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2024-2026 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define OPREGION_SIZE (8 * 1024)
#define OPREGION_HEADER_SIZE 0x100
#define OPREGION_ACPI_OFFSET 0x100 /* mailbox 1 */
#define OPREGION_SWSCI_OFFSET 0x200 /* mailbox 2 */
#define OPREGION_ASLE_OFFSET 0x300 /* mailbox 3 */
#define OPREGION_VBT_OFFSET 0x400 /* mailbox 4: the VBT, 6 KB */
#define OPREGION_ASLE_EXT_OFFSET 0x1C00 /* mailbox 5 */

/* header fields */
#define OPREGION_H_SIGNATURE 0x00 /* "IntelGraphicsMem" */
#define OPREGION_H_SIZE 0x10 /* KB */
#define OPREGION_H_OVER 0x14 /* version */
#define OPREGION_H_MBOXES 0x58 /* supported mailboxes */
#define OPREGION_H_DVER 0x5C
/* The raw VBT's address (2.0+) and size, in the ASLE mailbox (#3) --
 * not in the header. */
#define ASLE_RVDA 0xBA /* u64: physical (2.0) or from the OpRegion (2.1+) */
#define ASLE_RVDS 0xC2 /* u32 */
#define OPREGION_MBOX_ASLE (1 << 2)
#define OPREGION_MBOX_ASLE_EXT (1 << 4) /* mailbox 5 in use: the VBT ends before it */

/* mailbox 1 (ACPI) */
#define ACPI_DRDY 0x00 /* driver ready */
#define ACPI_CSTS 0x04
#define ACPI_CEVT 0x08
#define ACPI_DIDL 0x10 /* supported display ids (8 x u32) */
#define ACPI_CPDL 0x30
#define ACPI_CADL 0x50 /* currently attached (8 x u32) */
#define ACPI_NADL 0x70
#define ACPI_ASLP 0x90
#define ACPI_TIDX 0x94
#define ACPI_CHPD 0x98
#define ACPI_CLID 0x9C
#define ACPI_CDCK 0xA0
#define ACPI_SXSW 0xA4
#define ACPI_EVTS 0xA8
#define ACPI_CNOT 0xAC
#define ACPI_NRDY 0xB0

/* mailbox 3 (ASLE) */
#define ASLE_ARDY 0x00 /* driver readiness */
#define ASLE_ASLC 0x04 /* commands from the firmware */
#define ASLE_TCHE 0x08 /* technology enabled: bit 0 = ALS, 1 = backlight, 3 = PFIT */
#define ASLE_ALSI 0x0C
#define ASLE_BCLP 0x10 /* backlight brightness requested */
#define ASLE_PFIT 0x14
#define ASLE_CBLV 0x18 /* current brightness */
#define ASLE_ARDY_READY 1
#define ASLE_TCHE_BLC_EN (1 << 1)

/* The card's SPI flash: the region the option ROM is in, its offset
 * there, and the address/trigger pair that reads a dword of it. */
#define PRIMARY_SPI_TRIGGER 0x102040
#define PRIMARY_SPI_ADDRESS 0x102080
#define PRIMARY_SPI_REGIONID 0x102084
#define SPI_STATIC_REGIONS 0x102090
#define OPTIONROM_SPI_REGIONID_MASK 0xffu
#define OROM_OFFSET 0x1020c0
#define OROM_OFFSET_MASK (0x1fu << 16)
#define SPI_OROM_SIZE 0x200000u

/* The VBT header: "$VBT" and the name (20 bytes), version, header size,
 * the VBT's size (u16 at 24), checksum, reserved, the BDB's offset (u32
 * at 28); the BDB header is 22 bytes. */
#define VBT_SIGNATURE_LE 0x54425624u /* "$VBT" */
#define VBT_H_SIZE 24
#define VBT_H_BDB_OFFSET 28
#define VBT_HEADER_LEN 48
#define BDB_HEADER_LEN 22

static uint32_t rd32(const void *base, uint32_t off)
{
	return *(volatile const uint32_t *)((const uint8_t *)base + off);
}

static void wr32(void *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)((uint8_t *)base + off) = v;
}

static uint32_t spi_read32(struct i915_device *i915, uint32_t rom_offset, uint32_t off)
{
	i915_write32(i915, PRIMARY_SPI_ADDRESS, rom_offset + off);
	return i915_read32(i915, PRIMARY_SPI_TRIGGER);
}

/* A discrete card's VBT from the option ROM in its SPI flash: the first
 * "$VBT" in the image, checked to be whole before it is copied. */
static void vbt_from_spi(struct i915_device *i915)
{
	struct intel_opregion *op = &i915->display.opregion;
	uint32_t region = i915_read32(i915, SPI_STATIC_REGIONS) & OPTIONROM_SPI_REGIONID_MASK;
	uint32_t rom, off, size, bdb;

	i915_write32(i915, PRIMARY_SPI_REGIONID, region);
	rom = i915_read32(i915, OROM_OFFSET) & OROM_OFFSET_MASK;
	for (off = 0; off < SPI_OROM_SIZE; off += 4)
		if (spi_read32(i915, rom, off) == VBT_SIGNATURE_LE)
			break;
	if (off >= SPI_OROM_SIZE) {
		kprintf("[drm] i915: no VBT in the card's option ROM\n");
		return;
	}
	if (VBT_HEADER_LEN > SPI_OROM_SIZE - off) {
		kprintf("[drm] i915: the option ROM's VBT header is incomplete\n");
		return;
	}
	size = spi_read32(i915, rom, off + VBT_H_SIZE) & 0xffff;
	bdb = spi_read32(i915, rom, off + VBT_H_BDB_OFFSET);
	if (size < VBT_HEADER_LEN || size > SPI_OROM_SIZE - off || bdb > size ||
	    BDB_HEADER_LEN > size - bdb) {
		kprintf("[drm] i915: the option ROM's VBT is incomplete (%u bytes, BDB at %u)\n",
			size, bdb);
		return;
	}
	op->rom_vbt = kalloc((size + 3) & ~3u);
	if (!op->rom_vbt)
		return;
	for (uint32_t i = 0; i < size; i += 4) {
		uint32_t v = spi_read32(i915, rom, off + i);
		for (uint32_t b = 0; b < 4 && i + b < size; b++)
			op->rom_vbt[i + b] = (uint8_t)(v >> (8 * b));
	}
	op->vbt = op->rom_vbt;
	op->vbt_size = size;
	i915_dbg("[drm] i915: VBT from the SPI flash's option ROM (%u bytes)\n", size);
}

static int opregion_map(struct i915_device *i915)
{
	struct intel_opregion *op = &i915->display.opregion;

	uint32_t asls = pci_cfg_read32_dev(i915->pci, I915_PCI_ASLS);
	if (!asls) {
		kprintf("[drm] i915: no ACPI OpRegion (ASLS is zero)\n");
		return -ENODEV;
	}
	op->phys = asls;
	/* System memory the firmware reserved: mapped uncached through the
	 * direct map so mailbox writes are seen at once. */
	op->virt = (void *)mm_map_mmio_flags(op->phys, OPREGION_SIZE / 4096,
					     MM_MMIO_UC);
	if (!op->virt)
		return -ENOMEM;
	op->size = OPREGION_SIZE;
	const uint8_t *hdr = op->virt;
	static const char sig[16] = "IntelGraphicsMem";
	for (int i = 0; i < 16; i++) {
		if (hdr[i] != (uint8_t)sig[i]) {
			kprintf("[drm] i915: OpRegion at %llx has no signature\n",
				(unsigned long long)op->phys);
			mm_unmap_mmio((uint64_t)op->virt, OPREGION_SIZE / 4096);
			op->virt = NULL;
			return -ENODEV;
		}
	}
	uint32_t over = rd32(hdr, OPREGION_H_OVER);
	uint32_t mboxes = rd32(hdr, OPREGION_H_MBOXES);
	op->asle_present = !!(mboxes & OPREGION_MBOX_ASLE);

	/* The version is four bytes: reserved, revision, minor, major --
	 * so the major number is the top one, not the top half. */
	uint32_t major = (over >> 24) & 0xff;
	uint32_t minor = (over >> 16) & 0xff;

	/* The VBT: where the ASLE mailbox (2.0+) points, when it does and
	 * what is there is a VBT; else mailbox 4. */
	if (major >= 2 && op->asle_present) {
		const uint8_t *asle = hdr + OPREGION_ASLE_OFFSET;
		uint64_t rvda = (uint64_t)rd32(asle, ASLE_RVDA) |
				((uint64_t)rd32(asle, ASLE_RVDA + 4) << 32);
		uint32_t rvds = rd32(asle, ASLE_RVDS);
		if (rvda && rvds) {
			/* 2.1+: relative to the OpRegion; 2.0: absolute */
			uint64_t phys = (major > 2 || minor >= 1) ? op->phys + rvda : rvda;
			uint32_t pages = (uint32_t)(((phys & 0xfff) + rvds + 4095) / 4096);
			op->rvda_virt = (void *)mm_map_mmio_flags(phys, pages,
								  MM_MMIO_UC);
			if (op->rvda_virt) {
				/* (the mapping keeps the address's offset in its page) */
				const uint8_t *v = (const uint8_t *)op->rvda_virt;
				if (v[0] == '$' && v[1] == 'V' && v[2] == 'B' && v[3] == 'T') {
					op->rvda_pages = pages;
					op->vbt = v;
					op->vbt_size = rvds;
				} else {
					kprintf("[drm] i915: the VBT the OpRegion points at (%llx, %u bytes) is not one\n",
						(unsigned long long)phys, rvds);
					mm_unmap_mmio((uint64_t)op->rvda_virt, pages);
					op->rvda_virt = NULL;
				}
			}
		}
	}
	if (!op->vbt) {
		/* mailbox 4, up to mailbox 5 when that is in use (some boards'
		 * VBT runs on into it when it is not) */
		const uint8_t *v = hdr + OPREGION_VBT_OFFSET;
		if (v[0] == '$' && v[1] == 'V' && v[2] == 'B' && v[3] == 'T') {
			op->vbt = v;
			op->vbt_size = ((mboxes & OPREGION_MBOX_ASLE_EXT) ? OPREGION_ASLE_EXT_OFFSET :
									  OPREGION_SIZE) -
				       OPREGION_VBT_OFFSET;
		} else {
			kprintf("[drm] i915: the OpRegion carries no VBT\n");
		}
	}
	i915_dbg("[drm] i915: OpRegion %u.%u at %llx, mailboxes %x%s%s\n",
		major, minor, (unsigned long long)op->phys, mboxes,
		op->vbt ? ", VBT" : ", no VBT", op->asle_present ? ", ASLE" : "");
	return 0;
}

int intel_opregion_init(struct i915_device *i915)
{
	struct intel_opregion *op = &i915->display.opregion;
	int rc;

	mm_memset(op, 0, sizeof(*op));
	rc = opregion_map(i915);
	/* without the OpRegion's, a discrete card's own VBT */
	if (!op->vbt && (i915->info->flags & I915_INFO_IS_DGFX))
		vbt_from_spi(i915);
	return rc;
}

void intel_opregion_fini(struct i915_device *i915)
{
	struct intel_opregion *op = &i915->display.opregion;

	if (op->rvda_virt) {
		mm_unmap_mmio((uint64_t)op->rvda_virt, op->rvda_pages);
		op->rvda_virt = NULL;
	}
	if (op->virt) {
		mm_unmap_mmio((uint64_t)op->virt, OPREGION_SIZE / 4096);
		op->virt = NULL;
	}
	op->vbt = NULL;
	if (op->rom_vbt) {
		kfree(op->rom_vbt);
		op->rom_vbt = NULL;
	}
}

void intel_opregion_driver_ready(struct i915_device *i915)
{
	struct intel_opregion *op = &i915->display.opregion;
	uint8_t *base = op->virt;

	if (!base)
		return;
	/* Mailbox 1: the driver is here; every display id the firmware
	 * lists is "attached" as far as it is concerned. */
	wr32(base, OPREGION_ACPI_OFFSET + ACPI_DRDY, 1);
	for (int i = 0; i < 8; i++) {
		uint32_t did = rd32(base, OPREGION_ACPI_OFFSET + ACPI_DIDL + i * 4);
		wr32(base, OPREGION_ACPI_OFFSET + ACPI_CADL + i * 4, did);
	}
	/* Mailbox 3: backlight requests may come to the driver. */
	if (op->asle_present) {
		wr32(base, OPREGION_ASLE_OFFSET + ASLE_TCHE, ASLE_TCHE_BLC_EN);
		wr32(base, OPREGION_ASLE_OFFSET + ASLE_ARDY, ASLE_ARDY_READY);
	}
}
