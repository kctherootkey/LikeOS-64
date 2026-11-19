/*
 * likeosaudiosink -- play audio through the LikeOS audio service.
 *
 *   gst-launch-1.0 audiotestsrc ! audioconvert ! audioresample ! likeosaudiosink
 *
 * The element registers at rank PRIMARY + 16, so autoaudiosink -- which is
 * what playbin and WebKit's media player use -- picks it ahead of every
 * other sink.  autoaudiosink tries each candidate by setting it to READY and
 * takes the first that succeeds; open() therefore connects to audiod and
 * fails when nobody answers.  autoaudiosink then falls back to a fakesink,
 * and a page with a video still plays (silently) instead of hitting WebKit's
 * assertion that it got an audio sink.
 *
 * Life cycle, as GstAudioSink drives it:
 *
 *   open()       NULL->READY      is audiod there?  (control connection,
 *                                 opened and closed again)
 *   prepare()    caps known       la_open() a non-blocking stream with the
 *                                 negotiated format, role and names
 *   write()      ring thread      hand one segment to the stream; blocks
 *                                 until audiod has room, interruptibly
 *   delay()      clock queries    frames queued in audiod and the device
 *   pause()      PLAYING->PAUSED  la_pause(): the server holds the data
 *   resume()     PAUSED->PLAYING  la_resume()
 *   stop/reset   flushing, READY  la_flush(), drop what write() holds
 *   clear_all    FLUSH_STOP       la_flush() (seeks)
 *   unprepare()  caps change, READY  la_close()
 *   close()      READY->NULL
 *
 * Why write() does not simply call a blocking la_write(): GstAudioSink
 * requires pause() and stop() -- called from other threads -- to unblock a
 * write in progress, and a libaudio handle must never be used by two
 * threads at once.  So the stream is opened non-blocking, every la_*() call
 * happens under the element's stream lock, and write() waits for room in
 * poll() with that lock dropped, on the stream's descriptor plus a wake-up
 * pipe the other threads write to.
 *
 * When the service goes away mid-stream (audiod is restarted by init, for
 * one), or stops taking data for several seconds, the element does not fail
 * the pipeline: it keeps consuming data in real time, so the clock and any
 * video keep running, posts a warning, and reconnects with backoff.  The
 * "null-fallback" property turns that into a hard error instead.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gst/audio/streamvolume.h>

#include "gstlikeosaudiosink.h"

GST_DEBUG_CATEGORY_STATIC(likeos_audio_sink_debug);
#define GST_CAT_DEFAULT likeos_audio_sink_debug

/* ---- tunables ------------------------------------------------------------ */

/* How long write() sleeps in one poll() before it re-checks everything.
 * Wake-ups normally come from the server (room to write) or the wake-up pipe
 * (pause, stop, flush); this is only the safety net. */
#define WAIT_SLICE_MS 20

/* How often the writer asks audiod for the stream's latency. */
#define LATENCY_REFRESH_US (40 * G_USEC_PER_SEC / 1000)

/* A new latency sample closer than this to the extrapolated value is blended
 * in rather than taken outright, so the clock does not jitter with the
 * request's own scheduling noise. */
#define LATENCY_SMOOTH_US (30 * G_USEC_PER_SEC / 1000)

/* Latency replies above this are treated as bogus. */
#define LATENCY_SANE_MAX_US (10 * G_USEC_PER_SEC)

/* No progress on a stream that is not paused for this long: the service is
 * wedged.  Degrade rather than freeze the pipeline. */
#define STALL_US (4 * G_USEC_PER_SEC)

/* Reconnect backoff, doubled per failed or wedged connection and reset once
 * a stream has played for STABLE_US. */
#define RECONNECT_MIN_US (1 * G_USEC_PER_SEC)
#define RECONNECT_MAX_US (60 * G_USEC_PER_SEC)
#define STABLE_US (10 * G_USEC_PER_SEC)

/* POLLOUT wake-ups without progress before write() stops asking for POLLOUT
 * and waits for a server message instead.  Guards against a library that
 * reports writable room while the socket is the only thing ready. */
#define MAX_POLL_SPINS 8

/* Server-side buffering when "server-buffer-time" is 0: half the ring
 * buffer, within these bounds. */
#define AUTO_SERVER_BUFFER_MIN_MS 40
#define AUTO_SERVER_BUFFER_MAX_MS 150
#define SERVER_BUFFER_MIN_MS 10
#define SERVER_BUFFER_MAX_MS 2000

/* ---- properties ---------------------------------------------------------- */

enum {
	PROP_0,
	PROP_VOLUME,
	PROP_MUTE,
	PROP_CLIENT_NAME,
	PROP_STREAM_PROPERTIES,
	PROP_SERVER_BUFFER_TIME,
	PROP_NULL_FALLBACK,
	PROP_CONNECTED,
	PROP_ROLE,
};

/* ---- caps ---------------------------------------------------------------- */

/* Every sample format audiod converts from.  S16LE first: it is what the
 * mixer runs at, so it costs nothing to convert, and audioconvert's own
 * fixation still prefers whatever loses nothing from the source. */
#define LIKEOS_FORMATS \
	"{ S16LE, F32LE, S32LE, S24_32LE, S24LE, U8, S16BE, F64LE }"

/* Any channel layout of 1..8 channels.  audiod takes channel COUNTS and
 * treats them in the usual WAVE/SMPTE order (FL FR FC LFE, then the rear or
 * side pairs, then the rest); GStreamer interleaves in its own canonical
 * order, which puts every common layout's channels at exactly those
 * positions:
 *
 *   1  mono            5  FL FR FC RL RR      7  FL FR FC LFE RC SL SR
 *   2  FL FR           6  FL FR FC LFE RL RR  8  FL FR FC LFE RL RR SL SR
 *   3  FL FR FC           or FL FR FC LFE SL SR (5.1 with side surrounds)
 *   4  FL FR RL RR
 *
 * No channel-mask is fixed in the caps on purpose.  With one fixed mask per
 * count, a source in any OTHER layout of the same count -- 5.1 with side
 * instead of rear surrounds, which is what the FFmpeg decoders produce --
 * intersects with none of them, and audioconvert then falls back to the
 * template's first structure: it downmixed 5.1 films to mono.  Side and
 * rear surrounds sit at the same interleaved positions, so passing the
 * layout through is the correct thing to do. */
#define LIKEOS_SINK_CAPS                                        \
	"audio/x-raw, "                                         \
	"format = (string) " LIKEOS_FORMATS ", "                \
	"layout = (string) interleaved, "                       \
	"rate = (int) [ 8000, 192000 ], "                       \
	"channels = (int) [ 1, 8 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
	"sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(LIKEOS_SINK_CAPS));

/* The layout audiod assumes for each channel count, as GStreamer channel
 * masks; only used to say in the log when a stream's layout differs (it
 * is still played -- see above). */
static const guint64 expected_mask[LIKEOS_AUDIO_MAX_CHANNELS + 1] = {
	0,
	0, /* mono */
	0x3, /* FL FR */
	0x7, /* FL FR FC */
	0x33, /* FL FR RL RR */
	0x37, /* FL FR FC RL RR */
	0x3f, /* FL FR FC LFE RL RR */
	0xf0f, /* FL FR FC LFE RC SL SR */
	0xc3f, /* FL FR FC LFE RL RR SL SR */
};

/* ---- format and role tables ---------------------------------------------- */

static const struct {
	GstAudioFormat gst;
	guint32 aud;
} format_map[] = {
	{ GST_AUDIO_FORMAT_S16LE, AUD_FMT_S16_LE },
	{ GST_AUDIO_FORMAT_F32LE, AUD_FMT_F32_LE },
	{ GST_AUDIO_FORMAT_S32LE, AUD_FMT_S32_LE },
	/* 24 significant bits in the low part of 32 */
	{ GST_AUDIO_FORMAT_S24_32LE, AUD_FMT_S24_LE },
	/* 24 bits packed in 3 bytes */
	{ GST_AUDIO_FORMAT_S24LE, AUD_FMT_S24_3LE },
	{ GST_AUDIO_FORMAT_U8, AUD_FMT_U8 },
	{ GST_AUDIO_FORMAT_S16BE, AUD_FMT_S16_BE },
	{ GST_AUDIO_FORMAT_F64LE, AUD_FMT_F64_LE },
};

