/* LikeOS - ISO9660 filesystem driver (read-only)
 *
 * ISO9660 levels 1-3 with multi-extent files, Rock Ridge (PX PN SL NM CL PL
 * RE TF SF, CE continuation areas), Joliet (UCS-2 / UTF-16 names), zisofs,
 * 2048-byte optical devices and ISO images on 512-byte disks, hybrid images.
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
 * ISO9660 specifics
 * ===========================================================================
 *
 * Read-only: mount() marks the mount VFS_MS_RDONLY (m->flags) whatever was
 * asked, every mutating operation answers ST_ROFS, and there is no write
 * chokepoint to arm -- the guard block device's .write refuses everything
 * with WARN_RATELIMIT, and nothing in the driver calls a device write.
 * alloc_blocks stays NULL; write_inode refuses (nothing is ever dirty).
 *
 * Volume choice: Rock Ridge on the primary volume when the root's "."
 * record starts with SUSP SP and carries Rock Ridge entries or an RRIP ER
 * (unless `norock'), else Joliet (unless `nojoliet'), else the enhanced
 * (ISO 9660:1999) tree, else plain ISO9660 names (map=normal: ";1" version
 * suffix dropped, trailing dot of an extensionless name dropped, shown
 * lower-cased; map=off: as recorded).  A Rock Ridge volume falls back to
 * the plain name (and the uid=/gid=/mode=/dmode= attributes) per record
 * where a record has no NM (PX).
 *
 * The guard block device: geometry and the LBA shift.  The page cache
 * reads block_to_lba() == 0 as a hole (zeros), but an ISO file may start
 * at logical block 0 (hybrid images).  So guard LBA k is "view" sector
 * k - 1 and block_to_lba() of logical block b is 1 + b * spb; guard LBA 0
 * is never a data sector.  The view sector size `vss' is min(device sector,
 * logical block): a device with sectors larger than the logical block (an
 * image of 2048-byte blocks on a 4096-byte-sector disk, 512-byte blocks on
 * an optical drive) is presented as one of logical-block-sized sectors,
 * the guard reading the covering real sectors through a bounce buffer.  So
 * ISO9660 mounts on 512-, 1024-, 2048- and 4096-byte sector devices (the
 * "sector size exceeds the exposed block size" refusal of section 1 does
 * not apply to it).
 *
 * Streams: reserved handle ISOFS_HANDLE_VOLUME is the whole device (block
 * N = logical block N); it carries the driver's own metadata reads (System
 * Use continuation areas, directory "." records, zisofs headers, pointer
 * tables and compressed data, the bytes of non-aligned streams).  Every
 * file and directory opened or walked through gets a dynamic handle; the
 * streams nobody references wait on an LRU and are evicted (pages and
 * inode-cache entry forgotten first, metadata lock exclusive) beyond a
 * keep limit.  NOFILL streams: zisofs files (a read decodes the whole
 * zisofs block and installs all its pages), multi-extent files whose
 * non-final extents do not end on a block boundary (pages assembled byte
 * by byte from the extents), Rock Ridge SF sparse files (unreadable: EIO).
 * Interleaved files (file unit / gap) and extended attribute records are
 * mapped block by block (the cache's per-block read).
 *
 * Inode numbers (st_ino): a directory reports the LBA of its data (its
 * extent plus the XAR length -- equal to the extent in practice -- so its
 * own "." record, its parent's record and a Rock Ridge CL relocation
 * agree); any other object reports ISOFS_INO_FILE(byte offset on the
 * volume of its directory record -- the FIRST record of a multi-extent
 * file).  Rock Ridge PX serial numbers are not used for st_ino.  RE-marked
 * relocated directories are hidden at their relocated place.
 *
 * Directories are read through the page cache (one stream per directory);
 * records never straddle a logical block, a record that would is corrupt
 * (skipped with the rest of its block, and no negative dentry is made from
 * a scan that skipped something, or met System Use data it could not read
 * completely -- such a lookup answers EIO).  The dentry cache (_cs
 * interface) is keyed by the DIRECTORY'S st_ino (tagged) rather than its
 * chain id (stable across stream eviction) and the exact name; its value
 * is the record's offset in the directory.  Names match exactly; only
 * plain ISO 9660 names (no Rock Ridge / Joliet / 9660:1999 tree) with
 * check=relaxed fall back to a case-blind match when nothing matches
 * exactly.  Nothing whose System Use data was damaged is cached, readdir
 * only adds names nobody has recorded, and a directory scan has a work
 * budget for System Use data (base + per record; EIO beyond it).
 *
 * readdir parses only from positions it handed out (0, or where the last
 * call left off); any other position (lseek / seekdir) is first moved to
 * the record-set start at or after it.  Entries are assembled in a kernel
 * buffer under the metadata lock and copied out after it is released: no
 * user memory is touched while the lock is held, and shared holds are
 * recorded per task so release_locks_for_task gives back a dead task's
 * shared holds as well as an exclusive one.  The st_ino of a file is the
 * real volume offset of its record, through the directory's extent and
 * interleave mapping.  Unused streams are bounded in number and in the
 * bytes of their extent arrays.
 *
 * Mount options (comma separated, vfs_next_option()):
 *   uid=, gid=          owner when Rock Ridge gives none (default 0, 0)
 *   mode=, dmode=       permissions of files / directories without Rock
 *                       Ridge (default 0444 / 0555)
 *   overriderockperms   uid=/gid=/mode=/dmode= even where Rock Ridge has PX
 *   norock              ignore Rock Ridge
 *   nojoliet            ignore Joliet
 *   check=relaxed | strict   relaxed (default): plain ISO names that match
 *                       nothing exactly match without case; strict: exact
 *                       names only, and both-endian numbers of descriptors
 *                       and records must agree
 *   map=normal | off    plain-name mapping (above); acorn = normal
 *   block=512|1024|2048 logical block size instead of the descriptor's
 *   session=N           the N-th session (searched after the end of the
 *                       previous one; images without a table of contents)
 *   sbsector=N          the descriptor set at 2048-byte sector N + 16
 *   hide                hidden (existence bit) and associated files out
 *   unhide, showassoc   associated files shown as well (default: hidden
 *                       files shown, associated ones not)
 *   cruft               file lengths masked to 24 bits (broken masters)
 *   nocompress          zisofs files shown as recorded (compressed)
 *   iocharset=utf8      accepted (names are always UTF-8)
 * and the generic ro.  show_options() prints the non-default ones.
 * statfs: f_type ISOFS_SUPER_MAGIC (0x9660), no free blocks.
 */

