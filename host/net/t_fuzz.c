/* The receive-path fuzzer: mutated Ethernet frames into instance 0 under
 * AddressSanitizer and UndefinedBehaviorSanitizer.
 *
 * The corpus is recorded first from real traffic between the two
 * instances (handshakes, data, FIN, RST, UDP, fragments, ICMP, ARP),
 * then frames are mutated -- bit flips, random bytes, interesting header
 * values, truncation, extension, splicing -- and, half of the time, their
 * checksums repaired so the mutation reaches the protocol code behind the
 * checksum checks.  Normal traffic keeps flowing meanwhile and the timers
 * run.  Afterwards the instance must still work: a new connection moves
 * data intact and it answers a ping.
 *
 * A crash leaves the frame that caused it in
 * $TESTNET_DIR/fuzz-crash-<seed>-<iteration>.bin (and a one-frame pcap);
 * the run is a function of the seed, so --seed S --iters N reproduces.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define CORPUS_MAX 4096
struct cframe {
	unsigned len;
	uint8_t f[1600];
};
static struct cframe *corpus;
static int ncorpus;

static int record_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	(void)ctx;
	if (inst == 1 && ncorpus < CORPUS_MAX && len <= 1600) {
		corpus[ncorpus].len = len;
		memcpy(corpus[ncorpus].f, f, len);
		ncorpus++;
	}
	return 0;
}

/* ---- the frame being injected, for the crash report ---- */
static uint8_t cur_frame[2048];
static unsigned cur_len;
static unsigned long long cur_iter;

void __sanitizer_set_death_callback(void (*cb)(void)) __attribute__((weak));

static void save_crash(void)
{
	char p[1024];
	const char *dir = getenv("TESTNET_DIR");
	FILE *f;

	if (!cur_len)
		return;
	snprintf(p, sizeof(p), "%s/fuzz-crash-%llu-%llu.bin",
		 dir ? dir : "/tmp", g_opts.seed ? g_opts.seed : 1, cur_iter);
	f = fopen(p, "wb");
	if (f) {
		fwrite(cur_frame, cur_len, 1, f);
		fclose(f);
		fprintf(stderr, "test-net fuzz: crashing frame (%u bytes) of "
				"iteration %llu saved to %s\n",
			cur_len, cur_iter, p);
	}
	strcat(p, ".pcap");
	f = fopen(p, "wb");
	if (f) {
		static const uint32_t hdr[6] = { 0xa1b2c3d4, 0x00040002, 0, 0,
						 65535, 1 };
		uint32_t rec[4] = { 0, 0, cur_len, cur_len };
		fwrite(hdr, sizeof(hdr), 1, f);
		fwrite(rec, sizeof(rec), 1, f);
		fwrite(cur_frame, cur_len, 1, f);
		fclose(f);
	}
}

/* ---- mutations ---- */
static const uint32_t interesting[] = { 0,	  1,	  2,	  4,	  7,
					8,	  15,	  16,	  19,	  20,
					21,	  40,	  60,	  64,	  127,
					128,	  255,	  256,	  1460,	  1500,
					1514,	  0x7fff, 0x8000, 0xfffe, 0xffff };

