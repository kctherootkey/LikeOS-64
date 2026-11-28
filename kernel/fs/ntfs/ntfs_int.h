/* LikeOS - NTFS filesystem driver: declarations shared between its files
 *
 * Conventions shared with the exFAT and ISO9660 drivers, locking, write
 * safety, mount options and the on-disk format: ntfs.h.
 *
 * Copyright (C) 2026 The LikeOS Project
 *
 * ===========================================================================
 * Internal API overview
 * ===========================================================================
 *
 * The driver is organised in numbered sections; each starts with a banner
 * and a comment that states its contract.  The files of kernel/fs/ntfs/
 * and the sections they hold:
 *
 *  super.c     S22 mount, remount, unmount, option values, boot sector
 *              decoding, $LogFile restart pages; $Volume flags and arming
 *              (S23); statfs, sync, fsync, the clean-unmount sequence and
 *              the VFS operation table (S32)
 *  misc.c      S2 strings, allocation, NTFS times; S3 locking (the private
 *              rwsem, the lock instances); S23 write-side rules, errors
 *  bdev.c      S4 device I/O, the write chokepoint, the guard device; S23
 *              raw writes through a runlist, metadata write-through
 *  mft.c       S5 fixups; S7 MFT records, record validation; S23 record
 *              and index cache stores, MFT record writes; S25 MFT record
 *              allocation
 *  attrib.c    S8 attributes, attribute validation; S27 editable inodes
 *  runlist.c   S6 runlists and mapping-pairs decoding; S26 runlist editing
 *              and the mapping-pairs encoder
 *  inode.c     S9 streams and the handle table; S16 inodes; stat and inode
 *              flags (S32)
 *  aops.c      S10 page-cache superblock operations; S11 the iomap layer;
 *              S13 stream data reads and readahead
 *  compress.c  S12 LZNT1; S12b the WOF decompressors (XPRESS, LZX)
 *  unistr.c    UTF-16 <-> UTF-8 (S2); S14 $UpCase and name collation
 *  index.c     S15 indexes, node validation; S29 index entries and B+tree
 *              changes
 *  dir.c       S20 directory lookup and path resolution; readdir (S32)
 *  ea.c        S18a the validated $EA walk, loading EAs; S28 the EA writer
 *  xattr.c     S18b xattr names; the volume label, user.* / system.* /
 *              security.* xattrs and the stream view (S32)
 *  reparse.c   reparse data (S9, S16, S17); S17 reparse points and link
 *              targets
 *  security.c  descriptor and ACL validation; S19 security descriptors;
 *              S29b the $Secure writer
 *  lcnalloc.c  run arrays and the reservation list; S24 the cluster
 *              allocator, discard and trim
 *  file.c      S30 stream data writes, truncate, direct I/O; S21 / S32
 *              open, close, read, write, seek
 *  namei.c     name checks; S31 namespace operations and their VFS entries
 *              (S32), readlink, chdir
 *  logfile.c   S23 $LogFile reset and the clean log; S33 $LogFile replay
 *
 * The in-memory structures (S1) and the inline helpers are in the
 * driver-internal section of ntfs.h; the declarations shared between the
 * files are in this header.  The layers, bottom up:
 *
 *  S2  helpers         ntfs_le*, overflow-checked arithmetic, UTF-16 <->
 *                      UTF-8 (ntfs_utf16_to_utf8 / ntfs_utf8_to_utf16, WTF-8
 *                      for unpaired surrogates), NTFS time conversion.
 *  S3  locking         ntfs_rwsem_* (private recursive rwsem, ext4's design),
 *                      ntfs_meta_rlock/runlock (SHARED), ntfs_meta_wlock/
 *                      wunlock (EXCLUSIVE), ntfs_flock_shared/excl (per-file
 *                      content fence, hashed by MFT record number),
 *                      ntfs_alloc_lock (allocator mutex),
 *                      ntfs_meta_held_excl(), ntfs_assert_meta_*().
 *  S4  device I/O      ntfs_dev_read() (the only device read helper, LBAs in
 *                      DEVICE sectors), ntfs_dev_write()/ntfs_dev_discard()
 *                      (THE write chokepoint: refuses unless fs->armed), the
 *                      guard block device the page cache sees.
 *  S5  fixups          ntfs_mst_fixup() (validate + undo the update sequence
 *                      array of a FILE/INDX/RSTR record), ntfs_mst_protect()
 *                      (the inverse, for the writers).
 *  S6  runlists        ntfs_rl_* : decoded mapping pairs as a sorted array of
 *                      {vcn, lcn, len} with binary search; ntfs_mp_decode()
 *                      validates every run.
 *  S7  MFT records     ntfs_mft_read(fs, no, buf): a validated, fixed-up copy
 *                      of record `no' (record cache, LRU, ~1024 entries);
 *                      ntfs_mft_read_raw() for the mount bootstrap.
 *  S8  attributes      ntfs_attr_valid() (THE attribute validator), the
 *                      search context ntfs_actx_* (base record + attribute
 *                      list + extension records): ntfs_actx_open(),
 *                      ntfs_actx_find(type, name), ntfs_actx_close();
 *                      ntfs_attr_readall() (whole small attribute, uncached).
 *  S9  streams         the handle table: ntfs_stream_get(fs, mft, type, name)
 *                      / ntfs_stream_put(); a stream = (MFT reference,
 *                      attribute type, name) with sizes, flags, runlist and
 *                      (resident) value.  Chain id NTFS_BID_ENC(h, 0) keys
 *                      the page cache, the inode cache and (directories) the
 *                      dentry cache.  Reserved handles for $MFT, $MFT:$BITMAP,
 *                      $Bitmap, $LogFile, $Secure:$SDS.
 *  S10 sb ops          block_at / next_block / block_to_lba / map_block /
 *                      lock_map / lock_io / write_inode / alloc_blocks
 *                      (delayed allocation, S30).
 *  S11 iomap           ntfs_iomap_begin(fs, st, pos, len, flags, &map) /
 *                      ntfs_iomap_end(): byte range -> MAPPED / HOLE /
 *                      DELALLOC / UNWRITTEN / INLINE / COMPRESSED / ENCRYPTED
 *                      extent.  Used by the read path, SEEK_DATA/SEEK_HOLE
 *                      writeback and direct I/O.
 *  S12 LZNT1           ntfs_lznt1_decompress().
 *  S13 stream data     ntfs_stream_page() (one held page of a stream, NOFILL
 *                      pages filled through pagecache_install()),
 *                      ntfs_stream_pread() (byte range -> kernel or user
 *                      buffer), readahead.
 *  S14 collation       $UpCase, ntfs_collate_names(), ntfs_name_equal_ic().
 *  S15 indexes         ntfs_index_open/close, ntfs_ib_read() (INDX blocks,
 *                      validated, index cache), ntfs_index_lookup(),
 *                      ntfs_index_next() (readdir order, resume after a key).
 *  S16 inodes          ntfs_inode_load() -> ntfs_inode_t (times, sizes, link
 *                      count, reparse tag, EA/WSL data), ntfs_stat_fill().
 *  S17 reparse points  ntfs_reparse_read(), ntfs_link_target().
 *  S18 EAs + xattrs    ntfs_ea_* (validated $EA walk), WSL $LX* values,
 *                      user./system. xattr mapping, ADS xattr view.
 *  S19 security        $Secure ($SII lookup, $SDS read), own descriptors.
 *  S20 path lookup     ntfs_dir_lookup() (dcache _cs incl. negative
 *                      entries), ntfs_resolve() (iterative, links through
 *                      vfs_link_hop(), "name:stream" suffixes).
 *  S21 VFS ops         open/read/read_at/seek/readdir/close/stat/lstat/
 *                      fstat/readlink/chdir/statfs/xattrs/inode_flags/
 *                      fsync/sync/release_locks_for_task.
 *  S22 mount           options, boot sector, $MFT bootstrap, $MFTMirr,
 *                      $Volume, $AttrDef, $UpCase, $LogFile check,
 *                      hibernation, $Secure, $Extend, $BadClus, $Bitmap,
 *                      the read-write gate (bitmask, `force' overrides the
 *                      dirty flag only), show_options, remount, unmount.
 *
 * The write side, between S20 and S21:
 *
 *  S23 write infra     ntfs_now(), ntfs_latch() / ntfs_fs_error() (errors
 *                      latch read-only, the volume stays dirty),
 *                      ntfs_write_begin(), ntfs_rl_pwrite_raw() /
 *                      ntfs_rl_pwrite_rmw() (writes through a runlist,
 *                      whole device sectors), ntfs_wt_write() (metadata
 *                      stream write-through + cached pages updated),
 *                      ntfs_mft_write() (re-validated record, fresh USN,
 *                      whole I/O unit, $MFTMirr, record cache),
 *                      ntfs_logfile_reset(), ntfs_vol_flags() (set/clear
 *                      RMW), ntfs_arm() (called by the chokepoint before
 *                      the first write of a mount).
 *  S24 clusters        $Bitmap allocator: free count, reservations
 *                      (ntfs_resv_get/put), MFT zone, goal / contiguous /
 *                      fragment passes, ntfs_clusters_alloc() (all or
 *                      nothing), ntfs_clusters_free(), ntfs_barrier()
 *                      (device flush between ordered steps).
 *  S25 MFT records     ntfs_mft_alloc() / ntfs_mft_free(), record
 *                      formatting, $MFT and $MFT:$BITMAP growth.
 *  S26 runlist edit    ntfs_rl_set() / ntfs_rl_truncate() / copies, the
 *                      mapping-pairs encoder ntfs_mp_encode().
 *  S27 editable inode  ntfs_eino_t: a file's whole attribute set loaded
 *                      (base + extension records through the attribute
 *                      list), changed (set_res / set_nonres / set_value /
 *                      drop), committed (ntfs_eino_commit(): layout into
 *                      the base record or base + attribute list + fresh
 *                      extension records, ordered writes, deferred frees);
 *                      ntfs_eino_commit_mft() for record 0 in place.
 *  S28 EA writer       ntfs_ea_apply() ($EA + $EA_INFORMATION rebuilt
 *                      together), ntfs_wsl_set() ($LX* owner/mode/dev).
 *  S29 index changes   ntfs_ixs_t sessions: B+tree insert (split, root
 *                      push-down), delete (predecessor, empty-leaf merge,
 *                      root collapse), key update, ordered commit;
 *                      $FILE_NAME / entry builders, ntfs_dir_add_entry() /
 *                      ntfs_dir_del_entry().
 *  S30 stream data     delayed allocation (reservations, the alloc_blocks
 *                      hook ntfs_sb_alloc_blocks(), placement with
 *                      zeroing), buffered writes, resident data,
 *                      compressed -> plain conversion, truncate, the
 *                      attribute commit ntfs_stream_commit(), direct I/O.
 *  S31 namespace       create (files, directories, WSL / native symlinks,
 *                      WSL special files, $Reparse index), unlink / rmdir
 *                      (deferred delete of open files), link, rename,
 *                      chmod / chown / utimens, $FILE_NAME duplicates.
 *  S32 VFS write ops   write / write_at / truncate / mkdir / ... / user.*
 *                      xattrs, sync, the clean-unmount sequence
 *                      ntfs_settle().
 *
 * Compression, system compression and $LogFile replay:
 *
 *  S12b WOF decoders   ntfs_xpress_decompress(), ntfs_lzx_decompress();
 *                      LZNT1 compression (ntfs_lznt1_compress()) in S12.
 *  S29b $Secure writer ntfs_secure_id(), ntfs_eino_set_sd().
 *  S33 log replay      ntfs_log_replay() and the clean log of S23
 *                      (ntfs_logfile_write_clean()).
 *
 * Lock order (ntfs.h section 4): per-file rwsem -> metadata rwsem ->
 * allocator mutex -> spinlocks (record cache, index cache, handle table,
 * statfs cache, page/inode/dentry caches) -> block device.
 *
 * ===========================================================================
 * Features
 * ===========================================================================
 *
 * Reading:
 *   - boot sector / geometry validation (4Kn, 64 KB clusters, 512-byte
 *     clusters, sector size vs device block), $MFT bootstrap incl. a
 *     fragmented $MFT described by an attribute list, $MFTMirr comparison
 *     (first min(16, mirror) records), $Volume (version, flags, label),
 *     $AttrDef, $UpCase, $LogFile restart-area check (state reported),
 *     hibernation / Fast Startup detection, $Secure, $Extend, $BadClus
 *     validation, $Bitmap size check; read-only gate with one log line.
 *   - record cache with fixups, attribute walk with every hostile-input
 *     check, attribute lists, resident / non-resident attributes, runlists
 *     (binary search), sparse runs, initialized_size, LZNT1 compression,
 *     encrypted files (EOPNOTSUPP), WOF files recognised (EOPNOTSUPP),
 *     reparse points (symlinks, junctions, WSL links and special files),
 *     alternate data streams (path suffix + user. xattrs), EAs, WSL
 *     $LXUID/$LXGID/$LXMOD/$LXDEV, the file-flags EA, POSIX ACL xattrs
 *     (acl), security descriptors (system.ntfs_security),
 *     system.ntfs_attrib(_be), system.dos_attrib, system.ntfs_crtime(_be),
 *     system.ntfs_reparse_data.
 *   - $I30 B+trees: lookup with upcase collation (case-sensitive by
 *     default, nocase), readdir in collation order with resume keys,
 *     DOS names skipped, dcache with negative entries.
 *   - iomap layer, page cache integration with positional ids, NOFILL
 *     pages, coalesced reads and readahead, inode cache for open files.
 *   - stat / lstat / fstat / readlink / open / read / read_at / seek incl.
 *     SEEK_DATA/SEEK_HOLE / readdir / statfs / getxattr / listxattr /
 *     inode_flags / show_options / remount / unmount /
 *     release_locks_for_task.
 * Writing (NTFS_WRITE_SUPPORTED 1):
 *   - the read-write gate as a bitmask (hibernation, mount errors, mirror,
 *     dirty without `force', unclean / bad $LogFile, geometry, version,
 *     read-only device, system clusters not in use in $Bitmap); lazy
 *     arming inside the write chokepoint (first real write: $LogFile
 *     emptied, VOLUME_IS_DIRTY set, device flushed); errors latch
 *     read-only; sync never clears the dirty flag; clean unmount and
 *     remount read-only clear it after every commit and flush succeeded.
 *   - one device writer (ntfs_dev_write; ntfs_dev_discard);
 *     metadata written through in a fixed order with device-flush
 *     barriers; MFT records re-validated, MST-protected, whole I/O units,
 *     $MFTMirr updated.
 *   - cluster allocator (MFT zone, goal, contiguous first, exact
 *     reservations), MFT record allocator ($MFT and $MFT:$BITMAP growth),
 *     attribute sets of any size (attribute lists, extension records,
 *     non-resident attribute lists, multi-extent mapping pairs),
 *     resident <-> non-resident, sparse files.
 *   - delayed allocation (alloc_blocks), buffered writes through the page
 *     cache, initialized-size handling (gap zeroing, the page-aligned
 *     rule), truncate both ways, direct I/O (O_DIRECT 0x4000: the VFS
 *     passes open flags through unchanged; there is no named flag),
 *     fsync / sync / close commits.
 *   - create / O_CREAT / O_EXCL / O_TRUNC / O_APPEND, mkdir, rmdir, unlink
 *     (open files deleted at their last close), link, rename (any
 *     directory, over files and empty directories, case-only with
 *     nocase, loop check), symlink (WSL / native), mknod (WSL), chmod /
 *     chown (WSL EAs, READONLY), utimens, user.* xattrs (EAs), $FILE_NAME
 *     duplicates in the parent indexes, $Reparse index entries.
 *   - B+tree insertion and deletion with split, push-down, merge, collapse.
 * Further:
 *   - $LogFile replay (S33): restart area, checkpoint and table dumps,
 *     analysis, redo into an in-memory overlay with page-LSN comparison,
 *     undo of uncommitted transactions, validation, write-out (records
 *     with mirror, index blocks, bitmaps), flush, clean log; all or
 *     nothing, rw mounts only, never when hibernated, unknown operations
 *     and any inconsistency refuse with nothing written; after a replay
 *     the mount starts over on the replayed volume.  Opt-in: only with
 *     the mount option `recover' (never at a remount, never by automatic
 *     mounts); without it an unclean journal keeps the volume read-only.
 *   - a clean $LogFile (restart pages 1.1, empty checkpoint, tail copies)
 *     at clean unmount and remount read-only, before the clean flag.
 *   - $Secure writer (S29b): descriptors deduplicated by hash in $SDH,
 *     appended to $SDS (+ mirror) with $SII / $SDH entries; new files get
 *     a 72-byte $STANDARD_INFORMATION naming the shared default id;
 *     system.ntfs_security, system.ntfs_attrib(_be), system.dos_attrib,
 *     system.ntfs_crtime(_be) writes (owner or root); POSIX ACL
 *     xattrs (acl: access ACL sets the mode, chmod rewrites the ACL,
 *     default ACLs inherited at creation); security.* and
 *     trusted.* EAs.
 *   - compression: flags set / cleared on empty files and directories,
 *     inherited by new files, LZNT1 compressor, per-unit compressed
 *     writes and truncates, no size limit.
 *   - named streams created (open O_CREAT "file:stream"), removed (unlink
 *     "file:stream", EBUSY while open), resized, written and removed
 *     through the user.* xattr view.
 *   - WOF (system-compressed) reads: XPRESS4K/8K/16K and LZX chunks.
 *   - discard of freed clusters after the cleared bits are durable, a
 *     FITRIM-style trim of the free space.
 *   - the volume label as the root's system.ntfs_volume_label xattr;
 *     errors=continue|remount-ro|panic (default remount-ro);
 *     preallocated_size= as growing room behind file tails.
 *   - write ordering: commits are final once their referencing
 *     write is on the device (later failures leak, latch, never undo);
 *     index updates written bottom-up; duplicate entries removed with the
 *     name; system files and everything under $Extend never written;
 *     a forced mount never clears a dirty flag it found; boot sectors
 *     never written or allocated; known-bad clusters reserved before the
 *     first allocation; more system files checked against $Bitmap.
 * Not implemented, for want of an interface in the VFS: an ioctl entry for
 *   FITRIM (ntfs_trim_fs() exists and is tested by the host unit test),
 *   file attribute flags (file-flags EA writes: no FS_IOC_SETFLAGS),
 *   O_TMPFILE, fallocate.
 */

