// getifaddrs / freeifaddrs — built atop NET_GET_IFACE_INFO.

#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>

#define IFI_MAX 8

void freeifaddrs(struct ifaddrs *ifa) {
    while (ifa) {
        struct ifaddrs *n = ifa->ifa_next;
        free(ifa->ifa_name);
        free(ifa->ifa_addr);
        free(ifa->ifa_netmask);
        free(ifa->ifa_dstaddr);
        free(ifa);
        ifa = n;
    }
}

static struct sockaddr* mk_in(uint32_t ip_nbo) {
    struct sockaddr_in *sa = malloc(sizeof(*sa));
    if (!sa) return 0;
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = ip_nbo;
    return (struct sockaddr*)sa;
}

int getifaddrs(struct ifaddrs **ifap) {
    if (!ifap) return -1;
    *ifap = 0;

    net_iface_info_t info[IFI_MAX];
    int n = net_getinfo(NET_GET_IFACE_INFO, info, IFI_MAX);
    if (n < 0) return -1;

    struct ifaddrs *head = 0, *tail = 0;
    for (int i = 0; i < n; i++) {
        struct ifaddrs *ifa = malloc(sizeof(*ifa));
        if (!ifa) { freeifaddrs(head); return -1; }
        memset(ifa, 0, sizeof(*ifa));

        size_t nl = strlen(info[i].name);
        ifa->ifa_name = malloc(nl + 1);
        if (ifa->ifa_name) memcpy(ifa->ifa_name, info[i].name, nl + 1);

        ifa->ifa_flags   = info[i].flags;
        ifa->ifa_addr    = mk_in(htonl(info[i].ip_addr));
        ifa->ifa_netmask = mk_in(htonl(info[i].netmask));
        // Broadcast = ip | ~netmask
        uint32_t bcast = info[i].ip_addr | ~info[i].netmask;
        ifa->ifa_dstaddr = mk_in(htonl(bcast));

        if (!head) head = ifa; else tail->ifa_next = ifa;
        tail = ifa;
    }
    *ifap = head;
    return 0;
}
