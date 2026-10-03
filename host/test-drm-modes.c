/*
 * Tests for the display-mode library (kernel/dev/gpu/drm/drm_modes.c):
 * the standard tables, CVT / GTF / analog television timings, matching,
 * validation, sorting and the client-form wrappers -- the latter checked
 * against the previous implementation (test-drm-modes-old.inc).
 * See test-drm-modes.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

/* The kernel's fixed-width types come first and stand in for stdint.h. */
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <stdio.h>
#include <string.h>

#include "test-drm-modes-old.inc"

static int fails, checks;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			fails++;                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static int iabs_host(int v)
{
	return v < 0 ? -v : v;
}

static int umode_same(const struct drm_mode_modeinfo *a,
		      const struct drm_mode_modeinfo *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* Equal up to the name's terminator (the new code zero-fills the rest of
 * the name; the old one left what was there). */
static int umode_same_name(const struct drm_mode_modeinfo *a,
			   const struct drm_mode_modeinfo *b)
{
	struct drm_mode_modeinfo x = *a, y = *b;

	if (strcmp(a->name, b->name))
		return 0;
	memset(x.name, 0, sizeof(x.name));
	memset(y.name, 0, sizeof(y.name));
	return umode_same(&x, &y);
}

static void print_umode(const char *what, const struct drm_mode_modeinfo *m)
{
	printf("  %s: %u %u %u %u %u  %u %u %u %u  flags %x vrefresh %u '%s'\n",
	       what, m->clock, m->hdisplay, m->hsync_start, m->hsync_end,
	       m->htotal, m->vdisplay, m->vsync_start, m->vsync_end, m->vtotal,
	       m->flags, m->vrefresh, m->name);
}

/* ---- the client-form wrappers against the old code ---------------------- */

static void test_wrappers(void)
{
	struct drm_mode_modeinfo a, b;
	int i, n, j;

	/* the standard list: the same modes, byte for byte, except two
	 * corrected entries: 1920x1440@60 has DMT's vertical sync (1441-1444,
	 * the old entry had 1443-1447), and the old 2560x1440 "RB" at 268.5
	 * MHz (66.6 Hz, the 2560x1600 clock on the 1440-line timing) is gone,
	 * the 241.5 MHz entry after it being the real CVT-RB one */
	CHECK(drm_mode_table_count() == old_mode_table_count() - 1, "table count %d vs %d",
	      drm_mode_table_count(), old_mode_table_count());
	n = drm_mode_table_count();
	for (i = 0; i < n; i++) {
		int o = i < 37 ? i : i + 1;

		drm_mode_table_get(i, &a);
		old_mode_table_get(o, &b);
		if (i == 36) {
			CHECK(a.vsync_start == 1441 && a.vsync_end == 1444 &&
			      b.vsync_start == 1443 && b.vsync_end == 1447, "1920x1440 sync");
			b.vsync_start = a.vsync_start;
			b.vsync_end = a.vsync_end;
		}
		CHECK(umode_same(&a, &b), "table entry %d differs", i);
	}
	old_mode_table_get(37, &b);
	CHECK(b.clock == 268500 && b.vdisplay == 1440 && b.vrefresh == 67, "the dropped entry");
	CHECK(drm_mode_table_get(n, &a) == -1, "past the end");

	/* the CEA lookup: every code the old table had, identical; any
	 * other code still unknown */
	for (i = 0; i < 256; i++) {
		int ra = drm_mode_cea_vic((uint8_t)i, &a);
		int rb = old_mode_cea_vic((uint8_t)i, &b);

		CHECK(ra == rb, "vic %d: %d vs %d", i, ra, rb);
		if (ra == 0 && rb == 0 && i == 101) {
			/* 4096x2160@50: the old entry had the 3840-wide
			 * sync positions (4896/4984); CTA-861 says 5064/5152 */
			CHECK(a.hsync_start == 5064 && a.hsync_end == 5152 &&
			      b.hsync_start == 4896, "vic 101");
			b.hsync_start = a.hsync_start;
			b.hsync_end = a.hsync_end;
		}
		if (ra == 0 && rb == 0) {
			CHECK(umode_same(&a, &b), "vic %d differs", i);
			if (!umode_same(&a, &b)) {
				print_umode("new", &a);
				print_umode("old", &b);
			}
		}
	}

	/* drm_mode_finish on every table and CEA mode, interlaced too, and
	 * on a mode without totals (vrefresh kept) */
	for (i = 0; i < n; i++) {
		drm_mode_table_get(i, &a);
		a.flags ^= DRM_MODE_FLAG_INTERLACE;
		memset(a.name, 0x55, sizeof(a.name));
		b = a;
		drm_mode_finish(&a);
		old_mode_finish(&b);
		CHECK(umode_same_name(&a, &b), "finish of entry %d differs", i);
	}
	memset(&a, 0, sizeof(a));
	a.hdisplay = 800;
	a.vdisplay = 600;
	a.vrefresh = 77;
	b = a;
	drm_mode_finish(&a);
	old_mode_finish(&b);
	CHECK(umode_same_name(&a, &b) && a.vrefresh == 77 && !strcmp(a.name, "800x600"),
	      "finish without totals: %u '%s'", a.vrefresh, a.name);

	/* drm_mode_equal over all pairs of the table plus perturbed copies */
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			drm_mode_table_get(i, &a);
			drm_mode_table_get(j, &b);
			CHECK(drm_mode_equal(&a, &b) == old_mode_equal(&a, &b),
			      "equal %d %d", i, j);
		}
		drm_mode_table_get(i, &a);
		b = a;
		b.hskew = 3;
		b.vscan = 2;
		b.flags |= DRM_MODE_FLAG_PIC_AR_16_9 | DRM_MODE_FLAG_DBLCLK;
		b.type = 0;
		strcpy(b.name, "x");
		CHECK(drm_mode_equal(&a, &b) == old_mode_equal(&a, &b) &&
		      drm_mode_equal(&a, &b), "equal ignores skew/vscan/AR/name %d", i);
		b = a;
		b.clock++;
		CHECK(!drm_mode_equal(&a, &b) && !old_mode_equal(&a, &b), "clock compared %d", i);
		b = a;
		b.flags ^= DRM_MODE_FLAG_NHSYNC;
		CHECK(drm_mode_equal(&a, &b) == old_mode_equal(&a, &b), "polarity %d", i);
		b = a;
		b.vsync_end++;
		CHECK(!drm_mode_equal(&a, &b), "timing compared %d", i);
	}
}

