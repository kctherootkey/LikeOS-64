// LikeOS - NVMe I/O commands: the submission entry of every block request
// (read, write, flush, dataset management, write zeroes, zone append and
// zone management, protection information, FUA, limited retry, write
// streams), the NVM command set helpers (compare, verify, write
// uncorrectable, copy, fused compare and write), and the completion side:
// the disposition of a finished command (complete, retry after the
// command retry delay, fail over to another path), the status to block
// status mapping and the end of a request.
//
// The strategy is the established one:
//   * nvme_setup_cmd() runs from the transport's queue_rq with the PDU's
//     command buffer; the first preparation of a request clears the
//     per-command state, a retried request keeps its retry count;
//   * a request needing a payload that is not its data (the DSM range
//     list) gets it as the "special payload" of its nvme_request, from a
//     per-command allocation or -- when that fails -- the controller's one
//     discard page (one user at a time), so discard always makes progress;
//   * on completion: success or "do not retry" (DNR, fail-fast request,
//     max_retries reached, queue dying) completes; a path error of a
//     multipath request fails over; anything else is requeued after the
//     controller's CRDT delay selected by the status' CRD field.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_ns_internal.h>

/* ==========================================================================
 * Status mapping
 * ========================================================================== */

blk_status_t nvme_error_status(uint16_t status)
{
	switch (status & NVME_SCT_SC_MASK) {
	case NVME_SC_SUCCESS:
		return BLK_STS_OK;
	case NVME_SC_CAP_EXCEEDED:
		return BLK_STS_NOSPC;
	case NVME_SC_LBA_RANGE:
	case NVME_SC_CMD_INTERRUPTED:
	case NVME_SC_NS_NOT_READY:
		return BLK_STS_TARGET;
	case NVME_SC_BAD_ATTRIBUTES:
	case NVME_SC_INVALID_OPCODE:
	case NVME_SC_INVALID_FIELD:
	case NVME_SC_INVALID_NS:
		return BLK_STS_NOTSUPP;
	case NVME_SC_WRITE_FAULT:
	case NVME_SC_READ_ERROR:
	case NVME_SC_UNWRITTEN_BLOCK:
	case NVME_SC_ACCESS_DENIED:
	case NVME_SC_READ_ONLY:
	case NVME_SC_COMPARE_FAILED:
		return BLK_STS_MEDIUM;
	case NVME_SC_GUARD_CHECK:
	case NVME_SC_APPTAG_CHECK:
	case NVME_SC_REFTAG_CHECK:
	case NVME_SC_INVALID_PI:
		return BLK_STS_PROTECTION;
	case NVME_SC_RESERVATION_CONFLICT:
		return BLK_STS_RESV_CONFLICT;
	case NVME_SC_HOST_PATH_ERROR:
		return BLK_STS_TRANSPORT;
	case NVME_SC_ZONE_TOO_MANY_ACTIVE:
		return BLK_STS_ZONE_ACTIVE_RESOURCE;
	case NVME_SC_ZONE_TOO_MANY_OPEN:
		return BLK_STS_ZONE_OPEN_RESOURCE;
	default:
		return BLK_STS_IOERR;
	}
}

/* ==========================================================================
 * Completion: disposition, retry, end
 * ========================================================================== */

static unsigned int nvme_max_retries(void)
{
	return nvme_core_param.max_retries;
}

enum nvme_disposition nvme_decide_disposition(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);

	if (likely(nr->status == 0))
		return COMPLETE;
	/* Cancelled at the submitter's request: its status is the answer. */
	if (nr->flags & NVME_REQ_HOST_CANCEL)
		return COMPLETE;

	/* The retry count lives with the request (its tag -- and with it the
	 * PDU -- may change across a requeue). */
	if (blk_noretry_request(rq) || (nr->status & NVME_STATUS_DNR) ||
	    rq->retries >= nvme_max_retries())
		return COMPLETE;

	/* (Authentication exists only on fabrics: never AUTHENTICATE.) */

	if (rq->flags & BLK_REQ_MPATH) {
		if (nvme_is_path_error(nr->status) ||
		    (rq->queue && blk_queue_dying(rq->queue)))
			return FAILOVER;
	} else {
		if (rq->queue && blk_queue_dying(rq->queue))
			return COMPLETE;
	}
	return RETRY;
}

