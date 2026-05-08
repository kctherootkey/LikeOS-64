// LikeOS-64 UNIX Domain Sockets
#include "../../include/kernel/net.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/console.h"

// UNIX socket table
static unix_socket_t unix_sockets[MAX_UNIX_SOCKETS];

// Global table lock — serialises slot allocation, path lookup, and peer
// linkage.  Without it, two concurrent socket(AF_UNIX,...) calls can both
// observe the same inactive slot and race on memset/active=1, producing
// two fds that alias the same unix_socket_t.  Subsequent close on either
// fd then reuses the slot under the other task's feet, and a later
// unix_send/recv dereferences a half-initialised struct (peer pointer
// stale, lock state corrupt) — a classic UAF that surfaces as a kernel
// `ret` to a small/garbage RIP after many iterations.
//
// Held only briefly across slot scans and pointer rewires; never held
// across copy_from/to_user, slab_alloc, or busy-waits.
static spinlock_t unix_table_lock = SPINLOCK_INIT("unix_table");

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
// Caller MUST hold unix_table_lock.  Marks the slot active=1 atomically with
// the search so no second caller can claim the same index.
// ============================================================================
static int unix_alloc_locked(void) {
    for (int i = 0; i < MAX_UNIX_SOCKETS; i++) {
        if (!unix_sockets[i].active) {
            // Claim the slot before releasing the table lock.  active=1
            // makes the slot opaque to other allocators and to
            // unix_find_by_path (which also requires bound=1).
            unix_sockets[i].active = 1;
            return i;
        }
    }
    return -1;
}

