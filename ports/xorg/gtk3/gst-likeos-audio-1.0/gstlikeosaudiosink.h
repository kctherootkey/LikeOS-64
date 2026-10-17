/*
 * likeosaudiosink -- GStreamer audio output through the LikeOS audio
 * service (audiod), by way of libaudio.
 *
 * The element is a GstAudioSink: GStreamer's base classes keep the ring
 * buffer and the clock, and a dedicated thread hands this element one
 * segment (latency-time worth of frames, 10 ms by default) at a time through
 * write().  This element turns each segment into stream data for audiod,
 * which converts, resamples and mixes it with every other program's sound.
 *
 * Everything the element knows about the server stream lives in the
 * structure below.  Two locks guard it:
 *
 *   GST_OBJECT_LOCK   the user-visible settings that only matter when the
 *                     next stream is opened (client-name, stream-properties,
 *                     the tags seen on the pad, buffering, fallback);
 *   lock              the libaudio stream handle and everything derived from
 *                     it.  libaudio handles are not thread safe, and four
 *                     threads touch this one (the ring-buffer thread in
 *                     write(), the streaming thread in pause/stop/flush, the
 *                     clock in delay(), the application in the volume
 *                     setters), so EVERY la_*() call on the stream is made
 *                     with this lock held.  The writer never sleeps with it
 *                     held: it waits in poll() unlocked, and the other
 *                     threads interrupt that wait through `wake_fd'.
 *
 * The two locks are never nested.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef GST_LIKEOS_AUDIO_SINK_H
#define GST_LIKEOS_AUDIO_SINK_H

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiosink.h>

#include <audio/audio.h>

G_BEGIN_DECLS

#define GST_TYPE_LIKEOS_AUDIO_SINK (gst_likeos_audio_sink_get_type())
G_DECLARE_FINAL_TYPE(GstLikeosAudioSink, gst_likeos_audio_sink, GST,
		     LIKEOS_AUDIO_SINK, GstAudioSink)

/* Element defaults. */
#define LIKEOS_AUDIO_DEFAULT_BUFFER_TIME (200 * GST_MSECOND / GST_USECOND)
#define LIKEOS_AUDIO_DEFAULT_LATENCY_TIME (10 * GST_MSECOND / GST_USECOND)
#define LIKEOS_AUDIO_DEFAULT_VOLUME 1.0
#define LIKEOS_AUDIO_MAX_VOLUME 10.0
#define LIKEOS_AUDIO_DEFAULT_MUTE FALSE
#define LIKEOS_AUDIO_DEFAULT_SERVER_BUFFER 0 /* microseconds, 0 = auto */
#define LIKEOS_AUDIO_DEFAULT_NULL_FALLBACK TRUE

/* Format limits the caps advertise.  audiod itself takes 4000..384000 Hz,
 * but nothing a desktop plays lies outside this range and a narrower range
 * keeps upstream converters from picking an exotic rate. */
#define LIKEOS_AUDIO_MIN_RATE 8000
#define LIKEOS_AUDIO_MAX_RATE 192000
#define LIKEOS_AUDIO_MAX_CHANNELS 8

/* A message to post on the bus once the stream lock is released.  Posting
 * with the lock held is not safe: a synchronous bus handler (WebKit has one)
 * runs in the posting thread and may well change the element's state, which
 * would come straight back into pause() or stop() and that same lock. */
struct likeos_audio_pending_msg {
	gboolean error; /* FALSE: warning */
	gboolean set;
	gint code; /* GST_RESOURCE_ERROR_* */
	gchar *text; /* the user-facing part */
	gchar *debug; /* the detail */
};

struct _GstLikeosAudioSink {
	GstAudioSink parent;

	/* ---- settings, under GST_OBJECT_LOCK ------------------------------ */
	gchar *client_name; /* "client-name", NULL = derive */
	GstStructure *stream_props; /* "stream-properties", may be NULL */
	gchar *tag_title; /* GST_TAG_TITLE seen on the pad */
	gchar *tag_artist; /* GST_TAG_ARTIST seen on the pad */
	gint64 server_buffer_time; /* "server-buffer-time", us, 0 = auto */
	gboolean null_fallback; /* "null-fallback" */

	/* ---- the stream, under `lock' ------------------------------------- */
	GMutex lock;
	la_stream *stream; /* NULL while degraded or unprepared */
	la_format fmt; /* the negotiated format, for reconnects */
	guint32 role; /* AUD_ROLE_* chosen at prepare */
	guint32 server_buffer_ms; /* what we asked audiod to buffer */
	gchar app_name[AUDIOD_NAME_MAX];
	gchar stream_name[AUDIOD_NAME_MAX];
	guint bpf; /* bytes per frame */
	guint rate;
	gboolean prepared; /* between prepare() and unprepare() */
	gboolean paused; /* pause() seen, resume() not yet */
	guint discard_seq; /* bumped by stop/reset/flush: drop data */

	/* An error seen by a thread other than the writer.  Only the writer
	 * closes the stream -- it may be sitting in poll() on its descriptor --
	 * so the others just mark it and wake the writer. */
	gboolean broken;
	gint broken_err;

	/* Volume, as the GstStreamVolume interface sees it (linear). */
	gdouble volume;
	gboolean mute;

	/* ---- delay bookkeeping, under `lock' --------------------------------
	 *
	 * delay() is asked for constantly (every clock query), so it must not
	 * talk to the server.  The writer samples audiod's latency every few
	 * tens of milliseconds; between samples the queue is extrapolated:
	 *
	 *   queued(t) = lat_frames + (written - written_at_sample)
	 *               - (t - lat_time) * rate          (unless paused)
	 */
	gboolean have_sample;
	gint64 lat_frames; /* frames queued at lat_time */
	gint64 lat_time; /* monotonic us of the sample */
	guint64 written_frames; /* frames handed to libaudio, total */
	guint64 written_at_sample;
	guint written_rem; /* bytes of a partial frame */
	gint64 last_refresh; /* monotonic us of the last la_latency() */
	gint64 stream_since; /* when the current stream was opened */

	/* ---- degraded mode, under `lock' ------------------------------------
	 *
	 * Without a server stream (audiod gone, restarting, refusing the
	 * stream, or stalled) the element keeps consuming data at the
	 * stream's own rate, so a video keeps playing without sound instead of
	 * failing -- and it reconnects when the service is back. */
	gboolean degraded;
	gboolean failed; /* fallback off: an error was posted */
	gint64 null_epoch; /* monotonic us when null_frames was 0 */
	guint64 null_frames; /* frames "played" since null_epoch */
	guint null_rem; /* bytes of a partial frame */
	gint64 null_paused_at; /* when pause() froze the virtual clock */
	gint64 next_reconnect; /* monotonic us of the next attempt */
	gint64 reconnect_backoff; /* us, doubles per failure */
	guint64 reconnects; /* successful reconnects, for the log */
	guint poll_spins; /* POLLOUT wake-ups without progress */

	/* ---- the writer's wake-up pipe -------------------------------------- */
	gint wake_fd[2]; /* [0] read end, polled by write(); [1] written */
};

GST_ELEMENT_REGISTER_DECLARE(likeosaudiosink);

G_END_DECLS

#endif /* GST_LIKEOS_AUDIO_SINK_H */
