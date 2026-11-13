// LikeOS - NVMe core: controller lifetime, state machine, subsystems,
// quirks, keep-alive, asynchronous events, firmware activation, power
// states and APST, host behavior, timestamp, work helpers, boot knobs
//
// The transport independent half of a controller (struct nvme_ctrl,
// embedded in the transport's object):
//
//   lifetime    nvme_init_ctrl() gives the controller its instance, locks
//               and work items (refcount 1, state NEW); nvme_add_ctrl()
//               makes it visible (/dev/nvmeX through P6, /sys/class/nvme)
//               and takes one more reference, dropped by nvme_uninit_ctrl(); the last
//               nvme_put_ctrl() frees the core's parts, calls the
//               transport's free_ctrl and drops the subsystem;
//   state       one table (nvme_ctrl_state_transitions) decides every
//               change, made under ctrl->lock by nvme_change_ctrl_state();
//               RESETTING is the recovery gate: only the caller that moved
//               the controller there queues reset_work;
//   identify    nvme_init_ctrl_finish() after every enable: Identify
//               Controller, subsystem lookup by SUBNQN (first time), core
//               quirks by model/firmware, limits, effects log, APST,
//               timestamp, host behavior, hwmon, keep-alive need;
//   events      one Asynchronous Event Request is kept outstanding by the
//               transport; its completion lands in
//               nvme_complete_async_event() (CQ context), which dispatches
//               by type and lets async_event_work read the log page that
//               clears the event and post the next request;
//   firmware    an activation notice parks the controller in RESETTING
//               while fw_act_work waits for CSTS.PP to clear.
//
// Lock order and contexts: kernel/dev/nvme/nvme.h.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/dev/block/blk_softirq.h>
#include <kernel/fs/chrdev.h>
#include <kernel/ke/storage.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/percpu.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ==========================================================================
 * Globals
 * ========================================================================== */

struct nvme_core_params nvme_core_param = {
	.admin_timeout_s = NVME_DEFAULT_ADMIN_TIMEOUT_S,
	.io_timeout_s = NVME_DEFAULT_IO_TIMEOUT_S,
	.shutdown_timeout_s = NVME_DEFAULT_SHUTDOWN_TIMEOUT_S,
	.max_retries = NVME_DEFAULT_MAX_RETRIES,
	.default_ps_max_latency_us = NVME_DEFAULT_PS_MAX_LATENCY_US,
	.force_apst = 0,
	.apst_primary_timeout_ms = NVME_DEFAULT_APST_PRIMARY_TIMEOUT_MS,
	.apst_secondary_timeout_ms = NVME_DEFAULT_APST_SECONDARY_TIMEOUT_MS,
	.apst_primary_latency_tol_us = NVME_DEFAULT_APST_PRIMARY_LATENCY_TOL_US,
	.apst_secondary_latency_tol_us =
		NVME_DEFAULT_APST_SECONDARY_LATENCY_TOL_US,
	.multipath = 1,
	.multipath_always_on = 0,
	.iopolicy = NVME_IOPOLICY_NUMA,
	.streams = 0,
	.passthru_err_log = 0,
	.quirks_add = 0,
	.quirks_vid = 0,
	.quirks_did = 0,
};

struct list_head nvme_subsystems = LIST_HEAD_INIT(nvme_subsystems);
struct mutex nvme_subsystems_lock = MUTEX_INITIALIZER("nvme_subsystems");
struct ida nvme_instance_ida = IDA_INIT("nvme_instance");
struct ida nvme_ns_chr_minor_ida = IDA_INIT("nvme_ns_chr_minor");
uint32_t nvme_char_major;
uint32_t nvme_ns_chr_major;

/* How long nvme_cancel_work_sync() lets a running item finish. */
#define NVME_WORK_CANCEL_TIMEOUT_MS (10u * 60u * 1000u)

/* The largest AEN dword 0 value (bits 23:0 carry type, info, log page)
 * plus the "a result is pending" marker kept in the reserved bits. */
#define NVME_AEN_RESULT_MASK 0x00ffffffu
#define NVME_AEN_PENDING_MARK 0x80000000u

static void nvme_update_keep_alive(struct nvme_ctrl *ctrl, uint32_t kato_ms);

/* ==========================================================================
 * Boot command line knobs
 * ========================================================================== */

static int nvme_is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int nvme_cmdline_get(const char *key, char *buf, size_t cap)
{
	const char *cl = storage_boot_cmdline();
	size_t klen;
	int found = 0;

	if (!key || !*key || !cl)
		return 0;
	klen = kstrlen(key);
	while (*cl) {
		const char *tok, *end;

		while (*cl && nvme_is_space(*cl))
			cl++;
		if (!*cl)
			break;
		tok = cl;
		while (*cl && !nvme_is_space(*cl))
			cl++;
		end = cl;
		if ((size_t)(end - tok) < klen || kstrncmp(tok, key, klen))
			continue;
		if (tok + klen == end) {
			/* "key" alone: a set flag with an empty value */
			if (buf && cap)
				buf[0] = '\0';
			found = 1;
			continue;
		}
		if (tok[klen] != '=')
			continue;
		/* Last one wins: overwrite what an earlier one left. */
		if (buf && cap) {
			const char *v = tok + klen + 1;
			size_t n = (size_t)(end - v);

			if (n >= cap)
				n = cap - 1;
			kmemcpy(buf, v, n);
			buf[n] = '\0';
		}
		found = 1;
	}
	return found;
}

/* "123", "0x7b": 0 / -EINVAL. */
static int nvme_parse_u64(const char *s, uint64_t *out)
{
	uint64_t v = 0;
	int base = 10, digits = 0;

	if (!s || !*s)
		return -EINVAL;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (; *s; s++) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -EINVAL;
		v = v * (uint64_t)base + (uint64_t)d;
		digits++;
	}
	if (!digits)
		return -EINVAL;
	*out = v;
	return 0;
}

/* Hex without a prefix ("8086", "1f"): 0 / -EINVAL. */
static int nvme_parse_hex(const char *s, size_t n, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (!n)
		return -EINVAL;
	if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		n -= 2;
	}
	for (i = 0; i < n; i++) {
		char c = s[i];
		int d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			return -EINVAL;
		v = (v << 4) | (uint64_t)d;
	}
	*out = v;
	return 0;
}

/* "1", "y", "Y", "on", "true" / "0", "n", "N", "off", "false". */
static int nvme_parse_bool(const char *s, int *out)
{
	if (!s)
		return -EINVAL;
	if (!*s || !kstrcmp(s, "1") || !kstrcmp(s, "y") ||
	    !kstrcmp(s, "Y") || !kstrcmp(s, "on") || !kstrcmp(s, "true")) {
		*out = 1;
		return 0;
	}
	if (!kstrcmp(s, "0") || !kstrcmp(s, "n") || !kstrcmp(s, "N") ||
	    !kstrcmp(s, "off") || !kstrcmp(s, "false")) {
		*out = 0;
		return 0;
	}
	return -EINVAL;
}

static void nvme_knob_uint(const char *key, unsigned int *dst,
			   uint64_t max)
{
	char buf[32];
	uint64_t v;

	if (!nvme_cmdline_get(key, buf, sizeof(buf)))
		return;
	if (nvme_parse_u64(buf, &v) || v > max) {
		kprintf(NVME_LOG_PREFIX "ignoring %s=%s (invalid)\n", key, buf);
		return;
	}
	*dst = (unsigned int)v;
}

static void nvme_knob_u64(const char *key, uint64_t *dst)
{
	char buf[32];
	uint64_t v;

	if (!nvme_cmdline_get(key, buf, sizeof(buf)))
		return;
	if (nvme_parse_u64(buf, &v)) {
		kprintf(NVME_LOG_PREFIX "ignoring %s=%s (invalid)\n", key, buf);
		return;
	}
	*dst = v;
}

static void nvme_knob_bool(const char *key, int *dst)
{
	char buf[16];
	int v;

	if (!nvme_cmdline_get(key, buf, sizeof(buf)))
		return;
	if (nvme_parse_bool(buf, &v)) {
		kprintf(NVME_LOG_PREFIX "ignoring %s=%s (invalid)\n", key, buf);
		return;
	}
	*dst = v;
}

void nvme_core_parse_cmdline(void)
{
	struct nvme_core_params *p = &nvme_core_param;
	char buf[64];
	uint64_t v;

	nvme_knob_uint("nvme_core.admin_timeout", &p->admin_timeout_s,
		       0xffffffffULL);
	nvme_knob_uint("nvme_core.io_timeout", &p->io_timeout_s,
		       0xffffffffULL);
	/* shutdown_timeout and max_retries are bytes, as established. */
	nvme_knob_uint("nvme_core.shutdown_timeout", &p->shutdown_timeout_s,
		       255);
	nvme_knob_uint("nvme_core.max_retries", &p->max_retries, 255);
	nvme_knob_u64("nvme_core.default_ps_max_latency_us",
		      &p->default_ps_max_latency_us);
	nvme_knob_bool("nvme_core.force_apst", &p->force_apst);
	nvme_knob_uint("nvme_core.apst_primary_timeout_ms",
		       &p->apst_primary_timeout_ms, 0xffffffffULL);
	nvme_knob_uint("nvme_core.apst_secondary_timeout_ms",
		       &p->apst_secondary_timeout_ms, 0xffffffffULL);
	nvme_knob_uint("nvme_core.apst_primary_latency_tol_us",
		       &p->apst_primary_latency_tol_us, 0xffffffffULL);
	nvme_knob_uint("nvme_core.apst_secondary_latency_tol_us",
		       &p->apst_secondary_latency_tol_us, 0xffffffffULL);
	nvme_knob_bool("nvme_core.multipath", &p->multipath);
	nvme_knob_bool("nvme_core.multipath_always_on",
		       &p->multipath_always_on);
	nvme_knob_bool("nvme_core.streams", &p->streams);
	nvme_knob_bool("nvme_core.passthru_err_log", &p->passthru_err_log);

	if (nvme_cmdline_get("nvme_core.iopolicy", buf, sizeof(buf))) {
		if (!kstrcmp(buf, "numa"))
			p->iopolicy = NVME_IOPOLICY_NUMA;
		else if (!kstrcmp(buf, "round-robin"))
			p->iopolicy = NVME_IOPOLICY_RR;
		else if (!kstrcmp(buf, "queue-depth"))
			p->iopolicy = NVME_IOPOLICY_QD;
		else
			kprintf(NVME_LOG_PREFIX
				"ignoring nvme_core.iopolicy=%s\n", buf);
	}
	/* multipath_always_on implies native multipath. */
	if (p->multipath_always_on)
		p->multipath = 1;

	/* nvme_core.quirks=VVVV:DDDD:mask (hex) */
	if (nvme_cmdline_get("nvme_core.quirks", buf, sizeof(buf))) {
		const char *c1 = buf, *c2 = NULL, *c3 = NULL;
		uint64_t vid, did, mask;
		const char *q;

		for (q = buf; *q; q++) {
			if (*q != ':')
				continue;
			if (!c2)
				c2 = q + 1;
			else if (!c3)
				c3 = q + 1;
		}
		if (c2 && c3 &&
		    !nvme_parse_hex(c1, (size_t)(c2 - 1 - c1), &vid) &&
		    !nvme_parse_hex(c2, (size_t)(c3 - 1 - c2), &did) &&
		    !nvme_parse_hex(c3, kstrlen(c3), &mask) && vid <= 0xffff &&
		    did <= 0xffff) {
			p->quirks_vid = (uint16_t)vid;
			p->quirks_did = (uint16_t)did;
			p->quirks_add = (unsigned long)mask;
		} else {
			kprintf(NVME_LOG_PREFIX
				"ignoring nvme_core.quirks=%s (VID:DID:mask)\n",
				buf);
		}
	}
	(void)v;
}

/* ==========================================================================
 * Sleeping and work items (kblockd)
 * ========================================================================== */

void nvme_msleep(unsigned int ms)
{
	if (!ms)
		return;
	if (blk_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0)
			; /* a signal cut it short: sleep the rest */
		return;
	}
	while (ms) {
		uint32_t chunk = ms > 100 ? 100 : ms;

		lapic_delay_ms(chunk);
		ms -= chunk;
	}
}

static int nvme_work_cpu(void)
{
	int cpu = blk_this_cpu();

	return cpu < 0 ? 0 : cpu;
}

static int nvme_work_pending(const struct blk_work *w)
{
	return (__atomic_load_n(&w->flags, __ATOMIC_ACQUIRE) &
		BLK_WORK_PENDING) != 0;
}

