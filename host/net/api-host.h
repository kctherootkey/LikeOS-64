/* The boundary between the kernel world and the host world of the network
 * harness (host/test-net.sh).
 *
 * Included by both sides: by the kernel-side units of every stack instance
 * (freestanding, kernel headers) through prelude.h, and by the host-side
 * simulator and tests (C library).  The two worlds disagree about what
 * uint64_t, size_t and friends are, so this header uses plain C types only
 * and declares no structure either side already has.
 *
 *   hs_*      host services the kernel side calls (sim.c);
 *   ns_ops    one stack instance as the host sees it (glue.c fills it in;
 *             after the per-instance symbol rename each instance exports
 *             its own table, n0_ns_ops, n1_ns_ops, ...).
 *
 * Socket calls take a socket INDEX (what sock_create() returns, the value
 * the syscall layer stores behind a descriptor) and return a byte count or
 * a negative errno with the usual errno values.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_API_HOST_H
#define HOST_NET_API_HOST_H

/* ---- host services (sim.c) ---- */
unsigned long long hs_now_ns(void); /* the simulated clock */
unsigned hs_hz(void); /* the kernel's timer frequency */
void hs_klog(int inst, const char *text); /* one kprintf */
void hs_wire_tx(int inst, const void *frame, unsigned len);
/* Run the simulation while the instance's only task sleeps: until
 * still_blocked() returns 0, or until deadline_ns (0 = none) passes.
 * Returns 0 when woken, 1 at the deadline. */
int hs_block(int inst, int (*still_blocked)(void),
	     unsigned long long deadline_ns);
void hs_yield(int inst); /* let the simulation run one step */
/* the stack notified the poll queue of socket `sock' (-1: of something
 * that is not a socket of this instance) */
void hs_notify(int inst, int sock);
unsigned long long hs_seed(int inst);
void *hs_malloc(unsigned long n);
void hs_free(void *p);
void *hs_memset(void *p, int c, unsigned long n);
void *hs_memcpy(void *d, const void *s, unsigned long n);
int hs_memcmp(const void *a, const void *b, unsigned long n);
int hs_vsnprintf(char *b, unsigned long n, const char *fmt,
		 __builtin_va_list ap);
void hs_backtrace(void);
void hs_abort(void) __attribute__((noreturn));

/* ---- one stack instance ---- */
struct ns_cfg {
	unsigned ip; /* host byte order */
	unsigned netmask;
	unsigned gateway; /* 0: none */
	unsigned char mac[6];
	unsigned mtu;
	int gro_off; /* 1: GRO switched off (net_gro_enabled = 0) */
};

/* ns_ops.counter() indices: the stack's own protocol counters
 * (net_get_stats) followed by harness counters. */
enum ns_counter {
	NSC_IP_IN_RECEIVES,
	NSC_IP_IN_HDR_ERRORS,
	NSC_IP_IN_DELIVERS,
	NSC_IP_OUT_REQUESTS,
	NSC_TCP_ACTIVE_OPENS,
	NSC_TCP_PASSIVE_OPENS,
	NSC_TCP_ATTEMPT_FAILS,
	NSC_TCP_ESTAB_RESETS,
	NSC_TCP_IN_SEGS,
	NSC_TCP_OUT_SEGS,
	NSC_TCP_RETRANS_SEGS,
	NSC_TCP_IN_ERRS,
	NSC_TCP_OUT_RSTS,
	NSC_TCP_OOO_QUEUE_FULL,
	NSC_TCP_ACCEPTQ_FULL,
	NSC_TCP_SYNCOOKIE_SENT,
	NSC_UDP_IN_DATAGRAMS,
	NSC_UDP_OUT_DATAGRAMS,
	NSC_UDP_NO_PORTS,
	NSC_UDP_RCVBUF_ERRORS,
	NSC_SKB_ALLOC_FAIL,
	/* harness */
	NSC_KALLOC_LIVE, /* kernel heap blocks live now */
	NSC_KALLOC_BYTES, /* kernel heap bytes live now */
	NSC_KALLOC_PEAK, /* peak kernel heap bytes */
	NSC_WARNINGS, /* WARNING/BUG lines the kernel printed */
	NSC_TX_FRAMES, /* frames the device sent */
	NSC_RX_FRAMES, /* frames given to the device */
	NSC_TCP_CONNS, /* connections in the stack's table */
	NSC_SLAB_IRQOFF, /* slab_alloc/free calls with interrupts off */
	NSC_MAX
};

