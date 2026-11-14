/* libgpt host tests.
 *
 *   test_gpt unit                    in-memory tests (no files)
 *   test_gpt build IMG SECTOR_SIZE   write a table with several partitions
 *                                    on IMG, edit it, read it back
 *   test_gpt dump IMG [SECTOR_SIZE]  one line per partition:
 *                                    N FIRST LAST TYPE UUID NAME
 *   test_gpt status IMG [SECTOR_SIZE] the gpt_status flags of IMG
 *   test_gpt repair IMG [SECTOR_SIZE] gpt_repair from the good copy
 *   test_gpt mbr IMG                 the MBR partitions of IMG
 *   test_gpt verify IMG [SECTOR_SIZE] gpt_verify, prints messages
 *   test_gpt headers IMG SECTOR_SIZE  headers with valid CRCs but bad
 *                                    fields: read, repair and wipe
 *
 * run-tests.sh drives these against image files and the host's sgdisk and
 * sfdisk.  Exit status 0 = pass.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpt.h"
#include "gpt_internal.h"

static int g_fail;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			g_fail++;                                            \
		}                                                            \
	} while (0)

#define CHECK_EQ_U64(a, b)                                                   \
	do {                                                                 \
		unsigned long long _a = (a), _b = (b);                       \
		if (_a != _b) {                                              \
			fprintf(stderr, "FAIL %s:%d: %s == %llu, want %llu\n", \
				__FILE__, __LINE__, #a, _a, _b);             \
			g_fail++;                                            \
		}                                                            \
	} while (0)

/* ---- unit tests ------------------------------------------------------ */

