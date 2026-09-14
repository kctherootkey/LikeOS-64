// LikeOS-64 -- standard display timings (pure: the host tests compile it).
//
// Three sources of modes for a display that gives none, or too few, of its
// own: the VESA DMT list of fixed timings, the CEA-861 video codes that
// televisions use, and the CVT and GTF formulas that derive timings from a
// geometry.  Timings are what the pixel clock and the sync generator are
// programmed with, so unlike a virtual device's these have to be right.
#include <kernel/dev/gpu/drm_edid.h>

/* ---- helpers ------------------------------------------------------------- */

static void u32_to_str(char *dst, unsigned cap, uint32_t v)
{
	char tmp[12];
	unsigned n = 0, i;
	if (v == 0)
		tmp[n++] = '0';
	while (v && n < sizeof(tmp)) {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	}
	for (i = 0; i < n && i + 1 < cap; i++)
		dst[i] = tmp[n - 1 - i];
	dst[i] = 0;
}

void drm_mode_finish(struct drm_mode_modeinfo *m)
{
	uint64_t den = (uint64_t)m->htotal * m->vtotal;
	if (den) {
		uint64_t num = (uint64_t)m->clock * 1000;
		if (m->flags & DRM_MODE_FLAG_INTERLACE)
			num *= 2;
		if (m->flags & DRM_MODE_FLAG_DBLSCAN)
			den *= 2;
		m->vrefresh = (uint32_t)((num + den / 2) / den);
	}
	char *p = m->name;
	unsigned cap = sizeof(m->name);
	u32_to_str(p, cap, m->hdisplay);
	while (*p)
		p++;
	if ((unsigned)(p - m->name) + 1 < cap)
		*p++ = 'x';
	u32_to_str(p, cap - (unsigned)(p - m->name), m->vdisplay);
	while (*p)
		p++;
	if ((m->flags & DRM_MODE_FLAG_INTERLACE) &&
	    (unsigned)(p - m->name) + 1 < cap) {
		*p++ = 'i';
		*p = 0;
	}
}

int drm_mode_equal(const struct drm_mode_modeinfo *a,
		   const struct drm_mode_modeinfo *b)
{
	return a->clock == b->clock && a->hdisplay == b->hdisplay &&
	       a->hsync_start == b->hsync_start && a->hsync_end == b->hsync_end &&
	       a->htotal == b->htotal && a->vdisplay == b->vdisplay &&
	       a->vsync_start == b->vsync_start && a->vsync_end == b->vsync_end &&
	       a->vtotal == b->vtotal &&
	       (a->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_PHSYNC |
			    DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC |
			    DRM_MODE_FLAG_NVSYNC)) ==
		       (b->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_PHSYNC |
				    DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC |
				    DRM_MODE_FLAG_NVSYNC));
}

/* ---- the table ------------------------------------------------------------ */

struct std_mode {
	uint32_t clock;
	uint16_t hd, hss, hse, ht;
	uint16_t vd, vss, vse, vt;
	uint32_t flags;
};

#define PH DRM_MODE_FLAG_PHSYNC
#define NH DRM_MODE_FLAG_NHSYNC
#define PV DRM_MODE_FLAG_PVSYNC
#define NV DRM_MODE_FLAG_NVSYNC
#define IL DRM_MODE_FLAG_INTERLACE

/* VESA DMT, from the 640x480 of every monitor to the 4K of a modern one;
 * reduced-blanking entries marked RB in the comment. */
