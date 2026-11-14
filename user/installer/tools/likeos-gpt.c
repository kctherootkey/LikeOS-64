/* likeos-gpt -- show, check and edit GUID partition tables.
 *
 *   likeos-gpt [options] print      DEVICE
 *   likeos-gpt [options] verify     DEVICE
 *   likeos-gpt [options] info       DEVICE N
 *   likeos-gpt [options] new-table  DEVICE
 *   likeos-gpt [options] add        DEVICE [-n N] [--start S] [--size S]
 *                                          [--type T] [--name NAME]
 *                                          [--uuid GUID] [--attrs HEX]
 *   likeos-gpt [options] delete     DEVICE N
 *   likeos-gpt [options] resize     DEVICE N SIZE|max
 *   likeos-gpt [options] name       DEVICE N NAME
 *   likeos-gpt [options] type       DEVICE N TYPE
 *   likeos-gpt [options] attrs      DEVICE N HEX
 *   likeos-gpt [options] repair     DEVICE
 *   likeos-gpt [options] wipe       DEVICE
 *   likeos-gpt [options] types
 *
 * Options: --json (machine-readable output of print, verify, info and
 * types), --sector-size 512|4096 (image files only), --yes (confirm a
 * command that destroys a table: new-table over an existing one, wipe),
 * --no-reread (do not ask the kernel to re-read the table), --help,
 * --version.
 *
 * DEVICE is a block device (/dev/sda, /dev/nvme0n1) or a disk image file.
 * Sizes and positions are sectors when they are plain numbers, bytes with
 * a K, M, G or T suffix (powers of 1024; KiB, MiB, GiB, TiB are accepted
 * too).  TYPE is an alias (esp, likeos-root, data, swap, ...), a four-digit
 * code (ef00, 8304, ...) or a GUID; `likeos-gpt types` lists them.
 *
 * Only the editing commands write, and only to the device named.  Exit
 * status: 0 success, 1 failure (or problems found by verify), 2 usage.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gpt.h"

#define PROG "likeos-gpt"
#define MAX_ARGS 16

struct opts {
	int json;
	int yes;
	int no_reread;
	uint32_t sector_size;
	const char *command;
	const char *device;
	int nargs;
	const char *args[MAX_ARGS];
	/* add */
	int number;
	const char *start;
	const char *size;
	const char *type;
	const char *name;
	const char *uuid;
	const char *attrs;
};

/* ---- output helpers ------------------------------------------------- */

static void die_usage(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: " PROG " [options] COMMAND DEVICE [arguments]\n"
		"\n"
		"Commands:\n"
		"  print DEVICE               show the partition table\n"
		"  verify DEVICE              check both copies of the table\n"
		"  info DEVICE N              show one partition in detail\n"
		"  new-table DEVICE           write a new, empty GPT\n"
		"  add DEVICE [add options]   create a partition\n"
		"  delete DEVICE N            delete partition N\n"
		"  resize DEVICE N SIZE|max   move the end of partition N\n"
		"  name DEVICE N NAME         set the name of partition N\n"
		"  type DEVICE N TYPE         set the type of partition N\n"
		"  attrs DEVICE N HEX         set the attribute bits of N\n"
		"  repair DEVICE              rebuild a damaged or misplaced copy\n"
		"  wipe DEVICE                erase GPT and MBR (needs --yes)\n"
		"  types                      list the known partition types\n"
		"\n"
		"Add options:\n"
		"  -n, --number N             partition number (default: first free)\n"
		"  -s, --start POS            first sector (default: first free,\n"
		"                             aligned to 1 MiB)\n"
		"  -S, --size SIZE            size (default: the rest of the region)\n"
		"  -t, --type TYPE            alias, code or GUID (default: data)\n"
		"  -c, --name NAME            partition name (UTF-8)\n"
		"  -u, --uuid GUID            unique partition GUID (default: random)\n"
		"  -A, --attrs HEX            attribute bits\n"
		"\n"
		"Options:\n"
		"  --json                     machine-readable output\n"
		"  --sector-size 512|4096     sector size of an image file\n"
		"  --yes                      confirm destroying an existing table\n"
		"  --no-reread                do not make the kernel re-read the table\n"
		"  -h, --help                 this text\n"
		"  --version                  the version\n"
		"\n"
		"POS and SIZE are sectors, or bytes with a K/M/G/T suffix.\n");
}

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

/* A JSON string literal. */
static void json_str(const char *s)
{
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;
	     p++) {
		switch (*p) {
		case '"':
			fputs("\\\"", stdout);
			break;
		case '\\':
			fputs("\\\\", stdout);
			break;
		case '\n':
			fputs("\\n", stdout);
			break;
		case '\r':
			fputs("\\r", stdout);
			break;
		case '\t':
			fputs("\\t", stdout);
			break;
		default:
			if (*p < 0x20)
				printf("\\u%04x", *p);
			else
				putchar(*p);
		}
	}
	putchar('"');
}

/* "512.0 MiB" */
static void human_size(uint64_t bytes, char *buf, size_t len)
{
	static const char *units[] = { "bytes", "KiB", "MiB", "GiB", "TiB",
				       "PiB" };
	double v = (double)bytes;
	int u = 0;

	if (bytes < 1024) {
		snprintf(buf, len, "%llu bytes", (unsigned long long)bytes);
		return;
	}
	while (v >= 1024.0 && u < 5) {
		v /= 1024.0;
		u++;
	}
	snprintf(buf, len, "%.1f %s", v, units[u]);
}

