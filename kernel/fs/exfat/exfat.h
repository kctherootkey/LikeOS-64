/* LikeOS - exFAT filesystem driver (read-write)
 *
 * exFAT revision 1.x volumes: boot region with checksums, one FAT plus the
 * allocation bitmap, NoFatChain (contiguous) files, 64-bit sizes and large
 * volumes, UTF-16 names folded through the on-disk upcase table, entry sets
 * with checksum and NameHash, VolumeDirty, discard.
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
 *   latches the mount read-only (exFAT: per `errors=').
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
 * exFAT specifics
 * ===========================================================================
 *
 * Allocation happens in write() (alloc_blocks stays NULL): a write first
 * extends the cluster chain (NoFatChain kept while the run stays contiguous,
 * converted to a FAT chain otherwise), then dirties pages that already map.
 * Writes beyond ValidDataLength advance it first (zeroing the gap through
 * the cache); ValidDataLength is kept on a page boundary (or at the size)
 * by everything this driver writes, so only files written elsewhere can
 * have a NOFILL page (the one straddling an unaligned ValidDataLength).
 *
 * Metadata: the FAT region and the allocation bitmap do not go through the
 * page cache (the handles EXFAT_HANDLE_FAT / _BITMAP stay reserved and
 * unused there).  They live in the driver's bounded metadata cache --
 * units of min(cluster, 4 KB), each inside its region, a dirty bit per
 * device sector -- which only the commit writes, so the order in which
 * allocation state reaches the device is the driver's, not background
 * writeback's.  The upcase table (EXFAT_HANDLE_UPCASE) is read once at
 * mount.  Directory pages ARE page-cache pages of their directory stream,
 * but they are never marked dirty: changed entry sets are queued (pages
 * held) and written by the commit.
 *
 * Commit order (crash consistency without a journal): data of streams whose
 * size grows (a set never describes data that is not stored: a write error
 * keeps the old set, the stream stays dirty, fsync / close / sync report
 * EIO) -> bitmap allocations -> device sync -> FAT links (zeroed root
 * clusters before the link that is the root's length) -> device sync ->
 * entry sets -> device sync -> frees: truncation end marks in the FAT ->
 * device sync -> bitmap bits cleared -> device sync -> discard.  Every
 * namespace operation commits before it returns; write() defers the
 * entry-set update to close / fsync / sync / the next commit.  A commit
 * never runs inside a commit.  An allocation that fails half way is undone
 * completely (the stream untouched, the clusters given back).
 *
 * Entry sets are placed so that one device write stores them whole: a set
 * of up to 16 entries inside one 512-byte unit, a longer one starting on
 * one, none across a page, the sectors under it consecutive (the slots
 * between the last entry and such a set become unused entries).  A stop at
 * any point leaves at most lost clusters -- and, while a stream that is
 * already a FAT chain grows, possibly a chain longer than its DataLength
 * (the FAT link is stored before the entry set with the new length; fsck
 * trims the tail, nothing is lost) -- never a torn set, a cluster that two
 * files own or a file that names a free one.
 *
 * Arming: an operation that is allowed to change the volume only permits
 * writing; the chokepoint sets VolumeDirty (and syncs) right before the
 * first real write, so an operation that ends up changing nothing (a
 * failing or no-op rename, utimens omit/omit, O_TRUNC of an empty file)
 * writes nothing.  The first permission also checks that the bitmap marks
 * the root directory, the bitmap and the upcase table used (else read-only).
 * Every read-only reason is kept; `force' lifts only the dirty one.
 *
 * User memory is never touched with a filesystem lock held (reads copy from
 * held pages after unlocking, writes copy into a kernel buffer before
 * locking, readdir assembles its records in a kernel buffer), and shared
 * holds of the driver's rwsems are recorded per task, so
 * release_locks_for_task() gives back a dead task's shared holds too.
 *
 * Inode numbers (st_ino): the first cluster of the file or directory, the
 * root directory included.  A file with no cluster (empty) reports
 * EXFAT_INO_POS(first cluster of its directory, index of its File entry in
 * that directory); it changes when the file gets its first cluster, which is
 * harmless (the caches key streams by handle, not by st_ino).
 *
 * Names: case-preserving, case-insensitive: lookups compare names folded
 * through the volume's upcase table (loaded and checksum-verified at mount;
 * the built-in default table, and a read-only mount, when it is missing or
 * damaged), dcache *_cs API with keys
 * folded the same way.  Every created or changed entry set gets a fresh
 * SetChecksum and NameHash.
 *
 * Times: 2-second timestamps plus the 10 ms increments and the UTC offset
 * byte; written in UTC with the offset marked valid.
 *
 * Mount options (comma separated, vfs_next_option()):
 *   uid=, gid=          owner of every file (default 0, 0)
 *   umask=, fmask=, dmask=   permission masks (default 022); a file with
 *                       the read-only attribute reports no write bits
 *   allow_utime=        accepted and shown; who may set times is the VFS's
 *                       ownership rule
 *   iocharset=utf8      names are UTF-8 (the only character set; utf8 too)
 *   errors=remount-ro | continue | panic   reaction to corruption found
 *                       while mounted (default remount-ro)
 *   force               mount read-write although VolumeDirty is set (the
 *                       volume then stays marked dirty)
 *   discard / nodiscard TRIM freed clusters (after the cleared bitmap
 *                       bits are on the device) / do not (default)
 *   time=local | utc    timestamps without a valid UTC offset are local
 *                       time / UTC (default); with time=local new ones are
 *                       written in local time with their offset
 *   time_offset=MIN     local time's distance from UTC in minutes
 *                       (multiples of 15; implies time=local)
 * Unknown options and the generic flags are accepted without effect.
 * show_options() prints uid, gid, fmask, dmask, iocharset, errors and the
 * others when set.
 *
 * Without uid=/gid=/umask=/fmask=/dmask= every user may access everything
 * (the permission op allows it, as on FAT) except writing a file with the
 * read-only attribute; with any of them the generic mode check decides.
 */

