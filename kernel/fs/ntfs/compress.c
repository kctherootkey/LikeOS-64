/* LikeOS - NTFS filesystem driver
 *
 * LZNT1 compression, XPRESS and LZX (WOF) decompression.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#define NTFS_INTERNAL 1 /* the driver-internal section of ntfs.h */
#include <kernel/fs/ntfs/ntfs.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/fs/ntfs/ntfs_int.h>

/* ===========================================================================
 * S12  LZNT1 decompression
 *
 * A compression unit (16 clusters) is stored as a sequence of chunks, each
 * decompressing to at most 4096 bytes.  A chunk starts with a 16-bit
 * header: bits 0..11 = chunk length - 3 (header included), bit 15 = the
 * chunk is compressed; a zero header ends the unit.  An uncompressed chunk
 * holds 4096 literal bytes.  A compressed chunk is a sequence of groups:
 * one flag byte, then eight tokens; a clear flag bit is a literal byte, a
 * set bit a 16-bit back-reference whose split between displacement and
 * length depends on how far into the 4096-byte chunk the output is (the
 * more output, the more displacement bits).  Lengths are at least 3.
 *
 * Every read is bounded by the input, every write by the 4096-byte chunk
 * and the output buffer; a displacement must point into the current
 * chunk's output; the sub-block end is tested with ">="; all output
 * offsets are kept in full-width types.  A short chunk that is not
 * the last leaves zeros behind it, up to the next 4096-byte boundary.
 * =========================================================================== */

/* Decompress `in_len' bytes into `out' (`out_len' bytes).  Returns the
 * number of bytes produced (the rest of `out' is zeroed), or ST_IO. */
long ntfs_lznt1_decompress(const uint8_t *in, uint64_t in_len,
			   uint8_t *out, uint64_t out_len)
{
	uint64_t ip = 0, op = 0;

	while (op < out_len) {
		uint16_t hdr;
		uint64_t clen, cend, chunk_start, chunk_end;

		if (in_len - ip < 2)
			break; /* input exhausted: the rest is zero */
		hdr = ntfs_le16(in + ip);
		if (hdr == 0)
			break; /* end of the unit */
		clen = (uint64_t)(hdr & 0x0FFF) + 3; /* header included */
		if (clen > in_len - ip)
			return ST_IO;
		cend = ip + clen;
		ip += 2;
		chunk_start = op;
		chunk_end = op + NTFS_LZNT1_CHUNK;
		if (chunk_end > out_len)
			chunk_end = out_len;
		if (!(hdr & 0x8000)) {
			/* Stored chunk: 4096 literal bytes (fewer only at
			 * the end of the output). */
			uint64_t n = cend - ip;

			if (n > chunk_end - op)
				return ST_IO;
			mm_memcpy(out + op, in + ip, n);
			op += n;
			ip = cend;
		} else {
			while (ip < cend && op < chunk_end) {
				uint8_t flags = in[ip++];

				for (int bit = 0; bit < 8; bit++) {
					if (ip >= cend || op >= chunk_end)
						break;
					if (!(flags & (1U << bit))) {
						out[op++] = in[ip++];
						continue;
					}
					if (cend - ip < 2)
						return ST_IO;
					uint16_t tok = ntfs_le16(in + ip);
					uint64_t pos = op - chunk_start;
					uint32_t lbits = 12; /* length bits */
					uint32_t lmask;
					uint64_t disp, len;

					ip += 2;
					if (pos == 0)
						return ST_IO; /* nothing to copy */
					/* The displacement field grows with
					 * the output already produced. */
					for (uint64_t p = pos - 1; p >= 0x10;
					     p >>= 1)
						lbits--;
					lmask = (1U << lbits) - 1;
					disp = (uint64_t)(tok >> lbits) + 1;
					len = (uint64_t)(tok & lmask) + 3;
					if (disp > pos)
						return ST_IO;
					if (len > chunk_end - op)
						return ST_IO;
					/* Overlapping copy, byte by byte. */
					for (uint64_t k = 0; k < len; k++) {
						out[op] = out[op - disp];
						op++;
					}
				}
			}
			if (ip < cend && op >= chunk_end)
				return ST_IO; /* more input than room */
			ip = cend;
		}
		/* A short chunk: zeros up to the 4096-byte boundary. */
		if (op < chunk_end && in_len - ip >= 2 && ntfs_le16(in + ip)) {
			mm_memset(out + op, 0, chunk_end - op);
			op = chunk_end;
		}
	}
	if (op < out_len)
		mm_memset(out + op, 0, out_len - op);
	return (long)op;
}

/* ---- LZNT1 compression ----
 *
 * The inverse of the decoder above, one 4096-byte chunk at a time: a
 * greedy parse with hash chains over the chunk (three-byte hash, chains
 * walked at most NTFS_LZNT1_CHAIN steps).  At output position `pos' the
 * token's displacement field is 4 bits wide plus one bit for each doubling
 * of pos beyond 16 (the length field shrinks accordingly), so the longest
 * displacement and the longest match are recomputed for every position
 * exactly as the decoder derives them.  A chunk whose compressed form is
 * not smaller than the chunk itself is stored (header 0x3FFF + 4096 raw
 * bytes).  Chunk headers carry the signature value 3 in bits 12..14, as
 * other writers set it.  The workspace (hash heads and chains, 16 KB) is
 * allocated once per write and reused for every chunk; errors are
 * plain negative status values. */
#define NTFS_LZNT1_CHAIN 48
#define NTFS_LZNT1_NONE 0xFFFFU

static inline uint32_t ntfs_lznt1_hash(const uint8_t *p)
{
	return (((uint32_t)p[0] << 6) ^ ((uint32_t)p[1] << 3) ^ p[2]) &
	       (NTFS_LZNT1_HASH - 1);
}

