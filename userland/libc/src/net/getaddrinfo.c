// getaddrinfo / freeaddrinfo / gai_strerror / gethostbyname /
// getservbyname / getservbyport — minimal RFC 3493 implementation
// using on-system /etc/services + /etc/hosts and the kernel DNS resolver.

#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

// ---------- error strings -----------------------------------------------
const char *gai_strerror(int e) {
    switch (e) {
    case 0:             return "Success";
    case EAI_BADFLAGS:  return "Bad ai_flags";
    case EAI_NONAME:    return "Name or service not known";
    case EAI_AGAIN:     return "Temporary failure in name resolution";
    case EAI_FAIL:      return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:    return "Address family not supported";
    case EAI_SOCKTYPE:  return "Socket type not supported";
    case EAI_SERVICE:   return "Service not supported for socket type";
    case EAI_MEMORY:    return "Out of memory";
    case EAI_SYSTEM:    return "System error";
    case EAI_OVERFLOW:  return "Buffer overflow";
    default:            return "Unknown error";
    }
}

// ---------- /etc/services lookup ----------------------------------------
// Scans /etc/services for "name port/proto".  Returns port in host byte order
// or -1 if not found.
static int services_lookup(const char *name, const char *proto) {
    int fd = open("/etc/services", O_RDONLY);
    if (fd < 0) return -1;
    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;

        // Strip comments, skip blanks.
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) {
            char svc[64]; int si = 0;
            while (*p && *p != ' ' && *p != '\t' && si < 63) svc[si++] = *p++;
            svc[si] = 0;
            while (*p == ' ' || *p == '\t') p++;
            int port = 0;
            while (*p >= '0' && *p <= '9') port = port*10 + (*p++ - '0');
            char pr[16]; int pri = 0;
            if (*p == '/') {
                p++;
                while (*p && *p != ' ' && *p != '\t' && pri < 15) pr[pri++] = *p++;
            }
            pr[pri] = 0;
            if (strcmp(svc, name) == 0 && (!proto || strcmp(pr, proto) == 0))
                return port;
        }

        if (!eol) break;
        line = eol + 1;
    }
    return -1;
}

// ---------- /etc/hosts lookup -------------------------------------------
// Returns 1 on hit, 0 on miss.
static int hosts_lookup(const char *name, uint32_t *ip_out) {
    // Built-in localhost (RFC 6761).
    if (strcmp(name, "localhost") == 0 ||
        strcmp(name, "localhost.localdomain") == 0) {
        *ip_out = htonl(INADDR_LOOPBACK);
        return 1;
    }
    int fd = open("/etc/hosts", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (*p) {
            char addr[64]; int ai = 0;
            while (*p && *p != ' ' && *p != '\t' && ai < 63) addr[ai++] = *p++;
            addr[ai] = 0;
            struct in_addr a;
            if (inet_aton(addr, &a)) {
                while (*p == ' ' || *p == '\t') p++;
                while (*p) {
                    char tok[256]; int ti = 0;
                    while (*p && *p != ' ' && *p != '\t' && ti < 255) tok[ti++] = *p++;
                    tok[ti] = 0;
                    if (ti && strcmp(tok, name) == 0) {
                        *ip_out = a.s_addr;
                        return 1;
                    }
                    while (*p == ' ' || *p == '\t') p++;
                }
            }
        }

        if (!eol) break;
        line = eol + 1;
    }
    return 0;
}

// ---------- getservby{name,port} ----------------------------------------
struct servent *getservbyname(const char *name, const char *proto) {
    static struct servent se;
    static char      sname[64], sproto[16];
    static char     *aliases_null = 0;

    int port = services_lookup(name, proto);
    if (port < 0) return 0;

    int n = 0; while (name[n] && n < 63) { sname[n] = name[n]; n++; } sname[n] = 0;
    n = 0; if (proto) { while (proto[n] && n < 15) { sproto[n] = proto[n]; n++; } }
    sproto[n] = 0;

    se.s_name    = sname;
    se.s_aliases = &aliases_null;
    se.s_port    = htons((uint16_t)port);   // RFC 3493: network byte order
    se.s_proto   = sproto;
    return &se;
}

struct servent *getservbyport(int port, const char *proto) {
    (void)port; (void)proto;
    return 0;  // not implemented
}

// ---------- gethostbyname -----------------------------------------------
struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static char       hname[256];
    static uint32_t   addr;
    static char      *addr_list[2];
    static char      *aliases_null = 0;

    uint32_t ip = 0;
    if (inet_aton(name, (struct in_addr*)&ip)) {
        addr = ip;  // already network order
    } else if (hosts_lookup(name, &ip)) {
        addr = ip;
    } else {
        // Fall back to the kernel DNS resolver which returns host byte order.
        // Lazily program /etc/resolv.conf nameservers on first miss so the
        // kernel has a recursive resolver to talk to before DHCP completes.
        static int resolv_loaded = 0;
        if (!resolv_loaded) { res_init(); resolv_loaded = 1; }
        if (dns_resolve(name, &ip) < 0) return 0;
        addr = htonl(ip);
    }

    int n = 0; while (name[n] && n < 255) { hname[n] = name[n]; n++; } hname[n] = 0;
    addr_list[0] = (char*)&addr;
    addr_list[1] = 0;

    he.h_name      = hname;
    he.h_aliases   = &aliases_null;
    he.h_addrtype  = AF_INET;
    he.h_length    = 4;
    he.h_addr_list = addr_list;
    return &he;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
    (void)addr; (void)len; (void)type;
    return 0;  // not implemented
}

