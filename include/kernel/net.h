// LikeOS-64 Network Subsystem Interface
// Network device abstraction, protocol dispatch, and socket API

#ifndef _KERNEL_NET_H_
#define _KERNEL_NET_H_

#include "types.h"
#include "sched.h"  // spinlock_t

// ============================================================================
// Network Configuration
// ============================================================================
#define NET_MAX_DEVICES     4
#define NET_MTU_DEFAULT     1500
#define ETH_ALEN            6       // Ethernet address length
#define ETH_HLEN            14      // Ethernet header length
#define ETH_FRAME_MAX       1518    // Max Ethernet frame (MTU + header + FCS)
#define NET_RX_RING_SIZE    256
#define NET_TX_RING_SIZE    256
#define NET_RX_BUF_SIZE     2048

// ============================================================================
// Ethernet Constants
// ============================================================================
#define ETH_P_IP            0x0800
#define ETH_P_ARP           0x0806
#define ETH_P_IPV6          0x86DD

// Broadcast MAC address
extern const uint8_t eth_broadcast_addr[ETH_ALEN];

// ============================================================================
// IP Protocol Numbers
// ============================================================================
#define IP_PROTO_ICMP       1
#define IP_PROTO_TCP        6
#define IP_PROTO_UDP        17

// ============================================================================
// Network Byte Order Helpers
// ============================================================================
static inline uint16_t net_htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint16_t net_ntohs(uint16_t x) {
    return net_htons(x);
}

static inline uint32_t net_htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) |
           ((x >> 8)  & 0xFF00) |
           ((x << 8)  & 0xFF0000) |
           ((x << 24) & 0xFF000000);
}

static inline uint32_t net_ntohl(uint32_t x) {
    return net_htonl(x);
}

// ============================================================================
// IP Address Helpers
// ============================================================================
#define IP4_ADDR(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

// ============================================================================
// Network Device Abstraction
// ============================================================================
typedef struct net_device net_device_t;

typedef int (*net_send_fn)(net_device_t* dev, const uint8_t* data, uint16_t len);
typedef int (*net_link_status_fn)(net_device_t* dev);

struct net_device {
    const char* name;
    uint8_t mac_addr[ETH_ALEN];
    uint16_t mtu;
    uint32_t ip_addr;           // IPv4 address (host byte order)
    uint32_t netmask;           // Subnet mask (host byte order)
    uint32_t gateway;           // Default gateway (host byte order)
    uint32_t dns_server;        // DNS server (host byte order)
    net_send_fn send;
    net_link_status_fn link_status;
    void* driver_data;
    spinlock_t lock;

    // Statistics
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
};

// ============================================================================
// Ethernet Header
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;         // Network byte order
} eth_header_t;

// ============================================================================
// IPv4 Header
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;       // Version (4 bits) + IHL (4 bits)
    uint8_t  tos;
    uint16_t total_length;      // Network byte order
    uint16_t identification;
    uint16_t flags_fragment;    // Flags (3 bits) + Fragment offset (13 bits)
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_addr;          // Network byte order
    uint32_t dst_addr;          // Network byte order
} ipv4_header_t;

// ============================================================================
// ARP Header
// ============================================================================
typedef struct __attribute__((packed)) {
    uint16_t hw_type;           // 1 = Ethernet
    uint16_t proto_type;        // 0x0800 = IPv4
    uint8_t  hw_len;            // 6 for Ethernet
    uint8_t  proto_len;         // 4 for IPv4
    uint16_t opcode;            // 1 = request, 2 = reply
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;         // Network byte order
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;         // Network byte order
} __attribute__((packed)) arp_header_t;

#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2
#define ARP_HW_ETHER    1

// ============================================================================
// ICMP Header
// ============================================================================
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} icmp_header_t;

#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8
#define ICMP_DEST_UNREACH   3
#define ICMP_PORT_UNREACH   3   // Code for destination unreachable

// ============================================================================
// UDP Header
// ============================================================================
typedef struct __attribute__((packed)) {
    uint16_t src_port;          // Network byte order
    uint16_t dst_port;          // Network byte order
    uint16_t length;            // Network byte order
    uint16_t checksum;          // Network byte order
} udp_header_t;

// ============================================================================
// TCP Header
// ============================================================================
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;       // Upper 4 bits = offset in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

