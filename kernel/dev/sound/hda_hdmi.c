// LikeOS Intel High Definition Audio - HDMI and DisplayPort display codecs.
//
// A display codec is the audio function inside a graphics chip (Intel
// integrated graphics, NVIDIA and AMD discrete cards, a few chipset and
// SoC parts).  It has digital converters (the DMA streams' destination)
// and digital pin widgets, one per display port, sometimes with a list of
// "device entries" per pin for DisplayPort multi-stream.  The monitor
// describes itself in its ELD (EDID-Like Data): name, connection type,
// speaker allocation and the short audio descriptors that say which
// channel counts, rates and sample sizes it takes.
//
// This driver takes such a codec over completely (the generic analog
// parser does nothing with it):
//
//   * enumeration of pins, converters and device entries, with the vendor
//     tricks needed to see them all (Intel's "enable all pins and
//     converters" and DP 1.2 vendor verb);
//   * presence and ELD tracking through unsolicited responses, the display
//     driver's notification (snd_display_audio_notify -> hda_hdmi_notify)
//     and its ELD (snd_display_audio_ops()->get_eld), or polling;
//   * strict ELD parsing (every length and count checked against the
//     buffer), and an emulated ELD for AMD codecs from their vendor verbs;
//   * one PCM device per pin ("HDMI 0", "HDMI 1", ...) on the controller's
//     card, bound dynamically to the pins that have a monitor when there
//     are more pins than PCM devices;
//   * stream setup: converter selection (no two pins share a converter on
//     Intel), CEA-861 channel allocation and channel-slot mapping, the
//     audio InfoFrame through the DIP buffer (HDMI and DisplayPort
//     layouts, the NVIDIA DisplayPort layout), AMD's channel allocation
//     and multichannel vendor verbs, the display driver's audio clock
//     (sync_audio_rate) and power well.
//
// Locking: each codec has a sleeping lock (hh_lock) that serialises verb
// sequences, the pin state and the PCM slot bindings.  It is taken from
// process context only: PCM open/close/hw_params/prepare, the ksnd work
// thread (unsolicited responses, notifications, polling) and the probe.
// The PCM trigger and pointer callbacks run under the core's substream
// spinlock with interrupts off and only touch the stream engine.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda.h"
#include "hda_hdmi.h"
#include <kernel/ke/syscall.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ---- limits ------------------------------------------------------------------- */

#define HH_MAX_PINS 16 /* digital pins tracked per codec */
#define HH_MAX_CVTS 16 /* digital converters tracked per codec */
#define HH_MAX_CONNS 16 /* connection list entries kept per pin */
#define HH_MAX_DEVS 8 /* DisplayPort MST device entries per pin */
#define HH_MAX_NODES 128 /* widgets scanned under the function group */
#define HH_MAX_REG 16 /* display codecs in the whole system */

#define HH_ELD_MAX 256 /* the largest ELD buffer a codec may report */
#define HH_ELD_FIXED 20 /* header + baseline bytes before the monitor name */
#define HH_ELD_MAX_MNL 16
#define HH_ELD_MAX_SAD 15

#define HH_REPOLL_MS 300 /* ELD not readable yet: look again after */
#define HH_REPOLL_TRIES 6
#define HH_POLL_MS 2000 /* no unsolicited responses: poll this often */

/* ---- verbs and fields not in hda_verbs.h ----------------------------------------- */

/* GET_HDMI_DIP_SIZE payload: the ELD buffer instead of a DIP packet */
#define HH_DIPSIZE_ELD_BUF (1u << 3)
#define HH_DIPSIZE_PACK_IDX 0x07u
/* GET_HDMI_ELDD response */
#define HH_ELDD_VALID (1u << 31)
#define HH_ELDD_DATA 0xffu
/* DIP transmit control */
#define HH_DIPXMIT_DISABLE 0x00u
#define HH_DIPXMIT_ONCE 0x80u
#define HH_DIPXMIT_BEST 0xc0u
#define HH_DIPXMIT_MASK 0xc0u
/* DIGI_CONVERT_3 (bits 23:16 of the GET_DIGI_CONVERT_1 response) */
#define HH_DIG3_ICT 0x0fu
#define HH_DIG3_KAE (1u << 7)
/* device list */
#define HH_DEVLIST_LEN_MASK 0x3fu
#define HH_DE_PD (1u << 0)
#define HH_DE_ELDV (1u << 1)
#define HH_DE_IA (1u << 2)
/* the stream channel id that leaves a slot unused */
#define HH_SLOT_UNUSED 0xfu

/* Intel: a vendor widget whose verb exposes the hidden pins/converters
 * and turns on DisplayPort 1.2 (device entries). */
#define INTEL_VERB_GET_VENDOR 0xf81
#define INTEL_VERB_SET_VENDOR 0x781
#define INTEL_VENDOR_ALL_PINS_CVTS (1u << 0)
#define INTEL_VENDOR_DP12 (1u << 1)

/* AMD/ATI pin and converter verbs */
#define ATI_VERB_SET_CHANNEL_ALLOCATION 0x771
#define ATI_VERB_SET_DOWNMIX_INFO 0x772
#define ATI_VERB_SET_AUDIO_DESCRIPTOR 0x776
#define ATI_VERB_SET_MULTICHANNEL_01 0x777
#define ATI_VERB_SET_HBR_CONTROL 0x77c
#define ATI_VERB_SET_SINK_INFO_INDEX 0x780
#define ATI_VERB_SET_MULTICHANNEL_1 0x785
#define ATI_VERB_SET_MULTICHANNEL_MODE 0x789
#define ATI_VERB_SET_RAMP_RATE 0x770
#define ATI_VERB_GET_SPEAKER_ALLOCATION 0xf70
#define ATI_VERB_GET_CHANNEL_ALLOCATION 0xf71
#define ATI_VERB_GET_AUDIO_DESCRIPTOR 0xf76
#define ATI_VERB_GET_MULTICHANNEL_01 0xf77
#define ATI_VERB_GET_AUDIO_VIDEO_DELAY 0xf7b
#define ATI_VERB_GET_HBR_CONTROL 0xf7c
#define ATI_VERB_GET_SINK_INFO_DATA 0xf81
#define ATI_SPKALLOC_MASK 0x007fu
#define ATI_SPKALLOC_TYPE_HDMI 0x0100u
#define ATI_SPKALLOC_TYPE_DP 0x0200u
#define ATI_AUDIODESC_CHANNELS 0x00000007u
#define ATI_AUDIODESC_RATES 0x0000ff00u
#define ATI_AUDIODESC_LPCM_STEREO_RATES 0xff000000u
#define ATI_DELAY_VIDEO_LATENCY 0x000000ffu
#define ATI_DELAY_AUDIO_LATENCY 0x0000ff00u
#define ATI_OUT_ENABLE 0x1u
#define ATI_MULTICHANNEL_MODE_PAIRED 0
#define ATI_MULTICHANNEL_MODE_SINGLE 1
#define ATI_HBR_CAPABLE 0x01u
#define ATI_HBR_ENABLE 0x10u
#define ATI_INFO_IDX_MANUFACTURER_ID 0
#define ATI_INFO_IDX_PRODUCT_ID 1
#define ATI_INFO_IDX_SINK_DESC_LEN 2
#define ATI_INFO_IDX_PORT_ID_LOW 3
#define ATI_INFO_IDX_PORT_ID_HIGH 4
#define ATI_INFO_IDX_SINK_DESC_FIRST 5
#define ATI_RAMP_RATE_DEFAULT 180

/* NVIDIA MCP7x chipset HDMI (before the HDMI verbs existed) */
#define NV_MCP_VERB_SET_CHANNEL_ALLOCATION 0xf79
#define NV_MCP_VERB_SET_INFOFRAME_CHECKSUM 0xf7a
#define NV_MCP_VERB_SET_AUDIO_PROTECTION_ON 0xf98
#define NV_MCP_MASTER_CVT 0x04
#define NV_MCP_MASTER_PIN 0x05

/* ---- codec families -------------------------------------------------------------- */

enum hh_family {
	HH_GENERIC = 0, /* standard HDA HDMI verbs only */
	HH_INTEL_G4X, /* Bearlake .. Eaglelake, CedarTrail, Crestline */
	HH_INTEL_CPT, /* Ibex Peak, Cougar Point, Panther Point */
	HH_INTEL_BYT, /* Valleyview, Braswell */
	HH_INTEL_HSW, /* Haswell .. Kaby Lake, Broxton */
	HH_INTEL_GLK, /* Gemini Lake, Cannon Lake */
	HH_INTEL_ICL, /* Ice Lake, Jasper/Elkhart Lake */
	HH_INTEL_TGL, /* Tiger Lake, Rocket/Alder(-S)/Raptor Lake, DG1/DG2 */
	HH_INTEL_ADLP, /* Alder Lake-P and later: keep-alive capable */
	HH_NV_MCP, /* NVIDIA MCP67..MCP7A chipsets */
	HH_NV_LEGACY, /* NVIDIA GT21x .. GF1xx */
	HH_NV, /* NVIDIA Kepler and later (DP MST) */
	HH_NV_TEGRA, /* NVIDIA Tegra SoCs */
	HH_AMD, /* AMD/ATI R6xx and later (ELD emulated, vendor verbs) */
	HH_AMD_OLD, /* RS600/RS690/RS780 */
	HH_VIA, /* VIA VX900 */
	HH_ZX, /* Zhaoxin / Glenfly / Arise */
	HH_LOONGSON,
};

struct hh_model {
	uint32_t id;
	uint8_t family;
	const char *vendor;
	const char *name;
};

static const struct hh_model hh_models[] = {
	/* Intel */
	{ 0x80860054, HH_INTEL_CPT, "Intel", "Ibex Peak HDMI" },
	{ 0x80862800, HH_INTEL_GLK, "Intel", "Gemini Lake HDMI" },
	{ 0x80862801, HH_INTEL_G4X, "Intel", "Bearlake HDMI" },
	{ 0x80862802, HH_INTEL_G4X, "Intel", "Cantiga HDMI" },
	{ 0x80862803, HH_INTEL_G4X, "Intel", "Eaglelake HDMI" },
	{ 0x80862804, HH_INTEL_CPT, "Intel", "Ibex Peak HDMI" },
	{ 0x80862805, HH_INTEL_CPT, "Intel", "Cougar Point HDMI" },
	{ 0x80862806, HH_INTEL_CPT, "Intel", "Panther Point HDMI" },
	{ 0x80862807, HH_INTEL_HSW, "Intel", "Haswell HDMI" },
	{ 0x80862808, HH_INTEL_HSW, "Intel", "Broadwell HDMI" },
	{ 0x80862809, HH_INTEL_HSW, "Intel", "Skylake HDMI" },
	{ 0x8086280a, HH_INTEL_HSW, "Intel", "Broxton HDMI" },
	{ 0x8086280b, HH_INTEL_HSW, "Intel", "Kaby Lake HDMI" },
	{ 0x8086280c, HH_INTEL_GLK, "Intel", "Cannon Lake HDMI" },
	{ 0x8086280d, HH_INTEL_GLK, "Intel", "Gemini Lake HDMI" },
	{ 0x8086280f, HH_INTEL_ICL, "Intel", "Ice Lake HDMI" },
	{ 0x80862812, HH_INTEL_TGL, "Intel", "Tiger Lake HDMI" },
	{ 0x80862814, HH_INTEL_TGL, "Intel", "DG1 HDMI" },
	{ 0x80862815, HH_INTEL_TGL, "Intel", "Alder Lake HDMI" },
	{ 0x80862816, HH_INTEL_TGL, "Intel", "Rocket Lake HDMI" },
	{ 0x80862818, HH_INTEL_TGL, "Intel", "Raptor Lake HDMI" },
	{ 0x80862819, HH_INTEL_TGL, "Intel", "DG2 HDMI" },
	{ 0x8086281a, HH_INTEL_ICL, "Intel", "Jasper Lake HDMI" },
	{ 0x8086281b, HH_INTEL_ICL, "Intel", "Elkhart Lake HDMI" },
	{ 0x8086281c, HH_INTEL_ADLP, "Intel", "Alder Lake-P HDMI" },
	{ 0x8086281d, HH_INTEL_ADLP, "Intel", "Meteor Lake HDMI" },
	{ 0x8086281e, HH_INTEL_ADLP, "Intel", "Battlemage HDMI" },
	{ 0x8086281f, HH_INTEL_ADLP, "Intel", "Raptor Lake-P HDMI" },
	{ 0x80862820, HH_INTEL_ADLP, "Intel", "Lunar Lake HDMI" },
	{ 0x80862822, HH_INTEL_ADLP, "Intel", "Panther Lake HDMI" },
	{ 0x80862823, HH_INTEL_ADLP, "Intel", "Wildcat Lake HDMI" },
	{ 0x80862824, HH_INTEL_ADLP, "Intel", "Nova Lake HDMI" },
	{ 0x80862880, HH_INTEL_G4X, "Intel", "Cedar Trail HDMI" },
	{ 0x80862882, HH_INTEL_BYT, "Intel", "Valleyview2 HDMI" },
	{ 0x80862883, HH_INTEL_BYT, "Intel", "Braswell HDMI" },
	{ 0x808629fb, HH_INTEL_G4X, "Intel", "Crestline HDMI" },
	/* NVIDIA chipsets */
	{ 0x10de0001, HH_NV_MCP, "NVIDIA", "MCP73 HDMI" },
	{ 0x10de0002, HH_NV_MCP, "NVIDIA", "MCP77/78 HDMI" },
	{ 0x10de0003, HH_NV_MCP, "NVIDIA", "MCP77/78 HDMI" },
	{ 0x10de0004, HH_NV_MCP, "NVIDIA", "GPU 04 HDMI" },
	{ 0x10de0005, HH_NV_MCP, "NVIDIA", "MCP77/78 HDMI" },
	{ 0x10de0006, HH_NV_MCP, "NVIDIA", "MCP77/78 HDMI" },
	{ 0x10de0007, HH_NV_MCP, "NVIDIA", "MCP79/7A HDMI" },
	{ 0x10de0067, HH_NV_MCP, "NVIDIA", "MCP67 HDMI" },
	{ 0x10de8001, HH_NV_MCP, "NVIDIA", "MCP73 HDMI" },
	{ 0x10de8067, HH_NV_MCP, "NVIDIA", "MCP67/68 HDMI" },
	/* AMD/ATI */
	{ 0x1002793c, HH_AMD_OLD, "ATI", "RS600 HDMI" },
	{ 0x10027919, HH_AMD_OLD, "ATI", "RS600 HDMI" },
	{ 0x1002791a, HH_AMD_OLD, "ATI", "RS690/780 HDMI" },
	{ 0x1002aa01, HH_AMD, "AMD", "Radeon HDMI/DP" },
	/* others */
	{ 0x11069f80, HH_VIA, "VIA", "VX900 HDMI/DP" },
	{ 0x11069f81, HH_VIA, "VIA", "VX900 HDMI/DP" },
	{ 0x11069f84, HH_GENERIC, "VIA", "VX11 HDMI/DP" },
	{ 0x11069f85, HH_GENERIC, "VIA", "VX11 HDMI/DP" },
	{ 0x17e80047, HH_GENERIC, "Chrontel", "HDMI" },
	{ 0x10951390, HH_GENERIC, "Silicon Image", "SiI1390 HDMI" },
	{ 0x10951392, HH_GENERIC, "Silicon Image", "SiI1392 HDMI" },
	{ 0x1d179f86, HH_ZX, "Zhaoxin", "ZX-100S HDMI/DP" },
	{ 0x1d179f87, HH_ZX, "Zhaoxin", "ZX-100S HDMI/DP" },
	{ 0x1d179f88, HH_ZX, "Zhaoxin", "KX-5000 HDMI/DP" },
	{ 0x1d179f89, HH_ZX, "Zhaoxin", "KX-5000 HDMI/DP" },
	{ 0x1d179f8a, HH_ZX, "Zhaoxin", "KX-6000 HDMI/DP" },
	{ 0x1d179f8b, HH_ZX, "Zhaoxin", "KX-6000 HDMI/DP" },
	{ 0x1d179f8c, HH_ZX, "Zhaoxin", "KX-6000G HDMI/DP" },
	{ 0x1d179f8d, HH_ZX, "Zhaoxin", "KX-6000G HDMI/DP" },
	{ 0x1d179f8e, HH_ZX, "Zhaoxin", "KX-7000 HDMI/DP" },
	{ 0x1d179f8f, HH_ZX, "Zhaoxin", "KX-7000 HDMI/DP" },
	{ 0x1d179f90, HH_ZX, "Zhaoxin", "KX-7000 HDMI/DP" },
	{ 0x67663d82, HH_ZX, "Arise", "Arise 82 HDMI/DP" },
	{ 0x67663d83, HH_ZX, "Arise", "Arise 83 HDMI/DP" },
	{ 0x67663d84, HH_ZX, "Arise", "Arise 84 HDMI/DP" },
	{ 0x67663d85, HH_ZX, "Arise", "Arise 85 HDMI/DP" },
	{ 0x67663d86, HH_ZX, "Arise", "Arise 86 HDMI/DP" },
	{ 0x67663d87, HH_ZX, "Arise", "Arise 87 HDMI/DP" },
	{ 0x00147a47, HH_LOONGSON, "Loongson", "HDMI" },
	{ 0x4c545020, HH_GENERIC, "Lisuan", "HDMI/DP" },
	{ 0, 0, NULL, NULL },
};

