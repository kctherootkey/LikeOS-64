/* libinstaller host tests: everything but a whole installation.
 *
 *   test_backend WORKDIR
 *
 * WORKDIR must exist and be empty; the test makes image files (sparse) and
 * small directory trees in it.  The tool paths for disk images come from
 * the environment the way the library reads them (IL_MKFS_FAT, ...); the
 * script that runs this sets them to the host's tools.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "il_internal.h"

static int g_pass, g_fail;
static const char *g_work;

#define CHECK(cond, ...) check((cond), __FILE__, __LINE__, __VA_ARGS__)

static void check(int ok, const char *file, int line, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static void check(int ok, const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (ok) {
		g_pass++;
	} else {
		g_fail++;
		fprintf(stdout, "FAIL %s:%d: ", file, line);
		vfprintf(stdout, fmt, ap);
		fputc('\n', stdout);
	}
	va_end(ap);
}

static void wpath(char *buf, size_t len, const char *name)
{
	snprintf(buf, len, "%s/%s", g_work, name);
}

static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int sh(const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;
	int st;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	st = system(cmd);
	if (st == -1)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static int make_image(const char *path, uint64_t size)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* The test program is linked with --wrap=malloc: while g_malloc_left is
 * not negative, that many more allocations of the library succeed and the
 * ones after fail. */
static long g_malloc_left = -1;

void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n);

void *__wrap_malloc(size_t n)
{
	if (g_malloc_left == 0) {
		errno = ENOMEM;
		return NULL;
	}
	if (g_malloc_left > 0)
		g_malloc_left--;
	return __real_malloc(n);
}

static il_ctx *sim_ctx(const char *const *disks, const char *logname)
{
	struct il_options o;
	char log[1024];

	wpath(log, sizeof(log), logname);
	memset(&o, 0, sizeof(o));
	o.simulate = 1;
	o.simulate_disks = disks;
	o.log_path = log;
	o.verbose = getenv("IL_TEST_VERBOSE") != NULL;
	return il_ctx_new(&o);
}

/* ------------------------------------------------------------------ */

static void test_sizes(void)
{
	char b[64], dev[64];
	struct il_disk d;

	il_format_size(0, b, sizeof(b));
	CHECK(!strcmp(b, "0 B"), "size 0: %s", b);
	il_format_size(999, b, sizeof(b));
	CHECK(!strcmp(b, "999 B"), "size 999: %s", b);
	il_format_size(512ULL * 1024 * 1024, b, sizeof(b));
	CHECK(!strcmp(b, "536.9 MB"), "512 MiB: %s", b);
	il_format_size(512110190592ULL, b, sizeof(b));
	CHECK(!strcmp(b, "512.1 GB"), "512 GB: %s", b);
	il_format_size(999950000ULL, b, sizeof(b));
	CHECK(!strcmp(b, "1.0 GB"), "rounding up a unit: %s", b);
	il_format_size(2000000000000ULL, b, sizeof(b));
	CHECK(!strcmp(b, "2.0 TB"), "2 TB: %s", b);

	il_part_devname("/dev/sda", 2, dev, sizeof(dev));
	CHECK(!strcmp(dev, "/dev/sda2"), "sda2: %s", dev);
	il_part_devname("/dev/nvme0n1", 1, dev, sizeof(dev));
	CHECK(!strcmp(dev, "/dev/nvme0n1p1"), "nvme: %s", dev);
	il_part_devname("/dev/mmcblk0", 3, dev, sizeof(dev));
	CHECK(!strcmp(dev, "/dev/mmcblk0p3"), "mmc: %s", dev);

	memset(&d, 0, sizeof(d));
	strcpy(d.name, "nvme0n1");
	strcpy(d.model, "Samsung SSD 970");
	d.size_bytes = 512110190592ULL;
	d.transport = IL_TRANSPORT_NVME;
	il_disk_describe(&d, 0, b, sizeof(b));
	CHECK(!strcmp(b, "Disk 0 \xc2\xb7 Samsung SSD 970 \xc2\xb7 512.1 GB "
		      "\xc2\xb7 NVMe"), "describe: %s", b);
}

static void test_validators(void)
{
	char why[256], buf[64];

	CHECK(il_validate_hostname("likeos-3fa2", why, sizeof(why)) == 0,
	      "hostname ok: %s", why);
	CHECK(il_validate_hostname("", why, sizeof(why)) == -EINVAL, "empty");
	CHECK(il_validate_hostname("-abc", why, sizeof(why)) == -EINVAL, "lead -");
	CHECK(il_validate_hostname("abc-", why, sizeof(why)) == -EINVAL, "trail -");
	CHECK(il_validate_hostname("a.b", why, sizeof(why)) == -EINVAL, "dot");
	CHECK(il_validate_hostname("my pc", why, sizeof(why)) == -EINVAL, "blank");
	CHECK(il_validate_hostname("12345", why, sizeof(why)) == -EINVAL, "digits");
	CHECK(il_validate_hostname("h\xc3\xa4user", why, sizeof(why)) == -EINVAL,
	      "umlaut");
	CHECK(il_validate_hostname(
		      "a234567890123456789012345678901234567890123456789012345678901234",
		      why, sizeof(why)) == -EINVAL, "64 chars");
	CHECK(il_validate_hostname(
		      "a23456789012345678901234567890123456789012345678901234567890123",
		      why, sizeof(why)) == 0, "63 chars");
	CHECK(il_validate_hostname("localhost", why, sizeof(why)) == -EINVAL,
	      "localhost");

	CHECK(il_validate_username("ada", why, sizeof(why)) == 0, "ada: %s", why);
	CHECK(il_validate_username("_svc-1", why, sizeof(why)) == 0, "_svc-1");
	CHECK(il_validate_username("Ada", why, sizeof(why)) == -EINVAL, "upper");
	CHECK(il_validate_username("1ada", why, sizeof(why)) == -EINVAL, "digit");
	CHECK(il_validate_username("root", why, sizeof(why)) == -EINVAL, "root");
	CHECK(il_validate_username("render", why, sizeof(why)) == -EINVAL,
	      "render");
	CHECK(il_validate_username("a.b", why, sizeof(why)) == -EINVAL, "dot");
	CHECK(il_validate_username("abcdefghijklmnopqrstuvwxyz0123456",
				   why, sizeof(why)) == -EINVAL, "33 chars");

	CHECK(il_validate_password("x", why, sizeof(why)) == 0 &&
	      strstr(why, "short") != NULL, "pw x: valid, with a warning (%s)",
	      why);
	CHECK(il_validate_password("abcdefg", why, sizeof(why)) == 0 && why[0],
	      "pw of 7: a warning");
	CHECK(il_validate_password("abcdefgh", why, sizeof(why)) == 0 && !why[0],
	      "pw of 8: no word (%s)", why);
	CHECK(il_validate_password("", why, sizeof(why)) == -EINVAL, "pw empty");
	CHECK(il_validate_password("a\nb", why, sizeof(why)) == -EINVAL, "pw nl");
	CHECK(il_password_strength("") == 0, "strength empty");
	CHECK(il_password_strength("password") == 0, "strength common");
	CHECK(il_password_strength("aaaaaaaa") <= 1, "strength aaa");
	CHECK(il_password_strength("Tr0ub4dor&3x") >= 3, "strength good: %d",
	      il_password_strength("Tr0ub4dor&3x"));
	CHECK(il_password_strength("correct horse battery staple") == 4,
	      "strength long: %d",
	      il_password_strength("correct horse battery staple"));

	il_suggest_username("Ada Lovelace", buf, sizeof(buf));
	CHECK(!strcmp(buf, "ada"), "suggest ada: %s", buf);
	il_suggest_username("J\xc3\xbcrgen M\xc3\xbcller", buf, sizeof(buf));
	CHECK(!strcmp(buf, "juergen"), "suggest juergen: %s", buf);
	il_suggest_username("  Root ", buf, sizeof(buf));
	CHECK(!strcmp(buf, "root1"), "suggest root1: %s", buf);
	il_suggest_username("", buf, sizeof(buf));
	CHECK(!strcmp(buf, "user"), "suggest empty: %s", buf);
	il_suggest_username("42 Zaphod", buf, sizeof(buf));
	CHECK(il_validate_username(buf, why, sizeof(why)) == 0,
	      "suggest digits gives a valid name: %s", buf);

	il_suggest_hostname(buf, sizeof(buf));
	CHECK(strlen(buf) == 11 && il_starts_with(buf, "likeos-") &&
	      il_validate_hostname(buf, why, sizeof(why)) == 0,
	      "suggest hostname: %s", buf);
}

static void test_timezones(void)
{
	int n = il_timezone_count(), prev = -100000, uniq = 1;
	int berlin = il_timezone_find("Europe/Berlin");

	CHECK(n == 66, "66 zones (as the C library), got %d", n);
	{
		/* The names, for run-tests.sh to hold against libc's table. */
		char p[1024];
		FILE *f;

		wpath(p, sizeof(p), "zones.txt");
		f = fopen(p, "w");
		for (int i = 0; f && i < n; i++)
			fprintf(f, "%s\n", il_timezone_name(i));
		if (f)
			fclose(f);
	}
	CHECK(berlin >= 0 && !strcmp(il_timezone_label(berlin),
				      "(UTC+01:00) Berlin"),
	      "Berlin label: %s", berlin >= 0 ? il_timezone_label(berlin) : "-");
	CHECK(il_timezone_offset_minutes(berlin) == 60, "Berlin +60");
	{
		int i = il_timezone_find("Asia/Kolkata");

		CHECK(i >= 0 && !strcmp(il_timezone_label(i),
					"(UTC+05:30) Kolkata"),
		      "Kolkata: %s", i >= 0 ? il_timezone_label(i) : "-");
		i = il_timezone_find("America/New_York");
		CHECK(i >= 0 && !strcmp(il_timezone_label(i),
					"(UTC-05:00) New York"),
		      "New York: %s", i >= 0 ? il_timezone_label(i) : "-");
		i = il_timezone_find("UTC");
		CHECK(i >= 0 && !strcmp(il_timezone_label(i),
					"(UTC) Coordinated Universal Time"),
		      "UTC: %s", i >= 0 ? il_timezone_label(i) : "-");
		i = il_timezone_find("Europe/Istanbul");
		CHECK(i >= 0 && il_timezone_offset_minutes(i) == 180,
		      "Istanbul <+03>-3");
		i = il_timezone_find("America/Sao_Paulo");
		CHECK(i >= 0 && !strcmp(il_timezone_label(i),
					"(UTC-03:00) Sao Paulo"),
		      "Sao Paulo: %s", i >= 0 ? il_timezone_label(i) : "-");
	}
	CHECK(il_timezone_find("Mars/Olympus") == -1, "unknown zone");
	for (int i = 0; i < n; i++) {
		int off = il_timezone_offset_minutes(i);

		CHECK(off >= prev, "sorted by offset at %d", i);
		prev = off;
		CHECK(il_timezone_find(il_timezone_name(i)) == i, "find %s",
		      il_timezone_name(i));
		for (int j = 0; j < i; j++)
			if (!strcmp(il_timezone_label(i), il_timezone_label(j)))
				uniq = 0;
	}
	CHECK(uniq, "labels are unique");

	CHECK(il_locale_count() == 2, "2 locales");
	CHECK(!strcmp(il_locale_label(il_locale_find("en_GB.UTF-8")),
		      "English (United Kingdom)"), "en_GB label");
	CHECK(il_locale_find("en_US.utf8") == il_locale_find("en_US.UTF-8"),
	      "utf8 spelling");
	CHECK(il_locale_find("de_DE.UTF-8") == -1, "no German yet");
}

