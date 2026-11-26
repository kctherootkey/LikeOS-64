// LikeOS — NIC driver helpers
//
// Pure functions the network drivers share: address-filter hashes, receive
// descriptor status decoding, interrupt-moderation decisions and ring index
// arithmetic.  Nothing here touches hardware or kernel state, so the same
// header is compiled by the host tests (host/net/drivers) with
// NIC_UTIL_HOST defined.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_NIC_UTIL_H
#define LIKEOS_NIC_UTIL_H

#ifdef NIC_UTIL_HOST
#include <stdint.h>
#else
#include <kernel/uapi/types.h>
#endif

// ============================================================================
// CRC-32 (IEEE 802.3 polynomial) as the MAC address filters use it
// ============================================================================

/* The Ethernet CRC register after `len' bytes: initial value all ones, data
 * bits taken least significant first, the reflected register (shifting
 * right), no final inversion.  The filters of the AMD and DEC chips index
 * their hash tables with bits of this value. */
static inline uint32_t nic_crc32_le(const uint8_t *p, int len)
{
	uint32_t crc = 0xFFFFFFFFu;

	for (int i = 0; i < len; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
	}
	return crc;
}

/* The same CRC with the register shifting left (polynomial 0x04C11DB7);
 * data bits are still taken least significant first.  The Realtek and
 * National 8390 filters index with its top six bits. */
static inline uint32_t nic_crc32_be(const uint8_t *p, int len)
{
	uint32_t crc = 0xFFFFFFFFu;

	for (int i = 0; i < len; i++) {
		uint8_t o = p[i];

		for (int b = 0; b < 8; b++, o >>= 1) {
			uint32_t fb = (crc >> 31) ^ (o & 1u);

			crc = (crc << 1) ^ (fb ? 0x04C11DB7u : 0u);
		}
	}
	return crc;
}

/* Bit (0..63) of a 64-bit multicast filter indexed by the top six bits of
 * the left-shifting CRC: the rtl8139 MAR0..7 and the 8390 MAR registers
 * (byte bit/8, bit bit%8). */
static inline uint32_t nic_mc_hash64_be(const uint8_t addr[6])
{
	return nic_crc32_be(addr, 6) >> 26;
}

/* Bit (0..63) of the PCnet logical address filter (LADRF) for `addr': the
 * top six bits of the reflected CRC.  Word bit/16, bit bit%16. */
static inline uint32_t nic_mc_hash64_le(const uint8_t addr[6])
{
	return nic_crc32_le(addr, 6) >> 26;
}

static inline int nic_is_multicast(const uint8_t addr[6])
{
	return addr[0] & 1;
}

// ============================================================================
// Intel multicast table array (MTA)
// ============================================================================

/* Bit of the multicast table for `addr' with RCTL.MO = 0, for a table of
 * `nregs' 32-bit registers: 128 on the 8254x, 8257x and 82576 (a 12-bit
 * hash of address bits 36..47), 32 on the ICH/PCH LAN MACs (10 bits,
 * 38..47).  Register bit>>5, bit bit&31. */
static inline uint32_t nic_mta_hash(const uint8_t addr[6], uint32_t nregs)
{
	uint32_t bits = 0, shift;

	while ((1u << bits) < nregs * 32u)
		bits++; // 12 or 10
	shift = bits - 8; // how far byte 5 sits above byte 4's top bits
	return (((uint32_t)addr[4] >> (8 - shift)) |
		((uint32_t)addr[5] << shift)) &
	       ((1u << bits) - 1);
}

