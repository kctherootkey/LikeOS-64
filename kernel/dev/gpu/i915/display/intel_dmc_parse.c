// LikeOS-64 -- parsing the display microcontroller's firmware package.
//
// Pure: fixed-width types only, no kernel services, so the build host's
// compiler can run it against a synthetic package (host/test-dmc.sh).
// Every multi-byte field is read byte-wise: the file is little-endian
// and not necessarily aligned in memory.
#include <kernel/dev/gpu/i915/intel_dmc.h>

#define CSS_HEADER_SIZE 128
#define CSS_MODULE_TYPE_DMC 0x09
#define PACKAGE_HEADER_SIZE 16
#define FW_INFO_SIZE 12
#define DMC_SIGNATURE 0x40403E3E
#define DMC_HEADER_V1_SIZE 128
#define DMC_HEADER_V3_SIZE 256
#define DMC_OFFSET_INVALID 0xFFFFFFFFu

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* The CSS header's fields, as dword indices. */
#define CSS_MODULE_TYPE 0
#define CSS_HEADER_LEN 1
#define CSS_SIZE 6 /* whole file, dwords */
#define CSS_VERSION 22

/* The package header: header_len (u8, dwords), header_ver (u8),
 * reserved[10], num_entries (u32). */
/* An entry: v1 = reserved u16, stepping u8, substepping u8, offset u32
 * (dwords), reserved u32; v2 = reserved u8, dmc_id u8, stepping u8,
 * substepping u8, offset u32, reserved u32. */

/* The DMC header base: signature u32, header_len u8 (dwords),
 * header_ver u8, dmcc_ver u16, project u32, fw_size u32 (dwords),
 * fw_version u32 = 20 bytes.
 * v1 then: mmio_count u32, mmioaddr[8], mmiodata[8], dfile[32],
 *          reserved[2] (128 bytes in all).
 * v3 then: start_mmioaddr u32, reserved[9], dfile[32], mmio_count u32,
 *          mmioaddr[20], mmiodata[20] (256 bytes in all). */