/* ---- argument parsing --------------------------------------------- */

static int parse_uint(const char *s, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (!s || !*s || *s == '-')
		return -1;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || *end)
		return -1;
	*out = v;
	return 0;
}

/* Sectors, or bytes with a suffix.  round_up: a byte value that is not a
 * whole number of sectors rounds up (positions) or down (sizes). */
static int parse_sectors(const char *s, uint32_t ss, int round_up,
			 uint64_t *out)
{
	char *end;
	unsigned long long v;
	uint64_t mult = 0;

	if (!s || !*s)
		return -1;
	if (*s == '+')
		s++;
	/* strtoull would take "-1" (and " -1") as a huge value. */
	if (*s < '0' || *s > '9')
		return -1;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || end == s)
		return -1;
	if (*end == '\0') {
		*out = v;
		return 0;
	}
	switch (*end) {
	case 'b':
	case 'B':
		mult = 1;
		break;
	case 'k':
	case 'K':
		mult = 1ULL << 10;
		break;
	case 'm':
	case 'M':
		mult = 1ULL << 20;
		break;
	case 'g':
	case 'G':
		mult = 1ULL << 30;
		break;
	case 't':
	case 'T':
		mult = 1ULL << 40;
		break;
	default:
		return -1;
	}
	end++;
	if (mult > 1 && (strcasecmp(end, "iB") == 0 || strcasecmp(end, "B") == 0))
		end += strlen(end);
	if (*end)
		return -1;
	if (v > UINT64_MAX / mult)
		return -1;
	v *= mult;
	*out = round_up ? (v + ss - 1) / ss : v / ss;
	return 0;
}

static int parse_partno(const char *s)
{
	uint64_t v;

	if (parse_uint(s, &v) || v < 1 || v > GPT_MAX_ENTRIES)
		die_usage("'%s' is not a partition number", s ? s : "");
	return (int)v;
}

static const char *need_value(int argc, char **argv, int *i)
{
	if (*i + 1 >= argc)
		die_usage("option '%s' needs a value", argv[*i]);
	return argv[++*i];
}

static void parse_args(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--") == 0) {
			for (i++; i < argc; i++) {
				if (!o->command)
					o->command = argv[i];
				else if (!o->device &&
					 strcmp(o->command, "types") != 0)
					o->device = argv[i];
				else if (o->nargs < MAX_ARGS)
					o->args[o->nargs++] = argv[i];
			}
			break;
		}
		if (a[0] == '-' && a[1]) {
			if (strcmp(a, "--json") == 0) {
				o->json = 1;
			} else if (strcmp(a, "--yes") == 0 ||
				   strcmp(a, "-y") == 0) {
				o->yes = 1;
			} else if (strcmp(a, "--no-reread") == 0) {
				o->no_reread = 1;
			} else if (strcmp(a, "--sector-size") == 0) {
				uint64_t v;
				const char *s = need_value(argc, argv, &i);

				if (parse_uint(s, &v) || (v != 512 && v != 4096))
					die_usage("sector size must be 512 or "
						  "4096");
				o->sector_size = (uint32_t)v;
			} else if (strcmp(a, "-h") == 0 ||
				   strcmp(a, "--help") == 0) {
				usage(stdout);
				exit(0);
			} else if (strcmp(a, "--version") == 0) {
				printf(PROG " (libgpt %s)\n", GPT_LIB_VERSION);
				exit(0);
			} else if (strcmp(a, "-n") == 0 ||
				   strcmp(a, "--number") == 0) {
				o->number = parse_partno(need_value(argc, argv,
								    &i));
			} else if (strcmp(a, "-s") == 0 ||
				   strcmp(a, "--start") == 0) {
				o->start = need_value(argc, argv, &i);
			} else if (strcmp(a, "-S") == 0 ||
				   strcmp(a, "--size") == 0) {
				o->size = need_value(argc, argv, &i);
			} else if (strcmp(a, "-t") == 0 ||
				   strcmp(a, "--type") == 0) {
				o->type = need_value(argc, argv, &i);
			} else if (strcmp(a, "-c") == 0 ||
				   strcmp(a, "--name") == 0) {
				o->name = need_value(argc, argv, &i);
			} else if (strcmp(a, "-u") == 0 ||
				   strcmp(a, "--uuid") == 0) {
				o->uuid = need_value(argc, argv, &i);
			} else if (strcmp(a, "-A") == 0 ||
				   strcmp(a, "--attrs") == 0) {
				o->attrs = need_value(argc, argv, &i);
			} else {
				die_usage("unknown option '%s'", a);
			}
			continue;
		}
		if (!o->command)
			o->command = a;
		else if (!o->device && strcmp(o->command, "types") != 0)
			o->device = a;
		else if (o->nargs < MAX_ARGS)
			o->args[o->nargs++] = a;
		else
			die_usage("too many arguments");
	}
}

static int parse_hex64(const char *s, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (!s || !*s)
		return -1;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	/* strtoull would take "-1" as all bits set. */
	if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
	      (*s >= 'A' && *s <= 'F')))
		return -1;
	errno = 0;
	v = strtoull(s, &end, 16);
	if (errno || *end || end == s)
		return -1;
	*out = v;
	return 0;
}

