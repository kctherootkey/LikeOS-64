/*
 * route - show and manipulate the IP routing table
 *
 * Display, add, or delete entries in the kernel IP routing table.
 * Without arguments, shows the current routing table.
 *
 * Usage:
 *   route [-nNvee] [-FC]
 *   route add [-net|-host] target [gw GW] [netmask NM] [metric N] [dev IF]
 *   route del [-net|-host] target [gw GW] [netmask NM] [dev IF]
 *
 * Options:
 *   -n   numeric addresses    -N   resolve hardware names
 *   -v   verbose              -e   netstat-format output
 *   -F   FIB (default)        -C   routing cache
 *   -4   IPv4 (default)
 *
 * Route parameters: metric, mss, window, irtt, reject, mod, dyn, reinstate
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static const char *ip_to_str(uint32_t ip)
{
    static char bufs[4][16];
    static int idx = 0;
    char *buf = bufs[idx & 3];
    idx++;
    snprintf(buf, 16, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

static void show_route_table(int numeric, int extended, int verbose)
{
    (void)verbose;
    net_route_info_t routes[32];
    int n = net_getinfo(NET_GET_ROUTE_TABLE, routes, 32);

    printf("Kernel IP routing table\n");

    if (extended >= 2) {
        /* -ee format: Destination, Gateway, Genmask, Flags, MSS, Window, irtt, Iface */
        printf("%-16s %-16s %-16s %-6s %-6s %-3s %-3s %-5s %-6s %-5s %s\n",
               "Destination", "Gateway", "Genmask", "Flags", "Metric", "Ref", "Use",
               "MSS", "Window", "irtt", "Iface");
    } else if (extended) {
        /* -e format */
        printf("%-16s %-16s %-16s %-6s %-6s %-3s %-3s %s\n",
               "Destination", "Gateway", "Genmask", "Flags", "Metric", "Ref", "Use", "Iface");
    } else {
        /* Default format */
        printf("%-16s %-16s %-16s %-6s Metric Ref    Use Iface\n",
               "Destination", "Gateway", "Genmask", "Flags");
    }

    for (int i = 0; i < n; i++) {
        char flags[16] = "U";
        int fi = 1;
        if (routes[i].flags & RTF_GATEWAY)   flags[fi++] = 'G';
        if (routes[i].flags & RTF_HOST)      flags[fi++] = 'H';
        if (routes[i].flags & RTF_REINSTATE) flags[fi++] = 'R';
        if (routes[i].flags & RTF_DYNAMIC)   flags[fi++] = 'D';
        if (routes[i].flags & RTF_MODIFIED)  flags[fi++] = 'M';
        if (routes[i].dst_net == 0 && routes[i].netmask == 0) {
            /* nothing special for default route flag */
        }
        flags[fi] = '\0';

        const char *dest_str;
        if (routes[i].dst_net == 0 && !numeric)
            dest_str = "default";
        else
            dest_str = ip_to_str(routes[i].dst_net);

        const char *gw_str;
        if (routes[i].gateway == 0)
            gw_str = "*";
        else
            gw_str = ip_to_str(routes[i].gateway);

        if (extended >= 2) {
            printf("%-16s %-16s %-16s %-6s %-6d %-3d %-3d %-5d %-6d %-5d %s\n",
                   dest_str, gw_str, ip_to_str(routes[i].netmask),
                   flags, routes[i].metric, 0, 0, 0, 0, 0, routes[i].dev_name);
        } else if (extended) {
            printf("%-16s %-16s %-16s %-6s %-6d %-3d %-3d %s\n",
                   dest_str, gw_str, ip_to_str(routes[i].netmask),
                   flags, routes[i].metric, 0, 0, routes[i].dev_name);
        } else {
            printf("%-16s %-16s %-16s %-6s %-6d %-6d %3d %s\n",
                   dest_str, gw_str, ip_to_str(routes[i].netmask),
                   flags, routes[i].metric, 0, 0, routes[i].dev_name);
        }
    }
}

