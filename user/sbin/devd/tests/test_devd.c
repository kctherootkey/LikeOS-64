// devd host unit tests: protocol encoding, names, filesystem probes (with
// the exFAT, NTFS and Joliet labels), partition tables, the filesystem
// table, the device rules (policy.c), the volume agent's automount rule,
// the authorization decision and cancelling a format (the request, the
// replies, who may, and whether the volume was written to).
//
// usage: test_devd <scratch directory for images>
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "devd.h"

#define EXFAT_TEST_LABEL 0x83

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
	CHECK_STR(devp_errname(EUCLEAN), "EUCLEAN");

	/* MOUNT <name> [recover] */
	{
		int rec = -1;

		strcpy(line, "MOUNT sdb1");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(devp_mount_parse(tok, (int)n, &rec) == 0 && rec == 0);
		rec = -1;
		strcpy(line, "MOUNT sdb1 recover");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(n == 3 && devp_mount_parse(tok, (int)n, &rec) == 0 &&
		      rec == 1);
		strcpy(line, "MOUNT sdb1 force");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(devp_mount_parse(tok, (int)n, &rec) < 0);
		strcpy(line, "MOUNT sdb1 recover=1");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(devp_mount_parse(tok, (int)n, &rec) < 0);
		strcpy(line, "MOUNT sdb1 recover recover");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(devp_mount_parse(tok, (int)n, &rec) < 0);
		strcpy(line, "MOUNT");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(devp_mount_parse(tok, (int)n, &rec) < 0);
		strcpy(line, "UNMOUNT sdb1 recover");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(devp_mount_parse(tok, (int)n, &rec) < 0);
		strcpy(line, "MOUNT recover");
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		CHECK(devp_mount_parse(tok, (int)n, &rec) == 0 && rec == 0 &&
		      strcmp(tok[1], "recover") == 0);
	}
	CHECK(devp_parse_u64("4096", 1u << 24, &v) == 0 && v == 4096);
	CHECK(devp_parse_u64("-1", 100, &v) < 0);
	CHECK(devp_parse_u64("", 100, &v) < 0);
	CHECK(devp_parse_u64("101", 100, &v) < 0);
	CHECK(devp_parse_u64("18446744073709551616", UINT64_MAX, &v) < 0);
	CHECK(devp_parse_u64("1 ", 100, &v) < 0);
}

/* ---- cancelling a format ------------------------------------------------------------ */

/* The marks a formatting child sends, in order, fed to the same decision
 * devd makes: was the volume written to when the job was stopped? */
static int written_after(const char *const *marks, int n)
{
	int written = 0;

	for (int i = 0; i < n; i++) {
		int pct = -1;

		if (format_mark_writes(format_mark_parse(marks[i], &pct)))
			written = 1;
	}
	return written;
}

