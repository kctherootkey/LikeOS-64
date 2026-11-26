// LikeOS — TCP internals shared by the TCP modules
//
//   tcp.c            sockets: allocation, references, connect/listen/
//                    accept/close, send and receive entry points, socket
//                    options, memory accounting, diagnostics
//   tcp_input.c      segment processing: the state machine, ACKs, RTT,
//                    congestion control hooks, data and out-of-order
//                    queueing, SACK generation, delayed ACKs, window and
//                    receive-buffer tuning
//   tcp_output.c     building and sending segments: options, window
//                    selection, the write queue, Nagle/cork/TSQ,
//                    retransmission, ACKs, probes, resets
//   tcp_timer.c      per-socket timers on the net_timer wheel
//   tcp_ipv4.c       hash tables and lookup, the receive entry, ISNs,
//                    SYN cookies, stateless resets and ACKs, ICMP glue
//   tcp_minisocks.c  request socks (SYN_RECEIVED) and TIME_WAIT socks
//   tcp_sack.c       the SACK scoreboard: received SACK blocks tag the
//                    retransmit queue, D-SACK, reneging
//   tcp_recovery.c   loss detection and recovery: RACK, TLP, the loss
//                    state machine (Open/Disorder/CWR/Recovery/Loss),
//                    PRR, F-RTO, undo (timestamps, D-SACK), NewReno
//   tcp_rate.c       delivery-rate sampling
//   tcp_cong.c       the congestion-control framework and Reno, cwnd
//                    validation, restart after idle, pacing rate
//   tcp_cubic.c      CUBIC with HyStart++ (the default)
//
// Locking
// -------
// Lock order (taken with interrupts disabled, outermost first):
//
//   listener->lock  ->  ehash bucket lock | bind-hash lock | listen-hash
//                       lock | reap lock | TSQ list lock | net_timer
//                       wheel lock | skb pool locks
//   conn->lock      ->  (the same leaves)
//   conn->lock      ->  tw->lock of the TIME_WAIT sock tcp_time_wait() is
//                       making (nobody else can see it yet) -> the leaves
//   tw->lock        ->  (the same leaves)
//
// Otherwise two socket locks are never held at once: a child created from
// a request sock is published (hash, accept queue) under the listener's
// lock while nobody else can see it yet, and is locked only after the
// listener's lock is dropped.  No lock is taken while a hash-bucket lock
// is held; lookups take a reference under the bucket lock and lock the
// socket afterwards.
//
// Publication: an object goes into the established hash only when it is
// complete -- bound (bind hash), its timer armed, every reference its
// holders will drop already taken -- because from that moment another CPU
// may find it and finish it (a reset, a connect() reusing the tuple).
// The driver is never entered under any of these locks (dev_queue_xmit
// defers while interrupts are off; tcp_lock_release() flushes).  Nothing
// sleeps and nothing calls the slab allocator with one of them held:
// connections, request and TIME_WAIT socks are allocated before and freed
// after (from the reap softirq), and skbs under a lock come from the
// pools only (skb.c).
//
// References
// ----------
// Every hash-table object (full, request, TIME_WAIT sock) is reference
// counted; the memory is freed from the reap list (SOFTIRQ_TIMER, process
// context with interrupts on) once the count reaches zero, never where the
// last reference is dropped.  References are held by:
//   * the hash tables, while the object is hashed (a full socket's "proto
//     ref", a request or TIME_WAIT sock's table ref);
//   * the owning socket (net_socket_t::tcp);
//   * each pending timer (taken when the timer is armed from idle, dropped
//     by the callback or by a successful del);
//   * an accept-queue entry (the listener holds its children);
//   * a request sock, on its listener (rsk->listener);
//   * a lookup, until the caller is done (get on lookup, put on release);
//   * the TSQ list, while a socket waits on it.
// A count that reached zero is never raised again (tcp_skc_tryhold).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_TCP_H
#define LIKEOS_TCP_H

#include <kernel/net/net.h>
#include <kernel/net/skb.h>
#include <kernel/net/net_timer.h>
#include <kernel/net/stats.h>
#include <kernel/net/ip.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// Tunables (the defaults of the common stack's sysctls)
// ============================================================================
#define TCP_RMEM_MIN 4096
#define TCP_RMEM_DEFAULT 131072
#define TCP_RMEM_MAX 6291456
#define TCP_WMEM_MIN 4096
#define TCP_WMEM_DEFAULT 16384
#define TCP_WMEM_MAX 4194304
/* Global memory of every TCP queue together (bytes of skb truesize):
 * below LOW nothing is limited, above PRESSURE the buffers stop growing
 * and windows are held, above HIGH new data is refused.  Chosen below
 * what the packet pools can hold at most (skb.h: the static pools plus
 * SKB_DYN_LIMIT, about 76 MB), so TCP backs off before the receive path
 * of the whole machine runs out of buffers. */
#define TCP_MEM_LOW (24u << 20)
#define TCP_MEM_PRESSURE (32u << 20)
#define TCP_MEM_HIGH (48u << 20)
#define TCP_MEM_RECLAIM_MS 100 // at most one global reclaim pass per
#define TCP_SOMAXCONN 4096
#define TCP_MAX_ORPHANS 16384
#define TCP_MAX_TW_BUCKETS 262144
#define TCP_INIT_CWND 10
#define TCP_MAX_WSCALE 14
#define TCP_MIN_MSS 88
#define TCP_MSS_DEFAULT 536 // RFC 1122: when the peer announces none
#define TCP_MAX_QUICKACKS 16
#define TCP_LIMIT_OUTPUT_BYTES (1u << 20) // TSQ ceiling per socket
#define TCP_CHALLENGE_ACK_LIMIT 1000 // per second, all sockets
#define TCP_REORDERING 3 // duplicate ACKs for a fast retransmit
#define TCP_MAX_REORDERING 300
#define TCP_RACK_RECOVERY_THRESH 16 // recoveries a grown reo_wnd persists
#define TCP_PACING_SS_RATIO 200 // percent of cwnd/srtt in slow start
#define TCP_PACING_CA_RATIO 120 // ... in congestion avoidance
#define TCP_BASE_MSS 1024 // the black-hole fallback's MSS
#define TCP_MIN_RTT_WLEN_MS 300000 // the windowed minimum RTT
#define TCP_INFINITE_SSTHRESH 0x7FFFFFFFu
#define TCP_CWND_CLAMP (1u << 20) // segments; keeps the rate math in range
/* Black-hole detection (path MTU probing): 0 = off, as by default; with
 * 1, repeated timeouts fall back to TCP_BASE_MSS. */
