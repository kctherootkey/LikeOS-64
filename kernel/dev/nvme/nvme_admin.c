// LikeOS - NVMe admin tag set and queue, synchronous and asynchronous
// command submission, identify, features, log pages, abort, command
// effects and pass-through bracketing
//
// Every admin command the driver itself sends -- identify, features, log
// pages, abort, the keep-alive -- is a block-layer request on the
// controller's admin queue (ctrl->admin_q, a multi-queue queue without a
// disk on the admin tag set), exactly like a user pass-through command:
//
//   blk_mq_alloc_request()   a tag (the reserved pool for internal ones)
//                            and the transport's PDU, whose first member
//                            is struct nvme_request (nvme_req())
//   nvme_init_request()      the SQE copied into the PDU, flags cleared
//   blk_rq_map_kern()        the data buffer
//   blk_execute_rq()         queued, dispatched by the transport's
//                            queue_rq, waited for with the request's own
//                            completion (sleeps; no spinlock held)
//
// The completion carries the NVMe status in nvme_req(rq)->status (CQE
// status >> 1); the synchronous helpers return it as a positive value, a
// host failure as -errno.  The command ID is the tag with a 4-bit
// generation counter on top (nvme_assign_cid / nvme_find_rq), so a late
// completion of an aborted command can never complete the tag's next
// user.  The Asynchronous Event Request lives outside the tag set: the
// transport posts it with a command ID above NVME_AQ_BLK_MQ_DEPTH.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>

/* Reserved admin tags: one for the keep-alive (it must never wait behind
 * a full admin queue). */
#define NVME_AQ_RESERVED_TAGS 1

static uint32_t nvme_min_not_zero_u32(uint32_t a, uint32_t b)
{
	if (!a)
		return b;
	if (!b)
		return a;
	return a < b ? a : b;
}

/* ==========================================================================
 * Tag sets and the admin queue
 * ========================================================================== */

void nvme_set_ctrl_limits(struct nvme_ctrl *ctrl, struct blk_limits *lim,
			  bool is_admin)
{
	uint32_t drv_segments;

	/* One segment per controller page of the largest transfer, plus one
	 * for a misaligned start. */
	drv_segments = ctrl->max_hw_sectors /
			       (NVME_CTRL_PAGE_SIZE >> 9) +
		       1;
	lim->max_hw_sectors = ctrl->max_hw_sectors;
	lim->max_segments =
		nvme_min_not_zero_u32(drv_segments, ctrl->max_segments);
	if (lim->max_segments > 0xffff)
		lim->max_segments = 0xffff;
	if (ctrl->ops->get_virt_boundary)
		lim->virt_boundary_mask =
			ctrl->ops->get_virt_boundary(ctrl, is_admin);
	else
		lim->virt_boundary_mask = NVME_CTRL_PAGE_SIZE - 1;
	lim->max_segment_size = 0xffffffffu;
	lim->dma_alignment = 3;
}

int nvme_alloc_admin_tag_set(struct nvme_ctrl *ctrl,
			     struct blk_mq_tag_set *set,
			     const struct blk_mq_ops *ops,
			     unsigned int cmd_size)
{
	int ret;

	might_sleep();
	BUG_ON(!ops || cmd_size < sizeof(struct nvme_request));
	BUILD_BUG_ON(NVME_AQ_MQ_TAG_DEPTH <= NVME_AQ_RESERVED_TAGS);
	/* The AEN command IDs sit above every block-layer tag. */
	BUILD_BUG_ON(NVME_AQ_MQ_TAG_DEPTH > NVME_AQ_BLK_MQ_DEPTH);

	kmemset(set, 0, sizeof(*set));
	set->ops = ops;
	set->queue_depth = NVME_AQ_MQ_TAG_DEPTH;
	set->reserved_tags = NVME_AQ_RESERVED_TAGS;
	set->numa_node = (int)ctrl->numa_node;
	if (ctrl->ops->flags & NVME_F_BLOCKING)
		set->flags |= BLK_MQ_F_BLOCKING;
	set->cmd_size = cmd_size;
	set->driver_data = ctrl;
	set->nr_hw_queues = 1;
	set->nr_maps = 1;
	set->timeout_ms = nvme_core_param.admin_timeout_s * 1000u;
	ret = blk_mq_alloc_tag_set(set);
	if (ret)
		return ret;

	/* A queue left from before a reset goes now, not orphaned. */
	if (ctrl->admin_q) {
		blk_put_queue(ctrl->admin_q);
		ctrl->admin_q = NULL;
	}
	ctrl->admin_q = blk_mq_alloc_queue(set, NULL, NULL);
	if (!ctrl->admin_q) {
		blk_mq_free_tag_set(set);
		return -ENOMEM;
	}
	ctrl->admin_tagset = set;
	nvme_clear_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags);
	return 0;
}