static void nvme_retry_req(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);
	unsigned long delay_ms = 0;

	/* CRD selects CRDT1..3 (100 ms units); 0 retries at once. */
	BUG_ON(NVME_CRD(nr->status) > 3);
	if (nr->ctrl)
		delay_ms = nvme_crd_delay_ms(nr->ctrl, nr->status);

	rq->retries++;
	nr->retries = (uint8_t)(rq->retries > 255 ? 255 : rq->retries);
	blk_mq_requeue_request(rq, 0);
	blk_mq_delay_kick_requeue_list(rq->queue, delay_ms);
}

/* A zone append reports where the data landed (completion dword 0/1: the
 * LBA of the first block written).  rq->append_sector is in the disk's
 * logical sectors, like rq->sector. */
static void nvme_end_req_zoned(struct blk_request *rq)
{
	if (req_op(rq) == BLK_OP_ZONE_APPEND)
		rq->append_sector = le64_to_cpu(nvme_req(rq)->result.u64);
}

static void __nvme_end_req(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);

	if (unlikely(nr->status && !(rq->flags & BLK_REQ_QUIET))) {
		if (blk_rq_is_passthrough(rq))
			nvme_log_err_passthru(rq);
		else
			nvme_log_error(rq);
	}
	nvme_end_req_zoned(rq);
	if (rq->flags & BLK_REQ_MPATH)
		nvme_mpath_end_request(rq);
}

void nvme_end_req(struct blk_request *rq)
{
	blk_status_t status = nvme_error_status(nvme_req(rq)->status);

	__nvme_end_req(rq);
	blk_mq_end_request(rq, status);
}

/* When was the request handed to the controller? (keep-alive heuristics) */
static uint64_t nvme_rq_submit_time(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);

	if (nr->start_time_ns)
		return nr->start_time_ns;
	if (rq->dispatch_ns)
		return rq->dispatch_ns;
	return rq->submit_ns;
}

void nvme_complete_rq(struct blk_request *rq)
{
	struct nvme_ctrl *ctrl = nvme_req(rq)->ctrl;

	BUG_ON(!ctrl);
	nvme_cleanup_cmd(rq);

	/* A completion of a command issued after the last keep-alive check
	 * proves the controller alive (traffic based keep-alive); long
	 * running commands completing now prove nothing. */
	if (ctrl->kas && nvme_rq_submit_time(rq) >= ctrl->ka_last_check_ns)
		ctrl->comp_seen = true;

	switch (nvme_decide_disposition(rq)) {
	case COMPLETE:
		nvme_end_req(rq);
		return;
	case RETRY:
		nvme_retry_req(rq);
		return;
	case FAILOVER:
		nvme_failover_req(rq);
		return;
	case AUTHENTICATE:
		/* Never decided here (no fabrics authentication). */
		WARN_ON_ONCE(1);
		nvme_end_req(rq);
		return;
	}
}

void nvme_complete_batch_req(struct blk_request *rq)
{
	nvme_cleanup_cmd(rq);
	__nvme_end_req(rq);
}

void nvme_complete_batch(struct io_comp_batch *iob,
			 void (*fn)(struct blk_request *rq))
{
	struct blk_request *rq;

	for (rq = iob->req_list; rq; rq = rq->rq_next) {
		if (fn)
			fn(rq);
		nvme_complete_batch_req(rq);
	}
	blk_mq_end_request_batch(iob);
}

bool nvme_try_complete_req(struct blk_request *rq, __le16 status,
			   union nvme_result result)
{
	struct nvme_request *nr = nvme_req(rq);
	struct nvme_ctrl *ctrl = nr->ctrl;

	BUG_ON(!ctrl);
	/* A new generation for the tag: a late duplicate completion of this
	 * command ID is recognised as stale. */
	if (!(ctrl->quirks & NVME_QUIRK_SKIP_CID_GEN))
		__atomic_store_n(&nr->genctr, (uint8_t)(nvme_req_genctr(nr) + 1),
				 __ATOMIC_RELAXED);
	nr->status = (uint16_t)(le16_to_cpu(status) >> 1);
	nr->result = result;
	/* Status injection (fault_inject attributes). */
	nvme_should_fail(rq);
	/* Timeout injection: the completion is "lost". */
	if (unlikely(nvme_fault_drop_completion(ctrl, rq)))
		return true;
	return blk_mq_complete_request_remote(rq) != 0;
}

