// LikeOS Network Subsystem Interface
// Network device abstraction, protocol dispatch, and socket API
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_NET_H_
#define _KERNEL_NET_H_

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h> // spinlock_t
#include <kernel/ke/waitq.h> // struct wait_queue_head, for the poll queues
#include <kernel/uapi/bug.h> // refcount_t
#include <kernel/ke/timer.h> // timer_ms_to_ticks, for the time constants
#include <kernel/net/netdevice.h> // the core's parts of struct net_device
#include <kernel/net/dst.h> // struct dst_cache, the TCP route cache

// ============================================================================
// Network Configuration
// ============================================================================
#define NET_MAX_DEVICES 4
#define NET_MTU_DEFAULT 1500
#define ETH_ALEN 6 // Ethernet address length
#define ETH_HLEN 14 // Ethernet header length
#define ETH_FRAME_MAX 1518 // Max Ethernet frame (MTU + header + FCS)
#define NET_RX_RING_SIZE 256
#define NET_TX_RING_SIZE 256
#define NET_RX_BUF_SIZE 4096

// ============================================================================
// Ethernet Constants
// ============================================================================
#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_IPV6 0x86DD

// Broadcast MAC address
extern const uint8_t eth_broadcast_addr[ETH_ALEN];

// ============================================================================
// IP Protocol Numbers
// ============================================================================
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

// ============================================================================
// Network Byte Order Helpers
// ============================================================================
static inline uint16_t net_htons(uint16_t x)
{
	return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint16_t net_ntohs(uint16_t x)
{
	return net_htons(x);
}

static inline uint32_t net_htonl(uint32_t x)
{
	return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) |
	       ((x << 8) & 0xFF0000) | ((x << 24) & 0xFF000000);
}

static inline uint32_t net_ntohl(uint32_t x)
{
	return net_htonl(x);
}

// ============================================================================
// IP Address Helpers
// ============================================================================
#define IP4_ADDR(a, b, c, d)                             \
	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
	 ((uint32_t)(c) << 8) | (uint32_t)(d))

// ============================================================================
// Network Device Abstraction
// ============================================================================
typedef struct net_device net_device_t;

typedef int (*net_send_fn)(net_device_t *dev, const uint8_t *data,
			   uint16_t len);
typedef int (*net_link_status_fn)(net_device_t *dev);
typedef void (*net_shutdown_fn)(net_device_t *dev);
typedef void (*net_watchdog_fn)(net_device_t *dev);

struct net_device {
	const char *name;
	uint8_t mac_addr[ETH_ALEN];
	uint16_t mtu;
	uint32_t ip_addr; // IPv4 address (host byte order)
	uint32_t netmask; // Subnet mask (host byte order)
	uint32_t gateway; // Default gateway (host byte order)
	uint32_t dns_server; // DNS server (host byte order)
	net_send_fn send;
	net_link_status_fn link_status;
	// Optional: called from acpi_poweroff() to quiesce the NIC before S5
	// (mask interrupts, clear bus-master, disable Wake-on-LAN).  Without
	// this, PME# / WUC / live bus-master DMA can block the chipset's S5
	// transition on real hardware (screen black, fans off, but power
	// rail still hot — must press the power button to fully shut down).
	net_shutdown_fn shutdown;
	/* Optional: periodic link supervision, called from the network
	 * housekeeping softirq on CPU 0 (interrupts enabled, may busy-wait
	 * briefly, must not sleep) about ten times a second, and promptly
	 * after the driver raises SOFTIRQ_NET_HOUSEKEEPING -- which is how
	 * a link-change interrupt hands its slow work out of hard-IRQ
	 * context.  See net_housekeeping_softirq(). */
	net_watchdog_fn watchdog;
	void *driver_data;
	spinlock_t lock;

	// Statistics
	uint64_t rx_packets;
	uint64_t tx_packets;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint64_t rx_errors;
	uint64_t tx_errors;
	uint64_t rx_dropped;

	/* ---- core (kernel/net/dev.c, netdevice.h) ----
	 * Zero is the default for every field below: drivers that do not
	 * know them keep working, net_register() fills in the defaults. */
	uint32_t flags; // IFF_UP (admin), IFF_RUNNING (carrier), ...
	uint32_t priv_flags; // IFF_NO_QUEUE ...
	uint64_t features; // NETIF_F_*
	uint32_t tx_queue_len; // 0: NETDEV_TX_QUEUE_LEN
	uint32_t watchdog_timeo_ms; // 0: NETDEV_WATCHDOG_MS
	int ifindex;
	volatile int core_ready; // dev_init_core() done
	/* Optional skb transmit: takes the frame (and frees it) and returns
	 * NETDEV_TX_OK, or NETDEV_TX_BUSY and leaves the skb to the caller.
	 * Without it the core calls send() with the frame's bytes. */
	int (*xmit)(sk_buff_t *skb, net_device_t *dev);
	// Optional: the watchdog saw the transmit queue stall (softirq).
	void (*tx_timeout)(net_device_t *dev);
	// Optional: the multicast list or IFF_PROMISC/ALLMULTI changed.
	void (*set_rx_mode)(net_device_t *dev);
	// Optional: fill in counters the driver keeps itself.
	void (*get_stats64)(net_device_t *dev, struct net_device_stats64 *s);
	struct netdev_queue txq;
	struct netdev_mc_list mc;
	volatile uint32_t link_events; // linkwatch: carrier changed
	uint32_t _core_pad;
	uint64_t tx_dropped;
	uint64_t multicast;
	uint64_t rx_backlog_drops;
	uint64_t rx_bad_src; // frames from a group (multicast) source address
};

// ============================================================================
// Ethernet Header
// ============================================================================
typedef struct __attribute__((packed)) {
	uint8_t dst[ETH_ALEN];
	uint8_t src[ETH_ALEN];
	uint16_t ethertype; // Network byte order
} eth_header_t;

// ============================================================================
// IPv4 Header
// ============================================================================
typedef struct __attribute__((packed)) {
	uint8_t version_ihl; // Version (4 bits) + IHL (4 bits)
	uint8_t tos;
	uint16_t total_length; // Network byte order
	uint16_t identification;
	uint16_t flags_fragment; // Flags (3 bits) + Fragment offset (13 bits)
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;
	uint32_t src_addr; // Network byte order
	uint32_t dst_addr; // Network byte order
} ipv4_header_t;

// ============================================================================
// ARP Header
// ============================================================================
typedef struct __attribute__((packed)) {
	uint16_t hw_type; // 1 = Ethernet
	uint16_t proto_type; // 0x0800 = IPv4
	uint8_t hw_len; // 6 for Ethernet
	uint8_t proto_len; // 4 for IPv4
	uint16_t opcode; // 1 = request, 2 = reply
	uint8_t sender_mac[ETH_ALEN];
	uint32_t sender_ip; // Network byte order
	uint8_t target_mac[ETH_ALEN];
	uint32_t target_ip; // Network byte order
} __attribute__((packed)) arp_header_t;

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define ARP_HW_ETHER 1

// ============================================================================
// ICMP Header
// ============================================================================
typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t code;
	uint16_t checksum;
	uint16_t identifier;
	uint16_t sequence;
} icmp_header_t;

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8
#define ICMP_DEST_UNREACH 3
#define ICMP_REDIRECT 5
#define ICMP_TIME_EXCEEDED 11
#define ICMP_PARAM_PROBLEM 12
#define ICMP_TIMESTAMP 13
#define ICMP_TIMESTAMP_REPLY 14
#define ICMP_ADDRMASK 17
#define ICMP_ADDRMASK_REPLY 18
#define ICMP_PORT_UNREACH 3 // Code for destination unreachable
#define ICMP_NET_UNREACH 0 // codes for DEST_UNREACH
#define ICMP_HOST_UNREACH 1
#define ICMP_PROTO_UNREACH 2
#define ICMP_FRAG_NEEDED 4
#define ICMP_SRC_QUENCH 4 // type, deprecated per RFC 6633

// PMTU cache (32-entry LRU)
#define PMTU_CACHE_SIZE 32
typedef struct {
	uint32_t dst_ip;
	uint16_t mtu;
	uint16_t pad;
	uint64_t last_use;
} pmtu_entry_t;

// ============================================================================
// UDP Header
// ============================================================================
typedef struct __attribute__((packed)) {
	uint16_t src_port; // Network byte order
	uint16_t dst_port; // Network byte order
	uint16_t length; // Network byte order
	uint16_t checksum; // Network byte order
} udp_header_t;

// ============================================================================
// TCP Header
// ============================================================================
typedef struct __attribute__((packed)) {
	uint16_t src_port;
	uint16_t dst_port;
	uint32_t seq_num;
	uint32_t ack_num;
	uint8_t data_offset; // Upper 4 bits = offset in 32-bit words
	uint8_t flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgent_ptr;
} tcp_header_t;

// TCP flags
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20
#define TCP_ECE 0x40 // ECN echo (RFC 3168)
#define TCP_CWR 0x80 // congestion window reduced

/* Sequence-number arithmetic (RFC 9293 §3.4).  Sequence numbers live on a
 * 2^32 circle, so "a comes before b" is the sign of the 32-bit DIFFERENCE,
 * never a plain unsigned comparison: `ack > snd_una' is false for every
 * ACK that crossed the wrap while snd_una had not, and a connection whose
 * sequence space wrapped (4 GB transferred, or an ISN near the top of the
 * range -- one in every few connections) stopped accepting ACKs at all.
 * Every comparison of two sequence numbers goes through these. */
static inline int tcp_seq_before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

static inline int tcp_seq_after(uint32_t a, uint32_t b)
{
	return tcp_seq_before(b, a);
}

// lo <= seq <= hi on the circle (hi must not be before lo).
static inline int tcp_seq_between(uint32_t seq, uint32_t lo, uint32_t hi)
{
	return hi - lo >= seq - lo;
}

// TCP states
#define TCP_STATE_CLOSED 0
#define TCP_STATE_LISTEN 1
#define TCP_STATE_SYN_SENT 2
#define TCP_STATE_SYN_RECEIVED 3
#define TCP_STATE_ESTABLISHED 4
#define TCP_STATE_FIN_WAIT_1 5
#define TCP_STATE_FIN_WAIT_2 6
#define TCP_STATE_CLOSE_WAIT 7
#define TCP_STATE_CLOSING 8
#define TCP_STATE_LAST_ACK 9
#define TCP_STATE_TIME_WAIT 10

// TCP configuration
// Connections are dynamically allocated; there is no fixed connection-table
// size — the number of live connections is bounded only by available memory
// (and ultimately by the number of open file descriptors).  The protocol's
// tunables (buffer sizes, timeouts, limits) are in kernel/net/tcp.h.
#define TCP_MSS 1460 // the MSS of a standard Ethernet path (MTU 1500)
/* The window a SYN advertises: 44 segments, 64240 bytes for the MSS above.
 * This is the one receive window that goes on the wire unscaled, and it is
 * what the most common stack puts there (its initial window is the 16-bit
 * maximum rounded down to a whole number of segments).  65535 in a SYN is
 * what its OWN fingerprint databases file under "not that system" -- see
 * tcp_options_write() for why the SYN must read as the system the
 * User-Agent names. */
#define TCP_SYN_WINDOW (44 * TCP_MSS)
/* Largest single MSG_PEEK copy through a bounce buffer (recvmsg with
 * several iovecs): a cap on that copy, not on the receive buffer. */
#define TCP_RX_BUF_MAX (2 * 1024 * 1024)

/* Every protocol duration is defined in MILLISECONDS and converted to
 * ticks where it is used, against the tick rate the machine actually runs
 * at.  They used to be raw tick counts written for a 100 Hz tick, and the
 * tick is calibrated at boot -- around 200 Hz on most machines -- so every
 * one of them expired in half the time it claimed. */
