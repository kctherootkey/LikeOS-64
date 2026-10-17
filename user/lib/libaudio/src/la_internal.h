// libaudio internals: the connection object shared by stream and control
// handles, message framing, timeouts and the small helpers every part of the
// library uses.  Nothing here is exported from libaudio.so.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_LIBAUDIO_INTERNAL_H
#define LIKEOS_LIBAUDIO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <audio/audio.h>
#include <audio/protocol.h>

/* The library is built with -fvisibility=hidden; the la_* API is marked
 * for export explicitly, so no internal helper can leak into the ABI. */
#define LAI_HIDDEN __attribute__((visibility("hidden")))
#define LA_EXPORT __attribute__((visibility("default")))

/* ---- tunables -------------------------------------------------------------- */

/* connect(): the service may be (re)starting; a few short retries hide that
 * without making "audiod is not running" slow to report. */
#define LAI_CONNECT_TRIES 5
#define LAI_CONNECT_FIRST_DELAY_MS 20

/* How long a request waits for its reply before giving up. */
#define LAI_REPLY_TIMEOUT_MS 5000
/* la_close(): best effort only, never hold a closing program up. */
#define LAI_CLOSE_TIMEOUT_MS 250
/* A blocking la_write() on a stream that is not paused and gets no credit
 * for this long reports ETIMEDOUT instead of hanging forever. */
#define LAI_STALL_TIMEOUT_MS 10000
/* An event sound played with `wait' set: they are a few seconds at most. */
#define LAI_EVENT_WAIT_TIMEOUT_MS 30000
/* Extra slack a drain gets on top of the time its buffer takes to play. */
#define LAI_DRAIN_SLACK_MS 10000

/* One message on the wire, header included. */
#define LAI_MSG_MAX (sizeof(struct aud_header) + AUDIOD_MAX_PAYLOAD)
/* The service's error text, as carried in struct aud_error. */
#define LAI_ERRMSG_MAX 96
/* The largest frame: 8 channels of F64. */
#define LAI_FRAME_MAX 64
/* Upper bound on credit the library will ever account (bytes).  Credit only
 * lets the client send more, so this is overflow protection, not policy. */
#define LAI_CREDIT_CAP (64ull * 1024 * 1024)
/* Notifications queued on a control handle before the oldest are dropped. */
#define LAI_NOTIFY_QUEUE 256

/* ---- time ------------------------------------------------------------------ */

struct lai_deadline {
	int infinite;
	uint64_t end_ms;
};

LAI_HIDDEN uint64_t lai_now_ms(void);
/* timeout_ms < 0: no deadline. */
LAI_HIDDEN void lai_deadline_start(struct lai_deadline *d, int timeout_ms);
/* Milliseconds left as a poll() timeout: -1 infinite, 0 expired. */
LAI_HIDDEN int lai_deadline_left(const struct lai_deadline *d);
LAI_HIDDEN void lai_sleep_ms(unsigned ms);

/* ---- the connection ---------------------------------------------------------- */

struct lai_msg {
	uint32_t type;
	uint32_t len;
	/* Points into the connection's receive buffer; valid only until the
	 * next call that reads from the socket.  Not aligned: memcpy out. */
	const uint8_t *payload;
};

/* Called for every message that arrives while no reply is being looked
 * for, and for every message seen while waiting for one.  Returns 1 when
 * the message was an asynchronous one and has been handled, 0 when it is
 * not an asynchronous message for this kind of connection (it is then
 * treated as a reply), and a negative errno when it was malformed (the
 * connection is then dead). */
typedef int (*lai_async_fn)(void *ctx, const struct lai_msg *m);

struct lai_conn {
	int fd; /* AF_UNIX stream socket, O_NONBLOCK, close-on-exec */
	int dead; /* 0, or the negative errno that killed the connection */

	/* Receive side: bytes [rx_off, rx_len) are unparsed. */
	uint8_t *rx;
	size_t rx_off, rx_len;

	/* Transmit side: the unsent tail of ONE message that could not be
	 * written in full ([tx_off, tx_len)).  A message is never interleaved
	 * with another, so every send first finishes this one. */
	uint8_t *tx;
	size_t tx_off, tx_len;
	uint32_t tx_type; /* type of the message in tx */

	/* Replies still owed to requests that were given up on (timeout).
	 * They are recognised and dropped when they arrive, so a late reply
	 * is never taken for the answer to a later request. */
	uint32_t skip_replies;

	/* An ERROR that arrived with no request outstanding.  The handle
	 * reports it once from its next call. */
	int async_err;

	/* The service's message for the last error on this connection. */
	int err_code;
	char err_msg[LAI_ERRMSG_MAX];

	int reply_timeout_ms; /* LAI_REPLY_TIMEOUT_MS; tests shorten it */

	struct aud_hello_reply server; /* from the handshake */

	lai_async_fn async;
	void *async_ctx;
};

/* Take ownership of a connected socket.  The socket is made non-blocking
 * and close-on-exec.  On failure the descriptor is closed. */