static void test_sha256(void)
{
	struct il_sha256 s;
	uint8_t d[32];
	char hex[65];
	char *million;

	il_sha256_init(&s);
	il_sha256_final(&s, d);
	il_sha256_hex(d, hex);
	CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495"
		      "991b7852b855"), "sha256 empty: %s", hex);
	il_sha256_init(&s);
	il_sha256_update(&s, "abc", 3);
	il_sha256_final(&s, d);
	il_sha256_hex(d, hex);
	CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410"
		      "ff61f20015ad"), "sha256 abc: %s", hex);
	il_sha256_init(&s);
	il_sha256_update(&s, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnom"
			 "nopnopq", 56);
	il_sha256_final(&s, d);
	il_sha256_hex(d, hex);
	CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ec"
		      "edd419db06c1"), "sha256 56 bytes: %s", hex);
	million = malloc(1000000);
	memset(million, 'a', 1000000);
	il_sha256_init(&s);
	for (int i = 0; i < 1000000; i += 999)
		il_sha256_update(&s, million + i, i + 999 > 1000000 ?
				 (size_t)(1000000 - i) : 999);
	il_sha256_final(&s, d);
	il_sha256_hex(d, hex);
	CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d"
		      "39ccc7112cd0"), "sha256 million a: %s", hex);
	{
		char f[1024], sum[65];
		FILE *fp;

		wpath(f, sizeof(f), "million.bin");
		fp = fopen(f, "wb");
		fwrite(million, 1, 1000000, fp);
		fclose(fp);
		CHECK(il_sha256_file(f, sum, NULL, NULL, NULL) == 0 &&
		      !strcmp(sum, hex), "sha256 file: %s", sum);
	}
	free(million);
}

static void test_keyboard_conf(void)
{
	struct il_config c;
	char text[2048];

	il_config_defaults(&c);
	CHECK(il_keyboard_conf_text(&c, text, sizeof(text)) == 0, "kbd text");
	CHECK(strstr(text, "\nLAYOUT=us\nVARIANT=\nOPTIONS=\n") != NULL,
	      "one layout:\n%s", text);
	c.nlayouts = 2;
	strcpy(c.layouts[0], "de");
	strcpy(c.variants[0], "nodeadkeys");
	strcpy(c.layouts[1], "us");
	il_keyboard_conf_text(&c, text, sizeof(text));
	CHECK(strstr(text, "\nLAYOUT=de,us\nVARIANT=nodeadkeys,\n"
			   "OPTIONS=grp:alt_shift_toggle\n") != NULL,
	      "two layouts:\n%s", text);
	strcpy(c.xkb_options, "grp:win_space_toggle,compose:ralt");
	il_keyboard_conf_text(&c, text, sizeof(text));
	CHECK(strstr(text, "OPTIONS=grp:win_space_toggle,compose:ralt\n") != NULL,
	      "own grp option kept:\n%s", text);
	c.variants[0][0] = '\0';
	strcpy(c.xkb_options, "compose:ralt");
	il_keyboard_conf_text(&c, text, sizeof(text));
	CHECK(strstr(text, "\nVARIANT=\nOPTIONS=compose:ralt,grp:alt_shift_toggle\n")
	      != NULL, "defaults + switch added:\n%s", text);
}

static void test_answers(void)
{
	char path[1024], path2[1024], why[256];
	struct il_answers a, b;
	il_ctx *ctx;
	struct stat st;
	FILE *f;

	wpath(path, sizeof(path), "answers.conf");
	wpath(path2, sizeof(path2), "answers2.conf");
	ctx = sim_ctx(NULL, "answers.log");
	memset(&a, 0, sizeof(a));
	strcpy(a.disk, "/dev/nvme0n1");
	a.whole_disk = 0;
	a.root_partno = 3;
	a.reboot = 0;
	il_config_defaults(&a.cfg);
	strcpy(a.cfg.timezone, "Europe/Berlin");
	strcpy(a.cfg.time_format, "en_GB.UTF-8");
	a.cfg.nlayouts = 2;
	strcpy(a.cfg.layouts[0], "de");
	strcpy(a.cfg.variants[0], "nodeadkeys");
	strcpy(a.cfg.layouts[1], "us");
	strcpy(a.cfg.hostname, "box-1");
	strcpy(a.cfg.root_password, "r00t pass \"quoted\" # not a comment");
	strcpy(a.cfg.user.fullname, "Ada Lovelace");
	strcpy(a.cfg.user.username, "ada");
	strcpy(a.cfg.user.password, "secret\\backslash");
	CHECK(il_answers_save(ctx, path, &a, 1) == 0, "save: %s",
	      il_last_error(ctx));
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600,
	      "answer file mode 0600: %o", st.st_mode & 0777);
	CHECK(il_answers_load(ctx, path, &b) == 0, "load: %s",
	      il_last_error(ctx));
	CHECK(!strcmp(b.disk, a.disk) && b.whole_disk == 0 &&
	      b.root_partno == 3 && b.reboot == 0, "disk fields");
	CHECK(!strcmp(b.cfg.root_password, a.cfg.root_password),
	      "root password round trip: [%s]", b.cfg.root_password);
	CHECK(!strcmp(b.cfg.user.password, a.cfg.user.password),
	      "user password round trip: [%s]", b.cfg.user.password);
	CHECK(b.cfg.nlayouts == 2 && !strcmp(b.cfg.layouts[0], "de") &&
	      !strcmp(b.cfg.variants[0], "nodeadkeys") &&
	      !strcmp(b.cfg.layouts[1], "us") && !b.cfg.variants[1][0],
	      "layouts round trip");
	CHECK(!strcmp(b.cfg.timezone, "Europe/Berlin") &&
	      !strcmp(b.cfg.time_format, "en_GB.UTF-8") &&
	      !strcmp(b.cfg.hostname, "box-1") &&
	      !strcmp(b.cfg.user.fullname, "Ada Lovelace") &&
	      b.cfg.user_extra_groups == 1, "other fields");
	CHECK(il_config_validate(&b.cfg, why, sizeof(why)) == 0,
	      "loaded config valid: %s", why);

	/* Without passwords: hashes instead. */
	CHECK(il_answers_save(ctx, path2, &a, 0) == 0, "save hashed");
	CHECK(il_answers_load(ctx, path2, &b) == 0, "load hashed: %s",
	      il_last_error(ctx));
	CHECK(!b.cfg.root_password[0] &&
	      il_starts_with(b.cfg.root_password_hash, "$y$"),
	      "root hash: %s", b.cfg.root_password_hash);
	{
		struct crypt_data cd;
		const char *h;

		memset(&cd, 0, sizeof(cd));
		h = crypt_r(a.cfg.root_password, b.cfg.root_password_hash, &cd);
		CHECK(h && !strcmp(h, b.cfg.root_password_hash),
		      "the hash verifies the password");
		memset(&cd, 0, sizeof(cd));
		h = crypt_r(a.cfg.user.password, b.cfg.user_password_hash, &cd);
		CHECK(h && !strcmp(h, b.cfg.user_password_hash),
		      "the user hash verifies the password");
	}
	CHECK(il_config_validate(&b.cfg, why, sizeof(why)) == 0,
	      "hashed config valid: %s", why);

	/* Errors. */
	f = fopen(path, "w");
	fputs("# comment\nhostname = spaced\nkeyboard=de,us\n"
	      "keyboard_variant=nodeadkeys\n", f);
	fclose(f);
	CHECK(il_answers_load(ctx, path, &b) == -EINVAL,
	      "variant count mismatch refused");
	f = fopen(path, "w");
	fputs("hostnmae=typo\n", f);
	fclose(f);
	CHECK(il_answers_load(ctx, path, &b) == -EINVAL &&
	      strstr(il_last_error(ctx), "unknown setting"),
	      "unknown key refused: %s", il_last_error(ctx));
	f = fopen(path, "w");
	fputs("hostname=\"open\n", f);
	fclose(f);
	CHECK(il_answers_load(ctx, path, &b) == -EINVAL, "open quote refused");
	f = fopen(path, "w");
	fputs("whole_disk=no\n", f);
	fclose(f);
	CHECK(il_answers_load(ctx, path, &b) == -EINVAL,
	      "whole_disk=no without a partition refused");
	f = fopen(path, "w");
	fputs("  # indented comment\n\nhostname = spaced  \nreboot=off\n"
	      "user_extra_groups=false\nkeyboard=fr\n", f);
	fclose(f);
	CHECK(il_answers_load(ctx, path, &b) == 0 &&
	      !strcmp(b.cfg.hostname, "spaced") && b.reboot == 0 &&
	      b.cfg.user_extra_groups == 0 && !strcmp(b.cfg.layouts[0], "fr") &&
	      !strcmp(b.disk, "auto") && b.whole_disk == 1,
	      "blanks around '=' and defaults: [%s]", b.cfg.hostname);
	il_ctx_free(ctx);
}

/* il_answers_save when a bigger buffer cannot be had: -ENOMEM, the error
 * set, the file as it was, no temporary file left. */
static void test_answers_nomem(void)
{
	char path[1024], tmp[1100], why[256];
	struct il_answers a, b;
	il_ctx *ctx;
	int rc;

	wpath(path, sizeof(path), "answers-nomem.conf");
	snprintf(tmp, sizeof(tmp), "%s.il-new", path);
	ctx = sim_ctx(NULL, "answers-nomem.log");
	memset(&a, 0, sizeof(a));
	strcpy(a.disk, "/dev/sda");
	a.whole_disk = 1;
	il_config_defaults(&a.cfg);
	strcpy(a.cfg.hostname, "long-values");
	/* long values: the text outgrows its first buffer */
	memset(a.cfg.root_password, 'r', 200);
	a.cfg.root_password[200] = '\0';
	memset(a.cfg.user.password, 'u', 200);
	a.cfg.user.password[200] = '\0';
	memset(a.cfg.user.fullname, 'n', 120);
	a.cfg.user.fullname[120] = '\0';
	strcpy(a.cfg.user.username, "longuser");
	CHECK(il_answers_save(ctx, path, &a, 1) == 0, "save long values: %s",
	      il_last_error(ctx));
	CHECK(il_answers_load(ctx, path, &b) == 0 &&
	      !strcmp(b.cfg.root_password, a.cfg.root_password) &&
	      !strcmp(b.cfg.user.password, a.cfg.user.password) &&
	      !strcmp(b.cfg.user.fullname, a.cfg.user.fullname) &&
	      b.cfg.user_extra_groups == 1, "long values round trip");
	CHECK(il_config_validate(&b.cfg, why, sizeof(why)) == 0,
	      "long values valid: %s", why);

	/* the first buffer is there, a bigger one is not */
	a.cfg.user_extra_groups = 0;
	strcpy(a.cfg.hostname, "changed");
	g_malloc_left = 1;
	rc = il_answers_save(ctx, path, &a, 1);
	g_malloc_left = -1;
	CHECK(rc == -ENOMEM && strstr(il_last_error(ctx), "out of memory"),
	      "growth failure reported: %d %s", rc, il_last_error(ctx));
	CHECK(il_answers_load(ctx, path, &b) == 0 &&
	      !strcmp(b.cfg.hostname, "long-values") &&
	      b.cfg.user_extra_groups == 1 &&
	      !strcmp(b.cfg.user.password, a.cfg.user.password),
	      "the file was left as it was: %s", b.cfg.hostname);
	CHECK(!il_file_exists(tmp), "no temporary file left");
	/* nothing at all: no file is made */
	unlink(path);
	g_malloc_left = 1;
	rc = il_answers_save(ctx, path, &a, 1);
	g_malloc_left = -1;
	CHECK(rc == -ENOMEM && !il_file_exists(path) && !il_file_exists(tmp),
	      "no partial file written (%d)", rc);
	g_malloc_left = 0;
	rc = il_answers_save(ctx, path, &a, 1);
	g_malloc_left = -1;
	CHECK(rc == -ENOMEM && !il_file_exists(path), "first buffer refused");
	CHECK(il_answers_save(ctx, path, &a, 1) == 0 &&
	      il_answers_load(ctx, path, &b) == 0 &&
	      !strcmp(b.cfg.hostname, "changed") && !b.cfg.user_extra_groups,
	      "saved once memory is there: %s", il_last_error(ctx));
	il_ctx_free(ctx);
}

/* ------------------------------------------------------------------ */
/* Plans on image files                                                */
/* ------------------------------------------------------------------ */

