/* One stack instance as the host sees it (ns_ops, host/net/api-host.h).
 *
 * Kernel side of the network harness (host/test-net.sh): compiled into
 * every instance next to the real kernel/net sources and kstubs.c.  The
 * instance gets one simulated NIC whose transmit goes to the simulator's
 * wire (hs_wire_tx) and whose receive is the kernel's own net_rx_packet.
 * Every entry point runs the softirqs it raised before it returns, so the
 * host always sees the instance quiescent between two calls.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/net.h>
#include <kernel/net/softirq.h>
#include <kernel/net/skb.h>
#include <kernel/net/netdevice.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include "kstubs.h"

static net_device_t sim_dev;
static net_socket_t *sock_base; /* &sockets[0] of socket.c */

/* Which socket a poll queue belongs to (-1: none of ours). */
int glue_sock_index(struct wait_queue_head *h)
{
	uintptr_t base = (uintptr_t)sock_base;
	uintptr_t s = (uintptr_t)h - __builtin_offsetof(net_socket_t, poll_wq);

	if (!sock_base || s < base ||
	    s >= base + NET_MAX_SOCKETS * sizeof(net_socket_t) ||
	    (s - base) % sizeof(net_socket_t))
		return -1;
	return (int)((s - base) / sizeof(net_socket_t));
}

static uint64_t sim_tx_frames, sim_rx_frames;

static int sim_send(net_device_t *dev, const uint8_t *data, uint16_t len)
{
	dev->tx_packets++;
	dev->tx_bytes += len;
	sim_tx_frames++;
	hs_wire_tx(g_inst, data, len);
	return 0;
}

static int sim_link_status(net_device_t *dev)
{
	(void)dev;
	return 1;
}

static void g_pump(void)
{
	softirq_drain();
}

static int g_init(int inst, const struct ns_cfg *cfg)
{
	static char name[8];

	g_inst = inst;
	test_task_init();
	test_quiet = 1;
	net_init();
	test_quiet = 0;

	name[0] = 'e';
	name[1] = 't';
	name[2] = 'h';
	name[3] = '0';
	name[4] = 0;
	mm_memset(&sim_dev, 0, sizeof(sim_dev));
	sim_dev.name = name;
	for (int i = 0; i < ETH_ALEN; i++)
		sim_dev.mac_addr[i] = cfg->mac[i];
	sim_dev.mtu = (uint16_t)(cfg->mtu ? cfg->mtu : NET_MTU_DEFAULT);
	sim_dev.ip_addr = cfg->ip;
	sim_dev.netmask = cfg->netmask;
	sim_dev.gateway = cfg->gateway;
	sim_dev.send = sim_send;
	sim_dev.link_status = sim_link_status;
	/* every NIC driver polls through NAPI, which offers GRO */
	sim_dev.features = NETIF_F_GRO;
	if (cfg->gro_off)
		net_gro_enabled = 0;
	spinlock_init(&sim_dev.lock, "simnic");
	if (net_register(&sim_dev) < 0)
		return -1;
	/* what a DHCP lease sets up (dhcp.c): the on-link subnet and the
	 * default route through the gateway */
	route_add(cfg->ip & cfg->netmask, cfg->netmask, 0, &sim_dev, 0, RTF_UP);
	if (cfg->gateway)
		route_add(0, 0, cfg->gateway, &sim_dev, 0,
			  RTF_UP | RTF_GATEWAY);
	g_pump();
	return 0;
}

static void g_rx(const void *frame, unsigned len)
{
	sim_rx_frames++;
	net_rx_packet(&sim_dev, frame, (uint16_t)len);
	g_pump();
}

static void g_rx_nopump(const void *frame, unsigned len)
{
	sim_rx_frames++;
	net_rx_packet(&sim_dev, frame, (uint16_t)len);
}

static void g_tick(void)
{
	net_timer_tick();
	g_pump();
}

static int g_socket(int type)
{
	int r = sock_create(AF_INET, type, type == SOCK_RAW ? IPPROTO_ICMP : 0);
	if (r >= 0 && !sock_base)
		sock_base = sock_get(r) - r;
	g_pump();
	return r;
}

