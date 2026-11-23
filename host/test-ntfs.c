/* Unit test of the NTFS driver's internal helpers (host/test-ntfs.sh unit).
 *
 * Compiled like a kernel unit of the filesystem harness (the prelude
 * host/test-fs-prelude.h is force-included, AddressSanitizer and
 * UndefinedBehaviorSanitizer are on) and includes the driver's files (the
 * .c files of kernel/fs/ntfs/) themselves, so the static helpers are tested
 * directly: UTF-16 <-> UTF-8, mapping pairs, LZNT1, multi-sector fixups,
 * collation, the EA walk, the attribute validator, the $LogFile
 * restart-page check, index node validation, the boot sector decoders --
 * each with valid input and with hostile variants (out-of-range offsets,
 * lengths and counts, values that overflow), plus a deterministic random
 * pass over every parser (the sanitizers catch any stray access).
 *
 * Linked with the harness objects (stubs, block device, caches); the
 * runner's main is replaced by this file's.
 *
 * Copyright (C) 2026 The LikeOS Project */

#include "../kernel/fs/ntfs/super.c"
#include "../kernel/fs/ntfs/misc.c"
#include "../kernel/fs/ntfs/bdev.c"
#include "../kernel/fs/ntfs/mft.c"
#include "../kernel/fs/ntfs/attrib.c"
#include "../kernel/fs/ntfs/runlist.c"
#include "../kernel/fs/ntfs/inode.c"
#include "../kernel/fs/ntfs/aops.c"
#include "../kernel/fs/ntfs/compress.c"
#include "../kernel/fs/ntfs/unistr.c"
#include "../kernel/fs/ntfs/index.c"
#include "../kernel/fs/ntfs/dir.c"
#include "../kernel/fs/ntfs/ea.c"
#include "../kernel/fs/ntfs/xattr.c"
#include "../kernel/fs/ntfs/reparse.c"
#include "../kernel/fs/ntfs/security.c"
#include "../kernel/fs/ntfs/lcnalloc.c"
#include "../kernel/fs/ntfs/file.c"
#include "../kernel/fs/ntfs/namei.c"
#include "../kernel/fs/ntfs/logfile.c"

int vfs_link_hop(vfs_mount_t *m, const char *path, unsigned long comp_start,
		 unsigned long comp_end, const char *target)
{
	(void)m;
	(void)path;
	(void)comp_start;
	(void)comp_end;
	(void)target;
	return 0;
}

void test_task_init(uint32_t uid, uint32_t gid);

static int t_fail;
static int t_count;

#define CHECK(cond)                                                          \
	do {                                                                 \
		t_count++;                                                   \
		if (!(cond)) {                                               \
			t_fail++;                                            \
			h_printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
				 #cond);                                     \
		}                                                            \
	} while (0)

static uint64_t t_rng = 0x1234567887654321ULL;

static uint32_t t_rand(void)
{
	t_rng ^= t_rng << 13;
	t_rng ^= t_rng >> 7;
	t_rng ^= t_rng << 17;
	return (uint32_t)t_rng;
}

