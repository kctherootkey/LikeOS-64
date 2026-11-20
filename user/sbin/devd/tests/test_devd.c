// devd host unit tests: protocol encoding, names, filesystem probes,
// the filesystem table and the authorization decision.
//
// usage: test_devd <scratch directory for images>
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "devd.h"

static int g_fail, g_pass;

#define CHECK(c)                                                         \
	do {                                                             \
		if (c) {                                                 \
			g_pass++;                                        \
		} else {                                                 \
			g_fail++;                                        \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,   \
				__LINE__, #c);                           \
		}                                                        \
	} while (0)

#define CHECK_STR(a, b)                                                  \
	do {                                                             \
		const char *x_ = (a), *y_ = (b);                         \
		if (strcmp(x_, y_) == 0) {                               \
			g_pass++;                                        \
		} else {                                                 \
			g_fail++;                                        \
			fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", \
				__FILE__, __LINE__, x_, y_);             \
		}                                                        \
	} while (0)

/* ---- protocol ------------------------------------------------------------------- */

static void test_proto(void)
{
	char enc[256], dec[256], line[256];
	char *tok[DEVP_MAX_TOKENS];
	struct devp_line l;
	uint64_t v;
	int dup = 0;
	long n;

	devp_encode(enc, sizeof(enc), "My Stick", 8);
	CHECK_STR(enc, "My%20Stick");
	devp_encode(enc, sizeof(enc), "a=b%c\n\x7f\xc3\xa9", 9);
	CHECK_STR(enc, "a%3Db%25c%0A%7F%C3%A9");
	CHECK(devp_encode(enc, sizeof(enc), "", 0) == 0 && enc[0] == 0);
	/* cut at a whole escape, length still reported */
	CHECK(devp_encode(enc, 5, "  ", 2) == 6 && strcmp(enc, "%20") == 0);

	n = devp_decode(dec, sizeof(dec), "My%20Stick");
	CHECK(n == 8 && strcmp(dec, "My Stick") == 0);
	CHECK(devp_decode(dec, sizeof(dec), "%c3%A9") == 2);
	CHECK(devp_decode(dec, sizeof(dec), "%2") < 0);
	CHECK(devp_decode(dec, sizeof(dec), "%zz") < 0);
	CHECK(devp_decode(dec, sizeof(dec), "a=b") < 0);
	CHECK(devp_decode(dec, sizeof(dec), "a%b") < 0);
	CHECK(devp_decode(dec, 4, "abcd") < 0);
	CHECK(devp_decode(dec, sizeof(dec), "%00x") == 2 && dec[0] == 0);
	/* round trip of every byte */
	{
		char all[256], e[800], d[300];
		for (int i = 0; i < 256; i++)
			all[i] = (char)i;
		devp_encode(e, sizeof(e), all, 256);
		CHECK(devp_decode(d, sizeof(d), e) == 256 &&
		      memcmp(d, all, 256) == 0);
		for (char *p = e; *p; p++)
			CHECK((unsigned char)*p >= 0x21 &&
			      (unsigned char)*p <= 0x7e && *p != '=');
	}

	strcpy(line, "MOUNT sdb1");
	CHECK(devp_split(line, tok, DEVP_MAX_TOKENS) == 2 &&
	      strcmp(tok[1], "sdb1") == 0);
	strcpy(line, "MOUNT  sdb1");
	CHECK(devp_split(line, tok, DEVP_MAX_TOKENS) < 0);
	strcpy(line, " LIST");
	CHECK(devp_split(line, tok, DEVP_MAX_TOKENS) < 0);
	strcpy(line, "LIST ");
	CHECK(devp_split(line, tok, DEVP_MAX_TOKENS) < 0);
	strcpy(line, "LIST\r");
	CHECK(devp_split(line, tok, DEVP_MAX_TOKENS) < 0);
	strcpy(line, "");
	CHECK(devp_split(line, tok, DEVP_MAX_TOKENS) == 0);
	strcpy(line, "a b c d");
	CHECK(devp_split(line, tok, 3) < 0);
	strcpy(line, "FORMAT sdb1 fs=vfat label=X quick=1 cluster=0 fs=ext4");
	n = devp_split(line, tok, DEVP_MAX_TOKENS);
	CHECK(n == 7);
	CHECK_STR(devp_find(tok, (int)n, 2, "fs", &dup), "vfat");
	CHECK(dup == 1);
	dup = 0;
	CHECK_STR(devp_find(tok, (int)n, 2, "label", &dup), "X");
	CHECK(dup == 0 && devp_find(tok, (int)n, 2, "lab", NULL) == NULL);

	devp_line_begin(&l, "OK MOUNT");
	devp_line_enc(&l, "sdb1");
	devp_line_kv(&l, "path", "/media/u/My Stick");
	CHECK(devp_line_end(&l) > 0);
	CHECK(l.len && memcmp(l.buf, "OK MOUNT sdb1 path=/media/u/My%20Stick\n",
			      l.len) == 0);
	devp_line_begin(&l, "X");
	devp_line_kv_int(&l, "mountuid", -1);
	devp_line_kv_u64(&l, "size", 18446744073709551615ULL);
	devp_line_end(&l);
	CHECK(memcmp(l.buf, "X mountuid=-1 size=18446744073709551615\n",
		     l.len) == 0);
	{
		char big[3000];
		memset(big, ' ', sizeof(big) - 1);
		big[sizeof(big) - 1] = 0;
		devp_line_begin(&l, "X");
		devp_line_kv(&l, "label", big); /* 3 * 2999 bytes */
		CHECK(devp_line_end(&l) < 0);
	}

	CHECK_STR(devp_errname(EPERM), "EPERM");
	CHECK_STR(devp_errname(ENOTSUP), "ENOTSUP");
	CHECK_STR(devp_errname(12345), "EIO");
	CHECK(devp_parse_u64("4096", 1u << 24, &v) == 0 && v == 4096);
	CHECK(devp_parse_u64("-1", 100, &v) < 0);
	CHECK(devp_parse_u64("", 100, &v) < 0);
	CHECK(devp_parse_u64("101", 100, &v) < 0);
	CHECK(devp_parse_u64("18446744073709551616", UINT64_MAX, &v) < 0);
	CHECK(devp_parse_u64("1 ", 100, &v) < 0);
}

/* ---- names ----------------------------------------------------------------------- */

static void test_names(void)
{
	char out[256];

	label_display(out, sizeof(out), (const uint8_t *)"STICK      ", 11);
	CHECK_STR(out, "STICK");
	label_display(out, sizeof(out), (const uint8_t *)"A\x1b[2JB", 6);
	CHECK_STR(out, "A?[2JB");
	label_display(out, sizeof(out), (const uint8_t *)"\xc3\xa9t\xe9", 4);
	CHECK_STR(out, "\xc3\xa9t?");
	label_display(out, sizeof(out), (const uint8_t *)"ab\0cd", 5);
	CHECK_STR(out, "ab");
	label_display(out, sizeof(out), (const uint8_t *)"\xc2\x85x", 3);
	CHECK_STR(out, "?x"); /* C1 control */
	label_display(out, sizeof(out), (const uint8_t *)"\xed\xa0\x80", 3);
	CHECK_STR(out, "???"); /* surrogate */
	label_display(out, 4, (const uint8_t *)"ab\xc3\xa9", 4);
	CHECK_STR(out, "ab"); /* no half characters */

	mountname_choose(out, sizeof(out), "My Stick", "1234-ABCD", "sdb1");
	CHECK_STR(out, "My Stick");
	mountname_choose(out, sizeof(out), "../../etc", "", "sdb1");
	CHECK_STR(out, "_.._etc");
	mountname_choose(out, sizeof(out), "..", "1234-ABCD", "sdb1");
	CHECK_STR(out, "1234-ABCD");
	mountname_choose(out, sizeof(out), "...", "", "sdb1");
	CHECK_STR(out, "sdb1");
	mountname_choose(out, sizeof(out), ".hidden", "", "sdb1");
	CHECK_STR(out, "hidden");
	mountname_choose(out, sizeof(out), "a/b\\c\nd", "", "sdb1");
	CHECK_STR(out, "a_b_c?d");
	mountname_choose(out, sizeof(out), "", "", "sdb1");
	CHECK_STR(out, "sdb1");
	mountname_choose(out, sizeof(out), "   ", "", "");
	CHECK_STR(out, "disk");
	{
		char lab[200];
		memset(lab, 'x', 150);
		lab[150] = 0;
		mountname_choose(out, sizeof(out), lab, "", "sdb1");
		CHECK(strlen(out) == DEVD_MNTNAME_MAX);
		/* 2-byte characters: never cut in half */
		for (int i = 0; i < 100; i += 2) {
			lab[i] = (char)0xc3;
			lab[i + 1] = (char)0xa9;
		}
		lab[100] = 0;
		mountname_choose(out, sizeof(out), lab, "", "sdb1");
		CHECK(strlen(out) == 64 && utf8_text_ok(out, strlen(out)));
		lab[0] = 'x';
		mountname_choose(out, sizeof(out), lab, "", "sdb1");
		CHECK(strlen(out) <= 64 && utf8_text_ok(out, strlen(out)));
		mountname_suffix(out, sizeof(out), "\xc3\xa9\xc3\xa9", 1);
		CHECK_STR(out, "\xc3\xa9\xc3\xa9" "1");
		memset(lab, 'y', 64);
		lab[64] = 0;
		mountname_suffix(out, sizeof(out), lab, 12);
		CHECK(strlen(out) == 64 && strcmp(out + 62, "12") == 0);
		for (int i = 0; i < 64; i += 2) {
			lab[i] = (char)0xc3;
			lab[i + 1] = (char)0xa9;
		}
		mountname_suffix(out, sizeof(out), lab, 7);
		CHECK(strlen(out) == 63 && utf8_text_ok(out, 63));
	}

	CHECK(username_ok("kcope") && username_ok("a.b-c_1"));
	CHECK(!username_ok("") && !username_ok(".") && !username_ok("..") &&
	      !username_ok("-x") && !username_ok("a/b") &&
	      !username_ok("a b") && !username_ok("\xc3\xa9"));
	CHECK(!username_ok("abcdefghijklmnopqrstuvwxyz0123456"));
	CHECK(devname_ok("sdb1") && devname_ok("nvme0n1p2"));
	CHECK(!devname_ok("../sda") && !devname_ok("a/b") && !devname_ok("") &&
	      !devname_ok(".x"));
	CHECK(utf8_text_ok("abc\xc3\xa9", 5) && !utf8_text_ok("a\tb", 3) &&
	      !utf8_text_ok("\xc3", 1) && !utf8_text_ok("\xc0\xaf", 2));
}

/* ---- filesystem table ----------------------------------------------------------- */

static void test_fstab(void)
{
	const struct fs_type *v = fs_by_id("vfat"), *e = fs_by_id("ext4");
	char out[64], store[1024], opts[128];
	char *argv[24];
	struct probe_result pr;
	size_t n;
	int argc;

	CHECK(v && e && fs_by_id("ntfs") && !fs_by_id("xfs"));
	fs_table(&n);
	CHECK(n == 6);
	CHECK(v->is_default && !e->is_default && v->labelmax == 11 &&
	      e->labelmax == 16);
	CHECK(!fs_by_id("ntfs")->mkfs && !fs_by_id("ntfs")->kernel_type);

	CHECK(v->label_ok("my stick", 8, out, sizeof(out)) == 0);
	CHECK_STR(out, "MY STICK");
	CHECK(v->label_ok("ABCDEFGHIJKL", 12, out, sizeof(out)) < 0);
	CHECK(v->label_ok("A/B", 3, out, sizeof(out)) < 0);
	CHECK(v->label_ok("A*B", 3, out, sizeof(out)) < 0);
	CHECK(v->label_ok(" AB", 3, out, sizeof(out)) < 0);
	CHECK(v->label_ok("\xc3\xa9", 2, out, sizeof(out)) < 0);
	CHECK(v->label_ok("AB  ", 4, out, sizeof(out)) == 0 &&
	      strcmp(out, "AB") == 0);
	CHECK(v->label_ok("", 0, out, sizeof(out)) == 0 && out[0] == 0);
	CHECK(e->label_ok("Daten \xc3\xa9", 8, out, sizeof(out)) == 0);
	CHECK(e->label_ok("12345678901234567", 17, out, sizeof(out)) < 0);
	CHECK(e->label_ok("a/b", 3, out, sizeof(out)) < 0);
	CHECK(e->label_ok("a\nb", 3, out, sizeof(out)) < 0);
	CHECK(e->label_ok("\xff", 1, out, sizeof(out)) < 0);

	CHECK(fs_cluster_ok(v, 0) && fs_cluster_ok(v, 32768) &&
	      !fs_cluster_ok(v, 3000) && !fs_cluster_ok(e, 512));

	v->mount_opts(opts, sizeof(opts), 1000, 100);
	CHECK_STR(opts, "uid=1000,gid=100,fmask=0177,dmask=0077,shortname=mixed");
	e->mount_opts(opts, sizeof(opts), 1000, 100);
	CHECK_STR(opts, "");

	argc = v->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb", "STICK",
			    4096, 512, 1, 1000, 100);
	CHECK(argc == 9);
	if (argc == 9) {
		CHECK_STR(argv[0], "/sbin/mkfs.fat");
		CHECK_STR(argv[4], "STICK");
		CHECK_STR(argv[6], "8");
		CHECK_STR(argv[7], "-I");
		CHECK_STR(argv[8], "/dev/sdb");
		CHECK(argv[9] == NULL);
	}
	argc = v->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "", 0,
			    512, 0, 0, 0);
	CHECK(argc == 4 && strcmp(argv[3], "/dev/sdb1") == 0);
	CHECK(v->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "", 512,
			   4096, 0, 0, 0) < 0);
	CHECK(v->mkfs_argv(argv, 4, store, sizeof(store), "/dev/sdb1", "X",
			   4096, 512, 1, 0, 0) < 0);
	CHECK(v->mkfs_argv(argv, 24, store, 20, "/dev/sdb1", "X", 4096, 512, 1,
			   0, 0) < 0);
	argc = e->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1",
			    "Daten", 4096, 512, 0, 1000, 100);
	CHECK(argc == 12);
	if (argc == 12) {
		CHECK_STR(argv[0], "/sbin/mke2fs");
		CHECK_STR(argv[10], "root_owner=1000:100");
		CHECK_STR(argv[11], "/dev/sdb1");
		CHECK(argv[12] == NULL);
	}

	memset(&pr, 0, sizeof(pr));
	strcpy(pr.fstype, "vfat");
	pr.fat_bits = 32;
	CHECK(fs_supported(&pr));
	pr.fat_bits = 16;
	CHECK(!fs_supported(&pr));
	strcpy(pr.fstype, "ext4");
	CHECK(fs_supported(&pr));
	strcpy(pr.fstype, "ntfs");
	CHECK(!fs_supported(&pr));
	strcpy(pr.fstype, "unknown");
	CHECK(!fs_supported(&pr));
}

