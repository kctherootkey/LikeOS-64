/* The network harness's simulator and test API (host side; see
 * host/net/README).
 *
 * One process holds NINST complete stack instances (the real kernel/net
 * sources compiled once per instance with renamed symbols) and a
 * discrete-event simulator that drives them:
 *
 *   - one simulated clock (nanoseconds), moved only by the event loop;
 *   - a periodic timer tick per instance at the kernel's frequency;
 *   - a simulated wire between instance 0 and instance 1, one link per
 *     direction, each with a shared bottleneck (rate, drop-tail queue,
 *     MTU) and per-path impairments (delay, jitter, loss, Gilbert-Elliott
 *     burst loss, periodic loss windows, duplication, reordering, bit
 *     corruption); frames are classified into paths by a hook;
 *   - a pcap writer and a frame tap;
 *   - readiness notifications from the stacks' poll queues, delivered to
 *     an application callback after each event.
 *
 * Every random decision draws from one seeded generator: a run is a pure
 * function of its seed and configuration.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_SIM_H
#define HOST_NET_SIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "api-host.h"

#define NINST 2

/* kernel constants the tests use (kernel/net/net.h) */
#define K_SOCK_STREAM 1
#define K_SOCK_DGRAM 2
#define K_SOCK_RAW 3
#define K_SOL_SOCKET 1
#define K_IPPROTO_TCP 6
#define K_SO_ERROR 4
#define K_SO_KEEPALIVE 9
#define K_SO_LINGER 13
#define K_SO_RCVBUF 8
#define K_SO_SNDBUF 7
#define K_TCP_NODELAY 1
#define K_TCP_CORK 3
#define K_MSG_PEEK 0x02
#define K_MSG_OOB 0x01
#define K_MSG_DONTWAIT 0x40
#define K_MSG_TRUNC 0x20
#define K_SHUT_RD 0
#define K_SHUT_WR 1
#define K_SHUT_RDWR 2
#define K_POLLIN 0x0001
#define K_POLLPRI 0x0002
#define K_POLLOUT 0x0004
#define K_POLLERR 0x0008
#define K_POLLHUP 0x0010
#define K_FIONREAD 0x541B

enum { /* kernel TCP_STATE_* */
	TS_CLOSED,
	TS_LISTEN,
	TS_SYN_SENT,
	TS_SYN_RECEIVED,
	TS_ESTABLISHED,
	TS_FIN_WAIT_1,
	TS_FIN_WAIT_2,
	TS_CLOSE_WAIT,
	TS_CLOSING,
	TS_LAST_ACK,
	TS_TIME_WAIT,
};

extern const struct ns_ops *NS[NINST];
/* address of instance i: 10.0.0.(i+1)/24, MAC 02:00:00:00:00:(i+1) */
unsigned inst_ip(int i);
void inst_mac(int i, unsigned char mac[6]);

/* ---- clock and events ---- */
#define MS 1000000ULL
#define SEC 1000000000ULL
typedef unsigned long long simtime_t; /* ns */

simtime_t now_ns(void);
double now_s(void);
typedef void (*ev_fn)(void *arg, long a);
void sim_at(simtime_t t, ev_fn fn, void *arg, long a);
void sim_after(simtime_t dt, ev_fn fn, void *arg, long a);

/* Initialise the world: seed, tick frequency, and bring up the instances
 * (all of them, or only instance 0 for script mode). */
struct sim_opts {
	unsigned long long seed;
	unsigned hz; /* 0: 200 */
	unsigned mtu; /* both NICs; 0: 1500 */
	int ninst; /* 0: NINST */
	simtime_t limit; /* hard stop of simulated time; 0: 3600 s */
};
void sim_init(const struct sim_opts *o);

/* Run one event; 0 if none is left before the time limit. */
int sim_step(void);
/* Run all events up to absolute time t (the clock ends at t). */
void sim_run_until_time(simtime_t t);
void sim_run_for(simtime_t dt);
/* Run until cond(arg) is true or dt passed; returns 1 if it became true. */
int sim_run_cond(int (*cond)(void *), void *arg, simtime_t dt);

/* ---- the wire ---- */
#define MAX_PATHS 64
struct path_cfg {
	double delay_ms; /* one-way propagation delay */
	double jitter_ms; /* uniform extra delay 0..jitter (can reorder) */
	double rate_mbps; /* per-path rate before the bottleneck, 0 = none */
	unsigned queue_bytes; /* per-path queue limit, 0 = unlimited */
	double loss; /* independent loss probability */
	/* Gilbert-Elliott two-state loss: per-frame transition
	 * probabilities and the loss probability in the bad state */
	double ge_p_gb, ge_p_bg, ge_loss_bad;
	/* periodic total-loss windows: [k*period + offset, +len) */
	double burst_period_ms, burst_len_ms, burst_offset_ms;
	double dup; /* duplication probability */
	double reorder; /* probability a frame is held back reorder_ms */
	double reorder_ms;
	double corrupt; /* probability of one flipped bit (L3 and up) */
};

struct link_cfg {
	double rate_mbps; /* shared bottleneck, 0 = infinite */
	unsigned queue_bytes; /* drop-tail limit, 0 = unlimited */
	unsigned mtu; /* largest L3 packet carried, 0 = any */
	struct path_cfg path[MAX_PATHS];
	/* interrupt moderation of the receiving NIC: frames are handed to
	 * the stack in batches, the first of a batch at most this long
	 * after it arrived (GRO merges within a batch); 0 = one by one.
	 * TESTNET_RX_BATCH_US=A[,B] sets it for every test: A for link 1
	 * (instance 0 receives: the client of the benchmarks), B for link 0.
	 * A batch is processed at one instant (no CPU time passes), so the
	 * ACKs of a batch and everything they make the stack send share one
	 * timestamp -- far more ties than a real CPU produces. */
	double rx_batch_us;
	/* rewrite a frame entering the link (a middlebox: NAT, ...) */
	void (*rewrite)(uint8_t *frame, unsigned len, void *ctx);
	void *rewrite_ctx;
	/* frame -> path index (0..MAX_PATHS-1); NULL: everything path 0 */
	int (*classify)(const uint8_t *frame, unsigned len, void *ctx);
	void *classify_ctx;
};

