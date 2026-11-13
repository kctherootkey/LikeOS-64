// LikeOS - NVMe PCIe queues: rings, doorbells, completion processing, the blk-mq operations
//
// A queue pair is a submission ring (q_depth entries of 64 bytes, or 128
// for the Apple quirk) and a completion ring (16-byte entries) in host
// memory -- the SQ optionally in the Controller Memory Buffer -- plus two
// doorbells in BAR0.
//
//   SUBMISSION: queue_rq builds the command in the request's PDU (struct
//   nvme_iod), maps its data (nvme_pci_dma.c), copies the 64 bytes to the
//   SQ tail under sq_lock and rings the tail doorbell when the block layer
//   says the batch ended (bd->last), when the ring would otherwise fill up
//   before the next ring, or from commit_rqs.  queue_rqs takes a whole
//   plug list and rings once per queue.  With shadow doorbells (DBBUF,
//   emulated controllers) the new tail goes to the shadow page and the
//   MMIO write happens only when the controller's event index says it is
//   waiting for it.
//
//   COMPLETION: a CQE is new when its phase tag equals the queue's
//   current phase; the phase flips every time the head wraps.  The phase
//   is read first and the rest of the entry only after a read barrier.
//   Every consumed entry advances the head; the head doorbell is written
//   once per pass.  The command ID carries the tag and a 4-bit generation
//   counter, so a late completion of a reused tag is recognised and
//   dropped.  The asynchronous event requests have IDs above the block
//   layer's tags and go to the core directly.
//
//   ERROR HANDLING (nvme_timeout): a dead controller (fatal status,
//   NVM subsystem reset, surprise removal) is reset at once; otherwise the
//   CQ is polled (a lost interrupt); still outstanding, an I/O command is
//   aborted (at most abort_limit Aborts at a time, the request's timer
//   restarted), and a second timeout of the same command, an admin
//   timeout, or a timeout during bring-up disables the controller and
//   schedules the reset.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define NVME_PCI_Q_LOG NVME_LOG_PREFIX

static inline unsigned int sq_idx(unsigned int qid, uint32_t stride)
{
	return NVME_SQ_DB_IDX(qid, stride);
}

static inline unsigned int cq_idx(unsigned int qid, uint32_t stride)
{
	return NVME_CQ_DB_IDX(qid, stride);
}

static inline const char *nvme_opcode_str(uint16_t qid, uint8_t opcode)
{
	const char *s = qid ? nvme_get_opcode_str(opcode) :
			      nvme_get_admin_opcode_str(opcode);

	return s ? s : "unknown";
}

/* ==========================================================================
 * Shadow doorbells (DBBUF)
 * ========================================================================== */

static unsigned int nvme_dbbuf_size(struct nvme_dev *dev)
{
	return dev->nr_allocated_queues * 8u * dev->db_stride;
}

int nvme_dbbuf_dma_alloc(struct nvme_dev *dev)
{
	unsigned int mem_size = nvme_dbbuf_size(dev);

	might_sleep();
	if (!(dev->ctrl.oacs & NVME_CTRL_OACS_DBBUF_SUPP))
		return 0;

	if (dev->dbbuf_dbs) {
		/* The previous controller instance's values must not be
		 * observed by the new one. */
		BUG_ON(dev->dbbuf_size < mem_size);
		kmemset(dev->dbbuf_dbs, 0, dev->dbbuf_size);
		kmemset(dev->dbbuf_eis, 0, dev->dbbuf_size);
		return 0;
	}

	dev->dbbuf_dbs = nvme_dma_alloc_coherent(dev, mem_size, PAGE_SIZE,
						 &dev->dbbuf_dbs_dma_addr);
	if (!dev->dbbuf_dbs)
		goto fail;
	dev->dbbuf_eis = nvme_dma_alloc_coherent(dev, mem_size, PAGE_SIZE,
						 &dev->dbbuf_eis_dma_addr);
	if (!dev->dbbuf_eis)
		goto fail_free_dbbuf_dbs;
	dev->dbbuf_size = mem_size;
	return 0;

fail_free_dbbuf_dbs:
	nvme_dma_free_coherent(dev, mem_size, dev->dbbuf_dbs,
			       dev->dbbuf_dbs_dma_addr);
	dev->dbbuf_dbs = NULL;
fail:
	kprintf(NVME_PCI_Q_LOG "%s: unable to allocate dma for dbbuf\n",
		dev->ctrl.name);
	return -ENOMEM;
}

void nvme_dbbuf_dma_free(struct nvme_dev *dev)
{
	unsigned int mem_size = dev->dbbuf_size;

	if (dev->dbbuf_dbs) {
		nvme_dma_free_coherent(dev, mem_size, dev->dbbuf_dbs,
				       dev->dbbuf_dbs_dma_addr);
		dev->dbbuf_dbs = NULL;
	}
	if (dev->dbbuf_eis) {
		nvme_dma_free_coherent(dev, mem_size, dev->dbbuf_eis,
				       dev->dbbuf_eis_dma_addr);
		dev->dbbuf_eis = NULL;
	}
	dev->dbbuf_size = 0;
}

void nvme_dbbuf_init(struct nvme_dev *dev, struct nvme_queue *nvmeq, int qid)
{
	if (!dev->dbbuf_dbs || !qid)
		return;
	BUG_ON(cq_idx(qid, dev->db_stride) * sizeof(__le32) >=
	       dev->dbbuf_size);
	nvmeq->dbbuf_sq_db = &dev->dbbuf_dbs[sq_idx(qid, dev->db_stride)];
	nvmeq->dbbuf_cq_db = &dev->dbbuf_dbs[cq_idx(qid, dev->db_stride)];
	nvmeq->dbbuf_sq_ei = &dev->dbbuf_eis[sq_idx(qid, dev->db_stride)];
	nvmeq->dbbuf_cq_ei = &dev->dbbuf_eis[cq_idx(qid, dev->db_stride)];
}

void nvme_dbbuf_free(struct nvme_queue *nvmeq)
{
	if (!nvmeq->qid)
		return;
	nvmeq->dbbuf_sq_db = NULL;
	nvmeq->dbbuf_cq_db = NULL;
	nvmeq->dbbuf_sq_ei = NULL;
	nvmeq->dbbuf_cq_ei = NULL;
}

void nvme_dbbuf_set(struct nvme_dev *dev)
{
	struct nvme_command c;
	unsigned int i;

	might_sleep();
	if (!dev->dbbuf_dbs)
		return;
	kmemset(&c, 0, sizeof(c));
	c.dbbuf.opcode = nvme_admin_dbbuf;
	c.dbbuf.prp1 = cpu_to_le64(dev->dbbuf_dbs_dma_addr);
	c.dbbuf.prp2 = cpu_to_le64(dev->dbbuf_eis_dma_addr);

	if (nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0)) {
		kprintf(NVME_PCI_Q_LOG "%s: unable to set dbbuf\n",
			dev->ctrl.name);
		/* The queues stop using the pages before they go. */
		for (i = 1; i < dev->online_queues; i++)
			nvme_dbbuf_free(&dev->queues[i]);
		nvme_dbbuf_dma_free(dev);
	}
}

/* NVME_PCI_PURE_BEGIN(ring) ------------------------------------------------
 * The ring arithmetic: doorbells (MMIO and shadow), SQ tail, CQ head and
 * phase, CMB queue depth. */

/* Has the controller asked to be told about `new_idx' (its event index
 * lies in (old, new_idx])?  16-bit wrapping arithmetic. */
static inline int nvme_dbbuf_need_event(uint16_t event_idx, uint16_t new_idx,
					uint16_t old)
{
	return (uint16_t)(new_idx - event_idx - 1) < (uint16_t)(new_idx - old);
}

