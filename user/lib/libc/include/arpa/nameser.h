/*
 * arpa/nameser.h - DNS wire format
 *
 * The constants and layouts of the DNS protocol itself (RFC 1035 and its
 * successors), as every Unix has published them since BIND.  This is what a
 * program includes when it intends to look AT a DNS message rather than just
 * ask for an address: GLib's resolver reads MX, TXT, SRV and SOA records this
 * way, and so does anything else needing more than a name-to-address lookup.
 *
 * This header used to be empty -- it existed only so that tmux's base64
 * compatibility code would find something to include.  The names below are the
 * modern (ns_*) spellings; <arpa/nameser_compat.h>, included at the end,
 * supplies the traditional uppercase aliases.
 *
 * Layouts and constants only.  The message-walking helpers of libresolv
 * (ns_initparse, ns_parserr) are deliberately NOT declared, because they are
 * not implemented: a program that wants them should fail to compile here
 * rather than fail to link much later.  <resolv.h> declares what IS
 * implemented -- res_query and the name-decompression routine that goes with
 * it.
 */

#ifndef _ARPA_NAMESER_H
#define _ARPA_NAMESER_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Message size limits (RFC 1035 §2.3.4, §4.2.1). */
#define NS_PACKETSZ    512  /* Largest message carried over plain UDP */
#define NS_MAXDNAME    1025 /* Largest domain name, as text */
#define NS_MAXCDNAME   255  /* Largest compressed domain name */
#define NS_MAXLABEL    63   /* Largest single label */
#define NS_HFIXEDSZ    12   /* Fixed part of a message header */
#define NS_QFIXEDSZ    4    /* Fixed part of a question: type + class */
#define NS_RRFIXEDSZ   10   /* Fixed part of a record: type, class, ttl, len */
#define NS_INT32SZ     4
#define NS_INT16SZ     2
#define NS_INT8SZ      1
#define NS_INADDRSZ    4
#define NS_IN6ADDRSZ   16
#define NS_CMPRSFLGS   0xc0 /* The two high bits that mark a name pointer */
#define NS_DEFAULTPORT 53

/* Sections of a message, in the order they appear. */
typedef enum __ns_sect {
	ns_s_qd = 0, /* Question */
	ns_s_zn = 0, /* Zone (the same section, in an update message) */
	ns_s_an = 1, /* Answer */
	ns_s_pr = 1, /* Prerequisite (update) */
	ns_s_ns = 2, /* Authority */
	ns_s_ud = 2, /* Update */
	ns_s_ar = 3, /* Additional */
	ns_s_max = 4
} ns_sect;

/* Opcodes: what kind of request a message is. */
typedef enum __ns_opcode {
	ns_o_query = 0,
	ns_o_iquery = 1, /* Obsolete */
	ns_o_status = 2,
	ns_o_notify = 4,
	ns_o_update = 5
} ns_opcode;

/* Response codes, as returned in the header's low four bits. */
typedef enum __ns_rcode {
	ns_r_noerror = 0,
	ns_r_formerr = 1,  /* The server could not read the query */
	ns_r_servfail = 2, /* The server failed while answering it */
	ns_r_nxdomain = 3, /* The name does not exist */
	ns_r_notimpl = 4,
	ns_r_refused = 5,
	ns_r_yxdomain = 6,
	ns_r_yxrrset = 7,
	ns_r_nxrrset = 8,
	ns_r_notauth = 9,
	ns_r_notzone = 10
} ns_rcode;

/* Record classes.  Only `in' is of practical use; the rest are here because
 * the protocol defines them and software switches on them. */
typedef enum __ns_class {
	ns_c_invalid = 0,
	ns_c_in = 1,     /* The Internet */
	ns_c_2 = 2,      /* Unallocated */
	ns_c_chaos = 3,  /* MIT Chaosnet */
	ns_c_hs = 4,     /* MIT Hesiod */
	ns_c_none = 254, /* Prerequisite, in an update message */
	ns_c_any = 255,  /* Wildcard, for a query */
	ns_c_max = 65536
} ns_class;

