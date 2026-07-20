// LikeOS-64 Network Statistics — aggregate protocol counters (MIB-style)
//
// Per-CPU lock-free counters for every protocol event and, crucially, every
// packet-discard path that was previously silent.  Increments use the
// current CPU's private row so the fast path takes no lock and tolerates a
// torn read; readers fold all rows with net_stats_fold().  The internal
// enum below is kernel-private; net_get_stats() (socket.c) projects the
// folded totals onto the stable user-facing net_stats_info_t (net.h).
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
	/* UDP */
	NET_MIB_UDP_INDATAGRAMS,
	NET_MIB_UDP_OUTDATAGRAMS,
	NET_MIB_UDP_NOPORTS,
	NET_MIB_UDP_RCVBUFERRORS,
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
