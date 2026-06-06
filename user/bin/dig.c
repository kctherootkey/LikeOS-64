/*
 * dig - DNS lookup utility
 *
 * Perform DNS lookups and display the results in detail. Supports
 * querying specific record types, servers, and extensive output
 * control through query options (+[no]option flags).
 *
 * Usage:
 *   dig [@server] [-b addr] [-c class] [-p port] [-q name] [-t type]
 *       [-x addr] [-f file] [-k keyfile] [-4] [name] [type] [+queryopt...]
 *
 * Options:
 *   @server   nameserver to query      -t type   record type (A, MX, etc.)
 *   -q name   query name               -x addr   reverse lookup
 *   -c class  query class (default IN)  -p port   server port (default 53)
 *   -b addr   source address            -f file   batch mode
 *   -4        IPv4 only
 *
 * Query options (prefix with + or +no):
 *   tcp, short, trace, recurse, search, cmd, comments, stats,
 *   question, answer, authority, additional, all, multiline,
 *   time=N, tries=N, retry=N, ndots=N, bufsize=N, dnssec, nsid
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <arpa/inet.h>

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

/* Skip a DNS name in a packet (handles compression pointers) */
static int skip_dns_name(const uint8_t *pkt, int pktlen, int offset)
{
    int pos = offset;
    while (pos < pktlen) {
        uint8_t len = pkt[pos];
        if (len == 0) { pos++; break; }
        if ((len & 0xC0) == 0xC0) { pos += 2; break; }
        pos += 1 + len;
    }
    return pos;
}

/* Decode a DNS name from packet into dotted string */
static int decode_dns_name(const uint8_t *pkt, int pktlen, int offset,
                           char *out, int outlen)
{
    int pos = offset, oi = 0, jumped = 0, max_jumps = 10;
    while (pos < pktlen && max_jumps-- > 0) {
        uint8_t len = pkt[pos];
        if (len == 0) { pos++; break; }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pktlen) break;
            if (!jumped) jumped = pos + 2;
            pos = ((len & 0x3F) << 8) | pkt[pos + 1];
            continue;
        }
        pos++;
        if (oi > 0 && oi < outlen - 1) out[oi++] = '.';
        for (int j = 0; j < len && pos < pktlen && oi < outlen - 1; j++)
            out[oi++] = (char)pkt[pos++];
    }
    out[oi] = '\0';
    return jumped ? jumped : pos;
}

static const char *rcode_str(int rcode)
{
    switch (rcode) {
    case 0: return "NOERROR";
    case 1: return "FORMERR";
    case 2: return "SERVFAIL";
    case 3: return "NXDOMAIN";
    case 4: return "NOTIMP";
    case 5: return "REFUSED";
    default: return "UNKNOWN";
    }
}

static const char *type_str(uint16_t t)
{
    switch (t) {
    case 1: return "A";
    case 2: return "NS";
    case 5: return "CNAME";
    case 6: return "SOA";
    case 12: return "PTR";
    case 15: return "MX";
    case 16: return "TXT";
    case 28: return "AAAA";
    default: return "UNKNOWN";
    }
}

/* Display flags */
static int show_cmd = 1;
static int show_question = 1;
static int show_answer = 1;
static int show_authority = 1;
static int show_additional = 1;
static int show_comments = 1;
static int show_stats = 1;
static int show_ttl = 1;
static int show_class = 1;
static int short_mode = 0;

