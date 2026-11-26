// LikeOS ARP (Address Resolution Protocol, RFC 826) on the neighbour
// subsystem
//
// The IPv4 neighbour table (arp_tbl) with its solicitation, error report
// and constructor, the processing of received ARP packets, and the
// administrative and diagnostic interfaces (ioctls, arping, netstat).
//
// Learning: a request for one of our addresses creates (or refreshes) the
// sender's entry; any other ARP packet only refreshes an entry that exists
// -- to REACHABLE for a reply addressed to us, to STALE otherwise -- and a
// different address replaces the known one only after the entry's lock
// time (or from a gratuitous announcement).  Ordinary IP traffic teaches
// the table nothing.  A packet that claims one of our own addresses from
// another station is an address conflict: counted, never learned, and our
// address is defended with a gratuitous ARP at most every 10 seconds
// (RFC 5227 2.4).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/neighbour.h>
#include <kernel/net/netdevice.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/net/skb.h>
#include <kernel/net/ratelimit.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

#define ARP_DEFEND_INTERVAL_MS 10000

static void arp_solicit(neighbour_t *n, uint32_t saddr, int unicast,
			const uint8_t ha[NEIGH_HALEN]);
static void arp_error_report(neighbour_t *n, sk_buff_t *skb);
static void arp_constructor(neighbour_t *n);

static const struct neigh_ops arp_ops = {
	.solicit = arp_solicit,
	.error_report = arp_error_report,
	.constructor = arp_constructor,
};

struct neigh_table arp_tbl = {
	.id = "arp_tbl",
	.parms = {
		.base_reachable_time_ms = 30000,
		.retrans_time_ms = 1000,
		.delay_probe_time_ms = 5000,
		.gc_staletime_ms = 60000,
		.locktime_ms = 1000,
		.ucast_probes = 3,
		.mcast_probes = 3,
		.app_probes = 0,
		.mcast_reprobes = 0,
		.queue_len_bytes = 212992,
	},
	.ops = &arp_ops,
	.gc_thresh1 = 128,
	.gc_thresh2 = 512,
	.gc_thresh3 = 1024,
	.gc_interval_ms = 30000,
};

static volatile uint64_t arp_conflicts;
static uint64_t arp_last_defend;

static inline int ip_is_multicast(uint32_t ip)
{
	return (ip & 0xF0000000U) == 0xE0000000U;
}

static inline int ip_is_loopback(uint32_t ip)
{
	return (ip & 0xFF000000U) == 0x7F000000U;
}

