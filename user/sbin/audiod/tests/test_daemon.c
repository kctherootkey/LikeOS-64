// audiod host integration test: runs a host build of the daemon (null
// output, every path under a scratch directory) and talks the real wire
// protocol to it over its socket - streaming with flow control, drain,
// latency, the control requests, event sounds, rate limiting, protocol
// violations, a fuzzing barrage, SIGHUP and SIGTERM.
//
// usage: test_daemon <audiod binary> <scratch dir>
// The scratch directory must equal the AUDIOD_HOST_TEST path the binary
// was built with.  Nothing outside it is touched.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <audio/protocol.h>

static int failures, checks;
static char sock_path[100]; /* fits sun_path */

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			failures++;                                            \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int connect_sock(void)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, sock_path, sizeof(sock_path));
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	struct timeval tv = { 5, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

static int send_all(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		b += w;
		n -= (size_t)w;
	}
	return 0;
}

static int send_msg(int fd, uint32_t type, const void *p, uint32_t len)
{
	struct aud_header h = { type, len };
	if (send_all(fd, &h, sizeof(h)) < 0)
		return -1;
	if (len && send_all(fd, p, len) < 0)
		return -1;
	return 0;
}

static int recv_all(int fd, void *p, size_t n)
{
	uint8_t *b = p;
	while (n) {
		ssize_t r = recv(fd, b, n, 0);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		b += r;
		n -= (size_t)r;
	}
	return 0;
}

/* Receive one message; payload into buf (cap bytes).  Returns the type, or
 * -1 on EOF/error. */
static int recv_msg(int fd, void *buf, size_t cap, uint32_t *len)
{
	struct aud_header h;
	if (recv_all(fd, &h, sizeof(h)) < 0)
		return -1;
	if (h.len > cap)
		return -1;
	if (h.len && recv_all(fd, buf, h.len) < 0)
		return -1;
	if (len)
		*len = h.len;
	return (int)h.type;
}

/* Receive the next reply, absorbing REQUEST/UNDERRUN/NOTIFY into
 * counters. */
static uint64_t g_credit;
static int g_underruns, g_notifies;
static struct aud_notify g_last_notify;

static int recv_reply(int fd, void *buf, size_t cap, uint32_t *len)
{
	for (;;) {
		uint8_t tmp[70000];
		uint32_t l = 0;
		int t = recv_msg(fd, tmp, sizeof(tmp), &l);
		if (t < 0)
			return -1;
		if (t == AUD_MSG_REQUEST && l == sizeof(struct aud_request)) {
			struct aud_request rq;
			memcpy(&rq, tmp, sizeof(rq));
			g_credit += rq.bytes;
			continue;
		}
		if (t == AUD_MSG_UNDERRUN) {
			g_underruns++;
			continue;
		}
		if (t == AUD_MSG_NOTIFY && l == sizeof(struct aud_notify)) {
			memcpy(&g_last_notify, tmp, sizeof(g_last_notify));
			g_notifies++;
			continue;
		}
		if (l > cap)
			return -1;
		memcpy(buf, tmp, l);
		if (len)
			*len = l;
		return t;
	}
}

static int hello(int fd, uint32_t kind, struct aud_hello_reply *rep)
{
	struct aud_hello h;
	memset(&h, 0, sizeof(h));
	h.version = AUDIOD_PROTOCOL_VERSION;
	h.kind = kind;
	snprintf(h.name, sizeof(h.name), "test_daemon");
	if (send_msg(fd, AUD_MSG_HELLO, &h, sizeof(h)) < 0)
		return -1;
	struct aud_hello_reply r;
	uint32_t l;
	int t = recv_reply(fd, &r, sizeof(r), &l);
	if (t != AUD_MSG_HELLO || l != sizeof(r))
		return -1;
	if (rep)
		*rep = r;
	return 0;
}

/* Is the connection closed by the server (EOF within a second)? */
static int is_closed(int fd)
{
	struct pollfd p = { fd, POLLIN, 0 };
	double end = now_s() + 2.0;
	while (now_s() < end) {
		if (poll(&p, 1, 100) > 0) {
			uint8_t b[4096];
			ssize_t r = recv(fd, b, sizeof(b), MSG_DONTWAIT);
			if (r == 0 || (r < 0 && errno != EAGAIN &&
				       errno != EWOULDBLOCK))
				return 1;
		}
	}
	return 0;
}

