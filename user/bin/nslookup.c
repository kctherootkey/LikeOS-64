/*
 * nslookup - query Internet name servers
 *
 * Perform DNS lookups interactively or from the command line.
 * Supports changing query type, class, server, and various
 * resolver options through 'set' commands in interactive mode.
 *
 * Usage:
 *   nslookup [-option...] [name | -] [server]
 *
 * Interactive commands:
 *   host [server]          look up host
 *   server domain          change default server
 *   lserver domain         change server using initial server
 *   set keyword[=value]    modify settings
 *   exit                   quit
 *
 * Settings (via 'set' or -option):
 *   type/querytype, class, domain, port, timeout, retry,
 *   [no]debug, [no]d2, [no]search, [no]recurse, [no]vc, [no]fail
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
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

/* State variables */
static char default_server[64] = "(system DNS)";
static char default_server_addr[32] = "";
static char query_type[16] = "A";
static char query_class[8] = "IN";
static char search_domain[64] = "";
static int debug_mode = 0;
static int d2_mode = 0;
static int use_search = 1;
static int use_recurse = 1;
static int use_vc = 0;
static int use_fail = 0;
static int retry_count = 4;
static int timeout_secs = 0;
static int server_port = 53;

static void print_state(void)
{
    printf("Set options:\n");
    printf("  novc\t\t\t\t%s\n", use_vc ? "vc" : "novc");
    printf("  %sdebug\t\t\t%s\n", debug_mode ? "" : "no", debug_mode ? "debug" : "nodebug");
    printf("  %sd2\t\t\t\t%s\n", d2_mode ? "" : "no", d2_mode ? "d2" : "nod2");
    printf("  %ssearch\t\t\t%s\n", use_search ? "" : "no", use_search ? "search" : "nosearch");
    printf("  %srecurse\t\t\t%s\n", use_recurse ? "" : "no", use_recurse ? "recurse" : "norecurse");
    printf("  %sfail\t\t\t\t%s\n", use_fail ? "" : "no", use_fail ? "fail" : "nofail");
    printf("  timeout = %d\n", timeout_secs);
    printf("  retry = %d\n", retry_count);
    printf("  port = %d\n", server_port);
    printf("  querytype = %s\n", query_type);
    printf("  class = %s\n", query_class);
    if (search_domain[0]) printf("  domain = %s\n", search_domain);
    printf("  srchlist =\n");
    printf("  defname = %s\n", default_server);
}

static void process_set(const char *arg)
{
    if (strcmp(arg, "all") == 0) {
        print_state();
    } else if (strncmp(arg, "type=", 5) == 0 || strncmp(arg, "querytype=", 10) == 0) {
        const char *val = strchr(arg, '=') + 1;
        strncpy(query_type, val, sizeof(query_type) - 1);
    } else if (strncmp(arg, "class=", 6) == 0) {
        strncpy(query_class, strchr(arg, '=') + 1, sizeof(query_class) - 1);
    } else if (strncmp(arg, "domain=", 7) == 0) {
        strncpy(search_domain, strchr(arg, '=') + 1, sizeof(search_domain) - 1);
    } else if (strncmp(arg, "timeout=", 8) == 0) {
        timeout_secs = atoi(strchr(arg, '=') + 1);
    } else if (strncmp(arg, "retry=", 6) == 0) {
        retry_count = atoi(strchr(arg, '=') + 1);
    } else if (strncmp(arg, "port=", 5) == 0) {
        server_port = atoi(strchr(arg, '=') + 1);
    } else if (strcmp(arg, "debug") == 0) { debug_mode = 1; }
    else if (strcmp(arg, "nodebug") == 0) { debug_mode = 0; }
    else if (strcmp(arg, "d2") == 0) { d2_mode = 1; debug_mode = 1; }
    else if (strcmp(arg, "nod2") == 0) { d2_mode = 0; }
    else if (strcmp(arg, "search") == 0) { use_search = 1; }
    else if (strcmp(arg, "nosearch") == 0) { use_search = 0; }
    else if (strcmp(arg, "recurse") == 0) { use_recurse = 1; }
    else if (strcmp(arg, "norecurse") == 0) { use_recurse = 0; }
    else if (strcmp(arg, "vc") == 0) { use_vc = 1; }
    else if (strcmp(arg, "novc") == 0) { use_vc = 0; }
    else if (strcmp(arg, "fail") == 0) { use_fail = 1; }
    else if (strcmp(arg, "nofail") == 0) { use_fail = 0; }
    else {
        printf("*** Invalid option: %s\n", arg);
    }
}

