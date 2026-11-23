// LikeOS - ext4 filesystem driver: the journal: transactions, the deferred
// checkpoint log, replay on mount, ordered-mode writeback, commits and
// the commit thread
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* Begin a transaction (called on the outermost ext4_io_lock acquire). */
void ext4_txn_begin(ext4_fs_t *fs)
{
	if (!fs->j_enabled)
		return;
	fs->st->txn.active = 1;
	fs->st->txn.n = 0;
}

/* Buffer (or overwrite) a metadata block in the running transaction. */
void ext4_txn_capture(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
	/* A zero pbn here would be journaled as a block-0 tag and skipped (with a
	 * warning) on replay — catch the writer at the source instead. */
	WARN_ON_ONCE(pbn == 0);
	for (unsigned i = 0; i < fs->st->txn.n; i++)
		if (fs->st->txn.blk[i] == pbn) { /* re-dirtied in same op   */
			mm_memcpy(fs->st->txn.data[i], buf, fs->block_size);
			return;
		}
	if (fs->st->txn.n == fs->st->txn.cap) { /* grow the pointer arrays */
		unsigned nc = fs->st->txn.cap ? fs->st->txn.cap * 2 : 16;
		unsigned long *nb =
			(unsigned long *)kalloc(nc * sizeof(unsigned long));
		uint8_t **nd = (uint8_t **)kalloc(nc * sizeof(uint8_t *));
		if (!nb || !nd) { /* OOM: degrade to direct  */
			if (nb)
				kfree(nb);
			if (nd)
				kfree(nd);
			WARN_ON_ONCE(1);
			ext4_write_block_direct(fs, pbn, buf);
			return;
		}
		for (unsigned i = 0; i < fs->st->txn.n; i++) {
			nb[i] = fs->st->txn.blk[i];
			nd[i] = fs->st->txn.data[i];
		}
		for (unsigned i = fs->st->txn.n; i < nc; i++)
			nd[i] = 0; /* alloc on demand */
		if (fs->st->txn.blk)
			kfree(fs->st->txn.blk);
		if (fs->st->txn.data)
			kfree(fs->st->txn.data);
		fs->st->txn.blk = nb;
		fs->st->txn.data = nd;
		fs->st->txn.cap = nc;
	}
	if (!fs->st->txn.data[fs->st->txn.n])
		fs->st->txn.data[fs->st->txn.n] = (uint8_t *)kalloc(fs->block_size);
	if (!fs->st->txn.data[fs->st->txn.n]) {
		WARN_ON_ONCE(1);
		ext4_write_block_direct(fs, pbn, buf);
		return;
	}
	mm_memcpy(fs->st->txn.data[fs->st->txn.n], buf, fs->block_size);
	fs->st->txn.blk[fs->st->txn.n] = pbn;
	fs->st->txn.n++;
}

/* Read-your-writes: serve a block from the running transaction if buffered. */
int ext4_txn_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	for (unsigned i = 0; i < fs->st->txn.n; i++)
		if (fs->st->txn.blk[i] == pbn) {
			mm_memcpy(buf, fs->st->txn.data[i], fs->block_size);
			return 1;
		}
	return 0;
}

/* ===================================================================
 * Deferred-checkpoint circular journal log.
 *
 * Instead of checkpointing (writing committed metadata to its final location)
 * synchronously after every op — which cost a sync per op — committed
 * transactions accumulate as an "epoch" in the journal log and are checkpointed
 * lazily (when the log/pending set fills, on fsync/sync, or when an op frees a
 * block that still has a journal copy).  This drops the steady-state cost toward
 * ONE device sync per op (the commit), amortising the 2-sync checkpoint over the
 * whole epoch.
 *
 * st->ckpt is the in-memory authoritative copy of every metadata block written but
 * not yet checkpointed this epoch (latest content per block).  It serves two
 * roles: (1) read-your-writes — until checkpoint, a block's final disk location
 * still holds pre-epoch content, so reads must come from here; (2) the source
 * for the eventual checkpoint writeback.  Bounded by EXT4_CKPT_MAX_BLOCKS.
 *
 * Revoke records are NOT needed, and no checkpoint is forced either: freeing a
 * block drops its journal copy from st->txn / st->ckpt (ext4_drop_journal_copy).
 * Both live only in memory until ext4_journal_flush writes the batch, so a copy
 * that is forgotten was never on disk and can never be replayed over the
 * block's next use -- the same guarantee a revoke gives, without the I/O.
 * Serialized entirely by ext4_io_lock, like st->txn.
 * =================================================================== */
#define EXT4_CKPT_MAX_BLOCKS 256 /* pending unique-block cap (memory)  */
/* Transaction batching.  Each op's metadata is merged into the in-memory
 * batch (st->ckpt) on commit WITHOUT touching the disk; the batch is journalled +
 * checkpointed as ONE transaction only when it reaches this many distinct blocks,
 * on fsync/sync/unmount, or when a journalled block is freed.  This is what makes
 * the driver fast: the per-op device sync (a cache flush — brutally slow on USB)
 * is amortised over a whole batch of operations, like a real journal's ~5s commit
 * interval.  Between flushes the on-disk log is EMPTY, so a normal crash replays
 * nothing (instant recovery); only a crash during the brief flush window replays,
 * and at most this many blocks — keep it modest so that rare replay stays fast. */
#define EXT4_JOURNAL_BATCH 64
/* (legacy circular-log epoch bound — superseded by EXT4_JOURNAL_BATCH) */
#define EXT4_EPOCH_MAX_BLOCKS 32

/* Merge one block into the epoch pending set (latest content wins). */
static void ext4_ckpt_merge(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
	WARN_ON_ONCE(pbn == 0); /* see ext4_txn_capture */
	for (unsigned i = 0; i < fs->st->ckpt.n; i++)
		if (fs->st->ckpt.blk[i] == pbn) {
			mm_memcpy(fs->st->ckpt.data[i], buf, fs->block_size);
			return;
		}
	if (fs->st->ckpt.n == fs->st->ckpt.cap) {
		unsigned nc = fs->st->ckpt.cap ? fs->st->ckpt.cap * 2 : 32;
		unsigned long *nb =
			(unsigned long *)kalloc(nc * sizeof(unsigned long));
		uint8_t **nd = (uint8_t **)kalloc(nc * sizeof(uint8_t *));
		if (!nb || !nd) { /* OOM: checkpoint now to drain, then
                                           * write this block direct as a fallback */
			if (nb)
				kfree(nb);
			if (nd)
				kfree(nd);
			WARN_ON_ONCE(1);
			ext4_write_block_direct(fs, pbn, buf);
			return;
		}
		for (unsigned i = 0; i < fs->st->ckpt.n; i++) {
			nb[i] = fs->st->ckpt.blk[i];
			nd[i] = fs->st->ckpt.data[i];
		}
		for (unsigned i = fs->st->ckpt.n; i < nc; i++)
			nd[i] = 0;
		if (fs->st->ckpt.blk)
			kfree(fs->st->ckpt.blk);
		if (fs->st->ckpt.data)
			kfree(fs->st->ckpt.data);
		fs->st->ckpt.blk = nb;
		fs->st->ckpt.data = nd;
		fs->st->ckpt.cap = nc;
	}
	if (!fs->st->ckpt.data[fs->st->ckpt.n])
		fs->st->ckpt.data[fs->st->ckpt.n] = (uint8_t *)kalloc(fs->block_size);
	if (!fs->st->ckpt.data[fs->st->ckpt.n]) {
		WARN_ON_ONCE(1);
		ext4_write_block_direct(fs, pbn, buf);
		return;
	}
	mm_memcpy(fs->st->ckpt.data[fs->st->ckpt.n], buf, fs->block_size);
	fs->st->ckpt.blk[fs->st->ckpt.n] = pbn;
	fs->st->ckpt.n++;
}

/* Read-your-writes across the epoch: serve a committed-but-not-checkpointed
 * block (its final disk location is still stale until checkpoint). */
int ext4_ckpt_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	for (unsigned i = 0; i < fs->st->ckpt.n; i++)
		if (fs->st->ckpt.blk[i] == pbn) {
			mm_memcpy(buf, fs->st->ckpt.data[i], fs->block_size);
			return 1;
		}
	return 0;
}

/*
 * Drop any in-memory journal copy of a block that is being FREED.
 *
 * This is what a jbd2 revoke record is for, obtained here for nothing: both
 * st->txn (the running transaction) and st->ckpt (the committed-but-not-yet-
 * checkpointed batch) live only in memory -- ext4_journal_flush() writes the
 * batch to the log, checkpoints it to its final locations, empties the log and
 * resets st->ckpt.n to 0 in one go.  So a copy sitting in either of them has
 * never reached the disk, and simply forgetting it means there is nothing that
 * could ever be replayed over the block's next use.
 *
 * What this replaces: setting a force-checkpoint flag, which made the very next
 * commit do a full durable journal flush -- log write, checkpoint, two device
 * syncs -- on the spot.  Deleting a file typically frees blocks written moments
 * earlier and therefore still in the batch, so EVERY unlink forced one.  Claws
 * Mail removes its cache and temporary files as it exits, which is why exiting
 * took five to seven seconds of USB round trips.
 *
 * The data buffers are owned by the arrays and reused across flushes (see
 * ext4_ckpt_merge: a slot allocates only if its pointer is NULL), so removal
 * swaps the entry with the last one and keeps BOTH buffers -- overwriting the
 * pointer would leak a block-sized allocation per freed block.
 */
