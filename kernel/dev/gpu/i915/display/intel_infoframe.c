// LikeOS-64 -- HDMI infoframes: the packets that describe the picture.
//
// Pure: fixed-width types and the mode table only, so the build host's
// compiler can run it (host/test-hdmi.sh).  An infoframe is a 4-byte
// header (type, version, payload length, checksum) and a payload; the
// checksum makes the whole thing sum to zero modulo 256.
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/i915/intel_infoframe.h>

uint8_t intel_hdmi_vic_for_mode(const struct drm_mode_modeinfo *m)
{
	struct drm_mode_modeinfo cea;
	for (uint8_t vic = 1; vic < 128; vic++) {
		if (drm_mode_cea_vic(vic, &cea) != 0)
			continue;
		if (drm_mode_equal(&cea, m))
			return vic;
	}
	return 0;
}

int intel_hdmi_avi_infoframe(const struct drm_mode_modeinfo *m, uint8_t vic,
			     uint8_t *out, unsigned outlen)
{
	if (outlen < INTEL_AVI_INFOFRAME_SIZE)
		return -1;
	for (unsigned i = 0; i < INTEL_AVI_INFOFRAME_SIZE; i++)
		out[i] = 0;
	out[0] = 0x82; /* AVI */
	out[1] = 2; /* version */
	out[2] = 13; /* payload length */
	uint8_t *pb = out + 4;
	/* RGB, active-format information present, underscanned */
	pb[0] = (0 << 5) | (1 << 4) | 2;
	uint8_t aspect = 0;
	switch (m->flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_4_3:
		aspect = 1;
		break;
	case DRM_MODE_FLAG_PIC_AR_16_9:
		aspect = 2;
		break;
	default:
		break;
	}
	/* no colorimetry, picture aspect, active portion = whole picture */
	pb[1] = (uint8_t)((0 << 6) | (aspect << 4) | 8);
	pb[2] = 0; /* no scaling, default RGB quantisation range */
	pb[3] = vic;
	pb[4] = 0; /* no pixel repetition */
	unsigned sum = 0;
	for (unsigned i = 0; i < INTEL_AVI_INFOFRAME_SIZE; i++)
		sum += out[i];
	out[3] = (uint8_t)((256 - (sum & 0xff)) & 0xff);
	return INTEL_AVI_INFOFRAME_SIZE;
}
