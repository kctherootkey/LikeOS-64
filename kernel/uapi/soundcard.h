// LikeOS OSS-compatible sound UAPI - /dev/dsp and /dev/mixer
//
// The classic Open Sound System interface: request numbers, sample format
// bits and structures as programs written for it expect them, so ported
// software (GStreamer's osssink among it) works against /dev/dsp unchanged.
// Only the playback/capture and mixer subset listed here is implemented;
// anything else answers -EINVAL.  The native interface is
// <kernel/uapi/sound.h>.
//
// Mirrored into the userspace libc include tree as <sys/soundcard.h>; keep
// both copies in sync.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_SOUNDCARD_H_
#define _KERNEL_UAPI_SOUNDCARD_H_

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

#define SOUND_VERSION 0x030802
#define OPEN_SOUND_SYSTEM

/* The OSS request macros are the ordinary encoding under other names. */
#define _SIO(x, y) _IO(x, y)
#define _SIOR(x, y, t) _IOR(x, y, t)
#define _SIOW(x, y, t) _IOW(x, y, t)
#define _SIOWR(x, y, t) _IOWR(x, y, t)

/* ---- sample formats ---------------------------------------------------- */

#define AFMT_QUERY 0x00000000
#define AFMT_MU_LAW 0x00000001
#define AFMT_A_LAW 0x00000002
#define AFMT_IMA_ADPCM 0x00000004
#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010
#define AFMT_S16_BE 0x00000020
#define AFMT_S8 0x00000040
#define AFMT_U16_LE 0x00000080
#define AFMT_U16_BE 0x00000100
#define AFMT_MPEG 0x00000200
#define AFMT_AC3 0x00000400
#define AFMT_S32_LE 0x00001000
#define AFMT_S32_BE 0x00002000
#define AFMT_FLOAT 0x00004000
#define AFMT_S24_LE 0x00008000
#define AFMT_S24_BE 0x00010000
#define AFMT_SPDIF_RAW 0x00020000
#define AFMT_S24_PACKED 0x00040000

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define AFMT_S16_NE AFMT_S16_BE
#define AFMT_U16_NE AFMT_U16_BE
#define AFMT_S32_NE AFMT_S32_BE
#define AFMT_S24_NE AFMT_S24_BE
#else
#define AFMT_S16_NE AFMT_S16_LE
#define AFMT_U16_NE AFMT_U16_LE
#define AFMT_S32_NE AFMT_S32_LE
#define AFMT_S24_NE AFMT_S24_LE
#endif

/* ---- structures ----------------------------------------------------------- */

typedef struct audio_buf_info {
	int fragments; /* whole fragments that fit without blocking */
	int fragstotal; /* fragments in the buffer */
	int fragsize; /* bytes per fragment */
	int bytes; /* bytes that fit without blocking */
} audio_buf_info;

typedef struct count_info {
	int bytes; /* bytes processed since the start (wraps) */
	int blocks; /* fragments processed since the last call */
	int ptr; /* DMA position inside the buffer */
} count_info;

typedef struct buffmem_desc {
	unsigned *buffer;
	int size;
} buffmem_desc;

typedef struct mixer_info {
	char id[16];
	char name[32];
	int modify_counter;
	int fillers[10];
} mixer_info;

typedef struct oss_sysinfo {
	char product[32];
	char version[32];
	int versionnum;
	char options[128];
	int numaudios;
	int openedaudio[8];
	int numsynths;
	int nummidis;
	int numtimers;
	int nummixers;
	int openedmidi[8];
	int numcards;
	int numaudioengines;
	char license[16];
	char revision_info[256];
	int filler[172];
} oss_sysinfo;

/* ---- /dev/dsp requests ---------------------------------------------------- */