struct link_stats {
	unsigned long long tx_frames, tx_bytes, delivered, drop_loss,
		drop_queue, drop_mtu, dup, reordered, corrupted;
};

/* link 0 carries instance 0 -> 1, link 1 carries 1 -> 0 */
struct link_cfg *link_cfg(int dir);
const struct link_stats *link_stats(int dir);
void link_set_both(const struct path_cfg *p); /* path 0 of both links */

/* Called for every frame an instance transmits (before the link).
 * Return 1 to consume it (it never reaches the link). */
typedef int (*tap_fn)(int inst, const uint8_t *frame, unsigned len,
		      void *ctx);
void wire_set_tap(tap_fn fn, void *ctx);
/* Detach instance `inst' from the wire: its frames go only to the tap. */
void wire_detach(int inst);

/* pcap capture (Ethernet link type) of every frame transmitted;
 * at_rx = 1 records delivered frames instead */
int pcap_open(const char *path, int at_rx);
void pcap_close(void);

/* ---- readiness notifications to the application ---- */
typedef void (*app_fn)(int inst, int sock, void *ctx);
void sim_set_app(app_fn fn, void *ctx);
unsigned long long sim_notifications(int inst);

/* ---- randomness (the simulator's generator) ---- */
void sim_reseed(uint64_t seed);
uint64_t rnd64(void);
double rnd01(void);
unsigned rnd_below(unsigned n);

/* ---- logging and checks ---- */
extern int g_verbose;
void klog_dump_tail(FILE *f, int lines);
void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_check_failed;
#define CHECK(c, ...)                                                    \
	do {                                                             \
		if (!(c)) {                                              \
			printf("    check failed at %s:%d: %s: ", __FILE__, \
			       __LINE__, #c);                            \
			printf(__VA_ARGS__);                             \
			printf("\n");                                    \
			g_check_failed = 1;                              \
			return 1;                                        \
		}                                                        \
	} while (0)

/* ---- helpers on top of ns_ops (testlib.c) ---- */
/* data pattern of stream `id' at byte offset `off' */
static inline uint8_t pat_byte(unsigned id, uint64_t off)
{
	uint64_t x = off * 0x9E3779B97F4A7C15ULL + id * 0x632BE59BD9B4E019ULL;
	return (uint8_t)((x >> 32) ^ (x >> 56));
}
void pat_fill(unsigned id, uint64_t off, uint8_t *buf, size_t n);
/* 0 if buf matches the pattern, else 1 + index of the first mismatch */
size_t pat_check(unsigned id, uint64_t off, const uint8_t *buf, size_t n);

/* a nonblocking TCP pair: listener on inst `srv' port, client on the
 * other; runs until both ends are established.  -1 on failure. */
int tcp_pair(int cli_inst, int srv_inst, unsigned port, int *cli, int *srv,
	     int *lsock, simtime_t timeout);
int tcp_connect_nb(int inst, unsigned ip, unsigned port);
int wait_established(int inst, int s, simtime_t timeout);
/* A one-way stream of pattern `id' from socket sa of instance a to socket
 * sb of instance b, driven by the stacks' readiness notifications.  A
 * safety scan every 10 ms counts progress nobody was notified about
 * (g_missed_wakeups). */
struct flow {
	int a, sa, b, sb;
	unsigned id;
	unsigned long long n, sent, got;
	int close_after; /* 1: shutdown(SHUT_WR) when sent, 2: close */
	int closed_a, bad, err, eof, done;
	simtime_t t_start, t_done, t_last;
};
void flow_init(struct flow *f, int a, int sa, int b, int sb, unsigned id,
	       unsigned long long n);
/* run until every flow is done or the timeout; returns the number that
 * delivered all their bytes intact */
int flows_run(struct flow *f, int n, simtime_t timeout);
extern unsigned long long g_missed_wakeups;
/* move `n' bytes of pattern `id' from (a, sa) to (b, sb), reading as fast
 * as possible; returns bytes verified, -1 on corruption */
long long transfer(int a, int sa, int b, int sb, unsigned id,
		   unsigned long long n, simtime_t timeout);
int so_error(int inst, int s);

/* Capture what instance `inst' transmits (consumed: it never reaches the
 * wire when `consume' is set).  cap_next pops the oldest frame; it runs
 * the simulation up to `wait' for one to appear.  Returns the length,
 * 0 if none. */
void cap_start(int inst, int consume);
void cap_stop(void);
unsigned cap_next(uint8_t *buf, unsigned cap, simtime_t wait,
		  simtime_t *when);
/* the next captured frame of IPv4 protocol `proto' (0x806: ARP),
 * skipping others */
unsigned cap_next_proto(int proto, uint8_t *buf, unsigned cap,
			simtime_t wait);
int cap_pending(void);
/* checksums of an Ethernet frame's IPv4 header and TCP/UDP/ICMP payload:
 * 0 all good, else a bitmask (1 IP, 2 L4) */
int frame_csum_bad(const uint8_t *f, unsigned len);
/* recompute the IPv4 header and TCP/UDP/ICMP checksums of a frame */
void frame_fix_csums(uint8_t *f, unsigned len);
uint16_t inet_csum(const void *data, size_t len, uint32_t seed);

#endif