/* The pin of each display port (index = the display driver's port: 0 =
 * DDI A).  0 = the port has no audio pin. */
static const uint8_t hh_icl_port_map[] = { 0x00, 0x04, 0x06, 0x08, 0x0a, 0x0b };
static const uint8_t hh_tgl_port_map[] = { 0x04, 0x06, 0x08, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };

/* ---- CEA-861 speakers and channel allocations ----------------------------------- */

enum {
	SPK_FL = 1u << 0,
	SPK_FR = 1u << 1,
	SPK_FC = 1u << 2,
	SPK_LFE = 1u << 3,
	SPK_RL = 1u << 4,
	SPK_RR = 1u << 5,
	SPK_RC = 1u << 6,
	SPK_FLC = 1u << 7,
	SPK_FRC = 1u << 8,
	SPK_RLC = 1u << 9,
	SPK_RRC = 1u << 10,
	SPK_FLW = 1u << 11,
	SPK_FRW = 1u << 12,
	SPK_FLH = 1u << 13,
	SPK_FRH = 1u << 14,
	SPK_TC = 1u << 15,
	SPK_FCH = 1u << 16,
};

/* What each bit of the ELD's speaker allocation byte means (CEA-861
 * speaker allocation data block, first payload byte). */
static const uint32_t hh_eld_spk_bits[7] = {
	SPK_FL | SPK_FR, /* 0: FL/FR */
	SPK_LFE, /* 1: LFE */
	SPK_FC, /* 2: FC */
	SPK_RL | SPK_RR, /* 3: RL/RR */
	SPK_RC, /* 4: RC */
	SPK_FLC | SPK_FRC, /* 5: FLC/FRC */
	SPK_RLC | SPK_RRC, /* 6: RLC/RRC */
};

static const char *const hh_eld_spk_names[7] = {
	"FL/FR", "LFE", "FC", "RL/RR", "RC", "FLC/FRC", "RLC/RRC",
};

/* One channel allocation: which speaker each of the eight HDMI audio
 * slots carries (slot 0 first). */
struct hh_ca {
	uint8_t ca;
	uint8_t nch;
	uint32_t mask;
	uint32_t slot[8];
};

/* Slots 4-7 of allocations 0x00-0x1f, chosen by CA bits 4:2 (bit 0 is
 * LFE in slot 2, bit 1 FC in slot 3). */
static const uint32_t hh_ca_upper[8][4] = {
	{ 0, 0, 0, 0 },
	{ SPK_RC, 0, 0, 0 },
	{ SPK_RL, SPK_RR, 0, 0 },
	{ SPK_RL, SPK_RR, SPK_RC, 0 },
	{ SPK_RL, SPK_RR, SPK_RLC, SPK_RRC },
	{ 0, 0, SPK_FLC, SPK_FRC },
	{ SPK_RC, 0, SPK_FLC, SPK_FRC },
	{ SPK_RL, SPK_RR, SPK_FLC, SPK_FRC },
};

/* Allocations 0x20-0x31 (slot 3 FC where `fc', slot 2 LFE on odd CAs). */
static const struct {
	uint8_t fc;
	uint32_t s4, s5, s6, s7;
} hh_ca_high[9] = {
	{ 1, SPK_RL, SPK_RR, SPK_FCH, 0 }, /* 0x20/21 */
	{ 1, SPK_RL, SPK_RR, 0, SPK_TC }, /* 0x22/23 */
	{ 0, SPK_RL, SPK_RR, SPK_FLH, SPK_FRH }, /* 0x24/25 */
	{ 0, SPK_RL, SPK_RR, SPK_FLW, SPK_FRW }, /* 0x26/27 */
	{ 1, SPK_RL, SPK_RR, SPK_RC, SPK_TC }, /* 0x28/29 */
	{ 1, SPK_RL, SPK_RR, SPK_RC, SPK_FCH }, /* 0x2a/2b */
	{ 1, SPK_RL, SPK_RR, SPK_FCH, SPK_TC }, /* 0x2c/2d */
	{ 1, SPK_RL, SPK_RR, SPK_FLH, SPK_FRH }, /* 0x2e/2f */
	{ 1, SPK_RL, SPK_RR, SPK_FLW, SPK_FRW }, /* 0x30/31 */
};

#define HH_NUM_CA 0x32
static struct hh_ca hh_ca_table[HH_NUM_CA];
static int hh_ca_ready;

/* The speaker each stream channel stands for, by channel count, in the
 * order a WAVE file (and the audio service) interleaves them: front
 * left/right, centre, LFE, back, side. */
static const uint32_t hh_layout[9][8] = {
	{ 0 },
	{ SPK_FL },
	{ SPK_FL, SPK_FR },
	{ SPK_FL, SPK_FR, SPK_FC },
	{ SPK_FL, SPK_FR, SPK_RL, SPK_RR },
	{ SPK_FL, SPK_FR, SPK_FC, SPK_RL, SPK_RR },
	{ SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_RL, SPK_RR },
	{ SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_RC, SPK_RL, SPK_RR },
	{ SPK_FL, SPK_FR, SPK_FC, SPK_LFE, SPK_RLC, SPK_RRC, SPK_RL, SPK_RR },
};

static void hh_ca_init(void)
{
	if (hh_ca_ready)
		return;
	for (int ca = 0; ca < HH_NUM_CA; ca++) {
		struct hh_ca *e = &hh_ca_table[ca];
		mm_memset(e, 0, sizeof(*e));
		e->ca = (uint8_t)ca;
		e->slot[0] = SPK_FL;
		e->slot[1] = SPK_FR;
		if (ca < 0x20) {
			if (ca & 1)
				e->slot[2] = SPK_LFE;
			if (ca & 2)
				e->slot[3] = SPK_FC;
			for (int s = 0; s < 4; s++)
				e->slot[4 + s] = hh_ca_upper[(ca >> 2) & 7][s];
		} else {
			int h = (ca - 0x20) / 2;
			if (ca & 1)
				e->slot[2] = SPK_LFE;
			if (hh_ca_high[h].fc)
				e->slot[3] = SPK_FC;
			e->slot[4] = hh_ca_high[h].s4;
			e->slot[5] = hh_ca_high[h].s5;
			e->slot[6] = hh_ca_high[h].s6;
			e->slot[7] = hh_ca_high[h].s7;
		}
		for (int s = 0; s < 8; s++) {
			if (e->slot[s]) {
				e->nch++;
				e->mask |= e->slot[s];
			}
		}
	}
	hh_ca_ready = 1;
}

static int hh_popcount(uint32_t v)
{
	int n = 0;
	while (v) {
		v &= v - 1;
		n++;
	}
	return n;
}

/* ---- ELD ------------------------------------------------------------------------------ */

struct hh_sad {
	uint8_t format; /* 1 = LPCM, 2 = AC-3, ... */
	uint8_t channels; /* 1..8 */
	uint8_t rates; /* bit 0 32 kHz .. bit 6 192 kHz */
	uint8_t byte2; /* LPCM: bit 0 16, bit 1 20, bit 2 24 bit */
};

struct hh_eld {
	int valid; /* parsed and usable */
	int len; /* bytes in raw */
	uint8_t raw[HH_ELD_MAX];
	uint8_t ver;
	uint8_t cea_ver;
	uint8_t mnl;
	uint8_t sad_count;
	uint8_t conn_type; /* 0 HDMI, 1 DisplayPort */
	uint8_t supports_ai;
	uint8_t supports_hdcp;
	uint8_t aud_synch_delay; /* 2 ms units */
	uint8_t spk_alloc;
	uint16_t manufacturer;
	uint16_t product;
	char monitor[HH_ELD_MAX_MNL + 1];
	struct hh_sad sad[HH_ELD_MAX_SAD];
	/* what the LPCM descriptors allow together */
	uint32_t lpcm_rates; /* AUDIO_RATE_* */
	uint32_t lpcm_bits; /* bit 0 16, bit 1 20, bit 2 24 */
	uint32_t lpcm_max_ch;
	uint32_t spk_mask; /* SPK_* */
};

static const char *const hh_sad_format_names[16] = {
	"reserved", "LPCM", "AC-3", "MPEG1", "MP3", "MPEG2", "AAC", "DTS",
	"ATRAC", "SACD", "E-AC-3", "DTS-HD", "MLP", "DST", "WMA Pro", "extended",
};

/* Parse an ELD of `len' bytes into `e' (whose raw copy is kept).  Every
 * field is range-checked against the buffer: a malformed ELD from a
 * broken monitor or a codec still being written must not make us read
 * past it.  Returns 0 or -EINVAL. */
static int hh_eld_parse(const uint8_t *buf, int len, struct hh_eld *e)
{
	int baseline, mnl, nsad;

	mm_memset(e, 0, sizeof(*e));
	if (!buf || len < HH_ELD_FIXED || len > HH_ELD_MAX)
		return -EINVAL;
	mm_memcpy(e->raw, buf, (size_t)len);
	e->len = len;

	e->ver = (uint8_t)(buf[0] >> 3);
	/* 2: CEA-861-D or below; 31: a "partial" ELD some sources hand out */
	if (e->ver != 2 && e->ver != 31)
		return -EINVAL;
	baseline = buf[2] * 4;
	if (baseline <= 0)
		return -EINVAL;
	/* the declared block may be larger than what we hold only when the
	 * holder truncated it; everything parsed below stays inside `len' */
	if (4 + baseline < len)
		len = 4 + baseline;

	e->cea_ver = (uint8_t)(buf[4] >> 5);
	mnl = buf[4] & 0x1f;
	if (mnl > HH_ELD_MAX_MNL)
		return -EINVAL;
	e->mnl = (uint8_t)mnl;
	nsad = buf[5] >> 4;
	if (nsad > HH_ELD_MAX_SAD)
		return -EINVAL;
	e->conn_type = (uint8_t)((buf[5] >> 2) & 3);
	if (e->conn_type > 1)
		return -EINVAL;
	e->supports_ai = (uint8_t)((buf[5] >> 1) & 1);
	e->supports_hdcp = (uint8_t)(buf[5] & 1);
	e->aud_synch_delay = buf[6];
	if (e->aud_synch_delay > 0xfa)
		e->aud_synch_delay = 0;
	e->spk_alloc = buf[7] & 0x7f;
	e->manufacturer = (uint16_t)(buf[16] | (buf[17] << 8));
	e->product = (uint16_t)(buf[18] | (buf[19] << 8));

	if (HH_ELD_FIXED + mnl > len)
		return -EINVAL;
	for (int i = 0; i < mnl; i++) {
		uint8_t ch = buf[HH_ELD_FIXED + i];
		if (ch == 0)
			break;
		e->monitor[i] = (ch < 0x20 || ch > 0x7e) ? '?' : (char)ch;
	}
	for (int i = mnl - 1; i >= 0 && (e->monitor[i] == ' ' || e->monitor[i] == 0); i--)
		e->monitor[i] = 0;

	for (int i = 0; i < nsad; i++) {
		int off = HH_ELD_FIXED + mnl + 3 * i;
		struct hh_sad *s;
		if (off + 3 > len)
			return -EINVAL;
		s = &e->sad[e->sad_count];
		s->format = (uint8_t)((buf[off] >> 3) & 0x0f);
		s->channels = (uint8_t)((buf[off] & 7) + 1);
		s->rates = buf[off + 1] & 0x7f;
		s->byte2 = buf[off + 2];
		if (s->format == 0 || s->format == 15)
			continue; /* reserved/extended: not ours to play */
		e->sad_count++;
		if (s->format == 1) {
			/* SAD rate bit n is 32k, 44.1k, 48k, 88.2k, 96k,
			 * 176.4k, 192k: AUDIO_RATE_32000 is bit 4 */
			e->lpcm_rates |= (uint32_t)s->rates << 4;
			e->lpcm_bits |= s->byte2 & 7u;
			if (s->channels > e->lpcm_max_ch)
				e->lpcm_max_ch = s->channels;
		}
	}
	/* A sink that names no speakers takes at least stereo. */
	if (!e->spk_alloc)
		e->spk_mask = SPK_FL | SPK_FR;
	for (int b = 0; b < 7; b++)
		if (e->spk_alloc & (1u << b))
			e->spk_mask |= hh_eld_spk_bits[b];
	if (!e->sad_count)
		return -EINVAL;
	e->valid = 1;
	return 0;
}

/* "2ch 32-48 kHz 16/20/24 bit" for the log */
static void hh_eld_summary(const struct hh_eld *e, char *buf, size_t len)
{
	static const uint32_t khz[7] = { 32, 44, 48, 88, 96, 176, 192 };
	uint32_t lo = 0, hi = 0;
	for (int b = 0; b < 7; b++) {
		if (e->lpcm_rates & (1u << (b + 4))) {
			if (!lo)
				lo = khz[b];
			hi = khz[b];
		}
	}
	ksnprintf(buf, len, "%uch %u-%u kHz %s%s%s bit, %s", e->lpcm_max_ch, lo, hi,
		  (e->lpcm_bits & 1) ? "16" : "", (e->lpcm_bits & 2) ? "/20" : "",
		  (e->lpcm_bits & 4) ? "/24" : "", e->conn_type ? "DisplayPort" : "HDMI");
}

/* ---- per-codec state -------------------------------------------------------------------- */

struct hh_codec;

struct hh_pin {
	int index;
	uint8_t nid;
	uint32_t wcaps;
	uint32_t pincap;
	uint32_t cfg;
	uint8_t nconn;
	uint8_t conn[HH_MAX_CONNS];
	uint8_t is_hdmi, is_dp, hbr;
	uint8_t forced; /* default config said "not connected" */
	int ndev; /* device entries (1 = no list) */
	int dev; /* entry in use */
	int port; /* display driver port, -1 unknown */
	/* sink state */
	uint8_t present;
	struct hh_eld eld;
	int repolls;
	uint32_t sense_raw;
	uint32_t unsol_count;
	uint32_t eld_reads, eld_errors;
	/* routing */
	int cvt; /* converter index, -1 none */
	int mux; /* index of that converter in conn[] */
	int slot; /* PCM slot, -1 none */
	/* stream */
	uint32_t channels;
	int ca;
	uint8_t slot_ch[8]; /* stream channel carried by HDMI slot n */
	int streaming;
	int keepalive;
};

struct hh_cvt {
	uint8_t nid;
	uint32_t wcaps;
	uint32_t pcm; /* AC_PAR_PCM */
	uint32_t fmts; /* AC_PAR_STREAM */
	uint32_t max_ch;
	int pin; /* pin index using it, -1 free */
};

struct hh_slot {
	struct hh_codec *hc;
	int index;
	int global_index; /* "HDMI n" across the card */
	int pin; /* bound pin index, -1 */
	struct snd_pcm *pcm;
	struct hda_stream *st;
	int open;
	int powered; /* holds the display power */
	int prepared;
	uint32_t rate, channels, bits;
	uint16_t fmt;
	uint32_t out_id;
	/* last reported to the card */
	int rep_pin;
	int rep_present;
};

struct hh_codec {
	struct hda_codec *c;
	int reg; /* registry slot */
	uint32_t gen; /* registry generation (stale work detection) */
	const struct hh_model *model;
	uint8_t family;
	uint8_t vendor_nid; /* Intel vendor widget, 0 none */
	const uint8_t *port_map;
	int nport_map;
	int base_nid; /* Intel without a map: pin of port B */
	int max_dev; /* device entries used per pin */
	uint32_t afg_pcm, afg_fmts;
	int npins;
	struct hh_pin pin[HH_MAX_PINS];
	int ncvts;
	struct hh_cvt cvt[HH_MAX_CVTS];
	int nslots;
	struct hh_slot slot[SND_MAX_PCMS];
	/* flags */
	int mst; /* device entries are meaningful */
	int amd_rev3; /* AMD full channel remap */
	int dyn_pin_out; /* pin output enabled only while streaming */
	int nv_dp_layout; /* DP InfoFrame in the HDMI layout */
	int mcp; /* NVIDIA MCP: fixed converter and pin, 2 channels */
	int poll; /* no unsolicited responses: poll the pins */
	int use_display; /* ELD from the display driver */
	int muted;
	/* the lock */
	volatile int lock_word;
	task_t *lock_owner;
	int lock_depth;
	/* deferred work */
	volatile uint32_t pending; /* pins to look at */
	volatile int work_queued;
	volatile int poll_queued;
	volatile int busy;
	volatile int dead;
	char name[48];
	uint8_t eld_buf[HH_ELD_MAX]; /* scratch for reading an ELD */
};

