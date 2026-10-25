// audiod clients: the listening socket, per-connection state machines and
// the streams that stream connections own.
//
// Every connection is non-blocking.  Input is read into a fixed buffer and
// fed through the framing reader (proto.c), which rejects a message by its
// header before buffering any of it.  A connection says HELLO first and
// from then on is either a stream connection or a control connection; any
// message outside its kind, any malformed payload and any DATA beyond the
// granted credit closes it.  Requests that are well-formed but cannot be
// granted (limits, bad values) get an ERROR reply instead.
//
// Flow control: a stream's ring holds buffer_bytes.  The client starts
// with that much credit and may never have more bytes in flight than the
// ring has room for; as the mixer drains the ring, the freed room is handed
// back in REQUEST messages of at least request_min bytes.  So the ring
// never overflows and the daemon never has to buffer or drop client data.
//
// Replies are queued per connection (bounded); a client that stops reading
// its replies is dropped rather than allowed to grow the daemon.  While a
// DRAIN or a waited-for event sound is outstanding, the connection's input
// is not read, which keeps replies in request order.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "audiod.h"

/* ---- per-uid accounting ---------------------------------------------------- */

struct uid_usage *uid_get(uint32_t uid, int create)
{
	struct uid_usage *free_slot = NULL;
	for (int i = 0; i < MAX_UIDS; i++) {
		struct uid_usage *u = &srv.uids[i];
		if (u->uid == uid && (u->clients || u->streams || u->bytes ||
				      u->ev_refill_ms))
			return u;
		if (!free_slot && !u->clients && !u->streams && !u->bytes &&
		    !u->ev_refill_ms)
			free_slot = u;
	}
	if (create && !free_slot) {
		/* recycle a slot that only remembers an event-sound bucket */
		for (int i = 0; i < MAX_UIDS; i++) {
			struct uid_usage *u = &srv.uids[i];
			if (!u->clients && !u->streams && !u->bytes) {
				free_slot = u;
				break;
			}
		}
	}
	if (!create || !free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->uid = uid;
	return free_slot;
}

void uid_put_if_idle(struct uid_usage *u)
{
	if (!u)
		return;
	if (!u->clients && !u->streams && !u->bytes) {
		/* keep the event-sound bucket alive for a while so that
		 * reconnecting does not refill it */
		if (u->ev_refill_ms && now_ms() - u->ev_refill_ms > 60000)
			memset(u, 0, sizeof(*u));
	}
}

/* ---- listening socket ------------------------------------------------------ */

int clients_listen(const char *path)
{
	struct sockaddr_un sa;
	struct stat st;

	if (strlen(path) >= sizeof(sa.sun_path)) {
		log_err("socket path %s is too long", path);
		return -ENAMETOOLONG;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, path, strlen(path) + 1);

	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			log_err("%s exists and is not a socket; refusing to "
				"remove it",
				path);
			return -EEXIST;
		}
		/* someone listening there already? */
		int t = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (t >= 0) {
			if (connect(t, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
				close(t);
				log_err("another audiod is already running");
				return -EADDRINUSE;
			}
			close(t);
		}
		if (unlink(path) < 0 && errno != ENOENT) {
			log_err("cannot remove stale %s: %s", path,
				strerror(errno));
			return -errno;
		}
	}

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		int e = errno;
		log_err("socket: %s", strerror(e));
		return -e;
	}
	/* in case the flag was not honoured */
	int fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = errno;
		log_err("bind %s: %s", path, strerror(e));
		close(fd);
		return -e;
	}
	/* any local user may play sound */
	if (chmod(path, 0666) < 0)
		log_warn("chmod %s: %s", path, strerror(errno));
	if (listen(fd, 32) < 0) {
		int e = errno;
		log_err("listen: %s", strerror(e));
		close(fd);
		unlink(path);
		return -e;
	}
	return fd;
}

/* ---- client lookup and lifetime --------------------------------------------- */

struct client *client_by_id(uint32_t id)
{
	for (struct client *c = srv.clients; c; c = c->next)
		if (c->id == id && !c->dead)
			return c;
	return NULL;
}

struct stream *stream_by_id(uint32_t id)
{
	for (struct stream *s = srv.streams; s; s = s->next)
		if (s->id == id && !s->done)
			return s;
	return NULL;
}

static uint32_t next_id(uint32_t *ctr)
{
	uint32_t id = ++*ctr;
	if (id == 0)
		id = ++*ctr;
	return id;
}

