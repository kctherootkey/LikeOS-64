// LikeOS-64 -- the EDID parser (pure: no kernel calls, so the host tests
// compile it too).  See drm_edid.h.
#include <kernel/dev/gpu/drm_edid.h>

#ifndef EINVAL
#define EINVAL 22
#endif

static const uint8_t edid_header[8] = { 0x00, 0xff, 0xff, 0xff,
					0xff, 0xff, 0xff, 0x00 };

int drm_edid_block_valid(const uint8_t *block)
{
	uint8_t sum = 0;
	for (int i = 0; i < DRM_EDID_BLOCK; i++)
		sum = (uint8_t)(sum + block[i]);
	return sum == 0;
}

static void mode_set_name(struct drm_mode_modeinfo *m)
{
	drm_mode_finish(m);
}

static int add_mode(struct drm_edid_info *out, const struct drm_mode_modeinfo *m,
		    int preferred)
{
	if (m->hdisplay == 0 || m->vdisplay == 0 || m->clock == 0)
		return -1;
	for (int i = 0; i < out->nmodes; i++) {
		if (drm_mode_equal(&out->modes[i], m)) {
			if (preferred)
				out->preferred = i;
			return i;
		}
	}
	if (out->nmodes >= DRM_EDID_MAX_MODES)
		return -1;
	out->modes[out->nmodes] = *m;
	if (preferred)
		out->preferred = out->nmodes;
	return out->nmodes++;
}