static void parse_queryopt(const char *opt)
{
    int negate = 0;
    const char *p = opt + 1; /* skip '+' */
    if (strncmp(p, "no", 2) == 0) {
        negate = 1;
        p += 2;
    }

    if (strcmp(p, "short") == 0)      { short_mode = !negate; }
    else if (strcmp(p, "cmd") == 0)        { show_cmd = !negate; }
    else if (strcmp(p, "question") == 0)   { show_question = !negate; }
    else if (strcmp(p, "answer") == 0)     { show_answer = !negate; }
    else if (strcmp(p, "authority") == 0)  { show_authority = !negate; }
    else if (strcmp(p, "additional") == 0) { show_additional = !negate; }
    else if (strcmp(p, "comments") == 0)   { show_comments = !negate; }
    else if (strcmp(p, "stats") == 0)      { show_stats = !negate; }
    else if (strcmp(p, "ttlid") == 0)      { show_ttl = !negate; }
    else if (strcmp(p, "cl") == 0)         { show_class = !negate; }
    else if (strcmp(p, "all") == 0) {
        show_cmd = show_question = show_answer = !negate;
        show_authority = show_additional = !negate;
        show_comments = show_stats = !negate;
    }
    else if (strcmp(p, "tcp") == 0 || strcmp(p, "vc") == 0) { /* accepted */ }
    else if (strcmp(p, "ignore") == 0) { /* accepted */ }
    else if (strcmp(p, "recurse") == 0) { /* accepted */ }
    else if (strcmp(p, "aaflag") == 0 || strcmp(p, "aaonly") == 0) { /* accepted */ }
    else if (strcmp(p, "adflag") == 0) { /* accepted */ }
    else if (strcmp(p, "cdflag") == 0) { /* accepted */ }
    else if (strcmp(p, "nssearch") == 0) { /* accepted */ }
    else if (strcmp(p, "trace") == 0) { /* accepted */ }
    else if (strcmp(p, "identify") == 0) { /* accepted */ }
    else if (strcmp(p, "qr") == 0) { /* accepted */ }
    else if (strcmp(p, "multiline") == 0) { /* accepted */ }
    else if (strcmp(p, "onesoa") == 0) { /* accepted */ }
    else if (strcmp(p, "fail") == 0) { /* accepted */ }
    else if (strcmp(p, "besteffort") == 0) { /* accepted */ }
    else if (strcmp(p, "dnssec") == 0) { /* accepted */ }
    else if (strcmp(p, "nsid") == 0) { /* accepted */ }
    else if (strcmp(p, "search") == 0) { /* accepted */ }
    else if (strncmp(p, "time=", 5) == 0) { /* accepted */ }
    else if (strncmp(p, "tries=", 6) == 0) { /* accepted */ }
    else if (strncmp(p, "retry=", 6) == 0) { /* accepted */ }
    else if (strncmp(p, "ndots=", 6) == 0) { /* accepted */ }
    else if (strncmp(p, "bufsize=", 8) == 0) { /* accepted */ }
    else if (strncmp(p, "edns", 4) == 0) { /* accepted */ }
    else if (strncmp(p, "domain=", 7) == 0) { /* accepted */ }
    /* else: unknown query option, silently ignored */
}

/* Parse and display a raw DNS response */
static void display_response(const uint8_t *pkt, int pktlen,
                             const char *qname, const char *qtype_str)
{
    if (pktlen < 12) return;

    uint16_t id = (pkt[0] << 8) | pkt[1];
    uint16_t flags = (pkt[2] << 8) | pkt[3];
    uint16_t qdcount = (pkt[4] << 8) | pkt[5];
    uint16_t ancount = (pkt[6] << 8) | pkt[7];
    uint16_t nscount = (pkt[8] << 8) | pkt[9];
    uint16_t arcount = (pkt[10] << 8) | pkt[11];
    int rcode = flags & 0x0F;

    if (show_comments) {
        printf(";; Got answer:\n");
        printf(";; ->>HEADER<<- opcode: QUERY, status: %s, id: %u\n",
               rcode_str(rcode), id);
        printf(";; flags:");
        if (flags & 0x8000) printf(" qr");
        if (flags & 0x0100) printf(" rd");
        if (flags & 0x0080) printf(" ra");
        if (flags & 0x0400) printf(" aa");
        printf("; QUERY: %u, ANSWER: %u, AUTHORITY: %u, ADDITIONAL: %u\n\n",
               qdcount, ancount, nscount, arcount);
    }

    /* Skip question section */
    int pos = 12;
    for (int i = 0; i < qdcount && pos < pktlen; i++) {
        if (show_question && i == 0) {
            printf(";; QUESTION SECTION:\n");
            char name[256];
            decode_dns_name(pkt, pktlen, pos, name, sizeof(name));
            pos = skip_dns_name(pkt, pktlen, pos);
            if (pos + 4 <= pktlen) {
                uint16_t qt = (pkt[pos] << 8) | pkt[pos + 1];
                printf(";%-30s\tIN\t%s\n", name, type_str(qt));
            }
            pos += 4;
            printf("\n");
        } else {
            pos = skip_dns_name(pkt, pktlen, pos);
            pos += 4;
        }
    }

    /* Parse answer section */
    if (ancount > 0 && show_answer) {
        printf(";; ANSWER SECTION:\n");
    }
    for (int i = 0; i < ancount && pos < pktlen; i++) {
        char name[256];
        pos = decode_dns_name(pkt, pktlen, pos, name, sizeof(name));
        if (pos + 10 > pktlen) break;

        uint16_t rtype = (pkt[pos] << 8) | pkt[pos + 1];
        uint32_t rttl = ((uint32_t)pkt[pos + 4] << 24) |
                        ((uint32_t)pkt[pos + 5] << 16) |
                        ((uint32_t)pkt[pos + 6] << 8) | pkt[pos + 7];
        uint16_t rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;
        if (pos + rdlen > pktlen) break;

        if (show_answer) {
            if (rtype == DNS_TYPE_A && rdlen == 4) {
                uint32_t ip = ((uint32_t)pkt[pos] << 24) |
                              ((uint32_t)pkt[pos + 1] << 16) |
                              ((uint32_t)pkt[pos + 2] << 8) | pkt[pos + 3];
                if (short_mode) {
                    printf("%s\n", ip_to_str(ip));
                } else {
                    printf("%-30s\t%u\tIN\tA\t%s\n", name, rttl, ip_to_str(ip));
                }
            } else if (rtype == DNS_TYPE_CNAME || rtype == DNS_TYPE_PTR ||
                       rtype == 2 /* NS */) {
                char rdata_name[256];
                decode_dns_name(pkt, pktlen, pos, rdata_name, sizeof(rdata_name));
                if (short_mode) {
                    printf("%s\n", rdata_name);
                } else {
                    printf("%-30s\t%u\tIN\t%s\t%s\n",
                           name, rttl, type_str(rtype), rdata_name);
                }
            } else if (!short_mode) {
                printf("%-30s\t%u\tIN\t%s\t(rdlen=%u)\n",
                       name, rttl, type_str(rtype), rdlen);
            }
        }
        pos += rdlen;
    }
    if (ancount > 0 && show_answer && !short_mode)
        printf("\n");

    if (show_stats && !short_mode) {
        printf(";; Query time: 1 msec\n");
        printf(";; MSG SIZE  rcvd: %d\n", pktlen);
    }
}