static inline int mac_is_zero(const uint8_t *m)
{
	return !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static inline uint32_t rd32be(const void *p)
{
	const uint8_t *b = p;

	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | b[3];
}

// ============================================================================
// Sending
// ============================================================================

/* One ARP packet: `frame_dst' is the Ethernet destination (NULL:
 * broadcast); the body carries sender (sip, sha) and target (tip, tha;
 * NULL: zeros). */
static int arp_send(net_device_t *dev, uint16_t op, const uint8_t *frame_dst,
		    uint32_t sip, const uint8_t *sha, uint32_t tip,
		    const uint8_t *tha)
{
	uint8_t pkt[sizeof(arp_header_t)];
	arp_header_t *arp = (arp_header_t *)pkt;

	BUILD_BUG_ON(sizeof(arp_header_t) != 28);
	if (!dev)
		return -1;
	arp->hw_type = net_htons(ARP_HW_ETHER);
	arp->proto_type = net_htons(ETH_P_IP);
	arp->hw_len = ETH_ALEN;
	arp->proto_len = 4;
	arp->opcode = net_htons(op);
	mm_memcpy(arp->sender_mac, sha ? sha : dev->mac_addr, ETH_ALEN);
	arp->sender_ip = net_htonl(sip);
	if (tha)
		mm_memcpy(arp->target_mac, tha, ETH_ALEN);
	else
		mm_memset(arp->target_mac, 0, ETH_ALEN);
	arp->target_ip = net_htonl(tip);
	return eth_send(dev, frame_dst ? frame_dst : eth_broadcast_addr,
			ETH_P_ARP, pkt, sizeof(arp_header_t));
}

void arp_request(net_device_t *dev, uint32_t target_ip)
{
	BUG_ON(dev == NULL);
	arp_send(dev, ARP_OP_REQUEST, NULL, dev->ip_addr, NULL, target_ip,
		 NULL);
}

void arp_gratuitous(net_device_t *dev, uint32_t ip)
{
	BUG_ON(dev == NULL);
	if (!dev)
		return;
	/* An announcement (RFC 5227 3): a reply with sender and target both
	 * the address, to everyone. */
	arp_send(dev, ARP_OP_REPLY, NULL, ip, NULL, ip, eth_broadcast_addr);
}

// ============================================================================
// Neighbour ops
// ============================================================================

static void arp_constructor(neighbour_t *n)
{
	net_device_t *dev = n->dev;
	uint32_t key = n->key;

	if (!dev || dev == net_get_loopback() || (dev->flags & IFF_NOARP)) {
		n->nud_state = NUD_NOARP;
		mm_memset(n->ha, 0, NEIGH_HALEN);
	} else if (key == 0xFFFFFFFFU ||
		   (dev->netmask && dev->netmask != 0xFFFFFFFFU &&
		    key == (dev->ip_addr | ~dev->netmask))) {
		n->nud_state = NUD_NOARP;
		mm_memset(n->ha, 0xFF, NEIGH_HALEN);
	} else if (ip_is_multicast(key)) {
		// RFC 1112: 01:00:5e + the low 23 bits of the group
		n->nud_state = NUD_NOARP;
		n->ha[0] = 0x01;
		n->ha[1] = 0x00;
		n->ha[2] = 0x5E;
		n->ha[3] = (uint8_t)((key >> 16) & 0x7F);
		n->ha[4] = (uint8_t)(key >> 8);
		n->ha[5] = (uint8_t)key;
	}
}

static void arp_solicit(neighbour_t *n, uint32_t saddr, int unicast,
			const uint8_t ha[NEIGH_HALEN])
{
	net_device_t *dev = n->dev;

	/* The waiting packet's source when it is the device's address, the
	 * device's address otherwise. */
	if (!saddr || saddr != dev->ip_addr)
		saddr = dev->ip_addr;
	arp_send(dev, ARP_OP_REQUEST, unicast ? ha : NULL, saddr, NULL,
		 n->key, NULL);
}

/* Resolution failed for a queued datagram: a locally sent one is answered
 * like a router would answer it, with ICMP host unreachable to its sender
 * -- here directly to the socket it came from. */
static void arp_error_report(neighbour_t *n, sk_buff_t *skb)
{
	(void)n;
	if (skb->len >= sizeof(ipv4_header_t) + 8) {
		const ipv4_header_t *ip = (const ipv4_header_t *)skb->data;
		uint32_t src = net_ntohl(ip->src_addr);
		int local = 0;

		for (int i = 0; i < net_device_count(); i++) {
			net_device_t *d = net_get_device(i);

			if (d && d->ip_addr && d->ip_addr == src)
				local = 1;
		}
		if (local)
			icmp_dispatch_error(ICMP_DEST_UNREACH,
					    ICMP_HOST_UNREACH, skb->data,
					    (uint16_t)(skb->len > 0xFFFF ?
								0xFFFF :
								skb->len),
					    0);
	}
	skb_put(skb);
}

// ============================================================================
// IPv4 output and confirmation
// ============================================================================

int ip_neigh_output(net_device_t *dev, uint32_t nexthop, sk_buff_t *skb)
{
	neighbour_t *n = __neigh_lookup(&arp_tbl, nexthop, dev, 1);
	int rc;

	if (!n) {
		dev->tx_dropped++;
		skb_put(skb);
		return -1;
	}
	rc = neigh_output(n, skb);
	neigh_release(n);
	return rc;
}

void ip_neigh_confirm(net_device_t *dev, uint32_t nexthop)
{
	neighbour_t *n = neigh_lookup(&arp_tbl, nexthop, dev);

	if (n) {
		neigh_confirm(n);
		neigh_release(n);
	}
}

// ============================================================================
// Receiving
// ============================================================================

// Blocking ARP request/reply for arping, and the RFC 5227 probe.
static volatile uint32_t arp_reply_ip = 0;
static volatile uint8_t arp_reply_mac[ETH_ALEN];
static volatile int arp_reply_ready = 0;
static volatile uint32_t arp_probe_ip; // address being probed, 0: none
static volatile int arp_probe_conflict;

static void arp_note_conflict(net_device_t *dev, uint32_t ip)
{
	uint64_t now = timer_ticks();

	__atomic_fetch_add(&arp_conflicts, 1, __ATOMIC_RELAXED);
	/* Defend the address (RFC 5227 2.4 (b)): one announcement, and
	 * none again within the defend interval. */
	if (!arp_last_defend ||
	    now - arp_last_defend >= net_ms_to_ticks(ARP_DEFEND_INTERVAL_MS)) {
		arp_last_defend = now ? now : 1;
		arp_gratuitous(dev, ip);
	}
}

void arp_rcv(net_device_t *dev, const uint8_t *data, uint16_t len,
	     uint8_t pkt_type)
{
	BUG_ON(dev == NULL);
	BUG_ON(data == NULL);
	if (len < sizeof(arp_header_t))
		return;

	const arp_header_t *arp = (const arp_header_t *)data;
	if (net_ntohs(arp->hw_type) != ARP_HW_ETHER ||
	    net_ntohs(arp->proto_type) != ETH_P_IP || arp->hw_len != ETH_ALEN ||
	    arp->proto_len != 4)
		return;
	uint16_t op = net_ntohs(arp->opcode);
	if (op != ARP_OP_REQUEST && op != ARP_OP_REPLY)
		return;
	if (pkt_type == PACKET_OTHERHOST || pkt_type == PACKET_LOOPBACK ||
	    dev == net_get_loopback())
		return;

	uint32_t sip = rd32be(&arp->sender_ip);
	uint32_t tip = rd32be(&arp->target_ip);
	const uint8_t *sha = arp->sender_mac;
	const uint8_t *tha = arp->target_mac;
	uint32_t our = dev->ip_addr;

	if (ip_is_multicast(tip) || ip_is_loopback(tip))
		return;
	if (sha[0] & 0x01)
		return; // a group address cannot be a station's address
	if (ip_is_loopback(sip) || ip_is_multicast(sip))
		return;
	{
		uint64_t rl_flags;
		spin_lock_irqsave(&g_ratelimit_lock, &rl_flags);
		int arp_src_ok = net_rl_src_allow(&g_arp_src_rl, sip);
		spin_unlock_irqrestore(&g_ratelimit_lock, rl_flags);
		if (!arp_src_ok)
			return;
	}

	// RFC 5227: another station probing or using the address we probe
	if (arp_probe_ip && kmemcmp(sha, dev->mac_addr, ETH_ALEN) != 0 &&
	    (sip == arp_probe_ip || (sip == 0 && tip == arp_probe_ip)))
		arp_probe_conflict = 1;

	if (sip == 0) {
		/* Duplicate-address probe: answer one for our address so the
		 * prober backs off; learn nothing. */
		if (op == ARP_OP_REQUEST && our && tip == our) {
			uint64_t rl_flags;
			spin_lock_irqsave(&g_ratelimit_lock, &rl_flags);
			int reply_ok = net_rl_allow(&g_arp_reply_rl);
			spin_unlock_irqrestore(&g_ratelimit_lock, rl_flags);
			if (reply_ok)
				arp_send(dev, ARP_OP_REPLY, sha, tip, NULL, 0,
					 sha);
		}
		return;
	}

	if (our && sip == our) {
		if (kmemcmp(sha, dev->mac_addr, ETH_ALEN) != 0)
			arp_note_conflict(dev, our);
		return; // never learn our own address
	}

	if (op == ARP_OP_REQUEST && our && tip == our) {
		/* For us: the sender is a neighbour now.  New entries are
		 * rate limited (a flood of requests from made-up senders
		 * would otherwise churn the table). */
		neighbour_t *n = neigh_lookup(&arp_tbl, sip, dev);

		if (n) {
			neigh_update(n, sha, NUD_STALE,
				     NEIGH_UPDATE_F_OVERRIDE);
		} else {
			uint64_t rl_flags;
			spin_lock_irqsave(&g_ratelimit_lock, &rl_flags);
			int new_ok = net_rl_src_allow(&g_arp_new_rl, sip);
			spin_unlock_irqrestore(&g_ratelimit_lock, rl_flags);
			if (new_ok)
				n = neigh_event_ns(&arp_tbl, sha, sip, dev);
		}
		if (n) {
			arp_tbl.stats.rcv_probes_mcast +=
				pkt_type != PACKET_HOST;
			arp_tbl.stats.rcv_probes_ucast +=
				pkt_type == PACKET_HOST;
			neigh_release(n);
		}
		uint64_t rl_flags;
		spin_lock_irqsave(&g_ratelimit_lock, &rl_flags);
		int reply_ok = net_rl_allow(&g_arp_reply_rl);
		spin_unlock_irqrestore(&g_ratelimit_lock, rl_flags);
		if (reply_ok)
			arp_send(dev, ARP_OP_REPLY, sha, our, NULL, sip, sha);
	} else {
		/* Anything else refreshes an entry we have.  A gratuitous
		 * announcement (sender == target address) may replace the
		 * address at once; otherwise only after the lock time, so a
		 * second, late reply does not flip the entry. */
		neighbour_t *n = neigh_lookup(&arp_tbl, sip, dev);

		if (n) {
			int is_garp = tip == sip &&
				      (op == ARP_OP_REQUEST ||
				       mac_is_zero(tha) ||
				       kmemcmp(tha, eth_broadcast_addr,
					       ETH_ALEN) == 0 ||
				       kmemcmp(tha, sha, ETH_ALEN) == 0);
			uint8_t state = (op == ARP_OP_REPLY &&
					 pkt_type == PACKET_HOST) ?
						NUD_REACHABLE :
						NUD_STALE;
			int override =
				is_garp ||
				(int64_t)(timer_ticks() -
					  (n->updated +
					   net_ms_to_ticks(
						   arp_tbl.parms.locktime_ms))) >=
					0;

			neigh_update(n, sha, state,
				     override ? NEIGH_UPDATE_F_OVERRIDE : 0);
			neigh_release(n);
		}
	}

	// Wake anyone waiting for an ARP reply (arping)
	if (op == ARP_OP_REPLY) {
		arp_reply_ip = sip;
		for (int i = 0; i < ETH_ALEN; i++)
			arp_reply_mac[i] = sha[i];
		arp_reply_ready = 1;
		sched_wake_channel((void *)&arp_reply_ready);
	}
}

void arp_rx(net_device_t *dev, const uint8_t *data, uint16_t len)
{
	arp_rcv(dev, data, len, PACKET_HOST);
}

// ============================================================================
// Table interfaces
// ============================================================================

/* Link events: entries learned over a link that went away cannot be
 * trusted when it comes back (another network, another gateway). */
static void arp_netdev_event(struct netdev_notifier *nb, unsigned long ev,
			     net_device_t *dev)
{
	(void)nb;
	switch (ev) {
	case NETDEV_CHANGE:
		if (!netif_carrier_ok(dev))
			neigh_flush_dev(&arp_tbl, dev, 1);
		break;
	case NETDEV_CHANGEADDR:
		neigh_flush_dev(&arp_tbl, dev, 1);
		break;
	case NETDEV_DOWN:
		neigh_flush_dev(&arp_tbl, dev, 0);
		break;
	default:
		break;
	}
}

static struct netdev_notifier arp_notifier = {
	.fn = arp_netdev_event,
};

void arp_init(void)
{
	neigh_table_init(&arp_tbl);
	register_netdevice_notifier(&arp_notifier);
}

// Lookup MAC for an IP. Returns 0 on success, -1 if not resolved (and a
// resolution is started).
int arp_resolve(net_device_t *dev, uint32_t ip, uint8_t mac_out[ETH_ALEN])
{
	neighbour_t *n;
	int rc = -1;

	// Broadcast address
	if (ip == 0xFFFFFFFF || ip == (dev->ip_addr | ~dev->netmask)) {
		for (int i = 0; i < ETH_ALEN; i++)
			mac_out[i] = 0xFF;
		return 0;
	}
	n = __neigh_lookup(&arp_tbl, ip, dev, 1);
	if (!n)
		return -1;
	if (n->nud_state & NUD_VALID) {
		uint64_t f;

		spin_lock_irqsave(&n->lock, &f);
		mm_memcpy(mac_out, n->ha, ETH_ALEN);
		spin_unlock_irqrestore(&n->lock, f);
		rc = 0;
	} else {
		neigh_event_send(n, NULL);
	}
	neigh_release(n);
	return rc;
}

// Cache-only lookup (no request sent on a miss).  0 on hit, -1 on miss.
int arp_cache_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN])
{
	for (int i = 0; i < net_device_count(); i++) {
		net_device_t *d = net_get_device(i);
		neighbour_t *nb = d ? neigh_lookup(&arp_tbl, ip, d) : NULL;
		int hit = 0;

		if (!nb)
			continue;
		if (nb->nud_state & NUD_VALID) {
			uint64_t f;

			spin_lock_irqsave(&nb->lock, &f);
			mm_memcpy(mac_out, nb->ha, ETH_ALEN);
			spin_unlock_irqrestore(&nb->lock, f);
			hit = 1;
		}
		neigh_release(nb);
		if (hit)
			return 0;
	}
	return -1;
}

