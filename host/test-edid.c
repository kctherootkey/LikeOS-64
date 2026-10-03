/*
 * Tests for the EDID parser and the standard mode table.  See test-edid.sh.
 *
 * - the drivers' summary (drm_edid_parse) on a panel EDID built here, with
 *   and without a CTA extension;
 * - the recorded EDID corpus (test-edid-corpus.c) through the previous
 *   parser (test-edid-legacy.c) and the current one: the facts and the
 *   preferred mode must agree, every mode the previous parser found must
 *   still be there (or be one a plain RGB source cannot use), and the sink
 *   information of the full parse must say what the corpus says;
 * - block checks (header repair, version, checksum of CTA and other
 *   extensions), the HF-EEODB block count, quirks, DisplayID tiles and
 *   timings, ELD and short audio descriptors, stereo modes, inferred modes.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

/* The kernel's fixed-width types come first and stand in for stdint.h,
 * whose typedefs differ in spelling (long vs long long) on the host. */
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/drm_eld.h>
#include "../kernel/dev/gpu/drm/drm_edid_internal.h"
#include "test-edid-corpus.h"
#include <stdio.h>
#include <string.h>

/* stdlib.h would bring the host's stdint typedefs along. */
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);

/* The kernel allocator, for the parser. */
void *kalloc(size_t size)
{
	return malloc(size);
}

void kfree(void *ptr)
{
	free(ptr);
}

void *krealloc(void *ptr, size_t new_size)
{
	return realloc(ptr, new_size);
}

/* test-edid-legacy.c */
int legacy_edid_parse(const uint8_t *edid, unsigned len, struct drm_edid_info *out);

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void fix_checksum(uint8_t *b)
{
	uint8_t sum = 0;
	for (int i = 0; i < 127; i++)
		sum += b[i];
	b[127] = (uint8_t)(256 - sum);
}

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
	e[0x24] = 0x08; /* 1024x768@60 */
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
	fix_checksum(e);
}

static void set_vendor(uint8_t *e, char a, char b, char c, uint16_t product)
{
	uint16_t mfg = (uint16_t)(((a - '@') << 10) | ((b - '@') << 5) | (c - '@'));
	e[8] = mfg >> 8; e[9] = mfg & 0xff;
	e[10] = product & 0xff; e[11] = product >> 8;
	fix_checksum(e);
}

static int has_mode(const struct drm_edid_info *info, int w, int h, int hz)
{
	for (int i = 0; i < info->nmodes; i++)
		if (info->modes[i].hdisplay == w && info->modes[i].vdisplay == h &&
		    (!hz || (int)info->modes[i].vrefresh == hz))
			return 1;
	return 0;
}

/* The full parse into a connector view backed by static storage. */
static struct drm_display_info g_info;
static uint8_t g_eld[DRM_ELD_BUF_SIZE];
static struct drm_display_mode g_modes[256];
static struct drm_edid_conn g_conn;

static int parse_full(const uint8_t *edid, unsigned len, uint32_t type)
{
	drm_display_info_reset(&g_info);
	memset(&g_conn, 0, sizeof(g_conn));
	g_conn.connector_type = type;
	g_conn.info = &g_info;
	g_conn.eld = g_eld;
	g_conn.modes = g_modes;
	g_conn.max_modes = 256;
	return drm_edid_parse_full(edid, len, &g_conn);
}

static int full_has(int w, int h, int hz, uint32_t flags_mask, uint32_t flags)
{
	for (int i = 0; i < g_conn.nmodes; i++) {
		const struct drm_display_mode *m = &g_modes[i];
		if (m->hdisplay == w && m->vdisplay == h &&
		    (!hz || drm_mode_vrefresh(m) == hz) &&
		    (m->flags & flags_mask) == flags)
			return 1;
	}
	return 0;
}