void nvme_remove_admin_tag_set(struct nvme_ctrl *ctrl)
{
	might_sleep();
	/* The queue and tags go: no keep-alive may be running. */
	nvme_stop_keep_alive(ctrl);
	if (ctrl->admin_q)
		blk_mq_destroy_queue(ctrl->admin_q);
	if (ctrl->admin_tagset) {
		blk_mq_free_tag_set(ctrl->admin_tagset);
		ctrl->admin_tagset = NULL;
	}
	/* ctrl->admin_q keeps its last reference until nvme_free_ctrl. */
}

int nvme_alloc_io_tag_set(struct nvme_ctrl *ctrl, struct blk_mq_tag_set *set,
			  const struct blk_mq_ops *ops, unsigned int nr_maps,
			  unsigned int cmd_size)
{
	int ret;

	might_sleep();
	BUG_ON(!ops || cmd_size < sizeof(struct nvme_request));
	if (WARN_ON(nr_maps < 1 || nr_maps > HCTX_MAX_TYPES))
		return -EINVAL;
	if (ctrl->queue_count < 2)
		return -EINVAL;

	kmemset(set, 0, sizeof(*set));
	set->ops = ops;
	set->queue_depth = ctrl->sqsize < BLK_MQ_MAX_DEPTH - 1 ?
				   ctrl->sqsize :
				   BLK_MQ_MAX_DEPTH - 1;
	/* Some Apple controllers need tags unique across the admin and the
	 * (only) I/O queue: the first NVME_AQ_DEPTH stay unused. */
	if (ctrl->quirks & NVME_QUIRK_SHARED_TAGS)
		set->reserved_tags = NVME_AQ_DEPTH;
	set->numa_node = (int)ctrl->numa_node;
	if (ctrl->ops->flags & NVME_F_BLOCKING)
		set->flags |= BLK_MQ_F_BLOCKING;
	set->cmd_size = cmd_size;
	set->driver_data = ctrl;
	set->nr_hw_queues = ctrl->queue_count - 1;
	set->timeout_ms = nvme_core_param.io_timeout_s * 1000u;
	set->nr_maps = nr_maps;
	ret = blk_mq_alloc_tag_set(set);
	if (ret)
		return ret;
	ctrl->tagset = set;
	return 0;
}

void nvme_remove_io_tag_set(struct nvme_ctrl *ctrl)
{
	might_sleep();
	if (!ctrl->tagset)
		return;
	blk_mq_free_tag_set(ctrl->tagset);
	ctrl->tagset = NULL;
}

/* ==========================================================================
 * Per-request helpers for the transport's queue_rq
 * ========================================================================== */

static void nvme_clear_nvme_request(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);

	nr->status = 0;
	nr->retries = 0;
	nr->flags = 0;
	rq->flags |= BLK_REQ_DONTPREP;
}

void nvme_init_request(struct blk_request *rq, struct nvme_command *cmd)
{
	struct nvme_request *nr = nvme_req(rq);
	struct nvme_ns *ns = NULL;
	bool logging_enabled;

	/* The transport's init_request hooks the PDU up once per tag. */
	BUG_ON(!nr->cmd || !nr->ctrl);

	if (rq->queue)
		ns = blk_queue_get_queuedata(rq->queue);
	if (ns) {
		logging_enabled = ns->head && ns->head->passthru_err_log_enabled;
		rq->timeout_ms = nvme_core_param.io_timeout_s * 1000u;
	} else {
		/* No queuedata: the admin queue. */
		logging_enabled = nr->ctrl->passthru_err_log_enabled;
		rq->timeout_ms = nvme_core_param.admin_timeout_s * 1000u;
	}
	if (!logging_enabled)
		rq->flags |= BLK_REQ_QUIET;

	/* The transport chooses PRP or SGL itself. */
	cmd->common.flags &= (uint8_t)~NVME_CMD_SGL_ALL;

	rq->flags |= BLK_REQ_FAILFAST_DRIVER;
	if (rq->mq_hctx && rq->mq_hctx->type == HCTX_TYPE_POLL)
		rq->flags |= BLK_REQ_POLLED;
	nvme_clear_nvme_request(rq);
	kmemcpy(nr->cmd, cmd, sizeof(*cmd));
}

