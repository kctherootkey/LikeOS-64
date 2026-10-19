// LikeOS audio device UAPI - /dev/audioN native interface
//
// The PCM nodes /dev/audioN (and the OSS-compatible /dev/dsp alias of the
// same card) speak two dialects through ioctl(): the request numbers in
// <kernel/uapi/soundcard.h> for programs written against the classic OSS
// interface, and the native requests below, which the audio service (audiod)
// and the system tools use.  Data moves with write() (playback) and read()
// (capture), or through mmap() of the DMA ring plus AUDIO_COMMIT.
//
// Mirrored into the userspace libc include tree as <sys/audio.h>; keep both
// copies in sync.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_SOUND_H_
#define _KERNEL_UAPI_SOUND_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#include <kernel/uapi/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define AUDIO_API_VERSION 1

/* Character device numbers.  The minor of card n is (n << 4) | function. */
#define AUDIO_MAJOR 14
#define AUDIO_MINOR_MIXER 0 /* /dev/mixer[n]: controls only */
#define AUDIO_MINOR_DSP 3 /* /dev/dsp[n]: OSS-compatible PCM */
#define AUDIO_MINOR_AUDIO 4 /* /dev/audio[n]: native PCM */
#define AUDIO_MAX_CARDS 8

/* ---- sample formats -------------------------------------------------- */

/* Little-endian, interleaved.  The kernel never converts: it accepts what
 * the hardware can play and reports that in audio_info.formats. */
#define AUDIO_FMT_S16_LE 1 /* signed 16 bit */
#define AUDIO_FMT_S24_LE 2 /* signed 24 bit in the low bits of 32 */
#define AUDIO_FMT_S32_LE 3 /* signed 32 bit */
#define AUDIO_FMT_U8 4 /* unsigned 8 bit */
#define AUDIO_FMT_S24_3LE 5 /* signed 24 bit packed in 3 bytes */
#define AUDIO_FMT_FLOAT_LE 6 /* IEEE 754 single (DSP firmware only) */
#define AUDIO_FMT_S20_LE 7 /* signed 20 bit in the low bits of 32 */
#define AUDIO_FMT_MAX 7
#define AUDIO_FMT_BIT(f) (1u << (f))

/* ---- sample rates ---------------------------------------------------- */

#define AUDIO_RATE_8000 (1u << 0)
#define AUDIO_RATE_11025 (1u << 1)
#define AUDIO_RATE_16000 (1u << 2)
#define AUDIO_RATE_22050 (1u << 3)
#define AUDIO_RATE_32000 (1u << 4)
#define AUDIO_RATE_44100 (1u << 5)
#define AUDIO_RATE_48000 (1u << 6)
#define AUDIO_RATE_88200 (1u << 7)
#define AUDIO_RATE_96000 (1u << 8)
#define AUDIO_RATE_176400 (1u << 9)
#define AUDIO_RATE_192000 (1u << 10)
#define AUDIO_RATE_384000 (1u << 11)

/* ---- directions and states ------------------------------------------- */

#define AUDIO_DIR_PLAYBACK 0
#define AUDIO_DIR_CAPTURE 1

#define AUDIO_STATE_OPEN 0 /* no parameters yet */
#define AUDIO_STATE_SETUP 1 /* parameters set, buffer allocated */
#define AUDIO_STATE_PREPARED 2 /* ready to start */
#define AUDIO_STATE_RUNNING 3
#define AUDIO_STATE_PAUSED 4
#define AUDIO_STATE_DRAINING 5
#define AUDIO_STATE_XRUN 6 /* underrun (playback) or overrun (capture) */
#define AUDIO_STATE_DISCONNECTED 7 /* the hardware went away */

/* ---- AUDIO_GETINFO --------------------------------------------------- */

#define AUDIO_INFO_PLAYBACK (1u << 0)
#define AUDIO_INFO_CAPTURE (1u << 1)
#define AUDIO_INFO_MMAP (1u << 2) /* the ring can be mapped */
#define AUDIO_INFO_JACK_DETECT (1u << 3)
#define AUDIO_INFO_HDMI (1u << 4)
#define AUDIO_INFO_HW_VOLUME (1u << 5) /* a hardware amplifier exists */

