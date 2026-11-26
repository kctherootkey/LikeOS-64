/* Kernel-side probes for the IPv4 / ICMP / UDP tests of the network
 * harness (host/net/t_ip.c).
 *
 * Compiled into every stack instance next to the real kernel/net sources
 * (host/test-net.sh); after the per-instance rename the host calls them as
 * n0_kt_*() / n1_kt_*().
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/net.h>
#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/net/fib.h>
#include <kernel/net/skb.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/softirq.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include "kstubs.h"

static const struct {
	const char *name;
	int idx;
} kt_mibs[] = {
	{ "IpInReceives", NET_MIB_IP_INRECEIVES },
	{ "IpInHdrErrors", NET_MIB_IP_INHDRERRORS },
	{ "IpInDelivers", NET_MIB_IP_INDELIVERS },
	{ "IpOutRequests", NET_MIB_IP_OUTREQUESTS },
	{ "IpInTruncatedPkts", NET_MIB_IP_INTRUNCATEDPKTS },
	{ "IpInAddrErrors", NET_MIB_IP_INADDRERRORS },
	{ "IpInUnknownProtos", NET_MIB_IP_INUNKNOWNPROTOS },
	{ "IpInCsumErrors", NET_MIB_IP_INCSUMERRORS },
	{ "IpInMartians", NET_MIB_IP_INMARTIANS },
	{ "IpInSrcRouted", NET_MIB_IP_INSRCROUTED },
	{ "IpInBcastPkts", NET_MIB_IP_INBCASTPKTS },
	{ "IpInMcastPkts", NET_MIB_IP_INMCASTPKTS },
	{ "IpOutNoRoutes", NET_MIB_IP_OUTNOROUTES },
	{ "IpOutDiscards", NET_MIB_IP_OUTDISCARDS },
	{ "IpFragOKs", NET_MIB_IP_FRAGOKS },
	{ "IpFragFails", NET_MIB_IP_FRAGFAILS },
	{ "IpFragCreates", NET_MIB_IP_FRAGCREATES },
	{ "IpReasmReqds", NET_MIB_IP_REASMREQDS },
	{ "IpReasmOKs", NET_MIB_IP_REASMOKS },
	{ "IpReasmFails", NET_MIB_IP_REASMFAILS },
	{ "IpReasmTimeout", NET_MIB_IP_REASMTIMEOUT },
	{ "IpReasmOverlaps", NET_MIB_IP_REASMOVERLAPS },
	{ "IpReasmDups", NET_MIB_IP_REASMDUPS },
	{ "IcmpInMsgs", NET_MIB_ICMP_INMSGS },
	{ "IcmpInErrors", NET_MIB_ICMP_INERRORS },
	{ "IcmpInCsumErrors", NET_MIB_ICMP_INCSUMERRORS },
	{ "IcmpOutMsgs", NET_MIB_ICMP_OUTMSGS },
	{ "IcmpOutRateLimitGlobal", NET_MIB_ICMP_OUTRATELIMITGLOBAL },
	{ "IcmpOutRateLimitHost", NET_MIB_ICMP_OUTRATELIMITHOST },
	{ "IcmpInRedirects", NET_MIB_ICMP_INREDIRECTS },
	{ "IcmpInSrcQuenchs", NET_MIB_ICMP_INSRCQUENCHS },
	{ "IcmpOutParmProbs", NET_MIB_ICMP_OUTPARMPROBS },
	{ "IcmpOutDestUnreachs", NET_MIB_ICMP_OUTDESTUNREACHS },
	{ "IcmpOutTimeExcds", NET_MIB_ICMP_OUTTIMEEXCDS },
	{ "IcmpPmtuUpdates", NET_MIB_ICMP_PMTUUPDATES },
	{ "UdpInDatagrams", NET_MIB_UDP_INDATAGRAMS },
	{ "UdpOutDatagrams", NET_MIB_UDP_OUTDATAGRAMS },
	{ "UdpNoPorts", NET_MIB_UDP_NOPORTS },
	{ "UdpInErrors", NET_MIB_UDP_INERRORS },
	{ "UdpRcvbufErrors", NET_MIB_UDP_RCVBUFERRORS },
	{ "UdpSndbufErrors", NET_MIB_UDP_SNDBUFERRORS },
	{ "UdpInCsumErrors", NET_MIB_UDP_INCSUMERRORS },
	{ "UdpIgnoredMulti", NET_MIB_UDP_IGNOREDMULTI },
	{ "RawDrops", NET_MIB_RAW_DROPS },
};

static int kt_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* A protocol counter by its name; ~0 when there is no such counter. */
unsigned long long kt_mib(const char *name)
{
	for (unsigned i = 0; i < sizeof(kt_mibs) / sizeof(kt_mibs[0]); i++)
		if (kt_streq(kt_mibs[i].name, name))
			return net_stats_read((enum net_mib_idx)kt_mibs[i].idx);
	return ~0ULL;
}

