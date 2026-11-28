/* LikeOS - NTFS filesystem driver (read-write)
 *
 * NTFS 1.2 and 3.x volumes: MFT with record cache, fixups, attribute lists,
 * resident and non-resident attributes, B+tree directories ($I30), sparse and
 * LZNT1-compressed files, alternate data streams, reparse points, $Secure,
 * extended attributes, $LogFile handling and delayed allocation.
 *
 * The driver is the files of kernel/fs/ntfs/ (super.c, inode.c, mft.c,
 * attrib.c, index.c, file.c, namei.c, logfile.c, ...; ntfs_int.h maps
 * their sections).  The driver-internal section at the end of this header
 * and ntfs_int.h are shared by all of them.
 *
 * Copyright (C) 2026 The LikeOS Project
 *
 * ===========================================================================
 * Shared conventions of the NTFS, exFAT and ISO9660 drivers
 * (the same text heads ntfs.h, exfat.h and isofs.h; a change goes into all
 * three).  xx_ / XX_ stand for each driver's prefix: ntfs_ / NTFS_,
 * exfat_ / EXFAT_, isofs_ / ISOFS_.
 * ===========================================================================
 *
 * 1. Page cache integration (ext4 is the model)
 * ---------------------------------------------
 * Exposed block size (vfs_sb_ops_t.block_size): NTFS and exFAT expose 4096
 * when the cluster is 4 KB or larger -- a 64 KB cluster is sixteen exposed
 * blocks -- else the cluster size (512, 1024, 2048); ISO9660 exposes its
 * logical block size (2048 in practice).  The mount is refused when the
 * device's sector size does not divide 4096 or exceeds the exposed block
 * size.
 *
 * Block ids are POSITIONAL, through a per-mount stream-handle table:
 *
 *     bit  63      tag, always 1                      XX_BID_TAG
 *     bit  62      always 0 (every id stays below the end-of-chain marker)
 *     bits 61..38  stream handle h, 24 bits, 1..XX_HANDLE_MAX (0 invalid)
 *     bits 37..0   block index within the stream, in exposed blocks
 *
 * The chain id of stream h is XX_BID_ENC(h, 0) (always >= 2).  It is the
 * page-cache key (pc_page_t.cluster_id, the `start_cluster' passed to
 * pagecache_get*), the inode-cache key (ic_inode_t.start_cluster) and the
 * dcache parent key of a directory stream.  end_of_chain_marker() returns
 * XX_EOC_MARKER; next_block() returns XX_BID_EOC at the end.
 *
 * A stream is one byte sequence on disk.  NTFS: (MFT reference, attribute
 * type, attribute name) -- the unnamed $DATA, every alternate data stream,
 * every directory's $I30 index allocation; exFAT: every file and directory;
 * ISO9660: every file and directory -- plus reserved handles (below
 * XX_HANDLE_FIRST_DYNAMIC, defined per driver) for the volume's own
 * metadata streams.  A handle is allocated when the stream
 * is first opened and freed when its inode-cache entry dies; it is reused
 * only after pagecache_invalidate_file() of its chain id has run, so no
 * cached page of a dead stream can be found under a new one.
 *
 * The block-mapping ops are pure memory lookups over the mapping decoded
 * when the stream is opened (NTFS: the runlist, an array searched by binary
 * search; exFAT: an extent array -- contiguous NoFatChain files or the FAT
 * chain compacted into extents; ISO9660: the extent array of a multi-extent
 * file).  No I/O, no sleeping lock, inside them:
 *
 *   block_at(chain, N)       XX_BID_ENC(h, N) for N below the number of
 *                            exposed blocks covering the stream's data size,
 *                            holes included; 0 past it, and 0 for every
 *                            block of a NOFILL page (section 2).
 *   next_block(ENC(h, N))    block_at(chain, N + 1), or XX_BID_EOC.  The
 *                            cache's per-block read walks with it, so it
 *                            must agree with block_at exactly.
 *   block_to_lba(ENC(h, N))  the absolute device LBA (partition offset
 *                            included) of block N; 0 for a hole, a sparse
 *                            run, a block not placed yet (delayed
 *                            allocation) and a block wholly beyond the
 *                            initialized size / ValidDataLength -- the
 *                            cache reads 0 as zeros and never writes it.
 *   map_block(chain, N)      XX_BID_ENC(h, N) when block N has a disk
 *                            location, else 0.
 *
 * Reads of a mapped block whose bytes straddle the initialized size are not
 * a plain copy (the tail must read as zeros): such a page is NOFILL.
 *
 * Blocks smaller than a page: the cache coalesces a page into one transfer
 * only when the LBAs of its blocks are consecutive (or all 0); otherwise it
 * reads block by block (pagecache.c, coalesced read).
 *
 * Delayed allocation (NTFS): write() reserves clusters (exact reservation
 * accounting, so ENOSPC is reported by write(), never later) and dirties the
 * page; the blocks keep mapping to LBA 0 until writeback calls
 * vfs_sb_ops_t.alloc_blocks() (contract in vfs_sb.h): it places the reserved
 * blocks -- the whole reserved extent where it can, for contiguity -- with
 * the metadata lock held exclusive (lock_io), records any failure on the
 * stream for fsync()/close() to report, and returns 0, VFS_SB_ALLOC_NONE or
 * an ST_ error (the pages then stay dirty).  It must not take a stream lock.
 * exFAT and ISO9660 leave alloc_blocks NULL (exFAT allocates in write()).
 *
 * 2. NOFILL pages: data that is not a block copy of the device
 * ------------------------------------------------------------
 * LZNT1-compressed compression units, resident $DATA (inline in the MFT
 * record), zisofs-compressed ISO files, and the page that straddles the
 * initialized size / ValidDataLength.  block_at() answers 0 for every block
 * of such a page, so pagecache_get*() would not find it on the device (it
 * returns NULL) and readahead / the coalesced read stop at it.  The driver
 * fills such a page itself:
 *
 *   read   pg = pagecache_lookup_held(sb, chain, idx);
 *          on a miss: decode the page into a bounce buffer (never a stack
 *          buffer > 512 bytes), then
 *          pg = pagecache_install(sb, chain, idx, buf, len, 1);
 *          (the cached copy wins a race; bytes past `len' read as zeros)
 *          copy out of pg->data, pagecache_put(pg).
 *   write  write-through: under the stream lock exclusive the driver writes
 *          the new on-disk form itself (compressed unit rewritten
 *          uncompressed, the MFT record holding resident data), then
 *          updates the cached page in place (lookup_held or install, copy,
 *          pagecache_put) WITHOUT marking it dirty.  A NOFILL page is never
 *          dirty.  A write into the initialized-size straddle page first
 *          advances the initialized size (zeroing the gap), after which the
 *          page is an ordinary one.
 *
 * 3. Write safety
 * ---------------
 * - ONE write chokepoint per driver, xx_dev_write(fs, lba, count, buf)
 *   and xx_dev_discard(fs, lba, count): it refuses with ST_ROFS (and
 *   WARN_RATELIMIT) unless the mount is armed read-write and not latched
 *   read-only, and with ST_INVALID unless [lba, lba + count) lies inside the
 *   volume.  No other bdev->write, blk_discard* or device write anywhere in
 *   the driver.
 * - The page cache never sees the real device: vfs_sb_ops_t.bdev() returns
 *   a GUARD block_device_t embedded in the mount -- name, sector_size and
 *   total_sectors copied from the real one, .read forwarding to it, .write
 *   calling the chokepoint (the mount found from the guard's address with
 *   offsetof), .sync forwarding, every other member NULL.
 * - Lazy arming: a read-write mount writes NOTHING until the first mutating
 *   operation.  That one (under the metadata lock exclusive) arms the mount:
 *   NTFS resets $LogFile to clean and sets VOLUME_IS_DIRTY in $Volume, exFAT
 *   sets VolumeDirty, then the device is synced.  No atime updates, ever.
 *   A clean sync / unmount flushes data and metadata, syncs the device,
 *   clears the dirty flag, syncs again and disarms.  So mount, browse and
 *   unmount leave the volume byte-identical, in read-only and read-write
 *   mounts alike.
 * - Read-only gates (silent degrade to read-only with ONE log line):
 *   NTFS: hibernated / Fast Startup (hiberfil.sys "hibr"/"wake"), volume
 *   dirty without `force', $LogFile unclean and not replayable, $MFTMirr
 *   mismatch, unsupported version, bitmap / cluster-count mismatch.
 *   exFAT: backup boot region only, revision not 1.x, two FATs (TexFAT),
 *   VolumeDirty without `force'.  Both: a read-only device.  ISO9660 is
 *   read-only by definition (mount() forces VFS_MS_RDONLY behaviour).
 * - Ordering without a journal: allocate before reference, dereference
 *   before free (bitmap -> data -> MFT record (+ mirror) -> index; reverse
 *   on delete).  Discard only with the `discard' option and only after the
 *   cleared bitmap is on the device.  Corruption detected while read-write
 *   is handled per `errors=' (default remount-ro: the mount is latched
 *   read-only; a failed device write latches it in every case).
 *
 * 4. Locking (ext4's hierarchy, ext4/lock.c "Fine-grained locking engine")
 * -------------------------------------------------------------------
 *   per-stream rwsem  ->  metadata rwsem  ->  allocator mutex
 *       ->  cache spinlocks (record / index caches, handle table, the
 *           dcache / icache / pagecache)  ->  block device
 *
 * - The metadata rwsem is a private recursive rwsem like ext4's: its
 *   exclusive owner may take it again, exclusive or shared.
 *   vfs_sb_ops_t.lock_map / unlock_map = metadata SHARED, lock_io /
 *   unlock_io = metadata EXCLUSIVE.  The page cache maps under lock_map and
 *   transfers data with no filesystem lock: data-block lifetime is fenced by
 *   the per-stream rwsem, held SHARED by readers across their page reads and
 *   EXCLUSIVE by write / truncate / unlink / rename-over / O_TRUNC.
 * - Mutators take lockdep_assert_held()-checked locks; might_sleep() in
 *   every I/O helper.  No kfree, no I/O, no sleeping under a spinlock.
 * - vfs_ops_t.release_locks_for_task is implemented: a task killed inside
 *   the driver gives up its holds.
 * - pagecache_balance_dirty() is called from write() with lock_io held (so
 *   writeback of other streams, and alloc_blocks, may run nested in it).
 *
 * 5. Hostile media
 * ----------------
 * Every on-disk offset / length is overflow-checked before use; allocations
 * are capped; no recursion (explicit stacks, depth <= 32); no stack buffer
 * larger than 512 bytes.  A corrupt object yields ST_IO + WARN_RATELIMIT,
 * never a crash or a hang.  BUG_ON only for internal invariants, never for
 * disk data.
 *
 * 6. Probing
 * ----------
 * fstype.probe() reads only the boot sector / volume descriptors, bounded,
 * and recognises the signature.  While a driver is incomplete its
 * XX_DRIVER_READY define (top of xx.c) is 0: probe() answers 0 (so "auto"
 * never picks a half driver) and mount() returns ST_UNSUPPORTED.  It is 1
 * once mount works.
 *
 * ===========================================================================
 * NTFS specifics
 * ===========================================================================
 *
 * Inode numbers (st_ino): the MFT record number (48 bits of the file
 * reference; the root directory is 5).  An alternate data stream opened as
 * "name:stream" reports its file's number.  st_nlink counts the file's
 * $FILE_NAME attributes, a WIN32 + DOS pair (or a lone DOS alias) once;
 * directories report 1, as on any filesystem with no "." entries.
 *
 * Streams and handles: every stream (unnamed $DATA, each alternate data
 * stream, each directory's $I30 index allocation -- also for a small
 * directory without one, so that its chain id can key the dcache) gets a
 * handle when first opened; unreferenced streams stay cached (LRU) and are
 * evicted by invalidating their pages, inode-cache entry and dentries
 * before the handle is reused.  The reserved handles are pinned.
 *
 * Names: case-sensitive lookups; the $UpCase collation (loaded at mount)
 * orders the $I30 B+tree, and with `nocase' lookups fold through it (dcache
 * *_cs API, keys folded through $UpCase).  The DOS (8.3) name of a file with
 * a separate Win32 name is never listed.  UTF-16 <-> UTF-8 conversion lives
 * in unistr.c; unpaired surrogates are kept round-trippable.
 *
 * Mount options (comma separated, vfs_next_option()):
 *   uid=, gid=          owner of every file (default 0, 0).  Given
 *                       explicitly they also override the per-file WSL
 *                       owner extended attributes ($LXUID / $LXGID).
 *   umask=, fmask=, dmask=   permission masks (default fmask 0177, dmask
 *                       0077: files rw-------, directories rwx------, the
 *                       feature model's defaults).  umask= sets both.  A
 *                       file with a WSL $LXMOD attribute reports that mode
 *                       verbatim; the masks apply to the others only.
 *   force               mount read-write although $Volume is marked dirty /
 *                       modified by chkdsk (never overrides hibernation, an
 *                       error found while mounting or an unreplayable log)
 *   recover             replay an unclean $LogFile (all or nothing; any
 *                       doubt keeps the volume read-only, nothing written).
 *                       Without it a volume whose only problem is an
 *                       unclean journal mounts read-only ("unclean journal;
 *                       mount with -o recover to replay it, or run
 *                       chkdsk").  Only at mount time: a remount read-write
 *                       of such a volume is refused (unmount and mount with
 *                       recover).  Never passed by automatic mounts.
 *   discard             TRIM freed clusters
 *   acl                 POSIX ACLs (stored as extended attributes named
 *                       system.posix_acl_access / _default)
 *   nocase              case-insensitive lookups (case_sensitive=0 too)
 *   sparse              create new files sparse (disable_sparse turns it
 *                       off)
 *   show_sys_files / showmeta   list the system files ($Bitmap, ...); $MFT
 *                       itself is never listed.  They are always reachable
 *                       by name.
 *   hide_dot_files      do not list names starting with '.'
 *   nohidden            do not list entries with the HIDDEN attribute
 *   sys_immutable       system files are immutable (records below 16 are
 *                       always immutable, except directories)
 *   native_symlink=raw|rel   absolute symbolic links / junctions: raw
 *                       (default) shows the target as stored ("\??\"
 *                       stripped, '\' -> '/'), rel rewrites a target on
 *                       this volume into a path relative to the link
 *   streams_interface=auto|windows|none   "name:stream" path access to
 *                       alternate data streams: auto (default) tries the
 *                       real name first and the stream second; windows
 *                       takes every "a:b" as a stream and hides names
 *                       with ':'; none never parses streams
 *   errors=continue|remount-ro|panic   corruption found while writing:
 *                       remount-ro (default) latches the mount read-only,
 *                       continue fails only that operation, panic panics;
 *                       the volume stays marked dirty in every case
 *   discard             freed clusters are discarded once the cleared
 *                       $Bitmap bits are on the device
 *   mft_zone_multiplier=1..4   MFT zone of 12.5 % per step, kept for $MFT
 *                       growth (data uses it only when the rest is full)
 *   symlink=wsl|native  what symlink() creates: a WSL LX_SYMLINK reparse
 *                       point (default) or a native symbolic link
 *   windows_names       refuse names Windows cannot use; new names go
 *                       into the Win32 namespace (also with nocase), else
 *                       into the POSIX namespace
 *   preallocated_size=N growing room (bytes, default 64 KB) kept free
 *                       behind the tail of a growing file, so appends stay
 *                       contiguous; nothing is allocated for it on disk
 *   iocharset=utf8 / nls=utf8, case_sensitive=   accepted (names are
 *                       UTF-8 always)
 * and the generic ro / rw.  show_options() prints the non-default ones.
 *
 * Writing: delayed allocation through vfs_sb_ops_t.alloc_blocks,
 * metadata written through in a fixed order, lazy arming in the write
 * chokepoint, O_DIRECT (open flag 0x4000, passed through by the VFS) for
 * device-sector aligned requests on mapped data.
 *
 * statfs f_type: NTFS_SUPER_MAGIC (0x5346544E).
 */

