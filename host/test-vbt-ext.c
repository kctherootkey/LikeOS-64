/*
 * Tests for the VBT parser's per-port limits and feature fields: the HDMI
 * max data rate, DP++ detection, the DP link rate and lane limits, the
 * compression parameters block and the panel's variable refresh bit.
 * See test-vbt-ext.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/dev/gpu/i915/intel_display.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned put_block(uint8_t *bdb, unsigned off, uint8_t id, const uint8_t *body, unsigned len)
{
	bdb[off] = id;
	bdb[off + 1] = len & 0xff;
	bdb[off + 2] = len >> 8;
	memcpy(bdb + off + 3, body, len);
	return off + 3 + len;
}

#define CHILD 44

/* A table of BDB version `ver' with three children (eDP on A, DP+HDMI on
 * B named as DP, HDMI on C named as HDMI with an AUX channel), panel
 * type 2, an LFP power block whose VRR word is `vrr' and, when `dsc',
 * a compression parameters block.  Returns the table's length. */
static unsigned build(uint8_t *vbt, unsigned size, uint16_t ver, uint16_t vrr, int dsc,
		      int power)
{
	memset(vbt, 0, size);
	memcpy(vbt, "$VBT TEST           ", 20);
	vbt[22] = 48;
	uint32_t bdb_off = 48;
	vbt[28] = (uint8_t)bdb_off;
	uint8_t *bdb = vbt + bdb_off;
	memcpy(bdb, "BIOS_DATA_BLOCK ", 16);
	bdb[16] = ver & 0xff;
	bdb[17] = ver >> 8;
	bdb[18] = 22;
	unsigned off = 22;

	uint8_t gd[5 + CHILD * 3];
	memset(gd, 0, sizeof(gd));
	gd[4] = CHILD;
	uint8_t *c = gd + 5;
	/* eDP on DPA, asking for compression with entry 5 */
	c[0] = 1;
	c[2] = (uint8_t)((1 << 12) | (1 << 2)); c[3] = (uint8_t)(((1 << 12) | (1 << 2)) >> 8);
	c[10] = 0x02; /* compression enable */
	c[11] = 5;
	c[16] = 10; /* DVO_PORT_DPA */
	c[24] = 0x02;
	c[25] = 0x40;
	/* DP+HDMI on DPB: DP++, HDMI max data rate 594 (code 3), DP limit
	 * HBR2 (code 3 from 230, HBR from 216), two lanes (code 1, 244+),
	 * compression by the CPS method (ignored) */
	c = gd + 5 + CHILD;
	c[0] = 2;
	uint16_t dt = (1 << 2) | (1 << 4) | (1 << 1);
	c[2] = dt & 0xff; c[3] = dt >> 8;
	c[7] = (uint8_t)((3 << 5) | 4);
	c[10] = 0x06;
	c[11] = 2;
	c[16] = 7; /* DVO_PORT_DPB */
	c[23] = (uint8_t)(1 << 6);
	c[24] = 0x03;
	c[25] = 0x10;
	c[38] = 3;
	/* HDMI on HDMIC with AUX C: DP++ too; max data rate 165 (code 2) */
	c = gd + 5 + 2 * CHILD;
	c[0] = 3;
	dt = (1 << 2) | (1 << 4) | (1 << 1);
	c[2] = dt & 0xff; c[3] = dt >> 8;
	c[7] = (uint8_t)(2 << 5);
	c[16] = 2; /* DVO_PORT_HDMIC */
	c[24] = 0x01;
	c[25] = 0x20;
	off = put_block(bdb, off, 2, gd, sizeof(gd));

	uint8_t lo[2] = { 2, 0 };
	off = put_block(bdb, off, 40, lo, sizeof(lo));

	if (power) {
		uint8_t pw[60];
		memset(pw, 0, sizeof(pw));
		pw[56] = vrr & 0xff;
		pw[57] = vrr >> 8;
		off = put_block(bdb, off, 44, pw, sizeof(pw));
	}
	if (dsc) {
		uint8_t cp[2 + 16 * 13];
		memset(cp, 0, sizeof(cp));
		cp[0] = 13;
		uint8_t *e = cp + 2 + 5 * 13;
		e[0] = 0x21; /* 1.2 */
		e[1] = 1; /* 4 KB blocks */
		e[2] = 5;
		e[3] = 0x06; /* 1 or 2 slices */
		e[7] = 0x0a; /* 18-bit line buffer */
		e[8] = 1;
		e[9] = 3; /* 12 bpp */
		e[10] = 0x06; /* 8 and 10 bpc */
		e[11] = 108;
		off = put_block(bdb, off, 56, cp, sizeof(cp));
	}
	bdb[20] = off & 0xff;
	bdb[21] = off >> 8;
	return bdb_off + off;
}