static inline uint64_t net_ms_to_ticks(uint64_t ms)
{
	uint64_t t = timer_ms_to_ticks(ms);

	return t ? t : 1; // a nonzero duration never collapses to "now"
}

#define TCP_MAX_OPTIONS 40

// TCP options
#define TCP_OPT_END 0
#define TCP_OPT_NOP 1
#define TCP_OPT_MSS 2
#define TCP_OPT_MSS_LEN 4
#define TCP_OPT_WSCALE 3
#define TCP_OPT_WSCALE_LEN 3
#define TCP_OPT_SACK_PERM 4
#define TCP_OPT_SACK_PERM_LEN 2
#define TCP_OPT_SACK 5 // variable length: 2 + 8*n
#define TCP_OPT_TIMESTAMP 8
#define TCP_OPT_TIMESTAMP_LEN 10

#define TCP_MAX_SACK_BLOCKS 4

// TCP-level sockopts (IPPROTO_TCP)
#define TCP_NODELAY 1
#define TCP_MAXSEG 2
#define TCP_CORK 3
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6
#define TCP_SYNCNT 7
#define TCP_LINGER2 8
#define TCP_DEFER_ACCEPT 9
#define TCP_WINDOW_CLAMP 10
#define TCP_INFO 11
#define TCP_QUICKACK 12
#define TCP_CONGESTION 13 // the congestion control by name ("cubic", "reno")
#define TCP_USER_TIMEOUT 18
#define TCP_NOTSENT_LOWAT 25
#define TCP_CA_NAME_MAX 16

// tcp_info option flag bits (returned in tcpi_options)
#define TCPI_OPT_TIMESTAMPS 1
#define TCPI_OPT_SACK 2
#define TCPI_OPT_WSCALE 4
#define TCPI_OPT_ECN 8
#define TCPI_OPT_ECN_SEEN 16 // an ECT or CE segment arrived

/* TCP_INFO: the layout of the common ABI (every field at its usual
 * offset; a field the stack keeps no state for stays zero).  getsockopt
 * copies min(optlen, sizeof) bytes, so older, shorter copies of the struct
 * keep working. */
struct tcp_info {
	uint8_t tcpi_state;
	uint8_t tcpi_ca_state;
	uint8_t tcpi_retransmits;
	uint8_t tcpi_probes;
	uint8_t tcpi_backoff;
	uint8_t tcpi_options;
	uint8_t tcpi_snd_wscale_rcv_wscale; // low nibble snd, high nibble rcv
	uint8_t tcpi_pad0; // bit 0: delivery_rate_app_limited
	uint32_t tcpi_rto; // microseconds
	uint32_t tcpi_ato;
	uint32_t tcpi_snd_mss;
	uint32_t tcpi_rcv_mss;
	uint32_t tcpi_unacked;
	uint32_t tcpi_sacked;
	uint32_t tcpi_lost;
	uint32_t tcpi_retrans;
	uint32_t tcpi_fackets;
	uint32_t tcpi_last_data_sent;
	uint32_t tcpi_last_ack_sent;
	uint32_t tcpi_last_data_recv;
	uint32_t tcpi_last_ack_recv;
	uint32_t tcpi_pmtu;
	uint32_t tcpi_rcv_ssthresh;
	uint32_t tcpi_rtt; // microseconds (smoothed)
	uint32_t tcpi_rttvar; // microseconds
	uint32_t tcpi_snd_ssthresh;
	uint32_t tcpi_snd_cwnd; // segments
	uint32_t tcpi_advmss;
	uint32_t tcpi_reordering;
	uint32_t tcpi_rcv_rtt;
	uint32_t tcpi_rcv_space;
	uint32_t tcpi_total_retrans;
	uint64_t tcpi_pacing_rate; // bytes per second
	uint64_t tcpi_max_pacing_rate;
	uint64_t tcpi_bytes_acked;
	uint64_t tcpi_bytes_received;
	uint32_t tcpi_segs_out;
	uint32_t tcpi_segs_in;
	uint32_t tcpi_notsent_bytes;
	uint32_t tcpi_min_rtt; // microseconds
	uint32_t tcpi_data_segs_in;
	uint32_t tcpi_data_segs_out;
	uint64_t tcpi_delivery_rate; // bytes per second
	uint64_t tcpi_busy_time;
	uint64_t tcpi_rwnd_limited;
	uint64_t tcpi_sndbuf_limited;
	uint32_t tcpi_delivered;
	uint32_t tcpi_delivered_ce;
	uint64_t tcpi_bytes_sent;
	uint64_t tcpi_bytes_retrans;
	uint32_t tcpi_dsack_dups;
	uint32_t tcpi_reord_seen;
	uint32_t tcpi_rcv_ooopack;
	uint32_t tcpi_snd_wnd; // the peer's window, scaled (bytes)
	uint32_t tcpi_rcv_wnd; // ours, scaled (bytes)
	uint32_t tcpi_rehash;
	uint16_t tcpi_total_rto;
	uint16_t tcpi_total_rto_recoveries;
	uint32_t tcpi_total_rto_time; // milliseconds
};

/* ---------------------------------------------------------------------------
 * TCP sockets
 *
 * Three kinds of object stand for a TCP endpoint, and all three sit in the
 * same hash tables (kernel/net/tcp_ipv4.c):
 *   * tcp_conn_t, the full socket: a connection, or a listener;
 *   * the request sock, a half-open passive connection (SYN received,
 *     SYN-ACK sent) -- a few dozen bytes, no buffers;
 *   * the TIME_WAIT sock, what is left of a connection after it closed:
 *     the 4-tuple, the sequence numbers and a timer.
 * They begin with the same fields (TCP_SKC_COMMON), so a lookup returns a
 * struct tcp_skc and the receive path dispatches on skc_kind.  The
 * protocol internals (request and TIME_WAIT socks, the skb control block,
 * the tunables, the lock order and the refcount rules) are in
 * kernel/net/tcp.h.
 * ------------------------------------------------------------------------- */
#define TCP_SKC_FULL 1
#define TCP_SKC_REQ 2
#define TCP_SKC_TW 3

#define TCP_RTX_IDX_N 24 // entries of tcp_conn_t::rtx_idx
struct tcp_skc;

#define TCP_SKC_COMMON                                                       \
	int state; /* TCP_STATE_* */                                         \
	uint32_t local_ip; /* host order */                                  \
	uint32_t remote_ip;                                                  \
	uint16_t local_port;                                                 \
	uint16_t remote_port;                                                \
	volatile int refcount;                                               \
	uint8_t skc_kind; /* TCP_SKC_* */                                    \
	uint8_t skc_hashed; /* in the established or listening hash */       \
	uint8_t skc_bound; /* in the bind hash */                            \
	uint8_t skc_reaping; /* queued for the final free */                 \
	struct tcp_skc *skc_next; /* hash chain */                           \
	struct tcp_skc *skc_bind_next; /* bind hash chain */                 \
	struct tcp_skc *skc_reap_next;                                       \
	net_device_t *dev;

struct tcp_skc {
	TCP_SKC_COMMON
};

struct tcp_congestion_ops;
struct tcp_request_sock;

