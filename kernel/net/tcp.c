// LikeOS-64 TCP (Transmission Control Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/random.h"

// TCP connection table
tcp_conn_t tcp_connections[TCP_MAX_CONNECTIONS];
static spinlock_t tcp_lock = SPINLOCK_INIT("tcp");

// ---------------------------------------------------------------------------
// IRQ-friendly blocking acquire of a per-connection spinlock.
//
// Plain spin_lock_irqsave() spins with IRQs disabled.  Under heavy SMP
// contention (e.g. ksoftirqd on CPU 0 holding conn->lock to deliver a
// burst of TCP segments while another CPU's user task tries to recv from
// the same conn) the waiter can sit IRQs-off long enough to miss a TLB
// shootdown IPI from a third CPU doing slab_free() — the symptom is
// `SMP: TLB shootdown sync timeout (ack=N expect=N+1)` followed by an
// OS-wide multi-second freeze.
//
// Mirrors the trylock-with-IRQ-window pattern in smp_tlb_shootdown_sync():
// while the lock is contended, IRQs are enabled briefly between attempts
// so this CPU can ACK any pending IPIs.  Once acquired, IRQs are disabled
// (matching spin_lock_irqsave semantics).  Safe to use from any process
// or softirq context — DO NOT use from hard-IRQ context, where you
// must already be using spin_trylock-and-skip (see tcp_timer_tick).
static inline void tcp_lock_acquire(spinlock_t* lock, uint64_t* flags_out) {
    uint64_t f = local_irq_save();
    while (!spin_trylock(lock)) {
        local_irq_restore(f);
        __asm__ volatile("pause" ::: "memory");
        f = local_irq_save();
    }
    *flags_out = f;
}

static inline void tcp_lock_release(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    local_irq_restore(flags);
}

// Deferred-free queue for tcp_timer_tick.
//
// tcp_timer_tick runs in IRQ context (100Hz timer vector) on EVERY CPU
// with IRQs disabled.  If it called tcp_free_conn directly, slab_free
// could trigger an SMP TLB shootdown IPI that waits for ACKs from all
// CPUs — but those CPUs may simultaneously be running their own IRQ
// timer handler (IRQs off), unable to ACK, so the shootdown sync times
// out and the kernel logs "TLB shootdown sync timeout (ack=N expect=N+1)".
//
// To avoid this, IRQ-context callers (only tcp_timer_tick) push the
// to-be-freed conn pointer onto this small queue.  The queue is then
// drained from softirq / process context (where IRQs are enabled around
// handler invocations on at least one CPU, so IPIs can be serviced) by
// tcp_reap_pending(), which calls tcp_free_conn → slab_free safely.
static tcp_conn_t* tcp_pending_free[TCP_MAX_CONNECTIONS];
static uint32_t    tcp_pending_free_count = 0;
static spinlock_t  tcp_pending_free_lock = SPINLOCK_INIT("tcp_pf");
static void tcp_free_conn(tcp_conn_t* conn);   // forward

// Push a conn onto the deferred-free queue.  IRQ-safe (uses spinlock; the
// critical section is just an array append so it is bounded and very
// short — does NOT call slab).
static void tcp_defer_free(tcp_conn_t* conn) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_pending_free_lock, &flags);
    // Idempotent: avoid pushing the same conn twice (double-free risk).
    int already = 0;
    for (uint32_t i = 0; i < tcp_pending_free_count; i++) {
        if (tcp_pending_free[i] == conn) { already = 1; break; }
    }
    if (!already && tcp_pending_free_count < TCP_MAX_CONNECTIONS) {
        tcp_pending_free[tcp_pending_free_count++] = conn;
    }
    spin_unlock_irqrestore(&tcp_pending_free_lock, flags);
    // NOTE: do NOT raise a softirq here.  This is called from hard-IRQ
    // context (tcp_timer_tick).  The queue is drained opportunistically
    // from process-context entrypoints (tcp_alloc_conn, tcp_close) where
    // slab_free is safe.  Worst-case latency is bounded by socket churn;
    // the queue itself is bounded by TCP_MAX_CONNECTIONS.
}

// Drain the deferred-free queue.  MUST be called only from process
// context (NOT from any IRQ handler, NOT from softirq_drain that was
// entered from an IRQ tail).  tcp_free_conn → slab_free can issue a TLB
// shootdown IPI that needs other CPUs to have IRQs enabled.
void tcp_reap_pending(void) {
    for (;;) {
        tcp_conn_t* conn = NULL;
        uint64_t flags;
        spin_lock_irqsave(&tcp_pending_free_lock, &flags);
        if (tcp_pending_free_count > 0) {
            conn = tcp_pending_free[--tcp_pending_free_count];
        }
        spin_unlock_irqrestore(&tcp_pending_free_lock, flags);
        if (!conn) break;
        tcp_free_conn(conn);   // safe outside the pending-free lock
    }
}

// Forward declarations needed by helpers defined before their original sites.
static uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size);
static uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t size);
int tcp_send_segment(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t window,
                     const uint8_t* data, uint16_t data_len);
static void tcp_send_ack(tcp_conn_t* conn);

// ISN secret key (generated once at init, 128-bit for SipHash-2-4)
static uint8_t tcp_isn_secret[16];

// SYN cookie secret (separate from ISN secret)
static uint8_t tcp_syncookie_secret[16];

static uint16_t tcp_local_mss(net_device_t* dev) {
    uint16_t mtu = (dev && dev->mtu >= sizeof(ipv4_header_t) + sizeof(tcp_header_t)) ?
        dev->mtu : NET_MTU_DEFAULT;
    uint16_t mss = (uint16_t)(mtu - sizeof(ipv4_header_t) - sizeof(tcp_header_t));
    if (mss > TCP_MSS) mss = TCP_MSS;
    if (mss < 536) mss = 536;
    return mss;
}

static uint16_t tcp_effective_mss(tcp_conn_t* conn) {
    uint16_t mss = conn->max_seg_size ? conn->max_seg_size : TCP_MSS;
    if (mss > TCP_MSS) mss = TCP_MSS;
    if (mss < 536) mss = 536;
    return mss;
}

// ============================================================================
// RFC 7323 / RFC 2018 — Unified TCP option parser
// ============================================================================
typedef struct {
    uint16_t mss;             // 0 if absent
    int8_t   wscale;          // -1 if absent (else 0..14, RFC 7323 caps at 14)
    uint8_t  sack_perm;       // 1 if SACK Permitted seen
    uint8_t  ts_present;      // 1 if Timestamp option seen
    uint32_t tsval;
    uint32_t tsecr;
    uint8_t  sack_count;      // number of (left,right) pairs found
    struct { uint32_t left, right; } sack[TCP_MAX_SACK_BLOCKS];
} tcp_parsed_opts_t;

static void tcp_parse_options(const tcp_header_t* tcp, uint8_t data_offset,
                              tcp_parsed_opts_t* out) {
    out->mss = 0;
    out->wscale = -1;
    out->sack_perm = 0;
    out->ts_present = 0;
    out->tsval = 0;
    out->tsecr = 0;
    out->sack_count = 0;

    if (data_offset <= sizeof(tcp_header_t)) return;
    const uint8_t* opts = ((const uint8_t*)tcp) + sizeof(tcp_header_t);
    uint8_t opt_len = (uint8_t)(data_offset - sizeof(tcp_header_t));
    uint8_t i = 0;
    while (i < opt_len) {
        uint8_t k = opts[i];
        if (k == TCP_OPT_END) break;
        if (k == TCP_OPT_NOP) { i++; continue; }
        if (i + 1 >= opt_len) break;
        uint8_t l = opts[i + 1];
        if (l < 2 || i + l > opt_len) break;
        switch (k) {
        case TCP_OPT_MSS:
            if (l == 4) out->mss = (uint16_t)((opts[i+2] << 8) | opts[i+3]);
            break;
        case TCP_OPT_WSCALE:
            if (l == 3) {
                uint8_t s = opts[i+2];
                if (s > 14) s = 14;
                out->wscale = (int8_t)s;
            }
            break;
        case TCP_OPT_SACK_PERM:
            if (l == 2) out->sack_perm = 1;
            break;
        case TCP_OPT_TIMESTAMP:
            if (l == 10) {
                out->ts_present = 1;
                out->tsval = ((uint32_t)opts[i+2] << 24) |
                             ((uint32_t)opts[i+3] << 16) |
                             ((uint32_t)opts[i+4] << 8) |
                             ((uint32_t)opts[i+5]);
                out->tsecr = ((uint32_t)opts[i+6] << 24) |
                             ((uint32_t)opts[i+7] << 16) |
                             ((uint32_t)opts[i+8] << 8) |
                             ((uint32_t)opts[i+9]);
            }
            break;
        case TCP_OPT_SACK: {
            // Each block is 8 bytes (left,right). Header is 2 bytes (kind,len).
            uint8_t blocks = (l >= 2) ? (uint8_t)((l - 2) / 8) : 0;
            if (blocks > TCP_MAX_SACK_BLOCKS) blocks = TCP_MAX_SACK_BLOCKS;
            for (uint8_t b = 0; b < blocks; b++) {
                uint8_t off = (uint8_t)(i + 2 + b * 8);
                out->sack[b].left =
                    ((uint32_t)opts[off+0] << 24) |
                    ((uint32_t)opts[off+1] << 16) |
                    ((uint32_t)opts[off+2] << 8) |
                    ((uint32_t)opts[off+3]);
                out->sack[b].right =
                    ((uint32_t)opts[off+4] << 24) |
                    ((uint32_t)opts[off+5] << 16) |
                    ((uint32_t)opts[off+6] << 8) |
                    ((uint32_t)opts[off+7]);
            }
            out->sack_count = blocks;
            break;
        }
        default: break;
        }
        i = (uint8_t)(i + l);
    }
}

// Backwards-compat wrapper used by old callers.
static uint16_t tcp_parse_mss_option(const tcp_header_t* tcp, uint8_t data_offset) {
    tcp_parsed_opts_t p; tcp_parse_options(tcp, data_offset, &p); return p.mss;
}

// Encode 32-bit big-endian
static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// "Timestamp" we send: derive from timer_ticks so each tick is one TS unit
// (10ms, well within the RFC 7323 1ms-1s allowed range).
//
// RFC 7323 §5.4 / Appendix A: exposing the raw boot-relative tick counter
// in TSval lets any peer read this host's uptime and assists off-path
// blind-injection attacks that need to predict TSval. Mitigation is a
// random offset added to every emitted TSval; PAWS, RTTM and Karn rely on
// differences only, so the offset is invisible to the protocol.
//
// We use TWO offsets:
//   - a per-boot fallback (used when no conn context is available, e.g.
//     RST, SYN cookie SYN+ACK)
//   - a per-connection offset derived deterministically from the 4-tuple
//     via SipHash with the ISN secret. Per-conn offsets give each flow an
//     independent TS clock origin (best practice; defends against TS-based
//     uptime fingerprinting and cross-flow correlation), and the
//     deterministic 4-tuple derivation keeps SYN-cookie reconstruction
//     possible without storing extra state.
static uint32_t tcp_ts_offset = 0;
static int      tcp_ts_offset_set = 0;

static uint32_t tcp_ts_offset_global(void) {
    if (!tcp_ts_offset_set) {
        tcp_ts_offset = random_u32();
        tcp_ts_offset_set = 1;
    }
    return tcp_ts_offset;
}