int nvme_queue_work(struct blk_work *w)
{
	BUG_ON(!w || !w->fn);
	return kblockd_schedule_work(w) != 0 ? 1 : 0;
}

int nvme_queue_delayed_work(struct blk_work *w, uint64_t delay_ms)
{
	BUG_ON(!w || !w->fn);
	if (!delay_ms)
		return nvme_queue_work(w);
	/* Like queue_delayed_work: an item already pending (on its timer or
	 * a list) keeps its schedule. */
	if (nvme_work_pending(w))
		return 0;
	kblockd_mod_delayed_work_on((uint32_t)nvme_work_cpu(), w,
				    delay_ms * 1000000ULL);
	return 1;
}

int nvme_cancel_work_sync(struct blk_work *w)
{
	int ret;

	might_sleep();
	BUG_ON(!w);
	if (!w->fn)
		return 0; /* never initialised: nothing can run */
	ret = blk_work_cancel_sync(w, NVME_WORK_CANCEL_TIMEOUT_MS);
	WARN(ret == -ETIMEDOUT, "nvme: work %p did not finish in %u ms", w,
	     NVME_WORK_CANCEL_TIMEOUT_MS);
	return ret;
}

int nvme_flush_work(struct blk_work *w)
{
	int ret;

	might_sleep();
	BUG_ON(!w);
	if (!w->fn)
		return 0;
	/* Waits, without bound, until `w' is neither queued nor running
	 * (a pending delayed run is started at once, like a flush of a
	 * delayed item); -EBUSY from inside the item's own run. */
	ret = blk_work_flush(w);
	return ret < 0 ? ret : 0;
}

/* ==========================================================================
 * State machine
 * ========================================================================== */

const uint8_t nvme_ctrl_state_transitions[NVME_CTRL_NR_STATES]
					 [NVME_CTRL_NR_STATES] = {
	[NVME_CTRL_LIVE] = {
		[NVME_CTRL_CONNECTING] = 1,
	},
	[NVME_CTRL_RESETTING] = {
		[NVME_CTRL_NEW] = 1,
		[NVME_CTRL_LIVE] = 1,
	},
	[NVME_CTRL_CONNECTING] = {
		[NVME_CTRL_NEW] = 1,
		[NVME_CTRL_RESETTING] = 1,
	},
	[NVME_CTRL_DELETING] = {
		[NVME_CTRL_LIVE] = 1,
		[NVME_CTRL_RESETTING] = 1,
		[NVME_CTRL_CONNECTING] = 1,
	},
	[NVME_CTRL_DELETING_NOIO] = {
		[NVME_CTRL_DELETING] = 1,
		[NVME_CTRL_DEAD] = 1,
	},
	[NVME_CTRL_DEAD] = {
		[NVME_CTRL_DELETING] = 1,
	},
};

const char *nvme_ctrl_state_name(enum nvme_ctrl_state state)
{
	static const char *const names[NVME_CTRL_NR_STATES] = {
		[NVME_CTRL_NEW] = "new",
		[NVME_CTRL_LIVE] = "live",
		[NVME_CTRL_RESETTING] = "resetting",
		[NVME_CTRL_CONNECTING] = "connecting",
		[NVME_CTRL_DELETING] = "deleting",
		[NVME_CTRL_DELETING_NOIO] = "deleting (no IO)",
		[NVME_CTRL_DEAD] = "dead",
	};

	if ((unsigned int)state < NVME_CTRL_NR_STATES && names[state])
		return names[state];
	return "unknown state";
}

/* Fail-fast expiry belongs to fabrics connections (fast_io_fail_tmo);
 * a PCIe controller never arms it.  The work and its stop are kept so
 * the state machine reads like the established one. */
static void nvme_failfast_work(void *arg)
{
	struct nvme_ctrl *ctrl = arg;

	if (nvme_ctrl_state(ctrl) != NVME_CTRL_CONNECTING)
		return;
	nvme_set_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);
	kprintf(NVME_LOG_PREFIX "%s: failfast expired\n", ctrl->name);
	nvme_kick_requeue_lists(ctrl);
}

static void nvme_start_failfast_work(struct nvme_ctrl *ctrl)
{
	if (!(ctrl->ops->flags & NVME_F_FABRICS))
		return;
	nvme_queue_delayed_work(&ctrl->failfast_work,
				(uint64_t)nvme_core_param.io_timeout_s * 1000);
}

static void nvme_stop_failfast_work(struct nvme_ctrl *ctrl)
{
	if (!(ctrl->ops->flags & NVME_F_FABRICS))
		return;
	nvme_cancel_work_sync(&ctrl->failfast_work);
	nvme_clear_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);
}

bool nvme_change_ctrl_state(struct nvme_ctrl *ctrl,
			    enum nvme_ctrl_state new_state)
{
	enum nvme_ctrl_state old_state;
	uint64_t flags;
	bool changed;

	if (WARN_ON((unsigned int)new_state >= NVME_CTRL_NR_STATES))
		return false;

	spin_lock_irqsave(&ctrl->lock, &flags);
	old_state = nvme_ctrl_state(ctrl);
	BUG_ON((unsigned int)old_state >= NVME_CTRL_NR_STATES);
	changed = nvme_ctrl_state_transition_ok(old_state, new_state);
	if (changed) {
		__atomic_store_n(&ctrl->state, new_state, __ATOMIC_RELEASE);
		wake_up_all(&ctrl->state_wq);
	}
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (!changed)
		return false;

	if (new_state == NVME_CTRL_LIVE) {
		if (old_state == NVME_CTRL_CONNECTING)
			nvme_stop_failfast_work(ctrl);
		nvme_kick_requeue_lists(ctrl);
	} else if (new_state == NVME_CTRL_CONNECTING &&
		   old_state == NVME_CTRL_RESETTING) {
		nvme_start_failfast_work(ctrl);
	}
	return true;
}

bool nvme_state_terminal(const struct nvme_ctrl *ctrl)
{
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);

	switch (state) {
	case NVME_CTRL_NEW:
	case NVME_CTRL_LIVE:
	case NVME_CTRL_RESETTING:
	case NVME_CTRL_CONNECTING:
		return false;
	case NVME_CTRL_DELETING:
	case NVME_CTRL_DELETING_NOIO:
	case NVME_CTRL_DEAD:
		return true;
	default:
		WARN(1, "nvme: unhandled controller state %d", (int)state);
		return true;
	}
}

bool nvme_wait_reset(struct nvme_ctrl *ctrl)
{
	might_sleep();
	wait_event(ctrl->state_wq,
		   nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING) ||
			   nvme_state_terminal(ctrl));
	return nvme_ctrl_state(ctrl) == NVME_CTRL_RESETTING;
}

/* ==========================================================================
 * Reset and delete
 * ========================================================================== */

int nvme_try_sched_reset(struct nvme_ctrl *ctrl)
{
	if (nvme_ctrl_state(ctrl) != NVME_CTRL_RESETTING)
		return -EBUSY;
	if (WARN_ON_ONCE(!ctrl->reset_work.fn))
		return -EBUSY;
	if (!nvme_queue_work(&ctrl->reset_work))
		return -EBUSY;
	return 0;
}

int nvme_reset_ctrl(struct nvme_ctrl *ctrl)
{
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))
		return -EBUSY;
	if (WARN_ON_ONCE(!ctrl->reset_work.fn))
		return -EBUSY;
	if (!nvme_queue_work(&ctrl->reset_work))
		return -EBUSY;
	return 0;
}

int nvme_reset_ctrl_sync(struct nvme_ctrl *ctrl)
{
	int ret;

	might_sleep();
	ret = nvme_reset_ctrl(ctrl);
	if (!ret) {
		nvme_flush_work(&ctrl->reset_work);
		if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
			ret = -ENETRESET;
	}
	return ret;
}

static void nvme_do_delete_ctrl(struct nvme_ctrl *ctrl)
{
	might_sleep();
	kprintf(NVME_LOG_PREFIX "%s: removing controller: NQN \"%s\"\n",
		ctrl->name, ctrl->subsys ? ctrl->subsys->subnqn : "");

	nvme_flush_work(&ctrl->reset_work);
	nvme_stop_ctrl(ctrl);
	nvme_remove_namespaces(ctrl);
	if (ctrl->ops->delete_ctrl)
		ctrl->ops->delete_ctrl(ctrl);
	nvme_uninit_ctrl(ctrl);
}

static void nvme_delete_ctrl_work(void *arg)
{
	struct nvme_ctrl *ctrl = arg;

	nvme_do_delete_ctrl(ctrl);
	/* Drop what nvme_delete_ctrl() took for this run. */
	nvme_put_ctrl(ctrl);
}

int nvme_delete_ctrl(struct nvme_ctrl *ctrl)
{
	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING))
		return -EBUSY;
	/* delete_ctrl may free the transport's object: keep the controller
	 * alive until the work is done with it. */
	nvme_get_ctrl(ctrl);
	if (!nvme_queue_work(&ctrl->delete_work)) {
		nvme_put_ctrl(ctrl);
		return -EBUSY;
	}
	return 0;
}

void nvme_delete_ctrl_sync(struct nvme_ctrl *ctrl)
{
	might_sleep();
	/* Keep a reference until nvme_do_delete_ctrl() is done: the
	 * transport's delete_ctrl may drop the last other one. */
	nvme_get_ctrl(ctrl);
	if (nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING))
		nvme_do_delete_ctrl(ctrl);
	nvme_put_ctrl(ctrl);
}

void nvme_remove_ctrl(struct nvme_ctrl *ctrl)
{
	might_sleep();
	if (nvme_test_and_set_bit(NVME_CTRL_REMOVED, &ctrl->flags)) {
		WARN(1, "nvme: %s removed twice", ctrl->name);
		return;
	}
	nvme_get_ctrl(ctrl);
	/* A controller that is already being deleted (or dead) stays in
	 * that state; anything else goes to DELETING now. */
	nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING);
	nvme_do_delete_ctrl(ctrl);
	nvme_put_ctrl(ctrl);
}

/* ==========================================================================
 * Controller register helpers: enable / disable / wait ready
 * ========================================================================== */

int nvme_wait_ready(struct nvme_ctrl *ctrl, uint32_t mask, uint32_t val,
		    uint32_t timeout_s, const char *op)
{
	uint64_t deadline = hrtimer_now_ns() +
			    (uint64_t)timeout_s * 1000000000ULL;
	uint32_t csts = 0;
	int ret;

	might_sleep();
	while ((ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts)) ==
	       0) {
		if (csts == ~0u)
			return -ENODEV;
		if ((csts & mask) == val)
			break;

		nvme_msleep(1);
		if (blk_can_sleep() && sched_current() &&
		    task_fatal_signal_pending(sched_current()))
			return -EINTR;
		if (hrtimer_now_ns() > deadline) {
			kprintf(NVME_LOG_PREFIX
				"%s: device not ready; aborting %s, "
				"CSTS=0x%x\n",
				ctrl->name, op, csts);
			return -ENODEV;
		}
	}
	return ret;
}

int nvme_disable_ctrl(struct nvme_ctrl *ctrl, bool shutdown)
{
	uint32_t cc;
	int ret;

	might_sleep();
	/* Built in a local and published with one store: the scan work reads
	 * the value (nvme_multi_css) without the lock the writers hold. */
	cc = nvme_ctrl_config(ctrl) & ~(uint32_t)NVME_CC_SHN_MASK;
	if (shutdown)
		cc |= NVME_CC_SHN_NORMAL;
	else
		cc &= ~(uint32_t)NVME_CC_ENABLE;
	nvme_set_ctrl_config(ctrl, cc);

	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, cc);
	if (ret)
		return ret;

	if (shutdown)
		return nvme_wait_ready(ctrl, NVME_CSTS_SHST_MASK,
				       NVME_CSTS_SHST_CMPLT,
				       ctrl->shutdown_timeout_s ?
					       ctrl->shutdown_timeout_s :
					       nvme_core_param.shutdown_timeout_s,
				       "shutdown");
	if (ctrl->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY)
		nvme_msleep(NVME_QUIRK_DELAY_AMOUNT);
	return nvme_wait_ready(ctrl, NVME_CSTS_RDY, 0,
			       (NVME_CAP_TIMEOUT(ctrl->cap) + 1) / 2, "reset");
}

