/* Benchmarks of the network stack, in simulated time (deterministic: the
 * same sources and seed give the same numbers).  Every result line is
 * "key value"; host/test-net.sh bench compares them with
 * host/net/baseline.txt.
 *
 *   amazon  a page load: one HTML object, then ~500 objects of 2 KB ..
 *           500 KB (bounded Pareto) from 33 hosts, at most 6 connections
 *           per host, HTTP-like request/response with keep-alive; every
 *           host has its own RTT (20-120 ms), loss (0.1-1 %) and rate
 *           (50-200 Mbit/s) behind a shared 200 Mbit/s access link.
 *   live    a 6 Mbit/s server->client stream for 120 s (60 ms RTT, 20
 *           Mbit/s, 0.1 % loss, a 300 ms total-loss burst every 10 s)
 *           played out with a 2 s buffer.
 *   bulk    one 100 MB flow at RTT 10/50/150 ms and loss 0/0.1/1 % over
 *           a 1 Gbit/s link (cut off after 300 s of simulated time).
 *
 * Instance 0 is the client (the machine a browser runs on), instance 1
 * plays every server.  Both are this stack.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

static void result(const char *key, double v)
{
	char line[256];

	if (v == floor(v) && fabs(v) < 1e15)
		snprintf(line, sizeof(line), "%s %.0f\n", key, v);
	else
		snprintf(line, sizeof(line), "%s %.3f\n", key, v);
	fputs("    ", stdout);
	fputs(line, stdout);
	if (g_opts.out) {
		FILE *f = fopen(g_opts.out, "a");
		if (f) {
			fputs(line, f);
			fclose(f);
		}
	}
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

static double pct(double *v, int n, double p)
{
	if (n <= 0)
		return 0;
	qsort(v, (size_t)n, sizeof(*v), cmp_d);
	int i = (int)(p * (n - 1) + 0.5);
	return v[i];
}

/* a warm machine: the neighbours are known (the gateway's ARP entry is
 * resolved long before a page load) */
static void arp_warm(void)
{
	unsigned char m[6];

	for (int i = 0; i < 2; i++) {
		inst_mac(i ^ 1, m);
		NS[i]->arp_add(inst_ip(i ^ 1), m);
	}
}

static unsigned long long retrans_total(void)
{
	return NS[0]->counter(NSC_TCP_RETRANS_SEGS) +
	       NS[1]->counter(NSC_TCP_RETRANS_SEGS);
}

/* the server port of a TCP frame (either direction); -1 if none */
static int server_port(const uint8_t *f, unsigned len, int lo, int hi)
{
	if (len < 14 + 20 + 4 || f[12] != 8 || f[13] != 0 || f[23] != 6)
		return -1;
	unsigned ihl = (f[14] & 15) * 4u;
	if (14 + ihl + 4 > len)
		return -1;
	int sp = f[14 + ihl] << 8 | f[14 + ihl + 1];
	int dp = f[14 + ihl + 2] << 8 | f[14 + ihl + 3];
	if (sp >= lo && sp <= hi)
		return sp;
	if (dp >= lo && dp <= hi)
		return dp;
	return -1;
}

/* ================================================================== */
/* amazon                                                              */
/* ================================================================== */

#define AZ_HOSTS 33
#define AZ_CONNS_PER_HOST 6
#define AZ_OBJECTS 500
#define AZ_PORT0 9000
#define AZ_REQ 400
#define AZ_MAXSOCK 1024

struct az_obj {
	int host;
	unsigned size;
	simtime_t t_req, t_done, t_release;
	int done, released;
};

struct az_cconn { /* client side */
	int sock, host;
	int connected;
	int obj; /* current object, -1 idle */
	unsigned long long got;
	int req_sent;
	simtime_t t_last;
};

struct az_sconn { /* server side */
	int active;
	uint8_t req[AZ_REQ];
	unsigned req_got;
	int obj;
	unsigned long long left, off;
};