#ifndef LIKEOS_NTFS_H
#define LIKEOS_NTFS_H

#include <kernel/uapi/status.h>
#include <kernel/mm/vmscan.h>
#include <kernel/dev/block/block.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/pagecache.h>

/* ---- Block ids (section 1) ---------------------------------------------- */
#define NTFS_BID_TAG (1UL << 63)
#define NTFS_BID_HANDLE_SHIFT 38
#define NTFS_BID_HANDLE_BITS 24
#define NTFS_BID_INDEX_MASK ((1UL << NTFS_BID_HANDLE_SHIFT) - 1)
#define NTFS_HANDLE_MAX ((1UL << NTFS_BID_HANDLE_BITS) - 1)
#define NTFS_BID_ENC(h, idx)                                          \
	(NTFS_BID_TAG |                                               \
	 (((unsigned long)(h) & NTFS_HANDLE_MAX) << NTFS_BID_HANDLE_SHIFT) | \
	 ((unsigned long)(idx) & NTFS_BID_INDEX_MASK))
#define NTFS_BID_HANDLE(b) \
	(((unsigned long)(b) >> NTFS_BID_HANDLE_SHIFT) & NTFS_HANDLE_MAX)
#define NTFS_BID_INDEX(b) ((unsigned long)(b) & NTFS_BID_INDEX_MASK)
#define NTFS_BID_IS(b) \
	(((unsigned long)(b) & (NTFS_BID_TAG | (1UL << 62))) == NTFS_BID_TAG)
/* Every encoded id is below the marker (bit 62 is clear in all of them). */
#define NTFS_EOC_MARKER 0xC000000000000000UL
#define NTFS_BID_EOC 0xFFFFFFFFFFFFFFFFUL

/* Reserved stream handles: the volume's own metadata streams.  Handles of
 * files, alternate data streams and directory indexes start at
 * NTFS_HANDLE_FIRST_DYNAMIC. */
#define NTFS_HANDLE_MFT 1 /* $MFT:$DATA */
#define NTFS_HANDLE_MFT_BITMAP 2 /* $MFT:$BITMAP */
#define NTFS_HANDLE_BITMAP 3 /* $Bitmap:$DATA (cluster bitmap) */
#define NTFS_HANDLE_LOGFILE 4 /* $LogFile:$DATA */
#define NTFS_HANDLE_SECURE_SDS 5 /* $Secure:$SDS */
#define NTFS_HANDLE_FIRST_DYNAMIC 16

/* Root directory MFT record (st_ino of the mount's root). */
#define NTFS_ROOT_INO 5UL