bool nvme_dbbuf_update_and_check_event(uint16_t value, __le32 *dbbuf_db,
				       volatile __le32 *dbbuf_ei)
{
	if (dbbuf_db) {
		uint16_t old_value, event_idx;

		/* The queue entries are in memory before the shadow
		 * doorbell says so. */
		wmb();

		old_value = (uint16_t)le32_to_cpu(*(volatile __le32 *)dbbuf_db);
		*(volatile __le32 *)dbbuf_db = cpu_to_le32(value);

		/* The doorbell is updated before the event index is read;
		 * the controller orders its side the other way round. */
		mb();

		event_idx = (uint16_t)le32_to_cpu(*dbbuf_ei);
		if (!nvme_dbbuf_need_event(event_idx, value, old_value))
			return false;
	}
	return true;
}

void nvme_write_sq_db(struct nvme_queue *nvmeq, bool write_sq)
{
	lockdep_assert_held(&nvmeq->sq_lock);
	if (!write_sq) {
		uint16_t next_tail = (uint16_t)(nvmeq->sq_tail + 1);

		if (next_tail == nvmeq->q_depth)
			next_tail = 0;
		/* Ring anyway when the next command would reach the tail
		 * the controller last heard of (a full ring). */
		if (next_tail != nvmeq->last_sq_tail)
			return;
	}

	if (nvme_dbbuf_update_and_check_event(nvmeq->sq_tail,
					      nvmeq->dbbuf_sq_db,
					      nvmeq->dbbuf_sq_ei)) {
		/* Commands in a write-combining CMB must have left the
		 * write-combining buffers before the doorbell. */
		if (nvme_test_bit(NVMEQ_SQ_CMB, &nvmeq->flags))
			wmb();
		writel(nvmeq->sq_tail, nvmeq->q_db);
	}
	nvmeq->last_sq_tail = nvmeq->sq_tail;
}

void nvme_sq_copy_cmd(struct nvme_queue *nvmeq, struct nvme_command *cmd)
{
	lockdep_assert_held(&nvmeq->sq_lock);
	BUG_ON(nvmeq->sq_tail >= nvmeq->q_depth);
	kmemcpy((uint8_t *)nvmeq->sq_cmds +
			((uint32_t)nvmeq->sq_tail << nvmeq->sqes),
		cmd, sizeof(*cmd));
	if (++nvmeq->sq_tail == nvmeq->q_depth)
		nvmeq->sq_tail = 0;
	nvmeq->nr_submitted++;
}

/* The phase of the head entry is read first: it says whether the rest of
 * the entry is valid. */
bool nvme_cqe_pending(struct nvme_queue *nvmeq)
{
	volatile struct nvme_completion *hcqe = &nvmeq->cqes[nvmeq->cq_head];

	return (le16_to_cpu(READ_ONCE(hcqe->status)) & 1) == nvmeq->cq_phase;
}

static inline void nvme_ring_cq_doorbell(struct nvme_queue *nvmeq)
{
	uint16_t head = nvmeq->cq_head;

	if (nvme_dbbuf_update_and_check_event(head, nvmeq->dbbuf_cq_db,
					      nvmeq->dbbuf_cq_ei))
		writel(head, nvmeq->q_db + nvmeq->dev->db_stride);
}

static inline void nvme_update_cq_head(struct nvme_queue *nvmeq)
{
	uint32_t tmp = (uint32_t)nvmeq->cq_head + 1;

	if (tmp == nvmeq->q_depth) {
		nvmeq->cq_head = 0;
		nvmeq->cq_phase ^= 1;
	} else {
		nvmeq->cq_head = (uint16_t)tmp;
	}
}

static void nvme_handle_cqe(struct nvme_queue *nvmeq,
			    struct io_comp_batch *iob, uint16_t idx);

int nvme_poll_cq(struct nvme_queue *nvmeq, struct io_comp_batch *iob)
{
	int found = 0;

	if (!nvmeq->cqes)
		return 0;
	BUG_ON(nvmeq->cq_head >= nvmeq->q_depth);
	while (nvme_cqe_pending(nvmeq)) {
		found++;
		/* Load-load ordering between the phase and the rest of the
		 * entry. */
		dma_rmb();
		nvme_handle_cqe(nvmeq, iob, nvmeq->cq_head);
		nvme_update_cq_head(nvmeq);
	}
	if (found) {
		nvmeq->nr_completed += (uint64_t)found;
		nvme_ring_cq_doorbell(nvmeq);
	}
	return found;
}

int nvme_cmb_qdepth(struct nvme_dev *dev, int nr_io_queues, int entry_size)
{
	int q_depth = (int)dev->q_depth;
	uint64_t q_size_aligned =
		((uint64_t)q_depth * (uint64_t)entry_size +
		 NVME_CTRL_PAGE_SIZE - 1) &
		~(uint64_t)(NVME_CTRL_PAGE_SIZE - 1);

	if (nr_io_queues <= 0)
		return -ENOMEM;
	if (q_size_aligned * (uint64_t)nr_io_queues > dev->cmb_size) {
		uint64_t mem_per_q = dev->cmb_size / (uint64_t)nr_io_queues;

		mem_per_q &= ~(uint64_t)(NVME_CTRL_PAGE_SIZE - 1);
		q_depth = (int)(mem_per_q / (uint64_t)entry_size);

		/* Below this, host memory with the full depth is better. */
		if (q_depth < NVME_CMB_MIN_QUEUE_DEPTH)
			return -ENOMEM;
	}
	return q_depth;
}

/* NVME_PCI_PURE_END(ring) ----------------------------------------------- */

/* ==========================================================================
 * Hardware contexts and requests
 * ========================================================================== */

static int nvme_init_hctx_common(struct blk_mq_hw_ctx *hctx, void *data,
				 unsigned int qid)
{
	struct nvme_dev *dev = to_nvme_dev(data);
	struct nvme_queue *nvmeq;
	struct blk_mq_tags *tags = NULL;
	int rc;

	BUG_ON(qid >= dev->nr_allocated_queues);
	nvmeq = &dev->queues[qid];
	if (qid && dev->tagset.tags)
		tags = dev->tagset.tags[qid - 1];
	else if (!qid && dev->admin_tagset.tags)
		tags = dev->admin_tagset.tags[0];
	WARN_ON(tags && hctx->tags && tags != hctx->tags);
	rc = nvme_setup_descriptor_pools(dev);
	if (rc)
		return rc;
	/* Every namespace queue's hctx comes here while the queue is live
	 * (its interrupt handler reads these): store only what changes. */
	if (nvmeq->dev != dev)
		nvmeq->dev = dev;
	if (nvmeq->descriptor_pools != &dev->descriptor_pools)
		nvmeq->descriptor_pools = &dev->descriptor_pools;
	hctx->driver_data = nvmeq;
	return 0;
}

static int nvme_admin_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
				unsigned int hctx_idx)
{
	WARN_ON(hctx_idx != 0);
	return nvme_init_hctx_common(hctx, data, 0);
}

static int nvme_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			  unsigned int hctx_idx)
{
	return nvme_init_hctx_common(hctx, data, hctx_idx + 1);
}

static int nvme_pci_init_request(struct blk_mq_tag_set *set, void *pdu,
				 unsigned int hctx_idx, unsigned int tag)
{
	struct nvme_iod *iod = pdu;

	(void)hctx_idx;
	(void)tag;
	iod->req.ctrl = set->driver_data;
	iod->req.cmd = &iod->cmd;
	return 0;
}

static void nvme_commit_rqs(struct blk_mq_hw_ctx *hctx)
{
	struct nvme_queue *nvmeq = hctx->driver_data;
	uint64_t flags;

	spin_lock_irqsave(&nvmeq->sq_lock, &flags);
	if (nvmeq->sq_tail != nvmeq->last_sq_tail)
		nvme_write_sq_db(nvmeq, true);
	spin_unlock_irqrestore(&nvmeq->sq_lock, flags);
}

/* Does the request carry data to map: its sg list, or a payload the
 * core built (special payload, e.g. the DSM range list)? */
static bool nvme_pci_rq_has_data(struct blk_request *rq)
{
	return blk_rq_nr_phys_segments(rq) ||
	       (nvme_req(rq)->flags & NVME_REQ_SPECIAL_PAYLOAD);
}