#ifndef LIKEOS_NTFS_INT_H
#define LIKEOS_NTFS_INT_H

/* 1: the write path exists.  While 0 every mount ends up read-only,
 * whatever the gate decides, and the write chokepoint refuses everything
 * (NTFS_GATE_UNSUPPORTED). */
#define NTFS_WRITE_SUPPORTED 1

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef struct ntfs_actx {
	ntfs_fs_t *fs;
	uint64_t base_no;
	uint16_t base_seq;
	uint8_t *base; /* base record copy */
	uint8_t *ext; /* extension record buffer */
	uint64_t ext_no; /* record held in ext, ~0 = none */
	uint8_t *al; /* validated attribute list, or NULL */
	uint32_t al_len;
	uint32_t pos; /* next attribute offset / list entry offset */
	/* the last attribute found */
	uint8_t *rec; /* base or ext */
	uint64_t rec_no;
	ntfs_attr_record_t *a;
	uint32_t aoff;
	const ntfs_attr_list_entry_t *ale; /* its list entry, or NULL */
} ntfs_actx_t;

typedef struct ntfs_attr_sizes {
	uint64_t alloc_size;
	uint64_t data_size;
	uint64_t init_size;
	uint64_t comp_size;
	uint16_t flags;
	uint8_t comp_unit;
	int resident;
} ntfs_attr_sizes_t;