static int do_lookup(const char *name)
{
    printf("Server:\t\t%s\n", default_server);
    printf("Address:\t%s#%d\n\n",
           default_server_addr[0] ? default_server_addr : default_server,
           server_port);

    /* Check if name looks like an IP address (reverse lookup) */
    uint32_t check_ip = ntohl(inet_addr(name));
    int is_reverse = (check_ip != 0 && check_ip != 0xFFFFFFFF);

    dns_query_buf_t qbuf;
    memset(&qbuf, 0, sizeof(qbuf));

    if (is_reverse || strcmp(query_type, "PTR") == 0) {
        /* Reverse (PTR) lookup */
        if (is_reverse) {
            unsigned a = (check_ip >> 24) & 0xFF, b = (check_ip >> 16) & 0xFF;
            unsigned c = (check_ip >> 8) & 0xFF, d = check_ip & 0xFF;
            snprintf(qbuf.name, sizeof(qbuf.name),
                     "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
        } else {
            strncpy(qbuf.name, name, 255);
        }
        qbuf.qtype = DNS_TYPE_PTR;
    } else {
        strncpy(qbuf.name, name, 255);
        /* Map query_type string to DNS type value */
        if (strcmp(query_type, "A") == 0) qbuf.qtype = DNS_TYPE_A;
        else if (strcmp(query_type, "CNAME") == 0) qbuf.qtype = DNS_TYPE_CNAME;
        else if (strcmp(query_type, "NS") == 0) qbuf.qtype = 2;
        else if (strcmp(query_type, "MX") == 0) qbuf.qtype = 15;
        else if (strcmp(query_type, "SOA") == 0) qbuf.qtype = 6;
        else if (strcmp(query_type, "ANY") == 0) qbuf.qtype = 255;
        else qbuf.qtype = DNS_TYPE_A;
    }

    int ret = dns_query(&qbuf);
    if (ret != 0 || qbuf.response_len < 12) {
        printf("** server can't find %s: NXDOMAIN\n\n", name);
        return 1;
    }

    const uint8_t *pkt = qbuf.response;
    int pktlen = qbuf.response_len;
    uint16_t qdcount = (pkt[4] << 8) | pkt[5];
    uint16_t ancount = (pkt[6] << 8) | pkt[7];
    uint16_t flags = (pkt[2] << 8) | pkt[3];
    int rcode = flags & 0x0F;

    /* Skip question section */
    int pos = 12;
    for (int i = 0; i < qdcount; i++) {
        pos = skip_dns_name(pkt, pktlen, pos);
        pos += 4;
    }

    if (ancount == 0 || rcode != 0) {
        printf("** server can't find %s: NXDOMAIN\n\n", name);
        return 1;
    }

    printf("Non-authoritative answer:\n");

    for (int i = 0; i < ancount && pos < pktlen; i++) {
        char rname[256];
        pos = decode_dns_name(pkt, pktlen, pos, rname, sizeof(rname));
        if (pos + 10 > pktlen) break;
        uint16_t rtype = (pkt[pos] << 8) | pkt[pos + 1];
        uint16_t rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;
        if (pos + rdlen > pktlen) break;

        if (rtype == DNS_TYPE_A && rdlen == 4) {
            uint32_t ip = ((uint32_t)pkt[pos] << 24) |
                          ((uint32_t)pkt[pos + 1] << 16) |
                          ((uint32_t)pkt[pos + 2] << 8) | pkt[pos + 3];
            printf("Name:\t%s\nAddress: %s\n", name, ip_to_str(ip));
        } else if (rtype == DNS_TYPE_CNAME) {
            char cname[256];
            decode_dns_name(pkt, pktlen, pos, cname, sizeof(cname));
            printf("%s\tcanonical name = %s.\n", name, cname);
        } else if (rtype == DNS_TYPE_PTR) {
            char ptrname[256];
            decode_dns_name(pkt, pktlen, pos, ptrname, sizeof(ptrname));
            printf("%s\tname = %s.\n", qbuf.name, ptrname);
        } else if (rtype == 15 /* MX */ && rdlen >= 2) {
            uint16_t pref = (pkt[pos] << 8) | pkt[pos + 1];
            char mxname[256];
            decode_dns_name(pkt, pktlen, pos + 2, mxname, sizeof(mxname));
            printf("%s\tmail exchanger = %u %s.\n", name, pref, mxname);
        } else if (rtype == 2 /* NS */) {
            char nsname[256];
            decode_dns_name(pkt, pktlen, pos, nsname, sizeof(nsname));
            printf("%s\tnameserver = %s.\n", name, nsname);
        }

        if (debug_mode) {
            printf("    type = %s, class = %s\n", query_type, query_class);
        }

        pos += rdlen;
    }

    printf("\n");
    return 0;
}

static void set_server(const char *server)
{
    strncpy(default_server, server, sizeof(default_server) - 1);
    /* Try to resolve the server address */
    uint32_t ip;
    if (dns_resolve(server, &ip) == 0) {
        strncpy(default_server_addr, ip_to_str(ip), sizeof(default_server_addr) - 1);
    } else {
        strncpy(default_server_addr, server, sizeof(default_server_addr) - 1);
    }
}

static void parse_option(const char *opt)
{
    /* Handle -option style arguments */
    if (opt[0] != '-') return;
    opt++; /* skip '-' */

    if (strncmp(opt, "type=", 5) == 0 || strncmp(opt, "query=", 6) == 0 ||
        strncmp(opt, "querytype=", 10) == 0) {
        const char *val = strchr(opt, '=') + 1;
        strncpy(query_type, val, sizeof(query_type) - 1);
    } else if (strncmp(opt, "class=", 6) == 0) {
        strncpy(query_class, strchr(opt, '=') + 1, sizeof(query_class) - 1);
    } else if (strncmp(opt, "timeout=", 8) == 0) {
        timeout_secs = atoi(strchr(opt, '=') + 1);
    } else if (strncmp(opt, "retry=", 6) == 0) {
        retry_count = atoi(strchr(opt, '=') + 1);
    } else if (strncmp(opt, "port=", 5) == 0) {
        server_port = atoi(strchr(opt, '=') + 1);
    } else if (strncmp(opt, "domain=", 7) == 0) {
        strncpy(search_domain, strchr(opt, '=') + 1, sizeof(search_domain) - 1);
    } else if (strcmp(opt, "debug") == 0)     { debug_mode = 1; }
    else if (strcmp(opt, "nodebug") == 0)    { debug_mode = 0; }
    else if (strcmp(opt, "d2") == 0)         { d2_mode = 1; debug_mode = 1; }
    else if (strcmp(opt, "nod2") == 0)       { d2_mode = 0; }
    else if (strcmp(opt, "search") == 0)     { use_search = 1; }
    else if (strcmp(opt, "nosearch") == 0)   { use_search = 0; }
    else if (strcmp(opt, "recurse") == 0)    { use_recurse = 1; }
    else if (strcmp(opt, "norecurse") == 0)  { use_recurse = 0; }
    else if (strcmp(opt, "vc") == 0)         { use_vc = 1; }
    else if (strcmp(opt, "novc") == 0)       { use_vc = 0; }
    else if (strcmp(opt, "fail") == 0)       { use_fail = 1; }
    else if (strcmp(opt, "nofail") == 0)     { use_fail = 0; }
}

static void interactive_mode(void)
{
    char line[256];

    printf("> ");
    while (fgets(line, sizeof(line), stdin)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') { printf("> "); continue; }

        /* Parse interactive commands */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0)
            break;

        if (strncmp(line, "set ", 4) == 0) {
            process_set(line + 4);
            printf("> ");
            continue;
        }

        if (strncmp(line, "server ", 7) == 0) {
            set_server(line + 7);
            printf("Default server: %s\nAddress: %s#%d\n\n",
                   default_server, default_server_addr, server_port);
            printf("> ");
            continue;
        }

        if (strncmp(line, "lserver ", 8) == 0) {
            set_server(line + 8);
            printf("Default server: %s\nAddress: %s#%d\n\n",
                   default_server, default_server_addr, server_port);
            printf("> ");
            continue;
        }

        /* Anything else is a hostname lookup, possibly with server */
        char host[128] = "", server[128] = "";
        int n = sscanf(line, "%127s %127s", host, server);
        if (n >= 2) {
            /* Temporarily use specified server */
            char saved[64];
            strncpy(saved, default_server, sizeof(saved));
            set_server(server);
            do_lookup(host);
            strncpy(default_server, saved, sizeof(default_server));
        } else if (n == 1) {
            do_lookup(host);
        }

        printf("> ");
    }
}

