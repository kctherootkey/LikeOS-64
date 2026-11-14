/* Host test for boot/efi_loadopt.c: device paths, EFI_LOAD_OPTION,
 * UCS-2/UTF-8, GUID text and BOOTX64.CSV, against hand-written golden
 * bytes, plus a fuzz pass over every parser (run under ASan/UBSan by
 * host/test-efi-loadopt.sh).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../boot/efi_loadopt.h"

static int g_fail, g_pass;

#define CHECK(cond)                                                         \
	do {                                                                \
		if (cond) {                                                 \
			g_pass++;                                           \
		} else {                                                    \
			g_fail++;                                           \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,       \
				__LINE__, #cond);                           \
		}                                                           \
	} while (0)

static void dump(const char *what, const unsigned char *b, size_t n)
{
	fprintf(stderr, "%s (%zu):", what, n);
	for (size_t i = 0; i < n; i++)
		fprintf(stderr, "%s%02x", i % 16 ? " " : "\n  ", b[i]);
	fputc('\n', stderr);
}

#define CHECK_BYTES(got, n, want, wn)                                       \
	do {                                                                \
		if ((size_t)(n) == (size_t)(wn) &&                          \
		    memcmp((got), (want), (wn)) == 0) {                     \
			g_pass++;                                           \
		} else {                                                    \
			g_fail++;                                           \
			fprintf(stderr, "FAIL %s:%d: bytes differ\n",       \
				__FILE__, __LINE__);                        \
			dump("got", (got), (n));                            \
			dump("want", (want), (wn));                         \
		}                                                           \
	} while (0)

#define CHECK_STR(got, want)                                                \
	do {                                                                \
		if (strcmp((got), (want)) == 0) {                           \
			g_pass++;                                           \
		} else {                                                    \
			g_fail++;                                           \
			fprintf(stderr, "FAIL %s:%d:\n  got  \"%s\"\n"      \
				"  want \"%s\"\n", __FILE__, __LINE__,       \
				(got), (want));                             \
		}                                                           \
	} while (0)

/* The partition of the examples: GPT partition 1, start 0x800, size
 * 0x100000, unique GUID 0b1f2e3d-4c5b-6a79-8897-a6b5c4d3e2f1. */
static const char *k_guid_text = "0b1f2e3d-4c5b-6a79-8897-a6b5c4d3e2f1";
static const unsigned char k_guid_mem[16] = {
	0x3d, 0x2e, 0x1f, 0x0b, 0x5b, 0x4c, 0x79, 0x6a,
	0x88, 0x97, 0xa6, 0xb5, 0xc4, 0xd3, 0xe2, 0xf1
};

static struct elo_hd example_hd(void)
{
	struct elo_hd hd;

	memset(&hd, 0, sizeof(hd));
	hd.part_number = 1;
	hd.part_start = 0x800;
	hd.part_size = 0x100000;
	memcpy(hd.signature, k_guid_mem, 16);
	hd.mbr_type = ELO_HD_MBR_TYPE_GPT;
	hd.sig_type = ELO_HD_SIG_GUID;
	return hd;
}

/* HD(1,GPT,...,0x800,0x100000) node, 42 bytes. */
static const unsigned char k_hd_node[42] = {
	0x04, 0x01, 0x2a, 0x00,                         /* type, sub, len */
	0x01, 0x00, 0x00, 0x00,                         /* part number */
	0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* start */
	0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, /* size */
	0x3d, 0x2e, 0x1f, 0x0b, 0x5b, 0x4c, 0x79, 0x6a, /* signature */
	0x88, 0x97, 0xa6, 0xb5, 0xc4, 0xd3, 0xe2, 0xf1,
	0x02, 0x02                                      /* MBRType, SigType */
};

/* "\EFI\LIKEOS\BOOTX64.EFI" = 23 characters + NUL = 48 bytes, plus the
 * 4-byte node header. */