int nvme_enable_ctrl(struct nvme_ctrl *ctrl)
{
	unsigned int dev_page_min;
	uint32_t timeout, cc;
	int ret;

	might_sleep();
	ret = ctrl->ops->reg_read64(ctrl, NVME_REG_CAP, &ctrl->cap);
	if (ret) {
		kprintf(NVME_LOG_PREFIX "%s: reading CAP failed (%d)\n",
			ctrl->name, ret);
		return ret;
	}
	dev_page_min = (unsigned int)NVME_CAP_MPSMIN(ctrl->cap) + 12;
	if (NVME_CTRL_PAGE_SHIFT < dev_page_min) {
		kprintf(NVME_LOG_PREFIX
			"%s: minimum device page size %u too large for host "
			"(%u)\n",
			ctrl->name, 1u << dev_page_min,
			1u << NVME_CTRL_PAGE_SHIFT);
		return -ENODEV;
	}

	if (NVME_CAP_CSS(ctrl->cap) & NVME_CAP_CSS_CSI)
		cc = NVME_CC_CSS_CSI;
	else
		cc = NVME_CC_CSS_NVM;

	/* CC.CRIME would report ready before the media is: commands could
	 * then fail with Admin Command Media Not Ready, which nothing
	 * retries.  Keep it off. */
	cc &= ~(uint32_t)NVME_CC_CRIME;

	cc |= (NVME_CTRL_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT;
	cc |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
	cc |= NVME_CC_IOSQES | NVME_CC_IOCQES;
	nvme_set_ctrl_config(ctrl, cc);
	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, cc);
	if (ret)
		return ret;

	/* CAP may change after the first CC write. */
	ret = ctrl->ops->reg_read64(ctrl, NVME_REG_CAP, &ctrl->cap);
	if (ret)
		return ret;

	timeout = (uint32_t)NVME_CAP_TIMEOUT(ctrl->cap);
	if (ctrl->cap & NVME_CAP_CRMS_CRWMS) {
		uint32_t crto, ready_timeout;

		ret = ctrl->ops->reg_read32(ctrl, NVME_REG_CRTO, &crto);
		if (ret) {
			kprintf(NVME_LOG_PREFIX "%s: reading CRTO failed (%d)\n",
				ctrl->name, ret);
			return ret;
		}
		/* CRTO should never be below CAP.TO, but some devices get it
		 * wrong: use the larger one. */
		ready_timeout = NVME_CRTO_CRWMT(crto);
		if (ready_timeout < timeout) {
			static int warned;

			if (nvme_first_time(&warned)) {
				kprintf(NVME_LOG_PREFIX
					"%s: bad crto:%x cap:%llx\n",
					ctrl->name, crto,
					(unsigned long long)ctrl->cap);
			}
		} else {
			timeout = ready_timeout;
		}
	}

	cc |= NVME_CC_ENABLE;
	nvme_set_ctrl_config(ctrl, cc);
	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_CC, cc);
	if (ret)
		return ret;
	return nvme_wait_ready(ctrl, NVME_CSTS_RDY, NVME_CSTS_RDY,
			       (timeout + 1) / 2, "initialisation");
}

/* ==========================================================================
 * Lifetime
 * ========================================================================== */

void nvme_get_ctrl(struct nvme_ctrl *ctrl)
{
	refcount_inc(&ctrl->ref);
}

static void nvme_free_cels(struct nvme_ctrl *ctrl)
{
	unsigned int i;

	for (i = 0; i < NVME_NR_EFFECTS_CSI; i++) {
		struct nvme_effects_log *cel =
			__atomic_exchange_n(&ctrl->cels[i], NULL,
					    __ATOMIC_ACQ_REL);

		if (cel)
			kfree(cel);
	}
	ctrl->effects = NULL;
}

/* Take back every work item embedded in the controller before its memory
 * goes (they are idle by now on every regular path: a safety net against
 * a late event re-queueing one). */
static void nvme_cancel_ctrl_works(struct nvme_ctrl *ctrl)
{
	struct blk_work *const works[] = {
		&ctrl->reset_work,	 &ctrl->delete_work,
		&ctrl->scan_work,	 &ctrl->async_event_work,
		&ctrl->ka_work,		 &ctrl->failfast_work,
		&ctrl->fw_act_work,	 &ctrl->ana_work,
	};
	size_t i;

	for (i = 0; i < sizeof(works) / sizeof(works[0]); i++) {
		int ret;

		if (!works[i]->fn)
			continue;
		ret = blk_work_cancel_sync(works[i], NVME_WORK_CANCEL_TIMEOUT_MS);
		/* Freeing an item that is still running would be a
		 * use-after-free: the free work never runs inside one. */
		BUG_ON(ret == -EBUSY);
		WARN(ret, "nvme: %s: work %zu still busy at free (%d)",
		     ctrl->name, i, ret);
	}
}

/* The last reference went: release what the core owns, let the transport
 * free its object (which contains `ctrl'), then drop the subsystem.
 * Runs in nvme_free_work (see nvme_put_ctrl). */
static void nvme_free_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_subsystem *subsys = ctrl->subsys;

	might_sleep();
	BUG_ON(refcount_read(&ctrl->ref) != 0);

	nvme_cancel_ctrl_works(ctrl);
	if (ctrl->admin_q) {
		blk_put_queue(ctrl->admin_q);
		ctrl->admin_q = NULL;
	}
	if (!subsys || ctrl->instance != subsys->instance)
		ida_free(&nvme_instance_ida, (unsigned int)ctrl->instance);
	nvme_free_cels(ctrl);
	nvme_mpath_uninit(ctrl);
	/* nvme_uninit_ctrl shut hwmon's readers out; no reference (and so
	 * no sysfs reader) is left now. */
	nvme_hwmon_free(ctrl);

	if (subsys) {
		mutex_lock(&nvme_subsystems_lock);
		mutex_lock(&subsys->lock);
		list_del(&ctrl->subsys_entry);
		mutex_unlock(&subsys->lock);
		mutex_unlock(&nvme_subsystems_lock);
	}

	ctrl->ops->free_ctrl(ctrl);
	/* `ctrl' is gone now. */

	if (subsys)
		nvme_put_subsystem(subsys);
}

/* Controllers whose last reference went, freed by nvme_free_work: the
 * last put may happen inside one of the controller's own work items
 * (delete_work does it), and kblockd still touches an item after its
 * function returned, so the memory must go from a work item that does
 * not live in it.  The list is lock-free (push here, take all there). */
static struct nvme_ctrl *volatile nvme_free_list;
static struct blk_work nvme_free_work;

static void nvme_free_work_fn(void *arg)
{
	struct nvme_ctrl *ctrl;

	(void)arg;
	ctrl = __atomic_exchange_n(&nvme_free_list, NULL, __ATOMIC_ACQ_REL);
	while (ctrl) {
		struct nvme_ctrl *next = ctrl->free_next;

		nvme_free_ctrl(ctrl);
		ctrl = next;
	}
}

void nvme_put_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_ctrl *head;

	if (!refcount_dec_and_test(&ctrl->ref))
		return;
	head = __atomic_load_n(&nvme_free_list, __ATOMIC_ACQUIRE);
	do {
		ctrl->free_next = head;
	} while (!__atomic_compare_exchange_n(&nvme_free_list, &head, ctrl, 1,
					      __ATOMIC_ACQ_REL,
					      __ATOMIC_ACQUIRE));
	if (!nvme_free_work.fn)
		blk_work_init(&nvme_free_work, nvme_free_work_fn, NULL);
	nvme_queue_work(&nvme_free_work);
}

static void nvme_async_event_work(void *arg);
static void nvme_fw_act_work(void *arg);
static void nvme_keep_alive_work(void *arg);

int nvme_init_ctrl(struct nvme_ctrl *ctrl, pci_device_t *pdev,
		   const struct nvme_ctrl_ops *ops, unsigned long quirks)
{
	int ret;

	/* No might_sleep(): the transport probes at early boot (interrupts
	 * off, no scheduler) and nothing here blocks. */
	BUG_ON(!ctrl || !ops);
	BUG_ON(!ops->reg_read32 || !ops->reg_write32 || !ops->reg_read64 ||
	       !ops->free_ctrl || !ops->submit_async_event);

	__atomic_store_n(&ctrl->state, NVME_CTRL_NEW, __ATOMIC_RELEASE);
	ctrl->passthru_err_log_enabled =
		nvme_core_param.passthru_err_log != 0;
	nvme_clear_bit(NVME_CTRL_FAILFAST_EXPIRED, &ctrl->flags);
	spinlock_init(&ctrl->lock, "nvme_ctrl");
	mutex_init(&ctrl->namespaces_lock, "nvme_namespaces");
	mutex_init(&ctrl->scan_lock, "nvme_scan");
	mutex_init(&ctrl->shutdown_lock, "nvme_shutdown");
	INIT_LIST_HEAD(&ctrl->namespaces);
	INIT_LIST_HEAD(&ctrl->subsys_entry);
	ctrl->pdev = pdev;
	ctrl->ops = ops;
	ctrl->quirks = quirks;
	if (pdev && nvme_core_param.quirks_add &&
	    pdev->vendor_id == nvme_core_param.quirks_vid &&
	    pdev->device_id == nvme_core_param.quirks_did) {
		ctrl->quirks |= nvme_core_param.quirks_add;
		kprintf(NVME_LOG_PREFIX
			"%04x:%04x: quirks 0x%lx added by nvme_core.quirks\n",
			pdev->vendor_id, pdev->device_id,
			nvme_core_param.quirks_add);
	}
	ctrl->numa_node = (unsigned int)-1;
	ctrl->kato_s = 0;
	ctrl->subsys = NULL;
	ctrl->effects = NULL;
	kmemset(ctrl->cels, 0, sizeof(ctrl->cels));
	ctrl->hwmon = NULL;
	ctrl->aen_pending = 0;
	ctrl->aen_cfg = 0;

	blk_work_init(&ctrl->scan_work, nvme_scan_work, ctrl);
	blk_work_init(&ctrl->async_event_work, nvme_async_event_work, ctrl);
	blk_work_init(&ctrl->fw_act_work, nvme_fw_act_work, ctrl);
	blk_work_init(&ctrl->delete_work, nvme_delete_ctrl_work, ctrl);
	blk_work_init(&ctrl->ka_work, nvme_keep_alive_work, ctrl);
	blk_work_init(&ctrl->failfast_work, nvme_failfast_work, ctrl);
	wq_head_init(&ctrl->state_wq, "nvme_state");

	kmemset(&ctrl->ka_cmd, 0, sizeof(ctrl->ka_cmd));
	ctrl->ka_cmd.common.opcode = nvme_admin_keep_alive;
	ctrl->ka_last_check_ns = hrtimer_now_ns();

	ret = ida_alloc_max(&nvme_instance_ida, NVME_MAX_INSTANCES - 1);
	if (ret < 0)
		return ret;
	ctrl->instance = ret;
	ksnprintf(ctrl->name, sizeof(ctrl->name), "nvme%d", ctrl->instance);

	ret = nvme_mpath_init_ctrl(ctrl);
	if (ret) {
		ida_free(&nvme_instance_ida, (unsigned int)ctrl->instance);
		return ret;
	}

	refcount_set(&ctrl->ref, 1);
	return 0;
}

int nvme_add_ctrl(struct nvme_ctrl *ctrl)
{
	uint64_t lat = nvme_core_param.default_ps_max_latency_us;
	int ret;

	/* Early boot probe too (see nvme_init_ctrl): no might_sleep(). */
	/* The PM QoS latency tolerance starts at the boot default (capped
	 * like the established interface, which takes an s32). */
	if (lat > 0x7fffffffULL)
		lat = 0x7fffffffULL;
	ctrl->ps_max_latency_us = lat;

	ret = nvme_cdev_add_ctrl(ctrl);
	if (ret) {
		kprintf(NVME_LOG_PREFIX "%s: character device failed (%d)\n",
			ctrl->name, ret);
		return ret;
	}
	ret = nvme_sysfs_add_ctrl(ctrl);
	if (ret) {
		kprintf(NVME_LOG_PREFIX "%s: sysfs registration failed (%d)\n",
			ctrl->name, ret);
		nvme_cdev_del_ctrl(ctrl);
		return ret;
	}
	nvme_fault_inject_init(&ctrl->fault_inject, ctrl->name);
	if (nvme_debug_ctrl_init(ctrl))
		ctrl->debug = NULL; /* debugging is optional */
	nvme_get_ctrl(ctrl);
	return 0;
}

void nvme_uninit_ctrl(struct nvme_ctrl *ctrl)
{
	might_sleep();
	nvme_stop_keep_alive(ctrl);
	nvme_hwmon_exit(ctrl);
	nvme_fault_inject_fini(&ctrl->fault_inject);
	nvme_debug_ctrl_exit(ctrl);
	nvme_sysfs_del_ctrl(ctrl);
	nvme_cdev_del_ctrl(ctrl);
	nvme_put_ctrl(ctrl);
}

