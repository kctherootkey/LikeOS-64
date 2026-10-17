// LikeOS audio service wire protocol (audiod <-> libaudio).
//
// One AF_UNIX stream connection per client to AUDIOD_SOCKET_PATH.  Every
// message, in both directions, is a fixed header followed by `len' bytes
// of payload.  All integers are little-endian host order (the socket never
// leaves the machine).  The server checks `len' against
// AUDIOD_MAX_PAYLOAD before reading anything and drops the connection on
// any malformed or unknown message.
//
// A connection is either a STREAM connection (it opens exactly one stream
// with AUD_MSG_STREAM_OPEN and then sends DATA) or a CONTROL connection
// (volume, outputs, stream list, event sounds, subscriptions).  The first
// message is always AUD_MSG_HELLO.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_AUDIO_PROTOCOL_H
#define LIKEOS_AUDIO_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIOD_SOCKET_DIR "/var/run/audiod"
#define AUDIOD_SOCKET_PATH "/var/run/audiod/socket"
#define AUDIOD_PROTOCOL_VERSION 1
#define AUDIOD_MAX_PAYLOAD 65536 /* bytes in one message */
#define AUDIOD_NAME_MAX 64

/* ---- message types ------------------------------------------------------- */

enum aud_msg_type {
	/* both kinds */
	AUD_MSG_HELLO = 1, /* c->s: aud_hello; s->c: aud_hello_reply */
	AUD_MSG_ERROR = 2, /* s->c: aud_error (reply to a failed request) */
	AUD_MSG_OK = 3, /* s->c: empty acknowledgement */

	/* stream connection */
	AUD_MSG_STREAM_OPEN = 10, /* c->s: aud_stream_open; s->c: aud_stream_opened */
	AUD_MSG_DATA = 11, /* c->s: raw frames in the stream's format */
	AUD_MSG_DRAIN = 12, /* c->s: reply OK once everything was heard */
	AUD_MSG_FLUSH = 13, /* c->s: drop what is buffered; reply OK */
	AUD_MSG_PAUSE = 14, /* c->s: reply OK */
	AUD_MSG_RESUME = 15, /* c->s: reply OK */
	AUD_MSG_SET_STREAM_VOLUME = 16, /* c->s: aud_volume (own stream); OK */
	AUD_MSG_GET_LATENCY = 17, /* c->s: empty; s->c: aud_latency */
	AUD_MSG_REQUEST = 18, /* s->c: aud_request: room for more bytes */
	AUD_MSG_STREAM_CLOSE = 19, /* c->s: close (connection stays) */
	AUD_MSG_SET_STREAM_MUTE = 20, /* c->s: aud_mute; OK */
	AUD_MSG_UNDERRUN = 21, /* s->c: the stream ran dry */

	/* control connection */
	AUD_MSG_GET_SERVER_INFO = 40, /* -> aud_server_info */
	AUD_MSG_LIST_STREAMS = 41, /* -> aud_stream_list */
	AUD_MSG_GET_MASTER_VOLUME = 42, /* -> aud_volume */
	AUD_MSG_SET_MASTER_VOLUME = 43, /* aud_volume -> OK */
	AUD_MSG_GET_MUTE = 44, /* -> aud_mute */
	AUD_MSG_SET_MUTE = 45, /* aud_mute -> OK */
	AUD_MSG_GET_OUTPUTS = 46, /* -> aud_outputs */
	AUD_MSG_SET_OUTPUT = 47, /* aud_set_output -> OK */
	AUD_MSG_PLAY_EVENT = 48, /* aud_play_event -> OK (or after playing) */
	AUD_MSG_SUBSCRIBE = 49, /* aud_subscribe -> OK, then aud_notify msgs */
	AUD_MSG_NOTIFY = 50, /* s->c: aud_notify */
	AUD_MSG_SET_VOLUME_OF = 51, /* aud_volume_of: another stream (owner/root) */
	AUD_MSG_KILL_STREAM = 52, /* aud_stream_ref: owner or root */
	AUD_MSG_GET_CARDS = 53, /* -> aud_cards */
	AUD_MSG_SET_CARD = 54, /* aud_stream_ref (card index) -> OK; root only */
};

struct aud_header {
	uint32_t type; /* enum aud_msg_type */
	uint32_t len; /* payload bytes that follow */
};

/* ---- payloads ------------------------------------------------------------ */

#define AUD_CLIENT_STREAM 1
#define AUD_CLIENT_CONTROL 2

struct aud_hello {
	uint32_t version; /* AUDIOD_PROTOCOL_VERSION */
	uint32_t kind; /* AUD_CLIENT_* */
	char name[AUDIOD_NAME_MAX]; /* application name */
};

struct aud_hello_reply {
	uint32_t version;
	uint32_t client_id;
	uint32_t rate; /* the device rate the mixer runs at */
	uint32_t channels;
	uint32_t format; /* AUD_FMT_* of the device side */
	uint32_t reserved[3];
};

struct aud_error {
	int32_t code; /* negative errno */
	char message[96];
};

/* Sample formats a client may send (audiod converts). */
#define AUD_FMT_U8 1
#define AUD_FMT_S16_LE 2
#define AUD_FMT_S24_LE 3 /* 24 in the low bits of 32 */
#define AUD_FMT_S24_3LE 4 /* packed 3 bytes */
#define AUD_FMT_S32_LE 5
#define AUD_FMT_F32_LE 6
#define AUD_FMT_S16_BE 7
#define AUD_FMT_F64_LE 8
#define AUD_FMT_MAX 8