/* ---- opening ------------------------------------------------------- */

static int open_dev(const struct opts *o, struct gpt_dev *d, int write)
{
	int rc = gpt_dev_open(d, o->device,
			      write ? (GPT_OPEN_RDWR | GPT_OPEN_EXCL)
				    : GPT_OPEN_RDONLY,
			      o->sector_size);

	if (rc) {
		if (rc == -EBUSY)
			return fail("%s: in use (a partition is mounted?)",
				    o->device);
		return fail("%s: %s", o->device, strerror(-rc));
	}
	if (o->sector_size && !d->is_file && d->sector_size != o->sector_size)
		fprintf(stderr, PROG ": %s: --sector-size ignored, the device "
			"has %u-byte sectors\n", o->device, d->sector_size);
	return 0;
}

/* Reads the table for an edit: a damaged copy is refused (repair first). */
static int load_table(const struct opts *o, struct gpt_dev *d,
		      struct gpt_table *t)
{
	int rc = gpt_read(d, t);

	if (rc == -ENOENT)
		return fail("%s: no GUID partition table (use new-table)",
			    o->device);
	if (rc == -EBADMSG)
		return fail("%s: %s", o->device, t->errmsg);
	if (rc)
		return fail("%s: cannot read the table: %s", o->device,
			    strerror(-rc));
	return 0;
}

static int commit(const struct opts *o, struct gpt_dev *d,
		  struct gpt_table *t)
{
	int rc = gpt_write(d, t);

	if (rc)
		return fail("%s: %s", o->device, t->errmsg[0] ? t->errmsg :
			    strerror(-rc));
	if (!d->is_file && !o->no_reread) {
		rc = gpt_dev_reread(d, 10, 200);
		if (rc)
			fprintf(stderr, PROG ": %s: the table was written, but "
				"the kernel could not re-read it (%s); it is "
				"used after the next restart\n", o->device,
				strerror(-rc));
	}
	return 0;
}

/* ---- print ---------------------------------------------------------- */

static uint64_t total_free(const struct gpt_table *t)
{
	struct gpt_free fr[GPT_MAX_ENTRIES + 2];
	int n = gpt_free_regions(t, fr, (int)(sizeof(fr) / sizeof(fr[0])), 0);
	uint64_t sum = 0;

	for (int i = 0; i < n; i++)
		sum += fr[i].last_lba - fr[i].first_lba + 1;
	return sum;
}

static const char *status_word(int hdr, int ent)
{
	if (!hdr)
		return "damaged";
	if (!ent)
		return "entries damaged";
	return "OK";
}

static void print_gpt_text(const struct gpt_dev *d, const struct gpt_table *t)
{
	char g[GPT_GUID_STR_LEN], sz[32];
	int order[GPT_MAX_ENTRIES];
	int n = gpt_part_order(t, order, GPT_MAX_ENTRIES);
	uint64_t esec = (uint64_t)t->num_entries * t->entry_size;
	uint64_t free_sec = total_free(t);
	struct gpt_free fr[GPT_MAX_ENTRIES + 2];
	int nfr;

	esec = (esec + t->sector_size - 1) / t->sector_size;
	human_size(d->size_bytes, sz, sizeof(sz));
	printf("Disk %s: %llu sectors, %s\n", d->path,
	       (unsigned long long)d->nsectors, sz);
	printf("Sector size (logical/physical): %u/%u bytes\n", d->sector_size,
	       d->phys_sector_size);
	printf("Partition table: GPT (primary %s, backup %s%s)\n",
	       status_word(t->status.primary_header_ok,
			   t->status.primary_entries_ok),
	       status_word(t->status.backup_header_ok,
			   t->status.backup_entries_ok),
	       t->status.backup_not_at_end ? ", not at the end of the disk" : "");
	printf("Protective MBR: %s\n", mbr_kind_name(t->status.pmbr_kind));
	gpt_guid_format_upper(&t->disk_guid, g);
	printf("Disk identifier (GUID): %s\n", g);
	printf("Partition table holds up to %u entries\n", t->num_entries);
	printf("Main partition table begins at sector %llu and ends at sector "
	       "%llu\n", (unsigned long long)t->primary_entries_lba,
	       (unsigned long long)(t->primary_entries_lba + esec - 1));
	printf("First usable sector is %llu, last usable sector is %llu\n",
	       (unsigned long long)t->first_usable,
	       (unsigned long long)t->last_usable);
	printf("Partitions will be aligned on %llu-sector boundaries\n",
	       (unsigned long long)gpt_align_sectors(t));
	human_size(free_sec * t->sector_size, sz, sizeof(sz));
	printf("Total free space is %llu sectors (%s)\n",
	       (unsigned long long)free_sec, sz);
	printf("\n%-7s %15s %15s  %-11s %-5s %s\n", "Number", "Start (sector)",
	       "End (sector)", "Size", "Code", "Name");
	for (int k = 0; k < n; k++) {
		const struct gpt_part *p = &t->parts[order[k] - 1];
		const struct gpt_type_info *ti = gpt_type_lookup(&p->type);

		human_size((p->last_lba - p->first_lba + 1) * t->sector_size, sz,
			   sizeof(sz));
		printf("%6d  %15llu %15llu  %-11s %04X  %s\n", order[k],
		       (unsigned long long)p->first_lba,
		       (unsigned long long)p->last_lba, sz,
		       ti ? ti->code : 0, p->name);
	}
	if (n > 0) {
		printf("\n%-7s %-24s %-36s %s\n", "Number", "Type",
		       "Unique GUID (PARTUUID)", "Attributes");
		for (int k = 0; k < n; k++) {
			const struct gpt_part *p = &t->parts[order[k] - 1];

			gpt_guid_format(&p->uuid, g);
			printf("%6d  %-24s %-36s %016llx\n", order[k],
			       gpt_type_name(&p->type), g,
			       (unsigned long long)p->attributes);
		}
	}
	nfr = gpt_free_regions(t, fr, (int)(sizeof(fr) / sizeof(fr[0])), 1);
	if (nfr > 0) {
		printf("\nFree regions (aligned):\n");
		for (int i = 0; i < nfr; i++) {
			human_size((fr[i].last_lba - fr[i].first_lba + 1) *
					   t->sector_size, sz, sizeof(sz));
			printf("  %15llu %15llu  %s\n",
			       (unsigned long long)fr[i].first_lba,
			       (unsigned long long)fr[i].last_lba, sz);
		}
	}
}

