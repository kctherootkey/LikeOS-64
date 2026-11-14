/* libgpt -- partition type GUIDs.
 *
 * The types a disk is likely to carry, with the four-digit hex codes that
 * partitioning tools customarily use as short names for them (ef00 for the
 * EFI system partition and so on), a one-word alias for command lines, a
 * short name for tables and a sentence for a person.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gpt.h"

static const struct gpt_type_info g_types[] = {
	/* What LikeOS itself installs. */
	{ GPT_TYPE_ESP, 0xef00, "esp", "EFI system",
	  "EFI system partition: FAT file system with the boot loaders" },
	{ GPT_TYPE_LIKEOS_ROOT, 0x8304, "likeos-root", "LikeOS root",
	  "Root file system of a 64-bit x86 system (LikeOS root)" },
	{ GPT_TYPE_GENERIC_DATA, 0x8300, "data", "Generic data",
	  "File system data of any kind" },
	{ GPT_TYPE_SWAP, 0x8200, "swap", "Swap",
	  "Swap space for paged-out memory" },
	{ GPT_TYPE_BIOS_BOOT, 0xef02, "bios-boot", "BIOS boot",
	  "BIOS boot partition: second stage of a legacy boot loader" },
	{ "bc13c2ff-59e6-4262-a352-b275fd6f7172", 0xea00, "xbootldr",
	  "Extended boot loader",
	  "Extended boot loader partition for kernels and boot entries" },

	/* Other root and system partitions of the discoverable layout. */
	{ "44479540-f297-41b2-9af7-d131d5f0458a", 0x8303, "root-x86",
	  "Root (x86)", "Root file system of a 32-bit x86 system" },
	{ "b921b045-1df0-41c3-af44-4c6f280d3fae", 0x8305, "root-arm64",
	  "Root (ARM64)", "Root file system of a 64-bit ARM system" },
	{ "69dad710-2ce4-4e3c-b16c-21a1d49abed3", 0x8307, "root-arm",
	  "Root (ARM)", "Root file system of a 32-bit ARM system" },
	{ "72ec70a6-cf74-40e6-bd49-4bda08e8f224", 0x8329, "root-riscv64",
	  "Root (RISC-V 64)", "Root file system of a 64-bit RISC-V system" },
	{ "8484680c-9521-48c6-9c11-b0720656f69e", 0x8314, "usr",
	  "/usr (x86-64)", "The /usr file system of a 64-bit x86 system" },
	{ "b0e01050-ee5f-4390-949a-9101b17104e9", 0x8316, "usr-arm64",
	  "/usr (ARM64)", "The /usr file system of a 64-bit ARM system" },
	{ "933ac7e1-2eb4-4f13-b844-0e14e2aef915", 0x8302, "home", "Home",
	  "Home directories of the users" },
	{ "3b8f8425-20e0-4f3b-907f-1a25a76f98e8", 0x8306, "srv",
	  "Server data", "Data served by the system (/srv)" },
	{ "4d21b016-b534-45c2-a9fb-5c16e091fd2d", 0x8310, "var", "/var",
	  "Variable data (/var)" },
	{ "7ec6f557-3bc5-4aca-b293-16ef5df639d1", 0x8311, "var-tmp",
	  "/var/tmp", "Temporary data kept across reboots (/var/tmp)" },
	{ "773f91ef-66d4-49b5-bd83-d683bf40ad16", 0x8312, "user-home",
	  "Per-user home", "A home directory of one user" },
	{ "ca7d7ccb-63ed-4c53-861c-1742536059cc", 0x8309, "luks",
	  "Encrypted (LUKS)", "Encrypted volume in the LUKS format" },
	{ "7ffec5c9-2d00-49b7-8941-3ea10a5586b7", 0x8308, "crypt",
	  "Encrypted (plain)", "Encrypted volume without a header" },
	{ "e6d6d379-f507-44c2-a23c-238f2a3df928", 0x8e00, "lvm", "LVM",
	  "Physical volume of a logical volume manager" },
	{ "a19d880f-05fc-4d3b-a006-743f0f84911e", 0xfd00, "raid", "RAID",
	  "Member of a software RAID array" },
	{ "8da63339-0007-60c0-c436-083ac8230908", 0x8301, "reserved",
	  "Reserved", "Reserved space" },

	/* Other operating systems. */
	{ GPT_TYPE_MSR, 0x0c01, "msr", "Reserved (MSR)",
	  "Reserved partition of another system (MSR)" },
	{ GPT_TYPE_BASIC_DATA, 0x0700, "basic", "Basic data",
	  "Basic data: NTFS, exFAT or FAT file system" },
	{ "de94bba4-06d1-4d40-a16a-bfd50179d6ac", 0x2700, "recovery",
	  "Recovery", "Recovery environment of another system" },
	{ "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", 0x4201, "ldm-meta",
	  "LDM metadata", "Logical disk manager metadata (dynamic disk)" },
	{ "af9b60a0-1431-4f62-bc68-3311714a69ad", 0x4200, "ldm-data",
	  "LDM data", "Logical disk manager data (dynamic disk)" },
	{ "e75caf8f-f680-4cee-afa3-b001e56efc2d", 0x4202, "storage-spaces",
	  "Storage Spaces", "Storage Spaces pool member" },
	{ "37affc90-ef7d-4e96-91c3-2d7ae055b174", 0x7501, "gpfs",
	  "IBM GPFS", "IBM General Parallel File System" },

	/* Apple. */
	{ "48465300-0000-11aa-aa11-00306543ecac", 0xaf00, "hfs", "Apple HFS+",
	  "Apple HFS or HFS+ file system" },
	{ "7c3457ef-0000-11aa-aa11-00306543ecac", 0xaf0a, "apfs", "Apple APFS",
	  "Apple file system container" },
	{ "55465300-0000-11aa-aa11-00306543ecac", 0xa800, "apple-ufs",
	  "Apple UFS", "Apple UFS file system" },
	{ "426f6f74-0000-11aa-aa11-00306543ecac", 0xab00, "apple-boot",
	  "Apple boot", "Apple boot (recovery HD)" },
	{ "52414944-0000-11aa-aa11-00306543ecac", 0xaf01, "apple-raid",
	  "Apple RAID", "Apple RAID member" },
	{ "4c616265-6c00-11aa-aa11-00306543ecac", 0xaf03, "apple-label",
	  "Apple label", "Apple volume label" },
	{ "53746f72-6167-11aa-aa11-00306543ecac", 0xaf05, "apple-core",
	  "Apple Core Storage", "Apple Core Storage volume" },

	/* The BSDs and others. */
	{ "516e7cb6-6ecf-11d6-8ff8-00022d09712b", 0xa503, "freebsd-ufs",
	  "FreeBSD UFS", "FreeBSD UFS file system" },
	{ "83bd6b9d-7f41-11dc-be0b-001560b84f0f", 0xa501, "freebsd-boot",
	  "FreeBSD boot", "FreeBSD boot code" },
	{ "516e7cb5-6ecf-11d6-8ff8-00022d09712b", 0xa502, "freebsd-swap",
	  "FreeBSD swap", "FreeBSD swap space" },
	{ "516e7cba-6ecf-11d6-8ff8-00022d09712b", 0xa504, "freebsd-zfs",
	  "FreeBSD ZFS", "FreeBSD ZFS pool member" },
	{ "516e7cb4-6ecf-11d6-8ff8-00022d09712b", 0xa500, "freebsd-data",
	  "FreeBSD disklabel", "FreeBSD disklabel" },
	{ "824cc7a0-36a8-11e3-890a-952519ad3f61", 0xa600, "openbsd",
	  "OpenBSD data", "OpenBSD disklabel" },
	{ "49f48d5a-b10e-11dc-b99b-0019d1879648", 0xa902, "netbsd-ffs",
	  "NetBSD FFS", "NetBSD FFS file system" },
	{ "49f48d32-b10e-11dc-b99b-0019d1879648", 0xa901, "netbsd-swap",
	  "NetBSD swap", "NetBSD swap space" },
	{ "6a898cc3-1dd2-11b2-99a6-080020736631", 0xbf01, "zfs",
	  "Solaris /usr or ZFS", "Solaris /usr or Apple ZFS pool member" },
	{ "6a82cb45-1dd2-11b2-99a6-080020736631", 0xbe00, "solaris-boot",
	  "Solaris boot", "Solaris boot partition" },
	{ "fe3a2a5d-4f32-41a7-b725-accc3285a309", 0x7f00, "chromeos-kernel",
	  "ChromeOS kernel", "ChromeOS kernel" },
	{ "3cb8e202-3b7e-47dd-8a3c-7ff2a13cfcec", 0x7f01, "chromeos-root",
	  "ChromeOS root", "ChromeOS root file system" },
	{ "2e0a753d-9e48-43b0-8337-b15192cb1b5e", 0x7f02, "chromeos-reserved",
	  "ChromeOS reserved", "ChromeOS reserved space" },
	{ "aa31e02a-400f-11db-9590-000c2911d1b8", 0xfb00, "vmfs",
	  "VMware VMFS", "VMware VMFS file system" },
	{ "9d275380-40ad-11db-bf97-000c2911d1b8", 0xfc00, "vmkcore",
	  "VMware vmkcore", "VMware crash dump space" },
	{ "024dee41-33e7-11d3-9d69-0008c781f39f", 0xef01, "mbr-scheme",
	  "MBR partition scheme", "A legacy MBR inside a partition" },
	{ "d3bfe2de-3daf-11df-ba40-e3a556d89593", 0x8400, "intel-rst",
	  "Intel Rapid Start", "Intel Rapid Start hibernation" },
	{ "f4019732-066e-4e12-8273-346c5641494f", 0x0000, "sony-boot",
	  "Sony system", "Sony boot partition" },
	{ "bfbfafe7-a34f-448a-9a5b-6213eb736c22", 0x0000, "lenovo-boot",
	  "Lenovo system", "Lenovo boot partition" },
};

