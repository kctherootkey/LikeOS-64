// LikeOS - the per-CPU interrupt vector matrix
//
// Book-keeping of which IDT vector is in use on which CPU (see
// kernel/ke/irq_internal.h for the model).  The strategies:
//
//   * a regular allocation goes to the CPU of the requested mask that has
//     the most vectors left (spreads unmanaged interrupts over the CPUs);
//   * a managed interrupt first reserves one bit on every CPU of its mask
//     and later allocates one of those reserved bits on the CPU with the
//     fewest managed allocations (spreads queue vectors evenly, and an
//     allocation can never fail once the reservation succeeded);
//   * a bit is free on a CPU when it is in none of: the system map, that
//     CPU's managed map, that CPU's allocation map.
//
// Pure bit arithmetic: no locks (the caller serialises), no allocation,
// no hardware -- the same file is compiled into the host test.  The pure
// unhandled-interrupt accounting of the interrupt core lives at the end
// for the same reason.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/irq_internal.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/uapi/bug.h>

static inline int bm_test(const uint64_t *map, unsigned int bit)
{
	return (int)((map[bit / 64] >> (bit % 64)) & 1);
}

static inline void bm_set(uint64_t *map, unsigned int bit)
{
	map[bit / 64] |= 1ULL << (bit % 64);
}

static inline void bm_clear(uint64_t *map, unsigned int bit)
{
	map[bit / 64] &= ~(1ULL << (bit % 64));
}

/* Is `bit' taken on CPU map cm (system, reserved for managed, or
 * allocated)? */
static inline int matrix_bit_busy(const struct irq_matrix *m,
				  const struct irq_cpumap *cm, unsigned int bit)
{
	return bm_test(m->system_map, bit) || bm_test(cm->managed_map, bit) ||
	       bm_test(cm->alloc_map, bit);
}

/* The bits of [start, end) to try, in search order: from search_start
 * to the end, then from alloc_start up to search_start. */
static unsigned int matrix_search_next(const struct irq_matrix *m,
				       unsigned int i)
{
	unsigned int span = m->alloc_end - m->alloc_start;
	unsigned int off = (m->search_start - m->alloc_start + i) % span;

	return m->alloc_start + off;
}

void irq_matrix_setup(struct irq_matrix *m, unsigned int alloc_start,
		      unsigned int alloc_end, unsigned int search_start)
{
	BUG_ON(m == NULL);
	BUG_ON(alloc_end > IRQ_MATRIX_BITS || alloc_start >= alloc_end);
	BUG_ON(search_start < alloc_start || search_start >= alloc_end);
	__builtin_memset(m, 0, sizeof(*m));
	m->alloc_start = alloc_start;
	m->alloc_end = alloc_end;
	m->search_start = search_start;
	m->alloc_size = alloc_end - alloc_start;
	for (unsigned int c = 0; c < MAX_CPUS; c++)
		m->maps[c].available = m->alloc_size;
}

int irq_matrix_is_system(const struct irq_matrix *m, unsigned int bit)
{
	if (bit >= IRQ_MATRIX_BITS)
		return 0;
	return bm_test(m->system_map, bit);
}

/* A new system bit: every CPU loses one available vector when the bit is
 * inside the allocation range. */
static void matrix_take_system(struct irq_matrix *m, unsigned int bit)
{
	bm_set(m->system_map, bit);
	if (bit >= m->alloc_start && bit < m->alloc_end) {
		m->systembits_inalloc++;
		for (unsigned int c = 0; c < MAX_CPUS; c++) {
			WARN_ON_ONCE(m->maps[c].available == 0);
			if (m->maps[c].available)
				m->maps[c].available--;
		}
	}
}

int irq_matrix_assign_system(struct irq_matrix *m, unsigned int bit)
{
	BUG_ON(bit >= IRQ_MATRIX_BITS);
	if (bm_test(m->system_map, bit))
		return -EBUSY;
	for (unsigned int c = 0; c < MAX_CPUS; c++) {
		const struct irq_cpumap *cm = &m->maps[c];

		if (bm_test(cm->alloc_map, bit) || bm_test(cm->managed_map, bit))
			return -EBUSY;
	}
	matrix_take_system(m, bit);
	return 0;
}

int irq_matrix_alloc_system(struct irq_matrix *m, unsigned int first,
			    unsigned int last)
{
	if (last >= IRQ_MATRIX_BITS || first > last)
		return -EINVAL;
	for (unsigned int bit = first; bit <= last; bit++) {
		int busy = bm_test(m->system_map, bit);

		for (unsigned int c = 0; !busy && c < MAX_CPUS; c++) {
			const struct irq_cpumap *cm = &m->maps[c];

			busy = bm_test(cm->alloc_map, bit) ||
			       bm_test(cm->managed_map, bit);
		}
		if (!busy) {
			matrix_take_system(m, bit);
			return (int)bit;
		}
	}
	return -ENOSPC;
}