static int g_set_nonblock(int s, int on)
{
	int fl = sock_fcntl_net(s, F_GETFL, 0);

	if (fl < 0)
		return fl;
	fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
	return sock_fcntl_net(s, F_SETFL, (unsigned long)fl);
}

static void mk_addr(struct sockaddr_in *a, unsigned ip, unsigned port)
{
	mm_memset(a, 0, sizeof(*a));
	a->sin_family = AF_INET;
	a->sin_port = net_htons((uint16_t)port);
	a->sin_addr.s_addr = net_htonl(ip);
}

static int g_bind(int s, unsigned ip, unsigned port)
{
	struct sockaddr_in a;

	mk_addr(&a, ip, port);
	int r = sock_bind(s, &a);
	g_pump();
	return r;
}

static int g_listen(int s, int backlog)
{
	int r = sock_listen(s, backlog);
	g_pump();
	return r;
}

static int g_accept(int s, unsigned *ip, unsigned *port)
{
	struct sockaddr_in a;
	socklen_t len = sizeof(a);

	mm_memset(&a, 0, sizeof(a));
	int r = sock_accept(s, &a, &len);
	g_pump();
	if (r >= 0) {
		if (ip)
			*ip = net_ntohl(a.sin_addr.s_addr);
		if (port)
			*port = net_ntohs(a.sin_port);
	}
	return r;
}

static int g_connect(int s, unsigned ip, unsigned port)
{
	struct sockaddr_in a;

	mk_addr(&a, ip, port);
	int r = sock_connect(s, &a);
	g_pump();
	return r;
}

static int g_send(int s, const void *buf, unsigned long len, int flags)
{
	int r = sock_send(s, buf, len, flags);
	g_pump();
	return r;
}

static int g_recv(int s, void *buf, unsigned long len, int flags)
{
	int r = sock_recv(s, buf, len, flags);
	g_pump();
	return r;
}

static int g_sendto(int s, const void *buf, unsigned long len, int flags,
		    unsigned ip, unsigned port)
{
	struct sockaddr_in a;

	mk_addr(&a, ip, port);
	int r = sock_sendto(s, buf, len, flags, &a, sizeof(a));
	g_pump();
	return r;
}

static int g_recvfrom(int s, void *buf, unsigned long len, int flags,
		      unsigned *ip, unsigned *port)
{
	struct sockaddr_in a;
	socklen_t alen = sizeof(a);

	mm_memset(&a, 0, sizeof(a));
	int r = sock_recvfrom(s, buf, len, flags, &a, &alen);
	g_pump();
	if (r >= 0) {
		if (ip)
			*ip = net_ntohl(a.sin_addr.s_addr);
		if (port)
			*port = net_ntohs(a.sin_port);
	}
	return r;
}

static int g_close(int s)
{
	int r = sock_close(s);
	g_pump();
	return r;
}

static int g_shutdown(int s, int how)
{
	int r = sock_shutdown(s, how);
	g_pump();
	return r;
}

static int g_setsockopt(int s, int level, int name, const void *val,
			unsigned len)
{
	int r = sock_setsockopt(s, level, name, val, len);
	g_pump();
	return r;
}

static int g_getsockopt(int s, int level, int name, void *val, unsigned *len)
{
	socklen_t l = *len;
	int r = sock_getsockopt(s, level, name, val, &l);
	*len = l;
	return r;
}

static int g_poll(int s, int events)
{
	return sock_poll(s, (short)events);
}

static int g_ioctl(int s, unsigned long req, void *arg)
{
	int r = sock_ioctl_net(s, req, arg);
	g_pump();
	return r;
}

static int g_sockname(int s, int peer, unsigned *ip, unsigned *port)
{
	struct sockaddr_in a;
	socklen_t len = sizeof(a);
	int r;

	mm_memset(&a, 0, sizeof(a));
	r = peer ? sock_getpeername(s, &a, &len) :
		   sock_getsockname(s, &a, &len);
	if (r >= 0) {
		if (ip)
			*ip = net_ntohl(a.sin_addr.s_addr);
		if (port)
			*port = net_ntohs(a.sin_port);
	}
	return r;
}