blk_status_t nvme_host_path_error(struct blk_request *rq)
{
	nvme_req(rq)->status = NVME_SC_HOST_PATH_ERROR;
	blk_mq_set_request_complete(rq);
	nvme_complete_rq(rq);
	return BLK_STS_OK;
}

/* ==========================================================================
 * Command setup
 * ========================================================================== */

static void nvme_clear_nvme_request(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);

	nr->status = 0;
	nr->retries = 0;
	nr->flags = 0;
	nr->special_vec = NULL;
	nr->special_len = 0;
	rq->retries = 0;
	rq->flags |= BLK_REQ_DONTPREP;
}

static void nvme_setup_flush(struct nvme_ns *ns, struct nvme_command *cmnd)
{
	kmemset(cmnd, 0, sizeof(*cmnd));
	cmnd->common.opcode = nvme_cmd_flush;
	cmnd->common.nsid = cpu_to_le32(ns->head->ns_id);
}

/* The LBA and block count of a data or range request, in namespace
 * blocks. */
static uint64_t nvme_rq_slba(struct nvme_ns *ns, struct blk_request *rq)
{
	return nvme_sect_to_lba(ns->head, blk_rq_pos(rq));
}

static uint64_t nvme_rq_nlb(struct nvme_ns *ns, struct blk_request *rq)
{
	return blk_rq_bytes(rq) >> ns->head->lba_shift;
}

/*
 * Fill DSM ranges for [slba, slba + nlb): one range per 2^32 - 1 blocks
 * (the 32-bit NLB of a range).  Returns the number written, or max + 1
 * when they do not fit.  Pure (host-tested).
 */
unsigned int nvme_dsm_fill_ranges(struct nvme_dsm_range *range,
				  unsigned int max, uint64_t slba, uint64_t nlb);
unsigned int nvme_dsm_fill_ranges(struct nvme_dsm_range *range,
				  unsigned int max, uint64_t slba, uint64_t nlb)
{
	unsigned int n = 0;

	while (nlb) {
		uint32_t len = nlb > 0xffffffffULL ? 0xffffffffu :
						     (uint32_t)nlb;

		if (n >= max)
			return max + 1;
		range[n].cattr = cpu_to_le32(0);
		range[n].nlb = cpu_to_le32(len);
		range[n].slba = cpu_to_le64(slba);
		n++;
		slba += len;
		nlb -= len;
	}
	return n;
}

static void nvme_free_special_payload(struct nvme_ctrl *ctrl, void *range)
{
	if (range == ctrl->discard_page)
		nvme_clear_bit(0, &ctrl->discard_page_busy);
	else
		kfree(range);
}

static blk_status_t nvme_setup_discard(struct nvme_ns *ns,
				       struct blk_request *rq,
				       struct nvme_command *cmnd)
{
	struct nvme_ctrl *ctrl = ns->ctrl;
	struct nvme_dsm_range *range;
	unsigned int max_ranges, n;
	/* Some devices ignore the Number of Ranges field when deciding how
	 * much to transfer: always provide the largest range list. */
	const size_t alloc_size =
		sizeof(struct nvme_dsm_range) * NVME_DSM_MAX_RANGES;

	range = (struct nvme_dsm_range *)nvme_kzalloc(alloc_size);
	if (!range) {
		/* The controller's discard page; if it is busy too, a
		 * completion will free it: try again later. */
		if (!ctrl->discard_page ||
		    nvme_test_and_set_bit(0, &ctrl->discard_page_busy))
			return BLK_STS_RESOURCE;
		range = (struct nvme_dsm_range *)ctrl->discard_page;
		kmemset(range, 0, alloc_size);
	}

	max_ranges = ctrl->dmrl ? ctrl->dmrl : NVME_DSM_MAX_RANGES;
	if (max_ranges > NVME_DSM_MAX_RANGES)
		max_ranges = NVME_DSM_MAX_RANGES;
	/* The request (with what merged into it) covers one contiguous
	 * range of the disk: the elevator only merges neighbours. */
	n = nvme_dsm_fill_ranges(range, max_ranges, nvme_rq_slba(ns, rq),
				 nvme_rq_nlb(ns, rq));
	if (WARN_ON_ONCE(n == 0 || n > max_ranges)) {
		nvme_free_special_payload(ctrl, range);
		return BLK_STS_IOERR;
	}