static struct {
	struct az_obj obj[AZ_OBJECTS];
	int nobj;
	struct az_cconn cc[AZ_HOSTS * AZ_CONNS_PER_HOST];
	int ncc;
	int host_conns[AZ_HOSTS];
	int listen[AZ_HOSTS];
	short cmap[AZ_MAXSOCK]; /* client socket -> cc index + 1 */
	struct az_sconn sc[AZ_MAXSOCK]; /* by server socket */
	int done;
	simtime_t t0;
	unsigned long long stalls;
	double max_gap_ms;
	uint8_t *buf;
} az;

static int az_classify(const uint8_t *f, unsigned len, void *ctx)
{
	(void)ctx;
	int p = server_port(f, len, AZ_PORT0, AZ_PORT0 + AZ_HOSTS - 1);
	return p < 0 ? 0 : 1 + (p - AZ_PORT0);
}

/* The wire gives every client connection its own source address,
 * 10.1.<port / 256>.<port % 256>, and the servers route those through
 * the client: a browser's ~200 connections go to dozens of different
 * servers, which see them as many sources (as on the real network). */
#define NAT_NET 0x0a010000u
static void az_nat(uint8_t *f, unsigned len, void *ctx)
{
	int up = ctx != NULL;

	if (len < 34 || f[12] != 8 || f[13] != 0 || f[23] != 6)
		return;
	uint8_t *ip = f + 14;
	unsigned ihl = (ip[0] & 15) * 4u;
	if (14 + ihl + 4 > len)
		return;
	uint8_t *t = ip + ihl;
	if (up) { /* client -> server: rewrite the source */
		unsigned sp = (unsigned)(t[0] << 8 | t[1]);
		uint32_t a = NAT_NET | sp;
		ip[12] = (uint8_t)(a >> 24);
		ip[13] = (uint8_t)(a >> 16);
		ip[14] = (uint8_t)(a >> 8);
		ip[15] = (uint8_t)a;
	} else { /* server -> client: back to the client's address */
		if (ip[16] != 10 || ip[17] != 1)
			return;
		uint32_t a = inst_ip(0);
		ip[16] = (uint8_t)(a >> 24);
		ip[17] = (uint8_t)(a >> 16);
		ip[18] = (uint8_t)(a >> 8);
		ip[19] = (uint8_t)a;
	}
	frame_fix_csums(f, len);
}

static void az_note_gap(struct az_cconn *c)
{
	double gap = (double)(now_ns() - c->t_last) / 1e6;

	if (gap >= 500)
		az.stalls++;
	if (gap > az.max_gap_ms)
		az.max_gap_ms = gap;
	c->t_last = now_ns();
}

static int az_next_obj(int host)
{
	for (int i = 0; i < az.nobj; i++) {
		struct az_obj *o = &az.obj[i];
		if (o->host == host && o->released && !o->t_req &&
		    o->t_release <= now_ns())
			return i;
	}
	return -1;
}

static void az_client(int ci);

static void az_open_conns(int host)
{
	int want = 0;

	for (int i = 0; i < az.nobj; i++)
		if (az.obj[i].host == host && az.obj[i].released &&
		    !az.obj[i].t_req)
			want++;
	/* idle connections of the host take work first */
	for (int i = 0; i < az.ncc && want > 0; i++)
		if (az.cc[i].host == host && az.cc[i].connected &&
		    az.cc[i].obj < 0) {
			az_client(i);
			want--;
		}
	while (want > 0 && az.host_conns[host] < AZ_CONNS_PER_HOST) {
		int s = tcp_connect_nb(0, inst_ip(1), AZ_PORT0 + (unsigned)host);
		if (s < 0 || s >= AZ_MAXSOCK)
			return;
		struct az_cconn *c = &az.cc[az.ncc];
		memset(c, 0, sizeof(*c));
		c->sock = s;
		c->host = host;
		c->obj = -1;
		az.cmap[s] = (short)(az.ncc + 1);
		az.ncc++;
		az.host_conns[host]++;
		want--;
		az_client(az.ncc - 1);
	}
}

