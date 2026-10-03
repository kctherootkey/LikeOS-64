// LikeOS -- HDMI infoframes: build, check, pack, unpack and log them, and
// the AVI and vendor infoframes of a mode on a connector.
//
// The structured form of each infoframe and what its fields mean are in
// <kernel/dev/gpu/hdmi.h>.  Packing writes the 4-byte header (type,
// version, payload length, checksum) and the payload into a zeroed buffer,
// then sets the checksum so that the packet sums to zero modulo 256 (HDMI
// 1.4 section 5.3.5); unpacking verifies it.  A frame is checked before it
// is packed: a frame whose fields contradict its header (an AVI picture
// aspect it cannot carry, a vendor frame with both a video code and a 3D
// layout) is refused rather than sent half right.
//
// The second half turns a mode into the AVI and HDMI vendor infoframes for
// the sink on a connector, by what the sink's EDID said: whether it is an
// HDMI 2.0 sink, whether it has the HDMI vendor block, the video codes it
// lists and whether it honours the RGB quantisation range bit.
//
// Nothing here sleeps or locks; the log function is the only one that
// prints.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Avionic Design's code: MIT
// Portions Copyright (C) 2012 Avionic Design GmbH
// SPDX-License-Identifier for the portions derived from Intel's, Red Hat's, Luc Verhaegen's and Dennis Munsie's code: MIT
// Portions Copyright (c) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (c) 2007-2008 Intel Corporation
// Portions Copyright 2010 Red Hat, Inc.
// Portions Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>

#include <kernel/dev/gpu/hdmi.h>
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/drm_dp.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/drm/drm_mode.h>
#include <kernel/uapi/bug.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* ---- small helpers ------------------------------------------------------ */

static void hdmi_memset(void *dst, int c, size_t n)
{
	u8 *d = dst;

	while (n--)
		*d++ = (u8)c;
}

static void hdmi_memcpy(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;

	while (n--)
		*d++ = *s++;
}

/* The length of the string, looking at no more than `max' bytes: the SPD
 * strings of a received frame are fixed fields without a terminator. */
static size_t hdmi_strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n])
		n++;
	return n;
}

static u8 hdmi_infoframe_checksum(const u8 *ptr, size_t size)
{
	u8 csum = 0;
	size_t i;

	/* compute checksum */
	for (i = 0; i < size; i++)
		csum += ptr[i];

	return (u8)(256 - csum);
}

static void hdmi_infoframe_set_checksum(void *buffer, size_t size)
{
	u8 *ptr = buffer;

	ptr[3] = hdmi_infoframe_checksum(buffer, size);
}

/* ---- AVI ---------------------------------------------------------------- */

void hdmi_avi_infoframe_init(struct hdmi_avi_infoframe *frame)
{
	hdmi_memset(frame, 0, sizeof(*frame));

	frame->type = HDMI_INFOFRAME_TYPE_AVI;
	frame->version = 2;
	frame->length = HDMI_AVI_INFOFRAME_SIZE;
}

static int hdmi_avi_infoframe_check_only(const struct hdmi_avi_infoframe *frame)
{
	if (frame->type != HDMI_INFOFRAME_TYPE_AVI ||
	    frame->version != 2 ||
	    frame->length != HDMI_AVI_INFOFRAME_SIZE)
		return -EINVAL;

	/* The two bits of the field carry none, 4:3 and 16:9 only. */
	if (frame->picture_aspect > HDMI_PICTURE_ASPECT_16_9)
		return -EINVAL;

	return 0;
}

int hdmi_avi_infoframe_check(struct hdmi_avi_infoframe *frame)
{
	return hdmi_avi_infoframe_check_only(frame);
}

ssize_t hdmi_avi_infoframe_pack_only(const struct hdmi_avi_infoframe *frame,
				     void *buffer, size_t size)
{
	u8 *ptr = buffer;
	size_t length;
	int ret;

	ret = hdmi_avi_infoframe_check_only(frame);
	if (ret)
		return ret;

	length = HDMI_INFOFRAME_HEADER_SIZE + frame->length;

	if (size < length)
		return -ENOSPC;

	hdmi_memset(buffer, 0, size);

	ptr[0] = (u8)frame->type;
	ptr[1] = frame->version;
	ptr[2] = frame->length;
	ptr[3] = 0; /* checksum */

	/* start infoframe payload */
	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	ptr[0] = (u8)(((frame->colorspace & 0x3) << 5) | (frame->scan_mode & 0x3));

	/* Data byte 1, bit 4 says the active format aspect ratio is given. */
	if (frame->active_aspect & 0xf)
		ptr[0] |= BIT(4);

	/* Bits 3 and 2: horizontal / vertical bar data is given. */
	if (frame->top_bar || frame->bottom_bar)
		ptr[0] |= BIT(3);

	if (frame->left_bar || frame->right_bar)
		ptr[0] |= BIT(2);

	ptr[1] = (u8)(((frame->colorimetry & 0x3) << 6) |
		      ((frame->picture_aspect & 0x3) << 4) |
		      (frame->active_aspect & 0xf));

	ptr[2] = (u8)(((frame->extended_colorimetry & 0x7) << 4) |
		      ((frame->quantization_range & 0x3) << 2) |
		      (frame->nups & 0x3));

	if (frame->itc)
		ptr[2] |= BIT(7);

	ptr[3] = frame->video_code & 0x7f;

	ptr[4] = (u8)(((frame->ycc_quantization_range & 0x3) << 6) |
		      ((frame->content_type & 0x3) << 4) |
		      (frame->pixel_repeat & 0xf));

	ptr[5] = frame->top_bar & 0xff;
	ptr[6] = (frame->top_bar >> 8) & 0xff;
	ptr[7] = frame->bottom_bar & 0xff;
	ptr[8] = (frame->bottom_bar >> 8) & 0xff;
	ptr[9] = frame->left_bar & 0xff;
	ptr[10] = (frame->left_bar >> 8) & 0xff;
	ptr[11] = frame->right_bar & 0xff;
	ptr[12] = (frame->right_bar >> 8) & 0xff;

	hdmi_infoframe_set_checksum(buffer, length);

	return (ssize_t)length;
}

ssize_t hdmi_avi_infoframe_pack(struct hdmi_avi_infoframe *frame,
				void *buffer, size_t size)
{
	int ret;

	ret = hdmi_avi_infoframe_check(frame);
	if (ret)
		return ret;

	return hdmi_avi_infoframe_pack_only(frame, buffer, size);
}

/* ---- source product description ------------------------------------------ */

int hdmi_spd_infoframe_init(struct hdmi_spd_infoframe *frame,
			    const char *vendor, const char *product)
{
	size_t len;

	hdmi_memset(frame, 0, sizeof(*frame));

	frame->type = HDMI_INFOFRAME_TYPE_SPD;
	frame->version = 1;
	frame->length = HDMI_SPD_INFOFRAME_SIZE;

	/* Truncated to the field; no terminator when it fills the field. */
	len = hdmi_strnlen(vendor, sizeof(frame->vendor));
	hdmi_memcpy(frame->vendor, vendor, len);
	len = hdmi_strnlen(product, sizeof(frame->product));
	hdmi_memcpy(frame->product, product, len);

	return 0;
}

static int hdmi_spd_infoframe_check_only(const struct hdmi_spd_infoframe *frame)
{
	if (frame->type != HDMI_INFOFRAME_TYPE_SPD ||
	    frame->version != 1 ||
	    frame->length != HDMI_SPD_INFOFRAME_SIZE)
		return -EINVAL;

	return 0;
}

int hdmi_spd_infoframe_check(struct hdmi_spd_infoframe *frame)
{
	return hdmi_spd_infoframe_check_only(frame);
}

