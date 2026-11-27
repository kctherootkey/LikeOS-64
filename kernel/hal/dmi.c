// LikeOS HAL - platform identification from the SMBIOS (DMI) tables
//
// See dmi.h.  The strings are decoded once (the first query, or
// dmi_load_table) into a static set; later queries only read them.  The
// first structure of each type wins (boards that list a second BIOS or
// chassis structure keep the first's strings), a field is present when
// its structure is long enough to hold it, and the strings are kept as
// the firmware wrote them (only one made of blanks alone counts as
// empty), so that tables written for the firmware's strings match them.
//
// The modalias string is "dmi" followed by ":<prefix><value>" for each
// present field of a fixed list, with blanks, colons and non-ASCII
// characters left out of the values, and a closing ':'.  The sysfs export
// is dmi_id.c.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2007 Lennart Poettering

#include <kernel/hal/dmi.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

/* The ACPICA headers carry unused parameters in their stubs. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <acpi.h>
#pragma GCC diagnostic pop

#define DMI_STR_LEN 128
#define DMI_SCAN_BASE 0xf0000u
#define DMI_SCAN_LEN 0x10000u
#define DMI_TABLE_MAX (1024u * 1024u) /* sanity bound on the table */

extern uint64_t g_boot_smbios_entry;
extern uint64_t g_boot_smbios3_entry;

static char g_dmi[DMI_STRING_MAX][DMI_STR_LEN];
static uint32_t g_dmi_present; /* bit per enum dmi_field */
static uint32_t g_dmi_ver; /* major << 16 | minor << 8 | revision */
/* 0: not read yet, 1: read (or nothing found).  Set with release order
 * after the strings are complete; the first reader scans under
 * g_dmi_scan_lock, so the strings are written once and only read after. */
static volatile int g_dmi_state;
static DEFINE_MUTEX(g_dmi_scan_lock);

static int dmi_checksum_ok(const uint8_t *p, uint32_t len)
{
	uint8_t sum = 0;

	for (uint32_t i = 0; i < len; i++)
		sum = (uint8_t)(sum + p[i]);
	return sum == 0;
}

static int dmi_have(enum dmi_field f)
{
	return (g_dmi_present >> f) & 1;
}

static void dmi_mark(enum dmi_field f)
{
	g_dmi_present |= 1u << f;
}

/* String number `idx' (1-based) of the structure whose formatted area is
 * [s, s + flen) and whose string set ends before `end': as written, ""
 * for string 0 and for a string of blanks only. */
static void dmi_string(const uint8_t *s, uint32_t flen, const uint8_t *end,
		       uint8_t idx, char *out)
{
	const uint8_t *p = s + flen;
	uint8_t n = 1;
	size_t k = 0;

	out[0] = 0;
	if (!idx)
		return;
	while (p < end && *p) {
		if (n == idx) {
			const uint8_t *q = p;

			while (q < end && *q == ' ')
				q++;
			if (q >= end || !*q)
				return; /* blanks only */
			while (p < end && *p && k + 1 < DMI_STR_LEN)
				out[k++] = (char)*p++;
			out[k] = 0;
			return;
		}
		while (p < end && *p)
			p++;
		p++; /* the string's NUL */
		n++;
	}
}

static void dmi_save_ident(const uint8_t *s, uint32_t flen, const uint8_t *end,
			   uint32_t off, enum dmi_field f)
{
	if (dmi_have(f) || flen <= off)
		return;
	dmi_string(s, flen, end, s[off], g_dmi[f]);
	dmi_mark(f);
}

/* "major.minor" from the bytes at off - 1 and off; both 0xff: not
 * supported. */
static void dmi_save_release(const uint8_t *s, uint32_t flen, uint32_t off,
			     enum dmi_field f)
{
	if (dmi_have(f) || flen <= off)
		return;
	if (s[off - 1] == 0xff && s[off] == 0xff)
		return;
	ksnprintf(g_dmi[f], DMI_STR_LEN, "%u.%u", (unsigned int)s[off - 1],
		  (unsigned int)s[off]);
	dmi_mark(f);
}

static void dmi_save_uuid(const uint8_t *s, uint32_t flen, uint32_t off,
			  enum dmi_field f)
{
	static const char hex[] = "0123456789abcdef";
	/* SMBIOS 2.6 and later store the first three fields
	 * little-endian. */
	static const uint8_t le[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
					8, 9, 10, 11, 12, 13, 14, 15 };
	const uint8_t *d = s + off;
	int is_ff = 1, is_00 = 1;
	char *out = g_dmi[f];
	int k = 0;

