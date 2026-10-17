// LikeOS Intel audio DSP - private definitions of the topology code
// (sof_topology.c): the on-disk layout of ALSA SoC topology files, the
// vendor tuple tokens the DSP firmware's topologies use, the IPC4 message
// encodings and module configuration payloads, and the in-memory model the
// parser builds.
//
// Nothing here is shared with other files; sof_tplg.h is the public face.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_SOF_TPLG_INTERNAL_H
#define KERNEL_DEV_SOUND_SOF_TPLG_INTERNAL_H

#include "sof_tplg.h"

/* ==========================================================================
 * Topology file layout (little endian, packed, ABI version 5)
 *
 * A file is a sequence of blocks.  Every block starts with a fixed header
 * that names the element type, the element count and the payload size; the
 * payload holds `count' elements of that type back to back.  Most elements
 * end with a variable "private data" area (vendor tuple arrays) whose size
 * is stored in the element itself, so element N+1 starts after element N's
 * fixed part plus its private data.
 * ========================================================================== */

#define TPLG_MAGIC 0x41536F43u /* "CoSA" read as a little endian word */
#define TPLG_ABI_V4 4
#define TPLG_ABI_V5 5
#define TPLG_NAME_LEN 44 /* fixed name fields, not always terminated */
#define TPLG_TLV_WORDS 32
#define TPLG_MAX_CHAN 8
#define TPLG_STREAM_CONFIG_MAX 8
#define TPLG_HW_CONFIG_MAX 8
#define TPLG_NUM_TEXTS 16

/* block types */
#define TPLG_TYPE_MIXER 1
#define TPLG_TYPE_BYTES 2
#define TPLG_TYPE_ENUM 3
#define TPLG_TYPE_DAPM_GRAPH 4
#define TPLG_TYPE_DAPM_WIDGET 5
#define TPLG_TYPE_DAI_LINK 6
#define TPLG_TYPE_PCM 7
#define TPLG_TYPE_MANIFEST 8
#define TPLG_TYPE_CODEC_LINK 9
#define TPLG_TYPE_BACKEND_LINK 10
#define TPLG_TYPE_PDATA 11
#define TPLG_TYPE_DAI 12
#define TPLG_TYPE_VENDOR_FW 1000
#define TPLG_TYPE_VENDOR_CONFIG 1001
#define TPLG_TYPE_VENDOR_COEFF 1002
#define TPLG_TYPE_VENDOR_CODEC 1003

/* control "info" handler ids: what kind of control structure follows */
#define TPLG_CTL_VOLSW 1
#define TPLG_CTL_VOLSW_SX 2
#define TPLG_CTL_VOLSW_XR_SX 3
#define TPLG_CTL_ENUM 4
#define TPLG_CTL_BYTES 5
#define TPLG_CTL_ENUM_VALUE 6
#define TPLG_CTL_RANGE 7
#define TPLG_CTL_STROBE 8
#define TPLG_DAPM_CTL_VOLSW 64
#define TPLG_DAPM_CTL_ENUM_DOUBLE 65
#define TPLG_DAPM_CTL_ENUM_VIRT 66
#define TPLG_DAPM_CTL_ENUM_VALUE 67
#define TPLG_DAPM_CTL_PIN 68

/* the vendor "get/put" handler ids the DSP topologies use on controls */
#define TPLG_KCTL_VOL_ID 256
#define TPLG_KCTL_ENUM_ID 257
#define TPLG_KCTL_BYTES_ID 258
#define TPLG_KCTL_SWITCH_ID 259
#define TPLG_KCTL_BYTES_VOLATILE_RO 260
#define TPLG_KCTL_BYTES_VOLATILE_RW 261
#define TPLG_KCTL_BYTES_WO_ID 262

/* control element access bits */
#define TPLG_ACCESS_TLV_READ (1u << 4)

/* TLV types */
#define TPLG_TLVT_DB_SCALE 1

/* DAPM widget types as numbered in the file */
#define DAPM_INPUT 0
#define DAPM_OUTPUT 1
#define DAPM_MUX 2
#define DAPM_MIXER 3
#define DAPM_PGA 4
#define DAPM_OUT_DRV 5
#define DAPM_ADC 6
#define DAPM_DAC 7
#define DAPM_SWITCH 8
#define DAPM_PRE 9
#define DAPM_POST 10
#define DAPM_AIF_IN 11
#define DAPM_AIF_OUT 12
#define DAPM_DAI_IN 13
#define DAPM_DAI_OUT 14
#define DAPM_DAI_LINK 15
#define DAPM_BUFFER 16
#define DAPM_SCHEDULER 17
#define DAPM_EFFECT 18
#define DAPM_SIGGEN 19
#define DAPM_SRC 20
#define DAPM_ASRC 21
#define DAPM_ENCODER 22
#define DAPM_DECODER 23
#define DAPM_TYPE_LAST DAPM_DECODER

/* vendor tuple element types */
#define TUPLE_UUID 0
#define TUPLE_STRING 1
#define TUPLE_BOOL 2
#define TUPLE_BYTE 3
#define TUPLE_WORD 4
#define TUPLE_SHORT 5

struct tplg_hdr {
	uint32_t magic;
	uint32_t abi;
	uint32_t version;
	uint32_t type;
	uint32_t size; /* of this header */
	uint32_t vendor_type;
	uint32_t payload_size;
	uint32_t index; /* pipeline id for widget/control/graph blocks */
	uint32_t count;
} __attribute__((packed));

struct tplg_vendor_uuid_elem {
	uint32_t token;
	uint8_t uuid[16];
} __attribute__((packed));

struct tplg_vendor_value_elem {
	uint32_t token;
	uint32_t value;
} __attribute__((packed));

struct tplg_vendor_string_elem {
	uint32_t token;
	char string[TPLG_NAME_LEN];
} __attribute__((packed));

struct tplg_vendor_array {
	uint32_t size; /* of the whole array including this header */
	uint32_t type; /* TUPLE_* */
	uint32_t num_elems;
	/* elements follow */
} __attribute__((packed));

struct tplg_private {
	uint32_t size;
	/* data follows */
} __attribute__((packed));