void nvme_print_device_info(struct nvme_ctrl *ctrl)
{
	struct nvme_subsystem *subsys = ctrl->subsys;

	if (ctrl->ops->print_device_info) {
		ctrl->ops->print_device_info(ctrl);
		return;
	}
	if (!subsys)
		return;
	kprintf(NVME_LOG_PREFIX "%s: VID:%04x model:%.*s firmware:%.*s\n",
		ctrl->name, subsys->vendor_id,
		nvme_strlen(subsys->model, (int)sizeof(subsys->model)),
		subsys->model,
		nvme_strlen(subsys->firmware_rev,
			    (int)sizeof(subsys->firmware_rev)),
		subsys->firmware_rev);
}

/* ==========================================================================
 * Subsystems
 * ========================================================================== */

/* struct nvme_core_quirk_entry: model and firmware strings are space
 * padded in Identify; the table holds them NUL terminated (NULL: any). */
struct nvme_core_quirk_entry {
	uint16_t vid;
	const char *mn;
	const char *fr;
	unsigned long quirks;
};

static const struct nvme_core_quirk_entry nvme_core_quirks[] = {
	{
		/* This Toshiba device dies in any APST state. */
		.vid = 0x1179,
		.mn = "THNSF5256GPUK TOSHIBA",
		.quirks = NVME_QUIRK_NO_APST,
	},
	{
		/* This LiteON CL1-3D*-Q11 firmware races in suspend to idle
		 * (fixed in later firmware). */
		.vid = 0x14a4,
		.fr = "22301111",
		.quirks = NVME_QUIRK_SIMPLE_SUSPEND,
	},
	{
		/* Kioxia CD6-V / HPE PE8030: times out and aborts I/O under
		 * load (discards especially) with APST on. */
		.vid = 0x1e0f,
		.mn = "KCD6XVUL6T40",
		.quirks = NVME_QUIRK_NO_APST,
	},
	{
		/* The external Samsung X5 needs a delay before checking
		 * ready and more; it shares its PCI ID with the internal
		 * 970 Evo Plus, which wants none of it. */
		.vid = 0x144d,
		.mn = "Samsung Portable SSD X5",
		.quirks = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |
			  NVME_QUIRK_NO_DEEPEST_PS |
			  NVME_QUIRK_IGNORE_DEV_SUBNQN,
	},
};

/* `match' is NUL terminated, `idstr' space padded to `len'. */
static bool nvme_string_matches(const char *idstr, const char *match,
				size_t len)
{
	size_t matchlen;

	if (!match)
		return true;
	matchlen = kstrlen(match);
	WARN_ON_ONCE(matchlen > len);
	if (matchlen > len || kmemcmp(idstr, match, matchlen))
		return false;
	for (; matchlen < len; matchlen++)
		if (idstr[matchlen] != ' ')
			return false;
	return true;
}

unsigned long nvme_core_quirks_lookup(const struct nvme_id_ctrl *id,
				      uint16_t vid)
{
	unsigned long quirks = 0;
	uint16_t id_vid = le16_to_cpu(id->vid);
	size_t i;

	/* The table is keyed by Identify's VID only: a PCI vendor that
	 * differs from it (a rebadged controller) must not pull in another
	 * vendor's model/firmware quirks. */
	(void)vid;
	for (i = 0; i < sizeof(nvme_core_quirks) / sizeof(nvme_core_quirks[0]);
	     i++) {
		const struct nvme_core_quirk_entry *q = &nvme_core_quirks[i];

		if (q->vid != id_vid)
			continue;
		if (!nvme_string_matches(id->mn, q->mn, sizeof(id->mn)) ||
		    !nvme_string_matches(id->fr, q->fr, sizeof(id->fr)))
			continue;
		quirks |= q->quirks;
	}
	return quirks;
}

static void nvme_init_subnqn(struct nvme_subsystem *subsys,
			     struct nvme_ctrl *ctrl,
			     const struct nvme_id_ctrl *id)
{
	size_t nqnlen = 0;
	int off;

	if (!(ctrl->quirks & NVME_QUIRK_IGNORE_DEV_SUBNQN)) {
		while (nqnlen < NVMF_NQN_SIZE && id->subnqn[nqnlen])
			nqnlen++;
		if (nqnlen > 0 && nqnlen < NVMF_NQN_SIZE) {
			kmemcpy(subsys->subnqn, id->subnqn, nqnlen);
			subsys->subnqn[nqnlen] = '\0';
			return;
		}
		if (ctrl->vs >= NVME_VS(1, 2, 1))
			kprintf(NVME_LOG_PREFIX
				"%s: missing or invalid SUBNQN field\n",
				ctrl->name);
	}

	/* A substitute NQN in the historic form (close to the one of the
	 * base specification): vendor, subsystem vendor, serial, model. */
	off = ksnprintf(subsys->subnqn, NVMF_NQN_SIZE,
			"nqn.2014.08.org.nvmexpress:%04x%04x",
			le16_to_cpu(id->vid), le16_to_cpu(id->ssvid));
	BUG_ON(off < 0 || (size_t)off + sizeof(id->sn) + sizeof(id->mn) >=
				  sizeof(subsys->subnqn));
	kmemcpy(subsys->subnqn + off, id->sn, sizeof(id->sn));
	off += (int)sizeof(id->sn);
	kmemcpy(subsys->subnqn + off, id->mn, sizeof(id->mn));
	off += (int)sizeof(id->mn);
	kmemset(subsys->subnqn + off, 0, sizeof(subsys->subnqn) - (size_t)off);
}

static void nvme_destroy_subsystem(struct nvme_subsystem *subsys)
{
	mutex_lock(&nvme_subsystems_lock);
	list_del(&subsys->entry);
	mutex_unlock(&nvme_subsystems_lock);

	nvme_sysfs_del_subsys(subsys);
	ida_destroy(&subsys->ns_ida);
	if (subsys->instance >= 0)
		ida_free(&nvme_instance_ida, (unsigned int)subsys->instance);
	kfree(subsys);
}

void nvme_put_subsystem(struct nvme_subsystem *subsys)
{
	might_sleep();
	if (refcount_dec_and_test(&subsys->ref))
		nvme_destroy_subsystem(subsys);
}

static struct nvme_subsystem *nvme_find_get_subsystem_locked(
	const char *subsysnqn)
{
	struct nvme_subsystem *subsys;

	lockdep_assert_held(&nvme_subsystems_lock);
	/* Discovery subsystems never match: every discovery controller is
	 * its own subsystem. */
	if (!kstrcmp(subsysnqn, NVME_DISC_SUBSYS_NAME))
		return NULL;
	list_for_each_entry(subsys, &nvme_subsystems, entry) {
		if (kstrcmp(subsys->subnqn, subsysnqn))
			continue;
		if (!refcount_inc_not_zero(&subsys->ref))
			continue;
		return subsys;
	}
	return NULL;
}

static bool nvme_admin_ctrl(const struct nvme_ctrl *ctrl)
{
	return ctrl->cntrltype == NVME_CTRL_TYPE_ADMIN;
}

static bool nvme_validate_cntlid_locked(struct nvme_subsystem *subsys,
					struct nvme_ctrl *ctrl,
					const struct nvme_id_ctrl *id)
{
	struct nvme_ctrl *tmp;

	lockdep_assert_held(&nvme_subsystems_lock);
	list_for_each_entry(tmp, &subsys->ctrls, subsys_entry) {
		if (nvme_state_terminal(tmp))
			continue;
		if (tmp->cntlid == ctrl->cntlid) {
			kprintf(NVME_LOG_PREFIX
				"%s: duplicate cntlid %u with %s, subsys %s, "
				"rejecting\n",
				ctrl->name, ctrl->cntlid, tmp->name,
				subsys->subnqn);
			return false;
		}
		if (id->cmic & NVME_CTRL_CMIC_MULTI_CTRL)
			continue;
		kprintf(NVME_LOG_PREFIX
			"%s: subsystem does not support multiple "
			"controllers\n",
			ctrl->name);
		return false;
	}
	return true;
}

static int nvme_init_subsystem(struct nvme_ctrl *ctrl,
			       const struct nvme_id_ctrl *id)
{
	struct nvme_subsystem *subsys, *found;
	int ret;

	might_sleep();
	subsys = kcalloc(1, sizeof(*subsys));
	if (!subsys)
		return -ENOMEM;

	subsys->instance = -1;
	mutex_init(&subsys->lock, "nvme_subsys");
	refcount_set(&subsys->ref, 1);
	INIT_LIST_HEAD(&subsys->ctrls);
	INIT_LIST_HEAD(&subsys->nsheads);
	INIT_LIST_HEAD(&subsys->entry);
	nvme_init_subnqn(subsys, ctrl, id);
	BUILD_BUG_ON(sizeof(subsys->serial) != sizeof(id->sn));
	BUILD_BUG_ON(sizeof(subsys->model) != sizeof(id->mn));
	BUILD_BUG_ON(sizeof(subsys->firmware_rev) != sizeof(id->fr));
	kmemcpy(subsys->serial, id->sn, sizeof(subsys->serial));
	kmemcpy(subsys->model, id->mn, sizeof(subsys->model));
	subsys->vendor_id = le16_to_cpu(id->vid);
	subsys->cmic = id->cmic;

	/* Versions before 1.4 do not necessarily report a valid type. */
	if (id->cntrltype == NVME_CTRL_DISC ||
	    !kstrcmp(subsys->subnqn, NVME_DISC_SUBSYS_NAME))
		subsys->subtype = NVME_NQN_DISC;
	else
		subsys->subtype = NVME_NQN_NVME;
	nvme_mpath_default_iopolicy(subsys);

	mutex_lock(&nvme_subsystems_lock);
	found = nvme_find_get_subsystem_locked(subsys->subnqn);
	if (found) {
		kfree(subsys);
		subsys = found;
		if (!nvme_validate_cntlid_locked(subsys, ctrl, id)) {
			ret = -EINVAL;
			goto out_put_subsystem;
		}
	} else {
		ida_init(&subsys->ns_ida, "nvme_subsys_ns");
		subsys->instance = ctrl->instance;
		ret = nvme_sysfs_add_subsys(subsys);
		if (ret) {
			kprintf(NVME_LOG_PREFIX
				"%s: failed to register the subsystem (%d)\n",
				ctrl->name, ret);
			/* The instance number stays the controller's. */
			subsys->instance = -1;
			ida_destroy(&subsys->ns_ida);
			kfree(subsys);
			goto out_unlock;
		}
		list_add_tail(&subsys->entry, &nvme_subsystems);
	}

	ctrl->subsys = subsys;
	mutex_lock(&subsys->lock);
	list_add_tail(&ctrl->subsys_entry, &subsys->ctrls);
	mutex_unlock(&subsys->lock);
	mutex_unlock(&nvme_subsystems_lock);
	return 0;

out_put_subsystem:
	mutex_unlock(&nvme_subsystems_lock);
	nvme_put_subsystem(subsys);
	return ret;
out_unlock:
	mutex_unlock(&nvme_subsystems_lock);
	return ret;
}

/* ==========================================================================
 * Identify Controller and the limits / capabilities derived from it
 * ========================================================================== */

static uint32_t nvme_mps_to_sectors(struct nvme_ctrl *ctrl, uint32_t units)
{
	uint32_t page_shift = (uint32_t)NVME_CAP_MPSMIN(ctrl->cap) + 12;
	uint32_t shift = units + page_shift - 9;

	if (shift >= 32)
		return 0xffffffffu;
	return 1u << shift;
}

static uint32_t nvme_min_not_zero(uint32_t a, uint32_t b)
{
	if (!a)
		return b;
	if (!b)
		return a;
	return a < b ? a : b;
}

/* The effects the established driver always assumes on top of the log
 * (or instead of it): Format and Sanitize change everything and need
 * exclusive access, writes change logical block content; Security
 * Receive's "exclusive" restriction is masked off (it cannot be enforced
 * on the admin queue and would only freeze I/O for nothing). */
static void nvme_init_known_nvm_effects(struct nvme_ctrl *ctrl)
{
	struct nvme_effects_log *log = ctrl->effects;

	log->acs[nvme_admin_format_nvm] |=
		cpu_to_le32(NVME_CMD_EFFECTS_LBCC | NVME_CMD_EFFECTS_NCC |
			    NVME_CMD_EFFECTS_CSE_MASK);
	log->acs[nvme_admin_sanitize_nvm] |= cpu_to_le32(
		NVME_CMD_EFFECTS_LBCC | NVME_CMD_EFFECTS_CSE_MASK);
	log->acs[nvme_admin_security_recv] &=
		cpu_to_le32(~(uint32_t)NVME_CMD_EFFECTS_CSE_MASK);

	log->iocs[nvme_cmd_write] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC);
	log->iocs[nvme_cmd_write_zeroes] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC);
	log->iocs[nvme_cmd_write_uncor] |= cpu_to_le32(NVME_CMD_EFFECTS_LBCC);
}

