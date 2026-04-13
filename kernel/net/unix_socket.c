// LikeOS-64 UNIX Domain Sockets
#include "../../include/kernel/net.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/syscall.h"

// UNIX socket table
static unix_socket_t unix_sockets[MAX_UNIX_SOCKETS];

// ============================================================================
// String helpers
// ============================================================================
static int uds_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void uds_strcpy(char* dst, const char* src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// ============================================================================
// unix_get - Get unix socket by index (from FD)
// ============================================================================
unix_socket_t* unix_get(int usockfd) {
    int idx = UNIX_SOCKET_FD_IDX(usockfd);
    if (idx < 0 || idx >= MAX_UNIX_SOCKETS) return NULL;
    if (!unix_sockets[idx].active) return NULL;
    return &unix_sockets[idx];
}

// ============================================================================
// unix_alloc - Allocate a new unix socket slot
// ============================================================================
static int unix_alloc(void) {
    for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
        if (!unix_sockets[i].active) return i;
    }
    return -1;
}

// ============================================================================
// unix_find_by_path - Find a bound & listening socket by path
// ============================================================================
static unix_socket_t* unix_find_by_path(const char* path) {
    for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
        if (!unix_sockets[i].active) continue;
        if (!unix_sockets[i].bound) continue;
        if (uds_strcmp(unix_sockets[i].path, path) == 0)
            return &unix_sockets[i];
    }
    return NULL;
}

// ============================================================================
// unix_create - Create a new UNIX domain socket
// ============================================================================
int unix_create(int type) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EINVAL;

    int idx = unix_alloc();
    if (idx < 0) return -ENOMEM;

    unix_socket_t* us = &unix_sockets[idx];
    // Zero out the struct
    uint8_t* p = (uint8_t*)us;
    for (int i = 0; i < (int)sizeof(unix_socket_t); i++) p[i] = 0;

    us->active = 1;
    us->type = type;
    us->ref_count = 1;
    spinlock_init(&us->lock, "unix_sock");

    return MAKE_UNIX_SOCKET_FD(idx);
}

// ============================================================================
// unix_bind - Bind a UNIX socket to a path
// ============================================================================
int unix_bind(int usockfd, const struct sockaddr_un* addr) {
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;
    if (us->bound) return -EINVAL;
    if (!addr || addr->sun_family != AF_UNIX) return -EINVAL;

    // Check if path already in use
    if (addr->sun_path[0] && unix_find_by_path(addr->sun_path))
        return -EADDRINUSE;

    uint64_t flags;
    spin_lock_irqsave(&us->lock, &flags);
    uds_strcpy(us->path, addr->sun_path, UNIX_PATH_MAX);
    us->bound = 1;
    spin_unlock_irqrestore(&us->lock, flags);

    return 0;
}

// ============================================================================
// unix_listen - Mark a UNIX socket as listening
// ============================================================================
int unix_listen(int usockfd, int backlog) {
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;
    if (us->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (!us->bound) return -EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&us->lock, &flags);
    us->listening = 1;
    us->backlog = (backlog > 16) ? 16 : (backlog < 1 ? 1 : backlog);
    spin_unlock_irqrestore(&us->lock, flags);

    return 0;
}

// ============================================================================
// unix_accept - Accept a connection on a listening socket
// ============================================================================
int unix_accept(int usockfd, struct sockaddr_un* addr, socklen_t* addrlen) {
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;
    if (!us->listening) return -EINVAL;

    // Wait for incoming connection
    while (us->accept_head == us->accept_tail) {
        if (us->nonblock) return -EAGAIN;
        __asm__ volatile("pause");
        if (!us->active) return -EBADF;
    }

    uint64_t flags;
    spin_lock_irqsave(&us->lock, &flags);
    unix_socket_t* client = us->accept_queue[us->accept_head];
    us->accept_head = (us->accept_head + 1) % 16;
    spin_unlock_irqrestore(&us->lock, flags);

    if (!client) return -EFAULT;

    // Allocate server-side socket
    int new_idx = unix_alloc();
    if (new_idx < 0) {
        client->error = ECONNREFUSED;
        client->connected = 0;
        return -ENOMEM;
    }

    unix_socket_t* server = &unix_sockets[new_idx];
    uint8_t* p = (uint8_t*)server;
    for (int i = 0; i < (int)sizeof(unix_socket_t); i++) p[i] = 0;
    server->active = 1;
    server->type = SOCK_STREAM;
    server->connected = 1;
    server->ref_count = 1;
    server->parent = us;
    spinlock_init(&server->lock, "unix_sock");

    // Link peers
    server->peer = client;
    client->peer = server;
    client->connected = 1;

    // Fill addr if requested
    if (addr && addrlen) {
        addr->sun_family = AF_UNIX;
        uds_strcpy(addr->sun_path, client->path, UNIX_PATH_MAX);
        *addrlen = sizeof(struct sockaddr_un);
    }

    return MAKE_UNIX_SOCKET_FD(new_idx);
}

