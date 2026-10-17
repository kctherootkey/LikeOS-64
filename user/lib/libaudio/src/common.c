// libaudio common code: socket setup, message framing in both directions
// with full-read / full-write loops and strict length checks, the request /
// reply engine with timeouts, error strings, and the format and role name
// tables.
//
// Wire rules this file enforces for everything the service sends:
//   * a header announcing more than AUDIOD_MAX_PAYLOAD bytes kills the
//     connection before a single payload byte is read;
//   * every reply must have exactly the size its type allows (variable
//     replies are bounded by the caller), anything else is EPROTO;
//   * strings received in fixed fields are NUL-terminated and stripped of
//     control characters before the caller sees them.
// A dead connection stays dead: every later call returns the same error.
//
// Copyright (C) 2026 The LikeOS Project

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#include "la_internal.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

extern char *program_invocation_short_name;

/* ============================================================================
 * Time
 * ========================================================================== */

uint64_t lai_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void lai_deadline_start(struct lai_deadline *d, int timeout_ms)
{
	if (timeout_ms < 0) {
		d->infinite = 1;
		d->end_ms = 0;
		return;
	}
	d->infinite = 0;
	d->end_ms = lai_now_ms() + (uint64_t)timeout_ms;
}

int lai_deadline_left(const struct lai_deadline *d)
{
	uint64_t now, left;

	if (d->infinite)
		return -1;
	now = lai_now_ms();
	if (now >= d->end_ms)
		return 0;
	left = d->end_ms - now;
	return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}

void lai_sleep_ms(unsigned ms)
{
	struct timespec req, rem;

	req.tv_sec = ms / 1000u;
	req.tv_nsec = (long)(ms % 1000u) * 1000000L;
	/* A signal cuts the sleep short; carry on with what is left. */
	while (nanosleep(&req, &rem) != 0 && errno == EINTR)
		req = rem;
}

/* poll() one descriptor.  Returns revents, 0 on timeout, -errno on error.
 * EINTR is reported as 0 with *intr set, so callers re-check their own
 * deadline and try again. */
static int lai_poll1(int fd, short events, int timeout_ms, int *intr)
{
	struct pollfd p;
	int r;

	*intr = 0;
	p.fd = fd;
	p.events = events;
	p.revents = 0;
	r = poll(&p, 1, timeout_ms);
	if (r < 0) {
		if (errno == EINTR) {
			*intr = 1;
			return 0;
		}
		return -errno;
	}
	if (r == 0)
		return 0;
	if (p.revents & POLLNVAL)
		return -EBADF;
	return p.revents;
}

/* ============================================================================
 * Small helpers
 * ========================================================================== */

const char *lai_program_name(void)
{
	const char *n = program_invocation_short_name;

	if (!n || !*n)
		return "unknown";
	return n;
}

/* Length of the UTF-8 sequence a lead byte starts, 0 for a continuation or
 * an invalid lead. */
static size_t utf8_seq_len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xE0) == 0xC0)
		return 2;
	if ((c & 0xF0) == 0xE0)
		return 3;
	if ((c & 0xF8) == 0xF0)
		return 4;
	return 0;
}

void lai_copy_name(char *dst, size_t dstsz, const char *src)
{
	size_t i = 0;

	if (!dst || dstsz == 0)
		return;
	/* The whole field is cleared first: what goes on the wire must never
	 * carry stack or heap bytes of this process. */
	memset(dst, 0, dstsz);
	if (!src)
		return;
	while (src[i] && i < dstsz - 1) {
		unsigned char c = (unsigned char)src[i];
		size_t n = utf8_seq_len(c);

		if (n == 0) {
			/* Stray continuation or invalid byte. */
			dst[i++] = '?';
			continue;
		}
		if (n > 1) {
			size_t k;
			int ok = 1;

			/* A multi-byte character that does not fit is left
			 * out whole rather than cut in half. */
			if (i + n > dstsz - 1)
				break;
			for (k = 1; k < n; k++) {
				unsigned char cc = (unsigned char)src[i + k];

				if ((cc & 0xC0) != 0x80) {
					ok = 0;
					break;
				}
			}
			if (!ok) {
				dst[i++] = '?';
				continue;
			}
			memcpy(dst + i, src + i, n);
			i += n;
			continue;
		}
		dst[i++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
	}
}

void lai_fix_string(char *s, size_t sz)
{
	size_t i;

	if (!s || sz == 0)
		return;
	s[sz - 1] = '\0';
	for (i = 0; i < sz && s[i]; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c == 0x7F)
			s[i] = ' ';
	}
	/* Zero whatever follows the terminator, so copying the field around
	 * never moves bytes the service did not mean as text. */
	for (; i < sz; i++)
		s[i] = '\0';
}