static void test_plan_blank(void)
{
	char img[1024], why[512], desc[256];
	const char *disks[2];
	struct il_disk_list l;
	struct il_disk v;
	struct il_source_info src;
	il_ctx *ctx;
	il_plan *p;

	wpath(img, sizeof(img), "blank.img");
	make_image(img, 20ULL << 30);
	disks[0] = img;
	disks[1] = NULL;
	ctx = sim_ctx(disks, "plan-blank.log");
	CHECK(il_disks_scan(ctx, &l) == 0 && l.n == 1, "scan blank");
	CHECK(l.disks[0].table == IL_TABLE_NONE && l.disks[0].nparts == 0 &&
	      l.disks[0].nfree == 1, "blank: no table, one free region");
	CHECK(l.disks[0].free[0].start_bytes == IL_ALIGN_BYTES,
	      "free starts at 1 MiB");
	CHECK(il_disks_find(&l, "auto") == &l.disks[0], "auto finds it");
	p = il_plan_new(ctx, &l.disks[0]);
	memset(&src, 0, sizeof(src));
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == -EINVAL,
	      "blank plan invalid: %s", why);
	CHECK(il_plan_use_whole_disk(p) == 0, "whole disk: %s",
	      il_last_error(ctx));
	CHECK(il_plan_nops(p) == 5, "5 ops, got %d", il_plan_nops(p));
	CHECK(il_plan_root(p) == 2 && il_plan_esp(p) == 1, "root 2, esp 1");
	CHECK(il_plan_view(p, &v) == 0 && v.nparts == 2 &&
	      v.table == IL_TABLE_GPT, "view 2 parts");
	CHECK(v.parts[0].start_bytes == IL_ALIGN_BYTES &&
	      v.parts[0].size_bytes == IL_ESP_SIZE_BYTES && v.parts[0].is_esp &&
	      !strcmp(v.parts[0].fstype, "vfat") &&
	      !strcmp(v.parts[0].name, IL_ESP_PARTNAME), "view ESP");
	CHECK(v.parts[1].start_bytes == IL_ALIGN_BYTES + IL_ESP_SIZE_BYTES &&
	      v.parts[1].is_likeos_root && !strcmp(v.parts[1].fstype, "ext4") &&
	      v.parts[1].start_bytes + v.parts[1].size_bytes >
	      (20ULL << 30) - IL_ALIGN_BYTES, "view root to the end");
	CHECK(v.nfree == 0, "no free space left (%d)", v.nfree);
	src.uncompressed_bytes = 3ULL << 30;
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == 0,
	      "whole disk valid: %s", why);
	src.uncompressed_bytes = 20ULL << 30;
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == -ENOSPC,
	      "too big an image: %s", why);
	il_op_describe(il_plan_op(p, 0), desc, sizeof(desc));
	CHECK(!strcmp(desc, "Erase the whole disk and create a new GPT "
		      "partition table"), "op 0: %s", desc);
	il_op_describe(il_plan_op(p, 1), desc, sizeof(desc));
	CHECK(!strcmp(desc, "Create partition 1 (536.9 MB, EFI system)"),
	      "op 1: %s", desc);
	il_op_describe(il_plan_op(p, 4), desc, sizeof(desc));
	CHECK(!strcmp(desc, "Format partition 2 as ext4"), "op 4: %s", desc);
	{
		char sum[2048];

		il_plan_summary(p, sum, sizeof(sum));
		CHECK(strstr(sum, "LikeOS is installed on partition 2") != NULL,
		      "summary:\n%s", sum);
	}
	il_plan_free(p);

	/* create in free space on a blank disk: a table, an ESP, the root */
	p = il_plan_new(ctx, &l.disks[0]);
	{
		int partno = -1;

		CHECK(il_plan_create(p, 0, 8ULL << 30, &partno) == 0,
		      "create 8 GiB: %s", il_last_error(ctx));
		CHECK(partno == 2 && il_plan_esp(p) == 1, "ESP added before "
		      "the new partition (%d)", partno);
		il_plan_view(p, &v);
		CHECK(v.nparts == 2 && v.parts[1].size_bytes == 8ULL << 30 &&
		      v.nfree == 1, "view after create");
		CHECK(il_plan_select_root(p, partno) == 0, "select root");
		il_plan_view(p, &v);
		CHECK(v.parts[1].is_likeos_root &&
		      !strcmp(v.parts[1].name, IL_ROOT_PARTNAME),
		      "the new partition became the root partition");
		src.uncompressed_bytes = 1ULL << 30;
		CHECK(il_plan_validate(p, &src, why, sizeof(why)) == 0,
		      "valid: %s", why);
		CHECK(il_plan_extend(p, partno, 0) == 0, "extend to max: %s",
		      il_last_error(ctx));
		il_plan_view(p, &v);
		CHECK(v.nfree == 0, "extended over the rest");
		CHECK(il_plan_select_root(p, 1) == -EINVAL, "ESP refused as root");
		CHECK(il_plan_delete(p, partno) == 0, "delete planned partition");
		CHECK(il_plan_root(p) == -1, "root cleared");
		il_plan_view(p, &v);
		CHECK(v.nparts == 1, "only the ESP is left: %d", v.nparts);
		{
			int n = il_plan_nops(p), creates = 0;

			for (int i = 0; i < n; i++)
				if (il_plan_op(p, i)->kind == IL_OP_CREATE)
					creates++;
			CHECK(creates == 1, "the CREATE went away with the "
			      "partition (%d)", creates);
		}
		CHECK(il_plan_create(p, 0, 100ULL << 40, &partno) == -ENOSPC,
		      "100 TiB does not fit");
	}
	il_plan_free(p);
	il_ctx_free(ctx);
}

static void test_plan_existing(void)
{
	char img[1024], why[512];
	const char *disks[2];
	struct il_disk_list l;
	struct il_disk v;
	struct il_source_info src;
	il_ctx *ctx;
	il_plan *p;
	int partno = -1;

	if (sh("command -v sgdisk >/dev/null") != 0) {
		printf("SKIP plans on sgdisk tables (no sgdisk)\n");
		return;
	}
	wpath(img, sizeof(img), "existing.img");
	make_image(img, 16ULL << 30);
	CHECK(sh("sgdisk -o -n 1:2048:+300M -t 1:ef00 -c 1:'EFI system partition'"
		 " -n 2:0:+4G -t 2:0700 -c 2:Other -n 3:+2G:+2G -t 3:8300 "
		 "'%s' >/dev/null", img) == 0, "sgdisk made the table");
	/* a FAT file system on the ESP so it counts as usable */
	CHECK(sh("mkfs.fat -F 32 -n ESP --offset 2048 '%s' 307200 >/dev/null 2>&1",
		 img) == 0, "mkfs.fat in the ESP");
	disks[0] = img;
	disks[1] = NULL;
	ctx = sim_ctx(disks, "plan-existing.log");
	CHECK(il_disks_scan(ctx, &l) == 0 && l.n == 1, "scan");
	CHECK(l.disks[0].table == IL_TABLE_GPT && l.disks[0].nparts == 3,
	      "3 partitions (%d)", l.disks[0].nparts);
	CHECK(l.disks[0].parts[0].is_esp &&
	      !strcmp(l.disks[0].parts[0].fstype, "vfat") &&
	      !strcmp(l.disks[0].parts[0].fs_label, "ESP"),
	      "ESP with FAT (%s/%s)", l.disks[0].parts[0].fstype,
	      l.disks[0].parts[0].fs_label);
	CHECK(!strcmp(l.disks[0].parts[1].name, "Other") &&
	      !strcmp(l.disks[0].parts[1].type_desc, "Basic data"),
	      "basic data partition: %s / %s", l.disks[0].parts[1].name,
	      l.disks[0].parts[1].type_desc);
	CHECK(l.disks[0].nfree == 2, "two free regions (%d)", l.disks[0].nfree);
	p = il_plan_new(ctx, &l.disks[0]);
	/* the gap between partitions 2 and 3 (2 GiB) */
	CHECK(il_plan_create(p, 0, 0, &partno) == 0, "create in the gap: %s",
	      il_last_error(ctx));
	CHECK(partno == 4, "slot 4 (%d)", partno);
	CHECK(il_plan_esp(p) == 1, "the existing ESP is used");
	CHECK(il_plan_select_root(p, partno) == 0, "select");
	memset(&src, 0, sizeof(src));
	src.uncompressed_bytes = 600ULL << 20;
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == 0,
	      "valid on the existing ESP: %s", why);
	src.uncompressed_bytes = 0;
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == -ENOSPC,
	      "8 GiB default minimum not met: %s", why);
	il_plan_free(p);

	/* delete partition 3 and install there */
	p = il_plan_new(ctx, &l.disks[0]);
	CHECK(il_plan_delete(p, 3) == 0, "delete 3");
	il_plan_view(p, &v);
	CHECK(v.nparts == 2, "2 left");
	CHECK(il_plan_create(p, v.nfree - 1, 0, &partno) == 0 && partno == 3,
	      "create in the merged space (%d): %s", partno, il_last_error(ctx));
	CHECK(il_plan_select_root(p, partno) == 0, "select 3");
	src.uncompressed_bytes = 0;
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == 0,
	      "valid (8 GiB+ free at the end): %s", why);
	il_plan_free(p);

	/* select an existing partition: SET_TYPE + FORMAT */
	p = il_plan_new(ctx, &l.disks[0]);
	CHECK(il_plan_select_root(p, 3) == 0, "select existing 3");
	CHECK(il_plan_nops(p) == 2 && il_plan_op(p, 0)->kind == IL_OP_SET_TYPE &&
	      il_plan_op(p, 1)->kind == IL_OP_FORMAT, "type + format ops");
	CHECK(il_plan_select_root(p, 2) == 0 && il_plan_nops(p) == 2 &&
	      il_plan_op(p, 0)->partno == 2, "choosing again replaces them");
	CHECK(il_plan_select_root(p, 2) == 0 && il_plan_nops(p) == 2 &&
	      il_plan_op(p, 0)->kind == IL_OP_SET_TYPE &&
	      il_plan_op(p, 1)->kind == IL_OP_FORMAT,
	      "choosing the same partition twice keeps its changes");
	CHECK(il_plan_select_root(p, 9) == -ENOENT && il_plan_root(p) == 2 &&
	      il_plan_nops(p) == 2, "a failed choice leaves the plan as it was");
	CHECK(il_plan_extend(p, 2, 0) == 0, "grow 2 into the gap: %s",
	      il_last_error(ctx));
	il_plan_view(p, &v);
	CHECK(il_disk_part(&v, 2) &&
	      il_disk_part(&v, 2)->size_bytes == 6ULL << 30,
	      "partition 2 is 6 GiB now (%llu)", il_disk_part(&v, 2) ?
	      (unsigned long long)il_disk_part(&v, 2)->size_bytes : 0ULL);
	CHECK(il_plan_extend(p, 3, 1ULL << 30) == -EINVAL, "no shrinking");
	il_plan_free(p);
	il_ctx_free(ctx);
}