static void print_gpt_json(const struct gpt_dev *d, const struct gpt_table *t)
{
	char g[GPT_GUID_STR_LEN];
	int order[GPT_MAX_ENTRIES];
	int n = gpt_part_order(t, order, GPT_MAX_ENTRIES);
	struct gpt_free fr[GPT_MAX_ENTRIES + 2];
	int nfr = gpt_free_regions(t, fr, (int)(sizeof(fr) / sizeof(fr[0])), 1);

	printf("{\n  \"device\": ");
	json_str(d->path);
	printf(",\n  \"label\": \"gpt\",\n");
	printf("  \"size_bytes\": %llu,\n  \"sectors\": %llu,\n",
	       (unsigned long long)d->size_bytes,
	       (unsigned long long)d->nsectors);
	printf("  \"sector_size\": %u,\n  \"physical_sector_size\": %u,\n",
	       d->sector_size, d->phys_sector_size);
	gpt_guid_format(&t->disk_guid, g);
	printf("  \"disk_guid\": \"%s\",\n", g);
	printf("  \"first_usable\": %llu,\n  \"last_usable\": %llu,\n",
	       (unsigned long long)t->first_usable,
	       (unsigned long long)t->last_usable);
	printf("  \"entries\": %u,\n  \"entry_size\": %u,\n", t->num_entries,
	       t->entry_size);
	printf("  \"primary_ok\": %s,\n  \"backup_ok\": %s,\n",
	       t->status.primary_header_ok && t->status.primary_entries_ok ?
		       "true" : "false",
	       t->status.backup_header_ok && t->status.backup_entries_ok ?
		       "true" : "false");
	printf("  \"backup_not_at_end\": %s,\n",
	       t->status.backup_not_at_end ? "true" : "false");
	printf("  \"pmbr\": \"%s\",\n", mbr_kind_name(t->status.pmbr_kind));
	printf("  \"partitions\": [");
	for (int k = 0; k < n; k++) {
		const struct gpt_part *p = &t->parts[order[k] - 1];
		const struct gpt_type_info *ti = gpt_type_lookup(&p->type);

		printf("%s\n    {\"number\": %d, \"start\": %llu, \"end\": %llu, "
		       "\"sectors\": %llu, \"size_bytes\": %llu, ",
		       k ? "," : "", order[k],
		       (unsigned long long)p->first_lba,
		       (unsigned long long)p->last_lba,
		       (unsigned long long)(p->last_lba - p->first_lba + 1),
		       (unsigned long long)((p->last_lba - p->first_lba + 1) *
					    t->sector_size));
		gpt_guid_format(&p->type, g);
		printf("\"type\": \"%s\", \"code\": \"%04x\", \"type_name\": ", g,
		       ti ? ti->code : 0);
		json_str(gpt_type_name(&p->type));
		gpt_guid_format(&p->uuid, g);
		printf(", \"uuid\": \"%s\", \"name\": ", g);
		json_str(p->name);
		printf(", \"attributes\": \"%016llx\"}",
		       (unsigned long long)p->attributes);
	}
	printf("%s],\n  \"free\": [", n ? "\n  " : "");
	for (int i = 0; i < nfr; i++)
		printf("%s\n    {\"start\": %llu, \"end\": %llu, "
		       "\"size_bytes\": %llu}", i ? "," : "",
		       (unsigned long long)fr[i].first_lba,
		       (unsigned long long)fr[i].last_lba,
		       (unsigned long long)((fr[i].last_lba - fr[i].first_lba + 1) *
					    t->sector_size));
	printf("%s]\n}\n", nfr ? "\n  " : "");
}

