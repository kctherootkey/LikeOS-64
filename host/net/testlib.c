/* Helpers on top of the instances' socket calls (host side of the network
 * harness; see sim.h).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "sim.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void pat_fill(unsigned id, uint64_t off, uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++)
		buf[i] = pat_byte(id, off + i);
}

size_t pat_check(unsigned id, uint64_t off, const uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (buf[i] != pat_byte(id, off + i))
			return i + 1;
	return 0;
}

/* ---- capture ---- */
struct capf {
	simtime_t t;
	unsigned len;
	uint8_t f[1600];
};
static struct capf *capq;
static int cap_n, cap_inst, cap_consume;

static int cap_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	(void)ctx;
	if (inst != cap_inst)
		return 0;
	if (cap_n < 512 && len <= sizeof(capq[0].f)) {
		capq[cap_n].t = now_ns();
		capq[cap_n].len = len;
		memcpy(capq[cap_n].f, f, len);
		cap_n++;
	}
	return cap_consume;
}

void cap_start(int inst, int consume)
{
	if (!capq)
		capq = calloc(512, sizeof(*capq));
	cap_n = 0;
	cap_inst = inst;
	cap_consume = consume;
	wire_set_tap(cap_tap, NULL);
}

void cap_stop(void)
{
	wire_set_tap(NULL, NULL);
}

int cap_pending(void)
{
	return cap_n;
}

static int cap_have(void *p)
{
	(void)p;
	return cap_n > 0;
}

unsigned cap_next(uint8_t *buf, unsigned cap, simtime_t wait,
		  simtime_t *when)
{
	if (!cap_n)
		sim_run_cond(cap_have, NULL, wait);
	if (!cap_n)
		return 0;
	unsigned len = capq[0].len < cap ? capq[0].len : cap;
	memcpy(buf, capq[0].f, len);
	if (when)
		*when = capq[0].t;
	memmove(capq, capq + 1, (size_t)(cap_n - 1) * sizeof(*capq));
	cap_n--;
	return len;
}

unsigned cap_next_proto(int proto, uint8_t *buf, unsigned cap,
			simtime_t wait)
{
	simtime_t end = now_ns() + wait;

	for (;;) {
		simtime_t left = now_ns() < end ? end - now_ns() : 0;
		unsigned n = cap_next(buf, cap, left, NULL);
		if (!n)
			return 0;
		int p = n >= 14 && buf[12] == 8 && buf[13] == 6 ? 0x806 :
								  -1;
		if (p < 0 && n >= 34 && buf[12] == 8 && buf[13] == 0)
			p = buf[23];
		if (p == proto)
			return n;
	}
}

int so_error(int inst, int s)
{
	int v = 0;
	unsigned len = sizeof(v);

	if (NS[inst]->getsockopt(s, K_SOL_SOCKET, K_SO_ERROR, &v, &len) < 0)
		return -1;
	return v;
}

int tcp_connect_nb(int inst, unsigned ip, unsigned port)
{
	int s = NS[inst]->socket(K_SOCK_STREAM);

	if (s < 0)
		return s;
	NS[inst]->set_nonblock(s, 1);
	int r = NS[inst]->connect(s, ip, port);
	if (r < 0 && r != -EINPROGRESS) {
		NS[inst]->close(s);
		return r;
	}
	return s;
}

struct est_arg {
	int inst, s;
};

static int est_cond(void *p)
{
	struct est_arg *a = p;
	int st = NS[a->inst]->tcp_state(a->s);

	return st != TS_SYN_SENT && st != TS_SYN_RECEIVED;
}

int wait_established(int inst, int s, simtime_t timeout)
{
	struct est_arg a = { inst, s };

	sim_run_cond(est_cond, &a, timeout);
	return NS[inst]->tcp_state(s) == TS_ESTABLISHED ? 0 : -1;
}

struct acc_arg {
	int inst, ls, got;
};

static int acc_cond(void *p)
{
	struct acc_arg *a = p;

	if (a->got < 0)
		a->got = NS[a->inst]->accept(a->ls, NULL, NULL);
	return a->got >= 0;
}

int tcp_pair(int cli_inst, int srv_inst, unsigned port, int *cli, int *srv,
	     int *lsock, simtime_t timeout)
{
	int ls = NS[srv_inst]->socket(K_SOCK_STREAM);

	if (ls < 0)
		return -1;
	NS[srv_inst]->set_nonblock(ls, 1);
	if (NS[srv_inst]->bind(ls, 0, port) < 0 ||
	    NS[srv_inst]->listen(ls, 128) < 0)
		return -1;
	int c = tcp_connect_nb(cli_inst, inst_ip(srv_inst), port);
	if (c < 0)
		return -1;
	struct acc_arg a = { srv_inst, ls, -1 };
	if (!sim_run_cond(acc_cond, &a, timeout))
		return -1;
	if (wait_established(cli_inst, c, timeout) < 0)
		return -1;
	NS[srv_inst]->set_nonblock(a.got, 1);
	*cli = c;
	*srv = a.got;
	if (lsock)
		*lsock = ls;
	else
		NS[srv_inst]->close(ls);
	return 0;
}