	kmemset(cmnd, 0, sizeof(*cmnd));
	cmnd->dsm.opcode = nvme_cmd_dsm;
	cmnd->dsm.nsid = cpu_to_le32(ns->head->ns_id);
	cmnd->dsm.nr = cpu_to_le32(n - 1);
	cmnd->dsm.attributes = cpu_to_le32(NVME_DSMGMT_AD);

	nvme_req(rq)->special_vec = range;
	nvme_req(rq)->special_len = (uint32_t)alloc_size;
	nvme_req(rq)->flags |= NVME_REQ_SPECIAL_PAYLOAD;
	return BLK_STS_OK;
}

/* The expected initial reference tag: the request's first protection
 * interval (= logical block), 32 bits for the 16-bit guard format, 48
 * bits for the 64-bit guard format. */
static uint64_t nvme_rq_ref_tag(struct nvme_ns *ns, struct blk_request *rq)
{
	/* The protection interval is the logical block: the ref tag
	 * is the LBA on the device (the block layer remapped the buffer's
	 * tags from the submitter's seed to it). */
	return nvme_rq_slba(ns, rq);
}

static void nvme_set_app_tag(struct blk_request *rq, struct nvme_command *cmnd)
{
	uint16_t tag = rq->integrity ? rq->integrity->app_tag : 0;

	cmnd->rw.lbat = cpu_to_le16(tag);
	cmnd->rw.lbatm = cpu_to_le16(0xffff);
}

static void nvme_set_ref_tag(struct nvme_ns *ns, struct nvme_command *cmnd,
			     struct blk_request *rq)
{
	uint64_t ref48;

	/* Only types 1 and 2 have a reference tag. */
	switch (ns->head->pi_type) {
	case NVME_NS_DPS_PI_TYPE1:
	case NVME_NS_DPS_PI_TYPE2:
		break;
	default:
		return;
	}

	/* Read/write and write zeroes share the ref tag layout. */
	switch (ns->head->guard_type) {
	case NVME_NVM_NS_16B_GUARD:
		cmnd->rw.reftag =
			cpu_to_le32((uint32_t)nvme_rq_ref_tag(ns, rq));
		break;
	case NVME_NVM_NS_64B_GUARD:
		ref48 = nvme_lower_48_bits(nvme_rq_ref_tag(ns, rq));
		cmnd->rw.reftag = cpu_to_le32((uint32_t)ref48);
		cmnd->rw.cdw3 = cpu_to_le32((uint32_t)(ref48 >> 32));
		break;
	default:
		break;
	}
}

static blk_status_t nvme_setup_write_zeroes(struct nvme_ns *ns,
					    struct blk_request *rq,
					    struct nvme_command *cmnd)
{
	uint64_t nlb;

	kmemset(cmnd, 0, sizeof(*cmnd));

	/* Deallocation reads back zeroes on these: a DSM does it. */
	if (ns->ctrl->quirks & NVME_QUIRK_DEALLOCATE_ZEROES)
		return nvme_setup_discard(ns, rq, cmnd);

	nlb = nvme_rq_nlb(ns, rq);
	if (WARN_ON_ONCE(nlb == 0 || nlb > 0x10000))
		return BLK_STS_IOERR;

	cmnd->write_zeroes.opcode = nvme_cmd_write_zeroes;
	cmnd->write_zeroes.nsid = cpu_to_le32(ns->head->ns_id);
	cmnd->write_zeroes.slba = cpu_to_le64(nvme_rq_slba(ns, rq));
	cmnd->write_zeroes.length = cpu_to_le16((uint16_t)(nlb - 1));

	if (!(rq->flags & BLK_REQ_NOUNMAP) &&
	    (ns->head->features & NVME_NS_DEAC))
		cmnd->write_zeroes.control |= cpu_to_le16(NVME_WZ_DEAC);

	if (nvme_ns_has_pi(ns->head)) {
		cmnd->write_zeroes.control |=
			cpu_to_le16(NVME_RW_PRINFO_PRACT);
		nvme_set_ref_tag(ns, cmnd, rq);
	}
	return BLK_STS_OK;
}