extern int tcp_sysctl_mtu_probing;
/* The ECN mode: 0 off, 1 request and accept, 2 accept only (default). */
extern int tcp_sysctl_ecn;
/* TCP's own pacing: 0 (default) only for connections that set
 * SO_MAX_PACING_RATE, 1 for every connection. */
extern int tcp_sysctl_internal_pacing;
static inline int tcp_needs_internal_pacing(const struct tcp_conn *c)
{
	return c->pacing_on || tcp_sysctl_internal_pacing;
}
#define TCP_SYN_RETRIES 6
#define TCP_SYNACK_RETRIES 5
#define TCP_RETRIES1 3 // timeouts before black-hole detection
#define TCP_RETRIES2 15 // data: give up after this many timeouts
#define TCP_ORPHAN_RETRIES 5 // a FIN alone at the head (closing states)
#define TCP_ORPHAN_RETRIES_ALIVE 8 // an orphan's window probes while alive
#define TCP_KEEPALIVE_TIME_S 7200
#define TCP_KEEPALIVE_INTVL_S 75
#define TCP_KEEPALIVE_PROBES 9
#define TCP_DEFAULT_SCALING_RATIO 128 // payload : truesize = 1 : 2
#define TCP_NOTSENT_LOWAT_DEFAULT 0 // off

// Durations, milliseconds (converted with net_ms_to_ticks / tcp_ms()).
#define TCP_TIMEWAIT_LEN_MS 60000 // 2 * MSL
#define TCP_FIN_TIMEOUT_MS 60000 // orphaned FIN_WAIT2
#define TCP_TIMEOUT_INIT_MS 1000 // first SYN / SYN-ACK retransmission
#define TCP_DELACK_MIN_MS 40
#define TCP_DELACK_MAX_MS 200
#define TCP_ATO_MIN_MS 40
#define TCP_LOCAL_DROP_RETRY_MS 20 // a transmit that failed locally
#define TCP_CORK_MS 200 // TCP_CORK holds a partial segment this long
/* A closing connection (FIN_WAIT_1, CLOSING, LAST_ACK) whose
 * retransmissions keep failing LOCALLY (no buffers) never counts a
 * timeout; it is failed after this long without progress. */
#define TCP_CLOSING_IDLE_MS 300000
#define TCP_SYNCOOKIE_PERIOD_MS 64000 // one cookie counter step
#define TCP_SYNCOOKIE_VALID_MS (2 * TCP_SYNCOOKIE_PERIOD_MS)
#define TCP_TW_REUSE_DELAY_MS 1000 // an ephemeral port's TIME_WAIT reuse
#define TCP_INVALID_RATELIMIT_MS 500 // ACKs to rejected segments
#define TCP_PAWS_24DAYS_S (60u * 60u * 24u * 24u)
#define TCP_PAWS_MSL_S 60

// Microseconds.
#define TCP_RTO_MIN_US 200000u
#define TCP_RTO_MAX_US 120000000u
#define TCP_RTO_INITIAL_US 1000000u

static inline uint64_t tcp_ms(uint64_t ms)
{
	return net_ms_to_ticks(ms);
}

static inline uint64_t tcp_now(void)
{
	return timer_ticks();
}

static inline uint64_t tcp_now_us(void)
{
	return timer_get_precise_us();
}

static inline uint64_t tcp_us_to_ticks(uint64_t us)
{
	uint64_t t = timer_us_to_ticks(us);

	return t ? t : 1;
}

static inline uint64_t tcp_ticks_to_ms(uint64_t ticks)
{
	return ticks * timer_us_per_tick() / 1000u;
}

// Sequence arithmetic (net.h has the functions; these are the usual names).
#define before(a, b) tcp_seq_before((a), (b))
#define after(a, b) tcp_seq_after((a), (b))
static inline int between(uint32_t seq, uint32_t lo, uint32_t hi)
{
	return tcp_seq_between(seq, lo, hi);
}

// ============================================================================
// The skb control block
// ============================================================================
// Scoreboard bits of a sent segment (tcp_skb_cb.sacked).
#define TCPCB_SACKED_ACKED 0x01 // the peer SACKed it
#define TCPCB_SACKED_RETRANS 0x02 // retransmitted, not yet ACKed
#define TCPCB_LOST 0x04 // presumed lost
#define TCPCB_EVER_RETRANS 0x08 // was retransmitted at some time
#define TCPCB_TAGBITS (TCPCB_SACKED_ACKED | TCPCB_SACKED_RETRANS | TCPCB_LOST)
#define TCPCB_RETRANS (TCPCB_SACKED_RETRANS | TCPCB_EVER_RETRANS)

// tcp_skb_cb.rxflags (received segments)
/* The data area was allocated by TCP (a collapsed buffer): its tailroom
 * may take more data.  A received frame's buffer is never appended to. */
#define TCPCB_RX_OWN_DATA 0x01

// tcp_skb_cb.txflags (sent segments)
#define TCPCB_TX_APP_LIMITED 0x01 // sent while application-limited

struct tcp_skb_cb {
	uint32_t seq; // first sequence number
	uint32_t end_seq; // seq + payload + SYN + FIN
	uint8_t tcp_flags; // TCP_* flags of the segment
	uint8_t sacked; // TCPCB_* (sent segments)
	uint8_t ip_dsfield; // received: the IP TOS byte (ECN field)
	uint8_t txflags; // sent: TCPCB_TX_*
	uint16_t segs; // received: segments merged into this one (GRO)
	uint16_t rxflags; // received: TCPCB_RX_*
	union {
		struct { // received
			uint32_t ack_seq; // the ACK field
			uint16_t window; // the raw window field
			uint16_t urg_ptr; // the raw urgent pointer
			uint32_t tsval; // TSval (when present)
			uint32_t tsecr;
		};
		struct { // sent: the delivery state at the transmission
			uint32_t delivered; // conn->delivered
			uint32_t delivered_us32; // conn->delivered_us, low bits
			uint32_t first_tx_us32; // conn->first_tx_us, low bits
			uint32_t lost; // conn->lost
		} tx;
	};
	uint64_t tx_us; // sent: time of the latest transmission
	uint64_t first_tx_us; // sent: time of the first transmission
};