/* ---- authorization ------------------------------------------------------------- */

static void test_auth(void)
{
	CHECK(auth_console_decide(0, NULL, NULL, NULL));
	CHECK(auth_console_decide(1000, "1000\n", "7 1000\n", "7\n"));
	CHECK(auth_console_decide(1000, "1000", "7 1000", "7"));
	CHECK(!auth_console_decide(1001, "1000\n", "7 1000\n", "7\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "7 1000\n", "8\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "7 1001\n", "7\n"));
	CHECK(!auth_console_decide(1000, "1001\n", "7 1000\n", "7\n"));
	CHECK(!auth_console_decide(1000, "", "7 1000\n", "7\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "", "7\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "4294967295 1000\n",
				   "4294967295\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "7  1000\n", "7\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "7 1000\n\n", "7\n"));
	CHECK(!auth_console_decide(1000, "+1000\n", "7 1000\n", "7\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "7 1000\n", "7x\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "7 1000\n",
				   "000000000007\n"));
	CHECK(!auth_console_decide(1000, "1000\n", "7 1000\n", "-7\n"));
	CHECK(!auth_console_decide(1000, NULL, "7 1000\n", "7\n"));
	CHECK(!auth_console_decide(1000, "99999999999\n", "7 1000\n", "7\n"));
}