void clients_accept(void)
{
	for (int i = 0; i < 32; i++) {
		int fd = accept4(srv.listen_fd, NULL, NULL,
				 SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != ECONNABORTED)
				log_warn("accept: %s", strerror(errno));
			return;
		}
		/* The socket must be non-blocking: sends ignore
		 * MSG_DONTWAIT, so the descriptor flag is what counts. */
		int fl = fcntl(fd, F_GETFL);
		if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
			close(fd);
			continue;
		}

		struct ucred cred;
		socklen_t clen = sizeof(cred);
		memset(&cred, 0, sizeof(cred));
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 ||
		    clen < sizeof(cred)) {
			log_warn("client without peer credentials refused: %s",
				 strerror(errno));
			close(fd);
			continue;
		}

		uint32_t maxc = srv.cfg.max_clients;
		if (maxc > HARD_MAX_CLIENTS)
			maxc = HARD_MAX_CLIENTS;
		if (srv.nclients >= maxc) {
			log_warn("client limit (%u) reached; uid %u pid %d "
				 "refused",
				 maxc, cred.uid, (int)cred.pid);
			close(fd);
			continue;
		}
		struct uid_usage *u = uid_get(cred.uid, 1);
		uint32_t per_uid = maxc * 3 / 4;
		if (per_uid < 4)
			per_uid = 4;
		if (!u || (cred.uid != 0 && u->clients >= per_uid)) {
			log_warn("uid %u has too many connections; pid %d "
				 "refused",
				 cred.uid, (int)cred.pid);
			close(fd);
			continue;
		}

		struct client *c = calloc(1, sizeof(*c));
		if (!c) {
			close(fd);
			continue;
		}
		c->fd = fd;
		c->id = next_id(&srv.next_client_id);
		c->cred = cred;
		mr_init(&c->mr);
		c->accepted_ms = c->last_rx_ms = now_ms();
		c->next = srv.clients;
		srv.clients = c;
		srv.nclients++;
		u->clients++;
		log_debug("client %u connected (uid %u pid %d)", c->id, cred.uid,
			  (int)cred.pid);
	}
}

static void client_free(struct client *c)
{
	if (c->stream) {
		c->stream->owner = NULL;
		c->stream->done = 1;
		c->stream = NULL;
	}
	if (c->fd >= 0)
		close(c->fd);
	mr_free(&c->mr);
	free(c->out);
	struct uid_usage *u = uid_get(c->cred.uid, 0);
	if (u && u->clients)
		u->clients--;
	uid_put_if_idle(u);
	free(c);
}