int lai_sane_errno(int32_t code)
{
	if (code < 0 && code > -4096)
		return code;
	return -EIO;
}

int lai_event_name_ok(const char *name)
{
	size_t i, n;

	if (!name)
		return 0;
	n = strlen(name);
	if (n == 0 || n >= AUD_EVENT_NAME_MAX)
		return 0;
	/* Names are looked up as <dir>/<name>.wav by the service; anything
	 * that could step out of that directory is refused here already. */
	if (name[0] == '.' || name[0] == '-')
		return 0;
	if (strstr(name, ".."))
		return 0;
	for (i = 0; i < n; i++) {
		char c = name[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.')
			continue;
		return 0;
	}
	return 1;
}

/* ============================================================================
 * Connection setup
 * ========================================================================== */

static int set_fd_flags(int fd)
{
	int fl;

	fl = fcntl(fd, F_GETFD);
	if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
		return -errno;
	fl = fcntl(fd, F_GETFL);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
		return -errno;
	return 0;
}

static int retryable_connect_error(int e)
{
	switch (e) {
	case ECONNREFUSED: /* not listening yet, or backlog full */
	case ENOENT: /* socket file not created yet */
	case EAGAIN:
	case ETIMEDOUT:
	case EINTR:
		return 1;
	default:
		return 0;
	}
}

int lai_connect(const char *path, int *fdp)
{
	struct sockaddr_un sa;
	size_t plen;
	unsigned delay = LAI_CONNECT_FIRST_DELAY_MS;
	int last = ECONNREFUSED;
	int tries;

	if (!path || !fdp)
		return -EINVAL;
	*fdp = -1;
	plen = strlen(path);
	if (plen == 0 || plen >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, path, plen);

	for (tries = 0; tries < LAI_CONNECT_TRIES; tries++) {
		int fd;

		if (tries > 0) {
			lai_sleep_ms(delay);
			delay *= 2;
		}
		fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0 && errno == EINVAL && SOCK_CLOEXEC != 0)
			fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
			return -errno;
		/* Connect in blocking mode: a local socket either connects
		 * at once or is refused, it never waits for the server. */
		if (connect(fd, (const struct sockaddr *)&sa,
			    (socklen_t)sizeof(sa)) == 0) {
			*fdp = fd;
			return 0;
		}
		last = errno;
		close(fd);
		if (!retryable_connect_error(last))
			break;
	}
	return -last;
}

int lai_conn_init(struct lai_conn *c, int fd, lai_async_fn fn, void *ctx)
{
	int r;

	memset(c, 0, sizeof(*c));
	c->fd = -1;
	if (fd < 0)
		return -EBADF;
	r = set_fd_flags(fd);
	if (r < 0) {
		close(fd);
		return r;
	}
	c->rx = malloc(LAI_MSG_MAX);
	c->tx = malloc(LAI_MSG_MAX);
	if (!c->rx || !c->tx) {
		free(c->rx);
		free(c->tx);
		c->rx = c->tx = NULL;
		close(fd);
		return -ENOMEM;
	}
	c->fd = fd;
	c->reply_timeout_ms = LAI_REPLY_TIMEOUT_MS;
	c->async = fn;
	c->async_ctx = ctx;
	return 0;
}

void lai_conn_destroy(struct lai_conn *c)
{
	if (!c)
		return;
	if (c->fd >= 0) {
		/* close() is not retried on EINTR: the descriptor is gone
		 * either way, and a retry could close a reused number. */
		close(c->fd);
		c->fd = -1;
	}
	free(c->rx);
	free(c->tx);
	c->rx = c->tx = NULL;
	c->dead = -EBADF;
}

int lai_conn_fail(struct lai_conn *c, int err)
{
	if (err >= 0)
		err = -EIO;
	if (!c->dead)
		c->dead = err;
	return c->dead;
}

int lai_conn_take_async_err(struct lai_conn *c)
{
	int e = c->async_err;

	c->async_err = 0;
	return e;
}

static int map_io_errno(int e)
{
	switch (e) {
	case EPIPE:
	case ECONNRESET:
	case ENOTCONN:
	case ESHUTDOWN:
		return -EPIPE;
	default:
		return -e;
	}
}

/* ============================================================================
 * Transmit
 * ========================================================================== */

int lai_conn_tx_pending(const struct lai_conn *c)
{
	return c->tx_off < c->tx_len;
}

size_t lai_conn_tx_data_pending(const struct lai_conn *c)
{
	size_t left, hdr_left;

	if (c->tx_off >= c->tx_len || c->tx_type != AUD_MSG_DATA)
		return 0;
	left = c->tx_len - c->tx_off;
	/* Part of the header may still be unsent too. */
	hdr_left = c->tx_off < sizeof(struct aud_header) ?
			   sizeof(struct aud_header) - c->tx_off :
			   0;
	return left > hdr_left ? left - hdr_left : 0;
}