static size_t file_node(unsigned char *out, const char *path)
{
	size_t n = strlen(path), len = 4 + 2 * (n + 1);

	out[0] = 0x04;
	out[1] = 0x04;
	out[2] = (unsigned char)len;
	out[3] = (unsigned char)(len >> 8);
	for (size_t i = 0; i <= n; i++) {
		out[4 + 2 * i] = (unsigned char)path[i];
		out[5 + 2 * i] = 0;
	}
	return len;
}

static const unsigned char k_end[4] = { 0x7f, 0xff, 0x04, 0x00 };

static size_t golden_path(unsigned char *out, const char *path)
{
	size_t n = 0;

	memcpy(out, k_hd_node, 42);
	n = 42;
	n += file_node(out + n, path);
	memcpy(out + n, k_end, 4);
	return n + 4;
}

static void test_guid(void)
{
	unsigned char g[16];
	char t[37];
	static const unsigned char global[16] = {
		0x61, 0xdf, 0xe4, 0x8b, 0xca, 0x93, 0xd2, 0x11,
		0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c
	};

	CHECK(elo_guid_from_text("8be4df61-93ca-11d2-aa0d-00e098032b8c", g) == 0);
	CHECK_BYTES(g, 16, global, 16);
	elo_guid_to_text(global, t);
	CHECK_STR(t, "8be4df61-93ca-11d2-aa0d-00e098032b8c");
	CHECK(elo_guid_from_text("{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}", g) == 0);
	CHECK_BYTES(g, 16, global, 16);
	CHECK(elo_guid_from_text(k_guid_text, g) == 0);
	CHECK_BYTES(g, 16, k_guid_mem, 16);
	elo_guid_to_text(k_guid_mem, t);
	CHECK_STR(t, k_guid_text);
	CHECK(elo_guid_from_text("8be4df61-93ca-11d2-aa0d-00e098032b8", g) < 0);
	CHECK(elo_guid_from_text("8be4df61-93ca-11d2-aa0d-00e098032b8cc", g) < 0);
	CHECK(elo_guid_from_text("8be4df61x93ca-11d2-aa0d-00e098032b8c", g) < 0);
	CHECK(elo_guid_from_text("{8be4df61-93ca-11d2-aa0d-00e098032b8c", g) < 0);
	CHECK(elo_guid_from_text("8be4df61-93ca-11d2-aa0d-00e098032g8c", g) < 0);
	CHECK(elo_guid_from_text("", g) < 0);
}

static void test_hd_file(void)
{
	struct elo_hd hd = example_hd(), got;
	unsigned char buf[512], want[512];
	size_t n, wn, off = 99;

	wn = golden_path(want, "\\EFI\\LIKEOS\\BOOTX64.EFI");
	CHECK(wn == 42 + 52 + 4);
	n = elo_dp_hd_file(&hd, "\\EFI\\LIKEOS\\BOOTX64.EFI", buf, sizeof(buf));
	CHECK_BYTES(buf, n, want, wn);
	/* forward slashes, no leading separator: same bytes */
	n = elo_dp_hd_file(&hd, "EFI/LIKEOS/BOOTX64.EFI", buf, sizeof(buf));
	CHECK_BYTES(buf, n, want, wn);
	n = elo_dp_hd_file(&hd, "/EFI/LIKEOS/BOOTX64.EFI", buf, sizeof(buf));
	CHECK_BYTES(buf, n, want, wn);
	/* capacity: exact fits, one byte less does not */
	CHECK(elo_dp_hd_file(&hd, "\\EFI\\LIKEOS\\BOOTX64.EFI", buf, wn) == wn);
	CHECK(elo_dp_hd_file(&hd, "\\EFI\\LIKEOS\\BOOTX64.EFI", buf, wn - 1) == 0);
	CHECK(elo_dp_hd_file(&hd, "", buf, sizeof(buf)) == 0);
	CHECK(elo_dp_hd_file(&hd, "\\EFI\\\xc3", buf, sizeof(buf)) == 0);
	CHECK(elo_dp_hd_file(&hd, "\\\xf0\x9f\x98\x80", buf, sizeof(buf)) == 0);

	CHECK(elo_dp_length(want, wn) == wn);
	CHECK(elo_dp_length(want, wn + 10) == wn);
	CHECK(elo_dp_length(want, wn - 1) == 0);
	CHECK(elo_dp_valid(want, wn));
	CHECK(!elo_dp_valid(want, wn - 4));
	CHECK(!elo_dp_valid(want, wn + 4));
	CHECK(elo_dp_find_hd(want, wn, &got, &off) == 1);
	CHECK(off == 0);
	CHECK(got.part_number == 1 && got.part_start == 0x800 &&
	      got.part_size == 0x100000 && got.sig_type == 2 &&
	      got.mbr_type == 2 && !memcmp(got.signature, k_guid_mem, 16));
}