void client_close(struct client *c, const char *why)
{
	if (c->dead)
		return;
	if (why)
		log_info("client %u (%s, uid %u pid %d): closing: %s", c->id,
			 c->name[0] ? c->name : "?", c->cred.uid,
			 (int)c->cred.pid, why);
	c->dead = 1;
	if (c->stream) {
		/* removed at the next reap; stop mixing it now */
		c->stream->done = 1;
	}
	/* release the socket at once so the peer sees EOF */
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

/* ---- sending ------------------------------------------------------------------ */

static int client_flush(struct client *c)
{
	while (c->out_len) {
		ssize_t w = send(c->fd, c->out + c->out_off, c->out_len,
				 MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 1;
			client_close(c, "connection lost while sending");
			return -1;
		}
		if (w == 0)
			return 1;
		c->out_off += (size_t)w;
		c->out_len -= (size_t)w;
	}
	c->out_off = 0;
	return 0;
}

static int queue_bytes(struct client *c, const uint8_t *p, size_t n)
{
	if (c->out_off + c->out_len + n > c->out_cap) {
		/* compact first */
		if (c->out_off) {
			memmove(c->out, c->out + c->out_off, c->out_len);
			c->out_off = 0;
		}
		if (c->out_len + n > c->out_cap) {
			size_t want = c->out_cap ? c->out_cap : 4096;
			while (want < c->out_len + n)
				want *= 2;
			if (want > CLIENT_OUTQ_MAX) {
				client_close(c, "not reading its replies");
				return -1;
			}
			uint8_t *nb = realloc(c->out, want);
			if (!nb) {
				client_close(c, "out of memory");
				return -1;
			}
			c->out = nb;
			c->out_cap = want;
		}
	}
	memcpy(c->out + c->out_off + c->out_len, p, n);
	c->out_len += n;
	return 0;
}

int client_send(struct client *c, uint32_t type, const void *payload,
		uint32_t len)
{
	struct aud_header h;
	if (c->dead || c->fd < 0)
		return -1;
	h.type = type;
	h.len = len;
	if (queue_bytes(c, (const uint8_t *)&h, sizeof(h)) < 0)
		return -1;
	if (len && queue_bytes(c, payload, len) < 0)
		return -1;
	return client_flush(c) < 0 ? -1 : 0;
}

int client_send_error(struct client *c, int code, const char *msg)
{
	struct aud_error e;
	memset(&e, 0, sizeof(e));
	e.code = code > 0 ? -code : code;
	if (msg)
		snprintf(e.message, sizeof(e.message), "%s", msg);
	return client_send(c, AUD_MSG_ERROR, &e, sizeof(e));
}

int client_send_ok(struct client *c)
{
	return client_send(c, AUD_MSG_OK, NULL, 0);
}

void notify_all(uint32_t mask, uint32_t what, uint32_t a, uint32_t b,
		uint32_t cc)
{
	struct aud_notify n;
	n.what = what;
	n.a = a;
	n.b = b;
	n.c = cc;
	for (struct client *c = srv.clients; c; c = c->next) {
		if (c->dead || c->kind != AUD_CLIENT_CONTROL ||
		    !(c->sub_mask & mask))
			continue;
		client_send(c, AUD_MSG_NOTIFY, &n, sizeof(n));
	}
}

/* ---- streams ------------------------------------------------------------------- */

static void stream_unlink(struct stream *s)
{
	struct stream **pp = &srv.streams;
	while (*pp) {
		if (*pp == s) {
			*pp = s->next;
			return;
		}
		pp = &(*pp)->next;
	}
}

void stream_destroy(struct stream *s, int notify)
{
	uint32_t id = s->id;
	stream_unlink(s);
	if (s->owner && s->owner->stream == s)
		s->owner->stream = NULL;
	if (srv.nstreams)
		srv.nstreams--;
	if (s->evsnd) {
		if (srv.nevent_streams)
			srv.nevent_streams--;
		events_stream_done(s);
	}
	struct uid_usage *u = uid_get(s->uid, 0);
	if (u && !s->evsnd) {
		if (u->streams)
			u->streams--;
		u->bytes = u->bytes >= s->buffer_bytes ? u->bytes - s->buffer_bytes
						       : 0;
		uid_put_if_idle(u);
	}
	dsp_stream_free(&s->dsp);
	free(s);
	if (notify)
		notify_all(AUD_SUB_STREAMS, AUD_NOTIFY_STREAM_GONE, id, 0, 0);
}

void stream_set_volume(struct stream *s, uint32_t l, uint32_t r, uint32_t mute)
{
	s->vol_l = l > 100 ? 100 : l;
	s->vol_r = r > 100 ? 100 : r;
	s->mute = mute ? 1 : 0;
	dsp_stream_set_volume(&s->dsp, s->vol_l, s->vol_r, (int)s->mute);
}

void stream_kill(struct stream *s)
{
	if (s->killed || s->done)
		return;
	s->killed = 1;
	ramp_set(&s->dsp.fade, 0.0f, srv.dev.rate / 1000u * DSP_RAMP_MS);
	engine_wake();
}

static void stream_wake_if_ready(struct stream *s)
{
	if (s->prebuffering &&
	    (s->dsp.ring.used >= s->prebuf_bytes || s->dsp.eof)) {
		s->prebuffering = 0;
		s->underrun_sent = 0;
		if (!s->paused)
			engine_wake();
	}
}

/* Hand freed ring room back to the client. */
static void stream_grant(struct stream *s, int force)
{
	if (!s->owner || s->owner->dead || s->evsnd)
		return;
	size_t space = ring_space(&s->dsp.ring);
	if (space <= s->credit)
		return;
	size_t grant = space - s->credit;
	grant -= grant % s->dsp.frame_bytes;
	if (grant == 0)
		return;
	if (!force && grant < s->request_min)
		return;
	/* one REQUEST carries at most what fits a uint32 */
	if (grant > 0x7fffffffu)
		grant = 0x7fffffffu;
	struct aud_request rq;
	rq.bytes = (uint32_t)grant;
	rq.reserved = 0;
	if (client_send(s->owner, AUD_MSG_REQUEST, &rq, sizeof(rq)) == 0)
		s->credit += grant;
}

static size_t ms_to_bytes(uint32_t rate, size_t frame_bytes, uint32_t ms)
{
	uint64_t frames = ((uint64_t)rate * ms + 999) / 1000;
	return (size_t)(frames * frame_bytes);
}

static void stream_open(struct client *c, const struct aud_stream_open *o)
{
	struct stream_req rq;
	int rc = proto_check_stream_open(o, &rq);

	if (rc == -EBADMSG) {
		client_close(c, "STREAM_OPEN with an unterminated name");
		return;
	}
	if (rc < 0) {
		client_send_error(c, EINVAL, "invalid stream parameters");
		return;
	}
	if (c->stream) {
		client_close(c, "second STREAM_OPEN on one connection");
		return;
	}

	struct uid_usage *u = uid_get(c->cred.uid, 1);
	if (!u) {
		client_send_error(c, EBUSY, "too many users");
		return;
	}
	if (srv.nstreams - srv.nevent_streams >= HARD_MAX_STREAMS) {
		client_send_error(c, EBUSY, "too many streams");
		return;
	}
	if (u->streams >= srv.cfg.max_streams_per_user) {
		client_send_error(c, EBUSY, "too many streams for this user");
		return;
	}

	size_t fb = sf_bytes(rq.fmt) * rq.channels;
	uint32_t ms = rq.buffer_ms ? rq.buffer_ms : srv.cfg.default_buffer_ms;
	if (ms < 10)
		ms = 10;
	if (ms > 2000)
		ms = 2000;
	size_t ring = ms_to_bytes(rq.rate, fb, ms);
	/* never less than two device periods' worth of the source, or the
	 * stream underruns by construction */
	size_t min_ring = ms_to_bytes(rq.rate, fb,
				      2 * (srv.cfg.period_ms ? srv.cfg.period_ms
							     : 10));
	if (ring < min_ring)
		ring = min_ring;
	/* The engine renders up to two GRANTED device periods in one go
	 * (engine_fill), and the kernel rounds the period up from what was
	 * asked (1200 -> 1216 frames at 48 kHz); a resampled stream needs the
	 * filter's taps of lookahead on top.  A ring smaller than that runs
	 * dry on every such render and sits out a prebuffer of silence. */
	if (srv.dev.rate && srv.dev.period_frames) {
		uint64_t burst = 2ull * srv.dev.period_frames;
		uint64_t src = (burst * rq.rate + srv.dev.rate - 1) /
			       srv.dev.rate;

		if (rq.rate != srv.dev.rate)
			src += 64; /* the longest kernel, RS_QUALITY_BEST */
		if (ring < src * fb)
			ring = (size_t)(src * fb);
	}
	/* and at least one maximal DATA message, so a client that sends
	 * full-size messages is never stuck below the credit it needs */
	if (ring < AUDIOD_MAX_PAYLOAD / 4)
		ring = AUDIOD_MAX_PAYLOAD / 4;
	ring -= ring % fb;
	if (ring < fb)
		ring = fb;
	size_t budget = (size_t)srv.cfg.max_buffer_kb_per_user * 1024u;
	if (ring > 16u * 1024u * 1024u || u->bytes + ring > budget) {
		client_send_error(c, ENOMEM, "buffer budget exceeded");
		return;
	}

	struct stream *s = calloc(1, sizeof(*s));
	if (!s) {
		client_send_error(c, ENOMEM, "out of memory");
		return;
	}
	if (dsp_stream_init(&s->dsp, rq.fmt, rq.rate, (int)rq.channels, ring,
			    srv.dev.rate, srv.dev.channels,
			    srv.cfg.resample_quality) < 0) {
		free(s);
		client_send_error(c, ENOMEM, "cannot set up the stream");
		return;
	}

	s->id = next_id(&srv.next_stream_id);
	s->client_id = c->id;
	s->owner = c;
	s->uid = c->cred.uid;
	s->gid = c->cred.gid;
	s->pid = c->cred.pid;
	s->role = rq.role;
	s->aud_fmt = rq.aud_fmt;
	memcpy(s->name, rq.name, sizeof(s->name));
	memcpy(s->app, c->name, sizeof(s->app));
	s->buffer_bytes = ring;
	s->credit = ring;
	s->request_min = ring / 4;
	s->request_min -= s->request_min % fb;
	if (s->request_min < fb)
		s->request_min = fb;
	s->prebuf_bytes = ms_to_bytes(rq.rate, fb, 30);
	if (s->prebuf_bytes > ring / 2)
		s->prebuf_bytes = ring / 2;
	s->prebuf_bytes -= s->prebuf_bytes % fb;
	if (s->prebuf_bytes < fb)
		s->prebuf_bytes = fb;
	s->prebuffering = 1;
	stream_set_volume(s, rq.volume, rq.volume, 0);
	/* the first block plays at its volume without a ramp from 1.0 */
	for (int ch = 0; ch < DSP_MAX_CH; ch++)
		ramp_init(&s->dsp.gain[ch], s->dsp.gain[ch].target);
	if (rq.flags & AUD_STREAM_START_PAUSED) {
		s->paused = 1;
		ramp_init(&s->dsp.fade, 0.0f);
	}

	s->next = srv.streams;
	srv.streams = s;
	srv.nstreams++;
	u->streams++;
	u->bytes += ring;
	c->stream = s;

	struct aud_stream_opened rep;
	memset(&rep, 0, sizeof(rep));
	rep.stream_id = s->id;
	rep.buffer_bytes = (uint32_t)ring;
	rep.request_bytes = (uint32_t)ring;
	client_send(c, AUD_MSG_STREAM_OPEN, &rep, sizeof(rep));

	log_info("stream %u opened by client %u (%s, uid %u): %s %u Hz %u ch, "
		 "%u ms buffer, \"%s\"",
		 s->id, c->id, c->name, s->uid, sf_name(rq.fmt), rq.rate,
		 rq.channels, ms, s->name);
	notify_all(AUD_SUB_STREAMS, AUD_NOTIFY_STREAM_NEW, s->id, 0, 0);
}

static void stream_data(struct client *c, const uint8_t *p, uint32_t len)
{
	struct stream *s = c->stream;
	if (!s || s->done) {
		client_close(c, "DATA without an open stream");
		return;
	}
	if (len % s->dsp.frame_bytes) {
		client_close(c, "DATA is not a whole number of frames");
		return;
	}
	if (len > s->credit) {
		client_close(c, "DATA beyond the granted credit");
		return;
	}
	if (len == 0)
		return;
	size_t w = ring_write(&s->dsp.ring, p, len);
	/* the credit invariant guarantees room */
	if (w != len) {
		client_close(c, "internal: stream ring overflow");
		return;
	}
	s->credit -= len;
	if (s->dsp.eof && !s->draining) {
		/* more data after a completed drain: a new run */
		dsp_stream_set_eof(&s->dsp, 0);
		s->drain_target_set = 0;
		s->prebuffering = 1;
	}
	stream_wake_if_ready(s);
}

static void fill_latency(struct stream *s, struct aud_latency *l)
{
	memset(l, 0, sizeof(*l));
	double pending = dsp_stream_pending_frames(&s->dsp); /* source frames */
	uint32_t dev_delay = engine_delay_frames(); /* device frames */
	double us = 0.0;
	if (s->dsp.rate)
		us += pending * 1e6 / (double)s->dsp.rate;
	if (srv.dev.rate)
		us += (double)dev_delay * 1e6 / (double)srv.dev.rate;
	l->latency_us = (uint64_t)us;

	/* frames heard = taken from the ring, minus what the resampler and
	 * the device still hold, in source frames */
	double in_dev = srv.dev.rate ? (double)dev_delay * (double)s->dsp.rate /
					       (double)srv.dev.rate
				     : 0.0;
	double held = s->dsp.rs.bypass ? 0.0 : rs_pending_in(&s->dsp.rs);
	double played = (double)s->dsp.in_frames - held - in_dev;
	uint64_t pf = played > 0.0 ? (uint64_t)played : 0;
	if (pf < s->last_played)
		pf = s->last_played; /* never goes backwards */
	if (pf > s->dsp.in_frames)
		pf = s->dsp.in_frames;
	s->last_played = pf;
	l->played_frames = pf;
	l->timestamp_ns = now_ns();
	l->underruns = s->underruns;
}

static void stream_msg(struct client *c, uint32_t type, const uint8_t *p,
		       uint32_t len)
{
	struct stream *s = c->stream;

	if (type == AUD_MSG_STREAM_OPEN) {
		struct aud_stream_open o;
		memcpy(&o, p, sizeof(o));
		stream_open(c, &o);
		return;
	}
	if (type == AUD_MSG_DATA) {
		stream_data(c, p, len);
		return;
	}
	if (type == AUD_MSG_GET_SERVER_INFO) {
		control_handle(c, type, p, len);
		return;
	}
	if (!s || s->done) {
		client_close(c, "stream request without an open stream");
		return;
	}

	switch (type) {
	case AUD_MSG_DRAIN:
		if (s->paused) {
			client_send_error(c, EBUSY, "stream is paused");
			return;
		}
		s->draining = 1;
		s->drain_target_set = 0;
		dsp_stream_set_eof(&s->dsp, 1);
		{
			double pend = dsp_stream_pending_frames(&s->dsp);
			uint64_t ms = (uint64_t)(pend * 1000.0 / s->dsp.rate);
			if (srv.dev.rate)
				ms += (uint64_t)srv.dev.buffer_frames * 1000u /
				      srv.dev.rate;
			s->drain_deadline_ms = now_ms() + ms + 3000;
		}
		c->blocked = 1;
		stream_wake_if_ready(s);
		engine_wake();
		return;
	case AUD_MSG_FLUSH:
		dsp_stream_flush(&s->dsp);
		dsp_stream_set_eof(&s->dsp, 0);
		s->prebuffering = 1;
		s->underrun_sent = 0;
		s->drain_target_set = 0;
		client_send_ok(c);
		stream_grant(s, 1);
		return;
	case AUD_MSG_PAUSE:
		if (!s->paused) {
			s->paused = 1;
			ramp_set(&s->dsp.fade, 0.0f,
				 srv.dev.rate / 1000u * DSP_RAMP_MS);
		}
		client_send_ok(c);
		return;
	case AUD_MSG_RESUME:
		if (s->paused) {
			s->paused = 0;
			ramp_set(&s->dsp.fade, 1.0f,
				 srv.dev.rate / 1000u * DSP_RAMP_MS);
			if (!s->prebuffering)
				engine_wake();
			else
				stream_wake_if_ready(s);
		}
		client_send_ok(c);
		return;
	case AUD_MSG_SET_STREAM_VOLUME: {
		struct aud_volume v;
		memcpy(&v, p, sizeof(v));
		if (proto_check_volume(&v) < 0) {
			client_send_error(c, EINVAL, "volume must be 0..100");
			return;
		}
		stream_set_volume(s, v.left, v.right, s->mute);
		client_send_ok(c);
		return;
	}
	case AUD_MSG_SET_STREAM_MUTE: {
		struct aud_mute m;
		memcpy(&m, p, sizeof(m));
		if (proto_check_mute(&m) < 0) {
			client_send_error(c, EINVAL, "mute must be 0 or 1");
			return;
		}
		stream_set_volume(s, s->vol_l, s->vol_r, m.mute);
		client_send_ok(c);
		return;
	}
	case AUD_MSG_GET_LATENCY: {
		struct aud_latency l;
		fill_latency(s, &l);
		client_send(c, AUD_MSG_GET_LATENCY, &l, sizeof(l));
		return;
	}
	case AUD_MSG_STREAM_CLOSE:
		/* no reply: the client closes right after */
		log_info("stream %u closed by its client", s->id);
		s->done = 1;
		s->owner = NULL;
		c->stream = NULL;
		return;
	default:
		client_close(c, "unexpected request on a stream connection");
		return;
	}
}

/* ---- message dispatch ---------------------------------------------------------- */

static void handle_hello(struct client *c, const uint8_t *p)
{
	struct aud_hello h;
	struct aud_hello_reply r;
	char name[AUDIOD_NAME_MAX];

	memcpy(&h, p, sizeof(h));
	int rc = proto_check_hello(&h, name);
	if (rc == -EPROTONOSUPPORT) {
		client_send_error(c, EPROTONOSUPPORT,
				  "protocol version not supported");
		/* let the error go out; the client closes on it */
		c->kind = -1;
		return;
	}
	if (rc < 0) {
		client_close(c, "malformed HELLO");
		return;
	}
	c->kind = (int)h.kind;
	memcpy(c->name, name, sizeof(c->name));

	memset(&r, 0, sizeof(r));
	r.version = AUDIOD_PROTOCOL_VERSION;
	r.client_id = c->id;
	r.rate = srv.dev.rate;
	r.channels = (uint32_t)srv.dev.channels;
	r.format = sf_to_proto(srv.dev.fmt);
	client_send(c, AUD_MSG_HELLO, &r, sizeof(r));
	log_debug("client %u is \"%s\" (%s)", c->id, c->name,
		  c->kind == AUD_CLIENT_STREAM ? "stream" : "control");
}

static void handle_message(struct client *c, uint32_t type, const uint8_t *p,
			   uint32_t len)
{
	if (c->kind < 0) {
		client_close(c, "request after a refused HELLO");
		return;
	}
	if (c->kind == 0) {
		if (type != AUD_MSG_HELLO) {
			client_close(c, "first message is not HELLO");
			return;
		}
		handle_hello(c, p);
		return;
	}
	if (type == AUD_MSG_HELLO) {
		client_close(c, "second HELLO");
		return;
	}
	int want = c->kind == AUD_CLIENT_STREAM ? 1 : 2;
	if (!(proto_kind_of(type) & want)) {
		client_close(c, "request does not belong to this kind of "
				"connection");
		return;
	}
	if (c->kind == AUD_CLIENT_STREAM)
		stream_msg(c, type, p, len);
	else
		control_handle(c, type, p, len);
}

/* Parse what is buffered, as long as the client is not blocked. */
static void client_process(struct client *c)
{
	while (!c->dead && !c->blocked && c->in_off < c->in_len) {
		size_t used = 0;
		int r = mr_feed(&c->mr, c->inbuf + c->in_off,
				c->in_len - c->in_off, &used);
		c->in_off += used;
		if (r == MR_ERROR) {
			char why[96];
			snprintf(why, sizeof(why), "protocol violation: %s "
				 "(type %u, length %u)",
				 c->mr.error ? c->mr.error : "?", c->mr.h.type,
				 c->mr.h.len);
			client_close(c, why);
			return;
		}
		if (r == MR_MESSAGE) {
			uint32_t type = c->mr.h.type, len = c->mr.h.len;
			handle_message(c, type, c->mr.payload, len);
			mr_next(&c->mr);
		}
	}
	if (c->in_off >= c->in_len)
		c->in_off = c->in_len = 0;
}

void client_unblock(struct client *c)
{
	c->blocked = 0;
	client_process(c);
}

void client_io(struct client *c, short revents)
{
	if (c->dead)
		return;
	if (revents & POLLOUT) {
		if (client_flush(c) < 0)
			return;
	}
	if (revents & POLLNVAL) {
		client_close(c, "invalid descriptor");
		return;
	}

	/* bounded work per wakeup so one fast client cannot hold up the
	 * device: four buffers (64 KiB) at most */
	for (int round = 0; round < 4 && !c->dead && !c->blocked; round++) {
		if (c->in_off < c->in_len) {
			client_process(c);
			if (c->dead || c->blocked || c->in_off < c->in_len)
				break;
		}
		if (!(revents & (POLLIN | POLLHUP | POLLERR)))
			break;
		ssize_t r = recv(c->fd, c->inbuf, sizeof(c->inbuf), 0);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			client_close(c, "read error");
			return;
		}
		if (r == 0) {
			if (c->mr.hdr_got || c->mr.got)
				client_close(c, "disconnected mid-message");
			else
				client_close(c, NULL);
			return;
		}
		c->in_off = 0;
		c->in_len = (size_t)r;
		c->last_rx_ms = now_ms();
		client_process(c);
	}
	/* a blocked client that hung up is noticed here */
	if (!c->dead && c->blocked && (revents & (POLLHUP | POLLERR)) &&
	    !(revents & POLLIN))
		client_close(c, "hung up while waiting");
}

