// LikeOS-64 Poll / Select / Epoll Implementation
// Multiplexed I/O for sockets, pipes, and regular file descriptors

#include "../../include/kernel/net.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/pipe.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/devfs.h"

// ============================================================================
// Epoll instance table
// ============================================================================
epoll_instance_t epoll_instances[MAX_EPOLL_INSTANCES];
static spinlock_t epoll_lock = SPINLOCK_INIT("epoll");

// ============================================================================
// fd_poll_one - Poll a single fd for events (internal helper)
// Returns revents mask. Works for sockets, pipes, regular files, console.
// ============================================================================
static short fd_poll_one(int fd, short events) {
    task_t* cur = sched_current();
    if (!cur) return POLLNVAL;
    if (fd < 0) return POLLNVAL;

    // Check if it's a console fd (0-2 with NULL entry).
    // /dev/console is bidirectional, so any of fd 0/1/2 may be polled
    // for both POLLIN and POLLOUT (matches real-tty semantics).
    if (fd < 3) {
        short rev = 0;
        if (events & (POLLIN | POLLRDNORM)) {
            tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
            if (tty && tty->read_count > 0)
                rev |= POLLIN | POLLRDNORM;
        }
        if (events & (POLLOUT | POLLWRNORM))
            rev |= POLLOUT | POLLWRNORM;
        return rev;
    }

    if ((unsigned)fd >= TASK_MAX_FDS) return POLLNVAL;

    void* entry = cur->fd_table[fd];
    if (!entry) return POLLNVAL;

    // Socket fd marker
    if (IS_SOCKET_FD(entry)) {
        return (short)sock_poll(SOCKET_FD_IDX(entry), events);
    }

    // UNIX socket fd marker
    if (IS_UNIX_SOCKET_FD(entry)) {
        return (short)unix_poll((int)(uintptr_t)entry, events);
    }

    // Epoll fd marker
    if (IS_EPOLL_FD(entry)) {
        // Epoll fds are not themselves pollable in a meaningful way
        return POLLNVAL;
    }

    // Console dup markers (1, 2, 3) — all reference the bidirectional
    // /dev/console device, so each is both readable and writable.
    uintptr_t marker = (uintptr_t)entry;
    if (marker >= 1 && marker <= 3) {
        short rev = 0;
        if (events & (POLLIN | POLLRDNORM)) {
            tty_t* tty = cur->ctty ? cur->ctty : tty_get_console();
            if (tty && tty->read_count > 0)
                rev |= POLLIN | POLLRDNORM;
        }
        if (events & (POLLOUT | POLLWRNORM))
            rev |= POLLOUT | POLLWRNORM;
        return rev;
    }

    // Pipe fd
    if (pipe_is_end(entry)) {
        pipe_end_t* pe = (pipe_end_t*)entry;
        pipe_t* p = pe->pipe;
        short rev = 0;
        if (pe->is_read) {
            if ((events & (POLLIN | POLLRDNORM)) && p->used > 0)
                rev |= POLLIN | POLLRDNORM;
            if (p->writers == 0)
                rev |= POLLHUP;
        } else {
            if ((events & (POLLOUT | POLLWRNORM)) && p->used < p->size)
                rev |= POLLOUT | POLLWRNORM;
            if (p->readers == 0)
                rev |= POLLERR;
        }
        return rev;
    }

    // Pty master (opened via /dev/ptmx): readable when slave wrote bytes,
    // writable when slave is open, HUP when slave closed and buffer empty.
    {
        int pid = devfs_get_pty_master_id((vfs_file_t*)entry);
        if (pid >= 0)
            return (short)tty_pty_master_poll(pid, events);
    }

    // Tty/pty-slave (real terminal): always writable, readable when input is queued.
    {
        tty_t* tty = devfs_get_tty((vfs_file_t*)entry);
        if (tty) {
            short rev = 0;
            if ((events & (POLLIN | POLLRDNORM)) && tty->read_count > 0)
                rev |= POLLIN | POLLRDNORM;
            if (events & (POLLOUT | POLLWRNORM))
                rev |= POLLOUT | POLLWRNORM;
            return rev;
        }
    }

    // Regular file - always ready for read/write
    short rev = 0;
    if (events & (POLLIN | POLLRDNORM))
        rev |= POLLIN | POLLRDNORM;
    if (events & (POLLOUT | POLLWRNORM))
        rev |= POLLOUT | POLLWRNORM;
    return rev;
}

