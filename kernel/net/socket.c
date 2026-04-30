// LikeOS-64 Socket Layer
// Provides kernel-side socket abstraction for syscall layer

#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/pipe.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/random.h"
#include "../../include/kernel/sched.h"

// Socket table
static net_socket_t sockets[NET_MAX_SOCKETS];
static spinlock_t socket_lock = SPINLOCK_INIT("socket");

// IRQ-friendly blocking acquire of a TCP per-connection spinlock.
// Mirrors tcp_lock_acquire() in tcp.c — between trylock attempts IRQs
// are re-enabled so this CPU keeps ACKing TLB shootdown IPIs.  Avoids
// the OS-wide multi-second freeze (`SMP: TLB shootdown sync timeout`)
// that occurs when sock_recv spins IRQs-off on conn->lock while another
// CPU's slab_free or sched_remove_task initiates a TLB shootdown.
static inline void sock_conn_lock(spinlock_t* lock, uint64_t* flags_out) {
    uint64_t f = local_irq_save();
    while (!spin_trylock(lock)) {
        local_irq_restore(f);
        __asm__ volatile("pause" ::: "memory");
        f = local_irq_save();
    }
    *flags_out = f;
}

static inline void sock_conn_unlock(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    local_irq_restore(flags);
}

// Ephemeral port counter (49152-65535)
static uint16_t next_ephemeral_port = 49152;

void socket_init(void) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        sockets[i].active = 0;
    }
}

static uint16_t alloc_ephemeral_port(void) {
    // Randomized ephemeral port allocation
    for (int attempt = 0; attempt < 128; attempt++) {
        uint16_t port = 49152 + (uint16_t)(random_u32() % 16384);
        // Check for collision with bound sockets
        int in_use = 0;
        for (int i = 0; i < NET_MAX_SOCKETS; i++) {
            if (sockets[i].active && sockets[i].bound &&
                net_ntohs(sockets[i].local_addr.sin_port) == port) {
                in_use = 1;
                break;
            }
        }
        if (!in_use) return port;
    }
    // Fallback: sequential
    uint16_t port = next_ephemeral_port++;
    if (next_ephemeral_port < 49152)
        next_ephemeral_port = 49152;
    return port;
}

// Find a UDP socket bound to a given port and destination IP (called from udp.c)
net_socket_t* sock_find_udp(uint16_t port, uint32_t dst_ip) {
    net_socket_t* wildcard = NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t* s = &sockets[i];
        if (s->active && s->type == SOCK_DGRAM && s->bound &&
            net_ntohs(s->local_addr.sin_port) == port) {
            uint32_t local_ip = net_ntohl(s->local_addr.sin_addr.s_addr);
            if (local_ip == dst_ip)
                return s;
            if (local_ip == 0 && !wildcard)
                wildcard = s;
        }
    }
    return wildcard;
}

// ============================================================================
// sock_create - Create a new socket
// Returns socket descriptor (index into socket table) or negative errno
// ============================================================================
int sock_create(int domain, int type, int protocol) {
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return -ESOCKTNOSUPPORT;

    uint64_t flags;
    spin_lock_irqsave(&socket_lock, &flags);

    int fd = -ENOMEM;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            net_socket_t* s = &sockets[i];
            // Zero the entire struct first so any new fields default to 0.
            for (size_t b = 0; b < sizeof(*s); b++) ((uint8_t*)s)[b] = 0;
            s->type = type;
            if (type == SOCK_STREAM)      s->protocol = IPPROTO_TCP;
            else if (type == SOCK_DGRAM)  s->protocol = IPPROTO_UDP;
            else                          s->protocol = (uint8_t)protocol;
            s->domain = domain;
            s->ref_count = 1;
            s->active = 1;
            // Sensible defaults
            s->ip_ttl = 64;
            s->mcast_ttl = 1;
            s->mcast_loop = 1;
            s->sndbuf_size = 65536;
            s->rcvbuf_size = 65536;
            s->lock = (spinlock_t)SPINLOCK_INIT("sock");
            s->local_addr.sin_family = AF_INET;
            s->remote_addr.sin_family = AF_INET;

            fd = i;
            break;
        }
    }

    spin_unlock_irqrestore(&socket_lock, flags);
    return fd;
}

// ============================================================================
// sock_bind - Bind socket to a local address
// ============================================================================
int sock_bind(int sockfd, const struct sockaddr_in* addr) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;
    if (s->bound) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    s->local_addr = *addr;
    if (s->local_addr.sin_port == 0) {
        s->local_addr.sin_port = net_htons(alloc_ephemeral_port());
    }
    s->bound = 1;
    spin_unlock_irqrestore(&s->lock, flags);

    return 0;
}

// ============================================================================
// sock_listen - Mark socket as listening (TCP only)
// ============================================================================
int sock_listen(int sockfd, int backlog) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (!s->bound) return -EINVAL;

    net_device_t* dev = net_get_default_device();
    if (!dev) return -ENETDOWN;

    uint32_t local_ip = net_ntohl(s->local_addr.sin_addr.s_addr);
    uint16_t local_port = net_ntohs(s->local_addr.sin_port);

    tcp_conn_t* conn = tcp_listen(dev, local_ip, local_port,
                                   backlog > 0 ? backlog : 5);
    if (!conn) return -ENOMEM;

    uint64_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    s->tcp = conn;
    s->listening = 1;
    spin_unlock_irqrestore(&s->lock, flags);

    return 0;
}

// ============================================================================
// sock_accept - Accept a connection (TCP only, blocking)
// ============================================================================
int sock_accept(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active || !s->listening || !s->tcp) return -EINVAL;

    // Wait for a connection
    uint64_t deadline = s->rcv_timeout_ticks ?
        timer_ticks() + s->rcv_timeout_ticks : 0;

    tcp_conn_t* new_conn = NULL;
    while (1) {
        new_conn = tcp_accept(s->tcp);
        if (new_conn) break;
        if (s->nonblock) return -EAGAIN;
        if (deadline && timer_ticks() >= deadline) return -ETIMEDOUT;
        loopback_process_pending();
        sched_yield_in_kernel();
    }

    // Create a new socket for the accepted connection
    int newfd = sock_create(AF_INET, SOCK_STREAM, 0);
    if (newfd < 0) {
        tcp_close(new_conn);
        return newfd;
    }

    net_socket_t* ns = &sockets[newfd];
    ns->tcp = new_conn;
    ns->connected = 1;
    ns->bound = 1;
    ns->local_addr.sin_family = AF_INET;
    ns->local_addr.sin_port = net_htons(new_conn->local_port);
    ns->local_addr.sin_addr.s_addr = net_htonl(new_conn->local_ip);
    ns->remote_addr.sin_family = AF_INET;
    ns->remote_addr.sin_port = net_htons(new_conn->remote_port);
    ns->remote_addr.sin_addr.s_addr = net_htonl(new_conn->remote_ip);

    // Fill in caller's address if requested
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        *addr = ns->remote_addr;
        *addrlen = sizeof(struct sockaddr_in);
    }

    return newfd;
}

// ============================================================================
// sock_connect - Connect to remote host (TCP or set UDP default dest)
// ============================================================================
int sock_connect(int sockfd, const struct sockaddr_in* addr) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;
    if (s->connected) return -EISCONN;

    uint32_t dst_ip = net_ntohl(addr->sin_addr.s_addr);
    uint32_t next_hop = dst_ip;
    net_device_t* dev = route_lookup(dst_ip, &next_hop);
    if (!dev)
        dev = net_get_default_device();
    if (!dev) return -ENETDOWN;

    if (!s->bound) {
        s->local_addr.sin_port = net_htons(alloc_ephemeral_port());
        s->local_addr.sin_addr.s_addr = net_htonl(dev->ip_addr);
        s->local_addr.sin_family = AF_INET;
        s->bound = 1;
    }

    s->remote_addr = *addr;

    if (s->type == SOCK_DGRAM) {
        // UDP: just set default destination
        s->connected = 1;
        return 0;
    }

    // TCP: initiate connection
    uint16_t dst_port = net_ntohs(addr->sin_port);
    uint16_t src_port = net_ntohs(s->local_addr.sin_port);
    uint32_t local_ip = net_ntohl(s->local_addr.sin_addr.s_addr);

    tcp_conn_t* conn = tcp_connect(dev, local_ip, dst_ip, src_port, dst_port);
    if (!conn) return -ENOMEM;

    s->tcp = conn;

    // Wait for connection to complete (blocking)
    uint64_t deadline = s->rcv_timeout_ticks ?
        timer_ticks() + s->rcv_timeout_ticks : 0;

    while (!conn->connect_done) {
        if (s->nonblock) return -EINPROGRESS;
        if (deadline && timer_ticks() >= deadline) return -ETIMEDOUT;
        loopback_process_pending();
        sched_yield_in_kernel();
    }

    if (conn->error) {
        int err = conn->error;
        tcp_close(conn);
        s->tcp = NULL;
        return -err;
    }

    s->connected = 1;
    return 0;
}