/* Exposed block size never exceeds a page (section 1). */
#define NTFS_MAX_EXPOSED_BLOCK 4096UL

/* ===========================================================================
 * NTFS on-disk format
 * ===========================================================================
 *
 * Every multi-byte field is little-endian.  The structures are packed and
 * describe the documented on-disk layout; the driver reads them only after
 * the bounds of the containing buffer were checked (section 5, "Hostile
 * media"), and reads 8-byte fields through the packed members so that an
 * unaligned record never faults.
 */

#define NTFS_SUPER_MAGIC 0x5346544EUL /* "NTFS", statfs f_type */

/* The update sequence (multi-sector fixup) stride: always 512 bytes,
 * whatever the sector size of the volume or the device. */
#define NTFS_BLOCK_SIZE 512U
#define NTFS_BLOCK_SIZE_BITS 9

/* Magic numbers of the multi-sector protected records. */
#define NTFS_MAGIC_FILE 0x454C4946U /* "FILE" MFT record */
#define NTFS_MAGIC_INDX 0x58444E49U /* "INDX" index block */
#define NTFS_MAGIC_BAAD 0x44414142U /* "BAAD" failed fixup on disk */
#define NTFS_MAGIC_RSTR 0x52545352U /* "RSTR" $LogFile restart page */
#define NTFS_MAGIC_RCRD 0x44524352U /* "RCRD" $LogFile record page */
#define NTFS_MAGIC_CHKD 0x444B4843U /* "CHKD" restart page touched by chkdsk */

/* ---- Boot sector ---------------------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_boot_sector {
	uint8_t jump[3];
	uint8_t oem_id[8]; /* "NTFS    " */
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster; /* >= 0xF4: 2^(256 - value) */
	uint16_t reserved_sectors;
	uint8_t fats;
	uint16_t root_entries;
	uint16_t sectors;
	uint8_t media_type;
	uint16_t sectors_per_fat;
	uint16_t sectors_per_track;
	uint16_t heads;
	uint32_t hidden_sectors;
	uint32_t large_sectors;
	uint8_t physical_drive;
	uint8_t current_head;
	uint8_t extended_boot_signature;
	uint8_t reserved2;
	uint64_t number_of_sectors; /* volume size in NTFS sectors - 1 backup */
	uint64_t mft_lcn;
	uint64_t mftmirr_lcn;
	int8_t clusters_per_mft_record; /* < 0: 2^-value bytes */
	uint8_t reserved0[3];
	int8_t clusters_per_index_record; /* < 0: 2^-value bytes */
	uint8_t reserved1[3];
	uint64_t volume_serial_number;
	uint32_t checksum;
	uint8_t bootstrap[426];
	uint16_t end_of_sector_marker; /* 0xAA55 */
} ntfs_boot_sector_t;

/* ---- System files (MFT record numbers) ------------------------------------ */
#define NTFS_FILE_MFT 0
#define NTFS_FILE_MFTMIRR 1
#define NTFS_FILE_LOGFILE 2
#define NTFS_FILE_VOLUME 3
#define NTFS_FILE_ATTRDEF 4
#define NTFS_FILE_ROOT 5
#define NTFS_FILE_BITMAP 6
#define NTFS_FILE_BOOT 7
#define NTFS_FILE_BADCLUS 8
#define NTFS_FILE_SECURE 9 /* $Quota on NTFS 1.2 */
#define NTFS_FILE_UPCASE 10
#define NTFS_FILE_EXTEND 11
#define NTFS_FILE_FIRST_USER 16

/* MFT reference: 48-bit record number, 16-bit sequence number. */
#define NTFS_MREF_NO(r) ((uint64_t)(r) & 0x0000FFFFFFFFFFFFULL)
#define NTFS_MREF_SEQ(r) ((uint16_t)((uint64_t)(r) >> 48))
#define NTFS_MAKE_MREF(no, seq) \
	(((uint64_t)(no) & 0x0000FFFFFFFFFFFFULL) | ((uint64_t)(seq) << 48))
/* Record numbers are limited to 32 bits by the MFT bitmap layout. */
#define NTFS_MAX_MFT_NO 0xFFFFFFFFULL

/* ---- MFT record header ------------------------------------------------------ */
typedef struct __attribute__((packed)) ntfs_mft_record {
	uint32_t magic; /* "FILE" */
	uint16_t usa_ofs;
	uint16_t usa_count;
	uint64_t lsn;
	uint16_t sequence_number;
	uint16_t link_count;
	uint16_t attrs_offset;
	uint16_t flags; /* NTFS_MFT_REC_* */
	uint32_t bytes_in_use;
	uint32_t bytes_allocated;
	uint64_t base_mft_record; /* 0 for a base record */
	uint16_t next_attr_instance;
	/* NTFS 3.1+ only: */
	uint16_t reserved;
	uint32_t mft_record_number;
} ntfs_mft_record_t;

#define NTFS_MFT_REC_HDR_OLD 0x2A /* NTFS 3.0 and earlier: USA at 0x2A */
#define NTFS_MFT_REC_HDR 0x30 /* NTFS 3.1: USA at 0x30 */

#define NTFS_MFT_REC_IN_USE 0x0001
#define NTFS_MFT_REC_IS_DIRECTORY 0x0002
#define NTFS_MFT_REC_IS_4 0x0004 /* in $Extend */
#define NTFS_MFT_REC_IS_VIEW_INDEX 0x0008

/* ---- Attribute types ----------------------------------------------------- */
#define NTFS_AT_UNUSED 0x00U
#define NTFS_AT_STANDARD_INFORMATION 0x10U
#define NTFS_AT_ATTRIBUTE_LIST 0x20U
#define NTFS_AT_FILE_NAME 0x30U
#define NTFS_AT_OBJECT_ID 0x40U
#define NTFS_AT_SECURITY_DESCRIPTOR 0x50U
#define NTFS_AT_VOLUME_NAME 0x60U
#define NTFS_AT_VOLUME_INFORMATION 0x70U
#define NTFS_AT_DATA 0x80U
#define NTFS_AT_INDEX_ROOT 0x90U
#define NTFS_AT_INDEX_ALLOCATION 0xA0U
#define NTFS_AT_BITMAP 0xB0U
#define NTFS_AT_REPARSE_POINT 0xC0U
#define NTFS_AT_EA_INFORMATION 0xD0U
#define NTFS_AT_EA 0xE0U
#define NTFS_AT_PROPERTY_SET 0xF0U
#define NTFS_AT_LOGGED_UTILITY_STREAM 0x100U
#define NTFS_AT_FIRST_USER_DEFINED 0x1000U
#define NTFS_AT_END 0xFFFFFFFFU

/* Attribute record flags. */
#define NTFS_ATTR_IS_COMPRESSED 0x0001
#define NTFS_ATTR_COMPRESSION_MASK 0x00FF
#define NTFS_ATTR_IS_ENCRYPTED 0x4000
#define NTFS_ATTR_IS_SPARSE 0x8000

/* Attribute record header, common part. */
typedef struct __attribute__((packed)) ntfs_attr_record {
	uint32_t type;
	uint32_t length;
	uint8_t non_resident;
	uint8_t name_length; /* in UTF-16 units */
	uint16_t name_offset;
	uint16_t flags;
	uint16_t instance;
	union {
		struct __attribute__((packed)) {
			uint32_t value_length;
			uint16_t value_offset;
			uint8_t flags; /* RESIDENT_ATTR_IS_INDEXED 0x01 */
			uint8_t reserved;
		} resident;
		struct __attribute__((packed)) {
			int64_t lowest_vcn;
			int64_t highest_vcn;
			uint16_t mapping_pairs_offset;
			uint8_t compression_unit; /* log2 clusters, 0 = none */
			uint8_t reserved[5];
			int64_t allocated_size;
			int64_t data_size;
			int64_t initialized_size;
			int64_t compressed_size; /* compressed / sparse only */
		} non_resident;
	} data;
} ntfs_attr_record_t;

#define NTFS_ATTR_RESIDENT_HDR 24U /* header up to the value */
#define NTFS_ATTR_NONRES_HDR 64U /* up to initialized_size */
#define NTFS_ATTR_NONRES_HDR_COMP 72U /* + compressed_size */

/* ---- $STANDARD_INFORMATION -------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_standard_information {
	int64_t creation_time;
	int64_t last_data_change_time;
	int64_t last_mft_change_time;
	int64_t last_access_time;
	uint32_t file_attributes;
	/* NTFS 1.2 ends after 12 reserved bytes (48 bytes in all). */
	uint32_t maximum_versions;
	uint32_t version_number;
	uint32_t class_id;
	uint32_t owner_id;
	uint32_t security_id;
	uint64_t quota_charged;
	uint64_t usn;
} ntfs_standard_information_t;

#define NTFS_SI_SIZE_V12 48U
#define NTFS_SI_SIZE_V3 72U