// TCP flags
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10
#define TCP_URG     0x20

// TCP states
#define TCP_STATE_CLOSED        0
#define TCP_STATE_LISTEN        1
#define TCP_STATE_SYN_SENT      2
#define TCP_STATE_SYN_RECEIVED  3
#define TCP_STATE_ESTABLISHED   4
#define TCP_STATE_FIN_WAIT_1    5
#define TCP_STATE_FIN_WAIT_2    6
#define TCP_STATE_CLOSE_WAIT    7
#define TCP_STATE_CLOSING       8
#define TCP_STATE_LAST_ACK      9
#define TCP_STATE_TIME_WAIT     10

// TCP configuration
#define TCP_MAX_CONNECTIONS     64
#define TCP_WINDOW_SIZE         32768
#define TCP_MSS                 1460    // Maximum Segment Size
#define TCP_RX_BUF_SIZE         65536
#define TCP_TX_BUF_SIZE         65536
#define TCP_RETRANSMIT_TICKS    200     // 2 seconds at 100Hz
#define TCP_TIME_WAIT_TICKS     6000    // 60 seconds
#define TCP_MAX_RETRANSMITS     5
#define TCP_SYN_RETRANSMIT_TICKS 300    // 3 seconds

// TCP connection block
typedef struct tcp_conn {
    int state;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    // Sequence numbers
    uint32_t snd_una;           // Oldest unacknowledged
    uint32_t snd_nxt;           // Next to send
    uint32_t snd_wnd;           // Send window
    uint32_t rcv_nxt;           // Next expected
    uint32_t rcv_wnd;           // Receive window
    uint32_t iss;               // Initial send sequence number
    uint32_t irs;               // Initial receive sequence number

    // Receive buffer (ring buffer)
    uint8_t* rx_buf;
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_buf_size;

    // Transmit buffer (ring buffer)
    uint8_t* tx_buf;
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t tx_buf_size;

    // Retransmission
    uint64_t retransmit_tick;
    uint32_t retransmit_count;

    // TIME_WAIT timer
    uint64_t time_wait_tick;

    // Listen queue (for server sockets)
    struct tcp_conn* accept_queue[16];
    int accept_head;
    int accept_tail;
    int backlog;

    // Parent listener (for accepted connections)
    struct tcp_conn* parent;

    net_device_t* dev;
    spinlock_t lock;
    int active;
    int error;                  // Pending error (e.g. ECONNRESET)

    // Blocking wait support
    volatile int rx_ready;      // Data available in rx_buf
    volatile int tx_ready;      // Space available in tx_buf
    volatile int accept_ready;  // Connection pending in accept_queue
    volatile int connect_done;  // Connect completed (or failed)
} tcp_conn_t;

// ============================================================================
// DHCP Structures
// ============================================================================
#define DHCP_CLIENT_PORT    68
#define DHCP_SERVER_PORT    67
#define DHCP_MAGIC_COOKIE   0x63825363

#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_RELEASE        7
#define DHCP_INFORM         8

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
    uint8_t  options[312];
} dhcp_packet_t;

// ============================================================================
// Socket Abstraction (kernel-side)
// ============================================================================
#define SOCK_STREAM     1   // TCP
#define SOCK_DGRAM      2   // UDP
#define SOCK_RAW        3   // Raw IP

#define AF_INET         2
#define PF_INET         AF_INET
#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define PF_UNIX         AF_UNIX

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8

#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define INADDR_ANY      0
#define INADDR_BROADCAST 0xFFFFFFFF

#define NET_MAX_SOCKETS 128

// Socket address structures
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;           // Network byte order
};

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    sa_family_t sin_family;     // AF_INET
    uint16_t sin_port;          // Network byte order
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

// Kernel socket structure
typedef struct net_socket {
    int type;                   // SOCK_STREAM, SOCK_DGRAM, SOCK_RAW
    int protocol;
    int domain;                 // AF_INET
    int bound;                  // Has been bound to an address
    int listening;              // Is in listen state
    int connected;              // Is connected (TCP) or has default dest (UDP)
    int closed;                 // Socket has been shut down
    int nonblock;               // O_NONBLOCK flag
    int error;                  // Pending error

    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;

    // For TCP sockets
    tcp_conn_t* tcp;

    // For UDP sockets - simple receive queue
    struct {
        uint8_t data[NET_RX_BUF_SIZE];
        uint16_t len;
        struct sockaddr_in from;
    } udp_rx_queue[16];
    int udp_rx_head;
    int udp_rx_tail;
    volatile int udp_rx_ready;

    // Socket options
    int reuse_addr;
    uint64_t rcv_timeout_ticks; // 0 = infinite

    spinlock_t lock;
    int active;
    int ref_count;
} net_socket_t;