static void test_cancel(void)
{
	char line[256], *tok[DEVP_MAX_TOKENS];
	struct devp_line l;
	int n, pct = -1;

	/* the request */
	strcpy(line, "CANCEL sdb1");
	n = devp_split(line, tok, DEVP_MAX_TOKENS);
	CHECK(n == 2 && devp_cancel_parse(tok, n) == 0 &&
	      strcmp(tok[1], "sdb1") == 0);
	strcpy(line, "CANCEL");
	n = devp_split(line, tok, DEVP_MAX_TOKENS);
	CHECK(devp_cancel_parse(tok, n) < 0);
	strcpy(line, "CANCEL sdb1 now");
	n = devp_split(line, tok, DEVP_MAX_TOKENS);
	CHECK(devp_cancel_parse(tok, n) < 0);
	strcpy(line, "FORMAT sdb1");
	n = devp_split(line, tok, DEVP_MAX_TOKENS);
	CHECK(devp_cancel_parse(tok, n) < 0);
	strcpy(line, "cancel sdb1");
	n = devp_split(line, tok, DEVP_MAX_TOKENS);
	CHECK(devp_cancel_parse(tok, n) < 0);

	/* the replies, as devd writes them */
	devp_line_begin(&l, "OK");
	devp_line_word(&l, "CANCEL");
	devp_line_enc(&l, "sdb1");
	CHECK(devp_line_end(&l) > 0 && l.len == 15 &&
	      memcmp(l.buf, "OK CANCEL sdb1\n", 15) == 0);
	CHECK_STR(devp_errname(ECANCELED), "ECANCELED");
	devp_line_begin(&l, "ERR");
	devp_line_enc(&l, "FORMAT");
	devp_line_enc(&l, "sdb1");
	devp_line_word(&l, devp_errname(ECANCELED));
	devp_line_enc(&l, format_cancel_message(0, 0));
	{
		static const char want[] =
			"ERR FORMAT sdb1 ECANCELED Formatting%20was%20"
			"cancelled.%20The%20drive%20was%20not%20changed.\n";

		CHECK(devp_line_end(&l) == (long)strlen(want) &&
		      memcmp(l.buf, want, strlen(want)) == 0);
	}
	devp_line_begin(&l, "ERR");
	devp_line_enc(&l, "CANCEL");
	devp_line_enc(&l, "sdb1");
	devp_line_word(&l, devp_errname(ENOENT));
	devp_line_enc(&l, "no format of this drive is running");
	CHECK(devp_line_end(&l) > 0 &&
	      memcmp(l.buf, "ERR CANCEL sdb1 ENOENT no%20format", 34) == 0);

	/* who may */
	CHECK(format_cancel_allowed(1000, 1000));
	CHECK(format_cancel_allowed(0, 1000));
	CHECK(format_cancel_allowed(0, 0));
	CHECK(!format_cancel_allowed(1001, 1000));
	CHECK(!format_cancel_allowed(1000, 0));

	/* the child's marks */
	CHECK(format_mark_parse("P42", &pct) == FMARK_PROGRESS && pct == 42);
	CHECK(format_mark_parse("P0", &pct) == FMARK_PROGRESS && pct == 0);
	CHECK(format_mark_parse("P100", &pct) == FMARK_NONE);
	CHECK(format_mark_parse("P", &pct) == FMARK_NONE);
	CHECK(format_mark_parse("P-1", &pct) == FMARK_NONE);
	CHECK(format_mark_parse("W", &pct) == FMARK_WRITE);
	CHECK(format_mark_parse("M", &pct) == FMARK_MKFS);
	CHECK(format_mark_parse("WM", &pct) == FMARK_NONE);
	CHECK(format_mark_parse("", &pct) == FMARK_NONE);
	CHECK(!format_mark_writes(FMARK_PROGRESS) &&
	      !format_mark_writes(FMARK_NONE) &&
	      format_mark_writes(FMARK_WRITE) && format_mark_writes(FMARK_MKFS));

	/* written or not, in every phase */
	{
		/* stopped before the child said anything */
		CHECK(!written_after(NULL, 0));
		/* a quick format of a partition: nothing until mkfs */
		static const char *const quick_pre[] = { "P5" };
		static const char *const quick_mkfs[] = { "P5", "M" };
		/* a quick format of a whole drive: its ends wiped first */
		static const char *const whole_wipe[] = { "W" };
		static const char *const whole_mkfs[] = { "W", "P5", "M" };
		/* a full format: zeros from the start */
		static const char *const full_zero[] = { "W", "P1", "P2" };
		static const char *const full_mkfs[] = { "W", "P1", "P89",
							 "P90", "M" };

		CHECK(!written_after(quick_pre, 1));
		CHECK(written_after(quick_mkfs, 2));
		CHECK(written_after(whole_wipe, 1));
		CHECK(written_after(whole_mkfs, 3));
		CHECK(written_after(full_zero, 1));
		CHECK(written_after(full_zero, 3));
		CHECK(written_after(full_mkfs, 5));
	}

	/* the messages */
	CHECK_STR(format_cancel_message(0, 0),
		  "Formatting was cancelled. The drive was not changed.");
	CHECK_STR(format_cancel_message(0, 1),
		  "Formatting was cancelled. The drive was not changed.");
	CHECK_STR(format_cancel_message(1, 1),
		  "Formatting was cancelled. The drive must be formatted "
		  "before it can be used.");
	CHECK(strstr(format_cancel_message(1, 0), "must be formatted") &&
	      strstr(format_cancel_message(1, 0), "could not be cleared"));
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

/* argv[0..n-1] equal to the strings in `want' (NULL-terminated), and
 * argv[n] == NULL */
static int argv_is(char **argv, int argc, const char *const *want)
{
	int n = 0;

	while (want[n])
		n++;
	if (argc != n || argv[n] != NULL)
		return 0;
	for (int i = 0; i < n; i++)
		if (strcmp(argv[i], want[i]) != 0)
			return 0;
	return 1;
}

/* The exFAT and NTFS entries: labels, allocation unit sizes, mkfs argv. */
static void test_fstab_exfat_ntfs(void)
{
	const struct fs_type *x = fs_by_id("exfat"), *nt = fs_by_id("ntfs");
	char out[400], store[1024], lab[600];
	char *argv[24];
	int argc;

	CHECK(x && nt);
	if (!x || !nt)
		return;
	CHECK(x->mkfs_argv && x->label_ok && nt->mkfs_argv && nt->label_ok);
	CHECK(x->labelmax == 11 && nt->labelmax == 128);
	CHECK(x->defcluster == 0 && nt->defcluster == 4096);

	/* exFAT labels: 11 UTF-16 code units */
	CHECK(x->label_ok("", 0, out, sizeof(out)) == 0 && out[0] == 0);
	CHECK(x->label_ok("My Stick 01", 11, out, sizeof(out)) == 0);
	CHECK_STR(out, "My Stick 01");
	CHECK(x->label_ok("My Stick 012", 12, out, sizeof(out)) < 0);
	/* 11 x U+00FC (2 bytes each): 11 units, 22 bytes */
	strcpy(lab, "");
	for (int i = 0; i < 11; i++)
		strcat(lab, "\xc3\xbc");
	CHECK(x->label_ok(lab, 22, out, sizeof(out)) == 0);
	CHECK_STR(out, lab);
	strcat(lab, "a");
	CHECK(x->label_ok(lab, 23, out, sizeof(out)) < 0);
	/* U+1F600 is two units: 5 of them (10) + 1 fit, + 2 do not */
	strcpy(lab, "");
	for (int i = 0; i < 5; i++)
		strcat(lab, "\xf0\x9f\x98\x80");
	CHECK(x->label_ok(lab, 20, out, sizeof(out)) == 0);
	strcat(lab, "a");
	CHECK(x->label_ok(lab, 21, out, sizeof(out)) == 0);
	strcat(lab, "b");
	CHECK(x->label_ok(lab, 22, out, sizeof(out)) < 0);
	/* the characters a name on the volume may not contain, controls */
	for (const char *b = "\"*/:<>?\\|"; *b; b++) {
		char one[4] = { 'a', *b, 'z', 0 };
		CHECK(x->label_ok(one, 3, out, sizeof(out)) < 0);
		CHECK(nt->label_ok(one, 3, out, sizeof(out)) < 0);
	}
	CHECK(x->label_ok("a\tb", 3, out, sizeof(out)) < 0);
	CHECK(x->label_ok("a\x7f", 2, out, sizeof(out)) < 0);
	CHECK(x->label_ok("a\xc2\x85", 3, out, sizeof(out)) < 0); /* C1 NEL */
	CHECK(x->label_ok("\xff", 1, out, sizeof(out)) < 0);
	CHECK(x->label_ok("\xc3", 1, out, sizeof(out)) < 0);
	CHECK(x->label_ok("\xed\xa0\x80", 3, out, sizeof(out)) < 0);
	CHECK(x->label_ok("\xc0\xaf", 2, out, sizeof(out)) < 0);
	/* other punctuation and a leading dash are fine (the label is -L's
	 * own argument) */
	CHECK(x->label_ok("-a.b,c+d=[]", 11, out, sizeof(out)) == 0);
	CHECK(nt->label_ok("-a.b,c+d=[];'", 13, out, sizeof(out)) == 0);
	/* the output buffer bounds the label */
	CHECK(x->label_ok("abc", 3, out, 3) < 0);
	CHECK(x->label_ok("abc", 3, out, 4) == 0);

	/* NTFS labels: 128 UTF-16 code units */
	memset(lab, 'n', 128);
	lab[128] = 0;
	CHECK(nt->label_ok(lab, 128, out, sizeof(out)) == 0);
	CHECK(strlen(out) == 128);
	lab[128] = 'n';
	lab[129] = 0;
	CHECK(nt->label_ok(lab, 129, out, sizeof(out)) < 0);
	/* 128 x U+20AC (3 bytes each): 384 bytes, still 128 units */
	strcpy(lab, "");
	for (int i = 0; i < 128; i++)
		strcat(lab, "\xe2\x82\xac");
	CHECK(nt->label_ok(lab, 384, out, sizeof(out)) == 0);
	CHECK(strlen(out) == 384);
	CHECK(nt->label_ok(lab, 384, out, 384) < 0);
	strcat(lab, "x");
	CHECK(nt->label_ok(lab, 385, out, sizeof(out)) < 0);
	CHECK(nt->label_ok("Daten \xc3\xa4", 8, out, sizeof(out)) == 0);
	CHECK(nt->label_ok("a\nb", 3, out, sizeof(out)) < 0);
	CHECK(nt->label_ok("\xf5\x80\x80\x80", 4, out, sizeof(out)) < 0);

	/* allocation unit sizes offered */
	CHECK(!fs_cluster_ok(x, 512) && !fs_cluster_ok(x, 2048) &&
	      fs_cluster_ok(x, 4096) && fs_cluster_ok(x, 131072) &&
	      fs_cluster_ok(x, 32u << 20) && !fs_cluster_ok(x, 64u << 20) &&
	      !fs_cluster_ok(x, 12288) && fs_cluster_ok(x, 0));
	CHECK(fs_cluster_ok(nt, 512) && fs_cluster_ok(nt, 4096) &&
	      fs_cluster_ok(nt, 65536) && fs_cluster_ok(nt, 2u << 20) &&
	      !fs_cluster_ok(nt, 256) && !fs_cluster_ok(nt, 4u << 20) &&
	      !fs_cluster_ok(nt, 3072) && fs_cluster_ok(nt, 0));
	{
		int nx = 0, nn = 0;
		for (const uint32_t *c = x->clusters; *c; c++, nx++)
			CHECK(*c >= 4096 && !(*c & (*c - 1)));
		for (const uint32_t *c = nt->clusters; *c; c++, nn++)
			CHECK(*c >= 512 && !(*c & (*c - 1)));
		CHECK(nx == 14 && nn == 13);
	}

	/* exFAT argv */
	argc = x->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1",
			    "Stick", 0, 512, 0, 1000, 100);
	{
		static const char *const w[] = { "/sbin/mkfs.exfat", "-q", "-L",
						 "Stick", "-P", "none",
						 "/dev/sdb1", NULL };
		CHECK(argv_is(argv, argc, w));
	}
	argc = x->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb", "",
			    131072, 512, 1, 0, 0);
	{
		static const char *const w[] = { "/sbin/mkfs.exfat", "-q", "-c",
						 "131072", "-P", "none",
						 "/dev/sdb", NULL };
		CHECK(argv_is(argv, argc, w));
	}
	argc = x->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdc1",
			    "-x", 33554432, 4096, 0, 0, 0);
	{
		static const char *const w[] = { "/sbin/mkfs.exfat", "-q", "-L",
						 "-x", "-c", "33554432", "-P",
						 "none", "/dev/sdc1", NULL };
		CHECK(argv_is(argv, argc, w));
	}
	/* sizes the tool cannot make, or smaller than a sector */
	CHECK(x->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			   2048, 512, 0, 0, 0) < 0);
	CHECK(x->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			   64u << 20, 512, 0, 0, 0) < 0);
	CHECK(x->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			   12288, 512, 0, 0, 0) < 0);
	CHECK(x->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			   4096, 8192, 0, 0, 0) < 0);
	/* argv and store limits */
	CHECK(x->mkfs_argv(argv, 7, store, sizeof(store), "/dev/sdb1", "X", 0,
			   512, 0, 0, 0) < 0);
	CHECK(x->mkfs_argv(argv, 8, store, sizeof(store), "/dev/sdb1", "X", 0,
			   512, 0, 0, 0) == 7);
	CHECK(x->mkfs_argv(argv, 24, store, 40, "/dev/sdb1", "X", 0, 512, 0,
			   0, 0) < 0);
	/* each argument is stored once: 43 bytes hold all seven, and no
	 * argument comes back NULL when the store is nearly full */
	argc = x->mkfs_argv(argv, 24, store, 43, "/dev/sdb1", "X", 0, 512, 0,
			    0, 0);
	CHECK(argc == 7);
	for (int i = 0; i < argc; i++)
		CHECK(argv[i] != NULL);
	for (size_t cap = 0; cap < 43; cap++) {
		argc = x->mkfs_argv(argv, 24, store, cap, "/dev/sdb1", "X", 0,
				    512, 0, 0, 0);
		CHECK(argc < 0);
	}

	/* NTFS argv: always fast (devd's zero pass is the full format), no
	 * -F, also for a whole disk */
	argc = nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1",
			     "Daten", 0, 512, 0, 1000, 100);
	{
		static const char *const w[] = { "/sbin/mkntfs", "-q", "-f",
						 "-L", "Daten", "/dev/sdb1",
						 NULL };
		CHECK(argv_is(argv, argc, w));
	}
	argc = nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb", "",
			     65536, 512, 1, 0, 0);
	{
		static const char *const w[] = { "/sbin/mkntfs", "-q", "-f", "-c",
						 "65536", "/dev/sdb", NULL };
		CHECK(argv_is(argv, argc, w));
	}
	for (int i = 0; i < argc; i++)
		CHECK(strcmp(argv[i], "-F") != 0);
	argc = nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1",
			     "", 2u << 20, 4096, 0, 0, 0);
	CHECK(argc == 6 && strcmp(argv[4], "2097152") == 0);
	CHECK(nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			    512, 4096, 0, 0, 0) < 0);
	CHECK(nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			    256, 512, 0, 0, 0) < 0);
	CHECK(nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			    4u << 20, 512, 0, 0, 0) < 0);
	CHECK(nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			    6144, 512, 0, 0, 0) < 0);
	/* 4096 sectors per cluster at most: 2 MiB needs 512-byte sectors */
	CHECK(nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", "",
			    2u << 20, 256, 0, 0, 0) < 0);
	/* a 384-byte label fits the store devd uses */
	strcpy(lab, "");
	for (int i = 0; i < 128; i++)
		strcat(lab, "\xe2\x82\xac");
	argc = nt->mkfs_argv(argv, 24, store, sizeof(store), "/dev/sdb1", lab,
			     4096, 512, 0, 0, 0);
	CHECK(argc == 8 && strcmp(argv[4], lab) == 0);
	CHECK(nt->mkfs_argv(argv, 6, store, sizeof(store), "/dev/sdb1", "X", 0,
			    512, 0, 0, 0) < 0);
	CHECK(nt->mkfs_argv(argv, 24, store, 30, "/dev/sdb1", "X", 0, 512, 0,
			    0, 0) < 0);
}

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
	CHECK(!fs_by_id("iso9660")->mkfs && !fs_by_id("swap")->mkfs);
	CHECK_STR(fs_by_id("exfat")->mkfs, "/sbin/mkfs.exfat");
	CHECK_STR(fs_by_id("ntfs")->mkfs, "/sbin/mkntfs");
	CHECK_STR(fs_by_id("ntfs")->kernel_type, "ntfs");
	CHECK_STR(fs_by_id("exfat")->kernel_type, "exfat");
	CHECK_STR(fs_by_id("iso9660")->kernel_type, "iso9660");
	CHECK(!fs_by_id("swap")->kernel_type);
	CHECK(fs_by_id("iso9660")->always_ro && !fs_by_id("ntfs")->always_ro &&
	      !fs_by_id("exfat")->always_ro && !v->always_ro &&
	      !e->always_ro);

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
	fs_by_id("exfat")->mount_opts(opts, sizeof(opts), 1000, 100);
	CHECK_STR(opts, "uid=1000,gid=100,fmask=0177,dmask=0077");
	fs_by_id("ntfs")->mount_opts(opts, sizeof(opts), 1000, 100);
	CHECK_STR(opts, "uid=1000,gid=100,fmask=0177,dmask=0077");
	CHECK(strstr(opts, "force") == NULL);
	fs_by_id("iso9660")->mount_opts(opts, sizeof(opts), 1000, 100);
	CHECK_STR(opts, "uid=1000,gid=100");

	/* the whole data string: ext4 and NTFS ask before a journal is
	 * replayed; after the user agreed, ext4 replays without an option
	 * and NTFS with "recover"; the others are not changed */
	CHECK(fs_mount_data(e, 0, 1000, 100, opts, sizeof(opts)) == 0);
	CHECK_STR(opts, "recovery=ask");
	CHECK(fs_mount_data(e, 1, 1000, 100, opts, sizeof(opts)) == 0);
	CHECK_STR(opts, "");
	CHECK(fs_mount_data(fs_by_id("ntfs"), 0, 1000, 100, opts,
			    sizeof(opts)) == 0);
	CHECK_STR(opts, "uid=1000,gid=100,fmask=0177,dmask=0077,recovery=ask");
	CHECK(fs_mount_data(fs_by_id("ntfs"), 1, 1000, 100, opts,
			    sizeof(opts)) == 0);
	CHECK_STR(opts, "uid=1000,gid=100,fmask=0177,dmask=0077,recover");
	CHECK(strstr(opts, "force") == NULL);
	for (int rec = 0; rec < 2; rec++) {
		CHECK(fs_mount_data(v, rec, 1000, 100, opts, sizeof(opts)) ==
		      0);
		CHECK_STR(opts, "uid=1000,gid=100,fmask=0177,dmask=0077,"
				"shortname=mixed");
		CHECK(fs_mount_data(fs_by_id("exfat"), rec, 1000, 100, opts,
				    sizeof(opts)) == 0);
		CHECK_STR(opts, "uid=1000,gid=100,fmask=0177,dmask=0077");
		CHECK(fs_mount_data(fs_by_id("iso9660"), rec, 1000, 100, opts,
				    sizeof(opts)) == 0);
		CHECK_STR(opts, "uid=1000,gid=100");
	}
	/* the largest ids still fit in devd's buffer (160 bytes) */
	{
		char big[160];

		CHECK(fs_mount_data(fs_by_id("ntfs"), 0, 4294967295u,
				    4294967295u, big, sizeof(big)) == 0);
		CHECK(fs_mount_data(v, 0, 4294967295u, 4294967295u, big,
				    sizeof(big)) == 0);
	}
	/* too small: refused, never cut */
	CHECK(fs_mount_data(e, 0, 0, 0, opts, 12) == -ENAMETOOLONG);
	CHECK(fs_mount_data(e, 0, 0, 0, opts, 13) == 0);
	CHECK_STR(opts, "recovery=ask");
	CHECK(fs_mount_data(fs_by_id("ntfs"), 1, 0, 0, opts, 20) ==
	      -ENAMETOOLONG);
	CHECK(fs_mount_data(e, 1, 0, 0, opts, 1) == 0 && opts[0] == 0);

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

	test_fstab_exfat_ntfs();

	memset(&pr, 0, sizeof(pr));
	strcpy(pr.fstype, "vfat");
	pr.fat_bits = 32;
	CHECK(fs_supported(&pr));
	pr.fat_bits = 16;
	CHECK(fs_supported(&pr));
	pr.fat_bits = 12;
	CHECK(fs_supported(&pr));
	pr.fat_bits = 0;
	CHECK(!fs_supported(&pr));
	strcpy(pr.fstype, "ext4");
	CHECK(fs_supported(&pr));
	strcpy(pr.fstype, "ntfs");
	CHECK(fs_supported(&pr));
	strcpy(pr.fstype, "exfat");
	CHECK(fs_supported(&pr));
	strcpy(pr.fstype, "iso9660");
	CHECK(fs_supported(&pr));
	strcpy(pr.fstype, "swap");
	CHECK(!fs_supported(&pr));
	strcpy(pr.fstype, "none");
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

