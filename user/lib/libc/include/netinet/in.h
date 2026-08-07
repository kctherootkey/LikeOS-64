#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#ifdef __cplusplus
extern "C" {
#endif

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

/* Classful address predicates and the loopback network number (127.x). */
#define IN_CLASSA(a)      ((((uint32_t)(a)) & 0x80000000U) == 0)
#define IN_CLASSA_NET     0xff000000U
#define IN_CLASSA_NSHIFT  24
#define IN_CLASSB(a)      ((((uint32_t)(a)) & 0xc0000000U) == 0x80000000U)
#define IN_CLASSB_NET     0xffff0000U
#define IN_CLASSB_NSHIFT  16
#define IN_CLASSC(a)      ((((uint32_t)(a)) & 0xe0000000U) == 0xc0000000U)
#define IN_CLASSC_NET     0xffffff00U
#define IN_CLASSC_NSHIFT  8
#define IN_CLASSD(a)      ((((uint32_t)(a)) & 0xf0000000U) == 0xe0000000U)
#define IN_EXPERIMENTAL(a) ((((uint32_t)(a)) & 0xe0000000U) == 0xe0000000U)
#define IN_LOOPBACKNET    127

/* Well-known TCP/UDP port ranges (standard <netinet/in.h> constants). */
#define IPPORT_ECHO          7
#define IPPORT_DISCARD       9
#define IPPORT_SYSTAT        11
#define IPPORT_DAYTIME       13
#define IPPORT_NETSTAT       15
#define IPPORT_FTP           21
#define IPPORT_TELNET        23
#define IPPORT_SMTP          25
#define IPPORT_TIMESERVER    37
#define IPPORT_NAMESERVER    42
#define IPPORT_WHOIS         43
#define IPPORT_HTTP          80
#define IPPORT_RESERVED      1024 /* ports < 1024 are privileged */
#define IPPORT_USERRESERVED  5000 /* dynamic/private ports start above here */

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

/*
 * IPv6 - the kernel does not implement IPv6 yet, but the structure
 * definitions are required so that portable applications (libevent,
 * tmux, etc.) compile.  Routines that try to use AF_INET6 will fail at
 * the socket(2) layer with EAFNOSUPPORT.
 */
#define IPPROTO_IPV6        41
#define IPPROTO_ICMPV6      58

/*
 * IPv6 socket options (RFC 3493 §5.3, RFC 3542 §4-§6).
 *
 * The numbers are the conventional ones, so a program that hardcodes a value
 * -- and some do -- agrees with a program that uses the name.
 *
 * Defining them all is not a claim that the stack implements them: it does not
 * implement IPv6 at all, and setsockopt(2) reports that.  But a constant that
 * is merely absent stops a build outright, where one the kernel rejects
 * produces the error the caller is already written to handle -- GLib sets
 * IPV6_TCLASS to mark packet priority and treats a failure as "this system
 * does not offer it", which is exactly right here.
 */
#define IPV6_ADDRFORM       1
#define IPV6_2292PKTINFO    2
#define IPV6_2292HOPOPTS    3
#define IPV6_2292DSTOPTS    4
#define IPV6_2292RTHDR      5
#define IPV6_2292PKTOPTIONS 6
#define IPV6_CHECKSUM       7
#define IPV6_2292HOPLIMIT   8
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21
#define IPV6_ROUTER_ALERT   22
#define IPV6_MTU_DISCOVER   23
#define IPV6_MTU            24
#define IPV6_RECVERR        25
#define IPV6_V6ONLY         26
#define IPV6_JOIN_ANYCAST   27
#define IPV6_LEAVE_ANYCAST  28
#define IPV6_RECVPKTINFO    49
#define IPV6_PKTINFO        50
#define IPV6_RECVHOPLIMIT   51
#define IPV6_HOPLIMIT       52
#define IPV6_RECVHOPOPTS    53
#define IPV6_HOPOPTS        54
#define IPV6_RTHDRDSTOPTS   55
#define IPV6_RECVRTHDR      56
#define IPV6_RTHDR          57
#define IPV6_RECVDSTOPTS    58
#define IPV6_DSTOPTS        59
#define IPV6_RECVPATHMTU    60
#define IPV6_PATHMTU        61
#define IPV6_DONTFRAG       62
#define IPV6_RECVTCLASS     66
#define IPV6_TCLASS         67

/* The multicast join/leave structure names RFC 3493 gives these options. */
#define IPV6_ADD_MEMBERSHIP  IPV6_JOIN_GROUP
#define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP

/* Values for IPV6_MTU_DISCOVER. */
#define IPV6_PMTUDISC_DONT   0
#define IPV6_PMTUDISC_WANT   1
#define IPV6_PMTUDISC_DO     2
#define IPV6_PMTUDISC_PROBE  3

struct in6_addr {
    union {
        uint8_t  __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;    /* AF_INET6 */
    in_port_t       sin6_port;      /* Transport layer port */
    uint32_t        sin6_flowinfo;  /* IPv6 flow information */
    struct in6_addr sin6_addr;      /* IPv6 address */
    uint32_t        sin6_scope_id;  /* Scope ID (interface index) */
};

struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

/*
 * The ancillary data IPV6_PKTINFO and IPV6_RECVPKTINFO carry (RFC 3542 §6.6):
 * which address a datagram arrived on, and through which interface.
 *
 * Present because the OPTION NAMES above are.  Software tests for the constant
 * and takes the structure's existence as implied -- OpenSSL's DTLS code does
 * exactly that, and adding IPV6_PKTINFO without this broke a port that had
 * been building for months:
 *
 *     error: invalid application of 'sizeof' to incomplete type
 *            'struct in6_pktinfo'
 *
 * which names neither the constant nor the header that gained it.  A socket
 * option and the data it transfers are one interface; declaring half of it
 * tells a caller something untrue.
 */
struct in6_pktinfo {
    struct in6_addr ipi6_addr;    /* Source or destination address */
    unsigned int    ipi6_ifindex; /* Interface index */
};

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

#define IN6ADDR_ANY_INIT      { { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } } }
#define IN6ADDR_LOOPBACK_INIT { { { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 } } }

/* RFC 2553 address-test macros.  Operate on a (struct in6_addr *). */
#define IN6_IS_ADDR_UNSPECIFIED(a) \
	(((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && \
	 ((const uint32_t *)(a))[2] == 0 && ((const uint32_t *)(a))[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
	(((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && \
	 ((const uint32_t *)(a))[2] == 0 && \
	 ((const uint32_t *)(a))[3] == __builtin_bswap32(1))
#define IN6_IS_ADDR_MULTICAST(a)  (((const uint8_t *)(a))[0] == 0xff)
#define IN6_IS_ADDR_LINKLOCAL(a) \
	((((const uint8_t *)(a))[0] == 0xfe) && \
	 ((((const uint8_t *)(a))[1] & 0xc0) == 0x80))
#define IN6_IS_ADDR_SITELOCAL(a) \
	((((const uint8_t *)(a))[0] == 0xfe) && \
	 ((((const uint8_t *)(a))[1] & 0xc0) == 0xc0))
#define IN6_IS_ADDR_V4MAPPED(a) \
	(((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && \
	 ((const uint32_t *)(a))[2] == __builtin_bswap32(0x0000ffff))
#define IN6_IS_ADDR_V4COMPAT(a) \
	(((const uint32_t *)(a))[0] == 0 && ((const uint32_t *)(a))[1] == 0 && \
	 ((const uint32_t *)(a))[2] == 0 && \
	 __builtin_bswap32(((const uint32_t *)(a))[3]) > 1)
#define IN6_IS_ADDR_MC_NODELOCAL(a) \
	(IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x1))
#define IN6_IS_ADDR_MC_LINKLOCAL(a) \
	(IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x2))
#define IN6_IS_ADDR_MC_SITELOCAL(a) \
	(IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x5))
#define IN6_IS_ADDR_MC_ORGLOCAL(a) \
	(IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0x8))
#define IN6_IS_ADDR_MC_GLOBAL(a) \
	(IN6_IS_ADDR_MULTICAST(a) && ((((const uint8_t *)(a))[1] & 0xf) == 0xe))
#define IN6_ARE_ADDR_EQUAL(a, b) \
	(((const uint32_t *)(a))[0] == ((const uint32_t *)(b))[0] && \
	 ((const uint32_t *)(a))[1] == ((const uint32_t *)(b))[1] && \
	 ((const uint32_t *)(a))[2] == ((const uint32_t *)(b))[2] && \
	 ((const uint32_t *)(a))[3] == ((const uint32_t *)(b))[3])

// Byte order conversion
static __inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

static __inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}

static __inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) |
           ((x >> 8) & 0xFF00) |
           ((x << 8) & 0xFF0000) |
           ((x << 24) & 0xFF000000);
}

static __inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

#ifdef __cplusplus
}
#endif

#endif /* _NETINET_IN_H */
