// Host test: HDMI AVI infoframe packing and CEA video codes.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/i915/intel_infoframe.h>
#include <stdio.h>
#include <string.h>

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

	if (fails) {
		printf("hdmi: %d failures\n", fails);
		return 1;
	}
	printf("hdmi: all tests passed\n");
	return 0;
}
