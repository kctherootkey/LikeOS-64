/*
 * arpa/nameser_compat.h - the traditional DNS names
 *
 * The uppercase spellings BIND used before the ns_* namespace arrived, and
 * which most software still writes: C_IN rather than ns_c_in, T_MX rather than
 * ns_t_mx.  Each is an alias for the corresponding enumerator in
 * <arpa/nameser.h>, so the two spellings are the same value and can be mixed.
 *
 * Also the HEADER struct, which is the message header laid out as bitfields.
 * Software that builds or inspects a query by hand uses it, GLib's resolver
 * among them.
 */

#ifndef _ARPA_NAMESER_COMPAT_H
#define _ARPA_NAMESER_COMPAT_H

#include <arpa/nameser.h>
#include <endian.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACKETSZ   NS_PACKETSZ
#define MAXDNAME   NS_MAXDNAME
#define MAXCDNAME  NS_MAXCDNAME
#define MAXLABEL   NS_MAXLABEL
#define HFIXEDSZ   NS_HFIXEDSZ
#define QFIXEDSZ   NS_QFIXEDSZ
#define RRFIXEDSZ  NS_RRFIXEDSZ
#define INT32SZ    NS_INT32SZ
#define INT16SZ    NS_INT16SZ
#define INADDRSZ   NS_INADDRSZ
#define IN6ADDRSZ  NS_IN6ADDRSZ
#define INDIR_MASK NS_CMPRSFLGS
#define NAMESERVER_PORT NS_DEFAULTPORT

/* Opcodes */
#define QUERY  ns_o_query
#define IQUERY ns_o_iquery
#define STATUS ns_o_status
#define NS_NOTIFY_OP ns_o_notify
#define NS_UPDATE_OP ns_o_update

/* Response codes */
#define NOERROR  ns_r_noerror
#define FORMERR  ns_r_formerr
#define SERVFAIL ns_r_servfail
#define NXDOMAIN ns_r_nxdomain
#define NOTIMP   ns_r_notimpl
#define REFUSED  ns_r_refused

/* Classes */
#define C_IN    ns_c_in
#define C_CHAOS ns_c_chaos
#define C_HS    ns_c_hs
#define C_NONE  ns_c_none
#define C_ANY   ns_c_any

/* Types */
#define T_A     ns_t_a
#define T_NS    ns_t_ns
#define T_MD    ns_t_md
#define T_MF    ns_t_mf
#define T_CNAME ns_t_cname
#define T_SOA   ns_t_soa
#define T_MB    ns_t_mb
#define T_MG    ns_t_mg
#define T_MR    ns_t_mr
#define T_NULL  ns_t_null
#define T_WKS   ns_t_wks
#define T_PTR   ns_t_ptr
#define T_HINFO ns_t_hinfo
#define T_MINFO ns_t_minfo
#define T_MX    ns_t_mx
#define T_TXT   ns_t_txt
#define T_RP    ns_t_rp
#define T_AFSDB ns_t_afsdb
#define T_X25   ns_t_x25
#define T_ISDN  ns_t_isdn
#define T_RT    ns_t_rt
#define T_NSAP  ns_t_nsap
#define T_NSAP_PTR ns_t_nsap_ptr
#define T_SIG   ns_t_sig
#define T_KEY   ns_t_key
#define T_PX    ns_t_px
#define T_GPOS  ns_t_gpos
#define T_AAAA  ns_t_aaaa
#define T_LOC   ns_t_loc
#define T_NXT   ns_t_nxt
#define T_EID   ns_t_eid
#define T_NIMLOC ns_t_nimloc
#define T_SRV   ns_t_srv
#define T_ATMA  ns_t_atma
#define T_NAPTR ns_t_naptr
#define T_KX    ns_t_kx
#define T_CERT  ns_t_cert
#define T_A6    ns_t_a6
#define T_DNAME ns_t_dname
#define T_SINK  ns_t_sink
#define T_OPT   ns_t_opt
#define T_APL   ns_t_apl
#define T_DS    ns_t_ds
#define T_SSHFP ns_t_sshfp
#define T_RRSIG ns_t_rrsig
#define T_NSEC  ns_t_nsec
#define T_DNSKEY ns_t_dnskey
#define T_TLSA  ns_t_tlsa
#define T_SPF   ns_t_spf
#define T_UINFO ns_t_uinfo
#define T_UID   ns_t_uid
#define T_GID   ns_t_gid
#define T_UNSPEC ns_t_unspec
#define T_TKEY  ns_t_tkey
#define T_TSIG  ns_t_tsig
#define T_IXFR  ns_t_ixfr
#define T_AXFR  ns_t_axfr
#define T_MAILB ns_t_mailb
#define T_MAILA ns_t_maila
#define T_ANY   ns_t_any
#define T_URI   ns_t_uri
#define T_CAA   ns_t_caa

#define GETSHORT   NS_GET16
#define GETLONG    NS_GET32
#define PUTSHORT   NS_PUT16
#define PUTLONG    NS_PUT32

/*
 * The message header, as bitfields.
 *
 * Bitfield order within a byte follows the machine's endianness, which is why
 * this is written twice.  On the little-endian machine this system runs on,
 * the first branch is the live one; the other is kept because the struct is
 * copied verbatim from every other system's header and the symmetry is what
 * makes it checkable against them.
 */
typedef struct {
	unsigned id : 16; /* Query identifier, echoed in the response */
#if __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned rd : 1;     /* Recursion desired */
	unsigned tc : 1;     /* Truncated -- retry over TCP */
	unsigned aa : 1;     /* Authoritative answer */
	unsigned opcode : 4; /* Kind of query */
	unsigned qr : 1;     /* Response rather than query */
	unsigned rcode : 4;  /* Response code */
	unsigned cd : 1;     /* Checking disabled */
	unsigned ad : 1;     /* Authentic data */
	unsigned unused : 1;
	unsigned ra : 1; /* Recursion available */
#else
	unsigned qr : 1;
	unsigned opcode : 4;
	unsigned aa : 1;
	unsigned tc : 1;
	unsigned rd : 1;
	unsigned ra : 1;
	unsigned unused : 1;
	unsigned ad : 1;
	unsigned cd : 1;
	unsigned rcode : 4;
#endif
	unsigned qdcount : 16; /* Questions */
	unsigned ancount : 16; /* Answer records */
	unsigned nscount : 16; /* Authority records */
	unsigned arcount : 16; /* Additional records */
} HEADER;

#ifdef __cplusplus
}
#endif

#endif /* _ARPA_NAMESER_COMPAT_H */