// ============================================================================
// Intel 8254x/8257x/ICH receive descriptor status (legacy layout; the
// advanced layout of the 82576 has the same bits, errors in bits 31:24)
// ============================================================================
#define NIC_E1000_RXS_DD 0x01
#define NIC_E1000_RXS_EOP 0x02
#define NIC_E1000_RXS_IXSM 0x04 // ignore the checksum indications
#define NIC_E1000_RXS_VP 0x08
#define NIC_E1000_RXS_UDPCS 0x10 // UDP checksum computed (8257x and later)
#define NIC_E1000_RXS_TCPCS 0x20 // TCP (8254x: TCP or UDP) checksum computed
#define NIC_E1000_RXS_IPCS 0x40
#define NIC_E1000_RXE_CE 0x01 // CRC / alignment
#define NIC_E1000_RXE_SE 0x02 // symbol
#define NIC_E1000_RXE_SEQ 0x04 // sequence
#define NIC_E1000_RXE_CXE 0x10 // carrier extension
#define NIC_E1000_RXE_TCPE 0x20 // TCP/UDP checksum wrong
#define NIC_E1000_RXE_IPE 0x40 // IP header checksum wrong
#define NIC_E1000_RXE_RXE 0x80 // data error
#define NIC_E1000_RXE_FRAME_MASK                                            \
	(NIC_E1000_RXE_CE | NIC_E1000_RXE_SE | NIC_E1000_RXE_SEQ | \
	 NIC_E1000_RXE_CXE | NIC_E1000_RXE_RXE)

#define NIC_RXV_DELIVER 0 // a good, complete frame
#define NIC_RXV_ERROR 1 // a frame the hardware flagged as bad
#define NIC_RXV_FRAGMENT 2 // part of a frame larger than one buffer

/* What to do with a completed receive descriptor.  Every frame must fit in
 * one buffer: a descriptor without EOP starts a frame that is thrown away
 * descriptor by descriptor up to and including its EOP piece, which
 * `*discarding' tracks across calls. */
static inline int nic_e1000_rx_verdict(uint8_t status, uint8_t errors,
				       uint32_t len, uint32_t bufsize,
				       int *discarding)
{
	if (!(status & NIC_E1000_RXS_EOP)) {
		*discarding = 1;
		return NIC_RXV_FRAGMENT;
	}
	if (*discarding) {
		*discarding = 0;
		return NIC_RXV_FRAGMENT;
	}
	if (errors & NIC_E1000_RXE_FRAME_MASK)
		return NIC_RXV_ERROR;
	if (len == 0 || len > bufsize)
		return NIC_RXV_ERROR;
	return NIC_RXV_DELIVER;
}

#define NIC_CSUM_NONE 0 // not checked by the hardware: software verifies
#define NIC_CSUM_OK 1 // TCP/UDP checksum verified good
#define NIC_CSUM_BAD (-1) // the hardware found it wrong (software decides)

/* The transport checksum verdict of a received frame.  `udpcs' says whether
 * the MAC reports UDP separately (8257x/ICH/82576) or folds it into TCPCS
 * (8254x); `ipe_counts' whether an IP header error disqualifies the frame
 * too (8257x and later report both through the same check). */
static inline int nic_e1000_rx_csum(uint8_t status, uint8_t errors, int udpcs,
				    int ipe_counts)
{
	uint8_t done = NIC_E1000_RXS_TCPCS | (udpcs ? NIC_E1000_RXS_UDPCS : 0);
	uint8_t bad = NIC_E1000_RXE_TCPE | (ipe_counts ? NIC_E1000_RXE_IPE : 0);

	if (status & NIC_E1000_RXS_IXSM)
		return NIC_CSUM_NONE;
	if (errors & bad)
		return NIC_CSUM_BAD;
	if (!(status & done))
		return NIC_CSUM_NONE;
	return NIC_CSUM_OK;
}

// ============================================================================
// Interrupt ownership on a shared legacy (INTx) line
// ============================================================================
/* The dispatcher offers an interrupt on a shared line to one handler after
 * the other until one claims it, so a handler claims only what its device
 * raised: a cause that is enabled, or the device's own "asserted" flag.  A
 * cause latched while masked (receive work during a NAPI poll) does not
 * make another device's interrupt this one's.  Whatever was read is still
 * handled (the read cleared it); only the claim depends on this. */
#define NIC_E1000_ICR_INT_ASSERTED 0x80000000u // 8257x, 82576 and later

/* Intel 8254x/8257x/82576/ICH: ICR as read, IMS as read after it.  All
 * ones: the device is gone from the bus. */
