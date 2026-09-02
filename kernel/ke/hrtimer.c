// LikeOS-64 -- high-resolution timers on an HPET comparator.
#include <kernel/ke/hrtimer.h>
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
static hrtimer_t *g_head; /* sorted by expires_ns, earliest first */

uint64_t hrtimer_now_ns(void)
{
	return timer_get_precise_us() * 1000ULL;
}

int hrtimer_is_highres(void)
{
	return g_highres;
}

/* Program the comparator for the head of the queue.  Caller holds g_lock
 * with interrupts off.  A deadline that has already passed, or passes
 * while the comparator is being written (the classic one-shot race), is
 * caught by re-reading the counter afterwards and pulling the comparator
 * in to "now + a little" until it is safely ahead. */
static void hw_program(void)
{
	if (!g_highres)
		return;
	if (!g_head)
		return; /* nothing to wait for; a stale comparator fires at
			 * most once into an empty queue, harmlessly */

	uint64_t now_ns = hrtimer_now_ns();
	uint64_t delta_ns = g_head->expires_ns > now_ns ?
				    g_head->expires_ns - now_ns :
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
			return; /* comparator is ahead of the counter */
		delta_ns *= 2; /* lost the race; aim further out */
	}
}

void hrtimer_init(hrtimer_t *t, hrtimer_fn_t fn, void *arg)
{
	t->expires_ns = 0;
	t->fn = fn;
	t->arg = arg;
	t->next = NULL;
	t->armed = 0;
}

static void queue_remove_locked(hrtimer_t *t)
{
	hrtimer_t **pp = &g_head;

	while (*pp) {
		if (*pp == t) {
			*pp = t->next;
			t->next = NULL;
			t->armed = 0;
			return;
		}
		pp = &(*pp)->next;
	}
}

void hrtimer_start(hrtimer_t *t, uint64_t abs_ns)
{
	uint64_t flags;

	spin_lock_irqsave(&g_lock, &flags);
	if (t->armed)
		queue_remove_locked(t);
	t->expires_ns = abs_ns;
	t->armed = 1;

	hrtimer_t **pp = &g_head;
	while (*pp && (*pp)->expires_ns <= abs_ns)
		pp = &(*pp)->next;
	t->next = *pp;
	*pp = t;

	if (g_head == t)
		hw_program();
	spin_unlock_irqrestore(&g_lock, flags);
}

int hrtimer_cancel(hrtimer_t *t)
{
	uint64_t flags;
	int was_armed;

	spin_lock_irqsave(&g_lock, &flags);
	was_armed = t->armed;
	if (was_armed)
		queue_remove_locked(t);
	spin_unlock_irqrestore(&g_lock, flags);
	return was_armed;
}

/* Fire everything that is due.  Callbacks run without the queue lock so
 * they may re-arm; a timer is unlinked and marked unarmed BEFORE its
 * callback runs, so hrtimer_cancel() from another CPU that observes
 * armed == 0 knows the callback is either done or about to run without
 * touching the queue -- which is the guarantee documented in the header
 * only when the caller has also excluded its own callback's effects, as
 * every user here does by checking its own state after cancel. */
static void hrtimer_run_expired(void)
{
	uint64_t flags;

	spin_lock_irqsave(&g_lock, &flags);
	for (;;) {
		hrtimer_t *t = g_head;
		uint64_t now = hrtimer_now_ns();

		if (!t || t->expires_ns > now)
			break;
		g_head = t->next;
		t->next = NULL;
		t->armed = 0;
		spin_unlock_irqrestore(&g_lock, flags);
		t->fn(t);
		spin_lock_irqsave(&g_lock, &flags);
	}
	hw_program();
	spin_unlock_irqrestore(&g_lock, flags);
}

void hrtimer_irq(void)
{
	if (g_regs) {
		/* Edge-triggered: nothing to acknowledge in the ISR register
		 * (that is for level mode); the comparator simply fired. */
	}
	hrtimer_run_expired();
}

void hrtimer_tick(void)
{
	/* In high-resolution mode this is a safety net for a comparator
	 * write that lost its race after all; in fallback mode it is the
	 * only expiry path. */
	if (g_head)
		hrtimer_run_expired();
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
		cur->state = TASK_BLOCKED;
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
	/* Timers 0 and 1 are the ones legacy-replacement mode would take;
	 * leave them alone where a third exists. */
	g_timer_idx = ntimers >= 3 ? ntimers - 1 : (ntimers == 2 ? 1 : 0);

	uint64_t tcfg = rd(HPET_TN_CONFIG(g_timer_idx));
	uint32_t route_cap = (uint32_t)(tcfg >> 32);

	/* Pick a GSI this comparator can drive that is not one of the ISA
	 * lines (0-15), which other devices already use. */
	for (int g = 31; g >= 16; g--) {
		if (route_cap & (1u << g)) {
			g_gsi = g;
			break;
		}
	}
	if (g_gsi < 0) {
		kprintf("hrtimer: HPET timer %d has no free interrupt route (cap 0x%x), tick fallback\n",
			g_timer_idx, route_cap);
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