static void az_release(void *arg, long i)
{
	(void)arg;
	az_open_conns(az.obj[i].host);
}

static void az_complete(int oi)
{
	struct az_obj *o = &az.obj[oi];

	o->done = 1;
	o->t_done = now_ns();
	az.done++;
	if (oi == 0) {
		/* the HTML is in: everything else is discovered now, each
		 * after a parse delay of 0..50 ms */
		for (int i = 1; i < az.nobj; i++) {
			az.obj[i].released = 1;
			az.obj[i].t_release =
				now_ns() + (simtime_t)rnd_below(50) * MS;
			sim_at(az.obj[i].t_release, az_release, NULL, i);
		}
	}
}

static void az_client(int ci)
{
	struct az_cconn *c = &az.cc[ci];

	if (!c->connected) {
		int st = NS[0]->tcp_state(c->sock);
		if (st == TS_SYN_SENT || st == TS_SYN_RECEIVED)
			return;
		if (st != TS_ESTABLISHED)
			return; /* failed: its objects wait for another */
		c->connected = 1;
	}
	for (;;) {
		if (c->obj < 0) {
			c->obj = az_next_obj(c->host);
			if (c->obj < 0)
				return;
			struct az_obj *o = &az.obj[c->obj];
			o->t_req = now_ns();
			c->got = 0;
			c->req_sent = 0;
			c->t_last = now_ns();
		}
		struct az_obj *o = &az.obj[c->obj];
		if (!c->req_sent) {
			uint8_t req[AZ_REQ];
			memset(req, 'r', sizeof(req));
			memcpy(req, &c->obj, sizeof(int));
			memcpy(req + 4, &o->size, sizeof(unsigned));
			int r = NS[0]->send(c->sock, req, AZ_REQ, 0);
			if (r != AZ_REQ)
				return; /* a request always fits a fresh window */
			c->req_sent = 1;
		}
		for (;;) {
			int r = NS[0]->recv(c->sock, az.buf, 65536, 0);
			if (r <= 0)
				return;
			az_note_gap(c);
			c->got += (unsigned)r;
			if (c->got >= o->size) {
				az_complete(c->obj);
				c->obj = -1;
				break;
			}
		}
	}
}

static void az_server(int s)
{
	struct az_sconn *x = &az.sc[s];

	for (;;) {
		if (!x->left) {
			int r = NS[1]->recv(s, x->req + x->req_got,
					    AZ_REQ - x->req_got, 0);
			if (r <= 0)
				return;
			x->req_got += (unsigned)r;
			if (x->req_got < AZ_REQ)
				continue;
			x->req_got = 0;
			unsigned size;
			memcpy(&x->obj, x->req, sizeof(int));
			memcpy(&size, x->req + 4, sizeof(unsigned));
			x->left = size;
			x->off = 0;
		}
		while (x->left) {
			unsigned n = x->left > 65536 ? 65536 : (unsigned)x->left;
			pat_fill((unsigned)x->obj, x->off, az.buf, n);
			int r = NS[1]->send(s, az.buf, n, 0);
			if (r <= 0)
				return;
			x->left -= (unsigned)r;
			x->off += (unsigned)r;
		}
	}
}

static void az_app(int inst, int sock, void *ctx)
{
	(void)ctx;
	if (inst == 1) {
		for (int h = 0; h < AZ_HOSTS; h++)
			if (az.listen[h] == sock) {
				int a;
				while ((a = NS[1]->accept(sock, NULL, NULL)) >= 0) {
					NS[1]->set_nonblock(a, 1);
					if (a < AZ_MAXSOCK) {
						memset(&az.sc[a], 0, sizeof(az.sc[a]));
						az.sc[a].active = 1;
						az_server(a);
					}
				}
				return;
			}
		if (sock < AZ_MAXSOCK && az.sc[sock].active)
			az_server(sock);
		return;
	}
	if (sock < AZ_MAXSOCK && az.cmap[sock])
		az_client(az.cmap[sock] - 1);
}