static inline int nic_e1000_irq_ours(uint32_t icr, uint32_t ims)
{
	if (icr == 0xFFFFFFFFu)
		return 0;
	return (icr & NIC_E1000_ICR_INT_ASSERTED) || (icr & ims & 0x7FFFFFFFu);
}

/* rtl8139: ISR as read, IMR. */
static inline int nic_rtl8139_irq_ours(uint16_t isr, uint16_t imr)
{
	if (isr == 0xFFFFu)
		return 0;
	return (isr & imr) != 0;
}

// ============================================================================
// Ring arithmetic
// ============================================================================

/* Free descriptors of a ring of `size' where software fills at `ntu' (next
 * to use) and reclaims at `ntc' (next to clean); one slot always stays
 * empty so a full ring does not look like an empty one. */
static inline uint32_t nic_ring_unused(uint32_t ntu, uint32_t ntc,
				       uint32_t size)
{
	return ((ntc > ntu) ? 0 : size) + ntc - ntu - 1;
}

/* Advance a ring index; flip the generation bit when it wraps (vmxnet3). */
static inline void nic_ring_adv_gen(uint32_t *idx, uint32_t *gen,
				    uint32_t size)
{
	if (++*idx == size) {
		*idx = 0;
		*gen ^= 1u;
	}
}

// ============================================================================
// Interrupt moderation (Intel ITR): the adaptive, "conservative" policy
// ============================================================================
#define NIC_ITR_LOWEST 0 // ~70000 ints/s (conservative: like LOW)
#define NIC_ITR_LOW 1 // ~20000 ints/s
#define NIC_ITR_BULK 2 // ~4000 ints/s

#define NIC_ITR_RATE_LOWEST 70000u
#define NIC_ITR_RATE_LOW 20000u
#define NIC_ITR_RATE_BULK 4000u

/* The traffic class of one interrupt period from the frames and bytes it
 * moved, given the class of the previous period: small sparse frames want
 * low latency, many or large frames want fewer interrupts.  No traffic
 * keeps the class. */
static inline int nic_itr_classify(int cls, uint32_t packets, uint32_t bytes)
{
	uint32_t avg;

	if (packets == 0)
		return cls;
	avg = bytes / packets;
	switch (cls) {
	case NIC_ITR_LOWEST:
		if (avg > 8000)
			return NIC_ITR_BULK;
		if (packets < 5 && bytes > 512)
			return NIC_ITR_LOW;
		return NIC_ITR_LOWEST;
	case NIC_ITR_LOW:
		if (bytes > 10000) {
			if (avg > 8000 || packets < 10 || avg > 1200)
				return NIC_ITR_BULK;
			if (packets > 35)
				return NIC_ITR_LOWEST;
			return NIC_ITR_LOW;
		}
		if (avg > 2000)
			return NIC_ITR_BULK;
		if (packets <= 2 && bytes < 512)
			return NIC_ITR_LOWEST;
		return NIC_ITR_LOW;
	default: // NIC_ITR_BULK
		if (bytes > 25000) {
			if (packets > 35)
				return NIC_ITR_LOW;
			return NIC_ITR_BULK;
		}
		if (bytes < 6000)
			return NIC_ITR_LOW;
		return NIC_ITR_BULK;
	}
}

/* Interrupts per second for a class; `conservative' keeps the lowest
 * latency class at the LOW rate (the default policy). */
static inline uint32_t nic_itr_class_rate(int cls, int conservative)
{
	switch (cls) {
	case NIC_ITR_LOWEST:
		return conservative ? NIC_ITR_RATE_LOW : NIC_ITR_RATE_LOWEST;
	case NIC_ITR_BULK:
		return NIC_ITR_RATE_BULK;
	default:
		return NIC_ITR_RATE_LOW;
	}
}

/* The next rate from the current one and the target: a higher rate is
 * approached in steps of a quarter of the target (bursts of small frames
 * do not swing it at once), a lower one is taken at once. */
static inline uint32_t nic_itr_next_rate(uint32_t cur, uint32_t target)
{
	if (target > cur) {
		uint32_t step = cur + (target >> 2);

		return step < target ? step : target;
	}
	return target;
}

