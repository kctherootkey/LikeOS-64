// LikeOS-64 DNS Resolver
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/random.h"

// DNS constants
#define DNS_PORT            53
#define DNS_CLIENT_PORT     5353    // Our fixed source port for kernel DNS
#define DNS_MAX_NAME        255
#define DNS_MAX_PACKET      512
#define DNS_TIMEOUT_MS      3000
#define DNS_MAX_RETRIES     3

// DNS header flags
#define DNS_FLAG_QR         0x8000  // Response
#define DNS_FLAG_OPCODE     0x7800  // Opcode mask
#define DNS_FLAG_AA         0x0400  // Authoritative
#define DNS_FLAG_TC         0x0200  // Truncated
#define DNS_FLAG_RD         0x0100  // Recursion desired
#define DNS_FLAG_RA         0x0080  // Recursion available
#define DNS_FLAG_RCODE      0x000F  // Response code mask

// DNS record types
#define DNS_TYPE_A          1
#define DNS_TYPE_CNAME      5
#define DNS_CLASS_IN        1

// DNS header (12 bytes)
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

// DNS cache entry
#define DNS_CACHE_SIZE 16
typedef struct {
    char        hostname[64];
    uint32_t    ip;
    uint64_t    expire_tick;    // When this entry expires
    int         valid;
} dns_cache_entry_t;

static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];
static int dns_cache_next = 0;  // Next slot to use (round-robin)

// Response buffer for async receive
static uint8_t dns_rx_buf[DNS_MAX_PACKET];
static volatile int dns_rx_len = 0;
static volatile uint16_t dns_rx_id = 0;
static volatile int dns_rx_ready = 0;

// ============================================================================
// String helpers
// ============================================================================
static int dns_strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int dns_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void dns_strcpy(char* dst, const char* src, int maxlen) {
    int i = 0;
    while (src[i] && i < maxlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// ============================================================================
// DNS cache
// ============================================================================
static int dns_cache_lookup(const char* hostname, uint32_t* ip_out) {
    uint64_t now = timer_ticks();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) continue;
        if (dns_cache[i].expire_tick < now) {
            dns_cache[i].valid = 0;
            continue;
        }
        if (dns_strcmp(dns_cache[i].hostname, hostname) == 0) {
            *ip_out = dns_cache[i].ip;
            return 0;
        }
    }
    return -1;
}

static void dns_cache_insert(const char* hostname, uint32_t ip, uint32_t ttl) {
    // Minimum TTL = 60s, maximum = 86400s (1 day)
    if (ttl < 60) ttl = 60;
    if (ttl > 86400) ttl = 86400;

    // Check for update first
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && dns_strcmp(dns_cache[i].hostname, hostname) == 0) {
            dns_cache[i].ip = ip;
            dns_cache[i].expire_tick = timer_ticks() + (uint64_t)ttl * 1000;
            return;
        }
    }

    // Insert at next slot (round-robin)
    dns_cache_entry_t* e = &dns_cache[dns_cache_next];
    dns_strcpy(e->hostname, hostname, 64);
    e->ip = ip;
    e->expire_tick = timer_ticks() + (uint64_t)ttl * 1000;
    e->valid = 1;
    dns_cache_next = (dns_cache_next + 1) % DNS_CACHE_SIZE;
}

// ============================================================================
// DNS packet encoding
// ============================================================================

// Encode hostname into DNS wire format (labels)
// e.g., "www.example.com" -> "\3www\7example\3com\0"
static int dns_encode_name(const char* name, uint8_t* buf, int buflen) {
    int namelen = dns_strlen(name);
    if (namelen == 0 || namelen >= DNS_MAX_NAME) return -1;

    int pos = 0;
    int label_start = 0;

    for (int i = 0; i <= namelen; i++) {
        if (i == namelen || name[i] == '.') {
            int label_len = i - label_start;
            if (label_len == 0 || label_len > 63) return -1;
            if (pos + 1 + label_len >= buflen) return -1;
            buf[pos++] = (uint8_t)label_len;
            for (int j = label_start; j < i; j++)
                buf[pos++] = (uint8_t)name[j];
            label_start = i + 1;
        }
    }
    if (pos >= buflen) return -1;
    buf[pos++] = 0;  // Root label
    return pos;
}