/* Keep-alive (silent stream) on the codecs that support it: the display
 * keeps receiving an audio stream while nothing plays, so a monitor does
 * not cut the first second of every sound while its receiver wakes up.
 * Off by default: it keeps the converter and the display's audio busy. */
static const int hh_keepalive_enable = 0;

/* ---- registry ------------------------------------------------------------------------------ */

static spinlock_t g_hh_lock = SPINLOCK_INIT("hda_hdmi");
static struct hh_codec *g_hh[HH_MAX_REG];
static uint32_t g_hh_gen = 1;

static int hh_register(struct hh_codec *hc)
{
	uint64_t fl;
	int slot = -1;
	spin_lock_irqsave(&g_hh_lock, &fl);
	for (int i = 0; i < HH_MAX_REG; i++) {
		if (!g_hh[i]) {
			g_hh[i] = hc;
			slot = i;
			hc->reg = i;
			hc->gen = g_hh_gen++;
			if (!(g_hh_gen & 0x00ffffffu))
				g_hh_gen = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&g_hh_lock, fl);
	return slot;
}

static void hh_unregister(struct hh_codec *hc)
{
	uint64_t fl;
	spin_lock_irqsave(&g_hh_lock, &fl);
	if (hc->reg >= 0 && hc->reg < HH_MAX_REG && g_hh[hc->reg] == hc)
		g_hh[hc->reg] = NULL;
	hc->dead = 1;
	spin_unlock_irqrestore(&g_hh_lock, fl);
}

/* A work item carries (generation << 8 | registry slot): a codec removed
 * (and its slot reused) while the item waited is recognised. */
static void *hh_work_arg(struct hh_codec *hc)
{
	return (void *)(uintptr_t)(((uint64_t)hc->gen << 8) | (uint32_t)hc->reg);
}

/* Look a work item's codec up and pin it against removal (busy). */
static struct hh_codec *hh_get(void *arg)
{
	uint64_t v = (uint64_t)(uintptr_t)arg;
	int reg = (int)(v & 0xff);
	uint32_t gen = (uint32_t)(v >> 8);
	struct hh_codec *hc = NULL;
	uint64_t fl;

	if (reg < 0 || reg >= HH_MAX_REG)
		return NULL;
	spin_lock_irqsave(&g_hh_lock, &fl);
	if (g_hh[reg] && g_hh[reg]->gen == gen && !g_hh[reg]->dead) {
		hc = g_hh[reg];
		__sync_fetch_and_add(&hc->busy, 1);
	}
	spin_unlock_irqrestore(&g_hh_lock, fl);
	return hc;
}

static void hh_put(struct hh_codec *hc)
{
	__sync_fetch_and_sub(&hc->busy, 1);
}

static struct hh_codec *hh_of(struct hda_codec *c)
{
	if (!c || c->vendor != &hda_hdmi_driver || !c->vendor_data)
		return NULL;
	return (struct hh_codec *)c->vendor_data;
}

/* ---- the codec lock --------------------------------------------------------------------- */

static void hh_lock(struct hh_codec *hc)
{
	task_t *me = sched_current();

	if (hc->lock_depth > 0 && hc->lock_owner == me) {
		hc->lock_depth++;
		return;
	}
	while (__sync_lock_test_and_set(&hc->lock_word, 1))
		sched_yield_in_kernel();
	hc->lock_owner = me;
	hc->lock_depth = 1;
}

static void hh_unlock(struct hh_codec *hc)
{
	if (hc->lock_depth <= 0)
		return;
	if (--hc->lock_depth > 0)
		return;
	hc->lock_owner = NULL;
	__sync_lock_release(&hc->lock_word);
}

/* ---- verb helpers ----------------------------------------------------------------------- */

static uint32_t hh_rd(struct hh_codec *hc, uint8_t nid, uint32_t verb, uint32_t payload)
{
	return hda_codec_read(hc->c, nid, verb, payload);
}

static void hh_wr(struct hh_codec *hc, uint8_t nid, uint32_t verb, uint32_t payload)
{
	(void)hda_codec_write(hc->c, nid, verb, payload);
}

static uint32_t hh_par(struct hh_codec *hc, uint8_t nid, uint32_t par)
{
	return hda_param(hc->c, nid, par);
}

static int hh_is_intel(const struct hh_codec *hc)
{
	return hc->family >= HH_INTEL_G4X && hc->family <= HH_INTEL_ADLP;
}

/* Haswell and later: shared converters, vendor verbs, device entries. */
static int hh_is_intel_hsw_plus(const struct hh_codec *hc)
{
	return hc->family >= HH_INTEL_HSW && hc->family <= HH_INTEL_ADLP;
}

static int hh_is_amd(const struct hh_codec *hc)
{
	return hc->family == HH_AMD || hc->family == HH_AMD_OLD;
}

static int hh_is_nvidia(const struct hh_codec *hc)
{
	return hc->family >= HH_NV_MCP && hc->family <= HH_NV_TEGRA;
}

static uint32_t hh_wid_type(uint32_t wcaps)
{
	return (wcaps & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT;
}

/* channels a converter can carry: (CHAN_CNT_EXT << 1 | STEREO) + 1 */
static uint32_t hh_wcaps_channels(uint32_t wcaps)
{
	return ((((wcaps & AC_WCAP_CHAN_CNT_EXT) >> 13) << 1) | (wcaps & AC_WCAP_STEREO)) + 1;
}

/* Bring a widget to D0 (and wait for it on parts that need it). */
static void hh_power_d0(struct hh_codec *hc, uint8_t nid, int wait)
{
	uint32_t st = hh_rd(hc, nid, AC_VERB_GET_POWER_STATE, 0);
	if (st == 0xffffffffu)
		return;
	if (((st & AC_PWRST_ACTUAL) >> AC_PWRST_ACTUAL_SHIFT) == AC_PWRST_D0 &&
	    (st & AC_PWRST_SETTING) == AC_PWRST_D0)
		return;
	hh_wr(hc, nid, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	if (!wait)
		return;
	/* converters and pins left in D3 by the firmware take a while */
	for (int i = 0; i < 8; i++) {
		snd_mdelay(5);
		st = hh_rd(hc, nid, AC_VERB_GET_POWER_STATE, 0);
		if (st != 0xffffffffu &&
		    ((st & AC_PWRST_ACTUAL) >> AC_PWRST_ACTUAL_SHIFT) == AC_PWRST_D0)
			return;
	}
}

/* The connection list of a widget (the short and the long form, with
 * ranges), at most `max' entries; returns the count. */
static int hh_read_conns(struct hh_codec *hc, uint8_t nid, uint8_t *out, int max)
{
	uint32_t len = hh_par(hc, nid, AC_PAR_CONNLIST_LEN);
	int lng, n, per, shift, cnt = 0;
	uint32_t mask, range, prev = 0;

	if (len == 0xffffffffu)
		return 0;
	lng = (len & AC_CLIST_LONG) != 0;
	n = (int)(len & AC_CLIST_LENGTH);
	if (!n)
		return 0;
	if (n == 1 && !lng) {
		/* a single entry: the verb answers with it in bits 7:0 */
		uint32_t r = hh_rd(hc, nid, AC_VERB_GET_CONNECT_LIST, 0);
		if (r == 0xffffffffu)
			return 0;
		out[0] = (uint8_t)(r & 0x7f);
		return 1;
	}
	per = lng ? 2 : 4;
	shift = lng ? 16 : 8;
	mask = lng ? 0x7fffu : 0x7fu;
	range = lng ? 0x8000u : 0x80u;
	for (int i = 0; i < n && cnt < max; i += per) {
		uint32_t r = hh_rd(hc, nid, AC_VERB_GET_CONNECT_LIST, (uint32_t)i);
		if (r == 0xffffffffu)
			break;
		for (int j = 0; j < per && i + j < n && cnt < max; j++) {
			uint32_t v = (r >> (j * shift)) & ((1u << shift) - 1);
			uint32_t val = v & mask;
			if (!val)
				continue;
			if ((v & range) && prev && val > prev && val - prev <= 64) {
				for (uint32_t x = prev + 1; x <= val && cnt < max; x++)
					out[cnt++] = (uint8_t)x;
			} else if (val <= 0xff) {
				out[cnt++] = (uint8_t)val;
			}
			prev = val;
		}
	}
	return cnt;
}

static void hh_dev_select(struct hh_codec *hc, struct hh_pin *p, int dev)
{
	if (!hc->mst || p->ndev <= 1)
		return;
	if (dev < 0 || dev >= p->ndev)
		dev = 0;
	hh_wr(hc, p->nid, AC_VERB_SET_DEVICE_SEL, (uint32_t)dev);
}

/* The device entries of a pin (PD/ELDV/IA nibbles), at most `max'. */
static int hh_dev_list(struct hh_codec *hc, struct hh_pin *p, uint8_t *ent, int max)
{
	int n = p->ndev < max ? p->ndev : max;
	int got = 0;

	while (got < n) {
		uint32_t r = hh_rd(hc, p->nid, AC_VERB_GET_DEVICE_LIST, (uint32_t)got);
		if (r == 0xffffffffu)
			break;
		for (int i = 0; i < 8 && got < n; i++) {
			ent[got++] = (uint8_t)(r & 0xf);
			r >>= 4;
		}
	}
	return got;
}

/* Pick the device entry that carries the sink: one with presence and a
 * valid ELD, else one with presence, else the first. */
static void hh_pick_dev(struct hh_codec *hc, struct hh_pin *p)
{
	uint8_t ent[HH_MAX_DEVS];
	int n, best = -1;

	if (!hc->mst || p->ndev <= 1) {
		p->dev = 0;
		return;
	}
	n = hh_dev_list(hc, p, ent, HH_MAX_DEVS);
	for (int i = 0; i < n; i++)
		if ((ent[i] & HH_DE_PD) && (ent[i] & HH_DE_ELDV)) {
			best = i;
			break;
		}
	if (best < 0)
		for (int i = 0; i < n; i++)
			if (ent[i] & HH_DE_PD) {
				best = i;
				break;
			}
	p->dev = best < 0 ? 0 : best;
	hh_dev_select(hc, p, p->dev);
}

/* ---- the display side ------------------------------------------------------------------------- */

/* The display port of a pin (Intel), -1 when unknown. */
static int hh_pin_to_port(struct hh_codec *hc, uint8_t nid)
{
	if (!hh_is_intel(hc))
		return -1;
	if (hc->port_map) {
		for (int i = 0; i < hc->nport_map; i++)
			if (hc->port_map[i] && hc->port_map[i] == nid)
				return i;
		return -1;
	}
	if (hc->base_nid && nid >= hc->base_nid && nid < hc->base_nid + 3)
		return nid - hc->base_nid + 1; /* port B.. */
	return -1;
}

static int hh_port_to_pin_index(struct hh_codec *hc, int port)
{
	for (int i = 0; i < hc->npins; i++)
		if (hc->pin[i].port == port && port >= 0)
			return i;
	return -1;
}

static const struct snd_display_audio_ops *hh_display(struct hh_codec *hc, void **disp)
{
	const struct snd_display_audio_ops *ops;

	*disp = NULL;
	if (!hc->use_display)
		return NULL;
	ops = snd_display_audio_ops(disp);
	return ops;
}

static void hh_display_power(struct hh_codec *hc, int on)
{
	void *disp;
	const struct snd_display_audio_ops *ops = hh_display(hc, &disp);
	if (!ops)
		return;
	if (on && ops->get_power)
		ops->get_power(disp);
	else if (!on && ops->put_power)
		ops->put_power(disp);
}

/* ---- Intel vendor verbs ------------------------------------------------------------------------- */

static void hh_intel_vendor_bits(struct hh_codec *hc, uint32_t bits)
{
	uint32_t v;

	if (!hc->vendor_nid)
		return;
	v = hh_rd(hc, hc->vendor_nid, INTEL_VERB_GET_VENDOR, 0);
	if (v == 0xffffffffu || (v & bits) == bits)
		return;
	v |= bits;
	/* a read, so the write has completed before the tree is looked at */
	(void)hh_rd(hc, hc->vendor_nid, INTEL_VERB_SET_VENDOR, v & 0xff);
}

/* Show the hidden pins and converters and turn on DP 1.2 (the device
 * entries); repeated before every return to D0, or the converters may
 * answer nonsense. */
static void hh_intel_wake(struct hh_codec *hc)
{
	if (!hh_is_intel_hsw_plus(hc))
		return;
	hh_intel_vendor_bits(hc, INTEL_VENDOR_ALL_PINS_CVTS);
	hh_intel_vendor_bits(hc, INTEL_VENDOR_DP12);
}

/* ---- enumeration ----------------------------------------------------------------------------- */

static int hh_cvt_index(struct hh_codec *hc, uint8_t nid)
{
	for (int i = 0; i < hc->ncvts; i++)
		if (hc->cvt[i].nid == nid)
			return i;
	return -1;
}

static void hh_add_cvt(struct hh_codec *hc, uint8_t nid, uint32_t wcaps)
{
	struct hh_cvt *cv;

	if (hc->ncvts >= HH_MAX_CVTS)
		return;
	cv = &hc->cvt[hc->ncvts];
	mm_memset(cv, 0, sizeof(*cv));
	cv->nid = nid;
	cv->wcaps = wcaps;
	cv->pin = -1;
	cv->pcm = 0;
	cv->fmts = 0;
	if (wcaps & AC_WCAP_FORMAT_OVRD) {
		cv->pcm = hh_par(hc, nid, AC_PAR_PCM);
		cv->fmts = hh_par(hc, nid, AC_PAR_STREAM);
	}
	if (!cv->pcm || cv->pcm == 0xffffffffu)
		cv->pcm = hc->afg_pcm;
	if (!cv->fmts || cv->fmts == 0xffffffffu)
		cv->fmts = hc->afg_fmts;
	/* an HDMI converter always takes 48 kHz/16 bit stereo PCM */
	if (!(cv->pcm & AC_SUPPCM_RATES))
		cv->pcm |= (1u << 6);
	if (!(cv->pcm & (AC_SUPPCM_BITS_16 | AC_SUPPCM_BITS_20 | AC_SUPPCM_BITS_24 |
			 AC_SUPPCM_BITS_32)))
		cv->pcm |= AC_SUPPCM_BITS_16;
	cv->max_ch = hh_wcaps_channels(wcaps);
	/* AMD and Intel converters understate what they carry */
	if (hh_is_amd(hc)) {
		if (cv->max_ch < 8)
			cv->max_ch = 8;
		cv->pcm |= AC_SUPPCM_BITS_16 | AC_SUPPCM_BITS_20 | AC_SUPPCM_BITS_24;
		cv->pcm |= (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) |
			   (1u << 9) | (1u << 10);
	}
	if (hc->mcp)
		cv->max_ch = 2;
	if (cv->max_ch > 8)
		cv->max_ch = 8;
	if (cv->max_ch < 2)
		cv->max_ch = 2;
	hc->ncvts++;
}

static int hh_pin_wanted(uint32_t wcaps, uint32_t pincap)
{
	if (!(wcaps & AC_WCAP_DIGITAL))
		return 0;
	if (!(pincap & AC_PINCAP_OUT))
		return 0;
	return (pincap & (AC_PINCAP_HDMI | AC_PINCAP_DP)) != 0;
}

static void hh_add_pin(struct hh_codec *hc, uint8_t nid, uint32_t wcaps, uint32_t pincap,
		       uint32_t cfg, int forced)
{
	struct hh_pin *p;
	uint32_t devlen;

	if (hc->npins >= HH_MAX_PINS)
		return;
	p = &hc->pin[hc->npins];
	mm_memset(p, 0, sizeof(*p));
	p->index = hc->npins;
	p->nid = nid;
	p->wcaps = wcaps;
	p->pincap = pincap;
	p->cfg = cfg;
	p->forced = (uint8_t)forced;
	p->is_hdmi = (pincap & AC_PINCAP_HDMI) != 0;
	p->is_dp = (pincap & AC_PINCAP_DP) != 0;
	p->hbr = (pincap & AC_PINCAP_HBR) != 0;
	p->cvt = -1;
	p->mux = -1;
	p->slot = -1;
	p->port = -1;
	p->ndev = 1;
	if (hc->mst) {
		devlen = hh_par(hc, nid, AC_PAR_DEVLIST_LEN);
		if (devlen != 0xffffffffu) {
			p->ndev = (int)(devlen & HH_DEVLIST_LEN_MASK) + 1;
			if (p->ndev > hc->max_dev)
				p->ndev = hc->max_dev;
			if (p->ndev > HH_MAX_DEVS)
				p->ndev = HH_MAX_DEVS;
			if (p->ndev < 1)
				p->ndev = 1;
		}
	}
	if (wcaps & AC_WCAP_CONN_LIST)
		p->nconn = (uint8_t)hh_read_conns(hc, nid, p->conn, HH_MAX_CONNS);
	p->port = hh_pin_to_port(hc, nid);
	hc->npins++;
}

/* Walk the function group: digital converters and HDMI/DP pins.  Pins the
 * firmware marked "not connected" are taken only when no other pin is
 * left (some boards mark every display pin that way). */
static int hh_enumerate(struct hh_codec *hc)
{
	struct hda_codec *c = hc->c;
	uint32_t nc, start, n;
	uint8_t nc_nids[HH_MAX_PINS];
	uint32_t nc_wcaps[HH_MAX_PINS], nc_pincap[HH_MAX_PINS], nc_cfg[HH_MAX_PINS];
	int nnc = 0;

	hc->npins = 0;
	hc->ncvts = 0;
	if (!c->afg)
		return -ENODEV;
	hc->afg_pcm = hh_par(hc, c->afg, AC_PAR_PCM);
	hc->afg_fmts = hh_par(hc, c->afg, AC_PAR_STREAM);
	if (hc->afg_pcm == 0xffffffffu)
		hc->afg_pcm = 0;
	if (hc->afg_fmts == 0xffffffffu)
		hc->afg_fmts = AC_SUPFMT_PCM;
	nc = hh_par(hc, c->afg, AC_PAR_NODE_COUNT);
	if (nc == 0xffffffffu)
		return -EIO;
	start = (nc >> 16) & 0xff;
	n = nc & 0xff;
	if (!start)
		return -EIO;
	if (n > HH_MAX_NODES)
		n = HH_MAX_NODES;
	if (start + n > 0x100)
		n = 0x100 - start;

	for (uint32_t i = 0; i < n; i++) {
		uint8_t nid = (uint8_t)(start + i);
		uint32_t wcaps = hh_par(hc, nid, AC_PAR_AUDIO_WIDGET_CAP);
		uint32_t type;
		if (wcaps == 0xffffffffu)
			continue;
		type = hh_wid_type(wcaps);
		if (type == AC_WID_AUD_OUT) {
			if ((wcaps & AC_WCAP_DIGITAL) || hc->mcp)
				hh_add_cvt(hc, nid, wcaps);
		} else if (type == AC_WID_PIN) {
			uint32_t pincap = hh_par(hc, nid, AC_PAR_PIN_CAP);
			uint32_t cfg = hh_rd(hc, nid, AC_VERB_GET_CONFIG_DEFAULT, 0);
			if (pincap == 0xffffffffu)
				continue;
			if (!hh_pin_wanted(wcaps, pincap))
				continue;
			if (((cfg & AC_DEFCFG_PORT_CONN) >> AC_DEFCFG_PORT_CONN_SHIFT) ==
			    AC_JACK_PORT_NONE) {
				if (nnc < HH_MAX_PINS) {
					nc_nids[nnc] = nid;
					nc_wcaps[nnc] = wcaps;
					nc_pincap[nnc] = pincap;
					nc_cfg[nnc] = cfg;
					nnc++;
				}
				continue;
			}
			hh_add_pin(hc, nid, wcaps, pincap, cfg, 0);
		}
	}
	if (!hc->npins)
		for (int i = 0; i < nnc; i++)
			hh_add_pin(hc, nc_nids[i], nc_wcaps[i], nc_pincap[i], nc_cfg[i], 1);

	/* NVIDIA MCP7x: the pins predate the HDMI pin capability */
	if (hc->mcp && !hc->npins) {
		uint32_t wcaps = hh_par(hc, NV_MCP_MASTER_PIN, AC_PAR_AUDIO_WIDGET_CAP);
		if (wcaps != 0xffffffffu && hh_wid_type(wcaps) == AC_WID_PIN) {
			hh_add_pin(hc, NV_MCP_MASTER_PIN, wcaps,
				   AC_PINCAP_OUT | AC_PINCAP_HDMI | AC_PINCAP_PRES_DETECT,
				   hh_rd(hc, NV_MCP_MASTER_PIN, AC_VERB_GET_CONFIG_DEFAULT, 0), 1);
			hc->pin[0].nconn = 1;
			hc->pin[0].conn[0] = NV_MCP_MASTER_CVT;
		}
		if (hh_cvt_index(hc, NV_MCP_MASTER_CVT) < 0) {
			uint32_t cw = hh_par(hc, NV_MCP_MASTER_CVT, AC_PAR_AUDIO_WIDGET_CAP);
			if (cw != 0xffffffffu)
				hh_add_cvt(hc, NV_MCP_MASTER_CVT, cw);
		}
	}
	if (!hc->npins || !hc->ncvts)
		return -ENODEV;
	/* a pin whose connection list names no converter of ours is useless
	 * (except to MCP, handled above) */
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		int any = 0;
		for (int k = 0; k < p->nconn; k++)
			if (hh_cvt_index(hc, p->conn[k]) >= 0)
				any = 1;
		if (!any && p->nconn == 0 && hc->ncvts) {
			/* no connection list: the pin is wired to the
			 * converter of the same position */
			p->conn[0] = hc->cvt[i < hc->ncvts ? i : 0].nid;
			p->nconn = 1;
		}
	}
	return 0;
}

/* ---- pin and converter initialisation ----------------------------------------------------------- */

static void hh_pin_amp_unmute(struct hh_codec *hc, struct hh_pin *p, int mute)
{
	if (!(p->wcaps & AC_WCAP_OUT_AMP))
		return;
	hh_wr(hc, p->nid, AC_VERB_SET_AMP_GAIN_MUTE,
	      AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT | (mute ? AC_AMP_MUTE : 0));
}

static void hh_pin_ctl(struct hh_codec *hc, struct hh_pin *p, int out)
{
	uint32_t v = out ? AC_PINCTL_OUT_EN : 0;
	if (hc->mcp && out)
		v |= 0x05; /* MCP: the digital output wants these low bits */
	else if (p->hbr && out)
		v |= AC_PINCTL_EPT_NATIVE; /* LPCM, not HBR passthrough */
	hh_wr(hc, p->nid, AC_VERB_SET_PIN_WIDGET_CONTROL, v);
}

/* Intel: every pin starts on a converter of its own, so that a stream on
 * one pin does not come out of another one sharing its converter. */
static void hh_intel_spread_cvts(struct hh_codec *hc)
{
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		if (p->nconn < 2 || p->streaming)
			continue;
		int want = i % p->nconn;
		for (int d = 0; d < p->ndev; d++) {
			hh_dev_select(hc, p, d);
			hh_wr(hc, p->nid, AC_VERB_SET_CONNECT_SEL, (uint32_t)want);
		}
		hh_dev_select(hc, p, p->dev);
	}
}

static void hh_init_hw(struct hh_codec *hc)
{
	struct hda_codec *c = hc->c;

	hh_intel_wake(hc);
	if (c->afg)
		hh_wr(hc, c->afg, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	if (hc->mcp)
		hh_wr(hc, c->afg ? c->afg : 1, NV_MCP_VERB_SET_AUDIO_PROTECTION_ON, 1);

	for (int i = 0; i < hc->ncvts; i++) {
		struct hh_cvt *cv = &hc->cvt[i];
		hh_power_d0(hc, cv->nid, 0);
		if (cv->pin < 0) {
			hh_wr(hc, cv->nid, AC_VERB_SET_CHANNEL_STREAMID, 0);
			hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_1, 0);
		}
	}
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		hh_power_d0(hc, p->nid, 0);
		for (int d = 0; d < p->ndev; d++) {
			hh_dev_select(hc, p, d);
			hh_pin_amp_unmute(hc, p, hc->muted);
		}
		hh_dev_select(hc, p, p->dev);
		hh_pin_ctl(hc, p, hc->dyn_pin_out ? p->streaming : 1);
		if ((p->wcaps & AC_WCAP_UNSOL_CAP) && !hc->poll)
			hh_wr(hc, p->nid, AC_VERB_SET_UNSOLICITED_ENABLE,
			      AC_USRSP_EN | ((uint32_t)(i + 1) & AC_UNSOL_TAG));
		if (hh_is_amd(hc)) {
			hh_wr(hc, p->nid, ATI_VERB_SET_DOWNMIX_INFO, 0);
			if (hc->amd_rev3)
				hh_wr(hc, p->nid, ATI_VERB_SET_MULTICHANNEL_MODE,
				      ATI_MULTICHANNEL_MODE_SINGLE);
		}
	}
	if (hh_is_intel_hsw_plus(hc))
		hh_intel_spread_cvts(hc);
}

