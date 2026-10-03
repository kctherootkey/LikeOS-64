// Host test: HDMI AVI infoframe packing and CEA video codes, the
// driver's own packer and the infoframe library's way into the
// transmitter's buffer form (intel_infoframe.c), with sinks whose
// display info comes from real EDIDs (the EDID parser and its corpus).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/hdmi.h>
#include <kernel/dev/gpu/i915/intel_infoframe.h>
#include "test-edid-corpus.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* The host C library's allocator, spelled out: <stdlib.h> disagrees with
 * the kernel's fixed-width types, which come first. */
extern void *malloc(unsigned long);
extern void *realloc(void *, unsigned long);
extern void free(void *);
extern void abort(void);

/* ---- what the files under test need from the kernel ---------------------- */

uint64_t test_irq_flag = 0x200;
int test_locks_held;

void test_fatal(const char *what, const char *name)
{
	printf("FATAL: %s (%s)\n", what, name ? name : "?");
	abort();
}

int kprintf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}

void *kalloc(size_t size)
{
	return malloc(size ? size : 1);
}

void *krealloc(void *ptr, size_t size)
{
	return realloc(ptr, size ? size : 1);
}

void kfree(void *ptr)
{
	free(ptr);
}

static int fails;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fails++;                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static unsigned frame_sum(const uint8_t *f, unsigned n)
{
	unsigned s = 0;
	for (unsigned i = 0; i < n; i++)
		s += f[i];
	return s & 0xff;
}

/* ---- the infoframe library's way ------------------------------------------------ */

/* The transmitter's form sums to zero once the hole at byte 3 is
 * skipped: it is the packed frame with that byte in between. */
static unsigned dip_sum(const uint8_t *d, int n)
{
	unsigned s = 0;
	for (int i = 0; i < n; i++)
		if (i != 3)
			s += d[i];
	return s & 0xff;
}

static struct drm_connector conns[2];
static struct drm_display_mode probed[64];

static struct drm_connector *conn_from_edid(int slot, const unsigned char *edid, unsigned len)
{
	struct drm_connector *c = &conns[slot];
	struct drm_edid_conn ec;

	memset(&ec, 0, sizeof(ec));
	memset(c, 0, sizeof(*c));
	drm_display_info_reset(&c->display_info);
	ec.connector_type = DRM_MODE_CONNECTOR_HDMIA;
	ec.info = &c->display_info;
	ec.modes = probed;
	ec.max_modes = 64;
	CHECK(drm_edid_parse_full(edid, len, &ec) > 0, "corpus EDID parses");
	return c;
}

static void test_library(void)
{
	struct intel_hdmi_infoframes f;
	struct drm_mode_modeinfo m;
	struct drm_connector *c;

	/* No connector: VIC 16 as for an HDMI 2.0 sink without the vendor
	 * block; RGB, underscan, 16:9, active = picture; the hole at 3. */
	CHECK(drm_mode_cea_vic(16, &m) == 0, "VIC 16 in the table");
	CHECK(intel_hdmi_infoframes_compute(NULL, &m, 0, &f) == 0, "computes without a connector");
	CHECK(f.avi_len == 18, "AVI in the buffer form is 18 bytes (%d)", f.avi_len);
	CHECK(f.avi[0] == 0x82 && f.avi[1] == 2 && f.avi[2] == 13 && f.avi[3] == 0,
	      "AVI header %02x %02x %02x, hole %02x", f.avi[0], f.avi[1], f.avi[2], f.avi[3]);
	CHECK(dip_sum(f.avi, f.avi_len) == 0, "AVI checksum (%u)", dip_sum(f.avi, f.avi_len));
	CHECK((f.avi[5] & 0x60) == 0 && (f.avi[5] & 0x10) && (f.avi[5] & 3) == 2,
	      "RGB, active format present, underscan (%02x)", f.avi[5]);
	CHECK(f.avi[6] == 0x28, "16:9, active aspect = picture (%02x)", f.avi[6]);
	CHECK(f.avi[8] == 16, "VIC 16 (%u)", f.avi[8]);
	CHECK(f.spd_len == 30 && f.spd[0] == 0x83 && f.spd[3] == 0 && dip_sum(f.spd, f.spd_len) == 0,
	      "SPD in the buffer form (%d)", f.spd_len);
	CHECK(!memcmp(&f.spd[5], "Intel", 5) && !memcmp(&f.spd[13], "Integrated gfx", 14),
	      "SPD names the source");
	CHECK(f.spd[29] == 0x09, "SPD source: PC (%02x)", f.spd[29]);
	CHECK(f.vendor_len == 0, "no vendor infoframe without a sink");
	CHECK(f.gcp_enable && f.gcp == 0, "general control at 8 bpc");
	CHECK(intel_hdmi_infoframes_compute(NULL, &m, 1, &f) == 0 &&
	      !memcmp(&f.spd[13], "Discrete gfx", 12), "SPD of a discrete card");

	/* An HDMI 1.4 sink from the corpus that honours the Q bit: full
	 * range said; with the vendor block, the vendor infoframe goes too. */
	c = conn_from_edid(0, test_edid_hdmi_1080p_rgb_max_200mhz, 256);
	c->display_info.rgb_quant_range_selectable = true;
	c->display_info.has_hdmi_infoframe = true;
	CHECK(intel_hdmi_infoframes_compute(c, &m, 0, &f) == 0, "computes for the 1.4 sink");
	CHECK(((f.avi[7] >> 2) & 3) == 2, "Q = full range (%02x)", f.avi[7]);
	CHECK(dip_sum(f.avi, f.avi_len) == 0, "AVI checksum with Q");
	CHECK(f.vendor_len > 4 && f.vendor[0] == 0x81 && f.vendor[3] == 0 &&
	      dip_sum(f.vendor, f.vendor_len) == 0, "HDMI vendor infoframe (%d)", f.vendor_len);
	CHECK(f.vendor[5] == 0x03 && f.vendor[6] == 0x0c && f.vendor[7] == 0x00,
	      "HDMI licensing OUI %02x %02x %02x", f.vendor[5], f.vendor[6], f.vendor[7]);
	/* not honouring it: VIC 16's default (limited) is not what is sent,
	 * so Q stays default */
	c->display_info.rgb_quant_range_selectable = false;
	CHECK(intel_hdmi_infoframes_compute(c, &m, 0, &f) == 0 && ((f.avi[7] >> 2) & 3) == 0,
	      "Q = default without the bit (%02x)", f.avi[7]);
	/* no vendor block: no vendor infoframe */
	c->display_info.has_hdmi_infoframe = false;
	CHECK(intel_hdmi_infoframes_compute(c, &m, 0, &f) == 0 && f.vendor_len == 0,
	      "no vendor infoframe without the vendor block");

	/* 3840x2160@30 (VIC 95) for an HDMI 1.4 sink with the vendor block:
	 * the HDMI 1.4 4k code 1 in the vendor frame, none in the AVI */
	c->display_info.has_hdmi_infoframe = true;
	CHECK(drm_mode_cea_vic(95, &m) == 0, "VIC 95 in the table");
	CHECK(intel_hdmi_infoframes_compute(c, &m, 0, &f) == 0, "computes 4k30");
	CHECK(f.avi[8] == 0, "AVI VIC 0 for the HDMI 1.4 4k mode (%u)", f.avi[8]);
	CHECK(f.vendor_len >= 10 && (f.vendor[8] >> 5) == 1 && f.vendor[9] == 1,
	      "vendor frame: HDMI VIC 1 (%02x %02x)", f.vendor[8], f.vendor[9]);
	drm_display_info_reset(&c->display_info);
}

