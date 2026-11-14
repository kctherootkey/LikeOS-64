/* libgpt -- GUID partition tables and master boot records.
 *
 * A small, self-contained library that reads, checks, repairs, edits and
 * writes GUID partition tables (GPT) and their protective master boot
 * record, and reads legacy MBR tables including the chain of logical
 * partitions in an extended partition.  It works on a block device node or
 * on a plain image file alike, so every part of it can be tested on the
 * build host against image files.
 *
 * Layers:
 *   crc32.c   the IEEE 802.3 CRC-32 the GPT headers and entry arrays carry
 *   guid.c    GUIDs: random (version 4), text form, on-disk byte order
 *   blkio.c   open a device or an image, sector size, size, sector I/O
 *   mbr.c     protective and legacy MBR, CHS values, extended chains
 *   gpt.c     primary + backup copies: read, validate, repair, write
 *   layout.c  partitions: add, delete, resize, names, attributes, free space
 *   types.c   the table of known partition type GUIDs
 *
 * Conventions: functions returning int give 0 (or a count) on success and a
 * negative errno value on failure.  Table edits leave a sentence for a person
 * in gpt_table.errmsg when they fail.  Partition numbers are 1-based (the
 * index in the entry array plus one), as every tool shows them.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_GPT_H
#define LIKEOS_GPT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPT_LIB_VERSION "1.0"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define GPT_SIGNATURE          0x5452415020494645ULL /* "EFI PART" */
#define GPT_REVISION_1_0       0x00010000u
#define GPT_HEADER_SIZE        92u
#define GPT_ENTRY_SIZE         128u     /* what this library writes */
#define GPT_DEFAULT_ENTRIES    128u     /* what this library writes */
#define GPT_MAX_ENTRIES        1024u    /* the most it reads */
#define GPT_NAME_UNITS         36       /* UTF-16 code units in a name */
#define GPT_NAME_UTF8_MAX      (GPT_NAME_UNITS * 3 + 1)
#define GPT_ALIGN_BYTES        (1024ULL * 1024ULL) /* 1 MiB */

/* Attribute bits of a partition entry. */
#define GPT_ATTR_REQUIRED      (1ULL << 0)  /* platform required */
#define GPT_ATTR_NO_BLOCK_IO   (1ULL << 1)  /* firmware ignores it */
#define GPT_ATTR_LEGACY_BOOT   (1ULL << 2)  /* legacy BIOS bootable */
#define GPT_ATTR_READ_ONLY     (1ULL << 60) /* basic data: read-only */
#define GPT_ATTR_SHADOW_COPY   (1ULL << 61)
#define GPT_ATTR_HIDDEN        (1ULL << 62)
#define GPT_ATTR_NO_AUTOMOUNT  (1ULL << 63)

/* MBR partition type bytes the library knows by name. */
#define MBR_TYPE_EMPTY         0x00
#define MBR_TYPE_FAT12         0x01
#define MBR_TYPE_FAT16_SMALL   0x04
#define MBR_TYPE_EXTENDED_CHS  0x05
#define MBR_TYPE_FAT16         0x06
#define MBR_TYPE_NTFS          0x07     /* also exFAT, HPFS */
#define MBR_TYPE_FAT32_CHS     0x0b
#define MBR_TYPE_FAT32_LBA     0x0c
#define MBR_TYPE_FAT16_LBA     0x0e
#define MBR_TYPE_EXTENDED_LBA  0x0f
#define MBR_TYPE_DATA          0x83     /* generic data */
#define MBR_TYPE_SWAP          0x82
#define MBR_TYPE_EXTENDED_ALT  0x85
#define MBR_TYPE_LVM           0x8e
#define MBR_TYPE_RAID          0xfd
#define MBR_TYPE_GPT_PROTECTIVE 0xee
#define MBR_TYPE_ESP           0xef

#define MBR_MAX_PARTS          64       /* 4 primary + up to 60 logical */

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320)               */
/* ------------------------------------------------------------------ */