bool __nvme_check_ready(struct nvme_ctrl *ctrl, struct blk_request *rq,
			bool queue_live, enum nvme_ctrl_state state)
{
	struct nvme_request *req = nvme_req(rq);

	(void)state;
	/* A user's command on the admin queue of a controller that is not
	 * LIVE could overtake the initialisation sequence: busy it until
	 * the controller is back. */
	if (rq->queue == ctrl->admin_q && (req->flags & NVME_REQ_USERCMD))
		return false;
	/* (Fabrics would let connect/authentication through here.) */
	return queue_live;
}

blk_status_t nvme_fail_nonready_command(struct nvme_ctrl *ctrl,
					struct blk_request *rq)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);

	/* Requeue and retry once the controller recovered -- unless it is
	 * going away, fail-fast expired, the request wants no retries, or a
	 * multipath head can send it down another path. */
	if (state != NVME_CTRL_DELETING_NOIO && state != NVME_CTRL_DELETING &&
	    state != NVME_CTRL_DEAD &&
	    !nvme_test_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags) &&
	    !blk_noretry_request(rq) && !(rq->flags & BLK_REQ_MPATH))
		return BLK_STS_RESOURCE;

	if (!(rq->flags & BLK_REQ_DONTPREP))
		nvme_clear_nvme_request(rq);
	return nvme_host_path_error(rq);
}

void nvme_start_request(struct blk_request *rq)
{
	if (rq->flags & BLK_REQ_MPATH)
		nvme_mpath_start_request(rq);
	blk_mq_start_request(rq);
}

/* ==========================================================================
 * Cancelling what the controller can no longer complete
 * ========================================================================== */

int nvme_cancel_request(struct blk_request *rq, void *data)
{
	(void)data;
	/* Never a completed or idle request. */
	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT)
		return 1;

	nvme_req(rq)->status = NVME_SC_HOST_ABORTED_CMD;
	/* Atomic: a user cancel (NVME_REQ_HOST_CANCEL) may set its bit in
	 * the same byte at the same time. */
	__atomic_or_fetch(&nvme_req(rq)->flags, (uint8_t)NVME_REQ_CANCELLED,
			  __ATOMIC_RELEASE);
	blk_mq_complete_request(rq);
	return 1;
}

void nvme_cancel_tagset(struct nvme_ctrl *ctrl)
{
	might_sleep();
	if (ctrl->tagset) {
		blk_mq_tagset_busy_iter(ctrl->tagset, nvme_cancel_request,
					ctrl);
		blk_mq_tagset_wait_completed_request(ctrl->tagset);
	}
}

void nvme_cancel_admin_tagset(struct nvme_ctrl *ctrl)
{
	might_sleep();
	if (ctrl->admin_tagset) {
		blk_mq_tagset_busy_iter(ctrl->admin_tagset,
					nvme_cancel_request, ctrl);
		blk_mq_tagset_wait_completed_request(ctrl->admin_tagset);
	}
}

/* ==========================================================================
 * Synchronous submission
 * ========================================================================== */

int nvme_execute_rq(struct blk_request *rq, bool at_head)
{
	blk_status_t status;

	might_sleep();
	status = blk_execute_rq(rq, at_head ? 1 : 0);
	if (nvme_req(rq)->flags & NVME_REQ_CANCELLED)
		return -EINTR;
	if (nvme_req(rq)->status)
		return (int)nvme_req(rq)->status;
	return blk_status_to_errno(status);
}

int nvme_execute_user_rq(struct blk_request *rq)
{
	blk_status_t status;
	int rc;

	might_sleep();
	/* A polled request, or nobody to be killed: the plain wait. */
	if (blk_rq_is_poll(rq) || !blk_can_sleep() || !sched_current())
		return nvme_execute_rq(rq, false);

	blk_execute_rq_nowait(rq, 0);
	if (wait_event_killable(rq->wq, __atomic_load_n(&rq->done,
							__ATOMIC_ACQUIRE))) {
		/* Still queued: it never reaches the device.  Held by the
		 * device: the transport asks it to give the command up,
		 * instead of keeping a dying task for the command's full run
		 * (or its timeout).  Either way it still ends through its
		 * completion, which is waited for below. */
		(void)blk_mq_cancel_request(rq);
	}
	/* The completion (normal, cancelled, aborted, or from the timeout
	 * handling) is what frees the buffers: wait for it in any case. */
	rc = blk_wait(rq);
	status = errno_to_blk_status(rc);
	if (nvme_req(rq)->flags & NVME_REQ_CANCELLED)
		return -EINTR;
	if (nvme_req(rq)->status)
		return (int)nvme_req(rq)->status;
	return blk_status_to_errno(status);
}