// ---------- getaddrinfo / freeaddrinfo ----------------------------------
static struct addrinfo *ai_alloc(int socktype, int protocol,
                                  uint32_t ip_nbo, uint16_t port_nbo) {
    struct addrinfo *ai = malloc(sizeof(*ai) + sizeof(struct sockaddr_in));
    if (!ai) return 0;
    struct sockaddr_in *sa = (struct sockaddr_in*)(ai + 1);
    memset(ai, 0, sizeof(*ai));
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port   = port_nbo;
    sa->sin_addr.s_addr = ip_nbo;
    ai->ai_family   = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen  = sizeof(struct sockaddr_in);
    ai->ai_addr     = (struct sockaddr*)sa;
    return ai;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *n = res->ai_next;
        if (res->ai_canonname) free(res->ai_canonname);
        free(res);
        res = n;
    }
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_FAIL;
    *res = 0;

    int family    = hints ? hints->ai_family   : AF_UNSPEC;
    int socktype  = hints ? hints->ai_socktype : 0;
    int protocol  = hints ? hints->ai_protocol : 0;
    int flags     = hints ? hints->ai_flags    : 0;

    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    if (!node && !service) return EAI_NONAME;

    // Resolve service -> port (network byte order).
    uint16_t port_nbo = 0;
    if (service) {
        int allnum = 1;
        for (const char *p = service; *p; p++)
            if (*p < '0' || *p > '9') { allnum = 0; break; }
        if (allnum) {
            int p = 0;
            for (const char *q = service; *q; q++) p = p*10 + (*q - '0');
            port_nbo = htons((uint16_t)p);
        } else {
            const char *proto = 0;
            if (socktype == SOCK_STREAM) proto = "tcp";
            else if (socktype == SOCK_DGRAM) proto = "udp";
            int p = services_lookup(service, proto);
            if (p < 0 && proto) p = services_lookup(service, 0);
            if (p < 0) return EAI_SERVICE;
            port_nbo = htons((uint16_t)p);
        }
    }

    // Resolve node -> address.
    uint32_t ip_nbo = 0;
    if (!node) {
        ip_nbo = (flags & AI_PASSIVE) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    } else if (inet_aton(node, (struct in_addr*)&ip_nbo)) {
        // already nbo
    } else if (flags & AI_NUMERICHOST) {
        return EAI_NONAME;
    } else {
        uint32_t ip_h = 0;
        if (hosts_lookup(node, &ip_nbo)) {
            // already nbo
        } else {
            static int resolv_loaded2 = 0;
            if (!resolv_loaded2) { res_init(); resolv_loaded2 = 1; }
            if (dns_resolve(node, &ip_h) == 0) {
                ip_nbo = htonl(ip_h);
            } else {
                return EAI_NONAME;
            }
        }
    }

    int sts[2]; int nst = 0;
    if (socktype == 0) {
        sts[nst++] = SOCK_STREAM;
        sts[nst++] = SOCK_DGRAM;
    } else {
        sts[nst++] = socktype;
    }

    struct addrinfo *head = 0, *tail = 0;
    for (int i = 0; i < nst; i++) {
        int proto = protocol;
        if (proto == 0) proto = (sts[i] == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
        struct addrinfo *ai = ai_alloc(sts[i], proto, ip_nbo, port_nbo);
        if (!ai) { freeaddrinfo(head); return EAI_MEMORY; }
        if (!head) head = ai; else tail->ai_next = ai;
        tail = ai;
    }

    if ((flags & AI_CANONNAME) && node && head) {
        size_t l = strlen(node);
        head->ai_canonname = malloc(l + 1);
        if (head->ai_canonname) memcpy(head->ai_canonname, node, l + 1);
    }

    *res = head;
    return 0;
}

// ---------- getnameinfo (numeric only) ----------------------------------
int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags) {
    if (!sa || sa->sa_family != AF_INET || salen < (socklen_t)sizeof(struct sockaddr_in))
        return EAI_FAMILY;
    (void)flags;
    const struct sockaddr_in *si = (const struct sockaddr_in*)sa;
    if (host && hostlen) {
        if (!inet_ntop(AF_INET, &si->sin_addr, host, hostlen))
            return EAI_OVERFLOW;
    }
    if (serv && servlen) {
        unsigned p = ntohs(si->sin_port);
        // simple itoa
        char tmp[8]; int n = 0;
        if (p == 0) tmp[n++] = '0';
        else while (p) { tmp[n++] = '0' + p%10; p /= 10; }
        if ((socklen_t)(n+1) > servlen) return EAI_OVERFLOW;
        for (int i = 0; i < n; i++) serv[i] = tmp[n-1-i];
        serv[n] = 0;
    }
    return 0;
}
