// LikeOS — IPv4 internals: the receive control block, the protocol table,
// options, fragmentation and reassembly, output, ICMP, UDP and raw sockets.
//
// Receive path (softirq on NET_RX_CPU, interrupts enabled):
//   netif_receive_skb -> eth_rcv -> ip_rcv (header checks, input route:
//   local / broadcast / multicast or drop, options) -> ip_local_deliver
//   (reassembly) -> raw_local_deliver (copies for raw sockets) -> the
//   protocol's handler from inet_protos[] (tcp, udp, icmp, igmp).  A
//   handler gets skb->data at its own header, the network header set and
//   IPCB(skb) filled in, and always consumes the skb.
//
// ICMP errors about datagrams we sent go to the err_handler of the quoted
// protocol (struct icmp_err) after the ICMP layer validated the quote.
//
// Output: ip_send_skb() takes a transport payload (data at the transport
// header, IP header pushed here), routes it (struct ip_route), applies the
// path-MTU discovery mode, fragments when allowed and hands every frame to
// the loopback device or the neighbour of the next hop.  It never holds a
// lock across the device (dev_queue_xmit defers when called with
// interrupts off).
//
// Lock order of this layer (all taken with interrupts disabled):
//   udp table lock -> socket lock (s->lock) -> its skb queue locks
//   udp table lock / raw table lock -> fib lock -> dst pool lock (a
//     path-MTU report about one of the socket's datagrams)
//   raw table lock -> socket lock
//   frag queue lock -> frag table lock -> net timer wheel lock
//   igmp lock -> device multicast list lock, net timer wheel lock
//   icmp rate-limit lock, icmp reply lock: leaves
// The IP identification counters are atomics.  Nothing takes a table lock
// while holding a socket lock.
// None is held across a call into another layer, except that delivery to a
// socket queue happens under the table lock (so a socket that is being
// closed and unhashed is never queued to after its queues were purged).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_NET_IP_H
#define LIKEOS_NET_IP_H

#include <kernel/net/net.h>
#include <kernel/net/skb.h>
#include <kernel/net/dst.h>
#include <kernel/ke/syscall.h> // errno values

#define IPPROTO_IGMP 2

// Fragment field (host order)
#define IP_CE 0x8000 // reserved ("congestion") bit
#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFSET 0x1FFF

#define IP_MAX_PACKET 65535u
#define IP_HDR_MIN 20u
#define IP_HDR_MAX 60u
#define IP_DEFAULT_TTL 64

// Path-MTU discovery modes (IP_MTU_DISCOVER)
#define IP_PMTUDISC_DONT 0 // never DF
#define IP_PMTUDISC_WANT 1 // DF when the datagram fits the path MTU
#define IP_PMTUDISC_DO 2 // always DF; too big -> EMSGSIZE
#define IP_PMTUDISC_PROBE 3 // always DF, ignore the path MTU
#define IP_PMTUDISC_INTERFACE 4 // the device MTU, never DF on fragments
#define IP_PMTUDISC_OMIT 5 // like INTERFACE, ignore PMTU reports

// ---- options ----
#define IPOPT_COPIED(o) ((o)&0x80)
#define IPOPT_END 0
#define IPOPT_NOOP 1
#define IPOPT_RR 7
#define IPOPT_TS 68
#define IPOPT_SEC 130
#define IPOPT_LSRR 131
#define IPOPT_CIPSO 134
#define IPOPT_SID 136
#define IPOPT_SSRR 137
#define IPOPT_RA 148

#define IPOPT_TS_TSONLY 0
#define IPOPT_TS_TSANDADDR 1
#define IPOPT_TS_PRESPEC 3

// What ip_options_compile() found (offsets from the IP header, 0 = none).
struct ip_options {
	uint8_t optlen; // bytes of options (ihl * 4 - 20)
	uint8_t srr; // LSRR/SSRR
	uint8_t rr;
	uint8_t ts;
	uint8_t router_alert;
	uint8_t sec; // SEC/SID seen (accepted, ignored)
	uint8_t is_strictroute;
	uint8_t _pad;
};