struct tplg_tlv_dbscale {
	uint32_t min; /* signed, 1/100 dB */
	uint32_t step; /* 1/100 dB */
	uint32_t mute;
} __attribute__((packed));

struct tplg_ctl_tlv {
	uint32_t size;
	uint32_t type;
	union {
		uint32_t data[TPLG_TLV_WORDS];
		struct tplg_tlv_dbscale scale;
	};
} __attribute__((packed));

struct tplg_channel {
	uint32_t size;
	uint32_t reg;
	uint32_t shift;
	uint32_t id;
} __attribute__((packed));

struct tplg_io_ops {
	uint32_t get;
	uint32_t put;
	uint32_t info;
} __attribute__((packed));

struct tplg_ctl_hdr {
	uint32_t size; /* of this header */
	uint32_t type;
	char name[TPLG_NAME_LEN];
	uint32_t access;
	struct tplg_io_ops ops;
	struct tplg_ctl_tlv tlv;
} __attribute__((packed));

struct tplg_stream_caps {
	uint32_t size;
	char name[TPLG_NAME_LEN];
	uint64_t formats; /* PCM format bit mask (bit n = format n) */
	uint32_t rates; /* PCM rate bit mask */
	uint32_t rate_min;
	uint32_t rate_max;
	uint32_t channels_min;
	uint32_t channels_max;
	uint32_t periods_min;
	uint32_t periods_max;
	uint32_t period_size_min;
	uint32_t period_size_max;
	uint32_t buffer_size_min;
	uint32_t buffer_size_max;
	uint32_t sig_bits;
} __attribute__((packed));

struct tplg_stream {
	uint32_t size;
	char name[TPLG_NAME_LEN];
	uint64_t format;
	uint32_t rate;
	uint32_t period_bytes;
	uint32_t buffer_bytes;
	uint32_t channels;
} __attribute__((packed));

struct tplg_hw_config {
	uint32_t size;
	uint32_t id;
	uint32_t fmt;
	uint8_t clock_gated;
	uint8_t invert_bclk;
	uint8_t invert_fsync;
	uint8_t bclk_provider;
	uint8_t fsync_provider;
	uint8_t mclk_direction;
	uint16_t reserved;
	uint32_t mclk_rate;
	uint32_t bclk_rate;
	uint32_t fsync_rate;
	uint32_t tdm_slots;
	uint32_t tdm_slot_width;
	uint32_t tx_slots;
	uint32_t rx_slots;
	uint32_t tx_channels;
	uint32_t tx_chanmap[TPLG_MAX_CHAN];
	uint32_t rx_channels;
	uint32_t rx_chanmap[TPLG_MAX_CHAN];
} __attribute__((packed));

struct tplg_manifest {
	uint32_t size;
	uint32_t control_elems;
	uint32_t widget_elems;
	uint32_t graph_elems;
	uint32_t pcm_elems;
	uint32_t dai_link_elems;
	uint32_t dai_elems;
	uint32_t reserved[20];
	struct tplg_private priv;
} __attribute__((packed));

struct tplg_mixer_control {
	struct tplg_ctl_hdr hdr;
	uint32_t size;
	uint32_t min;
	uint32_t max;
	uint32_t platform_max;
	uint32_t invert;
	uint32_t num_channels;
	struct tplg_channel channel[TPLG_MAX_CHAN];
	struct tplg_private priv;
} __attribute__((packed));

struct tplg_enum_control {
	struct tplg_ctl_hdr hdr;
	uint32_t size;
	uint32_t num_channels;
	struct tplg_channel channel[TPLG_MAX_CHAN];
	uint32_t items;
	uint32_t mask;
	uint32_t count;
	char texts[TPLG_NUM_TEXTS][TPLG_NAME_LEN];
	uint32_t values[TPLG_NUM_TEXTS * TPLG_NAME_LEN / 4];
	struct tplg_private priv;
} __attribute__((packed));

struct tplg_bytes_control {
	struct tplg_ctl_hdr hdr;
	uint32_t size;
	uint32_t max;
	uint32_t mask;
	uint32_t base;
	uint32_t num_regs;
	struct tplg_io_ops ext_ops;
	struct tplg_private priv;
} __attribute__((packed));

struct tplg_graph_elem {
	char sink[TPLG_NAME_LEN];
	char control[TPLG_NAME_LEN];
	char source[TPLG_NAME_LEN];
} __attribute__((packed));

struct tplg_dapm_widget {
	uint32_t size;
	uint32_t id; /* DAPM_* */
	char name[TPLG_NAME_LEN];
	char sname[TPLG_NAME_LEN];
	uint32_t reg;
	uint32_t shift;
	uint32_t mask;
	uint32_t subseq;
	uint32_t invert;
	uint32_t ignore_suspend;
	uint16_t event_flags;
	uint16_t event_type;
	uint32_t num_kcontrols;
	struct tplg_private priv;
	/* num_kcontrols control elements follow the private data */
} __attribute__((packed));

struct tplg_pcm {
	uint32_t size;
	char pcm_name[TPLG_NAME_LEN];
	char dai_name[TPLG_NAME_LEN];
	uint32_t pcm_id;
	uint32_t dai_id;
	uint32_t playback;
	uint32_t capture;
	uint32_t compress;
	struct tplg_stream stream[TPLG_STREAM_CONFIG_MAX];
	uint32_t num_streams;
	struct tplg_stream_caps caps[2];
	uint32_t flag_mask;
	uint32_t flags;
	struct tplg_private priv;
} __attribute__((packed));

struct tplg_link_config {
	uint32_t size;
	uint32_t id;
	char name[TPLG_NAME_LEN];
	char stream_name[TPLG_NAME_LEN];
	struct tplg_stream stream[TPLG_STREAM_CONFIG_MAX];
	uint32_t num_streams;
	struct tplg_hw_config hw_config[TPLG_HW_CONFIG_MAX];
	uint32_t num_hw_configs;
	uint32_t default_hw_config_id;
	uint32_t flag_mask;
	uint32_t flags;
	struct tplg_private priv;
} __attribute__((packed));

struct tplg_dai {
	uint32_t size;
	char dai_name[TPLG_NAME_LEN];
	uint32_t dai_id;
	uint32_t playback;
	uint32_t capture;
	struct tplg_stream_caps caps[2];
	uint32_t flag_mask;
	uint32_t flags;
	struct tplg_private priv;
} __attribute__((packed));