static void print_help(void)
{
    printf("Usage: nslookup [-option...] [name | -] [server]\n\n");
    printf("Query Internet name servers.\n\n");
    printf("Arguments:\n");
    printf("  name          domain name to look up\n");
    printf("  -             enter interactive mode\n");
    printf("  server        DNS server to use\n\n");
    printf("Options:\n");
    printf("  -type=TYPE    set query type (A, AAAA, MX, NS, SOA, etc.)\n");
    printf("  -query=TYPE   same as -type\n");
    printf("  -class=CLASS  set query class (IN, CH, HS, ANY)\n");
    printf("  -timeout=N    timeout in seconds\n");
    printf("  -retry=N      number of retries\n");
    printf("  -port=N       server port number\n");
    printf("  -domain=NAME  set search domain\n");
    printf("  -[no]debug    turn debugging on/off\n");
    printf("  -[no]d2       turn exhaustive debugging on/off\n");
    printf("  -[no]search   use search list\n");
    printf("  -[no]recurse  ask for recursive queries\n");
    printf("  -[no]vc       use TCP (virtual circuit)\n");
    printf("  -[no]fail     try next server on SERVFAIL\n\n");
    printf("Interactive commands:\n");
    printf("  host [server]     look up host\n");
    printf("  server domain     set default server\n");
    printf("  lserver domain    set server using initial server\n");
    printf("  set keyword[=val] change state\n");
    printf("  set all           print state\n");
    printf("  exit              quit\n");
}

int main(int argc, char *argv[])
{
    const char *name = NULL;
    const char *server = NULL;
    int enter_interactive = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            if (strcmp(argv[i], "-") == 0) {
                enter_interactive = 1;
            } else {
                parse_option(argv[i]);
            }
            continue;
        }
        if (!name)
            name = argv[i];
        else if (!server)
            server = argv[i];
    }

    if (server)
        set_server(server);

    /* Interactive mode: no name given, or name is "-" */
    if (!name || enter_interactive) {
        if (name && strcmp(name, "-") != 0) {
            /* Do a single lookup, then enter interactive */
            do_lookup(name);
        }
        interactive_mode();
        return 0;
    }

    return do_lookup(name);
}
