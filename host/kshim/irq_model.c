/* A model of the kernel's interrupt core (kernel/ke/irq.c) for the GPIO
 * harness (host/kshim): the descriptor interface the GPIO drivers and
 * gpiolib use, with the same flow rules as the real core for the two
 * kinds they create:
 *
 *   * GSI descriptors (the controllers' parent lines): handlers run when
 *     the test "fires" the GSI (irqm_fire_gsi), with interrupts off;
 *   * virtual descriptors (GPIO lines): irq_handle_virtual runs the edge
 *     flow of the real core -- disabled or held by a oneshot thread:
 *     mark pending and mask; otherwise run the handlers, and when one asks
 *     for its thread with IRQF_ONESHOT, mask until the thread returns;
 *     enable_irq/oneshot completion unmask and replay a pending interrupt
 *     through the source's retrigger operation.
 *
 * Interrupt threads do not run by themselves: irqm_run_threads() runs the
 * thread functions that were woken, in process context, as the threads
 * would.  So a test can look at the hardware while a thread is pending.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>

#define IRQM_MAX 256
#define IRQM_ACTIONS 4

struct irqm_action {
	irq_handler_t fn, thread_fn;
	void *arg;
	unsigned long flags;
	const char *name;
	int thread_woken;
};

struct irqm_desc {
	int used;
	int kind; /* 0 virtual, 1 gsi */
	const struct irq_chip_ops *ops;
	void *data;
	int gsi, active_low, level;
	int depth, masked, pending, started, threads_oneshot;
	unsigned int trigger, wake_depth;
	struct irqm_action act[IRQM_ACTIONS];
	int nact;
	unsigned long count, unhandled;
	spinlock_t lock;
};

static struct irqm_desc g_irqm[IRQM_MAX];
static spinlock_t g_irqm_lock = SPINLOCK_INIT("irqm");

static struct irqm_desc *desc_of(int irq)
{
	int i = irq - IRQ_NR_FIRST;

	if (i < 0 || i >= IRQM_MAX || !g_irqm[i].used)
		return NULL;
	return &g_irqm[i];
}

static int desc_new(int kind, const struct irq_chip_ops *ops, void *data)
{
	uint64_t fl;

	spin_lock_irqsave(&g_irqm_lock, &fl);
	for (int i = 0; i < IRQM_MAX; i++) {
		if (!g_irqm[i].used) {
			kmemset(&g_irqm[i], 0, sizeof(g_irqm[i]));
			g_irqm[i].used = 1;
			g_irqm[i].kind = kind;
			g_irqm[i].ops = ops;
			g_irqm[i].data = data;
			spinlock_init(&g_irqm[i].lock, "irqm_desc");
			spin_unlock_irqrestore(&g_irqm_lock, fl);
			return IRQ_NR_FIRST + i;
		}
	}
	spin_unlock_irqrestore(&g_irqm_lock, fl);
	return -ENOSPC;
}

int irq_alloc_desc_virtual(const struct irq_chip_ops *ops, void *data,
			   const cpumask_t *affinity, int managed)
{
	(void)affinity;
	(void)managed;
	might_sleep();
	return desc_new(0, ops, data);
}

int irq_alloc_desc_gsi(const struct irq_chip_ops *ops, void *data, int gsi,
		       int active_low, int level)
{
	int irq = desc_new(1, ops, data);
	struct irqm_desc *d = desc_of(irq);

	might_sleep();
	if (d) {
		d->gsi = gsi;
		d->active_low = active_low;
		d->level = level;
	}
	return irq;
}

void irq_free_desc(int irq)
{
	struct irqm_desc *d = desc_of(irq);

	might_sleep();
	if (!d)
		return;
	WARN(d->nact, "irqm: descriptor %d freed with handlers", irq);
	d->used = 0;
}

static void dmask(struct irqm_desc *d, int irq)
{
	if (d->masked)
		return;
	if (d->ops && d->ops->mask)
		d->ops->mask(irq, d->data);
	d->masked = 1;
}

static void dunmask(struct irqm_desc *d, int irq)
{
	if (!d->masked)
		return;
	if (d->ops && d->ops->unmask)
		d->ops->unmask(irq, d->data);
	d->masked = 0;
}

static void dreplay(struct irqm_desc *d, int irq)
{
	if (!d->pending)
		return;
	if (d->kind == 0 && d->ops && d->ops->retrigger) {
		d->pending = 0;
		d->ops->retrigger(irq, d->data);
	}
}

static int needs_oneshot(const struct irqm_desc *d)
{
	return d->kind == 1 ||
	       (d->ops && (d->ops->flags & IRQ_CHIP_ONESHOT_REQUIRED));
}

static int default_primary(void *arg)
{
	(void)arg;
	return IRQ_WAKE_THREAD;
}