static int bit_set(const unsigned long *map, int bit)
{
	return (map[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1;
}

/* ---- the built panel ----------------------------------------------------- */

static void test_panel(void)
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
	CHECK(has_mode(&info, 640, 480, 0) && has_mode(&info, 1024, 768, 0) &&
	      has_mode(&info, 1280, 800, 0), "established/standard modes (%d modes)", info.nmodes);
	for (int i = 0; i < info.nmodes; i++)
		if (i != info.preferred)
			CHECK(!(info.modes[i].type & DRM_MODE_TYPE_PREFERRED), "second preferred mode %d", i);

	/* Corrupt the checksum: refused. */
	edid[127] ^= 1;
	CHECK(drm_edid_parse(edid, 128, &info) != 0, "bad checksum accepted");
	edid[127] ^= 1;

	/* A header damaged in transfer (the checksum is over the right
	 * bytes): two bytes wrong are repaired, three are refused. */
	edid[1] = 0x7f; edid[2] = 0x00;
	CHECK(drm_edid_parse(edid, 128, &info) == 0 && info.preferred >= 0, "header repair");
	edid[3] = 0x00;
	CHECK(drm_edid_parse(edid, 128, &info) != 0, "corrupt header accepted");
	build_panel(edid);
	/* Major version 2: refused. */
	edid[18] = 2; fix_checksum(edid);
	CHECK(drm_edid_parse(edid, 128, &info) != 0, "EDID version 2 accepted");
	build_panel(edid);

	/* CEA extension with VIC 16 (1080p60) and an HDMI vendor block. */
	uint8_t *x = edid + 128;
	memset(x, 0, 128);
	x[0] = 0x02; x[1] = 3; x[3] = 0x80;
	x[4] = (2 << 5) | 2; x[5] = 16; x[6] = 4; /* video block: VIC 16, VIC 4 */
	x[7] = (3 << 5) | 5; x[8] = 0x03; x[9] = 0x0c; x[10] = 0x00; x[11] = 0x10; x[12] = 0x00;
	x[2] = 13;
	fix_checksum(x);
	edid[126] = 1;
	fix_checksum(edid);
	CHECK(drm_edid_parse(edid, 256, &info) == 0, "parse with CEA");
	CHECK(info.is_hdmi, "hdmi flag");
	CHECK(has_mode(&info, 1280, 720, 60), "VIC 4 mode");
	CHECK(info.cea_underscan, "underscan");

	/* A CTA block with a bad checksum is still used. */
	x[127] ^= 0x55;
	CHECK(drm_edid_parse(edid, 256, &info) == 0 && info.is_hdmi &&
	      has_mode(&info, 1280, 720, 60), "CTA block with bad checksum dropped");
	/* Any other extension with a bad checksum is dropped. */
	x[0] = VTB_EXT;
	CHECK(drm_edid_parse(edid, 256, &info) == 0 && !info.is_hdmi, "bad VTB block used");
	x[0] = CEA_EXT;
	fix_checksum(x);

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
	CHECK(drm_mode_cea_vic(250, &m) != 0, "vic unknown");
}

/* ---- the corpus, old parser against new ---------------------------------- */

struct corpus {
	const char *name;
	const unsigned char *edid;
	unsigned len;
};

static const struct corpus corpus[] = {
	{ "dvi_1080p", test_edid_dvi_1080p, 128 },
	{ "hdmi_1080p_rgb_max_100mhz", test_edid_hdmi_1080p_rgb_max_100mhz, 256 },
	{ "hdmi_1080p_rgb_max_200mhz", test_edid_hdmi_1080p_rgb_max_200mhz, 256 },
	{ "hdmi_1080p_rgb_max_200mhz_hdr", test_edid_hdmi_1080p_rgb_max_200mhz_hdr, 256 },
	{ "hdmi_1080p_rgb_max_340mhz", test_edid_hdmi_1080p_rgb_max_340mhz, 256 },
	{ "hdmi_1080p_rgb_yuv_dc_max_200mhz", test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz, 256 },
	{ "hdmi_1080p_rgb_yuv_dc_max_340mhz", test_edid_hdmi_1080p_rgb_yuv_dc_max_340mhz, 256 },
	{ "hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz", test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz, 256 },
	{ "hdmi_4k_rgb_yuv420_dc_max_340mhz", test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz, 256 },
};

/* Is the old mode one the new summary leaves out on purpose: a mode the
 * sink takes only as YCbCr 4:2:0? */
static int is_420_only(const struct drm_mode_modeinfo *u)
{
	struct drm_display_mode dm;

	if (drm_mode_from_umode(&dm, u) != 0)
		return 0;
	dm.picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_NONE;
	return drm_mode_is_420_only(&g_info, &dm);
}

static void compare_with_legacy(const struct corpus *c, int verbose)
{
	static struct drm_edid_info old, new;
	int rc_old = legacy_edid_parse(c->edid, c->len, &old);
	int rc_new = drm_edid_parse(c->edid, c->len, &new);

	CHECK(rc_old == 0 && rc_new == 0, "%s: parse %d/%d", c->name, rc_old, rc_new);
	if (rc_old || rc_new)
		return;
	parse_full(c->edid, c->len, DRM_MODE_CONNECTOR_HDMIA);

	CHECK(old.version == new.version && old.revision == new.revision, "%s: version", c->name);
	CHECK(strcmp(old.vendor, new.vendor) == 0, "%s: vendor %s/%s", c->name, old.vendor, new.vendor);
	CHECK(old.product == new.product && old.serial == new.serial, "%s: product", c->name);
	CHECK(strcmp(old.name, new.name) == 0, "%s: name '%s'/'%s'", c->name, old.name, new.name);
	CHECK(old.mm_width == new.mm_width && old.mm_height == new.mm_height, "%s: size", c->name);
	CHECK(old.digital == new.digital, "%s: digital", c->name);
	CHECK(old.is_hdmi == new.is_hdmi, "%s: hdmi %d/%d", c->name, old.is_hdmi, new.is_hdmi);
	CHECK(old.has_audio == new.has_audio, "%s: audio %d/%d", c->name, old.has_audio, new.has_audio);
	CHECK(old.cea_underscan == new.cea_underscan, "%s: underscan", c->name);
	CHECK(old.has_range == new.has_range &&
	      memcmp(&old.range, &new.range, sizeof(old.range)) == 0, "%s: range", c->name);
	CHECK(new.nmodes <= DRM_EDID_MAX_MODES, "%s: %d modes", c->name, new.nmodes);

	/* The preferred mode is the same timing. */
	CHECK(old.preferred >= 0 && new.preferred >= 0, "%s: preferred %d/%d", c->name, old.preferred, new.preferred);
	if (old.preferred >= 0 && new.preferred >= 0)
		CHECK(drm_mode_equal(&old.modes[old.preferred], &new.modes[new.preferred]),
		      "%s: preferred %s/%s", c->name, old.modes[old.preferred].name,
		      new.modes[new.preferred].name);

	/* Every old mode is still offered, unless the sink takes it only as
	 * 4:2:0. */
	for (int i = 0; i < old.nmodes; i++) {
		int found = 0;
		for (int j = 0; j < new.nmodes && !found; j++)
			found = drm_mode_equal(&old.modes[i], &new.modes[j]);
		if (!found && is_420_only(&old.modes[i])) {
			if (verbose)
				printf("  %s: %s@%u left out (4:2:0 only)\n", c->name,
				       old.modes[i].name, old.modes[i].vrefresh);
			continue;
		}
		CHECK(found, "%s: old mode %s@%u %u kHz missing", c->name, old.modes[i].name,
		      old.modes[i].vrefresh, old.modes[i].clock);
	}
	if (verbose) {
		printf("  %s: old %d modes, new %d modes (full parse %d), bpc %d/%d:", c->name,
		       old.nmodes, new.nmodes, g_conn.nmodes, old.bpc, new.bpc);
		for (int j = 0; j < new.nmodes; j++) {
			int known = 0;
			for (int i = 0; i < old.nmodes && !known; i++)
				known = drm_mode_equal(&old.modes[i], &new.modes[j]);
			if (!known)
				printf(" +%s@%u", new.modes[j].name, new.modes[j].vrefresh);
		}
		printf("\n");
	}

	/* The summary of the bytes alone, the extension not stored. */
	CHECK(drm_edid_parse(c->edid, 128, &new) == 0 && new.preferred >= 0, "%s: base block only", c->name);
}

static void test_corpus_facts(void)
{
	/* DVI: an EDID 1.3 digital DFP sink means 8 bpc, RGB only. */
	parse_full(test_edid_dvi_1080p, 128, DRM_MODE_CONNECTOR_DVID);
	CHECK(!g_info.is_hdmi && g_info.bpc == 8 && g_info.color_formats == DRM_COLOR_FORMAT_RGB444,
	      "dvi: hdmi %d bpc %u formats %x", g_info.is_hdmi, g_info.bpc, g_info.color_formats);
	CHECK(g_info.width_mm == 1600 && g_info.height_mm == 900, "dvi: size");
	CHECK(full_has(1920, 1080, 60, 0, 0) && g_modes[0].type & DRM_MODE_TYPE_PREFERRED, "dvi: preferred");
	CHECK(g_info.source_physical_address == 0xffff, "dvi: no CEC address");

	static const struct { const unsigned char *e; int tmds; } clocks[] = {
		{ test_edid_hdmi_1080p_rgb_max_100mhz, 100000 },
		{ test_edid_hdmi_1080p_rgb_max_200mhz, 200000 },
		{ test_edid_hdmi_1080p_rgb_max_340mhz, 340000 },
	};
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		parse_full(clocks[i].e, 256, DRM_MODE_CONNECTOR_HDMIA);
		CHECK(g_info.is_hdmi && g_info.max_tmds_clock == clocks[i].tmds && g_info.bpc == 8 &&
		      g_info.color_formats == DRM_COLOR_FORMAT_RGB444 && g_info.cea_rev == 3,
		      "hdmi %d: hdmi %d tmds %d bpc %u formats %x", clocks[i].tmds, g_info.is_hdmi,
		      g_info.max_tmds_clock, g_info.bpc, g_info.color_formats);
		CHECK(g_info.edid_hdmi_rgb444_dc_modes == 0, "hdmi %d: deep colour", clocks[i].tmds);
		CHECK(g_info.vics_len == 1 && g_info.vics[0] == 16, "hdmi %d: vics", clocks[i].tmds);
		/* the ELD: version, name, HDMI, no SADs */
		CHECK((g_eld[DRM_ELD_VER] & DRM_ELD_VER_MASK) == DRM_ELD_VER_CEA861D, "eld version");
		CHECK(drm_eld_mnl(g_eld) == 9 && memcmp(g_eld + DRM_ELD_MONITOR_NAME_STRING, "Test EDID", 9) == 0,
		      "eld name");
		CHECK(drm_eld_get_conn_type(g_eld) == DRM_ELD_CONN_TYPE_HDMI && drm_eld_sad_count(g_eld) == 0,
		      "eld type/sads");
		CHECK(drm_eld_size(g_eld) == 4 + DIV_ROUND_UP(16 + 9, 4) * 4, "eld size %d", drm_eld_size(g_eld));
	}

	/* HDR static metadata and the luminance it implies. */
	parse_full(test_edid_hdmi_1080p_rgb_max_200mhz_hdr, 256, DRM_MODE_CONNECTOR_HDMIA);
	{
		const struct hdr_static_metadata *h = &g_info.hdr_sink_metadata.hdmi_type1;
		CHECK(h->eotf == ((1 << HDMI_EOTF_TRADITIONAL_GAMMA_SDR) | (1 << HDMI_EOTF_SMPTE_ST2084)) &&
		      h->metadata_type == 1 && h->max_cll == 82 && h->max_fall == 82 && h->min_cll == 81,
		      "hdr: eotf %x type %x cll %u fall %u min %u", h->eotf, h->metadata_type,
		      h->max_cll, h->max_fall, h->min_cll);
		CHECK(g_info.luminance_range.max_luminance == 296 && g_info.luminance_range.min_luminance == 0,
		      "hdr: luminance %u-%u", g_info.luminance_range.min_luminance,
		      g_info.luminance_range.max_luminance);
		CHECK(g_info.source_physical_address == 0x1234, "hdr: CEC address %x",
		      g_info.source_physical_address);
	}

	/* Deep colour and YCbCr. */
	static const unsigned char *const dc[] = {
		test_edid_hdmi_1080p_rgb_yuv_dc_max_200mhz,
		test_edid_hdmi_1080p_rgb_yuv_dc_max_340mhz,
	};
	for (unsigned i = 0; i < 2; i++) {
		parse_full(dc[i], 256, DRM_MODE_CONNECTOR_HDMIA);
		CHECK(g_info.color_formats == (DRM_COLOR_FORMAT_RGB444 | DRM_COLOR_FORMAT_YCBCR444 |
					       DRM_COLOR_FORMAT_YCBCR422),
		      "dc: formats %x", g_info.color_formats);
		CHECK(g_info.edid_hdmi_rgb444_dc_modes == (DRM_EDID_HDMI_DC_48 | DRM_EDID_HDMI_DC_36 |
							   DRM_EDID_HDMI_DC_30) &&
		      g_info.edid_hdmi_ycbcr444_dc_modes == g_info.edid_hdmi_rgb444_dc_modes &&
		      g_info.bpc == 16, "dc: modes %x/%x bpc %u", g_info.edid_hdmi_rgb444_dc_modes,
		      g_info.edid_hdmi_ycbcr444_dc_modes, g_info.bpc);
	}

	/* 4K only as 4:2:0, the HDMI forum block. */
	parse_full(test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz, 256, DRM_MODE_CONNECTOR_HDMIA);
	CHECK(g_info.hdmi.scdc.supported && g_info.hdmi.scdc.scrambling.supported &&
	      g_info.max_tmds_clock == 200000 && g_info.hdmi.y420_dc_modes == DRM_EDID_YCBCR420_DC_MASK,
	      "420: scdc %d scrambling %d tmds %d dc %x", g_info.hdmi.scdc.supported,
	      g_info.hdmi.scdc.scrambling.supported, g_info.max_tmds_clock, g_info.hdmi.y420_dc_modes);
	CHECK(g_info.color_formats & DRM_COLOR_FORMAT_YCBCR420, "420: format");
	CHECK(bit_set(g_info.hdmi.y420_vdb_modes, 95) && !bit_set(g_info.hdmi.y420_vdb_modes, 16), "420: vdb map");
	/* an empty capability map: no VDB mode also as 4:2:0 */
	CHECK(!bit_set(g_info.hdmi.y420_cmdb_modes, 95) && !bit_set(g_info.hdmi.y420_cmdb_modes, 16), "420: cmdb map");
	CHECK(full_has(3840, 2160, 30, 0, 0), "420: 4K mode in the full parse");
	{
		struct drm_edid_info s;
		CHECK(drm_edid_parse(test_edid_hdmi_1080p_rgb_yuv_4k_yuv420_dc_max_200mhz, 256, &s) == 0 &&
		      !has_mode(&s, 3840, 2160, 30) && s.bpc == 16, "420: 4K 4:2:0-only mode offered");
	}

	parse_full(test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz, 256, DRM_MODE_CONNECTOR_HDMIA);
	CHECK(g_info.max_tmds_clock == 340000 && bit_set(g_info.hdmi.y420_cmdb_modes, 95) &&
	      !bit_set(g_info.hdmi.y420_vdb_modes, 95), "4k: tmds %d", g_info.max_tmds_clock);
	CHECK(g_conn.nmodes > 0 && g_modes[0].hdisplay == 3840 && (g_modes[0].type & DRM_MODE_TYPE_PREFERRED),
	      "4k: preferred");
}