_Static_assert(sizeof(struct tplg_hdr) == 36, "tplg hdr");
_Static_assert(sizeof(struct tplg_vendor_uuid_elem) == 20, "uuid elem");
_Static_assert(sizeof(struct tplg_vendor_value_elem) == 8, "value elem");
_Static_assert(sizeof(struct tplg_vendor_string_elem) == 48, "string elem");
_Static_assert(sizeof(struct tplg_vendor_array) == 12, "vendor array");
_Static_assert(sizeof(struct tplg_ctl_tlv) == 136, "ctl tlv");
_Static_assert(sizeof(struct tplg_ctl_hdr) == 204, "ctl hdr");
_Static_assert(sizeof(struct tplg_stream_caps) == 104, "stream caps");
_Static_assert(sizeof(struct tplg_stream) == 72, "stream");
_Static_assert(sizeof(struct tplg_hw_config) == 120, "hw config");
_Static_assert(sizeof(struct tplg_manifest) == 112, "manifest");
_Static_assert(sizeof(struct tplg_mixer_control) == 360, "mixer control");
_Static_assert(sizeof(struct tplg_enum_control) == 1764, "enum control");
_Static_assert(sizeof(struct tplg_bytes_control) == 240, "bytes control");
_Static_assert(sizeof(struct tplg_graph_elem) == 132, "graph elem");
_Static_assert(sizeof(struct tplg_dapm_widget) == 132, "dapm widget");
_Static_assert(sizeof(struct tplg_pcm) == 912, "pcm");
_Static_assert(sizeof(struct tplg_link_config) == 1656, "link config");
_Static_assert(sizeof(struct tplg_dai) == 280, "dai");

/* PCM format bit numbers used in the caps' `formats' masks */
#define PCMF_S8 0
#define PCMF_U8 1
#define PCMF_S16_LE 2
#define PCMF_S24_LE 6
#define PCMF_S32_LE 10
#define PCMF_FLOAT_LE 14
#define PCMF_S20_LE 25
#define PCMF_S24_3LE 32

/* PCM rate bits used in the caps' `rates' masks */
#define PCMR_CONTINUOUS (1u << 30)
#define PCMR_KNOT (1u << 31)

/* ==========================================================================
 * Vendor tuple tokens of the DSP firmware's topologies
 * ========================================================================== */

#define TKN_BUF_SIZE 100
#define TKN_BUF_CAPS 101
#define TKN_BUF_FLAGS 102
#define TKN_DAI_TYPE 154 /* string: "SSP" "DMIC" "HDA" "ALH" ... */
#define TKN_DAI_INDEX 155
#define TKN_DAI_DIRECTION 156
#define TKN_SCHED_PERIOD 200
#define TKN_SCHED_PRIORITY 201
#define TKN_SCHED_MIPS 202
#define TKN_SCHED_CORE 203
#define TKN_SCHED_FRAMES 204
#define TKN_SCHED_TIME_DOMAIN 205
#define TKN_SCHED_DYNAMIC_PIPELINE 206
#define TKN_SCHED_LP_MODE 207
#define TKN_SCHED_MEM_USAGE 208
#define TKN_SCHED_USE_CHAIN_DMA 209
#define TKN_SCHED_KCPS 210
#define TKN_SCHED_DIRECTION 211
#define TKN_SCHED_DIRECTION_VALID 212
#define TKN_VOLUME_RAMP_STEP_TYPE 250
#define TKN_VOLUME_RAMP_STEP_MS 251
#define TKN_GAIN_RAMP_TYPE 260
#define TKN_GAIN_RAMP_DURATION 261
#define TKN_GAIN_VAL 262
#define TKN_SRC_RATE_IN 300
#define TKN_SRC_RATE_OUT 301
#define TKN_ASRC_RATE_IN 320
#define TKN_ASRC_RATE_OUT 321
#define TKN_ASRC_ASYNCHRONOUS_MODE 322
#define TKN_ASRC_OPERATION_MODE 323
#define TKN_PCM_DMAC_CONFIG 353
#define TKN_COMP_PERIOD_SINK_COUNT 400
#define TKN_COMP_PERIOD_SOURCE_COUNT 401
#define TKN_COMP_FORMAT 402
#define TKN_COMP_CORE_ID 404
#define TKN_COMP_UUID 405
#define TKN_COMP_CPC 406
#define TKN_COMP_IS_PAGES 409
#define TKN_COMP_NUM_AUDIO_FORMATS 410
#define TKN_COMP_NUM_INPUT_PINS 411
#define TKN_COMP_NUM_OUTPUT_PINS 412
#define TKN_COMP_INPUT_PIN_BINDING_WNAME 413
#define TKN_COMP_OUTPUT_PIN_BINDING_WNAME 414
#define TKN_COMP_NUM_INPUT_AUDIO_FORMATS 415
#define TKN_COMP_NUM_OUTPUT_AUDIO_FORMATS 416
#define TKN_COMP_NO_WNAME_IN_KCONTROL_NAME 417
#define TKN_COMP_SCHED_DOMAIN 418 /* string: "LL" or "DP" */
#define TKN_COMP_DOMAIN_ID 419
#define TKN_COMP_STACK_BYTES 420
#define TKN_COMP_HEAP_BYTES 421
#define TKN_INTEL_SSP_CLKS_CONTROL 500
#define TKN_INTEL_SSP_MCLK_ID 501
#define TKN_INTEL_SSP_SAMPLE_BITS 502
#define TKN_INTEL_SSP_FRAME_PULSE_WIDTH 503
#define TKN_INTEL_SSP_QUIRKS 504
#define TKN_INTEL_SSP_TDM_PADDING 505
#define TKN_INTEL_SSP_BCLK_DELAY 506
#define TKN_INTEL_DMIC_DRIVER_VERSION 600
#define TKN_INTEL_DMIC_CLK_MIN 601
#define TKN_INTEL_DMIC_CLK_MAX 602
#define TKN_INTEL_DMIC_DUTY_MIN 603
#define TKN_INTEL_DMIC_DUTY_MAX 604
#define TKN_INTEL_DMIC_NUM_PDM_ACTIVE 605
#define TKN_INTEL_DMIC_SAMPLE_RATE 608
#define TKN_INTEL_DMIC_FIFO_WORD_LENGTH 609
#define TKN_INTEL_DMIC_UNMUTE_RAMP_MS 610
#define TKN_TONE_SAMPLE_RATE 800
#define TKN_PROCESS_TYPE 900
#define TKN_STREAM_PLAYBACK_D0I3 1200
#define TKN_STREAM_CAPTURE_D0I3 1201
#define TKN_STREAM_PLAYBACK_PAUSE 1202
#define TKN_STREAM_CAPTURE_PAUSE 1203
#define TKN_MUTE_LED_USE 1300
#define TKN_MUTE_LED_DIRECTION 1301
#define TKN_INTEL_ALH_RATE 1400
#define TKN_INTEL_ALH_CH 1401
#define TKN_INTEL_HDA_RATE 1500
#define TKN_INTEL_HDA_CH 1501
#define TKN_MIXER_TYPE 1700
#define TKN_FMT_IN_RATE 1900
#define TKN_FMT_IN_BIT_DEPTH 1901
#define TKN_FMT_IN_VALID_BIT_DEPTH 1902
#define TKN_FMT_IN_CHANNELS 1903
#define TKN_FMT_IN_CH_MAP 1904
#define TKN_FMT_IN_CH_CFG 1905
#define TKN_FMT_IN_INTERLEAVING 1906
#define TKN_FMT_IN_FMT_CFG 1907
#define TKN_FMT_IN_SAMPLE_TYPE 1908
#define TKN_FMT_IN_PIN_INDEX 1909
#define TKN_FMT_OUT_RATE 1930
#define TKN_FMT_OUT_BIT_DEPTH 1931
#define TKN_FMT_OUT_VALID_BIT_DEPTH 1932
#define TKN_FMT_OUT_CHANNELS 1933
#define TKN_FMT_OUT_CH_MAP 1934
#define TKN_FMT_OUT_CH_CFG 1935
#define TKN_FMT_OUT_INTERLEAVING 1936
#define TKN_FMT_OUT_FMT_CFG 1937
#define TKN_FMT_OUT_SAMPLE_TYPE 1938
#define TKN_FMT_OUT_PIN_INDEX 1939
#define TKN_FMT_IBS 1970
#define TKN_FMT_OBS 1971
#define TKN_FMT_DMA_BUFFER_SIZE 1972
#define TKN_COPIER_NODE_TYPE 1980
#define TKN_COPIER_DEEP_BUFFER_DMA_MS 1981

