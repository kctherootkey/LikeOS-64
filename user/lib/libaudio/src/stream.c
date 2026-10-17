// libaudio playback streams: connect to audiod, open a stream, and feed it
// with flow control.
//
// Flow control works by credit.  The service tells the client how many
// bytes it may send: request_bytes in the STREAM_OPEN reply to begin with,
// then one AUD_MSG_REQUEST for every chunk of room that frees up as the
// mixer consumes the stream.  The client never sends more than it holds
// credit for, so the service never has to buffer more than it offered and a
// misbehaving client cannot grow audiod's memory.
//
// Every DATA message carries whole frames only.  A write that ends in the
// middle of a frame leaves the partial frame here, and the next write
// completes it, so callers may hand over any byte count.
//
// Blocking streams wait for credit inside la_write(); non-blocking ones
// (LA_OPT_NONBLOCK) take what fits and return a short count, or -EAGAIN when
// nothing fits.  The socket itself is always non-blocking: a DATA message
// that only partly fits into it is finished later from an internal copy,
// so a non-blocking caller is never held up by a slow service.
//
// SIGPIPE: every send uses MSG_NOSIGNAL, and the LikeOS kernel raises no
// SIGPIPE on sockets at all; a vanished service shows up as -EPIPE.
//
// Copyright (C) 2026 The LikeOS Project

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

#include "la_internal.h"

#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif

/* The longest a drain may take, whatever buffer size the service reports. */
#define LAI_DRAIN_MAX_MS (10 * 60 * 1000)

struct la_stream {
	struct lai_conn conn;

	la_format fmt;
	uint32_t frame_bytes;
	uint32_t bytes_per_sec;
	uint32_t max_chunk; /* AUDIOD_MAX_PAYLOAD rounded down to frames */
	uint32_t flags; /* as given at open: AUD_STREAM_* | LA_OPT_* */
	int nonblock;
	int paused;

	uint32_t stream_id;
	uint32_t buffer_bytes; /* service side buffer */
	uint32_t request_bytes; /* initial credit */

	uint64_t credit; /* bytes the service accepts now */
	uint64_t bytes_sent; /* DATA payload bytes, total */
	uint64_t requests; /* REQUEST messages seen */
	uint32_t underruns; /* UNDERRUN messages seen */

	/* A partial frame left over by the last write. */
	uint8_t frag[LAI_FRAME_MAX];
	uint32_t frag_len;
};

/* ============================================================================
 * Messages from the service
 * ========================================================================== */

static int stream_async(void *ctx, const struct lai_msg *m)
{
	la_stream *s = ctx;

	switch (m->type) {
	case AUD_MSG_REQUEST: {
		struct aud_request rq;

		if (m->len != sizeof(rq))
			return -EPROTO;
		memcpy(&rq, m->payload, sizeof(rq));
		/* Saturating: credit only allows sending, so a confused
		 * service can at worst receive what it asked for. */
		s->credit += rq.bytes;
		if (s->credit > LAI_CREDIT_CAP)
			s->credit = LAI_CREDIT_CAP;
		s->requests++;
		return 1;
	}
	case AUD_MSG_UNDERRUN:
		/* No payload is defined; tolerate a small one, never a
		 * large one. */
		if (m->len > sizeof(struct aud_request) * 2)
			return -EPROTO;
		if (s->underruns != UINT32_MAX)
			s->underruns++;
		return 1;
	default:
		return 0;
	}
}

/* ============================================================================
 * Opening
 * ========================================================================== */

#define LAI_STREAM_FLAGS_ALLOWED                                            \
	(AUD_STREAM_START_PAUSED | AUD_STREAM_NO_RESAMPLE_HINT |            \
	 LA_OPT_NONBLOCK | LA_OPT_VOLUME_ZERO)
#define LAI_STREAM_FLAGS_WIRE                                               \
	(AUD_STREAM_START_PAUSED | AUD_STREAM_NO_RESAMPLE_HINT)
#define LAI_BUFFER_MS_MAX 10000

static int check_format(const la_format *fmt)
{
	if (!fmt)
		return -EINVAL;
	if (fmt->format < 1 || fmt->format > AUD_FMT_MAX ||
	    lai_sample_bytes(fmt->format) == 0)
		return -EINVAL;
	if (fmt->rate < 4000 || fmt->rate > 384000)
		return -EINVAL;
	if (fmt->channels < 1 || fmt->channels > 8)
		return -EINVAL;
	return 0;
}

