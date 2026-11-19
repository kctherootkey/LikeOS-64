/* likeos-efiboot -- show and edit the firmware's boot entries.
 *
 *   likeos-efiboot [--list] [-v]
 *   likeos-efiboot --create --disk DEV --part N --loader PATH --label TEXT
 *                  [--first] [--options TEXT]
 *   likeos-efiboot --delete NUM
 *   likeos-efiboot --active NUM | --inactive NUM
 *   likeos-efiboot --bootorder NUM,NUM,... [--force]
 *   likeos-efiboot --bootnext NUM | --delete-bootnext
 *   likeos-efiboot --timeout SECONDS | --delete-timeout
 *
 * The firmware keeps its boot manager's settings in UEFI variables of the
 * EFI global variable GUID: Boot#### (one load option per entry: active
 * flag, label, device path of the loader, optional data), BootOrder (the
 * entries in the order they are tried), BootNext (one entry to try on the
 * next start only), BootCurrent (the entry this start came from) and
 * Timeout (seconds the firmware's own menu waits).  The kernel shows every
 * variable as a file, /sys/firmware/efi/efivars/<Name>-<GUID>: four bytes
 * of attributes (little-endian) followed by the value.  Writing a file in
 * one write() sets the variable, unlink() deletes it.
 *
 * --create makes the entry a freshly installed system needs: the device
 * path HD(<n>,GPT,<partition GUID>,<start>,<size>)/File(<loader>), taken
 * from the partition table of DEV, under the given label.  An entry that
 * already starts the same loader from the same partition is reused rather
 * than duplicated.  Several actions may be given at once; they run in the
 * order create, delete, active/inactive, bootorder, bootnext, timeout.
 *
 * NUM is a hexadecimal entry number (3, 0003, Boot0003).  Exit status: 0
 * success, 1 failure, 2 usage or no firmware variables on this system.
 *
 * For tests on the build host, LIKEOS_EFIVARS_DIR names a directory used
 * in place of /sys/firmware/efi/efivars.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gpt.h"
#include "../../../../boot/efi_loadopt.h"

#define PROG "likeos-efiboot"
#define VERSION "1.0"

#define EFIVARS_DIR "/sys/firmware/efi/efivars"
#define GLOBAL_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"

/* EFI variable attributes */
#define VAR_NON_VOLATILE      0x00000001u
#define VAR_BOOTSERVICE       0x00000002u
#define VAR_RUNTIME           0x00000004u
#define VAR_NV_BS_RT          (VAR_NON_VOLATILE | VAR_BOOTSERVICE | VAR_RUNTIME)

#define MAX_ENTRIES 4096   /* BootOrder entries considered */
#define MAX_BOOT_VARS 0x10000 /* Boot0000 .. BootFFFF */
#define MAX_LIST    64     /* numbers given to one option */
#define VAR_MAX     (64 * 1024)

/* ---- options ---------------------------------------------------------- */

struct opts {
	int list;
	int verbose;
	int quiet;
	int force;
	/* create */
	int create;
	const char *disk;
	const char *part;
	const char *loader;
	const char *label;
	const char *options;
	int first;
	/* the rest */
	int ndelete;
	unsigned delete_nums[MAX_LIST];
	int nactive;
	unsigned active_nums[MAX_LIST];
	int ninactive;
	unsigned inactive_nums[MAX_LIST];
	const char *bootorder;
	const char *bootnext;
	int delete_bootnext;
	const char *timeout;
	int delete_timeout;
};

static const char *g_dir = EFIVARS_DIR;
static int g_quiet;

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: " PROG " [options]\n"
		"\n"
		"Show the firmware's boot entries, or change them.\n"
		"\n"
		"Actions:\n"
		"      --list                show the boot entries (the default)\n"
		"  -c, --create              create a boot entry (or reuse an\n"
		"                            identical one); needs --disk, --part,\n"
		"                            --loader and --label\n"
		"  -B, --delete NUM          delete entry NUM\n"
		"  -a, --active NUM          mark entry NUM active\n"
		"  -A, --inactive NUM        mark entry NUM inactive\n"
		"  -o, --bootorder LIST      set the boot order (NUM,NUM,...)\n"
		"  -n, --bootnext NUM        start entry NUM on the next boot only\n"
		"  -N, --delete-bootnext     cancel BootNext\n"
		"  -t, --timeout SECONDS     the firmware menu's timeout\n"
		"  -T, --delete-timeout      remove the timeout setting\n"
		"\n"
		"Create options:\n"
		"  -d, --disk DEV            the disk holding the EFI system\n"
		"                            partition (/dev/sda, /dev/nvme0n1)\n"
		"  -p, --part N              its partition number\n"
		"  -l, --loader PATH         the loader on it (\\EFI\\LIKEOS\\BOOTX64.EFI)\n"
		"  -L, --label TEXT          the name the firmware shows\n"
		"  -f, --first               put the entry first in the boot order\n"
		"  -u, --options TEXT        options passed to the loader\n"
		"\n"
		"Options:\n"
		"      --force               accept entry numbers that do not exist\n"
		"  -q, --quiet               report errors only\n"
		"  -v, --verbose             show attributes and optional data too\n"
		"  -h, --help                this text\n"
		"      --version             the version\n"
		"\n"
		"NUM is a hexadecimal entry number: 3, 0003 or Boot0003.\n");
}