/* ---- the device rules ------------------------------------------------------------ */

static void mkdev(struct dev *d, int usb, int removable, int optical,
		  int disk_ro, int system)
{
	memset(d, 0, sizeof(*d));
	strcpy(d->name, "sdb1");
	strcpy(d->disk, "sdb");
	d->kind = DEV_PART;
	d->size = 1 << 20;
	d->removable = removable;
	d->system = system;
	dev_classify(d, usb, optical, disk_ro, 0);
}

static int allows(const struct dev *d, enum dev_op op)
{
	const char *why = NULL;
	int r = dev_policy_check(d, op, &why);

	if (r < 0) {
		CHECK(r == -EPERM && why && *why);
		return 0;
	}
	return 1;
}

static void test_policy(void)
{
	struct dev d, o;
	const struct fs_type *iso = fs_by_id("iso9660");
	const struct fs_type *ntfs = fs_by_id("ntfs");
	struct devp_flags f;

	/* a USB stick */
	mkdev(&d, 1, 1, 0, 0, 0);
	CHECK(d.usb && !d.optical && !d.internal && !d.readonly);
	CHECK(allows(&d, OP_MOUNT) && allows(&d, OP_UNMOUNT) &&
	      allows(&d, OP_EJECT) && allows(&d, OP_FORMAT));
	CHECK(dev_mount_flags(&d, ntfs) == (MS_NOSUID | MS_NODEV));
	CHECK(dev_mount_flags(&d, iso) == (MS_NOSUID | MS_NODEV | MS_RDONLY));
	/* write-protected: everything but format, read-only */
	mkdev(&d, 1, 1, 0, 1, 0);
	CHECK(d.readonly && allows(&d, OP_MOUNT) && allows(&d, OP_EJECT) &&
	      !allows(&d, OP_FORMAT));
	CHECK(dev_mount_flags(&d, ntfs) & MS_RDONLY);
	d.readonly = 0;
	dev_classify(&d, 1, 0, 0, 1); /* the partition alone */
	CHECK(d.readonly && !allows(&d, OP_FORMAT));
	/* the stick LikeOS runs from */
	mkdev(&d, 1, 1, 0, 0, 1);
	CHECK(!allows(&d, OP_MOUNT) && !allows(&d, OP_UNMOUNT) &&
	      !allows(&d, OP_EJECT) && !allows(&d, OP_FORMAT));

	/* a CD/DVD drive on SATA (removable, not USB) */
	mkdev(&d, 0, 1, 1, 0, 0);
	CHECK(d.optical && !d.internal && d.readonly && !d.usb);
	CHECK(allows(&d, OP_MOUNT) && allows(&d, OP_UNMOUNT) &&
	      allows(&d, OP_EJECT) && !allows(&d, OP_FORMAT));
	CHECK(dev_mount_flags(&d, iso) & MS_RDONLY);
	CHECK(dev_mount_flags(&d, fs_by_id("vfat")) ==
	      (MS_NOSUID | MS_NODEV | MS_RDONLY));
	/* behind USB */
	mkdev(&d, 1, 1, 1, 1, 0);
	CHECK(d.optical && !d.internal && allows(&d, OP_EJECT) &&
	      !allows(&d, OP_FORMAT));
	/* holding the running system (a live disc) */
	mkdev(&d, 0, 1, 1, 1, 1);
	CHECK(!allows(&d, OP_MOUNT) && !allows(&d, OP_EJECT));

	/* a built-in disk: mount and unmount only */
	mkdev(&d, 0, 0, 0, 0, 0);
	CHECK(d.internal && !d.usb && !d.optical && !d.readonly);
	CHECK(allows(&d, OP_MOUNT) && allows(&d, OP_UNMOUNT) &&
	      !allows(&d, OP_EJECT) && !allows(&d, OP_FORMAT));
	CHECK(dev_mount_flags(&d, ntfs) == (MS_NOSUID | MS_NODEV));
	/* a built-in card slot (removable, not USB) is built in too */
	mkdev(&d, 0, 1, 0, 0, 0);
	CHECK(d.internal && !allows(&d, OP_FORMAT) && !allows(&d, OP_EJECT));
	/* its EFI or recovery partition */
	mkdev(&d, 0, 0, 0, 0, 0);
	d.reserved = 1;
	CHECK(!allows(&d, OP_MOUNT) && !allows(&d, OP_UNMOUNT) &&
	      !allows(&d, OP_FORMAT));
	/* the system disk */
	mkdev(&d, 0, 0, 0, 0, 1);
	CHECK(!allows(&d, OP_MOUNT) && !allows(&d, OP_UNMOUNT) &&
	      !allows(&d, OP_EJECT) && !allows(&d, OP_FORMAT));
	/* USB but not removable: nothing */
	mkdev(&d, 1, 0, 0, 0, 0);
	CHECK(!d.internal && !allows(&d, OP_MOUNT));

	/* a medium that changed under a mount */
	mkdev(&o, 0, 1, 1, 1, 0);
	o.diskseq = 7;
	d = o;
	CHECK(!dev_medium_changed(&o, &d));
	d.diskseq = 8;
	CHECK(dev_medium_changed(&o, &d));
	d = o;
	d.size = 0;
	CHECK(dev_medium_changed(&o, &d));

	/* the volume agent's rule */
	memset(&f, 0, sizeof(f));
	f.usb = f.removable = f.supported = 1;
	CHECK(devp_automount(&f));
	f.readonly = 1;
	CHECK(devp_automount(&f));
	f.ejected = 1;
	CHECK(!devp_automount(&f));
	f.ejected = 0;
	f.system = 1;
	CHECK(!devp_automount(&f));
	f.system = 0;
	f.supported = 0;
	CHECK(!devp_automount(&f));
	memset(&f, 0, sizeof(f));
	f.optical = f.removable = f.supported = f.readonly = 1;
	CHECK(devp_automount(&f));
	memset(&f, 0, sizeof(f));
	f.internal = f.supported = 1;
	CHECK(!devp_automount(&f));
	f.removable = 1; /* a built-in card slot */
	CHECK(!devp_automount(&f));
	f.usb = 1; /* a confused line: never a built-in disk */
	CHECK(!devp_automount(&f));
}

