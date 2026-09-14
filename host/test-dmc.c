// Host test: the DMC firmware package parser on synthetic packages.
#include <kernel/dev/gpu/i915/intel_dmc.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fails++;                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Build a v1 package with two images: a wildcard one (3 dwords) and a
 * 'G','0' one (5 dwords) with two register writes. */
static unsigned build(uint8_t *buf, unsigned cap, int bad_sig, int mmio_out, uint32_t big)
{
	memset(buf, 0, cap);
	unsigned pos = 0;
	/* CSS */
	wr32(buf + 0, 0x09);
	wr32(buf + 4, 32);
	wr32(buf + 22 * 4, (1u << 16) | 27);
	pos = 128;
	/* package header v1: 16 + 20*12 = 256 */
	uint8_t *pkg = buf + pos;
	pkg[0] = 64;
	pkg[1] = 1;
	wr32(pkg + 12, 2);
	uint8_t *e0 = pkg + 16;
	e0[2] = '*';
	e0[3] = '*';
	wr32(e0 + 4, 0); /* offset 0 dwords after the package */
	uint8_t *e1 = pkg + 16 + 12;
	e1[2] = 'G';
	e1[3] = '0';
	wr32(e1 + 4, (128 + 3 * 4) / 4); /* after image 0 */
	pos += 256;
	/* image 0: header v1 + 3 dwords */
	uint8_t *h = buf + pos;
	wr32(h, bad_sig ? 0x12345678 : 0x40403E3E);
	h[4] = 128; /* a version-1 header counts its length in bytes */
	h[5] = 1;
	wr32(h + 12, big ? big : 3);
	wr32(h + 20, 0); /* no mmio */
	pos += 128;
	wr32(buf + pos, 0xAAAAAAAA);
	wr32(buf + pos + 4, 0xBBBBBBBB);
	wr32(buf + pos + 8, 0xCCCCCCCC);
	pos += 12;
	/* image 1 */
	h = buf + pos;
	wr32(h, 0x40403E3E);
	h[4] = 128;
	h[5] = 1;
	wr32(h + 12, 5);
	wr32(h + 20, 2);
	wr32(h + 24, mmio_out ? 0x45504 : 0x8F074);
	wr32(h + 28, 0x8F004);
	wr32(h + 56, 0x11);
	wr32(h + 60, 0x22);
	pos += 128;
	for (int i = 0; i < 5; i++)
		wr32(buf + pos + i * 4, 0x1000 + i);
	pos += 20;
	wr32(buf + 6 * 4, pos / 4); /* css size in dwords */
	return pos;
}

int main(void)
{
	static uint8_t fw[4096];
	struct intel_dmc_image img;
	unsigned len = build(fw, sizeof(fw), 0, 0, 0);

	memset(&img, 0, sizeof(img));
	int rc = intel_dmc_parse(fw, len, 'G', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == 0, "exact stepping parses (%d)", rc);
	CHECK(img.stepping == 'G' && img.substepping == '0', "picked G0 (%c%c)", img.stepping, img.substepping);
	CHECK(img.version == ((1u << 16) | 27), "version 1.27 (%08x)", img.version);
	CHECK(img.payload_dwords == 5, "5 dwords (%u)", img.payload_dwords);
	CHECK(img.mmio_count == 2 && img.mmioaddr[0] == 0x8F074 && img.mmiodata[1] == 0x22, "register writes");
	CHECK(img.start_mmioaddr == 0x80000, "v1 start");
	CHECK(img.payload && img.payload[0] == 0x00 && img.payload[1] == 0x10, "payload points at the program");

	memset(&img, 0, sizeof(img));
	rc = intel_dmc_parse(fw, len, 'C', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == 0, "other stepping falls back to the wildcard (%d)", rc);
	CHECK(img.stepping == '*' && img.payload_dwords == 3, "wildcard image (%c, %u)", img.stepping, img.payload_dwords);
	CHECK(img.mmio_count == 0, "no register writes");

	rc = intel_dmc_parse(fw, len - 1, 'G', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == INTEL_DMC_EBAD_CSS, "size mismatch is a bad CSS (%d)", rc);
	rc = intel_dmc_parse(fw, len, 'G', '0', 4, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == INTEL_DMC_ETOO_BIG, "program over the limit (%d)", rc);

	len = build(fw, sizeof(fw), 1, 0, 0);
	rc = intel_dmc_parse(fw, len, 'C', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == INTEL_DMC_EBAD_HEADER, "bad signature rejected (%d)", rc);

	len = build(fw, sizeof(fw), 0, 1, 0);
	rc = intel_dmc_parse(fw, len, 'G', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == INTEL_DMC_EBAD_MMIO, "register write outside the window rejected (%d)", rc);

	len = build(fw, sizeof(fw), 0, 0, 100000);
	rc = intel_dmc_parse(fw, len, 'C', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == INTEL_DMC_ETOO_BIG, "program larger than the file rejected (%d)", rc);

	/* a package with no matching image */
	len = build(fw, sizeof(fw), 0, 0, 0);
	fw[128 + 16 + 2] = 'Z';
	fw[128 + 16 + 3] = '9';
	rc = intel_dmc_parse(fw, len, 'C', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == INTEL_DMC_ENO_IMAGE, "no image for the stepping (%d)", rc);

	/* An entry naming a stepping the package has no image for: the
	 * shipped packages start with a run of these, and taking one as
	 * the answer rejects a package that does hold an image. */
	len = build(fw, sizeof(fw), 0, 0, 0);
	wr32(fw + 128 + 16 + 4, 0xffffffff); /* the wildcard entry has none */
	rc = intel_dmc_parse(fw, len, 'G', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == 0 && img.stepping == 'G', "an entry with no image is passed over (%d, %c)",
	      rc, img.stepping);
	rc = intel_dmc_parse(fw, len, 'C', '0', 0x6000, 0x80000, 0x8FFFF, 0x80000, &img);
	CHECK(rc == INTEL_DMC_ENO_IMAGE,
	      "and leaves nothing for a stepping it does not name (%d)", rc);

	if (fails) {
		printf("dmc: %d failures\n", fails);
		return 1;
	}
	printf("dmc: all tests passed\n");
	return 0;
}