int __nvme_submit_sync_cmd(struct blk_queue *q, struct nvme_command *cmd,
			   union nvme_result *result, void *buffer,
			   unsigned int bufflen, int qid,
			   unsigned int submit_flags)
{
	struct blk_request *rq;
	uint32_t blk_flags = 0;
	int err = 0;
	int ret;

	might_sleep();
	if (!q)
		return -ENODEV;
	if (submit_flags & NVME_SUBMIT_NOWAIT)
		blk_flags |= BLK_MQ_REQ_NOWAIT;
	if (submit_flags & NVME_SUBMIT_RESERVED)
		blk_flags |= BLK_MQ_REQ_RESERVED;
	if (qid == NVME_QID_ANY)
		rq = blk_mq_alloc_request(q, nvme_req_op(cmd), blk_flags, &err);
	else
		rq = blk_mq_alloc_request_hctx(q, nvme_req_op(cmd), blk_flags,
					       (unsigned int)(qid - 1), &err);
	if (!rq)
		return err ? err : -ENOMEM;

	nvme_init_request(rq, cmd);
	if (submit_flags & NVME_SUBMIT_RETRY)
		rq->flags &= ~(uint32_t)BLK_REQ_FAILFAST_DRIVER;

	if (buffer && bufflen) {
		ret = blk_rq_map_kern(rq, buffer, bufflen);
		if (ret)
			goto out;
	}

	ret = nvme_execute_rq(rq, (submit_flags & NVME_SUBMIT_AT_HEAD) != 0);
	if (result && ret >= 0)
		*result = nvme_req(rq)->result;
out:
	blk_mq_free_request(rq);
	return ret;
}

int nvme_submit_sync_cmd(struct blk_queue *q, struct nvme_command *cmd,
			 void *buffer, unsigned int bufflen)
{
	return __nvme_submit_sync_cmd(q, cmd, NULL, buffer, bufflen,
				      NVME_QID_ANY, 0);
}

/* ==========================================================================
 * Identify
 * ========================================================================== */

bool nvme_id_cns_ok(struct nvme_ctrl *ctrl, uint8_t cns)
{
	/* CNS is a full byte from 1.2 on. */
	if (ctrl->vs >= NVME_VS(1, 2, 0))
		return true;
	/* 1.1 made it two bits; values above would be truncated.  QEMU
	 * implements 1.0 behaviour while claiming 1.1: quirked. */
	if (ctrl->vs >= NVME_VS(1, 1, 0) &&
	    !(ctrl->quirks & NVME_QUIRK_IDENTIFY_CNS))
		return cns <= 3;
	/* 1.0: one bit. */
	return cns <= 1;
}

int nvme_identify(struct nvme_ctrl *ctrl, uint8_t cns, uint8_t csi,
		  uint32_t nsid, uint16_t cntid, void *buf)
{
	struct nvme_command c;

	might_sleep();
	if (!buf)
		return -EINVAL;
	kmemset(&c, 0, sizeof(c));
	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = cpu_to_le32(nsid);
	c.identify.cns = cns;
	c.identify.csi = csi;
	c.identify.ctrlid = cpu_to_le16(cntid);
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, buf,
				    NVME_IDENTIFY_DATA_SIZE);
}

int nvme_identify_ctrl(struct nvme_ctrl *ctrl, struct nvme_id_ctrl **id)
{
	int error;

	might_sleep();
	*id = kalloc(sizeof(struct nvme_id_ctrl));
	if (!*id)
		return -ENOMEM;
	error = nvme_identify(ctrl, NVME_ID_CNS_CTRL, 0, 0, 0, *id);
	if (error) {
		kfree(*id);
		*id = NULL;
	}
	return error;
}

int nvme_identify_ns(struct nvme_ctrl *ctrl, uint32_t nsid,
		     struct nvme_id_ns **id)
{
	int error;

	might_sleep();
	*id = kalloc(sizeof(struct nvme_id_ns));
	if (!*id)
		return -ENOMEM;
	error = nvme_identify(ctrl, NVME_ID_CNS_NS, 0, nsid, 0, *id);
	if (error) {
		kprintf(NVME_LOG_PREFIX
			"%s: Identify namespace %u failed (%d)\n",
			ctrl->name, nsid, error);
		kfree(*id);
		*id = NULL;
	}
	return error;
}