void ext4_drop_journal_copy(ext4_fs_t *fs, unsigned long pbn)
{
	for (unsigned i = 0; i < fs->st->txn.n; i++) {
		if (fs->st->txn.blk[i] != pbn)
			continue;
		uint8_t *keep = fs->st->txn.data[i];

		fs->st->txn.blk[i] = fs->st->txn.blk[fs->st->txn.n - 1];
		fs->st->txn.data[i] = fs->st->txn.data[fs->st->txn.n - 1];
		fs->st->txn.data[fs->st->txn.n - 1] = keep;
		fs->st->txn.n--;
		break;
	}
	for (unsigned i = 0; i < fs->st->ckpt.n; i++) {
		if (fs->st->ckpt.blk[i] != pbn)
			continue;
		uint8_t *keep = fs->st->ckpt.data[i];

		fs->st->ckpt.blk[i] = fs->st->ckpt.blk[fs->st->ckpt.n - 1];
		fs->st->ckpt.data[i] = fs->st->ckpt.data[fs->st->ckpt.n - 1];
		fs->st->ckpt.data[fs->st->ckpt.n - 1] = keep;
		fs->st->ckpt.n--;
		break;
	}
}

/* ===================================================================
 * Journal replay (jbd2 recovery) — replay-on-mount half.
 *
 * Closes the integrity hole where mount accepted the RECOVER incompat flag but
 * ignored it.  When the filesystem was not cleanly unmounted, jbd2 left
 * committed-but-not-yet-checkpointed transactions in the journal (inode #8);
 * we replay them to their final on-disk locations before any write touches the
 * filesystem.  Three passes over the circular log (classic jbd2 algorithm):
 *   SCAN   - find end_txn: the sequence just past the last COMMIT block.
 *   REVOKE - collect (block -> highest seq revoked) so superseded copies are
 *            not replayed.
 *   REPLAY - copy every committed descriptor tag's block to its final location
 *            unless a revoke at >= its sequence supersedes it.
 *
 * The jbd2 journal is BIG-ENDIAN on disk (unlike ext4 itself).  On a csum v2/v3
 * journal ALL three log checksums are verified on replay — the descriptor-block
 * TAIL csum (ext4_jdesc_csum_ok), each per-block TAG csum (ext4_jblock_csum),
 * and the per-transaction COMMIT-block csum (ext4_jcommit_csum_ok) — so a torn
 * or bit-rotted transaction is never applied (a bad descriptor/commit stops
 * replay there; a bad single block is not written).  The WRITE side stamps the
 * matching csum v3 (see ext4_txn_flush), so journaled writes are now enabled on
 * csum-v3 journals.  Replay is otherwise gated on the jbd2 magic + a contiguous
 * run of sequence numbers, correct for cleanly-written journals.  ASYNC_COMMIT
 * and fast-commit are still not specially handled.
 * =================================================================== */

static inline uint16_t be16(uint16_t v)
{
	return __builtin_bswap16(v);
}

/* Circular-log advance: log blocks live in [s_first, s_maxlen). */
static unsigned long jlog_advance(unsigned long cur, unsigned long n,
				  unsigned long first, unsigned long maxlen)
{
	unsigned long span = (maxlen > first) ? (maxlen - first) : 0;
	if (span == 0)
		return cur;
	return first + (((cur - first) + n) % span);
}

/* Read journal log block `lbno` (journal-file logical block) into buf. */
static int jlog_read(ext4_fs_t *fs, unsigned long lbno, void *buf)
{
	/* The map checks the block against the journal's own blocks (the
	 * system zone, ext4_journal_check) and answers 0 for any other. */
	unsigned long pbn = ext4_block_map(fs, fs->journal_inum, lbno);
	if (pbn == 0)
		return ST_IO; /* the journal file must be fully allocated */
	return ext4_read_block(fs, pbn, buf);
}

/* Write journal log block `lbno` (raw, bypassing the metadata cache and the
 * transaction capture — these go to the on-disk journal, not final FS blocks). */
static int jlog_write(ext4_fs_t *fs, unsigned long lbno, const void *buf)
{
	if (ext4_journal_aborted(fs))
		return ST_ROFS; /* nothing reaches an aborted log */
	unsigned long pbn = ext4_block_map(fs, fs->journal_inum, lbno);
	if (pbn == 0 || !ext4_write_target_ok(fs, pbn, 1))
		return ST_IO;
	return ext4_write_sectors(
		fs->bdev, fs->part_lba_offset + pbn * fs->sectors_per_block,
		fs->sectors_per_block, buf);
}

/* Revoke table: dynamic array of {block, highest-revoked-seq}. */
typedef struct {
	unsigned long block;
	uint32_t seq;
} jrev_ent_t;
typedef struct {
	jrev_ent_t *v;
	unsigned n, cap;
} jrev_tbl_t;

static void jrev_add(jrev_tbl_t *t, unsigned long block, uint32_t seq)
{
	for (unsigned i = 0; i < t->n; i++)
		if (t->v[i].block == block) { /* keep the latest revoke   */
			if (seq > t->v[i].seq)
				t->v[i].seq = seq;
			return;
		}
	if (t->n == t->cap) {
		unsigned ncap = t->cap ? t->cap * 2 : 64;
		jrev_ent_t *nv =
			(jrev_ent_t *)kalloc(ncap * sizeof(jrev_ent_t));
		if (!nv) {
			WARN_ON_ONCE(1);
			return;
		} /* drop: at worst over-replay */
		if (t->v) {
			mm_memcpy(nv, t->v, t->n * sizeof(jrev_ent_t));
			kfree(t->v);
		}
		t->v = nv;
		t->cap = ncap;
	}
	t->v[t->n].block = block;
	t->v[t->n].seq = seq;
	t->n++;
}

/* A block is superseded (skip replay) if revoked at a seq >= the txn seq. */
static int jrev_test(const jrev_tbl_t *t, unsigned long block, uint32_t seq)
{
	for (unsigned i = 0; i < t->n; i++)
		if (t->v[i].block == block && t->v[i].seq >= seq)
			return 1;
	return 0;
}

/* jbd2 csum v2/v3 seed = crc32c(~0, journal_uuid).  Folds with the BE
 * transaction sequence for the per-block TAG csum; used directly for the
 * descriptor-tail and commit-block csums.  (The journal SUPERBLOCK csum is
 * different: seed ~0 over the whole 1024-byte sb — see ext4_jsb_csum_set.) */
static inline uint32_t ext4_jcsum_seed(const journal_superblock_t *jsb)
{
	return ext4_crc32c(0xFFFFFFFFu, jsb->s_uuid, sizeof(jsb->s_uuid));
}

/* Per-block TAG checksum (csum v2/v3): crc32c(seed, BE(seq)) folded with the
 * journalled (escaped) block bytes.  v3 stores the full 32 bits in the tag,
 * v2 the low 16.  Computed over the exact bytes written to the log, so callers
 * must verify BEFORE un-escaping (and stamp AFTER escaping). */
static uint32_t ext4_jblock_csum(uint32_t seed, uint32_t seq, const void *blk,
				 unsigned bs)
{
	uint32_t seq_be = __builtin_bswap32(seq);
	uint32_t c = ext4_crc32c(seed, &seq_be, sizeof(seq_be));
	return ext4_crc32c(c, blk, bs);
}

/* Descriptor/revoke TAIL checksum (csum v2/v3): the last 4 bytes hold
 * crc32c(seed, whole block) computed with that tail field zeroed. */
static int ext4_jdesc_csum_ok(uint32_t seed, uint8_t *blk, unsigned bs)
{
	uint32_t *ptail = (uint32_t *)(blk + bs - 4);
	uint32_t raw = *ptail;
	*ptail = 0;
	uint32_t calc = ext4_crc32c(seed, blk, bs);
	*ptail = raw;
	return be32(raw) == calc;
}

/* jbd2 csum v2/v3 commit-block checksum (big-endian on disk).  The commit block
 * is valid iff crc32c(seed, block) with h_chksum[0] zeroed equals the stored
 * value.  h_chksum[0] sits at offset 0x10 (12-byte journal_header +
 * h_chksum_type/size/pad = 4).  A torn or bit-rotted commit fails this, so on a
 * csum journal we stop replay there instead of applying an incomplete
 * transaction.  FAIL-SAFE: a false negative only stops replay earlier (fs stays
 * consistent, as if the crash happened a txn sooner). */
static int ext4_jcommit_csum_ok(uint32_t seed, uint8_t *blk, unsigned bs)
{
	uint32_t *pchk = (uint32_t *)(blk + JBD2_COMMIT_CSUM_OFF);
	uint32_t raw = *pchk; /* stored be32 csum             */
	*pchk = 0;
	uint32_t calc = ext4_crc32c(seed, blk, bs);
	*pchk = raw; /* restore the buffer           */
	return be32(raw) == calc;
}

/* Stamp the journal SUPERBLOCK checksum (csum v2/v3): s_checksum @0xFC =
 * crc32c(~0, first 1024 bytes with that field zeroed).  Note the ~0 seed (the
 * uuid is inside the summed region), unlike the tag/desc/commit csums.  No-op
 * unless `enabled`.  `jsbbuf` is a full-block buffer. */
static void ext4_jsb_csum_set(uint8_t *jsbbuf, int enabled)
{
	if (!enabled)
		return;
	uint32_t *pc = (uint32_t *)(jsbbuf + JBD2_SB_CSUM_OFF);
	*pc = 0;
	*pc = __builtin_bswap32(
		ext4_crc32c(0xFFFFFFFFu, jsbbuf, JBD2_SB_CSUM_LEN));
}

