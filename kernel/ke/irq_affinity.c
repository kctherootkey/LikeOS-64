// LikeOS - interrupt affinity spreading
//
// group_cpus_evenly() splits the CPUs into a number of groups such that
// every CPU is in exactly one group, every group gets as close to the same
// number of CPUs as possible, and CPUs that are close to each other end up
// in the same group: first by memory node, then -- inside a node -- by
// cluster (the CPUs sharing a level-2 cache), and when a group takes CPUs
// one at a time, a CPU's SMT siblings are taken right after it.  The
// two-stage scheme (the CPUs present first, then the possible-but-absent
// ones starting at the next group) is kept; LikeOS has no CPU hot-plug,
// so the second stage finds nothing and the online CPUs are the present
// and the possible ones alike.
//
// irq_create_affinity_masks() turns an irq_affinity description into
// one mask per vector: pre/post vectors get the default affinity, each
// set is spread with group_cpus_evenly(), the spread vectors are managed.
//
// Topology: a CPU's SMT siblings and its cluster come from the APIC IDs
// (percpu.h) and the processor's topology/cache enumeration (CPUID leaves
// 0x1f/0xb for the SMT width, leaf 4 or 0x8000001d for the CPUs sharing
// the L2).  There is no NUMA information: one node.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/irq.h>
#include <kernel/ke/cpumask.h>
#include <kernel/uapi/bug.h>
#ifndef IRQ_AFFINITY_TEST_HOOKS
#include <kernel/ke/percpu.h>
#include <kernel/mm/memory.h>
#endif

#define AFF_MAX_NODES 8

/* ---- topology ----------------------------------------------------------- */

struct aff_topo {
	unsigned int ncpus; /* online CPUs 0 .. ncpus-1 */
	unsigned int nnodes; /* memory nodes (1 without NUMA data) */
	uint32_t apic[MAX_CPUS];
	uint8_t node[MAX_CPUS];
	uint32_t smt_shift; /* APIC ID >> smt_shift = core */
	uint32_t l2_shift; /* APIC ID >> l2_shift = cluster */
	int have_clusters; /* the L2 sharing is known */
};

#ifdef IRQ_AFFINITY_TEST_HOOKS
/* The host test supplies the topology. */
extern void aff_test_topology(struct aff_topo *t);
#define kcalloc(n, s) __builtin_calloc((n), (s))
#define kfree(p) __builtin_free(p)
#else
static inline void aff_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
	__asm__ volatile("cpuid"
			 : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
			 : "a"(leaf), "c"(sub));
}

static uint32_t aff_order(uint32_t n)
{
	if (n <= 1)
		return 0;
	return 32u - (uint32_t)__builtin_clz(n - 1u);
}

/* Width of the SMT field of the APIC ID: the extended topology leaf's
 * first level when it is the SMT level, else the logical processors per
 * core derived from leaf 1 / leaf 4. */
static uint32_t aff_smt_shift(uint32_t max_leaf)
{
	uint32_t r[4];

	for (uint32_t leaf = 0x1F; leaf >= 0x0B; leaf -= (0x1F - 0x0B)) {
		if (max_leaf < leaf)
			continue;
		aff_cpuid(leaf, 0, r);
		/* ECX[15:8] = level type, 1 = SMT; EBX = processors at
		 * this level (0: the leaf is not implemented). */
		if (((r[2] >> 8) & 0xFF) == 1 && (r[1] & 0xFFFF) != 0)
			return r[0] & 0x1F;
	}
	if (max_leaf >= 4) {
		uint32_t per_pkg, cores;

		aff_cpuid(1, 0, r);
		if (!(r[3] & (1u << 28)))
			return 0;
		per_pkg = (r[1] >> 16) & 0xFF;
		aff_cpuid(4, 0, r);
		cores = ((r[0] >> 26) & 0x3F) + 1;
		if (per_pkg > cores)
			return aff_order(per_pkg / cores);
	}
	return 0;
}

/* Width of the L2-sharing field: CPUID leaf 4 (Intel-compatible) or
 * 0x8000001d (AMD) level 2 data/unified cache, -1 when unknown. */