static void write_wav(const char *path, uint32_t rate, uint32_t frames)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	uint32_t data = frames * 4, riff = 36 + data, fmt_len = 16,
		 byte_rate = rate * 4;
	uint16_t tag = 1, ch = 2, align = 4, bits = 16;
	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt_len, 4, 1, f);
	fwrite(&tag, 2, 1, f);
	fwrite(&ch, 2, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&byte_rate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data, 4, 1, f);
	for (uint32_t i = 0; i < frames; i++) {
		int16_t v = (int16_t)(8000 * sin(2 * M_PI * 880.0 * i / rate));
		fwrite(&v, 2, 1, f);
		fwrite(&v, 2, 1, f);
	}
	fclose(f);
}

/* ---- scenarios -------------------------------------------------------------- */

static void scenario_stream(void)
{
	int fd = connect_sock();
	CHECK(fd >= 0, "connect");
	if (fd < 0)
		return;
	struct aud_hello_reply hr;
	CHECK(hello(fd, AUD_CLIENT_STREAM, &hr) == 0, "stream hello");
	CHECK(hr.version == AUDIOD_PROTOCOL_VERSION && hr.rate >= 8000,
	      "hello reply: version %u rate %u", hr.version, hr.rate);

	struct aud_stream_open o;
	memset(&o, 0, sizeof(o));
	o.format = AUD_FMT_S16_LE;
	o.rate = 44100;
	o.channels = 2;
	o.role = AUD_ROLE_MUSIC;
	o.buffer_ms = 50;
	o.volume = 100;
	snprintf(o.name, sizeof(o.name), "integration sine");
	send_msg(fd, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	struct aud_stream_opened op;
	uint32_t l;
	int t = recv_reply(fd, &op, sizeof(op), &l);
	CHECK(t == AUD_MSG_STREAM_OPEN && l == sizeof(op) && op.stream_id &&
		      op.request_bytes > 0 && op.request_bytes % 4 == 0,
	      "stream opened: type %d id %u request %u", t, op.stream_id,
	      op.request_bytes);
	g_credit = op.request_bytes;

	/* 1.5 s of audio with flow control; the null sink plays in real
	 * time, so this takes about 1.5 s minus the buffering */
	const uint32_t total = 44100 * 3 / 2 * 4;
	uint32_t sent = 0;
	static int16_t pcm[16384];
	for (int i = 0; i < 16384; i += 2)
		pcm[i] = pcm[i + 1] = (int16_t)(5000 * sin(i * 0.03));
	double t0 = now_s();
	int latency_checked = 0;
	while (sent < total) {
		/* absorb any pending credits */
		struct pollfd p = { fd, POLLIN, 0 };
		while (poll(&p, 1, g_credit ? 0 : 2000) > 0) {
			uint8_t tmp[64];
			uint32_t ll;
			int tt = recv_msg(fd, tmp, sizeof(tmp), &ll);
			if (tt == AUD_MSG_REQUEST) {
				struct aud_request rq;
				memcpy(&rq, tmp, sizeof(rq));
				g_credit += rq.bytes;
			} else if (tt == AUD_MSG_UNDERRUN) {
				g_underruns++;
			} else {
				CHECK(0, "unexpected message %d while streaming",
				      tt);
				close(fd);
				return;
			}
			if (g_credit)
				break;
		}
		if (!g_credit) {
			CHECK(0, "no credit for 2 s (flow control stuck)");
			close(fd);
			return;
		}
		uint32_t n = (uint32_t)g_credit;
		if (n > sizeof(pcm))
			n = sizeof(pcm);
		if (n > total - sent)
			n = total - sent;
		n -= n % 4;
		send_msg(fd, AUD_MSG_DATA, pcm, n);
		g_credit -= n;
		sent += n;

		if (!latency_checked && sent > total / 2) {
			/* a request answered while DATA flows */
			send_msg(fd, AUD_MSG_GET_LATENCY, NULL, 0);
			struct aud_latency lat;
			t = recv_reply(fd, &lat, sizeof(lat), &l);
			CHECK(t == AUD_MSG_GET_LATENCY && l == sizeof(lat),
			      "latency reply %d", t);
			CHECK(lat.latency_us > 1000 && lat.latency_us < 300000,
			      "latency %llu us",
			      (unsigned long long)lat.latency_us);
			latency_checked = 1;
		}
	}
	send_msg(fd, AUD_MSG_DRAIN, NULL, 0);
	t = recv_reply(fd, NULL, 0, &l);
	double el = now_s() - t0;
	CHECK(t == AUD_MSG_OK, "drain reply %d", t);
	CHECK(el > 1.2 && el < 2.5, "1.5 s of audio took %.2f s", el);
	CHECK(g_underruns == 0, "%d underruns while streaming", g_underruns);

	send_msg(fd, AUD_MSG_GET_LATENCY, NULL, 0);
	struct aud_latency lat;
	t = recv_reply(fd, &lat, sizeof(lat), &l);
	CHECK(t == AUD_MSG_GET_LATENCY, "latency after drain");
	CHECK(lat.played_frames > 44100 * 3 / 2 - 2000 &&
		      lat.played_frames <= 44100 * 3 / 2,
	      "played %llu frames", (unsigned long long)lat.played_frames);

	/* pause/resume/flush/volume/mute replies */
	uint32_t types[] = { AUD_MSG_PAUSE, AUD_MSG_RESUME, AUD_MSG_FLUSH };
	for (int i = 0; i < 3; i++) {
		send_msg(fd, types[i], NULL, 0);
		t = recv_reply(fd, NULL, 0, &l);
		CHECK(t == AUD_MSG_OK, "reply to %u: %d", types[i], t);
	}
	struct aud_volume v = { 50, 60, { 0, 0 } };
	send_msg(fd, AUD_MSG_SET_STREAM_VOLUME, &v, sizeof(v));
	CHECK(recv_reply(fd, NULL, 0, &l) == AUD_MSG_OK, "stream volume");
	v.left = 200;
	send_msg(fd, AUD_MSG_SET_STREAM_VOLUME, &v, sizeof(v));
	struct aud_error er;
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR &&
		      er.code == -EINVAL,
	      "stream volume 200 accepted");
	struct aud_mute mu = { 1, 0 };
	send_msg(fd, AUD_MSG_SET_STREAM_MUTE, &mu, sizeof(mu));
	CHECK(recv_reply(fd, NULL, 0, &l) == AUD_MSG_OK, "stream mute");

	/* paused drain is refused, not hung */
	send_msg(fd, AUD_MSG_PAUSE, NULL, 0);
	recv_reply(fd, NULL, 0, &l);
	send_msg(fd, AUD_MSG_DRAIN, NULL, 0);
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR,
	      "drain while paused");

	/* close: no reply, connection stays usable */
	send_msg(fd, AUD_MSG_STREAM_CLOSE, NULL, 0);
	send_msg(fd, AUD_MSG_GET_SERVER_INFO, NULL, 0);
	struct aud_server_info si;
	t = recv_reply(fd, &si, sizeof(si), &l);
	CHECK(t == AUD_MSG_GET_SERVER_INFO && l == sizeof(si),
	      "server info after close: %d", t);
	close(fd);
}

