// LikeOS libc socket syscall wrappers
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <poll.h>
#include <errno.h>
#include "syscall.h"

int socket(int domain, int type, int protocol) {
    long ret = syscall3(SYS_SOCKET, domain, type, protocol);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    long ret = syscall3(SYS_BIND, sockfd, (long)addr, addrlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int listen(int sockfd, int backlog) {
    long ret = syscall2(SYS_LISTEN, sockfd, backlog);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    long ret = syscall3(SYS_ACCEPT, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    long ret = syscall3(SYS_CONNECT, sockfd, (long)addr, addrlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    long ret = syscall6(SYS_SENDTO, sockfd, (long)buf, (long)len,
                        flags, (long)dest_addr, addrlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    long ret = syscall6(SYS_RECVFROM, sockfd, (long)buf, (long)len,
                        flags, (long)src_addr, (long)addrlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    long ret = syscall4(SYS_SEND, sockfd, (long)buf, (long)len, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    long ret = syscall4(SYS_RECV, sockfd, (long)buf, (long)len, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

int shutdown(int sockfd, int how) {
    long ret = syscall2(SYS_SHUTDOWN, sockfd, how);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen) {
    long ret = syscall5(SYS_SETSOCKOPT, sockfd, level, optname,
                        (long)optval, optlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen) {
    long ret = syscall5(SYS_GETSOCKOPT, sockfd, level, optname,
                        (long)optval, (long)optlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    long ret = syscall3(SYS_GETPEERNAME, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    long ret = syscall3(SYS_GETSOCKNAME, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    long ret = syscall4(SYS_SOCKETPAIR, domain, type, protocol, (long)sv);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    long ret = syscall4(SYS_ACCEPT4, sockfd, (long)addr, (long)addrlen, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    long ret = syscall3(SYS_SENDMSG, sockfd, (long)msg, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    long ret = syscall3(SYS_RECVMSG, sockfd, (long)msg, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

ssize_t sendfile(int out_fd, int in_fd, int64_t *offset, size_t count) {
    long ret = syscall4(SYS_SENDFILE, out_fd, in_fd, (long)offset, (long)count);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    long ret = syscall5(SYS_SELECT, nfds, (long)readfds, (long)writefds,
                        (long)exceptfds, (long)timeout);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const void *sigmask) {
    long ret = syscall6(SYS_PSELECT6, nfds, (long)readfds, (long)writefds,
                        (long)exceptfds, (long)timeout, (long)sigmask);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    long ret = syscall3(SYS_POLL, (long)fds, (long)nfds, timeout);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int ppoll(struct pollfd *fds, nfds_t nfds,
          const struct timespec *tmo_p, const void *sigmask) {
    long ret = syscall4(SYS_PPOLL, (long)fds, (long)nfds, (long)tmo_p, (long)sigmask);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int epoll_create(int size) {
    long ret = syscall1(SYS_EPOLL_CREATE, size);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int epoll_create1(int flags) {
    long ret = syscall1(SYS_EPOLL_CREATE1, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    long ret = syscall4(SYS_EPOLL_CTL, epfd, op, fd, (long)event);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int epoll_wait(int epfd, struct epoll_event *events,
               int maxevents, int timeout) {
    long ret = syscall4(SYS_EPOLL_WAIT, epfd, (long)events, maxevents, timeout);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int epoll_pwait(int epfd, struct epoll_event *events,
                int maxevents, int timeout, const void *sigmask) {
    long ret = syscall5(SYS_EPOLL_PWAIT, epfd, (long)events, maxevents,
                        timeout, (long)sigmask);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int dup3(int oldfd, int newfd, int flags) {
    long ret = syscall3(SYS_DUP3, oldfd, newfd, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}