static int aff_l2_shift(uint32_t max_leaf)
{
	uint32_t r[4], max_ext;
	char vendor[13];

	aff_cpuid(0, 0, r);
	__builtin_memcpy(vendor + 0, &r[1], 4);
	__builtin_memcpy(vendor + 4, &r[3], 4);
	__builtin_memcpy(vendor + 8, &r[2], 4);
	vendor[12] = '\0';
	if (__builtin_memcmp(vendor, "AuthenticAMD", 12) == 0 ||
	    __builtin_memcmp(vendor, "HygonGenuine", 12) == 0) {
		aff_cpuid(0x80000000u, 0, r);
		max_ext = r[0];
		if (max_ext < 0x8000001Du)
			return -1;
		for (uint32_t i = 0; i < 16; i++) {
			aff_cpuid(0x8000001Du, i, r);
			if ((r[0] & 0x1F) == 0)
				break;
			if (((r[0] >> 5) & 0x7) == 2)
				return (int)aff_order(((r[0] >> 14) & 0xFFF) +
						      1);
		}
		return -1;
	}
	if (max_leaf < 4)
		return -1;
	for (uint32_t i = 0; i < 16; i++) {
		aff_cpuid(4, i, r);
		if ((r[0] & 0x1F) == 0)
			break;
		if (((r[0] >> 5) & 0x7) == 2)
			return (int)aff_order(((r[0] >> 14) & 0xFFF) + 1);
	}
	return -1;
}

static void aff_get_topology(struct aff_topo *t)
{
	uint32_t r[4], max_leaf;
	int l2;

	__builtin_memset(t, 0, sizeof(*t));
	t->ncpus = cpumask_nr_online();
	t->nnodes = 1;
	for (unsigned int c = 0; c < t->ncpus; c++) {
		percpu_t *pc = percpu_get(c);

		t->apic[c] = pc ? pc->apic_id : c;
		t->node[c] = 0;
	}
	aff_cpuid(0, 0, r);
	max_leaf = r[0];
	if (max_leaf == 0)
		return;
	t->smt_shift = aff_smt_shift(max_leaf);
	l2 = aff_l2_shift(max_leaf);
	if (l2 >= 0) {
		t->l2_shift = (uint32_t)l2;
		/* A cluster is at least the core. */
		if (t->l2_shift < t->smt_shift)
			t->l2_shift = t->smt_shift;
		t->have_clusters = 1;
	}
}
#endif

static void aff_sibling_mask(const struct aff_topo *t, unsigned int cpu,
			     cpumask_t *out)
{
	cpumask_clear(out);
	for (unsigned int c = 0; c < t->ncpus; c++)
		if ((t->apic[c] >> t->smt_shift) ==
		    (t->apic[cpu] >> t->smt_shift))
			cpumask_set_cpu(c, out);
}

/* The cluster mask of `cpu'; empty when clusters are unknown. */
static void aff_cluster_mask(const struct aff_topo *t, unsigned int cpu,
			     cpumask_t *out)
{
	cpumask_clear(out);
	if (!t->have_clusters)
		return;
	for (unsigned int c = 0; c < t->ncpus; c++)
		if ((t->apic[c] >> t->l2_shift) ==
		    (t->apic[cpu] >> t->l2_shift))
			cpumask_set_cpu(c, out);
}

/* ---- grouping ----------------------------------------------------------- */

/* Move `cpus_per_grp' CPUs from nmsk into irqmsk, each one followed by its
 * SMT siblings while the group has room. */
static void grp_spread_init_one(const struct aff_topo *t, cpumask_t *irqmsk,
				cpumask_t *nmsk, unsigned int cpus_per_grp)
{
	cpumask_t sibl;

	while (cpus_per_grp > 0) {
		unsigned int cpu = cpumask_first(nmsk);
		unsigned int s;

		if (cpu >= MAX_CPUS)
			return;
		cpumask_clear_cpu(cpu, nmsk);
		cpumask_set_cpu(cpu, irqmsk);
		cpus_per_grp--;

		aff_sibling_mask(t, cpu, &sibl);
		for (s = cpumask_first(&sibl); cpus_per_grp > 0 && s < MAX_CPUS;
		     s = cpumask_next((int)s, &sibl)) {
			if (!cpumask_test_cpu(s, nmsk))
				continue;
			cpumask_clear_cpu(s, nmsk);
			cpumask_set_cpu(s, irqmsk);
			cpus_per_grp--;
		}
	}
}