/* il_plan_can_select_root and il_plan_clear_root */
static void test_plan_root_choice(void)
{
	char img[1024], img2[1024], why[512], err_before[IL_ERROR_MAX];
	const char *disks[3];
	struct il_disk_list l;
	struct il_disk v;
	struct il_source_info src;
	il_ctx *ctx;
	il_plan *p;
	int partno = -1;

	if (sh("command -v sgdisk >/dev/null") != 0) {
		printf("SKIP root choice checks (no sgdisk)\n");
		return;
	}
	wpath(img, sizeof(img), "choice.img");
	make_image(img, 16ULL << 30);
	CHECK(sh("sgdisk -o -n 1:2048:+300M -t 1:ef00 -c 1:'EFI system partition'"
		 " -n 2:0:+4G -t 2:0700 -c 2:data -n 3:+2G:+2G -t 3:8300 "
		 "'%s' >/dev/null", img) == 0, "choice: sgdisk made the table");
	CHECK(sh("mkfs.fat -F 32 -n ESP --offset 2048 '%s' 307200 >/dev/null 2>&1",
		 img) == 0, "choice: mkfs.fat in the ESP");
	/* a disk with one partition and no ESP */
	wpath(img2, sizeof(img2), "noesp.img");
	make_image(img2, 16ULL << 30);
	CHECK(sh("sgdisk -o -n 1:2048:+12G -t 1:8300 '%s' >/dev/null", img2) == 0,
	      "no-ESP disk made");
	disks[0] = img;
	disks[1] = img2;
	disks[2] = NULL;
	ctx = sim_ctx(disks, "plan-choice.log");
	CHECK(il_disks_scan(ctx, &l) == 0 && l.n == 2, "choice: scan");
	memset(&src, 0, sizeof(src));
	src.uncompressed_bytes = 600ULL << 20;

	p = il_plan_new(ctx, &l.disks[0]);
	il_set_error(ctx, EIO, "an earlier error");
	snprintf(err_before, sizeof(err_before), "%s", il_last_error(ctx));
	CHECK(il_plan_can_select_root(p, 3, &src, why, sizeof(why)) == 0 &&
	      !why[0], "partition 3 can hold LikeOS: %s", why);
	CHECK(il_plan_nops(p) == 0 && il_plan_root(p) == -1,
	      "the check did not change the plan");
	CHECK(il_plan_can_select_root(p, 1, &src, why, sizeof(why)) == -EINVAL &&
	      strstr(why, "EFI system") != NULL, "the ESP cannot: %s", why);
	CHECK(il_plan_can_select_root(p, 9, &src, why, sizeof(why)) == -ENOENT,
	      "no partition 9: %s", why);
	src.uncompressed_bytes = 0;
	CHECK(il_plan_can_select_root(p, 3, &src, why, sizeof(why)) == -ENOSPC &&
	      strstr(why, "too small") != NULL,
	      "2 GiB is below the default minimum: %s", why);
	CHECK(!strcmp(il_last_error(ctx), err_before),
	      "the context's error was left alone (%s)", il_last_error(ctx));
	src.uncompressed_bytes = 600ULL << 20;
	CHECK(il_plan_can_select_root(NULL, 3, &src, why, sizeof(why)) == -EINVAL,
	      "no plan");

	/* an existing partition: the type change and the format go again */
	CHECK(il_plan_select_root(p, 3) == 0 && il_plan_nops(p) == 2,
	      "select 3");
	CHECK(il_plan_can_select_root(p, 2, &src, why, sizeof(why)) == 0 &&
	      il_plan_root(p) == 3 && il_plan_nops(p) == 2,
	      "checking another partition keeps the choice: %s", why);
	CHECK(il_plan_clear_root(p) == 0 && il_plan_root(p) == -1 &&
	      il_plan_nops(p) == 0, "clear: nothing left (%d ops)",
	      il_plan_nops(p));
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == -EINVAL,
	      "without a choice the plan is not installable: %s", why);
	CHECK(il_plan_clear_root(p) == 0 && il_plan_nops(p) == 0,
	      "clearing twice is harmless");

	/* a partition the plan created stays, as plain data */
	CHECK(il_plan_create(p, 0, 0, &partno) == 0 && partno == 4,
	      "create 4 in the gap: %s", il_last_error(ctx));
	CHECK(il_plan_select_root(p, partno) == 0, "select 4");
	CHECK(il_plan_clear_root(p) == 0 && il_plan_root(p) == -1 &&
	      il_plan_nops(p) == 1 && il_plan_op(p, 0)->kind == IL_OP_CREATE &&
	      !strcmp(il_plan_op(p, 0)->type_guid, IL_GUID_GENERIC_DATA) &&
	      !il_plan_op(p, 0)->name[0], "clear: the new partition is data");
	il_plan_view(p, &v);
	CHECK(il_disk_part(&v, 4) && !il_disk_part(&v, 4)->is_likeos_root,
	      "view: partition 4 is not the root");
	CHECK(il_plan_can_select_root(p, 4, &src, why, sizeof(why)) == 0,
	      "4 can be chosen again: %s", why);
	CHECK(il_plan_select_root(p, 4) == 0 &&
	      il_plan_validate(p, &src, why, sizeof(why)) == 0,
	      "and is valid then: %s", why);
	il_plan_free(p);

	/* the whole disk: its layout stays, only the choice goes */
	p = il_plan_new(ctx, &l.disks[0]);
	CHECK(il_plan_use_whole_disk(p) == 0 && il_plan_root(p) == 2,
	      "whole disk");
	CHECK(il_plan_clear_root(p) == 0 && il_plan_root(p) == -1 &&
	      il_plan_nops(p) == 5 && il_plan_esp(p) == 1,
	      "clear on a whole disk keeps its partitions");
	CHECK(il_plan_can_select_root(p, 2, &src, why, sizeof(why)) == 0,
	      "partition 2 can be chosen again: %s", why);
	il_plan_free(p);

	/* no ESP on the disk: the check says so */
	p = il_plan_new(ctx, &l.disks[1]);
	CHECK(il_plan_can_select_root(p, 1, &src, why, sizeof(why)) == -EINVAL &&
	      strstr(why, "EFI system partition") != NULL,
	      "no ESP: %s", why);
	CHECK(il_plan_nops(p) == 0, "no-ESP check left the plan alone");
	il_plan_free(p);
	il_ctx_free(ctx);
}

static void test_plan_mbr(void)
{
	char img[1024], why[512];
	const char *disks[2];
	struct il_disk_list l;
	il_ctx *ctx;
	il_plan *p;
	int partno;

	if (sh("command -v sfdisk >/dev/null") != 0) {
		printf("SKIP MBR plans (no sfdisk)\n");
		return;
	}
	wpath(img, sizeof(img), "mbr.img");
	make_image(img, 12ULL << 30);
	CHECK(sh("printf 'label: dos\\n,1G,c\\n,2G,83\\n,,5\\n,1G,83\\n,1G,82\\n' | "
		 "sfdisk -q '%s' >/dev/null 2>&1", img) == 0, "sfdisk dos table");
	disks[0] = img;
	disks[1] = NULL;
	ctx = sim_ctx(disks, "plan-mbr.log");
	CHECK(il_disks_scan(ctx, &l) == 0 && l.n == 1, "scan");
	CHECK(l.disks[0].table == IL_TABLE_MBR && l.disks[0].nparts == 4,
	      "MBR with 4 partitions incl. 2 logical (%d)", l.disks[0].nparts);
	CHECK(l.disks[0].nparts == 4 && l.disks[0].parts[2].number == 5 &&
	      l.disks[0].parts[3].mbr_type == 0x82, "logical numbering");
	p = il_plan_new(ctx, &l.disks[0]);
	CHECK(il_plan_create(p, 0, 0, &partno) == -EMEDIUMTYPE,
	      "create on MBR refused");
	CHECK(il_plan_validate(p, NULL, why, sizeof(why)) == -EINVAL &&
	      strstr(why, "MBR"), "MBR validation: %s", why);
	CHECK(il_plan_new_table(p) == 0 && il_plan_create(p, 0, 0, &partno) == 0,
	      "after a new table: %s", il_last_error(ctx));
	CHECK(il_plan_select_root(p, partno) == 0 &&
	      il_plan_validate(p, NULL, why, sizeof(why)) == 0,
	      "valid after erasing: %s", why);
	il_plan_free(p);
	il_ctx_free(ctx);
}

static void test_plan_guards(void)
{
	struct il_disk d;
	il_ctx *ctx = sim_ctx(NULL, "guards.log");
	il_plan *p;
	char why[256];

	memset(&d, 0, sizeof(d));
	strcpy(d.name, "sdz");
	strcpy(d.dev, "/dev/sdz");
	d.size_bytes = 64ULL << 30;
	d.sector_size = 512;
	d.is_boot_medium = 1;
	il_disk_compute_free(&d);
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_use_whole_disk(p) == -EBUSY, "boot medium refused");
	CHECK(il_plan_validate(p, NULL, why, sizeof(why)) == -EBUSY,
	      "boot medium invalid: %s", why);
	il_plan_free(p);
	d.is_boot_medium = 0;
	d.read_only = 1;
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_use_whole_disk(p) == -EROFS, "read-only refused");
	il_plan_free(p);
	d.read_only = 0;
	d.size_bytes = 300ULL << 20;
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_use_whole_disk(p) == -ENOSPC, "300 MB disk too small");
	il_plan_free(p);
	d.size_bytes = 4ULL << 30;
	d.sector_size = 4096;
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_use_whole_disk(p) == 0, "4K-sector disk: %s",
	      il_last_error(ctx));
	{
		struct il_disk v;

		il_plan_view(p, &v);
		CHECK(v.nparts == 2 && v.parts[0].start_bytes == IL_ALIGN_BYTES &&
		      v.parts[1].start_bytes % IL_ALIGN_BYTES == 0,
		      "4K layout aligned");
	}
	il_plan_free(p);
	il_ctx_free(ctx);
}

/* ------------------------------------------------------------------ */
/* File system probing                                                 */
/* ------------------------------------------------------------------ */

static void test_probe(void)
{
	char img[1024];
	struct il_fsprobe fp;

	wpath(img, sizeof(img), "fat.img");
	make_image(img, 200ULL << 20);
	if (sh("mkfs.fat -F 32 -n LIKEOS-ESP -i 1234abcd '%s' >/dev/null 2>&1",
	       img) == 0) {
		CHECK(il_fs_probe(img, 0, &fp) == 0 && !strcmp(fp.fstype, "vfat") &&
		      !strcmp(fp.label, "LIKEOS-ESP") &&
		      !strcmp(fp.uuid, "1234-ABCD") && fp.free_bytes > 0,
		      "FAT32 probe: %s %s %s free %llu", fp.fstype, fp.label,
		      fp.uuid, (unsigned long long)fp.free_bytes);
	}
	wpath(img, sizeof(img), "ext4.img");
	make_image(img, 256ULL << 20);
	if (sh("mke2fs -F -q -t ext4 -b 4096 -O ^64bit -L likeos-root "
	       "-U 0b1d0b1d-1111-4222-8333-944455556666 '%s' >/dev/null 2>&1",
	       img) == 0) {
		CHECK(il_fs_probe(img, 0, &fp) == 0 && !strcmp(fp.fstype, "ext4") &&
		      !strcmp(fp.label, "likeos-root") &&
		      !strcmp(fp.uuid, "0b1d0b1d-1111-4222-8333-944455556666") &&
		      fp.size_bytes == 256ULL << 20 && fp.free_bytes > 0,
		      "ext4 probe: %s %s %s", fp.fstype, fp.label, fp.uuid);
	}
	wpath(img, sizeof(img), "ext2.img");
	make_image(img, 32ULL << 20);
	if (sh("mke2fs -F -q -t ext2 -L old '%s' >/dev/null 2>&1", img) == 0)
		CHECK(il_fs_probe(img, 0, &fp) == 0 && !strcmp(fp.fstype, "ext2"),
		      "ext2 probe: %s", fp.fstype);
	wpath(img, sizeof(img), "swap.img");
	make_image(img, 32ULL << 20);
	if (sh("mkswap -L sw -U 11111111-2222-4333-8444-555555555555 '%s' "
	       ">/dev/null 2>&1", img) == 0)
		CHECK(il_fs_probe(img, 0, &fp) == 0 && !strcmp(fp.fstype, "swap") &&
		      !strcmp(fp.label, "sw") &&
		      !strcmp(fp.uuid, "11111111-2222-4333-8444-555555555555"),
		      "swap probe: %s %s %s", fp.fstype, fp.label, fp.uuid);
	wpath(img, sizeof(img), "zero.img");
	make_image(img, 1ULL << 20);
	CHECK(il_fs_probe(img, 0, &fp) == -ENOENT, "nothing on zeroes");
}

/* ------------------------------------------------------------------ */
/* Configuration and accounts on a temporary root                      */
/* ------------------------------------------------------------------ */

static int file_has(const char *path, const char *needle)
{
	char *text = NULL;
	int found;

	if (il_read_file_alloc(path, &text, NULL) != 0)
		return 0;
	found = strstr(text, needle) != NULL;
	free(text);
	return found;
}

static int count_lines_starting(const char *path, const char *prefix)
{
	char *text = NULL, *save = NULL;
	int n = 0;

	if (il_read_file_alloc(path, &text, NULL) != 0)
		return -1;
	for (char *l = strtok_r(text, "\n", &save); l;
	     l = strtok_r(NULL, "\n", &save))
		if (il_starts_with(l, prefix))
			n++;
	free(text);
	return n;
}