static void print_mbr(const struct opts *o, const struct gpt_dev *d,
		      const struct mbr_table *m)
{
	char sz[32];

	if (o->json) {
		printf("{\n  \"device\": ");
		json_str(d->path);
		printf(",\n  \"label\": \"%s\",\n",
		       m->kind == MBR_KIND_LEGACY ? "dos" : "none");
		printf("  \"size_bytes\": %llu,\n  \"sectors\": %llu,\n"
		       "  \"sector_size\": %u,\n",
		       (unsigned long long)d->size_bytes,
		       (unsigned long long)d->nsectors, d->sector_size);
		printf("  \"disk_signature\": \"%08x\",\n  \"mbr\": \"%s\",\n",
		       m->disk_signature, mbr_kind_name(m->kind));
		printf("  \"chain_broken\": %s,\n  \"partitions\": [",
		       m->chain_broken ? "true" : "false");
		for (int i = 0; i < m->nparts; i++) {
			const struct mbr_part *p = &m->parts[i];

			printf("%s\n    {\"number\": %d, \"start\": %llu, "
			       "\"sectors\": %llu, \"type\": \"%02x\", "
			       "\"type_name\": ", i ? "," : "", p->number,
			       (unsigned long long)p->first_lba,
			       (unsigned long long)p->nsectors, p->type);
			json_str(mbr_type_name(p->type));
			printf(", \"active\": %s, \"extended\": %s, "
			       "\"logical\": %s}",
			       p->status == 0x80 ? "true" : "false",
			       p->is_extended ? "true" : "false",
			       p->is_logical ? "true" : "false");
		}
		printf("%s]\n}\n", m->nparts ? "\n  " : "");
		return;
	}
	human_size(d->size_bytes, sz, sizeof(sz));
	printf("Disk %s: %llu sectors, %s\n", d->path,
	       (unsigned long long)d->nsectors, sz);
	printf("Sector size (logical/physical): %u/%u bytes\n", d->sector_size,
	       d->phys_sector_size);
	if (m->kind == MBR_KIND_NONE || m->kind == MBR_KIND_EMPTY) {
		printf("No partition table.\n");
		return;
	}
	printf("Partition table: MBR (%s), disk signature 0x%08x%s\n",
	       mbr_kind_name(m->kind), m->disk_signature,
	       m->chain_broken ? ", logical chain damaged" : "");
	printf("\n%-7s %-4s %15s %15s  %-11s %-4s %s\n", "Number", "Boot",
	       "Start (sector)", "End (sector)", "Size", "Id", "Type");
	for (int i = 0; i < m->nparts; i++) {
		const struct mbr_part *p = &m->parts[i];

		human_size(p->nsectors * d->sector_size, sz, sizeof(sz));
		printf("%6d  %-4s %15llu %15llu  %-11s %02x   %s\n", p->number,
		       p->status == 0x80 ? "*" : "",
		       (unsigned long long)p->first_lba,
		       (unsigned long long)(p->first_lba + p->nsectors -
					    (p->nsectors ? 1 : 0)),
		       sz, p->type, mbr_type_name(p->type));
	}
}

static int cmd_print(const struct opts *o)
{
	struct gpt_dev d;
	struct gpt_table t;
	int rc;

	if (open_dev(o, &d, 0))
		return 1;
	rc = gpt_read(&d, &t);
	if (rc == 0) {
		if (o->json)
			print_gpt_json(&d, &t);
		else
			print_gpt_text(&d, &t);
		gpt_table_free(&t);
	} else if (rc == -ENOENT) {
		struct mbr_table m;

		rc = mbr_read(&d, &m);
		if (rc) {
			gpt_dev_close(&d);
			return fail("%s: %s", o->device, strerror(-rc));
		}
		print_mbr(o, &d, &m);
	} else {
		gpt_dev_close(&d);
		return fail("%s: %s", o->device,
			    t.errmsg[0] ? t.errmsg : strerror(-rc));
	}
	gpt_dev_close(&d);
	return 0;
}

/* ---- info ------------------------------------------------------------- */

static int cmd_info(const struct opts *o)
{
	struct gpt_dev d;
	struct gpt_table t;
	const struct gpt_part *p;
	const struct gpt_type_info *ti;
	char g[GPT_GUID_STR_LEN], sz[32];
	int partno;

	if (o->nargs != 1)
		die_usage("info needs a partition number");
	partno = parse_partno(o->args[0]);
	if (open_dev(o, &d, 0))
		return 1;
	if (load_table(o, &d, &t)) {
		gpt_dev_close(&d);
		return 1;
	}
	p = gpt_part_get(&t, partno);
	if (!p) {
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return fail("%s: partition %d does not exist", o->device, partno);
	}
	ti = gpt_type_lookup(&p->type);
	human_size((p->last_lba - p->first_lba + 1) * t.sector_size, sz,
		   sizeof(sz));
	if (o->json) {
		gpt_guid_format(&p->type, g);
		printf("{\"number\": %d, \"type\": \"%s\", \"type_name\": ",
		       partno, g);
		json_str(gpt_type_name(&p->type));
		gpt_guid_format(&p->uuid, g);
		printf(", \"uuid\": \"%s\", \"start\": %llu, \"end\": %llu, "
		       "\"sectors\": %llu, \"attributes\": \"%016llx\", "
		       "\"name\": ", g, (unsigned long long)p->first_lba,
		       (unsigned long long)p->last_lba,
		       (unsigned long long)(p->last_lba - p->first_lba + 1),
		       (unsigned long long)p->attributes);
		json_str(p->name);
		printf("}\n");
	} else {
		gpt_guid_format_upper(&p->type, g);
		printf("Partition GUID code: %s (%s)\n", g,
		       gpt_type_name(&p->type));
		gpt_guid_format_upper(&p->uuid, g);
		printf("Partition unique GUID: %s\n", g);
		printf("First sector: %llu\n", (unsigned long long)p->first_lba);
		printf("Last sector: %llu\n", (unsigned long long)p->last_lba);
		printf("Partition size: %llu sectors (%s)\n",
		       (unsigned long long)(p->last_lba - p->first_lba + 1), sz);
		printf("Attribute flags: %016llX\n",
		       (unsigned long long)p->attributes);
		printf("Partition name: '%s'\n", p->name);
		if (ti)
			printf("Type code: %04X (%s)\n", ti->code, ti->desc);
	}
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return 0;
}