/* ---- labels of NTFS, exFAT and Joliet --------------------------------------------- */

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static void put_utf16(uint8_t *p, const uint16_t *u, size_t n, int be)
{
	for (size_t i = 0; i < n; i++) {
		p[2 * i + (be ? 1 : 0)] = (uint8_t)u[i];
		p[2 * i + (be ? 0 : 1)] = (uint8_t)(u[i] >> 8);
	}
}

#define NT_CLUSTER 4096
#define NT_REC 1024

/* A $Volume record (record 3) with a $VOLUME_NAME, protected by the
 * update sequence, at `r'. */
static void nt_record(uint8_t *r, const uint16_t *name, size_t units)
{
	size_t a = 0x38, alen = (0x18 + units * 2 + 7) & ~(size_t)7;

	memset(r, 0, NT_REC);
	memcpy(r, "FILE", 4);
	put16(r + 4, 0x30);      /* update sequence at */
	put16(r + 6, 3);         /* 1 + 2 strides of 512 */
	put16(r + 0x14, (uint16_t)a);
	put16(r + 0x16, 1);      /* in use */
	put32(r + 0x2C, 3);
	put32(r + a, 0x10);      /* $STANDARD_INFORMATION, empty */
	put32(r + a + 4, 0x18);
	put16(r + a + 0x14, 0x18);
	a += 0x18;
	put32(r + a, 0x60);
	put32(r + a + 4, (uint32_t)alen);
	put32(r + a + 0x10, (uint32_t)(units * 2));
	put16(r + a + 0x14, 0x18);
	put_utf16(r + a + 0x18, name, units, 0);
	a += alen;
	put32(r + a, 0xFFFFFFFF);
	put32(r + 0x18, (uint32_t)(a + 8));
	put32(r + 0x1C, NT_REC);
	/* the update sequence: number 0x0042, the real tails kept */
	put16(r + 0x30, 0x0042);
	memcpy(r + 0x32, r + 510, 2);
	memcpy(r + 0x34, r + 1022, 2);
	put16(r + 510, 0x0042);
	put16(r + 1022, 0x0042);
}

