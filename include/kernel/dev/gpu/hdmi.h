// LikeOS -- HDMI infoframes: the packets that describe the picture and the
// sound to an HDMI sink.
//
// An HDMI source sends, during the blanking intervals, small data-island
// packets that tell the sink what it is looking at: the AVI infoframe (pixel
// encoding, colorimetry, picture and active aspect, quantisation range, the
// CEA video code), the vendor infoframe of the HDMI licensing OUI (the HDMI
// 1.4 4k video codes and the stereo 3D layout), the source product
// description, the audio infoframe and the Dynamic Range and Mastering
// infoframe that carries HDR static metadata.  Each one is a 4-byte header
// (type, version, payload length, checksum) and a payload; the checksum
// makes the whole packet sum to zero modulo 256.
//
// Here is the structured form of each infoframe, with:
//  - *_init()       a frame with the right type, version and length;
//  - *_check()      validate it and update the fields derived from others;
//  - *_pack_only()  the bytes the transmitter is loaded with (no update);
//  - *_pack()       check, then pack;
//  - hdmi_infoframe_unpack()  the structured form of received bytes;
//  - hdmi_infoframe_log()     a frame field by field in the kernel log;
// and the display-manager side: the AVI and vendor infoframes for a mode
// on a connector, and the AVI quantisation range for what the sink takes
// (drm_hdmi_infoframe.c).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Avionic Design's code: MIT
// Portions Copyright (C) 2012 Avionic Design GmbH

#ifndef KERNEL_DEV_GPU_HDMI_H
#define KERNEL_DEV_GPU_HDMI_H

#include <kernel/uapi/types.h>
#include <kernel/dev/gpu/gpu_util.h>
/* enum hdmi_quantization_range (the EDID code reports the default range
 * of a mode in it) */
#include <kernel/dev/gpu/drm_edid.h>
/* struct hdr_static_metadata, struct hdr_sink_metadata */
#include <kernel/dev/gpu/drm_display_info.h>

/* The pack functions return a byte count or a negative errno. */
typedef long ssize_t;

enum hdmi_packet_type {
	HDMI_PACKET_TYPE_NULL = 0x00,
	HDMI_PACKET_TYPE_AUDIO_CLOCK_REGEN = 0x01,
	HDMI_PACKET_TYPE_AUDIO_SAMPLE = 0x02,
	HDMI_PACKET_TYPE_GENERAL_CONTROL = 0x03,
	HDMI_PACKET_TYPE_ACP = 0x04,
	HDMI_PACKET_TYPE_ISRC1 = 0x05,
	HDMI_PACKET_TYPE_ISRC2 = 0x06,
	HDMI_PACKET_TYPE_ONE_BIT_AUDIO_SAMPLE = 0x07,
	HDMI_PACKET_TYPE_DST_AUDIO = 0x08,
	HDMI_PACKET_TYPE_HBR_AUDIO_STREAM = 0x09,
	HDMI_PACKET_TYPE_GAMUT_METADATA = 0x0a,
	/* + enum hdmi_infoframe_type */
};

enum hdmi_infoframe_type {
	HDMI_INFOFRAME_TYPE_VENDOR = 0x81,
	HDMI_INFOFRAME_TYPE_AVI = 0x82,
	HDMI_INFOFRAME_TYPE_SPD = 0x83,
	HDMI_INFOFRAME_TYPE_AUDIO = 0x84,
	HDMI_INFOFRAME_TYPE_DRM = 0x87,
};

/* The TMDS character rates of the HDMI versions, in Hz. */
#define HDMI_TMDS_CHAR_RATE_MIN_HZ 25000000
#define HDMI_1_0_TMDS_CHAR_RATE_MAX_HZ 165000000
#define HDMI_1_3_TMDS_CHAR_RATE_MAX_HZ 340000000
#define HDMI_2_0_TMDS_CHAR_RATE_MAX_HZ 600000000