/* Continues a CRC: pass 0 to start.  gpt_crc32(0, "123456789", 9) is
 * 0xCBF43926. */
uint32_t gpt_crc32(uint32_t crc, const void *buf, size_t len);

/* ------------------------------------------------------------------ */
/* GUIDs                                                               */
/* ------------------------------------------------------------------ */

/* A GUID in canonical order: b[0] is the first byte of the text form
 * "b0b1b2b3-b4b5-b6b7-b8b9-b10..b15".  The on-disk form stores the first
 * three groups little-endian; gpt_guid_to_disk/from_disk convert. */
struct gpt_guid {
	uint8_t b[16];
};

/* 36 characters plus the terminator. */
#define GPT_GUID_STR_LEN 37

void gpt_guid_to_disk(const struct gpt_guid *g, uint8_t out[16]);
void gpt_guid_from_disk(const uint8_t in[16], struct gpt_guid *g);
/* Accepts upper or lower case, with or without surrounding braces. */
int gpt_guid_parse(const char *s, struct gpt_guid *out);
/* Lower case, canonical. */
void gpt_guid_format(const struct gpt_guid *g, char out[GPT_GUID_STR_LEN]);
void gpt_guid_format_upper(const struct gpt_guid *g, char out[GPT_GUID_STR_LEN]);
/* A random version 4 GUID (getrandom(), /dev/urandom as a fallback). */
int gpt_guid_random(struct gpt_guid *out);
int gpt_guid_is_zero(const struct gpt_guid *g);
int gpt_guid_equal(const struct gpt_guid *a, const struct gpt_guid *b);
/* Fills buf with n random bytes; 0 or -errno. */
int gpt_random_bytes(void *buf, size_t n);

/* ------------------------------------------------------------------ */
/* Block I/O                                                           */
/* ------------------------------------------------------------------ */

enum gpt_open_flags {
	GPT_OPEN_RDONLY = 0,
	GPT_OPEN_RDWR   = 1 << 0,
	GPT_OPEN_EXCL   = 1 << 1,   /* O_EXCL on a block device: refused while
	                             * any partition of it is in use */
};

struct gpt_dev {
	int fd;
	char path[256];
	int is_file;                 /* a regular (image) file */
	int writable;
	uint32_t sector_size;        /* logical: BLKSSZGET, 512 for a file */
	uint32_t phys_sector_size;   /* BLKPBSZGET, the logical size otherwise */
	uint64_t size_bytes;         /* BLKGETSIZE64, or the file's size */
	uint64_t nsectors;           /* size_bytes / sector_size */
};

/* sector_size 0 = ask the device (512 for image files); 512 or 4096 force
 * a size (image files only; a device keeps its own). */
int gpt_dev_open(struct gpt_dev *d, const char *path, int flags,
		 uint32_t sector_size);
void gpt_dev_close(struct gpt_dev *d);
int gpt_dev_read(struct gpt_dev *d, uint64_t lba, uint64_t count, void *buf);
int gpt_dev_write(struct gpt_dev *d, uint64_t lba, uint64_t count,
		  const void *buf);
/* Byte-addressed I/O (whole sectors are read/modified/written as needed). */
int gpt_dev_pread(struct gpt_dev *d, uint64_t off, size_t len, void *buf);
int gpt_dev_pwrite(struct gpt_dev *d, uint64_t off, size_t len,
		   const void *buf);
/* Writes zeroes over [off, off + len). */
int gpt_dev_zero(struct gpt_dev *d, uint64_t off, uint64_t len);
int gpt_dev_sync(struct gpt_dev *d);
/* Asks the kernel to re-read the table (BLKRRPART); a no-op for files.
 * Retries up to `tries' times on EBUSY, sleeping delay_ms in between. */
int gpt_dev_reread(struct gpt_dev *d, int tries, int delay_ms);

/* ------------------------------------------------------------------ */
/* MBR                                                                 */
/* ------------------------------------------------------------------ */