// ============================================================================
// sock_sendto - Send data (UDP or unconnected)
// ============================================================================
int sock_sendto(int sockfd, const void* buf, size_t len, int flags,
                const struct sockaddr_in* dest_addr, socklen_t addrlen) {
    (void)flags;
    (void)addrlen;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;
    // Surface any pending async error (e.g. ICMP port-unreach) once
    if (s->error) {
        int e = s->error; s->error = 0; return -e;
    }

    // SOCK_RAW: caller may supply a full IP packet (IP_HDRINCL) or a payload.
    if (s->type == SOCK_RAW) {
        const struct sockaddr_in* target = dest_addr ? dest_addr : &s->remote_addr;
        uint32_t dst_ip = net_ntohl(target->sin_addr.s_addr);
        uint32_t next_hop = dst_ip;
        net_device_t* dev = route_lookup(dst_ip, &next_hop);
        if (!dev) dev = net_get_default_device();
        if (!dev) return -ENETDOWN;
        if (s->ip_hdrincl) {
            // Caller-supplied IP header — forward as-is via raw L3 send hook
            extern int ipv4_send_raw(net_device_t*, uint32_t,
                                     const uint8_t*, uint16_t);
            return ipv4_send_raw(dev, dst_ip, (const uint8_t*)buf, (uint16_t)len);
        }
        return ipv4_send_full(dev, dst_ip, (uint8_t)s->protocol,
                              (const uint8_t*)buf, (uint16_t)len,
                              s->ip_ttl ? s->ip_ttl : 64, s->ip_tos);
    }

    if (s->type == SOCK_DGRAM) {
        const struct sockaddr_in* target = dest_addr ? dest_addr : &s->remote_addr;
        if (target->sin_port == 0 && !s->connected) return -EDESTADDRREQ;

        uint32_t dst_ip = net_ntohl(target->sin_addr.s_addr);
        uint16_t dst_port = net_ntohs(target->sin_port);

        if (len > 0xFFFFU - 28U)
            return -EINVAL;

        // SO_BROADCAST gate: refuse class-D/limited bcast if not enabled
        int is_bcast = (dst_ip == 0xFFFFFFFFU);
        int is_dirbcast = 0;
        if (!is_bcast) {
            // Directed broadcast == host-portion all-ones for some local subnet
            for (int dn = 0; dn < 8; dn++) {
                net_device_t* dd = net_get_device(dn);
                if (!dd) continue;
                if (dd->ip_addr && dd->netmask &&
                    ((dst_ip | dd->netmask) == 0xFFFFFFFFU) &&
                    ((dst_ip & dd->netmask) == (dd->ip_addr & dd->netmask))) {
                    is_dirbcast = 1; break;
                }
            }
        }
        if ((is_bcast || is_dirbcast) && !s->broadcast)
            return -EACCES;

        uint32_t next_hop = dst_ip;
        net_device_t* dev = route_lookup(dst_ip, &next_hop);
        if (!dev)
            dev = net_get_default_device();
        if (!dev) return -ENETDOWN;

        // SO_BINDTODEVICE: if specified, override route choice
        if (s->bindtodevice[0]) {
            for (int dn = 0; dn < 8; dn++) {
                net_device_t* dd = net_get_device(dn);
                if (!dd) continue;
                int match = 1;
                for (int k = 0; k < 16 && s->bindtodevice[k]; k++) {
                    if (dd->name[k] != s->bindtodevice[k]) { match = 0; break; }
                }
                if (match) { dev = dd; break; }
            }
        }

        if (!s->bound) {
            s->local_addr.sin_port = net_htons(alloc_ephemeral_port());
            s->local_addr.sin_addr.s_addr = net_htonl(dev->ip_addr);
            s->bound = 1;
        }

        uint16_t src_port = net_ntohs(s->local_addr.sin_port);
        uint16_t send_len = (uint16_t)len;

        smap_disable();
        int ret = udp_send(dev, dst_ip, src_port, dst_port,
                           (const uint8_t*)buf, send_len);
        smap_enable();
        return ret < 0 ? ret : (int)send_len;
    }

    // TCP sendto - ignore dest_addr, use connection
    return sock_send(sockfd, buf, len, flags);
}

// ============================================================================
// sock_recvfrom - Receive data (UDP with source address)
// ============================================================================
int sock_recvfrom(int sockfd, void* buf, size_t len, int flags,
                  struct sockaddr_in* src_addr, socklen_t* addrlen) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;
    int peek = (flags & MSG_PEEK) != 0;
    int dontwait = (flags & MSG_DONTWAIT) != 0;

    if (s->type == SOCK_DGRAM || s->type == SOCK_RAW) {
        // Wait for data
        uint64_t deadline = s->rcv_timeout_ticks ?
            timer_ticks() + s->rcv_timeout_ticks : 0;

        while (!s->udp_rx_ready) {
            if (s->nonblock || dontwait) return -EAGAIN;
            if (deadline && timer_ticks() >= deadline) return -ETIMEDOUT;
            loopback_process_pending();
            sched_yield_in_kernel();
        }

        uint64_t sflags;
        spin_lock_irqsave(&s->lock, &sflags);

        if (s->udp_rx_head == s->udp_rx_tail) {
            s->udp_rx_ready = 0;
            spin_unlock_irqrestore(&s->lock, sflags);
            return -EAGAIN;
        }

        int idx = s->udp_rx_head;
        uint16_t data_len = s->udp_rx_queue[idx].len;
        if (data_len > len) data_len = (uint16_t)len;

        smap_disable();
        for (uint16_t i = 0; i < data_len; i++)
            ((uint8_t*)buf)[i] = s->udp_rx_queue[idx].data[i];
        smap_enable();

        if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
            *src_addr = s->udp_rx_queue[idx].from;
            *addrlen = sizeof(struct sockaddr_in);
        }

        if (!peek) {
            s->udp_rx_head = (s->udp_rx_head + 1) % 16;
            if (s->udp_rx_head == s->udp_rx_tail)
                s->udp_rx_ready = 0;
        }

        spin_unlock_irqrestore(&s->lock, sflags);
        return data_len;
    }

    // TCP recvfrom
    return sock_recv(sockfd, buf, len, flags);
}

// ============================================================================
// sock_send - Send data on connected socket (TCP)
// ============================================================================
int sock_send(int sockfd, const void* buf, size_t len, int flags) {
    (void)flags;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    if (s->type == SOCK_STREAM) {
        if (!s->tcp || !s->connected) return -ENOTCONN;
        if (s->tcp->state != TCP_STATE_ESTABLISHED) return -EPIPE;

        // tcp_send_data may return 0 when the inflight queue is full.  POSIX
        // forbids returning 0 from send() with non-zero len (callers treat it
        // as EOF), so block until at least one byte gets queued, or report
        // EAGAIN for nonblocking sockets.
        uint16_t to_send = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
        if (to_send == 0) return 0;

        for (;;) {
            if (!s->tcp || !s->connected) return -ENOTCONN;
            if (s->tcp->state != TCP_STATE_ESTABLISHED) return -EPIPE;

            smap_disable();
            int ret = tcp_send_data(s->tcp, (const uint8_t*)buf, to_send);
            smap_enable();

            if (ret < 0) return ret;
            if (ret > 0) return ret;

            if (s->nonblock) return -EAGAIN;
            loopback_process_pending();
            sched_yield_in_kernel();
        }
    }

    // UDP send (connected)
    if (!s->connected) return -EDESTADDRREQ;
    return sock_sendto(sockfd, buf, len, flags, &s->remote_addr, sizeof(struct sockaddr_in));
}