/* Compress chunk `in' (`n' <= 4096 bytes) into `out' (room for `cap'
 * bytes).  Returns the bytes written, header included: the compressed
 * form, or the stored form when compression does not pay (a stored chunk
 * is always 4096 bytes: `in' must then hold a whole chunk, the caller pads
 * with zeros).  0 when even that does not fit. */
static uint32_t ntfs_lznt1_chunk(const uint8_t *in, uint32_t n, uint8_t *out,
				 uint32_t cap, struct ntfs_lznt1_ws *w)
{
	uint32_t op = 2, pos = 0;

	for (uint32_t i = 0; i < NTFS_LZNT1_HASH; i++)
		w->head[i] = NTFS_LZNT1_NONE;
	while (pos < n) {
		uint32_t flag_at = op;
		uint8_t flags = 0;

		if (op + 1 > cap || op + 1 > NTFS_LZNT1_CHUNK + 2)
			goto store;
		op++;
		for (int bit = 0; bit < 8 && pos < n; bit++) {
			uint32_t lbits = 12, best_len = 0, best_disp = 0;
			uint32_t max_len, max_disp;

			if (pos > 0) {
				for (uint32_t p = pos - 1; p >= 0x10; p >>= 1)
					lbits--;
			}
			max_len = (1U << lbits) - 1 + 3;
			max_disp = 1U << (16 - lbits);
			if (max_len > n - pos)
				max_len = n - pos;
			if (pos > 0 && pos + 3 <= n) {
				uint32_t cand = w->head[ntfs_lznt1_hash(in + pos)];
				int steps = 0;

				while (cand != NTFS_LZNT1_NONE && cand < pos &&
				       pos - cand <= max_disp &&
				       steps++ < NTFS_LZNT1_CHAIN) {
					uint32_t l = 0;

					while (l < max_len &&
					       in[cand + l] == in[pos + l])
						l++;
					if (l > best_len) {
						best_len = l;
						best_disp = pos - cand;
						if (l == max_len)
							break;
					}
					cand = w->prev[cand];
				}
			}
			if (best_len >= 3) {
				uint32_t tok = ((best_disp - 1) << lbits) |
					       (best_len - 3);

				if (op + 2 > cap || op + 2 > NTFS_LZNT1_CHUNK + 2)
					goto store;
				ntfs_put_le16(out + op, (uint16_t)tok);
				op += 2;
				flags |= (uint8_t)(1U << bit);
				for (uint32_t k = 0; k < best_len; k++, pos++) {
					if (pos + 3 <= n) {
						uint32_t h = ntfs_lznt1_hash(in + pos);

						w->prev[pos] = w->head[h];
						w->head[h] = (uint16_t)pos;
					}
				}
			} else {
				if (op + 1 > cap || op + 1 > NTFS_LZNT1_CHUNK + 2)
					goto store;
				out[op++] = in[pos];
				if (pos + 3 <= n) {
					uint32_t h = ntfs_lznt1_hash(in + pos);

					w->prev[pos] = w->head[h];
					w->head[h] = (uint16_t)pos;
				}
				pos++;
			}
		}
		out[flag_at] = flags;
	}
	if (op < NTFS_LZNT1_CHUNK + 2) {
		/* bits 0..11: length - 3; 12..14: signature 3; 15: compressed */
		ntfs_put_le16(out, (uint16_t)(0xB000U | (op - 3)));
		return op;
	}
store:
	if (cap < NTFS_LZNT1_CHUNK + 2 || n != NTFS_LZNT1_CHUNK)
		return 0;
	ntfs_put_le16(out, (uint16_t)(0x3000U | (NTFS_LZNT1_CHUNK + 2 - 3)));
	mm_memcpy(out + 2, in, NTFS_LZNT1_CHUNK);
	return NTFS_LZNT1_CHUNK + 2;
}

/* Compress the first `valid' bytes of unit `in' (`ulen' bytes, zeros after
 * `valid') into `out' (`cap' bytes): whole 4096-byte chunks up to the one
 * holding the last valid byte.  Returns the compressed length, or 0 when
 * the result does not fit into `cap' (the caller stores the unit as it
 * is).  The rest of `out' up to `cap' is zeroed, which ends the unit for
 * a reader (a zero chunk header). */
uint32_t ntfs_lznt1_compress(const uint8_t *in, uint32_t valid,
			     uint8_t *out, uint32_t cap,
			     struct ntfs_lznt1_ws *w)
{
	uint32_t op = 0;

	for (uint32_t c = 0; c < valid; c += NTFS_LZNT1_CHUNK) {
		uint32_t k = ntfs_lznt1_chunk(in + c, NTFS_LZNT1_CHUNK, out + op,
					      cap - op, w);

		if (!k)
			return 0;
		op += k;
	}
	if (op < cap)
		mm_memset(out + op, 0, cap - op);
	return op;
}

/* ===========================================================================
 * S12b  WOF (system compression) decompressors: XPRESS Huffman and LZX
 * =========================================================================== */

