// LikeOS-64 -- the ACPI OpRegion of Intel graphics.
//
// An 8 KB region in system memory, named by the ASLS config register,
// through which firmware and driver talk about the display: the VBT is
// inside it (or, when too large, at an address it names), and its
// mailboxes carry the firmware's requests (backlight, display switch)
// and the driver's declaration that it owns the display.
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
#define OPREGION_VBT_SIZE (OPREGION_ASLE_EXT_OFFSET - OPREGION_VBT_OFFSET)

/* header fields */
#define OPREGION_H_SIGNATURE 0x00 /* "IntelGraphicsMem" */
#define OPREGION_H_SIZE 0x10 /* KB */
#define OPREGION_H_OVER 0x14 /* version */
#define OPREGION_H_MBOXES 0x58 /* supported mailboxes */
#define OPREGION_H_DVER 0x5C
#define OPREGION_H_RVDA 0xBA /* extended VBT address (2.0+), u64 */
#define OPREGION_H_RVDS 0xC2 /* its size, u32 */
#define OPREGION_MBOX_VBT (1 << 3)
#define OPREGION_MBOX_ASLE (1 << 2)

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

static uint32_t rd32(const void *base, uint32_t off)
{
	return *(volatile const uint32_t *)((const uint8_t *)base + off);
}

static void wr32(void *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)((uint8_t *)base + off) = v;
}

int intel_opregion_init(struct i915_device *i915)
{
	struct intel_opregion *op = &i915->display.opregion;

	mm_memset(op, 0, sizeof(*op));
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

	/* The VBT: in mailbox 4 unless the header (2.0+) points elsewhere. */
	if (major >= 2) {
		uint64_t rvda = *(volatile const uint64_t *)(hdr + OPREGION_H_RVDA);
		uint32_t rvds = rd32(hdr, OPREGION_H_RVDS);
		if (rvda && rvds) {
			/* 2.1+: relative to the OpRegion; 2.0: absolute */
			uint64_t phys = (major > 2 || minor >= 1) ? op->phys + rvda : rvda;
			uint32_t pages = (uint32_t)(((phys & 0xfff) + rvds + 4095) / 4096);
			op->rvda_virt = (void *)mm_map_mmio_flags(phys, pages,
								  MM_MMIO_UC);
			if (op->rvda_virt) {
				op->rvda_pages = pages;
				op->vbt = op->rvda_virt;
				op->vbt_size = rvds;
			}
		}
	}
	if (!op->vbt && (mboxes & OPREGION_MBOX_VBT)) {
		op->vbt = hdr + OPREGION_VBT_OFFSET;
		op->vbt_size = OPREGION_VBT_SIZE;
	}
	if (op->vbt && !(op->vbt[0] == '$' && op->vbt[1] == 'V' &&
			 op->vbt[2] == 'B' && op->vbt[3] == 'T')) {
		kprintf("[drm] i915: OpRegion VBT has no signature\n");
		op->vbt = NULL;
		op->vbt_size = 0;
	}
	i915_dbg("[drm] i915: OpRegion %u.%u at %llx, mailboxes %x%s%s\n",
		major, minor, (unsigned long long)op->phys, mboxes,
		op->vbt ? ", VBT" : ", no VBT", op->asle_present ? ", ASLE" : "");
	return 0;
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