/* sof_abi_hdr in bytes-control private data */
#define SOF_ABI_MAGIC 0x00464F53u /* "SOF\0" */
#define SOF_ABI_HDR_SIZE 32

/* the IPC4 topology manifest in the manifest's private data */
#define SOF_MANIFEST_HDR_SIZE 8 /* abi_major/minor/patch, count (u16 each) */
#define SOF_MANIFEST_TLV_HDR 8 /* type, size (u32 each) */
#define SOF_MANIFEST_DATA_TYPE_NHLT 1

/* ==========================================================================
 * IPC4 message encodings
 * ========================================================================== */

#define IPC4_MSG_TARGET_FW 0u
#define IPC4_MSG_TARGET_MODULE (1u << 30)
#define IPC4_MSG_REQUEST 0u
#define IPC4_MSG_TYPE(x) (((uint32_t)(x) & 0x1fu) << 24)

/* global (firmware) message types */
#define IPC4_GLB_CREATE_PIPELINE 17
#define IPC4_GLB_DELETE_PIPELINE 18
#define IPC4_GLB_SET_PIPELINE_STATE 19
#define IPC4_GLB_GET_PIPELINE_STATE 20
#define IPC4_GLB_CHAIN_DMA 14

/* chained DMA (no processing: host DMA feeds link DMA directly) */
#define IPC4_CHAIN_DMA_HOST_ID(x) ((uint32_t)(x) & 0x1fu)
#define IPC4_CHAIN_DMA_LINK_ID(x) (((uint32_t)(x) & 0x1fu) << 8)
#define IPC4_CHAIN_DMA_ALLOCATE (1u << 16)
#define IPC4_CHAIN_DMA_ENABLE (1u << 17)
#define IPC4_CHAIN_DMA_SCS (1u << 18)
#define IPC4_CHAIN_DMA_FIFO_SIZE(x) ((uint32_t)(x) & 0x1ffffffu)
#define IPC4_CHAIN_DMA_BUF_MS 2

/* pipeline messages */
#define IPC4_PIPE_INSTANCE(x) (((uint32_t)(x) & 0xffu) << 16)
#define IPC4_PIPE_PRIORITY(x) (((uint32_t)(x) & 0x1fu) << 11)
#define IPC4_PIPE_MEM_SIZE(x) ((uint32_t)(x) & 0x7ffu)
#define IPC4_PIPE_MEM_SIZE_MAX 0x7ffu
#define IPC4_PIPE_EXT_LP(x) ((uint32_t)(x) & 1u)
#define IPC4_PIPE_EXT_CORE(x) (((uint32_t)(x) & 0xfu) << 20)
#define IPC4_PIPE_EXT_PAYLOAD (1u << 29)
#define IPC4_PIPE_STATE_ID(x) (((uint32_t)(x) & 0xffu) << 16)
#define IPC4_PIPE_STATE(x) ((uint32_t)(x) & 0xffffu)
#define IPC4_PIPE_STATE_EXT_MULTI 1u

/* pipeline states */
#define IPC4_PIPE_INVALID 0
#define IPC4_PIPE_UNINITIALIZED 1
#define IPC4_PIPE_RESET 2
#define IPC4_PIPE_PAUSED 3
#define IPC4_PIPE_RUNNING 4
#define IPC4_PIPE_EOS 5

/* module message types */
#define IPC4_MOD_INIT_INSTANCE 0
#define IPC4_MOD_CONFIG_GET 1
#define IPC4_MOD_CONFIG_SET 2
#define IPC4_MOD_LARGE_CONFIG_GET 3
#define IPC4_MOD_LARGE_CONFIG_SET 4
#define IPC4_MOD_BIND 5
#define IPC4_MOD_UNBIND 6
#define IPC4_MOD_SET_DX 7
#define IPC4_MOD_SET_D0IX 8
#define IPC4_MOD_DELETE_INSTANCE 11

