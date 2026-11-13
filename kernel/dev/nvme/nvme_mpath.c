// LikeOS - NVMe native multipath: namespace head disks, path selection,
// failover, Asymmetric Namespace Access
//
// A namespace that several controllers of a subsystem attach (or any
// namespace with nvme_core.multipath_always_on) gets ONE visible disk,
// nvme<subsys>n<id>, the "head"; each controller's view of it is a hidden
// path disk nvme<subsys>c<ctrl>n<id>.  The head is a plain request-queue
// disk whose submit operation only routes:
//
//   * path selection by the subsystem's I/O policy -- "numa" (the cached
//     path of the submitting CPU, re-chosen when it stops being optimized;
//     optimized beats non-optimized, nearest controller first), "round-
//     robin" (the next usable path after the last one, optimized first),
//     "queue-depth" (the usable path whose controller has the fewest
//     multipath commands in flight);
//   * a usable path gets a clone of the request (sharing its data list),
//     submitted to the path disk with the multipath flag: an error that is
//     a path error (or the path queue dying) FAILS OVER -- the path request
//     ends quietly and the original goes onto the head's requeue list,
//     which kblockd resubmits through path selection;
//   * no usable path but one that may come back (a controller resetting or
//     connecting, or delayed removal configured): the request waits on the
//     requeue list until a path goes live; no path at all fails it.
//
// ANA: the ANA log (groups and their NSIDs and states) is read when the
// controller is identified, on an ANA change event, after every scan and
// when a command fails with an ANA status.  A group in the "change" state
// arms the ANATT timer (twice the transition time); when it expires the
// controller is reset.  A path becomes live (the head disk is added the
// first time) when its group is optimized or non-optimized and its
// controller is live.
//
// Lists: head->list (the paths) is walked under head->srcu by submitters
// and changed under subsys->lock; ctrl->namespaces is walked under
// ctrl->ns_srcu.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_ns_internal.h>

/* How long a request waits before trying again when a path's queue would
 * not take it right now (frozen, out of request slots). */
#define NVME_MPATH_BUSY_RETRY_MS 10u

static const char *const nvme_iopolicy_names[] = {
	[NVME_IOPOLICY_NUMA] = "numa",
	[NVME_IOPOLICY_RR] = "round-robin",
	[NVME_IOPOLICY_QD] = "queue-depth",
};

static const char *const nvme_ana_state_names[] = {
	[0] = "invalid state",
	[NVME_ANA_OPTIMIZED] = "optimized",
	[NVME_ANA_NONOPTIMIZED] = "non-optimized",
	[NVME_ANA_INACCESSIBLE] = "inaccessible",
	[NVME_ANA_PERSISTENT_LOSS] = "persistent-loss",
	[NVME_ANA_CHANGE] = "change",
};

/* ==========================================================================
 * Knobs and names
 * ========================================================================== */

bool nvme_mpath_always_on_param(void)
{
	return nvme_core_param.multipath_always_on != 0;
}

/* multipath_always_on implies multipath. */
bool nvme_mpath_multipath_param(void)
{
	return nvme_core_param.multipath != 0 || nvme_mpath_always_on_param();
}

bool nvme_mpath_enabled(void)
{
	return nvme_mpath_multipath_param();
}

const char *nvme_iopolicy_name(int iopolicy)
{
	if (iopolicy < 0 || iopolicy > NVME_IOPOLICY_QD)
		return "invalid";
	return nvme_iopolicy_names[iopolicy];
}

static bool nvme_str_starts(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return false;
	return true;
}

int nvme_iopolicy_from_name(const char *name)
{
	if (!name)
		return -EINVAL;
	if (nvme_str_starts(name, "numa"))
		return NVME_IOPOLICY_NUMA;
	if (nvme_str_starts(name, "round-robin"))
		return NVME_IOPOLICY_RR;
	if (nvme_str_starts(name, "queue-depth"))
		return NVME_IOPOLICY_QD;
	return -EINVAL;
}

const char *nvme_ana_state_name(enum nvme_ana_state state)
{
	unsigned int s = (unsigned int)state;

	if (s >= sizeof(nvme_ana_state_names) / sizeof(nvme_ana_state_names[0]) ||
	    !nvme_ana_state_names[s])
		return "invalid state";
	return nvme_ana_state_names[s];
}

void nvme_mpath_default_iopolicy(struct nvme_subsystem *subsys)
{
	int p = nvme_core_param.iopolicy;

	if (p < NVME_IOPOLICY_NUMA || p > NVME_IOPOLICY_QD)
		p = NVME_IOPOLICY_NUMA;
	subsys->iopolicy = (enum nvme_iopolicy)p;
}

static int nvme_subsys_iopolicy(struct nvme_subsystem *subsys)
{
	return (int)__atomic_load_n(&subsys->iopolicy, __ATOMIC_RELAXED);
}

void nvme_subsys_iopolicy_update(struct nvme_subsystem *subsys, int iopolicy)
{
	struct nvme_ctrl *ctrl;
	int old = nvme_subsys_iopolicy(subsys);

	might_sleep();
	if (iopolicy < NVME_IOPOLICY_NUMA || iopolicy > NVME_IOPOLICY_QD ||
	    old == iopolicy)
		return;
	__atomic_store_n(&subsys->iopolicy, (enum nvme_iopolicy)iopolicy,
			 __ATOMIC_RELEASE);

	/* A policy change forgets every cached path by design. */
	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry)
		nvme_mpath_clear_ctrl_paths(ctrl);
	mutex_unlock(&nvme_subsystems_lock);

	kprintf(NVME_LOG_PREFIX
		"subsysnqn %s iopolicy changed from %s to %s\n",
		subsys->subnqn, nvme_iopolicy_name(old),
		nvme_iopolicy_name(iopolicy));
}

bool nvme_ctrl_use_ana(struct nvme_ctrl *ctrl)
{
	return ctrl->ana_log_buf != NULL;
}

/* ==========================================================================
 * Freezing the head disks of a subsystem (pass-through with effects)
 * ========================================================================== */

/* The head disk is a request-queue disk without a tag set: its requests
 * wait on the frozen path queues instead (their clones are refused and
 * retried), so only multi-queue head queues take part here. */
static bool nvme_head_freezable(struct nvme_ns_head *h)
{
	return h->disk && h->disk->queue && blk_queue_is_mq(h->disk->queue);
}

void nvme_mpath_unfreeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);
	list_for_each_entry(h, &subsys->nsheads, entry)
		if (nvme_head_freezable(h))
			blk_mq_unfreeze_queue_non_owner(h->disk->queue);
}

void nvme_mpath_wait_freeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);
	list_for_each_entry(h, &subsys->nsheads, entry)
		if (nvme_head_freezable(h))
			blk_mq_freeze_queue_wait(h->disk->queue);
}

void nvme_mpath_start_freeze(struct nvme_subsystem *subsys)
{
	struct nvme_ns_head *h;

	lockdep_assert_held(&subsys->lock);
	list_for_each_entry(h, &subsys->nsheads, entry)
		if (nvme_head_freezable(h))
			blk_freeze_queue_start_non_owner(h->disk->queue);
}

/* ==========================================================================
 * Path selection
 * ========================================================================== */

static unsigned int nvme_path_cpu(void)
{
	unsigned int cpu = (unsigned int)this_cpu_id();

	return cpu < MAX_CPUS ? cpu : 0;
}

static struct nvme_ns *nvme_current_path(struct nvme_ns_head *head,
					 unsigned int cpu)
{
	return __atomic_load_n(&head->current_path[cpu], __ATOMIC_ACQUIRE);
}

static void nvme_set_current_path(struct nvme_ns_head *head, unsigned int cpu,
				  struct nvme_ns *ns)
{
	__atomic_store_n(&head->current_path[cpu], ns, __ATOMIC_RELEASE);
}

