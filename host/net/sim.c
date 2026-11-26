/* The network harness's discrete-event simulator, simulated wire, pcap
 * writer and the host services the stack instances call (hs_*).
 * See sim.h and host/net/README.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "sim.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <execinfo.h>
#include <unistd.h>

/* the instances: each exports its own ns_ops after the symbol rename */
extern const struct ns_ops n0_ns_ops, n1_ns_ops;
const struct ns_ops *NS[NINST] = { &n0_ns_ops, &n1_ns_ops };

int g_verbose;
int g_check_failed;

unsigned inst_ip(int i)
{
	return (10u << 24) | (unsigned)(i + 1);
}

void inst_mac(int i, unsigned char mac[6])
{
	mac[0] = 0x02;
	mac[1] = mac[2] = mac[3] = mac[4] = 0;
	mac[5] = (unsigned char)(i + 1);
}

/* ------------------------------------------------------------------ */
/* randomness                                                           */
/* ------------------------------------------------------------------ */

static uint64_t rs[4];

static uint64_t rotl(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t splitmix(uint64_t *x)
{
	uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static void rnd_seed(uint64_t s)
{
	for (int i = 0; i < 4; i++)
		rs[i] = splitmix(&s);
}

void sim_reseed(uint64_t seed)
{
	rnd_seed(seed ^ 0xbe7cULL);
}

uint64_t rnd64(void)
{
	uint64_t r = rotl(rs[1] * 5, 7) * 9, t = rs[1] << 17;

	rs[2] ^= rs[0];
	rs[3] ^= rs[1];
	rs[1] ^= rs[2];
	rs[0] ^= rs[3];
	rs[2] ^= t;
	rs[3] = rotl(rs[3], 45);
	return r;
}

double rnd01(void)
{
	return (double)(rnd64() >> 11) * (1.0 / 9007199254740992.0);
}

unsigned rnd_below(unsigned n)
{
	return n ? (unsigned)(rnd64() % n) : 0;
}

/* ------------------------------------------------------------------ */
/* the event queue                                                      */
/* ------------------------------------------------------------------ */

struct event {
	simtime_t t;
	unsigned long long seq;
	ev_fn fn;
	void *arg;
	long a;
};

static struct event *heap;
static size_t heap_n, heap_cap;
static unsigned long long ev_seq;
static simtime_t clock_ns;
static simtime_t time_limit;
static unsigned tick_hz = 200;
static int n_inst = NINST;
static unsigned long long seed_base;
static int loop_depth;

static int ev_less(const struct event *a, const struct event *b)
{
	return a->t < b->t || (a->t == b->t && a->seq < b->seq);
}

void sim_at(simtime_t t, ev_fn fn, void *arg, long a)
{
	if (t < clock_ns)
		t = clock_ns;
	if (heap_n == heap_cap) {
		heap_cap = heap_cap ? heap_cap * 2 : 4096;
		heap = realloc(heap, heap_cap * sizeof(*heap));
		if (!heap) {
			fprintf(stderr, "test-net: out of memory (events)\n");
			abort();
		}
	}
	size_t i = heap_n++;
	struct event e = { t, ev_seq++, fn, arg, a };

	while (i > 0) {
		size_t p = (i - 1) / 2;
		if (!ev_less(&e, &heap[p]))
			break;
		heap[i] = heap[p];
		i = p;
	}
	heap[i] = e;
}

void sim_after(simtime_t dt, ev_fn fn, void *arg, long a)
{
	sim_at(clock_ns + dt, fn, arg, a);
}

static struct event ev_pop(void)
{
	struct event top = heap[0], last = heap[--heap_n];
	size_t i = 0;

	for (;;) {
		size_t c = 2 * i + 1;
		if (c >= heap_n)
			break;
		if (c + 1 < heap_n && ev_less(&heap[c + 1], &heap[c]))
			c++;
		if (!ev_less(&heap[c], &last))
			break;
		heap[i] = heap[c];
		i = c;
	}
	if (heap_n)
		heap[i] = last;
	return top;
}

simtime_t now_ns(void)
{
	return clock_ns;
}

double now_s(void)
{
	return (double)clock_ns / 1e9;
}

/* ------------------------------------------------------------------ */
/* readiness notifications -> the application                            */
/* ------------------------------------------------------------------ */

#define MAX_SOCK 1024
static app_fn app_cb;
static void *app_ctx;
static unsigned char dirty[NINST][MAX_SOCK];
static int *dirty_q;
static size_t dirty_n, dirty_cap;
static unsigned long long notif_count[NINST];

void sim_set_app(app_fn fn, void *ctx)
{
	app_cb = fn;
	app_ctx = ctx;
}

unsigned long long sim_notifications(int inst)
{
	return notif_count[inst];
}

void hs_notify(int inst, int sock)
{
	if (inst < 0 || inst >= NINST)
		return;
	notif_count[inst]++;
	if (sock < 0 || sock >= MAX_SOCK || !app_cb || dirty[inst][sock])
		return;
	dirty[inst][sock] = 1;
	if (dirty_n == dirty_cap) {
		dirty_cap = dirty_cap ? dirty_cap * 2 : 1024;
		dirty_q = realloc(dirty_q, dirty_cap * sizeof(*dirty_q));
		if (!dirty_q)
			abort();
	}
	dirty_q[dirty_n++] = inst * MAX_SOCK + sock;
}

static void run_app(void)
{
	size_t i = 0;

	if (!app_cb || loop_depth)
		return;
	/* the callbacks may queue more */
	while (i < dirty_n) {
		int v = dirty_q[i++];
		int inst = v / MAX_SOCK, sock = v % MAX_SOCK;

		dirty[inst][sock] = 0;
		app_cb(inst, sock, app_ctx);
		if (i > 4000000) {
			fprintf(stderr, "test-net: application livelock\n");
			abort();
		}
	}
	dirty_n = 0;
}

/* ------------------------------------------------------------------ */
/* running                                                              */
/* ------------------------------------------------------------------ */

static void hs_fail_hang(int inst)
{
	fprintf(stderr,
		"\nFATAL: instance %d sleeps past the simulation limit "
		"(%.3f s): a blocking call nothing will ever complete\n",
		inst, (double)time_limit / 1e9);
	hs_backtrace();
	hs_abort();
}

int sim_step(void)
{
	if (!heap_n || heap[0].t > time_limit)
		return 0;
	struct event e = ev_pop();

	if (e.t > clock_ns)
		clock_ns = e.t;
	e.fn(e.arg, e.a);
	run_app();
	return 1;
}

void sim_run_until_time(simtime_t t)
{
	while (heap_n && heap[0].t <= t && heap[0].t <= time_limit)
		sim_step();
	if (clock_ns < t)
		clock_ns = t;
}

void sim_run_for(simtime_t dt)
{
	sim_run_until_time(clock_ns + dt);
}

int sim_run_cond(int (*cond)(void *), void *arg, simtime_t dt)
{
	simtime_t end = clock_ns + dt;

	run_app();
	while (!cond(arg)) {
		if (!heap_n || heap[0].t > end || heap[0].t > time_limit) {
			if (clock_ns < end)
				clock_ns = end;
			return cond(arg);
		}
		sim_step();
	}
	return 1;
}

int hs_block(int inst, int (*still_blocked)(void),
	     unsigned long long deadline_ns)
{
	loop_depth++;
	while (still_blocked()) {
		if (!heap_n || heap[0].t > time_limit) {
			if (deadline_ns && deadline_ns <= time_limit) {
				clock_ns = deadline_ns;
				loop_depth--;
				return 1;
			}
			hs_fail_hang(inst);
		}
		if (deadline_ns && heap[0].t > deadline_ns) {
			if (clock_ns < deadline_ns)
				clock_ns = deadline_ns;
			loop_depth--;
			return 1;
		}
		sim_step();
	}
	loop_depth--;
	return 0;
}

void hs_yield(int inst)
{
	loop_depth++;
	if (!sim_step())
		hs_fail_hang(inst);
	loop_depth--;
}

unsigned long long hs_now_ns(void)
{
	return clock_ns;
}

unsigned hs_hz(void)
{
	return tick_hz;
}

unsigned long long hs_seed(int inst)
{
	return seed_base * 0x9E3779B97F4A7C15ULL + 0x1234567ULL * (unsigned)(inst + 1);
}

/* ------------------------------------------------------------------ */
/* the wire                                                             */
/* ------------------------------------------------------------------ */

struct path_state {
	simtime_t busy; /* path serializer free at */
	int ge_bad; /* Gilbert-Elliott state */
};

static struct link_cfg links[NINST];
static struct link_stats lstats[NINST];
static struct path_state pstate[NINST][MAX_PATHS];
static simtime_t link_busy[NINST];
static tap_fn tap_cb;
static void *tap_ctx;
static int detached[NINST];
static FILE *pcap_f;
static int pcap_at_rx;

struct link_cfg *link_cfg(int dir)
{
	return &links[dir];
}

const struct link_stats *link_stats(int dir)
{
	return &lstats[dir];
}

void link_set_both(const struct path_cfg *p)
{
	links[0].path[0] = *p;
	links[1].path[0] = *p;
}

void wire_set_tap(tap_fn fn, void *ctx)
{
	tap_cb = fn;
	tap_ctx = ctx;
}

void wire_detach(int inst)
{
	detached[inst] = 1;
}

int pcap_open(const char *path, int at_rx)
{
	static const uint32_t hdr[6] = { 0xa1b23c4d, 0x00040002, 0, 0, 65535,
					 1 };

	pcap_close();
	pcap_f = fopen(path, "wb");
	if (!pcap_f)
		return -1;
	pcap_at_rx = at_rx;
	fwrite(hdr, sizeof(hdr), 1, pcap_f);
	return 0;
}

void pcap_close(void)
{
	if (pcap_f)
		fclose(pcap_f);
	pcap_f = NULL;
}

static void pcap_write(const uint8_t *f, unsigned len)
{
	uint32_t rec[4];

	if (!pcap_f)
		return;
	rec[0] = (uint32_t)(clock_ns / SEC);
	rec[1] = (uint32_t)(clock_ns % SEC);
	rec[2] = len;
	rec[3] = len;
	fwrite(rec, sizeof(rec), 1, pcap_f);
	fwrite(f, len, 1, pcap_f);
}

struct frame {
	int dst; /* instance */
	int dir; /* link */
	int path;
	unsigned len;
	uint8_t data[];
};

static struct frame *frame_new(int dst, int dir, int path, const void *data,
			       unsigned len)
{
	struct frame *fr = malloc(sizeof(*fr) + len);

	if (!fr)
		abort();
	fr->dst = dst;
	fr->dir = dir;
	fr->path = path;
	fr->len = len;
	memcpy(fr->data, data, len);
	return fr;
}

/* Frames waiting for the receiving NIC's next interrupt (rx_batch_us). */
static struct frame **rxb[NINST];
static unsigned rxb_n[NINST], rxb_cap[NINST];
static int rxb_armed[NINST];
/* TESTNET_CPU_NS: receive processing costs this much simulated time per
 * frame -- an instance busy with earlier frames takes the next one when
 * it is done (0: frames are processed the instant they arrive). */
static simtime_t cpu_ns_per_frame;
static simtime_t cpu_free[NINST];

/* When instance `d' can process `n' frames that are ready now; reserves
 * the CPU time for them. */
static simtime_t cpu_claim(int d, unsigned n)
{
	simtime_t start = cpu_free[d] > clock_ns ? cpu_free[d] : clock_ns;

	if (cpu_ns_per_frame)
		cpu_free[d] = start + n * cpu_ns_per_frame;
	return start;
}

static void ev_rx_batch(void *arg, long inst)
{
	struct frame **list = rxb[inst];
	unsigned n = rxb_n[inst];

	(void)arg;
	if (cpu_ns_per_frame && cpu_free[inst] > clock_ns) {
		/* still busy: the batch (and what joins it) waits */
		sim_at(cpu_free[inst], ev_rx_batch, NULL, inst);
		return;
	}
	cpu_claim((int)inst, n);
	/* frames arriving while the batch is handed over start a new one */
	rxb[inst] = NULL;
	rxb_n[inst] = rxb_cap[inst] = 0;
	rxb_armed[inst] = 0;
	for (unsigned i = 0; i < n; i++) {
		struct frame *fr = list[i];

		lstats[fr->dir].delivered++;
		if (pcap_at_rx)
			pcap_write(fr->data, fr->len);
		NS[inst]->rx_nopump(fr->data, fr->len);
		free(fr);
	}
	free(list);
	NS[inst]->pump();
}

static void ev_process(void *arg, long a)
{
	struct frame *fr = arg;

	(void)a;
	lstats[fr->dir].delivered++;
	if (pcap_at_rx)
		pcap_write(fr->data, fr->len);
	NS[fr->dst]->rx(fr->data, fr->len);
	free(fr);
}

static void ev_deliver(void *arg, long a)
{
	struct frame *fr = arg;
	double batch_us = links[fr->dir].rx_batch_us;

	(void)a;
	if (batch_us > 0) {
		int d = fr->dst;

		if (rxb_n[d] == rxb_cap[d]) {
			rxb_cap[d] = rxb_cap[d] ? 2 * rxb_cap[d] : 64;
			rxb[d] = realloc(rxb[d], rxb_cap[d] * sizeof(*rxb[d]));
			if (!rxb[d])
				abort();
		}
		rxb[d][rxb_n[d]++] = fr;
		if (!rxb_armed[d]) {
			rxb_armed[d] = 1;
			sim_at(clock_ns + (simtime_t)(batch_us * 1000.0),
			       ev_rx_batch, NULL, d);
		}
		return;
	}
	if (cpu_ns_per_frame) {
		simtime_t start = cpu_claim(fr->dst, 1);

		if (start > clock_ns) {
			sim_at(start, ev_process, fr, 0);
			return;
		}
	}
	ev_process(fr, 0);
}

/* past the bottleneck: propagation, jitter, reordering, duplication */
static void wire_propagate(struct frame *fr, simtime_t t)
{
	const struct path_cfg *p = &links[fr->dir].path[fr->path];
	struct link_stats *st = &lstats[fr->dir];
	simtime_t at = t + (simtime_t)(p->delay_ms * MS);

	if (p->jitter_ms > 0)
		at += (simtime_t)(rnd01() * p->jitter_ms * MS);
	if (p->reorder > 0 && rnd01() < p->reorder) {
		at += (simtime_t)(p->reorder_ms * MS);
		st->reordered++;
	}
	if (p->dup > 0 && rnd01() < p->dup) {
		st->dup++;
		sim_at(at + 1000,
		       ev_deliver,
		       frame_new(fr->dst, fr->dir, fr->path, fr->data, fr->len),
		       0);
	}
	sim_at(at, ev_deliver, fr, 0);
}

static simtime_t ser_ns(unsigned len, double mbps)
{
	return (simtime_t)((double)len * 8.0 * 1000.0 / mbps);
}

/* the shared bottleneck of the link */
static void ev_bottleneck(void *arg, long a)
{
	struct frame *fr = arg;
	struct link_cfg *L = &links[fr->dir];
	simtime_t t = clock_ns;

	(void)a;
	if (L->rate_mbps > 0) {
		simtime_t busy = link_busy[fr->dir];
		if (L->queue_bytes && busy > t &&
		    (double)(busy - t) * L->rate_mbps / 8000.0 + fr->len >
			    (double)L->queue_bytes) {
			lstats[fr->dir].drop_queue++;
			free(fr);
			return;
		}
		if (busy < t)
			busy = t;
		busy += ser_ns(fr->len, L->rate_mbps);
		link_busy[fr->dir] = busy;
		t = busy;
	}
	wire_propagate(fr, t);
}

static int path_loses(const struct path_cfg *p, struct path_state *ps)
{
	if (p->burst_period_ms > 0 && p->burst_len_ms > 0) {
		double ms = (double)clock_ns / 1e6 - p->burst_offset_ms;
		if (ms >= 0) {
			double ph = ms - p->burst_period_ms *
						 (double)(long long)(ms / p->burst_period_ms);
			if (ph < p->burst_len_ms)
				return 1;
		}
	}
	if (p->ge_p_gb > 0) {
		if (ps->ge_bad) {
			if (rnd01() < p->ge_p_bg)
				ps->ge_bad = 0;
		} else if (rnd01() < p->ge_p_gb) {
			ps->ge_bad = 1;
		}
		if (ps->ge_bad && rnd01() < p->ge_loss_bad)
			return 1;
	}
	return p->loss > 0 && rnd01() < p->loss;
}

void hs_wire_tx(int inst, const void *data, unsigned len)
{
	const uint8_t *f = data;
	int dir = inst, dst = inst ^ 1;
	struct link_cfg *L;
	struct link_stats *st;

	if (inst < 0 || inst >= NINST)
		abort();
	L = &links[dir];
	st = &lstats[dir];
	st->tx_frames++;
	st->tx_bytes += len;
	if (tap_cb && tap_cb(inst, f, len, tap_ctx))
		return;
	if (!pcap_at_rx)
		pcap_write(f, len);
	if (detached[inst] || dst >= n_inst || detached[dst])
		return;
	if (L->mtu && len > L->mtu + 14) {
		st->drop_mtu++;
		return;
	}
	struct frame *fr = frame_new(dst, dir, 0, f, len);
	if (L->rewrite)
		L->rewrite(fr->data, len, L->rewrite_ctx);
	int pi = L->classify ? L->classify(fr->data, len, L->classify_ctx) : 0;
	if (pi < 0 || pi >= MAX_PATHS)
		pi = 0;
	fr->path = pi;
	const struct path_cfg *p = &L->path[pi];
	struct path_state *ps = &pstate[dir][pi];

	if (path_loses(p, ps)) {
		st->drop_loss++;
		free(fr);
		return;
	}
	if (p->corrupt > 0 && len > 14 && rnd01() < p->corrupt) {
		unsigned bit = 14 * 8 + rnd_below((len - 14) * 8);
		fr->data[bit / 8] ^= (uint8_t)(1u << (bit % 8));
		st->corrupted++;
	}
	simtime_t t = clock_ns;
	if (p->rate_mbps > 0) {
		if (p->queue_bytes && ps->busy > t &&
		    (double)(ps->busy - t) * p->rate_mbps / 8000.0 + len >
			    (double)p->queue_bytes) {
			st->drop_queue++;
			free(fr);
			return;
		}
		if (ps->busy < t)
			ps->busy = t;
		ps->busy += ser_ns(len, p->rate_mbps);
		sim_at(ps->busy, ev_bottleneck, fr, 0);
		return;
	}
	ev_bottleneck(fr, 0);
}

/* ------------------------------------------------------------------ */
/* the instances' timer ticks                                           */
/* ------------------------------------------------------------------ */

static unsigned long long tick_k[NINST];

static simtime_t tick_time(unsigned long long k)
{
	return (k / tick_hz) * SEC + (k % tick_hz) * SEC / tick_hz;
}

static void ev_tick(void *arg, long inst)
{
	(void)arg;
	NS[inst]->tick();
	tick_k[inst]++;
	sim_at(tick_time(tick_k[inst]), ev_tick, NULL, inst);
}

void sim_init(const struct sim_opts *o)
{
	seed_base = o->seed;
	rnd_seed(o->seed ^ 0x5eedULL);
	tick_hz = o->hz ? o->hz : 200;
	n_inst = o->ninst ? o->ninst : NINST;
	time_limit = (o->limit ? o->limit : 3600 * SEC) + SEC;
	clock_ns = SEC; /* the stacks boot at 1 s uptime */
	{
		/* "A[,B]": instance 0's NIC (link 1) batches A us, instance
		 * 1's (link 0) B us (default 0) */
		const char *b = getenv("TESTNET_RX_BATCH_US");
		const char *cpu = getenv("TESTNET_CPU_NS");

		cpu_ns_per_frame = cpu ? (simtime_t)atoll(cpu) : 0;

		if (b) {
			const char *comma = strchr(b, ',');

			links[1].rx_batch_us = atof(b);
			links[0].rx_batch_us = comma ? atof(comma + 1) : 0;
		}
	}
	for (int i = 0; i < n_inst; i++) {
		struct ns_cfg c;

		memset(&c, 0, sizeof(c));
		c.gro_off = getenv("TESTNET_GRO_OFF") && atoi(getenv("TESTNET_GRO_OFF"));
		c.ip = inst_ip(i);
		c.netmask = 0xFFFFFF00u;
		c.gateway = 0;
		inst_mac(i, c.mac);
		c.mtu = o->mtu ? o->mtu : 1500;
		if (NS[i]->init(i, &c) < 0) {
			fprintf(stderr, "test-net: instance %d init failed\n", i);
			exit(2);
		}
		tick_k[i] = clock_ns / SEC * tick_hz + 1;
		sim_at(tick_time(tick_k[i]), ev_tick, NULL, i);
	}
}

/* ------------------------------------------------------------------ */
/* host services                                                        */
/* ------------------------------------------------------------------ */

#define KLOG_LINES 400
static char *klog_ring[KLOG_LINES];
static unsigned klog_pos;

void hs_klog(int inst, const char *text)
{
	char buf[2200];

	snprintf(buf, sizeof(buf), "[n%d %10.6f] %s", inst, now_s(), text);
	if (inst < 0 || g_verbose) {
		fputs(buf, stderr);
		if (buf[0] && buf[strlen(buf) - 1] != '\n')
			fputc('\n', stderr);
	}
	free(klog_ring[klog_pos % KLOG_LINES]);
	klog_ring[klog_pos % KLOG_LINES] = strdup(buf);
	klog_pos++;
}

void klog_dump_tail(FILE *f, int lines)
{
	unsigned n = klog_pos < KLOG_LINES ? klog_pos : KLOG_LINES;

	if ((unsigned)lines < n)
		n = (unsigned)lines;
	for (unsigned i = klog_pos - n; i < klog_pos; i++) {
		const char *l = klog_ring[i % KLOG_LINES];
		if (!l)
			continue;
		fputs("    ", f);
		fputs(l, f);
		if (l[0] && l[strlen(l) - 1] != '\n')
			fputc('\n', f);
	}
}

void tlog(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	fprintf(stderr, "[test %10.6f] ", now_s());
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void *hs_malloc(unsigned long n)
{
	return malloc(n);
}

void hs_free(void *p)
{
	free(p);
}

void *hs_memset(void *p, int c, unsigned long n)
{
	return memset(p, c, n);
}

void *hs_memcpy(void *d, const void *s, unsigned long n)
{
	return memmove(d, s, n);
}

int hs_memcmp(const void *a, const void *b, unsigned long n)
{
	return memcmp(a, b, n);
}

int hs_vsnprintf(char *b, unsigned long n, const char *fmt,
		 __builtin_va_list ap)
{
	return vsnprintf(b, n, fmt, ap);
}

void __sanitizer_print_stack_trace(void) __attribute__((weak));

void hs_backtrace(void)
{
	if (__sanitizer_print_stack_trace) {
		__sanitizer_print_stack_trace();
	} else {
		void *bt[64];
		int n = backtrace(bt, 64);
		backtrace_symbols_fd(bt, n, 2);
	}
}

void hs_abort(void)
{
	fflush(stdout);
	fprintf(stderr, "--- last kernel log lines ---\n");
	klog_dump_tail(stderr, 40);
	fflush(stderr);
	abort();
}

/* auto-generated stubs for kernel functions the harness does not
 * provide land here */
void hs_unstubbed(const char *name) __attribute__((noreturn));
void hs_unstubbed(const char *name)
{
	fprintf(stderr, "\nFATAL: the stack called %s(), which the harness "
			"does not implement (add it to host/net/kstubs.c)\n",
		name);
	hs_backtrace();
	hs_abort();
}
