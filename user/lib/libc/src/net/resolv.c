/*
 * LikeOS-64 resolv.c - DNS stub resolver
 *
 * The raw query interface: build a DNS message, send it to the servers named
 * in /etc/resolv.conf, and hand the reply back unparsed.  getaddrinfo() answers
 * "what address does this name have" and needs nothing else; this is for the
 * records it does not deal in -- MX, TXT, SRV, SOA -- which a mail client or a
 * service-discovery library reads for itself.
 *
 * Entirely in userspace over ordinary UDP and TCP sockets.  The kernel's
 * SYS_DNS_RESOLVE, which getaddrinfo uses, resolves a name to one address and
 * has no way to express anything else; adding record types to a syscall would
 * put a protocol parser in the kernel to no purpose.
 *
 * Truncated replies are retried over TCP, which matters more than it sounds:
 * a TXT record set routinely exceeds the 512 bytes UDP carries, and a caller
 * that never retries sees a valid-looking answer with records missing.
 */

#include <resolv.h>
#include <arpa/nameser.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

struct __res_state _res;

#define RESOLV_CONF "/etc/resolv.conf"

/* ------------------------------------------------------------------ *
 * Configuration
 * ------------------------------------------------------------------ */

static void res_set_defaults(res_state statp)
{
	memset(statp, 0, sizeof(*statp));
	statp->retrans = RES_TIMEOUT;
	statp->retry = RES_DFLRETRY;
	statp->options = RES_DEFAULT;
	statp->ndots = 1;
	statp->id = 0;
	statp->_vcsock = -1;
}

static void res_add_nameserver(res_state statp, const char *addr)
{
	struct in_addr in;

	if (statp->nscount >= MAXNS)
		return;
	if (inet_pton(AF_INET, addr, &in) != 1)
		return;
	statp->nsaddr_list[statp->nscount].sin_family = AF_INET;
	statp->nsaddr_list[statp->nscount].sin_port = htons(NS_DEFAULTPORT);
	statp->nsaddr_list[statp->nscount].sin_addr = in;
	statp->nscount++;
}

/* Split `line' on whitespace into the search list.  The storage is the state's
 * own defdname buffer, carved into pieces -- the interface hands out char *,
 * and this keeps them alive for as long as the state is. */
static void res_set_search(res_state statp, const char *line)
{
	size_t off = 0;
	int n = 0;

	while (*line && n < MAXDNSRCH) {
		size_t len = 0;

		while (*line == ' ' || *line == '\t')
			line++;
		while (line[len] && line[len] != ' ' && line[len] != '\t' &&
		       line[len] != '\n')
			len++;
		if (!len)
			break;
		if (off + len + 1 > sizeof(statp->defdname))
			break;
		memcpy(statp->defdname + off, line, len);
		statp->defdname[off + len] = '\0';
		statp->dnsrch[n++] = statp->defdname + off;
		off += len + 1;
		line += len;
	}
	statp->dnsrch[n] = NULL;
}

static int res_ninit_impl(res_state statp, int *from_file)
{
	FILE *f;
	char line[512];

	if (from_file)
		*from_file = 0;
	if (!statp) {
		errno = EFAULT;
		return -1;
	}
	res_set_defaults(statp);

	f = fopen(RESOLV_CONF, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char *p = line;
			char *nl;

			nl = strchr(p, '\n');
			if (nl)
				*nl = '\0';
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '#' || *p == ';' || *p == '\0')
				continue;

			if (!strncmp(p, "nameserver", 10) &&
			    (p[10] == ' ' || p[10] == '\t')) {
				p += 10;
				while (*p == ' ' || *p == '\t')
					p++;
				res_add_nameserver(statp, p);
			} else if ((!strncmp(p, "search", 6) &&
				    (p[6] == ' ' || p[6] == '\t'))) {
				res_set_search(statp, p + 6);
			} else if (!strncmp(p, "domain", 6) &&
				   (p[6] == ' ' || p[6] == '\t')) {
				/* One domain is a search list of one. */
				res_set_search(statp, p + 6);
			} else if (!strncmp(p, "options", 7)) {
				char *o = strstr(p, "timeout:");

				if (o) {
					int v = atoi(o + 8);

					if (v > 0 && v <= RES_MAXTIME)
						statp->retrans = v;
				}
				o = strstr(p, "attempts:");
				if (o) {
					int v = atoi(o + 9);

					if (v > 0 && v <= RES_MAXRETRY)
						statp->retry = v;
				}
				o = strstr(p, "ndots:");
				if (o) {
					int v = atoi(o + 6);

					if (v >= 0 && v <= RES_MAXNDOTS)
						statp->ndots = (unsigned)v;
				}
				if (strstr(p, "rotate"))
					statp->options |= RES_ROTATE;
				if (strstr(p, "use-vc"))
					statp->options |= RES_USEVC;
			}
		}
		fclose(f);
	}

	if (from_file)
		*from_file = statp->nscount;

	/* No configuration, or none of it usable: fall back to the local host,
	 * which is where a resolver would be if one were running here.  Better
	 * than reporting success with nowhere to send a query.  Counted
	 * separately from what the file supplied, because res_init() below
	 * must not program the kernel with a guess. */
	if (statp->nscount == 0)
		res_add_nameserver(statp, "127.0.0.1");

	statp->options |= RES_INIT;
	return 0;
}