// ============================================================================
// sock_recv - Receive data on connected socket (TCP)
// ============================================================================
int sock_recv(int sockfd, void* buf, size_t len, int flags) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;
    int peek = (flags & MSG_PEEK) != 0;
    int dontwait = (flags & MSG_DONTWAIT) != 0;
    int waitall = (flags & MSG_WAITALL) != 0;

    if (s->type == SOCK_STREAM) {
        if (!s->tcp) return -ENOTCONN;

        tcp_conn_t* conn = s->tcp;
        uint64_t deadline = s->rcv_timeout_ticks ?
            timer_ticks() + s->rcv_timeout_ticks : 0;
        size_t total = 0;
again:
        // Wait for data
        while (!conn->rx_ready && conn->state == TCP_STATE_ESTABLISHED) {
            if (s->nonblock || dontwait) return total ? (int)total : -EAGAIN;
            if (deadline && timer_ticks() >= deadline)
                return total ? (int)total : -ETIMEDOUT;
            loopback_process_pending();
            sched_yield_in_kernel();
        }

        if (conn->error) return total ? (int)total : -conn->error;

        // Connection closed - return 0 (EOF)
        if (conn->state == TCP_STATE_CLOSE_WAIT ||
            conn->state == TCP_STATE_CLOSED) {
            if (conn->rx_head == conn->rx_tail) return (int)total;
        }

        // Copy from rx buffer
        uint64_t cflags;
        sock_conn_lock(&conn->lock, &cflags);

        uint32_t avail = (conn->rx_tail - conn->rx_head + conn->rx_buf_size) % conn->rx_buf_size;
        uint32_t copy = avail;
        if (copy > (len - total)) copy = (uint32_t)(len - total);

        smap_disable();
        for (uint32_t i = 0; i < copy; i++) {
            ((uint8_t*)buf)[total + i] = conn->rx_buf[(conn->rx_head + i) % conn->rx_buf_size];
        }
        smap_enable();
        if (!peek) {
            conn->rx_head = (conn->rx_head + copy) % conn->rx_buf_size;
            if (conn->rx_head == conn->rx_tail)
                conn->rx_ready = 0;
        }

        sock_conn_unlock(&conn->lock, cflags);
        total += copy;

        if (waitall && !peek && total < len &&
            conn->state == TCP_STATE_ESTABLISHED)
            goto again;
        return (int)total;
    }

    // UDP recv (connected, no src addr)
    return sock_recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

// ============================================================================
// sock_close - Close a socket
// ============================================================================
int sock_close(int sockfd) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    // Drop one reference; only tear down on the final close.  The same socket
    // index is shared between parent and child after fork(), so a premature
    // teardown here would yank the listener out from under a peer that is
    // still blocked in accept() (observed: child exits, its fd cleanup closes
    // the inherited listener fd, parent's accept() loops forever on a freed
    // tcp_conn_t).  fork()/dup() bump ref_count without holding s->lock, so
    // the decrement must be atomic to race-correctly with them.
    int old = __atomic_sub_fetch(&s->ref_count, 1, __ATOMIC_ACQ_REL);
    if (old > 0) {
        return 0;
    }

    // We own the teardown path now (ref_count hit zero).  CRITICAL: do NOT
    // hold s->lock across tcp_close/tcp_abort.  Those call tcp_send_segment
    // (FIN packet, may queue+drain a softirq), tcp_free_conn → slab_free
    // (initiates a TLB-shootdown IPI on freed buffer pages), and
    // tcp_reap_pending (more slab_free).  Any of these can take many tens
    // of microseconds.  spin_lock_irqsave(&s->lock) keeps IRQs disabled
    // for that entire window, so this CPU cannot ACK TLB-shootdown IPIs
    // sent by *other* CPUs (e.g. a sibling task's process-exit
    // sched_remove_task → smp_tlb_shootdown_sync).  That is exactly the
    // `SMP: TLB shootdown sync timeout (ack=N expect=N+1)` scenario,
    // followed by an OS-wide multi-second freeze.
    //
    // Once ref_count is zero no other thread can re-enter sock_close on
    // this slot, and fork()/dup() refuse to bump a zero ref_count.  TCP
    // teardown takes its own conn->lock with the IRQ-friendly helper.
    tcp_conn_t* tcp_to_teardown = s->tcp;
    uint8_t  do_linger_block = 0;
    uint16_t linger_secs     = 0;
    if (tcp_to_teardown) {
        // SO_LINGER: l_onoff=1, l_linger=0 → abort with RST (RFC 1122 §4.2.2.13)
        if (s->linger_onoff && s->linger_seconds == 0) {
            tcp_abort(tcp_to_teardown);
        } else if (s->linger_onoff && s->linger_seconds > 0) {
            do_linger_block = 1;
            linger_secs = s->linger_seconds;
            tcp_close(tcp_to_teardown);
        } else {
            tcp_close(tcp_to_teardown);
        }
    }

    if (do_linger_block && tcp_to_teardown) {
        uint64_t deadline = timer_ticks() + (uint64_t)linger_secs * 100;
        while (tcp_to_teardown->state != TCP_STATE_CLOSED &&
               tcp_to_teardown->state != TCP_STATE_TIME_WAIT &&
               timer_ticks() < deadline) {
            sched_yield_in_kernel();
        }
    }

    // Final state flip under s->lock (cheap — just stores).
    uint64_t flags;
    spin_lock_irqsave(&s->lock, &flags);
    s->tcp     = NULL;
    s->closed  = 1;
    s->active  = 0;
    spin_unlock_irqrestore(&s->lock, flags);
    return 0;
}

// ============================================================================
// sock_shutdown - Shutdown part of a connection
// ============================================================================
int sock_shutdown(int sockfd, int how) {
    (void)how;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    if (s->type == SOCK_STREAM && s->tcp) {
        tcp_close(s->tcp);
        s->tcp = NULL;
        s->connected = 0;
        s->listening = 0;
    }

    return 0;
}

// ============================================================================
// sock_setsockopt / sock_getsockopt
// ============================================================================
int sock_setsockopt(int sockfd, int level, int optname,
                    const void* optval, socklen_t optlen) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    int ival = 0;
    if (optval && optlen >= sizeof(int)) ival = *(const int*)optval;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR:    s->reuse_addr = ival; return 0;
        case SO_REUSEPORT:    s->reuse_port = ival; return 0;
        case SO_BROADCAST:    s->broadcast = ival; return 0;
        case SO_OOBINLINE:    s->oobinline = ival; return 0;
        case SO_KEEPALIVE:
            if (s->tcp) s->tcp->keepalive = ival ? 1 : 0;
            return 0;
        case SO_SNDBUF:
            if (ival > 0) s->sndbuf_size = (uint32_t)ival;
            return 0;
        case SO_RCVBUF:
            if (ival > 0) s->rcvbuf_size = (uint32_t)ival;
            return 0;
        case SO_RCVTIMEO:
            if (optlen >= sizeof(uint64_t))
                s->rcv_timeout_ticks = *(const uint64_t*)optval / 10;
            else if (optlen >= sizeof(int))
                s->rcv_timeout_ticks = (uint64_t)ival / 10;
            return 0;
        case SO_SNDTIMEO:
            return 0;  // accepted; not enforced
        case SO_LINGER: {
            if (optlen >= sizeof(struct linger)) {
                const struct linger* l = (const struct linger*)optval;
                s->linger_onoff = (uint8_t)l->l_onoff;
                s->linger_seconds = (uint16_t)l->l_linger;
            }
            return 0;
        }
        case SO_BINDTODEVICE: {
            socklen_t n = optlen;
            if (n > 15) n = 15;
            for (socklen_t i = 0; i < n; i++)
                s->bindtodevice[i] = ((const char*)optval)[i];
            s->bindtodevice[n] = 0;
            return 0;
        }
        default:
            return -ENOPROTOOPT;
        }
    }

    if (level == SOL_IP || level == IPPROTO_IP) {
        switch (optname) {
        case IP_TTL:               if (ival > 0 && ival < 256) s->ip_ttl = (uint8_t)ival; return 0;
        case IP_TOS:               s->ip_tos = (uint8_t)ival; return 0;
        case IP_HDRINCL:           s->ip_hdrincl = ival ? 1 : 0; return 0;
        case IP_PKTINFO:           s->ip_recvpktinfo = ival ? 1 : 0; return 0;
        case IP_RECVTTL:           s->ip_recvttl = ival ? 1 : 0; return 0;
        case IP_RECVTOS:           s->ip_recvtos = ival ? 1 : 0; return 0;
        case IP_MTU_DISCOVER:      return 0;  // accepted; we only do PMTUD
        case IP_OPTIONS:           return 0;  // accepted; not emitted
        case IP_MULTICAST_TTL:     s->mcast_ttl = (uint8_t)(ival & 0xFF); return 0;
        case IP_MULTICAST_LOOP:    s->mcast_loop = ival ? 1 : 0; return 0;
        case IP_MULTICAST_IF:
            if (optlen >= sizeof(uint32_t))
                s->mcast_if = net_ntohl(*(const uint32_t*)optval);
            return 0;
        case IP_ADD_MEMBERSHIP: {
            if (optlen < sizeof(struct ip_mreq)) return -EINVAL;
            const struct ip_mreq* mr = (const struct ip_mreq*)optval;
            uint32_t group = net_ntohl(mr->imr_multiaddr.s_addr);
            // Track on socket so we can leave on close.
            int slot = -1;
            for (int i = 0; i < NET_MAX_MCAST_GROUPS; i++) {
                if (s->mcast_groups[i] == group) return 0;
                if (slot < 0 && s->mcast_groups[i] == 0) slot = i;
            }
            if (slot < 0) return -ENOBUFS;
            s->mcast_groups[slot] = group;
            net_device_t* dev = net_get_default_device();
            return igmp_join(dev, group);
        }
        case IP_DROP_MEMBERSHIP: {
            if (optlen < sizeof(struct ip_mreq)) return -EINVAL;
            const struct ip_mreq* mr = (const struct ip_mreq*)optval;
            uint32_t group = net_ntohl(mr->imr_multiaddr.s_addr);
            for (int i = 0; i < NET_MAX_MCAST_GROUPS; i++) {
                if (s->mcast_groups[i] == group) {
                    s->mcast_groups[i] = 0;
                    net_device_t* dev = net_get_default_device();
                    return igmp_leave(dev, group);
                }
            }
            return -EADDRNOTAVAIL;
        }
        default:
            return -ENOPROTOOPT;
        }
    }

    if (level == SOL_TCP || level == IPPROTO_TCP) {
        if (s->type != SOCK_STREAM) return -ENOPROTOOPT;
        switch (optname) {
        case TCP_NODELAY:
            if (s->tcp) s->tcp->nodelay = ival ? 1 : 0;
            return 0;
        case TCP_KEEPIDLE:
            if (s->tcp && ival > 0) s->tcp->keepidle_ticks = (uint32_t)ival * 100;
            return 0;
        case TCP_KEEPINTVL:
            if (s->tcp && ival > 0) s->tcp->keepintvl_ticks = (uint32_t)ival * 100;
            return 0;
        case TCP_KEEPCNT:
            if (s->tcp && ival > 0) s->tcp->keepcnt = (uint8_t)ival;
            return 0;
        case TCP_MAXSEG:
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }

    return -ENOPROTOOPT;
}