// ============================================================================
// unix_connect - Connect to a bound/listening UNIX socket
// ============================================================================
int unix_connect(int usockfd, const struct sockaddr_un* addr) {
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;
    if (us->connected) return -EISCONN;
    if (!addr || addr->sun_family != AF_UNIX) return -EINVAL;

    unix_socket_t* listener = unix_find_by_path(addr->sun_path);
    if (!listener || !listener->listening) return -ECONNREFUSED;

    uint64_t flags;
    spin_lock_irqsave(&listener->lock, &flags);

    // Check accept queue capacity
    int next = (listener->accept_tail + 1) % 16;
    if (next == listener->accept_head) {
        spin_unlock_irqrestore(&listener->lock, flags);
        return -ECONNREFUSED;
    }

    // Enqueue ourselves
    listener->accept_queue[listener->accept_tail] = us;
    listener->accept_tail = next;
    listener->accept_ready = 1;
    spin_unlock_irqrestore(&listener->lock, flags);

    // Wait for acceptance (peer link to be set up)
    while (!us->peer && !us->error) {
        if (us->nonblock) return -EINPROGRESS;
        __asm__ volatile("pause");
    }

    if (us->error) {
        int err = us->error;
        us->error = 0;
        return -err;
    }

    return 0;
}

// ============================================================================
// unix_send - Send data on a connected UNIX socket
// ============================================================================
int unix_send(int usockfd, const void* buf, size_t len, int flags) {
    (void)flags;
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;

    unix_socket_t* peer = us->peer;
    if (!peer) {
        if (us->type == SOCK_DGRAM) return -EDESTADDRREQ;
        return -ENOTCONN;
    }
    if (peer->closed || peer->peer_closed) return -EPIPE;

    const uint8_t* src = (const uint8_t*)buf;
    int sent = 0;

    uint64_t irqflags;
    spin_lock_irqsave(&peer->lock, &irqflags);

    for (size_t i = 0; i < len; i++) {
        int next = (peer->tail + 1) % (int)sizeof(peer->buf);
        if (next == peer->head) {
            // Buffer full
            if (sent > 0) break;
            spin_unlock_irqrestore(&peer->lock, irqflags);
            if (us->nonblock) return -EAGAIN;
            // Wait for space
            while ((peer->tail + 1) % (int)sizeof(peer->buf) == peer->head) {
                if (peer->closed) return -EPIPE;
                __asm__ volatile("pause");
            }
            spin_lock_irqsave(&peer->lock, &irqflags);
            next = (peer->tail + 1) % (int)sizeof(peer->buf);
            if (next == peer->head) break;
        }
        peer->buf[peer->tail] = src[i];
        peer->tail = next;
        sent++;
    }
    peer->ready = 1;
    spin_unlock_irqrestore(&peer->lock, irqflags);

    return sent > 0 ? sent : -EAGAIN;
}

// ============================================================================
// unix_recv - Receive data from a connected UNIX socket
// ============================================================================
int unix_recv(int usockfd, void* buf, size_t len, int flags) {
    (void)flags;
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;

    uint8_t* dst = (uint8_t*)buf;

    // Wait for data
    while (us->head == us->tail) {
        if (us->peer_closed || (us->peer && us->peer->closed))
            return 0;  // EOF
        if (!us->connected && us->type == SOCK_STREAM)
            return -ENOTCONN;
        if (us->nonblock) return -EAGAIN;
        __asm__ volatile("pause");
    }

    uint64_t irqflags;
    spin_lock_irqsave(&us->lock, &irqflags);

    int received = 0;
    for (size_t i = 0; i < len; i++) {
        if (us->head == us->tail) break;
        dst[i] = us->buf[us->head];
        us->head = (us->head + 1) % (int)sizeof(us->buf);
        received++;
    }
    if (us->head == us->tail)
        us->ready = 0;
    spin_unlock_irqrestore(&us->lock, irqflags);

    return received;
}