bool nvme_mpath_clear_current_path(struct nvme_ns *ns)
{
	struct nvme_ns_head *head = ns->head;
	bool changed = false;

	if (!head)
		return false;
	for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
		struct nvme_ns *expected = ns;

		if (__atomic_compare_exchange_n(&head->current_path[cpu],
						&expected, NULL, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED))
			changed = true;
	}
	return changed;
}

void nvme_mpath_clear_ctrl_paths(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;
	int idx;

	idx = nvme_srcu_read_lock(&ctrl->ns_srcu);
	nvme_list_for_each_entry_srcu(ns, &ctrl->namespaces, list) {
		nvme_mpath_clear_current_path(ns);
		if (ns->head->disk)
			nvme_queue_work(&ns->head->requeue_work);
	}
	nvme_srcu_read_unlock(&ctrl->ns_srcu, idx);
}

void nvme_kick_requeue_lists(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;
	int idx;

	idx = nvme_srcu_read_lock(&ctrl->ns_srcu);
	nvme_list_for_each_entry_srcu(ns, &ctrl->namespaces, list) {
		if (!ns->head->disk)
			continue;
		nvme_queue_work(&ns->head->requeue_work);
	}
	nvme_srcu_read_unlock(&ctrl->ns_srcu, idx);
}

void nvme_mpath_revalidate_paths(struct nvme_ns *ns)
{
	struct nvme_ns_head *head = ns->head;
	uint64_t capacity;
	struct nvme_ns *p;
	int idx;

	if (!head->disk)
		return;
	capacity = head->disk->capacity;
	idx = nvme_srcu_read_lock(&head->srcu);
	nvme_list_for_each_entry_srcu(p, &head->list, siblings) {
		if (p->disk && capacity != p->disk->capacity)
			nvme_clear_bit(NVME_NS_READY, &p->flags);
	}
	nvme_srcu_read_unlock(&head->srcu, idx);

	for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++)
		nvme_set_current_path(head, cpu, NULL);
	nvme_queue_work(&head->requeue_work);
}

static bool nvme_path_is_disabled(struct nvme_ns *ns)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ns->ctrl);

	/* DELETING is not disabled: I/O can still complete while the
	 * controller is connected, else it fails and comes back to the
	 * requeue list. */
	if (state != NVME_CTRL_LIVE && state != NVME_CTRL_DELETING)
		return true;
	if (nvme_test_bit(NVME_NS_ANA_PENDING, &ns->flags) ||
	    !nvme_test_bit(NVME_NS_READY, &ns->flags))
		return true;
	return false;
}

/* NUMA distance between the submitting CPU's node and the controller's:
 * LikeOS has one memory node, every path is local. */
#define NVME_LOCAL_DISTANCE 10

static int nvme_path_distance(struct nvme_ns_head *head, struct nvme_ns *ns,
			      unsigned int cpu)
{
	(void)head;
	(void)ns;
	(void)cpu;
	return NVME_LOCAL_DISTANCE;
}

static struct nvme_ns *__nvme_find_path(struct nvme_ns_head *head,
					unsigned int cpu)
{
	int found_distance = 0x7fffffff, fallback_distance = 0x7fffffff;
	struct nvme_ns *found = NULL, *fallback = NULL, *ns;
	int distance;

	nvme_list_for_each_entry_srcu(ns, &head->list, siblings) {
		if (nvme_path_is_disabled(ns))
			continue;

		if (nvme_subsys_iopolicy(head->subsys) == NVME_IOPOLICY_NUMA)
			distance = nvme_path_distance(head, ns, cpu);
		else
			distance = NVME_LOCAL_DISTANCE;

		switch (nvme_ns_ana_state(ns)) {
		case NVME_ANA_OPTIMIZED:
			if (distance < found_distance) {
				found_distance = distance;
				found = ns;
			}
			break;
		case NVME_ANA_NONOPTIMIZED:
			if (distance < fallback_distance) {
				fallback_distance = distance;
				fallback = ns;
			}
			break;
		default:
			break;
		}
	}

	if (!found)
		found = fallback;
	if (found)
		nvme_set_current_path(head, cpu, found);
	return found;
}

/* The path after `ns' in head->list, wrapping around (NULL: empty). */
static struct nvme_ns *nvme_next_ns(struct nvme_ns_head *head,
				    struct nvme_ns *ns)
{
	struct list_head *next = nvme_list_next_srcu(&ns->siblings);

	if (next == &head->list)
		next = nvme_list_next_srcu(&head->list);
	if (next == &head->list)
		return NULL;
	return list_entry(next, struct nvme_ns, siblings);
}

static bool nvme_head_list_singular(struct nvme_ns_head *head)
{
	struct list_head *first = nvme_list_next_srcu(&head->list);

	return first != &head->list &&
	       nvme_list_next_srcu(first) == &head->list;
}

static struct nvme_ns *nvme_round_robin_path(struct nvme_ns_head *head)
{
	unsigned int cpu = nvme_path_cpu();
	struct nvme_ns *ns, *found = NULL;
	struct nvme_ns *old = nvme_current_path(head, cpu);

	if (unlikely(!old))
		return __nvme_find_path(head, cpu);

	if (nvme_head_list_singular(head)) {
		if (nvme_path_is_disabled(old))
			return NULL;
		return old;
	}

	for (ns = nvme_next_ns(head, old); ns && ns != old;
	     ns = nvme_next_ns(head, ns)) {
		if (nvme_path_is_disabled(ns))
			continue;
		if (nvme_ns_ana_state(ns) == NVME_ANA_OPTIMIZED) {
			found = ns;
			goto out;
		}
		if (nvme_ns_ana_state(ns) == NVME_ANA_NONOPTIMIZED)
			found = ns;
	}

	/* The loop skipped the current path: keep it when it is optimized
	 * and no other optimized one exists, or when it is the only usable
	 * one. */
	if (!nvme_path_is_disabled(old) &&
	    (nvme_ns_ana_state(old) == NVME_ANA_OPTIMIZED ||
	     (!found && nvme_ns_ana_state(old) == NVME_ANA_NONOPTIMIZED)))
		return old;

	if (!found)
		return NULL;
out:
	nvme_set_current_path(head, cpu, found);
	return found;
}

static struct nvme_ns *nvme_queue_depth_path(struct nvme_ns_head *head)
{
	struct nvme_ns *best_opt = NULL, *best_nonopt = NULL, *ns;
	unsigned int min_depth_opt = 0xffffffffu, min_depth_nonopt = 0xffffffffu;
	unsigned int depth;

	nvme_list_for_each_entry_srcu(ns, &head->list, siblings) {
		if (nvme_path_is_disabled(ns))
			continue;

		depth = (unsigned int)__atomic_load_n(&ns->ctrl->nr_active,
						      __ATOMIC_RELAXED);
		switch (nvme_ns_ana_state(ns)) {
		case NVME_ANA_OPTIMIZED:
			if (depth < min_depth_opt) {
				min_depth_opt = depth;
				best_opt = ns;
			}
			break;
		case NVME_ANA_NONOPTIMIZED:
			if (depth < min_depth_nonopt) {
				min_depth_nonopt = depth;
				best_nonopt = ns;
			}
			break;
		default:
			break;
		}
		if (min_depth_opt == 0)
			return best_opt;
	}
	return best_opt ? best_opt : best_nonopt;
}

static bool nvme_path_is_optimized(struct nvme_ns *ns)
{
	return nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE &&
	       nvme_ns_ana_state(ns) == NVME_ANA_OPTIMIZED;
}

static struct nvme_ns *nvme_numa_path(struct nvme_ns_head *head)
{
	unsigned int cpu = nvme_path_cpu();
	struct nvme_ns *ns = nvme_current_path(head, cpu);

