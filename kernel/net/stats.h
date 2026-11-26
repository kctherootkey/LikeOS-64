// LikeOS Network Statistics — aggregate protocol counters (MIB-style)
//
// Per-CPU lock-free counters for every protocol event and, crucially, every
// packet-discard path that was previously silent.  Increments use the
// current CPU's private row so the fast path takes no lock and tolerates a
// torn read; readers fold all rows with net_stats_fold().  The internal
// enum below is kernel-private; net_get_stats() (socket.c) projects the
// folded totals onto the stable user-facing net_stats_info_t (net.h).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_NET_STATS_H_
#define _KERNEL_NET_STATS_H_

#include <kernel/uapi/types.h>
#include <kernel/ke/percpu.h>
#include <kernel/net/net.h>

// Internal counter indices.  Order is irrelevant (net_stats_fold maps each to
// a named struct field explicitly); append new counters before NET_MIB_MAX.
enum net_mib_idx {
	/* IP */
	NET_MIB_IP_INRECEIVES = 0,
	NET_MIB_IP_INHDRERRORS,
	NET_MIB_IP_INDELIVERS,
	NET_MIB_IP_OUTREQUESTS,
	/* TCP standard */
	NET_MIB_TCP_ACTIVEOPENS,
	NET_MIB_TCP_PASSIVEOPENS,
	NET_MIB_TCP_ATTEMPTFAILS,
	NET_MIB_TCP_ESTABRESETS,
	NET_MIB_TCP_INSEGS,
	NET_MIB_TCP_OUTSEGS,
	NET_MIB_TCP_RETRANSSEGS,
	NET_MIB_TCP_INERRS,
	NET_MIB_TCP_OUTRSTS,
	/* TCP extended */
	NET_MIB_TCP_PAWSDROP,
	NET_MIB_TCP_OOWSEQDROP,
	NET_MIB_TCP_CHALLENGEACK,
	NET_MIB_TCP_OOOQUEUEFULL,
	NET_MIB_TCP_OOOOVERSIZE,
	NET_MIB_TCP_OOOFINREFUSED,
	NET_MIB_TCP_ACCEPTQFULL,
	NET_MIB_TCP_LISTENERGONE,
	NET_MIB_TCP_RSTDATALOSS,
	NET_MIB_TCP_CONNTABLEFULL,
	NET_MIB_TCP_BACKLOGDROP,
	NET_MIB_TCP_PERSISTPROBES,
	NET_MIB_TCP_SYNCOOKIESENT,
	NET_MIB_TCP_SYNCOOKIERECV,
	NET_MIB_TCP_SYNCOOKIEFAIL,
	NET_MIB_TCP_TWCREATED,
	NET_MIB_TCP_TWREUSED,
	NET_MIB_TCP_TWKILLED,
	NET_MIB_TCP_INCSUMERRORS, // bad checksum on an inbound segment
	NET_MIB_TCP_OUTOFWINDOWICMPS, // ICMP error quoting a sequence we never sent
	/* UDP */
	NET_MIB_UDP_INDATAGRAMS,
	NET_MIB_UDP_OUTDATAGRAMS,
	NET_MIB_UDP_NOPORTS,
	NET_MIB_UDP_RCVBUFERRORS,
	NET_MIB_UDP_INERRORS,
	NET_MIB_UDP_SNDBUFERRORS,
	NET_MIB_UDP_INCSUMERRORS,
	NET_MIB_UDP_IGNOREDMULTI,
	/* IP, beyond the four standard ones above */
	NET_MIB_IP_INTRUNCATEDPKTS,
	NET_MIB_IP_INADDRERRORS,
	NET_MIB_IP_INUNKNOWNPROTOS,
	NET_MIB_IP_INDISCARDS,
	NET_MIB_IP_INCSUMERRORS,
	NET_MIB_IP_INMARTIANS, // martian source or destination
	NET_MIB_IP_INSRCROUTED, // source-routed, dropped
	NET_MIB_IP_INBCASTPKTS,
	NET_MIB_IP_INMCASTPKTS,
	NET_MIB_IP_OUTNOROUTES,
	NET_MIB_IP_OUTDISCARDS,
	NET_MIB_IP_FRAGOKS,
	NET_MIB_IP_FRAGFAILS,
	NET_MIB_IP_FRAGCREATES,
	NET_MIB_IP_REASMREQDS,
	NET_MIB_IP_REASMOKS,
	NET_MIB_IP_REASMFAILS,
	NET_MIB_IP_REASMTIMEOUT,
	NET_MIB_IP_REASMOVERLAPS,
	NET_MIB_IP_REASMDUPS,
	/* ICMP */
	NET_MIB_ICMP_INMSGS,
	NET_MIB_ICMP_INERRORS,
	NET_MIB_ICMP_INCSUMERRORS,
	NET_MIB_ICMP_OUTMSGS,
	NET_MIB_ICMP_OUTERRORS,
	NET_MIB_ICMP_OUTRATELIMITGLOBAL,
	NET_MIB_ICMP_OUTRATELIMITHOST,
	NET_MIB_ICMP_INECHOS,
	NET_MIB_ICMP_OUTECHOREPS,
	NET_MIB_ICMP_INTIMESTAMPS,
	NET_MIB_ICMP_OUTTIMESTAMPREPS,
	NET_MIB_ICMP_INDESTUNREACHS,
	NET_MIB_ICMP_INTIMEEXCDS,
	NET_MIB_ICMP_INPARMPROBS,
	NET_MIB_ICMP_INREDIRECTS,
	NET_MIB_ICMP_INSRCQUENCHS,
	NET_MIB_ICMP_OUTDESTUNREACHS,
	NET_MIB_ICMP_OUTTIMEEXCDS,
	NET_MIB_ICMP_OUTPARMPROBS,
	NET_MIB_ICMP_PMTUUPDATES, // path MTU lowered by a report
	/* raw sockets */
	NET_MIB_RAW_DROPS,
	NET_MIB_MAX
};

// Per-CPU counter rows.  Defined in stats.c.
extern uint64_t g_net_mib[MAX_CPUS][NET_MIB_MAX];

// Fast increment on the calling CPU's private row (no lock; torn-read safe).
void net_stats_inc(enum net_mib_idx idx);
#define NET_STATS_INC(idx) net_stats_inc(idx)

// Sum every CPU's row for one counter (reader side).
uint64_t net_stats_read(enum net_mib_idx idx);

#endif /* _KERNEL_NET_STATS_H_ */
