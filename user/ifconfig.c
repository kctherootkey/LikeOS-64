/*
 * ifconfig - configure network interfaces
 *
 * Display or modify the configuration of active network interfaces.
 * With no arguments, shows all active interfaces. An interface name
 * shows that interface only. Use -a to include inactive interfaces.
 *
 * Usage:
 *   ifconfig [-asv] [interface]
 *   ifconfig interface [up|down] [addr] [netmask NM] [gw GW] ...
 *
 * Options:
 *   -a            show all interfaces including inactive
 *   -s            short listing (one line per interface)
 *   -v            verbose output
 *
 * Interface parameters:
 *   up/down, [-]arp, [-]promisc, [-]allmulti, [-]multicast,
 *   mtu N, dstaddr, netmask, broadcast, pointopoint,
 *   hw class address, txqueuelen N, metric N
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static const char *ip_to_str(uint32_t ip)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    return inet_ntoa(a);
}

static void print_flags(uint16_t flags)
{
    printf("<");
    int first = 1;
    #define PF(f, s) if (flags & f) { if (!first) printf(","); printf(s); first = 0; }
    PF(IFF_UP, "UP");
    PF(IFF_BROADCAST, "BROADCAST");
    PF(IFF_DEBUG, "DEBUG");
    PF(IFF_LOOPBACK, "LOOPBACK");
    PF(IFF_POINTOPOINT, "POINTOPOINT");
    PF(IFF_NOTRAILERS, "NOTRAILERS");
    PF(IFF_RUNNING, "RUNNING");
    PF(IFF_NOARP, "NOARP");
    PF(IFF_PROMISC, "PROMISC");
    PF(IFF_ALLMULTI, "ALLMULTI");
    PF(IFF_MULTICAST, "MULTICAST");
    #undef PF
    printf(">");
}

/* Format byte count with units */
static void format_bytes(uint64_t bytes, char *buf, size_t bufsz)
{
    if (bytes >= 1073741824ULL)
        snprintf(buf, bufsz, "%lu.%lu GiB", (unsigned long)(bytes / 1073741824ULL),
                 (unsigned long)((bytes % 1073741824ULL) * 10 / 1073741824ULL));
    else if (bytes >= 1048576ULL)
        snprintf(buf, bufsz, "%lu.%lu MiB", (unsigned long)(bytes / 1048576ULL),
                 (unsigned long)((bytes % 1048576ULL) * 10 / 1048576ULL));
    else if (bytes >= 1024ULL)
        snprintf(buf, bufsz, "%lu.%lu KiB", (unsigned long)(bytes / 1024ULL),
                 (unsigned long)((bytes % 1024ULL) * 10 / 1024ULL));
    else
        snprintf(buf, bufsz, "%lu B", (unsigned long)bytes);
}

static void show_interface_full(const net_iface_info_t *iface, int verbose)
{
    (void)verbose;
    printf("%s: flags=%u ", iface->name, iface->flags);
    print_flags(iface->flags);
    printf("  mtu %u\n", (unsigned)iface->mtu);

    if (iface->ip_addr != 0 || (iface->flags & IFF_LOOPBACK)) {
        printf("        inet %s", ip_to_str(iface->ip_addr));
        printf("  netmask %s", ip_to_str(iface->netmask));
        if ((iface->flags & IFF_BROADCAST) && iface->netmask != 0) {
            uint32_t bcast = (iface->ip_addr & iface->netmask) | ~iface->netmask;
            printf("  broadcast %s", ip_to_str(bcast));
        }
        printf("\n");
    }

    if (!(iface->flags & IFF_LOOPBACK)) {
        printf("        ether %02x:%02x:%02x:%02x:%02x:%02x",
               iface->mac[0], iface->mac[1], iface->mac[2],
               iface->mac[3], iface->mac[4], iface->mac[5]);
        printf("  txqueuelen 1000  (Ethernet)\n");
    } else {
        printf("        txqueuelen 1000  (Local Loopback)\n");
    }

    char rxb[32], txb[32];
    format_bytes(iface->rx_bytes, rxb, sizeof(rxb));
    format_bytes(iface->tx_bytes, txb, sizeof(txb));

    printf("        RX packets %lu  bytes %lu (%s)\n",
           (unsigned long)iface->rx_packets, (unsigned long)iface->rx_bytes, rxb);
    printf("        RX errors %lu  dropped %lu  overruns 0  frame 0\n",
           (unsigned long)iface->rx_errors, (unsigned long)iface->rx_dropped);
    printf("        TX packets %lu  bytes %lu (%s)\n",
           (unsigned long)iface->tx_packets, (unsigned long)iface->tx_bytes, txb);
    printf("        TX errors %lu  dropped 0 overruns 0  carrier 0  collisions 0\n",
           (unsigned long)iface->tx_errors);
    printf("\n");
}