int request_threaded_irq(int irq, irq_handler_t fn, irq_handler_t thread_fn,
			 unsigned long flags, const char *name, void *arg)
{
	struct irqm_desc *d = desc_of(irq);
	uint64_t fl;

	might_sleep();
	if (!d || (!fn && !thread_fn))
		return -EINVAL;
	if (!fn) {
		fn = default_primary;
		if (needs_oneshot(d) && !(flags & IRQF_ONESHOT))
			return -EINVAL;
	}
	if (!needs_oneshot(d) && fn == default_primary)
		flags &= ~(unsigned long)IRQF_ONESHOT;
	if (d->nact >= IRQM_ACTIONS)
		return -EBUSY;
	if (d->nact && (!(flags & IRQF_SHARED) ||
			!(d->act[0].flags & IRQF_SHARED)))
		return -EBUSY;
	if (d->nact == 0 && !d->started) {
		spin_lock_irqsave(&d->lock, &fl);
		d->masked = 0;
		dmask(d, irq);
		spin_unlock_irqrestore(&d->lock, fl);
		if (d->ops && d->ops->compose_msg)
			d->ops->compose_msg(irq, d->data, 0, 0);
		if (d->ops && d->ops->startup)
			d->ops->startup(irq, d->data);
		d->started = 1;
	}
	spin_lock_irqsave(&d->lock, &fl);
	d->act[d->nact].fn = fn;
	d->act[d->nact].thread_fn = thread_fn;
	d->act[d->nact].arg = arg;
	d->act[d->nact].flags = flags;
	d->act[d->nact].name = name;
	d->act[d->nact].thread_woken = 0;
	d->nact++;
	if (d->nact == 1) {
		if (flags & IRQF_NO_AUTOEN) {
			d->depth = 1;
		} else {
			d->depth = 0;
			dunmask(d, irq);
			dreplay(d, irq);
		}
	}
	spin_unlock_irqrestore(&d->lock, fl);
	return 0;
}

int request_irq(int irq, irq_handler_t fn, unsigned long flags,
		const char *name, void *arg)
{
	return request_threaded_irq(irq, fn, NULL, flags, name, arg);
}

const char *free_irq(int irq, void *arg)
{
	struct irqm_desc *d = desc_of(irq);
	const char *name = NULL;
	uint64_t fl;
	int i;

	might_sleep();
	if (!d)
		return NULL;
	spin_lock_irqsave(&d->lock, &fl);
	for (i = 0; i < d->nact; i++)
		if (d->act[i].arg == arg)
			break;
	if (i == d->nact) {
		spin_unlock_irqrestore(&d->lock, fl);
		WARN(1, "irqm: free_irq of an unknown handler");
		return NULL;
	}
	name = d->act[i].name;
	for (; i + 1 < d->nact; i++)
		d->act[i] = d->act[i + 1];
	d->nact--;
	if (d->nact == 0) {
		dmask(d, irq);
		d->threads_oneshot = 0;
		d->pending = 0;
		d->depth = 0;
	}
	spin_unlock_irqrestore(&d->lock, fl);
	if (d->nact == 0 && d->started) {
		if (d->ops && d->ops->shutdown)
			d->ops->shutdown(irq, d->data);
		d->started = 0;
	}
	return name;
}

/* Run the handlers once (desc lock dropped while they run). */
static int run_handlers(struct irqm_desc *d, int irq, uint64_t *fl)
{
	struct irqm_action acts[IRQM_ACTIONS];
	int n = d->nact, ret = IRQ_NONE, oneshot = 0;

	kmemcpy(acts, d->act, sizeof(acts));
	spin_unlock_irqrestore(&d->lock, *fl);
	for (int i = 0; i < n; i++) {
		int r = acts[i].fn(acts[i].arg);

		if (r == IRQ_WAKE_THREAD) {
			if (acts[i].flags & IRQF_ONESHOT)
				oneshot = 1;
			spin_lock_irqsave(&d->lock, fl);
			if (i < d->nact)
				d->act[i].thread_woken = 1;
			spin_unlock_irqrestore(&d->lock, *fl);
		}
		ret |= r;
	}
	spin_lock_irqsave(&d->lock, fl);
	if (oneshot) {
		d->threads_oneshot = 1;
		dmask(d, irq);
	}
	d->count++;
	if (ret == IRQ_NONE)
		d->unhandled++;
	return ret;
}

void irq_handle_virtual(int irq)
{
	struct irqm_desc *d = desc_of(irq);
	uint64_t fl;

	if (!d || WARN(d->kind != 0, "irqm: %d is not virtual", irq))
		return;
	T_ASSERT(irqs_disabled()); /* hard interrupt context */
	spin_lock_irqsave(&d->lock, &fl);
	if (d->depth > 0 || d->nact == 0 || d->threads_oneshot) {
		d->pending = 1;
		dmask(d, irq);
		spin_unlock_irqrestore(&d->lock, fl);
		return;
	}
	do {
		d->pending = 0;
		(void)run_handlers(d, irq, &fl);
	} while (d->pending && d->depth == 0 && d->nact &&
		 !d->threads_oneshot);
	spin_unlock_irqrestore(&d->lock, fl);
}

/* ---- test interface --------------------------------------------------- */

/* Fire every GSI descriptor on `gsi' (a level line: the handlers run once;
 * the caller fires again while the line stays asserted).  Returns the OR
 * of the handlers' results. */