/* "media.role" values, as GStreamer elements and WebKit spell them.  WebKit
 * sends "video" or "music" for media elements, "webaudio" for the Web Audio
 * API, "speech" for speech synthesis and "dtmf" for WebRTC tones. */
static const struct {
	const char *name;
	guint32 role;
} role_map[] = {
	{ "video", AUD_ROLE_VIDEO },
	{ "movie", AUD_ROLE_VIDEO },
	{ "music", AUD_ROLE_MUSIC },
	{ "production", AUD_ROLE_MUSIC },
	{ "game", AUD_ROLE_GAME },
	{ "event", AUD_ROLE_EVENT },
	{ "notification", AUD_ROLE_EVENT },
	{ "alarm", AUD_ROLE_EVENT },
	{ "animation", AUD_ROLE_EVENT },
	{ "phone", AUD_ROLE_PHONE },
	{ "voip", AUD_ROLE_PHONE },
	{ "communication", AUD_ROLE_PHONE },
	{ "dtmf", AUD_ROLE_PHONE },
	{ "webaudio", AUD_ROLE_WEB },
	{ "web", AUD_ROLE_WEB },
	{ "speech", AUD_ROLE_OTHER },
	{ "a11y", AUD_ROLE_OTHER },
	{ "test", AUD_ROLE_OTHER },
	{ "other", AUD_ROLE_OTHER },
};

static guint32 map_format(GstAudioFormat f)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(format_map); i++)
		if (format_map[i].gst == f)
			return format_map[i].aud;
	return 0;
}

static guint32 map_role(const gchar *name)
{
	guint i;

	if (name == NULL || *name == '\0')
		return AUD_ROLE_OTHER;
	for (i = 0; i < G_N_ELEMENTS(role_map); i++)
		if (g_ascii_strcasecmp(name, role_map[i].name) == 0)
			return role_map[i].role;
	return AUD_ROLE_OTHER;
}

static const gchar *role_label(guint32 role)
{
	switch (role) {
	case AUD_ROLE_MUSIC:
		return "music";
	case AUD_ROLE_VIDEO:
		return "video";
	case AUD_ROLE_GAME:
		return "game";
	case AUD_ROLE_EVENT:
		return "event";
	case AUD_ROLE_PHONE:
		return "phone";
	case AUD_ROLE_WEB:
		return "webaudio";
	default:
		return "other";
	}
}

/* ---- small helpers --------------------------------------------------------- */

/* Copy a name into a fixed protocol field: valid UTF-8, no control
 * characters (they end up in audiod's log and `audioctl streams'), cut at a
 * character boundary.  Returns FALSE when nothing usable was left. */
static gboolean copy_name(gchar *dst, gsize size, const gchar *src)
{
	gchar *valid, *p;
	gsize len;

	dst[0] = '\0';
	if (src == NULL || size < 2)
		return FALSE;

	valid = g_utf8_make_valid(src, -1);
	for (p = valid; *p; p++) {
		guchar c = (guchar)*p;

		if (c < 0x20 || c == 0x7f)
			*p = ' ';
	}
	g_strstrip(valid);

	len = strlen(valid);
	if (len >= size) {
		/* At most size - 1 bytes fit.  Find the character holding the
		 * last byte that fits; if it runs past the limit, cut before
		 * it, so only whole characters are kept. */
		const gchar *limit = valid + size - 1;
		const gchar *start = g_utf8_find_prev_char(valid, limit);

		if (start == NULL)
			start = valid;
		if (start + g_utf8_skip[*(const guchar *)start] > limit)
			limit = start;
		len = (gsize)(limit - valid);
	}
	memcpy(dst, valid, len);
	dst[len] = '\0';
	g_free(valid);

	/* a cut can leave trailing blanks */
	while (len > 0 && dst[len - 1] == ' ')
		dst[--len] = '\0';
	return len > 0;
}

static gint64 frames_to_us(guint64 frames, guint rate)
{
	if (rate == 0)
		return 0;
	return (gint64)gst_util_uint64_scale_int(frames, G_USEC_PER_SEC, rate);
}

static gint64 us_to_frames(gint64 us, guint rate)
{
	if (us <= 0 || rate == 0)
		return 0;
	return (gint64)gst_util_uint64_scale_int((guint64)us, rate,
						 G_USEC_PER_SEC);
}

static guint32 volume_to_percent(gdouble v)
{
	/* audiod's stream volume is 0..100 with 100 as unity; the stream
	 * volume interface allows amplification up to 10x, which the service
	 * cannot do, so anything above unity plays at unity. */
	if (!(v > 0.0))
		return 0;
	if (v >= 1.0)
		return 100;
	return (guint32)(v * 100.0 + 0.5);
}

static gboolean is_retry_error(gssize err)
{
	return err == -EAGAIN || err == -EWOULDBLOCK || err == -EINTR;
}

static void pending_set(struct likeos_audio_pending_msg *m, gboolean error,
			gint code, const gchar *text, gchar *debug)
{
	/* Keep the first message of a call: it is the cause, later ones are
	 * consequences. */
	if (m->set) {
		g_free(debug);
		return;
	}
	m->set = TRUE;
	m->error = error;
	m->code = code;
	m->text = g_strdup(text);
	m->debug = debug;
}

static void pending_post(GstLikeosAudioSink *self,
			 struct likeos_audio_pending_msg *m)
{
	if (!m->set)
		return;
	if (m->error) {
		gst_element_message_full(GST_ELEMENT(self), GST_MESSAGE_ERROR,
					 GST_RESOURCE_ERROR, m->code, m->text,
					 m->debug, __FILE__, GST_FUNCTION,
					 __LINE__);
	} else {
		gst_element_message_full(GST_ELEMENT(self),
					 GST_MESSAGE_WARNING,
					 GST_RESOURCE_ERROR, m->code, m->text,
					 m->debug, __FILE__, GST_FUNCTION,
					 __LINE__);
	}
	/* gst_element_message_full() took both strings */
	m->text = NULL;
	m->debug = NULL;
	m->set = FALSE;
}

/* ---- the wake-up pipe ------------------------------------------------------ */

static gboolean wake_open(GstLikeosAudioSink *self, GError **error)
{
	gint fds[2];

	if (self->wake_fd[0] >= 0)
		return TRUE;
	if (!g_unix_open_pipe(fds, FD_CLOEXEC, error))
		return FALSE;
	if (!g_unix_set_fd_nonblocking(fds[0], TRUE, error) ||
	    !g_unix_set_fd_nonblocking(fds[1], TRUE, error)) {
		close(fds[0]);
		close(fds[1]);
		return FALSE;
	}
	self->wake_fd[0] = fds[0];
	self->wake_fd[1] = fds[1];
	return TRUE;
}

static void wake_close(GstLikeosAudioSink *self)
{
	if (self->wake_fd[0] >= 0)
		close(self->wake_fd[0]);
	if (self->wake_fd[1] >= 0)
		close(self->wake_fd[1]);
	self->wake_fd[0] = -1;
	self->wake_fd[1] = -1;
}

/* Interrupt the writer's poll().  Called after the state change it must
 * notice has been made under the lock, so a writer that checks the state
 * after draining the pipe cannot miss it.  A full pipe already holds a
 * wake-up, so EAGAIN is fine. */
static void wake_writer(GstLikeosAudioSink *self)
{
	const gchar b = 'w';
	gssize r;

	if (self->wake_fd[1] < 0)
		return;
	do {
		r = write(self->wake_fd[1], &b, 1);
	} while (r < 0 && errno == EINTR);
}

static void wake_drain(GstLikeosAudioSink *self)
{
	gchar buf[64];
	gssize r;

	if (self->wake_fd[0] < 0)
		return;
	do {
		r = read(self->wake_fd[0], buf, sizeof(buf));
	} while (r > 0 || (r < 0 && errno == EINTR));
}