ssize_t hdmi_spd_infoframe_pack_only(const struct hdmi_spd_infoframe *frame,
				     void *buffer, size_t size)
{
	u8 *ptr = buffer;
	size_t length;
	int ret;

	ret = hdmi_spd_infoframe_check_only(frame);
	if (ret)
		return ret;

	length = HDMI_INFOFRAME_HEADER_SIZE + frame->length;

	if (size < length)
		return -ENOSPC;

	hdmi_memset(buffer, 0, size);

	ptr[0] = (u8)frame->type;
	ptr[1] = frame->version;
	ptr[2] = frame->length;
	ptr[3] = 0; /* checksum */

	/* start infoframe payload */
	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	hdmi_memcpy(ptr, frame->vendor, sizeof(frame->vendor));
	hdmi_memcpy(ptr + 8, frame->product, sizeof(frame->product));

	ptr[24] = (u8)frame->sdi;

	hdmi_infoframe_set_checksum(buffer, length);

	return (ssize_t)length;
}

ssize_t hdmi_spd_infoframe_pack(struct hdmi_spd_infoframe *frame,
				void *buffer, size_t size)
{
	int ret;

	ret = hdmi_spd_infoframe_check(frame);
	if (ret)
		return ret;

	return hdmi_spd_infoframe_pack_only(frame, buffer, size);
}

/* ---- audio ---------------------------------------------------------------- */

int hdmi_audio_infoframe_init(struct hdmi_audio_infoframe *frame)
{
	hdmi_memset(frame, 0, sizeof(*frame));

	frame->type = HDMI_INFOFRAME_TYPE_AUDIO;
	frame->version = 1;
	frame->length = HDMI_AUDIO_INFOFRAME_SIZE;

	return 0;
}

static int hdmi_audio_infoframe_check_only(const struct hdmi_audio_infoframe *frame)
{
	if (frame->type != HDMI_INFOFRAME_TYPE_AUDIO ||
	    frame->version != 1 ||
	    frame->length != HDMI_AUDIO_INFOFRAME_SIZE)
		return -EINVAL;

	return 0;
}

int hdmi_audio_infoframe_check(const struct hdmi_audio_infoframe *frame)
{
	return hdmi_audio_infoframe_check_only(frame);
}

static void
hdmi_audio_infoframe_pack_payload(const struct hdmi_audio_infoframe *frame,
				  u8 *buffer)
{
	u8 channels;

	/* The field holds the channel count minus one; 0 also stands for
	 * "refer to the stream header". */
	if (frame->channels >= 2)
		channels = frame->channels - 1;
	else
		channels = 0;

	buffer[0] = (u8)(((frame->coding_type & 0xf) << 4) | (channels & 0x7));
	buffer[1] = (u8)(((frame->sample_frequency & 0x7) << 2) |
			 (frame->sample_size & 0x3));
	buffer[2] = frame->coding_type_ext & 0x1f;
	buffer[3] = frame->channel_allocation;
	buffer[4] = (u8)((frame->level_shift_value & 0xf) << 3);

	if (frame->downmix_inhibit)
		buffer[4] |= BIT(7);
}

ssize_t hdmi_audio_infoframe_pack_only(const struct hdmi_audio_infoframe *frame,
				       void *buffer, size_t size)
{
	u8 *ptr = buffer;
	size_t length;
	int ret;

	ret = hdmi_audio_infoframe_check_only(frame);
	if (ret)
		return ret;

	length = HDMI_INFOFRAME_HEADER_SIZE + frame->length;

	if (size < length)
		return -ENOSPC;

	hdmi_memset(buffer, 0, size);

	ptr[0] = (u8)frame->type;
	ptr[1] = frame->version;
	ptr[2] = frame->length;
	ptr[3] = 0; /* checksum */

	hdmi_audio_infoframe_pack_payload(frame,
					  ptr + HDMI_INFOFRAME_HEADER_SIZE);

	hdmi_infoframe_set_checksum(buffer, length);

	return (ssize_t)length;
}

ssize_t hdmi_audio_infoframe_pack(struct hdmi_audio_infoframe *frame,
				  void *buffer, size_t size)
{
	int ret;

	ret = hdmi_audio_infoframe_check(frame);
	if (ret)
		return ret;

	return hdmi_audio_infoframe_pack_only(frame, buffer, size);
}

int hdmi_audio_infoframe_pack_payload_only(const struct hdmi_audio_infoframe *frame,
					   u8 *buffer, size_t size)
{
	int ret;

	ret = hdmi_audio_infoframe_check(frame);
	if (ret)
		return ret;
	if (size < HDMI_AUDIO_INFOFRAME_SIZE)
		return -ENOSPC;

	hdmi_memset(buffer, 0, HDMI_AUDIO_INFOFRAME_SIZE);
	hdmi_audio_infoframe_pack_payload(frame, buffer);
	return 0;
}

ssize_t hdmi_audio_infoframe_pack_for_dp(const struct hdmi_audio_infoframe *frame,
					 struct dp_sdp *sdp, u8 dp_version)
{
	int ret;

	ret = hdmi_audio_infoframe_check(frame);
	if (ret)
		return ret;

	hdmi_memset(sdp->db, 0, sizeof(sdp->db));

	/* Secondary-data packet header */
	sdp->sdp_header.HB0 = 0;
	sdp->sdp_header.HB1 = (u8)frame->type;
	sdp->sdp_header.HB2 = DP_SDP_AUDIO_INFOFRAME_HB2;
	sdp->sdp_header.HB3 = (u8)((dp_version & 0x3f) << 2);

	hdmi_audio_infoframe_pack_payload(frame, sdp->db);

	/* the frame length plus the four SDP header bytes */
	return frame->length + 4;
}

/* ---- HDMI vendor ---------------------------------------------------------- */

int hdmi_vendor_infoframe_init(struct hdmi_vendor_infoframe *frame)
{
	hdmi_memset(frame, 0, sizeof(*frame));

	frame->type = HDMI_INFOFRAME_TYPE_VENDOR;
	frame->version = 1;

	frame->oui = HDMI_IEEE_OUI;

	/* 0 is a valid 3D layout (frame packing), so "none" has a value of
	 * its own. */
	frame->s3d_struct = HDMI_3D_STRUCTURE_INVALID;
	frame->length = HDMI_VENDOR_INFOFRAME_SIZE;

	return 0;
}

static int hdmi_vendor_infoframe_length(const struct hdmi_vendor_infoframe *frame)
{
	/* side by side (half) also carries the 3D_Ext_Data byte */
	if (frame->s3d_struct >= HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF)
		return 6;
	else if (frame->vic != 0 || frame->s3d_struct != HDMI_3D_STRUCTURE_INVALID)
		return 5;
	else
		return 4;
}

static int hdmi_vendor_infoframe_check_only(const struct hdmi_vendor_infoframe *frame)
{
	if (frame->type != HDMI_INFOFRAME_TYPE_VENDOR ||
	    frame->version != 1 ||
	    frame->oui != HDMI_IEEE_OUI)
		return -EINVAL;

	/* only one of those can be supplied */
	if (frame->vic != 0 && frame->s3d_struct != HDMI_3D_STRUCTURE_INVALID)
		return -EINVAL;

	if (frame->length != hdmi_vendor_infoframe_length(frame))
		return -EINVAL;

	return 0;
}

int hdmi_vendor_infoframe_check(struct hdmi_vendor_infoframe *frame)
{
	frame->length = (unsigned char)hdmi_vendor_infoframe_length(frame);

	return hdmi_vendor_infoframe_check_only(frame);
}

ssize_t hdmi_vendor_infoframe_pack_only(const struct hdmi_vendor_infoframe *frame,
					void *buffer, size_t size)
{
	u8 *ptr = buffer;
	size_t length;
	int ret;

	ret = hdmi_vendor_infoframe_check_only(frame);
	if (ret)
		return ret;

	length = HDMI_INFOFRAME_HEADER_SIZE + frame->length;

	if (size < length)
		return -ENOSPC;

	hdmi_memset(buffer, 0, size);

	ptr[0] = (u8)frame->type;
	ptr[1] = frame->version;
	ptr[2] = frame->length;
	ptr[3] = 0; /* checksum */

	/* HDMI OUI, least significant byte first */
	ptr[4] = 0x03;
	ptr[5] = 0x0c;
	ptr[6] = 0x00;

	if (frame->s3d_struct != HDMI_3D_STRUCTURE_INVALID) {
		ptr[7] = 0x2 << 5; /* video format: 3D */
		ptr[8] = (u8)((frame->s3d_struct & 0xf) << 4);
		if (frame->s3d_struct >= HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF)
			ptr[9] = (u8)((frame->s3d_ext_data & 0xf) << 4);
	} else if (frame->vic) {
		ptr[7] = 0x1 << 5; /* video format: extended resolution */
		ptr[8] = frame->vic;
	} else {
		ptr[7] = 0x0 << 5; /* video format: none */
	}

	hdmi_infoframe_set_checksum(buffer, length);

	return (ssize_t)length;
}

