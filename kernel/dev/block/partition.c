// LikeOS - partition tables (GPT and MBR) and partition block devices
//
// Two halves.  The first is a pair of PURE parsers -- blk_parse_gpt() and
// blk_parse_mbr() -- that read sectors only through a struct
// blk_pt_reader and fill a struct blk_pt_result: no locks, no registry, no
// device.  They are what the boxed host tests run against crafted images,
// and with BLK_HOST_TEST defined only this half is compiled.  The second
// half turns a parsed table into partition block devices (sda1 ...): the
// scan at disk_add() time, BLKRRPART rescans, BLKPG add/delete/resize, and
// the life of a struct blk_partition.
//
// ---- GPT (UEFI specification, chapter 5) ---------------------------------------------
//   LBA 0       protective MBR: a 0xEE entry starting at LBA 1.  Without
//               one the disk is not treated as GPT.  Further entries beside
//               the 0xEE one make it a "hybrid" MBR, which is accepted with
//               a note (the GPT is authoritative).
//   LBA 1       primary header: "EFI PART", header size 92..sector, CRC32
//               of the header with its CRC field zeroed, MyLBA = 1, a
//               usable range that excludes the header, the entry array
//               location, entry size 128 * 2^n, its CRC32.
//   the array   NumberOfPartitionEntries * SizeOfPartitionEntry bytes,
//               CRC32-checked as a whole.
//   last LBA    the backup header (AlternateLBA of the primary), whose
//               array normally sits just below it.
// A damaged primary (header or array) makes the parser use the backup
// header and its array, with a note; a backup that is damaged or not at
// the end of the disk (an image written to a bigger disk) is noted but
// does not matter while the primary is good.  Entries: unused when the
// type GUID is zero; first <= last; inside the disk (outside the usable
// range is noted); an entry overlapping an earlier one is dropped.  The
// partition number is the entry's index + 1, as partitioning tools number
// them.  Names are UTF-16LE (36 code units) and become UTF-8.
//
// ---- MBR -----------------------------------------------------------------------------
//   Signature 55 AA at byte 510; four primary entries at byte 446 (boot
//   indicator 00 or 80 -- anything else means this sector is not a
//   partition table, typically a filesystem boot sector on an unpartitioned
//   stick); a 0xEE entry means GPT and is left to the GPT parser.  The disk
//   signature at byte 440 gives the partitions their PARTUUID "ssssssss-nn".
//   An extended partition (05, 0F, 85) is listed itself, only its first
//   1 KiB exposed, and holds a chain of extended boot records: each EBR's
//   first entry is a logical partition (relative to that EBR), its next
//   extended entry points to the next EBR (relative to the extended
//   partition's start).  Logical partitions are numbered from 5.  The walk
//   stops on a missing signature, a link outside the extended partition,
//   an EBR it has visited before (a loop) and after DISK_MAX_PARTS
//   partitions.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>
#ifndef BLK_HOST_TEST
#include <kernel/ke/sched.h>
#endif

/* ==========================================================================
 * Well-known GPT partition type GUIDs
 * ========================================================================== */

const struct blk_guid BLK_GUID_ZERO = { { 0 } };
const struct blk_guid BLK_GUID_EFI_SYSTEM =
	BLK_GUID_INIT(0xc12a7328, 0xf81f, 0x11d2, 0xba4b, 0x00a0c93ec93bULL);
const struct blk_guid BLK_GUID_ROOT_X86_64 =
	BLK_GUID_INIT(0x4f68bce3, 0xe8cd, 0x4db1, 0x96e7, 0xfbcaf984b709ULL);
const struct blk_guid BLK_GUID_GENERIC_DATA =
	BLK_GUID_INIT(0x0fc63daf, 0x8483, 0x4772, 0x8e79, 0x3d69d8477de4ULL);
const struct blk_guid BLK_GUID_BASIC_DATA =
	BLK_GUID_INIT(0xebd0a0a2, 0xb9e5, 0x4433, 0x87c0, 0x68b6b72699c7ULL);
const struct blk_guid BLK_GUID_MS_RESERVED =
	BLK_GUID_INIT(0xe3c9e316, 0x0b5c, 0x4db8, 0x817d, 0xf92df00215aeULL);
const struct blk_guid BLK_GUID_SWAP =
	BLK_GUID_INIT(0x0657fd6d, 0xa4ab, 0x43c4, 0x84e5, 0x0933c84b4f4fULL);
const struct blk_guid BLK_GUID_HOME =
	BLK_GUID_INIT(0x933ac7e1, 0x2eb4, 0x4f13, 0xb844, 0x0e14e2aef915ULL);
const struct blk_guid BLK_GUID_SRV =
	BLK_GUID_INIT(0x3b8f8425, 0x20e0, 0x4f3b, 0x907f, 0x1a25a76f98e8ULL);
const struct blk_guid BLK_GUID_VAR =
	BLK_GUID_INIT(0x4d21b016, 0xb534, 0x45c2, 0xa9fb, 0x5c16e091fd2dULL);
const struct blk_guid BLK_GUID_BIOS_BOOT =
	BLK_GUID_INIT(0x21686148, 0x6449, 0x6e6f, 0x744e, 0x656564454649ULL);
const struct blk_guid BLK_GUID_LVM =
	BLK_GUID_INIT(0xe6d6d379, 0xf507, 0x44c2, 0xa23c, 0x238f2a3df928ULL);
const struct blk_guid BLK_GUID_RAID =
	BLK_GUID_INIT(0xa19d880f, 0x05fc, 0x4d3b, 0xa006, 0x743f0f84911eULL);

static const struct {
	const struct blk_guid *guid;
	const char *name;
} g_type_names[] = {
	{ &BLK_GUID_EFI_SYSTEM, "EFI System" },
	{ &BLK_GUID_ROOT_X86_64, "root (x86-64)" },
	{ &BLK_GUID_GENERIC_DATA, "generic data" },
	{ &BLK_GUID_BASIC_DATA, "basic data" },
	{ &BLK_GUID_MS_RESERVED, "reserved" },
	{ &BLK_GUID_SWAP, "swap" },
	{ &BLK_GUID_HOME, "home" },
	{ &BLK_GUID_SRV, "server data" },
	{ &BLK_GUID_VAR, "variable data" },
	{ &BLK_GUID_BIOS_BOOT, "BIOS boot" },
	{ &BLK_GUID_LVM, "logical volume" },
	{ &BLK_GUID_RAID, "RAID member" },
};

const char *blk_guid_type_name(const struct blk_guid *type)
{
	if (!type)
		return NULL;
	for (unsigned i = 0; i < sizeof(g_type_names) / sizeof(g_type_names[0]);
	     i++)
		if (blk_guid_equal(type, g_type_names[i].guid))
			return g_type_names[i].name;
	return NULL;
}

/* ==========================================================================
 * CRC32 (IEEE 802.3, reflected polynomial 0xEDB88320)
 * ========================================================================== */

static const uint32_t g_crc32_table[256] = {
	0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau,
	0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
	0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u,
	0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
	0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu,
	0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
	0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu,
	0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
	0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u,
	0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
	0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u,
	0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
	0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u,
	0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
	0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u,
	0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
	0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au,
	0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
	0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u,
	0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
	0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
	0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
	0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu,
	0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
	0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u,
	0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
	0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u,
	0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
	0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u,
	0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
	0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u,
	0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
	0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au,
	0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
	0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u,
	0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
	0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu,
	0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
	0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu,
	0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
	0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u,
	0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
	0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u,
	0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
	0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u,
	0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
	0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u,
	0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
	0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au,
	0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
	0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u,
	0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
	0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu,
	0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
	0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu,
	0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
	0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u,
	0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
	0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u,
	0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
	0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u,
	0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
	0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
	0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

uint32_t blk_crc32(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;

	/* Pre- and post-inverted, so the value of one call can be passed to
	 * the next to continue over another piece. */
	crc = ~crc;
	while (len--)
		crc = g_crc32_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

/* ==========================================================================
 * GUID text
 * ========================================================================== */

static const char g_hex[] = "0123456789abcdef";

void blk_guid_to_str(const struct blk_guid *g, char *out)
{
	/* Canonical order: the first three fields are stored little-endian,
	 * the last two big-endian. */
	static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
				       8, 9, 10, 11, 12, 13, 14, 15 };
	int o = 0;

	for (int i = 0; i < 16; i++) {
		uint8_t b = g->b[order[i]];

		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[o++] = '-';
		out[o++] = g_hex[b >> 4];
		out[o++] = g_hex[b & 15];
	}
	out[o] = 0;
}

static int pt_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int blk_guid_from_str(const char *s, struct blk_guid *out)
{
	static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
				       8, 9, 10, 11, 12, 13, 14, 15 };
	struct blk_guid g;
	int braces = 0;
	int bi = 0;

	if (!s || !out)
		return -EINVAL;
	if (*s == '{') {
		braces = 1;
		s++;
	}
	for (int pos = 0; pos < 36; pos++) {
		if (pos == 8 || pos == 13 || pos == 18 || pos == 23) {
			if (s[pos] != '-')
				return -EINVAL;
			continue;
		}
		{
			int hi = pt_hexval(s[pos]);
			int lo;

			if (hi < 0)
				return -EINVAL;
			pos++;
			lo = pt_hexval(s[pos]);
			if (lo < 0)
				return -EINVAL;
			g.b[order[bi++]] = (uint8_t)((hi << 4) | lo);
		}
	}
	s += 36;
	if (braces) {
		if (*s != '}')
			return -EINVAL;
		s++;
	}
	if (*s && *s != '\n')
		return -EINVAL;
	*out = g;
	return 0;
}