int sock_getsockopt(int sockfd, int level, int optname,
                    void* optval, socklen_t* optlen) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_ERROR:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = s->error;
                s->error = 0;
                *optlen = sizeof(int);
            }
            return 0;
        case SO_TYPE:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = s->type;
                *optlen = sizeof(int);
            }
            return 0;
        case SO_REUSEADDR:
        case SO_REUSEPORT:
        case SO_BROADCAST:
        case SO_OOBINLINE: {
            int v = (optname == SO_REUSEADDR) ? s->reuse_addr :
                    (optname == SO_REUSEPORT) ? s->reuse_port :
                    (optname == SO_BROADCAST) ? s->broadcast : s->oobinline;
            if (*optlen >= sizeof(int)) {
                *(int*)optval = v;
                *optlen = sizeof(int);
            }
            return 0;
        }
        case SO_KEEPALIVE:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = (s->tcp && s->tcp->keepalive) ? 1 : 0;
                *optlen = sizeof(int);
            }
            return 0;
        case SO_SNDBUF:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = (int)s->sndbuf_size;
                *optlen = sizeof(int);
            }
            return 0;
        case SO_RCVBUF:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = (int)s->rcvbuf_size;
                *optlen = sizeof(int);
            }
            return 0;
        case SO_LINGER:
            if (*optlen >= sizeof(struct linger)) {
                struct linger* l = (struct linger*)optval;
                l->l_onoff = s->linger_onoff;
                l->l_linger = s->linger_seconds;
                *optlen = sizeof(struct linger);
            }
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }

    if (level == SOL_IP || level == IPPROTO_IP) {
        switch (optname) {
        case IP_TTL:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = s->ip_ttl;
                *optlen = sizeof(int);
            }
            return 0;
        case IP_TOS:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = s->ip_tos;
                *optlen = sizeof(int);
            }
            return 0;
        case IP_HDRINCL:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = s->ip_hdrincl;
                *optlen = sizeof(int);
            }
            return 0;
        case IP_MULTICAST_TTL:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = s->mcast_ttl;
                *optlen = sizeof(int);
            }
            return 0;
        case IP_MULTICAST_LOOP:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = s->mcast_loop;
                *optlen = sizeof(int);
            }
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }

    if (level == SOL_TCP || level == IPPROTO_TCP) {
        if (s->type != SOCK_STREAM) return -ENOPROTOOPT;
        switch (optname) {
        case TCP_NODELAY:
            if (*optlen >= sizeof(int)) {
                *(int*)optval = (s->tcp && s->tcp->nodelay) ? 1 : 0;
                *optlen = sizeof(int);
            }
            return 0;
        case TCP_INFO:
            if (!s->tcp) return -ENOTCONN;
            if (*optlen < sizeof(struct tcp_info)) return -EINVAL;
            tcp_fill_info(s->tcp, (struct tcp_info*)optval);
            *optlen = sizeof(struct tcp_info);
            return 0;
        case TCP_KEEPIDLE:
            if (*optlen >= sizeof(int) && s->tcp) {
                *(int*)optval = (int)(s->tcp->keepidle_ticks / 100);
                *optlen = sizeof(int);
            }
            return 0;
        case TCP_KEEPINTVL:
            if (*optlen >= sizeof(int) && s->tcp) {
                *(int*)optval = (int)(s->tcp->keepintvl_ticks / 100);
                *optlen = sizeof(int);
            }
            return 0;
        case TCP_KEEPCNT:
            if (*optlen >= sizeof(int) && s->tcp) {
                *(int*)optval = (int)s->tcp->keepcnt;
                *optlen = sizeof(int);
            }
            return 0;
        default:
            return -ENOPROTOOPT;
        }
    }

    return -ENOPROTOOPT;
}

// ============================================================================
// raw_socket_deliver - hand a copy of an inbound IPv4 packet to every
// SOCK_RAW socket whose protocol matches (or IPPROTO_RAW).  Called by ipv4_rx
// before the normal demux.
// ============================================================================
void raw_socket_deliver(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                        const uint8_t* packet, uint16_t total_len) {
    (void)dst_ip;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t* s = &sockets[i];
        if (!s->active || s->type != SOCK_RAW) continue;
        if (s->protocol != protocol && s->protocol != IPPROTO_RAW) continue;

        uint64_t flags;
        spin_lock_irqsave(&s->lock, &flags);

        int next = (s->udp_rx_tail + 1) % 16;
        if (next == s->udp_rx_head) {
            spin_unlock_irqrestore(&s->lock, flags);
            continue;  // queue full, drop
        }
        uint16_t copy = total_len;
        if (copy > sizeof(s->udp_rx_queue[0].data))
            copy = sizeof(s->udp_rx_queue[0].data);
        for (uint16_t b = 0; b < copy; b++)
            s->udp_rx_queue[s->udp_rx_tail].data[b] = packet[b];
        s->udp_rx_queue[s->udp_rx_tail].len = copy;
        s->udp_rx_queue[s->udp_rx_tail].from.sin_family = AF_INET;
        s->udp_rx_queue[s->udp_rx_tail].from.sin_port = 0;
        s->udp_rx_queue[s->udp_rx_tail].from.sin_addr.s_addr = net_htonl(src_ip);
        s->udp_rx_tail = next;
        s->udp_rx_ready = 1;

        spin_unlock_irqrestore(&s->lock, flags);
    }
}

// ============================================================================
// sock_getpeername / sock_getsockname
// ============================================================================
int sock_getpeername(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;
    if (!s->connected) return -ENOTCONN;

    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        *addr = s->remote_addr;
        *addrlen = sizeof(struct sockaddr_in);
    }
    return 0;
}

int sock_getsockname(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        *addr = s->local_addr;
        *addrlen = sizeof(struct sockaddr_in);
    }
    return 0;
}

// ============================================================================
// sock_get - Get socket structure by index (for external use)
// ============================================================================
net_socket_t* sock_get(int sockfd) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return NULL;
    net_socket_t* s = &sockets[sockfd];
    return s->active ? s : NULL;
}

// ============================================================================
// sock_post_icmp_error — set error on a socket matching the 4-tuple
// ============================================================================
void sock_post_icmp_error(uint8_t proto,
                          uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port,
                          int error) {
    int wanted_type = (proto == IP_PROTO_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t* s = &sockets[i];
        if (!s->active || s->type != wanted_type) continue;
        // local side: src_ip/src_port from the original outbound packet
        if (net_ntohs(s->local_addr.sin_port) != src_port) continue;
        if (s->local_addr.sin_addr.s_addr != 0 &&
            net_ntohl(s->local_addr.sin_addr.s_addr) != src_ip)
            continue;
        if (s->connected) {
            if (net_ntohs(s->remote_addr.sin_port) != dst_port) continue;
            if (net_ntohl(s->remote_addr.sin_addr.s_addr) != dst_ip) continue;
        }
        s->error = error;
        s->udp_rx_ready = 1;     // wake any blocked recv
        if (proto == IP_PROTO_TCP && s->tcp) {
            s->tcp->error = error;
            s->tcp->rx_ready = 1;
            s->tcp->tx_ready = 1;
            s->tcp->connect_done = 1;
        }
    }
}