// ============================================================================
// Network Device Registry API
// ============================================================================
void net_init(void);
void net_start_dhcp(void);
int  net_register(net_device_t* dev);
net_device_t* net_get_device(int index);
net_device_t* net_get_default_device(void);
int  net_device_count(void);

// ============================================================================
// Ethernet Layer API
// ============================================================================
int  eth_send(net_device_t* dev, const uint8_t dst_mac[ETH_ALEN],
              uint16_t ethertype, const uint8_t* payload, uint16_t len);
void eth_rx(net_device_t* dev, const uint8_t* frame, uint16_t len);

// ============================================================================
// ARP API
// ============================================================================
void arp_init(void);
int  arp_resolve(net_device_t* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]);
int  arp_cache_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]);
void arp_rx(net_device_t* dev, const uint8_t* data, uint16_t len);
void arp_request(net_device_t* dev, uint32_t target_ip);
void arp_add_entry(uint32_t ip, const uint8_t mac[ETH_ALEN]);

// ============================================================================
// IPv4 API
// ============================================================================
void ipv4_init(void);
int  ipv4_send(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
               const uint8_t* payload, uint16_t len);
int  ipv4_send_ttl(net_device_t* dev, uint32_t dst_ip, uint8_t protocol,
                   const uint8_t* payload, uint16_t len, uint8_t ttl);
void ipv4_rx(net_device_t* dev, const uint8_t* data, uint16_t len);
uint16_t ipv4_checksum(const void* data, uint16_t len);

// ============================================================================
// ICMP API
// ============================================================================
void icmp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len, uint8_t rx_ttl);

// ============================================================================
// UDP API
// ============================================================================
void udp_init(void);
void udp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len);
int  udp_send(net_device_t* dev, uint32_t dst_ip,
              uint16_t src_port, uint16_t dst_port,
              const uint8_t* data, uint16_t len);
void udp_deliver_to_socket(uint32_t src_ip, uint16_t src_port,
                           uint16_t dst_port,
                           const uint8_t* data, uint16_t len);

// ============================================================================
// TCP API
// ============================================================================
void tcp_init(void);
void tcp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len);
int  tcp_send_data(tcp_conn_t* conn, const uint8_t* data, uint16_t len);
tcp_conn_t* tcp_connect(net_device_t* dev, uint32_t dst_ip,
                        uint16_t src_port, uint16_t dst_port);
tcp_conn_t* tcp_listen(net_device_t* dev, uint32_t local_ip,
                       uint16_t local_port, int backlog);
tcp_conn_t* tcp_accept(tcp_conn_t* listener);
int  tcp_close(tcp_conn_t* conn);
void tcp_timer_tick(void);
int  tcp_send_segment(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t seq, uint32_t ack,
                      uint8_t flags, uint16_t window,
                      const uint8_t* data, uint16_t data_len);

// ============================================================================
// DHCP API
// ============================================================================
void dhcp_init(void);
int  dhcp_discover(net_device_t* dev);
void dhcp_rx(net_device_t* dev, const uint8_t* data, uint16_t len);
int  dhcp_configured(void);

// ============================================================================
// Socket API (kernel-side, called from syscall layer)
// ============================================================================
void socket_init(void);
int  sock_create(int domain, int type, int protocol);
int  sock_bind(int sockfd, const struct sockaddr_in* addr);
int  sock_listen(int sockfd, int backlog);
int  sock_accept(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen);
int  sock_connect(int sockfd, const struct sockaddr_in* addr);
int  sock_sendto(int sockfd, const void* buf, size_t len, int flags,
                 const struct sockaddr_in* dest_addr, socklen_t addrlen);
int  sock_recvfrom(int sockfd, void* buf, size_t len, int flags,
                   struct sockaddr_in* src_addr, socklen_t* addrlen);