struct ns_tcpinfo { /* a subset of TCP_INFO */
	int state; /* TCP_STATE_* of the kernel */
	unsigned rto_us, rtt_us, rttvar_us;
	unsigned snd_cwnd, snd_ssthresh, snd_mss, rcv_mss;
	unsigned unacked, total_retrans;
};

struct ns_ops {
	/* bring the instance up: stack init, one simulated NIC, static
	 * address and routes (as a DHCP lease would set them) */
	int (*init)(int inst, const struct ns_cfg *cfg);
	/* a frame from the wire: net_rx_packet + the softirqs it raised */
	void (*rx)(const void *frame, unsigned len);
	/* the same without running the softirqs (one of a batch the NIC
	 * hands over at once: pump afterwards) */
	void (*rx_nopump)(const void *frame, unsigned len);
	/* one timer tick: net_timer_tick + the softirqs it raised */
	void (*tick)(void);
	/* run pending softirqs */
	void (*pump)(void);

	int (*socket)(int type); /* SOCK_STREAM 1, SOCK_DGRAM 2, SOCK_RAW 3 */
	int (*set_nonblock)(int s, int on);
	int (*bind)(int s, unsigned ip, unsigned port);
	int (*listen)(int s, int backlog);
	int (*accept)(int s, unsigned *ip, unsigned *port);
	int (*connect)(int s, unsigned ip, unsigned port);
	int (*send)(int s, const void *buf, unsigned long len, int flags);
	int (*recv)(int s, void *buf, unsigned long len, int flags);
	int (*sendto)(int s, const void *buf, unsigned long len, int flags,
		      unsigned ip, unsigned port);
	int (*recvfrom)(int s, void *buf, unsigned long len, int flags,
			unsigned *ip, unsigned *port);
	int (*close)(int s);
	int (*shutdown)(int s, int how);
	int (*setsockopt)(int s, int level, int name, const void *val,
			  unsigned len);
	int (*getsockopt)(int s, int level, int name, void *val,
			  unsigned *len);
	int (*poll)(int s, int events); /* sock_poll: revents */
	int (*ioctl)(int s, unsigned long req, void *arg);
	int (*sockname)(int s, int peer, unsigned *ip, unsigned *port);
	int (*tcpinfo)(int s, struct ns_tcpinfo *out);
	int (*tcp_state)(int s); /* connection state, -1 = none */
	/* tcp_send_oob on the socket's connection (send(MSG_OOB) does not
	 * reach it in the socket layer) */
	int (*send_oob)(int s, unsigned char byte);

	int (*ping)(unsigned dst, unsigned id, unsigned seq,
		    const void *data, unsigned len);
	/* a matching echo reply already received?  1 yes, 0 no */
	int (*ping_reply)(unsigned id, unsigned seq);

	void (*arp_add)(unsigned ip, const unsigned char mac[6]);
	/* route_add(dst, mask, gateway) on the instance's NIC */
	int (*route_add)(unsigned dst, unsigned mask, unsigned gw);
	unsigned long long (*counter)(int which);
	/* allocation fault injection: fail the Nth kalloc/slab_alloc from
	 * now (0 off); every > 0 also fails each every-th one after it */
	void (*alloc_fail)(unsigned long long nth, unsigned long long every);
	/* the same for packet buffers (skb_alloc) */
	void (*skb_fail)(unsigned long long nth, unsigned long long every);
	void (*quiet)(int on); /* kernel log to stderr off/on */
	/* every 12-byte SipHash (the ISN and timestamp-offset hash) returns
	 * `value' while on: lets a test place the ISN */
	void (*isn_fix)(int on, unsigned value);
	/* 1: slab_alloc/free with interrupts off is fatal (else counted) */
	void (*slab_strict)(int on);
};

#endif
