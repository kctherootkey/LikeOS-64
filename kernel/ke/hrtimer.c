// LikeOS -- high-resolution timers on an HPET comparator.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/hrtimer.h>
#include <kernel/ke/percpu.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/signal.h>
#include <kernel/hal/ioapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ---- HPET comparator ---------------------------------------------------- */

/* Registers (offsets in bytes). */
#define HPET_GEN_CAP 0x000
#define HPET_GEN_CONFIG 0x010
#define HPET_GEN_ISR 0x020
#define HPET_MAIN_COUNTER 0x0F0
#define HPET_TN_CONFIG(n) (0x100 + 0x20 * (n))
#define HPET_TN_COMPARATOR(n) (0x108 + 0x20 * (n))

#define HPET_CAP_COUNT_SIZE (1ULL << 13) /* 64-bit main counter */
#define HPET_CAP_NUM_TIM_SHIFT 8
#define HPET_TN_INT_TYPE_LEVEL (1ULL << 1)
#define HPET_TN_INT_ENB (1ULL << 2)
#define HPET_TN_TYPE_PERIODIC (1ULL << 3)
#define HPET_TN_32MODE (1ULL << 8)
#define HPET_TN_INT_ROUTE_SHIFT 9
#define HPET_TN_INT_ROUTE_MASK (0x1FULL << 9)
#define HPET_TN_FSB_EN (1ULL << 14)

static volatile uint64_t *g_regs; /* NULL: tick-driven fallback */
static uint32_t g_period_fs;
static int g_timer_idx = -1;
static int g_gsi = -1;
static int g_highres;

static inline uint64_t rd(uint32_t off)
{
	return g_regs[off / 8];
}
static inline void wr(uint32_t off, uint64_t v)
{
	g_regs[off / 8] = v;
}

/* ns <-> HPET ticks.  period is in femtoseconds; 1 ns = 1e6 fs. */
static inline uint64_t ns_to_hpet(uint64_t ns)
{
	/* ns * 1e6 / period; split to keep 64-bit headroom for large ns. */
	uint64_t whole = ns / 1000000ULL, rem = ns % 1000000ULL;
	return (whole * 1000000000000ULL) / g_period_fs +
	       (rem * 1000000ULL) / g_period_fs;
}

/* ---- Queue --------------------------------------------------------------- */

static spinlock_t g_lock;
/* One queue per CPU, each sorted by expires_ns, earliest first.
 *
 * With the local timers one-shot (g_lapic_ce), a timer goes on the queue of
 * the CPU that armed it, and that CPU's local timer is armed for it: its
 * callback runs there, in the local timer's interrupt.  Before that -- and
 * on a machine whose local timer cannot run one-shot -- everything is on
 * queue 0 and the HPET comparator serves it, as it always did.
 *
 * One lock for all of them: what it protects is short (a sorted insert or
 * an unlink), and a timer re-armed from another CPU moves between two
 * queues in one step under it. */
static hrtimer_t *g_cpu_head[MAX_CPUS];
static int g_lapic_ce;

uint64_t hrtimer_now_ns(void)
{
	return timer_get_precise_ns();
}

int hrtimer_is_highres(void)
{
	return g_highres;
}

/* Which queue a timer armed now goes on.  Caller holds g_lock. */
static int queue_for_new_timer(void)
{
	uint32_t c;

	if (!g_lapic_ce)
		return 0;
	c = this_cpu_id();
	if (c >= MAX_CPUS || !lapic_clockevent_ready(c))
		return 0; /* this CPU's local timer is not running yet */
	return (int)c;
}

/* Program the comparator for the head of queue 0 (HPET mode).  Caller holds
 * g_lock with interrupts off.  A deadline that has already passed, or
 * passes while the comparator is being written (the classic one-shot race),
 * is caught by re-reading the counter afterwards and pulling the
 * comparator in to "now + a little" until it is safely ahead. */
static void hw_program(void)
{
	hrtimer_t *head = g_cpu_head[0];

	if (!g_highres || g_lapic_ce)
		return;
	if (!head)
		return; /* nothing to wait for; a stale comparator fires at
			 * most once into an empty queue, harmlessly */

	uint64_t now_ns = hrtimer_now_ns();
	uint64_t delta_ns = head->expires_ns > now_ns ?
				    head->expires_ns - now_ns :
				    0;
	/* Never closer than 5 us: below that the write itself loses the
	 * race and the interrupt is missed for a full counter wrap. */
	if (delta_ns < 5000)
		delta_ns = 5000;

	for (int attempt = 0; attempt < 8; attempt++) {
		uint64_t cnt = rd(HPET_MAIN_COUNTER);
		uint64_t target = cnt + ns_to_hpet(delta_ns);

		wr(HPET_TN_COMPARATOR(g_timer_idx), target);
		if ((int64_t)(target - rd(HPET_MAIN_COUNTER)) > 0)
			break; /* comparator is ahead of the counter */
		delta_ns *= 2; /* lost the race; aim further out */
	}
}