	if (unlikely(!ns))
		return __nvme_find_path(head, cpu);
	if (unlikely(!nvme_path_is_optimized(ns)))
		return __nvme_find_path(head, cpu);
	return ns;
}

struct nvme_ns *nvme_find_path(struct nvme_ns_head *head)
{
	switch (nvme_subsys_iopolicy(head->subsys)) {
	case NVME_IOPOLICY_QD:
		return nvme_queue_depth_path(head);
	case NVME_IOPOLICY_RR:
		return nvme_round_robin_path(head);
	default:
		return nvme_numa_path(head);
	}
}

static bool nvme_available_path(struct nvme_ns_head *head)
{
	struct nvme_ns *ns;

	if (!nvme_test_bit(NVME_NSHEAD_DISK_LIVE, &head->flags))
		return false;

	nvme_list_for_each_entry_srcu(ns, &head->list, siblings) {
		if (nvme_test_bit(NVME_CTRL_FAILFAST_EXPIRED, &ns->ctrl->flags))
			continue;
		switch (nvme_ctrl_state(ns->ctrl)) {
		case NVME_CTRL_LIVE:
		case NVME_CTRL_RESETTING:
		case NVME_CTRL_CONNECTING:
			return true;
		default:
			break;
		}
	}
	/* delayed_removal_secs: wait for a path to come back instead of
	 * failing at once. */
	return nvme_mpath_queue_if_no_path(head);
}

/* ==========================================================================
 * Request accounting on the paths
 * ========================================================================== */

void nvme_mpath_start_request(struct blk_request *rq)
{
	struct nvme_ns *ns = (struct nvme_ns *)blk_queue_get_queuedata(rq->queue);
	struct nvme_request *nr = nvme_req(rq);

	if (!ns)
		return;
	if (nvme_subsys_iopolicy(ns->head->subsys) == NVME_IOPOLICY_QD &&
	    !(nr->flags & NVME_MPATH_CNT_ACTIVE)) {
		__atomic_add_fetch(&ns->ctrl->nr_active, 1, __ATOMIC_RELAXED);
		/* The flags byte is shared with bits set concurrently (a
		 * user cancel): every update of it is atomic. */
		__atomic_or_fetch(&nr->flags, (uint8_t)NVME_MPATH_CNT_ACTIVE,
				  __ATOMIC_RELAXED);
	}
	/* The head disk accounts its own requests (it is a request-queue
	 * disk): no separate statistics here. */
}

void nvme_mpath_end_request(struct blk_request *rq)
{
	struct nvme_ns *ns = (struct nvme_ns *)blk_queue_get_queuedata(rq->queue);
	struct nvme_request *nr = nvme_req(rq);
	int v;

	if (!ns || !(nr->flags & NVME_MPATH_CNT_ACTIVE))
		return;
	__atomic_and_fetch(&nr->flags, (uint8_t)~NVME_MPATH_CNT_ACTIVE,
			   __ATOMIC_RELAXED);
	/* Decrement if positive. */
	v = __atomic_load_n(&ns->ctrl->nr_active, __ATOMIC_RELAXED);
	while (v > 0 &&
	       !__atomic_compare_exchange_n(&ns->ctrl->nr_active, &v, v - 1,
					    false, __ATOMIC_RELAXED,
					    __ATOMIC_RELAXED))
		;
}

/* ==========================================================================
 * The head disk: submission, requeue, failover
 * ========================================================================== */

static void nvme_mpath_clone_end_io(struct blk_request *clone, int status);

/* Put `orig' on the head's requeue list; kick it now or after `delay_ms'
 * (0: leave it for the next kick -- a path going live). */
static void nvme_head_requeue(struct nvme_ns_head *head,
			      struct blk_request *orig, bool kick,
			      unsigned int delay_ms)
{
	uint64_t flags;

	orig->rq_next = NULL;
	spin_lock_irqsave(&head->requeue_lock, &flags);
	if (head->requeue_tail)
		head->requeue_tail->rq_next = orig;
	else
		head->requeue_head = orig;
	head->requeue_tail = orig;
	head->nr_requeued++;
	spin_unlock_irqrestore(&head->requeue_lock, flags);
	if (!kick)
		return;
	if (delay_ms)
		nvme_queue_delayed_work(&head->requeue_work, delay_ms);
	else
		nvme_queue_work(&head->requeue_work);
}

/* End a head request (it is at the head disk's "driver": us). */
static void nvme_head_end(struct blk_request *orig, int status)
{
	blk_complete_request(orig, status,
			     status ? (uint32_t)blk_rq_bytes(orig) : 0);
}

enum nvme_head_route {
	NVME_ROUTE_SUBMITTED, /* a clone went to a path */
	NVME_ROUTE_QUEUED, /* waiting on the requeue list */
	NVME_ROUTE_BUSY, /* try again shortly */
	NVME_ROUTE_FAILED, /* end it with *err */
};

/* Clone `orig' onto path `ns'. */
static enum nvme_head_route nvme_mpath_submit_clone(struct nvme_ns_head *head,
						    struct nvme_ns *ns,
						    struct blk_request *orig,
						    int *err)
{
	struct blk_request *clone;
	int ret;

	clone = blk_request_alloc(&ns->disk->bdev, orig->op, orig->sector,
				  orig->count);
	if (!clone)
		return NVME_ROUTE_BUSY;
	/* The data list is the original's (the original outlives the
	 * clone); never freed through the clone. */
	clone->sg = orig->sg;
	clone->nsg = orig->nsg;
	clone->sg_cap = orig->nsg;
	clone->timeout_ms = orig->timeout_ms;
	clone->write_hint = orig->write_hint;
	clone->write_stream = orig->write_stream;
	/* The clone may not wait for the path queue (this runs from the
	 * head's dispatch); refused, the original is retried. */
	clone->flags = (orig->flags & ~(uint32_t)(BLK_REQ_HEAD |
						 BLK_REQ_DONTPREP |
						 BLK_REQ_MQ_ALLOC |
						 BLK_REQ_RESERVED)) |
		       BLK_REQ_MPATH | BLK_REQ_NOWAIT;
	if (orig->flags & BLK_REQ_INTEGRITY)
		clone->integrity = orig->integrity;
	clone->private_data = orig;
	clone->end_io_data = head;
	clone->end_io = nvme_mpath_clone_end_io;

	ret = blk_submit(clone);
	if (!ret)
		return NVME_ROUTE_SUBMITTED;

	/* Refused before it started: end_io will not run. */
	blk_request_free(clone);
	switch (ret) {
	case -EAGAIN:
	case -EBUSY:
	case -ENOMEM:
		return NVME_ROUTE_BUSY;
	case -EIO:
		/* Refused with -EIO only when the path disk's device is
		 * gone (blk_mark_disk_dead): the same as going away. */
		if (!(disk_flags(ns->disk) & GENHD_FL_GONE)) {
			*err = ret;
			return NVME_ROUTE_FAILED;
		}
		/* fall through */
	case -ENODEV:
	case -ENXIO:
		/* The path disk is going away: forget it and try again
		 * shortly (its READY bit is already clear, so the next
		 * selection takes another path or none). */
		nvme_mpath_clear_current_path(ns);
		return NVME_ROUTE_BUSY;
	default:
		*err = ret;
		return NVME_ROUTE_FAILED;
	}
}

/* Choose a path for `orig' and hand it over.  `from_work': the requeue
 * work (process context) rather than the head queue's dispatch. */
