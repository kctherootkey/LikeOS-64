#ifndef _NET_ROUTE_H
#define _NET_ROUTE_H

#include <sys/socket.h>

/* Route flags */
#define RTF_UP          0x0001
#define RTF_GATEWAY     0x0002
#define RTF_HOST        0x0004
#define RTF_REINSTATE   0x0008
#define RTF_DYNAMIC     0x0010
#define RTF_MODIFIED    0x0020
#define RTF_DEFAULT     0x8000

struct rtentry {
    struct sockaddr rt_dst;
    struct sockaddr rt_gateway;
    struct sockaddr rt_genmask;
    short           rt_flags;
    int             rt_metric;
    char*           rt_dev;
};

/* ARP flags */
#define ATF_COM     0x02
#define ATF_PERM    0x04

#define IFNAMSIZ 16

struct arpreq {
    struct sockaddr arp_pa;
    struct sockaddr arp_ha;
    int             arp_flags;
    struct sockaddr arp_netmask;
    char            arp_dev[IFNAMSIZ];
};

#endif /* _NET_ROUTE_H */
