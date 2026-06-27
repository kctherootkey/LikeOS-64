// LikeOS-64 Network Rate-Limit Infrastructure
// Token-bucket rate limiter (global and per-source-IP variants).
//
// All functions are safe to call from softirq context (spinlock protected).
// Do NOT call from hard-IRQ context — the per-source table walk is O(N).

#ifndef _KERNEL_RATELIMIT_H_
#define _KERNEL_RATELIMIT_H_

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h> // spinlock_t

// ============================================================================
// Simple token-bucket limiter
// ============================================================================
typedef struct {
	uint64_t last_tick; // tick when tokens were last replenished
	uint32_t tokens; // current token count
	uint32_t rate; // tokens added per tick (100 Hz)
	uint32_t burst; // maximum token accumulation
} net_rl_t;

// Initialise a rate limiter (burst starts full).
static inline void net_rl_init(net_rl_t *rl, uint32_t rate_per_tick,
			       uint32_t burst)
{
	rl->last_tick = 0;
	rl->tokens = burst;
	rl->rate = rate_per_tick;
	rl->burst = burst;
}

// Consume one token.  Returns 1 if allowed, 0 if rate-limited.
// Caller must hold the appropriate spinlock.
int net_rl_allow(net_rl_t *rl);

// ============================================================================
// Per-source-IP token-bucket map
// ============================================================================
#define NET_RL_SRC_TABLE_SIZE 128

typedef struct {
	uint32_t src_ip; // 0 = free slot
	uint64_t last_tick;
	uint32_t tokens;
	uint32_t last_use_tick; // LRU tracking (low 32 bits of timer_ticks())
} net_rl_src_entry_t;

typedef struct {
	net_rl_src_entry_t entries[NET_RL_SRC_TABLE_SIZE];
	uint32_t rate; // shared rate config across all src entries
	uint32_t burst;
} net_rl_src_table_t;

// Initialise a per-source table.
void net_rl_src_init(net_rl_src_table_t *t, uint32_t rate_per_tick,
		     uint32_t burst);

// Consume one token for src_ip.  Returns 1 if allowed, 0 if rate-limited.
// Caller must hold the appropriate spinlock (typically g_ratelimit_lock).
int net_rl_src_allow(net_rl_src_table_t *t, uint32_t src_ip);

// ============================================================================
// Global per-protocol rate-limiters
// (defined in ratelimit.c; extern'd here for all consumers)
// ============================================================================
extern net_rl_t g_icmp_reply_rl; // ICMP echo replies:  100/s burst 20
extern net_rl_t g_arp_reply_rl; // ARP replies:         50/s burst 10
extern net_rl_t g_udp_unreach_rl; // ICMP port-unreach:   20/s burst  5

extern net_rl_src_table_t g_icmp_src_rl; // per-source ICMP:  20/s burst 10
extern net_rl_src_table_t g_arp_src_rl; // per-source ARP:   10/s burst  5
extern net_rl_src_table_t g_tcp_syn_rl; // per-source SYN:    5/s burst  5
extern net_rl_src_table_t g_frag_src_rl; // per-source frags: 50/s burst 20
extern net_rl_src_table_t g_arp_new_rl; // new ARP entries:  10/s burst 10

// Single spinlock protecting all global rate-limit state.
extern spinlock_t g_ratelimit_lock;

// Initialise all global rate-limit state (called from net_init).
void ratelimit_init(void);

#endif // _KERNEL_RATELIMIT_H_
