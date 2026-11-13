// LikeOS - NVMe fault injection
//
// Makes healthy hardware fail on purpose, so that the driver's error
// handling -- status decoding, retries, the timeout ladder (CQ poll,
// Abort, controller reset), recovery during I/O -- can be exercised on
// demand.  One injection object exists per controller (it covers the
// admin queue) and one per namespace (its I/O), as the established
// driver keeps them, each embedded in its owner (struct nvme_fault_inject
// in nvme_ctrl / nvme_ns).
//
// THE DECISION follows the conventional fault attribute rules exactly:
//   probability 0          never (the default: nothing is injected until
//                          somebody sets a probability)
//   times                  injections left; 0 stops, -1 is unlimited
//                          (default 1: one injection, then quiet)
//   space                  while larger than one, skip a command and count
//                          it down (the first commands pass)
//   interval > 1           only every interval-th candidate may fail
//   probability (percent)  fail when probability > a random 0..99
// and each injection counts down `times'.
//
// WHAT IS INJECTED (`kind'; LikeOS adds the last three to the established
// status injection):
//   status   the command completes with `status' (DNR added when
//            `dont_retry'), whatever the controller answered -- decided in
//            nvme_should_fail(), which the completion path calls for every
//            request (the established hook; default status Invalid Opcode
//            with DNR, as upstream sets it);
//   timeout  the completion is dropped (nvme_fault_drop_completion(), which
//            the transport asks before it completes a request): the request
//            stays in flight and runs into the block layer's timeout and the
//            transport's timeout handler -- CQ poll, Abort, controller reset.
//            This is the block layer's fake-timeout injection;
//   abort    as timeout, but only for I/O queue commands and only while the
//            controller still has Abort commands to spend (abort_limit), so
//            the first expiry certainly takes the Abort step rather than
//            the admin queue's immediate reset;
//   reset    the command completes normally and a controller reset is
//            scheduled (nvme_reset_ctrl) -- recovery while I/O is in flight.
// The Abort command itself is never dropped (the recovery must work).
// timeout and abort drop a completion only while the controller is LIVE:
// the ladder they exercise exists only there.  While a reset brings the
// controller back (RESETTING, CONNECTING) the timeout handler disables
// it at the first expiry instead, and the reset fails -- a command the
// reset re-issued while the object is still armed (a re-arm renews
// `times') would turn the injected lost completion into a dead
// controller.  Commands that were in flight when a reset started are
// cancelled by it, so no drop made while LIVE expires during a reset.
//
// INTERFACE (documented for disktest and test_libc):
//   /sys/kernel/debug/nvme/<name>/fault_inject/<attr>   (nvme_debug.c)
//   /sys/kernel/debug/<name>/fault_inject/<attr>        (the established
//                                                         place)
// with <name> the controller ("nvme0") or namespace disk ("nvme0n1") and
// <attr> one of probability interval times space verbose status dont_retry
// kind count injected; and the one-file form for a class attribute
// (nvme_fault_inject_show_name/store_name, grammar in nvme.h).  The boot
// option nvme_core.fail_request=<interval>,<probability>,<space>,<times>
// presets every object, as upstream's module parameter does.
//
// Locking: g_fi_lock (a leaf spinlock) guards the registry of named
// objects; the attribute readers and writers act under it, and
// nvme_fault_inject_fini() unregisters under it, so no attribute access
// can touch an object after its owner finished it.  The per-command hooks
// use only atomic operations on the object itself (its owner is alive
// while it has requests).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>

/* ==========================================================================
 * Defaults and the boot option
 * ========================================================================== */

/* The conventional fault attribute defaults: interval 1, probability 0,
 * space 0, times 1, verbose 2. */
#define NVME_FI_DEF_INTERVAL 1u
#define NVME_FI_DEF_PROBABILITY 0u
#define NVME_FI_DEF_SPACE 0u
#define NVME_FI_DEF_TIMES 1
#define NVME_FI_DEF_VERBOSE 2u

struct nvme_fi_defaults {
	uint32_t interval;
	uint32_t probability;
	uint32_t space;
	int32_t times;
};

static struct nvme_fi_defaults g_fi_default = {
	NVME_FI_DEF_INTERVAL,
	NVME_FI_DEF_PROBABILITY,
	NVME_FI_DEF_SPACE,
	NVME_FI_DEF_TIMES,
};
static volatile int g_fi_default_parsed;

