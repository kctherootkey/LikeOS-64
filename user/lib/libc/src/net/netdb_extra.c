// Implements RFC 3493 / 4.4BSD address arithmetic helpers, getprotoby*,
// if_nametoindex / if_indextoname / if_nameindex, and parsers for
// /etc/protocols and /etc/resolv.conf.
//
// Implementation strategy:
//   - inet_network/makeaddr/lnaof/netof use classful boundaries (8/16/24-bit)
//     selected from the high bits of the address per RFC 791 §3.2.
//   - getprotobyname/number reads /etc/protocols if present, else falls back
//     to a tiny built-in table covering the protocols we actually transport.
//   - if_* helpers use ifconfig-style ioctl on a SOCK_DGRAM probe socket.
//
// All returned pointers are owned by the libc and overwritten on the next
// call (POSIX-2008 §3.2 «Concept»: getprotobyname not thread-safe).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

// ===== inet_network / inet_makeaddr / inet_lnaof / inet_netof =====

in_addr_t inet_network(const char* cp) {
    // Like inet_addr() but result is in HOST byte order, and trailing parts
    // collapse (e.g. "10.1" -> 0x0A000001).
    if (!cp) return (in_addr_t)-1;
    uint32_t parts[4] = {0,0,0,0};
    int n = 0, digits = 0;
    while (*cp && n < 4) {
        if (*cp >= '0' && *cp <= '9') {
            parts[n] = parts[n]*10 + (uint32_t)(*cp - '0');
            digits = 1;
        } else if (*cp == '.' && digits) {
            n++; digits = 0;
        } else {
            return (in_addr_t)-1;
        }
        cp++;
    }
    if (digits) n++;
    uint32_t out = 0;
    switch (n) {
    case 1: out = parts[0]; break;
    case 2: out = (parts[0]<<24) | (parts[1] & 0xFFFFFF); break;
    case 3: out = (parts[0]<<24) | ((parts[1]&0xFF)<<16) | (parts[2] & 0xFFFF); break;
    case 4: out = (parts[0]<<24)|((parts[1]&0xFF)<<16)|((parts[2]&0xFF)<<8)|(parts[3]&0xFF); break;
    default: return (in_addr_t)-1;
    }
    return out;
}

static int classful_shift(uint32_t net) {
    // Returns the shift to apply for "host portion" given a classful net.
    if ((net & 0x80000000U) == 0) return 24;          // class A
    if ((net & 0xC0000000U) == 0x80000000U) return 16; // class B
    return 8;                                          // class C/D/E
}

struct in_addr inet_makeaddr(in_addr_t net, in_addr_t host) {
    struct in_addr a;
    int s = classful_shift(net << (32 - classful_shift(net)));   // simplified
    // Use bit width inferred from net's magnitude:
    int width;
    if (net < 0x100)        width = 24;     // A: 8-bit net
    else if (net < 0x10000) width = 16;     // B: 16-bit net
    else                    width = 8;      // C: 24-bit net
    (void)s;
    a.s_addr = htonl((net << width) | (host & ((1U << width) - 1)));
    return a;
}

in_addr_t inet_lnaof(struct in_addr in) {
    uint32_t a = ntohl(in.s_addr);
    int sh = classful_shift(a);
    return a & ((1U << sh) - 1);
}

in_addr_t inet_netof(struct in_addr in) {
    uint32_t a = ntohl(in.s_addr);
    int sh = classful_shift(a);
    return a >> sh;
}

// ===== getprotoby* =====

static struct protoent g_pe;
static char  g_pe_name[32];
static char* g_pe_aliases[1] = { 0 };

static const struct { const char* n; int p; } g_proto_fallback[] = {
    {"ip",   0},
    {"icmp", 1},
    {"igmp", 2},
    {"tcp",  6},
    {"udp",  17},
    {"raw",  255},
    {0, 0}
};

static int parse_protocols_line(char* line, char* name_out, size_t name_sz, int* num_out) {
    // Format: name  number  [aliases...]   # comment
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || *p == 0) return 0;
    char* name = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
    if (!*p) return 0;
    *p++ = 0;
    while (*p == ' ' || *p == '\t') p++;
    char* num = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '#') p++;
    *p = 0;
    int n = atoi(num);
    if (n < 0 || n > 255) return 0;
    size_t nl = strlen(name);
    if (nl >= name_sz) nl = name_sz - 1;
    for (size_t i = 0; i < nl; i++) name_out[i] = name[i];
    name_out[nl] = 0;
    *num_out = n;
    return 1;
}

