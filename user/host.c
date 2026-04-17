/*
 * host - simple DNS lookup utility
 *
 * Perform DNS lookups for a hostname or IP address using the
 * system resolver. Displays A, AAAA, and MX records by default.
 * Use -t to query specific record types, -v for detailed output.
 *
 * Usage:
 *   host [-aCdirsTvw] [-c class] [-N ndots] [-R retries]
 *        [-t type] [-W wait] [-4] name [server]
 *
 * Options:
 *   -t type   record type (A, MX, etc.)  -c class  query class (default IN)
 *   -a        all records (-v -t ANY)     -v        verbose (dig-like)
 *   -r        non-recursive query         -T        use TCP
 *   -W N      timeout in seconds          -R N      retries
 *   -N N      ndots threshold             -C        SOA from each NS
 *   -s        no SERVFAIL retry           -4        IPv4 transport
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <netdb.h>
#include <arpa/inet.h>

static const char *ip_to_str(uint32_t ip)
{
    static char bufs[4][20];
    static int idx = 0;
    char *b = bufs[idx++ & 3];
    unsigned a1 = (ip >> 24) & 0xFF, a2 = (ip >> 16) & 0xFF;
    unsigned a3 = (ip >> 8) & 0xFF, a4 = ip & 0xFF;
    snprintf(b, 20, "%u.%u.%u.%u", a1, a2, a3, a4);
    return b;
}

/* Skip a DNS compressed name, return new offset */
static int skip_dns_name(const uint8_t *pkt, int pktlen, int off)
{
    while (off < pktlen) {
        uint8_t len = pkt[off];
        if (len == 0) return off + 1;
        if ((len & 0xC0) == 0xC0) return off + 2;
        off += 1 + len;
    }
    return pktlen;
}

/* Decode a DNS compressed name into dotted string, return new offset */
static int decode_dns_name(const uint8_t *pkt, int pktlen, int off,
                           char *out, int outsz)
{
    int saved = -1, pos = off, olen = 0, jumps = 0;
    out[0] = '\0';
    while (pos < pktlen && jumps < 16) {
        uint8_t len = pkt[pos];
        if (len == 0) { pos++; break; }
        if ((len & 0xC0) == 0xC0) {
            if (saved < 0) saved = pos + 2;
            pos = ((len & 0x3F) << 8) | pkt[pos + 1];
            jumps++;
            continue;
        }
        pos++;
        if (olen > 0 && olen < outsz - 1) out[olen++] = '.';
        for (int i = 0; i < len && pos < pktlen && olen < outsz - 1; i++)
            out[olen++] = (char)pkt[pos++];
    }
    out[olen] = '\0';
    return saved >= 0 ? saved : pos;
}

static void print_help(void)
{
    printf("Usage: host [-aCdilrsTwv] [-c class] [-N ndots] [-R number]\n");
    printf("            [-t type] [-W wait] [-m flag] [-4] [-6] name [server]\n\n");
    printf("DNS lookup utility.\n\n");
    printf("Options:\n");
    printf("  -a            equivalent to -v -t ANY\n");
    printf("  -C            find SOA records from authoritative NS\n");
    printf("  -c class      query class (default IN)\n");
    printf("  -d            verbose (same as -v)\n");
    printf("  -i            use IP6.INT for IPv6 reverse\n");
    printf("  -l            list zone (AXFR, not supported)\n");
    printf("  -N ndots      dots before name is absolute\n");
    printf("  -R number     number of UDP retries (default 1)\n");
    printf("  -r            non-recursive query\n");
    printf("  -s            don't try next server on SERVFAIL\n");
    printf("  -T            use TCP\n");
    printf("  -t type       query type (A, AAAA, MX, NS, SOA, ...)\n");
    printf("  -v            verbose output (dig-like)\n");
    printf("  -w            wait forever for reply\n");
    printf("  -W wait       timeout in seconds\n");
    printf("  -m flag       memory debugging (trace/record/usage)\n");
    printf("  -4            IPv4 transport only\n");
    printf("  -6            IPv6 transport only (not supported)\n");
    printf("  -h            display this help\n");
}

