/* Tests for the logical ring context image layout.  See test-i915-lrc.sh. */
#include <kernel/dev/gpu/i915/i915_lrc_layout.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_renderstate.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	struct i915_lrc_layout l;
	static uint32_t regs[1024];

	CHECK(i915_lrc_layout(90, 0, &l) == 0, "gen9 render layout");
	CHECK(l.pages == 22, "render pages %u", l.pages);
	CHECK(l.lri0 == 1 && l.ctx_ctrl == 2 && l.ring_tail == 6 && l.ring_start == 8, "first block");
	CHECK(l.pdp0_l == 0x32 && l.pdp0_u == 0x30, "pdp0 %x %x", l.pdp0_l, l.pdp0_u);
	CHECK(l.lri0_count == 14 && l.lri1_count == 9, "counts %u %u", l.lri0_count, l.lri1_count);
	CHECK(l.rpcs == 0x42, "rpcs");
	memset(regs, 0, sizeof(regs));
	i915_lrc_init_regs(regs, &l, 90, RENDER_RING_BASE, 0, 0x00f00000, 32768,
			   0x0000000123456000ULL, 1, 0x80000088, 0x00e00000, 192);
	CHECK(regs[1] == (MI_LOAD_REGISTER_IMM(14) | MI_LRI_FORCE_POSTED), "lri0 header %08x", regs[1]);
	CHECK(regs[2] == RING_CONTEXT_CONTROL(RENDER_RING_BASE), "ctx ctrl reg %08x", regs[2]);
	/* The control word names both bits, always: the mask half decides
	 * which bits the write touches, so a bit left out keeps whatever
	 * the image it was copied from had. */
	CHECK(regs[3] == (I915_MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH) |
			  I915_MASKED_ENABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT) |
			  I915_MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT |
					      CTX_CTRL_RS_CTX_ENABLE)),
	      "ctx ctrl value %08x", regs[3]);
	CHECK(!(regs[3] & CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT) &&
		      ((regs[3] >> 16) & CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT),
	      "the save is named and never inhibited");
	CHECK((regs[3] >> 16) & CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT,
	      "restore inhibit is named when it is set");
	CHECK(regs[8] == RING_START(RENDER_RING_BASE) && regs[9] == 0x00f00000, "ring start");
	CHECK(regs[0xa] == RING_CTL(RENDER_RING_BASE) && regs[0xb] == (RING_CTL_SIZE(32768) | RING_VALID), "ring ctl %08x", regs[0xb]);
	CHECK(regs[0x21] == (MI_LOAD_REGISTER_IMM(9) | MI_LRI_FORCE_POSTED), "lri1 header %08x", regs[0x21]);
	CHECK(regs[0x32] == RING_PDP_LDW(RENDER_RING_BASE, 0) && regs[0x33] == 0x23456000, "pdp0 low %08x %08x", regs[0x32], regs[0x33]);
	CHECK(regs[0x30] == RING_PDP_UDW(RENDER_RING_BASE, 0) && regs[0x31] == 0x1, "pdp0 high %08x %08x", regs[0x30], regs[0x31]);
	/* The restore batch: its global address with its length in 64-byte
	 * lines in the low bits, and the point in the restore it runs at
	 * (Gen9: line 0x26), both named in the first block. */
	CHECK(regs[0x18] == RING_BB_PER_CTX_PTR(RENDER_RING_BASE) && regs[0x19] == 0,
	      "no per-context batch: %08x %08x", regs[0x18], regs[0x19]);
	CHECK(regs[0x1a] == RING_INDIRECT_CTX(RENDER_RING_BASE) && regs[0x1b] == (0x00e00000 | 3),
	      "restore batch pointer %08x %08x", regs[0x1a], regs[0x1b]);
	CHECK(regs[0x1c] == RING_INDIRECT_CTX_OFFSET(RENDER_RING_BASE) && regs[0x1d] == (0x26 << 6),
	      "restore batch offset %08x %08x", regs[0x1c], regs[0x1d]);
	CHECK(regs[0x41] == MI_LOAD_REGISTER_IMM(1) && regs[0x42] == GEN8_R_PWR_CLK_STATE, "rpcs block");
	CHECK(regs[0x43] == 0x80000088, "the power and clock request is carried into the image: %08x", regs[0x43]);
	/* The request itself.  A one-slice Gen9 part with three subslices of
	 * eight units gates only its execution units: enable, min and max
	 * eight.  A part with three slices asks for the slices as well; a
	 * low-power part with three subslices asks for the subslices instead;
	 * Gen8 asks for nothing; Gen11 puts the slice count elsewhere. */
	CHECK(i915_lrc_rpcs(9, 0, 1, 3, 8) == 0x80000088, "gen9 one slice: %08x", i915_lrc_rpcs(9, 0, 1, 3, 8));
	CHECK(i915_lrc_rpcs(9, 0, 3, 9, 8) == 0x80058088, "gen9 three slices: %08x", i915_lrc_rpcs(9, 0, 3, 9, 8));
	CHECK(i915_lrc_rpcs(9, 1, 1, 3, 6) == 0x80000b66, "gen9 low power: %08x", i915_lrc_rpcs(9, 1, 1, 3, 6));
	CHECK(i915_lrc_rpcs(8, 0, 2, 6, 8) == 0, "gen8 asks for nothing");
	CHECK(i915_lrc_rpcs(11, 0, 1, 8, 8) == 0x80000088, "gen11 one slice: %08x", i915_lrc_rpcs(11, 0, 1, 8, 8));
	CHECK(i915_lrc_rpcs(11, 0, 2, 16, 8) == 0x80042088, "gen11 two slices: %08x", i915_lrc_rpcs(11, 0, 2, 16, 8));
	/* a request for two units only does not enable gating control at all */
	CHECK(i915_lrc_rpcs(9, 0, 1, 2, 2) == 0, "two units per subslice: nothing to gate");
	/* a video engine: no render-only entries, 2 pages, 11 registers */
	CHECK(i915_lrc_layout(90, 2, &l) == 0 && l.pages == 2 && l.lri0_count == 11 && l.rpcs == 0, "vcs layout");
	memset(regs, 0, sizeof(regs));
	i915_lrc_init_regs(regs, &l, 90, GEN6_BSD_RING_BASE, 2, 0x1000, 32768, 0x2000, 0, 0x80000088,
			   0, 0);
	CHECK(regs[1] == (MI_LOAD_REGISTER_IMM(11) | MI_LRI_FORCE_POSTED), "vcs lri0 %08x", regs[1]);
	/* Not inhibited: the bit must be named AND clear, or an image
	 * copied from an inhibited one never restores its state -- which
	 * is a picture that is right until the first context switch. */
	CHECK(regs[3] == (I915_MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH) |
			  I915_MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT) |
			  I915_MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT |
					      CTX_CTRL_RS_CTX_ENABLE)),
	      "vcs ctrl %08x", regs[3]);
	CHECK((regs[3] >> 16) & CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT,
	      "restore inhibit is named when it is clear too");
	CHECK(!(regs[3] & CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT),
	      "and its value is clear");
	CHECK(regs[0x18] == 0 && regs[0x19] == 0, "no bb-per-ctx on vcs");
	CHECK(regs[0x41] == 0 && regs[0x43] == 0, "no power and clock request on vcs");
	CHECK(i915_lrc_layout(70, 0, &l) != 0, "gen7 refused");
	/* Without a restore batch the render image names none. */
	memset(regs, 0, sizeof(regs));
	i915_lrc_layout(90, 0, &l);
	i915_lrc_init_regs(regs, &l, 90, RENDER_RING_BASE, 0, 0x00f00000, 32768, 0x1000, 0,
			   0x80000088, 0, 0);
	CHECK(regs[0x1b] == 0 && regs[0x1d] == 0, "no restore batch: %08x %08x", regs[0x1b], regs[0x1d]);

	/* The restore batch itself: whole 64-byte lines, arbitration off
	 * at the start and on at the end, the register it borrows saved to
	 * the scratch given and loaded back from it, no end-of-batch. */
	static uint32_t bb[128];
	memset(bb, 0xff, sizeof(bb));
	uint32_t n = i915_lrc_wa_bb_gen9(bb, 0x00e00800);
	CHECK(n == 48, "restore batch words %u", n);
	CHECK(bb[0] == (MI_ARB_ON_OFF | MI_ARB_DISABLE), "starts with arbitration off: %08x", bb[0]);
	CHECK(bb[1] == (MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT) && bb[2] == GEN8_L3SQCREG4 &&
		      bb[3] == 0x00e00800 && bb[4] == 0,
	      "saves the L3 register to scratch: %08x %08x %08x %08x", bb[1], bb[2], bb[3], bb[4]);
	CHECK(bb[5] == MI_LOAD_REGISTER_IMM(1) && bb[6] == GEN8_L3SQCREG4 &&
		      bb[7] == (GEN8_L3SQCREG4_DEFAULT | GEN8_LQSC_FLUSH_COHERENT_LINES),
	      "sets the flush bit: %08x %08x %08x", bb[5], bb[6], bb[7]);
	CHECK(bb[8] == GFX_OP_PIPE_CONTROL(6) &&
		      bb[9] == (PIPE_CONTROL_CS_STALL | PIPE_CONTROL_DC_FLUSH_ENABLE),
	      "flushes: %08x %08x", bb[8], bb[9]);
	CHECK(bb[14] == (MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT) && bb[15] == GEN8_L3SQCREG4 &&
		      bb[16] == 0x00e00800,
	      "loads the L3 register back: %08x %08x %08x", bb[14], bb[15], bb[16]);
	CHECK(bb[18] == GFX_OP_PIPE_CONTROL(6) &&
		      bb[19] == (PIPE_CONTROL_FLUSH_L3 | PIPE_CONTROL_STORE_DATA_INDEX |
				 PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE) &&
		      bb[20] == LRC_PPHWSP_SCRATCH_ADDR,
	      "clears the shared local memory: %08x %08x %08x", bb[18], bb[19], bb[20]);
	CHECK(bb[24] == MI_LOAD_REGISTER_IMM(3) && bb[25] == COMMON_SLICE_CHICKEN2 &&
		      bb[26] == I915_MASKED_DISABLE(GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE) &&
		      bb[27] == FF_SLICE_CHICKEN &&
		      bb[28] == I915_MASKED_ENABLE(FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX) &&
		      bb[29] == _3D_CHICKEN3 &&
		      bb[30] == I915_MASKED_ENABLE(_3D_CHICKEN_SF_PROVOKING_VERTEX_FIX) &&
		      bb[31] == MI_NOOP,
	      "loads the three settings: %08x %08x %08x", bb[24], bb[26], bb[30]);
	CHECK(bb[32] == (MI_ARB_ON_OFF | MI_ARB_ENABLE), "arbitration back on: %08x", bb[32]);
	for (unsigned i = 33; i < 48; i++)
		CHECK(bb[i] == MI_NOOP, "padding word %u is %08x", i, bb[i]);
	CHECK(bb[48] == 0xffffffffu, "nothing written past the end");
	/* The null render state: fits a page, relocated to where it runs
	 * (the four base addresses of its STATE_BASE_ADDRESS command, each
	 * the batch's own address, modify-enable bit kept), ends with
	 * MI_BATCH_BUFFER_END with the state it points at after it. */
	static uint32_t page[1024];
	memset(page, 0xee, sizeof(page));
	CHECK(i915_renderstate_gen9_bytes() == 3840, "null state bytes %u", i915_renderstate_gen9_bytes());
	CHECK(i915_renderstate_gen9_build(page, 4096, 0x100000) == 0, "null state builds");
	CHECK(page[0] == GFX_OP_PIPE_CONTROL(6), "starts with a pipe control: %08x", page[0]);
	CHECK(page[6] == 0x69040300, "then selects the 3D pipeline: %08x", page[6]);
	CHECK(page[489] == 0x61010011, "the base address command: %08x", page[489]);
	CHECK(page[490] == 0x00100001 && page[491] == 0, "general state base %08x %08x", page[490], page[491]);
	CHECK(page[493] == 0x00100001 && page[494] == 0, "surface state base %08x %08x", page[493], page[494]);
	CHECK(page[495] == 0x00100001 && page[496] == 0, "dynamic state base %08x %08x", page[495], page[496]);
	CHECK(page[499] == 0x00100001 && page[500] == 0, "instruction base %08x %08x", page[499], page[500]);
	CHECK(page[497] == 1 && page[498] == 0, "the indirect object base is not relocated: %08x", page[497]);
	CHECK(page[878] == 0x7b000005, "the draw: %08x", page[878]);
	CHECK(page[885] == MI_BATCH_BUFFER_END, "ends at word 885: %08x", page[885]);
	CHECK(page[959] == 0 && page[960] == 0xeeeeeeeeu, "the state after it fills 960 words");
	CHECK(i915_renderstate_gen9_build(page, 4096, 0x00000ffffffff000ULL) == 0 &&
		      page[490] == 0xfffff001 && page[491] == 0x00000fff,
	      "a high address relocates both halves: %08x %08x", page[490], page[491]);
	CHECK(i915_renderstate_gen9_build(page, 4096, 0x100800) != 0, "an unaligned base is refused");
	CHECK(i915_renderstate_gen9_build(page, 3000, 0x100000) != 0, "too small a page is refused");
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("i915 lrc: all tests passed\n");
	return 0;
}
