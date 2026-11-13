// LikeOS - NVMe PCIe interrupts: vector allocation with affinity, the queue interrupt handler
//
// Every completion queue that is not polled has an interrupt vector; the
// admin queue has vector 0, I/O queue n has vector n.  The vectors are
// allocated in one go after the controller told how many I/O queues it
// grants (Set Features Number of Queues): one PRE-VECTOR for the admin
// queue with the default affinity, the rest spread over the CPUs in up to
// two sets -- default (read+write) queues and, when nvme.write_queues asks
// for separate write queues, read queues -- each set covering every CPU
// (managed affinity, group_cpus_evenly).  The block layer then maps each
// CPU to the hardware queue whose interrupt that CPU takes
// (blk_mq_map_hw_queues), so a request completes on the CPU or core that
// submitted it.  Poll queues (nvme.poll_queues) have no vector at all.
//
// The PCI core tries MSI-X, then MSI (a power of two, one target CPU),
// then the INTx line; with a single vector every queue shares it and each
// handler looks at its own CQ only (the line is registered shared).
// Controllers whose MSI is broken skip MSI; some Apple controllers need
// all queues on the first vector.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/io/console.h>

#define NVME_PCI_IRQ_LOG NVME_LOG_PREFIX

/* NVME_PCI_PURE_BEGIN(irq) -------------------------------------------------
 * Vector set arithmetic. */

/* `nrirqs' vectors are left for the read and write queues (the admin
 * queue's pre-vector is already taken): split them into the default set
 * and, with nvme.write_queues, a read set that keeps at least one
 * vector. */
void nvme_calc_irq_sets(struct irq_affinity *affd, unsigned int nrirqs)
{
	struct nvme_dev *dev = affd->priv;
	unsigned int nr_read_queues, nr_write_queues = dev->nr_write_queues;

	/* No vector for the queues: one default queue (its set size of one
	 * is ignored by the spreading for this case).  One vector, or no
	 * separate write queues: read and write share.  Separate write
	 * queues leave room for at least one read queue. */
	if (!nrirqs) {
		nrirqs = 1;
		nr_read_queues = 0;
	} else if (nrirqs == 1 || !nr_write_queues) {
		nr_read_queues = 0;
	} else if (nr_write_queues >= nrirqs) {
		nr_read_queues = 1;
	} else {
		nr_read_queues = nrirqs - nr_write_queues;
	}

	dev->io_queues[HCTX_TYPE_DEFAULT] = nrirqs - nr_read_queues;
	affd->set_size[HCTX_TYPE_DEFAULT] = nrirqs - nr_read_queues;
	dev->io_queues[HCTX_TYPE_READ] = nr_read_queues;
	affd->set_size[HCTX_TYPE_READ] = nr_read_queues;
	affd->nr_sets = nr_read_queues ? 2 : 1;
}

/* How many vectors to ask for and the poll queue count, for nr_io_queues
 * granted I/O queues. */
static unsigned int nvme_irq_queue_count(struct nvme_dev *dev,
					 unsigned int nr_io_queues,
					 unsigned int *poll_out)
{
	unsigned int poll_queues, irq_queues;

	/* Poll queues need no interrupt, but at least one I/O queue stays
	 * interrupt driven. */
	poll_queues = dev->nr_poll_queues;
	if (nr_io_queues == 0)
		poll_queues = 0;
	else if (poll_queues > nr_io_queues - 1)
		poll_queues = nr_io_queues - 1;
	*poll_out = poll_queues;

	/* The admin queue and every interrupt driven I/O queue, unless the
	 * controller wants them all on the first vector. */
	irq_queues = 1;
	if (!(dev->ctrl.quirks & NVME_QUIRK_SINGLE_VECTOR))
		irq_queues += nr_io_queues - poll_queues;
	return irq_queues;
}

int nvme_queue_irq_offset(struct nvme_dev *dev)
{
	/* With more than one vector the admin queue has its own. */
	if (dev->num_vecs > 1)
		return 1;
	return 0;
}

/* NVME_PCI_PURE_END(irq) ------------------------------------------------- */