/* Install an all-zero effects log for `csi' (no log page support). */
static int nvme_init_effects_log(struct nvme_ctrl *ctrl, uint8_t csi,
				 struct nvme_effects_log **log)
{
	struct nvme_effects_log *effects, *expected = NULL;

	if (csi >= NVME_NR_EFFECTS_CSI)
		return -EINVAL;
	effects = kcalloc(1, sizeof(*effects));
	if (!effects)
		return -ENOMEM;
	if (!__atomic_compare_exchange_n(&ctrl->cels[csi], &expected, effects,
					 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		kfree(effects);
		effects = expected;
	}
	*log = effects;
	return 0;
}

int nvme_get_effects_log(struct nvme_ctrl *ctrl, uint8_t csi,
			 struct nvme_effects_log **log)
{
	struct nvme_effects_log *cel, *expected = NULL;
	int ret;

	might_sleep();
	if (csi >= NVME_NR_EFFECTS_CSI)
		return -EINVAL;
	cel = __atomic_load_n(&ctrl->cels[csi], __ATOMIC_ACQUIRE);
	if (cel)
		goto out;

	BUILD_BUG_ON(sizeof(struct nvme_effects_log) != 4096);
	cel = kcalloc(1, sizeof(*cel));
	if (!cel)
		return -ENOMEM;
	ret = nvme_get_log(ctrl, 0x00, NVME_LOG_CMD_EFFECTS, 0, csi, cel,
			   sizeof(*cel), 0);
	if (ret) {
		kfree(cel);
		return ret;
	}
	if (!__atomic_compare_exchange_n(&ctrl->cels[csi], &expected, cel, 0,
					 __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		/* Someone else read it meanwhile: keep theirs. */
		kfree(cel);
		cel = expected;
	}
out:
	*log = cel;
	return 0;
}

static int nvme_init_effects(struct nvme_ctrl *ctrl,
			     const struct nvme_id_ctrl *id)
{
	int ret = 0;

	if (ctrl->effects)
		return 0;
	if (id->lpa & NVME_CTRL_LPA_CMD_EFFECTS_LOG) {
		ret = nvme_get_effects_log(ctrl, NVME_CSI_NVM, &ctrl->effects);
		if (ret < 0)
			return ret;
		/* An NVMe status (log not readable after all): fall back. */
		if (ret > 0)
			ctrl->effects = NULL;
	}
	if (!ctrl->effects) {
		ret = nvme_init_effects_log(ctrl, NVME_CSI_NVM, &ctrl->effects);
		if (ret < 0)
			return ret;
	}
	nvme_init_known_nvm_effects(ctrl);
	return 0;
}

static int nvme_init_identify(struct nvme_ctrl *ctrl)
{
	struct blk_limits lim;
	struct nvme_id_ctrl *id;
	uint32_t max_hw_sectors;
	int ret;

	might_sleep();
	ret = nvme_identify_ctrl(ctrl, &id);
	if (ret) {
		kprintf(NVME_LOG_PREFIX "%s: Identify Controller failed (%d)\n",
			ctrl->name, ret);
		return -EIO;
	}

	ctrl->cntlid = le16_to_cpu(id->cntlid);

	if (!ctrl->identified) {
		/* Quirks may depend on the firmware revision, so in theory
		 * they could change across a reset; they are looked up once,
		 * as established. */
		ctrl->quirks |= nvme_core_quirks_lookup(
			id, ctrl->pdev ? ctrl->pdev->vendor_id : 0);

		ret = nvme_init_subsystem(ctrl, id);
		if (ret)
			goto out_free;

		ret = nvme_init_effects(ctrl, id);
		if (ret)
			goto out_free;
	}
	/* The subsystem's controllers come up concurrently (each reset work
	 * copies its revision): one writer at a time. */
	mutex_lock(&ctrl->subsys->lock);
	kmemcpy(ctrl->subsys->firmware_rev, id->fr,
		sizeof(ctrl->subsys->firmware_rev));
	mutex_unlock(&ctrl->subsys->lock);

	if (nvme_core_param.force_apst &&
	    (ctrl->quirks & NVME_QUIRK_NO_DEEPEST_PS)) {
		kprintf(NVME_LOG_PREFIX
			"%s: forcibly allowing all power states due to "
			"nvme_core.force_apst -- use at your own risk\n",
			ctrl->name);
		ctrl->quirks &= ~(unsigned long)NVME_QUIRK_NO_DEEPEST_PS;
	}

	ctrl->crdt[0] = le16_to_cpu(id->crdt1);
	ctrl->crdt[1] = le16_to_cpu(id->crdt2);
	ctrl->crdt[2] = le16_to_cpu(id->crdt3);

	ctrl->oacs = le16_to_cpu(id->oacs);
	ctrl->oncs = le16_to_cpu(id->oncs);
	ctrl->mtfa = le16_to_cpu(id->mtfa);
	ctrl->oaes = le32_to_cpu(id->oaes);
	ctrl->wctemp = le16_to_cpu(id->wctemp);
	ctrl->cctemp = le16_to_cpu(id->cctemp);
	ctrl->hctma = (uint8_t)le16_to_cpu(id->hctma);
	ctrl->mntmt = le16_to_cpu(id->mntmt);
	ctrl->mxtmt = le16_to_cpu(id->mxtmt);
	ctrl->tmpthha = id->tmpthha;
	ctrl->cmic = id->cmic;
	ctrl->frmw = id->frmw;
	ctrl->lpa = id->lpa;
	ctrl->elpe = id->elpe;
	ctrl->aerl = id->aerl;
	ctrl->nn = le32_to_cpu(id->nn);
	ctrl->sanicap_bits = (uint8_t)(le32_to_cpu(id->sanicap) & 0xff);
	ctrl->rtd3e = le32_to_cpu(id->rtd3e);
	ctrl->rtd3r = le32_to_cpu(id->rtd3r);

	__atomic_store_n(&ctrl->abort_limit, (int)id->acl + 1,
			 __ATOMIC_RELEASE);
	ctrl->vwc = id->vwc;
	if (id->mdts)
		max_hw_sectors = nvme_mps_to_sectors(ctrl, id->mdts);
	else
		max_hw_sectors = 0xffffffffu;
	ctrl->max_hw_sectors =
		nvme_min_not_zero(ctrl->max_hw_sectors, max_hw_sectors);

	if (ctrl->admin_q) {
		queue_limits_start_update(ctrl->admin_q, &lim);
		nvme_set_ctrl_limits(ctrl, &lim, true);
		ret = queue_limits_commit_update(ctrl->admin_q, &lim);
		if (ret)
			goto out_free;
	}

	ctrl->sgls = le32_to_cpu(id->sgls);
	ctrl->kas = le16_to_cpu(id->kas);
	ctrl->max_namespaces = le32_to_cpu(id->mnan);
	__atomic_store_n(&ctrl->ctratt, le32_to_cpu(id->ctratt), __ATOMIC_RELAXED);

	ctrl->cntrltype = (enum nvme_ctrl_type_host)id->cntrltype;
	ctrl->dctype = id->dctype;

	if (id->rtd3e) {
		/* us -> s, at least the knob, at most a minute */
		uint32_t transition_time = le32_to_cpu(id->rtd3e) / 1000000u;
		unsigned int knob = nvme_core_param.shutdown_timeout_s;

		if (transition_time < knob)
			transition_time = knob;
		if (transition_time > 60)
			transition_time = 60;
		ctrl->shutdown_timeout_s = transition_time;
		if (ctrl->shutdown_timeout_s != knob)
			kprintf(NVME_LOG_PREFIX
				"%s: D3 entry latency set to %u seconds\n",
				ctrl->name, ctrl->shutdown_timeout_s);
	} else {
		ctrl->shutdown_timeout_s = nvme_core_param.shutdown_timeout_s;
	}

	ctrl->npss = id->npss;
	ctrl->apsta = id->apsta;
	if (ctrl->quirks & NVME_QUIRK_NO_APST) {
		if (nvme_core_param.force_apst && id->apsta) {
			kprintf(NVME_LOG_PREFIX
				"%s: forcibly allowing APST due to "
				"nvme_core.force_apst -- use at your own "
				"risk\n",
				ctrl->name);
			ctrl->apst_enabled = true;
		} else {
			ctrl->apst_enabled = false;
		}
	} else {
		ctrl->apst_enabled = id->apsta != 0;
	}
	BUILD_BUG_ON(sizeof(ctrl->psd) != sizeof(id->psd));
	kmemcpy(ctrl->psd, id->psd, sizeof(ctrl->psd));

	/* PCIe: the host memory buffer sizes (no fabrics capsule fields). */
	ctrl->hmpre = le32_to_cpu(id->hmpre);
	ctrl->hmmin = le32_to_cpu(id->hmmin);
	ctrl->hmminds = le32_to_cpu(id->hmminds);
	ctrl->hmmaxd = le16_to_cpu(id->hmmaxd);

	ret = nvme_mpath_init_identify(ctrl, id);
	if (ret < 0)
		goto out_free;
	ret = 0;

	ctrl->awupf = le16_to_cpu(id->awupf);
	ctrl->acwu = le16_to_cpu(id->acwu);
out_free:
	kfree(id);
	return ret;
}

/* Keep-alive is needed only when the controller runs the Keep Alive
 * Timer: KAS reports the feature, and a non-zero KATO (Get Features 0Fh,
 * milliseconds) means the timer is on.  A PCIe controller normally has
 * it off; then nothing is sent, as established. */
static void nvme_init_kato(struct nvme_ctrl *ctrl)
{
	uint32_t kato_ms = 0;
	int ret;

	if (!ctrl->kas) {
		ctrl->kato_s = 0;
		return;
	}
	ret = nvme_get_features(ctrl, NVME_FEAT_KATO, 0, NULL, 0, &kato_ms);
	if (ret) {
		ctrl->kato_s = 0;
		return;
	}
	ctrl->kato_s = (kato_ms + 999) / 1000;
	if (ctrl->kato_s)
		kprintf(NVME_LOG_PREFIX
			"%s: keep alive timer active (KATO %u ms): sending "
			"keep-alives every %u ms\n",
			ctrl->name, kato_ms, ctrl->kato_s * 1000 / 2);
}

int nvme_init_ctrl_finish(struct nvme_ctrl *ctrl, bool was_suspended)
{
	int ret;

	might_sleep();
	(void)was_suspended;
	ret = ctrl->ops->reg_read32(ctrl, NVME_REG_VS, &ctrl->vs);
	if (ret) {
		kprintf(NVME_LOG_PREFIX "%s: reading VS failed (%d)\n",
			ctrl->name, ret);
		return ret;
	}

	if ((uint32_t)NVME_CAP_MQES(ctrl->cap) < ctrl->sqsize)
		ctrl->sqsize = (uint16_t)NVME_CAP_MQES(ctrl->cap);

	if (ctrl->vs >= NVME_VS(1, 1, 0))
		ctrl->subsystem = NVME_CAP_NSSRC(ctrl->cap) != 0;

	ret = nvme_init_identify(ctrl);
	if (ret)
		return ret;

	if (nvme_admin_ctrl(ctrl)) {
		/* An administrative controller has the admin queue only. */
		kprintf(NVME_LOG_PREFIX
			"%s: subsystem %s is an administrative controller\n",
			ctrl->name, ctrl->subsys->subnqn);
		ctrl->queue_count = 1;
	}

	ret = nvme_configure_apst(ctrl);
	if (ret < 0)
		return ret;

	ret = nvme_configure_timestamp(ctrl);
	if (ret < 0)
		return ret;

	/* Streams directive (nvme_core.streams); an NVMe status only means
	 * no streams. */
	ret = nvme_enable_streams(ctrl);
	if (ret < 0)
		return ret;

	ret = nvme_configure_host_options(ctrl);
	if (ret < 0)
		return ret;

	if (!ctrl->identified) {
		/* hwmon is optional: only an interrupted reset is an error. */
		ret = nvme_hwmon_init(ctrl);
		if (ret == -EINTR)
			return ret;
	}

	nvme_clear_bit(NVME_CTRL_DIRTY_CAPABILITY, &ctrl->flags);
	ctrl->identified = true;

	nvme_stop_keep_alive(ctrl);
	nvme_init_kato(ctrl);
	nvme_start_keep_alive(ctrl);
	return 0;
}

/* ==========================================================================
 * Features: queues, power, APST, cache, timestamp, host behavior
 * ========================================================================== */

int nvme_set_queue_count(struct nvme_ctrl *ctrl, int *count)
{
	uint32_t q_count, result = 0;
	int status, nr_io_queues;

	might_sleep();
	if (*count < 1)
		return -EINVAL;
	q_count = (uint32_t)(*count - 1) | ((uint32_t)(*count - 1) << 16);
	status = nvme_set_features(ctrl, NVME_FEAT_NUM_QUEUES, q_count, NULL,
				   0, &result);
	/* A host error or a lost controller: nothing more can be done. */
	if (status < 0 || status == NVME_SC_HOST_PATH_ERROR)
		return status;

	/* A degraded controller may refuse; bring it up with the admin
	 * queue only, which may be the only way to repair it. */
	if (status > 0) {
		kprintf(NVME_LOG_PREFIX "%s: could not set queue count (%d)\n",
			ctrl->name, status);
		*count = 0;
	} else {
		uint32_t nsqa = result & 0xffff, ncqa = result >> 16;

		nr_io_queues = (int)((nsqa < ncqa ? nsqa : ncqa) + 1);
		if (nr_io_queues < *count)
			*count = nr_io_queues;
		ctrl->nr_io_queues_granted = (uint32_t)nr_io_queues;
	}
	return 0;
}

int nvme_set_power_state(struct nvme_ctrl *ctrl, uint32_t ps)
{
	might_sleep();
	if (ps > ctrl->npss)
		return -EINVAL;
	return nvme_set_features(ctrl, NVME_FEAT_POWER_MGMT, ps, NULL, 0,
				 NULL);
}

int nvme_get_power_state(struct nvme_ctrl *ctrl, uint32_t *ps)
{
	might_sleep();
	return nvme_get_features(ctrl, NVME_FEAT_POWER_MGMT, 0, NULL, 0, ps);
}

int nvme_set_vwc(struct nvme_ctrl *ctrl, bool enable)
{
	might_sleep();
	if (!(ctrl->vwc & NVME_CTRL_VWC_PRESENT))
		return -EOPNOTSUPP;
	return nvme_set_features(ctrl, NVME_FEAT_VOLATILE_WC,
				 enable ? NVME_FEAT_VWC_WCE : 0, NULL, 0,
				 NULL);
}

int nvme_configure_timestamp(struct nvme_ctrl *ctrl)
{
	uint64_t ts_ms;
	uint8_t data[8];
	int ret;

	might_sleep();
	if (!(ctrl->oncs & NVME_CTRL_ONCS_TIMESTAMP))
		return 0;

	/* 48-bit milliseconds since the epoch in the first 6 bytes. */
	ts_ms = timer_get_realtime_ns() / 1000000ULL;
	kmemset(data, 0, sizeof(data));
	put_unaligned_le64(ts_ms & 0x0000ffffffffffffULL, data);
	ret = nvme_set_features(ctrl, NVME_FEAT_TIMESTAMP, 0, data,
				sizeof(data), NULL);
	if (ret) {
		static int warned;

		if (nvme_first_time(&warned)) {
			kprintf(NVME_LOG_PREFIX
				"%s: could not set timestamp (%d)\n",
				ctrl->name, ret);
		}
	}
	return ret;
}

int nvme_configure_host_options(struct nvme_ctrl *ctrl)
{
	struct nvme_feat_host_behavior *host;
	uint8_t acre = 0, lbafee = 0;
	int ret;

	might_sleep();
	/* Advanced command retry only pays when retry delays are reported;
	 * the LBA format extension only with extended LBA formats. */
	if (ctrl->crdt[0])
		acre = NVME_ENABLE_ACRE;
	if (nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_ELBAS)
		lbafee = NVME_ENABLE_LBAFEE;
	if (!acre && !lbafee)
		return 0;

	BUILD_BUG_ON(sizeof(struct nvme_feat_host_behavior) != 512);
	host = kcalloc(1, sizeof(*host));
	if (!host)
		return 0;
	host->acre = acre;
	host->lbafee = lbafee;
	ret = nvme_set_features(ctrl, NVME_FEAT_HOST_BEHAVIOR, 0, host,
				sizeof(*host), NULL);
	kfree(host);
	return ret;
}

/* Is `total_latency_us' within the primary / secondary tolerance, and
 * not the tolerance band already used for a deeper state?  Then the
 * band's timeout is the transition time. */
static bool nvme_apst_get_transition_time(uint64_t total_latency_us,
					  uint64_t *transition_time_ms,
					  unsigned int *last_index,
					  uint64_t primary_timeout_ms,
					  uint64_t secondary_timeout_ms,
					  uint64_t primary_latency_tol_us,
					  uint64_t secondary_latency_tol_us)
{
	if (total_latency_us <= primary_latency_tol_us) {
		if (*last_index == 1)
			return false;
		*last_index = 1;
		*transition_time_ms = primary_timeout_ms;
		return true;
	}
	if (secondary_timeout_ms &&
	    total_latency_us <= secondary_latency_tol_us) {
		if (*last_index <= 2)
			return false;
		*last_index = 2;
		*transition_time_ms = secondary_timeout_ms;
		return true;
	}
	return false;
}

int nvme_apst_build_table(const struct nvme_id_power_state *psd,
			  unsigned int npss, unsigned long quirks,
			  uint64_t ps_max_latency_us,
			  uint64_t primary_timeout_ms,
			  uint64_t secondary_timeout_ms,
			  uint64_t primary_latency_tol_us,
			  uint64_t secondary_latency_tol_us,
			  __le64 entries[32], uint64_t *max_lat_us)
{
	unsigned int last_lt_index = 0xffffffffu;
	uint64_t target = 0;
	int max_ps = -1;
	int state;

	BUG_ON(npss > 31);
	kmemset(entries, 0, 32 * sizeof(__le64));
	*max_lat_us = 0;

	/* From the lowest-power state up: lower numbers use more power;
	 * NPSS is the index of the lowest-power state.  Each state's entry
	 * points at the deepest usable state below it found so far. */
	for (state = (int)npss; state >= 0; state--) {
		uint64_t total_latency_us, exit_latency_us, transition_ms;

		if (target)
			entries[state] = cpu_to_le64(target);

		/* The deepest state is off limits when quirked. */
		if (state == (int)npss && (quirks & NVME_QUIRK_NO_DEEPEST_PS))
			continue;
		/* Only non-operational states are idle targets. */
		if (!(psd[state].flags & NVME_PS_FLAGS_NON_OP_STATE))
			continue;
		exit_latency_us = (uint64_t)le32_to_cpu(psd[state].exit_lat);
		if (exit_latency_us > ps_max_latency_us)
			continue;
		total_latency_us = exit_latency_us +
				   le32_to_cpu(psd[state].entry_lat);

		if (primary_timeout_ms && primary_latency_tol_us) {
			if (!nvme_apst_get_transition_time(
				    total_latency_us, &transition_ms,
				    &last_lt_index, primary_timeout_ms,
				    secondary_timeout_ms,
				    primary_latency_tol_us,
				    secondary_latency_tol_us))
				continue;
		} else {
			/* At most 2 % of the time in transitions: idle 50x
			 * the round trip before going down. */
			transition_ms = (total_latency_us + 19) / 20;
			if (transition_ms > (1u << 24) - 1)
				transition_ms = (1u << 24) - 1;
		}

		target = ((uint64_t)state << 3) | (transition_ms << 8);
		if (max_ps == -1)
			max_ps = state;
		if (total_latency_us > *max_lat_us)
			*max_lat_us = total_latency_us;
	}
	return max_ps;
}

int nvme_configure_apst(struct nvme_ctrl *ctrl)
{
	struct nvme_feat_auto_pst *table;
	uint64_t max_lat_us = 0;
	unsigned int apste = 0;
	int max_ps = -1;
	int ret;

	might_sleep();
	/* No APST, or not identified yet: nothing to do. */
	if (!ctrl->apsta)
		return 0;
	if (ctrl->npss > 31) {
		kprintf(NVME_LOG_PREFIX "%s: NPSS is invalid; not using APST\n",
			ctrl->name);
		return 0;
	}

	BUILD_BUG_ON(sizeof(struct nvme_feat_auto_pst) != 256);
	table = kcalloc(1, sizeof(*table));
	if (!table)
		return 0;

	if (ctrl->apst_enabled && ctrl->ps_max_latency_us) {
		max_ps = nvme_apst_build_table(
			ctrl->psd, ctrl->npss, ctrl->quirks,
			ctrl->ps_max_latency_us,
			nvme_core_param.apst_primary_timeout_ms,
			nvme_core_param.apst_secondary_timeout_ms,
			nvme_core_param.apst_primary_latency_tol_us,
			nvme_core_param.apst_secondary_latency_tol_us,
			table->entries, &max_lat_us);
		if (max_ps == -1)
			kprintf(NVME_LOG_PREFIX
				"%s: APST enabled but no non-operational "
				"states are available\n",
				ctrl->name);
		else
			kprintf(NVME_LOG_PREFIX
				"%s: APST enabled: max PS = %d, max "
				"round-trip latency = %lluus\n",
				ctrl->name, max_ps,
				(unsigned long long)max_lat_us);
		apste = 1;
	}

	ret = nvme_set_features(ctrl, NVME_FEAT_AUTO_PST, apste, table,
				sizeof(*table), NULL);
	if (ret)
		kprintf(NVME_LOG_PREFIX "%s: failed to set APST feature (%d)\n",
			ctrl->name, ret);
	kfree(table);
	return ret;
}

/* ==========================================================================
 * Keep-alive
 * ========================================================================== */

/* Half the keep-alive timeout; a quarter with traffic based keep-alive
 * (one completion may postpone a keep-alive by up to two periods). */
static uint64_t nvme_keep_alive_work_period_ms(struct nvme_ctrl *ctrl)
{
	uint64_t delay = (uint64_t)ctrl->kato_s * 1000 / 2;

	if (nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_TBKAS)
		delay /= 2;
	return delay;
}

static void nvme_queue_keep_alive_work(struct nvme_ctrl *ctrl)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t delay_ms = nvme_keep_alive_work_period_ms(ctrl);
	uint64_t next = ctrl->ka_last_check_ns + delay_ms * 1000000ULL;

