/* LikeOS - FAT filesystem driver (FAT12, FAT16, FAT32 with long names)
 *
 * Copyright (C) 2026 The LikeOS Project
 *
 * The driver follows the FAT file system specification and the long file
 * name extension; it carries no code from any other implementation.  The
 * conventions it shares with the exFAT, NTFS and ISO9660 drivers (block ids,
 * page-cache integration, the write chokepoint, lazy arming, locking,
 * hostile media) are described at the top of exfat.h; this header repeats
 * what is specific to FAT.
 *
 * ===========================================================================
 * 1. Volumes
 * ===========================================================================
 *
 * The FAT type is the specification's: a boot sector whose 16-bit FAT size
 * is 0 describes FAT32; otherwise the cluster count decides between FAT12
 * (at most 4084 clusters) and FAT16 (`fat=12|16' overrides it).  FAT12 and
 * FAT16 keep their root directory in a fixed region between the FATs and
 * the data area; FAT32 keeps it in a cluster chain.  Sector sizes 512 to
 * 4096, clusters up to 256 KB, every FAT copy kept equal unless FAT32
 * mirroring is switched off (then only the active FAT is used).
 *
 * Mount: the boot sector is checked the way the specification defines its
 * fields (reserved sectors, FATs, media byte, powers of two, FAT big enough
 * for the clusters, regions inside the volume).  A FAT32 volume whose main
 * boot sector is unusable mounts read-only from the backup boot sector.  A
 * DOS 1.x floppy without a parameter block mounts with `dos1xfloppy'.
 *
 * ===========================================================================
 * 2. Write safety (exfat.h section 3, FAT specifics)
 * ===========================================================================
 *
 * One write chokepoint (fat_dev_write) and one discard chokepoint, each
 * bounded to the region the kind of write may touch: the boot sector (the
 * state byte), the FSInfo sector, the FAT copies, the fixed root directory,
 * the data area.  The page cache writes through a guard device whose write
 * is the chokepoint (data area only).
 *
 * Lazy arming: a read-write mount writes nothing until the first operation
 * that changes the volume really writes.  Arming marks the volume dirty --
 * the "clean shutdown" bit of FAT[1] cleared (FAT16, FAT32) and the dirty
 * bit of the boot sector's state byte set -- and syncs.  A clean sync,
 * unmount or read-only remount stores everything, writes the FSInfo free
 * count and next-free hint (FAT32), syncs, sets both marks back and syncs
 * again.  A volume that was dirty at mount stays marked dirty.
 *
 * Ordering without a journal (a stop at any point leaves at most lost
 * clusters, never a cluster two files own or a file naming a free one):
 *
 *   data of files that grew  ->  FAT entries of new clusters (zeroed
 *   directory clusters before their link)  ->  sync  ->  the links from
 *   the old last cluster of grown chains into the new clusters  ->  sync
 *   ->  directory entries  ->  sync  ->  end-of-chain marks of truncated
 *   chains  ->  sync  ->  FAT entries of freed clusters  ->  sync  ->
 *   discard (`discard' only).
 *
 * The one exception is a chain that grows: the link from its old last
 * cluster is stored before the entry with the larger size, so a stop in
 * between leaves a chain longer than its file (fsck trims the tail; no
 * data is lost and nothing is shared).  A rename that cannot be done in
 * place removes the old entry (and stores that) before it writes the new
 * one: a stop in between loses the name (the clusters are lost clusters),
 * never leaves two entries naming one chain.  A directory that moves to
 * another parent gets its new ".." stored in between, while nothing names
 * it, so no stop leaves a ".." pointing at the wrong parent.  A set of entries (long-name
 * entries and the short entry) is placed so that one device command stores
 * it whole.
 *
 * Growth of a file (a write past its end, a truncate upwards) stores zeros
 * for everything between the old end and the new data before the entry with
 * the new size: bytes past the end of a file are never shown.
 *
 * A directory entry that names a directory above it (an endless tree on a
 * damaged or hostile volume) is refused as corruption.
 *
 * Corruption found while mounted fails the operation with an I/O error and
 * reacts as `errors=' says (remount-ro by default: the mount latches
 * read-only, the volume stays marked dirty if it was armed).  An unlink of
 * a file whose cluster chain is corrupt removes the name and keeps the
 * clusters (lost, never freed: a corrupt chain may lead into other files)
 * before the error policy applies.
 *
 * ===========================================================================
 * 3. Names
 * ===========================================================================
 *
 * Long names: up to 255 UTF-16 code units in long-name entries (checksum
 * and sequence verified; orphans ignored), UTF-8 at the system interface
 * (`iocharset=utf8', the default; `iocharset=iso8859-1' and `ascii' with
 * `uni_xlate' escapes are supported), surrogate pairs for code points above
 * U+FFFF.  Trailing dots of a new name are dropped, a trailing space or one
 * of * ? < > | " : \ and control characters refuse it.
 *
 * Short names (8.3) in the OEM code page (`codepage=', default 437): a name
 * that is a valid short name gets no long-name entries when the
 * `shortname=' mode allows (win95 / mixed: upper case only; winnt: all
 * upper or all lower case per part, recorded in the case bits); every other
 * name gets a generated short name -- the base cut to six characters plus
 * "~1" .. "~9", then two characters, four hexadecimal digits of a name hash
 * and "~N" -- unique in its directory (`nonumtail' tries the plain form
 * first).  Short names are shown as stored (win95), lower case (lower) or
 * through the case bits (winnt, mixed: the default).
 *
 * Lookups are case-insensitive: names compare after folding through the
 * Unicode simple upper-case mapping, both the long name and the short name
 * of each entry are found, and the dcache keys are the folded UTF-8 names
 * (the *_cs interface).
 *
 * ===========================================================================
 * 4. Attributes, times, inode numbers
 * ===========================================================================
 *
 * The read-only attribute removes the write permission of a file (of a
 * directory too with `rodir'); chmod sets or clears it; other mode changes
 * and chown to a different owner fail with EPERM unless `quiet'.  The full
 * attribute byte (read-only 0x01, hidden 0x02, system 0x04, archive 0x20;
 * directory 0x10 read-only) is the extended attribute "system.fat_attrib":
 * a 32-bit little-endian value (one byte accepted on set).  It is not
 * listed by listxattr.
 *
 * Times are local time on disk (`tz=UTC' or `time_offset=MINUTES' say what
 * local means; the default is UTC): modification time with 2-second
 * resolution, creation time (reported as st_ctime) with 10 ms resolution,
 * access date.  Reading never updates the access date (no writes on idle
 * mounts).
 *
 * st_ino: 1 for the root directory, the first cluster for every object that
 * has one, else FAT_INO_POS(directory's first cluster, entry slot) -- stable
 * for as long as the object keeps its clusters and its place.
 *
 * ===========================================================================
 * 5. Mount options
 * ===========================================================================
 *
 *   uid= gid= umask= fmask= dmask= allow_utime=   ownership and modes
 *   quiet          chmod/chown that cannot be stored succeed silently
 *   showexec       execute bits only for .EXE .COM .BAT
 *   rodir          the read-only attribute also applies to directories
 *   sys_immutable  accepted (the system attribute is visible as an xattr)
 *   shortname=lower|win95|winnt|mixed     (default mixed)
 *   nonumtail      try a short name without "~N" first
 *   codepage=N     OEM code page of short names (437, 737, 775, 850, 852,
 *                  855, 857, 858, 860-866, 869, 874, 1250-1258)
 *   iocharset=utf8|iso8859-1|ascii   utf8 / utf8=1   uni_xlate
 *   errors=remount-ro|continue|panic
 *   discard        TRIM freed clusters once their FAT entries are stored
 *   flush          store a written file at its last close
 *   tz=UTC  time_offset=MINUTES
 *   usefree        trust the FSInfo free count instead of counting
 *   dos1xfloppy    mount DOS 1.x floppies without a parameter block
 *   fat=12|16|32   force the FAT type
 *   nfs= check= debug   accepted, no effect
 * Unknown options and the generic flags are accepted without effect.
 */

