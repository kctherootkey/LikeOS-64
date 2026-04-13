#ifndef _NET_IF_H
#define _NET_IF_H

#include <stdint.h>

#define IFNAMSIZ 16

// Interface flags
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

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct { uint16_t sa_family; char sa_data[14]; } ifr_addr;
        struct { uint16_t sa_family; char sa_data[14]; } ifr_dstaddr;
        struct { uint16_t sa_family; char sa_data[14]; } ifr_broadaddr;
        struct { uint16_t sa_family; char sa_data[14]; } ifr_netmask;
        struct { uint16_t sa_family; char sa_data[14]; } ifr_hwaddr;
        short  ifr_flags;
        int    ifr_ifindex;
        int    ifr_metric;
        int    ifr_mtu;
    };
};

struct ifconf {
    int   ifc_len;
    union {
        char*         ifc_buf;
        struct ifreq* ifc_req;
    };
};

#endif /* _NET_IF_H */