/* ---- CVT and GTF --------------------------------------------------------- */

/* Geometries the EDID standard timings and range descriptors produce. */
static const struct {
	uint16_t w, h;
} geoms[] = {
	{ 640, 480 }, { 720, 400 }, { 800, 600 }, { 848, 480 }, { 1024, 768 },
	{ 1152, 864 }, { 1280, 720 }, { 1280, 768 }, { 1280, 800 },
	{ 1280, 960 }, { 1280, 1024 }, { 1360, 768 }, { 1366, 768 },
	{ 1400, 1050 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 },
	{ 1680, 1050 }, { 1792, 1344 }, { 1856, 1392 }, { 1920, 1080 },
	{ 1920, 1200 }, { 1920, 1440 }, { 2048, 1152 }, { 2048, 1536 },
	{ 2560, 1080 }, { 2560, 1440 }, { 2560, 1600 }, { 3440, 1440 },
	{ 3840, 2160 }, { 4096, 2160 }, { 5120, 2880 },
};
static const uint32_t rates[] = { 50, 60, 70, 72, 75, 85, 100, 120, 144 };

static void test_cvt_gtf_against_old(void)
{
	struct drm_mode_modeinfo a, b;
	unsigned int g, r;
	int cvt_same = 0, cvt_clock_only = 0, cvt_diff = 0;
	int rb_same = 0, rb_clock_only = 0, rb_diff = 0;
	int gtf_same = 0, gtf_diff = 0;
	int shown = 0;

	for (g = 0; g < sizeof(geoms) / sizeof(geoms[0]); g++) {
		for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
			uint32_t w = geoms[g].w, h = geoms[g].h, hz = rates[r];

			drm_mode_cvt(&a, w, h, hz, 0);
			old_mode_cvt(&b, w, h, hz, 0);
			if (umode_same(&a, &b)) {
				cvt_same++;
			} else {
				struct drm_mode_modeinfo c = a;

				c.clock = b.clock;
				drm_mode_finish(&c);
				if (umode_same(&c, &b) || (c.clock = b.clock,
							   c.vrefresh = b.vrefresh,
							   umode_same(&c, &b))) {
					cvt_clock_only++;
				} else {
					cvt_diff++;
					if (shown++ < 6) {
						printf("  cvt %ux%u@%u differs:\n", w, h, hz);
						print_umode("new", &a);
						print_umode("old", &b);
					}
				}
			}

			drm_mode_cvt(&a, w, h, hz, 1);
			old_mode_cvt(&b, w, h, hz, 1);
			if (umode_same(&a, &b)) {
				rb_same++;
			} else {
				struct drm_mode_modeinfo c = a;

				c.clock = b.clock;
				c.vrefresh = b.vrefresh;
				if (umode_same(&c, &b)) {
					rb_clock_only++;
				} else {
					rb_diff++;
					if (shown++ < 12) {
						printf("  cvt-rb %ux%u@%u differs:\n", w, h, hz);
						print_umode("new", &a);
						print_umode("old", &b);
					}
				}
			}

			drm_mode_gtf(&a, w, h, hz);
			old_mode_gtf(&b, w, h, hz);
			if (umode_same(&a, &b)) {
				gtf_same++;
			} else {
				gtf_diff++;
				if (shown++ < 18) {
					printf("  gtf %ux%u@%u differs:\n", w, h, hz);
					print_umode("new", &a);
					print_umode("old", &b);
				}
			}
		}
	}
	printf("cvt vs old: %d same, %d clock only, %d other\n", cvt_same,
	       cvt_clock_only, cvt_diff);
	printf("cvt-rb vs old: %d same, %d clock only, %d other\n", rb_same,
	       rb_clock_only, rb_diff);
	printf("gtf vs old: %d same, %d other\n", gtf_same, gtf_diff);
}

/* Is the mode's timing the table entry's, the clock within the CVT
 * 250 kHz step?  Two things the formula does differently from the
 * published tables are not compared: the start of the horizontal sync
 * (always moved to the next character cell, a cell later than DMT where
 * it is already aligned) and the vertical sync width of 15:9 modes whose
 * height is not a multiple of 9 (1280x768: 10 lines, DMT 7). */
