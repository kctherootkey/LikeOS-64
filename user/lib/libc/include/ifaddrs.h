#ifndef _IFADDRS_H
#define _IFADDRS_H

#include <sys/socket.h>

struct ifaddrs {
    struct ifaddrs  *ifa_next;
    char            *ifa_name;
    unsigned int     ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    /* One address slot serves two purposes, as it does everywhere else: a
     * broadcast address on a broadcast link, a peer address on a
     * point-to-point one.  The union gives both spellings — code picks the
     * one that matches the interface it is looking at, and the X server's
     * access control uses ifa_broadaddr. */
    union {
        struct sockaddr *ifu_broadaddr;
        struct sockaddr *ifu_dstaddr;
    } ifa_ifu;
#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr
    void            *ifa_data;
};

int  getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#endif /* _IFADDRS_H */