#define TCP_SKB_CB(skb) ((struct tcp_skb_cb *)(skb)->cb)
BUILD_BUG_ON(sizeof(struct tcp_skb_cb) > SKB_CB_SIZE);

// Headroom a segment needs below its payload: TCP + IP + Ethernet.
#define TCP_MAX_HEADER (60 + 20 + 14 + 2)

// ============================================================================
// Option parsing
// ============================================================================
typedef struct {
	uint16_t mss; // 0 if absent
	int8_t wscale; // -1 if absent (else 0..14)
	uint8_t sack_perm;
	uint8_t ts_present;
	uint8_t sack_count;
	uint32_t tsval;
	uint32_t tsecr;
	struct tcp_sack_block sack[TCP_MAX_SACK_BLOCKS];
} tcp_parsed_opts_t;

void tcp_parse_options(const uint8_t *th, uint32_t doff, tcp_parsed_opts_t *out);

// ============================================================================
// Request socks and TIME_WAIT socks
// ============================================================================
struct tcp_request_sock {
	TCP_SKC_COMMON
	tcp_conn_t *listener; // held
	struct tcp_request_sock *rsk_next; // the listener's list
	net_timer_t timer; // SYN-ACK retransmission
	uint32_t iss; // our ISN (the SYN-ACK's seq)
	uint32_t irs; // the peer's ISN
	uint32_t rcv_wnd; // the window the SYN-ACK offered
	uint32_t window_clamp;
	uint32_t ts_recent;
	uint32_t ts_offset;
	uint64_t snt_synack_us; // first SYN-ACK (for the RTT sample)
	uint64_t last_oow_tick; // a retransmitted SYN was last answered
	uint16_t mss; // the peer's MSS option (clamped)
	uint16_t advmss; // the MSS our SYN-ACK announces
	uint8_t cookie; // answered with a SYN cookie: no state kept
	uint8_t snd_wscale;
	uint8_t rcv_wscale;
	uint8_t wscale_ok;
	uint8_t sack_ok;
	uint8_t ts_ok;
	uint8_t ecn_ok;
	uint8_t num_retrans; // SYN-ACKs resent
	uint8_t num_timeout; // timer expiries
	uint8_t acked; // a bare ACK arrived while TCP_DEFER_ACCEPT waits
	uint8_t completing; // a child is being made from it
	uint8_t dead; // unhashed; no longer usable
};

struct tcp_tw_sock {
	TCP_SKC_COMMON
	spinlock_t lock;
	net_timer_t timer;
	uint8_t substate; // TCP_STATE_TIME_WAIT or TCP_STATE_FIN_WAIT_2
	uint8_t rcv_wscale;
	uint8_t ts_enabled;
	uint8_t dead; // unhashed (killed or expired)
	uint32_t snd_nxt;
	uint32_t rcv_nxt;
	uint32_t rcv_wnd;
	uint32_t ts_recent;
	uint32_t ts_offset;
	uint64_t ts_recent_stamp; // tick
	uint64_t entry_stamp; // tick the sock was made (reuse delay)
	uint64_t last_oow_tick; // the rejected-segment ACK rate limit
};

// ============================================================================
// Congestion control hooks
// ============================================================================
#define TCP_CA_OPEN 0
#define TCP_CA_DISORDER 1
#define TCP_CA_CWR 2
#define TCP_CA_RECOVERY 3
#define TCP_CA_LOSS 4

#define CA_EVENT_TX_START 0 // first transmit when nothing was in flight
#define CA_EVENT_CWND_RESTART 1
#define CA_EVENT_COMPLETE_CWR 2
#define CA_EVENT_LOSS 3 // retransmission timeout

struct tcp_congestion_ops {
	const char *name;
	void (*init)(tcp_conn_t *c); // optional: the connection is up
	void (*release)(tcp_conn_t *c); // optional
	// the new ssthresh at a loss
	uint32_t (*ssthresh)(tcp_conn_t *c);
	// grow cwnd: `acked' segments newly acknowledged
	void (*cong_avoid)(tcp_conn_t *c, uint32_t ack, uint32_t acked);
	void (*set_state)(tcp_conn_t *c, uint8_t new_state); // optional
	void (*cwnd_event)(tcp_conn_t *c, int ev); // optional
	// an ACK acknowledged `acked' segments; rtt_us < 0: no sample
	void (*pkts_acked)(tcp_conn_t *c, uint32_t acked, int32_t rtt_us);
	uint32_t (*undo_cwnd)(tcp_conn_t *c); // the cwnd an undo restores
};
extern const struct tcp_congestion_ops tcp_reno;
extern const struct tcp_congestion_ops tcp_cubic;

// The flags of an ACK being processed (tcp_ack and its helpers).
#define FLAG_DATA 0x01 // the segment carried data
#define FLAG_WIN_UPDATE 0x02 // it updated the send window
#define FLAG_DATA_ACKED 0x04 // it acknowledged new data
#define FLAG_RETRANS_DATA_ACKED 0x08 // ... some of it retransmitted
#define FLAG_SYN_ACKED 0x10 // it acknowledged our SYN
#define FLAG_DATA_SACKED 0x20 // it SACKed new data
#define FLAG_ECE 0x40 // it carried ECE
#define FLAG_LOST_RETRANS 0x80 // a retransmission was lost again
#define FLAG_SLOWPATH 0x100 // full processing (window update rule)
#define FLAG_ORIG_SACK_ACKED 0x200 // never-retransmitted data (S)ACKed
#define FLAG_SND_UNA_ADVANCED 0x400 // snd_una moved
#define FLAG_DSACKING_ACK 0x800 // it carried a D-SACK
#define FLAG_SACK_RENEGING 0x2000 // the peer dropped SACKed data
#define FLAG_UPDATE_TS_RECENT 0x4000
#define FLAG_NO_CHALLENGE_ACK 0x8000
#define FLAG_ACK_MAYBE_DELAYED 0x10000 // the peer probably delayed it
#define FLAG_DSACK_TLP 0x20000 // a D-SACK of the tail loss probe
#define FLAG_SYNACK 0x40000 // the SYN-ACK of our connect: window unscaled
#define FLAG_SET_XMIT_TIMER 0x1000 // re-arm the TLP / RTO timer
#define FLAG_ACKED (FLAG_DATA_ACKED | FLAG_SYN_ACKED)
#define FLAG_NOT_DUP (FLAG_DATA | FLAG_WIN_UPDATE | FLAG_ACKED)
#define FLAG_CA_ALERT (FLAG_DATA_SACKED | FLAG_ECE | FLAG_DSACKING_ACK)
#define FLAG_FORWARD_PROGRESS (FLAG_ACKED | FLAG_DATA_SACKED)