#define EXT4_JPASS_SCAN 0
#define EXT4_JPASS_REVOKE 1
#define EXT4_JPASS_REPLAY 2

/* One pass over the committed log.  SCAN sets *end_txn (sequence past the last
 * commit); REVOKE/REPLAY are bounded by it.  Returns ST_OK or an I/O error. */
static int ext4_journal_pass(ext4_fs_t *fs, const journal_superblock_t *jsb,
			     int pass, uint32_t *end_txn, jrev_tbl_t *revtbl,
			     unsigned long *out_replayed)
{
	unsigned long first = be32(jsb->s_first);
	unsigned long maxlen = be32(jsb->s_maxlen);
	uint32_t jincompat = be32(jsb->s_feature_incompat);
	int is64 = (jincompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0;
	int csum3 = (jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0;
	int csum2 = (jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) != 0;
	int csum_any = csum2 || csum3; /* descriptor/commit csums    */
	unsigned tag_sz = csum3 ? 16u : (is64 ? 12u : 8u);
	uint32_t jseed = ext4_jcsum_seed(jsb);

	unsigned long next_log = be32(jsb->s_start);
	uint32_t next_seq = be32(jsb->s_sequence);
	unsigned long replayed = 0;
	int rc = ST_OK;

	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	uint8_t *data = (uint8_t *)kalloc(fs->block_size);
	if (!blk || !data) {
		if (blk)
			kfree(blk);
		if (data)
			kfree(data);
		return ST_NOMEM;
	}

	for (;;) {
		if (pass != EXT4_JPASS_SCAN && next_seq >= *end_txn)
			break; /* all committed txns done */
		if (jlog_read(fs, next_log, blk) != ST_OK) {
			rc = ST_IO;
			break;
		}
		const journal_header_t *h = (const journal_header_t *)blk;
		if (be32(h->h_magic) != JBD2_MAGIC_NUMBER)
			break; /* end of log     */
		if (be32(h->h_sequence) != next_seq)
			break; /* stale: end     */

		uint32_t bt = be32(h->h_blocktype);
		if (bt == JBD2_DESCRIPTOR_BLOCK) {
			/* csum v2/v3: verify the descriptor TAIL csum before trusting any
             * tag.  A bad tail means a torn/corrupt descriptor — stop here, so
             * the tag walk never advances next_log off garbage. */
			if (csum_any &&
			    !ext4_jdesc_csum_ok(jseed, blk, fs->block_size)) {
				if (pass == EXT4_JPASS_SCAN)
					kprintf("ext4: journal descriptor csum bad at seq %u; "
						"stopping replay there\n",
						next_seq);
				break;
			}
			unsigned off = sizeof(journal_header_t);
			unsigned long data_idx = 0;
			while (off + tag_sz <= fs->block_size) {
				const uint8_t *tag = blk + off;
				uint32_t blocknr =
					be32(*(const uint32_t *)(tag + 0));
				uint32_t flags =
					csum3 ? be32(*(const uint32_t
							       *)(tag +
								  JBD2_TAG3_FLAGS_OFF)) :
						be16(*(const uint16_t *)(tag +
									 6));
				unsigned long target = blocknr;
				if (is64) /* high 32 at +8 (both)*/
					target |=
						((unsigned long)be32(*(
							 const uint32_t
								 *)(tag +
								    JBD2_TAG3_BLKHI_OFF))
						 << 32);

				if (pass == EXT4_JPASS_REPLAY) {
					/* The journalled copy is 1 + data_idx blocks past the
                     * descriptor in the log. */
					unsigned long dl = jlog_advance(
						next_log, 1 + data_idx, first,
						maxlen);
					if (jlog_read(fs, dl, data) != ST_OK) {
						rc = ST_IO;
						goto out;
					}
					/* Per-block TAG csum (csum v2/v3) is over the journalled
                     * (escaped) bytes — verify BEFORE restoring the magic. */
					int tagbad = 0;
					if (csum_any) {
						uint32_t want =
							ext4_jblock_csum(
								jseed, next_seq,
								data,
								fs->block_size);
						int ok =
							csum3 ? (be32(*(const uint32_t
										*)(tag +
										   JBD2_TAG3_CSUM_OFF)) ==
								 want) :
								(be16(*(const uint16_t
										*)(tag +
										   JBD2_TAG_V2_CSUM_OFF)) ==
								 (uint16_t)
									 want);
						if (!ok) {
							kprintf("ext4: journal block tag csum bad (block %lu, "
								"seq %u); not applying that block\n",
								target,
								next_seq);
							tagbad = 1;
						}
					}
					if (flags &
					    JBD2_FLAG_ESCAPE) { /* restore escaped magic */
						uint32_t m = __builtin_bswap32(
							JBD2_MAGIC_NUMBER);
						mm_memcpy(data, &m, 4);
					}
					if (target == 0) {
						/* An otherwise csum-valid descriptor should never
						 * carry a block-0 tag (the writer refuses pbn 0);
						 * log enough context to identify the source. */
						kprintf("ext4: journal seq %u tag %lu (of a %s descriptor at log block %lu) targets block 0 - skipped\n",
							next_seq, data_idx,
							tagbad ? "csum-BAD" :
								 "csum-ok",
							next_log);
					} else if (!tagbad &&
						   !jrev_test(revtbl, target,
							      next_seq)) {
						/* A tag naming a block past the
						 * end of the filesystem: the
						 * journal is damaged, and the
						 * replay stops before writing
						 * anything there. */
						if (target >= fs->blocks_count) {
							kprintf("ext4: journal seq %u tag %lu names block %lu, beyond the filesystem (%lu blocks): replay stopped\n",
								next_seq,
								data_idx,
								target,
								fs->blocks_count);
							rc = ST_INVALID;
							goto out;
						}
						if (ext4_write_block(fs, target,
								     data) !=
						    ST_OK) {
							rc = ST_IO;
							goto out;
						}
						replayed++;
					}
				}
				data_idx++;
				off += tag_sz;
				if (!(flags & JBD2_FLAG_SAME_UUID))
					off += 16; /* a UUID follows tag  */
				if (flags & JBD2_FLAG_LAST_TAG)
					break;
			}
			next_log = jlog_advance(next_log, 1 + data_idx, first,
						maxlen);
		} else if (bt == JBD2_COMMIT_BLOCK) {
			if (csum_any &&
			    !ext4_jcommit_csum_ok(jseed, blk, fs->block_size)) {
				/* Torn/corrupt commit on a csum journal: this transaction never
                 * completed, so it — and everything after — must NOT be replayed. */
				if (pass == EXT4_JPASS_SCAN)
					kprintf("ext4: journal commit csum bad at seq %u; "
						"stopping replay there\n",
						next_seq);
				break;
			}
			next_seq++; /* transaction boundary */
			if (pass == EXT4_JPASS_SCAN)
				*end_txn = next_seq;
			next_log = jlog_advance(next_log, 1, first, maxlen);
		} else if (bt == JBD2_REVOKE_BLOCK) {
			if (pass == EXT4_JPASS_REVOKE) {
				const jbd2_revoke_header_t *rh =
					(const jbd2_revoke_header_t *)blk;
				unsigned used = be32(rh->r_count);
				if (used > fs->block_size)
					used = fs->block_size;
				unsigned rsz = is64 ? 8u : 4u;
				unsigned p = sizeof(jbd2_revoke_header_t);
				while (p + rsz <= used) {
					unsigned long rb = be32(
						*(const uint32_t *)(blk + p));
					if (is64)
						rb |= ((unsigned long)be32(*(
							       const uint32_t
								       *)(blk +
									  p +
									  4))
						       << 32);
					jrev_add(revtbl, rb, next_seq);
					p += rsz;
				}
			}
			next_log = jlog_advance(next_log, 1, first, maxlen);
		} else {
			break; /* unknown type: end   */
		}
	}
out:
	kfree(blk);
	kfree(data);
	if (out_replayed)
		*out_replayed = replayed;
	return rc;
}

/* ---- The journal inode at mount ------------------------------------------
 *
 * The log is written to whatever blocks the journal inode maps, so before
 * anything is replayed or committed its map is walked once: every block it
 * maps has to lie inside the filesystem, clear of the groups' metadata and
 * mapped once, and the journal superblock's geometry has to fit the inode.
 * Those blocks then join the system zone, owned by the journal inode, so
 * that no other inode can map them and no allocation can hand them out.  A
 * journal failing this is corrupt and the filesystem is not mounted. */

struct ext4_jcheck {
	struct ext4_sysz *v;
	unsigned n, cap;
	unsigned long sb_pbn; /* where log block 0 is (0: unmapped) */
};

static int ext4_jcheck_run(ext4_fs_t *fs, void *arg, unsigned long lblk,
			   unsigned long pbn, unsigned long len)
{
	struct ext4_jcheck *jc = (struct ext4_jcheck *)arg;

	if (!ext4_blk_valid(fs, fs->journal_inum, pbn, len)) {
		kprintf("ext4: journal inode %u maps blocks %lu-%lu, outside "
			"the data area or on filesystem metadata\n",
			fs->journal_inum, pbn, pbn + len - 1);
		return ST_INVALID;
	}
	if (lblk == 0)
		jc->sb_pbn = pbn;
	if (jc->n == jc->cap) {
		unsigned ncap = jc->cap ? jc->cap * 2 : 16;
		struct ext4_sysz *nv =
			(struct ext4_sysz *)kalloc(ncap * sizeof(*nv));

		if (!nv)
			return ST_NOMEM;
		if (jc->v) {
			mm_memcpy(nv, jc->v, jc->n * sizeof(*nv));
			kfree(jc->v);
		}
		jc->v = nv;
		jc->cap = ncap;
	}
	jc->v[jc->n++] = (struct ext4_sysz){ pbn, len, fs->journal_inum };
	return ST_OK;
}

int ext4_journal_check(ext4_fs_t *fs)
{
	ext4_inode in;
	struct ext4_jcheck jc = { 0, 0, 0, 0 };
	int rc;

	if (fs->journal_inum == 0)
		return ST_OK;
	if (!ext4_get_inode_cached(fs, fs->journal_inum, &in) ||
	    (in.i_mode & S_IFMT) != S_IFREG) {
		kprintf("ext4: journal inode %u is not a readable regular file; "
			"not mounted\n",
			fs->journal_inum);
		return ST_INVALID;
	}
	unsigned long nblocks = ext4_inode_size(&in) / fs->block_size;
	if (nblocks > fs->blocks_count) {
		kprintf("ext4: journal inode %u is larger than the filesystem; "
			"not mounted\n",
			fs->journal_inum);
		return ST_INVALID;
	}

	struct ext4_walk w = { ext4_jcheck_run, &jc, fs->journal_inum, nblocks,
			       0, 0, 0 };
	if (in.i_flags & EXT4_INODE_EXTENTS_FL)
		rc = ext4_ext_walk(fs, &w, &in);
	else
		rc = ext4_ind_walk(fs, &w, &in);
	if (rc == ST_OK)
		rc = ext4_walk_emit(fs, &w, 0, 0, 0); /* the last run */
	if (rc != ST_OK) {
		if (rc != ST_NOMEM)
			kprintf("ext4: journal inode %u block map is corrupt; "
				"not mounted\n",
				fs->journal_inum);
		if (jc.v)
			kfree(jc.v);
		return rc == ST_NOMEM ? ST_NOMEM : ST_INVALID;
	}

	/* The geometry of a journal this driver would use (one it does not
	 * recognise is left alone, as before): the log lies in [s_first,
	 * s_maxlen) of the inode. */
	if (jc.sb_pbn) {
		uint8_t *b = (uint8_t *)ext4_bget(fs);

		if (!b) {
			if (jc.v)
				kfree(jc.v);
			return ST_NOMEM;
		}
		if (ext4_read_block(fs, jc.sb_pbn, b) == ST_OK) {
			const journal_superblock_t *jsb =
				(const journal_superblock_t *)b;
			uint32_t bt = be32(jsb->s_header.h_blocktype);
			unsigned long first = be32(jsb->s_first);
			unsigned long maxlen = be32(jsb->s_maxlen);

			if (be32(jsb->s_header.h_magic) == JBD2_MAGIC_NUMBER &&
			    (bt == JBD2_SUPERBLOCK_V1 ||
			     bt == JBD2_SUPERBLOCK_V2) &&
			    (maxlen > nblocks || first == 0 ||
			     first >= maxlen)) {
				kprintf("ext4: journal log blocks %lu-%lu do not "
					"fit the journal inode (%lu blocks); "
					"not mounted\n",
					first, maxlen, nblocks);
				rc = ST_INVALID;
			}
		}
		ext4_bput(fs, b);
	}
	if (rc == ST_OK)
		rc = ext4_setup_system_zone(fs, jc.v, jc.n);
	if (jc.v)
		kfree(jc.v);
	return rc;
}

/* Replay the journal of `fs`.  Safe to call on every mount that has a journal:
 * it reads the journal superblock and no-ops silently when the log is clean
 * (s_start == 0).  This makes the journal's own log-tail pointer the source of
 * truth for "needs recovery", independent of the ext4 RECOVER flag — the two
 * live in different sectors and a device write-back cache can persist one
 * without the other across a crash (observed: VMware kept the committed
 * journal but not the RECOVER bit, so a RECOVER-only gate missed the replay).
 * Sets *did_work = 1 iff blocks were actually replayed (caller must then drop
 * its metadata caches).  Returns ST_OK on success including the no-op cases. */
/* Replay the journal.  With `dry', nothing is written: ST_ROFS says the
 * journal holds committed transactions a replay would write, ST_OK that it
 * holds none. */
int ext4_journal_recover(ext4_fs_t *fs, int *did_work, int dry)
{
	if (did_work)
		*did_work = 0;
	if (fs->journal_inum == 0)
		return ST_OK;
	uint8_t *jblk = (uint8_t *)kalloc(fs->block_size);
	if (!jblk)
		return ST_NOMEM;

	/* The journal superblock is journal log block 0. */
	unsigned long sb_pbn = ext4_block_map(fs, fs->journal_inum, 0);
	if (sb_pbn == 0 || ext4_read_block(fs, sb_pbn, jblk) != ST_OK) {
		kfree(jblk);
		return ST_IO;
	}
	journal_superblock_t *jsb = (journal_superblock_t *)jblk;
	if (be32(jsb->s_header.h_magic) != JBD2_MAGIC_NUMBER) {
		kprintf("ext4: bad journal superblock magic\n");
		kfree(jblk);
		return ST_INVALID;
	}
	uint32_t sbt = be32(jsb->s_header.h_blocktype);
	if (sbt != JBD2_SUPERBLOCK_V1 && sbt != JBD2_SUPERBLOCK_V2) {
		kprintf("ext4: unexpected journal sb blocktype %u\n", sbt);
		kfree(jblk);
		return ST_INVALID;
	}
	if (be32(jsb->s_blocksize) != fs->block_size) {
		kprintf("ext4: journal blocksize %u != fs %u (unsupported)\n",
			be32(jsb->s_blocksize), fs->block_size);
		kfree(jblk);
		return ST_UNSUPPORTED;
	}

	uint32_t jincompat = be32(jsb->s_feature_incompat);
	uint32_t known =
		JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT |
		JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3 |
		JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT |
		JBD2_FEATURE_INCOMPAT_FAST_COMMIT;
	if (jincompat & ~known)
		kprintf("ext4: journal unknown incompat 0x%x; attempting replay anyway\n",
			jincompat & ~known);
	if (jincompat &
	    (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))
		kprintf("ext4: journal csum-v%d: descriptor/tag/commit csums verified on replay\n",
			(jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) ? 3 : 2);
	if (jincompat & (JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT |
			 JBD2_FEATURE_INCOMPAT_FAST_COMMIT))
		kprintf("ext4: journal async/fast-commit not specially handled (tolerant replay)\n");

	/* Always report the journal state at mount so a "did it recover?" question
     * is never a guess.  s_start is the log tail: 0 == clean (nothing to
     * replay), non-zero == a committed-but-uncheckpointed transaction exists. */
	uint32_t jstart = be32(jsb->s_start);
	kprintf("ext4: journal state s_start=%u seq=%u recover_flag=%d\n",
		jstart, be32(jsb->s_sequence),
		(fs->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) ? 1 : 0);
	if (jstart == 0) {
		kfree(jblk);
		return ST_OK; /* clean log: nothing to replay     */
	}
	if (!dry)
		kprintf("ext4: replaying journal (s_start=%u)...\n", jstart);

	/* Pass 1 — scan to the end of the committed log. */
	uint32_t end_txn = be32(jsb->s_sequence);
	jrev_tbl_t revtbl = { 0, 0, 0 };
	int rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_SCAN, &end_txn, &revtbl,
				   0);
	if (rc != ST_OK) {
		kfree(jblk);
		return rc;
	}
	if (end_txn == be32(jsb->s_sequence)) {
		kprintf("ext4: journal has no committed transactions; nothing to replay\n");
		kfree(jblk);
		return ST_OK;
	}
	if (dry) {
		kfree(jblk);
		return ST_ROFS; /* a replay would write */
	}

	/* Pass 2 — collect revoke records (only if the journal uses them). */
	if (jincompat & JBD2_FEATURE_INCOMPAT_REVOKE) {
		rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_REVOKE, &end_txn,
				       &revtbl, 0);
		if (rc != ST_OK) {
			if (revtbl.v)
				kfree(revtbl.v);
			kfree(jblk);
			return rc;
		}
	}

	/* Pass 3 — replay committed blocks to their final locations. */
	unsigned long replayed = 0;
	rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_REPLAY, &end_txn, &revtbl,
			       &replayed);
	if (revtbl.v)
		kfree(revtbl.v);
	if (rc != ST_OK) {
		kfree(jblk);
		return rc;
	}

	/* Mark the journal empty: s_start = 0, s_sequence = end_txn.  On a csum
     * journal the superblock carries its own checksum (seed ~0 over 1024 bytes),
     * so restamp it after editing or e2fsck will reject the journal sb. */
	jsb->s_start = __builtin_bswap32(0);
	jsb->s_sequence = __builtin_bswap32(end_txn);
	ext4_jsb_csum_set(jblk,
			  (jincompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 |
					JBD2_FEATURE_INCOMPAT_CSUM_V3)) != 0);
	if (ext4_write_block(fs, sb_pbn, jblk) != ST_OK) {
		kfree(jblk);
		return ST_IO;
	}
	ext4_dev_sync(fs, "replay"); /* persist before RECOVER clears */

	kprintf("ext4: journal replay complete (%lu block(s) recovered, end_txn=%u)\n",
		replayed, end_txn);
	if (did_work && replayed > 0)
		*did_work = 1;
	kfree(jblk);
	return ST_OK;
}