/* (The EDID code's private header spells these two the same way.) */
#define HDMI_IEEE_OUI 0x000c03
#define HDMI_FORUM_IEEE_OUI 0xc45dd8
#define HDMI_INFOFRAME_HEADER_SIZE 4
#define HDMI_AVI_INFOFRAME_SIZE 13
#define HDMI_SPD_INFOFRAME_SIZE 25
#define HDMI_AUDIO_INFOFRAME_SIZE 10
#define HDMI_DRM_INFOFRAME_SIZE 26
#define HDMI_VENDOR_INFOFRAME_SIZE 4

/* HDMI 1.3a table 5-14: the largest InfoFrame_length is 27, without the
 * packet header and the checksum byte.  The checksum is counted in
 * HDMI_INFOFRAME_HEADER_SIZE, so HDMI_INFOFRAME_SIZE(MAX) is the largest
 * buffer any infoframe needs. */
#define HDMI_MAX_INFOFRAME_SIZE 27

#define HDMI_INFOFRAME_SIZE(type) \
	(HDMI_INFOFRAME_HEADER_SIZE + HDMI_##type##_INFOFRAME_SIZE)

struct hdmi_any_infoframe {
	enum hdmi_infoframe_type type;
	unsigned char version;
	unsigned char length;
};

enum hdmi_colorspace {
	HDMI_COLORSPACE_RGB,
	HDMI_COLORSPACE_YUV422,
	HDMI_COLORSPACE_YUV444,
	HDMI_COLORSPACE_YUV420,
	HDMI_COLORSPACE_RESERVED4,
	HDMI_COLORSPACE_RESERVED5,
	HDMI_COLORSPACE_RESERVED6,
	HDMI_COLORSPACE_IDO_DEFINED,
};

enum hdmi_scan_mode {
	HDMI_SCAN_MODE_NONE,
	HDMI_SCAN_MODE_OVERSCAN,
	HDMI_SCAN_MODE_UNDERSCAN,
	HDMI_SCAN_MODE_RESERVED,
};

enum hdmi_colorimetry {
	HDMI_COLORIMETRY_NONE,
	HDMI_COLORIMETRY_ITU_601,
	HDMI_COLORIMETRY_ITU_709,
	HDMI_COLORIMETRY_EXTENDED,
};

/* The same values as DRM_MODE_PICTURE_ASPECT_* (a mode's
 * picture_aspect_ratio), which the infoframe code relies on. */
enum hdmi_picture_aspect {
	HDMI_PICTURE_ASPECT_NONE,
	HDMI_PICTURE_ASPECT_4_3,
	HDMI_PICTURE_ASPECT_16_9,
	HDMI_PICTURE_ASPECT_64_27,
	HDMI_PICTURE_ASPECT_256_135,
	HDMI_PICTURE_ASPECT_RESERVED,
};

enum hdmi_active_aspect {
	HDMI_ACTIVE_ASPECT_16_9_TOP = 2,
	HDMI_ACTIVE_ASPECT_14_9_TOP = 3,
	HDMI_ACTIVE_ASPECT_16_9_CENTER = 4,
	HDMI_ACTIVE_ASPECT_PICTURE = 8,
	HDMI_ACTIVE_ASPECT_4_3 = 9,
	HDMI_ACTIVE_ASPECT_16_9 = 10,
	HDMI_ACTIVE_ASPECT_14_9 = 11,
	HDMI_ACTIVE_ASPECT_4_3_SP_14_9 = 13,
	HDMI_ACTIVE_ASPECT_16_9_SP_14_9 = 14,
	HDMI_ACTIVE_ASPECT_16_9_SP_4_3 = 15,
};

enum hdmi_extended_colorimetry {
	HDMI_EXTENDED_COLORIMETRY_XV_YCC_601,
	HDMI_EXTENDED_COLORIMETRY_XV_YCC_709,
	HDMI_EXTENDED_COLORIMETRY_S_YCC_601,
	HDMI_EXTENDED_COLORIMETRY_OPYCC_601,
	HDMI_EXTENDED_COLORIMETRY_OPRGB,

	/* The following EC values are only defined in CEA-861-F. */
	HDMI_EXTENDED_COLORIMETRY_BT2020_CONST_LUM,
	HDMI_EXTENDED_COLORIMETRY_BT2020,
	HDMI_EXTENDED_COLORIMETRY_RESERVED,
};

/* enum hdmi_quantization_range: drm_edid.h. */

/* non-uniform picture scaling */
enum hdmi_nups {
	HDMI_NUPS_UNKNOWN,
	HDMI_NUPS_HORIZONTAL,
	HDMI_NUPS_VERTICAL,
	HDMI_NUPS_BOTH,
};

enum hdmi_ycc_quantization_range {
	HDMI_YCC_QUANTIZATION_RANGE_LIMITED,
	HDMI_YCC_QUANTIZATION_RANGE_FULL,
};

enum hdmi_content_type {
	HDMI_CONTENT_TYPE_GRAPHICS,
	HDMI_CONTENT_TYPE_PHOTO,
	HDMI_CONTENT_TYPE_CINEMA,
	HDMI_CONTENT_TYPE_GAME,
};

/* The EDID parser's private header gives the metadata type and the EOTF
 * values as macros of the same names and values; a file that includes it
 * before this header gets the enumerations without enumerators. */
enum hdmi_metadata_type {
#ifndef HDMI_STATIC_METADATA_TYPE1
	HDMI_STATIC_METADATA_TYPE1 = 0,
#else
	HDMI_METADATA_TYPE_FIRST__ = HDMI_STATIC_METADATA_TYPE1,
#endif
};

enum hdmi_eotf {
#ifndef HDMI_EOTF_TRADITIONAL_GAMMA_SDR
	HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
	HDMI_EOTF_TRADITIONAL_GAMMA_HDR,
	HDMI_EOTF_SMPTE_ST2084,
	HDMI_EOTF_BT_2100_HLG,
#else
	HDMI_EOTF_FIRST__ = HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
	HDMI_EOTF_LAST__ = HDMI_EOTF_BT_2100_HLG,
#endif
};

struct hdmi_avi_infoframe {
	enum hdmi_infoframe_type type;
	unsigned char version;
	unsigned char length;
	bool itc;
	unsigned char pixel_repeat;
	enum hdmi_colorspace colorspace;
	enum hdmi_scan_mode scan_mode;
	enum hdmi_colorimetry colorimetry;
	enum hdmi_picture_aspect picture_aspect;
	enum hdmi_active_aspect active_aspect;
	enum hdmi_extended_colorimetry extended_colorimetry;
	enum hdmi_quantization_range quantization_range;
	enum hdmi_nups nups;
	unsigned char video_code;
	enum hdmi_ycc_quantization_range ycc_quantization_range;
	enum hdmi_content_type content_type;
	unsigned short top_bar;
	unsigned short bottom_bar;
	unsigned short left_bar;
	unsigned short right_bar;
};

/* The Dynamic Range and Mastering infoframe (CTA-861-G). */
struct hdmi_drm_infoframe {
	enum hdmi_infoframe_type type;
	unsigned char version;
	unsigned char length;
	enum hdmi_eotf eotf;
	enum hdmi_metadata_type metadata_type;
	struct {
		u16 x, y;
	} display_primaries[3];
	struct {
		u16 x, y;
	} white_point;
	u16 max_display_mastering_luminance;
	u16 min_display_mastering_luminance;
	u16 max_cll;
	u16 max_fall;
};

void hdmi_avi_infoframe_init(struct hdmi_avi_infoframe *frame);
ssize_t hdmi_avi_infoframe_pack(struct hdmi_avi_infoframe *frame, void *buffer,
				size_t size);
ssize_t hdmi_avi_infoframe_pack_only(const struct hdmi_avi_infoframe *frame,
				     void *buffer, size_t size);
int hdmi_avi_infoframe_check(struct hdmi_avi_infoframe *frame);
int hdmi_drm_infoframe_init(struct hdmi_drm_infoframe *frame);
ssize_t hdmi_drm_infoframe_pack(struct hdmi_drm_infoframe *frame, void *buffer,
				size_t size);
ssize_t hdmi_drm_infoframe_pack_only(const struct hdmi_drm_infoframe *frame,
				     void *buffer, size_t size);
int hdmi_drm_infoframe_check(struct hdmi_drm_infoframe *frame);
/* The DRM infoframe from its 26 data bytes alone (no header). */
int hdmi_drm_infoframe_unpack_only(struct hdmi_drm_infoframe *frame,
				   const void *buffer, size_t size);

enum hdmi_spd_sdi {
	HDMI_SPD_SDI_UNKNOWN,
	HDMI_SPD_SDI_DSTB,
	HDMI_SPD_SDI_DVDP,
	HDMI_SPD_SDI_DVHS,
	HDMI_SPD_SDI_HDDVR,
	HDMI_SPD_SDI_DVC,
	HDMI_SPD_SDI_DSC,
	HDMI_SPD_SDI_VCD,
	HDMI_SPD_SDI_GAME,
	HDMI_SPD_SDI_PC,
	HDMI_SPD_SDI_BD,
	HDMI_SPD_SDI_SACD,
	HDMI_SPD_SDI_HDDVD,
	HDMI_SPD_SDI_PMP,
};

struct hdmi_spd_infoframe {
	enum hdmi_infoframe_type type;
	unsigned char version;
	unsigned char length;
	char vendor[8];
	char product[16];
	enum hdmi_spd_sdi sdi;
};

int hdmi_spd_infoframe_init(struct hdmi_spd_infoframe *frame,
			    const char *vendor, const char *product);
ssize_t hdmi_spd_infoframe_pack(struct hdmi_spd_infoframe *frame, void *buffer,
				size_t size);
ssize_t hdmi_spd_infoframe_pack_only(const struct hdmi_spd_infoframe *frame,
				     void *buffer, size_t size);
int hdmi_spd_infoframe_check(struct hdmi_spd_infoframe *frame);

enum hdmi_audio_coding_type {
	HDMI_AUDIO_CODING_TYPE_STREAM,
	HDMI_AUDIO_CODING_TYPE_PCM,
	HDMI_AUDIO_CODING_TYPE_AC3,
	HDMI_AUDIO_CODING_TYPE_MPEG1,
	HDMI_AUDIO_CODING_TYPE_MP3,
	HDMI_AUDIO_CODING_TYPE_MPEG2,
	HDMI_AUDIO_CODING_TYPE_AAC_LC,
	HDMI_AUDIO_CODING_TYPE_DTS,
	HDMI_AUDIO_CODING_TYPE_ATRAC,
	HDMI_AUDIO_CODING_TYPE_DSD,
	HDMI_AUDIO_CODING_TYPE_EAC3,
	HDMI_AUDIO_CODING_TYPE_DTS_HD,
	HDMI_AUDIO_CODING_TYPE_MLP,
	HDMI_AUDIO_CODING_TYPE_DST,
	HDMI_AUDIO_CODING_TYPE_WMA_PRO,
	HDMI_AUDIO_CODING_TYPE_CXT,
};

enum hdmi_audio_sample_size {
	HDMI_AUDIO_SAMPLE_SIZE_STREAM,
	HDMI_AUDIO_SAMPLE_SIZE_16,
	HDMI_AUDIO_SAMPLE_SIZE_20,
	HDMI_AUDIO_SAMPLE_SIZE_24,
};

enum hdmi_audio_sample_frequency {
	HDMI_AUDIO_SAMPLE_FREQUENCY_STREAM,
	HDMI_AUDIO_SAMPLE_FREQUENCY_32000,
	HDMI_AUDIO_SAMPLE_FREQUENCY_44100,
	HDMI_AUDIO_SAMPLE_FREQUENCY_48000,
	HDMI_AUDIO_SAMPLE_FREQUENCY_88200,
	HDMI_AUDIO_SAMPLE_FREQUENCY_96000,
	HDMI_AUDIO_SAMPLE_FREQUENCY_176400,
	HDMI_AUDIO_SAMPLE_FREQUENCY_192000,
};

enum hdmi_audio_coding_type_ext {
	/* Refer to Audio Coding Type (CT) field in Data Byte 1 */
	HDMI_AUDIO_CODING_TYPE_EXT_CT,

	/* The next three CXT values are defined in CEA-861-E only.  They
	 * do not exist in older versions, and in CEA-861-F they are defined
	 * as 'Not in use'. */
	HDMI_AUDIO_CODING_TYPE_EXT_HE_AAC,
	HDMI_AUDIO_CODING_TYPE_EXT_HE_AAC_V2,
	HDMI_AUDIO_CODING_TYPE_EXT_MPEG_SURROUND,

	/* The following CXT values are only defined in CEA-861-F. */
	HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_HE_AAC,
	HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_HE_AAC_V2,
	HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_AAC_LC,
	HDMI_AUDIO_CODING_TYPE_EXT_DRA,
	HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_HE_AAC_SURROUND,
	HDMI_AUDIO_CODING_TYPE_EXT_MPEG4_AAC_LC_SURROUND = 10,
};

struct hdmi_audio_infoframe {
	enum hdmi_infoframe_type type;
	unsigned char version;
	unsigned char length;
	unsigned char channels;
	enum hdmi_audio_coding_type coding_type;
	enum hdmi_audio_sample_size sample_size;
	enum hdmi_audio_sample_frequency sample_frequency;
	enum hdmi_audio_coding_type_ext coding_type_ext;
	unsigned char channel_allocation;
	unsigned char level_shift_value;
	bool downmix_inhibit;
};

int hdmi_audio_infoframe_init(struct hdmi_audio_infoframe *frame);
ssize_t hdmi_audio_infoframe_pack(struct hdmi_audio_infoframe *frame,
				  void *buffer, size_t size);
ssize_t hdmi_audio_infoframe_pack_only(const struct hdmi_audio_infoframe *frame,
				       void *buffer, size_t size);
int hdmi_audio_infoframe_check(const struct hdmi_audio_infoframe *frame);
/* The five audio infoframe payload bytes alone, for a transport that
 * frames them itself (a DisplayPort secondary data packet carries them
 * after its own header).  `buffer' takes HDMI_AUDIO_INFOFRAME_SIZE bytes;
 * returns 0 or -EINVAL for a frame that fails the check. */
int hdmi_audio_infoframe_pack_payload_only(const struct hdmi_audio_infoframe *frame,
					   u8 *buffer, size_t size);

/* The audio infoframe as a DisplayPort secondary data packet (struct
 * dp_sdp, drm_dp.h): the SDP header with `dp_version' in it, the payload
 * in the data bytes.  The bytes written (frame length + 4 header bytes)
 * or a negative errno. */
struct dp_sdp;
ssize_t hdmi_audio_infoframe_pack_for_dp(const struct hdmi_audio_infoframe *frame,
					 struct dp_sdp *sdp, u8 dp_version);

enum hdmi_3d_structure {
	HDMI_3D_STRUCTURE_INVALID = -1,
	HDMI_3D_STRUCTURE_FRAME_PACKING = 0,
	HDMI_3D_STRUCTURE_FIELD_ALTERNATIVE,
	HDMI_3D_STRUCTURE_LINE_ALTERNATIVE,
	HDMI_3D_STRUCTURE_SIDE_BY_SIDE_FULL,
	HDMI_3D_STRUCTURE_L_DEPTH,
	HDMI_3D_STRUCTURE_L_DEPTH_GFX_GFX_DEPTH,
	HDMI_3D_STRUCTURE_TOP_AND_BOTTOM,
	HDMI_3D_STRUCTURE_SIDE_BY_SIDE_HALF = 8,
};

struct hdmi_vendor_infoframe {
	enum hdmi_infoframe_type type;
	unsigned char version;
	unsigned char length;
	unsigned int oui;
	u8 vic;
	enum hdmi_3d_structure s3d_struct;
	unsigned int s3d_ext_data;
};

/* struct hdr_static_metadata and struct hdr_sink_metadata (what the sink's
 * EDID says of HDR): drm_display_info.h. */

int hdmi_vendor_infoframe_init(struct hdmi_vendor_infoframe *frame);
ssize_t hdmi_vendor_infoframe_pack(struct hdmi_vendor_infoframe *frame,
				   void *buffer, size_t size);
ssize_t hdmi_vendor_infoframe_pack_only(const struct hdmi_vendor_infoframe *frame,
					void *buffer, size_t size);
int hdmi_vendor_infoframe_check(struct hdmi_vendor_infoframe *frame);

union hdmi_vendor_any_infoframe {
	struct {
		enum hdmi_infoframe_type type;
		unsigned char version;
		unsigned char length;
		unsigned int oui;
	} any;
	struct hdmi_vendor_infoframe hdmi;
};

/* Every infoframe: they all begin with the same header, which says which
 * one it is, so the generic functions below dispatch on any.type. */
union hdmi_infoframe {
	struct hdmi_any_infoframe any;
	struct hdmi_avi_infoframe avi;
	struct hdmi_spd_infoframe spd;
	union hdmi_vendor_any_infoframe vendor;
	struct hdmi_audio_infoframe audio;
	struct hdmi_drm_infoframe drm;
};

ssize_t hdmi_infoframe_pack(union hdmi_infoframe *frame, void *buffer,
			    size_t size);
ssize_t hdmi_infoframe_pack_only(const union hdmi_infoframe *frame,
				 void *buffer, size_t size);
int hdmi_infoframe_unpack(union hdmi_infoframe *frame,
			  const void *buffer, size_t size);
/* Each field of the frame on a line of its own.  `level' (NULL: none) is
 * put in front of every line, then `dev' (NULL: none) and ": ". */
void hdmi_infoframe_log(const char *level, const char *dev,
			const union hdmi_infoframe *frame);

/* ---- the display-manager side (drm_hdmi_infoframe.c) --------------------
 *
 * Infoframes for a mode on a connector, from what the sink's EDID said
 * (connector->display_info).  The connector may be NULL: the frame is then
 * built as for an HDMI 2.0 sink without the HDMI vendor block. */

struct drm_connector;
struct drm_display_mode;

/* The AVI infoframe of a mode: pixel repetition for a double-clocked mode,
 * the CEA video code (0 for an HDMI 1.4 4k code, which goes in the vendor
 * infoframe, and for a CEA-861-F code an HDMI 1.4 sink did not list), the
 * picture aspect from the mode or its code, the active format "same as
 * picture", underscan.  0, or -EINVAL for a picture aspect the frame
 * cannot carry and the code does not imply. */
int drm_hdmi_avi_infoframe_from_display_mode(struct hdmi_avi_infoframe *frame,
					     const struct drm_connector *connector,
					     const struct drm_display_mode *mode);
/* The RGB (Q) and YCC (YQ) quantisation range fields for the range sent:
 * Q only when the sink takes it or it is the mode's default, YQ full only
 * for an HDMI 2.0 sink sent full range.  @connector may be NULL: the sink
 * is then taken not to select the RGB range. */
void drm_hdmi_avi_infoframe_quant_range(struct hdmi_avi_infoframe *frame,
					const struct drm_connector *connector,
					const struct drm_display_mode *mode,
					enum hdmi_quantization_range rgb_quant_range);
/* The HDMI vendor infoframe of a mode: its HDMI 1.4 video code and its
 * stereo layout.  -EINVAL when the sink has no HDMI vendor block (no frame
 * is to be sent). */
int drm_hdmi_vendor_infoframe_from_display_mode(struct hdmi_vendor_infoframe *frame,
						const struct drm_connector *connector,
						const struct drm_display_mode *mode);

#endif