static void nt_boot(uint8_t *img)
{
	memcpy(img + 3, "NTFS    ", 8);
	put16(img + 0x0B, 512);
	img[0x0D] = 8;
	put64(img + 0x30, 4);    /* $MFT at cluster 4 */
	put64(img + 0x38, 8);    /* $MFTMirr at cluster 8 */
	img[0x40] = 0xF6;        /* 2^10 = 1024-byte records */
	put64(img + 0x48, 0x1122334455667788ULL);
}

static void test_probe_labels(void)
{
	static uint8_t img[1 << 20];
	struct probe_result pr;
	struct mem_ctx m = { img, sizeof(img) };
	static const uint16_t win[] = { 'W', 0xEF, 'n', ' ', 'C', 0xD83D,
					0xDE00 };
	uint8_t *rec = img + 4 * NT_CLUSTER + 3 * NT_REC;
	uint8_t *mir = img + 8 * NT_CLUSTER + 3 * NT_REC;

	/* NTFS: the label of the $Volume record */
	memset(img, 0, sizeof(img));
	nt_boot(img);
	nt_record(rec, win, 7);
	probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
	CHECK_STR(pr.fstype, "ntfs");
	CHECK_STR(pr.uuid, "1122334455667788");
	CHECK_STR(pr.label, "W\xc3\xafn C\xf0\x9f\x98\x80");
	/* without reading further: no label, no harm */
	probe_buffer(img, PROBE_BYTES, NULL, NULL, &pr);
	CHECK_STR(pr.fstype, "ntfs");
	CHECK_STR(pr.label, "");
	/* a torn record: the mirror's copy */
	nt_record(mir, win, 3);
	put16(rec + 1022, 0x0043);
	probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
	CHECK_STR(pr.label, "W\xc3\xafn");
	/* both torn: none */
	put16(mir + 510, 0x0001);
	probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
	CHECK_STR(pr.label, "");
	/* hostile attribute lengths and offsets */
	nt_record(rec, win, 7);
	put32(rec + 0x38 + 0x18 + 0x10, 0x7FFFFFF0); /* value length */
	probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
	CHECK_STR(pr.fstype, "ntfs");
	nt_record(rec, win, 7);
	put32(rec + 0x38 + 4, 0); /* attribute length 0: no endless walk */
	probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
	CHECK_STR(pr.label, "");
	nt_record(rec, win, 7);
	put32(rec + 0x2C, 5); /* another record's number */
	probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
	CHECK_STR(pr.label, "");
	/* unpaired surrogates and a control character */
	{
		static const uint16_t bad[] = { 'a', 0xDC00, 'b', 0x1B, 'c',
						0xD800 };

		nt_record(rec, bad, 6);
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.label, "a?b?c?");
	}
	/* insane geometry and an MFT beyond the image */
	for (int i = 0; i < 6; i++) {
		nt_record(rec, win, 7);
		nt_boot(img);
		switch (i) {
		case 0: img[0x40] = 0x80; break;     /* 2^128 bytes */
		case 1: img[0x40] = 0x7F; break;     /* 127 clusters */
		case 2: img[0x0D] = 0x00; break;
		case 3: img[0x0D] = 0x81; break;     /* 2^127-byte clusters */
		case 4: put64(img + 0x30, 0xFFFFFFFFFFFFFFFFULL);
			put64(img + 0x38, 0); break;
		case 5: put16(img + 0x0B, 3); break;
		}
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.fstype, "ntfs");
		CHECK_STR(pr.label, "");
	}
	/* random records */
	srand(2);
	for (int it = 0; it < 2000; it++) {
		nt_boot(img);
		nt_record(rec, win, 7);
		for (int k = 0; k < 24; k++)
			rec[rand() % NT_REC] = (uint8_t)rand();
		if (it & 1)
			img[0x40] = (uint8_t)rand();
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK(utf8_text_ok(pr.label, strlen(pr.label)));
	}

	/* exFAT: the volume label entry of the root directory, also in the
	 * second cluster of its chain */
	{
		static const uint16_t stick[] = { 'S', 't', 0xFC, 'c', 'k' };
		const uint64_t heap = 1024 * 512, cs = 4096;
		uint8_t *fat = img + 128 * 512;
		uint8_t *c4 = img + heap + 2 * cs, *c7 = img + heap + 5 * cs;

		memset(img, 0, sizeof(img));
		memcpy(img + 3, "EXFAT   ", 8);
		put32(img + 0x50, 128);   /* FAT at sector 128 */
		put32(img + 0x54, 8);     /* 8 sectors of FAT */
		put32(img + 0x58, 1024);  /* the heap at sector 1024 */
		put32(img + 0x5C, 100);   /* clusters */
		put32(img + 0x60, 4);     /* the root directory */
		put32(img + 0x64, 0xA1B2C3D4);
		img[0x6C] = 9;
		img[0x6D] = 3;
		c4[0] = 0x81;             /* allocation bitmap entry first */
		c4[32] = EXFAT_TEST_LABEL;
		c4[33] = 5;
		put_utf16(c4 + 34, stick, 5, 0);
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.fstype, "exfat");
		CHECK_STR(pr.uuid, "A1B2-C3D4");
		CHECK_STR(pr.label, "St\xc3\xbc" "ck");
		/* the label in the next cluster of the chain */
		memset(c4, 0, cs);
		for (size_t off = 0; off < cs; off += 32)
			c4[off] = 0x85;
		put32(fat + 4 * 4, 7);
		put32(fat + 7 * 4, 0xFFFFFFFF);
		c7[0] = EXFAT_TEST_LABEL;
		c7[1] = 2;
		put_utf16(c7 + 2, stick, 2, 0);
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.label, "St");
		/* a chain that loops, without a label: it ends */
		memset(c7, 0, cs);
		for (size_t off = 0; off < cs; off += 32)
			c7[off] = 0x85;
		put32(fat + 7 * 4, 4);
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.label, "");
		/* a character count past the entry */
		memset(c4, 0, cs);
		c4[0] = EXFAT_TEST_LABEL;
		c4[1] = 200;
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.label, "");
		/* shifts out of range, root outside the heap */
		img[0x6C] = 13;
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.fstype, "exfat");
		img[0x6C] = 9;
		img[0x6D] = 30;
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		img[0x6D] = 3;
		put32(img + 0x60, 500);
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.label, "");
		put32(img + 0x60, 0xFFFFFFFF);
		put32(img + 0x5C, 0xFFFFFFF0);
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.label, "");
		/* random boot sectors */
		srand(3);
		for (int it = 0; it < 2000; it++) {
			for (size_t i = 0x40; i < 0x70; i++)
				img[i] = (uint8_t)rand();
			img[0x6C] = (uint8_t)(9 + rand() % 4);
			probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
			CHECK(utf8_text_ok(pr.label, strlen(pr.label)));
		}
	}

	/* ISO 9660 with a Joliet descriptor after the primary one */
	{
		static const uint16_t j[] = { 0xDC, 'b', 'e', 'r', ' ', 'C',
					      'D' };
		static const uint16_t sp[] = { ' ' };

		memset(img, 0, sizeof(img));
		img[0x8000] = 1;
		memcpy(img + 0x8001, "CD001", 5);
		memcpy(img + 0x8028, "UBER_CD                         ", 32);
		img[0x8800] = 2;
		memcpy(img + 0x8801, "CD001", 5);
		memcpy(img + 0x8800 + 88, "%/E", 3);
		for (int i = 0; i < 16; i++)
			put_utf16(img + 0x8800 + 40 + 2 * i, sp, 1, 1);
		put_utf16(img + 0x8800 + 40, j, 7, 1);
		img[0x9000] = 255;
		memcpy(img + 0x9001, "CD001", 5);
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.fstype, "iso9660");
		CHECK_STR(pr.label, "\xc3\x9c" "ber CD");
		/* not Joliet: the primary label stays */
		img[0x8800 + 90] = 'X';
		probe_buffer(img, PROBE_BYTES, mem_read, &m, &pr);
		CHECK_STR(pr.label, "UBER_CD");
		/* the Joliet descriptor beyond the probed bytes */
		img[0x8800] = 0; /* a boot record in its place */
		img[0x9000] = 2;
		memcpy(img + 0x9000 + 88, "%/@", 3);
		put_utf16(img + 0x9000 + 40, j, 7, 1);
		probe_buffer(img, 0x9000, mem_read, &m, &pr);
		CHECK_STR(pr.fstype, "iso9660");
		CHECK_STR(pr.label, "\xc3\x9c" "ber CD");
		/* no terminator anywhere: the search is bounded */
		memset(img + 0x9000, 0, 2048);
		memcpy(img + 0x9001, "CD001", 5);
		for (int i = 0; i < 40; i++) {
			uint8_t *vd = img + 0x9800 + 2048 * i;

			vd[0] = 0;
			memcpy(vd + 1, "CD001", 5);
		}
		probe_buffer(img, 0x9000, mem_read, &m, &pr);
		CHECK_STR(pr.label, "UBER_CD");
	}

	/* UTF-16 on its own */
	{
		char out[DEVD_LABEL_MAX];
		static const uint8_t be[] = { 0x00, 'A', 0xD8, 0x3D, 0xDE, 0x00,
					      0x00, 0x00, 0x00, 'Z' };

		utf16_label(out, sizeof(out), be, 5, 1);
		CHECK_STR(out, "A\xf0\x9f\x98\x80"); /* stops at the NUL */
		utf16_label(out, 4, be, 3, 1);
		CHECK_STR(out, "A"); /* no half characters */
	}
}