int res_ninit(res_state statp)
{
	return res_ninit_impl(statp, NULL);
}

/*
 * res_init() does one thing more than res_ninit(&_res), and has done since
 * before the rest of this file existed: it also programs the KERNEL's
 * resolver, which is what getaddrinfo() goes through.  /etc/resolv.conf is
 * userspace's, so something has to carry its contents across, and this is
 * where the system has always done it -- getaddrinfo() and the shell both call
 * res_init() for exactly that side effect.
 *
 * It returns the number of servers installed rather than the zero the
 * interface specifies.  That is deliberate and is kept: callers here test for
 * it, and code written to the standard checks for a NEGATIVE return, so both
 * readings agree about success.
 */
int res_init(void)
{
	int from_file = 0;
	int installed = 0;
	int i;

	if (res_ninit_impl(&_res, &from_file) < 0)
		return -1;

	for (i = 0; i < from_file; i++)
		if (set_dns_server(NULL, _res.nsaddr_list[i].sin_addr.s_addr) == 0)
			installed++;

	return installed;
}

void res_nclose(res_state statp)
{
	if (statp && statp->_vcsock >= 0) {
		close(statp->_vcsock);
		statp->_vcsock = -1;
	}
}

void res_close(void)
{
	res_nclose(&_res);
}

/* ------------------------------------------------------------------ *
 * Names on the wire
 * ------------------------------------------------------------------ */

/*
 * "www.example.com" becomes \3www\7example\3com\0.
 *
 * A trailing dot is the root and is not a label of its own; an empty name is
 * the root alone, which encodes as a single zero byte.
 */
int dn_comp(const char *src, unsigned char *dst, int dstsiz,
	    unsigned char **dnptrs, unsigned char **lastdnptr)
{
	unsigned char *out = dst;
	const char *p = src;
	int left = dstsiz;

	/* No back-references are generated, so the compression state is not
	 * consulted.  Writing the name in full is always valid. */
	(void)dnptrs;
	(void)lastdnptr;

	if (!src || !dst)
		return -1;

	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);

		if (len == 0) {
			/* An empty label is only legal as the trailing dot. */
			if (dot && dot[1] == '\0')
				break;
			return -1;
		}
		if (len > NS_MAXLABEL)
			return -1;
		if (left < (int)len + 1)
			return -1;
		*out++ = (unsigned char)len;
		memcpy(out, p, len);
		out += len;
		left -= (int)len + 1;
		if (!dot)
			break;
		p = dot + 1;
	}
	if (left < 1)
		return -1;
	*out++ = 0; /* Root label terminates the name */
	return (int)(out - dst);
}

int dn_skipname(const unsigned char *ptr, const unsigned char *eom)
{
	const unsigned char *p = ptr;

	while (p < eom) {
		unsigned int len = *p;

		if ((len & NS_CMPRSFLGS) == NS_CMPRSFLGS) {
			/* A pointer is two bytes and ends the name. */
			if (p + 1 >= eom)
				return -1;
			return (int)(p + 2 - ptr);
		}
		if (len & NS_CMPRSFLGS)
			return -1; /* Reserved bit combination */
		p += len + 1;
		if (len == 0)
			return (int)(p - ptr);
	}
	return -1;
}

