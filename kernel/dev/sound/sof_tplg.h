// LikeOS Intel audio DSP - topology interface (sof_topology.c) used by the
// PCM side (sof_pcm.c).
//
// A topology file describes the DSP processing graph for a machine: PCM
// devices exposed to the host, pipelines of modules (host copier, gain,
// mixer, DAI copier...), DAI links (SoundWire/ALH, SSP, DMIC, HDA) and
// their configuration.  sof_topology.c parses it and drives the firmware
// through IPC4 to instantiate, connect and run the pipelines.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_SOF_TPLG_H
#define KERNEL_DEV_SOUND_SOF_TPLG_H

#include "sof.h"

#define SOF_TPLG_NAME_MAX 44

/* DAI types a PCM path can end at */
#define SOF_DAI_NONE 0
#define SOF_DAI_HDA 1 /* HDA link (HDMI codecs) */
#define SOF_DAI_ALH 2 /* SoundWire through the audio link hub */
#define SOF_DAI_SSP 3
#define SOF_DAI_DMIC 4

struct sof_tplg_pcm {
	int id; /* the topology's PCM id */
	char name[SOF_TPLG_NAME_MAX]; /* "Jack Out", "Speaker", "HDMI1" */
	int has_playback, has_capture;
	uint32_t formats; /* AUDIO_FMT_BIT() mask */
	uint32_t rates; /* AUDIO_RATE_* mask */
	uint32_t channels_min, channels_max;
	int dai_type; /* SOF_DAI_* the playback path ends at */
	int dai_index; /* link number / SSP index / HDA codec pin */
	uint32_t output_type; /* AUDIO_OUT_* this PCM feeds */
};

/* Load and parse a topology file (path under /lib/firmware) and append
 * its PCMs/pipelines.  Several "function topologies" may be loaded one
 * after another.  0 or -errno. */
int sof_tplg_load(struct sof_dev *sdev, const char *path);
/* Free everything (also on failure). */
void sof_tplg_free(struct sof_dev *sdev);

int sof_tplg_pcm_count(struct sof_dev *sdev);
const struct sof_tplg_pcm *sof_tplg_pcm_get(struct sof_dev *sdev, int idx);

/* Instantiate the pipelines of a PCM direction in the firmware for the
 * given format, with `host_stream_tag' the HDA host DMA stream the copier
 * reads from (playback) or writes to (capture).  Also configures the DAI
 * (calling sof_dai_sdw_config / sdw_stream_prepare for SoundWire). */
int sof_tplg_pcm_prepare(struct sof_dev *sdev, int idx, int direction,
			 uint32_t rate, uint32_t channels, uint32_t bits,
			 int host_stream_tag);
/* SND_TRIGGER_* -> pipeline state RUNNING/PAUSED. */
int sof_tplg_pcm_trigger(struct sof_dev *sdev, int idx, int direction,
			 int cmd);
/* Delete the pipelines again. */
int sof_tplg_pcm_free(struct sof_dev *sdev, int idx, int direction);

/* Gain module control for software-visible volume (0..100), when the
 * topology has a gain/volume module on the path. */
int sof_tplg_set_volume(struct sof_dev *sdev, int idx, uint32_t l,
			uint32_t r);

/* Text dump for AUDIO_DIAG. */
long sof_tplg_dump(struct sof_dev *sdev, char *buf, size_t len);

#endif /* KERNEL_DEV_SOUND_SOF_TPLG_H */