static void nvme_pci_unmap_rq(struct blk_request *rq)
{
	struct nvme_dev *dev = to_nvme_dev(nvme_req(rq)->ctrl);
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);

	/* The completion's status decides whether bounced read data is
	 * the device's (copied back) or garbage (dropped).  Only the status
	 * code type and code say that: the More, Command Retry Delay and Do
	 * Not Retry bits do not make a successful command's data any less
	 * the device's (nvme_error_status masks the same way). */
	iod->data_valid = (nvme_req(rq)->status & NVME_SCT_SC_MASK) ==
			  NVME_SC_SUCCESS;

	if (blk_integrity_rq(rq))
		nvme_unmap_metadata(dev, rq);
	if (nvme_pci_rq_has_data(rq))
		nvme_unmap_data(dev, rq);
}

void nvme_pci_complete_rq(struct blk_request *rq)
{
	nvme_pci_unmap_rq(rq);
	nvme_complete_rq(rq);
}

void nvme_pci_complete_batch(struct io_comp_batch *iob)
{
	nvme_complete_batch(iob, nvme_pci_unmap_rq);
}

static blk_status_t nvme_prep_rq(struct nvme_dev *dev, struct blk_request *rq)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);
	blk_status_t ret;

	iod->flags = 0;
	iod->nr_descriptors = 0;
	iod->data_valid = 0;
	iod->total_len = 0;
	iod->meta_total_len = 0;
	iod->nr_dma_vecs = 0;
	iod->dma_vecs = NULL;
	iod->first_dma = 0;
	iod->bounce = NULL;
	iod->bounce_len = 0;
	iod->meta_flags = 0;
	iod->meta_descriptor = NULL;
	iod->meta_bounce = NULL;

	ret = nvme_setup_cmd(rq->queue ? rq->queue->queuedata : NULL, rq);
	if (ret)
		return ret;

	if (nvme_pci_rq_has_data(rq)) {
		ret = nvme_map_data(dev, rq, &iod->cmd);
		if (ret)
			goto out_free_cmd;
	}

	if (blk_integrity_rq(rq)) {
		ret = nvme_map_metadata(dev, rq, &iod->cmd);
		if (ret)
			goto out_unmap_data;
	}

	nvme_start_request(rq);
	return BLK_STS_OK;
out_unmap_data:
	if (nvme_pci_rq_has_data(rq))
		nvme_unmap_data(dev, rq);
out_free_cmd:
	nvme_cleanup_cmd(rq);
	return ret;
}

static blk_status_t nvme_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct nvme_queue *nvmeq = hctx->driver_data;
	struct nvme_dev *dev = nvmeq->dev;
	struct blk_request *rq = bd->rq;
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);
	blk_status_t ret;
	uint64_t flags;

	/* Drains requests into a queue being torn down. */
	if (unlikely(!nvme_test_bit(NVMEQ_ENABLED, &nvmeq->flags)))
		return BLK_STS_IOERR;

	if (unlikely(!nvme_check_ready(&dev->ctrl, rq, true)))
		return nvme_fail_nonready_command(&dev->ctrl, rq);

	ret = nvme_prep_rq(dev, rq);
	if (unlikely(ret))
		return ret;
	/* Before the doorbell: afterwards the command may complete and its
	 * tag be reused before the trace reads it. */
	nvme_trace_submit(&dev->ctrl, nvmeq->qid, &iod->cmd);
	spin_lock_irqsave(&nvmeq->sq_lock, &flags);
	nvme_sq_copy_cmd(nvmeq, &iod->cmd);
	nvme_write_sq_db(nvmeq, bd->last != 0);
	spin_unlock_irqrestore(&nvmeq->sq_lock, flags);
	return BLK_STS_OK;
}

static struct blk_request *nvme_rq_list_pop(struct rq_list *l)
{
	struct blk_request *rq = l->head;

	if (rq) {
		l->head = rq->rq_next;
		if (!l->head)
			l->tail = NULL;
		rq->rq_next = NULL;
	}
	return rq;
}

static void nvme_rq_list_add_tail(struct rq_list *l, struct blk_request *rq)
{
	rq->rq_next = NULL;
	if (l->tail)
		l->tail->rq_next = rq;
	else
		l->head = rq;
	l->tail = rq;
}

static void nvme_submit_cmds(struct nvme_queue *nvmeq, struct rq_list *rqlist)
{
	struct blk_request *rq;
	uint64_t flags;

	if (rq_list_empty(rqlist))
		return;

	spin_lock_irqsave(&nvmeq->sq_lock, &flags);
	while ((rq = nvme_rq_list_pop(rqlist))) {
		struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);

		nvme_sq_copy_cmd(nvmeq, &iod->cmd);
	}
	nvme_write_sq_db(nvmeq, true);
	spin_unlock_irqrestore(&nvmeq->sq_lock, flags);
}

static bool nvme_prep_rq_batch(struct nvme_queue *nvmeq,
			       struct blk_request *rq)
{
	/* Drains requests into a queue being torn down. */
	if (unlikely(!nvme_test_bit(NVMEQ_ENABLED, &nvmeq->flags)))
		return false;
	if (unlikely(!nvme_check_ready(&nvmeq->dev->ctrl, rq, true)))
		return false;
	return nvme_prep_rq(nvmeq->dev, rq) == BLK_STS_OK;
}

static void nvme_queue_rqs(struct rq_list *rqlist)
{
	struct rq_list submit_list, requeue_list;
	struct nvme_queue *nvmeq = NULL;
	struct blk_request *rq;

	rq_list_init(&submit_list);
	rq_list_init(&requeue_list);
	while ((rq = nvme_rq_list_pop(rqlist))) {
		if (nvmeq && nvmeq != rq->mq_hctx->driver_data)
			nvme_submit_cmds(nvmeq, &submit_list);
		nvmeq = rq->mq_hctx->driver_data;

		if (nvme_prep_rq_batch(nvmeq, rq))
			nvme_rq_list_add_tail(&submit_list, rq);
		else
			nvme_rq_list_add_tail(&requeue_list, rq);
	}

	if (nvmeq)
		nvme_submit_cmds(nvmeq, &submit_list);
	*rqlist = requeue_list;
}

/* ==========================================================================
 * Completion processing
 * ========================================================================== */

static struct blk_mq_tags *nvme_queue_tagset(struct nvme_queue *nvmeq)
{
	struct nvme_dev *dev = nvmeq->dev;

	if (!nvmeq->qid)
		return dev->admin_tagset.tags ? dev->admin_tagset.tags[0] :
						NULL;
	if (!dev->tagset.tags || nvmeq->qid > dev->tagset.nr_hw_queues)
		return NULL;
	return dev->tagset.tags[nvmeq->qid - 1];
}

/* The request a command ID names (nvme_find_rq: the tag in flight with
 * the same generation -- a late completion of an aborted or cancelled
 * command that reused its tag is dropped), after a range check of the tag
 * against the queue's tag space. */
static struct blk_request *nvme_pci_find_rq(struct nvme_queue *nvmeq,
					    struct blk_mq_tags *tags,
					    uint16_t command_id)
{
	uint16_t tag = nvme_tag_from_cid(command_id);

	if (unlikely(tag >= tags->nr_tags)) {
		nvme_pci_warn_ratelimited(NVME_PCI_Q_LOG
					  "%s: tag %#x out of range on queue %u\n",
					  nvmeq->dev->ctrl.name, tag,
					  nvmeq->qid);
		return NULL;
	}
	return nvme_find_rq(tags, command_id);
}

static void nvme_handle_cqe(struct nvme_queue *nvmeq,
			    struct io_comp_batch *iob, uint16_t idx)
{
	volatile struct nvme_completion *hcqe = &nvmeq->cqes[idx];
	struct nvme_dev *dev = nvmeq->dev;
	struct nvme_completion cqe;
	struct blk_mq_tags *tags;
	struct blk_request *rq;
	union nvme_result result;

	cqe.result.u64 = hcqe->result.u64;
	cqe.sq_head = hcqe->sq_head;
	cqe.sq_id = hcqe->sq_id;
	cqe.command_id = READ_ONCE(hcqe->command_id);
	cqe.status = hcqe->status;
	/* Every consumed entry goes to the trace ring, AEN ones too. */
	nvme_trace_complete(&dev->ctrl, nvmeq->qid, &cqe);