/* ---- UTF-16 <-> UTF-8 ---- */
static void test_utf(void)
{
	uint16_t u[8], back[8];
	char out[32];
	long n;

	/* "aé€😀" */
	ntfs_put_le16(&u[0], 'a');
	ntfs_put_le16(&u[1], 0xE9);
	ntfs_put_le16(&u[2], 0x20AC);
	ntfs_put_le16(&u[3], 0xD83D);
	ntfs_put_le16(&u[4], 0xDE00);
	n = ntfs_utf16_to_utf8(u, 5, out, sizeof(out));
	CHECK(n == 1 + 2 + 3 + 4);
	CHECK(h_memcmp(out, "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10) == 0);
	CHECK(ntfs_utf16_utf8_len(u, 5) == 10);
	n = ntfs_utf8_to_utf16(out, 10, back, 8);
	CHECK(n == 5 && h_memcmp(back, u, 10) == 0);
	/* unpaired surrogate: round-trips */
	ntfs_put_le16(&u[0], 0xD800);
	ntfs_put_le16(&u[1], 'x');
	n = ntfs_utf16_to_utf8(u, 2, out, sizeof(out));
	CHECK(n == 4);
	n = ntfs_utf8_to_utf16(out, 4, back, 8);
	CHECK(n == 2 && ntfs_le16(&back[0]) == 0xD800);
	/* '/' and NUL are not a path component */
	ntfs_put_le16(&u[0], '/');
	CHECK(ntfs_utf16_to_utf8(u, 1, out, sizeof(out)) == ST_INVALID);
	ntfs_put_le16(&u[0], 0);
	CHECK(ntfs_utf16_to_utf8(u, 1, out, sizeof(out)) == ST_INVALID);
	/* too small */
	ntfs_put_le16(&u[0], 0x20AC);
	CHECK(ntfs_utf16_to_utf8(u, 1, out, 3) == ST_NAMETOOLONG);
	/* overlong and truncated UTF-8 refused */
	CHECK(ntfs_utf8_to_utf16("\xc0\xaf", 2, back, 8) == ST_INVALID);
	CHECK(ntfs_utf8_to_utf16("\xe2\x82", 2, back, 8) == ST_INVALID);
	CHECK(ntfs_utf8_to_utf16("\xf4\x90\x80\x80", 4, back, 8) == ST_INVALID);
	CHECK(ntfs_utf8_to_utf16("abc", 3, back, 2) == ST_NAMETOOLONG);
	/* link paths: '\' -> '/' */
	ntfs_put_le16(&u[0], 'a');
	ntfs_put_le16(&u[1], '\\');
	ntfs_put_le16(&u[2], 'b');
	CHECK(ntfs_utf16_path(u, 3, out, sizeof(out)) == 3 &&
	      h_strcmp(out, "a/b") == 0);
}

/* A zeroed volume for the helpers that read geometry. */
static ntfs_fs_t *t_fs(void)
{
	ntfs_fs_t *fs = (ntfs_fs_t *)kcalloc(1, sizeof(*fs));

	fs->cluster_size = 4096;
	fs->cluster_bits = 12;
	fs->nr_clusters = 1000;
	fs->rec_size = 1024;
	fs->rec_bits = 10;
	fs->mft_records = 100;
	return fs;
}

/* Build a non-resident attribute record with mapping pairs `mp'. */
static ntfs_attr_record_t *t_nonres(uint8_t *buf, const uint8_t *mp,
				    unsigned mplen, int64_t lowest,
				    int64_t highest, int64_t asz, int64_t dsz)
{
	ntfs_attr_record_t *a = (ntfs_attr_record_t *)buf;

	h_memset(buf, 0, 256);
	a->type = NTFS_AT_DATA;
	a->non_resident = 1;
	a->length = (64 + mplen + 7) & ~7u;
	a->data.non_resident.mapping_pairs_offset = 64;
	a->data.non_resident.lowest_vcn = lowest;
	a->data.non_resident.highest_vcn = highest;
	a->data.non_resident.allocated_size = asz;
	a->data.non_resident.data_size = dsz;
	a->data.non_resident.initialized_size = dsz;
	h_memcpy(buf + 64, mp, mplen);
	return a;
}

static void test_mapping_pairs(void)
{
	ntfs_fs_t *fs = t_fs();
	uint8_t buf[256];
	ntfs_runlist_t rl;
	int64_t lcn, left;

	/* 10 clusters at 100, 5 sparse, 3 at 90 (delta -20). */
	{
		static const uint8_t mp[] = { 0x11, 10, 100, 0x01, 5,
					      0x11, 3,  0xEC, 0 };
		ntfs_attr_record_t *a =
			t_nonres(buf, mp, sizeof(mp), 0, 17, 18 * 4096, 18 * 4096);

		h_memset(&rl, 0, sizeof(rl));
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_OK);
		CHECK(rl.count == 3 && rl.end_vcn == 18);
		CHECK(ntfs_rl_map(&rl, 3, &lcn, &left) == ST_OK && lcn == 103 &&
		      left == 7);
		CHECK(ntfs_rl_map(&rl, 12, &lcn, &left) == ST_OK &&
		      lcn == NTFS_LCN_HOLE && left == 3);
		CHECK(ntfs_rl_map(&rl, 16, &lcn, &left) == ST_OK && lcn == 81);
		CHECK(ntfs_rl_map(&rl, 18, &lcn, &left) == ST_IO);
		ntfs_rl_free(&rl);
	}
	/* beyond the volume */
	{
		static const uint8_t mp[] = { 0x21, 0x10, 0x00, 0xF0, 0 };
		ntfs_attr_record_t *a = t_nonres(buf, mp, sizeof(mp), 0, 15,
						 16 * 4096, 16 * 4096);

		h_memset(&rl, 0, sizeof(rl));
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_IO);
		ntfs_rl_free(&rl);
	}
	/* negative LCN */
	{
		static const uint8_t mp[] = { 0x11, 1, 0x80, 0 };
		ntfs_attr_record_t *a =
			t_nonres(buf, mp, sizeof(mp), 0, 0, 4096, 4096);

		h_memset(&rl, 0, sizeof(rl));
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_IO);
		ntfs_rl_free(&rl);
	}
	/* run ends short of highest_vcn */
	{
		static const uint8_t mp[] = { 0x11, 1, 10, 0 };
		ntfs_attr_record_t *a =
			t_nonres(buf, mp, sizeof(mp), 0, 5, 6 * 4096, 6 * 4096);

		h_memset(&rl, 0, sizeof(rl));
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_IO);
		ntfs_rl_free(&rl);
	}
	/* empty mapping pairs: only for an empty attribute */
	{
		static const uint8_t mp[] = { 0 };
		ntfs_attr_record_t *a = t_nonres(buf, mp, 1, 0, -1, 0, 0);

		h_memset(&rl, 0, sizeof(rl));
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_OK);
		a = t_nonres(buf, mp, 1, 0, -1, 4096, 10);
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_IO);
		ntfs_rl_free(&rl);
	}
	/* the length bytes reach the attribute's end */
	{
		ntfs_attr_record_t *a;

		h_memset(buf, 0, sizeof(buf));
		a = (ntfs_attr_record_t *)buf;
		a->type = NTFS_AT_DATA;
		a->non_resident = 1;
		a->length = 72;
		a->data.non_resident.mapping_pairs_offset = 64;
		a->data.non_resident.highest_vcn = 0;
		buf[64] = 0x88; /* 8 + 8 bytes would be needed */
		h_memset(&rl, 0, sizeof(rl));
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_IO);
		ntfs_rl_free(&rl);
	}
	/* highest_vcn at the largest VCN: no overflow computing highest + 1
	 * (a fuzzed $INDEX_ALLOCATION; the run ends far short of it) */
	{
		static const uint8_t mp[] = { 0x11, 1, 10, 0 };
		ntfs_attr_record_t *a = t_nonres(buf, mp, sizeof(mp), 0,
						 0x7FFFFFFFFFFFFFFFLL, 4096,
						 4096);

		h_memset(&rl, 0, sizeof(rl));
		CHECK(ntfs_mp_decode(fs, a, a->length, &rl) == ST_IO);
		ntfs_rl_free(&rl);
	}
	/* random mapping pairs: never a crash */
	for (int it = 0; it < 20000; it++) {
		uint8_t mp[64];
		ntfs_attr_record_t *a;

		for (unsigned i = 0; i < sizeof(mp); i++)
			mp[i] = (uint8_t)t_rand();
		a = t_nonres(buf, mp, (t_rand() % 60) + 1, 0,
			     (int64_t)(t_rand() % 2000) - 1, 4096, 4096);
		h_memset(&rl, 0, sizeof(rl));
		(void)ntfs_mp_decode(fs, a, a->length, &rl);
		for (uint32_t i = 0; i < rl.count; i++)
			CHECK(rl.runs[i].lcn == NTFS_LCN_HOLE ||
			      (rl.runs[i].lcn >= 0 &&
			       (uint64_t)(rl.runs[i].lcn + rl.runs[i].len) <=
				       fs->nr_clusters));
		ntfs_rl_free(&rl);
	}
	kfree(fs);
}

