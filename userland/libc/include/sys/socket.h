#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <stdint.h>
#include <stddef.h>

// Socket types
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3

// Socket type flags
#define SOCK_NONBLOCK   0x0800
#define SOCK_CLOEXEC    0x80000

// Address families
#define AF_UNSPEC       0
#define AF_INET         2
#define PF_INET         AF_INET

// Socket option levels
#define SOL_SOCKET      1

// Socket options
#define SO_REUSEADDR    2
#define SO_ERROR        4
#define SO_KEEPALIVE    9
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21

// Shutdown modes
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

// Message flags
#define MSG_PEEK        0x02
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

// Scatter/gather I/O
struct iovec {
    void*  iov_base;
    size_t iov_len;
};

struct msghdr {
    void*         msg_name;
    socklen_t     msg_namelen;
    struct iovec* msg_iov;
    size_t        msg_iovlen;
    void*         msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

// Socket syscall wrappers
int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);
ssize_t sendfile(int out_fd, int in_fd, int64_t *offset, size_t count);
int shutdown(int sockfd, int how);
int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen);
int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen);
int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

#endif /* _SYS_SOCKET_H */