ssize_t hdmi_vendor_infoframe_pack(struct hdmi_vendor_infoframe *frame,
				   void *buffer, size_t size)
{
	int ret;

	ret = hdmi_vendor_infoframe_check(frame);
	if (ret)
		return ret;

	return hdmi_vendor_infoframe_pack_only(frame, buffer, size);
}

static int
hdmi_vendor_any_infoframe_check_only(const union hdmi_vendor_any_infoframe *frame)
{
	if (frame->any.type != HDMI_INFOFRAME_TYPE_VENDOR ||
	    frame->any.version != 1)
		return -EINVAL;

	return 0;
}

/* ---- Dynamic Range and Mastering -------------------------------------- */

int hdmi_drm_infoframe_init(struct hdmi_drm_infoframe *frame)
{
	hdmi_memset(frame, 0, sizeof(*frame));

	frame->type = HDMI_INFOFRAME_TYPE_DRM;
	frame->version = 1;
	frame->length = HDMI_DRM_INFOFRAME_SIZE;

	return 0;
}

static int hdmi_drm_infoframe_check_only(const struct hdmi_drm_infoframe *frame)
{
	if (frame->type != HDMI_INFOFRAME_TYPE_DRM ||
	    frame->version != 1)
		return -EINVAL;

	if (frame->length != HDMI_DRM_INFOFRAME_SIZE)
		return -EINVAL;

	return 0;
}

int hdmi_drm_infoframe_check(struct hdmi_drm_infoframe *frame)
{
	return hdmi_drm_infoframe_check_only(frame);
}

ssize_t hdmi_drm_infoframe_pack_only(const struct hdmi_drm_infoframe *frame,
				     void *buffer, size_t size)
{
	u8 *ptr = buffer;
	size_t length;
	int i;

	length = HDMI_INFOFRAME_HEADER_SIZE + frame->length;

	if (size < length)
		return -ENOSPC;

	hdmi_memset(buffer, 0, size);

	ptr[0] = (u8)frame->type;
	ptr[1] = frame->version;
	ptr[2] = frame->length;
	ptr[3] = 0; /* checksum */

	/* start infoframe payload */
	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	*ptr++ = (u8)frame->eotf;
	*ptr++ = (u8)frame->metadata_type;

	/* every 16-bit value least significant byte first */
	for (i = 0; i < 3; i++) {
		*ptr++ = (u8)frame->display_primaries[i].x;
		*ptr++ = (u8)(frame->display_primaries[i].x >> 8);
		*ptr++ = (u8)frame->display_primaries[i].y;
		*ptr++ = (u8)(frame->display_primaries[i].y >> 8);
	}

	*ptr++ = (u8)frame->white_point.x;
	*ptr++ = (u8)(frame->white_point.x >> 8);

	*ptr++ = (u8)frame->white_point.y;
	*ptr++ = (u8)(frame->white_point.y >> 8);

	*ptr++ = (u8)frame->max_display_mastering_luminance;
	*ptr++ = (u8)(frame->max_display_mastering_luminance >> 8);

	*ptr++ = (u8)frame->min_display_mastering_luminance;
	*ptr++ = (u8)(frame->min_display_mastering_luminance >> 8);

	*ptr++ = (u8)frame->max_cll;
	*ptr++ = (u8)(frame->max_cll >> 8);

	*ptr++ = (u8)frame->max_fall;
	*ptr++ = (u8)(frame->max_fall >> 8);

	hdmi_infoframe_set_checksum(buffer, length);

	return (ssize_t)length;
}

ssize_t hdmi_drm_infoframe_pack(struct hdmi_drm_infoframe *frame,
				void *buffer, size_t size)
{
	int ret;

	ret = hdmi_drm_infoframe_check(frame);
	if (ret)
		return ret;

	return hdmi_drm_infoframe_pack_only(frame, buffer, size);
}

/* ---- any vendor infoframe (only the HDMI one is known) ------------------ */

static int
hdmi_vendor_any_infoframe_check(union hdmi_vendor_any_infoframe *frame)
{
	int ret;

	ret = hdmi_vendor_any_infoframe_check_only(frame);
	if (ret)
		return ret;

	/* we only know about HDMI vendor infoframes */
	if (frame->any.oui != HDMI_IEEE_OUI)
		return -EINVAL;

	return hdmi_vendor_infoframe_check(&frame->hdmi);
}

static ssize_t
hdmi_vendor_any_infoframe_pack_only(const union hdmi_vendor_any_infoframe *frame,
				    void *buffer, size_t size)
{
	int ret;

	ret = hdmi_vendor_any_infoframe_check_only(frame);
	if (ret)
		return ret;

	/* we only know about HDMI vendor infoframes */
	if (frame->any.oui != HDMI_IEEE_OUI)
		return -EINVAL;

	return hdmi_vendor_infoframe_pack_only(&frame->hdmi, buffer, size);
}

static ssize_t
hdmi_vendor_any_infoframe_pack(union hdmi_vendor_any_infoframe *frame,
			       void *buffer, size_t size)
{
	int ret;

	ret = hdmi_vendor_any_infoframe_check(frame);
	if (ret)
		return ret;

	return hdmi_vendor_any_infoframe_pack_only(frame, buffer, size);
}

/* ---- any infoframe -------------------------------------------------------- */