static unsigned long long az_missed;

/* the safety scan: progress here is a wakeup the stack never gave */
static void az_scan(void *arg, long a)
{
	(void)arg;
	(void)a;
	if (az.done == az.nobj)
		return;
	unsigned long long before = 0, after = 0;
	for (int i = 0; i < az.ncc; i++)
		before += az.cc[i].got + (unsigned)az.cc[i].connected;
	for (int h = 0; h < AZ_HOSTS; h++)
		az_app(1, az.listen[h], NULL);
	for (int s = 0; s < AZ_MAXSOCK; s++)
		if (az.sc[s].active)
			az_server(s);
	for (int i = 0; i < az.ncc; i++)
		az_client(i);
	for (int i = 0; i < az.ncc; i++)
		after += az.cc[i].got + (unsigned)az.cc[i].connected;
	if (after != before)
		az_missed++;
	sim_after(10 * MS, az_scan, NULL, 0);
}

static int az_done(void *p)
{
	(void)p;
	return az.done == az.nobj;
}

static int t_amazon(void)
{
	struct link_cfg *down = link_cfg(1), *up = link_cfg(0);

	char key[64];
	const char *run = g_cur_test->arg;

	memset(&az, 0, sizeof(az));
	az.buf = malloc(65536);
	arp_warm();
	/* the page of run sN; another --seed than 1 loads other pages */
	sim_reseed(strtoull(run + 1, NULL, 0) +
		   ((g_opts.seed ? g_opts.seed : 1) - 1) * 1000);
	/* the hosts' paths */
	for (int h = 0; h < AZ_HOSTS; h++) {
		struct path_cfg p = { 0 };
		double rtt = 20 + rnd01() * 100;
		p.delay_ms = rtt / 2;
		p.loss = 0.001 + rnd01() * 0.009;
		p.rate_mbps = 50 + rnd01() * 150;
		p.queue_bytes = 512 * 1024;
		down->path[1 + h] = p;
		p.rate_mbps = 0;
		up->path[1 + h] = p;
	}
	down->classify = up->classify = az_classify;
	up->rewrite = az_nat;
	up->rewrite_ctx = (void *)1;
	down->rewrite = az_nat;
	down->rewrite_ctx = NULL;
	NS[1]->route_add(0, 0, inst_ip(0));
	down->rate_mbps = 200;
	down->queue_bytes = 1024 * 1024;
	/* the objects: the HTML first, then a Zipf-ish spread over hosts */
	double wsum = 0;
	for (int h = 0; h < AZ_HOSTS; h++)
		wsum += 1.0 / (h + 1);
	az.nobj = AZ_OBJECTS;
	for (int i = 0; i < az.nobj; i++) {
		struct az_obj *o = &az.obj[i];
		if (i == 0) {
			o->host = 0;
			o->size = 100 * 1024;
			o->released = 1;
			continue;
		}
		double u = rnd01() * wsum, acc = 0;
		o->host = AZ_HOSTS - 1;
		for (int h = 0; h < AZ_HOSTS; h++) {
			acc += 1.0 / (h + 1);
			if (u <= acc) {
				o->host = h;
				break;
			}
		}
		/* bounded Pareto, alpha 1.1, 2 KB .. 500 KB */
		double L = 2048, H = 500 * 1024, a = 1.1;
		double x = rnd01();
		double v = pow(-(x * pow(H, a) - x * pow(L, a) - pow(H, a)) /
				       (pow(H, a) * pow(L, a)),
			       -1.0 / a);
		o->size = (unsigned)v;
	}
	unsigned long long total = 0;
	for (int i = 0; i < az.nobj; i++)
		total += az.obj[i].size;
	/* the servers */
	for (int h = 0; h < AZ_HOSTS; h++) {
		int ls = NS[1]->socket(K_SOCK_STREAM);
		NS[1]->set_nonblock(ls, 1);
		CHECK(NS[1]->bind(ls, 0, AZ_PORT0 + (unsigned)h) == 0 &&
			      NS[1]->listen(ls, 128) == 0,
		      "listen %d", h);
		az.listen[h] = ls;
	}
	unsigned long long r0 = retrans_total();
	sim_set_app(az_app, NULL);
	az.t0 = now_ns();
	az_open_conns(0);
	sim_after(10 * MS, az_scan, NULL, 0);
	sim_run_cond(az_done, NULL, 300 * SEC);
	sim_set_app(NULL, NULL);
	if (g_verbose)
		for (int i = 0; i < az.ncc; i++) {
			struct az_cconn *c = &az.cc[i];
			tlog("conn %d host %d sock %d state %d connected %d obj %d "
			     "got %llu req_sent %d so_error %d",
			     i, c->host, c->sock, NS[0]->tcp_state(c->sock),
			     c->connected, c->obj, c->got, c->req_sent,
			     so_error(0, c->sock));
		}
	if (g_verbose)
		for (int i = 0; i < 2; i++)
			tlog("inst %d: attempt fails %llu out rsts %llu acceptq "
			     "full %llu estab resets %llu conns %llu",
			     i, NS[i]->counter(NSC_TCP_ATTEMPT_FAILS),
			     NS[i]->counter(NSC_TCP_OUT_RSTS),
			     NS[i]->counter(NSC_TCP_ACCEPTQ_FULL),
			     NS[i]->counter(NSC_TCP_ESTAB_RESETS),
			     NS[i]->counter(NSC_TCP_CONNS));
	/* responses still outstanding at the end stalled until now */
	for (int i = 0; i < az.ncc; i++)
		if (az.cc[i].obj >= 0)
			az_note_gap(&az.cc[i]);

	double *ot = malloc(sizeof(double) * (size_t)az.nobj);
	int n = 0;
	simtime_t last = az.t0;
	for (int i = 0; i < az.nobj; i++)
		if (az.obj[i].done) {
			ot[n++] = (double)(az.obj[i].t_done - az.obj[i].t_req) / 1e6;
			if (az.obj[i].t_done > last)
				last = az.obj[i].t_done;
		}
	printf("    %d objects, %.1f MB, %d connections\n", az.nobj,
	       (double)total / 1e6, az.ncc);
#define AZR(k, v)                                                         \
	do {                                                              \
		snprintf(key, sizeof(key), "amazon.%s.%s", run, k);        \
		result(key, v);                                           \
	} while (0)
	AZR("total_ms", az.done == az.nobj ? (double)(last - az.t0) / 1e6 :
					     300000.0);
	AZR("objects_failed", az.nobj - az.done);
	AZR("obj_p50_ms", pct(ot, n, 0.5));
	AZR("obj_p95_ms", pct(ot, n, 0.95));
	AZR("obj_max_ms", pct(ot, n, 1.0));
	AZR("retrans", (double)(retrans_total() - r0));
	AZR("stalls", (double)az.stalls);
	AZR("max_stall_ms", az.max_gap_ms);
	AZR("missed_wakeups", (double)az_missed);
	free(ot);
	free(az.buf);
	return 0;
}