#ifndef LIKEOS_FAT32_H
#define LIKEOS_FAT32_H

#include <kernel/uapi/status.h>
#include <kernel/dev/block/block.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>

/* ---- Block ids (exfat.h section 1) ---------------------------------------- */
#define FAT_BID_TAG (1UL << 63)
#define FAT_BID_HANDLE_SHIFT 38
#define FAT_BID_HANDLE_BITS 24
#define FAT_BID_INDEX_MASK ((1UL << FAT_BID_HANDLE_SHIFT) - 1)
#define FAT_HANDLE_MAX ((1UL << FAT_BID_HANDLE_BITS) - 1)
#define FAT_BID_ENC(h, idx)                                                 \
	(FAT_BID_TAG |                                                      \
	 (((unsigned long)(h) & FAT_HANDLE_MAX) << FAT_BID_HANDLE_SHIFT) |  \
	 ((unsigned long)(idx) & FAT_BID_INDEX_MASK))
#define FAT_BID_HANDLE(b) \
	(((unsigned long)(b) >> FAT_BID_HANDLE_SHIFT) & FAT_HANDLE_MAX)
#define FAT_BID_INDEX(b) ((unsigned long)(b) & FAT_BID_INDEX_MASK)
#define FAT_BID_IS(b) \
	(((unsigned long)(b) & (FAT_BID_TAG | (1UL << 62))) == FAT_BID_TAG)