#define IPC4_MOD_ID(x) ((uint32_t)(x) & 0xffffu)
#define IPC4_MOD_INSTANCE(x) (((uint32_t)(x) & 0xffu) << 16)
/* INIT_INSTANCE extension */
#define IPC4_MOD_EXT_PARAM_SIZE(x) ((uint32_t)(x) & 0xffffu)
#define IPC4_MOD_EXT_PPL_ID(x) (((uint32_t)(x) & 0xffu) << 16)
#define IPC4_MOD_EXT_CORE_ID(x) (((uint32_t)(x) & 0xfu) << 24)
#define IPC4_MOD_EXT_DOMAIN(x) (((uint32_t)(x) & 1u) << 28)
#define IPC4_MOD_EXT_EXTENDED_INIT (1u << 29)
/* BIND/UNBIND extension */
#define IPC4_MOD_EXT_DST_ID(x) ((uint32_t)(x) & 0xffffu)
#define IPC4_MOD_EXT_DST_INSTANCE(x) (((uint32_t)(x) & 0xffu) << 16)
#define IPC4_MOD_EXT_DST_QUEUE(x) (((uint32_t)(x) & 0x7u) << 24)
#define IPC4_MOD_EXT_SRC_QUEUE(x) (((uint32_t)(x) & 0x7u) << 27)
/* LARGE_CONFIG_SET/GET extension */
#define IPC4_MOD_EXT_MSG_SIZE(x) ((uint32_t)(x) & 0xfffffu)
#define IPC4_MOD_EXT_MSG_PARAM_ID(x) (((uint32_t)(x) & 0xffu) << 20)
#define IPC4_MOD_EXT_MSG_LAST_BLOCK (1u << 28)
#define IPC4_MOD_EXT_MSG_FIRST_BLOCK (1u << 29)

/* module init extended payload (DP memory requirements) */
#define IPC4_MOD_INIT_EXT_OBJ_ARRAY (1u << 2)
#define IPC4_MOD_INIT_EXT_OBJ_LAST 1u
#define IPC4_MOD_INIT_EXT_OBJ_ID(x) (((uint32_t)(x) & 0x7fffu) << 1)
#define IPC4_MOD_INIT_EXT_OBJ_WORDS(x) (((uint32_t)(x) & 0xffffu) << 16)
#define IPC4_MOD_INIT_DATA_ID_DP_DATA 1

/* base firmware module and its parameters */
#define IPC4_BASEFW_MODULE_ID 0
#define IPC4_BASEFW_PARAM_MODULES_INFO 9

/* IPC payload limit of the mailbox */
#define IPC4_MSG_MAX_SIZE 4096u

/* module manifest type flags */
#define IPC4_MODULE_LOAD_TYPE_MASK 0xfu
#define IPC4_MODULE_AUTO_START (1u << 4)
#define IPC4_MODULE_LL (1u << 5)
#define IPC4_MODULE_DP (1u << 6)
#define IPC4_MODULE_LIB_CODE (1u << 7)
#define IPC4_MODULE_INIT_CONFIG(t) (((t) >> 8) & 0xfu)
#define IPC4_INIT_CONFIG_BASE_CFG 0
#define IPC4_INIT_CONFIG_BASE_CFG_WITH_EXT 1

/* sample types of the audio format */
#define IPC4_TYPE_MSB_INTEGER 0
#define IPC4_TYPE_LSB_INTEGER 1
#define IPC4_TYPE_SIGNED_INTEGER 2
#define IPC4_TYPE_UNSIGNED_INTEGER 3
#define IPC4_TYPE_FLOAT 4

#define IPC4_FMT_CFG_CHANNELS(x) ((x) & 0xffu)
#define IPC4_FMT_CFG_VALID_BITS(x) (((x) >> 8) & 0xffu)
#define IPC4_FMT_CFG_SAMPLE_TYPE(x) (((x) >> 16) & 0xffu)

/* gateway node ids */
#define IPC4_NODE_INDEX(x) ((uint32_t)(x) & 0xffu)
#define IPC4_NODE_TYPE(x) ((uint32_t)(x) << 8)
#define IPC4_NODE_GET_TYPE(id) ((id) >> 8)
#define IPC4_NODE_INDEX_SSP(x) (((uint32_t)(x) & 0xfu) << 4)
#define IPC4_NODE_INDEX_DMIC(x) ((uint32_t)(x) & 0x7u)
#define IPC4_INVALID_NODE_ID 0xffffffffu
#define IPC4_CHAIN_DMA_NODE_ID 0x7fffffffu
#define IPC4_ALH_MULTI_GTW_BASE 0x50
#define IPC4_ALH_MULTI_GTW_COUNT 8
#define IPC4_MIN_DMA_BUFFER_MS 4

/* gateway (connector node) classes, for the dump */
#define IPC4_NODE_HDA_HOST_OUT 0
#define IPC4_NODE_HDA_HOST_IN 1
#define IPC4_NODE_HDA_HOST_INOUT 2
#define IPC4_NODE_HDA_LINK_OUT 8
#define IPC4_NODE_HDA_LINK_IN 9
#define IPC4_NODE_HDA_LINK_INOUT 10
#define IPC4_NODE_DMIC_LINK_IN 11
#define IPC4_NODE_I2S_LINK_OUT 12
#define IPC4_NODE_I2S_LINK_IN 13
#define IPC4_NODE_ALH_LINK_OUT 16
#define IPC4_NODE_ALH_LINK_IN 17

/* gain module */
#define IPC4_GAIN_PARAM_ID 0
#define IPC4_GAIN_ALL_CHANNELS 0xffffffffu
#define IPC4_VOL_ZERO_DB 0x7fffffffu
#define IPC4_SWITCH_CONTROL_PARAM_ID 200
#define IPC4_ENUM_CONTROL_PARAM_ID 201
#define IPC4_BYTES_CONTROL_PARAM_ID 202

/* copier module parameters */
#define IPC4_COPIER_PARAM_SET_SINK_FORMAT 2