/* ---- reading the ELD ------------------------------------------------------------------------ */

/* Through the HDMI verbs, byte by byte.  Each byte carries a valid bit:
 * the display driver may be rewriting the buffer right now, in which
 * case we give up and look again later.  Returns the length or a
 * negative errno. */
static int hh_read_eld_verbs(struct hh_codec *hc, struct hh_pin *p, uint8_t *buf, int max)
{
	uint32_t size = hh_rd(hc, p->nid, AC_VERB_GET_HDMI_DIP_SIZE, HH_DIPSIZE_ELD_BUF);
	int n;

	if (size == 0xffffffffu)
		return -EIO;
	size &= 0x1ff;
	if (size == 0)
		size = 128; /* some codecs report 0 and still hold one */
	if (size < HH_ELD_FIXED || size > HH_ELD_MAX)
		return -ERANGE;
	n = (int)size;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++) {
		uint32_t v = hh_rd(hc, p->nid, AC_VERB_GET_HDMI_ELDD, (uint32_t)i);
		if (v == 0xffffffffu || !(v & HH_ELDD_VALID))
			return -EAGAIN;
		buf[i] = (uint8_t)(v & HH_ELDD_DATA);
		/* the version byte is never 0: a 0 here is a DVI sink or
		 * a buffer the graphics side has not filled yet */
		if (i == 0 && buf[0] == 0)
			return -EAGAIN;
	}
	return n;
}

/* AMD codecs have no ELD buffer: build one from their sink-information
 * and audio-descriptor vendor verbs. */
static int hh_read_eld_amd(struct hh_codec *hc, struct hh_pin *p, uint8_t *buf, int max)
{
	uint32_t spk, sad, synch;
	int pos, desc_len = 0;

	if (max < HH_ELD_FIXED + HH_ELD_MAX_MNL + 3 * HH_ELD_MAX_SAD)
		return -ENOSPC;
	spk = hh_rd(hc, p->nid, ATI_VERB_GET_SPEAKER_ALLOCATION, 0);
	if (spk == 0xffffffffu || spk == 0)
		return -ENOENT;
	mm_memset(buf, 0, (size_t)max);
	buf[0] = 2 << 3; /* CEA-861-D */
	buf[7] = (uint8_t)(spk & ATI_SPKALLOC_MASK);
	if (spk & ATI_SPKALLOC_TYPE_DP)
		buf[5] |= 1 << 2;
	pos = HH_ELD_FIXED;

	if (hc->amd_rev3) {
		uint32_t v;
		hh_wr(hc, p->nid, ATI_VERB_SET_SINK_INFO_INDEX, ATI_INFO_IDX_PORT_ID_LOW);
		v = hh_rd(hc, p->nid, ATI_VERB_GET_SINK_INFO_DATA, 0);
		for (int i = 0; i < 4; i++)
			buf[8 + i] = (uint8_t)(v >> (8 * i));
		hh_wr(hc, p->nid, ATI_VERB_SET_SINK_INFO_INDEX, ATI_INFO_IDX_PORT_ID_HIGH);
		v = hh_rd(hc, p->nid, ATI_VERB_GET_SINK_INFO_DATA, 0);
		for (int i = 0; i < 4; i++)
			buf[12 + i] = (uint8_t)(v >> (8 * i));
		hh_wr(hc, p->nid, ATI_VERB_SET_SINK_INFO_INDEX, ATI_INFO_IDX_MANUFACTURER_ID);
		v = hh_rd(hc, p->nid, ATI_VERB_GET_SINK_INFO_DATA, 0);
		buf[16] = (uint8_t)v;
		buf[17] = (uint8_t)(v >> 8);
		hh_wr(hc, p->nid, ATI_VERB_SET_SINK_INFO_INDEX, ATI_INFO_IDX_PRODUCT_ID);
		v = hh_rd(hc, p->nid, ATI_VERB_GET_SINK_INFO_DATA, 0);
		buf[18] = (uint8_t)v;
		buf[19] = (uint8_t)(v >> 8);
		hh_wr(hc, p->nid, ATI_VERB_SET_SINK_INFO_INDEX, ATI_INFO_IDX_SINK_DESC_LEN);
		v = hh_rd(hc, p->nid, ATI_VERB_GET_SINK_INFO_DATA, 0);
		desc_len = v == 0xffffffffu ? 0 : (int)(v & 0xff);
		if (desc_len > HH_ELD_MAX_MNL)
			desc_len = HH_ELD_MAX_MNL;
		buf[4] |= (uint8_t)desc_len;
		for (int i = 0; i < desc_len; i++) {
			hh_wr(hc, p->nid, ATI_VERB_SET_SINK_INFO_INDEX,
			      (uint32_t)(ATI_INFO_IDX_SINK_DESC_FIRST + i));
			v = hh_rd(hc, p->nid, ATI_VERB_GET_SINK_INFO_DATA, 0);
			buf[pos++] = (uint8_t)v;
		}
	}

	/* one descriptor per coding type: LPCM (1) .. WMA Pro (14), SACD (9)
	 * and DST (13) excepted */
	for (uint32_t t = 1; t <= 14; t++) {
		if (t == 9 || t == 13)
			continue;
		if (pos + 6 > HH_ELD_FIXED + desc_len + 3 * HH_ELD_MAX_SAD)
			break;
		hh_wr(hc, p->nid, ATI_VERB_SET_AUDIO_DESCRIPTOR, t << 3);
		sad = hh_rd(hc, p->nid, ATI_VERB_GET_AUDIO_DESCRIPTOR, 0);
		if (sad == 0xffffffffu || sad == 0)
			continue;
		if (sad & ATI_AUDIODESC_RATES) {
			buf[pos++] = (uint8_t)(sad & 0xff);
			buf[pos++] = (uint8_t)((sad >> 8) & 0xff);
			buf[pos++] = (uint8_t)((sad >> 16) & 0xff);
		}
		/* LPCM carries a separate stereo rate mask in the top byte */
		if (t == 1 && (sad & ATI_AUDIODESC_LPCM_STEREO_RATES) &&
		    ((sad & ATI_AUDIODESC_LPCM_STEREO_RATES) >> 16) != (sad & ATI_AUDIODESC_RATES)) {
			buf[pos++] = (uint8_t)(((sad & 0xff) & ~ATI_AUDIODESC_CHANNELS) | 0x1);
			buf[pos++] = (uint8_t)((sad >> 24) & 0xff);
			buf[pos++] = (uint8_t)((sad >> 16) & 0xff);
		}
	}
	if (pos == HH_ELD_FIXED + desc_len)
		return -ENOENT;

	/* video latency relative to audio, from the VSDB-style values */
	synch = hh_rd(hc, p->nid, ATI_VERB_GET_AUDIO_VIDEO_DELAY, 0);
	if (synch != 0xffffffffu && (synch & ATI_DELAY_VIDEO_LATENCY) &&
	    (synch & ATI_DELAY_AUDIO_LATENCY)) {
		uint32_t vl = synch & ATI_DELAY_VIDEO_LATENCY;
		uint32_t al = (synch & ATI_DELAY_AUDIO_LATENCY) >> 8;
		if (vl <= 0xfb && al <= 0xfb && vl > al)
			buf[6] = (uint8_t)(vl - al);
	}
	buf[5] |= (uint8_t)(((pos - HH_ELD_FIXED - desc_len) / 3) << 4);
	pos = (pos + 3) & ~3;
	if (pos > max)
		pos = max & ~3;
	buf[2] = (uint8_t)((pos - 4) / 4);
	return pos;
}

/* ---- presence, ELD and the PCM slots ---------------------------------------------------------- */

static void hh_update_slot_hw(struct hh_codec *hc, struct hh_slot *s);
static void hh_report_slots(struct hh_codec *hc);
static void hh_bind_slots(struct hh_codec *hc);
static void hh_restore_stream(struct hh_codec *hc, struct hh_pin *p);

/* Look at one pin: presence, the device entry in use and the ELD.
 * Returns 1 when the ELD could not be read yet and a later look is
 * wanted. */