// TCP connection block (the full socket)
typedef struct tcp_conn {
	TCP_SKC_COMMON

	/* Everything below is under `lock' unless noted.  The lock is taken
	 * with interrupts disabled (tcp_lock_acquire), from process context
	 * and from the softirqs alike. */
	spinlock_t lock;
	/* The net_socket_t that owns this connection (socket.c sets it at
	 * attach and clears it at close).  The self-pointer sentinel means
	 * "the tcp layer owns it, no socket yet" (a child on the accept
	 * queue); NULL means "orphaned": closed by the application, still
	 * finishing its protocol (FIN_WAIT, LAST_ACK, ...). */
	void *owner_socket;
	int active; // created and not yet freed (socket.c re-validates it)
	int error; // pending hard error (ECONNRESET, ETIMEDOUT, ...)
	/* An ICMP error that did not abort the connection (RFC 1122
	 * 4.2.3.9 / RFC 5927): reported through SO_ERROR only when no hard
	 * error is pending, and the error a retransmission timeout fails
	 * the connection with in place of a bare ETIMEDOUT. */
	int soft_error;
	uint8_t active_open; // opened by connect(), not by a listener
	uint8_t proto_ref; // the protocol's own reference is held
	uint8_t orphan_counted; // counted in the global orphan count
	uint8_t userlocks; // TCP_SNDBUF_LOCK | TCP_RCVBUF_LOCK

	// Readiness, read without the lock by the socket layer and poll.
	volatile int rx_ready; // unread data in the receive queue
	volatile int tx_ready; // room in the send buffer
	volatile int accept_ready; // a child waits on the accept queue
	volatile int connect_done; // the handshake ended (well or not)

	// ---- sequence space (RFC 9293 3.3.1) ----
	uint32_t iss;
	uint32_t irs;
	uint32_t snd_una; // oldest unacknowledged
	uint32_t snd_nxt; // next to send
	uint32_t snd_wnd; // peer's window (scaled)
	uint32_t snd_wl1; // segment seq of the last window update
	uint32_t max_window; // largest window the peer offered
	uint32_t write_seq; // end of the data queued by the application
	uint32_t pushed_seq; // last byte marked PSH
	uint32_t snd_up; // urgent pointer: just past the urgent data
	uint32_t rcv_nxt; // next expected
	uint32_t rcv_wnd; // window we advertised last (bytes)
	/* rcv_nxt as carried by the last ACK we sent (RFC 7323 Last.ACK.sent;
	 * the right edge we promised is rcv_wup + rcv_wnd). */
	uint32_t rcv_wup;
	uint32_t copied_seq; // next byte the application reads
	/* The urgent byte's sequence number (the received urgent pointer
	 * points just past it), valid while urg_state is not TCP_URG_NONE */
	uint32_t rcv_up;
	uint32_t rcv_ssthresh; // current window clamp (grows with data)
	uint32_t window_clamp; // largest window we will ever offer
	uint32_t rcv_tsecr; // TSecr of the segment being processed
	uint32_t rcv_rtt_last_tsecr; // last TSecr the receiver RTT used
	uint32_t syn_tsval; // TSval of our first SYN (PAWS on the SYN-ACK)
	uint32_t snd_sml; // end of the last small segment sent (Nagle)
	uint32_t pred_flags; // header prediction possible

	// ---- negotiated options ----
	uint8_t ws_enabled;
	uint8_t snd_wscale; // shift of the peer's window
	uint8_t rcv_wscale; // shift of ours
	uint8_t sack_ok;
	uint8_t ts_enabled;
	uint8_t ecn_flags; // TCP_ECN_* (tcp.h): negotiated, CWR owed, ...
	uint8_t nonagle; // TCP_NAGLE_* bits (tcp.h)
	uint8_t cork; // TCP_CORK
	uint16_t peer_mss; // the MSS the peer announced (536 if none)
	uint16_t mss_clamp; // peer_mss clamped by TCP_MAXSEG
	uint16_t mss_cache; // payload of a full-sized segment now
	uint16_t advmss; // the MSS we announced
	uint16_t user_mss; // TCP_MAXSEG, 0 = none
	uint16_t tcp_header_len; // 20 + timestamps
	uint32_t pmtu_cookie; // path MTU mss_cache was computed for

	// ---- queues (skbs, data at the payload; cb is struct tcp_skb_cb) ----
	skb_queue_t write_queue; // queued, not yet sent
	skb_queue_t rtx_queue; // sent, not yet acknowledged (seq order)
	skb_queue_t rcv_queue; // in order, not yet read
	struct rb_root ooo_queue; // out of order, by sequence
	struct sk_buff *ooo_last_skb; // most recently added to ooo_queue
	uint32_t rcvq_bytes; // unread bytes on rcv_queue

	// ---- memory (bytes, skb truesize) ----
	int rcvbuf; // SO_RCVBUF (auto-tuned unless locked)
	int sndbuf; // SO_SNDBUF (auto-tuned unless locked)
	uint32_t rmem_alloc; // rcv_queue + ooo_queue
	uint32_t wmem_queued; // write_queue + rtx_queue
	volatile uint32_t wmem_tx; // segments in the device layer (TSQ), atomic
	uint32_t notsent_lowat; // TCP_NOTSENT_LOWAT, 0 = off
	uint8_t scaling_ratio; // payload / truesize of received skbs, /256
	uint8_t tsq_flags; // TSQ_* (tcp.h), atomic
	uint8_t mem_pressure_seen;
	uint8_t _pad_mem;

	// ---- SACK ----
	uint8_t num_sacks; // blocks in selective_acks
	uint8_t dsack; // duplicate_sack is to be sent
	uint8_t _pad_sack[2];
	/* selective_acks / duplicate_sack: what we report; recv_sack_cache:
	 * the peer's blocks the scoreboard already processed (sorted). */
	struct tcp_sack_block {
		uint32_t start_seq;
		uint32_t end_seq;
	} selective_acks[TCP_MAX_SACK_BLOCKS], duplicate_sack[1],
		recv_sack_cache[TCP_MAX_SACK_BLOCKS];

	// ---- timestamps (RFC 7323) ----
	uint32_t ts_recent; // the TSval we echo
	uint64_t ts_recent_stamp; // tick ts_recent was taken (PAWS aging)
	uint32_t ts_offset; // our TSval clock offset for this 4-tuple

	// ---- RTT / RTO (RFC 6298), microseconds ----
	uint32_t srtt_us;
	uint32_t rttvar_us;
	uint32_t rto_us;
	uint8_t rto_backoff; // exponential backoff exponent
	uint8_t retransmits; // consecutive timeouts of the head
	uint8_t probes_out; // unanswered window / keepalive probes
	uint8_t pending_timer; // TCP_TIME_RETRANS / TCP_TIME_PROBE0
	uint64_t retrans_stamp; // tick of the first unanswered retransmission
	/* tick of the first zero-window probe of this closed-window episode
	 * (0: none) -- answered probes do not reset it, an opened window does */
	uint64_t probes_tstamp;

	// ---- congestion control (segments) ----
	const struct tcp_congestion_ops *ca_ops;
	uint64_t ca_priv[12]; // the congestion control's own state
	uint32_t cwnd;
	uint32_t ssthresh;
	uint32_t cwnd_cnt; // congestion avoidance: ACKs toward +1
	uint32_t prior_cwnd;
	uint32_t prior_ssthresh; // to restore on undo (0: no undo)
	uint32_t high_seq; // snd_nxt when Recovery / CWR / Loss began
	uint8_t ca_state; // TCP_CA_* (tcp.h)
	uint8_t ca_setsockopt; // TCP_CONGESTION chose ca_ops
	uint8_t is_cwnd_limited; // this window was limited by cwnd
	uint8_t frto; // F-RTO (RFC 5682) after this timeout
	uint32_t max_packets_out; // largest flight in this window
	uint32_t cwnd_usage_seq; // ... the window ends here
	uint32_t snd_cwnd_used; // application-limited: flight used
	uint64_t snd_cwnd_stamp; // tick cwnd was last validated
	uint32_t packets_out; // segments on rtx_queue
	uint32_t retrans_out; // ... of them retransmitted, not yet ACKed
	uint32_t sacked_out; // ... SACKed (scoreboard)
	uint32_t lost_out; // ... marked lost (scoreboard)
	uint32_t total_retrans;
	uint64_t lsndtime; // tick of the last data sent

	// ---- loss detection and recovery (tcp_sack.c, tcp_recovery.c) ----
	uint32_t undo_marker; // snd_una when the reduction began, 0: no undo
	int32_t undo_retrans; // retransmissions not proven spurious yet
	uint32_t retrans_ts; // TSval of the episode's first retransmission
	uint32_t reordering; // reordering degree (segments)
	uint32_t reord_seen; // reordering events
	uint32_t fack_seq; // end of the highest SACKed segment
	uint32_t prr_delivered; // PRR (RFC 6937): delivered in recovery
	uint32_t prr_out; // ... and sent
	uint32_t tlp_high_seq; // snd_nxt at the tail loss probe, 0: none
	uint8_t tlp_retrans; // the probe was a retransmission
	uint8_t is_sack_reneg; // the peer dropped SACKed data
	uint8_t mtup_enabled; // black-hole fallback active
	uint8_t _pad_rec;
	uint32_t mtup_search_low; // ... its MTU
	struct sk_buff *retransmit_skb_hint; // where the lost walk resumes
	/* The segments in flight that may still be lost, by (last) send
	 * time: neither SACKed nor marked lost by RACK (skb->ts_next/prev,
	 * tcp_recovery.c).  RACK walks only these, oldest first. */
	struct sk_buff *tsorted_head;
	struct sk_buff *tsorted_tail;
	/* A sparse index of the retransmit queue by sequence (tcp_sack.c):
	 * every 2^rtx_idx_shift-th segment sent, oldest first, so the SACK
	 * scoreboard finds a block's segments without walking the queue
	 * from an end.  An entry is valid while its seq is not below
	 * snd_una (only acknowledged segments are freed). */
	struct {
		uint32_t seq;
		struct sk_buff *skb;
	} rtx_idx[TCP_RTX_IDX_N];
	uint8_t rtx_idx_n;
	uint8_t rtx_idx_shift;
	uint16_t rtx_idx_count;
	uint32_t lost; // segments ever marked lost
	uint32_t delivered; // segments delivered (ACKed or SACKed)
	uint32_t delivered_ce; // ... with a CE echo
	uint32_t dsack_dups; // segments the peer reported twice
	struct {
		uint64_t mstamp; // send time of the latest (S)ACKed segment
		uint32_t rtt_us; // its RTT
		uint32_t end_seq; // its end
		uint32_t last_delivered; // reo_wnd adapted at this delivered
		uint8_t reo_wnd_steps; // reordering window, in min_rtt/4
		uint8_t reo_wnd_persist; // recoveries to keep it
		uint8_t dsack_seen; // a D-SACK since the last adaptation
		uint8_t advanced; // mstamp moved: look for losses
	} rack;
	struct {
		uint32_t t; // ms
		uint32_t v; // us
	} min_rtt[3]; // windowed minimum RTT (best, 2nd, 3rd)

	// ---- delivery rate (tcp_rate.c), pacing ----
	uint64_t first_tx_us; // start of the current send flight
	uint64_t delivered_us; // time of the latest delivery
	uint32_t app_limited; // delivered count where app-limited ends, 0: not
	uint32_t rate_delivered; // the last rate sample: segments ...
	uint64_t rate_interval_us; // ... over this interval
	uint8_t rate_app_limited; // ... while application-limited
	/* TCP paces this connection itself (SO_MAX_PACING_RATE was set, or
	 * tcp_sysctl_internal_pacing); otherwise pacing_rate only sizes TSQ */
	uint8_t pacing_on;
	uint16_t inet_id; // the IP ID of this connection's next datagram
	uint64_t pacing_rate; // bytes per second, 0: not paced (yet)
	uint64_t max_pacing_rate; // SO_MAX_PACING_RATE cap, ~0: none
	uint64_t wstamp_ns; // earliest departure of the next segment
	uint32_t received_ce; // CE marks received
	uint32_t rcv_ooopack; // segments that arrived out of order
	uint32_t segs_in, segs_out, data_segs_in, data_segs_out;
	uint64_t bytes_acked, bytes_received, bytes_sent, bytes_retrans;
	uint16_t total_rto, total_rto_recoveries;
	uint32_t total_rto_time; // ms
	uint64_t rto_stamp; // tick the current RTO recovery began, 0: none

	// ---- timers (net_timer wheel; a pending timer holds a reference) ----
	net_timer_t retransmit_timer; // RTO, zero-window probe, handshake
	net_timer_t delack_timer;
	net_timer_t keepalive_timer; // keepalive; orphaned FIN_WAIT2
	net_timer_t pacing_timer; // the next paced transmission
	uint64_t retransmit_expires; // tick the retransmit timer is due

	// ---- ACK scheduling (delayed ACK / quick ACK) ----
	uint8_t ack_pending; // TCP_ACK_* bits
	uint8_t ack_quick; // ACKs still to send at once
	uint8_t ack_pingpong; // interactive: delay ACKs up to the max
	uint8_t ack_retry;
	uint32_t ack_ato; // ACK timeout estimate, ticks
	uint64_t ack_timeout; // tick the delayed ACK is due
	uint64_t ack_lrcvtime; // tick the last data segment arrived
	uint32_t rcv_mss; // estimate of the peer's segment size
	uint32_t last_seg_size;

	// ---- receive-buffer auto-tuning ----
	struct {
		uint32_t rtt_us; // receiver's RTT estimate (scaled << 3)
		uint32_t seq;
		uint64_t time_us;
	} rcv_rtt_est;
	struct {
		uint32_t space; // bytes copied in the last RTT
		uint32_t seq;
		uint64_t time_us;
	} rcvq_space;

	// ---- socket options ----
	int nodelay; // TCP_NODELAY
	int keepalive; // SO_KEEPALIVE
	uint32_t keepidle_ticks;
	uint32_t keepintvl_ticks;
	uint32_t keepcnt;
	uint32_t user_timeout_ms; // TCP_USER_TIMEOUT, 0 = off
	uint32_t defer_accept; // TCP_DEFER_ACCEPT, SYN-ACK retransmissions
	int linger2_ms; // TCP_LINGER2, 0 = default, < 0 = no FIN_WAIT2 wait
	uint8_t syn_retries; // TCP_SYNCNT, 0 = default
	uint8_t urg_state; // TCP_URG_* of the latest urgent pointer
	uint8_t urgent_byte; // the urgent byte (TCP_URG_VALID / _READ)
	uint8_t urg_inline; // SO_OOBINLINE: the urgent byte stays in band
	/* A reader / a writer is copying between the queues and user memory
	 * with the lock dropped (one at a time, as one call's data must stay
	 * together in the stream; under `lock'). */
	uint8_t rx_busy;
	uint8_t tx_busy;
	uint8_t snd_urg_pending; // urgent mode: snd_up is in effect
	uint8_t fin_pending; // our FIN is owed (no buffer for it yet)
	uint64_t last_oow_ack_tick; // out-of-window ACK rate limit
	uint64_t last_rx_tick; // tick the last segment arrived
	uint64_t cork_deadline;

	// ---- listener ----
	struct tcp_conn *accept_head; // children ready for accept()
	struct tcp_conn *accept_tail;
	struct tcp_conn *accept_next; // child: link on the listener's queue
	uint32_t ack_backlog; // children on the accept queue
	uint32_t max_ack_backlog; // listen() backlog (<= somaxconn)
	uint32_t reqsk_qlen; // request socks of this listener
	uint64_t synq_overflow_tick; // last SYN answered with a cookie
	struct tcp_request_sock *reqsk_list; // ... chained through rsk_next
	int backlog;

	// ---- route ----
	struct dst_cache dst_cache;

	// ---- deferred transmission (TSQ) ----
	struct tcp_conn *tsq_next; // on the TSQ list
} tcp_conn_t;