/* Build the STREAM_OPEN payload from the caller's options. */
static int build_open(const la_format *fmt, const la_stream_options *opt,
		      struct aud_stream_open *o, uint32_t *flags_out)
{
	la_stream_options def;
	const char *sname;
	uint32_t vol;
	int r;

	r = check_format(fmt);
	if (r < 0)
		return r;
	if (!opt) {
		memset(&def, 0, sizeof(def));
		opt = &def;
	}
	if (opt->flags & ~(uint32_t)LAI_STREAM_FLAGS_ALLOWED)
		return -EINVAL;
	if (opt->role > AUD_ROLE_OTHER)
		return -EINVAL;
	if (opt->volume > 100)
		return -EINVAL;

	vol = opt->volume;
	if (vol == 0 && !(opt->flags & LA_OPT_VOLUME_ZERO))
		vol = 100;

	memset(o, 0, sizeof(*o));
	o->format = fmt->format;
	o->rate = fmt->rate;
	o->channels = fmt->channels;
	o->role = opt->role ? opt->role : AUD_ROLE_OTHER;
	o->buffer_ms = opt->buffer_ms > LAI_BUFFER_MS_MAX ? LAI_BUFFER_MS_MAX :
							    opt->buffer_ms;
	o->volume = vol;
	o->flags = opt->flags & LAI_STREAM_FLAGS_WIRE;
	sname = opt->stream_name ? opt->stream_name :
		opt->app_name	 ? opt->app_name :
				   lai_program_name();
	lai_copy_name(o->name, sizeof(o->name), sname);
	*flags_out = opt->flags;
	return 0;
}

static void stream_free(la_stream *s)
{
	if (!s)
		return;
	lai_conn_destroy(&s->conn);
	free(s);
}

la_stream *lai_stream_open_fd(int fd, const la_format *fmt,
			      const la_stream_options *opt, int *err)
{
	struct aud_stream_open o;
	struct aud_stream_opened rep;
	const uint8_t *p;
	uint32_t plen, flags = 0;
	la_stream *s;
	int r;

	r = build_open(fmt, opt, &o, &flags);
	if (r < 0) {
		if (fd >= 0)
			close(fd);
		goto fail_noalloc;
	}
	s = calloc(1, sizeof(*s));
	if (!s) {
		if (fd >= 0)
			close(fd);
		r = -ENOMEM;
		goto fail_noalloc;
	}
	r = lai_conn_init(&s->conn, fd, stream_async, s);
	if (r < 0) {
		free(s);
		goto fail_noalloc;
	}

	s->fmt = *fmt;
	s->frame_bytes = la_frame_bytes(fmt);
	s->bytes_per_sec = s->frame_bytes * fmt->rate; /* <= 64 * 384000 */
	s->max_chunk = (AUDIOD_MAX_PAYLOAD / s->frame_bytes) * s->frame_bytes;
	s->flags = flags;
	s->nonblock = (flags & LA_OPT_NONBLOCK) ? 1 : 0;
	s->paused = (flags & AUD_STREAM_START_PAUSED) ? 1 : 0;

	r = lai_conn_hello(&s->conn, AUD_CLIENT_STREAM,
			   opt ? opt->app_name : NULL);
	if (r < 0)
		goto fail;

	r = lai_conn_request(&s->conn, AUD_MSG_STREAM_OPEN, &o, sizeof(o),
			     AUD_MSG_STREAM_OPEN, sizeof(rep), sizeof(rep), &p,
			     &plen, s->conn.reply_timeout_ms);
	if (r < 0)
		goto fail;
	memcpy(&rep, p, sizeof(rep));
	s->stream_id = rep.stream_id;
	s->buffer_bytes = rep.buffer_bytes;
	s->request_bytes = rep.request_bytes;
	/* Added, not assigned: a REQUEST the service sent right behind its
	 * reply may already have been counted. */
	s->credit += rep.request_bytes;
	if (s->credit > LAI_CREDIT_CAP)
		s->credit = LAI_CREDIT_CAP;
	return s;

fail:
	stream_free(s);
fail_noalloc:
	if (err)
		*err = r;
	return NULL;
}