/* ---- synthetic EDIDs ------------------------------------------------------ */

static void test_quirks(void)
{
	uint8_t edid[128];
	struct drm_edid_info info;

	/* SDC 0x3652 says 8 bpc and is a 6 bpc panel. */
	build_panel(edid);
	set_vendor(edid, 'S', 'D', 'C', 0x3652);
	CHECK(drm_edid_parse(edid, 128, &info) == 0 && info.bpc == 6, "force 6bpc: %d", info.bpc);

	/* SAM 596: the first detailed mode is wrong, the largest 60 Hz one is
	 * preferred. */
	build_panel(edid);
	edid[54] = 6500 & 0xff; edid[55] = 6500 >> 8; /* DTD 0: 1024x768 */
	edid[56] = 1024 & 0xff; edid[57] = 320 & 0xff; edid[58] = ((1024 >> 8) << 4) | (320 >> 8);
	edid[59] = 768 & 0xff; edid[60] = 38; edid[61] = ((768 >> 8) << 4);
	edid[62] = 24; edid[63] = 136; edid[64] = (3 << 4) | 6; edid[65] = 0;
	set_vendor(edid, 'S', 'A', 'M', 596);
	CHECK(drm_edid_parse(edid, 128, &info) == 0 && info.preferred >= 0, "prefer large: parse");
	if (info.preferred >= 0)
		CHECK(info.modes[info.preferred].hdisplay == 1280 && info.modes[info.preferred].vrefresh == 60,
		      "prefer large: %s@%u", info.modes[info.preferred].name,
		      info.modes[info.preferred].vrefresh);
	/* Without the quirk the first detailed timing is preferred. */
	set_vendor(edid, 'L', 'G', 'D', 0x046b);
	CHECK(drm_edid_parse(edid, 128, &info) == 0 && info.preferred >= 0 &&
	      info.modes[info.preferred].hdisplay == 1024, "no quirk: preferred");

	/* A driver-visible quirk. */
	build_panel(edid);
	set_vendor(edid, 'H', 'W', 'P', 0x2869);
	parse_full(edid, 128, DRM_MODE_CONNECTOR_DisplayPort);
	CHECK(drm_edid_has_quirk(&g_info, DRM_EDID_QUIRK_DP_DPCD_PROBE), "DPCD probe quirk");
}