/* Short (-s) format - similar to netstat -i */
static void show_interface_short(const net_iface_info_t *iface)
{
    char flg[8];
    int fi = 0;
    if (iface->flags & IFF_RUNNING) flg[fi++] = 'R';
    if (iface->flags & IFF_UP) flg[fi++] = 'U';
    if (iface->flags & IFF_BROADCAST) flg[fi++] = 'B';
    if (iface->flags & IFF_LOOPBACK) flg[fi++] = 'L';
    if (iface->flags & IFF_MULTICAST) flg[fi++] = 'M';
    flg[fi] = '\0';

    printf("%-10s %-5u %-6lu %-6lu %-6lu %-6lu %s\n",
           iface->name, iface->mtu,
           (unsigned long)iface->rx_packets, 0UL,
           (unsigned long)iface->tx_packets, 0UL,
           flg);
}

static void show_all_interfaces(int show_all, int short_format, int verbose)
{
    net_iface_info_t ifaces[8];
    int n = net_getinfo(NET_GET_IFACE_INFO, ifaces, 8);

    if (short_format) {
        printf("Iface      MTU   RX-OK  RX-ERR TX-OK  TX-ERR Flg\n");
        for (int i = 0; i < n; i++) {
            if (!show_all && !(ifaces[i].flags & IFF_UP))
                continue;
            show_interface_short(&ifaces[i]);
        }
    } else {
        for (int i = 0; i < n; i++) {
            if (!show_all && !(ifaces[i].flags & IFF_UP))
                continue;
            show_interface_full(&ifaces[i], verbose);
        }
    }
}

static void show_one_interface(const char *name, int verbose)
{
    net_iface_info_t ifaces[8];
    int n = net_getinfo(NET_GET_IFACE_INFO, ifaces, 8);
    for (int i = 0; i < n; i++) {
        if (strcmp(ifaces[i].name, name) == 0) {
            show_interface_full(&ifaces[i], verbose);
            return;
        }
    }
    fprintf(stderr, "%s: error fetching interface information: Device not found\n", name);
}

static int set_addr(int fd, const char *ifname, unsigned long req, const char *addr_str)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = inet_addr(addr_str);

    return ioctl(fd, req, &ifr);
}

static int set_flags(int fd, const char *ifname, short flags, int set)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) return -1;

    if (set)
        ifr.ifr_flags |= flags;
    else
        ifr.ifr_flags &= ~flags;

    return ioctl(fd, SIOCSIFFLAGS, &ifr);
}

static int parse_hwaddr(const char *str, unsigned char *mac)
{
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        for (int i = 0; i < 6; i++) mac[i] = (unsigned char)m[i];
        return 0;
    }
    return -1;
}

static void print_help(void)
{
    printf("Usage:\n");
    printf("  ifconfig [-a] [-s] [-v] [interface]\n");
    printf("  ifconfig [-v] interface [aftype] options | address ...\n\n");
    printf("Options:\n");
    printf("  interface         interface name (e.g. eth0)\n");
    printf("  up                activate the interface\n");
    printf("  down              shut down the interface\n");
    printf("  [-]arp            enable/disable ARP protocol on this interface\n");
    printf("  [-]promisc        enable/disable promiscuous mode\n");
    printf("  [-]allmulti        enable/disable all-multicast mode\n");
    printf("  [-]multicast      enable/disable multicast flag\n");
    printf("  metric N          set the interface metric\n");
    printf("  mtu N             set the Maximum Transfer Unit\n");
    printf("  dstaddr addr      set the remote IP address for a point-to-point link\n");
    printf("  netmask addr      set the IP network mask for this interface\n");
    printf("  add addr/prefixlen   add an IPv6 address (not supported)\n");
    printf("  del addr/prefixlen   remove an IPv6 address (not supported)\n");
    printf("  tunnel aa.bb.cc.dd   create a new SIT device (not supported)\n");
    printf("  irq addr          set the interrupt line (not supported)\n");
    printf("  io_addr addr      set the I/O space base address (not supported)\n");
    printf("  mem_start addr    set shared memory start address (not supported)\n");
    printf("  media type        set the physical port or medium type (not supported)\n");
    printf("  [-]broadcast [addr]  set/clear the broadcast address\n");
    printf("  [-]pointopoint [addr] set/clear point-to-point mode\n");
    printf("  hw class address  set the hardware address (class: ether)\n");
    printf("  txqueuelen length set the transmit queue length\n");
    printf("  name newname      rename the interface (not supported)\n");
    printf("  address           set the IP address of this interface\n");
    printf("\n  -a                display all interfaces (even if down)\n");
    printf("  -s                display a short list (like netstat -i)\n");
    printf("  -v                be more verbose for some error conditions\n");
}

