// LikeOS -- the logical ring context image (pure: host tests compile it).
//
// What the hardware saves and restores when it switches contexts: a page
// of hardware status followed by the register state, laid out as the
// MI_LOAD_REGISTER_IMM sequences the context restore executes.  The
// driver fills the state page in with the ring and page directory the
// context should run with; the hardware writes the rest at its first
// context save.
//
// From Gen12 on the driver lays the whole image out itself, as tables of
// register offsets per engine kind, because the layout moved: the
// secondary batch registers are gone, the per-context batch pointers and
// the context id took their place, and the render image of Xe_HP and
// Meteor Lake differs again.  Gen12 contexts also carry two batches of
// their own in the image, run at every restore, that put back what the
// restore itself gets wrong.  Xe2 made the context timestamp 64-bit,
// replaced page directory pointers 1-3 by address space ids, gave the
// copy engines an image of their own and needs no restore batches.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_lrc_layout.h>
#include <kernel/dev/gpu/i915/i915_reg.h>

#ifndef EINVAL
#define EINVAL 22
#endif

/* ---- the Gen12 layout tables -------------------------------------------- */

/* One byte at a time: a byte with the top bit set skips that many words
 * (bits 6:0); otherwise it starts a register load of `count' (bits 5:0)
 * registers, posted when bit 6 is set, and the register offsets follow,
 * relative to the engine, in dwords, seven bits a byte, a set top bit
 * meaning more bytes follow.  Zero ends the table. */
#define NOP(x) (0x80 | (x))
#define LRI(count, flags) (((flags) << 6) | (count))
#define POSTED 1
#define REG(x) ((x) >> 2)
#define REG16(x) (((x) >> 9) | 0x80), (((x) >> 2) & 0x7f)
#define END 0

static const uint8_t gen12_xcs_offsets[] = {
	NOP(1),
	LRI(13, POSTED),
	REG16(0x244),
	REG(0x034),
	REG(0x030),
	REG(0x038),
	REG(0x03c),
	REG(0x168),
	REG(0x140),
	REG(0x110),
	REG(0x1c0),
	REG(0x1c4),
	REG(0x1c8),
	REG(0x180),
	REG16(0x2b4),

	NOP(5),
	LRI(9, POSTED),
	REG16(0x3a8),
	REG16(0x28c),
	REG16(0x288),
	REG16(0x284),
	REG16(0x280),
	REG16(0x27c),
	REG16(0x278),
	REG16(0x274),
	REG16(0x270),

	END
};

static const uint8_t dg2_xcs_offsets[] = {
	NOP(1),
	LRI(15, POSTED),
	REG16(0x244),
	REG(0x034),
	REG(0x030),
	REG(0x038),
	REG(0x03c),
	REG(0x168),
	REG(0x140),
	REG(0x110),
	REG(0x1c0),
	REG(0x1c4),
	REG(0x1c8),
	REG(0x180),
	REG16(0x2b4),
	REG(0x120),
	REG(0x124),

	NOP(1),
	LRI(9, POSTED),
	REG16(0x3a8),
	REG16(0x28c),
	REG16(0x288),
	REG16(0x284),
	REG16(0x280),
	REG16(0x27c),
	REG16(0x278),
	REG16(0x274),
	REG16(0x270),

	END
};

