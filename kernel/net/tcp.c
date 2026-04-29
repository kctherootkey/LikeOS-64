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

static uint16_t tcp_parse_mss_option(const tcp_header_t* tcp, uint8_t data_offset) {
    if (data_offset <= sizeof(tcp_header_t)) return 0;

    const uint8_t* opts = ((const uint8_t*)tcp) + sizeof(tcp_header_t);
    uint8_t opt_len = (uint8_t)(data_offset - sizeof(tcp_header_t));
    uint8_t idx = 0;

    while (idx < opt_len) {
        uint8_t kind = opts[idx];
        if (kind == TCP_OPT_END) break;
        if (kind == TCP_OPT_NOP) {
            idx++;
            continue;
        }
        if (idx + 1 >= opt_len) break;
        uint8_t len = opts[idx + 1];
        if (len < 2 || idx + len > opt_len) break;
        if (kind == TCP_OPT_MSS && len == TCP_OPT_MSS_LEN) {
            return (uint16_t)((opts[idx + 2] << 8) | opts[idx + 3]);
        }
        idx = (uint8_t)(idx + len);
    }

    return 0;
}

static uint8_t tcp_build_mss_option(uint8_t* options, uint16_t mss) {
    options[0] = TCP_OPT_MSS;
    options[1] = TCP_OPT_MSS_LEN;
    options[2] = (uint8_t)(mss >> 8);
    options[3] = (uint8_t)mss;
    return TCP_OPT_MSS_LEN;
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
                               uint16_t window) {
    uint8_t options[TCP_OPT_MSS_LEN];
    uint8_t opt_len = tcp_build_mss_option(options, tcp_local_mss(dev));
    return tcp_send_segment_ex(dev, src_ip, dst_ip, src_port, dst_port,
                               seq, ack, flags, window, NULL, 0,
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

static tcp_conn_t* tcp_alloc_conn(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!tcp_connections[i].active) {
            tcp_conn_t* conn = &tcp_connections[i];
            conn->active = 1;
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
            conn->lock = (spinlock_t)SPINLOCK_INIT("tcp_conn");

            // Allocate RX/TX buffers
            conn->rx_buf = (uint8_t*)slab_alloc(TCP_RX_BUF_SIZE);
            conn->tx_buf = (uint8_t*)slab_alloc(TCP_TX_BUF_SIZE);
            if (!conn->rx_buf || !conn->tx_buf) {
                if (conn->rx_buf) slab_free(conn->rx_buf);
                if (conn->tx_buf) slab_free(conn->tx_buf);
                conn->active = 0;
                return NULL;
            }
            conn->rx_buf_size = TCP_RX_BUF_SIZE;
            conn->tx_buf_size = TCP_TX_BUF_SIZE;
            conn->rx_head = 0;
            conn->rx_tail = 0;
            conn->tx_head = 0;
            conn->tx_tail = 0;

            return conn;
        }
    }
    return NULL;
}

static void tcp_detach_listener_children(tcp_conn_t* listener) {
    if (!listener) return;

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &tcp_connections[i];
        if (conn->active && conn->parent == listener)
            conn->parent = NULL;
    }
}

static void tcp_free_conn(tcp_conn_t* conn) {
    if (conn->rx_buf) slab_free(conn->rx_buf);
    if (conn->tx_buf) slab_free(conn->tx_buf);
    conn->rx_buf = NULL;
    conn->tx_buf = NULL;
    conn->parent = NULL;
    conn->active = 0;
    conn->state = TCP_STATE_CLOSED;
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
    tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                     conn->local_port, conn->remote_port,
                     conn->snd_nxt, conn->rcv_nxt,
                     TCP_ACK, (uint16_t)conn->rcv_wnd,
                     NULL, 0);
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
    spin_lock_irqsave(&tcp_lock, &flags);

    tcp_conn_t* conn = tcp_alloc_conn();
    if (!conn) {
        spin_unlock_irqrestore(&tcp_lock, flags);
        return NULL;
    }

    conn->dev = dev;
    conn->local_ip = local_ip;
    conn->remote_ip = dst_ip;
    conn->local_port = src_port;
    conn->remote_port = dst_port;
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

    spin_unlock_irqrestore(&tcp_lock, flags);

    // Send SYN
    tcp_send_syn_packet(dev, local_ip, dst_ip, src_port, dst_port,
                        conn->iss, 0, TCP_SYN, TCP_WINDOW_SIZE);

    return conn;
}