// What an ACK asks the retransmission machinery to send.
#define REXMIT_NONE 0
#define REXMIT_LOST 1 // what is marked lost
#define REXMIT_NEW 2 // new data (F-RTO step 2b)

// What the scoreboard learned from one ACK's SACK blocks.
struct tcp_sacktag_state {
	uint64_t first_sackt; // send time of the first newly SACKed segment
	uint64_t last_sackt; // ... of the last
	uint32_t reord; // lowest never-retransmitted newly SACKed seq
	uint32_t sack_delivered; // segments newly SACKed (+ D-SACKed)
	int flag; // FLAG_*
	struct tcp_rate_sample *rate;
};

// One delivery-rate sample (tcp_rate.c).
struct tcp_rate_sample {
	uint64_t prior_mstamp; // starting timestamp of the interval
	uint32_t prior_delivered; // conn->delivered at prior_mstamp
	int32_t delivered; // segments delivered over the interval
	int64_t interval_us; // its length, -1: no sample
	uint32_t snd_interval_us;
	uint32_t rcv_interval_us;
	int64_t rtt_us; // RTT of the last (S)ACKed segment, -1: none
	int32_t losses; // segments marked lost by this ACK
	uint32_t acked_sacked; // segments (S)ACKed by this ACK
	uint32_t prior_in_flight;
	uint32_t last_end_seq;
	uint8_t is_app_limited;
	uint8_t is_retrans;
	uint8_t is_ack_delayed;
};

// conn->ecn_flags
#define TCP_ECN_OK 1 // negotiated
#define TCP_ECN_QUEUE_CWR 2 // the next new data carries CWR
#define TCP_ECN_DEMAND_CWR 4 // we echo ECE until the peer's CWR
#define TCP_ECN_SEEN 8 // an ECT or CE segment arrived
// The ECN field of the IP header (RFC 3168).
#define INET_ECN_NOT_ECT 0
#define INET_ECN_ECT_1 1
#define INET_ECN_ECT_0 2
#define INET_ECN_CE 3
#define INET_ECN_MASK 3

// ============================================================================
// Socket bits
// ============================================================================
// conn->nonagle
#define TCP_NAGLE_OFF 1 // TCP_NODELAY
#define TCP_NAGLE_CORK 2 // TCP_CORK
#define TCP_NAGLE_PUSH 4 // push this one now

// conn->ack_pending
#define TCP_ACK_SCHED 1 // an ACK is owed
#define TCP_ACK_TIMER 2 // the delayed-ACK timer runs
#define TCP_ACK_PUSHED 4 // a short (pushed) segment arrived
#define TCP_ACK_PUSHED2 8
#define TCP_ACK_NOW 16 // send the ACK at the end of this segment

// conn->pending_timer
#define TCP_TIME_RETRANS 1
#define TCP_TIME_PROBE0 3
#define TCP_TIME_LOSS_PROBE 5 // tail loss probe (TLP)
#define TCP_TIME_REO_TIMEOUT 6 // RACK reordering window ends

// conn->tsq_flags
#define TSQ_THROTTLED 0x1 // held back by the TSQ limit
#define TSQ_QUEUED 0x2 // on the TSQ list

// ============================================================================
// Extended counters (beyond the MIB of stats.h)
// ============================================================================
enum tcp_xmib {
	TCPX_DELAYEDACKS,
	TCPX_QUICKACKS,
	TCPX_HPHITS, // header prediction: data fast path
	TCPX_HPACKS, // header prediction: pure ACK fast path
	TCPX_PUREACKS,
	TCPX_OFOQUEUE,
	TCPX_OFODROP,
	TCPX_OFOMERGE,
	TCPX_DSACKSENT,
	TCPX_DSACKOFOSENT,
	TCPX_PRUNECALLED,
	TCPX_RCVPRUNED,
	TCPX_OFOPRUNED,
	TCPX_RCVCOLLAPSED,
	TCPX_RCVQDROP,
	TCPX_ZEROWINDOWDROP,
	TCPX_LISTENOVERFLOWS,
	TCPX_LISTENDROPS,
	TCPX_REQQFULLDOCOOKIES,
	TCPX_REQQFULLDROP,
	TCPX_SYNRETRANS,
	TCPX_ABORTONDATA,
	TCPX_ABORTONCLOSE,
	TCPX_ABORTONMEMORY,
	TCPX_ABORTONTIMEOUT,
	TCPX_ABORTONLINGER,
	TCPX_ABORTFAILED,
	TCPX_MEMORYPRESSURES,
	TCPX_MEMRECLAIMS, // global out-of-order reclaim passes
	TCPX_TWCREATED,
	TCPX_TWKILLED,
	TCPX_TWRECYCLED,
	TCPX_TWREUSED,
	TCPX_PAWSESTAB,
	TCPX_PAWSACTIVE,
	TCPX_CHALLENGEACK,
	TCPX_SYNCHALLENGE,
	TCPX_ACKSKIPPEDCHALLENGE,
	TCPX_ACKSKIPPEDSYNRECV, // a retransmitted SYN not answered (rate limit)
	TCPX_ACKSKIPPEDPAWS, // ... a PAWS-rejected segment
	TCPX_ACKSKIPPEDSEQ, // ... an out-of-window segment
	TCPX_ACKSKIPPEDFINWAIT2, // ... by a FIN_WAIT2 minisock
	TCPX_ACKSKIPPEDTIMEWAIT, // ... by a TIME_WAIT sock
	TCPX_DEFERACCEPTDROP,
	TCPX_TIMEOUTS,
	TCPX_FASTRETRANS,
	TCPX_SLOWSTARTRETRANS,
	TCPX_PARTIALRECOVERY,
	TCPX_TSQDEFER,
	TCPX_AUTOCORKING,
	TCPX_WINPROBE,
	TCPX_KEEPALIVE,
	TCPX_RCVBUFGROW,
	TCPX_SNDBUFGROW,
	TCPX_FINWAIT2TIMEOUT,
	TCPX_ORPHANS_KILLED,
	TCPX_MTUREDUCED,
	TCPX_SYNCOOKIESENT,
	TCPX_SYNCOOKIERECV,
	TCPX_SYNCOOKIEFAILED,
	TCPX_LOSSPROBES,
	TCPX_LOSSPROBERECOVERY,
	TCPX_SACKRECOVERY,
	TCPX_RENORECOVERY,
	TCPX_SACKRENEGING,
	TCPX_DSACKRECV,
	TCPX_DSACKOFORECV,
	TCPX_DSACKIGNOREDOLD,
	TCPX_DSACKIGNOREDNOUNDO,
	TCPX_DSACKIGNOREDDUBIOUS,
	TCPX_DSACKUNDO,
	TCPX_FULLUNDO,
	TCPX_LOSSUNDO,
	TCPX_SPURIOUSRTOS,
	TCPX_SACKREORDER,
	TCPX_TSREORDER,
	TCPX_RENOREORDER,
	TCPX_LOSTRETRANSMIT,
	TCPX_SACKDISCARD,
	TCPX_HYSTARTDELAYDETECT,
	TCPX_HYSTARTDELAYCWND,
	TCPX_HYSTARTCSSRESUME,
	TCPX_DELIVEREDCE,
	TCPX_ECNNEGOTIATED,
	TCPX_PACINGDEFER,
	TCPX_MTUPFAIL,
	TCPX_MAX
};
extern volatile uint64_t tcp_xmib[TCPX_MAX];
static inline void TCPX_INC(enum tcp_xmib i)
{
	__atomic_fetch_add(&tcp_xmib[i], 1, __ATOMIC_RELAXED);
}
static inline void TCPX_ADD(enum tcp_xmib i, uint64_t v)
{
	__atomic_fetch_add(&tcp_xmib[i], v, __ATOMIC_RELAXED);
}
// A counter by name (tests); ~0 when unknown.
uint64_t tcp_xmib_read(const char *name);

