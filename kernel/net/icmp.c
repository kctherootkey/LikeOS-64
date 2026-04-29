// LikeOS-64 ICMP (Internet Control Message Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/random.h"

// Static send buffer to avoid large stack allocations
static uint8_t icmp_send_buf[NET_MTU_DEFAULT];
static spinlock_t icmp_send_lock = SPINLOCK_INIT("icmp_tx");

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
    if (copy_len > sizeof(icmp_send_buf) - sizeof(icmp_header_t))
        copy_len = (uint16_t)(sizeof(icmp_send_buf) - sizeof(icmp_header_t));

    uint64_t flags;
    spin_lock_irqsave(&icmp_send_lock, &flags);

    icmp_header_t* icmp = (icmp_header_t*)icmp_send_buf;
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->identifier = net_htons((uint16_t)(aux_data >> 16));
    icmp->sequence = net_htons((uint16_t)aux_data);

    for (uint16_t i = 0; i < copy_len; i++)
        icmp_send_buf[sizeof(icmp_header_t) + i] = quoted_ip_packet[i];

    uint16_t total = (uint16_t)(sizeof(icmp_header_t) + copy_len);
    icmp->checksum = ipv4_checksum(icmp_send_buf, total);

    int ret = ipv4_send(dev, dst_ip, IP_PROTO_ICMP, icmp_send_buf, total);
    spin_unlock_irqrestore(&icmp_send_lock, flags);
    return ret;
}

// Process received ICMP packet
void icmp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len, uint8_t rx_ttl) {
    if (len < sizeof(icmp_header_t)) return;

    const icmp_header_t* icmp = (const icmp_header_t*)data;

    // Verify checksum over entire ICMP message
    if (ipv4_checksum(data, len) != 0) return;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        // Send echo reply using static buffer
        if (len > NET_MTU_DEFAULT) return;

        uint64_t flags;
        spin_lock_irqsave(&icmp_send_lock, &flags);

        // Copy entire ICMP message
        for (uint16_t i = 0; i < len; i++)
            icmp_send_buf[i] = data[i];

        // Modify header for reply
        icmp_header_t* r = (icmp_header_t*)icmp_send_buf;
        r->type = ICMP_ECHO_REPLY;
        r->code = 0;
        r->checksum = 0;
        r->checksum = ipv4_checksum(icmp_send_buf, len);

        ipv4_send(dev, src_ip, IP_PROTO_ICMP, icmp_send_buf, len);
        spin_unlock_irqrestore(&icmp_send_lock, flags);
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

    uint64_t flags;
    spin_lock_irqsave(&icmp_send_lock, &flags);

    icmp_header_t* icmp = (icmp_header_t*)icmp_send_buf;
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->identifier = net_htons(id);
    icmp->sequence = net_htons(seq);
    icmp->checksum = 0;

    if (payload && payload_len > 0) {
        for (uint16_t i = 0; i < payload_len; i++)
            icmp_send_buf[sizeof(icmp_header_t) + i] = payload[i];
    }

    icmp->checksum = ipv4_checksum(icmp_send_buf, total);

    // Record send timestamp before transmitting
    icmp_send_us = timer_get_precise_us();

    // Use special send with custom TTL
    int ret = ipv4_send_ttl(dev, dst_ip, IP_PROTO_ICMP, icmp_send_buf, total, ttl);
    spin_unlock_irqrestore(&icmp_send_lock, flags);
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
            if (entry->type == ICMP_ECHO_REPLY &&
                (entry->id != expected_id || entry->seq != expected_seq)) {
                continue;
            }
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