/* ---- probes on crafted buffers -------------------------------------------------- */

struct mem_ctx {
	const uint8_t *buf;
	size_t len;
};

static int mem_read(void *ctx, uint64_t off, void *dst, size_t n)
{
	struct mem_ctx *m = ctx;

	if (off > m->len || n > m->len - off)
		return -1;
	memcpy(dst, m->buf + off, n);
	return 0;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static void test_probe_crafted(void)
{
	static uint8_t img[PROBE_BYTES];
	struct probe_result pr;
	struct mem_ctx m = { img, sizeof(img) };

	/* nothing */
	memset(img, 0, sizeof(img));
	probe_buffer(img, sizeof(img), mem_read, &m, &pr);
	CHECK_STR(pr.fstype, "unknown");

	/* ext4 with a hostile label */
	memset(img, 0, sizeof(img));
	put16(img + 1024 + 0x38, 0xEF53);
	memcpy(img + 1024 + 0x78, "\x1b]0;pwn\x07/../x", 13);
	img[1024 + 0x68] = 0xab;
	probe_buffer(img, sizeof(img), NULL, NULL, &pr);
	CHECK_STR(pr.fstype, "ext4");
	CHECK_STR(pr.label, "?]0;pwn?/../x");
	CHECK(strlen(pr.uuid) == 36);

	/* NTFS, exFAT */
	memset(img, 0, sizeof(img));
	memcpy(img + 3, "NTFS    ", 8);
	probe_buffer(img, sizeof(img), NULL, NULL, &pr);
	CHECK_STR(pr.fstype, "ntfs");
	memcpy(img + 3, "EXFAT   ", 8);
	put32(img + 0x64, 0x1234ABCD);
	probe_buffer(img, sizeof(img), NULL, NULL, &pr);
	CHECK_STR(pr.fstype, "exfat");
	CHECK_STR(pr.uuid, "1234-ABCD");

	/* FAT16 with the label in its root directory */
	memset(img, 0, sizeof(img));
	put16(img + 0x0B, 512);
	img[0x0D] = 4;      /* sectors per cluster */
	put16(img + 0x0E, 1); /* reserved */
	img[0x10] = 2;
	put16(img + 0x11, 512); /* root entries: 32 sectors */
	put16(img + 0x13, 0);
	put32(img + 0x20, 40000);
	put16(img + 0x16, 40); /* sectors per FAT */
	img[0x24 + 2] = 0x29;
	put32(img + 0x24 + 3, 0xCAFEBABE);
	memcpy(img + 0x24 + 7, "NO NAME    ", 11);
	memcpy(img + 0x36, "FAT16   ", 8);
	img[510] = 0x55;
	img[511] = 0xAA;
	{
		uint8_t *root = img + (1 + 2 * 40) * 512;
		/* a deleted entry and a long-name part come first */
		root[0] = 0xE5;
		root[11] = 0x08;
		root[32] = 'A';
		root[32 + 11] = 0x0F;
		memcpy(root + 64, "BACKUP 2024", 11);
		root[64 + 11] = 0x08;
	}
	probe_buffer(img, sizeof(img), mem_read, &m, &pr);
	CHECK_STR(pr.fstype, "vfat");
	CHECK(pr.fat_bits == 16);
	CHECK_STR(pr.label, "BACKUP 2024");
	CHECK_STR(pr.uuid, "CAFE-BABE");
	/* without the root directory: the boot sector's "NO NAME" = none */
	probe_buffer(img, sizeof(img), NULL, NULL, &pr);
	CHECK_STR(pr.label, "");
	/* insane geometry is not FAT */
	img[0x0D] = 3;
	probe_buffer(img, sizeof(img), mem_read, &m, &pr);
	CHECK(strcmp(pr.fstype, "vfat") != 0);

	/* FAT32 whose root cluster points far outside the volume */
	memset(img, 0, sizeof(img));
	put16(img + 0x0B, 512);
	img[0x0D] = 1;
	put16(img + 0x0E, 32);
	img[0x10] = 2;
	put32(img + 0x20, 200000);
	put32(img + 0x24, 1600);
	put32(img + 0x2C, 0xFFFFFFF0);
	memcpy(img + 0x52, "FAT32   ", 8);
	img[510] = 0x55;
	img[511] = 0xAA;
	probe_buffer(img, sizeof(img), mem_read, &m, &pr);
	CHECK_STR(pr.fstype, "vfat");
	CHECK(pr.fat_bits == 32 && pr.label[0] == 0);

	/* swap and ISO 9660 */
	memset(img, 0, sizeof(img));
	memcpy(img + 4096 - 10, "SWAPSPACE2", 10);
	memcpy(img + 1024 + 28, "myswap", 6);
	probe_buffer(img, sizeof(img), NULL, NULL, &pr);
	CHECK_STR(pr.fstype, "swap");
	CHECK_STR(pr.label, "myswap");
	memset(img, 0, sizeof(img));
	memcpy(img + 0x8001, "CD001", 5);
	memcpy(img + 0x8028, "LIKEOS_LIVE                     ", 32);
	memcpy(img + 0x8000 + 813, "2026112012345600", 16);
	probe_buffer(img, sizeof(img), NULL, NULL, &pr);
	CHECK_STR(pr.fstype, "iso9660");
	CHECK_STR(pr.label, "LIKEOS_LIVE");
	CHECK_STR(pr.uuid, "2026-11-20-12-34-56-00");

	/* short buffers never read past their end (ASan watches) */
	for (size_t len = 0; len < 3000; len += 37) {
		uint8_t *b = malloc(len ? len : 1);
		struct mem_ctx mc = { b, len };
		memcpy(b, img, len);
		probe_buffer(b, len, mem_read, &mc, &pr);
		free(b);
	}

	/* random FAT-looking garbage */
	srand(1);
	for (int it = 0; it < 3000; it++) {
		for (size_t i = 0; i < 1024; i++)
			img[i] = (uint8_t)rand();
		memcpy(img + (it & 1 ? 0x52 : 0x36), it & 1 ? "FAT32   " :
							       "FAT16   ", 8);
		img[510] = 0x55;
		img[511] = 0xAA;
		probe_buffer(img, sizeof(img), mem_read, &m, &pr);
		CHECK(utf8_text_ok(pr.label, strlen(pr.label)));
	}
}

/* ---- real images made with the build machine's tools ---------------------------- */

static int have(const char *path)
{
	return access(path, X_OK) == 0;
}

static int make_image(const char *path, off_t size)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	int r;

	if (fd < 0)
		return -1;
	r = ftruncate(fd, size);
	close(fd);
	return r;
}