static void test_config_accounts(const char *resetc)
{
	char root[1024], p[1200];
	struct il_config cfg;
	struct il_target t;
	struct stat st;
	il_ctx *ctx;
	char why[256];

	wpath(root, sizeof(root), "root");
	CHECK(sh("mkdir -p '%s/etc/skel/.config/app' '%s/install/x' "
		 "'%s/usr/share/zoneinfo/Europe' '%s/var/lib/dbus' && "
		 "cp '%s/passwd' '%s/group' '%s/shadow' '%s/hosts' '%s/etc/' && "
		 "echo 'export A=1' > '%s/etc/skel/.profile' && "
		 "echo cfg > '%s/etc/skel/.config/app/rc' && "
		 "ln -s .profile '%s/etc/skel/.bash_profile' && "
		 "echo zone > '%s/usr/share/zoneinfo/Europe/Berlin' && "
		 "echo live > '%s/etc/inittab.install' && "
		 "echo 0123 > '%s/var/lib/dbus/machine-id' && "
		 "printf '# placeholder\\nUUID=@ROOT@ / ext4 defaults 0 1\\n"
		 "tmpfs /tmp tmpfs mode=1777 0 0\\n' > '%s/etc/fstab'",
		 root, root, root, root, resetc, resetc, resetc, resetc, root,
		 root, root, root, root, root, root, root) == 0,
	      "temporary root made");
	/* the installers and their session: on the live medium only */
	CHECK(sh("mkdir -p '%s/usr/sbin' '%s/usr/bin' '%s/usr/libexec' "
		 "'%s/usr/share/likeos/installer' && "
		 "echo cli > '%s/usr/sbin/likeos-install' && "
		 "echo gui > '%s/usr/bin/likeos-installer' && "
		 "echo session > '%s/usr/libexec/likeos-installer-session' && "
		 "echo rc > '%s/usr/share/likeos/installer/xinitrc' && "
		 "echo keep > '%s/usr/sbin/keep-me'",
		 root, root, root, root, root, root, root, root, root) == 0,
	      "installer files in the temporary root");
	ctx = sim_ctx(NULL, "config.log");
	il_config_defaults(&cfg);
	strcpy(cfg.hostname, "testbox");
	strcpy(cfg.timezone, "Europe/Berlin");
	strcpy(cfg.time_format, "en_GB.UTF-8");
	cfg.nlayouts = 2;
	strcpy(cfg.layouts[0], "de");
	strcpy(cfg.layouts[1], "us");
	strcpy(cfg.root_password, "rootpw");
	strcpy(cfg.user.fullname, "Ada: Lovelace, Countess");
	strcpy(cfg.user.username, "ada");
	strcpy(cfg.user.password, "adapw");
	CHECK(il_config_validate(&cfg, why, sizeof(why)) == -EINVAL,
	      "':' in the full name refused: %s", why);
	strcpy(cfg.user.fullname, "Ada Lovelace, Countess");
	CHECK(il_config_validate(&cfg, why, sizeof(why)) == 0, "valid: %s", why);
	memset(&t, 0, sizeof(t));
	strcpy(t.root_dir, root);
	snprintf(t.esp_dir, sizeof(t.esp_dir), "%s/boot/efi", root);
	strcpy(t.root_fsuuid, "0b1d0b1d-1111-4222-8333-944455556666");
	strcpy(t.esp_fsuuid, "1234-ABCD");

	for (int round = 0; round < 2; round++) {
		CHECK(il_configure_target(ctx, &cfg, &t) == 0, "configure %d: %s",
		      round, il_last_error(ctx));
		CHECK(il_accounts_apply(ctx, root, &cfg) == 0, "accounts %d: %s",
		      round, il_last_error(ctx));
	}
	snprintf(p, sizeof(p), "%s/etc/fstab", root);
	CHECK(file_has(p, "UUID=0b1d0b1d-1111-4222-8333-944455556666  /          "
		       "ext4  defaults                    0  1\n") &&
	      file_has(p, "UUID=1234-ABCD  /boot/efi  vfat  "
		       "umask=0077,shortname=mixed  0  2\n") &&
	      file_has(p, "tmpfs /tmp tmpfs mode=1777 0 0\n") &&
	      !file_has(p, "@ROOT@") && count_lines_starting(p, "UUID=") == 2,
	      "fstab");
	snprintf(p, sizeof(p), "%s/etc/hostname", root);
	CHECK(file_has(p, "testbox\n"), "hostname");
	snprintf(p, sizeof(p), "%s/etc/hosts", root);
	CHECK(file_has(p, "127.0.1.1   testbox\n") &&
	      file_has(p, "::1         localhost") && !file_has(p, "r00tbox"),
	      "hosts");
	snprintf(p, sizeof(p), "%s/etc/likeos/keyboard.conf", root);
	CHECK(file_has(p, "\nLAYOUT=de,us\nVARIANT=\nOPTIONS=grp:alt_shift_toggle\n"),
	      "keyboard.conf");
	snprintf(p, sizeof(p), "%s/etc/timezone", root);
	CHECK(file_has(p, "Europe/Berlin\n"), "timezone");
	{
		char link[256];
		ssize_t n;

		snprintf(p, sizeof(p), "%s/etc/localtime", root);
		n = readlink(p, link, sizeof(link) - 1);
		if (n > 0)
			link[n] = 0;
		CHECK(n > 0 && !strcmp(link, "/usr/share/zoneinfo/Europe/Berlin"),
		      "localtime link");
	}
	snprintf(p, sizeof(p), "%s/etc/locale.conf", root);
	CHECK(file_has(p, "LANG=en_US.UTF-8\nLC_TIME=en_GB.UTF-8\n"), "locale");
	snprintf(p, sizeof(p), "%s/etc/machine-id", root);
	{
		char id[64];

		CHECK(il_read_line(p, id, sizeof(id)) == 0 && strlen(id) == 32 &&
		      strspn(id, "0123456789abcdef") == 32 && id[12] == '4',
		      "machine-id: %s", id);
		CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0444,
		      "machine-id mode");
	}
	snprintf(p, sizeof(p), "%s/var/lib/dbus/machine-id", root);
	CHECK(lstat(p, &st) == 0 && S_ISLNK(st.st_mode), "dbus link");
	snprintf(p, sizeof(p), "%s/install", root);
	CHECK(!il_file_exists(p), "/install removed");
	snprintf(p, sizeof(p), "%s/etc/inittab.install", root);
	CHECK(!il_file_exists(p), "inittab.install removed");
	{
		static const char *const gone[] = {
			"/usr/sbin/likeos-install", "/usr/bin/likeos-installer",
			"/usr/libexec/likeos-installer-session",
			"/usr/share/likeos/installer", NULL,
		};

		for (int i = 0; gone[i]; i++) {
			snprintf(p, sizeof(p), "%s%s", root, gone[i]);
			CHECK(!il_file_exists(p), "%s removed", gone[i]);
		}
		snprintf(p, sizeof(p), "%s/usr/sbin/keep-me", root);
		CHECK(il_file_exists(p), "other programs in /usr/sbin kept");
		snprintf(p, sizeof(p), "%s/usr/share/likeos", root);
		CHECK(il_file_exists(p), "/usr/share/likeos itself kept");
	}

	snprintf(p, sizeof(p), "%s/etc/passwd", root);
	CHECK(file_has(p, "\nada:x:1000:1000:Ada Lovelace Countess:/home/ada:"
		       "/bin/bash\n"), "passwd line");
	CHECK(count_lines_starting(p, "ada:") == 1, "one passwd line");
	CHECK(file_has(p, "root:x:0:0:root:/root:/bin/bash\n"), "root kept");
	CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0644, "passwd 0644");
	snprintf(p, sizeof(p), "%s/etc/group", root);
	CHECK(file_has(p, "\naudio:x:29:ada\n") && file_has(p, "\nrender:x:108:ada\n")
	      && file_has(p, "\nusers:x:100:ada\n") &&
	      file_has(p, "\nada:x:1000:\n") && file_has(p, "\nvideo:x:44:\n"),
	      "groups");
	CHECK(count_lines_starting(p, "users:") == 1, "one users group");
	snprintf(p, sizeof(p), "%s/etc/shadow", root);
	CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600, "shadow 0600");
	{
		char *text = NULL, *save = NULL;
		int ok_root = 0, ok_ada = 0;

		il_read_file_alloc(p, &text, NULL);
		for (char *l = text ? strtok_r(text, "\n", &save) : NULL; l;
		     l = strtok_r(NULL, "\n", &save)) {
			char name[64], hash[256];
			long days;
			struct crypt_data cd;
			const char *h;

			if (sscanf(l, "%63[^:]:%255[^:]:%ld:0:99999:7:::", name,
				   hash, &days) != 3)
				continue;
			memset(&cd, 0, sizeof(cd));
			if (!strcmp(name, "root")) {
				h = crypt_r("rootpw", hash, &cd);
				ok_root = h && !strcmp(h, hash) &&
					  il_starts_with(hash, "$y$") &&
					  days == il_days_since_epoch();
			} else if (!strcmp(name, "ada")) {
				h = crypt_r("adapw", hash, &cd);
				ok_ada = h && !strcmp(h, hash) &&
					 il_starts_with(hash, "$y$");
			}
		}
		free(text);
		CHECK(ok_root, "root's yescrypt hash verifies");
		CHECK(ok_ada, "ada's yescrypt hash verifies");
		CHECK(count_lines_starting(p, "ada:") == 1, "one shadow line");
	}
	snprintf(p, sizeof(p), "%s/home/ada", root);
	CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode) &&
	      (st.st_mode & 0777) == 0700, "home 0700");
	snprintf(p, sizeof(p), "%s/home/ada/.config/app/rc", root);
	CHECK(file_has(p, "cfg\n"), "skel copied (deep)");
	snprintf(p, sizeof(p), "%s/home/ada/.bash_profile", root);
	CHECK(lstat(p, &st) == 0 && S_ISLNK(st.st_mode), "skel symlink kept");
	/* A home that is a symbolic link out of the target is refused and
	 * what it points at keeps its mode. */
	{
		char outside[1024], link[1200];

		wpath(outside, sizeof(outside), "outside-home");
		snprintf(link, sizeof(link), "%s/home/bob", root);
		CHECK(sh("echo x > '%s' && chmod 0644 '%s' && ln -s '%s' '%s'",
			 outside, outside, outside, link) == 0, "plant home link");
		il_strlcpy(cfg.user.username, "bob", sizeof(cfg.user.username));
		CHECK(il_accounts_apply(ctx, root, &cfg) == -EEXIST,
		      "home symlink refused: %s", il_last_error(ctx));
		CHECK(stat(outside, &st) == 0 && (st.st_mode & 07777) == 0644,
		      "the link's target keeps its mode (%o)",
		      (unsigned)(st.st_mode & 07777));
	}
	il_ctx_free(ctx);
}

/* ------------------------------------------------------------------ */
/* The disk the running system lives on, looked up again when writing */
/* ------------------------------------------------------------------ */