/* ==========================================================================
 * Small parsers (pure; shared by every store path)
 * ========================================================================== */

static bool fi_isspace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Parse one integer from s[0..n): optional sign, decimal or 0x hex.  The
 * whole span must be consumed.  0 / -EINVAL / -ERANGE. */
static int fi_parse_s64(const char *s, size_t n, int64_t *out)
{
	size_t i = 0;
	int neg = 0;
	uint64_t v = 0, base = 10;
	int digits = 0;

	while (i < n && fi_isspace(s[i]))
		i++;
	while (n > i && fi_isspace(s[n - 1]))
		n--;
	if (i >= n)
		return -EINVAL;
	if (s[i] == '-' || s[i] == '+') {
		neg = s[i] == '-';
		i++;
	}
	if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
		base = 16;
		i += 2;
	}
	for (; i < n; i++) {
		char c = s[i];
		uint64_t d;

		if (c >= '0' && c <= '9')
			d = (uint64_t)(c - '0');
		else if (base == 16 && c >= 'a' && c <= 'f')
			d = (uint64_t)(c - 'a' + 10);
		else if (base == 16 && c >= 'A' && c <= 'F')
			d = (uint64_t)(c - 'A' + 10);
		else
			return -EINVAL;
		if (v > (0x7fffffffffffffffULL - d) / base)
			return -ERANGE;
		v = v * base + d;
		digits++;
	}
	if (!digits)
		return -EINVAL;
	*out = neg ? -(int64_t)v : (int64_t)v;
	return 0;
}

/* A boolean the way the established attribute files take it: 1/0, y/n,
 * Y/N, on/off. */
static int fi_parse_bool(const char *s, size_t n, bool *out)
{
	size_t i = 0;

	while (i < n && fi_isspace(s[i]))
		i++;
	while (n > i && fi_isspace(s[n - 1]))
		n--;
	if (i >= n)
		return -EINVAL;
	s += i;
	n -= i;
	if (n == 1 && (s[0] == '1' || s[0] == 'y' || s[0] == 'Y')) {
		*out = true;
		return 0;
	}
	if (n == 1 && (s[0] == '0' || s[0] == 'n' || s[0] == 'N')) {
		*out = false;
		return 0;
	}
	if (n == 2 && (s[0] == 'o' || s[0] == 'O') &&
	    (s[1] == 'n' || s[1] == 'N')) {
		*out = true;
		return 0;
	}
	if (n == 3 && (s[0] == 'o' || s[0] == 'O') &&
	    (s[1] == 'f' || s[1] == 'F') && (s[2] == 'f' || s[2] == 'F')) {
		*out = false;
		return 0;
	}
	return -EINVAL;
}

static bool fi_word_eq(const char *s, size_t n, const char *w)
{
	size_t k = 0;

	while (k < n && w[k] && s[k] == w[k])
		k++;
	return k == n && w[k] == 0;
}

static const char *const fi_kind_names[] = {
	[NVME_FAULT_STATUS] = "status",
	[NVME_FAULT_TIMEOUT] = "timeout",
	[NVME_FAULT_ABORT] = "abort",
	[NVME_FAULT_RESET] = "reset",
};
#define FI_NR_KINDS ((int)(sizeof(fi_kind_names) / sizeof(fi_kind_names[0])))

static const char *fi_kind_name(unsigned int kind)
{
	if (kind < (unsigned int)FI_NR_KINDS && fi_kind_names[kind])
		return fi_kind_names[kind];
	return "?";
}

static int fi_parse_kind(const char *s, size_t n, uint8_t *out)
{
	size_t i = 0;

	while (i < n && fi_isspace(s[i]))
		i++;
	while (n > i && fi_isspace(s[n - 1]))
		n--;
	for (int k = 0; k < FI_NR_KINDS; k++) {
		if (fi_kind_names[k] && fi_word_eq(s + i, n - i, fi_kind_names[k])) {
			*out = (uint8_t)k;
			return 0;
		}
	}
	/* A number is accepted as well (0..3). */
	{
		int64_t v;

		if (!fi_parse_s64(s, n, &v) && v >= 0 && v < FI_NR_KINDS) {
			*out = (uint8_t)v;
			return 0;
		}
	}
	return -EINVAL;
}