LA_EXPORT la_stream *la_open(const la_format *fmt,
			     const la_stream_options *opt, int *err)
{
	struct aud_stream_open o;
	uint32_t flags;
	int fd, r;

	if (err)
		*err = 0;
	/* Refuse bad parameters before touching the service. */
	r = build_open(fmt, opt, &o, &flags);
	if (r < 0) {
		if (err)
			*err = r;
		return NULL;
	}
	r = lai_connect(AUDIOD_SOCKET_PATH, &fd);
	if (r < 0) {
		if (err)
			*err = r;
		return NULL;
	}
	return lai_stream_open_fd(fd, fmt, opt, err);
}

LA_EXPORT la_stream *la_open_simple(uint32_t format, uint32_t rate,
				    uint32_t channels, const char *name,
				    int *err)
{
	la_format f;
	la_stream_options o;

	memset(&f, 0, sizeof(f));
	memset(&o, 0, sizeof(o));
	f.format = format;
	f.rate = rate;
	f.channels = channels;
	o.role = AUD_ROLE_OTHER;
	o.stream_name = name;
	return la_open(&f, &o, err);
}

/* ============================================================================
 * Writing
 * ========================================================================== */

/* Common entry checks.  0 when the handle may be used. */
static int stream_usable(la_stream *s)
{
	int e;

	if (!s)
		return -EBADF;
	if (s->conn.dead)
		return s->conn.dead;
	e = lai_conn_take_async_err(&s->conn);
	if (e)
		return e;
	return 0;
}

/* Send one DATA message of `chunk' bytes: the stashed partial frame first,
 * then bytes from the caller.  `chunk' is a whole number of frames and at
 * least one frame, so the stash always fits in front of it. */
static int send_chunk(la_stream *s, const uint8_t *src, size_t chunk,
		      size_t *from_data)
{
	struct iovec v[2];
	int n = 0, r;
	size_t fd_bytes = s->frag_len;

	if (fd_bytes) {
		v[n].iov_base = s->frag;
		v[n].iov_len = fd_bytes;
		n++;
	}
	if (chunk > fd_bytes) {
		v[n].iov_base = (void *)src;
		v[n].iov_len = chunk - fd_bytes;
		n++;
	}
	r = lai_conn_sendv(&s->conn, AUD_MSG_DATA, v, n,
			   s->nonblock ? 0 : LAI_STALL_TIMEOUT_MS);
	if (r < 0)
		return r;
	*from_data = chunk - fd_bytes;
	s->frag_len = 0;
	s->credit -= chunk;
	s->bytes_sent += chunk;
	return 0;
}

LA_EXPORT ssize_t la_write(la_stream *s, const void *data, size_t bytes)
{
	const uint8_t *src = data;
	struct lai_conn *c;
	uint64_t last_progress;
	size_t done = 0;
	int r;

	r = stream_usable(s);
	if (r < 0)
		return r;
	if (bytes && !data)
		return -EINVAL;
	if (bytes == 0)
		return 0;
	if (bytes > (size_t)SSIZE_MAX)
		bytes = (size_t)SSIZE_MAX;
	c = &s->conn;

	/* Pick up credit that arrived since the last call; finish an old
	 * message if possible. */
	r = lai_conn_flush(c, 0);
	if (r < 0 && r != -EAGAIN)
		return r;
	if (r == -EAGAIN && s->nonblock)
		return -EAGAIN;
	r = lai_conn_pump(c);
	if (r < 0)
		return r;

	last_progress = lai_now_ms();
	while (done < bytes) {
		size_t fb = s->frame_bytes;
		size_t avail = s->frag_len + (bytes - done);
		size_t whole = (avail / fb) * fb;
		size_t chunk, taken = 0;
		uint64_t credit = (s->credit / fb) * fb;

		if (whole == 0) {
			/* Not even one frame: keep it for the next call.
			 * avail < fb <= LAI_FRAME_MAX, so it fits. */
			memcpy(s->frag + s->frag_len, src + done, bytes - done);
			s->frag_len += (uint32_t)(bytes - done);
			done = bytes;
			break;
		}
		chunk = whole;
		if (chunk > s->max_chunk)
			chunk = s->max_chunk;
		if ((uint64_t)chunk > credit)
			chunk = (size_t)credit;

		if (chunk > 0) {
			r = send_chunk(s, src + done, chunk, &taken);
			if (r == -EAGAIN)
				break; /* non-blocking: the socket is full */
			if (r < 0)
				return done ? (ssize_t)done : r;
			done += taken;
			last_progress = lai_now_ms();
			continue;
		}

		/* No credit. */
		if (s->nonblock)
			break;
		{
			int wait_ms;
			uint64_t idle = lai_now_ms() - last_progress;

			if (s->paused) {
				/* A paused stream gets credit back only once
				 * it is resumed; that may take as long as the
				 * user likes.  Wake up now and then anyway. */
				wait_ms = 1000;
			} else {
				if (idle >= LAI_STALL_TIMEOUT_MS)
					return done ? (ssize_t)done :
						      -ETIMEDOUT;
				wait_ms = (int)(LAI_STALL_TIMEOUT_MS - idle);
			}
			r = lai_conn_wait(c, wait_ms);
			if (r < 0)
				return done ? (ssize_t)done : r;
			if (c->async_err)
				return done ? (ssize_t)done :
					      lai_conn_take_async_err(c);
		}
	}
	if (done == 0)
		return -EAGAIN;
	return (ssize_t)done;
}

