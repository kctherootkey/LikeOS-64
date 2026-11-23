// LikeOS - ext4 filesystem driver: directory index hashes (legacy, half-MD4, TEA)
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * htree (hash-indexed directory) write support.
 *
 * On-disk an htree dir keeps its real entries in plain leaf blocks (read by the
 * linear scanner in ext4_dir_lookup_disk) plus an index: block 0 is a dx_root
 * (dot/dotdot + dx_root_info + a sorted {hash,logical-block} array), and for
 * deep trees, interior dx_node blocks.  We hash the name, descend the index to
 * the target leaf, insert there, and on a full leaf split it and propagate a new
 * index entry upward (growing the tree from depth 0 to 1 when the root fills).
 *
 * The dirhash (verified byte-exact against e2fsprogs) and the dx checksum are
 * the e2fsck gates; both are below.  A single index node holds up to ~510 leaf
 * pointers, so depth 1 covers far more entries than this system will ever put in
 * one directory; a full interior node (needing a node split / depth 2) returns
 * ST_NOSPC rather than build an unverified structure.
 * =================================================================== */

/* ---- the directory hash (mirrors e2fsprogs lib/ext2fs/dirhash.c) ---- */
#define EXT4_DX_DELTA 0x9E3779B9
static void ext4_dx_tea(uint32_t buf[4], const uint32_t in[4])
{
	uint32_t sum = 0, b0 = buf[0], b1 = buf[1];
	uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
	for (int n = 0; n < 16; n++) {
		sum += EXT4_DX_DELTA;
		b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
		b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
	}
	buf[0] += b0;
	buf[1] += b1;
}
#define DXF(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define DXG(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define DXH(x, y, z) ((x) ^ (y) ^ (z))
#define DXR(f, a, b, c, d, x, s) \
	(a += f(b, c, d) + (x), a = (a << (s)) | (a >> (32 - (s))))
static void ext4_dx_md4(uint32_t buf[4], const uint32_t in[8])
{
	uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
	DXR(DXF, a, b, c, d, in[0], 3);
	DXR(DXF, d, a, b, c, in[1], 7);
	DXR(DXF, c, d, a, b, in[2], 11);
	DXR(DXF, b, c, d, a, in[3], 19);
	DXR(DXF, a, b, c, d, in[4], 3);
	DXR(DXF, d, a, b, c, in[5], 7);
	DXR(DXF, c, d, a, b, in[6], 11);
	DXR(DXF, b, c, d, a, in[7], 19);
	DXR(DXG, a, b, c, d, in[1] + 0x5A827999, 3);
	DXR(DXG, d, a, b, c, in[3] + 0x5A827999, 5);
	DXR(DXG, c, d, a, b, in[5] + 0x5A827999, 9);
	DXR(DXG, b, c, d, a, in[7] + 0x5A827999, 13);
	DXR(DXG, a, b, c, d, in[0] + 0x5A827999, 3);
	DXR(DXG, d, a, b, c, in[2] + 0x5A827999, 5);
	DXR(DXG, c, d, a, b, in[4] + 0x5A827999, 9);
	DXR(DXG, b, c, d, a, in[6] + 0x5A827999, 13);
	DXR(DXH, a, b, c, d, in[3] + 0x6ED9EBA1, 3);
	DXR(DXH, d, a, b, c, in[7] + 0x6ED9EBA1, 9);
	DXR(DXH, c, d, a, b, in[2] + 0x6ED9EBA1, 11);
	DXR(DXH, b, c, d, a, in[6] + 0x6ED9EBA1, 15);
	DXR(DXH, a, b, c, d, in[1] + 0x6ED9EBA1, 3);
	DXR(DXH, d, a, b, c, in[5] + 0x6ED9EBA1, 9);
	DXR(DXH, c, d, a, b, in[0] + 0x6ED9EBA1, 11);
	DXR(DXH, b, c, d, a, in[4] + 0x6ED9EBA1, 15);
	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}
static void ext4_dx_str2hb(const char *msg, int len, uint32_t *buf, int num,
			   int sg)
{
	uint32_t pad = (uint32_t)len | ((uint32_t)len << 8);
	pad |= pad << 16;
	uint32_t val = pad;
	if (len > num * 4)
		len = num * 4;
	for (int i = 0; i < len; i++) {
		int c = sg ? (int)(signed char)msg[i] :
			     (int)(unsigned char)msg[i];
		val = (uint32_t)c + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}
static uint32_t ext4_dx_hack(const char *name, int len, int sg)
{
	uint32_t h0 = 0x12a3fe2d, h1 = 0x37abe8f9;
	while (len--) {
		int c = sg ? (int)(signed char)*name :
			     (int)(unsigned char)*name;
		name++;
		uint32_t h = h1 + (h0 ^ (uint32_t)(c * 7152373));
		if (h & 0x80000000u)
			h -= 0x7fffffff;
		h1 = h0;
		h0 = h;
	}
	return h0 << 1;
}
/* Major hash for placement; *minor optional.  Uses the fs hash seed if set. */
uint32_t ext4_dx_hash(const ext4_fs_t *fs, int version, const char *name,
			     int len, uint32_t *minor_out)
{
	uint32_t buf[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	int has_seed = 0;
	for (int i = 0; i < 4; i++)
		if (fs->sb_copy.s_hash_seed[i]) {
			has_seed = 1;
			break;
		}
	if (has_seed)
		for (int i = 0; i < 4; i++)
			buf[i] = fs->sb_copy.s_hash_seed[i];
	uint32_t hash = 0, minor = 0;
	uint32_t in[8];
	const char *p = name;
	int l = len;
	switch (version) {
	case EXT4_HASH_LEGACY_UNS:
		hash = ext4_dx_hack(name, len, 0);
		break;
	case EXT4_HASH_LEGACY:
		hash = ext4_dx_hack(name, len, 1);
		break;
	case EXT4_HASH_HALF_MD4_UNS:
	case EXT4_HASH_HALF_MD4:
		while (l > 0) {
			ext4_dx_str2hb(p, l, in, 8,
				       version == EXT4_HASH_HALF_MD4);
			ext4_dx_md4(buf, in);
			l -= 32;
			p += 32;
		}
		hash = buf[1];
		minor = buf[2];
		break;
	case EXT4_HASH_TEA_UNS:
	case EXT4_HASH_TEA:
		while (l > 0) {
			ext4_dx_str2hb(p, l, in, 4, version == EXT4_HASH_TEA);
			ext4_dx_tea(buf, in);
			l -= 16;
			p += 16;
		}
		hash = buf[0];
		minor = buf[1];
		break;
	default:
		while (l > 0) {
			ext4_dx_str2hb(p, l, in, 8, 1);
			ext4_dx_md4(buf, in);
			l -= 32;
			p += 32;
		}
		hash = buf[1];
		minor = buf[2];
		break; /* unknown -> half_md4 */
	}
	hash &= ~1u;
	if (hash == (uint32_t)(EXT4_HTREE_EOF << 1))
		hash = (uint32_t)((EXT4_HTREE_EOF - 1) << 1);
	if (minor_out)
		*minor_out = minor;
	return hash;
}