struct mbr_part {
	int number;              /* 1..4 primary, 5.. logical */
	uint8_t status;          /* 0x80 = active */
	uint8_t type;
	uint8_t chs_first[3];
	uint8_t chs_last[3];
	uint64_t first_lba;      /* absolute */
	uint64_t nsectors;
	int is_extended;         /* a container of logical partitions */
	int is_logical;
};

enum mbr_kind {
	MBR_KIND_NONE = 0,       /* no 0x55aa signature */
	MBR_KIND_EMPTY,          /* signature, but no partitions */
	MBR_KIND_PROTECTIVE,     /* one 0xee entry covering the disk */
	MBR_KIND_HYBRID,         /* 0xee plus other entries */
	MBR_KIND_LEGACY,         /* an ordinary MBR partition table */
};

struct mbr_table {
	int kind;                /* enum mbr_kind */
	uint32_t disk_signature; /* bytes 440..443 */
	uint16_t reserved;       /* bytes 444..445 */
	uint8_t boot_code[440];
	int nparts;
	struct mbr_part parts[MBR_MAX_PARTS];
	int chain_broken;        /* the logical chain had a loop or bad link */
};

int mbr_read(struct gpt_dev *d, struct mbr_table *out);
/* Writes the four primary entries of t (logical partitions are not
 * written), keeping the boot code of t. */
int mbr_write(struct gpt_dev *d, const struct mbr_table *t);
/* A protective MBR: one 0xee entry from LBA 1 to the end (capped at
 * 0xffffffff), keeping the disk signature and boot code already there
 * when keep_boot_code is set. */
int mbr_write_protective(struct gpt_dev *d, int keep_boot_code);
/* CHS of an LBA in the usual 255 heads, 63 sectors geometry; LBAs beyond
 * what CHS can address give 1023/254/63. */
void mbr_lba_to_chs(uint64_t lba, uint8_t chs[3]);
const char *mbr_type_name(uint8_t type);
const char *mbr_kind_name(int kind);

/* ------------------------------------------------------------------ */
/* GPT                                                                 */
/* ------------------------------------------------------------------ */

/* The 92-byte on-disk header (little-endian). */
struct gpt_header_raw {
	uint64_t signature;
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint8_t disk_guid[16];
	uint64_t entries_lba;
	uint32_t num_entries;
	uint32_t entry_size;
	uint32_t entries_crc32;
} __attribute__((packed));

/* A 128-byte on-disk entry. */
struct gpt_entry_raw {
	uint8_t type_guid[16];
	uint8_t part_guid[16];
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t attributes;
	uint16_t name[GPT_NAME_UNITS];
} __attribute__((packed));

/* One slot of the entry array, decoded. */
struct gpt_part {
	int used;                     /* type GUID is not zero */
	struct gpt_guid type;
	struct gpt_guid uuid;         /* unique partition GUID (PARTUUID) */
	uint64_t first_lba;
	uint64_t last_lba;            /* inclusive */
	uint64_t attributes;
	uint16_t name_raw[GPT_NAME_UNITS];
	char name[GPT_NAME_UTF8_MAX]; /* UTF-8 form of name_raw */
};

/* What gpt_read found, copy by copy. */
struct gpt_status {
	int pmbr_kind;                /* enum mbr_kind of LBA 0 */
	int primary_header_ok;        /* signature, size, CRC, my_lba */
	int primary_entries_ok;       /* entry array CRC */
	int backup_header_ok;
	int backup_entries_ok;
	int backup_not_at_end;        /* the disk grew, or the backup moved */
	int headers_differ;           /* both valid, but describe different
	                               * tables (usable range, GUID, count) */
	int using_backup;             /* the table came from the backup */
	char primary_problem[96];
	char backup_problem[96];
};

