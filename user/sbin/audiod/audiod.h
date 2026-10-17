// audiod - the LikeOS audio service: shared declarations.
//
// One single-threaded poll loop owns everything: the listening socket, the
// clients, their streams, the mixer and the device.  The device's demand
// for frames drives the mixer; the mixer drains the stream rings; freed ring
// space becomes REQUEST credits for the clients.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef AUDIOD_H
#define AUDIOD_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <audio/protocol.h>

#include "dsp.h"
#include "parse.h"

#define AUDIOD_VERSION "1.0"
#define AUDIOD_CONFIG_PATH "/etc/audiod.conf"
#define AUDIOD_STATE_DIR "/var/lib/audiod"
#define AUDIOD_STATE_PATH "/var/lib/audiod/state"
#define AUDIOD_SOUNDS_DIR "/usr/share/sounds/likeos"
#define AUDIOD_USER "audiod"
#define AUDIOD_GROUP "audio"
#define AUDIOD_FALLBACK_UID 63
#define AUDIOD_FALLBACK_GID 29

/* The host integration test (tests/) builds the daemon with every path
 * moved under a scratch directory; the real build never defines this. */
#ifdef AUDIOD_HOST_TEST
#undef AUDIOD_SOCKET_DIR
#undef AUDIOD_SOCKET_PATH
#undef AUDIOD_STATE_DIR
#undef AUDIOD_STATE_PATH
#undef AUDIOD_SOUNDS_DIR
#undef AUDIOD_CONFIG_PATH
#define AUDIOD_SOCKET_DIR AUDIOD_HOST_TEST "/run"
#define AUDIOD_SOCKET_PATH AUDIOD_HOST_TEST "/run/socket"
#define AUDIOD_STATE_DIR AUDIOD_HOST_TEST "/lib"
#define AUDIOD_STATE_PATH AUDIOD_HOST_TEST "/lib/state"
#define AUDIOD_SOUNDS_DIR AUDIOD_HOST_TEST "/sounds"
#define AUDIOD_CONFIG_PATH AUDIOD_HOST_TEST "/audiod.conf"
#endif

/* hard ceilings, whatever the configuration says */
#define HARD_MAX_CLIENTS 256
#define HARD_MAX_STREAMS AUD_MAX_STREAMS
#define MAX_EVENT_STREAMS 8
#define CLIENT_OUTQ_MAX (512u * 1024u) /* unread replies before we give up */
#define HELLO_TIMEOUT_MS 5000
#define PARTIAL_MSG_TIMEOUT_MS 15000
#define STATE_SAVE_DELAY_MS 2000
#define REOPEN_INTERVAL_MS 3000

/* ---- log.c ----------------------------------------------------------------- */

#define AL_ERR 0
#define AL_WARN 1
#define AL_INFO 2
#define AL_DEBUG 3