// ============================================================================
// unix_close - Close a UNIX domain socket
// ============================================================================
int unix_close(int usockfd) {
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;

    uint64_t flags;
    spin_lock_irqsave(&us->lock, &flags);

    us->ref_count--;
    if (us->ref_count > 0) {
        spin_unlock_irqrestore(&us->lock, flags);
        return 0;
    }

    us->closed = 1;

    // Notify peer
    if (us->peer) {
        us->peer->peer_closed = 1;
        us->peer->ready = 1;  // Wake up readers
        us->peer->peer = NULL;
    }

    us->active = 0;
    us->bound = 0;
    us->listening = 0;
    us->connected = 0;
    us->peer = NULL;
    us->head = 0;
    us->tail = 0;

    spin_unlock_irqrestore(&us->lock, flags);
    return 0;
}

// ============================================================================
// unix_socketpair - Create a pair of connected UNIX domain sockets
// ============================================================================
int unix_socketpair(int type, int sv[2]) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EINVAL;

    int idx0 = unix_alloc();
    if (idx0 < 0) return -ENOMEM;
    unix_sockets[idx0].active = 1;  // Reserve

    int idx1 = unix_alloc();
    if (idx1 < 0) {
        unix_sockets[idx0].active = 0;
        return -ENOMEM;
    }

    unix_socket_t* s0 = &unix_sockets[idx0];
    unix_socket_t* s1 = &unix_sockets[idx1];

    // Initialize s0
    uint8_t* p = (uint8_t*)s0;
    for (int i = 0; i < (int)sizeof(unix_socket_t); i++) p[i] = 0;
    s0->active = 1;
    s0->type = type;
    s0->connected = 1;
    s0->ref_count = 1;
    spinlock_init(&s0->lock, "unix_sock");

    // Initialize s1
    p = (uint8_t*)s1;
    for (int i = 0; i < (int)sizeof(unix_socket_t); i++) p[i] = 0;
    s1->active = 1;
    s1->type = type;
    s1->connected = 1;
    s1->ref_count = 1;
    spinlock_init(&s1->lock, "unix_sock");

    // Link peers
    s0->peer = s1;
    s1->peer = s0;

    sv[0] = MAKE_UNIX_SOCKET_FD(idx0);
    sv[1] = MAKE_UNIX_SOCKET_FD(idx1);

    return 0;
}

// ============================================================================
// unix_shutdown - Shutdown part of a UNIX socket connection
// ============================================================================
int unix_shutdown(int usockfd, int how) {
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return -EBADF;
    if (!us->connected) return -ENOTCONN;

    if (how == SHUT_WR || how == SHUT_RDWR) {
        // Stop writing — signal peer
        if (us->peer) {
            us->peer->peer_closed = 1;
            us->peer->ready = 1;
        }
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        // Stop reading
        us->peer_closed = 1;
    }

    return 0;
}

// ============================================================================
// unix_poll - Poll a UNIX socket for events
// ============================================================================
int unix_poll(int usockfd, short events) {
    unix_socket_t* us = unix_get(usockfd);
    if (!us) return 0;

    short revents = 0;

    if (events & POLLIN) {
        if (us->head != us->tail) revents |= POLLIN;
        if (us->peer_closed || (us->peer && us->peer->closed))
            revents |= POLLIN | POLLHUP;
        if (us->listening && us->accept_head != us->accept_tail)
            revents |= POLLIN;
    }

    if (events & POLLOUT) {
        if (us->peer && !us->peer->closed) {
            int next = (us->peer->tail + 1) % (int)sizeof(us->peer->buf);
            if (next != us->peer->head) revents |= POLLOUT;
        }
        if (!us->peer || us->peer->closed) revents |= POLLHUP;
    }

    if (us->closed) revents |= POLLHUP;
    if (us->error) revents |= POLLERR;

    return revents;
}