/* firmware memory accounting used for the pipeline's memory pages */
#define IPC4_FW_PAGE_BYTES 4096u
#define IPC4_PIPELINE_OBJECT_SIZE 448u
#define IPC4_DATA_QUEUE_OBJECT_SIZE 128u
#define IPC4_LL_TASK_OBJECT_SIZE 72u
#define IPC4_DP_TASK_OBJECT_SIZE 104u
#define IPC4_DP_TASK_LIST_SIZE (12u + 8u)
#define IPC4_LL_TASK_LIST_ITEM_SIZE 12u
#define IPC4_MODULE_INSTANCE_LIST_ITEM_SIZE 12u
#define IPC4_FW_MAX_QUEUE_COUNT 8u

/* ---- IPC4 payload structures (all 32-bit words, little endian) --------- */

struct ipc4_audio_format {
	uint32_t sampling_frequency;
	uint32_t bit_depth;
	uint32_t ch_map;
	uint32_t ch_cfg;
	uint32_t interleaving_style;
	uint32_t fmt_cfg; /* channels | valid bits << 8 | sample type << 16 */
} __attribute__((packed));

struct ipc4_base_module_cfg {
	uint32_t cpc;
	uint32_t ibs;
	uint32_t obs;
	uint32_t is_pages;
	struct ipc4_audio_format audio_fmt;
} __attribute__((packed));

struct ipc4_copier_gateway_cfg {
	uint32_t node_id;
	uint32_t dma_buffer_size;
	uint32_t config_length; /* in 32-bit words, the blob that follows */
} __attribute__((packed));

struct ipc4_copier_data {
	struct ipc4_base_module_cfg base;
	struct ipc4_audio_format out_format;
	uint32_t copier_feature_mask;
	struct ipc4_copier_gateway_cfg gtw_cfg;
	/* config_length words follow */
} __attribute__((packed));

struct ipc4_gain_params {
	uint32_t channels;
	uint32_t init_val;
	uint32_t curve_type;
	uint32_t reserved;
	uint32_t curve_duration_l;
	uint32_t curve_duration_h;
} __attribute__((packed));

struct ipc4_gain_data {
	struct ipc4_base_module_cfg base;
	struct ipc4_gain_params params;
} __attribute__((packed));

struct ipc4_src_data {
	struct ipc4_base_module_cfg base;
	uint32_t sink_rate;
} __attribute__((packed));

struct ipc4_asrc_data {
	struct ipc4_base_module_cfg base;
	uint32_t out_freq;
	uint32_t asrc_mode;
} __attribute__((packed));

struct ipc4_pin_format {
	uint32_t pin_index;
	uint32_t buffer_size;
	struct ipc4_audio_format audio_fmt;
} __attribute__((packed));

struct ipc4_base_module_cfg_ext {
	uint16_t num_input_pin_fmts;
	uint16_t num_output_pin_fmts;
	uint8_t reserved[12];
	/* struct ipc4_pin_format pin_formats[] follow */
} __attribute__((packed));

struct ipc4_copier_sink_format {
	uint32_t sink_id;
	struct ipc4_audio_format source_fmt;
	struct ipc4_audio_format sink_fmt;
} __attribute__((packed));

struct ipc4_module_init_ext_init {
	uint32_t word0;
	uint32_t rsvd1;
	uint32_t rsvd2;
} __attribute__((packed));

struct ipc4_dp_memory_data {
	uint32_t domain_id;
	uint32_t stack_bytes;
	uint32_t heap_bytes;
} __attribute__((packed));

/* ALH (SoundWire) gateway blob */
#define IPC4_ALH_MAX_DEVICES 16
struct ipc4_alh_mapping {
	uint32_t device; /* node id of one link's ALH stream */
	uint32_t channel_mask;
} __attribute__((packed));

/* the module entry layout of the firmware manifest (MODULES_INFO reply) */
#define FW_MAN_MODULE_SIZE 116
#define FW_MAN_MODULE_OFF_ID 0
#define FW_MAN_MODULE_OFF_NAME 4
#define FW_MAN_MODULE_OFF_UUID 12
#define FW_MAN_MODULE_OFF_TYPE 28
#define FW_MAN_MODULE_OFF_AFFINITY 72
#define FW_MAN_MODULE_OFF_INSTANCE_MAX 76
#define FW_MAN_MODULE_OFF_BSS 78

_Static_assert(sizeof(struct ipc4_audio_format) == 24, "ipc4 audio fmt");
_Static_assert(sizeof(struct ipc4_base_module_cfg) == 40, "ipc4 base cfg");
_Static_assert(sizeof(struct ipc4_copier_data) == 80, "ipc4 copier");
_Static_assert(sizeof(struct ipc4_gain_data) == 64, "ipc4 gain");
_Static_assert(sizeof(struct ipc4_pin_format) == 32, "ipc4 pin fmt");
_Static_assert(sizeof(struct ipc4_base_module_cfg_ext) == 16, "ipc4 cfg ext");
_Static_assert(sizeof(struct ipc4_copier_sink_format) == 52, "sink fmt");

/* ==========================================================================
 * Hooks provided by other parts of the DSP driver, when they exist.
 *
 * The module lookup (sof_loader.c) reads the firmware manifests; when it
 * does not know a module the code asks the firmware for its module list
 * over IPC.  The NHLT blob provider is optional (a weak reference): without
 * it the ACPI NHLT table and the topology manifests are searched.
 * ========================================================================== */

struct sof_fw_module_info {
	uint32_t module_id; /* (library id << 12) | index */
	uint32_t type; /* manifest type flags (IPC4_MODULE_*) */
	uint32_t instance_max; /* 0 = unknown */
	uint32_t cpc; /* cycles per chunk from the module config, 0 = none */
	uint32_t is_pages; /* instance bss pages, 0 = none */
};

int sof_fw_module_find(struct sof_dev *sdev, const uint8_t uuid[16],
		       struct sof_fw_module_info *out);
int sof_nhlt_get_blob(struct sof_dev *sdev, int link_type, int instance,
		      int dir, uint32_t rate, uint32_t channels, uint32_t bits,
		      const void **blob, uint32_t *len) __attribute__((weak));

/* NHLT link types */
#define NHLT_LINK_HDA 0
#define NHLT_LINK_DSP 1
#define NHLT_LINK_DMIC 2
#define NHLT_LINK_SSP 3
#define NHLT_LINK_SDW 5