/* File attribute flags ($STANDARD_INFORMATION / $FILE_NAME). */
#define NTFS_FILE_ATTR_READONLY 0x00000001U
#define NTFS_FILE_ATTR_HIDDEN 0x00000002U
#define NTFS_FILE_ATTR_SYSTEM 0x00000004U
#define NTFS_FILE_ATTR_DIRECTORY 0x00000010U
#define NTFS_FILE_ATTR_ARCHIVE 0x00000020U
#define NTFS_FILE_ATTR_DEVICE 0x00000040U
#define NTFS_FILE_ATTR_NORMAL 0x00000080U
#define NTFS_FILE_ATTR_TEMPORARY 0x00000100U
#define NTFS_FILE_ATTR_SPARSE_FILE 0x00000200U
#define NTFS_FILE_ATTR_REPARSE_POINT 0x00000400U
#define NTFS_FILE_ATTR_COMPRESSED 0x00000800U
#define NTFS_FILE_ATTR_OFFLINE 0x00001000U
#define NTFS_FILE_ATTR_NOT_CONTENT_INDEXED 0x00002000U
#define NTFS_FILE_ATTR_ENCRYPTED 0x00004000U
#define NTFS_FILE_ATTR_RECALL_ON_OPEN 0x00040000U
#define NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT 0x10000000U
#define NTFS_FILE_ATTR_DUP_VIEW_INDEX_PRESENT 0x20000000U

/* ---- $ATTRIBUTE_LIST entry ---------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_attr_list_entry {
	uint32_t type;
	uint16_t length; /* multiple of 8 */
	uint8_t name_length;
	uint8_t name_offset;
	int64_t lowest_vcn;
	uint64_t mft_reference;
	uint16_t instance;
	/* name follows */
} ntfs_attr_list_entry_t;

#define NTFS_ALE_HDR 26U
/* Attribute lists larger than this are refused (corrupt). */
#define NTFS_ATTR_LIST_MAX (256U * 1024U)

/* ---- $FILE_NAME ---------------------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_file_name {
	uint64_t parent_directory;
	int64_t creation_time;
	int64_t last_data_change_time;
	int64_t last_mft_change_time;
	int64_t last_access_time;
	int64_t allocated_size;
	int64_t data_size;
	uint32_t file_attributes;
	union {
		struct __attribute__((packed)) {
			uint16_t packed_ea_size;
			uint16_t reserved;
		} ea;
		uint32_t reparse_point_tag;
	} type;
	uint8_t file_name_length; /* in UTF-16 units */
	uint8_t file_name_type; /* NTFS_FILE_NAME_* */
	/* name follows */
} ntfs_file_name_t;

#define NTFS_FN_HDR 66U
#define NTFS_FILE_NAME_POSIX 0
#define NTFS_FILE_NAME_WIN32 1
#define NTFS_FILE_NAME_DOS 2
#define NTFS_FILE_NAME_WIN32_AND_DOS 3
#define NTFS_MAX_NAME_LEN 255U

/* ---- $VOLUME_INFORMATION ------------------------------------------------ */
typedef struct __attribute__((packed)) ntfs_volume_information {
	uint64_t reserved;
	uint8_t major_ver;
	uint8_t minor_ver;
	uint16_t flags; /* NTFS_VOLUME_* */
} ntfs_volume_information_t;

#define NTFS_VOLUME_IS_DIRTY 0x0001U
#define NTFS_VOLUME_RESIZE_LOG_FILE 0x0002U
#define NTFS_VOLUME_UPGRADE_ON_MOUNT 0x0004U
#define NTFS_VOLUME_MOUNTED_ON_NT4 0x0008U
#define NTFS_VOLUME_DELETE_USN_UNDERWAY 0x0010U
#define NTFS_VOLUME_REPAIR_OBJECT_ID 0x0020U
#define NTFS_VOLUME_CHKDSK_UNDERWAY 0x4000U
#define NTFS_VOLUME_MODIFIED_BY_CHKDSK 0x8000U
/* Flags that keep a volume read-only (dirty, log resize pending, upgrade
 * pending, chkdsk underway / modified). */
#define NTFS_VOLUME_MUST_MOUNT_RO_MASK 0xC027U
/* Longest volume label, in UTF-16 units. */
#define NTFS_MAX_LABEL_LEN 128U

/* ---- $AttrDef entry -------------------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_attr_def {
	uint16_t name[64];
	uint32_t type;
	uint32_t display_rule;
	uint32_t collation_rule;
	uint32_t flags;
	int64_t min_size;
	int64_t max_size;
} ntfs_attr_def_t;

#define NTFS_ATTRDEF_ENTRY 160U

/* ---- Indexes --------------------------------------------------------------- */
#define NTFS_COLLATION_BINARY 0x00U
#define NTFS_COLLATION_FILE_NAME 0x01U
#define NTFS_COLLATION_UNICODE_STRING 0x02U
#define NTFS_COLLATION_NTOFS_ULONG 0x10U
#define NTFS_COLLATION_NTOFS_SID 0x11U
#define NTFS_COLLATION_NTOFS_SECURITY_HASH 0x12U
#define NTFS_COLLATION_NTOFS_ULONGS 0x13U

typedef struct __attribute__((packed)) ntfs_index_header {
	uint32_t entries_offset; /* from the start of this header */
	uint32_t index_length; /* bytes in use, from this header */
	uint32_t allocated_size;
	uint8_t flags; /* root: LARGE_INDEX; block: INDEX_NODE */
	uint8_t reserved[3];
} ntfs_index_header_t;

#define NTFS_INDEX_HDR 16U
#define NTFS_SMALL_INDEX 0x00 /* root: no $INDEX_ALLOCATION */
#define NTFS_LARGE_INDEX 0x01 /* root: has $INDEX_ALLOCATION */
#define NTFS_LEAF_NODE 0x00
#define NTFS_INDEX_NODE 0x01

typedef struct __attribute__((packed)) ntfs_index_root {
	uint32_t type; /* indexed attribute type ($FILE_NAME), 0 = view */
	uint32_t collation_rule;
	uint32_t index_block_size;
	uint8_t clusters_per_index_block;
	uint8_t reserved[3];
	ntfs_index_header_t index;
} ntfs_index_root_t;

#define NTFS_INDEX_ROOT_HDR 16U /* before the index header */

typedef struct __attribute__((packed)) ntfs_index_block {
	uint32_t magic; /* "INDX" */
	uint16_t usa_ofs;
	uint16_t usa_count;
	uint64_t lsn;
	int64_t index_block_vcn;
	ntfs_index_header_t index;
} ntfs_index_block_t;

#define NTFS_INDX_HDR 0x18U /* bytes before the index header */

typedef struct __attribute__((packed)) ntfs_index_entry {
	union {
		uint64_t indexed_file; /* $I30: MFT reference */
		struct __attribute__((packed)) {
			uint16_t data_offset;
			uint16_t data_length;
			uint32_t reserved;
		} vi; /* view indexes */
	} data;
	uint16_t length; /* multiple of 8 */
	uint16_t key_length;
	uint16_t flags; /* NTFS_INDEX_ENTRY_* */
	uint16_t reserved;
	/* key follows; INDEX_ENTRY_NODE: child VCN in the last 8 bytes */
} ntfs_index_entry_t;

#define NTFS_IE_HDR 16U
#define NTFS_INDEX_ENTRY_NODE 0x0001
#define NTFS_INDEX_ENTRY_END 0x0002
/* Deepest B+tree accepted. */
#define NTFS_INDEX_MAX_DEPTH 32

/* ---- $Secure -------------------------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_sds_header {
	uint32_t hash;
	uint32_t security_id;
	uint64_t offset; /* of this entry in $SDS */
	uint32_t length; /* header + descriptor */
} ntfs_sds_header_t;

#define NTFS_SDS_HDR 20U
#define NTFS_SDS_BLOCK (256U * 1024U) /* mirrored at +256 KB */
#define NTFS_SD_HDR 20U /* self-relative descriptor header */
#define NTFS_SD_MAX (64U * 1024U)

/* ---- Reparse points -------------------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_reparse_point {
	uint32_t reparse_tag;
	uint16_t reparse_data_length;
	uint16_t reserved;
	/* data follows */
} ntfs_reparse_point_t;

#define NTFS_REPARSE_HDR 8U
#define NTFS_REPARSE_MAX (16U * 1024U)
#define NTFS_IO_REPARSE_TAG_MOUNT_POINT 0xA0000003U
#define NTFS_IO_REPARSE_TAG_SYMLINK 0xA000000CU
#define NTFS_IO_REPARSE_TAG_DEDUP 0x80000013U
#define NTFS_IO_REPARSE_TAG_WOF 0x80000017U
#define NTFS_IO_REPARSE_TAG_LX_SYMLINK 0xA000001DU
#define NTFS_IO_REPARSE_TAG_AF_UNIX 0x80000023U
#define NTFS_IO_REPARSE_TAG_LX_FIFO 0x80000024U
#define NTFS_IO_REPARSE_TAG_LX_CHR 0x80000025U
#define NTFS_IO_REPARSE_TAG_LX_BLK 0x80000026U
#define NTFS_SYMLINK_FLAG_RELATIVE 0x00000001U

/* WOF ("system compression") reparse data. */
typedef struct __attribute__((packed)) ntfs_wof_reparse {
	uint32_t version; /* 1 */
	uint32_t provider; /* 2 = file provider */
	uint32_t provider_version; /* 1 */
	uint32_t compression_format; /* NTFS_WOF_* */
} ntfs_wof_reparse_t;

#define NTFS_WOF_XPRESS4K 0
#define NTFS_WOF_LZX 1
#define NTFS_WOF_XPRESS8K 2
#define NTFS_WOF_XPRESS16K 3
#define NTFS_WOF_NONE 0xFF /* not a readable WOF format */