// ---- receive control block (skb->cb) ----
#define IPSKB_REASSEMBLED 0x0001 // put together from fragments
#define IPSKB_LOOPED 0x0002 // our own datagram, looped back

struct inet_skb_parm {
	struct ip_options opt;
	uint16_t flags; // IPSKB_*
	uint16_t frag_max_size; // reassembled: the largest fragment
	int iif; // ifindex of the receiving device
	uint32_t saddr; // host order
	uint32_t daddr;
	uint8_t rt_type; // RTN_LOCAL / RTN_BROADCAST / RTN_MULTICAST
	uint8_t tos;
	uint8_t ttl;
	uint8_t proto;
	uint16_t ihl; // header bytes
	uint16_t tot_len; // datagram bytes (after reassembly: all of it)
};

#define IPCB(skb) ((struct inet_skb_parm *)((skb)->cb))

/* The tail of skb->cb, after the IPCB: whoever owns the skb next (the
 * reassembly queue, a datagram socket queue) keeps its own data here. */
#define IPCB_TAIL_OFF 32
BUILD_BUG_ON(sizeof(struct inet_skb_parm) > IPCB_TAIL_OFF);

// ---- the protocol table ----
struct icmp_err;
struct net_protocol {
	int (*handler)(sk_buff_t *skb); // consumes the skb
	void (*err_handler)(const struct icmp_err *e);
};
// Install the handler of `proto' (boot time).
void inet_add_protocol(const struct net_protocol *p, uint8_t proto);
const struct net_protocol *inet_get_protocol(uint8_t proto);

// An ICMP error about a datagram this host sent, as the ICMP layer found
// it after validating the quote.
struct icmp_err {
	uint8_t type, code;
	uint8_t proto; // of the quoted datagram
	uint8_t fatal; // a hard error (RFC 1122 4.2.3.9)
	int err; // errno it maps to (positive)
	uint32_t info; // FRAG_NEEDED: next-hop MTU; PARAMPROB: pointer
	uint32_t saddr, daddr; // quoted header, host order (saddr: ours)
	uint32_t from; // who sent the ICMP message (host order)
	const uint8_t *iph; // the quoted IP header
	uint32_t ihl; // its length
	const uint8_t *l4; // the quoted transport bytes
	uint32_t l4len; // >= 8
	sk_buff_t *skb; // the ICMP message (data at the ICMP header)
};

// ---- receive ----
int ip_rcv(sk_buff_t *skb); // data at the IP header; consumes
void ip_local_deliver(sk_buff_t *skb);
// Is `group' joined on `dev' (224.0.0.1 always is)?
int ip_mc_is_member(net_device_t *dev, uint32_t group);
// One of this host's addresses (127/8 included)?
int ip_addr_is_local(uint32_t addr);
// The device whose address is `addr', NULL if none.
net_device_t *ip_dev_by_addr(uint32_t addr);
net_device_t *ip_dev_by_index(int ifindex);
// A subnet or the limited broadcast address of some device?
int ip_addr_is_broadcast(uint32_t addr);

// ---- options ----
// Parse and check the options of the header at `iph' (ihl > 5).  0, or
// -1 with *bad set to the offset (from iph) of the byte at fault.
int ip_options_compile(const uint8_t *iph, struct ip_options *opt,
		       uint32_t *bad);
// Turn the options a fragment after the first must not carry into NOPs.
void ip_options_fragment(uint8_t *iph);

// ---- fragmentation ----
void ipfrag_init(void);
// Queue a fragment; returns the reassembled datagram (data at its IP
// header, IPCB set) or NULL when the fragment was queued or dropped.
sk_buff_t *ip_defrag(sk_buff_t *skb);
// Split the datagram at skb (data at the IP header, total length > mtu)
// into fragments of at most `mtu' bytes; each goes to out(ctx, frag).
// Consumes skb.  0 or -errno.
int ip_do_fragment(sk_buff_t *skb, uint32_t mtu,
		   int (*out)(void *ctx, sk_buff_t *frag), void *ctx);
