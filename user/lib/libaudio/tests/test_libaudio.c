// Host tests for libaudio: a fake audiod on the other end of a socketpair
// checks what the library puts on the wire (message encoding, names, field
// values), drives flow control (credit, partial frames, non-blocking writes
// with a full socket, large writes chunked to the payload limit), and feeds
// the library malformed, late, unsolicited and hostile messages to check
// that every one is caught and nothing is trusted.
//
// Build and run (ASan + UBSan):  make -C user/lib/libaudio host-test
//
// Copyright (C) 2026 The LikeOS Project

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "la_internal.h"

/* ============================================================================
 * Assertions
 * ========================================================================== */

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		__atomic_add_fetch(&g_checks, 1, __ATOMIC_RELAXED);          \
		if (!(cond)) {                                               \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,      \
				__LINE__, #cond);                            \
			__atomic_add_fetch(&g_failures, 1, __ATOMIC_RELAXED); \
		}                                                            \
	} while (0)

#define CHECK_EQ(a, b)                                                       \
	do {                                                                 \
		long long _a = (long long)(a), _b = (long long)(b);          \
		__atomic_add_fetch(&g_checks, 1, __ATOMIC_RELAXED);          \
		if (_a != _b) {                                              \
			fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", \
				__FILE__, __LINE__, #a, #b, _a, _b);         \
			__atomic_add_fetch(&g_failures, 1, __ATOMIC_RELAXED); \
		}                                                            \
	} while (0)

/* ============================================================================
 * Fake service helpers (blocking socket, every wait bounded)
 * ========================================================================== */

#define SRV_TIMEOUT_MS 5000

static int srv_read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		int r = poll(&pf, 1, SRV_TIMEOUT_MS);
		ssize_t k;

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -ETIMEDOUT;
		k = read(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return k == 0 ? -EPIPE : -errno;
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

static int srv_write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t k = send(fd, p, n, MSG_NOSIGNAL);

		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -errno;
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

/* Receive one message; payload up to `cap' bytes.  Returns the type, or a
 * negative errno. */
static int srv_recv(int fd, uint32_t *len, void *buf, size_t cap)
{
	struct aud_header h;
	int r = srv_read_full(fd, &h, sizeof(h));

	if (r < 0)
		return r;
	if (h.len > cap || h.len > AUDIOD_MAX_PAYLOAD) {
		fprintf(stderr, "  server: message type %u too long (%u)\n",
			h.type, h.len);
		return -EMSGSIZE;
	}
	r = srv_read_full(fd, buf, h.len);
	if (r < 0)
		return r;
	*len = h.len;
	return (int)h.type;
}

static void srv_send(int fd, uint32_t type, const void *p, uint32_t len)
{
	struct aud_header h = { .type = type, .len = len };

	CHECK(srv_write_full(fd, &h, sizeof(h)) == 0);
	if (len)
		CHECK(srv_write_full(fd, p, len) == 0);
}

static void srv_ok(int fd)
{
	srv_send(fd, AUD_MSG_OK, NULL, 0);
}

static void srv_error(int fd, int code, const char *msg)
{
	struct aud_error e;

	memset(&e, 0, sizeof(e));
	e.code = code;
	snprintf(e.message, sizeof(e.message), "%s", msg);
	srv_send(fd, AUD_MSG_ERROR, &e, sizeof(e));
}

static void srv_request(int fd, uint32_t bytes)
{
	struct aud_request rq = { .bytes = bytes };

	srv_send(fd, AUD_MSG_REQUEST, &rq, sizeof(rq));
}

static void srv_notify(int fd, uint32_t what, uint32_t a, uint32_t b,
		       uint32_t c)
{
	struct aud_notify n = { .what = what, .a = a, .b = b, .c = c };

	srv_send(fd, AUD_MSG_NOTIFY, &n, sizeof(n));
}

/* Expect a message of `type' with exactly `len' bytes into buf. */
static int srv_expect(int fd, uint32_t type, void *buf, size_t len)
{
	static __thread uint8_t tmp[AUDIOD_MAX_PAYLOAD];
	uint32_t got_len = 0;
	int t = srv_recv(fd, &got_len, tmp, sizeof(tmp));

	if (t < 0) {
		fprintf(stderr, "  server: expected type %u, got error %d\n",
			type, t);
		CHECK(t >= 0);
		return -1;
	}
	if ((uint32_t)t != type || got_len != len) {
		fprintf(stderr,
			"  server: expected type %u len %zu, got %d len %u\n",
			type, len, t, got_len);
		CHECK((uint32_t)t == type && got_len == len);
		return -1;
	}
	if (len && buf)
		memcpy(buf, tmp, len);
	return 0;
}

/* HELLO handshake from the service side. */
static int srv_hello_v(int fd, uint32_t kind, const char *want_name,
		       uint32_t version)
{
	struct aud_hello h;
	struct aud_hello_reply rep;
	size_t i;

	if (srv_expect(fd, AUD_MSG_HELLO, &h, sizeof(h)) < 0)
		return -1;
	CHECK_EQ(h.version, AUDIOD_PROTOCOL_VERSION);
	CHECK_EQ(h.kind, kind);
	if (want_name)
		CHECK(strcmp(h.name, want_name) == 0);
	/* The field is zero-padded: nothing of the client's memory leaks. */
	for (i = strlen(h.name); i < sizeof(h.name); i++)
		CHECK(h.name[i] == 0);
	memset(&rep, 0, sizeof(rep));
	rep.version = version;
	rep.client_id = 42;
	rep.rate = 48000;
	rep.channels = 2;
	rep.format = AUD_FMT_S16_LE;
	srv_send(fd, AUD_MSG_HELLO, &rep, sizeof(rep));
	return 0;
}

static int srv_hello(int fd, uint32_t kind, const char *want_name)
{
	return srv_hello_v(fd, kind, want_name, AUDIOD_PROTOCOL_VERSION);
}

/* STREAM_OPEN from the service side; *o receives the request. */
static int srv_open(int fd, struct aud_stream_open *o, uint32_t id,
		    uint32_t buffer_bytes, uint32_t request_bytes)
{
	struct aud_stream_opened rep;

	if (srv_expect(fd, AUD_MSG_STREAM_OPEN, o, sizeof(*o)) < 0)
		return -1;
	memset(&rep, 0, sizeof(rep));
	rep.stream_id = id;
	rep.buffer_bytes = buffer_bytes;
	rep.request_bytes = request_bytes;
	srv_send(fd, AUD_MSG_STREAM_OPEN, &rep, sizeof(rep));
	return 0;
}

/* ============================================================================
 * Scenario plumbing
 * ========================================================================== */

struct scenario {
	int fd; /* service end */
	sem_t go; /* client -> service */
	sem_t done; /* service -> client */
	volatile size_t shared; /* a number the client hands over */
	void (*fn)(struct scenario *);
	pthread_t th;
};

static void *scenario_thread(void *arg)
{
	struct scenario *sc = arg;

	sc->fn(sc);
	close(sc->fd);
	return NULL;
}

/* Start a fake service running `fn'; returns the client's end. */
static int scenario_start(struct scenario *sc, void (*fn)(struct scenario *))
{
	int sv[2];

	memset(sc, 0, sizeof(*sc));
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		perror("socketpair");
		exit(2);
	}
	sc->fd = sv[1];
	sc->fn = fn;
	sem_init(&sc->go, 0, 0);
	sem_init(&sc->done, 0, 0);
	if (pthread_create(&sc->th, NULL, scenario_thread, sc) != 0) {
		perror("pthread_create");
		exit(2);
	}
	return sv[0];
}

static void scenario_end(struct scenario *sc)
{
	pthread_join(sc->th, NULL);
	sem_destroy(&sc->go);
	sem_destroy(&sc->done);
}

/* Wait for the service thread to signal, bounded. */
static int sem_wait_ms(sem_t *s, int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	while (sem_timedwait(s, &ts) != 0) {
		if (errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static uint8_t pattern(size_t off)
{
	return (uint8_t)((off * 7u + (off >> 8)) & 0xff);
}

static la_stream *open_s16(int fd, uint32_t flags, int *err)
{
	la_format f = { AUD_FMT_S16_LE, 48000, 2 };
	la_stream_options o;

	memset(&o, 0, sizeof(o));
	o.role = AUD_ROLE_MUSIC;
	o.buffer_ms = 50;
	o.flags = flags;
	o.app_name = "testapp";
	o.stream_name = "my stream";
	return lai_stream_open_fd(fd, &f, &o, err);
}

/* ============================================================================
 * 1. Open + blocking write under flow control
 * ========================================================================== */

#define T1_TOTAL 20000u
#define T1_GRANT 4096u

static void t1_service(struct scenario *sc)
{
	static uint8_t buf[AUDIOD_MAX_PAYLOAD];
	struct aud_stream_open o;
	size_t got = 0;
	int64_t credit = T1_GRANT;
	uint32_t len;
	int t;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, "testapp") < 0)
		return;
	if (srv_open(sc->fd, &o, 7, 16384, T1_GRANT) < 0)
		return;
	CHECK_EQ(o.format, AUD_FMT_S16_LE);
	CHECK_EQ(o.rate, 48000);
	CHECK_EQ(o.channels, 2);
	CHECK_EQ(o.role, AUD_ROLE_MUSIC);
	CHECK_EQ(o.buffer_ms, 50);
	CHECK_EQ(o.volume, 100); /* 0 without LA_OPT_VOLUME_ZERO */
	CHECK_EQ(o.flags, 0);
	CHECK_EQ(o.reserved, 0);
	CHECK(strcmp(o.name, "my stream") == 0);

	while (got < T1_TOTAL) {
		size_t i;

		t = srv_recv(sc->fd, &len, buf, sizeof(buf));
		CHECK_EQ(t, AUD_MSG_DATA);
		if (t != AUD_MSG_DATA)
			return;
		CHECK(len % 4 == 0);
		CHECK(len > 0);
		credit -= len;
		CHECK(credit >= 0); /* never more than granted */
		for (i = 0; i < len; i++)
			if (buf[i] != pattern(got + i)) {
				CHECK(buf[i] == pattern(got + i));
				break;
			}
		got += len;
		if (credit == 0 && got < T1_TOTAL) {
			srv_request(sc->fd, T1_GRANT);
			credit += T1_GRANT;
		}
	}
	CHECK_EQ(got, T1_TOTAL);
	/* la_close says goodbye, then the socket closes. */
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_CLOSE, NULL, 0) == 0);
	CHECK(srv_recv(sc->fd, &len, buf, sizeof(buf)) == -EPIPE);
}

static void test_open_and_blocking_write(void)
{
	struct scenario sc;
	static uint8_t data[T1_TOTAL];
	la_stream *s;
	int err = 1;
	size_t i;

	printf("open + blocking write with credit\n");
	for (i = 0; i < sizeof(data); i++)
		data[i] = pattern(i);
	s = open_s16(scenario_start(&sc, t1_service), LA_OPT_VOLUME_ZERO * 0,
		     &err);
	CHECK(s != NULL);
	if (!s) {
		fprintf(stderr, "  open failed: %d\n", err);
		scenario_end(&sc);
		return;
	}
	CHECK_EQ(la_stream_id(s), 7);
	CHECK(la_fd(s) >= 0);
	CHECK_EQ(la_write(s, data, sizeof(data)), (ssize_t)sizeof(data));
	la_close(s);
	scenario_end(&sc);
}

/* ============================================================================
 * 2. Partial frames, drain, latency, errors, stream controls
 * ========================================================================== */

static void t2_service(struct scenario *sc)
{
	struct aud_stream_open o;
	uint8_t buf[64];
	struct aud_latency l;
	struct aud_volume v;
	struct aud_mute m;
	uint32_t len;
	int t, i;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, "testapp") < 0)
		return;
	if (srv_open(sc->fd, &o, 9, 8192, 4096) < 0)
		return;
	/* 3 bytes then 5 bytes of 4-byte frames: one DATA of 8 bytes. */
	t = srv_recv(sc->fd, &len, buf, sizeof(buf));
	CHECK_EQ(t, AUD_MSG_DATA);
	CHECK_EQ(len, 8);
	for (i = 0; i < 8; i++)
		CHECK_EQ(buf[i], i + 1);
	/* 6 more bytes: one frame goes now, 2 bytes wait. */
	t = srv_recv(sc->fd, &len, buf, sizeof(buf));
	CHECK_EQ(t, AUD_MSG_DATA);
	CHECK_EQ(len, 4);

	/* Drain: credit arrives before the acknowledgement. */
	CHECK(srv_expect(sc->fd, AUD_MSG_DRAIN, NULL, 0) == 0);
	srv_request(sc->fd, 12);
	srv_send(sc->fd, AUD_MSG_UNDERRUN, NULL, 0);
	srv_ok(sc->fd);

	/* Latency. */
	CHECK(srv_expect(sc->fd, AUD_MSG_GET_LATENCY, NULL, 0) == 0);
	memset(&l, 0, sizeof(l));
	l.latency_us = 12345;
	l.played_frames = 100;
	l.underruns = 2;
	srv_send(sc->fd, AUD_MSG_GET_LATENCY, &l, sizeof(l));

	/* Pause refused. */
	CHECK(srv_expect(sc->fd, AUD_MSG_PAUSE, NULL, 0) == 0);
	srv_error(sc->fd, -EBUSY, "device \x01 busy");
	/* Resume acknowledged with its own type, empty. */
	CHECK(srv_expect(sc->fd, AUD_MSG_RESUME, NULL, 0) == 0);
	srv_send(sc->fd, AUD_MSG_RESUME, NULL, 0);
	/* Volume. */
	CHECK(srv_expect(sc->fd, AUD_MSG_SET_STREAM_VOLUME, &v, sizeof(v)) ==
	      0);
	CHECK_EQ(v.left, 50);
	CHECK_EQ(v.right, 60);
	CHECK_EQ(v.reserved[0], 0);
	srv_ok(sc->fd);
	/* Mute normalised to 1. */
	CHECK(srv_expect(sc->fd, AUD_MSG_SET_STREAM_MUTE, &m, sizeof(m)) == 0);
	CHECK_EQ(m.mute, 1);
	srv_ok(sc->fd);
	/* Flush. */
	CHECK(srv_expect(sc->fd, AUD_MSG_FLUSH, NULL, 0) == 0);
	srv_ok(sc->fd);
	/* Error code out of range becomes EIO. */
	CHECK(srv_expect(sc->fd, AUD_MSG_PAUSE, NULL, 0) == 0);
	srv_error(sc->fd, 5, "positive");
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_CLOSE, NULL, 0) == 0);
}