struct gpt_table {
	uint32_t sector_size;
	uint64_t nsectors;            /* disk size in sectors */
	struct gpt_guid disk_guid;
	uint64_t primary_lba;         /* 1 */
	uint64_t backup_lba;          /* the last sector */
	uint64_t primary_entries_lba; /* 2 */
	uint64_t backup_entries_lba;  /* backup_lba - entry sectors */
	uint64_t first_usable;
	uint64_t last_usable;
	uint32_t num_entries;
	uint32_t entry_size;
	uint32_t revision;
	struct gpt_part *parts;       /* num_entries slots (heap) */
	struct gpt_status status;     /* filled by gpt_read */
	char errmsg[160];             /* why the last edit failed */
};

/* An empty table for a disk of nsectors sectors: random disk GUID,
 * 128 entries, the usual reserved areas.  t must be freed with
 * gpt_table_free(). */
int gpt_table_new(struct gpt_table *t, uint32_t sector_size,
		  uint64_t nsectors);
/* A deep copy (dst must not be initialised). */
int gpt_table_copy(struct gpt_table *dst, const struct gpt_table *src);
void gpt_table_free(struct gpt_table *t);

/* Reads the table: the primary copy when it is valid, else the backup.
 * 0 on success (t must then be freed), -ENOENT when neither copy has a GPT
 * signature (a blank or MBR disk), -EBADMSG when both are damaged; the
 * gpt_status of t says what was found either way when t was filled. */
int gpt_read(struct gpt_dev *d, struct gpt_table *t);
/* Only the header and entries at a given LBA (my_lba as on disk);
 * for tools that show both copies. */
int gpt_read_header(struct gpt_dev *d, uint64_t lba,
		    struct gpt_header_raw *hdr, int *crc_ok);

/* Writes the protective MBR, the primary header and entry array, the
 * backup entry array and header (backup moved to the end of the disk),
 * with fresh CRCs, then syncs. */
int gpt_write(struct gpt_dev *d, struct gpt_table *t);
/* Rewrites both copies from the good one; also moves a misplaced backup
 * to the end of the disk and widens last_usable accordingly. */
int gpt_repair(struct gpt_dev *d, struct gpt_table *t);
/* Zeroes LBA 0, the primary header + array, the backup array + header. */
int gpt_wipe(struct gpt_dev *d);

/* A problem gpt_verify found. */
typedef void (*gpt_report_fn)(int severity, const char *msg, void *user);
#define GPT_SEV_INFO    0
#define GPT_SEV_WARNING 1
#define GPT_SEV_ERROR   2

/* Checks a table: both copies, CRCs, usable range, partitions inside it,
 * overlaps, first > last, duplicate unique GUIDs, alignment (warning),
 * protective MBR.  Returns the number of errors (warnings not counted). */
int gpt_verify(struct gpt_dev *d, const struct gpt_table *t,
	       gpt_report_fn report, void *user);

/* ------------------------------------------------------------------ */
/* Layout editing (in memory; gpt_write puts it on disk)               */
/* ------------------------------------------------------------------ */

/* The 1 MiB alignment in sectors. */
uint64_t gpt_align_sectors(const struct gpt_table *t);
uint64_t gpt_align_up(uint64_t lba, uint64_t align);
uint64_t gpt_align_down(uint64_t lba, uint64_t align);

struct gpt_free {
	uint64_t first_lba;   /* aligned when the region list is aligned */
	uint64_t last_lba;    /* inclusive */
};

/* Free space between and around the partitions, ascending.  With
 * aligned set every region starts on the alignment and regions smaller
 * than one alignment unit are left out.  Returns the count (at most max)
 * or -errno. */
int gpt_free_regions(const struct gpt_table *t, struct gpt_free *out,
		     int max, int aligned);

struct gpt_part_spec {
	int partno;               /* 1..num_entries, 0 = the first free slot */
	uint64_t first_lba;       /* 0 = start of the first free region that
	                           * holds size_lba (aligned) */
	uint64_t size_lba;        /* 0 = up to the end of that free region */
	struct gpt_guid type;     /* zero = generic data */
	struct gpt_guid uuid;     /* zero = random */
	uint64_t attributes;
	const char *name;         /* UTF-8, NULL = none */
};