/* After queue `q''s head changed: point the device serving it at the new
 * head.  Caller holds g_lock with interrupts off.  A local timer can only
 * be programmed from its own CPU; a remote queue whose head became LATER
 * (a cancel) is left alone -- its timer fires early into nothing and is
 * re-armed then -- and timers are only ever added to the arming CPU's own
 * queue, so a head that became EARLIER is always local. */
static void queue_head_changed(int q)
{
	if (!g_lapic_ce) {
		hw_program();
		return;
	}
	if ((uint32_t)q == this_cpu_id())
		lapic_clockevent_program(g_cpu_head[q] ?
						 g_cpu_head[q]->expires_ns :
						 0);
}

void hrtimer_init(hrtimer_t *t, hrtimer_fn_t fn, void *arg)
{
	t->expires_ns = 0;
	t->fn = fn;
	t->arg = arg;
	t->next = NULL;
	t->armed = 0;
	t->cpu = 0;
}

/* Unlink `t' from the queue it is on.  Returns 1 if it was that queue's
 * head.  Caller holds g_lock. */
static int queue_remove_locked(hrtimer_t *t)
{
	int q = t->cpu;
	hrtimer_t **pp;

	if (q < 0 || q >= MAX_CPUS)
		q = 0;
	pp = &g_cpu_head[q];
	while (*pp) {
		if (*pp == t) {
			int was_head = (pp == &g_cpu_head[q]);

			*pp = t->next;
			t->next = NULL;
			t->armed = 0;
			return was_head;
		}
		pp = &(*pp)->next;
	}
	t->armed = 0;
	return 0;
}

void hrtimer_start(hrtimer_t *t, uint64_t abs_ns)
{
	uint64_t flags;
	int q;

	spin_lock_irqsave(&g_lock, &flags);
	if (t->armed)
		(void)queue_remove_locked(t);
	q = queue_for_new_timer();
	t->expires_ns = abs_ns;
	t->armed = 1;
	t->cpu = q;

	hrtimer_t **pp = &g_cpu_head[q];
	while (*pp && (*pp)->expires_ns <= abs_ns)
		pp = &(*pp)->next;
	t->next = *pp;
	*pp = t;

	if (g_cpu_head[q] == t)
		queue_head_changed(q);
	spin_unlock_irqrestore(&g_lock, flags);
}

int hrtimer_cancel(hrtimer_t *t)
{
	uint64_t flags;
	int was_armed;

	spin_lock_irqsave(&g_lock, &flags);
	was_armed = t->armed;
	if (was_armed)
		(void)queue_remove_locked(t);
	spin_unlock_irqrestore(&g_lock, flags);
	return was_armed;
}

/* Fire everything that is due on queue `q'.  Callbacks run without the
 * queue lock so they may re-arm; a timer is unlinked and marked unarmed
 * BEFORE its callback runs, so hrtimer_cancel() from another CPU that
 * observes armed == 0 knows the callback is either done or about to run
 * without touching the queue -- which is the guarantee documented in the
 * header only when the caller has also excluded its own callback's effects,
 * as every user here does by checking its own state after cancel.
 * Returns the deadline of the next timer left on the queue (0: none). */
static uint64_t hrtimer_run_queue(int q)
{
	uint64_t flags, next;

	spin_lock_irqsave(&g_lock, &flags);
	for (;;) {
		hrtimer_t *t = g_cpu_head[q];
		uint64_t now = hrtimer_now_ns();

		if (!t || t->expires_ns > now)
			break;
		g_cpu_head[q] = t->next;
		t->next = NULL;
		t->armed = 0;
		spin_unlock_irqrestore(&g_lock, flags);
		t->fn(t);
		spin_lock_irqsave(&g_lock, &flags);
	}
	next = g_cpu_head[q] ? g_cpu_head[q]->expires_ns : 0;
	if (!g_lapic_ce)
		hw_program();
	spin_unlock_irqrestore(&g_lock, flags);
	return next;
}

uint64_t hrtimer_run_local(void)
{
	uint32_t c = this_cpu_id();

	if (c >= MAX_CPUS)
		return 0;
	return hrtimer_run_queue((int)c);
}