static void test_partial_frames_and_controls(void)
{
	struct scenario sc;
	const uint8_t a[3] = { 1, 2, 3 }, b[5] = { 4, 5, 6, 7, 8 };
	const uint8_t c6[6] = { 9, 9, 9, 9, 7, 7 };
	uint64_t lat = 0, played = 0;
	la_stream *s;
	int err = 0;

	printf("partial frames, drain, latency, errors, controls\n");
	s = open_s16(scenario_start(&sc, t2_service), 0, &err);
	CHECK(s != NULL);
	if (!s) {
		scenario_end(&sc);
		return;
	}
	CHECK_EQ(la_write(s, a, 3), 3);
	CHECK_EQ(la_write(s, b, 5), 5);
	CHECK_EQ(la_write(s, c6, 6), 6);
	/* The 2 stashed bytes are dropped by the drain. */
	CHECK_EQ(la_drain(s), 0);
	CHECK_EQ(la_latency(s, &lat, &played), 0);
	CHECK_EQ(lat, 12345);
	CHECK_EQ(played, 100);
	CHECK_EQ(la_pause(s), -EBUSY);
	CHECK(strcmp(lai_stream_conn(s)->err_msg, "device   busy") == 0);
	CHECK_EQ(la_resume(s), 0);
	CHECK_EQ(la_set_volume(s, 101, 0), -EINVAL); /* nothing sent */
	CHECK_EQ(la_set_volume(s, 50, 60), 0);
	CHECK_EQ(la_set_mute(s, 5), 0);
	CHECK_EQ(la_flush(s), 0);
	CHECK_EQ(la_pause(s), -EIO);
	/* Credit from the drain: 12 bytes + 4096 - 12 sent. */
	CHECK_EQ(la_writable(s), 4096);
	la_close(s);
	scenario_end(&sc);
}