static enum nvme_head_route nvme_ns_head_route(struct nvme_ns_head *head,
					       struct blk_request *orig,
					       int *err)
{
	enum nvme_head_route r;
	struct nvme_ns *ns;
	int idx;

	*err = 0;
	if (blk_rq_is_passthrough(orig)) {
		*err = -EOPNOTSUPP;
		return NVME_ROUTE_FAILED;
	}
	idx = nvme_srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (likely(ns)) {
		r = nvme_mpath_submit_clone(head, ns, orig, err);
	} else if (nvme_available_path(head)) {
		nvme_pr_ratelimited(head->disk->disk_name,
				    "no usable path - requeuing I/O\n");
		r = NVME_ROUTE_QUEUED;
	} else {
		nvme_pr_ratelimited(head->disk->disk_name,
				    "no available path - failing I/O\n");
		*err = -EIO;
		r = NVME_ROUTE_FAILED;
	}
	nvme_srcu_read_unlock(&head->srcu, idx);
	return r;
}

/* blk_ops->submit of the head disk (any context, no sleeping). */
static int nvme_ns_head_submit(struct gendisk *disk, struct blk_request *req)
{
	struct nvme_ns_head *head = (struct nvme_ns_head *)disk->private_data;
	int err;

	BUG_ON(!head);
	switch (nvme_ns_head_route(head, req, &err)) {
	case NVME_ROUTE_SUBMITTED:
		return 0;
	case NVME_ROUTE_QUEUED:
		nvme_head_requeue(head, req, false, 0);
		return 0;
	case NVME_ROUTE_BUSY:
		/* The head queue dispatches it again shortly. */
		return -EBUSY;
	case NVME_ROUTE_FAILED:
	default:
		return err ? err : -EIO;
	}
}

static void nvme_mpath_clone_end_io(struct blk_request *clone, int status)
{
	uintptr_t v = (uintptr_t)clone->private_data;
	struct blk_request *orig = (struct blk_request *)(v & ~(uintptr_t)1);
	struct nvme_ns_head *head = (struct nvme_ns_head *)clone->end_io_data;

	BUG_ON(!orig || !head);
	if (v & 1) {
		/* Failed over: the original takes another path. */
		nvme_head_requeue(head, orig, true, 0);
	} else if ((status == -EAGAIN || status == -EBUSY) &&
		   !(orig->flags & BLK_REQ_NOWAIT)) {
		/* Our own no-wait refusal (the path queue was frozen or
		 * full): not the submitter's business, try again. */
		nvme_head_requeue(head, orig, true, NVME_MPATH_BUSY_RETRY_MS);
	} else {
		if (orig->op == BLK_OP_ZONE_APPEND)
			orig->append_sector = clone->append_sector;
		nvme_head_end(orig, status);
	}
	blk_request_free(clone);
}

void nvme_failover_req(struct blk_request *rq)
{
	struct nvme_ns *ns = (struct nvme_ns *)blk_queue_get_queuedata(rq->queue);
	uint16_t status = nvme_req(rq)->status & NVME_SCT_SC_MASK;

	BUG_ON(!ns);
	nvme_mpath_clear_current_path(ns);

	/* An ANA error: the controller is alive but not serving this
	 * namespace; re-read the ANA log and use any other path now. */
	if (nvme_is_ana_error(status) && ns->ctrl->ana_log_buf) {
		nvme_set_bit(NVME_NS_ANA_PENDING, &ns->flags);
		nvme_queue_work(&ns->ctrl->ana_work);
	}

	if (WARN_ON_ONCE(rq->end_io != nvme_mpath_clone_end_io)) {
		/* Not a request of the head (split or submitted to the path
		 * directly): nothing to move, it fails here. */
		nvme_end_req(rq);
		return;
	}
	/* The path request ends quietly; its end_io moves the original to
	 * the requeue list. */
	rq->private_data = (void *)((uintptr_t)rq->private_data | 1);
	nvme_req(rq)->status = 0;
	nvme_end_req(rq);
}

static void nvme_requeue_work(void *arg)
{
	struct nvme_ns_head *head = (struct nvme_ns_head *)arg;
	struct blk_request *rq, *next;
	uint64_t flags;
	int err;

	spin_lock_irqsave(&head->requeue_lock, &flags);
	next = head->requeue_head;
	head->requeue_head = NULL;
	head->requeue_tail = NULL;
	spin_unlock_irqrestore(&head->requeue_lock, flags);

	while ((rq = next) != NULL) {
		next = rq->rq_next;
		rq->rq_next = NULL;
		switch (nvme_ns_head_route(head, rq, &err)) {
		case NVME_ROUTE_SUBMITTED:
			break;
		case NVME_ROUTE_QUEUED:
			/* Until a path goes live (it kicks the list). */
			nvme_head_requeue(head, rq, false, 0);
			break;
		case NVME_ROUTE_BUSY:
			nvme_head_requeue(head, rq, true,
					  NVME_MPATH_BUSY_RETRY_MS);
			break;
		case NVME_ROUTE_FAILED:
		default:
			nvme_head_end(rq, err ? err : -EIO);
			break;
		}
	}
}

/* ==========================================================================
 * The head disk: operations, creation, removal
 * ========================================================================== */

static struct nvme_ns_head *nvme_bdev_head(block_device_t *bdev)
{
	return bdev && bdev->disk ?
		       (struct nvme_ns_head *)bdev->disk->private_data :
		       NULL;
}

static int nvme_ns_head_open(block_device_t *bdev, int flags)
{
	struct nvme_ns_head *head = nvme_bdev_head(bdev);

	(void)flags;
	if (!head || !nvme_tryget_ns_head(head))
		return -ENXIO;
	return 0;
}

static void nvme_ns_head_release(block_device_t *bdev)
{
	struct nvme_ns_head *head = nvme_bdev_head(bdev);

	if (head)
		nvme_put_ns_head(head);
}

static long nvme_ns_head_ioctl_mode(block_device_t *bdev, unsigned long cmd,
				    void *argp, struct task *cur,
				    int open_flags)
{
	struct nvme_ns_head *head = nvme_bdev_head(bdev);

	if (!head)
		return -ENXIO;
	return nvme_ns_head_ioctl(head, cmd, argp, cur,
				  nvme_ioctl_open_flags(bdev, open_flags));
}

static int nvme_ns_head_get_unique_id(struct gendisk *disk, uint8_t id[16],
				      int type)
{
	struct nvme_ns_head *head = (struct nvme_ns_head *)disk->private_data;
	struct nvme_ns *ns;
	int idx, ret = -EWOULDBLOCK;

	idx = nvme_srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (ns)
		ret = nvme_ns_get_unique_id(ns, id, type);
	nvme_srcu_read_unlock(&head->srcu, idx);
	return ret;
}

static int nvme_ns_head_report_zones(struct gendisk *disk, uint64_t sector,
				     unsigned int nr_zones, report_zones_cb cb,
				     void *data)
{
	struct nvme_ns_head *head = (struct nvme_ns_head *)disk->private_data;
	struct nvme_ns *ns;
	int idx, ret = -EWOULDBLOCK;

	idx = nvme_srcu_read_lock(&head->srcu);
	ns = nvme_find_path(head);
	if (ns)
		ret = nvme_ns_report_zones(ns, sector, nr_zones, cb, data);
	nvme_srcu_read_unlock(&head->srcu, idx);
	return ret;
}

const struct blk_ops nvme_ns_head_ops = {
	.name = "nvme-mpath",
	.submit = nvme_ns_head_submit,
	.open = nvme_ns_head_open,
	.release = nvme_ns_head_release,
	.ioctl_mode = nvme_ns_head_ioctl_mode,
	.report_zones = nvme_ns_head_report_zones,
	.pr_ops = &nvme_pr_ops,
	.get_unique_id = nvme_ns_head_get_unique_id,
};

/* The partition scan of the head disk runs here, never from the
 * controller's scan work: its reads may wait for a path that only the
 * scan work could bring back. */