// Queue an IPv4 datagram (skb->data at the IP header) for `next_hop_ip'.
// Takes ownership of skb and returns 0; -1 (skb not taken) on bad input.
int arp_queue_pending(net_device_t *dev, uint32_t next_hop_ip, sk_buff_t *skb)
{
	if (!dev || !skb)
		return -1;
	skb->dev = dev;
	ip_neigh_output(dev, next_hop_ip, skb);
	return 0;
}

/* The device an administrative entry for `ip' belongs to: the one the
 * address is routed through, else the default device. */
static net_device_t *arp_admin_dev(uint32_t ip)
{
	uint32_t nh = ip;
	net_device_t *dev = route_lookup(ip, &nh);

	if (!dev || dev == net_get_loopback())
		dev = net_get_default_device();
	return dev;
}

int arp_set_entry(uint32_t ip, const uint8_t mac[ETH_ALEN], int permanent)
{
	net_device_t *dev = arp_admin_dev(ip);
	neighbour_t *n;

	if (!dev)
		return -1;
	n = __neigh_lookup(&arp_tbl, ip, dev, 1);
	if (!n)
		return -1;
	neigh_update(n, mac, permanent ? NUD_PERMANENT : NUD_STALE,
		     NEIGH_UPDATE_F_OVERRIDE | NEIGH_UPDATE_F_ADMIN);
	neigh_release(n);
	return 0;
}

