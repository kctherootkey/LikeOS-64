// LikeOS-64 -- EDID: what a display says about itself.
//
// The 128-byte base block a monitor or panel answers with over DDC or AUX,
// plus optional extension blocks (CEA-861 for televisions and HDMI sinks),
// parsed into the modes it supports and the facts a driver needs: its
// preferred timing, its physical size, its name, whether it is an HDMI
// sink.  The parser is pure (no kernel dependencies) so the host test
// harness can run it against recorded blocks.
#ifndef KERNEL_DEV_GPU_DRM_EDID_H
#define KERNEL_DEV_GPU_DRM_EDID_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/drm/drm_mode.h>

#define DRM_EDID_BLOCK 128
#define DRM_EDID_MAX_BLOCKS 4 /* base + up to three extensions */
#define DRM_EDID_MAX_MODES 64

struct drm_edid_range {
	uint32_t min_vrefresh, max_vrefresh; /* Hz, 0 = unknown */
	uint32_t min_hfreq, max_hfreq; /* kHz */
	uint32_t max_clock_khz; /* 0 = unknown */
};

struct drm_edid_info {
	uint8_t version, revision;
	char vendor[4]; /* three letters */
	uint16_t product;
	uint32_t serial;
	char name[14]; /* the monitor name descriptor, if any */
	uint32_t mm_width, mm_height; /* physical size, 0 = unknown */
	int digital;
	int bpc; /* bits per colour from the input descriptor, 0 = unknown */
	int is_hdmi; /* CEA extension carries the HDMI vendor block */
	int cea_underscan;
	int has_audio;
	struct drm_edid_range range;
	int has_range;
	struct drm_mode_modeinfo modes[DRM_EDID_MAX_MODES];
	int nmodes;
	int preferred; /* index into modes, -1 = none */
};

/* Parse `len' bytes (a whole number of blocks).  Returns 0 or -EINVAL when
 * the base block is not an EDID (bad header or checksum).  Extension
 * blocks with a bad checksum are skipped, not fatal. */
int drm_edid_parse(const uint8_t *edid, unsigned len, struct drm_edid_info *out);

/* The number of extension blocks the base block announces. */
static inline unsigned drm_edid_extensions(const uint8_t *edid)
{
	return edid[126];
}

/* One block's checksum: the bytes must sum to zero. */
int drm_edid_block_valid(const uint8_t *block);

/* ---- standard timings (drm_modes.c) --------------------------------- */

/* Compute a mode from its geometry with the CVT formula (reduced blanking
 * when `rb'), or the GTF formula.  Fills every field including the name. */
void drm_mode_cvt(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh, int rb);
void drm_mode_gtf(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh);
/* The standard table (DMT and the common CEA timings): entry i, or -1
 * past the end. */
int drm_mode_table_get(int i, struct drm_mode_modeinfo *m);
int drm_mode_table_count(void);
/* A CEA-861 video identification code's mode, 0 when unknown. */
int drm_mode_cea_vic(uint8_t vic, struct drm_mode_modeinfo *m);
/* Same geometry and timing (name and type ignored). */
int drm_mode_equal(const struct drm_mode_modeinfo *a,
		   const struct drm_mode_modeinfo *b);
/* Fill in vrefresh and name from the timings. */
void drm_mode_finish(struct drm_mode_modeinfo *m);
/* Is the mode within the sink's range and the caller's clock limit? */
int drm_mode_in_range(const struct drm_mode_modeinfo *m,
		      const struct drm_edid_range *r, uint32_t max_clock_khz,
		      uint32_t max_w, uint32_t max_h);

#endif
