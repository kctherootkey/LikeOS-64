// LikeOS Intel High Definition Audio - HDMI/DisplayPort codec helpers that
// the controller side and the card's control interface may call in
// addition to what hda.h declares (hda_hdmi_add_pcms, hda_hdmi_notify).
//
// Output ids: every HDMI/DP PCM device of a display codec is one output in
// AUDIO_GETOUTPUTS, with the id HDA_HDMI_OUT_ID(codec address, slot), so
// they never collide with the analog outputs of the card (whose ids are
// small numbers).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_HDA_HDMI_H
#define KERNEL_DEV_SOUND_HDA_HDMI_H

#include "hda.h"

#define HDA_HDMI_OUT_ID_BASE 0x100u
#define HDA_HDMI_OUT_ID(addr, slot) \
	(HDA_HDMI_OUT_ID_BASE + ((uint32_t)(addr) & 0xf) * 16u + ((uint32_t)(slot) & 0xf))
/* Is `id' one of the HDMI/DP output ids (any codec)? */
#define HDA_HDMI_IS_OUT_ID(id) ((id) >= HDA_HDMI_OUT_ID_BASE && (id) < HDA_HDMI_OUT_ID_BASE + 0x100u)

/* Is `c' a display codec driven by hda_hdmi.c? */
int hda_hdmi_is_display_codec(struct hda_codec *c);

/* The HDMI/DP outputs of display codec `c' (one per PCM device it
 * created), at most `max' of them, into `out'.  Returns the number
 * written (0 for a codec that is not a display codec). */
int hda_hdmi_get_outputs(struct hda_codec *c, struct audio_output_desc *out, int max);

/* Mute (1) or unmute (0) every HDMI/DP pin of the codec (the pin's
 * output amplifier where it has one, else the converter's digital
 * enable).  0 or -ENOENT when the codec has nothing to mute. */
int hda_hdmi_set_mute(struct hda_codec *c, uint32_t mute);
int hda_hdmi_get_mute(struct hda_codec *c, uint32_t *mute);

/* A text dump of the codec's pins, converters, device entries and ELDs
 * for AUDIO_DIAG; returns the length written into `buf' (kernel memory,
 * always NUL-terminated when len > 0). */
long hda_hdmi_dump(struct hda_codec *c, char *buf, size_t len);

/* Re-read presence and ELD of every pin of every display codec (resume,
 * or a caller that suspects it missed a notification). */
void hda_hdmi_rescan_all(void);

#endif /* KERNEL_DEV_SOUND_HDA_HDMI_H */