// SipHash-2-4(secret, 4-tuple) truncated to 32 bits.  Reuses the ISN secret
// since both serve identical "unpredictable per 4-tuple, stable for the
// lifetime of the boot" requirements.
static uint32_t tcp_compute_ts_offset(uint32_t local_ip, uint32_t remote_ip,
                                       uint16_t local_port, uint16_t remote_port) {
    uint8_t data[12];
    data[0]  = (uint8_t)(local_ip >> 24);
    data[1]  = (uint8_t)(local_ip >> 16);
    data[2]  = (uint8_t)(local_ip >> 8);
    data[3]  = (uint8_t)(local_ip);
    data[4]  = (uint8_t)(remote_ip >> 24);
    data[5]  = (uint8_t)(remote_ip >> 16);
    data[6]  = (uint8_t)(remote_ip >> 8);
    data[7]  = (uint8_t)(remote_ip);
    data[8]  = (uint8_t)(local_port >> 8);
    data[9]  = (uint8_t)(local_port);
    data[10] = (uint8_t)(remote_port >> 8);
    data[11] = (uint8_t)(remote_port);
    // Mix in a constant tag so this hash is domain-separated from the ISN.
    uint64_t h = siphash_2_4(tcp_isn_secret, data, 12);
    return (uint32_t)(h ^ 0x54534F46U /* 'TSOF' */);
}

static uint32_t tcp_ts_now_for(const tcp_conn_t* conn) {
    uint32_t off = (conn && conn->ts_offset) ? conn->ts_offset
                                              : tcp_ts_offset_global();
    return (uint32_t)timer_ticks() + off;
}

// Backwards-compatible global accessor for paths with no conn (RST, etc.).
__attribute__((unused))
static uint32_t tcp_ts_now(void) {
    return (uint32_t)timer_ticks() + tcp_ts_offset_global();
}

// Build TCP option block. Caller passes flags actually being sent.
//   - SYN (no ACK): always offer MSS, WSCALE, SACK_PERM, TS (TSecr=0)
//   - SYN+ACK:      mirror what we negotiated in conn (mss + ws/sack/ts iff peer offered)
//   - non-SYN:      TS (if ts_enabled) + SACK blocks for OOO (if sack_ok)
// Returns option byte count (already padded with NOPs to multiple of 4).
static uint8_t tcp_build_options(tcp_conn_t* conn, uint8_t flags,
                                 uint16_t mss_to_advertise,
                                 uint8_t* buf) {
    uint8_t n = 0;
    int is_syn  = (flags & TCP_SYN) != 0;
    int is_synack = is_syn && (flags & TCP_ACK);

    // MSS only on SYN / SYN+ACK
    if (is_syn) {
        buf[n++] = TCP_OPT_MSS;
        buf[n++] = TCP_OPT_MSS_LEN;
        buf[n++] = (uint8_t)(mss_to_advertise >> 8);
        buf[n++] = (uint8_t)mss_to_advertise;
    }

    // SACK Permitted on SYN; on SYN+ACK only if peer offered (sack_ok set)
    if (is_syn) {
        if (!is_synack || (conn && conn->sack_ok)) {
            buf[n++] = TCP_OPT_NOP;
            buf[n++] = TCP_OPT_NOP;
            buf[n++] = TCP_OPT_SACK_PERM;
            buf[n++] = TCP_OPT_SACK_PERM_LEN;
        }
    }

    // Window Scale: SYN always offers; SYN+ACK only if peer offered (snd_wscale set via ws_enabled)
    if (is_syn) {
        if (!is_synack || (conn && conn->ws_enabled)) {
            buf[n++] = TCP_OPT_NOP;
            buf[n++] = TCP_OPT_WSCALE;
            buf[n++] = TCP_OPT_WSCALE_LEN;
            // We use rcv_wscale = 7 (128x scale → 8MB max window) by default
            uint8_t my_ws = (conn && conn->rcv_wscale) ? conn->rcv_wscale : 7;
            buf[n++] = my_ws;
        }
    }

    // Timestamps: include on SYN unconditionally; on later segments only if negotiated
    if (is_syn || (conn && conn->ts_enabled)) {
        // Pad to 4-byte boundary first for clean TS layout
        // (RFC 7323 §3 recommends 2 NOPs to align 10-byte TS to 32-bit boundary)
        buf[n++] = TCP_OPT_NOP;
        buf[n++] = TCP_OPT_NOP;
        buf[n++] = TCP_OPT_TIMESTAMP;
        buf[n++] = TCP_OPT_TIMESTAMP_LEN;
        put_be32(buf + n, tcp_ts_now_for(conn)); n += 4;
        uint32_t tsecr = (conn && conn->ts_enabled) ? conn->ts_recent : 0;
        put_be32(buf + n, tsecr); n += 4;
    }

    // SACK blocks on non-SYN when we have OOO data
    if (!is_syn && conn && conn->sack_ok && conn->ooo_count > 0) {
        // Build coalesced blocks from ooo[] (already sorted by seq on insert)
        uint32_t blocks_l[TCP_MAX_SACK_BLOCKS];
        uint32_t blocks_r[TCP_MAX_SACK_BLOCKS];
        uint8_t  bn = 0;
        for (uint8_t i2 = 0; i2 < conn->ooo_count && bn < TCP_MAX_SACK_BLOCKS; i2++) {
            uint32_t l = conn->ooo[i2].seq;
            uint32_t r = l + conn->ooo[i2].len;
            if (bn > 0 && blocks_r[bn - 1] == l) {
                blocks_r[bn - 1] = r;
            } else {
                blocks_l[bn] = l; blocks_r[bn] = r; bn++;
            }
        }
        if (bn > 0 && n + 2 + bn * 8 + 2 <= TCP_MAX_OPTIONS) {
            buf[n++] = TCP_OPT_NOP;
            buf[n++] = TCP_OPT_NOP;
            buf[n++] = TCP_OPT_SACK;
            buf[n++] = (uint8_t)(2 + bn * 8);
            for (uint8_t b = 0; b < bn; b++) {
                put_be32(buf + n, blocks_l[b]); n += 4;
                put_be32(buf + n, blocks_r[b]); n += 4;
            }
        }
    }

    // Pad to 4-byte boundary with NOPs
    while (n & 3) buf[n++] = TCP_OPT_NOP;
    return n;
}

// Window-scaling helper: clamp our advertised receive window so it fits in 16
// bits after the rcv_wscale shift.
static uint16_t tcp_advertised_window(tcp_conn_t* conn) {
    uint32_t avail = ring_free(conn->rx_head, conn->rx_tail, conn->rx_buf_size);
    if (avail == 0) return 0;
    uint32_t shifted = avail >> conn->rcv_wscale;
    if (shifted > 0xFFFFu) shifted = 0xFFFFu;
    return (uint16_t)shifted;
}

static int tcp_send_segment_ex(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint32_t seq, uint32_t ack,
                               uint8_t flags, uint16_t window,
                               const uint8_t* data, uint16_t data_len,
                               const uint8_t* options, uint8_t options_len) {
    uint8_t padded_options = options_len;
    if (padded_options & 3)
        padded_options = (uint8_t)((padded_options + 3) & ~3U);
    if (padded_options > TCP_MAX_OPTIONS) return -1;

    uint16_t tcp_len = (uint16_t)(sizeof(tcp_header_t) + padded_options + data_len);
    uint8_t pkt[sizeof(tcp_header_t) + TCP_MAX_OPTIONS + TCP_MSS];
    if (tcp_len > sizeof(pkt)) return -1;

    tcp_header_t* tcp = (tcp_header_t*)pkt;
    tcp->src_port = net_htons(src_port);
    tcp->dst_port = net_htons(dst_port);
    tcp->seq_num = net_htonl(seq);
    tcp->ack_num = net_htonl(ack);
    tcp->data_offset = (uint8_t)(((sizeof(tcp_header_t) + padded_options) / 4) << 4);
    tcp->flags = flags;
    tcp->window = net_htons(window);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    uint8_t* opt_dst = pkt + sizeof(tcp_header_t);
    for (uint8_t i = 0; i < padded_options; i++) opt_dst[i] = 0;
    for (uint8_t i = 0; i < options_len; i++) opt_dst[i] = options[i];

    for (uint16_t i = 0; i < data_len; i++)
        pkt[sizeof(tcp_header_t) + padded_options + i] = data[i];

    uint8_t pseudo[12 + sizeof(tcp_header_t) + TCP_MAX_OPTIONS + TCP_MSS];
    uint32_t s = net_htonl(src_ip);
    uint32_t d = net_htonl(dst_ip);
    pseudo[0] = (s >> 24) & 0xFF; pseudo[1] = (s >> 16) & 0xFF;
    pseudo[2] = (s >> 8) & 0xFF;  pseudo[3] = s & 0xFF;
    pseudo[4] = (d >> 24) & 0xFF; pseudo[5] = (d >> 16) & 0xFF;
    pseudo[6] = (d >> 8) & 0xFF;  pseudo[7] = d & 0xFF;
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    pseudo[10] = (tcp_len >> 8) & 0xFF;
    pseudo[11] = tcp_len & 0xFF;
    for (uint16_t i = 0; i < tcp_len; i++)
        pseudo[12 + i] = pkt[i];

    tcp->checksum = ipv4_checksum(pseudo, (uint16_t)(12 + tcp_len));

    return ipv4_send(dev, dst_ip, IP_PROTO_TCP, pkt, tcp_len);
}

static int tcp_queue_inflight(tcp_conn_t* conn, uint32_t seq, uint8_t flags,
                              const uint8_t* data, uint16_t len) {
    if (conn->inflight_count >= TCP_MAX_INFLIGHT) {
        conn->tx_ready = 0;
        return -1;
    }

    tcp_inflight_segment_t* seg = &conn->inflight[conn->inflight_count++];
    seg->seq = seq;
    seg->len = len;
    seg->flags = flags;
    seg->retransmit_count = 0;
    seg->send_us = timer_get_precise_us();
    for (uint16_t i = 0; i < len; i++) seg->data[i] = data[i];
    conn->tx_ready = conn->inflight_count < TCP_MAX_INFLIGHT;
    return 0;
}

static void tcp_drop_first_inflight(tcp_conn_t* conn) {
    if (conn->inflight_count == 0) return;
    for (uint8_t i = 1; i < conn->inflight_count; i++)
        conn->inflight[i - 1] = conn->inflight[i];
    conn->inflight_count--;
    conn->tx_ready = 1;
}

static void tcp_ack_inflight(tcp_conn_t* conn, uint32_t ack) {
    while (conn->inflight_count > 0) {
        tcp_inflight_segment_t* seg = &conn->inflight[0];
        uint32_t seg_end = seg->seq + seg->len + ((seg->flags & (TCP_SYN | TCP_FIN)) ? 1U : 0U);

        if (ack >= seg_end) {
            tcp_drop_first_inflight(conn);
            continue;
        }

        if (ack > seg->seq && seg->len > 0 && !(seg->flags & (TCP_SYN | TCP_FIN))) {
            uint16_t trim = (uint16_t)(ack - seg->seq);
            if (trim > seg->len) trim = seg->len;
            for (uint16_t i = trim; i < seg->len; i++)
                seg->data[i - trim] = seg->data[i];
            seg->seq = ack;
            seg->len = (uint16_t)(seg->len - trim);
        }
        break;
    }
    conn->tx_ready = conn->inflight_count < TCP_MAX_INFLIGHT;
}

static int tcp_send_syn_packet(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint32_t seq, uint32_t ack, uint8_t flags,
                               uint16_t window, tcp_conn_t* conn) {
    uint8_t options[TCP_MAX_OPTIONS];
    // Pass conn so the SYN's TSval uses the per-connection ts_offset; otherwise
    // tcp_build_options(NULL, ...) falls back to the global offset and the peer
    // saves a ts_recent that does not match the offset our later data segments
    // will use, causing every data segment to be PAWS-rejected (RFC 7323 §5.3).
    uint8_t opt_len = tcp_build_options(conn, flags, tcp_local_mss(dev), options);
    return tcp_send_segment_ex(dev, src_ip, dst_ip, src_port, dst_port,
                               seq, ack, flags, window, NULL, 0,
                               options, opt_len);
}

