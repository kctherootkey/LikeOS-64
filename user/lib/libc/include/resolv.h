/*
 * resolv.h - the DNS stub resolver
 *
 * The layer beneath getaddrinfo(3): where that answers "what address does this
 * name have", these send a DNS query of any type and hand back the raw reply
 * for the caller to parse.  It is the only way to reach the records
 * getaddrinfo does not deal in -- MX, TXT, SRV, SOA -- and it is what GLib's
 * GResolver, and mail software generally, is written against.
 *
 * This header used to declare nothing.  The comment explaining that said the
 * API "is not part of this libc", which was true and no longer is.
 *
 * Configuration comes from /etc/resolv.conf: `nameserver' lines give the
 * servers to try in order, `search' the domains appended to an unqualified
 * name, and `options timeout:' / `options attempts:' how long and how often to
 * wait.  It is read once per res_init(), not once per query.
 */

#ifndef _RESOLV_H
#define _RESOLV_H

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAXNS          3 /* Nameservers remembered from resolv.conf */
#define MAXDFLSRCH     3 /* Domains derived from the local one */
#define MAXDNSRCH      6 /* Entries in the search list */
#define RES_TIMEOUT    5 /* Seconds to wait for a reply, by default */
#define MAXRESOLVSORT  10
#define RES_MAXNDOTS   15
#define RES_MAXRETRANS 30
#define RES_MAXRETRY   5
#define RES_DFLRETRY   2
#define RES_MAXTIME    65535

/* Bits in the state's `options' field. */
#define RES_INIT      0x00000001 /* The state has been filled in */
#define RES_DEBUG     0x00000002
#define RES_USEVC     0x00000008 /* Use TCP rather than UDP */
#define RES_STAYOPEN  0x00000010
#define RES_IGNTC     0x00000020 /* Do not retry a truncated answer */
#define RES_RECURSE   0x00000040 /* Ask the server to recurse */
#define RES_DEFNAMES  0x00000080 /* Append the default domain */
#define RES_DNSRCH    0x00000200 /* Walk the search list */
#define RES_NOALIASES 0x00001000
#define RES_ROTATE    0x00004000 /* Round-robin the nameservers */
#define RES_USE_EDNS0 0x00100000
#define RES_SNGLKUP   0x00200000

#define RES_DEFAULT (RES_RECURSE | RES_DEFNAMES | RES_DNSRCH)

/*
 * Resolver state.
 *
 * The layout is fixed by the interface rather than by us: software declares a
 * `struct __res_state' of its own and reaches into .nscount and .nsaddr_list,
 * so the fields carry the conventional names in the conventional order.
 * Members this implementation does not act on are present and left zero rather
 * than left out, so that such code still compiles and reads a defined value.
 */
struct __res_state {
	int retrans; /* Seconds to wait for a reply */
	int retry;   /* Attempts per nameserver */
	unsigned long options;
	int nscount; /* Nameservers in nsaddr_list */
	struct sockaddr_in nsaddr_list[MAXNS];
	unsigned short id;           /* Next query identifier */
	char *dnsrch[MAXDNSRCH + 1]; /* Search list, NULL terminated */
	char defdname[256];          /* Default domain */
	unsigned long pfcode;
	unsigned ndots : 4; /* Dots needed before a name is tried bare */
	unsigned nsort : 4;
	char unused[3];
	struct {
		struct in_addr addr;
		uint32_t mask;
	} sort_list[MAXRESOLVSORT];
	int res_h_errno; /* Error from the most recent lookup */
	int _vcsock;     /* TCP socket, while one is open */
	int _flags;
	char _pad[52]; /* Room the interface reserves */
};

typedef struct __res_state *res_state;

/* The process-wide state, used by the functions that do not take one. */
extern struct __res_state _res;

/*
 * Read /etc/resolv.conf into the state.  The query functions call this
 * themselves the first time they are used, so an application rarely needs to.
 * Returns 0, or -1 if no usable configuration could be found.
 */
int res_init(void);
int res_ninit(res_state statp);

/* Release anything the state holds open. */
void res_close(void);
void res_nclose(res_state statp);

/*
 * Look up `dname' and place the reply in `answer'.
 *
 * Returns the length of the reply.  That may EXCEED anslen when the answer was
 * larger than the buffer, in which case the buffer holds as much as fits --
 * the caller is expected to notice and retry with more room.  Returns -1 and
 * sets h_errno on failure: HOST_NOT_FOUND for a name that does not exist,
 * TRY_AGAIN for a timeout or a server failure, NO_DATA for a name that exists
 * but has no record of the type asked for.
 *
 * res_query asks exactly what it was given; res_search also tries the entries
 * of the search list, which is what a user typing an unqualified name expects.
 */
int res_query(const char *__dname, int __class, int __type,
	      unsigned char *__answer, int __anslen);
int res_nquery(res_state __statp, const char *__dname, int __class, int __type,
	       unsigned char *__answer, int __anslen);
int res_search(const char *__dname, int __class, int __type,
	       unsigned char *__answer, int __anslen);
int res_nsearch(res_state __statp, const char *__dname, int __class,
		int __type, unsigned char *__answer, int __anslen);

/* Build a query message without sending it, and send a prepared one. */
int res_mkquery(int __op, const char *__dname, int __class, int __type,
		const unsigned char *__data, int __datalen,
		const unsigned char *__newrr, unsigned char *__buf,
		int __buflen);
int res_nmkquery(res_state __statp, int __op, const char *__dname, int __class,
		 int __type, const unsigned char *__data, int __datalen,
		 const unsigned char *__newrr, unsigned char *__buf,
		 int __buflen);
int res_send(const unsigned char *__msg, int __msglen, unsigned char *__answer,
	     int __anslen);
int res_nsend(res_state __statp, const unsigned char *__msg, int __msglen,
	      unsigned char *__answer, int __anslen);

/*
 * Expand a name from a message into text.
 *
 * Names in a DNS message are compressed: one may end in a pointer back to an
 * earlier name rather than repeat it, so a name cannot simply be copied out.
 * `msg' and `eom' bound the message, which is what makes following those
 * pointers safe.  Returns how many bytes of the message were consumed at
 * `src', or -1 if the name is malformed.
 */
int dn_expand(const unsigned char *msg, const unsigned char *eom,
	      const unsigned char *src, char *dst, int dstsiz);

/* Encode a name into a message.  No back-references are generated -- the name
 * is written in full, which is always valid and is all a single-question query
 * needs.  Returns the encoded length, or -1. */
int dn_comp(const char *src, unsigned char *dst, int dstsiz,
	    unsigned char **dnptrs, unsigned char **lastdnptr);

/* Length of the encoded name at `ptr', without expanding it. */
int dn_skipname(const unsigned char *ptr, const unsigned char *eom);

/*
 * b64_ntop / b64_pton are NOT declared here.
 *
 * They belong to this header historically, and tmux and OpenSSH include it
 * expecting them -- but both ship their own declaration in their compatibility
 * layer, and a second one here would have to match those exactly or break the
 * builds that work today.
 */

#ifdef __cplusplus
}
#endif

#endif /* _RESOLV_H */