void irq_matrix_free_system(struct irq_matrix *m, unsigned int bit)
{
	if (WARN_ON_ONCE(bit >= IRQ_MATRIX_BITS))
		return;
	if (WARN_ON_ONCE(!bm_test(m->system_map, bit)))
		return;
	/* An I/O APIC line routed to the same vector keeps it taken: the
	 * line is never unrouted (its vector is fixed, 32 + GSI). */
	if (bm_test(m->legacy_map, bit))
		return;
	bm_clear(m->system_map, bit);
	if (bit >= m->alloc_start && bit < m->alloc_end) {
		BUG_ON(m->systembits_inalloc == 0);
		m->systembits_inalloc--;
		for (unsigned int c = 0; c < MAX_CPUS; c++)
			m->maps[c].available++;
	}
}

int irq_matrix_assign_legacy(struct irq_matrix *m, unsigned int bit)
{
	BUG_ON(bit >= IRQ_MATRIX_BITS);
	if (bm_test(m->legacy_map, bit))
		return 0;
	if (bit < m->alloc_start || bit >= m->alloc_end) {
		/* A fixed vector below (or above) the managed range: nothing
		 * else is ever handed out there. */
		bm_set(m->legacy_map, bit);
		return 0;
	}
	if (!bm_test(m->system_map, bit)) {
		/* A per-CPU allocation (or a managed reservation) already
		 * owns the vector on some CPU: the line would be delivered
		 * into another interrupt's handler there. */
		for (unsigned int c = 0; c < MAX_CPUS; c++) {
			const struct irq_cpumap *cm = &m->maps[c];

			if (bm_test(cm->alloc_map, bit) ||
			    bm_test(cm->managed_map, bit))
				return -EBUSY;
		}
		matrix_take_system(m, bit);
	}
	/* Already a system vector (the system-wide pool handed it to an old
	 * driver's MSI): both share the vector-indexed handler table, as
	 * they always did; the line keeps it once the MSI is gone. */
	bm_set(m->legacy_map, bit);
	return 0;
}

int irq_matrix_is_legacy(const struct irq_matrix *m, unsigned int bit)
{
	if (bit >= IRQ_MATRIX_BITS)
		return 0;
	return bm_test(m->legacy_map, bit);
}

/* `num' consecutive free bits aligned to `align' on CPU map cm, in the
 * search order (a block never wraps around the end of the range).
 * Returns the first bit, or alloc_end when there is no room. */
static unsigned int matrix_find_area(const struct irq_matrix *m,
				     const struct irq_cpumap *cm,
				     unsigned int num, unsigned int align)
{
	for (unsigned int i = 0; i < m->alloc_size; i++) {
		unsigned int start = matrix_search_next(m, i);
		unsigned int k;

		if (align > 1 && (start % align))
			continue;
		if (start + num > m->alloc_end)
			continue;
		for (k = 0; k < num; k++)
			if (matrix_bit_busy(m, cm, start + k))
				break;
		if (k == num)
			return start;
	}
	return m->alloc_end;
}

/* Find and mark (in the managed or the allocation map). */
static unsigned int matrix_alloc_area(struct irq_matrix *m,
				      struct irq_cpumap *cm, unsigned int num,
				      unsigned int align, int managed)
{
	unsigned int start = matrix_find_area(m, cm, num, align);

	if (start >= m->alloc_end)
		return start;
	for (unsigned int k = 0; k < num; k++) {
		if (managed)
			bm_set(cm->managed_map, start + k);
		else
			bm_set(cm->alloc_map, start + k);
	}
	return start;
}

/* The online CPU of msk with the most available vectors (UINT_MAX when
 * none has any). */
static unsigned int matrix_find_best_cpu(const struct irq_matrix *m,
					 const cpumask_t *msk)
{
	unsigned int cpu, best = ~0u, maxavl = 0;

	for_each_cpu(cpu, msk) {
		const struct irq_cpumap *cm = &m->maps[cpu];

		if (!irq_matrix_cpu_online(cpu) || cm->available <= maxavl)
			continue;
		best = cpu;
		maxavl = cm->available;
	}
	return best;
}

/* The online CPU of msk with the fewest managed allocations. */
static unsigned int matrix_find_best_cpu_managed(const struct irq_matrix *m,
						 const cpumask_t *msk)
{
	unsigned int cpu, best = ~0u, allocated = ~0u;

	for_each_cpu(cpu, msk) {
		const struct irq_cpumap *cm = &m->maps[cpu];

		if (!irq_matrix_cpu_online(cpu) ||
		    cm->managed_allocated > allocated)
			continue;
		/* Ties go to the later CPU, as in the strict comparison
		 * above: the walk is in CPU order. */
		best = cpu;
		allocated = cm->managed_allocated;
	}
	return best;
}

