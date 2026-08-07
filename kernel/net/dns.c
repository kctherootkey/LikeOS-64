// LikeOS-64 DNS Resolver
#include <kernel/net/net.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/timer.h>
#include <kernel/dev/rand/random.h>
#include <kernel/uapi/bug.h>

// DNS constants
#define DNS_PORT 53
#define DNS_CLIENT_PORT 5353 // Our fixed source port for kernel DNS
#define DNS_MAX_NAME 255
#define DNS_MAX_PACKET 512
#define DNS_TIMEOUT_MS 3000
#define DNS_MAX_RETRIES 3

// DNS header flags
#define DNS_FLAG_QR 0x8000 // Response
#define DNS_FLAG_OPCODE 0x7800 // Opcode mask
#define DNS_FLAG_AA 0x0400 // Authoritative
#define DNS_FLAG_TC 0x0200 // Truncated
#define DNS_FLAG_RD 0x0100 // Recursion desired
#define DNS_FLAG_RA 0x0080 // Recursion available
#define DNS_FLAG_RCODE 0x000F // Response code mask

// DNS record types
#define DNS_TYPE_A 1
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_PTR 12
#define DNS_CLASS_IN 1

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
	char hostname[64];
	uint32_t ip;
	uint64_t expire_tick; // When this entry expires
	int valid;
} dns_cache_entry_t;

static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];
static int dns_cache_next = 0; // Next slot to use (round-robin)

// Response buffer for async receive.
//
// dns_rx() fills this from the network receive path while a resolver is
// spinning on dns_rx_ready in task context, so the id and the buffer MUST be
// read together, under dns_lock.  Reading the id, deciding the buffer is ours,
// and only then copying it out is a check-then-use race: another lookup's reply
// can land in between and get copied out as our answer.  A reverse lookup
// answering NXDOMAIN (routine, and netstat/ping do them constantly) then
// surfaces as NXDOMAIN for a perfectly good name.
static uint8_t dns_rx_buf[DNS_MAX_PACKET];
static int dns_rx_len = 0;
static uint16_t dns_rx_id = 0;
static volatile int dns_rx_ready = 0;
static spinlock_t dns_lock = SPINLOCK_INIT("dns");

/* Does `resp` answer the question `query` asked?
 *
 * Matching on the 16-bit transaction id alone is not enough.  The resolver
 * uses ONE fixed source port (DNS_CLIENT_PORT) for every lookup in the system,
 * so a late straggler — a reply to a query that already timed out and retried,
 * or to one whose process has since exited — can arrive while an unrelated
 * lookup is waiting.  If the two ids happen to collide (1 in 65536), that reply
 * is accepted as ours.  A reverse (PTR) lookup answering NXDOMAIN is routine,
 * so the straggler is quite likely to BE an NXDOMAIN — and a perfectly good
 * name comes back "not found".  Rare, unreproducible, and exactly what was
 * observed under teststress.
 *
 * A response echoes the question section verbatim (it is never compressed), so
 * comparing it byte-for-byte against the query we built settles it: same name,
 * same qtype, same qclass. */
static int dns_answers_our_question(const uint8_t *resp, int resp_len,
				    const uint8_t *query, int query_len)
{
	int qlen = query_len - (int)sizeof(dns_header_t);
	if (qlen <= 0 || resp_len < query_len)
		return 0;
	const dns_header_t *rh = (const dns_header_t *)resp;
	if (net_ntohs(rh->qdcount) != 1)
		return 0; /* not the single question we asked */
	for (int i = 0; i < qlen; i++)
		if (resp[sizeof(dns_header_t) + i] !=
		    query[sizeof(dns_header_t) + i])
			return 0;
	return 1;
}

// ============================================================================
// String helpers
// ============================================================================
static int dns_strlen(const char *s)
{
	int len = 0;
	while (s[len])
		len++;
	return len;
}