int main(void)
{
	static uint8_t vbt[4096];
	struct intel_vbt out;
	unsigned len;

	len = build(vbt, sizeof(vbt), 244, 1u << 2, 1, 1);
	CHECK(intel_vbt_parse(vbt, len, &out) == 0, "parse 244");
	CHECK(out.nchildren == 3, "children %d", out.nchildren);
	CHECK(out.port[1].hdmi_max_tmds_khz == 594000, "B max TMDS %u", out.port[1].hdmi_max_tmds_khz);
	CHECK(out.port[2].hdmi_max_tmds_khz == 165000, "C max TMDS %u", out.port[2].hdmi_max_tmds_khz);
	CHECK(out.port[0].hdmi_max_tmds_khz == 0, "A max TMDS %u", out.port[0].hdmi_max_tmds_khz);
	CHECK(out.port[1].hdmi_level_shift == 4, "B level shifter %u still the low bits", out.port[1].hdmi_level_shift);
	CHECK(out.port[1].dp_dual_mode && out.port[2].dp_dual_mode && !out.port[0].dp_dual_mode,
	      "dual mode %u %u %u", out.port[0].dp_dual_mode, out.port[1].dp_dual_mode, out.port[2].dp_dual_mode);
	CHECK(out.port[1].dp_max_link_rate_khz == 540000, "B DP limit %u", out.port[1].dp_max_link_rate_khz);
	CHECK(out.port[0].dp_max_link_rate_khz == 0, "A DP limit %u", out.port[0].dp_max_link_rate_khz);
	CHECK(out.port[1].dp_max_lanes == 2 && out.port[0].dp_max_lanes == 1,
	      "lanes %u %u", out.port[0].dp_max_lanes, out.port[1].dp_max_lanes);
	CHECK(out.port[0].compression_enable && out.port[0].dsc_valid && out.port[0].dsc_index == 5, "A compression");
	CHECK(out.port[0].dsc.version_major == 1 && out.port[0].dsc.version_minor == 2, "dsc version %u.%u",
	      out.port[0].dsc.version_major, out.port[0].dsc.version_minor);
	CHECK(out.port[0].dsc.rc_buffer_block_size == 1 && out.port[0].dsc.rc_buffer_size == 5, "rc buffer");
	CHECK(out.port[0].dsc.slices_per_line == 6 && out.port[0].dsc.line_buffer_depth == 10, "slices / line buffer");
	CHECK(out.port[0].dsc.block_prediction_enable == 1 && out.port[0].dsc.max_bpp == 3, "prediction / bpp");
	CHECK(out.port[0].dsc.support_8bpc && out.port[0].dsc.support_10bpc && !out.port[0].dsc.support_12bpc, "bpc");
	CHECK(out.port[0].dsc.slice_height == 108, "slice height %u", out.port[0].dsc.slice_height);
	CHECK(out.port[1].compression_enable && out.port[1].dsc_cps && !out.port[1].dsc_valid, "CPS ignored");
	CHECK(out.lfp_vrr == 1, "panel 2's VRR bit");

	len = build(vbt, sizeof(vbt), 244, 1u << 3, 0, 1);
	CHECK(intel_vbt_parse(vbt, len, &out) == 0, "parse 244 without compression block");
	CHECK(out.lfp_vrr == 0, "panel 2 has no VRR bit");
	CHECK(out.port[0].compression_enable && !out.port[0].dsc_valid, "no block, no parameters");

	/* 229: the DP limit counts down from HBR3, code 3 = RBR; no lane
	 * limit before 244; VRR not in the table (before 233): allowed */
	len = build(vbt, sizeof(vbt), 229, 0, 1, 1);
	CHECK(intel_vbt_parse(vbt, len, &out) == 0, "parse 229");
	CHECK(out.port[1].dp_max_link_rate_khz == 162000, "216 encoding %u", out.port[1].dp_max_link_rate_khz);
	CHECK(out.port[0].dp_max_link_rate_khz == 810000, "216 encoding of 0 %u", out.port[0].dp_max_link_rate_khz);
	CHECK(out.port[1].dp_max_lanes == 0, "no lane limit before 244");
	CHECK(out.lfp_vrr == 1, "VRR before 233");

	/* 203: no HDMI max data rate yet, no DP limit */
	len = build(vbt, sizeof(vbt), 203, 0, 0, 0);
	CHECK(intel_vbt_parse(vbt, len, &out) == 0, "parse 203");
	CHECK(out.port[1].hdmi_max_tmds_khz == 0 && out.port[1].dp_max_link_rate_khz == 0, "203 limits");
	CHECK(out.lfp_vrr == 1, "no power block");

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("vbt-ext: all tests passed\n");
	return 0;
}