int irq_matrix_reserve_managed(struct irq_matrix *m, const cpumask_t *msk)
{
	unsigned int cpu, failed;

	for_each_cpu(cpu, msk) {
		struct irq_cpumap *cm = &m->maps[cpu];
		unsigned int bit = matrix_alloc_area(m, cm, 1, 1, 1);

		if (bit >= m->alloc_end)
			goto cleanup;
		cm->managed++;
		BUG_ON(cm->available == 0);
		cm->available--;
	}
	return 0;
cleanup:
	failed = cpu;
	for_each_cpu(cpu, msk) {
		cpumask_t one;

		if (cpu == failed)
			break;
		cpumask_clear(&one);
		cpumask_set_cpu(cpu, &one);
		irq_matrix_remove_managed(m, &one);
	}
	return -ENOSPC;
}

void irq_matrix_remove_managed(struct irq_matrix *m, const cpumask_t *msk)
{
	unsigned int cpu;

	for_each_cpu(cpu, msk) {
		struct irq_cpumap *cm = &m->maps[cpu];
		unsigned int bit;
		int found = 0;

		if (WARN_ON_ONCE(!cm->managed))
			continue;
		/* Any reserved bit that is not allocated: the managed
		 * interrupts free their allocation when they shut down. */
		for (bit = m->alloc_start; bit < m->alloc_end; bit++) {
			if (bm_test(cm->managed_map, bit) &&
			    !bm_test(cm->alloc_map, bit)) {
				found = 1;
				break;
			}
		}
		if (WARN_ON_ONCE(!found))
			continue;
		bm_clear(cm->managed_map, bit);
		cm->managed--;
		cm->available++;
	}
}

int irq_matrix_alloc_managed(struct irq_matrix *m, const cpumask_t *msk,
			     unsigned int *mapped_cpu)
{
	unsigned int cpu, bit;
	struct irq_cpumap *cm;

	if (cpumask_empty(msk))
		return -EINVAL;
	cpu = matrix_find_best_cpu_managed(m, msk);
	if (cpu == ~0u)
		return -ENOSPC;
	cm = &m->maps[cpu];
	for (bit = m->alloc_start; bit < m->alloc_end; bit++)
		if (bm_test(cm->managed_map, bit) && !bm_test(cm->alloc_map, bit))
			break;
	if (bit >= m->alloc_end)
		return -ENOSPC;
	bm_set(cm->alloc_map, bit);
	cm->allocated++;
	cm->managed_allocated++;
	m->total_allocated++;
	*mapped_cpu = cpu;
	return (int)bit;
}

int irq_matrix_alloc(struct irq_matrix *m, const cpumask_t *msk, int reserved,
		     unsigned int *mapped_cpu)
{
	unsigned int cpu, bit;
	struct irq_cpumap *cm;

	if (cpumask_empty(msk))
		return -EINVAL;
	cpu = matrix_find_best_cpu(m, msk);
	if (cpu == ~0u)
		return -ENOSPC;
	cm = &m->maps[cpu];
	bit = matrix_alloc_area(m, cm, 1, 1, 0);
	if (bit >= m->alloc_end)
		return -ENOSPC;
	cm->allocated++;
	cm->available--;
	m->total_allocated++;
	if (reserved && m->global_reserved)
		m->global_reserved--;
	*mapped_cpu = cpu;
	return (int)bit;
}

int irq_matrix_alloc_block(struct irq_matrix *m, const cpumask_t *msk,
			   unsigned int num, unsigned int *mapped_cpu)
{
	unsigned int cpu, best = ~0u, maxavl = 0, bit;
	struct irq_cpumap *cm;

	if (cpumask_empty(msk) || num == 0 || (num & (num - 1)) || num > 32)
		return -EINVAL;
	if (num == 1)
		return irq_matrix_alloc(m, msk, 0, mapped_cpu);
	/* The CPU with the most available vectors among those where an
	 * aligned block of `num' still fits. */
	for_each_cpu(cpu, msk) {
		cm = &m->maps[cpu];
		if (!irq_matrix_cpu_online(cpu) || cm->available < num ||
		    cm->available <= maxavl)
			continue;
		if (matrix_find_area(m, cm, num, num) >= m->alloc_end)
			continue;
		best = cpu;
		maxavl = cm->available;
	}
	if (best == ~0u)
		return -ENOSPC;
	cm = &m->maps[best];
	bit = matrix_alloc_area(m, cm, num, num, 0);
	BUG_ON(bit >= m->alloc_end);
	cm->allocated += num;
	cm->available -= num;
	m->total_allocated += num;
	*mapped_cpu = best;
	return (int)bit;
}