int dn_expand(const unsigned char *msg, const unsigned char *eom,
	      const unsigned char *src, char *dst, int dstsiz)
{
	const unsigned char *p = src;
	char *out = dst;
	int left = dstsiz;
	int consumed = -1;
	int hops = 0;

	if (!msg || !eom || !src || !dst || dstsiz <= 0)
		return -1;

	for (;;) {
		unsigned int len;

		if (p < msg || p >= eom)
			return -1;
		len = *p;

		if ((len & NS_CMPRSFLGS) == NS_CMPRSFLGS) {
			unsigned int off;

			if (p + 1 >= eom)
				return -1;
			off = ((len & 0x3f) << 8) | p[1];
			/* The length of the name AT src ends at the pointer;
			 * everything after it lives elsewhere in the message. */
			if (consumed < 0)
				consumed = (int)(p + 2 - src);
			/* A pointer must point strictly backwards.  That alone
			 * makes a loop impossible, but the hop count stays as a
			 * second line of defence against a message crafted to
			 * walk a chain of them. */
			if (msg + off >= p)
				return -1;
			if (++hops > NS_MAXCDNAME)
				return -1;
			p = msg + off;
			continue;
		}
		if (len & NS_CMPRSFLGS)
			return -1; /* Reserved bit combination */

		p++;
		if (len == 0) {
			/* Root: the name is complete.  An empty name prints as
			 * a lone dot, which is what the root is called. */
			if (out == dst) {
				if (left < 2)
					return -1;
				*out++ = '.';
			}
			*out = '\0';
			return consumed < 0 ? (int)(p - src) : consumed;
		}
		if (p + len > eom)
			return -1;
		/* Room for the label, the dot after it, and the terminator. */
		if (left < (int)len + 2)
			return -1;
		if (out != dst) {
			*out++ = '.';
			left--;
		}
		memcpy(out, p, len);
		out += len;
		left -= (int)len;
		p += len;
	}
}

/* ------------------------------------------------------------------ *
 * Building a query
 * ------------------------------------------------------------------ */

/* Identifiers should not be guessable from one another: a resolver that
 * numbers its queries 1, 2, 3 lets anything that can see one reply forge the
 * next.  Seeded from the clock and the pid the first time it is needed. */
static unsigned short res_next_id(res_state statp)
{
	if (statp->id == 0) {
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		statp->id = (unsigned short)(ts.tv_nsec ^ (ts.tv_sec << 8) ^
					     (getpid() << 3));
		if (statp->id == 0)
			statp->id = 1;
	}
	/* A multiplicative step rather than an increment, so consecutive
	 * queries are not consecutive numbers. */
	statp->id = (unsigned short)(statp->id * 1103515245u + 12345u);
	return statp->id;
}

int res_nmkquery(res_state statp, int op, const char *dname, int class,
		 int type, const unsigned char *data, int datalen,
		 const unsigned char *newrr, unsigned char *buf, int buflen)
{
	unsigned char *cp = buf;
	int n;
	unsigned short id;
	unsigned short flags;

	(void)data;
	(void)datalen;
	(void)newrr;

	if (!statp || !dname || !buf || buflen < NS_HFIXEDSZ + NS_QFIXEDSZ + 1) {
		errno = EINVAL;
		return -1;
	}

	id = res_next_id(statp);
	flags = (unsigned short)((op & 0x0f) << 11);
	if (statp->options & RES_RECURSE)
		flags |= 0x0100; /* rd */

	NS_PUT16(id, cp);
	NS_PUT16(flags, cp);
	NS_PUT16(1, cp); /* One question */
	NS_PUT16(0, cp); /* No answers */
	NS_PUT16(0, cp); /* No authority records */
	NS_PUT16(0, cp); /* No additional records */

	n = dn_comp(dname, cp, buflen - (int)(cp - buf) - NS_QFIXEDSZ, NULL,
		    NULL);
	if (n < 0)
		return -1;
	cp += n;

	NS_PUT16(type, cp);
	NS_PUT16(class, cp);

	return (int)(cp - buf);
}

int res_mkquery(int op, const char *dname, int class, int type,
		const unsigned char *data, int datalen,
		const unsigned char *newrr, unsigned char *buf, int buflen)
{
	if (!(_res.options & RES_INIT) && res_init() < 0)
		return -1;
	return res_nmkquery(&_res, op, dname, class, type, data, datalen, newrr,
			    buf, buflen);
}

/* ------------------------------------------------------------------ *
 * Sending
 * ------------------------------------------------------------------ */

/* A reply belongs to this query only if the identifier matches and it is
 * marked as a response.  Without both checks a late reply to an earlier query,
 * or a packet from anywhere at all, is taken as the answer. */
static int res_reply_matches(const unsigned char *query,
			     const unsigned char *reply, int replylen)
{
	if (replylen < NS_HFIXEDSZ)
		return 0;
	if (query[0] != reply[0] || query[1] != reply[1])
		return 0;
	if (!(reply[2] & 0x80)) /* qr */
		return 0;
	return 1;
}

/* One exchange with one server over UDP.  Returns the reply length, 0 on
 * timeout, or -1 on a local error. */