	/* The asynchronous event requests never time out, survive every
	 * freeze and are not aborted: they have no request at all. */
	if (unlikely(nvme_is_aen_req(nvmeq->qid, cqe.command_id))) {
		nvme_complete_async_event(&dev->ctrl, cqe.status, &hcqe->result);
		return;
	}

	tags = nvme_queue_tagset(nvmeq);
	if (unlikely(!tags)) {
		WARN_RATELIMIT(1, NVME_PCI_Q_LOG
			       "%s: completion on queue %u without a tag set",
			       dev->ctrl.name, nvmeq->qid);
		return;
	}
	rq = nvme_pci_find_rq(nvmeq, tags, cqe.command_id);
	if (unlikely(!rq)) {
		nvme_pci_warn_ratelimited(NVME_PCI_Q_LOG
					  "%s: invalid id %d completed on queue %d\n",
					  dev->ctrl.name, cqe.command_id,
					  le16_to_cpu(cqe.sq_id));
		return;
	}

	/* Status and timeout injection happen in nvme_try_complete_req. */
	result.u64 = cqe.result.u64;
	if (!nvme_try_complete_req(rq, cqe.status, result) &&
	    !blk_mq_add_to_batch(rq, iob,
				 nvme_req(rq)->status != NVME_SC_SUCCESS,
				 nvme_pci_complete_batch))
		nvme_pci_complete_rq(rq);
}

void nvme_poll_irqdisable(struct nvme_queue *nvmeq)
{
	pci_device_t *pdev = nvmeq->dev->pdev;
	uint64_t flags;
	int irq;

	might_sleep();
	WARN_ON_ONCE(nvme_test_bit(NVMEQ_POLLED, &nvmeq->flags));

	irq = pci_irq_vector(pdev, nvmeq->cq_vector);
	if (irq >= 0)
		disable_irq(irq);
	spin_lock_irqsave(&nvmeq->cq_poll_lock, &flags);
	nvme_poll_cq(nvmeq, NULL);
	spin_unlock_irqrestore(&nvmeq->cq_poll_lock, flags);
	if (irq >= 0)
		enable_irq(irq);
}

static int nvme_poll(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob)
{
	struct nvme_queue *nvmeq = hctx->driver_data;
	uint64_t flags;
	int found;

	if (!nvme_test_bit(NVMEQ_POLLED, &nvmeq->flags) ||
	    !nvme_cqe_pending(nvmeq))
		return 0;

	spin_lock_irqsave(&nvmeq->cq_poll_lock, &flags);
	found = nvme_poll_cq(nvmeq, iob);
	spin_unlock_irqrestore(&nvmeq->cq_poll_lock, flags);
	return found;
}

void nvme_pci_submit_async_event(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);
	struct nvme_queue *nvmeq = &dev->queues[0];
	struct nvme_command c;
	uint64_t flags;

	kmemset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_async_event;
	c.common.command_id = NVME_AQ_BLK_MQ_DEPTH;

	spin_lock_irqsave(&nvmeq->sq_lock, &flags);
	if (nvmeq->sq_cmds && nvme_test_bit(NVMEQ_ENABLED, &nvmeq->flags)) {
		nvme_sq_copy_cmd(nvmeq, &c);
		nvme_write_sq_db(nvmeq, true);
	}
	spin_unlock_irqrestore(&nvmeq->sq_lock, flags);
}

/* ==========================================================================
 * Queue creation and deletion on the controller
 * ========================================================================== */

static int adapter_delete_queue(struct nvme_dev *dev, uint8_t opcode,
				uint16_t id)
{
	struct nvme_command c;

	kmemset(&c, 0, sizeof(c));
	c.delete_queue.opcode = opcode;
	c.delete_queue.qid = cpu_to_le16(id);
	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_alloc_cq(struct nvme_dev *dev, uint16_t qid,
			    struct nvme_queue *nvmeq, int16_t vector)
{
	struct nvme_command c;
	int flags = NVME_QUEUE_PHYS_CONTIG;

	if (!nvme_test_bit(NVMEQ_POLLED, &nvmeq->flags))
		flags |= NVME_CQ_IRQ_ENABLED;

	/* No data is attached: the PRP field survives as it is. */
	kmemset(&c, 0, sizeof(c));
	c.create_cq.opcode = nvme_admin_create_cq;
	c.create_cq.prp1 = cpu_to_le64(nvmeq->cq_dma_addr);
	c.create_cq.cqid = cpu_to_le16(qid);
	c.create_cq.qsize = cpu_to_le16((uint16_t)(nvmeq->q_depth - 1));
	c.create_cq.cq_flags = cpu_to_le16((uint16_t)flags);
	c.create_cq.irq_vector = cpu_to_le16((uint16_t)vector);
	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_alloc_sq(struct nvme_dev *dev, uint16_t qid,
			    struct nvme_queue *nvmeq)
{
	struct nvme_ctrl *ctrl = &dev->ctrl;
	struct nvme_command c;
	int flags = NVME_QUEUE_PHYS_CONTIG;

	/* Some drives turn weighted round robin with urgent priority on
	 * when MEDIUM is not set (urgent is zero): every queue urgent. */
	if (ctrl->quirks & NVME_QUIRK_MEDIUM_PRIO_SQ)
		flags |= NVME_SQ_PRIO_MEDIUM;

	kmemset(&c, 0, sizeof(c));
	c.create_sq.opcode = nvme_admin_create_sq;
	c.create_sq.prp1 = cpu_to_le64(nvmeq->sq_dma_addr);
	c.create_sq.sqid = cpu_to_le16(qid);
	c.create_sq.qsize = cpu_to_le16((uint16_t)(nvmeq->q_depth - 1));
	c.create_sq.sq_flags = cpu_to_le16((uint16_t)flags);
	c.create_sq.cqid = cpu_to_le16(qid);
	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_delete_cq(struct nvme_dev *dev, uint16_t cqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_cq, cqid);
}

static int adapter_delete_sq(struct nvme_dev *dev, uint16_t sqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_sq, sqid);
}

/* ==========================================================================
 * Timeout handling
 * ========================================================================== */

static void abort_endio(struct blk_request *rq, int status)
{
	struct nvme_dev *dev = to_nvme_dev(nvme_req(rq)->ctrl);

	(void)status;
	kprintf(NVME_PCI_Q_LOG "%s: Abort status: 0x%x\n", dev->ctrl.name,
		nvme_req(rq)->status);
	__atomic_add_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL);
	blk_mq_free_request(rq);
}

enum blk_eh_timer_return nvme_timeout(struct blk_request *rq)
{
	struct nvme_iod *iod = blk_mq_rq_to_pdu(rq);
	struct nvme_queue *nvmeq = rq->mq_hctx->driver_data;
	struct nvme_dev *dev = nvmeq->dev;
	pci_device_t *pdev = dev->pdev;
	struct blk_request *abort_req;
	struct nvme_command cmd;
	uint32_t csts;
	uint8_t opcode;
	int err = 0;

	might_sleep();
	csts = dev->bar ? readl((volatile uint8_t *)dev->bar + NVME_REG_CSTS) :
			  0xffffffffu;