// ============================================================================
// sock_socketpair - Create a pair of connected sockets
// Only AF_INET + SOCK_DGRAM supported (connected UDP pair on loopback)
// ============================================================================
int sock_socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    if (domain != AF_INET) return -EAFNOSUPPORT;
    int real_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (real_type != SOCK_DGRAM && real_type != SOCK_STREAM)
        return -ESOCKTNOSUPPORT;

    int fd0 = sock_create(domain, real_type, 0);
    if (fd0 < 0) return fd0;
    int fd1 = sock_create(domain, real_type, 0);
    if (fd1 < 0) { sock_close(fd0); return fd1; }

    net_socket_t* s0 = &sockets[fd0];
    net_socket_t* s1 = &sockets[fd1];

    uint16_t port0 = alloc_ephemeral_port();
    uint16_t port1 = alloc_ephemeral_port();

    s0->local_addr.sin_family = AF_INET;
    s0->local_addr.sin_port = net_htons(port0);
    s0->local_addr.sin_addr.s_addr = net_htonl(0x7F000001);
    s0->remote_addr.sin_family = AF_INET;
    s0->remote_addr.sin_port = net_htons(port1);
    s0->remote_addr.sin_addr.s_addr = net_htonl(0x7F000001);
    s0->bound = 1;
    s0->connected = 1;

    s1->local_addr.sin_family = AF_INET;
    s1->local_addr.sin_port = net_htons(port1);
    s1->local_addr.sin_addr.s_addr = net_htonl(0x7F000001);
    s1->remote_addr.sin_family = AF_INET;
    s1->remote_addr.sin_port = net_htons(port0);
    s1->remote_addr.sin_addr.s_addr = net_htonl(0x7F000001);
    s1->bound = 1;
    s1->connected = 1;

    if (type & SOCK_NONBLOCK) {
        s0->nonblock = 1;
        s1->nonblock = 1;
    }

    sv[0] = fd0;
    sv[1] = fd1;
    return 0;
}

// ============================================================================
// sock_accept4 - Accept with flags (SOCK_NONBLOCK, SOCK_CLOEXEC)
// ============================================================================
int sock_accept4(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen, int flags) {
    int newfd = sock_accept(sockfd, addr, addrlen);
    if (newfd < 0) return newfd;

    if (flags & SOCK_NONBLOCK) {
        net_socket_t* ns = &sockets[newfd];
        ns->nonblock = 1;
    }
    // SOCK_CLOEXEC is noted but not enforced (no exec in this OS context)
    return newfd;
}

// ============================================================================
// sock_sendmsg - Send message with scatter/gather
// ============================================================================
int sock_sendmsg(int sockfd, const struct msghdr* msg, int flags) {
    if (!msg) return -EINVAL;

    // Calculate total length from iovec
    size_t total = 0;
    smap_disable();
    for (int i = 0; i < msg->msg_iovlen; i++)
        total += msg->msg_iov[i].iov_len;

    if (total == 0) return 0;

    // Copy scatter buffers into a single contiguous buffer
    uint8_t buf[2048];
    if (total > sizeof(buf)) total = sizeof(buf);

    size_t off = 0;
    for (int i = 0; i < msg->msg_iovlen && off < total; i++) {
        size_t chunk = msg->msg_iov[i].iov_len;
        if (off + chunk > total) chunk = total - off;
        for (size_t j = 0; j < chunk; j++)
            buf[off + j] = ((const uint8_t*)msg->msg_iov[i].iov_base)[j];
        off += chunk;
    }

    // Use sendto with optional address
    const struct sockaddr_in* dest = NULL;
    struct sockaddr_in dest_copy;
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
        dest_copy = *(const struct sockaddr_in*)msg->msg_name;
        dest = &dest_copy;
    }
    smap_enable();

    return sock_sendto(sockfd, buf, total, flags, dest,
                       dest ? sizeof(struct sockaddr_in) : 0);
}

// ============================================================================
// sock_recvmsg - Receive message with scatter/gather
// ============================================================================
int sock_recvmsg(int sockfd, struct msghdr* msg, int flags) {
    if (!msg) return -EINVAL;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    // Calculate total iovec capacity
    size_t total = 0;
    smap_disable();
    for (int i = 0; i < msg->msg_iovlen; i++)
        total += msg->msg_iov[i].iov_len;
    smap_enable();

    if (total == 0) return 0;

    uint8_t buf[2048];
    if (total > sizeof(buf)) total = sizeof(buf);

    struct sockaddr_in src_addr;
    socklen_t addrlen = sizeof(src_addr);

    // For DGRAM/RAW, capture per-packet metadata BEFORE recvfrom dequeues
    uint32_t pkt_dst_ip = 0;
    uint8_t  pkt_ttl = 0, pkt_tos = 0;
    uint32_t pkt_ifindex = 0;
    int      have_meta = 0;
    if ((s->type == SOCK_DGRAM || s->type == SOCK_RAW) &&
        s->udp_rx_head != s->udp_rx_tail) {
        int idx = s->udp_rx_head;
        pkt_dst_ip = s->udp_rx_queue[idx].dst_ip;
        pkt_ttl    = s->udp_rx_queue[idx].ttl;
        pkt_tos    = s->udp_rx_queue[idx].tos;
        pkt_ifindex = s->udp_rx_queue[idx].ifindex;
        have_meta = 1;
    }

    int ret = sock_recvfrom(sockfd, buf, total, flags, &src_addr, &addrlen);
    if (ret < 0) return ret;

    // Scatter into iovecs
    smap_disable();
    size_t off = 0;
    for (int i = 0; i < msg->msg_iovlen && off < (size_t)ret; i++) {
        size_t chunk = msg->msg_iov[i].iov_len;
        if (off + chunk > (size_t)ret) chunk = (size_t)ret - off;
        for (size_t j = 0; j < chunk; j++)
            ((uint8_t*)msg->msg_iov[i].iov_base)[j] = buf[off + j];
        off += chunk;
    }

    // Return source address if requested
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
        *(struct sockaddr_in*)msg->msg_name = src_addr;
        msg->msg_namelen = sizeof(struct sockaddr_in);
    }

    // RFC 2292 ancillary data
    size_t cmsg_avail = msg->msg_controllen;
    size_t cmsg_used  = 0;
    msg->msg_flags = 0;
    if (have_meta && msg->msg_control && cmsg_avail > 0) {
        unsigned char* cp = (unsigned char*)msg->msg_control;
        if (s->ip_recvpktinfo &&
            cmsg_used + CMSG_SPACE(sizeof(struct in_pktinfo)) <= cmsg_avail) {
            struct cmsghdr* c = (struct cmsghdr*)(cp + cmsg_used);
            c->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
            c->cmsg_level = IPPROTO_IP;
            c->cmsg_type  = IP_PKTINFO;
            struct in_pktinfo pi = {0};
            pi.ipi_ifindex = (int)pkt_ifindex;
            pi.ipi_addr.s_addr = net_htonl(pkt_dst_ip);
            pi.ipi_spec_dst.s_addr = net_htonl(pkt_dst_ip);
            for (size_t k = 0; k < sizeof(pi); k++)
                CMSG_DATA(c)[k] = ((unsigned char*)&pi)[k];
            cmsg_used += CMSG_SPACE(sizeof(struct in_pktinfo));
        }
        if (s->ip_recvttl &&
            cmsg_used + CMSG_SPACE(sizeof(int)) <= cmsg_avail) {
            struct cmsghdr* c = (struct cmsghdr*)(cp + cmsg_used);
            c->cmsg_len = CMSG_LEN(sizeof(int));
            c->cmsg_level = IPPROTO_IP;
            c->cmsg_type  = IP_RECVTTL;
            int v = pkt_ttl;
            for (size_t k = 0; k < sizeof(v); k++)
                CMSG_DATA(c)[k] = ((unsigned char*)&v)[k];
            cmsg_used += CMSG_SPACE(sizeof(int));
        }
        if (s->ip_recvtos &&
            cmsg_used + CMSG_SPACE(sizeof(int)) <= cmsg_avail) {
            struct cmsghdr* c = (struct cmsghdr*)(cp + cmsg_used);
            c->cmsg_len = CMSG_LEN(sizeof(int));
            c->cmsg_level = IPPROTO_IP;
            c->cmsg_type  = IP_RECVTOS;
            int v = pkt_tos;
            for (size_t k = 0; k < sizeof(v); k++)
                CMSG_DATA(c)[k] = ((unsigned char*)&v)[k];
            cmsg_used += CMSG_SPACE(sizeof(int));
        }
    }
    msg->msg_controllen = cmsg_used;
    smap_enable();

    return ret;
}