/* ============================================================================
 * 3. Non-blocking: no credit, then credit via la_process
 * ========================================================================== */

static void t3_service(struct scenario *sc)
{
	struct aud_stream_open o;
	uint8_t buf[512];
	uint32_t len;
	int t;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	if (srv_open(sc->fd, &o, 3, 1024, 0) < 0)
		return;
	CHECK_EQ(o.flags, AUD_STREAM_START_PAUSED); /* LA_OPT_* stripped */
	CHECK(sem_wait_ms(&sc->go, SRV_TIMEOUT_MS) == 0);
	srv_request(sc->fd, 256);
	t = srv_recv(sc->fd, &len, buf, sizeof(buf));
	CHECK_EQ(t, AUD_MSG_DATA);
	CHECK_EQ(len, 256);
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_CLOSE, NULL, 0) == 0);
}

static void test_nonblocking_credit(void)
{
	struct scenario sc;
	uint8_t data[400];
	struct pollfd pf;
	la_stream *s;
	int err = 0, i;

	printf("non-blocking write and credit\n");
	memset(data, 0x55, sizeof(data));
	s = open_s16(scenario_start(&sc, t3_service),
		     LA_OPT_NONBLOCK | AUD_STREAM_START_PAUSED, &err);
	CHECK(s != NULL);
	if (!s) {
		scenario_end(&sc);
		return;
	}
	CHECK_EQ(la_write(s, data, sizeof(data)), -EAGAIN);
	CHECK_EQ(la_writable(s), 0);
	sem_post(&sc.go);
	pf.fd = la_fd(s);
	pf.events = POLLIN;
	pf.revents = 0;
	CHECK_EQ(poll(&pf, 1, 3000), 1);
	for (i = 0; i < 50 && la_writable(s) == 0; i++)
		usleep(10000);
	CHECK_EQ(la_process(s), 0);
	CHECK_EQ(la_writable(s), 256);
	CHECK_EQ(la_write(s, data, sizeof(data)), 256);
	CHECK_EQ(la_write(s, data, sizeof(data)), -EAGAIN);
	la_close(s);
	scenario_end(&sc);
}

/* ============================================================================
 * 4. Non-blocking with a full socket: the stashed tail keeps framing
 * ========================================================================== */

static void t4_service(struct scenario *sc)
{
	static uint8_t buf[AUDIOD_MAX_PAYLOAD];
	struct aud_stream_open o;
	size_t got = 0, total, i;
	uint32_t len;
	int t, bad = 0;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	if (srv_open(sc->fd, &o, 4, 1 << 20, 32u << 20) < 0)
		return;
	/* Read nothing until the client has filled the socket. */
	CHECK(sem_wait_ms(&sc->go, 20000) == 0);
	total = sc->shared;
	while (got < total) {
		t = srv_recv(sc->fd, &len, buf, sizeof(buf));
		CHECK_EQ(t, AUD_MSG_DATA);
		if (t != AUD_MSG_DATA)
			return;
		CHECK(len % 4 == 0 && len <= AUDIOD_MAX_PAYLOAD);
		for (i = 0; i < len && !bad; i++)
			if (buf[i] != pattern(got + i))
				bad = 1;
		got += len;
	}
	CHECK(!bad);
	CHECK_EQ(got, total);
	CHECK(srv_expect(sc->fd, AUD_MSG_DRAIN, NULL, 0) == 0);
	srv_ok(sc->fd);
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_CLOSE, NULL, 0) == 0);
}

static void test_nonblocking_full_socket(void)
{
	struct scenario sc;
	static uint8_t data[1 << 20];
	size_t accepted = 0, i;
	la_stream *s;
	int err = 0, rounds = 0;
	ssize_t r;

	printf("non-blocking write into a full socket\n");
	s = open_s16(scenario_start(&sc, t4_service), LA_OPT_NONBLOCK, &err);
	CHECK(s != NULL);
	if (!s) {
		scenario_end(&sc);
		return;
	}
	/* Write a continuous pattern until the socket refuses. */
	for (;;) {
		size_t n = 100000 + 2; /* not frame aligned on purpose */

		for (i = 0; i < n; i++)
			data[i] = pattern(accepted + i);
		r = la_write(s, data, n);
		if (r == -EAGAIN)
			break;
		CHECK(r > 0);
		if (r <= 0)
			break;
		accepted += (size_t)r;
		if (++rounds > 1000) {
			CHECK(!"socket never filled");
			break;
		}
	}
	CHECK(lai_conn_tx_pending(lai_stream_conn(s)));
	CHECK_EQ(la_writable(s), 0);
	/* What went out as DATA is all but a stashed partial frame. */
	sc.shared = accepted - (accepted % 4);
	sem_post(&sc.go);
	CHECK_EQ(la_drain(s), 0);
	la_close(s);
	scenario_end(&sc);
}

/* ============================================================================
 * 5. Timeouts: a late reply is not taken for the next one
 * ========================================================================== */