static void test_boot_medium_recheck(void)
{
	char sys[1024], img[1200], mounts[1200];
	struct il_options o;
	struct il_disk_list *dl;
	struct il_target tg;
	il_plan *p = NULL;
	il_ctx *ctx;
	char log[1024];

	wpath(sys, sizeof(sys), "sysroot-boot");
	snprintf(img, sizeof(img), "%s/dev/ilzz", sys);
	snprintf(mounts, sizeof(mounts), "%s/proc/mounts", sys);
	CHECK(sh("mkdir -p '%s/sys/block/ilzz/queue' '%s/sys/block/ilzz/ilzz2' "
		 "'%s/proc' '%s/dev' && cd '%s/sys/block/ilzz' && "
		 "echo 4194304 > size && echo 0 > ro && echo 0 > removable && "
		 "echo 512 > queue/logical_block_size && "
		 "echo 2 > ilzz2/partition && echo 2048 > ilzz2/start && "
		 "echo 4096 > ilzz2/size && "
		 "echo '/dev/sda1 / ext4 rw 0 0' > '%s' && "
		 "truncate -s 2G '%s'", sys, sys, sys, sys, sys, mounts,
		 img) == 0, "fake sysroot");
	wpath(log, sizeof(log), "bootrecheck.log");
	memset(&o, 0, sizeof(o));
	o.sysroot = sys;
	o.log_path = log;
	o.verbose = getenv("IL_TEST_VERBOSE") != NULL;
	ctx = il_ctx_new(&o);
	dl = calloc(1, sizeof(*dl));
	CHECK(ctx && dl && il_disks_scan(ctx, dl) == 0 && dl->n == 1 &&
	      !dl->disks[0].is_boot_medium, "the disk is not the boot medium "
	      "at the scan");
	if (dl && dl->n == 1) {
		p = il_plan_new(ctx, &dl->disks[0]);
		CHECK(il_plan_use_whole_disk(p) == 0, "whole disk: %s",
		      il_last_error(ctx));
	}
	/* The running system's root moved onto that disk since the scan. */
	CHECK(sh("echo '/dev/ilzz2 / ext4 rw 0 0' > '%s'", mounts) == 0,
	      "root now on the disk");
	if (p) {
		memset(&tg, 0, sizeof(tg));
		CHECK(il_partition_apply(ctx, p, &tg) == -EBUSY,
		      "partitioning refused: %s", il_last_error(ctx));
		CHECK(sh("cmp -s -n 1048576 '%s' /dev/zero", img) == 0,
		      "the disk was not written");
	}
	il_plan_free(p);
	free(dl);
	il_ctx_free(ctx);
}

/* ------------------------------------------------------------------ */
/* The disk is identified again right before it is written            */
/* ------------------------------------------------------------------ */

/* The disk GUID sgdisk reports for an image, lower case. */
static void disk_guid_of(const char *img, char *out, size_t len)
{
	char cmd[1200];
	FILE *f;

	out[0] = '\0';
	snprintf(cmd, sizeof(cmd), "sgdisk -p '%s' 2>/dev/null | sed -n "
		 "'s/^Disk identifier (GUID): //p' | tr A-F a-f", img);
	f = popen(cmd, "r");
	if (!f)
		return;
	if (fgets(out, (int)len, f))
		out[strcspn(out, "\n")] = '\0';
	pclose(f);
}

static void test_disk_identity(void)
{
	char img[1024], guid[64], again[64];
	const char *disks[2];
	struct il_disk_list *l = calloc(1, sizeof(*l));
	struct il_disk *other = calloc(1, sizeof(*other));
	struct il_target tg;
	il_ctx *ctx;
	il_plan *p;

	if (sh("command -v sgdisk >/dev/null") != 0) {
		printf("SKIP disk identity (no sgdisk)\n");
		free(l);
		free(other);
		return;
	}
	wpath(img, sizeof(img), "ident.img");
	make_image(img, 12ULL << 30);
	CHECK(sh("sgdisk -o -U 11111111-2222-4333-8444-555555555555 "
		 "-n 1:2048:+300M -t 1:ef00 -n 2:0:+9G -t 2:8300 '%s' "
		 ">/dev/null", img) == 0, "ident: sgdisk made the table");
	CHECK(sh("mkfs.fat -F 32 --offset 2048 '%s' 307200 >/dev/null 2>&1",
		 img) == 0, "ident: FAT in the ESP");
	disks[0] = img;
	disks[1] = NULL;
	ctx = sim_ctx(disks, "ident.log");
	CHECK(il_disks_scan(ctx, l) == 0 && l->n == 1 &&
	      !strcmp(l->disks[0].disk_guid,
		      "11111111-2222-4333-8444-555555555555"),
	      "ident: scan (%s)", l->n ? l->disks[0].disk_guid : "");

	/* Another disk GUID at the node: refused, nothing written. */
	CHECK(sh("sgdisk -U 99999999-2222-4333-8444-555555555555 '%s' "
		 ">/dev/null", img) == 0, "ident: new disk GUID");
	for (int whole = 0; whole < 2; whole++) {
		p = il_plan_new(ctx, &l->disks[0]);
		CHECK(whole ? il_plan_use_whole_disk(p) == 0
			    : il_plan_select_root(p, 2) == 0,
		      "ident: plan %d: %s", whole, il_last_error(ctx));
		memset(&tg, 0, sizeof(tg));
		CHECK(il_partition_apply(ctx, p, &tg) == -ESTALE &&
		      !strcmp(il_last_error(ctx), "The disks changed since "
			      "they were listed; choose the disk again."),
		      "ident: another disk GUID refused (%s): %s",
		      whole ? "whole disk" : "one partition",
		      il_last_error(ctx));
		disk_guid_of(img, guid, sizeof(guid));
		CHECK(!strcmp(guid, "99999999-2222-4333-8444-555555555555"),
		      "ident: the table was not written (%s)", guid);
		il_plan_free(p);
	}
	/* A GPT where the scan saw none. */
	*other = l->disks[0];
	other->table = IL_TABLE_NONE;
	other->disk_guid[0] = '\0';
	other->nparts = 0;
	il_disk_compute_free(other);
	p = il_plan_new(ctx, other);
	CHECK(il_plan_use_whole_disk(p) == 0, "ident: blank plan");
	memset(&tg, 0, sizeof(tg));
	CHECK(il_partition_apply(ctx, p, &tg) == -ESTALE,
	      "ident: a GPT that was not there refused: %s",
	      il_last_error(ctx));
	il_plan_free(p);

	/* Listed again: it is installable (simulate mode works). */
	CHECK(il_disks_scan(ctx, l) == 0 && l->n == 1, "ident: scan again");
	p = il_plan_new(ctx, &l->disks[0]);
	CHECK(il_plan_select_root(p, 2) == 0, "ident: select 2");
	memset(&tg, 0, sizeof(tg));
	CHECK(il_partition_apply(ctx, p, &tg) == 0 && tg.sector_size == 512,
	      "ident: written after the new listing: %s", il_last_error(ctx));
	disk_guid_of(img, again, sizeof(again));
	CHECK(!strcmp(again, "99999999-2222-4333-8444-555555555555"),
	      "ident: the disk keeps its GUID (%s)", again);
	il_plan_free(p);

	/* Another serial number or model than listed. */
	CHECK(il_disks_scan(ctx, l) == 0 && l->n == 1, "ident: scan 3");
	*other = l->disks[0];
	strcpy(other->serial, "S3RIAL");
	p = il_plan_new(ctx, other);
	CHECK(il_plan_select_root(p, 2) == 0, "ident: select (serial)");
	memset(&tg, 0, sizeof(tg));
	CHECK(il_partition_apply(ctx, p, &tg) == -ESTALE,
	      "ident: serial number differs: %s", il_last_error(ctx));
	il_plan_free(p);
	*other = l->disks[0];
	strcpy(other->model, "other.img");
	p = il_plan_new(ctx, other);
	CHECK(il_plan_select_root(p, 2) == 0, "ident: select (model)");
	memset(&tg, 0, sizeof(tg));
	CHECK(il_partition_apply(ctx, p, &tg) == -ESTALE,
	      "ident: model differs: %s", il_last_error(ctx));
	il_plan_free(p);
	/* Another size. */
	*other = l->disks[0];
	other->size_bytes += 1ULL << 20;
	p = il_plan_new(ctx, other);
	CHECK(il_plan_select_root(p, 2) == 0, "ident: select (size)");
	memset(&tg, 0, sizeof(tg));
	CHECK(il_partition_apply(ctx, p, &tg) == -ESTALE &&
	      strstr(il_last_error(ctx), "choose the disk again"),
	      "ident: size differs: %s", il_last_error(ctx));
	il_plan_free(p);
	il_ctx_free(ctx);
	free(l);
	free(other);
}

/* The same through sysfs: model and serial number read again. */
static void test_disk_identity_sysfs(void)
{
	char sys[1024], img[1200], blk[1200], log[1024];
	struct il_options o;
	struct il_disk_list *dl = calloc(1, sizeof(*dl));
	struct il_target tg;
	il_plan *p = NULL;
	il_ctx *ctx;

	wpath(sys, sizeof(sys), "sysroot-ident");
	snprintf(img, sizeof(img), "%s/dev/ilyy", sys);
	snprintf(blk, sizeof(blk), "%s/sys/block/ilyy", sys);
	CHECK(sh("mkdir -p '%s/queue' '%s/device' '%s/proc' '%s/dev' && "
		 "cd '%s' && echo 4194304 > size && echo 0 > ro && "
		 "echo 0 > removable && echo 512 > queue/logical_block_size && "
		 "echo 'Disk One' > device/model && "
		 "echo SER-1 > device/serial && echo ATA > device/vendor && "
		 "echo '/dev/sda1 / ext4 rw 0 0' > '%s/proc/mounts' && "
		 "truncate -s 2G '%s'", blk, blk, sys, sys, blk, sys, img) == 0,
	      "ident sysfs: fake sysroot");
	wpath(log, sizeof(log), "ident-sysfs.log");
	memset(&o, 0, sizeof(o));
	o.sysroot = sys;
	o.log_path = log;
	o.verbose = getenv("IL_TEST_VERBOSE") != NULL;
	ctx = il_ctx_new(&o);
	CHECK(ctx && dl && il_disks_scan(ctx, dl) == 0 && dl->n == 1 &&
	      !strcmp(dl->disks[0].model, "Disk One") &&
	      !strcmp(dl->disks[0].serial, "SER-1"),
	      "ident sysfs: scan (%s / %s)", dl && dl->n ? dl->disks[0].model :
	      "", dl && dl->n ? dl->disks[0].serial : "");
	if (dl && dl->n == 1) {
		p = il_plan_new(ctx, &dl->disks[0]);
		CHECK(il_plan_use_whole_disk(p) == 0, "ident sysfs: whole disk");
	}
	if (p) {
		/* another disk took the name since the listing */
		CHECK(sh("echo SER-2 > '%s/device/serial'", blk) == 0,
		      "ident sysfs: serial changed");
		memset(&tg, 0, sizeof(tg));
		CHECK(il_partition_apply(ctx, p, &tg) == -ESTALE &&
		      strstr(il_last_error(ctx), "choose the disk again"),
		      "ident sysfs: serial number differs: %s",
		      il_last_error(ctx));
		CHECK(sh("echo SER-1 > '%s/device/serial' && "
			 "echo 'Disk Two' > '%s/device/model'", blk, blk) == 0,
		      "ident sysfs: model changed");
		CHECK(il_partition_apply(ctx, p, &tg) == -ESTALE,
		      "ident sysfs: model differs: %s", il_last_error(ctx));
		/* the same disk: the check passes (the node itself, /dev/ilyy
		 * of the host, does not exist) */
		CHECK(sh("echo 'Disk One' > '%s/device/model'", blk) == 0,
		      "ident sysfs: model back");
		CHECK(il_partition_apply(ctx, p, &tg) != -ESTALE &&
		      !strstr(il_last_error(ctx), "choose the disk again"),
		      "ident sysfs: the same disk passes: %s",
		      il_last_error(ctx));
		CHECK(sh("cmp -s -n 1048576 '%s' /dev/zero", img) == 0,
		      "ident sysfs: the disk was not written");
	}
	il_plan_free(p);
	free(dl);
	il_ctx_free(ctx);
}

/* ------------------------------------------------------------------ */
/* FAT32 on the EFI system partition                                   */
/* ------------------------------------------------------------------ */

/* A GPT disk with an ESP of esp_mib (FAT already on it) and a root. */
static void esp_disk(struct il_disk *d, uint32_t ss, uint64_t esp_mib)
{
	memset(d, 0, sizeof(*d));
	strcpy(d->name, "sdq");
	strcpy(d->dev, "/dev/sdq");
	d->size_bytes = 64ULL << 30;
	d->sector_size = ss;
	d->phys_sector_size = ss;
	d->table = IL_TABLE_GPT;
	strcpy(d->disk_guid, "11111111-2222-4333-8444-555555555555");
	d->nparts = 2;
	d->parts[0].number = 1;
	d->parts[0].start_bytes = 1ULL << 20;
	d->parts[0].size_bytes = esp_mib << 20;
	strcpy(d->parts[0].type_guid, IL_GUID_ESP);
	strcpy(d->parts[0].fstype, "vfat");
	d->parts[0].is_esp = 1;
	d->parts[1].number = 2;
	d->parts[1].start_bytes = (esp_mib + 1) << 20;
	d->parts[1].size_bytes = 16ULL << 30;
	strcpy(d->parts[1].type_guid, IL_GUID_GENERIC_DATA);
	il_disk_compute_free(d);
}