#define NTFS_SLOT_DYING ((ntfs_stream_t *)~0UL)

/* ntfs_stream_get() flags */
#define NTFS_SG_WOF 0x1 /* the file is a WOF reparse point */
#define NTFS_SG_ENCRYPTED 0x2 /* the file is $EFS-encrypted */
#define NTFS_SG_IC 0x4 /* match the name case-insensitively */
/* With NTFS_SG_WOF: the WOF chunk format + 1 in bits 8..11 (0: unknown). */
#define NTFS_SG_WOF_FMT(f) ((((int)(f) + 1) & 0xF) << 8)
#define NTFS_SG_WOF_FMT_OF(sg) ((((sg) >> 8) & 0xF) - 1)

#define NTFS_IOMAP_HOLE 0
#define NTFS_IOMAP_MAPPED 1
#define NTFS_IOMAP_UNWRITTEN 2
#define NTFS_IOMAP_DELALLOC 3
#define NTFS_IOMAP_INLINE 4
#define NTFS_IOMAP_COMPRESSED 5
#define NTFS_IOMAP_ENCRYPTED 6

/* ntfs_iomap_begin() flags */
#define NTFS_IOMAP_READ 0x0
#define NTFS_IOMAP_WRITE 0x1
#define NTFS_IOMAP_DIRECT 0x2
#define NTFS_IOMAP_REPORT 0x4 /* SEEK_DATA / SEEK_HOLE */