static void t5_service(struct scenario *sc)
{
	struct aud_stream_open o;
	struct aud_latency l;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	if (srv_open(sc->fd, &o, 5, 1024, 1024) < 0)
		return;
	CHECK(srv_expect(sc->fd, AUD_MSG_GET_LATENCY, NULL, 0) == 0);
	usleep(400 * 1000); /* the client gives up after 150 ms */
	memset(&l, 0, sizeof(l));
	l.latency_us = 999;
	srv_send(sc->fd, AUD_MSG_GET_LATENCY, &l, sizeof(l));
	CHECK(srv_expect(sc->fd, AUD_MSG_PAUSE, NULL, 0) == 0);
	srv_ok(sc->fd);
	CHECK(srv_expect(sc->fd, AUD_MSG_GET_LATENCY, NULL, 0) == 0);
	l.latency_us = 777;
	srv_send(sc->fd, AUD_MSG_GET_LATENCY, &l, sizeof(l));
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_CLOSE, NULL, 0) == 0);
}

static void test_timeout_late_reply(void)
{
	struct scenario sc;
	uint64_t lat = 0;
	la_stream *s;
	int err = 0;

	printf("reply timeout and late reply\n");
	s = open_s16(scenario_start(&sc, t5_service), 0, &err);
	CHECK(s != NULL);
	if (!s) {
		scenario_end(&sc);
		return;
	}
	lai_stream_conn(s)->reply_timeout_ms = 150;
	CHECK_EQ(la_latency(s, &lat, NULL), -ETIMEDOUT);
	lai_stream_conn(s)->reply_timeout_ms = 3000;
	/* The late latency reply arrives first and must be skipped. */
	CHECK_EQ(la_pause(s), 0);
	CHECK_EQ(la_latency(s, &lat, NULL), 0);
	CHECK_EQ(lat, 777);
	la_close(s);
	scenario_end(&sc);
}

/* ============================================================================
 * 6. Protocol violations on a stream connection
 * ========================================================================== */

enum violation {
	V_HUGE_LEN,
	V_SHORT_REQUEST,
	V_UNKNOWN_TYPE,
	V_UNSOLICITED_OK,
	V_BAD_LATENCY_SIZE,
	V_BIG_ERROR,
	V_BIG_UNDERRUN,
	V_NOTIFY_ON_STREAM,
	V_COUNT
};

static volatile int g_violation;

static void t6_service(struct scenario *sc)
{
	struct aud_stream_open o;
	struct aud_header h;
	uint8_t junk[200];

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	if (srv_open(sc->fd, &o, 6, 1024, 0) < 0)
		return;
	memset(junk, 0x41, sizeof(junk));
	switch (g_violation) {
	case V_HUGE_LEN:
		h.type = AUD_MSG_REQUEST;
		h.len = AUDIOD_MAX_PAYLOAD + 1;
		CHECK(srv_write_full(sc->fd, &h, sizeof(h)) == 0);
		break;
	case V_SHORT_REQUEST:
		srv_send(sc->fd, AUD_MSG_REQUEST, junk, 4);
		break;
	case V_UNKNOWN_TYPE:
		srv_send(sc->fd, 999, NULL, 0);
		break;
	case V_UNSOLICITED_OK:
		srv_ok(sc->fd);
		break;
	case V_BAD_LATENCY_SIZE:
		CHECK(srv_expect(sc->fd, AUD_MSG_GET_LATENCY, NULL, 0) == 0);
		srv_send(sc->fd, AUD_MSG_GET_LATENCY, junk, 16);
		break;
	case V_BIG_ERROR:
		CHECK(srv_expect(sc->fd, AUD_MSG_GET_LATENCY, NULL, 0) == 0);
		srv_send(sc->fd, AUD_MSG_ERROR, junk, sizeof(junk));
		break;
	case V_BIG_UNDERRUN:
		srv_send(sc->fd, AUD_MSG_UNDERRUN, junk, 100);
		break;
	case V_NOTIFY_ON_STREAM:
		srv_notify(sc->fd, AUD_NOTIFY_VOLUME, 1, 2, 3);
		break;
	}
	/* Wait for the client to finish with the connection. */
	CHECK(sem_wait_ms(&sc->go, SRV_TIMEOUT_MS) == 0);
}

static void test_protocol_violations(void)
{
	static const char *const names[V_COUNT] = {
		"oversized length", "short REQUEST",  "unknown type",
		"unsolicited OK",   "bad reply size", "oversized ERROR",
		"oversized UNDERRUN", "NOTIFY on a stream",
	};
	int v;

	for (v = 0; v < V_COUNT; v++) {
		struct scenario sc;
		uint64_t lat;
		la_stream *s;
		int err = 0, r = 0, i;

		printf("protocol violation: %s\n", names[v]);
		g_violation = v;
		s = open_s16(scenario_start(&sc, t6_service), 0, &err);
		CHECK(s != NULL);
		if (!s) {
			sem_post(&sc.go);
			scenario_end(&sc);
			continue;
		}
		if (v == V_BAD_LATENCY_SIZE || v == V_BIG_ERROR) {
			r = la_latency(s, &lat, NULL);
		} else {
			/* The message arrives asynchronously; poll for it. */
			for (i = 0; i < 300; i++) {
				r = la_process(s);
				if (r != 0)
					break;
				usleep(5000);
			}
		}
		CHECK_EQ(r, -EPROTO);
		/* Dead for good. */
		CHECK_EQ(la_write(s, "abcd", 4), -EPROTO);
		CHECK_EQ(la_drain(s), -EPROTO);
		CHECK_EQ(la_process(s), -EPROTO);
		sem_post(&sc.go);
		la_close(s);
		scenario_end(&sc);
	}
}

/* ============================================================================
 * 7. Hello problems, service closing, async errors, underruns
 * ========================================================================== */

static void t7a_service(struct scenario *sc)
{
	srv_hello_v(sc->fd, AUD_CLIENT_STREAM, NULL, 2);
	CHECK(sem_wait_ms(&sc->go, SRV_TIMEOUT_MS) == 0);
}

static void t7b_service(struct scenario *sc)
{
	struct aud_stream_open o;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_OPEN, &o, sizeof(o)) == 0);
	srv_error(sc->fd, -EDQUOT, "too many streams for uid 1000");
}

static void t7c_service(struct scenario *sc)
{
	struct aud_stream_open o;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	if (srv_open(sc->fd, &o, 8, 1024, 4096) < 0)
		return;
	/* Leave: the socket closes when this returns. */
}

static void t7d_service(struct scenario *sc)
{
	struct aud_stream_open o;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	if (srv_open(sc->fd, &o, 8, 1024, 0) < 0)
		return;
	srv_send(sc->fd, AUD_MSG_UNDERRUN, NULL, 0);
	srv_send(sc->fd, AUD_MSG_UNDERRUN, NULL, 0);
	srv_error(sc->fd, -EDQUOT, "limit");
	CHECK(sem_wait_ms(&sc->go, SRV_TIMEOUT_MS) == 0);
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_CLOSE, NULL, 0) == 0);
}