/* ==========================================================================
 * Parser helpers
 * ========================================================================== */

static inline uint16_t pt_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t pt_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline uint64_t pt_le64(const uint8_t *p)
{
	return (uint64_t)pt_le32(p) | ((uint64_t)pt_le32(p + 4) << 32);
}

static int pt_memeq(const void *a, const void *b, size_t n)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;

	for (size_t i = 0; i < n; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

/* Record a problem: printed now, the first one kept in out->warn. */
static void pt_note(struct blk_pt_result *out, const char *msg)
{
	kprintf(BLK_LOG_PREFIX "partition table: %s\n", msg);
	if (out && !out->warn[0]) {
		size_t k = 0;
		while (msg[k] && k < sizeof(out->warn) - 1) {
			out->warn[k] = msg[k];
			k++;
		}
		out->warn[k] = 0;
	}
}

static int pt_reader_ok(const struct blk_pt_reader *rd)
{
	if (!rd || !rd->read)
		return 0;
	if (rd->sector_size < 512 || rd->sector_size > 65536 ||
	    (rd->sector_size & (rd->sector_size - 1)))
		return 0;
	return 1;
}

static int pt_read(const struct blk_pt_reader *rd, uint64_t lba,
		   uint32_t count, void *buf)
{
	if (lba >= rd->nr_sectors || lba + count > rd->nr_sectors ||
	    lba + count < lba)
		return -EINVAL;
	return rd->read(rd->ctx, lba, count, buf);
}

/* UTF-16LE (n code units, NUL-terminated or not) -> UTF-8 into out[cap],
 * cut at a character boundary.  Unpaired surrogates become U+FFFD. */
static void pt_utf16le_to_utf8(const uint8_t *in, int n, char *out, size_t cap)
{
	size_t o = 0;

	for (int i = 0; i < n; i++) {
		uint32_t c = pt_le16(in + 2 * i);
		char enc[4];
		int len;

		if (c == 0)
			break;
		if (c >= 0xD800 && c <= 0xDBFF) {
			uint32_t lo = (i + 1 < n) ? pt_le16(in + 2 * (i + 1)) : 0;
			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
				i++;
			} else {
				c = 0xFFFD;
			}
		} else if (c >= 0xDC00 && c <= 0xDFFF) {
			c = 0xFFFD;
		}
		if (c < 0x80) {
			enc[0] = (char)c;
			len = 1;
		} else if (c < 0x800) {
			enc[0] = (char)(0xC0 | (c >> 6));
			enc[1] = (char)(0x80 | (c & 0x3F));
			len = 2;
		} else if (c < 0x10000) {
			enc[0] = (char)(0xE0 | (c >> 12));
			enc[1] = (char)(0x80 | ((c >> 6) & 0x3F));
			enc[2] = (char)(0x80 | (c & 0x3F));
			len = 3;
		} else {
			enc[0] = (char)(0xF0 | (c >> 18));
			enc[1] = (char)(0x80 | ((c >> 12) & 0x3F));
			enc[2] = (char)(0x80 | ((c >> 6) & 0x3F));
			enc[3] = (char)(0x80 | (c & 0x3F));
			len = 4;
		}
		if (o + (size_t)len + 1 > cap)
			break;
		for (int k = 0; k < len; k++)
			out[o++] = enc[k];
	}
	out[o] = 0;
}

static void pt_result_reset(struct blk_pt_result *out)
{
	mm_memset(out, 0, sizeof(*out));
	out->type = BLK_PT_NONE;
}

/* Does [s, s+n) overlap an entry already accepted (the extended
 * container of an MBR excluded)? */
static int pt_overlaps(const struct blk_pt_result *out, uint64_t s, uint64_t n,
		       int *with)
{
	for (int i = 0; i < out->nr; i++) {
		const struct blk_pt_entry *e = &out->e[i];

		if (e->flags & PART_FL_EXTENDED)
			continue;
		if (s < e->start_lba + e->nr_sectors && e->start_lba < s + n) {
			if (with)
				*with = e->partno;
			return 1;
		}
	}
	return 0;
}

/* ==========================================================================
 * MBR
 * ========================================================================== */

#define MBR_SIG_OFF 510
#define MBR_TABLE_OFF 446
#define MBR_DISKSIG_OFF 440
#define MBR_TYPE_GPT 0xEE

static int pt_mbr_is_extended(uint8_t t)
{
	return t == 0x05 || t == 0x0F || t == 0x85;
}

/* A filesystem boot sector (a disk formatted without a partition table):
 * x86 jump at byte 0 and a FAT/NTFS/exFAT marker where one belongs. */
static int pt_looks_like_vbr(const uint8_t *b)
{
	int jump = (b[0] == 0xEB && b[2] == 0x90) || b[0] == 0xE9;

	if (!jump)
		return 0;
	return pt_memeq(b + 54, "FAT", 3) || pt_memeq(b + 82, "FAT", 3) ||
	       pt_memeq(b + 3, "NTFS    ", 8) || pt_memeq(b + 3, "EXFAT   ", 8);
}

static void pt_add_entry(struct blk_pt_result *out, int partno, uint32_t flags,
			 uint64_t start, uint64_t nr, uint8_t type,
			 uint8_t active)
{
	struct blk_pt_entry *e;

	if (out->nr >= DISK_MAX_PARTS)
		return;
	e = &out->e[out->nr++];
	mm_memset(e, 0, sizeof(*e));
	e->partno = partno;
	e->flags = flags | (active == 0x80 ? PART_FL_BOOTABLE : 0);
	e->start_lba = start;
	e->nr_sectors = nr;
	e->mbr_type = type;
	e->mbr_active = active;
}

/* Walk the EBR chain of one extended partition. */
static int pt_mbr_extended(const struct blk_pt_reader *rd,
			   struct blk_pt_result *out, uint8_t *buf,
			   uint64_t ext_start, uint64_t ext_size, int *partno)
{
	uint64_t visited[DISK_MAX_PARTS];
	int nvis = 0;
	uint64_t this_sect = ext_start, this_size = ext_size;
	uint64_t ext_end = ext_start + ext_size;
	char msg[128];

	for (;;) {
		uint64_t next_ebr = 0, next_size = 0;
		int rc;

		if (*partno > DISK_MAX_PARTS) {
			ksnprintf(msg, sizeof(msg),
				  "more than %d partitions: the rest of the extended chain is ignored",
				  DISK_MAX_PARTS);
			pt_note(out, msg);
			return 0;
		}
		for (int i = 0; i < nvis; i++) {
			if (visited[i] == this_sect) {
				ksnprintf(msg, sizeof(msg),
					  "loop in the extended partition chain at sector %llu",
					  (unsigned long long)this_sect);
				pt_note(out, msg);
				return 0;
			}
		}
		if (nvis >= DISK_MAX_PARTS) {
			pt_note(out, "extended partition chain too long");
			return 0;
		}
		visited[nvis++] = this_sect;

		rc = pt_read(rd, this_sect, 1, buf);
		if (rc) {
			ksnprintf(msg, sizeof(msg),
				  "extended boot record at sector %llu unreadable (%d)",
				  (unsigned long long)this_sect, rc);
			pt_note(out, msg);
			return 0;
		}
		if (pt_le16(buf + MBR_SIG_OFF) != 0xAA55) {
			ksnprintf(msg, sizeof(msg),
				  "extended boot record at sector %llu has no signature",
				  (unsigned long long)this_sect);
			pt_note(out, msg);
			return 0;
		}
		for (int i = 0; i < 4; i++) {
			const uint8_t *pe = buf + MBR_TABLE_OFF + 16 * i;
			uint8_t type = pe[4];
			uint64_t start = pt_le32(pe + 8);
			uint64_t size = pt_le32(pe + 12);
			uint64_t abs;
			int with;

			if (!type || !size)
				continue;
			if (pt_mbr_is_extended(type)) {
				/* the link to the next record, relative to the
				 * extended partition's start */
				if (!next_ebr) {
					next_ebr = ext_start + start;
					next_size = size;
				}
				continue;
			}
			/* Entries 3 and 4 of an EBR are unusual; accept them
			 * only when they stay inside this record's area. */
			if (i >= 2 && (start + size > this_size))
				continue;
			abs = this_sect + start;
			if (abs < ext_start || abs + size > ext_end ||
			    abs + size < abs) {
				ksnprintf(msg, sizeof(msg),
					  "logical partition %d lies outside its extended partition: ignored",
					  *partno);
				pt_note(out, msg);
				continue;
			}
			if (abs + size > rd->nr_sectors) {
				ksnprintf(msg, sizeof(msg),
					  "logical partition %d extends beyond the disk: ignored",
					  *partno);
				pt_note(out, msg);
				continue;
			}
			if (pt_overlaps(out, abs, size, &with)) {
				ksnprintf(msg, sizeof(msg),
					  "logical partition %d overlaps partition %d: ignored",
					  *partno, with);
				pt_note(out, msg);
				continue;
			}
			if (*partno > DISK_MAX_PARTS)
				break;
			pt_add_entry(out, (*partno)++, PART_FL_LOGICAL, abs, size,
				     type, pe[0]);
		}
		if (!next_ebr)
			return 0;
		if (next_ebr <= ext_start || next_ebr >= ext_end ||
		    next_ebr + next_size > ext_end + 1) {
			/* (the +1 tolerates the off-by-one some tools write
			 * into the last link) */
			if (next_ebr <= ext_start || next_ebr >= ext_end) {
				ksnprintf(msg, sizeof(msg),
					  "extended partition chain leaves its partition (sector %llu)",
					  (unsigned long long)next_ebr);
				pt_note(out, msg);
				return 0;
			}
		}
		this_sect = next_ebr;
		this_size = next_size;
	}
}