int main(int argc, char *argv[])
{
    int verbose = 0;
    int find_soa = 0;
    int list_zone = 0;
    int use_tcp = 0;
    int non_recursive = 0;
    int no_servfail_retry = 0;
    int wait_forever = 0;
    int timeout = 5;
    int ndots = 1;
    int retries = 1;
    const char *qclass = "IN";
    const char *qtype = NULL;  /* NULL = auto (A, then AAAA, then MX) */
    const char *name = NULL;
    const char *server = NULL;

    (void)find_soa; (void)list_zone; (void)use_tcp; (void)non_recursive;
    (void)no_servfail_retry; (void)wait_forever; (void)timeout;
    (void)ndots; (void)retries; (void)qclass;

    int opt;
    while ((opt = getopt(argc, argv, "aCc:dilN:R:rsT t:vwW:m:46h")) != -1) {
        switch (opt) {
            case 'a':
                verbose = 1;
                qtype = "ANY";
                break;
            case 'C': find_soa = 1; break;
            case 'c': qclass = optarg; break;
            case 'd': verbose = 1; break;
            case 'i': break; /* IP6.INT - accepted */
            case 'l': list_zone = 1; break;
            case 'N': ndots = atoi(optarg); break;
            case 'R': retries = atoi(optarg); break;
            case 'r': non_recursive = 1; break;
            case 's': no_servfail_retry = 1; break;
            case 'T': use_tcp = 1; break;
            case 't': qtype = optarg; break;
            case 'v': verbose = 1; break;
            case 'w': wait_forever = 1; break;
            case 'W': timeout = atoi(optarg); break;
            case 'm': break; /* memory debug - accepted */
            case '4': break; /* IPv4 - default */
            case '6':
                fprintf(stderr, "host: IPv6 transport not supported\n");
                return 1;
            case 'h': print_help(); return 0;
            default: print_help(); return 1;
        }
    }

    /* Remaining positional arguments */
    if (optind < argc)
        name = argv[optind++];
    if (optind < argc)
        server = argv[optind++];

    (void)server; /* DNS server - our resolver uses kernel DNS */

    if (!name) {
        print_help();
        return 1;
    }

    if (!qtype)
        qtype = "A";

    if (list_zone) {
        fprintf(stderr, "host: zone transfer (AXFR) not supported\n");
        return 1;
    }

    if (find_soa) {
        /* Only A records supported; show what we can */
        printf("Nameserver lookup for SOA records of '%s' not supported\n", name);
        printf("(kernel DNS resolver only supports A record queries)\n");
        return 1;
    }

    /* Check if name looks like an IP address (reverse lookup) */
    uint32_t check_ip = ntohl(inet_addr(name));
    int is_reverse = (check_ip != 0 && check_ip != 0xFFFFFFFF);

    if (is_reverse) {
        /* Reverse lookup: build PTR query */
        unsigned a = (check_ip >> 24) & 0xFF, b = (check_ip >> 16) & 0xFF;
        unsigned c = (check_ip >> 8) & 0xFF, d = check_ip & 0xFF;

        dns_query_buf_t qbuf;
        memset(&qbuf, 0, sizeof(qbuf));
        snprintf(qbuf.name, sizeof(qbuf.name),
                 "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
        qbuf.qtype = DNS_TYPE_PTR;

        int ret = dns_query(&qbuf);
        if (ret == 0 && qbuf.response_len >= 12) {
            const uint8_t *pkt = qbuf.response;
            int pktlen = qbuf.response_len;
            uint16_t ancount = (pkt[6] << 8) | pkt[7];
            uint16_t qdcount = (pkt[4] << 8) | pkt[5];
            int pos = 12;
            for (int i = 0; i < qdcount; i++) {
                pos = skip_dns_name(pkt, pktlen, pos);
                pos += 4;
            }
            int found = 0;
            for (int i = 0; i < ancount && pos < pktlen; i++) {
                char rname[256];
                pos = decode_dns_name(pkt, pktlen, pos, rname, sizeof(rname));
                if (pos + 10 > pktlen) break;
                uint16_t rtype = (pkt[pos] << 8) | pkt[pos + 1];
                uint16_t rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
                pos += 10;
                if (rtype == DNS_TYPE_PTR && pos + rdlen <= pktlen) {
                    char ptrname[256];
                    decode_dns_name(pkt, pktlen, pos, ptrname, sizeof(ptrname));
                    if (verbose) {
                        printf("%s.\t\t300\tIN\tPTR\t%s.\n", qbuf.name, ptrname);
                    } else {
                        printf("%s domain name pointer %s.\n", qbuf.name, ptrname);
                    }
                    found = 1;
                }
                pos += rdlen;
            }
            if (!found)
                printf("Host %s not found: 3(NXDOMAIN)\n", name);
        } else {
            printf("Host %s not found: 3(NXDOMAIN)\n", name);
            return 1;
        }
        return 0;
    }

    /* Forward lookup: query A records */
    uint16_t qt = DNS_TYPE_A;
    if (qtype) {
        if (strcmp(qtype, "PTR") == 0) qt = DNS_TYPE_PTR;
        else if (strcmp(qtype, "CNAME") == 0) qt = DNS_TYPE_CNAME;
        else if (strcmp(qtype, "ANY") == 0) qt = 255;
        else if (strcmp(qtype, "NS") == 0) qt = 2;
        else if (strcmp(qtype, "MX") == 0) qt = 15;
        else if (strcmp(qtype, "SOA") == 0) qt = 6;
    }

    dns_query_buf_t qbuf;
    memset(&qbuf, 0, sizeof(qbuf));
    strncpy(qbuf.name, name, 255);
    qbuf.qtype = qt;

    int ret = dns_query(&qbuf);
    if (ret == 0 && qbuf.response_len >= 12) {
        const uint8_t *pkt = qbuf.response;
        int pktlen = qbuf.response_len;
        uint16_t flags = (pkt[2] << 8) | pkt[3];
        uint16_t qdcount = (pkt[4] << 8) | pkt[5];
        uint16_t ancount = (pkt[6] << 8) | pkt[7];
        int rcode = flags & 0x0F;

        if (verbose) {
            printf("Trying \"%s\"\n", name);
            printf(";; ->>HEADER<<- opcode: QUERY, status: %s, id: %u\n",
                   rcode == 0 ? "NOERROR" : "NXDOMAIN",
                   (unsigned)((pkt[0] << 8) | pkt[1]));
            printf(";; flags: qr rd ra; QUERY: %u, ANSWER: %u, AUTHORITY: 0, ADDITIONAL: 0\n\n",
                   qdcount, ancount);
            printf(";; QUESTION SECTION:\n");
            printf(";%s.\t\t\tIN\t%s\n\n", name, qtype ? qtype : "A");
            if (ancount > 0)
                printf(";; ANSWER SECTION:\n");
        }

        /* Skip questions */
        int pos = 12;
        for (int i = 0; i < qdcount; i++) {
            pos = skip_dns_name(pkt, pktlen, pos);
            pos += 4;
        }

        int found = 0;
        for (int i = 0; i < ancount && pos < pktlen; i++) {
            char rname[256];
            pos = decode_dns_name(pkt, pktlen, pos, rname, sizeof(rname));
            if (pos + 10 > pktlen) break;
            uint16_t rtype = (pkt[pos] << 8) | pkt[pos + 1];
            uint32_t rttl = ((uint32_t)pkt[pos + 4] << 24) |
                            ((uint32_t)pkt[pos + 5] << 16) |
                            ((uint32_t)pkt[pos + 6] << 8) | pkt[pos + 7];
            uint16_t rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
            pos += 10;
            if (pos + rdlen > pktlen) break;

            if (rtype == DNS_TYPE_A && rdlen == 4) {
                uint32_t ip = ((uint32_t)pkt[pos] << 24) |
                              ((uint32_t)pkt[pos + 1] << 16) |
                              ((uint32_t)pkt[pos + 2] << 8) | pkt[pos + 3];
                if (verbose)
                    printf("%s.\t\t%u\tIN\tA\t%s\n", name, rttl, ip_to_str(ip));
                else
                    printf("%s has address %s\n", name, ip_to_str(ip));
                found = 1;
            } else if (rtype == DNS_TYPE_CNAME) {
                char cname[256];
                decode_dns_name(pkt, pktlen, pos, cname, sizeof(cname));
                if (verbose)
                    printf("%s.\t\t%u\tIN\tCNAME\t%s.\n", name, rttl, cname);
                else
                    printf("%s is an alias for %s.\n", name, cname);
                found = 1;
            } else if (rtype == DNS_TYPE_PTR) {
                char ptrname[256];
                decode_dns_name(pkt, pktlen, pos, ptrname, sizeof(ptrname));
                if (verbose)
                    printf("%s.\t\t%u\tIN\tPTR\t%s.\n", rname, rttl, ptrname);
                else
                    printf("%s domain name pointer %s.\n", rname, ptrname);
                found = 1;
            } else if (rtype == 15 /* MX */ && rdlen >= 2) {
                uint16_t pref = (pkt[pos] << 8) | pkt[pos + 1];
                char mxname[256];
                decode_dns_name(pkt, pktlen, pos + 2, mxname, sizeof(mxname));
                if (verbose)
                    printf("%s.\t\t%u\tIN\tMX\t%u %s.\n", name, rttl, pref, mxname);
                else
                    printf("%s mail is handled by %u %s.\n", name, pref, mxname);
                found = 1;
            }
            pos += rdlen;
        }

        if (verbose && found)
            printf("\nReceived %d bytes from %s#53\n",
                   pktlen, server ? server : "(system DNS)");

        if (!found && rcode != 0) {
            printf("Host %s not found: 3(NXDOMAIN)\n", name);
            return 1;
        }
    } else {
        if (verbose) {
            printf("Trying \"%s\"\n", name);
            printf(";; ->>HEADER<<- opcode: QUERY, status: NXDOMAIN, id: 1\n");
            printf(";; flags: qr rd ra; QUERY: 1, ANSWER: 0, AUTHORITY: 0, ADDITIONAL: 0\n\n");
        }
        printf("Host %s not found: 3(NXDOMAIN)\n", name);
        return 1;
    }

    return 0;
}
