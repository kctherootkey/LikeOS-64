// LikeOS -- loading the display microcontroller (DMC).
//
// The DMC is a small controller in the display engine that runs the
// display's deep power states: with the pipes idle it can power the
// display core down and bring it back on its own.  Its program comes as
// a firmware file (one per platform, chosen per silicon stepping);
// loading it is writing the program into the controller's memory
// through a register window and making the few register writes the
// image asks for.
//
// The program is loaded so that the hardware is as the firmware package
// expects it; the power states themselves (DC5/DC6) are not enabled by
// this driver.  Entering them is the controller's decision whenever the
// display is idle, and getting back out relies on power-well and clock
// state this driver does not track yet; with the states off the display
// simply stays powered, which is what every other part of the driver
// assumes.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_dmc.h>
#include <kernel/io/console.h>
#include <kernel/ke/firmware.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The controller's memory window and the range its register writes may
 * touch. */
#define DMC_V1_MMIO_START 0x80000
#define DMC_MMIO_LO 0x80000
#define DMC_MMIO_HI 0x8FFFF
#define DMC_MAX_FW_DWORDS 0x6000

/* The silicon stepping letters the packages are keyed by, from the PCI
 * revision. */
static void stepping_for(struct i915_device *i915, char *st, char *sub)
{
	uint8_t rev = pci_cfg_read8(i915->pci, 0x08);
	*st = INTEL_DMC_ANY_STEPPING;
	*sub = INTEL_DMC_ANY_STEPPING;
	switch (i915->info->platform) {
	case I915_PLATFORM_SKYLAKE:
		if (rev <= 9) {
			*st = (char)('A' + rev);
			*sub = '0';
		}
		break;
	case I915_PLATFORM_KABYLAKE:
	case I915_PLATFORM_COFFEELAKE: {
		static const char kbl[][2] = { { 'A', '0' }, { 'B', '0' }, { 'C', '0' },
					       { 'D', '0' }, { 'F', '0' }, { 'C', '0' },
					       { 'D', '1' }, { 'G', '0' } };
		if (rev < 8) {
			*st = kbl[rev][0];
			*sub = kbl[rev][1];
		}
		break;
	}
	case I915_PLATFORM_BROXTON:
	case I915_PLATFORM_GEMINILAKE: {
		static const char bxt[][2] = { { 'A', '0' }, { 'A', '1' }, { 'A', '2' },
					       { 'B', '0' }, { 'B', '1' }, { 'B', '2' } };
		if (rev < 6) {
			*st = bxt[rev][0];
			*sub = bxt[rev][1];
		}
		break;
	}
	default:
		break;
	}
}

static void dmc_program(struct i915_device *i915, const struct intel_dmc_image *img)
{
	const uint8_t *p = img->payload;
	for (uint32_t i = 0; i < img->payload_dwords; i++, p += 4) {
		uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
			     ((uint32_t)p[3] << 24);
		i915_write32(i915, img->start_mmioaddr + i * 4, v);
	}
	for (uint32_t i = 0; i < img->mmio_count; i++)
		i915_write32(i915, img->mmioaddr[i], img->mmiodata[i]);
	/* keep the cores and the memory-up path out of the controller's
	 * hands while the display is driven */
	uint32_t dbg = i915_read32(i915, DC_STATE_DEBUG);
	dbg |= DC_STATE_DEBUG_MASK_MEMORY_UP;
	if (i915->info->flags & I915_INFO_IS_LP)
		dbg |= DC_STATE_DEBUG_MASK_CORES;
	i915_write32(i915, DC_STATE_DEBUG, dbg);
	(void)i915_read32(i915, DC_STATE_DEBUG);
}

int intel_dmc_load(struct i915_device *i915)
{
	void *data = NULL;
	size_t len = 0;
	struct intel_dmc_image img;
	char st, sub;

	if (!(i915->info->flags & I915_INFO_HAS_DMC) || !i915->info->dmc_fw)
		return 0;
	if (i915->display.dmc_loaded)
		return 0;
	int rc = firmware_request(i915->info->dmc_fw, &data, &len);
	if (rc) {
		kprintf("[drm] i915: DMC firmware %s not available (%d); the display's deep power states stay off\n",
			i915->info->dmc_fw, rc);
		return rc;
	}
	stepping_for(i915, &st, &sub);
	mm_memset(&img, 0, sizeof(img));
	rc = intel_dmc_parse(data, (unsigned)len, st, sub, DMC_MAX_FW_DWORDS, DMC_MMIO_LO,
			     DMC_MMIO_HI, DMC_V1_MMIO_START, &img);
	if (rc) {
		kprintf("[drm] i915: DMC firmware %s rejected (%d) for stepping %c%c\n",
			i915->info->dmc_fw, rc, st, sub);
		firmware_release(data);
		return -EINVAL;
	}
	dmc_program(i915, &img);
	i915->display.dmc_loaded = 1;
	i915->display.dmc_version = img.version;
	kprintf("[drm] i915: DMC %u.%u loaded from %s (image %c%c, %u dwords at %05x, %u register writes); DC states not enabled\n",
		img.version >> 16, img.version & 0xffff, i915->info->dmc_fw, img.stepping,
		img.substepping, img.payload_dwords, img.start_mmioaddr, img.mmio_count);
	firmware_release(data);
	return 0;
}
