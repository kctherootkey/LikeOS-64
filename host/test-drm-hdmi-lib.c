/*
 * Tests for the HDMI library: the infoframe packer, checker, unpacker and
 * logger (kernel/dev/gpu/drm/drm_hdmi_infoframe.c), the AVI and vendor
 * infoframes of a mode on a connector whose display info comes from real
 * EDIDs (the EDID parser and its corpus), the HDMI helpers
 * (drm_hdmi_helper.c: colorimetry, content type, HDR metadata, TMDS
 * character rate, audio N/CTS), and the SCDC and DP dual-mode register
 * access (drm_scdc_helper.c, drm_dp_dual_mode_helper.c) against a fake
 * I2C bus that models a sink's SCDC registers at 0x54 and an adaptor's
 * registers at 0x40.  The infoframe byte vectors are worked out by hand
 * from the field layouts of CTA-861 and HDMI 1.4.  See test-drm-hdmi-lib.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 * SPDX-License-Identifier for the portions derived from the connector unit tests: GPL-2.0
 */

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/hdmi.h>
#include <kernel/dev/gpu/drm_hdmi_helper.h>
#include <kernel/dev/gpu/drm_scdc_helper.h>
#include <kernel/dev/gpu/drm_dp_dual_mode_helper.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/drm_dp.h>
#include <kernel/dev/i2c.h>
#include <kernel/ke/syscall.h>
#include "test-edid-corpus.h"

/* The host C library's declarations, spelled out: its headers disagree
 * with the kernel's fixed-width types, which come first. */
extern int printf(const char *, ...);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void *realloc(void *, unsigned long);
extern void free(void *);
extern void abort(void);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, unsigned long);
extern char *strstr(const char *, const char *);

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- what the files under test need from the kernel ---------------------- */

uint64_t test_irq_flag = 0x200;
int test_locks_held;
static int warnings;
static char lastlog[4096];
static int loglen;

void test_fatal(const char *what, const char *name)
{
	printf("FATAL: %s (%s)\n", what, name ? name : "?");
	abort();
}

int kprintf(const char *fmt, ...)
{
	char buf[512];
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	if (!strncmp(buf, "WARNING", 7))
		warnings++;
	for (int i = 0; buf[i] && loglen + 1 < (int)sizeof(lastlog); i++)
		lastlog[loglen++] = buf[i];
	lastlog[loglen] = 0;
	return n;
}

static void log_reset(void)
{
	loglen = 0;
	lastlog[0] = 0;
}

void *kalloc(size_t size)
{
	return malloc(size ? size : 1);
}

void *kcalloc(size_t count, size_t size)
{
	return calloc(count ? count : 1, size ? size : 1);
}

void *krealloc(void *ptr, size_t size)
{
	return realloc(ptr, size ? size : 1);
}

void kfree(void *ptr)
{
	free(ptr);
}

static unsigned long delay_us_total;

void lapic_delay_us(uint32_t us)
{
	delay_us_total += us;
}

void lapic_delay_ms(uint32_t ms)
{
	delay_us_total += (unsigned long)ms * 1000;
	/* the fake LSPCON follows a mode change after some time */
	extern void fake_time_passes(unsigned ms);
	fake_time_passes(ms);
}

/* ---- helpers ---------------------------------------------------------- */

static int bytes_are(const u8 *got, const u8 *want, int n)
{
	if (!memcmp(got, want, (unsigned long)n))
		return 1;
	printf("  got: ");
	for (int i = 0; i < n; i++)
		printf("%02x ", got[i]);
	printf("\n  want:");
	for (int i = 0; i < n; i++)
		printf(" %02x", want[i]);
	printf("\n");
	return 0;
}

static int sums_to_zero(const u8 *b, int n)
{
	unsigned s = 0;

	for (int i = 0; i < n; i++)
		s += b[i];
	return (s & 0xff) == 0;
}

static void cea_mode(struct drm_display_mode *m, u8 vic)
{
	const struct drm_display_mode *t = drm_cea_mode_for_vic(vic);

	memset(m, 0, sizeof(*m));
	if (t)
		drm_mode_init(m, t);
}

static void hdmi_mode(struct drm_display_mode *m, u8 vic)
{
	const struct drm_display_mode *t = drm_hdmi_mode_for_vic(vic);

	memset(m, 0, sizeof(*m));
	if (t)
		drm_mode_init(m, t);
}

/* A connector whose display info is what the EDID says. */
static struct drm_connector conns[4];
static struct drm_display_mode probed[64];

static struct drm_connector *conn_from_edid(int slot, const u8 *edid, unsigned len)
{
	struct drm_connector *c = &conns[slot];
	struct drm_edid_conn ec;
	int n;

	memset(&ec, 0, sizeof(ec));
	drm_display_info_reset(&c->display_info);
	memset(c, 0, sizeof(*c));
	ec.connector_type = DRM_MODE_CONNECTOR_HDMIA;
	ec.info = &c->display_info;
	ec.modes = probed;
	ec.max_modes = 64;
	n = drm_edid_parse_full(edid, len, &ec);
	CHECK(n > 0, "EDID parse gave %d", n);
	return c;
}

/* ---- infoframe vectors ---------------------------------------------------- */