/* ---- verify ----------------------------------------------------------- */

struct vstate {
	int json;
	int count;
	int warnings;
};

static const char *sev_name(int sev)
{
	return sev == GPT_SEV_ERROR ? "error" :
	       sev == GPT_SEV_WARNING ? "warning" : "info";
}

static void verify_report(int sev, const char *msg, void *user)
{
	struct vstate *v = user;

	if (sev == GPT_SEV_WARNING)
		v->warnings++;
	if (v->json) {
		printf("%s\n    {\"severity\": \"%s\", \"message\": ",
		       v->count ? "," : "", sev_name(sev));
		json_str(msg);
		putchar('}');
	} else {
		printf("%s: %s\n", sev_name(sev), msg);
	}
	v->count++;
}

static int cmd_verify(const struct opts *o)
{
	struct gpt_dev d;
	struct gpt_table t;
	struct vstate v = { o->json, 0, 0 };
	int rc, errors;

	if (open_dev(o, &d, 0))
		return 1;
	rc = gpt_read(&d, &t);
	if (rc) {
		if (o->json) {
			printf("{\"device\": ");
			json_str(o->device);
			printf(", \"errors\": 1, \"warnings\": 0, \"messages\": "
			       "[{\"severity\": \"error\", \"message\": ");
			json_str(t.errmsg[0] ? t.errmsg : strerror(-rc));
			printf("}]}\n");
		} else {
			printf("error: %s\n", t.errmsg[0] ? t.errmsg :
			       strerror(-rc));
		}
		gpt_dev_close(&d);
		return 1;
	}
	if (o->json) {
		printf("{\"device\": ");
		json_str(o->device);
		printf(", \"messages\": [");
	}
	errors = gpt_verify(&d, &t, verify_report, &v);
	if (o->json) {
		printf("%s], \"errors\": %d, \"warnings\": %d}\n",
		       v.count ? "\n  " : "", errors, v.warnings);
	} else if (errors == 0) {
		struct gpt_free fr[GPT_MAX_ENTRIES + 2];
		int nfr = gpt_free_regions(&t, fr,
					   (int)(sizeof(fr) / sizeof(fr[0])), 0);

		printf("No problems found. %llu free sectors (%llu bytes) "
		       "available in %d region%s%s.\n",
		       (unsigned long long)total_free(&t),
		       (unsigned long long)(total_free(&t) * t.sector_size),
		       nfr < 0 ? 0 : nfr, nfr == 1 ? "" : "s",
		       v.warnings ? ", with warnings above" : "");
	} else {
		printf("%d problem%s found.\n", errors, errors == 1 ? "" : "s");
	}
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return errors ? 1 : 0;
}

/* ---- editing ---------------------------------------------------------- */

static int cmd_new_table(const struct opts *o)
{
	struct gpt_dev d;
	struct gpt_table t, old;
	struct mbr_table m;
	int rc, had = 0;

	if (open_dev(o, &d, 1))
		return 1;
	rc = gpt_read(&d, &old);
	if (rc == 0) {
		had = gpt_part_count(&old) > 0;
		gpt_table_free(&old);
	} else if (rc == -EBADMSG) {
		had = 1;
	} else if (mbr_read(&d, &m) == 0 && m.kind == MBR_KIND_LEGACY) {
		had = 1;
	}
	if (had && !o->yes) {
		gpt_dev_close(&d);
		return fail("%s already has partitions; add --yes to erase the "
			    "table", o->device);
	}
	/* The new table first: when it cannot be made (a disk too small),
	 * the old one must still be there. */
	rc = gpt_table_new(&t, d.sector_size, d.nsectors);
	if (rc) {
		gpt_dev_close(&d);
		return fail("%s: %s", o->device, t.errmsg);
	}
	rc = gpt_wipe(&d);
	if (rc) {
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return fail("%s: cannot erase the old table: %s", o->device,
			    strerror(-rc));
	}
	rc = commit(o, &d, &t);
	if (!rc) {
		char g[GPT_GUID_STR_LEN];

		gpt_guid_format(&t.disk_guid, g);
		printf("Created a new GPT on %s (disk GUID %s, sectors %llu-%llu "
		       "usable).\n", o->device, g,
		       (unsigned long long)t.first_usable,
		       (unsigned long long)t.last_usable);
	}
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return rc;
}