/* A CTA block: data blocks then nothing. */
static uint8_t *cta_begin(uint8_t *x)
{
	memset(x, 0, 128);
	x[0] = CEA_EXT; x[1] = 3;
	return x + 4;
}

static void cta_end(uint8_t *x, uint8_t *p, uint8_t flags)
{
	x[2] = (uint8_t)(p - x);
	x[3] = flags;
	fix_checksum(x);
}

static void test_audio_and_stereo(void)
{
	static uint8_t edid[256];
	uint8_t *x = edid + 128, *p;

	build_panel(edid);
	edid[126] = 1;
	fix_checksum(edid);
	p = cta_begin(x);
	/* audio: LPCM 2ch 32/44.1/48 kHz 16/20/24 bit, AC-3 6ch 48 kHz 640 kbit */
	*p++ = (CTA_DB_AUDIO << 5) | 6;
	*p++ = (1 << 3) | 1; *p++ = 0x07; *p++ = 0x07;
	*p++ = (2 << 3) | 5; *p++ = 0x04; *p++ = 80;
	/* speakers FL/FR, LFE, FC */
	*p++ = (CTA_DB_SPEAKER << 5) | 3; *p++ = 0x07; *p++ = 0; *p++ = 0;
	/* video: VIC 16 (1080p60), VIC 4 (720p60) */
	*p++ = (CTA_DB_VIDEO << 5) | 2; *p++ = 16; *p++ = 4;
	/* HDMI: address 1.0.0.0, AI, latency 20/30 ms, 3D present */
	*p++ = (CTA_DB_VENDOR << 5) | 12;
	*p++ = 0x03; *p++ = 0x0c; *p++ = 0x00; *p++ = 0x10; *p++ = 0x00;
	*p++ = 0x80; /* AI */
	*p++ = 30; /* 150 MHz */
	*p++ = 0x80 | 0x20; /* latency present, HDMI video present */
	*p++ = 11; *p++ = 16; /* video 20 ms, audio 30 ms */
	*p++ = 0x80; /* 3D present */
	*p++ = 0x00; /* no VICs, no 3D structures */
	cta_end(x, p, 0x40 | 0x80);

	parse_full(edid, 256, DRM_MODE_CONNECTOR_HDMIA);
	CHECK(g_info.has_audio && g_info.is_hdmi && g_info.max_tmds_clock == 150000, "audio sink");
	CHECK(drm_eld_sad_count(g_eld) == 2 && (g_eld[DRM_ELD_SAD_COUNT_CONN_TYPE] & DRM_ELD_SUPPORTS_AI) &&
	      g_eld[DRM_ELD_SPEAKER] == 0x07, "eld sads %d", drm_eld_sad_count(g_eld));
	{
		struct cea_sad sad;
		CHECK(drm_eld_sad_get(g_eld, 1, &sad) == 0 && sad.format == 2 && sad.channels == 5 &&
		      sad.byte2 == 80, "eld sad 1");
		CHECK(drm_eld_sad_get(g_eld, 2, &sad) != 0, "eld sad 2");
	}
	CHECK(g_conn.latency_present[0] && g_conn.video_latency[0] == 11 && g_conn.audio_latency[0] == 16,
	      "latency");
	{
		struct drm_display_mode m;
		memset(&m, 0, sizeof(m));
		CHECK(drm_av_sync_delay(&g_conn, &m) == 0, "av delay %d", drm_av_sync_delay(&g_conn, &m));
	}
	{
		struct cea_sad *sads = NULL;
		uint8_t *sadb = NULL;
		int n = drm_edid_to_sad((const struct edid *)edid, &sads);
		CHECK(n == 2 && sads && sads[0].format == 1 && sads[0].channels == 1 && sads[0].freq == 7,
		      "to_sad %d", n);
		free(sads);
		n = drm_edid_to_speaker_allocation((const struct edid *)edid, &sadb);
		CHECK(n == 3 && sadb && sadb[0] == 0x07, "speaker allocation %d", n);
		free(sadb);
		CHECK(drm_detect_hdmi_monitor((const struct edid *)edid) &&
		      drm_detect_monitor_audio((const struct edid *)edid), "detect hdmi/audio");
	}
	/* The mandatory stereo modes are in the full parse, not in the
	 * summary; the 59.94 Hz twins are in both. */
	CHECK(full_has(1280, 720, 60, DRM_MODE_FLAG_3D_MASK, DRM_MODE_FLAG_3D_FRAME_PACKING) &&
	      full_has(1280, 720, 60, DRM_MODE_FLAG_3D_MASK, DRM_MODE_FLAG_3D_TOP_AND_BOTTOM), "stereo modes");
	{
		struct drm_edid_info s;
		int twins = 0;
		CHECK(drm_edid_parse(edid, 256, &s) == 0, "stereo summary");
		for (int i = 0; i < s.nmodes; i++) {
			CHECK(!(s.modes[i].flags & (DRM_MODE_FLAG_3D_MASK | DRM_MODE_FLAG_PIC_AR_MASK)),
			      "summary mode %s flags %x", s.modes[i].name, s.modes[i].flags);
			if (s.modes[i].hdisplay == 1280 && s.modes[i].vdisplay == 720)
				twins++;
		}
		CHECK(twins == 2, "720p and its 59.94 Hz twin: %d", twins);
		CHECK(drm_edid_parse(edid, 256, &s) == 0 && s.has_audio, "summary audio");
	}
}