/* ---- Extended attributes ----------------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_ea_information {
	uint16_t ea_length; /* packed size of the EAs */
	uint16_t need_ea_count;
	uint32_t ea_query_length; /* size of the $EA stream in use */
} ntfs_ea_information_t;

typedef struct __attribute__((packed)) ntfs_ea_entry {
	uint32_t next_entry_offset; /* 0 = last */
	uint8_t flags; /* NTFS_EA_NEED_EA */
	uint8_t ea_name_length; /* without the NUL */
	uint16_t ea_value_length;
	/* name, NUL, value follow */
} ntfs_ea_entry_t;

#define NTFS_EA_HDR 8U
#define NTFS_EA_NEED_EA 0x80
#define NTFS_EA_MAX (64U * 1024U)

/* ---- $LogFile ---------------------------------------------------------------- */
typedef struct __attribute__((packed)) ntfs_restart_page_header {
	uint32_t magic; /* "RSTR" or "CHKD" */
	uint16_t usa_ofs;
	uint16_t usa_count;
	int64_t chkdsk_lsn;
	uint32_t system_page_size;
	uint32_t log_page_size;
	uint16_t restart_area_offset;
	int16_t minor_ver;
	int16_t major_ver;
} ntfs_restart_page_header_t;

typedef struct __attribute__((packed)) ntfs_restart_area {
	int64_t current_lsn;
	uint16_t log_clients;
	uint16_t client_free_list;
	uint16_t client_in_use_list;
	uint16_t flags; /* NTFS_RESTART_VOLUME_IS_CLEAN */
	uint32_t seq_number_bits;
	uint16_t restart_area_length;
	uint16_t client_array_offset;
	int64_t file_size;
	uint32_t last_lsn_data_length;
	uint16_t log_record_header_length;
	uint16_t log_page_data_offset;
	uint32_t restart_log_open_count;
	uint32_t reserved;
} ntfs_restart_area_t;

#define NTFS_RESTART_AREA_HDR 48U
#define NTFS_LOG_CLIENT_RECORD 160U
#define NTFS_LOGFILE_NO_CLIENT 0xFFFFU
#define NTFS_RESTART_VOLUME_IS_CLEAN 0x0002U

/* ---- Compression ----------------------------------------------------------------- */
#define NTFS_COMPRESSION_UNIT 4 /* log2 clusters per unit */
#define NTFS_LZNT1_CHUNK 4096U
#define NTFS_COMPRESS_MAX_CLUSTER 4096U

/* The filesystem type mount(2) instantiates for "ntfs". */
extern const vfs_fstype_t ntfs_fstype;

/* ===========================================================================
 * Driver-internal section
 * ===========================================================================
 *
 * Seen only by the files of the driver, the .c files of kernel/fs/ntfs/
 * (each defines NTFS_INTERNAL before it includes this header): the
 * in-memory structures (S1), the inline helpers they use (S2 byte order
 * and overflow-checked arithmetic, collation, index entry accessors), a
 * few shared types, and the prototypes of the support routines.  The
 * other declarations shared between the files are in ntfs_int.h, which
 * also maps the section numbers (S1 .. S33) to the files. */
#ifdef NTFS_INTERNAL

#include <kernel/ke/sched.h> /* spinlock_t */

/* Helpers without a caller inside the kernel: the trim of the free space
 * (no ioctl hook in the VFS yet; the host unit test calls it) and a
 * UTF-16 length helper. */
#define NTFS_MAYBE_UNUSED __attribute__((unused))

/* ===========================================================================
 * S1  In-memory structures
 * =========================================================================== */

/* ---- Limits ---------------------------------------------------------------- */

/* Record cache entries (fixed-up copies of MFT records). */
#define NTFS_RC_ENTRIES 1024
#define NTFS_RC_HASH 2048
/* Index block cache entries (fixed-up, validated INDX copies). */
#define NTFS_IC_ENTRIES 256
#define NTFS_IC_HASH 512
/* Stream handle table: slots (handles 0..NTFS_SLOTS-1), and how many
 * unreferenced streams may be kept around for their cached pages -- which
 * are keyed by the handle and go with it.  They stay until memory runs
 * short (the reclaim thread forgets the oldest through the filesystem's
 * shrinker) or the handle table needs room: this is a ceiling, not the
 * number they are trimmed to. */
#define NTFS_SLOTS 16384
#define NTFS_STREAM_HASH 2048
#define NTFS_STREAM_KEEP (NTFS_SLOTS * 3 / 4)
/* Per-file content fences (hashed by MFT record number). */
#define NTFS_FLOCKS 64
/* Most runs one stream may have (an absurdly fragmented file still fits;
 * a crafted attribute cannot make us allocate without bound). */
#define NTFS_MAX_RUNS (1U << 20)
/* Runlist memory unreferenced cached streams may keep in all; beyond it
 * the oldest are evicted (a crafted volume cannot pin many large
 * runlists). */
#define NTFS_LRU_RL_BUDGET (64UL * 1024 * 1024)
/* Most extents (attribute records) one stream may be split into. */
#define NTFS_MAX_EXTENTS 65536
/* Whole-attribute reads (EA, reparse, attribute list, ...) are capped. */
#define NTFS_READALL_MAX (64U * 1024U)
/* Device reads are split into requests of at most this many sectors. */
#define NTFS_MAX_SECTORS_PER_READ 256
/* Symbolic links followed during one resolution. */
#define NTFS_MAX_LINKS 40
/* Upcase table: 64 K UTF-16 units. */
#define NTFS_UPCASE_MAX 65536U
/* $AttrDef: formatters write 2560 bytes; anything up to 64 KB is accepted. */
#define NTFS_ATTRDEF_MAX (64U * 1024U)
/* Readdir: entries gathered from one leaf per descent. */
#define NTFS_RD_BATCH 32
/* $MFTMirr: records compared. */
#define NTFS_MIRR_CMP_MAX 16
/* Freed extents queued for discard until the next device flush. */
#define NTFS_DISC_MAX 64

/* ---- Locks ----------------------------------------------------------------- */

/* A private recursive reader/writer semaphore, the design of ext4's: shared
 * holders count in `readers'; the exclusive owner may take it again (either
 * mode) as a depth increment; queued writers defer new readers except a
 * task that already holds some filesystem lock shared (its nested shared
 * acquisition must not wait behind a writer waiting for it); an exclusive
 * release admits the readers queued at that moment before the next writer
 * (reader_turn), so neither side starves. */
/* Tasks whose shared holds are recorded per lock (release_locks_for_task
 * gives a dead task's shared holds back; holds beyond the table are only
 * counted). */
#define NTFS_RWSEM_TRACK 32

typedef struct ntfs_rhold {
	uint64_t task; /* holder's task id */
	int count; /* its shared holds; 0: slot free */
} ntfs_rhold_t;

typedef struct ntfs_rwsem {
	volatile int readers;
	volatile int writer;
	volatile uint64_t owner;
	volatile int wdepth;
	volatile int w_wait;
	volatile int r_wait;
	volatile int reader_turn;
	int untracked; /* shared holds not in rh[] */
	ntfs_rhold_t rh[NTFS_RWSEM_TRACK];
	spinlock_t lock;
} ntfs_rwsem_t;

/* ---- Runlists --------------------------------------------------------------- */

#define NTFS_LCN_HOLE (-1LL)

typedef struct ntfs_run {
	int64_t vcn;
	int64_t lcn; /* NTFS_LCN_HOLE for a sparse run */
	int64_t len; /* clusters, > 0 */
} ntfs_run_t;

typedef struct ntfs_runlist {
	ntfs_run_t *runs; /* sorted by vcn, contiguous, coalesced */
	uint32_t count;
	uint32_t cap;
	int64_t end_vcn; /* first VCN the runlist does not describe */
} ntfs_runlist_t;

/* ---- Streams ------------------------------------------------------------- */

/* ntfs_stream_t.flags */
#define NTFS_ST_RESIDENT 0x0001U /* value inline in the MFT record */
#define NTFS_ST_COMPRESSED 0x0002U /* LZNT1 compression units */
#define NTFS_ST_SPARSE 0x0004U
#define NTFS_ST_ENCRYPTED 0x0008U /* $EFS: data is never returned */
#define NTFS_ST_WOF 0x0010U /* system-compressed (data elsewhere) */
#define NTFS_ST_NOALLOC 0x0020U /* directory index without allocation */
#define NTFS_ST_INDEX 0x0040U /* an index allocation stream */
#define NTFS_ST_SYSTEM 0x0080U /* a reserved metadata stream */
#define NTFS_ST_BAD 0x0100U /* corrupt: every data access fails */
#define NTFS_ST_DELALLOC 0x0200U /* blocks await allocation (dl) */
#define NTFS_ST_DIRTY 0x0400U /* has (or had) dirty pages */
#define NTFS_ST_META 0x0800U /* sizes / runlist differ from the disk */
#define NTFS_ST_WASRES 0x1000U /* resident on disk, non-resident here */
#define NTFS_ST_DEAD 0x2000U /* its file was deleted: no more commits */
#define NTFS_ST_PROTECTED 0x4000U /* volume metadata: never written by users */

/* A run of clusters that write() reserved and writeback has not placed yet
 * (delayed allocation; S30). */
typedef struct ntfs_dext {
	int64_t vcn;
	int64_t len;
} ntfs_dext_t;