static void test_avi_pack(void)
{
	struct hdmi_avi_infoframe f;
	union hdmi_infoframe u;
	u8 buf[HDMI_INFOFRAME_SIZE(AVI) + 3];
	struct drm_display_mode m;
	ssize_t n;
	/* VIC 16, 1920x1080@60 16:9: RGB, active format given, underscan;
	 * picture 16:9, active = picture; VIC 16. */
	static const u8 vic16[17] = { 0x82, 0x02, 0x0d, 0x25, 0x12, 0x28, 0x00, 0x10,
				      0x00, 0, 0, 0, 0, 0, 0, 0, 0 };
	/* the same with Q = limited (it is VIC 16's default range) */
	static const u8 vic16_lim[17] = { 0x82, 0x02, 0x0d, 0x21, 0x12, 0x28, 0x04, 0x10,
					  0x00, 0, 0, 0, 0, 0, 0, 0, 0 };

	cea_mode(&m, 16);
	CHECK(m.hdisplay == 1920 && m.picture_aspect_ratio == DRM_MODE_PICTURE_ASPECT_16_9,
	      "VIC 16 table entry");
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, NULL, &m) == 0, "AVI from VIC 16");
	memset(buf, 0xee, sizeof(buf));
	n = hdmi_avi_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 17, "AVI length %ld", (long)n);
	CHECK(bytes_are(buf, vic16, 17), "AVI VIC 16 bytes");
	CHECK(buf[17] == 0 && buf[19] == 0, "the rest of the buffer is cleared");
	CHECK(sums_to_zero(buf, 17), "AVI checksum");

	/* the mode without its own aspect takes the VIC's */
	m.picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_NONE;
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, NULL, &m) == 0 &&
	      f.picture_aspect == HDMI_PICTURE_ASPECT_16_9 && f.video_code == 16,
	      "aspect from the VIC");

	/* too small a buffer */
	CHECK(hdmi_avi_infoframe_pack(&f, buf, 16) == -ENOSPC, "AVI into 16 bytes");

	/* unpack round trip */
	CHECK(hdmi_infoframe_unpack(&u, vic16, sizeof(vic16)) == 0, "AVI unpack");
	CHECK(u.avi.video_code == 16 && u.avi.picture_aspect == HDMI_PICTURE_ASPECT_16_9 &&
	      u.avi.active_aspect == HDMI_ACTIVE_ASPECT_PICTURE &&
	      u.avi.scan_mode == HDMI_SCAN_MODE_UNDERSCAN, "AVI unpacked fields");
	u8 bad[17];
	memcpy(bad, vic16, 17);
	bad[7] ^= 1;
	CHECK(hdmi_infoframe_unpack(&u, bad, sizeof(bad)) == -EINVAL, "AVI bad checksum refused");
	CHECK(hdmi_infoframe_unpack(&u, vic16, 16) == -EINVAL, "AVI short buffer refused");

	/* quantisation: Q limited is the default range, so it is sent */
	struct drm_connector *dvi14 = conn_from_edid(0, test_edid_hdmi_1080p_rgb_max_200mhz, 256);
	cea_mode(&m, 16);
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, dvi14, &m) == 0, "AVI on 1.4 sink");
	drm_hdmi_avi_infoframe_quant_range(&f, dvi14, &m, HDMI_QUANTIZATION_RANGE_LIMITED);
	n = hdmi_avi_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 17 && bytes_are(buf, vic16_lim, 17), "AVI VIC 16 limited range bytes");
	/* full is not the default, and a sink that does not say it takes Q
	 * (QS=0 in its video capability block) gets Q=0 */
	dvi14->display_info.rgb_quant_range_selectable = false;
	drm_hdmi_avi_infoframe_quant_range(&f, dvi14, &m, HDMI_QUANTIZATION_RANGE_FULL);
	CHECK(f.quantization_range == HDMI_QUANTIZATION_RANGE_DEFAULT &&
	      f.ycc_quantization_range == HDMI_YCC_QUANTIZATION_RANGE_LIMITED,
	      "full range on a QS=0 HDMI 1.4 sink: Q default, YQ limited");
	/* a sink that takes Q and is HDMI 2.0: Q full, YQ full */
	dvi14->display_info.rgb_quant_range_selectable = true;
	dvi14->display_info.hdmi.scdc.supported = true;
	drm_hdmi_avi_infoframe_quant_range(&f, dvi14, &m, HDMI_QUANTIZATION_RANGE_FULL);
	CHECK(f.quantization_range == HDMI_QUANTIZATION_RANGE_FULL &&
	      f.ycc_quantization_range == HDMI_YCC_QUANTIZATION_RANGE_FULL,
	      "full range on an HDMI 2.0 QS=1 sink");
	n = hdmi_avi_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 17 && buf[6] == 0x08 && buf[8] == 0x40 && sums_to_zero(buf, 17),
	      "Q and YQ bits");
	/* VIC 1 (640x480) defaults to full range */
	cea_mode(&m, 1);
	CHECK(drm_default_rgb_quant_range(&m) == HDMI_QUANTIZATION_RANGE_FULL, "VIC 1 full");

	/* double-clocked modes repeat each pixel */
	cea_mode(&m, 6);
	CHECK(m.flags & DRM_MODE_FLAG_DBLCLK, "VIC 6 is double clocked");
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, NULL, &m) == 0 &&
	      f.pixel_repeat == 1 && f.video_code == 6, "VIC 6 pixel repeat");

	/* 64:27 is carried by the VIC alone */
	cea_mode(&m, 79);
	CHECK(m.picture_aspect_ratio == DRM_MODE_PICTURE_ASPECT_64_27, "VIC 79 is 64:27");
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, NULL, &m) == 0 &&
	      f.video_code == 79 && f.picture_aspect == HDMI_PICTURE_ASPECT_NONE,
	      "64:27 via VIC 79");
	/* a 64:27 aspect no code implies cannot be sent */
	m.hdisplay = 1000;
	m.hsync_start = 1100;
	m.hsync_end = 1200;
	m.htotal = 1300;
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, NULL, &m) == -EINVAL,
	      "64:27 without a VIC refused");
	/* the checker refuses what the field cannot hold */
	hdmi_avi_infoframe_init(&f);
	f.picture_aspect = HDMI_PICTURE_ASPECT_64_27;
	CHECK(hdmi_avi_infoframe_pack(&f, buf, sizeof(buf)) == -EINVAL, "AVI aspect 64:27 refused");

	/* bars */
	hdmi_avi_infoframe_init(&f);
	f.top_bar = 0x1234;
	f.right_bar = 0x0102;
	n = hdmi_avi_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 17 && buf[4] == 0x0c && buf[9] == 0x34 && buf[10] == 0x12 &&
	      buf[15] == 0x02 && buf[16] == 0x01, "bar data and its flags");
	CHECK(hdmi_infoframe_unpack(&u, buf, 17) == 0 && u.avi.top_bar == 0x1234 &&
	      u.avi.right_bar == 0x0102 && u.avi.left_bar == 0, "bars unpacked");
}