int blk_parse_mbr(const struct blk_pt_reader *rd, struct blk_pt_result *out)
{
	uint8_t *buf;
	int rc, empty = 1;
	int partno = 5;
	uint64_t ext_start[4], ext_size[4];
	int next = 0;
	char msg[128];

	if (!out)
		return -EINVAL;
	pt_result_reset(out);
	if (!pt_reader_ok(rd))
		return -EINVAL;
	if (rd->nr_sectors < 1)
		return -ENOENT;
	buf = (uint8_t *)kalloc(rd->sector_size);
	if (!buf)
		return -ENOMEM;
	rc = pt_read(rd, 0, 1, buf);
	if (rc) {
		kfree(buf);
		return -EIO;
	}
	if (pt_le16(buf + MBR_SIG_OFF) != 0xAA55) {
		kfree(buf);
		return -ENOENT;
	}
	for (int i = 0; i < 4; i++) {
		const uint8_t *pe = buf + MBR_TABLE_OFF + 16 * i;

		/* A boot indicator other than 00/80: not a partition table
		 * (a filesystem's boot sector carries code here). */
		if (pe[0] != 0x00 && pe[0] != 0x80) {
			kfree(buf);
			return -ENOENT;
		}
		if (pe[4] == MBR_TYPE_GPT) {
			kfree(buf);
			return -ENOENT; /* protective/hybrid: GPT decides */
		}
		if (pe[4])
			empty = 0;
	}
	if (empty && pt_looks_like_vbr(buf)) {
		kfree(buf);
		return -ENOENT; /* a filesystem on the whole disk */
	}
	out->type = BLK_PT_MBR;
	out->mbr_signature = pt_le32(buf + MBR_DISKSIG_OFF);

	for (int i = 0; i < 4; i++) {
		const uint8_t *pe = buf + MBR_TABLE_OFF + 16 * i;
		uint8_t type = pe[4];
		uint64_t start = pt_le32(pe + 8);
		uint64_t size = pt_le32(pe + 12);
		int slot = i + 1;
		int with;

		if (!type || !size)
			continue;
		if (start == 0) {
			ksnprintf(msg, sizeof(msg),
				  "partition %d starts at sector 0: ignored", slot);
			pt_note(out, msg);
			continue;
		}
		if (start >= rd->nr_sectors) {
			ksnprintf(msg, sizeof(msg),
				  "partition %d starts beyond the end of the disk: ignored",
				  slot);
			pt_note(out, msg);
			continue;
		}
		if (start + size > rd->nr_sectors) {
			ksnprintf(msg, sizeof(msg),
				  "partition %d extends beyond the end of the disk: truncated",
				  slot);
			pt_note(out, msg);
			size = rd->nr_sectors - start;
		}
		if (pt_mbr_is_extended(type)) {
			/* The container itself, only its first 1 KiB visible:
			 * enough for tools to see it, too little to hurt the
			 * EBRs by writing to it. */
			uint64_t vis = 1024 / rd->sector_size;

			if (!vis)
				vis = 1;
			if (vis > size)
				vis = size;
			pt_add_entry(out, slot, PART_FL_EXTENDED, start, vis, type,
				     pe[0]);
			if (next < 4) {
				ext_start[next] = start;
				ext_size[next] = size;
				next++;
			}
			continue;
		}
		if (pt_overlaps(out, start, size, &with)) {
			ksnprintf(msg, sizeof(msg),
				  "partition %d overlaps partition %d", slot, with);
			pt_note(out, msg);
		}
		pt_add_entry(out, slot, 0, start, size, type, pe[0]);
	}
	for (int k = 0; k < next; k++) {
		rc = pt_mbr_extended(rd, out, buf, ext_start[k], ext_size[k],
				     &partno);
		if (rc)
			break;
	}
	kfree(buf);
	return 0;
}

/* ==========================================================================
 * GPT
 * ========================================================================== */

#define GPT_SIGNATURE "EFI PART"
#define GPT_HDR_MIN 92
#define GPT_ENTRY_MIN 128
#define GPT_ENTRY_MAX 4096
#define GPT_ARRAY_MAX (1024 * 1024) /* bytes of entries we accept */

struct pt_gpt_hdr {
	uint32_t revision;
	uint32_t header_size;
	uint64_t my_lba, alt_lba;
	uint64_t first_usable, last_usable;
	struct blk_guid disk_guid;
	uint64_t entries_lba;
	uint32_t num_entries, entry_size, entries_crc;
};

/* 0 none, 1 protective, 2 hybrid. */
static int pt_pmbr_kind(const uint8_t *mbr, uint64_t nr_sectors,
			struct blk_pt_result *out)
{
	int ee = -1, others = 0;
	char msg[128];

	if (pt_le16(mbr + MBR_SIG_OFF) != 0xAA55)
		return 0;
	for (int i = 0; i < 4; i++) {
		const uint8_t *pe = mbr + MBR_TABLE_OFF + 16 * i;

		if (pe[4] == MBR_TYPE_GPT) {
			if (ee < 0)
				ee = i;
		} else if (pe[4]) {
			others++;
		}
	}
	if (ee < 0)
		return 0;
	{
		const uint8_t *pe = mbr + MBR_TABLE_OFF + 16 * ee;
		uint32_t start = pt_le32(pe + 8);
		uint32_t size = pt_le32(pe + 12);
		uint64_t want = nr_sectors - 1;

		if (start != 1)
			return 0; /* the 0xEE entry must cover LBA 1 */
		if (want > 0xFFFFFFFFULL)
			want = 0xFFFFFFFFULL;
		if (!others && size != want && size != 0xFFFFFFFFu) {
			ksnprintf(msg, sizeof(msg),
				  "protective MBR covers %u sectors, the disk has %llu",
				  size, (unsigned long long)nr_sectors);
			pt_note(out, msg);
		}
	}
	return others ? 2 : 1;
}

/* Read and validate the header at `lba'.  0, -EINVAL (not a valid header)
 * or -EIO (unreadable). */