// SYN+ACK from a known conn so we can mirror negotiated options.
static int tcp_send_synack_conn(tcp_conn_t* conn, uint16_t window) {
    uint8_t options[TCP_MAX_OPTIONS];
    uint8_t opt_len = tcp_build_options(conn, TCP_SYN | TCP_ACK,
                                        tcp_local_mss(conn->dev), options);
    return tcp_send_segment_ex(conn->dev, conn->local_ip, conn->remote_ip,
                               conn->local_port, conn->remote_port,
                               conn->iss, conn->rcv_nxt,
                               TCP_SYN | TCP_ACK, window, NULL, 0,
                               options, opt_len);
}

static void tcp_fail_connection(tcp_conn_t* conn, int error) {
    conn->state = TCP_STATE_CLOSED;
    conn->error = error;
    conn->connect_done = 1;
    conn->rx_ready = 1;
    conn->tx_ready = 1;
    conn->inflight_count = 0;
}

// RFC 6528: ISN = hash(secret, src_ip, dst_ip, src_port, dst_port) + time_counter
// Time counter advances ~64K per second
static uint32_t tcp_generate_isn(uint32_t src_ip, uint32_t dst_ip,
                                  uint16_t src_port, uint16_t dst_port) {
    uint8_t data[12];
    data[0]  = (uint8_t)(src_ip >> 24);
    data[1]  = (uint8_t)(src_ip >> 16);
    data[2]  = (uint8_t)(src_ip >> 8);
    data[3]  = (uint8_t)(src_ip);
    data[4]  = (uint8_t)(dst_ip >> 24);
    data[5]  = (uint8_t)(dst_ip >> 16);
    data[6]  = (uint8_t)(dst_ip >> 8);
    data[7]  = (uint8_t)(dst_ip);
    data[8]  = (uint8_t)(src_port >> 8);
    data[9]  = (uint8_t)(src_port);
    data[10] = (uint8_t)(dst_port >> 8);
    data[11] = (uint8_t)(dst_port);

    uint64_t hash = siphash_2_4(tcp_isn_secret, data, 12);

    // Time component: advance ~64K per second (timer is 100Hz, so ticks/4 * 256)
    uint32_t time_comp = (uint32_t)((timer_ticks() / 4) << 8);

    return (uint32_t)hash + time_comp;
}

// ============================================================================
// SYN Cookies - RFC 4987
// ============================================================================
// Encode MSS option as 3-bit index
static const uint16_t syncookie_mss_table[8] = {
    536, 1024, 1460, 1480, 4312, 8960, 1440, 1452
};

static int mss_to_index(uint16_t mss) {
    int best = 0;
    for (int i = 0; i < 8; i++) {
        if (syncookie_mss_table[i] <= mss &&
            syncookie_mss_table[i] >= syncookie_mss_table[best])
            best = i;
    }
    return best;
}

static uint32_t tcp_syncookie_generate(uint32_t src_ip, uint32_t dst_ip,
                                        uint16_t src_port, uint16_t dst_port,
                                        uint32_t seq, uint16_t mss) {
    (void)seq;
    uint8_t data[16];
    uint32_t time_slot = (uint32_t)(timer_ticks() / 600); // ~6 second slots

    data[0]  = (uint8_t)(src_ip >> 24);
    data[1]  = (uint8_t)(src_ip >> 16);
    data[2]  = (uint8_t)(src_ip >> 8);
    data[3]  = (uint8_t)(src_ip);
    data[4]  = (uint8_t)(dst_ip >> 24);
    data[5]  = (uint8_t)(dst_ip >> 16);
    data[6]  = (uint8_t)(dst_ip >> 8);
    data[7]  = (uint8_t)(dst_ip);
    data[8]  = (uint8_t)(src_port >> 8);
    data[9]  = (uint8_t)(src_port);
    data[10] = (uint8_t)(dst_port >> 8);
    data[11] = (uint8_t)(dst_port);
    data[12] = (uint8_t)(time_slot >> 24);
    data[13] = (uint8_t)(time_slot >> 16);
    data[14] = (uint8_t)(time_slot >> 8);
    data[15] = (uint8_t)(time_slot);

    uint64_t hash = siphash_2_4(tcp_syncookie_secret, data, 16);
    int mss_idx = mss_to_index(mss);
    // Cookie = hash[31:3] | mss_idx[2:0]
    return ((uint32_t)(hash) & ~7U) | (uint32_t)mss_idx;
}

static int tcp_syncookie_validate(uint32_t src_ip, uint32_t dst_ip,
                                   uint16_t src_port, uint16_t dst_port,
                                   uint32_t cookie, uint16_t* mss_out) {
    int mss_idx = cookie & 7;

    // Try current and previous time slots
    for (int delta = 0; delta <= 1; delta++) {
        uint8_t data[16];
        uint32_t time_slot = (uint32_t)(timer_ticks() / 600) - (uint32_t)delta;

        data[0]  = (uint8_t)(src_ip >> 24);
        data[1]  = (uint8_t)(src_ip >> 16);
        data[2]  = (uint8_t)(src_ip >> 8);
        data[3]  = (uint8_t)(src_ip);
        data[4]  = (uint8_t)(dst_ip >> 24);
        data[5]  = (uint8_t)(dst_ip >> 16);
        data[6]  = (uint8_t)(dst_ip >> 8);
        data[7]  = (uint8_t)(dst_ip);
        data[8]  = (uint8_t)(src_port >> 8);
        data[9]  = (uint8_t)(src_port);
        data[10] = (uint8_t)(dst_port >> 8);
        data[11] = (uint8_t)(dst_port);
        data[12] = (uint8_t)(time_slot >> 24);
        data[13] = (uint8_t)(time_slot >> 16);
        data[14] = (uint8_t)(time_slot >> 8);
        data[15] = (uint8_t)(time_slot);

        uint64_t hash = siphash_2_4(tcp_syncookie_secret, data, 16);
        uint32_t expected = ((uint32_t)(hash) & ~7U) | (uint32_t)mss_idx;
        if (expected == cookie) {
            if (mss_out) *mss_out = syncookie_mss_table[mss_idx];
            return 1; // Valid
        }
    }
    return 0; // Invalid
}

void tcp_init(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connections[i].active = 0;
        tcp_connections[i].state = TCP_STATE_CLOSED;
    }
    // Generate ISN and SYN cookie secrets from CSPRNG
    random_get_bytes(tcp_isn_secret, sizeof(tcp_isn_secret), 0);
    random_get_bytes(tcp_syncookie_secret, sizeof(tcp_syncookie_secret), 0);
}

// ============================================================================
// RFC 6298 RTT / RTO update.  Called whenever new ACK acknowledges a segment
// for which we have a clean send-time sample (Karn: skip retransmitted segs).
// All times in microseconds.
// ============================================================================
#define TCP_RTO_MIN_US     (200000U)        // 200 ms
#define TCP_RTO_MAX_US     (60000000U)      // 60 s
#define TCP_RTO_INITIAL_US (1000000U)       // 1 s (RFC 6298 section 2.1)

static void tcp_update_rtt(tcp_conn_t* conn, uint32_t r_us) {
    if (r_us == 0) return;
    if (conn->srtt_us == 0) {
        // First measurement (RFC 6298 section 2.2)
        conn->srtt_us = r_us;
        conn->rttvar_us = r_us / 2;
    } else {
        // RTT_VAR := (1-beta)*RTT_VAR + beta*|SRTT-R|, beta=1/4
        uint32_t diff = (conn->srtt_us > r_us) ? (conn->srtt_us - r_us)
                                                : (r_us - conn->srtt_us);
        conn->rttvar_us = (conn->rttvar_us * 3 + diff) / 4;
        // SRTT := (1-alpha)*SRTT + alpha*R, alpha=1/8
        conn->srtt_us = (conn->srtt_us * 7 + r_us) / 8;
    }
    // RTO = SRTT + max(G, 4*RTTVAR), G=10ms granularity (timer is 100Hz)
    uint64_t rto = (uint64_t)conn->srtt_us + 4ULL * conn->rttvar_us;
    if (rto < TCP_RTO_MIN_US) rto = TCP_RTO_MIN_US;
    if (rto > TCP_RTO_MAX_US) rto = TCP_RTO_MAX_US;
    conn->rto_us = (uint32_t)rto;
    conn->rto_backoff = 0;
}

static uint64_t tcp_rto_ticks(tcp_conn_t* conn) {
    uint32_t rto = conn->rto_us ? conn->rto_us : TCP_RTO_INITIAL_US;
    if (conn->rto_backoff) {
        uint32_t shift = conn->rto_backoff > 6 ? 6 : conn->rto_backoff;
        if (rto > (TCP_RTO_MAX_US >> shift))
            rto = TCP_RTO_MAX_US;
        else
            rto <<= shift;
    }
    // 100Hz timer => 10000us per tick
    uint64_t ticks = rto / 10000U;
    if (ticks < 1) ticks = 1;
    return ticks;
}

// Caller MUST hold tcp_lock.  Buffers MUST be pre-allocated by the caller
// OUTSIDE tcp_lock and passed in via rx_buf / tx_buf — this function never
// invokes the slab allocator.  This is non-negotiable: slab_alloc /
// slab_free can trigger a TLB shootdown IPI that spins waiting for ACK
// from every CPU; if any other CPU is spinning on tcp_lock with IRQs off
// at that moment, it cannot service the IPI and the system hangs with
// "TLB shootdown sync timeout".
//
// Returns a slot with active=0 still set; the caller is responsible for
// filling in the 4-tuple (local_ip / local_port / remote_ip / remote_port)
// and any state-specific fields, then calling tcp_publish_conn() to
// atomically publish active=1 with a release barrier.  This split is
// required because tcp_rx / tcp_find_conn / tcp_find_listener /
// tcp_timer_tick walk the connection table WITHOUT taking tcp_lock, gating
// every access on conn->active alone — if active=1 were set before the
// 4-tuple was rewritten, another CPU could match the OLD 4-tuple of a
// recycled slot and dispatch a packet to the wrong (about-to-be-rewritten)
// connection.
//
// On allocation failure (table full) returns NULL; caller must slab_free
// the buffers it pre-allocated.
static tcp_conn_t* tcp_alloc_conn(uint8_t* rx_buf, uint8_t* tx_buf) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!tcp_connections[i].active) {
            tcp_conn_t* conn = &tcp_connections[i];
            // Slot is currently active=0.  We hold tcp_lock so no other
            // allocator can race us.  Initialise all per-conn state, but
            // leave active=0 — caller publishes after 4-tuple is written.
            conn->lock = (spinlock_t)SPINLOCK_INIT("tcp_conn");
            conn->state = TCP_STATE_CLOSED;
            conn->error = 0;
            conn->rx_ready = 0;
            conn->tx_ready = 1;
            conn->accept_ready = 0;
            conn->connect_done = 0;
            conn->accept_head = 0;
            conn->accept_tail = 0;
            conn->backlog = 0;
            conn->parent = NULL;
            conn->retransmit_count = 0;
            conn->retransmit_tick = 0;
            conn->time_wait_tick = 0;
            conn->peer_mss = TCP_MSS;
            conn->max_seg_size = TCP_MSS;
            conn->inflight_count = 0;

            // RFC 6298 initial RTO (no measurement yet)
            conn->srtt_us = 0;
            conn->rttvar_us = 0;
            conn->rto_us = TCP_RTO_INITIAL_US;
            conn->rto_backoff = 0;

            // RFC 5681 NewReno: cwnd starts at 10 segments (RFC 6928 IW10)
            conn->cwnd = 10;
            conn->ssthresh = 0xFFFFFFFFu;
            conn->dup_acks = 0;
            conn->total_retrans = 0;

            conn->nodelay = 0;
            conn->keepalive = 0;
            conn->keepidle_ticks = 7200 * 100; // 2 hours
            conn->keepintvl_ticks = 75 * 100;  // 75 s
            conn->keepcnt = 9;
            conn->keep_probes_sent = 0;
            conn->keep_next_tick = 0;
            conn->last_rx_tick = timer_ticks();

            // RFC 7323 / 2018 — feature negotiation state (cleared until peer agrees)
            conn->ts_enabled = 0;
            conn->ts_recent = 0;
            conn->ts_recent_age = 0;
            conn->ws_enabled = 0;
            conn->snd_wscale = 0;
            conn->rcv_wscale = 7;       // we always offer 7 (128x); cleared if not negotiated
            conn->sack_ok = 0;
            conn->sack_block_count = 0;
            conn->ooo_count = 0;
            conn->delayed_ack_pending = 0;
            conn->segs_since_ack = 0;
            conn->delayed_ack_deadline = 0;
            conn->cork = 0;
            conn->cork_deadline = 0;
            conn->fin_wait_2_deadline = 0;
            conn->urgent_valid = 0;
            conn->urgent_byte = 0;
            conn->snd_up = 0;
            conn->rcv_up = 0;
            conn->snd_urg_pending = 0;

            // Adopt pre-allocated RX/TX buffers (allocator was caller, OUTSIDE tcp_lock).
            conn->rx_buf = rx_buf;
            conn->tx_buf = tx_buf;
            conn->rx_buf_size = TCP_RX_BUF_SIZE;
            conn->tx_buf_size = TCP_TX_BUF_SIZE;
            conn->rx_head = 0;
            conn->rx_tail = 0;
            conn->tx_head = 0;
            conn->tx_tail = 0;

            // Caller will set 4-tuple, then call tcp_publish_conn(conn).
            // Leave active=0 here.
            return conn;
        }
    }
    return NULL;
}