int main(void)
{
	struct drm_mode_modeinfo m;
	uint8_t f[INTEL_AVI_INFOFRAME_SIZE];

	/* 1920x1080@60 is CEA VIC 16, 16:9 */
	CHECK(drm_mode_cea_vic(16, &m) == 0, "VIC 16 in the table");
	CHECK(m.hdisplay == 1920 && m.vdisplay == 1080 && m.clock == 148500,
	      "VIC 16 timing %ux%u %u", m.hdisplay, m.vdisplay, m.clock);
	CHECK(intel_hdmi_vic_for_mode(&m) == 16, "lookup of the VIC 16 timing gives %u",
	      intel_hdmi_vic_for_mode(&m));
	CHECK(intel_hdmi_avi_infoframe(&m, 16, f, sizeof(f)) == INTEL_AVI_INFOFRAME_SIZE, "packs");
	CHECK(f[0] == 0x82 && f[1] == 2 && f[2] == 13, "header %02x %02x %02x", f[0], f[1], f[2]);
	CHECK(frame_sum(f, sizeof(f)) == 0, "checksum makes the sum zero (%u)", frame_sum(f, sizeof(f)));
	CHECK((f[4] & 0x60) == 0, "RGB");
	CHECK((f[4] & 0x10) != 0, "active format present");
	CHECK((f[4] & 0x03) == 2, "underscan");
	CHECK((f[5] & 0x0f) == 8, "active aspect = picture");
	if (m.flags & DRM_MODE_FLAG_PIC_AR_16_9)
		CHECK(((f[5] >> 4) & 3) == 2, "16:9 picture aspect");
	CHECK(f[7] == 16, "VIC byte %u", f[7]);
	CHECK(f[8] == 0, "no pixel repetition");

	/* 1280x720@60 is VIC 4 */
	CHECK(drm_mode_cea_vic(4, &m) == 0, "VIC 4 in the table");
	CHECK(intel_hdmi_vic_for_mode(&m) == 4, "VIC 4 lookup gives %u", intel_hdmi_vic_for_mode(&m));

	/* a DMT timing that is not a CEA one: 1280x1024@60 */
	memset(&m, 0, sizeof(m));
	m.clock = 108000;
	m.hdisplay = 1280;
	m.hsync_start = 1328;
	m.hsync_end = 1440;
	m.htotal = 1688;
	m.vdisplay = 1024;
	m.vsync_start = 1025;
	m.vsync_end = 1028;
	m.vtotal = 1066;
	m.vrefresh = 60;
	m.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
	CHECK(intel_hdmi_vic_for_mode(&m) == 0, "1280x1024 is not a CEA timing");
	CHECK(intel_hdmi_avi_infoframe(&m, 0, f, sizeof(f)) == INTEL_AVI_INFOFRAME_SIZE, "packs without VIC");
	CHECK(f[7] == 0 && ((f[5] >> 4) & 3) == 0, "no VIC, no aspect");
	CHECK(frame_sum(f, sizeof(f)) == 0, "checksum");
	/* 4:3 flagged */
	m.flags |= DRM_MODE_FLAG_PIC_AR_4_3;
	intel_hdmi_avi_infoframe(&m, 0, f, sizeof(f));
	CHECK(((f[5] >> 4) & 3) == 1, "4:3 picture aspect");
	CHECK(frame_sum(f, sizeof(f)) == 0, "checksum with aspect");

	/* too small a buffer */
	CHECK(intel_hdmi_avi_infoframe(&m, 0, f, 16) < 0, "short buffer refused");

	test_library();

	if (fails) {
		printf("hdmi: %d failures\n", fails);
		return 1;
	}
	printf("hdmi: all tests passed\n");
	return 0;
}