typedef struct ntfs_iomap {
	int type;
	uint64_t offset; /* byte offset in the stream */
	uint64_t length; /* bytes */
	uint64_t pbyte; /* MAPPED: device byte offset of `offset' */
	const uint8_t *inline_data; /* INLINE: data at `offset' */
} ntfs_iomap_t;

#define NTFS_RD_PAGES 16

typedef struct ntfs_index {
	ntfs_fs_t *fs;
	uint64_t mft_no;
	ntfs_stream_t *alloc; /* referenced; NULL for a small index */
	int own_alloc; /* alloc was referenced by ntfs_index_open() */
	uint32_t collation;
	uint32_t ib_size;
	uint32_t vcn_bits;
	uint8_t *root; /* $INDEX_ROOT value copy */
	uint32_t root_len;
	uint8_t *blk; /* current block */
	uint8_t *blk2; /* second block buffer (readdir) */
} ntfs_index_t;

/* A search key.  FILE_NAME indexes: a name (units); others: raw bytes. */
typedef struct ntfs_ikey {
	const void *key;
	uint32_t len; /* units for FILE_NAME / UNICODE_STRING, bytes else */
	int ic; /* case-insensitive FILE_NAME search */
} ntfs_ikey_t;

/* Callback of ntfs_index_walk(): an entry in collation order.  Return 0 to
 * continue, 1 to stop, < 0 for an error. */
typedef int (*ntfs_ie_cb)(ntfs_index_t *ix, const ntfs_index_entry_t *e,
			  void *arg);

typedef struct ntfs_res {
	uint64_t mref; /* the object (the base file for a stream) */
	uint64_t parent; /* its directory */
	uint32_t depth; /* components from the root to `parent' */
	int is_stream;
	uint16_t sname[NTFS_MAX_NAME_LEN];
	uint32_t snlen;
	/* The last component as given (UTF-8), for the namespace changes;
	 * last_missing: it does not exist, missing_dir is the directory it
	 * would be in (ST_NOT_FOUND is returned). */
	int last_missing;
	uint64_t missing_dir;
	uint32_t llen;
	char lname[VFS_NAME_MAX + 1];
} ntfs_res_t;

#define NTFS_BM_PAGE_BITS (PAGE_SIZE * 8)
/* Clusters kept back from data reservations for metadata. */
#define NTFS_META_FLOOR 64ULL

/* ntfs_clusters_alloc() flags */
#define NTFS_AL_RESERVED 0x1 /* consumes a delayed-allocation reservation */
#define NTFS_AL_META 0x2 /* metadata: may use the floor */
#define NTFS_AL_MFT 0x4 /* $MFT growth: the zone first */
#define NTFS_AL_CONTIG 0x8 /* exactly one run or nothing */
/* The tail of a growing file: look for room to keep growing in place
 * (preallocated_size=, below). */
#define NTFS_AL_TAIL 0x10

#define NTFS_FIRST_FREE_REC 24
#define NTFS_MFT_GROW_RECS 64

typedef struct ntfs_eattr {
	uint8_t *b; /* the attribute record */
	uint64_t orec; /* MFT record it was in, ~0: new */
	uint16_t oinst; /* its instance there */
	uint8_t dirty;
	uint32_t slot; /* layout: 0 = base, k = k-th extension record */
	uint16_t inst; /* layout: instance it gets */
} ntfs_eattr_t;

typedef struct ntfs_eext {
	uint64_t no;
	uint16_t seq;
	uint16_t usa_ofs;
	uint16_t usn;
	uint16_t next_inst;
	uint64_t lsn;
	uint32_t nattr; /* attributes it held at load */
	int keep; /* layout: written as it is */
} ntfs_eext_t;

typedef struct ntfs_eino {
	ntfs_fs_t *fs;
	uint64_t no;
	uint16_t seq;
	uint16_t link_count;
	uint16_t rflags;
	uint16_t next_inst;
	uint16_t usa_ofs;
	uint16_t usn;
	uint64_t lsn;
	int is_new;
	ntfs_eattr_t *a;
	uint32_t n, cap;
	ntfs_eext_t *x; /* extension records it had */
	uint32_t nx, xcap;
	int had_al;
	int al_nonres;
	ntfs_runlist_t al_rl; /* the old non-resident list's clusters */
	ntfs_run_t *fafter; /* free after a successful commit */
	uint32_t nfafter, cfafter;
	ntfs_run_t *fundo; /* free when the commit fails */
	uint32_t nfundo, cfundo;
} ntfs_eino_t;

typedef struct ntfs_ea_op {
	const char *name;
	uint32_t nlen;
	const uint8_t *val; /* NULL: remove */
	uint32_t vlen;
	uint8_t flags;
	int must_exist; /* removal / replace of an absent entry: ST_NODATA */
} ntfs_ea_op_t;

typedef struct ntfs_ient {
	uint8_t *e; /* entry without a child VCN (length field matches) */
	int64_t child; /* the child VCN, -1 in a leaf */
} ntfs_ient_t;

typedef struct ntfs_xnode {
	int64_t vcn; /* -1: the root */
	int is_node;
	ntfs_ient_t *ents;
	uint32_t n, cap;
	int64_t end_child;
	int dirty, is_new, freed;
	uint32_t depth;
	uint64_t lsn;
	uint16_t usn;
	struct ntfs_xnode *next;
} ntfs_xnode_t;