static void test_crc32(void)
{
	CHECK(gpt_crc32(0, "123456789", 9) == 0xCBF43926u);
	CHECK(gpt_crc32(0, "", 0) == 0);
	/* Continuing a CRC equals one pass over the whole. */
	CHECK(gpt_crc32(gpt_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
	{
		uint8_t zeros[32] = { 0 };

		CHECK(gpt_crc32(0, zeros, 32) == 0x190A55ADu);
	}
}

static void test_guid(void)
{
	static const uint8_t esp_disk[16] = {
		0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
		0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
	};
	struct gpt_guid g, h, r1, r2;
	uint8_t disk[16];
	char s[GPT_GUID_STR_LEN];

	CHECK(gpt_guid_parse(GPT_TYPE_ESP, &g) == 0);
	gpt_guid_to_disk(&g, disk);
	CHECK(memcmp(disk, esp_disk, 16) == 0);
	gpt_guid_from_disk(disk, &h);
	CHECK(gpt_guid_equal(&g, &h));
	gpt_guid_format(&g, s);
	CHECK(strcmp(s, GPT_TYPE_ESP) == 0);
	gpt_guid_format_upper(&g, s);
	CHECK(strcmp(s, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") == 0);
	CHECK(gpt_guid_parse("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}", &h) == 0);
	CHECK(gpt_guid_equal(&g, &h));
	CHECK(gpt_guid_parse("c12a7328f81f-11d2-ba4b-00a0c93ec93b", &h) != 0);
	CHECK(gpt_guid_parse("c12a7328-f81f-11d2-ba4b-00a0c93ec93", &h) != 0);
	CHECK(gpt_guid_parse("g12a7328-f81f-11d2-ba4b-00a0c93ec93b", &h) != 0);
	CHECK(gpt_guid_parse("c12a7328-f81f-11d2-ba4b-00a0c93ec93b0", &h) != 0);
	CHECK(gpt_guid_random(&r1) == 0);
	CHECK(gpt_guid_random(&r2) == 0);
	CHECK(!gpt_guid_equal(&r1, &r2));
	CHECK((r1.b[6] >> 4) == 4);
	CHECK((r1.b[8] & 0xc0) == 0x80);
	CHECK(!gpt_guid_is_zero(&r1));
	memset(&h, 0, sizeof(h));
	CHECK(gpt_guid_is_zero(&h));
}

static void test_names(void)
{
	uint16_t raw[GPT_NAME_UNITS];
	char out[GPT_NAME_UTF8_MAX];
	/* "Größe 😀": the emoji is U+1F600 -> d83d de00. */
	const char *name = "Gr\xc3\xb6\xc3\x9f" "e \xf0\x9f\x98\x80";
	int n;

	n = gpt_name_from_utf8(name, raw);
	CHECK(n == 8);
	CHECK(raw[0] == 'G' && raw[2] == 0xf6 && raw[3] == 0xdf);
	CHECK(raw[6] == 0xd83d && raw[7] == 0xde00);
	CHECK(raw[8] == 0);
	gpt_name_to_utf8(raw, GPT_NAME_UNITS, out, sizeof(out));
	CHECK(strcmp(out, name) == 0);
	/* 36 units fit, 37 do not. */
	CHECK(gpt_name_from_utf8("123456789012345678901234567890123456",
				 raw) == 36);
	CHECK(gpt_name_from_utf8("1234567890123456789012345678901234567",
				 raw) == -ENAMETOOLONG);
	/* A surrogate pair may not be split at the end. */
	CHECK(gpt_name_from_utf8("12345678901234567890123456789012345"
				 "\xf0\x9f\x98\x80", raw) == -ENAMETOOLONG);
	/* Bad UTF-8: a lone continuation byte, an overlong form, a
	 * surrogate. */
	CHECK(gpt_name_from_utf8("a\x80", raw) == -EILSEQ);
	CHECK(gpt_name_from_utf8("\xc0\xaf", raw) == -EILSEQ);
	CHECK(gpt_name_from_utf8("\xed\xa0\x80", raw) == -EILSEQ);
	/* A lone surrogate on disk reads as U+FFFD. */
	memset(raw, 0, sizeof(raw));
	raw[0] = 'x';
	raw[1] = 0xd800;
	raw[2] = 'y';
	gpt_name_to_utf8(raw, GPT_NAME_UNITS, out, sizeof(out));
	CHECK(strcmp(out, "x\xef\xbf\xbdy") == 0);
	/* A small output buffer truncates at a character boundary. */
	n = gpt_name_from_utf8(name, raw);
	gpt_name_to_utf8(raw, GPT_NAME_UNITS, out, 4);
	CHECK(strcmp(out, "Gr") == 0);
}

static void test_types(void)
{
	struct gpt_guid g, h;

	CHECK(gpt_type_parse("esp", &g) == 0);
	CHECK(gpt_guid_parse(GPT_TYPE_ESP, &h) == 0 && gpt_guid_equal(&g, &h));
	CHECK(gpt_type_parse("EF00", &g) == 0 && gpt_guid_equal(&g, &h));
	CHECK(gpt_type_parse("0xef00", &g) == 0 && gpt_guid_equal(&g, &h));
	CHECK(gpt_type_parse("8304", &g) == 0);
	CHECK(gpt_guid_parse(GPT_TYPE_LIKEOS_ROOT, &h) == 0 &&
	      gpt_guid_equal(&g, &h));
	CHECK(strcmp(gpt_type_name(&h), "LikeOS root") == 0);
	CHECK(gpt_type_parse("likeos-root", &g) == 0 && gpt_guid_equal(&g, &h));
	CHECK(gpt_type_parse("swap", &g) == 0);
	CHECK(strcmp(gpt_type_name(&g), "Swap") == 0);
	CHECK(gpt_type_parse("data", &g) == 0);
	CHECK(strcmp(gpt_type_name(&g), "Generic data") == 0);
	CHECK(gpt_type_parse("nonsense", &g) == -ENOENT);
	CHECK(gpt_type_parse("12345678-1234-1234-1234-123456789abc", &g) == 0);
	CHECK(strcmp(gpt_type_name(&g), "Unknown") == 0);
	/* Every GUID in the table parses, is unique and is found again. */
	for (int i = 0; i < gpt_type_count(); i++) {
		const struct gpt_type_info *ti = gpt_type_at(i);

		CHECK(gpt_guid_parse(ti->guid, &g) == 0);
		CHECK(gpt_type_lookup(&g) == ti);
		for (int j = i + 1; j < gpt_type_count(); j++) {
			CHECK(strcmp(ti->guid, gpt_type_at(j)->guid) != 0);
			if (ti->code)
				CHECK(ti->code != gpt_type_at(j)->code);
		}
	}
}

static void test_layout(uint32_t ss)
{
	struct gpt_table t;
	struct gpt_part_spec spec;
	struct gpt_free fr[8];
	uint64_t mib = (1024 * 1024) / ss;
	uint64_t nsec = 1024 * mib; /* 1 GiB */
	int n, p1, p2, p3, order[8];

	CHECK(gpt_table_new(&t, ss, nsec) == 0);
	CHECK_EQ_U64(t.first_usable, ss == 512 ? 34 : 6);
	CHECK_EQ_U64(t.last_usable, ss == 512 ? nsec - 34 : nsec - 6);
	CHECK_EQ_U64(t.num_entries, 128);
	CHECK_EQ_U64(gpt_align_sectors(&t), mib);

	n = gpt_free_regions(&t, fr, 8, 1);
	CHECK(n == 1);
	CHECK_EQ_U64(fr[0].first_lba, mib);
	CHECK_EQ_U64(fr[0].last_lba, t.last_usable);
	n = gpt_free_regions(&t, fr, 8, 0);
	CHECK(n == 1 && fr[0].first_lba == t.first_usable);

	memset(&spec, 0, sizeof(spec));
	CHECK(gpt_type_parse("esp", &spec.type) == 0);
	spec.size_lba = 100 * mib;
	spec.name = "EFI system partition";
	CHECK(gpt_part_add(&t, &spec, &p1) == 0);
	CHECK(p1 == 1);
	CHECK_EQ_U64(t.parts[0].first_lba, mib);
	CHECK_EQ_U64(t.parts[0].last_lba, 101 * mib - 1);

	memset(&spec, 0, sizeof(spec));
	spec.size_lba = 200 * mib;
	CHECK(gpt_part_add(&t, &spec, &p2) == 0);
	CHECK(p2 == 2);
	CHECK_EQ_U64(t.parts[1].first_lba, 101 * mib);

	memset(&spec, 0, sizeof(spec));
	CHECK(gpt_type_parse("likeos-root", &spec.type) == 0);
	CHECK(gpt_part_add(&t, &spec, &p3) == 0);
	CHECK(p3 == 3);
	CHECK_EQ_U64(t.parts[2].first_lba, 301 * mib);
	CHECK_EQ_U64(t.parts[2].last_lba, t.last_usable);
	CHECK(gpt_free_regions(&t, fr, 8, 1) == 0);

	/* No room left. */
	memset(&spec, 0, sizeof(spec));
	CHECK(gpt_part_add(&t, &spec, NULL) == -ENOSPC);
	CHECK(t.errmsg[0] != '\0');

	/* Delete the middle one: a hole; add with explicit number into it. */
	CHECK(gpt_part_delete(&t, 2) == 0);
	CHECK(gpt_part_delete(&t, 2) == -ENOENT);
	n = gpt_free_regions(&t, fr, 8, 1);
	CHECK(n == 1 && fr[0].first_lba == 101 * mib &&
	      fr[0].last_lba == 301 * mib - 1);
	memset(&spec, 0, sizeof(spec));
	spec.partno = 7;
	spec.size_lba = 50 * mib;
	CHECK(gpt_part_add(&t, &spec, &p2) == 0 && p2 == 7);
	/* Grow it to the maximum, shrink, grow too far. */
	CHECK_EQ_U64(gpt_part_max_size(&t, 7), 200 * mib);
	CHECK(gpt_part_resize(&t, 7, 0) == 0);
	CHECK_EQ_U64(t.parts[6].last_lba, 301 * mib - 1);
	CHECK(gpt_part_resize(&t, 7, 20 * mib) == 0);
	CHECK_EQ_U64(t.parts[6].last_lba, 121 * mib - 1);
	CHECK(gpt_part_resize(&t, 7, 201 * mib) == -ENOSPC);
	/* Explicit start inside a partition and overlapping adds. */
	memset(&spec, 0, sizeof(spec));
	spec.first_lba = 110 * mib;
	CHECK(gpt_part_add(&t, &spec, NULL) == -EEXIST);
	spec.first_lba = 121 * mib;
	spec.size_lba = 181 * mib;
	CHECK(gpt_part_add(&t, &spec, NULL) == -ENOSPC);
	spec.size_lba = 180 * mib;
	CHECK(gpt_part_add(&t, &spec, &p2) == 0 && p2 == 2);
	CHECK(gpt_overlap(&t, 121 * mib, 121 * mib, 0) == 2);
	CHECK(gpt_overlap(&t, 121 * mib, 121 * mib, 2) == 0);
	/* Duplicate number and out-of-range number. */
	memset(&spec, 0, sizeof(spec));
	spec.partno = 1;
	CHECK(gpt_part_add(&t, &spec, NULL) == -EEXIST);
	spec.partno = 129;
	CHECK(gpt_part_add(&t, &spec, NULL) == -EINVAL);
	/* Order by start. */
	n = gpt_part_order(&t, order, 8);
	CHECK(n == 4);
	CHECK(order[0] == 1 && order[1] == 7 && order[2] == 2 && order[3] == 3);
	CHECK(gpt_part_count(&t) == 4);
	CHECK(gpt_part_first_free_slot(&t) == 4);
	/* Names, types, attributes, uuids. */
	CHECK(gpt_part_set_name(&t, 3, "likeos-root") == 0);
	CHECK(strcmp(gpt_part_get(&t, 3)->name, "likeos-root") == 0);
	CHECK(gpt_part_set_name(&t, 3, "\xff") == -EILSEQ);
	{
		struct gpt_guid g;

		CHECK(gpt_type_parse("swap", &g) == 0);
		CHECK(gpt_part_set_type(&t, 7, &g) == 0);
		memset(&g, 0, sizeof(g));
		CHECK(gpt_part_set_type(&t, 7, &g) == -EINVAL);
		g = gpt_part_get(&t, 1)->uuid;
		CHECK(gpt_part_set_uuid(&t, 3, &g) == -EEXIST);
		CHECK(gpt_part_set_uuid(&t, 3, NULL) == 0);
	}
	CHECK(gpt_part_set_attributes(&t, 1, GPT_ATTR_REQUIRED) == 0);
	CHECK(gpt_part_get(&t, 1)->attributes == GPT_ATTR_REQUIRED);
	CHECK(gpt_verify(NULL, &t, NULL, NULL) == 0);
	/* Copy is deep. */
	{
		struct gpt_table c;

		CHECK(gpt_table_copy(&c, &t) == 0);
		CHECK(gpt_part_delete(&c, 1) == 0);
		CHECK(gpt_part_get(&t, 1) != NULL);
		gpt_table_free(&c);
	}
	/* An overlap planted by hand is found by verify. */
	t.parts[6].last_lba = 130 * mib;
	CHECK(gpt_verify(NULL, &t, NULL, NULL) == 1);
	gpt_table_free(&t);

	/* A damaged entry that ends on the last possible sector: the free
	 * space after it must not wrap around to sector 0. */
	CHECK(gpt_table_new(&t, ss, nsec) == 0);
	CHECK(gpt_type_parse("data", &t.parts[0].type) == 0);
	t.parts[0].used = 1;
	t.parts[0].first_lba = 500 * mib;
	t.parts[0].last_lba = UINT64_MAX;
	t.parts[1] = t.parts[0];
	t.parts[1].first_lba = 600 * mib;
	t.parts[1].last_lba = 700 * mib - 1;
	n = gpt_free_regions(&t, fr, 8, 0);
	CHECK(n == 1);
	CHECK_EQ_U64(fr[0].first_lba, t.first_usable);
	CHECK_EQ_U64(fr[0].last_lba, 500 * mib - 1);
	memset(&spec, 0, sizeof(spec));
	spec.size_lba = 550 * mib; /* the 600 MiB a wrap would invent */
	CHECK(gpt_part_add(&t, &spec, NULL) == -ENOSPC);
	gpt_table_free(&t);

	/* A disk too small for a table. */
	CHECK(gpt_table_new(&t, ss, 10) == -ENOSPC);
}

static int cmd_unit(void)
{
	test_crc32();
	test_guid();
	test_names();
	test_types();
	test_layout(512);
	test_layout(4096);
	printf("unit tests: %s (%d failures)\n", g_fail ? "FAILED" : "passed",
	       g_fail);
	return g_fail ? 1 : 0;
}

/* ---- image tests ----------------------------------------------------- */

static uint32_t arg_ss(int argc, char **argv, int i)
{
	return argc > i ? (uint32_t)strtoul(argv[i], NULL, 10) : 0;
}

static int cmd_build(const char *img, uint32_t ss)
{
	struct gpt_dev d;
	struct gpt_table t, r;
	struct gpt_part_spec spec;
	uint64_t mib;
	int rc, pn;
	const char *uname = "Wurzel \xc3\xa4\xc3\xb6\xc3\xbc \xf0\x9f\x98\x80";

	rc = gpt_dev_open(&d, img, GPT_OPEN_RDWR | GPT_OPEN_EXCL, ss);
	if (rc) {
		fprintf(stderr, "open %s: %s\n", img, strerror(-rc));
		return 1;
	}
	CHECK(d.is_file);
	CHECK(d.sector_size == (ss ? ss : 512));
	mib = (1024 * 1024) / d.sector_size;
	CHECK(gpt_wipe(&d) == 0);
	CHECK(gpt_table_new(&t, d.sector_size, d.nsectors) == 0);

	memset(&spec, 0, sizeof(spec));
	CHECK(gpt_type_parse("esp", &spec.type) == 0);
	spec.size_lba = 512 * mib;
	spec.name = "EFI system partition";
	CHECK(gpt_part_add(&t, &spec, &pn) == 0 && pn == 1);
	memset(&spec, 0, sizeof(spec));
	spec.size_lba = 64 * mib;
	spec.name = "scratch";
	CHECK(gpt_part_add(&t, &spec, &pn) == 0 && pn == 2);
	memset(&spec, 0, sizeof(spec));
	CHECK(gpt_type_parse("likeos-root", &spec.type) == 0);
	spec.name = uname;
	CHECK(gpt_part_add(&t, &spec, &pn) == 0 && pn == 3);
	CHECK(gpt_write(&d, &t) == 0);

	/* Edit: delete the scratch partition, shrink root, add swap at the
	 * end, grow the ESP into the hole. */
	CHECK(gpt_part_delete(&t, 2) == 0);
	CHECK(gpt_part_resize(&t, 3, gpt_part_get(&t, 3)->last_lba -
				     gpt_part_get(&t, 3)->first_lba + 1 -
				     128 * mib) == 0);
	CHECK(gpt_part_resize(&t, 1, 0) == 0);
	CHECK_EQ_U64(gpt_part_get(&t, 1)->last_lba, 577 * mib - 1);
	memset(&spec, 0, sizeof(spec));
	CHECK(gpt_type_parse("swap", &spec.type) == 0);
	spec.name = "swap";
	CHECK(gpt_part_add(&t, &spec, &pn) == 0 && pn == 2);
	CHECK(gpt_part_get(&t, 2)->first_lba > gpt_part_get(&t, 3)->last_lba);
	CHECK_EQ_U64(gpt_part_get(&t, 2)->last_lba, t.last_usable);
	CHECK(gpt_part_set_attributes(&t, 1, GPT_ATTR_REQUIRED) == 0);
	CHECK(gpt_write(&d, &t) == 0);
	gpt_dev_close(&d);

	/* Read it back from a fresh open and compare. */
	rc = gpt_dev_open(&d, img, GPT_OPEN_RDONLY, ss);
	CHECK(rc == 0);
	CHECK(gpt_read(&d, &r) == 0);
	CHECK(r.status.primary_header_ok && r.status.primary_entries_ok);
	CHECK(r.status.backup_header_ok && r.status.backup_entries_ok);
	CHECK(!r.status.backup_not_at_end && !r.status.headers_differ);
	CHECK(r.status.pmbr_kind == MBR_KIND_PROTECTIVE);
	CHECK(gpt_guid_equal(&r.disk_guid, &t.disk_guid));
	CHECK(r.first_usable == t.first_usable && r.last_usable == t.last_usable);
	for (uint32_t i = 0; i < t.num_entries; i++) {
		CHECK(r.parts[i].used == t.parts[i].used);
		CHECK(r.parts[i].first_lba == t.parts[i].first_lba);
		CHECK(r.parts[i].last_lba == t.parts[i].last_lba);
		CHECK(r.parts[i].attributes == t.parts[i].attributes);
		CHECK(gpt_guid_equal(&r.parts[i].type, &t.parts[i].type));
		CHECK(gpt_guid_equal(&r.parts[i].uuid, &t.parts[i].uuid));
		CHECK(strcmp(r.parts[i].name, t.parts[i].name) == 0);
	}
	CHECK(strcmp(gpt_part_get(&r, 3)->name, uname) == 0);
	CHECK(gpt_verify(&d, &r, NULL, NULL) == 0);
	{
		struct mbr_table m;

		CHECK(mbr_read(&d, &m) == 0);
		CHECK(m.kind == MBR_KIND_PROTECTIVE);
		CHECK(m.nparts == 1 && m.parts[0].type == 0xee);
		CHECK(m.parts[0].first_lba == 1);
		CHECK(m.parts[0].nsectors ==
		      (d.nsectors - 1 > 0xffffffffULL ? 0xffffffffULL :
		       d.nsectors - 1));
		CHECK(m.parts[0].chs_first[0] == 0 &&
		      m.parts[0].chs_first[1] == 2 &&
		      m.parts[0].chs_first[2] == 0);
	}
	gpt_table_free(&r);
	gpt_table_free(&t);
	gpt_dev_close(&d);
	printf("build %s (%u-byte sectors): %s\n", img, ss ? ss : 512,
	       g_fail ? "FAILED" : "passed");
	return g_fail ? 1 : 0;
}

static int open_read(const char *img, uint32_t ss, struct gpt_dev *d,
		     struct gpt_table *t)
{
	int rc = gpt_dev_open(d, img, GPT_OPEN_RDONLY, ss);

	if (rc) {
		fprintf(stderr, "open %s: %s\n", img, strerror(-rc));
		return rc;
	}
	rc = gpt_read(d, t);
	if (rc) {
		fprintf(stderr, "read %s: %s (%s)\n", img, strerror(-rc),
			t->errmsg);
		gpt_dev_close(d);
	}
	return rc;
}

static int cmd_dump(const char *img, uint32_t ss)
{
	struct gpt_dev d;
	struct gpt_table t;
	char tg[GPT_GUID_STR_LEN], ug[GPT_GUID_STR_LEN];

	if (open_read(img, ss, &d, &t))
		return 1;
	gpt_guid_format(&t.disk_guid, tg);
	printf("disk %s %llu %llu\n", tg, (unsigned long long)t.first_usable,
	       (unsigned long long)t.last_usable);
	for (uint32_t i = 0; i < t.num_entries; i++) {
		const struct gpt_part *p = &t.parts[i];

		if (!p->used)
			continue;
		gpt_guid_format(&p->type, tg);
		gpt_guid_format(&p->uuid, ug);
		printf("%u %llu %llu %s %s %016llx %s\n", i + 1,
		       (unsigned long long)p->first_lba,
		       (unsigned long long)p->last_lba, tg, ug,
		       (unsigned long long)p->attributes, p->name);
	}
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return 0;
}

static int cmd_status(const char *img, uint32_t ss)
{
	struct gpt_dev d;
	struct gpt_table t;
	int rc;

	rc = gpt_dev_open(&d, img, GPT_OPEN_RDONLY, ss);
	if (rc)
		return 1;
	rc = gpt_read(&d, &t);
	printf("read=%d primary_header=%d primary_entries=%d backup_header=%d "
	       "backup_entries=%d backup_not_at_end=%d headers_differ=%d "
	       "using_backup=%d pmbr=%s\n", rc,
	       t.status.primary_header_ok, t.status.primary_entries_ok,
	       t.status.backup_header_ok, t.status.backup_entries_ok,
	       t.status.backup_not_at_end, t.status.headers_differ,
	       t.status.using_backup, mbr_kind_name(t.status.pmbr_kind));
	if (rc == 0)
		gpt_table_free(&t);
	gpt_dev_close(&d);
	return 0;
}

static int cmd_repair(const char *img, uint32_t ss)
{
	struct gpt_dev d;
	struct gpt_table t;
	int rc;

	rc = gpt_dev_open(&d, img, GPT_OPEN_RDWR, ss);
	if (rc)
		return 1;
	rc = gpt_read(&d, &t);
	if (rc) {
		fprintf(stderr, "repair: %s\n", t.errmsg);
		gpt_dev_close(&d);
		return 1;
	}
	rc = gpt_repair(&d, &t);
	if (rc)
		fprintf(stderr, "repair: %s\n", t.errmsg);
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return rc ? 1 : 0;
}

static int cmd_mbr(const char *img)
{
	struct gpt_dev d;
	struct mbr_table m;
	int rc;

	rc = gpt_dev_open(&d, img, GPT_OPEN_RDONLY, 0);
	if (rc)
		return 1;
	rc = mbr_read(&d, &m);
	gpt_dev_close(&d);
	if (rc)
		return 1;
	printf("kind=%s signature=%08x chain_broken=%d\n",
	       mbr_kind_name(m.kind), m.disk_signature, m.chain_broken);
	for (int i = 0; i < m.nparts; i++)
		printf("%d %llu %llu %x%s%s\n", m.parts[i].number,
		       (unsigned long long)m.parts[i].first_lba,
		       (unsigned long long)m.parts[i].nsectors, m.parts[i].type,
		       m.parts[i].is_extended ? " extended" : "",
		       m.parts[i].is_logical ? " logical" : "");
	return 0;
}

static void print_report(int sev, const char *msg, void *user)
{
	(void)user;
	printf("%d %s\n", sev, msg);
}

static int cmd_verify(const char *img, uint32_t ss)
{
	struct gpt_dev d;
	struct gpt_table t;
	int errors;

	if (open_read(img, ss, &d, &t))
		return 1;
	errors = gpt_verify(&d, &t, print_report, NULL);
	printf("errors=%d\n", errors);
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return errors ? 1 : 0;
}

/* ---- crafted headers ------------------------------------------------- */

/* Sets `width' bytes at byte `off' of the header at lba to v and gives
 * the header a valid CRC again: a table as a faulty tool would write it. */
static int patch_header(struct gpt_dev *d, uint64_t lba, unsigned off,
			uint64_t v, int width)
{
	uint8_t s[4096];
	uint32_t hsz;

	if (gpt_dev_read(d, lba, 1, s))
		return -1;
	for (int i = 0; i < width; i++)
		s[off + i] = (uint8_t)(v >> (8 * i));
	hsz = gpt_le32(s + GH_HEADER_SIZE);
	gpt_put32(s + GH_HEADER_CRC, 0);
	gpt_put32(s + GH_HEADER_CRC, gpt_crc32(0, s, hsz));
	return gpt_dev_write(d, lba, 1, s) ? -1 : 0;
}

/* A new table with one partition from 1 MiB to the end. */
static int fresh_table(struct gpt_dev *d)
{
	struct gpt_table t;
	struct gpt_part_spec spec;
	int rc;

	if (gpt_wipe(d) || gpt_table_new(&t, d->sector_size, d->nsectors))
		return -1;
	memset(&spec, 0, sizeof(spec));
	rc = gpt_part_add(&t, &spec, NULL);
	if (!rc)
		rc = gpt_write(d, &t);
	gpt_table_free(&t);
	return rc ? -1 : 0;
}

static int cmd_headers(const char *img, uint32_t ss)
{
	struct gpt_dev d;
	struct gpt_table t;
	struct gpt_header_raw h;
	uint8_t *s;
	uint64_t last, marker;
	int rc, ok;

	rc = gpt_dev_open(&d, img, GPT_OPEN_RDWR, ss);
	if (rc) {
		fprintf(stderr, "open %s: %s\n", img, strerror(-rc));
		return 1;
	}
	last = d.nsectors - 1;
	s = malloc(d.sector_size);
	if (!s)
		return 1;

	/* An entry array LBA so large that LBA + size wraps: the header
	 * is invalid, not "valid with an unreadable array". */
	CHECK(fresh_table(&d) == 0);
	CHECK(patch_header(&d, 1, GH_ENTRIES_LBA, UINT64_MAX - 1, 8) == 0);
	CHECK(gpt_read(&d, &t) == 0);
	CHECK(!t.status.primary_header_ok);
	CHECK(strstr(t.status.primary_problem, "entry array LBA") != NULL);
	CHECK(t.status.using_backup);
	CHECK(gpt_repair(&d, &t) == 0);
	gpt_table_free(&t);
	CHECK(gpt_read(&d, &t) == 0);
	CHECK(t.status.primary_header_ok && t.status.primary_entries_ok);
	gpt_table_free(&t);

	/* A backup whose usable area covers the primary header and array
	 * is not a copy to rebuild the primary from. */
	CHECK(fresh_table(&d) == 0);
	CHECK(patch_header(&d, last, GH_FIRST_USABLE, 1, 8) == 0);
	CHECK(gpt_read(&d, &t) == 0);
	CHECK(t.status.primary_header_ok && !t.status.backup_header_ok);
	CHECK(!t.status.using_backup);
	gpt_table_free(&t);
	CHECK(gpt_dev_read(&d, 1, 1, s) == 0);
	s[GH_MY_LBA] ^= 0xff; /* the primary CRC no longer matches */
	CHECK(gpt_dev_write(&d, 1, 1, s) == 0);
	CHECK(gpt_read(&d, &t) == -EBADMSG);

	/* The primary's alternate_lba points into the backup array while
	 * the backup sits at the end: reported, and repair rewrites it. */
	CHECK(fresh_table(&d) == 0);
	CHECK(patch_header(&d, 1, GH_ALT_LBA, last - 1, 8) == 0);
	CHECK(gpt_read(&d, &t) == 0);
	CHECK(t.status.backup_header_ok && t.status.backup_entries_ok);
	CHECK(t.status.backup_not_at_end);
	CHECK(gpt_repair(&d, &t) == 0);
	gpt_table_free(&t);
	CHECK(gpt_read_header(&d, 1, &h, &ok) == 0 && ok);
	CHECK_EQ_U64(h.alternate_lba, last);
	CHECK(gpt_read(&d, &t) == 0);
	CHECK(!t.status.backup_not_at_end);
	gpt_table_free(&t);

	/* gpt_wipe trusts a backup header only within the entry size
	 * bounds: an 8 KiB entry size must not make it zero the 1 MiB
	 * "array" (128 entries) it would describe, here inside the
	 * partition. */
	CHECK(fresh_table(&d) == 0);
	marker = 8 * 1024 * 1024 / d.sector_size;
	memset(s, 0xa5, d.sector_size);
	CHECK(gpt_dev_write(&d, marker, 1, s) == 0);
	CHECK(patch_header(&d, last, GH_ENTRY_SIZE, 8192, 4) == 0);
	CHECK(patch_header(&d, last, GH_ENTRIES_LBA, marker, 8) == 0);
	CHECK(gpt_wipe(&d) == 0);
	CHECK(gpt_dev_read(&d, marker, 1, s) == 0);
	CHECK(s[0] == 0xa5 && s[d.sector_size - 1] == 0xa5);
	CHECK(gpt_read(&d, &t) == -ENOENT);

	free(s);
	gpt_dev_close(&d);
	printf("headers %s (%u-byte sectors): %s\n", img, ss ? ss : 512,
	       g_fail ? "FAILED" : "passed");
	return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "unit") == 0)
		return cmd_unit();
	if (argc >= 3 && strcmp(argv[1], "build") == 0)
		return cmd_build(argv[2], arg_ss(argc, argv, 3));
	if (argc >= 3 && strcmp(argv[1], "dump") == 0)
		return cmd_dump(argv[2], arg_ss(argc, argv, 3));
	if (argc >= 3 && strcmp(argv[1], "status") == 0)
		return cmd_status(argv[2], arg_ss(argc, argv, 3));
	if (argc >= 3 && strcmp(argv[1], "repair") == 0)
		return cmd_repair(argv[2], arg_ss(argc, argv, 3));
	if (argc >= 3 && strcmp(argv[1], "mbr") == 0)
		return cmd_mbr(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "verify") == 0)
		return cmd_verify(argv[2], arg_ss(argc, argv, 3));
	if (argc >= 3 && strcmp(argv[1], "headers") == 0)
		return cmd_headers(argv[2], arg_ss(argc, argv, 3));
	fprintf(stderr, "usage: test_gpt unit|build|dump|status|repair|mbr|"
		"verify|headers IMG [SECTOR_SIZE]\n");
	return 2;
}