// ============================================================================
// tcp.c
// ============================================================================
// The interrupt-friendly socket lock (see tcp.c).
void tcp_lock_acquire(spinlock_t *lock, uint64_t *flags_out);
void tcp_lock_release(spinlock_t *lock, uint64_t flags);

// References on any hash-table object.
static inline void tcp_skc_hold(struct tcp_skc *skc)
{
	__atomic_fetch_add(&skc->refcount, 1, __ATOMIC_ACQ_REL);
}
int tcp_skc_tryhold(struct tcp_skc *skc); // 0 when it already hit zero
void tcp_skc_put(struct tcp_skc *skc); // frees from the reap softirq

static inline void tcp_conn_hold(tcp_conn_t *c)
{
	tcp_skc_hold((struct tcp_skc *)c);
}
static inline void tcp_conn_put(tcp_conn_t *c)
{
	if (c)
		tcp_skc_put((struct tcp_skc *)c);
}

tcp_conn_t *tcp_conn_alloc(void); // process/softirq context, IRQs on
void tcp_set_state(tcp_conn_t *c, int state);
void tcp_done(tcp_conn_t *c); // terminal: CLOSED, unhashed, timers off
void tcp_reset(tcp_conn_t *c, int err); // the peer reset it / it failed
void tcp_poll_notify(tcp_conn_t *c); // readiness changed (any event)
void tcp_poll_notify_key(tcp_conn_t *c, unsigned key); // these POLL* bits
void tcp_wake_reader(tcp_conn_t *c);
void tcp_listener_notify(tcp_conn_t *listener);
int tcp_orphan_too_many(void);
void tcp_orphan_inc(tcp_conn_t *c);
void tcp_orphan_dec(tcp_conn_t *c);
uint32_t tcp_orphan_count(void);
uint32_t tcp_full_sock_count(void); // full sockets in existence

// memory accounting
void tcp_mem_charge(int64_t bytes);
int tcp_under_memory_pressure(void);
int tcp_mem_over_limit(void); // above TCP_MEM_HIGH
void tcp_mem_reclaim_kick(void); // ask for a global out-of-order reclaim
void tcp_mem_reclaim(void); // the reap softirq: run it when asked
uint64_t tcp_mem_allocated(void);
void tcp_rmem_charge(tcp_conn_t *c, sk_buff_t *skb);
void tcp_rmem_uncharge(tcp_conn_t *c, sk_buff_t *skb);
void tcp_wmem_charge(tcp_conn_t *c, sk_buff_t *skb);
void tcp_wmem_uncharge(tcp_conn_t *c, sk_buff_t *skb);
int tcp_stream_memory_free(const tcp_conn_t *c);
void tcp_update_tx_ready(tcp_conn_t *c); // after the send queue shrank
void tcp_sndbuf_expand(tcp_conn_t *c);
void tcp_init_buffer_space(tcp_conn_t *c);
int tcp_space(const tcp_conn_t *c); // free receive buffer, as window
int tcp_full_space(const tcp_conn_t *c);
uint32_t tcp_win_from_space(const tcp_conn_t *c, int space);
int tcp_space_from_win(const tcp_conn_t *c, uint32_t win);
/* The initial receive window, its clamp and scale for a buffer that
 * offers `space' bytes of window (tcp.c). */
void tcp_select_initial_window(uint32_t space, uint32_t mss, uint32_t clamp,
			       int rcvbuf_locked, uint32_t *rcv_wnd,
			       uint32_t *window_clamp, uint8_t *rcv_wscale);
void tcp_purge_queues(tcp_conn_t *c); // all four queues, no lock needed

// ============================================================================
// tcp_ipv4.c
// ============================================================================
void tcp_hash_init(void);
/* ehash + bhash; -EADDRINUSE.  user_port: the application bound the local
 * port (a TIME_WAIT sock holding the tuple may then be reused at once). */
int tcp_hash_connect(tcp_conn_t *c, int user_port);
void tcp_hash_listener(tcp_conn_t *c);
void tcp_unhash(tcp_conn_t *c); // whatever tables it is on
int tcp_ehash_insert_req(struct tcp_request_sock *req); // 0, or -EEXIST
void tcp_ehash_remove(struct tcp_skc *skc);
/* Every full socket that holds out-of-order data, held and with no lock
 * taken, until fn returns nonzero.  Process/softirq context, interrupts
 * on, no lock held. */
void tcp_ehash_for_each_ofo(int (*fn)(tcp_conn_t *c, void *arg), void *arg);
/* Replace `old' by `newsk' in the established hash (a request sock by its
 * child, a full socket by its TIME_WAIT sock); 0 when `old' was still
 * hashed (the table's reference moves to newsk; the caller drops old's
 * table reference), -1 when old was gone. */