static void test_avi_vic_ranges(void)
{
	struct hdmi_avi_infoframe f;
	struct drm_display_mode m;
	struct drm_connector *c14 = conn_from_edid(1, test_edid_hdmi_1080p_rgb_max_200mhz, 256);
	struct drm_connector *c20 = conn_from_edid(2, test_edid_hdmi_4k_rgb_yuv420_dc_max_340mhz, 256);
	u8 save;

	CHECK(c14->display_info.is_hdmi && !c14->display_info.has_hdmi_infoframe,
	      "1.4 corpus sink: HDMI, without HDMI_Video_present");
	/* as one with HDMI_Video_present set in its HDMI block */
	c14->display_info.has_hdmi_infoframe = true;
	CHECK(!c14->display_info.hdmi.scdc.supported &&
	      !(c14->display_info.color_formats & DRM_COLOR_FORMAT_YCBCR420),
	      "1.4 corpus sink is not an HDMI 2.0 sink");
	CHECK(c20->display_info.color_formats & DRM_COLOR_FORMAT_YCBCR420 ||
	      c20->display_info.hdmi.scdc.supported, "4k corpus sink is an HDMI 2.0 sink");

	/* VIC 97 (3840x2160@60) is CTA-861-F: not for an HDMI 1.4 sink that
	 * does not list it */
	cea_mode(&m, 97);
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, c14, &m) == 0 && f.video_code == 0,
	      "VIC 97 to a 1.4 sink: %u", f.video_code);
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, c20, &m) == 0 && f.video_code == 97,
	      "VIC 97 to a 2.0 sink: %u", f.video_code);
	/* ... unless the sink lists it */
	u8 vics[2] = { 16, 97 };
	u8 *old = c14->display_info.vics;
	int oldlen = c14->display_info.vics_len;
	c14->display_info.vics = vics;
	c14->display_info.vics_len = 2;
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, c14, &m) == 0 && f.video_code == 97,
	      "VIC 97 listed by a 1.4 sink");
	c14->display_info.vics = old;
	c14->display_info.vics_len = oldlen;

	/* VIC 95 = HDMI VIC 1 (3840x2160@30): the vendor frame carries it */
	hdmi_mode(&m, 1);
	CHECK(m.hdisplay == 3840 && drm_match_hdmi_mode(&m) == 1, "HDMI VIC 1 mode");
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, c14, &m) == 0 &&
	      f.video_code == 0 && f.picture_aspect == HDMI_PICTURE_ASPECT_16_9,
	      "4k30 on an HDMI-block sink: no AVI VIC, aspect 16:9 (vic %u aspect %d)",
	      f.video_code, f.picture_aspect);
	/* ... and without the HDMI block the AVI frame carries VIC 95 */
	save = (u8)c20->display_info.has_hdmi_infoframe;
	c20->display_info.has_hdmi_infoframe = false;
	CHECK(drm_hdmi_avi_infoframe_from_display_mode(&f, c20, &m) == 0 && f.video_code == 95,
	      "4k30 without the HDMI block: AVI VIC %u", f.video_code);
	c20->display_info.has_hdmi_infoframe = save;
}

static void test_vendor(void)
{
	struct hdmi_vendor_infoframe f;
	union hdmi_infoframe u;
	struct drm_display_mode m;
	u8 buf[HDMI_INFOFRAME_SIZE(MAX)];
	ssize_t n;
	struct drm_connector *c14 = conn_from_edid(1, test_edid_hdmi_1080p_rgb_max_200mhz, 256);
	static const u8 hvic1[9] = { 0x81, 0x01, 0x05, 0x49, 0x03, 0x0c, 0x00, 0x20, 0x01 };
	static const u8 empty[8] = { 0x81, 0x01, 0x04, 0x6b, 0x03, 0x0c, 0x00, 0x00 };
	static const u8 sbs_half[10] = { 0x81, 0x01, 0x06, 0xa9, 0x03, 0x0c, 0x00, 0x40, 0x80, 0x00 };
	static const u8 fpack[9] = { 0x81, 0x01, 0x05, 0x2a, 0x03, 0x0c, 0x00, 0x40, 0x00 };

	/* a sink with HDMI_Video_present */
	c14->display_info.has_hdmi_infoframe = true;
	hdmi_mode(&m, 1);
	CHECK(drm_hdmi_vendor_infoframe_from_display_mode(&f, c14, &m) == 0, "vendor 4k30");
	n = hdmi_vendor_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 9 && bytes_are(buf, hvic1, 9), "vendor HDMI VIC 1 bytes");

	cea_mode(&m, 16);
	CHECK(drm_hdmi_vendor_infoframe_from_display_mode(&f, c14, &m) == 0, "vendor 1080p");
	n = hdmi_vendor_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 8 && bytes_are(buf, empty, 8), "empty vendor frame still sent");

	m.flags |= DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF;
	CHECK(drm_hdmi_vendor_infoframe_from_display_mode(&f, c14, &m) == 0 &&
	      f.s3d_struct == HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF && f.vic == 0, "3D SbS half");
	n = hdmi_vendor_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 10 && bytes_are(buf, sbs_half, 10), "vendor SbS half bytes");
	CHECK(hdmi_infoframe_unpack(&u, buf, (size_t)n) == 0 &&
	      u.vendor.hdmi.s3d_struct == HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF &&
	      u.vendor.hdmi.length == 6, "SbS half unpacked");

	m.flags &= ~DRM_MODE_FLAG_3D_MASK;
	m.flags |= DRM_MODE_FLAG_3D_FRAME_PACKING;
	CHECK(drm_hdmi_vendor_infoframe_from_display_mode(&f, c14, &m) == 0, "3D frame packing");
	n = hdmi_vendor_infoframe_pack(&f, buf, sizeof(buf));
	CHECK(n == 9 && bytes_are(buf, fpack, 9), "vendor frame packing bytes");

	/* a 3D 4k mode gets no HDMI VIC (one or the other) */
	hdmi_mode(&m, 1);
	m.flags |= DRM_MODE_FLAG_3D_TOP_AND_BOTTOM;
	CHECK(drm_hdmi_vendor_infoframe_from_display_mode(&f, c14, &m) == 0 &&
	      f.vic == 0 && f.s3d_struct == HDMI_3D_STRUCTURE_TOP_AND_BOTTOM, "3D 4k: no VIC");

	/* no HDMI block, no NULL connector: no frame */
	CHECK(drm_hdmi_vendor_infoframe_from_display_mode(&f, NULL, &m) == -EINVAL, "NULL connector");
	c14->display_info.has_hdmi_infoframe = false;
	CHECK(drm_hdmi_vendor_infoframe_from_display_mode(&f, c14, &m) == -EINVAL, "no HDMI block");
	c14->display_info.has_hdmi_infoframe = true;

	/* both a VIC and a 3D layout: refused */
	hdmi_vendor_infoframe_init(&f);
	f.vic = 1;
	f.s3d_struct = HDMI_3D_STRUCTURE_FRAME_PACKING;
	CHECK(hdmi_vendor_infoframe_pack(&f, buf, sizeof(buf)) == -EINVAL, "VIC and 3D refused");
	/* pack_only does not fix the length up */
	hdmi_vendor_infoframe_init(&f);
	f.vic = 1;
	CHECK(hdmi_vendor_infoframe_pack_only(&f, buf, sizeof(buf)) == -EINVAL,
	      "pack_only with a stale length refused");
	CHECK(hdmi_vendor_infoframe_check(&f) == 0 && f.length == 5, "check fixes the length");

	/* round trip of the HDMI VIC frame, and a corrupted OUI */
	CHECK(hdmi_infoframe_unpack(&u, hvic1, sizeof(hvic1)) == 0 && u.vendor.hdmi.vic == 1 &&
	      u.vendor.hdmi.s3d_struct == HDMI_3D_STRUCTURE_INVALID, "HDMI VIC unpacked");
	u8 bad[9];
	memcpy(bad, hvic1, 9);
	bad[5] = 0x0d;
	bad[3] = (u8)(bad[3] - 1);
	CHECK(sums_to_zero(bad, 9) && hdmi_infoframe_unpack(&u, bad, 9) == -EINVAL,
	      "wrong OUI refused");

	/* the generic packer: a vendor frame of another OUI is not known */
	memset(&u, 0, sizeof(u));
	hdmi_vendor_infoframe_init(&u.vendor.hdmi);
	u.vendor.any.oui = HDMI_FORUM_IEEE_OUI;
	CHECK(hdmi_infoframe_pack(&u, buf, sizeof(buf)) == -EINVAL, "HDMI Forum OUI refused");
}