typedef struct ntfs_stream {
	uint32_t handle; /* chain id = NTFS_BID_ENC(handle, 0) */
	int refs; /* under fs->hdl_lock */
	uint64_t mft_no; /* base MFT record */
	uint16_t seq; /* its sequence number when opened */
	uint32_t type; /* attribute type */
	uint8_t name_len; /* UTF-16 units */
	uint16_t *name; /* NULL when unnamed */
	uint32_t flags; /* NTFS_ST_* */
	uint64_t data_size;
	uint64_t init_size;
	uint64_t alloc_size;
	uint64_t comp_size; /* compressed / sparse: clusters in use */
	uint8_t comp_unit; /* log2 clusters per compression unit */
	uint32_t cu_bytes; /* compression unit in bytes */
	/* System-compressed (WOF) files: the format of the chunks in the
	 * file's WofCompressedData stream (NTFS_WOF_*; NTFS_WOF_NONE when the
	 * reparse data named no known format) and the chunk size. */
	uint8_t wof_fmt;
	uint32_t wof_chunk;
	ntfs_runlist_t rl;
	uint8_t *res_data; /* resident value copy */
	uint32_t res_len;
	/* Index allocation streams: geometry of the index. */
	uint32_t ib_size;
	uint32_t ib_vcn_bits; /* VCN unit: cluster or 512 bytes */
	/* Write state (all under the metadata lock exclusive). */
	ntfs_dext_t *dl; /* reserved, unplaced VCN runs, sorted */
	uint32_t dl_n;
	uint32_t dl_cap;
	uint64_t dl_resv; /* clusters reserved in dl */
	int err; /* writeback / allocation error for fsync/close */
	int pinned; /* holds an extra reference while META/DIRTY */
	int64_t mtime_pend; /* NTFS time of a data change not committed */
	uint64_t since_commit; /* clusters placed since the last commit */
	int64_t pl_lo, pl_hi; /* VCNs placed since then (pl_hi 0: none) */
	ntfs_runlist_t old_rl; /* clusters to free after the next commit
				* (a compressed layout replaced) */
	/* Hash chain and LRU of unreferenced streams (fs->hdl_lock). */
	struct ntfs_stream *hash_next;
	struct ntfs_stream *lru_prev;
	struct ntfs_stream *lru_next;
	int on_lru;
	unsigned long lru_bytes; /* runlist bytes counted while on the LRU */
} ntfs_stream_t;

/* ---- Mount options ------------------------------------------------------- */

#define NTFS_ERRORS_CONTINUE 0
#define NTFS_ERRORS_RO 1
#define NTFS_ERRORS_PANIC 2

#define NTFS_SYMLINK_RAW 0
#define NTFS_SYMLINK_REL 1

#define NTFS_STREAMS_AUTO 0
#define NTFS_STREAMS_WINDOWS 1
#define NTFS_STREAMS_NONE 2

#define NTFS_DEFAULT_FMASK 0177U
#define NTFS_DEFAULT_DMASK 0077U

typedef struct ntfs_opts {
	uint32_t uid;
	uint32_t gid;
	uint32_t fmask;
	uint32_t dmask;
	int uid_set; /* uid= given: overrides $LXUID */
	int gid_set; /* gid= given: overrides $LXGID */
	int force;
	int recover; /* replay an unclean $LogFile (S33) */
	int ask_recover; /* recovery=ask: fail with ST_UCLEAN instead */
	int discard;
	int acl;
	int nocase;
	int sparse;
	int show_sys;
	int hide_dot;
	int nohidden;
	int sys_immutable;
	int windows_names;
	int native_symlink; /* NTFS_SYMLINK_* */
	int streams; /* NTFS_STREAMS_* */
	int errors; /* NTFS_ERRORS_* */
	int mft_zone_mult; /* 1..4 */
	int errors_set; /* errors= was given */
	uint64_t prealloc; /* bytes */
	int symlink_native; /* symlink=native */
} ntfs_opts_t;

/* ---- $LogFile state (reported for the rw gate) -------------------------- */

#define NTFS_LOG_UNKNOWN 0
#define NTFS_LOG_CLEAN 1 /* restart area says clean / no clients */
#define NTFS_LOG_EMPTY 2 /* all 0xFF: emptied by an earlier mount */
#define NTFS_LOG_UNCLEAN 3 /* has log records to replay */
#define NTFS_LOG_BAD 4 /* structurally invalid */
#define NTFS_LOG_ABSENT 5 /* NTFS 1.x volumes without restart pages */

/* ---- The mounted volume ----------------------------------------------------- */

struct ntfs_rc_entry {
	uint64_t no; /* MFT record number; ~0 = empty */
	uint8_t *data; /* rec_size bytes, fixed up and validated */
	uint64_t stamp; /* LRU clock */
	int next; /* hash chain (index), -1 = end */
};

struct ntfs_ic_entry {
	uint32_t handle; /* index allocation stream; 0 = empty */
	int64_t vcn;
	uint8_t *data; /* ib_size bytes */
	uint32_t size;
	uint64_t stamp;
	int next;
};

typedef struct ntfs_fs {
	/* --- device and geometry --- */
	block_device_t *bdev; /* the real (claimed) device */
	/* The device the page cache sees: reads forward to bdev, writes go
	 * through ntfs_dev_write() (ntfs.h section 3). */
	block_device_t guard;
	vfs_superblock_t sb;
	vfs_mount_t *mnt;
	uint32_t dev_ss; /* device logical sector size */
	uint32_t dev_ss_bits;
	uint64_t dev_sectors;
	uint32_t sector_size; /* NTFS bytes per sector */
	uint32_t cluster_size;
	uint32_t cluster_bits;
	uint64_t nr_clusters;
	uint64_t mft_lcn;
	uint64_t mftmirr_lcn;
	uint32_t rec_size; /* MFT record */
	uint32_t rec_bits;
	uint32_t ib_size; /* default index block size (boot sector) */
	uint64_t serial;
	uint32_t exp_bs; /* exposed block size (<= 4096) */
	uint32_t exp_bits;
	uint32_t io_unit; /* max(device sector, record) for MFT writes */
	uint64_t mft_records; /* records covered by $MFT's data size */
	/* --- volume --- */
	uint8_t major_ver;
	uint8_t minor_ver;
	uint16_t vol_flags;
	char *label; /* UTF-8, NUL terminated (may be "") */
	uint16_t *upcase;
	uint32_t upcase_len; /* entries */
	uint8_t *attrdef;
	uint32_t attrdef_len;
	/* --- state --- */
	int ro; /* read-only (mount request, gate or latch) */
	int rw_ok; /* the gate would allow read-write */
	uint32_t gate; /* NTFS_GATE_* reasons for read-only (bitmask) */
	int armed; /* the write chokepoint is open (lazy arming) */
	int arming; /* arming in progress (its own writes pass) */
	int latched_ro; /* corruption found while read-write */
	int errors; /* runtime errors recorded: the volume stays dirty */
	int keep_dirty; /* something was left behind (a leak a check
			 * reclaims): the volume stays dirty, writing goes on */
	uint64_t io_errs; /* failed device writes (chokepoint) */
	uint64_t unflushed; /* writes since the last cache flush */
	int hibernated; /* hibernated / Fast Startup / undeterminable */
	int was_dirty; /* VOLUME_IS_DIRTY was set before this mount */
	int replayed; /* the mount replayed $LogFile and must start over */
	ntfs_runlist_t bad_rl; /* $BadClus:$Bad (identity runs = bad) */
	int bad_done; /* its free clusters are marked in use */
	int mount_errors; /* errors recorded while mounting */
	int mirr_mismatch;
	int geometry_ro; /* MFT geometry supported read-only only */
	int log_state; /* NTFS_LOG_* */
	int64_t log_current_lsn;
	int badclus_ok;
	const char *ro_reason; /* why the gate chose read-only */
	ntfs_opts_t opt;
	/* --- locks --- */
	ntfs_rwsem_t meta;
	ntfs_rwsem_t flock[NTFS_FLOCKS];
	ntfs_rwsem_t alloc;
	spinlock_t rc_lock;
	spinlock_t ic_lock;
	spinlock_t hdl_lock;
	spinlock_t sf_lock;
	/* --- record cache --- */
	struct ntfs_rc_entry rc[NTFS_RC_ENTRIES];
	int rc_hash[NTFS_RC_HASH];
	uint64_t rc_clock;
	/* --- index block cache --- */
	struct ntfs_ic_entry ic[NTFS_IC_ENTRIES];
	int ic_hash[NTFS_IC_HASH];
	uint64_t ic_clock;
	/* --- stream handle table --- */
	ntfs_stream_t **slots; /* NTFS_SLOTS pointers */
	ntfs_stream_t *st_hash[NTFS_STREAM_HASH];
	ntfs_stream_t lru; /* sentinel of unreferenced streams */
	uint32_t lru_count;
	/* Gives unreferenced streams back under memory pressure. */
	struct vm_shrinker shrinker;
	unsigned long lru_rl_bytes; /* runlist bytes of the LRU's streams */
	uint32_t slot_hint;
	ntfs_stream_t *st_mft; /* reserved handles, pinned */
	ntfs_stream_t *st_mftbmp;
	ntfs_stream_t *st_bitmap;
	ntfs_stream_t *st_logfile;
	ntfs_stream_t *st_sds;
	/* --- statfs cache --- */
	int sf_valid;
	uint64_t sf_free_clusters;
	uint64_t sf_free_records;
	/* --- allocators (metadata lock exclusive) --- */
	int bm_valid; /* free_clusters counted */
	uint64_t free_clusters; /* clear bits of $Bitmap */
	uint64_t resv_clusters; /* reserved by delayed allocation */
	uint64_t zone_start; /* MFT zone [zone_start, zone_end) */
	uint64_t zone_end;
	uint64_t data_hint; /* next-fit position for data */
	int mftbm_valid; /* free_records counted */
	uint64_t free_records; /* clear bits of $MFT:$BITMAP */
	uint64_t mft_hint; /* next record number to try */
	uint64_t mirr_recs; /* records mirrored in $MFTMirr */
	ntfs_runlist_t mirr_rl; /* $MFTMirr's runlist */
	uint16_t new_usa_ofs; /* update sequence offset of new records */
	uint8_t ir_cpib; /* clusters_per_index_block of new roots */
	struct ntfs_ofile *ofiles[64]; /* open-file table (hdl_lock) */
	/* --- discard (option `discard'): freed extents waiting for the
	 * next device flush, after which their cleared bits are durable --- */
	ntfs_run_t disc[NTFS_DISC_MAX];
	uint32_t disc_n;
	uint64_t discarded; /* clusters discarded (statistics) */
	/* --- $Secure writer (S29b): scanned once before the first new
	 * descriptor --- */
	int sec_valid; /* sec_next_id / sec_sds_end known */
	int sec_broken; /* $Secure unusable: own descriptors instead */
	uint32_t sec_next_id;
	uint64_t sec_sds_end; /* first byte after the last $SDS entry */
	uint32_t sec_default_id; /* id of the default descriptor, 0: none */
	/* --- path state --- */
	uint64_t cwd; /* MFT reference of the working directory */
} ntfs_fs_t;