// Publish a freshly-allocated conn after the caller has written the
// 4-tuple.  MUST be called with tcp_lock held (so the publish is ordered
// w.r.t. tcp_alloc_conn / tcp_free_conn slot reuse).  The compiler barrier
// prevents the optimiser from reordering the active=1 store before the
// 4-tuple stores; on x86 store-store ordering is a hardware guarantee.
static inline void tcp_publish_conn(tcp_conn_t* conn) {
    __asm__ volatile("" ::: "memory");
    conn->active = 1;
}

static void tcp_detach_listener_children(tcp_conn_t* listener) {
    if (!listener) return;

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &tcp_connections[i];
        if (conn->active && conn->parent == listener)
            conn->parent = NULL;
    }
}

// Release a connection slot back to the free pool.
//
// LOCKING: takes ONLY conn->lock.  Does NOT take tcp_lock — doing so
// would extend a tcp_lock-held + IRQs-off section across the time we
// spend waiting for conn->lock, and conn->lock can be held for many
// milliseconds by tcp_send_data looping over segments doing device PIO.
// During that window any third CPU initiating a TLB shootdown via the
// slab allocator would time out, because every CPU spinning on tcp_lock
// has IRQs disabled and cannot service the shootdown IPI.
//
// The slot-reuse race is still safe without tcp_lock here:
//   - tcp_alloc_conn callers DO hold tcp_lock, so two simultaneous
//     allocators cannot both claim the same slot.
//   - We clear active=0 LAST under conn->lock, after NULLing rx_buf /
//     tx_buf and zeroing the 4-tuple.  An allocator that subsequently
//     sees active=0 (lock-free read inside tcp_lock) is therefore
//     guaranteed to see the slot fully quiesced.
//   - slab_free() is done OUTSIDE conn->lock to avoid a slab→conn
//     lock-order inversion (slab_free itself takes a global slab lock
//     and may trigger a TLB shootdown).
//
// Caller MUST NOT hold conn->lock.  Safe to call with or without
// tcp_lock held.
static void tcp_free_conn(tcp_conn_t* conn) {
    uint64_t flags;
    tcp_lock_acquire(&conn->lock, &flags);
    // Idempotent: if already freed (active=0), do nothing.  Two CPUs may
    // race to free the same slot (e.g. tcp_timer_tick TIME_WAIT expiry vs.
    // tcp_connect's TIME_WAIT recycle); whichever loses the race finds
    // active=0 and bails.
    if (!conn->active) {
        tcp_lock_release(&conn->lock, flags);
        return;
    }
    void* old_rx = conn->rx_buf;
    void* old_tx = conn->tx_buf;
    conn->rx_buf = NULL;
    conn->tx_buf = NULL;
    conn->parent = NULL;
    conn->state = TCP_STATE_CLOSED;
    // Zero the 4-tuple so a lock-free walker that races between our
    // active=0 store and a future tcp_alloc_conn cannot match a stale tuple.
    conn->local_ip = 0;
    conn->local_port = 0;
    conn->remote_ip = 0;
    conn->remote_port = 0;
    // Publish active=0 LAST under the lock so anyone re-checking active
    // under conn->lock will see CLOSED + NULL buffers + zero tuple
    // consistently.
    __asm__ volatile("" ::: "memory");
    conn->active = 0;
    tcp_lock_release(&conn->lock, flags);

    // slab_free OUTSIDE conn->lock — see comment above.
    if (old_rx) slab_free(old_rx);
    if (old_tx) slab_free(old_tx);
}

// Find connection by 4-tuple
static tcp_conn_t* tcp_find_conn(uint32_t local_ip, uint16_t local_port,
                                  uint32_t remote_ip, uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &tcp_connections[i];
        if (c->active && c->state != TCP_STATE_LISTEN &&
            c->local_port == local_port && c->remote_port == remote_port &&
            (c->local_ip == local_ip || c->local_ip == 0) &&
            c->remote_ip == remote_ip) {
            return c;
        }
    }
    return NULL;
}

// Find listening socket on port
static tcp_conn_t* tcp_find_listener(uint32_t local_ip, uint16_t local_port) {
    tcp_conn_t* wildcard = NULL;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &tcp_connections[i];
        if (c->active && c->state == TCP_STATE_LISTEN &&
            c->local_port == local_port) {
            if (c->local_ip == local_ip)
                return c;
            if (c->local_ip == 0 && !wildcard)
                wildcard = c;
        }
    }
    return wildcard;
}

// Ring buffer helpers
static uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size) {
    return (tail - head + size) % size;
}

static uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t size) {
    return size - 1 - ring_used(head, tail, size);
}

// ============================================================================
// Send TCP Segment
// ============================================================================
int tcp_send_segment(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t window,
                     const uint8_t* data, uint16_t data_len) {
    return tcp_send_segment_ex(dev, src_ip, dst_ip, src_port, dst_port,
                               seq, ack, flags, window, data, data_len,
                               NULL, 0);
}

static void tcp_send_ack(tcp_conn_t* conn) {
    uint8_t opts[TCP_MAX_OPTIONS];
    uint8_t olen = tcp_build_options(conn, TCP_ACK, 0, opts);
    uint16_t win = tcp_advertised_window(conn);
    tcp_send_segment_ex(conn->dev, conn->local_ip, conn->remote_ip,
                        conn->local_port, conn->remote_port,
                        conn->snd_nxt, conn->rcv_nxt,
                        TCP_ACK, win, NULL, 0, opts, olen);
    conn->delayed_ack_pending = 0;
    conn->segs_since_ack = 0;
}

static void tcp_send_rst(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         uint32_t seq, uint32_t ack) {
    tcp_send_segment(dev, src_ip, dst_ip, src_port, dst_port,
                     seq, ack, TCP_RST | TCP_ACK, 0, NULL, 0);
}

// ============================================================================
// TCP Connect (active open)
// ============================================================================
tcp_conn_t* tcp_connect(net_device_t* dev, uint32_t local_ip, uint32_t dst_ip,
                        uint16_t src_port, uint16_t dst_port) {
    uint64_t flags;

    // Drain any TIME_WAIT slots deferred by tcp_timer_tick — process
    // context, slab_free is safe here.
    tcp_reap_pending();

    // Pre-allocate the new conn's RX/TX buffers BEFORE taking tcp_lock —
    // slab_alloc may trigger a TLB shootdown IPI, which would deadlock if
    // any other CPU were spinning on tcp_lock with IRQs off.
    uint8_t* new_rx = (uint8_t*)slab_alloc(TCP_RX_BUF_SIZE);
    uint8_t* new_tx = (uint8_t*)slab_alloc(TCP_TX_BUF_SIZE);
    if (!new_rx || !new_tx) {
        if (new_rx) slab_free(new_rx);
        if (new_tx) slab_free(new_tx);
        return NULL;
    }

    // RFC 6191 / SO_REUSEADDR: recycle any TIME_WAIT slot for the same
    // 4-tuple.  Done WITHOUT tcp_lock — tcp_free_conn is idempotent and
    // takes only conn->lock.  Doing this under tcp_lock would extend the
    // tcp_lock critical section across slab_free (TLB-shootdown deadlock).
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* tw = &tcp_connections[i];
        // Lock-free pre-check; tcp_free_conn re-checks active under conn->lock.
        if (tw->active && tw->state == TCP_STATE_TIME_WAIT &&
            tw->local_port == src_port && tw->remote_port == dst_port &&
            tw->local_ip == local_ip && tw->remote_ip == dst_ip) {
            tcp_free_conn(tw);
        }
    }

    spin_lock_irqsave(&tcp_lock, &flags);

    tcp_conn_t* conn = tcp_alloc_conn(new_rx, new_tx);
    if (!conn) {
        spin_unlock_irqrestore(&tcp_lock, flags);
        slab_free(new_rx);
        slab_free(new_tx);
        return NULL;
    }

    conn->dev = dev;
    conn->local_ip = local_ip;
    conn->remote_ip = dst_ip;
    conn->local_port = src_port;
    conn->remote_port = dst_port;
    conn->ts_offset = tcp_compute_ts_offset(local_ip, dst_ip, src_port, dst_port);
    conn->iss = tcp_generate_isn(local_ip, dst_ip, src_port, dst_port);
    conn->snd_una = conn->iss;
    conn->snd_nxt = conn->iss + 1;
    conn->snd_wnd = TCP_WINDOW_SIZE;
    conn->rcv_wnd = TCP_WINDOW_SIZE;
    conn->peer_mss = TCP_MSS;
    conn->max_seg_size = tcp_local_mss(dev);
    conn->state = TCP_STATE_SYN_SENT;
    conn->retransmit_tick = timer_ticks() + TCP_SYN_RETRANSMIT_TICKS;
    conn->retransmit_count = 0;

    // Publish: 4-tuple is set, now lock-free walkers may match this slot.
    tcp_publish_conn(conn);
    spin_unlock_irqrestore(&tcp_lock, flags);

    // Send SYN
    tcp_send_syn_packet(dev, local_ip, dst_ip, src_port, dst_port,
                        conn->iss, 0, TCP_SYN, TCP_WINDOW_SIZE, conn);

    return conn;
}

// ============================================================================
// TCP Listen (passive open)
// ============================================================================
tcp_conn_t* tcp_listen(net_device_t* dev, uint32_t local_ip,
                       uint16_t local_port, int backlog) {
    uint64_t flags;

    // Drain deferred-free queue (process context — safe to slab_free).
    tcp_reap_pending();

    // Pre-allocate buffers BEFORE taking tcp_lock — slab_alloc may trigger
    // a TLB shootdown IPI that would deadlock against tcp_lock holders.
    uint8_t* new_rx = (uint8_t*)slab_alloc(TCP_RX_BUF_SIZE);
    uint8_t* new_tx = (uint8_t*)slab_alloc(TCP_TX_BUF_SIZE);
    if (!new_rx || !new_tx) {
        if (new_rx) slab_free(new_rx);
        if (new_tx) slab_free(new_tx);
        return NULL;
    }

    spin_lock_irqsave(&tcp_lock, &flags);

    tcp_conn_t* conn = tcp_alloc_conn(new_rx, new_tx);
    if (!conn) {
        spin_unlock_irqrestore(&tcp_lock, flags);
        slab_free(new_rx);
        slab_free(new_tx);
        return NULL;
    }

    conn->dev = dev;
    conn->local_ip = local_ip;
    conn->local_port = local_port;
    conn->remote_ip = 0;
    conn->remote_port = 0;
    conn->state = TCP_STATE_LISTEN;
    conn->backlog = backlog > 16 ? 16 : backlog;
    conn->max_seg_size = tcp_local_mss(dev);

    // Publish: 4-tuple is set, now lock-free walkers may match this slot.
    tcp_publish_conn(conn);
    spin_unlock_irqrestore(&tcp_lock, flags);
    return conn;
}