static const struct std_mode dmt[] = {
	{ 25175, 640, 656, 752, 800, 480, 490, 492, 525, NH | NV },
	{ 31500, 640, 664, 704, 832, 480, 489, 492, 520, NH | NV }, /* 72 */
	{ 31500, 640, 656, 720, 840, 480, 481, 484, 500, NH | NV }, /* 75 */
	{ 36000, 640, 696, 752, 832, 480, 481, 484, 509, NH | NV }, /* 85 */
	{ 36000, 800, 824, 896, 1024, 600, 601, 603, 625, PH | PV }, /* 56 */
	{ 40000, 800, 840, 968, 1056, 600, 601, 605, 628, PH | PV }, /* 60 */
	{ 50000, 800, 856, 976, 1040, 600, 637, 643, 666, PH | PV }, /* 72 */
	{ 49500, 800, 816, 896, 1056, 600, 601, 604, 625, PH | PV }, /* 75 */
	{ 56250, 800, 832, 896, 1048, 600, 601, 604, 631, PH | PV }, /* 85 */
	{ 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, NH | NV }, /* 60 */
	{ 75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, NH | NV }, /* 70 */
	{ 78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, PH | PV }, /* 75 */
	{ 94500, 1024, 1072, 1168, 1376, 768, 769, 772, 808, PH | PV }, /* 85 */
	{ 108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, PH | PV }, /* 75 */
	{ 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV }, /* 60 */
	{ 68250, 1280, 1328, 1360, 1440, 768, 771, 778, 790, PH | NV }, /* 60 RB */
	{ 79500, 1280, 1344, 1472, 1664, 768, 771, 778, 798, NH | PV }, /* 60 */
	{ 102250, 1280, 1360, 1488, 1696, 768, 771, 778, 805, NH | PV }, /* 75 */
	{ 71000, 1280, 1328, 1360, 1440, 800, 803, 809, 823, PH | NV }, /* 60 RB */
	{ 83500, 1280, 1352, 1480, 1680, 800, 803, 809, 831, NH | PV }, /* 60 */
	{ 106500, 1280, 1360, 1488, 1696, 800, 803, 809, 838, NH | PV }, /* 75 */
	{ 108000, 1280, 1376, 1488, 1800, 960, 961, 964, 1000, PH | PV }, /* 60 */
	{ 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, PH | PV }, /* 60 */
	{ 135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, PH | PV }, /* 75 */
	{ 157500, 1280, 1344, 1504, 1728, 1024, 1025, 1028, 1072, PH | PV }, /* 85 */
	{ 85500, 1360, 1424, 1536, 1792, 768, 771, 777, 795, PH | PV }, /* 60 */
	{ 101000, 1400, 1448, 1480, 1560, 1050, 1053, 1057, 1080, PH | NV }, /* 60 RB */
	{ 121750, 1400, 1488, 1632, 1864, 1050, 1053, 1057, 1089, NH | PV }, /* 60 */
	{ 88750, 1440, 1488, 1520, 1600, 900, 903, 909, 926, PH | NV }, /* 60 RB */
	{ 106500, 1440, 1520, 1672, 1904, 900, 903, 909, 934, NH | PV }, /* 60 */
	{ 162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, PH | PV }, /* 60 */
	{ 119000, 1680, 1728, 1760, 1840, 1050, 1053, 1059, 1080, PH | NV }, /* 60 RB */
	{ 146250, 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, NH | PV }, /* 60 */
	{ 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV }, /* 60 */
	{ 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, PH | NV }, /* 60 RB */
	{ 193250, 1920, 2056, 2256, 2592, 1200, 1203, 1209, 1245, NH | PV }, /* 60 */
	{ 234000, 1920, 2048, 2256, 2600, 1440, 1443, 1447, 1500, NH | PV }, /* 60 */
	{ 268500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, PH | NV }, /* 60 RB */
	{ 241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, PH | NV }, /* 60 RB2 */
	{ 268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646, PH | NV }, /* 60 RB */
	{ 348500, 2560, 2752, 3032, 3504, 1600, 1603, 1609, 1658, NH | PV }, /* 60 */
	{ 533250, 3840, 3888, 3920, 4000, 2160, 2163, 2168, 2222, PH | NV }, /* 60 RB */
	{ 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV }, /* 30 */
	{ 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV }, /* 60 CEA */
};

int drm_mode_table_count(void)
{
	return (int)(sizeof(dmt) / sizeof(dmt[0]));
}

