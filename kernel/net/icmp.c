// LikeOS-64 ICMP (Internet Control Message Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/random.h"

// Static send buffer to avoid large stack allocations
static uint8_t icmp_send_buf[NET_MTU_DEFAULT];
static spinlock_t icmp_send_lock = SPINLOCK_INIT("icmp_tx");

// ICMP reply buffer for ping
static volatile uint32_t icmp_reply_src = 0;
static volatile uint8_t  icmp_reply_type = 0;
static volatile uint8_t  icmp_reply_code = 0;
static volatile uint16_t icmp_reply_id = 0;
static volatile uint16_t icmp_reply_seq = 0;
static volatile uint8_t  icmp_reply_ttl = 0;
static volatile int      icmp_reply_ready = 0;

// Kernel-side timestamps for RTT (microseconds from timer_get_precise_us)
static volatile uint64_t icmp_send_us = 0;
static volatile uint64_t icmp_recv_us = 0;

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
        icmp->type == 11 /* Time Exceeded */) {
        icmp_recv_us = timer_get_precise_us();
        icmp_reply_src = src_ip;
        icmp_reply_type = icmp->type;
        icmp_reply_code = icmp->code;
        icmp_reply_id = net_ntohs(icmp->identifier);
        icmp_reply_seq = net_ntohs(icmp->sequence);
        icmp_reply_ttl = rx_ttl;
        icmp_reply_ready = 1;
        sched_wake_channel((void*)&icmp_reply_ready);
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

    // Clear reply buffer before sending so fast replies aren't lost
    icmp_reply_ready = 0;

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
        // Check if this reply matches our expected ID and seq
        uint16_t rid = icmp_reply_id;
        uint16_t rseq = icmp_reply_seq;
        uint8_t rtype = icmp_reply_type;
        if (rtype == 0 /* ECHO_REPLY */ && (rid != expected_id || rseq != expected_seq)) {
            // Wrong reply — discard and keep waiting
            icmp_reply_ready = 0;
            continue;
        }
        break;
    }
    if (src_ip) *src_ip = icmp_reply_src;
    if (type_out) *type_out = icmp_reply_type;
    if (code_out) *code_out = icmp_reply_code;
    if (seq_out) *seq_out = icmp_reply_seq;
    if (ttl_out) *ttl_out = icmp_reply_ttl;
    if (rtt_us_out) {
        uint64_t t0 = icmp_send_us;
        uint64_t t1 = icmp_recv_us;
        *rtt_us_out = (t1 > t0) ? (t1 - t0) : 0;
    }
    icmp_reply_ready = 0;
    return 0;
}
