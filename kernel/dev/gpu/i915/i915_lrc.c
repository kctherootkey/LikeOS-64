// LikeOS -- the logical ring context image (pure: host tests compile it).
//
// What the hardware saves and restores when it switches contexts: a page
// of hardware status followed by the register state, laid out as the
// MI_LOAD_REGISTER_IMM sequences the context restore executes.  The
// driver fills the state page in with the ring and page directory the
// context should run with; the hardware writes the rest at its first
// context save.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_lrc_layout.h>
#include <kernel/dev/gpu/i915/i915_reg.h>

#ifndef EINVAL
#define EINVAL 22
#endif

int i915_lrc_layout(unsigned gen_x10, int engine_class, struct i915_lrc_layout *out)
{
	int render = (engine_class == 0);

	for (unsigned i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	if (gen_x10 < 80 || gen_x10 > 127)
		return -EINVAL;
	/* Gen8 through Gen12: the same first block; Gen12 moves nothing the
	 * driver writes (the timestamp and PDP block sit at 0x21 on all). */
	out->lri0 = 0x01;
	out->ctx_ctrl = 0x02;
	out->ring_head = 0x04;
	out->ring_tail = 0x06;
	out->ring_start = 0x08;
	out->ring_ctl = 0x0a;
	out->bb_head_u = 0x0c;
	out->bb_head_l = 0x0e;
	out->bb_state = 0x10;
	out->sbb_head_u = 0x12;
	out->sbb_head_l = 0x14;
	out->sbb_state = 0x16;
	if (render) {
		out->bb_per_ctx = 0x18;
		out->indirect_ctx = 0x1a;
		out->indirect_ctx_off = 0x1c;
		out->lri0_count = 14;
	} else {
		out->lri0_count = 11;
	}
	out->lri1 = 0x21;
	out->timestamp = 0x22;
	out->pdp3_u = 0x24;
	out->pdp3_l = 0x26;
	out->pdp2_u = 0x28;
	out->pdp2_l = 0x2a;
	out->pdp1_u = 0x2c;
	out->pdp1_l = 0x2e;
	out->pdp0_u = 0x30;
	out->pdp0_l = 0x32;
	out->lri1_count = 9;
	if (render) {
		out->lri2 = 0x41;
		out->rpcs = 0x42;
	}
	/* image sizes: the ppHWSP page plus the state */
	if (render)
		out->pages = gen_x10 >= 110 ? 22 : (gen_x10 >= 90 ? 22 : 20);
	else
		out->pages = 2;
	return 0;
}

static void set_reg(uint32_t *regs, uint16_t idx, uint32_t reg, uint32_t val)
{
	regs[idx] = reg;
	regs[idx + 1] = val;
}

/* The render power and clock state a context asks for.  From Gen9 on,
 * power gating can leave slices, subslices and execution units partly
 * enabled, and a context that does not ask for all of them may run
 * with some of its threads never dispatched: geometry with pieces
 * missing, text with glyphs missing, while simple fills come out
 * right.  The request names only the parts this generation can gate:
 * slices when there is more than one (not on the low-power parts),
 * subslices on the low-power parts when there is more than one, and
 * execution units when a subslice has more than two.  Before Gen9
 * nothing is gated and the register stays zero. */
uint32_t i915_lrc_rpcs(unsigned gen, int is_lp, unsigned slices,
		       unsigned subslices, unsigned eus_per_subslice)
{
	uint32_t rpcs = 0;

	if (gen < 9)
		return 0;
	if (!is_lp && slices > 1) {
		rpcs |= GEN8_RPCS_ENABLE | GEN8_RPCS_S_CNT_ENABLE;
		rpcs |= slices << (gen >= 11 ? GEN11_RPCS_S_CNT_SHIFT :
					       GEN8_RPCS_S_CNT_SHIFT);
	}
	if (is_lp && subslices > 1) {
		rpcs |= GEN8_RPCS_ENABLE | GEN8_RPCS_SS_CNT_ENABLE;
		rpcs |= subslices << GEN8_RPCS_SS_CNT_SHIFT;
	}
	if (eus_per_subslice > 2) {
		rpcs |= GEN8_RPCS_ENABLE;
		rpcs |= eus_per_subslice << GEN8_RPCS_EU_MIN_SHIFT;
		rpcs |= eus_per_subslice << GEN8_RPCS_EU_MAX_SHIFT;
	}
	return rpcs;
}

void i915_lrc_init_regs(uint32_t *regs, const struct i915_lrc_layout *l,
			unsigned gen_x10, uint32_t mmio_base, int engine_class,
			uint32_t ring_ggtt, uint32_t ring_size, uint64_t pml4,
			int inhibit_restore, uint32_t rpcs,
			uint32_t wa_bb_ggtt, uint32_t wa_bb_bytes)
{
	int render = (engine_class == 0);
	/* The control word is a masked register: a bit is only changed
	 * where the matching mask bit is set.  The restore-inhibit bit
	 * must therefore be named in EVERY image, set or clear -- an image
	 * copied from one that ran with it set keeps it otherwise, and a
	 * context whose restore is inhibited is handed the engine without
	 * the state it saved.  Its first batch still draws correctly,
	 * because the client writes the whole pipeline into it; everything
	 * after the next context switch draws with whatever the previous
	 * context left behind. */
	uint32_t ctrl = I915_MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH);

	ctrl |= inhibit_restore ?
			I915_MASKED_ENABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT) :
			I915_MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT);
	/* The save must never be inhibited, and the resource streamer's
	 * context is not used: both named, both clear, before Gen11. */
	if (gen_x10 < 110)
		ctrl |= I915_MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT |
					    CTX_CTRL_RS_CTX_ENABLE);
	regs[l->lri0] = MI_LOAD_REGISTER_IMM(l->lri0_count) | MI_LRI_FORCE_POSTED;
	set_reg(regs, l->ctx_ctrl, RING_CONTEXT_CONTROL(mmio_base), ctrl);
	set_reg(regs, l->ring_head, RING_HEAD(mmio_base), 0);
	set_reg(regs, l->ring_tail, RING_TAIL(mmio_base), 0);
	set_reg(regs, l->ring_start, RING_START(mmio_base), ring_ggtt);
	set_reg(regs, l->ring_ctl, RING_CTL(mmio_base),
		RING_CTL_SIZE(ring_size) | RING_VALID);
	set_reg(regs, l->bb_head_u, RING_BBADDR_UDW(mmio_base), 0);
	set_reg(regs, l->bb_head_l, RING_BBADDR(mmio_base), 0);
	set_reg(regs, l->bb_state, RING_BBSTATE(mmio_base), RING_BB_PPGTT);
	set_reg(regs, l->sbb_head_u, RING_SBBADDR_UDW(mmio_base), 0);
	set_reg(regs, l->sbb_head_l, RING_SBBADDR(mmio_base), 0);
	set_reg(regs, l->sbb_state, RING_SBBSTATE(mmio_base), 0);
	if (render) {
		/* The indirect context batch: the engine runs it in the
		 * middle of every restore of this context, at the point the
		 * offset register names.  The pointer carries the batch's
		 * length in 64-byte lines in its low bits. */
		uint32_t off = gen_x10 >= 120 ? GEN12_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT :
			       gen_x10 >= 110 ? GEN11_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT :
			       gen_x10 >= 100 ? GEN10_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT :
			       gen_x10 >= 90 ? GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT :
						GEN8_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT;
		set_reg(regs, l->bb_per_ctx, RING_BB_PER_CTX_PTR(mmio_base), 0);
		if (wa_bb_bytes) {
			set_reg(regs, l->indirect_ctx, RING_INDIRECT_CTX(mmio_base),
				(wa_bb_ggtt & ~63u) | (wa_bb_bytes / 64));
			set_reg(regs, l->indirect_ctx_off,
				RING_INDIRECT_CTX_OFFSET(mmio_base), off << 6);
		} else {
			set_reg(regs, l->indirect_ctx, RING_INDIRECT_CTX(mmio_base), 0);
			set_reg(regs, l->indirect_ctx_off,
				RING_INDIRECT_CTX_OFFSET(mmio_base), 0);
		}
	}
	regs[l->lri1] = MI_LOAD_REGISTER_IMM(l->lri1_count) | MI_LRI_FORCE_POSTED;
	set_reg(regs, l->timestamp, RING_CTX_TIMESTAMP(mmio_base), 0);
	set_reg(regs, l->pdp3_u, RING_PDP_UDW(mmio_base, 3), 0);
	set_reg(regs, l->pdp3_l, RING_PDP_LDW(mmio_base, 3), 0);
	set_reg(regs, l->pdp2_u, RING_PDP_UDW(mmio_base, 2), 0);
	set_reg(regs, l->pdp2_l, RING_PDP_LDW(mmio_base, 2), 0);
	set_reg(regs, l->pdp1_u, RING_PDP_UDW(mmio_base, 1), 0);
	set_reg(regs, l->pdp1_l, RING_PDP_LDW(mmio_base, 1), 0);
	/* 48-bit: PDP0 carries the page map level 4 */
	set_reg(regs, l->pdp0_u, RING_PDP_UDW(mmio_base, 0), (uint32_t)(pml4 >> 32));
	set_reg(regs, l->pdp0_l, RING_PDP_LDW(mmio_base, 0), (uint32_t)pml4);
	if (render) {
		regs[l->lri2] = MI_LOAD_REGISTER_IMM(1);
		set_reg(regs, l->rpcs, GEN8_R_PWR_CLK_STATE, rpcs);
	}
}