int irqm_fire_gsi(int gsi);
int irqm_fire_gsi(int gsi)
{
	int ret = 0;
	uint64_t fl = local_irq_save();

	for (int i = 0; i < IRQM_MAX; i++) {
		struct irqm_desc *d = &g_irqm[i];
		uint64_t dfl;

		if (!d->used || d->kind != 1 || d->gsi != gsi || !d->nact ||
		    d->depth)
			continue;
		spin_lock_irqsave(&d->lock, &dfl);
		ret |= run_handlers(d, IRQ_NR_FIRST + i, &dfl);
		spin_unlock_irqrestore(&d->lock, dfl);
	}
	local_irq_restore(fl);
	return ret;
}

/* Run the woken thread functions (process context); returns how many. */
int irqm_run_threads(void);
int irqm_run_threads(void)
{
	int ran = 0;

	for (int i = 0; i < IRQM_MAX; i++) {
		struct irqm_desc *d = &g_irqm[i];
		uint64_t fl;

		if (!d->used)
			continue;
		for (int a = 0; a < d->nact; a++) {
			irq_handler_t tf;
			void *arg;

			spin_lock_irqsave(&d->lock, &fl);
			if (!d->act[a].thread_woken) {
				spin_unlock_irqrestore(&d->lock, fl);
				continue;
			}
			d->act[a].thread_woken = 0;
			tf = d->act[a].thread_fn;
			arg = d->act[a].arg;
			spin_unlock_irqrestore(&d->lock, fl);
			if (tf)
				(void)tf(arg);
			ran++;
			spin_lock_irqsave(&d->lock, &fl);
			d->threads_oneshot = 0;
			if (d->depth == 0 && d->nact && d->masked) {
				dunmask(d, IRQ_NR_FIRST + i);
				dreplay(d, IRQ_NR_FIRST + i);
			}
			spin_unlock_irqrestore(&d->lock, fl);
		}
	}
	return ran;
}

unsigned long irqm_count(int irq);
unsigned long irqm_count(int irq)
{
	struct irqm_desc *d = desc_of(irq);

	return d ? d->count : 0;
}

/* ---- the rest of the interface ------------------------------------------ */

void disable_irq_nosync(int irq)
{
	struct irqm_desc *d = desc_of(irq);
	uint64_t fl;

	if (!d)
		return;
	spin_lock_irqsave(&d->lock, &fl);
	if (d->depth++ == 0)
		dmask(d, irq);
	spin_unlock_irqrestore(&d->lock, fl);
}

void disable_irq(int irq)
{
	might_sleep();
	disable_irq_nosync(irq);
}

void enable_irq(int irq)
{
	struct irqm_desc *d = desc_of(irq);
	uint64_t fl;

	if (!d)
		return;
	spin_lock_irqsave(&d->lock, &fl);
	if (WARN(d->depth == 0, "irqm: unbalanced enable_irq")) {
		spin_unlock_irqrestore(&d->lock, fl);
		return;
	}
	if (--d->depth == 0 && d->nact && !d->threads_oneshot) {
		dunmask(d, irq);
		dreplay(d, irq);
	}
	spin_unlock_irqrestore(&d->lock, fl);
}

void synchronize_irq(int irq)
{
	(void)irq;
	might_sleep();
}

int irq_set_irq_type(int irq, unsigned int type)
{
	struct irqm_desc *d = desc_of(irq);
	uint64_t fl;
	int rc;

	if (!d)
		return -EINVAL;
	if (!d->ops || !d->ops->set_type)
		return -ENOSYS;
	spin_lock_irqsave(&d->lock, &fl);
	rc = d->ops->set_type(irq, d->data, type);
	if (!rc)
		d->trigger = type;
	spin_unlock_irqrestore(&d->lock, fl);
	return rc;
}

unsigned int irq_get_trigger_type(int irq)
{
	struct irqm_desc *d = desc_of(irq);

	return d ? d->trigger : IRQ_TYPE_NONE;
}

int irq_set_irq_wake(int irq, unsigned int on)
{
	struct irqm_desc *d = desc_of(irq);
	uint64_t fl;
	int rc = 0;

	if (!d)
		return -EINVAL;
	spin_lock_irqsave(&d->lock, &fl);
	if (on) {
		if (d->wake_depth == 0 && d->ops && d->ops->set_wake)
			rc = d->ops->set_wake(irq, d->data, 1);
		if (!rc)
			d->wake_depth++;
	} else if (d->wake_depth) {
		if (d->wake_depth == 1 && d->ops && d->ops->set_wake)
			rc = d->ops->set_wake(irq, d->data, 0);
		d->wake_depth--;
	}
	spin_unlock_irqrestore(&d->lock, fl);
	return rc;
}

int irq_is_wake_enabled(int irq)
{
	struct irqm_desc *d = desc_of(irq);

	return d ? d->wake_depth != 0 : 0;
}

int irq_desc_masked(int irq)
{
	struct irqm_desc *d = desc_of(irq);

	return d ? d->masked : 1;
}