/* DisplayID extension: a tiled display and a type I timing. */
static void test_displayid(void)
{
	static uint8_t edid[256];
	uint8_t *x = edid + 128, *p;

	build_panel(edid);
	edid[126] = 1;
	fix_checksum(edid);
	memset(x, 0, 128);
	x[0] = DISPLAYID_EXT;
	p = x + 1;
	p[0] = 0x12; /* DisplayID 1.2 */
	p[2] = PRODUCT_TYPE_MONITOR;
	p += 4;
	/* tiled display: 2x1, this one at 1,0, 1920x2160 tiles */
	*p++ = DATA_BLOCK_TILED_DISPLAY; *p++ = 0; *p++ = 22;
	*p++ = 0x80; /* one physical monitor */
	*p++ = (1 << 4) | 0; /* 2 horizontal - 1, 1 vertical - 1 */
	*p++ = (1 << 4) | 0; /* location h 1, v 0 */
	*p++ = 0;
	*p++ = (1920 - 1) & 0xff; *p++ = (1920 - 1) >> 8;
	*p++ = (2160 - 1) & 0xff; *p++ = (2160 - 1) >> 8;
	p += 5; /* bezel */
	memcpy(p, "ABCDEFGHI", 9); p += 9;
	/* type I detailed timing: 1920x2160 at 266.64 MHz, preferred */
	*p++ = DATA_BLOCK_TYPE_1_DETAILED_TIMING; *p++ = 0; *p++ = 20;
	{
		unsigned clk = 26664 - 1; /* 10 kHz units, minus one */
		*p++ = clk & 0xff; *p++ = (clk >> 8) & 0xff; *p++ = clk >> 16;
		*p++ = 0x80;
		uint16_t v[8] = { 1920 - 1, 80 - 1, (uint16_t)(0x8000 | (48 - 1)), 32 - 1,
				  2160 - 1, 62 - 1, 3 - 1, 5 - 1 };
		for (int i = 0; i < 8; i++) { *p++ = v[i] & 0xff; *p++ = v[i] >> 8; }
	}
	x[2] = (uint8_t)(p - (x + 5)); /* section payload bytes */
	{
		uint8_t sum = 0;
		for (uint8_t *q = x + 1; q < p; q++)
			sum += *q;
		*p = (uint8_t)(256 - sum);
	}
	fix_checksum(x);

	parse_full(edid, 256, DRM_MODE_CONNECTOR_DisplayPort);
	CHECK(g_conn.tile.has_tile && g_conn.tile.tile_is_single_monitor && g_conn.tile.num_h_tile == 2 &&
	      g_conn.tile.num_v_tile == 1 && g_conn.tile.tile_h_loc == 1 && g_conn.tile.tile_v_loc == 0 &&
	      g_conn.tile.tile_h_size == 1920 && g_conn.tile.tile_v_size == 2160 &&
	      memcmp(g_conn.tile.topology_id, "ABCDEFGH", 8) == 0,
	      "tile %d %dx%d at %d,%d size %ux%u", g_conn.tile.has_tile, g_conn.tile.num_h_tile,
	      g_conn.tile.num_v_tile, g_conn.tile.tile_h_loc, g_conn.tile.tile_v_loc,
	      g_conn.tile.tile_h_size, g_conn.tile.tile_v_size);
	CHECK(full_has(1920, 2160, 0, DRM_MODE_FLAG_PHSYNC, DRM_MODE_FLAG_PHSYNC), "displayid timing");
	CHECK(drm_eld_get_conn_type(g_eld) == DRM_ELD_CONN_TYPE_DP, "eld dp");
}