// ============================================================================
// TCP Accept (from listener)
// ============================================================================
tcp_conn_t* tcp_accept(tcp_conn_t* listener) {
    if (!listener || listener->state != TCP_STATE_LISTEN) return NULL;

    uint64_t flags;
    tcp_lock_acquire(&listener->lock, &flags);

    if (listener->accept_head != listener->accept_tail) {
        tcp_conn_t* conn = listener->accept_queue[listener->accept_head];
        listener->accept_head = (listener->accept_head + 1) % 16;
        if (listener->accept_head == listener->accept_tail)
            listener->accept_ready = 0;
        if (conn)
            conn->parent = NULL;

        tcp_lock_release(&listener->lock, flags);
        return conn;
    }

    tcp_lock_release(&listener->lock, flags);

    // Fallback: if a child connection reached an accept-ready state but was
    // not linked into the explicit accept queue, return it directly.
    uint64_t tcp_flags;
    spin_lock_irqsave(&tcp_lock, &tcp_flags);

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &tcp_connections[i];
        if (!conn->active || conn->parent != listener)
            continue;

        if (conn->state == TCP_STATE_ESTABLISHED ||
            conn->state == TCP_STATE_CLOSE_WAIT) {
            conn->parent = NULL;
            spin_unlock_irqrestore(&tcp_lock, tcp_flags);
            return conn;
        }
    }

    spin_unlock_irqrestore(&tcp_lock, tcp_flags);
    return NULL;
}

// ============================================================================
// TCP Close
// ============================================================================
int tcp_close(tcp_conn_t* conn) {
    if (!conn) return -1;

    uint64_t flags;
    tcp_lock_acquire(&conn->lock, &flags);

    switch (conn->state) {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_SYN_RECEIVED:
        if (tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                             conn->local_port, conn->remote_port,
                             conn->snd_nxt, conn->rcv_nxt,
                             TCP_FIN | TCP_ACK, (uint16_t)conn->rcv_wnd,
                             NULL, 0) == 0) {
            tcp_queue_inflight(conn, conn->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
            conn->snd_nxt++;
        }
        conn->state = TCP_STATE_FIN_WAIT_1;
        tcp_lock_release(&conn->lock, flags);
        break;

    case TCP_STATE_CLOSE_WAIT:
        if (tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                             conn->local_port, conn->remote_port,
                             conn->snd_nxt, conn->rcv_nxt,
                             TCP_FIN | TCP_ACK, (uint16_t)conn->rcv_wnd,
                             NULL, 0) == 0) {
            tcp_queue_inflight(conn, conn->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
            conn->snd_nxt++;
        }
        conn->state = TCP_STATE_LAST_ACK;
        tcp_lock_release(&conn->lock, flags);
        break;

    case TCP_STATE_LISTEN:
        tcp_lock_release(&conn->lock, flags);

        spin_lock_irqsave(&tcp_lock, &flags);
        tcp_detach_listener_children(conn);
        spin_unlock_irqrestore(&tcp_lock, flags);

        conn->state = TCP_STATE_CLOSED;
        tcp_free_conn(conn);
        break;

    case TCP_STATE_SYN_SENT:
        conn->state = TCP_STATE_CLOSED;
        tcp_lock_release(&conn->lock, flags);
        tcp_free_conn(conn);
        break;

    default:
        tcp_lock_release(&conn->lock, flags);
        break;
    }

    // Opportunistic drain of slots deferred for free by tcp_timer_tick.
    // We are in process context (a syscall) — slab_free is safe here.
    tcp_reap_pending();
    return 0;
}

// ============================================================================
// TCP Send Data
// ============================================================================
int tcp_send_data(tcp_conn_t* conn, const uint8_t* data, uint16_t len) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) return -1;
    if (len == 0) return 0;

    uint64_t flags;
    tcp_lock_acquire(&conn->lock, &flags);

    uint16_t sent = 0;
    uint16_t seg_mss = tcp_effective_mss(conn);
    // Reserve room for TS option (12 bytes after NOP padding) when negotiated
    if (conn->ts_enabled && seg_mss > 12) seg_mss -= 12;

    // RFC 5681: don't put more than min(cwnd, snd_wnd) bytes in flight.
    uint32_t flightsize = 0;
    for (uint8_t i = 0; i < conn->inflight_count; i++)
        flightsize += conn->inflight[i].len;
    uint32_t cwnd_bytes = conn->cwnd * seg_mss;
    uint32_t window_bytes = conn->snd_wnd ? conn->snd_wnd : seg_mss;
    uint32_t allowed = cwnd_bytes < window_bytes ? cwnd_bytes : window_bytes;
    uint32_t budget = (allowed > flightsize) ? (allowed - flightsize) : 0;

    while (sent < len) {
        if (conn->inflight_count >= TCP_MAX_INFLIGHT) {
            conn->tx_ready = 0;
            break;
        }
        if (budget == 0) {
            if (sent == 0) conn->tx_ready = 0;
            break;
        }

        uint16_t seg_len = (uint16_t)(len - sent);
        if (seg_len > seg_mss) seg_len = seg_mss;
        if (seg_len > budget) seg_len = (uint16_t)budget;

        // Nagle: when nodelay is off and any unacked data outstanding, only
        // send MSS-sized segments to coalesce small writes.
        if (!conn->nodelay && flightsize > 0 && seg_len < seg_mss && sent > 0)
            break;

        // TCP_CORK: hold partial segments up to ~200ms unless filled
        if (conn->cork && seg_len < seg_mss) {
            if (conn->cork_deadline == 0)
                conn->cork_deadline = timer_ticks() + 20;
            if (timer_ticks() < conn->cork_deadline) break;
            conn->cork_deadline = 0;
        }

        uint8_t opts[TCP_MAX_OPTIONS];
        uint8_t olen = tcp_build_options(conn, TCP_ACK | TCP_PSH, 0, opts);
        if (tcp_send_segment_ex(conn->dev, conn->local_ip, conn->remote_ip,
                                conn->local_port, conn->remote_port,
                                conn->snd_nxt, conn->rcv_nxt,
                                TCP_ACK | TCP_PSH, tcp_advertised_window(conn),
                                data + sent, seg_len, opts, olen) < 0) {
            break;
        }

        tcp_queue_inflight(conn, conn->snd_nxt, TCP_ACK | TCP_PSH,
                           data + sent, seg_len);
        conn->snd_nxt += seg_len;
        sent += seg_len;
        flightsize += seg_len;
        budget -= seg_len;
    }

    if (sent == 0) {
        conn->tx_ready = 0;
        tcp_lock_release(&conn->lock, flags);
        return 0;
    }

    conn->retransmit_tick = timer_ticks() + tcp_rto_ticks(conn);
    conn->retransmit_count = 0;

    tcp_lock_release(&conn->lock, flags);
    return sent;
}