/*
 * NVMe has no atomic write command: a write within the device's atomic
 * limits is atomic, one outside them is silently not.  The submitter
 * keeps to the queue's limits; check anyway.
 */
static bool nvme_valid_atomic_write(struct blk_request *rq)
{
	const struct blk_limits *lim = &rq->queue->limits;
	uint64_t bytes = blk_rq_bytes(rq);
	uint32_t boundary = lim->atomic_write_hw_boundary;

	if (bytes > lim->atomic_write_unit_max)
		return false;
	if (boundary) {
		uint64_t mask = (uint64_t)boundary - 1, imask = ~mask;
		uint64_t start = blk_rq_pos(rq) << 9;
		uint64_t end = start + bytes - 1;

		/* Larger than the boundary must cross it. */
		if (bytes > boundary)
			return false;
		if ((start & imask) != (end & imask))
			return false;
	}
	return true;
}

static blk_status_t nvme_setup_rw(struct nvme_ns *ns, struct blk_request *rq,
				  struct nvme_command *cmnd, uint8_t op)
{
	uint16_t control = 0;
	uint32_t dsmgmt = 0;
	uint64_t nlb;

	if (rq->flags & BLK_REQ_FUA)
		control |= NVME_RW_FUA;
	if (rq->flags & (BLK_REQ_FAILFAST_DEV | BLK_REQ_RAHEAD))
		control |= NVME_RW_LR;
	if (rq->flags & BLK_REQ_RAHEAD)
		dsmgmt |= NVME_RW_DSM_FREQ_PREFETCH;

	if (op == nvme_cmd_write) {
		if (ns->head->nr_plids) {
			/* Flexible data placement: write stream N is the
			 * namespace's placement handle N - 1. */
			blk_status_t sts = nvme_write_stream_dspec(
				ns, rq->write_stream, &control, &dsmgmt);

			if (sts != BLK_STS_OK)
				return sts;
		} else if (nvme_ctrl_nr_streams(ns->ctrl)) {
			nvme_assign_write_stream(ns->ctrl, rq, &control,
						 &dsmgmt);
		}
	}

	if ((rq->flags & BLK_REQ_ATOMIC) && !nvme_valid_atomic_write(rq))
		return BLK_STS_INVAL;

	nlb = nvme_rq_nlb(ns, rq);
	if (WARN_ON_ONCE(nlb == 0 || nlb > 0x10000))
		return BLK_STS_IOERR;

	cmnd->rw.opcode = op;
	cmnd->rw.flags = 0;
	cmnd->rw.nsid = cpu_to_le32(ns->head->ns_id);
	cmnd->rw.cdw2 = 0;
	cmnd->rw.cdw3 = 0;
	cmnd->rw.metadata = 0;
	cmnd->rw.slba = cpu_to_le64(nvme_rq_slba(ns, rq));
	cmnd->rw.length = cpu_to_le16((uint16_t)(nlb - 1));
	cmnd->rw.reftag = 0;
	cmnd->rw.lbat = 0;
	cmnd->rw.lbatm = 0;

	if (ns->head->ms) {
		/*
		 * Formatted with metadata: the block layer supplies the
		 * metadata buffer when it has an integrity profile.  Without
		 * one only PI can be handled -- the controller inserts and
		 * strips it (PRACT); other metadata formats were given a
		 * capacity of 0 and never get here.
		 */
		if (!blk_integrity_rq(rq)) {
			if (WARN_ON_ONCE(!nvme_ns_has_pi(ns->head)))
				return BLK_STS_NOTSUPP;
			control |= NVME_RW_PRINFO_PRACT;
			nvme_set_ref_tag(ns, cmnd, rq);
		} else if (rq->integrity) {
			uint32_t f = rq->integrity->flags;

			if (f & BIP_CHECK_GUARD)
				control |= NVME_RW_PRINFO_PRCHK_GUARD;
			if (f & BIP_CHECK_REFTAG) {
				control |= NVME_RW_PRINFO_PRCHK_REF;
				if (op == nvme_cmd_zone_append)
					control |= NVME_RW_APPEND_PIREMAP;
				nvme_set_ref_tag(ns, cmnd, rq);
			}
			if (f & BIP_CHECK_APPTAG) {
				control |= NVME_RW_PRINFO_PRCHK_APP;
				nvme_set_app_tag(rq, cmnd);
			}
		}
	}