int tcp_ehash_replace(struct tcp_skc *old, struct tcp_skc *newsk);
void tcp_bhash_add(struct tcp_skc *skc);
void tcp_bhash_remove(struct tcp_skc *skc);
struct tcp_skc *tcp_lookup_established(uint32_t laddr, uint16_t lport,
				       uint32_t raddr, uint16_t rport);
tcp_conn_t *tcp_lookup_listener(uint32_t laddr, uint16_t lport);
uint32_t tcp_isn(uint32_t saddr, uint32_t daddr, uint16_t sport,
		 uint16_t dport);
uint32_t tcp_ts_offset_for(uint32_t saddr, uint32_t daddr, uint16_t sport,
			   uint16_t dport);
uint32_t tcp_ts_clock_ms(void);
static inline uint32_t tcp_ts_now(uint32_t offset)
{
	return tcp_ts_clock_ms() + offset;
}
// Stateless replies (no socket, or a request / TIME_WAIT sock).
void tcp_v4_send_reset(net_device_t *dev, uint32_t saddr, uint32_t daddr,
		       const uint8_t *th, uint32_t seg_len);
void tcp_v4_send_ack(uint32_t saddr, uint32_t daddr, uint16_t sport,
		     uint16_t dport, uint32_t seq, uint32_t ack, uint32_t win,
		     int ts, uint32_t tsval, uint32_t tsecr);
// SYN cookies.
uint32_t tcp_cookie_init_sequence(uint32_t saddr, uint32_t daddr,
				  uint16_t sport, uint16_t dport, uint32_t sseq,
				  uint16_t *mssp);
// 0 when invalid, else the MSS the cookie encodes.
uint16_t tcp_cookie_check(uint32_t saddr, uint32_t daddr, uint16_t sport,
			  uint16_t dport, uint32_t sseq, uint32_t cookie);
uint32_t tcp_cookie_init_timestamp(uint32_t ts_now, int wscale_ok,
				   uint8_t snd_wscale, int sack_ok, int ecn_ok);
// 0 when the echoed timestamp carries no valid option bits.
int tcp_cookie_decode_options(uint32_t tsecr, uint32_t ts_now, int *wscale_ok,
			      uint8_t *snd_wscale, int *sack_ok, int *ecn_ok);
int tcp_challenge_ack_allowed(void); // the global RFC 5961 budget

// ============================================================================
// tcp_minisocks.c
// ============================================================================
void tcp_minisocks_init(void);
void tcp_minisocks_maintain(void); // reap softirq: refill reserves
/* SYN to a listener, held by the caller; called without locks (it takes
 * listener->lock itself where it needs it).  0 or a reason to drop. */
int tcp_conn_request(tcp_conn_t *listener, sk_buff_t *skb,
		     const tcp_parsed_opts_t *opt, uint32_t saddr,
		     uint32_t daddr, net_device_t *dev);
/* A segment for a request sock (held by the caller).  Returns the new
 * child, held by the caller, with the segment still to be processed on
 * it; NULL when the segment was consumed here (answered, dropped). */
tcp_conn_t *tcp_check_req(struct tcp_request_sock *req, sk_buff_t *skb,
			  const tcp_parsed_opts_t *opt);
/* An ACK whose cookie is valid made a connection: the child, held, or
 * NULL -- with *reset set when the ACK must be answered with a reset (no
 * valid cookie), clear when it is just dropped (the accept queue is full:
 * the peer's next segment tries again). */
tcp_conn_t *tcp_cookie_child(tcp_conn_t *listener, sk_buff_t *skb,
			     const tcp_parsed_opts_t *opt, uint32_t saddr,
			     uint32_t daddr, net_device_t *dev, int *reset);
void tcp_reqsk_drop(struct tcp_request_sock *req); // unhash + put
void tcp_listen_stop(tcp_conn_t *listener); // under listener->lock
int tcp_synack_retransmit(struct tcp_request_sock *req);
void tcp_reqsk_free(struct tcp_request_sock *req); // reap
// TIME_WAIT.
void tcp_time_wait(tcp_conn_t *c, int substate, uint64_t timeo_ticks);
// A segment for a TIME_WAIT sock (held by the caller): 1 when a new SYN
// may open a connection through a listener, 0 when it was consumed.
int tcp_timewait_state_process(struct tcp_tw_sock *tw, sk_buff_t *skb,
			       const tcp_parsed_opts_t *opt);
// connect() to a 4-tuple a TIME_WAIT sock holds: 1 when it may be reused
// (the sock is claimed; c takes over its sequence space and timestamps),
// 0 when not.  user_port: the application bound the local port itself.
int tcp_twsk_unique(struct tcp_tw_sock *tw, int user_port, tcp_conn_t *c);
extern int tcp_sysctl_tw_reuse; // 0 never, 1 always, 2 loopback (default)
void tcp_tw_kill(struct tcp_tw_sock *tw);
void tcp_tw_free(struct tcp_tw_sock *tw); // reap
uint32_t tcp_tw_count(void);

// ============================================================================
// tcp_input.c
// ============================================================================
// Segment for a full socket; called with conn->lock held; consumes skb.
void tcp_rcv_state_process(tcp_conn_t *c, sk_buff_t *skb,
			   const tcp_parsed_opts_t *opt);
void tcp_init_transfer(tcp_conn_t *c); // the handshake completed
void tcp_rtt_estimator(tcp_conn_t *c, uint32_t r_us);
void tcp_set_rto(tcp_conn_t *c);
void tcp_rtt_sample_ts(tcp_conn_t *c, const tcp_parsed_opts_t *opt);
void tcp_enter_quickack_mode(tcp_conn_t *c, uint32_t max_quickacks);
void tcp_rcv_space_adjust(tcp_conn_t *c);
void tcp_cleanup_rbuf(tcp_conn_t *c, int copied);
int tcp_paws_reject_recent(uint32_t ts_recent, uint64_t ts_recent_stamp,
			   uint32_t tsval, int rst);
void tcp_ofo_purge(tcp_conn_t *c);
void tcp_dsack_set(tcp_conn_t *c, uint32_t seq, uint32_t end_seq);
void tcp_prune_ofo_queue(tcp_conn_t *c);
uint32_t tcp_ofo_reclaim(tcp_conn_t *c); // all of it; bytes freed