	/* A device that is gone is shut down at once: it unblocks error
	 * recovery that waits for it, and removal cannot proceed while a
	 * reset waits for commands that will never complete. */
	if (pci_dev_is_disconnected(pdev))
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);
	if (nvme_state_terminal(&dev->ctrl))
		goto disable;

	/* Error recovery of the PCI core runs: a reset now would make it
	 * fail. */
	smp_mb();
	if (pci_channel_offline(pdev))
		return BLK_EH_RESET_TIMER;

	/* A failed controller is reset immediately. */
	if (nvme_should_reset(dev, csts)) {
		nvme_warn_reset(dev, csts);
		goto disable;
	}

	/* A lost interrupt? */
	if (nvme_test_bit(NVMEQ_POLLED, &nvmeq->flags))
		nvme_poll(rq->mq_hctx, NULL);
	else
		nvme_poll_irqdisable(nvmeq);

	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT) {
		kprintf(NVME_PCI_Q_LOG
			"%s: I/O tag %d (%04x) QID %d timeout, completion polled\n",
			dev->ctrl.name, rq->tag, nvme_cid(rq), nvmeq->qid);
		return BLK_EH_DONE;
	}

	/* A timeout while the controller comes up disables it at once; the
	 * reset work sees the forced cancellation.  Every outstanding
	 * request is completed by the disable. */
	switch (nvme_ctrl_state(&dev->ctrl)) {
	case NVME_CTRL_CONNECTING:
		nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_DELETING);
		/* fall through */
	case NVME_CTRL_DELETING:
		nvme_pci_warn_ratelimited(NVME_PCI_Q_LOG
					  "%s: I/O tag %d (%04x) QID %d timeout, disable controller\n",
					  dev->ctrl.name, rq->tag, nvme_cid(rq),
					  nvmeq->qid);
		/* Atomic: a user cancel may set NVME_REQ_HOST_CANCEL in
		 * the same byte concurrently. */
		__atomic_or_fetch(&nvme_req(rq)->flags,
				  (uint8_t)NVME_REQ_CANCELLED, __ATOMIC_RELEASE);
		nvme_dev_disable(dev, true);
		return BLK_EH_DONE;
	case NVME_CTRL_RESETTING:
		return BLK_EH_RESET_TIMER;
	default:
		break;
	}

	/* A second timeout of an aborted command, or any admin timeout:
	 * the controller is in a bad state -- disable and reset it. */
	opcode = nvme_req(rq)->cmd->common.opcode;
	if (!nvmeq->qid || (iod->flags & IOD_ABORTED)) {
		kprintf(NVME_PCI_Q_LOG
			"%s: I/O tag %d (%04x) opcode %#x (%s) QID %d timeout, reset controller\n",
			dev->ctrl.name, rq->tag, nvme_cid(rq), opcode,
			nvme_opcode_str(nvmeq->qid, opcode), nvmeq->qid);
		/* Atomic: a user cancel may set NVME_REQ_HOST_CANCEL in
		 * the same byte concurrently. */
		__atomic_or_fetch(&nvme_req(rq)->flags,
				  (uint8_t)NVME_REQ_CANCELLED, __ATOMIC_RELEASE);
		goto disable;
	}

	if (__atomic_sub_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL) <
	    0) {
		__atomic_add_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL);
		return BLK_EH_RESET_TIMER;
	}
	iod->flags |= IOD_ABORTED;

	kmemset(&cmd, 0, sizeof(cmd));
	cmd.abort.opcode = nvme_admin_abort_cmd;
	cmd.abort.cid = nvme_cid(rq);
	cmd.abort.sqid = cpu_to_le16(nvmeq->qid);

	kprintf(NVME_PCI_Q_LOG
		"%s: I/O tag %d (%04x) opcode %#x (%s) QID %d timeout, aborting req_op:%s(%u) size:%llu\n",
		dev->ctrl.name, rq->tag, nvme_cid(rq), opcode,
		nvme_opcode_str(nvmeq->qid, opcode), nvmeq->qid,
		blk_op_str(req_op(rq)), (unsigned)req_op(rq),
		(unsigned long long)blk_rq_bytes(rq));

	abort_req = blk_mq_alloc_request(dev->ctrl.admin_q, nvme_req_op(&cmd),
					 BLK_MQ_REQ_NOWAIT, &err);
	if (!abort_req) {
		__atomic_add_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL);
		return BLK_EH_RESET_TIMER;
	}
	nvme_init_request(abort_req, &cmd);
	abort_req->end_io = abort_endio;
	abort_req->end_io_data = NULL;
	blk_execute_rq_nowait(abort_req, 0);

	/* The aborted command completes when the Abort does; the timer runs
	 * again, and a second expiry resets the controller. */
	return BLK_EH_RESET_TIMER;

disable:
	if (!nvme_change_ctrl_state(&dev->ctrl, NVME_CTRL_RESETTING)) {
		if (nvme_state_terminal(&dev->ctrl))
			nvme_dev_disable(dev, true);
		return BLK_EH_DONE;
	}

	nvme_dev_disable(dev, false);
	if (nvme_try_sched_reset(&dev->ctrl))
		nvme_unquiesce_io_queues(&dev->ctrl);
	return BLK_EH_DONE;
}

/* ==========================================================================
 * Cancellation of a command the device holds (submitter gave up)
 * ========================================================================== */

static void nvme_cancel_endio(struct blk_request *rq, int status)
{
	struct nvme_dev *dev = to_nvme_dev(nvme_req(rq)->ctrl);

	(void)status;
	if (nvme_req(rq)->status)
		nvme_pci_warn_ratelimited(NVME_PCI_Q_LOG
					  "%s: Cancel status: 0x%x\n",
					  dev->ctrl.name, nvme_req(rq)->status);
	blk_mq_free_request(rq);
}

static void nvme_cancel_abort_endio(struct blk_request *rq, int status)
{
	struct nvme_dev *dev = to_nvme_dev(nvme_req(rq)->ctrl);

	(void)status;
	if (nvme_req(rq)->status)
		nvme_pci_warn_ratelimited(NVME_PCI_Q_LOG
					  "%s: Abort (cancel) status: 0x%x\n",
					  dev->ctrl.name, nvme_req(rq)->status);
	__atomic_add_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL);
	blk_mq_free_request(rq);
}

/*
 * NVMe 2.1 Cancel (I/O opcode 18h, Single Command Cancel) goes on the
 * submission queue of the command it names.  A controller whose effects
 * log does not list Cancel gets the admin Abort instead, within the
 * controller's Abort Command Limit (shared with the timeout handler).
 * The command then completes with the device's status, which is final
 * (NVME_REQ_HOST_CANCEL); a device that ignores the request still meets
 * the request's timeout.
 */
int nvme_pci_cancel_request(struct blk_request *rq)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	struct blk_request *creq;
	struct nvme_command cmd;
	struct nvme_queue *nvmeq;
	struct nvme_dev *dev;
	struct nvme_ns *ns;
	int err = 0;

	if (!hctx || !hctx->driver_data || !rq->queue)
		return -EBUSY;
	nvmeq = hctx->driver_data;
	dev = nvmeq->dev;
	/* Admin commands are never cancelled (Format, Sanitize, firmware
	 * commits must run to their end once started). */
	if (!nvmeq->qid)
		return -EOPNOTSUPP;
	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT ||
	    nvme_ctrl_state(&dev->ctrl) != NVME_CTRL_LIVE)
		return -EBUSY;
	ns = blk_queue_get_queuedata(rq->queue);

	kmemset(&cmd, 0, sizeof(cmd));
	if (ns && (nvme_command_effects(&dev->ctrl, ns, nvme_cmd_cancel) &
		   NVME_CMD_EFFECTS_CSUPP)) {
		cmd.cancel.opcode = nvme_cmd_cancel;
		cmd.cancel.nsid = nvme_req(rq)->cmd ?
					  nvme_req(rq)->cmd->common.nsid :
					  cpu_to_le32(ns->head->ns_id);
		cmd.cancel.sqid = cpu_to_le16(nvmeq->qid);
		cmd.cancel.cid = nvme_cid(rq);
		cmd.cancel.cdw11 = cpu_to_le32(NVME_CANCEL_ACODE_SINGLE);
		creq = blk_mq_alloc_request_hctx(rq->queue, nvme_req_op(&cmd),
						 BLK_MQ_REQ_NOWAIT,
						 hctx->queue_num, &err);
		if (!creq)
			return -EBUSY;
		__atomic_or_fetch(&nvme_req(rq)->flags,
				  (uint8_t)NVME_REQ_HOST_CANCEL,
				  __ATOMIC_RELEASE);
		nvme_init_request(creq, &cmd);
		creq->end_io = nvme_cancel_endio;
		creq->end_io_data = NULL;
		blk_execute_rq_nowait(creq, 1);
		return 0;
	}

	if (!dev->ctrl.admin_q)
		return -EBUSY;
	if (__atomic_sub_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL) <
	    0) {
		__atomic_add_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL);
		return -EBUSY;
	}
	cmd.abort.opcode = nvme_admin_abort_cmd;
	cmd.abort.cid = nvme_cid(rq);
	cmd.abort.sqid = cpu_to_le16(nvmeq->qid);
	creq = blk_mq_alloc_request(dev->ctrl.admin_q, nvme_req_op(&cmd),
				    BLK_MQ_REQ_NOWAIT, &err);
	if (!creq) {
		__atomic_add_fetch(&dev->ctrl.abort_limit, 1, __ATOMIC_ACQ_REL);
		return -EBUSY;
	}
	__atomic_or_fetch(&nvme_req(rq)->flags, (uint8_t)NVME_REQ_HOST_CANCEL,
			  __ATOMIC_RELEASE);
	nvme_init_request(creq, &cmd);
	creq->end_io = nvme_cancel_abort_endio;
	creq->end_io_data = NULL;
	blk_execute_rq_nowait(creq, 0);
	return 0;
}