#ifndef LIKEOS_ISOFS_H
#define LIKEOS_ISOFS_H

#include <kernel/uapi/status.h>
#include <kernel/dev/block/block.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>

/* ---- Block ids (section 1) ---------------------------------------------- */
#define ISOFS_BID_TAG (1UL << 63)
#define ISOFS_BID_HANDLE_SHIFT 38
#define ISOFS_BID_HANDLE_BITS 24
#define ISOFS_BID_INDEX_MASK ((1UL << ISOFS_BID_HANDLE_SHIFT) - 1)
#define ISOFS_HANDLE_MAX ((1UL << ISOFS_BID_HANDLE_BITS) - 1)
#define ISOFS_BID_ENC(h, idx)                                            \
	(ISOFS_BID_TAG |                                                 \
	 (((unsigned long)(h) & ISOFS_HANDLE_MAX) << ISOFS_BID_HANDLE_SHIFT) | \
	 ((unsigned long)(idx) & ISOFS_BID_INDEX_MASK))
#define ISOFS_BID_HANDLE(b) \
	(((unsigned long)(b) >> ISOFS_BID_HANDLE_SHIFT) & ISOFS_HANDLE_MAX)
#define ISOFS_BID_INDEX(b) ((unsigned long)(b) & ISOFS_BID_INDEX_MASK)
#define ISOFS_BID_IS(b) \
	(((unsigned long)(b) & (ISOFS_BID_TAG | (1UL << 62))) == ISOFS_BID_TAG)
/* Every encoded id is below the marker (bit 62 is clear in all of them). */
#define ISOFS_EOC_MARKER 0xC000000000000000UL
#define ISOFS_BID_EOC 0xFFFFFFFFFFFFFFFFUL

/* Reserved handle: the whole device as one stream (block N = logical
 * block N), for the driver's own metadata reads.  Files and directories
 * get handles from ISOFS_HANDLE_FIRST_DYNAMIC on. */
#define ISOFS_HANDLE_VOLUME 1
#define ISOFS_HANDLE_FIRST_DYNAMIC 16

/* statfs f_type. */
#define ISOFS_SUPER_MAGIC 0x9660

/* st_ino of a non-directory: bit 44 above any byte offset of a volume of
 * 2^32 logical blocks of 2048 bytes (2^43), so it never equals a directory's
 * extent LBA (< 2^32). */
#define ISOFS_INO_FILE(record_byte_offset) \
	((1UL << 44) | ((unsigned long)(record_byte_offset) & ((1UL << 44) - 1)))

/* The primary volume descriptor's place: logical sector 16 of 2048 bytes. */
#define ISOFS_VD_START_BYTE 32768UL
#define ISOFS_VD_SIZE 2048UL

/* The filesystem type mount(2) instantiates for "iso9660" (alias "isofs"). */
extern const vfs_fstype_t isofs_fstype;

#endif /* LIKEOS_ISOFS_H */