static void die_usage(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void die_usage(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, PROG ": ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\nTry '" PROG " --help'.\n");
	exit(2);
}

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int fail(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, PROG ": ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	return 1;
}

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void info(const char *fmt, ...)
{
	va_list ap;

	if (g_quiet)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

/* Why a variable could not be read or written, for a person. */
static const char *why(int err)
{
	if (err == EACCES || err == EPERM)
		return "permission denied (must be run as root)";
	if (err == ENOSPC)
		return "no space left in the firmware's variable store";
	if (err == EIO)
		return "the firmware reported an error";
	if (err == EINVAL)
		return "the firmware refused the value";
	return strerror(err);
}

/* "3", "0003", "Boot0003", "0x3" -> 0x0003; -1 when not a number. */
static long parse_num(const char *s)
{
	char *end;
	unsigned long v;

	if (!s)
		return -1;
	if (strncasecmp(s, "Boot", 4) == 0)
		s += 4;
	if (!*s || *s == '-' || *s == '+')
		return -1;
	errno = 0;
	v = strtoul(s, &end, 16);
	if (errno || *end || v > 0xffff)
		return -1;
	return (long)v;
}

static unsigned num_arg(const char *opt, const char *s)
{
	long v = parse_num(s);

	if (v < 0)
		die_usage("%s: '%s' is not an entry number (hexadecimal, 0-FFFF)",
			  opt, s ? s : "");
	return (unsigned)v;
}

/* --opt=value or --opt value */
static const char *opt_value(int argc, char **argv, int *i, const char *eq)
{
	if (eq)
		return eq + 1;
	if (*i + 1 >= argc)
		die_usage("%s needs a value", argv[*i]);
	return argv[++*i];
}

static void parse_args(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	for (int i = 1; i < argc; i++) {
		char name[64];
		const char *a = argv[i], *eq = NULL;

		/* Split "--name=value". */
		if (a[0] == '-' && a[1] == '-' && (eq = strchr(a, '=')) != NULL) {
			size_t l = (size_t)(eq - a);

			if (l >= sizeof(name))
				die_usage("unknown option '%s'", a);
			memcpy(name, a, l);
			name[l] = 0;
			a = name;
		}
#define IS(s, l) (strcmp(a, s) == 0 || strcmp(a, l) == 0)
#define VAL() opt_value(argc, argv, &i, eq)
		if (strcmp(a, "--list") == 0) {
			o->list = 1;
		} else if (IS("-c", "--create")) {
			o->create = 1;
		} else if (IS("-d", "--disk")) {
			o->disk = VAL();
		} else if (IS("-p", "--part")) {
			o->part = VAL();
		} else if (IS("-l", "--loader")) {
			o->loader = VAL();
		} else if (IS("-L", "--label")) {
			o->label = VAL();
		} else if (IS("-u", "--options")) {
			o->options = VAL();
		} else if (IS("-f", "--first")) {
			o->first = 1;
		} else if (IS("-B", "--delete")) {
			if (o->ndelete == MAX_LIST)
				die_usage("too many --delete options");
			o->delete_nums[o->ndelete++] = num_arg(a, VAL());
		} else if (IS("-a", "--active")) {
			if (o->nactive == MAX_LIST)
				die_usage("too many --active options");
			o->active_nums[o->nactive++] = num_arg(a, VAL());
		} else if (IS("-A", "--inactive")) {
			if (o->ninactive == MAX_LIST)
				die_usage("too many --inactive options");
			o->inactive_nums[o->ninactive++] = num_arg(a, VAL());
		} else if (IS("-o", "--bootorder")) {
			o->bootorder = VAL();
		} else if (IS("-n", "--bootnext")) {
			o->bootnext = VAL();
		} else if (IS("-N", "--delete-bootnext")) {
			o->delete_bootnext = 1;
		} else if (IS("-t", "--timeout")) {
			o->timeout = VAL();
		} else if (IS("-T", "--delete-timeout")) {
			o->delete_timeout = 1;
		} else if (strcmp(a, "--force") == 0) {
			o->force = 1;
		} else if (IS("-q", "--quiet")) {
			o->quiet = 1;
		} else if (IS("-v", "--verbose")) {
			o->verbose = 1;
		} else if (IS("-h", "--help")) {
			usage(stdout);
			exit(0);
		} else if (strcmp(a, "--version") == 0) {
			printf(PROG " " VERSION "\n");
			exit(0);
		} else {
			die_usage("unknown argument '%s'", argv[i]);
		}
		if (eq && (strcmp(a, "--list") == 0 || IS("-c", "--create") ||
			   IS("-f", "--first") || IS("-N", "--delete-bootnext") ||
			   IS("-T", "--delete-timeout") || strcmp(a, "--force") == 0 ||
			   IS("-q", "--quiet") || IS("-v", "--verbose")))
			die_usage("%s takes no value", a);
#undef VAL
#undef IS
	}
	if (o->create && (!o->disk || !o->part || !o->loader || !o->label))
		die_usage("--create needs --disk, --part, --loader and --label");
	if (!o->create && (o->disk || o->part || o->loader || o->label ||
			   o->options || o->first))
		die_usage("--disk, --part, --loader, --label, --options and "
			  "--first belong to --create");
	if (o->bootnext && o->delete_bootnext)
		die_usage("--bootnext and --delete-bootnext exclude each other");
	if (o->timeout && o->delete_timeout)
		die_usage("--timeout and --delete-timeout exclude each other");
	for (int a = 0; a < o->nactive; a++)
		for (int b = 0; b < o->ninactive; b++)
			if (o->active_nums[a] == o->inactive_nums[b])
				die_usage("Boot%04X cannot be both active and "
					  "inactive", o->active_nums[a]);
}

/* ---- variables ----------------------------------------------------------- */

struct var {
	uint32_t attr;
	uint8_t *data;
	size_t len;
};

static void var_free(struct var *v)
{
	free(v->data);
	v->data = NULL;
	v->len = 0;
}

static void var_path(char *buf, size_t len, const char *name)
{
	snprintf(buf, len, "%s/%s-%s", g_dir, name, GLOBAL_GUID);
}

/* 0, or -errno (-ENOENT: there is no such variable). */
static int var_read(const char *name, struct var *v)
{
	char path[512];
	uint8_t *buf = NULL;
	size_t cap = 0, n = 0;
	int fd, err = 0;

	memset(v, 0, sizeof(*v));
	var_path(path, sizeof(path), name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	for (;;) {
		ssize_t r;

		if (n == cap) {
			uint8_t *nb;

			if (cap >= VAR_MAX + 4) {
				err = EFBIG;
				break;
			}
			cap = cap ? cap * 2 : 1024;
			nb = realloc(buf, cap);
			if (!nb) {
				err = ENOMEM;
				break;
			}
			buf = nb;
		}
		r = read(fd, buf + n, cap - n);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			err = errno;
			break;
		}
		if (r == 0)
			break;
		n += (size_t)r;
	}
	close(fd);
	if (!err && n < 4)
		err = n == 0 ? ENOENT : EIO; /* gone meanwhile, or not a variable */
	if (err) {
		free(buf);
		return -err;
	}
	v->attr = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 |
		  (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
	v->len = n - 4;
	memmove(buf, buf + 4, v->len);
	v->data = buf;
	return 0;
}

/* One write() of attributes and value: 0 or -errno. */
static int var_write(const char *name, uint32_t attr, const void *data,
		     size_t len)
{
	char path[512];
	uint8_t *buf = malloc(len + 4);
	ssize_t w;
	int fd, err = 0;

	if (!buf)
		return -ENOMEM;
	buf[0] = (uint8_t)attr;
	buf[1] = (uint8_t)(attr >> 8);
	buf[2] = (uint8_t)(attr >> 16);
	buf[3] = (uint8_t)(attr >> 24);
	if (len)
		memcpy(buf + 4, data, len);
	var_path(path, sizeof(path), name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		err = errno;
		free(buf);
		return -err;
	}
	do {
		w = write(fd, buf, len + 4);
	} while (w < 0 && errno == EINTR);
	if (w < 0)
		err = errno;
	else if ((size_t)w != len + 4)
		err = EIO;
	if (close(fd) < 0 && !err)
		err = errno;
	free(buf);
	return -err;
}

static int var_delete(const char *name)
{
	char path[512];

	var_path(path, sizeof(path), name);
	return unlink(path) < 0 ? -errno : 0;
}

static void boot_name(char *buf, size_t len, unsigned num)
{
	snprintf(buf, len, "Boot%04X", num & 0xffff);
}

/* A 16-bit variable (BootCurrent, BootNext, Timeout): 0, -ENOENT, or
 * -EINVAL when it has the wrong size. */
static int var_read_u16(const char *name, unsigned *out)
{
	struct var v;
	int r = var_read(name, &v);

	if (r)
		return r;
	if (v.len != 2) {
		var_free(&v);
		return -EINVAL;
	}
	*out = v.data[0] | v.data[1] << 8;
	var_free(&v);
	return 0;
}

static int var_write_u16(const char *name, unsigned val)
{
	uint8_t b[2] = { (uint8_t)val, (uint8_t)(val >> 8) };

	return var_write(name, VAR_NV_BS_RT, b, 2);
}

/* ---- BootOrder ---------------------------------------------------------- */

struct order {
	uint32_t attr;      /* the variable's, VAR_NV_BS_RT for a new one */
	int exists;
	size_t n;
	uint16_t num[MAX_ENTRIES];
};

static int order_read(struct order *o)
{
	struct var v;
	int r;

	o->attr = VAR_NV_BS_RT;
	o->exists = 0;
	o->n = 0;
	r = var_read("BootOrder", &v);
	if (r == -ENOENT)
		return 0;
	if (r)
		return fail("BootOrder cannot be read: %s", why(-r));
	o->exists = 1;
	o->attr = v.attr;
	for (size_t i = 0; i + 1 < v.len && o->n < MAX_ENTRIES; i += 2)
		o->num[o->n++] = (uint16_t)(v.data[i] | v.data[i + 1] << 8);
	if (v.len & 1)
		fprintf(stderr, PROG ": warning: BootOrder has an odd length; "
			"its last byte is ignored\n");
	var_free(&v);
	return 0;
}

static int order_write(const struct order *o)
{
	uint8_t buf[2 * MAX_ENTRIES];
	int r;

	for (size_t i = 0; i < o->n; i++) {
		buf[2 * i] = (uint8_t)o->num[i];
		buf[2 * i + 1] = (uint8_t)(o->num[i] >> 8);
	}
	r = var_write("BootOrder", o->attr ? o->attr : VAR_NV_BS_RT, buf,
		      2 * o->n);
	if (r)
		return fail("BootOrder cannot be written: %s", why(-r));
	return 0;
}

static int order_has(const struct order *o, unsigned num)
{
	for (size_t i = 0; i < o->n; i++)
		if (o->num[i] == num)
			return 1;
	return 0;
}

/* Remove every occurrence of num; 1 when there was one. */
static int order_remove(struct order *o, unsigned num)
{
	size_t j = 0;
	int found = 0;

	for (size_t i = 0; i < o->n; i++) {
		if (o->num[i] == num)
			found = 1;
		else
			o->num[j++] = o->num[i];
	}
	o->n = j;
	return found;
}

static void order_text(const struct order *o, char *buf, size_t len)
{
	size_t p = 0;

	buf[0] = 0;
	for (size_t i = 0; i < o->n && p + 6 < len; i++)
		p += (size_t)snprintf(buf + p, len - p, "%s%04X", i ? "," : "",
				      o->num[i]);
}

/* ---- the Boot#### variables ------------------------------------------- */

static int cmp_u16(const void *a, const void *b)
{
	return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* Sort out[0..n) and drop repeated numbers (the same Boot#### with the
 * GUID spelt in another case); the count left. */
static size_t unique_u16(uint16_t *out, size_t n)
{
	size_t j = 0;

	qsort(out, n, sizeof(out[0]), cmp_u16);
	for (size_t i = 0; i < n; i++)
		if (j == 0 || out[i] != out[j - 1])
			out[j++] = out[i];
	return j;
}

/* The numbers of all Boot#### variables, ascending, each once.  out holds
 * max numbers (MAX_BOOT_VARS covers every possible one); -1 on error,
 * never a shortened list. */
static int boot_numbers(uint16_t *out, size_t max, size_t *count)
{
	DIR *d = opendir(g_dir);
	struct dirent *e;
	size_t n = 0;

	*count = 0;
	if (!d)
		return fail("%s cannot be listed: %s", g_dir, why(errno));
	while ((e = readdir(d)) != NULL) {
		const char *s = e->d_name;
		unsigned v = 0;
		int ok = strncmp(s, "Boot", 4) == 0;

		for (int i = 4; ok && i < 8; i++) {
			char c = s[i];

			if (c >= '0' && c <= '9')
				v = v * 16 + (unsigned)(c - '0');
			else if (c >= 'A' && c <= 'F')
				v = v * 16 + (unsigned)(c - 'A' + 10);
			else
				ok = 0;
		}
		if (!ok || s[8] != '-' || strcasecmp(s + 9, GLOBAL_GUID) != 0)
			continue;
		if (n == max) {
			n = unique_u16(out, n);
			if (n == max) {
				closedir(d);
				return fail("%s lists more than %zu Boot#### "
					    "variables", g_dir, max);
			}
		}
		out[n++] = (uint16_t)v;
	}
	closedir(d);
	*count = unique_u16(out, n);
	return 0;
}

static int boot_exists(unsigned num)
{
	char name[16], path[512];
	struct stat st;

	boot_name(name, sizeof(name), num);
	var_path(path, sizeof(path), name);
	return stat(path, &st) == 0;
}

/* A Boot#### read and parsed: 0, -ENOENT, or another -errno (-EBADMSG for a
 * malformed load option, with v still filled). */
static int boot_read(unsigned num, struct var *v, struct elo_load_option *lo)
{
	char name[16];
	int r;

	boot_name(name, sizeof(name), num);
	r = var_read(name, v);
	if (r)
		return r;
	if (elo_load_option_parse(v->data, v->len, lo) != 0)
		return -EBADMSG;
	return 0;
}

/* ---- --list --------------------------------------------------------------- */

/* Optional data shown as text when it is NUL-terminated printable UCS-2,
 * as hex otherwise. */
static void show_optional(const uint8_t *p, size_t n)
{
	int text = n >= 2 && !(n & 1);

	for (size_t i = 0; text && i < n; i += 2) {
		unsigned c = p[i] | p[i + 1] << 8;

		if (c == 0)
			text = i + 2 == n;
		else if (c < 0x20 || (c >= 0xd800 && c <= 0xdfff))
			text = 0;
	}
	if (text) {
		char s[1024];

		elo_ucs2_to_utf8(p, n / 2, s, sizeof(s));
		printf("    options: \"%s\"\n", s);
		return;
	}
	printf("    optional data (%zu bytes):", n);
	for (size_t i = 0; i < n; i++)
		printf("%s%02x", i % 16 ? " " : "\n      ", p[i]);
	putchar('\n');
}

static void show_entry(unsigned num, int verbose)
{
	struct var v;
	struct elo_load_option lo;
	char desc[512], path[2048];
	int r = boot_read(num, &v, &lo);

	if (r == -EBADMSG) {
		printf("Boot%04X  (malformed load option, %zu bytes)\n", num, v.len);
		var_free(&v);
		return;
	}
	if (r) {
		printf("Boot%04X  (%s)\n", num, r == -ENOENT ? "missing" : why(-r));
		return;
	}
	elo_ucs2_to_utf8(lo.desc, lo.desc_units, desc, sizeof(desc));
	elo_dp_to_text(lo.dp, lo.dp_len, path, sizeof(path));
	printf("Boot%04X%c %s\t%s\n", num,
	       lo.attributes & ELO_LOAD_OPTION_ACTIVE ? '*' : ' ', desc, path);
	if (verbose) {
		printf("    attributes: 0x%08x%s%s%s\n", lo.attributes,
		       lo.attributes & ELO_LOAD_OPTION_ACTIVE ? " active" : "",
		       lo.attributes & ELO_LOAD_OPTION_HIDDEN ? " hidden" : "",
		       (lo.attributes & ELO_LOAD_OPTION_CATEGORY) ==
				       ELO_LOAD_OPTION_CATEGORY_APP ?
			       " application" : "");
		if (lo.opt_len)
			show_optional(lo.opt, lo.opt_len);
	}
	var_free(&v);
}

static int cmd_list(int verbose)
{
	static uint16_t nums[MAX_BOOT_VARS];
	static struct order order;
	size_t n;
	unsigned u;
	char text[8 * 64];
	int r;

	r = var_read_u16("BootCurrent", &u);
	if (r == 0)
		printf("BootCurrent: %04X\n", u);
	r = var_read_u16("BootNext", &u);
	if (r == 0)
		printf("BootNext: %04X\n", u);
	r = var_read_u16("Timeout", &u);
	if (r == 0)
		printf("Timeout: %u second%s\n", u, u == 1 ? "" : "s");
	if (order_read(&order))
		return 1;
	order_text(&order, text, sizeof(text));
	if (order.exists)
		printf("BootOrder: %s\n", text);
	if (boot_numbers(nums, MAX_BOOT_VARS, &n))
		return 1;
	/* The ones in BootOrder first, in that order, then the rest. */
	for (size_t i = 0; i < order.n; i++) {
		int dup = 0;

		for (size_t j = 0; j < i; j++)
			if (order.num[j] == order.num[i])
				dup = 1;
		if (!dup)
			show_entry(order.num[i], verbose);
	}
	for (size_t i = 0; i < n; i++)
		if (!order_has(&order, nums[i]))
			show_entry(nums[i], verbose);
	return 0;
}

/* ---- --create ------------------------------------------------------------- */

/* The Hard Drive node of partition `partno' of `disk'. */
static int disk_partition(const char *disk, int partno, struct elo_hd *hd)
{
	struct gpt_dev dev;
	struct gpt_table t;
	int r;

	memset(hd, 0, sizeof(*hd));
	r = gpt_dev_open(&dev, disk, GPT_OPEN_RDONLY, 0);
	if (r)
		return fail("%s cannot be opened: %s", disk, strerror(-r));
	r = gpt_read(&dev, &t);
	if (r == 0) {
		const struct gpt_part *p = gpt_part_get(&t, partno);

		if (!p) {
			gpt_table_free(&t);
			gpt_dev_close(&dev);
			return fail("%s has no partition %d", disk, partno);
		}
		hd->part_number = (uint32_t)partno;
		hd->part_start = p->first_lba;
		hd->part_size = p->last_lba - p->first_lba + 1;
		gpt_guid_to_disk(&p->uuid, hd->signature);
		hd->mbr_type = ELO_HD_MBR_TYPE_GPT;
		hd->sig_type = ELO_HD_SIG_GUID;
		gpt_table_free(&t);
		gpt_dev_close(&dev);
		return 0;
	}
	if (r != -ENOENT) {
		gpt_dev_close(&dev);
		return fail("the partition table of %s cannot be read: %s", disk,
			    r == -EBADMSG ? "both GPT copies are damaged" :
					    strerror(-r));
	}
	/* No GPT: an MBR disk. */
	{
		static struct mbr_table m;

		r = mbr_read(&dev, &m);
		gpt_dev_close(&dev);
		if (r)
			return fail("the partition table of %s cannot be read: %s",
				    disk, strerror(-r));
		if (m.kind != MBR_KIND_LEGACY && m.kind != MBR_KIND_HYBRID)
			return fail("%s has no partition table", disk);
		for (int i = 0; i < m.nparts; i++) {
			if (m.parts[i].number != partno || m.parts[i].is_extended)
				continue;
			hd->part_number = (uint32_t)partno;
			hd->part_start = m.parts[i].first_lba;
			hd->part_size = m.parts[i].nsectors;
			hd->signature[0] = (uint8_t)m.disk_signature;
			hd->signature[1] = (uint8_t)(m.disk_signature >> 8);
			hd->signature[2] = (uint8_t)(m.disk_signature >> 16);
			hd->signature[3] = (uint8_t)(m.disk_signature >> 24);
			hd->mbr_type = ELO_HD_MBR_TYPE_MBR;
			hd->sig_type = ELO_HD_SIG_MBR;
			return 0;
		}
		return fail("%s has no partition %d", disk, partno);
	}
}

/* Is the existing entry what --create would write (same label, active,
 * same options)?  The device path matched already. */
static int entry_same(const struct elo_load_option *lo, const char *label,
		      const uint8_t *opt, size_t optn)
{
	char desc[512];

	elo_ucs2_to_utf8(lo->desc, lo->desc_units, desc, sizeof(desc));
	if (strcmp(desc, label) != 0)
		return 0;
	if (lo->attributes != ELO_LOAD_OPTION_ACTIVE)
		return 0;
	if (lo->opt_len != optn || (optn && memcmp(lo->opt, opt, optn) != 0))
		return 0;
	return 1;
}

static int cmd_create(const struct opts *o)
{
	static uint16_t nums[MAX_BOOT_VARS];
	static struct order order;
	struct elo_hd hd;
	uint8_t dp[4096], opt[4096], lo[8192];
	size_t dpn, optn = 0, lon, n;
	long partno;
	char *end, name[16], text[2048];
	long found = -1;
	unsigned num = 0;
	int r, changed = 0;

	errno = 0;
	partno = strtol(o->part, &end, 10);
	if (errno || *end || partno < 1 || partno > 0xffff)
		return fail("--part: '%s' is not a partition number", o->part);
	if (disk_partition(o->disk, (int)partno, &hd))
		return 1;
	dpn = elo_dp_hd_file(&hd, o->loader, dp, sizeof(dp));
	if (!dpn)
		return fail("--loader: '%s' is not a usable path (UTF-8, "
			    "characters up to U+FFFF)", o->loader);
	if (o->options) {
		optn = elo_utf8_to_ucs2(o->options, opt, sizeof(opt));
		if (!optn)
			return fail("--options: the text is not valid UTF-8 or "
				    "too long");
	}
	lon = elo_load_option_build(ELO_LOAD_OPTION_ACTIVE, o->label, dp, dpn,
				    optn ? opt : NULL, optn, lo, sizeof(lo));
	if (!lon)
		return fail("--label: '%s' is not usable (UTF-8, characters up to "
			    "U+FFFF)", o->label);

	/* An entry that already starts this loader from this partition? */
	if (boot_numbers(nums, MAX_BOOT_VARS, &n))
		return 1;
	for (size_t i = 0; i < n && found < 0; i++) {
		struct var v;
		struct elo_load_option e;

		if (boot_read(nums[i], &v, &e) != 0) {
			var_free(&v);
			continue;
		}
		if (elo_dp_matches(e.dp, e.dp_len, &hd, o->loader)) {
			found = nums[i];
			if (entry_same(&e, o->label, opt, optn)) {
				info("Boot%04X already starts %s; kept as it is",
				     nums[i], o->loader);
			} else {
				changed = 1;
			}
		}
		var_free(&v);
	}
	if (order_read(&order))
		return 1;
	if (found >= 0) {
		num = (unsigned)found;
	} else {
		/* The lowest number neither in use nor named by BootOrder. */
		for (num = 0; num <= 0xffff; num++) {
			int used = order_has(&order, num);

			for (size_t i = 0; i < n && !used; i++)
				used = nums[i] == num;
			if (!used)
				break;
		}
		if (num > 0xffff)
			return fail("every boot entry number is in use");
		changed = 1;
	}
	boot_name(name, sizeof(name), num);
	if (changed) {
		r = var_write(name, VAR_NV_BS_RT, lo, lon);
		if (r)
			return fail("%s cannot be written: %s", name, why(-r));
		elo_dp_to_text(dp, dpn, text, sizeof(text));
		info("%s %s: \"%s\" %s", found >= 0 ? "updated" : "created", name,
		     o->label, text);
	}

	/* BootOrder */
	if (o->first) {
		int dups = 0;

		for (size_t i = 1; i < order.n; i++)
			if (order.num[i] == num)
				dups = 1;
		if (!(order.n && order.num[0] == num) || dups) {
			order_remove(&order, num);
			if (order.n >= MAX_ENTRIES)
				order.n = MAX_ENTRIES - 1;
			memmove(order.num + 1, order.num,
				order.n * sizeof(order.num[0]));
			order.num[0] = (uint16_t)num;
			order.n++;
			if (order_write(&order))
				return 1;
			info("%s put first in BootOrder", name);
		} else {
			info("%s is first in BootOrder already", name);
		}
	} else if (!order_has(&order, num)) {
		if (order.n >= MAX_ENTRIES)
			return fail("BootOrder is full");
		order.num[order.n++] = (uint16_t)num;
		if (order_write(&order))
			return 1;
		info("%s added to the end of BootOrder", name);
	}
	return 0;
}

/* ---- the smaller actions --------------------------------------------- */

static int cmd_delete(const struct opts *o, unsigned num)
{
	static struct order order;
	char name[16];
	unsigned next;
	int r;

	boot_name(name, sizeof(name), num);
	r = var_delete(name);
	if (r == -ENOENT)
		return fail("%s does not exist", name);
	if (r)
		return fail("%s cannot be deleted: %s", name, why(-r));
	info("deleted %s", name);
	if (order_read(&order))
		return 1;
	if (order_remove(&order, num)) {
		if (order_write(&order))
			return 1;
		info("%s removed from BootOrder", name);
	}
	if (var_read_u16("BootNext", &next) == 0 && next == num) {
		r = var_delete("BootNext");
		if (r && r != -ENOENT)
			return fail("BootNext cannot be deleted: %s", why(-r));
		info("BootNext pointed at %s; removed", name);
	}
	(void)o;
	return 0;
}

static int cmd_set_active(unsigned num, int active)
{
	struct var v;
	struct elo_load_option lo;
	char name[16];
	uint32_t a;
	int r;

	boot_name(name, sizeof(name), num);
	r = boot_read(num, &v, &lo);
	if (r == -ENOENT)
		return fail("%s does not exist", name);
	if (r == -EBADMSG) {
		var_free(&v);
		return fail("%s is not a well-formed boot entry; left alone", name);
	}
	if (r)
		return fail("%s cannot be read: %s", name, why(-r));
	a = lo.attributes;
	if (!!(a & ELO_LOAD_OPTION_ACTIVE) == !!active) {
		info("%s is already %s", name, active ? "active" : "inactive");
		var_free(&v);
		return 0;
	}
	a = active ? a | ELO_LOAD_OPTION_ACTIVE : a & ~ELO_LOAD_OPTION_ACTIVE;
	v.data[0] = (uint8_t)a;
	v.data[1] = (uint8_t)(a >> 8);
	v.data[2] = (uint8_t)(a >> 16);
	v.data[3] = (uint8_t)(a >> 24);
	r = var_write(name, v.attr, v.data, v.len);
	var_free(&v);
	if (r)
		return fail("%s cannot be written: %s", name, why(-r));
	info("%s marked %s", name, active ? "active" : "inactive");
	return 0;
}

static int cmd_bootorder(const struct opts *o)
{
	static struct order order;
	char *copy = strdup(o->bootorder), *save = NULL, *tok;
	char text[8 * 64];

	if (!copy)
		return fail("out of memory");
	if (order_read(&order)) {
		free(copy);
		return 1;
	}
	order.n = 0;
	for (tok = strtok_r(copy, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		long v = parse_num(tok);

		if (v < 0) {
			free(copy);
			return fail("--bootorder: '%s' is not an entry number", tok);
		}
		if (order_has(&order, (unsigned)v)) {
			free(copy);
			return fail("--bootorder: Boot%04lX is listed twice", v);
		}
		if (!boot_exists((unsigned)v)) {
			if (!o->force) {
				free(copy);
				return fail("--bootorder: Boot%04lX does not exist "
					    "(--force to set it anyway)", v);
			}
			fprintf(stderr, PROG ": warning: Boot%04lX does not exist\n",
				v);
		}
		if (order.n >= MAX_ENTRIES) {
			free(copy);
			return fail("--bootorder: too many entries");
		}
		order.num[order.n++] = (uint16_t)v;
	}
	free(copy);
	if (order.n == 0)
		return fail("--bootorder: no entry numbers given");
	if (order_write(&order))
		return 1;
	order_text(&order, text, sizeof(text));
	info("BootOrder: %s", text);
	return 0;
}

static int cmd_bootnext(const struct opts *o)
{
	unsigned num = num_arg("--bootnext", o->bootnext);
	int r;

	if (!boot_exists(num)) {
		if (!o->force)
			return fail("Boot%04X does not exist (--force to set "
				    "BootNext anyway)", num);
		fprintf(stderr, PROG ": warning: Boot%04X does not exist\n", num);
	}
	r = var_write_u16("BootNext", num);
	if (r)
		return fail("BootNext cannot be written: %s", why(-r));
	info("BootNext: %04X", num);
	return 0;
}

static int cmd_delete_var(const char *name)
{
	int r = var_delete(name);

	if (r == -ENOENT) {
		info("there is no %s", name);
		return 0;
	}
	if (r)
		return fail("%s cannot be deleted: %s", name, why(-r));
	info("deleted %s", name);
	return 0;
}

static int cmd_timeout(const struct opts *o)
{
	char *end;
	unsigned long v;
	int r;

	errno = 0;
	v = strtoul(o->timeout, &end, 10);
	if (errno || *end || !*o->timeout || o->timeout[0] == '-' || v > 0xffff)
		return fail("--timeout: '%s' is not a number of seconds "
			    "(0-65535)", o->timeout);
	r = var_write_u16("Timeout", (unsigned)v);
	if (r)
		return fail("Timeout cannot be written: %s", why(-r));
	info("Timeout: %lu second%s", v, v == 1 ? "" : "s");
	return 0;
}

int main(int argc, char **argv)
{
	struct opts o;
	struct stat st;
	const char *env = getenv("LIKEOS_EFIVARS_DIR");
	int status = 0, action;

	parse_args(argc, argv, &o);
	g_quiet = o.quiet;
	if (env && *env)
		g_dir = env;
	if (stat(g_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, PROG ": firmware variables are not available on "
			"this system\n");
		return 2;
	}
	action = o.create || o.ndelete || o.nactive || o.ninactive ||
		 o.bootorder || o.bootnext || o.delete_bootnext || o.timeout ||
		 o.delete_timeout;

	/* Each action stops the ones after it when it fails: they were asked
	 * for together, usually on the assumption that the earlier ones
	 * worked. */
	if (o.create)
		status = cmd_create(&o);
	for (int i = 0; !status && i < o.ndelete; i++)
		status = cmd_delete(&o, o.delete_nums[i]);
	for (int i = 0; !status && i < o.nactive; i++)
		status = cmd_set_active(o.active_nums[i], 1);
	for (int i = 0; !status && i < o.ninactive; i++)
		status = cmd_set_active(o.inactive_nums[i], 0);
	if (!status && o.bootorder)
		status = cmd_bootorder(&o);
	if (!status && o.bootnext)
		status = cmd_bootnext(&o);
	if (!status && o.delete_bootnext)
		status = cmd_delete_var("BootNext");
	if (!status && o.timeout)
		status = cmd_timeout(&o);
	if (!status && o.delete_timeout)
		status = cmd_delete_var("Timeout");

	if (!action || o.list || (o.verbose && !status)) {
		if (action && !g_quiet)
			putchar('\n');
		if (cmd_list(o.verbose))
			status = 1;
	}
	return status ? 1 : 0;
}