// Build a DNS query packet
static int dns_build_query(const char* hostname, uint16_t query_id,
                           uint8_t* buf, int buflen) {
    if (buflen < (int)sizeof(dns_header_t) + 4) return -1;

    // Header
    dns_header_t* hdr = (dns_header_t*)buf;
    hdr->id = net_htons(query_id);
    hdr->flags = net_htons(DNS_FLAG_RD);  // Recursion desired
    hdr->qdcount = net_htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    int pos = sizeof(dns_header_t);

    // Question: encoded name + type (A) + class (IN)
    int name_len = dns_encode_name(hostname, buf + pos, buflen - pos);
    if (name_len < 0) return -1;
    pos += name_len;

    if (pos + 4 > buflen) return -1;
    buf[pos++] = 0; buf[pos++] = DNS_TYPE_A;     // QTYPE = A
    buf[pos++] = 0; buf[pos++] = DNS_CLASS_IN;   // QCLASS = IN

    return pos;
}

// ============================================================================
// DNS response parsing
// ============================================================================

// Skip a DNS name (handles compression pointers)
static int dns_skip_name(const uint8_t* pkt, int pktlen, int offset) {
    int pos = offset;
    while (pos < pktlen) {
        uint8_t len = pkt[pos];
        if (len == 0) { pos++; break; }
        if ((len & 0xC0) == 0xC0) {
            // Compression pointer — 2 bytes
            pos += 2;
            break;
        }
        pos += 1 + len;
    }
    return pos;
}

// Parse DNS response for A record
static int dns_parse_response(const uint8_t* pkt, int pktlen,
                              uint16_t expected_id,
                              uint32_t* ip_out, uint32_t* ttl_out) {
    if (pktlen < (int)sizeof(dns_header_t)) return -1;

    const dns_header_t* hdr = (const dns_header_t*)pkt;

    // Verify response
    uint16_t id = net_ntohs(hdr->id);
    uint16_t flags = net_ntohs(hdr->flags);
    uint16_t qdcount = net_ntohs(hdr->qdcount);
    uint16_t ancount = net_ntohs(hdr->ancount);

    if (id != expected_id) return -1;
    if (!(flags & DNS_FLAG_QR)) return -1;  // Not a response
    if ((flags & DNS_FLAG_RCODE) != 0) return -1;  // Error in response

    // Skip question section
    int pos = sizeof(dns_header_t);
    for (uint16_t i = 0; i < qdcount; i++) {
        pos = dns_skip_name(pkt, pktlen, pos);
        pos += 4;  // QTYPE + QCLASS
        if (pos > pktlen) return -1;
    }

    // Parse answer section — look for A record
    for (uint16_t i = 0; i < ancount; i++) {
        pos = dns_skip_name(pkt, pktlen, pos);
        if (pos + 10 > pktlen) return -1;

        uint16_t rtype = ((uint16_t)pkt[pos] << 8) | pkt[pos + 1];
        uint16_t rclass = ((uint16_t)pkt[pos + 2] << 8) | pkt[pos + 3];
        uint32_t rttl = ((uint32_t)pkt[pos + 4] << 24) |
                         ((uint32_t)pkt[pos + 5] << 16) |
                         ((uint32_t)pkt[pos + 6] << 8) |
                         (uint32_t)pkt[pos + 7];
        uint16_t rdlen = ((uint16_t)pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;

        if (pos + rdlen > pktlen) return -1;

        if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN && rdlen == 4) {
            // Found an A record
            *ip_out = ((uint32_t)pkt[pos] << 24) |
                      ((uint32_t)pkt[pos + 1] << 16) |
                      ((uint32_t)pkt[pos + 2] << 8) |
                      (uint32_t)pkt[pos + 3];
            *ttl_out = rttl;
            return 0;
        }

        pos += rdlen;  // Skip RDATA (e.g., CNAME records)
    }

    return -1;  // No A record found
}

