/*
 * traceroute - trace the route to a network host
 *
 * Print the route packets take to reach a network host by sending
 * probe packets with increasing TTL values and displaying ICMP
 * time-exceeded replies from each intermediate hop.
 *
 * Usage:
 *   traceroute [-dFInr] [-f ttl] [-g gate] [-i dev] [-m max_ttl]
 *              [-p port] [-q N] [-s addr] [-t tos] [-w timeout]
 *              [-z wait] host [packetlen]
 *
 * Options:
 *   -f ttl    first hop TTL (default 1)     -m max   max hops (default 30)
 *   -n        numeric output                -q N     probes per hop (default 3)
 *   -I        ICMP ECHO probes (default)    -i dev   network interface
 *   -s addr   source address                -p port  destination port
 *   -w secs   wait timeout                  -d       debug mode
 *   -F        do not fragment               -r       bypass routing
 *   -t tos    Type of Service               -z ms    send wait interval
 *   --sport   source port                   --mtu    discover MTU
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>

static const char *ip_to_str(uint32_t ip)
{
    struct in_addr a;
    a.s_addr = htonl(ip);
    return inet_ntoa(a);
}

static uint32_t resolve_host(const char *host)
{
    uint32_t ip = inet_addr(host);
    if (ip != 0 && ip != 0xFFFFFFFF)
        return ntohl(ip);
    /* Consult /etc/hosts (and built-in localhost) before DNS. */
    struct hostent *he = gethostbyname(host);
    if (he && he->h_addr_list && he->h_addr_list[0] && he->h_length == 4) {
        uint32_t a;
        memcpy(&a, he->h_addr_list[0], 4);
        return ntohl(a);
    }
    uint32_t resolved;
    if (dns_resolve(host, &resolved) == 0)
        return resolved;
    return 0;
}

static void print_help(const char *progname)
{
    printf("Usage: %s [OPTIONS] host [packetlen]\n", progname);
    printf("Print the route packets take to network host.\n\n");
    printf("Options:\n");
    printf("  -4              use IPv4 (default)\n");
    printf("  -6              use IPv6 (not supported)\n");
    printf("  -d              enable socket level debugging\n");
    printf("  -F              set Don't Fragment bit\n");
    printf("  -f first_ttl    start from the first_ttl hop (default 1)\n");
    printf("  -g gate         loose source route gateway\n");
    printf("  -I              use ICMP ECHO for probes\n");
    printf("  -T              use TCP SYN for probes (not supported)\n");
    printf("  -i device       specify network interface\n");
    printf("  -m max_ttl      max time-to-live (default 30)\n");
    printf("  -N squeries     simultaneous probes (default 16)\n");
    printf("  -n              print numeric addresses only\n");
    printf("  -p port         destination port\n");
    printf("  -t tos          set Type of Service\n");
    printf("  -l flow_label   IPv6 flow label (not supported)\n");
    printf("  -w MAX,HERE,NEAR  wait times in seconds\n");
    printf("  -q nqueries     probes per hop (default 3)\n");
    printf("  -r              bypass routing tables\n");
    printf("  -s src_addr     use source address\n");
    printf("  -z sendwait     time between probes in ms\n");
    printf("  -e              show ICMP extensions\n");
    printf("  -A              AS path lookups (not supported)\n");
    printf("  -V              print version and exit\n");
    printf("  -U              use UDP probes\n");
    printf("  -D              use DCCP probes (not supported)\n");
    printf("  -P proto        use raw IP protocol\n");
    printf("  -M method       use method (default/icmp/tcp/udp)\n");
    printf("  --sport=port    source port\n");
    printf("  --fwmark=mark   firewall mark (not supported)\n");
    printf("  --mtu           discover path MTU\n");
    printf("  --back          print backward hop count\n");
    printf("  -h, --help      display this help\n");
}