// ============================================================================
// TCP Listen (passive open)
// ============================================================================
tcp_conn_t* tcp_listen(net_device_t* dev, uint32_t local_ip,
                       uint16_t local_port, int backlog) {
    uint64_t flags;
    spin_lock_irqsave(&tcp_lock, &flags);

    tcp_conn_t* conn = tcp_alloc_conn();
    if (!conn) {
        spin_unlock_irqrestore(&tcp_lock, flags);
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

    spin_unlock_irqrestore(&tcp_lock, flags);
    return conn;
}

// ============================================================================
// TCP Accept (from listener)
// ============================================================================
tcp_conn_t* tcp_accept(tcp_conn_t* listener) {
    if (!listener || listener->state != TCP_STATE_LISTEN) return NULL;

    uint64_t flags;
    spin_lock_irqsave(&listener->lock, &flags);

    if (listener->accept_head != listener->accept_tail) {
        tcp_conn_t* conn = listener->accept_queue[listener->accept_head];
        listener->accept_head = (listener->accept_head + 1) % 16;
        if (listener->accept_head == listener->accept_tail)
            listener->accept_ready = 0;
        if (conn)
            conn->parent = NULL;

        spin_unlock_irqrestore(&listener->lock, flags);
        return conn;
    }

    spin_unlock_irqrestore(&listener->lock, flags);

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
    spin_lock_irqsave(&conn->lock, &flags);

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
        spin_unlock_irqrestore(&conn->lock, flags);
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
        spin_unlock_irqrestore(&conn->lock, flags);
        break;

    case TCP_STATE_LISTEN:
        spin_unlock_irqrestore(&conn->lock, flags);

        spin_lock_irqsave(&tcp_lock, &flags);
        tcp_detach_listener_children(conn);
        spin_unlock_irqrestore(&tcp_lock, flags);

        conn->state = TCP_STATE_CLOSED;
        tcp_free_conn(conn);
        break;

    case TCP_STATE_SYN_SENT:
        conn->state = TCP_STATE_CLOSED;
        spin_unlock_irqrestore(&conn->lock, flags);
        tcp_free_conn(conn);
        break;

    default:
        spin_unlock_irqrestore(&conn->lock, flags);
        break;
    }

    return 0;
}

