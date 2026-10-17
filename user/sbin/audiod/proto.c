// audiod wire protocol: message framing and payload validation.
//
// The rule is to judge a message by its header before reading its body.
// A header announcing more than AUDIOD_MAX_PAYLOAD bytes, a type the
// client has no business sending, or a length that does not match the
// fixed structure of its type ends the connection without a single payload
// byte being buffered.  The payload checks then validate every field;
// names must be terminated inside their field and never reach the logs or
// other clients with control characters in them.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "parse.h"

#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif

/* ---- type table ------------------------------------------------------------ */

#define K_STREAM 1
#define K_CONTROL 2
#define K_ANY 3

struct type_desc {
	uint32_t type;
	const char *name;
	uint32_t min, max; /* payload length range */
	int kind;
};

#define FIXED(t, n, sz, k) { t, n, (uint32_t)(sz), (uint32_t)(sz), k }

static const struct type_desc types[] = {
	FIXED(AUD_MSG_HELLO, "HELLO", sizeof(struct aud_hello), K_ANY),
	FIXED(AUD_MSG_STREAM_OPEN, "STREAM_OPEN",
	      sizeof(struct aud_stream_open), K_STREAM),
	{ AUD_MSG_DATA, "DATA", 0, AUDIOD_MAX_PAYLOAD, K_STREAM },
	FIXED(AUD_MSG_DRAIN, "DRAIN", 0, K_STREAM),
	FIXED(AUD_MSG_FLUSH, "FLUSH", 0, K_STREAM),
	FIXED(AUD_MSG_PAUSE, "PAUSE", 0, K_STREAM),
	FIXED(AUD_MSG_RESUME, "RESUME", 0, K_STREAM),
	FIXED(AUD_MSG_SET_STREAM_VOLUME, "SET_STREAM_VOLUME",
	      sizeof(struct aud_volume), K_STREAM),
	FIXED(AUD_MSG_GET_LATENCY, "GET_LATENCY", 0, K_STREAM),
	FIXED(AUD_MSG_STREAM_CLOSE, "STREAM_CLOSE", 0, K_STREAM),
	FIXED(AUD_MSG_SET_STREAM_MUTE, "SET_STREAM_MUTE",
	      sizeof(struct aud_mute), K_STREAM),
	FIXED(AUD_MSG_GET_SERVER_INFO, "GET_SERVER_INFO", 0, K_ANY),
	FIXED(AUD_MSG_LIST_STREAMS, "LIST_STREAMS", 0, K_CONTROL),
	FIXED(AUD_MSG_GET_MASTER_VOLUME, "GET_MASTER_VOLUME", 0, K_CONTROL),
	FIXED(AUD_MSG_SET_MASTER_VOLUME, "SET_MASTER_VOLUME",
	      sizeof(struct aud_volume), K_CONTROL),
	FIXED(AUD_MSG_GET_MUTE, "GET_MUTE", 0, K_CONTROL),
	FIXED(AUD_MSG_SET_MUTE, "SET_MUTE", sizeof(struct aud_mute),
	      K_CONTROL),
	FIXED(AUD_MSG_GET_OUTPUTS, "GET_OUTPUTS", 0, K_CONTROL),
	FIXED(AUD_MSG_SET_OUTPUT, "SET_OUTPUT", sizeof(struct aud_set_output),
	      K_CONTROL),
	FIXED(AUD_MSG_PLAY_EVENT, "PLAY_EVENT", sizeof(struct aud_play_event),
	      K_CONTROL),
	FIXED(AUD_MSG_SUBSCRIBE, "SUBSCRIBE", sizeof(struct aud_subscribe),
	      K_CONTROL),
	FIXED(AUD_MSG_SET_VOLUME_OF, "SET_VOLUME_OF",
	      sizeof(struct aud_volume_of), K_CONTROL),
	FIXED(AUD_MSG_KILL_STREAM, "KILL_STREAM", sizeof(struct aud_stream_ref),
	      K_CONTROL),
	FIXED(AUD_MSG_GET_CARDS, "GET_CARDS", 0, K_CONTROL),
	FIXED(AUD_MSG_SET_CARD, "SET_CARD", sizeof(struct aud_stream_ref),
	      K_CONTROL),
};