/* Every encoded id is below the marker (bit 62 is clear in all of them). */
#define FAT_EOC_MARKER 0xC000000000000000UL
#define FAT_BID_EOC 0xFFFFFFFFFFFFFFFFUL

/* Handles below this one are reserved (the FAT region does not go through
 * the page cache; nothing uses them).  The root directory is a dynamic
 * stream like every other. */
#define FAT_HANDLE_FIRST_DYNAMIC 16

/* st_ino of an object without clusters: bit 62 marks it (cluster numbers
 * are 28-bit), then its directory's first cluster and its entry slot. */
#define FAT_INO_POS(dir_cluster, entry_index)                                 \
	((1UL << 62) | (((unsigned long)(dir_cluster) & 0xFFFFFFFFUL) << 24) | \
	 ((unsigned long)(entry_index) & 0xFFFFFFUL))
#define FAT_ROOT_INO 1UL

/* Exposed block size never exceeds a page. */
#define FAT_MAX_EXPOSED_BLOCK 4096UL

/* ===========================================================================
 * On-disk format
 * ===========================================================================
 * Every multi-byte field is little-endian and read with byte loads. */

/* ---- Boot sector (BIOS parameter block) ---------------------------------- */
#define FAT_BS_JUMP 0 /* EB xx 90 or E9 xx xx */
#define FAT_BS_OEM 3 /* 8 bytes */
#define FAT_BS_BPS 11 /* u16 bytes per sector */
#define FAT_BS_SPC 13 /* u8 sectors per cluster */
#define FAT_BS_RESERVED 14 /* u16 reserved sectors */
#define FAT_BS_NFATS 16 /* u8 */
#define FAT_BS_ROOT_ENTRIES 17 /* u16 (0 on FAT32) */
#define FAT_BS_TOTAL16 19 /* u16 */
#define FAT_BS_MEDIA 21 /* u8 */
#define FAT_BS_FATSZ16 22 /* u16 (0 on FAT32) */
#define FAT_BS_SPT 24 /* u16 */
#define FAT_BS_HEADS 26 /* u16 */
#define FAT_BS_HIDDEN 28 /* u32 */
#define FAT_BS_TOTAL32 32 /* u32 */
/* FAT12 / FAT16 extended parameters */
#define FAT_BS16_DRIVE 36
#define FAT_BS16_STATE 37 /* state byte: bit 0 dirty */
#define FAT_BS16_SIG 38 /* 0x29: serial, label, type follow */
#define FAT_BS16_SERIAL 39
#define FAT_BS16_LABEL 43 /* 11 bytes */
#define FAT_BS16_FSTYPE 54 /* "FAT12   " / "FAT16   " */
/* FAT32 extended parameters */
#define FAT_BS32_FATSZ 36 /* u32 */
#define FAT_BS32_EXTFLAGS 40 /* u16: bits 0-3 active FAT, bit 7 no mirroring */
#define FAT_BS32_VERSION 42 /* u16, must be 0 */
#define FAT_BS32_ROOT 44 /* u32 root cluster */
#define FAT_BS32_FSINFO 48 /* u16 FSInfo sector */
#define FAT_BS32_BACKUP 50 /* u16 backup boot sector */
#define FAT_BS32_DRIVE 64
#define FAT_BS32_STATE 65 /* state byte: bit 0 dirty */
#define FAT_BS32_SIG 66 /* 0x29 */
#define FAT_BS32_SERIAL 67
#define FAT_BS32_LABEL 71 /* 11 bytes */
#define FAT_BS32_FSTYPE 82 /* "FAT32   " */
#define FAT_BS_SIGNATURE 510 /* 55 AA */