static int pt_gpt_header(const struct blk_pt_reader *rd, uint64_t lba,
			 uint8_t *buf, struct pt_gpt_hdr *h, const char **why)
{
	uint32_t crc_stored, crc;
	uint64_t array_bytes, array_sectors;

	*why = "";
	if (lba == 0 || lba >= rd->nr_sectors) {
		*why = "its location is outside the disk";
		return -EINVAL;
	}
	if (pt_read(rd, lba, 1, buf)) {
		*why = "it could not be read";
		return -EIO;
	}
	if (!pt_memeq(buf, GPT_SIGNATURE, 8)) {
		*why = "no EFI PART signature";
		return -EINVAL;
	}
	h->revision = pt_le32(buf + 8);
	h->header_size = pt_le32(buf + 12);
	if (h->header_size < GPT_HDR_MIN || h->header_size > rd->sector_size) {
		*why = "header size out of range";
		return -EINVAL;
	}
	crc_stored = pt_le32(buf + 16);
	buf[16] = buf[17] = buf[18] = buf[19] = 0;
	crc = blk_crc32(0, buf, h->header_size);
	if (crc != crc_stored) {
		*why = "header CRC mismatch";
		return -EINVAL;
	}
	h->my_lba = pt_le64(buf + 24);
	h->alt_lba = pt_le64(buf + 32);
	h->first_usable = pt_le64(buf + 40);
	h->last_usable = pt_le64(buf + 48);
	for (int i = 0; i < 16; i++)
		h->disk_guid.b[i] = buf[56 + i];
	h->entries_lba = pt_le64(buf + 72);
	h->num_entries = pt_le32(buf + 80);
	h->entry_size = pt_le32(buf + 84);
	h->entries_crc = pt_le32(buf + 88);

	if (h->my_lba != lba) {
		*why = "MyLBA does not match its location";
		return -EINVAL;
	}
	if (h->first_usable > h->last_usable ||
	    h->last_usable >= rd->nr_sectors) {
		*why = "usable range invalid";
		return -EINVAL;
	}
	if (h->my_lba >= h->first_usable && h->my_lba <= h->last_usable) {
		*why = "header inside the usable range";
		return -EINVAL;
	}
	if (h->entry_size < GPT_ENTRY_MIN || h->entry_size > GPT_ENTRY_MAX ||
	    (h->entry_size & (h->entry_size - 1))) {
		*why = "partition entry size invalid";
		return -EINVAL;
	}
	if (h->num_entries == 0) {
		*why = "no partition entries";
		return -EINVAL;
	}
	array_bytes = (uint64_t)h->num_entries * h->entry_size;
	if (array_bytes > GPT_ARRAY_MAX) {
		*why = "partition entry array too large";
		return -EINVAL;
	}
	array_sectors = (array_bytes + rd->sector_size - 1) / rd->sector_size;
	if (h->entries_lba == 0 || h->entries_lba + array_sectors > rd->nr_sectors ||
	    (h->entries_lba <= h->last_usable &&
	     h->entries_lba + array_sectors > h->first_usable)) {
		*why = "partition entry array misplaced";
		return -EINVAL;
	}
	return 0;
}

/* Read the entry array of a valid header and check its CRC.  The array
 * (allocated) is returned in *arr. */
static int pt_gpt_entries(const struct blk_pt_reader *rd,
			  const struct pt_gpt_hdr *h, uint8_t **arr,
			  const char **why)
{
	uint64_t bytes = (uint64_t)h->num_entries * h->entry_size;
	uint32_t sectors = (uint32_t)((bytes + rd->sector_size - 1) /
				      rd->sector_size);
	uint8_t *a;

	*arr = NULL;
	a = (uint8_t *)kalloc((size_t)sectors * rd->sector_size);
	if (!a) {
		*why = "no memory for the entry array";
		return -ENOMEM;
	}
	if (pt_read(rd, h->entries_lba, sectors, a)) {
		kfree(a);
		*why = "entry array could not be read";
		return -EIO;
	}
	if (blk_crc32(0, a, (size_t)bytes) != h->entries_crc) {
		kfree(a);
		*why = "entry array CRC mismatch";
		return -EINVAL;
	}
	*arr = a;
	return 0;
}

/* A valid header together with a valid array. */
static int pt_gpt_load(const struct blk_pt_reader *rd, uint64_t lba,
		       uint8_t *buf, struct pt_gpt_hdr *h, uint8_t **arr,
		       const char **why, int *header_ok)
{
	int rc = pt_gpt_header(rd, lba, buf, h, why);

	*header_ok = rc == 0;
	if (rc)
		return rc;
	return pt_gpt_entries(rd, h, arr, why);
}

/* Differences between the two headers worth a note. */
static void pt_gpt_compare(const struct pt_gpt_hdr *p,
			   const struct pt_gpt_hdr *b,
			   struct blk_pt_result *out)
{
	if (p->my_lba != b->alt_lba)
		pt_note(out, "backup GPT header does not point back to the primary");
	if (p->alt_lba != b->my_lba)
		pt_note(out, "primary GPT header does not point to the backup");
	if (p->first_usable != b->first_usable ||
	    p->last_usable != b->last_usable)
		pt_note(out, "GPT headers disagree on the usable range");
	if (p->num_entries != b->num_entries || p->entry_size != b->entry_size)
		pt_note(out, "GPT headers disagree on the entry array size");
	if (p->entries_crc != b->entries_crc)
		pt_note(out, "GPT entry arrays differ");
	if (!blk_guid_equal(&p->disk_guid, &b->disk_guid))
		pt_note(out, "GPT headers disagree on the disk GUID");
}

static void pt_gpt_parse_entries(const struct blk_pt_reader *rd,
				 const struct pt_gpt_hdr *h, const uint8_t *arr,
				 int backup, struct blk_pt_result *out)
{
	char msg[128];

	for (uint32_t i = 0; i < h->num_entries; i++) {
		const uint8_t *e = arr + (size_t)i * h->entry_size;
		struct blk_guid type;
		uint64_t first, last;
		struct blk_pt_entry *pe;
		int with;

		for (int k = 0; k < 16; k++)
			type.b[k] = e[k];
		if (blk_guid_is_zero(&type))
			continue;
		if (i >= DISK_MAX_PARTS) {
			ksnprintf(msg, sizeof(msg),
				  "GPT entry %u beyond the %d partitions supported: ignored",
				  i + 1, DISK_MAX_PARTS);
			pt_note(out, msg);
			continue;
		}
		first = pt_le64(e + 32);
		last = pt_le64(e + 40);
		if (first > last) {
			ksnprintf(msg, sizeof(msg),
				  "GPT partition %u ends before it starts: ignored",
				  i + 1);
			pt_note(out, msg);
			continue;
		}
		if (last >= rd->nr_sectors) {
			ksnprintf(msg, sizeof(msg),
				  "GPT partition %u extends beyond the end of the disk: ignored",
				  i + 1);
			pt_note(out, msg);
			continue;
		}
		if (first < h->first_usable || last > h->last_usable) {
			ksnprintf(msg, sizeof(msg),
				  "GPT partition %u lies outside the usable area",
				  i + 1);
			pt_note(out, msg);
		}
		if (pt_overlaps(out, first, last - first + 1, &with)) {
			ksnprintf(msg, sizeof(msg),
				  "GPT partition %u overlaps partition %d: ignored",
				  i + 1, with);
			pt_note(out, msg);
			continue;
		}
		pe = &out->e[out->nr++];
		mm_memset(pe, 0, sizeof(*pe));
		pe->partno = (int)i + 1;
		pe->start_lba = first;
		pe->nr_sectors = last - first + 1;
		pe->type_guid = type;
		for (int k = 0; k < 16; k++)
			pe->part_guid.b[k] = e[16 + k];
		pe->gpt_attrs = pt_le64(e + 48);
		pt_utf16le_to_utf8(e + 56, 36, pe->name, sizeof(pe->name));
		pe->flags = (backup ? PART_FL_BACKUP_GPT : 0) |
			    ((pe->gpt_attrs & BLK_GPT_ATTR_LEGACY_BOOT) ?
				     PART_FL_BOOTABLE :
				     0);
	}
}