static void test_failures(void)
{
	struct scenario sc;
	static uint8_t data[8192];
	la_stream *s;
	int err = 0, i, r = 0;

	printf("hello with a different protocol version\n");
	s = open_s16(scenario_start(&sc, t7a_service), 0, &err);
	CHECK(s == NULL);
	CHECK_EQ(err, -EPROTONOSUPPORT);
	sem_post(&sc.go);
	scenario_end(&sc);

	printf("stream open refused\n");
	s = open_s16(scenario_start(&sc, t7b_service), 0, &err);
	CHECK(s == NULL);
	CHECK_EQ(err, -EDQUOT);
	scenario_end(&sc);

	printf("service goes away (no SIGPIPE)\n");
	s = open_s16(scenario_start(&sc, t7c_service), 0, &err);
	CHECK(s != NULL);
	scenario_end(&sc); /* the service end is closed now */
	if (s) {
		ssize_t w = la_write(s, data, sizeof(data));

		/* The first send may still land in the dead socket's buffer
		 * on some systems; the error must surface by the next. */
		if (w > 0)
			w = la_write(s, data, sizeof(data));
		CHECK_EQ(w, -EPIPE);
		CHECK_EQ(la_drain(s), -EPIPE);
		CHECK_EQ(la_writable(s), -EPIPE);
		la_close(s);
	}

	printf("asynchronous error and underruns\n");
	s = open_s16(scenario_start(&sc, t7d_service), 0, &err);
	CHECK(s != NULL);
	if (s) {
		for (i = 0; i < 300; i++) {
			r = la_process(s);
			if (r != 0)
				break;
			usleep(5000);
		}
		CHECK_EQ(r, -EDQUOT);
		CHECK(strcmp(lai_stream_conn(s)->err_msg, "limit") == 0);
		CHECK_EQ(la_process(s), 0); /* reported once */
		sem_post(&sc.go);
		la_close(s);
	} else {
		sem_post(&sc.go);
	}
	scenario_end(&sc);
}

/* ============================================================================
 * 8. Large blocking write: chunks never exceed the payload limit
 * ========================================================================== */

#define T8_TOTAL (300000u - 300000u % 6u)

static void t8_service(struct scenario *sc)
{
	static uint8_t buf[AUDIOD_MAX_PAYLOAD];
	struct aud_stream_open o;
	size_t got = 0, granted = 1 << 17, i;
	uint32_t len;
	int t, bad = 0, msgs = 0;

	if (srv_hello(sc->fd, AUD_CLIENT_STREAM, NULL) < 0)
		return;
	if (srv_open(sc->fd, &o, 10, 1 << 18, 1 << 17) < 0)
		return;
	CHECK_EQ(o.channels, 3);
	while (got < T8_TOTAL) {
		t = srv_recv(sc->fd, &len, buf, sizeof(buf));
		CHECK_EQ(t, AUD_MSG_DATA);
		if (t != AUD_MSG_DATA)
			return;
		CHECK(len % 6 == 0);
		CHECK(len <= AUDIOD_MAX_PAYLOAD);
		for (i = 0; i < len && !bad; i++)
			if (buf[i] != pattern(got + i))
				bad = 1;
		got += len;
		msgs++;
		/* Grant only what the client still needs: once it holds
		 * credit for everything it may finish and close at once. */
		if (granted < T8_TOTAL) {
			srv_request(sc->fd, len);
			granted += len;
		}
	}
	CHECK(!bad);
	CHECK(msgs >= 5);
	CHECK(srv_expect(sc->fd, AUD_MSG_STREAM_CLOSE, NULL, 0) == 0);
}

static void test_large_write(void)
{
	struct scenario sc;
	static uint8_t data[T8_TOTAL];
	la_format f = { AUD_FMT_S16_LE, 44100, 3 };
	la_stream *s;
	int err = 0;
	size_t i;

	printf("large blocking write, 3 channels\n");
	for (i = 0; i < sizeof(data); i++)
		data[i] = pattern(i);
	s = lai_stream_open_fd(scenario_start(&sc, t8_service), &f, NULL,
			       &err);
	CHECK(s != NULL);
	if (s) {
		CHECK_EQ(la_write(s, data, sizeof(data)),
			 (ssize_t)sizeof(data));
		la_close(s);
	}
	scenario_end(&sc);
}

/* ============================================================================
 * 9. Control connection
 * ========================================================================== */

static void fill_unterminated(char *s, size_t n, char c)
{
	memset(s, c, n);
}