/*
 * Decompressors for system-compressed (WOF) files
 * ===============================================
 *
 * A system-compressed file keeps its real contents in a named data stream
 * that is split into independent chunks.  Every chunk is compressed on its
 * own (no history is shared between chunks), so a chunk can be decoded with
 * nothing but its compressed bytes and its exact uncompressed size.  Two
 * codecs are implemented here:
 *
 *   XPRESS Huffman  chunks of 4, 8 or 16 KiB (the "XPRESS4K/8K/16K" modes)
 *   LZX             chunks of 32 KiB, 32 KiB window
 *
 * Selecting the codec, locating the chunk table and handling chunks that
 * were stored uncompressed (compressed size == uncompressed size) is the
 * caller's business.  The two entry points below only turn one compressed
 * chunk into exactly out_len bytes:
 *
 *   ntfs_xpress_decompress(in, in_len, out, out_len, ws)
 *   ntfs_lzx_decompress(in, in_len, out, out_len, ws)
 *
 * Both return ST_OK when exactly out_len bytes were produced and ST_IO for
 * any malformed input.  ws is a caller-provided workspace of at least
 * NTFS_XPRESS_WS_SIZE / NTFS_LZX_WS_SIZE bytes, aligned to 8 bytes; it holds
 * every table, so the functions themselves use only a few dozen bytes of
 * stack.  The workspace does not need to be initialised.
 *
 * Hostile-input rules followed throughout:
 *   - every input byte is fetched through a bounds check against in_len;
 *     bits "after the end" are supplied as zero only so that a Huffman
 *     look-ahead may peek past the last word -- actually consuming such a
 *     bit is an error (ST_IO);
 *   - every output write is bounded by out_len, every match must fit into
 *     the output (and, for LZX, into its block) and may not reach before
 *     the start of the chunk;
 *   - Huffman code length sets must form a complete prefix code;
 *     over-subscribed and incomplete sets are rejected.  The only
 *     exception is a set in which every length is zero (an "empty code"):
 *     LZX legitimately transmits such codes (e.g. a length tree when no
 *     long match occurs, an aligned-offset tree that is never consulted),
 *     so an empty code is accepted when built and only decoding a symbol
 *     from it is an error;
 *   - things deliberately NOT required, because the exact output size is
 *     known and nothing past it is ever consumed: the XPRESS end-of-stream
 *     symbol, the final XPRESS look-ahead word, LZX trailing padding and
 *     the pad byte of an odd-sized uncompressed block that ends the chunk;
 *     bytes after the end of the chunk's data are ignored;
 *   - every loop advances on input or output, there is no recursion and
 *     all multi-byte values are assembled from single bytes (no unaligned
 *     loads, no shifts by the full type width, no signed overflow).
 */

/* ------------------------------------------------------------------------ */
/* Shared: canonical Huffman decoding                                       */
/* ------------------------------------------------------------------------ */

/*
 * Both formats transmit Huffman codes only as a list of codeword lengths,
 * one per symbol (0 = symbol unused).  The codewords themselves follow the
 * canonical rule:
 *
 *   - shorter codewords come numerically before longer ones;
 *   - among codewords of the same length, the smaller symbol value gets the
 *     smaller codeword;
 *   - the first codeword of length L is (last codeword of length L-1 + 1)
 *     shifted left by one bit.
 *
 * Codewords are stored most significant bit first in the bitstream, so the
 * next max_len bits of the stream, read as a number, start with the
 * codeword.  Decoding uses a direct lookup table indexed by the first
 * fast_bits bits for all codewords of at most fast_bits bits; longer
 * codewords fall back to a canonical walk over the per-length counts, which
 * needs no further tables.
 *
 * A length set is valid when the Kraft sum of 2^-len over all used symbols
 * is exactly 1 (complete code).  A larger sum means some codewords would
 * collide (over-subscribed), a smaller one leaves bit patterns without a
 * symbol (incomplete).  Both are rejected, except the all-zero set (see
 * above), which is recorded as "empty".
 */

#define WOF_HUFF_MAX_SYMS   512u   /* largest alphabet (XPRESS: 512)       */
#define WOF_HUFF_MAX_LEN    16u    /* longest codeword (LZX main tree: 16)  */
#define WOF_HUFF_FAST_MAX   10u    /* widest direct lookup table            */

struct wof_huff {
	/* Direct table: entry = (symbol << 5) | codeword length, or 0 when the
	 * codeword is longer than fast_bits (slow path). */
	uint16_t fast[1u << WOF_HUFF_FAST_MAX];
	/* Number of codewords of every length 0..WOF_HUFF_MAX_LEN. */
	uint16_t count[WOF_HUFF_MAX_LEN + 1u];
	/* Used symbols sorted by (codeword length, symbol value). */
	uint16_t sorted[WOF_HUFF_MAX_SYMS];
	uint8_t  max_len;     /* look-ahead width / longest allowed codeword */
	uint8_t  fast_bits;   /* index width of fast[]                       */
	uint8_t  empty;       /* 1 = all lengths were zero                   */
	uint8_t  pad;
};

/*
 * Build a decoder for nsyms symbols with the code lengths lens[0..nsyms).
 * Every length must be <= max_len.  Returns ST_OK (possibly with
 * h->empty = 1) or ST_IO for an over-subscribed or incomplete set.
 */