#ifndef LIKEOS_EXFAT_H
#define LIKEOS_EXFAT_H

#include <kernel/uapi/status.h>
#include <kernel/dev/block/block.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>

/* ---- Block ids (section 1) ---------------------------------------------- */
#define EXFAT_BID_TAG (1UL << 63)
#define EXFAT_BID_HANDLE_SHIFT 38
#define EXFAT_BID_HANDLE_BITS 24
#define EXFAT_BID_INDEX_MASK ((1UL << EXFAT_BID_HANDLE_SHIFT) - 1)
#define EXFAT_HANDLE_MAX ((1UL << EXFAT_BID_HANDLE_BITS) - 1)
#define EXFAT_BID_ENC(h, idx)                                            \
	(EXFAT_BID_TAG |                                                 \
	 (((unsigned long)(h) & EXFAT_HANDLE_MAX) << EXFAT_BID_HANDLE_SHIFT) | \
	 ((unsigned long)(idx) & EXFAT_BID_INDEX_MASK))
#define EXFAT_BID_HANDLE(b) \
	(((unsigned long)(b) >> EXFAT_BID_HANDLE_SHIFT) & EXFAT_HANDLE_MAX)
#define EXFAT_BID_INDEX(b) ((unsigned long)(b) & EXFAT_BID_INDEX_MASK)
#define EXFAT_BID_IS(b) \
	(((unsigned long)(b) & (EXFAT_BID_TAG | (1UL << 62))) == EXFAT_BID_TAG)
/* Every encoded id is below the marker (bit 62 is clear in all of them). */
#define EXFAT_EOC_MARKER 0xC000000000000000UL
#define EXFAT_BID_EOC 0xFFFFFFFFFFFFFFFFUL

/* Reserved stream handles: the volume's own metadata streams.  Handles of
 * files and directories (the root directory included) start at
 * EXFAT_HANDLE_FIRST_DYNAMIC. */
#define EXFAT_HANDLE_FAT 1 /* the FAT region (first FAT) */
#define EXFAT_HANDLE_BITMAP 2 /* the allocation bitmap */
#define EXFAT_HANDLE_UPCASE 3 /* the upcase table (read at mount) */
#define EXFAT_HANDLE_FIRST_DYNAMIC 16

/* st_ino of a file without clusters: bit 62 marks it (cluster numbers are
 * 32-bit), then its directory's first cluster and its entry index. */
#define EXFAT_INO_POS(dir_cluster, entry_index)                 \
	((1UL << 62) | (((unsigned long)(dir_cluster) & 0xFFFFFFFFUL) << 24) | \
	 ((unsigned long)(entry_index) & 0xFFFFFFUL))

/* Exposed block size never exceeds a page (section 1). */
#define EXFAT_MAX_EXPOSED_BLOCK 4096UL

/* ===========================================================================
 * On-disk format (exFAT file system specification, revision 1.00)
 * ===========================================================================
 *
 * All multi-byte fields are little-endian and read with byte loads (no
 * structure overlays: nothing on disk is trusted to be aligned).
 *
 * Volume layout, in file-system sectors of 2^BytesPerSectorShift bytes:
 *
 *   0 .. 11     main boot region: boot sector, 8 extended boot sectors,
 *               OEM parameters, a reserved sector, the boot checksum sector
 *   12 .. 23    backup boot region (same layout)
 *   FatOffset   the FAT (NumberOfFats copies of FatLength sectors)
 *   ClusterHeapOffset   cluster 2, 3, ... ClusterCount + 1
 *
 * The root directory, the allocation bitmap and the upcase table live in
 * the cluster heap; the latter two are described by critical primary
 * entries of the root directory. */