void kt_ipfrag(unsigned *queues, unsigned long long *mem,
	       unsigned long long *evicted, unsigned long long *timeouts)
{
	struct ipfrag_stats st;

	ipfrag_get_stats(&st);
	if (queues)
		*queues = st.queues;
	if (mem)
		*mem = st.mem;
	if (evicted)
		*evicted = st.evicted;
	if (timeouts)
		*timeouts = st.timeouts;
}

void kt_ipfrag_thresh(unsigned long long high, unsigned long long low)
{
	ipfrag_set_thresh(high, low);
}

void kt_icmp_rl_reset(void)
{
	icmp_rl_reset();
}

unsigned kt_pmtu(unsigned daddr)
{
	return fib_exception_get_pmtu(daddr);
}

unsigned kt_udp_hashed(void)
{
	struct udp_table_stats st;

	udp_get_table_stats(&st);
	return st.sockets;
}

/* What recvmsg() returned, cmsgs decoded. */
struct kt_msg {
	int ret;
	int flags; // msg_flags
	unsigned src_ip, src_port;
	int have_pktinfo, pkt_ifindex;
	unsigned pkt_addr, pkt_spec_dst;
	int have_ttl, ttl;
	int have_tos, tos;
	int have_err;
	unsigned ee_errno, ee_origin, ee_type, ee_code, ee_info, offender;
	unsigned controllen;
};

int kt_recvmsg(int sock, void *buf, unsigned len, int flags,
	       struct kt_msg *out)
{
	struct sockaddr_in sa;
	struct iovec iov;
	struct msghdr m;
	uint8_t ctl[256];
	struct cmsghdr *c;
	int r;

	mm_memset(out, 0, sizeof(*out));
	mm_memset(&sa, 0, sizeof(sa));
	mm_memset(ctl, 0, sizeof(ctl));
	iov.iov_base = buf;
	iov.iov_len = len;
	mm_memset(&m, 0, sizeof(m));
	m.msg_name = &sa;
	m.msg_namelen = sizeof(sa);
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl;
	m.msg_controllen = sizeof(ctl);
	r = sock_recvmsg(sock, &m, flags);
	softirq_drain();
	out->ret = r;
	if (r < 0)
		return r;
	out->flags = m.msg_flags;
	out->src_ip = net_ntohl(sa.sin_addr.s_addr);
	out->src_port = net_ntohs(sa.sin_port);
	out->controllen = (unsigned)m.msg_controllen;
	for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
		if (c->cmsg_level != IPPROTO_IP)
			continue;
		if (c->cmsg_type == IP_PKTINFO) {
			struct in_pktinfo pi;

			mm_memcpy(&pi, CMSG_DATA(c), sizeof(pi));
			out->have_pktinfo = 1;
			out->pkt_ifindex = pi.ipi_ifindex;
			out->pkt_addr = net_ntohl(pi.ipi_addr.s_addr);
			out->pkt_spec_dst = net_ntohl(pi.ipi_spec_dst.s_addr);
		} else if (c->cmsg_type == IP_RECVTTL) {
			mm_memcpy(&out->ttl, CMSG_DATA(c), sizeof(int));
			out->have_ttl = 1;
		} else if (c->cmsg_type == IP_RECVTOS) {
			mm_memcpy(&out->tos, CMSG_DATA(c), sizeof(int));
			out->have_tos = 1;
		} else if (c->cmsg_type == IP_RECVERR) {
			struct sock_extended_err ee;
			struct sockaddr_in off;

			mm_memcpy(&ee, CMSG_DATA(c), sizeof(ee));
			mm_memcpy(&off, CMSG_DATA(c) + sizeof(ee), sizeof(off));
			out->have_err = 1;
			out->ee_errno = ee.ee_errno;
			out->ee_origin = ee.ee_origin;
			out->ee_type = ee.ee_type;
			out->ee_code = ee.ee_code;
			out->ee_info = ee.ee_info;
			out->offender = net_ntohl(off.sin_addr.s_addr);
		}
	}
	return r;
}