int nvme_identify_ns_cs_indep(struct nvme_ctrl *ctrl, uint32_t nsid,
			      struct nvme_id_ns_cs_indep **id)
{
	int error;

	might_sleep();
	*id = kalloc(sizeof(struct nvme_id_ns_cs_indep));
	if (!*id)
		return -ENOMEM;
	error = nvme_identify(ctrl, NVME_ID_CNS_NS_CS_INDEP, 0, nsid, 0, *id);
	if (error) {
		kfree(*id);
		*id = NULL;
	}
	return error;
}

int nvme_identify_active_ns_list(struct nvme_ctrl *ctrl, uint32_t prev,
				 __le32 *list)
{
	might_sleep();
	BUILD_BUG_ON(1024 * sizeof(__le32) != NVME_IDENTIFY_DATA_SIZE);
	return nvme_identify(ctrl, NVME_ID_CNS_NS_ACTIVE_LIST, 0, prev, 0,
			     list);
}

/* One Namespace Identification Descriptor: its payload length, or -1
 * when the controller reported a bogus length. */
static int nvme_process_ns_desc(struct nvme_ctrl *ctrl,
				struct nvme_ns_ids *ids,
				const struct nvme_ns_id_desc *cur,
				bool *csi_seen)
{
	const uint8_t *data = (const uint8_t *)cur + sizeof(*cur);

	switch (cur->nidt) {
	case NVME_NIDT_EUI64:
		if (cur->nidl != NVME_NIDT_EUI64_LEN)
			goto bogus;
		if (!(ctrl->quirks & NVME_QUIRK_BOGUS_NID))
			kmemcpy(ids->eui64, data, NVME_NIDT_EUI64_LEN);
		return NVME_NIDT_EUI64_LEN;
	case NVME_NIDT_NGUID:
		if (cur->nidl != NVME_NIDT_NGUID_LEN)
			goto bogus;
		if (!(ctrl->quirks & NVME_QUIRK_BOGUS_NID))
			kmemcpy(ids->nguid, data, NVME_NIDT_NGUID_LEN);
		return NVME_NIDT_NGUID_LEN;
	case NVME_NIDT_UUID:
		if (cur->nidl != NVME_NIDT_UUID_LEN)
			goto bogus;
		if (!(ctrl->quirks & NVME_QUIRK_BOGUS_NID))
			kmemcpy(ids->uuid, data, NVME_NIDT_UUID_LEN);
		return NVME_NIDT_UUID_LEN;
	case NVME_NIDT_CSI:
		if (cur->nidl != NVME_NIDT_CSI_LEN)
			goto bogus;
		ids->csi = data[0];
		*csi_seen = true;
		return NVME_NIDT_CSI_LEN;
	default:
		/* Unknown types are skipped. */
		return cur->nidl;
	}
bogus:
	kprintf(NVME_LOG_PREFIX
		"%s: controller returned bogus length %u for NIDT %u\n",
		ctrl->name, cur->nidl, cur->nidt);
	return -1;
}

int nvme_identify_ns_descs(struct nvme_ctrl *ctrl, struct nvme_ns_info *info)
{
	bool csi_seen = false;
	int status, pos, len;
	uint8_t *data;

	might_sleep();
	if (ctrl->vs < NVME_VS(1, 3, 0) && !nvme_multi_css(ctrl))
		return 0;
	if (ctrl->quirks & NVME_QUIRK_NO_NS_DESC_LIST)
		return 0;

	data = kcalloc(1, NVME_IDENTIFY_DATA_SIZE);
	if (!data)
		return -ENOMEM;
	status = nvme_identify(ctrl, NVME_ID_CNS_NS_DESC_LIST, 0, info->nsid, 0,
			       data);
	if (status) {
		kprintf(NVME_LOG_PREFIX
			"%s: Identify Descriptors failed (nsid=%u, "
			"status=0x%x)\n",
			ctrl->name, info->nsid, status);
		goto free_data;
	}

	for (pos = 0; pos + (int)sizeof(struct nvme_ns_id_desc) <=
		      NVME_IDENTIFY_DATA_SIZE;
	     pos += len) {
		const struct nvme_ns_id_desc *cur =
			(const struct nvme_ns_id_desc *)(data + pos);

		if (cur->nidl == 0)
			break;
		/* A descriptor running past the page is garbage. */
		if (pos + (int)sizeof(*cur) + cur->nidl >
		    NVME_IDENTIFY_DATA_SIZE)
			break;
		len = nvme_process_ns_desc(ctrl, &info->ids, cur, &csi_seen);
		if (len < 0)
			break;
		len += (int)sizeof(*cur);
	}

	if (nvme_multi_css(ctrl) && !csi_seen) {
		kprintf(NVME_LOG_PREFIX
			"%s: command set not reported for nsid %u\n",
			ctrl->name, info->nsid);
		status = -EINVAL;
	}
free_data:
	kfree(data);
	return status;
}