// ============================================================================
// DHCP Structures
// ============================================================================
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_DECLINE 4
#define DHCP_ACK 5
#define DHCP_NAK 6
#define DHCP_RELEASE 7
#define DHCP_INFORM 8

typedef struct __attribute__((packed)) {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
	uint32_t magic_cookie;
	uint8_t options[312];
} dhcp_packet_t;

// ============================================================================
// Socket Abstraction (kernel-side)
// ============================================================================
#define SOCK_STREAM 1 // TCP
#define SOCK_DGRAM 2 // UDP
#define SOCK_RAW 3 // Raw IP
/* Connection-oriented, record-preserving; AF_UNIX only.  The value matches
 * the userspace header (sys/socket.h). */
#define SOCK_SEQPACKET 5

#define AF_INET 2
#define PF_INET AF_INET
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define PF_UNIX AF_UNIX

#define SOL_SOCKET 1
#define SOL_IP 0
#define SOL_TCP 6
#define SOL_UDP 17
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ACCEPTCONN 30 // read-only: reports the listening state
#define SO_ERROR 4
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER 13
#define SO_REUSEPORT 15
#define SO_PEERCRED 17
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_BINDTODEVICE 25
#define SO_PRIORITY 12
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_TIMESTAMP 29
#define SO_TIMESTAMPNS 35
#define SO_MARK 36
#define SO_PROTOCOL 38
#define SO_DOMAIN 39
#define SO_PEEK_OFF 42
#define SO_MAX_PACING_RATE 47 // bytes/s; setting it turns pacing on
/* Ancillary data of SOL_SOCKET: the receive time of a datagram. */
#define SCM_TIMESTAMP SO_TIMESTAMP
#define SCM_TIMESTAMPNS SO_TIMESTAMPNS

/* What SO_PEERCRED returns: who is on the other end of a UNIX socket.
 *
 * Recorded by the KERNEL when the connection was made, not supplied by either
 * side, which is the whole value of it -- a peer cannot claim to be somebody
 * else.  That is what makes it an authentication mechanism, and it is how
 * D-Bus's EXTERNAL auth decides whether a client may talk to the bus.
 *
 * The three fields and their order are the ones every other system uses, so a
 * program's own struct matches this one. */
struct ucred {
	int32_t pid;
	uint32_t uid;
	uint32_t gid;
};

// IP-level sockopts (SOL_IP)
#define IP_TOS 1
#define IP_TTL 2
#define IP_HDRINCL 3
#define IP_MULTICAST_IF 32
#define IP_MULTICAST_TTL 33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36
#define IP_PKTINFO 8
#define IP_RECVTTL 12
#define IP_RECVTOS 13
#define IP_MTU_DISCOVER 10
#define IP_RECVERR 11
#define IP_MTU 14
#define IP_OPTIONS 4
#define IP_MULTICAST_ALL 49

// cmsghdr (RFC 2292)
struct cmsghdr {
	uint32_t cmsg_len;
	int cmsg_level;
	int cmsg_type;
};
#define CMSG_ALIGN(len) (((len) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define CMSG_DATA(cmsg) \
	((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_FIRSTHDR(mhdr)                                 \
	((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
		 (struct cmsghdr *)(mhdr)->msg_control :    \
		 (struct cmsghdr *)0)
#define CMSG_NXTHDR(mhdr, cmsg)                                            \
	(((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) +         \
		  sizeof(struct cmsghdr) >                                 \
	  (unsigned char *)(mhdr)->msg_control + (mhdr)->msg_controllen) ? \
		 (struct cmsghdr *)0 :                                     \
		 (struct cmsghdr *)((unsigned char *)(cmsg) +              \
				    CMSG_ALIGN((cmsg)->cmsg_len)))

struct linger {
	int l_onoff;
	int l_linger;
};

struct in_addr_fwd {
	uint32_t s_addr;
}; /* internal forward */

struct ip_mreq {
	struct in_addr_fwd imr_multiaddr; // multicast group address
	struct in_addr_fwd imr_interface; // local interface address (any = 0)
};

struct in_pktinfo {
	int ipi_ifindex;
	struct in_addr_fwd ipi_spec_dst;
	struct in_addr_fwd ipi_addr;
};

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_RAW 255

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define INADDR_ANY 0
#define INADDR_BROADCAST 0xFFFFFFFF

// recv/send flags
#define MSG_PEEK 0x02
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL 0x100
#define MSG_TRUNC 0x20
#define MSG_CTRUNC 0x08
#define MSG_OOB 0x01
#define MSG_NOSIGNAL 0x4000
#define MSG_ERRQUEUE 0x2000
#define MSG_MORE 0x8000
#define MSG_EOR 0x80
#define MSG_WAITFORONE 0x10000 /* recvmmsg: block for the first only */

/* SCM_RIGHTS — passing of open file descriptors via sendmsg/recvmsg
 * SOL_SOCKET-level ancillary data.  Userspace defines the same value
 * in <sys/socket.h>; mirrored here so kernel-internal consumers
 * (sock_sendmsg / sock_recvmsg) don't need to pull in libc headers. */
#define SCM_RIGHTS 0x01

/* 8x the old 128: with the UDP queue out of line a slot is a few hundred
 * bytes, so the whole table costs less than ONE slot used to. */
#define NET_MAX_SOCKETS 1024

// Socket address structures
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;
typedef uint32_t in_addr_t;

struct in_addr {
	in_addr_t s_addr; // Network byte order
};

struct sockaddr {
	sa_family_t sa_family;
	char sa_data[14];
};

struct sockaddr_in {
	sa_family_t sin_family; // AF_INET
	uint16_t sin_port; // Network byte order
	struct in_addr sin_addr;
	uint8_t sin_zero[8];
};

// Bits of net_socket_t::tcp_opt_set
#define SOCK_TCPOPT_KEEPALIVE 0x01
#define SOCK_TCPOPT_NODELAY 0x02
#define SOCK_TCPOPT_CORK 0x04
#define SOCK_TCPOPT_KEEPIDLE 0x08
#define SOCK_TCPOPT_KEEPINTVL 0x10
#define SOCK_TCPOPT_KEEPCNT 0x20

// Kernel socket structure
typedef struct net_socket {
	int type; // SOCK_STREAM, SOCK_DGRAM, SOCK_RAW
	int protocol;
	int domain; // AF_INET
	int bound; // Has been bound to an address
	int listening; // Is in listen state
	int connected; // Is connected (TCP) or has default dest (UDP)
	int closed; // Socket has been shut down
	/* shutdown(2) direction flags.  These are OUR end's declarations, per
	 * direction, and neither detaches the connection: a half-closed TCP
	 * socket keeps receiving after SHUT_WR (FIN_WAIT_1/2 carry data), and
	 * keeps sending after SHUT_RD.  See sock_shutdown() for why treating
	 * shutdown() as close() broke every caller that half-closes. */
	int tx_shutdown; // SHUT_WR applied: sends fail with EPIPE
	int rx_shutdown; // SHUT_RD applied: reads drain the ring, then EOF
	int nonblock; // O_NONBLOCK flag
	int error; // Pending error

	struct sockaddr_in local_addr;
	struct sockaddr_in remote_addr;

	// For TCP sockets
	tcp_conn_t *tcp;

	/* Datagram sockets (UDP, raw): what arrived, as skbs (see
	 * kernel/net/datagram.c), and the errors reported to the socket
	 * (IP_RECVERR).  Both are charged by truesize to rx_mem, which is
	 * bounded by rcvbuf_size.  Under s->lock. */
	skb_queue_t rx_queue;
	skb_queue_t err_queue;
	uint32_t rx_mem;
	uint32_t rx_drops; // datagrams refused: receive buffer full
	/* Readiness of a datagram socket: something queued or an error
	 * pending.  Receivers park on its address. */
	volatile int udp_rx_ready;
	/* Lookup tables (udp.c: by port and by local address + port; raw.c:
	 * by protocol).  `hashed' stays set until the socket is off every
	 * table; sock_create() does not reuse such a slot. */
	struct net_socket *hnext;
	struct net_socket *h2next;
	volatile uint8_t hashed;
	uint8_t udp_userlocks; // bind() named the address (1) / port (2)
	uint8_t udp_cork; // UDP_CORK
	uint8_t pmtudisc; // IP_MTU_DISCOVER mode
	uint8_t ip_recverr; // IP_RECVERR
	uint8_t mc_all; // IP_MULTICAST_ALL
	uint16_t inet_id; // IP identification of a connected socket
	uint32_t udp_saddr; // source address to send from (0: route's)
	int bound_ifindex; // SO_BINDTODEVICE, 0: any
	struct udp_cork_state *cork; // pending corked datagram (udp.c)
	volatile int tx_owner; // a sender owns the cork

	// Socket options
	int reuse_addr;
	int reuse_port;
	/* Effective UID of the process that bound this socket's local port.
	 * Set at every bind (explicit or implicit ephemeral).  Gates
	 * SO_REUSEPORT group membership: a second bind may share the port
	 * only with the same euid (or a privileged caller) — otherwise any
	 * user could join another user's reuseport group and hijack a share
	 * of its incoming connections. */
	uint32_t bind_euid;
	int broadcast;
	int oobinline;
	int sndbuf_size;
	int rcvbuf_size;
	int linger_onoff;
	int linger_seconds;
	char bindtodevice[16];
	uint64_t rcv_timeout_ticks; // 0 = infinite
	/* SO_SNDTIMEO: how long a blocking send() or connect() waits.  0 =
	 * infinite.  connect() used to take SO_RCVTIMEO's value instead. */
	uint64_t snd_timeout_ticks;
	/* SO_RCVLOWAT: bytes a TCP socket needs queued to poll readable
	 * (0 means 1).  SO_MARK / SO_PRIORITY: stored, reported. */
	int rcvlowat;
	uint32_t sk_mark;
	int sk_priority;
	/* SO_MAX_PACING_RATE (bytes per second, ~0 = none): applied to the
	 * TCP connection like the TCP options below (pacing_set: it was set,
	 * which turns TCP's own pacing on for the connection). */
	uint64_t max_pacing_rate;
	uint8_t pacing_set;
	/* SO_TIMESTAMP (1) / SO_TIMESTAMPNS (2): datagrams carry their
	 * receive time as ancillary data. */
	uint8_t rcv_tstamp;
	/* MSG_MORE on the send in progress: more data follows at once, so
	 * TCP may hold a partial segment back (as TCP_CORK does). */
	volatile uint8_t tx_more;

	/* TCP options set on the SOCKET.  They are what getsockopt()
	 * reports, and they are applied to the connection when it is created
	 * -- by connect(), listen(), or accept(), which also copies the
	 * listening socket's values to the accepted one, as every caller
	 * expects.  They used to go straight to s->tcp and were silently
	 * dropped when set before connect() or listen(), which is when most
	 * programs set them.  tcp_opt_set says which were set explicitly;
	 * the others keep the connection's defaults. */
	uint8_t tcp_opt_set; // SOCK_TCPOPT_* bits
	uint8_t tcp_opt_keepalive;
	uint8_t tcp_opt_nodelay;
	uint8_t tcp_opt_cork;
	uint32_t tcp_opt_keepidle_ticks;
	uint32_t tcp_opt_keepintvl_ticks;
	uint32_t tcp_opt_keepcnt;
	/* SO_SNDBUF / SO_RCVBUF set by the application on a stream socket
	 * (TCP_SNDBUF_LOCK / TCP_RCVBUF_LOCK): that buffer is not auto-tuned. */
	uint8_t sk_userlocks;
	/* Further IPPROTO_TCP options the connection keeps (TCP_MAXSEG,
	 * TCP_USER_TIMEOUT, TCP_NOTSENT_LOWAT, TCP_DEFER_ACCEPT, ...), set
	 * before the connection exists: applied when it is made. */
#define SOCK_TCP_XOPTS 10
	uint8_t tcp_xopt_n;
	int tcp_xopt_name[SOCK_TCP_XOPTS];
	int tcp_xopt_val[SOCK_TCP_XOPTS];

	// IP-level options
	uint8_t ip_ttl;
	uint8_t ip_tos;
	uint8_t ip_hdrincl;
	uint8_t ip_recvpktinfo;
	uint8_t ip_recvttl;
	uint8_t ip_recvtos;
	uint8_t mcast_ttl;
	uint8_t mcast_loop;
	uint32_t mcast_if;
#define NET_MAX_MCAST_GROUPS 8
	uint32_t mcast_groups
		[NET_MAX_MCAST_GROUPS]; // host byte order; 0 = empty slot
	net_device_t *mcast_devs[NET_MAX_MCAST_GROUPS]; // joined on

	spinlock_t lock;
	int active;
	int ref_count;
	/* Who is polling THIS socket.
	 *
	 * A readiness change wakes this queue and nothing else, so the cost of
	 * an arriving segment is proportional to the number of tasks waiting
	 * on this connection rather than to the number of tasks on the
	 * machine.  See <kernel/ke/waitq.h> for what that replaced. */
	struct wait_queue_head poll_wq;
} net_socket_t;

// ============================================================================
// Network Device Registry API
// ============================================================================
void net_init(void);
void net_start_dhcp(void);
int net_register(net_device_t *dev);
net_device_t *net_get_device(int index);
net_device_t *net_get_default_device(void);
int net_device_count(void);

// Called from acpi_poweroff() before the S5 transition.  Walks the device
// registry and invokes each NIC's optional shutdown() callback.
void net_quiesce_for_poweroff(void);

// ============================================================================
// Ethernet Layer API
// ============================================================================
int eth_send(net_device_t *dev, const uint8_t dst_mac[ETH_ALEN],
	     uint16_t ethertype, const uint8_t *payload, uint16_t len);
void eth_rx(net_device_t *dev, const uint8_t *frame, uint16_t len);
// A received frame as an skb (data at the Ethernet header).  Consumes it.
struct sk_buff;
int eth_rcv(struct sk_buff *skb);

// ============================================================================
// ARP API
// ============================================================================
void arp_init(void);
int arp_resolve(net_device_t *dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]);
int arp_cache_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]);
// Forward decl; full type in <kernel/net/skb.h>
struct sk_buff;
// Queue an IPv4 datagram skb pending ARP resolution of next_hop_ip. Takes
// ownership of skb on success (returns 0).  Returns -1 if the pending pool
// is full; caller still owns skb in that case and must drop it via skb_put.
int arp_queue_pending(net_device_t *dev, uint32_t next_hop_ip,
		      struct sk_buff *skb);