typedef struct ntfs_ixs {
	ntfs_fs_t *fs;
	uint64_t mref;
	uint16_t name[8];
	uint32_t nlen;
	ntfs_index_t ix; /* for the block reader and the collation */
	ntfs_stream_t *alloc; /* the allocation stream (referenced) */
	uint8_t ir_hdr[NTFS_INDEX_ROOT_HDR]; /* type .. reserved */
	ntfs_xnode_t *root;
	ntfs_xnode_t *nodes; /* every node of the session */
	uint8_t *bmp;
	uint32_t bmp_len;
	uint8_t *bmp0; /* $BITMAP as on the device when loaded */
	uint32_t bmp0_len;
	int bmp_loaded, bmp_dirty, had_bmp;
	uint32_t root_cap;
	int root_dirty;
	/* allocation growth (undo) */
	int alloc_changed;
	int had_alloc;
	ntfs_runlist_t alloc_old_rl;
	uint64_t alloc_old_size;
	uint32_t alloc_old_flags;
	ntfs_run_t *newruns;
	uint32_t nnew, cnew;
	uint8_t **reins; /* separators of freed leaves, to insert again */
	uint32_t nre;
	int deleting;
	int touch; /* update the owner's times */
	int done;
} ntfs_ixs_t;

struct ntfs_ofile {
	uint64_t no;
	int opens;
	int unlinked;
	struct ntfs_ofile *next;
};

#define NTFS_MK_FILE 0
#define NTFS_MK_DIR 1
#define NTFS_MK_LINK 2
#define NTFS_MK_NODE 3

typedef struct ntfs_mkargs {
	int kind;
	uint32_t mode; /* S_IF* | permissions */
	const char *target; /* symlink */
	uint64_t dev; /* mknod */
} ntfs_mkargs_t;

/* ===========================================================================
 * Functions and data shared between the files, by the file that defines them
 * =========================================================================== */

/* super.c */
extern const vfs_ops_t ntfs_vfs_ops;
int ntfs_arm(ntfs_fs_t *fs);
int ntfs_bitmap_free(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t limit,
		     uint64_t *free_out);

/* misc.c */
void ntfs_meta_rlock(ntfs_fs_t *fs);
void ntfs_meta_runlock(ntfs_fs_t *fs);
void ntfs_meta_wlock(ntfs_fs_t *fs);
void ntfs_meta_wunlock(ntfs_fs_t *fs);
int ntfs_meta_held_excl(ntfs_fs_t *fs);
void ntfs_flock_shared(ntfs_fs_t *fs, uint64_t mft_no);
void ntfs_funlock_shared(ntfs_fs_t *fs, uint64_t mft_no);
void ntfs_flock_excl(ntfs_fs_t *fs, uint64_t mft_no);
void ntfs_funlock_excl(ntfs_fs_t *fs, uint64_t mft_no);
void ntfs_alloc_lock(ntfs_fs_t *fs);
void ntfs_alloc_unlock(ntfs_fs_t *fs);
void ntfs_latch(ntfs_fs_t *fs, const char *why);
int ntfs_fs_error(ntfs_fs_t *fs, uint64_t no, const char *why);
int ntfs_protected(ntfs_fs_t *fs, uint64_t mref);
void ntfs_fs_broken(ntfs_fs_t *fs, uint64_t no, const char *why);
void ntfs_leaked(ntfs_fs_t *fs);
int ntfs_write_begin(ntfs_fs_t *fs);

/* bdev.c */
int ntfs_dev_read(ntfs_fs_t *fs, uint64_t lba, uint64_t count,
		  void *buf);
int ntfs_dev_pread(ntfs_fs_t *fs, uint64_t off, uint64_t len, void *buf);
int ntfs_read_clusters(ntfs_fs_t *fs, int64_t lcn, uint64_t n,
		       void *buf);
int ntfs_dev_write(ntfs_fs_t *fs, uint64_t lba, uint64_t count,
		   const void *buf);
int ntfs_dev_discard(ntfs_fs_t *fs, uint64_t lba, uint64_t count,
		     int explicit);
int ntfs_dev_flush(ntfs_fs_t *fs);
void ntfs_guard_init(ntfs_fs_t *fs);
int ntfs_rl_pwrite_raw(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
		       uint64_t off, const uint8_t *buf, uint64_t len);
int ntfs_rl_pwrite_rmw(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
		       uint64_t off, const uint8_t *src, uint64_t len);
int ntfs_wt_write(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t off,
		  const uint8_t *src, uint64_t len);

/* mft.c */
void ntfs_rc_init(ntfs_fs_t *fs);
void ntfs_rc_release(ntfs_fs_t *fs);
int ntfs_mft_read_raw(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
		      uint64_t no, uint8_t *buf);
int ntfs_mft_read(ntfs_fs_t *fs, uint64_t no, uint8_t *buf);
void ntfs_ic_store(ntfs_fs_t *fs, uint32_t handle, int64_t vcn,
		   const uint8_t *buf, uint32_t size);
void ntfs_ic_forget(ntfs_fs_t *fs, uint32_t handle, int64_t vcn);
int ntfs_mft_write_ex(ntfs_fs_t *fs, uint64_t no, uint8_t *rec,
		      int mirror_first);
int ntfs_mft_write(ntfs_fs_t *fs, uint64_t no, uint8_t *rec);
int ntfs_mft_alloc(ntfs_fs_t *fs, uint64_t *no_out, uint16_t *seq_out);
int ntfs_mft_free(ntfs_fs_t *fs, uint64_t no);

/* attrib.c */
extern const uint16_t ntfs_unnamed[1];
void ntfs_actx_close(ntfs_actx_t *c);
void ntfs_actx_reset(ntfs_actx_t *c);
int ntfs_actx_open(ntfs_fs_t *fs, uint64_t mref, ntfs_actx_t **out);
int ntfs_actx_find(ntfs_actx_t *c, uint32_t type, const uint16_t *name,
		   uint32_t nlen, int flags);
int ntfs_attr_runlist(ntfs_fs_t *fs, uint64_t mref, uint32_t type,
		      const uint16_t *name, uint32_t nlen,
		      ntfs_runlist_t *rl, ntfs_attr_sizes_t *sz);
int ntfs_rl_pread(ntfs_fs_t *fs, const ntfs_runlist_t *rl, uint64_t off,
		  uint64_t len, uint64_t init, uint8_t *dst);
int ntfs_attr_readall(ntfs_fs_t *fs, uint64_t mref, uint32_t type,
		      const uint16_t *name, uint32_t nlen, uint32_t cap,
		      uint8_t **out, uint32_t *out_len);
void ntfs_eino_free(ntfs_eino_t *e);
int ntfs_eino_load(ntfs_fs_t *fs, uint64_t mref, ntfs_eino_t **out);
ntfs_eino_t *ntfs_eino_new(ntfs_fs_t *fs, uint64_t no, uint16_t seq,
			   uint16_t rflags);
long ntfs_eino_find(const ntfs_eino_t *e, uint32_t type,
		    const uint16_t *name, uint32_t nlen, uint32_t from);