/* ---- Inode information --------------------------------------------------- */

typedef struct ntfs_inode {
	uint64_t mft_no;
	uint16_t seq;
	uint16_t rec_flags;
	int is_dir;
	uint32_t fattr; /* $STANDARD_INFORMATION file attributes */
	int64_t crtime; /* NTFS times */
	int64_t mtime;
	int64_t ctime;
	int64_t atime;
	uint32_t security_id;
	int has_sd; /* own $SECURITY_DESCRIPTOR attribute */
	uint32_t nlink;
	uint64_t parent; /* MFT reference of the first non-DOS parent */
	/* unnamed $DATA */
	int has_data;
	int data_resident;
	uint16_t data_flags;
	uint64_t data_size;
	uint64_t alloc_size;
	uint64_t comp_size;
	/* directory index */
	uint64_t dir_size;
	/* reparse point */
	int has_reparse;
	uint32_t reparse_tag;
	uint32_t reparse_len;
	uint32_t wof_format; /* valid WOF data: NTFS_WOF_*, else ~0 */
	/* extended attributes */
	int has_ea;
	int has_ea_info;
	uint32_t ea_query_len;
	int has_lxuid, has_lxgid, has_lxmod, has_lxdev;
	uint32_t lx_uid, lx_gid, lx_mode;
	uint32_t lx_major, lx_minor;
	int has_lflags;
	uint32_t lflags; /* the file-flags EA */
	/* symbolic link target length, filled by stat */
	uint32_t link_len;
} ntfs_inode_t;

/* Bits of the file-flags EA. */
#define NTFS_LFLAG_IMMUTABLE 0x1U
#define NTFS_LFLAG_APPEND 0x2U
#define NTFS_LFLAG_NODUMP 0x4U
#define NTFS_LFLAG_NOATIME 0x8U

/* ---- Open files ------------------------------------------------------------- */

typedef struct ntfs_file {
	vfs_file_t vfs; /* MUST be first */
	ntfs_fs_t *fs;
	uint64_t mref; /* MFT reference (number + sequence) */
	ntfs_stream_t *st; /* data stream / directory index stream */
	int is_dir;
	int is_ads; /* an alternate data stream */
	uint32_t mode;
	unsigned long pos;
	/* page-cache read-ahead state */
	pc_readahead_t ra; /* page-cache read-ahead state */
	void *inode; /* ic_inode_t* */
	/* readdir state: ordinal of the next entry and the resume key (the
	 * name of the last entry returned) */
	uint64_t rd_ord;
	uint16_t *rd_key;
	uint8_t rd_key_len;
	int rd_key_valid;
	int rd_eof;
	uint64_t rd_skip; /* after a seek: ordinals to pass over */
	uint64_t rd_parent; /* ".." */
} ntfs_file_t;

/* ===========================================================================
 * S2  Helpers: byte order, overflow-checked arithmetic (inline; the other
 *     helpers are in misc.c and unistr.c)
 * =========================================================================== */