#define FAT_STATE_DIRTY 0x01u
#define FAT_EXT_SIG 0x29u
#define FAT_EXT_SIG_OLD 0x28u
#define FAT_EXTFLAGS_NOMIRROR 0x0080u
#define FAT_EXTFLAGS_ACTIVE 0x000Fu

/* ---- FSInfo sector (FAT32) ----------------------------------------------- */
#define FAT_FSI_LEAD_SIG 0 /* 0x41615252 */
#define FAT_FSI_STRUC_SIG 484 /* 0x61417272 */
#define FAT_FSI_FREE 488 /* u32, 0xFFFFFFFF unknown */
#define FAT_FSI_NEXT 492 /* u32, 0xFFFFFFFF unknown */
#define FAT_FSI_TRAIL_SIG 508 /* 0xAA550000 */
#define FAT_FSI_LEAD_VAL 0x41615252u
#define FAT_FSI_STRUC_VAL 0x61417272u
#define FAT_FSI_TRAIL_VAL 0xAA550000u
#define FAT_FSI_UNKNOWN 0xFFFFFFFFu

/* ---- FAT entries ------------------------------------------------------------ */
#define FAT_FIRST_CLUSTER 2u
#define FAT_ENT_FREE 0u
#define FAT12_BAD 0xFF7u
#define FAT12_EOC 0xFFFu
#define FAT12_EOC_MIN 0xFF8u
#define FAT12_MAX_CLUSTERS 4084u /* 0xFF4 */
#define FAT16_BAD 0xFFF7u
#define FAT16_EOC 0xFFFFu
#define FAT16_EOC_MIN 0xFFF8u
#define FAT16_MAX_CLUSTERS 65524u /* 0xFFF4 */
#define FAT32_MASK 0x0FFFFFFFu
#define FAT32_BAD 0x0FFFFFF7u
#define FAT32_EOC 0x0FFFFFFFu
#define FAT32_EOC_MIN 0x0FFFFFF8u
#define FAT32_MAX_CLUSTERS 0x0FFFFFF5u /* clusters 2 .. 0x0FFFFFF6 */
/* FAT[1] flags: set = clean shutdown / no disk error. */
#define FAT16_CLEAN_BIT 0x8000u
#define FAT16_HARDERR_BIT 0x4000u
#define FAT32_CLEAN_BIT 0x08000000u
#define FAT32_HARDERR_BIT 0x04000000u

/* ---- Directory entries (32 bytes) ----------------------------------------- */
#define FAT_DENTRY_SIZE 32u
#define FAT_DENTRY_SHIFT 5
#define FAT_DE_NAME 0 /* 11 bytes: base 8, extension 3 */
#define FAT_DE_ATTR 11
#define FAT_DE_NTRES 12 /* case bits */
#define FAT_DE_CTIME_CS 13 /* 10 ms units, 0..199 */
#define FAT_DE_CTIME 14 /* u16 */
#define FAT_DE_CDATE 16 /* u16 */
#define FAT_DE_ADATE 18 /* u16 */
#define FAT_DE_CLUS_HI 20 /* u16 (FAT32) */
#define FAT_DE_MTIME 22 /* u16 */
#define FAT_DE_MDATE 24 /* u16 */
#define FAT_DE_CLUS_LO 26 /* u16 */
#define FAT_DE_SIZE 28 /* u32 */

