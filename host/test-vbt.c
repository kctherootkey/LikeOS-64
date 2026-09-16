/*
 * Tests for the VBT parser.  See test-vbt.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/dev/gpu/i915/intel_display.h>
#include <stdio.h>
#include <string.h>

static int fails;

/* Rewrite the panel type in the already-built LVDS options block. */
static void lo_panel_type(uint8_t *vbt, unsigned bdb_off, uint8_t type)
{
	uint8_t *bdb = vbt + bdb_off;
	unsigned off = 22;
	while (off + 3 <= 8192) {
		uint8_t id = bdb[off];
		unsigned blen = (unsigned)bdb[off + 1] | ((unsigned)bdb[off + 2] << 8);
		if (!id && !blen)
			break;
		if (id == 40) {
			bdb[off + 3] = type;
			return;
		}
		off += 3 + blen;
	}
}
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned put_block(uint8_t *bdb, unsigned off, uint8_t id, const uint8_t *body, unsigned len)
{
	bdb[off] = id;
	bdb[off + 1] = len & 0xff;
	bdb[off + 2] = len >> 8;
	memcpy(bdb + off + 3, body, len);
	return off + 3 + len;
}

int main(void)
{
	static uint8_t vbt[8192];
	memset(vbt, 0, sizeof(vbt));
	memcpy(vbt, "$VBT SKYLAKE        ", 20);
	vbt[20] = 200; vbt[21] = 0; /* version */
	vbt[22] = 48; vbt[23] = 0; /* header size */
	/* The header's fields in the order the firmware writes them: the
	 * table's own size at 24, its checksum at 26, and only at 28 the
	 * offset of the data blocks.  The sizes are filled in below, once
	 * the blocks are built. */
	uint32_t bdb_off = 48;
	vbt[28] = bdb_off & 0xff;
	vbt[29] = (bdb_off >> 8) & 0xff;
	vbt[30] = 0;
	vbt[31] = 0;
	uint8_t *bdb = vbt + bdb_off;
	memcpy(bdb, "BIOS_DATA_BLOCK ", 16);
	bdb[16] = 220 & 0xff; bdb[17] = 0; /* BDB version 220 */
	bdb[18] = 22; bdb[19] = 0; /* header size */
	unsigned off = 22;

	/* general definitions: child size 44, two children: eDP on DPA (aux A, ddc 0), HDMI+DP on DPB/HDMIB */
	uint8_t gd[5 + 44 * 2];
	memset(gd, 0, sizeof(gd));
	gd[4] = 44;
	uint8_t *c = gd + 5;
	c[0] = 0x05; c[1] = 0x04; /* handle */
	c[2] = (uint8_t)((1 << 12) | (1 << 2) | (1 << 9)); c[3] = (uint8_t)(((1 << 12) | (1 << 2) | (1 << 9)) >> 8); /* internal + DP + digital */
	c[7] = 6; /* HDMI level shifter, low five bits of byte 7 */
	c[16] = 10; /* DVO_PORT_DPA */
	c[24] = 0x02; /* signalling: DisplayPort */
	c[25] = 0x40; /* AUX A */
	c = gd + 5 + 44;
	c[0] = 0x10; c[1] = 0x00;
	uint16_t dt = (1 << 2) | (1 << 9) | (1 << 4); /* DP + digital + TMDS */
	c[2] = dt & 0xff; c[3] = dt >> 8;
	c[7] = 9; /* HDMI level shifter */
	c[16] = 7; /* DVO_PORT_DPB */
	c[19] = 5; /* ddc pin */
	c[23] = 0x02; /* lane reversal */
	c[24] = 0x03; /* signalling: HDMI and DisplayPort */
	c[25] = 0x10; /* AUX B */
	off = put_block(bdb, off, 2, gd, sizeof(gd));
	/* lvds options: panel type 3 */
	uint8_t lo[2] = { 3, 0 };
	off = put_block(bdb, off, 40, lo, sizeof(lo));
	/* LFP data pointers + data: panel 3's DVO timing at data offset 3+18*3 */
	uint8_t ptrs[1 + 16 * 9];
	memset(ptrs, 0, sizeof(ptrs));
	ptrs[0] = 16;
	uint8_t data[4 * 18];
	memset(data, 0, sizeof(data));
	for (int p = 0; p < 16; p++) {
		uint16_t o = 3 + 18 * (p < 4 ? p : 0);
		ptrs[1 + p * 9 + 3] = o & 0xff; ptrs[1 + p * 9 + 4] = o >> 8; ptrs[1 + p * 9 + 5] = 18;
	}
	uint8_t *d = data + 18 * 3;
	d[0] = 14850 & 0xff; d[1] = 14850 >> 8;
	d[2] = 1920 & 0xff; d[3] = 280 & 0xff; d[4] = ((1920 >> 8) << 4) | (280 >> 8);
	d[5] = 1080 & 0xff; d[6] = 45; d[7] = ((1080 >> 8) << 4);
	d[8] = 88; d[9] = 44; d[10] = (4 << 4) | 5; d[11] = 0; d[17] = 0x1e;
	off = put_block(bdb, off, 41, ptrs, sizeof(ptrs));
	off = put_block(bdb, off, 42, data, sizeof(data));
	/* eDP: power seq for panel 3: T3 2100 (210 ms), T8 500, T9 500, T10 5000, T12 5000; then color depth, fast link */
	uint8_t edp[260];
	memset(edp, 0, sizeof(edp));
	uint8_t *seq = edp + 3 * 10;
	seq[0] = 2100 & 0xff; seq[1] = 2100 >> 8; seq[2] = 500 & 0xff; seq[3] = 500 >> 8;
	seq[4] = 500 & 0xff; seq[5] = 500 >> 8; seq[6] = 5000 & 0xff; seq[7] = 5000 >> 8;
	seq[8] = 5000 & 0xff; seq[9] = 5000 >> 8;
	uint32_t depth = 1u << (3 * 2); /* 8 bpc for panel 3 */
	edp[160] = depth & 0xff; edp[161] = (depth >> 8) & 0xff; edp[162] = (depth >> 16) & 0xff; edp[163] = depth >> 24;
	/* the link of panel 3: rate in the low nibble, lanes in the high
	 * one, one byte per panel plus one of swing and pre-emphasis */
	edp[164 + 3 * 2] = (uint8_t)(1 | (1 << 4)); /* 2.7 GHz, 2 lanes */
	edp[164 + 3 * 2 + 1] = 0x21; /* pre-emphasis 1, swing 2 */
	/* another panel's entry, to catch a reader that strides four */
	edp[164 + 6 * 2] = (uint8_t)(2 | (0 << 4)); /* 5.4 GHz, ONE lane */
	/* voltage swing and pre-emphasis, four bits per panel, from 204:
	 * panel 3 asks for swing 2, so not the low-voltage table */
	edp[204 + 1] = 0x20;
	off = put_block(bdb, off, 27, edp, sizeof(edp));
	/* backlight: entry size 6; panel 3: type 2, pwm 200 Hz, min 10 */
	uint8_t bl[1 + 16 * 6 + 16 + 16];
	memset(bl, 0, sizeof(bl));
	bl[0] = 6;
	bl[1 + 3 * 6] = 0x02; bl[1 + 3 * 6 + 1] = 200; bl[1 + 3 * 6 + 2] = 0; bl[1 + 3 * 6 + 3] = 10;
	/* Panel 4 is the same but with the active-low flag really set, so the
	 * pair distinguishes the flag (bit 2) from the type field under it
	 * (bits 0-1, where PWM is 2 and therefore has bit 1 set). */
	bl[1 + 4 * 6] = 0x06; bl[1 + 4 * 6 + 1] = 200; bl[1 + 4 * 6 + 2] = 0; bl[1 + 4 * 6 + 3] = 10;
	off = put_block(bdb, off, 43, bl, sizeof(bl));
	bdb[20] = off & 0xff; bdb[21] = off >> 8;
	/* the whole table's size, and a checksum byte, where a reader that
	 * confuses them with the block offset would pick them up */
	vbt[24] = (bdb_off + off) & 0xff;
	vbt[25] = ((bdb_off + off) >> 8) & 0xff;
	vbt[26] = 0x5a;
	vbt[27] = 0;

	struct intel_vbt out;
	CHECK(intel_vbt_parse(vbt, bdb_off + off, &out) == 0, "parse");
	CHECK(out.version == 220, "version %u", out.version);
	CHECK(out.nchildren == 2, "children %d", out.nchildren);
	CHECK(out.port[0].present && out.port[0].supports_edp && out.port[0].is_internal, "port A eDP");
	CHECK(out.port[0].aux_ch == 0, "port A aux %u", out.port[0].aux_ch);
	CHECK(out.port[1].present && out.port[1].supports_dp && out.port[1].supports_hdmi, "port B DP+HDMI");
	CHECK(out.port[1].aux_ch == 1 && out.port[1].ddc_pin == 5, "port B aux %u ddc %u", out.port[1].aux_ch, out.port[1].ddc_pin);
	CHECK(!out.port[2].present, "port C absent");
	CHECK(out.panel_type == 3, "panel type %d", out.panel_type);
	CHECK(out.panel_mode_valid && out.panel_mode.hdisplay == 1920 && out.panel_mode.vdisplay == 1080 && out.panel_mode.clock == 148500, "panel mode");
	CHECK(out.panel_mode.htotal == 2200 && out.panel_mode.vtotal == 1125, "panel timings %u %u", out.panel_mode.htotal, out.panel_mode.vtotal);
	CHECK(out.pps.valid && out.pps.t1_t3 == 2100 && out.pps.t8 == 500 && out.pps.t10 == 5000, "pps %u %u %u", out.pps.t1_t3, out.pps.t8, out.pps.t10);
	CHECK(out.panel_bpc == 8, "bpc %d", out.panel_bpc);
	CHECK(out.edp_rate == 1 && out.edp_lanes == 2, "edp link %u x%u", out.edp_rate, out.edp_lanes);
	CHECK(out.edp_low_vswing == 0, "panel 3 is not on the low-voltage table");
	CHECK(out.port[0].hdmi_level_shift == 6 && out.port[1].hdmi_level_shift == 9,
	      "hdmi level shifters %u %u", out.port[0].hdmi_level_shift,
	      out.port[1].hdmi_level_shift);
	CHECK(out.port[1].lane_reversal == 1 && out.port[0].lane_reversal == 0,
	      "lane reversal %u %u", out.port[1].lane_reversal, out.port[0].lane_reversal);
	CHECK(out.backlight_valid && out.backlight_pwm_hz == 200 && out.backlight_min == 10, "backlight %u %u", out.backlight_pwm_hz, out.backlight_min);
	/* Panel 3's type is 2 (PWM), which sets bit 1 of the same byte.  A
	 * reader that takes the polarity from bit 1 rather than bit 2 calls
	 * every panel active low and drives the backlight inverted. */
	CHECK(out.backlight_active_low == 0, "panel 3 is not active low (got %u)", out.backlight_active_low);
	/* A panel whose swing setting is zero runs on the low-voltage
	 * table; the field is four bits per panel from byte 204. */
	{
		uint8_t save = vbt[0];
		(void)save;
		lo_panel_type(vbt, bdb_off, 4);
		struct intel_vbt out4;
		CHECK(intel_vbt_parse(vbt, bdb_off + off, &out4) == 0, "parse with panel 4");
		CHECK(out4.edp_low_vswing == 1, "panel 4 is on the low-voltage table");
		CHECK(out4.backlight_active_low == 1, "panel 4 IS active low (got %u)", out4.backlight_active_low);
		lo_panel_type(vbt, bdb_off, 3);
	}

	/* garbage refused */
	CHECK(intel_vbt_parse(vbt + 1, 100, &out) != 0, "bad signature accepted");
	/* a block offset that cannot be one */
	uint8_t save[4] = { vbt[28], vbt[29], vbt[30], vbt[31] };
	vbt[28] = 4;
	vbt[29] = vbt[30] = vbt[31] = 0;
	CHECK(intel_vbt_parse(vbt, bdb_off + off, &out) != 0,
	      "a block offset inside the header accepted");
	vbt[28] = save[0];
	vbt[29] = save[1];
	vbt[30] = save[2];
	vbt[31] = save[3];
	CHECK(intel_vbt_parse(vbt, bdb_off + off, &out) == 0, "parses again");
	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("vbt: all tests passed\n");
	return 0;
}