struct node_groups {
	unsigned int id;
	unsigned int ncpus; /* input; UINT_MAX = node unused */
	unsigned int ngroups; /* output */
};

/* Ascending by CPU count (insertion sort: a handful of entries). */
static void sort_node_groups(struct node_groups *ng, unsigned int n)
{
	for (unsigned int i = 1; i < n; i++) {
		struct node_groups v = ng[i];
		unsigned int j = i;

		while (j > 0 && ng[j - 1].ncpus > v.ncpus) {
			ng[j] = ng[j - 1];
			j--;
		}
		ng[j] = v;
	}
}

/* Give every unit (node or cluster) its share of numgrps in proportion to
 * its CPU count, smallest unit first, at least one each: the share never
 * exceeds the unit's CPU count (numgrps <= numcpus). */
static void alloc_groups_to_nodes(unsigned int numgrps, unsigned int numcpus,
				  struct node_groups *ng, unsigned int n)
{
	unsigned int remaining = numcpus;

	sort_node_groups(ng, n);
	for (unsigned int i = 0; i < n; i++) {
		unsigned int ncpus, ngroups;

		if (ng[i].ncpus == ~0u)
			continue;
		WARN_ON_ONCE(numgrps == 0);
		ncpus = ng[i].ncpus;
		ngroups = remaining ? numgrps * ncpus / remaining : 0;
		if (ngroups < 1)
			ngroups = 1;
		WARN_ON_ONCE(ngroups > ncpus);
		ng[i].ngroups = ngroups;
		remaining -= ncpus;
		numgrps = numgrps > ngroups ? numgrps - ngroups : 0;
	}
}

/* Spread the CPUs of nmsk (ncpus of them) over nv->ngroups groups starting
 * at *curgrp, wrapping at last_grp; the remainder goes one extra CPU per
 * group to the first groups. */
static void assign_cpus_to_groups(const struct aff_topo *t, unsigned int ncpus,
				  cpumask_t *nmsk, const struct node_groups *nv,
				  cpumask_t *masks, unsigned int *curgrp,
				  unsigned int last_grp)
{
	unsigned int extra = ncpus - nv->ngroups * (ncpus / nv->ngroups);

	for (unsigned int v = 0; v < nv->ngroups; v++, *curgrp += 1) {
		unsigned int per = ncpus / nv->ngroups;

		if (extra) {
			per++;
			extra--;
		}
		if (*curgrp >= last_grp)
			*curgrp = 0;
		grp_spread_init_one(t, &masks[*curgrp], nmsk, per);
	}
}

/* Inside one node: when the groups are at least as many as the clusters,
 * give every cluster its share and spread inside it (no group crosses a
 * cluster).  Returns 1 when done that way, 0 to fall back to the plain
 * spread. */
static int try_group_cluster_cpus(const struct aff_topo *t, unsigned int ncpus,
				  unsigned int ngroups, const cpumask_t *node_msk,
				  cpumask_t *masks, unsigned int *curgrp,
				  unsigned int last_grp)
{
	cpumask_t msk, cl, nmsk;
	cpumask_t clusters[MAX_CPUS];
	struct node_groups cg[MAX_CPUS];
	unsigned int ncluster = 0, cpu;

	if (!t->have_clusters)
		return 0;
	cpumask_copy(&msk, node_msk);
	while ((cpu = cpumask_first(&msk)) < MAX_CPUS) {
		aff_cluster_mask(t, cpu, &cl);
		if (cpumask_empty(&cl))
			return 0;
		cpumask_and(&clusters[ncluster], &cl, node_msk);
		cg[ncluster].id = ncluster;
		cg[ncluster].ncpus = cpumask_weight(&clusters[ncluster]);
		cg[ncluster].ngroups = 0;
		cpumask_andnot(&msk, &msk, &cl);
		ncluster++;
	}
	/* Fewer groups than clusters: crossing clusters is inevitable. */
	if (ncluster == 0 || ncluster > ngroups)
		return 0;
	alloc_groups_to_nodes(ngroups, ncpus, cg, ncluster);
	for (unsigned int i = 0; i < ncluster; i++) {
		const struct node_groups *nv = &cg[i];
		unsigned int nc;

		cpumask_and(&nmsk, node_msk, &clusters[nv->id]);
		nc = cpumask_weight(&nmsk);
		if (!nc)
			continue;
		WARN_ON_ONCE(nv->ngroups > nc);
		assign_cpus_to_groups(t, nc, &nmsk, nv, masks, curgrp, last_grp);
	}
	return 1;
}