static int wof_huff_build(struct wof_huff *h, const uint8_t *lens,
						  uint32_t nsyms, uint32_t max_len, uint32_t fast_bits)
{
	uint16_t offs[WOF_HUFF_MAX_LEN + 2u];
	int64_t left;
	uint32_t i, l, k, j, total, idx, code;

	if (nsyms > WOF_HUFF_MAX_SYMS || max_len > WOF_HUFF_MAX_LEN ||
		fast_bits == 0 || fast_bits > WOF_HUFF_FAST_MAX || fast_bits > max_len)
		return ST_IO;

	h->max_len = (uint8_t)max_len;
	h->fast_bits = (uint8_t)fast_bits;
	h->empty = 0;

	for (l = 0; l <= WOF_HUFF_MAX_LEN; l++)
		h->count[l] = 0;
	for (i = 0; i < nsyms; i++) {
		if (lens[i] > max_len)
			return ST_IO;
		h->count[lens[i]]++;
	}

	/*
	 * Kraft check: "left" is the number of unused codewords of the current
	 * length.  It starts at 1 (the empty prefix), doubles with every extra
	 * bit and loses one entry per codeword of that length.  At most
	 * 2^16, far from any overflow.
	 */
	left = 1;
	total = 0;
	for (l = 1; l <= max_len; l++) {
		left = left * 2 - (int64_t)h->count[l];
		if (left < 0)
			return ST_IO;                  /* over-subscribed */
		total += h->count[l];
	}
	if (total == 0) {
		h->empty = 1;                      /* all-zero set: empty code */
		return ST_OK;
	}
	if (left != 0)
		return ST_IO;                      /* incomplete */

	/* Sort the used symbols by (length, value): counting sort. */
	offs[1] = 0;
	for (l = 1; l < max_len; l++)
		offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
	for (i = 0; i < nsyms; i++)
		if (lens[i] != 0)
			h->sorted[offs[lens[i]]++] = (uint16_t)i;

	/*
	 * Fill the direct table.  A codeword c of length l <= fast_bits owns
	 * the 2^(fast_bits - l) entries whose top l bits equal c.  Codes are
	 * handed out in canonical order: increment within a length, shift
	 * left when moving to the next length.  Entries belonging to longer
	 * codewords stay 0 and select the slow path.
	 */
	for (i = 0; i < (1u << fast_bits); i++)
		h->fast[i] = 0;
	code = 0;
	idx = 0;
	for (l = 1; l <= fast_bits; l++) {
		for (k = 0; k < h->count[l]; k++) {
			uint32_t shift = fast_bits - l;
			uint32_t first = code << shift;
			uint16_t entry = (uint16_t)((uint32_t)h->sorted[idx] << 5 | l);

			for (j = 0; j < (1u << shift); j++)
				h->fast[first + j] = entry;
			idx++;
			code++;
		}
		code <<= 1;
	}
	return ST_OK;
}

/*
 * Decode one symbol.  "window" holds the next h->max_len bits of the
 * stream, first bit in the most significant position.  Returns the symbol
 * and stores the codeword length in *len, or returns -1 (empty code).
 */
static int wof_huff_decode(const struct wof_huff *h, uint32_t window,
						   uint32_t *len)
{
	uint32_t e, l, code, first, index, cnt;

	if (h->empty)
		return -1;

	e = h->fast[window >> (h->max_len - h->fast_bits)];
	if (e != 0) {
		*len = e & 31u;
		return (int)(e >> 5);
	}

	/*
	 * Slow path, canonical walk: "first" is the first codeword of length l
	 * and "index" the position of its symbol in sorted[].  If the top l
	 * bits of the window lie within [first, first + count[l]) they are a
	 * codeword of this length.  The unsigned subtraction wraps for
	 * code < first, which then simply fails the range test.
	 */
	first = 0;
	index = 0;
	for (l = 1; l <= h->max_len; l++) {
		code = window >> (h->max_len - l);
		cnt = h->count[l];
		if (code - first < cnt) {
			*len = l;
			return (int)h->sorted[index + (code - first)];
		}
		index += cnt;
		first = (first + cnt) << 1;
	}
	return -1;   /* not reached for a complete code */
}

/* No chunk comes anywhere near this; keeps all position arithmetic small. */
#define WOF_MAX_INPUT       0x01000000u