int  sock_send(int sockfd, const void* buf, size_t len, int flags);
int  sock_recv(int sockfd, void* buf, size_t len, int flags);
int  sock_close(int sockfd);
int  sock_shutdown(int sockfd, int how);
int  sock_setsockopt(int sockfd, int level, int optname,
                     const void* optval, socklen_t optlen);
int  sock_getsockopt(int sockfd, int level, int optname,
                     void* optval, socklen_t* optlen);
int  sock_getpeername(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen);
int  sock_getsockname(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen);

// ============================================================================
// Packet RX Handler (called by NIC driver on packet receive)
// ============================================================================
void net_rx_packet(net_device_t* dev, const uint8_t* data, uint16_t len);

// ============================================================================
// Timer hook (called from timer IRQ for TCP timers)
// ============================================================================
void net_timer_tick(void);

// ============================================================================
// Socket FD Markers (stored in fd_table[] alongside vfs_file_t* and pipe markers)
// ============================================================================
#define SOCKET_FD_BASE      0x10000UL
#define EPOLL_FD_BASE       0x20000UL
#define UNIX_SOCKET_FD_BASE 0x30000UL
#define MAX_EPOLL_INSTANCES 32
#define MAX_EPOLL_ENTRIES   64
#define MAX_UNIX_SOCKETS    64

#define IS_SOCKET_FD(ptr)   ((uintptr_t)(ptr) >= SOCKET_FD_BASE && \
                             (uintptr_t)(ptr) < SOCKET_FD_BASE + NET_MAX_SOCKETS)
#define SOCKET_FD_IDX(ptr)  ((int)((uintptr_t)(ptr) - SOCKET_FD_BASE))
#define MAKE_SOCKET_FD(idx) ((struct vfs_file*)(SOCKET_FD_BASE + (unsigned)(idx)))

#define IS_EPOLL_FD(ptr)    ((uintptr_t)(ptr) >= EPOLL_FD_BASE && \
                             (uintptr_t)(ptr) < EPOLL_FD_BASE + MAX_EPOLL_INSTANCES)
#define EPOLL_FD_IDX(ptr)   ((int)((uintptr_t)(ptr) - EPOLL_FD_BASE))
#define MAKE_EPOLL_FD(idx)  ((struct vfs_file*)(EPOLL_FD_BASE + (unsigned)(idx)))

#define IS_UNIX_SOCKET_FD(ptr)  ((uintptr_t)(ptr) >= UNIX_SOCKET_FD_BASE && \
                                 (uintptr_t)(ptr) < UNIX_SOCKET_FD_BASE + MAX_UNIX_SOCKETS)
#define UNIX_SOCKET_FD_IDX(ptr) ((int)((uintptr_t)(ptr) - UNIX_SOCKET_FD_BASE))
#define MAKE_UNIX_SOCKET_FD(idx) ((struct vfs_file*)(UNIX_SOCKET_FD_BASE + (unsigned)(idx)))

// ============================================================================
// Scatter/Gather I/O (sendmsg/recvmsg)
// ============================================================================
struct iovec {
    void*  iov_base;
    size_t iov_len;
};

struct msghdr {
    void*         msg_name;       // Optional address
    socklen_t     msg_namelen;    // Size of address
    struct iovec* msg_iov;        // Scatter/gather array
    int           msg_iovlen;     // Number of elements in msg_iov
    void*         msg_control;    // Ancillary data (unused)
    size_t        msg_controllen; // Ancillary data length (unused)
    int           msg_flags;      // Flags on received message
};

// ============================================================================
// Poll / Select / Epoll
// ============================================================================
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLRDBAND  0x0080
#define POLLWRNORM  0x0100
#define POLLWRBAND  0x0200

struct pollfd {
    int   fd;
    short events;
    short revents;
};

// fd_set for select()
#define FD_SETSIZE  1024
#define __NFDBITS   (8 * sizeof(unsigned long))
#define __FD_ELT(d) ((d) / __NFDBITS)
#define __FD_MASK(d) (1UL << ((d) % __NFDBITS))

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / __NFDBITS];
} fd_set;

#define FD_ZERO(set)       do { for (unsigned _i = 0; _i < FD_SETSIZE/__NFDBITS; _i++) (set)->fds_bits[_i] = 0; } while(0)
#define FD_SET(d, set)     ((set)->fds_bits[__FD_ELT(d)] |= __FD_MASK(d))
#define FD_CLR(d, set)     ((set)->fds_bits[__FD_ELT(d)] &= ~__FD_MASK(d))
#define FD_ISSET(d, set)   (((set)->fds_bits[__FD_ELT(d)] & __FD_MASK(d)) != 0)

