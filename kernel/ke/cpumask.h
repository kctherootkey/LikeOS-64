// LikeOS - CPU masks
//
// A set of CPU indexes (0 .. MAX_CPUS-1, the logical numbering of percpu.h
// and smp.c; CPU 0 is the boot processor).  Used for interrupt affinity
// (which CPUs a vector's handler may run on), queue maps (which CPUs submit
// to a hardware queue) and the spreading of per-queue vectors over the
// machine.
//
// A cpumask_t is a value type: copy it with plain assignment, pass small
// masks by pointer.  None of these helpers lock anything; a mask shared
// between CPUs is protected by its owner's lock.
//
// Online CPUs: the processors that were started are 0 .. n-1 with
// n = percpu_get_online_count() (no CPU hot-plug); cpumask_online() fills that.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_CPUMASK_H
#define KERNEL_KE_CPUMASK_H

#include <kernel/uapi/types.h>
/* MAX_CPUS, spelled exactly as kernel/ke/percpu.h spells it (an identical
 * redefinition is no error), so this header stays light enough for
 * kernel/hal/pci.h and kernel/ke/irq.h; cpumask.c checks the two agree. */
#ifndef MAX_CPUS
#define MAX_CPUS 64
#endif

#define CPUMASK_WORDS ((MAX_CPUS + 63) / 64)

typedef struct cpumask {
	uint64_t bits[CPUMASK_WORDS];
} cpumask_t;

#define CPUMASK_NONE                     \
	{                                \
		{                        \
			0                \
		}                        \
	}

static inline void cpumask_clear(cpumask_t *m)
{
	for (int i = 0; i < CPUMASK_WORDS; i++)
		m->bits[i] = 0;
}

/* Every index below MAX_CPUS (possible, not necessarily online). */
static inline void cpumask_setall(cpumask_t *m)
{
	for (int i = 0; i < CPUMASK_WORDS; i++)
		m->bits[i] = ~0ULL;
	if (MAX_CPUS % 64)
		m->bits[CPUMASK_WORDS - 1] &= (1ULL << (MAX_CPUS % 64)) - 1;
}

static inline void cpumask_set_cpu(unsigned int cpu, cpumask_t *m)
{
	if (cpu < MAX_CPUS)
		m->bits[cpu / 64] |= 1ULL << (cpu % 64);
}

static inline void cpumask_clear_cpu(unsigned int cpu, cpumask_t *m)
{
	if (cpu < MAX_CPUS)
		m->bits[cpu / 64] &= ~(1ULL << (cpu % 64));
}

static inline int cpumask_test_cpu(unsigned int cpu, const cpumask_t *m)
{
	if (cpu >= MAX_CPUS)
		return 0;
	return (m->bits[cpu / 64] >> (cpu % 64)) & 1;
}

/* Set the bit and say whether it was set before. */
static inline int cpumask_test_and_set_cpu(unsigned int cpu, cpumask_t *m)
{
	int was = cpumask_test_cpu(cpu, m);

	cpumask_set_cpu(cpu, m);
	return was;
}

static inline unsigned int cpumask_weight(const cpumask_t *m)
{
	unsigned int n = 0;

	for (int i = 0; i < CPUMASK_WORDS; i++) {
		uint64_t v = m->bits[i];

		v = v - ((v >> 1) & 0x5555555555555555ULL);
		v = (v & 0x3333333333333333ULL) +
		    ((v >> 2) & 0x3333333333333333ULL);
		v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
		n += (unsigned int)((v * 0x0101010101010101ULL) >> 56);
	}
	return n;
}

static inline int cpumask_empty(const cpumask_t *m)
{
	for (int i = 0; i < CPUMASK_WORDS; i++)
		if (m->bits[i])
			return 0;
	return 1;
}

static inline int cpumask_equal(const cpumask_t *a, const cpumask_t *b)
{
	for (int i = 0; i < CPUMASK_WORDS; i++)
		if (a->bits[i] != b->bits[i])
			return 0;
	return 1;
}

/* Does a share any CPU with b? */
static inline int cpumask_intersects(const cpumask_t *a, const cpumask_t *b)
{
	for (int i = 0; i < CPUMASK_WORDS; i++)
		if (a->bits[i] & b->bits[i])
			return 1;
	return 0;
}

/* Is every CPU of a also in b? */
static inline int cpumask_subset(const cpumask_t *a, const cpumask_t *b)
{
	for (int i = 0; i < CPUMASK_WORDS; i++)
		if (a->bits[i] & ~b->bits[i])
			return 0;
	return 1;
}

static inline void cpumask_copy(cpumask_t *dst, const cpumask_t *src)
{
	*dst = *src;
}

/* dst = a & b; returns 1 when the result is not empty. */
static inline int cpumask_and(cpumask_t *dst, const cpumask_t *a,
			      const cpumask_t *b)
{
	uint64_t any = 0;

	for (int i = 0; i < CPUMASK_WORDS; i++) {
		dst->bits[i] = a->bits[i] & b->bits[i];
		any |= dst->bits[i];
	}
	return any != 0;
}

static inline void cpumask_or(cpumask_t *dst, const cpumask_t *a,
			      const cpumask_t *b)
{
	for (int i = 0; i < CPUMASK_WORDS; i++)
		dst->bits[i] = a->bits[i] | b->bits[i];
}

/* dst = a & ~b; returns 1 when the result is not empty. */
static inline int cpumask_andnot(cpumask_t *dst, const cpumask_t *a,
				 const cpumask_t *b)
{
	uint64_t any = 0;

	for (int i = 0; i < CPUMASK_WORDS; i++) {
		dst->bits[i] = a->bits[i] & ~b->bits[i];
		any |= dst->bits[i];
	}
	return any != 0;
}

/* The first set CPU at or after `start', MAX_CPUS when none. */
static inline unsigned int cpumask_next_from(unsigned int start,
					     const cpumask_t *m)
{
	for (unsigned int c = start; c < MAX_CPUS; c++) {
		uint64_t w = m->bits[c / 64] >> (c % 64);

		if (!w) {
			c = (c / 64) * 64 + 63; /* skip the rest of the word */
			continue;
		}
		return c + (unsigned int)__builtin_ctzll(w);
	}
	return MAX_CPUS;
}

static inline unsigned int cpumask_first(const cpumask_t *m)
{
	return cpumask_next_from(0, m);
}

/* The next set CPU after `cpu' (-1 starts the walk). */
static inline unsigned int cpumask_next(int cpu, const cpumask_t *m)
{
	return cpumask_next_from((unsigned int)(cpu + 1), m);
}

/* The n-th (0-based) set CPU, MAX_CPUS when there are fewer. */
static inline unsigned int cpumask_nth(unsigned int n, const cpumask_t *m)
{
	unsigned int c;

	for (c = cpumask_first(m); c < MAX_CPUS; c = cpumask_next((int)c, m))
		if (n-- == 0)
			return c;
	return MAX_CPUS;
}

#define for_each_cpu(cpu, mask)                                       \
	for ((cpu) = cpumask_first(mask); (cpu) < MAX_CPUS;            \
	     (cpu) = cpumask_next((int)(cpu), (mask)))

/* The online CPUs (0 .. percpu_get_online_count()-1).
 * Any context. */
void cpumask_online(cpumask_t *out);
/* Number of online CPUs (>= 1). */
unsigned int cpumask_nr_online(void);

#define for_each_online_cpu(cpu)                                      \
	for ((cpu) = 0; (cpu) < cpumask_nr_online(); (cpu)++)

#endif /* KERNEL_KE_CPUMASK_H */