static unsigned mutate(uint8_t *f, unsigned len, unsigned cap)
{
	int nmut = 1 + (int)rnd_below(4);

	for (int m = 0; m < nmut; m++) {
		unsigned r = rnd_below(100);
		if (len < 15)
			break;
		if (r < 30) { /* bit flips, mostly in the headers */
			unsigned span = rnd_below(3) ? (len < 74 ? len : 74) : len;
			for (unsigned k = 1 + rnd_below(8); k; k--) {
				unsigned b = 14 * 8 + rnd_below((span - 14) * 8);
				f[b / 8] ^= (uint8_t)(1u << (b % 8));
			}
		} else if (r < 45) { /* random bytes */
			for (unsigned k = 1 + rnd_below(4); k; k--)
				f[14 + rnd_below(len - 14)] = (uint8_t)rnd64();
		} else if (r < 65) { /* an interesting value in a header field */
			unsigned off = 14 + rnd_below(len - 14 < 60 ? len - 14 : 60);
			uint32_t v = interesting[rnd_below(
				sizeof(interesting) / sizeof(interesting[0]))];
			if (rnd_below(2) && off + 1 < len) {
				f[off] = (uint8_t)(v >> 8);
				f[off + 1] = (uint8_t)v;
			} else {
				f[off] = (uint8_t)v;
			}
		} else if (r < 72) { /* IP header fields that steer parsing */
			uint8_t *ip = f + 14;
			switch (rnd_below(5)) {
			case 0:
				ip[0] = (uint8_t)(0x40 | rnd_below(16));
				break;
			case 1:
				ip[2] = (uint8_t)rnd_below(256);
				ip[3] = (uint8_t)rnd64();
				break;
			case 2:
				ip[6] = (uint8_t)(rnd64() & 0x3f);
				ip[7] = (uint8_t)rnd64();
				break;
			case 3:
				ip[9] = (uint8_t)(rnd_below(2) ? rnd64() : 6);
				break;
			default:
				if (len > 34 + 13)
					f[34 + 12] = (uint8_t)(rnd_below(16) << 4);
				break;
			}
		} else if (r < 82) { /* truncate */
			len = 14 + rnd_below(len - 14);
		} else if (r < 90) { /* extend */
			unsigned add = 1 + rnd_below(200);
			if (len + add > cap)
				add = cap - len;
			for (unsigned k = 0; k < add; k++)
				f[len + k] = (uint8_t)rnd64();
			len += add;
		} else { /* splice the tail of another frame */
			const struct cframe *o = &corpus[rnd_below((unsigned)ncorpus)];
			unsigned at = 14 + rnd_below(len - 14);
			unsigned from = o->len > 14 ? 14 + rnd_below(o->len - 14) : 0;
			unsigned n = o->len - from;
			if (at + n > cap)
				n = cap - at;
			memcpy(f + at, o->f + from, n);
			len = at + n;
		}
	}
	return len;
}

/* ---- the corpus: real traffic towards instance 0 ---- */
static int build_corpus(int *ls_out)
{
	int c, s, ls;
	uint8_t buf[3000];

	wire_set_tap(record_tap, NULL);
	/* instance 1 connects to instance 0, both directions of data */
	if (tcp_pair(1, 0, 8080, &c, &s, &ls, 5 * SEC))
		return -1;
	if (transfer(1, c, 0, s, 1, 60000, 10 * SEC) != 60000 ||
	    transfer(0, s, 1, c, 2, 60000, 10 * SEC) != 60000)
		return -1;
	NS[1]->close(c);
	sim_run_for(SEC);
	NS[0]->close(s);
	sim_run_for(SEC);
	/* a connection to a closed port, an abort */
	int r = tcp_connect_nb(1, inst_ip(0), 9999);
	sim_run_for(SEC);
	NS[1]->close(r);
	int c2, s2;
	if (tcp_pair(1, 0, 8081, &c2, &s2, NULL, 5 * SEC))
		return -1;
	int lg[2] = { 1, 0 };
	NS[1]->setsockopt(c2, K_SOL_SOCKET, K_SO_LINGER, lg, sizeof(lg));
	NS[1]->close(c2);
	sim_run_for(SEC);
	NS[0]->close(s2);
	/* UDP, small and fragmented, and ICMP */
	int u = NS[1]->socket(K_SOCK_DGRAM);
	int u0 = NS[0]->socket(K_SOCK_DGRAM);
	NS[0]->bind(u0, 0, 5353);
	pat_fill(3, 0, buf, sizeof(buf));
	NS[1]->sendto(u, buf, 100, 0, inst_ip(0), 5353);
	NS[1]->sendto(u, buf, 3000, 0, inst_ip(0), 5353);
	NS[1]->sendto(u, buf, 50, 0, inst_ip(0), 7777);
	sim_run_for(100 * MS);
	NS[1]->ping(inst_ip(0), 1, 1, buf, 56);
	sim_run_for(100 * MS);
	wire_set_tap(NULL, NULL);
	*ls_out = ls;
	return ncorpus > 20 ? 0 : -1;
}