/* Checkpoint batch entries [lo, hi): write each to its final location, make
 * them durable, then mark the journal log empty (s_start=0).  TWO device
 * syncs.  Ordering: the final writes are made durable BEFORE s_start=0, so a
 * crash mid-checkpoint replays the transaction again (idempotent), and a
 * crash after s_start=0 finds the final data already durable.  Leaves
 * RECOVER set (only ext4_journal_clean clears it) -- s_start=0 alone makes
 * recovery a no-op.  Called with st->txn inactive, so the writeback goes
 * direct.  Returns ST_OK, or an error with the log left as it is (the
 * committed transaction stays replayable). */
static int ext4_checkpoint(ext4_fs_t *fs, unsigned lo, unsigned hi)
{
	int failed = 0;
	for (unsigned i = lo; i < hi; i++) /* 1. pending -> final     */
		if (ext4_write_block_direct(fs, fs->st->ckpt.blk[i],
					    fs->st->ckpt.data[i]) != ST_OK)
			failed = 1;
	/* The superblock (free counts) rides the batch instead of being written on
     * every allocating op — it becomes durable here with the matching GDT/bitmaps,
     * so the on-disk super and group descriptors are always consistent at a flush
     * (and mount recomputes the free totals from the GDT anyway).  Re-derive the
     * free totals from the (authoritative) GDT first so the super can never drift
     * from the descriptors — the same self-healing the mount path applies. */
	{
		int is64 = (fs->feature_incompat &
			    EXT4_FEATURE_INCOMPAT_64BIT) != 0;
		uint64_t fb = 0;
		uint32_t fi = 0;
		for (unsigned g = 0; g < fs->groups_count; g++) {
			uint32_t gb = fs->gdt[g].bg_free_blocks_count_lo;
			uint32_t gi = fs->gdt[g].bg_free_inodes_count_lo;
			if (is64) {
				gb |= (uint32_t)fs->gdt[g]
					      .bg_free_blocks_count_hi
				      << 16;
				gi |= (uint32_t)fs->gdt[g]
					      .bg_free_inodes_count_hi
				      << 16;
			}
			fb += gb;
			fi += gi;
		}
		fs->sb_copy.s_free_blocks_count_lo = (uint32_t)fb;
		fs->sb_copy.s_free_blocks_count_hi = (uint32_t)(fb >> 32);
		fs->sb_copy.s_free_inodes_count = fi;
	}
	if (ext4_write_super(fs) != ST_OK)
		failed = 1;
	if (ext4_dev_sync(fs, "checkpoint-final") !=
	    ST_OK) /* finals durable BEFORE s_start=0 */
		failed = 1;

	/* If ANY final write or its barrier failed, the on-disk final locations
	 * are an incomplete image of this batch.  Emptying the log (s_start=0) and
	 * dropping the in-memory batch here would be a SILENT, UNRECOVERABLE revert
	 * of every block whose final write failed — e.g. a directory's inode-table
	 * block reverts below its already-persisted grown extent tree / htree leaf,
	 * which is exactly the "/tmp entry vanishes, extent past i_size" corruption.
	 * Instead keep the committed transaction in the journal (so a later
	 * checkpoint, or a mount-time replay, can complete it) and keep st->ckpt (so
	 * read-your-writes still serves the fresh content and no reader observes the
	 * half-written disk).  The next flush re-commits the batch (a superset,
	 * latest-content-wins) from j_first and retries the checkpoint. */
	if (failed) {
		/* An abort (an error found meanwhile) refuses the writes on
		 * purpose; the transaction stays in the log for the next
		 * mount to replay, and that is all there is to say. */
		WARN_RATELIMIT(!ext4_journal_aborted(fs),
			       "ext4: checkpoint incomplete (device write/flush "
			       "failed) - keeping journal + batch for recovery");
		return ST_IO;
	}

	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	jsb->s_start = 0; /* 2. empty the log        */
	jsb->s_sequence = __builtin_bswap32(fs->j_sequence);
	ext4_jsb_csum_set(fs->j_sb_buf, fs->j_csum3);
	jlog_write(fs, 0, fs->j_sb_buf);
	ext4_dev_sync(fs, "checkpoint-jsb");
	fs->st->jhead = fs->j_first;
	return ST_OK;
}