/* Stream roles: what the stream is for (volume policy, ducking, list). */
#define AUD_ROLE_MUSIC 1
#define AUD_ROLE_VIDEO 2
#define AUD_ROLE_GAME 3
#define AUD_ROLE_EVENT 4 /* system and notification sounds */
#define AUD_ROLE_PHONE 5
#define AUD_ROLE_WEB 6
#define AUD_ROLE_OTHER 7

struct aud_stream_open {
	uint32_t format; /* AUD_FMT_* */
	uint32_t rate; /* 4000..384000 */
	uint32_t channels; /* 1..8 */
	uint32_t role; /* AUD_ROLE_* */
	uint32_t buffer_ms; /* wanted buffering, 0 = default */
	uint32_t volume; /* initial 0..100 (100 = unity) */
	uint32_t flags; /* AUD_STREAM_* */
	uint32_t reserved;
	char name[AUDIOD_NAME_MAX]; /* "YouTube - luakit", file name, ... */
};

#define AUD_STREAM_START_PAUSED (1u << 0)
#define AUD_STREAM_NO_RESAMPLE_HINT (1u << 1)

struct aud_stream_opened {
	uint32_t stream_id;
	uint32_t buffer_bytes; /* the server's buffer for this stream */
	uint32_t request_bytes; /* send at most this much before a REQUEST */
	uint32_t reserved[5];
};

struct aud_request {
	uint32_t bytes; /* the client may send this many more bytes */
	uint32_t reserved;
};

struct aud_latency {
	uint64_t latency_us; /* buffered in server + device, microseconds */
	uint64_t played_frames; /* frames of this stream already heard */
	uint64_t timestamp_ns; /* monotonic time of the above */
	uint32_t underruns;
	uint32_t reserved;
};

struct aud_volume {
	uint32_t left, right; /* 0..100 */
	uint32_t reserved[2];
};

struct aud_mute {
	uint32_t mute;
	uint32_t reserved;
};

struct aud_volume_of {
	uint32_t stream_id;
	uint32_t left, right;
	uint32_t reserved;
};

struct aud_stream_ref {
	uint32_t id;
	uint32_t reserved;
};

#define AUD_MAX_STREAMS 64

struct aud_stream_entry {
	uint32_t stream_id;
	uint32_t client_id;
	uint32_t uid;
	uint32_t pid;
	uint32_t role;
	uint32_t format, rate, channels;
	uint32_t left, right;
	uint32_t mute;
	uint32_t state; /* 0 playing, 1 paused, 2 draining, 3 underrun */
	uint64_t played_frames;
	char name[AUDIOD_NAME_MAX];
	char app[AUDIOD_NAME_MAX];
};

struct aud_stream_list {
	uint32_t count;
	uint32_t reserved;
	struct aud_stream_entry entry[]; /* count entries */
};

#define AUD_MAX_OUTPUTS 16

struct aud_output {
	uint32_t id;
	uint32_t type; /* AUDIO_OUT_* from <sys/audio.h> */
	uint32_t present, active;
	char name[32];
	char monitor[32];
};

struct aud_outputs {
	uint32_t count;
	uint32_t mode; /* AUDIO_OUTSEL_* */
	uint32_t selected;
	uint32_t reserved;
	struct aud_output out[AUD_MAX_OUTPUTS];
};

struct aud_set_output {
	uint32_t mode;
	uint32_t id;
};

#define AUD_EVENT_NAME_MAX 32

struct aud_play_event {
	char name[AUD_EVENT_NAME_MAX]; /* "startup", "notify", ... no '/' */
	uint32_t volume; /* 0..100 */
	uint32_t wait; /* 1 = reply only when finished */
};

/* Subscription masks */
#define AUD_SUB_VOLUME (1u << 0)
#define AUD_SUB_OUTPUTS (1u << 1)
#define AUD_SUB_STREAMS (1u << 2)
#define AUD_SUB_CARDS (1u << 3)

struct aud_subscribe {
	uint32_t mask;
	uint32_t reserved;
};

#define AUD_NOTIFY_VOLUME 1 /* a = left, b = right, c = mute */
#define AUD_NOTIFY_OUTPUT 2 /* a = active output id, b = present mask */
#define AUD_NOTIFY_STREAM_NEW 3 /* a = stream id */
#define AUD_NOTIFY_STREAM_GONE 4 /* a = stream id */
#define AUD_NOTIFY_CARD 5 /* a = card index, b = present */

struct aud_notify {
	uint32_t what;
	uint32_t a, b, c;
};

struct aud_server_info {
	uint32_t version;
	uint32_t rate, channels, format; /* mixer output */
	uint32_t period_frames, periods;
	uint32_t card; /* card index in use */
	uint32_t streams; /* streams open now */
	uint64_t uptime_ms;
	uint64_t underruns; /* device underruns since start */
	char card_name[64];
	char codec[64];
	char driver[32];
};

#define AUD_MAX_CARDS 8

struct aud_card_entry {
	uint32_t index;
	uint32_t in_use;
	char name[64];
	char codec[64];
	char driver[32];
};

struct aud_cards {
	uint32_t count;
	uint32_t reserved;
	struct aud_card_entry card[AUD_MAX_CARDS];
};

#ifdef __cplusplus
}
#endif

#endif /* LIKEOS_AUDIO_PROTOCOL_H */
