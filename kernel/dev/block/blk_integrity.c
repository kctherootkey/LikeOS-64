// LikeOS - block integrity: protection information (T10 DIF / DIX and the
// NVMe 64-bit guard format) for disks with metadata
//
// A disk with an integrity profile (struct blk_integrity in its limits:
// metadata_size bytes of metadata per protection interval of
// 2^interval_exp data bytes) carries, next to the data of every request,
// a metadata buffer.  Inside each interval's metadata, at pi_offset, sits
// a protection information tuple:
//
//   16-bit guard (BLK_INTEGRITY_CSUM_CRC, T10 DIF):    8 bytes
//       be16 guard   CRC16 T10-DIF (poly 0x8BB7) over the interval's data
//                    (and the metadata bytes before the tuple)
//       be16 app tag opaque
//       be32 ref tag type 1/2: the lower 32 bits of the interval number
//   64-bit guard (BLK_INTEGRITY_CSUM_CRC64, NVMe):     16 bytes
//       be64 guard   CRC64 NVMe (Rocksoft poly 0xAD93D23594C93659,
//                    reflected, ~0 in and out)
//       be16 app tag
//       be48 ref tag the lower 48 bits of the interval number
//
// Type 3 has no reference tag (BLK_INTEGRITY_REF_TAG clear).  An app tag
// of 0xffff (type 1/2), or app 0xffff with an all-ones ref tag (type 3),
// escapes a tuple from checking.
//
// The block layer's part, as the established implementation divides it:
//   * blk_integrity_prep (submission): a disk request without metadata of
//     its own gets a buffer; for a write its PI is generated (unless the
//     profile says NOGENERATE and the device can insert PI itself);
//   * blk_integrity_prepare (blk_mq_start_request, writes): its ref
//     tags, generated from a "virtual" seed, are remapped to the physical
//     interval numbers the device checks;
//   * blk_integrity_complete (reads): back from physical to virtual;
//   * blk_integrity_verify (reads the block layer protects): every tuple
//     is checked; a mismatch fails the request with BLK_STS_PROTECTION.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define T10_PI_APP_ESCAPE 0xffffu
#define T10_PI_REF_ESCAPE 0xffffffffu

/* ==========================================================================
 * Checksums
 * ========================================================================== */

/* CRC16 T10-DIF: polynomial 0x8BB7, MSB first, no reflection, no
 * inversion (the check value of "123456789" is 0xD0DB). */
static uint16_t g_crc_t10dif_table[256];
/* CRC64 NVMe: polynomial 0xAD93D23594C93659, reflected
 * (0x9A6C9329AC4BC9B5), initial and final ~0 (check value of
 * "123456789": 0xAE8B14860A799888). */
static uint64_t g_crc64_nvme_table[256];
static volatile int g_crc_tables_ready;

static void blk_crc_tables_init(void)
{
	if (__atomic_load_n(&g_crc_tables_ready, __ATOMIC_ACQUIRE))
		return;
	for (unsigned int i = 0; i < 256; i++) {
		uint16_t c = (uint16_t)(i << 8);
		uint64_t r = i;

		for (int b = 0; b < 8; b++)
			c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8BB7 : c << 1);
		g_crc_t10dif_table[i] = c;
		for (int b = 0; b < 8; b++)
			r = (r & 1) ? (r >> 1) ^ 0x9A6C9329AC4BC9B5ULL : r >> 1;
		g_crc64_nvme_table[i] = r;
	}
	/* Idempotent: two CPUs racing here write the same values. */
	__atomic_store_n(&g_crc_tables_ready, 1, __ATOMIC_RELEASE);
}

uint16_t crc_t10dif_update(uint16_t crc, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;

	blk_crc_tables_init();
	while (len--)
		crc = (uint16_t)((crc << 8) ^
				 g_crc_t10dif_table[((crc >> 8) ^ *p++) & 0xff]);
	return crc;
}

uint64_t crc64_nvme(uint64_t crc, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;

	blk_crc_tables_init();
	crc = ~crc;
	while (len--)
		crc = (crc >> 8) ^ g_crc64_nvme_table[(crc ^ *p++) & 0xff];
	return ~crc;
}

