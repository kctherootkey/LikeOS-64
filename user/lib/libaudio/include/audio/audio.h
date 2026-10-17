// libaudio - the LikeOS audio client library.
//
// Programs play sound through the system audio service (audiod) with this
// library; they never open /dev/audioN themselves.  Two halves:
//
//   * streams:  la_open() a stream in any common format, la_write() frames
//               (blocking or not), la_drain(), la_close().  audiod converts,
//               resamples and mixes it with everything else that plays.
//   * control:  volume, mute, outputs (speaker/headphone/HDMI), the list of
//               playing streams, system event sounds, change notifications.
//
// Thread safety: one la_stream / la_ctl handle must not be used from two
// threads at once; different handles are independent.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_AUDIO_AUDIO_H
#define LIKEOS_AUDIO_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <audio/protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LA_VERSION_MAJOR 1
#define LA_VERSION_MINOR 0

/* Formats and roles are the protocol's (AUD_FMT_*, AUD_ROLE_*). */
typedef struct la_stream la_stream;
typedef struct la_ctl la_ctl;

typedef struct la_format {
	uint32_t format; /* AUD_FMT_* */
	uint32_t rate; /* Hz */
	uint32_t channels;
} la_format;

typedef struct la_stream_options {
	uint32_t role; /* AUD_ROLE_*, 0 = AUD_ROLE_OTHER */
	uint32_t buffer_ms; /* 0 = server default (about 100 ms) */
	uint32_t volume; /* 0..100, 0 means 100 unless flags say otherwise */
	uint32_t flags; /* AUD_STREAM_* | LA_OPT_* */
	const char *app_name; /* NULL = program name */
	const char *stream_name; /* NULL = app_name */
} la_stream_options;

#define LA_OPT_NONBLOCK (1u << 16) /* la_write returns short counts */
#define LA_OPT_VOLUME_ZERO (1u << 17) /* honour volume 0 */

/* ---- errors -------------------------------------------------------------- */

/* Functions return >= 0 on success and a negative errno on failure.
 * la_strerror() describes it, including the service's own message for the
 * last error on the handle when there is one. */
const char *la_strerror(int err);
/* The service's own message for the last error on a handle ("" if none). */
const char *la_stream_errmsg(la_stream *s);
const char *la_ctl_errmsg(la_ctl *c);

/* ---- streams --------------------------------------------------------------- */

/* Connect to audiod and open a playback stream.  Returns NULL and sets
 * *err (if non-NULL) on failure. */
la_stream *la_open(const la_format *fmt, const la_stream_options *opt,
		   int *err);
/* Simple form: role AUD_ROLE_OTHER, default buffering. */
la_stream *la_open_simple(uint32_t format, uint32_t rate, uint32_t channels,
			  const char *name, int *err);

/* Queue `bytes' of interleaved frames.  Blocks until all are accepted
 * unless the stream is non-blocking; returns the bytes taken or -errno. */
ssize_t la_write(la_stream *s, const void *data, size_t bytes);
/* Bytes that can be written now without blocking. */
ssize_t la_writable(la_stream *s);
/* Block until everything written has been heard. */
int la_drain(la_stream *s);
/* Discard everything buffered and not yet heard. */
int la_flush(la_stream *s);
int la_pause(la_stream *s);
int la_resume(la_stream *s);
/* Per-stream volume, 0..100 each side. */
int la_set_volume(la_stream *s, uint32_t left, uint32_t right);
int la_set_mute(la_stream *s, int mute);
/* Time until a frame written now is heard, and frames already heard. */
int la_latency(la_stream *s, uint64_t *latency_us, uint64_t *played_frames);
/* The descriptor to poll() for POLLOUT (room to write) when non-blocking;
 * call la_process() when it becomes readable to handle server messages. */
int la_fd(la_stream *s);
int la_process(la_stream *s);
uint32_t la_stream_id(la_stream *s);
uint32_t la_frame_bytes(const la_format *fmt);
void la_close(la_stream *s);

/* ---- control ------------------------------------------------------------- */

la_ctl *la_ctl_open(const char *app_name, int *err);
void la_ctl_close(la_ctl *c);
int la_ctl_fd(la_ctl *c);

int la_get_server_info(la_ctl *c, struct aud_server_info *info);
int la_get_volume(la_ctl *c, uint32_t *left, uint32_t *right);
int la_set_volume_master(la_ctl *c, uint32_t left, uint32_t right);
int la_get_mute(la_ctl *c, int *mute);
int la_set_mute_master(la_ctl *c, int mute);
int la_get_outputs(la_ctl *c, struct aud_outputs *out);
int la_set_output(la_ctl *c, uint32_t mode, uint32_t id);
int la_get_cards(la_ctl *c, struct aud_cards *cards);
int la_set_card(la_ctl *c, uint32_t index);

/* The streams playing now.  `max' entries at most; returns the count
 * stored.  la_list_streams(c, NULL, 0) returns how many there are, to size
 * the array. */
int la_list_streams(la_ctl *c, struct aud_stream_entry *out, int max);
int la_set_stream_volume(la_ctl *c, uint32_t stream_id, uint32_t left,
			 uint32_t right);
int la_kill_stream(la_ctl *c, uint32_t stream_id);

/* Play a system sound from /usr/share/sounds/likeos/<name>.wav.
 * `wait' blocks until it finished. */
int la_play_event(la_ctl *c, const char *name, uint32_t volume, int wait);
/* Convenience: open a control connection, play, close. */
int la_play_event_simple(const char *name, int wait);

/* Change notifications: subscribe, then poll la_ctl_fd() and read them. */
int la_subscribe(la_ctl *c, uint32_t mask);
/* Returns 1 and fills *n when a notification was waiting, 0 if none,
 * -errno on error.  Non-blocking. */
int la_next_notify(la_ctl *c, struct aud_notify *n);

/* ---- helpers --------------------------------------------------------------- */

/* Names for formats/roles/output types, for tools. */
const char *la_format_name(uint32_t fmt);
const char *la_role_name(uint32_t role);
int la_parse_format(const char *name, uint32_t *fmt);
int la_parse_role(const char *name, uint32_t *role);

#ifdef __cplusplus
}
#endif

#endif /* LIKEOS_AUDIO_AUDIO_H */
