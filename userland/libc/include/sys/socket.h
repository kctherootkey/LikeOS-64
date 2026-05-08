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
#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2
#define PF_UNSPEC       AF_UNSPEC
#define PF_UNIX         AF_UNIX
#define PF_LOCAL        AF_LOCAL
#define PF_INET         AF_INET

// Socket option levels
#define SOL_SOCKET      1
#define SOL_IP          0
#define SOL_TCP         6
#define SOL_UDP         17

// Socket options (SOL_SOCKET)
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_LINGER       13
#define SO_REUSEPORT    15
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_BINDTODEVICE 25

struct linger {
    int l_onoff;
    int l_linger;
};

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

/* Generic socket-address container large enough to hold any AF. */
struct sockaddr_storage {
    sa_family_t ss_family;
    char        __ss_pad1[6];
    long        __ss_align;
    char        __ss_pad2[112];
};

// Scatter/gather I/O
#ifndef _STRUCT_IOVEC_DEFINED
#define _STRUCT_IOVEC_DEFINED
struct iovec {
    void*  iov_base;
    size_t iov_len;
};
#endif

struct msghdr {
    void*         msg_name;
    socklen_t     msg_namelen;
    struct iovec* msg_iov;
    size_t        msg_iovlen;
    void*         msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

// Ancillary data records — RFC 2292 §4.2.
struct cmsghdr {
    socklen_t cmsg_len;
    int       cmsg_level;
    int       cmsg_type;
};

#define CMSG_ALIGN(n)   (((n) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define CMSG_DATA(c)    ((unsigned char*)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(l)     (CMSG_ALIGN(sizeof(struct cmsghdr)) + (l))
#define CMSG_SPACE(l)   (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(l))
#define CMSG_FIRSTHDR(m) \
    ((m)->msg_controllen >= sizeof(struct cmsghdr) \
        ? (struct cmsghdr*)(m)->msg_control : (struct cmsghdr*)0)
#define CMSG_NXTHDR(m, c) \
    (((unsigned char*)(c) + CMSG_ALIGN((c)->cmsg_len) + sizeof(struct cmsghdr) > \
      (unsigned char*)(m)->msg_control + (m)->msg_controllen) \
        ? (struct cmsghdr*)0 \
        : (struct cmsghdr*)((unsigned char*)(c) + CMSG_ALIGN((c)->cmsg_len)))

/* Ancillary message types (SOL_SOCKET) */
#define SCM_RIGHTS      0x01
#define SCM_CREDENTIALS 0x02

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