static void nvme_partition_scan_work(void *arg)
{
	struct nvme_ns_head *head = (struct nvme_ns_head *)arg;
	struct gendisk *disk = head->disk;

	if (WARN_ON_ONCE(!disk ||
			 !(disk_clear_flags(disk, GENHD_FL_SUPPRESS_PART_SCAN) &
			   GENHD_FL_SUPPRESS_PART_SCAN)))
		return;
	/* A head without a block interface keeps no partitions. */
	if (disk_flags(disk) & GENHD_FL_HIDDEN)
		return;
	if ((disk_flags(disk) & (GENHD_FL_UP | GENHD_FL_DEAD)) == GENHD_FL_UP)
		blk_partition_rescan(disk);
}

static void nvme_remove_head(struct nvme_ns_head *head)
{
	might_sleep();
	if (nvme_test_and_clear_bit(NVME_NSHEAD_DISK_LIVE, &head->flags)) {
		/* With DISK_LIVE clear the requeued I/O fails. */
		nvme_queue_work(&head->requeue_work);
		nvme_cdev_del_head(head);
		nvme_srcu_synchronize(&head->srcu);
		disk_del(head->disk);
	}
	nvme_put_ns_head(head);
}

static void nvme_remove_head_work(void *arg)
{
	struct nvme_ns_head *head = (struct nvme_ns_head *)arg;
	bool remove = false;

	mutex_lock(&head->subsys->lock);
	if (list_empty(&head->list)) {
		list_del_init(&head->entry);
		remove = true;
	}
	__atomic_store_n(&head->remove_work_pending, 0, __ATOMIC_RELEASE);
	mutex_unlock(&head->subsys->lock);
	if (remove)
		nvme_remove_head(head);
}

void nvme_mpath_set_delayed_removal(struct nvme_ns_head *head,
				    unsigned int secs)
{
	mutex_lock(&head->subsys->lock);
	head->delayed_removal_secs = secs;
	if (secs)
		nvme_set_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags);
	else
		nvme_clear_bit(NVME_NSHEAD_QUEUE_IF_NO_PATH, &head->flags);
	mutex_unlock(&head->subsys->lock);
	/* The new flag is seen by every path selection after this. */
	nvme_srcu_synchronize(&head->srcu);
}

int nvme_mpath_alloc_disk(struct nvme_ctrl *ctrl, struct nvme_ns_head *head)
{
	struct blk_limits *lim;
	struct gendisk *disk;
	char name[DISK_NAME_LEN];
	int ret;

	mutex_init(&head->lock, "nvme_head");
	spinlock_init(&head->requeue_lock, "nvme_head_requeue");
	head->requeue_head = NULL;
	head->requeue_tail = NULL;
	blk_work_init(&head->requeue_work, nvme_requeue_work, head);
	blk_work_init(&head->partition_scan_work, nvme_partition_scan_work,
		      head);
	blk_work_init(&head->remove_work, nvme_remove_head_work, head);
	head->delayed_removal_secs = 0;

	/*
	 * multipath_always_on: a head disk for every namespace (private
	 * ones too, when their NSID is unique).  Otherwise only for
	 * subsystems with several controllers, with multipath on.
	 */
	if (!nvme_mpath_always_on_param()) {
		if (!(ctrl->subsys->cmic & NVME_CTRL_CMIC_MULTI_CTRL) ||
		    !nvme_mpath_multipath_param())
			return 0;
	}
	if (!nvme_is_unique_nsid(ctrl, head))
		return 0;

	ret = ksnprintf(name, sizeof(name), "nvme%dn%d", ctrl->subsys->instance,
			head->instance);
	if (ret < 0 || ret >= (int)sizeof(name))
		return -ENAMETOOLONG;
	disk = disk_alloc_named(BLK_DISK_NVME, &nvme_ns_head_ops, head, name);
	if (!disk)
		return -ENOMEM;
	disk->private_data = head;

	/* Stacking defaults: no limit until the paths stack theirs in. */
	lim = &disk->limits;
	kmemset(lim, 0, sizeof(*lim));
	lim->logical_block_size = 512;
	lim->physical_block_size = 512;
	lim->max_hw_sectors = 0xffffffffu;
	lim->max_sectors = 0xffffffffu;
	lim->max_dev_sectors = 0xffffffffu;
	lim->max_segments = 0xffff;
	lim->max_segment_size = 0xffffffffu;
	lim->seg_boundary_mask = ~0ULL;
	lim->dma_alignment = 3;
	lim->queue_depth = BLK_QUEUE_MAX_TAGS;
	lim->features = BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT | BLK_FEAT_POLL |
			BLK_FEAT_ATOMIC_WRITES;
	if (head->ids.csi == NVME_CSI_ZNS)
		lim->features |= BLK_FEAT_ZONED;

	/* The partition scan comes from partition_scan_work. */
	disk->flags |= GENHD_FL_SUPPRESS_PART_SCAN | GENHD_FL_MEDIA_PRESENT;
	if (blk_kblockd_running())
		disk->flags |= GENHD_FL_HOTPLUGGED;
	disk_get(disk); /* the head's own reference (nvme_mpath_put_disk) */
	head->disk = disk;
	nvme_fill_disk_identity(ctrl, head, disk);
	/* The disk's reference on the head. */
	nvme_tryget_ns_head(head);
	return 0;
}

static void nvme_mpath_set_live(struct nvme_ns *ns)
{
	struct nvme_ns_head *head = ns->head;

	might_sleep();
	if (!head->disk)
		return;

	/* Two paths may go live at once: one adds the disk. */
	if (!nvme_test_and_set_bit(NVME_NSHEAD_DISK_LIVE, &head->flags)) {
		head->disk->bdev.ready = 1;
		if (disk_add(head->disk)) {
			nvme_clear_bit(NVME_NSHEAD_DISK_LIVE, &head->flags);
			return;
		}
		nvme_set_bit(NVME_NSHEAD_DISK_ADDED, &head->flags);
		nvme_cdev_add_head(head);
		nvme_queue_work(&head->partition_scan_work);
	}

	nvme_set_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags);

	mutex_lock(&head->lock);
	if (nvme_path_is_optimized(ns)) {
		int idx = nvme_srcu_read_lock(&head->srcu);

		for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++)
			__nvme_find_path(head, cpu);
		nvme_srcu_read_unlock(&head->srcu, idx);
	}
	mutex_unlock(&head->lock);

	nvme_srcu_synchronize(&head->srcu);
	nvme_queue_work(&head->requeue_work);
}

void nvme_mpath_remove_sysfs_link(struct nvme_ns *ns)
{
	/* The head's multipath/ directory entries are the sysfs part's
	 * (nvme_sysfs_del_ns); only the bookkeeping is ours. */
	nvme_clear_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags);
}

void nvme_mpath_remove_disk(struct nvme_ns_head *head)
{
	bool remove = false;

	might_sleep();
	if (!head->disk)
		return;

	mutex_lock(&head->subsys->lock);
	/* A new path may have attached since the last one went
	 * (nvme_init_ns_head racing nvme_ns_remove). */
	if (!list_empty(&head->list))
		goto out;

	if (head->delayed_removal_secs) {
		__atomic_store_n(&head->remove_work_pending, 1,
				 __ATOMIC_RELEASE);
		nvme_queue_delayed_work(&head->remove_work,
					(uint64_t)head->delayed_removal_secs *
						1000u);
	} else {
		list_del_init(&head->entry);
		remove = true;
	}
out:
	mutex_unlock(&head->subsys->lock);
	if (remove)
		nvme_remove_head(head);
}