#define SNDCTL_DSP_RESET _SIO('P', 0)
#define SNDCTL_DSP_HALT SNDCTL_DSP_RESET
#define SNDCTL_DSP_SYNC _SIO('P', 1)
#define SNDCTL_DSP_SPEED _SIOWR('P', 2, int)
#define SNDCTL_DSP_STEREO _SIOWR('P', 3, int)
#define SNDCTL_DSP_GETBLKSIZE _SIOWR('P', 4, int)
#define SNDCTL_DSP_SAMPLESIZE SNDCTL_DSP_SETFMT
#define SNDCTL_DSP_SETFMT _SIOWR('P', 5, int)
#define SNDCTL_DSP_CHANNELS _SIOWR('P', 6, int)
#define SOUND_PCM_WRITE_CHANNELS SNDCTL_DSP_CHANNELS
#define SOUND_PCM_WRITE_FILTER _SIOWR('P', 7, int)
#define SNDCTL_DSP_POST _SIO('P', 8)
#define SNDCTL_DSP_SUBDIVIDE _SIOWR('P', 9, int)
#define SNDCTL_DSP_SETFRAGMENT _SIOWR('P', 10, int)
#define SNDCTL_DSP_GETFMTS _SIOR('P', 11, int)
#define SNDCTL_DSP_GETOSPACE _SIOR('P', 12, audio_buf_info)
#define SNDCTL_DSP_GETISPACE _SIOR('P', 13, audio_buf_info)
#define SNDCTL_DSP_NONBLOCK _SIO('P', 14)
#define SNDCTL_DSP_GETCAPS _SIOR('P', 15, int)
#define SNDCTL_DSP_GETTRIGGER _SIOR('P', 16, int)
#define SNDCTL_DSP_SETTRIGGER _SIOW('P', 16, int)
#define SNDCTL_DSP_GETIPTR _SIOR('P', 17, count_info)
#define SNDCTL_DSP_GETOPTR _SIOR('P', 18, count_info)
#define SNDCTL_DSP_MAPINBUF _SIOR('P', 19, buffmem_desc)
#define SNDCTL_DSP_MAPOUTBUF _SIOR('P', 20, buffmem_desc)
#define SNDCTL_DSP_SETSYNCRO _SIO('P', 21)
#define SNDCTL_DSP_SETDUPLEX _SIO('P', 22)
#define SNDCTL_DSP_GETODELAY _SIOR('P', 23, int)
#define SNDCTL_DSP_HALT_INPUT _SIO('P', 33)
#define SNDCTL_DSP_HALT_OUTPUT _SIO('P', 34)
#define SNDCTL_DSP_LOW_WATER _SIOW('P', 34, int)
#define SNDCTL_DSP_SKIP _SIO('P', 36)
#define SNDCTL_DSP_SILENCE _SIO('P', 37)

#define SOUND_PCM_READ_RATE _SIOR('P', 2, int)
#define SOUND_PCM_READ_CHANNELS _SIOR('P', 6, int)
#define SOUND_PCM_READ_BITS _SIOR('P', 5, int)

/* SNDCTL_DSP_GETCAPS bits */
#define DSP_CAP_REVISION 0x000000ff
#define DSP_CAP_DUPLEX 0x00000100
#define DSP_CAP_REALTIME 0x00000200
#define DSP_CAP_BATCH 0x00000400
#define DSP_CAP_COPROC 0x00000800
#define DSP_CAP_TRIGGER 0x00001000
#define DSP_CAP_MMAP 0x00002000
#define DSP_CAP_MULTI 0x00004000
#define DSP_CAP_BIND 0x00008000
#define DSP_CAP_INPUT 0x00010000
#define DSP_CAP_OUTPUT 0x00020000

/* SNDCTL_DSP_SETTRIGGER bits */
#define PCM_ENABLE_INPUT 0x00000001
#define PCM_ENABLE_OUTPUT 0x00000002

/* ---- /dev/mixer requests --------------------------------------------------- */

#define SOUND_MIXER_NRDEVICES 25
#define SOUND_MIXER_VOLUME 0
#define SOUND_MIXER_BASS 1
#define SOUND_MIXER_TREBLE 2
#define SOUND_MIXER_SYNTH 3
#define SOUND_MIXER_PCM 4
#define SOUND_MIXER_SPEAKER 5
#define SOUND_MIXER_LINE 6
#define SOUND_MIXER_MIC 7
#define SOUND_MIXER_CD 8
#define SOUND_MIXER_IMIX 9
#define SOUND_MIXER_ALTPCM 10
#define SOUND_MIXER_RECLEV 11
#define SOUND_MIXER_IGAIN 12
#define SOUND_MIXER_OGAIN 13
#define SOUND_MIXER_LINE1 14
#define SOUND_MIXER_LINE2 15
#define SOUND_MIXER_LINE3 16
#define SOUND_MIXER_DIGITAL1 17
#define SOUND_MIXER_DIGITAL2 18
#define SOUND_MIXER_DIGITAL3 19
#define SOUND_MIXER_PHONEIN 20
#define SOUND_MIXER_PHONEOUT 21
#define SOUND_MIXER_VIDEO 22
#define SOUND_MIXER_RADIO 23
#define SOUND_MIXER_MONITOR 24