/* One non-blocking attempt to push the stashed tail.  1 when it is all
 * out, 0 when the socket is full, -errno on error. */
static int flush_once(struct lai_conn *c)
{
	while (c->tx_off < c->tx_len) {
		ssize_t n = send(c->fd, c->tx + c->tx_off,
				 c->tx_len - c->tx_off,
				 MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n > 0) {
			c->tx_off += (size_t)n;
			continue;
		}
		if (n == 0)
			return 0;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return lai_conn_fail(c, map_io_errno(errno));
	}
	c->tx_off = c->tx_len = 0;
	return 1;
}

/* Read what the socket has (and process it) while we wait for room to
 * write.  The service may itself be blocked writing credit to us; reading
 * here is what keeps the two sides from waiting on each other. */
static int pump_while_blocked(struct lai_conn *c);

int lai_conn_flush(struct lai_conn *c, int timeout_ms)
{
	struct lai_deadline dl;
	int r;

	if (c->dead)
		return c->dead;
	if (c->tx_off >= c->tx_len)
		return 0;
	lai_deadline_start(&dl, timeout_ms);
	for (;;) {
		int left, intr;

		r = flush_once(c);
		if (r < 0)
			return r;
		if (r == 1)
			return 0;
		left = lai_deadline_left(&dl);
		if (left == 0)
			return timeout_ms == 0 ? -EAGAIN : -ETIMEDOUT;
		r = lai_poll1(c->fd, POLLOUT | POLLIN, left, &intr);
		if (r < 0)
			return lai_conn_fail(c, r);
		if (r & POLLIN) {
			r = pump_while_blocked(c);
			if (r < 0)
				return r;
		}
	}
}

int lai_conn_sendv(struct lai_conn *c, uint32_t type, const struct iovec *iov,
		   int iovcnt, int timeout_ms)
{
	struct iovec v[5];
	struct aud_header h;
	struct lai_deadline dl;
	size_t total = 0, sent = 0, msg_len;
	int nv = 0, i, r;

	if (c->dead)
		return c->dead;
	if (iovcnt < 0 || iovcnt > 4 || (iovcnt > 0 && !iov))
		return -EINVAL;
	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len > AUDIOD_MAX_PAYLOAD ||
		    (iov[i].iov_len && !iov[i].iov_base))
			return -EINVAL;
		total += iov[i].iov_len;
	}
	if (total > AUDIOD_MAX_PAYLOAD)
		return -EMSGSIZE;

	/* Finish the previous message before a byte of this one goes out. */
	r = lai_conn_flush(c, timeout_ms);
	if (r < 0)
		return r;

	memset(&h, 0, sizeof(h));
	h.type = type;
	h.len = (uint32_t)total;
	v[nv].iov_base = &h;
	v[nv].iov_len = sizeof(h);
	nv++;
	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		v[nv].iov_base = iov[i].iov_base;
		v[nv].iov_len = iov[i].iov_len;
		nv++;
	}
	msg_len = sizeof(h) + total;

	lai_deadline_start(&dl, timeout_ms);
	for (;;) {
		struct msghdr mh;
		struct iovec cur[5];
		size_t skip = sent;
		int nc = 0, left, intr;
		ssize_t n;

		/* The pieces still to send, starting `sent' bytes in. */
		for (i = 0; i < nv; i++) {
			if (skip >= v[i].iov_len) {
				skip -= v[i].iov_len;
				continue;
			}
			cur[nc].iov_base = (uint8_t *)v[i].iov_base + skip;
			cur[nc].iov_len = v[i].iov_len - skip;
			skip = 0;
			nc++;
		}
		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = cur;
		mh.msg_iovlen = (size_t)nc;
		n = sendmsg(c->fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			sent += (size_t)n;
			if (sent >= msg_len)
				return 0;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			return lai_conn_fail(c, map_io_errno(errno));

		/* The socket is full. */
		left = lai_deadline_left(&dl);
		if (left == 0) {
			if (sent == 0)
				return timeout_ms == 0 ? -EAGAIN : -ETIMEDOUT;
			break; /* committed: stash the rest below */
		}
		r = lai_poll1(c->fd, POLLOUT | POLLIN, left, &intr);
		if (r < 0)
			return lai_conn_fail(c, r);
		if (r & POLLIN) {
			r = pump_while_blocked(c);
			if (r < 0)
				return r;
		}
	}

	/* Part of the message is on the socket; the rest must follow before
	 * anything else does.  Keep it, laid out at the offsets it has in the
	 * whole message (the bytes before tx_off are never looked at), so the
	 * header arithmetic in lai_conn_tx_data_pending holds. */
	{
		size_t skip = sent, pos = sent;

		for (i = 0; i < nv; i++) {
			if (skip >= v[i].iov_len) {
				skip -= v[i].iov_len;
				continue;
			}
			memcpy(c->tx + pos, (uint8_t *)v[i].iov_base + skip,
			       v[i].iov_len - skip);
			pos += v[i].iov_len - skip;
			skip = 0;
		}
		c->tx_off = sent;
		c->tx_len = msg_len;
		c->tx_type = type;
	}
	return 0;
}