static const uint8_t gen12_rcs_offsets[] = {
	NOP(1),
	LRI(13, POSTED),
	REG16(0x244),
	REG(0x034),
	REG(0x030),
	REG(0x038),
	REG(0x03c),
	REG(0x168),
	REG(0x140),
	REG(0x110),
	REG(0x1c0),
	REG(0x1c4),
	REG(0x1c8),
	REG(0x180),
	REG16(0x2b4),

	NOP(5),
	LRI(9, POSTED),
	REG16(0x3a8),
	REG16(0x28c),
	REG16(0x288),
	REG16(0x284),
	REG16(0x280),
	REG16(0x27c),
	REG16(0x278),
	REG16(0x274),
	REG16(0x270),

	LRI(3, POSTED),
	REG(0x1b0),
	REG16(0x5a8),
	REG16(0x5ac),

	NOP(6),
	LRI(1, 0),
	REG(0x0c8),
	NOP(3 + 9 + 1),

	LRI(51, POSTED),
	REG16(0x588),
	REG16(0x588),
	REG16(0x588),
	REG16(0x588),
	REG16(0x588),
	REG16(0x588),
	REG(0x028),
	REG(0x09c),
	REG(0x0c0),
	REG(0x178),
	REG(0x17c),
	REG16(0x358),
	REG(0x170),
	REG(0x150),
	REG(0x154),
	REG(0x158),
	REG16(0x41c),
	REG16(0x600),
	REG16(0x604),
	REG16(0x608),
	REG16(0x60c),
	REG16(0x610),
	REG16(0x614),
	REG16(0x618),
	REG16(0x61c),
	REG16(0x620),
	REG16(0x624),
	REG16(0x628),
	REG16(0x62c),
	REG16(0x630),
	REG16(0x634),
	REG16(0x638),
	REG16(0x63c),
	REG16(0x640),
	REG16(0x644),
	REG16(0x648),
	REG16(0x64c),
	REG16(0x650),
	REG16(0x654),
	REG16(0x658),
	REG16(0x65c),
	REG16(0x660),
	REG16(0x664),
	REG16(0x668),
	REG16(0x66c),
	REG16(0x670),
	REG16(0x674),
	REG16(0x678),
	REG16(0x67c),
	REG(0x068),
	REG(0x084),
	NOP(1),

	END
};

static const uint8_t dg2_rcs_offsets[] = {
	NOP(1),
	LRI(15, POSTED),
	REG16(0x244),
	REG(0x034),
	REG(0x030),
	REG(0x038),
	REG(0x03c),
	REG(0x168),
	REG(0x140),
	REG(0x110),
	REG(0x1c0),
	REG(0x1c4),
	REG(0x1c8),
	REG(0x180),
	REG16(0x2b4),
	REG(0x120),
	REG(0x124),

	NOP(1),
	LRI(9, POSTED),
	REG16(0x3a8),
	REG16(0x28c),
	REG16(0x288),
	REG16(0x284),
	REG16(0x280),
	REG16(0x27c),
	REG16(0x278),
	REG16(0x274),
	REG16(0x270),

	LRI(3, POSTED),
	REG(0x1b0),
	REG16(0x5a8),
	REG16(0x5ac),

	NOP(6),
	LRI(1, 0),
	REG(0x0c8),

	END
};

static const uint8_t mtl_rcs_offsets[] = {
	NOP(1),
	LRI(15, POSTED),
	REG16(0x244),
	REG(0x034),
	REG(0x030),
	REG(0x038),
	REG(0x03c),
	REG(0x168),
	REG(0x140),
	REG(0x110),
	REG(0x1c0),
	REG(0x1c4),
	REG(0x1c8),
	REG(0x180),
	REG16(0x2b4),
	REG(0x120),
	REG(0x124),

	NOP(1),
	LRI(9, POSTED),
	REG16(0x3a8),
	REG16(0x28c),
	REG16(0x288),
	REG16(0x284),
	REG16(0x280),
	REG16(0x27c),
	REG16(0x278),
	REG16(0x274),
	REG16(0x270),

	NOP(2),
	LRI(2, POSTED),
	REG16(0x5a8),
	REG16(0x5ac),

	NOP(6),
	LRI(1, 0),
	REG(0x0c8),

	END
};

/* Xe2 and Xe3: the first two blocks every engine has; the context
 * timestamp is 64-bit, the page directory pointers 1-3 gave way to the
 * address space ids and the indirect ring state pointer. */