static int g_tcpinfo(int s, struct ns_tcpinfo *out)
{
	struct tcp_info ti;
	socklen_t len = sizeof(ti);
	int r;

	mm_memset(&ti, 0, sizeof(ti));
	r = sock_getsockopt(s, IPPROTO_TCP, TCP_INFO, &ti, &len);
	if (r < 0)
		return r;
	{
		/* tcpi_state is in the ABI's numbering; the harness speaks
		 * TCP_STATE_* */
		static const int k[12] = { -1,
					   TCP_STATE_ESTABLISHED,
					   TCP_STATE_SYN_SENT,
					   TCP_STATE_SYN_RECEIVED,
					   TCP_STATE_FIN_WAIT_1,
					   TCP_STATE_FIN_WAIT_2,
					   TCP_STATE_TIME_WAIT,
					   TCP_STATE_CLOSED,
					   TCP_STATE_CLOSE_WAIT,
					   TCP_STATE_LAST_ACK,
					   TCP_STATE_LISTEN,
					   TCP_STATE_CLOSING };

		out->state = ti.tcpi_state < 12 ? k[ti.tcpi_state] : -1;
	}
	out->rto_us = ti.tcpi_rto;
	out->rtt_us = ti.tcpi_rtt;
	out->rttvar_us = ti.tcpi_rttvar;
	out->snd_cwnd = ti.tcpi_snd_cwnd;
	out->snd_ssthresh = ti.tcpi_snd_ssthresh;
	out->snd_mss = ti.tcpi_snd_mss;
	out->rcv_mss = ti.tcpi_rcv_mss;
	out->unacked = ti.tcpi_unacked;
	out->total_retrans = ti.tcpi_total_retrans;
	return 0;
}

static int g_tcp_state(int s)
{
	net_socket_t *so = sock_get(s);

	if (!so || !so->tcp)
		return -1;
	return so->tcp->state;
}

static int g_send_oob(int s, unsigned char byte)
{
	net_socket_t *so = sock_get(s);

	if (!so || !so->tcp)
		return -ENOTCONN;
	int r = tcp_send_oob(so->tcp, byte);
	g_pump();
	return r;
}

static int g_ping(unsigned dst, unsigned id, unsigned seq, const void *data,
		  unsigned len)
{
	int r = icmp_send_echo(&sim_dev, dst, (uint16_t)id, (uint16_t)seq,
			       data, (uint16_t)len, 64);
	g_pump();
	return r;
}

/* Wait (the simulation runs meanwhile) up to timeout_ms for the reply:
 * 1 got it, 0 not. */
static int g_ping_reply(unsigned id, unsigned seq)
{
	uint32_t src = 0;
	uint8_t type = 0, code = 0, ttl = 0;
	uint16_t rseq = 0;
	uint64_t rtt = 0;

	int r = icmp_recv_reply(&src, (uint16_t)id, &type, &code, &rseq,
				timer_ms_to_ticks(1000), &rtt, (uint16_t)seq,
				&ttl);
	return r >= 0 && type == ICMP_ECHO_REPLY;
}

static void g_arp_add(unsigned ip, const unsigned char mac[6])
{
	arp_add_entry(ip, mac);
	g_pump();
}

static int g_route_add(unsigned dst, unsigned mask, unsigned gw)
{
	return route_add(dst, mask, gw, &sim_dev, 0,
			 gw ? (RTF_UP | RTF_GATEWAY) : RTF_UP);
}