static int run(char **argv)
{
	pid_t pid = fork();
	int st = 0;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		dup2(null, 0);
		dup2(null, 1);
		dup2(null, 2);
		execv(argv[0], argv);
		_exit(127);
	}
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_images(const char *dir)
{
	char path[512], store[1024];
	char *argv[24];
	struct probe_result pr;
	const struct fs_type *t;

	/* FAT32, made with exactly the arguments devd would pass */
	t = fs_by_id("vfat");
	snprintf(path, sizeof(path), "%s/fat32.img", dir);
	if (have("/sbin/mkfs.fat") && make_image(path, 64 << 20) == 0 &&
	    t->mkfs_argv(argv, 24, store, sizeof(store), path, "MY STICK", 512,
			 512, 1, 1000, 1000) > 0) {
		CHECK(run(argv) == 0);
		CHECK(probe_device(path, &pr) == 0);
		CHECK_STR(pr.fstype, "vfat");
		CHECK(pr.fat_bits == 32);
		CHECK_STR(pr.label, "MY STICK");
		CHECK(strlen(pr.uuid) == 9);
		CHECK(fs_supported(&pr));
		/* the label other systems change is the root directory's */
		{
			int fd = open(path, O_RDWR);
			if (fd >= 0) {
				CHECK(pwrite(fd, "NO NAME    ", 11, 0x47) == 11);
				close(fd);
			}
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.label, "MY STICK");
		}
	} else {
		printf("  (no mkfs.fat: FAT32 image skipped)\n");
	}

	/* ext4 */
	t = fs_by_id("ext4");
	snprintf(path, sizeof(path), "%s/ext4.img", dir);
	if (have("/sbin/mke2fs") && make_image(path, 32 << 20) == 0 &&
	    t->mkfs_argv(argv, 24, store, sizeof(store), path,
			 "Daten \xc3\xa9", 4096, 512, 0, 1000, 1000) > 0) {
		CHECK(run(argv) == 0);
		CHECK(probe_device(path, &pr) == 0);
		CHECK_STR(pr.fstype, "ext4");
		CHECK_STR(pr.label, "Daten \xc3\xa9");
		CHECK(strlen(pr.uuid) == 36);
	} else {
		printf("  (no mke2fs: ext4 image skipped)\n");
	}

	snprintf(path, sizeof(path), "%s/swap.img", dir);
	if (have("/usr/sbin/mkswap") && make_image(path, 4 << 20) == 0) {
		char *a[] = { "/usr/sbin/mkswap", "-L", "sw1", path, NULL };
		CHECK(run(a) == 0);
		CHECK(probe_device(path, &pr) == 0);
		CHECK_STR(pr.fstype, "swap");
		CHECK_STR(pr.label, "sw1");
	}

	snprintf(path, sizeof(path), "%s/ntfs.img", dir);
	if (have("/usr/sbin/mkfs.ntfs") && make_image(path, 16 << 20) == 0) {
		char *a[] = { "/usr/sbin/mkfs.ntfs", "-F", "-f", "-q", "-L",
			      "Win", path, NULL };
		if (run(a) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.fstype, "ntfs");
		}
	}

	snprintf(path, sizeof(path), "%s/cd.iso", dir);
	if (have("/usr/bin/genisoimage")) {
		char src[512];
		char *a[] = { "/usr/bin/genisoimage", "-quiet", "-V", "MYCD",
			      "-o", path, src, NULL };
		snprintf(src, sizeof(src), "%s/isosrc", dir);
		mkdir(src, 0700);
		if (run(a) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.fstype, "iso9660");
			CHECK_STR(pr.label, "MYCD");
		}
	}

	CHECK(probe_device("/nonexistent/devd-test", &pr) < 0);
}

int main(int argc, char **argv)
{
	g_log_level = LOG_ERR;
	test_proto();
	test_names();
	test_fstab();
	test_auth();
	test_probe_crafted();
	if (argc > 1)
		test_images(argv[1]);
	printf("devd tests: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