#define XE2_CTX_COMMON                                                    \
	NOP(1),			/* [0x00] */                              \
	LRI(15, POSTED),	/* [0x01] */                              \
	REG16(0x244),		/* [0x02] CTXT_SR_CTL */                  \
	REG(0x034),		/* [0x04] RING_BUFFER_HEAD */             \
	REG(0x030),		/* [0x06] RING_BUFFER_TAIL */             \
	REG(0x038),		/* [0x08] RING_BUFFER_START */            \
	REG(0x03c),		/* [0x0a] RING_BUFFER_CONTROL */          \
	REG(0x168),		/* [0x0c] BB_ADDR_UDW */                  \
	REG(0x140),		/* [0x0e] BB_ADDR */                      \
	REG(0x110),		/* [0x10] BB_STATE */                     \
	REG(0x1c0),		/* [0x12] BB_PER_CTX_PTR */               \
	REG(0x1c4),		/* [0x14] RCS_INDIRECT_CTX */             \
	REG(0x1c8),		/* [0x16] RCS_INDIRECT_CTX_OFFSET */      \
	REG(0x180),		/* [0x18] CCID */                         \
	REG16(0x2b4),		/* [0x1a] SEMAPHORE_TOKEN */              \
	REG(0x120),		/* [0x1c] PRT_BB_STATE */                 \
	REG(0x124),		/* [0x1e] PRT_BB_STATE_UDW */             \
									  \
	NOP(1),			/* [0x20] */                              \
	LRI(9, POSTED),		/* [0x21] */                              \
	REG16(0x3a8),		/* [0x22] CTX_TIMESTAMP */                \
	REG16(0x3ac),		/* [0x24] CTX_TIMESTAMP_UDW */            \
	REG(0x108),		/* [0x26] INDIRECT_RING_STATE */          \
	REG16(0x284),		/* [0x28] dummy reg */                    \
	REG16(0x280),		/* [0x2a] CS_ACC_CTR_THOLD */             \
	REG16(0x27c),		/* [0x2c] CS_CTX_SYS_PASID */             \
	REG16(0x278),		/* [0x2e] CS_CTX_ASID */                  \
	REG16(0x274),		/* [0x30] PTBP_UDW */                     \
	REG16(0x270)		/* [0x32] PTBP_LDW */

static const uint8_t xe2_rcs_offsets[] = {
	XE2_CTX_COMMON,

	NOP(2),			/* [0x34] */
	LRI(2, POSTED),		/* [0x36] */
	REG16(0x5a8),		/* [0x37] CONTEXT_SCHEDULING_ATTRIBUTES */
	REG16(0x5ac),		/* [0x39] PREEMPTION_STATUS */

	NOP(6),			/* [0x41] */
	LRI(1, 0),		/* [0x47] */
	REG(0x0c8),		/* [0x48] R_PWR_CLK_STATE */

	END
};

static const uint8_t xe2_bcs_offsets[] = {
	XE2_CTX_COMMON,

	NOP(4 + 8 + 1),		/* [0x34] */
	LRI(2, POSTED),		/* [0x41] */
	REG16(0x200),		/* [0x42] BCS_SWCTRL */
	REG16(0x204),		/* [0x44] BLIT_CCTL */

	END
};

static const uint8_t xe2_xcs_offsets[] = {
	XE2_CTX_COMMON,

	END
};

#undef XE2_CTX_COMMON
#undef END
#undef REG16
#undef REG
#undef LRI
#undef NOP

/* Render and (Xe_HP on) compute engines share the render image; from
 * Xe2 on the compute engines' image is that of the other engines, and
 * the copy engines have one of their own. */
static const uint8_t *gen12_offsets(unsigned gen_x10, int engine_class)
{
	int rcs = engine_class == 0 || engine_class == 4;

	if (gen_x10 >= 200) {
		if (engine_class == 0)
			return xe2_rcs_offsets;
		if (engine_class == 1)
			return xe2_bcs_offsets;
		return xe2_xcs_offsets;
	}
	if (rcs)
		return gen_x10 >= 127 ? mtl_rcs_offsets :
		       gen_x10 >= 125 ? dg2_rcs_offsets :
					gen12_rcs_offsets;
	return gen_x10 >= 125 ? dg2_xcs_offsets : gen12_xcs_offsets;
}

/* Write the register loads and offsets a table names (not the values);
 * `close' ends the image with a batch end, as an image never saved by
 * the hardware must. */