static int hh_sense_pin(struct hh_codec *hc, int idx)
{
	struct hh_pin *p;
	uint8_t *buf = hc->eld_buf; /* the codec lock is held */
	struct hh_eld *neu;
	int present = 0, eldv = 0, n = -ENOENT, from_display = 0, changed;
	uint32_t sense;
	void *disp;
	const struct snd_display_audio_ops *ops;

	if (idx < 0 || idx >= hc->npins)
		return 0;
	p = &hc->pin[idx];
	neu = kalloc(sizeof(*neu));
	if (!neu)
		return 0;

	hh_pick_dev(hc, p);
	if (p->pincap & AC_PINCAP_TRIG_REQ) {
		hh_wr(hc, p->nid, AC_VERB_SET_PIN_SENSE, 0);
		snd_udelay(100);
	}
	sense = hh_rd(hc, p->nid, AC_VERB_GET_PIN_SENSE, 0);
	p->sense_raw = sense;
	if (sense != 0xffffffffu) {
		present = (sense & AC_PINSENSE_PRESENCE) != 0;
		eldv = present && (sense & AC_PINSENSE_ELDV);
	}
	if (hc->mcp) {
		/* no ELD on these: presence is all there is */
		eldv = 0;
	}

	/* Intel with a display driver that publishes ELDs: it knows what
	 * runs on the port, the codec only learns it from the registers
	 * the display wrote. */
	ops = hh_display(hc, &disp);
	if (ops && ops->get_eld && p->port >= 0) {
		mm_memset(buf, 0, HH_ELD_MAX);
		n = ops->get_eld(disp, p->port, buf, HH_ELD_MAX);
		if (n > HH_ELD_MAX)
			n = HH_ELD_MAX;
		if (n > 0) {
			from_display = 1;
			present = 1;
			eldv = 1;
		} else {
			/* the display has no audio sink there */
			present = 0;
			eldv = 0;
		}
	}
	if (!from_display && eldv) {
		p->eld_reads++;
		if (hh_is_amd(hc))
			n = hh_read_eld_amd(hc, p, buf, HH_ELD_MAX);
		else
			n = hh_read_eld_verbs(hc, p, buf, HH_ELD_MAX);
		if (n < 0)
			p->eld_errors++;
	} else if (!from_display && present && hh_is_amd(hc)) {
		/* AMD never sets ELDV: the emulation is the ELD */
		n = hh_read_eld_amd(hc, p, buf, HH_ELD_MAX);
		eldv = n > 0;
	}

	mm_memset(neu, 0, sizeof(*neu));
	if (eldv && n > 0 && hh_eld_parse(buf, n, neu) != 0) {
		neu->valid = 0;
		p->eld_errors++;
	}
	if (hc->mcp && present) {
		/* no ELD: a stereo LPCM sink at the standard rates */
		mm_memset(neu, 0, sizeof(*neu));
		neu->valid = 1;
		neu->ver = 2;
		neu->sad_count = 1;
		neu->sad[0].format = 1;
		neu->sad[0].channels = 2;
		neu->sad[0].rates = 0x07;
		neu->sad[0].byte2 = 0x01;
		neu->lpcm_rates = AUDIO_RATE_32000 | AUDIO_RATE_44100 | AUDIO_RATE_48000;
		neu->lpcm_bits = 1;
		neu->lpcm_max_ch = 2;
		neu->spk_mask = SPK_FL | SPK_FR;
		kstrncpy(neu->monitor, "HDMI sink", sizeof(neu->monitor) - 1);
	}

	/* presence without a readable ELD yet: look again a few times (the
	 * graphics side writes the buffer after it raises presence) */
	if (present && !neu->valid && !hc->mcp && p->repolls < HH_REPOLL_TRIES) {
		p->repolls++;
		kfree(neu);
		return 1;
	}
	p->repolls = 0;

	changed = (p->present != (uint8_t)present) || (p->eld.valid != neu->valid) ||
		  (neu->valid && (p->eld.len != neu->len ||
				  kmemcmp(p->eld.raw, neu->raw, (size_t)neu->len) != 0));
	p->present = (uint8_t)present;
	if (changed) {
		int was_valid = p->eld.valid;
		mm_memcpy(&p->eld, neu, sizeof(*neu));
		if (neu->valid) {
			char sum[80];
			hh_eld_summary(neu, sum, sizeof(sum));
			snd_info("hda", "%s: pin %02x%s: monitor \"%s\" (%s)\n", hc->name, p->nid,
				 p->ndev > 1 ? " dev" : "", neu->monitor[0] ? neu->monitor : "?",
				 sum);
		} else if (was_valid || present) {
			snd_info("hda", "%s: pin %02x: %s\n", hc->name, p->nid,
				 present ? "sink without usable audio" : "monitor gone");
		}
		/* the sink came back while its stream runs: the converter
		 * selection and InfoFrame may have been reset with the
		 * transcoder */
		if (neu->valid && !was_valid && p->streaming)
			hh_restore_stream(hc, p);
	}
	kfree(neu);
	return 0;
}

/* The pin a slot plays to has a monitor with usable audio. */
static int hh_pin_live(const struct hh_pin *p)
{
	return p->present && p->eld.valid;
}

/* Keep every PCM slot bound to a pin, preferring pins with a monitor.
 * A slot that is open keeps its pin whatever happens to the monitor. */
static void hh_bind_slots(struct hh_codec *hc)
{
	/* 1: monitors without a slot take a free slot, or one bound to a
	 * pin without a monitor that nobody has open */
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		if (!hh_pin_live(p) || p->slot >= 0)
			continue;
		int target = -1;
		for (int s = 0; s < hc->nslots && target < 0; s++)
			if (hc->slot[s].pin < 0)
				target = s;
		for (int s = 0; s < hc->nslots && target < 0; s++) {
			struct hh_slot *sl = &hc->slot[s];
			if (!sl->open && sl->pin >= 0 && !hh_pin_live(&hc->pin[sl->pin]))
				target = s;
		}
		if (target < 0)
			continue;
		struct hh_slot *sl = &hc->slot[target];
		if (sl->pin >= 0)
			hc->pin[sl->pin].slot = -1;
		sl->pin = i;
		p->slot = target;
	}
	/* 2: empty slots take any pin without one */
	for (int s = 0; s < hc->nslots; s++) {
		struct hh_slot *sl = &hc->slot[s];
		if (sl->pin >= 0)
			continue;
		for (int i = 0; i < hc->npins; i++) {
			if (hc->pin[i].slot < 0) {
				hc->pin[i].slot = s;
				sl->pin = i;
				break;
			}
		}
	}
}

/* Tell the card what changed: per slot, its pin's monitor. */
static void hh_report_slots(struct hh_codec *hc)
{
	struct snd_card *card = hc->c->ctrl ? hc->c->ctrl->card : NULL;

	for (int s = 0; s < hc->nslots; s++) {
		struct hh_slot *sl = &hc->slot[s];
		int present = sl->pin >= 0 && hh_pin_live(&hc->pin[sl->pin]);
		if (!sl->open)
			hh_update_slot_hw(hc, sl);
		if (sl->rep_pin == sl->pin && sl->rep_present == present)
			continue;
		sl->rep_pin = sl->pin;
		sl->rep_present = present;
		if (card) {
			snd_jack_report(card, sl->out_id, present);
			snd_card_event(card, AUDIO_EV_HOTPLUG, sl->out_id, (uint32_t)present);
		}
	}
}

/* Look at the pins in `mask' (all with ~0).  Returns the pins that want
 * another look later. */
static uint32_t hh_sense_mask(struct hh_codec *hc, uint32_t mask)
{
	uint32_t again = 0;

	hh_lock(hc);
	for (int i = 0; i < hc->npins; i++)
		if ((mask & (1u << i)) && hh_sense_pin(hc, i))
			again |= 1u << i;
	hh_bind_slots(hc);
	hh_report_slots(hc);
	hh_unlock(hc);
	return again;
}

/* ---- deferred work --------------------------------------------------------------------------- */

static void hh_work(void *arg);
static void hh_poll_work(void *arg);

static void hh_queue(struct hh_codec *hc, uint32_t pins, uint32_t delay_ms)
{
	__sync_fetch_and_or(&hc->pending, pins);
	if (__sync_lock_test_and_set(&hc->work_queued, 1))
		return; /* one item at a time; it picks the new bits up */
	if (snd_schedule_work(hh_work, hh_work_arg(hc), delay_ms) != 0)
		__sync_lock_release(&hc->work_queued);
}

static void hh_work(void *arg)
{
	struct hh_codec *hc = hh_get(arg);
	uint32_t pins, again;

	if (!hc)
		return;
	__sync_lock_release(&hc->work_queued);
	pins = __sync_fetch_and_and(&hc->pending, 0);
	if (pins) {
		again = hh_sense_mask(hc, pins);
		if (again && !hc->dead)
			hh_queue(hc, again, HH_REPOLL_MS);
	}
	hh_put(hc);
}

static void hh_poll_work(void *arg)
{
	struct hh_codec *hc = hh_get(arg);

	if (!hc)
		return;
	__sync_lock_release(&hc->poll_queued);
	(void)hh_sense_mask(hc, 0xffffffffu);
	if (hc->poll && !hc->dead && !__sync_lock_test_and_set(&hc->poll_queued, 1)) {
		if (snd_schedule_work(hh_poll_work, hh_work_arg(hc), HH_POLL_MS) != 0)
			__sync_lock_release(&hc->poll_queued);
	}
	hh_put(hc);
}

static void hh_start_poll(struct hh_codec *hc)
{
	if (!hc->poll || __sync_lock_test_and_set(&hc->poll_queued, 1))
		return;
	if (snd_schedule_work(hh_poll_work, hh_work_arg(hc), HH_POLL_MS) != 0)
		__sync_lock_release(&hc->poll_queued);
}

/* Work that could not be queued during the probe (the work thread
 * starts later) is queued from the next entry point that runs. */
static void hh_kick(struct hh_codec *hc)
{
	if (hc->pending && !hc->work_queued)
		hh_queue(hc, 0, 0);
	if (hc->poll && !hc->poll_queued)
		hh_start_poll(hc);
}

/* ---- channel allocation and mapping ------------------------------------------------------------ */

/* The CEA channel allocation for `nch' channels to a sink with speakers
 * `sink' (SPK_*), and in map[slot] the stream channel each HDMI slot
 * carries (HH_SLOT_UNUSED for none). */
static int hh_pick_ca(uint32_t nch, uint32_t sink, uint8_t map[8])
{
	uint32_t want = 0;
	int best = -1, best_score = -1;

	hh_ca_init();
	if (nch < 1)
		nch = 1;
	if (nch > 8)
		nch = 8;
	for (uint32_t i = 0; i < nch; i++)
		want |= hh_layout[nch][i];
	for (int s = 0; s < 8; s++)
		map[s] = HH_SLOT_UNUSED;
	if (nch <= 2) {
		map[0] = 0;
		map[1] = nch == 2 ? 1 : 0;
		return 0;
	}
	if (!sink)
		sink = ~0u;

	/* score: exactly our layout and inside the sink (best); inside the
	 * sink with the most of our speakers; exactly our layout anyway; any
	 * allocation with that many channels */
	for (int i = 0; i < HH_NUM_CA; i++) {
		const struct hh_ca *e = &hh_ca_table[i];
		int score;
		if (e->nch != nch)
			continue;
		if (e->mask == want && (e->mask & ~sink) == 0)
			score = 1000;
		else if ((e->mask & ~sink) == 0)
			score = 500 + hh_popcount(e->mask & want) * 10;
		else if (e->mask == want)
			score = 300;
		else
			score = 100 + hh_popcount(e->mask & want) * 10 -
				hh_popcount(e->mask & ~sink);
		if (score > best_score) {
			best_score = score;
			best = i;
		}
	}
	if (best < 0)
		return 0;
	{
		const struct hh_ca *e = &hh_ca_table[best];
		uint8_t used[8] = { 0 };
		uint8_t placed[8] = { 0 };
		/* channels whose speaker the allocation has */
		for (uint32_t ch = 0; ch < nch; ch++) {
			for (int s = 0; s < 8; s++) {
				if (!used[s] && e->slot[s] && e->slot[s] == hh_layout[nch][ch]) {
					map[s] = (uint8_t)ch;
					used[s] = 1;
					placed[ch] = 1;
					break;
				}
			}
		}
		/* the rest onto the remaining speakers, in slot order */
		for (uint32_t ch = 0; ch < nch; ch++) {
			if (placed[ch])
				continue;
			for (int s = 0; s < 8; s++) {
				if (!used[s] && e->slot[s]) {
					map[s] = (uint8_t)ch;
					used[s] = 1;
					placed[ch] = 1;
					break;
				}
			}
		}
		return e->ca;
	}
}

static void hh_set_channel_count(struct hh_codec *hc, struct hh_cvt *cv, uint32_t ch)
{
	uint32_t cur = hh_rd(hc, cv->nid, AC_VERB_GET_CVT_CHAN_COUNT, 0);
	if (ch < 1)
		ch = 1;
	if (cur == 0xffffffffu || (cur & 0xff) + 1 != ch)
		hh_wr(hc, cv->nid, AC_VERB_SET_CVT_CHAN_COUNT, ch - 1);
}

/* The standard HDMI channel slot verbs: payload channel << 4 | slot. */
static void hh_program_slots_std(struct hh_codec *hc, struct hh_pin *p)
{
	/* NVIDIA codecs play stereo on slots 0/1 whatever the map says */
	if (hh_is_nvidia(hc) && p->channels <= 2)
		return;
	for (int s = 0; s < 8; s++)
		hh_wr(hc, p->nid, AC_VERB_SET_HDMI_CHAN_SLOT,
		      ((uint32_t)(p->slot_ch[s] & 0xf) << 4) | (uint32_t)s);
}

static int hh_amd_swap(int slot)
{
	/* paired mode swaps FC and LFE by itself */
	return slot == 2 ? 3 : slot == 3 ? 2 : slot;
}

/* AMD: one verb per slot (full remap) or per slot pair. */
static void hh_program_slots_amd(struct hh_codec *hc, struct hh_pin *p)
{
	if (hc->amd_rev3) {
		for (int s = 0; s < 8; s++) {
			uint32_t verb = ATI_VERB_SET_MULTICHANNEL_01 + (uint32_t)(s / 2) +
					(uint32_t)(s % 2) * 0x0e;
			uint32_t ch = p->slot_ch[s];
			hh_wr(hc, p->nid, verb, ch == HH_SLOT_UNUSED ? 0 : ((ch << 4) | ATI_OUT_ENABLE));
		}
		return;
	}
	for (int pair = 0; pair < 4; pair++) {
		uint32_t a = p->slot_ch[hh_amd_swap(2 * pair)];
		uint32_t b = p->slot_ch[hh_amd_swap(2 * pair + 1)];
		uint32_t ch;
		uint32_t verb = ATI_VERB_SET_MULTICHANNEL_01 + (uint32_t)pair;
		if (a == HH_SLOT_UNUSED && b == HH_SLOT_UNUSED) {
			hh_wr(hc, p->nid, verb, 0);
			continue;
		}
		ch = a != HH_SLOT_UNUSED ? a : (b > 0 ? b - 1 : 0);
		ch &= ~1u;
		hh_wr(hc, p->nid, verb, (ch << 4) | ATI_OUT_ENABLE);
	}
}

/* ---- the audio InfoFrame -------------------------------------------------------------------- */

static void hh_dip_index(struct hh_codec *hc, struct hh_pin *p, uint32_t packet, uint32_t byte)
{
	hh_wr(hc, p->nid, AC_VERB_SET_HDMI_DIP_INDEX, ((packet & 7) << 5) | (byte & 0x1f));
}

/* Build the packet: the HDMI layout (header, checksum, PB1..PB5) or the
 * DisplayPort one (SDP header, no checksum).  Returns its length. */
static int hh_build_infoframe(struct hh_codec *hc, struct hh_pin *p, uint8_t *b)
{
	uint32_t cc = p->channels ? p->channels - 1 : 1;
	int dp = p->eld.valid ? p->eld.conn_type == 1 : (p->is_dp && !p->is_hdmi);

	mm_memset(b, 0, 9);
	if (!dp || hc->nv_dp_layout) {
		uint8_t sum = 0;
		b[0] = 0x84; /* audio InfoFrame */
		if (dp) {
			b[1] = 0x1b; /* the NVIDIA DisplayPort variant */
			b[2] = 0x11 << 2;
		} else {
			b[1] = 0x01; /* version 1 */
			b[2] = 0x0a; /* length */
		}
		b[4] = (uint8_t)(cc & 7); /* CC; CT = 0: refer to the stream */
		b[5] = 0; /* SF/SS from the stream */
		b[6] = 0;
		b[7] = (uint8_t)p->ca;
		b[8] = 0; /* LSV 0 dB, downmix allowed */
		for (int i = 0; i < 9; i++)
			sum = (uint8_t)(sum + b[i]);
		b[3] = (uint8_t)(0x100 - sum);
		return 9;
	}
	b[0] = 0x84;
	b[1] = 0x1b; /* SDP length */
	b[2] = 0x11 << 2; /* version */
	b[3] = (uint8_t)(cc & 7);
	b[4] = 0;
	b[5] = 0;
	b[6] = (uint8_t)p->ca;
	b[7] = 0;
	return 9;
}