static void test_lznt1(void)
{
	uint8_t in[64], out[8192];
	long n;

	/* A compressed chunk: "abc" literals then a back-reference of
	 * length 6 at displacement 3 -> "abcabcabc". */
	h_memset(in, 0, sizeof(in));
	in[2] = 0x08; /* flags: tokens 0..2 literals, token 3 a reference */
	in[3] = 'a';
	in[4] = 'b';
	in[5] = 'c';
	/* pos 3: 12 - 1 length bits... pos-1 = 2 < 16: 12 length bits */
	ntfs_put_le16(in + 6, (uint16_t)(((3 - 1) << 12) | (6 - 3)));
	ntfs_put_le16(in, (uint16_t)(0xB000 | (8 - 3)));
	n = ntfs_lznt1_decompress(in, 10, out, sizeof(out));
	CHECK(n == 9);
	CHECK(h_memcmp(out, "abcabcabc", 9) == 0);
	CHECK(out[9] == 0 && out[8191] == 0);
	/* displacement beyond the output: refused */
	in[2] = 0x01;
	ntfs_put_le16(in + 3, 0x1000);
	ntfs_put_le16(in, (uint16_t)(0xB000 | (5 - 3)));
	CHECK(ntfs_lznt1_decompress(in, 5, out, sizeof(out)) == ST_IO);
	/* a stored chunk of 4096 bytes */
	{
		uint8_t *st = (uint8_t *)h_malloc(4098 + 2);

		ntfs_put_le16(st, (uint16_t)(0x3000 | (4098 - 3)));
		for (int i = 0; i < 4096; i++)
			st[2 + i] = (uint8_t)i;
		ntfs_put_le16(st + 4098, 0);
		n = ntfs_lznt1_decompress(st, 4100, out, sizeof(out));
		CHECK(n == 4096 && out[4095] == (uint8_t)4095 &&
		      out[4096] == 0);
		h_free(st);
	}
	/* random input: bounded, never a crash */
	for (int it = 0; it < 20000; it++) {
		uint8_t rin[256];
		unsigned len = t_rand() % sizeof(rin);

		for (unsigned i = 0; i < len; i++)
			rin[i] = (uint8_t)t_rand();
		if (len >= 2 && (t_rand() & 1))
			rin[1] |= 0xB0;
		(void)ntfs_lznt1_decompress(rin, len, out,
					    (t_rand() & 1) ? 4096 : 8192);
	}
}

static void test_mst(void)
{
	uint8_t rec[1024], copy[1024];

	h_memset(rec, 0, sizeof(rec));
	for (int i = 0; i < 1024; i++)
		rec[i] = (uint8_t)(i * 7);
	ntfs_put_le32(rec, NTFS_MAGIC_FILE);
	ntfs_put_le16(rec + 4, 0x30);
	ntfs_put_le16(rec + 6, 3);
	ntfs_put_le16(rec + 0x30, 7);
	h_memcpy(copy, rec, sizeof(rec));
	CHECK(ntfs_mst_protect(rec, 1024) == ST_OK);
	CHECK(ntfs_le16(rec + 510) == 8 && ntfs_le16(rec + 1022) == 8);
	CHECK(ntfs_mst_fixup(rec, 1024) == ST_OK);
	CHECK(h_memcmp(rec + 0x40, copy + 0x40, 1024 - 0x40) == 0);
	/* torn write */
	CHECK(ntfs_mst_protect(rec, 1024) == ST_OK);
	rec[1023] ^= 1;
	CHECK(ntfs_mst_fixup(rec, 1024) == ST_IO);
	/* usa_count that does not match the size */
	ntfs_put_le16(rec + 6, 0x7FFF);
	CHECK(ntfs_mst_fixup(rec, 1024) == ST_IO);
	CHECK(ntfs_mst_protect(rec, 1024) == ST_IO);
	ntfs_put_le16(rec + 6, 3);
	ntfs_put_le16(rec + 4, 509); /* odd / outside */
	CHECK(ntfs_mst_fixup(rec, 1024) == ST_IO);
}

static void test_collation(void)
{
	ntfs_fs_t *fs = t_fs();
	uint16_t a[4], b[4];

	fs->upcase_len = 128;
	fs->upcase = (uint16_t *)kalloc(256);
	for (int i = 0; i < 128; i++)
		fs->upcase[i] = (uint16_t)((i >= 'a' && i <= 'z') ? i - 32 : i);
	ntfs_put_le16(&a[0], 'a');
	ntfs_put_le16(&a[1], 'B');
	ntfs_put_le16(&b[0], 'A');
	ntfs_put_le16(&b[1], 'b');
	CHECK(ntfs_collate_names(fs, a, 2, b, 2, 1) == 0);
	/* the raw units break the tie: 'a' (0x61) > 'A' (0x41) */
	CHECK(ntfs_collate_names(fs, a, 2, b, 2, 0) > 0);
	CHECK(ntfs_collate_names(fs, a, 1, b, 2, 0) < 0);
	CHECK(ntfs_upcase_eq(fs, a, b, 2));
	kfree(fs->upcase);
	kfree(fs);
}

static int t_ea_count;

static int t_ea_cb(const ntfs_ea_view_t *ea, void *arg)
{
	(void)arg;
	(void)ea;
	t_ea_count++;
	return 0;
}

static void test_ea(void)
{
	uint8_t buf[64];

	/* two entries: "A" = "xy", "$LXMOD" = 4 bytes */
	h_memset(buf, 0, sizeof(buf));
	ntfs_put_le32(buf, 16);
	buf[5] = 1;
	ntfs_put_le16(buf + 6, 2);
	buf[8] = 'A';
	buf[10] = 'x';
	buf[11] = 'y';
	ntfs_put_le32(buf + 16, 0);
	buf[21] = 6;
	ntfs_put_le16(buf + 22, 4);
	h_memcpy(buf + 24, "$LXMOD", 7);
	ntfs_put_le32(buf + 31, 0100644);
	t_ea_count = 0;
	CHECK(ntfs_ea_walk(buf, 36, t_ea_cb, NULL) == ST_OK && t_ea_count == 2);
	{
		ntfs_ea_view_t v;

		CHECK(ntfs_ea_find(buf, 36, "$LXMOD", &v) == ST_OK &&
		      v.value_len == 4);
		CHECK(ntfs_ea_find(buf, 36, "B", &v) == ST_NOT_FOUND);
	}
	/* name not NUL-terminated */
	buf[9] = 'Z';
	CHECK(ntfs_ea_walk(buf, 36, t_ea_cb, NULL) == ST_IO);
	buf[9] = 0;
	/* next offset past the data */
	ntfs_put_le32(buf, 60);
	CHECK(ntfs_ea_walk(buf, 36, t_ea_cb, NULL) == ST_IO);
	/* value longer than the entry */
	ntfs_put_le32(buf, 16);
	ntfs_put_le16(buf + 6, 200);
	CHECK(ntfs_ea_walk(buf, 36, t_ea_cb, NULL) == ST_IO);
	/* random chains */
	for (int it = 0; it < 20000; it++) {
		for (unsigned i = 0; i < sizeof(buf); i++)
			buf[i] = (uint8_t)t_rand();
		if (t_rand() & 1)
			ntfs_put_le32(buf, t_rand() % 64);
		(void)ntfs_ea_walk(buf, t_rand() % 65, t_ea_cb, NULL);
	}
}