// ============================================================================
// TCP Receive Processing
// ============================================================================
void tcp_rx(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
            const uint8_t* data, uint16_t len) {
    if (len < sizeof(tcp_header_t)) return;

    const tcp_header_t* tcp = (const tcp_header_t*)data;
    uint16_t src_port = net_ntohs(tcp->src_port);
    uint16_t dst_port = net_ntohs(tcp->dst_port);
    uint32_t seq = net_ntohl(tcp->seq_num);
    uint32_t ack = net_ntohl(tcp->ack_num);
    uint8_t  tcp_flags = tcp->flags;
    uint16_t window = net_ntohs(tcp->window);
    uint8_t  data_offset = (tcp->data_offset >> 4) * 4;
    tcp_parsed_opts_t pop;
    tcp_parse_options(tcp, data_offset, &pop);
    uint16_t peer_mss = pop.mss;
    uint16_t urg_ptr = net_ntohs(tcp->urgent_ptr);

    if (data_offset > len) return;
    const uint8_t* payload = data + data_offset;
    uint16_t payload_len = len - data_offset;

    uint32_t local_ip = dst_ip;

    // Find existing connection
    tcp_conn_t* conn = tcp_find_conn(local_ip, dst_port, src_ip, src_port);

    if (!conn) {
        // Check for listener
        tcp_conn_t* listener = tcp_find_listener(local_ip, dst_port);
        if (listener && (tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK)) {
            // Check if accept queue is full — use SYN cookies if so
            int queue_used = (listener->accept_tail - listener->accept_head + 16) % 16;
            if (queue_used >= listener->backlog) {
                // Accept queue full: respond with SYN cookie instead of allocating state
                uint32_t cookie = tcp_syncookie_generate(
                    src_ip, local_ip, src_port, dst_port, seq, TCP_MSS);
                tcp_send_segment(dev, local_ip, src_ip, dst_port, src_port,
                                 cookie, seq + 1,
                                 TCP_SYN | TCP_ACK, TCP_WINDOW_SIZE, NULL, 0);
                return;
            }

            // New connection on listening socket.
            // Pre-allocate buffers BEFORE taking tcp_lock — slab_alloc may
            // trigger a TLB shootdown IPI that would deadlock against
            // tcp_lock holders.
            uint8_t* nc_rx = (uint8_t*)slab_alloc(TCP_RX_BUF_SIZE);
            uint8_t* nc_tx = (uint8_t*)slab_alloc(TCP_TX_BUF_SIZE);
            if (!nc_rx || !nc_tx) {
                if (nc_rx) slab_free(nc_rx);
                if (nc_tx) slab_free(nc_tx);
                // Fallback to SYN cookie
                uint32_t cookie = tcp_syncookie_generate(
                    src_ip, local_ip, src_port, dst_port, seq, TCP_MSS);
                tcp_send_segment(dev, local_ip, src_ip, dst_port, src_port,
                                 cookie, seq + 1,
                                 TCP_SYN | TCP_ACK, TCP_WINDOW_SIZE, NULL, 0);
                return;
            }

            uint64_t flags;
            spin_lock_irqsave(&tcp_lock, &flags);

            tcp_conn_t* new_conn = tcp_alloc_conn(nc_rx, nc_tx);
            if (!new_conn) {
                spin_unlock_irqrestore(&tcp_lock, flags);
                slab_free(nc_rx);
                slab_free(nc_tx);
                // Fallback to SYN cookie
                uint32_t cookie = tcp_syncookie_generate(
                    src_ip, local_ip, src_port, dst_port, seq, TCP_MSS);
                tcp_send_segment(dev, local_ip, src_ip, dst_port, src_port,
                                 cookie, seq + 1,
                                 TCP_SYN | TCP_ACK, TCP_WINDOW_SIZE, NULL, 0);
                return;
            }

            new_conn->dev = dev;
            new_conn->local_ip = local_ip;
            new_conn->remote_ip = src_ip;
            new_conn->local_port = dst_port;
            new_conn->remote_port = src_port;
            new_conn->ts_offset = tcp_compute_ts_offset(local_ip, src_ip, dst_port, src_port);
            new_conn->iss = tcp_generate_isn(local_ip, src_ip, dst_port, src_port);
            new_conn->irs = seq;
            new_conn->snd_una = new_conn->iss;
            new_conn->snd_nxt = new_conn->iss + 1;
            new_conn->rcv_nxt = seq + 1;
            new_conn->snd_wnd = window;     // not yet scaled (SYN window per RFC 7323)
            new_conn->rcv_wnd = TCP_WINDOW_SIZE;
            new_conn->peer_mss = peer_mss ? peer_mss : TCP_MSS;
            new_conn->max_seg_size = new_conn->peer_mss;
            new_conn->state = TCP_STATE_SYN_RECEIVED;
            new_conn->parent = listener;

            // RFC 7323/2018 — adopt peer-offered options
            if (pop.ts_present) {
                new_conn->ts_enabled = 1;
                new_conn->ts_recent = pop.tsval;
                new_conn->ts_recent_age = (uint32_t)timer_ticks();
            }
            if (pop.wscale >= 0) {
                new_conn->ws_enabled = 1;
                new_conn->snd_wscale = (uint8_t)pop.wscale;
                // rcv_wscale already set in alloc; will be advertised in SYN+ACK
            } else {
                new_conn->rcv_wscale = 0;
            }
            if (pop.sack_perm) new_conn->sack_ok = 1;

            // Publish: 4-tuple set, now lock-free walkers may match this slot.
            tcp_publish_conn(new_conn);
            spin_unlock_irqrestore(&tcp_lock, flags);

            // Send SYN+ACK with mirrored options
            tcp_send_synack_conn(new_conn, tcp_advertised_window(new_conn));
            return;
        }

        // SYN cookie validation: ACK for unknown connection with a listener
        if (listener && (tcp_flags & TCP_ACK) && !(tcp_flags & TCP_SYN)) {
            uint32_t cookie = ack - 1; // The ISN we sent was cookie, client ACKs cookie+1
            uint16_t cookie_mss = 0;
            if (tcp_syncookie_validate(src_ip, local_ip, src_port, dst_port,
                                       cookie, &cookie_mss)) {
                // Valid SYN cookie - create connection directly in ESTABLISHED state.
                // Pre-allocate buffers BEFORE taking tcp_lock (TLB-shootdown safety).
                uint8_t* cc_rx = (uint8_t*)slab_alloc(TCP_RX_BUF_SIZE);
                uint8_t* cc_tx = (uint8_t*)slab_alloc(TCP_TX_BUF_SIZE);
                if (!cc_rx || !cc_tx) {
                    if (cc_rx) slab_free(cc_rx);
                    if (cc_tx) slab_free(cc_tx);
                    return;
                }

                uint64_t flags;
                spin_lock_irqsave(&tcp_lock, &flags);

                tcp_conn_t* new_conn = tcp_alloc_conn(cc_rx, cc_tx);
                if (!new_conn) {
                    spin_unlock_irqrestore(&tcp_lock, flags);
                    slab_free(cc_rx);
                    slab_free(cc_tx);
                    return;
                }

                new_conn->dev = dev;
                new_conn->local_ip = local_ip;
                new_conn->remote_ip = src_ip;
                new_conn->local_port = dst_port;
                new_conn->remote_port = src_port;
                // ts_offset deliberately left 0 here: the SYN+ACK that this
                // ACK echoes was emitted from the stateless cookie path with
                // the global TS offset, so RTT/PAWS for the rest of this
                // conn must continue using the global offset for consistency.
                new_conn->iss = cookie;
                new_conn->irs = seq - 1;
                new_conn->snd_una = cookie + 1;
                new_conn->snd_nxt = cookie + 1;
                new_conn->rcv_nxt = seq;
                new_conn->snd_wnd = window;
                new_conn->rcv_wnd = TCP_WINDOW_SIZE;
                new_conn->peer_mss = cookie_mss ? cookie_mss : TCP_MSS;
                new_conn->max_seg_size = new_conn->peer_mss;
                new_conn->state = TCP_STATE_ESTABLISHED;
                new_conn->parent = listener;

                // Publish new_conn FIRST (4-tuple is set; lock-free walkers
                // may now match it), THEN release tcp_lock, THEN take
                // listener->lock for the accept_queue enqueue.  We do NOT
                // hold tcp_lock and listener->lock simultaneously: doing so
                // would extend the IRQ-off section across two lock
                // acquisitions and contribute to TLB-shootdown timeouts
                // when other CPUs are already contending listener->lock
                // via tcp_accept().
                tcp_publish_conn(new_conn);
                spin_unlock_irqrestore(&tcp_lock, flags);

                // Enqueue to listener accept queue under listener->lock so
                // it serialises with tcp_accept (the reader).
                uint64_t lflags;
                tcp_lock_acquire(&listener->lock, &lflags);
                int next_tail = (listener->accept_tail + 1) % 16;
                if (next_tail != listener->accept_head) {
                    listener->accept_queue[listener->accept_tail] = new_conn;
                    listener->accept_tail = next_tail;
                    listener->accept_ready = 1;
                }
                tcp_lock_release(&listener->lock, lflags);
                return;
            }
        }

        // No connection and no listener - send RST
        if (!(tcp_flags & TCP_RST)) {
            if (tcp_flags & TCP_ACK) {
                tcp_send_rst(dev, local_ip, src_ip, dst_port, src_port, ack, 0);
            } else {
                tcp_send_rst(dev, local_ip, src_ip, dst_port, src_port,
                             0, seq + payload_len + ((tcp_flags & TCP_SYN) ? 1 : 0));
            }
        }
        return;
    }

    uint64_t flags;
    tcp_lock_acquire(&conn->lock, &flags);

    // TOCTOU re-validate: tcp_find_conn() above ran lock-free, so between
    // that lookup and acquiring conn->lock the slot may have been freed
    // (active=0) or recycled with a different 4-tuple by tcp_alloc_conn on
    // another CPU.  If anything no longer matches, drop the packet — the
    // caller's payload pointer would otherwise be applied to the wrong
    // connection (silent cross-stream corruption) or a freed buffer (UAF).
    if (!conn->active ||
        conn->local_port != dst_port ||
        conn->remote_port != src_port ||
        conn->remote_ip != src_ip ||
        (conn->local_ip != local_ip && conn->local_ip != 0)) {
        tcp_lock_release(&conn->lock, flags);
        return;
    }

    // Process by state
    switch (conn->state) {
    case TCP_STATE_SYN_SENT:
        if ((tcp_flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == conn->snd_nxt) {
                conn->irs = seq;
                conn->rcv_nxt = seq + 1;
                conn->snd_una = ack;
                conn->snd_wnd = window;     // SYN window unscaled
                conn->peer_mss = peer_mss ? peer_mss : TCP_MSS;
                conn->max_seg_size = conn->peer_mss;
                conn->state = TCP_STATE_ESTABLISHED;
                conn->connect_done = 1;

                // Negotiate TS / WS / SACK based on what the peer echoed
                if (pop.ts_present) {
                    conn->ts_enabled = 1;
                    conn->ts_recent = pop.tsval;
                    conn->ts_recent_age = (uint32_t)timer_ticks();
                }
                if (pop.wscale >= 0) {
                    conn->ws_enabled = 1;
                    conn->snd_wscale = (uint8_t)pop.wscale;
                } else {
                    conn->rcv_wscale = 0;   // peer didn't agree
                }
                if (pop.sack_perm) conn->sack_ok = 1;

                // Send ACK (now with TS option if negotiated, scaled window)
                tcp_send_ack(conn);
            }
        } else if (tcp_flags & TCP_RST) {
            tcp_fail_connection(conn, ECONNREFUSED);
        } else if ((tcp_flags & TCP_SYN) && !(tcp_flags & TCP_ACK)) {
            // RFC 793 §3.4 simultaneous open — both sides sent SYN
            conn->irs = seq;
            conn->rcv_nxt = seq + 1;
            conn->snd_wnd = window;
            if (pop.ts_present) { conn->ts_enabled = 1; conn->ts_recent = pop.tsval; }
            if (pop.wscale >= 0) { conn->ws_enabled = 1; conn->snd_wscale = (uint8_t)pop.wscale; }
            if (pop.sack_perm) conn->sack_ok = 1;
            conn->state = TCP_STATE_SYN_RECEIVED;
            tcp_send_synack_conn(conn, tcp_advertised_window(conn));
        }
        break;

    case TCP_STATE_SYN_RECEIVED:
        if (tcp_flags & TCP_ACK) {
            if (ack == conn->snd_nxt) {
                conn->snd_una = ack;
                // First ACK after SYN+ACK: window is now scaled
                {
                    uint32_t scaled = (uint32_t)window;
                    if (conn->ws_enabled) scaled <<= conn->snd_wscale;
                    conn->snd_wnd = scaled;
                }
                conn->max_seg_size = conn->peer_mss ? conn->peer_mss : TCP_MSS;
                conn->state = TCP_STATE_ESTABLISHED;

                // Enqueue in parent's accept queue
                if (conn->parent) {
                    tcp_conn_t* p = conn->parent;
                    uint64_t pflags;
                    spin_lock_irqsave(&p->lock, &pflags);
                    int next = (p->accept_tail + 1) % 16;
                    if (next != p->accept_head) {
                        p->accept_queue[p->accept_tail] = conn;
                        p->accept_tail = next;
                        p->accept_ready = 1;
                    }
                    spin_unlock_irqrestore(&p->lock, pflags);
                }
            }
        }
        if (tcp_flags & TCP_RST) {
            conn->state = TCP_STATE_CLOSED;
            tcp_lock_release(&conn->lock, flags);
            tcp_free_conn(conn);
            return;
        }
        break;

    case TCP_STATE_ESTABLISHED:
        if (tcp_flags & TCP_RST) {
            tcp_fail_connection(conn, ECONNRESET);
            break;
        }
        conn->last_rx_tick = timer_ticks();
        conn->keep_probes_sent = 0;

        // RFC 7323 §5.3 PAWS — drop segments with TSval older than ts_recent
        // (only if seg has data and we have a ts_recent).
        if (conn->ts_enabled && pop.ts_present) {
            if ((int32_t)(pop.tsval - conn->ts_recent) < 0 && payload_len > 0) {
                tcp_send_ack(conn);
                break;
            }
            // Update ts_recent if seg covers ts_recent's ack point
            if ((int32_t)(seq - conn->rcv_nxt) <= 0 &&
                (int32_t)(pop.tsval - conn->ts_recent) >= 0) {
                conn->ts_recent = pop.tsval;
                conn->ts_recent_age = (uint32_t)timer_ticks();
            }
        }

        // Apply RFC 7323 window scaling on inbound advertised window
        {
            uint32_t scaled = (uint32_t)window;
            if (conn->ws_enabled) scaled <<= conn->snd_wscale;
            conn->snd_wnd = scaled;
        }

        // RFC 6093: track urgent pointer for MSG_OOB / SIOCATMARK
        if (tcp_flags & TCP_URG) {
            uint32_t up = seq + urg_ptr;
            conn->rcv_up = up;
            // Save the OOB byte (last byte of urgent data per BSD semantics)
            if (urg_ptr > 0 && urg_ptr <= payload_len) {
                conn->urgent_byte = payload[urg_ptr - 1];
                conn->urgent_valid = 1;
            }
        }

        // Process ACK
        if (tcp_flags & TCP_ACK) {
            // Store any peer-sent SACK blocks for the retransmit timer to use
            if (conn->sack_ok && pop.sack_count > 0) {
                conn->sack_block_count = pop.sack_count;
                for (uint8_t b = 0; b < pop.sack_count; b++) {
                    conn->sack_blocks[b].left = pop.sack[b].left;
                    conn->sack_blocks[b].right = pop.sack[b].right;
                }
            }
            if (ack > conn->snd_una && ack <= conn->snd_nxt) {
                // Prefer RFC 7323 TSecr for RTT (ignores Karn ambiguity for retrans)
                int rtt_sampled = 0;
                uint64_t now_us = timer_get_precise_us();
                if (conn->ts_enabled && pop.ts_present && pop.tsecr != 0) {
                    uint32_t now_ts = tcp_ts_now_for(conn);
                    uint32_t elapsed_ticks = now_ts - pop.tsecr;
                    // each TS unit = one 100Hz tick = 10ms = 10000us
                    tcp_update_rtt(conn, elapsed_ticks * 10000U);
                    rtt_sampled = 1;
                }
                if (!rtt_sampled) {
                    for (uint8_t i = 0; i < conn->inflight_count && !rtt_sampled; i++) {
                        tcp_inflight_segment_t* seg = &conn->inflight[i];
                        uint32_t seg_end = seg->seq + seg->len +
                            ((seg->flags & (TCP_SYN | TCP_FIN)) ? 1U : 0U);
                        if (ack >= seg_end && seg->retransmit_count == 0 && seg->send_us) {
                            if (now_us > seg->send_us)
                                tcp_update_rtt(conn, (uint32_t)(now_us - seg->send_us));
                            rtt_sampled = 1;
                        }
                    }
                }
                conn->snd_una = ack;
                tcp_ack_inflight(conn, ack);
                conn->retransmit_count = 0;
                conn->retransmit_tick = timer_ticks() + tcp_rto_ticks(conn);
                conn->tx_ready = conn->inflight_count < TCP_MAX_INFLIGHT;
                conn->dup_acks = 0;

                // RFC 5681 NewReno congestion control on new ACK.
                if (conn->cwnd < conn->ssthresh) {
                    conn->cwnd++;
                    if (conn->cwnd > 65535U) conn->cwnd = 65535U;
                } else {
                    static uint32_t ca_counter = 0;
                    ca_counter++;
                    if (ca_counter >= conn->cwnd) {
                        conn->cwnd++;
                        ca_counter = 0;
                    }
                }
            } else if (ack == conn->snd_una && payload_len == 0 &&
                       conn->inflight_count > 0) {
                conn->dup_acks++;
                if (conn->dup_acks == 3) {
                    uint32_t flight = conn->inflight_count;
                    conn->ssthresh = flight > 2 ? flight / 2 : 2;
                    conn->cwnd = conn->ssthresh + 3;
                    if (conn->inflight_count > 0) {
                        tcp_inflight_segment_t* seg = &conn->inflight[0];
                        tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                                         conn->local_port, conn->remote_port,
                                         seg->seq, conn->rcv_nxt, seg->flags,
                                         tcp_advertised_window(conn),
                                         seg->data, seg->len);
                        seg->retransmit_count++;
                        conn->total_retrans++;
                    }
                } else if (conn->dup_acks > 3) {
                    conn->cwnd++;
                }
            }
        }

        // Process data: in-order vs out-of-order
        if (payload_len > 0) {
            if (seq == conn->rcv_nxt) {
                uint32_t avail = ring_free(conn->rx_head, conn->rx_tail, conn->rx_buf_size);
                uint32_t copy = payload_len;
                if (copy > avail) copy = avail;
                for (uint32_t i = 0; i < copy; i++) {
                    conn->rx_buf[conn->rx_tail] = payload[i];
                    conn->rx_tail = (conn->rx_tail + 1) % conn->rx_buf_size;
                }
                conn->rcv_nxt += copy;
                conn->rx_ready = 1;

                // Drain any contiguous OOO segments
                int progress = 1;
                while (progress && conn->ooo_count > 0) {
                    progress = 0;
                    for (uint8_t k = 0; k < conn->ooo_count; k++) {
                        if (conn->ooo[k].seq == conn->rcv_nxt) {
                            uint16_t l = conn->ooo[k].len;
                            uint32_t a2 = ring_free(conn->rx_head, conn->rx_tail,
                                                    conn->rx_buf_size);
                            if (l > a2) l = (uint16_t)a2;
                            for (uint16_t j = 0; j < l; j++) {
                                conn->rx_buf[conn->rx_tail] = conn->ooo[k].data[j];
                                conn->rx_tail = (conn->rx_tail + 1) % conn->rx_buf_size;
                            }
                            conn->rcv_nxt += l;
                            // Remove this entry
                            for (uint8_t m = k + 1; m < conn->ooo_count; m++)
                                conn->ooo[m - 1] = conn->ooo[m];
                            conn->ooo_count--;
                            progress = 1;
                            break;
                        }
                    }
                }

                // RFC 1122 §4.2.3.2: delayed ACK — defer up to 200ms or every 2nd seg
                conn->segs_since_ack++;
                if (conn->segs_since_ack >= 2 || conn->ooo_count > 0 ||
                    (tcp_flags & TCP_PSH)) {
                    tcp_send_ack(conn);
                } else if (!conn->delayed_ack_pending) {
                    conn->delayed_ack_pending = 1;
                    conn->delayed_ack_deadline = timer_ticks() + 20; // ~200ms @ 100Hz
                }
            } else if ((int32_t)(seq - conn->rcv_nxt) > 0 && payload_len <= TCP_MSS) {
                // Out-of-order: insert sorted, dedup
                int dup = 0;
                uint8_t pos = 0;
                for (; pos < conn->ooo_count; pos++) {
                    if (conn->ooo[pos].seq == seq) { dup = 1; break; }
                    if ((int32_t)(seq - conn->ooo[pos].seq) < 0) break;
                }
                if (!dup && conn->ooo_count < TCP_MAX_OOO) {
                    for (uint8_t k = conn->ooo_count; k > pos; k--)
                        conn->ooo[k] = conn->ooo[k - 1];
                    conn->ooo[pos].seq = seq;
                    conn->ooo[pos].len = payload_len;
                    for (uint16_t j = 0; j < payload_len; j++)
                        conn->ooo[pos].data[j] = payload[j];
                    conn->ooo_count++;
                }
                // Send immediate dup-ACK with SACK info (helps fast retransmit)
                tcp_send_ack(conn);
            } else {
                // Already-received data — duplicate ACK
                tcp_send_ack(conn);
            }
        }

        // Process FIN
        if (tcp_flags & TCP_FIN) {
            conn->rcv_nxt = seq + payload_len + 1;
            conn->state = TCP_STATE_CLOSE_WAIT;
            conn->rx_ready = 1;  // Wake up reader (EOF)

            tcp_send_ack(conn);
        }
        break;

    case TCP_STATE_FIN_WAIT_1:
        if (tcp_flags & TCP_ACK) {
            if (ack == conn->snd_nxt) {
                conn->snd_una = ack;
                tcp_ack_inflight(conn, ack);
                if (tcp_flags & TCP_FIN) {
                    conn->rcv_nxt = seq + payload_len + 1;
                    conn->state = TCP_STATE_TIME_WAIT;
                    conn->time_wait_tick = timer_ticks() + TCP_TIME_WAIT_TICKS;
                    tcp_send_ack(conn);
                } else {
                    conn->state = TCP_STATE_FIN_WAIT_2;
                    conn->fin_wait_2_deadline = timer_ticks() + 6000; // 60s
                }
            }
        }
        if ((tcp_flags & TCP_FIN) && conn->state == TCP_STATE_FIN_WAIT_1) {
            conn->rcv_nxt = seq + payload_len + 1;
            conn->state = TCP_STATE_CLOSING;
            tcp_send_ack(conn);
        }
        break;

    case TCP_STATE_FIN_WAIT_2:
        if (tcp_flags & TCP_FIN) {
            conn->rcv_nxt = seq + payload_len + 1;
            conn->state = TCP_STATE_TIME_WAIT;
            conn->time_wait_tick = timer_ticks() + TCP_TIME_WAIT_TICKS;
            tcp_send_ack(conn);
        }
        break;

    case TCP_STATE_CLOSING:
        if ((tcp_flags & TCP_ACK) && ack == conn->snd_nxt) {
            conn->snd_una = ack;
            tcp_ack_inflight(conn, ack);
            conn->state = TCP_STATE_TIME_WAIT;
            conn->time_wait_tick = timer_ticks() + TCP_TIME_WAIT_TICKS;
        }
        break;

    case TCP_STATE_LAST_ACK:
        if ((tcp_flags & TCP_ACK) && ack == conn->snd_nxt) {
            conn->snd_una = ack;
            tcp_ack_inflight(conn, ack);
            conn->state = TCP_STATE_CLOSED;
            tcp_lock_release(&conn->lock, flags);
            tcp_free_conn(conn);
            return;
        }
        break;

    case TCP_STATE_TIME_WAIT:
        // Re-send ACK for any FIN
        if (tcp_flags & TCP_FIN) {
            tcp_send_ack(conn);
            conn->time_wait_tick = timer_ticks() + TCP_TIME_WAIT_TICKS;
        }
        break;

    default:
        break;
    }

    tcp_lock_release(&conn->lock, flags);
}