/* Sleep up to `ms' or until woken.  Called WITHOUT the stream lock. */
static void wake_sleep(GstLikeosAudioSink *self, gint ms)
{
	struct pollfd p;

	if (self->wake_fd[0] < 0) {
		g_usleep((gulong)ms * 1000);
		return;
	}
	p.fd = self->wake_fd[0];
	p.events = POLLIN;
	p.revents = 0;
	if (poll(&p, 1, ms) > 0)
		wake_drain(self);
}

/* ---- GObject boilerplate --------------------------------------------------- */

#define gst_likeos_audio_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstLikeosAudioSink, gst_likeos_audio_sink,
			GST_TYPE_AUDIO_SINK,
			G_IMPLEMENT_INTERFACE(GST_TYPE_STREAM_VOLUME, NULL);
			GST_DEBUG_CATEGORY_INIT(likeos_audio_sink_debug,
						"likeosaudiosink", 0,
						"LikeOS audio sink"));

GST_ELEMENT_REGISTER_DEFINE(likeosaudiosink, "likeosaudiosink",
			    GST_RANK_PRIMARY + 16, GST_TYPE_LIKEOS_AUDIO_SINK);

static void gst_likeos_audio_sink_finalize(GObject *object);
static void gst_likeos_audio_sink_set_property(GObject *object, guint prop_id,
					       const GValue *value,
					       GParamSpec *pspec);
static void gst_likeos_audio_sink_get_property(GObject *object, guint prop_id,
					       GValue *value,
					       GParamSpec *pspec);
static gboolean gst_likeos_audio_sink_event(GstBaseSink *bsink,
					    GstEvent *event);
static gboolean gst_likeos_audio_sink_open(GstAudioSink *asink);
static gboolean gst_likeos_audio_sink_prepare(GstAudioSink *asink,
					      GstAudioRingBufferSpec *spec);
static gboolean gst_likeos_audio_sink_unprepare(GstAudioSink *asink);
static gboolean gst_likeos_audio_sink_close(GstAudioSink *asink);
static gint gst_likeos_audio_sink_write(GstAudioSink *asink, gpointer data,
					guint length);
static guint gst_likeos_audio_sink_delay(GstAudioSink *asink);
static void gst_likeos_audio_sink_reset(GstAudioSink *asink);
static void gst_likeos_audio_sink_pause(GstAudioSink *asink);
static void gst_likeos_audio_sink_resume(GstAudioSink *asink);
static void gst_likeos_audio_sink_stop(GstAudioSink *asink);
static void gst_likeos_audio_sink_clear_all(GstAudioSink *asink);

static void
gst_likeos_audio_sink_class_init(GstLikeosAudioSinkClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
	GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS(klass);
	GstAudioSinkClass *audiosink_class = GST_AUDIO_SINK_CLASS(klass);

	gobject_class->finalize = gst_likeos_audio_sink_finalize;
	gobject_class->set_property = gst_likeos_audio_sink_set_property;
	gobject_class->get_property = gst_likeos_audio_sink_get_property;

	/* The GstStreamVolume interface is nothing but these two property
	 * names; playbin and WebKit drive the sink's own volume through it
	 * instead of inserting a volume element. */
	g_object_class_install_property(
		gobject_class, PROP_VOLUME,
		g_param_spec_double(
			"volume", "Volume",
			"Linear volume of this stream, 1.0 = 100% "
			"(the service does not amplify above 1.0)",
			0.0, LIKEOS_AUDIO_MAX_VOLUME,
			LIKEOS_AUDIO_DEFAULT_VOLUME,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
				GST_PARAM_MUTABLE_PLAYING));

	g_object_class_install_property(
		gobject_class, PROP_MUTE,
		g_param_spec_boolean("mute", "Mute", "Mute this stream",
				     LIKEOS_AUDIO_DEFAULT_MUTE,
				     G_PARAM_READWRITE |
					     G_PARAM_STATIC_STRINGS |
					     GST_PARAM_MUTABLE_PLAYING));

	/* The two names WebKit (and other applications written for sound
	 * servers) look for with g_object_class_find_property() on whatever
	 * autoaudiosink picked. */
	g_object_class_install_property(
		gobject_class, PROP_CLIENT_NAME,
		g_param_spec_string(
			"client-name", "Client name",
			"Application name shown by the audio service "
			"(default: the program's name; used from the next "
			"stream on)",
			NULL,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
				GST_PARAM_MUTABLE_READY));

	g_object_class_install_property(
		gobject_class, PROP_STREAM_PROPERTIES,
		g_param_spec_boxed(
			"stream-properties", "Stream properties",
			"Properties of the stream: media.role (video, music, "
			"game, event, phone, webaudio, ...), media.name, "
			"media.title, media.artist, application.name",
			GST_TYPE_STRUCTURE,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
				GST_PARAM_MUTABLE_READY));

	g_object_class_install_property(
		gobject_class, PROP_SERVER_BUFFER_TIME,
		g_param_spec_int64(
			"server-buffer-time", "Server buffer time",
			"Audio the service buffers for this stream, in "
			"microseconds (0 = half of buffer-time, 40..150 ms)",
			0, (gint64)SERVER_BUFFER_MAX_MS * 1000,
			LIKEOS_AUDIO_DEFAULT_SERVER_BUFFER,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
				GST_PARAM_MUTABLE_READY));

	g_object_class_install_property(
		gobject_class, PROP_NULL_FALLBACK,
		g_param_spec_boolean(
			"null-fallback", "Fall back to silence",
			"When the audio service is lost or refuses the "
			"stream, keep playing in real time without sound and "
			"reconnect, instead of failing with an error",
			LIKEOS_AUDIO_DEFAULT_NULL_FALLBACK,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
				GST_PARAM_MUTABLE_PLAYING));

	g_object_class_install_property(
		gobject_class, PROP_CONNECTED,
		g_param_spec_boolean(
			"connected", "Connected",
			"Whether a stream to the audio service is open now",
			FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(
		gobject_class, PROP_ROLE,
		g_param_spec_string(
			"role", "Role",
			"The stream role in use, from stream-properties' "
			"media.role",
			NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	gst_element_class_set_static_metadata(
		element_class, "LikeOS audio sink", "Sink/Audio",
		"Plays audio through the LikeOS audio service (audiod), "
		"mixed with every other program's sound",
		"The LikeOS Project");

	gst_element_class_add_static_pad_template(element_class,
						  &sink_template);

	basesink_class->event = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_event);

	audiosink_class->open = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_open);
	audiosink_class->prepare =
		GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_prepare);
	audiosink_class->unprepare =
		GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_unprepare);
	audiosink_class->close = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_close);
	audiosink_class->write = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_write);
	audiosink_class->delay = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_delay);
	audiosink_class->reset = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_reset);
	audiosink_class->pause = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_pause);
	audiosink_class->resume =
		GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_resume);
	audiosink_class->stop = GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_stop);

	/* clear_all lives in a class extension that GstAudioSink keeps in its
	 * class-private area.  The `extension' pointer a subclass inherits
	 * still points at GstAudioSink's OWN copy (the class structure is
	 * copied from the parent, pointer and all), so writing through it
	 * would install this element's hook into every other GstAudioSink in
	 * the process.  Point it at this class's copy of the private area
	 * first, then fill that in. */
	audiosink_class->extension = G_TYPE_CLASS_GET_PRIVATE(
		klass, GST_TYPE_AUDIO_SINK, GstAudioSinkClassExtension);
	audiosink_class->extension->clear_all =
		GST_DEBUG_FUNCPTR(gst_likeos_audio_sink_clear_all);
}

static void gst_likeos_audio_sink_init(GstLikeosAudioSink *self)
{
	GstAudioBaseSink *base = GST_AUDIO_BASE_SINK(self);

	g_mutex_init(&self->lock);
	self->wake_fd[0] = -1;
	self->wake_fd[1] = -1;
	self->volume = LIKEOS_AUDIO_DEFAULT_VOLUME;
	self->mute = LIKEOS_AUDIO_DEFAULT_MUTE;
	self->server_buffer_time = LIKEOS_AUDIO_DEFAULT_SERVER_BUFFER;
	self->null_fallback = LIKEOS_AUDIO_DEFAULT_NULL_FALLBACK;
	self->role = AUD_ROLE_OTHER;
	self->reconnect_backoff = RECONNECT_MIN_US;

	/* 200 ms of ring buffer in 10 ms segments.  These are GstAudioBaseSink's
	 * own defaults today; they are set explicitly because the server-side
	 * buffering below is derived from them and must not change under it if
	 * the base class's defaults ever do. */
	base->buffer_time = LIKEOS_AUDIO_DEFAULT_BUFFER_TIME;
	base->latency_time = LIKEOS_AUDIO_DEFAULT_LATENCY_TIME;
}