static int hh_infoframe_current(struct hh_codec *hc, struct hh_pin *p, const uint8_t *b, int n)
{
	uint32_t x;

	hh_dip_index(hc, p, 0, 0);
	x = hh_rd(hc, p->nid, AC_VERB_GET_HDMI_DIP_XMIT, 0);
	if (x == 0xffffffffu || (x & HH_DIPXMIT_MASK) != HH_DIPXMIT_BEST)
		return 0;
	for (int i = 0; i < n; i++) {
		uint32_t v = hh_rd(hc, p->nid, AC_VERB_GET_HDMI_DIP_DATA, 0);
		if (v == 0xffffffffu || (uint8_t)v != b[i])
			return 0;
	}
	return 1;
}

static void hh_program_infoframe(struct hh_codec *hc, struct hh_pin *p)
{
	uint8_t b[9];
	int n;

	if (hh_is_amd(hc)) {
		/* the hardware builds the frame from the allocation */
		hh_wr(hc, p->nid, ATI_VERB_SET_CHANNEL_ALLOCATION, (uint32_t)p->ca);
		return;
	}
	if (hc->mcp) {
		uint32_t chan = p->channels ? p->channels - 1 : 1;
		uint32_t mask = 0;
		hh_wr(hc, hc->c->afg ? hc->c->afg : 1, NV_MCP_VERB_SET_CHANNEL_ALLOCATION, mask);
		hh_wr(hc, hc->c->afg ? hc->c->afg : 1, NV_MCP_VERB_SET_INFOFRAME_CHECKSUM,
		      (0x71 - chan - mask) & 0xff);
		return;
	}
	n = hh_build_infoframe(hc, p, b);
	if (hh_infoframe_current(hc, p, b, n))
		return;
	/* off, rewrite from byte 0, on again ("best effort": every frame) */
	hh_dip_index(hc, p, 0, 0);
	hh_wr(hc, p->nid, AC_VERB_SET_HDMI_DIP_XMIT, HH_DIPXMIT_DISABLE);
	hh_dip_index(hc, p, 0, 0);
	for (int i = 0; i < n; i++)
		hh_wr(hc, p->nid, AC_VERB_SET_HDMI_DIP_DATA, b[i]);
	hh_dip_index(hc, p, 0, 0);
	hh_wr(hc, p->nid, AC_VERB_SET_HDMI_DIP_XMIT, HH_DIPXMIT_BEST);
}

/* ---- converter selection ---------------------------------------------------------------------- */

static int hh_choose_cvt(struct hh_codec *hc, struct hh_pin *p)
{
	if (p->cvt >= 0 && p->cvt < hc->ncvts && hc->cvt[p->cvt].pin == p->index)
		return 0;
	for (int k = 0; k < p->nconn; k++) {
		int ci = hh_cvt_index(hc, p->conn[k]);
		if (ci < 0 || hc->cvt[ci].pin >= 0)
			continue;
		hc->cvt[ci].pin = p->index;
		p->cvt = ci;
		p->mux = k;
		return 0;
	}
	return -EBUSY;
}

static void hh_release_cvt(struct hh_codec *hc, struct hh_pin *p)
{
	if (p->cvt >= 0 && p->cvt < hc->ncvts && hc->cvt[p->cvt].pin == p->index)
		hc->cvt[p->cvt].pin = -1;
	p->cvt = -1;
	p->mux = -1;
}

/* Intel pins share their connection lists: a converter selected by two
 * pins plays on both monitors, and muting one mutes the other.  Every
 * other pin (and device entry) that points at our converter is moved to
 * one nobody has. */
static void hh_intel_not_share(struct hh_codec *hc, struct hh_pin *mine)
{
	uint8_t cvt_nid;

	if (mine->cvt < 0)
		return;
	cvt_nid = hc->cvt[mine->cvt].nid;
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *q = &hc->pin[i];
		int idx = -1;
		if (q == mine && q->ndev <= 1)
			continue;
		for (int k = 0; k < q->nconn; k++)
			if (q->conn[k] == cvt_nid)
				idx = k;
		if (idx < 0)
			continue;
		for (int d = 0; d < q->ndev; d++) {
			uint32_t cur;
			if (q == mine && d == mine->dev)
				continue;
			hh_dev_select(hc, q, d);
			cur = hh_rd(hc, q->nid, AC_VERB_GET_CONNECT_SEL, 0);
			if (cur == 0xffffffffu || (int)(cur & 0xff) != idx)
				continue;
			for (int k = 0; k < q->nconn; k++) {
				int ci = hh_cvt_index(hc, q->conn[k]);
				if (ci >= 0 && hc->cvt[ci].pin < 0 && hc->cvt[ci].nid != cvt_nid) {
					hh_wr(hc, q->nid, AC_VERB_SET_CONNECT_SEL, (uint32_t)k);
					break;
				}
			}
		}
		hh_dev_select(hc, q, q->dev);
	}
}

/* ---- stream setup ------------------------------------------------------------------------------- */

static void hh_keepalive(struct hh_codec *hc, struct hh_pin *p, int on);

/* Program pin `p' and its converter for the slot's stream: everything a
 * codec needs between "stream tag and format known" and the DMA start. */
static int hh_setup_pin_stream(struct hh_codec *hc, struct hh_pin *p, int tag, uint16_t fmt,
			       uint32_t channels)
{
	struct hh_cvt *cv;

	if (p->cvt < 0)
		return -ENODEV;
	cv = &hc->cvt[p->cvt];
	if (p->keepalive)
		hh_keepalive(hc, p, 0);

	hh_pick_dev(hc, p);
	if (hh_is_intel(hc)) {
		/* converters and pins may come out of the firmware in D3 */
		hh_power_d0(hc, cv->nid, 1);
		hh_power_d0(hc, p->nid, 1);
	}
	if (p->nconn > 1 && p->mux >= 0) {
		uint32_t cur = hh_rd(hc, p->nid, AC_VERB_GET_CONNECT_SEL, 0);
		if (cur == 0xffffffffu || (int)(cur & 0xff) != p->mux)
			hh_wr(hc, p->nid, AC_VERB_SET_CONNECT_SEL, (uint32_t)p->mux);
	}
	if (hh_is_intel_hsw_plus(hc) || hc->family == HH_INTEL_BYT)
		hh_intel_not_share(hc, p);
	if (hh_is_amd(hc) && hc->amd_rev3)
		hh_wr(hc, cv->nid, ATI_VERB_SET_RAMP_RATE,
		      (fmt & AC_FMT_TYPE_NON_PCM) ? 0 : ATI_RAMP_RATE_DEFAULT);
	if (hh_is_amd(hc)) {
		uint32_t hbr = hh_rd(hc, p->nid, ATI_VERB_GET_HBR_CONTROL, 0);
		if (hbr != 0xffffffffu && (hbr & ATI_HBR_CAPABLE) && (hbr & ATI_HBR_ENABLE))
			hh_wr(hc, p->nid, ATI_VERB_SET_HBR_CONTROL, hbr & ~ATI_HBR_ENABLE);
	}

	/* the digital converter off while its format changes */
	hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_1, 0);

	p->channels = channels;
	p->ca = hh_pick_ca(channels, p->eld.valid ? p->eld.spk_mask : 0, p->slot_ch);
	if (!hc->mcp) {
		hh_set_channel_count(hc, cv, channels < 2 ? 2 : channels);
		/* some converters forget their amps: unmute at every setup */
		hh_pin_amp_unmute(hc, p, hc->muted);
		if (hh_is_amd(hc))
			hh_program_slots_amd(hc, p);
		else
			hh_program_slots_std(hc, p);
	}
	hh_program_infoframe(hc, p);

	hh_wr(hc, cv->nid, AC_VERB_SET_CHANNEL_STREAMID,
	      ((uint32_t)(tag & 0xf) << AC_CONV_STREAM_SHIFT) | 0);
	hh_wr(hc, cv->nid, AC_VERB_SET_STREAM_FORMAT, fmt);
	hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_2, 0);
	hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_1, hc->muted ? 0 : AC_DIG1_ENABLE);
	if (hc->dyn_pin_out || hc->mcp)
		hh_pin_ctl(hc, p, 1);
	p->streaming = 1;
	return 0;
}

static void hh_cleanup_pin_stream(struct hh_codec *hc, struct hh_pin *p)
{
	if (p->cvt >= 0) {
		struct hh_cvt *cv = &hc->cvt[p->cvt];
		hh_wr(hc, cv->nid, AC_VERB_SET_CHANNEL_STREAMID, 0);
		hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_1, 0);
	}
	if (hc->dyn_pin_out)
		hh_pin_ctl(hc, p, 0);
	p->streaming = 0;
	p->channels = 0;
	if (hh_keepalive_enable && hc->family == HH_INTEL_ADLP && hh_pin_live(p))
		hh_keepalive(hc, p, 1);
}

/* The sink came back under a running stream: put the routing and the
 * InfoFrame back (the transcoder change may have reset them). */
static void hh_restore_stream(struct hh_codec *hc, struct hh_pin *p)
{
	struct hh_slot *sl;

	if (p->slot < 0 || p->slot >= hc->nslots)
		return;
	sl = &hc->slot[p->slot];
	if (!sl->st || !sl->fmt)
		return;
	(void)hh_setup_pin_stream(hc, p, sl->st->tag, sl->fmt, sl->channels);
	if (hc->use_display) {
		void *disp;
		const struct snd_display_audio_ops *ops = hh_display(hc, &disp);
		if (ops && ops->sync_audio_rate && p->port >= 0)
			(void)ops->sync_audio_rate(disp, p->port, (int)sl->rate);
	}
}

/* Keep-alive: a converter programmed with a silent 48 kHz stereo format
 * and stream 0xf (no DMA) with the KAE bit set keeps the display's audio
 * link up between sounds. */
static void hh_keepalive(struct hh_codec *hc, struct hh_pin *p, int on)
{
	struct hh_cvt *cv;
	uint32_t d1, d3;

	if (!on) {
		if (!p->keepalive)
			return;
		if (p->cvt >= 0) {
			cv = &hc->cvt[p->cvt];
			d1 = hh_rd(hc, cv->nid, AC_VERB_GET_DIGI_CONVERT_1, 0);
			if (d1 != 0xffffffffu) {
				d3 = (d1 >> 16) & 0xff;
				hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_3, d3 & ~HH_DIG3_KAE);
			}
			snd_udelay(150);
		}
		p->keepalive = 0;
		return;
	}
	if (p->keepalive || p->streaming)
		return;
	if (hh_choose_cvt(hc, p) != 0)
		return;
	cv = &hc->cvt[p->cvt];
	{
		uint16_t fmt = hda_calc_format(48000, 2, 16);
		hh_power_d0(hc, cv->nid, 1);
		p->channels = 2;
		p->ca = 0;
		for (int s = 0; s < 8; s++)
			p->slot_ch[s] = s < 2 ? (uint8_t)s : HH_SLOT_UNUSED;
		hh_set_channel_count(hc, cv, 2);
		hh_program_slots_std(hc, p);
		hh_program_infoframe(hc, p);
		hh_wr(hc, cv->nid, AC_VERB_SET_CHANNEL_STREAMID, (0xfu << AC_CONV_STREAM_SHIFT) | 0xf);
		hh_wr(hc, cv->nid, AC_VERB_SET_STREAM_FORMAT, fmt);
		snd_udelay(150);
		hh_wr(hc, cv->nid, AC_VERB_SET_CHANNEL_STREAMID, (0xfu << AC_CONV_STREAM_SHIFT) | 0);
		hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_1, AC_DIG1_ENABLE);
		d1 = hh_rd(hc, cv->nid, AC_VERB_GET_DIGI_CONVERT_1, 0);
		d3 = d1 == 0xffffffffu ? 0 : (d1 >> 16) & 0xff;
		hh_wr(hc, cv->nid, AC_VERB_SET_DIGI_CONVERT_3, d3 | HH_DIG3_KAE);
	}
	p->keepalive = 1;
}

/* ---- PCM capabilities ------------------------------------------------------------------------ */

#define HH_HDMI_RATES                                                                         \
	(AUDIO_RATE_32000 | AUDIO_RATE_44100 | AUDIO_RATE_48000 | AUDIO_RATE_88200 |         \
	 AUDIO_RATE_96000 | AUDIO_RATE_176400 | AUDIO_RATE_192000)

/* The union of what the converters a pin can reach support. */
static void hh_pin_cvt_caps(struct hh_codec *hc, struct hh_pin *p, uint32_t *pcm,
			    uint32_t *maxch)
{
	*pcm = 0;
	*maxch = 2;
	for (int k = 0; k < p->nconn; k++) {
		int ci = hh_cvt_index(hc, p->conn[k]);
		if (ci < 0)
			continue;
		*pcm |= hc->cvt[ci].pcm;
		if (hc->cvt[ci].max_ch > *maxch)
			*maxch = hc->cvt[ci].max_ch;
	}
	if (!*pcm)
		*pcm = (1u << 6) | AC_SUPPCM_BITS_16;
}

static void hh_fill_hw(struct hh_codec *hc, struct hh_pin *p, struct snd_pcm_hw *hw)
{
	uint32_t pcm = (1u << 6) | AC_SUPPCM_BITS_16, maxch = 2;

	mm_memset(hw, 0, sizeof(*hw));
	if (p)
		hh_pin_cvt_caps(hc, p, &pcm, &maxch);
	hw->formats = AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
	if (pcm & (AC_SUPPCM_BITS_20 | AC_SUPPCM_BITS_24 | AC_SUPPCM_BITS_32))
		hw->formats |= AUDIO_FMT_BIT(AUDIO_FMT_S32_LE);
	hw->rates = (pcm & AC_SUPPCM_RATES) & HH_HDMI_RATES;
	if (!hw->rates)
		hw->rates = AUDIO_RATE_48000;
	hw->channels_min = 2;
	hw->channels_max = maxch;
	if (p && p->eld.valid) {
		uint32_t r = hw->rates & p->eld.lpcm_rates;
		if (r)
			hw->rates = r;
		if (p->eld.lpcm_max_ch >= 2 && p->eld.lpcm_max_ch < hw->channels_max)
			hw->channels_max = p->eld.lpcm_max_ch;
		/* a sink that takes only 16-bit samples gets only those */
		if (p->eld.lpcm_bits && !(p->eld.lpcm_bits & 6))
			hw->formats = AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
	}
	if (hc->amd_rev3 == 0 && hh_is_amd(hc) && (hw->channels_max & 1))
		hw->channels_max &= ~1u; /* paired mapping: even counts */
	if (hw->channels_max > 8)
		hw->channels_max = 8;
	if (hw->channels_max < 2)
		hw->channels_max = 2;
	hw->period_bytes_min = 256;
	hw->period_bytes_max = 16 * 1024;
	hw->periods_min = 2;
	hw->periods_max = 32;
	hw->buffer_bytes_max = 512 * 1024;
	hw->buffer_align = 128;
	hw->period_align = 128;
	hw->dma32 = hc->c->ctrl ? !hc->c->ctrl->addr64 : 1;
	hw->dma_coherent = 0;
}

/* A slot not in use follows its pin's sink (rates, channels). */
static void hh_update_slot_hw(struct hh_codec *hc, struct hh_slot *s)
{
	struct snd_pcm_hw hw;

	if (!s->pcm || s->open)
		return;
	hh_fill_hw(hc, s->pin >= 0 ? &hc->pin[s->pin] : NULL, &hw);
	s->pcm->hw[AUDIO_DIR_PLAYBACK] = hw;
}

/* ---- PCM operations ------------------------------------------------------------------------- */

static struct hh_slot *hh_slot_of(struct snd_pcm_substream *ss)
{
	if (!ss || !ss->pcm || !ss->pcm->private_data)
		return NULL;
	return (struct hh_slot *)ss->pcm->private_data;
}