	cmnd->rw.control = cpu_to_le16(control);
	cmnd->rw.dsmgmt = cpu_to_le32(dsmgmt);
	return BLK_STS_OK;
}

void nvme_cleanup_cmd(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);

	if (nr->flags & NVME_REQ_SPECIAL_PAYLOAD) {
		BUG_ON(!nr->special_vec);
		nvme_free_special_payload(nr->ctrl, nr->special_vec);
		nr->special_vec = NULL;
		nr->special_len = 0;
		/* Atomic: the byte also takes concurrent cancel bits. */
		__atomic_and_fetch(&nr->flags,
				   (uint8_t)~NVME_REQ_SPECIAL_PAYLOAD,
				   __ATOMIC_RELAXED);
	}
}

/* Is the block range of a data/range request inside the namespace? */
static bool nvme_rq_in_range(struct nvme_ns *ns, struct blk_request *rq)
{
	uint64_t cap = ns->disk ? ns->disk->capacity : 0;
	uint64_t slba, nlb;

	if (!cap)
		return true; /* not known (capacity 0 means no block I/O) */
	slba = nvme_rq_slba(ns, rq);
	nlb = nvme_rq_nlb(ns, rq);
	return slba < cap && nlb <= cap - slba;
}

blk_status_t nvme_setup_cmd(struct nvme_ns *ns, struct blk_request *rq)
{
	struct nvme_command *cmd = nvme_req(rq)->cmd;
	blk_status_t ret = BLK_STS_OK;
	enum blk_op op = req_op(rq);

	BUG_ON(!cmd || !nvme_req(rq)->ctrl);
	/* Only pass-through runs without a namespace (the admin queue). */
	BUG_ON(!ns && !blk_rq_is_passthrough(rq));
	if (!(rq->flags & BLK_REQ_DONTPREP))
		nvme_clear_nvme_request(rq);
	else
		nvme_req(rq)->retries =
			(uint8_t)(rq->retries > 255 ? 255 : rq->retries);

	/* A data or range op must stay inside the namespace (the block
	 * layer checked against the partition; this is the device's own
	 * guard against a stale capacity after a resize). */
	if (ns && (blk_op_has_data(op) || op == BLK_OP_DISCARD ||
		   op == BLK_OP_SECURE_ERASE || op == BLK_OP_WRITE_ZEROES) &&
	    !blk_rq_is_passthrough(rq) && !nvme_rq_in_range(ns, rq)) {
		nvme_pr_ratelimited(ns->disk ? ns->disk->disk_name : "nvme",
				    "%s beyond the namespace end (lba %llu)\n",
				    blk_op_str(op),
				    (unsigned long long)nvme_rq_slba(ns, rq));
		return BLK_STS_IOERR;
	}

	switch (op) {
	case BLK_OP_DRV_IN:
	case BLK_OP_DRV_OUT:
		/* Prepared before execution (nvme_init_request). */
		break;
	case BLK_OP_FLUSH:
		nvme_setup_flush(ns, cmd);
		break;
	case BLK_OP_ZONE_RESET_ALL:
	case BLK_OP_ZONE_RESET:
		ret = nvme_setup_zone_mgmt_send(ns, rq, cmd, NVME_ZONE_RESET);
		break;
	case BLK_OP_ZONE_OPEN:
		ret = nvme_setup_zone_mgmt_send(ns, rq, cmd, NVME_ZONE_OPEN);
		break;
	case BLK_OP_ZONE_CLOSE:
		ret = nvme_setup_zone_mgmt_send(ns, rq, cmd, NVME_ZONE_CLOSE);
		break;
	case BLK_OP_ZONE_FINISH:
		ret = nvme_setup_zone_mgmt_send(ns, rq, cmd, NVME_ZONE_FINISH);
		break;
	case BLK_OP_WRITE_ZEROES:
		ret = nvme_setup_write_zeroes(ns, rq, cmd);
		break;
	case BLK_OP_DISCARD:
	case BLK_OP_SECURE_ERASE:
		/* (Secure erase is never advertised: the namespace has no
		 * secure_erase limit; treated as a deallocate if it comes.) */
		ret = nvme_setup_discard(ns, rq, cmd);
		break;
	case BLK_OP_READ:
		ret = nvme_setup_rw(ns, rq, cmd, nvme_cmd_read);
		break;
	case BLK_OP_WRITE:
		ret = nvme_setup_rw(ns, rq, cmd, nvme_cmd_write);
		break;
	case BLK_OP_ZONE_APPEND:
		ret = nvme_setup_rw(ns, rq, cmd, nvme_cmd_zone_append);
		break;
	default:
		WARN_ON_ONCE(1);
		return BLK_STS_IOERR;
	}

	/* The command ID with the tag's next generation. */
	nvme_assign_cid(nvme_req(rq)->ctrl, rq, cmd);
	return ret;
}