/* ================================================================== */
/* live stream                                                         */
/* ================================================================== */

#define LV_RATE 750000.0 /* bytes per second: 6 Mbit/s */
#define LV_SECS 120
#define LV_BUFFER_S 2.0

static struct {
	int cs, ss;
	unsigned long long produced, sent, got;
	simtime_t t_start, t_last_data;
	double max_gap_ms;
	/* playout */
	int playing, started;
	double pos; /* content seconds played */
	simtime_t t_startup, t_rebuf;
	int rebuffers;
	double rebuf_total, rebuf_max;
	unsigned long long missed;
	uint8_t *buf;
} lv;

static void lv_server(void)
{
	while (lv.sent < lv.produced) {
		unsigned long long n = lv.produced - lv.sent;
		if (n > 65536)
			n = 65536;
		pat_fill(77, lv.sent, lv.buf, n);
		int r = NS[1]->send(lv.ss, lv.buf, n, 0);
		if (r <= 0)
			return;
		lv.sent += (unsigned)r;
	}
}

static int lv_client(void)
{
	int prog = 0;

	for (;;) {
		int r = NS[0]->recv(lv.cs, lv.buf, 65536, 0);
		if (r <= 0)
			break;
		double gap = (double)(now_ns() - lv.t_last_data) / 1e6;
		if (lv.got && gap > lv.max_gap_ms)
			lv.max_gap_ms = gap;
		lv.t_last_data = now_ns();
		lv.got += (unsigned)r;
		prog = 1;
	}
	return prog;
}