/* PciRoot(0x0)/Pci(0x1f,0x2)/Sata(0x0,0xffff,0x0) in front of a path. */
static size_t long_form(unsigned char *out, const unsigned char *tail,
			size_t tlen)
{
	static const unsigned char prefix[] = {
		0x02, 0x01, 0x0c, 0x00, 0xd0, 0x41, 0x03, 0x0a,
		0x00, 0x00, 0x00, 0x00,                       /* PciRoot(0) */
		0x01, 0x01, 0x06, 0x00, 0x02, 0x1f,           /* Pci(1f,2) */
		0x03, 0x12, 0x0a, 0x00, 0x00, 0x00, 0xff, 0xff,
		0x00, 0x00                                    /* Sata */
	};

	memcpy(out, prefix, sizeof(prefix));
	memcpy(out + sizeof(prefix), tail, tlen);
	return sizeof(prefix) + tlen;
}

static void test_matches(void)
{
	struct elo_hd hd = example_hd(), other;
	unsigned char p[512], l[600], split[600];
	size_t n = golden_path(p, "\\EFI\\LIKEOS\\BOOTX64.EFI"), ln, sn;
	char text[512];

	CHECK(elo_dp_matches(p, n, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI"));
	CHECK(elo_dp_matches(p, n, &hd, "/efi/likeos/bootx64.efi"));
	CHECK(elo_dp_matches(p, n, &hd, "EFI\\LikeOS\\BootX64.efi"));
	ln = long_form(l, p, n);
	CHECK(elo_dp_valid(l, ln));
	CHECK(elo_dp_matches(l, ln, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI"));

	/* the path split over two File nodes */
	memcpy(split, k_hd_node, 42);
	sn = 42;
	sn += file_node(split + sn, "\\EFI\\LIKEOS");
	sn += file_node(split + sn, "\\BOOTX64.EFI");
	memcpy(split + sn, k_end, 4);
	sn += 4;
	CHECK(elo_dp_matches(split, sn, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI"));

	/* split without a separator at the join: File(\EFI\LIKEOS) followed
	 * by File(BOOTX64.EFI) is the same file */
	memcpy(split, k_hd_node, 42);
	sn = 42;
	sn += file_node(split + sn, "\\EFI\\LIKEOS");
	sn += file_node(split + sn, "BOOTX64.EFI");
	memcpy(split + sn, k_end, 4);
	sn += 4;
	CHECK(elo_dp_matches(split, sn, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI"));
	CHECK(elo_dp_matches(split, sn, &hd, "/efi/likeos/bootx64.efi"));
	CHECK(!elo_dp_matches(split, sn, &hd, "\\EFI\\LIKEOSBOOTX64.EFI"));
	CHECK(!elo_dp_matches(split, sn, &hd, "\\EFI\\LIKEOS\\BOOTX64.EF"));

	/* separators on both sides of the join, three nodes, no leading
	 * separator in the first: still one separator each */
	memcpy(split, k_hd_node, 42);
	sn = 42;
	sn += file_node(split + sn, "EFI\\");
	sn += file_node(split + sn, "\\LikeOS\\");
	sn += file_node(split + sn, "bootx64.efi");
	memcpy(split + sn, k_end, 4);
	sn += 4;
	CHECK(elo_dp_matches(split, sn, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI"));
	CHECK(elo_dp_matches(split, sn, &hd, "EFI/LikeOS/BootX64.efi"));
	CHECK(elo_dp_matches(split, sn, &hd, "\\EFI\\\\LIKEOS//BOOTX64.EFI"));
	CHECK(!elo_dp_matches(split, sn, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI\\X"));
	CHECK(!elo_dp_matches(split, sn, &hd, "\\EFI\\LIKEOS"));

	CHECK(!elo_dp_matches(p, n, &hd, "\\EFI\\LIKEOS\\BOOTX64.EF"));
	CHECK(!elo_dp_matches(p, n, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFIX"));
	CHECK(!elo_dp_matches(p, n, &hd, "\\EFI\\BOOT\\BOOTX64.EFI"));
	CHECK(!elo_dp_matches(p, n, &hd, ""));
	other = hd;
	other.signature[15] ^= 1;
	CHECK(!elo_dp_matches(p, n, &other, "\\EFI\\LIKEOS\\BOOTX64.EFI"));
	other = hd;
	other.part_number = 2;
	CHECK(!elo_dp_matches(p, n, &other, "\\EFI\\LIKEOS\\BOOTX64.EFI"));
	other = hd;
	other.sig_type = ELO_HD_SIG_MBR;
	CHECK(!elo_dp_matches(p, n, &other, "\\EFI\\LIKEOS\\BOOTX64.EFI"));
	/* the start and size do not take part */
	other = hd;
	other.part_start = 1;
	other.part_size = 2;
	CHECK(elo_dp_matches(p, n, &other, "\\EFI\\LIKEOS\\BOOTX64.EFI"));

	/* MBR partition: signature 0x12345678, partition 2 */
	{
		struct elo_hd m;
		unsigned char mp[256];
		size_t mn;

		memset(&m, 0, sizeof(m));
		m.part_number = 2;
		m.part_start = 2048;
		m.part_size = 4096;
		m.signature[0] = 0x78;
		m.signature[1] = 0x56;
		m.signature[2] = 0x34;
		m.signature[3] = 0x12;
		m.mbr_type = ELO_HD_MBR_TYPE_MBR;
		m.sig_type = ELO_HD_SIG_MBR;
		mn = elo_dp_hd_file(&m, "\\EFI\\BOOT\\BOOTX64.EFI", mp, sizeof(mp));
		CHECK(mn > 0);
		CHECK(elo_dp_matches(mp, mn, &m, "/EFI/BOOT/BOOTX64.EFI"));
		m.signature[0] ^= 1;
		CHECK(!elo_dp_matches(mp, mn, &m, "/EFI/BOOT/BOOTX64.EFI"));
		elo_dp_to_text(mp, mn, text, sizeof(text));
		CHECK_STR(text, "HD(2,MBR,0x12345678,0x800,0x1000)/File(\\EFI\\BOOT\\BOOTX64.EFI)");
	}

	/* text */
	elo_dp_to_text(p, n, text, sizeof(text));
	CHECK_STR(text, "HD(1,GPT,0b1f2e3d-4c5b-6a79-8897-a6b5c4d3e2f1,0x800,0x100000)/File(\\EFI\\LIKEOS\\BOOTX64.EFI)");
	elo_dp_to_text(l, ln, text, sizeof(text));
	CHECK_STR(text, "PciRoot(0x0)/Pci(0x1f,0x2)/Sata(0x0,0xffff,0x0)/HD(1,GPT,0b1f2e3d-4c5b-6a79-8897-a6b5c4d3e2f1,0x800,0x100000)/File(\\EFI\\LIKEOS\\BOOTX64.EFI)");
	{
		/* truncation: snprintf-like */
		char small[10];
		size_t want = elo_dp_to_text(p, n, small, sizeof(small));

		CHECK(want == strlen("HD(1,GPT,0b1f2e3d-4c5b-6a79-8897-a6b5c4d3e2f1,0x800,0x100000)/File(\\EFI\\LIKEOS\\BOOTX64.EFI)"));
		CHECK_STR(small, "HD(1,GPT,");
	}
}

static void test_text_nodes(void)
{
	char text[512];
	/* USB(0x3,0x0)/NVMe(0x1,00-11-22-33-44-55-66-77)/PcieRoot(0x1)/
	 * VenHw(guid,0102)/unknown node/End Instance/MAC/End */
	static const unsigned char dp[] = {
		0x03, 0x05, 0x06, 0x00, 0x03, 0x00,
		0x03, 0x17, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x02, 0x01, 0x0c, 0x00, 0xd0, 0x41, 0x08, 0x0a, 0x01, 0, 0, 0,
		0x01, 0x04, 0x16, 0x00,
		0x61, 0xdf, 0xe4, 0x8b, 0xca, 0x93, 0xd2, 0x11,
		0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c, 0x01, 0x02,
		0x09, 0x33, 0x05, 0x00, 0xab,
		0x7f, 0x01, 0x04, 0x00,
		0x02, 0x01, 0x0c, 0x00, 0xd0, 0x41, 0x01, 0x05, 0x02, 0, 0, 0,
		0x7f, 0xff, 0x04, 0x00
	};

	CHECK(elo_dp_valid(dp, sizeof(dp)));
	elo_dp_to_text(dp, sizeof(dp), text, sizeof(text));
	CHECK_STR(text, "USB(0x3,0x0)/NVMe(0x1,00-11-22-33-44-55-66-77)/PcieRoot(0x1)/VenHw(8be4df61-93ca-11d2-aa0d-00e098032b8c,0102)/Path(9,51,ab),Acpi(PNP0501,0x2)");
	/* malformed: a node longer than the buffer */
	{
		static const unsigned char bad[] = { 0x04, 0x04, 0x40, 0x00, 0x41, 0x00 };

		elo_dp_to_text(bad, sizeof(bad), text, sizeof(text));
		CHECK_STR(text, "<malformed>");
		CHECK(elo_dp_length(bad, sizeof(bad)) == 0);
	}
}

static void test_load_option(void)
{
	struct elo_hd hd = example_hd();
	unsigned char dp[512], lo[1024], want[1024];
	size_t dn = golden_path(dp, "\\EFI\\LIKEOS\\BOOTX64.EFI"), n, wn = 0;
	struct elo_load_option o;
	char desc[64];
	static const char d[] = "LikeOS";

	/* golden EFI_LOAD_OPTION */
	want[wn++] = 0x01;
	want[wn++] = 0x00;
	want[wn++] = 0x00;
	want[wn++] = 0x00;
	want[wn++] = (unsigned char)dn;
	want[wn++] = (unsigned char)(dn >> 8);
	for (size_t i = 0; i <= strlen(d); i++) {
		want[wn++] = (unsigned char)d[i];
		want[wn++] = 0;
	}
	memcpy(want + wn, dp, dn);
	wn += dn;
	CHECK(wn == 6 + 14 + 98);

	n = elo_load_option_build(ELO_LOAD_OPTION_ACTIVE, "LikeOS", dp, dn, NULL, 0,
				  lo, sizeof(lo));
	CHECK_BYTES(lo, n, want, wn);
	CHECK(elo_load_option_build(1, "LikeOS", dp, dn, NULL, 0, lo, wn) == wn);
	CHECK(elo_load_option_build(1, "LikeOS", dp, dn, NULL, 0, lo, wn - 1) == 0);
	CHECK(elo_load_option_build(1, "LikeOS", dp, dn - 1, NULL, 0, lo, sizeof(lo)) == 0);
	CHECK(elo_load_option_build(1, "LikeOS", dp, 0, NULL, 0, lo, sizeof(lo)) == 0);
	CHECK(elo_load_option_build(1, "Like\xffOS", dp, dn, NULL, 0, lo, sizeof(lo)) == 0);

	CHECK(elo_load_option_parse(want, wn, &o) == 0);
	CHECK(o.attributes == 1 && o.dp_len == dn && o.desc_units == 6 &&
	      o.opt == NULL && o.opt_len == 0 && o.dp == want + 20);
	elo_ucs2_to_utf8(o.desc, o.desc_units, desc, sizeof(desc));
	CHECK_STR(desc, "LikeOS");
	CHECK(elo_dp_matches(o.dp, o.dp_len, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI"));

	/* with optional data */
	{
		unsigned char opt[16];
		size_t on = elo_utf8_to_ucs2("quiet", opt, sizeof(opt));

		CHECK(on == 12);
		n = elo_load_option_build(1 | ELO_LOAD_OPTION_HIDDEN, "Ünïcødé",
					  dp, dn, opt, on, lo, sizeof(lo));
		CHECK(n > 0);
		CHECK(elo_load_option_parse(lo, n, &o) == 0);
		CHECK(o.attributes == 9 && o.opt_len == 12 && !memcmp(o.opt, opt, 12));
		elo_ucs2_to_utf8(o.desc, o.desc_units, desc, sizeof(desc));
		CHECK_STR(desc, "Ünïcødé");
	}

	/* malformed */
	CHECK(elo_load_option_parse(want, 5, &o) < 0);
	CHECK(elo_load_option_parse(want, 12, &o) < 0);   /* description cut */
	CHECK(elo_load_option_parse(want, wn - 1, &o) < 0); /* path cut */
	{
		unsigned char bad[1024];

		memcpy(bad, want, wn);
		bad[4] = 0;
		bad[5] = 0; /* FilePathListLength 0 */
		CHECK(elo_load_option_parse(bad, wn, &o) < 0);
		memcpy(bad, want, wn);
		bad[4] = 0xff; /* past the end */
		CHECK(elo_load_option_parse(bad, wn, &o) < 0);
		memcpy(bad, want, wn);
		bad[20 + 2] = 0;
		bad[20 + 3] = 0; /* zero-length node */
		CHECK(elo_load_option_parse(bad, wn, &o) < 0);
		memcpy(bad, want, wn);
		bad[wn - 3] = 0x01; /* End Instance instead of End Entire */
		CHECK(elo_load_option_parse(bad, wn, &o) < 0);
		/* description never terminated */
		memset(bad, 'A', 64);
		CHECK(elo_load_option_parse(bad, 64, &o) < 0);
	}
}

static void test_text_conv(void)
{
	unsigned char u[64];
	char s[64];
	size_t n;

	n = elo_utf8_to_ucs2("A\xc3\xa9\xe2\x86\x91", u, sizeof(u)); /* Aé↑ */
	CHECK(n == 8);
	CHECK(u[0] == 'A' && u[1] == 0 && u[2] == 0xe9 && u[3] == 0 &&
	      u[4] == 0x91 && u[5] == 0x21 && u[6] == 0 && u[7] == 0);
	n = elo_ucs2_to_utf8(u, 4, s, sizeof(s));
	CHECK_STR(s, "A\xc3\xa9\xe2\x86\x91");
	CHECK(n == 6);
	/* invalid UTF-8 */
	CHECK(elo_utf8_to_ucs2("\x80", u, sizeof(u)) == 0);
	CHECK(elo_utf8_to_ucs2("\xc0\xaf", u, sizeof(u)) == 0);      /* overlong */
	CHECK(elo_utf8_to_ucs2("\xed\xa0\x80", u, sizeof(u)) == 0);  /* surrogate */
	CHECK(elo_utf8_to_ucs2("\xf0\x9f\x98\x80", u, sizeof(u)) == 0); /* > U+FFFF */
	CHECK(elo_utf8_to_ucs2("abc", u, 7) == 0);
	CHECK(elo_utf8_to_ucs2("abc", u, 8) == 8);
	CHECK(elo_utf8_to_ucs2("", u, 2) == 2);
	/* surrogate pair and lone surrogates */
	{
		static const unsigned char pair[] = { 0x3d, 0xd8, 0x00, 0xde, 0x41, 0x00,
						      0x00, 0xdc, 0x00, 0xd8 };

		elo_ucs2_to_utf8(pair, 5, s, sizeof(s));
		CHECK_STR(s, "\xf0\x9f\x98\x80" "A\xef\xbf\xbd\xef\xbf\xbd");
	}
	/* cut at a character boundary */
	CHECK(elo_utf8_to_ucs2("A\xc3\xa9\xe2\x86\x91", u, sizeof(u)) == 8);
	n = elo_ucs2_to_utf8(u, 4, s, 4); /* "Aé" fits (3 bytes + NUL) */
	CHECK(n == 3);
	CHECK_STR(s, "A\xc3\xa9");
	n = elo_ucs2_to_utf8(u, 4, s, 3);
	CHECK(n == 1);
	CHECK_STR(s, "A");
}

static size_t to_utf16(unsigned char *out, const char *s, int bom)
{
	size_t n = 0;

	if (bom) {
		out[n++] = 0xff;
		out[n++] = 0xfe;
	}
	for (; *s; s++) {
		out[n++] = (unsigned char)*s;
		out[n++] = 0;
	}
	return n;
}

static void test_csv(void)
{
	unsigned char b[1024];
	char f[64], l[64], o[64];
	size_t n;
	const char *il = "BOOTX64.EFI,LikeOS,,This is the boot entry for LikeOS\r\n";

	/* exactly what the installer writes */
	n = to_utf16(b, il, 1);
	CHECK(n == 2 + 2 * strlen(il));
	CHECK(elo_csv_parse(b, n, f, sizeof(f), l, sizeof(l), o, sizeof(o)) == 0);
	CHECK_STR(f, "BOOTX64.EFI");
	CHECK_STR(l, "LikeOS");
	CHECK_STR(o, "");
	/* no BOM */
	n = to_utf16(b, il, 0);
	CHECK(elo_csv_parse(b, n, f, sizeof(f), l, sizeof(l), o, sizeof(o)) == 0);
	CHECK_STR(f, "BOOTX64.EFI");
	CHECK_STR(l, "LikeOS");
	/* ASCII, comment and empty lines, options, blanks */
	{
		const char *a = "# boot entry\n\n   \r\n altx64.efi , My OS ,quiet splash ,c\n";

		CHECK(elo_csv_parse((const unsigned char *)a, strlen(a), f, sizeof(f),
				    l, sizeof(l), o, sizeof(o)) == 0);
		CHECK_STR(f, "altx64.efi");
		CHECK_STR(l, "My OS");
		CHECK_STR(o, "quiet splash");
	}
	/* UTF-8 with BOM and non-ASCII label */
	{
		const char *a = "\xef\xbb\xbf" "loader.efi,Système\n";

		CHECK(elo_csv_parse((const unsigned char *)a, strlen(a), f, sizeof(f),
				    l, sizeof(l), o, sizeof(o)) == 0);
		CHECK_STR(f, "loader.efi");
		CHECK_STR(l, "Système");
		CHECK_STR(o, "");
	}
	/* only comments / empty / no file name */
	{
		const char *a = "# nothing\n\n";
		const char *c = ",label\n";

		CHECK(elo_csv_parse((const unsigned char *)a, strlen(a), f, sizeof(f),
				    l, sizeof(l), o, sizeof(o)) < 0);
		CHECK(elo_csv_parse((const unsigned char *)c, strlen(c), f, sizeof(f),
				    l, sizeof(l), o, sizeof(o)) < 0);
		CHECK(elo_csv_parse((const unsigned char *)"", 0, f, sizeof(f),
				    l, sizeof(l), o, sizeof(o)) < 0);
	}
	/* truncation, NULL label/options */
	{
		char tiny[6];

		n = to_utf16(b, il, 1);
		CHECK(elo_csv_parse(b, n, tiny, sizeof(tiny), NULL, 0, NULL, 0) == 0);
		CHECK_STR(tiny, "BOOTX");
	}
	/* odd length UTF-16 tail */
	n = to_utf16(b, "x.efi,L", 1);
	b[n++] = 'Z';
	CHECK(elo_csv_parse(b, n, f, sizeof(f), l, sizeof(l), o, sizeof(o)) == 0);
	CHECK_STR(f, "x.efi");
	CHECK_STR(l, "L");
}

/* ---- fuzzing ------------------------------------------------------------ */

static unsigned long long g_rng = 0x9e3779b97f4a7c15ULL;

static unsigned rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return (unsigned)(g_rng >> 11);
}

static void fuzz_one(const unsigned char *src, size_t n)
{
	/* An exact-size heap copy, so ASan sees every read past the end. */
	unsigned char *b = malloc(n ? n : 1);
	struct elo_load_option o;
	struct elo_hd hd = example_hd(), got;
	char text[300], f[40], l[40], op[40];
	size_t off;

	if (!b)
		abort();
	memcpy(b, src, n);
	(void)elo_dp_length(b, n);
	(void)elo_dp_valid(b, n);
	(void)elo_dp_find_hd(b, n, &got, &off);
	(void)elo_dp_matches(b, n, &hd, "\\EFI\\LIKEOS\\BOOTX64.EFI");
	(void)elo_dp_to_text(b, n, text, sizeof(text));
	if (strlen(text) >= sizeof(text))
		abort();
	if (elo_load_option_parse(b, n, &o) == 0) {
		char d[200];

		if (o.dp + o.dp_len + o.opt_len != b + n)
			abort();
		elo_ucs2_to_utf8(o.desc, o.desc_units, d, sizeof(d));
		(void)elo_dp_to_text(o.dp, o.dp_len, text, sizeof(text));
	}
	(void)elo_csv_parse(b, n, f, sizeof(f), l, sizeof(l), op, sizeof(op));
	(void)elo_ucs2_to_utf8(b, n / 2, text, sizeof(text));
	free(b);
	/* UTF-8 input must be NUL-terminated */
	{
		char *s = malloc(n + 1);
		unsigned char u[600];

		if (!s)
			abort();
		memcpy(s, src, n);
		s[n] = 0;
		(void)elo_utf8_to_ucs2(s, u, sizeof(u));
		(void)elo_dp_hd_file(&hd, s, u, sizeof(u));
		(void)elo_guid_from_text(s, got.signature);
		free(s);
	}
}

static void fuzz(int rounds)
{
	struct elo_hd hd = example_hd();
	unsigned char seed[3][1024], buf[1100];
	size_t seedn[3];

	seedn[0] = golden_path(seed[0], "\\EFI\\LIKEOS\\BOOTX64.EFI");
	seedn[1] = elo_load_option_build(1, "LikeOS", seed[0], seedn[0], NULL, 0,
					 seed[1], sizeof(seed[1]));
	seedn[2] = to_utf16(seed[2], "BOOTX64.EFI,LikeOS,,c\r\n", 1);
	(void)hd;
	for (int r = 0; r < rounds; r++) {
		size_t n;

		if (r % 4 == 0) {
			n = rnd() % 200;
			for (size_t i = 0; i < n; i++)
				buf[i] = (unsigned char)rnd();
		} else {
			int s = rnd() % 3;
			int edits = 1 + rnd() % 6;

			n = seedn[s];
			memcpy(buf, seed[s], n);
			for (int e = 0; e < edits; e++) {
				switch (rnd() % 4) {
				case 0:
					if (n)
						buf[rnd() % n] = (unsigned char)rnd();
					break;
				case 1:
					if (n)
						buf[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8));
					break;
				case 2:
					if (n)
						n = rnd() % n;
					break;
				default:
					if (n + 4 < sizeof(buf)) {
						size_t at = n ? rnd() % n : 0;

						memmove(buf + at + 2, buf + at, n - at);
						buf[at] = (unsigned char)rnd();
						buf[at + 1] = (unsigned char)rnd();
						n += 2;
					}
					break;
				}
			}
		}
		fuzz_one(buf, n);
	}
	g_pass++;
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 300000;

	test_guid();
	test_hd_file();
	test_matches();
	test_text_nodes();
	test_load_option();
	test_text_conv();
	test_csv();
	fuzz(rounds);
	printf("efi_loadopt: %d checks passed, %d failed (fuzz rounds %d)\n",
	       g_pass, g_fail, rounds);
	return g_fail ? 1 : 0;
}