/* Little-endian helpers that only ever read single bytes. */
static uint32_t wof_get_le16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t wof_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		   (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------------------ */
/* XPRESS Huffman                                                           */
/* ------------------------------------------------------------------------ */

/*
 * XPRESS Huffman ("LZ77 + Huffman") chunk layout
 * ----------------------------------------------
 *
 *   bytes 0..255   code lengths of the 512-symbol Huffman code, 4 bits each:
 *                  byte k holds symbol 2k in its low nibble and symbol
 *                  2k+1 in its high nibble (lengths 0..15, 0 = unused).
 *   bytes 256..    the bitstream, interleaved with plain bytes.
 *
 * Symbols 0..255 are literal bytes.  Symbols 256..511 start a match:
 *
 *   s = symbol - 256
 *   length header = s & 15, offset bit count n = s >> 4  (0..15)
 *
 *   length: header < 15  -> length = header + 3
 *           header == 15 -> read a plain byte b:
 *                             b < 255  -> length = 15 + b + 3
 *                             b == 255 -> read a 16-bit little-endian
 *                                         value v; v < 15 is corrupt;
 *                                         length = v + 3
 *   offset: (1 << n) | (next n bits of the bitstream), i.e. 1..65535
 *
 * The bitstream consists of 16-bit little-endian words whose bits are used
 * from the most significant bit down.  The decoder starts by loading the
 * first two words (32 bits) and, whenever fewer than 16 unread bits remain
 * after consuming bits, immediately loads the next word.  The plain bytes
 * of long match lengths are taken from the input position just after the
 * last loaded word, at the moment right after the match symbol has been
 * consumed (and before its offset bits are read).  The encoder lays the
 * stream out to match exactly this load schedule, so the decoder must
 * follow it bit for bit.
 *
 * The format allows the code table to change every 64 KiB of output; a
 * chunk never exceeds 64 KiB, so exactly one table is used and longer
 * outputs are rejected.  A stream normally ends with symbol 256 (a
 * length-3 match at offset 1) as end marker; since the exact output size
 * is known, decoding simply stops after out_len bytes and never needs it.
 *
 * Some descriptions of the format add a further escape (v == 0 followed by
 * a 32-bit length) for matches longer than 65538 bytes.  Such a match can
 * never occur inside a chunk of at most 64 KiB, so v == 0 is rejected
 * together with the other values below 15.
 */

#define WOF_XPRESS_NUM_SYMS     512u
#define WOF_XPRESS_MAX_LEN      15u
#define WOF_XPRESS_FAST_BITS    10u
#define WOF_XPRESS_MAX_OUT      65536u

struct wof_xpress_ws {
	struct wof_huff code;
	uint8_t lens[WOF_XPRESS_NUM_SYMS];
};

/* Compile-time check that the workspace size above is large enough. */
typedef char wof_xpress_ws_size_check
	[(sizeof(struct wof_xpress_ws) <= NTFS_XPRESS_WS_SIZE) ? 1 : -1];

/* XPRESS bit reader: avail unread bits sit in the low bits of buf, the next
 * bit is bit (avail - 1).  Words that lie past the end of the input are
 * loaded as zero and counted in "phantom"; consuming them is an error. */
struct wof_xbits {
	const uint8_t *in;
	uint32_t in_len;
	uint32_t pos;        /* input position of the next word / plain byte */
	uint64_t buf;
	uint32_t avail;      /* 16..32 between operations                    */
	uint32_t phantom;    /* zero bits at the bottom of buf past the input */
};

static void wof_xbits_load(struct wof_xbits *b)
{
	uint32_t w = 0;

	if (b->pos <= b->in_len && b->in_len - b->pos >= 2)
		w = wof_get_le16(b->in + b->pos);
	else
		b->phantom += 16;
	b->pos += 2;
	b->buf = (b->buf << 16) | w;
	b->avail += 16;
}

/* Consume n (<= 16) bits; refill as soon as fewer than 16 remain. */
static int wof_xbits_consume(struct wof_xbits *b, uint32_t n)
{
	if (n > b->avail - b->phantom)
		return ST_IO;
	b->avail -= n;
	if (b->avail < 16)
		wof_xbits_load(b);
	return ST_OK;
}

/* Return the next n (1..16) bits without consuming them. */
static uint32_t wof_xbits_peek(const struct wof_xbits *b, uint32_t n)
{
	return (uint32_t)(b->buf >> (b->avail - n)) & ((1u << n) - 1u);
}

int ntfs_xpress_decompress(const uint8_t *in, uint32_t in_len,
							   uint8_t *out, uint32_t out_len, void *ws)
{
	struct wof_xpress_ws *w = (struct wof_xpress_ws *)ws;
	struct wof_xbits b;
	uint32_t i, pos, len, off, n;
	int sym;

	if (out_len == 0)
		return ST_OK;
	if (out_len > WOF_XPRESS_MAX_OUT || in_len < 256 || in_len > WOF_MAX_INPUT)
		return ST_IO;

	mm_memset(w, 0, sizeof(*w));

	/* Unpack the 512 four-bit code lengths and build the decoder. */
	for (i = 0; i < 256; i++) {
		w->lens[2 * i] = (uint8_t)(in[i] & 15u);
		w->lens[2 * i + 1] = (uint8_t)(in[i] >> 4);
	}
	if (wof_huff_build(&w->code, w->lens, WOF_XPRESS_NUM_SYMS,
					   WOF_XPRESS_MAX_LEN, WOF_XPRESS_FAST_BITS) != ST_OK ||
		w->code.empty)
		return ST_IO;

	/* Prime the bit reader with the first two words. */
	b.in = in;
	b.in_len = in_len;
	b.pos = 256;
	b.buf = 0;
	b.avail = 0;
	b.phantom = 0;
	wof_xbits_load(&b);
	wof_xbits_load(&b);

	pos = 0;
	while (pos < out_len) {
		/* avail >= 16 here, enough to look at a full 15-bit codeword. */
		sym = wof_huff_decode(&w->code,
							  wof_xbits_peek(&b, WOF_XPRESS_MAX_LEN), &n);
		if (sym < 0 || wof_xbits_consume(&b, n) != ST_OK)
			return ST_IO;

		if (sym < 256) {
			out[pos++] = (uint8_t)sym;
			continue;
		}

		/* Match: length header in the low 4 bits, offset bit count above. */
		sym -= 256;
		len = (uint32_t)sym & 15u;
		n = (uint32_t)sym >> 4;

		if (len == 15) {
			/* Plain length bytes follow the words loaded so far. */
			if (b.pos >= in_len)
				return ST_IO;
			len = in[b.pos++];
			if (len == 255) {
				if (b.pos > in_len || in_len - b.pos < 2)
					return ST_IO;
				len = wof_get_le16(in + b.pos);
				b.pos += 2;
				if (len < 15)
					return ST_IO;
			} else {
				len += 15;
			}
		}
		len += 3;

		/* Offset: implicit top bit plus n explicit low bits (n <= 15). */
		off = 1u << n;
		if (n != 0) {
			off |= wof_xbits_peek(&b, n);
			if (wof_xbits_consume(&b, n) != ST_OK)
				return ST_IO;
		}

		if (off > pos || len > out_len - pos)
			return ST_IO;

		/* Byte-wise copy: overlapping matches (off < len) repeat data. */
		for (i = 0; i < len; i++) {
			out[pos] = out[pos - off];
			pos++;
		}
	}
	return ST_OK;
}

/* ------------------------------------------------------------------------ */
/* LZX                                                                      */
/* ------------------------------------------------------------------------ */

/*
 * LZX chunk layout (variant used by system-compressed files)
 * ----------------------------------------------------------
 *
 * Window and chunk size are 32 KiB.  Every chunk starts with a fresh
 * decoder: all previous code lengths are zero and the recent-offsets queue
 * is R0 = R1 = R2 = 1.  There is no stream header; the chunk is a sequence
 * of blocks that together produce exactly out_len bytes.
 *
 * Bitstream: 16-bit little-endian words, bits used from the most
 * significant bit down (same convention as XPRESS Huffman).  Uncompressed
 * blocks interrupt the bitstream with byte-aligned raw data.
 *
 * Block header:
 *   3 bits   block type: 1 = verbatim, 2 = aligned offset, 3 = uncompressed
 *   1 bit    1 -> block size is 32768
 *            0 -> 16 more bits follow holding the block size
 *
 * Verbatim block:      main tree, length tree, then the compressed data.
 * Aligned offset block: 8 x 3-bit lengths of the aligned-offset tree, then
 *                      as verbatim.
 * Uncompressed block:  the bitstream is padded with 1..16 zero bits to the
 *                      next 16-bit boundary (a full 16-bit word of padding
 *                      when it is already aligned), then R0, R1, R2 as
 *                      32-bit little-endian values (they replace the
 *                      queue), then block-size raw bytes, then one padding
 *                      byte if the block size is odd.  The next block
 *                      header starts a new bitstream right after that.
 *
 * Tree transmission.  Code lengths are sent as differences to the lengths
 * the same tree had in the previous block of the chunk (initially all
 * zero), coded with a 20-symbol "pretree":
 *   20 x 4 bits   pretree code lengths
 *   then pretree symbols until all lengths of the range are known:
 *     0..16  length = (previous length - symbol) mod 17
 *     17     4 bits n: 4 + n lengths become 0
 *     18     5 bits n: 20 + n lengths become 0
 *     19     1 bit n, then one more pretree symbol d (0..16):
 *            4 + n lengths all become (previous length of the first of
 *            them - d) mod 17
 * The main tree (256 literals + 8 symbols per offset slot = 496 symbols
 * with 30 slots) is sent in two ranges, 0..255 and 256..495, each with its
 * own pretree; the length tree (249 symbols) is sent with one pretree.
 * Main and length codewords are at most 16 bits, pretree codewords at
 * most 15 bits, aligned-offset codewords at most 7 bits.
 *
 * Compressed data: main tree symbols.
 *   0..255   literal byte
 *   256..495 match; s = symbol - 256:
 *              length header = s & 7, offset slot = s >> 3
 *              length = header + 2, and if header == 7 a length-tree
 *              symbol is added (lengths 2..257)
 *            The length-tree symbol (if any) precedes the offset bits.
 *
 * Offsets.  Slots 0, 1, 2 reuse the recent offsets R0, R1, R2; using R1 or
 * R2 swaps it with R0.  Slot s >= 3 carries a "formatted offset"
 *   base(s) + footer,  base(s) = (2 + (s & 1)) << ((s - 2) / 2),
 *   footer = (s - 2) / 2 extra bits,
 * and the real offset is formatted offset - 2; it is pushed to the front
 * of the queue (R2 = R1, R1 = R0, R0 = offset).  In aligned offset blocks,
 * a footer of 3 or more bits is sent as its upper (bits - 3) bits verbatim
 * followed by an aligned-tree symbol giving the low 3 bits; shorter
 * footers are always verbatim.
 *
 * Matches must stay within their block and may not reach before the
 * start of the chunk (chunks are independent).
 *
 * E8 call translation.  Before compression, every E8 byte (x86 CALL) at
 * chunk position i < chunk size - 10 had the 32-bit little-endian
 * relative target that follows it converted to an absolute one, with a
 * fixed translation size T = 12000000; the 4 operand bytes are then
 * skipped.  The decoder undoes this over the decoded chunk:
 *
 *   a = signed 32-bit value after an E8 at position i
 *   if (-i <= a < T):  value = (a >= 0) ? a - i : a + T
 *   continue at i + 5 (any E8: whether translated or not)
 */

#define WOF_LZX_NUM_CHARS       256u
#define WOF_LZX_NUM_SLOTS       30u
#define WOF_LZX_MAIN_SYMS       (WOF_LZX_NUM_CHARS + 8u * WOF_LZX_NUM_SLOTS)
#define WOF_LZX_LEN_SYMS        249u
#define WOF_LZX_ALIGNED_SYMS    8u
#define WOF_LZX_PRE_SYMS        20u
#define WOF_LZX_MAX_LEN         16u
#define WOF_LZX_PRE_MAX_LEN     15u
#define WOF_LZX_ALIGNED_MAX_LEN 7u
#define WOF_LZX_CHUNK           32768u
#define WOF_LZX_E8_SIZE         12000000

#define WOF_LZX_VERBATIM        1u
#define WOF_LZX_ALIGNED         2u
#define WOF_LZX_UNCOMPRESSED    3u

struct wof_lzx_ws {
	struct wof_huff main_code;
	struct wof_huff len_code;
	struct wof_huff aligned_code;
	struct wof_huff pre_code;
	uint8_t main_lens[WOF_LZX_MAIN_SYMS];  /* persist across blocks */
	uint8_t len_lens[WOF_LZX_LEN_SYMS];    /* persist across blocks */
	uint8_t aligned_lens[WOF_LZX_ALIGNED_SYMS];
	uint8_t pre_lens[WOF_LZX_PRE_SYMS];
};

typedef char wof_lzx_ws_size_check
	[(sizeof(struct wof_lzx_ws) <= NTFS_LZX_WS_SIZE) ? 1 : -1];

/*
 * LZX bit reader.  Same buffer convention as the XPRESS reader, but it is
 * refilled lazily to more than 48 bits before every read, so any read of
 * up to 32 bits or any 16-bit Huffman look-ahead fits.  Errors are sticky
 * in "err"; reads after an error return 0.
 */
struct wof_bits {
	const uint8_t *in;
	uint32_t in_len;
	uint32_t pos;        /* input position of the next word to load */
	uint64_t buf;
	uint32_t avail;
	uint32_t phantom;    /* zero bits at the bottom of buf past the input */
	int err;
};

static void wof_bits_init(struct wof_bits *b, const uint8_t *in,
						  uint32_t in_len, uint32_t pos)
{
	b->in = in;
	b->in_len = in_len;
	b->pos = pos;
	b->buf = 0;
	b->avail = 0;
	b->phantom = 0;
	b->err = 0;
}

static void wof_bits_fill(struct wof_bits *b)
{
	while (b->avail <= 48) {
		uint32_t w = 0;

		if (b->pos <= b->in_len && b->in_len - b->pos >= 2)
			w = wof_get_le16(b->in + b->pos);
		else
			b->phantom += 16;
		b->pos += 2;
		b->buf = (b->buf << 16) | w;
		b->avail += 16;
	}
}

/* Read n (0..32) bits as an unsigned number, first bit most significant. */
static uint32_t wof_bits_get(struct wof_bits *b, uint32_t n)
{
	if (n == 0 || b->err)
		return 0;
	wof_bits_fill(b);
	if (n > b->avail - b->phantom) {
		b->err = 1;
		return 0;
	}
	b->avail -= n;
	return (uint32_t)(b->buf >> b->avail) &
		   (uint32_t)(((uint64_t)1 << n) - 1u);
}

/* Decode one Huffman symbol; returns -1 (and sets err) on failure. */
static int wof_bits_sym(struct wof_bits *b, const struct wof_huff *h)
{
	uint32_t window, len;
	int sym;

	if (b->err)
		return -1;
	wof_bits_fill(b);
	window = (uint32_t)(b->buf >> (b->avail - h->max_len)) &
			 ((1u << h->max_len) - 1u);
	sym = wof_huff_decode(h, window, &len);
	if (sym < 0 || len > b->avail - b->phantom) {
		b->err = 1;
		return -1;
	}
	b->avail -= len;
	return sym;
}

/*
 * Skip the 1..16 padding bits that precede an uncompressed block and
 * return the input position of the first raw byte.  avail is always the
 * number of unread bits of whole words that were loaded, so avail % 16 is
 * what is left of the current word (0 = aligned: skip a whole word).
 * The padding must be present in the input.
 */
static int wof_bits_align(struct wof_bits *b, uint32_t *raw_pos)
{
	uint32_t pad = b->avail & 15u;

	if (pad == 0)
		pad = 16;
	(void)wof_bits_get(b, pad);
	if (b->err)
		return ST_IO;
	/* The remaining avail bits are whole, unread words: give them back. */
	*raw_pos = b->pos - b->avail / 8u;
	return ST_OK;
}

/*
 * Read code lengths lens[0..n) (delta-coded against their current values)
 * using a freshly transmitted pretree.
 */
static int wof_lzx_read_lens(struct wof_bits *b, struct wof_lzx_ws *w,
							 uint8_t *lens, uint32_t n)
{
	uint32_t i, run, val;
	int sym;

	for (i = 0; i < WOF_LZX_PRE_SYMS; i++)
		w->pre_lens[i] = (uint8_t)wof_bits_get(b, 4);
	if (b->err ||
		wof_huff_build(&w->pre_code, w->pre_lens, WOF_LZX_PRE_SYMS,
					   WOF_LZX_PRE_MAX_LEN, 8) != ST_OK)
		return ST_IO;

	i = 0;
	while (i < n) {
		sym = wof_bits_sym(b, &w->pre_code);
		if (sym < 0)
			return ST_IO;
		if (sym <= 16) {
			lens[i] = (uint8_t)((lens[i] + 17u - (uint32_t)sym) % 17u);
			i++;
			continue;
		}
		if (sym == 17) {
			run = 4 + wof_bits_get(b, 4);
			val = 0;
		} else if (sym == 18) {
			run = 20 + wof_bits_get(b, 5);
			val = 0;
		} else {
			run = 4 + wof_bits_get(b, 1);
			sym = wof_bits_sym(b, &w->pre_code);
			if (sym < 0 || sym > 16)
				return ST_IO;
			val = (lens[i] + 17u - (uint32_t)sym) % 17u;
		}
		if (b->err || run > n - i)
			return ST_IO;
		while (run != 0) {
			lens[i++] = (uint8_t)val;
			run--;
		}
	}
	return b->err ? ST_IO : ST_OK;
}

/* Undo the E8 call translation over one decoded chunk. */
static void wof_lzx_undo_e8(uint8_t *out, uint32_t out_len)
{
	uint32_t i = 0;

	if (out_len <= 10)
		return;
	while (i < out_len - 10) {
		int64_t a, v;
		uint32_t u;

		if (out[i] != 0xE8) {
			i++;
			continue;
		}
		u = wof_get_le32(out + i + 1);
		a = (int64_t)u;
		if (u & 0x80000000u)
			a -= (int64_t)0x100000000LL;      /* sign-extend */
		if (a >= -(int64_t)i && a < WOF_LZX_E8_SIZE) {
			v = (a >= 0) ? a - (int64_t)i : a + WOF_LZX_E8_SIZE;
			u = (uint32_t)v;                  /* fits: -i <= v < T */
			out[i + 1] = (uint8_t)u;
			out[i + 2] = (uint8_t)(u >> 8);
			out[i + 3] = (uint8_t)(u >> 16);
			out[i + 4] = (uint8_t)(u >> 24);
		}
		i += 5;
	}
}

/* Decode the symbols of one verbatim or aligned offset block. */
static int wof_lzx_block(struct wof_bits *b, struct wof_lzx_ws *w,
						 uint32_t type, uint32_t r[3],
						 uint8_t *out, uint32_t *ppos, uint32_t end)
{
	uint32_t pos = *ppos;
	uint32_t len, slot, nbits, off, i;
	int sym;

	while (pos < end) {
		sym = wof_bits_sym(b, &w->main_code);
		if (sym < 0)
			return ST_IO;
		if (sym < (int)WOF_LZX_NUM_CHARS) {
			out[pos++] = (uint8_t)sym;
			continue;
		}

		sym -= (int)WOF_LZX_NUM_CHARS;
		len = ((uint32_t)sym & 7u) + 2u;
		slot = (uint32_t)sym >> 3;              /* < 30 by alphabet size */
		if (len == 9) {
			sym = wof_bits_sym(b, &w->len_code);
			if (sym < 0)
				return ST_IO;
			len += (uint32_t)sym;
		}

		if (slot == 0) {
			off = r[0];
		} else if (slot == 1) {
			off = r[1];
			r[1] = r[0];
			r[0] = off;
		} else if (slot == 2) {
			off = r[2];
			r[2] = r[0];
			r[0] = off;
		} else {
			nbits = (slot - 2) >> 1;
			off = (2u + (slot & 1u)) << nbits;  /* base(slot) */
			if (type == WOF_LZX_ALIGNED && nbits >= 3) {
				off += wof_bits_get(b, nbits - 3) << 3;
				sym = wof_bits_sym(b, &w->aligned_code);
				if (sym < 0)
					return ST_IO;
				off += (uint32_t)sym;
			} else {
				off += wof_bits_get(b, nbits);
			}
			off -= 2;                           /* base >= 3: off >= 1 */
			r[2] = r[1];
			r[1] = r[0];
			r[0] = off;
		}

		if (b->err || off == 0 || off > pos || len > end - pos)
			return ST_IO;
		for (i = 0; i < len; i++) {
			out[pos] = out[pos - off];
			pos++;
		}
	}
	*ppos = pos;
	return ST_OK;
}

int ntfs_lzx_decompress(const uint8_t *in, uint32_t in_len,
						    uint8_t *out, uint32_t out_len, void *ws)
{
	struct wof_lzx_ws *w = (struct wof_lzx_ws *)ws;
	struct wof_bits b;
	uint32_t r[3];
	uint32_t pos, type, size, raw;

	if (out_len == 0)
		return ST_OK;
	if (out_len > WOF_LZX_CHUNK || in_len > WOF_MAX_INPUT)
		return ST_IO;

	/* Fresh state per chunk: zero previous lengths, queue = 1, 1, 1. */
	mm_memset(w, 0, sizeof(*w));
	r[0] = r[1] = r[2] = 1;
	wof_bits_init(&b, in, in_len, 0);

	pos = 0;
	while (pos < out_len) {
		type = wof_bits_get(&b, 3);
		if (wof_bits_get(&b, 1))
			size = WOF_LZX_CHUNK;
		else
			size = wof_bits_get(&b, 16);
		if (b.err || size == 0 || size > out_len - pos)
			return ST_IO;

		if (type == WOF_LZX_UNCOMPRESSED) {
			if (wof_bits_align(&b, &raw) != ST_OK ||
				raw > in_len || in_len - raw < 12 ||
				in_len - raw - 12 < size)
				return ST_IO;
			r[0] = wof_get_le32(in + raw);
			r[1] = wof_get_le32(in + raw + 4);
			r[2] = wof_get_le32(in + raw + 8);
			mm_memcpy(out + pos, in + raw + 12, size);
			pos += size;
			/* Resume the bitstream after the data and its odd-size pad
			 * byte; a missing pad byte simply ends the input. */
			raw += 12 + size + (size & 1u);
			wof_bits_init(&b, in, in_len, raw);
			continue;
		}

		if (type != WOF_LZX_VERBATIM && type != WOF_LZX_ALIGNED)
			return ST_IO;

		if (type == WOF_LZX_ALIGNED) {
			uint32_t i;

			for (i = 0; i < WOF_LZX_ALIGNED_SYMS; i++)
				w->aligned_lens[i] = (uint8_t)wof_bits_get(&b, 3);
			if (b.err ||
				wof_huff_build(&w->aligned_code, w->aligned_lens,
							   WOF_LZX_ALIGNED_SYMS, WOF_LZX_ALIGNED_MAX_LEN,
							   WOF_LZX_ALIGNED_MAX_LEN) != ST_OK)
				return ST_IO;
		}

		if (wof_lzx_read_lens(&b, w, w->main_lens, WOF_LZX_NUM_CHARS) != ST_OK ||
			wof_lzx_read_lens(&b, w, w->main_lens + WOF_LZX_NUM_CHARS,
							  WOF_LZX_MAIN_SYMS - WOF_LZX_NUM_CHARS) != ST_OK ||
			wof_huff_build(&w->main_code, w->main_lens, WOF_LZX_MAIN_SYMS,
						   WOF_LZX_MAX_LEN, WOF_HUFF_FAST_MAX) != ST_OK ||
			wof_lzx_read_lens(&b, w, w->len_lens, WOF_LZX_LEN_SYMS) != ST_OK ||
			wof_huff_build(&w->len_code, w->len_lens, WOF_LZX_LEN_SYMS,
						   WOF_LZX_MAX_LEN, WOF_HUFF_FAST_MAX) != ST_OK)
			return ST_IO;

		if (wof_lzx_block(&b, w, type, r, out, &pos, pos + size) != ST_OK)
			return ST_IO;
	}

	wof_lzx_undo_e8(out, out_len);
	return ST_OK;
}