LA_EXPORT ssize_t la_writable(la_stream *s)
{
	uint64_t credit;
	int r;

	r = stream_usable(s);
	if (r < 0)
		return r;
	r = lai_conn_flush(&s->conn, 0);
	if (r < 0 && r != -EAGAIN)
		return r;
	r = lai_conn_pump(&s->conn);
	if (r < 0)
		return r;
	if (lai_conn_tx_pending(&s->conn))
		return 0;
	credit = (s->credit / s->frame_bytes) * s->frame_bytes;
	if (credit <= s->frag_len)
		return 0;
	credit -= s->frag_len;
	if (credit > (uint64_t)SSIZE_MAX)
		credit = (uint64_t)SSIZE_MAX;
	return (ssize_t)credit;
}

/* ============================================================================
 * Stream control
 * ========================================================================== */

/* Before a control request goes out, the DATA message in flight must be
 * finished; a stream request must never land inside one. */
static int finish_data(la_stream *s)
{
	return lai_conn_flush(&s->conn, LAI_STALL_TIMEOUT_MS);
}

static int simple_request(la_stream *s, uint32_t type, const void *req,
			  size_t len, int timeout_ms)
{
	int r = stream_usable(s);

	if (r < 0)
		return r;
	r = finish_data(s);
	if (r < 0)
		return r;
	return lai_conn_request(&s->conn, type, req, len, AUD_MSG_OK, 0, 0,
				NULL, NULL, timeout_ms);
}

/* How long a drain may take: twice the time the service's buffer plays
 * for, plus slack for the device's own queue. */
static int drain_timeout_ms(const la_stream *s)
{
	uint64_t buffered = (uint64_t)s->buffer_bytes + s->request_bytes;
	uint64_t ms;

	if (s->bytes_per_sec == 0)
		return LAI_DRAIN_SLACK_MS;
	ms = buffered * 1000u / s->bytes_per_sec;
	ms = ms * 2 + LAI_DRAIN_SLACK_MS;
	if (ms > LAI_DRAIN_MAX_MS)
		ms = LAI_DRAIN_MAX_MS;
	return (int)ms;
}

LA_EXPORT int la_drain(la_stream *s)
{
	int r = stream_usable(s);

	if (r < 0)
		return r;
	r = finish_data(s);
	if (r < 0)
		return r;
	/* A trailing partial frame can never be played. */
	s->frag_len = 0;
	return lai_conn_request(&s->conn, AUD_MSG_DRAIN, NULL, 0, AUD_MSG_OK,
				0, 0, NULL, NULL, drain_timeout_ms(s));
}

LA_EXPORT int la_flush(la_stream *s)
{
	int r = simple_request(s, AUD_MSG_FLUSH, NULL, 0,
			       s ? s->conn.reply_timeout_ms : 0);

	if (r == 0)
		s->frag_len = 0;
	return r;
}

LA_EXPORT int la_pause(la_stream *s)
{
	int r = simple_request(s, AUD_MSG_PAUSE, NULL, 0,
			       s ? s->conn.reply_timeout_ms : 0);

	if (r == 0)
		s->paused = 1;
	return r;
}