static unsigned long long g_counter(int which)
{
	net_stats_info_t st;

	mm_memset(&st, 0, sizeof(st));
	net_get_stats(&st);
	switch (which) {
	case NSC_IP_IN_RECEIVES:
		return st.ip_in_receives;
	case NSC_IP_IN_HDR_ERRORS:
		return st.ip_in_hdr_errors;
	case NSC_IP_IN_DELIVERS:
		return st.ip_in_delivers;
	case NSC_IP_OUT_REQUESTS:
		return st.ip_out_requests;
	case NSC_TCP_ACTIVE_OPENS:
		return st.tcp_active_opens;
	case NSC_TCP_PASSIVE_OPENS:
		return st.tcp_passive_opens;
	case NSC_TCP_ATTEMPT_FAILS:
		return st.tcp_attempt_fails;
	case NSC_TCP_ESTAB_RESETS:
		return st.tcp_estab_resets;
	case NSC_TCP_IN_SEGS:
		return st.tcp_in_segs;
	case NSC_TCP_OUT_SEGS:
		return st.tcp_out_segs;
	case NSC_TCP_RETRANS_SEGS:
		return st.tcp_retrans_segs;
	case NSC_TCP_IN_ERRS:
		return st.tcp_in_errs;
	case NSC_TCP_OUT_RSTS:
		return st.tcp_out_rsts;
	case NSC_TCP_OOO_QUEUE_FULL:
		return st.tcp_ooo_queue_full;
	case NSC_TCP_ACCEPTQ_FULL:
		return st.tcp_acceptq_full;
	case NSC_TCP_SYNCOOKIE_SENT:
		return st.tcp_syncookie_sent;
	case NSC_UDP_IN_DATAGRAMS:
		return st.udp_in_datagrams;
	case NSC_UDP_OUT_DATAGRAMS:
		return st.udp_out_datagrams;
	case NSC_UDP_NO_PORTS:
		return st.udp_no_ports;
	case NSC_UDP_RCVBUF_ERRORS:
		return st.udp_rcvbuf_errors;
	case NSC_SKB_ALLOC_FAIL:
		return st.skb_alloc_fail;
	case NSC_KALLOC_LIVE:
		return test_kalloc_live;
	case NSC_KALLOC_BYTES:
		return test_kalloc_bytes;
	case NSC_KALLOC_PEAK:
		return test_kalloc_peak;
	case NSC_WARNINGS:
		return test_warnings;
	case NSC_TX_FRAMES:
		return sim_tx_frames;
	case NSC_RX_FRAMES:
		return sim_rx_frames;
	case NSC_SLAB_IRQOFF:
		return test_slab_irqoff;
	case NSC_TCP_CONNS: {
		static net_tcp_info_t *buf;

		if (!buf)
			buf = hs_malloc(sizeof(*buf) * 65536);
		return buf ? (unsigned long long)net_get_tcp_connections(buf,
									 65536) :
			     0;
	}
	}
	return 0;
}

static void g_alloc_fail(unsigned long long nth, unsigned long long every)
{
	test_alloc_fail(nth, every);
}

static void g_skb_fail(unsigned long long nth, unsigned long long every)
{
	test_skb_fail(nth, every);
}

static void g_quiet(int on)
{
	test_quiet = on;
}

static void g_isn_fix(int on, unsigned value)
{
	test_sip12_fixed = on;
	test_sip12_value = value;
}

static void g_slab_strict(int on)
{
	test_slab_strict = on;
}

const struct ns_ops ns_ops = {
	.init = g_init,
	.rx = g_rx,
	.rx_nopump = g_rx_nopump,
	.tick = g_tick,
	.pump = g_pump,
	.socket = g_socket,
	.set_nonblock = g_set_nonblock,
	.bind = g_bind,
	.listen = g_listen,
	.accept = g_accept,
	.connect = g_connect,
	.send = g_send,
	.recv = g_recv,
	.sendto = g_sendto,
	.recvfrom = g_recvfrom,
	.close = g_close,
	.shutdown = g_shutdown,
	.setsockopt = g_setsockopt,
	.getsockopt = g_getsockopt,
	.poll = g_poll,
	.ioctl = g_ioctl,
	.sockname = g_sockname,
	.tcpinfo = g_tcpinfo,
	.tcp_state = g_tcp_state,
	.send_oob = g_send_oob,
	.ping = g_ping,
	.ping_reply = g_ping_reply,
	.arp_add = g_arp_add,
	.route_add = g_route_add,
	.counter = g_counter,
	.alloc_fail = g_alloc_fail,
	.skb_fail = g_skb_fail,
	.quiet = g_quiet,
	.isn_fix = g_isn_fix,
	.slab_strict = g_slab_strict,
};
