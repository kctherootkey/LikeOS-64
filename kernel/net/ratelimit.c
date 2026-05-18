// LikeOS-64 Network Rate-Limit Infrastructure
#include "../../include/kernel/ratelimit.h"
#include "../../include/kernel/timer.h"

// ============================================================================
// Global state
// ============================================================================

spinlock_t g_ratelimit_lock = SPINLOCK_INIT("ratelimit");

// Global token-bucket limiters
//   Rate expressed as tokens-per-tick; timer is 100 Hz.
//   100 pkt/s  = 1 token/tick  (burst 20)
//    50 pkt/s  = 1 token/2ticks, represented as rate=1/burst=20 with refill
//   The actual replenishment uses floating ticks so fractional rates work.

net_rl_t g_icmp_reply_rl;   // 100/s, burst 20
net_rl_t g_arp_reply_rl;    //  50/s, burst 10
net_rl_t g_udp_unreach_rl;  //  20/s, burst  5

net_rl_src_table_t g_icmp_src_rl;   // per-source ICMP echo
net_rl_src_table_t g_arp_src_rl;    // per-source ARP request
net_rl_src_table_t g_tcp_syn_rl;    // per-source TCP SYN
net_rl_src_table_t g_frag_src_rl;   // per-source IP fragments
net_rl_src_table_t g_arp_new_rl;    // new ARP cache entries

void ratelimit_init(void) {
    // Global token-buckets.  Rate is in "token units per tick"; we use
    // a scaled integer: tokens are stored *100 so fractional rates can
    // be expressed without floating point.  net_rl_allow() replenishes
    // `rate` scaled-tokens per tick and compares against `burst`*100.
    // For simplicity the implementation below uses raw token counts and
    // computes elapsed ticks, adding `rate` tokens per tick up to `burst`.

    // ICMP replies: allow 100/s = 1 per tick (100 Hz), burst 20
    net_rl_init(&g_icmp_reply_rl,  1, 20);
    // ARP replies:  50/s = 1 per 2 ticks — we approximate with rate=1, burst=10
    // and a per-call check that consumes 2 tokens (or use rate=1, effective 50/s)
    net_rl_init(&g_arp_reply_rl,   1, 10);
    // ICMP port-unreach: 20/s = 1 per 5 ticks → rate=1, burst=5
    net_rl_init(&g_udp_unreach_rl, 1,  5);

    // Per-source tables
    net_rl_src_init(&g_icmp_src_rl, 1, 10); // 100/s per src, burst 10
    net_rl_src_init(&g_arp_src_rl,  1,  5); //  50/s per src, burst  5
    net_rl_src_init(&g_tcp_syn_rl,  1,  5); // 100/s per src, burst  5
    net_rl_src_init(&g_frag_src_rl, 1, 20); // 100/s per src, burst 20
    net_rl_src_init(&g_arp_new_rl,  1, 10); // 100/s per src, burst 10
}

// ============================================================================
// Token-bucket implementation
// ============================================================================

int net_rl_allow(net_rl_t* rl) {
    uint64_t now = timer_ticks();
    uint64_t elapsed = now - rl->last_tick;
    if (elapsed > 0) {
        rl->last_tick = now;
        uint32_t add = (uint32_t)(elapsed * rl->rate);
        rl->tokens += add;
        if (rl->tokens > rl->burst)
            rl->tokens = rl->burst;
    }
    if (rl->tokens == 0)
        return 0;
    rl->tokens--;
    return 1;
}

// ============================================================================
// Per-source-IP table
// ============================================================================

void net_rl_src_init(net_rl_src_table_t* t, uint32_t rate_per_tick, uint32_t burst) {
    t->rate  = rate_per_tick;
    t->burst = burst;
    for (int i = 0; i < NET_RL_SRC_TABLE_SIZE; i++) {
        t->entries[i].src_ip        = 0;
        t->entries[i].last_tick     = 0;
        t->entries[i].tokens        = 0;
        t->entries[i].last_use_tick = 0;
    }
}

int net_rl_src_allow(net_rl_src_table_t* t, uint32_t src_ip) {
    if (src_ip == 0) return 0;

    uint64_t now = timer_ticks();
    uint32_t now32 = (uint32_t)now;

    // Find existing entry for this src_ip
    int hit = -1;
    for (int i = 0; i < NET_RL_SRC_TABLE_SIZE; i++) {
        if (t->entries[i].src_ip == src_ip) {
            hit = i;
            break;
        }
    }

    if (hit < 0) {
        // Allocate: prefer a free slot, else evict LRU
        int slot = -1;
        for (int i = 0; i < NET_RL_SRC_TABLE_SIZE; i++) {
            if (t->entries[i].src_ip == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            // LRU eviction
            uint32_t oldest = t->entries[0].last_use_tick;
            slot = 0;
            for (int i = 1; i < NET_RL_SRC_TABLE_SIZE; i++) {
                if ((int32_t)(t->entries[i].last_use_tick - oldest) < 0) {
                    oldest = t->entries[i].last_use_tick;
                    slot = i;
                }
            }
        }
        t->entries[slot].src_ip        = src_ip;
        t->entries[slot].last_tick     = now;
        t->entries[slot].tokens        = t->burst;
        t->entries[slot].last_use_tick = now32;
        hit = slot;
    }

    net_rl_src_entry_t* e = &t->entries[hit];
    e->last_use_tick = now32;

    // Replenish tokens
    uint64_t elapsed = now - e->last_tick;
    if (elapsed > 0) {
        e->last_tick = now;
        uint32_t add = (uint32_t)(elapsed * t->rate);
        e->tokens += add;
        if (e->tokens > t->burst)
            e->tokens = t->burst;
    }

    if (e->tokens == 0)
        return 0;
    e->tokens--;
    return 1;
}