static bool nvme_is_io_ctrl(const struct nvme_ctrl *ctrl)
{
	/* No discovery controllers without fabrics: every controller that
	 * is not an administrative one does I/O. */
	return ctrl->cntrltype != NVME_CTRL_TYPE_ADMIN;
}

static uint32_t nvme_mps_units_to_sectors(struct nvme_ctrl *ctrl,
					  uint32_t units)
{
	uint32_t shift = units + (uint32_t)NVME_CAP_MPSMIN(ctrl->cap) + 12 - 9;

	return shift >= 32 ? 0xffffffffu : 1u << shift;
}

int nvme_init_non_mdts_limits(struct nvme_ctrl *ctrl)
{
	struct nvme_id_ctrl_nvm *id;
	int ret;

	might_sleep();
	/* MDTS does not apply to Write Zeroes, but the driver stays
	 * cautious and limits it like every other command. */
	if ((ctrl->oncs & NVME_CTRL_ONCS_WRITE_ZEROES) &&
	    !(ctrl->quirks & NVME_QUIRK_DISABLE_WRITE_ZEROES))
		ctrl->max_zeroes_sectors = ctrl->max_hw_sectors;
	else
		ctrl->max_zeroes_sectors = 0;

	if (!nvme_is_io_ctrl(ctrl) ||
	    !nvme_id_cns_ok(ctrl, NVME_ID_CNS_CS_CTRL) ||
	    nvme_test_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags))
		return 0;

	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl_nvm) != NVME_IDENTIFY_DATA_SIZE);
	id = kcalloc(1, sizeof(*id));
	if (!id)
		return -ENOMEM;
	ret = nvme_identify(ctrl, NVME_ID_CNS_CS_CTRL, NVME_CSI_NVM, 0, 0, id);
	if (ret)
		goto free_data;

	ctrl->dmrl = id->dmrl;
	ctrl->dmrsl = le32_to_cpu(id->dmrsl);
	if (id->wzsl)
		ctrl->max_zeroes_sectors =
			nvme_mps_units_to_sectors(ctrl, id->wzsl);
free_data:
	/* Not supported after all: never ask again. */
	if (ret > 0)
		nvme_set_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags);
	kfree(id);
	return ret;
}

/* ==========================================================================
 * Features
 * ========================================================================== */

static int nvme_features(struct nvme_ctrl *ctrl, uint8_t op, unsigned int fid,
			 unsigned int dword11, void *buffer, size_t buflen,
			 uint32_t *result)
{
	union nvme_result res;
	struct nvme_command c;
	int ret;

	might_sleep();
	kmemset(&c, 0, sizeof(c));
	res.u64 = 0;
	c.features.opcode = op;
	c.features.fid = cpu_to_le32(fid);
	c.features.dword11 = cpu_to_le32(dword11);
	ret = __nvme_submit_sync_cmd(ctrl->admin_q, &c, &res, buffer,
				     (unsigned int)buflen, NVME_QID_ANY, 0);
	if (ret >= 0 && result)
		*result = le32_to_cpu(res.u32);
	return ret;
}

int nvme_set_features(struct nvme_ctrl *ctrl, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      uint32_t *result)
{
	return nvme_features(ctrl, nvme_admin_set_features, fid, dword11,
			     buffer, buflen, result);
}

int nvme_get_features(struct nvme_ctrl *ctrl, unsigned int fid,
		      unsigned int dword11, void *buffer, size_t buflen,
		      uint32_t *result)
{
	return nvme_features(ctrl, nvme_admin_get_features, fid, dword11,
			     buffer, buflen, result);
}

/* ==========================================================================
 * Log pages
 * ========================================================================== */