struct audio_info {
	uint32_t version; /* AUDIO_API_VERSION */
	uint32_t card; /* card index */
	char card_name[64]; /* "HDA Intel PCH" */
	char driver[32]; /* "hda", "ac97", "es1371", "sof" */
	char codec[64]; /* "Realtek ALC298" */
	char bus[32]; /* "pci:0000:00:1f.3" */
	uint32_t flags; /* AUDIO_INFO_* */
	uint32_t formats; /* AUDIO_FMT_BIT() mask, playback */
	uint32_t rates; /* AUDIO_RATE_* mask, playback */
	uint32_t min_channels, max_channels;
	uint32_t capture_formats, capture_rates;
	uint32_t capture_min_channels, capture_max_channels;
	uint32_t min_period_bytes, max_period_bytes;
	uint32_t min_periods, max_periods;
	uint32_t max_buffer_bytes;
	uint32_t num_outputs; /* see AUDIO_GETOUTPUTS */
	uint32_t num_inputs;
	uint32_t reserved[16];
};

/* ---- AUDIO_SETPARAMS / AUDIO_GETPARAMS -------------------------------- */

/* In: what the caller wants.  Out: what was granted (the nearest the
 * hardware can do; the caller converts if it differs).  Allowed only while
 * the stream is not running.  period_frames/periods 0 = driver default. */
struct audio_params {
	uint32_t direction; /* AUDIO_DIR_* */
	uint32_t format; /* AUDIO_FMT_* */
	uint32_t rate; /* frames per second */
	uint32_t channels;
	uint32_t period_frames;
	uint32_t periods;
	uint32_t buffer_bytes; /* out: size of the ring (mmap length) */
	uint32_t frame_bytes; /* out */
	uint32_t flags; /* AUDIO_PARAM_* */
	uint32_t reserved[7];
};

#define AUDIO_PARAM_NO_AUTOSTART (1u << 0) /* write() does not start */
#define AUDIO_PARAM_MMAP (1u << 1) /* data moves through mmap+COMMIT */

/* ---- AUDIO_GETPOS ----------------------------------------------------- */

struct audio_pos {
	uint64_t hw_frames; /* frames the hardware has consumed/produced */
	uint64_t appl_frames; /* frames written/read by the application */
	uint32_t delay_frames; /* until the next written frame is heard */
	uint32_t avail_frames; /* writable (playback) / readable (capture) */
	uint64_t timestamp_ns; /* monotonic time of hw_frames */
	uint32_t state; /* AUDIO_STATE_* */
	uint32_t xruns; /* since open */
	uint32_t hw_offset_bytes; /* DMA position inside the ring */
	uint32_t reserved[5];
};

/* ---- volume, mute, outputs ------------------------------------------- */

#define AUDIO_CTL_MASTER 0
#define AUDIO_CTL_PCM 1
#define AUDIO_CTL_SPEAKER 2
#define AUDIO_CTL_HEADPHONE 3
#define AUDIO_CTL_LINE 4
#define AUDIO_CTL_HDMI 5
#define AUDIO_CTL_CAPTURE 6
#define AUDIO_CTL_MIC_BOOST 7
#define AUDIO_CTL_MAX 7

/* left/right are 0..100.  db_* are hundredths of a decibel at the current
 * setting and the control's range (0 and 0 when the control has no dB
 * scale). */
struct audio_volume {
	uint32_t control; /* AUDIO_CTL_* */
	uint32_t left, right;
	int32_t db_left, db_right;
	int32_t db_min, db_max;
	uint32_t steps; /* hardware steps, 0 = software only */
	uint32_t reserved[4];
};

struct audio_mute {
	uint32_t control;
	uint32_t mute; /* 1 = muted */
	uint32_t reserved[2];
};

#define AUDIO_OUT_SPEAKER 1
#define AUDIO_OUT_HEADPHONE 2
#define AUDIO_OUT_LINE 3
#define AUDIO_OUT_HDMI 4
#define AUDIO_OUT_DP 5
#define AUDIO_OUT_SPDIF 6
#define AUDIO_OUT_HEADSET 7