void ntfs_eino_remove(ntfs_eino_t *e, uint32_t idx);
int ntfs_eino_drop(ntfs_eino_t *e, uint32_t type, const uint16_t *name,
		   uint32_t nlen, int free_clusters);
uint8_t *ntfs_ab_res(uint32_t type, const uint16_t *name, uint32_t nlen,
		     uint16_t flags, const void *val, uint32_t vlen,
		     uint8_t rflags);
int ntfs_eino_put(ntfs_eino_t *e, uint8_t *b, uint64_t orec,
		  uint16_t oinst);
int ntfs_eino_set_res(ntfs_eino_t *e, uint32_t type,
		      const uint16_t *name, uint32_t nlen,
		      uint16_t flags, const void *val, uint32_t vlen);
int ntfs_eino_set_nonres(ntfs_eino_t *e, uint32_t type,
			 const uint16_t *name, uint32_t nlen,
			 uint16_t flags, uint8_t cu,
			 const ntfs_runlist_t *rl, uint64_t alloc,
			 uint64_t data, uint64_t init, uint64_t comp);
int ntfs_eino_value(ntfs_eino_t *e, uint32_t type, const uint16_t *name,
		    uint32_t nlen, uint32_t cap, uint8_t **out,
		    uint32_t *out_len);
int ntfs_eino_set_value(ntfs_eino_t *e, uint32_t type,
			const uint16_t *name, uint32_t nlen,
			const uint8_t *val, uint32_t vlen,
			uint32_t res_max);
int ntfs_eino_commit(ntfs_eino_t *e);
int ntfs_eino_commit_mft(ntfs_fs_t *fs, const ntfs_runlist_t *drl,
			 uint64_t dsize, uint64_t dinit);
int ntfs_mftbm_grow(ntfs_fs_t *fs, uint64_t need);

/* inode.c */
extern const uint16_t ntfs_i30_name[4];
void ntfs_lru_del(ntfs_fs_t *fs, ntfs_stream_t *st);
void ntfs_stream_ref(ntfs_fs_t *fs, ntfs_stream_t *st);
void ntfs_stream_put(ntfs_fs_t *fs, ntfs_stream_t *st);
void ntfs_shrinker_register(ntfs_fs_t *fs);
ntfs_stream_t *ntfs_stream_find_locked(ntfs_fs_t *fs, uint64_t no,
				       uint16_t seq, uint32_t type,
				       const uint16_t *name,
				       uint32_t nlen, int ic);
int ntfs_index_geometry(ntfs_fs_t *fs, uint32_t ib_size, uint32_t *bits);
int ntfs_stream_get(ntfs_fs_t *fs, uint64_t mref, uint32_t type,
		    const uint16_t *name, uint32_t nlen, int sgflags,
		    ntfs_stream_t **out);
int ntfs_dir_stream(ntfs_fs_t *fs, uint64_t mref, ntfs_stream_t **out);
void ntfs_stream_install_reserved(ntfs_fs_t *fs, ntfs_stream_t *st,
				  uint32_t handle);
int ntfs_stream_open_reserved(ntfs_fs_t *fs, uint64_t no, uint32_t type,
			      const char *aname, uint32_t handle,
			      ntfs_stream_t **out);
void ntfs_streams_release(ntfs_fs_t *fs);
int ntfs_page_nofill(const ntfs_stream_t *st, uint64_t idx);
int ntfs_inode_load(ntfs_fs_t *fs, uint64_t mref, ntfs_inode_t *ino);
int ntfs_inode_is_link(const ntfs_inode_t *ino);
uint32_t ntfs_inode_mode(ntfs_fs_t *fs, const ntfs_inode_t *ino);
int ntfs_stat(vfs_mount_t *m, const char *path, struct kstat *st);
int ntfs_lstat(vfs_mount_t *m, const char *path, struct kstat *st);
int ntfs_fstat(vfs_file_t *f, struct kstat *st);
int ntfs_inode_flags(vfs_mount_t *m, unsigned long ino_no,
		     uint32_t *out_flags);

/* aops.c */
extern const vfs_sb_ops_t ntfs_sb_ops;
ntfs_fs_t *ntfs_sb_fs(vfs_superblock_t *sb);
int ntfs_iomap_begin(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
		     uint64_t len, int flags, ntfs_iomap_t *map);
int ntfs_iomap_end(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
		   uint64_t len, uint64_t done, int flags,
		   const ntfs_iomap_t *map);
int ntfs_stream_page(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t idx,
		     pc_page_t **out);
int ntfs_stream_pread(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t off,
		      uint64_t len, void *buf, int to_user);
int ntfs_stream_read_pages(ntfs_fs_t *fs, ntfs_stream_t *st,
			   uint64_t pos, uint64_t bytes,
			   pc_page_t **pages, uint32_t *got,
			   pc_readahead_t *ra);

/* index.c */
/* Geometry of a directory index stream: block size and VCN unit from the
 * $INDEX_ROOT; NOALLOC when there is no $INDEX_ALLOCATION. */
int ntfs_index_root_read(ntfs_fs_t *fs, uint64_t mref,
			 const uint16_t *name, uint32_t nlen,
			 uint8_t **root, uint32_t *rlen);
void ntfs_index_close(ntfs_index_t *ix);
int ntfs_index_open(ntfs_fs_t *fs, uint64_t mref, const uint16_t *name,
		    uint32_t nlen, ntfs_stream_t *alloc,
		    ntfs_index_t **out);
void ntfs_ic_unhash(ntfs_fs_t *fs, int e);
void ntfs_ic_insert(ntfs_fs_t *fs, uint32_t handle, int64_t vcn,
		    const uint8_t *buf, uint32_t size);
void ntfs_ic_drop_stream(ntfs_fs_t *fs, uint32_t handle);
void ntfs_ic_init(ntfs_fs_t *fs);
void ntfs_ic_release(ntfs_fs_t *fs);
int ntfs_index_lookup(ntfs_index_t *ix, const ntfs_ikey_t *k,
		      uint8_t **out);
int ntfs_index_walk(ntfs_index_t *ix, const ntfs_ikey_t *after,
		    ntfs_ie_cb cb, void *arg);
void ntfs_ixs_free(ntfs_ixs_t *s);
void ntfs_ixs_abort(ntfs_ixs_t *s);
int ntfs_ixs_open(ntfs_fs_t *fs, uint64_t mref, const uint16_t *name,
		  uint32_t nlen, ntfs_ixs_t **out);
int ntfs_ixs_insert(ntfs_ixs_t *s, uint8_t *e);
int ntfs_ixs_update(ntfs_ixs_t *s, const ntfs_ikey_t *k,
		    const uint8_t *e);