static void lv_app(int inst, int sock, void *ctx)
{
	(void)ctx;
	if (inst == 1 && sock == lv.ss)
		lv_server();
	else if (inst == 0 && sock == lv.cs)
		lv_client();
}

/* every 10 ms: the encoder produces, the player consumes */
static void lv_tick(void *arg, long a)
{
	(void)arg;
	(void)a;
	double t = (double)(now_ns() - lv.t_start) / 1e9;

	if (t <= LV_SECS)
		lv.produced = (unsigned long long)(t * LV_RATE);
	lv_server();
	if (lv_client())
		lv.missed++;
	double have = (double)lv.got / LV_RATE; /* content seconds received */
	if (!lv.started) {
		if (have >= LV_BUFFER_S) {
			lv.started = lv.playing = 1;
			lv.t_startup = now_ns() - lv.t_start;
		}
	} else if (lv.playing) {
		lv.pos += 0.010;
		if (lv.pos >= have && lv.pos < LV_SECS - 0.01) {
			lv.playing = 0;
			lv.rebuffers++;
			lv.t_rebuf = now_ns();
		}
	} else if (have - lv.pos >= LV_BUFFER_S || have >= LV_SECS - 0.01) {
		double d = (double)(now_ns() - lv.t_rebuf) / 1e9;
		lv.rebuf_total += d;
		if (d > lv.rebuf_max)
			lv.rebuf_max = d;
		lv.playing = 1;
	}
	if (t < LV_SECS + 30)
		sim_after(10 * MS, lv_tick, NULL, 0);
}

static int t_live(void)
{
	struct path_cfg p = { 0 };

	memset(&lv, 0, sizeof(lv));
	lv.buf = malloc(65536);
	arp_warm();
	p.delay_ms = 30;
	p.loss = 0.001;
	link_set_both(&p);
	link_cfg(1)->path[0].burst_period_ms = 10000;
	link_cfg(1)->path[0].burst_len_ms = 300;
	link_cfg(1)->path[0].burst_offset_ms = 5000; /* first at 5 s in */
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 20;
	link_cfg(0)->queue_bytes = link_cfg(1)->queue_bytes = 256 * 1024;
	CHECK(tcp_pair(0, 1, 7000, &lv.cs, &lv.ss, NULL, 10 * SEC) == 0,
	      "no connection");
	unsigned long long r0 = retrans_total();
	lv.t_start = now_ns();
	link_cfg(1)->path[0].burst_offset_ms = (double)lv.t_start / 1e6 + 5000;
	lv.t_last_data = now_ns();
	sim_set_app(lv_app, NULL);
	sim_after(10 * MS, lv_tick, NULL, 0);
	sim_run_for((LV_SECS + 30) * SEC);
	sim_set_app(NULL, NULL);
	if (!lv.playing && lv.started) { /* still stalled at the end */
		double d = (double)(now_ns() - lv.t_rebuf) / 1e9;
		lv.rebuf_total += d;
		if (d > lv.rebuf_max)
			lv.rebuf_max = d;
	}
	result("live.startup_ms", lv.started ? (double)lv.t_startup / 1e6 :
					       (double)(LV_SECS * 1000));
	result("live.rebuffers", lv.rebuffers);
	result("live.rebuffer_ms", lv.rebuf_total * 1000);
	result("live.max_rebuffer_ms", lv.rebuf_max * 1000);
	result("live.max_stall_ms", lv.max_gap_ms);
	result("live.delivered_pct",
	       100.0 * (double)lv.got / (LV_RATE * LV_SECS));
	result("live.retrans", (double)(retrans_total() - r0));
	result("live.missed_wakeups", (double)lv.missed);
	free(lv.buf);
	return 0;
}