// ============================================================================
// dns_rx - Called by UDP layer when a DNS response arrives (port DNS_CLIENT_PORT)
// ============================================================================
void dns_rx(const uint8_t* data, uint16_t len) {
    if (len < (int)sizeof(dns_header_t) || len > DNS_MAX_PACKET) return;

    const dns_header_t* hdr = (const dns_header_t*)data;
    uint16_t id = net_ntohs(hdr->id);
    uint16_t flags = net_ntohs(hdr->flags);

    // Must be a response
    if (!(flags & DNS_FLAG_QR)) return;

    // Copy to response buffer
    for (int i = 0; i < len; i++)
        dns_rx_buf[i] = data[i];
    dns_rx_len = len;
    dns_rx_id = id;
    dns_rx_ready = 1;
}

// ============================================================================
// dns_resolve - Resolve a hostname to an IPv4 address
// ============================================================================
int dns_resolve(const char* hostname, uint32_t* ip_out) {
    if (!hostname || !ip_out) return -EINVAL;

    // Handle numeric IP addresses (a.b.c.d)
    {
        uint32_t parts[4];
        int nparts = 0;
        uint32_t val = 0;
        int has_digit = 0;
        int is_numeric = 1;

        for (int i = 0; hostname[i]; i++) {
            char c = hostname[i];
            if (c >= '0' && c <= '9') {
                val = val * 10 + (uint32_t)(c - '0');
                if (val > 255) { is_numeric = 0; break; }
                has_digit = 1;
            } else if (c == '.') {
                if (!has_digit || nparts >= 3) { is_numeric = 0; break; }
                parts[nparts++] = val;
                val = 0;
                has_digit = 0;
            } else {
                is_numeric = 0;
                break;
            }
        }
        if (is_numeric && has_digit && nparts == 3) {
            parts[nparts] = val;
            *ip_out = (parts[0] << 24) | (parts[1] << 16) |
                      (parts[2] << 8) | parts[3];
            return 0;
        }
    }

    // Handle "localhost"
    if (dns_strcmp(hostname, "localhost") == 0) {
        *ip_out = 0x7F000001;  // 127.0.0.1
        return 0;
    }

    // Check cache
    if (dns_cache_lookup(hostname, ip_out) == 0) {
        return 0;
    }

    // Get DNS server
    net_device_t* dev = net_get_default_device();
    if (!dev) return -ENETDOWN;
    uint32_t dns_server = dev->dns_server;
    if (dns_server == 0) return -ENETUNREACH;

    // Build query
    uint8_t query_buf[DNS_MAX_PACKET];
    uint16_t query_id = (uint16_t)random_u32();

    int query_len = dns_build_query(hostname, query_id, query_buf, DNS_MAX_PACKET);
    if (query_len < 0) return -EINVAL;

    // Send query and wait for response
    for (int retry = 0; retry < DNS_MAX_RETRIES; retry++) {
        dns_rx_ready = 0;
        dns_rx_len = 0;

        int ret = udp_send(dev, dns_server, DNS_CLIENT_PORT, DNS_PORT,
                           query_buf, (uint16_t)query_len);
        if (ret < 0) return ret;

        // Wait for response with timeout
        uint64_t start = timer_ticks();
        while (!dns_rx_ready) {
            if (timer_ticks() - start > DNS_TIMEOUT_MS) break;
            // Yield CPU — simplified busy-wait with pause
            __asm__ volatile("pause");
        }

        if (!dns_rx_ready) continue;  // Timeout, retry
        if (dns_rx_id != query_id) continue;  // Wrong response, retry

        // Parse response
        uint32_t ip = 0, ttl = 0;
        if (dns_parse_response(dns_rx_buf, dns_rx_len, query_id,
                               &ip, &ttl) == 0) {
            *ip_out = ip;
            dns_cache_insert(hostname, ip, ttl);
            return 0;
        }
        // Parse failed — don't retry, it's a definitive error
        return -ENOENT;
    }

    return -ETIMEDOUT;
}

// ============================================================================
// dns_init - Initialize DNS resolver
// ============================================================================
void dns_init(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_cache[i].valid = 0;
    }
    dns_rx_ready = 0;
}