static void scenario_underrun(void)
{
	int fd = connect_sock();
	if (fd < 0 || hello(fd, AUD_CLIENT_STREAM, NULL) < 0) {
		CHECK(0, "underrun: connect");
		return;
	}
	struct aud_stream_open o;
	memset(&o, 0, sizeof(o));
	o.format = AUD_FMT_F32_LE;
	o.rate = 48000;
	o.channels = 1;
	o.volume = 100;
	send_msg(fd, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	struct aud_stream_opened op;
	uint32_t l;
	g_underruns = 0;
	CHECK(recv_reply(fd, &op, sizeof(op), &l) == AUD_MSG_STREAM_OPEN,
	      "open f32");
	/* 100 ms, then silence from the client: one UNDERRUN */
	static float buf[4800];
	uint32_t n = sizeof(buf) < op.request_bytes ? sizeof(buf)
						    : op.request_bytes;
	send_msg(fd, AUD_MSG_DATA, buf, n - n % 4);
	struct pollfd p = { fd, POLLIN, 0 };
	double end = now_s() + 1.5;
	while (now_s() < end && !g_underruns) {
		if (poll(&p, 1, 100) > 0) {
			uint8_t tmp[64];
			int t = recv_msg(fd, tmp, sizeof(tmp), &l);
			if (t == AUD_MSG_UNDERRUN) {
				CHECK(l == 0, "UNDERRUN carries %u bytes", l);
				g_underruns++;
			}
		}
	}
	CHECK(g_underruns == 1, "underrun reported %d times", g_underruns);
	close(fd);
}

static void scenario_violations(void)
{
	struct aud_hello_reply hr;
	int fd;

	/* request before HELLO */
	fd = connect_sock();
	send_msg(fd, AUD_MSG_GET_SERVER_INFO, NULL, 0);
	CHECK(is_closed(fd), "request before HELLO not closed");
	close(fd);

	/* wrong version: an ERROR, then closed */
	fd = connect_sock();
	struct aud_hello h;
	memset(&h, 0, sizeof(h));
	h.version = 77;
	h.kind = AUD_CLIENT_CONTROL;
	send_msg(fd, AUD_MSG_HELLO, &h, sizeof(h));
	struct aud_error er;
	uint32_t l;
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR,
	      "version mismatch error");
	close(fd);

	/* oversized header */
	fd = connect_sock();
	hello(fd, AUD_CLIENT_STREAM, &hr);
	struct aud_header big = { AUD_MSG_DATA, AUDIOD_MAX_PAYLOAD + 1 };
	send_all(fd, &big, sizeof(big));
	CHECK(is_closed(fd), "oversized message not closed");
	close(fd);

	/* unknown type */
	fd = connect_sock();
	hello(fd, AUD_CLIENT_CONTROL, &hr);
	send_msg(fd, 4242, NULL, 0);
	CHECK(is_closed(fd), "unknown type not closed");
	close(fd);

	/* stream request on a control connection */
	fd = connect_sock();
	hello(fd, AUD_CLIENT_CONTROL, &hr);
	send_msg(fd, AUD_MSG_DRAIN, NULL, 0);
	CHECK(is_closed(fd), "kind mismatch not closed");
	close(fd);

	/* DATA without a stream, partial frames, beyond credit */
	fd = connect_sock();
	hello(fd, AUD_CLIENT_STREAM, &hr);
	uint8_t junk[64] = { 0 };
	send_msg(fd, AUD_MSG_DATA, junk, 64);
	CHECK(is_closed(fd), "DATA without stream not closed");
	close(fd);

	struct aud_stream_open o;
	memset(&o, 0, sizeof(o));
	o.format = AUD_FMT_S16_LE;
	o.rate = 48000;
	o.channels = 2;
	o.volume = 100;
	fd = connect_sock();
	hello(fd, AUD_CLIENT_STREAM, &hr);
	send_msg(fd, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	struct aud_stream_opened op;
	recv_reply(fd, &op, sizeof(op), &l);
	send_msg(fd, AUD_MSG_DATA, junk, 3);
	CHECK(is_closed(fd), "partial frame not closed");
	close(fd);

	fd = connect_sock();
	hello(fd, AUD_CLIENT_STREAM, &hr);
	send_msg(fd, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	recv_reply(fd, &op, sizeof(op), &l);
	static uint8_t lots[AUDIOD_MAX_PAYLOAD];
	uint32_t sent = 0;
	int closed = 0;
	while (sent <= op.request_bytes + AUDIOD_MAX_PAYLOAD) {
		if (send_msg(fd, AUD_MSG_DATA, lots, sizeof(lots)) < 0) {
			closed = 1;
			break;
		}
		sent += sizeof(lots);
	}
	CHECK(closed || is_closed(fd), "credit overrun not closed");
	close(fd);

	/* bad parameters: an ERROR, the connection stays */
	fd = connect_sock();
	hello(fd, AUD_CLIENT_STREAM, &hr);
	o.rate = 1000;
	send_msg(fd, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR &&
		      er.code == -EINVAL,
	      "bad rate");
	o.rate = 48000;
	send_msg(fd, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	CHECK(recv_reply(fd, &op, sizeof(op), &l) == AUD_MSG_STREAM_OPEN,
	      "open after a refused open");
	close(fd);
}

static void scenario_control(void)
{
	int fd = connect_sock(), fd2 = connect_sock();
	uint32_t l;
	if (fd < 0 || fd2 < 0) {
		CHECK(0, "control connect");
		return;
	}
	CHECK(hello(fd, AUD_CLIENT_CONTROL, NULL) == 0, "control hello");
	CHECK(hello(fd2, AUD_CLIENT_CONTROL, NULL) == 0, "control hello 2");

	struct aud_subscribe sb = { AUD_SUB_VOLUME | AUD_SUB_STREAMS, 0 };
	send_msg(fd2, AUD_MSG_SUBSCRIBE, &sb, sizeof(sb));
	CHECK(recv_reply(fd2, NULL, 0, &l) == AUD_MSG_OK, "subscribe");

	struct aud_volume v = { 30, 40, { 0, 0 } };
	send_msg(fd, AUD_MSG_SET_MASTER_VOLUME, &v, sizeof(v));
	CHECK(recv_reply(fd, NULL, 0, &l) == AUD_MSG_OK, "set master volume");
	send_msg(fd, AUD_MSG_GET_MASTER_VOLUME, NULL, 0);
	memset(&v, 0, sizeof(v));
	CHECK(recv_reply(fd, &v, sizeof(v), &l) == AUD_MSG_GET_MASTER_VOLUME &&
		      v.left == 30 && v.right == 40,
	      "get master volume %u/%u", v.left, v.right);
	v.left = 101;
	send_msg(fd, AUD_MSG_SET_MASTER_VOLUME, &v, sizeof(v));
	struct aud_error er;
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR,
	      "volume 101 accepted");

	/* the subscriber heard about it */
	g_notifies = 0;
	send_msg(fd2, AUD_MSG_GET_MUTE, NULL, 0);
	struct aud_mute mu;
	CHECK(recv_reply(fd2, &mu, sizeof(mu), &l) == AUD_MSG_GET_MUTE,
	      "get mute");
	CHECK(g_notifies >= 1 && g_last_notify.what == AUD_NOTIFY_VOLUME &&
		      g_last_notify.a == 30 && g_last_notify.b == 40,
	      "volume notification (%d, what %u a %u b %u)", g_notifies,
	      g_last_notify.what, g_last_notify.a, g_last_notify.b);

	mu.mute = 1;
	send_msg(fd, AUD_MSG_SET_MUTE, &mu, sizeof(mu));
	CHECK(recv_reply(fd, NULL, 0, &l) == AUD_MSG_OK, "mute");
	mu.mute = 0;
	send_msg(fd, AUD_MSG_SET_MUTE, &mu, sizeof(mu));
	CHECK(recv_reply(fd, NULL, 0, &l) == AUD_MSG_OK, "unmute");

	struct aud_outputs outs;
	send_msg(fd, AUD_MSG_GET_OUTPUTS, NULL, 0);
	CHECK(recv_reply(fd, &outs, sizeof(outs), &l) == AUD_MSG_GET_OUTPUTS &&
		      l == sizeof(outs),
	      "outputs");
	struct aud_cards cs;
	send_msg(fd, AUD_MSG_GET_CARDS, NULL, 0);
	CHECK(recv_reply(fd, &cs, sizeof(cs), &l) == AUD_MSG_GET_CARDS &&
		      l == sizeof(cs),
	      "cards");
	struct aud_stream_ref ref = { 0, 0 };
	send_msg(fd, AUD_MSG_SET_CARD, &ref, sizeof(ref));
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR &&
		      (getuid() == 0 || er.code == -EPERM),
	      "SET_CARD by a user");

	/* a stream shows up in the list, and can be killed by its owner */
	int sfd = connect_sock();
	hello(sfd, AUD_CLIENT_STREAM, NULL);
	struct aud_stream_open o;
	memset(&o, 0, sizeof(o));
	o.format = AUD_FMT_S16_LE;
	o.rate = 22050;
	o.channels = 1;
	o.volume = 80;
	snprintf(o.name, sizeof(o.name), "listed");
	send_msg(sfd, AUD_MSG_STREAM_OPEN, &o, sizeof(o));
	struct aud_stream_opened op;
	recv_reply(sfd, &op, sizeof(op), &l);

	static uint8_t lbuf[sizeof(struct aud_stream_list) +
			    80 * sizeof(struct aud_stream_entry)];
	send_msg(fd, AUD_MSG_LIST_STREAMS, NULL, 0);
	int t = recv_reply(fd, lbuf, sizeof(lbuf), &l);
	struct aud_stream_list *sl = (struct aud_stream_list *)lbuf;
	CHECK(t == AUD_MSG_LIST_STREAMS &&
		      l == sizeof(*sl) + sl->count * sizeof(sl->entry[0]),
	      "list streams length");
	int found = 0;
	for (uint32_t i = 0; i < sl->count; i++)
		if (sl->entry[i].stream_id == op.stream_id &&
		    !strcmp(sl->entry[i].name, "listed") &&
		    sl->entry[i].rate == 22050)
			found = 1;
	CHECK(found, "stream not listed");

	struct aud_volume_of vo = { op.stream_id, 10, 10, 0 };
	send_msg(fd, AUD_MSG_SET_VOLUME_OF, &vo, sizeof(vo));
	CHECK(recv_reply(fd, NULL, 0, &l) == AUD_MSG_OK, "volume of own stream");
	vo.stream_id = 999999;
	send_msg(fd, AUD_MSG_SET_VOLUME_OF, &vo, sizeof(vo));
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR &&
		      er.code == -ENOENT,
	      "volume of a missing stream");
	ref.id = op.stream_id;
	send_msg(fd, AUD_MSG_KILL_STREAM, &ref, sizeof(ref));
	CHECK(recv_reply(fd, NULL, 0, &l) == AUD_MSG_OK, "kill");
	CHECK(is_closed(sfd), "killed stream's connection not closed");
	close(sfd);

	/* event sounds */
	struct aud_play_event pe;
	memset(&pe, 0, sizeof(pe));
	snprintf(pe.name, sizeof(pe.name), "notify");
	pe.volume = 50;
	pe.wait = 1;
	double t0 = now_s();
	send_msg(fd, AUD_MSG_PLAY_EVENT, &pe, sizeof(pe));
	t = recv_reply(fd, &er, sizeof(er), &l);
	double el = now_s() - t0;
	CHECK(t == AUD_MSG_OK, "event sound: reply %d (%s)", t,
	      t == AUD_MSG_ERROR ? er.message : "");
	CHECK(el > 0.25 && el < 1.5, "300 ms event sound waited %.2f s", el);

	memset(pe.name, 0, sizeof(pe.name));
	snprintf(pe.name, sizeof(pe.name), "../../etc");
	pe.wait = 0;
	send_msg(fd, AUD_MSG_PLAY_EVENT, &pe, sizeof(pe));
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR &&
		      er.code == -EINVAL,
	      "path in an event name");
	memset(pe.name, 0, sizeof(pe.name));
	snprintf(pe.name, sizeof(pe.name), "missing");
	send_msg(fd, AUD_MSG_PLAY_EVENT, &pe, sizeof(pe));
	CHECK(recv_reply(fd, &er, sizeof(er), &l) == AUD_MSG_ERROR &&
		      er.code == -ENOENT,
	      "missing event sound: %d", er.code);
	/* the rate limit (10 per 10 s, some already spent) */
	int busy = 0;
	for (int i = 0; i < 20; i++) {
		memset(pe.name, 0, sizeof(pe.name));
		snprintf(pe.name, sizeof(pe.name), "notify");
		send_msg(fd, AUD_MSG_PLAY_EVENT, &pe, sizeof(pe));
		t = recv_reply(fd, &er, sizeof(er), &l);
		if (t == AUD_MSG_ERROR && er.code == -EBUSY)
			busy++;
	}
	CHECK(busy >= 10, "event rate limit: only %d refused", busy);

	/* the unterminated name is a violation */
	memset(pe.name, 'a', sizeof(pe.name));
	send_msg(fd, AUD_MSG_PLAY_EVENT, &pe, sizeof(pe));
	CHECK(is_closed(fd), "unterminated event name not closed");
	close(fd);
	close(fd2);
}