int main(int argc, char *argv[])
{
    int show_all = 0;
    int short_format = 0;
    int verbose = 0;
    int i = 1;

    /* Parse leading options (-a, -s, -v, -h, --help) */
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-a") == 0) { show_all = 1; i++; }
        else if (strcmp(argv[i], "-s") == 0) { short_format = 1; i++; }
        else if (strcmp(argv[i], "-v") == 0) { verbose = 1; i++; }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        else break; /* not our option */
    }

    /* No interface specified: show all */
    if (i >= argc) {
        show_all_interfaces(show_all || (argc == 1), short_format, verbose);
        return 0;
    }

    const char *ifname = argv[i++];

    /* Only interface name given: show it */
    if (i >= argc) {
        show_one_interface(ifname, verbose);
        return 0;
    }

    /* Configuration mode - open socket */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "ifconfig: socket: Cannot create socket\n");
        return 1;
    }

    while (i < argc) {
        /* up */
        if (strcmp(argv[i], "up") == 0) {
            set_flags(fd, ifname, IFF_UP | IFF_RUNNING, 1);
            i++;
        }
        /* down */
        else if (strcmp(argv[i], "down") == 0) {
            set_flags(fd, ifname, IFF_UP | IFF_RUNNING, 0);
            i++;
        }
        /* arp / -arp */
        else if (strcmp(argv[i], "arp") == 0) {
            set_flags(fd, ifname, IFF_NOARP, 0); /* clear NOARP = enable ARP */
            i++;
        }
        else if (strcmp(argv[i], "-arp") == 0) {
            set_flags(fd, ifname, IFF_NOARP, 1); /* set NOARP = disable ARP */
            i++;
        }
        /* promisc / -promisc */
        else if (strcmp(argv[i], "promisc") == 0) {
            set_flags(fd, ifname, IFF_PROMISC, 1);
            i++;
        }
        else if (strcmp(argv[i], "-promisc") == 0) {
            set_flags(fd, ifname, IFF_PROMISC, 0);
            i++;
        }
        /* allmulti / -allmulti */
        else if (strcmp(argv[i], "allmulti") == 0) {
            set_flags(fd, ifname, IFF_ALLMULTI, 1);
            i++;
        }
        else if (strcmp(argv[i], "-allmulti") == 0) {
            set_flags(fd, ifname, IFF_ALLMULTI, 0);
            i++;
        }
        /* multicast / -multicast */
        else if (strcmp(argv[i], "multicast") == 0) {
            set_flags(fd, ifname, IFF_MULTICAST, 1);
            i++;
        }
        else if (strcmp(argv[i], "-multicast") == 0) {
            set_flags(fd, ifname, IFF_MULTICAST, 0);
            i++;
        }
        /* metric N */
        else if (strcmp(argv[i], "metric") == 0 && i + 1 < argc) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            ifr.ifr_metric = atoi(argv[i + 1]);
            ioctl(fd, SIOCSIFMETRIC, &ifr);
            i += 2;
        }
        /* mtu N */
        else if (strcmp(argv[i], "mtu") == 0 && i + 1 < argc) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            ifr.ifr_mtu = atoi(argv[i + 1]);
            if (ioctl(fd, SIOCSIFMTU, &ifr) < 0)
                fprintf(stderr, "ifconfig: SIOCSIFMTU: Invalid argument\n");
            i += 2;
        }
        /* dstaddr addr */
        else if (strcmp(argv[i], "dstaddr") == 0 && i + 1 < argc) {
            set_addr(fd, ifname, SIOCSIFDSTADDR, argv[i + 1]);
            i += 2;
        }
        /* netmask addr */
        else if (strcmp(argv[i], "netmask") == 0 && i + 1 < argc) {
            set_addr(fd, ifname, SIOCSIFNETMASK, argv[i + 1]);
            i += 2;
        }
        /* broadcast [addr] / -broadcast */
        else if (strcmp(argv[i], "broadcast") == 0) {
            if (i + 1 < argc && argv[i+1][0] != '-' &&
                strcmp(argv[i+1], "up") != 0 && strcmp(argv[i+1], "down") != 0) {
                set_addr(fd, ifname, SIOCSIFBRDADDR, argv[i + 1]);
                set_flags(fd, ifname, IFF_BROADCAST, 1);
                i += 2;
            } else {
                set_flags(fd, ifname, IFF_BROADCAST, 1);
                i++;
            }
        }
        else if (strcmp(argv[i], "-broadcast") == 0) {
            set_flags(fd, ifname, IFF_BROADCAST, 0);
            i++;
        }
        /* pointopoint [addr] / -pointopoint */
        else if (strcmp(argv[i], "pointopoint") == 0) {
            if (i + 1 < argc && argv[i+1][0] != '-' &&
                strcmp(argv[i+1], "up") != 0 && strcmp(argv[i+1], "down") != 0) {
                set_addr(fd, ifname, SIOCSIFDSTADDR, argv[i + 1]);
                set_flags(fd, ifname, IFF_POINTOPOINT, 1);
                i += 2;
            } else {
                set_flags(fd, ifname, IFF_POINTOPOINT, 1);
                i++;
            }
        }
        else if (strcmp(argv[i], "-pointopoint") == 0) {
            set_flags(fd, ifname, IFF_POINTOPOINT, 0);
            i++;
        }
        /* hw class address */
        else if (strcmp(argv[i], "hw") == 0 && i + 2 < argc) {
            if (strcmp(argv[i+1], "ether") != 0) {
                fprintf(stderr, "ifconfig: hardware class '%s' not supported. Use 'ether'.\n", argv[i+1]);
                i += 3;
                continue;
            }
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            unsigned char mac[6];
            if (parse_hwaddr(argv[i + 2], mac) == 0) {
                ifr.ifr_hwaddr.sa_family = 1; /* ARPHRD_ETHER */
                for (int j = 0; j < 6; j++)
                    ifr.ifr_hwaddr.sa_data[j] = (char)mac[j];
                if (ioctl(fd, SIOCSIFHWADDR, &ifr) < 0)
                    fprintf(stderr, "ifconfig: SIOCSIFHWADDR: Operation not permitted\n");
            } else {
                fprintf(stderr, "ifconfig: invalid hardware address '%s'\n", argv[i+2]);
            }
            i += 3;
        }
        /* txqueuelen length */
        else if (strcmp(argv[i], "txqueuelen") == 0 && i + 1 < argc) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            /* Use a direct ioctl - ifr_metric union overlaps, use raw data */
            ifr.ifr_metric = atoi(argv[i + 1]); /* txqueuelen stored there */
            ioctl(fd, SIOCSIFTXQLEN, &ifr);
            i += 2;
        }
        /* add addr/prefixlen (IPv6 - not supported) */
        else if (strcmp(argv[i], "add") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: IPv6 address add not supported\n");
            i += 2;
        }
        /* del addr/prefixlen (IPv6 - not supported) */
        else if (strcmp(argv[i], "del") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: IPv6 address del not supported\n");
            i += 2;
        }
        /* tunnel (not supported) */
        else if (strcmp(argv[i], "tunnel") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: tunnel not supported\n");
            i += 2;
        }
        /* irq (not supported) */
        else if (strcmp(argv[i], "irq") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: irq not supported\n");
            i += 2;
        }
        /* io_addr (not supported) */
        else if (strcmp(argv[i], "io_addr") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: io_addr not supported\n");
            i += 2;
        }
        /* mem_start (not supported) */
        else if (strcmp(argv[i], "mem_start") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: mem_start not supported\n");
            i += 2;
        }
        /* media (not supported) */
        else if (strcmp(argv[i], "media") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: media type not supported\n");
            i += 2;
        }
        /* name newname (not supported) */
        else if (strcmp(argv[i], "name") == 0 && i + 1 < argc) {
            fprintf(stderr, "ifconfig: interface rename not supported\n");
            i += 2;
        }
        /* inet / inet6 address family prefixes (skip the keyword) */
        else if (strcmp(argv[i], "inet") == 0) {
            i++;
        }
        else if (strcmp(argv[i], "inet6") == 0) {
            fprintf(stderr, "ifconfig: IPv6 not supported\n");
            i++;
        }
        /* Anything else: assume it's an IP address */
        else {
            set_addr(fd, ifname, SIOCSIFADDR, argv[i]);
            i++;
        }
    }

    close(fd);
    return 0;
}