static int timing_close(const struct drm_display_mode *m,
			const struct drm_display_mode *t)
{
	return m->hdisplay == t->hdisplay &&
	       m->hsync_start - t->hsync_start >= 0 &&
	       m->hsync_start - t->hsync_start <= 8 &&
	       m->hsync_end == t->hsync_end && m->htotal == t->htotal &&
	       m->vdisplay == t->vdisplay && m->vsync_start == t->vsync_start &&
	       (m->vsync_end == t->vsync_end || m->vdisplay == 768) &&
	       m->vtotal == t->vtotal &&
	       m->clock - t->clock < 250 && t->clock - m->clock < 250 &&
	       (m->flags & 0xf) == (t->flags & 0xf);
}

static void test_cvt_published(void)
{
	struct drm_display_mode m;
	const struct drm_display_mode *t;
	struct drm_mode_modeinfo u;

	/* DMT entries that are CVT timings: the formula must give them */
	static const struct {
		int w, h, hz, rb;
	} cvt_dmt[] = {
		{ 1280, 768, 60, 0 }, { 1280, 768, 60, 1 }, { 1280, 800, 60, 0 },
		{ 1280, 800, 60, 1 }, { 1400, 1050, 60, 0 }, { 1400, 1050, 60, 1 },
		{ 1440, 900, 60, 0 }, { 1440, 900, 60, 1 }, { 1680, 1050, 60, 0 },
		{ 1680, 1050, 60, 1 }, { 1920, 1200, 60, 0 }, { 1920, 1200, 60, 1 },
		{ 2560, 1600, 60, 0 }, { 2560, 1600, 60, 1 }, { 1280, 768, 75, 0 },
		{ 1280, 800, 75, 0 }, { 1680, 1050, 75, 0 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cvt_dmt) / sizeof(cvt_dmt[0]); i++) {
		t = drm_mode_find_dmt(cvt_dmt[i].w, cvt_dmt[i].h, cvt_dmt[i].hz,
				      cvt_dmt[i].rb);
		CHECK(t != NULL, "DMT %dx%d@%d rb %d present", cvt_dmt[i].w,
		      cvt_dmt[i].h, cvt_dmt[i].hz, cvt_dmt[i].rb);
		if (!t)
			continue;
		CHECK(drm_cvt_mode(&m, cvt_dmt[i].w, cvt_dmt[i].h, cvt_dmt[i].hz,
				   cvt_dmt[i].rb, false, false) == 0, "cvt");
		CHECK(timing_close(&m, t),
		      "cvt %dx%d@%d rb %d: %d %d %d %d %d / %d %d %d %d vs DMT %d %d %d %d %d / %d %d %d %d",
		      cvt_dmt[i].w, cvt_dmt[i].h, cvt_dmt[i].hz, cvt_dmt[i].rb,
		      m.clock, m.hdisplay, m.hsync_start, m.hsync_end, m.htotal,
		      m.vdisplay, m.vsync_start, m.vsync_end, m.vtotal,
		      t->clock, t->hdisplay, t->hsync_start, t->hsync_end, t->htotal,
		      t->vdisplay, t->vsync_start, t->vsync_end, t->vtotal);
		CHECK(drm_mode_is_rb(&m) == cvt_dmt[i].rb, "rb-ness");
	}

	/* the old host vectors (test-edid.c) */
	drm_mode_cvt(&u, 1920, 1080, 60, 1);
	CHECK(u.hdisplay == 1920 && u.vdisplay == 1080 && u.htotal == 2080, "cvt-rb htotal %u", u.htotal);
	CHECK(u.clock >= 138000 && u.clock <= 139500, "cvt-rb clock %u", u.clock);
	CHECK(u.vrefresh == 60, "cvt-rb vrefresh %u", u.vrefresh);
	CHECK((u.flags & 0xf) == (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC), "cvt-rb sync");
	drm_mode_cvt(&u, 1280, 800, 60, 0);
	CHECK(u.hdisplay == 1280 && u.htotal == 1680 && u.vtotal == 831, "cvt 1280x800 %u %u", u.htotal, u.vtotal);
	CHECK(u.clock >= 83000 && u.clock <= 84000, "cvt clock %u", u.clock);
	CHECK(u.type == DRM_MODE_TYPE_DRIVER && !strcmp(u.name, "1280x800"), "type/name");
	drm_mode_gtf(&u, 1024, 768, 60);
	CHECK(u.hdisplay == 1024 && u.vtotal == 795, "gtf %u", u.vtotal);
	/* the classic GTF 1024x768@60: 64.11 MHz, 1344 x 795 */
	CHECK(u.htotal == 1344 && u.clock >= 64000 && u.clock <= 64200 &&
	      (u.flags & 0xf) == (DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC),
	      "gtf 1024x768: %u %u %x", u.htotal, u.clock, u.flags);

	/* interlace and margins */
	CHECK(drm_cvt_mode(&m, 1920, 1080, 60, false, true, false) == 0, "cvt i");
	CHECK((m.flags & DRM_MODE_FLAG_INTERLACE) && !strcmp(m.name, "1920x1080i") &&
	      m.vdisplay == 1080 && (m.vtotal & 1) == 0 && m.vtotal > 1080,
	      "cvt interlaced %u '%s'", m.vtotal, m.name);
	/* the requested rate is the field pair's: the field rate is 120 */
	CHECK(drm_mode_vrefresh(&m) == 120, "cvt interlaced refresh %d", drm_mode_vrefresh(&m));
	CHECK(drm_cvt_mode(&m, 1024, 768, 60, false, false, true) == 0, "cvt margins");
	CHECK(m.hdisplay == 1024 + 2 * 16 && m.vdisplay == 768 + 2 * 13,
	      "cvt margins %ux%u", m.hdisplay, m.vdisplay);
	/* GTF's borders are in the blanking: the active size stays */
	CHECK(drm_gtf_mode(&m, 1024, 768, 60, false, 1) == 0 && m.hdisplay == 1024 &&
	      m.htotal > 1344, "gtf margins %u %u", m.hdisplay, m.htotal);
	/* custom GTF parameters swap the polarity */
	CHECK(drm_gtf_mode_complex(&m, 1024, 768, 60, false, 0, 3600, 80, 128, 40) == 0 &&
	      m.flags == (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC), "gtf2 flags %x", m.flags);

	/* refused geometries, no division by zero */
	CHECK(drm_cvt_mode(&m, 0, 768, 60, false, false, false) == -EINVAL, "cvt w 0");
	CHECK(drm_cvt_mode(&m, 1024, 0, 60, true, false, false) == -EINVAL, "cvt h 0");
	CHECK(drm_cvt_mode(&m, 1024, 1, 60, true, true, false) == -EINVAL, "cvt rb interlaced 1 line");
	CHECK(drm_gtf_mode(&m, 1024, 768, 0, false, 0) == -EINVAL, "gtf 0 Hz");
	CHECK(drm_gtf_mode(&m, 0, 768, 60, false, 0) == -EINVAL, "gtf w 0");
	CHECK(drm_gtf_mode(&m, 1024, 768, 2000, false, 0) == -EINVAL ||
	      m.hdisplay == 1024, "gtf 2000 Hz does not trap");
	drm_mode_cvt(&u, 0, 0, 60, 1);
	CHECK(u.clock == 0 && u.hdisplay == 0 && u.name[0] == 0, "cvt wrapper on 0x0");
}