static void test_spd_audio_drm(void)
{
	struct hdmi_spd_infoframe spd;
	struct hdmi_audio_infoframe au;
	struct hdmi_drm_infoframe dr;
	union hdmi_infoframe u;
	u8 buf[HDMI_INFOFRAME_SIZE(MAX)];
	u8 pl[HDMI_AUDIO_INFOFRAME_SIZE];
	ssize_t n;
	static const u8 audio[14] = { 0x84, 0x01, 0x0a, 0x53, 0x11, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0 };
	static const u8 drm_payload[26] = {
		0x02, 0x00, 0x48, 0x8a, 0x08, 0x39, 0x34, 0x21, 0xaa, 0x9b, 0x96, 0x19, 0xfc,
		0x08, 0x13, 0x3d, 0x42, 0x40, 0xe8, 0x03, 0x32, 0x00, 0xe8, 0x03, 0x90, 0x01,
	};

	/* SPD: the strings truncated to their fields */
	hdmi_spd_infoframe_init(&spd, "Intel Corporation", "Integrated gfx");
	spd.sdi = HDMI_SPD_SDI_PC;
	n = hdmi_spd_infoframe_pack(&spd, buf, sizeof(buf));
	CHECK(n == 29 && buf[0] == 0x83 && buf[1] == 1 && buf[2] == 25 && sums_to_zero(buf, 29),
	      "SPD header and checksum");
	CHECK(!memcmp(buf + 4, "Intel Co", 8) && !memcmp(buf + 12, "Integrated gfx\0\0", 16) &&
	      buf[28] == 9, "SPD payload");
	CHECK(hdmi_infoframe_unpack(&u, buf, (size_t)n) == 0 && !memcmp(u.spd.vendor, "Intel Co", 8) &&
	      !memcmp(u.spd.product, "Integrated gfx", 14) && u.spd.product[14] == 0 &&
	      u.spd.sdi == HDMI_SPD_SDI_PC, "SPD unpacked");

	/* audio: 2 channels, PCM, 48 kHz, 16 bit */
	hdmi_audio_infoframe_init(&au);
	au.channels = 2;
	au.coding_type = HDMI_AUDIO_CODING_TYPE_PCM;
	au.sample_frequency = HDMI_AUDIO_SAMPLE_FREQUENCY_48000;
	au.sample_size = HDMI_AUDIO_SAMPLE_SIZE_16;
	n = hdmi_audio_infoframe_pack(&au, buf, sizeof(buf));
	CHECK(n == 14 && bytes_are(buf, audio, 14), "audio bytes");
	CHECK(hdmi_audio_infoframe_pack_payload_only(&au, pl, sizeof(pl)) == 0 &&
	      bytes_are(pl, audio + 4, HDMI_AUDIO_INFOFRAME_SIZE), "audio payload alone");
	struct dp_sdp sdp;
	memset(&sdp, 0xee, sizeof(sdp));
	CHECK(hdmi_audio_infoframe_pack_for_dp(&au, &sdp, 0x13) == 14 &&
	      sdp.sdp_header.HB0 == 0 && sdp.sdp_header.HB1 == 0x84 &&
	      sdp.sdp_header.HB2 == 0x1b && sdp.sdp_header.HB3 == (0x13 << 2) &&
	      bytes_are(sdp.db, audio + 4, 5) && sdp.db[5] == 0 && sdp.db[31] == 0,
	      "audio infoframe as a DisplayPort SDP");
	au.downmix_inhibit = true;
	au.level_shift_value = 5;
	au.channel_allocation = 0x13;
	n = hdmi_audio_infoframe_pack(&au, buf, sizeof(buf));
	CHECK(n == 14 && buf[7] == 0x13 && buf[8] == (0x80 | (5 << 3)) && sums_to_zero(buf, 14),
	      "audio byte 4 and 5");
	/* unpack keeps the raw channel field (count - 1) */
	CHECK(hdmi_infoframe_unpack(&u, buf, 14) == 0 && u.audio.channels == 1 &&
	      u.audio.downmix_inhibit && u.audio.level_shift_value == 5, "audio unpacked");

	/* Dynamic Range and Mastering: BT.2020 primaries, D65, PQ */
	hdmi_drm_infoframe_init(&dr);
	dr.eotf = HDMI_EOTF_SMPTE_ST2084;
	dr.metadata_type = HDMI_STATIC_METADATA_TYPE1;
	dr.display_primaries[0].x = 35400;
	dr.display_primaries[0].y = 14600;
	dr.display_primaries[1].x = 8500;
	dr.display_primaries[1].y = 39850;
	dr.display_primaries[2].x = 6550;
	dr.display_primaries[2].y = 2300;
	dr.white_point.x = 15635;
	dr.white_point.y = 16450;
	dr.max_display_mastering_luminance = 1000;
	dr.min_display_mastering_luminance = 50;
	dr.max_cll = 1000;
	dr.max_fall = 400;
	n = hdmi_drm_infoframe_pack(&dr, buf, sizeof(buf));
	CHECK(n == 30 && buf[0] == 0x87 && buf[1] == 1 && buf[2] == 26 && sums_to_zero(buf, 30),
	      "DRM header and checksum");
	CHECK(bytes_are(buf + 4, drm_payload, 26), "DRM payload");
	CHECK(hdmi_infoframe_unpack(&u, buf, (size_t)n) == 0 &&
	      !memcmp(&u.drm, &dr, sizeof(dr)), "DRM round trip");
	CHECK(hdmi_drm_infoframe_unpack_only(&u.drm, drm_payload, 25) == -EINVAL,
	      "DRM data bytes short");

	/* the generic packer refuses an unknown type, with a warning */
	memset(&u, 0, sizeof(u));
	u.any.type = (enum hdmi_infoframe_type)0x85;
	warnings = 0;
	CHECK(hdmi_infoframe_pack(&u, buf, sizeof(buf)) == -EINVAL && warnings == 1,
	      "unknown type refused");
	CHECK(hdmi_infoframe_pack_only(&u, buf, sizeof(buf)) == -EINVAL, "unknown type (only)");
	u8 junk[4] = { 0x85, 1, 0, 0 };
	CHECK(hdmi_infoframe_unpack(&u, junk, 4) == -EINVAL, "unknown type unpack");
}