/* One stage: spread the CPUs of cpu_mask over numgrps groups starting at
 * startgrp.  Returns the number of groups filled. */
static int group_cpus_stage(const struct aff_topo *t, unsigned int startgrp,
			    unsigned int numgrps, const cpumask_t *cpu_mask,
			    cpumask_t *masks)
{
	cpumask_t node_to_cpumask[AFF_MAX_NODES];
	cpumask_t nmsk;
	struct node_groups ng[AFF_MAX_NODES];
	unsigned int nodes = 0, curgrp = startgrp, last_grp = numgrps;
	unsigned int numcpus = 0, done = 0, nodemask = 0;
	unsigned int nnodes = t->nnodes;

	if (cpumask_empty(cpu_mask))
		return 0;
	if (nnodes == 0 || nnodes > AFF_MAX_NODES)
		nnodes = 1;
	for (unsigned int n = 0; n < nnodes; n++)
		cpumask_clear(&node_to_cpumask[n]);
	for (unsigned int c = 0; c < t->ncpus; c++)
		cpumask_set_cpu(c, &node_to_cpumask[t->node[c] < nnodes ?
							    t->node[c] :
							    0]);
	for (unsigned int n = 0; n < nnodes; n++) {
		if (cpumask_intersects(cpu_mask, &node_to_cpumask[n])) {
			nodemask |= 1u << n;
			nodes++;
		}
	}

	/* No more groups than nodes: one group per node (round robin). */
	if (numgrps <= nodes) {
		for (unsigned int n = 0; n < nnodes; n++) {
			if (!(nodemask & (1u << n)))
				continue;
			cpumask_and(&nmsk, cpu_mask, &node_to_cpumask[n]);
			cpumask_or(&masks[curgrp], &masks[curgrp], &nmsk);
			if (++curgrp == last_grp)
				curgrp = 0;
		}
		return (int)numgrps;
	}

	for (unsigned int n = 0; n < nnodes; n++) {
		ng[n].id = n;
		ng[n].ncpus = ~0u;
		ng[n].ngroups = 0;
	}
	for (unsigned int n = 0; n < nnodes; n++) {
		unsigned int nc;

		if (!(nodemask & (1u << n)))
			continue;
		cpumask_and(&nmsk, cpu_mask, &node_to_cpumask[n]);
		nc = cpumask_weight(&nmsk);
		if (!nc)
			continue;
		numcpus += nc;
		ng[n].ncpus = nc;
	}
	alloc_groups_to_nodes(numgrps < numcpus ? numgrps : numcpus, numcpus,
			      ng, nnodes);

	for (unsigned int i = 0; i < nnodes; i++) {
		const struct node_groups *nv = &ng[i];
		unsigned int nc;

		if (nv->ncpus == ~0u)
			continue;
		cpumask_and(&nmsk, cpu_mask, &node_to_cpumask[nv->id]);
		nc = cpumask_weight(&nmsk);
		if (!nc)
			continue;
		WARN_ON_ONCE(nv->ngroups > nc);
		if (try_group_cluster_cpus(t, nc, nv->ngroups, &nmsk, masks,
					   &curgrp, last_grp)) {
			done += nv->ngroups;
			continue;
		}
		assign_cpus_to_groups(t, nc, &nmsk, nv, masks, &curgrp,
				      last_grp);
		done += nv->ngroups;
	}
	return (int)done;
}