/* ---- analog television (drm_modes_test.c vectors) ------------------------ */

static void test_analog_tv(void)
{
	struct drm_display_mode m, e;

	CHECK(drm_analog_tv_mode(&m, DRM_MODE_TV_MODE_NTSC, 13500000, 720, 480, true) == 0, "ntsc");
	CHECK(drm_mode_vrefresh(&m) == 60, "ntsc refresh %d", drm_mode_vrefresh(&m));
	CHECK(m.hdisplay == 720 && m.hsync_start == 736 && m.htotal == 858,
	      "ntsc h %u %u %u", m.hdisplay, m.hsync_start, m.htotal);
	CHECK(m.vdisplay == 480 && m.vtotal == 525, "ntsc v %u %u", m.vdisplay, m.vtotal);
	CHECK(!strcmp(m.name, "720x480i"), "ntsc name %s", m.name);
	CHECK(drm_mode_analog_ntsc_480i(&e) == 0 && drm_display_mode_equal(&e, &m), "ntsc inline");

	CHECK(drm_analog_tv_mode(&m, DRM_MODE_TV_MODE_PAL, 13500000, 720, 576, true) == 0, "pal");
	CHECK(drm_mode_vrefresh(&m) == 50, "pal refresh %d", drm_mode_vrefresh(&m));
	CHECK(m.hdisplay == 720 && m.hsync_start == 732 && m.htotal == 864,
	      "pal h %u %u %u", m.hdisplay, m.hsync_start, m.htotal);
	CHECK(m.vdisplay == 576 && m.vtotal == 625, "pal v %u %u", m.vdisplay, m.vtotal);
	CHECK(drm_mode_analog_pal_576i(&e) == 0 && drm_display_mode_equal(&e, &m), "pal inline");

	CHECK(drm_analog_tv_mode(&m, DRM_MODE_TV_MODE_MONOCHROME, 13500000, 720, 576, true) == 0, "mono");
	CHECK(drm_mode_vrefresh(&m) == 50 && m.hsync_start == 732 && m.htotal == 864 &&
	      m.vtotal == 625, "mono timings");

	CHECK(drm_analog_tv_mode(&m, DRM_MODE_TV_MODE_MAX, 13500000, 720, 576, true) == -EINVAL, "bad standard");
	CHECK(drm_analog_tv_mode(&m, DRM_MODE_TV_MODE_PAL, 13500000, 300, 576, true) == -EINVAL, "too narrow");
	CHECK(drm_analog_tv_mode(&m, DRM_MODE_TV_MODE_PAL, 0, 720, 576, true) == -EINVAL, "no clock");
}

/* ---- tables -------------------------------------------------------------- */