// ============================================================================
// sys_select_internal - select() implementation
// Scans readfds/writefds/exceptfds for ready file descriptors.
// timeout_ticks: 0 = poll (non-blocking), (uint64_t)-1 = block forever
// Returns number of ready fds, or negative errno.
// ============================================================================
int sys_select_internal(int nfds, fd_set* readfds, fd_set* writefds,
                        fd_set* exceptfds, uint64_t timeout_ticks) {
    if (nfds < 0 || nfds > FD_SETSIZE) return -EINVAL;

    fd_set r_in, w_in, e_in;
    if (readfds) r_in = *readfds; else FD_ZERO(&r_in);
    if (writefds) w_in = *writefds; else FD_ZERO(&w_in);
    if (exceptfds) e_in = *exceptfds; else FD_ZERO(&e_in);

    uint64_t deadline = 0;
    if (timeout_ticks == 0) {
        // Non-blocking poll
    } else if (timeout_ticks != (uint64_t)-1) {
        deadline = timer_ticks() + timeout_ticks;
    }

    while (1) {
        int count = 0;
        fd_set r_out, w_out, e_out;
        FD_ZERO(&r_out);
        FD_ZERO(&w_out);
        FD_ZERO(&e_out);

        for (int fd = 0; fd < nfds; fd++) {
            short events = 0;
            if (readfds && FD_ISSET(fd, &r_in)) events |= POLLIN;
            if (writefds && FD_ISSET(fd, &w_in)) events |= POLLOUT;
            if (exceptfds && FD_ISSET(fd, &e_in)) events |= POLLPRI;

            if (events == 0) continue;

            short rev = fd_poll_one(fd, events);

            if ((rev & (POLLIN | POLLRDNORM | POLLHUP | POLLERR)) &&
                readfds && FD_ISSET(fd, &r_in)) {
                FD_SET(fd, &r_out);
                count++;
            }
            if ((rev & (POLLOUT | POLLWRNORM)) &&
                writefds && FD_ISSET(fd, &w_in)) {
                FD_SET(fd, &w_out);
                count++;
            }
            if ((rev & (POLLERR | POLLPRI)) &&
                exceptfds && FD_ISSET(fd, &e_in)) {
                FD_SET(fd, &e_out);
                count++;
            }
        }

        if (count > 0 || timeout_ticks == 0) {
            if (readfds) *readfds = r_out;
            if (writefds) *writefds = w_out;
            if (exceptfds) *exceptfds = e_out;
            return count;
        }

        // Block until deadline or forever
        if (timeout_ticks != (uint64_t)-1 && timer_ticks() >= deadline) {
            if (readfds) FD_ZERO(readfds);
            if (writefds) FD_ZERO(writefds);
            if (exceptfds) FD_ZERO(exceptfds);
            return 0;
        }

        __asm__ volatile("pause");
    }
}

// ============================================================================
// sys_poll_internal - poll() implementation
// Scans array of pollfd structs for ready fds.
// timeout_ticks: 0 = non-blocking, (uint64_t)-1 = block forever
// Returns number of ready fds, or negative errno.
// ============================================================================
int sys_poll_internal(struct pollfd* fds, int nfds, uint64_t timeout_ticks) {
    if (nfds < 0 || !fds) return -EINVAL;

    uint64_t deadline = 0;
    if (timeout_ticks == 0) {
        // Non-blocking poll
    } else if (timeout_ticks != (uint64_t)-1) {
        deadline = timer_ticks() + timeout_ticks;
    }

    while (1) {
        int count = 0;

        for (int i = 0; i < nfds; i++) {
            fds[i].revents = fd_poll_one(fds[i].fd, fds[i].events);
            if (fds[i].revents != 0)
                count++;
        }

        if (count > 0 || timeout_ticks == 0)
            return count;

        if (timeout_ticks != (uint64_t)-1 && timer_ticks() >= deadline)
            return 0;

        __asm__ volatile("pause");
    }
}

// ============================================================================
// Epoll implementation
// ============================================================================

int epoll_create_internal(int flags) {
    (void)flags;
    uint64_t fl;
    spin_lock_irqsave(&epoll_lock, &fl);

    int idx = -1;
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (!epoll_instances[i].active) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        spin_unlock_irqrestore(&epoll_lock, fl);
        return -ENFILE;
    }

    epoll_instance_t* ep = &epoll_instances[idx];
    ep->active = 1;
    ep->nentries = 0;
    ep->lock = (spinlock_t)SPINLOCK_INIT("epoll_inst");

    spin_unlock_irqrestore(&epoll_lock, fl);
    return idx;
}