void clients_timeouts(uint64_t now)
{
	for (struct client *c = srv.clients; c; c = c->next) {
		if (c->dead)
			continue;
		if (c->kind == 0 && now - c->accepted_ms > HELLO_TIMEOUT_MS) {
			client_close(c, "no HELLO in time");
			continue;
		}
		if (c->kind < 0 && now - c->accepted_ms > HELLO_TIMEOUT_MS) {
			client_close(c, "refused");
			continue;
		}
		if (mr_partial(&c->mr) &&
		    now - c->last_rx_ms > PARTIAL_MSG_TIMEOUT_MS) {
			client_close(c, "stalled in the middle of a message");
			continue;
		}
	}
}

void clients_reap(void)
{
	/* streams first: finished, killed or orphaned */
	struct stream *s = srv.streams;
	while (s) {
		struct stream *next = s->next;
		if (s->done || (s->owner && s->owner->dead)) {
			if (s->owner && s->owner->stream == s)
				s->owner->stream = NULL;
			s->owner = NULL;
			log_debug("stream %u removed", s->id);
			stream_destroy(s, 1);
		}
		s = next;
	}

	struct client **pp = &srv.clients;
	while (*pp) {
		struct client *c = *pp;
		if (c->dead) {
			*pp = c->next;
			if (srv.nclients)
				srv.nclients--;
			client_free(c);
			continue;
		}
		pp = &c->next;
	}
}