	if (now > next)
		delay_ms = 0;
	else
		delay_ms = (next - now) / 1000000ULL;
	nvme_queue_delayed_work(&ctrl->ka_work, delay_ms);
}

static void nvme_keep_alive_end_io(struct blk_request *rq, int status)
{
	struct nvme_ctrl *ctrl = rq->end_io_data;
	uint64_t now = hrtimer_now_ns();
	uint64_t timeout_ns = (uint64_t)rq->timeout_ms * 1000000ULL;
	uint64_t start = rq->deadline_ns > timeout_ns ?
				 rq->deadline_ns - timeout_ns :
				 now;
	uint64_t rtt_ms = now > start ? (now - start) / 1000000ULL : 0;
	uint64_t delay_ms = nvme_keep_alive_work_period_ms(ctrl);
	enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);
	uint16_t nvme_status = nvme_req(rq)->status;

	/* Run the next check one period after this one was sent. */
	if (rtt_ms <= delay_ms) {
		delay_ms -= rtt_ms;
	} else {
		WARN_RATELIMIT(1, "nvme: %s: long keepalive RTT (%llu ms)",
			       ctrl->name, (unsigned long long)rtt_ms);
		delay_ms = 0;
	}

	blk_mq_free_request(rq);

	if (status || nvme_status) {
		WARN_RATELIMIT(1, "nvme: %s: keep-alive failed (%d/0x%x)",
			       ctrl->name, status, nvme_status);
		return;
	}

	ctrl->ka_last_check_ns = now;
	ctrl->comp_seen = false;
	if (state == NVME_CTRL_LIVE || state == NVME_CTRL_CONNECTING)
		nvme_queue_delayed_work(&ctrl->ka_work, delay_ms);
}