static void test_tables(void)
{
	const struct drm_display_mode *t;
	struct drm_display_mode m;
	int i, n, vic, count;

	CHECK(drm_dmt_num_modes() == 88, "DMT 0x01-0x58: %d", drm_dmt_num_modes());
	CHECK(drm_dmt_mode(-1) == NULL && drm_dmt_mode(88) == NULL, "DMT bounds");
	/* every DMT entry passes the basic checks and is named by its size */
	for (i = 0; i < drm_dmt_num_modes(); i++) {
		char name[DRM_DISPLAY_MODE_LEN];

		t = drm_dmt_mode(i);
		CHECK(drm_mode_validate_basic(t) == MODE_OK, "DMT %d valid", i);
		drm_mode_init(&m, t);
		strcpy(name, m.name);
		drm_mode_set_name(&m);
		CHECK(!strcmp(name, m.name), "DMT %d name %s vs %s", i, name, m.name);
	}
	t = drm_mode_find_dmt(1920, 1080, 60, false);
	CHECK(t && t->clock == 148500 && t->htotal == 2200, "find 1080p");
	t = drm_mode_find_dmt(1920, 1200, 60, true);
	CHECK(t && t->clock == 154000 && drm_mode_is_rb(t), "find 1920x1200 RB");
	CHECK(drm_mode_find_dmt(1234, 567, 60, false) == NULL, "find unknown");

	/* CEA: 1-127 and 193-219, each valid, matching its own code */
	count = 0;
	for (vic = 1; vic < drm_cea_num_vics(); vic = drm_cea_next_vic((uint8_t)vic)) {
		uint8_t got;

		t = drm_cea_mode_for_vic((uint8_t)vic);
		CHECK(t != NULL, "vic %d present", vic);
		if (!t)
			continue;
		count++;
		CHECK(drm_mode_validate_basic(t) == MODE_OK, "vic %d valid", vic);
		CHECK(t->picture_aspect_ratio != DRM_MODE_PICTURE_ASPECT_NONE,
		      "vic %d has an aspect", vic);
		CHECK(drm_get_cea_aspect_ratio((uint8_t)vic) == t->picture_aspect_ratio, "aspect %d", vic);
		got = drm_match_cea_mode(t);
		CHECK(got == vic, "match vic %d gives %u", vic, got);
		CHECK(drm_match_cea_mode_clock_tolerance(t, 5) == vic, "tolerance match %d", vic);
		/* the 59.94 / 60 Hz twin matches too */
		drm_mode_init(&m, t);
		m.clock = (int)drm_cea_mode_alternate_clock(t);
		if (m.clock != t->clock)
			CHECK(drm_match_cea_mode(&m) == vic, "alternate clock of %d", vic);
		/* without an aspect the first code of the timing is found */
		drm_mode_init(&m, t);
		m.picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_NONE;
		got = drm_match_cea_mode(&m);
		CHECK(got != 0 && got <= vic, "aspect-less match of %d gives %u", vic, got);
	}
	CHECK(count == 127 + 27, "CEA count %d", count);
	CHECK(drm_cea_mode_for_vic(0) == NULL && drm_cea_mode_for_vic(128) == NULL &&
	      drm_cea_mode_for_vic(192) == NULL && drm_cea_mode_for_vic(220) == NULL,
	      "CEA gaps");
	CHECK(!drm_valid_cea_vic(150) && drm_valid_cea_vic(219), "valid vic");
	CHECK(drm_get_cea_aspect_ratio(0) == DRM_MODE_PICTURE_ASPECT_NONE, "aspect of 0");
	/* the codes with a variable front porch */
	CHECK(drm_cea_mode_for_vic(8)->vtotal == 262 && drm_cea_mode_for_vic(9)->vtotal == 262 &&
	      drm_cea_mode_for_vic(12)->vtotal == 262 && drm_cea_mode_for_vic(13)->vtotal == 262 &&
	      drm_cea_mode_for_vic(23)->vtotal == 312 && drm_cea_mode_for_vic(24)->vtotal == 312 &&
	      drm_cea_mode_for_vic(27)->vtotal == 312 && drm_cea_mode_for_vic(28)->vtotal == 312,
	      "variable porch codes");
	drm_mode_init(&m, drm_cea_mode_for_vic(8));
	n = 0;
	while (drm_cea_mode_alternate_timings(8, &m))
		n++;
	CHECK(n == 1 && m.vtotal == 263, "vic 8 variants %d %u", n, m.vtotal);
	drm_mode_init(&m, drm_cea_mode_for_vic(23));
	n = 0;
	while (drm_cea_mode_alternate_timings(23, &m))
		n++;
	CHECK(n == 2 && m.vtotal == 314, "vic 23 variants %d %u", n, m.vtotal);
	CHECK(drm_match_cea_mode(&m) == 23 || drm_match_cea_mode(&m) == 24, "vic 23 long porch matches");
	drm_mode_init(&m, drm_cea_mode_for_vic(16));
	m.clock = 148352; /* 59.94 */
	CHECK(drm_match_cea_mode(&m) == 16, "1080p59.94");
	m.clock = 148000;
	CHECK(drm_match_cea_mode(&m) == 0, "1080p at 148 MHz is not CEA");
	CHECK(drm_match_cea_mode_clock_tolerance(&m, 600) == 16, "within 600 kHz");
	m.clock = 0;
	CHECK(drm_match_cea_mode(&m) == 0, "no clock");

	/* HDMI 4k codes */
	CHECK(drm_hdmi_num_vics() == 5, "HDMI codes %d", drm_hdmi_num_vics());
	CHECK(!drm_valid_hdmi_vic(0) && drm_valid_hdmi_vic(4) && !drm_valid_hdmi_vic(5), "hdmi valid");
	CHECK(drm_hdmi_mode_for_vic(0) == NULL && drm_hdmi_mode_for_vic(5) == NULL, "hdmi bounds");
	for (vic = 1; vic < drm_hdmi_num_vics(); vic++) {
		t = drm_hdmi_mode_for_vic((uint8_t)vic);
		CHECK(t && drm_match_hdmi_mode(t) == vic, "hdmi vic %d", vic);
		CHECK(t && drm_match_hdmi_mode_clock_tolerance(t, 5) == vic, "hdmi tolerance %d", vic);
		CHECK(drm_get_hdmi_aspect_ratio((uint8_t)vic) == t->picture_aspect_ratio, "hdmi aspect");
	}

	/* every entry of the standard list is a DMT or CEA timing, or the CVT
	 * reduced-blanking one */
	n = drm_mode_table_count();
	for (i = 0; i < n; i++) {
		struct drm_mode_modeinfo u;
		struct drm_display_mode d;
		int found = 0, j;

		drm_mode_table_get(i, &u);
		drm_mode_from_umode(&d, &u);
		for (j = 0; j < drm_dmt_num_modes() && !found; j++)
			found = drm_mode_match(&d, drm_dmt_mode(j), DRM_MODE_MATCH_TIMINGS |
					       DRM_MODE_MATCH_FLAGS) &&
				d.clock == drm_dmt_mode(j)->clock;
		if (!found) {
			d.picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9;
			found = drm_match_cea_mode(&d) != 0;
		}
		if (!found && drm_mode_is_rb(&d)) {
			struct drm_display_mode c;

			drm_cvt_mode(&c, d.hdisplay, d.vdisplay, 60, true, false, false);
			found = drm_mode_match(&d, &c, DRM_MODE_MATCH_TIMINGS) &&
				iabs_host(d.clock - c.clock) < 250;
		}
		CHECK(found, "standard entry %d (%ux%u %u) is in no table", i,
		      u.hdisplay, u.vdisplay, u.clock);
	}
}