static int cmd_add(const struct opts *o)
{
	struct gpt_dev d;
	struct gpt_table t;
	struct gpt_part_spec spec;
	const struct gpt_part *p;
	char sz[32], g[GPT_GUID_STR_LEN];
	int rc, partno = 0;

	memset(&spec, 0, sizeof(spec));
	spec.partno = o->number;
	if (o->nargs)
		die_usage("add takes options, not arguments");
	if (open_dev(o, &d, 1))
		return 1;
	if (o->start && (parse_sectors(o->start, d.sector_size, 1,
				       &spec.first_lba) || !spec.first_lba)) {
		gpt_dev_close(&d);
		die_usage("bad start '%s'", o->start);
	}
	/* A size of 0 sectors (or bytes that round down to it) would mean
	 * "the rest of the region" to the library: refused. */
	if (o->size && (parse_sectors(o->size, d.sector_size, 0,
				      &spec.size_lba) || !spec.size_lba)) {
		gpt_dev_close(&d);
		die_usage("bad size '%s'", o->size);
	}
	if (o->type && gpt_type_parse(o->type, &spec.type)) {
		gpt_dev_close(&d);
		die_usage("unknown type '%s' (see '" PROG " types')", o->type);
	}
	if (o->uuid && gpt_guid_parse(o->uuid, &spec.uuid)) {
		gpt_dev_close(&d);
		die_usage("bad GUID '%s'", o->uuid);
	}
	if (o->attrs && parse_hex64(o->attrs, &spec.attributes)) {
		gpt_dev_close(&d);
		die_usage("bad attributes '%s'", o->attrs);
	}
	spec.name = o->name;
	if (load_table(o, &d, &t)) {
		gpt_dev_close(&d);
		return 1;
	}
	rc = gpt_part_add(&t, &spec, &partno);
	if (rc) {
		fail("%s: %s", o->device, t.errmsg);
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return 1;
	}
	rc = commit(o, &d, &t);
	if (!rc) {
		p = gpt_part_get(&t, partno);
		human_size((p->last_lba - p->first_lba + 1) * t.sector_size, sz,
			   sizeof(sz));
		gpt_guid_format(&p->uuid, g);
		if (o->json)
			printf("{\"number\": %d, \"start\": %llu, \"end\": %llu, "
			       "\"uuid\": \"%s\"}\n", partno,
			       (unsigned long long)p->first_lba,
			       (unsigned long long)p->last_lba, g);
		else
			printf("Created partition %d: sectors %llu-%llu (%s), "
			       "%s, PARTUUID %s\n", partno,
			       (unsigned long long)p->first_lba,
			       (unsigned long long)p->last_lba, sz,
			       gpt_type_name(&p->type), g);
	}
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return rc;
}

/* The shared shape of delete / resize / name / type / attrs. */
static int cmd_edit(const struct opts *o)
{
	struct gpt_dev d;
	struct gpt_table t;
	const char *c = o->command;
	int need = strcmp(c, "delete") == 0 ? 1 : 2;
	int partno, rc;

	if (o->nargs != need)
		die_usage("%s needs %s", c, need == 1 ? "a partition number" :
			  "a partition number and a value");
	partno = parse_partno(o->args[0]);
	if (open_dev(o, &d, 1))
		return 1;
	if (load_table(o, &d, &t)) {
		gpt_dev_close(&d);
		return 1;
	}
	if (strcmp(c, "delete") == 0) {
		rc = gpt_part_delete(&t, partno);
	} else if (strcmp(c, "resize") == 0) {
		uint64_t size = 0;

		/* Size 0 means "max" to the library; only "max" says so. */
		if (strcmp(o->args[1], "max") != 0 &&
		    (parse_sectors(o->args[1], t.sector_size, 0, &size) ||
		     !size)) {
			gpt_table_free(&t);
			gpt_dev_close(&d);
			die_usage("bad size '%s'", o->args[1]);
		}
		rc = gpt_part_resize(&t, partno, size);
	} else if (strcmp(c, "name") == 0) {
		rc = gpt_part_set_name(&t, partno, o->args[1]);
	} else if (strcmp(c, "type") == 0) {
		struct gpt_guid g;

		if (gpt_type_parse(o->args[1], &g)) {
			gpt_table_free(&t);
			gpt_dev_close(&d);
			die_usage("unknown type '%s' (see '" PROG " types')",
				  o->args[1]);
		}
		rc = gpt_part_set_type(&t, partno, &g);
	} else {
		uint64_t a;

		if (parse_hex64(o->args[1], &a)) {
			gpt_table_free(&t);
			gpt_dev_close(&d);
			die_usage("bad attributes '%s'", o->args[1]);
		}
		rc = gpt_part_set_attributes(&t, partno, a);
	}
	if (rc) {
		fail("%s: %s", o->device, t.errmsg);
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return 1;
	}
	rc = commit(o, &d, &t);
	if (!rc && !o->json) {
		const struct gpt_part *p = gpt_part_get(&t, partno);

		if (strcmp(c, "delete") == 0)
			printf("Deleted partition %d.\n", partno);
		else if (strcmp(c, "resize") == 0 && p)
			printf("Partition %d now ends at sector %llu (%llu "
			       "sectors).\n", partno,
			       (unsigned long long)p->last_lba,
			       (unsigned long long)(p->last_lba - p->first_lba +
						    1));
		else
			printf("Partition %d changed.\n", partno);
	}
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return rc;
}