static void nvme_keep_alive_work(void *arg)
{
	struct nvme_ctrl *ctrl = arg;
	bool comp_seen = ctrl->comp_seen;
	struct blk_request *rq;
	int err = 0;

	ctrl->ka_last_check_ns = hrtimer_now_ns();

	if ((nvme_ctrl_ctratt(ctrl) & NVME_CTRL_ATTR_TBKAS) && comp_seen) {
		/* Traffic counts as keep-alive: just look again later. */
		ctrl->comp_seen = false;
		nvme_queue_keep_alive_work(ctrl);
		return;
	}
	if (!ctrl->admin_q || !ctrl->kato_s)
		return;

	rq = blk_mq_alloc_request(ctrl->admin_q, nvme_req_op(&ctrl->ka_cmd),
				  BLK_MQ_REQ_RESERVED | BLK_MQ_REQ_NOWAIT,
				  &err);
	if (!rq) {
		/* No tag for the keep-alive: the controller is in trouble. */
		kprintf(NVME_LOG_PREFIX "%s: keep-alive failed: %d\n",
			ctrl->name, err);
		nvme_reset_ctrl(ctrl);
		return;
	}
	nvme_init_request(rq, &ctrl->ka_cmd);
	rq->timeout_ms = ctrl->kato_s * 1000;
	rq->end_io = nvme_keep_alive_end_io;
	rq->end_io_data = ctrl;
	blk_execute_rq_nowait(rq, 0);
}

void nvme_start_keep_alive(struct nvme_ctrl *ctrl)
{
	if (likely(ctrl->kato_s == 0))
		return;
	nvme_queue_keep_alive_work(ctrl);
}

void nvme_stop_keep_alive(struct nvme_ctrl *ctrl)
{
	if (likely(ctrl->kato_s == 0))
		return;
	nvme_cancel_work_sync(&ctrl->ka_work);
}

/* A Set Features KATO pass-through succeeded: follow the new timeout
 * (cdw11 in milliseconds). */
static void nvme_update_keep_alive(struct nvme_ctrl *ctrl, uint32_t kato_ms)
{
	unsigned int new_kato = (kato_ms + 999) / 1000;

	kprintf(NVME_LOG_PREFIX
		"%s: keep alive interval updated from %u ms to %u ms\n",
		ctrl->name, ctrl->kato_s * 1000 / 2, new_kato * 1000 / 2);
	nvme_stop_keep_alive(ctrl);
	ctrl->kato_s = new_kato;
	nvme_start_keep_alive(ctrl);
}

/* nvme_passthru_end (nvme_admin.c) after a successful Set Features KATO. */
void nvme_core_kato_changed(struct nvme_ctrl *ctrl, uint32_t kato_ms)
{
	nvme_update_keep_alive(ctrl, kato_ms);
}

/* ==========================================================================
 * Asynchronous events
 * ========================================================================== */

/* The events the established driver enables (when OAES offers them). */
#define NVME_AEN_SUPPORTED                                                \
	(NVME_AEN_CFG_NS_ATTR | NVME_AEN_CFG_FW_ACT |                     \
	 NVME_AEN_CFG_ANA_CHANGE | NVME_AEN_CFG_DISC_CHANGE)
/* The ones LikeOS adds because nobody in userland reads the log pages
 * that clear them: SMART critical warnings and the aggregate / telemetry
 * notices (each processed by async_event_work). */
#define NVME_AEN_LIKEOS_EXTRA                                             \
	(NVME_AEN_CFG_TELEMETRY | NVME_AEN_CFG_PLM_AGGREGATE |            \
	 NVME_AEN_CFG_LBA_STATUS | NVME_AEN_CFG_ENDGRP_AGGREGATE |        \
	 NVME_AEN_CFG_NSS_SHUTDOWN | NVME_AEN_CFG_TEMP_HYSTERESIS)

static uint32_t nvme_aen_smart_mask(const struct nvme_ctrl *ctrl)
{
	uint32_t mask = NVME_SMART_CRIT_SPARE | NVME_SMART_CRIT_TEMPERATURE |
			NVME_SMART_CRIT_RELIABILITY | NVME_SMART_CRIT_MEDIA |
			NVME_SMART_CRIT_VOLATILE_MEMORY;

	if (ctrl->vs >= NVME_VS(1, 4, 0))
		mask |= NVME_SMART_CRIT_PMR_RO;
	return mask;
}

static void nvme_enable_aen(struct nvme_ctrl *ctrl)
{
	uint32_t base = ctrl->oaes & NVME_AEN_SUPPORTED;
	uint32_t extra = (ctrl->oaes & NVME_AEN_LIKEOS_EXTRA) |
			 nvme_aen_smart_mask(ctrl);
	uint32_t result = 0;
	int status;

	might_sleep();
	status = nvme_set_features(ctrl, NVME_FEAT_ASYNC_EVENT, base | extra,
				   NULL, 0, &result);
	if (status == 0) {
		ctrl->aen_cfg = base | extra;
	} else if (status > 0) {
		/* Refused (an enable bit it does not know): fall back to the
		 * established set. */
		ctrl->aen_cfg = 0;
		if (base) {
			status = nvme_set_features(ctrl, NVME_FEAT_ASYNC_EVENT,
						   base, NULL, 0, &result);
			if (status)
				kprintf(NVME_LOG_PREFIX
					"%s: failed to configure AEN (cfg "
					"%x)\n",
					ctrl->name, base);
			else
				ctrl->aen_cfg = base;
		}
	} else {
		kprintf(NVME_LOG_PREFIX "%s: failed to configure AEN (%d)\n",
			ctrl->name, status);
	}

	nvme_queue_work(&ctrl->async_event_work);
}

static bool nvme_ctrl_pp_status(struct nvme_ctrl *ctrl)
{
	uint32_t csts;

	if (ctrl->ops->reg_read32(ctrl, NVME_REG_CSTS, &csts))
		return false;
	if (csts == ~0u)
		return false;
	return (nvme_ctrl_config(ctrl) & NVME_CC_ENABLE) && (csts & NVME_CSTS_PP);
}

/* Read the Firmware Slot log (it also clears the activation event) and
 * take the running revision from it. */
static void nvme_get_fw_slot_info(struct nvme_ctrl *ctrl)
{
	struct nvme_fw_slot_info_log *log;
	uint8_t next_fw_slot, cur_fw_slot;

	might_sleep();
	BUILD_BUG_ON(sizeof(struct nvme_fw_slot_info_log) != 512);
	log = kalloc(sizeof(*log));
	if (!log)
		return;
	if (nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_FW_SLOT, 0,
			 NVME_CSI_NVM, log, sizeof(*log), 0)) {
		kprintf(NVME_LOG_PREFIX "%s: Get FW SLOT INFO log error\n",
			ctrl->name);
		goto out_free_log;
	}

	cur_fw_slot = log->afi & 0x7;
	next_fw_slot = (log->afi & 0x70) >> 4;
	if (!cur_fw_slot || (next_fw_slot && cur_fw_slot != next_fw_slot)) {
		kprintf(NVME_LOG_PREFIX
			"%s: firmware is activated after next controller "
			"level reset\n",
			ctrl->name);
		goto out_free_log;
	}
	BUILD_BUG_ON(sizeof(log->frs[0]) != sizeof(ctrl->subsys->firmware_rev));
	kmemcpy(ctrl->subsys->firmware_rev, &log->frs[cur_fw_slot - 1],
		sizeof(ctrl->subsys->firmware_rev));
out_free_log:
	kfree(log);
}

static void nvme_fw_act_work(void *arg)
{
	struct nvme_ctrl *ctrl = arg;
	uint64_t deadline;

	might_sleep();
	if (ctrl->mtfa)
		deadline = hrtimer_now_ns() +
			   (uint64_t)ctrl->mtfa * NVME_MTFA_UNIT_MS * 1000000ULL;
	else
		deadline = hrtimer_now_ns() +
			   (uint64_t)nvme_core_param.admin_timeout_s *
				   1000000000ULL;

	nvme_quiesce_io_queues(ctrl);
	while (nvme_ctrl_pp_status(ctrl)) {
		if (hrtimer_now_ns() > deadline) {
			kprintf(NVME_LOG_PREFIX
				"%s: fw activation timeout, reset controller\n",
				ctrl->name);
			nvme_try_sched_reset(ctrl);
			return;
		}
		nvme_msleep(100);
	}

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING) ||
	    !nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE))
		return;

	nvme_unquiesce_io_queues(ctrl);
	/* The slot log read also clears the event. */
	nvme_get_fw_slot_info(ctrl);
	nvme_queue_work(&ctrl->async_event_work);
}

static const char *nvme_aer_type_name(uint32_t type)
{
	switch (type) {
	case NVME_AER_ERROR:
		return "error";
	case NVME_AER_SMART:
		return "smart/health";
	case NVME_AER_NOTICE:
		return "notice";
	case NVME_AER_IMMEDIATE:
		return "immediate";
	case NVME_AER_ONESHOT:
		return "one-shot";
	case NVME_AER_CSS:
		return "command set specific";
	case NVME_AER_VS:
		return "vendor specific";
	default:
		return "reserved";
	}
}

/* Notices dispatched at once (CQ context); false: no new AER now (the
 * firmware activation work posts it). */
static bool nvme_handle_aen_notice(struct nvme_ctrl *ctrl, uint32_t result)
{
	uint32_t aer_notice_type = NVME_AER_RESULT_INFO(result);
	bool requeue = true;

	switch (aer_notice_type) {
	case NVME_AER_NOTICE_NS_CHANGED:
		nvme_set_bit(NVME_AER_NOTICE_NS_CHANGED_EVENT, &ctrl->events);
		nvme_queue_scan(ctrl);
		break;
	case NVME_AER_NOTICE_FW_ACT_STARTING:
		/* RESETTING keeps every other recovery out while the
		 * controller activates its new firmware. */
		if (nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING)) {
			nvme_set_bit(NVME_AER_NOTICE_FW_ACT_STARTING_EVENT,
				     &ctrl->events);
			requeue = false;
			nvme_queue_work(&ctrl->fw_act_work);
		}
		break;
	case NVME_AER_NOTICE_ANA:
		if (!ctrl->ana_log_buf)
			break;
		nvme_queue_work(&ctrl->ana_work);
		break;
	case NVME_AER_NOTICE_DISC_CHANGED:
		ctrl->aen_result = result;
		break;
	case NVME_AER_NOTICE_TELEMETRY:
	case NVME_AER_NOTICE_PLM_AGGREGATE:
	case NVME_AER_NOTICE_LBA_STATUS:
	case NVME_AER_NOTICE_ENDGRP_AGGREGATE:
	case NVME_AER_NOTICE_ZONE_DESCR_CHANGED:
		/* async_event_work reads the log page that clears it. */
		__atomic_store_n(&ctrl->aen_pending,
				 (result & NVME_AEN_RESULT_MASK) |
					 NVME_AEN_PENDING_MARK,
				 __ATOMIC_RELEASE);
		break;
	default:
		WARN_RATELIMIT(1, "nvme: %s: async event result %08x",
			       ctrl->name, result);
		break;
	}
	return requeue;
}

static void nvme_handle_aer_persistent_error(struct nvme_ctrl *ctrl)
{
	kprintf(NVME_LOG_PREFIX
		"%s: resetting controller due to persistent internal error\n",
		ctrl->name);
	nvme_reset_ctrl(ctrl);
}

void nvme_complete_async_event(struct nvme_ctrl *ctrl, __le16 status,
			       volatile union nvme_result *res)
{
	uint32_t result = le32_to_cpu(res->u32);
	uint32_t aer_type = NVME_AER_RESULT_TYPE(result);
	uint32_t aer_subtype = NVME_AER_RESULT_INFO(result);
	bool requeue = true;

	/* A failed request (aborted by a queue deletion or reset) is not
	 * reposted here: the reset posts a fresh one. */
	if ((le16_to_cpu(status) >> 1) != NVME_SC_SUCCESS)
		return;

	__atomic_add_fetch(&ctrl->aen_count, 1, __ATOMIC_RELAXED);
	ctrl->aen_last_result = result;

	switch (aer_type) {
	case NVME_AER_NOTICE:
		requeue = nvme_handle_aen_notice(ctrl, result);
		break;
	case NVME_AER_ERROR:
		/* A persistent internal error: the reset posts the next
		 * request, not async_event_work. */
		if (aer_subtype == NVME_AER_ERROR_PERSIST_INT_ERR) {
			nvme_handle_aer_persistent_error(ctrl);
			return;
		}
		/* fall through */
	case NVME_AER_SMART:
	case NVME_AER_CSS:
	case NVME_AER_VS:
	case NVME_AER_IMMEDIATE:
	case NVME_AER_ONESHOT:
		ctrl->aen_result = result;
		__atomic_store_n(&ctrl->aen_pending,
				 (result & NVME_AEN_RESULT_MASK) |
					 NVME_AEN_PENDING_MARK,
				 __ATOMIC_RELEASE);
		break;
	default:
		break;
	}

	if (requeue)
		nvme_queue_work(&ctrl->async_event_work);
}

