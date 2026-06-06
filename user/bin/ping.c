/*
 * ping - send ICMP ECHO_REQUEST to network hosts
 *
 * Send ICMP ECHO_REQUEST packets to a host and report round-trip
 * times and packet loss statistics. Useful for testing network
 * connectivity and measuring latency.
 *
 * Usage:
 *   ping [-aAbBdDfLnOqrRUvV] [-c count] [-i interval] [-I iface]
 *        [-l preload] [-m mark] [-M hint] [-p pattern] [-Q tos]
 *        [-s size] [-S sndbuf] [-t ttl] [-w deadline] [-W timeout]
 *        destination
 *
 * Options:
 *   -c N      stop after N packets     -i N      interval (seconds)
 *   -s N      packet size (default 56)  -t N      TTL
 *   -w N      deadline (seconds)        -W N      response timeout
 *   -I iface  source interface          -n        numeric output
 *   -q        quiet                     -v        verbose
 *   -f        flood ping                -a        audible
 *   -A        adaptive interval         -D        timestamps
 *   -R        record route              -r        bypass routing
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <signal.h>

/* Statistics (global for signal handler) */
static const char *g_dest;
static int g_sent, g_received, g_errors;
static uint32_t g_min_ms = 0xFFFFFFFF, g_max_ms, g_total_ms;

static void print_stats(void)
{
    int loss = g_sent > 0 ? ((g_sent - g_received) * 100 / g_sent) : 0;
    printf("\n--- %s ping statistics ---\n", g_dest);
    printf("%d packets transmitted, %d received", g_sent, g_received);
    if (g_errors > 0)
        printf(", +%d errors", g_errors);
    printf(", %d%% packet loss\n", loss);

    if (g_received > 0) {
        uint32_t avg_ms = g_total_ms / (uint32_t)g_received;
        uint32_t mdev = (g_max_ms - g_min_ms) / 2;
        printf("rtt min/avg/max/mdev = %u.%03u/%u.%03u/%u.%03u/%u.%03u ms\n",
               g_min_ms / 1000, g_min_ms % 1000,
               avg_ms / 1000, avg_ms % 1000,
               g_max_ms / 1000, g_max_ms % 1000,
               mdev / 1000, mdev % 1000);
    }
}

static void sigint_handler(int sig)
{
    (void)sig;
    print_stats();
    exit(g_received > 0 ? 0 : 1);
}

static void print_version(void)
{
    printf("ping (LikeOS iputils) 1.0\n");
}

static void print_help(const char *progname)
{
    printf("Usage: %s [OPTIONS] destination\n\n", progname);
    printf("Options:\n");
    printf("  -4                use IPv4 (default)\n");
    printf("  -6                use IPv6 (not supported)\n");
    printf("  -a                audible ping\n");
    printf("  -A                adaptive ping\n");
    printf("  -b                allow pinging broadcast address\n");
    printf("  -B                sticky source address\n");
    printf("  -c count          stop after count packets\n");
    printf("  -d                use SO_DEBUG socket option\n");
    printf("  -D                print timestamps\n");
    printf("  -f                flood ping\n");
    printf("  -h                print help and exit\n");
    printf("  -i interval       seconds between packets (default 1)\n");
    printf("  -I interface      source address or interface name\n");
    printf("  -l preload        send preload number of packets as fast as possible\n");
    printf("  -L                suppress loopback of multicast packets\n");
    printf("  -m mark           tag the packets going out\n");
    printf("  -M pmtudisc_opt   path MTU discovery (do|want|dont)\n");
    printf("  -n                no dns name resolution\n");
    printf("  -O                report outstanding replies\n");
    printf("  -p pattern        pattern to fill out packet\n");
    printf("  -q                quiet output\n");
    printf("  -Q tos            set quality of service related bits\n");
    printf("  -r                bypass normal routing tables\n");
    printf("  -R                record route\n");
    printf("  -s packetsize     number of data bytes to be sent (default 56)\n");
    printf("  -S sndbuf         set socket sndbuf\n");
    printf("  -t ttl            set the IP Time to Live\n");
    printf("  -U                print user-to-user latency\n");
    printf("  -v                verbose output\n");
    printf("  -V                print version and exit\n");
    printf("  -w deadline       reply wait deadline in seconds\n");
    printf("  -W timeout        time to wait for response in seconds (default 5)\n");
}