// Reassembly diagnostics (tests).
struct ipfrag_stats {
	uint32_t queues; // queues in existence
	uint64_t mem; // bytes held
	uint64_t high_thresh, low_thresh;
	uint64_t evicted; // queues evicted under memory pressure
	uint64_t timeouts;
};
void ipfrag_get_stats(struct ipfrag_stats *st);
void ipfrag_set_thresh(uint64_t high, uint64_t low); // tests

// ---- output ----
// Where a datagram for daddr goes.
struct ip_route {
	struct dst_entry *dst; // held; NULL for the no-route fallback
	net_device_t *dev; // egress device (the loopback device for local)
	uint32_t nexthop;
	uint32_t saddr; // source address to use
	uint32_t mtu; // path MTU (exceptions included)
	uint32_t dev_mtu; // the device's MTU
	uint8_t type; // RTN_UNICAST / LOCAL / BROADCAST / MULTICAST
	uint8_t loopback; // delivered through the loopback device
	uint8_t mtu_locked; // the path MTU came from a report below 552
	uint8_t _pad;
};
// 0 or -errno (-ENETUNREACH, -EHOSTUNREACH, ...).  `saddr' 0: pick one.
int ip_route_out(uint32_t daddr, uint32_t saddr, net_device_t *oif,
		 struct ip_route *rt);
void ip_route_put(struct ip_route *rt);
// The MTU a sender in mode `pmtudisc' must respect on this route.
uint32_t ip_route_mtu(const struct ip_route *rt, uint8_t pmtudisc);

// Per-datagram output parameters.
#define IPTX_ID_PER_DEST 0x01 // ID from the per-destination counter even
	// with DF (the TCP path keeps its historic counter)
#define IPTX_NO_FRAG 0x02 // too big is -EMSGSIZE (raw IP_HDRINCL)
struct ip_tx {
	uint32_t saddr; // 0: from the route
	uint32_t daddr;
	net_device_t *oif; // NULL: any
	uint8_t proto;
	uint8_t ttl; // 0: the route's hop limit
	uint8_t tos;
	uint8_t pmtudisc; // IP_PMTUDISC_*
	uint8_t mc_ttl; // multicast TTL
	uint8_t mc_loop; // loop multicast back when joined
	uint8_t flags; // IPTX_*
	uint8_t _pad;
	uint16_t *idp; // connected socket's ID counter, NULL: none
};
// Send the transport payload at skb (data at the transport header) as an
// IPv4 datagram.  Consumes skb.  `rt' may be NULL (routed here) or an
// already looked-up route (not released here).  0 or -errno; -EMSGSIZE
// when DF is required and the datagram is larger than *mtu_out (set).
int ip_send_skb(sk_buff_t *skb, const struct ip_tx *tx, struct ip_route *rt,
		uint32_t *mtu_out);
// Send a complete datagram (data at the IP header, header final) on a
// route: loopback or neighbour output.  Consumes skb.
int ip_finish_output(struct ip_route *rt, sk_buff_t *skb);
// For TCP: a segment (data at the TCP header) with DF, routed through the
// connection's cache when one is given (an obsolete route is looked up
// again).  `idp': the connection's IP ID counter (NULL: per destination).
// Consumes skb.
int ip_queue_xmit(sk_buff_t *skb, uint32_t saddr, uint32_t daddr,
		  uint8_t proto, uint8_t ttl, uint8_t tos,
		  struct dst_cache *cache, uint16_t *idp);
// The IP identification for a datagram (see ipv4.c).
uint16_t ip_select_ident(uint32_t saddr, uint32_t daddr, uint8_t proto,
			 uint32_t segs);
// The next hop of `rt' confirmed (an upper layer saw progress).
void ip_output_init(void);
void ip_input_init(void);

