// LikeOS-64 ICMP (Internet Control Message Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/random.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/skb.h"

// All TX sites use a per-call sk_buff from the size-classed pool; no shared
// static TX buffer / TX spinlock is held across the lower-layer call, so
// the receive path (which now runs in softirq context with IRQs enabled)
// cannot deadlock against an in-flight echo / timestamp / addrmask reply.

#define ICMP_REPLY_QUEUE_SIZE 16

typedef struct {
    uint32_t src_ip;
    uint8_t type;
    uint8_t code;
    uint16_t id;
    uint16_t seq;
    uint8_t ttl;
    uint8_t pad[3];
    uint64_t recv_us;
} icmp_reply_entry_t;

static icmp_reply_entry_t icmp_reply_queue[ICMP_REPLY_QUEUE_SIZE];
static int icmp_reply_head = 0;
static int icmp_reply_tail = 0;
static volatile int icmp_reply_ready = 0;
static spinlock_t icmp_reply_lock = SPINLOCK_INIT("icmp_reply");

// Kernel-side timestamps for RTT (microseconds from timer_get_precise_us)
static volatile uint64_t icmp_send_us = 0;
static volatile uint64_t icmp_recv_us = 0;

static void icmp_queue_reply(uint32_t src_ip, uint8_t type, uint8_t code,
                             uint16_t id, uint16_t seq, uint8_t ttl) {
    uint64_t flags;
    spin_lock_irqsave(&icmp_reply_lock, &flags);

    int next_tail = (icmp_reply_tail + 1) % ICMP_REPLY_QUEUE_SIZE;
    if (next_tail == icmp_reply_head)
        icmp_reply_head = (icmp_reply_head + 1) % ICMP_REPLY_QUEUE_SIZE;

    icmp_reply_entry_t* entry = &icmp_reply_queue[icmp_reply_tail];
    entry->src_ip = src_ip;
    entry->type = type;
    entry->code = code;
    entry->id = id;
    entry->seq = seq;
    entry->ttl = ttl;
    entry->recv_us = timer_get_precise_us();
    icmp_reply_tail = next_tail;
    icmp_reply_ready = (icmp_reply_head != icmp_reply_tail);

    spin_unlock_irqrestore(&icmp_reply_lock, flags);
    sched_wake_channel((void*)&icmp_reply_ready);
}

static void icmp_extract_reply_identity(const uint8_t* data, uint16_t len,
                                        uint8_t type, uint16_t* id_out,
                                        uint16_t* seq_out) {
    if (type == ICMP_ECHO_REPLY || type == ICMP_ECHO_REQUEST) {
        if (len >= sizeof(icmp_header_t)) {
            const icmp_header_t* icmp = (const icmp_header_t*)data;
            *id_out = net_ntohs(icmp->identifier);
            *seq_out = net_ntohs(icmp->sequence);
        }
        return;
    }

    if (len < sizeof(icmp_header_t) + sizeof(ipv4_header_t)) return;
    const uint8_t* quoted = data + sizeof(icmp_header_t);
    const ipv4_header_t* inner_ip = (const ipv4_header_t*)quoted;
    uint8_t inner_ihl = (inner_ip->version_ihl & 0x0F) * 4;
    if (inner_ihl < sizeof(ipv4_header_t) || len < sizeof(icmp_header_t) + inner_ihl + sizeof(icmp_header_t))
        return;
    if (inner_ip->protocol != IP_PROTO_ICMP) return;

    const icmp_header_t* inner_icmp = (const icmp_header_t*)(quoted + inner_ihl);
    *id_out = net_ntohs(inner_icmp->identifier);
    *seq_out = net_ntohs(inner_icmp->sequence);
}

int icmp_send_error_packet(net_device_t* dev, uint32_t dst_ip,
                           uint8_t type, uint8_t code,
                           const uint8_t* quoted_ip_packet,
                           uint16_t quoted_len, uint32_t aux_data) {
    if (!dev || !quoted_ip_packet) return -1;

    uint16_t copy_len = quoted_len;
    if (copy_len > NET_MTU_DEFAULT - sizeof(icmp_header_t))
        copy_len = (uint16_t)(NET_MTU_DEFAULT - sizeof(icmp_header_t));
    uint16_t total = (uint16_t)(sizeof(icmp_header_t) + copy_len);

    sk_buff_t* skb = skb_alloc(total);
    if (!skb) return -1;
    skb->dev = dev;

    uint8_t* buf = skb_append(skb, total);
    icmp_header_t* icmp = (icmp_header_t*)buf;
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->identifier = net_htons((uint16_t)(aux_data >> 16));
    icmp->sequence = net_htons((uint16_t)aux_data);

    for (uint16_t i = 0; i < copy_len; i++)
        buf[sizeof(icmp_header_t) + i] = quoted_ip_packet[i];

    icmp->checksum = ipv4_checksum(buf, total);

    int ret = ipv4_send(dev, dst_ip, IP_PROTO_ICMP, skb->data, skb->len);
    skb_put(skb);
    return ret;
}