int blk_parse_gpt(const struct blk_pt_reader *rd, struct blk_pt_result *out)
{
	uint8_t *buf;
	struct pt_gpt_hdr ph, bh;
	uint8_t *parr = NULL, *barr = NULL;
	const char *pwhy = "", *bwhy = "";
	int prc, brc = -EINVAL, pkind;
	int phdr_ok = 0, bhdr_ok = 0;
	uint64_t last = 0;
	char msg[160];

	if (!out)
		return -EINVAL;
	pt_result_reset(out);
	if (!pt_reader_ok(rd))
		return -EINVAL;
	if (rd->nr_sectors < 3)
		return -ENOENT;
	last = rd->nr_sectors - 1;
	buf = (uint8_t *)kalloc(rd->sector_size);
	if (!buf)
		return -ENOMEM;
	if (pt_read(rd, 0, 1, buf)) {
		kfree(buf);
		return -EIO;
	}
	pkind = pt_pmbr_kind(buf, rd->nr_sectors, out);
	if (!pkind) {
		kfree(buf);
		return -ENOENT;
	}
	if (pkind == 2)
		pt_note(out, "hybrid MBR: the GPT is used, the MBR entries are ignored");

	prc = pt_gpt_load(rd, 1, buf, &ph, &parr, &pwhy, &phdr_ok);
	if (prc == 0) {
		/* Primary good.  The backup is checked for the notes only. */
		if (ph.alt_lba != last) {
			ksnprintf(msg, sizeof(msg),
				  "backup GPT header not at the end of the disk (sector %llu, last sector %llu)",
				  (unsigned long long)ph.alt_lba,
				  (unsigned long long)last);
			pt_note(out, msg);
		}
		if (ph.alt_lba < rd->nr_sectors && ph.alt_lba != 1) {
			brc = pt_gpt_load(rd, ph.alt_lba, buf, &bh, &barr,
					  &bwhy, &bhdr_ok);
			if (brc) {
				ksnprintf(msg, sizeof(msg),
					  "backup GPT is invalid (%s); using the primary",
					  bwhy);
				pt_note(out, msg);
			} else {
				pt_gpt_compare(&ph, &bh, out);
			}
		}
		pt_gpt_parse_entries(rd, &ph, parr, 0, out);
		out->disk_guid = ph.disk_guid;
		out->type = BLK_PT_GPT;
		goto done;
	}

	ksnprintf(msg, sizeof(msg), "primary GPT is invalid (%s); trying the backup",
		  pwhy);
	pt_note(out, msg);
	/* Where the backup is: the primary header names it when the header
	 * itself was fine (only its array was not); the last sector always. */
	if (phdr_ok && ph.alt_lba != last && ph.alt_lba < rd->nr_sectors &&
	    ph.alt_lba > 1)
		brc = pt_gpt_load(rd, ph.alt_lba, buf, &bh, &barr, &bwhy,
				  &bhdr_ok);
	if (brc)
		brc = pt_gpt_load(rd, last, buf, &bh, &barr, &bwhy, &bhdr_ok);
	if (brc) {
		ksnprintf(msg, sizeof(msg),
			  "backup GPT is invalid too (%s): no usable partition table",
			  bwhy);
		pt_note(out, msg);
		kfree(buf);
		if (parr)
			kfree(parr);
		return (prc == -EIO && brc == -EIO) ? -EIO :
		       (prc == -ENOMEM || brc == -ENOMEM) ? -ENOMEM :
							       -EINVAL;
	}
	pt_note(out, "using the backup GPT; the primary should be repaired");
	out->used_backup_gpt = 1;
	pt_gpt_parse_entries(rd, &bh, barr, 1, out);
	out->disk_guid = bh.disk_guid;
	out->type = BLK_PT_GPT;

done:
	if (parr)
		kfree(parr);
	if (barr)
		kfree(barr);
	kfree(buf);
	return 0;
}

int blk_parse_partitions(const struct blk_pt_reader *rd,
			 struct blk_pt_result *out)
{
	int rc;

	if (!out)
		return -EINVAL;
	rc = blk_parse_gpt(rd, out);
	if (rc == 0 || rc == -ENOMEM)
		return rc;
	if (rc == -ENOENT) {
		/* No protective MBR: an MBR table, or nothing. */
		return blk_parse_mbr(rd, out);
	}
	/* A protective MBR with no usable GPT behind it.  The MBR parser
	 * would refuse the 0xEE entry; report the damage. */
	{
		char keep[128];
		size_t k = 0;

		while (out->warn[k] && k < sizeof(keep) - 1) {
			keep[k] = out->warn[k];
			k++;
		}
		keep[k] = 0;
		pt_result_reset(out);
		for (k = 0; keep[k]; k++)
			out->warn[k] = keep[k];
		out->warn[k] = 0;
	}
	return rc;
}

/* ==========================================================================
 * Partition devices (kernel only)
 * ========================================================================== */
#ifndef BLK_HOST_TEST

const char *blk_pt_type_name(enum blk_pt_type t)
{
	switch (t) {
	case BLK_PT_GPT:
		return "gpt";
	case BLK_PT_MBR:
		return "dos";
	default:
		return "none";
	}
}

static int pt_disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
	struct gendisk *disk = (struct gendisk *)ctx;

	/* Probing reads: quiet (an unreadable sector is reported once by the
	 * parser), no driver retries beyond the first. */
	return blk_read_sync_flags(&disk->bdev, lba, count, buf,
				   BLK_REQ_QUIET | BLK_REQ_FAILFAST);
}

int blk_partition_busy_locked(struct gendisk *disk, struct blk_partition *p)
{
	if (p)
		return p->bdev.open_count > 0 || p->bdev.holder != NULL;
	for (int i = 1; i <= DISK_MAX_PARTS; i++) {
		struct blk_partition *q = disk->part[i];
		if (q && (q->bdev.open_count > 0 || q->bdev.holder))
			return 1;
	}
	return 0;
}

/* Alignment offset of a partition: how far its first sector is from a
 * physical sector boundary, in bytes. */
static uint32_t pt_alignment_offset(const struct gendisk *disk, uint64_t start)
{
	uint32_t phys = disk->limits.physical_block_size;
	uint32_t lbs = disk->limits.logical_block_size;
	uint64_t off;

	if (!phys || phys <= lbs)
		return 0;
	off = (start * lbs + phys - disk->limits.alignment_offset) % phys;
	return off ? (uint32_t)(phys - off) : 0;
}

struct blk_partition *blk_partition_create_locked(struct gendisk *disk,
						  const struct blk_pt_entry *e)
{
	struct blk_partition *p;
	block_device_t *b;
	int partno;

	if (!disk || !e)
		return NULL;
	partno = e->partno;
	if (partno < 1 || partno > DISK_MAX_PARTS) {
		kprintf(BLK_LOG_PREFIX "%s: partition number %d out of range\n",
			disk->disk_name, partno);
		return NULL;
	}
	if (disk->part[partno]) {
		kprintf(BLK_LOG_PREFIX "%s: partition %d exists already\n",
			disk->disk_name, partno);
		return NULL;
	}
	if (!e->nr_sectors || e->start_lba + e->nr_sectors > disk->capacity ||
	    e->start_lba + e->nr_sectors < e->start_lba) {
		kprintf(BLK_LOG_PREFIX "%s: partition %d (%llu+%llu) does not fit the disk\n",
			disk->disk_name, partno,
			(unsigned long long)e->start_lba,
			(unsigned long long)e->nr_sectors);
		return NULL;
	}
	p = (struct blk_partition *)kcalloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->disk = disk;
	p->partno = partno;
	p->flags = e->flags;
	p->start_lba = e->start_lba;
	p->nr_sectors = e->nr_sectors;
	p->type_guid = e->type_guid;
	p->part_guid = e->part_guid;
	p->gpt_attrs = e->gpt_attrs;
	for (size_t k = 0; k < sizeof(p->name); k++)
		p->name[k] = e->name[k];
	p->name[sizeof(p->name) - 1] = 0;
	p->mbr_type = e->mbr_type;
	p->mbr_active = e->mbr_active;
	if (!blk_guid_is_zero(&p->part_guid))
		blk_guid_to_str(&p->part_guid, p->partuuid);
	else if (disk->pt_type == BLK_PT_MBR)
		ksnprintf(p->partuuid, sizeof(p->partuuid), "%08x-%02x",
			  disk->mbr_signature, partno);
	if (disk_part_name(disk->disk_name, partno, p->dev_name,
			   sizeof(p->dev_name))) {
		kfree(p);
		return NULL;
	}

	b = &p->bdev;
	b->name = p->dev_name;
	b->sector_size = disk->bdev.sector_size;
	b->total_sectors = (unsigned long)p->nr_sectors;
	b->read = blk_legacy_read;
	b->write = blk_legacy_write;
	b->sync = blk_legacy_sync;
	b->release_locks_for_task = NULL;
	b->driver_data = disk->bdev.driver_data;
	b->magic = BLOCK_DEVICE_MAGIC;
	b->flags = BDEV_F_PARTITION;
	b->ops = disk->ops;
	b->disk = disk;
	b->part = p;
	b->parent = &disk->bdev;
	b->queue = disk->queue;
	b->start_lba = p->start_lba;
	if (partno < DISK_MINORS && disk->kind == BLK_DISK_SD) {
		b->major = disk->major;
		b->minor = disk->first_minor + (uint32_t)partno;
	} else {
		uint32_t m;
		if (genhd_ext_minor_alloc_locked(&m)) {
			kprintf(BLK_LOG_PREFIX "%s: no device number left for %s\n",
				disk->disk_name, p->dev_name);
			kfree(p);
			return NULL;
		}
		b->major = BLK_EXT_MAJOR;
		b->minor = m;
	}
	b->physical_sector_size = disk->bdev.physical_sector_size;
	b->alignment_offset = pt_alignment_offset(disk, p->start_lba);
	b->io_min = disk->bdev.io_min;
	b->io_opt = disk->bdev.io_opt;
	b->max_sectors = disk->bdev.max_sectors;
	b->queue_depth = disk->bdev.queue_depth;
	b->discard_granularity = disk->bdev.discard_granularity;
	b->discard_max_sectors = disk->bdev.discard_max_sectors;
	b->write_zeroes_max_sectors = disk->bdev.write_zeroes_max_sectors;
	b->ro = 0; /* the disk's own protection is checked separately */
	b->removable = disk->bdev.removable;
	b->rotational = disk->bdev.rotational;
	b->flush_supported = disk->bdev.flush_supported;
	b->fua_supported = disk->bdev.fua_supported;
	b->discard_zeroes_data = disk->bdev.discard_zeroes_data;
	b->ready = disk->bdev.ready;
	refcount_set(&b->refcount, 1); /* the registry's */
	b->registry_index = -1;
	b->open_count = 0;
	b->holder = NULL;
	b->diskseq = disk->diskseq;

	disk_get(disk); /* a partition pins its disk */
	if (block_register(b) != ST_OK) {
		if (b->major == BLK_EXT_MAJOR)
			genhd_ext_minor_free_locked(b->minor);
		disk_put(disk);
		kfree(p);
		return NULL;
	}
	disk->part[partno] = p;
	if (partno > disk->nr_parts)
		disk->nr_parts = partno;
	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		int rc = blk_sysfs_add_partition_locked(p);
		if (rc)
			kprintf(BLK_LOG_PREFIX "%s: sysfs entries failed (%d)\n",
				p->dev_name, rc);
	}
	/* The node now; the filesystem identity (I/O) is probed by the
	 * caller after the registry mutex is dropped, and its links follow
	 * then. */
	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		int rc = blkdev_node_add_locked(b);
		if (rc)
			kprintf(BLK_LOG_PREFIX "%s: /dev node failed (%d)\n",
				p->dev_name, rc);
	}
	return p;
}