/* ---- Boot sector field offsets ------------------------------------------ */
#define EXFAT_BS_JUMP 0 /* EB 76 90 */
#define EXFAT_BS_FSNAME 3 /* "EXFAT   " */
#define EXFAT_BS_MBZ 11 /* 53 zero bytes (legacy BPB) */
#define EXFAT_BS_MBZ_LEN 53
#define EXFAT_BS_PART_OFFSET 64 /* u64, informational only */
#define EXFAT_BS_VOL_LENGTH 72 /* u64, in fs sectors */
#define EXFAT_BS_FAT_OFFSET 80 /* u32, fs sectors */
#define EXFAT_BS_FAT_LENGTH 84 /* u32, fs sectors (one FAT) */
#define EXFAT_BS_HEAP_OFFSET 88 /* u32, fs sectors */
#define EXFAT_BS_CLUSTER_COUNT 92 /* u32 */
#define EXFAT_BS_ROOT_CLUSTER 96 /* u32 */
#define EXFAT_BS_SERIAL 100 /* u32 */
#define EXFAT_BS_REVISION 104 /* u16: high byte major, low byte minor */
#define EXFAT_BS_FLAGS 106 /* u16 VolumeFlags (not checksummed) */
#define EXFAT_BS_BPS_SHIFT 108 /* u8, 9 .. 12 */
#define EXFAT_BS_SPC_SHIFT 109 /* u8, 0 .. 25 - BytesPerSectorShift */
#define EXFAT_BS_NFATS 110 /* u8, 1 or 2 (TexFAT) */
#define EXFAT_BS_DRIVE 111 /* u8 */
#define EXFAT_BS_PERCENT 112 /* u8 PercentInUse (not checksummed) */
#define EXFAT_BS_SIGNATURE 510 /* 55 AA */

/* VolumeFlags bits. */
#define EXFAT_VF_ACTIVE_FAT 0x0001u
#define EXFAT_VF_DIRTY 0x0002u
#define EXFAT_VF_MEDIA_FAILURE 0x0004u
#define EXFAT_VF_CLEAR_TO_ZERO 0x0008u

/* PercentInUse value meaning "not available". */
#define EXFAT_PERCENT_UNKNOWN 0xFFu

/* Boot regions, in fs sectors. */
#define EXFAT_BOOT_REGION_SECTORS 12
#define EXFAT_BOOT_CHECKSUM_SECTOR 11
#define EXFAT_BACKUP_BOOT_FIRST 12
/* Extended boot sectors end in this 32-bit signature. */
#define EXFAT_EXT_BOOT_SIGNATURE 0xAA550000u

/* ---- FAT entries ---------------------------------------------------------- */
#define EXFAT_FAT_FREE 0x00000000u
#define EXFAT_FAT_BAD 0xFFFFFFF7u
#define EXFAT_FAT_EOC 0xFFFFFFFFu
#define EXFAT_FAT_MEDIA 0xFFFFFFF8u /* FAT[0] */
#define EXFAT_FIRST_CLUSTER 2u
#define EXFAT_MAX_CLUSTER_COUNT 0xFFFFFFF5u

/* ---- Directory entries (32 bytes each) ---------------------------------- */
#define EXFAT_DENTRY_SIZE 32u
#define EXFAT_DENTRY_SHIFT 5
/* EntryType byte: bit 7 InUse, bit 6 TypeCategory (secondary), bit 5
 * TypeImportance (benign), bits 0..4 TypeCode.  0x00 is the end-of-
 * directory marker; any other value with InUse clear is an unused entry. */
#define EXFAT_ET_EOD 0x00u
#define EXFAT_ET_INUSE 0x80u
#define EXFAT_ET_SECONDARY 0x40u
#define EXFAT_ET_BENIGN 0x20u
#define EXFAT_ET_CODE_MASK 0x1Fu
#define EXFAT_ET_BITMAP 0x81u /* allocation bitmap      (critical primary) */
#define EXFAT_ET_UPCASE 0x82u /* upcase table           (critical primary) */
#define EXFAT_ET_LABEL 0x83u /* volume label           (critical primary) */
#define EXFAT_ET_FILE 0x85u /* file / directory       (critical primary) */
#define EXFAT_ET_GUID 0xA0u /* volume GUID            (benign primary)   */
#define EXFAT_ET_TEXFAT 0xA1u /* TexFAT padding         (benign primary)   */
#define EXFAT_ET_ACT 0xA2u /* access control table   (benign primary)   */
#define EXFAT_ET_STREAM 0xC0u /* stream extension       (critical second.) */
#define EXFAT_ET_NAME 0xC1u /* file name              (critical second.) */
#define EXFAT_ET_VENDOR_EXT 0xE0u /* vendor extension    (benign secondary) */
#define EXFAT_ET_VENDOR_ALLOC 0xE1u /* vendor allocation (benign secondary) */