// ============================================================================
// unix_find_by_path - Find a bound & listening socket by path
// Caller MUST hold unix_table_lock so the returned pointer is stable
// against concurrent close (which clears active/bound under the lock).
// ============================================================================
static unix_socket_t* unix_find_by_path_locked(const char* path) {
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

    uint64_t tflags;
    spin_lock_irqsave(&unix_table_lock, &tflags);
    int idx = unix_alloc_locked();
    if (idx < 0) {
        spin_unlock_irqrestore(&unix_table_lock, tflags);
        return -ENOMEM;
    }

    unix_socket_t* us = &unix_sockets[idx];
    // Zero out the struct, then re-establish active=1 (claimed by alloc).
    // The zeroing is done while we still hold unix_table_lock so that no
    // other allocator/finder can observe the half-zeroed state.
    uint8_t* p = (uint8_t*)us;
    for (int i = 0; i < (int)sizeof(unix_socket_t); i++) p[i] = 0;

    us->active = 1;
    us->type = type;
    us->ref_count = 1;
    spinlock_init(&us->lock, "unix_sock");

    spin_unlock_irqrestore(&unix_table_lock, tflags);
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

    // Path lookup + write of bound=1 must be atomic against other binds
    // and against unix_close clearing bound — otherwise two binds can
    // both see the path free and both succeed, producing duplicates that
    // confuse subsequent unix_find_by_path callers.
    uint64_t tflags;
    spin_lock_irqsave(&unix_table_lock, &tflags);
    if (addr->sun_path[0] && unix_find_by_path_locked(addr->sun_path)) {
        spin_unlock_irqrestore(&unix_table_lock, tflags);
        return -EADDRINUSE;
    }
    uds_strcpy(us->path, addr->sun_path, UNIX_PATH_MAX);
    us->bound = 1;
    spin_unlock_irqrestore(&unix_table_lock, tflags);

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

    // Wait for incoming connection.  Re-fetch the queue indices each
    // iteration via volatile reads so the compiler can't hoist them out
    // of the loop, and bail if the listener was closed under us.  We
    // MUST yield rather than just pause: in the common case the task
    // that will enqueue (a forked child running unix_connect) is on the
    // same CPU as the listener, and a tight pause loop here will starve
    // it forever — the test hangs with parent on accept and no child
    // visible because the child never gets CPU.
    for (;;) {
        int h = *(volatile int*)&us->accept_head;
        int t = *(volatile int*)&us->accept_tail;
        if (h != t) break;
        if (!*(volatile int*)&us->active) return -EBADF;
        if (us->nonblock) return -EAGAIN;
        sched_yield_in_kernel();
    }

    uint64_t flags;
    spin_lock_irqsave(&us->lock, &flags);
    unix_socket_t* client = NULL;
    if (us->accept_head != us->accept_tail) {
        int h = us->accept_head;
        if (h >= 0 && h < 16) {
            client = us->accept_queue[h];
            us->accept_queue[h] = NULL;
            us->accept_head = (h + 1) % 16;
        }
    }
    spin_unlock_irqrestore(&us->lock, flags);

    if (!client) return -EAGAIN;

    // Allocate server-side socket — must hold table lock across the slot
    // scan + memset + active=1 publish so no concurrent unix_create can
    // alias the same slot (the bug that caused random kernel UAF crashes
    // after many teststress iterations).
    uint64_t tflags;
    spin_lock_irqsave(&unix_table_lock, &tflags);
    int new_idx = unix_alloc_locked();
    if (new_idx < 0) {
        spin_unlock_irqrestore(&unix_table_lock, tflags);
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

    // Link peers under the table lock so close cannot race with the
    // bidirectional pointer write (close clears peer->peer = NULL also
    // under unix_table_lock — see unix_close).
    server->peer = client;
    client->peer = server;
    client->connected = 1;
    spin_unlock_irqrestore(&unix_table_lock, tflags);

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

    // Look up listener and enqueue under unix_table_lock so the listener
    // cannot be closed (and its slot reused) between lookup and enqueue.
    uint64_t tflags;
    spin_lock_irqsave(&unix_table_lock, &tflags);
    unix_socket_t* listener = unix_find_by_path_locked(addr->sun_path);
    if (!listener || !listener->listening) {
        spin_unlock_irqrestore(&unix_table_lock, tflags);
        return -ECONNREFUSED;
    }

    uint64_t flags;
    spin_lock_irqsave(&listener->lock, &flags);

    // Check accept queue capacity
    int next = (listener->accept_tail + 1) % 16;
    if (next == listener->accept_head) {
        spin_unlock_irqrestore(&listener->lock, flags);
        spin_unlock_irqrestore(&unix_table_lock, tflags);
        return -ECONNREFUSED;
    }

    // Enqueue ourselves
    listener->accept_queue[listener->accept_tail] = us;
    listener->accept_tail = next;
    listener->accept_ready = 1;
    spin_unlock_irqrestore(&listener->lock, flags);
    spin_unlock_irqrestore(&unix_table_lock, tflags);

    // Wait for acceptance (peer link to be set up).  Re-read peer/error
    // each iteration via volatile so the compiler doesn't hoist.  Yield
    // rather than pause so the listener task on another CPU (or this
    // CPU) can actually run accept and link us.
    while (!*(volatile void**)&us->peer && !*(volatile int*)&us->error) {
        if (us->nonblock) return -EINPROGRESS;
        if (!*(volatile int*)&us->active) return -EBADF;
        sched_yield_in_kernel();
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

    // Snapshot peer pointer under unix_table_lock so it cannot be set
    // to a freed-and-reused slot mid-deref.  Once we have a reference
    // (peer->ref_count++), the slot cannot be freed under us even if
    // unix_close clears us->peer afterwards.
    uint64_t tflags;
    spin_lock_irqsave(&unix_table_lock, &tflags);
    unix_socket_t* peer = us->peer;
    if (!peer || !peer->active) {
        spin_unlock_irqrestore(&unix_table_lock, tflags);
        if (us->type == SOCK_DGRAM) return -EDESTADDRREQ;
        return -ENOTCONN;
    }
    __atomic_fetch_add(&peer->ref_count, 1, __ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&unix_table_lock, tflags);

    if (peer->closed || peer->peer_closed) {
        __atomic_fetch_sub(&peer->ref_count, 1, __ATOMIC_ACQ_REL);
        return -EPIPE;
    }

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
            if (us->nonblock) {
                __atomic_fetch_sub(&peer->ref_count, 1, __ATOMIC_ACQ_REL);
                return -EAGAIN;
            }
            // Wait for space — yield so the receiver gets CPU and drains.
            while ((peer->tail + 1) % (int)sizeof(peer->buf) == peer->head) {
                if (peer->closed) {
                    __atomic_fetch_sub(&peer->ref_count, 1, __ATOMIC_ACQ_REL);
                    return -EPIPE;
                }
                sched_yield_in_kernel();
            }
            spin_lock_irqsave(&peer->lock, &irqflags);
            next = (peer->tail + 1) % (int)sizeof(peer->buf);
            if (next == peer->head) break;
        }
        smap_disable();
        peer->buf[peer->tail] = src[i];
        smap_enable();
        peer->tail = next;
        peer->bytes_written++;
        sent++;
    }
    peer->ready = 1;
    spin_unlock_irqrestore(&peer->lock, irqflags);

    // Drop the reference we took above.  If this was the last ref and
    // peer was already closed, unix_close's deferred-free logic (or
    // our own dec dropping ref to 0) will tear it down on next close.
    __atomic_fetch_sub(&peer->ref_count, 1, __ATOMIC_ACQ_REL);

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

    // Wait for data.  Re-read indices and peer-state via volatile each
    // iteration so the compiler can't hoist them out of the loop.  Also
    // bail if our own slot is closed under us (e.g. dup'd fd in another
    // task closed twice).
    for (;;) {
        int h = *(volatile int*)&us->head;
        int t = *(volatile int*)&us->tail;
        if (h != t) break;
        if (!*(volatile int*)&us->active) return -EBADF;
        if (*(volatile int*)&us->peer_closed) return 0;
        volatile unix_socket_t* const* peer_slot = (volatile unix_socket_t* const*)&us->peer;
        unix_socket_t* p = (unix_socket_t*)*peer_slot;
        if (p && *(volatile int*)&p->closed) return 0;
        if (!us->connected && us->type == SOCK_STREAM) return -ENOTCONN;
        if (us->nonblock) return -EAGAIN;
        sched_yield_in_kernel();
    }

    uint64_t irqflags;
    spin_lock_irqsave(&us->lock, &irqflags);

    int received = 0;
    for (size_t i = 0; i < len; i++) {
        if (us->head == us->tail) break;
        smap_disable();
        dst[i] = us->buf[us->head];
        smap_enable();
        us->head = (us->head + 1) % (int)sizeof(us->buf);
        us->bytes_read++;
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

    int old = __atomic_fetch_sub(&us->ref_count, 1, __ATOMIC_ACQ_REL);
    if (old > 1) return 0;

    uint64_t tflags;
    spin_lock_irqsave(&unix_table_lock, &tflags);

    // If a unix_send between dec and lock acquire took a reference, it
    // bumped ref_count back above 0.  Defer teardown to the caller that
    // drops the last ref; we just return.
    if (__atomic_load_n(&us->ref_count, __ATOMIC_ACQUIRE) > 0) {
        spin_unlock_irqrestore(&unix_table_lock, tflags);
        return 0;
    }

    uint64_t flags;
    spin_lock_irqsave(&us->lock, &flags);

    us->closed = 1;

    // Notify peer.  Both peer-link writes happen under unix_table_lock
    // so a concurrent unix_send observing us->peer cannot capture the
    // pointer just before peer is freed.
    unix_socket_t* peer = us->peer;
    if (peer) {
        peer->peer_closed = 1;
        peer->ready = 1;  // Wake up readers
        peer->peer = NULL;
    }

    us->active = 0;
    us->bound = 0;
    us->listening = 0;
    us->connected = 0;
    us->peer = NULL;
    us->head = 0;
    us->tail = 0;

    spin_unlock_irqrestore(&us->lock, flags);
    spin_unlock_irqrestore(&unix_table_lock, tflags);
    return 0;
}

// ============================================================================
// unix_socketpair - Create a pair of connected UNIX domain sockets
// ============================================================================
int unix_socketpair(int type, int sv[2]) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EINVAL;

    // Both slot allocations + memsets + active=1 publish must happen
    // under unix_table_lock so no concurrent unix_create can alias the
    // same slot.
    uint64_t tflags;
    spin_lock_irqsave(&unix_table_lock, &tflags);
    int idx0 = unix_alloc_locked();
    if (idx0 < 0) {
        spin_unlock_irqrestore(&unix_table_lock, tflags);
        return -ENOMEM;
    }

    int idx1 = unix_alloc_locked();
    if (idx1 < 0) {
        unix_sockets[idx0].active = 0;
        spin_unlock_irqrestore(&unix_table_lock, tflags);
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
    spin_unlock_irqrestore(&unix_table_lock, tflags);

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

// ============================================================================
// SCM_RIGHTS file-descriptor passing
// ============================================================================
// Per-socket FIFO of pending in-band file descriptors.  Sender pushes the
// fd_table entry (an opaque void* — could be a vfs_file_t*, a socket
// marker, a pipe end, or a stdio marker 1..3) onto the *peer's* queue
// after taking the appropriate reference.  Receiver pops the head entry
// in recvmsg and installs it in its own fd_table.
//
// The queue is small (16) because the imsg framing tmux uses sends at
// most one fd per message and the peer drains promptly.  When full we
// return -EAGAIN so the sender can retry; this preserves ordering with
// data bytes already accepted by unix_send.
int unix_push_fd(unix_socket_t* sock, void* entry) {
    if (!sock) return -EBADF;
    uint64_t flags;
    spin_lock_irqsave(&sock->lock, &flags);
    int next = (sock->pending_fd_tail + 1) % 16;
    if (next == sock->pending_fd_head) {
        spin_unlock_irqrestore(&sock->lock, flags);
        return -EAGAIN;
    }
    sock->pending_fds[sock->pending_fd_tail] = entry;
    /* Associate this fd with the byte offset that immediately precedes
     * the data the sender is about to push.  recvmsg uses this to clamp
     * the byte count it returns so the cmsg lines up with the right
     * imsg frame on the peer side. */
    sock->pending_fd_off[sock->pending_fd_tail] = sock->bytes_written;
    sock->pending_fd_tail = next;
    spin_unlock_irqrestore(&sock->lock, flags);
    return 0;
}

int unix_pop_fd(unix_socket_t* sock, void** out_entry) {
    if (!sock || !out_entry) return -EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&sock->lock, &flags);
    if (sock->pending_fd_head == sock->pending_fd_tail) {
        spin_unlock_irqrestore(&sock->lock, flags);
        return -EAGAIN;
    }
    *out_entry = sock->pending_fds[sock->pending_fd_head];
    sock->pending_fd_head = (sock->pending_fd_head + 1) % 16;
    spin_unlock_irqrestore(&sock->lock, flags);
    return 0;
}

int unix_peek_fd_offset(unix_socket_t* sock, uint64_t* out_off) {
    if (!sock || !out_off) return -EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&sock->lock, &flags);
    if (sock->pending_fd_head == sock->pending_fd_tail) {
        spin_unlock_irqrestore(&sock->lock, flags);
        return -EAGAIN;
    }
    *out_off = sock->pending_fd_off[sock->pending_fd_head];
    spin_unlock_irqrestore(&sock->lock, flags);
    return 0;
}