#define AUDIO_OUTF_JACK_DETECT (1u << 0) /* `present' is meaningful */
#define AUDIO_OUTF_DOCK (1u << 1) /* on a docking station */
#define AUDIO_OUTF_FIXED (1u << 2) /* built in, always present */

#define AUDIO_MAX_OUTPUTS 16

struct audio_output_desc {
	uint32_t id;
	uint32_t type; /* AUDIO_OUT_* */
	uint32_t flags; /* AUDIO_OUTF_* */
	uint32_t present; /* something is plugged in (or built in) */
	uint32_t active; /* sound goes out here now */
	char name[32]; /* "Speaker", "Headphone", "HDMI 1" */
	char monitor[32]; /* HDMI/DP: the monitor's name from its ELD */
	uint32_t reserved[3];
};

/* AUDIO_SETOUTPUT modes */
#define AUDIO_OUTSEL_AUTO 0 /* headphone when present, else speaker */
#define AUDIO_OUTSEL_ID 1 /* exactly output `id' */
#define AUDIO_OUTSEL_ALL 2 /* every present output */

struct audio_outputs {
	uint32_t count;
	uint32_t mode; /* AUDIO_OUTSEL_* in force */
	uint32_t selected; /* the id for AUDIO_OUTSEL_ID */
	uint32_t reserved;
	struct audio_output_desc out[AUDIO_MAX_OUTPUTS];
};

struct audio_setoutput {
	uint32_t mode;
	uint32_t id;
	uint32_t reserved[2];
};

/* ---- events (AUDIO_GETEVENT; poll() reports POLLPRI while queued) ------ */

#define AUDIO_EV_JACK 1 /* output/input `id' plugged (value 1) or out */
#define AUDIO_EV_XRUN 2 /* value = direction */
#define AUDIO_EV_VOLUME 3 /* a control changed: id = AUDIO_CTL_* */
#define AUDIO_EV_OUTPUT 4 /* the active output set changed */
#define AUDIO_EV_HOTPLUG 5 /* HDMI/DP monitor with audio came or went */
#define AUDIO_EV_DISCONNECT 6 /* the card is gone */

struct audio_event {
	uint32_t type;
	uint32_t id;
	uint32_t value;
	uint32_t reserved;
	uint64_t timestamp_ns;
};

/* ---- diagnostics (AUDIO_DIAG) ----------------------------------------- */

/* The driver writes a human-readable dump (controller, codecs, widget graph,
 * pin configuration, streams) into the caller's buffer.  `buf' is a user
 * pointer; `written' comes back as the number of bytes stored. */
struct audio_diag {
	uint64_t buf;
	uint32_t len;
	uint32_t written;
	uint32_t what; /* AUDIO_DIAG_* */
	uint32_t reserved[3];
};

#define AUDIO_DIAG_SUMMARY 0
#define AUDIO_DIAG_CODECS 1 /* full codec and widget dump */
#define AUDIO_DIAG_STREAMS 2

/* ---- PCM devices (AUDIO_GETPCMINFO / AUDIO_SELECTPCM) ------------------- */

/* A card can have several PCM devices: the analog one and one per HDMI/DP
 * port.  An open handle starts on the first device that has the directions
 * it was opened for; AUDIO_SELECTPCM moves it to another one (while not
 * running).  `index' is an input of AUDIO_GETPCMINFO. */
#define AUDIO_PCM_PLAYBACK (1u << 0)
#define AUDIO_PCM_CAPTURE (1u << 1)
#define AUDIO_PCM_SELECTED (1u << 2) /* the handle uses this device */
#define AUDIO_PCM_BUSY (1u << 3) /* another handle has it open */

struct audio_pcm_info {
	uint32_t index; /* in: which device */
	uint32_t count; /* out: devices on the card */
	uint32_t flags; /* AUDIO_PCM_* */
	uint32_t output_id; /* the output it feeds, 0 = the general device */
	uint32_t output_type; /* AUDIO_OUT_*, 0 = analog/general */
	char name[32]; /* "Analog", "HDMI 0" */
	uint32_t reserved[7];
};

/* ---- requests ---------------------------------------------------------- */

#define AUDIO_IOC_TYPE 'A'