/* ===================================================================
 * Journaled writes (ordered mode), deferred-checkpoint circular log.
 *
 * Called from ext4_io_unlock on the outermost release with the lock held.  Each
 * op's buffered metadata (st->txn) is journalled to the log head and made durable
 * with ONE device sync, then recorded in the in-memory epoch pending set
 * (st->ckpt) — it is NOT checkpointed to its final location yet.  Steady-state
 * cost is therefore one sync per op; the 2-sync checkpoint (ext4_checkpoint) is
 * amortised over a whole epoch and runs only when the log/pending set fills, the
 * batch fills, or on fsync/sync.
 *
 * Crash safety: recovery reads s_start (the epoch start, == j_first, made
 * durable by the epoch's first commit sync) and replays every committed txn in
 * sequence order until the chain breaks (torn/absent commit, validated by the
 * csum-v3 commit + descriptor + tag csums).  Because checkpoint is deferred, the
 * committed metadata lives only in the log until then, so reads consult st->ckpt
 * (read-your-writes) — critically, the allocator reads bitmaps from there, so it
 * never double-allocates a block the epoch already used.  Monotonic sequences +
 * never letting an epoch wrap (we checkpoint before the head reaches j_maxlen)
 * mean a stale older epoch left in the log can never extend the new chain.
 * Data blocks are written in place during the op and flushed by the commit sync
 * ("ordered").  No jbd2 revoke records are needed: a journal copy lives only in
 * memory until this function writes it, so a block that is freed simply has its
 * copy dropped from the in-memory batch (ext4_drop_journal_copy) and there is
 * nothing left that could be replayed over the block's next use.
 * =================================================================== */
/* Durably commit the whole in-memory batch (st->ckpt) as ONE journal transaction,
 * then checkpoint it to its final locations and empty the log.  Called lazily —
 * when the batch fills, on fsync/sync/unmount, or when a journalled block is
 * freed — NOT per operation.  This is the single device-sync cost that is now
 * amortised across an entire batch of operations.
 *
 * Crash safety is unchanged in shape from the old per-op commit, just batched:
 * the txn is made durable (commit sync) before its blocks are written to their
 * final homes (checkpoint), which is itself made durable before the log is
 * emptied (s_start=0).  Between flushes the on-disk log is empty, so a crash
 * outside the brief flush window replays nothing. */
/* ---- Ordered data: which inodes this transaction must store first --------
 *
 * Metadata that points at freshly written blocks must not reach the device
 * before the blocks do.  Otherwise a crash in between leaves an inode whose
 * extents name blocks that were never written -- and what is read back is
 * whatever those blocks held for their previous owner, which is somebody
 * else's data.
 *
 * The obvious way to guarantee it is to store EVERY dirty page before every
 * commit.  It is correct, and it is also why an unrelated program doing a
 * small write() could be held up for as long as the whole system's backlog
 * took to write: commits run inside write(), and the bill goes to whoever is
 * standing there.
 *
 * Only the files this transaction actually wrote to need ordering.  So each
 * write records its inode here, and the commit stores exactly those.  A
 * program that wrote nothing pays nothing.
 *
 * Guarded by the filesystem I/O lock, which every writer and the commit both
 * hold exclusively -- no separate lock, and none needed.
 */
#define EXT4_ORDERED_INIT_CAP 64
/* Set when an inode could not be recorded.  The set is then incomplete, so the
 * commit falls back to storing everything -- slower, never wrong. */
/* The array and its count are changed under the I/O lock as described above,
 * and also under this, so that the commit thread can read the set before it
 * takes the I/O lock (ext4_ordered_writeback). */

void ext4_ordered_add(ext4_fs_t *fs, unsigned long ino)
{
	uint64_t fl;

	if (ino == 0)
		return;

	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	for (unsigned i = 0; i < fs->st->ord_n; i++) {
		if (fs->st->ord_ino[i] == ino) {
			spin_unlock_irqrestore(&fs->st->ord_lock, fl);
			return; /* already recorded */
		}
	}
	if (fs->st->ord_n < fs->st->ord_cap) {
		fs->st->ord_ino[fs->st->ord_n++] = ino;
		spin_unlock_irqrestore(&fs->st->ord_lock, fl);
		return;
	}
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);

	/* Grown outside the spinlock (allocation may sleep).  Nothing else
	 * adds meanwhile: every writer holds the I/O lock exclusively. */
	unsigned ncap = fs->st->ord_cap ? fs->st->ord_cap * 2 : EXT4_ORDERED_INIT_CAP;
	unsigned long *n = (unsigned long *)kalloc(ncap * sizeof(unsigned long));
	unsigned long *old;

	if (!n) {
		/* Cannot record it, so the set no longer describes the
		 * transaction.  Say so; the commit will store everything
		 * rather than miss this inode. */
		fs->st->ord_overflow = 1;
		return;
	}
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	for (unsigned i = 0; i < fs->st->ord_n; i++)
		n[i] = fs->st->ord_ino[i];
	old = fs->st->ord_ino;
	fs->st->ord_ino = n;
	fs->st->ord_cap = ncap;
	fs->st->ord_ino[fs->st->ord_n++] = ino;
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	if (old)
		kfree(old);
}

/* The set as it stands, as cache ids, in a fresh array the caller frees;
 * NULL when it is empty (or there is no memory for the copy). */