// ---- ICMP ----
void icmp_init(void);
int icmp_rcv(sk_buff_t *skb);
// Send an ICMP error about the received datagram skb_in (its network
// header set; IPCB valid).  `info' is the 32-bit rest of the ICMP header
// in host order (MTU for FRAG_NEEDED, pointer << 24 for PARAMPROB).
// Applies every "never send" rule and the rate limits.
void icmp_send(sk_buff_t *skb_in, uint8_t type, uint8_t code, uint32_t info);
// errno and severity of an ICMP error (RFC 1122 4.2.3.9 / 4.2.4.1).
int icmp_err_convert(uint8_t type, uint8_t code, int *fatal);
// Path-MTU reporting: lower the exception for daddr; returns the new path
// MTU (>= 552), 0 when nothing changed.  *locked: below the minimum.
uint32_t ip_update_pmtu(uint32_t daddr, uint32_t mtu, int *locked);
// Whether a socket in path-MTU mode `pmtudisc' takes reports about its
// datagrams: INTERFACE and OMIT ignore the path MTU, so a report about
// one of their datagrams changes nothing.  Every other mode does -- for
// the destination of the quoted datagram, which is all a report proves.
static inline int ip_sk_accept_pmtu(uint8_t pmtudisc)
{
	return pmtudisc != IP_PMTUDISC_INTERFACE &&
	       pmtudisc != IP_PMTUDISC_OMIT;
}
struct icmp_rl_stats {
	uint64_t out_msgs, out_errors, rl_global, rl_host;
	uint64_t in_msgs, in_errors, in_csum_errors;
};
void icmp_get_stats(struct icmp_rl_stats *st);
void icmp_rl_reset(void); // tests: refill the buckets

#define ICMP_SOURCE_QUENCH 4
#define ICMP_PARAMETERPROB ICMP_PARAM_PROBLEM
#define ICMP_INFO_REQUEST 15
#define ICMP_INFO_REPLY 16
#define ICMP_EXC_TTL 0
#define ICMP_EXC_FRAGTIME 1
#define ICMP_SR_FAILED 5
#define ICMP_NET_UNKNOWN 6
#define ICMP_HOST_UNKNOWN 7
#define ICMP_HOST_ISOLATED 8
#define ICMP_NET_ANO 9
#define ICMP_HOST_ANO 10
#define ICMP_NET_UNR_TOS 11
#define ICMP_HOST_UNR_TOS 12
#define ICMP_PKT_FILTERED 13
#define ICMP_PREC_VIOLATION 14
#define ICMP_PREC_CUTOFF 15
#define NR_ICMP_UNREACH 15
#define NR_ICMP_TYPES 18

// ---- datagram sockets (UDP and raw) ----
/* What a datagram socket keeps about a queued datagram (skb->cb, which it
 * owns once the datagram is queued: written only then, over the IPCB; the
 * skb's data is the payload: past the UDP header, or the whole IP datagram
 * for a raw socket). */
struct dgram_cb {
	uint32_t saddr; // host order
	uint16_t sport; // host order
	uint8_t ttl;
	uint8_t tos;
	uint32_t daddr;
	int ifindex;
	uint32_t _pad;
};
#define DGRAM_CB(skb) ((struct dgram_cb *)((skb)->cb))
BUILD_BUG_ON(sizeof(struct dgram_cb) > SKB_CB_SIZE);

/* An entry of the error queue (IP_RECVERR): the extended error, who sent
 * it, and where the failed datagram was going; data is its payload. */
#define SO_EE_ORIGIN_NONE 0
#define SO_EE_ORIGIN_LOCAL 1
#define SO_EE_ORIGIN_ICMP 2
struct sock_extended_err {
	uint32_t ee_errno;
	uint8_t ee_origin;
	uint8_t ee_type;
	uint8_t ee_code;
	uint8_t ee_pad;
	uint32_t ee_info;
	uint32_t ee_data;
};
struct sock_exterr_cb {
	struct sock_extended_err ee;
	uint32_t offender; // host order, 0: none
	uint32_t addr; // original destination, host order
	uint16_t port; // original destination port, host order
	uint16_t _pad;
	uint32_t _pad2;
};
#define SKB_EXT_ERR(skb) ((struct sock_exterr_cb *)((skb)->cb))
BUILD_BUG_ON(sizeof(struct sock_exterr_cb) > SKB_CB_SIZE);