static void gst_likeos_audio_sink_finalize(GObject *object)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(object);

	/* unprepare() and close() ran unless the element never left NULL;
	 * either way nothing else can hold a reference now. */
	if (self->stream != NULL) {
		la_close(self->stream);
		self->stream = NULL;
	}
	wake_close(self);

	g_free(self->client_name);
	g_free(self->tag_title);
	g_free(self->tag_artist);
	if (self->stream_props != NULL)
		gst_structure_free(self->stream_props);
	g_mutex_clear(&self->lock);

	G_OBJECT_CLASS(parent_class)->finalize(object);
}

/* ---- delay bookkeeping (all with `lock' held) ------------------------------- */

/* Frames queued in the server and device now, unclamped (negative means
 * the extrapolation ran past an underrun). */
static gint64 stream_queue_raw(GstLikeosAudioSink *self, gint64 now)
{
	gint64 q;

	if (!self->have_sample)
		return (gint64)(self->written_frames - self->written_at_sample);
	q = self->lat_frames +
	    (gint64)(self->written_frames - self->written_at_sample);
	if (!self->paused)
		q -= us_to_frames(now - self->lat_time, self->rate);
	return q;
}

/* The same for degraded mode: frames the virtual device has not "played". */
static gint64 null_queue_raw(GstLikeosAudioSink *self, gint64 now)
{
	gint64 at = self->paused ? self->null_paused_at : now;
	gint64 elapsed = at - self->null_epoch;

	return (gint64)self->null_frames - us_to_frames(elapsed, self->rate);
}

static gint64 current_queue(GstLikeosAudioSink *self, gint64 now)
{
	gint64 q;

	if (!self->prepared)
		return 0;
	if (self->stream == NULL)
		q = self->degraded ? null_queue_raw(self, now) : 0;
	else
		q = stream_queue_raw(self, now);
	return q > 0 ? q : 0;
}

/* Restart the extrapolation from `frames' queued at `now'. */
static void rebase_sample(GstLikeosAudioSink *self, gint64 frames, gint64 now)
{
	self->have_sample = TRUE;
	self->lat_frames = frames > 0 ? frames : 0;
	self->lat_time = now;
	self->written_at_sample = self->written_frames;
}

/* Account `bytes' handed to the stream. */
static void account_written(GstLikeosAudioSink *self, gsize bytes,
			    gint64 now)
{
	guint64 total;

	/* If the extrapolated queue ran dry the server underran; what we
	 * write now starts a fresh queue, not one that is "owed" playback
	 * time from before the gap. */
	if (self->have_sample && !self->paused &&
	    stream_queue_raw(self, now) < 0)
		rebase_sample(self, 0, now);

	total = (guint64)self->written_rem + bytes;
	self->written_frames += total / self->bpf;
	self->written_rem = (guint)(total % self->bpf);
}

/* ---- stream management (all with `lock' held) ------------------------------- */

static void apply_volume(GstLikeosAudioSink *self)
{
	guint32 v = volume_to_percent(self->volume);
	gint r;

	if (self->stream == NULL || self->broken)
		return;
	r = la_set_volume(self->stream, v, v);
	if (r < 0 && !is_retry_error(r)) {
		GST_WARNING_OBJECT(self, "setting the volume failed: %s",
				   la_strerror(r));
		self->broken = TRUE;
		self->broken_err = r;
		return;
	}
	r = la_set_mute(self->stream, self->mute ? 1 : 0);
	if (r < 0 && !is_retry_error(r)) {
		GST_WARNING_OBJECT(self, "setting mute failed: %s",
				   la_strerror(r));
		self->broken = TRUE;
		self->broken_err = r;
	}
}

/* Open the server stream with the parameters prepare() settled.  Returns 0
 * or a negative errno. */
static gint open_stream(GstLikeosAudioSink *self)
{
	la_stream_options opt;
	la_stream *s;
	gint err = 0;
	guint32 v = volume_to_percent(self->volume);

	memset(&opt, 0, sizeof(opt));
	opt.role = self->role;
	opt.buffer_ms = self->server_buffer_ms;
	opt.volume = v;
	opt.flags = LA_OPT_NONBLOCK;
	if (v == 0)
		opt.flags |= LA_OPT_VOLUME_ZERO;
	/* Start paused when the pipeline is: a reconnect during a pause must
	 * not start playing what is written afterwards. */
	if (self->paused)
		opt.flags |= AUD_STREAM_START_PAUSED;
	opt.app_name = self->app_name[0] ? self->app_name : NULL;
	opt.stream_name = self->stream_name[0] ? self->stream_name : NULL;

	s = la_open(&self->fmt, &opt, &err);
	if (s == NULL)
		return err < 0 ? err : -ECONNREFUSED;

	self->stream = s;
	self->broken = FALSE;
	self->broken_err = 0;
	self->poll_spins = 0;
	self->stream_since = g_get_monotonic_time();
	self->last_refresh = 0;

	/* Mute is not part of the open request. */
	if (self->mute) {
		gint r = la_set_mute(s, 1);

		if (r < 0 && !is_retry_error(r)) {
			self->broken = TRUE;
			self->broken_err = r;
		}
	}

	GST_INFO_OBJECT(self,
			"stream %u open: %s %u Hz %u ch, role %s, "
			"server buffer %u ms, app \"%s\", name \"%s\"",
			la_stream_id(s), la_format_name(self->fmt.format),
			self->fmt.rate, self->fmt.channels,
			role_label(self->role), self->server_buffer_ms,
			self->app_name, self->stream_name);
	return 0;
}

/* Close the server stream and go on in degraded mode (or fail, when the
 * fallback is off).  `err' and `what' say why, for the message. */
static void lose_stream(GstLikeosAudioSink *self, gint err, const gchar *what,
			struct likeos_audio_pending_msg *msg)
{
	gint64 now = g_get_monotonic_time();
	gint64 queued = current_queue(self, now);
	gboolean fallback;

	if (self->stream != NULL) {
		la_close(self->stream);
		self->stream = NULL;
	}
	self->broken = FALSE;
	self->broken_err = 0;

	/* The virtual device carries on from where the real one was, so the
	 * clock does not jump: what was queued "plays out" in real time. */
	self->degraded = TRUE;
	self->null_epoch = now;
	self->null_frames = (guint64)queued;
	self->null_rem = 0;
	self->null_paused_at = now;
	self->have_sample = FALSE;

	/* Back off further each time: a service that accepts and then wedges
	 * would otherwise cost a multi-second stall per attempt. */
	self->next_reconnect = now + self->reconnect_backoff;
	self->reconnect_backoff =
		MIN(self->reconnect_backoff * 2, (gint64)RECONNECT_MAX_US);

	GST_OBJECT_LOCK(self);
	fallback = self->null_fallback;
	GST_OBJECT_UNLOCK(self);

	if (fallback) {
		GST_WARNING_OBJECT(self,
				   "audio service lost (%s: %s); playing "
				   "silently, next reconnect in %" G_GINT64_FORMAT
				   " ms",
				   what, la_strerror(err),
				   (self->next_reconnect - now) / 1000);
		pending_set(msg, FALSE, GST_RESOURCE_ERROR_WRITE,
			    "Lost the connection to the audio service; "
			    "continuing without sound",
			    g_strdup_printf("%s: %s", what, la_strerror(err)));
	} else {
		self->failed = TRUE;
		pending_set(msg, TRUE, GST_RESOURCE_ERROR_WRITE,
			    "Lost the connection to the audio service",
			    g_strdup_printf("%s: %s", what, la_strerror(err)));
	}
}