static void set_offsets(uint32_t *regs, const uint8_t *data, uint32_t base, int close)
{
	while (*data) {
		uint8_t count, flags;

		if (*data & 0x80) { /* skip */
			count = *data++ & 0x7f;
			regs += count;
			continue;
		}
		count = *data & 0x3f;
		flags = *data >> 6;
		data++;
		*regs = MI_LOAD_REGISTER_IMM(count) | MI_LRI_LRM_CS_MMIO;
		if (flags & 1)
			*regs |= MI_LRI_FORCE_POSTED;
		regs++;
		do {
			uint32_t offset = 0;
			uint8_t v;

			do {
				v = *data++;
				offset <<= 7;
				offset |= v & 0x7f;
			} while (v & 0x80);
			regs[0] = base + (offset << 2);
			regs += 2;
		} while (--count);
	}
	if (close)
		*regs = MI_BATCH_BUFFER_END | 1u;
}

/* Xe2 and Xe3: the indices the driver writes, the image's size -- the
 * per-process status page and three pages of state for the render
 * engine, two for compute, one for the rest -- and none of the restore
 * batches the Gen12 contexts carry (the parts need none of their
 * workarounds). */
static void xe2_layout(int engine_class, struct i915_lrc_layout *out)
{
	out->lri0 = 0x01;
	out->ctx_ctrl = 0x02;
	out->ring_head = 0x04;
	out->ring_tail = 0x06;
	out->ring_start = 0x08;
	out->ring_ctl = 0x0a;
	out->bb_head_u = 0x0c;
	out->bb_head_l = 0x0e;
	out->bb_state = 0x10;
	out->bb_per_ctx = 0x12;
	out->indirect_ctx = 0x14;
	out->indirect_ctx_off = 0x16;
	out->ccid = 0x18;
	out->lri0_count = 15;
	out->lri1 = 0x21;
	out->timestamp = 0x22;
	out->timestamp_udw = 0x24;
	out->pdp0_u = 0x30;
	out->pdp0_l = 0x32;
	out->lri1_count = 9;
	if (engine_class == 0) {
		out->lri2 = 0x47;
		out->rpcs = 0x48;
		out->hw_pages = 1 + 3;
	} else if (engine_class == 4) {
		out->hw_pages = 1 + 2;
	} else {
		out->hw_pages = 1 + 1;
	}
	out->pages = out->hw_pages;
}

static void gen12_layout(unsigned gen_x10, int engine_class, struct i915_lrc_layout *out)
{
	int rcs = engine_class == 0 || engine_class == 4;
	int xehp = gen_x10 >= 125;

	out->lri0 = 0x01;
	out->ctx_ctrl = 0x02;
	out->ring_head = 0x04;
	out->ring_tail = 0x06;
	out->ring_start = 0x08;
	out->ring_ctl = 0x0a;
	out->bb_head_u = 0x0c;
	out->bb_head_l = 0x0e;
	out->bb_state = 0x10;
	out->bb_per_ctx = 0x12;
	out->indirect_ctx = 0x14;
	out->indirect_ctx_off = 0x16;
	out->ccid = 0x18;
	out->lri0_count = xehp ? 15 : 13;
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
	if (engine_class == 0) {
		out->lri2 = 0x41;
		out->rpcs = 0x42;
	}
	out->mi_mode = xehp ? 0x70 : 0x60;
	out->bb_offset = xehp ? 0x80 : 0x70;
	out->gpr0 = xehp ? 0x84 : 0x74;
	/* the command buffer cache control: the render image's, and from
	 * Xe_HP on a slot in every image (a dummy one outside render) */
	out->cmd_buf_cctl = xehp ? 0xc6 : (engine_class == 0 ? 0xb6 : 0);
	/* 14 pages for render and compute, 2 for the rest, the per-process
	 * status page included; then the two restore batch pages */
	out->hw_pages = rcs ? 14 : 2;
	out->wa_bb_page = out->hw_pages;
	out->pages = out->hw_pages + 2;
}