void hrtimer_clockevent_started(void)
{
	uint64_t flags;
	uint32_t c = this_cpu_id();

	spin_lock_irqsave(&g_lock, &flags);
	if (!g_lapic_ce && c == 0) {
		/* The boot CPU's local timer serves queue 0 from now on; the
		 * comparator is parked and its interrupt left masked off. */
		g_lapic_ce = 1;
		if (g_regs) {
			wr(HPET_TN_COMPARATOR(g_timer_idx),
			   rd(HPET_MAIN_COUNTER) + ~0ULL / 4);
			wr(HPET_TN_CONFIG(g_timer_idx),
			   rd(HPET_TN_CONFIG(g_timer_idx)) & ~HPET_TN_INT_ENB);
		}
		g_highres = 1;
		kprintf("hrtimer: on the CPUs' local timers (HPET comparator retired)\n");
	}
	if (g_lapic_ce && c < MAX_CPUS)
		lapic_clockevent_program(g_cpu_head[c] ?
						 g_cpu_head[c]->expires_ns : 0);
	spin_unlock_irqrestore(&g_lock, flags);
}

void hrtimer_irq(void)
{
	/* Edge-triggered: nothing to acknowledge in the ISR register (that is
	 * for level mode); the comparator simply fired.  Once the local timers
	 * carry the queues the comparator is retired, and a late interrupt
	 * from it finds nothing to do. */
	if (!g_lapic_ce)
		(void)hrtimer_run_queue(0);
}

void hrtimer_tick(void)
{
	/* In HPET mode this is a safety net for a comparator write that lost
	 * its race after all; without a comparator it is the only expiry
	 * path.  With the local timers in charge every CPU's own timer
	 * interrupt runs its queue, so there is nothing to do here. */
	if (!g_lapic_ce && g_cpu_head[0])
		(void)hrtimer_run_queue(0);
}

/* ---- Sleeping ------------------------------------------------------------ */

struct sleep_wait {
	hrtimer_t timer;
	task_t *task;
};

static void sleep_fire(hrtimer_t *t)
{
	struct sleep_wait *w = (struct sleep_wait *)t->arg;
	task_t *task = w->task;

	/* Same claim discipline as every other waker: BLOCKED -> READY by
	 * CAS, and enqueue only what was claimed. */
	if (sched_claim_wake(task, TASK_BLOCKED)) {
		task->wait_channel = NULL;
		task->wakeup_tick = 0;
		sched_enqueue_ready(task);
	}
}

static void wait_timer_fire(hrtimer_t *t)
{
	task_t *task = (task_t *)t->arg;

	/* Only a task still parked: claimed by CAS, as every waker does, and
	 * enqueued only if claimed. */
	if (sched_claim_wake(task, TASK_BLOCKED)) {
		task->wait_channel = NULL;
		task->wakeup_tick = 0;
		sched_enqueue_ready(task);
	}
}

void hrtimer_wait_arm(struct task *task, uint64_t abs_ns)
{
	/* A queued timer must never be initialised again: off the queue
	 * first.  Normally a no-op -- every wait disarms on its way out. */
	hrtimer_cancel(&task->wait_timer);
	hrtimer_init(&task->wait_timer, wait_timer_fire, task);
	hrtimer_start(&task->wait_timer, abs_ns);
}

void hrtimer_wait_disarm(struct task *task)
{
	hrtimer_cancel(&task->wait_timer);
}

int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns)
{
	task_t *cur = sched_current();
	struct sleep_wait w;

	if (!cur)
		return -EFAULT;
	hrtimer_init(&w.timer, sleep_fire, &w);
	w.task = cur;

	for (;;) {
		uint64_t now = hrtimer_now_ns();

		if (now >= abs_ns) {
			if (remaining_ns)
				*remaining_ns = 0;
			return 0;
		}
		if (signal_pending(cur)) {
			if (remaining_ns)
				*remaining_ns = abs_ns - now;
			return -EINTR;
		}

		/* Two orderings, both required, and they nest.
		 *
		 * The identity goes down before TASK_BLOCKED: the state is what
		 * makes a task claimable, and a waker that arrives on the old
		 * `wait_channel' or an old `wakeup_tick' claims a task that is
		 * not waiting on either.
		 *
		 * BLOCKED then goes down before ARMING, so a comparator that
		 * fires before sched_schedule() runs finds a task it can claim;
		 * the scheduler then sees a READY task and does not park it
		 * (see the park/unpark CAS notes in sched.c). */
		uint64_t flags = local_irq_save();
		cur->wait_channel = &w;
		/* Belt and braces: the periodic tick wakes it too, one tick
		 * late, should the comparator ever be missed. */
		cur->wakeup_tick = timer_ticks() + timer_ns_to_ticks(abs_ns - now) + 2;
		task_set_blocked(cur);
		hrtimer_start(&w.timer, abs_ns);
		local_irq_restore(flags);

		sched_schedule();

		hrtimer_cancel(&w.timer);
		cur->wakeup_tick = 0;
		/* And the channel, which points into THIS frame: `w' goes out
		 * of scope at the end of the call, and a channel left behind
		 * is a stack address that some later allocation is free to
		 * reuse as a channel of its own -- at which point a wake for
		 * that object claims this task instead. */
		cur->wait_channel = NULL;
	}
}