/* ---- flows: one-way pattern streams, both ends nonblocking ---- */
#define FMAP 1024
static struct flow *fl_tab;
static int fl_n;
static short fl_map[NINST][FMAP][2]; /* socket -> up to 2 flow indices + 1 */
static uint8_t *fl_buf;
unsigned long long g_missed_wakeups;

/* move what the windows allow; 1 if the flow made progress */
static int flow_pump(struct flow *x)
{
	int prog = 0;

	if (x->done)
		return 0;
	while (x->sent < x->n) {
		unsigned long long chunk = x->n - x->sent;
		if (chunk > 65536)
			chunk = 65536;
		pat_fill(x->id, x->sent, fl_buf, chunk);
		int r = NS[x->a]->send(x->sa, fl_buf, chunk, 0);
		if (r <= 0) {
			if (r < 0 && r != -EAGAIN)
				x->err = r;
			break;
		}
		x->sent += (unsigned)r;
		prog = 1;
	}
	if (x->sent >= x->n && x->close_after && !x->closed_a) {
		x->closed_a = 1;
		if (x->close_after == 1)
			NS[x->a]->shutdown(x->sa, K_SHUT_WR);
		else
			NS[x->a]->close(x->sa);
		prog = 1;
	}
	for (;;) {
		int r = NS[x->b]->recv(x->sb, fl_buf, 65536, 0);
		if (r < 0) {
			if (r != -EAGAIN)
				x->err = r;
			break;
		}
		if (r == 0) {
			x->eof = 1;
			break;
		}
		if (pat_check(x->id, x->got, fl_buf, (size_t)r))
			x->bad = 1;
		x->got += (unsigned)r;
		x->t_last = now_ns();
		prog = 1;
	}
	if (x->got >= x->n && (!x->close_after || x->eof || x->close_after == 2))
		x->done = 1;
	if (x->bad || x->err || (x->eof && x->got < x->n))
		x->done = 1;
	if (x->done)
		x->t_done = now_ns();
	return prog;
}

static void flow_notify(int inst, int sock, void *ctx)
{
	(void)ctx;
	if (sock < 0 || sock >= FMAP)
		return;
	for (int k = 0; k < 2; k++) {
		int i = fl_map[inst][sock][k] - 1;
		if (i >= 0)
			flow_pump(&fl_tab[i]);
	}
}

static int flows_done(void *p)
{
	(void)p;
	for (int i = 0; i < fl_n; i++)
		if (!fl_tab[i].done)
			return 0;
	return 1;
}

/* the safety scan: progress found here is a readiness change nobody was
 * told about */
static void flows_scan(void *arg, long a)
{
	(void)arg;
	(void)a;
	if (!fl_tab || flows_done(NULL))
		return;
	for (int i = 0; i < fl_n; i++)
		if (flow_pump(&fl_tab[i]))
			g_missed_wakeups++;
	sim_after(10 * MS, flows_scan, NULL, 0);
}

int flows_run(struct flow *f, int n, simtime_t timeout)
{
	int done = 0;

	fl_tab = f;
	fl_n = n;
	memset(fl_map, 0, sizeof(fl_map));
	fl_buf = malloc(65536);
	for (int i = 0; i < n; i++) {
		int ends[2][2] = { { f[i].a, f[i].sa }, { f[i].b, f[i].sb } };
		f[i].t_start = now_ns();
		for (int e = 0; e < 2; e++) {
			short *m = fl_map[ends[e][0]][ends[e][1] % FMAP];
			if (!m[0])
				m[0] = (short)(i + 1);
			else if (m[0] != i + 1)
				m[1] = (short)(i + 1);
		}
	}
	sim_set_app(flow_notify, NULL);
	for (int i = 0; i < n; i++)
		flow_pump(&f[i]);
	sim_after(10 * MS, flows_scan, NULL, 0);
	sim_run_cond(flows_done, NULL, timeout);
	sim_set_app(NULL, NULL);
	for (int i = 0; i < n; i++)
		if (f[i].done && f[i].got >= f[i].n && !f[i].bad)
			done++;
	fl_tab = NULL;
	free(fl_buf);
	return done;
}

