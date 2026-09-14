// LikeOS-64 -- the display microcontroller's firmware image (pure parser).
//
// The DMC firmware package: a CSS header (the signed-module wrapper),
// a package header listing one image per silicon stepping, and per
// image a DMC header (program size, a few register writes to make after
// loading) followed by the program.  intel_dmc_parse() picks the image
// for a stepping and checks it; intel_dmc.c writes it into the
// controller's memory.
#ifndef KERNEL_DEV_GPU_I915_INTEL_DMC_H
#define KERNEL_DEV_GPU_I915_INTEL_DMC_H

#include <kernel/uapi/types.h>

#define INTEL_DMC_MAX_MMIO 20
#define INTEL_DMC_ANY_STEPPING '*'

struct intel_dmc_image {
	uint32_t version; /* major << 16 | minor */
	uint32_t start_mmioaddr; /* where the program is written */
	uint32_t mmio_count;
	uint32_t mmioaddr[INTEL_DMC_MAX_MMIO];
	uint32_t mmiodata[INTEL_DMC_MAX_MMIO];
	const uint8_t *payload; /* into the file: program dwords, little-endian */
	uint32_t payload_dwords;
	char stepping, substepping; /* of the image chosen */
	uint32_t header_ver; /* 1 (Gen9-11) or 3 (Gen12+) */
};

/* Errors */
#define INTEL_DMC_EBAD_CSS -1
#define INTEL_DMC_EBAD_PACKAGE -2
#define INTEL_DMC_ENO_IMAGE -3
#define INTEL_DMC_EBAD_HEADER -4
#define INTEL_DMC_ETOO_BIG -5
#define INTEL_DMC_EBAD_MMIO -6

/* Parse `fw' (`len' bytes) for the image matching `stepping'/`substepping'
 * (an exact match first, then the stepping with any substepping, then
 * the wildcard image).  `max_fw_dwords' bounds the program; the register
 * writes must fall in [mmio_lo, mmio_hi].  `v1_start' is where a
 * version-1 image is loaded (its header carries no address). */
int intel_dmc_parse(const uint8_t *fw, unsigned len, char stepping, char substepping,
		    uint32_t max_fw_dwords, uint32_t mmio_lo, uint32_t mmio_hi,
		    uint32_t v1_start, struct intel_dmc_image *out);

#endif