	if (dmi_have(f) || flen < off + 16)
		return;
	for (int i = 0; i < 16; i++) {
		if (d[i] != 0x00)
			is_00 = 0;
		if (d[i] != 0xff)
			is_ff = 0;
	}
	if (is_ff || is_00)
		return;
	for (int i = 0; i < 16; i++) {
		uint8_t b = d[g_dmi_ver >= 0x020600 ? le[i] : i];

		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[k++] = '-';
		out[k++] = hex[b >> 4];
		out[k++] = hex[b & 15];
	}
	out[k] = 0;
	dmi_mark(f);
}

static void dmi_save_type(const uint8_t *s, uint32_t flen, uint32_t off,
			  enum dmi_field f)
{
	if (dmi_have(f) || flen <= off)
		return;
	ksnprintf(g_dmi[f], DMI_STR_LEN, "%u", (unsigned int)(s[off] & 0x7f));
	dmi_mark(f);
}

void dmi_set_version(uint32_t version)
{
	g_dmi_ver = version;
}

void dmi_load_table(const uint8_t *t, uint32_t len, uint32_t max_count)
{
	uint32_t pos = 0, count = 0;

	kmemset(g_dmi, 0, sizeof(g_dmi));
	g_dmi_present = 0;
	while (t && pos + 4 <= len && (!max_count || count < max_count)) {
		uint8_t type = t[pos], flen = t[pos + 1];
		const uint8_t *s = t + pos, *end;
		uint32_t next;

		/* A short structure leaves the next one unknowable. */
		if (flen < 4 || pos + flen > len)
			break;
		/* The string set ends with two NULs; a structure that runs
		 * off the table is not decoded. */
		next = pos + flen;
		while (next + 1 < len && (t[next] || t[next + 1]))
			next++;
		if (next + 1 >= len)
			break;
		end = t + next + 1;
		switch (type) {
		case 0: /* BIOS information */
			dmi_save_ident(s, flen, end, 0x04, DMI_BIOS_VENDOR);
			dmi_save_ident(s, flen, end, 0x05, DMI_BIOS_VERSION);
			dmi_save_ident(s, flen, end, 0x08, DMI_BIOS_DATE);
			dmi_save_release(s, flen, 0x15, DMI_BIOS_RELEASE);
			dmi_save_release(s, flen, 0x17,
					 DMI_EC_FIRMWARE_RELEASE);
			break;
		case 1: /* system information */
			dmi_save_ident(s, flen, end, 0x04, DMI_SYS_VENDOR);
			dmi_save_ident(s, flen, end, 0x05, DMI_PRODUCT_NAME);
			dmi_save_ident(s, flen, end, 0x06, DMI_PRODUCT_VERSION);
			dmi_save_ident(s, flen, end, 0x07, DMI_PRODUCT_SERIAL);
			dmi_save_uuid(s, flen, 0x08, DMI_PRODUCT_UUID);
			dmi_save_ident(s, flen, end, 0x19, DMI_PRODUCT_SKU);
			dmi_save_ident(s, flen, end, 0x1a, DMI_PRODUCT_FAMILY);
			break;
		case 2: /* baseboard information */
			dmi_save_ident(s, flen, end, 0x04, DMI_BOARD_VENDOR);
			dmi_save_ident(s, flen, end, 0x05, DMI_BOARD_NAME);
			dmi_save_ident(s, flen, end, 0x06, DMI_BOARD_VERSION);
			dmi_save_ident(s, flen, end, 0x07, DMI_BOARD_SERIAL);
			dmi_save_ident(s, flen, end, 0x08, DMI_BOARD_ASSET_TAG);
			break;
		case 3: /* chassis */
			dmi_save_ident(s, flen, end, 0x04, DMI_CHASSIS_VENDOR);
			dmi_save_type(s, flen, 0x05, DMI_CHASSIS_TYPE);
			dmi_save_ident(s, flen, end, 0x06, DMI_CHASSIS_VERSION);
			dmi_save_ident(s, flen, end, 0x07, DMI_CHASSIS_SERIAL);
			dmi_save_ident(s, flen, end, 0x08,
				       DMI_CHASSIS_ASSET_TAG);
			break;
		default:
			break;
		}
		pos = next + 2;
		count++;
		/* Without a structure count (64-bit entry point) the
		 * end-of-table structure ends it; with one, OEM structures
		 * have been seen behind it, so the count rules. */
		if (!max_count && type == 127)
			break;
	}
	__atomic_store_n(&g_dmi_state, 1, __ATOMIC_RELEASE);
}

/* An entry point at `e' (`avail' readable bytes): the 64-bit "_SM3_" one
 * or the 32-bit "_SM_" one with its "_DMI_" anchor. */