static int hh_pcm_open(struct snd_pcm_substream *ss)
{
	struct hh_slot *sl = hh_slot_of(ss);
	struct hh_codec *hc;
	struct hh_pin *p;
	struct hda_stream *st;
	int rc;

	if (!sl || ss->direction != AUDIO_DIR_PLAYBACK)
		return -ENODEV;
	hc = sl->hc;
	if (hc->dead)
		return -ENODEV;
	hh_kick(hc);
	hh_lock(hc);
	if (sl->open) {
		hh_unlock(hc);
		return -EBUSY;
	}
	/* bind (again) now: a monitor may have come since */
	hh_bind_slots(hc);
	if (sl->pin < 0) {
		hh_unlock(hc);
		return -ENODEV;
	}
	p = &hc->pin[sl->pin];
	if (p->keepalive)
		hh_keepalive(hc, p, 0);
	rc = hh_choose_cvt(hc, p);
	if (rc) {
		hh_unlock(hc);
		return rc;
	}
	st = hda_stream_assign(hc->c->ctrl, AUDIO_DIR_PLAYBACK);
	if (!st) {
		hh_release_cvt(hc, p);
		hh_unlock(hc);
		return -EBUSY;
	}
	st->substream = ss;
	st->codec = hc->c;
	st->hdmi = 1;
	ss->private_data = st;
	sl->st = st;
	sl->open = 1;
	sl->prepared = 0;
	sl->fmt = 0;
	if (!sl->powered) {
		hh_display_power(hc, 1);
		sl->powered = 1;
	}
	hh_unlock(hc);
	return 0;
}

static void hh_pcm_close(struct snd_pcm_substream *ss)
{
	struct hh_slot *sl = hh_slot_of(ss);
	struct hh_codec *hc;

	if (!sl)
		return;
	hc = sl->hc;
	hh_lock(hc);
	if (sl->pin >= 0) {
		struct hh_pin *p = &hc->pin[sl->pin];
		if (p->streaming)
			hh_cleanup_pin_stream(hc, p);
		if (!p->keepalive)
			hh_release_cvt(hc, p);
	}
	if (sl->st) {
		hda_stream_stop(sl->st);
		hda_stream_reset(sl->st);
		sl->st->substream = NULL;
		sl->st->codec = NULL;
		sl->st->hdmi = 0;
		hda_stream_release(sl->st);
		sl->st = NULL;
	}
	ss->private_data = NULL;
	sl->open = 0;
	sl->prepared = 0;
	sl->fmt = 0;
	if (sl->powered) {
		hh_display_power(hc, 0);
		sl->powered = 0;
	}
	/* the monitor may have changed while the slot was held */
	hh_bind_slots(hc);
	hh_report_slots(hc);
	hh_unlock(hc);
}

static uint32_t hh_bits_for(struct hh_codec *hc, struct hh_pin *p, uint32_t format)
{
	uint32_t pcm = 0;

	if (format == AUDIO_FMT_S16_LE)
		return 16;
	if (format != AUDIO_FMT_S32_LE)
		return 0;
	if (p->cvt >= 0)
		pcm = hc->cvt[p->cvt].pcm;
	/* HDMI carries at most 24 bits; the 32-bit container is filled from
	 * the top, so the low bits are simply dropped */
	if (pcm & AC_SUPPCM_BITS_24)
		return 24;
	if (pcm & AC_SUPPCM_BITS_32)
		return 32;
	if (pcm & AC_SUPPCM_BITS_20)
		return 20;
	return 0;
}

static int hh_program(struct hh_slot *sl, struct snd_pcm_substream *ss)
{
	struct hh_codec *hc = sl->hc;
	struct hh_pin *p;
	uint32_t bits, rate = ss->params.rate, ch = ss->params.channels;
	uint16_t fmt;
	int rc;

	if (sl->pin < 0 || !sl->st)
		return -ENODEV;
	p = &hc->pin[sl->pin];
	if (ch < 2 || ch > 8 || !audio_rate_bit(rate) || !(audio_rate_bit(rate) & HH_HDMI_RATES))
		return -EINVAL;
	if (p->cvt < 0 || ch > hc->cvt[p->cvt].max_ch)
		return -EINVAL;
	if (hh_is_amd(hc) && !hc->amd_rev3 && (ch & 1))
		return -EINVAL;
	bits = hh_bits_for(hc, p, ss->params.format);
	if (!bits)
		return -EINVAL;
	fmt = hda_calc_format(rate, ch, bits);
	if (!fmt)
		return -EINVAL;
	if (!ss->dma_phys || !ss->buffer_bytes || !ss->period_bytes)
		return -EINVAL;

	hda_stream_reset(sl->st);
	rc = hda_stream_setup(sl->st, ss->dma_phys, ss->buffer_bytes, ss->period_bytes, fmt);
	if (rc)
		return rc;
	sl->rate = rate;
	sl->channels = ch;
	sl->bits = bits;
	sl->fmt = fmt;

	/* the display's audio clock (N/CTS, Maud/Naud) for the rate */
	{
		void *disp;
		const struct snd_display_audio_ops *ops = hh_display(hc, &disp);
		if (ops && ops->sync_audio_rate && p->port >= 0)
			(void)ops->sync_audio_rate(disp, p->port, (int)rate);
	}
	return hh_setup_pin_stream(hc, p, sl->st->tag, fmt, ch);
}

static int hh_pcm_hw_params(struct snd_pcm_substream *ss)
{
	struct hh_slot *sl = hh_slot_of(ss);
	int rc;

	if (!sl)
		return -ENODEV;
	hh_lock(sl->hc);
	rc = hh_program(sl, ss);
	if (rc == 0)
		sl->prepared = 0;
	hh_unlock(sl->hc);
	return rc;
}

static int hh_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct hh_slot *sl = hh_slot_of(ss);

	if (!sl)
		return 0;
	hh_lock(sl->hc);
	if (sl->st) {
		hda_stream_stop(sl->st);
		hda_stream_reset(sl->st);
	}
	if (sl->pin >= 0 && sl->hc->pin[sl->pin].streaming)
		hh_cleanup_pin_stream(sl->hc, &sl->hc->pin[sl->pin]);
	sl->fmt = 0;
	sl->prepared = 0;
	hh_unlock(sl->hc);
	return 0;
}

static int hh_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct hh_slot *sl = hh_slot_of(ss);
	int rc;

	if (!sl)
		return -ENODEV;
	hh_lock(sl->hc);
	/* back to the start of the ring, and the codec reprogrammed: the
	 * display may have changed the transcoder under us */
	rc = hh_program(sl, ss);
	if (rc == 0)
		sl->prepared = 1;
	hh_unlock(sl->hc);
	return rc;
}

static int hh_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct hda_stream *st = (struct hda_stream *)ss->private_data;

	if (!st)
		return -ENODEV;
	switch (cmd) {
	case SND_TRIGGER_START:
	case SND_TRIGGER_RESUME:
		hda_stream_start(st);
		return 0;
	case SND_TRIGGER_STOP:
	case SND_TRIGGER_PAUSE:
		hda_stream_stop(st);
		return 0;
	default:
		return -EINVAL;
	}
}

static uint32_t hh_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct hda_stream *st = (struct hda_stream *)ss->private_data;
	uint32_t pos;

	if (!st || !ss->buffer_bytes)
		return 0;
	pos = hda_stream_position(st);
	return pos < ss->buffer_bytes ? pos : 0;
}

static const struct snd_pcm_ops hh_pcm_ops = {
	.open = hh_pcm_open,
	.close = hh_pcm_close,
	.hw_params = hh_pcm_hw_params,
	.hw_free = hh_pcm_hw_free,
	.prepare = hh_pcm_prepare,
	.trigger = hh_pcm_trigger,
	.pointer = hh_pcm_pointer,
	.delay = NULL,
};

/* ---- matching and probing ------------------------------------------------------------------- */

static const struct hh_model *hh_lookup(uint32_t id)
{
	for (const struct hh_model *m = hh_models; m->id; m++)
		if (m->id == id)
			return m;
	return NULL;
}

/* A family for an id the table does not list: newer Intel display codecs
 * keep the latest layout, NVIDIA ids say their generation by range. */
static uint8_t hh_family_guess(uint32_t id)
{
	uint16_t ven = (uint16_t)(id >> 16), dev = (uint16_t)id;

	if (ven == 0x8086 && (dev & 0xff00) == 0x2800) {
		if (dev > 0x2824 && dev < 0x2880)
			return HH_INTEL_ADLP;
		return HH_INTEL_TGL;
	}
	if (ven == 0x10de) {
		if (dev <= 0x0007 || dev == 0x0067 || dev == 0x8001 || dev == 0x8067)
			return HH_NV_MCP;
		if (dev < 0x0020)
			return HH_NV_LEGACY;
		if (dev < 0x0040)
			return HH_NV_TEGRA;
		return HH_NV;
	}
	if (ven == 0x1002)
		return HH_AMD;
	return HH_GENERIC;
}

/* Does the function group have only digital (HDMI/DP) pins?  Then it is a
 * display codec whatever its vendor. */
static int hh_afg_digital_only(struct hda_codec *c)
{
	uint32_t nc, start, n;
	int digital = 0, analog = 0;

	if (!c->afg)
		return 0;
	nc = hda_param(c, c->afg, AC_PAR_NODE_COUNT);
	if (nc == 0xffffffffu)
		return 0;
	start = (nc >> 16) & 0xff;
	n = nc & 0xff;
	if (n > HH_MAX_NODES)
		n = HH_MAX_NODES;
	for (uint32_t i = 0; i < n && start + i < 0x100; i++) {
		uint8_t nid = (uint8_t)(start + i);
		uint32_t wcaps = hda_param(c, nid, AC_PAR_AUDIO_WIDGET_CAP);
		uint32_t pincap, cfg;
		if (wcaps == 0xffffffffu || hh_wid_type(wcaps) != AC_WID_PIN)
			continue;
		pincap = hda_param(c, nid, AC_PAR_PIN_CAP);
		if (pincap == 0xffffffffu)
			continue;
		if (hh_pin_wanted(wcaps, pincap)) {
			digital++;
			continue;
		}
		cfg = hda_codec_read(c, nid, AC_VERB_GET_CONFIG_DEFAULT, 0);
		if (cfg != 0xffffffffu &&
		    ((cfg & AC_DEFCFG_PORT_CONN) >> AC_DEFCFG_PORT_CONN_SHIFT) == AC_JACK_PORT_NONE)
			continue; /* an unconnected analog pin does not count */
		analog++;
	}
	return digital > 0 && analog == 0;
}

static int hh_match(struct hda_codec *c)
{
	uint16_t ven = (uint16_t)(c->vendor_id >> 16), dev = (uint16_t)c->vendor_id;

	if (hh_lookup(c->vendor_id))
		return 1;
	/* every Intel 0x28xx is a display codec */
	if (ven == 0x8086 && (dev & 0xff00) == 0x2800)
		return 1;
	/* NVIDIA and AMD graphics functions carry nothing else */
	if ((ven == 0x10de || ven == 0x1002) && hh_afg_digital_only(c))
		return 1;
	return hh_afg_digital_only(c);
}

/* Before the generic code looks: Intel's hidden pins out, so whoever
 * enumerates sees them; the codec is ours. */
static int hh_pre_parse(struct hda_codec *c)
{
	const struct hh_model *m = hh_lookup(c->vendor_id);
	uint8_t fam = m ? m->family : hh_family_guess(c->vendor_id);
	uint8_t vnid = 0;

	c->is_hdmi = 1;
	switch (fam) {
	case HH_INTEL_HSW:
		vnid = 0x08;
		break;
	case HH_INTEL_GLK:
		vnid = 0x0b;
		break;
	case HH_INTEL_ICL:
	case HH_INTEL_TGL:
	case HH_INTEL_ADLP:
		vnid = 0x02;
		break;
	default:
		break;
	}
	if (vnid) {
		uint32_t v = hda_codec_read(c, vnid, INTEL_VERB_GET_VENDOR, 0);
		if (v != 0xffffffffu && !(v & INTEL_VENDOR_ALL_PINS_CVTS))
			(void)hda_codec_read(c, vnid, INTEL_VERB_SET_VENDOR,
					     (v | INTEL_VENDOR_ALL_PINS_CVTS) & 0xff);
	}
	return 0;
}

static void hh_setup_family(struct hh_codec *hc)
{
	uint32_t id = hc->c->vendor_id;

	hc->max_dev = 1;
	hc->mst = 0;
	switch (hc->family) {
	case HH_INTEL_HSW:
		hc->vendor_nid = 0x08;
		hc->base_nid = 5;
		hc->mst = 1;
		hc->max_dev = 3;
		break;
	case HH_INTEL_GLK:
		hc->vendor_nid = 0x0b;
		hc->base_nid = 5;
		hc->mst = 1;
		hc->max_dev = 3;
		break;
	case HH_INTEL_ICL:
		hc->vendor_nid = 0x02;
		hc->port_map = hh_icl_port_map;
		hc->nport_map = (int)sizeof(hh_icl_port_map);
		hc->mst = 1;
		hc->max_dev = 3;
		break;
	case HH_INTEL_TGL:
	case HH_INTEL_ADLP:
		hc->vendor_nid = 0x02;
		hc->port_map = hh_tgl_port_map;
		hc->nport_map = (int)sizeof(hh_tgl_port_map);
		hc->mst = 1;
		hc->max_dev = 4;
		break;
	case HH_INTEL_CPT:
	case HH_INTEL_BYT:
		/* Ibex Peak and Valleyview number their pins from 4 */
		hc->base_nid = (id == 0x80860054 || id == 0x80862804 || id == 0x80862882) ? 4 : 5;
		break;
	case HH_NV:
		hc->mst = 1;
		hc->max_dev = HH_MAX_DEVS;
		hc->dyn_pin_out = 1;
		hc->nv_dp_layout = 1;
		break;
	case HH_NV_LEGACY:
	case HH_NV_TEGRA:
		hc->dyn_pin_out = 1;
		hc->nv_dp_layout = 1;
		break;
	case HH_NV_MCP:
		hc->mcp = 1;
		break;
	case HH_AMD:
		hc->amd_rev3 = id == 0x1002aa01 && (hc->c->revision_id & 0xff00) >= 0x0300;
		break;
	default:
		break;
	}
	/* the display driver speaks for the Intel codecs it drives (Haswell
	 * and later: the ports have a known pin) */
	hc->use_display = hh_is_intel_hsw_plus(hc);
}

static int hh_probe(struct hda_codec *c)
{
	struct hh_codec *hc;
	const struct hh_model *m;
	int rc;

	if (!c || !c->ctrl)
		return -ENODEV;
	hc = kalloc(sizeof(*hc));
	if (!hc)
		return -ENOMEM;
	mm_memset(hc, 0, sizeof(*hc));
	hc->c = c;
	hc->reg = -1;
	m = hh_lookup(c->vendor_id);
	hc->model = m;
	hc->family = m ? m->family : hh_family_guess(c->vendor_id);
	hh_setup_family(hc);
	/* the display driver's notifications stand in for unsolicited
	 * responses on the codecs it speaks for */
	hc->poll = !hc->use_display &&
		   (!c->unsol_capable || (c->ctrl->flags & HDA_F_NO_UNSOL) != 0);
	for (int s = 0; s < SND_MAX_PCMS; s++) {
		hc->slot[s].pin = -1;
		hc->slot[s].rep_pin = -2;
		hc->slot[s].rep_present = -1;
	}
	ksnprintf(hc->name, sizeof(hc->name), "codec %d %s", c->addr,
		  m ? m->name : "HDMI/DP");

	c->is_hdmi = 1;
	if (m) {
		kstrncpy(c->vendor_name, m->vendor, sizeof(c->vendor_name) - 1);
		c->vendor_name[sizeof(c->vendor_name) - 1] = 0;
		kstrncpy(c->chip_name, m->name, sizeof(c->chip_name) - 1);
		c->chip_name[sizeof(c->chip_name) - 1] = 0;
	} else if (!c->chip_name[0]) {
		ksnprintf(c->chip_name, sizeof(c->chip_name), "HDMI %04x", c->vendor_id & 0xffff);
	}

	hh_intel_wake(hc);
	if (c->afg)
		hh_wr(hc, c->afg, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	/* the pin/converter tree changes once the vendor bit is in; give
	 * the codec a moment, and try again if it answered half */
	rc = -ENODEV;
	for (int tries = 0; tries < 3 && rc; tries++) {
		rc = hh_enumerate(hc);
		if (rc)
			snd_mdelay(10);
	}
	if (rc) {
		snd_info("hda", "codec %d (%08x): no HDMI/DP pins or converters\n", c->addr,
			 c->vendor_id);
		kfree(hc);
		return rc;
	}
	c->vendor_data = hc;
	if (hh_register(hc) < 0) {
		snd_info("hda", "%s: too many display codecs\n", hc->name);
		c->vendor_data = NULL;
		kfree(hc);
		return -ENOSPC;
	}

	hh_lock(hc);
	hh_init_hw(hc);
	for (int i = 0; i < hc->npins; i++)
		(void)hh_sense_pin(hc, i);
	hh_unlock(hc);

	snd_info("hda", "%s: %d pin(s), %d converter(s)%s%s%s\n", hc->name, hc->npins, hc->ncvts,
		 hc->mst ? ", DP MST entries" : "", hc->use_display ? ", display-driven ELD" : "",
		 hc->poll ? ", polled" : "");
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		snd_info("hda", "%s:   pin %02x%s%s%s port %d, %d conn, %d dev%s\n", hc->name, p->nid,
			 p->is_hdmi ? " HDMI" : "", p->is_dp ? " DP" : "", p->hbr ? " HBR" : "",
			 p->port, p->nconn, p->ndev, p->forced ? " (forced)" : "");
	}
	/* pins whose ELD was not readable yet get another look */
	hh_queue(hc, (1u << hc->npins) - 1, HH_REPOLL_MS);
	hh_start_poll(hc);
	return 0;
}