static unsigned long *ext4_ordered_snapshot(ext4_fs_t *fs, unsigned *count)
{
	uint64_t fl;
	unsigned want, n = 0;
	unsigned long *copy;

	*count = 0;
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	want = fs->st->ord_n;
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	if (!want)
		return NULL;
	copy = (unsigned long *)kalloc((want + 16) * sizeof(unsigned long));
	if (!copy)
		return NULL;
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	for (unsigned i = 0; i < fs->st->ord_n && n < want + 16; i++)
		copy[n++] = EXT4_BID_ENC(fs->st->ord_ino[i], 0);
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	if (!n) {
		kfree(copy);
		return NULL;
	}
	*count = n;
	return copy;
}

/*
 * Store the data this transaction is responsible for, and forget it.
 * Caller holds the I/O lock.
 *
 * Returns non-zero if anything was written, so the caller knows whether a
 * barrier is needed before the commit.
 */
static int ext4_ordered_flush(ext4_fs_t *fs)
{
	int wrote = 0;
	unsigned n;
	unsigned long *ids;
	uint64_t fl;

	might_sleep();

	if (fs->st->ord_overflow) {
		/* The record is incomplete -- store everything instead of
		 * guessing which inode was missed. */
		WARN_ON_ONCE(1);
		wrote = pagecache_flush_sb(&fs->sb);
		fs->st->ord_overflow = 0;
		spin_lock_irqsave(&fs->st->ord_lock, &fl);
		fs->st->ord_n = 0;
		spin_unlock_irqrestore(&fs->st->ord_lock, fl);
		return wrote;
	}

	/* One pass over the dirty list for the whole set, not one per file. */
	ids = ext4_ordered_snapshot(fs, &n);
	if (ids) {
		wrote = pagecache_flush_fileset(&fs->sb, ids, n);
		kfree(ids);
	} else if (fs->st->ord_n) {
		/* no memory for the copy: store everything */
		wrote = pagecache_flush_sb(&fs->sb);
	}
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	fs->st->ord_n = 0;
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	return wrote;
}

/*
 * The same data, stored by the commit thread BEFORE it takes the I/O lock.
 *
 * The commit holds the I/O lock -- the metadata lock, exclusively -- from the
 * moment it starts until the transaction is on the device, and every lookup,
 * stat and page-in on the filesystem waits for it meanwhile.  Storing the
 * transaction's file data inside that hold made it as long as the data took to
 * reach the device: after a browser had cached a few hundred megabytes of
 * video, tens of seconds on a USB stick, with every program that needed a page
 * from disk frozen.  Written here first, a batch at a time (each batch takes
 * the lock only around its own device writes, so readers get their turn in
 * between), the commit is left with only what was written in the last moment.
 * The order the commit promises -- data before the metadata that refers to it
 * -- is kept: the commit still stores whatever is dirty, under the lock.
 */
static void ext4_ordered_writeback(ext4_fs_t *fs)
{
	unsigned n;
	unsigned long *ids = ext4_ordered_snapshot(fs, &n);

	if (!ids)
		return;
	(void)pagecache_flush_fileset(&fs->sb, ids, n);
	kfree(ids);
}

/* ---- Transactions in the log ---------------------------------------------
 *
 * One transaction is laid out the way every journal reader expects it:
 *
 *	descriptor, its blocks, [descriptor, its blocks, ...] commit
 *
 * all carrying the transaction's sequence number.  A descriptor holds as
 * many tags as fit in front of its tail (the 4-byte checksum tail of a
 * csum-v3 journal); the first tag of each descriptor is followed by the
 * journal's UUID, the others say SAME_UUID, and the last one of each
 * descriptor says LAST_TAG.  So a batch is never limited by what one
 * descriptor block holds -- with 1 KB blocks that is only 62 tags. */

/* Tags one descriptor block holds. */
static unsigned long ext4_jdesc_tags(ext4_fs_t *fs, int csum3, int is64)
{
	unsigned tagsz = csum3 ? JBD2_TAG3_SIZE : (is64 ? 12u : 8u);
	unsigned space = fs->block_size - sizeof(journal_header_t) -
			 (csum3 ? 4u : 0u) - 16u; /* the first tag's UUID */
	return space / tagsz;
}

/* Log blocks a transaction of `n' metadata blocks takes. */
static unsigned long ext4_jtxn_blocks(ext4_fs_t *fs, unsigned long n)
{
	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	int is64 = (be32(jsb->s_feature_incompat) &
		    JBD2_FEATURE_INCOMPAT_64BIT) != 0;
	/* An upgrade still to come (ext4_journal_upgrade) can only make the
	 * tags larger: count with the larger ones. */
	int csum3 = fs->j_csum3 || fs->st->j_upgrade;
	unsigned long per = ext4_jdesc_tags(fs, csum3, is64);

	if (per == 0)
		return (unsigned long)-1;
	return n + (n + per - 1) / per + 1;
}

/* Log blocks available to one transaction. */
static unsigned long ext4_jlog_span(ext4_fs_t *fs)
{
	return fs->j_maxlen > fs->j_first ? fs->j_maxlen - fs->j_first : 0;
}

/* The most metadata blocks one transaction can carry in this log. */
static unsigned long ext4_jtxn_max(ext4_fs_t *fs)
{
	unsigned long lo = 0, hi = ext4_jlog_span(fs);

	while (lo < hi) { /* largest m with ext4_jtxn_blocks(m) <= span */
		unsigned long m = lo + (hi - lo + 1) / 2;
		if (ext4_jtxn_blocks(fs, m) <= ext4_jlog_span(fs))
			lo = m;
		else
			hi = m - 1;
	}
	return lo;
}

/* Bring the journal to csum v3 on a metadata_csum filesystem whose journal
 * mkfs left plain (or with CSUM_V3 but no checksum type, which journal
 * readers reject).  Done by the first transaction this mount writes, not by
 * the mount itself: a mount that changes nothing writes nothing.  Only with
 * an empty log -- the format of the transactions in it must not change
 * under them.  Sets CSUM_V3 + s_checksum_type=crc32c and clears the
 * obsolete v1 compat checksum in the cached journal superblock, which the
 * caller stores next (with s_start pointing at the new transaction). */
static void ext4_journal_upgrade(ext4_fs_t *fs)
{
	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	int was_v3 = fs->j_csum3;

	if (!fs->st->j_upgrade || jsb->s_start != 0)
		return;
	fs->st->j_upgrade = 0;
	jsb->s_feature_incompat = __builtin_bswap32(
		be32(jsb->s_feature_incompat) | JBD2_FEATURE_INCOMPAT_CSUM_V3);
	jsb->s_feature_compat = __builtin_bswap32(
		be32(jsb->s_feature_compat) & ~JBD2_FEATURE_COMPAT_CHECKSUM);
	fs->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF] = JBD2_CRC32C_CHKSUM;
	fs->j_csum3 = 1;
	kprintf("ext4: %s journal to csum-v3 (metadata_csum fs)\n",
		was_v3 ? "fixed s_checksum_type on" : "upgraded plain");
}

/* Write batch entries [lo, hi) to the log as transaction `seq', starting at
 * log block j_first, and make it durable (the commit sync).  The journal
 * superblock is pointed at it first.  Returns ST_OK or the first error. */