// ============================================================================
// tcp_output.c
// ============================================================================
uint32_t tcp_current_mss(tcp_conn_t *c);
uint32_t tcp_sync_mss(tcp_conn_t *c, uint32_t pmtu);
uint16_t tcp_select_window(tcp_conn_t *c);
uint32_t __tcp_select_window(tcp_conn_t *c);
int tcp_transmit_skb(tcp_conn_t *c, sk_buff_t *skb, int clone_it);
int tcp_write_xmit(tcp_conn_t *c, uint32_t mss_now, int nonagle, int push_one);
void tcp_push(tcp_conn_t *c, int nonagle, int more);
void tcp_push_pending_frames(tcp_conn_t *c);
void tcp_data_snd_check(tcp_conn_t *c);
void tcp_send_ack(tcp_conn_t *c);
void tcp_send_delayed_ack(tcp_conn_t *c);
void tcp_send_challenge_ack(tcp_conn_t *c);
/* The ACK owed to a segment that is rejected (out of window, PAWS, an old
 * duplicate) is sent at most once per TCP_INVALID_RATELIMIT_MS -- unless
 * the segment carries data (no ACK loop is made of data).  1: skip it.
 * `skb' may be NULL (a challenge ACK: always limited). */
int tcp_oow_rate_limited_stamp(uint64_t *last_tick, const sk_buff_t *skb,
			       enum tcp_xmib mib);
int tcp_oow_rate_limited(tcp_conn_t *c, const sk_buff_t *skb,
			 enum tcp_xmib mib);
void tcp_send_dupack(tcp_conn_t *c, sk_buff_t *skb);
int tcp_send_syn(tcp_conn_t *c); // connect(): the SYN
int tcp_send_synack(tcp_conn_t *c); // simultaneous open
int tcp_make_synack_req(struct tcp_request_sock *req, const sk_buff_t *syn);
void tcp_send_fin(tcp_conn_t *c);
void tcp_send_active_reset(tcp_conn_t *c);
int tcp_retransmit_skb(tcp_conn_t *c, sk_buff_t *skb);
// the same without the retransmission accounting (a TLP probe)
int __tcp_retransmit_skb(tcp_conn_t *c, sk_buff_t *skb);
void tcp_xmit_retransmit_queue(tcp_conn_t *c);
int tcp_pacing_check(tcp_conn_t *c); // 1: paced, the timer will send
void tcp_tsq_write(tcp_conn_t *c); // TSQ / pacing resumes the queues
uint32_t tcp_mtu_to_mss(const tcp_conn_t *c, uint32_t pmtu);
uint32_t tcp_mss_to_mtu(const tcp_conn_t *c, uint32_t mss);
void tcp_mtu_probing(tcp_conn_t *c); // repeated timeouts: black hole?
void tcp_send_probe0(tcp_conn_t *c);
int tcp_write_wakeup(tcp_conn_t *c);
int tcp_fragment(tcp_conn_t *c, skb_queue_t *q, sk_buff_t *skb, uint32_t len);
int tcp_trim_head(tcp_conn_t *c, sk_buff_t *skb, uint32_t len);
void tcp_mtu_reduced(tcp_conn_t *c, uint32_t mtu);
uint32_t tcp_options_size_established(const tcp_conn_t *c);
void tcp_tsq_init(void);
void tcp_check_probe_timer(tcp_conn_t *c);
uint32_t tcp_packets_in_flight(const tcp_conn_t *c);
static inline sk_buff_t *tcp_send_head(const tcp_conn_t *c)
{
	return skb_peek(&c->write_queue);
}

// ============================================================================
// tcp_cong.c
// ============================================================================
static inline int tcp_in_slow_start(const tcp_conn_t *c)
{
	return c->cwnd < c->ssthresh;
}
static inline int tcp_in_cwnd_reduction(const tcp_conn_t *c)
{
	return c->ca_state == TCP_CA_CWR || c->ca_state == TCP_CA_RECOVERY;
}
static inline int tcp_is_sack(const tcp_conn_t *c)
{
	return c->sack_ok != 0;
}
static inline int tcp_is_reno(const tcp_conn_t *c)
{
	return !tcp_is_sack(c);
}
void tcp_init_cwnd(tcp_conn_t *c);
void tcp_ca_init(tcp_conn_t *c); // the handshake completed
void tcp_ca_release(tcp_conn_t *c);
const struct tcp_congestion_ops *tcp_ca_default(void);
int tcp_set_congestion_control(tcp_conn_t *c, int id);
void tcp_set_ca_state(tcp_conn_t *c, uint8_t st);
void tcp_ca_event(tcp_conn_t *c, int ev);
// slow start: grow by `acked' up to ssthresh; returns what is left over
uint32_t tcp_slow_start(tcp_conn_t *c, uint32_t acked);
// additive increase: one segment per `w' segments acknowledged
void tcp_cong_avoid_ai(tcp_conn_t *c, uint32_t w, uint32_t acked);
int tcp_is_cwnd_limited(const tcp_conn_t *c);
uint32_t tcp_current_ssthresh(const tcp_conn_t *c);
void tcp_cwnd_validate(tcp_conn_t *c, int is_cwnd_limited);
void tcp_slow_start_after_idle_check(tcp_conn_t *c);
void tcp_update_pacing_rate(tcp_conn_t *c);
/* After an ACK: reduce the window (CWR, Recovery: PRR) or grow it. */
void tcp_cong_control(tcp_conn_t *c, uint32_t ack, uint32_t acked_sacked,
		      int flag, const struct tcp_rate_sample *rs);
uint32_t tcp_min_rtt(const tcp_conn_t *c); // us, ~0u when unknown
void tcp_update_rtt_min(tcp_conn_t *c, uint32_t rtt_us);
uint32_t tcp_cubic_root(uint64_t a); // integer cube root (tests)
void tcp_cubic_get_state(tcp_conn_t *c, uint32_t *wmax, uint32_t *K,
			 uint32_t *in_css);

// ============================================================================
// tcp_rate.c
// ============================================================================
void tcp_rate_skb_sent(tcp_conn_t *c, sk_buff_t *skb);
void tcp_rate_skb_delivered(tcp_conn_t *c, sk_buff_t *skb,
			    struct tcp_rate_sample *rs);
void tcp_rate_gen(tcp_conn_t *c, uint32_t delivered, uint32_t lost,
		  int is_sack_reneg, struct tcp_rate_sample *rs);
void tcp_rate_check_app_limited(tcp_conn_t *c);
uint64_t tcp_delivery_rate(const tcp_conn_t *c); // bytes per second