#define FAT_NAME_END 0x00u /* this and every later entry are free */
#define FAT_NAME_DELETED 0xE5u
#define FAT_NAME_KANJI_E5 0x05u /* a stored 0x05 stands for a leading 0xE5 */

/* Case bits of the NT reserved byte. */
#define FAT_CASE_LOWER_BASE 0x08u
#define FAT_CASE_LOWER_EXT 0x10u

/* Attributes. */
#define FAT_ATTR_RO 0x01u
#define FAT_ATTR_HIDDEN 0x02u
#define FAT_ATTR_SYSTEM 0x04u
#define FAT_ATTR_VOLUME 0x08u
#define FAT_ATTR_DIR 0x10u
#define FAT_ATTR_ARCHIVE 0x20u
#define FAT_ATTR_LFN 0x0Fu
#define FAT_ATTR_LFN_MASK 0x3Fu
/* Attributes a user may change (system.fat_attrib). */
#define FAT_ATTR_CHANGEABLE \
	(FAT_ATTR_RO | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_ARCHIVE)

/* Long-name entries. */
#define FAT_LFN_ORD 0 /* sequence 1..20, 0x40 on the last */
#define FAT_LFN_NAME1 1 /* 5 units */
#define FAT_LFN_ATTR 11 /* 0x0F */
#define FAT_LFN_TYPE 12 /* 0 */
#define FAT_LFN_CHKSUM 13
#define FAT_LFN_NAME2 14 /* 6 units */
#define FAT_LFN_CLUS 26 /* 0 */
#define FAT_LFN_NAME3 28 /* 2 units */
#define FAT_LFN_LAST 0x40u
#define FAT_LFN_ORD_MASK 0x1Fu
#define FAT_LFN_CHARS 13u
#define FAT_LFN_MAX_ENTRIES 20u

/* ---- Limits ------------------------------------------------------------------ */
#define FAT_NAME_MAX 255u /* UTF-16 code units */
#define FAT_SET_MAX_ENTRIES (FAT_LFN_MAX_ENTRIES + 1u)
#define FAT_SET_MAX_BYTES (FAT_SET_MAX_ENTRIES * FAT_DENTRY_SIZE)
#define FAT_MAX_DIR_ENTRIES 65536u
#define FAT_MAX_DIR_BYTES (FAT_MAX_DIR_ENTRIES * FAT_DENTRY_SIZE)
#define FAT_MAX_FILE_SIZE 0xFFFFFFFFULL
#define FAT_MAX_FATS 4u

/* statfs f_type. */
#define FAT_STATFS_MAGIC 0x4d44UL

/* ===========================================================================
 * Interface
 * =========================================================================== */

struct fat_fs;

/* A FAT volume brought up outside mount(2) -- the root filesystem of the
 * storage bootstrap.  The instance itself is private to the driver (fat_int.h). */
typedef struct fat32_fs {
	struct fat_fs *fs;
} fat32_fs_t;

/* Recognise the FAT volume at the start of `bdev' (a partition, or a disk
 * formatted without a partition table) and bring it up read-write with the
 * default options.  Touches no cache (the caches may not be initialised
 * yet) and writes nothing.  ST_OK, or an ST_ code with nothing allocated. */
int fat32_mount(const block_device_t *bdev, fat32_fs_t *out);
/* Free what fat32_mount() allocated (not `out' itself).  The caller has
 * dropped every cache entry of the instance first. */
void fat32_release(fat32_fs_t *fs);
/* Make the instance the root filesystem. */
int fat32_vfs_register_root(fat32_fs_t *fs);

/* mount(2) type "vfat" (also "fat32", "msdos", "fat", "fat12", "fat16"). */
extern const vfs_fstype_t fat32_fstype;

#endif /* LIKEOS_FAT32_H */