static int t_fuzz(void)
{
	unsigned long long iters = g_opts.iters ? g_opts.iters : 200000;
	double secs = g_opts.secs;
	int ls;
	struct timespec t0, t1;

	corpus = calloc(CORPUS_MAX, sizeof(*corpus));
	CHECK(build_corpus(&ls) == 0, "corpus (%d frames)", ncorpus);
	if (secs > 0)
		printf("    corpus: %d frames; running %.0f s\n", ncorpus, secs);
	else
		printf("    corpus: %d frames; %llu iterations\n", ncorpus, iters);
	fflush(stdout);
	if (__sanitizer_set_death_callback)
		__sanitizer_set_death_callback(save_crash);
	NS[0]->quiet(1);
	NS[1]->quiet(1);

	/* background traffic: instance 1 keeps a connection busy */
	int bc, bs;
	CHECK(tcp_pair(1, 0, 8090, &bc, &bs, NULL, 5 * SEC) == 0, "pair");
	uint8_t *bb = malloc(65536);
	unsigned char mac0[6];
	inst_mac(0, mac0);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	unsigned long long i;
	for (i = 0;; i++) {
		if (secs > 0) {
			if ((i & 255) == 0) {
				clock_gettime(CLOCK_MONOTONIC, &t1);
				if ((double)(t1.tv_sec - t0.tv_sec) +
					    (double)(t1.tv_nsec - t0.tv_nsec) / 1e9 >
				    secs)
					break;
			}
		} else if (i >= iters) {
			break;
		}
		const struct cframe *base = &corpus[rnd_below((unsigned)ncorpus)];
		memcpy(cur_frame, base->f, base->len);
		cur_len = mutate(cur_frame, base->len, sizeof(cur_frame) - 64);
		if (rnd_below(2))
			frame_fix_csums(cur_frame, cur_len);
		memcpy(cur_frame, mac0, 6); /* always to us */
		cur_iter = i;
		NS[0]->rx(cur_frame, cur_len);
		if ((i & 31) == 0) {
			/* time moves: timers, retransmissions, the peer */
			sim_run_for(5 * MS);
			pat_fill(9, 0, bb, 4000);
			NS[1]->send(bc, bb, 4000, 0);
			while (NS[0]->recv(bs, bb, 65536, 0) > 0)
				;
			int a;
			while ((a = NS[0]->accept(ls, NULL, NULL)) >= 0)
				NS[0]->close(a);
		}
	}
	cur_len = 0;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double el = (double)(t1.tv_sec - t0.tv_sec) +
		    (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("    %llu frames in %.1f s (%.0f/s), %llu kernel warnings\n", i,
	       el, (double)i / el, NS[0]->counter(NSC_WARNINGS));
	NS[0]->quiet(0);
	NS[1]->quiet(0);

	/* liveness: the instance still works */
	sim_run_for(2 * SEC);
	int c, s;
	CHECK(tcp_pair(1, 0, 8100, &c, &s, NULL, 10 * SEC) == 0,
	      "no new connection after fuzzing");
	CHECK(transfer(1, c, 0, s, 5, 200000, 60 * SEC) == 200000,
	      "data after fuzzing");
	CHECK(transfer(0, s, 1, c, 6, 200000, 60 * SEC) == 200000,
	      "data back after fuzzing");
	NS[1]->ping(inst_ip(0), 0x99, 1, bb, 32);
	CHECK(NS[1]->ping_reply(0x99, 1) == 1, "no ping reply after fuzzing");
	free(bb);
	return 0;
}

const struct test tests_fuzz[] = {
	{ .name = "fuzz-rx", .group = "fuzz", .fn = t_fuzz, .allow_warn = 1,
	  .limit = 100000 * SEC },
	{ 0 },
};