void log_set_level(int level);
int log_get_level(void);
void log_set_console(int on); /* also write to /dev/console */
void logmsg(int level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void vlogmsg(int level, const char *fmt, va_list ap);
const char *log_level_name(int level);
int log_level_from_name(const char *s); /* -1 if unknown */

#define log_err(...) logmsg(AL_ERR, __VA_ARGS__)
#define log_warn(...) logmsg(AL_WARN, __VA_ARGS__)
#define log_info(...) logmsg(AL_INFO, __VA_ARGS__)
#define log_debug(...) logmsg(AL_DEBUG, __VA_ARGS__)

/* ---- config.c ------------------------------------------------------------ */

#define HWVOL_AUTO (-1)
#define HWVOL_NO 0
#define HWVOL_YES 1

struct audiod_config {
	char device[64]; /* "auto", "/dev/audioN" or a card number */
	uint32_t rate; /* 0 = auto (48000, else 44100, else best) */
	int format; /* SF_*, SF_NONE = auto (s16 preferred) */
	uint32_t channels; /* 0 = auto (2) */
	uint32_t period_ms;
	uint32_t periods;
	uint32_t default_volume;
	int resample_quality; /* RS_QUALITY_* */
	int event_sounds;
	uint32_t event_rate; /* event sounds per uid per 10 s */
	uint32_t max_streams_per_user;
	uint32_t max_clients;
	uint32_t max_buffer_kb_per_user;
	uint32_t default_buffer_ms;
	uint32_t suspend_ms; /* stop the device after this much silence */
	int hw_volume; /* HWVOL_* */
	int log_level; /* AL_* */
};

void config_defaults(struct audiod_config *c);
/* Parse configuration text.  On error nothing is stored and `err' says
 * which line and why.  Pure: used by the host tests too. */
int config_parse(const char *text, size_t len, struct audiod_config *out,
		 char *err, size_t errlen);
/* Load a file: 0 loaded, 1 absent (defaults), -1 rejected (`out'
 * untouched).  The file must be a regular file owned by root (or by the
 * running user when not root) and not writable by group or others. */
int config_load(const char *path, struct audiod_config *out);
/* Is the device setting a path we accept (/dev/audioN, /dev/dsp[N])? */
int config_device_ok(const char *dev);

/* ---- state.c ------------------------------------------------------------- */

struct audiod_state {
	uint32_t vol_l, vol_r;
	uint32_t mute;
	uint32_t out_mode, out_id;
	uint32_t pcm_output; /* output id of the PCM device in use, 0 analog */
	int32_t card; /* -1: none recorded */
};

int state_load(const char *path, struct audiod_state *st);
int state_save(const char *path, const struct audiod_state *st);
int state_parse(const char *text, size_t len, struct audiod_state *st);

/* ---- device.c ------------------------------------------------------------ */

struct audio_dev {
	int fd; /* PCM node, -1 when closed */
	int evfd; /* mixer node for events when the PCM node has none */
	int events_on_pcm;
	int events_ok; /* GETEVENT answers on the chosen node */
	uint32_t ev_spurious;
	int null; /* virtual sink paced by the clock */
	int card;
	char path[64];
	char mixer_path[64];

	int have_info;
	char card_name[64], codec[64], driver[32], bus[32];
	uint32_t info_flags;

	int fmt; /* SF_* */
	uint32_t hw_fmt; /* AUDIO_FMT_* */
	uint32_t rate;
	int channels;
	size_t frame_bytes;
	uint32_t period_frames, periods, buffer_frames;

	int running; /* AUDIO_START issued */
	uint64_t written; /* frames accepted by the device since open */
	uint32_t delay; /* frames queued, from the last position query */
	uint32_t avail;
	uint32_t xruns;
	uint64_t null_t0_ns; /* null sink: time of frame 0 */
	uint64_t null_base; /* frames "played" before null_t0 */

	/* rendered but not yet accepted */
	uint8_t *pend;
	size_t pend_cap, pend_off, pend_len;

	/* controls */
	/* PCM device of the card the handle plays on (HDMI/DP ports have
	 * their own) */
	uint32_t pcm_count, pcm_index;
	uint32_t pcm_output_id; /* 0 = the general/analog device */

	int hw_vol_ctl; /* AUDIO_CTL_* used for master volume, -1 none */
	int hw_mute_ctl; /* -1 none */
	struct aud_outputs outs; /* in protocol form */
	int have_outs;
};

/* Open the configured device (or scan for one); 0 or negative errno. */
int dev_open(struct audio_dev *d, const struct audiod_config *cfg,
	     int card_hint);
/* A clock-paced sink that discards everything (no hardware). */
void dev_open_null(struct audio_dev *d, const struct audiod_config *cfg);
void dev_close(struct audio_dev *d);
/* Query position: 0, -EPIPE after an underrun, -ENODEV when gone. */
int dev_update(struct audio_dev *d);
/* Restart after an underrun or a stop. */
int dev_prepare(struct audio_dev *d);
int dev_start(struct audio_dev *d);
void dev_stop(struct audio_dev *d);
/* Room for a render of `frames' frames in d->pend. */
uint8_t *dev_pend_buffer(struct audio_dev *d, size_t frames);
void dev_pend_commit(struct audio_dev *d, size_t frames);
/* Push pending bytes: 0 all written, 1 device full, <0 error. */
int dev_flush(struct audio_dev *d);
/* Milliseconds until the null sink wants more, or the device poll
 * timeout safety net. */
int dev_timeout_ms(const struct audio_dev *d);
/* Events: returns how many were handled via the callback. */
typedef void (*dev_event_fn)(uint32_t type, uint32_t id, uint32_t value);
int dev_read_events(struct audio_dev *d, dev_event_fn fn);
int dev_event_fd(const struct audio_dev *d, short *events);
/* Controls */
int dev_set_volume(struct audio_dev *d, uint32_t l, uint32_t r);
int dev_get_volume(struct audio_dev *d, uint32_t *l, uint32_t *r);
int dev_set_mute(struct audio_dev *d, int mute);
int dev_refresh_outputs(struct audio_dev *d);
int dev_set_output(struct audio_dev *d, uint32_t mode, uint32_t id);
/* Move the handle to the PCM device feeding `output_id' (0 = analog):
 * 0 already there, 1 moved (the format may have changed: reconfigure),
 * negative errno on failure. */
int dev_select_pcm(struct audio_dev *d, const struct audiod_config *cfg,
		   uint32_t output_id);
int dev_has_pcm_for(struct audio_dev *d, uint32_t output_id);
/* All cards, the one in use from cache */
int dev_list_cards(const struct audio_dev *d, struct aud_cards *out);
uint64_t now_ns(void);
uint64_t now_ms(void);

/* ---- streams and clients (clients.c) --------------------------------------- */

#define STRM_PLAYING 0
#define STRM_PAUSED 1
#define STRM_DRAINING 2
#define STRM_UNDERRUN 3

struct client;

struct stream {
	struct stream *next;
	uint32_t id;
	uint32_t client_id; /* 0 for internal (event) streams */
	struct client *owner; /* NULL for internal streams */
	uint32_t uid, gid;
	int32_t pid;
	uint32_t role;
	char name[AUDIOD_NAME_MAX];
	char app[AUDIOD_NAME_MAX];
	uint32_t aud_fmt;

	struct dsp_stream dsp;
	uint32_t vol_l, vol_r, mute;

	size_t buffer_bytes; /* ring size */
	size_t credit; /* bytes the client may still send */
	size_t request_min; /* do not send REQUESTs smaller than this */
	size_t prebuf_bytes;

	int prebuffering; /* waiting for prebuf_bytes before playing */
	int paused;
	int draining; /* a DRAIN awaits its OK */
	uint64_t drain_target; /* engine frame at which all was heard */
	int drain_target_set;
	uint64_t drain_deadline_ms;
	int underrun_sent;
	uint32_t underruns;
	int killed; /* fading out before removal */
	uint64_t last_played;

	/* internal event streams */
	struct ev_sound *evsnd;
	uint32_t waiter_client; /* reply OK to this client when done */
	int done; /* finished playing, remove */
};

struct client {
	struct client *next;
	int fd;
	uint32_t id;
	struct ucred cred;
	int kind; /* 0 until HELLO, then AUD_CLIENT_* */
	char name[AUDIOD_NAME_MAX];

	uint8_t inbuf[16384];
	size_t in_off, in_len;
	struct msg_reader mr;

	uint8_t *out;
	size_t out_off, out_len, out_cap;

	struct stream *stream; /* stream connection */
	uint32_t sub_mask;
	int blocked; /* waiting to reply (drain, event wait) */
	int dead;
	uint64_t accepted_ms, last_rx_ms;
};

/* ---- the server ------------------------------------------------------------ */

struct uid_usage {
	uint32_t uid;
	uint32_t clients, streams;
	size_t bytes;
	/* event sound token bucket */
	uint32_t ev_tokens;
	uint64_t ev_refill_ms;
};

#define MAX_UIDS 64

struct server {
	struct audiod_config cfg;
	char cfg_path[256];
	char dev_override[64];

	struct audio_dev dev;
	struct mixer mix;
	int mix_ready;

	int listen_fd;
	struct client *clients;
	uint32_t nclients;
	struct stream *streams;
	uint32_t nstreams;
	uint32_t nevent_streams;
	uint32_t next_client_id, next_stream_id;
	struct uid_usage uids[MAX_UIDS];

	/* master state (persisted) */
	uint32_t vol_l, vol_r, mute;
	uint32_t out_mode, out_id;
	int32_t card_pref;
	int state_dirty;
	uint64_t state_due_ms;

	uint64_t start_ms;
	uint64_t engine_written; /* frames rendered to devices, ever */
	uint64_t dev_written_base; /* engine_written when this dev opened */
	uint64_t underruns;
	uint64_t last_audible_ms;
	int suspended;
	uint64_t reopen_due_ms;
	int terminating;
	int privileged; /* still root (before the drop) */
};

extern struct server srv;

/* audiod.c: the engine */
void engine_wake(void); /* a stream became playable: resume the device */
uint64_t engine_played(void);
uint32_t engine_delay_frames(void);
void engine_reopen(int card_hint);
int engine_switch_card(int card); /* SET_CARD: open first, then switch */
void engine_master_changed(void); /* push volume/mute to device+mixer */
/* Route to output `id' (mode OUTSEL_*): selects the HDMI/DP PCM device
 * when the output has its own, the analog one otherwise. */
int engine_route_output(uint32_t mode, uint32_t id);
void state_mark_dirty(void);

/* clients.c */
int clients_listen(const char *path);
void clients_accept(void);
void client_io(struct client *c, short revents);
void client_close(struct client *c, const char *why);
void clients_reap(void);
int client_send(struct client *c, uint32_t type, const void *payload,
		uint32_t len);
int client_send_error(struct client *c, int code, const char *msg);
int client_send_ok(struct client *c);
struct client *client_by_id(uint32_t id);
void clients_timeouts(uint64_t now);
/* A DRAIN or waited-for event completed: read the client's input again. */
void client_unblock(struct client *c);
void notify_all(uint32_t mask, uint32_t what, uint32_t a, uint32_t b,
		uint32_t c);
struct uid_usage *uid_get(uint32_t uid, int create);
void uid_put_if_idle(struct uid_usage *u);

/* streams (clients.c) */
struct stream *stream_by_id(uint32_t id);
void stream_destroy(struct stream *s, int notify);
void stream_set_volume(struct stream *s, uint32_t l, uint32_t r,
		       uint32_t mute);
void stream_kill(struct stream *s);
/* After each render: credits, drains, killed and finished streams. */
void streams_after_render(void);
void streams_check_drains(void);
int streams_retarget(void);
int streams_any_active(void);
uint64_t streams_next_deadline(void);
size_t streams_render(float *acc, size_t frames);

/* control.c */
int control_handle(struct client *c, uint32_t type, const uint8_t *p,
		   uint32_t len);
void control_fill_server_info(struct aud_server_info *si);

/* events.c */
int events_play(struct client *c, const char name[AUD_EVENT_NAME_MAX],
		uint32_t volume, int wait);
void events_stream_done(struct stream *s);
void events_cache_clear(void);
void events_put(struct ev_sound *e);

#endif /* AUDIOD_H */