/* ---- the mixing side of the streams -------------------------------------------- */

static int stream_mixable(const struct stream *s)
{
	if (s->done || (s->owner && s->owner->dead))
		return 0;
	if (s->prebuffering)
		return 0;
	if (s->paused && ramp_done(&s->dsp.fade) && s->dsp.fade.cur == 0.0f)
		return 0;
	if (s->dsp.eof && s->dsp.tail_done && dsp_stream_queued_frames(&s->dsp) == 0)
		return 0;
	return 1;
}

int streams_any_active(void)
{
	for (struct stream *s = srv.streams; s; s = s->next)
		if (stream_mixable(s) || (s->draining && !s->done) ||
		    (s->evsnd && !s->done))
			return 1;
	return 0;
}

size_t streams_render(float *acc, size_t frames)
{
	size_t active = 0;
	for (struct stream *s = srv.streams; s; s = s->next) {
		if (!stream_mixable(s))
			continue;
		size_t got = dsp_stream_mix(&s->dsp, acc, frames);
		if (got)
			active++;
		if (got < frames && !s->dsp.eof && !s->paused && !s->killed) {
			/* ran dry: wait for a refill before playing again */
			s->underruns++;
			s->prebuffering = 1;
			if (!s->underrun_sent && s->owner) {
				s->underrun_sent = 1;
				client_send(s->owner, AUD_MSG_UNDERRUN, NULL, 0);
				log_debug("stream %u underrun", s->id);
			}
		}
	}
	return active;
}