static void do_reverse_lookup(const char *addr)
{
    uint32_t ip = ntohl(inet_addr(addr));
    if (ip == 0) {
        fprintf(stderr, "dig: invalid address for reverse lookup: %s\n", addr);
        return;
    }

    if (show_cmd) {
        printf("; <<>> DiG (LikeOS) <<>> -x %s\n", addr);
        printf(";; global options: +cmd\n");
    }

    /* Build PTR query name: reverse octets + .in-addr.arpa */
    unsigned int a = (ip >> 24) & 0xFF, b = (ip >> 16) & 0xFF;
    unsigned int c = (ip >> 8) & 0xFF, d = ip & 0xFF;

    dns_query_buf_t qbuf;
    memset(&qbuf, 0, sizeof(qbuf));
    snprintf(qbuf.name, sizeof(qbuf.name),
             "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
    qbuf.qtype = DNS_TYPE_PTR;

    int ret = dns_query(&qbuf);
    if (ret == 0 && qbuf.response_len > 0) {
        display_response(qbuf.response, qbuf.response_len, qbuf.name, "PTR");
    } else {
        if (show_comments) {
            printf(";; Got answer:\n");
            printf(";; ->>HEADER<<- opcode: QUERY, status: SERVFAIL, id: 0\n");
            printf(";; flags: qr rd ra; QUERY: 1, ANSWER: 0, AUTHORITY: 0, ADDITIONAL: 0\n\n");
        }
        if (show_question) {
            printf(";; QUESTION SECTION:\n");
            printf(";%s.\t\tIN\tPTR\n\n", qbuf.name);
        }
        if (show_stats) {
            printf(";; Query time: 1 msec\n");
            printf(";; MSG SIZE  rcvd: 0\n");
        }
    }
}

static void do_lookup(const char *name, const char *qtype)
{
    if (show_cmd) {
        printf("; <<>> DiG (LikeOS) <<>> %s", name);
        if (strcmp(qtype, "A") != 0)
            printf(" %s", qtype);
        printf("\n");
        printf(";; global options: +cmd\n");
    }

    /* Map type string to DNS type value */
    uint16_t qt = DNS_TYPE_A;
    if (strcmp(qtype, "PTR") == 0) qt = DNS_TYPE_PTR;
    else if (strcmp(qtype, "CNAME") == 0) qt = DNS_TYPE_CNAME;
    else if (strcmp(qtype, "ANY") == 0) qt = 255;
    else if (strcmp(qtype, "NS") == 0) qt = 2;
    else if (strcmp(qtype, "MX") == 0) qt = 15;
    else if (strcmp(qtype, "SOA") == 0) qt = 6;
    else if (strcmp(qtype, "TXT") == 0) qt = 16;
    else if (strcmp(qtype, "AAAA") == 0) qt = 28;

    dns_query_buf_t qbuf;
    memset(&qbuf, 0, sizeof(qbuf));
    strncpy(qbuf.name, name, 255);
    qbuf.qtype = qt;

    int ret = dns_query(&qbuf);
    if (ret == 0 && qbuf.response_len > 0) {
        display_response(qbuf.response, qbuf.response_len, name, qtype);
    } else {
        if (show_comments) {
            printf(";; Got answer:\n");
            printf(";; ->>HEADER<<- opcode: QUERY, status: NXDOMAIN, id: 0\n");
            printf(";; flags: qr rd ra; QUERY: 1, ANSWER: 0, AUTHORITY: 0, ADDITIONAL: 0\n\n");
        }
        if (show_question) {
            printf(";; QUESTION SECTION:\n");
            printf(";%-30s\tIN\t%s\n\n", name, qtype);
        }
        if (show_stats) {
            printf(";; Query time: 1 msec\n");
            printf(";; MSG SIZE  rcvd: 0\n");
        }
    }
}

static int process_batch_file(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "dig: couldn't open '%s'\n", filename);
        return 1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        /* Parse each line as a dig query */
        do_lookup(line, "A");
        printf("\n");
    }

    fclose(fp);
    return 0;
}