static struct protoent* lookup_protocols(const char* match_name, int match_num) {
    FILE* f = fopen("/etc/protocols", "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof(line), f)) {
            char nm[32]; int n;
            if (!parse_protocols_line(line, nm, sizeof(nm), &n)) continue;
            int hit = 0;
            if (match_name && strcmp(nm, match_name) == 0) hit = 1;
            if (!match_name && n == match_num) hit = 1;
            if (hit) {
                strncpy(g_pe_name, nm, sizeof(g_pe_name)-1);
                g_pe_name[sizeof(g_pe_name)-1] = 0;
                g_pe.p_name = g_pe_name;
                g_pe.p_aliases = g_pe_aliases;
                g_pe.p_proto = n;
                fclose(f);
                return &g_pe;
            }
        }
        fclose(f);
    }
    for (int i = 0; g_proto_fallback[i].n; i++) {
        if (match_name && strcmp(g_proto_fallback[i].n, match_name) == 0) {
            strncpy(g_pe_name, g_proto_fallback[i].n, sizeof(g_pe_name)-1);
            g_pe_name[sizeof(g_pe_name)-1] = 0;
            g_pe.p_name = g_pe_name;
            g_pe.p_aliases = g_pe_aliases;
            g_pe.p_proto = g_proto_fallback[i].p;
            return &g_pe;
        }
        if (!match_name && g_proto_fallback[i].p == match_num) {
            strncpy(g_pe_name, g_proto_fallback[i].n, sizeof(g_pe_name)-1);
            g_pe_name[sizeof(g_pe_name)-1] = 0;
            g_pe.p_name = g_pe_name;
            g_pe.p_aliases = g_pe_aliases;
            g_pe.p_proto = g_proto_fallback[i].p;
            return &g_pe;
        }
    }
    return 0;
}

struct protoent* getprotobyname(const char* name) {
    if (!name) return 0;
    return lookup_protocols(name, -1);
}
struct protoent* getprotobynumber(int proto) {
    return lookup_protocols(0, proto);
}
void setprotoent(int stayopen) { (void)stayopen; }
void endprotoent(void)         { }

// ===== if_nametoindex / if_indextoname / if_nameindex =====

unsigned int if_nametoindex(const char* ifname) {
    if (!ifname) return 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    int r = ioctl(s, SIOCGIFINDEX, &ifr);
    close(s);
    if (r < 0) return 0;
    return (unsigned int)ifr.ifr_ifindex;
}

char* if_indextoname(unsigned int ifindex, char* ifname) {
    if (!ifname) return 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    // Walk ifindex 0..N-1 and compare returned indices.
    for (unsigned int i = 0; i < 16; i++) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        ifr.ifr_ifindex = (int)i;
        if (ioctl(s, SIOCGIFNAME, &ifr) < 0) continue;
        if ((unsigned int)ifr.ifr_ifindex == ifindex || i == ifindex) {
            strncpy(ifname, ifr.ifr_name, IFNAMSIZ - 1);
            ifname[IFNAMSIZ - 1] = 0;
            close(s);
            return ifname;
        }
    }
    close(s);
    return 0;
}

static struct if_nameindex g_nameindex_storage[16];
static char  g_nameindex_names[16][IFNAMSIZ];

struct if_nameindex* if_nameindex(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    int n = 0;
    for (unsigned int i = 0; i < 16 && n < 15; i++) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        ifr.ifr_ifindex = (int)i;
        if (ioctl(s, SIOCGIFNAME, &ifr) < 0) continue;
        strncpy(g_nameindex_names[n], ifr.ifr_name, IFNAMSIZ - 1);
        g_nameindex_names[n][IFNAMSIZ - 1] = 0;
        g_nameindex_storage[n].if_index = i;
        g_nameindex_storage[n].if_name  = g_nameindex_names[n];
        n++;
    }
    close(s);
    g_nameindex_storage[n].if_index = 0;
    g_nameindex_storage[n].if_name = 0;
    return g_nameindex_storage;
}

void if_freenameindex(struct if_nameindex* ptr) { (void)ptr; }

// ===== res_init: parse /etc/resolv.conf =====
//
// resolver(5) syntax (BSD/POSIX 4.4 style):
//   nameserver <ipv4>
//   search    <domain> [domain ...]
//   domain    <domain>
//   options   timeout:N attempts:N ndots:N
//   sortlist  <ipv4/mask>
//   ;  or  #   comment to end of line
//
// We honour `nameserver` (programs the kernel resolver via set_dns_server)
// and silently accept the rest -- they're advisory for the stub resolver
// and don't affect the kernel.

int res_init(void) {
    FILE* f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;
    int installed = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == 0) continue;
        // Tokenize first word.
        char* kw = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (!*p) continue;
        *p++ = 0;
        if (strcmp(kw, "nameserver") != 0) continue;
        while (*p == ' ' || *p == '\t') p++;
        char* val = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '#'
               && *p != ';') p++;
        *p = 0;
        struct in_addr a;
        if (inet_aton(val, &a) == 0) continue;        // skip IPv6 / garbage
        if (set_dns_server(NULL, a.s_addr) == 0) installed++;
    }
    fclose(f);
    return installed;
}