// Process received ICMP packet
void icmp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len, uint8_t rx_ttl) {
    if (len < sizeof(icmp_header_t)) return;

    const icmp_header_t* icmp = (const icmp_header_t*)data;

    // Verify checksum over entire ICMP message
    if (ipv4_checksum(data, len) != 0) return;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        // Echo reply: copy entire received message and flip the type byte.
        // Length may be up to 65535 (reassembled IPv4 datagram); skb_alloc
        // picks the small or jumbo pool class as needed.
        sk_buff_t* skb = skb_alloc(len);
        if (!skb) return;
        skb->dev = dev;
        uint8_t* buf = skb_append(skb, len);
        for (uint16_t i = 0; i < len; i++) buf[i] = data[i];
        icmp_header_t* r = (icmp_header_t*)buf;
        r->type = ICMP_ECHO_REPLY;
        r->code = 0;
        r->checksum = 0;
        r->checksum = ipv4_checksum(buf, len);
        ipv4_send(dev, src_ip, IP_PROTO_ICMP, skb->data, skb->len);
        skb_put(skb);
    }

    // RFC 791 §3.2.2.8 Timestamp Request — reply with kernel time
    if (icmp->type == ICMP_TIMESTAMP && icmp->code == 0 && len >= 20) {
        sk_buff_t* skb = skb_alloc(len);
        if (!skb) goto skip_ts_reply;
        skb->dev = dev;
        uint8_t* buf = skb_append(skb, len);
        for (uint16_t i = 0; i < len; i++) buf[i] = data[i];
        icmp_header_t* r = (icmp_header_t*)buf;
        r->type = ICMP_TIMESTAMP_REPLY;
        r->code = 0;
        r->checksum = 0;
        // Receive + transmit timestamps. RFC 792 p.16: when the sender cannot
        // supply standard time (ms past midnight UT) the high-order bit MUST
        // be set to indicate a non-standard value. We have no wall clock and
        // exposing raw uptime would disclose boot time to any remote prober
        // (CVE-1999-0524 class), so we report a per-boot randomly offset
        // counter with the non-standard-time bit set.
        static uint32_t icmp_ts_offset = 0;
        static int      icmp_ts_offset_set = 0;
        if (!icmp_ts_offset_set) {
            icmp_ts_offset = random_u32();
            icmp_ts_offset_set = 1;
        }
        uint32_t ts_ms = ((uint32_t)(timer_ticks() * 10) + icmp_ts_offset)
                         | 0x80000000U; // non-standard time flag (RFC 792)
        uint8_t* p = buf + 12; // skip type/code/cksum/id/seq + orig TS
        p[0] = (uint8_t)(ts_ms >> 24); p[1] = (uint8_t)(ts_ms >> 16);
        p[2] = (uint8_t)(ts_ms >> 8);  p[3] = (uint8_t)ts_ms;
        p[4] = p[0]; p[5] = p[1]; p[6] = p[2]; p[7] = p[3];
        r->checksum = ipv4_checksum(buf, len);
        ipv4_send(dev, src_ip, IP_PROTO_ICMP, skb->data, skb->len);
        skb_put(skb);
    }
skip_ts_reply:

    // RFC 950 §3.1 Address Mask Request — reply with our netmask
    if (icmp->type == ICMP_ADDRMASK && icmp->code == 0 && len >= 12 && dev) {
        sk_buff_t* skb = skb_alloc(len);
        if (!skb) goto skip_mask_reply;
        skb->dev = dev;
        uint8_t* buf = skb_append(skb, len);
        for (uint16_t i = 0; i < len; i++) buf[i] = data[i];
        icmp_header_t* r = (icmp_header_t*)buf;
        r->type = ICMP_ADDRMASK_REPLY;
        r->code = 0;
        r->checksum = 0;
        uint32_t mask = dev->netmask;
        buf[8]  = (uint8_t)(mask >> 24); buf[9]  = (uint8_t)(mask >> 16);
        buf[10] = (uint8_t)(mask >> 8);  buf[11] = (uint8_t)mask;
        r->checksum = ipv4_checksum(buf, len);
        ipv4_send(dev, src_ip, IP_PROTO_ICMP, skb->data, skb->len);
        skb_put(skb);
    }