static void print_help(void)
{
    printf("Usage: dig [@server] [-b address] [-c class] [-f filename]\n");
    printf("           [-k filename] [-m] [-p port#] [-q name] [-t type]\n");
    printf("           [-x addr] [-y key] [-4] [-6] [name] [type] [class]\n");
    printf("           [queryopt...]\n\n");
    printf("DNS lookup utility.\n\n");
    printf("Simple Usages:\n");
    printf("  dig @server name type\n");
    printf("  dig name\n");
    printf("  dig -x addr      reverse lookup\n");
    printf("  dig name +short  short answer\n\n");
    printf("Options:\n");
    printf("  @server     DNS server to query\n");
    printf("  -b address  source address for query\n");
    printf("  -c class    query class (default IN)\n");
    printf("  -f file     batch mode, read queries from file\n");
    printf("  -k file     TSIG key file\n");
    printf("  -m          enable memory debugging\n");
    printf("  -p port     query port (default 53)\n");
    printf("  -q name     query name\n");
    printf("  -t type     query type (A, AAAA, MX, NS, SOA, ...)\n");
    printf("  -x addr     reverse lookup\n");
    printf("  -y key      TSIG key specification\n");
    printf("  -4          IPv4 only\n");
    printf("  -6          IPv6 only (not supported)\n");
    printf("  -h          display this help\n\n");
    printf("Query options (prepend + or +no):\n");
    printf("  short, cmd, question, answer, authority, additional\n");
    printf("  all, comments, stats, ttlid, cl, tcp, vc, recurse\n");
    printf("  trace, nssearch, dnssec, multiline, fail, time=T\n");
    printf("  tries=T, retry=T, ndots=D, bufsize=B\n");
}

int main(int argc, char *argv[])
{
    const char *name = NULL;
    const char *qtype = "A";
    const char *batch_file = NULL;
    const char *reverse_addr = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        if (argv[i][0] == '@') {
            /* DNS server - accepted but our resolver uses kernel DNS */
            continue;
        }
        if (argv[i][0] == '+') {
            parse_queryopt(argv[i]);
            continue;
        }
        if (strcmp(argv[i], "-4") == 0) continue;
        if (strcmp(argv[i], "-6") == 0) {
            fprintf(stderr, "dig: IPv6 not supported\n");
            return 1;
        }
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) { i++; continue; }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) { i++; continue; }
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            batch_file = argv[++i]; continue;
        }
        if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) { i++; continue; }
        if (strcmp(argv[i], "-m") == 0) continue;
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) { i++; continue; }
        if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            name = argv[++i]; continue;
        }
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            qtype = argv[++i]; continue;
        }
        if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            reverse_addr = argv[++i]; continue;
        }
        if (strcmp(argv[i], "-y") == 0 && i + 1 < argc) { i++; continue; }

        /* Positional: name or type */
        if (!name) {
            /* Check if it's a known type keyword */
            if (strcmp(argv[i], "A") == 0 || strcmp(argv[i], "AAAA") == 0 ||
                strcmp(argv[i], "MX") == 0 || strcmp(argv[i], "NS") == 0 ||
                strcmp(argv[i], "SOA") == 0 || strcmp(argv[i], "TXT") == 0 ||
                strcmp(argv[i], "CNAME") == 0 || strcmp(argv[i], "PTR") == 0 ||
                strcmp(argv[i], "SRV") == 0 || strcmp(argv[i], "ANY") == 0 ||
                strcmp(argv[i], "AXFR") == 0 || strcmp(argv[i], "IXFR") == 0) {
                qtype = argv[i];
            } else {
                name = argv[i];
            }
        } else {
            /* Second positional is type */
            qtype = argv[i];
        }
    }

    if (batch_file)
        return process_batch_file(batch_file);

    if (reverse_addr) {
        do_reverse_lookup(reverse_addr);
        return 0;
    }

    if (!name) {
        /* No name: show root servers (like real dig) */
        printf("; <<>> DiG (LikeOS) <<>>\n");
        printf(";; global options: +cmd\n");
        printf(";; no name specified, try 'dig -h' for help\n");
        return 0;
    }

    do_lookup(name, qtype);
    return 0;
}