/* ---- the mode helpers ------------------------------------------------------ */

static void test_helpers(void)
{
	struct drm_display_mode m, a, b;
	struct drm_display_mode list[8];
	static struct drm_display_info info_store;
	struct drm_display_info *info = &info_store;
	char buf[128], ref[160];
	int hd, vd, n;

	drm_mode_init(&m, drm_cea_mode_for_vic(5)); /* 1080i */
	CHECK(drm_mode_vrefresh(&m) == 60, "1080i refresh %d", drm_mode_vrefresh(&m));
	drm_mode_set_crtcinfo(&m, CRTC_INTERLACE_HALVE_V);
	CHECK(m.crtc_vdisplay == 540 && m.crtc_vtotal == 562 && m.crtc_vsync_start == 542,
	      "halved %u %u %u", m.crtc_vdisplay, m.crtc_vtotal, m.crtc_vsync_start);
	CHECK(m.crtc_vblank_start == 540 && m.crtc_vblank_end == 562 &&
	      m.crtc_hblank_start == 1920 && m.crtc_hblank_end == 2200, "blank values");
	drm_mode_set_crtcinfo(&m, 0);
	CHECK(m.crtc_vdisplay == 1080, "not halved");

	drm_mode_init(&m, drm_dmt_mode(3)); /* 640x480@60 */
	m.flags |= DRM_MODE_FLAG_DBLSCAN;
	CHECK(drm_mode_vrefresh(&m) == 30, "dblscan refresh %d", drm_mode_vrefresh(&m));
	drm_mode_set_crtcinfo(&m, 0);
	CHECK(m.crtc_vdisplay == 960 && m.crtc_vtotal == 1050, "dblscan doubled");
	drm_mode_set_crtcinfo(&m, CRTC_NO_DBLSCAN);
	CHECK(m.crtc_vdisplay == 480, "dblscan kept");
	m.flags &= ~DRM_MODE_FLAG_DBLSCAN;
	m.vscan = 3;
	drm_mode_set_crtcinfo(&m, 0);
	CHECK(m.crtc_vdisplay == 1440 && drm_mode_vrefresh(&m) == 20, "vscan");
	drm_mode_set_crtcinfo(&m, CRTC_NO_VSCAN);
	CHECK(m.crtc_vdisplay == 480, "vscan kept");

	drm_mode_init(&m, drm_cea_mode_for_vic(4)); /* 720p */
	m.flags |= DRM_MODE_FLAG_3D_FRAME_PACKING;
	drm_mode_get_hv_timing(&m, &hd, &vd);
	CHECK(hd == 1280 && vd == 720 + 750, "frame packing %dx%d", hd, vd);
	drm_mode_set_crtcinfo(&m, CRTC_STEREO_DOUBLE);
	CHECK(m.crtc_clock == 148500 && m.crtc_vtotal == 1500, "stereo clock/vtotal");
	m.flags = (m.flags & ~DRM_MODE_FLAG_3D_MASK) | DRM_MODE_FLAG_3D_TOP_AND_BOTTOM;
	drm_mode_get_hv_timing(&m, &hd, &vd);
	CHECK(vd == 720, "top-and-bottom not doubled");
	CHECK(drm_mode_is_stereo(&m), "stereo");

	/* refresh overflow and empty modes */
	memset(&m, 0, sizeof(m));
	CHECK(drm_mode_vrefresh(&m) == 0, "empty refresh");
	m.clock = 100000;
	m.htotal = 65535;
	m.vtotal = 65535;
	m.flags = DRM_MODE_FLAG_DBLSCAN;
	CHECK(drm_mode_vrefresh(&m) == 0, "den overflow");

	/* names */
	memset(&m, 0, sizeof(m));
	m.hdisplay = 65535;
	m.vdisplay = 65535;
	m.flags = DRM_MODE_FLAG_INTERLACE;
	drm_mode_set_name(&m);
	CHECK(!strcmp(m.name, "65535x65535i"), "name %s", m.name);

	/* match */
	drm_mode_init(&a, drm_dmt_mode(3));
	drm_mode_init(&b, &a);
	CHECK(drm_mode_match(NULL, NULL, ~0u) && !drm_mode_match(&a, NULL, 0), "NULLs");
	drm_mode_init(&a, drm_cea_mode_for_vic(16));
	drm_mode_init(&b, &a);
	b.clock = 148499; /* same pixel period in ps */
	CHECK(drm_display_mode_equal(&a, &b), "clock as period");
	b.clock = 25000;
	CHECK(!drm_display_mode_equal(&a, &b) && drm_mode_equal_no_clocks(&a, &b), "no clocks");
	b.flags |= DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF;
	CHECK(!drm_mode_equal_no_clocks(&a, &b) && drm_mode_equal_no_clocks_no_stereo(&a, &b),
	      "stereo");
	b.picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3;
	CHECK(!drm_mode_match(&a, &b, DRM_MODE_MATCH_ASPECT_RATIO), "aspect");
	drm_mode_init(&b, &a);
	b.vtotal += 10;
	b.vsync_start += 10;
	b.vsync_end += 10;
	CHECK(!drm_mode_match(&a, &b, DRM_MODE_MATCH_TIMINGS) &&
	      drm_mode_match(&a, &b, DRM_MODE_MATCH_TIMINGS_VRR), "vrr timings");

	/* validation */
	drm_mode_init(&m, drm_dmt_mode(3));
	CHECK(drm_mode_validate_driver(NULL, &m) == MODE_OK, "valid");
	m.type = 0x80;
	CHECK(drm_mode_validate_basic(&m) == MODE_BAD, "type");
	m.type = DRM_MODE_TYPE_DRIVER;
	m.flags |= 1u << 30;
	CHECK(drm_mode_validate_basic(&m) == MODE_BAD, "flags");
	m.flags = (9u << 14);
	CHECK(drm_mode_validate_basic(&m) == MODE_BAD, "3d");
	drm_mode_init(&m, drm_dmt_mode(3));
	m.clock = 0;
	CHECK(drm_mode_validate_basic(&m) == MODE_CLOCK_LOW, "clock");
	drm_mode_init(&m, drm_dmt_mode(3));
	m.hsync_end = m.htotal + 1;
	CHECK(drm_mode_validate_basic(&m) == MODE_H_ILLEGAL, "h");
	drm_mode_init(&m, drm_dmt_mode(3));
	m.vsync_start = m.vdisplay - 1;
	CHECK(drm_mode_validate_basic(&m) == MODE_V_ILLEGAL, "v");
	drm_mode_init(&m, drm_dmt_mode(3));
	CHECK(drm_mode_validate_size(&m, 640, 480) == MODE_OK &&
	      drm_mode_validate_size(&m, 639, 0) == MODE_VIRTUAL_X &&
	      drm_mode_validate_size(&m, 0, 479) == MODE_VIRTUAL_Y, "size");
	CHECK(!strcmp(drm_get_mode_status_name(MODE_OK), "OK") &&
	      !strcmp(drm_get_mode_status_name(MODE_NO_420), "NO_420") &&
	      !strcmp(drm_get_mode_status_name(MODE_ERROR), "ERROR") &&
	      !strcmp(drm_get_mode_status_name(MODE_STALE), "STALE") &&
	      !strcmp(drm_get_mode_status_name((enum drm_mode_status)99), ""), "status names");

	/* 4:2:0 */
	drm_mode_init(&m, drm_cea_mode_for_vic(97)); /* 2160p60 */
	CHECK(!drm_mode_is_420(info, &m) && drm_mode_validate_ycbcr420(&m, info, false) == MODE_OK,
	      "no 420");
	info->hdmi.y420_vdb_modes[97 / (8 * sizeof(unsigned long))] |= 1ul << (97 % (8 * sizeof(unsigned long)));
	CHECK(drm_mode_is_420_only(info, &m) && drm_mode_is_420(info, &m) &&
	      !drm_mode_is_420_also(info, &m), "420 only");
	CHECK(drm_mode_validate_ycbcr420(&m, info, false) == MODE_NO_420 &&
	      drm_mode_validate_ycbcr420(&m, info, true) == MODE_OK, "420 validate");
	info->hdmi.y420_cmdb_modes[96 / (8 * sizeof(unsigned long))] |= 1ul << (96 % (8 * sizeof(unsigned long)));
	drm_mode_init(&m, drm_cea_mode_for_vic(96));
	CHECK(drm_mode_is_420_also(info, &m) && !drm_mode_is_420_only(info, &m), "420 also");

	/* client form round trip */
	{
		struct drm_mode_modeinfo u;

		drm_mode_init(&m, drm_cea_mode_for_vic(16));
		drm_mode_convert_to_umode(&u, &m);
		CHECK((u.flags & DRM_MODE_FLAG_PIC_AR_MASK) == DRM_MODE_FLAG_PIC_AR_16_9 &&
		      u.vrefresh == 60 && !strcmp(u.name, "1920x1080"), "to umode");
		CHECK(drm_mode_convert_umode(NULL, &a, &u) == 0 && drm_display_mode_equal(&a, &m) &&
		      a.crtc_htotal == 2200 && a.status == MODE_OK, "from umode");
		u.flags |= DRM_MODE_FLAG_PIC_AR_MASK;
		CHECK(drm_mode_convert_umode(NULL, &a, &u) == -EINVAL, "bad aspect");
		u.flags &= ~DRM_MODE_FLAG_PIC_AR_MASK;
		u.htotal = 100;
		CHECK(drm_mode_convert_umode(NULL, &a, &u) == -EINVAL && a.status == MODE_H_ILLEGAL,
		      "invalid umode");
		u.clock = 0x80000000u;
		CHECK(drm_mode_convert_umode(NULL, &a, &u) == -ERANGE, "range");
	}

	/* formatting */
	drm_mode_init(&m, drm_dmt_mode(3));
	n = drm_mode_snprint(buf, sizeof(buf), &m);
	snprintf(ref, sizeof(ref), DRM_MODE_FMT, DRM_MODE_ARG(&m));
	CHECK(!strcmp(buf, ref) && n == (int)strlen(buf), "snprint '%s' vs '%s'", buf, ref);
	m.clock = -5;
	drm_mode_snprint(buf, sizeof(buf), &m);
	snprintf(ref, sizeof(ref), DRM_MODE_FMT, DRM_MODE_ARG(&m));
	CHECK(!strcmp(buf, ref), "snprint negative '%s' vs '%s'", buf, ref);
	CHECK(drm_mode_snprint(buf, 5, &m) == 4 && !strcmp(buf, "\"640"), "snprint short '%s'", buf);

	/* prune, sort, list update, preferred */
	n = 0;
	drm_mode_init(&list[n++], drm_dmt_mode(3)); /* 640x480@60 */
	drm_mode_init(&list[n++], drm_cea_mode_for_vic(16)); /* 1080p60 */
	drm_mode_init(&list[n++], drm_dmt_mode(4)); /* 640x480@72 */
	drm_mode_init(&list[n++], drm_cea_mode_for_vic(31)); /* 1080p50 */
	drm_mode_init(&list[n++], drm_dmt_mode(9)); /* 800x600@72 */
	drm_mode_init(&list[n++], drm_cea_mode_for_vic(4)); /* 720p */
	list[2].status = MODE_CLOCK_HIGH;
	list[5].type |= DRM_MODE_TYPE_PREFERRED;
	n = drm_mode_prune_invalid(list, n, true);
	CHECK(n == 5 && list[2].clock == drm_cea_mode_for_vic(31)->clock, "prune %d", n);
	drm_mode_sort(list, n);
	CHECK(list[0].vdisplay == 720 && list[1].vdisplay == 1080 &&
	      drm_mode_vrefresh(&list[1]) == 60 && drm_mode_vrefresh(&list[2]) == 50 &&
	      list[3].hdisplay == 800 && list[4].hdisplay == 640, "sort order");
	{
		struct drm_display_mode probed[3];

		drm_mode_init(&probed[0], drm_dmt_mode(8)); /* 800x600@60: new */
		drm_mode_init(&probed[1], &list[1]); /* same as 1080p60 */
		probed[1].type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
		drm_mode_init(&probed[2], &list[3]);
		probed[2].type = DRM_MODE_TYPE_USERDEF;
		list[3].status = MODE_STALE;
		n = drm_mode_list_update(list, n, 8, probed, 3);
		CHECK(n == 6 && list[5].hdisplay == 800 && drm_mode_vrefresh(&list[5]) == 60,
		      "update append %d", n);
		CHECK(list[1].type == (DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER), "preferred merged");
		CHECK(list[3].type == DRM_MODE_TYPE_USERDEF && list[3].status == MODE_OK, "stale replaced");
		n = drm_mode_list_update(list, n, 6, probed, 1);
		CHECK(n == 6, "duplicate not appended");
		drm_mode_init(&a, drm_dmt_mode(20));
		n = drm_mode_list_update(list, n, 6, &a, 1);
		CHECK(n == 6, "full list stays full");
	}
	drm_mode_set_preferred(list, n, 640, 480);
	CHECK((list[4].type & DRM_MODE_TYPE_PREFERRED) && list[4].hdisplay == 640 &&
	      !(list[5].type & DRM_MODE_TYPE_PREFERRED), "set preferred");
	drm_mode_sort(list, n);
	CHECK(list[0].hdisplay == 1920 && list[0].type & DRM_MODE_TYPE_PREFERRED, "sort again");
}

int main(void)
{
	test_wrappers();
	test_cvt_gtf_against_old();
	test_cvt_published();
	test_analog_tv();
	test_tables();
	test_helpers();

	if (fails) {
		printf("modes: %d of %d checks failed\n", fails, checks);
		return 1;
	}
	printf("modes: all %d checks passed\n", checks);
	return 0;
}