LAI_HIDDEN int lai_conn_init(struct lai_conn *c, int fd, lai_async_fn fn,
			     void *ctx);
LAI_HIDDEN void lai_conn_destroy(struct lai_conn *c);

/* Connect to `path' with retries.  Returns 0 and *fd, or -errno. */
LAI_HIDDEN int lai_connect(const char *path, int *fd);

/* Send one message made of `iovcnt' pieces (at most 4).  timeout_ms == 0
 * means "do not wait": -EAGAIN when the previous message still cannot be
 * finished and nothing new was queued; a new message that only partly fits
 * is accepted and its tail kept for later.  timeout_ms > 0 waits that long,
 * < 0 forever.  Returns 0 when the message is committed (sent, or its tail
 * stashed), or -errno. */
LAI_HIDDEN int lai_conn_sendv(struct lai_conn *c, uint32_t type,
			      const struct iovec *iov, int iovcnt,
			      int timeout_ms);
LAI_HIDDEN int lai_conn_send(struct lai_conn *c, uint32_t type,
			     const void *payload, size_t len, int timeout_ms);
/* Finish the stashed tail.  0 when nothing is pending any more. */
LAI_HIDDEN int lai_conn_flush(struct lai_conn *c, int timeout_ms);
LAI_HIDDEN int lai_conn_tx_pending(const struct lai_conn *c);
/* Payload bytes of a stashed DATA message not yet on the socket. */
LAI_HIDDEN size_t lai_conn_tx_data_pending(const struct lai_conn *c);

/* Read whatever is available without blocking and handle every complete
 * message (asynchronous ones through the callback).  0 or -errno. */
LAI_HIDDEN int lai_conn_pump(struct lai_conn *c);
/* Wait up to timeout_ms for the socket to become readable (or, with a
 * stashed tail, writable), then pump.  Returns 1 when something happened,
 * 0 on timeout, -errno on error. */
LAI_HIDDEN int lai_conn_wait(struct lai_conn *c, int timeout_ms);

/* Send a request and wait for its reply.
 *
 *   reply_type == AUD_MSG_OK: an empty acknowledgement is expected;
 *   otherwise a message of reply_type whose payload is between reply_min
 *   and reply_max bytes.  *reply / *reply_len (may be NULL) then point at
 *   the payload inside the receive buffer, valid until the next call on
 *   the connection.
 *
 * An AUD_MSG_ERROR reply returns its (validated) negative errno and keeps
 * the service's text in err_msg.  Anything else is a protocol violation and
 * kills the connection.  timeout_ms as for lai_conn_sendv. */
LAI_HIDDEN int lai_conn_request(struct lai_conn *c, uint32_t type,
				const void *req, size_t req_len,
				uint32_t reply_type, size_t reply_min,
				size_t reply_max, const uint8_t **reply,
				uint32_t *reply_len, int timeout_ms);

/* HELLO exchange.  `name' NULL = program name. */
LAI_HIDDEN int lai_conn_hello(struct lai_conn *c, uint32_t kind,
			      const char *name);

/* Mark the connection dead with `err' (negative) and return it. */
LAI_HIDDEN int lai_conn_fail(struct lai_conn *c, int err);
/* Take (and clear) an asynchronous ERROR, or 0. */
LAI_HIDDEN int lai_conn_take_async_err(struct lai_conn *c);

/* ---- helpers ------------------------------------------------------------------ */

/* Copy a name into a fixed protocol field: truncated at a character
 * boundary, control characters replaced, the rest of the field zeroed. */
LAI_HIDDEN void lai_copy_name(char *dst, size_t dstsz, const char *src);
/* Make a received fixed-size string safe to print: NUL-terminated within
 * its field, control characters replaced. */
LAI_HIDDEN void lai_fix_string(char *s, size_t sz);
/* The program's short name, never NULL. */
LAI_HIDDEN const char *lai_program_name(void);
/* Bytes per sample of AUD_FMT_*, 0 when unknown. */
LAI_HIDDEN uint32_t lai_sample_bytes(uint32_t fmt);
/* A server-supplied errno: negative and plausible, else -EIO. */
LAI_HIDDEN int lai_sane_errno(int32_t code);
/* An event sound name the service will accept (see la_play_event). */
LAI_HIDDEN int lai_event_name_ok(const char *name);
static inline uint32_t lai_clamp100(uint32_t v)
{
	return v > 100 ? 100 : v;
}

/* For the host test harness: open handles over an already connected
 * socket instead of AUDIOD_SOCKET_PATH. */
LAI_HIDDEN la_stream *lai_stream_open_fd(int fd, const la_format *fmt,
					 const la_stream_options *opt,
					 int *err);
LAI_HIDDEN la_ctl *lai_ctl_open_fd(int fd, const char *app_name, int *err);
LAI_HIDDEN struct lai_conn *lai_stream_conn(la_stream *s);
LAI_HIDDEN struct lai_conn *lai_ctl_conn(la_ctl *c);

#endif /* LIKEOS_LIBAUDIO_INTERNAL_H */