static int dns_strcmp(const char *a, const char *b)
{
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static void dns_strcpy(char *dst, const char *src, int maxlen)
{
	int i = 0;
	while (src[i] && i < maxlen - 1) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

// ============================================================================
// DNS cache
// ============================================================================
static int dns_cache_lookup(const char *hostname, uint32_t *ip_out)
{
	BUG_ON(hostname == NULL);
	uint64_t now = timer_ticks();
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		if (!dns_cache[i].valid)
			continue;
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

static void dns_cache_insert(const char *hostname, uint32_t ip, uint32_t ttl)
{
	// Minimum TTL = 60s, maximum = 86400s (1 day)
	if (ttl < 60)
		ttl = 60;
	if (ttl > 86400)
		ttl = 86400;

	// Check for update first
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		if (dns_cache[i].valid &&
		    dns_strcmp(dns_cache[i].hostname, hostname) == 0) {
			dns_cache[i].ip = ip;
			dns_cache[i].expire_tick =
				timer_ticks() + timer_s_to_ticks(ttl);
			return;
		}
	}

	// Insert at next slot (round-robin)
	dns_cache_entry_t *e = &dns_cache[dns_cache_next];
	dns_strcpy(e->hostname, hostname, 64);
	e->ip = ip;
	/* The TTL is in SECONDS.  `ttl * 1000' was neither seconds nor ticks: at
	 * 100Hz it held every entry for ten times its TTL, and the factor moved
	 * again with the calibrated tick rate. */
	e->expire_tick = timer_ticks() + timer_s_to_ticks(ttl);
	e->valid = 1;
	dns_cache_next = (dns_cache_next + 1) % DNS_CACHE_SIZE;
}

// ============================================================================
// DNS packet encoding
// ============================================================================

// Encode hostname into DNS wire format (labels)
// e.g., "www.example.com" -> "\3www\7example\3com\0"
static int dns_encode_name(const char *name, uint8_t *buf, int buflen)
{
	int namelen = dns_strlen(name);
	if (namelen == 0 || namelen >= DNS_MAX_NAME)
		return -1;

	int pos = 0;
	int label_start = 0;

	for (int i = 0; i <= namelen; i++) {
		if (i == namelen || name[i] == '.') {
			int label_len = i - label_start;
			if (label_len == 0 || label_len > 63)
				return -1;
			if (pos + 1 + label_len >= buflen)
				return -1;
			buf[pos++] = (uint8_t)label_len;
			for (int j = label_start; j < i; j++)
				buf[pos++] = (uint8_t)name[j];
			label_start = i + 1;
		}
	}
	if (pos >= buflen)
		return -1;
	buf[pos++] = 0; // Root label
	return pos;
}

// Build a DNS query packet
static int dns_build_query(const char *hostname, uint16_t query_id,
			   uint8_t *buf, int buflen)
{
	if (buflen < (int)sizeof(dns_header_t) + 4)
		return -1;

	// Header
	dns_header_t *hdr = (dns_header_t *)buf;
	hdr->id = net_htons(query_id);
	hdr->flags = net_htons(DNS_FLAG_RD); // Recursion desired
	hdr->qdcount = net_htons(1);
	hdr->ancount = 0;
	hdr->nscount = 0;
	hdr->arcount = 0;

	int pos = sizeof(dns_header_t);

	// Question: encoded name + type (A) + class (IN)
	int name_len = dns_encode_name(hostname, buf + pos, buflen - pos);
	if (name_len < 0)
		return -1;
	pos += name_len;

	if (pos + 4 > buflen)
		return -1;
	buf[pos++] = 0;
	buf[pos++] = DNS_TYPE_A; // QTYPE = A
	buf[pos++] = 0;
	buf[pos++] = DNS_CLASS_IN; // QCLASS = IN

	return pos;
}

// ============================================================================
// DNS response parsing
// ============================================================================

// Skip a DNS name (handles compression pointers)
static int dns_skip_name(const uint8_t *pkt, int pktlen, int offset)
{
	int pos = offset;
	while (pos < pktlen) {
		uint8_t len = pkt[pos];
		if (len == 0) {
			pos++;
			break;
		}
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
static int dns_parse_response(const uint8_t *pkt, int pktlen,
			      uint16_t expected_id, uint32_t *ip_out,
			      uint32_t *ttl_out)
{
	if (pktlen < (int)sizeof(dns_header_t))
		return -1;

	const dns_header_t *hdr = (const dns_header_t *)pkt;

	// Verify response
	uint16_t id = net_ntohs(hdr->id);
	uint16_t flags = net_ntohs(hdr->flags);
	uint16_t qdcount = net_ntohs(hdr->qdcount);
	uint16_t ancount = net_ntohs(hdr->ancount);
	WARN_RATELIMIT(ancount > 64,
		       "dns_parse_response: suspicious ancount=%u", ancount);
	if (!(flags & DNS_FLAG_QR))
		return -1; // Not a response
	if ((flags & DNS_FLAG_RCODE) != 0)
		return -1; // Error in response

	// Skip question section
	int pos = sizeof(dns_header_t);
	for (uint16_t i = 0; i < qdcount; i++) {
		pos = dns_skip_name(pkt, pktlen, pos);
		pos += 4; // QTYPE + QCLASS
		if (pos > pktlen)
			return -1;
	}

	// Parse answer section — look for A record
	for (uint16_t i = 0; i < ancount; i++) {
		pos = dns_skip_name(pkt, pktlen, pos);
		if (pos + 10 > pktlen)
			return -1;

		uint16_t rtype = ((uint16_t)pkt[pos] << 8) | pkt[pos + 1];
		uint16_t rclass = ((uint16_t)pkt[pos + 2] << 8) | pkt[pos + 3];
		uint32_t rttl = ((uint32_t)pkt[pos + 4] << 24) |
				((uint32_t)pkt[pos + 5] << 16) |
				((uint32_t)pkt[pos + 6] << 8) |
				(uint32_t)pkt[pos + 7];
		uint16_t rdlen = ((uint16_t)pkt[pos + 8] << 8) | pkt[pos + 9];
		pos += 10;

		if (pos + rdlen > pktlen)
			return -1;

		WARN_RATELIMIT(
			rtype == DNS_TYPE_A && rdlen != 4,
			"dns_parse_response: A record with rdlen=%u (expected 4)",
			rdlen);
		if (rtype == DNS_TYPE_A && rclass == DNS_CLASS_IN &&
		    rdlen == 4) {
			// Found an A record
			*ip_out = ((uint32_t)pkt[pos] << 24) |
				  ((uint32_t)pkt[pos + 1] << 16) |
				  ((uint32_t)pkt[pos + 2] << 8) |
				  (uint32_t)pkt[pos + 3];
			*ttl_out = rttl;
			return 0;
		}

		pos += rdlen; // Skip RDATA (e.g., CNAME records)
	}

	return -1; // No A record found
}

// ============================================================================
// dns_rx - Called by UDP layer when a DNS response arrives (port DNS_CLIENT_PORT)
// ============================================================================
void dns_rx(const uint8_t *data, uint16_t len)
{
	BUG_ON(data == NULL);
	if (len < (int)sizeof(dns_header_t) || len > DNS_MAX_PACKET)
		return;

	const dns_header_t *hdr = (const dns_header_t *)data;
	uint16_t id = net_ntohs(hdr->id);
	uint16_t flags = net_ntohs(hdr->flags);

	// Must be a response
	if (!(flags & DNS_FLAG_QR))
		return;

	// Copy to response buffer.  Under dns_lock so a resolver cannot observe
	// this id paired with a different reply's bytes.
	uint64_t lflags;
	spin_lock_irqsave(&dns_lock, &lflags);
	for (int i = 0; i < len; i++)
		dns_rx_buf[i] = data[i];
	dns_rx_len = len;
	dns_rx_id = id;
	dns_rx_ready = 1;
	spin_unlock_irqrestore(&dns_lock, lflags);
}

// ============================================================================
// dns_resolve - Resolve a hostname to an IPv4 address
// ============================================================================
int dns_resolve(const char *hostname, uint32_t *ip_out)
{
	BUG_ON(hostname == NULL);
	BUG_ON(ip_out == NULL);
	BUILD_BUG_ON(sizeof(dns_header_t) != 12);
	might_sleep();
	if (!hostname || !ip_out)
		return -EINVAL;

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
				if (val > 255) {
					is_numeric = 0;
					break;
				}
				has_digit = 1;
			} else if (c == '.') {
				if (!has_digit || nparts >= 3) {
					is_numeric = 0;
					break;
				}
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
		*ip_out = 0x7F000001; // 127.0.0.1
		return 0;
	}

	// Check cache
	if (dns_cache_lookup(hostname, ip_out) == 0) {
		return 0;
	}

	// Get DNS server
	net_device_t *dev = net_get_default_device();
	if (!dev)
		return -ENETDOWN;
	uint32_t dns_server = dev->dns_server;
	if (dns_server == 0)
		return -ENETUNREACH;

	// Build query
	uint8_t query_buf[DNS_MAX_PACKET];
	uint16_t query_id = (uint16_t)random_u32();

	int query_len =
		dns_build_query(hostname, query_id, query_buf, DNS_MAX_PACKET);
	if (query_len < 0)
		return -EINVAL;

	// Send query and wait for response
	for (int retry = 0; retry < DNS_MAX_RETRIES; retry++) {
		/* Arm for this attempt under the lock: an unlocked reset can
		 * clobber a reply dns_rx() is landing right now, leaving
		 * ready=1 with len=0. */
		uint64_t aflags;
		spin_lock_irqsave(&dns_lock, &aflags);
		dns_rx_ready = 0;
		dns_rx_len = 0;
		spin_unlock_irqrestore(&dns_lock, aflags);

		int ret = udp_send(dev, dns_server, DNS_CLIENT_PORT, DNS_PORT,
				   query_buf, (uint16_t)query_len);
		if (ret < 0)
			return ret;

		// Wait for response with timeout
		uint64_t start = timer_ticks();
		while (!dns_rx_ready) {
			if (timer_ticks() - start > DNS_TIMEOUT_MS)
				break;
			// Yield CPU — simplified busy-wait with pause
			__asm__ volatile("pause");
		}

		if (!dns_rx_ready)
			continue; // Timeout, retry

		/* Take the id and the bytes together, then work only from the
		 * snapshot.  Checking dns_rx_id and afterwards parsing the live
		 * dns_rx_buf lets a reply that arrives in between be parsed as
		 * ours. */
		uint8_t snap[DNS_MAX_PACKET];
		int snap_len;
		uint16_t snap_id;
		uint64_t lflags;
		spin_lock_irqsave(&dns_lock, &lflags);
		snap_len = dns_rx_len;
		snap_id = dns_rx_id;
		if (snap_len < 0)
			snap_len = 0;
		if (snap_len > DNS_MAX_PACKET)
			snap_len = DNS_MAX_PACKET;
		for (int i = 0; i < snap_len; i++)
			snap[i] = dns_rx_buf[i];
		spin_unlock_irqrestore(&dns_lock, lflags);

		if (snap_id != query_id)
			continue; // Wrong response, retry
		if (!dns_answers_our_question(snap, snap_len, query_buf,
					      query_len))
			continue; // id collided with a straggler — not ours

		// Parse response
		uint32_t ip = 0, ttl = 0;
		if (dns_parse_response(snap, snap_len, query_id, &ip, &ttl) ==
		    0) {
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
void dns_init(void)
{
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		dns_cache[i].valid = 0;
	}
	dns_rx_ready = 0;
}

// ============================================================================
// dns_query_raw - Send a DNS query and return raw response for userspace
// ============================================================================

// Build a DNS query with arbitrary record type
static int dns_build_query_type(const char *name, uint16_t qtype,
				uint16_t query_id, uint8_t *buf, int buflen)
{
	if (buflen < (int)sizeof(dns_header_t) + 4)
		return -1;

	dns_header_t *hdr = (dns_header_t *)buf;
	hdr->id = net_htons(query_id);
	hdr->flags = net_htons(DNS_FLAG_RD);
	hdr->qdcount = net_htons(1);
	hdr->ancount = 0;
	hdr->nscount = 0;
	hdr->arcount = 0;

	int pos = sizeof(dns_header_t);
	int name_len = dns_encode_name(name, buf + pos, buflen - pos);
	if (name_len < 0)
		return -1;
	pos += name_len;

	if (pos + 4 > buflen)
		return -1;
	buf[pos++] = (uint8_t)(qtype >> 8);
	buf[pos++] = (uint8_t)(qtype & 0xFF);
	buf[pos++] = 0;
	buf[pos++] = DNS_CLASS_IN;

	return pos;
}

// Send DNS query for name/type and return raw response
// Returns response length, or negative errno
int dns_query_raw(const char *name, uint16_t qtype, uint8_t *response,
		  int max_len)
{
	if (!name || !response || max_len < (int)sizeof(dns_header_t))
		return -EINVAL;

	net_device_t *dev = net_get_default_device();
	if (!dev)
		return -ENETDOWN;
	uint32_t dns_server = dev->dns_server;
	if (dns_server == 0)
		return -ENETUNREACH;

	uint8_t query_buf[DNS_MAX_PACKET];
	uint16_t query_id = (uint16_t)random_u32();

	int query_len = dns_build_query_type(name, qtype, query_id, query_buf,
					     DNS_MAX_PACKET);
	if (query_len < 0)
		return -EINVAL;

	for (int retry = 0; retry < DNS_MAX_RETRIES; retry++) {
		/* Arm for this attempt under the lock: an unlocked reset can
		 * clobber a reply dns_rx() is landing right now, leaving
		 * ready=1 with len=0. */
		uint64_t aflags;
		spin_lock_irqsave(&dns_lock, &aflags);
		dns_rx_ready = 0;
		dns_rx_len = 0;
		spin_unlock_irqrestore(&dns_lock, aflags);

		int ret = udp_send(dev, dns_server, DNS_CLIENT_PORT, DNS_PORT,
				   query_buf, (uint16_t)query_len);
		if (ret < 0)
			return ret;

		uint64_t start = timer_ticks();
		while (!dns_rx_ready) {
			if (timer_ticks() - start > DNS_TIMEOUT_MS)
				break;
			__asm__ volatile("pause");
		}

		if (!dns_rx_ready)
			continue;

		/* Copy the id and the bytes out together, then decide.  The old
		 * order — test dns_rx_id, then copy dns_rx_buf — let a reply
		 * arriving in between be handed back as the answer to this query.
		 * That is how a valid name came back NXDOMAIN: the bytes copied
		 * were some other lookup's (a PTR NXDOMAIN is routine). */
		int copy_len;
		uint16_t got_id;
		uint64_t lflags;
		spin_lock_irqsave(&dns_lock, &lflags);
		copy_len = dns_rx_len;
		got_id = dns_rx_id;
		if (copy_len < 0)
			copy_len = 0;
		if (copy_len > max_len)
			copy_len = max_len;
		for (int i = 0; i < copy_len; i++)
			response[i] = dns_rx_buf[i];
		spin_unlock_irqrestore(&dns_lock, lflags);

		if (got_id != query_id)
			continue; // Someone else's answer — retry
		if (!dns_answers_our_question(response, copy_len, query_buf,
					      query_len))
			continue; // id collided with a straggler — not ours

		return copy_len;
	}

	return -ETIMEDOUT;
}

// ============================================================================
// dns_decode_name - Decode a DNS name (with compression) from a packet
// ============================================================================
static int dns_decode_name(const uint8_t *pkt, int pktlen, int offset,
			   char *out, int maxlen)
{
	int pos = offset;
	int opos = 0;
	int jumped = 0;
	int end_pos = -1;
	int max_jumps = 10;

	while (pos < pktlen && opos < maxlen - 1) {
		uint8_t len = pkt[pos];
		if (len == 0) {
			if (!jumped)
				end_pos = pos + 1;
			break;
		}
		if ((len & 0xC0) == 0xC0) {
			if (pos + 1 >= pktlen)
				return -1;
			if (!jumped)
				end_pos = pos + 2;
			pos = ((len & 0x3F) << 8) | pkt[pos + 1];
			jumped = 1;
			if (--max_jumps <= 0)
				return -1;
			continue;
		}
		pos++;
		if (pos + len > pktlen)
			return -1;
		if (opos > 0 && opos < maxlen - 1)
			out[opos++] = '.';
		for (int j = 0; j < len && opos < maxlen - 1; j++)
			out[opos++] = (char)pkt[pos + j];
		pos += len;
	}
	out[opos] = '\0';
	return (end_pos >= 0) ? end_pos : pos;
}

// ============================================================================
// dns_resolve_reverse - Reverse DNS lookup (PTR record) for an IPv4 address
// ip_nbo: IP address in network byte order
// out: buffer for the resulting hostname
// maxlen: size of out buffer
// Returns 0 on success, negative errno on failure
// ============================================================================
int dns_resolve_reverse(uint32_t ip_nbo, char *out, int maxlen)
{
	if (!out || maxlen < 2)
		return -EINVAL;

	// Build "d.c.b.a.in-addr.arpa" name
	uint8_t a = (uint8_t)(ip_nbo & 0xFF);
	uint8_t b = (uint8_t)((ip_nbo >> 8) & 0xFF);
	uint8_t c = (uint8_t)((ip_nbo >> 16) & 0xFF);
	uint8_t d = (uint8_t)((ip_nbo >> 24) & 0xFF);

	char name[64];
	int npos = 0;
// Helper to append a decimal byte
#define APPEND_BYTE(v)                                       \
	do {                                                 \
		uint8_t _v = (v);                            \
		if (_v >= 100)                               \
			name[npos++] = '0' + _v / 100;       \
		if (_v >= 10)                                \
			name[npos++] = '0' + (_v / 10) % 10; \
		name[npos++] = '0' + _v % 10;                \
	} while (0)

	APPEND_BYTE(d);
	name[npos++] = '.';
	APPEND_BYTE(c);
	name[npos++] = '.';
	APPEND_BYTE(b);
	name[npos++] = '.';
	APPEND_BYTE(a);
#undef APPEND_BYTE

	// Append ".in-addr.arpa"
	const char *suffix = ".in-addr.arpa";
	for (int i = 0; suffix[i]; i++)
		name[npos++] = suffix[i];
	name[npos] = '\0';

	// Send PTR query
	uint8_t response[DNS_MAX_PACKET];
	int rlen = dns_query_raw(name, DNS_TYPE_PTR, response, DNS_MAX_PACKET);
	if (rlen < (int)sizeof(dns_header_t))
		return -ENOENT;

	const dns_header_t *hdr = (const dns_header_t *)response;
	uint16_t flags = net_ntohs(hdr->flags);
	uint16_t qdcount = net_ntohs(hdr->qdcount);
	uint16_t ancount = net_ntohs(hdr->ancount);

	if (!(flags & DNS_FLAG_QR))
		return -ENOENT;
	if ((flags & DNS_FLAG_RCODE) != 0)
		return -ENOENT;
	if (ancount == 0)
		return -ENOENT;

	// Skip question section
	int pos = sizeof(dns_header_t);
	for (uint16_t i = 0; i < qdcount; i++) {
		pos = dns_skip_name(response, rlen, pos);
		pos += 4;
		if (pos > rlen)
			return -ENOENT;
	}

	// Parse answer section — look for PTR record
	for (uint16_t i = 0; i < ancount; i++) {
		pos = dns_skip_name(response, rlen, pos);
		if (pos + 10 > rlen)
			return -ENOENT;

		uint16_t rtype =
			((uint16_t)response[pos] << 8) | response[pos + 1];
		uint16_t rdlen =
			((uint16_t)response[pos + 8] << 8) | response[pos + 9];
		pos += 10;

		if (pos + rdlen > rlen)
			return -ENOENT;

		if (rtype == DNS_TYPE_PTR) {
			// PTR RDATA is a compressed domain name
			int ret = dns_decode_name(response, rlen, pos, out,
						  maxlen);
			if (ret < 0)
				return -ENOENT;
			return 0;
		}
		pos += rdlen;
	}

	return -ENOENT;
}