static int dmi_parse_entry(const uint8_t *e, uint32_t avail, int want3,
			   uint64_t *table, uint32_t *tlen, uint32_t *tcount,
			   uint32_t *ver)
{
	uint8_t elen;
	uint32_t smbios_ver;

	if (want3) {
		if (avail < 0x18 || kmemcmp(e, "_SM3_", 5))
			return 0;
		elen = e[6];
		if (elen < 0x18 || elen > 0x20 || elen > avail ||
		    !dmi_checksum_ok(e, elen))
			return 0;
		*tlen = (uint32_t)e[0x0c] | ((uint32_t)e[0x0d] << 8) |
			((uint32_t)e[0x0e] << 16) | ((uint32_t)e[0x0f] << 24);
		*table = 0;
		for (int b = 7; b >= 0; b--)
			*table = (*table << 8) | e[0x10 + b];
		*tcount = 0;
		*ver = ((uint32_t)e[7] << 16) | ((uint32_t)e[8] << 8) | e[9];
		return 1;
	}
	if (avail < 0x1f || kmemcmp(e, "_SM_", 4))
		return 0;
	elen = e[5];
	if (elen < 0x1f || elen > avail || !dmi_checksum_ok(e, elen))
		return 0;
	if (kmemcmp(e + 0x10, "_DMI_", 5) || !dmi_checksum_ok(e + 0x10, 0x0f))
		return 0;
	*tlen = (uint32_t)e[0x16] | ((uint32_t)e[0x17] << 8);
	*table = (uint64_t)e[0x18] | ((uint64_t)e[0x19] << 8) |
		 ((uint64_t)e[0x1a] << 16) | ((uint64_t)e[0x1b] << 24);
	*tcount = (uint32_t)e[0x1c] | ((uint32_t)e[0x1d] << 8);
	/* Some firmware reports impossible versions; the known ones are
	 * corrected as they are meant. */
	smbios_ver = ((uint32_t)e[6] << 8) | e[7];
	if (smbios_ver == 0x021f || smbios_ver == 0x0221)
		smbios_ver = 0x0203;
	else if (smbios_ver == 0x0233)
		smbios_ver = 0x0206;
	*ver = smbios_ver << 8;
	return 1;
}

static int dmi_boot_entry(uint64_t phys, int want3, uint64_t *table,
			  uint32_t *tlen, uint32_t *tcount, uint32_t *ver)
{
	const uint8_t *e;
	int ok;

	if (!phys)
		return 0;
	e = AcpiOsMapMemory((ACPI_PHYSICAL_ADDRESS)phys, 0x20);
	if (!e)
		return 0;
	ok = dmi_parse_entry(e, 0x20, want3, table, tlen, tcount, ver);
	AcpiOsUnmapMemory((void *)e, 0x20);
	return ok;
}

static void dmi_scan_firmware(void)
{
	uint64_t table = 0;
	uint32_t tlen = 0, tcount = 0, ver = 0;
	const uint8_t *t;
	int found;

	/* UEFI: the entry points the bootloader reported, the 64-bit one
	 * first (its table may lie above 4 GB). */
	found = dmi_boot_entry(g_boot_smbios3_entry, 1, &table, &tlen,
			       &tcount, &ver) ||
		dmi_boot_entry(g_boot_smbios_entry, 0, &table, &tlen, &tcount,
			       &ver);
	if (!found) {
		/* else the legacy BIOS area, on 16-byte boundaries */
		const uint8_t *area = AcpiOsMapMemory(
			(ACPI_PHYSICAL_ADDRESS)DMI_SCAN_BASE, DMI_SCAN_LEN);

		if (area) {
			for (int want3 = 1; want3 >= 0 && !found; want3--)
				for (uint32_t off = 0;
				     off + 0x20 <= DMI_SCAN_LEN && !found;
				     off += 16)
					found = dmi_parse_entry(
						area + off, DMI_SCAN_LEN - off,
						want3, &table, &tlen, &tcount,
						&ver);
			AcpiOsUnmapMemory((void *)area, DMI_SCAN_LEN);
		}
	}
	if (!found || !table || tlen < 4) {
		__atomic_store_n(&g_dmi_state, 1, __ATOMIC_RELEASE);
		return;
	}
	if (tlen > DMI_TABLE_MAX)
		tlen = DMI_TABLE_MAX;
	t = AcpiOsMapMemory((ACPI_PHYSICAL_ADDRESS)table, tlen);
	if (!t) {
		__atomic_store_n(&g_dmi_state, 1, __ATOMIC_RELEASE);
		return;
	}
	dmi_set_version(ver);
	dmi_load_table(t, tlen, tcount);
	AcpiOsUnmapMemory((void *)t, tlen);
	kprintf("dmi: SMBIOS %u.%u: %s %s, board %s %s, BIOS %s %s\n",
		ver >> 16, (ver >> 8) & 0xff, g_dmi[DMI_SYS_VENDOR],
		g_dmi[DMI_PRODUCT_NAME], g_dmi[DMI_BOARD_VENDOR],
		g_dmi[DMI_BOARD_NAME], g_dmi[DMI_BIOS_VERSION],
		g_dmi[DMI_BIOS_DATE]);
}

