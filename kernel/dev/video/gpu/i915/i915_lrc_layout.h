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
	uint16_t lri1, timestamp, timestamp_udw; /* the upper half: Xe2 on */
	uint16_t pdp3_u, pdp3_l, pdp2_u, pdp2_l, pdp1_u, pdp1_l, pdp0_u, pdp0_l;
	uint16_t lri2, rpcs; /* render only */
	uint16_t lri0_count, lri1_count;
	uint16_t pages; /* size of the image in pages, incl. the ppHWSP */
	/* Gen12 on (0 where the layout has none): the context id, the
	 * MI_MODE copy whose stop bit must be clear, the batch offset, the
	 * first general purpose register and the command buffer cache
	 * control */
	uint16_t ccid, mi_mode, bb_offset, gpr0, cmd_buf_cctl;
	/* Gen12 on: the hardware's part of the image is `hw_pages' pages;
	 * the two after it are the context's own: the batch every restore
	 * of it runs midway (the indirect context batch), and the batch
	 * run after the restore (the per-context batch).  `pages' counts
	 * them. */
	uint16_t hw_pages, wa_bb_page;
};

/* What the restore batches of a Gen12 context need to know. */
struct i915_lrc_wa_bb {
	uint32_t image_ggtt; /* the context image's global address */
	uint32_t aux_inv; /* the engine's AUX table invalidation register, 0 for none */
	uint8_t render; /* the render engine's batch (else the other engines') */
	uint8_t state_cache_inv; /* Wa_18022495364: graphics 12.00-12.10 */
	uint8_t icache_inv; /* Wa_16013000631: DG2-G11 */
	uint8_t draw_watermark; /* Wa_16014892111: DG2, 12.70/12.71 before B0 */
	/* Wa_16018031267, Wa_16018063123: the first copy engine of
	 * 12.55-12.71 blits nothing at an address of the context's space
	 * after every restore, with this cache control entry */
	uint8_t fastcolor_blt, blt_mocs;
	uint64_t scratch_addr;
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
/* A three-level (32-bit) space: the four page directories into PDP0-3
 * of a state page written by i915_lrc_init_regs. */
void i915_lrc_set_pdps(uint32_t *regs, const struct i915_lrc_layout *l, const uint64_t pd[4]);
/* The batch the render engine runs at every context restore on Gen9,
 * written to `b' (at least 64 words), with `scratch_ggtt' a global
 * address of 8 free bytes it keeps a register in meanwhile.  Returns
 * the number of words, a multiple of 16 (whole 64-byte lines). */
uint32_t i915_lrc_wa_bb_gen9(uint32_t *b, uint32_t scratch_ggtt);
/* Gen12 on: write a context's two restore batches, the indirect context
 * batch into `indirect' and the per-context batch into `per_ctx' (a page
 * each, the two pages of the image at wa_bb_page).  Returns the length
 * of the indirect context batch in bytes, whole 64-byte lines. */
uint32_t i915_lrc_wa_bb_gen12(uint32_t *indirect, uint32_t *per_ctx,
			      const struct i915_lrc_layout *l, const struct i915_lrc_wa_bb *p);

#endif