/* "interval,probability,space,times" (the established boot syntax). */
static int fi_parse_defaults(const char *str, struct nvme_fi_defaults *d)
{
	int64_t v[4];
	int nr = 0;
	const char *p = str;

	while (nr < 4) {
		const char *e = p;

		while (*e && *e != ',')
			e++;
		if (fi_parse_s64(p, (size_t)(e - p), &v[nr]))
			return -EINVAL;
		nr++;
		if (!*e)
			break;
		p = e + 1;
	}
	if (nr != 4)
		return -EINVAL;
	if (v[0] < 0 || v[0] > 0xffffffffLL || v[1] < 0 || v[1] > 100 ||
	    v[2] < 0 || v[2] > 0x7fffffffLL || v[3] < -1 ||
	    v[3] > 0x7fffffffLL)
		return -EINVAL;
	d->interval = (uint32_t)v[0];
	d->probability = (uint32_t)v[1];
	d->space = (uint32_t)v[2];
	d->times = (int32_t)v[3];
	return 0;
}

static void fi_parse_boot_option_once(void)
{
	char buf[64];
	struct nvme_fi_defaults d;

	if (__atomic_exchange_n(&g_fi_default_parsed, 1, __ATOMIC_ACQ_REL))
		return;
	if (nvme_cmdline_get("nvme_core.fail_request", buf, sizeof(buf)) != 1)
		return;
	if (fi_parse_defaults(buf, &d)) {
		kprintf(NVME_LOG_PREFIX
			"nvme_core.fail_request=%s: expected "
			"<interval>,<probability>,<space>,<times>\n",
			buf);
		return;
	}
	g_fi_default = d;
	kprintf(NVME_LOG_PREFIX "fault injection preset: interval %u "
				"probability %u space %u times %d\n",
		d.interval, d.probability, d.space, d.times);
}

/* ==========================================================================
 * The decision
 * ========================================================================== */

/* A small lock-free generator for the probability draw (no cryptographic
 * quality needed, usable in an interrupt handler). */
static volatile uint64_t g_fi_rng_state;

static uint32_t fi_random_below_100(void)
{
	uint64_t old, x;

	old = __atomic_load_n(&g_fi_rng_state, __ATOMIC_RELAXED);
	do {
		x = old ? old : (hrtimer_now_ns() | 1);
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
	} while (!__atomic_compare_exchange_n(&g_fi_rng_state, &old, x, false,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED));
	return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32) % 100u;
}

/* Rate limit of the injection messages: a burst of 10 per 5 s. */
static volatile uint64_t g_fi_rl_start_ns;
static volatile uint32_t g_fi_rl_count;