static void scenario_fuzz(void)
{
	uint32_t seed = 4711;
	for (int c = 0; c < 60; c++) {
		int fd = connect_sock();
		if (fd < 0) {
			CHECK(0, "fuzz connect %d", c);
			continue;
		}
		if (c % 3)
			hello(fd, c % 2 ? AUD_CLIENT_STREAM : AUD_CLIENT_CONTROL,
			      NULL);
		uint8_t buf[2048];
		for (size_t i = 0; i < sizeof(buf); i++) {
			seed = seed * 1103515245u + 12345u;
			buf[i] = (uint8_t)(seed >> 16);
		}
		if (c % 4 == 0) {
			/* plausible headers with random payloads */
			for (size_t i = 0; i + 8 < sizeof(buf); i += 64) {
				uint32_t t = 1 + (buf[i] % 60);
				uint32_t l = buf[i + 1] % 56;
				memcpy(buf + i, &t, 4);
				memcpy(buf + i + 4, &l, 4);
			}
		}
		send(fd, buf, sizeof(buf), MSG_NOSIGNAL | MSG_DONTWAIT);
		close(fd);
	}
	/* still alive and answering */
	int fd = connect_sock();
	CHECK(fd >= 0 && hello(fd, AUD_CLIENT_CONTROL, NULL) == 0,
	      "daemon dead after fuzzing");
	if (fd >= 0) {
		struct aud_server_info si;
		uint32_t l;
		send_msg(fd, AUD_MSG_GET_SERVER_INFO, NULL, 0);
		CHECK(recv_reply(fd, &si, sizeof(si), &l) ==
			      AUD_MSG_GET_SERVER_INFO,
		      "server info after fuzzing");
		close(fd);
	}

	/* a silent connection is dropped after the HELLO timeout */
	fd = connect_sock();
	CHECK(fd >= 0, "connect for timeout");
	struct timeval tv = { 8, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	uint8_t b;
	double t0 = now_s();
	ssize_t r = recv(fd, &b, 1, 0);
	CHECK(r == 0 && now_s() - t0 > 4.0, "silent client not dropped (%zd)",
	      r);
	close(fd);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: test_daemon <audiod> <scratch dir>\n");
		return 2;
	}
	const char *bin = argv[1], *dir = argv[2];
	char path[600];

	snprintf(path, sizeof(path), "%s/run", dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/lib", dir);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/sounds", dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/sounds/notify.wav", dir);
	write_wav(path, 44100, 44100 * 3 / 10);
	if (strlen(dir) + sizeof("/run/socket") > sizeof(sock_path)) {
		fprintf(stderr, "scratch directory path too long for a socket\n");
		return 2;
	}
	snprintf(sock_path, sizeof(sock_path), "%s/run/socket", dir);
	snprintf(path, sizeof(path), "%s/audiod.conf", dir);
	FILE *f = fopen(path, "w");
	if (!f)
		return 2;
	fprintf(f, "# integration test\ndevice = /dev/audio7\nevent_rate = 10\n"
		   "suspend_ms = 500\nlog_level = info\n");
	fclose(f);
	chmod(path, 0644);

	pid_t pid = fork();
	if (pid == 0) {
		execl(bin, bin, "-f", "-c", path, (char *)NULL);
		_exit(127);
	}
	/* wait for the socket */
	int up = 0;
	for (int i = 0; i < 100 && !up; i++) {
		int fd = connect_sock();
		if (fd >= 0) {
			close(fd);
			up = 1;
		} else {
			struct timespec ts = { 0, 50000000 };
			nanosleep(&ts, NULL);
		}
	}
	CHECK(up, "daemon did not come up");
	if (up) {
		scenario_stream();
		scenario_underrun();
		scenario_violations();
		scenario_control();
		kill(pid, SIGHUP);
		scenario_fuzz();
	}

	kill(pid, SIGTERM);
	int status = 0;
	double t0 = now_s();
	while (waitpid(pid, &status, WNOHANG) == 0 && now_s() - t0 < 5.0) {
		struct timespec ts = { 0, 20000000 };
		nanosleep(&ts, NULL);
	}
	if (now_s() - t0 >= 5.0) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		CHECK(0, "daemon did not stop on SIGTERM");
	} else {
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "daemon exit status %d", status);
	}
	struct stat st;
	CHECK(stat(sock_path, &st) < 0, "socket left behind");
	snprintf(path, sizeof(path), "%s/lib/state", dir);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 077) == 0,
	      "state not saved privately");

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