/* ==========================================================================
 * The in-memory model
 * ========================================================================== */

#define TPLG_MAX_FILES 8
#define TPLG_MAX_WIDGETS 512
#define TPLG_MAX_ROUTES 1024
#define TPLG_MAX_PIPES 128
#define TPLG_MAX_PCMS 32
#define TPLG_MAX_LINKS 64
#define TPLG_MAX_FMTS 64 /* audio formats per widget direction */
#define TPLG_MAX_PINS 8
#define TPLG_MAX_KCTLS 8 /* kcontrols kept per widget */
#define TPLG_MAX_BYTES_DATA 8192 /* bytes control blob kept */
#define TPLG_MAX_PATH 48 /* widgets on one PCM direction's path */
#define TPLG_MAX_PATH_PIPES 16
#define TPLG_MAX_PATH_ROUTES 64
#define TPLG_MAX_PATH_DAIS 8
#define TPLG_MAX_MODULES 128 /* firmware modules remembered */
#define TPLG_MAX_PIPE_INSTANCES 32 /* pipeline instance ids */
#define TPLG_MAX_MOD_INSTANCES 64 /* instance ids per module */
#define TPLG_MAX_NHLT 3

/* scheduling domain of a module */
#define TPLG_DOMAIN_UNSET 0
#define TPLG_DOMAIN_LL 1
#define TPLG_DOMAIN_DP 2

/* one entry of a widget's audio format list */
struct tplg_fmt {
	uint32_t rate;
	uint32_t bit_depth;
	uint32_t valid_bits; /* VALID_BIT_DEPTH token, 0 = from fmt_cfg */
	uint32_t channels; /* CHANNELS token, 0 = from fmt_cfg */
	uint32_t ch_map;
	uint32_t ch_cfg;
	uint32_t interleaving;
	uint32_t fmt_cfg;
	uint32_t sample_type; /* SAMPLE_TYPE token, ~0 = from fmt_cfg */
	uint32_t pin_index;
	uint32_t buffer_size; /* ibs or obs */
};

struct tplg_kctl {
	uint32_t kind; /* TPLG_TYPE_MIXER / _BYTES / _ENUM */
	char name[SOF_TPLG_NAME_MAX];
	uint32_t info, get, put; /* io ops */
	uint32_t access;
	/* mixer */
	uint32_t min, max, invert, num_channels;
	int has_db;
	int32_t db_min; /* 1/100 dB */
	uint32_t db_step; /* 1/100 dB */
	int db_mute;
	/* enum */
	uint32_t items;
	/* bytes: the default blob (sof_abi_hdr + data) */
	uint32_t bytes_max;
	uint8_t *bytes;
	uint32_t bytes_len;
	/* runtime value (mixer: steps per channel) */
	uint32_t value[2];
};

struct tplg_widget {
	int file; /* which loaded topology */
	uint32_t pipe_index; /* the block index = pipeline id */
	int pipe; /* index into tplg->pipes, -1 if none */
	uint32_t id; /* DAPM_* */
	char name[SOF_TPLG_NAME_MAX];
	char sname[SOF_TPLG_NAME_MAX];

	/* component tokens */
	uint8_t uuid[16];
	int has_uuid;
	uint32_t core;
	uint32_t domain; /* TPLG_DOMAIN_* */
	uint32_t domain_id, stack_bytes, heap_bytes;
	uint32_t cpc;
	uint32_t is_pages;
	uint32_t num_input_pins, num_output_pins;
	char in_bind[TPLG_MAX_PINS][SOF_TPLG_NAME_MAX];
	char out_bind[TPLG_MAX_PINS][SOF_TPLG_NAME_MAX];
	int n_in_bind, n_out_bind;

	/* audio formats */
	uint32_t num_in_fmts, num_out_fmts;
	struct tplg_fmt *in_fmts, *out_fmts;

	/* copier */
	int has_node_type;
	uint32_t node_type;
	uint32_t deep_buffer_ms;
	int dai_type; /* SOF_DAI_* (0 = not a DAI or unknown) */
	char dai_type_str[12];
	uint32_t dai_index;
	int has_dai_index;
	uint32_t dai_direction;

	/* pipeline (scheduler widget) */
	uint32_t priority, lp_mode, sched_core, period, frames, time_domain;
	uint32_t mem_usage_tkn, kcps, dynamic, use_chain_dma;
	uint32_t direction;
	int direction_valid;

	/* gain */
	uint32_t gain_ramp_type, gain_ramp_duration, gain_init;
	int has_gain_init;
	/* src / asrc */
	uint32_t src_rate_in, src_rate_out, asrc_mode, asrc_async;
	/* mixer */
	uint32_t mixer_type;
	/* process */
	char process_type[SOF_TPLG_NAME_MAX];
	/* buffer */
	uint32_t buf_size, buf_caps;

	/* controls */
	int nkctl;
	struct tplg_kctl kctl[TPLG_MAX_KCTLS];

	/* ---- runtime ---------------------------------------------------- */
	int use_count; /* PCM streams that set this module up */
	int instance_id; /* -1 when not instantiated */
	int module_resolved;
	struct sof_fw_module_info mod;
	int fmt_valid; /* formats chosen */
	int in_fmt_idx, out_fmt_idx;
	struct ipc4_base_module_cfg base; /* as sent */
	struct ipc4_audio_format out_format; /* copier output / module output */
	uint32_t node_id; /* copier gateway */
	int alh_group; /* multi-gateway group, -1 none */
	uint32_t in_q_used, out_q_used; /* queue ids handed out (no binding) */
	uint32_t dma_buffer_size; /* copier gateway buffer */
	const uint8_t *dai_blob; /* SSP/DMIC gateway blob (NHLT) */
	uint32_t dai_blob_len;
	int sdw_link; /* ALH: the SoundWire link, -1 */
	uint32_t alh_count; /* ALH: devices in the (multi) gateway */
	struct ipc4_alh_mapping alh_map[IPC4_ALH_MAX_DEVICES];
	uint8_t alh_link[IPC4_ALH_MAX_DEVICES]; /* link of each device */
	uint8_t alh_stream_id[IPC4_ALH_MAX_DEVICES];
	uint8_t alh_dma_tag[IPC4_ALH_MAX_DEVICES]; /* ACE 2+ link DMA */
	uint32_t link_tag; /* HDA link DMA stream tag (HDA DAI, ACE 2+) */
};