/* ================================================================== */
/* bulk                                                                */
/* ================================================================== */

static int bulk_run(double rtt_ms, double loss)
{
	struct path_cfg p = { 0 };
	int c, s;
	char key[96];

	arp_warm();
	p.delay_ms = rtt_ms / 2;
	p.loss = loss;
	link_set_both(&p);
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 1000;
	link_cfg(0)->queue_bytes = link_cfg(1)->queue_bytes = 4 * 1024 * 1024;
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 30 * SEC) == 0,
	      "no connection");
	unsigned long long r0 = retrans_total();
	struct flow f;
	flow_init(&f, 1, s, 0, c, 1, 100ULL * 1000 * 1000);
	flows_run(&f, 1, 300 * SEC);
	CHECK(!f.bad, "data corrupted");
	double secs = (double)((f.done ? f.t_done : now_ns()) - f.t_start) / 1e9;
	const char *ls = loss == 0 ? "0" : loss < 0.005 ? "0.1" : "1";
	snprintf(key, sizeof(key), "bulk.rtt%.0f.loss%s.goodput_mbps", rtt_ms,
		 ls);
	result(key, secs > 0 ? (double)f.got * 8 / secs / 1e6 : 0);
	snprintf(key, sizeof(key), "bulk.rtt%.0f.loss%s.retrans", rtt_ms, ls);
	result(key, (double)(retrans_total() - r0));
	return 0;
}

static int t_bulk(void)
{
	double rtt = 10, loss = 0;

	sscanf(g_cur_test->arg, "%lf %lf", &rtt, &loss);
	return bulk_run(rtt, loss);
}

#define B(n, a) { .name = n, .group = "bench", .fn = t_bulk, .arg = a, \
		  .limit = 400 * SEC, .allow_warn = 1 }
const struct test tests_bench[] = {
	{ .name = "bench-amazon-1", .group = "bench", .fn = t_amazon,
	  .arg = "s1", .limit = 400 * SEC, .allow_warn = 1 },
	{ .name = "bench-amazon-2", .group = "bench", .fn = t_amazon,
	  .arg = "s2", .limit = 400 * SEC, .allow_warn = 1 },
	{ .name = "bench-amazon-3", .group = "bench", .fn = t_amazon,
	  .arg = "s3", .limit = 400 * SEC, .allow_warn = 1 },
	{ .name = "bench-live", .group = "bench", .fn = t_live,
	  .limit = 400 * SEC, .allow_warn = 1 },
	B("bench-bulk-rtt10-loss0", "10 0"),
	B("bench-bulk-rtt10-loss0.1", "10 0.001"),
	B("bench-bulk-rtt10-loss1", "10 0.01"),
	B("bench-bulk-rtt50-loss0", "50 0"),
	B("bench-bulk-rtt50-loss0.1", "50 0.001"),
	B("bench-bulk-rtt50-loss1", "50 0.01"),
	B("bench-bulk-rtt150-loss0", "150 0"),
	B("bench-bulk-rtt150-loss0.1", "150 0.001"),
	B("bench-bulk-rtt150-loss1", "150 0.01"),
	{ 0 },
};