// Epoll
#define EPOLL_CTL_ADD   1
#define EPOLL_CTL_DEL   2
#define EPOLL_CTL_MOD   3

#define EPOLLIN         0x001
#define EPOLLPRI        0x002
#define EPOLLOUT        0x004
#define EPOLLERR        0x008
#define EPOLLHUP        0x010
#define EPOLLRDNORM     0x040
#define EPOLLRDBAND     0x080
#define EPOLLWRNORM     0x100
#define EPOLLWRBAND     0x200
#define EPOLLONESHOT    (1 << 30)
#define EPOLLET         (1 << 31)

typedef union epoll_data {
    void*    ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

// Epoll instance (kernel internal)
typedef struct {
    int active;
    struct {
        int      fd;
        uint32_t events;
        uint64_t data;
        int      oneshot_triggered;
    } entries[MAX_EPOLL_ENTRIES];
    int nentries;
    spinlock_t lock;
} epoll_instance_t;

// ============================================================================
// Network Interface Ioctls (Linux-compatible values)
// ============================================================================
// Socket-level / interface ioctls
#define SIOCGIFNAME         0x8910
#define SIOCSIFLINK         0x8911
#define SIOCGIFCONF         0x8912
#define SIOCGIFFLAGS        0x8913
#define SIOCSIFFLAGS        0x8914
#define SIOCGIFADDR         0x8915
#define SIOCSIFADDR         0x8916
#define SIOCGIFDSTADDR      0x8917
#define SIOCSIFDSTADDR      0x8918
#define SIOCGIFBRDADDR      0x8919
#define SIOCSIFBRDADDR      0x891A
#define SIOCGIFNETMASK      0x891B
#define SIOCSIFNETMASK      0x891C
#define SIOCGIFMETRIC       0x891D
#define SIOCSIFMETRIC       0x891E
#define SIOCGIFMEM          0x891F
#define SIOCSIFMEM          0x8920
#define SIOCGIFMTU          0x8921
#define SIOCSIFMTU          0x8922
#define SIOCSIFNAME         0x8923
#define SIOCSIFHWADDR       0x8924
#define SIOCGIFENCAP        0x8925
#define SIOCSIFENCAP        0x8926
#define SIOCGIFHWADDR       0x8927
#define SIOCGIFSLAVE        0x8929
#define SIOCSIFSLAVE        0x8930
#define SIOCADDMULTI        0x8931
#define SIOCDELMULTI        0x8932
#define SIOCGIFINDEX        0x8933
#define SIOGIFINDEX         SIOCGIFINDEX
#define SIOCSIFPFLAGS       0x8934
#define SIOCGIFPFLAGS       0x8935
#define SIOCDIFADDR         0x8936
#define SIOCSIFHWBROADCAST  0x8937
#define SIOCGIFCOUNT        0x8938

// Routing / ARP / protocol ioctls
#define SIOCADDRT           0x890B
#define SIOCDELRT           0x890C
#define SIOCRTMSG           0x890D
#define SIOCGARP            0x8954
#define SIOCSARP            0x8955
#define SIOCDARP            0x8956
#define SIOCGSTAMP          0x8906
#define SIOCGSTAMPNS        0x8907

// Bridge / VLAN / misc
#define SIOCBRADDBR         0x89A0
#define SIOCBRDELBR         0x89A1
#define SIOCBRADDIF         0x89A2
#define SIOCBRDELIF         0x89A3
#define SIOCGIFVLAN         0x8982
#define SIOCSIFVLAN         0x8983

// Interface flags (for SIOCGIFFLAGS / SIOCSIFFLAGS)
#define IFF_UP          0x0001
#define IFF_BROADCAST   0x0002
#define IFF_DEBUG       0x0004
#define IFF_LOOPBACK    0x0008
#define IFF_POINTOPOINT 0x0010
#define IFF_NOTRAILERS  0x0020
#define IFF_RUNNING     0x0040
#define IFF_NOARP       0x0080
#define IFF_PROMISC     0x0100
#define IFF_ALLMULTI     0x0200
#define IFF_MULTICAST   0x1000

#define IFNAMSIZ        16

// struct ifreq - interface request (for ioctl)
struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_dstaddr;
        struct sockaddr ifr_broadaddr;
        struct sockaddr ifr_netmask;
        struct sockaddr ifr_hwaddr;
        short           ifr_flags;
        int             ifr_ifindex;
        int             ifr_metric;
        int             ifr_mtu;
        char            ifr_slave[IFNAMSIZ];
        char            ifr_newname[IFNAMSIZ];
        char            ifr_data[14];
    };
};