cpumask_t *group_cpus_evenly(unsigned int numgrps, unsigned int *nummasks)
{
	struct aff_topo *t;
	cpumask_t present, others, possible;
	cpumask_t *masks;
	unsigned int curgrp = 0, nr_present, nr_others = 0;
	int ret;

	if (numgrps == 0 || !nummasks)
		return NULL;
	/* The topology is big (a cluster table per call on the stack of
	 * the stages): keep the snapshot itself off the stack. */
	t = kcalloc(1, sizeof(*t));
	if (!t)
		return NULL;
	masks = kcalloc(numgrps, sizeof(*masks));
	if (!masks) {
		kfree(t);
		return NULL;
	}
#ifdef IRQ_AFFINITY_TEST_HOOKS
	aff_test_topology(t);
#else
	aff_get_topology(t);
#endif
	BUG_ON(t->ncpus == 0 || t->ncpus > MAX_CPUS);
	cpumask_clear(&present);
	for (unsigned int c = 0; c < t->ncpus; c++)
		cpumask_set_cpu(c, &present);
	/* Possible == present: no CPU can appear later. */
	cpumask_copy(&possible, &present);

	ret = group_cpus_stage(t, curgrp, numgrps, &present, masks);
	if (ret < 0)
		goto fail;
	nr_present = (unsigned int)ret;
	curgrp = nr_present >= numgrps ? 0 : nr_present;
	cpumask_andnot(&others, &possible, &present);
	ret = group_cpus_stage(t, curgrp, numgrps, &others, masks);
	if (ret >= 0)
		nr_others = (unsigned int)ret;
	*nummasks = nr_present + nr_others < numgrps ? nr_present + nr_others :
						       numgrps;
	kfree(t);
	return masks;
fail:
	kfree(t);
	kfree(masks);
	return NULL;
}

/* ---- affinity masks for a vector set --------------------------------- */

static void default_calc_sets(struct irq_affinity *affd, unsigned int affvecs)
{
	affd->nr_sets = 1;
	affd->set_size[0] = affvecs;
}

/* The default affinity of an unspread vector: every online CPU. */
static void irq_default_affinity(cpumask_t *m)
{
	cpumask_online(m);
}

struct irq_affinity_desc *irq_create_affinity_masks(unsigned int nvecs,
						    struct irq_affinity *affd)
{
	unsigned int affvecs, curvec, usedvecs, i;
	struct irq_affinity_desc *masks;

	BUG_ON(affd == NULL);
	if (nvecs > affd->pre_vectors + affd->post_vectors)
		affvecs = nvecs - affd->pre_vectors - affd->post_vectors;
	else
		affvecs = 0;

	/* The driver gets to adjust its sets to the count really obtained
	 * even when nothing is left to spread. */
	if (!affd->calc_sets)
		affd->calc_sets = default_calc_sets;
	affd->calc_sets(affd, affvecs);

	if (WARN_ON_ONCE(affd->nr_sets > IRQ_AFFINITY_MAX_SETS))
		return NULL;
	if (!affvecs)
		return NULL;

	masks = kcalloc(nvecs, sizeof(*masks));
	if (!masks)
		return NULL;

	for (curvec = 0; curvec < affd->pre_vectors; curvec++)
		irq_default_affinity(&masks[curvec].mask);

	for (i = 0, usedvecs = 0; i < affd->nr_sets; i++) {
		unsigned int nr_masks = 0, this_vecs = affd->set_size[i];
		cpumask_t *result;

		if (this_vecs == 0)
			continue;
		result = group_cpus_evenly(this_vecs, &nr_masks);
		if (!result) {
			kfree(masks);
			return NULL;
		}
		for (unsigned int j = 0; j < nr_masks && curvec + j < nvecs; j++)
			cpumask_copy(&masks[curvec + j].mask, &result[j]);
		kfree(result);
		curvec += nr_masks;
		usedvecs += nr_masks;
	}

	if (usedvecs >= affvecs)
		curvec = affd->pre_vectors + affvecs;
	else
		curvec = affd->pre_vectors + usedvecs;
	for (; curvec < nvecs; curvec++)
		irq_default_affinity(&masks[curvec].mask);

	for (i = affd->pre_vectors; i < nvecs - affd->post_vectors; i++)
		masks[i].is_managed = 1;
	return masks;
}

unsigned int irq_calc_affinity_vectors(unsigned int minvec, unsigned int maxvec,
				       const struct irq_affinity *affd)
{
	unsigned int resv, set_vecs;

	BUG_ON(affd == NULL);
	resv = affd->pre_vectors + affd->post_vectors;
	if (resv > minvec)
		return 0;
	if (affd->calc_sets)
		set_vecs = maxvec - resv;
	else
		set_vecs = cpumask_nr_online();
	return resv + (set_vecs < maxvec - resv ? set_vecs : maxvec - resv);
}