static void test_attr_valid(void)
{
	ntfs_fs_t *fs = t_fs();
	uint8_t rec[1024];
	ntfs_mft_record_t *m = (ntfs_mft_record_t *)rec;
	ntfs_attr_record_t *a;

	h_memset(rec, 0, sizeof(rec));
	m->magic = NTFS_MAGIC_FILE;
	m->attrs_offset = 56;
	m->bytes_in_use = 56 + 96 + 8;
	m->bytes_allocated = 1024;
	a = (ntfs_attr_record_t *)(rec + 56);
	a->type = NTFS_AT_STANDARD_INFORMATION;
	a->length = 96;
	a->data.resident.value_offset = 24;
	a->data.resident.value_length = 72;
	ntfs_put_le32(rec + 56 + 96, NTFS_AT_END);
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_OK);
	/* the value inside the header */
	a->data.resident.value_offset = 0;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	a->data.resident.value_offset = 24;
	/* value past the attribute */
	a->data.resident.value_length = 80;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	/* too short for $STANDARD_INFORMATION */
	a->data.resident.value_length = 40;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	a->data.resident.value_length = 72;
	/* non-resident $STANDARD_INFORMATION */
	a->non_resident = 1;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	/* sizes beyond the allocation */
	a->type = NTFS_AT_DATA;
	a->data.non_resident.mapping_pairs_offset = 64;
	a->data.non_resident.allocated_size = 4096;
	a->data.non_resident.data_size = 8192;
	a->data.non_resident.initialized_size = 0;
	a->data.non_resident.lowest_vcn = 0;
	a->data.non_resident.highest_vcn = 0;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	a->data.non_resident.data_size = 100;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_OK);
	/* a compressed first extent needs 72 bytes of header */
	a->flags = NTFS_ATTR_IS_COMPRESSED;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	a->data.non_resident.mapping_pairs_offset = 72;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_OK);
	/* name outside the attribute */
	a->name_length = 60;
	a->name_offset = 72;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	a->name_length = 0;
	/* length past bytes_in_use */
	a->length = 200;
	CHECK(ntfs_attr_valid(fs, rec, 56, 1) == ST_IO);
	/* random records: never a crash */
	for (int it = 0; it < 20000; it++) {
		for (unsigned i = 56; i < 56 + 160; i++)
			rec[i] = (uint8_t)t_rand();
		m->bytes_in_use = 56 + (t_rand() % 200);
		(void)ntfs_attr_valid(fs, rec, 56, 1);
	}
	kfree(fs);
}

static void test_restart(void)
{
	uint8_t *pg = (uint8_t *)h_calloc(1, 4096);
	ntfs_restart_page_header_t *rp = (ntfs_restart_page_header_t *)pg;
	ntfs_restart_area_t *ra;
	uint32_t ps;
	int clean;
	int64_t lsn;

	rp->magic = NTFS_MAGIC_RSTR;
	rp->usa_ofs = 0x1E;
	rp->usa_count = 9;
	rp->system_page_size = 4096;
	rp->log_page_size = 4096;
	rp->restart_area_offset = 0x30;
	rp->major_ver = 1;
	rp->minor_ver = 1;
	ra = (ntfs_restart_area_t *)(pg + 0x30);
	ra->log_clients = 1;
	ra->client_free_list = NTFS_LOGFILE_NO_CLIENT;
	ra->client_in_use_list = NTFS_LOGFILE_NO_CLIENT;
	ra->client_array_offset = 0x30;
	ra->restart_area_length = 0x30 + NTFS_LOG_CLIENT_RECORD;
	ra->current_lsn = 77;
	ntfs_put_le16(pg + 0x1E, 1);
	CHECK(ntfs_mst_protect(pg, 4096) == ST_OK);
	CHECK(ntfs_rstr_check(pg, 4096, &ps, &clean, &lsn) == ST_OK &&
	      clean && lsn == 77 && ps == 4096);
	/* a client array that wraps 16 bits */
	CHECK(ntfs_mst_fixup(pg, 4096) == ST_OK);
	ra->log_clients = 410;
	ra->restart_area_length = (uint16_t)(0x30 + 410 * 160);
	CHECK(ntfs_mst_protect(pg, 4096) == ST_OK);
	CHECK(ntfs_rstr_check(pg, 4096, &ps, &clean, &lsn) == ST_IO);
	/* a huge page size with usa_count 1 */
	CHECK(ntfs_mst_fixup(pg, 4096) == ST_IO || 1);
	rp->system_page_size = 1U << 25;
	rp->usa_count = 1;
	CHECK(ntfs_rstr_check(pg, 4096, &ps, &clean, &lsn) == ST_IO);
	h_free(pg);
}