#define SOUND_MIXER_RECSRC 0xff
#define SOUND_MIXER_DEVMASK 0xfe
#define SOUND_MIXER_RECMASK 0xfd
#define SOUND_MIXER_CAPS 0xfc
#define SOUND_MIXER_STEREODEVS 0xfb
#define SOUND_MIXER_OUTSRC 0xfa
#define SOUND_MIXER_OUTMASK 0xf9

#define SOUND_MASK_VOLUME (1 << SOUND_MIXER_VOLUME)
#define SOUND_MASK_PCM (1 << SOUND_MIXER_PCM)
#define SOUND_MASK_SPEAKER (1 << SOUND_MIXER_SPEAKER)
#define SOUND_MASK_LINE (1 << SOUND_MIXER_LINE)
#define SOUND_MASK_MIC (1 << SOUND_MIXER_MIC)
#define SOUND_MASK_IGAIN (1 << SOUND_MIXER_IGAIN)
#define SOUND_MASK_OGAIN (1 << SOUND_MIXER_OGAIN)

#define SOUND_CAP_EXCL_INPUT 0x00000001

#define MIXER_READ(dev) _SIOR('M', dev, int)
#define MIXER_WRITE(dev) _SIOWR('M', dev, int)
#define SOUND_MIXER_READ_VOLUME MIXER_READ(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_READ_PCM MIXER_READ(SOUND_MIXER_PCM)
#define SOUND_MIXER_READ_SPEAKER MIXER_READ(SOUND_MIXER_SPEAKER)
#define SOUND_MIXER_READ_LINE MIXER_READ(SOUND_MIXER_LINE)
#define SOUND_MIXER_READ_MIC MIXER_READ(SOUND_MIXER_MIC)
#define SOUND_MIXER_READ_IGAIN MIXER_READ(SOUND_MIXER_IGAIN)
#define SOUND_MIXER_READ_RECSRC MIXER_READ(SOUND_MIXER_RECSRC)
#define SOUND_MIXER_READ_DEVMASK MIXER_READ(SOUND_MIXER_DEVMASK)
#define SOUND_MIXER_READ_RECMASK MIXER_READ(SOUND_MIXER_RECMASK)
#define SOUND_MIXER_READ_STEREODEVS MIXER_READ(SOUND_MIXER_STEREODEVS)
#define SOUND_MIXER_READ_CAPS MIXER_READ(SOUND_MIXER_CAPS)
#define SOUND_MIXER_WRITE_VOLUME MIXER_WRITE(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_WRITE_PCM MIXER_WRITE(SOUND_MIXER_PCM)
#define SOUND_MIXER_WRITE_SPEAKER MIXER_WRITE(SOUND_MIXER_SPEAKER)
#define SOUND_MIXER_WRITE_LINE MIXER_WRITE(SOUND_MIXER_LINE)
#define SOUND_MIXER_WRITE_MIC MIXER_WRITE(SOUND_MIXER_MIC)
#define SOUND_MIXER_WRITE_IGAIN MIXER_WRITE(SOUND_MIXER_IGAIN)
#define SOUND_MIXER_WRITE_RECSRC MIXER_WRITE(SOUND_MIXER_RECSRC)

#define SOUND_MIXER_INFO _SIOR('M', 101, mixer_info)
#define OSS_GETVERSION _SIOR('M', 118, int)
#define SNDCTL_SYSINFO _SIOR('X', 1, oss_sysinfo)

/* Mixer values pack left in bits 0-7 and right in bits 8-15, 0..100. */
#define SOUND_MIXER_LEVEL(l, r) (((l) & 0xff) | (((r) & 0xff) << 8))

#define SOUND_DEVICE_LABELS                                                  \
	{ "Vol  ", "Bass ", "Trebl", "Synth", "Pcm  ", "Spkr ", "Line ",     \
	  "Mic  ", "CD   ", "Mix  ", "Pcm2 ", "Rec  ", "IGain", "OGain",     \
	  "Line1", "Line2", "Line3", "Digital1", "Digital2", "Digital3",   \
	  "PhoneIn", "PhoneOut", "Video", "Radio", "Monitor" }

#define SOUND_DEVICE_NAMES                                                   \
	{ "vol", "bass", "treble", "synth", "pcm", "speaker", "line",        \
	  "mic", "cd", "mix", "pcm2", "rec", "igain", "ogain", "line1",      \
	  "line2", "line3", "dig1", "dig2", "dig3", "phin", "phout",         \
	  "video", "radio", "monitor" }

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_UAPI_SOUNDCARD_H_ */