// struct ifconf - interface configuration list
struct ifconf {
    int  ifc_len;
    union {
        char*          ifc_buf;
        struct ifreq*  ifc_req;
    };
};

// struct rtentry - routing table entry (simplified)
struct rtentry {
    struct sockaddr rt_dst;
    struct sockaddr rt_gateway;
    struct sockaddr rt_genmask;
    short           rt_flags;
    int             rt_metric;
    char*           rt_dev;
};

// struct arpreq - ARP request
struct arpreq {
    struct sockaddr arp_pa;       // Protocol address
    struct sockaddr arp_ha;       // Hardware address
    int             arp_flags;
    struct sockaddr arp_netmask;
    char            arp_dev[IFNAMSIZ];
};

#define ATF_COM     0x02    // Completed entry (valid)
#define ATF_PERM    0x04    // Permanent entry

// ============================================================================
// Extended Socket API (kernel-side)
// ============================================================================
int  sock_socketpair(int domain, int type, int protocol, int sv[2]);
int  sock_accept4(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen, int flags);
int  sock_sendmsg(int sockfd, const struct msghdr* msg, int flags);
int  sock_recvmsg(int sockfd, struct msghdr* msg, int flags);
int  sock_poll(int sockfd, short events);
int  sock_ioctl_net(int sockfd, unsigned long request, void* argp);
int  sock_fcntl_net(int sockfd, int cmd, unsigned long arg);
net_socket_t* sock_get(int sockfd);

// ============================================================================
// Poll / Select / Epoll API (kernel-side)
// ============================================================================
int  sys_select_internal(int nfds, fd_set* readfds, fd_set* writefds,
                         fd_set* exceptfds, uint64_t timeout_ticks);
int  sys_poll_internal(struct pollfd* fds, int nfds, uint64_t timeout_ticks);
int  epoll_create_internal(int flags);
int  epoll_ctl_internal(int epfd_idx, int op, int fd, struct epoll_event* event);
int  epoll_wait_internal(int epfd_idx, struct epoll_event* events,
                         int maxevents, uint64_t timeout_ticks);
int  net_ioctl(unsigned long request, void* argp);

// ============================================================================
// Sendfile (kernel-side)
// ============================================================================
int  sock_sendfile(int out_fd, int in_fd, int64_t* offset, size_t count);

// ============================================================================
// Routing Table
// ============================================================================
#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002
#define RTF_HOST    0x0004
#define RTF_REJECT  0x0200

void route_init(void);
int  route_add(uint32_t dst_net, uint32_t netmask, uint32_t gateway,
               net_device_t* dev, uint32_t metric, uint16_t flags);
int  route_del(uint32_t dst_net, uint32_t netmask, uint32_t gateway);
net_device_t* route_lookup(uint32_t dst_ip, uint32_t* next_hop_out);
net_device_t* net_get_loopback(void);
void loopback_process_pending(void);

// ============================================================================
// UNIX Domain Socket Address
// ============================================================================
#define UNIX_PATH_MAX 108

struct sockaddr_un {
    sa_family_t sun_family;         // AF_UNIX
    char sun_path[UNIX_PATH_MAX];   // Pathname
};

// UNIX domain socket (kernel internal)
typedef struct unix_socket {
    int active;
    int type;                       // SOCK_STREAM or SOCK_DGRAM
    int bound;
    int listening;
    int connected;
    int closed;
    int nonblock;
    int error;
    char path[UNIX_PATH_MAX];       // Bind path (or empty for abstract)
    struct unix_socket* peer;       // Connected peer
    struct unix_socket* parent;     // Listener (for accepted sockets)

    // Data buffer (bidirectional ring buffer)
    uint8_t buf[8192];
    int head;
    int tail;
    volatile int ready;             // Data available
    volatile int peer_closed;       // Peer has closed

    // Accept queue (for listening sockets)
    struct unix_socket* accept_queue[16];
    int accept_head;
    int accept_tail;
    int backlog;
    volatile int accept_ready;

    spinlock_t lock;
    int ref_count;
} unix_socket_t;