/* HF-EEODB: the base block announces one extension, the EEODB three. */
static void test_eeodb(void)
{
	static uint8_t edid[512];
	uint8_t *x, *p;
	struct drm_edid_info info;

	build_panel(edid);
	edid[126] = 1;
	fix_checksum(edid);
	x = edid + 128;
	p = cta_begin(x);
	*p++ = (CTA_DB_EXTENDED_TAG << 5) | 2; *p++ = CTA_EXT_DB_HF_EEODB; *p++ = 3;
	cta_end(x, p, 0);
	x = edid + 256;
	p = cta_begin(x);
	*p++ = (CTA_DB_VIDEO << 5) | 1; *p++ = 19; /* 720p50 */
	cta_end(x, p, 0);
	x = edid + 384;
	p = cta_begin(x);
	*p++ = (CTA_DB_VIDEO << 5) | 1; *p++ = 17; /* 576p50 */
	cta_end(x, p, 0);

	{
		struct drm_edid de = { .size = 512, .edid = (const struct edid *)edid };
		CHECK(edid_hfeeodb_block_count(de.edid) == 4 && __drm_edid_block_count(&de) == 4,
		      "eeodb count %d", __drm_edid_block_count(&de));
	}
	CHECK(drm_edid_parse(edid, 512, &info) == 0 && has_mode(&info, 1280, 720, 50) &&
	      has_mode(&info, 720, 576, 50), "eeodb blocks parsed");
	/* Stored blocks bound what is read. */
	CHECK(drm_edid_parse(edid, 384, &info) == 0 && has_mode(&info, 1280, 720, 50) &&
	      !has_mode(&info, 720, 576, 50), "eeodb stored blocks");
}