/* Degraded mode: try to get a stream back.  TRUE when one is open again. */
static gboolean try_reconnect(GstLikeosAudioSink *self, gint64 now)
{
	gint64 queued;
	gint r;

	if (self->failed || self->paused || now < self->next_reconnect)
		return FALSE;

	queued = null_queue_raw(self, now);
	r = open_stream(self);
	if (r < 0) {
		GST_DEBUG_OBJECT(self, "reconnect failed: %s", la_strerror(r));
		self->next_reconnect = now + self->reconnect_backoff;
		self->reconnect_backoff = MIN(self->reconnect_backoff * 2,
					      (gint64)RECONNECT_MAX_US);
		return FALSE;
	}

	self->degraded = FALSE;
	self->reconnects++;
	/* The new server queue starts empty, but the virtual one may still
	 * have had a few milliseconds in it: start the extrapolation there
	 * and let the next latency samples pull it to the truth. */
	rebase_sample(self, queued, now);
	GST_INFO_OBJECT(self, "audio service back (reconnect %" G_GUINT64_FORMAT
			      ")", self->reconnects);
	return TRUE;
}

/* Handle whatever the server sent.  Only with the descriptor actually
 * readable: the check is repeated under the lock because another thread
 * (a volume change, say) may have consumed the data that woke the writer,
 * and la_process() must not be left to block on an empty socket. */
static gint service_stream(GstLikeosAudioSink *self,
			   struct likeos_audio_pending_msg *msg)
{
	struct pollfd p;
	gint fd, r;

	if (self->stream == NULL)
		return 0;
	fd = la_fd(self->stream);
	if (fd < 0) {
		lose_stream(self, -EBADF, "la_fd", msg);
		return -EBADF;
	}
	p.fd = fd;
	p.events = POLLIN;
	p.revents = 0;
	do {
		r = poll(&p, 1, 0);
	} while (r < 0 && errno == EINTR);
	if (r <= 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
		return 0;

	r = la_process(self->stream);
	if (r < 0 && !is_retry_error(r)) {
		lose_stream(self, r, "la_process", msg);
		return r;
	}
	return 0;
}

/* Sample the server's latency for this stream (writer thread only). */
static void refresh_latency(GstLikeosAudioSink *self, gint64 now,
			    struct likeos_audio_pending_msg *msg)
{
	guint64 lat_us = 0, played = 0;
	gint64 fresh, t;
	gint r;

	if (self->stream == NULL || self->paused)
		return;
	if (self->last_refresh != 0 &&
	    now - self->last_refresh < LATENCY_REFRESH_US)
		return;
	self->last_refresh = now;

	r = la_latency(self->stream, &lat_us, &played);
	if (r < 0) {
		if (!is_retry_error(r))
			lose_stream(self, r, "la_latency", msg);
		return;
	}
	if (lat_us > LATENCY_SANE_MAX_US) {
		GST_DEBUG_OBJECT(self, "ignoring latency %" G_GUINT64_FORMAT
				       " us", lat_us);
		return;
	}

	t = g_get_monotonic_time();
	fresh = us_to_frames((gint64)lat_us, self->rate);
	if (self->have_sample) {
		gint64 old = stream_queue_raw(self, t);
		gint64 diff = fresh - old;
		gint64 window = us_to_frames(LATENCY_SMOOTH_US, self->rate);

		/* Close to what we expected: move a quarter of the way.  Far
		 * off (underrun, flush): take it as it is. */
		if (old >= 0 && diff > -window && diff < window)
			fresh = old + diff / 4;
	}
	rebase_sample(self, fresh, t);

	/* A stream that has been playing for a while resets the backoff. */
	if (t - self->stream_since > STABLE_US)
		self->reconnect_backoff = RECONNECT_MIN_US;

	GST_LOG_OBJECT(self,
		       "latency %" G_GUINT64_FORMAT " us, played %"
		       G_GUINT64_FORMAT ", queue %" G_GINT64_FORMAT " frames",
		       lat_us, played, self->lat_frames);
}

/* Is the ring buffer really running?  write() may be called a last time
 * after pause() flipped it, and must then return rather than wait. */
static gboolean ring_started(GstLikeosAudioSink *self)
{
	GstAudioRingBuffer *rb = GST_AUDIO_BASE_SINK(self)->ringbuffer;

	if (rb == NULL)
		return FALSE;
	return g_atomic_int_get(&rb->state) ==
	       GST_AUDIO_RING_BUFFER_STATE_STARTED;
}

/* Wait for room in the stream.  Entered and left with `lock' held; drops it
 * for the poll() itself. */
static void wait_for_room(GstLikeosAudioSink *self,
			  struct likeos_audio_pending_msg *msg)
{
	struct pollfd p[2];
	la_stream *s = self->stream;
	gssize room;
	gint fd, r;
	gboolean want_out = FALSE;

	fd = la_fd(s);
	if (fd < 0) {
		lose_stream(self, -EBADF, "la_fd", msg);
		return;
	}

	/* Credit from the server arrives as a message (readable); a full
	 * socket shows as not writable.  Ask for POLLOUT only when the
	 * library says it has credit, and stop asking when POLLOUT keeps
	 * firing without anything getting through. */
	room = la_writable(s);
	if (room < 0 && !is_retry_error(room)) {
		lose_stream(self, (gint)room, "la_writable", msg);
		return;
	}
	if (room > 0 && self->poll_spins < MAX_POLL_SPINS)
		want_out = TRUE;

	p[0].fd = self->wake_fd[0];
	p[0].events = POLLIN;
	p[0].revents = 0;
	p[1].fd = fd;
	p[1].events = POLLIN | (want_out ? POLLOUT : 0);
	p[1].revents = 0;

	g_mutex_unlock(&self->lock);
	r = poll(p, 2, WAIT_SLICE_MS);
	if (p[0].revents & POLLIN)
		wake_drain(self);
	g_mutex_lock(&self->lock);

	if (r < 0 && errno != EINTR)
		GST_DEBUG_OBJECT(self, "poll: %s", g_strerror(errno));

	/* The stream may have been replaced or closed meanwhile only by this
	 * thread, so `s' is still ours -- but a stop() may have marked it. */
	if (self->stream != s)
		return;
	if (self->broken) {
		lose_stream(self, self->broken_err, "control", msg);
		return;
	}
	if (want_out && (p[1].revents & POLLOUT) &&
	    !(p[1].revents & POLLIN))
		self->poll_spins++;
	if (p[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
		service_stream(self, msg);
}

/* Degraded-mode write: consume in real time.  Returns the bytes taken; 0
 * means "slept, look at the state again". */
static guint null_write(GstLikeosAudioSink *self, guint len)
{
	gint64 now = g_get_monotonic_time();
	gint64 ahead, lead;
	guint64 total;

	if (!self->degraded) {
		/* Prepared but never connected and no fallback mode: nothing
		 * sensible to do but drop it. */
		return len;
	}

	ahead = frames_to_us(self->null_frames, self->rate) -
		(now - self->null_epoch);
	if (ahead < 0) {
		/* Behind (the writer was starved): do not bank the lost time
		 * as credit, or the next writes would rush through. */
		self->null_epoch = now - frames_to_us(self->null_frames,
						      self->rate);
		ahead = 0;
	}

	/* Keep about as much "queued" as the server would have. */
	lead = (gint64)self->server_buffer_ms * 1000;
	if (ahead > lead) {
		gint64 ms = (ahead - lead) / 1000 + 1;

		g_mutex_unlock(&self->lock);
		wake_sleep(self, (gint)MIN(ms, (gint64)WAIT_SLICE_MS * 2));
		g_mutex_lock(&self->lock);
		return 0;
	}

	total = (guint64)self->null_rem + len;
	self->null_frames += total / self->bpf;
	self->null_rem = (guint)(total % self->bpf);
	return len;
}

/* ---- GstAudioSink vmethods ------------------------------------------------- */

static gboolean gst_likeos_audio_sink_open(GstAudioSink *asink)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);
	struct aud_server_info info;
	GError *gerr = NULL;
	gchar app[AUDIOD_NAME_MAX];
	gchar *client;
	la_ctl *ctl;
	gint err = 0, r;