// ============================================================================
// sock_poll - Check socket for events
// Returns bitmask of POLLIN/POLLOUT/POLLERR/POLLHUP
// ============================================================================
int sock_poll(int sockfd, short events) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return POLLNVAL;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return POLLNVAL;

    short revents = 0;

    if (s->type == SOCK_DGRAM) {
        if ((events & (POLLIN | POLLRDNORM)) && s->udp_rx_ready)
            revents |= POLLIN | POLLRDNORM;
        if (events & (POLLOUT | POLLWRNORM))
            revents |= POLLOUT | POLLWRNORM;  // UDP is always writable
    } else if (s->type == SOCK_STREAM) {
        if (s->listening && s->tcp) {
            // Listening socket: readable when accept queue is non-empty
            if ((events & (POLLIN | POLLRDNORM)) && s->tcp->accept_ready)
                revents |= POLLIN | POLLRDNORM;
        } else if (s->tcp) {
            tcp_conn_t* conn = s->tcp;
            // Readable if data available or connection closed
            if (events & (POLLIN | POLLRDNORM)) {
                if (conn->rx_ready || conn->rx_head != conn->rx_tail)
                    revents |= POLLIN | POLLRDNORM;
                if (conn->state == TCP_STATE_CLOSE_WAIT ||
                    conn->state == TCP_STATE_CLOSED)
                    revents |= POLLIN | POLLRDNORM;  // EOF
            }
            // Writable if connection established and space in tx buffer
            if (events & (POLLOUT | POLLWRNORM)) {
                if (conn->state == TCP_STATE_ESTABLISHED && conn->tx_ready)
                    revents |= POLLOUT | POLLWRNORM;
            }
            // Error / hangup
            if (conn->error)
                revents |= POLLERR;
            if (conn->state == TCP_STATE_CLOSED ||
                conn->state == TCP_STATE_CLOSE_WAIT ||
                conn->state == TCP_STATE_TIME_WAIT)
                revents |= POLLHUP;
        } else if (!s->connected && !s->listening) {
            // Unconnected TCP socket is writable (can connect)
            if (events & (POLLOUT | POLLWRNORM))
                revents |= POLLOUT | POLLWRNORM;
        }
    }

    if (s->error)
        revents |= POLLERR;

    return revents;
}

// ============================================================================
// sock_ioctl_net - Handle network ioctls on a socket fd
// ============================================================================
int sock_ioctl_net(int sockfd, unsigned long request, void* argp) {
    (void)sockfd;  // Most net ioctls don't need the socket itself
    return net_ioctl(request, argp);
}

// ============================================================================
// net_find_dev_by_name - Find a device by interface name (includes loopback)
// ============================================================================
static net_device_t* net_find_dev_by_name(const char* name) {
    // Check loopback first
    net_device_t* lo = net_get_loopback();
    if (lo) {
        const char* n = lo->name;
        int match = 1;
        for (int j = 0; j < IFNAMSIZ; j++) {
            if (name[j] != n[j]) { match = 0; break; }
            if (!n[j]) break;
        }
        if (match) return lo;
    }
    // Check registered devices
    for (int i = 0; i < net_device_count(); i++) {
        net_device_t* d = net_get_device(i);
        if (!d) continue;
        const char* n = d->name;
        int match = 1;
        for (int j = 0; j < IFNAMSIZ; j++) {
            if (name[j] != n[j]) { match = 0; break; }
            if (!n[j]) break;
        }
        if (match) return d;
    }
    return NULL;
}

// ============================================================================
// net_dev_is_loopback - Check if device is the loopback interface
// ============================================================================
static int net_dev_is_loopback(net_device_t* dev) {
    return dev == net_get_loopback();
}