/* ---- Platform bring-up --------------------------------------------------- */

/* The HPET block is mapped by timer_init_hpet(); this borrows the mapping
 * (through the accessor timer.c exports) and takes one comparator. */
extern volatile uint64_t *timer_hpet_regs(void);
extern uint32_t timer_hpet_period_fs(void);

void hrtimer_init_hw(void)
{
	spinlock_init(&g_lock, "hrtimer");
	g_regs = timer_hpet_regs();
	g_period_fs = timer_hpet_period_fs();
	if (!g_regs || !g_period_fs) {
		kprintf("hrtimer: no HPET, timers run at tick granularity\n");
		g_regs = NULL;
		return;
	}

	uint64_t cap = rd(HPET_GEN_CAP);
	int ntimers = (int)((cap >> HPET_CAP_NUM_TIM_SHIFT) & 0x1F) + 1;

	if (!(cap & HPET_CAP_COUNT_SIZE)) {
		kprintf("hrtimer: HPET counter is 32-bit, using tick fallback\n");
		g_regs = NULL;
		return;
	}
	/* A comparator that can raise an interrupt on an I/O APIC line that
	 * is not one of the ISA lines (0-15), which other devices already use.
	 * Not simply the last one: on Intel chipsets only the first three
	 * comparators can be routed to the I/O APIC at all -- the rest report
	 * an empty route capability and are meant for MSI delivery -- and
	 * taking the last of eight left the ThinkPad P50 on the tick, where
	 * every timed wait ended up to a whole tick late.  Timers 0 and 1 are
	 * the ones legacy-replacement mode would take, so they are the last
	 * resort, used only where no other comparator can be routed. */
	uint64_t tcfg = 0;
	uint32_t route_cap = 0;
	static const int order_fallback[] = { 1, 0 };

	/* Legacy replacement routes timers 0 and 1 to IRQ 0 and 8 whatever
	 * their own route says: then they are not candidates at all. */
	int legacy_rt = (rd(HPET_GEN_CONFIG) & 0x2) != 0;

	/* Only a line the I/O APIC has: the comparator may offer lines up to
	 * 31 that this machine's I/O APIC does not reach (VirtualBox: 24
	 * lines, comparator offering 20-31). */
	int max_gsi = (int)ioapic_max_gsi();

	if (max_gsi > 31)
		max_gsi = 31;
	for (int pass = 0; pass < (legacy_rt ? 1 : 2) && g_gsi < 0; pass++) {
		int count = pass == 0 ? ntimers - 2 : 2;

		for (int k = 0; k < count && g_gsi < 0; k++) {
			int idx = pass == 0 ? ntimers - 1 - k : order_fallback[k];

			if (idx < 0 || idx >= ntimers)
				continue;
			tcfg = rd(HPET_TN_CONFIG(idx));
			route_cap = (uint32_t)(tcfg >> 32);
			for (int g = max_gsi; g >= 16; g--) {
				if (route_cap & (1u << g)) {
					g_gsi = g;
					g_timer_idx = idx;
					break;
				}
			}
		}
	}
	if (g_gsi < 0) {
		kprintf("hrtimer: no HPET comparator of %d can be routed to an I/O APIC line above 15, tick fallback\n",
			ntimers);
		g_regs = NULL;
		return;
	}

	/* One-shot, edge-triggered, 64-bit, routed to our GSI; enabled. */
	tcfg &= ~(HPET_TN_INT_TYPE_LEVEL | HPET_TN_TYPE_PERIODIC |
		  HPET_TN_32MODE | HPET_TN_FSB_EN | HPET_TN_INT_ROUTE_MASK);
	tcfg |= ((uint64_t)g_gsi << HPET_TN_INT_ROUTE_SHIFT) | HPET_TN_INT_ENB;
	wr(HPET_TN_CONFIG(g_timer_idx), tcfg);
	/* Park the comparator far in the future until something is armed. */
	wr(HPET_TN_COMPARATOR(g_timer_idx), rd(HPET_MAIN_COUNTER) + ~0ULL / 4);

	if (ioapic_configure_legacy_irq((uint8_t)g_gsi, HRTIMER_VECTOR,
					IOAPIC_POLARITY_HIGH,
					IOAPIC_TRIGGER_EDGE) != 0) {
		kprintf("hrtimer: IOAPIC route for GSI %d failed, tick fallback\n",
			g_gsi);
		wr(HPET_TN_CONFIG(g_timer_idx), tcfg & ~HPET_TN_INT_ENB);
		g_regs = NULL;
		return;
	}
	g_highres = 1;
	kprintf("hrtimer: HPET timer %d on GSI %d (vector %d), %u fs/tick\n",
		g_timer_idx, g_gsi, HRTIMER_VECTOR, g_period_fs);
}
