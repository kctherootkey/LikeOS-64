/* The TCP probes of the network harness: what the kernel side
 * (host/net/kt_tcp.c, compiled into every instance) reports to the host
 * side (host/net/t_tcp.c).  Plain C types only -- included by both
 * worlds.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_KT_TCP_H
#define HOST_NET_KT_TCP_H

struct kt_tcp_sockinfo {
	int state;
	int rcvbuf, sndbuf;
	unsigned rmem_alloc, wmem_queued;
	unsigned window_clamp, rcv_ssthresh, rcv_wnd;
	unsigned rcv_wscale, snd_wscale, ws_enabled, sack_ok, ts_enabled;
	unsigned mss_cache, advmss, mss_clamp, rcv_mss;
	unsigned ack_quick, ack_pingpong, ack_ato_ms;
	unsigned num_sacks, ooo_skbs, rcvq_skbs, rcvq_bytes;
	unsigned cwnd, ssthresh, packets_out, snd_wnd;
	unsigned userlocks, scaling_ratio;
	unsigned rcv_space, rcv_rtt_us;
};

#endif