int nvme_setup_irqs(struct nvme_dev *dev, unsigned int nr_io_queues)
{
	pci_device_t *pdev = dev->pdev;
	struct irq_affinity affd;
	unsigned int irq_queues, poll_queues;
	unsigned int flags = PCI_IRQ_ALL_TYPES | PCI_IRQ_AFFINITY;

	might_sleep();
	kmemset(&affd, 0, sizeof(affd));
	affd.pre_vectors = 1;
	affd.calc_sets = nvme_calc_irq_sets;
	affd.priv = dev;

	irq_queues = nvme_irq_queue_count(dev, nr_io_queues, &poll_queues);
	dev->io_queues[HCTX_TYPE_POLL] = poll_queues;

	/* The single interrupt case; nvme_calc_irq_sets updates it. */
	dev->io_queues[HCTX_TYPE_DEFAULT] = 1;
	dev->io_queues[HCTX_TYPE_READ] = 0;

	if (dev->ctrl.quirks & NVME_QUIRK_BROKEN_MSI)
		flags &= ~PCI_IRQ_MSI;
	return pci_alloc_irq_vectors_affinity(pdev, 1, irq_queues, flags,
					      &affd);
}

void nvme_pci_map_queues(struct blk_mq_tag_set *set)
{
	struct nvme_dev *dev = to_nvme_dev(set->driver_data);
	unsigned int i, qoff, offset;

	offset = (unsigned int)nvme_queue_irq_offset(dev);
	for (i = 0, qoff = 0; i < set->nr_maps; i++) {
		struct blk_mq_queue_map *map = &set->map[i];

		map->nr_queues = dev->io_queues[i];
		if (!map->nr_queues) {
			BUG_ON(i == HCTX_TYPE_DEFAULT);
			continue;
		}

		/* Poll queues have no interrupt and so no affinity: the
		 * plain CPU spread. */
		map->queue_offset = qoff;
		if (i != HCTX_TYPE_POLL && offset)
			blk_mq_map_hw_queues(map, dev->pdev, offset);
		else
			blk_mq_map_queues(map);
		qoff += map->nr_queues;
		offset += map->nr_queues;
	}
}

/* ==========================================================================
 * Handlers
 * ========================================================================== */

int nvme_irq(void *data)
{
	struct nvme_queue *nvmeq = data;
	DEFINE_IO_COMP_BATCH(iob);

	nvmeq->nr_irqs++;
	if (nvme_poll_cq(nvmeq, &iob)) {
		if (iob.req_list)
			nvme_pci_complete_batch(&iob);
		return IRQ_HANDLED;
	}
	nvmeq->nr_spurious++;
	return IRQ_NONE;
}

int nvme_irq_check(void *data)
{
	struct nvme_queue *nvmeq = data;

	if (nvme_cqe_pending(nvmeq))
		return IRQ_WAKE_THREAD;
	return IRQ_NONE;
}

int nvme_pci_request_queue_irq(struct nvme_queue *nvmeq)
{
	struct nvme_dev *dev = nvmeq->dev;
	pci_device_t *pdev = dev->pdev;
	int irq, rc;

	might_sleep();
	irq = pci_irq_vector(pdev, nvmeq->cq_vector);
	if (irq < 0)
		return irq;
	ksnprintf(nvmeq->irq_name, sizeof(nvmeq->irq_name), "nvme%dq%d",
		  dev->ctrl.instance, nvmeq->qid);
	if (nvmeq->qid < 8)
		ksnprintf(dev->irq_name[nvmeq->qid],
			  sizeof(dev->irq_name[nvmeq->qid]), "%s",
			  nvmeq->irq_name);
	/* Shared: with a single vector every queue's handler is on it, and
	 * an INTx line may be shared with other devices. */
	if (nvme_pci_param.use_threaded_interrupts)
		rc = request_threaded_irq(irq, nvme_irq_check, nvme_irq,
					  IRQF_SHARED, nvmeq->irq_name, nvmeq);
	else
		rc = request_irq(irq, nvme_irq, IRQF_SHARED, nvmeq->irq_name,
				 nvmeq);
	if (rc == 0)
		nvmeq->irq = irq;
	return rc;
}

void nvme_pci_free_queue_irq(struct nvme_queue *nvmeq)
{
	might_sleep();
	if (nvmeq->irq < 0)
		return;
	free_irq(nvmeq->irq, nvmeq);
	nvmeq->irq = -1;
}

/* ==========================================================================
 * Interrupt coalescing
 * ========================================================================== */

int nvme_set_irq_coalescing(struct nvme_dev *dev, uint8_t time_100us,
			    uint8_t threshold)
{
	uint32_t result = 0;
	int ret;

	might_sleep();
	ret = nvme_set_features(&dev->ctrl, NVME_FEAT_IRQ_COALESCE,
				NVME_FEAT_IRQ_COALESCE_VAL(threshold,
							   time_100us),
				NULL, 0, &result);
	if (ret)
		kprintf(NVME_PCI_IRQ_LOG
			"%s: interrupt coalescing (time %u00 us, threshold %u) not set: %d\n",
			dev->ctrl.name, time_100us, threshold + 1u, ret);
	return ret;
}