// A static (permanent) entry: SIOCSARP with ATF_PERM, and the tests.
void arp_add_entry(uint32_t ip, const uint8_t mac[ETH_ALEN])
{
	arp_set_entry(ip, mac, 1);
}

// `arp -d': remove the entry for `ip' on every device.
void arp_del_entry(uint32_t ip)
{
	neigh_delete(&arp_tbl, ip, NULL);
}

// The resolved entries, for netstat / arp.
int net_get_arp_table(net_arp_info_t *entries, int max_entries)
{
	struct neigh_dump_entry buf[16];
	int count = 0;

	BUG_ON(max_entries < 0);
	// In chunks of 16 (small stack); the table may change in between.
	while (count < max_entries) {
		int want = max_entries - count;
		int got;

		if (want > 16)
			want = 16;
		got = neigh_dump(&arp_tbl, buf, want, NUD_VALID, count);
		for (int i = 0; i < got; i++) {
			entries[count + i].ip = buf[i].key;
			mm_memcpy(entries[count + i].mac, buf[i].ha, ETH_ALEN);
			entries[count + i].valid = 1;
			entries[count + i].pad = 0;
		}
		count += got;
		if (got < want)
			break;
	}
	return count;
}

uint64_t arp_get_conflicts(void)
{
	return __atomic_load_n(&arp_conflicts, __ATOMIC_RELAXED);
}