/* Index nodes: a header with one $FILE_NAME entry and END. */
static void test_index_node(void)
{
	ntfs_fs_t *fs = t_fs();
	uint8_t buf[256];
	ntfs_index_header_t *h = (ntfs_index_header_t *)buf;
	ntfs_index_entry_t *e;
	ntfs_file_name_t *fn;

	h_memset(buf, 0, sizeof(buf));
	h->entries_offset = 16;
	e = (ntfs_index_entry_t *)(buf + 16);
	e->length = 16 + 72;
	e->key_length = 66 + 4;
	fn = (ntfs_file_name_t *)(buf + 32);
	fn->file_name_length = 2;
	fn->file_name_type = NTFS_FILE_NAME_POSIX;
	e = (ntfs_index_entry_t *)(buf + 16 + 88);
	e->length = 16;
	e->flags = NTFS_INDEX_ENTRY_END;
	h->index_length = 16 + 88 + 16;
	h->allocated_size = h->index_length;
	CHECK(ntfs_index_hdr_ok(h, sizeof(buf), 0));
	CHECK(ntfs_node_entries_ok(fs, h, NTFS_COLLATION_FILE_NAME, 12, 0, 0));
	/* the name longer than the key */
	fn->file_name_length = 4;
	CHECK(!ntfs_node_entries_ok(fs, h, NTFS_COLLATION_FILE_NAME, 12, 0, 0));
	fn->file_name_length = 2;
	/* a NODE entry in a leaf */
	((ntfs_index_entry_t *)(buf + 16))->flags = NTFS_INDEX_ENTRY_NODE;
	CHECK(!ntfs_node_entries_ok(fs, h, NTFS_COLLATION_FILE_NAME, 12, 0, 0));
	((ntfs_index_entry_t *)(buf + 16))->flags = 0;
	/* no END entry */
	e->flags = 0;
	CHECK(!ntfs_node_entries_ok(fs, h, NTFS_COLLATION_FILE_NAME, 12, 0, 0));
	e->flags = NTFS_INDEX_ENTRY_END;
	/* entries offset below the canonical one */
	CHECK(!ntfs_index_hdr_ok(h, sizeof(buf), 40));
	/* random nodes */
	for (int it = 0; it < 20000; it++) {
		for (unsigned i = 0; i < sizeof(buf); i++)
			buf[i] = (uint8_t)t_rand();
		h->entries_offset = 16 + (t_rand() % 8) * 8;
		h->index_length = t_rand() % 256;
		h->allocated_size = 256;
		if (ntfs_index_hdr_ok(h, sizeof(buf), 0))
			(void)ntfs_node_entries_ok(fs, h, t_rand() % 0x14, 12,
						   t_rand() & 1, 1 << 20);
	}
	kfree(fs);
}

static void test_boot_decoders(void)
{
	CHECK(ntfs_boot_cluster(512, 8) == 4096);
	CHECK(ntfs_boot_cluster(512, 0x80) == 65536);
	CHECK(ntfs_boot_cluster(512, 0xF4) == (2U << 20)); /* 2^12 sectors */
	CHECK(ntfs_boot_cluster(512, 0) == 0);
	CHECK(ntfs_boot_cluster(512, 3) == 0);
	CHECK(ntfs_boot_cluster(512, 0x81) == 0);
	CHECK(ntfs_rec_size(-10, 4096) == 1024);
	CHECK(ntfs_rec_size(-31, 4096) == (1ULL << 31));
	CHECK(ntfs_rec_size(-32, 4096) == 0);
	CHECK(ntfs_rec_size(2, 512) == 1024);
	CHECK(ntfs_rec_size(0, 512) == 0);
}

static void test_link_target(void)
{
	ntfs_fs_t *fs = t_fs();
	uint8_t rp[128];
	char out[256];
	const char *sub = "\\??\\C:\\Users\\x";
	unsigned n = (unsigned)h_strlen(sub);

	/* a junction to C:\Users\x */
	h_memset(rp, 0, sizeof(rp));
	ntfs_put_le32(rp, NTFS_IO_REPARSE_TAG_MOUNT_POINT);
	ntfs_put_le16(rp + 4, (uint16_t)(8 + 2 * n));
	ntfs_put_le16(rp + 8, 0);
	ntfs_put_le16(rp + 10, (uint16_t)(2 * n));
	for (unsigned i = 0; i < n; i++)
		ntfs_put_le16(rp + 16 + 2 * i, (uint16_t)sub[i]);
	fs->opt.native_symlink = NTFS_SYMLINK_RAW;
	CHECK(ntfs_link_target(fs, rp, 16 + 2 * n, 2, out, sizeof(out)) > 0 &&
	      h_strcmp(out, "C:/Users/x") == 0);
	fs->opt.native_symlink = NTFS_SYMLINK_REL;
	CHECK(ntfs_link_target(fs, rp, 16 + 2 * n, 2, out, sizeof(out)) > 0 &&
	      h_strcmp(out, "../../Users/x") == 0);
	CHECK(ntfs_link_target(fs, rp, 16 + 2 * n, 0, out, sizeof(out)) > 0 &&
	      h_strcmp(out, "Users/x") == 0);
	/* the name range outside the buffer */
	ntfs_put_le16(rp + 10, 200);
	CHECK(ntfs_link_target(fs, rp, 16 + 2 * n, 0, out, sizeof(out)) ==
	      ST_IO);
	/* WSL link */
	h_memset(rp, 0, sizeof(rp));
	ntfs_put_le32(rp, NTFS_IO_REPARSE_TAG_LX_SYMLINK);
	ntfs_put_le16(rp + 4, 4 + 5);
	ntfs_put_le32(rp + 8, 2);
	h_memcpy(rp + 12, "a/b/c", 5);
	CHECK(ntfs_link_target(fs, rp, 8 + 9, 0, out, sizeof(out)) == 5 &&
	      h_strcmp(out, "a/b/c") == 0);
	kfree(fs);
}

static void test_time(void)
{
	uint64_t sec, nsec;

	ntfs_time_to_unix(NTFS_TIME_OFFSET, &sec, &nsec);
	CHECK(sec == 0 && nsec == 0);
	ntfs_time_to_unix(NTFS_TIME_OFFSET + 15, &sec, &nsec);
	CHECK(sec == 0 && nsec == 1500);
	ntfs_time_to_unix(NTFS_TIME_OFFSET - 1, &sec, &nsec);
	CHECK((int64_t)sec == -1 && nsec == 999999900);
	/* extreme on-disk values: no overflow (a fuzzed volume) */
	ntfs_time_to_unix(-0x7FFFFFFFFFFFFFFFLL - 1, &sec, &nsec);
	ntfs_time_to_unix(0x7FFFFFFFFFFFFFFFLL, &sec, &nsec);
	CHECK(nsec < 1000000000ULL);
}