void nvme_mpath_put_disk(struct nvme_ns_head *head)
{
	struct gendisk *disk = head->disk;

	might_sleep();
	if (!disk)
		return;
	/* Every pending request is resubmitted (and failed). */
	nvme_queue_work(&head->requeue_work);
	nvme_flush_work(&head->requeue_work);
	nvme_flush_work(&head->partition_scan_work);
	nvme_cancel_work_sync(&head->remove_work);
	WARN_ON(head->requeue_head != NULL);
	head->disk = NULL;
	disk_put(disk); /* the head's reference */
	if (!nvme_test_bit(NVME_NSHEAD_DISK_ADDED, &head->flags))
		disk_put(disk); /* never added: the allocation's */
}

/* ==========================================================================
 * Head limits
 * ========================================================================== */

static uint64_t nvme_min_nz(uint64_t a, uint64_t b)
{
	if (!a)
		return b;
	if (!b)
		return a;
	return a < b ? a : b;
}

#define NVME_FEAT_INHERIT                                                    \
	(BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA | BLK_FEAT_ROTATIONAL |        \
	 BLK_FEAT_STABLE_WRITES | BLK_FEAT_ZONED)

void nvme_mpath_stack_limits(struct nvme_ns *ns, bool unsupported,
			     uint64_t capacity, bool ro)
{
	struct nvme_ns_head *head = ns->head;
	struct gendisk *hd = head->disk;
	const struct blk_limits *nl = &ns->disk->limits;
	struct blk_limits lim;
	bool restack;

	might_sleep();
	if (!hd)
		return;
	if (hd->queue)
		queue_limits_start_update(hd->queue, &lim);
	else
		lim = hd->limits;

	/*
	 * The device configuration (block sizes, I/O hints) is this path's:
	 * it may change with a format.  The splitting limits are the least
	 * common denominator of all paths -- counted in logical blocks, so
	 * they start over when the block size changes.
	 */
	restack = lim.logical_block_size != nl->logical_block_size;
	lim.logical_block_size = nl->logical_block_size;
	lim.physical_block_size = nl->physical_block_size;
	lim.io_min = nl->io_min;
	lim.io_opt = nl->io_opt;
	if (restack) {
		lim.max_hw_sectors = nl->max_hw_sectors;
		lim.max_sectors = nl->max_sectors;
		lim.max_dev_sectors = nl->max_dev_sectors;
		lim.max_discard_sectors = nl->max_discard_sectors;
		lim.max_hw_discard_sectors = nl->max_hw_discard_sectors;
		lim.max_write_zeroes_sectors = nl->max_write_zeroes_sectors;
		lim.max_hw_zone_append_sectors = nl->max_hw_zone_append_sectors;
		lim.atomic_write_hw_max = nl->atomic_write_hw_max;
		lim.atomic_write_hw_unit_max = nl->atomic_write_hw_unit_max;
		lim.atomic_write_hw_unit_min = nl->atomic_write_hw_unit_min;
		lim.atomic_write_hw_boundary = nl->atomic_write_hw_boundary;
	} else {
		lim.max_hw_sectors = (uint32_t)nvme_min_nz(lim.max_hw_sectors,
							   nl->max_hw_sectors);
		lim.max_sectors = (uint32_t)nvme_min_nz(lim.max_sectors,
							nl->max_sectors);
		lim.max_dev_sectors = (uint32_t)nvme_min_nz(
			lim.max_dev_sectors, nl->max_dev_sectors);
		/* Discard / write zeroes: every path must support them. */
		lim.max_discard_sectors =
			lim.max_discard_sectors < nl->max_discard_sectors ?
				lim.max_discard_sectors :
				nl->max_discard_sectors;
		lim.max_hw_discard_sectors =
			lim.max_hw_discard_sectors < nl->max_hw_discard_sectors ?
				lim.max_hw_discard_sectors :
				nl->max_hw_discard_sectors;
		lim.max_write_zeroes_sectors =
			lim.max_write_zeroes_sectors <
					nl->max_write_zeroes_sectors ?
				lim.max_write_zeroes_sectors :
				nl->max_write_zeroes_sectors;
		lim.max_hw_zone_append_sectors = (uint32_t)nvme_min_nz(
			lim.max_hw_zone_append_sectors,
			nl->max_hw_zone_append_sectors);
		lim.atomic_write_hw_max = nl->atomic_write_hw_max <
							  lim.atomic_write_hw_max ?
						  nl->atomic_write_hw_max :
						  lim.atomic_write_hw_max;
		lim.atomic_write_hw_unit_max =
			nl->atomic_write_hw_unit_max <
					lim.atomic_write_hw_unit_max ?
				nl->atomic_write_hw_unit_max :
				lim.atomic_write_hw_unit_max;
		lim.atomic_write_hw_unit_min =
			nl->atomic_write_hw_unit_min >
					lim.atomic_write_hw_unit_min ?
				nl->atomic_write_hw_unit_min :
				lim.atomic_write_hw_unit_min;
		lim.atomic_write_hw_boundary = (uint32_t)nvme_min_nz(
			lim.atomic_write_hw_boundary,
			nl->atomic_write_hw_boundary);
	}
	lim.max_segments = (uint32_t)nvme_min_nz(lim.max_segments,
						 nl->max_segments);
	lim.max_segment_size = (uint32_t)nvme_min_nz(lim.max_segment_size,
						      nl->max_segment_size);
	lim.virt_boundary_mask |= nl->virt_boundary_mask;
	lim.seg_boundary_mask = lim.seg_boundary_mask & nl->seg_boundary_mask;
	if (nl->dma_alignment > lim.dma_alignment)
		lim.dma_alignment = nl->dma_alignment;
	lim.discard_granularity = nl->discard_granularity;
	lim.discard_alignment = nl->discard_alignment;
	lim.max_discard_segments = (uint32_t)nvme_min_nz(
		lim.max_discard_segments, nl->max_discard_segments);
	lim.chunk_sectors = nl->chunk_sectors;
	lim.max_open_zones = nl->max_open_zones;
	lim.max_active_zones = nl->max_active_zones;
	lim.queue_depth = BLK_QUEUE_MAX_TAGS;
	lim.features = (lim.features & ~(uint32_t)NVME_FEAT_INHERIT) |
		       (nl->features & NVME_FEAT_INHERIT);
	if (!(nl->features & BLK_FEAT_ATOMIC_WRITES))
		lim.features &= ~(uint32_t)BLK_FEAT_ATOMIC_WRITES;
	lim.write_cache = nl->write_cache;
	lim.fua = nl->fua;
	lim.rotational = nl->rotational;
	lim.discard_zeroes_data = nl->discard_zeroes_data;

	if (unsupported)
		disk_set_flags(hd, GENHD_FL_HIDDEN | GENHD_FL_NO_PART_SCAN);
	else
		lim.integrity = nl->integrity;
	lim.max_write_streams = nl->max_write_streams;
	lim.write_stream_granularity = nl->write_stream_granularity;

	if (nvme_ns_commit_limits(hd, &lim))
		nvme_ctrl_warn(ns->ctrl, "%s: path limits rejected\n",
			       hd->disk_name);
	nvme_ns_set_capacity(hd, capacity, lim.logical_block_size);
	disk_set_ro(hd, ro);
	nvme_ns_fill_bdev(hd, &lim, ro);
	if (nvme_is_power_of_2(head->zsze) && (lim.features & BLK_FEAT_ZONED))
		hd->nr_zones = ns->disk->nr_zones;
	nvme_mpath_revalidate_paths(ns);
}

/* ==========================================================================
 * Asymmetric Namespace Access
 * ========================================================================== */

/*
 * Walk the group descriptors of an ANA log: check each one and call `cb'.
 * Pure (host-tested).  0, the callback's non-zero return, or -EINVAL for a
 * malformed log.
 */
int nvme_ana_log_walk(const void *buf, size_t size, uint32_t anagrpmax,
		      int (*cb)(const struct nvme_ana_group_desc *desc,
				void *data),
		      void *data);