int intel_dmc_parse(const uint8_t *fw, unsigned len, char stepping, char substepping,
		    uint32_t max_fw_dwords, uint32_t mmio_lo, uint32_t mmio_hi,
		    uint32_t v1_start, struct intel_dmc_image *out)
{
	unsigned pos = 0;

	if (!fw || !out || len < CSS_HEADER_SIZE)
		return INTEL_DMC_EBAD_CSS;
	const uint8_t *css = fw;
	if (rd32(css + CSS_MODULE_TYPE * 4) != CSS_MODULE_TYPE_DMC)
		return INTEL_DMC_EBAD_CSS;
	if (rd32(css + CSS_HEADER_LEN * 4) * 4 != CSS_HEADER_SIZE)
		return INTEL_DMC_EBAD_CSS;
	if (rd32(css + CSS_SIZE * 4) * 4 != len)
		return INTEL_DMC_EBAD_CSS;
	out->version = rd32(css + CSS_VERSION * 4);
	pos += CSS_HEADER_SIZE;

	/* the package header and its table of images */
	if (len < pos + PACKAGE_HEADER_SIZE)
		return INTEL_DMC_EBAD_PACKAGE;
	const uint8_t *pkg = fw + pos;
	unsigned pkg_len = pkg[0] * 4;
	unsigned pkg_ver = pkg[1];
	uint32_t num_entries = rd32(pkg + 12);
	unsigned max_entries;
	if (pkg_ver == 1)
		max_entries = 20;
	else if (pkg_ver == 2)
		max_entries = 32;
	else
		return INTEL_DMC_EBAD_PACKAGE;
	if (pkg_len != PACKAGE_HEADER_SIZE + max_entries * FW_INFO_SIZE)
		return INTEL_DMC_EBAD_PACKAGE;
	if (num_entries > max_entries)
		num_entries = max_entries;
	if (len < pos + pkg_len)
		return INTEL_DMC_EBAD_PACKAGE;
	const uint8_t *entries = pkg + PACKAGE_HEADER_SIZE;
	pos += pkg_len;

	/* the image for this stepping: exact, then any substepping, then
	 * the wildcard */
	int best = -1, best_rank = 0;
	for (unsigned i = 0; i < num_entries; i++) {
		const uint8_t *e = entries + i * FW_INFO_SIZE;
		char st, sub;
		if (pkg_ver == 1) {
			st = (char)e[2];
			sub = (char)e[3];
		} else {
			if (e[1] != 0) /* dmc_id: 0 is the main controller */
				continue;
			st = (char)e[2];
			sub = (char)e[3];
		}
		int rank;
		if (st == stepping && sub == substepping)
			rank = 3;
		else if (st == stepping && sub == INTEL_DMC_ANY_STEPPING)
			rank = 2;
		else if (st == INTEL_DMC_ANY_STEPPING && sub == INTEL_DMC_ANY_STEPPING)
			rank = 1;
		else
			continue;
		/* An entry can name a stepping the package has no image for. */
		if (rd32(e + 4) == DMC_OFFSET_INVALID)
			continue;
		if (rank > best_rank) {
			best_rank = rank;
			best = (int)i;
		}
	}
	if (best < 0)
		return INTEL_DMC_ENO_IMAGE;
	const uint8_t *e = entries + (unsigned)best * FW_INFO_SIZE;
	out->stepping = (char)e[2];
	out->substepping = (char)e[3];
	uint32_t off_dw = rd32(e + 4);
	if (off_dw > (len - pos) / 4)
		return INTEL_DMC_EBAD_HEADER;
	pos += off_dw * 4;

	/* the image's header */
	if (len < pos + 20)
		return INTEL_DMC_EBAD_HEADER;
	const uint8_t *h = fw + pos;
	if (rd32(h) != DMC_SIGNATURE)
		return INTEL_DMC_EBAD_HEADER;
	/* The header's length field is written in dwords by some versions
	 * and in bytes by others; both name the same fixed-size header, so
	 * take whichever reading matches it. */
	unsigned hver = h[5];
	unsigned expect = hver == 3 ? DMC_HEADER_V3_SIZE : DMC_HEADER_V1_SIZE;
	unsigned hlen = (h[4] == expect) ? expect :
			(h[4] * 4 == expect) ? expect : (unsigned)h[4] * 4;
	uint32_t fw_size = rd32(h + 12);
	out->header_ver = hver;
	if (hver == 1) {
		if (hlen != DMC_HEADER_V1_SIZE || len < pos + hlen)
			return INTEL_DMC_EBAD_HEADER;
		out->mmio_count = rd32(h + 20);
		if (out->mmio_count > 8)
			return INTEL_DMC_EBAD_HEADER;
		for (uint32_t i = 0; i < out->mmio_count; i++) {
			out->mmioaddr[i] = rd32(h + 24 + i * 4);
			out->mmiodata[i] = rd32(h + 56 + i * 4);
		}
		out->start_mmioaddr = v1_start;
	} else if (hver == 3) {
		if (hlen != DMC_HEADER_V3_SIZE || len < pos + hlen)
			return INTEL_DMC_EBAD_HEADER;
		out->start_mmioaddr = rd32(h + 20);
		out->mmio_count = rd32(h + 92);
		if (out->mmio_count > INTEL_DMC_MAX_MMIO)
			return INTEL_DMC_EBAD_HEADER;
		for (uint32_t i = 0; i < out->mmio_count; i++) {
			out->mmioaddr[i] = rd32(h + 96 + i * 4);
			out->mmiodata[i] = rd32(h + 176 + i * 4);
		}
		if (out->start_mmioaddr < mmio_lo || out->start_mmioaddr > mmio_hi)
			return INTEL_DMC_EBAD_MMIO;
	} else {
		return INTEL_DMC_EBAD_HEADER;
	}
	for (uint32_t i = 0; i < out->mmio_count; i++)
		if (out->mmioaddr[i] < mmio_lo || out->mmioaddr[i] > mmio_hi)
			return INTEL_DMC_EBAD_MMIO;
	pos += hlen;

	/* the program */
	if (fw_size == 0 || fw_size > max_fw_dwords)
		return INTEL_DMC_ETOO_BIG;
	if (fw_size > (len - pos) / 4)
		return INTEL_DMC_EBAD_HEADER;
	out->payload = fw + pos;
	out->payload_dwords = fw_size;
	return 0;
}