/* ---- partition tables -------------------------------------------------------------- */

static void guid_put(uint8_t *p, const char *t)
{
	uint8_t be[16];
	int n = 0;

	for (const char *c = t; *c && n < 16; c++) {
		unsigned v = 0;

		if (*c == '-')
			continue;
		if (sscanf(c, "%2x", &v) != 1)
			v = 0;
		be[n++] = (uint8_t)v;
		c++;
	}
	p[0] = be[3];
	p[1] = be[2];
	p[2] = be[1];
	p[3] = be[0];
	p[4] = be[5];
	p[5] = be[4];
	p[6] = be[7];
	p[7] = be[6];
	memcpy(p + 8, be + 8, 8);
}

static void gpt_image(uint8_t *img, size_t size, uint32_t lbs)
{
	uint8_t *ents = img + 2 * lbs;

	memset(img, 0, size);
	img[446 + 4] = 0xEE;
	put32(img + 446 + 8, 1);
	img[510] = 0x55;
	img[511] = 0xAA;
	memcpy(img + lbs, "EFI PART", 8);
	put64(img + lbs + 0x48, 2);
	put32(img + lbs + 0x50, 128);
	put32(img + lbs + 0x54, 128);
	guid_put(ents, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
	put64(ents + 32, 256);
	guid_put(ents + 128, "E3C9E316-0B5C-4DB8-817D-F92DF00215AE");
	put64(ents + 128 + 32, 1000);
	guid_put(ents + 256, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
	put64(ents + 256 + 32, 2000);
	guid_put(ents + 384, "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC");
	put64(ents + 384 + 32, 9000);
	guid_put(ents + 512, "0FC63DAF-8483-4772-8E79-3D69D8477DE4");
	put64(ents + 512 + 32, 12000);
}

static void test_partition_tables(void)
{
	static uint8_t img[256 * 1024];
	struct mem_ctx m = { img, sizeof(img) };
	struct pt_reserved pt;

	gpt_image(img, sizeof(img), 512);
	pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
	CHECK(pt.n == 3 && pt_reserved_has(&pt, 256) &&
	      pt_reserved_has(&pt, 1000) && pt_reserved_has(&pt, 9000) &&
	      !pt_reserved_has(&pt, 2000) && !pt_reserved_has(&pt, 12000));
	/* 4 KiB sectors: starts in 512-byte units, as sysfs gives them */
	gpt_image(img, sizeof(img), 4096);
	pt_reserved_read(mem_read, &m, 4096, sizeof(img), &pt);
	CHECK(pt.n == 3 && pt_reserved_has(&pt, 256 * 8) &&
	      pt_reserved_has(&pt, 9000 * 8));
	/* a bad block size counts as 512 (then there is no header there) */
	pt_reserved_read(mem_read, &m, 3000, sizeof(img), &pt);
	CHECK(pt.n == 0);
	/* hostile headers */
	gpt_image(img, sizeof(img), 512);
	put32(img + 512 + 0x54, 8);
	pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
	CHECK(pt.n == 0);
	gpt_image(img, sizeof(img), 512);
	put32(img + 512 + 0x50, 0xFFFFFFFF);
	pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
	CHECK(pt.n == 0);
	gpt_image(img, sizeof(img), 512);
	put64(img + 512 + 0x48, 0xFFFFFFFFFFFFFFF0ULL);
	pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
	CHECK(pt.n == 0);
	gpt_image(img, sizeof(img), 512);
	put32(img + 512 + 0x50, 1024); /* entries past the end of the disk */
	pt_reserved_read(mem_read, &m, 512, 64 * 1024, &pt);
	CHECK(pt.n == 0);
	/* many reserved entries: capped */
	gpt_image(img, sizeof(img), 512);
	for (int i = 0; i < 128; i++) {
		guid_put(img + 1024 + 128 * i,
			 "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
		put64(img + 1024 + 128 * i + 32, 100 + (uint64_t)i);
	}
	pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
	CHECK(pt.n == PT_RESERVED_MAX);
	/* an MBR with an EFI system partition and a recovery partition */
	memset(img, 0, sizeof(img));
	img[446 + 4] = 0x0C;
	put32(img + 446 + 8, 2048);
	img[462 + 4] = 0xEF;
	put32(img + 462 + 8, 4096);
	img[478 + 4] = 0x27;
	put32(img + 478 + 8, 8192);
	img[494 + 4] = 0x07;
	put32(img + 494 + 8, 16384);
	img[510] = 0x55;
	img[511] = 0xAA;
	pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
	CHECK(pt.n == 2 && pt_reserved_has(&pt, 4096) &&
	      pt_reserved_has(&pt, 8192) && !pt_reserved_has(&pt, 2048));
	/* nothing at all */
	memset(img, 0, sizeof(img));
	pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
	CHECK(pt.n == 0);
	/* random headers */
	srand(4);
	for (int it = 0; it < 2000; it++) {
		gpt_image(img, sizeof(img), 512);
		for (int k = 0; k < 8; k++)
			img[512 + 0x48 + rand() % 16] = (uint8_t)rand();
		pt_reserved_read(mem_read, &m, 512, sizeof(img), &pt);
		CHECK(pt.n >= 0 && pt.n <= PT_RESERVED_MAX);
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

/* The first existing executable of `paths' (NULL-terminated), or NULL. */
static const char *first_tool(const char *const *paths)
{
	for (; *paths; paths++)
		if (have(*paths))
			return *paths;
	return NULL;
}

/* Bytes per cluster of the exFAT or NTFS volume at the start of `path',
 * from its boot sector; 0 when it cannot be read. */
static uint64_t boot_cluster_bytes(const char *path, int ntfs)
{
	uint8_t b[512];
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return 0;
	r = pread(fd, b, sizeof(b), 0);
	close(fd);
	if (r != (ssize_t)sizeof(b))
		return 0;
	if (!ntfs) {
		if (memcmp(b + 3, "EXFAT   ", 8) != 0 || b[0x6c] > 12 ||
		    b[0x6d] > 25)
			return 0;
		return (uint64_t)1 << (b[0x6c] + b[0x6d]);
	}
	if (memcmp(b + 3, "NTFS    ", 8) != 0)
		return 0;
	/* sectors per cluster: a count, or above 0x80 a negative shift */
	return (uint64_t)(b[0x0b] | b[0x0c] << 8) *
	       (b[0x0d] <= 0x80 ? b[0x0d] : (1u << (256 - b[0x0d])));
}

/* One exFAT or NTFS format with exactly the argument vector devd builds,
 * run with the ported tool from `tooldir' (built for the build machine by
 * "make run-ports"), checked with the build machine's fsck. */
static void format_case(const char *tooldir, const char *dir,
			const char *fsid, const char *label, uint32_t cluster,
			uint32_t want_cluster, off_t size, int whole_disk)
{
	static const char *const fsck_exfat[] = { "/sbin/fsck.exfat",
						  "/usr/sbin/fsck.exfat", NULL };
	static const char *const ntfsfix[] = { "/bin/ntfsfix",
					       "/usr/bin/ntfsfix", NULL };
	const struct fs_type *t = fs_by_id(fsid);
	int ntfs = strcmp(fsid, "ntfs") == 0;
	char path[512], tool[512], store[1024], plain[400];
	char *argv[26], *run_argv[27];
	struct probe_result pr;
	const char *fsck;
	int argc, k = 0;

	snprintf(path, sizeof(path), "%s/fmt-%s.img", dir, fsid);
	CHECK(t && t->label_ok(label, strlen(label), plain, sizeof(plain)) ==
		       0);
	argc = t ? t->mkfs_argv(argv, 24, store, sizeof(store), path, plain,
				cluster, 512, whole_disk, 1000, 1000) :
		   -1;
	CHECK(argc > 1 && make_image(path, size) == 0);
	if (argc <= 1)
		return;
	snprintf(tool, sizeof(tool), "%s/%s", tooldir,
		 strrchr(argv[0], '/') + 1);
	run_argv[k++] = tool;
	/* an image file is not a block device: the one argument devd never
	 * passes, and only mkntfs wants it */
	if (ntfs)
		run_argv[k++] = "-F";
	for (int i = 1; i <= argc; i++)
		run_argv[k++] = argv[i];
	CHECK(run(run_argv) == 0);

	fsck = first_tool(ntfs ? ntfsfix : fsck_exfat);
	if (fsck) {
		char *f[] = { (char *)fsck, "-n", path, NULL };
		CHECK(run(f) == 0);
	} else {
		printf("  (no %s on the build machine: %s not checked)\n",
		       ntfs ? "ntfsfix" : "fsck.exfat", fsid);
	}
	CHECK(probe_device(path, &pr) == 0);
	CHECK_STR(pr.fstype, fsid);
	/* the display label holds DEVD_LABEL_MAX - 1 bytes of it */
	if (strlen(label) < DEVD_LABEL_MAX)
		CHECK_STR(pr.label, label);
	else
		CHECK(pr.label[0] &&
		      strncmp(pr.label, label, strlen(pr.label)) == 0);
	CHECK(fs_supported(&pr));
	CHECK(boot_cluster_bytes(path, ntfs) == want_cluster);
	unlink(path);
}

static void test_formats(const char *dir)
{
	const char *tools = getenv("DEVD_TEST_MKFS_DIR");
	char ue[64] = "", euro[400] = "", astral[64] = "";

	if (!tools || !*tools) {
		printf("  (exFAT/NTFS formats skipped: \"make run-ports\" "
		       "runs them)\n");
		return;
	}
	for (int i = 0; i < 11; i++)
		strcat(ue, "\xc3\xbc");
	for (int i = 0; i < 128; i++)
		strcat(euro, "\xe2\x82\xac");
	for (int i = 0; i < 5; i++)
		strcat(astral, "\xf0\x9f\x98\x80");
	strcat(astral, "x");

	/* exFAT: the default unit follows the size (32 KiB from 256 MiB) */
	format_case(tools, dir, "exfat", "Stick \xc3\xbc", 0, 4096, 64 << 20,
		    1);
	format_case(tools, dir, "exfat", ue, 131072, 131072, 256 << 20, 0);
	format_case(tools, dir, "exfat", astral, 0, 32768, 512 << 20, 0);
	format_case(tools, dir, "exfat", "", 32u << 20, 32u << 20,
		    (off_t)2 << 30, 0);
	format_case(tools, dir, "exfat", "-dash", 4096, 4096, 32 << 20, 1);
	/* NTFS */
	format_case(tools, dir, "ntfs", "Daten \xc3\xa4", 0, 4096, 64 << 20, 1);
	format_case(tools, dir, "ntfs", euro, 65536, 65536, 256 << 20, 0);
	format_case(tools, dir, "ntfs", "-dash", 512, 512, 32 << 20, 0);
	format_case(tools, dir, "ntfs", "", 2u << 20, 2u << 20,
		    (off_t)1 << 30, 0);
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
			      "Win \xc3\xa4", path, NULL };
		char *b[] = { "/usr/sbin/mkfs.ntfs", "-F", "-f", "-q", "-L",
			      "Big", "-c", "65536", "-s", "4096", path, NULL };
		if (run(a) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.fstype, "ntfs");
			CHECK_STR(pr.label, "Win \xc3\xa4");
			CHECK(strlen(pr.uuid) == 16);
			CHECK(fs_supported(&pr));
		}
		if (make_image(path, 64 << 20) == 0 && run(b) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.label, "Big");
		}
	} else {
		printf("  (no mkfs.ntfs: NTFS image skipped)\n");
	}

	snprintf(path, sizeof(path), "%s/exfat.img", dir);
	if (have("/sbin/mkfs.exfat") && make_image(path, 32 << 20) == 0) {
		char *a[] = { "/sbin/mkfs.exfat", "-L", "Stick \xc3\xbc", path,
			      NULL };
		char *b[] = { "/sbin/mkfs.exfat", "-c", "128K", "-L", "BIG",
			      path, NULL };
		if (run(a) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.fstype, "exfat");
			CHECK_STR(pr.label, "Stick \xc3\xbc");
			CHECK(strlen(pr.uuid) == 9);
			CHECK(fs_supported(&pr));
		}
		if (run(b) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.label, "BIG");
		}
	} else {
		printf("  (no mkfs.exfat: exFAT image skipped)\n");
	}

	snprintf(path, sizeof(path), "%s/cd.iso", dir);
	if (have("/usr/bin/genisoimage")) {
		char src[512];
		char *a[] = { "/usr/bin/genisoimage", "-quiet", "-V", "MYCD",
			      "-o", path, src, NULL };
		char *j[] = { "/usr/bin/genisoimage", "-quiet", "-J",
			      "-input-charset", "utf-8", "-V",
			      "Mein Br\xc3\xbc" "cken-CD", "-o", path, src,
			      NULL };
		snprintf(src, sizeof(src), "%s/isosrc", dir);
		mkdir(src, 0700);
		if (run(a) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.fstype, "iso9660");
			CHECK_STR(pr.label, "MYCD");
			CHECK(fs_supported(&pr));
		}
		if (run(j) == 0) {
			CHECK(probe_device(path, &pr) == 0);
			CHECK_STR(pr.fstype, "iso9660");
			CHECK_STR(pr.label, "Mein Br\xc3\xbc" "cken-CD");
		}
	} else {
		printf("  (no genisoimage: ISO 9660 image skipped)\n");
	}

	CHECK(probe_device("/nonexistent/devd-test", &pr) < 0);

	test_formats(dir);
}

int main(int argc, char **argv)
{
	g_log_level = LOG_ERR;
	test_proto();
	test_names();
	test_fstab();
	test_auth();
	test_policy();
	test_cancel();
	test_probe_crafted();
	test_probe_labels();
	test_partition_tables();
	if (argc > 1)
		test_images(argv[1]);
	printf("devd tests: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