static void test_log(void)
{
	union hdmi_infoframe u;
	struct drm_display_mode m;

	cea_mode(&m, 16);
	drm_hdmi_avi_infoframe_from_display_mode(&u.avi, NULL, &m);
	log_reset();
	hdmi_infoframe_log("<7>", "i915", &u);
	CHECK(strstr(lastlog, "<7>i915: HDMI infoframe: Auxiliary Video Information (AVI), version 2, length 13\n") != NULL,
	      "AVI log header: %s", lastlog);
	CHECK(strstr(lastlog, "video code: 16\n") && strstr(lastlog, "picture aspect: 16:9\n") &&
	      strstr(lastlog, "scan mode: Underscan\n"), "AVI log fields");

	hdmi_spd_infoframe_init(&u.spd, "LikeOS", "a product name, too long");
	log_reset();
	hdmi_infoframe_log(NULL, NULL, &u);
	CHECK(strstr(lastlog, "HDMI infoframe: Source Product Description (SPD)") == lastlog &&
	      strstr(lastlog, "vendor: LikeOS\n") && strstr(lastlog, "product: a product name, \n"),
	      "SPD log: %s", lastlog);

	hdmi_vendor_infoframe_init(&u.vendor.hdmi);
	u.vendor.hdmi.s3d_struct = HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF;
	log_reset();
	hdmi_infoframe_log("", NULL, &u);
	CHECK(strstr(lastlog, "3D structure: Side-by-side (Half)\n") &&
	      strstr(lastlog, "3D extension data: 0\n"), "vendor log: %s", lastlog);
}

/* ---- HDMI helpers --------------------------------------------------------- */

static void test_helpers(void)
{
	struct drm_connector *c = conn_from_edid(3, test_edid_hdmi_1080p_rgb_max_200mhz_hdr, 256);
	struct drm_connector_state st;
	struct hdmi_avi_infoframe f;
	struct hdmi_drm_infoframe dr;
	struct hdr_output_metadata md;
	struct drm_blob blob;
	u8 buf[HDMI_INFOFRAME_SIZE(MAX)];

	memset(&st, 0, sizeof(st));
	st.conn = c;
	hdmi_avi_infoframe_init(&f);

	st.colorspace = DRM_MODE_COLORIMETRY_BT2020_RGB;
	drm_hdmi_avi_infoframe_colorimetry(&f, &st);
	CHECK(f.colorimetry == HDMI_COLORIMETRY_EXTENDED &&
	      f.extended_colorimetry == HDMI_EXTENDED_COLORIMETRY_BT2020, "BT2020 RGB");
	st.colorspace = DRM_MODE_COLORIMETRY_BT709_YCC;
	drm_hdmi_avi_infoframe_colorimetry(&f, &st);
	CHECK(f.colorimetry == HDMI_COLORIMETRY_ITU_709 && f.extended_colorimetry == 0, "BT709");
	st.colorspace = DRM_MODE_COLORIMETRY_OPRGB;
	drm_hdmi_avi_infoframe_colorimetry(&f, &st);
	CHECK(f.colorimetry == HDMI_COLORIMETRY_EXTENDED &&
	      f.extended_colorimetry == HDMI_EXTENDED_COLORIMETRY_OPRGB, "opRGB");
	st.colorspace = DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65;
	drm_hdmi_avi_infoframe_colorimetry(&f, &st);
	CHECK(f.colorimetry == HDMI_COLORIMETRY_NONE && f.extended_colorimetry == 0,
	      "DCI-P3 has no AVI encoding");

	st.content_type = DRM_MODE_CONTENT_TYPE_GAME;
	drm_hdmi_avi_infoframe_content_type(&f, &st);
	CHECK(f.content_type == HDMI_CONTENT_TYPE_GAME && f.itc, "content type game");
	st.content_type = DRM_MODE_CONTENT_TYPE_NO_DATA;
	drm_hdmi_avi_infoframe_content_type(&f, &st);
	CHECK(f.content_type == HDMI_CONTENT_TYPE_GRAPHICS && !f.itc, "content type none");

	f.top_bar = 7;
	drm_hdmi_avi_infoframe_bars(&f, &st);
	CHECK(f.top_bar == 0 && f.left_bar == 0, "no bars");

	/* HDR metadata */
	CHECK(drm_hdmi_infoframe_set_hdr_metadata(&dr, &st) == -EINVAL, "no HDR blob");
	memset(&md, 0, sizeof(md));
	md.metadata_type = HDMI_STATIC_METADATA_TYPE1;
	md.hdmi_metadata_type1.eotf = HDMI_EOTF_SMPTE_ST2084;
	md.hdmi_metadata_type1.display_primaries[1].y = 39850;
	md.hdmi_metadata_type1.white_point.x = 15635;
	md.hdmi_metadata_type1.max_display_mastering_luminance = 1000;
	md.hdmi_metadata_type1.min_display_mastering_luminance = 50;
	md.hdmi_metadata_type1.max_cll = 900;
	md.hdmi_metadata_type1.max_fall = 300;
	memset(&blob, 0, sizeof(blob));
	blob.data = &md;
	blob.length = sizeof(md);
	st.hdr_output_metadata = &blob;
	CHECK(drm_hdmi_infoframe_set_hdr_metadata(&dr, &st) == 0, "HDR metadata");
	CHECK(dr.eotf == HDMI_EOTF_SMPTE_ST2084 && dr.display_primaries[1].y == 39850 &&
	      dr.white_point.x == 15635 && dr.max_display_mastering_luminance == 1000 &&
	      dr.min_display_mastering_luminance == 50 && dr.max_cll == 900 && dr.max_fall == 300,
	      "HDR fields");
	CHECK(hdmi_drm_infoframe_pack(&dr, buf, sizeof(buf)) == 30 && sums_to_zero(buf, 30),
	      "HDR frame packs");
	blob.length = 4;
	CHECK(drm_hdmi_infoframe_set_hdr_metadata(&dr, &st) == -EINVAL, "short HDR blob");
}