static int nvme_get_log_one(struct nvme_ctrl *ctrl, uint32_t nsid,
			    uint8_t log_page, uint8_t lsp, uint16_t lsi,
			    uint8_t csi, void *log, size_t size,
			    uint64_t offset)
{
	struct nvme_command c;
	uint32_t dwlen = nvme_bytes_to_numd(size);

	kmemset(&c, 0, sizeof(c));
	c.get_log_page.opcode = nvme_admin_get_log_page;
	c.get_log_page.nsid = cpu_to_le32(nsid);
	c.get_log_page.lid = log_page;
	c.get_log_page.lsp = lsp;
	c.get_log_page.numdl = cpu_to_le16((uint16_t)(dwlen & 0xffff));
	c.get_log_page.numdu = cpu_to_le16((uint16_t)(dwlen >> 16));
	c.get_log_page.lpol = cpu_to_le32((uint32_t)offset);
	c.get_log_page.lpou = cpu_to_le32((uint32_t)(offset >> 32));
	c.get_log_page.csi = csi;
	c.get_log_page.lsi = cpu_to_le16(lsi);
	return nvme_submit_sync_cmd(ctrl->admin_q, &c, log,
				    (unsigned int)size);
}

int nvme_get_log_lsi(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t log_page,
		     uint8_t lsp, uint16_t lsi, uint8_t csi, void *log,
		     size_t size, uint64_t offset)
{
	uint64_t max_bytes;
	size_t done = 0, chunk;
	int ret = 0;

	might_sleep();
	/* NUMD counts dwords, LPO must be dword aligned. */
	if (WARN_ON(!size || (size & 3) || (offset & 3) || !log))
		return -EINVAL;

	/* A transfer is limited by MDTS like any other command; a log page
	 * larger than that is read in pieces with an advancing offset --
	 * possible only when the controller supports LPO/NUMDU (LPA bit
	 * 2).  Without it, one command (the controller may then cut the
	 * data short, as it would anyway). */
	max_bytes = (uint64_t)ctrl->max_hw_sectors << 9;
	if (!(ctrl->lpa & NVME_CTRL_LPA_EXTENDED_DATA) || !max_bytes ||
	    max_bytes >= size)
		return nvme_get_log_one(ctrl, nsid, log_page, lsp, lsi, csi,
					log, size, offset);
	chunk = (size_t)(max_bytes & ~(uint64_t)(NVME_CTRL_PAGE_SIZE - 1));
	if (!chunk)
		chunk = (size_t)(max_bytes & ~3ULL);
	if (!chunk)
		chunk = 4;

	while (done < size) {
		size_t n = size - done < chunk ? size - done : chunk;
		uint8_t this_lsp = lsp;

		/* Keep the event retained until the last piece is read. */
		if (done + n < size)
			this_lsp |= NVME_LOG_RAE;
		ret = nvme_get_log_one(ctrl, nsid, log_page, this_lsp, lsi,
				       csi, (uint8_t *)log + done, n,
				       offset + done);
		if (ret)
			break;
		done += n;
	}
	return ret;
}

int nvme_get_log(struct nvme_ctrl *ctrl, uint32_t nsid, uint8_t log_page,
		 uint8_t lsp, uint8_t csi, void *log, size_t size,
		 uint64_t offset)
{
	return nvme_get_log_lsi(ctrl, nsid, log_page, lsp, 0, csi, log, size,
				offset);
}

/* ==========================================================================
 * Abort
 * ========================================================================== */

int nvme_abort_cmd(struct nvme_ctrl *ctrl, uint16_t sqid, uint16_t cid)
{
	struct nvme_command c;
	union nvme_result res;
	int ret;

	might_sleep();
	/* ACL + 1 Aborts may be outstanding at once. */
	if (__atomic_sub_fetch(&ctrl->abort_limit, 1, __ATOMIC_ACQ_REL) < 0) {
		__atomic_add_fetch(&ctrl->abort_limit, 1, __ATOMIC_ACQ_REL);
		return -EBUSY;
	}

	kmemset(&c, 0, sizeof(c));
	res.u64 = 0;
	c.abort.opcode = nvme_admin_abort_cmd;
	c.abort.cid = cid; /* opaque to the controller, no byte swap */
	c.abort.sqid = cpu_to_le16(sqid);
	ret = __nvme_submit_sync_cmd(ctrl->admin_q, &c, &res, NULL, 0,
				     NVME_QID_ANY,
				     NVME_SUBMIT_AT_HEAD | NVME_SUBMIT_NOWAIT);
	__atomic_add_fetch(&ctrl->abort_limit, 1, __ATOMIC_ACQ_REL);
	if (!ret && (le32_to_cpu(res.u32) & 1))
		kprintf(NVME_LOG_PREFIX
			"%s: abort of qid %u cid %#x: command not aborted\n",
			ctrl->name, sqid, cid);
	return ret;
}