// ============================================================================
// tcp_sack.c
// ============================================================================
/* The peer's SACK blocks (opt) against the retransmit queue.  Returns
 * FLAG_* bits (FLAG_DATA_SACKED, FLAG_DSACKING_ACK, ...). */
int tcp_sacktag_write_queue(tcp_conn_t *c, const tcp_parsed_opts_t *opt,
			    uint32_t ack_seq, uint32_t prior_snd_una,
			    struct tcp_sacktag_state *st);
uint32_t tcp_highest_sack_seq(const tcp_conn_t *c);
void tcp_rtx_idx_add(tcp_conn_t *c, sk_buff_t *skb); // appended to rtx_queue
void tcp_rtx_idx_reset(tcp_conn_t *c); // the queue was emptied
void tcp_check_sack_reordering(tcp_conn_t *c, uint32_t low_seq, int ts);
// Non-SACK peers: duplicate ACKs stand in for SACKed segments.
void tcp_add_reno_sack(tcp_conn_t *c, int num_dupack, int ece_ack);
void tcp_remove_reno_sacks(tcp_conn_t *c, uint32_t acked, int ece_ack);
void tcp_reset_reno_sack(tcp_conn_t *c);
void tcp_count_delivered(tcp_conn_t *c, uint32_t delivered, int ece_ack);
// segments of `skb' at the current MSS (1 unless the path MTU shrank)
uint32_t tcp_skb_pcount(const tcp_conn_t *c, const sk_buff_t *skb);

// ============================================================================
// tcp_recovery.c
// ============================================================================
void tcp_mark_skb_lost(tcp_conn_t *c, sk_buff_t *skb);
// The time-ordered list of segments in flight (RACK).
void tcp_tsorted_move_tail(tcp_conn_t *c, sk_buff_t *skb); // (re)sent now
void tcp_tsorted_insert_after(tcp_conn_t *c, sk_buff_t *pos, sk_buff_t *skb);
void tcp_tsorted_del(tcp_conn_t *c, sk_buff_t *skb);
void tcp_tsorted_reset(tcp_conn_t *c); // every segment went
void tcp_rack_advance(tcp_conn_t *c, uint8_t sacked, uint32_t end_seq,
		      uint64_t xmit_us);
int tcp_rack_mark_lost(tcp_conn_t *c);
void tcp_rack_reo_timeout(tcp_conn_t *c);
void tcp_rack_update_reo_wnd(tcp_conn_t *c, const struct tcp_rate_sample *rs);
void tcp_fastretrans_alert(tcp_conn_t *c, uint32_t prior_snd_una,
			   int num_dupack, int *flag, int *rexmit);
void tcp_process_tlp_ack(tcp_conn_t *c, uint32_t ack, int flag);
void tcp_enter_loss(tcp_conn_t *c);
void tcp_enter_cwr(tcp_conn_t *c);
void tcp_init_cwnd_reduction(tcp_conn_t *c);
void tcp_end_cwnd_reduction(tcp_conn_t *c);
void tcp_cwnd_reduction(tcp_conn_t *c, int newly_acked_sacked, int newly_lost,
			int flag);
int tcp_schedule_loss_probe(tcp_conn_t *c, int advancing_rto);
void tcp_send_loss_probe(tcp_conn_t *c);
void tcp_rearm_rto(tcp_conn_t *c);
void tcp_set_xmit_timer(tcp_conn_t *c);
void tcp_xmit_recovery(tcp_conn_t *c, int rexmit);
void tcp_simple_retransmit(tcp_conn_t *c);
int tcp_any_retrans_done(const tcp_conn_t *c);
void tcp_clear_retrans_hints(tcp_conn_t *c);

// ============================================================================
// tcp_timer.c
// ============================================================================
void tcp_init_xmit_timers(tcp_conn_t *c);
void tcp_clear_xmit_timers(tcp_conn_t *c);
void tcp_reset_xmit_timer(tcp_conn_t *c, uint8_t what, uint64_t when_ticks);
void tcp_clear_xmit_timer(tcp_conn_t *c);
void tcp_reset_delack_timer(tcp_conn_t *c, uint64_t expires);
void tcp_clear_delack_timer(tcp_conn_t *c);
void tcp_reset_keepalive_timer(tcp_conn_t *c, uint64_t when_ticks);
void tcp_delete_keepalive_timer(tcp_conn_t *c);
void tcp_set_keepalive(tcp_conn_t *c, int on);
uint64_t tcp_rto_ticks(const tcp_conn_t *c); // RTO with backoff
uint64_t tcp_syn_rto_ticks(uint32_t sent);
uint64_t tcp_probe0_when(const tcp_conn_t *c);
void tcp_reset_pacing_timer(tcp_conn_t *c, uint64_t expires);
// net_timer helpers that keep the "pending timer holds a reference" rule.
void tcp_skc_timer_mod(struct tcp_skc *skc, net_timer_t *t, uint64_t expires);
void tcp_skc_timer_del(struct tcp_skc *skc, net_timer_t *t);

// ============================================================================
// Small helpers
// ============================================================================
static inline int tcp_state_synchronized(int st)
{
	return st == TCP_STATE_ESTABLISHED || st == TCP_STATE_FIN_WAIT_1 ||
	       st == TCP_STATE_FIN_WAIT_2 || st == TCP_STATE_CLOSE_WAIT ||
	       st == TCP_STATE_CLOSING || st == TCP_STATE_LAST_ACK;
}

static inline uint32_t tcp_receive_window(const tcp_conn_t *c)
{
	int32_t win = (int32_t)(c->rcv_wup + c->rcv_wnd - c->rcv_nxt);

	return win < 0 ? 0 : (uint32_t)win;
}

static inline int tcp_is_orphan(const tcp_conn_t *c)
{
	return c->owner_socket == NULL;
}

static inline uint32_t tcp_skb_len(const sk_buff_t *skb)
{
	return skb->len;
}

static inline int tcp_rtx_queue_empty(const tcp_conn_t *c)
{
	return skb_queue_empty(&c->rtx_queue);
}

static inline int tcp_write_queue_empty(const tcp_conn_t *c)
{
	return skb_queue_empty(&c->write_queue);
}

static inline uint32_t tcp_notsent_bytes(const tcp_conn_t *c)
{
	return c->write_seq - c->snd_nxt;
}

static inline void tcp_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint32_t tcp_get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t tcp_get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline void tcp_put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline uint32_t tcp_min32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static inline uint32_t tcp_max32(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

#endif // LIKEOS_TCP_H