void blk_partition_release(struct blk_partition *p)
{
	struct gendisk *disk;

	if (!p)
		return;
	disk = p->disk;
	WARN_ON(p->bdev.flags & BDEV_F_REGISTERED);
	p->bdev.magic = 0;
	kfree(p);
	disk_put(disk);
}

void blk_partition_remove_locked(struct blk_partition *p)
{
	struct gendisk *disk;
	block_device_t *b;

	if (!p)
		return;
	disk = p->disk;
	b = &p->bdev;
	b->flags |= BDEV_F_DEAD;
	b->ready = 0;
	if (!(disk->flags & GENHD_FL_HIDDEN)) {
		blkdev_node_remove_locked(b);
		blk_sysfs_remove_partition_locked(p);
	}
	block_unregister(b);
	if (disk->part[p->partno] == p)
		disk->part[p->partno] = NULL;
	if (b->major == BLK_EXT_MAJOR)
		genhd_ext_minor_free_locked(b->minor);
	{
		int hi = 0;
		for (int i = 1; i <= DISK_MAX_PARTS; i++)
			if (disk->part[i])
				hi = i;
		disk->nr_parts = hi;
	}
	/* The registry's reference; requests in flight and open handles keep
	 * the record until they are done with it. */
	blk_bdev_put(b);
}

int blk_partition_remove_all_locked(struct gendisk *disk, int force)
{
	if (!disk)
		return -EINVAL;
	if (!force && blk_partition_busy_locked(disk, NULL))
		return -EBUSY;
	for (int i = DISK_MAX_PARTS; i >= 1; i--)
		if (disk->part[i])
			blk_partition_remove_locked(disk->part[i]);
	disk->nr_parts = 0;
	return 0;
}

int blk_partition_scan(struct gendisk *disk)
{
	struct blk_pt_result *res;
	struct blk_pt_reader rd;
	struct blk_partition **made;
	char line[256];
	size_t lp = 0;
	int rc, in_logical = 0, nmade = 0;

	if (!disk)
		return -EINVAL;
	blk_might_sleep();
	if ((disk->flags & GENHD_FL_NO_PARTS) || disk->kind == BLK_DISK_SR)
		return 0;
	if (!disk->capacity || !disk->bdev.ready)
		return 0;
	res = (struct blk_pt_result *)kalloc(sizeof(*res));
	if (!res)
		return -ENOMEM;
	made = (struct blk_partition **)kcalloc(DISK_MAX_PARTS, sizeof(*made));
	if (!made) {
		kfree(res);
		return -ENOMEM;
	}
	/* 1. Read and parse the table: I/O, no registry mutex. */
	rd.read = pt_disk_read;
	rd.ctx = disk;
	rd.sector_size = disk->bdev.sector_size;
	rd.nr_sectors = disk->capacity;
	rc = blk_parse_partitions(&rd, res);

	/* 2. Register what was found. */
	blk_mutex_lock(&g_disk_registry_mutex);
	if (disk->flags & GENHD_FL_DEAD) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		kfree(made);
		kfree(res);
		return -ENODEV;
	}
	if (rc) {
		disk->pt_type = BLK_PT_NONE;
		blk_mutex_unlock(&g_disk_registry_mutex);
		if (rc == -ENOENT)
			kprintf(BLK_LOG_PREFIX "%s: no partition table\n",
				disk->disk_name);
		else
			kprintf(BLK_LOG_PREFIX "%s: partition table unusable (%d)\n",
				disk->disk_name, rc);
		kfree(made);
		kfree(res);
		return (rc == -ENOENT || rc == -EINVAL) ? 0 : rc;
	}
	if (disk->nr_parts) {
		/* A concurrent scan got here first. */
		blk_mutex_unlock(&g_disk_registry_mutex);
		kfree(made);
		kfree(res);
		return 0;
	}
	disk->pt_type = res->type;
	disk->disk_guid = res->disk_guid;
	disk->mbr_signature = res->mbr_signature;
	line[0] = 0;
	for (int i = 0; i < res->nr; i++) {
		struct blk_partition *p =
			blk_partition_create_locked(disk, &res->e[i]);

		if (!p)
			continue;
		/* held for the identity probe below */
		blk_bdev_get(&p->bdev);
		made[nmade++] = p;
		if ((p->flags & PART_FL_LOGICAL) && !in_logical) {
			in_logical = 1;
			lp += (size_t)ksnprintf(line + lp, sizeof(line) - lp, " <");
		}
		if (lp < sizeof(line))
			lp += (size_t)ksnprintf(line + lp, sizeof(line) - lp,
						" %s", p->dev_name);
		if (lp >= sizeof(line))
			lp = sizeof(line) - 1;
	}
	if (in_logical && lp < sizeof(line) - 3)
		lp += (size_t)ksnprintf(line + lp, sizeof(line) - lp, " >");
	kprintf(BLK_LOG_PREFIX "%s: %s%s%s\n", disk->disk_name,
		blk_pt_type_name(disk->pt_type), line[0] ? ":" : " (empty)",
		line);
	blk_mutex_unlock(&g_disk_registry_mutex);

	/* 3. Each partition's filesystem identity: I/O again, unlocked. */
	for (int i = 0; i < nmade; i++) {
		genhd_refresh_identity(&made[i]->bdev);
		blk_bdev_put(&made[i]->bdev);
	}
	kfree(made);
	kfree(res);
	return 0;
}

int blk_partition_rescan(struct gendisk *disk)
{
	int rc;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (disk->flags & GENHD_FL_DEAD) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return -ENODEV;
	}
	if ((disk->flags & GENHD_FL_NO_PARTS) ||
	    disk->kind == BLK_DISK_SR) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return -EINVAL;
	}
	/* Refused while any partition is open or mounted (its device would
	 * vanish under its users), and while the whole disk is held
	 * exclusively -- mounted without a table, or claimed with O_EXCL. */
	if (disk->bdev.holder &&
	    !blkdev_holder_is_node_locked(disk->bdev.holder)) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return -EBUSY;
	}
	rc = blk_partition_remove_all_locked(disk, 0);
	if (rc) {
		blk_mutex_unlock(&g_disk_registry_mutex);
		return rc;
	}
	disk->pt_type = BLK_PT_NONE;
	disk->diskseq = genhd_next_diskseq();
	disk->bdev.diskseq = disk->diskseq;
	disk_get(disk);
	blk_mutex_unlock(&g_disk_registry_mutex);

	/* The new table is read without the registry mutex. */
	rc = blk_partition_scan(disk);
	/* The whole disk's identity may have changed with the table. */
	genhd_refresh_identity(&disk->bdev);
	disk_put(disk);
	return rc;
}

/* Does [start, start+n) collide with a partition other than `skip'? */
static int pt_range_busy_locked(struct gendisk *disk, uint64_t start,
				uint64_t n, int skip)
{
	for (int i = 1; i <= DISK_MAX_PARTS; i++) {
		struct blk_partition *p = disk->part[i];

		if (!p || i == skip || (p->flags & PART_FL_EXTENDED))
			continue;
		if (start < p->start_lba + p->nr_sectors &&
		    p->start_lba < start + n)
			return 1;
	}
	return 0;
}