void arp_rx(net_device_t *dev, const uint8_t *data, uint16_t len);
void arp_request(net_device_t *dev, uint32_t target_ip);
void arp_add_entry(uint32_t ip, const uint8_t mac[ETH_ALEN]);
// RFC 5227 ARP probe (DAD): sends 3 probes ~200ms apart with sender_ip=0,
// returns 0 if no collision detected, -1 if a reply for `ip` was seen.
int arp_probe(net_device_t *dev, uint32_t ip);
// Send a gratuitous ARP reply to announce the given IP on the LAN.
void arp_gratuitous(net_device_t *dev, uint32_t ip);
void arp_del_entry(uint32_t ip);
// An administrative entry: PERMANENT, or STALE when !permanent.
int arp_set_entry(uint32_t ip, const uint8_t mac[ETH_ALEN], int permanent);
// Receive with the link-layer packet type (PACKET_*, skb.h).
void arp_rcv(net_device_t *dev, const uint8_t *data, uint16_t len,
	     uint8_t pkt_type);
uint64_t arp_get_conflicts(void);
// Send an IPv4 datagram (skb->data at the IP header) to the on-link next
// hop through the neighbour table.  Consumes the skb.  0 or -1 (dropped).
int ip_neigh_output(net_device_t *dev, uint32_t nexthop, struct sk_buff *skb);
// The next hop was confirmed reachable by an upper layer.
void ip_neigh_confirm(net_device_t *dev, uint32_t nexthop);

// ============================================================================
// IGMPv2 (RFC 2236) — minimal: send membership reports on join, leaves on drop
// ============================================================================
void igmp_init(void);
int igmp_join(net_device_t *dev, uint32_t group); // group in host byte order
int igmp_leave(net_device_t *dev, uint32_t group);
void igmp_rx(net_device_t *dev, uint32_t src_ip, const uint8_t *data,
	     uint16_t len);
// Is `group' (host order) joined on `dev'?
int igmp_is_member(net_device_t *dev, uint32_t group);

// ============================================================================
// IPv4 API
// ============================================================================
void ipv4_init(void);
int ipv4_send(net_device_t *dev, uint32_t dst_ip, uint8_t protocol,
	      const uint8_t *payload, uint16_t len);
int ipv4_send_ttl(net_device_t *dev, uint32_t dst_ip, uint8_t protocol,
		  const uint8_t *payload, uint16_t len, uint8_t ttl);
// Full control: TTL + TOS, used by SOCK_RAW / per-socket IP_TTL/IP_TOS.
int ipv4_send_full(net_device_t *dev, uint32_t dst_ip, uint8_t protocol,
		   const uint8_t *payload, uint16_t len, uint8_t ttl,
		   uint8_t tos);
// Send caller-supplied IP packet verbatim (SOCK_RAW with IP_HDRINCL).
int ipv4_send_raw(net_device_t *dev, uint32_t dst_ip,
		  const uint8_t *full_ip_packet, uint16_t total_len);
void ipv4_rx(net_device_t *dev, const uint8_t *data, uint16_t len);
uint16_t ipv4_checksum(const void *data, uint16_t len);

// ============================================================================
// ICMP API
// ============================================================================
// An ICMP error about a datagram this host sent, reported locally (no
// message on the wire): `inner_ip' is the datagram.  Goes to the socket or
// connection it came from.
void icmp_dispatch_error(uint8_t icmp_type, uint8_t icmp_code,
			 const uint8_t *inner_ip, uint16_t inner_len,
			 uint16_t mtu_hint);

// PMTU cache (RFC 1191)
void pmtu_set(uint32_t dst_ip, uint16_t mtu);
uint16_t pmtu_get(uint32_t dst_ip); // returns 0 if no entry

// ============================================================================
// UDP API
// ============================================================================
void udp_init(void);
// A datagram from the kernel itself (DHCP, DNS): out of `dev', without
// DF.  0 or -1.
int udp_send(net_device_t *dev, uint32_t dst_ip, uint16_t src_port,
	     uint16_t dst_port, const uint8_t *data, uint16_t len);

// ============================================================================
// TCP API
// ============================================================================
void tcp_init(void);
// A received segment: data at the TCP header, IPCB(skb) filled in by the IP
// layer.  Always consumes the skb.  Softirq context.
int tcp_v4_rcv(sk_buff_t *skb);
/* Queue application data for sending: as much as the send buffer takes
 * (the count), 0 when it is full (wait for tx_ready), or -errno.  `urg':
 * the data ends with urgent data (MSG_OOB). */
int tcp_send_data(tcp_conn_t *conn, const uint8_t *data, uint16_t len);
int tcp_send_data_urg(tcp_conn_t *conn, const uint8_t *data, uint16_t len,
		      int urg);
int tcp_send_oob(tcp_conn_t *conn, uint8_t byte);
int tcp_at_mark(tcp_conn_t *conn);
/* Copy up to `len' bytes of in-order received data to `buf' (the caller
 * has made the user buffer resident and opened SMAP; the copy itself runs
 * with interrupts on and no lock held).  Returns the count (0 when nothing
 * is queued).  A read stops at the urgent mark; without SO_OOBINLINE the
 * urgent byte is not part of the stream.  Advertises the reopened window,
 * tunes the receive buffer.  `peek': leave the data queued. */
int tcp_recv_copy(tcp_conn_t *conn, void *buf, size_t len, int peek);
/* recv(MSG_OOB): 1 with the urgent byte in *byte (taken unless peek), 0 at
 * end of stream, -EAGAIN when it has not arrived yet, -EINVAL when there
 * is none to read (or SO_OOBINLINE is on).  Never blocks. */
int tcp_recv_urg(tcp_conn_t *conn, int peek, uint8_t *byte);
// SO_OOBINLINE on the connection.
void tcp_set_oobinline(tcp_conn_t *conn, int on);
#define TCP_URG_NONE 0
#define TCP_URG_NOTYET 1 // the pointer is known, the byte not received yet
#define TCP_URG_VALID 2 // the byte is here, not read with MSG_OOB yet
#define TCP_URG_READ 3 // ... read with MSG_OOB (still out of band)
// Bytes of unread in-order data (FIONREAD).
uint32_t tcp_inq(tcp_conn_t *conn);
/* ICMP "fragmentation needed" quoting our segment `seq' with the next-hop
 * MTU `info': validated against the connection, then the path MTU is
 * lowered and the connection re-segments (tcp.c). */
void tcp_v4_mtu_report(uint32_t local_ip, uint16_t local_port,
		       uint32_t remote_ip, uint16_t remote_port, uint32_t seq,
		       uint32_t info);
tcp_conn_t *tcp_connect(net_device_t *dev, uint32_t local_ip, uint32_t dst_ip,
			uint16_t src_port, uint16_t dst_port);
/* What a connection must know before its SYN goes out: the buffer sizes
 * (the window scale is chosen from them), the MSS clamp, the SYN retries
 * and the window clamp the socket asked for.  0 = the default. */
struct tcp_conn_params {
	int sndbuf;
	int rcvbuf;
	uint8_t userlocks; // TCP_SNDBUF_LOCK | TCP_RCVBUF_LOCK
	uint8_t syn_retries;
	uint16_t user_mss;
	uint32_t window_clamp;
	uint8_t user_port; // the application bound the local port itself
	int *err; // out: why no connection was made (-errno)
};
tcp_conn_t *tcp_connect_opts(net_device_t *dev, uint32_t local_ip,
			     uint32_t dst_ip, uint16_t src_port,
			     uint16_t dst_port, const struct tcp_conn_params *p);
tcp_conn_t *tcp_listen(net_device_t *dev, uint32_t local_ip,
		       uint16_t local_port, int backlog);
// listen() again on a listening socket: only the backlog changes.
void tcp_listen_set_backlog(tcp_conn_t *listener, int backlog);
tcp_conn_t *tcp_accept(tcp_conn_t *listener);
int tcp_close(tcp_conn_t *conn);
// shutdown(SHUT_WR): FIN after the queued data, the receive side stays.
int tcp_shutdown(tcp_conn_t *conn);
void tcp_abort(tcp_conn_t *conn); // Send RST (SO_LINGER l_onoff=0)
// Reference counting: hold/put a connection so it is not freed while used.
void tcp_conn_get(tcp_conn_t *conn);
void tcp_conn_release(tcp_conn_t *conn);
/* Take the pending hard error (reported once); 0 when none. */
int tcp_sock_error(tcp_conn_t *conn);
void tcp_set_cork(tcp_conn_t *conn, int on);
/* SO_MAX_PACING_RATE: cap the connection's pacing rate (bytes/s); any
 * value but ~0 also turns TCP's own pacing on for it. */
void tcp_set_max_pacing_rate(tcp_conn_t *conn, uint64_t rate);
/* SO_SNDBUF / SO_RCVBUF (already doubled by the socket layer); a size set
 * by the application stops the auto-tuning of that buffer. */