// UNIX domain socket API
int  unix_create(int type);
int  unix_bind(int usockfd, const struct sockaddr_un* addr);
int  unix_listen(int usockfd, int backlog);
int  unix_accept(int usockfd, struct sockaddr_un* addr, socklen_t* addrlen);
int  unix_connect(int usockfd, const struct sockaddr_un* addr);
int  unix_send(int usockfd, const void* buf, size_t len, int flags);
int  unix_recv(int usockfd, void* buf, size_t len, int flags);
int  unix_close(int usockfd);
int  unix_socketpair(int type, int sv[2]);
int  unix_shutdown(int usockfd, int how);
unix_socket_t* unix_get(int usockfd);
int  unix_poll(int usockfd, short events);

// ============================================================================
// DNS Resolver
// ============================================================================
int  dns_resolve(const char* hostname, uint32_t* ip_out);
int  dns_resolve_reverse(uint32_t ip_nbo, char* out, int maxlen);
int  dns_query_raw(const char* name, uint16_t qtype,
                   uint8_t* response, int max_len);
void dns_init(void);
void dns_rx(const uint8_t* data, uint16_t len);

#define DNS_CLIENT_PORT 5353

// ============================================================================
// Userspace-visible info structures (for SYS_NET_GETINFO)
// ============================================================================

// ARP table entry (userland view)
typedef struct net_arp_info {
    uint32_t ip;                // Host byte order
    uint8_t  mac[6];
    uint8_t  valid;
    uint8_t  pad;
} net_arp_info_t;

// Route table entry (userland view)
typedef struct net_route_info {
    uint32_t dst_net;           // Host byte order
    uint32_t netmask;           // Host byte order
    uint32_t gateway;           // Host byte order
    uint16_t flags;
    uint16_t metric;
    char     dev_name[16];
} net_route_info_t;

// TCP connection info (userland view)
typedef struct net_tcp_info {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int      state;
    uint32_t rx_queue;
    uint32_t tx_queue;
} net_tcp_info_t;

// UDP socket info (userland view)
typedef struct net_udp_info {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint32_t rx_queue;
} net_udp_info_t;

// Interface statistics (userland view)
typedef struct net_iface_info {
    char     name[16];
    uint8_t  mac[6];
    uint16_t mtu;
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    uint16_t flags;
    uint16_t pad;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
} net_iface_info_t;

// Raw DNS query buffer (shared with userspace)
typedef struct dns_query_buf {
    char     name[256];
    uint16_t qtype;
    uint16_t pad;
    int      response_len;
    uint8_t  response[512];
} dns_query_buf_t;

// ============================================================================
// Extended kernel API for userspace networking commands
// ============================================================================
int  net_get_arp_table(net_arp_info_t* entries, int max_entries);
int  net_get_route_table(net_route_info_t* entries, int max_entries);
int  net_get_tcp_connections(net_tcp_info_t* entries, int max_entries);
int  net_get_udp_sockets(net_udp_info_t* entries, int max_entries);
int  net_get_iface_info(net_iface_info_t* entries, int max_entries);
int  dhcp_release(net_device_t* dev);
int  dhcp_renew(net_device_t* dev);
int  dhcp_get_status(void);

// Raw ICMP send/receive for ping, traceroute, arping
int  icmp_send_echo(net_device_t* dev, uint32_t dst_ip, uint16_t id,
                    uint16_t seq, const uint8_t* data, uint16_t len, uint8_t ttl);
int  icmp_recv_reply(uint32_t* src_ip, uint16_t expected_id,
                     uint8_t* type_out, uint8_t* code_out,
                     uint16_t* seq_out, uint64_t timeout_ticks,
                     uint64_t* rtt_us_out, uint16_t expected_seq,
                     uint8_t* ttl_out);
int  arp_send_request(net_device_t* dev, uint32_t target_ip);
int  arp_recv_reply(uint32_t target_ip, uint8_t mac_out[6], uint64_t timeout_ticks);
void net_set_hostname(const char* name);
const char* net_get_hostname(void);

#endif // _KERNEL_NET_H_
