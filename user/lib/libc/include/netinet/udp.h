/*
 * Copyright (C) 2026 The LikeOS Project
 *
 * The UDP header and the UDP-level socket options (setsockopt level
 * IPPROTO_UDP / SOL_UDP).
 */

#ifndef _NETINET_UDP_H
#define _NETINET_UDP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct udphdr {
    uint16_t uh_sport; /* source port */
    uint16_t uh_dport; /* destination port */
    uint16_t uh_ulen;  /* length of header and data */
    uint16_t uh_sum;   /* checksum, 0: none */
};

#ifndef SOL_UDP
#define SOL_UDP 17
#endif

/* Collect the data of several sends into one datagram, sent when the
 * option is cleared again. */
#define UDP_CORK 1

#ifdef __cplusplus
}
#endif

#endif /* _NETINET_UDP_H */