#define TCP_SNDBUF_LOCK 0x1
#define TCP_RCVBUF_LOCK 0x2
void tcp_set_buffers(tcp_conn_t *conn, int sndbuf, int rcvbuf, uint8_t locks);
// IPPROTO_TCP options kept by the connection: 0 or -errno.  Besides the
// TCP_* names: the socket-level keepalive switch and the keepalive times
// (in seconds, as TCP_KEEPIDLE / TCP_KEEPINTVL take them).
#define TCP_SOCKOPT_KEEPALIVE 0x10001
int tcp_setsockopt_int(tcp_conn_t *conn, int optname, int val);
/* TCP_CONGESTION: the congestion controls by name.  tcp_ca_find returns
 * an id > 0 (what tcp_setsockopt_int(TCP_CONGESTION) takes) or -ENOENT;
 * tcp_ca_name the name of an id (0: the default). */
int tcp_ca_find(const char *name, size_t len);
const char *tcp_ca_name(int id);
int tcp_ca_id(const tcp_conn_t *conn);
int tcp_getsockopt_int(tcp_conn_t *conn, int optname, int *val);
void tcp_icmp_error(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
		    uint16_t remote_port, uint32_t seq, int err);
void tcp_reap_pending(void);
void tcp_fill_info(tcp_conn_t *conn, struct tcp_info *info);
struct tty; // forward declaration for dump output
void tcp_dump_table(struct tty *tty);
int tcp_local_port_in_use(uint16_t port);
/* Is this exact 4-tuple already taken (any state but LISTEN/CLOSED)?  The
 * question a connecting socket has to ask before claiming a local port --
 * see the note in alloc_ephemeral_port(). */
int tcp_4tuple_in_use(uint32_t local_ip, uint16_t local_port,
		      uint32_t remote_ip, uint16_t remote_port);

// ============================================================================
// DHCP API
// ============================================================================
void dhcp_init(void);
int dhcp_discover(net_device_t *dev);
void dhcp_rx(net_device_t *dev, const uint8_t *data, uint16_t len);
int dhcp_configured(void);

// ============================================================================
// Socket API (kernel-side, called from syscall layer)
// ============================================================================
void socket_init(void);
int sock_create(int domain, int type, int protocol);
int sock_bind(int sockfd, const struct sockaddr_in *addr);
int sock_listen(int sockfd, int backlog);
int sock_accept(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen);
int sock_connect(int sockfd, const struct sockaddr_in *addr);
int sock_sendto(int sockfd, const void *buf, size_t len, int flags,
		const struct sockaddr_in *dest_addr, socklen_t addrlen);
int sock_recvfrom(int sockfd, void *buf, size_t len, int flags,
		  struct sockaddr_in *src_addr, socklen_t *addrlen);
int sock_send(int sockfd, const void *buf, size_t len, int flags);
int sock_recv(int sockfd, void *buf, size_t len, int flags);
int sock_close(int sockfd);
int sock_shutdown(int sockfd, int how);
int sock_setsockopt(int sockfd, int level, int optname, const void *optval,
		    socklen_t optlen);
int sock_getsockopt(int sockfd, int level, int optname, void *optval,
		    socklen_t *optlen);
int sock_getpeername(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen);
int sock_getsockname(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen);

// ============================================================================
// Packet RX Handler (called by NIC driver on packet receive)
// ============================================================================
void net_rx_packet(net_device_t *dev, const uint8_t *data, uint16_t len);

// ============================================================================
// Timer hook (called from timer IRQ for TCP timers)
// ============================================================================
void net_timer_tick(void);
void net_housekeeping_kick(void); // run the housekeeping softirq soon

// ============================================================================
// Socket FD Markers (stored in fd_table[] alongside vfs_file_t* and pipe markers)
// ============================================================================
#define SOCKET_FD_BASE 0x10000UL
#define EPOLL_FD_BASE 0x20000UL
/* 0x30000 was the UNIX socket marker range.  Retired: a UNIX socket
 * descriptor now holds the socket itself.  Do not reuse this range. */
/* Epoll instances are SYSTEM-WIDE, not per process, so this ceiling is shared
 * by every program running at once -- and running out is not a soft failure.
 * GLib probes whether a descriptor is pollable by creating a throwaway epoll
 * set for it (gio/giounix-private.c, _g_fd_is_pollable), and if epoll_create1
 * fails it calls g_error(), which ABORTS the process.  So a GTK application
 * does not degrade when this table is full: it dies.  32 was one X session's
 * worth with nothing to spare.  An instance is allocated when created (the
 * table holds pointers, 32 KB), and how many descriptors one watches is
 * bounded only by the system-wide watch limit (eventpoll.c).  The marker range
 * (EPOLL_FD_BASE up to the retired 0x30000) allows up to 65536. */
#define MAX_EPOLL_INSTANCES 4096
/* Also SYSTEM-WIDE.  Every socket(), every accept() and every socketpair() end
 * takes a slot, and an X session spends two per client connection (the
 * client's end and the server's accepted end) before the clients' own
 * socketpairs.  Exhaustion is reported honestly (unix_create -> -ENOMEM,
 * unix_accept -> -ENOMEM plus the waiting client's error), but for a GTK
 * program that surfaces as an unexplained failure to reach the display.
 *
 * A slot is 656 bytes, so 1024 of them is ~670 KB.  It used to be ~8.6 KB
 * because the 8 KB data ring was inlined into every slot whether the socket
 * existed or not; the ring became a pointer allocated on first use, which made
 * the table an order of magnitude cheaper to widen than the old note here
 * claimed.  Bounded by the fd encoding: UNIX_SOCKET_FD_BASE ranges are 0x10000
 * apart, so anything up to 65536 is representable. */
#define MAX_UNIX_SOCKETS 1024

#define IS_SOCKET_FD(ptr)                      \
	((uintptr_t)(ptr) >= SOCKET_FD_BASE && \
	 (uintptr_t)(ptr) < SOCKET_FD_BASE + NET_MAX_SOCKETS)
#define SOCKET_FD_IDX(ptr) ((int)((uintptr_t)(ptr) - SOCKET_FD_BASE))
#define MAKE_SOCKET_FD(idx) \
	((struct vfs_file *)(SOCKET_FD_BASE + (unsigned)(idx)))

#define IS_EPOLL_FD(ptr)                      \
	((uintptr_t)(ptr) >= EPOLL_FD_BASE && \
	 (uintptr_t)(ptr) < EPOLL_FD_BASE + MAX_EPOLL_INSTANCES)
#define EPOLL_FD_IDX(ptr) ((int)((uintptr_t)(ptr) - EPOLL_FD_BASE))
#define MAKE_EPOLL_FD(idx) \
	((struct vfs_file *)(EPOLL_FD_BASE + (unsigned)(idx)))

/* A UNIX socket descriptor holds the socket ITSELF, not an index into a
 * table.  0x30000 is retired and must not be reused as a marker range.
 * Identify one with unix_sock_is(); see its definition for why that is safe
 * to call on any descriptor-table value. */

// ============================================================================
// Scatter/Gather I/O (sendmsg/recvmsg)
// ============================================================================
struct iovec {
	void *iov_base;
	size_t iov_len;
};

struct msghdr {
	void *msg_name; // Optional address
	socklen_t msg_namelen; // Size of address
	struct iovec *msg_iov; // Scatter/gather array
	int msg_iovlen; // Number of elements in msg_iov
	void *msg_control; // Ancillary data (unused)
	size_t msg_controllen; // Ancillary data length (unused)
	int msg_flags; // Flags on received message
};

// ============================================================================
// Poll / Select / Epoll
// ============================================================================
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200
#define POLLMSG 0x0400
#define POLLRDHUP 0x2000 /* the peer closed its sending side (or shut down) */

struct pollfd {
	int fd;
	short events;
	short revents;
};

// fd_set for select()
#define FD_SETSIZE 1024
#define __NFDBITS (8 * sizeof(unsigned long))
#define __FD_ELT(d) ((d) / __NFDBITS)
#define __FD_MASK(d) (1UL << ((d) % __NFDBITS))

typedef struct {
	unsigned long fds_bits[FD_SETSIZE / __NFDBITS];
} fd_set;

#define FD_ZERO(set)                                                     \
	do {                                                             \
		for (unsigned _i = 0; _i < FD_SETSIZE / __NFDBITS; _i++) \
			(set)->fds_bits[_i] = 0;                         \
	} while (0)
#define FD_SET(d, set) ((set)->fds_bits[__FD_ELT(d)] |= __FD_MASK(d))
#define FD_CLR(d, set) ((set)->fds_bits[__FD_ELT(d)] &= ~__FD_MASK(d))
#define FD_ISSET(d, set) (((set)->fds_bits[__FD_ELT(d)] & __FD_MASK(d)) != 0)

// Epoll
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN 0x001
#define EPOLLPRI 0x002
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLLRDNORM 0x040
#define EPOLLRDBAND 0x080
#define EPOLLWRNORM 0x100
#define EPOLLWRBAND 0x200
#define EPOLLMSG 0x400
#define EPOLLRDHUP 0x2000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP (1U << 29)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET (1U << 31)

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
} __attribute__((packed));

/* epoll_create1() flags.  Same value userspace uses (sys/epoll.h). */
#define EPOLL_CLOEXEC 0x80000


// ============================================================================
// Network Interface Ioctls (conventional numeric values)
// ============================================================================
// Socket-level / interface ioctls
#define SIOCGIFNAME 0x8910
#define SIOCSIFLINK 0x8911
#define SIOCGIFCONF 0x8912
#define SIOCGIFFLAGS 0x8913
#define SIOCSIFFLAGS 0x8914
#define SIOCGIFADDR 0x8915
#define SIOCSIFADDR 0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCSIFDSTADDR 0x8918
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891A
#define SIOCGIFNETMASK 0x891B
#define SIOCSIFNETMASK 0x891C
#define SIOCGIFMETRIC 0x891D
#define SIOCSIFMETRIC 0x891E
#define SIOCGIFMEM 0x891F
#define SIOCSIFMEM 0x8920
#define SIOCGIFMTU 0x8921
#define SIOCSIFMTU 0x8922
#define SIOCSIFNAME 0x8923
#define SIOCSIFHWADDR 0x8924
#define SIOCGIFENCAP 0x8925
#define SIOCSIFENCAP 0x8926
#define SIOCGIFHWADDR 0x8927
#define SIOCGIFSLAVE 0x8929
#define SIOCSIFSLAVE 0x8930
#define SIOCADDMULTI 0x8931
#define SIOCDELMULTI 0x8932
#define SIOCGIFINDEX 0x8933
#define SIOGIFINDEX SIOCGIFINDEX
#define SIOCSIFPFLAGS 0x8934
#define SIOCGIFPFLAGS 0x8935
#define SIOCDIFADDR 0x8936
#define SIOCSIFHWBROADCAST 0x8937
#define SIOCGIFCOUNT 0x8938

// Routing / ARP / protocol ioctls
#define SIOCADDRT 0x890B
#define SIOCDELRT 0x890C
#define SIOCRTMSG 0x890D
#define SIOCGARP 0x8954
#define SIOCSARP 0x8955
#define SIOCDARP 0x8956
#define SIOCGSTAMP 0x8906
#define SIOCGSTAMPNS 0x8907
#define SIOCINQ 0x541B /* = FIONREAD: bytes readable now */
#define SIOCOUTQ 0x5411 /* = TIOCOUTQ: bytes sent and not yet acknowledged */
#define SIOCOUTQNSD 0x894B /* bytes not yet sent */
#define SIOCATMARK 0x8905 /* the next byte read is the urgent one */

