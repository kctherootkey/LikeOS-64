/*
 * Tests for the EDID parser and the standard mode table.  See test-edid.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

/* The kernel's fixed-width types come first and stand in for stdint.h,
 * whose typedefs differ in spelling (long vs long long) on the host. */
#include <kernel/dev/gpu/drm_edid.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A 1920x1080 eDP panel's base block, built here field by field: a
 * detailed timing at 148.5 MHz with the preferred flag, a name, a range
 * descriptor, two established and one standard timing. */
static void build_panel(uint8_t *e)
{
	memset(e, 0, 128);
	const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	memcpy(e, hdr, 8);
	/* manufacturer "LGD" = L(12) G(7) D(4): 5 bits each */
	uint16_t mfg = (12 << 10) | (7 << 5) | 4;
	e[8] = mfg >> 8; e[9] = mfg & 0xff;
	e[10] = 0x6b; e[11] = 0x04; /* product */
	e[18] = 1; e[19] = 4; /* EDID 1.4 */
	e[20] = 0x80 | (2 << 4); /* digital, 8 bpc */
	e[21] = 35; e[22] = 19; /* cm */
	e[24] = 0x02; /* preferred timing flag */
	e[0x23] = 0x20; /* 640x480@60 */
	e[0x24] = 0x10; /* 1024x768@60 */
	/* standard timing 1280x800@60: (1280/8-31)=129, aspect 16:10 (0), 60-60=0 */
	e[0x26] = 129; e[0x27] = 0x00;
	for (int i = 1; i < 8; i++) { e[0x26 + i * 2] = 1; e[0x27 + i * 2] = 1; }
	/* DTD 0: 1920x1080 @ 148.50 MHz: hblank 280, vblank 45, hso 88, hsw 44, vso 4, vsw 5 */
	uint8_t *d = e + 54;
	d[0] = 14850 & 0xff; d[1] = 14850 >> 8;
	d[2] = 1920 & 0xff; d[3] = 280 & 0xff; d[4] = ((1920 >> 8) << 4) | (280 >> 8);
	d[5] = 1080 & 0xff; d[6] = 45; d[7] = ((1080 >> 8) << 4) | 0;
	d[8] = 88; d[9] = 44; d[10] = (4 << 4) | 5; d[11] = 0;
	d[12] = 344 & 0xff; d[13] = 194; d[14] = ((344 >> 8) << 4) | 0;
	d[17] = 0x18 | 0x02 | 0x04; /* digital separate, +h +v */
	/* descriptor 1: name */
	d = e + 72; d[3] = 0xfc; memcpy(d + 5, "LP156WF6\n    ", 13);
	/* descriptor 2: range 48-75 Hz, 30-90 kHz, 170 MHz */
	d = e + 90; d[3] = 0xfd; d[5] = 48; d[6] = 75; d[7] = 30; d[8] = 90; d[9] = 17;
	/* descriptor 3: unused */
	d = e + 108; d[3] = 0x10;
	uint8_t sum = 0;
	for (int i = 0; i < 127; i++) sum += e[i];
	e[127] = (uint8_t)(256 - sum);
}