/* An 18-byte detailed timing descriptor. */
static int parse_dtd(const uint8_t *d, struct drm_mode_modeinfo *m)
{
	uint32_t clock = (uint32_t)(d[0] | (d[1] << 8)) * 10; /* kHz */
	uint32_t hactive = d[2] | ((d[4] & 0xf0) << 4);
	uint32_t hblank = d[3] | ((d[4] & 0x0f) << 8);
	uint32_t vactive = d[5] | ((d[7] & 0xf0) << 4);
	uint32_t vblank = d[6] | ((d[7] & 0x0f) << 8);
	uint32_t hsync_off = d[8] | ((d[11] & 0xc0) << 2);
	uint32_t hsync_w = d[9] | ((d[11] & 0x30) << 4);
	uint32_t vsync_off = (d[10] >> 4) | ((d[11] & 0x0c) << 2);
	uint32_t vsync_w = (d[10] & 0x0f) | ((d[11] & 0x03) << 4);
	uint8_t flags = d[17];

	if (clock == 0 || hactive == 0 || vactive == 0)
		return -1;
	for (int i = 0; i < (int)sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;
	m->clock = clock;
	m->hdisplay = (uint16_t)hactive;
	m->hsync_start = (uint16_t)(hactive + hsync_off);
	m->hsync_end = (uint16_t)(hactive + hsync_off + hsync_w);
	m->htotal = (uint16_t)(hactive + hblank);
	m->vdisplay = (uint16_t)vactive;
	m->vsync_start = (uint16_t)(vactive + vsync_off);
	m->vsync_end = (uint16_t)(vactive + vsync_off + vsync_w);
	m->vtotal = (uint16_t)(vactive + vblank);
	if (flags & 0x80)
		m->flags |= DRM_MODE_FLAG_INTERLACE;
	/* Bits 4:3: sync type.  Only the digital separate kind carries
	 * polarities: bit 1 = hsync positive, bit 2 = vsync positive. */
	if ((flags & 0x18) == 0x18) {
		m->flags |= (flags & 0x02) ? DRM_MODE_FLAG_PHSYNC :
					     DRM_MODE_FLAG_NHSYNC;
		m->flags |= (flags & 0x04) ? DRM_MODE_FLAG_PVSYNC :
					     DRM_MODE_FLAG_NVSYNC;
	} else {
		m->flags |= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
	}
	m->type = DRM_MODE_TYPE_DRIVER;
	mode_set_name(m);
	return 0;
}

static void copy_text(char *dst, unsigned cap, const uint8_t *src)
{
	unsigned n = 0;
	for (unsigned i = 0; i < 13 && n + 1 < cap; i++) {
		uint8_t c = src[i];
		if (c == 0x0a || c == 0)
			break;
		if (c < 0x20 || c > 0x7e)
			c = ' ';
		dst[n++] = (char)c;
	}
	while (n > 0 && dst[n - 1] == ' ')
		n--;
	dst[n] = 0;
}

/* The established timings: three bytes of bits, each a fixed mode. */
static const struct {
	uint16_t w, h, hz;
} established[24] = {
	/* byte 0x23 */
	{ 800, 600, 60 }, { 800, 600, 56 }, { 640, 480, 75 }, { 640, 480, 72 },
	{ 640, 480, 67 }, { 640, 480, 60 }, { 720, 400, 88 }, { 720, 400, 70 },
	/* byte 0x24 */
	{ 1280, 1024, 75 }, { 1024, 768, 75 }, { 1024, 768, 70 },
	{ 1024, 768, 60 }, { 1024, 768, 87 }, { 832, 624, 75 },
	{ 800, 600, 75 }, { 800, 600, 72 },
	/* byte 0x25 */
	{ 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
	{ 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 1152, 870, 75 },
};

static int table_lookup(uint32_t w, uint32_t h, uint32_t hz,
			struct drm_mode_modeinfo *m)
{
	int n = drm_mode_table_count();
	for (int i = 0; i < n; i++) {
		struct drm_mode_modeinfo t;
		drm_mode_table_get(i, &t);
		if (t.hdisplay == w && t.vdisplay == h && t.vrefresh == hz &&
		    !(t.flags & DRM_MODE_FLAG_INTERLACE)) {
			*m = t;
			return 0;
		}
	}
	return -1;
}

static void parse_established(const uint8_t *e, struct drm_edid_info *out)
{
	for (int i = 0; i < 24; i++) {
		if (!(e[0x23 + i / 8] & (0x80 >> (i % 8))))
			continue;
		if (!established[i].w)
			continue;
		struct drm_mode_modeinfo m;
		if (table_lookup(established[i].w, established[i].h,
				 established[i].hz, &m) == 0)
			add_mode(out, &m, 0);
	}
}

/* A standard timing: two bytes giving width, aspect and refresh. */
static void parse_standard(const uint8_t *s, struct drm_edid_info *out)
{
	if (s[0] == 0x01 && s[1] == 0x01)
		return; /* unused */
	if (s[0] == 0)
		return;
	uint32_t w = ((uint32_t)s[0] + 31) * 8;
	uint32_t hz = (s[1] & 0x3f) + 60;
	uint32_t h;
	switch (s[1] >> 6) {
	case 0:
		h = (out->version == 1 && out->revision < 3) ? w : w * 10 / 16;
		break;
	case 1:
		h = w * 3 / 4;
		break;
	case 2:
		h = w * 4 / 5;
		break;
	default:
		h = w * 9 / 16;
		break;
	}
	struct drm_mode_modeinfo m;
	if (table_lookup(w, h, hz, &m) != 0)
		drm_mode_cvt(&m, w, h, hz, 0);
	add_mode(out, &m, 0);
}

static void parse_range(const uint8_t *d, struct drm_edid_info *out)
{
	out->has_range = 1;
	out->range.min_vrefresh = d[5];
	out->range.max_vrefresh = d[6];
	out->range.min_hfreq = d[7];
	out->range.max_hfreq = d[8];
	out->range.max_clock_khz = (uint32_t)d[9] * 10000;
	/* EDID 1.4 offset flags in byte 4 */
	if (out->version == 1 && out->revision >= 4) {
		if (d[4] & 0x02)
			out->range.max_vrefresh += 255;
		if ((d[4] & 0x03) == 0x03)
			out->range.min_vrefresh += 255;
		if (d[4] & 0x08)
			out->range.max_hfreq += 255;
		if ((d[4] & 0x0c) == 0x0c)
			out->range.min_hfreq += 255;
	}
}

static void parse_base(const uint8_t *e, struct drm_edid_info *out)
{
	out->version = e[18];
	out->revision = e[19];
	uint16_t mfg = (uint16_t)((e[8] << 8) | e[9]);
	out->vendor[0] = (char)('A' + ((mfg >> 10) & 0x1f) - 1);
	out->vendor[1] = (char)('A' + ((mfg >> 5) & 0x1f) - 1);
	out->vendor[2] = (char)('A' + (mfg & 0x1f) - 1);
	out->vendor[3] = 0;
	out->product = (uint16_t)(e[10] | (e[11] << 8));
	out->serial = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
		      ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
	out->digital = !!(e[20] & 0x80);
	if (out->digital && out->revision >= 4) {
		static const int bpc[8] = { 0, 6, 8, 10, 12, 14, 16, 0 };
		out->bpc = bpc[(e[20] >> 4) & 0x7];
	}
	/* Size in cm, or an aspect ratio when one of them is zero. */
	if (e[21] && e[22]) {
		out->mm_width = (uint32_t)e[21] * 10;
		out->mm_height = (uint32_t)e[22] * 10;
	}

	parse_established(e, out);
	for (int i = 0; i < 8; i++)
		parse_standard(e + 0x26 + i * 2, out);

	/* Four 18-byte descriptors.  A pixel clock of zero marks a display
	 * descriptor: name, range limits, serial, or more standard timings. */
	int preferred_flag = !!(e[24] & 0x02) || out->revision < 4;
	for (int i = 0; i < 4; i++) {
		const uint8_t *d = e + 54 + i * 18;
		if (d[0] || d[1]) {
			struct drm_mode_modeinfo m;
			if (parse_dtd(d, &m) == 0)
				add_mode(out, &m, i == 0 && preferred_flag);
			continue;
		}
		switch (d[3]) {
		case 0xfc: /* monitor name */
			copy_text(out->name, sizeof(out->name), d + 5);
			break;
		case 0xfd: /* range limits */
			parse_range(d, out);
			break;
		case 0xfa: /* six more standard timings */
			for (int j = 0; j < 6; j++)
				parse_standard(d + 5 + j * 2, out);
			break;
		default:
			break;
		}
	}
}

/* CEA-861 extension: short video descriptors, detailed timings, and the
 * HDMI vendor-specific data block. */
static void parse_cea(const uint8_t *ext, struct drm_edid_info *out)
{
	uint8_t rev = ext[1];
	uint8_t dtd_off = ext[2];

	if (rev >= 2) {
		out->cea_underscan = !!(ext[3] & 0x80);
		out->has_audio = !!(ext[3] & 0x40);
	}
	if (rev >= 3 && dtd_off > 4) {
		unsigned i = 4;
		while (i < dtd_off && i < DRM_EDID_BLOCK) {
			uint8_t tag = ext[i] >> 5;
			uint8_t len = ext[i] & 0x1f;
			const uint8_t *p = ext + i + 1;
			if (i + 1 + len > DRM_EDID_BLOCK)
				break;
			if (tag == 2) { /* video data block */
				for (unsigned j = 0; j < len; j++) {
					struct drm_mode_modeinfo m;
					uint8_t vic = p[j] & 0x7f;
					if (drm_mode_cea_vic(vic, &m) == 0)
						add_mode(out, &m, 0);
				}
			} else if (tag == 3 && len >= 3) { /* vendor block */
				uint32_t oui = (uint32_t)p[0] |
					       ((uint32_t)p[1] << 8) |
					       ((uint32_t)p[2] << 16);
				if (oui == 0x000c03) /* HDMI 1.x */
					out->is_hdmi = 1;
				if (oui == 0xc45dd8) /* HDMI forum (2.0) */
					out->is_hdmi = 1;
			}
			i += 1 + len;
		}
	}
	if (dtd_off >= 4) {
		for (unsigned i = dtd_off; i + 18 <= DRM_EDID_BLOCK - 1; i += 18) {
			const uint8_t *d = ext + i;
			if (!d[0] && !d[1])
				break;
			struct drm_mode_modeinfo m;
			if (parse_dtd(d, &m) == 0)
				add_mode(out, &m, 0);
		}
	}
}

int drm_edid_parse(const uint8_t *edid, unsigned len, struct drm_edid_info *out)
{
	for (unsigned i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	out->preferred = -1;
	if (!edid || len < DRM_EDID_BLOCK)
		return -EINVAL;
	for (int i = 0; i < 8; i++)
		if (edid[i] != edid_header[i])
			return -EINVAL;
	if (!drm_edid_block_valid(edid))
		return -EINVAL;
	parse_base(edid, out);
	unsigned next = drm_edid_extensions(edid);
	for (unsigned b = 1; b <= next && (b + 1) * DRM_EDID_BLOCK <= len; b++) {
		const uint8_t *ext = edid + b * DRM_EDID_BLOCK;
		if (!drm_edid_block_valid(ext))
			continue;
		if (ext[0] == 0x02)
			parse_cea(ext, out);
	}
	/* No preferred flag anywhere: the first detailed timing, else the
	 * largest mode. */
	if (out->preferred < 0 && out->nmodes > 0) {
		int best = 0;
		for (int i = 1; i < out->nmodes; i++) {
			uint32_t a = (uint32_t)out->modes[i].hdisplay *
				     out->modes[i].vdisplay;
			uint32_t b = (uint32_t)out->modes[best].hdisplay *
				     out->modes[best].vdisplay;
			if (a > b || (a == b && out->modes[i].vrefresh >
						       out->modes[best].vrefresh))
				best = i;
		}
		out->preferred = best;
	}
	if (out->preferred >= 0)
		out->modes[out->preferred].type |= DRM_MODE_TYPE_PREFERRED;
	return 0;
}