int lai_conn_send(struct lai_conn *c, uint32_t type, const void *payload,
		  size_t len, int timeout_ms)
{
	struct iovec v;

	if (len == 0)
		return lai_conn_sendv(c, type, NULL, 0, timeout_ms);
	v.iov_base = (void *)payload;
	v.iov_len = len;
	return lai_conn_sendv(c, type, &v, 1, timeout_ms);
}

/* ============================================================================
 * Receive
 * ========================================================================== */

/* Read what is available into the receive buffer.  1 when bytes came in,
 * 0 when there were none, -errno on error or end of file. */
static int conn_fill(struct lai_conn *c)
{
	int got = 0;

	if (c->dead)
		return c->dead;
	/* Move the unparsed tail to the front: a whole message always fits
	 * in the buffer, so after this there is room unless a complete
	 * message is already waiting. */
	if (c->rx_off > 0) {
		size_t keep = c->rx_len - c->rx_off;

		if (keep)
			memmove(c->rx, c->rx + c->rx_off, keep);
		c->rx_len = keep;
		c->rx_off = 0;
	}
	while (c->rx_len < LAI_MSG_MAX) {
		ssize_t n = recv(c->fd, c->rx + c->rx_len,
				 LAI_MSG_MAX - c->rx_len, MSG_DONTWAIT);

		if (n > 0) {
			c->rx_len += (size_t)n;
			got = 1;
			continue;
		}
		if (n == 0) {
			/* The service closed the connection.  What was
			 * already read is still processed by the caller;
			 * the error sticks from now on. */
			lai_conn_fail(c, -EPIPE);
			return got ? 1 : -EPIPE;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		lai_conn_fail(c, map_io_errno(errno));
		return got ? 1 : c->dead;
	}
	return got;
}

/* Extract the next complete message.  1 with *m filled, 0 when none is
 * complete yet, -EPROTO on an impossible header. */
static int conn_take(struct lai_conn *c, struct lai_msg *m)
{
	struct aud_header h;
	size_t avail = c->rx_len - c->rx_off;

	if (avail < sizeof(h))
		return 0;
	memcpy(&h, c->rx + c->rx_off, sizeof(h));
	/* Never trust the announced length: it is checked before anything
	 * is done with it, and a lie ends the connection. */
	if (h.len > AUDIOD_MAX_PAYLOAD) {
		c->rx_off = c->rx_len = 0;
		return lai_conn_fail(c, -EPROTO);
	}
	if (avail < sizeof(h) + h.len)
		return 0;
	m->type = h.type;
	m->len = h.len;
	m->payload = c->rx + c->rx_off + sizeof(h);
	c->rx_off += sizeof(h) + h.len;
	if (c->rx_off == c->rx_len)
		c->rx_off = c->rx_len = 0;
	return 1;
}

/* Parse an ERROR message into the connection's error record.  Returns the
 * service's (validated) negative errno, or -EPROTO when malformed. */
static int parse_error(struct lai_conn *c, const struct lai_msg *m)
{
	struct aud_error e;

	if (m->len < sizeof(int32_t) || m->len > sizeof(e))
		return -EPROTO;
	memset(&e, 0, sizeof(e));
	memcpy(&e, m->payload, m->len);
	lai_fix_string(e.message, sizeof(e.message));
	c->err_code = lai_sane_errno(e.code);
	memcpy(c->err_msg, e.message, sizeof(c->err_msg));
	c->err_msg[sizeof(c->err_msg) - 1] = '\0';
	return c->err_code;
}

/* Is this a message of the kind that answers a request? */
static int is_reply_type(uint32_t t)
{
	switch (t) {
	case AUD_MSG_HELLO:
	case AUD_MSG_ERROR:
	case AUD_MSG_OK:
	case AUD_MSG_STREAM_OPEN:
	case AUD_MSG_DRAIN:
	case AUD_MSG_FLUSH:
	case AUD_MSG_PAUSE:
	case AUD_MSG_RESUME:
	case AUD_MSG_SET_STREAM_VOLUME:
	case AUD_MSG_GET_LATENCY:
	case AUD_MSG_STREAM_CLOSE:
	case AUD_MSG_SET_STREAM_MUTE:
	case AUD_MSG_GET_SERVER_INFO:
	case AUD_MSG_LIST_STREAMS:
	case AUD_MSG_GET_MASTER_VOLUME:
	case AUD_MSG_SET_MASTER_VOLUME:
	case AUD_MSG_GET_MUTE:
	case AUD_MSG_SET_MUTE:
	case AUD_MSG_GET_OUTPUTS:
	case AUD_MSG_SET_OUTPUT:
	case AUD_MSG_PLAY_EVENT:
	case AUD_MSG_SUBSCRIBE:
	case AUD_MSG_SET_VOLUME_OF:
	case AUD_MSG_KILL_STREAM:
	case AUD_MSG_GET_CARDS:
	case AUD_MSG_SET_CARD:
		return 1;
	default:
		return 0;
	}
}

/* Handle a message that is not the reply being waited for (or arrives
 * when none is).  0 when consumed, -errno when it kills the connection. */
static int dispatch_unsolicited(struct lai_conn *c, const struct lai_msg *m)
{
	int r = 0;

	if (c->async) {
		r = c->async(c->async_ctx, m);
		if (r < 0)
			return lai_conn_fail(c, r);
		if (r > 0)
			return 0;
	}
	if (!is_reply_type(m->type))
		return lai_conn_fail(c, -EPROTO); /* unknown message */
	if (c->skip_replies > 0) {
		/* The late answer to a request that timed out. */
		c->skip_replies--;
		return 0;
	}
	if (m->type == AUD_MSG_ERROR) {
		/* An error not tied to a request (e.g. about DATA). */
		r = parse_error(c, m);
		if (r == -EPROTO)
			return lai_conn_fail(c, r);
		c->async_err = r;
		return 0;
	}
	/* A reply nobody asked for: the two sides disagree about the
	 * conversation, nothing after this can be trusted. */
	return lai_conn_fail(c, -EPROTO);
}

static int pump_messages(struct lai_conn *c)
{
	struct lai_msg m;
	int r;

	while ((r = conn_take(c, &m)) > 0) {
		r = dispatch_unsolicited(c, &m);
		if (r < 0)
			return r;
	}
	return r;
}

static int pump_while_blocked(struct lai_conn *c)
{
	int r = conn_fill(c);

	if (r < 0 && c->rx_len == c->rx_off)
		return r;
	r = pump_messages(c);
	if (r < 0)
		return r;
	return c->dead;
}

int lai_conn_pump(struct lai_conn *c)
{
	int r;

	if (c->dead && c->rx_len == c->rx_off)
		return c->dead;
	for (;;) {
		r = conn_fill(c);
		/* Messages already received are processed even when the
		 * connection just ended: credit or a notification that came
		 * in before the close is still valid. */
		if (pump_messages(c) < 0)
			return c->dead;
		if (r <= 0)
			break;
	}
	return c->dead;
}

int lai_conn_wait(struct lai_conn *c, int timeout_ms)
{
	struct lai_deadline dl;
	int r;

	if (c->dead)
		return c->dead;
	/* A complete message may already be buffered. */
	if (c->rx_len - c->rx_off >= sizeof(struct aud_header)) {
		size_t before = c->rx_len - c->rx_off;

		r = pump_messages(c);
		if (r < 0)
			return r;
		if (c->rx_len - c->rx_off != before)
			return 1;
	}
	lai_deadline_start(&dl, timeout_ms);
	for (;;) {
		short ev = POLLIN;
		int left, intr, rev;

		if (lai_conn_tx_pending(c)) {
			r = flush_once(c);
			if (r < 0)
				return r;
			if (r == 0)
				ev |= POLLOUT;
		}
		left = lai_deadline_left(&dl);
		rev = lai_poll1(c->fd, ev, left, &intr);
		if (rev < 0)
			return lai_conn_fail(c, rev);
		if (rev == 0) {
			if (intr && lai_deadline_left(&dl) != 0)
				continue;
			return 0;
		}
		if (rev & POLLOUT) {
			r = flush_once(c);
			if (r < 0)
				return r;
		}
		if (rev & (POLLIN | POLLHUP | POLLERR)) {
			r = lai_conn_pump(c);
			if (r < 0)
				return r;
		}
		return 1;
	}
}

/* ============================================================================
 * Requests
 * ========================================================================== */

/* Validate a candidate reply.  1 = it is ours and fine (rc in *rc),
 * -errno = protocol violation. */
static int check_reply(struct lai_conn *c, const struct lai_msg *m,
		       uint32_t req_type, uint32_t reply_type,
		       size_t reply_min, size_t reply_max, int *rc)
{
	if (m->type == AUD_MSG_ERROR) {
		int e = parse_error(c, m);

		if (e == -EPROTO)
			return -EPROTO;
		*rc = e;
		return 1;
	}
	if (reply_type == AUD_MSG_OK) {
		/* An acknowledgement: OK, or the request's own type, empty. */
		if ((m->type == AUD_MSG_OK || m->type == req_type) &&
		    m->len == 0) {
			*rc = 0;
			return 1;
		}
		return -EPROTO;
	}
	if (m->type != reply_type)
		return -EPROTO;
	if (m->len < reply_min || m->len > reply_max)
		return -EPROTO;
	*rc = 0;
	return 1;
}

int lai_conn_request(struct lai_conn *c, uint32_t type, const void *req,
		     size_t req_len, uint32_t reply_type, size_t reply_min,
		     size_t reply_max, const uint8_t **reply,
		     uint32_t *reply_len, int timeout_ms)
{
	struct lai_deadline dl;
	struct lai_msg m;
	int r, rc = 0;

	if (reply)
		*reply = NULL;
	if (reply_len)
		*reply_len = 0;
	if (c->dead)
		return c->dead;
	if (reply_max > AUDIOD_MAX_PAYLOAD || reply_min > reply_max)
		return -EINVAL;

	lai_deadline_start(&dl, timeout_ms);
	r = lai_conn_send(c, type, req, req_len, timeout_ms);
	if (r < 0)
		return r;

	/* Messages already buffered that precede our reply are processed in
	 * order; the reply is the first reply-class message that is not owed
	 * to an abandoned request. */
	for (;;) {
		int left, intr;

		while ((r = conn_take(c, &m)) > 0) {
			if (c->async) {
				r = c->async(c->async_ctx, &m);
				if (r < 0)
					return lai_conn_fail(c, r);
				if (r > 0)
					continue;
			}
			if (!is_reply_type(m.type))
				return lai_conn_fail(c, -EPROTO);
			if (c->skip_replies > 0) {
				c->skip_replies--;
				continue;
			}
			r = check_reply(c, &m, type, reply_type, reply_min,
					reply_max, &rc);
			if (r < 0)
				return lai_conn_fail(c, r);
			if (rc == 0) {
				if (reply)
					*reply = m.payload;
				if (reply_len)
					*reply_len = m.len;
			}
			return rc;
		}
		if (r < 0)
			return r;
		if (c->dead) {
			/* End of file came with the last bytes. */
			return c->dead;
		}
		if (lai_conn_tx_pending(c)) {
			/* Our request itself is still partly unsent. */
			r = flush_once(c);
			if (r < 0)
				return r;
		}
		left = lai_deadline_left(&dl);
		if (left == 0) {
			/* Its answer will still come; make sure it is not
			 * mistaken for the answer to the next request. */
			c->skip_replies++;
			return -ETIMEDOUT;
		}
		r = lai_poll1(c->fd,
			      (short)(POLLIN |
				      (lai_conn_tx_pending(c) ? POLLOUT : 0)),
			      left, &intr);
		if (r < 0)
			return lai_conn_fail(c, r);
		if (r == 0)
			continue; /* timeout or signal: the loop re-checks */
		if (r & POLLOUT) {
			r = flush_once(c);
			if (r < 0)
				return r;
		}
		r = conn_fill(c);
		if (r < 0 && c->rx_len == c->rx_off)
			return r;
	}
}

int lai_conn_hello(struct lai_conn *c, uint32_t kind, const char *name)
{
	struct aud_hello h;
	struct aud_hello_reply rep;
	const uint8_t *p;
	uint32_t plen;
	int r;

	memset(&h, 0, sizeof(h));
	h.version = AUDIOD_PROTOCOL_VERSION;
	h.kind = kind;
	lai_copy_name(h.name, sizeof(h.name), name ? name : lai_program_name());
	r = lai_conn_request(c, AUD_MSG_HELLO, &h, sizeof(h), AUD_MSG_HELLO,
			     sizeof(rep), sizeof(rep), &p, &plen,
			     c->reply_timeout_ms);
	if (r < 0)
		return r;
	memcpy(&rep, p, sizeof(rep));
	if (rep.version != AUDIOD_PROTOCOL_VERSION)
		return lai_conn_fail(c, -EPROTONOSUPPORT);
	/* The device side description is informational; keep it sane. */
	if (rep.channels > 32)
		rep.channels = 0;
	if (rep.format > AUD_FMT_MAX)
		rep.format = 0;
	c->server = rep;
	return 0;
}

/* ============================================================================
 * Error strings
 * ========================================================================== */

struct lai_errstr {
	int code;
	const char *text;
};

static const struct lai_errstr lai_errors[] = {
	{ EPIPE, "connection to the audio service was lost" },
	{ ECONNREFUSED, "the audio service is not running" },
	{ ENOENT, "the audio service is not running (no socket)" },
	{ EACCES, "permission denied" },
	{ EPERM, "operation not permitted" },
	{ EINVAL, "invalid argument" },
	{ ENOMEM, "out of memory" },
	{ EAGAIN, "no room to write now (try again)" },
	{ ETIMEDOUT, "the audio service did not answer in time" },
	{ EPROTO, "audio protocol error" },
	{ EPROTONOSUPPORT, "the audio service speaks another protocol version" },
	{ EMSGSIZE, "message too long" },
	{ EBUSY, "device or resource busy" },
	{ ENODEV, "no sound card" },
	{ ENXIO, "no such output or card" },
	{ ENOSPC, "too many streams" },
	{ EDQUOT, "per-user audio limit reached" },
	{ EINTR, "interrupted" },
	{ EBADF, "invalid or closed handle" },
	{ ESRCH, "no such stream" },
#if defined(ENOTSUP)
	{ ENOTSUP, "not supported" },
#endif
#if defined(EOPNOTSUPP) && defined(ENOTSUP) && EOPNOTSUPP != ENOTSUP
	{ EOPNOTSUPP, "not supported" },
#endif
	{ ENOSYS, "not implemented by the audio service" },
	{ EIO, "input/output error" },
	{ ENOTCONN, "not connected to the audio service" },
	{ ECONNRESET, "connection to the audio service was reset" },
	{ EOVERFLOW, "value too large" },
	{ ERANGE, "value out of range" },
	{ ENAMETOOLONG, "name too long" },
	{ EEXIST, "already exists" },
	{ ENOTDIR, "the audio service path is not a directory" },
	{ EMFILE, "too many open files" },
	{ ENFILE, "too many open files in the system" },
	{ ECANCELED, "cancelled" },
	{ EALREADY, "already in that state" },
	{ ENOMSG, "no message" },
	{ EFAULT, "bad address" },
	{ ENOEXEC, "unsupported sound file" },
	{ EILSEQ, "invalid data" },
};

LA_EXPORT const char *la_strerror(int err)
{
	size_t i;

	if (err == 0)
		return "success";
	if (err < 0)
		err = -err;
	for (i = 0; i < sizeof(lai_errors) / sizeof(lai_errors[0]); i++)
		if (lai_errors[i].code == err)
			return lai_errors[i].text;
	return "unknown error";
}

/* ============================================================================
 * Formats and roles
 * ========================================================================== */

struct lai_fmt_info {
	uint32_t fmt;
	uint32_t bytes; /* per sample */
	const char *name; /* canonical */
};

static const struct lai_fmt_info lai_formats[] = {
	{ AUD_FMT_U8, 1, "u8" },
	{ AUD_FMT_S16_LE, 2, "s16le" },
	{ AUD_FMT_S24_LE, 4, "s24le" },
	{ AUD_FMT_S24_3LE, 3, "s24_3le" },
	{ AUD_FMT_S32_LE, 4, "s32le" },
	{ AUD_FMT_F32_LE, 4, "f32le" },
	{ AUD_FMT_S16_BE, 2, "s16be" },
	{ AUD_FMT_F64_LE, 8, "f64le" },
};

struct lai_alias {
	const char *name;
	uint32_t value;
};

/* Every spelling tools and file headers commonly use.  Matched without
 * regard to case. */
static const struct lai_alias lai_format_aliases[] = {
	{ "u8", AUD_FMT_U8 },
	{ "u8le", AUD_FMT_U8 },
	{ "uint8", AUD_FMT_U8 },
	{ "s16le", AUD_FMT_S16_LE },
	{ "s16_le", AUD_FMT_S16_LE },
	{ "s16", AUD_FMT_S16_LE },
	{ "int16", AUD_FMT_S16_LE },
	{ "pcm16", AUD_FMT_S16_LE },
	{ "cd", AUD_FMT_S16_LE },
	{ "s24le", AUD_FMT_S24_LE },
	{ "s24_le", AUD_FMT_S24_LE },
	{ "s24", AUD_FMT_S24_LE },
	{ "s24_32", AUD_FMT_S24_LE },
	{ "s24in32", AUD_FMT_S24_LE },
	{ "s24_3le", AUD_FMT_S24_3LE },
	{ "s24le3", AUD_FMT_S24_3LE },
	{ "s24_3", AUD_FMT_S24_3LE },
	{ "s24packed", AUD_FMT_S24_3LE },
	{ "pcm24", AUD_FMT_S24_3LE },
	{ "s32le", AUD_FMT_S32_LE },
	{ "s32_le", AUD_FMT_S32_LE },
	{ "s32", AUD_FMT_S32_LE },
	{ "int32", AUD_FMT_S32_LE },
	{ "pcm32", AUD_FMT_S32_LE },
	{ "f32le", AUD_FMT_F32_LE },
	{ "f32_le", AUD_FMT_F32_LE },
	{ "f32", AUD_FMT_F32_LE },
	{ "float", AUD_FMT_F32_LE },
	{ "float32", AUD_FMT_F32_LE },
	{ "float_le", AUD_FMT_F32_LE },
	{ "s16be", AUD_FMT_S16_BE },
	{ "s16_be", AUD_FMT_S16_BE },
	{ "f64le", AUD_FMT_F64_LE },
	{ "f64_le", AUD_FMT_F64_LE },
	{ "f64", AUD_FMT_F64_LE },
	{ "double", AUD_FMT_F64_LE },
	{ "float64", AUD_FMT_F64_LE },
};

static const char *const lai_role_names[] = {
	NULL, "music", "video", "game", "event", "phone", "web", "other",
};

static const struct lai_alias lai_role_aliases[] = {
	{ "music", AUD_ROLE_MUSIC },
	{ "audio", AUD_ROLE_MUSIC },
	{ "video", AUD_ROLE_VIDEO },
	{ "movie", AUD_ROLE_VIDEO },
	{ "film", AUD_ROLE_VIDEO },
	{ "game", AUD_ROLE_GAME },
	{ "event", AUD_ROLE_EVENT },
	{ "system", AUD_ROLE_EVENT },
	{ "notification", AUD_ROLE_EVENT },
	{ "alert", AUD_ROLE_EVENT },
	{ "a11y", AUD_ROLE_EVENT },
	{ "phone", AUD_ROLE_PHONE },
	{ "voice", AUD_ROLE_PHONE },
	{ "communication", AUD_ROLE_PHONE },
	{ "web", AUD_ROLE_WEB },
	{ "browser", AUD_ROLE_WEB },
	{ "other", AUD_ROLE_OTHER },
	{ "production", AUD_ROLE_OTHER },
	{ "test", AUD_ROLE_OTHER },
};

uint32_t lai_sample_bytes(uint32_t fmt)
{
	size_t i;

	for (i = 0; i < sizeof(lai_formats) / sizeof(lai_formats[0]); i++)
		if (lai_formats[i].fmt == fmt)
			return lai_formats[i].bytes;
	return 0;
}

LA_EXPORT uint32_t
la_frame_bytes(const la_format *fmt)
{
	uint32_t b;

	if (!fmt || fmt->channels < 1 || fmt->channels > 8)
		return 0;
	b = lai_sample_bytes(fmt->format);
	return b * fmt->channels;
}

LA_EXPORT const char *la_format_name(uint32_t fmt)
{
	size_t i;

	for (i = 0; i < sizeof(lai_formats) / sizeof(lai_formats[0]); i++)
		if (lai_formats[i].fmt == fmt)
			return lai_formats[i].name;
	return "unknown";
}

LA_EXPORT const char *la_role_name(uint32_t role)
{
	if (role >= 1 &&
	    role < sizeof(lai_role_names) / sizeof(lai_role_names[0]))
		return lai_role_names[role];
	return "unknown";
}

/* Parse a decimal number fully, within [lo, hi]. */
static int parse_number(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
	uint32_t v = 0;

	if (!*s)
		return -EINVAL;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -EINVAL;
		if (v > (UINT32_MAX - 9) / 10)
			return -ERANGE;
		v = v * 10 + (uint32_t)(*s - '0');
	}
	if (v < lo || v > hi)
		return -EINVAL;
	*out = v;
	return 0;
}