int main(void)
{
	uint8_t edid[256];
	struct drm_edid_info info;

	build_panel(edid);
	CHECK(drm_edid_block_valid(edid), "checksum");
	CHECK(drm_edid_parse(edid, 128, &info) == 0, "parse");
	CHECK(strcmp(info.vendor, "LGD") == 0, "vendor %s", info.vendor);
	CHECK(info.product == 0x046b, "product %x", info.product);
	CHECK(info.digital && info.bpc == 8, "digital %d bpc %d", info.digital, info.bpc);
	CHECK(info.mm_width == 350 && info.mm_height == 190, "size %ux%u", info.mm_width, info.mm_height);
	CHECK(strcmp(info.name, "LP156WF6") == 0, "name '%s'", info.name);
	CHECK(info.has_range && info.range.max_clock_khz == 170000 && info.range.max_vrefresh == 75, "range");
	CHECK(info.preferred >= 0, "preferred");
	if (info.preferred >= 0) {
		struct drm_mode_modeinfo *m = &info.modes[info.preferred];
		CHECK(m->clock == 148500 && m->hdisplay == 1920 && m->vdisplay == 1080, "pref %ux%u@%u", m->hdisplay, m->vdisplay, m->clock);
		CHECK(m->hsync_start == 2008 && m->hsync_end == 2052 && m->htotal == 2200, "h timings %u %u %u", m->hsync_start, m->hsync_end, m->htotal);
		CHECK(m->vsync_start == 1084 && m->vsync_end == 1089 && m->vtotal == 1125, "v timings %u %u %u", m->vsync_start, m->vsync_end, m->vtotal);
		CHECK(m->vrefresh == 60, "vrefresh %u", m->vrefresh);
		CHECK((m->flags & (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC)) == (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC), "polarity");
		CHECK(m->type & DRM_MODE_TYPE_PREFERRED, "type");
		CHECK(strcmp(m->name, "1920x1080") == 0, "name %s", m->name);
	}
	int have640 = 0, have1024 = 0, have1280 = 0;
	for (int i = 0; i < info.nmodes; i++) {
		if (info.modes[i].hdisplay == 640 && info.modes[i].vdisplay == 480) have640 = 1;
		if (info.modes[i].hdisplay == 1024 && info.modes[i].vdisplay == 768) have1024 = 1;
		if (info.modes[i].hdisplay == 1280 && info.modes[i].vdisplay == 800) have1280 = 1;
	}
	CHECK(have640 && have1024 && have1280, "established/standard modes (%d modes)", info.nmodes);

	/* Corrupt the checksum: refused. */
	edid[127] ^= 1;
	CHECK(drm_edid_parse(edid, 128, &info) != 0, "bad checksum accepted");
	edid[127] ^= 1;

	/* CEA extension with VIC 16 (1080p60) and an HDMI vendor block. */
	uint8_t *x = edid + 128;
	memset(x, 0, 128);
	x[0] = 0x02; x[1] = 3; x[3] = 0x80;
	x[4] = (2 << 5) | 2; x[5] = 16; x[6] = 4; /* video block: VIC 16, VIC 4 */
	x[7] = (3 << 5) | 5; x[8] = 0x03; x[9] = 0x0c; x[10] = 0x00; x[11] = 0x10; x[12] = 0x00;
	x[2] = 13;
	uint8_t sum = 0; for (int i = 0; i < 127; i++) sum += x[i]; x[127] = (uint8_t)(256 - sum);
	edid[126] = 1;
	sum = 0; for (int i = 0; i < 127; i++) sum += edid[i]; edid[127] = (uint8_t)(256 - sum);
	CHECK(drm_edid_parse(edid, 256, &info) == 0, "parse with CEA");
	CHECK(info.is_hdmi, "hdmi flag");
	int have720 = 0;
	for (int i = 0; i < info.nmodes; i++)
		if (info.modes[i].hdisplay == 1280 && info.modes[i].vdisplay == 720) have720 = 1;
	CHECK(have720, "VIC 4 mode");

	/* Mode table and CVT. */
	struct drm_mode_modeinfo m;
	CHECK(drm_mode_table_count() > 30, "table size");
	drm_mode_cvt(&m, 1920, 1080, 60, 1);
	CHECK(m.hdisplay == 1920 && m.vdisplay == 1080 && m.htotal == 2080, "cvt-rb htotal %u", m.htotal);
	CHECK(m.clock >= 138000 && m.clock <= 139500, "cvt-rb clock %u", m.clock);
	CHECK(m.vrefresh == 60, "cvt-rb vrefresh %u", m.vrefresh);
	drm_mode_cvt(&m, 1280, 800, 60, 0);
	CHECK(m.hdisplay == 1280 && m.htotal == 1680 && m.vtotal == 831, "cvt 1280x800 %u %u", m.htotal, m.vtotal);
	CHECK(m.clock >= 83000 && m.clock <= 84000, "cvt clock %u", m.clock);
	drm_mode_gtf(&m, 1024, 768, 60);
	CHECK(m.hdisplay == 1024 && m.vtotal == 795, "gtf %u", m.vtotal);
	CHECK(drm_mode_cea_vic(16, &m) == 0 && m.clock == 148500, "vic16");
	CHECK(drm_mode_cea_vic(200, &m) != 0, "vic unknown");

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("edid/modes: all tests passed\n");
	return 0;
}