int epoll_ctl_internal(int epfd_idx, int op, int fd, struct epoll_event* event) {
    if (epfd_idx < 0 || epfd_idx >= MAX_EPOLL_INSTANCES) return -EBADF;
    epoll_instance_t* ep = &epoll_instances[epfd_idx];
    if (!ep->active) return -EBADF;

    uint64_t fl;
    spin_lock_irqsave(&ep->lock, &fl);

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (!event) { spin_unlock_irqrestore(&ep->lock, fl); return -EINVAL; }
        // Check for duplicate
        for (int i = 0; i < ep->nentries; i++) {
            if (ep->entries[i].fd == fd) {
                spin_unlock_irqrestore(&ep->lock, fl);
                return -EEXIST;
            }
        }
        if (ep->nentries >= MAX_EPOLL_ENTRIES) {
            spin_unlock_irqrestore(&ep->lock, fl);
            return -ENOMEM;
        }
        int idx = ep->nentries++;
        ep->entries[idx].fd = fd;
        ep->entries[idx].events = event->events;
        ep->entries[idx].data = event->data.u64;
        ep->entries[idx].oneshot_triggered = 0;
        break;
    }
    case EPOLL_CTL_DEL: {
        int found = -1;
        for (int i = 0; i < ep->nentries; i++) {
            if (ep->entries[i].fd == fd) { found = i; break; }
        }
        if (found < 0) {
            spin_unlock_irqrestore(&ep->lock, fl);
            return -ENOENT;
        }
        // Shift entries down
        for (int i = found; i < ep->nentries - 1; i++)
            ep->entries[i] = ep->entries[i + 1];
        ep->nentries--;
        break;
    }
    case EPOLL_CTL_MOD: {
        if (!event) { spin_unlock_irqrestore(&ep->lock, fl); return -EINVAL; }
        int found = -1;
        for (int i = 0; i < ep->nentries; i++) {
            if (ep->entries[i].fd == fd) { found = i; break; }
        }
        if (found < 0) {
            spin_unlock_irqrestore(&ep->lock, fl);
            return -ENOENT;
        }
        ep->entries[found].events = event->events;
        ep->entries[found].data = event->data.u64;
        ep->entries[found].oneshot_triggered = 0;
        break;
    }
    default:
        spin_unlock_irqrestore(&ep->lock, fl);
        return -EINVAL;
    }

    spin_unlock_irqrestore(&ep->lock, fl);
    return 0;
}

int epoll_wait_internal(int epfd_idx, struct epoll_event* events,
                        int maxevents, uint64_t timeout_ticks) {
    if (epfd_idx < 0 || epfd_idx >= MAX_EPOLL_INSTANCES) return -EBADF;
    epoll_instance_t* ep = &epoll_instances[epfd_idx];
    if (!ep->active) return -EBADF;
    if (maxevents <= 0 || !events) return -EINVAL;

    uint64_t deadline = 0;
    if (timeout_ticks == 0) {
        // Non-blocking
    } else if (timeout_ticks != (uint64_t)-1) {
        deadline = timer_ticks() + timeout_ticks;
    }

    while (1) {
        int count = 0;

        uint64_t fl;
        spin_lock_irqsave(&ep->lock, &fl);

        for (int i = 0; i < ep->nentries && count < maxevents; i++) {
            if (ep->entries[i].oneshot_triggered &&
                (ep->entries[i].events & EPOLLONESHOT))
                continue;

            short poll_events = 0;
            if (ep->entries[i].events & (EPOLLIN | EPOLLRDNORM))
                poll_events |= POLLIN;
            if (ep->entries[i].events & (EPOLLOUT | EPOLLWRNORM))
                poll_events |= POLLOUT;
            if (ep->entries[i].events & EPOLLPRI)
                poll_events |= POLLPRI;

            short rev = fd_poll_one(ep->entries[i].fd, poll_events);
            if (rev == 0) continue;

            uint32_t ep_events = 0;
            if (rev & (POLLIN | POLLRDNORM)) ep_events |= EPOLLIN;
            if (rev & (POLLOUT | POLLWRNORM)) ep_events |= EPOLLOUT;
            if (rev & POLLERR) ep_events |= EPOLLERR;
            if (rev & POLLHUP) ep_events |= EPOLLHUP;
            if (rev & POLLPRI) ep_events |= EPOLLPRI;

            ep_events &= (ep->entries[i].events | EPOLLERR | EPOLLHUP);
            if (ep_events == 0) continue;

            events[count].events = ep_events;
            events[count].data.u64 = ep->entries[i].data;
            count++;

            if (ep->entries[i].events & EPOLLONESHOT)
                ep->entries[i].oneshot_triggered = 1;
        }

        spin_unlock_irqrestore(&ep->lock, fl);

        if (count > 0 || timeout_ticks == 0)
            return count;

        if (timeout_ticks != (uint64_t)-1 && timer_ticks() >= deadline)
            return 0;

        __asm__ volatile("pause");
    }
}