/* The ITR register value (interval in 256 ns units) for a rate. */
static inline uint32_t nic_itr_reg(uint32_t rate)
{
	if (rate == 0)
		return 0;
	return 1000000000u / (rate * 256u);
}

// ============================================================================
// Latency Tolerance Reporting (I217 and later PCH LAN)
// ============================================================================
#define NIC_LTRV_VALUE_MASK 0x03FFu
#define NIC_LTRV_SCALE_SHIFT 10
#define NIC_LTRV_SCALE_MASK 0x1C00u
#define NIC_LTRV_SCALE_MAX 5

/* Nanoseconds an encoded latency (10-bit value, 3-bit scale of 2^(5*s) ns)
 * stands for. */
static inline uint64_t nic_ltr_decode_ns(uint16_t enc)
{
	uint32_t scale = (enc & NIC_LTRV_SCALE_MASK) >> NIC_LTRV_SCALE_SHIFT;

	return (uint64_t)(enc & NIC_LTRV_VALUE_MASK) << (5 * scale);
}

/* The latency the receive packet buffer tolerates at a link speed: the time
 * to fill what is left of `rxa_kb' KB once one maximum frame is in it,
 * encoded, and capped to the platform's maximum (`platform_max', already
 * encoded, from PCI config space; a platform that reports none, 0, gets a
 * zero tolerance -- it then never sleeps deep enough to lose frames).
 * Returns 0xFFFF when the value cannot be encoded. */
static inline uint16_t nic_ltr_encode(uint32_t rxa_kb, uint32_t max_frame,
				      uint32_t speed_mbps,
				      uint16_t platform_max)
{
	uint64_t value, rxa = (uint64_t)rxa_kb * 512u;
	uint32_t scale = 0;
	uint16_t enc;

	if (speed_mbps == 0)
		return 0xFFFF;
	value = rxa > max_frame ? (rxa - max_frame) * (16000u / speed_mbps) :
				  0;
	while (value > NIC_LTRV_VALUE_MASK) {
		scale++;
		value = (value + 31) >> 5; // round up
	}
	if (scale > NIC_LTRV_SCALE_MAX)
		return 0xFFFF;
	enc = (uint16_t)((scale << NIC_LTRV_SCALE_SHIFT) | value);
	if (nic_ltr_decode_ns(enc) > nic_ltr_decode_ns(platform_max))
		enc = platform_max;
	return enc;
}

// ============================================================================
// IEEE 802.3 Annex 28B pause resolution
// ============================================================================
#define NIC_FC_NONE 0
#define NIC_FC_RX 1 // honour received pause frames
#define NIC_FC_TX 2 // send pause frames
#define NIC_FC_FULL 3

#define NIC_ADV_PAUSE 0x0400 // MII advertisement: symmetric pause
#define NIC_ADV_ASM_DIR 0x0800 // asymmetric pause

/* Flow control after autonegotiation from our advertisement (`local') and
 * the partner's (`remote'), when full flow control was requested. */
static inline int nic_fc_resolve(uint16_t local, uint16_t remote,
				 int full_duplex)
{
	int lsym = !!(local & NIC_ADV_PAUSE), lasm = !!(local & NIC_ADV_ASM_DIR);
	int rsym = !!(remote & NIC_ADV_PAUSE),
	    rasm = !!(remote & NIC_ADV_ASM_DIR);

	if (!full_duplex)
		return NIC_FC_NONE;
	if (lsym && rsym)
		return NIC_FC_FULL;
	if (!lsym && lasm && rsym && rasm)
		return NIC_FC_TX;
	if (lsym && lasm && !rsym && rasm)
		return NIC_FC_RX;
	return NIC_FC_NONE;
}

// ============================================================================
// vmxnet3 receive completion
// ============================================================================
#define NIC_VMX_RCD_CNC (1u << 30) // word 0: checksum not calculated
#define NIC_VMX_RCD_TUC (1u << 16) // word 3: TCP/UDP checksum correct
#define NIC_VMX_RCD_UDP (1u << 17)
#define NIC_VMX_RCD_TCP (1u << 18)
#define NIC_VMX_RCD_IPC (1u << 19) // IP header checksum correct
#define NIC_VMX_RCD_V4 (1u << 21)
#define NIC_VMX_RCD_FRG (1u << 22) // an IP fragment

