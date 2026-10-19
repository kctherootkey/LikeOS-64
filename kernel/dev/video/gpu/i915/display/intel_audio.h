// LikeOS -- display audio: the graphics half of HDMI and DisplayPort sound.
//
// The HD Audio controller's display codec plays into the display engine:
// the transcoder carries the samples to the port.  For that the display
// side has to tell the codec what the monitor takes (its ELD, written
// into the transcoder's hardware ELD buffer and handed to the sound core
// on request), raise "presence" for the codec's pin, program the audio
// clock relation (HDMI N/CTS, DisplayPort Maud/Naud) for the pixel or link
// clock, keep the audio power well and the display core clock (at least
// twice the 96 MHz audio bit clock) up while sound may play, and tell the
// sound core whenever a monitor with audio comes or goes.
//
// intel_audio.c implements that for the DDI display (Haswell and later),
// registered with the sound core through snd_display_audio_register().
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_AUDIO_H
#define KERNEL_DEV_GPU_I915_INTEL_AUDIO_H

#include <kernel/uapi/types.h>

struct i915_device;
struct intel_output;
struct intel_pipe;

/* Once the DDI display is up (i915_drv.c): audio registers to their
 * defaults, the audio well up, the hooks registered with the sound core.
 * Does nothing on the older displays (intel_legacy_*.c). */
void intel_audio_init(struct i915_device *i915);
void intel_audio_fini(struct i915_device *i915);
/* Around system suspend: the power-well counts are rebuilt by the
 * display's resume, the audio's own references are taken again after. */
void intel_audio_suspend(struct i915_device *i915);
void intel_audio_resume(struct i915_device *i915);

/* Does the sink on output `o' take audio (an HDMI sink or an external
 * DisplayPort sink whose EDID lists audio)?  Used for the CDCLK rule of
 * a configuration about to be set. */
int intel_audio_output_wants_audio(struct i915_device *i915, const struct intel_output *o);
/* Is audio enabled on output `o' right now? */
int intel_audio_output_active(struct i915_device *i915, const struct intel_output *o);

/* After the transcoder, the port and the link of `o' on pipe `p' are up
 * (display lock held): audio on, if the sink takes it. */
void intel_audio_codec_enable(struct i915_device *i915, struct intel_output *o,
			      struct intel_pipe *p);
/* Before the transcoder of `o' goes off (display lock held). */
void intel_audio_codec_disable(struct i915_device *i915, struct intel_output *o,
			       struct intel_pipe *p);
/* The firmware's pipe on transcoder `transcoder' is taken down: any audio
 * it left enabled there goes off with it. */
void intel_audio_release_transcoder(struct i915_device *i915, int transcoder);

#endif /* KERNEL_DEV_GPU_I915_INTEL_AUDIO_H */