static void t9_service(struct scenario *sc)
{
	struct aud_volume v;
	struct aud_mute m;
	struct aud_outputs outs;
	struct aud_cards cards;
	struct aud_server_info si;
	struct aud_set_output so;
	struct aud_volume_of vo;
	struct aud_stream_ref ref;
	static uint8_t list[sizeof(struct aud_stream_list) +
			    3 * sizeof(struct aud_stream_entry)];
	struct aud_stream_list *sl = (struct aud_stream_list *)list;
	int i;

	if (srv_hello(sc->fd, AUD_CLIENT_CONTROL, "ctltest") < 0)
		return;

	CHECK(srv_expect(sc->fd, AUD_MSG_GET_MASTER_VOLUME, NULL, 0) == 0);
	memset(&v, 0, sizeof(v));
	v.left = 150;
	v.right = 30;
	srv_send(sc->fd, AUD_MSG_GET_MASTER_VOLUME, &v, sizeof(v));

	CHECK(srv_expect(sc->fd, AUD_MSG_SET_MASTER_VOLUME, &v, sizeof(v)) ==
	      0);
	CHECK_EQ(v.left, 70);
	CHECK_EQ(v.right, 80);
	srv_ok(sc->fd);

	CHECK(srv_expect(sc->fd, AUD_MSG_GET_MUTE, NULL, 0) == 0);
	m.mute = 7;
	m.reserved = 0;
	srv_send(sc->fd, AUD_MSG_GET_MUTE, &m, sizeof(m));

	CHECK(srv_expect(sc->fd, AUD_MSG_SET_MUTE, &m, sizeof(m)) == 0);
	CHECK_EQ(m.mute, 0);
	srv_ok(sc->fd);

	CHECK(srv_expect(sc->fd, AUD_MSG_GET_OUTPUTS, NULL, 0) == 0);
	memset(&outs, 0, sizeof(outs));
	outs.count = 99;
	for (i = 0; i < AUD_MAX_OUTPUTS; i++) {
		outs.out[i].id = (uint32_t)i;
		outs.out[i].present = 5;
		fill_unterminated(outs.out[i].name, sizeof(outs.out[i].name),
				  'A');
		fill_unterminated(outs.out[i].monitor,
				  sizeof(outs.out[i].monitor), '\n');
	}
	srv_send(sc->fd, AUD_MSG_GET_OUTPUTS, &outs, sizeof(outs));

	CHECK(srv_expect(sc->fd, AUD_MSG_SET_OUTPUT, &so, sizeof(so)) == 0);
	CHECK_EQ(so.mode, 2);
	CHECK_EQ(so.id, 3);
	srv_ok(sc->fd);

	CHECK(srv_expect(sc->fd, AUD_MSG_GET_CARDS, NULL, 0) == 0);
	memset(&cards, 0, sizeof(cards));
	cards.count = 2;
	snprintf(cards.card[0].name, sizeof(cards.card[0].name), "HDA Intel");
	fill_unterminated(cards.card[1].driver, sizeof(cards.card[1].driver),
			  'x');
	fill_unterminated(cards.card[5].name, sizeof(cards.card[5].name),
			  'y'); /* beyond count: must be cleared */
	srv_send(sc->fd, AUD_MSG_GET_CARDS, &cards, sizeof(cards));

	CHECK(srv_expect(sc->fd, AUD_MSG_SET_CARD, &ref, sizeof(ref)) == 0);
	CHECK_EQ(ref.id, 1);
	srv_ok(sc->fd);

	CHECK(srv_expect(sc->fd, AUD_MSG_GET_SERVER_INFO, NULL, 0) == 0);
	memset(&si, 0, sizeof(si));
	si.version = 1;
	si.rate = 48000;
	si.format = 99;
	fill_unterminated(si.card_name, sizeof(si.card_name), 'c');
	fill_unterminated(si.codec, sizeof(si.codec), 'd');
	snprintf(si.driver, sizeof(si.driver), "hda");
	srv_send(sc->fd, AUD_MSG_GET_SERVER_INFO, &si, sizeof(si));

	/* Stream list with 3 entries, twice. */
	for (i = 0; i < 2; i++) {
		int k;

		CHECK(srv_expect(sc->fd, AUD_MSG_LIST_STREAMS, NULL, 0) == 0);
		memset(list, 0, sizeof(list));
		sl->count = 3;
		for (k = 0; k < 3; k++) {
			sl->entry[k].stream_id = 100 + (uint32_t)k;
			sl->entry[k].role = (k == 2) ? 77 : AUD_ROLE_WEB;
			sl->entry[k].left = 200;
			sl->entry[k].state = 9;
			fill_unterminated(sl->entry[k].name,
					  sizeof(sl->entry[k].name), 'n');
			snprintf(sl->entry[k].app, sizeof(sl->entry[k].app),
				 "app%d", k);
		}
		srv_send(sc->fd, AUD_MSG_LIST_STREAMS, list, sizeof(list));
	}

	CHECK(srv_expect(sc->fd, AUD_MSG_SET_VOLUME_OF, &vo, sizeof(vo)) == 0);
	CHECK_EQ(vo.stream_id, 101);
	CHECK_EQ(vo.left, 10);
	CHECK_EQ(vo.right, 20);
	srv_error(sc->fd, -EPERM, "not your stream");

	CHECK(srv_expect(sc->fd, AUD_MSG_KILL_STREAM, &ref, sizeof(ref)) == 0);
	CHECK_EQ(ref.id, 102);
	srv_ok(sc->fd);

	/* A list whose count does not match its length. */
	CHECK(srv_expect(sc->fd, AUD_MSG_LIST_STREAMS, NULL, 0) == 0);
	sl->count = 3;
	srv_send(sc->fd, AUD_MSG_LIST_STREAMS, list,
		 (uint32_t)(sizeof(struct aud_stream_list) +
			    2 * sizeof(struct aud_stream_entry)));
	CHECK(sem_wait_ms(&sc->go, SRV_TIMEOUT_MS) == 0);
}

static void test_control(void)
{
	struct scenario sc;
	struct aud_outputs outs;
	struct aud_cards cards;
	struct aud_server_info si;
	struct aud_stream_entry e[3];
	uint32_t l = 0, r = 0;
	int mute = -1, i;
	la_ctl *c;
	int err = 0;

	printf("control requests\n");
	c = lai_ctl_open_fd(scenario_start(&sc, t9_service), "ctltest", &err);
	CHECK(c != NULL);
	if (!c) {
		sem_post(&sc.go);
		scenario_end(&sc);
		return;
	}
	CHECK(la_ctl_fd(c) >= 0);
	CHECK_EQ(la_get_volume(c, &l, &r), 0);
	CHECK_EQ(l, 100); /* clamped */
	CHECK_EQ(r, 30);
	CHECK_EQ(la_set_volume_master(c, 70, 101), -EINVAL);
	CHECK_EQ(la_set_volume_master(c, 70, 80), 0);
	CHECK_EQ(la_get_mute(c, &mute), 0);
	CHECK_EQ(mute, 1);
	CHECK_EQ(la_set_mute_master(c, 0), 0);
	CHECK_EQ(la_get_outputs(c, &outs), 0);
	CHECK_EQ(outs.count, AUD_MAX_OUTPUTS);
	CHECK_EQ(strlen(outs.out[0].name), sizeof(outs.out[0].name) - 1);
	CHECK_EQ(outs.out[3].present, 1);
	CHECK(outs.out[15].monitor[0] == ' ');
	CHECK_EQ(la_set_output(c, 2, 3), 0);
	CHECK_EQ(la_get_cards(c, &cards), 0);
	CHECK_EQ(cards.count, 2);
	CHECK(strcmp(cards.card[0].name, "HDA Intel") == 0);
	CHECK_EQ(strlen(cards.card[1].driver), sizeof(cards.card[1].driver) - 1);
	CHECK_EQ(cards.card[5].name[0], 0);
	CHECK_EQ(la_set_card(c, 1), 0);
	CHECK_EQ(la_get_server_info(c, &si), 0);
	CHECK_EQ(si.format, 0);
	CHECK_EQ(strlen(si.card_name), sizeof(si.card_name) - 1);
	CHECK(strcmp(si.driver, "hda") == 0);

	memset(e, 0xEE, sizeof(e));
	CHECK_EQ(la_list_streams(c, e, 2), 2);
	CHECK_EQ(e[0].stream_id, 100);
	CHECK_EQ(e[1].stream_id, 101);
	CHECK_EQ(e[0].left, 100);
	CHECK_EQ(e[0].state, 0);
	CHECK_EQ(strlen(e[0].name), sizeof(e[0].name) - 1);
	CHECK(strcmp(e[1].app, "app1") == 0);
	CHECK_EQ(e[2].stream_id, 0xEEEEEEEEu); /* untouched */
	CHECK_EQ(la_list_streams(c, NULL, 0), 3);
	CHECK_EQ(la_list_streams(c, NULL, 2), -EINVAL);
	CHECK_EQ(la_list_streams(c, e, -1), -EINVAL);

	CHECK_EQ(la_set_stream_volume(c, 101, 10, 20), -EPERM);
	CHECK(strcmp(lai_ctl_conn(c)->err_msg, "not your stream") == 0);
	CHECK_EQ(la_kill_stream(c, 102), 0);

	CHECK_EQ(la_list_streams(c, e, 3), -EPROTO);
	CHECK_EQ(la_get_volume(c, &l, &r), -EPROTO); /* dead now */
	for (i = 0; i < 1; i++)
		sem_post(&sc.go);
	la_ctl_close(c);
	scenario_end(&sc);
}

/* ============================================================================
 * 10. Subscriptions and notification queueing
 * ========================================================================== */

#define T10_FLOOD 300