/* A continuous-frequency EDID 1.4 with a CVT range: the inferred modes. */
static void test_inferred(void)
{
	uint8_t edid[128];
	struct drm_edid_info old, info;

	build_panel(edid);
	edid[24] |= DRM_EDID_FEATURE_CONTINUOUS_FREQ;
	edid[90 + 10] = DRM_EDID_CVT_SUPPORT_FLAG; /* range descriptor: CVT */
	edid[90 + 11] = 1; /* CVT 1.1 */
	edid[90 + 13] = 0; /* no max active pixels */
	edid[90 + 14] = 0xf0; /* aspects */
	edid[90 + 15] = DRM_EDID_CVT_FLAGS_REDUCED_BLANKING;
	fix_checksum(edid);
	CHECK(legacy_edid_parse(edid, 128, &old) == 0 && drm_edid_parse(edid, 128, &info) == 0, "inferred parse");
	CHECK(info.nmodes > old.nmodes && has_mode(&info, 1680, 1050, 60) && has_mode(&info, 1600, 900, 60),
	      "inferred modes: %d (old %d)", info.nmodes, old.nmodes);
	for (int i = 0; i < info.nmodes; i++)
		CHECK(info.modes[i].clock <= 170000 && info.modes[i].hdisplay <= 1920,
		      "inferred mode %s out of range", info.modes[i].name);
	parse_full(edid, 128, DRM_MODE_CONNECTOR_eDP);
	CHECK(g_info.monitor_range.min_vfreq == 0, "range: only for range-limits-only");
	edid[90 + 10] = DRM_EDID_RANGE_LIMITS_ONLY_FLAG;
	fix_checksum(edid);
	parse_full(edid, 128, DRM_MODE_CONNECTOR_eDP);
	CHECK(g_info.monitor_range.min_vfreq == 48 && g_info.monitor_range.max_vfreq == 75, "vrr range %u-%u",
	      g_info.monitor_range.min_vfreq, g_info.monitor_range.max_vfreq);
}

