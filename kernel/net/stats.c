// LikeOS-64 Network Statistics — per-CPU aggregate protocol counters.
//
// Every protocol event and packet-discard path bumps a counter here so that
// conditions that were previously silent (out-of-window drops, PAWS drops,
// reassembly-queue overflow, backlog overflow, buffer exhaustion) become
// observable through netstat and the Ctrl+N diagnostic dump.  The fast path
// is a single lock-free add to the calling CPU's private row.
#include <kernel/net/stats.h>
#include <kernel/net/skb.h>
#include <kernel/ke/percpu.h>

uint64_t g_net_mib[MAX_CPUS][NET_MIB_MAX];

// Before percpu_init() the GS base is 0 and this_cpu_id() would fault; in that
// (BSP-only) early window we are always CPU 0.
static inline uint32_t stats_safe_cpu_id(void)
{
	return read_gs_base_msr() ? this_cpu_id() : 0;
}

void net_stats_inc(enum net_mib_idx idx)
{
	if ((unsigned)idx >= NET_MIB_MAX)
		return;
	uint32_t cpu = stats_safe_cpu_id();
	if (cpu >= MAX_CPUS)
		cpu = 0;
	/* Relaxed: a torn read at fold time at worst mis-sums one increment,
	 * which is acceptable for diagnostics and avoids a hot-path locked op. */
	__atomic_fetch_add(&g_net_mib[cpu][idx], 1, __ATOMIC_RELAXED);
}

uint64_t net_stats_read(enum net_mib_idx idx)
{
	if ((unsigned)idx >= NET_MIB_MAX)
		return 0;
	uint64_t sum = 0;
	for (uint32_t c = 0; c < MAX_CPUS; c++)
		sum += __atomic_load_n(&g_net_mib[c][idx], __ATOMIC_RELAXED);
	return sum;
}

// Fold the per-CPU rows into the stable user-facing struct.  Each internal
// counter maps to exactly one named field here.
int net_get_stats(net_stats_info_t *out)
{
	if (!out)
		return -1;
	out->ip_in_receives = net_stats_read(NET_MIB_IP_INRECEIVES);
	out->ip_in_hdr_errors = net_stats_read(NET_MIB_IP_INHDRERRORS);
	out->ip_in_delivers = net_stats_read(NET_MIB_IP_INDELIVERS);
	out->ip_out_requests = net_stats_read(NET_MIB_IP_OUTREQUESTS);

	out->tcp_active_opens = net_stats_read(NET_MIB_TCP_ACTIVEOPENS);
	out->tcp_passive_opens = net_stats_read(NET_MIB_TCP_PASSIVEOPENS);
	out->tcp_attempt_fails = net_stats_read(NET_MIB_TCP_ATTEMPTFAILS);
	out->tcp_estab_resets = net_stats_read(NET_MIB_TCP_ESTABRESETS);
	out->tcp_in_segs = net_stats_read(NET_MIB_TCP_INSEGS);
	out->tcp_out_segs = net_stats_read(NET_MIB_TCP_OUTSEGS);
	out->tcp_retrans_segs = net_stats_read(NET_MIB_TCP_RETRANSSEGS);
	out->tcp_in_errs = net_stats_read(NET_MIB_TCP_INERRS);
	out->tcp_out_rsts = net_stats_read(NET_MIB_TCP_OUTRSTS);

	out->tcp_paws_drop = net_stats_read(NET_MIB_TCP_PAWSDROP);
	out->tcp_oow_seq_drop = net_stats_read(NET_MIB_TCP_OOWSEQDROP);
	out->tcp_challenge_ack = net_stats_read(NET_MIB_TCP_CHALLENGEACK);
	out->tcp_ooo_queue_full = net_stats_read(NET_MIB_TCP_OOOQUEUEFULL);
	out->tcp_ooo_oversize_drop = net_stats_read(NET_MIB_TCP_OOOOVERSIZE);
	out->tcp_ooo_fin_refused = net_stats_read(NET_MIB_TCP_OOOFINREFUSED);
	out->tcp_acceptq_full = net_stats_read(NET_MIB_TCP_ACCEPTQFULL);
	out->tcp_listener_gone_rst = net_stats_read(NET_MIB_TCP_LISTENERGONE);
	out->tcp_rst_data_loss = net_stats_read(NET_MIB_TCP_RSTDATALOSS);
	out->tcp_conn_table_full = net_stats_read(NET_MIB_TCP_CONNTABLEFULL);
	out->tcp_backlog_drop = net_stats_read(NET_MIB_TCP_BACKLOGDROP);
	out->tcp_persist_probes = net_stats_read(NET_MIB_TCP_PERSISTPROBES);
	out->tcp_syncookie_sent = net_stats_read(NET_MIB_TCP_SYNCOOKIESENT);
	out->tcp_syncookie_recv = net_stats_read(NET_MIB_TCP_SYNCOOKIERECV);
	out->tcp_syncookie_fail = net_stats_read(NET_MIB_TCP_SYNCOOKIEFAIL);
	out->tcp_tw_created = net_stats_read(NET_MIB_TCP_TWCREATED);
	out->tcp_tw_reused = net_stats_read(NET_MIB_TCP_TWREUSED);
	out->tcp_tw_killed = net_stats_read(NET_MIB_TCP_TWKILLED);

	out->udp_in_datagrams = net_stats_read(NET_MIB_UDP_INDATAGRAMS);
	out->udp_out_datagrams = net_stats_read(NET_MIB_UDP_OUTDATAGRAMS);
	out->udp_no_ports = net_stats_read(NET_MIB_UDP_NOPORTS);
	out->udp_rcvbuf_errors = net_stats_read(NET_MIB_UDP_RCVBUFERRORS);

	out->skb_alloc_fail = skb_get_alloc_failures();
	return 0;
}