#define NTYPES ((int)(sizeof(g_types) / sizeof(g_types[0])))

int gpt_type_count(void)
{
	return NTYPES;
}

const struct gpt_type_info *gpt_type_at(int i)
{
	if (i < 0 || i >= NTYPES)
		return NULL;
	return &g_types[i];
}

const struct gpt_type_info *gpt_type_lookup(const struct gpt_guid *type)
{
	char s[GPT_GUID_STR_LEN];

	if (!type)
		return NULL;
	gpt_guid_format(type, s);
	for (int i = 0; i < NTYPES; i++)
		if (strcmp(g_types[i].guid, s) == 0)
			return &g_types[i];
	return NULL;
}

const char *gpt_type_name(const struct gpt_guid *type)
{
	const struct gpt_type_info *ti = gpt_type_lookup(type);

	if (type && gpt_guid_is_zero(type))
		return "Unused";
	return ti ? ti->name : "Unknown";
}

static int gpt_parse_code(const char *s, unsigned *out)
{
	char *end;
	unsigned long v;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	if (strlen(s) != 4)
		return -1;
	for (int i = 0; i < 4; i++) {
		char c = s[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F')))
			return -1;
	}
	v = strtoul(s, &end, 16);
	if (*end)
		return -1;
	*out = (unsigned)v;
	return 0;
}

int gpt_type_parse(const char *s, struct gpt_guid *out)
{
	unsigned code;

	if (!s || !s[0] || !out)
		return -EINVAL;
	if (gpt_guid_parse(s, out) == 0)
		return 0;
	for (int i = 0; i < NTYPES; i++)
		if (g_types[i].alias && strcasecmp(g_types[i].alias, s) == 0)
			return gpt_guid_parse(g_types[i].guid, out);
	if (gpt_parse_code(s, &code) == 0 && code != 0) {
		for (int i = 0; i < NTYPES; i++)
			if (g_types[i].code == code)
				return gpt_guid_parse(g_types[i].guid, out);
	}
	/* The short names, as a last resort ("EFI system"). */
	for (int i = 0; i < NTYPES; i++)
		if (strcasecmp(g_types[i].name, s) == 0)
			return gpt_guid_parse(g_types[i].guid, out);
	return -ENOENT;
}