ssize_t
hdmi_infoframe_pack_only(const union hdmi_infoframe *frame, void *buffer, size_t size)
{
	ssize_t length;

	switch (frame->any.type) {
	case HDMI_INFOFRAME_TYPE_AVI:
		length = hdmi_avi_infoframe_pack_only(&frame->avi,
						      buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_DRM:
		length = hdmi_drm_infoframe_pack_only(&frame->drm,
						      buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_SPD:
		length = hdmi_spd_infoframe_pack_only(&frame->spd,
						      buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_AUDIO:
		length = hdmi_audio_infoframe_pack_only(&frame->audio,
							buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		length = hdmi_vendor_any_infoframe_pack_only(&frame->vendor,
							     buffer, size);
		break;
	default:
		WARN(1, "Bad infoframe type %d", frame->any.type);
		length = -EINVAL;
	}

	return length;
}

ssize_t
hdmi_infoframe_pack(union hdmi_infoframe *frame,
		    void *buffer, size_t size)
{
	ssize_t length;

	switch (frame->any.type) {
	case HDMI_INFOFRAME_TYPE_AVI:
		length = hdmi_avi_infoframe_pack(&frame->avi, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_DRM:
		length = hdmi_drm_infoframe_pack(&frame->drm, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_SPD:
		length = hdmi_spd_infoframe_pack(&frame->spd, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_AUDIO:
		length = hdmi_audio_infoframe_pack(&frame->audio, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		length = hdmi_vendor_any_infoframe_pack(&frame->vendor,
							buffer, size);
		break;
	default:
		WARN(1, "Bad infoframe type %d", frame->any.type);
		length = -EINVAL;
	}

	return length;
}

/* ---- logging ---------------------------------------------------------- */

/* One line: the caller's level prefix, the device name, the text. */
#define hdmi_log(fmt, ...) \
	kprintf("%s%s%s" fmt, level ? level : "", dev ? dev : "", \
		dev ? ": " : "", ##__VA_ARGS__)

static const char *hdmi_infoframe_type_get_name(enum hdmi_infoframe_type type)
{
	if ((int)type < 0x80 || (int)type > 0x9f)
		return "Invalid";
	switch (type) {
	case HDMI_INFOFRAME_TYPE_VENDOR:
		return "Vendor";
	case HDMI_INFOFRAME_TYPE_AVI:
		return "Auxiliary Video Information (AVI)";
	case HDMI_INFOFRAME_TYPE_SPD:
		return "Source Product Description (SPD)";
	case HDMI_INFOFRAME_TYPE_AUDIO:
		return "Audio";
	case HDMI_INFOFRAME_TYPE_DRM:
		return "Dynamic Range and Mastering";
	}
	return "Reserved";
}

static void hdmi_infoframe_log_header(const char *level, const char *dev,
				      const struct hdmi_any_infoframe *frame)
{
	hdmi_log("HDMI infoframe: %s, version %u, length %u\n",
		 hdmi_infoframe_type_get_name(frame->type),
		 frame->version, frame->length);
}

static const char *hdmi_colorspace_get_name(enum hdmi_colorspace colorspace)
{
	switch (colorspace) {
	case HDMI_COLORSPACE_RGB:
		return "RGB";
	case HDMI_COLORSPACE_YUV422:
		return "YCbCr 4:2:2";
	case HDMI_COLORSPACE_YUV444:
		return "YCbCr 4:4:4";
	case HDMI_COLORSPACE_YUV420:
		return "YCbCr 4:2:0";
	case HDMI_COLORSPACE_RESERVED4:
		return "Reserved (4)";
	case HDMI_COLORSPACE_RESERVED5:
		return "Reserved (5)";
	case HDMI_COLORSPACE_RESERVED6:
		return "Reserved (6)";
	case HDMI_COLORSPACE_IDO_DEFINED:
		return "IDO Defined";
	}
	return "Invalid";
}

static const char *hdmi_scan_mode_get_name(enum hdmi_scan_mode scan_mode)
{
	switch (scan_mode) {
	case HDMI_SCAN_MODE_NONE:
		return "No Data";
	case HDMI_SCAN_MODE_OVERSCAN:
		return "Overscan";
	case HDMI_SCAN_MODE_UNDERSCAN:
		return "Underscan";
	case HDMI_SCAN_MODE_RESERVED:
		return "Reserved";
	}
	return "Invalid";
}

static const char *hdmi_colorimetry_get_name(enum hdmi_colorimetry colorimetry)
{
	switch (colorimetry) {
	case HDMI_COLORIMETRY_NONE:
		return "No Data";
	case HDMI_COLORIMETRY_ITU_601:
		return "ITU601";
	case HDMI_COLORIMETRY_ITU_709:
		return "ITU709";
	case HDMI_COLORIMETRY_EXTENDED:
		return "Extended";
	}
	return "Invalid";
}

static const char *
hdmi_picture_aspect_get_name(enum hdmi_picture_aspect picture_aspect)
{
	switch (picture_aspect) {
	case HDMI_PICTURE_ASPECT_NONE:
		return "No Data";
	case HDMI_PICTURE_ASPECT_4_3:
		return "4:3";
	case HDMI_PICTURE_ASPECT_16_9:
		return "16:9";
	case HDMI_PICTURE_ASPECT_64_27:
		return "64:27";
	case HDMI_PICTURE_ASPECT_256_135:
		return "256:135";
	case HDMI_PICTURE_ASPECT_RESERVED:
		return "Reserved";
	}
	return "Invalid";
}

static const char *
hdmi_active_aspect_get_name(enum hdmi_active_aspect active_aspect)
{
	if ((int)active_aspect < 0 || (int)active_aspect > 0xf)
		return "Invalid";

	switch (active_aspect) {
	case HDMI_ACTIVE_ASPECT_16_9_TOP:
		return "16:9 Top";
	case HDMI_ACTIVE_ASPECT_14_9_TOP:
		return "14:9 Top";
	case HDMI_ACTIVE_ASPECT_16_9_CENTER:
		return "16:9 Center";
	case HDMI_ACTIVE_ASPECT_PICTURE:
		return "Same as Picture";
	case HDMI_ACTIVE_ASPECT_4_3:
		return "4:3";
	case HDMI_ACTIVE_ASPECT_16_9:
		return "16:9";
	case HDMI_ACTIVE_ASPECT_14_9:
		return "14:9";
	case HDMI_ACTIVE_ASPECT_4_3_SP_14_9:
		return "4:3 SP 14:9";
	case HDMI_ACTIVE_ASPECT_16_9_SP_14_9:
		return "16:9 SP 14:9";
	case HDMI_ACTIVE_ASPECT_16_9_SP_4_3:
		return "16:9 SP 4:3";
	}
	return "Reserved";
}

static const char *
hdmi_extended_colorimetry_get_name(enum hdmi_extended_colorimetry ext_col)
{
	switch (ext_col) {
	case HDMI_EXTENDED_COLORIMETRY_XV_YCC_601:
		return "xvYCC 601";
	case HDMI_EXTENDED_COLORIMETRY_XV_YCC_709:
		return "xvYCC 709";
	case HDMI_EXTENDED_COLORIMETRY_S_YCC_601:
		return "sYCC 601";
	case HDMI_EXTENDED_COLORIMETRY_OPYCC_601:
		return "opYCC 601";
	case HDMI_EXTENDED_COLORIMETRY_OPRGB:
		return "opRGB";
	case HDMI_EXTENDED_COLORIMETRY_BT2020_CONST_LUM:
		return "BT.2020 Constant Luminance";
	case HDMI_EXTENDED_COLORIMETRY_BT2020:
		return "BT.2020";
	case HDMI_EXTENDED_COLORIMETRY_RESERVED:
		return "Reserved";
	}
	return "Invalid";
}

static const char *
hdmi_quantization_range_get_name(enum hdmi_quantization_range qrange)
{
	switch (qrange) {
	case HDMI_QUANTIZATION_RANGE_DEFAULT:
		return "Default";
	case HDMI_QUANTIZATION_RANGE_LIMITED:
		return "Limited";
	case HDMI_QUANTIZATION_RANGE_FULL:
		return "Full";
	case HDMI_QUANTIZATION_RANGE_RESERVED:
		return "Reserved";
	}
	return "Invalid";
}

static const char *hdmi_nups_get_name(enum hdmi_nups nups)
{
	switch (nups) {
	case HDMI_NUPS_UNKNOWN:
		return "Unknown Non-uniform Scaling";
	case HDMI_NUPS_HORIZONTAL:
		return "Horizontally Scaled";
	case HDMI_NUPS_VERTICAL:
		return "Vertically Scaled";
	case HDMI_NUPS_BOTH:
		return "Horizontally and Vertically Scaled";
	}
	return "Invalid";
}

static const char *
hdmi_ycc_quantization_range_get_name(enum hdmi_ycc_quantization_range qrange)
{
	switch (qrange) {
	case HDMI_YCC_QUANTIZATION_RANGE_LIMITED:
		return "Limited";
	case HDMI_YCC_QUANTIZATION_RANGE_FULL:
		return "Full";
	}
	return "Invalid";
}

static const char *
hdmi_content_type_get_name(enum hdmi_content_type content_type)
{
	switch (content_type) {
	case HDMI_CONTENT_TYPE_GRAPHICS:
		return "Graphics";
	case HDMI_CONTENT_TYPE_PHOTO:
		return "Photo";
	case HDMI_CONTENT_TYPE_CINEMA:
		return "Cinema";
	case HDMI_CONTENT_TYPE_GAME:
		return "Game";
	}
	return "Invalid";
}

static void hdmi_avi_infoframe_log(const char *level, const char *dev,
				   const struct hdmi_avi_infoframe *frame)
{
	hdmi_infoframe_log_header(level, dev,
				  (const struct hdmi_any_infoframe *)frame);

	hdmi_log("    colorspace: %s\n",
		 hdmi_colorspace_get_name(frame->colorspace));
	hdmi_log("    scan mode: %s\n",
		 hdmi_scan_mode_get_name(frame->scan_mode));
	hdmi_log("    colorimetry: %s\n",
		 hdmi_colorimetry_get_name(frame->colorimetry));
	hdmi_log("    picture aspect: %s\n",
		 hdmi_picture_aspect_get_name(frame->picture_aspect));
	hdmi_log("    active aspect: %s\n",
		 hdmi_active_aspect_get_name(frame->active_aspect));
	hdmi_log("    itc: %s\n", frame->itc ? "IT Content" : "No Data");
	hdmi_log("    extended colorimetry: %s\n",
		 hdmi_extended_colorimetry_get_name(frame->extended_colorimetry));
	hdmi_log("    quantization range: %s\n",
		 hdmi_quantization_range_get_name(frame->quantization_range));
	hdmi_log("    nups: %s\n", hdmi_nups_get_name(frame->nups));
	hdmi_log("    video code: %u\n", frame->video_code);
	hdmi_log("    ycc quantization range: %s\n",
		 hdmi_ycc_quantization_range_get_name(frame->ycc_quantization_range));
	hdmi_log("    hdmi content type: %s\n",
		 hdmi_content_type_get_name(frame->content_type));
	hdmi_log("    pixel repeat: %u\n", frame->pixel_repeat);
	hdmi_log("    bar top %u, bottom %u, left %u, right %u\n",
		 frame->top_bar, frame->bottom_bar,
		 frame->left_bar, frame->right_bar);
}

static const char *hdmi_spd_sdi_get_name(enum hdmi_spd_sdi sdi)
{
	if ((int)sdi < 0 || (int)sdi > 0xff)
		return "Invalid";
	switch (sdi) {
	case HDMI_SPD_SDI_UNKNOWN:
		return "Unknown";
	case HDMI_SPD_SDI_DSTB:
		return "Digital STB";
	case HDMI_SPD_SDI_DVDP:
		return "DVD Player";
	case HDMI_SPD_SDI_DVHS:
		return "D-VHS";
	case HDMI_SPD_SDI_HDDVR:
		return "HDD Videorecorder";
	case HDMI_SPD_SDI_DVC:
		return "DVC";
	case HDMI_SPD_SDI_DSC:
		return "DSC";
	case HDMI_SPD_SDI_VCD:
		return "Video CD";
	case HDMI_SPD_SDI_GAME:
		return "Game";
	case HDMI_SPD_SDI_PC:
		return "PC General";
	case HDMI_SPD_SDI_BD:
		return "Blu-Ray Disc (BD)";
	case HDMI_SPD_SDI_SACD:
		return "Super Audio CD";
	case HDMI_SPD_SDI_HDDVD:
		return "HD DVD";
	case HDMI_SPD_SDI_PMP:
		return "PMP";
	}
	return "Reserved";
}

static void hdmi_spd_infoframe_log(const char *level, const char *dev,
				   const struct hdmi_spd_infoframe *frame)
{
	/* The fields need not be terminated: copied with one to spare. */
	char vendor[sizeof(frame->vendor) + 1];
	char product[sizeof(frame->product) + 1];

	hdmi_memset(vendor, 0, sizeof(vendor));
	hdmi_memset(product, 0, sizeof(product));
	hdmi_memcpy(vendor, frame->vendor, sizeof(frame->vendor));
	hdmi_memcpy(product, frame->product, sizeof(frame->product));

	hdmi_infoframe_log_header(level, dev,
				  (const struct hdmi_any_infoframe *)frame);

	hdmi_log("    vendor: %s\n", vendor);
	hdmi_log("    product: %s\n", product);
	hdmi_log("    source device information: %s (0x%x)\n",
		 hdmi_spd_sdi_get_name(frame->sdi), frame->sdi);
}

static const char *
hdmi_audio_coding_type_get_name(enum hdmi_audio_coding_type coding_type)
{
	switch (coding_type) {
	case HDMI_AUDIO_CODING_TYPE_STREAM:
		return "Refer to Stream Header";
	case HDMI_AUDIO_CODING_TYPE_PCM:
		return "PCM";
	case HDMI_AUDIO_CODING_TYPE_AC3:
		return "AC-3";
	case HDMI_AUDIO_CODING_TYPE_MPEG1:
		return "MPEG1";
	case HDMI_AUDIO_CODING_TYPE_MP3:
		return "MP3";
	case HDMI_AUDIO_CODING_TYPE_MPEG2:
		return "MPEG2";
	case HDMI_AUDIO_CODING_TYPE_AAC_LC:
		return "AAC";
	case HDMI_AUDIO_CODING_TYPE_DTS:
		return "DTS";
	case HDMI_AUDIO_CODING_TYPE_ATRAC:
		return "ATRAC";
	case HDMI_AUDIO_CODING_TYPE_DSD:
		return "One Bit Audio";
	case HDMI_AUDIO_CODING_TYPE_EAC3:
		return "Dolby Digital +";
	case HDMI_AUDIO_CODING_TYPE_DTS_HD:
		return "DTS-HD";
	case HDMI_AUDIO_CODING_TYPE_MLP:
		return "MAT (MLP)";
	case HDMI_AUDIO_CODING_TYPE_DST:
		return "DST";
	case HDMI_AUDIO_CODING_TYPE_WMA_PRO:
		return "WMA PRO";
	case HDMI_AUDIO_CODING_TYPE_CXT:
		return "Refer to CXT";
	}
	return "Invalid";
}

static const char *
hdmi_audio_sample_size_get_name(enum hdmi_audio_sample_size sample_size)
{
	switch (sample_size) {
	case HDMI_AUDIO_SAMPLE_SIZE_STREAM:
		return "Refer to Stream Header";
	case HDMI_AUDIO_SAMPLE_SIZE_16:
		return "16 bit";
	case HDMI_AUDIO_SAMPLE_SIZE_20:
		return "20 bit";
	case HDMI_AUDIO_SAMPLE_SIZE_24:
		return "24 bit";
	}
	return "Invalid";
}

static const char *
hdmi_audio_sample_frequency_get_name(enum hdmi_audio_sample_frequency freq)
{
	switch (freq) {
	case HDMI_AUDIO_SAMPLE_FREQUENCY_STREAM:
		return "Refer to Stream Header";
	case HDMI_AUDIO_SAMPLE_FREQUENCY_32000:
		return "32 kHz";
	case HDMI_AUDIO_SAMPLE_FREQUENCY_44100:
		return "44.1 kHz (CD)";
	case HDMI_AUDIO_SAMPLE_FREQUENCY_48000:
		return "48 kHz";
	case HDMI_AUDIO_SAMPLE_FREQUENCY_88200:
		return "88.2 kHz";
	case HDMI_AUDIO_SAMPLE_FREQUENCY_96000:
		return "96 kHz";
	case HDMI_AUDIO_SAMPLE_FREQUENCY_176400:
		return "176.4 kHz";
	case HDMI_AUDIO_SAMPLE_FREQUENCY_192000:
		return "192 kHz";
	}
	return "Invalid";
}

static const char *
hdmi_audio_coding_type_ext_get_name(enum hdmi_audio_coding_type_ext ctx)
{
	if ((int)ctx < 0 || (int)ctx > 0x1f)
		return "Invalid";

	switch (ctx) {
	case HDMI_AUDIO_CODING_TYPE_EXT_CT:
		return "Refer to CT";
	case HDMI_AUDIO_CODING_TYPE_EXT_HE_AAC:
		return "HE AAC";
	case HDMI_AUDIO_CODING_TYPE_EXT_HE_AAC_V2:
		return "HE AAC v2";
	case HDMI_AUDIO_CODING_TYPE_EXT_MPEG_SURROUND:
		return "MPEG SURROUND";
	case HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_HE_AAC:
		return "MPEG-4 HE AAC";
	case HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_HE_AAC_V2:
		return "MPEG-4 HE AAC v2";
	case HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_AAC_LC:
		return "MPEG-4 AAC LC";
	case HDMI_AUDIO_CODING_TYPE_EXT_DRA:
		return "DRA";
	case HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_HE_AAC_SURROUND:
		return "MPEG-4 HE AAC + MPEG Surround";
	case HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_AAC_LC_SURROUND:
		return "MPEG-4 AAC LC + MPEG Surround";
	}
	return "Reserved";
}

static void hdmi_audio_infoframe_log(const char *level, const char *dev,
				     const struct hdmi_audio_infoframe *frame)
{
	hdmi_infoframe_log_header(level, dev,
				  (const struct hdmi_any_infoframe *)frame);

	if (frame->channels)
		hdmi_log("    channels: %u\n", frame->channels - 1);
	else
		hdmi_log("    channels: Refer to stream header\n");
	hdmi_log("    coding type: %s\n",
		 hdmi_audio_coding_type_get_name(frame->coding_type));
	hdmi_log("    sample size: %s\n",
		 hdmi_audio_sample_size_get_name(frame->sample_size));
	hdmi_log("    sample frequency: %s\n",
		 hdmi_audio_sample_frequency_get_name(frame->sample_frequency));
	hdmi_log("    coding type ext: %s\n",
		 hdmi_audio_coding_type_ext_get_name(frame->coding_type_ext));
	hdmi_log("    channel allocation: 0x%x\n",
		 frame->channel_allocation);
	hdmi_log("    level shift value: %u dB\n",
		 frame->level_shift_value);
	hdmi_log("    downmix inhibit: %s\n",
		 frame->downmix_inhibit ? "Yes" : "No");
}

static void hdmi_drm_infoframe_log(const char *level, const char *dev,
				   const struct hdmi_drm_infoframe *frame)
{
	int i;

	hdmi_infoframe_log_header(level, dev,
				  (const struct hdmi_any_infoframe *)frame);
	hdmi_log("length: %d\n", frame->length);
	hdmi_log("metadata type: %d\n", frame->metadata_type);
	hdmi_log("eotf: %d\n", frame->eotf);
	for (i = 0; i < 3; i++) {
		hdmi_log("x[%d]: %d\n", i, frame->display_primaries[i].x);
		hdmi_log("y[%d]: %d\n", i, frame->display_primaries[i].y);
	}

	hdmi_log("white point x: %d\n", frame->white_point.x);
	hdmi_log("white point y: %d\n", frame->white_point.y);

	hdmi_log("max_display_mastering_luminance: %d\n",
		 frame->max_display_mastering_luminance);
	hdmi_log("min_display_mastering_luminance: %d\n",
		 frame->min_display_mastering_luminance);

	hdmi_log("max_cll: %d\n", frame->max_cll);
	hdmi_log("max_fall: %d\n", frame->max_fall);
}

static const char *
hdmi_3d_structure_get_name(enum hdmi_3d_structure s3d_struct)
{
	if ((int)s3d_struct < 0 || (int)s3d_struct > 0xf)
		return "Invalid";

	switch (s3d_struct) {
	case HDMI_3D_STRUCTURE_FRAME_PACKING:
		return "Frame Packing";
	case HDMI_3D_STRUCTURE_FIELD_ALTERNATIVE:
		return "Field Alternative";
	case HDMI_3D_STRUCTURE_LINE_ALTERNATIVE:
		return "Line Alternative";
	case HDMI_3D_STRUCTURE_SIDE_BY_SIDE_FULL:
		return "Side-by-side (Full)";
	case HDMI_3D_STRUCTURE_L_DEPTH:
		return "L + Depth";
	case HDMI_3D_STRUCTURE_L_DEPTH_GFX_GFX_DEPTH:
		return "L + Depth + Graphics + Graphics-depth";
	case HDMI_3D_STRUCTURE_TOP_AND_BOTTOM:
		return "Top-and-Bottom";
	case HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF:
		return "Side-by-side (Half)";
	default:
		break;
	}
	return "Reserved";
}

static void
hdmi_vendor_any_infoframe_log(const char *level, const char *dev,
			      const union hdmi_vendor_any_infoframe *frame)
{
	const struct hdmi_vendor_infoframe *hvf = &frame->hdmi;

	hdmi_infoframe_log_header(level, dev,
				  (const struct hdmi_any_infoframe *)frame);

	if (frame->any.oui != HDMI_IEEE_OUI) {
		hdmi_log("    not a HDMI vendor infoframe\n");
		return;
	}
	if (hvf->vic == 0 && hvf->s3d_struct == HDMI_3D_STRUCTURE_INVALID) {
		hdmi_log("    empty frame\n");
		return;
	}

	if (hvf->vic)
		hdmi_log("    HDMI VIC: %u\n", hvf->vic);
	if (hvf->s3d_struct != HDMI_3D_STRUCTURE_INVALID) {
		hdmi_log("    3D structure: %s\n",
			 hdmi_3d_structure_get_name(hvf->s3d_struct));
		if (hvf->s3d_struct >= HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF)
			hdmi_log("    3D extension data: %d\n",
				 hvf->s3d_ext_data);
	}
}

void hdmi_infoframe_log(const char *level, const char *dev,
			const union hdmi_infoframe *frame)
{
	switch (frame->any.type) {
	case HDMI_INFOFRAME_TYPE_AVI:
		hdmi_avi_infoframe_log(level, dev, &frame->avi);
		break;
	case HDMI_INFOFRAME_TYPE_SPD:
		hdmi_spd_infoframe_log(level, dev, &frame->spd);
		break;
	case HDMI_INFOFRAME_TYPE_AUDIO:
		hdmi_audio_infoframe_log(level, dev, &frame->audio);
		break;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		hdmi_vendor_any_infoframe_log(level, dev, &frame->vendor);
		break;
	case HDMI_INFOFRAME_TYPE_DRM:
		hdmi_drm_infoframe_log(level, dev, &frame->drm);
		break;
	}
}

#undef hdmi_log

/* ---- unpacking ------------------------------------------------------------ */

static int hdmi_avi_infoframe_unpack(struct hdmi_avi_infoframe *frame,
				     const void *buffer, size_t size)
{
	const u8 *ptr = buffer;

	if (size < HDMI_INFOFRAME_SIZE(AVI))
		return -EINVAL;

	if (ptr[0] != HDMI_INFOFRAME_TYPE_AVI ||
	    ptr[1] != 2 ||
	    ptr[2] != HDMI_AVI_INFOFRAME_SIZE)
		return -EINVAL;

	if (hdmi_infoframe_checksum(buffer, HDMI_INFOFRAME_SIZE(AVI)) != 0)
		return -EINVAL;

	hdmi_avi_infoframe_init(frame);

	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	frame->colorspace = (ptr[0] >> 5) & 0x3;
	if (ptr[0] & 0x10)
		frame->active_aspect = ptr[1] & 0xf;
	if (ptr[0] & 0x8) {
		frame->top_bar = (unsigned short)((ptr[6] << 8) | ptr[5]);
		frame->bottom_bar = (unsigned short)((ptr[8] << 8) | ptr[7]);
	}
	if (ptr[0] & 0x4) {
		frame->left_bar = (unsigned short)((ptr[10] << 8) | ptr[9]);
		frame->right_bar = (unsigned short)((ptr[12] << 8) | ptr[11]);
	}
	frame->scan_mode = ptr[0] & 0x3;

	frame->colorimetry = (ptr[1] >> 6) & 0x3;
	frame->picture_aspect = (ptr[1] >> 4) & 0x3;
	frame->active_aspect = ptr[1] & 0xf;

	frame->itc = ptr[2] & 0x80 ? true : false;
	frame->extended_colorimetry = (ptr[2] >> 4) & 0x7;
	frame->quantization_range = (ptr[2] >> 2) & 0x3;
	frame->nups = ptr[2] & 0x3;

	frame->video_code = ptr[3] & 0x7f;
	frame->ycc_quantization_range = (ptr[4] >> 6) & 0x3;
	frame->content_type = (ptr[4] >> 4) & 0x3;

	frame->pixel_repeat = ptr[4] & 0xf;

	return 0;
}

static int hdmi_spd_infoframe_unpack(struct hdmi_spd_infoframe *frame,
				     const void *buffer, size_t size)
{
	const u8 *ptr = buffer;
	int ret;

	if (size < HDMI_INFOFRAME_SIZE(SPD))
		return -EINVAL;

	if (ptr[0] != HDMI_INFOFRAME_TYPE_SPD ||
	    ptr[1] != 1 ||
	    ptr[2] != HDMI_SPD_INFOFRAME_SIZE) {
		return -EINVAL;
	}

	if (hdmi_infoframe_checksum(buffer, HDMI_INFOFRAME_SIZE(SPD)) != 0)
		return -EINVAL;

	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	/* the strings are bounded by their fields (hdmi_strnlen) */
	ret = hdmi_spd_infoframe_init(frame, (const char *)ptr,
				      (const char *)ptr + 8);
	if (ret)
		return ret;

	frame->sdi = ptr[24];

	return 0;
}

static int hdmi_audio_infoframe_unpack(struct hdmi_audio_infoframe *frame,
				       const void *buffer, size_t size)
{
	const u8 *ptr = buffer;
	int ret;

	if (size < HDMI_INFOFRAME_SIZE(AUDIO))
		return -EINVAL;

	if (ptr[0] != HDMI_INFOFRAME_TYPE_AUDIO ||
	    ptr[1] != 1 ||
	    ptr[2] != HDMI_AUDIO_INFOFRAME_SIZE) {
		return -EINVAL;
	}

	if (hdmi_infoframe_checksum(buffer, HDMI_INFOFRAME_SIZE(AUDIO)) != 0)
		return -EINVAL;

	ret = hdmi_audio_infoframe_init(frame);
	if (ret)
		return ret;

	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	frame->channels = ptr[0] & 0x7;
	frame->coding_type = (ptr[0] >> 4) & 0xf;
	frame->sample_size = ptr[1] & 0x3;
	frame->sample_frequency = (ptr[1] >> 2) & 0x7;
	frame->coding_type_ext = ptr[2] & 0x1f;
	frame->channel_allocation = ptr[3];
	frame->level_shift_value = (ptr[4] >> 3) & 0xf;
	frame->downmix_inhibit = ptr[4] & 0x80 ? true : false;

	return 0;
}

static int
hdmi_vendor_any_infoframe_unpack(union hdmi_vendor_any_infoframe *frame,
				 const void *buffer, size_t size)
{
	const u8 *ptr = buffer;
	size_t length;
	int ret;
	u8 hdmi_video_format;
	struct hdmi_vendor_infoframe *hvf = &frame->hdmi;

	if (size < HDMI_INFOFRAME_HEADER_SIZE)
		return -EINVAL;

	if (ptr[0] != HDMI_INFOFRAME_TYPE_VENDOR ||
	    ptr[1] != 1 ||
	    (ptr[2] != 4 && ptr[2] != 5 && ptr[2] != 6))
		return -EINVAL;

	length = ptr[2];

	if (size < HDMI_INFOFRAME_HEADER_SIZE + length)
		return -EINVAL;

	if (hdmi_infoframe_checksum(buffer,
				    HDMI_INFOFRAME_HEADER_SIZE + length) != 0)
		return -EINVAL;

	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	/* HDMI OUI */
	if ((ptr[0] != 0x03) ||
	    (ptr[1] != 0x0c) ||
	    (ptr[2] != 0x00))
		return -EINVAL;

	hdmi_video_format = ptr[3] >> 5;

	if (hdmi_video_format > 0x2)
		return -EINVAL;

	ret = hdmi_vendor_infoframe_init(hvf);
	if (ret)
		return ret;

	hvf->length = (unsigned char)length;

	if (hdmi_video_format == 0x2) {
		if (length != 5 && length != 6)
			return -EINVAL;
		hvf->s3d_struct = ptr[4] >> 4;
		if (hvf->s3d_struct >= HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF) {
			if (length != 6)
				return -EINVAL;
			hvf->s3d_ext_data = ptr[5] >> 4;
		}
	} else if (hdmi_video_format == 0x1) {
		if (length != 5)
			return -EINVAL;
		hvf->vic = ptr[4];
	} else {
		if (length != 4)
			return -EINVAL;
	}

	return 0;
}

int hdmi_drm_infoframe_unpack_only(struct hdmi_drm_infoframe *frame,
				   const void *buffer, size_t size)
{
	const u8 *ptr = buffer;
	const u8 *temp;
	u8 x_lsb, x_msb;
	u8 y_lsb, y_msb;
	int ret;
	int i;

	if (size < HDMI_DRM_INFOFRAME_SIZE)
		return -EINVAL;

	ret = hdmi_drm_infoframe_init(frame);
	if (ret)
		return ret;

	frame->eotf = ptr[0] & 0x7;
	frame->metadata_type = ptr[1] & 0x7;

	temp = ptr + 2;
	for (i = 0; i < 3; i++) {
		x_lsb = *temp++;
		x_msb = *temp++;
		frame->display_primaries[i].x = (u16)((x_msb << 8) | x_lsb);
		y_lsb = *temp++;
		y_msb = *temp++;
		frame->display_primaries[i].y = (u16)((y_msb << 8) | y_lsb);
	}

	frame->white_point.x = (u16)((ptr[15] << 8) | ptr[14]);
	frame->white_point.y = (u16)((ptr[17] << 8) | ptr[16]);

	frame->max_display_mastering_luminance = (u16)((ptr[19] << 8) | ptr[18]);
	frame->min_display_mastering_luminance = (u16)((ptr[21] << 8) | ptr[20]);
	frame->max_cll = (u16)((ptr[23] << 8) | ptr[22]);
	frame->max_fall = (u16)((ptr[25] << 8) | ptr[24]);

	return 0;
}

static int hdmi_drm_infoframe_unpack(struct hdmi_drm_infoframe *frame,
				     const void *buffer, size_t size)
{
	const u8 *ptr = buffer;
	int ret;

	if (size < HDMI_INFOFRAME_SIZE(DRM))
		return -EINVAL;

	if (ptr[0] != HDMI_INFOFRAME_TYPE_DRM ||
	    ptr[1] != 1 ||
	    ptr[2] != HDMI_DRM_INFOFRAME_SIZE)
		return -EINVAL;

	if (hdmi_infoframe_checksum(buffer, HDMI_INFOFRAME_SIZE(DRM)) != 0)
		return -EINVAL;

	ret = hdmi_drm_infoframe_unpack_only(frame, ptr + HDMI_INFOFRAME_HEADER_SIZE,
					     size - HDMI_INFOFRAME_HEADER_SIZE);
	return ret;
}

int hdmi_infoframe_unpack(union hdmi_infoframe *frame,
			  const void *buffer, size_t size)
{
	int ret;
	const u8 *ptr = buffer;

	if (size < HDMI_INFOFRAME_HEADER_SIZE)
		return -EINVAL;

	switch (ptr[0]) {
	case HDMI_INFOFRAME_TYPE_AVI:
		ret = hdmi_avi_infoframe_unpack(&frame->avi, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_DRM:
		ret = hdmi_drm_infoframe_unpack(&frame->drm, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_SPD:
		ret = hdmi_spd_infoframe_unpack(&frame->spd, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_AUDIO:
		ret = hdmi_audio_infoframe_unpack(&frame->audio, buffer, size);
		break;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		ret = hdmi_vendor_any_infoframe_unpack(&frame->vendor, buffer, size);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* ==== infoframes for a mode on a connector ============================== */

/* An HDMI 2.0 sink: one with SCDC, or one that takes YCbCr 4:2:0.  A
 * NULL connector (a bridge driver with none at hand) counts as one. */
static bool is_hdmi2_sink(const struct drm_connector *connector)
{
	if (!connector)
		return true;

	return connector->display_info.hdmi.scdc.supported ||
	       (connector->display_info.color_formats & DRM_COLOR_FORMAT_YCBCR420);
}

/* Does the sink's CTA video data block list this code? */
static bool cta_vdb_has_vic(const struct drm_connector *connector, u8 vic)
{
	const struct drm_display_info *info = &connector->display_info;
	int i;

	if (!vic || !info->vics)
		return false;

	for (i = 0; i < info->vics_len; i++) {
		if (info->vics[i] == vic)
			return true;
	}

	return false;
}

static u8 drm_mode_hdmi_vic(const struct drm_connector *connector,
			    const struct drm_display_mode *mode)
{
	bool has_hdmi_infoframe = connector ?
		connector->display_info.has_hdmi_infoframe : false;

	if (!has_hdmi_infoframe)
		return 0;

	/* No HDMI VIC when signalling 3D video format */
	if (mode->flags & DRM_MODE_FLAG_3D_MASK)
		return 0;

	return drm_match_hdmi_mode(mode);
}

static u8 drm_mode_avi_cea_vic(const struct drm_connector *connector,
			       const struct drm_display_mode *mode)
{
	/* A mode that is one of the HDMI 1.4b 4k modes is signalled by its
	 * HDMI VIC in the vendor infoframe, every other one by its CEA VIC
	 * in the AVI infoframe. */
	if (drm_mode_hdmi_vic(connector, mode))
		return 0;

	return drm_match_cea_mode(mode);
}

/* No VIC defined by HDMI 2.0 in the AVI infoframe of a sink that conforms
 * to HDMI 1.4:
 *
 *   HDMI 1.4 (CTA-861-D) VIC range: [1..64]
 *   HDMI 2.0 (CTA-861-F) VIC range: [1..107]
 *
 * unless the sink lists the VIC in its CTA video data block -- then it
 * knows the code, whatever its HDMI version. */
static u8 vic_for_avi_infoframe(const struct drm_connector *connector, u8 vic)
{
	if (!is_hdmi2_sink(connector) && vic > 64 &&
	    !cta_vdb_has_vic(connector, vic))
		return 0;

	return vic;
}

int drm_hdmi_avi_infoframe_from_display_mode(struct hdmi_avi_infoframe *frame,
					     const struct drm_connector *connector,
					     const struct drm_display_mode *mode)
{
	enum hdmi_picture_aspect picture_aspect;
	u8 vic, hdmi_vic;

	if (!frame || !mode)
		return -EINVAL;

	hdmi_avi_infoframe_init(frame);

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		frame->pixel_repeat = 1;

	vic = drm_mode_avi_cea_vic(connector, mode);
	hdmi_vic = drm_mode_hdmi_vic(connector, mode);

	frame->picture_aspect = HDMI_PICTURE_ASPECT_NONE;

	/* The content type and IT content flags start at their defaults: a
	 * driver fills them from the connector state when it has one
	 * (drm_hdmi_avi_infoframe_content_type). */
	frame->content_type = HDMI_CONTENT_TYPE_GRAPHICS;
	frame->itc = 0;

	/* The picture aspect ratio the mode asks for, else the one its CEA
	 * or HDMI video code has. */
	picture_aspect = (enum hdmi_picture_aspect)mode->picture_aspect_ratio;
	if (picture_aspect == HDMI_PICTURE_ASPECT_NONE) {
		if (vic)
			picture_aspect = drm_get_cea_aspect_ratio(vic);
		else if (hdmi_vic)
			picture_aspect = drm_get_hdmi_aspect_ratio(hdmi_vic);
	}

	/* The infoframe can't convey anything but none, 4:3 and 16:9, so a
	 * wider aspect is only satisfied by a VIC that implies it. */
	if (picture_aspect > HDMI_PICTURE_ASPECT_16_9) {
		if (vic) {
			if ((int)picture_aspect != drm_get_cea_aspect_ratio(vic))
				return -EINVAL;
		} else if (hdmi_vic) {
			if ((int)picture_aspect != drm_get_hdmi_aspect_ratio(hdmi_vic))
				return -EINVAL;
		} else {
			return -EINVAL;
		}

		picture_aspect = HDMI_PICTURE_ASPECT_NONE;
	}

	frame->video_code = vic_for_avi_infoframe(connector, vic);
	frame->picture_aspect = picture_aspect;
	frame->active_aspect = HDMI_ACTIVE_ASPECT_PICTURE;
	frame->scan_mode = HDMI_SCAN_MODE_UNDERSCAN;

	return 0;
}

void drm_hdmi_avi_infoframe_quant_range(struct hdmi_avi_infoframe *frame,
					const struct drm_connector *connector,
					const struct drm_display_mode *mode,
					enum hdmi_quantization_range rgb_quant_range)
{
	/* Without a connector nothing says the sink takes the Q bit. */
	int selectable = connector &&
			 connector->display_info.rgb_quant_range_selectable;

	/* CEA-861:
	 * "A Source shall not send a non-zero Q value that does not correspond
	 *  to the default RGB Quantization Range for the transmitted Picture
	 *  unless the Sink indicates support for the Q bit in a Video
	 *  Capabilities Data Block."
	 *
	 * HDMI 2.0 recommends sending non-zero Q when it does match the
	 * default RGB quantization range for the mode, even when QS=0. */
	if (selectable ||
	    rgb_quant_range == drm_default_rgb_quant_range(mode))
		frame->quantization_range = rgb_quant_range;
	else
		frame->quantization_range = HDMI_QUANTIZATION_RANGE_DEFAULT;

	/* CEA-861-F:
	 * "When transmitting any RGB colorimetry, the Source should set the
	 *  YQ-field to match the RGB Quantization Range being transmitted
	 *  (e.g., when Limited Range RGB, set YQ=0 or when Full Range RGB,
	 *  set YQ=1) and the Sink shall ignore the YQ-field."
	 *
	 * Some sinks (VIZ Model 67/E261VA, for one) are confused by a non-zero
	 * YQ with RGB.  Nothing tells which CEA-861 version a sink follows,
	 * so non-zero YQ goes to HDMI 2.0 sinks only, HDMI 2.0 being based
	 * on CEA-861-F. */
	if (!is_hdmi2_sink(connector) ||
	    rgb_quant_range == HDMI_QUANTIZATION_RANGE_LIMITED)
		frame->ycc_quantization_range =
			HDMI_YCC_QUANTIZATION_RANGE_LIMITED;
	else
		frame->ycc_quantization_range =
			HDMI_YCC_QUANTIZATION_RANGE_FULL;
}

static enum hdmi_3d_structure
s3d_structure_from_display_mode(const struct drm_display_mode *mode)
{
	u32 layout = mode->flags & DRM_MODE_FLAG_3D_MASK;

	switch (layout) {
	case DRM_MODE_FLAG_3D_FRAME_PACKING:
		return HDMI_3D_STRUCTURE_FRAME_PACKING;
	case DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE:
		return HDMI_3D_STRUCTURE_FIELD_ALTERNATIVE;
	case DRM_MODE_FLAG_3D_LINE_ALTERNATIVE:
		return HDMI_3D_STRUCTURE_LINE_ALTERNATIVE;
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL:
		return HDMI_3D_STRUCTURE_SIDE_BY_SIDE_FULL;
	case DRM_MODE_FLAG_3D_L_DEPTH:
		return HDMI_3D_STRUCTURE_L_DEPTH;
	case DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH:
		return HDMI_3D_STRUCTURE_L_DEPTH_GFX_GFX_DEPTH;
	case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
		return HDMI_3D_STRUCTURE_TOP_AND_BOTTOM;
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
		return HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF;
	default:
		return HDMI_3D_STRUCTURE_INVALID;
	}
}

int drm_hdmi_vendor_infoframe_from_display_mode(struct hdmi_vendor_infoframe *frame,
						const struct drm_connector *connector,
						const struct drm_display_mode *mode)
{
	/* A NULL connector (a bridge driver with none at hand) has no HDMI
	 * vendor block to go by. */
	bool has_hdmi_infoframe = connector ?
		connector->display_info.has_hdmi_infoframe : false;
	int err;

	if (!frame || !mode)
		return -EINVAL;

	if (!has_hdmi_infoframe)
		return -EINVAL;

	err = hdmi_vendor_infoframe_init(frame);
	if (err < 0)
		return err;

	/* The frame goes out even when it says nothing (no HDMI VIC, no 3D)
	 * to a sink that understands it, as HDMI 2.0 appendix F suggests:
	 * some sinks do not switch from 3D back to 2D when the source just
	 * stops sending the frame. */
	frame->vic = drm_mode_hdmi_vic(connector, mode);
	frame->s3d_struct = s3d_structure_from_display_mode(mode);

	return 0;
}
