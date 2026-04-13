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

// Socket table
static net_socket_t sockets[NET_MAX_SOCKETS];
static spinlock_t socket_lock = SPINLOCK_INIT("socket");

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
    if (next_ephemeral_port > 65535)
        next_ephemeral_port = 49152;
    return port;
}

// Find a UDP socket bound to a given port (called from udp.c)
net_socket_t* sock_find_udp(uint16_t port) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t* s = &sockets[i];
        if (s->active && s->type == SOCK_DGRAM && s->bound &&
            net_ntohs(s->local_addr.sin_port) == port) {
            return s;
        }
    }
    return NULL;
}

// ============================================================================
// sock_create - Create a new socket
// Returns socket descriptor (index into socket table) or negative errno
// ============================================================================
int sock_create(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -ESOCKTNOSUPPORT;

    uint64_t flags;
    spin_lock_irqsave(&socket_lock, &flags);

    int fd = -ENOMEM;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            net_socket_t* s = &sockets[i];
            s->type = type;
            s->protocol = (type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
            s->domain = domain;
            s->bound = 0;
            s->listening = 0;
            s->connected = 0;
            s->closed = 0;
            s->nonblock = 0;
            s->error = 0;
            s->tcp = NULL;
            s->udp_rx_head = 0;
            s->udp_rx_tail = 0;
            s->udp_rx_ready = 0;
            s->reuse_addr = 0;
            s->rcv_timeout_ticks = 0;
            s->ref_count = 1;
            s->active = 1;
            s->lock = (spinlock_t)SPINLOCK_INIT("sock");

            // Zero addresses
            for (int j = 0; j < (int)sizeof(struct sockaddr_in); j++) {
                ((uint8_t*)&s->local_addr)[j] = 0;
                ((uint8_t*)&s->remote_addr)[j] = 0;
            }
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
    if (local_ip == 0) local_ip = dev->ip_addr;
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
        __asm__ volatile("pause");
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

    net_device_t* dev = net_get_default_device();
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
    uint32_t dst_ip = net_ntohl(addr->sin_addr.s_addr);
    uint16_t dst_port = net_ntohs(addr->sin_port);
    uint16_t src_port = net_ntohs(s->local_addr.sin_port);

    tcp_conn_t* conn = tcp_connect(dev, dst_ip, src_port, dst_port);
    if (!conn) return -ENOMEM;

    s->tcp = conn;

    // Wait for connection to complete (blocking)
    uint64_t deadline = s->rcv_timeout_ticks ?
        timer_ticks() + s->rcv_timeout_ticks : 0;

    while (!conn->connect_done) {
        if (s->nonblock) return -EINPROGRESS;
        if (deadline && timer_ticks() >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
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

    if (s->type == SOCK_DGRAM) {
        const struct sockaddr_in* target = dest_addr ? dest_addr : &s->remote_addr;
        if (target->sin_port == 0 && !s->connected) return -EDESTADDRREQ;

        uint32_t dst_ip = net_ntohl(target->sin_addr.s_addr);
        uint16_t src_port = net_ntohs(s->local_addr.sin_port);
        uint16_t dst_port = net_ntohs(target->sin_port);

        uint16_t send_len = len > NET_MTU_DEFAULT - 28 ?
                             NET_MTU_DEFAULT - 28 : (uint16_t)len;

        // Loopback shortcut: deliver directly to destination socket
        if ((dst_ip & 0xFF000000) == 0x7F000000) {
            extern void udp_deliver_to_socket(uint32_t src_ip, uint16_t src_port,
                                              uint16_t dst_port,
                                              const uint8_t* data, uint16_t len);
            udp_deliver_to_socket(dst_ip, src_port, dst_port,
                                 (const uint8_t*)buf, send_len);
            return (int)send_len;
        }

        net_device_t* dev = net_get_default_device();
        if (!dev) return -ENETDOWN;

        if (!s->bound) {
            s->local_addr.sin_port = net_htons(alloc_ephemeral_port());
            s->local_addr.sin_addr.s_addr = net_htonl(dev->ip_addr);
            s->bound = 1;
        }

        int ret = udp_send(dev, dst_ip, src_port, dst_port,
                           (const uint8_t*)buf, send_len);
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
    (void)flags;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    if (s->type == SOCK_DGRAM) {
        // Wait for data
        uint64_t deadline = s->rcv_timeout_ticks ?
            timer_ticks() + s->rcv_timeout_ticks : 0;

        while (!s->udp_rx_ready) {
            if (s->nonblock) return -EAGAIN;
            if (deadline && timer_ticks() >= deadline) return -ETIMEDOUT;
            __asm__ volatile("pause");
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

        for (uint16_t i = 0; i < data_len; i++)
            ((uint8_t*)buf)[i] = s->udp_rx_queue[idx].data[i];

        if (src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
            *src_addr = s->udp_rx_queue[idx].from;
            *addrlen = sizeof(struct sockaddr_in);
        }

        s->udp_rx_head = (s->udp_rx_head + 1) % 16;
        if (s->udp_rx_head == s->udp_rx_tail)
            s->udp_rx_ready = 0;

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

        uint16_t send_len = len > TCP_MSS ? TCP_MSS : (uint16_t)len;
        int ret = tcp_send_data(s->tcp, (const uint8_t*)buf, send_len);
        return ret;
    }

    // UDP send (connected)
    if (!s->connected) return -EDESTADDRREQ;
    return sock_sendto(sockfd, buf, len, flags, &s->remote_addr, sizeof(struct sockaddr_in));
}

// ============================================================================
// sock_recv - Receive data on connected socket (TCP)
// ============================================================================
int sock_recv(int sockfd, void* buf, size_t len, int flags) {
    (void)flags;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    if (s->type == SOCK_STREAM) {
        if (!s->tcp) return -ENOTCONN;

        tcp_conn_t* conn = s->tcp;
        uint64_t deadline = s->rcv_timeout_ticks ?
            timer_ticks() + s->rcv_timeout_ticks : 0;

        // Wait for data
        while (!conn->rx_ready && conn->state == TCP_STATE_ESTABLISHED) {
            if (s->nonblock) return -EAGAIN;
            if (deadline && timer_ticks() >= deadline) return -ETIMEDOUT;
            __asm__ volatile("pause");
        }

        if (conn->error) return -conn->error;

        // Connection closed - return 0 (EOF)
        if (conn->state == TCP_STATE_CLOSE_WAIT ||
            conn->state == TCP_STATE_CLOSED) {
            if (conn->rx_head == conn->rx_tail) return 0;
        }

        // Copy from rx buffer
        uint64_t cflags;
        spin_lock_irqsave(&conn->lock, &cflags);

        uint32_t avail = (conn->rx_tail - conn->rx_head + conn->rx_buf_size) % conn->rx_buf_size;
        uint32_t copy = avail;
        if (copy > len) copy = (uint32_t)len;

        for (uint32_t i = 0; i < copy; i++) {
            ((uint8_t*)buf)[i] = conn->rx_buf[(conn->rx_head + i) % conn->rx_buf_size];
        }
        conn->rx_head = (conn->rx_head + copy) % conn->rx_buf_size;

        if (conn->rx_head == conn->rx_tail)
            conn->rx_ready = 0;

        spin_unlock_irqrestore(&conn->lock, cflags);
        return (int)copy;
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

    uint64_t flags;
    spin_lock_irqsave(&s->lock, &flags);

    if (s->tcp) {
        tcp_close(s->tcp);
        s->tcp = NULL;
    }

    s->closed = 1;
    s->active = 0;

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
    }

    return 0;
}

// ============================================================================
// sock_setsockopt / sock_getsockopt
// ============================================================================
int sock_setsockopt(int sockfd, int level, int optname,
                    const void* optval, socklen_t optlen) {
    (void)level;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    switch (optname) {
    case SO_REUSEADDR:
        if (optlen >= sizeof(int))
            s->reuse_addr = *(const int*)optval;
        return 0;
    case SO_RCVTIMEO: {
        // Interpret as milliseconds for simplicity
        if (optlen >= sizeof(uint64_t))
            s->rcv_timeout_ticks = *(const uint64_t*)optval / 10;
        else if (optlen >= sizeof(int))
            s->rcv_timeout_ticks = (uint64_t)(*(const int*)optval) / 10;
        return 0;
    }
    case SO_KEEPALIVE:
        return 0;  // Accepted but ignored
    default:
        return -ENOPROTOOPT;
    }
}

int sock_getsockopt(int sockfd, int level, int optname,
                    void* optval, socklen_t* optlen) {
    (void)level;
    if (sockfd < 0 || sockfd >= NET_MAX_SOCKETS) return -EBADF;
    net_socket_t* s = &sockets[sockfd];
    if (!s->active) return -EBADF;

    switch (optname) {
    case SO_ERROR: {
        if (*optlen >= sizeof(int)) {
            *(int*)optval = s->error;
            s->error = 0;
            *optlen = sizeof(int);
        }
        return 0;
    }
    case SO_REUSEADDR: {
        if (*optlen >= sizeof(int)) {
            *(int*)optval = s->reuse_addr;
            *optlen = sizeof(int);
        }
        return 0;
    }
    default:
        return -ENOPROTOOPT;
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
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in))
        dest = (const struct sockaddr_in*)msg->msg_name;

    return sock_sendto(sockfd, buf, total, flags, dest,
                       dest ? sizeof(struct sockaddr_in) : 0);
}

// ============================================================================
// sock_recvmsg - Receive message with scatter/gather
// ============================================================================
int sock_recvmsg(int sockfd, struct msghdr* msg, int flags) {
    if (!msg) return -EINVAL;

    // Calculate total iovec capacity
    size_t total = 0;
    for (int i = 0; i < msg->msg_iovlen; i++)
        total += msg->msg_iov[i].iov_len;

    if (total == 0) return 0;

    uint8_t buf[2048];
    if (total > sizeof(buf)) total = sizeof(buf);

    struct sockaddr_in src_addr;
    socklen_t addrlen = sizeof(src_addr);
    int ret = sock_recvfrom(sockfd, buf, total, flags, &src_addr, &addrlen);
    if (ret < 0) return ret;

    // Scatter into iovecs
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
    msg->msg_flags = 0;
    msg->msg_controllen = 0;

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
                if (conn->state == TCP_STATE_ESTABLISHED)
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