LA_EXPORT int la_resume(la_stream *s)
{
	int r = simple_request(s, AUD_MSG_RESUME, NULL, 0,
			       s ? s->conn.reply_timeout_ms : 0);

	if (r == 0)
		s->paused = 0;
	return r;
}

LA_EXPORT int la_set_volume(la_stream *s, uint32_t left, uint32_t right)
{
	struct aud_volume v;

	if (!s)
		return -EBADF;
	if (left > 100 || right > 100)
		return -EINVAL;
	memset(&v, 0, sizeof(v));
	v.left = left;
	v.right = right;
	return simple_request(s, AUD_MSG_SET_STREAM_VOLUME, &v, sizeof(v),
			      s->conn.reply_timeout_ms);
}

LA_EXPORT int la_set_mute(la_stream *s, int mute)
{
	struct aud_mute m;

	if (!s)
		return -EBADF;
	memset(&m, 0, sizeof(m));
	m.mute = mute ? 1 : 0;
	return simple_request(s, AUD_MSG_SET_STREAM_MUTE, &m, sizeof(m),
			      s->conn.reply_timeout_ms);
}

LA_EXPORT int la_latency(la_stream *s, uint64_t *latency_us,
			 uint64_t *played_frames)
{
	struct aud_latency l;
	const uint8_t *p;
	uint32_t plen;
	uint64_t local, extra_us;
	int r;

	r = stream_usable(s);
	if (r < 0)
		return r;
	/* No finish_data() here: the latency is asked for often, from
	 * timing-critical code, and a DATA tail still waiting in our own
	 * buffer is simply added below.  The request itself queues behind
	 * that tail and the send finishes it first. */
	r = lai_conn_request(&s->conn, AUD_MSG_GET_LATENCY, NULL, 0,
			     AUD_MSG_GET_LATENCY, sizeof(l), sizeof(l), &p,
			     &plen, s->conn.reply_timeout_ms);
	if (r < 0)
		return r;
	memcpy(&l, p, sizeof(l));

	/* What is still on this side counts too: the unsent part of a DATA
	 * message and a partial frame. */
	local = (uint64_t)lai_conn_tx_data_pending(&s->conn) + s->frag_len;
	extra_us = s->bytes_per_sec ? local * 1000000u / s->bytes_per_sec : 0;
	if (latency_us) {
		uint64_t v = l.latency_us;

		*latency_us = (v > UINT64_MAX - extra_us) ? UINT64_MAX :
							    v + extra_us;
	}
	if (played_frames)
		*played_frames = l.played_frames;
	if (l.underruns > s->underruns)
		s->underruns = l.underruns;
	return 0;
}

LA_EXPORT int la_fd(la_stream *s)
{
	if (!s)
		return -EBADF;
	if (s->conn.fd < 0)
		return -EBADF;
	return s->conn.fd;
}

LA_EXPORT int la_process(la_stream *s)
{
	int r;

	r = stream_usable(s);
	if (r < 0)
		return r;
	r = lai_conn_flush(&s->conn, 0);
	if (r < 0 && r != -EAGAIN)
		return r;
	r = lai_conn_pump(&s->conn);
	if (r < 0)
		return r;
	return lai_conn_take_async_err(&s->conn);
}

LA_EXPORT uint32_t la_stream_id(la_stream *s)
{
	return s ? s->stream_id : 0;
}

LA_EXPORT void la_close(la_stream *s)
{
	if (!s)
		return;
	if (!s->conn.dead) {
		/* Best effort and briefly: finish a DATA message in flight
		 * (the service would otherwise see a truncated message, which
		 * is harmless but noisy), then say goodbye.  No reply is
		 * waited for; closing the socket ends the stream anyway. */
		if (lai_conn_flush(&s->conn, LAI_CLOSE_TIMEOUT_MS) == 0)
			(void)lai_conn_send(&s->conn, AUD_MSG_STREAM_CLOSE,
					    NULL, 0, LAI_CLOSE_TIMEOUT_MS);
	}
	stream_free(s);
}

struct lai_conn *lai_stream_conn(la_stream *s)
{
	return s ? &s->conn : NULL;
}

LA_EXPORT const char *la_stream_errmsg(la_stream *s)
{
	if (!s)
		return "";
	return s->conn.err_msg;
}