/* ==========================================================================
 * Queue memory and life cycle
 * ========================================================================== */

static void nvme_free_queue(struct nvme_queue *nvmeq)
{
	struct nvme_dev *dev = nvmeq->dev;

	/* The controller must not be able to touch the rings any more. */
	BUG_ON(nvme_test_bit(NVMEQ_ENABLED, &nvmeq->flags));
	if (nvmeq->cqes)
		nvme_dma_free_coherent(dev, CQ_SIZE(nvmeq),
				       (void *)nvmeq->cqes,
				       nvmeq->cq_dma_addr);
	nvmeq->cqes = NULL;
	nvmeq->cq_dma_addr = 0;
	if (!nvmeq->sq_cmds)
		return;

	if (nvme_test_and_clear_bit(NVMEQ_SQ_CMB, &nvmeq->flags))
		nvme_cmb_free_sq(dev, nvmeq->cmb_offset, SQ_SIZE(nvmeq));
	else
		nvme_dma_free_coherent(dev, SQ_SIZE(nvmeq), nvmeq->sq_cmds,
				       nvmeq->sq_dma_addr);
	nvmeq->sq_cmds = NULL;
	nvmeq->sq_dma_addr = 0;
}

void nvme_free_queues(struct nvme_dev *dev, int lowest)
{
	int i;

	for (i = (int)dev->ctrl.queue_count - 1; i >= lowest; i--) {
		dev->ctrl.queue_count--;
		nvme_free_queue(&dev->queues[i]);
	}
}

void nvme_suspend_queue(struct nvme_dev *dev, unsigned int qid)
{
	struct nvme_queue *nvmeq = &dev->queues[qid];

	if (!nvme_test_and_clear_bit(NVMEQ_ENABLED, &nvmeq->flags))
		return;

	/* queue_rq sees NVMEQ_ENABLED cleared. */
	smp_mb();

	BUG_ON(dev->online_queues == 0);
	dev->online_queues--;
	if (!nvmeq->qid && dev->ctrl.admin_q)
		nvme_quiesce_admin_queue(&dev->ctrl);
	if (!nvme_test_and_clear_bit(NVMEQ_POLLED, &nvmeq->flags))
		nvme_pci_free_queue_irq(nvmeq);
}

void nvme_suspend_io_queues(struct nvme_dev *dev)
{
	int i;

	for (i = (int)dev->ctrl.queue_count - 1; i > 0; i--)
		nvme_suspend_queue(dev, (unsigned int)i);
}

/* Called only on a disabled controller once every other path that looks
 * at its CQs has synchronised (except poll): the last chance to see a
 * natural completion before the cancel. */
void nvme_reap_pending_cqes(struct nvme_dev *dev)
{
	uint64_t flags;
	int i;

	for (i = (int)dev->ctrl.queue_count - 1; i > 0; i--) {
		spin_lock_irqsave(&dev->queues[i].cq_poll_lock, &flags);
		nvme_poll_cq(&dev->queues[i], NULL);
		spin_unlock_irqrestore(&dev->queues[i].cq_poll_lock, flags);
	}
}

static int nvme_alloc_sq_cmds(struct nvme_dev *dev, struct nvme_queue *nvmeq,
			      int qid)
{
	if (qid && dev->cmb_use_sqes && (dev->cmbsz & NVME_CMBSZ_SQS)) {
		uint64_t bus = 0, off = 0;
		void *p = nvme_cmb_alloc_sq(dev, SQ_SIZE(nvmeq), &bus, &off);

		if (p) {
			nvmeq->sq_cmds = p;
			nvmeq->sq_dma_addr = bus;
			nvmeq->cmb_offset = off;
			nvme_set_bit(NVMEQ_SQ_CMB, &nvmeq->flags);
			return 0;
		}
	}

	nvmeq->sq_cmds = nvme_dma_alloc_coherent(dev, SQ_SIZE(nvmeq),
						 NVME_CTRL_PAGE_SIZE,
						 &nvmeq->sq_dma_addr);
	if (!nvmeq->sq_cmds)
		return -ENOMEM;
	return 0;
}

int nvme_pci_alloc_queue(struct nvme_dev *dev, int qid, int depth)
{
	struct nvme_queue *nvmeq;

	might_sleep();
	BUG_ON(qid < 0 || (unsigned int)qid >= dev->nr_allocated_queues);
	nvmeq = &dev->queues[qid];
	if (dev->ctrl.queue_count > (uint32_t)qid)
		return 0;
	if (WARN_ON(depth < NVME_PCI_MIN_QUEUE_SIZE ||
		    depth > NVME_MAX_TAGS_PER_QUEUE))
		return -EINVAL;

	nvmeq->flags = 0;
	nvmeq->sqes = (uint8_t)(qid ? dev->io_sqes : NVME_ADM_SQES);
	nvmeq->q_depth = (uint32_t)depth;
	nvmeq->dev = dev;
	nvmeq->cqes = nvme_dma_alloc_coherent(dev, CQ_SIZE(nvmeq),
					      NVME_CTRL_PAGE_SIZE,
					      &nvmeq->cq_dma_addr);
	if (!nvmeq->cqes)
		goto free_nvmeq;

	if (nvme_alloc_sq_cmds(dev, nvmeq, qid))
		goto free_cqdma;

	spinlock_init(&nvmeq->sq_lock, "nvmeq_sq");
	spinlock_init(&nvmeq->cq_poll_lock, "nvmeq_cq_poll");
	init_completion(&nvmeq->delete_done);
	nvmeq->cq_head = 0;
	nvmeq->cq_phase = 1;
	nvmeq->sq_tail = 0;
	nvmeq->last_sq_tail = 0;
	nvmeq->q_db = &dev->dbs[sq_idx((unsigned int)qid, dev->db_stride)];
	nvmeq->qid = (uint16_t)qid;
	nvmeq->irq = -1;
	if (!nvmeq->descriptor_pools)
		nvmeq->descriptor_pools = &dev->descriptor_pools;
	dev->ctrl.queue_count++;
	return 0;

free_cqdma:
	nvme_dma_free_coherent(dev, CQ_SIZE(nvmeq), (void *)nvmeq->cqes,
			       nvmeq->cq_dma_addr);
	nvmeq->cqes = NULL;
free_nvmeq:
	return -ENOMEM;
}

static void nvme_init_queue(struct nvme_queue *nvmeq, uint16_t qid)
{
	struct nvme_dev *dev = nvmeq->dev;

	nvmeq->sq_tail = 0;
	nvmeq->last_sq_tail = 0;
	nvmeq->cq_head = 0;
	nvmeq->cq_phase = 1;
	nvmeq->q_db = &dev->dbs[sq_idx(qid, dev->db_stride)];
	kmemset((void *)nvmeq->cqes, 0, CQ_SIZE(nvmeq));
	nvme_dbbuf_init(dev, nvmeq, qid);
	dev->online_queues++;
	/* The first interrupt sees the initialisation. */
	wmb();
}

/* Take shutdown_lock for the queue setup unless a disable holds it or
 * the controller left CONNECTING. */