static int res_send_udp(res_state statp, const struct sockaddr_in *ns,
			const unsigned char *msg, int msglen,
			unsigned char *answer, int anslen)
{
	int fd;
	int got = 0;
	struct pollfd pfd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	if (sendto(fd, msg, msglen, 0, (const struct sockaddr *)ns,
		   sizeof(*ns)) != msglen) {
		close(fd);
		return -1;
	}

	/* Wait only for a reply that is ours.  Anything else -- a stray packet,
	 * or the answer to a query that already timed out -- is discarded and
	 * the wait resumes, rather than being handed back as the answer. */
	pfd.fd = fd;
	pfd.events = POLLIN;
	for (;;) {
		int r = poll(&pfd, 1, statp->retrans * 1000);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (r == 0) {
			close(fd);
			return 0; /* Timed out */
		}

		got = (int)recv(fd, answer, anslen, 0);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (res_reply_matches(msg, answer, got))
			break;
	}
	close(fd);
	return got;
}

/*
 * The same exchange over TCP, used when a UDP reply came back truncated.
 *
 * TCP frames each message with a two-byte length, so this reads that first and
 * then exactly that many bytes.  The answer may be larger than the caller's
 * buffer: the excess is drained and discarded, and the FULL length is returned,
 * which is how the caller learns it needs more room.
 */
static int res_send_tcp(res_state statp, const struct sockaddr_in *ns,
			const unsigned char *msg, int msglen,
			unsigned char *answer, int anslen)
{
	unsigned char lenbuf[2];
	unsigned char *frame;
	int fd, n, want, have = 0;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (const struct sockaddr *)ns, sizeof(*ns)) < 0) {
		close(fd);
		return -1;
	}

	frame = malloc(msglen + 2);
	if (!frame) {
		close(fd);
		return -1;
	}
	frame[0] = (unsigned char)(msglen >> 8);
	frame[1] = (unsigned char)msglen;
	memcpy(frame + 2, msg, msglen);
	n = (int)write(fd, frame, msglen + 2);
	free(frame);
	if (n != msglen + 2) {
		close(fd);
		return -1;
	}

	while (have < 2) {
		n = (int)read(fd, lenbuf + have, 2 - have);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		have += n;
	}
	want = (lenbuf[0] << 8) | lenbuf[1];

	have = 0;
	while (have < want) {
		int room = anslen - have;
		unsigned char scratch[512];
		unsigned char *dst;
		int chunk;

		if (room > 0) {
			dst = answer + have;
			chunk = room < want - have ? room : want - have;
		} else {
			/* Past the caller's buffer: read and drop, so the
			 * connection is drained and the true length known. */
			dst = scratch;
			chunk = (int)sizeof(scratch) < want - have ?
					(int)sizeof(scratch) :
					want - have;
		}
		n = (int)read(fd, dst, chunk);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		have += n;
	}
	close(fd);

	if (!res_reply_matches(msg, answer, have < anslen ? have : anslen))
		return -1;
	(void)statp;
	return want;
}

int res_nsend(res_state statp, const unsigned char *msg, int msglen,
	      unsigned char *answer, int anslen)
{
	int attempt;

	if (!statp || !msg || !answer || msglen < NS_HFIXEDSZ) {
		errno = EINVAL;
		return -1;
	}
	if (!(statp->options & RES_INIT) && res_ninit(statp) < 0)
		return -1;
	if (statp->nscount == 0) {
		statp->res_h_errno = NO_RECOVERY;
		h_errno = NO_RECOVERY;
		return -1;
	}

	for (attempt = 0; attempt < statp->retry; attempt++) {
		int i;

		for (i = 0; i < statp->nscount; i++) {
			const struct sockaddr_in *ns = &statp->nsaddr_list[i];
			int n;

			if (statp->options & RES_USEVC)
				n = res_send_tcp(statp, ns, msg, msglen,
						 answer, anslen);
			else
				n = res_send_udp(statp, ns, msg, msglen,
						 answer, anslen);

			if (n < 0)
				continue; /* This server is unreachable */
			if (n == 0)
				continue; /* Timed out; try the next */

			/* Truncated: the rest of the answer only exists over
			 * TCP.  A TXT or SRV set of any size hits this
			 * routinely, and a caller that ignored it would see a
			 * well-formed reply with records missing. */
			if (n >= NS_HFIXEDSZ && (answer[2] & 0x02) &&
			    !(statp->options & (RES_IGNTC | RES_USEVC))) {
				int t = res_send_tcp(statp, ns, msg, msglen,
						     answer, anslen);

				if (t > 0)
					return t;
			}
			return n;
		}
	}

	statp->res_h_errno = TRY_AGAIN;
	h_errno = TRY_AGAIN;
	return -1;
}