int ntfs_ixs_empty(ntfs_ixs_t *s, int *empty);
int ntfs_ixs_commit(ntfs_ixs_t *s);
int ntfs_ixs_delete_commit(ntfs_ixs_t *s, const ntfs_ikey_t *k,
			   uint64_t mref);
int ntfs_dir_add_entry(ntfs_fs_t *fs, uint64_t dir, uint64_t mref,
		       const uint8_t *fn, uint32_t fl, int touch);
int ntfs_dir_del_entry(ntfs_fs_t *fs, uint64_t dir, uint64_t mref,
		       const uint16_t *name, uint32_t nlen, int touch);

/* dir.c */
int ntfs_dc_key(ntfs_fs_t *fs, const char *name, uint32_t nlen,
		char *key, uint16_t *u16);
int ntfs_dir_lookup(ntfs_fs_t *fs, uint64_t dir, const char *name,
		    uint32_t nlen, uint64_t *mref, uint32_t *fattr);
int ntfs_parent_of(ntfs_fs_t *fs, uint64_t mref, uint64_t *parent);
uint32_t ntfs_dir_depth(ntfs_fs_t *fs, uint64_t mref);
int ntfs_resolve(ntfs_fs_t *fs, const char *path, int follow,
		 ntfs_res_t *res);
int ntfs_inode_quick_link(ntfs_fs_t *fs, uint64_t mref, int *is_link,
			  int *is_dir);
long ntfs_readdir(vfs_file_t *f, void *buf, long bytes);

/* ea.c */
int ntfs_ea_load(ntfs_fs_t *fs, uint64_t mref, uint8_t **buf,
		 uint32_t *qlen);
int ntfs_inode_load_eas(ntfs_fs_t *fs, uint64_t mref, ntfs_inode_t *ino);
int ntfs_ea_apply(ntfs_eino_t *e, const ntfs_ea_op_t *ops,
		  uint32_t nops);
int ntfs_wsl_set(ntfs_eino_t *e, int has_uid, uint32_t uid,
		 int has_gid, uint32_t gid, int has_mode, uint32_t mode,
		 int has_dev, uint32_t major, uint32_t minor);

/* xattr.c */
int ntfs_acl_chmod(ntfs_fs_t *fs, ntfs_eino_t *e, uint64_t mref,
		   uint32_t mode);
int ntfs_acl_inherit(ntfs_fs_t *fs, ntfs_eino_t *e, uint64_t dir,
		     int is_dir, uint32_t *mode, int *changed);
int ntfs_set_data_flags(ntfs_fs_t *fs, ntfs_eino_t *e, int comp,
			int sparse);
int ntfs_setxattr(vfs_mount_t *m, const char *path, int nofollow,
		  const char *name, const void *val, unsigned long size,
		  int flags);
int ntfs_removexattr(vfs_mount_t *m, const char *path, int nofollow,
		     const char *name);
int ntfs_fsetxattr(vfs_file_t *f, const char *name, const void *val,
		   unsigned long size, int flags);
int ntfs_fremovexattr(vfs_file_t *f, const char *name);
int ntfs_getxattr(vfs_mount_t *m, const char *path, int nofollow,
		  const char *name, void *val, unsigned long size);
int ntfs_listxattr(vfs_mount_t *m, const char *path, int nofollow,
		   char *list, unsigned long size);
int ntfs_fgetxattr(vfs_file_t *f, const char *name, void *val,
		   unsigned long size);
int ntfs_flistxattr(vfs_file_t *f, char *list, unsigned long size);
int ntfs_getxattr_ino(vfs_mount_t *m, unsigned long ino,
		      const char *name, void *val, unsigned long size);

/* reparse.c */
int ntfs_reparse_read(ntfs_fs_t *fs, uint64_t mref, uint8_t **buf,
		      uint32_t *len);
int ntfs_link_target_ino(ntfs_fs_t *fs, const ntfs_inode_t *ino,
			 uint32_t depth, char *out, unsigned long cap);

/* security.c */
int ntfs_sd_get(ntfs_fs_t *fs, const ntfs_inode_t *ino, uint8_t **out,
		uint32_t *out_len);
int ntfs_secure_id(ntfs_fs_t *fs, const uint8_t *sd, uint32_t len,
		   uint32_t *id);
int ntfs_eino_set_secid(ntfs_eino_t *e, uint32_t id);
int ntfs_eino_set_sd(ntfs_eino_t *e, const uint8_t *sd, uint32_t len);

/* lcnalloc.c */
int ntfs_barrier(ntfs_fs_t *fs);
void ntfs_sf_publish(ntfs_fs_t *fs);
int ntfs_bm_next_free(ntfs_fs_t *fs, uint64_t from, uint64_t to,
		      uint64_t *out, uint32_t budget, uint32_t *scanned);
int ntfs_resv_get(ntfs_fs_t *fs, uint64_t n);
void ntfs_resv_put(ntfs_fs_t *fs, uint64_t n);
int ntfs_clusters_free(ntfs_fs_t *fs, int64_t lcn, uint64_t count);
int ntfs_rl_free_clusters(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
			  int64_t v0, int64_t v1);
int ntfs_clusters_alloc(ntfs_fs_t *fs, int64_t goal, uint64_t count,
			int flags, ntfs_run_t **runs, uint32_t *nruns);
/* Make what was written so far durable before the next step depends on
 * it (allocation before reference, dereference before free). */
void ntfs_discard_run(ntfs_fs_t *fs);

/* file.c */
void ntfs_stream_unpin(ntfs_fs_t *fs, ntfs_stream_t *st);
void ntfs_unreserve_from(ntfs_fs_t *fs, ntfs_stream_t *st, int64_t vcn);
int ntfs_sb_alloc_blocks(vfs_superblock_t *sb, unsigned long chain,
			 unsigned long first, unsigned long count);
int ntfs_stream_commit(ntfs_fs_t *fs, ntfs_stream_t *st);
long ntfs_stream_write(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t pos,
		       const uint8_t *src, uint64_t n);
int ntfs_stream_truncate(ntfs_fs_t *fs, ntfs_stream_t *st,
			 uint64_t size);
int ntfs_entry_listed(ntfs_fs_t *fs, uint64_t dir_no,
		      const ntfs_index_entry_t *e);
void ntfs_wlock_file(ntfs_fs_t *fs, uint64_t no);
void ntfs_wunlock_file(ntfs_fs_t *fs, uint64_t no);
int ntfs_commit_locked(ntfs_fs_t *fs, ntfs_stream_t *st);
int ntfs_commit_all(ntfs_fs_t *fs);
long ntfs_write(vfs_file_t *f, const void *buf, long bytes);
long ntfs_write_at(vfs_file_t *f, const void *buf, long bytes,
		   long off);