int i915_lrc_layout(unsigned gen_x10, int engine_class, struct i915_lrc_layout *out)
{
	int render = (engine_class == 0);

	for (unsigned i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	/* Xe3P keeps the ring's state outside the image (indirect ring
	 * state), which is not driven */
	if (gen_x10 < 80 || (gen_x10 > 127 && gen_x10 < 200) || gen_x10 >= 350)
		return -EINVAL;
	if (gen_x10 >= 200) {
		xe2_layout(engine_class, out);
		return 0;
	}
	if (gen_x10 >= 120) {
		gen12_layout(gen_x10, engine_class, out);
		return 0;
	}
	/* Gen8 through Gen11: the same first block. */
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
	out->hw_pages = out->pages;
	if (gen_x10 >= 90)
		out->mi_mode = 0x54;
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

/* Gen12 on: the whole layout is written, the values the driver owns
 * after it.  An image made from the golden one keeps everything else the
 * hardware saved; an image never saved starts zeroed, its restore
 * inhibited. */
static void gen12_init_regs(uint32_t *regs, const struct i915_lrc_layout *l,
			    unsigned gen_x10, uint32_t mmio_base, int engine_class,
			    uint32_t ring_ggtt, uint32_t ring_size, uint64_t pml4,
			    int inhibit_restore, uint32_t rpcs,
			    uint32_t wa_bb_ggtt, uint32_t wa_bb_bytes)
{
	uint32_t ctrl;

	if (inhibit_restore)
		for (unsigned i = 0; i < 1024; i++)
			regs[i] = 0;
	set_offsets(regs, gen12_offsets(gen_x10, engine_class), mmio_base, inhibit_restore);

	/* The restore-inhibit bit is named in every image, set or clear
	 * (see below); the save is never inhibited on Gen11 and later,
	 * where the bit is reserved. */
	ctrl = I915_MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH) |
	       I915_MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT);
	if (inhibit_restore)
		ctrl |= CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT;
	regs[l->ctx_ctrl + 1] = ctrl;
	regs[l->timestamp + 1] = 0;
	if (l->timestamp_udw)
		regs[l->timestamp_udw + 1] = 0;
	if (l->bb_offset)
		regs[l->bb_offset + 1] = 0;

	/* 48-bit: PDP0 carries the page map level 4, the others unused;
	 * from Xe2 on the top table as a directory entry */
	regs[l->pdp0_u + 1] = (uint32_t)(pml4 >> 32);
	regs[l->pdp0_l + 1] = (uint32_t)pml4;

	/* The context's own restore batches: the indirect context batch
	 * with its length in 64-byte lines and the point in the restore
	 * it runs at, and the per-context batch, forced to run. */
	if (wa_bb_bytes) {
		regs[l->indirect_ctx + 1] = (wa_bb_ggtt & ~63u) | (wa_bb_bytes / 64);
		regs[l->indirect_ctx_off + 1] = GEN12_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT << 6;
		regs[l->bb_per_ctx + 1] = (wa_bb_ggtt + 4096) | PER_CTX_BB_FORCE |
					  PER_CTX_BB_VALID;
	}

	/* A context saved while the engine was being stopped carries the
	 * stop bit; it must never be restored with it. */
	if (l->mi_mode) {
		regs[l->mi_mode + 1] &= ~(uint32_t)STOP_RING;
		regs[l->mi_mode + 1] |= (uint32_t)STOP_RING << 16;
	}

	regs[l->ring_start + 1] = ring_ggtt;
	regs[l->ring_head + 1] = 0;
	regs[l->ring_tail + 1] = 0;
	regs[l->ring_ctl + 1] = RING_CTL_SIZE(ring_size) | RING_VALID;
	if (engine_class == 0)
		regs[l->rpcs + 1] = rpcs;
}