/* 1 when the device verified the TCP/UDP checksum of an unfragmented IPv4
 * frame, 0 when software has to. */
static inline int nic_vmxnet3_rx_csum_ok(uint32_t w0, uint32_t w3)
{
	if (w0 & NIC_VMX_RCD_CNC)
		return 0;
	if (!(w3 & NIC_VMX_RCD_V4) || (w3 & NIC_VMX_RCD_FRG))
		return 0;
	if (!(w3 & (NIC_VMX_RCD_TCP | NIC_VMX_RCD_UDP)))
		return 0;
	return (w3 & (NIC_VMX_RCD_IPC | NIC_VMX_RCD_TUC)) ==
	       (NIC_VMX_RCD_IPC | NIC_VMX_RCD_TUC);
}

// ============================================================================
// rtl8139 receive ring
// ============================================================================
#define NIC_RTL_RX_EARLY_LEN 0xFFF0u // header length while DMA is under way

/* The ring offset of the frame after the one at `off' whose header says
 * `len' bytes (frame + CRC): the 4-byte header, the frame, rounded up to a
 * dword, modulo the ring length. */
static inline uint32_t nic_rtl8139_rx_next(uint32_t off, uint32_t len,
					   uint32_t ring_len)
{
	return ((off + len + 4u + 3u) & ~3u) % ring_len;
}

/* The CAPR value that releases everything before `off' (the chip keeps it
 * 16 bytes behind; the 16-bit register wraps, so offset 0 is 0xFFF0). */
static inline uint16_t nic_rtl8139_capr(uint32_t off)
{
	return (uint16_t)(off - 16u);
}

// ============================================================================
// DEC 21x4x setup frame (192 bytes, 48 longwords; each 16-bit filter word
// sits in both halves of its longword)
// ============================================================================
#define NIC_TULIP_SETUP_LEN 192
#define NIC_TULIP_PERFECT_MAX 14 // multicast addresses beside ours + broadcast

static inline void nic_tulip_put_addr(uint32_t *lw, const uint8_t a[6])
{
	for (int i = 0; i < 3; i++) {
		uint32_t w = (uint32_t)a[2 * i] | ((uint32_t)a[2 * i + 1] << 8);

		lw[i] = w | (w << 16);
	}
}

/* Perfect filtering: up to 14 multicast addresses, the unused entries the
 * broadcast address, the 16th entry our own address. */
static inline void nic_tulip_setup_perfect(uint32_t frame[48],
					   const uint8_t own[6],
					   const uint8_t (*mc)[6], int n)
{
	static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF,
					  0xFF, 0xFF, 0xFF };

	for (int e = 0; e < 15; e++)
		nic_tulip_put_addr(&frame[3 * e], e < n ? mc[e] : bcast);
	nic_tulip_put_addr(&frame[45], own);
}

/* Hash filtering (512-bit table indexed by the low nine bits of the
 * reflected CRC, broadcast included) plus our own address in entry 13. */
static inline void nic_tulip_setup_hash(uint32_t frame[48],
					const uint8_t own[6],
					const uint8_t (*mc)[6], int n)
{
	static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF,
					  0xFF, 0xFF, 0xFF };
	uint16_t table[32];

	for (int i = 0; i < 32; i++)
		table[i] = 0;
	for (int i = 0; i <= n; i++) {
		uint32_t idx = nic_crc32_le(i < n ? mc[i] : bcast, 6) & 0x1FF;

		table[idx >> 4] |= (uint16_t)(1u << (idx & 15));
	}
	for (int i = 0; i < 48; i++)
		frame[i] = 0;
	for (int i = 0; i < 32; i++)
		frame[i] = (uint32_t)table[i] | ((uint32_t)table[i] << 16);
	nic_tulip_put_addr(&frame[39], own);
}

#endif // LIKEOS_NIC_UTIL_H
