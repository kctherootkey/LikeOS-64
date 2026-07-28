#ifndef _NETINET_IP_H
#define _NETINET_IP_H

/*
 * IPv4 header (RFC 791) and IP type-of-service / DSCP definitions.
 * Standard 4.4BSD layout, kept byte-compatible so packet-handling software
 * (OpenSSH, traceroute, ping) builds unmodified.
 */

#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>

__BEGIN_DECLS

/*
 * Structure of an internet header, naked of options.
 */
struct ip {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	unsigned int ip_hl : 4; /* header length */
	unsigned int ip_v : 4;  /* version */
#else
	unsigned int ip_v : 4;  /* version */
	unsigned int ip_hl : 4; /* header length */
#endif
	uint8_t ip_tos;              /* type of service */
	unsigned short ip_len;       /* total length */
	unsigned short ip_id;        /* identification */
	unsigned short ip_off;       /* fragment offset field */
#define IP_RF 0x8000             /* reserved fragment flag */
#define IP_DF 0x4000             /* dont fragment flag */
#define IP_MF 0x2000             /* more fragments flag */
#define IP_OFFMASK 0x1fff        /* mask for fragmenting bits */
	uint8_t ip_ttl;              /* time to live */
	uint8_t ip_p;                /* protocol */
	unsigned short ip_sum;       /* checksum */
	struct in_addr ip_src, ip_dst; /* source and dest address */
};

/*
 * Time stamp option structure.
 */
struct ip_timestamp {
	uint8_t ipt_code;   /* IPOPT_TS */
	uint8_t ipt_len;    /* size of structure (variable) */
	uint8_t ipt_ptr;    /* index of current entry */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	unsigned int ipt_flg : 4; /* flags, see below */
	unsigned int ipt_oflw : 4; /* overflow counter */
#else
	unsigned int ipt_oflw : 4; /* overflow counter */
	unsigned int ipt_flg : 4;  /* flags, see below */
#endif
	uint32_t data[9];
};

#define IPVERSION 4 /* IP version number */
#define IP_MAXPACKET 65535 /* maximum packet size */

/*
 * Definitions for IP type of service (ip_tos).
 */
#define IPTOS_TOS_MASK 0x1E
#define IPTOS_TOS(tos) ((tos) & IPTOS_TOS_MASK)
#define IPTOS_LOWDELAY 0x10
#define IPTOS_THROUGHPUT 0x08
#define IPTOS_RELIABILITY 0x04
#define IPTOS_LOWCOST 0x02
#define IPTOS_MINCOST IPTOS_LOWCOST

/*
 * Definitions for IP precedence (also in ip_tos) (deprecated).
 */
#define IPTOS_PREC_MASK 0xe0
#define IPTOS_PREC(tos) ((tos) & IPTOS_PREC_MASK)
#define IPTOS_PREC_NETCONTROL 0xe0
#define IPTOS_PREC_INTERNETCONTROL 0xc0
#define IPTOS_PREC_CRITIC_ECP 0xa0
#define IPTOS_PREC_FLASHOVERRIDE 0x80
#define IPTOS_PREC_FLASH 0x60
#define IPTOS_PREC_IMMEDIATE 0x40
#define IPTOS_PREC_PRIORITY 0x20
#define IPTOS_PREC_ROUTINE 0x00

/*
 * Differentiated Services Field (DSCP), RFC 2474 / RFC 3168.
 */
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP(x) ((x) & IPTOS_DSCP_MASK)
#define IPTOS_DSCP_AF11 0x28
#define IPTOS_DSCP_AF12 0x30
#define IPTOS_DSCP_AF13 0x38
#define IPTOS_DSCP_AF21 0x48
#define IPTOS_DSCP_AF22 0x50
#define IPTOS_DSCP_AF23 0x58
#define IPTOS_DSCP_AF31 0x68
#define IPTOS_DSCP_AF32 0x70
#define IPTOS_DSCP_AF33 0x78
#define IPTOS_DSCP_AF41 0x88
#define IPTOS_DSCP_AF42 0x90
#define IPTOS_DSCP_AF43 0x98
#define IPTOS_DSCP_EF 0xb8
#define IPTOS_DSCP_CS0 0x00
#define IPTOS_DSCP_CS1 0x20
#define IPTOS_DSCP_CS2 0x40
#define IPTOS_DSCP_CS3 0x60
#define IPTOS_DSCP_CS4 0x80
#define IPTOS_DSCP_CS5 0xa0
#define IPTOS_DSCP_CS6 0xc0
#define IPTOS_DSCP_CS7 0xe0

#define IPTOS_ECN_MASK 0x03
#define IPTOS_ECN(x) ((x) & IPTOS_ECN_MASK)
#define IPTOS_ECN_NOT_ECT 0x00
#define IPTOS_ECN_ECT1 0x01
#define IPTOS_ECN_ECT0 0x02
#define IPTOS_ECN_CE 0x03

/*
 * Definitions for options.
 */
#define IPOPT_COPY 0x80
#define IPOPT_CLASS_MASK 0x60
#define IPOPT_NUMBER_MASK 0x1f
#define IPOPT_CONTROL 0x00
#define IPOPT_RESERVED1 0x20
#define IPOPT_DEBMEAS 0x40
#define IPOPT_RESERVED2 0x60
#define IPOPT_EOL 0    /* end of option list */
#define IPOPT_NOP 1    /* no operation */
#define IPOPT_RR 7     /* record packet route */
#define IPOPT_TS 68    /* timestamp */
#define IPOPT_SECURITY 130 /* provide s,c,h,tcc */
#define IPOPT_LSRR 131 /* loose source route */
#define IPOPT_SATID 136 /* satnet id */
#define IPOPT_SSRR 137 /* strict source route */
#define IPOPT_RA 148   /* router alert */
#define IPOPT_MINOFF 4 /* min value of ptr */

#define MAX_IPOPTLEN 40

/* flag bits for ipt_flg */
#define IPOPT_TS_TSONLY 0    /* timestamps only */
#define IPOPT_TS_TSANDADDR 1 /* timestamps and addresses */
#define IPOPT_TS_PRESPEC 3   /* specified modules only */

__END_DECLS

#endif /* _NETINET_IP_H */