static void print_help(void)
{
    printf("Usage: route [-nNvee] [-FC] [family]\n");
    printf("       route [-v] [-FC] {add|del} [-net|-host] target\n");
    printf("             [netmask NM] [gw GW] [metric N] [mss M]\n");
    printf("             [window W] [irtt I] [reject] [mod] [dyn]\n");
    printf("             [reinstate] [[dev] If]\n\n");
    printf("Options:\n");
    printf("  -A family    use the specified address family\n");
    printf("  -F           operate on the kernel's FIB (default)\n");
    printf("  -C           operate on the kernel's routing cache\n");
    printf("  -v           be verbose\n");
    printf("  -n           don't resolve names\n");
    printf("  -e           use netstat-format output\n");
    printf("  -N           resolve hardware names\n");
    printf("  -4           IPv4 (default)\n");
    printf("  -6           IPv6 (not supported)\n");
    printf("  add          add a new route\n");
    printf("  del          delete a route\n");
    printf("  -net         the target is a network\n");
    printf("  -host        the target is a host\n");
    printf("  netmask NM   specify the netmask of the route\n");
    printf("  gw GW        route packets via a gateway\n");
    printf("  metric N     set the metric for the route\n");
    printf("  mss M        set TCP Maximum Segment Size\n");
    printf("  window W     set TCP window size\n");
    printf("  irtt I       set initial round trip time\n");
    printf("  reject       install a blocking route\n");
    printf("  mod          modify a route entry\n");
    printf("  dyn          reinstall a dynamic route\n");
    printf("  reinstate    reinstall a deleted route\n");
    printf("  dev If       force the route to this device\n");
    printf("  -V, --version  display version\n");
    printf("  -h, --help     display this help\n");
}