int main(int argc, char *argv[])
{
    int max_ttl = 30;
    int nqueries = 3;
    int first_ttl = 1;
    int numeric = 0;
    int wait_max = 5;
    int squeries = 16;
    int sendwait = 0;
    int show_extensions = 0;
    int discover_mtu = 0;
    int show_back = 0;
    int debug = 0;
    int dont_fragment = 0;
    int tos = 0;
    int port = 33434;
    int sport = 0;
    const char *src_addr = NULL;
    const char *device = NULL;
    const char *gateway = NULL;
    const char *method = "icmp";
    int packetlen = 60;

    (void)squeries; (void)sendwait; (void)show_extensions;
    (void)discover_mtu; (void)show_back; (void)debug;
    (void)dont_fragment; (void)tos; (void)port; (void)sport;
    (void)src_addr; (void)device; (void)gateway;

    static struct option long_options[] = {
        {"help",    no_argument,       0, 'h'},
        {"sport",   required_argument, 0, 0x100},
        {"fwmark",  required_argument, 0, 0x101},
        {"mtu",     no_argument,       0, 0x102},
        {"back",    no_argument,       0, 0x103},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "46dFf:g:ITi:m:N:np:t:l:w:q:rs:z:eAVUDP:M:Oh",
                              long_options, NULL)) != -1) {
        switch (opt) {
            case '4': break; /* IPv4 default */
            case '6':
                fprintf(stderr, "traceroute: IPv6 not supported\n");
                return 1;
            case 'd': debug = 1; break;
            case 'F': dont_fragment = 1; break;
            case 'f': first_ttl = atoi(optarg); break;
            case 'g': gateway = optarg; break;
            case 'I': method = "icmp"; break;
            case 'T':
                fprintf(stderr, "traceroute: TCP method not supported\n");
                return 1;
            case 'i': device = optarg; break;
            case 'm': max_ttl = atoi(optarg); break;
            case 'N': squeries = atoi(optarg); break;
            case 'n': numeric = 1; break;
            case 'p': port = atoi(optarg); break;
            case 't': tos = atoi(optarg); break;
            case 'l':
                fprintf(stderr, "traceroute: IPv6 flow labels not supported\n");
                return 1;
            case 'w': {
                /* Parse MAX,HERE,NEAR or just MAX */
                wait_max = atoi(optarg);
                break;
            }
            case 'q': nqueries = atoi(optarg); break;
            case 'r': break; /* bypass routing - accepted */
            case 's': src_addr = optarg; break;
            case 'z': sendwait = atoi(optarg); break;
            case 'e': show_extensions = 1; break;
            case 'A': break; /* AS lookups - silently accepted */
            case 'V':
                printf("traceroute (LikeOS) 1.0\n");
                return 0;
            case 'U': method = "udp"; break;
            case 'D':
                fprintf(stderr, "traceroute: DCCP not supported\n");
                return 1;
            case 'P': break; /* raw protocol - accepted */
            case 'M': method = optarg; break;
            case 'O': break; /* method options */
            case 0x100: /* --sport */
                sport = atoi(optarg); break;
            case 0x101: /* --fwmark */
                break; /* silently accepted */
            case 0x102: /* --mtu */
                discover_mtu = 1; break;
            case 0x103: /* --back */
                show_back = 1; break;
            case 'h': print_help(argv[0]); return 0;
            default: print_help(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "traceroute: missing host operand\n");
        print_help(argv[0]);
        return 1;
    }

    const char *host = argv[optind++];
    if (optind < argc)
        packetlen = atoi(argv[optind]);

    uint32_t dst_ip = resolve_host(host);
    if (dst_ip == 0) {
        fprintf(stderr, "traceroute: %s: Name or service not known\n", host);
        return 1;
    }

    (void)method; /* We always use ICMP internally */

    printf("traceroute to %s (%s), %d hops max, %d byte packets\n",
           host, ip_to_str(dst_ip), max_ttl, packetlen);

    uint16_t id = (uint16_t)(getpid() & 0xFFFF);
    uint16_t seq = 1;

    for (int ttl = first_ttl; ttl <= max_ttl; ttl++) {
        printf("%2d ", ttl);
        int reached = 0;
        uint32_t prev_src = 0;

        for (int q = 0; q < nqueries; q++) {
            uint32_t id_seq = ((uint32_t)id << 16) | seq;
            struct timeval tv_send, tv_recv;
            gettimeofday(&tv_send, NULL);
            int ret = raw_send(RAW_SEND_ICMP_ECHO, dst_ip, id_seq, (uint32_t)ttl);
            if (ret < 0) {
                printf(" *");
                seq++;
                continue;
            }

            uint64_t timeout_ticks = (uint64_t)wait_max * 100;
            icmp_reply_t reply;
            ret = raw_recv(RAW_RECV_ICMP_REPLY, &reply, id_seq, timeout_ticks);
            if (ret == 0) {
                gettimeofday(&tv_recv, NULL);
                uint32_t src = ((reply.src_ip >> 24) & 0xFF) |
                               ((reply.src_ip >> 8) & 0xFF00) |
                               ((reply.src_ip << 8) & 0xFF0000) |
                               ((reply.src_ip << 24) & 0xFF000000);
                uint8_t type = reply.type;
                uint8_t code = reply.code;

                long rtt_us = (tv_recv.tv_sec - tv_send.tv_sec) * 1000000L +
                              (tv_recv.tv_usec - tv_send.tv_usec);
                if (rtt_us < 0) rtt_us = 0;

                if (q == 0 || src != prev_src) {
                    if (q > 0) printf("\n   ");
                    /* Reverse DNS lookup for hop hostname */
                    if (!numeric) {
                        char hname[256];
                        uint32_t src_nbo = htonl(src);
                        if (dns_resolve_reverse(src_nbo, hname, sizeof(hname)) == 0) {
                            printf(" %s (%s)", hname, ip_to_str(src));
                        } else {
                            printf(" %s (%s)", ip_to_str(src), ip_to_str(src));
                        }
                    } else {
                        printf(" %s (%s)", ip_to_str(src), ip_to_str(src));
                    }
                    prev_src = src;
                }
                printf("  %lu.%03lu ms", (unsigned long)(rtt_us / 1000),
                       (unsigned long)(rtt_us % 1000));

                if (type == ICMP_DEST_UNREACH)
                    printf(" !%u", code);

                if (type == 0) /* Echo reply - reached destination */
                    reached = 1;
            } else {
                printf(" *");
            }
            seq++;
        }
        printf("\n");

        if (reached)
            break;
    }

    return 0;
}