// ============================================================================
// TCP Timer (called from net_timer_tick, ~100Hz)
// ============================================================================
//
// IMPORTANT: tcp_timer_tick runs from the per-CPU timer IRQ (vector 32).  At
// 100Hz on every CPU, multiple instances can race with each other AND with
// sock_send/sock_recv/tcp_rx running on other CPUs.  Every per-conn body
// below mutates fields (inflight[], snd_nxt, rcv_nxt, cwnd, state, ...) that
// are also touched by the data path under conn->lock; we MUST take the same
// lock here or we corrupt the inflight array (torn struct copies during
// tcp_drop_first_inflight while tcp_queue_inflight writes the next slot),
// build retransmit packets from half-shifted segments (wrong bytes for a
// given seq → mysterious payload mismatches on the receiver), and free
// conn->rx_buf out from under a concurrent sock_recv.
void tcp_timer_tick(void) {
    uint64_t now = timer_ticks();

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &tcp_connections[i];
        // Lock-free pre-check is safe: ->active is only set/cleared with the
        // global tcp_lock held in alloc/free, and a stale read just causes
        // us to skip an entry one tick early or pick it up one tick late.
        if (!conn->active) continue;

        // CRITICAL: this function runs in IRQ context (100Hz timer vector)
        // on EVERY CPU.  We MUST NOT spin waiting for conn->lock here:
        // with 4 CPUs, three of them could be spin-waiting IRQ-off on the
        // same conn->lock that a fourth CPU holds (e.g. inside
        // tcp_send_data doing PIO).  If a fifth event (slab page release)
        // initiates a TLB shootdown in that window, none of the spinning
        // CPUs can ACK the IPI -> "TLB shootdown sync timeout".
        //
        // Use trylock and skip on contention -- the conn will be picked up
        // on the next tick (10 ms later).  Timer work is best-effort by
        // design; missing one tick has no correctness impact (RTO/keepalive
        // deadlines are absolute, not deltas).
        //
        // We are already in IRQ context so IRQs are disabled by the CPU's
        // IRQ delivery; no need for spin_lock_irqsave's flag save/restore.
        if (!spin_trylock(&conn->lock)) continue;

        // Re-check after acquiring the lock: another CPU may have freed it.
        if (!conn->active) {
            spin_unlock(&conn->lock);
            continue;
        }

        int do_free = 0;

        // TIME_WAIT expiry
        if (conn->state == TCP_STATE_TIME_WAIT && now >= conn->time_wait_tick) {
            conn->state = TCP_STATE_CLOSED;
            do_free = 1;
            goto unlock_conn;
        }

        // FIN_WAIT_2 timeout (RFC 1122 SHOULD discard after a while)
        if (conn->state == TCP_STATE_FIN_WAIT_2 &&
            conn->fin_wait_2_deadline && now >= conn->fin_wait_2_deadline) {
            conn->state = TCP_STATE_CLOSED;
            do_free = 1;
            goto unlock_conn;
        }

        // Delayed ACK fire
        if (conn->state == TCP_STATE_ESTABLISHED &&
            conn->delayed_ack_pending && now >= conn->delayed_ack_deadline) {
            tcp_send_ack(conn);
        }

        // TCP_CORK deadline — wake send path by clearing tx_ready oscillation
        if (conn->cork && conn->cork_deadline && now >= conn->cork_deadline) {
            conn->cork_deadline = 0;
            conn->tx_ready = 1;
        }

        // Retransmission timeout
        if (conn->state == TCP_STATE_SYN_SENT && now >= conn->retransmit_tick) {
            if (conn->retransmit_count >= TCP_MAX_RETRANSMITS) {
                tcp_fail_connection(conn, ETIMEDOUT);
                goto unlock_conn;
            }
            // Retransmit SYN
            tcp_send_syn_packet(conn->dev, conn->local_ip, conn->remote_ip,
                                conn->local_port, conn->remote_port,
                                conn->iss, 0, TCP_SYN, TCP_WINDOW_SIZE, conn);
            conn->retransmit_count++;
            conn->retransmit_tick = now + TCP_SYN_RETRANSMIT_TICKS;
        }

        if ((conn->state == TCP_STATE_ESTABLISHED ||
             conn->state == TCP_STATE_FIN_WAIT_1 ||
             conn->state == TCP_STATE_LAST_ACK ||
             conn->state == TCP_STATE_CLOSING) &&
            conn->inflight_count > 0 && now >= conn->retransmit_tick) {
            if (conn->retransmit_count >= TCP_MAX_RETRANSMITS) {
                tcp_fail_connection(conn, ETIMEDOUT);
                goto unlock_conn;
            }

            // RFC 6298: on RTO, ssthresh = max(flightsize/2, 2*MSS), cwnd = 1.
            // Karn: don't sample RTT on retransmits.  Exponential backoff.
            uint32_t flight = conn->inflight_count;
            conn->ssthresh = flight > 2 ? flight / 2 : 2;
            conn->cwnd = 1;
            conn->dup_acks = 0;
            conn->rto_backoff++;

            tcp_inflight_segment_t* seg = &conn->inflight[0];
            // Skip retransmission for segments fully covered by a SACK block
            if (conn->sack_ok && conn->sack_block_count > 0) {
                uint32_t s_start = seg->seq;
                uint32_t s_end = seg->seq + seg->len;
                for (uint8_t b = 0; b < conn->sack_block_count; b++) {
                    if ((int32_t)(conn->sack_blocks[b].left - s_start) <= 0 &&
                        (int32_t)(conn->sack_blocks[b].right - s_end) >= 0) {
                        // Already SACKed — drop from inflight head and skip retx
                        tcp_drop_first_inflight(conn);
                        seg = NULL;
                        break;
                    }
                }
            }
            if (seg) {
                tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                                 conn->local_port, conn->remote_port,
                                 seg->seq, conn->rcv_nxt, seg->flags,
                                 tcp_advertised_window(conn), seg->data, seg->len);
                seg->retransmit_count++;
                seg->send_us = 0;  // invalidate RTT sample for retransmitted seg
            }
            conn->retransmit_count++;
            conn->total_retrans++;
            conn->retransmit_tick = now + tcp_rto_ticks(conn);
        }

        // SO_KEEPALIVE: send 0-byte probe at seq=snd_una-1 after idle.
        if (conn->keepalive && conn->state == TCP_STATE_ESTABLISHED &&
            conn->inflight_count == 0) {
            uint64_t idle = now - conn->last_rx_tick;
            if (conn->keep_probes_sent == 0 && idle >= conn->keepidle_ticks) {
                tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                                 conn->local_port, conn->remote_port,
                                 conn->snd_una - 1, conn->rcv_nxt,
                                 TCP_ACK, (uint16_t)conn->rcv_wnd, NULL, 0);
                conn->keep_probes_sent = 1;
                conn->keep_next_tick = now + conn->keepintvl_ticks;
            } else if (conn->keep_probes_sent > 0 && now >= conn->keep_next_tick) {
                if (conn->keep_probes_sent >= conn->keepcnt) {
                    tcp_fail_connection(conn, ETIMEDOUT);
                    goto unlock_conn;
                }
                tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                                 conn->local_port, conn->remote_port,
                                 conn->snd_una - 1, conn->rcv_nxt,
                                 TCP_ACK, (uint16_t)conn->rcv_wnd, NULL, 0);
                conn->keep_probes_sent++;
                conn->keep_next_tick = now + conn->keepintvl_ticks;
            }
        }