static void t10_service(struct scenario *sc)
{
	struct aud_subscribe sb;
	struct aud_mute m;
	int i;

	if (srv_hello(sc->fd, AUD_CLIENT_CONTROL, NULL) < 0)
		return;
	CHECK(srv_expect(sc->fd, AUD_MSG_SUBSCRIBE, &sb, sizeof(sb)) == 0);
	CHECK_EQ(sb.mask, AUD_SUB_VOLUME | AUD_SUB_STREAMS);
	CHECK_EQ(sb.reserved, 0);
	/* Notifications overtake the acknowledgement. */
	srv_notify(sc->fd, AUD_NOTIFY_VOLUME, 120, 40, 9);
	srv_notify(sc->fd, 99, 0, 0, 0); /* unknown kind: dropped */
	srv_notify(sc->fd, AUD_NOTIFY_STREAM_NEW, 17, 5, 5);
	srv_ok(sc->fd);

	/* A flood larger than the queue, ahead of a reply. */
	CHECK(srv_expect(sc->fd, AUD_MSG_GET_MUTE, NULL, 0) == 0);
	for (i = 0; i < T10_FLOOD; i++)
		srv_notify(sc->fd, AUD_NOTIFY_STREAM_GONE, (uint32_t)i, 0, 0);
	memset(&m, 0, sizeof(m));
	srv_send(sc->fd, AUD_MSG_GET_MUTE, &m, sizeof(m));

	/* Asynchronous notification, then a malformed one. */
	CHECK(sem_wait_ms(&sc->go, SRV_TIMEOUT_MS) == 0);
	srv_notify(sc->fd, AUD_NOTIFY_CARD, 1, 1, 0);
	srv_send(sc->fd, AUD_MSG_NOTIFY, &m, 4);
	CHECK(sem_wait_ms(&sc->go, SRV_TIMEOUT_MS) == 0);
}

static void test_subscribe(void)
{
	struct scenario sc;
	struct aud_notify n;
	int mute, i, r = 0, first = -1, count = 0, last = -1;
	la_ctl *c;
	int err = 0;

	printf("subscriptions and notification queue\n");
	c = lai_ctl_open_fd(scenario_start(&sc, t10_service), NULL, &err);
	CHECK(c != NULL);
	if (!c) {
		sem_post(&sc.go);
		sem_post(&sc.go);
		scenario_end(&sc);
		return;
	}
	CHECK_EQ(la_subscribe(c, 1u << 9), -EINVAL);
	CHECK_EQ(la_subscribe(c, AUD_SUB_VOLUME | AUD_SUB_STREAMS), 0);
	CHECK_EQ(la_next_notify(c, &n), 1);
	CHECK_EQ(n.what, AUD_NOTIFY_VOLUME);
	CHECK_EQ(n.a, 100);
	CHECK_EQ(n.b, 40);
	CHECK_EQ(n.c, 1);
	CHECK_EQ(la_next_notify(c, &n), 1);
	CHECK_EQ(n.what, AUD_NOTIFY_STREAM_NEW);
	CHECK_EQ(n.a, 17);
	CHECK_EQ(n.b, 0);
	CHECK_EQ(la_next_notify(c, &n), 0);

	CHECK_EQ(la_get_mute(c, &mute), 0);
	CHECK_EQ(mute, 0);
	while ((r = la_next_notify(c, &n)) == 1) {
		if (first < 0)
			first = (int)n.a;
		CHECK(last < 0 || (int)n.a == last + 1);
		last = (int)n.a;
		count++;
	}
	CHECK_EQ(r, 0);
	CHECK_EQ(count, LAI_NOTIFY_QUEUE);
	CHECK_EQ(first, T10_FLOOD - LAI_NOTIFY_QUEUE);
	CHECK_EQ(last, T10_FLOOD - 1);

	sem_post(&sc.go);
	for (i = 0, r = 0; i < 300; i++) {
		r = la_next_notify(c, &n);
		if (r != 0)
			break;
		usleep(5000);
	}
	CHECK_EQ(r, 1);
	CHECK_EQ(n.what, AUD_NOTIFY_CARD);
	for (i = 0, r = 0; i < 300; i++) {
		r = la_next_notify(c, &n);
		if (r != 0)
			break;
		usleep(5000);
	}
	CHECK_EQ(r, -EPROTO);
	sem_post(&sc.go);
	la_ctl_close(c);
	scenario_end(&sc);
}

/* ============================================================================
 * 11. Event sounds
 * ========================================================================== */

static void t11_service(struct scenario *sc)
{
	struct aud_play_event ev;
	size_t i;

	if (srv_hello(sc->fd, AUD_CLIENT_CONTROL, NULL) < 0)
		return;
	CHECK(srv_expect(sc->fd, AUD_MSG_PLAY_EVENT, &ev, sizeof(ev)) == 0);
	CHECK(strcmp(ev.name, "startup") == 0);
	for (i = strlen(ev.name); i < sizeof(ev.name); i++)
		CHECK(ev.name[i] == 0);
	CHECK_EQ(ev.volume, 80);
	CHECK_EQ(ev.wait, 1);
	usleep(100 * 1000); /* "playing" */
	srv_ok(sc->fd);
	CHECK(srv_expect(sc->fd, AUD_MSG_PLAY_EVENT, &ev, sizeof(ev)) == 0);
	CHECK(strcmp(ev.name, "volume.tick_2") == 0);
	CHECK_EQ(ev.wait, 0);
	srv_error(sc->fd, -ENOENT, "no such sound");
}

static void test_events(void)
{
	static const char *const bad[] = {
		"",	  "../x", "a/b",	 ".hidden", "-opt", "x y",
		"a..b", "tab\t", "ümlaut", "semi;colon",
	};
	struct scenario sc;
	la_ctl *c;
	size_t i;
	int err = 0;

	printf("event sounds\n");
	c = lai_ctl_open_fd(scenario_start(&sc, t11_service), NULL, &err);
	CHECK(c != NULL);
	if (!c) {
		scenario_end(&sc);
		return;
	}
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK_EQ(la_play_event(c, bad[i], 50, 0), -EINVAL);
	CHECK_EQ(la_play_event(c, "0123456789012345678901234567890123", 50,
			       0),
		 -ENAMETOOLONG);
	CHECK_EQ(la_play_event(c, NULL, 50, 0), -EINVAL);
	CHECK_EQ(la_play_event(c, "startup", 101, 0), -EINVAL);
	CHECK_EQ(la_play_event(c, "startup", 80, 1), 0);
	CHECK_EQ(la_play_event(c, "volume.tick_2", 10, 0), -ENOENT);
	CHECK_EQ(la_play_event_simple("../../etc/shadow", 0), -EINVAL);
	la_ctl_close(c);
	scenario_end(&sc);
}

/* ============================================================================
 * 12. Helpers and argument checks
 * ========================================================================== */