void i915_lrc_init_regs(uint32_t *regs, const struct i915_lrc_layout *l,
			unsigned gen_x10, uint32_t mmio_base, int engine_class,
			uint32_t ring_ggtt, uint32_t ring_size, uint64_t pml4,
			int inhibit_restore, uint32_t rpcs,
			uint32_t wa_bb_ggtt, uint32_t wa_bb_bytes)
{
	int render = (engine_class == 0);

	if (gen_x10 >= 120) {
		gen12_init_regs(regs, l, gen_x10, mmio_base, engine_class, ring_ggtt,
				ring_size, pml4, inhibit_restore, rpcs, wa_bb_ggtt,
				wa_bb_bytes);
		return;
	}
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

void i915_lrc_set_pdps(uint32_t *regs, const struct i915_lrc_layout *l, const uint64_t pd[4])
{
	regs[l->pdp3_u + 1] = (uint32_t)(pd[3] >> 32);
	regs[l->pdp3_l + 1] = (uint32_t)pd[3];
	regs[l->pdp2_u + 1] = (uint32_t)(pd[2] >> 32);
	regs[l->pdp2_l + 1] = (uint32_t)pd[2];
	regs[l->pdp1_u + 1] = (uint32_t)(pd[1] >> 32);
	regs[l->pdp1_l + 1] = (uint32_t)pd[1];
	regs[l->pdp0_u + 1] = (uint32_t)(pd[0] >> 32);
	regs[l->pdp0_l + 1] = (uint32_t)pd[0];
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

/* ---- the Gen12 restore batches -------------------------------------------- */

/* The register offsets the restore batches name are relative to the
 * engine (MI_LRI_LRM_CS_MMIO), so the batches are the same on every
 * engine of a kind. */
#define CS_GPR0 GEN8_RING_CS_GPR(0, 0)
#define CS_CTX_TIMESTAMP RING_CTX_TIMESTAMP(0)
#define CS_CMD_BUF_CCTL RING_CMD_BUF_CCTL(0)
#define CS_PREDICATE_RESULT RING_PREDICATE_RESULT(0)

/* Where in the indirect context page the predication workaround keeps
 * its batch and its result. */
#define PREDICATE_RESULT_BB 2048
#define PREDICATE_RESULT_WA (4096 - 8)

static uint32_t *lrm_from_image(uint32_t *cs, uint32_t reg, uint32_t image_ggtt, uint16_t slot)
{
	*cs++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT | MI_LRI_LRM_CS_MMIO;
	*cs++ = reg;
	*cs++ = image_ggtt + 4096 + (uint32_t)(slot + 1) * 4;
	*cs++ = 0;
	return cs;
}

static uint32_t *lrr_cs(uint32_t *cs, uint32_t src, uint32_t dst)
{
	*cs++ = MI_LOAD_REGISTER_REG | MI_LRR_SOURCE_CS_MMIO | MI_LRI_LRM_CS_MMIO;
	*cs++ = src;
	*cs++ = dst;
	return cs;
}

/* The context timestamp restored from the image is not the one the
 * hardware counts from until it is written once more: load the saved
 * value into a scratch register and from there into the timestamp,
 * twice, as the hardware wants it. */
static uint32_t *emit_timestamp_wa(uint32_t *cs, const struct i915_lrc_layout *l, uint32_t img)
{
	cs = lrm_from_image(cs, CS_GPR0, img, l->timestamp);
	cs = lrr_cs(cs, CS_GPR0, CS_CTX_TIMESTAMP);
	cs = lrr_cs(cs, CS_GPR0, CS_CTX_TIMESTAMP);
	return cs;
}

/* The command buffer cache control does not survive the restore either:
 * the same trick. */
static uint32_t *emit_cmd_buf_wa(uint32_t *cs, const struct i915_lrc_layout *l, uint32_t img)
{
	cs = lrm_from_image(cs, CS_GPR0, img, l->cmd_buf_cctl);
	cs = lrr_cs(cs, CS_GPR0, CS_CMD_BUF_CCTL);
	return cs;
}

/* ...and the scratch register used for both gets its own value back. */
static uint32_t *emit_restore_scratch(uint32_t *cs, const struct i915_lrc_layout *l, uint32_t img)
{
	return lrm_from_image(cs, CS_GPR0, img, l->gpr0);
}

static uint32_t *pipe_control12(uint32_t *b, uint32_t group0, uint32_t group1, uint32_t addr)
{
	b[0] = GFX_OP_PIPE_CONTROL(6) | group0;
	b[1] = group1;
	b[2] = addr;
	b[3] = 0;
	b[4] = 0;
	b[5] = 0;
	return b + 6;
}

/* The compression metadata's translation (the AUX table) is cached; a
 * context's surfaces may have moved, so the cache is emptied and the
 * engine waits until the hardware says it is. */
static uint32_t *emit_aux_table_inv(uint32_t *cs, uint32_t reg)
{
	if (!reg)
		return cs;
	*cs++ = MI_LOAD_REGISTER_IMM(1) | MI_LRI_MMIO_REMAP_EN;
	*cs++ = reg;
	*cs++ = AUX_INV;
	*cs++ = MI_SEMAPHORE_WAIT_TOKEN | MI_SEMAPHORE_REGISTER_POLL | MI_SEMAPHORE_POLL |
		MI_SEMAPHORE_SAD_EQ_SDD;
	*cs++ = 0;
	*cs++ = reg;
	*cs++ = 0;
	*cs++ = 0;
	return cs;
}

/* Predication left on by a client's batch would turn the ring's own
 * commands into no-ops after the next restore.  This batch, placed in
 * the second half of the indirect context page, turns it off: the store
 * and the predicated batch end are skipped while predication is on, so
 * the predicate is cleared and the result written for the next batch
 * start to load. */
static uint32_t *emit_predicate_disable(uint32_t *cs, uint32_t indirect_ggtt)
{
	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT | (4 - 2);
	*cs++ = indirect_ggtt + PREDICATE_RESULT_WA;
	*cs++ = 0;
	*cs++ = 0; /* no predication */
	/* predicated end: only ends here if the predicate result is clear */
	*cs++ = MI_BATCH_BUFFER_END | (1u << 15);
	*cs++ = MI_SET_PREDICATE | MI_SET_PREDICATE_DISABLE;
	/* not predicated any longer */
	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT | (4 - 2);
	*cs++ = indirect_ggtt + PREDICATE_RESULT_WA;
	*cs++ = 0;
	*cs++ = 1; /* predication back on before the next batch */
	*cs++ = MI_BATCH_BUFFER_END;
	return cs;
}

uint32_t i915_lrc_wa_bb_gen12(uint32_t *indirect, uint32_t *per_ctx,
			      const struct i915_lrc_layout *l, const struct i915_lrc_wa_bb *p)
{
	uint32_t indirect_ggtt = p->image_ggtt + (uint32_t)l->wa_bb_page * 4096;
	uint32_t *cs = indirect;

	for (unsigned i = 0; i < 1024; i++)
		indirect[i] = per_ctx[i] = 0;

	cs = emit_timestamp_wa(cs, l, p->image_ggtt);
	if (p->render)
		cs = emit_cmd_buf_wa(cs, l, p->image_ggtt);
	cs = emit_restore_scratch(cs, l, p->image_ggtt);
	/* Wa_16013000631 */
	if (p->icache_inv)
		cs = pipe_control12(cs, 0, PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE, 0);
	cs = emit_aux_table_inv(cs, p->aux_inv);
	if (p->render) {
		/* Wa_18022495364 */
		if (p->state_cache_inv) {
			*cs++ = MI_LOAD_REGISTER_IMM(1);
			*cs++ = GEN12_CS_DEBUG_MODE2;
			*cs++ = I915_MASKED_ENABLE(INSTRUCTION_STATE_CACHE_INVALIDATE);
		}
		/* Wa_16014892111: the vertical watermark the tuning guide
		 * asks for is not saved and restored right; it is loaded
		 * here instead, every other bit at the default 0 */
		if (p->draw_watermark) {
			*cs++ = MI_LOAD_REGISTER_IMM(1);
			*cs++ = DRAW_WATERMARK;
			*cs++ = VERT_WM_VAL;
		}
	}
	while ((cs - indirect) & 15)
		*cs++ = MI_NOOP;
	uint32_t bytes = (uint32_t)(cs - indirect) * 4;
	(void)emit_predicate_disable(indirect + PREDICATE_RESULT_BB / 4, indirect_ggtt);

	/* The per-context batch. */
	cs = per_ctx;
	if (p->fastcolor_blt) {
		/* Wa_16018031267, Wa_16018063123: four sub-blits of a 2x5
		 * surface, each writing nothing */
		uint32_t mocs = (uint32_t)p->blt_mocs << 1;
		*cs++ = XY_FAST_COLOR_BLT_CMD | (16 - 2);
		*cs++ = XY_FAST_COLOR_BLT_MOCS(mocs) | 0x3f;
		*cs++ = 0;
		*cs++ = (4u << 16) | 1;
		*cs++ = (uint32_t)p->scratch_addr;
		*cs++ = (uint32_t)(p->scratch_addr >> 32);
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = 0x20004004;
		*cs++ = 0x10;
		*cs++ = 0;
	}
	*cs++ = MI_BATCH_BUFFER_END;
	return bytes;
}