/* ---- the indirect context batch ------------------------------------------ */

static uint32_t *pipe_control(uint32_t *b, uint32_t flags, uint32_t addr)
{
	b[0] = GFX_OP_PIPE_CONTROL(6);
	b[1] = flags;
	b[2] = addr;
	b[3] = 0;
	b[4] = 0;
	b[5] = 0;
	return b + 6;
}

/* What Gen9 must do at every context restore, as the part's own
 * documentation asks: flush the L3 lines it keeps coherent, with the
 * bit that makes the flush reach them set for the duration (the
 * register is saved to scratch and loaded back, since a batch cannot
 * read it); clear the shared local memory; and load three settings
 * that are not carried by the context image.  Arbitration is off while
 * it runs.  No end-of-batch: the engine runs exactly the length the
 * pointer register names. */
uint32_t i915_lrc_wa_bb_gen9(uint32_t *b, uint32_t scratch_ggtt)
{
	uint32_t *p = b;

	*p++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;
	*p++ = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	*p++ = GEN8_L3SQCREG4;
	*p++ = scratch_ggtt;
	*p++ = 0;
	*p++ = MI_LOAD_REGISTER_IMM(1);
	*p++ = GEN8_L3SQCREG4;
	*p++ = GEN8_L3SQCREG4_DEFAULT | GEN8_LQSC_FLUSH_COHERENT_LINES;
	p = pipe_control(p, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_DC_FLUSH_ENABLE, 0);
	*p++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	*p++ = GEN8_L3SQCREG4;
	*p++ = scratch_ggtt;
	*p++ = 0;
	p = pipe_control(p, PIPE_CONTROL_FLUSH_L3 | PIPE_CONTROL_STORE_DATA_INDEX |
				    PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE,
			 LRC_PPHWSP_SCRATCH_ADDR);
	*p++ = MI_LOAD_REGISTER_IMM(3);
	*p++ = COMMON_SLICE_CHICKEN2;
	*p++ = I915_MASKED_DISABLE(GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE);
	*p++ = FF_SLICE_CHICKEN;
	*p++ = I915_MASKED_ENABLE(FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX);
	*p++ = _3D_CHICKEN3;
	*p++ = I915_MASKED_ENABLE(_3D_CHICKEN_SF_PROVOKING_VERTEX_FIX);
	*p++ = MI_NOOP;
	*p++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	while ((p - b) & 15)
		*p++ = MI_NOOP;
	return (uint32_t)(p - b);
}