static void test_helpers(void)
{
	la_format f;
	uint32_t v, fmt;
	char name[8];
	int err = 0;
	la_stream_options o;

	printf("helpers and argument checks\n");
	for (fmt = 1; fmt <= AUD_FMT_MAX; fmt++) {
		CHECK_EQ(la_parse_format(la_format_name(fmt), &v), 0);
		CHECK_EQ(v, fmt);
	}
	CHECK(strcmp(la_format_name(0), "unknown") == 0);
	CHECK(strcmp(la_format_name(99), "unknown") == 0);
	CHECK_EQ(la_parse_format("S16_LE", &v), 0);
	CHECK_EQ(v, AUD_FMT_S16_LE);
	CHECK_EQ(la_parse_format("float", &v), 0);
	CHECK_EQ(v, AUD_FMT_F32_LE);
	CHECK_EQ(la_parse_format("S24_3LE", &v), 0);
	CHECK_EQ(v, AUD_FMT_S24_3LE);
	CHECK_EQ(la_parse_format("6", &v), 0);
	CHECK_EQ(v, 6);
	CHECK_EQ(la_parse_format("9", &v), -EINVAL);
	CHECK_EQ(la_parse_format("99999999999", &v), -EINVAL);
	CHECK_EQ(la_parse_format("bogus", &v), -EINVAL);
	CHECK_EQ(la_parse_format(NULL, &v), -EINVAL);

	for (v = AUD_ROLE_MUSIC; v <= AUD_ROLE_OTHER; v++) {
		uint32_t r2;

		CHECK_EQ(la_parse_role(la_role_name(v), &r2), 0);
		CHECK_EQ(r2, v);
	}
	CHECK_EQ(la_parse_role("Browser", &v), 0);
	CHECK_EQ(v, AUD_ROLE_WEB);
	CHECK_EQ(la_parse_role("notification", &v), 0);
	CHECK_EQ(v, AUD_ROLE_EVENT);
	CHECK_EQ(la_parse_role("0", &v), -EINVAL);
	CHECK(strcmp(la_role_name(0), "unknown") == 0);

	f.format = AUD_FMT_S24_3LE;
	f.rate = 48000;
	f.channels = 6;
	CHECK_EQ(la_frame_bytes(&f), 18);
	f.format = AUD_FMT_F64_LE;
	f.channels = 8;
	CHECK_EQ(la_frame_bytes(&f), 64);
	f.channels = 9;
	CHECK_EQ(la_frame_bytes(&f), 0);
	CHECK_EQ(la_frame_bytes(NULL), 0);

	CHECK(strcmp(la_strerror(0), "success") == 0);
	CHECK(strcmp(la_strerror(-EPIPE), la_strerror(EPIPE)) == 0);
	CHECK(strcmp(la_strerror(-123456), "unknown error") == 0);
	CHECK(strstr(la_strerror(-ECONNREFUSED), "not running") != NULL);

	/* Name copying: truncation keeps whole UTF-8 characters, control
	 * characters are replaced, the rest is zero. */
	lai_copy_name(name, sizeof(name), "abcd\xc3\xa4\xc3\xa4");
	CHECK(strcmp(name, "abcd\xc3\xa4") == 0); /* the 2nd 'ä' does not fit */
	CHECK_EQ(name[6], 0);
	lai_copy_name(name, sizeof(name), "a\nb\x80");
	CHECK(strcmp(name, "a b?") == 0);
	lai_copy_name(name, sizeof(name), NULL);
	CHECK_EQ(name[0], 0);

	CHECK(lai_event_name_ok("startup"));
	CHECK(lai_event_name_ok("notify-2.ok"));
	CHECK(!lai_event_name_ok("a..b"));

	/* Bad open parameters never reach the service. */
	memset(&o, 0, sizeof(o));
	f.format = AUD_FMT_S16_LE;
	f.rate = 100;
	f.channels = 2;
	CHECK(la_open(&f, NULL, &err) == NULL);
	CHECK_EQ(err, -EINVAL);
	f.rate = 48000;
	f.channels = 0;
	CHECK(la_open(&f, NULL, &err) == NULL);
	CHECK_EQ(err, -EINVAL);
	f.channels = 2;
	f.format = 0;
	CHECK(la_open(&f, NULL, &err) == NULL);
	CHECK_EQ(err, -EINVAL);
	f.format = AUD_FMT_S16_LE;
	o.flags = 1u << 30;
	CHECK(la_open(&f, &o, &err) == NULL);
	CHECK_EQ(err, -EINVAL);
	o.flags = 0;
	o.volume = 101;
	CHECK(la_open(&f, &o, &err) == NULL);
	CHECK_EQ(err, -EINVAL);
	o.volume = 0;
	o.role = 8;
	CHECK(la_open(&f, &o, &err) == NULL);
	CHECK_EQ(err, -EINVAL);
	CHECK(la_open(NULL, NULL, &err) == NULL);
	CHECK_EQ(err, -EINVAL);

	/* NULL handles. */
	CHECK_EQ(la_write(NULL, "x", 1), -EBADF);
	CHECK_EQ(la_writable(NULL), -EBADF);
	CHECK_EQ(la_drain(NULL), -EBADF);
	CHECK_EQ(la_flush(NULL), -EBADF);
	CHECK_EQ(la_pause(NULL), -EBADF);
	CHECK_EQ(la_resume(NULL), -EBADF);
	CHECK_EQ(la_set_volume(NULL, 1, 1), -EBADF);
	CHECK_EQ(la_set_mute(NULL, 1), -EBADF);
	CHECK_EQ(la_latency(NULL, NULL, NULL), -EBADF);
	CHECK_EQ(la_fd(NULL), -EBADF);
	CHECK_EQ(la_process(NULL), -EBADF);
	CHECK_EQ(la_stream_id(NULL), 0);
	la_close(NULL);
	CHECK_EQ(la_ctl_fd(NULL), -EBADF);
	CHECK_EQ(la_get_volume(NULL, &v, &v), -EBADF);
	CHECK_EQ(la_set_volume_master(NULL, 1, 1), -EBADF);
	CHECK_EQ(la_list_streams(NULL, NULL, 0), -EBADF);
	CHECK_EQ(la_subscribe(NULL, 0), -EBADF);
	CHECK_EQ(la_next_notify(NULL, NULL), -EBADF);
	CHECK_EQ(la_play_event(NULL, "x", 1, 0), -EBADF);
	la_ctl_close(NULL);
}

/* No service on this machine: the connect fails cleanly and quickly. */
static void test_no_service(void)
{
	la_format f = { AUD_FMT_S16_LE, 48000, 2 };
	uint64_t t0;
	int err = 0;

	printf("no service running\n");
	if (access(AUDIOD_SOCKET_PATH, F_OK) == 0) {
		printf("  (skipped: %s exists here)\n", AUDIOD_SOCKET_PATH);
		return;
	}
	t0 = lai_now_ms();
	CHECK(la_open(&f, NULL, &err) == NULL);
	CHECK(err == -ENOENT || err == -ECONNREFUSED);
	CHECK(la_ctl_open("x", &err) == NULL);
	CHECK(err == -ENOENT || err == -ECONNREFUSED);
	CHECK_EQ(la_play_event_simple("startup", 0), err);
	CHECK(lai_now_ms() - t0 < 5000);
}

int main(void)
{
	/* SIGPIPE keeps its default action: if the library ever let one
	 * through, this process would die and the run would fail. */
	signal(SIGPIPE, SIG_DFL);
	setvbuf(stdout, NULL, _IONBF, 0);

	test_helpers();
	test_open_and_blocking_write();
	test_partial_frames_and_controls();
	test_nonblocking_credit();
	test_nonblocking_full_socket();
	test_timeout_late_reply();
	test_protocol_violations();
	test_failures();
	test_large_write();
	test_control();
	test_subscribe();
	test_events();
	test_no_service();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
