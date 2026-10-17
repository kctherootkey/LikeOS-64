// audiod input parsing: the client wire protocol framing and payload
// checks (proto.c) and the WAV reader for event sounds (wav.c).
//
// Both are pure functions over byte buffers with no system dependencies,
// so the host test harness runs them under the address and undefined
// behaviour sanitizers with hostile input.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef AUDIOD_PARSE_H
#define AUDIOD_PARSE_H

#include <stddef.h>
#include <stdint.h>

#include <audio/protocol.h>

/* ---- message framing -------------------------------------------------- */

#define MR_NEED_MORE 0
#define MR_MESSAGE 1
#define MR_ERROR (-1)

/* Incremental reader for one connection: bytes go in as they arrive, a
 * complete message comes out.  The header is checked before a single
 * payload byte is accepted: unknown types, types only the server sends,
 * and lengths that do not match the type (or exceed AUDIOD_MAX_PAYLOAD)
 * are errors, and the connection is to be dropped. */
struct msg_reader {
	struct aud_header h;
	uint8_t hdr[sizeof(struct aud_header)];
	size_t hdr_got;
	int have_header;
	uint8_t *payload;
	size_t cap;
	size_t got;
	const char *error; /* why MR_ERROR was returned */
};

void mr_init(struct msg_reader *mr);
void mr_free(struct msg_reader *mr);
/* Consume bytes from `data'; stops right after a complete message.
 * `*used' is set to the bytes consumed in every case. */
int mr_feed(struct msg_reader *mr, const uint8_t *data, size_t len,
	    size_t *used);
/* Done with the current message: prepare for the next header. */
void mr_next(struct msg_reader *mr);
/* Bytes held for an incomplete message (0 at a message boundary). */
size_t mr_partial(const struct msg_reader *mr);

/* Allowed payload length range for a client->server type; -1 if the
 * type may not be sent by a client at all. */
int proto_len_range(uint32_t type, uint32_t *min, uint32_t *max);
/* Does this request belong on a stream (1), control (2) or either (3)
 * connection?  0 for anything else. */
int proto_kind_of(uint32_t type);
const char *proto_type_name(uint32_t type);

/* ---- payload checks ------------------------------------------------------ */

/* Copy a fixed-size, NUL-terminated name field.  The terminator must lie
 * inside the field (-1 otherwise); control characters become '?'. */
int proto_copy_name(char *dst, const char *src, size_t field);

/* 0 ok; -EPROTONOSUPPORT wrong version; -EINVAL bad kind; -EBADMSG name */
int proto_check_hello(const struct aud_hello *h, char name[AUDIOD_NAME_MAX]);

struct stream_req {
	int fmt; /* SF_* */
	uint32_t aud_fmt;
	uint32_t rate;
	uint32_t channels;
	uint32_t role;
	uint32_t buffer_ms;
	uint32_t volume;
	uint32_t flags;
	char name[AUDIOD_NAME_MAX];
};

#define PROTO_MIN_RATE 4000u
#define PROTO_MAX_RATE 384000u
#define PROTO_MAX_BUFFER_MS 10000u

/* 0 ok; -EBADMSG name not terminated (protocol violation); -EINVAL a
 * value out of range (a refusable request). */
int proto_check_stream_open(const struct aud_stream_open *o,
			    struct stream_req *out);
int proto_check_volume(const struct aud_volume *v);
int proto_check_mute(const struct aud_mute *m);

/* Output selection modes, numerically equal to AUDIO_OUTSEL_*. */
#define OUTSEL_AUTO 0
#define OUTSEL_ID 1
#define OUTSEL_ALL 2
int proto_check_set_output(const struct aud_set_output *s);
int proto_check_subscribe(const struct aud_subscribe *s);

/* Event names: 1..31 characters of [a-z0-9_-], not starting with '-'. */
int event_name_valid(const char *name);
/* 0 ok, -EBADMSG unterminated, -EINVAL bad name or values */
int proto_check_play_event(const struct aud_play_event *e,
			   char name[AUD_EVENT_NAME_MAX]);

/* ---- WAV ----------------------------------------------------------------- */

#define WAV_MAX_BYTES (16u * 1024u * 1024u)

struct wav_info {
	int fmt; /* SF_* */
	uint32_t rate;
	uint32_t channels;
	uint32_t block_align;
	uint32_t bits;
	const uint8_t *data; /* points into the parsed buffer */
	size_t data_len; /* whole frames only */
	size_t frames;
};

/* Parse a RIFF/WAVE file held in memory.  Accepts PCM 8/16/24/32 bit,
 * IEEE float 32/64 bit and the extensible header with those sub-formats,
 * 1..8 channels, 4000..384000 Hz.  Every chunk is bounds-checked; a short
 * or overlong data chunk is clipped to the file.  0 or a negative errno. */
int wav_parse(const uint8_t *buf, size_t len, struct wav_info *out);

#endif /* AUDIOD_PARSE_H */