static void hh_remove(struct hda_codec *c)
{
	struct hh_codec *hc = hh_of(c);

	if (!hc)
		return;
	hh_unregister(hc);
	/* work that already looked the codec up finishes first */
	for (int spin = 0; hc->busy && spin < 100000; spin++)
		sched_yield_in_kernel();
	hh_lock(hc);
	for (int s = 0; s < hc->nslots; s++) {
		struct hh_slot *sl = &hc->slot[s];
		if (sl->st) {
			hda_stream_stop(sl->st);
			sl->st->substream = NULL;
			sl->st->codec = NULL;
			sl->st->hdmi = 0;
			hda_stream_release(sl->st);
			sl->st = NULL;
		}
		if (sl->powered) {
			hh_display_power(hc, 0);
			sl->powered = 0;
		}
		if (sl->pcm)
			sl->pcm->private_data = NULL;
	}
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		if (p->keepalive)
			hh_keepalive(hc, p, 0);
		if ((p->wcaps & AC_WCAP_UNSOL_CAP))
			hh_wr(hc, p->nid, AC_VERB_SET_UNSOLICITED_ENABLE, 0);
	}
	hh_unlock(hc);
	c->vendor_data = NULL;
	kfree(hc);
}

/* After a controller reset or resume: the vendor bits, the widgets and
 * the routing are gone; streams that were running are set up again by
 * the PCM core's prepare. */
static int hh_init(struct hda_codec *c)
{
	struct hh_codec *hc = hh_of(c);

	if (!hc)
		return 0;
	hh_lock(hc);
	hh_init_hw(hc);
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		p->keepalive = 0;
		if (p->streaming)
			hh_restore_stream(hc, p);
	}
	hh_unlock(hc);
	hh_queue(hc, 0xffffffffu, 0);
	return 0;
}

/* An unsolicited response: the tag names the pin; the presence and ELD
 * bits in it are not trusted (some codecs report a change, not the new
 * state), the pin is simply looked at again. */
static void hh_unsol(struct hda_codec *c, uint32_t res)
{
	struct hh_codec *hc = hh_of(c);
	uint32_t tag, again;

	if (!hc)
		return;
	tag = (res & AC_UNSOL_RES_TAG) >> AC_UNSOL_RES_TAG_SHIFT;
	if (tag >= 1 && (int)tag <= hc->npins) {
		hc->pin[tag - 1].unsol_count++;
		again = hh_sense_mask(hc, 1u << (tag - 1));
	} else {
		again = hh_sense_mask(hc, 0xffffffffu);
	}
	if (again)
		hh_queue(hc, again, HH_REPOLL_MS);
}

const struct hda_vendor_driver hda_hdmi_driver = {
	.name = "hdmi",
	.match = hh_match,
	.pre_parse = hh_pre_parse,
	.post_parse = NULL,
	.init = hh_init,
	.automute = NULL,
	.unsol = hh_unsol,
	.playback_hook = NULL,
	.probe = hh_probe,
	.remove = hh_remove,
};

/* ---- card glue --------------------------------------------------------------------------------- */

int hda_hdmi_is_display_codec(struct hda_codec *c)
{
	return hh_of(c) != NULL;
}

int hda_hdmi_add_pcms(struct hda_codec *c)
{
	struct hh_codec *hc = hh_of(c);
	struct hda_controller *ctrl;
	struct snd_card *card;
	int want, avail, made = 0;

	if (!hc)
		return -ENODEV;
	ctrl = c->ctrl;
	card = ctrl ? ctrl->card : NULL;
	if (!card)
		return -ENODEV;
	if (hc->nslots)
		return 0;

	avail = SND_MAX_PCMS - card->npcm;
	if (HDA_MAX_HDMI_PINS - ctrl->npcm_hdmi < avail)
		avail = HDA_MAX_HDMI_PINS - ctrl->npcm_hdmi;
	want = hc->npins;
	if (want > avail)
		want = avail;
	if (want <= 0) {
		snd_info("hda", "%s: no PCM device left on the card for HDMI/DP\n", hc->name);
		return -ENOSPC;
	}
	if (want < hc->npins)
		snd_info("hda", "%s: %d pins share %d PCM device(s) (bound to monitors as they come)\n",
			 hc->name, hc->npins, want);

	hh_lock(hc);
	hc->nslots = want;
	/* slots go to the pins with a monitor first */
	hh_bind_slots(hc);
	for (int s = 0; s < want; s++) {
		struct hh_slot *sl = &hc->slot[s];
		struct snd_pcm_hw hw;
		char name[32];
		int rc;

		sl->hc = hc;
		sl->index = s;
		sl->global_index = ctrl->npcm_hdmi;
		sl->out_id = HDA_HDMI_OUT_ID(c->addr, s);
		hh_fill_hw(hc, sl->pin >= 0 ? &hc->pin[sl->pin] : NULL, &hw);
		ksnprintf(name, sizeof(name), "HDMI %d", sl->global_index);
		rc = snd_pcm_new(card, name, &hh_pcm_ops, &hw, NULL, sl, &sl->pcm);
		if (rc || !sl->pcm) {
			snd_info("hda", "%s: PCM device %s not created (%d)\n", hc->name, name, rc);
			sl->pcm = NULL;
			break;
		}
		/* AUDIO_GETPCMINFO: the output this device feeds */
		sl->pcm->output_id = sl->out_id;
		sl->pcm->output_type = AUDIO_OUT_HDMI;
		ctrl->pcm_hdmi[ctrl->npcm_hdmi++] = sl->pcm;
		made++;
	}
	/* slots whose PCM could not be made release their pins */
	for (int s = made; s < want; s++) {
		struct hh_slot *sl = &hc->slot[s];
		if (sl->pin >= 0)
			hc->pin[sl->pin].slot = -1;
		sl->pin = -1;
	}
	hc->nslots = made;
	if (made) {
		/* (the output count is the controller's, from the merged
		 * output lists) */
		card->info_flags |= AUDIO_INFO_HDMI | AUDIO_INFO_PLAYBACK | AUDIO_INFO_JACK_DETECT;
		if (!card->codec[0])
			ksnprintf(card->codec, sizeof(card->codec), "%s %s", c->vendor_name,
				  c->chip_name);
	}
	hh_report_slots(hc);
	hh_unlock(hc);
	return made ? 0 : -ENODEV;
}

int hda_hdmi_get_outputs(struct hda_codec *c, struct audio_output_desc *out, int max)
{
	struct hh_codec *hc = hh_of(c);
	int n = 0;

	if (!hc || !out || max <= 0)
		return 0;
	hh_kick(hc);
	hh_lock(hc);
	for (int s = 0; s < hc->nslots && n < max; s++) {
		struct hh_slot *sl = &hc->slot[s];
		struct audio_output_desc *d = &out[n++];
		struct hh_pin *p = sl->pin >= 0 ? &hc->pin[sl->pin] : NULL;
		int dp;

		mm_memset(d, 0, sizeof(*d));
		d->id = sl->out_id;
		dp = p ? (p->eld.valid ? p->eld.conn_type == 1 : (p->is_dp && !p->is_hdmi)) : 0;
		d->type = dp ? AUDIO_OUT_DP : AUDIO_OUT_HDMI;
		d->flags = AUDIO_OUTF_JACK_DETECT;
		d->present = p ? (uint32_t)hh_pin_live(p) : 0;
		d->active = sl->open && p && p->streaming;
		ksnprintf(d->name, sizeof(d->name), "%s %d", dp ? "DisplayPort" : "HDMI",
			  sl->global_index + 1);
		if (p && p->eld.valid && p->eld.monitor[0]) {
			kstrncpy(d->monitor, p->eld.monitor, sizeof(d->monitor) - 1);
			d->monitor[sizeof(d->monitor) - 1] = 0;
		}
	}
	hh_unlock(hc);
	return n;
}

int hda_hdmi_set_mute(struct hda_codec *c, uint32_t mute)
{
	struct hh_codec *hc = hh_of(c);

	if (!hc)
		return -ENOENT;
	hh_lock(hc);
	hc->muted = mute ? 1 : 0;
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		for (int d = 0; d < p->ndev; d++) {
			hh_dev_select(hc, p, d);
			hh_pin_amp_unmute(hc, p, hc->muted);
		}
		hh_dev_select(hc, p, p->dev);
		/* no amp: the converter's digital enable is the mute */
		if (!(p->wcaps & AC_WCAP_OUT_AMP) && p->cvt >= 0 && p->streaming)
			hh_wr(hc, hc->cvt[p->cvt].nid, AC_VERB_SET_DIGI_CONVERT_1,
			      hc->muted ? 0 : AC_DIG1_ENABLE);
	}
	hh_unlock(hc);
	return 0;
}

int hda_hdmi_get_mute(struct hda_codec *c, uint32_t *mute)
{
	struct hh_codec *hc = hh_of(c);

	if (!hc || !mute)
		return -ENOENT;
	*mute = (uint32_t)hc->muted;
	return 0;
}

/* From the display driver (through the sound core): the audio sink on a
 * display port came or went.  Called with the display's locks held, so
 * nothing is done here but queueing the look. */
void hda_hdmi_notify(int port, int present)
{
	struct hh_codec *list[HH_MAX_REG];
	int n = 0;
	uint64_t fl;

	(void)present;
	if (port < 0 || port > 31)
		return;
	spin_lock_irqsave(&g_hh_lock, &fl);
	for (int i = 0; i < HH_MAX_REG; i++)
		if (g_hh[i] && !g_hh[i]->dead && g_hh[i]->use_display) {
			__sync_fetch_and_add(&g_hh[i]->busy, 1);
			list[n++] = g_hh[i];
		}
	spin_unlock_irqrestore(&g_hh_lock, fl);

	for (int i = 0; i < n; i++) {
		struct hh_codec *hc = list[i];
		/* the pin table is fixed after probe: no lock needed to read it */
		int pi = hh_port_to_pin_index(hc, port);
		if (pi >= 0) {
			hc->pin[pi].repolls = 0;
			hh_queue(hc, 1u << pi, 0);
		}
		hh_put(hc);
	}
}

void hda_hdmi_rescan_all(void)
{
	struct hh_codec *list[HH_MAX_REG];
	int n = 0;
	uint64_t fl;

	spin_lock_irqsave(&g_hh_lock, &fl);
	for (int i = 0; i < HH_MAX_REG; i++)
		if (g_hh[i] && !g_hh[i]->dead) {
			__sync_fetch_and_add(&g_hh[i]->busy, 1);
			list[n++] = g_hh[i];
		}
	spin_unlock_irqrestore(&g_hh_lock, fl);
	for (int i = 0; i < n; i++) {
		hh_queue(list[i], 0xffffffffu, 0);
		hh_start_poll(list[i]);
		hh_put(list[i]);
	}
}

/* ---- the diagnostic dump --------------------------------------------------------------------- */

struct hh_out {
	char *p;
	size_t len, pos;
};

static void hh_printf(struct hh_out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!o->len || o->pos + 1 >= o->len)
		return;
	__builtin_va_start(ap, fmt);
	n = kvsnprintf(o->p + o->pos, o->len - o->pos, fmt, ap);
	__builtin_va_end(ap);
	if (n <= 0)
		return;
	o->pos += (size_t)n;
	if (o->pos >= o->len)
		o->pos = o->len - 1;
	o->p[o->pos] = 0;
}

long hda_hdmi_dump(struct hda_codec *c, char *buf, size_t len)
{
	struct hh_codec *hc = hh_of(c);
	struct hh_out o = { buf, len, 0 };

	if (!buf || !len)
		return 0;
	buf[0] = 0;
	if (!hc)
		return 0;
	hh_lock(hc);
	hh_printf(&o, "HDMI/DP codec %d: %08x rev %08x %s %s\n", c->addr, c->vendor_id,
		  c->revision_id, c->vendor_name, c->chip_name);
	hh_printf(&o, "  family %u%s%s%s%s%s\n", hc->family, hc->mst ? " mst" : "",
		  hc->use_display ? " display-eld" : "", hc->poll ? " polled" : "",
		  hc->amd_rev3 ? " amd-remap" : "", hc->muted ? " muted" : "");
	for (int i = 0; i < hc->ncvts; i++) {
		struct hh_cvt *cv = &hc->cvt[i];
		uint32_t conv = hh_rd(hc, cv->nid, AC_VERB_GET_CONV, 0);
		uint32_t fmt = hh_rd(hc, cv->nid, AC_VERB_GET_STREAM_FORMAT, 0);
		hh_printf(&o, "  cvt %02x: pcm %08x max %uch, stream %x fmt %04x, pin %d\n", cv->nid,
			  cv->pcm, cv->max_ch, conv == 0xffffffffu ? 0 : (conv >> 4) & 0xf,
			  fmt == 0xffffffffu ? 0 : fmt & 0xffff, cv->pin);
	}
	for (int i = 0; i < hc->npins; i++) {
		struct hh_pin *p = &hc->pin[i];
		hh_printf(&o, "  pin %02x: cfg %08x cap %08x port %d dev %d/%d sense %08x%s%s\n", p->nid,
			  p->cfg, p->pincap, p->port, p->dev, p->ndev, p->sense_raw,
			  p->present ? " present" : "", p->streaming ? " streaming" : "");
		hh_printf(&o, "    conn:");
		for (int k = 0; k < p->nconn; k++)
			hh_printf(&o, " %02x%s", p->conn[k], k == p->mux ? "*" : "");
		hh_printf(&o, "  slot %d, cvt %d, unsol %u, eld reads %u errors %u\n", p->slot,
			  p->cvt, p->unsol_count, p->eld_reads, p->eld_errors);
		if (hc->mst && p->ndev > 1) {
			uint8_t ent[HH_MAX_DEVS];
			int ne = hh_dev_list(hc, p, ent, HH_MAX_DEVS);
			hh_printf(&o, "    devices:");
			for (int d = 0; d < ne; d++)
				hh_printf(&o, " %d:%s%s%s", d, (ent[d] & HH_DE_PD) ? "P" : "-",
					  (ent[d] & HH_DE_ELDV) ? "E" : "-",
					  (ent[d] & HH_DE_IA) ? "A" : "-");
			hh_printf(&o, "\n");
			hh_dev_select(hc, p, p->dev);
		}
		if (p->eld.valid) {
			struct hh_eld *e = &p->eld;
			hh_printf(&o, "    monitor \"%s\" %s, ELD v%u cea %u, %d bytes, mfg %04x prod %04x\n",
				  e->monitor, e->conn_type ? "DP" : "HDMI", e->ver, e->cea_ver, e->len,
				  e->manufacturer, e->product);
			hh_printf(&o, "    speakers:");
			for (int b = 0; b < 7; b++)
				if (e->spk_alloc & (1u << b))
					hh_printf(&o, " %s", hh_eld_spk_names[b]);
			hh_printf(&o, ", sync delay %u ms%s%s\n", e->aud_synch_delay * 2u,
				  e->supports_ai ? ", AI" : "", e->supports_hdcp ? ", HDCP" : "");
			for (int s = 0; s < e->sad_count; s++) {
				const struct hh_sad *sd = &e->sad[s];
				hh_printf(&o, "    sad %d: %s %uch rates %02x b2 %02x\n", s,
					  hh_sad_format_names[sd->format & 15], sd->channels, sd->rates,
					  sd->byte2);
			}
		}
		if (p->streaming) {
			hh_printf(&o, "    stream: %uch ca %02x slots", p->channels, (uint32_t)p->ca);
			for (int s = 0; s < 8; s++)
				hh_printf(&o, " %x", p->slot_ch[s]);
			hh_printf(&o, "\n");
		}
	}
	for (int s = 0; s < hc->nslots; s++) {
		struct hh_slot *sl = &hc->slot[s];
		hh_printf(&o, "  pcm HDMI %d: pin %d %s%s rate %u ch %u bits %u tag %d\n",
			  sl->global_index, sl->pin, sl->open ? "open" : "idle",
			  sl->prepared ? " prepared" : "", sl->rate, sl->channels, sl->bits,
			  sl->st ? sl->st->tag : 0);
	}
	hh_unlock(hc);
	return (long)o.pos;
}