// ============================================================================
// TCP Send Data
// ============================================================================
int tcp_send_data(tcp_conn_t* conn, const uint8_t* data, uint16_t len) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) return -1;

    uint64_t flags;
    spin_lock_irqsave(&conn->lock, &flags);

    if (len == 0) return 0;

    uint16_t sent = 0;
    uint16_t seg_mss = tcp_effective_mss(conn);

    while (sent < len) {
        if (conn->inflight_count >= TCP_MAX_INFLIGHT) {
            conn->tx_ready = 0;
            break;
        }

        uint16_t seg_len = (uint16_t)(len - sent);
        if (seg_len > seg_mss) seg_len = seg_mss;

        if (tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                             conn->local_port, conn->remote_port,
                             conn->snd_nxt, conn->rcv_nxt,
                             TCP_ACK | TCP_PSH, (uint16_t)conn->rcv_wnd,
                             data + sent, seg_len) < 0) {
            break;
        }

        tcp_queue_inflight(conn, conn->snd_nxt, TCP_ACK | TCP_PSH,
                           data + sent, seg_len);
        conn->snd_nxt += seg_len;
        sent += seg_len;
    }

    if (sent == 0) {
        conn->tx_ready = 0;
        spin_unlock_irqrestore(&conn->lock, flags);
        return 0;
    }

    conn->retransmit_tick = timer_ticks() + TCP_RETRANSMIT_TICKS;
    conn->retransmit_count = 0;

    spin_unlock_irqrestore(&conn->lock, flags);
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
    uint16_t peer_mss = tcp_parse_mss_option(tcp, data_offset);

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

            // New connection on listening socket
            uint64_t flags;
            spin_lock_irqsave(&tcp_lock, &flags);

            tcp_conn_t* new_conn = tcp_alloc_conn();
            if (!new_conn) {
                spin_unlock_irqrestore(&tcp_lock, flags);
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
            new_conn->iss = tcp_generate_isn(local_ip, src_ip, dst_port, src_port);
            new_conn->irs = seq;
            new_conn->snd_una = new_conn->iss;
            new_conn->snd_nxt = new_conn->iss + 1;
            new_conn->rcv_nxt = seq + 1;
            new_conn->snd_wnd = window;
            new_conn->rcv_wnd = TCP_WINDOW_SIZE;
            new_conn->peer_mss = peer_mss ? peer_mss : TCP_MSS;
            new_conn->max_seg_size = new_conn->peer_mss;
            new_conn->state = TCP_STATE_SYN_RECEIVED;
            new_conn->parent = listener;

            spin_unlock_irqrestore(&tcp_lock, flags);

            // Send SYN+ACK
            tcp_send_syn_packet(dev, local_ip, src_ip, dst_port, src_port,
                                new_conn->iss, new_conn->rcv_nxt,
                                TCP_SYN | TCP_ACK, TCP_WINDOW_SIZE);
            return;
        }

        // SYN cookie validation: ACK for unknown connection with a listener
        if (listener && (tcp_flags & TCP_ACK) && !(tcp_flags & TCP_SYN)) {
            uint32_t cookie = ack - 1; // The ISN we sent was cookie, client ACKs cookie+1
            uint16_t cookie_mss = 0;
            if (tcp_syncookie_validate(src_ip, local_ip, src_port, dst_port,
                                       cookie, &cookie_mss)) {
                // Valid SYN cookie - create connection directly in ESTABLISHED state
                uint64_t flags;
                spin_lock_irqsave(&tcp_lock, &flags);

                tcp_conn_t* new_conn = tcp_alloc_conn();
                if (!new_conn) {
                    spin_unlock_irqrestore(&tcp_lock, flags);
                    return;
                }

                new_conn->dev = dev;
                new_conn->local_ip = local_ip;
                new_conn->remote_ip = src_ip;
                new_conn->local_port = dst_port;
                new_conn->remote_port = src_port;
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

                // Enqueue to listener accept queue
                int next_tail = (listener->accept_tail + 1) % 16;
                if (next_tail != listener->accept_head) {
                    listener->accept_queue[listener->accept_tail] = new_conn;
                    listener->accept_tail = next_tail;
                    listener->accept_ready = 1;
                }

                spin_unlock_irqrestore(&tcp_lock, flags);
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
    spin_lock_irqsave(&conn->lock, &flags);

    // Process by state
    switch (conn->state) {
    case TCP_STATE_SYN_SENT:
        if ((tcp_flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == conn->snd_nxt) {
                conn->irs = seq;
                conn->rcv_nxt = seq + 1;
                conn->snd_una = ack;
                conn->snd_wnd = window;
                conn->peer_mss = peer_mss ? peer_mss : TCP_MSS;
                conn->max_seg_size = conn->peer_mss;
                conn->state = TCP_STATE_ESTABLISHED;
                conn->connect_done = 1;

                // Send ACK
                tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                                 conn->local_port, conn->remote_port,
                                 conn->snd_nxt, conn->rcv_nxt,
                                 TCP_ACK, (uint16_t)conn->rcv_wnd, NULL, 0);
            }
        } else if (tcp_flags & TCP_RST) {
            tcp_fail_connection(conn, ECONNREFUSED);
        }
        break;

    case TCP_STATE_SYN_RECEIVED:
        if (tcp_flags & TCP_ACK) {
            if (ack == conn->snd_nxt) {
                conn->snd_una = ack;
                conn->snd_wnd = window;
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
            spin_unlock_irqrestore(&conn->lock, flags);
            tcp_free_conn(conn);
            return;
        }
        break;

    case TCP_STATE_ESTABLISHED:
        if (tcp_flags & TCP_RST) {
            tcp_fail_connection(conn, ECONNRESET);
            break;
        }

        // Process ACK
        if (tcp_flags & TCP_ACK) {
            if (ack > conn->snd_una && ack <= conn->snd_nxt) {
                conn->snd_una = ack;
                conn->snd_wnd = window;
                tcp_ack_inflight(conn, ack);
                conn->retransmit_count = 0;
                conn->retransmit_tick = timer_ticks() + TCP_RETRANSMIT_TICKS;
                conn->tx_ready = conn->inflight_count < TCP_MAX_INFLIGHT;
            }
        }

        // Process data
        if (payload_len > 0 && seq == conn->rcv_nxt) {
            // Copy to receive buffer
            uint32_t avail = ring_free(conn->rx_head, conn->rx_tail, conn->rx_buf_size);
            uint32_t copy = payload_len;
            if (copy > avail) copy = avail;

            for (uint32_t i = 0; i < copy; i++) {
                conn->rx_buf[conn->rx_tail] = payload[i];
                conn->rx_tail = (conn->rx_tail + 1) % conn->rx_buf_size;
            }
            conn->rcv_nxt += copy;
            conn->rx_ready = 1;

            // Send ACK
            tcp_send_ack(conn);
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
            spin_unlock_irqrestore(&conn->lock, flags);
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

    spin_unlock_irqrestore(&conn->lock, flags);
}

// ============================================================================
// TCP Timer (called from net_timer_tick, ~100Hz)
// ============================================================================
void tcp_timer_tick(void) {
    uint64_t now = timer_ticks();

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &tcp_connections[i];
        if (!conn->active) continue;

        // TIME_WAIT expiry
        if (conn->state == TCP_STATE_TIME_WAIT && now >= conn->time_wait_tick) {
            conn->state = TCP_STATE_CLOSED;
            tcp_free_conn(conn);
            continue;
        }

        // Retransmission timeout
        if (conn->state == TCP_STATE_SYN_SENT && now >= conn->retransmit_tick) {
            if (conn->retransmit_count >= TCP_MAX_RETRANSMITS) {
                tcp_fail_connection(conn, ETIMEDOUT);
                continue;
            }
            // Retransmit SYN
            tcp_send_syn_packet(conn->dev, conn->local_ip, conn->remote_ip,
                                conn->local_port, conn->remote_port,
                                conn->iss, 0, TCP_SYN, TCP_WINDOW_SIZE);
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
                continue;
            }

            tcp_inflight_segment_t* seg = &conn->inflight[0];
            tcp_send_segment(conn->dev, conn->local_ip, conn->remote_ip,
                             conn->local_port, conn->remote_port,
                             seg->seq, conn->rcv_nxt, seg->flags,
                             (uint16_t)conn->rcv_wnd, seg->data, seg->len);
            seg->retransmit_count++;
            conn->retransmit_count++;
            conn->retransmit_tick = now + TCP_RETRANSMIT_TICKS;
        }
    }
}