/* The TMDS character rate, as the connector unit tests check it. */
static void test_mode_clock(void)
{
	struct drm_display_mode m;
	static const u8 yuv420_vics[] = { 96, 97, 101, 102, 106, 107 };

	cea_mode(&m, 16);
	CHECK(!(m.flags & DRM_MODE_FLAG_DBLCLK), "VIC 16 single clocked");
	CHECK(drm_hdmi_compute_mode_clock(&m, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444) ==
	      m.clock * 1000ULL, "RGB 8 bpc");
	CHECK(drm_hdmi_compute_mode_clock(&m, 10, DRM_OUTPUT_COLOR_FORMAT_RGB444) ==
	      m.clock * 1250ULL, "RGB 10 bpc");
	CHECK(drm_hdmi_compute_mode_clock(&m, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444) ==
	      m.clock * 1500ULL, "RGB 12 bpc");
	for (int bpc = 8; bpc <= 12; bpc += 2)
		CHECK(drm_hdmi_compute_mode_clock(&m, (unsigned)bpc, DRM_OUTPUT_COLOR_FORMAT_YCBCR422) ==
		      m.clock * 1000ULL, "4:2:2 %d bpc", bpc);
	CHECK(drm_hdmi_compute_mode_clock(&m, 16, DRM_OUTPUT_COLOR_FORMAT_YCBCR422) == 0,
	      "4:2:2 16 bpc refused");

	cea_mode(&m, 1);
	CHECK(drm_hdmi_compute_mode_clock(&m, 10, DRM_OUTPUT_COLOR_FORMAT_RGB444) == 0, "VIC 1 10 bpc");
	CHECK(drm_hdmi_compute_mode_clock(&m, 12, DRM_OUTPUT_COLOR_FORMAT_RGB444) == 0, "VIC 1 12 bpc");
	CHECK(drm_hdmi_compute_mode_clock(&m, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444) == 25175000ULL,
	      "VIC 1 8 bpc");

	cea_mode(&m, 6);
	CHECK(drm_hdmi_compute_mode_clock(&m, 8, DRM_OUTPUT_COLOR_FORMAT_RGB444) ==
	      m.clock * 1000ULL * 2, "double clocked");

	for (unsigned i = 0; i < sizeof(yuv420_vics); i++) {
		cea_mode(&m, yuv420_vics[i]);
		CHECK(m.clock && !(m.flags & DRM_MODE_FLAG_DBLCLK) &&
		      drm_hdmi_compute_mode_clock(&m, 8, DRM_OUTPUT_COLOR_FORMAT_YCBCR420) ==
		      m.clock * 1000ULL / 2, "4:2:0 VIC %u", yuv420_vics[i]);
	}
	cea_mode(&m, 96);
	CHECK(drm_hdmi_compute_mode_clock(&m, 10, DRM_OUTPUT_COLOR_FORMAT_YCBCR420) ==
	      m.clock * 625ULL, "4:2:0 10 bpc");
	CHECK(drm_hdmi_compute_mode_clock(&m, 12, DRM_OUTPUT_COLOR_FORMAT_YCBCR420) ==
	      m.clock * 750ULL, "4:2:0 12 bpc");
}

static void test_acr(void)
{
	unsigned n, cts;

	drm_hdmi_acr_get_n_cts(148500000ULL, 48000, &n, &cts);
	CHECK(n == 6144 && cts == 148500, "148.5 MHz 48k: %u %u", n, cts);
	drm_hdmi_acr_get_n_cts(148500000ULL, 44100, &n, &cts);
	CHECK(n == 6272 && cts == 165000, "148.5 MHz 44.1k: %u %u", n, cts);
	drm_hdmi_acr_get_n_cts(148500000ULL, 192000, &n, &cts);
	CHECK(n == 6144 * 4 && cts == 148500, "192k takes the 48k entry: %u %u", n, cts);
	drm_hdmi_acr_get_n_cts(25175000ULL, 48000, &n, &cts);
	CHECK(n == 6864 && cts == 28125, "25.175 MHz 48k: %u %u", n, cts);
	drm_hdmi_acr_get_n_cts(74175824ULL, 32000, &n, &cts);
	CHECK(n == 11648 && cts == 210937, "74.176 MHz 32k: %u %u", n, cts);
	drm_hdmi_acr_get_n_cts(100000000ULL, 48000, &n, &cts);
	CHECK(n == 6144 && cts == 100000, "other clock 48k: %u %u", n, cts);
	drm_hdmi_acr_get_n_cts(148500000ULL, 22050, &n, &cts);
	CHECK(n == 2822 && cts == 148479, "other sample rate: %u %u", n, cts);
	drm_hdmi_acr_get_n_cts(100000000ULL, 0, &n, &cts);
	CHECK(n == 0 && cts == 0, "no sample rate: %u %u", n, cts);
}

/* ---- a fake I2C bus: an SCDC sink at 0x54, a dual-mode adaptor at 0x40 ---- */

struct fake_dev {
	u8 regs[256];
	u8 ptr; /* the register pointer, set by a write's first byte */
	bool present;
	bool sub_addressing; /* a read starts at ptr (else always at 0) */
	bool ignore_writes; /* drops every write after the offset */
};

static struct fake_dev scdc_dev, dual_dev;
static int bus_fail; /* return this from the next transfer (negative) */
static int bus_short; /* complete only this many messages of the next one */
static int nxfers;
static struct i2c_msg last_msgs[4];
static int last_n;
static int lspcon_settle_ms = -1; /* -1: mode changes never take */
static int lspcon_pending;

void fake_time_passes(unsigned ms)
{
	if (lspcon_settle_ms < 0 || !lspcon_pending)
		return;
	lspcon_settle_ms -= (int)ms;
	if (lspcon_settle_ms <= 0) {
		dual_dev.regs[DP_DUAL_MODE_LSPCON_CURRENT_MODE] =
			dual_dev.regs[DP_DUAL_MODE_LSPCON_MODE_CHANGE];
		lspcon_pending = 0;
	}
}

