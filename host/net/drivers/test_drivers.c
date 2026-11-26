// LikeOS — host tests of the NIC drivers' pure logic (kernel/dev/nic/nic_util.h)
//
// Address-filter hashes (Intel MTA, PCnet LADRF, Realtek/8390 MAR, DEC
// setup frames), receive descriptor decoding, interrupt moderation, LTR
// encoding, pause resolution, ring index and generation arithmetic,
// vmxnet3 checksum flags, the rtl8139 ring walk and the shared-line
// interrupt claim.  Built and run by
// host/test-net.sh drivers, with AddressSanitizer and UBSan.
//
// Copyright (C) 2026 The LikeOS Project

#define NIC_UTIL_HOST 1
#include "kernel/dev/nic/nic_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// multicast groups used in the setup-frame tests (the stack's list holds 32)
#define NETDEV_MC_TEST 32

static int failures, checks;

#define CHECK(cond)                                                     \
	do {                                                            \
		checks++;                                               \
		if (!(cond)) {                                          \
			failures++;                                     \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n",    \
				__FILE__, __LINE__, #cond);             \
		}                                                       \
	} while (0)

#define CHECK_EQ(a, b)                                                    \
	do {                                                              \
		unsigned long long va_ = (unsigned long long)(a);         \
		unsigned long long vb_ = (unsigned long long)(b);         \
		checks++;                                                 \
		if (va_ != vb_) {                                         \
			failures++;                                       \
			fprintf(stderr,                                   \
				"%s:%d: %s == %s failed: %llx != %llx\n", \
				__FILE__, __LINE__, #a, #b, va_, vb_);    \
		}                                                         \
	} while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 16);
}

static uint32_t bitrev32(uint32_t v)
{
	uint32_t r = 0;

	for (int i = 0; i < 32; i++)
		if (v & (1u << i))
			r |= 1u << (31 - i);
	return r;
}

/* The zlib-convention CRC-32, table driven: an independent cross-check. */
static uint32_t crc32_zlib(const uint8_t *p, size_t n)
{
	static uint32_t table[256];
	static int init;
	uint32_t c = 0xFFFFFFFFu;

	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;

			for (int k = 0; k < 8; k++)
				v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			table[i] = v;
		}
		init = 1;
	}
	for (size_t i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

static void test_crc(void)
{
	const uint8_t check[] = "123456789";
	const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

	// the CRC-32 check value, without the final inversion
	CHECK_EQ(nic_crc32_le(check, 9), ~0xCBF43926u);
	// the left-shifting register is the mirror image of the reflected one
	for (int t = 0; t < 2000; t++) {
		uint8_t buf[16];
		int n = (int)(rnd() % 16);

		for (int i = 0; i < n; i++)
			buf[i] = (uint8_t)rnd();
		CHECK_EQ(nic_crc32_le(buf, n), ~crc32_zlib(buf, (size_t)n));
		CHECK_EQ(nic_crc32_be(buf, n), bitrev32(nic_crc32_le(buf, n)));
	}
	// the broadcast address is bit 255 of the DEC 512-bit hash
	CHECK_EQ(nic_crc32_le(bcast, 6) & 0x1FF, 255);
	for (int t = 0; t < 1000; t++) {
		uint8_t a[6];

		for (int i = 0; i < 6; i++)
			a[i] = (uint8_t)rnd();
		CHECK(nic_mc_hash64_le(a) < 64);
		CHECK(nic_mc_hash64_be(a) < 64);
		CHECK_EQ(nic_mc_hash64_be(a), nic_crc32_be(a, 6) >> 26);
	}
}

static void test_mta(void)
{
	const uint8_t allhosts[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0x01 };
	const uint8_t mdns[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB };

	// 4096-bit table: address bits 36..47
	CHECK_EQ(nic_mta_hash(allhosts, 128), 0x010);
	CHECK_EQ(nic_mta_hash(mdns, 128), 0xFB0);
	// 1024-bit table (ICH/PCH): bits 38..47
	CHECK_EQ(nic_mta_hash(allhosts, 32), 0x004);
	CHECK_EQ(nic_mta_hash(mdns, 32), 0x3EC);
	for (int t = 0; t < 5000; t++) {
		uint8_t a[6];

		for (int i = 0; i < 6; i++)
			a[i] = (uint8_t)rnd();
		uint32_t h128 = nic_mta_hash(a, 128), h32 = nic_mta_hash(a, 32);

		CHECK(h128 < 4096);
		CHECK(h32 < 1024);
		CHECK_EQ(h128, (((uint32_t)a[5] << 8) | a[4]) >> 4);
		CHECK_EQ(h32, (((uint32_t)a[5] << 8) | a[4]) >> 6);
	}
}

static void test_rx_verdict(void)
{
	int disc = 0;
	const uint8_t eop = NIC_E1000_RXS_DD | NIC_E1000_RXS_EOP;
	const uint8_t mid = NIC_E1000_RXS_DD;

	CHECK_EQ(nic_e1000_rx_verdict(eop, 0, 60, 2048, &disc),
		 NIC_RXV_DELIVER);
	CHECK_EQ(disc, 0);
	// a frame spanning three buffers is dropped piece by piece
	CHECK_EQ(nic_e1000_rx_verdict(mid, 0, 2048, 2048, &disc),
		 NIC_RXV_FRAGMENT);
	CHECK_EQ(disc, 1);
	CHECK_EQ(nic_e1000_rx_verdict(mid, 0, 2048, 2048, &disc),
		 NIC_RXV_FRAGMENT);
	CHECK_EQ(nic_e1000_rx_verdict(eop, 0, 100, 2048, &disc),
		 NIC_RXV_FRAGMENT);
	CHECK_EQ(disc, 0);
	CHECK_EQ(nic_e1000_rx_verdict(eop, 0, 1514, 2048, &disc),
		 NIC_RXV_DELIVER);
	// frame errors
	CHECK_EQ(nic_e1000_rx_verdict(eop, NIC_E1000_RXE_CE, 64, 2048, &disc),
		 NIC_RXV_ERROR);
	CHECK_EQ(nic_e1000_rx_verdict(eop, NIC_E1000_RXE_RXE, 64, 2048,
				      &disc),
		 NIC_RXV_ERROR);
	// a checksum error is not a frame error
	CHECK_EQ(nic_e1000_rx_verdict(eop, NIC_E1000_RXE_TCPE, 64, 2048,
				      &disc),
		 NIC_RXV_DELIVER);
	CHECK_EQ(nic_e1000_rx_verdict(eop, 0, 0, 2048, &disc), NIC_RXV_ERROR);
	CHECK_EQ(nic_e1000_rx_verdict(eop, 0, 2049, 2048, &disc),
		 NIC_RXV_ERROR);
}

static void test_rx_csum(void)
{
	const uint8_t base = NIC_E1000_RXS_DD | NIC_E1000_RXS_EOP;

	CHECK_EQ(nic_e1000_rx_csum(base | NIC_E1000_RXS_TCPCS, 0, 1, 1),
		 NIC_CSUM_OK);
	CHECK_EQ(nic_e1000_rx_csum(base | NIC_E1000_RXS_UDPCS, 0, 1, 1),
		 NIC_CSUM_OK);
	// 8254x: no separate UDP bit
	CHECK_EQ(nic_e1000_rx_csum(base | NIC_E1000_RXS_UDPCS, 0, 0, 0),
		 NIC_CSUM_NONE);
	CHECK_EQ(nic_e1000_rx_csum(base | NIC_E1000_RXS_TCPCS |
					   NIC_E1000_RXS_IXSM,
				   0, 1, 1),
		 NIC_CSUM_NONE);
	CHECK_EQ(nic_e1000_rx_csum(base | NIC_E1000_RXS_TCPCS,
				   NIC_E1000_RXE_TCPE, 1, 1),
		 NIC_CSUM_BAD);
	CHECK_EQ(nic_e1000_rx_csum(base | NIC_E1000_RXS_TCPCS |
					   NIC_E1000_RXS_IPCS,
				   NIC_E1000_RXE_IPE, 1, 1),
		 NIC_CSUM_BAD);
	// 8254x: an IP header error alone does not condemn the TCP sum
	CHECK_EQ(nic_e1000_rx_csum(base | NIC_E1000_RXS_TCPCS,
				   NIC_E1000_RXE_IPE, 0, 0),
		 NIC_CSUM_OK);
	CHECK_EQ(nic_e1000_rx_csum(base, 0, 1, 1), NIC_CSUM_NONE);
}

static void test_rings(void)
{
	// one slot always empty
	CHECK_EQ(nic_ring_unused(0, 0, 256), 255);
	CHECK_EQ(nic_ring_unused(255, 0, 256), 0);
	CHECK_EQ(nic_ring_unused(5, 6, 256), 0);
	CHECK_EQ(nic_ring_unused(6, 5, 256), 254);
	CHECK_EQ(nic_ring_unused(0, 200, 256), 199);
	// against a model of fill/clean
	for (uint32_t size = 2; size <= 40; size++) {
		uint32_t ntu = 0, ntc = 0, used = 0;

		for (int step = 0; step < 4000; step++) {
			CHECK_EQ(nic_ring_unused(ntu, ntc, size),
				 size - 1 - used);
			if ((rnd() & 1) && used < size - 1) {
				ntu = (ntu + 1) % size;
				used++;
			} else if (used) {
				ntc = (ntc + 1) % size;
				used--;
			}
		}
	}
	// generation bits flip once per lap
	{
		uint32_t idx = 0, gen = 1, flips = 0, last = 1;

		for (int i = 0; i < 3 * 1024; i++) {
			nic_ring_adv_gen(&idx, &gen, 1024);
			if (gen != last) {
				flips++;
				last = gen;
				CHECK_EQ(idx, 0);
			}
			CHECK(idx < 1024);
		}
		CHECK_EQ(flips, 3);
		CHECK_EQ(gen, 0);
	}
}

static void test_itr(void)
{
	// the interval register for the three rates (256 ns units)
	CHECK_EQ(nic_itr_reg(20000), 195);
	CHECK_EQ(nic_itr_reg(4000), 976);
	CHECK_EQ(nic_itr_reg(70000), 55);
	CHECK_EQ(nic_itr_reg(0), 0);
	// conservative: lowest latency served at the low-latency rate
	CHECK_EQ(nic_itr_class_rate(NIC_ITR_LOWEST, 1), 20000);
	CHECK_EQ(nic_itr_class_rate(NIC_ITR_LOWEST, 0), 70000);
	CHECK_EQ(nic_itr_class_rate(NIC_ITR_BULK, 1), 4000);
	// no traffic: class kept
	CHECK_EQ(nic_itr_classify(NIC_ITR_BULK, 0, 0), NIC_ITR_BULK);
	// a bulk transfer: 64 full frames in one period
	CHECK_EQ(nic_itr_classify(NIC_ITR_LOW, 64, 64 * 1514), NIC_ITR_BULK);
	// pings: one small frame
	CHECK_EQ(nic_itr_classify(NIC_ITR_LOW, 1, 98), NIC_ITR_LOWEST);
	CHECK_EQ(nic_itr_classify(NIC_ITR_BULK, 2, 200), NIC_ITR_LOW);
	// many small frames (request/response, ACKs)
	CHECK_EQ(nic_itr_classify(NIC_ITR_LOW, 40, 40 * 300), NIC_ITR_LOWEST);
	CHECK_EQ(nic_itr_classify(NIC_ITR_BULK, 40, 40 * 1000), NIC_ITR_LOW);
	CHECK_EQ(nic_itr_classify(NIC_ITR_LOWEST, 3, 3000), NIC_ITR_LOW);
	CHECK_EQ(nic_itr_classify(NIC_ITR_LOWEST, 1, 9000), NIC_ITR_BULK);
	// rising rates step by a quarter of the target, falling ones jump
	CHECK_EQ(nic_itr_next_rate(4000, 20000), 9000);
	CHECK_EQ(nic_itr_next_rate(19000, 20000), 20000);
	CHECK_EQ(nic_itr_next_rate(20000, 4000), 4000);
	// the walk from bulk to low converges in a few periods
	{
		uint32_t r = 4000;
		int n = 0;

		while (r != 20000 && n < 10) {
			r = nic_itr_next_rate(r, 20000);
			n++;
		}
		CHECK_EQ(r, 20000);
		CHECK(n <= 4);
	}
	// every class always yields a valid class
	for (int t = 0; t < 10000; t++) {
		int c = (int)(rnd() % 3);
		int nc = nic_itr_classify(c, rnd() % 200, rnd() % 300000);

		CHECK(nc >= NIC_ITR_LOWEST && nc <= NIC_ITR_BULK);
	}
}

static void test_ltr(void)
{
	// 26 KB packet buffer at 1000 Mb/s: (26*512 - 1518) * 16 = 188704
	uint16_t enc = nic_ltr_encode(26, 1518, 1000, 0x1003);

	CHECK(enc != 0xFFFF);
	CHECK((enc & NIC_LTRV_VALUE_MASK) <= 1023);
	CHECK(nic_ltr_decode_ns(enc) >= 188704);
	CHECK(nic_ltr_decode_ns(enc) < 188704 + (1u << 10) * 2);
	// a platform that allows less caps it
	CHECK_EQ(nic_ltr_encode(26, 1518, 1000, 0x0801), 0x0801);
	// none reported: zero tolerance
	CHECK_EQ(nic_ltr_encode(26, 1518, 1000, 0), 0);
	// slower links tolerate more
	{
		uint16_t e100 = nic_ltr_encode(26, 1518, 100, 0x1FFF);
		uint16_t e10 = nic_ltr_encode(26, 1518, 10, 0x1FFF);

		CHECK(nic_ltr_decode_ns(e100) > nic_ltr_decode_ns(enc));
		CHECK(nic_ltr_decode_ns(e10) > nic_ltr_decode_ns(e100));
		CHECK(nic_ltr_decode_ns(e100) >= (uint64_t)11794 * 160);
	}
	CHECK_EQ(nic_ltr_encode(26, 1518, 0, 0x1003), 0xFFFF);
	CHECK_EQ(nic_ltr_encode(1, 1518, 1000, 0x1003), 0); // no room: 0
	for (int t = 0; t < 2000; t++) {
		uint32_t kb = 1 + rnd() % 64, spd = (rnd() & 1) ? 1000 : 100;
		uint64_t want = (uint64_t)kb * 512 > 1518 ?
					((uint64_t)kb * 512 - 1518) *
						(16000 / spd) :
					0;
		uint16_t e = nic_ltr_encode(kb, 1518, spd, 0x17FF);

		CHECK((e & NIC_LTRV_VALUE_MASK) <= 1023);
		CHECK(nic_ltr_decode_ns(e) >= want ||
		      e == 0x17FF); // rounded up or capped
	}
}

static void test_fc(void)
{
	const uint16_t S = NIC_ADV_PAUSE, A = NIC_ADV_ASM_DIR;

	CHECK_EQ(nic_fc_resolve(S | A, S | A, 1), NIC_FC_FULL);
	CHECK_EQ(nic_fc_resolve(S | A, S, 1), NIC_FC_FULL);
	CHECK_EQ(nic_fc_resolve(S | A, A, 1), NIC_FC_RX);
	CHECK_EQ(nic_fc_resolve(A, S | A, 1), NIC_FC_TX);
	CHECK_EQ(nic_fc_resolve(A, A, 1), NIC_FC_NONE);
	CHECK_EQ(nic_fc_resolve(S | A, 0, 1), NIC_FC_NONE);
	CHECK_EQ(nic_fc_resolve(0, S | A, 1), NIC_FC_NONE);
	CHECK_EQ(nic_fc_resolve(S | A, S | A, 0), NIC_FC_NONE); // half duplex
}

static void test_vmxnet3(void)
{
	const uint32_t ok = NIC_VMX_RCD_V4 | NIC_VMX_RCD_IPC | NIC_VMX_RCD_TUC;

	CHECK_EQ(nic_vmxnet3_rx_csum_ok(0, ok | NIC_VMX_RCD_TCP), 1);
	CHECK_EQ(nic_vmxnet3_rx_csum_ok(0, ok | NIC_VMX_RCD_UDP), 1);
	CHECK_EQ(nic_vmxnet3_rx_csum_ok(0, ok), 0); // neither TCP nor UDP
	CHECK_EQ(nic_vmxnet3_rx_csum_ok(NIC_VMX_RCD_CNC,
					ok | NIC_VMX_RCD_TCP),
		 0);
	CHECK_EQ(nic_vmxnet3_rx_csum_ok(0, ok | NIC_VMX_RCD_TCP |
						   NIC_VMX_RCD_FRG),
		 0);
	CHECK_EQ(nic_vmxnet3_rx_csum_ok(0, (ok & ~NIC_VMX_RCD_TUC) |
						   NIC_VMX_RCD_TCP),
		 0);
	CHECK_EQ(nic_vmxnet3_rx_csum_ok(0, (ok & ~NIC_VMX_RCD_V4) |
						   NIC_VMX_RCD_TCP),
		 0);
	// the generation bit (31) and type bits do not disturb it
	CHECK_EQ(nic_vmxnet3_rx_csum_ok(0, ok | NIC_VMX_RCD_TCP | 0x80000000u |
						   (3u << 24)),
		 1);
}

/* The rtl8139 ring walk against a byte-level model of the chip writing
 * frames one after another with WRAP: every header lands where the walk
 * expects it, and the read pointer the chip sees stays in step. */
static void test_rtl8139(void)
{
	const uint32_t ring = 32768;

	CHECK_EQ(nic_rtl8139_rx_next(0, 64, ring), 68);
	CHECK_EQ(nic_rtl8139_rx_next(0, 65, ring), 72);
	CHECK_EQ(nic_rtl8139_rx_next(32760, 64, ring), 60);
	CHECK_EQ(nic_rtl8139_capr(0), 0xFFF0);
	CHECK_EQ(nic_rtl8139_capr(100), 84);
	CHECK_EQ(nic_rtl8139_capr(32760), 32744);
	{
		uint32_t chip = 0, drv = 0;

		for (int i = 0; i < 100000; i++) {
			uint32_t len = 64 + rnd() % (1518 - 64 + 1) + 4;

			CHECK_EQ(drv, chip);
			CHECK((drv & 3) == 0);
			CHECK(drv < ring);
			// the chip's next write position (4-byte aligned)
			chip = ((chip + len + 4 + 3) & ~3u) % ring;
			drv = nic_rtl8139_rx_next(drv, len, ring);
			// CAPR + 16 is the read position, modulo the ring
			CHECK_EQ(((uint32_t)nic_rtl8139_capr(drv) + 16) % ring,
				 drv);
		}
	}
	CHECK_EQ(NIC_RTL_RX_EARLY_LEN, 0xFFF0);
}

static int lw_half_ok(uint32_t lw)
{
	return (lw & 0xFFFF) == (lw >> 16);
}

static void test_tulip(void)
{
	const uint8_t own[6] = { 0x00, 0x15, 0x5D, 0x01, 0x02, 0x03 };
	uint8_t mc[NETDEV_MC_TEST][6];
	uint32_t frame[48];

	for (int i = 0; i < NETDEV_MC_TEST; i++) {
		mc[i][0] = 0x01;
		mc[i][1] = 0x00;
		mc[i][2] = 0x5E;
		mc[i][3] = 0x00;
		mc[i][4] = (uint8_t)(i >> 8);
		mc[i][5] = (uint8_t)i;
	}
	// perfect filtering, two groups
	nic_tulip_setup_perfect(frame, own, (const uint8_t(*)[6])mc, 2);
	for (int i = 0; i < 48; i++)
		CHECK(lw_half_ok(frame[i]));
	CHECK_EQ(frame[0] & 0xFFFF, 0x0001); // 01:00
	CHECK_EQ(frame[1] & 0xFFFF, 0x005E); // 5E:00 (LE pair)
	CHECK_EQ(frame[2] & 0xFFFF, 0x0000);
	CHECK_EQ(frame[5] & 0xFFFF, 0x0100); // second group: 00:01
	for (int e = 2; e < 15; e++)
		for (int k = 0; k < 3; k++)
			CHECK_EQ(frame[3 * e + k], 0xFFFFFFFFu); // broadcast
	CHECK_EQ(frame[45] & 0xFFFF, 0x1500);
	CHECK_EQ(frame[46] & 0xFFFF, 0x015D);
	CHECK_EQ(frame[47] & 0xFFFF, 0x0302);
	// fourteen groups still fit; entry 14 stays broadcast
	nic_tulip_setup_perfect(frame, own, (const uint8_t(*)[6])mc,
				NIC_TULIP_PERFECT_MAX);
	CHECK_EQ(frame[42], 0xFFFFFFFFu);
	CHECK_EQ(frame[13 * 3 + 2] & 0xFFFF, 0x0D00);
	// hash filtering: every listed group's bit, broadcast (bit 255),
	// our own address in entry 13
	nic_tulip_setup_hash(frame, own, (const uint8_t(*)[6])mc,
			     NETDEV_MC_TEST);
	for (int i = 0; i < 48; i++)
		CHECK(lw_half_ok(frame[i]));
	CHECK(frame[15] & 0x8000u);
	for (int i = 0; i < NETDEV_MC_TEST; i++) {
		uint32_t idx = nic_crc32_le(mc[i], 6) & 0x1FF;

		CHECK(frame[idx >> 4] & (1u << (idx & 15)));
	}
	CHECK_EQ(frame[39] & 0xFFFF, 0x1500);
	CHECK_EQ(frame[41] & 0xFFFF, 0x0302);
	CHECK_EQ(frame[32], 0);
	CHECK_EQ(frame[38], 0);
}

static void test_pcnet_rtl_filters(void)
{
	// PCnet LADRF: word bit/16, bit bit%16 -- every bit reachable
	uint64_t seen_le = 0, seen_be = 0;

	for (int t = 0; t < 20000; t++) {
		uint8_t a[6] = { 0x01, 0x00, 0x5E, (uint8_t)(rnd() & 0x7F),
				 (uint8_t)rnd(), (uint8_t)rnd() };

		seen_le |= 1ull << nic_mc_hash64_le(a);
		seen_be |= 1ull << nic_mc_hash64_be(a);
	}
	CHECK_EQ(seen_le, ~0ull);
	CHECK_EQ(seen_be, ~0ull);
}

/* Shared-line interrupt ownership: enabled causes or the device's own
 * "asserted" flag claim it; causes latched while masked do not. */
static void test_irq_claim(void)
{
	const uint32_t RXT0 = 0x80, TXDW = 0x01, LSC = 0x04;
	const uint32_t napi = RXT0 | TXDW;

	// e1000/igb: enabled cause pending -> ours
	CHECK(nic_e1000_irq_ours(RXT0, napi | LSC));
	CHECK(nic_e1000_irq_ours(LSC, napi | LSC));
	// receive work latched while the poll runs (masked): not ours
	CHECK(!nic_e1000_irq_ours(RXT0, LSC));
	CHECK(!nic_e1000_irq_ours(RXT0 | TXDW, 0));
	// ... unless the MAC says it asserted the line (8257x and later)
	CHECK(nic_e1000_irq_ours(NIC_E1000_ICR_INT_ASSERTED | RXT0, LSC));
	// nothing pending, or the device is gone
	CHECK(!nic_e1000_irq_ours(0, napi | LSC));
	CHECK(!nic_e1000_irq_ours(0xFFFFFFFFu, 0xFFFFFFFFu));
	// rtl8139: ISR & IMR
	CHECK(nic_rtl8139_irq_ours(0x0001, 0x007F));
	CHECK(!nic_rtl8139_irq_ours(0x0001, 0x007E)); // ROK masked
	CHECK(nic_rtl8139_irq_ours(0x0021, 0x007E)); // + an enabled one
	CHECK(!nic_rtl8139_irq_ours(0, 0xFFFF));
	CHECK(!nic_rtl8139_irq_ours(0xFFFF, 0xFFFF));
	// exhaustive: ours exactly when an enabled bit is pending
	for (uint32_t isr = 0; isr < 0x100; isr++)
		for (uint32_t imr = 0; imr < 0x100; imr += 7)
			CHECK_EQ(nic_rtl8139_irq_ours((uint16_t)isr,
						      (uint16_t)imr),
				 (isr & imr) != 0);
}

int main(void)
{
	test_crc();
	test_mta();
	test_rx_verdict();
	test_rx_csum();
	test_rings();
	test_itr();
	test_ltr();
	test_fc();
	test_vmxnet3();
	test_rtl8139();
	test_tulip();
	test_pcnet_rtl_filters();
	test_irq_claim();
	printf("drivers: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
