// LikeOS - the online CPU mask (cpumask.h)
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/cpumask.h>
#include <kernel/ke/percpu.h>
#include <kernel/uapi/bug.h>

unsigned int cpumask_nr_online(void)
{
	BUILD_BUG_ON(CPUMASK_WORDS * 64 < MAX_CPUS);
	uint32_t n = percpu_get_online_count();

	if (n == 0)
		n = 1;
	if (n > MAX_CPUS)
		n = MAX_CPUS;
	return n;
}

void cpumask_online(cpumask_t *out)
{
	unsigned int n = cpumask_nr_online();

	cpumask_clear(out);
	for (unsigned int c = 0; c < n; c++)
		cpumask_set_cpu(c, out);
}