static void test_esp_fat32_min(void)
{
	struct il_disk d;
	struct il_source_info src;
	il_ctx *ctx = sim_ctx(NULL, "esp-fat32.log");
	il_plan *p;
	char why[512];

	memset(&src, 0, sizeof(src));
	src.uncompressed_bytes = 600ULL << 20;

	/* 4096-byte sectors, ESP of 200 MiB: kept as it is, fine */
	esp_disk(&d, 4096, 200);
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_select_root(p, 2) == 0 &&
	      il_plan_validate(p, &src, why, sizeof(why)) == 0,
	      "4K: a kept 200 MiB ESP is fine: %s", why);
	/* formatted, it is too small for FAT32 */
	CHECK(il_plan_format(p, 1) == 0, "4K: format the ESP");
	CHECK(il_plan_validate(p, &src, why, sizeof(why)) == -ENOSPC &&
	      strstr(why, "FAT32") && strstr(why, "4096-byte"),
	      "4K: 200 MiB ESP refused for FAT32: %s", why);
	il_plan_free(p);
	/* 256 MiB: still too few clusters; 257 MiB: enough */
	esp_disk(&d, 4096, 256);
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_format(p, 1) == 0 &&
	      il_plan_can_select_root(p, 2, &src, why, sizeof(why)) ==
	      -ENOSPC && strstr(why, "FAT32"),
	      "4K: 256 MiB ESP refused, also when choosing the root: %s", why);
	il_plan_free(p);
	esp_disk(&d, 4096, 257);
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_format(p, 1) == 0 && il_plan_select_root(p, 2) == 0 &&
	      il_plan_validate(p, &src, why, sizeof(why)) == 0,
	      "4K: 257 MiB ESP formatted: %s", why);
	il_plan_free(p);
	/* 512-byte sectors: the 100 MiB floor is the limit */
	esp_disk(&d, 512, 100);
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_format(p, 1) == 0 && il_plan_select_root(p, 2) == 0 &&
	      il_plan_validate(p, &src, why, sizeof(why)) == 0,
	      "512: 100 MiB ESP formatted: %s", why);
	il_plan_free(p);
	esp_disk(&d, 512, 99);
	p = il_plan_new(ctx, &d);
	CHECK(il_plan_format(p, 1) == 0 && il_plan_select_root(p, 2) == 0 &&
	      il_plan_validate(p, &src, why, sizeof(why)) == -ENOSPC &&
	      strstr(why, "100 MB"), "512: 99 MiB ESP refused: %s", why);
	il_plan_free(p);
	/* the whole-disk layout (512 MiB ESP) is valid with both */
	for (uint32_t ss = 512; ss <= 4096; ss *= 8) {
		esp_disk(&d, ss, 200);
		p = il_plan_new(ctx, &d);
		CHECK(il_plan_use_whole_disk(p) == 0 &&
		      il_plan_validate(p, &src, why, sizeof(why)) == 0,
		      "whole disk with %u-byte sectors: %s", ss, why);
		il_plan_free(p);
	}
	il_ctx_free(ctx);
}

/* mkfs.fat is told the logical sector size when it is not 512. */
static void test_format_esp_sector_size(void)
{
	static const uint32_t sizes[] = { 512, 4096 };
	struct il_target t;
	il_ctx *ctx = sim_ctx(NULL, "format-esp.log");

	if (!getenv("IL_MKFS_FAT")) {
		printf("SKIP mkfs.fat sector size (IL_MKFS_FAT not set)\n");
		il_ctx_free(ctx);
		return;
	}
	for (int i = 0; i < 2; i++) {
		unsigned char bs[128];
		char fs[1200];
		int fd;

		memset(&t, 0, sizeof(t));
		snprintf(t.disk_dev, sizeof(t.disk_dev), "%s/esp%u.img", g_work,
			 sizes[i]);
		t.esp_partno = 1;
		t.esp_size = 300ULL << 20;
		t.format_esp = 1;
		t.sector_size = sizes[i];
		CHECK(il_format_esp(ctx, &t) == 0, "mkfs.fat (%u): %s",
		      sizes[i], il_last_error(ctx));
		snprintf(fs, sizeof(fs), "%s.p1.fs", t.disk_dev);
		memset(bs, 0, sizeof(bs));
		fd = open(fs, O_RDONLY);
		if (fd >= 0) {
			if (read(fd, bs, sizeof(bs)) != (ssize_t)sizeof(bs))
				memset(bs, 0, sizeof(bs));
			close(fd);
		}
		CHECK((uint32_t)(bs[11] | bs[12] << 8) == sizes[i] &&
		      !memcmp(bs + 82, "FAT32   ", 8),
		      "FAT32 with %u-byte sectors (boot sector says %u)",
		      sizes[i], (unsigned)(bs[11] | bs[12] << 8));
		CHECK(sh("fsck.fat -n '%s' >/dev/null 2>&1", fs) == 0,
		      "fsck.fat accepts the %u-byte one", sizes[i]);
		unlink(fs);
	}
	il_ctx_free(ctx);
}

/* ------------------------------------------------------------------ */
/* The account's primary group                                         */
/* ------------------------------------------------------------------ */

/* A root with the image's account files and extra lines. */
static void acct_root(char *root, size_t len, const char *name,
		      const char *resetc, const char *passwd_add,
		      const char *group_add)
{
	wpath(root, len, name);
	CHECK(sh("rm -rf '%s' && mkdir -p '%s/etc' && "
		 "cp '%s/passwd' '%s/group' '%s/shadow' '%s/etc/' && "
		 "printf '%s' >> '%s/etc/passwd' && "
		 "printf '%s' >> '%s/etc/group'", root, root, resetc, resetc,
		 resetc, root, passwd_add, root, group_add, root) == 0,
	      "account root %s", name);
}

/* The passwd line of user and whether its gid has a group line. */
static int primary_group_ok(const char *root, const char *user,
			    char *pwline, size_t len)
{
	char p[1200], *text = NULL, *save = NULL, gid[32] = "";
	int found = 0;

	pwline[0] = '\0';
	snprintf(p, sizeof(p), "%s/etc/passwd", root);
	if (il_read_file_alloc(p, &text, NULL) != 0)
		return 0;
	for (char *l = strtok_r(text, "\n", &save); l;
	     l = strtok_r(NULL, "\n", &save))
		if (il_starts_with(l, user) && l[strlen(user)] == ':') {
			il_strlcpy(pwline, l, len);
			sscanf(l, "%*[^:]:%*[^:]:%*[^:]:%31[^:]", gid);
		}
	free(text);
	snprintf(p, sizeof(p), "%s/etc/group", root);
	if (!gid[0] || il_read_file_alloc(p, &text, NULL) != 0)
		return 0;
	save = NULL;
	for (char *l = strtok_r(text, "\n", &save); l;
	     l = strtok_r(NULL, "\n", &save)) {
		char g[32] = "";

		if (sscanf(l, "%*[^:]:%*[^:]:%31[^:]", g) == 1 &&
		    !strcmp(g, gid))
			found = 1;
	}
	free(text);
	return found;
}

static void test_accounts_group_gid(const char *resetc)
{
	char root[1024], p[1200], pw[256];
	struct il_config cfg;
	il_ctx *ctx = sim_ctx(NULL, "accounts-gid.log");

	il_config_defaults(&cfg);
	strcpy(cfg.root_password, "rootpw");
	strcpy(cfg.user.fullname, "Ada");
	strcpy(cfg.user.username, "ada");
	strcpy(cfg.user.password, "adapw");

	/* The image has a group "ada" (gid 1001); 1000 is free in both. */
	acct_root(root, sizeof(root), "acct-own-group", resetc, "",
		  "ada:x:1001:\n");
	CHECK(il_accounts_apply(ctx, root, &cfg) == 0, "own group: %s",
	      il_last_error(ctx));
	CHECK(primary_group_ok(root, "ada", pw, sizeof(pw)) &&
	      il_starts_with(pw, "ada:x:1001:1001:"),
	      "own group: its gid is the primary one (%s)", pw);
	snprintf(p, sizeof(p), "%s/etc/group", root);
	CHECK(count_lines_starting(p, "ada:") == 1 &&
	      file_has(p, "\nada:x:1001:\n"), "own group: one group line");
	CHECK(il_accounts_apply(ctx, root, &cfg) == 0 &&
	      primary_group_ok(root, "ada", pw, sizeof(pw)) &&
	      il_starts_with(pw, "ada:x:1001:1001:"),
	      "own group: again (%s): %s", pw, il_last_error(ctx));

	/* The same, with uid 1001 taken: another uid, the group's gid. */
	acct_root(root, sizeof(root), "acct-own-group-uid", resetc,
		  "bob:x:1001:100:Bob:/home/bob:/bin/sh\n", "ada:x:1001:\n");
	CHECK(il_accounts_apply(ctx, root, &cfg) == 0 &&
	      primary_group_ok(root, "ada", pw, sizeof(pw)) &&
	      il_starts_with(pw, "ada:x:1000:1001:"),
	      "own group, uid taken (%s): %s", pw, il_last_error(ctx));

	/* A system group of that name is refused. */
	acct_root(root, sizeof(root), "acct-sys-group", resetc, "",
		  "ada:x:50:\n");
	CHECK(il_accounts_apply(ctx, root, &cfg) == -EEXIST &&
	      strstr(il_last_error(ctx), "system group"),
	      "system group refused: %s", il_last_error(ctx));
	acct_root(root, sizeof(root), "acct-nogroup", resetc, "",
		  "ada:x:65534:\n");
	CHECK(il_accounts_apply(ctx, root, &cfg) == -EEXIST,
	      "gid 65534 refused: %s", il_last_error(ctx));

	/* No such group: the first id free in both. */
	acct_root(root, sizeof(root), "acct-fresh", resetc,
		  "bob:x:1000:1000:Bob:/home/bob:/bin/sh\n",
		  "bob:x:1000:\ndevs:x:1001:\n");
	CHECK(il_accounts_apply(ctx, root, &cfg) == 0 &&
	      primary_group_ok(root, "ada", pw, sizeof(pw)) &&
	      il_starts_with(pw, "ada:x:1002:1002:"),
	      "fresh ids (%s): %s", pw, il_last_error(ctx));
	snprintf(p, sizeof(p), "%s/etc/group", root);
	CHECK(file_has(p, "\nada:x:1002:\n"), "fresh ids: the group line");

	/* An existing account whose gid has no group line, next to a group
	 * of its name: that group becomes the primary one. */
	acct_root(root, sizeof(root), "acct-retry", resetc,
		  "ada:x:1000:1000:Ada:/home/ada:/bin/bash\n", "ada:x:1001:\n");
	CHECK(il_accounts_apply(ctx, root, &cfg) == 0 &&
	      primary_group_ok(root, "ada", pw, sizeof(pw)) &&
	      il_starts_with(pw, "ada:x:1000:1001:"),
	      "existing account repointed (%s): %s", pw, il_last_error(ctx));
	il_ctx_free(ctx);
}

/* ---- the progress model (il_progress.c) ----------------------------------
 *
 * An unpacking played on a simulated clock, shaped like the real image: the
 * archive is sorted by name, so a few hundred big files come first (85 %
 * of the bytes) and then thousands of small ones.  Each entry costs
 * a * MiB + b seconds.  The model must find that out and say so. */

#define PM_BIG       300
#define PM_FILES     16624
#define PM_BYTES     1300000000ULL
#define PM_BIG_BYTES 1105000000ULL

struct pm_run {
	double a, b;             /* the true costs: s per MiB, s per file */
	double cum[PM_FILES];    /* end time of each entry, seconds */
	uint64_t size[PM_FILES];
	double x_secs;           /* the whole unpacking */
	double post[3];          /* configure, boot loader, finalize */
	/* findings */
	int backwards, pct_bad, pct_100_early, eta_early, eta_lazy;
	int eta_far, eta_jumpy, short_while_long;
	double worst_ratio_lo, worst_ratio_hi;
	double frac_end_of_big;  /* the unpacking's share after the big files */
	int final_pct;
};