static bool fi_ratelimit(void)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t start = __atomic_load_n(&g_fi_rl_start_ns, __ATOMIC_RELAXED);

	if (now - start > 5000000000ULL) {
		if (__atomic_compare_exchange_n(&g_fi_rl_start_ns, &start, now,
						false, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			__atomic_store_n(&g_fi_rl_count, 0, __ATOMIC_RELAXED);
	}
	return __atomic_add_fetch(&g_fi_rl_count, 1, __ATOMIC_RELAXED) <= 10;
}

/* The conventional fault attribute decision for one command (size 1). */
static bool nvme_fi_should_fail(struct nvme_fault_inject *fi)
{
	uint32_t prob, interval, space;
	int32_t times;
	uint32_t count;

	prob = __atomic_load_n(&fi->probability, __ATOMIC_RELAXED);
	if (prob == 0)
		return false;
	times = __atomic_load_n(&fi->times, __ATOMIC_RELAXED);
	if (times == 0)
		return false;
	/* space: skip while it is larger than the size of this "allocation"
	 * (one command), counting it down. */
	space = __atomic_load_n(&fi->space, __ATOMIC_RELAXED);
	while (space > 1) {
		if (__atomic_compare_exchange_n(&fi->space, &space, space - 1,
						false, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			return false;
	}
	count = __atomic_add_fetch(&fi->count, 1, __ATOMIC_RELAXED);
	interval = __atomic_load_n(&fi->interval, __ATOMIC_RELAXED);
	if (interval > 1 && (count % interval) != 0)
		return false;
	if (prob <= fi_random_below_100())
		return false;
	/* Count the injection down unless unlimited (negative). */
	times = __atomic_load_n(&fi->times, __ATOMIC_RELAXED);
	while (times > 0) {
		if (__atomic_compare_exchange_n(&fi->times, &times, times - 1,
						false, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			break;
	}
	if (times == 0)
		return false; /* another CPU used the last one */
	__atomic_add_fetch(&fi->injected, 1, __ATOMIC_RELAXED);
	return true;
}

/* The object a request is accounted to: its namespace's for I/O on a
 * namespace queue, the controller's for the admin queue. */
static struct nvme_fault_inject *nvme_rq_fault_inject(struct blk_request *rq)
{
	struct blk_queue *q = rq->queue;
	struct nvme_request *nreq = nvme_req(rq);

	if (q && q->disk) {
		struct nvme_ns *ns = (struct nvme_ns *)q->queuedata;

		if (!ns) {
			WARN_ON_ONCE(1); /* a namespace queue without its ns */
			return NULL;
		}
		return &ns->fault_inject;
	}
	if (!nreq->ctrl)
		return NULL;
	return &nreq->ctrl->fault_inject;
}

static uint8_t nvme_rq_opcode(struct blk_request *rq)
{
	struct nvme_command *c = nvme_req(rq)->cmd;

	return c ? c->common.opcode : 0xff;
}

static void fi_report(struct nvme_fault_inject *fi, struct blk_request *rq,
		      const char *what, uint16_t status)
{
	struct nvme_ctrl *ctrl = nvme_req(rq)->ctrl;

	if (!__atomic_load_n(&fi->verbose, __ATOMIC_RELAXED) || !fi_ratelimit())
		return;
	kprintf(NVME_LOG_PREFIX "%s: FAULT_INJECTION: %s qid %u cid %u "
				"opcode 0x%02x status 0x%x (times left %d)\n",
		ctrl ? ctrl->name : "?", what, (unsigned)nvme_req_qid(rq),
		(unsigned)nvme_cid(rq), (unsigned)nvme_rq_opcode(rq),
		(unsigned)status, (int)fi->times);
}

void nvme_should_fail(struct blk_request *rq)
{
	struct nvme_fault_inject *fi;
	uint8_t kind;

	if (unlikely(!rq))
		return;
	fi = nvme_rq_fault_inject(rq);
	if (!fi || !__atomic_load_n(&fi->probability, __ATOMIC_RELAXED))
		return;
	kind = __atomic_load_n(&fi->kind, __ATOMIC_RELAXED);
	if (kind != NVME_FAULT_STATUS && kind != NVME_FAULT_RESET)
		return;
	if (!nvme_fi_should_fail(fi))
		return;

	if (kind == NVME_FAULT_STATUS) {
		uint16_t status = __atomic_load_n(&fi->status, __ATOMIC_RELAXED);

		/* Inject the status code and the DNR bit. */
		if (__atomic_load_n(&fi->dont_retry, __ATOMIC_RELAXED))
			status |= NVME_STATUS_DNR;
		nvme_req(rq)->status = status;
		fi_report(fi, rq, "status", status);
		return;
	}

	/* NVME_FAULT_RESET: the command completes as the controller
	 * answered; the controller is reset under the I/O that follows. */
	{
		struct nvme_ctrl *ctrl = nvme_req(rq)->ctrl;
		int r;

		if (!ctrl)
			return;
		r = nvme_reset_ctrl(ctrl);
		fi_report(fi, rq, r ? "reset (refused: state)" : "reset", 0);
	}
}

bool nvme_fault_drop_completion(struct nvme_ctrl *ctrl, struct blk_request *rq)
{
	struct nvme_fault_inject *fi;
	uint8_t kind;

	if (unlikely(!rq))
		return false;
	fi = nvme_rq_fault_inject(rq);
	if (!fi || !__atomic_load_n(&fi->probability, __ATOMIC_RELAXED))
		return false;
	kind = __atomic_load_n(&fi->kind, __ATOMIC_RELAXED);
	if (kind != NVME_FAULT_TIMEOUT && kind != NVME_FAULT_ABORT)
		return false;
	/* Only a live controller has the timeout ladder (see above). */
	if (!ctrl || nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
		return false;
	/* The recovery's own command always gets through. */
	if (nvme_req_qid(rq) == 0 &&
	    nvme_rq_opcode(rq) == nvme_admin_abort_cmd)
		return false;
	if (kind == NVME_FAULT_ABORT) {
		if (nvme_req_qid(rq) == 0)
			return false;
		if (ctrl && __atomic_load_n(&ctrl->abort_limit,
					    __ATOMIC_RELAXED) <= 0)
			return false;
	}
	if (!nvme_fi_should_fail(fi))
		return false;
	fi_report(fi, rq, kind == NVME_FAULT_ABORT ? "drop (abort path)" :
						      "drop (timeout)",
		  0);
	return true;
}

/* ==========================================================================
 * The registry of named objects
 * ========================================================================== */

struct nvme_fi_entry {
	struct list_head link; /* g_fi_list, ascending key */
	unsigned int key; /* stable listing key */
	char name[NVME_NAME_LEN + 8];
	struct nvme_fault_inject *fi;
};

static LIST_HEAD(g_fi_list);
static spinlock_t g_fi_lock = SPINLOCK_INIT("nvme_fault_inject");
static unsigned int g_fi_next_key = 1;

static struct nvme_fi_entry *fi_find_name_locked(const char *name)
{
	struct nvme_fi_entry *e;

	list_for_each_entry(e, &g_fi_list, link)
		if (kstrcmp(e->name, name) == 0)
			return e;
	return NULL;
}

static void fi_set_defaults(struct nvme_fault_inject *fi)
{
	fi->probability = g_fi_default.probability;
	fi->interval = g_fi_default.interval;
	fi->times = g_fi_default.times;
	fi->space = g_fi_default.space;
	fi->verbose = NVME_FI_DEF_VERBOSE;
	/* The established defaults of the status and dont_retry
	 * attributes. */
	fi->status = NVME_SC_INVALID_OPCODE;
	fi->dont_retry = 1;
	fi->kind = NVME_FAULT_STATUS;
}

void nvme_fault_inject_init(struct nvme_fault_inject *fi, const char *name)
{
	struct nvme_fi_entry *e;
	uint64_t fl;
	size_t k = 0;

	if (WARN_ON(!fi || !name))
		return;
	fi_parse_boot_option_once();
	mm_memset((void *)fi, 0, sizeof(*fi));
	fi_set_defaults(fi);

	e = kalloc(sizeof(*e));
	if (!e) {
		kprintf(NVME_LOG_PREFIX "%s: no memory for the fault injection "
					"attributes\n",
			name);
		return;
	}
	mm_memset(e, 0, sizeof(*e));
	while (name[k] && k < sizeof(e->name) - 1) {
		e->name[k] = name[k];
		k++;
	}
	e->name[k] = 0;
	e->fi = fi;

	spin_lock_irqsave(&g_fi_lock, &fl);
	if (fi_find_name_locked(e->name)) {
		spin_unlock_irqrestore(&g_fi_lock, fl);
		kprintf(NVME_LOG_PREFIX "%s: fault injection attributes exist "
					"already\n",
			e->name);
		kfree(e);
		return;
	}
	e->key = g_fi_next_key++;
	list_add_tail(&e->link, &g_fi_list);
	spin_unlock_irqrestore(&g_fi_lock, fl);

	nvme_debugfs_init_once();
}

void nvme_fault_inject_fini(struct nvme_fault_inject *fi)
{
	struct nvme_fi_entry *e, *n, *found = NULL;
	uint64_t fl;

	if (!fi)
		return;
	spin_lock_irqsave(&g_fi_lock, &fl);
	list_for_each_entry_safe(e, n, &g_fi_list, link) {
		if (e->fi == fi) {
			list_del(&e->link);
			found = e;
			break;
		}
	}
	spin_unlock_irqrestore(&g_fi_lock, fl);
	/* Nothing is injected any more by an object whose owner is going. */
	__atomic_store_n(&fi->probability, 0, __ATOMIC_RELAXED);
	if (found)
		kfree(found);
}

int nvme_fault_list_names(unsigned int *cursor, char *name, long cap)
{
	struct nvme_fi_entry *e;
	uint64_t fl;
	int found = 0;

	if (!cursor || !name || cap <= 0)
		return 0;
	spin_lock_irqsave(&g_fi_lock, &fl);
	list_for_each_entry(e, &g_fi_list, link) {
		if (e->key >= *cursor) {
			ksnprintf(name, (size_t)cap, "%s", e->name);
			*cursor = e->key + 1;
			found = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&g_fi_lock, fl);
	return found;
}

bool nvme_fault_name_exists(const char *name)
{
	uint64_t fl;
	bool r;

	if (!name)
		return false;
	spin_lock_irqsave(&g_fi_lock, &fl);
	r = fi_find_name_locked(name) != NULL;
	spin_unlock_irqrestore(&g_fi_lock, fl);
	return r;
}

/* ==========================================================================
 * Attributes
 * ========================================================================== */

static const char *const fi_attr_names[NVME_FI_ATTR_NR] = {
	[NVME_FI_ATTR_PROBABILITY] = "probability",
	[NVME_FI_ATTR_INTERVAL] = "interval",
	[NVME_FI_ATTR_TIMES] = "times",
	[NVME_FI_ATTR_SPACE] = "space",
	[NVME_FI_ATTR_VERBOSE] = "verbose",
	[NVME_FI_ATTR_STATUS] = "status",
	[NVME_FI_ATTR_DONT_RETRY] = "dont_retry",
	[NVME_FI_ATTR_KIND] = "kind",
	[NVME_FI_ATTR_COUNT] = "count",
	[NVME_FI_ATTR_INJECTED] = "injected",
};

const char *nvme_fault_attr_name(int attr)
{
	if (attr < 0 || attr >= NVME_FI_ATTR_NR)
		return NULL;
	return fi_attr_names[attr];
}

bool nvme_fault_attr_writable(int attr)
{
	return attr >= 0 && attr < NVME_FI_ATTR_NR &&
	       attr != NVME_FI_ATTR_COUNT && attr != NVME_FI_ATTR_INJECTED;
}

/* Format one attribute of `fi' (registry lock held: plain reads). */
static int fi_format_attr(const struct nvme_fault_inject *fi, int attr,
			  char *buf, size_t cap)
{
	switch (attr) {
	case NVME_FI_ATTR_PROBABILITY:
		return ksnprintf(buf, cap, "%u\n", (unsigned)fi->probability);
	case NVME_FI_ATTR_INTERVAL:
		return ksnprintf(buf, cap, "%u\n", (unsigned)fi->interval);
	case NVME_FI_ATTR_TIMES:
		return ksnprintf(buf, cap, "%d\n", (int)fi->times);
	case NVME_FI_ATTR_SPACE:
		return ksnprintf(buf, cap, "%u\n", (unsigned)fi->space);
	case NVME_FI_ATTR_VERBOSE:
		return ksnprintf(buf, cap, "%u\n", (unsigned)fi->verbose);
	case NVME_FI_ATTR_STATUS:
		return ksnprintf(buf, cap, "0x%04x\n", (unsigned)fi->status);
	case NVME_FI_ATTR_DONT_RETRY:
		return ksnprintf(buf, cap, "%s\n", fi->dont_retry ? "Y" : "N");
	case NVME_FI_ATTR_KIND:
		return ksnprintf(buf, cap, "%s\n", fi_kind_name(fi->kind));
	case NVME_FI_ATTR_COUNT:
		return ksnprintf(buf, cap, "%u\n", (unsigned)fi->count);
	case NVME_FI_ATTR_INJECTED:
		return ksnprintf(buf, cap, "%u\n", (unsigned)fi->injected);
	default:
		return -EINVAL;
	}
}

/* A parsed attribute value, applied later under the lock. */
struct fi_value {
	int attr;
	int64_t num;
	bool flag;
	uint8_t kind;
};

static int fi_parse_attr_value(int attr, const char *s, size_t n,
			       struct fi_value *v)
{
	int r;

	v->attr = attr;
	switch (attr) {
	case NVME_FI_ATTR_PROBABILITY:
		r = fi_parse_s64(s, n, &v->num);
		if (r)
			return r;
		/* A probability above 100 fails every candidate, as with
		 * the established attribute; negative is meaningless. */
		if (v->num < 0 || v->num > 0xffffffffLL)
			return -EINVAL;
		return 0;
	case NVME_FI_ATTR_INTERVAL:
	case NVME_FI_ATTR_VERBOSE:
		r = fi_parse_s64(s, n, &v->num);
		if (r)
			return r;
		if (v->num < 0 || v->num > 0xffffffffLL)
			return -EINVAL;
		return 0;
	case NVME_FI_ATTR_SPACE:
		r = fi_parse_s64(s, n, &v->num);
		if (r)
			return r;
		if (v->num < 0 || v->num > 0x7fffffffLL)
			return -EINVAL;
		return 0;
	case NVME_FI_ATTR_TIMES:
		r = fi_parse_s64(s, n, &v->num);
		if (r)
			return r;
		if (v->num < -0x80000000LL || v->num > 0x7fffffffLL)
			return -EINVAL;
		return 0;
	case NVME_FI_ATTR_STATUS:
		r = fi_parse_s64(s, n, &v->num);
		if (r)
			return r;
		if (v->num < 0 || v->num > 0xffff)
			return -ERANGE;
		return 0;
	case NVME_FI_ATTR_DONT_RETRY:
		return fi_parse_bool(s, n, &v->flag);
	case NVME_FI_ATTR_KIND:
		return fi_parse_kind(s, n, &v->kind);
	default:
		return -EPERM; /* read-only or unknown */
	}
}

static void fi_apply_locked(struct nvme_fault_inject *fi,
			    const struct fi_value *v)
{
	switch (v->attr) {
	case NVME_FI_ATTR_PROBABILITY:
		__atomic_store_n(&fi->probability, (uint32_t)v->num,
				 __ATOMIC_RELAXED);
		break;
	case NVME_FI_ATTR_INTERVAL:
		__atomic_store_n(&fi->interval, (uint32_t)v->num,
				 __ATOMIC_RELAXED);
		break;
	case NVME_FI_ATTR_TIMES:
		__atomic_store_n(&fi->times, (int32_t)v->num, __ATOMIC_RELAXED);
		break;
	case NVME_FI_ATTR_SPACE:
		__atomic_store_n(&fi->space, (uint32_t)v->num, __ATOMIC_RELAXED);
		break;
	case NVME_FI_ATTR_VERBOSE:
		fi->verbose = (uint32_t)v->num;
		break;
	case NVME_FI_ATTR_STATUS:
		__atomic_store_n(&fi->status, (uint16_t)v->num, __ATOMIC_RELAXED);
		break;
	case NVME_FI_ATTR_DONT_RETRY:
		__atomic_store_n(&fi->dont_retry, (uint8_t)(v->flag ? 1 : 0),
				 __ATOMIC_RELAXED);
		break;
	case NVME_FI_ATTR_KIND:
		__atomic_store_n(&fi->kind, v->kind, __ATOMIC_RELAXED);
		break;
	default:
		break;
	}
}

long nvme_fault_attr_show(const char *name, int attr, char *buf, long cap)
{
	struct nvme_fi_entry *e;
	uint64_t fl;
	char tmp[48];
	int n;

	if (!name || attr < 0 || attr >= NVME_FI_ATTR_NR || cap < 0)
		return -EINVAL;
	spin_lock_irqsave(&g_fi_lock, &fl);
	e = fi_find_name_locked(name);
	if (!e) {
		spin_unlock_irqrestore(&g_fi_lock, fl);
		return -ENODEV;
	}
	n = fi_format_attr(e->fi, attr, tmp, sizeof(tmp));
	spin_unlock_irqrestore(&g_fi_lock, fl);
	if (n < 0)
		return n;
	for (int i = 0; i < n && i < cap; i++)
		buf[i] = tmp[i];
	return n;
}

long nvme_fault_attr_store(const char *name, int attr, const char *buf,
			   long len)
{
	struct nvme_fi_entry *e;
	struct fi_value v;
	uint64_t fl;
	int r;

	if (!name || !buf || len < 0)
		return -EINVAL;
	if (!nvme_fault_attr_writable(attr))
		return -EPERM;
	r = fi_parse_attr_value(attr, buf, (size_t)len, &v);
	if (r)
		return r;
	spin_lock_irqsave(&g_fi_lock, &fl);
	e = fi_find_name_locked(name);
	if (!e) {
		spin_unlock_irqrestore(&g_fi_lock, fl);
		return -ENODEV;
	}
	fi_apply_locked(e->fi, &v);
	spin_unlock_irqrestore(&g_fi_lock, fl);
	return len;
}

/* ---- the one-file form ------------------------------------------------- */

long nvme_fault_inject_show_name(const char *name, char *buf, long cap)
{
	struct nvme_fi_entry *e;
	struct nvme_fault_inject snap;
	uint64_t fl;
	char tmp[256];
	int n;

	if (!name || !buf || cap < 0)
		return -EINVAL;
	spin_lock_irqsave(&g_fi_lock, &fl);
	e = fi_find_name_locked(name);
	if (!e) {
		spin_unlock_irqrestore(&g_fi_lock, fl);
		return -ENODEV;
	}
	mm_memcpy(&snap, (const void *)e->fi, sizeof(snap));
	spin_unlock_irqrestore(&g_fi_lock, fl);

	n = ksnprintf(tmp, sizeof(tmp),
		      "kind=%s probability=%u interval=%u times=%d space=%u "
		      "status=0x%04x dont_retry=%u verbose=%u count=%u "
		      "injected=%u\n",
		      fi_kind_name(snap.kind), (unsigned)snap.probability,
		      (unsigned)snap.interval, (int)snap.times,
		      (unsigned)snap.space, (unsigned)snap.status,
		      (unsigned)snap.dont_retry, (unsigned)snap.verbose,
		      (unsigned)snap.count, (unsigned)snap.injected);
	if (n < 0)
		return n;
	for (int i = 0; i < n && i < cap; i++)
		buf[i] = tmp[i];
	return n;
}

/* Up to one value per attribute plus the bare words. */
#define FI_MAX_WORDS (NVME_FI_ATTR_NR + 2)

long nvme_fault_inject_store_name(const char *name, const char *buf, long len)
{
	struct fi_value vals[FI_MAX_WORDS];
	int nvals = 0;
	bool clear = false, off = false;
	struct nvme_fi_entry *e;
	uint64_t fl;
	long i = 0;

	if (!name || !buf || len < 0)
		return -EINVAL;
	/* Parse everything first: a bad word changes nothing. */
	while (i < len) {
		long s, eq = -1, end;

		while (i < len && (fi_isspace(buf[i]) || buf[i] == ','))
			i++;
		if (i >= len)
			break;
		s = i;
		while (i < len && !fi_isspace(buf[i]) && buf[i] != ',') {
			if (buf[i] == '=' && eq < 0)
				eq = i;
			i++;
		}
		end = i;
		if (eq < 0) {
			if (fi_word_eq(buf + s, (size_t)(end - s), "off"))
				off = true;
			else if (fi_word_eq(buf + s, (size_t)(end - s), "clear"))
				clear = true;
			else
				return -EINVAL;
			continue;
		}
		{
			int attr = -1;

			for (int a = 0; a < NVME_FI_ATTR_NR; a++) {
				if (fi_word_eq(buf + s, (size_t)(eq - s),
					       fi_attr_names[a])) {
					attr = a;
					break;
				}
			}
			if (attr < 0 || !nvme_fault_attr_writable(attr))
				return -EINVAL;
			if (nvals >= FI_MAX_WORDS)
				return -E2BIG;
			if (fi_parse_attr_value(attr, buf + eq + 1,
						(size_t)(end - eq - 1),
						&vals[nvals]))
				return -EINVAL;
			nvals++;
		}
	}
	if (!nvals && !clear && !off)
		return -EINVAL;

	spin_lock_irqsave(&g_fi_lock, &fl);
	e = fi_find_name_locked(name);
	if (!e) {
		spin_unlock_irqrestore(&g_fi_lock, fl);
		return -ENODEV;
	}
	if (clear) {
		fi_set_defaults(e->fi);
		e->fi->count = 0;
		e->fi->injected = 0;
	}
	for (int k = 0; k < nvals; k++)
		fi_apply_locked(e->fi, &vals[k]);
	if (off)
		__atomic_store_n(&e->fi->probability, 0, __ATOMIC_RELAXED);
	spin_unlock_irqrestore(&g_fi_lock, fl);
	return len;
}