/* ==========================================================================
 * Profiles and payloads
 * ========================================================================== */

static inline const struct blk_integrity *blk_rq_bi(const struct blk_request *rq)
{
	return rq->queue ? &rq->queue->limits.integrity : NULL;
}

static inline uint32_t blk_bi_tuple(const struct blk_integrity *bi)
{
	return bi->metadata_size ? bi->metadata_size : bi->tuple_size;
}

static inline uint32_t blk_bi_interval(const struct blk_integrity *bi)
{
	return bi->interval_exp ? 1u << bi->interval_exp : 512u;
}

int blk_integrity_rq(const struct blk_request *rq)
{
	return rq && (rq->flags & BLK_REQ_INTEGRITY) && rq->integrity &&
	       rq->integrity->buf;
}

const struct blk_integrity *blk_get_integrity(const struct gendisk *disk)
{
	const struct blk_integrity *bi;

	if (!disk || !disk->queue)
		return NULL;
	bi = &disk->queue->limits.integrity;
	return blk_bi_tuple(bi) ? bi : NULL;
}

int blk_rq_integrity_map_kern(struct blk_request *rq, void *buf,
			      unsigned int len, uint32_t bip_flags)
{
	struct blk_integrity_payload *bip;

	if (!rq || !buf || !len)
		return -EINVAL;
	if (rq->integrity && rq->integrity->buf)
		return -EBUSY;
	bip = &rq->integrity_inline;
	mm_memset(bip, 0, sizeof(*bip));
	bip->buf = buf;
	bip->phys = 0;
	bip->len = len;
	bip->flags = bip_flags;
	bip->seed = blk_rq_pos(rq);
	rq->integrity = bip;
	rq->nr_integrity_segs = 1;
	rq->flags |= BLK_REQ_INTEGRITY | BLK_REQ_NOMERGE;
	return 0;
}

/* The device can insert and strip the PI itself (no metadata beyond the
 * tuple): with NOGENERATE / NOVERIFY nothing is attached at all. */
static int blk_bi_offload_capable(const struct blk_integrity *bi)
{
	return blk_bi_tuple(bi) == bi->pi_tuple_size &&
	       ((bi->csum_type == BLK_INTEGRITY_CSUM_CRC &&
		 bi->pi_tuple_size == 8) ||
		(bi->csum_type == BLK_INTEGRITY_CSUM_CRC64 &&
		 bi->pi_tuple_size == 16));
}

/* ==========================================================================
 * Walking the data interval by interval
 * ========================================================================== */

/* A cursor over a request's sg list: intervals may cross element
 * boundaries (the checksums are continued across them). */
struct blk_data_cursor {
	const struct blk_request *rq;
	uint32_t idx, off;
};

/* Feed `len' data bytes from the cursor to the checksum. */
static void blk_cursor_csum(struct blk_data_cursor *c, uint32_t len,
			    uint8_t csum_type, uint16_t *crc16, uint64_t *crc64)
{
	while (len && c->idx < c->rq->nsg) {
		const struct blk_sg_entry *e = &c->rq->sg[c->idx];
		uint32_t take = e->len - c->off;
		const uint8_t *p = (const uint8_t *)e->virt + c->off;

		if (take > len)
			take = len;
		if (csum_type == BLK_INTEGRITY_CSUM_CRC64)
			*crc64 = crc64_nvme(*crc64, p, take);
		else
			*crc16 = crc_t10dif_update(*crc16, p, take);
		len -= take;
		c->off += take;
		if (c->off == e->len) {
			c->idx++;
			c->off = 0;
		}
	}
}

static inline uint64_t blk_be48_get(const uint8_t *p)
{
	return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
	       ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
	       ((uint64_t)p[4] << 8) | (uint64_t)p[5];
}

static inline void blk_be48_put(uint8_t *p, uint64_t v)
{
	p[0] = (uint8_t)(v >> 40);
	p[1] = (uint8_t)(v >> 32);
	p[2] = (uint8_t)(v >> 24);
	p[3] = (uint8_t)(v >> 16);
	p[4] = (uint8_t)(v >> 8);
	p[5] = (uint8_t)v;
}