static inline uint16_t ntfs_le16(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static inline uint32_t ntfs_le32(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;

	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

static inline uint64_t ntfs_le64(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | b[i];
	return v;
}

static inline void ntfs_put_le16(void *p, uint16_t v)
{
	uint8_t *b = (uint8_t *)p;

	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

static inline void ntfs_put_le32(void *p, uint32_t v)
{
	uint8_t *b = (uint8_t *)p;

	for (int i = 0; i < 4; i++)
		b[i] = (uint8_t)(v >> (8 * i));
}

static inline void ntfs_put_le64(void *p, uint64_t v)
{
	uint8_t *b = (uint8_t *)p;

	for (int i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (8 * i));
}

/* a + b without wrapping: 0 and *out unchanged on overflow. */
static inline int ntfs_add_ok(uint64_t a, uint64_t b, uint64_t *out)
{
	if (a > ~0ULL - b)
		return 0;
	*out = a + b;
	return 1;
}

static inline int ntfs_mul_ok(uint64_t a, uint64_t b, uint64_t *out)
{
	if (a && b > ~0ULL / a)
		return 0;
	*out = a * b;
	return 1;
}

/* Is [off, off + len) inside [0, size)?  Overflow-safe. */
static inline int ntfs_range_ok(uint64_t off, uint64_t len, uint64_t size)
{
	return off <= size && len <= size - off;
}

static inline int ntfs_is_pow2(uint64_t v)
{
	return v && !(v & (v - 1));
}

static inline uint32_t ntfs_log2(uint64_t v)
{
	uint32_t n = 0;

	while (v > 1) {
		v >>= 1;
		n++;
	}
	return n;
}

static inline uint64_t ntfs_min64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

/* ---- S8: attribute search flags ---------------------------------------- */

#define NTFS_FIND_IC 0x1 /* compare names case-insensitively */

/* ---- S12, S12b: compressor and decompressor workspaces ------------------ */

/* The LZNT1 compressor's workspace (hash heads and chains), allocated by
 * the compressed-stream writer (S30) once per write. */
#define NTFS_LZNT1_HASH (1U << 12)

struct ntfs_lznt1_ws {
	uint16_t head[NTFS_LZNT1_HASH];
	uint16_t prev[NTFS_LZNT1_CHUNK];
};

/* Workspace sizes of the WOF decoders (S12b; checked against the
 * workspace structures there at compile time), allocated by the WOF fill
 * (S13). */
#define NTFS_XPRESS_WS_SIZE     4096u

#define NTFS_LZX_WS_SIZE        14336u

/* ---- S14: $UpCase ---------------------------------------------------------- */

static inline uint16_t ntfs_upc(ntfs_fs_t *fs, uint16_t c)
{
	return (fs->upcase && c < fs->upcase_len) ? fs->upcase[c] : c;
}

/* ---- S15: index nodes and entries ----------------------------------------- */

static inline const ntfs_index_header_t *ntfs_blk_hdr(const uint8_t *blk)
{
	return &((const ntfs_index_block_t *)blk)->index;
}

static inline const ntfs_index_entry_t *
ntfs_ie_at(const ntfs_index_header_t *h, uint32_t pos)
{
	return (const ntfs_index_entry_t *)((const uint8_t *)h + pos);
}

static inline int64_t ntfs_ie_child(const ntfs_index_entry_t *e)
{
	return (int64_t)ntfs_le64((const uint8_t *)e + e->length - 8);
}

static inline const uint8_t *ntfs_ie_key(const ntfs_index_entry_t *e)
{
	return (const uint8_t *)e + NTFS_IE_HDR;
}

/* $FILE_NAME key of an $I30 entry. */
static inline const ntfs_file_name_t *ntfs_ie_fn(const ntfs_index_entry_t *e)
{
	return (const ntfs_file_name_t *)ntfs_ie_key(e);
}

static inline const uint16_t *ntfs_fn_name(const ntfs_file_name_t *fn)
{
	return (const uint16_t *)((const uint8_t *)fn + NTFS_FN_HDR);
}

/* ---- S18a, S18b: the EA walk and the xattr name list -------------------- */

/* One EA as the validated walk (ntfs_ea_walk()) hands it to its
 * callback. */
typedef struct ntfs_ea_view {
	const char *name; /* NUL-terminated, inside the buffer */
	uint32_t name_len;
	const uint8_t *value;
	uint32_t value_len;
	uint8_t flags;
} ntfs_ea_view_t;

typedef int (*ntfs_ea_cb)(const ntfs_ea_view_t *ea, void *arg);

/* A listxattr result being built (ntfs_xlist_add(), ntfs_xlist_ea_cb()). */
struct ntfs_xlist {
	const ntfs_fs_t *fs;
	char *list;
	unsigned long size;
	unsigned long used;
	char name[264];
};

/* ===========================================================================
 * Support routines, by the section they serve
 * =========================================================================== */

/* S2: strings, allocation, UTF-16 <-> UTF-8, times */
unsigned long ntfs_strlen(const char *s);
int ntfs_streq(const char *a, const char *b);
int ntfs_strneq(const char *a, const char *b, unsigned long n);
void *ntfs_zalloc(unsigned long n);
void ntfs_free(void *p);
long ntfs_utf16_to_utf8(const uint16_t *s, uint32_t n, char *out,
			unsigned long cap);
long ntfs_utf8_to_utf16(const char *s, unsigned long len, uint16_t *out,
			uint32_t cap);
uint32_t ntfs_ascii_to_utf16(const char *s, uint16_t *out, uint32_t cap);
void ntfs_time_to_unix(int64_t t, uint64_t *sec, uint64_t *nsec);
void ntfs_memmove(void *dst, const void *src, unsigned long n);
int64_t ntfs_now(void);
int64_t ntfs_unix_to_time(int64_t sec, long nsec);
int ntfs_all_zero(const uint8_t *p, uint64_t n);

/* S3: the private reader/writer semaphore */
void ntfs_rwsem_init(ntfs_rwsem_t *sem, const char *name);
void ntfs_rwsem_read_lock(ntfs_rwsem_t *sem);
void ntfs_rwsem_read_unlock(ntfs_rwsem_t *sem);
void ntfs_rwsem_write_lock(ntfs_rwsem_t *sem);
void ntfs_rwsem_write_unlock(ntfs_rwsem_t *sem);
int ntfs_rwsem_held_excl(ntfs_rwsem_t *sem);
int ntfs_rwsem_release_owner(ntfs_rwsem_t *sem, uint64_t task_id);

/* S5: update sequence arrays */
int ntfs_mst_fixup(uint8_t *buf, uint32_t size);
int ntfs_mst_protect(uint8_t *buf, uint32_t size);

/* S6: runlists, the mapping-pairs decoder */
void ntfs_rl_free(ntfs_runlist_t *rl);
int ntfs_rl_append(ntfs_runlist_t *rl, int64_t vcn, int64_t lcn,
		   int64_t len);
int ntfs_rl_map(const ntfs_runlist_t *rl, int64_t vcn, int64_t *lcn,
		int64_t *left);
int ntfs_mp_decode(ntfs_fs_t *fs, const ntfs_attr_record_t *a,
		   uint32_t alen, ntfs_runlist_t *rl);

/* S26: runlist editing, the mapping-pairs encoder */
int ntfs_rl_copy(ntfs_runlist_t *dst, const ntfs_runlist_t *src);
uint64_t ntfs_rl_allocated(const ntfs_runlist_t *rl, int64_t v0,
			   int64_t v1);
int ntfs_rl_set(ntfs_runlist_t *rl, int64_t vcn, int64_t len,
		int64_t lcn);
int ntfs_rl_truncate(ntfs_runlist_t *rl, int64_t end_vcn);
uint32_t ntfs_mp_encode(const ntfs_runlist_t *rl, int64_t v0,
			int64_t v1, uint8_t *buf, uint32_t cap,
			int64_t *stop);

/* S24, S30: run arrays, the reservation list */
int ntfs_runs_add(ntfs_run_t **runs, uint32_t *n, uint32_t *cap,
		  int64_t lcn, int64_t len);
uint64_t ntfs_dl_cut(ntfs_stream_t *st, int64_t vcn, int64_t len);
int ntfs_dl_uncovered(ntfs_stream_t *st, int64_t v0, int64_t v1,
		      int add, uint64_t *count);

/* S7, S8: record and attribute validation */
int ntfs_mft_rec_check(ntfs_fs_t *fs, uint8_t *buf, uint64_t no);
int ntfs_attr_valid(ntfs_fs_t *fs, const uint8_t *rec, uint32_t aoff,
		    uint64_t no);
int ntfs_al_valid(ntfs_fs_t *fs, const uint8_t *al, uint32_t len,
		  uint64_t no);
int ntfs_name_match(ntfs_fs_t *fs, const uint16_t *name, uint32_t nlen,
		    const uint16_t *want, uint32_t wlen, int flags);
int ntfs_rec_verify(ntfs_fs_t *fs, const uint8_t *rec, uint64_t no);
ntfs_attr_record_t *ntfs_rec_attr(ntfs_fs_t *fs, uint8_t *rec,
				  uint64_t no, uint32_t type,
				  int by_instance, uint16_t instance,
				  int unnamed);

/* S12: LZNT1 */
long ntfs_lznt1_decompress(const uint8_t *in, uint64_t in_len,
			   uint8_t *out, uint64_t out_len);
uint32_t ntfs_lznt1_compress(const uint8_t *in, uint32_t valid,
			     uint8_t *out, uint32_t cap,
			     struct ntfs_lznt1_ws *w);

/* S12b: WOF decompressors */
int ntfs_xpress_decompress(const uint8_t *in, uint32_t in_len, uint8_t *out,
			   uint32_t out_len, void *ws);
int ntfs_lzx_decompress(const uint8_t *in, uint32_t in_len, uint8_t *out,
			uint32_t out_len, void *ws);

/* S14: name collation */
int ntfs_upcase_eq(ntfs_fs_t *fs, const uint16_t *a, const uint16_t *b,
		   uint32_t n);
int ntfs_collate_names(ntfs_fs_t *fs, const uint16_t *a, uint32_t alen,
		       const uint16_t *b, uint32_t blen, int ic_only);

/* S15: index node validation */
int ntfs_index_hdr_ok(const ntfs_index_header_t *h, uint32_t avail,
		      uint32_t min_entries);
int ntfs_node_entries_ok(ntfs_fs_t *fs, const ntfs_index_header_t *h,
			 uint32_t coll, uint32_t vcn_bits, int is_node,
			 uint64_t alloc_size);
int ntfs_index_root_check(ntfs_fs_t *fs, const uint8_t *root,
			  uint32_t rlen, uint64_t no);

/* S29: $FILE_NAME keys and $I30 entries */
uint8_t *ntfs_fn_build(uint64_t parent, const uint16_t *name,
		       uint32_t nlen, uint8_t ntype, int64_t crt,
		       int64_t mt, int64_t ct, int64_t at,
		       uint64_t alloc, uint64_t data, uint32_t fattr,
		       uint32_t ea_or_tag, uint32_t *len);
uint8_t *ntfs_ie_build(uint64_t mref, const uint8_t *fn, uint32_t fl);

/* S9, S16, S17: reparse data */
uint32_t ntfs_wof_parse(const uint8_t *rp, uint32_t rlen);
int ntfs_tag_is_link(uint32_t tag);
long ntfs_utf16_path(const uint16_t *s, uint32_t n, char *out,
		     unsigned long cap);
long ntfs_link_target(ntfs_fs_t *fs, const uint8_t *rp, uint32_t rlen,
		      uint32_t depth, char *out, unsigned long cap);
uint8_t ntfs_fn_dtype(const ntfs_file_name_t *fn);

/* S18a: the validated EA walk */
int ntfs_ea_walk(const uint8_t *buf, uint32_t qlen, ntfs_ea_cb cb,
		 void *arg);
int ntfs_ea_find(const uint8_t *buf, uint32_t qlen, const char *name,
		 ntfs_ea_view_t *out);

/* S18b: xattr names */
int ntfs_ea_reserved(const char *name, uint32_t nl);
int ntfs_has_xattr_prefix(const char *name, uint32_t nl);
int ntfs_xlist_add(struct ntfs_xlist *x, const char *n, uint32_t len);
int ntfs_xlist_ea_cb(const ntfs_ea_view_t *ea, void *arg);

/* S19, S29b, S31, S32: security descriptors, POSIX ACLs */
int ntfs_sd_check(const uint8_t *sd, uint32_t len);
uint32_t ntfs_sd_hash(const uint8_t *sd, uint32_t len);
int ntfs_sd_valid(const uint8_t *sd, uint32_t len);
uint8_t *ntfs_sd_default(uint32_t *len);
int ntfs_pacl_valid(const uint8_t *v, uint32_t len, int allow_empty,
		    int *equiv);
uint32_t ntfs_pacl_mode(uint8_t *v, uint32_t len);
void ntfs_pacl_apply_mode(uint8_t *v, uint32_t len, uint32_t mode,
			  int and);

/* S31: name checks */
int ntfs_win_name_ok(const uint16_t *u, uint32_t n);
int ntfs_stream_name_ok(const uint16_t *name, uint32_t nlen);

/* S22, S23: option values, boot sector, $LogFile restart pages */
uint32_t ntfs_log_seq_bits(uint64_t size);
int ntfs_opt_num(const char *v, unsigned base, uint64_t *out);
int ntfs_opt_bool(const char *v, int *out);
uint64_t ntfs_rec_size(int8_t c, uint32_t cluster);
uint64_t ntfs_boot_cluster(uint32_t bps, uint8_t spc);
int ntfs_boot_sane(const uint8_t *b);
int ntfs_rstr_check(const uint8_t *buf, uint32_t len, uint32_t *psize,
		    int *clean, int64_t *lsn);

#endif /* NTFS_INTERNAL */

#endif /* LIKEOS_NTFS_H */