/* Bookkeeping after each render: credits, drains, killed and finished
 * streams. */
void streams_after_render(void)
{
	for (struct stream *s = srv.streams; s; s = s->next) {
		if (s->done)
			continue;
		if (s->killed && (!stream_mixable(s) ||
				  (ramp_done(&s->dsp.fade) &&
				   s->dsp.fade.cur == 0.0f))) {
			s->done = 1;
			if (s->owner)
				client_close(s->owner, "stream killed");
			continue;
		}
		if ((s->draining || s->evsnd) && !s->drain_target_set &&
		    dsp_stream_finished(&s->dsp)) {
			/* the last sample went into the block just rendered */
			s->drain_target = srv.engine_written;
			s->drain_target_set = 1;
		}
		stream_grant(s, s->prebuffering);
	}
	streams_check_drains();
}

void streams_check_drains(void)
{
	uint64_t now = now_ms();
	uint64_t played = engine_played();
	for (struct stream *s = srv.streams; s; s = s->next) {
		if (s->done || !(s->draining || s->evsnd))
			continue;
		int timed_out = s->drain_deadline_ms && now >= s->drain_deadline_ms;
		if (!(s->drain_target_set && played >= s->drain_target) &&
		    !timed_out)
			continue;
		if (timed_out && !(s->drain_target_set && played >= s->drain_target))
			log_debug("stream %u: drain deadline passed", s->id);

		if (s->evsnd) {
			s->done = 1; /* events_stream_done replies to a waiter */
			continue;
		}
		s->draining = 0;
		struct client *c = s->owner;
		if (c && !c->dead) {
			client_send_ok(c);
			client_unblock(c);
		}
	}
}

uint64_t streams_next_deadline(void)
{
	uint64_t next = 0;
	for (struct stream *s = srv.streams; s; s = s->next) {
		if (s->done || !(s->draining || s->evsnd) || !s->drain_deadline_ms)
			continue;
		if (!next || s->drain_deadline_ms < next)
			next = s->drain_deadline_ms;
	}
	return next;
}

int streams_retarget(void)
{
	int bad = 0;
	for (struct stream *s = srv.streams; s; s = s->next) {
		if (s->done)
			continue;
		if (dsp_stream_retarget(&s->dsp, srv.dev.rate, srv.dev.channels) <
		    0) {
			log_err("stream %u cannot follow the new device format",
				s->id);
			s->done = 1;
			if (s->owner)
				client_close(s->owner, "cannot convert to the new "
						       "device format");
			bad++;
		}
		/* audio in flight on the old device is gone */
		s->drain_target_set = 0;
	}
	return bad;
}