static int ext4_jlog_txn_write(ext4_fs_t *fs, unsigned lo, unsigned hi,
			       uint32_t seq, uint8_t *db, uint8_t *cpy)
{
	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	int is64 = (be32(jsb->s_feature_incompat) &
		    JBD2_FEATURE_INCOMPAT_64BIT) != 0;
	int csum3 = fs->j_csum3;
	unsigned tagsz = csum3 ? JBD2_TAG3_SIZE : (is64 ? 12u : 8u);
	unsigned long per = ext4_jdesc_tags(fs, csum3, is64);
	unsigned long first = fs->j_first;
	unsigned long pos = first; /* next log block to fill */
	int err = ST_OK;

	/* Point the journal superblock at the transaction. */
	jsb->s_start = __builtin_bswap32((uint32_t)first);
	jsb->s_sequence = __builtin_bswap32(seq);
	ext4_jsb_csum_set(fs->j_sb_buf, csum3);
	if (jlog_write(fs, 0, fs->j_sb_buf) != ST_OK)
		err = ST_IO;

	for (unsigned d = lo; d < hi && err == ST_OK;) {
		unsigned cnt = (hi - d < per) ? hi - d : (unsigned)per;
		unsigned long dpos = pos; /* this descriptor's log block */
		unsigned off = sizeof(journal_header_t);

		mm_memset(db, 0, fs->block_size);
		journal_header_t *dh = (journal_header_t *)db;
		dh->h_magic = __builtin_bswap32(JBD2_MAGIC_NUMBER);
		dh->h_blocktype = __builtin_bswap32(JBD2_DESCRIPTOR_BLOCK);
		dh->h_sequence = __builtin_bswap32(seq);
		for (unsigned k = 0; k < cnt; k++) {
			unsigned i = d + k;
			unsigned long tgt = fs->st->ckpt.blk[i];
			uint32_t flags = 0;
			if (k > 0)
				flags |= JBD2_FLAG_SAME_UUID;
			if (k == cnt - 1)
				flags |= JBD2_FLAG_LAST_TAG;
			uint32_t fw;
			mm_memcpy(&fw, fs->st->ckpt.data[i], 4);
			int escape = (fw == __builtin_bswap32(JBD2_MAGIC_NUMBER));
			if (escape)
				flags |= JBD2_FLAG_ESCAPE;

			/* The journalled (escaped) copy must exist before the
			 * tag csum, which covers the exact bytes in the log. */
			mm_memcpy(cpy, fs->st->ckpt.data[i], fs->block_size);
			if (escape) {
				uint32_t z = 0;
				mm_memcpy(cpy, &z, 4);
			}

			uint8_t *tag = db + off;
			*(uint32_t *)(tag + 0) = __builtin_bswap32(
				(uint32_t)(tgt & 0xFFFFFFFFUL));
			if (csum3) { /* 16-byte csum-v3 tag  */
				*(uint32_t *)(tag + JBD2_TAG3_FLAGS_OFF) =
					__builtin_bswap32(flags);
				*(uint32_t *)(tag + JBD2_TAG3_BLKHI_OFF) =
					is64 ? __builtin_bswap32(
						       (uint32_t)(tgt >> 32)) :
					       0;
				uint32_t tc = ext4_jblock_csum(fs->j_csum_seed,
							       seq, cpy,
							       fs->block_size);
				*(uint32_t *)(tag + JBD2_TAG3_CSUM_OFF) =
					__builtin_bswap32(tc);
			} else { /* classic 8/12-byte tag */
				*(uint16_t *)(tag + 4) = 0; /* no csum */
				*(uint16_t *)(tag + 6) =
					__builtin_bswap16((uint16_t)flags);
				if (is64)
					*(uint32_t *)(tag + 8) =
						__builtin_bswap32(
							(uint32_t)(tgt >> 32));
			}
			off += tagsz;
			if (!(flags & JBD2_FLAG_SAME_UUID)) { /* uuid follows */
				mm_memcpy(db + off, fs->j_uuid, 16);
				off += 16;
			}
			if (jlog_write(fs, dpos + 1 + k, cpy) != ST_OK) {
				err = ST_IO;
				break;
			}
		}
		if (csum3) { /* descriptor tail csum */
			uint32_t *dt = (uint32_t *)(db + fs->block_size - 4);
			*dt = 0;
			*dt = __builtin_bswap32(
				ext4_crc32c(fs->j_csum_seed, db, fs->block_size));
		}
		if (err == ST_OK && jlog_write(fs, dpos, db) != ST_OK)
			err = ST_IO;
		pos = dpos + 1 + cnt;
		d += cnt;
	}
	if (err == ST_OK) {
		mm_memset(cpy, 0, fs->block_size); /* the commit block */
		journal_header_t *ch = (journal_header_t *)cpy;
		ch->h_magic = __builtin_bswap32(JBD2_MAGIC_NUMBER);
		ch->h_blocktype = __builtin_bswap32(JBD2_COMMIT_BLOCK);
		ch->h_sequence = __builtin_bswap32(seq);
		if (csum3) {
			uint32_t *cc = (uint32_t *)(cpy + JBD2_COMMIT_CSUM_OFF);
			*cc = 0;
			*cc = __builtin_bswap32(ext4_crc32c(fs->j_csum_seed,
							    cpy,
							    fs->block_size));
		}
		if (jlog_write(fs, pos, cpy) != ST_OK)
			err = ST_IO;
		pos++;
	}
	if (ext4_dev_sync(fs, "commit") != ST_OK) /* commit durable */
		err = ST_IO;
	fs->st->jhead = pos;
	return err;
}

/* Durably commit the whole in-memory batch (st->ckpt), then checkpoint it to
 * its final locations and empty the log.  Called lazily -- when the batch
 * fills, on fsync/sync/unmount, by the periodic commit -- NOT per operation.
 *
 * The batch goes to the log as ONE transaction (as many descriptor blocks as
 * its tags need): every operation merged into it is then applied by a
 * replay entirely or not at all.  ext4_txn_flush keeps the batch small
 * enough for the log.  Only a single operation larger than the whole log
 * (possible with a tiny journal) is written as several transactions in a
 * row, each one checkpointed before the next reuses the log -- the best a
 * log of that size allows.
 *
 * Crash safety: the transaction is durable (commit sync) before its blocks
 * are written to their final homes (checkpoint), which is itself durable
 * before the log is emptied (s_start=0).  Between flushes the on-disk log is
 * empty, so a crash outside the brief flush window replays nothing. */
void ext4_journal_flush(ext4_fs_t *fs)
{
	if (!fs || !fs->j_enabled || !fs->j_sb_buf || ext4_journal_aborted(fs))
		return;
	/* data=ordered: write the buffered file data to its final home AND make it
     * durable BEFORE this transaction commits the metadata that references those
     * blocks, so a crash can never expose stale block contents through committed
     * metadata.  One extra sync here is cheap — this runs per batch, not per op. */
	{
		/* Store the data this transaction wrote before the metadata
		 * describing it -- and only that data.  Which inodes those are
		 * was recorded as the writes happened; see ext4_ordered_add.
		 *
		 * Taking the filesystem I/O lock again from here is safe: it is
		 * recursive and only the outermost release commits, so this
		 * cannot re-enter the journal. */
		int data_written = ext4_ordered_flush(fs);

		if (fs->st->wb_len > 0) {
			ext4_wb_flush(fs);
			data_written++;
		}
		if (data_written > 0)
			ext4_dev_sync(fs, "ordered-data");
	}
	/* An error found while the data was stored aborted the journal. */
	if (ext4_journal_aborted(fs))
		return;
	/* The frees of the operations in this batch become reusable once it
	 * is committed (ext4_pf_add); later ones stay pending. */
	unsigned pf_upto = fs->st->pf_merged;
	unsigned n = fs->st->ckpt.n;
	if (n == 0) {
		ext4_pf_release(fs, pf_upto); /* nothing uncommitted */
		return; /* nothing dirty */
	}

	unsigned long span = ext4_jlog_span(fs);
	uint8_t *db = (uint8_t *)ext4_bget(fs);
	uint8_t *cpy = (uint8_t *)ext4_bget(fs);
	if (!db || !cpy) {
		/* No buffer for the log blocks: keep the batch (it stays
		 * readable from memory) and try again at the next flush. */
		if (db)
			ext4_bput(fs, db);
		if (cpy)
			ext4_bput(fs, cpy);
		WARN_RATELIMIT(1, "ext4: no memory to commit the journal batch");
		return;
	}

	/* The first transaction of this mount fixes the journal's format. */
	ext4_journal_upgrade(fs);

	/* 1. mark needs-recovery, durably, once per dirty period (amortised). */
	if (!(fs->sb_copy.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER)) {
		fs->sb_copy.s_feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
		fs->feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
		ext4_write_super(fs);
		ext4_dev_sync(fs, "set-recover");
	}

	/* 2. transaction(s), each committed and then checkpointed. */
	for (unsigned lo = 0; lo < n;) {
		unsigned long most = ext4_jtxn_max(fs);
		unsigned cnt = (n - lo < most) ? n - lo : (unsigned)most;

		if (cnt == 0) /* not even one block fits: cannot be */
			break;
		if (cnt < n && lo == 0)
			kprintf("ext4: %u metadata blocks exceed the journal "
				"(%lu blocks): committed as several "
				"transactions\n",
				n, span);
		uint32_t seq = fs->j_sequence;
		if (ext4_jlog_txn_write(fs, lo, lo + cnt, seq, db, cpy) !=
		    ST_OK) {
			/* The transaction may not be complete in the log, so
			 * the final locations are not touched: the batch
			 * stays, and the next flush commits it again -- unless
			 * an error aborted the journal, which never commits
			 * it. */
			WARN_RATELIMIT(!ext4_journal_aborted(fs),
				       "ext4: journal commit failed - "
				       "keeping the batch");
			break;
		}
		fs->j_sequence = seq + 1;
		/* 3. checkpoint: the blocks to their final homes, log
		 * emptied. */
		if (ext4_checkpoint(fs, lo, lo + cnt) != ST_OK)
			break;
		lo += cnt;
		if (lo == n) {
			fs->st->ckpt.n = 0; /* batch closed */
			fs->st->epoch_open = 0;
			ext4_pf_release(fs, pf_upto);
		}
	}
	ext4_bput(fs, db);
	ext4_bput(fs, cpy);
}

/* Per-operation commit (outermost ext4_io_unlock): merge this op's captured
 * metadata into the in-memory batch — NO disk I/O — and durably flush the batch
 * only when it fills (or a freed-block forces it).  fsync/sync/unmount flush it
 * explicitly.  Atomicity on a mid-op task death is preserved because st->txn is
 * still per-op: the dead-task handler discards st->txn while the already-merged
 * batch (st->ckpt) of completed ops stays intact. */
void ext4_txn_flush(ext4_fs_t *fs)
{
	if (!fs->st->txn.active)
		return;
	fs->st->txn.active = 0;
	unsigned n = fs->st->txn.n;
	fs->st->txn.n = 0;
	if (n == 0)
		return; /* read-only op: nothing to commit */

	if (!fs || !fs->j_enabled ||
	    !fs->j_sb_buf) { /* journaling off: direct */
		for (unsigned i = 0; i < n; i++)
			ext4_write_block_direct(fs, fs->st->txn.blk[i],
						fs->st->txn.data[i]);
		return;
	}

	/* The batch is committed as one transaction, which must fit in the
	 * log: if this operation's blocks could take it past that, the batch
	 * so far is committed first, so no operation is ever split between
	 * two transactions (only one larger than the whole log can be). */
	if (fs->st->ckpt.n &&
	    (unsigned long)fs->st->ckpt.n + n > ext4_jtxn_max(fs))
		ext4_journal_flush(fs);

	for (unsigned i = 0; i < n;
	     i++) /* accumulate into the batch (in memory) */
		ext4_ckpt_merge(fs, fs->st->txn.blk[i], fs->st->txn.data[i]);
	fs->st->epoch_open = 1;
	fs->st->pf_merged = fs->st->pf_n; /* its frees ride this batch */

	/* Flush durably only when the batch is full, or this op freed a block that
     * still has a journal copy (it must be checkpointed before reuse). */
	if (fs->st->ckpt.n >= EXT4_JOURNAL_BATCH)
		ext4_journal_flush(fs);
}