int ntfs_truncate(vfs_file_t *f, unsigned long size);
int ntfs_open_mode(vfs_mount_t *m, const char *path, int flags,
		   unsigned int mode, vfs_file_t **out);
int ntfs_open(vfs_mount_t *m, const char *path, int flags,
	      vfs_file_t **out);
int ntfs_close(vfs_file_t *f);
long ntfs_read(vfs_file_t *f, void *buf, long bytes);
long ntfs_read_at(vfs_file_t *f, void *buf, long bytes, long off);
long ntfs_seek(vfs_file_t *f, long offset, int whence);

/* namei.c */
int ntfs_ofile_open(ntfs_fs_t *fs, uint64_t no);
int ntfs_ofile_close(ntfs_fs_t *fs, uint64_t no);
int ntfs_new_name(ntfs_fs_t *fs, const char *name, uint32_t nlen,
		  uint16_t *u, uint32_t *ulen, uint8_t *ntype);
void ntfs_dc_forget(ntfs_fs_t *fs, uint64_t dir, const char *name,
		    uint32_t nlen);
ntfs_standard_information_t *ntfs_eino_si(ntfs_eino_t *e);
int ntfs_update_dups(ntfs_fs_t *fs, uint64_t mref);
int ntfs_file_delete(ntfs_fs_t *fs, uint64_t mref);
int ntfs_stream_create(ntfs_fs_t *fs, uint64_t mref,
		       const uint16_t *name, uint32_t nlen);
int ntfs_stream_remove(ntfs_fs_t *fs, uint64_t mref,
		       const uint16_t *name, uint32_t nlen, int ic);
void ntfs_creds(uint32_t *uid, uint32_t *gid);
int ntfs_create_in(ntfs_fs_t *fs, uint64_t dir, const uint16_t *u,
		   uint32_t ulen, uint8_t ntype, const ntfs_mkargs_t *mk,
		   uint64_t *out);
int ntfs_mkdir(vfs_mount_t *m, const char *path, unsigned int mode);
int ntfs_symlink(vfs_mount_t *m, const char *target,
		 const char *linkpath);
int ntfs_mknod(vfs_mount_t *m, const char *path, unsigned int mode,
	       uint64_t dev);
int ntfs_unlink(vfs_mount_t *m, const char *path);
int ntfs_rmdir(vfs_mount_t *m, const char *path);
int ntfs_link(vfs_mount_t *m, const char *oldpath, const char *newpath,
	      int follow);
int ntfs_rename(vfs_mount_t *m, const char *oldpath,
		const char *newpath);
int ntfs_chmod(vfs_mount_t *m, const char *path, unsigned int mode);
int ntfs_chown(vfs_mount_t *m, const char *path, int uid, int gid,
	       int nofollow);
int ntfs_fchmod(vfs_file_t *f, unsigned int mode);
int ntfs_fchown(vfs_file_t *f, int uid, int gid);
int ntfs_utimens(vfs_mount_t *m, const char *path, int nofollow,
		 const struct vfs_utimes *t);
int ntfs_futimens(vfs_file_t *f, const struct vfs_utimes *t);
int ntfs_readlink(vfs_mount_t *m, const char *path, char *buf,
		  unsigned long bufsz);
int ntfs_chdir(vfs_mount_t *m, const char *path);

/* logfile.c */
int ntfs_logfile_reset(ntfs_fs_t *fs);
int ntfs_logfile_write_clean(ntfs_fs_t *fs);
int ntfs_log_replay(ntfs_fs_t *fs);

/* ===========================================================================
 * Inline helpers shared between the files
 * =========================================================================== */

/* Assertions for mutators and readers (the rwsem is not a spinlock, so
 * lockdep_assert_held() cannot check it). */
static inline void ntfs_assert_meta_excl(ntfs_fs_t *fs)
{
	WARN_ON_ONCE(!ntfs_meta_held_excl(fs));
}

static inline void ntfs_assert_meta_any(ntfs_fs_t *fs)
{
	WARN_ON_ONCE(!fs->meta.writer && fs->meta.readers <= 0);
}

static inline uint64_t ntfs_byte_to_lba(ntfs_fs_t *fs, uint64_t byte)
{
	return byte >> fs->dev_ss_bits;
}

/* Bytes at the start of the volume that belong to $Boot: its 8 KB, and at
 * least the whole of cluster 0. */
static inline uint64_t ntfs_boot_bytes(const ntfs_fs_t *fs)
{
	return fs->cluster_size > 8192 ? fs->cluster_size : 8192;
}

/* May the chokepoint write at all?  A mount the gate let go read-write,
 * not latched read-only, with write support compiled in. */
static inline int ntfs_writable(const ntfs_fs_t *fs)
{
	return NTFS_WRITE_SUPPORTED && !fs->ro && !fs->latched_ro;
}

/* Name of an attribute (UTF-16LE units in place). */
static inline const uint16_t *ntfs_attr_name(const ntfs_attr_record_t *a)
{
	return (const uint16_t *)((const uint8_t *)a + a->name_offset);
}

static inline const uint8_t *ntfs_attr_value(const ntfs_attr_record_t *a)
{
	return (const uint8_t *)a + a->data.resident.value_offset;
}

/* The stream behind a handle, or NULL.  Lock-free reads of a pointer slot;
 * see the lifetime rules above. */
static inline ntfs_stream_t *ntfs_slot(ntfs_fs_t *fs, unsigned long h)
{
	ntfs_stream_t *st;

	if (h == 0 || h >= NTFS_SLOTS || !fs->slots)
		return NULL;
	st = __atomic_load_n(&fs->slots[h], __ATOMIC_ACQUIRE);
	return st == NTFS_SLOT_DYING ? NULL : st;
}

static inline unsigned long ntfs_chain(const ntfs_stream_t *st)
{
	return NTFS_BID_ENC(st->handle, 0);
}

static inline int ntfs_ic_bucket(uint32_t handle, int64_t vcn)
{
	uint64_t h = ((uint64_t)handle << 32) ^ (uint64_t)vcn;

	return (int)((h * 0x9E3779B97F4A7C15ULL) >> 55) & (NTFS_IC_HASH - 1);
}

static inline uint64_t ntfs_ino_mref(const ntfs_inode_t *ino)
{
	return NTFS_MAKE_MREF(ino->mft_no, ino->seq);
}

static inline ntfs_attr_record_t *ntfs_ea_hdr(const ntfs_eattr_t *x)
{
	return (ntfs_attr_record_t *)x->b;
}

static inline ntfs_fs_t *ntfs_fs_of(vfs_mount_t *m)
{
	return m ? (ntfs_fs_t *)m->fs_data : NULL;
}

static inline ntfs_file_t *ntfs_nf(vfs_file_t *f)
{
	return f ? (ntfs_file_t *)f->fs_private : NULL;
}

#endif /* LIKEOS_NTFS_INT_H */
