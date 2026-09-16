// LikeOS -- the logical ring context image layout (pure: fixed-width
// types only, so the host tests compile it).
//
// What the hardware saves and restores when it switches contexts: a page
// of hardware status followed by the register state, laid out as the
// MI_LOAD_REGISTER_IMM sequences the context restore executes.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_LRC_LAYOUT_H
#define KERNEL_DEV_GPU_I915_LRC_LAYOUT_H

#include <kernel/uapi/types.h>

/* Dword indices into the register-state page. */
struct i915_lrc_layout {
	uint16_t lri0, ctx_ctrl, ring_head, ring_tail, ring_start, ring_ctl;
	uint16_t bb_head_u, bb_head_l, bb_state, sbb_head_u, sbb_head_l, sbb_state;
	uint16_t bb_per_ctx, indirect_ctx, indirect_ctx_off; /* render only */
	uint16_t lri1, timestamp;
	uint16_t pdp3_u, pdp3_l, pdp2_u, pdp2_l, pdp1_u, pdp1_l, pdp0_u, pdp0_l;
	uint16_t lri2, rpcs; /* render only */
	uint16_t lri0_count, lri1_count;
	uint16_t pages; /* size of the image in pages, incl. the ppHWSP */
};

int i915_lrc_layout(unsigned gen_x10, int engine_class, struct i915_lrc_layout *out);
/* The render power and clock state request for a part's topology. */
uint32_t i915_lrc_rpcs(unsigned gen, int is_lp, unsigned slices,
		       unsigned subslices, unsigned eus_per_subslice);
/* Write the register state for an engine into `regs' (the state page),
 * with the ring and page directory it should use, and the power and
 * clock state request (render engine only). */
void i915_lrc_init_regs(uint32_t *regs, const struct i915_lrc_layout *l,
			unsigned gen_x10, uint32_t mmio_base, int engine_class,
			uint32_t ring_ggtt, uint32_t ring_size, uint64_t pml4,
			int inhibit_restore, uint32_t rpcs,
			uint32_t wa_bb_ggtt, uint32_t wa_bb_bytes);
/* The batch the render engine runs at every context restore on Gen9,
 * written to `b' (at least 64 words), with `scratch_ggtt' a global
 * address of 8 free bytes it keeps a register in meanwhile.  Returns
 * the number of words, a multiple of 16 (whole 64-byte lines). */
uint32_t i915_lrc_wa_bb_gen9(uint32_t *b, uint32_t scratch_ggtt);

#endif