/* ==========================================================================
 * NVM command set helpers (compare, verify, write uncorrectable, copy,
 * fused compare and write)
 * ========================================================================== */

static void nvme_init_lba_cmd(struct nvme_ns *ns, struct nvme_command *c,
			      uint8_t op, uint64_t slba, uint32_t nlb,
			      uint16_t control)
{
	kmemset(c, 0, sizeof(*c));
	c->rw.opcode = op;
	c->rw.nsid = cpu_to_le32(ns->head->ns_id);
	c->rw.slba = cpu_to_le64(slba);
	c->rw.length = cpu_to_le16((uint16_t)(nlb ? nlb - 1 : 0));
	/* Namespaces with PI and no metadata buffer from the caller: let
	 * the controller check/strip (PRACT) unless the caller decided. */
	if (nvme_ns_has_pi(ns->head) && !(control & NVME_RW_PRINFO_MASK)) {
		control |= NVME_RW_PRINFO_PRACT;
		if (ns->head->pi_type == NVME_NS_DPS_PI_TYPE1 ||
		    ns->head->pi_type == NVME_NS_DPS_PI_TYPE2) {
			if (ns->head->guard_type == NVME_NVM_NS_64B_GUARD) {
				uint64_t r = nvme_lower_48_bits(slba);

				c->rw.reftag = cpu_to_le32((uint32_t)r);
				c->rw.cdw3 = cpu_to_le32((uint32_t)(r >> 32));
			} else {
				c->rw.reftag = cpu_to_le32((uint32_t)slba);
			}
		}
	}
	c->rw.control = cpu_to_le16(control);
}

void nvme_init_compare_cmd(struct nvme_ns *ns, struct nvme_command *c,
			   uint64_t slba, uint32_t nlb, uint16_t control)
{
	BUG_ON(!ns || !c);
	WARN_ON_ONCE(nlb == 0 || nlb > 0x10000);
	nvme_init_lba_cmd(ns, c, nvme_cmd_compare, slba, nlb, control);
}

void nvme_init_verify_cmd(struct nvme_ns *ns, struct nvme_command *c,
			  uint64_t slba, uint32_t nlb, uint16_t control)
{
	BUG_ON(!ns || !c);
	WARN_ON_ONCE(nlb == 0 || nlb > 0x10000);
	/* Verify moves no data: FUA/LR/PRINFO only. */
	nvme_init_lba_cmd(ns, c, nvme_cmd_verify, slba, nlb,
			  control & (NVME_RW_FUA | NVME_RW_LR |
				     NVME_RW_PRINFO_MASK | NVME_RW_STC));
}

void nvme_init_write_uncor_cmd(struct nvme_ns *ns, struct nvme_command *c,
			       uint64_t slba, uint32_t nlb)
{
	BUG_ON(!ns || !c);
	WARN_ON_ONCE(nlb == 0 || nlb > 0x10000);
	kmemset(c, 0, sizeof(*c));
	c->rw.opcode = nvme_cmd_write_uncor;
	c->rw.nsid = cpu_to_le32(ns->head->ns_id);
	c->rw.slba = cpu_to_le64(slba);
	c->rw.length = cpu_to_le16((uint16_t)(nlb ? nlb - 1 : 0));
}