int res_send(const unsigned char *msg, int msglen, unsigned char *answer,
	     int anslen)
{
	if (!(_res.options & RES_INIT) && res_init() < 0)
		return -1;
	return res_nsend(&_res, msg, msglen, answer, anslen);
}

/* ------------------------------------------------------------------ *
 * Queries
 * ------------------------------------------------------------------ */

/* Turn a server's response code and answer count into the h_errno the
 * interface reports.  The distinction callers act on is "no such name" versus
 * "that name has nothing of this type", which are different codes. */
static int res_classify(const unsigned char *answer, int anslen)
{
	int rcode, ancount;

	if (anslen < NS_HFIXEDSZ)
		return NO_RECOVERY;
	rcode = answer[3] & 0x0f;
	ancount = (answer[6] << 8) | answer[7];

	switch (rcode) {
	case ns_r_noerror:
		return ancount > 0 ? 0 : NO_DATA;
	case ns_r_nxdomain:
		return HOST_NOT_FOUND;
	case ns_r_servfail:
		return TRY_AGAIN;
	case ns_r_refused:
	case ns_r_notimpl:
	case ns_r_formerr:
	default:
		return NO_RECOVERY;
	}
}

int res_nquery(res_state statp, const char *dname, int class, int type,
	       unsigned char *answer, int anslen)
{
	unsigned char query[NS_PACKETSZ];
	int qlen, n, err;

	if (!statp || !dname || !answer) {
		errno = EINVAL;
		return -1;
	}
	if (!(statp->options & RES_INIT) && res_ninit(statp) < 0)
		return -1;

	qlen = res_nmkquery(statp, ns_o_query, dname, class, type, NULL, 0,
			    NULL, query, (int)sizeof(query));
	if (qlen < 0) {
		statp->res_h_errno = NO_RECOVERY;
		h_errno = NO_RECOVERY;
		return -1;
	}

	n = res_nsend(statp, query, qlen, answer, anslen);
	if (n < 0)
		return -1;

	err = res_classify(answer, n < anslen ? n : anslen);
	if (err != 0) {
		statp->res_h_errno = err;
		h_errno = err;
		return -1;
	}
	statp->res_h_errno = 0;
	return n;
}

int res_query(const char *dname, int class, int type, unsigned char *answer,
	      int anslen)
{
	if (!(_res.options & RES_INIT) && res_init() < 0)
		return -1;
	return res_nquery(&_res, dname, class, type, answer, anslen);
}

int res_nsearch(res_state statp, const char *dname, int class, int type,
		unsigned char *answer, int anslen)
{
	char buf[NS_MAXDNAME];
	int dots = 0;
	int n, i;
	const char *p;

	if (!statp || !dname || !answer) {
		errno = EINVAL;
		return -1;
	}
	if (!(statp->options & RES_INIT) && res_ninit(statp) < 0)
		return -1;

	for (p = dname; *p; p++)
		if (*p == '.')
			dots++;

	/* A name with enough dots, or a trailing one, is meant as it stands and
	 * is tried first.  Fully-qualified names are the common case, so
	 * trying them first also avoids a pointless round trip per search
	 * domain. */
	if (dots >= (int)statp->ndots || (p > dname && p[-1] == '.')) {
		n = res_nquery(statp, dname, class, type, answer, anslen);
		if (n >= 0)
			return n;
		/* A name that definitively does not exist is not worth
		 * appending domains to; a timeout might be. */
		if (statp->res_h_errno == HOST_NOT_FOUND &&
		    (p > dname && p[-1] == '.'))
			return -1;
	}

	if (statp->options & RES_DNSRCH) {
		for (i = 0; statp->dnsrch[i]; i++) {
			int len = snprintf(buf, sizeof(buf), "%s.%s", dname,
					   statp->dnsrch[i]);

			if (len < 0 || (size_t)len >= sizeof(buf))
				continue;
			n = res_nquery(statp, buf, class, type, answer, anslen);
			if (n >= 0)
				return n;
		}
	}

	/* Last resort: the bare name, if it was not tried above. */
	if (dots < (int)statp->ndots) {
		n = res_nquery(statp, dname, class, type, answer, anslen);
		if (n >= 0)
			return n;
	}
	return -1;
}

int res_search(const char *dname, int class, int type, unsigned char *answer,
	       int anslen)
{
	if (!(_res.options & RES_INIT) && res_init() < 0)
		return -1;
	return res_nsearch(&_res, dname, class, type, answer, anslen);
}