skip_mask_reply:

    // RFC 1191 PMTUD: DEST_UNREACH/FRAG_NEEDED carries next-hop MTU in id+seq.
    // Also dispatch any ICMP error to a matching socket.
    if (icmp->type == ICMP_DEST_UNREACH || icmp->type == ICMP_TIME_EXCEEDED ||
        icmp->type == ICMP_PARAM_PROBLEM) {
        const uint8_t* inner = data + sizeof(icmp_header_t);
        uint16_t ilen = (len > sizeof(icmp_header_t)) ?
                        (uint16_t)(len - sizeof(icmp_header_t)) : 0;
        uint16_t mtu_hint = 0;
        if (icmp->type == ICMP_DEST_UNREACH && icmp->code == ICMP_FRAG_NEEDED)
            mtu_hint = net_ntohs(icmp->sequence);   // RFC 1191 places MTU in low 16 bits
        icmp_dispatch_error(icmp->type, icmp->code, inner, ilen, mtu_hint);
    }

    // Store reply for userspace (echo reply, dest unreachable, time exceeded)
    if (icmp->type == ICMP_ECHO_REPLY ||
        icmp->type == ICMP_DEST_UNREACH ||
        icmp->type == ICMP_TIME_EXCEEDED) {
        uint16_t reply_id = 0;
        uint16_t reply_seq = 0;
        icmp_recv_us = timer_get_precise_us();
        icmp_extract_reply_identity(data, len, icmp->type, &reply_id, &reply_seq);
        icmp_queue_reply(src_ip, icmp->type, icmp->code, reply_id, reply_seq, rx_ttl);
    }
}

// Send ICMP echo request with configurable TTL
int icmp_send_echo(net_device_t* dev, uint32_t dst_ip, uint16_t id,
                   uint16_t seq, const uint8_t* payload, uint16_t payload_len,
                   uint8_t ttl) {
    if (!dev) return -1;
    uint16_t total = sizeof(icmp_header_t) + payload_len;
    if (total > NET_MTU_DEFAULT) return -1;

    sk_buff_t* skb = skb_alloc(total);
    if (!skb) return -1;
    skb->dev = dev;

    uint8_t* buf = skb_append(skb, total);
    icmp_header_t* icmp = (icmp_header_t*)buf;
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->identifier = net_htons(id);
    icmp->sequence = net_htons(seq);
    icmp->checksum = 0;

    if (payload && payload_len > 0) {
        for (uint16_t i = 0; i < payload_len; i++)
            buf[sizeof(icmp_header_t) + i] = payload[i];
    }

    icmp->checksum = ipv4_checksum(buf, total);

    // Record send timestamp before transmitting
    icmp_send_us = timer_get_precise_us();

    int ret = ipv4_send_ttl(dev, dst_ip, IP_PROTO_ICMP, skb->data, skb->len, ttl);
    skb_put(skb);
    return ret;
}