/* The container checks. */
static void test_containers(void)
{
	static uint8_t edid[256];
	const struct drm_edid *de;

	build_panel(edid);
	de = drm_edid_alloc_checked(edid, 128);
	CHECK(de && de->size == 128 && drm_edid_valid(de), "checked base");
	CHECK(de && drm_edid_get_panel_id(de) == drm_edid_encode_panel_id('L', 'G', 'D', 0x046b), "panel id");
	drm_edid_free(de);
	/* the base block announces an extension that fails: dropped */
	edid[126] = 1;
	fix_checksum(edid);
	memset(edid + 128, 0x5a, 128);
	edid[128] = 0x40;
	de = drm_edid_alloc_checked(edid, 256);
	CHECK(de && de->size == 128 && de->edid->extensions == 0 && drm_edid_valid(de), "invalid ext dropped");
	drm_edid_free(de);
	{
		char name[16];
		drm_edid_get_monitor_name((const struct edid *)edid, name, sizeof(name));
		CHECK(strcmp(name, "LP156WF6") == 0, "monitor name '%s'", name);
	}
	CHECK(drm_edid_header_is_valid(edid) == 8, "header score");
	memset(edid, 0, 128);
	CHECK(edid_block_check(edid, true) == EDID_BLOCK_ZERO, "zero block");
	CHECK(drm_edid_alloc_checked(edid, 128) == NULL, "zero block accepted");
}

int main(int argc, char **argv)
{
	int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

	test_panel();
	for (unsigned i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++)
		compare_with_legacy(&corpus[i], verbose);
	test_corpus_facts();
	test_quirks();
	test_audio_and_stereo();
	test_displayid();
	test_eeodb();
	test_inferred();
	test_containers();
	drm_display_info_reset(&g_info);

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("edid/modes: all tests passed\n");
	return 0;
}