void irq_matrix_free(struct irq_matrix *m, unsigned int cpu, unsigned int bit,
		     int managed)
{
	struct irq_cpumap *cm;

	if (WARN_ON_ONCE(cpu >= MAX_CPUS))
		return;
	if (WARN_ON_ONCE(bit < m->alloc_start || bit >= m->alloc_end))
		return;
	cm = &m->maps[cpu];
	if (WARN_ON_ONCE(!bm_test(cm->alloc_map, bit)))
		return;
	bm_clear(cm->alloc_map, bit);
	BUG_ON(cm->allocated == 0);
	cm->allocated--;
	BUG_ON(m->total_allocated == 0);
	m->total_allocated--;
	if (managed) {
		WARN_ON_ONCE(!bm_test(cm->managed_map, bit));
		BUG_ON(cm->managed_allocated == 0);
		cm->managed_allocated--;
	} else {
		cm->available++;
	}
}

void irq_matrix_reserve(struct irq_matrix *m)
{
	if (m->global_reserved >= irq_matrix_available(m))
		WARN_RATELIMIT(1,
			       "irq: interrupt reservation exceeds the free vectors");
	m->global_reserved++;
}

void irq_matrix_remove_reserved(struct irq_matrix *m)
{
	if (WARN_ON_ONCE(m->global_reserved == 0))
		return;
	m->global_reserved--;
}

unsigned int irq_matrix_available(const struct irq_matrix *m)
{
	unsigned int n = 0, cpu;
	unsigned int online = cpumask_nr_online();

	for (cpu = 0; cpu < online && cpu < MAX_CPUS; cpu++)
		n += m->maps[cpu].available;
	return n;
}

unsigned int irq_matrix_allocated_on(const struct irq_matrix *m,
				     unsigned int cpu)
{
	if (cpu >= MAX_CPUS)
		return 0;
	return m->maps[cpu].allocated - m->maps[cpu].managed_allocated;
}

int irq_matrix_test_alloc(const struct irq_matrix *m, unsigned int cpu,
			  unsigned int bit)
{
	if (cpu >= MAX_CPUS || bit >= IRQ_MATRIX_BITS)
		return 0;
	return bm_test(m->maps[cpu].alloc_map, bit);
}

/* ==========================================================================
 * Unhandled-interrupt accounting (see irq_internal.h)
 * ========================================================================== */

int irq_spurious_note(struct irq_spurious *sp, int action_ret,
		      uint32_t threads_handled, uint64_t (*clock)(void))
{
	if (action_ret < 0 || action_ret > (IRQ_HANDLED | IRQ_WAKE_THREAD))
		return IRQ_SPURIOUS_BOGUS;
	if (action_ret & IRQ_WAKE_THREAD) {
		if (action_ret == IRQ_WAKE_THREAD) {
			uint32_t handled;

			/* First wake since a primary claimed one: nothing
			 * to judge yet. */
			if (!(sp->threads_handled_last &
			      IRQ_SPURIOUS_DEFERRED)) {
				sp->threads_handled_last |=
					IRQ_SPURIOUS_DEFERRED;
				return IRQ_SPURIOUS_OK;
			}
			/* Did a thread claim an interrupt since the last
			 * look?  (The previous wake's verdict; the flag
			 * stays set for this one.) */
			handled = threads_handled | IRQ_SPURIOUS_DEFERRED;
			if (handled != sp->threads_handled_last) {
				action_ret = IRQ_HANDLED;
				sp->threads_handled_last = handled;
			} else {
				action_ret = IRQ_NONE;
			}
		} else {
			/* A primary handler claimed it: the threads'
			 * results do not matter for this one. */
			sp->threads_handled_last &= ~IRQ_SPURIOUS_DEFERRED;
		}
	}
	if (action_ret == IRQ_NONE) {
		uint64_t now = clock();

		if (now - sp->last_unhandled_ns > IRQ_SPURIOUS_RESET_NS)
			sp->unhandled = 1;
		else
			sp->unhandled++;
		sp->last_unhandled_ns = now;
	}
	if (!sp->unhandled)
		return IRQ_SPURIOUS_OK;
	if (++sp->irq_count < IRQ_SPURIOUS_WINDOW)
		return IRQ_SPURIOUS_OK;
	sp->irq_count = 0;
	if (sp->unhandled > IRQ_SPURIOUS_LIMIT) {
		sp->unhandled = 0;
		return IRQ_SPURIOUS_STUCK;
	}
	sp->unhandled = 0;
	return IRQ_SPURIOUS_OK;
}
