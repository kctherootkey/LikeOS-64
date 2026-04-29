#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

// IP protocols
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_RAW     255

// Special addresses
#define INADDR_ANY       ((uint32_t)0x00000000)
#define INADDR_BROADCAST ((uint32_t)0xFFFFFFFF)
#define INADDR_LOOPBACK  ((uint32_t)0x7F000001)
#define INADDR_NONE      ((uint32_t)0xFFFFFFFF)

#define IN_MULTICAST(a)  (((uint32_t)(a) & 0xF0000000U) == 0xE0000000U)

// IP-level (SOL_IP / IPPROTO_IP) sockopts
#define IP_TOS                 1
#define IP_TTL                 2
#define IP_HDRINCL             3
#define IP_PKTINFO             8
#define IP_MULTICAST_IF        32
#define IP_MULTICAST_TTL       33
#define IP_MULTICAST_LOOP      34
#define IP_ADD_MEMBERSHIP      35
#define IP_DROP_MEMBERSHIP     36

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;    // Network byte order
};

struct sockaddr_in {
    sa_family_t sin_family;   // AF_INET
    in_port_t   sin_port;     // Port (network byte order)
    struct in_addr sin_addr;  // Address (network byte order)
    uint8_t     sin_zero[8];  // Padding
};

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

struct in_pktinfo {
    int            ipi_ifindex;
    struct in_addr ipi_spec_dst;
    struct in_addr ipi_addr;
};

// Byte order conversion
static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}

static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) |
           ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) |
           ((x << 24) & 0xFF000000);
}

static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

#endif /* _NETINET_IN_H */