static inline uint16_t blk_be16_get(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void blk_be16_put(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline uint32_t blk_be32_get(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void blk_be32_put(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint64_t blk_be64_get(const uint8_t *p)
{
	return ((uint64_t)blk_be32_get(p) << 32) | blk_be32_get(p + 4);
}

static inline void blk_be64_put(uint8_t *p, uint64_t v)
{
	blk_be32_put(p, (uint32_t)(v >> 32));
	blk_be32_put(p + 4, (uint32_t)v);
}

/* The IP checksum (ones' complement sum of 16-bit words, folded and
 * inverted) the established ip_compute_csum() gives, over `len' bytes
 * that may cross sg elements.  Returned as the raw 16-bit value stored
 * into the guard (its in-memory bytes are network order). */
static uint16_t blk_ip_fold(uint64_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static uint16_t blk_cursor_ip(struct blk_data_cursor *c, uint32_t len)
{
	uint64_t sum = 0;
	uint32_t pos = 0;

	while (len && c->idx < c->rq->nsg) {
		const struct blk_sg_entry *e = &c->rq->sg[c->idx];
		uint32_t take = e->len - c->off;
		const uint8_t *p = (const uint8_t *)e->virt + c->off;

		if (take > len)
			take = len;
		for (uint32_t i = 0; i < take; i++, pos++)
			sum += (pos & 1) ? ((uint64_t)p[i] << 8) : p[i];
		len -= take;
		c->off += take;
		if (c->off == e->len) {
			c->idx++;
			c->off = 0;
		}
	}
	return blk_ip_fold(sum);
}

static uint16_t blk_ip_buf(const uint8_t *p, uint32_t len)
{
	uint64_t sum = 0;

	for (uint32_t i = 0; i < len; i++)
		sum += (i & 1) ? ((uint64_t)p[i] << 8) : p[i];
	return blk_ip_fold(sum);
}

/* The guard of one interval: the data, then the metadata bytes in front
 * of the tuple. */
static void blk_pi_guard(struct blk_data_cursor *c,
			 const struct blk_integrity *bi, const uint8_t *meta,
			 uint16_t *g16, uint64_t *g64)
{
	*g16 = 0;
	*g64 = 0;
	if (bi->csum_type == BLK_INTEGRITY_CSUM_IP) {
		/* As the established implementation does it, the metadata
		 * in front of the tuple replaces (does not continue) the
		 * data's IP checksum. */
		*g16 = blk_cursor_ip(c, blk_bi_interval(bi));
		if (bi->pi_offset)
			*g16 = blk_ip_buf(meta, bi->pi_offset);
		return;
	}
	blk_cursor_csum(c, blk_bi_interval(bi), bi->csum_type, g16, g64);
	if (bi->pi_offset) {
		if (bi->csum_type == BLK_INTEGRITY_CSUM_CRC64)
			*g64 = crc64_nvme(*g64, meta, bi->pi_offset);
		else
			*g16 = crc_t10dif_update(*g16, meta, bi->pi_offset);
	}
}

/* Intervals the data of `rq' covers (and the metadata buffer can hold). */
static uint32_t blk_pi_intervals(const struct blk_request *rq,
				 const struct blk_integrity *bi)
{
	uint64_t data = 0;
	uint32_t tuple = blk_bi_tuple(bi);
	uint32_t n;

	for (uint32_t i = 0; i < rq->nsg; i++)
		data += rq->sg[i].len;
	n = (uint32_t)(data >> (bi->interval_exp ? bi->interval_exp : 9));
	if (tuple && rq->integrity && n > rq->integrity->len / tuple)
		n = rq->integrity->len / tuple;
	return n;
}

blk_status_t blk_integrity_generate(struct blk_request *rq)
{
	const struct blk_integrity *bi = blk_rq_bi(rq);
	struct blk_data_cursor c = { rq, 0, 0 };
	uint8_t *meta;
	uint64_t seed;
	uint32_t tuple, n;

	if (!blk_integrity_rq(rq) || !bi || !blk_bi_tuple(bi))
		return BLK_STS_OK;
	if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE)
		return BLK_STS_OK;
	tuple = blk_bi_tuple(bi);
	if (bi->pi_offset + bi->pi_tuple_size > tuple)
		return BLK_STS_PROTECTION;
	meta = (uint8_t *)rq->integrity->buf;
	seed = rq->integrity->seed;
	n = blk_pi_intervals(rq, bi);
	for (uint32_t i = 0; i < n; i++, meta += tuple, seed++) {
		uint8_t *pi = meta + bi->pi_offset;
		uint16_t g16;
		uint64_t g64;

		blk_pi_guard(&c, bi, meta, &g16, &g64);
		if (bi->csum_type == BLK_INTEGRITY_CSUM_CRC64) {
			blk_be64_put(pi, g64);
			blk_be16_put(pi + 8, 0);
			blk_be48_put(pi + 10, (bi->flags & BLK_INTEGRITY_REF_TAG) ?
						      seed & ((1ULL << 48) - 1) :
						      0);
		} else {
			if (bi->csum_type == BLK_INTEGRITY_CSUM_IP) {
				pi[0] = (uint8_t)g16;
				pi[1] = (uint8_t)(g16 >> 8);
			} else {
				blk_be16_put(pi, g16);
			}
			blk_be16_put(pi + 2, 0);
			blk_be32_put(pi + 4, (bi->flags & BLK_INTEGRITY_REF_TAG) ?
						     (uint32_t)seed :
						     0);
		}
	}
	return BLK_STS_OK;
}

blk_status_t blk_integrity_verify(struct blk_request *rq)
{
	const struct blk_integrity *bi = blk_rq_bi(rq);
	struct blk_data_cursor c = { rq, 0, 0 };
	const char *name = rq->disk ? rq->disk->disk_name : "?";
	uint8_t *meta;
	uint64_t seed;
	uint32_t tuple, n;

	if (!blk_integrity_rq(rq) || !bi || !blk_bi_tuple(bi))
		return BLK_STS_OK;
	if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE)
		return BLK_STS_OK;
	tuple = blk_bi_tuple(bi);
	if (bi->pi_offset + bi->pi_tuple_size > tuple)
		return BLK_STS_PROTECTION;
	meta = (uint8_t *)rq->integrity->buf;
	seed = rq->integrity->seed;
	n = blk_pi_intervals(rq, bi);
	for (uint32_t i = 0; i < n; i++, meta += tuple, seed++) {
		const uint8_t *pi = meta + bi->pi_offset;
		uint16_t g16;
		uint64_t g64;

		if (bi->csum_type == BLK_INTEGRITY_CSUM_CRC64) {
			uint16_t app = blk_be16_get(pi + 8);
			uint64_t ref = blk_be48_get(pi + 10);

			if (bi->flags & BLK_INTEGRITY_REF_TAG) {
				if (app == T10_PI_APP_ESCAPE)
					goto skip64;
				if (ref != (seed & ((1ULL << 48) - 1))) {
					WARN_RATELIMIT(1, "%s: ref tag error at location %llu (rcvd %llu)",
						       name,
						       (unsigned long long)seed,
						       (unsigned long long)ref);
					return BLK_STS_PROTECTION;
				}
			} else if (app == T10_PI_APP_ESCAPE &&
				   ref == ((1ULL << 48) - 1)) {
				goto skip64;
			}
			blk_pi_guard(&c, bi, meta, &g16, &g64);
			if (blk_be64_get(pi) != g64) {
				WARN_RATELIMIT(1, "%s: guard tag error at sector %llu (rcvd %016llx, want %016llx)",
					       name, (unsigned long long)seed,
					       (unsigned long long)blk_be64_get(pi),
					       (unsigned long long)g64);
				return BLK_STS_PROTECTION;
			}
			continue;
skip64:
			blk_cursor_csum(&c, blk_bi_interval(bi), bi->csum_type,
					&g16, &g64);
		} else {
			uint16_t app = blk_be16_get(pi + 2);
			uint32_t ref = blk_be32_get(pi + 4);

			if (bi->flags & BLK_INTEGRITY_REF_TAG) {
				if (app == T10_PI_APP_ESCAPE)
					goto skip16;
				if (ref != (uint32_t)seed) {
					WARN_RATELIMIT(1, "%s: ref tag error at location %llu (rcvd %u)",
						       name,
						       (unsigned long long)seed,
						       ref);
					return BLK_STS_PROTECTION;
				}
			} else if (app == T10_PI_APP_ESCAPE &&
				   ref == T10_PI_REF_ESCAPE) {
				goto skip16;
			}
			blk_pi_guard(&c, bi, meta, &g16, &g64);
			if ((bi->csum_type == BLK_INTEGRITY_CSUM_IP ?
				     (uint16_t)(pi[0] | (pi[1] << 8)) :
				     blk_be16_get(pi)) != g16) {
				WARN_RATELIMIT(1, "%s: guard tag error at sector %llu (rcvd %04x, want %04x)",
					       name, (unsigned long long)seed,
					       blk_be16_get(pi), g16);
				return BLK_STS_PROTECTION;
			}
			continue;
skip16:
			blk_cursor_csum(&c, blk_bi_interval(bi), bi->csum_type,
					&g16, &g64);
		}
	}
	return BLK_STS_OK;
}

/* ==========================================================================
 * Reference tag remapping (type 1/2)
 * ========================================================================== */

/* The first interval number of the request as the device counts it. */
static uint64_t blk_pi_phys_ref(const struct blk_request *rq,
				const struct blk_integrity *bi)
{
	uint32_t shift = bi->interval_exp ? bi->interval_exp : 9;
	uint32_t lbs = rq->disk ? rq->disk->limits.logical_block_size : 512;
	uint32_t lshift = 9;

	while ((1u << lshift) < lbs)
		lshift++;
	if (!bi->interval_exp)
		shift = lshift;
	return blk_rq_pos(rq) >> (shift - 9);
}

/* Rewrite every reference tag equal to `from'+i into `to'+i, for the
 * first `intervals' intervals. */
static void blk_pi_remap(struct blk_request *rq, const struct blk_integrity *bi,
			 uint64_t from, uint64_t to, uint32_t intervals)
{
	uint8_t *meta = (uint8_t *)rq->integrity->buf;
	uint32_t tuple = blk_bi_tuple(bi);

	for (uint32_t i = 0; i < intervals; i++, meta += tuple, from++, to++) {
		uint8_t *pi = meta + bi->pi_offset;

		if (bi->csum_type == BLK_INTEGRITY_CSUM_CRC64) {
			if (blk_be48_get(pi + 10) == (from & ((1ULL << 48) - 1)))
				blk_be48_put(pi + 10, to & ((1ULL << 48) - 1));
		} else {
			if (blk_be32_get(pi + 4) == (uint32_t)from)
				blk_be32_put(pi + 4, (uint32_t)to);
		}
	}
}

void blk_integrity_prepare(struct blk_request *rq)
{
	const struct blk_integrity *bi = blk_rq_bi(rq);

	if (!blk_integrity_rq(rq) || !bi || !(bi->flags & BLK_INTEGRITY_REF_TAG))
		return;
	if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE)
		return;
	/* Already remapped (a retry of the same request)? */
	if (rq->integrity->flags & BIP_MAPPED_INTEGRITY)
		return;
	blk_pi_remap(rq, bi, rq->integrity->seed, blk_pi_phys_ref(rq, bi),
		     blk_pi_intervals(rq, bi));
	rq->integrity->flags |= BIP_MAPPED_INTEGRITY;
}

void blk_integrity_complete(struct blk_request *rq, unsigned int nr_bytes)
{
	const struct blk_integrity *bi = blk_rq_bi(rq);
	uint32_t intervals;

	if (!blk_integrity_rq(rq) || !bi || !(bi->flags & BLK_INTEGRITY_REF_TAG))
		return;
	if (bi->csum_type == BLK_INTEGRITY_CSUM_NONE)
		return;
	intervals = nr_bytes >> (bi->interval_exp ? bi->interval_exp : 9);
	if (intervals > blk_pi_intervals(rq, bi))
		intervals = blk_pi_intervals(rq, bi);
	blk_pi_remap(rq, bi, blk_pi_phys_ref(rq, bi), rq->integrity->seed,
		     intervals);
}

/* ==========================================================================
 * The block layer's own metadata buffers
 * ========================================================================== */

int blk_integrity_prep(struct blk_request *rq)
{
	const struct blk_integrity *bi;
	struct blk_integrity_payload *bip;
	uint64_t data, len, phys;
	uint32_t tuple, pages;
	int set_flags = 1, zero_only = 0;

	if (!rq->queue || rq->integrity || !blk_queue_is_mq(rq->queue))
		return 0;
	bi = &rq->queue->limits.integrity;
	tuple = blk_bi_tuple(bi);
	if (!tuple)
		return 0;
	switch (rq->op) {
	case BLK_OP_READ:
		if (bi->flags & BLK_INTEGRITY_NOVERIFY) {
			if (blk_bi_offload_capable(bi))
				return 0;
			set_flags = 0;
		}
		break;
	case BLK_OP_WRITE:
		if (bi->flags & BLK_INTEGRITY_NOGENERATE) {
			if (blk_bi_offload_capable(bi))
				return 0;
			set_flags = 0;
			zero_only = 1;
		}
		break;
	default:
		return 0;
	}
	data = blk_rq_payload_bytes(rq);
	len = (data >> (bi->interval_exp ? bi->interval_exp : 9)) * tuple;
	if (!len)
		return 0;
	pages = (uint32_t)((len + 4095) / 4096);
	phys = mm_allocate_contiguous_pages(pages);
	if (!phys)
		return -ENOMEM;
	bip = &rq->integrity_inline;
	mm_memset(bip, 0, sizeof(*bip));
	bip->phys = phys;
	bip->buf = phys_to_virt(phys);
	bip->len = (uint32_t)len;
	mm_memset(bip->buf, 0, (size_t)pages * 4096);
	bip->flags = BIP_BLOCK_INTEGRITY;
	if (set_flags) {
		if (bi->csum_type == BLK_INTEGRITY_CSUM_IP)
			bip->flags |= BIP_IP_CHECKSUM;
		if (bi->csum_type)
			bip->flags |= BIP_CHECK_GUARD;
		if (bi->flags & BLK_INTEGRITY_REF_TAG)
			bip->flags |= BIP_CHECK_REFTAG;
	}
	bip->seed = blk_rq_pos(rq);
	rq->integrity = bip;
	rq->nr_integrity_segs = 1;
	rq->flags |= BLK_REQ_INTEGRITY | BLK_REQ_NOMERGE;
	rq->bl.internal |= BLK_RQI_INTEGRITY_GEN;
	if (rq->op == BLK_OP_WRITE && set_flags && !zero_only)
		(void)blk_integrity_generate(rq);
	return 0;
}

void blk_integrity_free(struct blk_request *rq)
{
	struct blk_integrity_payload *bip;

	if (!(rq->bl.internal & BLK_RQI_INTEGRITY_GEN))
		return;
	bip = rq->integrity;
	rq->bl.internal &= ~BLK_RQI_INTEGRITY_GEN;
	rq->flags &= ~BLK_REQ_INTEGRITY;
	rq->integrity = NULL;
	rq->nr_integrity_segs = 0;
	if (bip && bip->phys) {
		VM_BUG_ON(bip->phys & 4095);
		mm_free_contiguous_pages(bip->phys, (bip->len + 4095) / 4096);
	}
	if (bip)
		mm_memset(bip, 0, sizeof(*bip));
}

blk_status_t blk_integrity_end(struct blk_request *rq, blk_status_t status)
{
	const struct blk_integrity *bi = blk_rq_bi(rq);

	if (!blk_integrity_rq(rq) || !bi)
		return status;
	if (rq->op != BLK_OP_READ || status != BLK_STS_OK)
		return status;
	/* Back to the virtual reference tags the submitter generated
	 * with, then -- for metadata the block layer owns -- the check. */
	blk_integrity_complete(rq, (unsigned int)blk_rq_payload_bytes(rq));
	if ((rq->integrity->flags & BIP_BLOCK_INTEGRITY) &&
	    (rq->integrity->flags & (BIP_CHECK_GUARD | BIP_CHECK_REFTAG)) &&
	    !(bi->flags & BLK_INTEGRITY_NOVERIFY))
		return blk_integrity_verify(rq);
	return status;
}