unlock_conn:
        spin_unlock(&conn->lock);

        // CANNOT call tcp_free_conn here: we are in IRQ context (IRQs off)
        // on every CPU at 100Hz.  tcp_free_conn → slab_free → TLB
        // shootdown IPI would deadlock against other CPUs simultaneously
        // running their own timer-IRQ handler with IRQs disabled.  Defer
        // to softirq/process context where IRQs are enabled.
        if (do_free) {
            tcp_defer_free(conn);
        }
    }
}

// ============================================================================
// TCP_INFO sockopt — fill struct tcp_info from a connection's runtime state.
// ============================================================================
void tcp_fill_info(tcp_conn_t* conn, struct tcp_info* info) {
    if (!conn || !info) return;
    for (size_t i = 0; i < sizeof(*info); i++) ((uint8_t*)info)[i] = 0;

    info->tcpi_state = (uint8_t)conn->state;
    info->tcpi_ca_state = (conn->dup_acks >= 3) ? 3 /* recovery */ : 0;
    info->tcpi_retransmits = (uint8_t)conn->retransmit_count;
    info->tcpi_backoff = conn->rto_backoff;
    info->tcpi_options = 0;
    if (conn->ts_enabled) info->tcpi_options |= 1;     // TIMESTAMPS
    if (conn->sack_ok)    info->tcpi_options |= 2;     // SACK
    if (conn->ws_enabled) info->tcpi_options |= 4;     // WSCALE
    info->tcpi_snd_wscale_rcv_wscale =
        (uint8_t)((conn->snd_wscale & 0x0F) | ((conn->rcv_wscale & 0x0F) << 4));
    info->tcpi_rto = conn->rto_us ? conn->rto_us : TCP_RTO_INITIAL_US;
    info->tcpi_ato = 40000;
    info->tcpi_snd_mss = conn->max_seg_size ? conn->max_seg_size : TCP_MSS;
    info->tcpi_rcv_mss = conn->peer_mss ? conn->peer_mss : TCP_MSS;
    info->tcpi_unacked = conn->inflight_count;
    info->tcpi_sacked = 0;
    info->tcpi_lost = 0;
    info->tcpi_retrans = conn->retransmit_count;
    info->tcpi_pmtu = conn->dev ? conn->dev->mtu : NET_MTU_DEFAULT;
    info->tcpi_rtt = conn->srtt_us;
    info->tcpi_rttvar = conn->rttvar_us;
    info->tcpi_snd_ssthresh = conn->ssthresh;
    info->tcpi_snd_cwnd = conn->cwnd;
    info->tcpi_advmss = conn->max_seg_size ? conn->max_seg_size : TCP_MSS;
    info->tcpi_reordering = 3;
    info->tcpi_rcv_space = conn->rx_buf_size;
    info->tcpi_total_retrans = conn->total_retrans;
}

// ============================================================================
// RFC 6093 / 793 — Urgent (OOB) send: emit a single byte with URG=1.
// ============================================================================
int tcp_send_oob(tcp_conn_t* conn, uint8_t byte) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) return -1;
    uint64_t flags;
    tcp_lock_acquire(&conn->lock, &flags);

    // Build TCP packet manually since urgent_ptr is in header
    uint8_t opts[TCP_MAX_OPTIONS];
    uint8_t olen = tcp_build_options(conn, TCP_ACK | TCP_URG | TCP_PSH, 0, opts);

    // Use tcp_send_segment_ex with manual urgent_ptr override — but our helper
    // does not expose it. Send a one-byte payload with URG flag and rely on
    // the receiver tracking urgent pointer = seq+1 via raw header field.
    // For simplicity: send via segment_ex then patch checksum is too much; use
    // a small inline path.

    // Build packet
    uint16_t tcp_len = (uint16_t)(sizeof(tcp_header_t) + olen + 1);
    uint8_t pkt[sizeof(tcp_header_t) + TCP_MAX_OPTIONS + 1];
    tcp_header_t* tcp = (tcp_header_t*)pkt;
    tcp->src_port = net_htons(conn->local_port);
    tcp->dst_port = net_htons(conn->remote_port);
    tcp->seq_num = net_htonl(conn->snd_nxt);
    tcp->ack_num = net_htonl(conn->rcv_nxt);
    tcp->data_offset = (uint8_t)(((sizeof(tcp_header_t) + olen) / 4) << 4);
    tcp->flags = TCP_ACK | TCP_URG | TCP_PSH;
    tcp->window = net_htons(tcp_advertised_window(conn));
    tcp->checksum = 0;
    tcp->urgent_ptr = net_htons(1);   // points one past last urgent byte
    for (uint8_t i = 0; i < olen; i++) pkt[sizeof(tcp_header_t) + i] = opts[i];
    pkt[sizeof(tcp_header_t) + olen] = byte;

    // Pseudo-header checksum
    uint8_t pseudo[12 + sizeof(tcp_header_t) + TCP_MAX_OPTIONS + 1];
    uint32_t s = net_htonl(conn->local_ip);
    uint32_t d = net_htonl(conn->remote_ip);
    pseudo[0]=(s>>24)&0xFF; pseudo[1]=(s>>16)&0xFF;
    pseudo[2]=(s>>8)&0xFF;  pseudo[3]=s&0xFF;
    pseudo[4]=(d>>24)&0xFF; pseudo[5]=(d>>16)&0xFF;
    pseudo[6]=(d>>8)&0xFF;  pseudo[7]=d&0xFF;
    pseudo[8]=0; pseudo[9]=IP_PROTO_TCP;
    pseudo[10]=(tcp_len>>8)&0xFF; pseudo[11]=tcp_len&0xFF;
    for (uint16_t i = 0; i < tcp_len; i++) pseudo[12+i] = pkt[i];
    tcp->checksum = ipv4_checksum(pseudo, (uint16_t)(12 + tcp_len));

    int rv = ipv4_send(conn->dev, conn->remote_ip, IP_PROTO_TCP, pkt, tcp_len);
    if (rv == 0) {
        conn->snd_up = conn->snd_nxt + 1;
        conn->snd_nxt += 1;
        tcp_queue_inflight(conn, conn->snd_nxt - 1, TCP_ACK | TCP_URG | TCP_PSH,
                           &byte, 1);
    }
    tcp_lock_release(&conn->lock, flags);
    return rv;
}

// SIOCATMARK equivalent: returns 1 when next byte to read is the urgent mark.
int tcp_at_mark(tcp_conn_t* conn) {
    if (!conn) return 0;
    return (conn->rcv_nxt == conn->rcv_up) ? 1 : 0;
}

// RFC 1191: clamp a connection's effective MSS when an ICMP frag-needed
// arrives carrying the next-hop MTU.
void tcp_handle_pmtu(uint32_t local_ip, uint16_t local_port,
                     uint32_t remote_ip, uint16_t remote_port,
                     uint16_t new_mtu) {
    if (new_mtu < 68) return;
    uint16_t new_mss = (uint16_t)(new_mtu - sizeof(ipv4_header_t) - sizeof(tcp_header_t));
    if (new_mss < 256) new_mss = 256;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* c = &tcp_connections[i];
        if (!c->active) continue;
        if (c->local_port != local_port || c->remote_port != remote_port) continue;
        if (c->local_ip != local_ip || c->remote_ip != remote_ip) continue;
        if (c->max_seg_size > new_mss) c->max_seg_size = new_mss;
        if (c->peer_mss > new_mss) c->peer_mss = new_mss;
    }
}

// RFC 793 §3.5 abort: send RST and tear down connection immediately.
// Used by SO_LINGER l_onoff=1 l_linger=0.
void tcp_abort(tcp_conn_t* conn) {
    if (!conn) return;
    uint64_t flags;
    tcp_lock_acquire(&conn->lock, &flags);
    if (conn->state != TCP_STATE_CLOSED && conn->dev) {
        tcp_send_rst(conn->dev, conn->local_ip, conn->remote_ip,
                     conn->local_port, conn->remote_port,
                     conn->snd_nxt, conn->rcv_nxt);
    }
    conn->state = TCP_STATE_CLOSED;
    tcp_lock_release(&conn->lock, flags);
    tcp_free_conn(conn);
}