/* Record types. */
typedef enum __ns_type {
	ns_t_invalid = 0,
	ns_t_a = 1,      /* IPv4 address */
	ns_t_ns = 2,     /* Authoritative name server */
	ns_t_md = 3,     /* Obsolete */
	ns_t_mf = 4,     /* Obsolete */
	ns_t_cname = 5,  /* Canonical name */
	ns_t_soa = 6,    /* Start of authority */
	ns_t_mb = 7,     /* Obsolete */
	ns_t_mg = 8,     /* Obsolete */
	ns_t_mr = 9,     /* Obsolete */
	ns_t_null = 10,  /* Obsolete */
	ns_t_wks = 11,   /* Obsolete */
	ns_t_ptr = 12,   /* Name pointer, used for reverse lookups */
	ns_t_hinfo = 13, /* Host information */
	ns_t_minfo = 14, /* Mailbox information */
	ns_t_mx = 15,    /* Mail exchanger */
	ns_t_txt = 16,   /* Free-form text */
	ns_t_rp = 17,
	ns_t_afsdb = 18,
	ns_t_x25 = 19,
	ns_t_isdn = 20,
	ns_t_rt = 21,
	ns_t_nsap = 22,
	ns_t_nsap_ptr = 23,
	ns_t_sig = 24,
	ns_t_key = 25,
	ns_t_px = 26,
	ns_t_gpos = 27,
	ns_t_aaaa = 28, /* IPv6 address */
	ns_t_loc = 29,
	ns_t_nxt = 30,
	ns_t_eid = 31,
	ns_t_nimloc = 32,
	ns_t_srv = 33, /* Service location */
	ns_t_atma = 34,
	ns_t_naptr = 35,
	ns_t_kx = 36,
	ns_t_cert = 37,
	ns_t_a6 = 38,
	ns_t_dname = 39,
	ns_t_sink = 40,
	ns_t_opt = 41, /* EDNS0 pseudo-record */
	ns_t_apl = 42,
	ns_t_ds = 43,
	ns_t_sshfp = 44,
	ns_t_ipseckey = 45,
	ns_t_rrsig = 46,
	ns_t_nsec = 47,
	ns_t_dnskey = 48,
	ns_t_dhcid = 49,
	ns_t_nsec3 = 50,
	ns_t_nsec3param = 51,
	ns_t_tlsa = 52,
	ns_t_smimea = 53,
	ns_t_hip = 55,
	ns_t_ninfo = 56,
	ns_t_rkey = 57,
	ns_t_talink = 58,
	ns_t_cds = 59,
	ns_t_cdnskey = 60,
	ns_t_openpgpkey = 61,
	ns_t_csync = 62,
	ns_t_spf = 99,
	ns_t_uinfo = 100,
	ns_t_uid = 101,
	ns_t_gid = 102,
	ns_t_unspec = 103,
	ns_t_nid = 104,
	ns_t_l32 = 105,
	ns_t_l64 = 106,
	ns_t_lp = 107,
	ns_t_eui48 = 108,
	ns_t_eui64 = 109,
	ns_t_tkey = 249,
	ns_t_tsig = 250,
	ns_t_ixfr = 251,  /* Incremental zone transfer */
	ns_t_axfr = 252,  /* Zone transfer */
	ns_t_mailb = 253, /* Obsolete */
	ns_t_maila = 254, /* Obsolete */
	ns_t_any = 255,   /* Wildcard, for a query */
	ns_t_uri = 256,
	ns_t_caa = 257,
	ns_t_avc = 258,
	ns_t_ta = 32768,
	ns_t_dlv = 32769,
	ns_t_max = 65536
} ns_type;

/*
 * Reading and writing the big-endian integers in a message.
 *
 * A byte at a time rather than through a 16- or 32-bit load, because a field
 * in a DNS message has no alignment to rely on: the header is 12 bytes, a name
 * is however long it is, and the integers after it land wherever they land.
 *
 * Each advances the pointer past what it read or wrote, which is how the
 * traditional parsing loops are written.
 */
#define NS_GET16(s, cp)                                                  \
	do {                                                             \
		const unsigned char *t_cp = (const unsigned char *)(cp); \
		(s) = ((uint16_t)t_cp[0] << 8) | ((uint16_t)t_cp[1]);    \
		(cp) += NS_INT16SZ;                                      \
	} while (0)

#define NS_GET32(l, cp)                                                       \
	do {                                                                  \
		const unsigned char *t_cp = (const unsigned char *)(cp);      \
		(l) = ((uint32_t)t_cp[0] << 24) | ((uint32_t)t_cp[1] << 16) | \
		      ((uint32_t)t_cp[2] << 8) | ((uint32_t)t_cp[3]);         \
		(cp) += NS_INT32SZ;                                           \
	} while (0)

#define NS_PUT16(s, cp)                                       \
	do {                                                  \
		uint16_t t_s = (uint16_t)(s);                 \
		unsigned char *t_cp = (unsigned char *)(cp);  \
		*t_cp++ = (unsigned char)(t_s >> 8);          \
		*t_cp = (unsigned char)t_s;                   \
		(cp) += NS_INT16SZ;                           \
	} while (0)

#define NS_PUT32(l, cp)                                       \
	do {                                                  \
		uint32_t t_l = (uint32_t)(l);                 \
		unsigned char *t_cp = (unsigned char *)(cp);  \
		*t_cp++ = (unsigned char)(t_l >> 24);         \
		*t_cp++ = (unsigned char)(t_l >> 16);         \
		*t_cp++ = (unsigned char)(t_l >> 8);          \
		*t_cp = (unsigned char)t_l;                   \
		(cp) += NS_INT32SZ;                           \
	} while (0)

#ifdef __cplusplus
}
#endif

/* The traditional names, which is what most software still uses.  Included
 * last and unconditionally, because every system carrying this header carries
 * that one too, and code includes whichever spelling it learned first. */
#include <arpa/nameser_compat.h>

#endif /* _ARPA_NAMESER_H */