static int cmd_repair(const struct opts *o)
{
	struct gpt_dev d;
	struct gpt_table t;
	int rc;

	if (open_dev(o, &d, 1))
		return 1;
	rc = gpt_read(&d, &t);
	if (rc == -ENOENT) {
		gpt_dev_close(&d);
		return fail("%s: no GUID partition table to repair", o->device);
	}
	if (rc) {
		gpt_dev_close(&d);
		return fail("%s: %s; nothing left to repair from", o->device,
			    t.errmsg);
	}
	if (t.status.primary_header_ok && t.status.primary_entries_ok &&
	    t.status.backup_header_ok && t.status.backup_entries_ok &&
	    !t.status.backup_not_at_end && !t.status.headers_differ &&
	    t.status.pmbr_kind == MBR_KIND_PROTECTIVE) {
		printf("%s: the table is intact; rewriting it anyway.\n",
		       o->device);
	} else {
		if (!t.status.primary_header_ok || !t.status.primary_entries_ok)
			printf("Primary table: %s -> rebuilt from the backup.\n",
			       t.status.primary_problem);
		if (!t.status.backup_header_ok || !t.status.backup_entries_ok)
			printf("Backup table: %s -> rebuilt from the primary.\n",
			       t.status.backup_problem);
		if (t.status.backup_not_at_end)
			printf("Backup table moved to the end of the disk.\n");
		if (t.status.headers_differ)
			printf("The copies differed; the %s one was kept.\n",
			       t.status.using_backup ? "backup" : "primary");
		if (t.status.pmbr_kind != MBR_KIND_PROTECTIVE)
			printf("Protective MBR rewritten (was: %s).\n",
			       mbr_kind_name(t.status.pmbr_kind));
	}
	rc = gpt_repair(&d, &t);
	if (rc) {
		fail("%s: %s", o->device, t.errmsg);
		gpt_table_free(&t);
		gpt_dev_close(&d);
		return 1;
	}
	if (!d.is_file && !o->no_reread && gpt_dev_reread(&d, 10, 200))
		fprintf(stderr, PROG ": %s: the kernel could not re-read the "
			"table; it is used after the next restart\n", o->device);
	printf("Repaired: both copies written, usable sectors %llu-%llu.\n",
	       (unsigned long long)t.first_usable,
	       (unsigned long long)t.last_usable);
	gpt_table_free(&t);
	gpt_dev_close(&d);
	return 0;
}

static int cmd_wipe(const struct opts *o)
{
	struct gpt_dev d;
	int rc;

	if (!o->yes)
		return fail("wipe erases the partition table of %s; add --yes",
			    o->device);
	if (open_dev(o, &d, 1))
		return 1;
	rc = gpt_wipe(&d);
	if (!rc && !d.is_file && !o->no_reread)
		(void)gpt_dev_reread(&d, 10, 200);
	gpt_dev_close(&d);
	if (rc)
		return fail("%s: %s", o->device, strerror(-rc));
	printf("Erased the partition table of %s.\n", o->device);
	return 0;
}

static int cmd_types(const struct opts *o)
{
	int n = gpt_type_count();

	if (o->json)
		printf("[");
	for (int i = 0; i < n; i++) {
		const struct gpt_type_info *ti = gpt_type_at(i);

		if (o->json) {
			printf("%s\n  {\"guid\": \"%s\", \"code\": \"%04x\", "
			       "\"alias\": ", i ? "," : "", ti->guid, ti->code);
			json_str(ti->alias ? ti->alias : "");
			printf(", \"name\": ");
			json_str(ti->name);
			printf(", \"description\": ");
			json_str(ti->desc);
			putchar('}');
		} else {
			printf("%04X  %-17s %-36s %s\n", ti->code,
			       ti->alias ? ti->alias : "-", ti->guid, ti->name);
		}
	}
	if (o->json)
		printf("\n]\n");
	return 0;
}

int main(int argc, char **argv)
{
	struct opts o;

	parse_args(argc, argv, &o);
	if (!o.command)
		die_usage("no command given");
	if (strcmp(o.command, "help") == 0) {
		usage(stdout);
		return 0;
	}
	if (strcmp(o.command, "types") == 0)
		return cmd_types(&o);
	if (!o.device)
		die_usage("%s needs a device or image file", o.command);
	if (strcmp(o.command, "print") == 0 || strcmp(o.command, "list") == 0)
		return cmd_print(&o);
	if (strcmp(o.command, "verify") == 0)
		return cmd_verify(&o);
	if (strcmp(o.command, "info") == 0)
		return cmd_info(&o);
	if (strcmp(o.command, "new-table") == 0)
		return cmd_new_table(&o);
	if (strcmp(o.command, "add") == 0)
		return cmd_add(&o);
	if (strcmp(o.command, "delete") == 0 || strcmp(o.command, "resize") == 0 ||
	    strcmp(o.command, "name") == 0 || strcmp(o.command, "type") == 0 ||
	    strcmp(o.command, "attrs") == 0)
		return cmd_edit(&o);
	if (strcmp(o.command, "repair") == 0)
		return cmd_repair(&o);
	if (strcmp(o.command, "wipe") == 0)
		return cmd_wipe(&o);
	die_usage("unknown command '%s'", o.command);
}