int blk_partition_add(struct gendisk *disk, int partno, uint64_t start_lba,
		      uint64_t nr_sectors)
{
	struct blk_pt_entry e;
	struct blk_partition *p;
	int rc = 0;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	if (partno < 1 || partno > DISK_MAX_PARTS)
		return -EINVAL;
	if (!nr_sectors || start_lba + nr_sectors < start_lba)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	if (disk->flags & GENHD_FL_DEAD) {
		rc = -ENODEV;
		goto out;
	}
	if ((disk->flags & GENHD_FL_NO_PARTS) || disk->kind == BLK_DISK_SR) {
		rc = -EINVAL;
		goto out;
	}
	if (start_lba + nr_sectors > disk->capacity) {
		rc = -EINVAL;
		goto out;
	}
	if (disk->part[partno] ||
	    pt_range_busy_locked(disk, start_lba, nr_sectors, 0)) {
		rc = -EBUSY;
		goto out;
	}
	mm_memset(&e, 0, sizeof(e));
	e.partno = partno;
	e.flags = PART_FL_FROM_BLKPG;
	e.start_lba = start_lba;
	e.nr_sectors = nr_sectors;
	p = blk_partition_create_locked(disk, &e);
	if (!p) {
		rc = -ENOMEM;
		goto out;
	}
	kprintf(BLK_LOG_PREFIX "%s: partition %s added (%llu+%llu)\n",
		disk->disk_name, p->dev_name, (unsigned long long)start_lba,
		(unsigned long long)nr_sectors);
	blk_bdev_get(&p->bdev);
	blk_mutex_unlock(&g_disk_registry_mutex);
	genhd_refresh_identity(&p->bdev);
	blk_bdev_put(&p->bdev);
	return 0;
out:
	blk_mutex_unlock(&g_disk_registry_mutex);
	return rc;
}

int blk_partition_del(struct gendisk *disk, int partno)
{
	struct blk_partition *p;
	int rc = 0;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	if (partno < 1 || partno > DISK_MAX_PARTS)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	p = disk->part[partno];
	if (!p) {
		rc = -ENXIO;
		goto out;
	}
	if (blk_partition_busy_locked(disk, p)) {
		rc = -EBUSY;
		goto out;
	}
	kprintf(BLK_LOG_PREFIX "%s: partition %s deleted\n", disk->disk_name,
		p->dev_name);
	blk_partition_remove_locked(p);
out:
	blk_mutex_unlock(&g_disk_registry_mutex);
	return rc;
}

int blk_partition_resize(struct gendisk *disk, int partno, uint64_t nr_sectors)
{
	struct blk_partition *p;
	int rc = 0;

	if (!disk || disk->magic != GENDISK_MAGIC)
		return -EINVAL;
	if (partno < 1 || partno > DISK_MAX_PARTS || !nr_sectors)
		return -EINVAL;
	blk_might_sleep();
	blk_mutex_lock(&g_disk_registry_mutex);
	p = disk->part[partno];
	if (!p) {
		rc = -ENXIO;
		goto out;
	}
	if (p->start_lba + nr_sectors > disk->capacity ||
	    p->start_lba + nr_sectors < p->start_lba) {
		rc = -EINVAL;
		goto out;
	}
	if (pt_range_busy_locked(disk, p->start_lba, nr_sectors, partno)) {
		rc = -EBUSY;
		goto out;
	}
	kprintf(BLK_LOG_PREFIX "%s: partition %s resized %llu -> %llu sectors\n",
		disk->disk_name, p->dev_name, (unsigned long long)p->nr_sectors,
		(unsigned long long)nr_sectors);
	p->nr_sectors = nr_sectors;
	p->bdev.total_sectors = (unsigned long)nr_sectors;
out:
	blk_mutex_unlock(&g_disk_registry_mutex);
	return rc;
}

/* ==========================================================================
 * Filesystem identity: what /dev/disk/by-uuid and by-label are made of
 * ========================================================================== */

/* Bytes read from the start of a device for the probe: enough for the
 * ISO 9660 primary volume descriptor at 32 KiB and the swap header at the
 * end of the first 4 KiB page. */
#define FSID_PROBE_BYTES (36 * 1024)

static void fsid_copy_trim(char *dst, size_t cap, const uint8_t *src,
			   size_t n)
{
	size_t len = 0;

	/* stop at NUL, trim trailing blanks */
	while (len < n && src[len])
		len++;
	while (len && (src[len - 1] == ' ' || src[len - 1] == 0))
		len--;
	if (len >= cap)
		len = cap - 1;
	for (size_t i = 0; i < len; i++)
		dst[i] = (char)src[i];
	dst[len] = 0;
}

static void fsid_set_type(struct blk_fs_identity *id, const char *t)
{
	size_t k = 0;

	while (t[k] && k < sizeof(id->type) - 1) {
		id->type[k] = t[k];
		k++;
	}
	id->type[k] = 0;
}

/* 16 raw bytes as a canonical UUID text (big-endian byte order, which is
 * how ext*, swap and most others store theirs). */
static void fsid_uuid16(char *out, const uint8_t *u)
{
	int o = 0;

	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[o++] = '-';
		out[o++] = g_hex[u[i] >> 4];
		out[o++] = g_hex[u[i] & 15];
	}
	out[o] = 0;
}

static int fsid_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

/* ext2/3/4: superblock at byte 1024. */
static int fsid_ext(const uint8_t *b, size_t len, struct blk_fs_identity *id)
{
	const uint8_t *sb = b + 1024;
	uint32_t compat, incompat, ro_compat;

	if (len < 2048 || pt_le16(sb + 56) != 0xEF53)
		return 0;
	compat = pt_le32(sb + 92);
	incompat = pt_le32(sb + 96);
	ro_compat = pt_le32(sb + 100);
	if (incompat & 0x0008) /* JOURNAL_DEV: an external journal */
		fsid_set_type(id, "jbd");
	else if ((incompat & (0x0040 | 0x0080 | 0x0200 | 0x0100 | 0x0400 |
			      0x1000 | 0x8000 | 0x10000)) ||
		 (ro_compat & (0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0400)))
		fsid_set_type(id, "ext4");
	else if (compat & 0x0004) /* HAS_JOURNAL */
		fsid_set_type(id, "ext3");
	else
		fsid_set_type(id, "ext2");
	if (!fsid_zero(sb + 104, 16))
		fsid_uuid16(id->uuid, sb + 104);
	fsid_copy_trim(id->label, sizeof(id->label), sb + 120, 16);
	return 1;
}

/* The swap area signature ("SWAPSPACE2") at the end of a 4 KiB page. */
static int fsid_swap(const uint8_t *b, size_t len, struct blk_fs_identity *id)
{
	if (len < 4096)
		return 0;
	if (!pt_memeq(b + 4086, "SWAPSPACE2", 10) &&
	    !pt_memeq(b + 4086, "SWAP-SPACE", 10))
		return 0;
	fsid_set_type(id, "swap");
	if (!fsid_zero(b + 1036, 16))
		fsid_uuid16(id->uuid, b + 1036);
	fsid_copy_trim(id->label, sizeof(id->label), b + 1052, 16);
	return 1;
}

/* ISO 9660 primary volume descriptor at 32 KiB. */
static int fsid_iso9660(const uint8_t *b, size_t len,
			struct blk_fs_identity *id)
{
	const uint8_t *pvd = b + 32768;
	const uint8_t *d;
	int ok = 1;

	if (len < 32768 + 2048 || pvd[0] != 1 || !pt_memeq(pvd + 1, "CD001", 5))
		return 0;
	fsid_set_type(id, "iso9660");
	fsid_copy_trim(id->label, sizeof(id->label), pvd + 40, 32);
	/* The UUID is the volume creation time "YYYY-MM-DD-HH-MM-SS-cc". */
	d = pvd + 813;
	for (int i = 0; i < 16; i++)
		if (d[i] < '0' || d[i] > '9')
			ok = 0;
	if (ok && !pt_memeq(d, "0000000000000000", 16))
		ksnprintf(id->uuid, sizeof(id->uuid),
			  "%c%c%c%c-%c%c-%c%c-%c%c-%c%c-%c%c-%c%c", d[0], d[1],
			  d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
			  d[11], d[12], d[13], d[14], d[15]);
	return 1;
}