static int nvme_setup_io_queues_trylock(struct nvme_dev *dev)
{
	if (!mutex_trylock(&dev->ctrl.shutdown_lock))
		return -ENODEV;
	if (nvme_ctrl_state(&dev->ctrl) != NVME_CTRL_CONNECTING) {
		mutex_unlock(&dev->ctrl.shutdown_lock);
		return -ENODEV;
	}
	return 0;
}

static int nvme_create_queue(struct nvme_queue *nvmeq, int qid, bool polled)
{
	struct nvme_dev *dev = nvmeq->dev;
	int result;
	uint16_t vector = 0;

	might_sleep();
	nvme_clear_bit(NVMEQ_DELETE_ERROR, &nvmeq->flags);

	/* The queue's vector is its qid, unless there is only one. */
	if (!polled)
		vector = dev->num_vecs == 1 ? 0 : (uint16_t)qid;
	else
		nvme_set_bit(NVMEQ_POLLED, &nvmeq->flags);

	result = adapter_alloc_cq(dev, (uint16_t)qid, nvmeq, (int16_t)vector);
	if (result)
		return result;

	result = adapter_alloc_sq(dev, (uint16_t)qid, nvmeq);
	if (result < 0)
		return result;
	if (result)
		goto release_cq;

	nvmeq->cq_vector = vector;

	result = nvme_setup_io_queues_trylock(dev);
	if (result)
		return result;
	nvme_init_queue(nvmeq, (uint16_t)qid);
	if (!polled) {
		result = nvme_pci_request_queue_irq(nvmeq);
		if (result < 0)
			goto release_sq;
	}

	nvme_set_bit(NVMEQ_ENABLED, &nvmeq->flags);
	mutex_unlock(&dev->ctrl.shutdown_lock);
	return result;

release_sq:
	dev->online_queues--;
	mutex_unlock(&dev->ctrl.shutdown_lock);
	adapter_delete_sq(dev, (uint16_t)qid);
release_cq:
	adapter_delete_cq(dev, (uint16_t)qid);
	return result;
}

const struct blk_mq_ops nvme_mq_admin_ops = {
	.queue_rq = nvme_queue_rq,
	.complete = nvme_pci_complete_rq,
	.init_hctx = nvme_admin_init_hctx,
	.init_request = nvme_pci_init_request,
	.timeout = nvme_timeout,
};

const struct blk_mq_ops nvme_mq_ops = {
	.queue_rq = nvme_queue_rq,
	.queue_rqs = nvme_queue_rqs,
	.complete = nvme_pci_complete_rq,
	.commit_rqs = nvme_commit_rqs,
	.init_hctx = nvme_init_hctx,
	.init_request = nvme_pci_init_request,
	.map_queues = nvme_pci_map_queues,
	.timeout = nvme_timeout,
	.poll = nvme_poll,
	.cancel = nvme_pci_cancel_request,
};

void nvme_dev_remove_admin(struct nvme_dev *dev)
{
	if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q)) {
		/* After a reset during removal user requests may wait on a
		 * stopped queue: start it so they are flushed. */
		nvme_unquiesce_admin_queue(&dev->ctrl);
		nvme_remove_admin_tag_set(&dev->ctrl);
	}
}

int nvme_pci_configure_admin_queue(struct nvme_dev *dev)
{
	struct nvme_queue *nvmeq;
	volatile uint8_t *bar;
	uint32_t aqa;
	int result;

	might_sleep();
	result = nvme_remap_bar(dev, nvme_db_bar_size(dev, 0));
	if (result < 0)
		return result;
	bar = dev->bar;

	dev->subsystem = readl(bar + NVME_REG_VS) >= NVME_VS(1, 1, 0) ?
				 NVME_CAP_NSSRC(dev->ctrl.cap) :
				 0;
	dev->ctrl.subsystem = dev->subsystem;

	if (dev->subsystem && (readl(bar + NVME_REG_CSTS) & NVME_CSTS_NSSRO))
		writel(NVME_CSTS_NSSRO, bar + NVME_REG_CSTS);

	/* A controller handed over enabled (firmware boot driver): clear
	 * CC.EN only.  A shutdown notification could make it complete
	 * commands into an admin queue whose memory we know nothing
	 * about. */
	result = nvme_disable_ctrl(&dev->ctrl, false);
	if (result < 0) {
		/* CSTS.RDY never followed: something is stuck.  The bigger
		 * hammer, a Function Level Reset, then try again. */
		result = pcie_reset_flr(dev->pdev, 0);
		if (result < 0)
			return result;

		pci_restore_state(dev->pdev);
		result = nvme_disable_ctrl(&dev->ctrl, false);
		if (result < 0)
			return result;

		kprintf(NVME_PCI_Q_LOG
			"%s: controller reset completed after pcie flr\n",
			dev->ctrl.name);
	}

	result = nvme_pci_alloc_queue(dev, 0, NVME_AQ_DEPTH);
	if (result)
		return result;

	dev->ctrl.numa_node = 0;

	nvmeq = &dev->queues[0];
	aqa = nvmeq->q_depth - 1;
	aqa |= aqa << 16;

	writel(aqa, bar + NVME_REG_AQA);
	lo_hi_writeq(nvmeq->sq_dma_addr, bar + NVME_REG_ASQ);
	lo_hi_writeq(nvmeq->cq_dma_addr, bar + NVME_REG_ACQ);

	result = nvme_enable_ctrl(&dev->ctrl);
	if (result)
		return result;

	nvmeq->cq_vector = 0;
	nvme_init_queue(nvmeq, 0);
	result = nvme_pci_request_queue_irq(nvmeq);
	if (result) {
		dev->online_queues--;
		return result;
	}

	nvme_set_bit(NVMEQ_ENABLED, &nvmeq->flags);
	return result;
}

int nvme_create_io_queues(struct nvme_dev *dev)
{
	unsigned int i, max, rw_queues;
	int ret = 0;

	might_sleep();
	for (i = dev->ctrl.queue_count; i <= dev->max_qid; i++) {
		if (nvme_pci_alloc_queue(dev, (int)i, (int)dev->q_depth)) {
			ret = -ENOMEM;
			break;
		}
	}

	max = dev->max_qid < dev->ctrl.queue_count - 1 ?
		      dev->max_qid :
		      dev->ctrl.queue_count - 1;
	if (max != 1 && dev->io_queues[HCTX_TYPE_POLL]) {
		rw_queues = dev->io_queues[HCTX_TYPE_DEFAULT] +
			    dev->io_queues[HCTX_TYPE_READ];
	} else {
		rw_queues = max;
	}

	for (i = dev->online_queues; i <= max; i++) {
		bool polled = i > rw_queues;

		ret = nvme_create_queue(&dev->queues[i], (int)i, polled);
		if (ret)
			break;
	}

	/* Failing Create SQ/CQ commands are ignored: fewer queues work,
	 * and a controller without I/O queues still takes admin commands
	 * (a firmware update for a buggy one). */
	return ret >= 0 ? 0 : ret;
}

unsigned int nvme_max_io_queues(struct nvme_dev *dev)
{
	/* Admin and I/O tags shared (Apple): one I/O queue only. */
	unsigned int possible;

	if (dev->ctrl.quirks & NVME_QUIRK_SHARED_TAGS)
		return 1;
	possible = blk_mq_num_possible_queues(0);
	if (WARN_ON_ONCE(possible == 0))
		possible = 1;
	return possible + dev->nr_write_queues + dev->nr_poll_queues;
}