/* server -> client types, for log messages only */
static const struct {
	uint32_t type;
	const char *name;
} reply_names[] = {
	{ AUD_MSG_ERROR, "ERROR" },	    { AUD_MSG_OK, "OK" },
	{ AUD_MSG_REQUEST, "REQUEST" },	    { AUD_MSG_UNDERRUN, "UNDERRUN" },
	{ AUD_MSG_NOTIFY, "NOTIFY" },
};

static const struct type_desc *find_type(uint32_t type)
{
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		if (types[i].type == type)
			return &types[i];
	return NULL;
}

int proto_len_range(uint32_t type, uint32_t *min, uint32_t *max)
{
	const struct type_desc *d = find_type(type);
	if (!d)
		return -1;
	*min = d->min;
	*max = d->max;
	return 0;
}

int proto_kind_of(uint32_t type)
{
	const struct type_desc *d = find_type(type);
	return d ? d->kind : 0;
}

const char *proto_type_name(uint32_t type)
{
	const struct type_desc *d = find_type(type);
	if (d)
		return d->name;
	for (size_t i = 0; i < sizeof(reply_names) / sizeof(reply_names[0]);
	     i++)
		if (reply_names[i].type == type)
			return reply_names[i].name;
	return "unknown";
}

/* ---- framing ---------------------------------------------------------------- */

void mr_init(struct msg_reader *mr)
{
	memset(mr, 0, sizeof(*mr));
}

void mr_free(struct msg_reader *mr)
{
	free(mr->payload);
	memset(mr, 0, sizeof(*mr));
}

void mr_next(struct msg_reader *mr)
{
	/* The payload buffer is kept: at most AUDIOD_MAX_PAYLOAD per
	 * connection, and a streaming client needs it for every DATA. */
	mr->hdr_got = 0;
	mr->have_header = 0;
	mr->got = 0;
	memset(&mr->h, 0, sizeof(mr->h));
}

size_t mr_partial(const struct msg_reader *mr)
{
	return mr->hdr_got + mr->got;
}

static int check_header(struct msg_reader *mr)
{
	uint32_t min, max;
	if (mr->h.len > AUDIOD_MAX_PAYLOAD) {
		mr->error = "payload too large";
		return MR_ERROR;
	}
	if (proto_len_range(mr->h.type, &min, &max) < 0) {
		mr->error = "unknown or server-only message type";
		return MR_ERROR;
	}
	if (mr->h.len < min || mr->h.len > max) {
		mr->error = "payload length does not match the message type";
		return MR_ERROR;
	}
	if (mr->h.len > mr->cap) {
		/* grow to the exact size needed; DATA gets the full maximum
		 * at once so a streaming client allocates only once */
		size_t want = mr->h.type == AUD_MSG_DATA ? AUDIOD_MAX_PAYLOAD
							 : mr->h.len;
		uint8_t *p = realloc(mr->payload, want);
		if (!p) {
			mr->error = "out of memory";
			return MR_ERROR;
		}
		mr->payload = p;
		mr->cap = want;
	}
	return MR_NEED_MORE;
}

int mr_feed(struct msg_reader *mr, const uint8_t *data, size_t len,
	    size_t *used)
{
	size_t off = 0;

	*used = 0;
	if (!mr->have_header) {
		size_t need = sizeof(mr->hdr) - mr->hdr_got;
		size_t take = len < need ? len : need;
		memcpy(mr->hdr + mr->hdr_got, data, take);
		mr->hdr_got += take;
		off += take;
		if (mr->hdr_got < sizeof(mr->hdr)) {
			*used = off;
			return MR_NEED_MORE;
		}
		memcpy(&mr->h, mr->hdr, sizeof(mr->h));
		mr->have_header = 1;
		mr->got = 0;
		if (check_header(mr) == MR_ERROR) {
			*used = off;
			return MR_ERROR;
		}
	}

	size_t need = mr->h.len - mr->got;
	size_t take = (len - off) < need ? (len - off) : need;
	if (take) {
		memcpy(mr->payload + mr->got, data + off, take);
		mr->got += take;
		off += take;
	}
	*used = off;
	return mr->got == mr->h.len ? MR_MESSAGE : MR_NEED_MORE;
}

/* ---- payload checks ---------------------------------------------------------- */

int proto_copy_name(char *dst, const char *src, size_t field)
{
	size_t n = 0;
	while (n < field && src[n] != '\0')
		n++;
	if (n == field) {
		dst[0] = '\0';
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)src[i];
		dst[i] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
	}
	dst[n] = '\0';
	return 0;
}