// ============================================================================
// net_ioctl - Handle interface-level network ioctls
// ============================================================================
int net_ioctl(unsigned long request, void* argp) {
    if (!argp) return -EFAULT;

    switch (request) {
    case SIOCGIFCONF: {
        struct ifconf* ifc = (struct ifconf*)argp;
        int count = net_device_count();
        int total = count + (net_get_loopback() ? 1 : 0);
        int needed = total * (int)sizeof(struct ifreq);
        if (ifc->ifc_len < needed || !ifc->ifc_buf) {
            ifc->ifc_len = needed;
            return 0;
        }
        int idx = 0;
        // Add loopback first
        net_device_t* lo = net_get_loopback();
        if (lo) {
            struct ifreq* ifr = &ifc->ifc_req[idx];
            for (int j = 0; j < IFNAMSIZ; j++) ifr->ifr_name[j] = 0;
            const char* n = lo->name;
            for (int j = 0; n[j] && j < IFNAMSIZ - 1; j++)
                ifr->ifr_name[j] = n[j];
            struct sockaddr_in* sin = (struct sockaddr_in*)&ifr->ifr_addr;
            sin->sin_family = AF_INET;
            sin->sin_addr.s_addr = net_htonl(lo->ip_addr);
            sin->sin_port = 0;
            idx++;
        }
        for (int i = 0; i < count; i++) {
            net_device_t* dev = net_get_device(i);
            if (!dev) continue;
            struct ifreq* ifr = &ifc->ifc_req[idx];
            for (int j = 0; j < IFNAMSIZ; j++) ifr->ifr_name[j] = 0;
            const char* n = dev->name;
            for (int j = 0; n[j] && j < IFNAMSIZ - 1; j++)
                ifr->ifr_name[j] = n[j];
            struct sockaddr_in* sin = (struct sockaddr_in*)&ifr->ifr_addr;
            sin->sin_family = AF_INET;
            sin->sin_addr.s_addr = net_htonl(dev->ip_addr);
            sin->sin_port = 0;
            idx++;
        }
        ifc->ifc_len = idx * (int)sizeof(struct ifreq);
        return 0;
    }

    case SIOCGIFFLAGS: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        if (net_dev_is_loopback(dev))
            ifr->ifr_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
        else
            ifr->ifr_flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
        return 0;
    }

    case SIOCSIFFLAGS:
        return 0;  // Accept but ignore flag changes

    case SIOCGIFADDR: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        struct sockaddr_in* sin = (struct sockaddr_in*)&ifr->ifr_addr;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = net_htonl(dev->ip_addr);
        sin->sin_port = 0;
        return 0;
    }

    case SIOCSIFADDR: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        struct sockaddr_in* sin = (struct sockaddr_in*)&ifr->ifr_addr;
        dev->ip_addr = net_ntohl(sin->sin_addr.s_addr);
        return 0;
    }

    case SIOCGIFNETMASK: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        struct sockaddr_in* sin = (struct sockaddr_in*)&ifr->ifr_netmask;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = net_htonl(dev->netmask);
        sin->sin_port = 0;
        return 0;
    }

    case SIOCSIFNETMASK: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        struct sockaddr_in* sin = (struct sockaddr_in*)&ifr->ifr_netmask;
        dev->netmask = net_ntohl(sin->sin_addr.s_addr);
        return 0;
    }

    case SIOCGIFBRDADDR: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        struct sockaddr_in* sin = (struct sockaddr_in*)&ifr->ifr_broadaddr;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = net_htonl(dev->ip_addr | ~dev->netmask);
        sin->sin_port = 0;
        return 0;
    }

    case SIOCSIFBRDADDR:
        return 0;  // Accept but ignore

    case SIOCGIFMTU: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        ifr->ifr_mtu = dev->mtu;
        return 0;
    }

    case SIOCSIFMTU: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        if (ifr->ifr_mtu < 68 || ifr->ifr_mtu > 9000) return -EINVAL;
        dev->mtu = (uint16_t)ifr->ifr_mtu;
        return 0;
    }

    case SIOCGIFHWADDR: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        ifr->ifr_hwaddr.sa_family = 1;  // ARPHRD_ETHER
        for (int j = 0; j < ETH_ALEN; j++)
            ifr->ifr_hwaddr.sa_data[j] = (char)dev->mac_addr[j];
        return 0;
    }

    case SIOCGIFINDEX: {
        struct ifreq* ifr = (struct ifreq*)argp;
        net_device_t* dev = net_find_dev_by_name(ifr->ifr_name);
        if (!dev) return -ENOENT;
        if (net_dev_is_loopback(dev)) {
            ifr->ifr_ifindex = 1;  // lo is always index 1
        } else {
            for (int i = 0; i < net_device_count(); i++) {
                if (net_get_device(i) == dev) { ifr->ifr_ifindex = i + 2; break; }
            }
        }
        return 0;
    }

    case SIOCGIFNAME: {
        struct ifreq* ifr = (struct ifreq*)argp;
        int idx = ifr->ifr_ifindex - 1;
        net_device_t* dev = net_get_device(idx);
        if (!dev) return -ENOENT;
        for (int j = 0; j < IFNAMSIZ; j++) ifr->ifr_name[j] = 0;
        const char* n = dev->name;
        for (int j = 0; n[j] && j < IFNAMSIZ - 1; j++)
            ifr->ifr_name[j] = n[j];
        return 0;
    }

    case SIOCGIFMETRIC: {
        struct ifreq* ifr = (struct ifreq*)argp;
        ifr->ifr_metric = 0;
        return 0;
    }

    case SIOCSIFMETRIC:
    case SIOCSIFHWADDR:
    case SIOCGIFDSTADDR:
    case SIOCSIFDSTADDR:
    case SIOCGIFMEM:
    case SIOCSIFMEM:
    case SIOCSIFNAME:
    case SIOCGIFENCAP:
    case SIOCSIFENCAP:
    case SIOCGIFSLAVE:
    case SIOCSIFSLAVE:
    case SIOCADDMULTI:
    case SIOCDELMULTI:
    case SIOCSIFPFLAGS:
    case SIOCGIFPFLAGS:
    case SIOCDIFADDR:
    case SIOCSIFHWBROADCAST:
    case SIOCSIFLINK:
        return -EOPNOTSUPP;

    case SIOCGIFCOUNT: {
        int* countp = (int*)argp;
        *countp = net_device_count() + (net_get_loopback() ? 1 : 0);
        return 0;
    }

    // ARP ioctls
    case SIOCGARP: {
        struct arpreq* req = (struct arpreq*)argp;
        struct sockaddr_in* sin = (struct sockaddr_in*)&req->arp_pa;
        uint32_t ip = net_ntohl(sin->sin_addr.s_addr);
        uint8_t mac[ETH_ALEN];
        net_device_t* dev = net_get_default_device();
        if (!dev) return -ENETDOWN;
        if (arp_resolve(dev, ip, mac) == 0) {
            for (int j = 0; j < ETH_ALEN; j++)
                req->arp_ha.sa_data[j] = (char)mac[j];
            req->arp_ha.sa_family = 1;  // ARPHRD_ETHER
            req->arp_flags = ATF_COM;
            return 0;
        }
        return -ENOENT;
    }

    case SIOCSARP: {
        struct arpreq* req = (struct arpreq*)argp;
        struct sockaddr_in* sin = (struct sockaddr_in*)&req->arp_pa;
        uint32_t ip = net_ntohl(sin->sin_addr.s_addr);
        uint8_t mac[ETH_ALEN];
        for (int j = 0; j < ETH_ALEN; j++)
            mac[j] = (uint8_t)req->arp_ha.sa_data[j];
        arp_add_entry(ip, mac);
        return 0;
    }

    case SIOCDARP:
        return 0;  // Accept but no-op (ARP entries expire naturally)

    // Routing ioctls
    case SIOCADDRT: {
        if (!argp) return -EFAULT;
        smap_disable();
        struct rtentry* rt = (struct rtentry*)argp;
        struct sockaddr_in* dst = (struct sockaddr_in*)&rt->rt_dst;
        struct sockaddr_in* gw = (struct sockaddr_in*)&rt->rt_gateway;
        struct sockaddr_in* mask = (struct sockaddr_in*)&rt->rt_genmask;
        uint32_t dst_ip = net_ntohl(dst->sin_addr.s_addr);
        uint32_t gw_ip = net_ntohl(gw->sin_addr.s_addr);
        uint32_t netmask = net_ntohl(mask->sin_addr.s_addr);
        uint16_t flags = (uint16_t)rt->rt_flags;
        smap_enable();

        net_device_t* rdev = net_get_default_device();
        return route_add(dst_ip, netmask, gw_ip, rdev, 0, flags);
    }
    case SIOCDELRT: {
        if (!argp) return -EFAULT;
        smap_disable();
        struct rtentry* rt = (struct rtentry*)argp;
        struct sockaddr_in* dst = (struct sockaddr_in*)&rt->rt_dst;
        struct sockaddr_in* gw = (struct sockaddr_in*)&rt->rt_gateway;
        struct sockaddr_in* mask = (struct sockaddr_in*)&rt->rt_genmask;
        uint32_t dst_ip = net_ntohl(dst->sin_addr.s_addr);
        uint32_t gw_ip = net_ntohl(gw->sin_addr.s_addr);
        uint32_t netmask = net_ntohl(mask->sin_addr.s_addr);
        (void)netmask;
        smap_enable();
        return route_del(dst_ip, netmask, gw_ip);
    }
    case SIOCRTMSG:
        return -EOPNOTSUPP;

    case SIOCGSTAMP:
    case SIOCGSTAMPNS:
        return -EOPNOTSUPP;

    // Bridge / VLAN - not supported
    case SIOCBRADDBR:
    case SIOCBRDELBR:
    case SIOCBRADDIF:
    case SIOCBRDELIF:
    case SIOCGIFVLAN:
    case SIOCSIFVLAN:
        return -EOPNOTSUPP;

    default:
        return -EINVAL;
    }
}

// ============================================================================
// sock_fcntl_net - Handle fcntl on a socket
// ============================================================================
int sock_fcntl_net(int sockfd, int cmd, unsigned long arg) {
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    switch (cmd) {
    case F_GETFL: {
        int flags = 0;
        if (s->nonblock) flags |= O_NONBLOCK;
        flags |= O_RDWR;
        return flags;
    }
    case F_SETFL:
        s->nonblock = (arg & O_NONBLOCK) ? 1 : 0;
        return 0;
    case F_GETFD:
        return 0;  // No CLOEXEC tracking
    case F_SETFD:
        return 0;  // Accept but ignore
    default:
        return -EINVAL;
    }
}