/* Adds a partition; *partno_out gets its number.  The start is aligned up
 * to 1 MiB when first_lba is 0; an explicit first_lba is taken as given
 * (it must lie in free space). */
int gpt_part_add(struct gpt_table *t, const struct gpt_part_spec *spec,
		 int *partno_out);
int gpt_part_delete(struct gpt_table *t, int partno);
/* Moves the end of a partition; new_size_lba 0 = as far as the free space
 * after it reaches. */
int gpt_part_resize(struct gpt_table *t, int partno, uint64_t new_size_lba);
/* The largest size the partition can grow to without moving. */
uint64_t gpt_part_max_size(const struct gpt_table *t, int partno);
int gpt_part_set_name(struct gpt_table *t, int partno, const char *utf8);
int gpt_part_set_type(struct gpt_table *t, int partno,
		      const struct gpt_guid *type);
int gpt_part_set_uuid(struct gpt_table *t, int partno,
		      const struct gpt_guid *uuid);
int gpt_part_set_attributes(struct gpt_table *t, int partno, uint64_t attrs);
/* NULL when partno is out of range or the slot is empty. */
const struct gpt_part *gpt_part_get(const struct gpt_table *t, int partno);
int gpt_part_count(const struct gpt_table *t);
/* The first empty slot (1-based), or -ENOSPC. */
int gpt_part_first_free_slot(const struct gpt_table *t);
/* The partition overlapping [first, last], ignoring partno `ignore'
 * (0 = none); 0 when the range is free. */
int gpt_overlap(const struct gpt_table *t, uint64_t first, uint64_t last,
		int ignore);
/* Sorted list of the used partition numbers, ascending by first LBA. */
int gpt_part_order(const struct gpt_table *t, int *out, int max);

/* UTF-16LE (36 units, zero padded) <-> UTF-8.  utf8_to_name returns
 * -ENAMETOOLONG when the name does not fit, -EILSEQ on bad UTF-8. */
int gpt_name_to_utf8(const uint16_t *name, size_t units, char *out,
		     size_t outlen);
int gpt_name_from_utf8(const char *utf8, uint16_t name[GPT_NAME_UNITS]);

/* ------------------------------------------------------------------ */
/* Partition types                                                     */
/* ------------------------------------------------------------------ */

struct gpt_type_info {
	const char *guid;         /* canonical lower-case text */
	uint16_t code;            /* the customary 4-digit hex code, ef00 */
	const char *alias;        /* short name for command lines: "esp" */
	const char *name;         /* "EFI system" */
	const char *desc;         /* a sentence */
};

int gpt_type_count(void);
const struct gpt_type_info *gpt_type_at(int i);
/* NULL for an unknown GUID. */
const struct gpt_type_info *gpt_type_lookup(const struct gpt_guid *type);
/* "esp", "ef00", "EF00", "0xef00", or a GUID -> GUID; -ENOENT if unknown. */
int gpt_type_parse(const char *s, struct gpt_guid *out);
/* "EFI system", or "Unknown" for a GUID not in the table. */
const char *gpt_type_name(const struct gpt_guid *type);

#define GPT_TYPE_ESP          "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define GPT_TYPE_LIKEOS_ROOT  "4f68bce3-e8cd-4db1-96e7-fbcaf984b709"
#define GPT_TYPE_GENERIC_DATA "0fc63daf-8483-4772-8e79-3d69d8477de4"
#define GPT_TYPE_BASIC_DATA   "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
#define GPT_TYPE_MSR          "e3c9e316-0b5c-4db8-817d-f92df00215ae"
#define GPT_TYPE_SWAP         "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"
#define GPT_TYPE_BIOS_BOOT    "21686148-6449-6e6f-744e-656564454649"

#ifdef __cplusplus
}
#endif

#endif /* LIKEOS_GPT_H */
