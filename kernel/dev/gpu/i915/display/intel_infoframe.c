// LikeOS -- HDMI infoframes: the packets that describe the picture.
//
// Pure: fixed-width types, the mode table and the infoframe library
// only, so the build host's compiler can run it (host/test-hdmi.sh).  An
// infoframe is a 4-byte header (type, version, payload length, checksum)
// and a payload; the checksum makes the whole thing sum to zero modulo
// 256.
//
// Two ways to build them.  The driver's own AVI packer (RGB, the CEA
// code and aspect of the mode, default quantisation range) is what HDMI
// sinks were sent before the infoframe library existed, and what they
// are sent with I915_FEAT_HDMI_INFOFRAME_LIB off.  The library's way
// (intel_hdmi_infoframes_compute()) builds the AVI infoframe from the
// mode and what the sink's EDID says (HDMI 1.4 4k codes go in the vendor
// infoframe instead, a code the sink did not list is left out for an
// HDMI 1.4 sink, the full-range RGB the pipe sends is stated where the
// sink honours it), and adds the source product description and the HDMI
// vendor infoframe.  Both end in the transmitter's buffer form: the
// three header bytes, a hole the hardware fills, the rest.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006 Dave Airlie <airlied@linux.ie>
// Portions Copyright (C) 2006-2009 Intel Corporation

#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/hdmi.h>
#include <kernel/dev/gpu/drm.h>
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

/* ---- the infoframe library's way ---------------------------------------------- */

/* The data island buffer is one byte longer than the packed infoframe:
 * byte 3 is the hardware's (the ECC of the header, or a DisplayPort
 * header byte -- the same buffers carry DisplayPort secondary data
 * packets).
 *
 *	DW0: ECC/DP | HB2 | HB1 | HB0
 *	DW1:   DB3  | DB2 | DB1 | DB0
 *	DW2:   DB7  | DB6 | DB5 | DB4	...
 *
 * (HB a header byte, DB a data byte, DB0 the checksum.)  The packer
 * knows nothing of the hole: the frame is packed one byte in and the
 * header moved back over the gap. */
int intel_hdmi_infoframe_to_dip(const union hdmi_infoframe *f, uint8_t *dip,
				unsigned size)
{
	ssize_t len;

	if (size < 4)
		return -EINVAL;
	for (unsigned i = 0; i < size; i++)
		dip[i] = 0;
	len = hdmi_infoframe_pack_only(f, dip + 1, size - 1);
	if (len < 0)
		return (int)len;
	dip[0] = dip[1];
	dip[1] = dip[2];
	dip[2] = dip[3];
	dip[3] = 0;
	return (int)len + 1;
}

int intel_hdmi_infoframes_compute(const struct drm_connector *c,
				  const struct drm_mode_modeinfo *m, int discrete,
				  struct intel_hdmi_infoframes *out)
{
	struct drm_display_mode mode;
	union hdmi_infoframe f;
	int rc;

	for (unsigned i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	rc = drm_mode_from_umode(&mode, m);
	if (rc)
		return rc;

	/* General control: at 8 bits per component there is no colour
	 * depth to indicate and no pixel packing phase; the packet goes
	 * out all the same. */
	out->gcp_enable = 1;
	out->gcp = 0;

	/* AVI: what the pipe sends is RGB at full range (there is no
	 * limited-range output here), stated as such where the sink takes
	 * the bit or full range is the mode's default. */
	rc = drm_hdmi_avi_infoframe_from_display_mode(&f.avi, c, &mode);
	if (rc)
		return rc;
	f.avi.colorspace = HDMI_COLORSPACE_RGB;
	if (c)
		drm_hdmi_avi_infoframe_quant_range(&f.avi, c, &mode,
						   HDMI_QUANTIZATION_RANGE_FULL);
	rc = hdmi_avi_infoframe_check(&f.avi);
	if (rc)
		return rc;
	rc = intel_hdmi_infoframe_to_dip(&f, out->avi, sizeof(out->avi));
	if (rc < 0)
		return rc;
	out->avi_len = rc;

	/* Source product description: who is sending. */
	rc = hdmi_spd_infoframe_init(&f.spd, "Intel",
				     discrete ? "Discrete gfx" : "Integrated gfx");
	if (rc == 0) {
		f.spd.sdi = HDMI_SPD_SDI_PC;
		rc = hdmi_spd_infoframe_check(&f.spd);
	}
	if (rc == 0) {
		rc = intel_hdmi_infoframe_to_dip(&f, out->spd, sizeof(out->spd));
		if (rc > 0)
			out->spd_len = rc;
	}

	/* HDMI vendor infoframe: the HDMI 1.4 4k video codes and 3D, for a
	 * sink that has the HDMI vendor block. */
	if (c && c->display_info.has_hdmi_infoframe &&
	    drm_hdmi_vendor_infoframe_from_display_mode(&f.vendor.hdmi, c, &mode) == 0 &&
	    hdmi_vendor_infoframe_check(&f.vendor.hdmi) == 0) {
		rc = intel_hdmi_infoframe_to_dip(&f, out->vendor, sizeof(out->vendor));
		if (rc > 0)
			out->vendor_len = rc;
	}
	return 0;
}