// ============================================================================
// sock_sendfile - Send file contents to a socket/pipe/file fd
// Reads from in_fd (must be a regular file) and writes to out_fd
// (socket, pipe, or file). If offset is non-NULL, use it as the read
// position without modifying the file's seek position.
// Returns number of bytes transferred, or negative errno.
// ============================================================================
int sock_sendfile(int out_fd, int in_fd, int64_t* offset, size_t count) {
    task_t* cur = sched_current();
    if (!cur) return -EFAULT;
    if (count == 0) return 0;

    // ---- Resolve in_fd: must be a regular VFS file (seekable) ----
    if (in_fd < 0 || in_fd >= TASK_MAX_FDS) return -EBADF;
    struct vfs_file* in_file = cur->fd_table[in_fd];
    if (!in_file) return -EBADF;
    uintptr_t in_marker = (uintptr_t)in_file;
    // Reject sockets, pipes, console markers as input
    if (IS_SOCKET_FD(in_file) || IS_EPOLL_FD(in_file))
        return -EINVAL;
    if (in_marker <= 3)  // console markers
        return -EINVAL;
    if (pipe_is_end(in_file))
        return -EINVAL;

    // ---- Resolve out_fd ----
    if (out_fd < 0 || out_fd >= TASK_MAX_FDS) return -EBADF;
    struct vfs_file* out_entry = cur->fd_table[out_fd];
    if (!out_entry && (out_fd == 1 || out_fd == 2)) {
        // stdout/stderr without explicit fd_table entry - treat as console
    } else if (!out_entry) {
        return -EBADF;
    }

    // Determine output type
    int out_is_socket = 0;
    int out_sock_idx = -1;
    int out_is_pipe = 0;
    int out_is_console = 0;
    uintptr_t out_marker = out_entry ? (uintptr_t)out_entry : 0;

    if (out_entry && IS_SOCKET_FD(out_entry)) {
        out_is_socket = 1;
        out_sock_idx = SOCKET_FD_IDX(out_entry);
    } else if (out_entry && pipe_is_end(out_entry)) {
        out_is_pipe = 1;
    } else if (out_marker == 2 || out_marker == 3 ||
               (!out_entry && (out_fd == 1 || out_fd == 2))) {
        out_is_console = 1;
    } else if (out_entry && out_marker > 3 && !IS_EPOLL_FD(out_entry)) {
        // Regular file - handled by the else branch in the write loop
    } else {
        return -EBADF;
    }

    // ---- Handle offset: if non-NULL, save and restore file position ----
    int64_t saved_pos = -1;
    if (offset) {
        // Save current file position
        saved_pos = vfs_seek(in_file, 0, 1);  // SEEK_CUR
        if (saved_pos < 0) return -ESPIPE;
        // Seek to the requested offset
        int64_t sret = vfs_seek(in_file, *offset, 0);  // SEEK_SET
        if (sret < 0) return (int)sret;
    }

    // ---- Transfer loop ----
    // Heap-allocated bounce buffer (kernel stack is only 8KB).
    #define SENDFILE_BUF_SIZE 4096
    uint8_t* sendfile_buf = (uint8_t*)kalloc(SENDFILE_BUF_SIZE);
    if (!sendfile_buf) {
        if (offset) vfs_seek(in_file, saved_pos, 0);
        return -ENOMEM;
    }
    size_t total = 0;
    int err = 0;
    // Yield the CPU every 64 KB to stay cooperative
    size_t since_yield = 0;
    #define SENDFILE_YIELD_BYTES (64 * 1024)

    while (total < count) {
        // Check for pending signals so the transfer is interruptible
        if (cur && signal_pending(cur)) {
            err = -EINTR;
            break;
        }

        size_t chunk = count - total;
        if (chunk > SENDFILE_BUF_SIZE)
            chunk = SENDFILE_BUF_SIZE;

        // Read from input file
        long nread = vfs_read(in_file, sendfile_buf, (long)chunk);
        if (nread <= 0) {
            if (nread < 0) err = (int)nread;
            break;  // EOF or error
        }

        // Write to output
        size_t written = 0;
        while (written < (size_t)nread) {
            long nw;
            if (out_is_socket) {
                nw = sock_send(out_sock_idx,
                               sendfile_buf + written,
                               (size_t)nread - written, 0);
            } else if (out_is_pipe) {
                // Direct pipe buffer write (kernel-to-kernel)
                pipe_end_t* pe = (pipe_end_t*)out_entry;
                pipe_t* pp = pe->pipe;
                if (!pp || pe->is_read) { err = -EBADF; break; }
                uint64_t pflags;
                spin_lock_irqsave(&pp->lock, &pflags);
                if (pp->readers == 0) {
                    spin_unlock_irqrestore(&pp->lock, pflags);
                    err = -EPIPE;
                    break;
                }
                size_t space = pp->size - pp->used;
                size_t todo = (size_t)nread - written;
                if (todo > space) todo = space;
                if (todo == 0) {
                    // Pipe full — wake readers and yield, then retry
                    spin_unlock_irqrestore(&pp->lock, pflags);
                    sched_wake_channel(pp);
                    sched_schedule();
                    continue;
                }
                size_t first = pp->size - pp->write_pos;
                if (first > todo) first = todo;
                mm_memcpy(pp->buffer + pp->write_pos,
                          sendfile_buf + written, first);
                if (todo > first)
                    mm_memcpy(pp->buffer,
                              sendfile_buf + written + first,
                              todo - first);
                pp->write_pos = (pp->write_pos + todo) % pp->size;
                pp->used += todo;
                spin_unlock_irqrestore(&pp->lock, pflags);
                sched_wake_channel(pp);
                nw = (long)todo;
            } else if (out_is_console) {
                tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
                nw = tty_write(tty, sendfile_buf + written,
                               (long)((size_t)nread - written));
            } else {
                // Regular file output
                nw = vfs_write(out_entry, sendfile_buf + written,
                               (long)((size_t)nread - written));
            }
            if (nw <= 0) {
                if (nw < 0) err = (int)nw;
                break;
            }
            written += (size_t)nw;
        }
        total += written;
        if (err != 0 || written < (size_t)nread)
            break;

        // Yield periodically so we don't starve other tasks
        since_yield += written;
        if (since_yield >= SENDFILE_YIELD_BYTES) {
            since_yield = 0;
            sched_schedule();
        }
    }

    #undef SENDFILE_YIELD_BYTES
    #undef SENDFILE_BUF_SIZE

    kfree(sendfile_buf);

    // ---- Restore / update offset ----
    if (offset) {
        // Update offset to reflect bytes read
        *offset += (int64_t)total;
        // Restore original file position
        vfs_seek(in_file, saved_pos, 0);  // SEEK_SET
    }

    return total > 0 ? (int)total : err;
}

// ============================================================================
// Network info queries for userspace commands (netstat, ifconfig, etc.)
// ============================================================================

// Get TCP connection info
int net_get_tcp_connections(net_tcp_info_t* entries, int max_entries) {
    extern tcp_conn_t tcp_connections[TCP_MAX_CONNECTIONS];
    int count = 0;
    for (int i = 0; i < TCP_MAX_CONNECTIONS && count < max_entries; i++) {
        tcp_conn_t* c = &tcp_connections[i];
        if (!c->active) continue;
        entries[count].local_ip = c->local_ip;
        entries[count].local_port = c->local_port;
        entries[count].remote_ip = c->remote_ip;
        entries[count].remote_port = c->remote_port;
        entries[count].state = c->state;
        // Calculate queue sizes
        uint32_t rx_used = 0;
        if (c->rx_buf) {
            if (c->rx_tail >= c->rx_head)
                rx_used = c->rx_tail - c->rx_head;
            else
                rx_used = c->rx_buf_size - c->rx_head + c->rx_tail;
        }
        entries[count].rx_queue = rx_used;
        uint32_t tx_used = 0;
        for (uint8_t seg = 0; seg < c->inflight_count; seg++) {
            tx_used += c->inflight[seg].len;
            if (c->inflight[seg].flags & (TCP_SYN | TCP_FIN))
                tx_used++;
        }
        entries[count].tx_queue = tx_used;
        count++;
    }
    return count;
}

// Get UDP socket info
int net_get_udp_sockets(net_udp_info_t* entries, int max_entries) {
    int count = 0;
    for (int i = 0; i < NET_MAX_SOCKETS && count < max_entries; i++) {
        net_socket_t* s = &sockets[i];
        if (!s->active || s->type != SOCK_DGRAM) continue;
        entries[count].local_ip = net_ntohl(s->local_addr.sin_addr.s_addr);
        entries[count].local_port = net_ntohs(s->local_addr.sin_port);
        entries[count].remote_ip = net_ntohl(s->remote_addr.sin_addr.s_addr);
        entries[count].remote_port = net_ntohs(s->remote_addr.sin_port);
        int rx_count = s->udp_rx_tail - s->udp_rx_head;
        if (rx_count < 0) rx_count += 16;
        entries[count].rx_queue = (uint32_t)rx_count;
        count++;
    }
    return count;
}

// Get interface info
int net_get_iface_info(net_iface_info_t* entries, int max_entries) {
    int count = 0;
    // Loopback first
    net_device_t* lo = net_get_loopback();
    if (lo && count < max_entries) {
        const char* n = lo->name;
        int j = 0;
        while (n[j] && j < 15) { entries[count].name[j] = n[j]; j++; }
        entries[count].name[j] = '\0';
        for (int m = 0; m < 6; m++) entries[count].mac[m] = lo->mac_addr[m];
        entries[count].mtu = lo->mtu;
        entries[count].ip_addr = lo->ip_addr;
        entries[count].netmask = lo->netmask;
        entries[count].gateway = lo->gateway;
        entries[count].dns_server = lo->dns_server;
        entries[count].flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
        entries[count].pad = 0;
        entries[count].rx_packets = lo->rx_packets;
        entries[count].tx_packets = lo->tx_packets;
        entries[count].rx_bytes = lo->rx_bytes;
        entries[count].tx_bytes = lo->tx_bytes;
        entries[count].rx_errors = lo->rx_errors;
        entries[count].tx_errors = lo->tx_errors;
        entries[count].rx_dropped = lo->rx_dropped;
        count++;
    }
    // Hardware devices
    for (int i = 0; i < net_device_count() && count < max_entries; i++) {
        net_device_t* dev = net_get_device(i);
        if (!dev) continue;
        const char* n = dev->name;
        int j = 0;
        while (n[j] && j < 15) { entries[count].name[j] = n[j]; j++; }
        entries[count].name[j] = '\0';
        for (int m = 0; m < 6; m++) entries[count].mac[m] = dev->mac_addr[m];
        entries[count].mtu = dev->mtu;
        entries[count].ip_addr = dev->ip_addr;
        entries[count].netmask = dev->netmask;
        entries[count].gateway = dev->gateway;
        entries[count].dns_server = dev->dns_server;
        entries[count].flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
        entries[count].pad = 0;
        entries[count].rx_packets = dev->rx_packets;
        entries[count].tx_packets = dev->tx_packets;
        entries[count].rx_bytes = dev->rx_bytes;
        entries[count].tx_bytes = dev->tx_bytes;
        entries[count].rx_errors = dev->rx_errors;
        entries[count].tx_errors = dev->tx_errors;
        entries[count].rx_dropped = dev->rx_dropped;
        count++;
    }
    return count;
}