static uint32_t resolve_host(const char *host)
{
    if (strcmp(host, "0.0.0.0") == 0)
        return 0x7F000001;

    /* Try as dotted IP first */
    uint32_t ip = inet_addr(host);
    if (ip != 0 && ip != 0xFFFFFFFF)
        return ntohl(ip);

    /* Consult /etc/hosts (and built-in localhost) before DNS, per
     * nsswitch.conf default order.  gethostbyname() handles both. */
    struct hostent *he = gethostbyname(host);
    if (he && he->h_addr_list && he->h_addr_list[0] && he->h_length == 4) {
        uint32_t a;
        memcpy(&a, he->h_addr_list[0], 4);   /* network byte order */
        return ntohl(a);
    }

    /* DNS resolve as a final fallback */
    uint32_t resolved;
    if (dns_resolve(host, &resolved) == 0)
        return resolved;
    return 0;
}

static const char *ip_to_str(uint32_t ip)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    return inet_ntoa(a);
}

static const char *icmp_unreach_reason(uint8_t code)
{
    switch (code) {
        case 0: return "Network unreachable";
        case 1: return "Host unreachable";
        case 3: return "Port unreachable";
        case 4: return "Fragmentation needed";
        default: return "Destination unreachable";
    }
}

int main(int argc, char *argv[])
{
    int count = -1;         /* -1 = infinite */
    int interval = 1;       /* seconds between packets */
    int packetsize = 56;    /* data bytes */
    int ttl = 64;
    int deadline = 0;       /* -w: overall timeout in seconds */
    int timeout = 5;        /* -W: per-packet timeout in seconds */
    int quiet = 0;
    int verbose = 0;
    int numeric = 0;
    int audible = 0;
    int adaptive = 0;
    int flood = 0;
    int print_timestamp = 0;
    int record_route = 0;
    int preload = 0;
    int report_outstanding = 0;
    int opt;

    /* These options are accepted but have no effect in LikeOS */
    (void)adaptive;
    (void)record_route;

    while ((opt = getopt(argc, argv, "46aAbBc:dDfhi:I:l:Lm:M:nOp:qQ:rRs:S:t:UvVw:W:")) != -1) {
        switch (opt) {
            case '4': /* IPv4 - default */ break;
            case '6':
                fprintf(stderr, "ping: IPv6 is not supported\n");
                return 1;
            case 'a': audible = 1; break;
            case 'A': adaptive = 1; break;
            case 'b': /* allow broadcast - accepted */ break;
            case 'B': /* sticky source - accepted */ break;
            case 'c': count = atoi(optarg); break;
            case 'd': /* SO_DEBUG - accepted but no effect */ break;
            case 'D': print_timestamp = 1; break;
            case 'f': flood = 1; interval = 0; break;
            case 'h': print_help(argv[0]); return 0;
            case 'i': interval = atoi(optarg); break;
            case 'I': /* source address/interface - accepted */ break;
            case 'l': preload = atoi(optarg); break;
            case 'L': /* suppress loopback - accepted */ break;
            case 'm': /* mark - accepted but no effect */ break;
            case 'M': /* pmtudisc - accepted but no effect */ break;
            case 'n': numeric = 1; break;
            case 'O': report_outstanding = 1; break;
            case 'p': /* pattern - accepted but no effect */ break;
            case 'q': quiet = 1; break;
            case 'Q': /* TOS - accepted but no effect */ break;
            case 'r': /* bypass routing - accepted */ break;
            case 'R': record_route = 1; break;
            case 's': packetsize = atoi(optarg); break;
            case 'S': /* sndbuf - accepted but no effect */ break;
            case 't': ttl = atoi(optarg); break;
            case 'U': /* user-to-user latency - accepted */ break;
            case 'v': verbose = 1; break;
            case 'V': print_version(); return 0;
            case 'w': deadline = atoi(optarg); break;
            case 'W': timeout = atoi(optarg); break;
            default:
                fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "ping: usage error: Destination address required\n");
        return 1;
    }

    (void)numeric; /* display control only, we always show IP */

    const char *dest = argv[optind];
    uint32_t dst_ip = resolve_host(dest);
    if (dst_ip == 0) {
        fprintf(stderr, "ping: %s: Name or service not known\n", dest);
        return 2;
    }

    g_dest = dest;
    signal(SIGINT, sigint_handler);

    int total_size = packetsize + 8; /* + ICMP header */
    printf("PING %s (%s) %d(%d) bytes of data.\n",
           dest, ip_to_str(dst_ip), packetsize, total_size + 20);

    uint16_t id = (uint16_t)(getpid() & 0xFFFF);

    /* Send preload packets */
    for (int p = 0; p < preload; p++) {
        uint32_t id_seq = ((uint32_t)id << 16) | (uint16_t)(p + 1);
        raw_send(RAW_SEND_ICMP_ECHO, dst_ip, id_seq, (uint32_t)ttl);
        g_sent++;
    }

    int seq_start = preload + 1;
    for (int seq = seq_start; count < 0 || (seq - seq_start) < count; seq++) {
        /* Check deadline */
        if (deadline > 0) {
            /* Approximate: we rely on the loop timing */
            int elapsed = (seq - seq_start) * (interval > 0 ? interval : 1);
            if (elapsed >= deadline)
                break;
        }

        /* Send ICMP echo */
        uint32_t id_seq = ((uint32_t)id << 16) | (uint16_t)seq;
        struct timeval tv_send, tv_recv;
        gettimeofday(&tv_send, NULL);
        int ret = raw_send(RAW_SEND_ICMP_ECHO, dst_ip, id_seq, (uint32_t)ttl);
        if (ret < 0) {
            if (!quiet)
                fprintf(stderr, "ping: sendto: Network is unreachable\n");
            g_sent++;
            g_errors++;
            if (!flood && interval > 0) sleep((unsigned)interval);
            continue;
        }
        g_sent++;

        /* Wait for reply */
        uint64_t timeout_ticks = (uint64_t)timeout * 100;
        icmp_reply_t reply;
        ret = raw_recv(RAW_RECV_ICMP_REPLY, &reply, id_seq, timeout_ticks);
        if (ret == 0) {
            gettimeofday(&tv_recv, NULL);
            g_received++;
            uint32_t src = ((reply.src_ip >> 24) & 0xFF) |
                           ((reply.src_ip >> 8) & 0xFF00) |
                           ((reply.src_ip << 8) & 0xFF0000) |
                           ((reply.src_ip << 24) & 0xFF000000);
            uint8_t type = reply.type;
            uint8_t code = reply.code;
            uint16_t rseq = (uint16_t)((reply.seq >> 8) | (reply.seq << 8));

            /* Measure RTT in the waiting thread context. */
            long rtt_us = (tv_recv.tv_sec - tv_send.tv_sec) * 1000000L +
                          (tv_recv.tv_usec - tv_send.tv_usec);
            if (rtt_us < 0) rtt_us = 0;

            /* Received TTL from IP header */
            uint8_t recv_ttl = reply.ttl;

            uint32_t rtt_ms_10 = (uint32_t)rtt_us; /* in µs units */
            if (g_min_ms > rtt_ms_10) g_min_ms = rtt_ms_10;
            if (g_max_ms < rtt_ms_10) g_max_ms = rtt_ms_10;
            g_total_ms += rtt_ms_10;

            if (flood) {
                /* Flood mode: print backspace for each reply */
                if (!quiet) write(STDOUT_FILENO, "\b \b", 3);
            } else if (!quiet) {
                if (print_timestamp) {
                    struct timeval tv_now;
                    gettimeofday(&tv_now, NULL);
                    printf("[%ld.%06ld] ", tv_now.tv_sec, tv_now.tv_usec);
                }
                if (type == 0) {
                    printf("%d bytes from %s: icmp_seq=%u ttl=%u time=%lu.%03lu ms\n",
                           packetsize + 8, ip_to_str(src), rseq, recv_ttl,
                           (unsigned long)(rtt_us / 1000),
                           (unsigned long)(rtt_us % 1000));
                } else if (type == 11) {
                    printf("From %s icmp_seq=%u Time to live exceeded\n",
                           ip_to_str(src), rseq);
                } else if (type == 3) {
                    printf("From %s icmp_seq=%u %s\n",
                           ip_to_str(src), rseq, icmp_unreach_reason(code));
                }
            }

            if (audible && !quiet)
                write(STDOUT_FILENO, "\a", 1);
        } else {
            if (flood) {
                if (!quiet) write(STDOUT_FILENO, ".", 1);
            } else if (report_outstanding && !quiet) {
                printf("no answer yet for icmp_seq=%d\n", seq);
            } else if (verbose && !quiet) {
                printf("Request timeout for icmp_seq %d\n", seq);
            }
        }

        if (count >= 0 && (seq - seq_start + 1) >= count)
            break;

        if (!flood && interval > 0)
            sleep((unsigned)interval);
    }

    /* Statistics */
    if (flood && !quiet) printf("\n");
    print_stats();

    return g_received > 0 ? 0 : 1;
}