int main(int argc, char *argv[])
{
    int numeric = 0;
    int extended = 0;
    int verbose = 0;
    int i = 1;

    /* Parse leading options */
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-n") == 0)        { numeric = 1; i++; }
        else if (strcmp(argv[i], "-N") == 0)   { i++; /* resolve HW names - accepted */ }
        else if (strcmp(argv[i], "-v") == 0)   { verbose = 1; i++; }
        else if (strcmp(argv[i], "-e") == 0)   { extended++; i++; }
        else if (strcmp(argv[i], "-ee") == 0)  { extended = 2; i++; }
        else if (strcmp(argv[i], "-F") == 0)   { i++; /* FIB - default */ }
        else if (strcmp(argv[i], "-C") == 0)   { i++; /* route cache - same */ }
        else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            if (strcmp(argv[i+1], "inet") != 0 && strcmp(argv[i+1], "inet4") != 0) {
                fprintf(stderr, "route: address family '%s' not supported\n", argv[i+1]);
                return 1;
            }
            i += 2;
        }
        else if (strcmp(argv[i], "-4") == 0)   { i++; /* IPv4 - default */ }
        else if (strcmp(argv[i], "-6") == 0)   {
            fprintf(stderr, "route: IPv6 not supported\n");
            return 1;
        }
        else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("route (LikeOS net-tools) 1.0\n");
            return 0;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        else break;
    }

    /* No add/del -> show table */
    if (i >= argc) {
        show_route_table(numeric, extended, verbose);
        return 0;
    }

    int adding = 0, deleting = 0;
    if (strcmp(argv[i], "add") == 0) { adding = 1; i++; }
    else if (strcmp(argv[i], "del") == 0 || strcmp(argv[i], "delete") == 0) { deleting = 1; i++; }
    else {
        /* Not add/del, could be an unknown command or just show table */
        show_route_table(numeric, extended, verbose);
        return 0;
    }

    /* Parse -net / -host */
    int is_host = 0;
    if (i < argc && strcmp(argv[i], "-net") == 0) { i++; }
    else if (i < argc && strcmp(argv[i], "-host") == 0) { is_host = 1; i++; }

    if (i >= argc) {
        fprintf(stderr, "route: need a target network or host\n");
        return 1;
    }

    /* Target */
    const char *target = argv[i++];
    const char *netmask_str = NULL;
    const char *gw_str = NULL;
    const char *dev = NULL;
    int metric = 0;
    int mss = 0;
    int window = 0;
    int irtt = 0;
    int reject = 0;
    int route_mod = 0;
    int route_dyn = 0;
    int route_reinstate = 0;

    while (i < argc) {
        if (strcmp(argv[i], "netmask") == 0 && i + 1 < argc) {
            netmask_str = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "gw") == 0 && i + 1 < argc) {
            gw_str = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "metric") == 0 && i + 1 < argc) {
            metric = atoi(argv[i + 1]); i += 2;
        } else if (strcmp(argv[i], "mss") == 0 && i + 1 < argc) {
            mss = atoi(argv[i + 1]); i += 2;
        } else if (strcmp(argv[i], "window") == 0 && i + 1 < argc) {
            window = atoi(argv[i + 1]); i += 2;
        } else if (strcmp(argv[i], "irtt") == 0 && i + 1 < argc) {
            irtt = atoi(argv[i + 1]); i += 2;
        } else if (strcmp(argv[i], "reject") == 0) {
            reject = 1; i++;
        } else if (strcmp(argv[i], "mod") == 0) {
            route_mod = 1; i++;
        } else if (strcmp(argv[i], "dyn") == 0) {
            route_dyn = 1; i++;
        } else if (strcmp(argv[i], "reinstate") == 0) {
            route_reinstate = 1; i++;
        } else if (strcmp(argv[i], "dev") == 0 && i + 1 < argc) {
            dev = argv[i + 1]; i += 2;
        } else {
            /* Might be device name without "dev" prefix */
            dev = argv[i]; i++;
        }
    }

    (void)mss; (void)window; (void)irtt;
    (void)route_mod; (void)route_dyn;

    /* Build rtentry */
    struct rtentry rt;
    memset(&rt, 0, sizeof(rt));

    struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
    dst->sin_family = AF_INET;
    if (strcmp(target, "default") == 0)
        dst->sin_addr.s_addr = 0;
    else
        dst->sin_addr.s_addr = inet_addr(target);

    if (gw_str) {
        struct sockaddr_in *gw = (struct sockaddr_in *)&rt.rt_gateway;
        gw->sin_family = AF_INET;
        gw->sin_addr.s_addr = inet_addr(gw_str);
        rt.rt_flags |= RTF_GATEWAY;
    }

    struct sockaddr_in *mask = (struct sockaddr_in *)&rt.rt_genmask;
    mask->sin_family = AF_INET;
    if (netmask_str)
        mask->sin_addr.s_addr = inet_addr(netmask_str);
    else if (is_host)
        mask->sin_addr.s_addr = inet_addr("255.255.255.255");
    else if (strcmp(target, "default") == 0)
        mask->sin_addr.s_addr = 0;
    else
        mask->sin_addr.s_addr = inet_addr("255.255.255.0");

    rt.rt_flags |= RTF_UP;
    if (is_host) rt.rt_flags |= RTF_HOST;
    if (reject) rt.rt_flags |= 0x0200; /* RTF_REJECT */
    if (route_reinstate) rt.rt_flags |= RTF_REINSTATE;
    rt.rt_metric = metric;
    rt.rt_dev = dev ? (char *)dev : NULL;

    if (verbose) {
        printf("route: %s %s via %s dev %s metric %d\n",
               adding ? "add" : "del", target,
               gw_str ? gw_str : "*",
               dev ? dev : "(auto)", metric);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "route: socket: Cannot create socket\n");
        return 1;
    }

    int ret;
    if (adding)
        ret = ioctl(fd, SIOCADDRT, &rt);
    else
        ret = ioctl(fd, SIOCDELRT, &rt);

    close(fd);

    if (ret < 0) {
        fprintf(stderr, "route: %s: %s\n",
                adding ? "SIOCADDRT" : "SIOCDELRT",
                "Operation failed");
        return 1;
    }

    return 0;
}