	if (!wake_open(self, &gerr)) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE,
				  ("Could not set up the audio output"),
				  ("wake-up pipe: %s",
				   gerr ? gerr->message : "unknown"));
		g_clear_error(&gerr);
		return FALSE;
	}

	GST_OBJECT_LOCK(self);
	client = g_strdup(self->client_name);
	GST_OBJECT_UNLOCK(self);
	if (!copy_name(app, sizeof(app), client) &&
	    !copy_name(app, sizeof(app), g_get_prgname()))
		g_strlcpy(app, "gstreamer", sizeof(app));
	g_free(client);

	/* Is the service there?  A control connection is the cheapest way to
	 * ask that without creating a stream; it is closed again right away,
	 * so an element sitting in READY does not hold a server slot. */
	ctl = la_ctl_open(app, &err);
	if (ctl == NULL) {
		GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE,
				  ("Could not connect to the audio service"),
				  ("%s: %s", AUDIOD_SOCKET_PATH,
				   la_strerror(err < 0 ? err : -ECONNREFUSED)));
		wake_close(self);
		return FALSE;
	}

	memset(&info, 0, sizeof(info));
	r = la_get_server_info(ctl, &info);
	if (r >= 0) {
		info.card_name[sizeof(info.card_name) - 1] = '\0';
		info.codec[sizeof(info.codec) - 1] = '\0';
		GST_INFO_OBJECT(self,
				"audio service v%u: %s %u Hz %u ch, card %u "
				"\"%s\" (%s), %u streams",
				info.version, la_format_name(info.format),
				info.rate, info.channels, info.card,
				info.card_name, info.codec, info.streams);
	} else {
		GST_DEBUG_OBJECT(self, "server info unavailable: %s",
				 la_strerror(r));
	}
	la_ctl_close(ctl);

	g_mutex_lock(&self->lock);
	self->failed = FALSE;
	self->reconnect_backoff = RECONNECT_MIN_US;
	g_mutex_unlock(&self->lock);
	return TRUE;
}

static gboolean gst_likeos_audio_sink_close(GstAudioSink *asink)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);

	g_mutex_lock(&self->lock);
	/* unprepare() normally did this already */
	if (self->stream != NULL) {
		la_close(self->stream);
		self->stream = NULL;
	}
	self->prepared = FALSE;
	self->degraded = FALSE;
	g_mutex_unlock(&self->lock);

	wake_close(self);
	return TRUE;
}

/* Work out the role and names for a new stream from the settings. */
static void settle_identity(GstLikeosAudioSink *self, guint32 *role,
			    gchar *app, gchar *name, gsize size,
			    gint64 *server_buffer_time)
{
	const gchar *media_role = NULL, *media_name = NULL;
	const gchar *media_title = NULL, *media_artist = NULL;
	const gchar *app_prop = NULL;
	gchar *joined = NULL;

	GST_OBJECT_LOCK(self);
	if (self->stream_props != NULL) {
		media_role =
			gst_structure_get_string(self->stream_props,
						 "media.role");
		media_name =
			gst_structure_get_string(self->stream_props,
						 "media.name");
		media_title =
			gst_structure_get_string(self->stream_props,
						 "media.title");
		media_artist =
			gst_structure_get_string(self->stream_props,
						 "media.artist");
		app_prop = gst_structure_get_string(self->stream_props,
						    "application.name");
	}
	if (media_title == NULL)
		media_title = self->tag_title;
	if (media_artist == NULL)
		media_artist = self->tag_artist;

	*role = map_role(media_role);

	/* application: client-name, then the properties, then the program */
	if (!copy_name(app, size, self->client_name) &&
	    !copy_name(app, size, app_prop) &&
	    !copy_name(app, size, g_get_application_name()) &&
	    !copy_name(app, size, g_get_prgname()))
		g_strlcpy(app, "gstreamer", size);

	/* stream: an explicit name, else "artist - title", else the title,
	 * else the element's own name (libaudio falls back to the app). */
	if (media_title != NULL && media_artist != NULL)
		joined = g_strdup_printf("%s - %s", media_artist,
					 media_title);
	if (!copy_name(name, size, media_name) &&
	    !copy_name(name, size, joined) &&
	    !copy_name(name, size, media_title))
		name[0] = '\0';
	g_free(joined);

	*server_buffer_time = self->server_buffer_time;
	GST_OBJECT_UNLOCK(self);

	if (media_role != NULL && *role == AUD_ROLE_OTHER)
		GST_DEBUG_OBJECT(self, "media.role \"%s\" treated as other",
				 media_role);
}

static gboolean gst_likeos_audio_sink_prepare(GstAudioSink *asink,
					      GstAudioRingBufferSpec *spec)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);
	struct likeos_audio_pending_msg msg = { 0 };
	gchar app[AUDIOD_NAME_MAX], name[AUDIOD_NAME_MAX];
	gint64 sbt, ring_ms, buf_ms;
	guint32 aud_fmt, role;
	gint rate, channels, bpf, r;
	gboolean fallback, degraded, ok = TRUE;

	if (spec->type != GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
				  ("Only raw audio can be played"),
				  ("ring buffer format type %d", spec->type));
		return FALSE;
	}
	if (GST_AUDIO_INFO_LAYOUT(&spec->info) !=
	    GST_AUDIO_LAYOUT_INTERLEAVED) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
				  ("Only interleaved audio can be played"),
				  (NULL));
		return FALSE;
	}

	aud_fmt = map_format(GST_AUDIO_INFO_FORMAT(&spec->info));
	rate = GST_AUDIO_INFO_RATE(&spec->info);
	channels = GST_AUDIO_INFO_CHANNELS(&spec->info);
	bpf = GST_AUDIO_INFO_BPF(&spec->info);

	/* The caps already restrict all of this; the checks are for
	 * applications that hand-craft caps past the template. */
	if (aud_fmt == 0) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
				  ("Unsupported sample format"),
				  ("format %s",
				   gst_audio_format_to_string(
					   GST_AUDIO_INFO_FORMAT(
						   &spec->info))));
		return FALSE;
	}
	if (rate < LIKEOS_AUDIO_MIN_RATE || rate > LIKEOS_AUDIO_MAX_RATE ||
	    channels < 1 || channels > LIKEOS_AUDIO_MAX_CHANNELS || bpf <= 0) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
				  ("Unsupported audio format"),
				  ("rate %d, channels %d, frame %d bytes",
				   rate, channels, bpf));
		return FALSE;
	}

	/* Note layouts other than the one audiod assumes for this count. */
	if (channels > 2 && !GST_AUDIO_INFO_IS_UNPOSITIONED(&spec->info)) {
		guint64 mask = 0;

		if (gst_audio_channel_positions_to_mask(
			    spec->info.position, channels, FALSE, &mask) &&
		    mask != expected_mask[channels])
			GST_INFO_OBJECT(self,
					"%d-channel layout 0x%" G_GINT64_MODIFIER
					"x played as 0x%" G_GINT64_MODIFIER "x",
					channels, mask,
					expected_mask[channels]);
	}

	settle_identity(self, &role, app, name, sizeof(app), &sbt);

	/* Server buffering.  The ring buffer here already holds buffer-time;
	 * what audiod holds on top only has to ride out scheduling gaps of the
	 * thread that writes to it, so by default it is half the ring, within
	 * bounds, and never less than two segments. */
	ring_ms = (gint64)(spec->buffer_time / 1000);
	if (sbt > 0) {
		buf_ms = sbt / 1000;
	} else {
		buf_ms = ring_ms / 2;
		buf_ms = CLAMP(buf_ms, AUTO_SERVER_BUFFER_MIN_MS,
			       AUTO_SERVER_BUFFER_MAX_MS);
	}
	buf_ms = MAX(buf_ms, (gint64)(spec->latency_time / 1000) * 2);
	buf_ms = CLAMP(buf_ms, SERVER_BUFFER_MIN_MS, SERVER_BUFFER_MAX_MS);

	g_mutex_lock(&self->lock);
	if (self->stream != NULL) {
		/* prepare without unprepare: should not happen, be safe */
		la_close(self->stream);
		self->stream = NULL;
	}
	self->fmt.format = aud_fmt;
	self->fmt.rate = (guint32)rate;
	self->fmt.channels = (guint32)channels;
	self->rate = (guint)rate;
	self->bpf = (guint)bpf;
	self->role = role;
	self->server_buffer_ms = (guint32)buf_ms;
	g_strlcpy(self->app_name, app, sizeof(self->app_name));
	g_strlcpy(self->stream_name, name, sizeof(self->stream_name));

	self->paused = FALSE;
	self->broken = FALSE;
	self->broken_err = 0;
	self->discard_seq++;
	self->have_sample = FALSE;
	self->lat_frames = 0;
	self->lat_time = 0;
	self->written_frames = 0;
	self->written_at_sample = 0;
	self->written_rem = 0;
	self->last_refresh = 0;
	self->degraded = FALSE;
	self->null_frames = 0;
	self->null_rem = 0;
	self->poll_spins = 0;

	/* open_stream() passes the volume with the request and sets mute. */
	r = open_stream(self);
	if (r == 0) {
		self->prepared = TRUE;
	} else {
		GST_OBJECT_LOCK(self);
		fallback = self->null_fallback;
		GST_OBJECT_UNLOCK(self);

		if (fallback && !self->failed) {
			gint64 now = g_get_monotonic_time();

			/* Reachable a moment ago in open(), refused now
			 * (restarting, or out of stream slots): play on in
			 * silence and keep trying. */
			self->prepared = TRUE;
			self->degraded = TRUE;
			self->null_epoch = now;
			self->null_paused_at = now;
			self->next_reconnect = now + self->reconnect_backoff;
			self->reconnect_backoff =
				MIN(self->reconnect_backoff * 2,
				    (gint64)RECONNECT_MAX_US);
			pending_set(&msg, FALSE, GST_RESOURCE_ERROR_OPEN_WRITE,
				    "The audio service refused the stream; "
				    "playing without sound",
				    g_strdup_printf("la_open: %s",
						    la_strerror(r)));
		} else {
			ok = FALSE;
			pending_set(&msg, TRUE, GST_RESOURCE_ERROR_OPEN_WRITE,
				    "Could not open an audio stream",
				    g_strdup_printf("la_open (%s, %d Hz, %d "
						    "ch): %s",
						    la_format_name(aud_fmt),
						    rate, channels,
						    la_strerror(r)));
		}
	}
	degraded = self->degraded;
	g_mutex_unlock(&self->lock);

	pending_post(self, &msg);
	g_object_notify(G_OBJECT(self), "connected");

	GST_DEBUG_OBJECT(self,
			 "prepared: %s %d Hz %d ch, segment %d bytes x %d, "
			 "ring %" G_GINT64_FORMAT " ms, server %" G_GINT64_FORMAT
			 " ms%s",
			 la_format_name(aud_fmt), rate, channels,
			 spec->segsize, spec->segtotal, ring_ms, buf_ms,
			 ok && degraded ? " (degraded)" : "");
	return ok;
}