static void pm_entries(struct pm_run *r)
{
	uint64_t small = (PM_BYTES - PM_BIG_BYTES) / (PM_FILES - PM_BIG);
	double t = 0.0;

	for (int i = 0; i < PM_FILES; i++) {
		r->size[i] = i < PM_BIG ? PM_BIG_BYTES / PM_BIG : small;
		t += r->a * (double)r->size[i] / (1024.0 * 1024.0) + r->b;
		r->cum[i] = t;
	}
	r->x_secs = t;
}

/* Bytes and entries done at second t of the unpacking. */
static void pm_at(const struct pm_run *r, double t, uint64_t *bytes,
		  uint64_t *files)
{
	uint64_t b = 0;
	int i = 0;

	while (i < PM_FILES && r->cum[i] <= t)
		b += r->size[i++];
	if (i < PM_FILES) {
		double start = i ? r->cum[i - 1] : 0.0;
		double bt = r->a * (double)r->size[i] / (1024.0 * 1024.0);
		double in = t - start;

		if (bt > 0.0 && in > 0.0)
			b += (uint64_t)((double)r->size[i] *
					(in < bt ? in / bt : 1.0));
	}
	*bytes = b;
	*files = (uint64_t)i;
}

/* The true time left at second t of the unpacking. */
static double pm_true_left(const struct pm_run *r, double t_in_extract)
{
	double left = r->x_secs - t_in_extract;

	if (left < 0.0)
		left = 0.0;
	return left + r->post[0] + r->post[1] + r->post[2];
}

static void pm_check(struct pm_run *r, struct il_track *t, uint64_t now,
		     double true_left, int judge, struct il_progress *last)
{
	struct il_progress p;

	memset(&p, 0, sizeof(p));
	il_track_report(t, now, &p);
	if (p.overall + 1e-12 < last->overall || p.percent < last->percent ||
	    p.step < last->step ||
	    (p.step == last->step && p.step_fraction + 1e-12 <
	     last->step_fraction))
		r->backwards++;
	if (!t->finished && (p.percent != (int)(p.overall * 100.0) ||
			     p.percent > 99) && !(p.percent == 99 &&
						  p.overall >= 0.99))
		r->pct_bad++;
	if (!t->finished && p.percent >= 100)
		r->pct_100_early++;
	if (judge && p.eta_seconds >= 0 && true_left > 0.0) {
		double ratio = (double)p.eta_seconds / true_left;

		if (true_left >= 120.0 && p.eta_seconds < 60)
			r->short_while_long++;
		if (true_left > 30.0) {
			if (ratio < r->worst_ratio_lo)
				r->worst_ratio_lo = ratio;
			if (ratio > r->worst_ratio_hi)
				r->worst_ratio_hi = ratio;
			if (ratio < 0.7 || ratio > 1.45)
				r->eta_far++;
		}
	}
	if (judge && p.eta_seconds < 0)
		r->eta_lazy++;
	if (judge && last->eta_seconds >= 0 && p.eta_seconds >= 0) {
		/* one second later: down by about one, not a leap */
		int d = p.eta_seconds - last->eta_seconds;
		int room = 4 + last->eta_seconds / 20;

		if (d > room || d < -1 - room)
			r->eta_jumpy++;
	}
	*last = p;
}

/* judge_after: seconds of the unpacking after which the time left must be
 * right (the warm-up of the fit and, for a wrong default, the first
 * seconds of the small files). */
static void pm_play(struct pm_run *r, double judge_after)
{
	struct il_track t;
	struct il_progress last;
	uint64_t t0 = 1000000, now = t0, bytes, files;
	double pre[IL_STEP_EXTRACT] = { 1.0, 2.0, 6.0, 1.0 };
	double big_end = r->cum[PM_BIG - 1];

	memset(&last, 0, sizeof(last));
	last.eta_seconds = -1;
	r->worst_ratio_lo = 1e9;
	r->worst_ratio_hi = 0.0;
	il_track_init(&t, now);
	il_track_totals(&t, PM_BYTES, PM_FILES, PM_BYTES / 3);
	for (int s = 0; s < IL_STEP_EXTRACT; s++) {
		il_track_step(&t, s, now);
		for (int k = 1; k <= 10; k++) {
			now += (uint64_t)(pre[s] * 100.0);
			il_track_fraction(&t, k / 10.0);
			pm_check(r, &t, now, 0, 0, &last);
			if (last.eta_seconds >= 0)
				r->eta_early++;
		}
	}
	il_track_step(&t, IL_STEP_EXTRACT, now);
	for (double x = 0.0; ; x += 0.1) {
		uint64_t at = now + (uint64_t)(x * 1000.0 + 0.5);
		int judge = x >= judge_after;

		if (x > r->x_secs)
			x = r->x_secs;
		pm_at(r, x, &bytes, &files);
		il_track_extract(&t, at, bytes, files, bytes / 3);
		/* the report every 100 ms; judged once a second */
		if ((int)(x * 10.0 + 0.5) % 10 == 0 || x >= r->x_secs) {
			pm_check(r, &t, at, pm_true_left(r, x), judge, &last);
			if (x < IL_TRACK_WARMUP_MS / 1000.0 - 0.05 &&
			    last.eta_seconds >= 0)
				r->eta_early++;
		} else {
			struct il_progress p;

			il_track_report(&t, at, &p);
		}
		if (x <= big_end && x + 0.1 > big_end)
			r->frac_end_of_big = last.step_fraction;
		if (x >= r->x_secs) {
			now = at;
			break;
		}
	}
	/* configure, boot loader, finalize (the sync in one go) */
	for (int s = 0; s < 3; s++) {
		double d = r->post[s], left_after = 0.0;

		for (int k = s + 1; k < 3; k++)
			left_after += r->post[k];
		il_track_step(&t, IL_STEP_EXTRACT + 1 + s, now);
		for (double x = 0.0; x < d; x += 1.0) {
			double f = s == 2 ? (x < 0.05 * d ? 0.1 : 0.3) : x / d;

			il_track_fraction(&t, f);
			pm_check(r, &t, now + (uint64_t)(x * 1000.0),
				 d - x + left_after, 1, &last);
		}
		now += (uint64_t)(d * 1000.0);
		il_track_fraction(&t, 1.0);
		pm_check(r, &t, now, left_after, 0, &last);
	}
	il_track_finish(&t, now);
	pm_check(r, &t, now, 0.0, 0, &last);
	r->final_pct = last.percent;
}

static void test_progress_model(void)
{
	static struct pm_run r;
	struct il_track t;
	struct il_progress p;

	/* 1. A slow disk, as measured on LikeOS (7 MB/s and 30 ms a file:
	 *    about 220 KiB of bytes per file, near the default). */
	memset(&r, 0, sizeof(r));
	r.a = 0.14;
	r.b = 0.030;
	pm_entries(&r);
	r.post[0] = r.x_secs * 0.06 / 0.75;
	r.post[1] = r.x_secs * 0.03 / 0.75;
	r.post[2] = r.x_secs * 0.07 / 0.75;
	pm_play(&r, IL_TRACK_WARMUP_MS / 1000.0 + 5.0);
	CHECK(!r.backwards, "model: progress never goes back (%d)", r.backwards);
	CHECK(!r.pct_bad && !r.pct_100_early, "model: percent is floor(overall) "
	      "and below 100 until the end (%d, %d)", r.pct_bad,
	      r.pct_100_early);
	CHECK(r.final_pct == 100, "model: 100 %% at the end (%d)", r.final_pct);
	CHECK(!r.eta_early, "model: no time left before the fit (%d)",
	      r.eta_early);
	CHECK(r.eta_lazy == 0, "model: a time left once warmed up (%d)",
	      r.eta_lazy);
	CHECK(r.frac_end_of_big < 0.4, "model: after the big files (85 %% of "
	      "the bytes) the share follows the work: %.3f (time share %.3f)",
	      r.frac_end_of_big, r.cum[PM_BIG - 1] / r.x_secs);
	CHECK(!r.short_while_long, "model: never under a minute while two or "
	      "more are left (%d)", r.short_while_long);
	CHECK(!r.eta_far, "model: time left within -30 %%/+45 %% of the truth "
	      "(%d off; worst %.2f .. %.2f)", r.eta_far, r.worst_ratio_lo,
	      r.worst_ratio_hi);
	CHECK(!r.eta_jumpy, "model: time left runs down smoothly (%d jumps)",
	      r.eta_jumpy);
	printf("model 1: unpacking %.0f s, share after the big files %.3f, "
	       "time left %.2f .. %.2f of the truth\n", r.x_secs,
	       r.frac_end_of_big, r.worst_ratio_lo, r.worst_ratio_hi);

	/* 2. Files far dearer than the default (a fast disk, 50 MB/s, and
	 *    30 ms a file: 1.5 MiB of bytes per file).  While only big files
	 *    have been seen nobody can know; from 20 s of small files on the
	 *    time left must be right, and it must never have said "less than
	 *    a minute" with two minutes left. */
	memset(&r, 0, sizeof(r));
	r.a = 0.02;
	r.b = 0.030;
	pm_entries(&r);
	r.post[0] = 8.0;
	r.post[1] = 3.0;
	r.post[2] = 30.0;
	pm_play(&r, r.cum[PM_BIG - 1] + 20.0);
	CHECK(!r.backwards && !r.pct_bad && !r.pct_100_early &&
	      r.final_pct == 100, "model 2: percent consistent (%d %d %d %d)",
	      r.backwards, r.pct_bad, r.pct_100_early, r.final_pct);
	CHECK(!r.short_while_long, "model 2: never under a minute while two "
	      "or more are left (%d)", r.short_while_long);
	CHECK(r.worst_ratio_lo >= 0.7 && r.worst_ratio_lo < 1e8,
	      "model 2: time left not too short once small files were seen "
	      "(worst %.2f)", r.worst_ratio_lo);
	CHECK(!r.eta_jumpy, "model 2: smooth (%d jumps)", r.eta_jumpy);
	printf("model 2: unpacking %.0f s, time left %.2f .. %.2f of the "
	       "truth after %.0f s\n", r.x_secs, r.worst_ratio_lo,
	       r.worst_ratio_hi, r.cum[PM_BIG - 1] + 20.0);

	/* 3. An image without files= / bytes=: the archive bytes count. */
	il_track_init(&t, 1);
	il_track_totals(&t, 0, 0, 1000);
	il_track_step(&t, IL_STEP_EXTRACT, 1);
	il_track_extract(&t, 2, 0, 0, 500);
	il_track_report(&t, 2, &p);
	CHECK(p.step_fraction > 0.49 && p.step_fraction < 0.51,
	      "model: compressed share without totals (%.3f)", p.step_fraction);
	CHECK(p.eta_seconds == -1, "model: no time left in the warm-up");
	il_track_extract(&t, 10002, 0, 0, 750);
	il_track_report(&t, 10002, &p);
	CHECK(p.eta_seconds > 0 && p.percent < 100 && p.step_fraction > 0.7,
	      "model: without totals the time left follows the archive "
	      "(%d s, %.3f)", p.eta_seconds, p.step_fraction);
}

int main(int argc, char **argv)
{
	const char *resetc;

	if (argc < 3) {
		fprintf(stderr, "usage: test_backend WORKDIR RES_ETC\n");
		return 2;
	}
	g_work = argv[1];
	resetc = argv[2];
	test_sizes();
	test_validators();
	test_timezones();
	test_sha256();
	test_keyboard_conf();
	test_answers();
	test_answers_nomem();
	test_probe();
	test_plan_blank();
	test_plan_existing();
	test_plan_mbr();
	test_plan_root_choice();
	test_plan_guards();
	test_config_accounts(resetc);
	test_boot_medium_recheck();
	test_disk_identity();
	test_disk_identity_sysfs();
	test_esp_fat32_min();
	test_format_esp_sector_size();
	test_accounts_group_gid(resetc);
	test_progress_model();
	printf("test_backend: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