int nvme_init_copy_cmd(struct nvme_ns *ns, struct nvme_command *c, void *buf,
		       size_t buflen, uint64_t sdlba,
		       const struct nvme_copy_src *ranges, unsigned int nr)
{
	struct nvme_copy_range *d = (struct nvme_copy_range *)buf;
	struct nvme_ns_head *head;
	uint64_t total = 0;

	BUG_ON(!ns || !c);
	head = ns->head;
	if (!(ns->ctrl->oncs & NVME_CTRL_ONCS_COPY))
		return -EOPNOTSUPP;
	/* Descriptor format 0 is the one every copy-capable controller
	 * takes; a 64-bit guard namespace needs format 1 (not offered). */
	if (!(ns->ctrl->ocfs & NVME_CTRL_OCFS_FMT_0) ||
	    (nvme_ns_has_pi(head) &&
	     head->guard_type != NVME_NVM_NS_16B_GUARD))
		return -EOPNOTSUPP;
	if (!nr || nr > 256 || nr > (unsigned int)head->msrc + 1)
		return -EINVAL;
	if (!buf || buflen < nr * sizeof(*d))
		return -EINVAL;
	kmemset(buf, 0, nr * sizeof(*d));
	for (unsigned int i = 0; i < nr; i++) {
		if (!ranges[i].nlb || ranges[i].nlb > 0x10000)
			return -EINVAL;
		if (head->mssrl && ranges[i].nlb > head->mssrl)
			return -EINVAL;
		total += ranges[i].nlb;
		d[i].slba = cpu_to_le64(ranges[i].slba);
		d[i].nlb = cpu_to_le16((uint16_t)(ranges[i].nlb - 1));
		if (head->pi_type == NVME_NS_DPS_PI_TYPE1 ||
		    head->pi_type == NVME_NS_DPS_PI_TYPE2)
			d[i].eilbrt = cpu_to_le32((uint32_t)ranges[i].slba);
	}
	if (head->mcl && total > head->mcl)
		return -EINVAL;

	kmemset(c, 0, sizeof(*c));
	c->copy.opcode = nvme_cmd_copy;
	c->copy.nsid = cpu_to_le32(head->ns_id);
	c->copy.sdlba = cpu_to_le64(sdlba);
	c->copy.nr_range = (uint8_t)(nr - 1);
	c->copy.desfmt = NVME_COPY_FMT_0;
	if (nvme_ns_has_pi(head)) {
		/* Let the controller generate/check PI on both sides. */
		c->copy.desfmt |= (uint8_t)(NVME_COPY_PRINFO_PRACT
					    << NVME_COPY_PRINFOR_SHIFT);
		c->copy.control = cpu_to_le16(NVME_RW_PRINFO_PRACT);
		if (head->pi_type != NVME_NS_DPS_PI_TYPE3)
			c->copy.reftag = cpu_to_le32((uint32_t)sdlba);
	}
	return 0;
}

int nvme_init_fused_cmp_write(struct nvme_ns *ns, struct nvme_command *cmp,
			      struct nvme_command *wr, uint64_t slba,
			      uint32_t nlb)
{
	uint32_t limit;

	BUG_ON(!ns || !cmp || !wr);
	if (!(ns->ctrl->fuses & NVME_CTRL_FUSES_COMPARE_AND_WRITE))
		return -EOPNOTSUPP;
	if (!(ns->ctrl->oncs & NVME_CTRL_ONCS_COMPARE))
		return -EOPNOTSUPP;
	/* Atomic compare & write unit: the namespace's when it reports
	 * atomics, else the controller's (both 0's based). */
	limit = (uint32_t)(ns->head->nacwu ? ns->head->nacwu :
					     ns->ctrl->acwu) + 1;
	if (!nlb || nlb > limit)
		return -EOPNOTSUPP;

	nvme_init_lba_cmd(ns, cmp, nvme_cmd_compare, slba, nlb, 0);
	cmp->common.flags |= NVME_CMD_FUSE_FIRST;
	nvme_init_lba_cmd(ns, wr, nvme_cmd_write, slba, nlb, 0);
	wr->common.flags |= NVME_CMD_FUSE_SECOND;
	return 0;
}

int nvme_ns_submit_io_sync(struct nvme_ns *ns, struct nvme_command *c,
			   void *buf, unsigned int len)
{
	might_sleep();
	BUG_ON(!ns);
	if (!ns->queue)
		return -ENODEV;
	/* Fused halves need two adjacent submission queue slots: never
	 * through the one-command path. */
	if (c->common.flags & NVME_CMD_FUSE_MASK)
		return -EINVAL;
	c->common.nsid = cpu_to_le32(ns->head->ns_id);
	return nvme_submit_sync_cmd(ns->queue, c, buf, len);
}
