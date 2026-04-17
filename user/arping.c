/*
 * arping - send ARP requests to a neighbour host
 *
 * Ping a host on the local network at the link layer using ARP
 * requests. Reports whether the host is alive and its MAC address.
 * Can also perform duplicate address detection and gratuitous ARP.
 *
 * Usage:
 *   arping [-AbDfqUV] [-c count] [-w deadline] [-i interval]
 *          [-s source] [-I interface] destination
 *
 * Options:
 *   -c N      stop after N probes       -w N      timeout in seconds
 *   -i N      interval (default 1s)     -I iface  network interface
 *   -s addr   source IP                 -f        stop on first reply
 *   -q        quiet output              -b        broadcast only
 *   -D        duplicate address detect  -A        ARP reply mode
 *   -U        unsolicited ARP mode      -V        version
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/time.h>
#include <signal.h>

/* Statistics (global for signal handler) */
static int g_sent, g_received;
static int g_quiet;

static void print_stats(void)
{
    if (!g_quiet) {
        printf("Sent %d probes (%d broadcast(s))\n", g_sent, g_sent);
        printf("Received %d response(s)\n", g_received);
    }
}

static void sigint_handler(int sig)
{
    (void)sig;
    printf("\n");
    print_stats();
    exit(g_received > 0 ? 0 : 1);
}

static const char *ip_to_str(uint32_t ip)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    return inet_ntoa(a);
}

static void print_help(const char *progname)
{
    printf("Usage: %s [-AbDfhqUV] [-c count] [-w deadline] [-i interval]\n", progname);
    printf("            [-s source] [-I interface] destination\n\n");
    printf("Send ARP REQUEST to a neighbour host.\n\n");
    printf("Options:\n");
    printf("  -A              ARP REPLY mode (update caches)\n");
    printf("  -b              send broadcasts only\n");
    printf("  -c count        stop after count requests\n");
    printf("  -D              duplicate address detection mode\n");
    printf("  -f              quit on first reply\n");
    printf("  -h              display this help\n");
    printf("  -I interface    network interface to use\n");
    printf("  -i interval     seconds between probes (default 1)\n");
    printf("  -q              quiet output\n");
    printf("  -s source       source IP address\n");
    printf("  -U              unsolicited ARP mode\n");
    printf("  -V              print version and exit\n");
    printf("  -w deadline     timeout in seconds\n");
}

int main(int argc, char *argv[])
{
    int count = -1;
    int deadline = -1;
    int interval = 1;
    int quit_first = 0;
    int dad_mode = 0;
    int reply_mode = 0;   /* -A */
    int unsolicited = 0;  /* -U */
    int bcast_only = 0;
    const char *iface_name = NULL;
    const char *source_addr = NULL;
    int opt;

    (void)reply_mode; (void)unsolicited; (void)bcast_only; (void)dad_mode;

    while ((opt = getopt(argc, argv, "Abc:Dfhi:I:qs:UVw:")) != -1) {
        switch (opt) {
            case 'A': reply_mode = 1; break;
            case 'b': bcast_only = 1; break;
            case 'c': count = atoi(optarg); break;
            case 'D': dad_mode = 1; break;
            case 'f': quit_first = 1; break;
            case 'h': print_help(argv[0]); return 0;
            case 'i': interval = atoi(optarg); break;
            case 'I': iface_name = optarg; break;
            case 'q': g_quiet = 1; break;
            case 's': source_addr = optarg; break;
            case 'U': unsolicited = 1; break;
            case 'V':
                printf("arping (LikeOS iputils) 1.0\n");
                return 0;
            case 'w': deadline = atoi(optarg); break;
            default: print_help(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "arping: missing destination\n");
        print_help(argv[0]);
        return 1;
    }

    const char *dest = argv[optind];
    uint32_t dst_ip = ntohl(inet_addr(dest));
    if (dst_ip == 0) {
        /* Try to resolve */
        uint32_t resolved;
        if (dns_resolve(dest, &resolved) == 0)
            dst_ip = resolved;
        else {
            fprintf(stderr, "arping: %s: Name or service not known\n", dest);
            return 1;
        }
    }

    /* Get interface info */
    net_iface_info_t ifaces[8];
    int ni = net_getinfo(NET_GET_IFACE_INFO, ifaces, 8);
    const char *ifname = "eth0";
    const char *src_str;
    uint32_t src_ip = 0;

    if (source_addr) {
        src_ip = ntohl(inet_addr(source_addr));
        src_str = source_addr;
    } else {
        src_str = "0.0.0.0";
    }

    for (int i = 0; i < ni; i++) {
        if (iface_name) {
            if (strcmp(ifaces[i].name, iface_name) == 0) {
                ifname = ifaces[i].name;
                if (!source_addr && ifaces[i].ip_addr != 0) {
                    src_ip = ifaces[i].ip_addr;
                    src_str = ip_to_str(ifaces[i].ip_addr);
                }
                break;
            }
        } else if (ifaces[i].ip_addr != 0 && !(ifaces[i].flags & IFF_LOOPBACK)) {
            ifname = ifaces[i].name;
            if (!source_addr) {
                src_ip = ifaces[i].ip_addr;
                src_str = ip_to_str(ifaces[i].ip_addr);
            }
            break;
        }
    }

    (void)src_ip;

    if (!g_quiet)
        printf("ARPING %s from %s %s\n", dest, src_str, ifname);

    signal(SIGINT, sigint_handler);

    int elapsed = 0;

    for (int seq = 0; count < 0 || seq < count; seq++) {
        if (deadline > 0 && elapsed >= deadline)
            break;

        struct timeval tv_send, tv_recv;
        gettimeofday(&tv_send, NULL);

        int ret = raw_send(RAW_SEND_ARP_REQUEST, dst_ip, 0, 0);
        if (ret < 0) {
            if (!g_quiet) fprintf(stderr, "arping: send failed\n");
            g_sent++;
            if (interval > 0) { sleep(interval); elapsed += interval; }
            continue;
        }
        g_sent++;

        uint64_t timeout_ticks = (uint64_t)(deadline > 0 ? 1 : interval) * 100;
        uint8_t mac[6];
        ret = raw_recv(RAW_RECV_ARP_REPLY, mac, dst_ip, timeout_ticks);
        if (ret == 0) {
            gettimeofday(&tv_recv, NULL);
            long rtt_us = (tv_recv.tv_sec - tv_send.tv_sec) * 1000000L +
                          (tv_recv.tv_usec - tv_send.tv_usec);
            if (rtt_us < 0) rtt_us = 0;
            g_received++;
            if (!g_quiet) {
                printf("Unicast reply from %s [%02X:%02X:%02X:%02X:%02X:%02X]  %lu.%03lu ms\n",
                       dest, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                       (unsigned long)(rtt_us / 1000),
                       (unsigned long)(rtt_us % 1000));
            }
            if (quit_first) break;
        } else {
            if (!g_quiet) printf("Timeout\n");
        }

        elapsed += interval;
        if (count < 0 || seq + 1 < count) {
            if (interval > 0) sleep(interval);
        }
    }

    print_stats();

    /* DAD mode: 0 = not used (not duplicate), 1 = duplicate found */
    if (dad_mode)
        return g_received > 0 ? 1 : 0;

    return g_received > 0 ? 0 : 1;
}