// Receive ICMP reply (blocking with timeout, filtered by ID and seq)
int icmp_recv_reply(uint32_t* src_ip, uint16_t expected_id,
                    uint8_t* type_out, uint8_t* code_out,
                    uint16_t* seq_out, uint64_t timeout_ticks,
                    uint64_t* rtt_us_out, uint16_t expected_seq,
                    uint8_t* ttl_out) {
    uint64_t start = timer_ticks();
    task_t* cur = sched_current();
    for (;;) {
        while (!icmp_reply_ready) {
            if (timer_ticks() - start > timeout_ticks) return -1;
            // Block the task until woken by icmp_rx or timeout
            cur->state = TASK_BLOCKED;
            cur->wait_channel = (void*)&icmp_reply_ready;
            // Set a wakeup deadline so we wake on timeout even without a reply
            uint64_t remaining = timeout_ticks - (timer_ticks() - start);
            if (remaining > timeout_ticks) remaining = 0; // underflow guard
            cur->wakeup_tick = timer_ticks() + remaining;
            sched_schedule();
            cur->wait_channel = NULL;
            cur->wakeup_tick = 0;
        }
        uint64_t flags;
        spin_lock_irqsave(&icmp_reply_lock, &flags);

        int found = -1;
        for (int idx = icmp_reply_head; idx != icmp_reply_tail;
             idx = (idx + 1) % ICMP_REPLY_QUEUE_SIZE) {
            icmp_reply_entry_t* entry = &icmp_reply_queue[idx];
            // Match by (id, seq) for every type we deliver here:
            //   ECHO_REPLY            -> id/seq are the peer's echoed values
            //   DEST_UNREACH /        -> id/seq were extracted from the
            //   TIME_EXCEEDED            embedded ICMP echo header inside
            //                            the error packet by icmp_rx().  If
            //                            the inner packet wasn't ICMP at all
            //                            (e.g. our own kernel returned a
            //                            port-unreachable for an outbound
            //                            DNS UDP query), id/seq stay 0 and
            //                            no real ping waiter matches.
            if (entry->id != expected_id || entry->seq != expected_seq)
                continue;
            found = idx;
            break;
        }

        if (found < 0) {
            // Stale entries (e.g. echo replies for a previous seq) remain in the
            // queue but don't match this caller.  Clear the readiness flag so
            // the outer wait blocks for a new packet instead of busy-spinning
            // through the unmatched queue forever.
            icmp_reply_ready = 0;
            spin_unlock_irqrestore(&icmp_reply_lock, flags);
            continue;
        }

        icmp_reply_entry_t entry = icmp_reply_queue[found];
        while (found != icmp_reply_head) {
            int prev = (found - 1 + ICMP_REPLY_QUEUE_SIZE) % ICMP_REPLY_QUEUE_SIZE;
            icmp_reply_queue[found] = icmp_reply_queue[prev];
            found = prev;
        }
        icmp_reply_head = (icmp_reply_head + 1) % ICMP_REPLY_QUEUE_SIZE;
        icmp_reply_ready = (icmp_reply_head != icmp_reply_tail);
        spin_unlock_irqrestore(&icmp_reply_lock, flags);

        if (src_ip) *src_ip = entry.src_ip;
        if (type_out) *type_out = entry.type;
        if (code_out) *code_out = entry.code;
        if (seq_out) *seq_out = entry.seq;
        if (ttl_out) *ttl_out = entry.ttl;
        if (rtt_us_out) {
            uint64_t t0 = icmp_send_us;
            uint64_t t1 = entry.recv_us;
            *rtt_us_out = (t1 > t0) ? (t1 - t0) : 0;
        }
        return 0;
    }
}

// ============================================================================
// RFC 792 §3.2.1 — Dispatch an inbound ICMP error to its originating socket.
// inner_ip points to the quoted IP header (always 8 bytes of L4 follows).
// ============================================================================
void icmp_dispatch_error(uint8_t icmp_type, uint8_t icmp_code,
                         const uint8_t* inner_ip, uint16_t inner_len,
                         uint16_t mtu_hint) {
    if (inner_len < sizeof(ipv4_header_t) + 8) return;
    const ipv4_header_t* ip = (const ipv4_header_t*)inner_ip;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_header_t) || inner_len < ihl + 8) return;

    uint32_t orig_src = net_ntohl(ip->src_addr);
    uint32_t orig_dst = net_ntohl(ip->dst_addr);
    uint8_t  proto    = ip->protocol;
    const uint8_t* l4 = inner_ip + ihl;
    uint16_t orig_sport = (uint16_t)((l4[0] << 8) | l4[1]);
    uint16_t orig_dport = (uint16_t)((l4[2] << 8) | l4[3]);

    // Map ICMP type/code to errno
    int err = 0;
    switch (icmp_type) {
    case ICMP_DEST_UNREACH:
        switch (icmp_code) {
        case ICMP_NET_UNREACH:   err = ENETUNREACH; break;
        case ICMP_HOST_UNREACH:  err = EHOSTUNREACH; break;
        case ICMP_PROTO_UNREACH: err = ENOPROTOOPT; break;
        case ICMP_PORT_UNREACH:  err = ECONNREFUSED; break;
        case ICMP_FRAG_NEEDED:
            // PMTUD: clamp PMTU cache; do NOT report as connection error
            if (mtu_hint >= 68) {
                pmtu_set(orig_dst, mtu_hint);
                if (proto == IP_PROTO_TCP)
                    tcp_handle_pmtu(orig_src, orig_sport,
                                    orig_dst, orig_dport, mtu_hint);
            }
            return;
        default:                 err = EHOSTUNREACH; break;
        }
        break;
    case ICMP_TIME_EXCEEDED: err = EHOSTUNREACH; break;
    case ICMP_PARAM_PROBLEM: err = EPROTO; break;
    default: return;
    }

    sock_post_icmp_error(proto, orig_src, orig_sport, orig_dst, orig_dport, err);
}