struct tplg_route {
	int file;
	int src, sink; /* widget indices */
	char control[SOF_TPLG_NAME_MAX];
	/* runtime */
	int refs; /* streams that need the binding; bound while > 0 */
	int src_q, dst_q;
};

struct tplg_pipe {
	int file;
	uint32_t index; /* pipeline id in the topology */
	int sched; /* scheduler widget, -1 when the topology has none */
	/* runtime */
	int use_count;
	int instance_id; /* -1 when not created */
	int state; /* IPC4_PIPE_* */
	int started, paused; /* PCM streams in the started/paused state */
	uint32_t mem_pages;
};

struct tplg_link {
	int file;
	uint32_t id;
	char name[SOF_TPLG_NAME_MAX];
	char stream_name[SOF_TPLG_NAME_MAX];
	int dai_type; /* from tokens / name */
	uint32_t alh_rate, alh_ch, hda_rate, hda_ch;
	uint32_t ssp_sample_bits, ssp_mclk_id;
	uint32_t dmic_rate, dmic_pdm_active;
	uint32_t hw_rate, hw_channels, hw_slot_width; /* default hw config */
	int num_hw_configs;
};

/* the runtime of one direction of a PCM */
struct tplg_stream_rt {
	int prepared;
	int state; /* IPC4_PIPE_* last requested */
	int pushed; /* paused by SND_TRIGGER_PAUSE (not stopped) */
	int chain; /* chained DMA: host DMA straight into link DMA */
	int chain_allocated;
	uint32_t chain_primary; /* host/link DMA ids, SCS */
	uint32_t chain_fifo; /* the DMA buffer size field */
	uint32_t rate, channels, bits;
	int host_tag;
	int nw;
	int w[TPLG_MAX_PATH]; /* widgets in data-flow order */
	int w_acq[TPLG_MAX_PATH]; /* this stream holds a use count */
	int np;
	int p[TPLG_MAX_PATH_PIPES]; /* pipelines, data-flow order */
	int p_acq[TPLG_MAX_PATH_PIPES];
	int nr;
	int r[TPLG_MAX_PATH_ROUTES]; /* routes inside the path */
	int r_acq[TPLG_MAX_PATH_ROUTES];
	int nd;
	int dai[TPLG_MAX_PATH_DAIS]; /* DAI copier widgets on the path */
	int nsdw;
	int sdw_link[TPLG_MAX_PATH_DAIS]; /* SoundWire links prepared */
	int sdw_fn[TPLG_MAX_PATH_DAIS]; /* their SOF_SDW_FN_* (0 unknown) */
	int sdw_enabled[TPLG_MAX_PATH_DAIS];
	int gain; /* gain widget used for volume, -1 */
};

struct tplg_pcm_int {
	struct sof_tplg_pcm pub;
	int file;
	char dai_name[SOF_TPLG_NAME_MAX];
	char cap_name[2][SOF_TPLG_NAME_MAX];
	uint32_t rate_min[2], rate_max[2];
	uint32_t ch_min[2], ch_max[2];
	uint64_t pcm_formats[2];
	uint32_t pcm_rates[2];
	int pause_supported[2];
	int d0i3_compatible[2];
	int host[2]; /* host copier widget per direction, -1 */
	struct tplg_stream_rt rt[2];
	uint32_t vol_l, vol_r; /* 0..100 */
	int vol_set;
};

struct tplg_fw_module {
	uint8_t uuid[16];
	struct sof_fw_module_info info;
	uint64_t inst_used[(TPLG_MAX_MOD_INSTANCES + 63) / 64];
};

struct tplg_state {
	struct sof_dev *sdev;
	spinlock_t lock; /* protects `busy' and the ALH table */
	volatile int busy; /* an operation is running */

	int nfiles;
	char file_name[TPLG_MAX_FILES][64];

	int nw, nr, np, npcm, nlink;
	int wcap, rcap; /* allocated entries of w[] and r[] */
	struct tplg_widget *w; /* grows up to TPLG_MAX_WIDGETS */
	struct tplg_route *r; /* grows up to TPLG_MAX_ROUTES */
	struct tplg_pipe pipes[TPLG_MAX_PIPES];
	struct tplg_pcm_int pcm[TPLG_MAX_PCMS];
	struct tplg_link link[TPLG_MAX_LINKS];

	/* firmware module table (from the hook or the firmware's list) */
	int nmods;
	int mods_queried;
	struct tplg_fw_module mods[TPLG_MAX_MODULES];

	uint64_t pipe_ids_used; /* pipeline instance id bitmap */
	uint32_t alh_groups_used; /* multi-gateway group bitmap */

	/* NHLT blobs embedded in topology manifests */
	int nnhlt;
	uint8_t *nhlt[TPLG_MAX_NHLT];
	uint32_t nhlt_len[TPLG_MAX_NHLT];

	/* ALH stream ids the SoundWire side assigned [link][dir], -1, and
	 * the link DMA tags attached to them (ACE 2+) */
	int alh_stream[SOF_MAX_SDW_LINKS][2];
	int alh_dma_tag[SOF_MAX_SDW_LINKS][2];

	uint16_t abi_major, abi_minor, abi_patch;
	uint32_t ipc_errors;
};

/* host test builds replace the kernel's locking with nothing */
#ifdef SOF_TPLG_HOST_TEST
#define TPLG_LOCK_DECL
#define tplg_lock_irqsave(l, f) ((void)(l), (void)(f))
#define tplg_unlock_irqrestore(l, f) ((void)(l), (void)(f))
#define tplg_lock_init(l, n) ((void)(l), (void)(n))
#else
#define tplg_lock_irqsave(l, f) spin_lock_irqsave((l), (f))
#define tplg_unlock_irqrestore(l, f) spin_unlock_irqrestore((l), (f))
#define tplg_lock_init(l, n) spinlock_init((l), (n))
#endif

#endif /* KERNEL_DEV_SOUND_SOF_TPLG_INTERNAL_H */