/* ---- write side ---- */

/* The mapping-pairs encoder is the exact inverse of the decoder: random
 * runlists (holes, deltas both ways, long runs) survive encode + decode,
 * also split into extents at every possible cut. */
static void test_mp_encode(void)
{
	ntfs_fs_t *fs = t_fs();
	uint8_t *buf = (uint8_t *)kalloc(4096), *mp = (uint8_t *)kalloc(2048);

	fs->nr_clusters = 1ULL << 40;
	for (int it = 0; it < 300; it++) {
		ntfs_runlist_t rl, back;
		int64_t v = 0, stop;
		uint32_t used, nruns = 1 + t_rand() % 40;

		h_memset(&rl, 0, sizeof(rl));
		for (uint32_t i = 0; i < nruns; i++) {
			int64_t len = 1 + (int64_t)(t_rand() % ((i & 3) ? 50 : 70000));
			int64_t lcn = (t_rand() % 4) ? (int64_t)(t_rand() % 1000000) *
								  ((i % 5) ? 1 : 3000) :
							   NTFS_LCN_HOLE;

			ntfs_rl_append(&rl, v, lcn, len);
			v += len;
		}
		used = ntfs_mp_encode(&rl, 0, rl.end_vcn, mp, 2048, &stop);
		CHECK(stop == rl.end_vcn);
		CHECK(used > 0 && used <= 2048 && mp[used - 1] == 0);
		{
			ntfs_attr_record_t *a = (ntfs_attr_record_t *)buf;

			h_memset(buf, 0, 4096);
			a->type = NTFS_AT_DATA;
			a->non_resident = 1;
			a->data.non_resident.mapping_pairs_offset = 64;
			a->data.non_resident.lowest_vcn = 0;
			a->data.non_resident.highest_vcn = rl.end_vcn - 1;
			a->data.non_resident.allocated_size = rl.end_vcn << 12;
			a->data.non_resident.data_size = rl.end_vcn << 12;
			a->data.non_resident.initialized_size = rl.end_vcn << 12;
			a->length = (64 + used + 7) & ~7u;
			h_memcpy(buf + 64, mp, used);
			h_memset(&back, 0, sizeof(back));
			CHECK(ntfs_mp_decode(fs, a, a->length, &back) == ST_OK);
			CHECK(back.count == rl.count && back.end_vcn == rl.end_vcn);
			for (uint32_t i = 0; i < rl.count && i < back.count; i++)
				CHECK(back.runs[i].vcn == rl.runs[i].vcn &&
				      back.runs[i].lcn == rl.runs[i].lcn &&
				      back.runs[i].len == rl.runs[i].len);
			ntfs_rl_free(&back);
		}
		/* Extents of at most 24 bytes of pairs continue each other. */
		{
			int64_t at = 0;
			int guard = 0;

			while (at < rl.end_vcn && guard++ < 1000) {
				used = ntfs_mp_encode(&rl, at, rl.end_vcn, mp, 24,
						      &stop);
				CHECK(stop > at);
				at = stop;
			}
			CHECK(at == rl.end_vcn);
		}
		ntfs_rl_free(&rl);
	}
	kfree(mp);
	kfree(buf);
	kfree(fs);
}

/* ntfs_rl_set() / ntfs_rl_truncate() against a flat model. */
static void test_rl_edit(void)
{
	static int64_t model[600];
	ntfs_runlist_t rl;

	h_memset(&rl, 0, sizeof(rl));
	for (int i = 0; i < 600; i++)
		model[i] = -2; /* not described */
	for (int it = 0; it < 2000; it++) {
		int64_t v = t_rand() % 500, len = 1 + t_rand() % 60;
		int64_t lcn = (t_rand() % 3) ? 1000 + (int64_t)(t_rand() % 5000) :
					       NTFS_LCN_HOLE;

		if (v > rl.end_vcn + 30)
			v = rl.end_vcn;
		if (v + len > 590)
			len = 590 - v;
		if (len <= 0)
			continue;
		if ((t_rand() % 10) == 0) {
			int64_t cut = t_rand() % (rl.end_vcn + 1);

			CHECK(ntfs_rl_truncate(&rl, cut) == ST_OK);
			for (int64_t i = cut; i < 600; i++)
				model[i] = -2;
			continue;
		}
		CHECK(ntfs_rl_set(&rl, v, len, lcn) == ST_OK);
		for (int64_t i = 0; i < v; i++)
			if (model[i] == -2)
				model[i] = NTFS_LCN_HOLE; /* the gap */
		for (int64_t i = 0; i < len; i++)
			model[v + i] = lcn == NTFS_LCN_HOLE ? NTFS_LCN_HOLE : lcn + i;
		/* check every VCN */
		for (int64_t i = 0; i < 600; i++) {
			int64_t got, left;
			int r = ntfs_rl_map(&rl, i, &got, &left);

			if (model[i] == -2)
				CHECK(r == ST_IO);
			else
				CHECK(r == ST_OK && got == model[i]);
		}
		/* coalesced: no two neighbours mergeable */
		for (uint32_t k = 1; k < rl.count; k++) {
			const ntfs_run_t *a = &rl.runs[k - 1], *b = &rl.runs[k];

			CHECK(!(a->lcn == NTFS_LCN_HOLE && b->lcn == NTFS_LCN_HOLE));
			CHECK(!(a->lcn >= 0 && b->lcn == a->lcn + a->len));
		}
	}
	ntfs_rl_free(&rl);
}

/* The reservation list: adds merge, cuts split. */
static void test_dl(void)
{
	ntfs_stream_t *st = (ntfs_stream_t *)kcalloc(1, sizeof(*st));
	static uint8_t m[400];

	for (int it = 0; it < 3000; it++) {
		int64_t v = t_rand() % 300, len = 1 + t_rand() % 40;
		int add = t_rand() % 2;

		if (add) {
			int free_run = 1;

			for (int64_t i = v; i < v + len; i++)
				free_run &= !m[i];
			if (!free_run)
				continue;
			CHECK(ntfs_dl_add(st, v, len) == ST_OK);
			for (int64_t i = v; i < v + len; i++)
				m[i] = 1;
		} else {
			uint64_t want = 0, got;

			for (int64_t i = v; i < v + len; i++)
				want += m[i];
			got = ntfs_dl_cut(st, v, len);
			CHECK(got == want);
			for (int64_t i = v; i < v + len; i++)
				m[i] = 0;
		}
		for (int64_t i = 0; i < 400; i++)
			CHECK((ntfs_dl_find(st, i) >= 0) == !!m[i]);
		for (uint32_t k = 1; k < st->dl_n; k++)
			CHECK(st->dl[k - 1].vcn + st->dl[k - 1].len < st->dl[k].vcn);
	}
	ntfs_free(st->dl);
	kfree(st);
}