static void std_to_mode(const struct std_mode *s, struct drm_mode_modeinfo *m)
{
	for (unsigned i = 0; i < sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;
	m->clock = s->clock;
	m->hdisplay = s->hd;
	m->hsync_start = s->hss;
	m->hsync_end = s->hse;
	m->htotal = s->ht;
	m->vdisplay = s->vd;
	m->vsync_start = s->vss;
	m->vsync_end = s->vse;
	m->vtotal = s->vt;
	m->flags = s->flags;
	m->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_finish(m);
}

int drm_mode_table_get(int i, struct drm_mode_modeinfo *m)
{
	if (i < 0 || i >= drm_mode_table_count())
		return -1;
	std_to_mode(&dmt[i], m);
	return 0;
}

/* CEA-861 video codes: the ones sinks actually list. */
static const struct {
	uint8_t vic;
	struct std_mode m;
} cea[] = {
	{ 1, { 25175, 640, 656, 752, 800, 480, 490, 492, 525, NH | NV } },
	{ 2, { 27000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV } },
	{ 3, { 27000, 720, 736, 798, 858, 480, 489, 495, 525, NH | NV } },
	{ 4, { 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV } },
	{ 5, { 74250, 1920, 2008, 2052, 2200, 1080, 1084, 1094, 1125, PH | PV | IL } },
	{ 16, { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV } },
	{ 17, { 27000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV } },
	{ 18, { 27000, 720, 732, 796, 864, 576, 581, 586, 625, NH | NV } },
	{ 19, { 74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750, PH | PV } },
	{ 20, { 74250, 1920, 2448, 2492, 2640, 1080, 1084, 1094, 1125, PH | PV | IL } },
	{ 31, { 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV } },
	{ 32, { 74250, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, PH | PV } },
	{ 33, { 74250, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV } },
	{ 34, { 74250, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV } },
	{ 60, { 59400, 1280, 3040, 3080, 3300, 720, 725, 730, 750, PH | PV } },
	{ 61, { 74250, 1280, 3700, 3740, 3960, 720, 725, 730, 750, PH | PV } },
	{ 62, { 74250, 1280, 3040, 3080, 3300, 720, 725, 730, 750, PH | PV } },
	{ 63, { 297000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV } },
	{ 64, { 297000, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, PH | PV } },
	{ 93, { 297000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV } },
	{ 94, { 297000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV } },
	{ 95, { 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV } },
	{ 96, { 594000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV } },
	{ 97, { 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV } },
	{ 98, { 297000, 4096, 5116, 5204, 5500, 2160, 2168, 2178, 2250, PH | PV } },
	{ 101, { 594000, 4096, 4896, 4984, 5280, 2160, 2168, 2178, 2250, PH | PV } },
	{ 102, { 594000, 4096, 4184, 4272, 4400, 2160, 2168, 2178, 2250, PH | PV } },
};

int drm_mode_cea_vic(uint8_t vic, struct drm_mode_modeinfo *m)
{
	for (unsigned i = 0; i < sizeof(cea) / sizeof(cea[0]); i++) {
		if (cea[i].vic == vic) {
			std_to_mode(&cea[i].m, m);
			return 0;
		}
	}
	return -1;
}

/* ---- CVT ------------------------------------------------------------------- */

/* VESA CVT 1.2.  Fixed-point in units of 1/1000 where the standard works
 * with fractions; margins off; the "reduced blanking" variant (v1) for
 * digital sinks. */
void drm_mode_cvt(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh, int rb)
{
	const uint32_t cell = 8; /* character cell, pixels */
	const uint32_t min_vsync_bp = 550; /* us */
	const uint32_t min_v_porch = 3; /* lines */
	uint32_t hdisplay = (w / cell) * cell;
	uint32_t vdisplay = h;
	uint32_t vsync;
	uint64_t hperiod; /* ns * 1000 */

	for (unsigned i = 0; i < sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;
	if (!vrefresh)
		vrefresh = 60;

	/* Sync width from the aspect ratio. */
	if (w * 3 == h * 4)
		vsync = 4;
	else if (w * 9 == h * 16)
		vsync = 5;
	else if (w * 10 == h * 16)
		vsync = 6;
	else if (w * 4 == h * 5)
		vsync = 7;
	else if (w * 9 == h * 15)
		vsync = 7;
	else
		vsync = 10;

	if (!rb) {
		/* h period estimate (ns): (1e9/vrefresh - min_vsync_bp*1000) /
		 * (vdisplay + min_v_porch) */
		uint64_t hp_est = (1000000000ULL / vrefresh - (uint64_t)min_vsync_bp * 1000) /
				  (vdisplay + min_v_porch);
		uint32_t vsync_bp = (uint32_t)(((uint64_t)min_vsync_bp * 1000 + hp_est - 1) / hp_est);
		if (vsync_bp < vsync + min_v_porch)
			vsync_bp = vsync + min_v_porch;
		uint32_t vback = vsync_bp - vsync;
		uint32_t vtotal = vdisplay + min_v_porch + vsync + vback;
		/* ideal duty cycle: 30 - 300 * hp_est(us) * ... :
		 * cvt: C' = 30, M' = 300; duty = C' - (M' * hperiod_us / 1000) */
		int64_t duty = 30000 - (int64_t)(300 * hp_est) / 1000000 * 1; /* in 1/1000 % */
		duty = 30000 - (int64_t)(300ULL * hp_est) / 1000; /* hp_est ns -> us*1000 */
		if (duty < 20000)
			duty = 20000;
		uint32_t hblank = (uint32_t)(((uint64_t)hdisplay * (uint64_t)duty) /
					     (uint64_t)(100000 - duty));
		hblank = (hblank / (2 * cell)) * (2 * cell);
		uint32_t htotal = hdisplay + hblank;
		uint32_t hsync = ((htotal * 8 / 100) / cell) * cell;
		uint32_t hfront = hblank / 2 - hsync;
		uint32_t hback = hblank / 2;
		(void)hback;
		hperiod = (1000000000ULL * 1000 / vrefresh) / vtotal; /* ps */
		uint64_t clock_khz = ((uint64_t)htotal * 1000000000ULL / hperiod);
		clock_khz = (clock_khz / 250) * 250; /* 0.25 MHz steps */
		m->clock = (uint32_t)clock_khz;
		m->hdisplay = (uint16_t)hdisplay;
		m->hsync_start = (uint16_t)(hdisplay + hfront);
		m->hsync_end = (uint16_t)(hdisplay + hfront + hsync);
		m->htotal = (uint16_t)htotal;
		m->vdisplay = (uint16_t)vdisplay;
		m->vsync_start = (uint16_t)(vdisplay + min_v_porch);
		m->vsync_end = (uint16_t)(vdisplay + min_v_porch + vsync);
		m->vtotal = (uint16_t)vtotal;
		m->flags = NH | PV;
	} else {
		/* Reduced blanking: 160-pixel horizontal blank, 460 us
		 * vertical blank minimum, positive hsync, negative vsync. */
		const uint32_t rb_hblank = 160, rb_hsync = 32, rb_min_vblank = 460;
		uint64_t hp_est = (1000000000ULL / vrefresh - (uint64_t)rb_min_vblank * 1000) /
				  vdisplay; /* ns */
		uint32_t vbi = (uint32_t)(((uint64_t)rb_min_vblank * 1000 + hp_est - 1) / hp_est);
		if (vbi < 3 + vsync + 6)
			vbi = 3 + vsync + 6;
		uint32_t vtotal = vdisplay + vbi;
		uint32_t htotal = hdisplay + rb_hblank;
		hperiod = (1000000000ULL * 1000 / vrefresh) / vtotal;
		uint64_t clock_khz = ((uint64_t)htotal * 1000000000ULL / hperiod);
		clock_khz = (clock_khz / 250) * 250;
		m->clock = (uint32_t)clock_khz;
		m->hdisplay = (uint16_t)hdisplay;
		m->hsync_start = (uint16_t)(hdisplay + rb_hblank / 2 - rb_hsync);
		m->hsync_end = (uint16_t)(hdisplay + rb_hblank / 2);
		m->htotal = (uint16_t)htotal;
		m->vdisplay = (uint16_t)vdisplay;
		m->vsync_start = (uint16_t)(vdisplay + 3);
		m->vsync_end = (uint16_t)(vdisplay + 3 + vsync);
		m->vtotal = (uint16_t)vtotal;
		m->flags = PH | NV;
	}
	m->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_finish(m);
}

/* VESA GTF (the older formula; what a range-limits descriptor without a
 * CVT flag implies). */
void drm_mode_gtf(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh)
{
	const uint32_t cell = 8;
	const uint32_t min_porch = 1, vsync_rqd = 3, min_vsync_bp = 550;
	uint32_t hdisplay = (w / cell) * cell;

	for (unsigned i = 0; i < sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;
	if (!vrefresh)
		vrefresh = 60;
	/* h period estimate (ns) */
	uint64_t hp = (1000000000ULL / vrefresh - (uint64_t)min_vsync_bp * 1000) /
		      (h + min_porch);
	uint32_t vsync_bp = (uint32_t)(((uint64_t)min_vsync_bp * 1000 + hp / 2) / hp);
	uint32_t vtotal = h + vsync_bp + min_porch;
	/* duty cycle: C - M*hperiod(us); C'=40, M'=600 with the GTF defaults */
	int64_t duty = 40000 - (int64_t)(600ULL * hp) / 1000;
	if (duty < 20000)
		duty = 20000;
	uint32_t hblank = (uint32_t)(((uint64_t)hdisplay * (uint64_t)duty) /
				     (uint64_t)(100000 - duty));
	hblank = (hblank / (2 * cell)) * (2 * cell);
	uint32_t htotal = hdisplay + hblank;
	uint32_t hsync = ((htotal * 8 / 100) / cell) * cell;
	uint32_t hfront = hblank / 2 - hsync;
	uint64_t hperiod_ps = (1000000000ULL * 1000 / vrefresh) / vtotal;
	uint64_t clock_khz = ((uint64_t)htotal * 1000000000ULL / hperiod_ps);
	m->clock = (uint32_t)clock_khz;
	m->hdisplay = (uint16_t)hdisplay;
	m->hsync_start = (uint16_t)(hdisplay + hfront);
	m->hsync_end = (uint16_t)(hdisplay + hfront + hsync);
	m->htotal = (uint16_t)htotal;
	m->vdisplay = (uint16_t)h;
	m->vsync_start = (uint16_t)(h + min_porch);
	m->vsync_end = (uint16_t)(h + min_porch + vsync_rqd);
	m->vtotal = (uint16_t)vtotal;
	m->flags = NH | PV;
	m->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_finish(m);
}

int drm_mode_in_range(const struct drm_mode_modeinfo *m,
		      const struct drm_edid_range *r, uint32_t max_clock_khz,
		      uint32_t max_w, uint32_t max_h)
{
	if (max_clock_khz && m->clock > max_clock_khz)
		return 0;
	if (max_w && m->hdisplay > max_w)
		return 0;
	if (max_h && m->vdisplay > max_h)
		return 0;
	if (r) {
		uint32_t hfreq_khz = m->htotal ? (m->clock / m->htotal) : 0;
		if (r->max_clock_khz && m->clock > r->max_clock_khz)
			return 0;
		if (r->min_vrefresh && m->vrefresh < r->min_vrefresh)
			return 0;
		if (r->max_vrefresh && m->vrefresh > r->max_vrefresh)
			return 0;
		if (r->min_hfreq && hfreq_khz < r->min_hfreq)
			return 0;
		if (r->max_hfreq && hfreq_khz > r->max_hfreq)
			return 0;
	}
	return 1;
}
