// LikeOS -- vmwgfx: the device's 3D capabilities.
//
// What the 3D device can do is a table of capability words.  Where it has
// guest-backed objects the table is read once, one SVGA3D_DEVCAP_* index at
// a time through SVGA_REG_DEV_CAP, into vmw_device.devcaps[].  An older
// device keeps it in the FIFO register area instead, starting at
// SVGA_FIFO_3D_CAPS, as a chain of SVGA3dCapsRecord records.
//
// DRM_VMW_GET_3D_CAP hands the table out in the format the client reads:
//   - a client that knows about guest-backed objects (it asked for
//     DRM_VMW_PARAM_MAX_MOB_MEMORY first, see vmw_ioctl.c) gets the flat
//     array, one word per index;
//   - one that does not, on a guest-backed device, gets the array
//     translated into the record format it expects: one DEVCAPS record of
//     (index, value) pairs, closed by an empty record;
//   - on a device without guest-backed objects everybody gets the FIFO
//     records as the device wrote them.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2021 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/mm/memory.h>

/* The table as a client unaware of guest-backed objects reads it: one
 * record of (index, value) pairs.  vmw_devcaps_size() adds a word for the
 * empty record that ends the chain. */
struct svga_3d_compat_cap {
	SVGA3dFifoCapsRecordHeader header;
	SVGA3dFifoCapPair pairs[SVGA3D_DEVCAP_MAX];
};

/* Is the table the device's guest-backed array?
 *
 * The array is read, and is only worth anything, when guest-backed objects
 * are in use: vmwgfx_init() fills it only then, and turns has_gb off
 * (with 3D) when their setup fails, so the device's SVGA_CAP_GBOBJECTS bit
 * alone would describe a table that was never read. */
static bool vmw_devcaps_gb(const struct vmw_device *v)
{
	return v->has_gb != 0;
}

/* A version of the client stack used MULTISAMPLE_MASKABLESAMPLES to learn
 * how many samples the device supports.  Multisampling was never offered
 * for surfaces backed by a MOB, and the device marks that capability
 * deprecated, so such a client is told 0. */
static uint32_t vmw_mask_legacy_multisample(unsigned int cap, uint32_t fmt_value)
{
	if (cap == SVGA3D_DEVCAP_DEAD5)
		return 0;
	return fmt_value;
}

static int vmw_fill_compat_cap(struct vmw_device *v, void *bounce, size_t size)
{
	struct svga_3d_compat_cap *compat_cap = bounce;
	size_t pair_offset = offsetof(struct svga_3d_compat_cap, pairs);
	unsigned int max_size;
	unsigned int i;

	if (size < pair_offset)
		return -EINVAL;

	max_size = (unsigned int)((size - pair_offset) /
				  sizeof(SVGA3dFifoCapPair));
	if (max_size > SVGA3D_DEVCAP_MAX)
		max_size = SVGA3D_DEVCAP_MAX;

	compat_cap->header.length = (uint32_t)((pair_offset +
				     max_size * sizeof(SVGA3dFifoCapPair)) /
				    sizeof(uint32_t));
	compat_cap->header.type = SVGA3D_FIFO_CAPS_RECORD_DEVCAPS;

	for (i = 0; i < max_size; ++i) {
		compat_cap->pairs[i][0] = i;
		compat_cap->pairs[i][1] =
			vmw_mask_legacy_multisample(i, v->devcaps[i]);
	}
	return 0;
}

/* Read the guest-backed table.
 *
 * One word past the last index (SVGA3D_DEVCAP_MAX itself) is read as well
 * and handed out with the rest: that is the size this driver has always
 * reported in DRM_VMW_PARAM_3D_CAPS_SIZE, and the client sizes its own
 * table from that answer, so changing it would change what every client
 * already running here sees. */
int vmw_devcaps_create(struct vmw_device *v)
{
	if (!vmw_devcaps_gb(v))
		return 0;
	for (uint32_t i = 0; i <= SVGA3D_DEVCAP_MAX; i++) {
		vmsvga2_hw_write_reg(SVGA_REG_DEV_CAP, i);
		v->devcaps[i] = vmsvga2_hw_read_reg(SVGA_REG_DEV_CAP);
	}
	return 0;
}

/* The table is part of the device structure; forgetting it is all there
 * is to do. */
void vmw_devcaps_destroy(struct vmw_device *v)
{
	mm_memset(v->devcaps, 0, sizeof(v->devcaps));
}

uint32_t vmw_devcaps_size(const struct vmw_device *v, bool gb_aware)
{
	/* No 3D, no table: the FIFO area exists on every device, but what
	 * it holds is only meaningful when the host offers 3D. */
	if (!v->has_3d)
		return 0;
	if (vmw_devcaps_gb(v) && gb_aware)
		return (SVGA3D_DEVCAP_MAX + 1) * sizeof(uint32_t);
	if (vmw_devcaps_gb(v))
		return sizeof(struct svga_3d_compat_cap) + sizeof(uint32_t);
	return (SVGA_FIFO_3D_CAPS_LAST - SVGA_FIFO_3D_CAPS + 1) *
	       sizeof(uint32_t);
}

/* `dst' is zeroed by the caller and dst_size is at most vmw_devcaps_size():
 * the compat format relies on the zero word after its record to end the
 * chain. */
int vmw_devcaps_copy(struct vmw_device *v, bool gb_aware, void *dst,
		     uint32_t dst_size)
{
	if (!v->has_3d)
		return -EINVAL;
	if (vmw_devcaps_gb(v) && gb_aware) {
		if (dst_size > sizeof(v->devcaps))
			dst_size = sizeof(v->devcaps);
		mm_memcpy(dst, v->devcaps, dst_size);
		return 0;
	}
	if (vmw_devcaps_gb(v))
		return vmw_fill_compat_cap(v, dst, dst_size);

	/* The FIFO records, word by word through the hardware layer (the
	 * FIFO mapping is its own); a size that ends inside a word gets
	 * that word's leading bytes. */
	uint32_t max_words = SVGA_FIFO_3D_CAPS_LAST - SVGA_FIFO_3D_CAPS + 1;
	uint8_t *out = dst;

	for (uint32_t i = 0; i < max_words && i * 4 < dst_size; i++) {
		uint32_t w = vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_CAPS + i);
		uint32_t n = dst_size - i * 4 < 4 ? dst_size - i * 4 : 4;

		mm_memcpy(out + i * 4, &w, n);
	}
	return 0;
}