// Bridge / VLAN / misc
#define SIOCBRADDBR 0x89A0
#define SIOCBRDELBR 0x89A1
#define SIOCBRADDIF 0x89A2
#define SIOCBRDELIF 0x89A3
#define SIOCGIFVLAN 0x8982
#define SIOCSIFVLAN 0x8983

// Interface flags (for SIOCGIFFLAGS / SIOCSIFFLAGS)
#define IFF_UP 0x0001
#define IFF_BROADCAST 0x0002
#define IFF_DEBUG 0x0004
#define IFF_LOOPBACK 0x0008
#define IFF_POINTOPOINT 0x0010
#define IFF_NOTRAILERS 0x0020
#define IFF_RUNNING 0x0040
#define IFF_NOARP 0x0080
#define IFF_PROMISC 0x0100
#define IFF_ALLMULTI 0x0200
#define IFF_MULTICAST 0x1000

#define IFNAMSIZ 16

// struct ifreq - interface request (for ioctl)
struct ifreq {
	char ifr_name[IFNAMSIZ];
	union {
		struct sockaddr ifr_addr;
		struct sockaddr ifr_dstaddr;
		struct sockaddr ifr_broadaddr;
		struct sockaddr ifr_netmask;
		struct sockaddr ifr_hwaddr;
		short ifr_flags;
		int ifr_ifindex;
		int ifr_metric;
		int ifr_mtu;
		char ifr_slave[IFNAMSIZ];
		char ifr_newname[IFNAMSIZ];
		char ifr_data[14];
	};
};

// struct ifconf - interface configuration list
struct ifconf {
	int ifc_len;
	union {
		char *ifc_buf;
		struct ifreq *ifc_req;
	};
};

// struct rtentry - routing table entry (simplified)
struct rtentry {
	struct sockaddr rt_dst;
	struct sockaddr rt_gateway;
	struct sockaddr rt_genmask;
	short rt_flags;
	int rt_metric;
	char *rt_dev;
};

// struct arpreq - ARP request
struct arpreq {
	struct sockaddr arp_pa; // Protocol address
	struct sockaddr arp_ha; // Hardware address
	int arp_flags;
	struct sockaddr arp_netmask;
	char arp_dev[IFNAMSIZ];
};

#define ATF_COM 0x02 // Completed entry (valid)
#define ATF_PERM 0x04 // Permanent entry

// ============================================================================
// Extended Socket API (kernel-side)
// ============================================================================
int sock_socketpair(int domain, int type, int protocol, int sv[2]);
int sock_accept4(int sockfd, struct sockaddr_in *addr, socklen_t *addrlen,
		 int flags);
int sock_sendmsg(int sockfd, const struct msghdr *msg, int flags);
int sock_recvmsg(int sockfd, struct msghdr *msg, int flags);
int sock_poll(int sockfd, short events);
int sock_ioctl_net(int sockfd, unsigned long request, void *argp);
int sock_fcntl_net(int sockfd, int cmd, unsigned long arg);
net_socket_t *sock_get(int sockfd);
/* A reference on a live socket (ref_count not yet 0), released with
 * sock_close(); 0 when its last reference is gone. */
int sock_tryhold(int sockfd);

// ============================================================================
// Poll / Select / Epoll API (kernel-side)
// ============================================================================
int sys_select_internal(int nfds, fd_set *readfds, fd_set *writefds,
			fd_set *exceptfds, uint64_t timeout_ns);
int sys_poll_internal(struct pollfd *fds, int nfds, uint64_t timeout_ns);
/* References on an epoll instance (kernel/net/eventpoll.c).  An instance is
 * global while its descriptors are per process: fork and dup take one
 * (epoll_get, on an instance already referenced), close and exec drop one,
 * and the instance goes with the last.  epoll_tryget takes one only while
 * the instance is still live (fdhold). */
void epoll_get(int idx);
void epoll_put(int idx);
int epoll_tryget(int idx);
int net_ioctl(unsigned long request, void *argp);

// ============================================================================
// Sendfile (kernel-side)
// ============================================================================
int sock_sendfile(int out_fd, int in_fd, int64_t *offset, size_t count);

// ============================================================================
// Routing Table
// ============================================================================
#define RTF_UP 0x0001
#define RTF_GATEWAY 0x0002
#define RTF_HOST 0x0004
#define RTF_REJECT 0x0200

void route_init(void);
int route_add(uint32_t dst_net, uint32_t netmask, uint32_t gateway,
	      net_device_t *dev, uint32_t metric, uint16_t flags);
int route_del(uint32_t dst_net, uint32_t netmask, uint32_t gateway);
net_device_t *route_lookup(uint32_t dst_ip, uint32_t *next_hop_out);
struct dst_entry;
// A held output route for `daddr' (kernel/net/dst.h): 0, or a negative
// errno (-ENETUNREACH, -EHOSTUNREACH, ...).  `oif' restricts the limited
// and subnet broadcast cases to that device (NULL: any).
int ip_route_output(uint32_t daddr, uint32_t saddr, net_device_t *oif,
		    struct dst_entry **out);
// An upper layer saw progress towards `daddr' (neighbour confirmation).
void ip_confirm_daddr(uint32_t daddr);
net_device_t *net_get_loopback(void);
void loopback_process_pending(void);

// ============================================================================
// UNIX Domain Socket Address
// ============================================================================
#define UNIX_PATH_MAX 108

/* Capacity of the in-band descriptor queue (SCM_RIGHTS in flight per socket).
 * The table below holds MAX_UNIX_SOCKETS of these statically, so the size is
 * a compromise: big enough that a burst of descriptor-carrying SEQPACKET
 * messages does not bounce with EAGAIN, small enough not to double the
 * table.  See the field's comment in unix_socket. */
#define UNIX_PENDING_FDS 32

struct sockaddr_un {
	sa_family_t sun_family; // AF_UNIX
	char sun_path[UNIX_PATH_MAX]; // Pathname
};

// UNIX domain socket (kernel internal)
typedef struct unix_socket {
	/* MUST stay first.  A descriptor table entry is one of several kinds
	 * of thing -- a small tagged integer, a pipe end, a file, a socket --
	 * and telling them apart means reading a known value out of the
	 * candidate.  pipe_end_t keeps its own magic at offset 0 for the same
	 * reason, so the two predicates read the same place and cannot mistake
	 * each other's objects.  Cleared before the memory is released, so a
	 * stale pointer fails identification rather than passing it. */
	uint64_t magic;
	/* A small stable number to report to userspace -- st_ino, /proc/self/fd
	 * -- in place of the address.  The address is a kernel pointer now and
	 * must never be handed out. */
	uint64_t id;
	int active;
	int type; // SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET
	int bound;
	/* A pathname bind also creates an S_IFSOCK node in the filesystem, and
	 * this records that we own it so close() removes it again.  Abstract
	 * names have no filesystem presence and leave this clear. */
	int has_node;
	int listening;
	int connected;
	int closed;
	int nonblock;
	int error;
	/* Bound name, as RAW BYTES — not a C string.
	 *
	 * Two disjoint namespaces share this field, told apart by the first byte
	 * (a pathname can never start with NUL):
	 *   path[0] != '\0'  -> filesystem pathname; path_len = strlen
	 *   path[0] == '\0'  -> abstract name; path_len counts the leading NUL,
	 *                       so an abstract socket with an empty name has
	 *                       path_len == 1.  The bytes after the NUL are
	 *                       arbitrary (they may contain NULs), which is why
	 *                       this cannot be a C string and every comparison
	 *                       must be (path_len, memcmp).
	 * path_len == 0 means unnamed: not reachable by name at all. */
	char path[UNIX_PATH_MAX];
	int path_len;
	struct unix_socket *peer; // Connected peer
	struct unix_socket *parent; // Listener (for accepted sockets)

	/* The peer's name, COPIED at the moment the connection is made.
	 *
	 * getpeername() must keep answering after the far end has closed: the
	 * connection is over, but this socket is still open and still knows who
	 * it was talking to, and callers ask precisely when they are cleaning up
	 * after a peer that has just gone away.  Chasing `peer` instead means the
	 * answer disappears the instant the other side calls close() -- a plain
	 * accept-then-close on the server left the client unable to name the
	 * address it had just successfully connected to.
	 *
	 * peer_valid says a connection was made at all; without it there is no
	 * way to tell "connected to an unnamed socket" (normal: a client that
	 * never bound) from "never connected" (ENOTCONN). */
	char peer_path[UNIX_PATH_MAX];
	int peer_path_len;
	int peer_valid;

	/* Data ring, allocated on first use rather than inlined.
	 *
	 * It used to be a fixed uint8_t[8192] inside every one of the
	 * MAX_UNIX_SOCKETS slots.  Inlining a ring big enough to be useful
	 * would cost that size times the whole table whether the sockets exist
	 * or not, so the ring is allocated when a socket first carries data and
	 * released when it closes.  Listening sockets never carry data and
	 * never allocate one.
	 *
	 * The size matters for throughput: an X client pushing a window's worth
	 * of pixels through a small ring stalls once per ringful, and every
	 * stall is a context switch. */
	uint8_t *buf;
	int bufsz;
	int head;
	int tail;
	volatile int ready; // Data available
	volatile int peer_closed; // Peer has closed
	/* shutdown(SHUT_RD) was applied to THIS end: reads return end of file.
	 *
	 * Deliberately not peer_closed, which means "the far end hung up" and
	 * is what poll() reports a hangup from.  Setting that on ourselves made
	 * a socket report POLLHUP to its own owner for ever, on a connection
	 * the peer was still using quite happily. */
	volatile int shut_rd;

	// Accept queue (for listening sockets)
	struct unix_socket *accept_queue[16];
	int accept_head;
	int accept_tail;
	int backlog;
	volatile int accept_ready;

	// Pending in-band file descriptors (SCM_RIGHTS).
	// Sender pushes to peer's queue with the raw fd_table entry pointer
	// (refcount already bumped); receiver pops and installs in its own
	// fd_table.  Each pending fd is associated with an absolute byte
	// offset (the cumulative number of bytes that had been written into
	// the receive ring at the time the fd was attached); recvmsg
	// delivers the fd together with bytes up to (and not past) that
	// boundary so stream-mode SCM_RIGHTS is correctly framed.
	// Sized for the SEQPACKET users: WebKit's IPC attaches every shared
	// buffer and file of a message as descriptors on ONE sendmsg, so a
	// burst of messages can leave several dozen queued before the reader
	// drains them.  tmux, the original user, sends at most one per imsg.
	// Over-flow returns -EAGAIN to the caller.
	void *pending_fds[UNIX_PENDING_FDS];
	uint64_t pending_fd_off[UNIX_PENDING_FDS];
	int pending_fd_head;
	int pending_fd_tail;
	uint64_t bytes_written; /* total bytes ever pushed into recv ring */
	uint64_t bytes_read; /* total bytes ever consumed from recv ring */

	spinlock_t lock;
	/* DESCRIPTORS referring to this socket: one at creation, one more per
	 * dup() and per fork() that inherits the table, one fewer per close().
	 * The socket is CONNECTED while this is above zero -- closing one of
	 * several descriptors must not disturb the others, and must not tell
	 * the peer anything.  Reaching zero is the hangup, and nothing else. */
	int ref_count;
	/* MEMORY lifetime, which is a different question from the hangup.
	 *
	 * The slot stays claimed while this is above zero, so a pointer to this
	 * socket cannot quietly become a pointer to a DIFFERENT one.  That was
	 * the whole difficulty: sockets live in a fixed array, so a stale
	 * pointer never faults -- it silently reads whichever socket now
	 * occupies the slot, and the damage surfaces somewhere else entirely,
	 * as a connection that reports a hangup it was never told about.
	 *
	 * Everything that can outlive the descriptors holds one: the peer link,
	 * each entry queued on a listener, and every operation in flight.  The
	 * socket is destroyed exactly once, when the last of them lets go.
	 *
	 * This replaces two ad-hoc counters (senders in flight, and a flag
	 * saying the release had been left to them).  They were an attempt to
	 * answer this question for one caller at a time; a reference count
	 * answers it for all of them. */
	refcount_t refcount;

	/* Who is polling THIS socket -- see the note on net_socket_t's. */
	struct wait_queue_head poll_wq;

	/* Credentials, for SO_PEERCRED.
	 *
	 * `self' is whoever created this socket, refreshed at listen() and at
	 * connect() -- the two moments a socket commits to a connection, and
	 * the moments conventional Unix samples.  `peer' is a COPY of the other
	 * end's `self', taken when the two were joined.
	 *
	 * A copy rather than a look through ->peer, and that is the point: the
	 * peer process may have exited, and its socket may have been freed and
	 * the slot reused, long before anybody asks who it was.  The answer has
	 * to be the one that was true at connect time, and it has to still be
	 * readable. */
	struct ucred self_cred;
	struct ucred peer_cred;
	int has_peer_cred;
} unix_socket_t;

