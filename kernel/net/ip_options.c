// LikeOS — IPv4 header options (RFC 791) as a host sees them
//
// Received options are checked for consistency and remembered by offset in
// IPCB(skb)->opt; nothing here rewrites the received header.  A host does
// not forward, so a record-route or timestamp option needs no slot of ours
// filled in, and a source route (LSRR/SSRR) is refused by the input path
// (source routing is off, as it is by default everywhere).  A malformed
// option makes the datagram invalid: the caller answers it with an ICMP
// parameter problem pointing at the byte at fault (rate-limited by
// icmp_send()) and drops it.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/uapi/bug.h>

int ip_options_compile(const uint8_t *iph, struct ip_options *opt,
		       uint32_t *bad)
{
	uint32_t ihl = (uint32_t)(iph[0] & 0x0F) * 4;
	const uint8_t *optptr = iph + IP_HDR_MIN;
	uint32_t l;

	BUG_ON(ihl < IP_HDR_MIN || ihl > IP_HDR_MAX);
	opt->optlen = (uint8_t)(ihl - IP_HDR_MIN);
	opt->srr = opt->rr = opt->ts = opt->router_alert = 0;
	opt->sec = opt->is_strictroute = 0;
	l = opt->optlen;

	while (l > 0) {
		uint32_t optlen;

		switch (*optptr) {
		case IPOPT_END:
			return 0; // the rest is padding
		case IPOPT_NOOP:
			l--;
			optptr++;
			continue;
		default:
			break;
		}
		if (l < 2) {
			*bad = (uint32_t)(optptr - iph);
			return -1;
		}
		optlen = optptr[1];
		if (optlen < 2 || optlen > l) {
			*bad = (uint32_t)(optptr - iph);
			return -1;
		}
		switch (*optptr) {
		case IPOPT_SSRR:
		case IPOPT_LSRR:
			if (optlen < 3) {
				*bad = (uint32_t)(optptr + 1 - iph);
				return -1;
			}
			if (optptr[2] < 4) {
				*bad = (uint32_t)(optptr + 2 - iph);
				return -1;
			}
			if (opt->srr) { // only one source route
				*bad = (uint32_t)(optptr - iph);
				return -1;
			}
			opt->is_strictroute = *optptr == IPOPT_SSRR;
			opt->srr = (uint8_t)(optptr - iph);
			break;
		case IPOPT_RR:
			if (opt->rr) {
				*bad = (uint32_t)(optptr - iph);
				return -1;
			}
			if (optlen < 3) {
				*bad = (uint32_t)(optptr + 1 - iph);
				return -1;
			}
			if (optptr[2] < 4) {
				*bad = (uint32_t)(optptr + 2 - iph);
				return -1;
			}
			/* Room left for an address: the pointer plus the
			 * address must stay inside the option.  A full option
			 * (pointer past its end) is fine. */
			if (optptr[2] <= optlen && (uint32_t)optptr[2] + 3 > optlen) {
				*bad = (uint32_t)(optptr + 2 - iph);
				return -1;
			}
			opt->rr = (uint8_t)(optptr - iph);
			break;
		case IPOPT_TS:
			if (opt->ts) {
				*bad = (uint32_t)(optptr - iph);
				return -1;
			}
			if (optlen < 4) {
				*bad = (uint32_t)(optptr + 1 - iph);
				return -1;
			}
			if (optptr[2] < 5) {
				*bad = (uint32_t)(optptr + 2 - iph);
				return -1;
			}
			if (optptr[2] <= optlen) {
				uint32_t need = 3; // the timestamp itself

				if ((optptr[3] & 0x0F) == IPOPT_TS_TSANDADDR ||
				    (optptr[3] & 0x0F) == IPOPT_TS_PRESPEC)
					need = 7; // address and timestamp
				if ((uint32_t)optptr[2] + need > optlen) {
					*bad = (uint32_t)(optptr + 2 - iph);
					return -1;
				}
			} else if ((optptr[3] & 0x0F) != IPOPT_TS_PRESPEC) {
				/* Full: the overflow counter would count this
				 * hop; at 15 it cannot any more. */
				if ((optptr[3] >> 4) == 15) {
					*bad = (uint32_t)(optptr + 3 - iph);
					return -1;
				}
			}
			opt->ts = (uint8_t)(optptr - iph);
			break;
		case IPOPT_RA:
			if (optlen < 4) {
				*bad = (uint32_t)(optptr + 1 - iph);
				return -1;
			}
			if (optptr[2] == 0 && optptr[3] == 0)
				opt->router_alert = (uint8_t)(optptr - iph);
			break;
		case IPOPT_SEC:
		case IPOPT_SID:
			opt->sec = 1; // accepted, nothing to do as a host
			break;
		default:
			break; // unknown options are ignored (RFC 1122 3.2.1.8)
		}
		l -= optlen;
		optptr += optlen;
	}
	return 0;
}

void ip_options_fragment(uint8_t *iph)
{
	uint32_t ihl = (uint32_t)(iph[0] & 0x0F) * 4;
	uint8_t *optptr = iph + IP_HDR_MIN;
	uint32_t l = ihl > IP_HDR_MIN ? ihl - IP_HDR_MIN : 0;

	while (l > 0) {
		uint32_t optlen;

		if (*optptr == IPOPT_END)
			return;
		if (*optptr == IPOPT_NOOP) {
			l--;
			optptr++;
			continue;
		}
		if (l < 2)
			return;
		optlen = optptr[1];
		if (optlen < 2 || optlen > l)
			return;
		if (!IPOPT_COPIED(*optptr)) {
			for (uint32_t i = 0; i < optlen; i++)
				optptr[i] = IPOPT_NOOP;
		}
		l -= optlen;
		optptr += optlen;
	}
}