#define AUDIO_GETVERSION _IOR(AUDIO_IOC_TYPE, 0x80, uint32_t)
#define AUDIO_GETINFO _IOR(AUDIO_IOC_TYPE, 0x81, struct audio_info)
#define AUDIO_SETPARAMS _IOWR(AUDIO_IOC_TYPE, 0x82, struct audio_params)
#define AUDIO_GETPARAMS _IOR(AUDIO_IOC_TYPE, 0x83, struct audio_params)
#define AUDIO_GETPOS _IOR(AUDIO_IOC_TYPE, 0x84, struct audio_pos)
#define AUDIO_START _IO(AUDIO_IOC_TYPE, 0x85)
#define AUDIO_PAUSE _IO(AUDIO_IOC_TYPE, 0x86)
#define AUDIO_RESUME _IO(AUDIO_IOC_TYPE, 0x87)
#define AUDIO_DRAIN _IO(AUDIO_IOC_TYPE, 0x88) /* block until played */
#define AUDIO_DROP _IO(AUDIO_IOC_TYPE, 0x89) /* stop and discard */
#define AUDIO_PREPARE _IO(AUDIO_IOC_TYPE, 0x8a) /* recover from XRUN */
#define AUDIO_COMMIT _IOW(AUDIO_IOC_TYPE, 0x8b, uint32_t) /* mmap frames */
#define AUDIO_GETVOL _IOWR(AUDIO_IOC_TYPE, 0x90, struct audio_volume)
#define AUDIO_SETVOL _IOWR(AUDIO_IOC_TYPE, 0x91, struct audio_volume)
#define AUDIO_GETMUTE _IOWR(AUDIO_IOC_TYPE, 0x92, struct audio_mute)
#define AUDIO_SETMUTE _IOW(AUDIO_IOC_TYPE, 0x93, struct audio_mute)
#define AUDIO_GETOUTPUTS _IOR(AUDIO_IOC_TYPE, 0x94, struct audio_outputs)
#define AUDIO_SETOUTPUT _IOW(AUDIO_IOC_TYPE, 0x95, struct audio_setoutput)
#define AUDIO_GETINPUTS _IOR(AUDIO_IOC_TYPE, 0x96, struct audio_outputs)
#define AUDIO_SETINPUT _IOW(AUDIO_IOC_TYPE, 0x97, struct audio_setoutput)
#define AUDIO_GETEVENT _IOR(AUDIO_IOC_TYPE, 0x98, struct audio_event)
#define AUDIO_DIAG _IOWR(AUDIO_IOC_TYPE, 0x99, struct audio_diag)
#define AUDIO_GETPCMINFO _IOWR(AUDIO_IOC_TYPE, 0x9a, struct audio_pcm_info)
#define AUDIO_SELECTPCM _IOW(AUDIO_IOC_TYPE, 0x9b, uint32_t)

/* Helpers */
static inline uint32_t audio_format_bytes(uint32_t fmt)
{
	switch (fmt) {
	case AUDIO_FMT_U8:
		return 1;
	case AUDIO_FMT_S16_LE:
		return 2;
	case AUDIO_FMT_S24_3LE:
		return 3;
	case AUDIO_FMT_S24_LE:
	case AUDIO_FMT_S20_LE:
	case AUDIO_FMT_S32_LE:
	case AUDIO_FMT_FLOAT_LE:
		return 4;
	default:
		return 0;
	}
}

static inline uint32_t audio_rate_bit(uint32_t rate)
{
	switch (rate) {
	case 8000: return AUDIO_RATE_8000;
	case 11025: return AUDIO_RATE_11025;
	case 16000: return AUDIO_RATE_16000;
	case 22050: return AUDIO_RATE_22050;
	case 32000: return AUDIO_RATE_32000;
	case 44100: return AUDIO_RATE_44100;
	case 48000: return AUDIO_RATE_48000;
	case 88200: return AUDIO_RATE_88200;
	case 96000: return AUDIO_RATE_96000;
	case 176400: return AUDIO_RATE_176400;
	case 192000: return AUDIO_RATE_192000;
	case 384000: return AUDIO_RATE_384000;
	default: return 0;
	}
}

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_UAPI_SOUND_H_ */