// UNIX domain socket API
int unix_create(int type, unix_socket_t **out);
int unix_bind(unix_socket_t *us, const struct sockaddr_un *addr,
	      socklen_t addrlen);
/* Fill in this socket's name (peer == 0) or its peer's (peer == 1).
 * *addrlen is in/out: buffer size in, required size out. */
int unix_getname(unix_socket_t *us, int peer, struct sockaddr_un *addr,
		 socklen_t *addrlen);
int unix_listen(unix_socket_t *us, int backlog);
int unix_accept(unix_socket_t *us, struct sockaddr_un *addr,
		socklen_t *addrlen, unix_socket_t **out);
int unix_connect(unix_socket_t *us, const struct sockaddr_un *addr,
		 socklen_t addrlen);
int unix_send(unix_socket_t *us, const void *buf, size_t len, int flags);
/* unix_send carrying in-band descriptors, queued atomically with the first
 * byte it places (a SEQPACKET socket gets one record).  On ANY negative
 * return nothing was queued and the caller still owns the entries'
 * references; on success the peer owns them.  See unix_socket.c. */
int unix_send_fds(unix_socket_t *us, const void *buf, size_t len, int flags,
		  void **fd_entries, int nfds);
int unix_recv(unix_socket_t *us, void *buf, size_t len, int flags);
int unix_close(unix_socket_t *us);
int unix_socketpair(int type, unix_socket_t *sv[2]);
int unix_shutdown(unix_socket_t *us, int how);
/* SOL_SOCKET options on a local socket.  Options that do nothing here are
 * accepted (and read back as off) rather than refused, because callers treat a
 * failing setsockopt as the socket being unusable; genuinely unimplemented
 * options return -ENOPROTOOPT.  optval/optlen are KERNEL pointers -- the
 * syscall layer copies to and from userspace. */
int unix_setsockopt(unix_socket_t *us, int level, int optname,
		    const void *optval, socklen_t optlen);
int unix_getsockopt(unix_socket_t *us, int level, int optname, void *optval,
		    socklen_t *optlen);

int unix_poll(unix_socket_t *us, short events);
/* Take one more DESCRIPTOR reference on an already-referenced socket: what
 * dup() and every fork() inheriting the table owe it.  Both counters move
 * together, under the socket's lock, so a close running at the same moment
 * cannot see one without the other. */
void unix_sock_fdget(unix_socket_t *us);
/* Resolve a descriptor marker to its socket, taking a reference that keeps it
 * alive for as long as the caller holds it.  Release with unix_sock_put_ref().
 * Returns NULL if the marker names no live socket. */
unix_socket_t *unix_sock_lookup_hold(unix_socket_t *us);
/* Is this descriptor-table value a UNIX socket?  Safe on any value: numeric
 * markers are rejected before anything is dereferenced. */
bool unix_sock_is(const void *p);
void unix_sock_put_ref(unix_socket_t *us);
/* SCM_RIGHTS file-descriptor passing helpers.  push() enqueues an
 * fd_table entry (already ref-counted by caller) onto the receiver's
 * queue; pop() removes the head entry.  Both return 0 on success or a
 * negative errno (EAGAIN if queue full / empty). */
int unix_push_fd(unix_socket_t *sock, void *entry);
void unix_fd_space_wake(unix_socket_t *sock);
/* Queue an in-band descriptor for `us`'s PEER, finding and pinning that peer
 * inside the socket layer.  Callers must not resolve the peer themselves: a
 * peer pointer read once and used later can name a different socket. */
int unix_send_fd(unix_socket_t *us, void *entry);
/* One SEQPACKET record and its in-band descriptors in a single atomic
 * insert -- see unix_send_record in unix_socket.c for why the two must
 * never be queued separately. */
int unix_send_record_msg(unix_socket_t *us, const void *buf, size_t len,
			 void **fd_entries, int nfds);
int unix_pop_fd(unix_socket_t *sock, void **out_entry);
/* Peek the byte offset associated with the head of the pending-fd queue,
 * if any.  Returns 0 on success and stores the absolute byte offset (in
 * the receiver's bytes_read coordinate system) into *out_off; returns
 * -EAGAIN when the queue is empty. */
int unix_peek_fd_offset(unix_socket_t *sock, uint64_t *out_off);

// ============================================================================
// DNS Resolver
// ============================================================================
int dns_resolve(const char *hostname, uint32_t *ip_out);
int dns_resolve_reverse(uint32_t ip_nbo, char *out, int maxlen);
int dns_query_raw(const char *name, uint16_t qtype, uint8_t *response,
		  int max_len);
void dns_init(void);
void dns_rx(const uint8_t *data, uint16_t len);

#define DNS_CLIENT_PORT 5353

// ============================================================================
// Userspace-visible info structures (for SYS_NET_GETINFO)
// ============================================================================

// ARP table entry (userland view)
typedef struct net_arp_info {
	uint32_t ip; // Host byte order
	uint8_t mac[6];
	uint8_t valid;
	uint8_t pad;
} net_arp_info_t;

// Route table entry (userland view)
typedef struct net_route_info {
	uint32_t dst_net; // Host byte order
	uint32_t netmask; // Host byte order
	uint32_t gateway; // Host byte order
	uint16_t flags;
	uint16_t metric;
	char dev_name[16];
} net_route_info_t;

// TCP connection info (userland view)
typedef struct net_tcp_info {
	uint32_t local_ip;
	uint16_t local_port;
	uint32_t remote_ip;
	uint16_t remote_port;
	int state;
	uint32_t rx_queue;
	uint32_t tx_queue;
} net_tcp_info_t;

// UDP socket info (userland view)
typedef struct net_udp_info {
	uint32_t local_ip;
	uint16_t local_port;
	uint32_t remote_ip;
	uint16_t remote_port;
	uint32_t rx_queue;
} net_udp_info_t;

// Interface statistics (userland view)
typedef struct net_iface_info {
	char name[16];
	uint8_t mac[6];
	uint16_t mtu;
	uint32_t ip_addr;
	uint32_t netmask;
	uint32_t gateway;
	uint32_t dns_server;
	uint16_t flags;
	uint16_t pad;
	uint64_t rx_packets;
	uint64_t tx_packets;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint64_t rx_errors;
	uint64_t tx_errors;
	uint64_t rx_dropped;
} net_iface_info_t;

// Aggregate protocol counters (userland view).  Named fields keep the
// user-facing ABI stable; the kernel folds its internal per-CPU counter
// array into this struct in net_get_stats().  Counter names follow the
// standard management-information-base conventions (RFC 4293 IP,
// RFC 4022 TCP, RFC 4113 UDP) plus extended drop/event counters that make
// previously-silent discard paths observable.
typedef struct net_stats_info {
	/* IP */
	uint64_t ip_in_receives;
	uint64_t ip_in_hdr_errors;
	uint64_t ip_in_delivers;
	uint64_t ip_out_requests;
	/* TCP (standard) */
	uint64_t tcp_active_opens;
	uint64_t tcp_passive_opens;
	uint64_t tcp_attempt_fails;
	uint64_t tcp_estab_resets;
	uint64_t tcp_in_segs;
	uint64_t tcp_out_segs;
	uint64_t tcp_retrans_segs;
	uint64_t tcp_in_errs;
	uint64_t tcp_out_rsts;
	/* TCP (extended drop/event accounting) */
	uint64_t tcp_paws_drop;
	uint64_t tcp_oow_seq_drop;
	uint64_t tcp_challenge_ack;
	uint64_t tcp_ooo_queue_full;
	uint64_t tcp_ooo_oversize_drop;
	uint64_t tcp_ooo_fin_refused;
	uint64_t tcp_acceptq_full;
	uint64_t tcp_listener_gone_rst;
	uint64_t tcp_rst_data_loss;
	uint64_t tcp_conn_table_full;
	uint64_t tcp_backlog_drop;
	uint64_t tcp_persist_probes;
	uint64_t tcp_syncookie_sent;
	uint64_t tcp_syncookie_recv;
	uint64_t tcp_syncookie_fail;
	uint64_t tcp_tw_created;
	uint64_t tcp_tw_reused;
	uint64_t tcp_tw_killed;
	/* UDP */
	uint64_t udp_in_datagrams;
	uint64_t udp_out_datagrams;
	uint64_t udp_no_ports;
	uint64_t udp_rcvbuf_errors;
	/* buffer pool */
	uint64_t skb_alloc_fail;
} net_stats_info_t;

// Raw DNS query buffer (shared with userspace)
typedef struct dns_query_buf {
	char name[256];
	uint16_t qtype;
	uint16_t pad;
	int response_len;
	uint8_t response[512];
} dns_query_buf_t;

// ============================================================================
// Extended kernel API for userspace networking commands
// ============================================================================
int net_get_arp_table(net_arp_info_t *entries, int max_entries);
int net_get_route_table(net_route_info_t *entries, int max_entries);
int net_get_tcp_connections(net_tcp_info_t *entries, int max_entries);
int net_get_udp_sockets(net_udp_info_t *entries, int max_entries);
int net_get_iface_info(net_iface_info_t *entries, int max_entries);
int net_get_stats(net_stats_info_t *out);
int dhcp_release(net_device_t *dev);
int dhcp_renew(net_device_t *dev);
void dhcp_tick(void);
int dhcp_get_status(void);
// Invalidate the cached lease without sending a RELEASE.  Called by
// link drivers on a link-DOWN edge so the next dhcp_renew() will
// perform a full DISCOVER instead of a (now-stale) unicast renewal.
void dhcp_invalidate(net_device_t *dev);

// Raw ICMP send/receive for ping, traceroute, arping
int icmp_send_echo(net_device_t *dev, uint32_t dst_ip, uint16_t id,
		   uint16_t seq, const uint8_t *data, uint16_t len,
		   uint8_t ttl);
int icmp_recv_reply(uint32_t *src_ip, uint16_t expected_id, uint8_t *type_out,
		    uint8_t *code_out, uint16_t *seq_out,
		    uint64_t timeout_ticks, uint64_t *rtt_us_out,
		    uint16_t expected_seq, uint8_t *ttl_out);
int arp_send_request(net_device_t *dev, uint32_t target_ip);
int arp_recv_reply(uint32_t target_ip, uint8_t mac_out[6],
		   uint64_t timeout_ticks);
void net_set_hostname(const char *name);
const char *net_get_hostname(void);

/* Shared with the syscall layer split out of ke/syscall.c. */
struct task;
void poll_sigmask_restore_pending(struct task *cur);

#endif // _KERNEL_NET_H_
