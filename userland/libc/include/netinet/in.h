#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

// IP protocols
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

// Special addresses
#define INADDR_ANY       ((uint32_t)0x00000000)
#define INADDR_BROADCAST ((uint32_t)0xFFFFFFFF)
#define INADDR_LOOPBACK  ((uint32_t)0x7F000001)

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
