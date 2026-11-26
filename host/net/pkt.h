/* Building and parsing Ethernet/IPv4/TCP/UDP/ICMP/ARP frames for the
 * network harness's injection tests and script mode (host side).
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_PKT_H
#define HOST_NET_PKT_H

#include <stdint.h>

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10
#define F_URG 0x20

struct tcpseg {
	uint32_t sip, dip; /* host order */
	uint16_t sport, dport;
	uint32_t seq, ack;
	uint8_t flags;
	uint16_t win, urg;
	/* options (in) / parsed options (out) */
	int mss; /* -1 absent */
	int ws; /* -1 absent */
	int sackok;
	int ts;
	uint32_t tsval, tsecr;
	int nsack;
	uint32_t sack[4][2];
	uint8_t raw_opts[40]; /* parse: the option bytes as sent */
	unsigned raw_optlen;
	const uint8_t *data;
	unsigned len;
	/* parse only */
	int csum_ok;
	uint8_t ttl;
};

void tcpseg_init(struct tcpseg *s); /* no options, zero everything */

/* frames: dst MAC, src MAC; return the frame length */
unsigned pkt_tcp(uint8_t *out, const uint8_t dmac[6], const uint8_t smac[6],
		 const struct tcpseg *s);
/* an IPv4 packet (header built here) around an L4 payload; frag is the
 * flags/offset field in host order (0x4000 = DF) */
unsigned pkt_ip(uint8_t *out, const uint8_t dmac[6], const uint8_t smac[6],
		uint32_t sip, uint32_t dip, uint8_t proto, const uint8_t *l4,
		unsigned l4len, uint16_t id, uint16_t frag, uint8_t ttl);
unsigned pkt_udp(uint8_t *out, const uint8_t dmac[6], const uint8_t smac[6],
		 uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport,
		 const uint8_t *data, unsigned len);
unsigned pkt_icmp_echo(uint8_t *out, const uint8_t dmac[6],
		       const uint8_t smac[6], uint32_t sip, uint32_t dip,
		       uint8_t type, uint16_t id, uint16_t seq,
		       const uint8_t *data, unsigned len);
/* an ICMP error (type/code) quoting `quoted' (an IP packet) */
unsigned pkt_icmp_err(uint8_t *out, const uint8_t dmac[6],
		      const uint8_t smac[6], uint32_t sip, uint32_t dip,
		      uint8_t type, uint8_t code, uint32_t aux,
		      const uint8_t *quoted, unsigned qlen);
unsigned pkt_arp(uint8_t *out, int op, const uint8_t smac[6], uint32_t sip,
		 const uint8_t tmac[6], uint32_t tip, const uint8_t dmac[6]);

/* parse: 0 = a TCP segment over IPv4 (fills s), -1 otherwise */
int pkt_parse_tcp(const uint8_t *f, unsigned len, struct tcpseg *s);
/* 0 = UDP over IPv4 (unfragmented) */
int pkt_parse_udp(const uint8_t *f, unsigned len, uint32_t *sip,
		  uint32_t *dip, uint16_t *sport, uint16_t *dport,
		  const uint8_t **data, unsigned *dlen);
/* IPv4 protocol of the frame, -1 if not IPv4; ARP -> 0x806 */
int pkt_proto(const uint8_t *f, unsigned len);
/* "S.", "P.", "F.", "R", ... */
void pkt_flags_str(uint8_t flags, char out[8]);

#endif