static gboolean gst_likeos_audio_sink_unprepare(GstAudioSink *asink)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);

	/* The ring-buffer thread is joined before this is called, so nobody
	 * is inside write(). */
	g_mutex_lock(&self->lock);
	if (self->stream != NULL) {
		GST_DEBUG_OBJECT(self, "closing stream %u",
				 la_stream_id(self->stream));
		la_close(self->stream);
		self->stream = NULL;
	}
	self->prepared = FALSE;
	self->degraded = FALSE;
	self->paused = FALSE;
	self->broken = FALSE;
	self->have_sample = FALSE;
	g_mutex_unlock(&self->lock);

	g_object_notify(G_OBJECT(self), "connected");
	return TRUE;
}

static gint gst_likeos_audio_sink_write(GstAudioSink *asink, gpointer data,
					guint length)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);
	struct likeos_audio_pending_msg msg = { 0 };
	const guint8 *p = data;
	gboolean was_connected, now_connected;
	gint64 last_progress;
	guint done = 0;
	guint seq;

	if (length == 0 || data == NULL)
		return 0;

	g_mutex_lock(&self->lock);
	if (!self->prepared) {
		g_mutex_unlock(&self->lock);
		return (gint)length;
	}
	was_connected = self->stream != NULL;
	seq = self->discard_seq;
	last_progress = g_get_monotonic_time();

	while (done < length) {
		gint64 now = g_get_monotonic_time();
		gssize n;

		/* stop(), reset() or a flush: what we hold is stale.  Claim
		 * it written, so the ring buffer moves on without a warning. */
		if (self->discard_seq != seq) {
			done = length;
			break;
		}

		if (self->stream != NULL && self->broken) {
			lose_stream(self, self->broken_err, "control", &msg);
			continue;
		}

		/* ---- no stream: degraded, real-time pacing ---- */
		if (self->stream == NULL) {
			guint took;

			if (self->paused) {
				if (!ring_started(self))
					break;
				/* resume() is on its way */
				g_mutex_unlock(&self->lock);
				wake_sleep(self, 1);
				g_mutex_lock(&self->lock);
				continue;
			}
			if (self->degraded && try_reconnect(self, now))
				continue;
			took = null_write(self, length - done);
			done += took;
			continue;
		}

		/* ---- a stream: hand over as much as it takes ---- */
		n = la_write(self->stream, p + done, length - done);
		if (n > 0) {
			if ((gsize)n > length - done)
				n = (gssize)(length - done);
			done += (guint)n;
			account_written(self, (gsize)n, now);
			last_progress = now;
			self->poll_spins = 0;
			continue;
		}
		if (n < 0 && !is_retry_error(n)) {
			lose_stream(self, (gint)n, "la_write", &msg);
			continue;
		}

		/* No room.  Paused: the server keeps what it has and this
		 * segment's remainder goes; return so the ring thread can
		 * park (GstAudioSink requires pause to unblock writes). */
		if (self->paused && !ring_started(self))
			break;
		if (self->paused) {
			last_progress = now;
		} else if (now - last_progress > STALL_US) {
			lose_stream(self, -ETIMEDOUT, "stream stalled", &msg);
			continue;
		}

		wait_for_room(self, &msg);
	}

	if (self->stream != NULL)
		refresh_latency(self, g_get_monotonic_time(), &msg);
	now_connected = self->stream != NULL;
	g_mutex_unlock(&self->lock);

	pending_post(self, &msg);
	if (was_connected != now_connected)
		g_object_notify(G_OBJECT(self), "connected");
	return (gint)done;
}

static guint gst_likeos_audio_sink_delay(GstAudioSink *asink)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);
	gint64 q;

	g_mutex_lock(&self->lock);
	q = current_queue(self, g_get_monotonic_time());
	g_mutex_unlock(&self->lock);

	if (q > (gint64)G_MAXUINT)
		q = G_MAXUINT;
	return (guint)q;
}

/* Drop everything queued, here and in the server.  `keep_paused': a flush
 * during a pause (a seek) leaves the stream paused for the resume() that
 * follows; a stop does not, because the ring buffer restarts a stopped
 * stream through start(), which has no hook here. */
static void discard_all(GstLikeosAudioSink *self, gboolean keep_paused,
			const gchar *why)
{
	gint64 now;
	gint r;

	g_mutex_lock(&self->lock);
	if (!self->prepared) {
		g_mutex_unlock(&self->lock);
		return;
	}
	now = g_get_monotonic_time();
	self->discard_seq++;

	if (self->stream != NULL && !self->broken) {
		r = la_flush(self->stream);
		if (r < 0 && !is_retry_error(r)) {
			GST_DEBUG_OBJECT(self, "%s: flush failed: %s", why,
					 la_strerror(r));
			self->broken = TRUE;
			self->broken_err = r;
		}
		if (!keep_paused && self->paused && !self->broken) {
			r = la_resume(self->stream);
			if (r < 0 && !is_retry_error(r)) {
				self->broken = TRUE;
				self->broken_err = r;
			}
		}
	}
	if (!keep_paused)
		self->paused = FALSE;

	self->written_rem = 0;
	rebase_sample(self, 0, now);
	self->last_refresh = 0;
	self->null_frames = 0;
	self->null_rem = 0;
	self->null_epoch = now;
	self->null_paused_at = now;
	g_mutex_unlock(&self->lock);

	GST_DEBUG_OBJECT(self, "%s: discarded", why);
	wake_writer(self);
}

static void gst_likeos_audio_sink_stop(GstAudioSink *asink)
{
	discard_all(GST_LIKEOS_AUDIO_SINK(asink), FALSE, "stop");
}

