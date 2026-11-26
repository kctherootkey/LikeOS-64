/* Frame builder and parser for the network harness (see pkt.h).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "pkt.h"
#include "sim.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

void tcpseg_init(struct tcpseg *s)
{
	memset(s, 0, sizeof(*s));
	s->mss = -1;
	s->ws = -1;
}

static unsigned eth_hdr(uint8_t *out, const uint8_t dmac[6],
			const uint8_t smac[6], uint16_t type)
{
	memcpy(out, dmac, 6);
	memcpy(out + 6, smac, 6);
	put16(out + 12, type);
	return 14;
}

static uint32_t pseudo(uint32_t sip, uint32_t dip, uint8_t proto, unsigned len)
{
	return (sip >> 16) + (sip & 0xffff) + (dip >> 16) + (dip & 0xffff) +
	       proto + len;
}

unsigned pkt_ip(uint8_t *out, const uint8_t dmac[6], const uint8_t smac[6],
		uint32_t sip, uint32_t dip, uint8_t proto, const uint8_t *l4,
		unsigned l4len, uint16_t id, uint16_t frag, uint8_t ttl)
{
	unsigned o = eth_hdr(out, dmac, smac, 0x0800);
	uint8_t *ip = out + o;

	ip[0] = 0x45;
	ip[1] = 0;
	put16(ip + 2, (uint16_t)(20 + l4len));
	put16(ip + 4, id);
	put16(ip + 6, frag);
	ip[8] = ttl;
	ip[9] = proto;
	put16(ip + 10, 0);
	put32(ip + 12, sip);
	put32(ip + 16, dip);
	put16(ip + 10, inet_csum(ip, 20, 0));
	memcpy(ip + 20, l4, l4len);
	return o + 20 + l4len;
}

unsigned pkt_tcp(uint8_t *out, const uint8_t dmac[6], const uint8_t smac[6],
		 const struct tcpseg *s)
{
	uint8_t l4[1600];
	uint8_t *o = l4 + 20;

	if (s->mss >= 0) {
		o[0] = 2;
		o[1] = 4;
		put16(o + 2, (uint16_t)s->mss);
		o += 4;
	}
	if (s->sackok) {
		o[0] = 1;
		o[1] = 1;
		o[2] = 4;
		o[3] = 2;
		o += 4;
	}
	if (s->ts) {
		o[0] = 1;
		o[1] = 1;
		o[2] = 8;
		o[3] = 10;
		put32(o + 4, s->tsval);
		put32(o + 8, s->tsecr);
		o += 12;
	}
	if (s->ws >= 0) {
		o[0] = 1;
		o[1] = 3;
		o[2] = 3;
		o[3] = (uint8_t)s->ws;
		o += 4;
	}
	if (s->nsack > 0) {
		o[0] = 1;
		o[1] = 1;
		o[2] = 5;
		o[3] = (uint8_t)(2 + 8 * s->nsack);
		o += 4;
		for (int i = 0; i < s->nsack; i++) {
			put32(o, s->sack[i][0]);
			put32(o + 4, s->sack[i][1]);
			o += 8;
		}
	}
	if (s->raw_optlen) {
		memcpy(o, s->raw_opts, s->raw_optlen);
		o += s->raw_optlen;
		while ((o - l4) % 4)
			*o++ = 0;
	}
	unsigned hl = (unsigned)(o - l4);
	put16(l4, s->sport);
	put16(l4 + 2, s->dport);
	put32(l4 + 4, s->seq);
	put32(l4 + 8, s->ack);
	l4[12] = (uint8_t)((hl / 4) << 4);
	l4[13] = s->flags;
	put16(l4 + 14, s->win);
	put16(l4 + 16, 0);
	put16(l4 + 18, s->urg);
	if (s->len)
		memcpy(l4 + hl, s->data, s->len);
	unsigned tl = hl + s->len;
	put16(l4 + 16, inet_csum(l4, tl, pseudo(s->sip, s->dip, 6, tl)));
	return pkt_ip(out, dmac, smac, s->sip, s->dip, 6, l4, tl, 0, 0x4000,
		      64);
}

unsigned pkt_udp(uint8_t *out, const uint8_t dmac[6], const uint8_t smac[6],
		 uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport,
		 const uint8_t *data, unsigned len)
{
	uint8_t l4[1600];

	put16(l4, sport);
	put16(l4 + 2, dport);
	put16(l4 + 4, (uint16_t)(8 + len));
	put16(l4 + 6, 0);
	memcpy(l4 + 8, data, len);
	uint16_t c = inet_csum(l4, 8 + len, pseudo(sip, dip, 17, 8 + len));
	put16(l4 + 6, c ? c : 0xffff);
	return pkt_ip(out, dmac, smac, sip, dip, 17, l4, 8 + len, 1, 0, 64);
}

unsigned pkt_icmp_echo(uint8_t *out, const uint8_t dmac[6],
		       const uint8_t smac[6], uint32_t sip, uint32_t dip,
		       uint8_t type, uint16_t id, uint16_t seq,
		       const uint8_t *data, unsigned len)
{
	uint8_t l4[1600];

	l4[0] = type;
	l4[1] = 0;
	put16(l4 + 2, 0);
	put16(l4 + 4, id);
	put16(l4 + 6, seq);
	memcpy(l4 + 8, data, len);
	put16(l4 + 2, inet_csum(l4, 8 + len, 0));
	return pkt_ip(out, dmac, smac, sip, dip, 1, l4, 8 + len, 2, 0, 64);
}

unsigned pkt_icmp_err(uint8_t *out, const uint8_t dmac[6],
		      const uint8_t smac[6], uint32_t sip, uint32_t dip,
		      uint8_t type, uint8_t code, uint32_t aux,
		      const uint8_t *quoted, unsigned qlen)
{
	uint8_t l4[1600];

	if (qlen > 548)
		qlen = 548;
	l4[0] = type;
	l4[1] = code;
	put16(l4 + 2, 0);
	put32(l4 + 4, aux);
	memcpy(l4 + 8, quoted, qlen);
	put16(l4 + 2, inet_csum(l4, 8 + qlen, 0));
	return pkt_ip(out, dmac, smac, sip, dip, 1, l4, 8 + qlen, 3, 0, 64);
}

unsigned pkt_arp(uint8_t *out, int op, const uint8_t smac[6], uint32_t sip,
		 const uint8_t tmac[6], uint32_t tip, const uint8_t dmac[6])
{
	unsigned o = eth_hdr(out, dmac, smac, 0x0806);
	uint8_t *a = out + o;

	put16(a, 1);
	put16(a + 2, 0x0800);
	a[4] = 6;
	a[5] = 4;
	put16(a + 6, (uint16_t)op);
	memcpy(a + 8, smac, 6);
	put32(a + 14, sip);
	memcpy(a + 18, tmac, 6);
	put32(a + 24, tip);
	return o + 28;
}

int pkt_proto(const uint8_t *f, unsigned len)
{
	if (len < 14)
		return -1;
	uint16_t et = get16(f + 12);
	if (et == 0x0806)
		return 0x806;
	if (et != 0x0800 || len < 34)
		return -1;
	return f[23];
}

int pkt_parse_tcp(const uint8_t *f, unsigned len, struct tcpseg *s)
{
	tcpseg_init(s);
	if (pkt_proto(f, len) != 6)
		return -1;
	const uint8_t *ip = f + 14;
	unsigned ihl = (ip[0] & 15) * 4u, tot = get16(ip + 2);
	if (ihl < 20 || tot < ihl + 20 || 14 + tot > len)
		return -1;
	const uint8_t *t = ip + ihl;
	unsigned hl = (unsigned)(t[12] >> 4) * 4u;
	if (hl < 20 || ihl + hl > tot)
		return -1;
	s->sip = get32(ip + 12);
	s->dip = get32(ip + 16);
	s->ttl = ip[8];
	s->sport = get16(t);
	s->dport = get16(t + 2);
	s->seq = get32(t + 4);
	s->ack = get32(t + 8);
	s->flags = t[13];
	s->win = get16(t + 14);
	s->urg = get16(t + 18);
	s->data = t + hl;
	s->len = tot - ihl - hl;
	s->csum_ok = !(frame_csum_bad(f, len) & 2);
	s->raw_optlen = hl - 20;
	memcpy(s->raw_opts, t + 20, s->raw_optlen);
	for (unsigned i = 20; i < hl;) {
		uint8_t k = t[i];
		if (k == 0)
			break;
		if (k == 1) {
			i++;
			continue;
		}
		if (i + 1 >= hl)
			break;
		unsigned ol = t[i + 1];
		if (ol < 2 || i + ol > hl)
			break;
		if (k == 2 && ol == 4)
			s->mss = get16(t + i + 2);
		else if (k == 3 && ol == 3)
			s->ws = t[i + 2];
		else if (k == 4 && ol == 2)
			s->sackok = 1;
		else if (k == 8 && ol == 10) {
			s->ts = 1;
			s->tsval = get32(t + i + 2);
			s->tsecr = get32(t + i + 6);
		} else if (k == 5 && ol >= 10) {
			s->nsack = (int)((ol - 2) / 8);
			if (s->nsack > 4)
				s->nsack = 4;
			for (int j = 0; j < s->nsack; j++) {
				s->sack[j][0] = get32(t + i + 2 + 8 * j);
				s->sack[j][1] = get32(t + i + 6 + 8 * j);
			}
		}
		i += ol;
	}
	return 0;
}

int pkt_parse_udp(const uint8_t *f, unsigned len, uint32_t *sip,
		  uint32_t *dip, uint16_t *sport, uint16_t *dport,
		  const uint8_t **data, unsigned *dlen)
{
	if (pkt_proto(f, len) != 17)
		return -1;
	const uint8_t *ip = f + 14;
	unsigned ihl = (ip[0] & 15) * 4u, tot = get16(ip + 2);
	if (ihl < 20 || tot < ihl + 8 || 14 + tot > len)
		return -1;
	if ((get16(ip + 6) & 0x3fff) != 0)
		return -1;
	const uint8_t *u = ip + ihl;
	*sip = get32(ip + 12);
	*dip = get32(ip + 16);
	*sport = get16(u);
	*dport = get16(u + 2);
	*data = u + 8;
	*dlen = tot - ihl - 8;
	return 0;
}

void pkt_flags_str(uint8_t flags, char out[8])
{
	int n = 0;

	if (flags & F_SYN)
		out[n++] = 'S';
	if (flags & F_FIN)
		out[n++] = 'F';
	if (flags & F_RST)
		out[n++] = 'R';
	if (flags & F_PSH)
		out[n++] = 'P';
	if (flags & F_URG)
		out[n++] = 'U';
	if (flags & F_ACK)
		out[n++] = '.';
	out[n] = 0;
}