static int lookup_alias(const struct lai_alias *tab, size_t n,
			const char *name, uint32_t *out)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (strcasecmp(tab[i].name, name) == 0) {
			*out = tab[i].value;
			return 0;
		}
	}
	return -EINVAL;
}

LA_EXPORT int la_parse_format(const char *name,
							   uint32_t *fmt)
{
	uint32_t v;

	if (!name || !fmt)
		return -EINVAL;
	if (strlen(name) > 32)
		return -EINVAL;
	if (name[0] >= '0' && name[0] <= '9') {
		if (parse_number(name, 1, AUD_FMT_MAX, &v) < 0)
			return -EINVAL;
		*fmt = v;
		return 0;
	}
	if (lookup_alias(lai_format_aliases,
			 sizeof(lai_format_aliases) /
				 sizeof(lai_format_aliases[0]),
			 name, &v) < 0)
		return -EINVAL;
	*fmt = v;
	return 0;
}

LA_EXPORT int la_parse_role(const char *name,
							 uint32_t *role)
{
	uint32_t v;

	if (!name || !role)
		return -EINVAL;
	if (strlen(name) > 32)
		return -EINVAL;
	if (name[0] >= '0' && name[0] <= '9') {
		if (parse_number(name, AUD_ROLE_MUSIC, AUD_ROLE_OTHER, &v) < 0)
			return -EINVAL;
		*role = v;
		return 0;
	}
	if (lookup_alias(lai_role_aliases,
			 sizeof(lai_role_aliases) / sizeof(lai_role_aliases[0]),
			 name, &v) < 0)
		return -EINVAL;
	*role = v;
	return 0;
}