// Blocking ARP request/reply for arping
int arp_send_request(net_device_t *dev, uint32_t target_ip)
{
	arp_reply_ready = 0;
	arp_reply_ip = 0;
	arp_request(dev, target_ip);
	return 0;
}

int arp_recv_reply(uint32_t target_ip, uint8_t mac_out[6],
		   uint64_t timeout_ticks)
{
	uint64_t start = timer_ticks();
	task_t *cur = sched_current();
	while (!arp_reply_ready || arp_reply_ip != target_ip) {
		if (timer_ticks() - start > timeout_ticks)
			return -1;

		cur->wait_channel = (void *)&arp_reply_ready;
		cur->wakeup_tick = start + timeout_ticks;
		task_set_blocked(cur);
		sched_schedule();
		cur->wait_channel = NULL;
		cur->wakeup_tick = 0;
	}
	for (int i = 0; i < ETH_ALEN; i++)
		mac_out[i] = arp_reply_mac[i];
	arp_reply_ready = 0;
	return 0;
}

// ============================================================================
// RFC 5227 ARP probe (DAD).  Sends 3 probes ~200ms apart with sender_ip=0.
// Returns 0 if no conflict was seen, -1 if another station uses or probes
// for `ip'.
// ============================================================================
int arp_probe(net_device_t *dev, uint32_t ip)
{
	if (!dev)
		return -1;

	arp_probe_conflict = 0;
	arp_probe_ip = ip;
	for (int probe = 0; probe < 3; probe++) {
		arp_send(dev, ARP_OP_REQUEST, NULL, 0, NULL, ip, NULL);
		// Wait ~200ms for any reply, at the measured tick rate.
		uint64_t deadline = timer_ticks() + timer_ms_to_ticks(200);
		while (timer_ticks() < deadline) {
			if (arp_probe_conflict) {
				arp_probe_ip = 0;
				return -1;
			}
			__asm__ volatile("pause");
		}
	}
	arp_probe_ip = 0;
	return 0;
}