static int fake_xfer(struct i2c_adapter *a, struct i2c_msg *msgs, int n)
{
	(void)a;
	nxfers++;
	last_n = n;
	for (int i = 0; i < n && i < 4; i++)
		last_msgs[i] = msgs[i];
	if (bus_fail) {
		int r = bus_fail;

		bus_fail = 0;
		return r;
	}
	for (int i = 0; i < n; i++) {
		struct fake_dev *d = msgs[i].addr == 0x54 ? &scdc_dev :
				     msgs[i].addr == 0x40 ? &dual_dev : NULL;

		if (bus_short && i == bus_short) {
			bus_short = 0;
			return i;
		}
		if (!d || !d->present)
			return i ? i : -6; /* NAK: -ENXIO */
		if (msgs[i].flags & I2C_M_RD) {
			u8 p = d->sub_addressing ? d->ptr : 0;

			for (int k = 0; k < msgs[i].len; k++)
				msgs[i].buf[k] = d->regs[(u8)(p + k)];
			d->ptr = (u8)(p + msgs[i].len);
		} else {
			if (msgs[i].len < 1)
				continue;
			d->ptr = msgs[i].buf[0];
			if (d->ignore_writes)
				continue;
			for (int k = 1; k < msgs[i].len; k++)
				d->regs[(u8)(d->ptr + k - 1)] = msgs[i].buf[k];
			if (d == &dual_dev && d->ptr == DP_DUAL_MODE_LSPCON_MODE_CHANGE &&
			    msgs[i].len > 1)
				lspcon_pending = 1;
		}
	}
	return n;
}

static struct i2c_adapter bus = { .xfer = fake_xfer, .name = "fake ddc" };

static void test_scdc(void)
{
	u8 v = 0, id[SCDC_DEVICE_ID_SIZE];

	memset(&scdc_dev, 0, sizeof(scdc_dev));
	scdc_dev.present = true;
	scdc_dev.sub_addressing = true;
	scdc_dev.regs[SCDC_SINK_VERSION] = 1;
	memcpy(&scdc_dev.regs[SCDC_DEVICE_ID], "LIKEOSTV", 8);
	scdc_dev.regs[SCDC_TMDS_CONFIG] = SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

	nxfers = 0;
	CHECK(drm_scdc_readb(&bus, SCDC_SINK_VERSION, &v) == 0 && v == 1, "sink version");
	CHECK(nxfers == 1 && last_n == 2 && last_msgs[0].addr == 0x54 &&
	      last_msgs[0].flags == 0 && last_msgs[0].len == 1 &&
	      (last_msgs[1].flags & I2C_M_RD) && last_msgs[1].len == 1,
	      "an SCDC read is offset write + read");
	CHECK(drm_scdc_read(&bus, SCDC_DEVICE_ID, id, sizeof(id)) == 0 &&
	      !memcmp(id, "LIKEOSTV", 8), "device ID block");

	/* scrambling keeps the clock ratio bit */
	CHECK(drm_scdc_set_scrambling(&bus, true), "scrambling on");
	CHECK(scdc_dev.regs[SCDC_TMDS_CONFIG] == (SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 | SCDC_SCRAMBLING_ENABLE),
	      "TMDS config %02x", scdc_dev.regs[SCDC_TMDS_CONFIG]);
	CHECK(last_n == 1 && last_msgs[0].len == 2, "a write is one message");
	CHECK(drm_scdc_set_high_tmds_clock_ratio(&bus, false), "ratio 1/10");
	CHECK(scdc_dev.regs[SCDC_TMDS_CONFIG] == SCDC_SCRAMBLING_ENABLE, "ratio bit cleared");
	delay_us_total = 0;
	CHECK(drm_scdc_set_high_tmds_clock_ratio(&bus, true) && delay_us_total >= 1000,
	      "ratio 1/40 then the sink's 1 ms");
	CHECK(drm_scdc_set_scrambling(&bus, false) &&
	      scdc_dev.regs[SCDC_TMDS_CONFIG] == SCDC_TMDS_BIT_CLOCK_RATIO_BY_40, "scrambling off");

	scdc_dev.regs[SCDC_SCRAMBLER_STATUS] = SCDC_SCRAMBLING_STATUS;
	CHECK(drm_scdc_get_scrambling_status(&bus), "scrambler locked");
	scdc_dev.regs[SCDC_SCRAMBLER_STATUS] = 0;
	CHECK(!drm_scdc_get_scrambling_status(&bus), "scrambler not locked");

	/* failures */
	bus_fail = -5;
	CHECK(drm_scdc_readb(&bus, SCDC_SINK_VERSION, &v) == -5, "bus error passed up");
	bus_short = 1;
	CHECK(drm_scdc_readb(&bus, SCDC_SINK_VERSION, &v) == -EPROTO, "short transfer -EPROTO");
	bus_fail = -5;
	CHECK(!drm_scdc_set_scrambling(&bus, true), "scrambling fails on a dead bus");
	bus_fail = -5;
	CHECK(!drm_scdc_get_scrambling_status(&bus), "status false on a dead bus");
	scdc_dev.present = false;
	CHECK(!drm_scdc_set_high_tmds_clock_ratio(&bus, true), "no sink (DVI)");
	CHECK(drm_scdc_writeb(NULL, SCDC_TMDS_CONFIG, 0) < 0, "no adapter");
	u8 blk[3] = { 1, 2, 3 };
	scdc_dev.present = true;
	CHECK(drm_scdc_write(&bus, SCDC_CONFIG_0, blk, 3) == 0 &&
	      !memcmp(&scdc_dev.regs[SCDC_CONFIG_0], blk, 3), "block write");
}

static const char hdmi_id[16] = "DP-HDMI ADAPTOR\x04";

static void dual_reset(u8 adaptor_id, bool hdmi)
{
	memset(&dual_dev, 0, sizeof(dual_dev));
	dual_dev.present = true;
	if (hdmi)
		memcpy(dual_dev.regs, hdmi_id, 16);
	else
		memcpy(dual_dev.regs, "DP-DVI ADAPTOR  ", 16);
	dual_dev.regs[DP_DUAL_MODE_ADAPTOR_ID] = adaptor_id;
}