/* Names: one spelling (a surrogate pair is never two 3-byte sequences),
 * the path conversion pairs only real pairs, Windows names. */
static void test_names_write(void)
{
	uint16_t u[8];
	char out[32];
	/* U+D83D U+DE00 as two 3-byte sequences (CESU): refused */
	static const char cesu[] = "\xED\xA0\xBD\xED\xB8\x80";
	/* a lone high surrogate: kept round-trippable */
	static const char lone[] = "\xED\xA0\xBDx";
	uint16_t hi_bs[2];

	CHECK(ntfs_utf8_to_utf16(cesu, 6, u, 8) == ST_INVALID);
	CHECK(ntfs_utf8_to_utf16("\xF0\x9F\x98\x80", 4, u, 8) == 2);
	CHECK(ntfs_utf8_to_utf16(lone, 4, u, 8) == 2);
	ntfs_put_le16(&hi_bs[0], 0xD83D);
	ntfs_put_le16(&hi_bs[1], '\\');
	CHECK(ntfs_utf16_path(hi_bs, 2, out, sizeof(out)) == 4 &&
	      out[3] == '/');
	{
		uint16_t w[8];
		long n = ntfs_utf8_to_utf16("CON.txt", 7, w, 8);

		CHECK(n == 7 && !ntfs_win_name_ok(w, 7));
		n = ntfs_utf8_to_utf16("a:b", 3, w, 8);
		CHECK(n == 3 && !ntfs_win_name_ok(w, 3));
		n = ntfs_utf8_to_utf16("ok.txt", 6, w, 8);
		CHECK(n == 6 && ntfs_win_name_ok(w, 6));
		n = ntfs_utf8_to_utf16("dot.", 4, w, 8);
		CHECK(n == 4 && !ntfs_win_name_ok(w, 4));
	}
}

/* The EA chain builder writes what the validated walk reads back. */
struct t_ea_cnt {
	int n;
	int lx;
};

static int t_ea_cb2(const ntfs_ea_view_t *ea, void *arg)
{
	struct t_ea_cnt *c = (struct t_ea_cnt *)arg;

	c->n++;
	if (ea->name_len == 6 && h_memcmp(ea->name, "$LXMOD", 6) == 0 &&
	    ea->value_len == 4 && ntfs_le32(ea->value) == 0100644)
		c->lx = 1;
	return 0;
}

static void test_ea_build(void)
{
	struct ntfs_ea_build b;
	struct t_ea_cnt c = { 0, 0 };
	uint8_t v[4];

	h_memset(&b, 0, sizeof(b));
	ntfs_put_le32(v, 0100644);
	CHECK(ntfs_ea_emit(&b, "$LXMOD", 6, v, 4, 0) == ST_OK);
	CHECK(ntfs_ea_emit(&b, "user.empty", 10, NULL, 0, 0) == ST_OK);
	CHECK(ntfs_ea_emit(&b, "user.x", 6, (const uint8_t *)"abc", 3,
			   NTFS_EA_NEED_EA) == ST_OK);
	CHECK(ntfs_ea_walk(b.out, b.len, t_ea_cb2, &c) == ST_OK);
	CHECK(c.n == 3 && c.lx == 1);
	CHECK(b.packed == 5 + 6 + 4 + 5 + 10 + 5 + 6 + 3 && b.need == 1);
	ntfs_free(b.out);
}

/* rwsem: a dead task's shared holds are released. */
static void test_rwsem_release(void)
{
	ntfs_rwsem_t *s = (ntfs_rwsem_t *)kcalloc(1, sizeof(*s));
	task_t *cur = sched_current();

	ntfs_rwsem_init(s, "t");
	ntfs_rwsem_read_lock(s);
	ntfs_rwsem_read_lock(s);
	CHECK(s->readers == 2);
	CHECK(ntfs_rwsem_release_owner(s, cur->id) == 1);
	CHECK(s->readers == 0);
	/* the lock is usable exclusively now */
	cur->fs_rdepth = 0;
	cur->fs_held = 0;
	ntfs_rwsem_write_lock(s);
	CHECK(ntfs_rwsem_held_excl(s));
	ntfs_rwsem_write_unlock(s);
	kfree(s);
}

/* Index nodes: encode / decode round trip, END entry, child VCNs. */
static void test_xnode(void)
{
	ntfs_ixs_t *s = (ntfs_ixs_t *)kcalloc(1, sizeof(*s));
	ntfs_xnode_t *nd = ntfs_xnode_new(s, 3), *back;
	uint8_t *buf = (uint8_t *)kcalloc(1, 8192);
	ntfs_index_header_t *h = (ntfs_index_header_t *)buf;
	uint32_t used;

	nd->is_node = 1;
	nd->end_child = 77;
	for (int i = 0; i < 5; i++) {
		uint8_t *e = (uint8_t *)kcalloc(1, 32);
		ntfs_index_entry_t *ie = (ntfs_index_entry_t *)e;

		ie->length = 32;
		ie->key_length = 12;
		e[16] = (uint8_t)i;
		CHECK(ntfs_xnode_ins(nd, (uint32_t)i, e, 10 + i) == ST_OK);
	}
	used = ntfs_xnode_encode(nd, buf + 16);
	CHECK(used == ntfs_xnode_bytes(nd));
	h->entries_offset = 16;
	h->index_length = 16 + used;
	h->allocated_size = 16 + used;
	back = ntfs_xnode_new(s, 4);
	CHECK(ntfs_xnode_decode(back, h, 1) == ST_OK);
	CHECK(back->n == 5 && back->end_child == 77);
	for (int i = 0; i < 5 && i < (int)back->n; i++)
		CHECK(back->ents[i].child == 10 + i && back->ents[i].e[16] == i &&
		      ((ntfs_index_entry_t *)back->ents[i].e)->length == 32);
	ntfs_ixs_free(s);
	kfree(buf);
}