/* Read `len' bytes of log `lid' with RAE clear, which ends the event
 * that pointed at it; the contents are only logged. */
static int nvme_aen_read_log(struct nvme_ctrl *ctrl, uint8_t lid,
			     uint8_t csi, size_t len, void **out)
{
	void *buf = kcalloc(1, len);
	int ret;

	if (!buf)
		return -ENOMEM;
	ret = nvme_get_log(ctrl, NVME_NSID_ALL, lid, 0, csi, buf, len, 0);
	if (ret || !out) {
		kfree(buf);
		if (out)
			*out = NULL;
		return ret;
	}
	*out = buf;
	return 0;
}

/* Process one event in process context: tell the log, read what clears
 * it, refresh the health data. */
static void nvme_aen_process(struct nvme_ctrl *ctrl, uint32_t result)
{
	uint32_t type = NVME_AER_RESULT_TYPE(result);
	uint32_t info = NVME_AER_RESULT_INFO(result);
	uint32_t lid = NVME_AER_RESULT_LID(result);
	void *buf = NULL;
	int ret;

	might_sleep();
	kprintf(NVME_LOG_PREFIX
		"%s: async event 0x%06x (%s, info 0x%02x, log 0x%02x)\n",
		ctrl->name, result, nvme_aer_type_name(type), info, lid);

	switch (type) {
	case NVME_AER_ERROR:
		nvme_hwmon_error_event(ctrl);
		break;
	case NVME_AER_SMART:
		nvme_hwmon_smart_event(ctrl, result);
		break;
	case NVME_AER_NOTICE:
		switch (info) {
		case NVME_AER_NOTICE_TELEMETRY:
			/* Controller-initiated telemetry data is available;
			 * reading the header clears the notice. */
			ret = nvme_aen_read_log(ctrl, NVME_LOG_TELEMETRY_CTRL,
						NVME_CSI_NVM,
						NVME_TELEMETRY_BLOCK_SIZE, &buf);
			if (!ret && buf) {
				struct nvme_telemetry_log *t = buf;

				kprintf(NVME_LOG_PREFIX
					"%s: controller-initiated telemetry "
					"available (generation %u)\n",
					ctrl->name, t->ctrldgn);
			}
			break;
		case NVME_AER_NOTICE_PLM_AGGREGATE:
			nvme_aen_read_log(ctrl, NVME_LOG_PLM_AGGREGATE,
					  NVME_CSI_NVM, 512, NULL);
			break;
		case NVME_AER_NOTICE_LBA_STATUS:
			nvme_aen_read_log(ctrl, NVME_LOG_LBA_STATUS,
					  NVME_CSI_NVM, 512, NULL);
			break;
		case NVME_AER_NOTICE_ENDGRP_AGGREGATE:
			ret = nvme_aen_read_log(ctrl, NVME_LOG_ENDGRP_EVENT_AGG,
						NVME_CSI_NVM, 512, &buf);
			if (!ret && buf)
				kprintf(NVME_LOG_PREFIX
					"%s: endurance group event(s): %llu "
					"entries\n",
					ctrl->name,
					(unsigned long long)
						get_unaligned_le64(buf));
			break;
		case NVME_AER_NOTICE_ZONE_DESCR_CHANGED:
			/* Per-namespace changed zone lists: revalidation by
			 * the next scan picks the new zone state up. */
			nvme_queue_scan(ctrl);
			break;
		default:
			break;
		}
		break;
	case NVME_AER_CSS:
		/* The namespace layer reads (and so clears) these logs. */
		switch (info) {
		case NVME_AEN_RESV_LOG_PAGE_AVAILABLE:
			nvme_pr_resv_event(ctrl);
			break;
		case NVME_AER_CSS_SANITIZE_COMPLETED:
		case NVME_AER_CSS_SANITIZE_DEALLOC_UNEXP:
			nvme_sanitize_event(ctrl, result);
			break;
		default:
			break;
		}
		break;
	case NVME_AER_IMMEDIATE:
		if (info == NVME_AER_IMMEDIATE_NSS_SHUTDOWN)
			kprintf(NVME_LOG_PREFIX
				"%s: NVM subsystem shutdown in progress\n",
				ctrl->name);
		break;
	case NVME_AER_ONESHOT:
		if (info == NVME_AER_ONESHOT_TEMP_HYSTERESIS)
			nvme_hwmon_smart_event(ctrl, result);
		break;
	default:
		/* Vendor specific: logged above, nothing to read (a vendor
		 * log page has no known size). */
		break;
	}
	if (buf)
		kfree(buf);
}

/* More events than this in one second is a storm (a condition the
 * controller keeps re-raising): the next request waits a second. */
#define NVME_AEN_STORM_LIMIT 64

static void nvme_async_event_work(void *arg)
{
	struct nvme_ctrl *ctrl = arg;
	uint32_t pending;

	pending = __atomic_exchange_n(&ctrl->aen_pending, 0, __ATOMIC_ACQ_REL);
	ctrl->aen_result = 0;
	if (pending & NVME_AEN_PENDING_MARK) {
		uint64_t now = hrtimer_now_ns();

		if (now - ctrl->aen_window_ns > 1000000000ULL) {
			ctrl->aen_window_ns = now;
			ctrl->aen_window_count = 0;
		}
		if (++ctrl->aen_window_count > NVME_AEN_STORM_LIMIT) {
			if (ctrl->aen_window_count == NVME_AEN_STORM_LIMIT + 1)
				kprintf(NVME_LOG_PREFIX
					"%s: asynchronous event storm "
					"(0x%06x): pausing event requests "
					"for 1 s\n",
					ctrl->name,
					pending & NVME_AEN_RESULT_MASK);
			if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE)
				nvme_queue_delayed_work(&ctrl->async_event_work,
							1000);
			return;
		}
		if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE)
			nvme_aen_process(ctrl, pending & NVME_AEN_RESULT_MASK);
	}

	/* The transport makes this safe by flushing async_event_work after
	 * leaving LIVE and before freeing the admin queue. */
	if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE)
		ctrl->ops->submit_async_event(ctrl);
}

/* ==========================================================================
 * Start / stop
 * ========================================================================== */

void nvme_stop_ctrl(struct nvme_ctrl *ctrl)
{
	might_sleep();
	nvme_mpath_stop(ctrl);
	nvme_stop_failfast_work(ctrl);
	nvme_flush_work(&ctrl->async_event_work);
	nvme_cancel_work_sync(&ctrl->fw_act_work);
	if (ctrl->ops->stop_ctrl)
		ctrl->ops->stop_ctrl(ctrl);
}

void nvme_start_ctrl(struct nvme_ctrl *ctrl)
{
	might_sleep();
	nvme_enable_aen(ctrl);

	if (ctrl->queue_count > 1) {
		nvme_queue_scan(ctrl);
		nvme_unquiesce_io_queues(ctrl);
		nvme_mpath_update(ctrl);
	}

	kprintf(NVME_LOG_PREFIX "%s: controller live (%u queue%s)\n",
		ctrl->name, ctrl->queue_count,
		ctrl->queue_count == 1 ? "" : "s");
	nvme_set_bit(NVME_CTRL_STARTED_ONCE, &ctrl->flags);
}

/* ==========================================================================
 * Recovery helpers over every namespace queue
 * ========================================================================== */

void nvme_mark_namespaces_dead(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	might_sleep();
	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		if (ns->disk)
			blk_mark_disk_dead(ns->disk);
	mutex_unlock(&ctrl->namespaces_lock);
}

void nvme_unfreeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	might_sleep();
	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		if (ns->queue)
			blk_mq_unfreeze_queue_non_owner(ns->queue);
	mutex_unlock(&ctrl->namespaces_lock);
	nvme_clear_bit(NVME_CTRL_FROZEN, &ctrl->flags);
}

int nvme_wait_freeze_timeout(struct nvme_ctrl *ctrl, long timeout_ms)
{
	struct nvme_ns *ns;
	uint64_t deadline;

	might_sleep();
	if (timeout_ms <= 0)
		return 0;
	deadline = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		uint64_t now = hrtimer_now_ns();

		if (!ns->queue)
			continue;
		if (now >= deadline) {
			timeout_ms = 0;
			break;
		}
		if (!blk_mq_freeze_queue_wait_timeout(
			    ns->queue, (deadline - now) / 1000000ULL + 1)) {
			timeout_ms = 0;
			break;
		}
	}
	mutex_unlock(&ctrl->namespaces_lock);
	if (timeout_ms > 0) {
		uint64_t now = hrtimer_now_ns();

		timeout_ms = now >= deadline ?
				     1 :
				     (long)((deadline - now) / 1000000ULL) + 1;
	}
	return (int)timeout_ms;
}

void nvme_wait_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	might_sleep();
	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		if (ns->queue)
			blk_mq_freeze_queue_wait(ns->queue);
	mutex_unlock(&ctrl->namespaces_lock);
}

void nvme_start_freeze(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	might_sleep();
	nvme_set_bit(NVME_CTRL_FROZEN, &ctrl->flags);
	mutex_lock(&ctrl->namespaces_lock);
	/* Started here (a timeout or reset path), ended by the reset work:
	 * the non-owner form. */
	list_for_each_entry(ns, &ctrl->namespaces, list)
		if (ns->queue)
			blk_freeze_queue_start_non_owner(ns->queue);
	mutex_unlock(&ctrl->namespaces_lock);
}

void nvme_quiesce_io_queues(struct nvme_ctrl *ctrl)
{
	if (!ctrl->tagset)
		return;
	if (!nvme_test_and_set_bit(NVME_CTRL_STOPPED, &ctrl->flags))
		blk_mq_quiesce_tagset(ctrl->tagset);
	else
		blk_mq_wait_quiesce_done(ctrl->tagset);
}

void nvme_unquiesce_io_queues(struct nvme_ctrl *ctrl)
{
	if (!ctrl->tagset)
		return;
	if (nvme_test_and_clear_bit(NVME_CTRL_STOPPED, &ctrl->flags))
		blk_mq_unquiesce_tagset(ctrl->tagset);
}

void nvme_quiesce_admin_queue(struct nvme_ctrl *ctrl)
{
	if (!ctrl->admin_q)
		return;
	if (!nvme_test_and_set_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
		blk_mq_quiesce_queue(ctrl->admin_q);
	else
		blk_mq_wait_quiesce_done(ctrl->admin_q->tag_set);
}

void nvme_unquiesce_admin_queue(struct nvme_ctrl *ctrl)
{
	if (!ctrl->admin_q)
		return;
	if (nvme_test_and_clear_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
		blk_mq_unquiesce_queue(ctrl->admin_q);
}

/* No timeout handler of `q' runs any more once this returns. */
static void nvme_sync_queue(struct blk_queue *q)
{
	if (q)
		blk_sync_queue(q);
}

void nvme_sync_io_queues(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	might_sleep();
	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list)
		nvme_sync_queue(ns->queue);
	mutex_unlock(&ctrl->namespaces_lock);
}

void nvme_sync_queues(struct nvme_ctrl *ctrl)
{
	nvme_sync_io_queues(ctrl);
	if (ctrl->admin_q)
		nvme_sync_queue(ctrl->admin_q);
}

/* ==========================================================================
 * Entry points
 * ========================================================================== */

int nvme_init(void)
{
	int ret;

	_nvme_spec_check();
	_nvme_core_check();

	nvme_core_parse_cmdline();
	blk_work_init(&nvme_free_work, nvme_free_work_fn, NULL);

	/* /dev/nvmeX and /dev/ngXnY: dynamic character majors. */
	ret = chrdev_register(0, "nvme");
	if (ret < 0) {
		kprintf(NVME_LOG_PREFIX "no character major for nvme (%d)\n",
			ret);
		return ret;
	}
	nvme_char_major = (uint32_t)ret;
	ret = chrdev_register(0, "nvme-generic");
	if (ret < 0) {
		kprintf(NVME_LOG_PREFIX
			"no character major for nvme-generic (%d)\n",
			ret);
		chrdev_unregister(nvme_char_major, "nvme");
		nvme_char_major = 0;
		return ret;
	}
	nvme_ns_chr_major = (uint32_t)ret;

	nvme_sysfs_init();

	ret = nvme_pci_init();
	if (ret && ret != -ENODEV)
		kprintf(NVME_LOG_PREFIX "PCIe transport init failed (%d)\n",
			ret);
	return ret;
}

int nvme_late_init(void)
{
	if (!nvme_char_major)
		return -ENODEV;
	nvme_pci_late_init();
	return 0;
}