void flow_init(struct flow *f, int a, int sa, int b, int sb, unsigned id,
	       unsigned long long n)
{
	memset(f, 0, sizeof(*f));
	f->a = a;
	f->sa = sa;
	f->b = b;
	f->sb = sb;
	f->id = id;
	f->n = n;
}

long long transfer(int a, int sa, int b, int sb, unsigned id,
		   unsigned long long n, simtime_t timeout)
{
	struct flow f;

	flow_init(&f, a, sa, b, sb, id, n);
	flows_run(&f, 1, timeout);
	if (f.bad)
		return -1;
	if (f.err)
		tlog("transfer: error %d after %llu bytes", f.err, f.got);
	return (long long)f.got;
}

/* ---- checksums ---- */
uint16_t inet_csum(const void *data, size_t len, uint32_t seed)
{
	const uint8_t *p = data;
	uint32_t s = seed;

	while (len > 1) {
		s += (uint32_t)(p[0] << 8 | p[1]);
		p += 2;
		len -= 2;
	}
	if (len)
		s += (uint32_t)p[0] << 8;
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (uint16_t)~s;
}

int frame_csum_bad(const uint8_t *f, unsigned len)
{
	int bad = 0;

	if (len < 34 || f[12] != 0x08 || f[13] != 0x00)
		return 0;
	const uint8_t *ip = f + 14;
	unsigned ihl = (ip[0] & 15) * 4u, tot = (unsigned)(ip[2] << 8 | ip[3]);
	if (ihl < 20 || tot < ihl || 14 + tot > len)
		return 1;
	if (inet_csum(ip, ihl, 0) != 0)
		bad |= 1;
	unsigned frag = (unsigned)((ip[6] & 0x3f) << 8 | ip[7]);
	if (frag) /* a fragment: the L4 checksum covers the whole datagram */
		return bad;
	const uint8_t *l4 = ip + ihl;
	unsigned l4len = tot - ihl;
	uint32_t ph = 0;
	ph += (uint32_t)(ip[12] << 8 | ip[13]) + (uint32_t)(ip[14] << 8 | ip[15]);
	ph += (uint32_t)(ip[16] << 8 | ip[17]) + (uint32_t)(ip[18] << 8 | ip[19]);
	ph += ip[9] + l4len;
	if (ip[9] == 6) {
		if (inet_csum(l4, l4len, ph) != 0)
			bad |= 2;
	} else if (ip[9] == 17) {
		if (l4len >= 8 && (l4[6] | l4[7]) && inet_csum(l4, l4len, ph))
			bad |= 2;
	} else if (ip[9] == 1) {
		if (inet_csum(l4, l4len, 0) != 0)
			bad |= 2;
	}
	return bad;
}

void frame_fix_csums(uint8_t *f, unsigned len)
{
	if (len < 34 || f[12] != 8 || f[13] != 0)
		return;
	uint8_t *ip = f + 14;
	unsigned ihl = (ip[0] & 15) * 4u, tot = (unsigned)(ip[2] << 8 | ip[3]);
	if (ihl < 20 || 14 + ihl > len)
		return;
	ip[10] = ip[11] = 0;
	uint16_t c = inet_csum(ip, ihl, 0);
	ip[10] = (uint8_t)(c >> 8);
	ip[11] = (uint8_t)c;
	if (tot < ihl || 14 + tot > len || (ip[6] & 0x3f) || ip[7])
		return;
	uint8_t *l4 = ip + ihl;
	unsigned l = tot - ihl;
	uint32_t ph = (uint32_t)(ip[12] << 8 | ip[13]) +
		      (uint32_t)(ip[14] << 8 | ip[15]) +
		      (uint32_t)(ip[16] << 8 | ip[17]) +
		      (uint32_t)(ip[18] << 8 | ip[19]) + ip[9] + l;
	if (ip[9] == 6 && l >= 20) {
		l4[16] = l4[17] = 0;
		c = inet_csum(l4, l, ph);
		l4[16] = (uint8_t)(c >> 8);
		l4[17] = (uint8_t)c;
	} else if (ip[9] == 17 && l >= 8) {
		if (!l4[6] && !l4[7])
			return; /* no checksum */
		l4[6] = l4[7] = 0;
		c = inet_csum(l4, l, ph);
		if (!c)
			c = 0xffff;
		l4[6] = (uint8_t)(c >> 8);
		l4[7] = (uint8_t)c;
	} else if (ip[9] == 1 && l >= 4) {
		l4[2] = l4[3] = 0;
		c = inet_csum(l4, l, 0);
		l4[2] = (uint8_t)(c >> 8);
		l4[3] = (uint8_t)c;
	}
}