int nvme_ana_log_walk(const void *buf, size_t size, uint32_t anagrpmax,
		      int (*cb)(const struct nvme_ana_group_desc *desc,
				void *data),
		      void *data)
{
	const struct nvme_ana_rsp_hdr *hdr = (const struct nvme_ana_rsp_hdr *)buf;
	size_t offset = sizeof(struct nvme_ana_rsp_hdr);
	unsigned int ngrps;
	int error;

	if (size < sizeof(*hdr))
		return -EINVAL;
	ngrps = le16_to_cpu(hdr->ngrps);
	for (unsigned int i = 0; i < ngrps; i++) {
		const struct nvme_ana_group_desc *desc;
		uint32_t nr_nsids;
		size_t nsid_buf_size;

		if (WARN_ON_ONCE(size < sizeof(*desc) ||
				 offset > size - sizeof(*desc)))
			return -EINVAL;
		desc = (const struct nvme_ana_group_desc *)((const uint8_t *)buf +
							    offset);
		nr_nsids = le32_to_cpu(desc->nnsids);
		nsid_buf_size = (size_t)nr_nsids * sizeof(__le32);

		if (WARN_ON_ONCE(desc->grpid == 0))
			return -EINVAL;
		if (WARN_ON_ONCE(le32_to_cpu(desc->grpid) > anagrpmax))
			return -EINVAL;
		if (WARN_ON_ONCE(desc->state == 0))
			return -EINVAL;
		if (WARN_ON_ONCE(desc->state > NVME_ANA_CHANGE))
			return -EINVAL;

		offset += sizeof(*desc);
		if (WARN_ON_ONCE(nsid_buf_size > size ||
				 offset > size - nsid_buf_size))
			return -EINVAL;

		error = cb(desc, data);
		if (error)
			return error;
		offset += nsid_buf_size;
	}
	return 0;
}

struct nvme_ana_cb_ctx {
	struct nvme_ctrl *ctrl;
	void *data;
	int (*fn)(struct nvme_ctrl *ctrl, const struct nvme_ana_group_desc *d,
		  void *data);
};

static int nvme_ana_cb_tramp(const struct nvme_ana_group_desc *desc,
			     void *data)
{
	struct nvme_ana_cb_ctx *c = (struct nvme_ana_cb_ctx *)data;

	return c->fn(c->ctrl, desc, c->data);
}

static int nvme_parse_ana_log(struct nvme_ctrl *ctrl, void *data,
			      int (*cb)(struct nvme_ctrl *ctrl,
					const struct nvme_ana_group_desc *d,
					void *data))
{
	struct nvme_ana_cb_ctx c = { .ctrl = ctrl, .data = data, .fn = cb };

	lockdep_assert_held(&ctrl->ana_lock);
	return nvme_ana_log_walk(ctrl->ana_log_buf, ctrl->ana_log_size,
				 ctrl->anagrpmax, nvme_ana_cb_tramp, &c);
}

static bool nvme_state_is_live(enum nvme_ana_state state)
{
	return state == NVME_ANA_OPTIMIZED || state == NVME_ANA_NONOPTIMIZED;
}

static void nvme_update_ns_ana_state(const struct nvme_ana_group_desc *desc,
				     struct nvme_ns *ns)
{
	__atomic_store_n(&ns->ana_grpid, le32_to_cpu(desc->grpid),
			 __ATOMIC_RELAXED);
	nvme_ns_set_ana_state(ns, (enum nvme_ana_state)(desc->state &
							 NVME_ANA_STATE_MASK));
	nvme_clear_bit(NVME_NS_ANA_PENDING, &ns->flags);
	/*
	 * Going live sends I/O to the head and so to this path, which a
	 * controller that is not live cannot take (deadlock when called
	 * from the identify of a resetting controller): nvme_mpath_update()
	 * replays the log once the controller is live.
	 */
	if (nvme_state_is_live(nvme_ns_ana_state(ns)) &&
	    nvme_ctrl_state(ns->ctrl) == NVME_CTRL_LIVE)
		nvme_mpath_set_live(ns);
	else if (nvme_test_bit(NVME_NSHEAD_DISK_LIVE, &ns->head->flags))
		nvme_set_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags);
}

static int nvme_update_ana_state(struct nvme_ctrl *ctrl,
				 const struct nvme_ana_group_desc *desc,
				 void *data)
{
	uint32_t nr_nsids = le32_to_cpu(desc->nnsids), n = 0;
	unsigned int *nr_change_groups = (unsigned int *)data;
	struct nvme_ns *ns;
	int idx;

	if (desc->state == NVME_ANA_CHANGE)
		(*nr_change_groups)++;
	if (!nr_nsids)
		return 0;

	/* Both the namespace list and the group's NSIDs are ascending:
	 * walk them side by side. */
	idx = nvme_srcu_read_lock(&ctrl->ns_srcu);
	nvme_list_for_each_entry_srcu(ns, &ctrl->namespaces, list) {
		uint32_t nsid;
again:
		nsid = le32_to_cpu(desc->nsids[n]);
		if (ns->head->ns_id < nsid)
			continue;
		if (ns->head->ns_id == nsid)
			nvme_update_ns_ana_state(desc, ns);
		if (++n == nr_nsids)
			break;
		if (ns->head->ns_id > nsid)
			goto again;
	}
	nvme_srcu_read_unlock(&ctrl->ns_srcu, idx);
	return 0;
}

/* ANATT timer arming (see the anatt_fires comment in nvme.h). */
static void nvme_anatt_arm(struct nvme_ctrl *ctrl, uint64_t delay_ms)
{
	uint64_t flags;

	spin_lock_irqsave(&ctrl->anatt_lock, &flags);
	if (hrtimer_cancel(&ctrl->anatt_timer))
		__atomic_sub_fetch(&ctrl->anatt_fires, 1, __ATOMIC_ACQ_REL);
	__atomic_add_fetch(&ctrl->anatt_fires, 1, __ATOMIC_ACQ_REL);
	hrtimer_start_rel(&ctrl->anatt_timer, delay_ms * 1000000ULL);
	spin_unlock_irqrestore(&ctrl->anatt_lock, flags);
}

/* Disarm and wait until no callback runs any more. */
static void nvme_anatt_cancel_sync(struct nvme_ctrl *ctrl)
{
	uint64_t flags;

	spin_lock_irqsave(&ctrl->anatt_lock, &flags);
	if (hrtimer_cancel(&ctrl->anatt_timer))
		__atomic_sub_fetch(&ctrl->anatt_fires, 1, __ATOMIC_ACQ_REL);
	spin_unlock_irqrestore(&ctrl->anatt_lock, flags);
	while (__atomic_load_n(&ctrl->anatt_fires, __ATOMIC_ACQUIRE) > 0) {
		if (blk_can_sleep())
			nvme_msleep(1);
		else
			__builtin_ia32_pause();
	}
	BUG_ON(__atomic_load_n(&ctrl->anatt_fires, __ATOMIC_ACQUIRE) < 0);
}

static void nvme_anatt_timeout(struct hrtimer *t)
{
	struct nvme_ctrl *ctrl = (struct nvme_ctrl *)t->arg;

	nvme_ctrl_info(ctrl, "ANATT timeout, resetting controller.\n");
	nvme_reset_ctrl(ctrl);
	/* The last access to ctrl. */
	__atomic_sub_fetch(&ctrl->anatt_fires, 1, __ATOMIC_ACQ_REL);
}