int proto_check_hello(const struct aud_hello *h, char name[AUDIOD_NAME_MAX])
{
	if (proto_copy_name(name, h->name, sizeof(h->name)) < 0)
		return -EBADMSG;
	if (h->version != AUDIOD_PROTOCOL_VERSION)
		return -EPROTONOSUPPORT;
	if (h->kind != AUD_CLIENT_STREAM && h->kind != AUD_CLIENT_CONTROL)
		return -EINVAL;
	return 0;
}

#define KNOWN_STREAM_FLAGS (AUD_STREAM_START_PAUSED | AUD_STREAM_NO_RESAMPLE_HINT)

int proto_check_stream_open(const struct aud_stream_open *o,
			    struct stream_req *out)
{
	memset(out, 0, sizeof(*out));
	if (proto_copy_name(out->name, o->name, sizeof(o->name)) < 0)
		return -EBADMSG;
	out->fmt = sf_from_proto(o->format);
	if (out->fmt == SF_NONE)
		return -EINVAL;
	if (o->rate < PROTO_MIN_RATE || o->rate > PROTO_MAX_RATE)
		return -EINVAL;
	if (o->channels < 1 || o->channels > DSP_MAX_CH)
		return -EINVAL;
	/* role 0 is taken as "other": a zero-initialised request is not
	 * worth refusing over it */
	if (o->role > AUD_ROLE_OTHER)
		return -EINVAL;
	if (o->buffer_ms > PROTO_MAX_BUFFER_MS)
		return -EINVAL;
	if (o->volume > 100)
		return -EINVAL;
	if (o->flags & ~(uint32_t)KNOWN_STREAM_FLAGS)
		return -EINVAL;
	out->aud_fmt = o->format;
	out->rate = o->rate;
	out->channels = o->channels;
	out->role = o->role ? o->role : AUD_ROLE_OTHER;
	out->buffer_ms = o->buffer_ms;
	out->volume = o->volume;
	out->flags = o->flags;
	return 0;
}

int proto_check_volume(const struct aud_volume *v)
{
	if (v->left > 100 || v->right > 100)
		return -EINVAL;
	return 0;
}

int proto_check_mute(const struct aud_mute *m)
{
	if (m->mute > 1)
		return -EINVAL;
	return 0;
}

int proto_check_set_output(const struct aud_set_output *s)
{
	if (s->mode != OUTSEL_AUTO && s->mode != OUTSEL_ID &&
	    s->mode != OUTSEL_ALL)
		return -EINVAL;
	/* output ids are the device's own (HDMI/DP ports from 0x100 up);
	 * the caller checks that the id exists */
	if (s->mode == OUTSEL_ID && s->id > 0xffff)
		return -EINVAL;
	return 0;
}

int proto_check_subscribe(const struct aud_subscribe *s)
{
	const uint32_t all =
		AUD_SUB_VOLUME | AUD_SUB_OUTPUTS | AUD_SUB_STREAMS | AUD_SUB_CARDS;
	if (s->mask & ~all)
		return -EINVAL;
	return 0;
}

int event_name_valid(const char *name)
{
	size_t n = 0;
	if (!name || !name[0])
		return 0;
	for (; name[n]; n++) {
		char c = name[n];
		if (n >= AUD_EVENT_NAME_MAX - 1)
			return 0;
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		      c == '_' || c == '-'))
			return 0;
	}
	if (name[0] == '-')
		return 0; /* would read as an option to anything shown it */
	/* '/' and '.' are outside the alphabet, so neither a path nor ".."
	 * can be formed; checked once more for clarity */
	if (strstr(name, "..") || strchr(name, '/'))
		return 0;
	return 1;
}

int proto_check_play_event(const struct aud_play_event *e,
			   char name[AUD_EVENT_NAME_MAX])
{
	/* raw copy without sanitising: the name must be valid as sent */
	size_t n = 0;
	while (n < sizeof(e->name) && e->name[n] != '\0')
		n++;
	if (n == sizeof(e->name)) {
		name[0] = '\0';
		return -EBADMSG;
	}
	memcpy(name, e->name, n);
	name[n] = '\0';
	if (!event_name_valid(name))
		return -EINVAL;
	if (e->volume > 100 || e->wait > 1)
		return -EINVAL;
	return 0;
}