/* Generic primary / secondary fields. */
#define EXFAT_DE_TYPE 0
#define EXFAT_DE_SECONDARY_COUNT 1 /* primary: u8 */
#define EXFAT_DE_SET_CHECKSUM 2 /* primary: u16 */
#define EXFAT_DE_GSFLAGS 1 /* secondary: GeneralSecondaryFlags */
#define EXFAT_DE_FIRST_CLUSTER 20 /* u32 (generic layout) */
#define EXFAT_DE_DATA_LENGTH 24 /* u64 (generic layout) */

/* File directory entry (0x85). */
#define EXFAT_FILE_ATTR 4 /* u16 FileAttributes */
#define EXFAT_FILE_CTIME 8 /* u32 CreateTimestamp */
#define EXFAT_FILE_MTIME 12 /* u32 LastModifiedTimestamp */
#define EXFAT_FILE_ATIME 16 /* u32 LastAccessedTimestamp */
#define EXFAT_FILE_CTIME_10MS 20 /* u8 0..199 */
#define EXFAT_FILE_MTIME_10MS 21 /* u8 0..199 */
#define EXFAT_FILE_CTIME_UTC 22 /* u8 UtcOffset */
#define EXFAT_FILE_MTIME_UTC 23
#define EXFAT_FILE_ATIME_UTC 24

/* FileAttributes. */
#define EXFAT_ATTR_RO 0x0001u
#define EXFAT_ATTR_HIDDEN 0x0002u
#define EXFAT_ATTR_SYSTEM 0x0004u
#define EXFAT_ATTR_DIR 0x0010u
#define EXFAT_ATTR_ARCHIVE 0x0020u
#define EXFAT_ATTR_VALID 0x0037u /* the defined bits */

/* Stream extension entry (0xC0). */
#define EXFAT_STREAM_NAME_LEN 3 /* u8 NameLength */
#define EXFAT_STREAM_NAME_HASH 4 /* u16 */
#define EXFAT_STREAM_VDL 8 /* u64 ValidDataLength */
#define EXFAT_STREAM_FIRST_CLUSTER 20
#define EXFAT_STREAM_DATA_LENGTH 24

/* GeneralSecondaryFlags. */
#define EXFAT_SF_ALLOC_POSSIBLE 0x01u
#define EXFAT_SF_NO_FAT_CHAIN 0x02u

/* File name entry (0xC1): 15 UTF-16 code units at offset 2. */
#define EXFAT_NAME_CHARS_OFF 2
#define EXFAT_NAME_PER_ENTRY 15u

/* Allocation bitmap entry (0x81) and upcase table entry (0x82). */
#define EXFAT_BITMAP_FLAGS 1 /* bit 0: second bitmap (TexFAT) */
#define EXFAT_UPCASE_CHECKSUM 4 /* u32 TableChecksum */

/* Volume label entry (0x83). */
#define EXFAT_LABEL_COUNT 1 /* u8 CharacterCount, <= 11 */
#define EXFAT_LABEL_CHARS 2
#define EXFAT_LABEL_MAX 11

/* UtcOffset byte: bit 7 OffsetValid, bits 0..6 a signed count of 15
 * minute steps (-48 .. +56). */
#define EXFAT_UTC_VALID 0x80u

/* ---- Limits ---------------------------------------------------------------- */
#define EXFAT_NAME_MAX 255u /* UTF-16 code units */
#define EXFAT_SET_MIN_SECONDARY 2u /* stream + one name entry */
#define EXFAT_SET_MAX_SECONDARY 18u /* stream + 17 name entries */
#define EXFAT_SET_MAX_ENTRIES 19u
#define EXFAT_SET_MAX_BYTES (EXFAT_SET_MAX_ENTRIES * EXFAT_DENTRY_SIZE)
#define EXFAT_MAX_DIR_BYTES (256UL << 20) /* a directory's DataLength */
#define EXFAT_UPCASE_MAX_BYTES (128UL << 10) /* 65536 UTF-16 code units */

/* statfs f_type. */
#define EXFAT_STATFS_MAGIC 0x2011BAB0UL

/* The filesystem type mount(2) instantiates for "exfat". */
extern const vfs_fstype_t exfat_fstype;

#endif /* LIKEOS_EXFAT_H */