static int nvme_read_ana_log(struct nvme_ctrl *ctrl)
{
	unsigned int nr_change_groups = 0;
	int error;

	might_sleep();
	mutex_lock(&ctrl->ana_lock);
	error = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_ANA, 0, NVME_CSI_NVM,
			     ctrl->ana_log_buf, ctrl->ana_log_size, 0);
	if (error) {
		nvme_ctrl_warn(ctrl, "Failed to get ANA log: %d\n", error);
		goto out_unlock;
	}

	error = nvme_parse_ana_log(ctrl, &nr_change_groups,
				   nvme_update_ana_state);
	if (error)
		goto out_unlock;

	/*
	 * One ANATT timer for all groups (a timer per group would only
	 * guard against a target that starts new transitions while never
	 * finishing old ones; it still times out once all are in change
	 * state).  Twice ANATT, for transport and event latency.
	 */
	if (nr_change_groups)
		nvme_anatt_arm(ctrl, (uint64_t)ctrl->anatt * 2u * 1000u);
	else
		nvme_anatt_cancel_sync(ctrl);
out_unlock:
	mutex_unlock(&ctrl->ana_lock);
	return error;
}

void nvme_ana_work(void *arg)
{
	struct nvme_ctrl *ctrl = (struct nvme_ctrl *)arg;

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
		return;
	if (!ctrl->ana_log_buf)
		return;
	nvme_read_ana_log(ctrl);
}

void nvme_mpath_update(struct nvme_ctrl *ctrl)
{
	unsigned int nr_change_groups = 0;

	if (!ctrl->ana_log_buf)
		return;
	mutex_lock(&ctrl->ana_lock);
	nvme_parse_ana_log(ctrl, &nr_change_groups, nvme_update_ana_state);
	mutex_unlock(&ctrl->ana_lock);
}

static int nvme_lookup_ana_group_desc(struct nvme_ctrl *ctrl,
				      const struct nvme_ana_group_desc *desc,
				      void *data)
{
	struct nvme_ana_group_desc *dst = (struct nvme_ana_group_desc *)data;

	(void)ctrl;
	if (desc->grpid != dst->grpid)
		return 0;
	kmemcpy(dst, desc, sizeof(*dst));
	return -ENXIO; /* found: stop the walk */
}

void nvme_mpath_add_disk(struct nvme_ns *ns, __le32 anagrpid)
{
	might_sleep();
	if (nvme_ctrl_use_ana(ns->ctrl)) {
		struct nvme_ana_group_desc desc;

		kmemset(&desc, 0, sizeof(desc));
		desc.grpid = anagrpid;
		mutex_lock(&ns->ctrl->ana_lock);
		__atomic_store_n(&ns->ana_grpid, le32_to_cpu(anagrpid),
				 __ATOMIC_RELAXED);
		nvme_parse_ana_log(ns->ctrl, &desc, nvme_lookup_ana_group_desc);
		mutex_unlock(&ns->ctrl->ana_lock);
		if (desc.state) {
			/* The group is known: take its state. */
			nvme_update_ns_ana_state(&desc, ns);
		} else {
			/* Unknown yet: re-read the log. */
			nvme_set_bit(NVME_NS_ANA_PENDING, &ns->flags);
			nvme_queue_work(&ns->ctrl->ana_work);
		}
	} else {
		nvme_ns_set_ana_state(ns, NVME_ANA_OPTIMIZED);
		nvme_mpath_set_live(ns);
	}

	if ((ns->disk->limits.features & BLK_FEAT_ZONED) && ns->head->disk)
		ns->head->disk->nr_zones = ns->disk->nr_zones;
}

/* ==========================================================================
 * Controller setup / teardown
 * ========================================================================== */

int nvme_mpath_init_ctrl(struct nvme_ctrl *ctrl)
{
	if (__atomic_load_n(&ctrl->mpath_inited, __ATOMIC_ACQUIRE))
		return 0;
	nvme_srcu_init(&ctrl->ns_srcu, "nvme_ns_srcu");
	/* The scan work item (the core initialises it too; nothing can
	 * have queued it before the controller is set up). */
	if (!ctrl->scan_work.fn)
		blk_work_init(&ctrl->scan_work, nvme_scan_work, ctrl);
	mutex_init(&ctrl->ana_lock, "nvme_ana");
	spinlock_init(&ctrl->anatt_lock, "nvme_anatt");
	ctrl->anatt_fires = 0;
	hrtimer_init(&ctrl->anatt_timer, nvme_anatt_timeout, ctrl);
	blk_work_init(&ctrl->ana_work, nvme_ana_work, ctrl);
	ctrl->discard_page_busy = 0;
	__atomic_store_n(&ctrl->mpath_inited, 1, __ATOMIC_RELEASE);
	return 0;
}

int nvme_mpath_init_identify(struct nvme_ctrl *ctrl,
			     const struct nvme_id_ctrl *id)
{
	size_t max_transfer_size = (size_t)ctrl->max_hw_sectors << 9;
	size_t ana_log_size;
	int error = 0;

	might_sleep();
	/* Identify fields the namespace and I/O code needs, refreshed with
	 * every identify (a firmware update may change them). */
	ctrl->fuses = le16_to_cpu(id->fuses);
	ctrl->ocfs = le16_to_cpu(id->ocfs);
	ctrl->fna = id->fna;
	ctrl->sanicap = le32_to_cpu(id->sanicap);

	/* Multipath on and the capability present? */
	if (!nvme_mpath_multipath_param() || !ctrl->subsys ||
	    !(ctrl->subsys->cmic & NVME_CTRL_CMIC_ANA))
		return 0;

	/* Reset with every identify (controller reset). */
	__atomic_store_n(&ctrl->nr_active, 0, __ATOMIC_RELAXED);

	if (!ctrl->max_namespaces ||
	    ctrl->max_namespaces > le32_to_cpu(id->nn)) {
		nvme_ctrl_err(ctrl, "Invalid MNAN value %u\n",
			      ctrl->max_namespaces);
		return -EINVAL;
	}

	ctrl->anacap = id->anacap;
	ctrl->anatt = id->anatt;
	ctrl->nanagrpid = le32_to_cpu(id->nanagrpid);
	ctrl->anagrpmax = le32_to_cpu(id->anagrpmax);

	ana_log_size = sizeof(struct nvme_ana_rsp_hdr) +
		       (size_t)ctrl->nanagrpid *
			       sizeof(struct nvme_ana_group_desc) +
		       (size_t)ctrl->max_namespaces * sizeof(__le32);
	if (ana_log_size > max_transfer_size) {
		nvme_ctrl_err(ctrl,
			      "ANA log page size (%llu) larger than MDTS (%llu).\n",
			      (unsigned long long)ana_log_size,
			      (unsigned long long)max_transfer_size);
		nvme_ctrl_err(ctrl, "disabling ANA support.\n");
		goto out_uninit;
	}
	if (ana_log_size > ctrl->ana_log_size) {
		nvme_mpath_stop(ctrl);
		nvme_mpath_uninit(ctrl);
		ctrl->ana_log_buf =
			(struct nvme_ana_rsp_hdr *)nvme_kzalloc(ana_log_size);
		if (!ctrl->ana_log_buf)
			return -ENOMEM;
	}
	ctrl->ana_log_size = ana_log_size;
	error = nvme_read_ana_log(ctrl);
	if (error)
		goto out_uninit;
	return 0;

out_uninit:
	nvme_mpath_uninit(ctrl);
	return error;
}

void nvme_mpath_uninit(struct nvme_ctrl *ctrl)
{
	if (ctrl->ana_log_buf)
		kfree(ctrl->ana_log_buf);
	ctrl->ana_log_buf = NULL;
	ctrl->ana_log_size = 0;
}

void nvme_mpath_stop(struct nvme_ctrl *ctrl)
{
	might_sleep();
	if (!__atomic_load_n(&ctrl->mpath_inited, __ATOMIC_ACQUIRE))
		return;
	/* Also without ANA: a timer armed before ANA got disabled. */
	nvme_anatt_cancel_sync(ctrl);
	if (!nvme_ctrl_use_ana(ctrl))
		return;
	nvme_cancel_work_sync(&ctrl->ana_work);
}