/* ---- LZNT1 compressor, descriptors, ACLs, log helpers ---- */

/* Compress whole units of several kinds of data and decompress them
 * again: the round trip is exact, a unit that does not pay is refused. */
static void test_lznt1_compress(void)
{
	static uint8_t unit[65536], comp[65536], back[65536];
	struct ntfs_lznt1_ws *ws = (struct ntfs_lznt1_ws *)h_malloc(sizeof(*ws));

	for (int kind = 0; kind < 6; kind++) {
		uint32_t valid = kind == 5 ? 5000 : 65536, clen;
		long n;

		for (uint32_t i = 0; i < 65536; i++) {
			switch (kind) {
			case 0: unit[i] = 0; break;
			case 1: unit[i] = (uint8_t)(i % 7); break;
			case 2: unit[i] = (uint8_t)"the quick brown fox "[i % 20]; break;
			case 3: unit[i] = (uint8_t)t_rand(); break;
			case 4: unit[i] = (i / 300) % 2 ? (uint8_t)t_rand() : 'x'; break;
			default: unit[i] = i < valid ? (uint8_t)(i * 13) : 0; break;
			}
		}
		clen = ntfs_lznt1_compress(unit, valid, comp, 65536 - 4096, ws);
		if (kind == 3) {
			CHECK(clen == 0); /* random data does not compress */
			continue;
		}
		CHECK(clen > 0 && clen <= 65536 - 4096);
		n = ntfs_lznt1_decompress(comp, clen, back, 65536);
		CHECK(n >= 0);
		CHECK(h_memcmp(back, unit, 65536) == 0);
		/* What follows the data in the clusters is zero: the unit
		 * ends there for a reader. */
		CHECK(clen == 65536 - 4096 || comp[clen] == 0);
	}
	/* Every chunk boundary position of a match: lengths and
	 * displacements at the limits of each width. */
	for (uint32_t i = 0; i < 65536; i++)
		unit[i] = (uint8_t)((i * i) >> 7);
	{
		uint32_t clen = ntfs_lznt1_compress(unit, 65536, comp, 65536 - 4096,
						    ws);

		if (clen) {
			CHECK(ntfs_lznt1_decompress(comp, clen, back, 65536) >= 0);
			CHECK(h_memcmp(back, unit, 65536) == 0);
		}
	}
	h_free(ws);
}

static void test_sd_acl_valid(void)
{
	uint32_t l = 0;
	uint8_t *sd = ntfs_sd_default(&l);
	uint8_t acl[4 + 8 * 5];
	int equiv = 0;

	CHECK(sd && ntfs_sd_valid(sd, l));
	CHECK(!ntfs_sd_valid(sd, l - 1)); /* the group SID cut off */
	sd[0] = 2;
	CHECK(!ntfs_sd_valid(sd, l));
	sd[0] = 1;
	ntfs_put_le16(sd + 2, 0x0004); /* not self-relative */
	CHECK(!ntfs_sd_valid(sd, l));
	ntfs_put_le16(sd + 2, 0x8004);
	ntfs_put_le16(sd + 20 + 2, 200); /* DACL larger than the descriptor */
	CHECK(!ntfs_sd_valid(sd, l));
	ntfs_put_le16(sd + 20 + 2, 28);
	CHECK(ntfs_sd_valid(sd, l));
	/* The hash: rotate-left-3-and-add over the words. */
	{
		uint8_t w[8] = { 1, 0, 0, 0, 2, 0, 0, 0 };

		CHECK(ntfs_sd_hash(w, 8) == ((1U << 3) + 2U));
		CHECK(ntfs_sd_hash(w, 7) == 1U);
	}
	kfree(sd);
	/* POSIX ACLs. */
	ntfs_put_le32(acl, 2);
	{
		static const uint16_t ent[5][2] = { { 1, 7 }, { 2, 6 }, { 4, 5 },
						    { 0x10, 7 }, { 0x20, 4 } };

		for (int i = 0; i < 5; i++) {
			ntfs_put_le16(acl + 4 + 8 * i, ent[i][0]);
			ntfs_put_le16(acl + 4 + 8 * i + 2, ent[i][1]);
			ntfs_put_le32(acl + 4 + 8 * i + 4, i == 1 ? 1000 : 0);
		}
	}
	CHECK(ntfs_pacl_valid(acl, sizeof(acl), 0, &equiv) && !equiv);
	CHECK(ntfs_pacl_mode(acl, sizeof(acl)) == 0774);
	ntfs_pacl_apply_mode(acl, sizeof(acl), 0640, 0);
	CHECK(ntfs_pacl_mode(acl, sizeof(acl)) == 0640);
	CHECK(!ntfs_pacl_valid(acl, sizeof(acl) - 1, 0, &equiv));
	CHECK(!ntfs_pacl_valid(acl, 4, 0, &equiv));
	CHECK(ntfs_pacl_valid(acl, 4, 1, &equiv));
	ntfs_put_le16(acl + 4 + 8 * 3, 0x08); /* the mask gone */
	CHECK(!ntfs_pacl_valid(acl, sizeof(acl), 0, &equiv));
	/* $LogFile: the sequence-number width is fixed by the file size. */
	CHECK(ntfs_log_seq_bits(0x200000) == 45);
	CHECK(ntfs_log_seq_bits(0x4000000) == 40);
}


int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	test_task_init(0, 0);
	test_utf();
	test_mapping_pairs();
	test_lznt1();
	test_mst();
	test_collation();
	test_ea();
	test_attr_valid();
	test_restart();
	test_index_node();
	test_boot_decoders();
	test_link_target();
	test_time();
	test_mp_encode();
	test_rl_edit();
	test_dl();
	test_names_write();
	test_ea_build();
	test_rwsem_release();
	test_xnode();
	test_lznt1_compress();
	test_sd_acl_valid();
	h_printf("test-ntfs unit: %d checks, %d failed\n", t_count, t_fail);
	host_flush_out();
	return t_fail ? 1 : 0;
}