static void gst_likeos_audio_sink_reset(GstAudioSink *asink)
{
	/* Only used by GstAudioSink when neither pause nor stop exist; kept
	 * for completeness with the same meaning as stop. */
	discard_all(GST_LIKEOS_AUDIO_SINK(asink), FALSE, "reset");
}

static void gst_likeos_audio_sink_clear_all(GstAudioSink *asink)
{
	discard_all(GST_LIKEOS_AUDIO_SINK(asink), TRUE, "clear-all");
}

static void gst_likeos_audio_sink_pause(GstAudioSink *asink)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);
	gint64 now;
	gint r;

	g_mutex_lock(&self->lock);
	if (!self->prepared || self->paused) {
		g_mutex_unlock(&self->lock);
		wake_writer(self);
		return;
	}
	now = g_get_monotonic_time();

	/* Freeze the extrapolation at what is queued now. */
	if (self->stream != NULL)
		rebase_sample(self, stream_queue_raw(self, now), now);
	self->null_paused_at = now;
	self->paused = TRUE;

	/* Cork the stream: what audiod holds stays there and plays on
	 * resume, instead of running out while the pipeline's clock stands
	 * still (which would put audio ahead of video afterwards). */
	if (self->stream != NULL && !self->broken) {
		r = la_pause(self->stream);
		if (r < 0 && !is_retry_error(r)) {
			GST_DEBUG_OBJECT(self, "la_pause: %s", la_strerror(r));
			self->broken = TRUE;
			self->broken_err = r;
		}
	}
	g_mutex_unlock(&self->lock);

	GST_DEBUG_OBJECT(self, "paused");
	wake_writer(self);
}

static void gst_likeos_audio_sink_resume(GstAudioSink *asink)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(asink);
	gint64 now;
	gint r;

	g_mutex_lock(&self->lock);
	if (!self->prepared || !self->paused) {
		g_mutex_unlock(&self->lock);
		return;
	}
	now = g_get_monotonic_time();

	if (self->stream != NULL && !self->broken) {
		r = la_resume(self->stream);
		if (r < 0 && !is_retry_error(r)) {
			GST_DEBUG_OBJECT(self, "la_resume: %s",
					 la_strerror(r));
			self->broken = TRUE;
			self->broken_err = r;
		}
	}
	/* The frozen queue starts draining again from now. */
	self->lat_time = now;
	if (self->degraded)
		self->null_epoch += now - self->null_paused_at;
	self->paused = FALSE;
	g_mutex_unlock(&self->lock);

	GST_DEBUG_OBJECT(self, "resumed");
	wake_writer(self);
}

/* ---- events: titles for the stream name ------------------------------------ */

static gboolean gst_likeos_audio_sink_event(GstBaseSink *bsink,
					    GstEvent *event)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(bsink);

	if (GST_EVENT_TYPE(event) == GST_EVENT_TAG) {
		GstTagList *list = NULL;
		gchar *title = NULL, *artist = NULL;

		gst_event_parse_tag(event, &list);
		if (list != NULL) {
			gst_tag_list_get_string(list, GST_TAG_TITLE, &title);
			gst_tag_list_get_string(list, GST_TAG_ARTIST, &artist);
		}
		/* Only used for the NEXT stream: the protocol has no way to
		 * rename an open one, and a running stream keeps its name. */
		if (title != NULL || artist != NULL) {
			GST_OBJECT_LOCK(self);
			if (title != NULL) {
				g_free(self->tag_title);
				self->tag_title = title;
				title = NULL;
			}
			if (artist != NULL) {
				g_free(self->tag_artist);
				self->tag_artist = artist;
				artist = NULL;
			}
			GST_OBJECT_UNLOCK(self);
		}
		g_free(title);
		g_free(artist);
	} else if (GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START) {
		/* A new stream: the previous one's title no longer applies. */
		GST_OBJECT_LOCK(self);
		g_clear_pointer(&self->tag_title, g_free);
		g_clear_pointer(&self->tag_artist, g_free);
		GST_OBJECT_UNLOCK(self);
	}

	return GST_BASE_SINK_CLASS(parent_class)->event(bsink, event);
}

/* ---- properties -------------------------------------------------------------- */

static void set_volume_state(GstLikeosAudioSink *self, gboolean is_mute,
			     gdouble volume, gboolean mute)
{
	gboolean changed;

	g_mutex_lock(&self->lock);
	if (is_mute) {
		changed = self->mute != mute;
		self->mute = mute;
	} else {
		changed = self->volume != volume;
		self->volume = volume;
	}
	if (changed && self->stream != NULL)
		apply_volume(self);
	g_mutex_unlock(&self->lock);

	/* A broken stream is noticed and replaced by the writer; nudge it. */
	if (changed)
		wake_writer(self);
}

static void gst_likeos_audio_sink_set_property(GObject *object, guint prop_id,
					       const GValue *value,
					       GParamSpec *pspec)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(object);

	switch (prop_id) {
	case PROP_VOLUME:
		set_volume_state(self, FALSE, g_value_get_double(value),
				 FALSE);
		break;
	case PROP_MUTE:
		set_volume_state(self, TRUE, 0.0, g_value_get_boolean(value));
		break;
	case PROP_CLIENT_NAME: {
		const gchar *v = g_value_get_string(value);

		GST_OBJECT_LOCK(self);
		g_free(self->client_name);
		self->client_name = (v != NULL && *v) ? g_strdup(v) : NULL;
		GST_OBJECT_UNLOCK(self);
		break;
	}
	case PROP_STREAM_PROPERTIES: {
		const GstStructure *s = gst_value_get_structure(value);

		GST_OBJECT_LOCK(self);
		if (self->stream_props != NULL)
			gst_structure_free(self->stream_props);
		self->stream_props = s ? gst_structure_copy(s) : NULL;
		GST_OBJECT_UNLOCK(self);
		GST_DEBUG_OBJECT(self, "stream-properties: %" GST_PTR_FORMAT,
				 s);
		break;
	}
	case PROP_SERVER_BUFFER_TIME:
		GST_OBJECT_LOCK(self);
		self->server_buffer_time = g_value_get_int64(value);
		GST_OBJECT_UNLOCK(self);
		break;
	case PROP_NULL_FALLBACK:
		GST_OBJECT_LOCK(self);
		self->null_fallback = g_value_get_boolean(value);
		GST_OBJECT_UNLOCK(self);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void gst_likeos_audio_sink_get_property(GObject *object, guint prop_id,
					       GValue *value,
					       GParamSpec *pspec)
{
	GstLikeosAudioSink *self = GST_LIKEOS_AUDIO_SINK(object);

	switch (prop_id) {
	case PROP_VOLUME:
		g_mutex_lock(&self->lock);
		g_value_set_double(value, self->volume);
		g_mutex_unlock(&self->lock);
		break;
	case PROP_MUTE:
		g_mutex_lock(&self->lock);
		g_value_set_boolean(value, self->mute);
		g_mutex_unlock(&self->lock);
		break;
	case PROP_CLIENT_NAME:
		GST_OBJECT_LOCK(self);
		g_value_set_string(value, self->client_name);
		GST_OBJECT_UNLOCK(self);
		break;
	case PROP_STREAM_PROPERTIES:
		GST_OBJECT_LOCK(self);
		gst_value_set_structure(value, self->stream_props);
		GST_OBJECT_UNLOCK(self);
		break;
	case PROP_SERVER_BUFFER_TIME:
		GST_OBJECT_LOCK(self);
		g_value_set_int64(value, self->server_buffer_time);
		GST_OBJECT_UNLOCK(self);
		break;
	case PROP_NULL_FALLBACK:
		GST_OBJECT_LOCK(self);
		g_value_set_boolean(value, self->null_fallback);
		GST_OBJECT_UNLOCK(self);
		break;
	case PROP_CONNECTED:
		g_mutex_lock(&self->lock);
		g_value_set_boolean(value, self->stream != NULL);
		g_mutex_unlock(&self->lock);
		break;
	case PROP_ROLE:
		g_mutex_lock(&self->lock);
		g_value_set_string(value, self->prepared ?
						  role_label(self->role) :
						  NULL);
		g_mutex_unlock(&self->lock);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}