static void test_dual_mode(void)
{
	u8 v[3];
	bool on;
	enum drm_lspcon_mode lm;

	/* reads start at 0 whatever the adaptor does with the offset */
	dual_reset(0xa0, true);
	dual_dev.regs[DP_DUAL_MODE_IEEE_OUI] = 0x00;
	dual_dev.regs[DP_DUAL_MODE_IEEE_OUI + 1] = 0x1c;
	dual_dev.regs[DP_DUAL_MODE_IEEE_OUI + 2] = 0xf8;
	CHECK(drm_dp_dual_mode_read(&bus, DP_DUAL_MODE_IEEE_OUI, v, 3) == 0 &&
	      v[0] == 0x00 && v[1] == 0x1c && v[2] == 0xf8, "OUI read");
	CHECK(last_n == 2 && last_msgs[0].addr == 0x40 && last_msgs[0].buf[0] == 0 &&
	      last_msgs[1].len == DP_DUAL_MODE_IEEE_OUI + 3, "read from offset 0, %u bytes",
	      last_msgs[1].len);
	dual_dev.sub_addressing = true;
	CHECK(drm_dp_dual_mode_read(&bus, DP_DUAL_MODE_IEEE_OUI, v, 3) == 0 && v[1] == 0x1c,
	      "OUI read on a sub-addressing adaptor");

	CHECK(drm_dp_dual_mode_detect(NULL, &bus) == DRM_DP_DUAL_MODE_TYPE2_HDMI, "type 2 HDMI");
	dual_reset(0xa0, false);
	CHECK(drm_dp_dual_mode_detect(NULL, &bus) == DRM_DP_DUAL_MODE_TYPE2_DVI, "type 2 DVI");
	dual_reset(0xa8, true);
	CHECK(drm_dp_dual_mode_detect(NULL, &bus) == DRM_DP_DUAL_MODE_LSPCON, "LSPCON");
	dual_reset(0x00, true);
	CHECK(drm_dp_dual_mode_detect(NULL, &bus) == DRM_DP_DUAL_MODE_TYPE1_HDMI, "type 1 HDMI");
	dual_reset(0xff, false);
	CHECK(drm_dp_dual_mode_detect(NULL, &bus) == DRM_DP_DUAL_MODE_TYPE1_DVI, "type 1 DVI");
	dual_reset(0x55, true);
	log_reset();
	CHECK(drm_dp_dual_mode_detect(NULL, &bus) == DRM_DP_DUAL_MODE_TYPE1_HDMI &&
	      strstr(lastlog, "unexpected adaptor ID 55"), "odd ID: type 1 and a message");
	dual_dev.present = false;
	CHECK(drm_dp_dual_mode_detect(NULL, &bus) == DRM_DP_DUAL_MODE_UNKNOWN, "no answer");

	/* TMDS clock limits */
	dual_reset(0xa0, true);
	dual_dev.regs[DP_DUAL_MODE_MAX_TMDS_CLOCK] = 120;
	CHECK(drm_dp_dual_mode_max_tmds_clock(NULL, DRM_DP_DUAL_MODE_TYPE2_HDMI, &bus) == 300000,
	      "type 2 limit 300 MHz");
	dual_dev.regs[DP_DUAL_MODE_MAX_TMDS_CLOCK] = 0xff;
	CHECK(drm_dp_dual_mode_max_tmds_clock(NULL, DRM_DP_DUAL_MODE_TYPE2_HDMI, &bus) == 165000,
	      "0xff reads as 165 MHz");
	nxfers = 0;
	CHECK(drm_dp_dual_mode_max_tmds_clock(NULL, DRM_DP_DUAL_MODE_TYPE1_DVI, &bus) == 165000 &&
	      nxfers == 0, "type 1: 165 MHz, no read");
	CHECK(drm_dp_dual_mode_max_tmds_clock(NULL, DRM_DP_DUAL_MODE_NONE, &bus) == 0, "native");

	/* TMDS output buffers */
	CHECK(drm_dp_dual_mode_set_tmds_output(NULL, DRM_DP_DUAL_MODE_TYPE2_HDMI, &bus, false) == 0 &&
	      dual_dev.regs[DP_DUAL_MODE_TMDS_OEN] == DP_DUAL_MODE_TMDS_DISABLE, "buffers off");
	CHECK(drm_dp_dual_mode_get_tmds_output(NULL, DRM_DP_DUAL_MODE_TYPE2_HDMI, &bus, &on) == 0 &&
	      !on, "buffers read off");
	CHECK(drm_dp_dual_mode_set_tmds_output(NULL, DRM_DP_DUAL_MODE_LSPCON, &bus, true) == 0 &&
	      dual_dev.regs[DP_DUAL_MODE_TMDS_OEN] == 0, "buffers on");
	dual_dev.ignore_writes = true;
	dual_dev.regs[DP_DUAL_MODE_TMDS_OEN] = 0;
	nxfers = 0;
	CHECK(drm_dp_dual_mode_set_tmds_output(NULL, DRM_DP_DUAL_MODE_TYPE2_HDMI, &bus, false) == -EIO &&
	      nxfers == 6, "three tries then -EIO (%d transfers)", nxfers);
	nxfers = 0;
	on = false;
	CHECK(drm_dp_dual_mode_set_tmds_output(NULL, DRM_DP_DUAL_MODE_TYPE1_HDMI, &bus, false) == 0 &&
	      drm_dp_dual_mode_get_tmds_output(NULL, DRM_DP_DUAL_MODE_TYPE1_HDMI, &bus, &on) == 0 &&
	      on && nxfers == 0, "type 1: nothing written, always on");

	CHECK(!strcmp(drm_dp_get_dual_mode_type_name(DRM_DP_DUAL_MODE_LSPCON), "lspcon") &&
	      !strcmp(drm_dp_get_dual_mode_type_name(DRM_DP_DUAL_MODE_UNKNOWN), "unknown"), "names");

	/* LSPCON mode switch */
	dual_reset(0xa8, true);
	CHECK(drm_lspcon_get_mode(NULL, &bus, &lm) == 0 && lm == DRM_LSPCON_MODE_LS, "LS");
	CHECK(drm_lspcon_get_mode(NULL, &bus, NULL) == -EINVAL, "NULL mode");
	lspcon_settle_ms = 30;
	CHECK(drm_lspcon_set_mode(NULL, &bus, DRM_LSPCON_MODE_PCON, 100) == 0 &&
	      drm_lspcon_get_mode(NULL, &bus, &lm) == 0 && lm == DRM_LSPCON_MODE_PCON,
	      "switched to PCON");
	lspcon_settle_ms = -1;
	delay_us_total = 0;
	CHECK(drm_lspcon_set_mode(NULL, &bus, DRM_LSPCON_MODE_LS, 25) == -ETIMEDOUT &&
	      delay_us_total == 30000, "a 25 ms time-out ends (%lu us)", delay_us_total);
	dual_dev.present = false;
	CHECK(drm_lspcon_get_mode(NULL, &bus, &lm) == -EFAULT, "LSPCON gone");
}

int main(void)
{
	test_avi_pack();
	test_avi_vic_ranges();
	test_vendor();
	test_spd_audio_drm();
	test_log();
	test_helpers();
	test_mode_clock();
	test_acr();
	test_scdc();
	test_dual_mode();

	printf("%d checks, %d failed, %d warnings logged\n", checks, fails, warnings);
	return fails ? 1 : 0;
}