/* NTFS: OEM id "NTFS    ", 64-bit volume serial at 0x48. */
static int fsid_ntfs(const uint8_t *b, size_t len, struct blk_fs_identity *id)
{
	if (len < 512 || !pt_memeq(b + 3, "NTFS    ", 8))
		return 0;
	fsid_set_type(id, "ntfs");
	ksnprintf(id->uuid, sizeof(id->uuid), "%016llX",
		  (unsigned long long)pt_le64(b + 0x48));
	return 1;
}

/* exFAT: OEM id "EXFAT   ", serial at 100; the label is a root directory
 * entry of type 0x83, read from the root directory's first cluster. */
static int fsid_exfat(block_device_t *bdev, const uint8_t *b, size_t len,
		      struct blk_fs_identity *id)
{
	uint32_t serial, heap_off, root_cluster;
	uint8_t bps_shift, spc_shift;
	uint64_t cluster_bytes, root_byte;

	if (len < 512 || !pt_memeq(b + 3, "EXFAT   ", 8))
		return 0;
	fsid_set_type(id, "exfat");
	serial = pt_le32(b + 100);
	ksnprintf(id->uuid, sizeof(id->uuid), "%04X-%04X", serial >> 16,
		  serial & 0xFFFF);
	heap_off = pt_le32(b + 88);
	root_cluster = pt_le32(b + 96);
	bps_shift = b[108];
	spc_shift = b[109];
	if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift ||
	    root_cluster < 2)
		return 1;
	cluster_bytes = 1ULL << (bps_shift + spc_shift);
	if (cluster_bytes > 64 * 1024)
		cluster_bytes = 64 * 1024; /* the label is near the start */
	root_byte = ((uint64_t)heap_off << bps_shift) +
		    (uint64_t)(root_cluster - 2) * (1ULL << (bps_shift + spc_shift));
	{
		uint32_t ss = bdev->sector_size;
		uint64_t lba = root_byte / ss;
		uint32_t n = (uint32_t)((cluster_bytes + ss - 1) / ss);
		uint8_t *dir;

		if (root_byte % ss || lba + n > bdev->total_sectors)
			return 1;
		dir = (uint8_t *)kalloc((size_t)n * ss);
		if (!dir)
			return 1;
		if (blk_read_sync_flags(bdev, lba, n, dir, BLK_REQ_QUIET) == 0) {
			for (uint64_t off = 0; off + 32 <= cluster_bytes;
			     off += 32) {
				const uint8_t *e = dir + off;
				if (e[0] == 0x00)
					break;
				if (e[0] == 0x83) {
					int chars = e[1] > 11 ? 11 : e[1];
					pt_utf16le_to_utf8(e + 2, chars, id->label,
							   sizeof(id->label));
					break;
				}
			}
		}
		kfree(dir);
	}
	return 1;
}

/* FAT12/16/32: a BIOS parameter block that makes sense.  The label of the
 * root directory's volume entry wins over the boot sector's copy (it is
 * the one tools change). */
static int fsid_fat(block_device_t *bdev, const uint8_t *b, size_t len,
		    struct blk_fs_identity *id)
{
	uint16_t bps, reserved, root_entries, fatsz16, tot16;
	uint8_t spc, nfats, media;
	uint32_t fatsz, tot, root_dir_sectors, first_data, root_cluster = 0;
	int fat32;
	const uint8_t *ext;
	uint32_t serial;
	char bslabel[16];

	if (len < 512)
		return 0;
	if (!((b[0] == 0xEB && b[2] == 0x90) || b[0] == 0xE9))
		return 0;
	bps = pt_le16(b + 11);
	spc = b[13];
	reserved = pt_le16(b + 14);
	nfats = b[16];
	root_entries = pt_le16(b + 17);
	tot16 = pt_le16(b + 19);
	media = b[21];
	fatsz16 = pt_le16(b + 22);
	if (bps < 512 || bps > 4096 || (bps & (bps - 1)))
		return 0;
	if (!spc || (spc & (spc - 1)) || !reserved || !nfats || nfats > 4)
		return 0;
	if (media != 0xF0 && media < 0xF8)
		return 0;
	fat32 = fatsz16 == 0 && root_entries == 0;
	fatsz = fat32 ? pt_le32(b + 36) : fatsz16;
	tot = tot16 ? tot16 : pt_le32(b + 32);
	if (!fatsz || !tot)
		return 0;
	ext = b + (fat32 ? 64 : 36); /* the extended boot signature block */
	if (ext[2] != 0x29 && ext[2] != 0x28)
		return 0;
	fsid_set_type(id, "vfat");
	serial = pt_le32(ext + 3);
	ksnprintf(id->uuid, sizeof(id->uuid), "%04X-%04X", serial >> 16,
		  serial & 0xFFFF);
	fsid_copy_trim(bslabel, sizeof(bslabel), ext + 7, 11);
	if (kstrcmp(bslabel, "NO NAME") != 0)
		ksnprintf(id->label, sizeof(id->label), "%s", bslabel);

	/* the root directory (FAT12/16: fixed area; FAT32: its first
	 * cluster) */
	root_dir_sectors = ((uint32_t)root_entries * 32 + bps - 1) / bps;
	first_data = reserved + nfats * fatsz + root_dir_sectors;
	{
		uint64_t byte_off, bytes;
		uint32_t ss = bdev->sector_size;

		if (fat32) {
			root_cluster = pt_le32(b + 44);
			if (root_cluster < 2)
				return 1;
			byte_off = ((uint64_t)first_data +
				    (uint64_t)(root_cluster - 2) * spc) * bps;
			bytes = (uint64_t)spc * bps;
		} else {
			byte_off = ((uint64_t)reserved + (uint64_t)nfats * fatsz) *
				   bps;
			bytes = (uint64_t)root_dir_sectors * bps;
		}
		if (bytes > 64 * 1024)
			bytes = 64 * 1024;
		if (!bytes || byte_off % ss)
			return 1;
		{
			uint64_t lba = byte_off / ss;
			uint32_t n = (uint32_t)((bytes + ss - 1) / ss);
			uint8_t *dir;

			if (lba + n > bdev->total_sectors)
				return 1;
			dir = (uint8_t *)kalloc((size_t)n * ss);
			if (!dir)
				return 1;
			if (blk_read_sync_flags(bdev, lba, n, dir,
						BLK_REQ_QUIET) == 0) {
				for (uint64_t off = 0; off + 32 <= bytes;
				     off += 32) {
					const uint8_t *e = dir + off;
					if (e[0] == 0x00)
						break;
					if (e[0] == 0xE5)
						continue;
					/* volume label: attribute 0x08 alone
					 * (0x0F is a long-name entry) */
					if ((e[11] & 0x3F) == 0x08) {
						char l[16];
						fsid_copy_trim(l, sizeof(l), e,
							       11);
						if (l[0])
							ksnprintf(id->label,
								  sizeof(id->label),
								  "%s", l);
						break;
					}
				}
			}
			kfree(dir);
		}
	}
	return 1;
}

int blk_probe_fs_identity(block_device_t *bdev, struct blk_fs_identity *out)
{
	uint8_t *buf;
	uint32_t ss, n;
	uint64_t want;
	size_t len;
	int rc, found = 0;

	if (!bdev || !out)
		return -EINVAL;
	mm_memset(out, 0, sizeof(*out));
	blk_might_sleep();
	ss = bdev->sector_size ? bdev->sector_size : 512;
	if (!bdev->total_sectors)
		return -ENOENT;
	want = FSID_PROBE_BYTES;
	if ((uint64_t)bdev->total_sectors * ss < want)
		want = (uint64_t)bdev->total_sectors * ss;
	n = (uint32_t)((want + ss - 1) / ss);
	if (!n)
		return -ENOENT;
	buf = (uint8_t *)kcalloc(n, ss);
	if (!buf)
		return -ENOMEM;
	rc = blk_read_sync_flags(bdev, 0, n, buf, BLK_REQ_QUIET | BLK_REQ_FAILFAST);
	if (rc) {
		kfree(buf);
		return rc == -ENOMEDIUM ? -ENOENT : -EIO;
	}
	len = (size_t)n * ss;
	/* Order matters where signatures can coexist: an ISO hybrid image
	 * carries an MBR and a FAT-looking boot sector as well; swap and ext
	 * leave the first sector alone. */
	if (fsid_iso9660(buf, len, out))
		found = 1;
	else if (fsid_swap(buf, len, out))
		found = 1;
	else if (fsid_ext(buf, len, out))
		found = 1;
	else if (fsid_exfat(bdev, buf, len, out))
		found = 1;
	else if (fsid_ntfs(buf, len, out))
		found = 1;
	else if (fsid_fat(bdev, buf, len, out))
		found = 1;
	kfree(buf);
	if (!found) {
		mm_memset(out, 0, sizeof(*out));
		return -ENOENT;
	}
	out->valid = 1;
	return 0;
}

#endif /* !BLK_HOST_TEST */
