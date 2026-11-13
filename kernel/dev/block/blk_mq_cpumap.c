// LikeOS - multi-queue block layer: CPU -> hardware queue maps
//
// Every CPU submits to exactly one hardware context of each type
// (default, read, poll).  The map of a type says which: mq_map[cpu] is
// an index into the tag set's hardware queues, inside the type's range
// queue_offset .. queue_offset + nr_queues - 1.
//
//   * blk_mq_map_queues() spreads the CPUs evenly over the queues with
//     group_cpus_evenly() -- CPUs that share a core or a last-level cache
//     end up on the same queue -- when the device has no interrupt
//     affinity to follow (one interrupt, polled queues);
//   * blk_mq_map_hw_queues() follows the interrupt affinity of a PCI
//     device: queue i of the map is served by vector offset + i, and
//     every CPU in that vector's affinity mask submits to queue i, so a
//     request completes on the CPU (or at least the cache domain) that
//     submitted it.  A device without per-vector affinity falls back to
//     the even spread.
//
// Both may sleep (group_cpus_evenly allocates).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/irq.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/smp.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* CPUs that exist: every processor the firmware lists (the MADT entries
 * smp_init starts, known from acpi_init on -- long before the APs run),
 * and every online CPU.  A driver probed before SMP is up sizes its
 * queues by this set, so it must not shrink to the boot CPU then.
 * LikeOS has no CPU hot-plug: once SMP is up the two sets agree unless
 * an AP failed to start. */
static void blk_mq_possible_cpus(cpumask_t *out)
{
	uint32_t n = acpi_get_cpu_count();

	cpumask_online(out);
	if (n > MAX_CPUS)
		n = MAX_CPUS;
	for (uint32_t cpu = 0; cpu < n; cpu++)
		cpumask_set_cpu(cpu, out);
	if (cpumask_empty(out))
		cpumask_set_cpu(0, out);
}

static unsigned int blk_mq_num_queues(const cpumask_t *mask,
				      unsigned int max_queues)
{
	unsigned int num = cpumask_weight(mask);

	/* min_not_zero(): 0 means "no cap" */
	if (!max_queues)
		return num;
	if (!num)
		return max_queues;
	return num < max_queues ? num : max_queues;
}

unsigned int blk_mq_num_possible_queues(unsigned int max_queues)
{
	cpumask_t m;

	blk_mq_possible_cpus(&m);
	return blk_mq_num_queues(&m, max_queues);
}

unsigned int blk_mq_num_online_queues(unsigned int max_queues)
{
	cpumask_t m;

	cpumask_online(&m);
	if (cpumask_empty(&m))
		cpumask_set_cpu(0, &m);
	return blk_mq_num_queues(&m, max_queues);
}

/* Every slot of the map, also those of CPUs that do not exist, gets a
 * valid queue: a lookup with a stale CPU number can never index past the
 * type's range. */
static void blk_mq_map_fill_default(struct blk_mq_queue_map *qmap)
{
	for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++)
		qmap->mq_map[cpu] = qmap->queue_offset;
}

void blk_mq_map_queues(struct blk_mq_queue_map *qmap)
{
	cpumask_t *masks;
	unsigned int nr_masks = 0;

	BUG_ON(qmap == NULL || qmap->mq_map == NULL);
	blk_mq_map_fill_default(qmap);
	if (!qmap->nr_queues)
		return;
	masks = group_cpus_evenly(qmap->nr_queues, &nr_masks);
	if (!masks || !nr_masks) {
		/* Out of memory: everybody on the first queue of the type. */
		if (masks)
			kfree(masks);
		return;
	}
	for (unsigned int queue = 0; queue < qmap->nr_queues; queue++) {
		const cpumask_t *m = &masks[queue % nr_masks];

		for (unsigned int cpu = cpumask_first(m); cpu < MAX_CPUS;
		     cpu = cpumask_next((int)cpu, m))
			qmap->mq_map[cpu] = qmap->queue_offset + queue;
	}
	kfree(masks);
}

void blk_mq_map_hw_queues(struct blk_mq_queue_map *qmap,
			  struct pci_device *dev, unsigned int offset)
{
	cpumask_t covered, all;

	BUG_ON(qmap == NULL || qmap->mq_map == NULL);
	if (!dev || !qmap->nr_queues)
		goto fallback;
	cpumask_clear(&covered);
	blk_mq_map_fill_default(qmap);
	for (unsigned int queue = 0; queue < qmap->nr_queues; queue++) {
		const cpumask_t *mask = pci_irq_get_affinity(dev, queue + offset);

		if (!mask || cpumask_empty(mask))
			goto fallback;
		for (unsigned int cpu = cpumask_first(mask); cpu < MAX_CPUS;
		     cpu = cpumask_next((int)cpu, mask)) {
			qmap->mq_map[cpu] = qmap->queue_offset + queue;
			cpumask_set_cpu(cpu, &covered);
		}
	}
	/* A CPU no vector's affinity names (the masks are spread over every
	 * CPU, so this is a CPU that came up after the vectors were spread)
	 * still needs a queue of the type: spread them round robin. */
	blk_mq_possible_cpus(&all);
	{
		unsigned int i = 0;

		for (unsigned int cpu = cpumask_first(&all); cpu < MAX_CPUS;
		     cpu = cpumask_next((int)cpu, &all)) {
			if (cpumask_test_cpu(cpu, &covered))
				continue;
			qmap->mq_map[cpu] =
				qmap->queue_offset + (i++ % qmap->nr_queues);
		}
	}
	return;

fallback:
	blk_mq_map_queues(qmap);
}