static void dmi_ensure_loaded(void)
{
	if (!__atomic_load_n(&g_dmi_state, __ATOMIC_ACQUIRE)) {
		might_sleep();
		mutex_lock(&g_dmi_scan_lock);
		if (!__atomic_load_n(&g_dmi_state, __ATOMIC_ACQUIRE))
			dmi_scan_firmware();
		mutex_unlock(&g_dmi_scan_lock);
	}
}

const char *dmi_get_system_info(enum dmi_field field)
{
	if ((unsigned int)field >= DMI_STRING_MAX || field == DMI_NONE)
		return "";
	dmi_ensure_loaded();
	return g_dmi[field];
}

int dmi_field_present(enum dmi_field field)
{
	if ((unsigned int)field >= DMI_STRING_MAX || field == DMI_NONE)
		return 0;
	dmi_ensure_loaded();
	return dmi_have(field);
}

static int dmi_str_has(const char *s, const char *needle)
{
	size_t n = kstrlen(needle);

	if (!n)
		return 1;
	for (; *s; s++)
		if (!kstrncmp(s, needle, n))
			return 1;
	return 0;
}

int dmi_matches(const struct dmi_system_id *id)
{
	int any = 0;

	for (int i = 0; i < DMI_MAX_MATCHES; i++) {
		const struct dmi_strmatch *m = &id->matches[i];
		const char *v;

		if (m->slot == DMI_NONE)
			continue;
		if (m->slot >= DMI_STRING_MAX || !m->substr)
			return 0;
		if (!dmi_field_present((enum dmi_field)m->slot))
			return 0;
		v = dmi_get_system_info((enum dmi_field)m->slot);
		if (m->exact ? kstrcmp(v, m->substr) != 0 :
			       !dmi_str_has(v, m->substr))
			return 0;
		any = 1;
	}
	return any;
}

const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list)
{
	for (const struct dmi_system_id *d = list; d && d->matches[0].slot;
	     d++)
		if (dmi_matches(d))
			return d;
	return NULL;
}

/* ==========================================================================
 * modalias
 * ========================================================================== */

/* Append to buf at *pos (counting what does not fit). */
static void ma_putc(char *buf, long cap, long *pos, char c)
{
	if (*pos + 1 < cap)
		buf[*pos] = c;
	(*pos)++;
}

long dmi_get_modalias(char *buf, long cap)
{
	/* New fields only ever go at the end: match patterns are prefixes
	 * of this string. */
	static const struct {
		const char *prefix;
		enum dmi_field field;
	} fields[] = {
		{ "bvn", DMI_BIOS_VENDOR },
		{ "bvr", DMI_BIOS_VERSION },
		{ "bd", DMI_BIOS_DATE },
		{ "br", DMI_BIOS_RELEASE },
		{ "efr", DMI_EC_FIRMWARE_RELEASE },
		{ "svn", DMI_SYS_VENDOR },
		{ "pn", DMI_PRODUCT_NAME },
		{ "pvr", DMI_PRODUCT_VERSION },
		{ "rvn", DMI_BOARD_VENDOR },
		{ "rn", DMI_BOARD_NAME },
		{ "rvr", DMI_BOARD_VERSION },
		{ "cvn", DMI_CHASSIS_VENDOR },
		{ "ct", DMI_CHASSIS_TYPE },
		{ "cvr", DMI_CHASSIS_VERSION },
		{ "sku", DMI_PRODUCT_SKU },
		{ "pfa", DMI_PRODUCT_FAMILY },
	};
	long pos = 0;

	dmi_ensure_loaded();
	for (const char *p = "dmi"; *p; p++)
		ma_putc(buf, cap, &pos, *p);
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (!dmi_have(fields[i].field))
			continue;
		ma_putc(buf, cap, &pos, ':');
		for (const char *p = fields[i].prefix; *p; p++)
			ma_putc(buf, cap, &pos, *p);
		for (const unsigned char *v =
			     (const unsigned char *)g_dmi[fields[i].field];
		     *v; v++)
			if (*v > ' ' && *v < 127 && *v != ':')
				ma_putc(buf, cap, &pos, (char)*v);
	}
	ma_putc(buf, cap, &pos, ':');
	if (cap > 0)
		buf[pos < cap ? pos : cap - 1] = 0;
	return pos;
}