/* sendmsg() of one buffer to ip:port (0: connected) with flags. */
int kt_sendmsg(int sock, const void *buf, unsigned len, int flags, unsigned ip,
	       unsigned port)
{
	struct sockaddr_in sa;
	struct iovec iov;
	struct msghdr m;
	int r;

	mm_memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = net_htons((uint16_t)port);
	sa.sin_addr.s_addr = net_htonl(ip);
	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	mm_memset(&m, 0, sizeof(m));
	if (ip || port) {
		m.msg_name = &sa;
		m.msg_namelen = sizeof(sa);
	}
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	r = sock_sendmsg(sock, &m, flags);
	softirq_drain();
	return r;
}

/* A raw socket of `proto'. */
int kt_raw_socket(int proto)
{
	int r = sock_create(AF_INET, SOCK_RAW, proto);

	softirq_drain();
	return r;
}

/* connect() with AF_UNSPEC. */
int kt_disconnect(int sock)
{
	struct sockaddr_in sa;
	int r;

	mm_memset(&sa, 0, sizeof(sa));
	r = sock_connect(sock, &sa);
	softirq_drain();
	return r;
}

/* Join `group' (on the interface with address ifaddr, 0: default). */
int kt_mc_join(int sock, unsigned group, unsigned ifaddr, int drop)
{
	struct ip_mreq mr;

	mr.imr_multiaddr.s_addr = net_htonl(group);
	mr.imr_interface.s_addr = net_htonl(ifaddr);
	int r = sock_setsockopt(sock, IPPROTO_IP,
				drop ? IP_DROP_MEMBERSHIP : IP_ADD_MEMBERSHIP,
				&mr, sizeof(mr));
	softirq_drain();
	return r;
}

/* A received frame handed to the protocols as a driver with receive
 * checksum offload would: an skb, CHECKSUM_UNNECESSARY when `verified'. */
int kt_rx_skb(const void *frame, unsigned len, int verified)
{
	net_device_t *dev = net_get_default_device();
	sk_buff_t *skb;

	if (!dev)
		return -1;
	skb = skb_alloc(len);
	if (!skb)
		return -1;
	mm_memcpy(skb_append(skb, len), frame, len);
	skb->dev = dev;
	skb->ip_summed = verified ? CHECKSUM_UNNECESSARY : CHECKSUM_NONE;
	netif_receive_skb(skb);
	softirq_drain();
	return 0;
}

/* Packet memory in use (truesize of every live skb). */
unsigned long long kt_skb_inuse(void)
{
	skb_mem_stats_t st;

	skb_get_mem_stats(&st);
	return st.inuse_bytes;
}

/* Counters of the instance's NIC: 0 frames from a group source address,
 * 1 rx_dropped, 2 multicast list entries, 3 users of the first entry. */
unsigned long long kt_dev_stat(int which)
{
	net_device_t *dev = net_get_default_device();

	switch (which) {
	case 0:
		return dev->rx_bad_src;
	case 1:
		return dev->rx_dropped;
	case 2:
		return dev->mc.count;
	case 3:
		return dev->mc.count ? dev->mc.a[0].users : 0;
	default:
		return ~0ULL;
	}
}

/* The NIC's address (0: none, as before a DHCP lease). */
void kt_dev_addr(unsigned ip)
{
	net_device_t *dev = net_get_default_device();

	dev->ip_addr = ip;
}

/* A clone handed to IP output while the original keeps the data: the
 * original's headroom is not written.  0, or the failing step. */
int kt_clone_send(unsigned daddr)
{
	sk_buff_t *skb = skb_alloc(100), *c;
	struct ip_tx tx;
	uint8_t *mark;
	int rc;

	if (!skb)
		return 1;
	mm_memset(skb_append(skb, 100), 0x5A, 100);
	mark = skb->data - 64;
	mm_memset(mark, 0xA5, 64);
	c = skb_clone(skb);
	if (!c) {
		skb_put(skb);
		return 2;
	}
	mm_memset(&tx, 0, sizeof(tx));
	tx.daddr = daddr;
	tx.proto = 253; // experimental
	tx.pmtudisc = IP_PMTUDISC_WANT;
	rc = ip_send_skb(c, &tx, NULL, NULL);
	softirq_drain();
	for (int i = 0; i < 64; i++) {
		if (mark[i] != 0xA5) {
			skb_put(skb);
			return 3;
		}
	}
	for (unsigned i = 0; i < 100; i++) {
		if (skb->data[i] != 0x5A) {
			skb_put(skb);
			return 4;
		}
	}
	skb_put(skb);
	return rc < 0 ? 5 : 0;
}