/* ==========================================================================
 * Command effects and pass-through bracketing
 * ========================================================================== */

/* Effects assumed when no log was read (before identify): the commands
 * that change namespaces or need exclusive access, as the established
 * defaults. */
static uint32_t nvme_default_admin_effects(uint8_t opcode)
{
	switch (opcode) {
	case nvme_admin_format_nvm:
		return NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC |
		       NVME_CMD_EFFECTS_NCC | NVME_CMD_EFFECTS_CSE_MASK;
	case nvme_admin_sanitize_nvm:
		return NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC |
		       NVME_CMD_EFFECTS_CSE_MASK;
	case nvme_admin_ns_mgmt:
	case nvme_admin_ns_attach:
		return NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_NIC |
		       NVME_CMD_EFFECTS_NCC;
	default:
		return 0;
	}
}

uint32_t nvme_command_effects(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			      uint8_t opcode)
{
	uint32_t effects = 0;

	if (ns) {
		const struct nvme_effects_log *log =
			ns->head && ns->head->effects ? ns->head->effects :
							ctrl->effects;

		if (log)
			effects = le32_to_cpu(log->iocs[opcode]);
		if (effects & ~(uint32_t)(NVME_CMD_EFFECTS_CSUPP |
					  NVME_CMD_EFFECTS_LBCC)) {
			static int warned;

			if (nvme_first_time(&warned)) {
				kprintf(NVME_LOG_PREFIX
					"%s: I/O command %02x has unusual "
					"effects %08x\n",
					ctrl->name, opcode, effects);
			}
		}
		/* Freezing every I/O queue for an I/O command would
		 * deadlock on itself. */
		effects &= ~(uint32_t)NVME_CMD_EFFECTS_CSE_MASK;
	} else {
		if (ctrl->effects)
			effects = le32_to_cpu(ctrl->effects->acs[opcode]);
		else
			effects = nvme_default_admin_effects(opcode);
		/* Relaxations override the execution restrictions. */
		if (effects & NVME_CMD_EFFECTS_CSER_MASK)
			effects &= ~(uint32_t)NVME_CMD_EFFECTS_CSE_MASK;
	}
	return effects;
}

uint32_t nvme_passthru_start(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
			     uint8_t opcode)
{
	uint32_t effects = nvme_command_effects(ctrl, ns, opcode);

	might_sleep();
	/* For simplicity every namespace's I/O stops even when the effects
	 * name only one namespace. */
	if (effects & NVME_CMD_EFFECTS_CSE_MASK) {
		mutex_lock(&ctrl->scan_lock);
		mutex_lock(&ctrl->subsys->lock);
		nvme_mpath_start_freeze(ctrl->subsys);
		nvme_mpath_wait_freeze(ctrl->subsys);
		nvme_start_freeze(ctrl);
		nvme_wait_freeze(ctrl);
	}
	return effects;
}

void nvme_passthru_end(struct nvme_ctrl *ctrl, struct nvme_ns *ns,
		       uint32_t effects, struct nvme_command *cmd, int status)
{
	might_sleep();
	if (effects & NVME_CMD_EFFECTS_CSE_MASK) {
		nvme_unfreeze(ctrl);
		nvme_mpath_unfreeze(ctrl->subsys);
		mutex_unlock(&ctrl->subsys->lock);
		mutex_unlock(&ctrl->scan_lock);
	}
	if (effects & NVME_CMD_EFFECTS_CCC) {
		if (!nvme_test_and_set_bit(NVME_CTRL_DIRTY_CAPABILITY,
					   &ctrl->flags))
			kprintf(NVME_LOG_PREFIX
				"%s: controller capabilities changed, reset "
				"may be required to take effect\n",
				ctrl->name);
	}
	if (effects & (NVME_CMD_EFFECTS_NIC | NVME_CMD_EFFECTS_NCC)) {
		nvme_queue_scan(ctrl);
		nvme_flush_work(&ctrl->scan_work);
	}
	if (ns || !cmd)
		return;

	switch (cmd->common.opcode) {
	case nvme_admin_set_features:
		switch (le32_to_cpu(cmd->common.cdw10) & 0xff) {
		case NVME_FEAT_KATO:
			/* The host's keep-alive period follows a KATO
			 * changed by Set Features. */
			if (!status)
				nvme_core_kato_changed(
					ctrl, le32_to_cpu(cmd->common.cdw11));
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}