// Queue an error on the socket's error queue when it asked for them
// (IP_RECVERR; charged to its receive buffer, dropped when full) and set it
// as the pending error.  Any context; the caller holds no socket lock (a
// table lock is fine).  Wakes the socket unless `nowake' (the caller wakes
// it after dropping its table lock).
void ip_queue_error(net_socket_t *s, int err, uint8_t origin, uint8_t type,
		    uint8_t code, uint32_t info, uint32_t offender,
		    uint32_t daddr, uint16_t dport, const uint8_t *payload,
		    uint32_t plen, int nowake);

// The socket table's slot of s (socket.c), -1 if s is not in it.
int sock_slot(const net_socket_t *s);
// Wake the datagram sockets whose slot bits are set (a bitmap of
// NET_MAX_SOCKETS bits).  No lock held.
void sock_wake_slots(const uint64_t *bits);

// Queue a received datagram on a socket (charging skb->truesize to its
// receive buffer).  Called with the socket's table lock held, takes
// s->lock.  0 (consumed), -1 (refused: the caller still owns skb).
int dgram_enqueue(net_socket_t *s, sk_buff_t *skb);
// Wake whoever waits on the socket (after the table lock is dropped).
void dgram_wake(net_socket_t *s);
// The next datagram, dequeued (uncharged) or, with peek, held and left
// queued.  NULL when there is none.  The caller drops its reference.
sk_buff_t *dgram_dequeue(net_socket_t *s, int peek);
// Length of the next datagram (FIONREAD), 0 when none.
uint32_t dgram_first_len(net_socket_t *s);
// Take (and clear) the pending error, 0 when none.
int dgram_take_error(net_socket_t *s);
// The oldest entry of the error queue (MSG_ERRQUEUE), NULL when empty.
sk_buff_t *dgram_dequeue_err(net_socket_t *s);
// Free what is queued (both queues, or only the error queue).
void dgram_purge(net_socket_t *s, int errq_only);

// UDP (udp.c)
int udp_rcv(sk_buff_t *skb);
void udp_err(const struct icmp_err *e);
int udp_bind(net_socket_t *s, const struct sockaddr_in *addr);
int udp_connect(net_socket_t *s, const struct sockaddr_in *addr);
int udp_sendmsg(net_socket_t *s, const void *buf, size_t len, int flags,
		const struct sockaddr_in *dest);
int udp_setsockopt(net_socket_t *s, int optname, int val);
int udp_getsockopt(net_socket_t *s, int optname, int *val);
void udp_destroy(net_socket_t *s); // unhash, release the cork
// Unhash a raw or UDP socket on close (before its queues are purged).
void udp_unhash(net_socket_t *s);
struct udp_table_stats {
	uint32_t sockets; // hashed
	uint32_t max_chain; // longest port chain
};
void udp_get_table_stats(struct udp_table_stats *st);

#define UDP_CORK 1
#define SOL_UDP_LEVEL 17

// Raw sockets (raw.c)
void raw_init(void);
void raw_hash(net_socket_t *s);
void raw_unhash(net_socket_t *s);
// Give copies of the datagram (data at the transport header, network
// header set) to the raw sockets of `proto'.  1 when any took one.
int raw_local_deliver(sk_buff_t *skb, uint8_t proto);
void raw_icmp_error(const struct icmp_err *e);
int raw_sendmsg(net_socket_t *s, const void *buf, size_t len,
		const struct sockaddr_in *dest);

// Statistics helpers (net_stats_read of the counters in stats.h).
#endif // LIKEOS_NET_IP_H