int nvme_setup_io_queues(struct nvme_dev *dev)
{
	struct nvme_queue *adminq = &dev->queues[0];
	pci_device_t *pdev = dev->pdev;
	unsigned int nr_io_queues;
	unsigned long size;
	int result, count;

	might_sleep();
	/* The knobs are sampled once per reset. */
	dev->nr_write_queues = nvme_pci_param.write_queues;
	dev->nr_poll_queues = nvme_pci_param.poll_queues;

	if (dev->ctrl.tagset) {
		/* The tag set's maps exist only as allocated at first:
		 * special queues cannot be added later. */
		if (dev->ctrl.tagset->nr_maps < 3)
			dev->nr_poll_queues = 0;
		if (dev->ctrl.tagset->nr_maps < 2)
			dev->nr_write_queues = 0;
	}

	/* The allocated queue slots may exceed what the knobs ask for
	 * now. */
	nr_io_queues = nvme_max_io_queues(dev);
	if (nr_io_queues > dev->nr_allocated_queues - 1)
		nr_io_queues = dev->nr_allocated_queues - 1;
	count = (int)nr_io_queues;
	result = nvme_set_queue_count(&dev->ctrl, &count);
	if (result < 0)
		return result;
	nr_io_queues = count > 0 ? (unsigned int)count : 0;
	if (nr_io_queues > dev->nr_allocated_queues - 1)
		nr_io_queues = dev->nr_allocated_queues - 1;

	if (nr_io_queues == 0)
		return 0;

	/* The interrupts go as soon as NVMEQ_ENABLED is cleared, and under
	 * the lock nvme_dev_disable frees the vectors with. */
	result = nvme_setup_io_queues_trylock(dev);
	if (result)
		return result;
	if (nvme_test_and_clear_bit(NVMEQ_ENABLED, &adminq->flags))
		nvme_pci_free_queue_irq(adminq);

	if (dev->cmb_use_sqes) {
		result = nvme_cmb_qdepth(dev, (int)nr_io_queues,
					 (int)sizeof(struct nvme_command));
		if (result > 0) {
			dev->q_depth = (uint32_t)result;
			dev->ctrl.sqsize = (uint16_t)(result - 1);
		} else {
			dev->cmb_use_sqes = false;
		}
	}

	for (;;) {
		size = nvme_db_bar_size(dev, nr_io_queues);
		result = nvme_remap_bar(dev, size);
		if (!result)
			break;
		if (!--nr_io_queues) {
			result = -ENOMEM;
			goto out_unlock;
		}
	}
	adminq->q_db = dev->dbs;

retry:
	/* The admin queue's interrupt goes before the vectors do. */
	if (nvme_test_and_clear_bit(NVMEQ_ENABLED, &adminq->flags))
		nvme_pci_free_queue_irq(adminq);

	/* The single vector taken for the bring-up goes before the full
	 * range is allocated. */
	pci_free_irq_vectors(pdev);

	result = nvme_setup_irqs(dev, nr_io_queues);
	if (result <= 0) {
		result = -EIO;
		goto out_unlock;
	}

	dev->num_vecs = (unsigned int)result;
	result = result - 1 > 1 ? result - 1 : 1;
	dev->max_qid = (unsigned int)result + dev->io_queues[HCTX_TYPE_POLL];

	result = nvme_pci_request_queue_irq(adminq);
	if (result)
		goto out_unlock;
	nvme_set_bit(NVMEQ_ENABLED, &adminq->flags);
	mutex_unlock(&dev->ctrl.shutdown_lock);

	result = nvme_create_io_queues(dev);
	if (result || dev->online_queues < 2)
		return result;

	if (dev->online_queues - 1 < dev->max_qid) {
		/* Fewer queues came up than vectors were spread for: start
		 * over with that many so every queue has its vector. */
		nr_io_queues = dev->online_queues - 1;
		nvme_delete_io_queues(dev);
		result = nvme_setup_io_queues_trylock(dev);
		if (result)
			return result;
		nvme_suspend_io_queues(dev);
		goto retry;
	}
	kprintf(NVME_PCI_Q_LOG "%s: %u/%u/%u default/read/poll queues\n",
		dev->ctrl.name, dev->io_queues[HCTX_TYPE_DEFAULT],
		dev->io_queues[HCTX_TYPE_READ], dev->io_queues[HCTX_TYPE_POLL]);
	return 0;
out_unlock:
	mutex_unlock(&dev->ctrl.shutdown_lock);
	return result;
}

static void nvme_del_queue_end(struct blk_request *rq, int status)
{
	struct nvme_queue *nvmeq = rq->end_io_data;

	(void)status;
	blk_mq_free_request(rq);
	complete(&nvmeq->delete_done);
}

static void nvme_del_cq_end(struct blk_request *rq, int status)
{
	struct nvme_queue *nvmeq = rq->end_io_data;

	if (status)
		nvme_set_bit(NVMEQ_DELETE_ERROR, &nvmeq->flags);
	nvme_del_queue_end(rq, status);
}

static int nvme_delete_queue(struct nvme_queue *nvmeq, uint8_t opcode)
{
	struct blk_queue *q = nvmeq->dev->ctrl.admin_q;
	struct blk_request *rq;
	struct nvme_command cmd;
	int err = 0;

	if (!q)
		return -ENODEV;
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.delete_queue.opcode = opcode;
	cmd.delete_queue.qid = cpu_to_le16(nvmeq->qid);

	rq = blk_mq_alloc_request(q, nvme_req_op(&cmd), BLK_MQ_REQ_NOWAIT,
				  &err);
	if (!rq)
		return err ? err : -ENOMEM;
	nvme_init_request(rq, &cmd);

	if (opcode == nvme_admin_delete_cq)
		rq->end_io = nvme_del_cq_end;
	else
		rq->end_io = nvme_del_queue_end;
	rq->end_io_data = nvmeq;

	init_completion(&nvmeq->delete_done);
	blk_execute_rq_nowait(rq, 0);
	return 0;
}

/* Delete the I/O SQs (then CQs) in parallel batches, as many at once as
 * the admin tags allow, waiting up to the admin timeout in all. */
static bool __nvme_delete_io_queues(struct nvme_dev *dev, uint8_t opcode)
{
	int nr_queues = (int)dev->online_queues - 1, sent = 0;
	long timeout;

retry:
	timeout = (long)nvme_pci_admin_timeout_ms();
	while (nr_queues > 0) {
		if (nvme_delete_queue(&dev->queues[nr_queues], opcode))
			break;
		nr_queues--;
		sent++;
	}
	while (sent) {
		struct nvme_queue *nvmeq = &dev->queues[nr_queues + sent];

		timeout = wait_for_completion_io_timeout(&nvmeq->delete_done,
							 timeout);
		if (timeout == 0)
			return false;

		sent--;
		if (nr_queues)
			goto retry;
	}
	return true;
}

void nvme_delete_io_queues(struct nvme_dev *dev)
{
	might_sleep();
	if (__nvme_delete_io_queues(dev, nvme_admin_delete_sq))
		__nvme_delete_io_queues(dev, nvme_admin_delete_cq);
}

unsigned int nvme_pci_nr_maps(struct nvme_dev *dev)
{
	if (dev->io_queues[HCTX_TYPE_POLL])
		return 3;
	if (dev->io_queues[HCTX_TYPE_READ])
		return 2;
	return 1;
}

void nvme_free_tagset(struct nvme_dev *dev)
{
	if (dev->tagset.tags)
		nvme_remove_io_tag_set(&dev->ctrl);
	dev->ctrl.tagset = NULL;
}

bool nvme_pci_update_nr_queues(struct nvme_dev *dev)
{
	might_sleep();
	if (!dev->ctrl.tagset) {
		int rc = nvme_alloc_io_tag_set(&dev->ctrl, &dev->tagset,
					       &nvme_mq_ops,
					       nvme_pci_nr_maps(dev),
					       sizeof(struct nvme_iod));

		if (rc)
			kprintf(NVME_PCI_Q_LOG
				"%s: I/O tag set allocation failed (%d)\n",
				dev->ctrl.name, rc);
		return true;
	}

	/* Racing a disable: give up. */
	if (!mutex_trylock(&dev->ctrl.shutdown_lock))
		return false;

	/* The disable already ran. */
	if (!dev->online_queues) {
		mutex_unlock(&dev->ctrl.shutdown_lock);
		return false;
	}

	blk_mq_update_nr_hw_queues(&dev->tagset, (int)dev->online_queues - 1);
	/* Queues no longer used go. */
	nvme_free_queues(dev, (int)dev->online_queues);
	mutex_unlock(&dev->ctrl.shutdown_lock);
	return true;
}