/* Clean the journal: checkpoint any open epoch to its final locations, mark the
 * log empty (s_start=0) and drop needs-recovery.  Called on explicit sync /
 * fsync / shutdown — NOT per commit.  After this the on-disk fs is fully
 * consistent and a reboot finds a clean fs that does not replay.  Returns
 * ST_OK, or ST_IO when the batch could not be committed and checkpointed
 * (or the journal is aborted): then the log is left as it is -- a
 * transaction it holds may be all there is of blocks whose final write
 * failed, and the next flush or mount completes it. */
int ext4_journal_clean(ext4_fs_t *fs)
{
	if (!fs || !fs->j_enabled || !fs->j_sb_buf)
		return ST_OK;
	if (ext4_journal_aborted(fs))
		return ST_IO;
	ext4_journal_flush(
		fs); /* durably commit the batch -> log -> final, s_start=0 */
	/* Aborted during that flush, or the commit or the checkpoint failed
	 * (the batch is still there): what the log holds is not to be thrown
	 * away -- it is not marked empty, and RECOVER stays. */
	if (ext4_journal_aborted(fs) || fs->st->ckpt.n != 0)
		return ST_IO;
	if (!(fs->sb_copy.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER))
		return ST_OK; /* already clean */
	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	if (jsb->s_start != 0) { /* ensure log empty on disk   */
		jsb->s_start = 0;
		jsb->s_sequence = __builtin_bswap32(fs->j_sequence);
		ext4_jsb_csum_set(fs->j_sb_buf, fs->j_csum3);
		jlog_write(fs, 0, fs->j_sb_buf);
		ext4_dev_sync(fs, "clean-jsb");
	}
	fs->sb_copy.s_feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
	fs->feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
	ext4_write_super(fs);
	ext4_dev_sync(fs, "clean-recover");
	return ST_OK;
}

/* ---- Aborting the journal --------------------------------------------------
 *
 * An error that turns the filesystem read-only (errors=remount-ro) may have
 * been met half way through an operation, whose metadata changes so far are
 * in the running transaction and the batch.  Committing them would make the
 * half-done operation durable -- the next mount would replay it.  So from
 * the abort on nothing is committed and nothing more is written to the log:
 *
 *   - the batch (st->ckpt) and the running transaction are never written;
 *     they stay in memory only so that this mount goes on reading what it
 *     already showed, and are dropped at unmount;
 *   - a transaction the log already holds -- committed, its checkpoint not
 *     yet complete -- stays there: the log is not marked empty and RECOVER
 *     stays set, so the next mount replays it;
 *   - the superblock records the error (ext4_fs_error), and later writes
 *     are refused (EROFS), sync and fsync fail (EIO).
 *
 * Set before the read-only latch, by whichever task found the error; read
 * by the commit paths, which all run under the I/O lock. */
void ext4_journal_abort(ext4_fs_t *fs)
{
	if (!fs || !fs->st || !fs->j_enabled)
		return;
	if (__atomic_exchange_n(&fs->st->j_aborted, 1, __ATOMIC_ACQ_REL))
		return; /* already */
	kprintf("ext4: journal aborted: uncommitted changes are discarded\n");
}

/* Periodic journal commit, mirroring a real journal's ~5s commit interval: the
 * in-memory batch is committed durably on this timer too, not only when it fills
 * or on fsync — so a crash loses at most this interval's worth of recent work
 * instead of an unbounded amount.  Runs in its own kernel task because the flush
 * takes the I/O mutex and sleeps on disk I/O (so it can't live in a timer IRQ).
 *
 * One task serves every mounted ext4 filesystem: they are on a list, and a
 * pass marks the filesystem it is working on (commit_busy) so an unmount can
 * take it off the list and wait for the pass to finish before freeing it. */
#define EXT4_COMMIT_INTERVAL_SEC 5
static uint8_t s_flush_stack[16384] __attribute__((aligned(16)));
static int s_flush_thread_started;
static ext4_fs_t *s_commit_list;
static spinlock_t s_commit_lock = SPINLOCK_INIT("ext4_commit");

/* One commit pass over `fs'. */
static void ext4_commit_pass(ext4_fs_t *fs)
{
	/* di_n is read without its lock on purpose: it is only being
	 * asked "is there anything to do", and a stale answer costs one
	 * interval, not correctness.  It is here so that a filesystem
	 * with no journal -- which has no batch to commit and would
	 * otherwise skip this whole block -- still stores the sizes
	 * write() left in memory on a timer. */
	if (fs->read_only || ext4_journal_aborted(fs) ||
	    !(fs->j_enabled || fs->st->di_n))
		return;
	if (fs->j_enabled)
		ext4_ordered_writeback(fs);
	ext4_io_lock(fs);
	/* Sizes and timestamps that write() left in memory get
	 * stored here, on the same schedule and for the same
	 * reason as the pages: this thread holds no filesystem
	 * lock coming in, so it can take the metadata lock
	 * exclusively, and a program is not made to wait for
	 * it.  The inode blocks it writes join this thread's
	 * own transaction and are committed by the release of
	 * the lock below, so they reach the log on the next
	 * pass rather than this one -- which is what a
	 * five-second commit interval means in any case. */
	ext4_di_flush(fs);
	if (fs->j_enabled)
		ext4_journal_flush(
			fs); /* commit + checkpoint the batch (no-op if empty) */
	ext4_io_unlock(fs);
}

static void ext4_flush_thread(void *arg)
{
	(void)arg;
	for (;;) {
		task_t *self = sched_current();
		if (self) { /* sleep ~EXT4_COMMIT_INTERVAL_SEC */
			self->wait_channel = (void *)&s_commit_list;
			self->wakeup_tick = timer_ticks() +
					    (uint64_t)EXT4_COMMIT_INTERVAL_SEC *
						    timer_get_frequency();
			task_set_blocked(self);
			sched_schedule();
			self->wakeup_tick = 0;
			self->wait_channel = NULL;
			if (self->state != TASK_RUNNING)
				self->state = TASK_RUNNING;
		}
		/* Store whatever the cache is holding, if the timer has asked
		 * for it.  This thread is the place for it: it holds no
		 * filesystem lock, so it may take the I/O lock for writing.
		 * Doing the same from a cache lookup would mean asking for that
		 * lock while a reader already holds the metadata lock, which
		 * does not end. */
		pagecache_writeback_if_due();

		/* Each filesystem in turn, marked busy while its pass runs. */
		ext4_fs_t *done = NULL;
		for (;;) {
			uint64_t fl;
			ext4_fs_t *fs;

			spin_lock_irqsave(&s_commit_lock, &fl);
			fs = s_commit_list;
			if (done) {
				/* The one after the last done, if it is still
				 * on the list; else the walk is over. */
				ext4_fs_t *p = s_commit_list;
				while (p && p != done)
					p = p->st->commit_next;
				fs = p ? p->st->commit_next : NULL;
				done->st->commit_busy--;
			}
			if (fs)
				fs->st->commit_busy++;
			spin_unlock_irqrestore(&s_commit_lock, fl);
			if (!fs)
				break;
			ext4_commit_pass(fs);
			done = fs;
		}
	}
}

/* Put a filesystem on the commit thread's list (starting the thread with the
 * first journalled one), or take it off -- waiting for a pass that is
 * working on it to finish. */
void ext4_commit_attach(ext4_fs_t *fs)
{
	uint64_t fl;

	spin_lock_irqsave(&s_commit_lock, &fl);
	fs->st->commit_next = s_commit_list;
	s_commit_list = fs;
	spin_unlock_irqrestore(&s_commit_lock, fl);

	/* Start the periodic commit task once a journalled filesystem is live.
	 * It is a TASK_KERNEL thread, so sched_add_task makes it unkillable by
	 * design (sys_kill and sched_signal_pgrp both reject TASK_KERNEL with
	 * -EPERM).  Name it for ps, which shows kernel threads bracketed, e.g.
	 * "[ext4-commit]". */
	if (!s_flush_thread_started && fs->j_enabled) {
		s_flush_thread_started = 1;
		task_t *t = sched_add_task(ext4_flush_thread, 0, s_flush_stack,
					   sizeof(s_flush_stack));
		if (t) {
			const char *nm = "ext4-commit";
			unsigned i = 0;
			for (; nm[i] && i < sizeof(t->comm) - 1; i++)
				t->comm[i] = nm[i];
			t->comm[i] = '\0';
		}
	}
}

void ext4_commit_detach(ext4_fs_t *fs)
{
	uint64_t fl;

	spin_lock_irqsave(&s_commit_lock, &fl);
	ext4_fs_t **pp = &s_commit_list;
	while (*pp && *pp != fs)
		pp = &(*pp)->st->commit_next;
	if (*pp)
		*pp = fs->st->commit_next;
	fs->st->commit_next = NULL;
	spin_unlock_irqrestore(&s_commit_lock, fl);
	while (__atomic_load_n(&fs->st->commit_busy, __ATOMIC_ACQUIRE) != 0)
		sched_yield_in_kernel();
}
