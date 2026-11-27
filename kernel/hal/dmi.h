// LikeOS HAL - platform identification from the SMBIOS (DMI) tables
//
// Drivers that carry per-machine quirks match the platform by the strings
// of the SMBIOS structure table: BIOS information (type 0), system
// information (type 1), baseboard (type 2) and chassis (type 3).  The
// table is found through the entry points the bootloader took from the
// UEFI configuration tables, else in the legacy BIOS area, read once on
// first use and kept as strings (trailing blanks removed).
//
// Matching follows the established convention: an entry lists up to four
// (field, string) pairs that must all hold; a pair holds when the field
// contains the string (`exact' asks for equality instead).  A field the
// firmware does not provide never matches.
//
// The identification is also exported to user space under
// /sys/devices/virtual/dmi/id (one read-only file per field the firmware
// provides, serial numbers and the UUID readable by root only, plus
// `modalias' and `uevent' with the "dmi:bvn...:" match string), which
// hardware databases and input quirk tables key on.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2007 Lennart Poettering

#ifndef KERNEL_HAL_DMI_H
#define KERNEL_HAL_DMI_H

#include <kernel/uapi/types.h>

enum dmi_field {
	DMI_NONE = 0,
	DMI_BIOS_VENDOR,
	DMI_BIOS_VERSION,
	DMI_BIOS_DATE,
	DMI_SYS_VENDOR,
	DMI_PRODUCT_NAME,
	DMI_PRODUCT_VERSION,
	DMI_PRODUCT_SKU,
	DMI_PRODUCT_FAMILY,
	DMI_BOARD_VENDOR,
	DMI_BOARD_NAME,
	DMI_BOARD_VERSION,
	DMI_CHASSIS_VENDOR,
	DMI_CHASSIS_TYPE, /* the type number, in decimal */
	DMI_BIOS_RELEASE, /* "major.minor" of the system BIOS */
	DMI_EC_FIRMWARE_RELEASE, /* "major.minor" of the EC firmware */
	DMI_PRODUCT_SERIAL,
	DMI_PRODUCT_UUID,
	DMI_BOARD_SERIAL,
	DMI_BOARD_ASSET_TAG,
	DMI_CHASSIS_VERSION,
	DMI_CHASSIS_SERIAL,
	DMI_CHASSIS_ASSET_TAG,
	DMI_STRING_MAX,
};

#define DMI_MAX_MATCHES 4

struct dmi_strmatch {
	unsigned char slot; /* enum dmi_field */
	unsigned char exact;
	const char *substr;
};

struct dmi_system_id {
	const char *ident;
	struct dmi_strmatch matches[DMI_MAX_MATCHES];
	const void *driver_data;
};

#define DMI_MATCH(a, b) { .slot = (a), .exact = 0, .substr = (b) }
#define DMI_EXACT_MATCH(a, b) { .slot = (a), .exact = 1, .substr = (b) }

/* The string of a field, "" when the firmware gives none (or there is no
 * SMBIOS).  Strings as the firmware has them; one of only blanks is "".
 * Process context on first use (maps the table). */
const char *dmi_get_system_info(enum dmi_field field);
/* Does the firmware provide the field (possibly empty). */
int dmi_field_present(enum dmi_field field);
/* The "dmi:bvn<..>:bvr<..>:...:" match string (fields the firmware
 * provides, in the fixed order, blanks, colons and non-ASCII left out)
 * into buf (cap bytes, NUL-terminated when cap > 0); the full length
 * (without the NUL), which may exceed cap. */
long dmi_get_modalias(char *buf, long cap);
/* Create /sys/devices/virtual/dmi/id and /sys/class/dmi/id (once, after
 * the ACPI tables are reachable). */
void dmi_sysfs_init(void);
/* Does entry `id' match this machine. */
int dmi_matches(const struct dmi_system_id *id);
/* The first entry of a table (ended by an entry without matches) that
 * matches, or NULL. */
const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list);
/* Decode a structure table (`count' structures, 0 = up to the end-of-table
 * structure) as the machine's identification, replacing what was there.
 * The firmware path uses it; so does the host test harness. */
void dmi_load_table(const uint8_t *table, uint32_t len, uint32_t count);
/* The SMBIOS version of the next dmi_load_table (major << 16 | minor << 8
 * | revision; the UUID's byte order depends on it).  The firmware path
 * takes it from the entry point. */
void dmi_set_version(uint32_t version);

#endif /* KERNEL_HAL_DMI_H */
