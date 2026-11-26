// LikeOS epoll
//
// An epoll set watches any number of open objects -- "items", each one an
// object and the descriptor number it was added under.  An item is registered
// on the wait queue(s) of its object once, when it is added, and stays there:
// the object's wakes call ep_poll_callback(), which puts the item on the
// set's ready list and wakes one thread waiting in epoll_wait().  epoll_wait()
// then looks only at the ready list -- never at every item -- asking each
// ready item's object for its current state before reporting it.
//
// Items are keyed by object, not by descriptor number: closing the last
// descriptor of an object removes it from every set (eventpoll_release(),
// called from the object's final release), as everywhere else.  A UNIX
// socket, whose code has no such hook, is kept in memory by its items and
// dropped from a set once its last descriptor is gone.
//
// Locks, outermost first:
//
//   ep->mtx       per set, sleeping: serialises epoll_ctl() on the set.
//   ep_tab_lock   global: the item table (by object), every set's item
//                 list, an item's `hashed' state, the nesting graph.
//   ep_idx_lock   global: the instance table (index -> set).
//   a wait-queue head's lock (an object's queue): held by the waker while
//                 the callbacks run.
//   ep->lock      per set: the ready list, the items' ready state and
//                 events, the waiters.  Taken inside a wait-queue lock (the
//                 callback), so NEVER hold it while taking a wait-queue lock
//                 or asking an object for its state: both happen with it
//                 dropped.  Short sections, interrupts off.
//
// An item is removed by whoever takes it out of the item table ("claims"
// it, ep_tab_lock): epoll_ctl(DEL), the object's release, the set's own
// teardown, or epoll_wait finding a UNIX socket closed.  The claimer takes
// the item off its wait queues (after which no callback can be running for
// it) and only then marks it dead under ep->lock.  An item an epoll_wait is
// looking at is pinned (ep->lock); a dead pinned item is freed by the last
// unpin.  The object of an item is asked for its state only with a reference
// taken while ep->lock showed the item alive (so its release had not
// finished), and that reference is dropped with no lock held.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/eventpoll.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/cred.h>
#include <kernel/mm/memory.h>
#include <kernel/fs/file.h>
#include <kernel/uapi/bug.h>

/* How deep sets may watch sets: a chain of at most EP_MAX_NESTS + 1. */
#define EP_MAX_NESTS 4
/* The largest maxevents: what fits an int's worth of bytes. */
#define EP_MAX_EVENTS ((int)(0x7fffffff / sizeof(struct epoll_event)))
/* Wait queues one object may name (most name one). */
#define EP_NWAIT 4
/* Items looked at per pass of epoll_wait (kept on the stack). */
#define EP_CHUNK 16
#define EP_HASH_BITS 12
#define EP_HASH_SIZE (1u << EP_HASH_BITS)

/* Bits of an item's events that are flags, not readiness. */
#define EP_PRIVATE_BITS (EPOLLWAKEUP | EPOLLONESHOT | EPOLLET | EPOLLEXCLUSIVE)
/* What EPOLLEXCLUSIVE may be combined with. */
#define EP_EXCLUSIVE_OK_BITS                                              \
	(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLWAKEUP | EPOLLET | \
	 EPOLLEXCLUSIVE)

/* An item's place relative to the ready list (ep->lock). */
#define EPI_IDLE 0
#define EPI_READY 1 /* on the ready list */
#define EPI_TX 2 /* taken off it by an epoll_wait looking at it */

struct eventpoll;

struct ep_pwq {
	struct wait_queue_entry wait; /* priv: the item */
	struct wait_queue_head *head;
};

struct epitem {
	/* ep_tab_lock: the item table, the set's lists */
	struct epitem *h_next, **h_pprev;
	struct epitem *e_next, **e_pprev;
	struct epitem *t_next, **t_pprev; /* items watching a set */
	/* ep->lock: the ready list */
	struct epitem *rd_next, *rd_prev;
	struct eventpoll *ep;
	void *file; /* the fd-table entry: the object */
	struct eventpoll *tgt; /* the set this item watches, if it is one */
	int fd;
	uint32_t events; /* ep->lock once inserted */
	uint64_t data;
	int pin; /* ep->lock */
	uint8_t rdstate; /* EPI_*, ep->lock */
	uint8_t tx_pending; /* woken while EPI_TX */
	uint8_t dead; /* removed; freed by the last unpin */
	uint8_t hashed; /* in the item table (ep_tab_lock) */
	uint8_t item_held; /* holds the object's memory (poll_obj_item_hold) */
	uint8_t exclusive;
	uint8_t nwait;
	uint8_t nwait_overflow;
	struct ep_pwq pwq[EP_NWAIT];
};

/* A thread in epoll_wait(), on ep->waiters (ep->lock). */
struct ep_waiter {
	task_t *task;
	struct ep_waiter *next, **pprev;
};

struct eventpoll {
	spinlock_t lock;
	struct epitem *rd_head, *rd_tail;
	int nready;
	struct ep_waiter *waiters;
	/* poll()/select()/other sets watching THIS set */
	struct wait_queue_head poll_wq;
	struct mutex mtx;
	struct epitem *items; /* ep_tab_lock */
	struct epitem *tgt_items; /* ...of them, the ones watching a set */
	int nitems; /* ep_tab_lock */
	int users; /* descriptors and lookups; 0: being torn down */
	int memref; /* the table's, plus every claimer's while it works */
	int idx;
	/* Nesting checks (ep_tab_lock): memoised per check */
	uint32_t lc_gen_down, lc_gen_up;
	int lc_down, lc_up;
};

static struct eventpoll *ep_table[MAX_EPOLL_INSTANCES];
static spinlock_t ep_idx_lock = SPINLOCK_INIT("epoll_idx");
/* Held while an item leaves the queues it is on, and while a queue that is
 * about to be freed sends its items away (eventpoll_wq_free): an item's
 * queue pointer is only followed under it.  Taken outside every wait-queue
 * lock, never inside one. */
static spinlock_t ep_pollfree_lock = SPINLOCK_INIT("epoll_pollfree");
static spinlock_t ep_tab_lock = SPINLOCK_INIT("epoll_tab");
static struct epitem *ep_hash[EP_HASH_SIZE];
static uint32_t ep_lc_gen;
static struct eventpoll *ep_lc_target;
static int64_t ep_watches;
static int64_t ep_max_watches;

/* Each item costs this much, for the system-wide limit. */
#define EP_ITEM_COST (sizeof(struct epitem) + 64)

static unsigned ep_hash_fn(const void *file)
{
	uint64_t x = (uint64_t)(uintptr_t)file;

	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	return (unsigned)(x & (EP_HASH_SIZE - 1));
}

/* The system-wide number of items: 4% of memory, as everywhere. */
static int64_t ep_watch_limit(void)
{
	int64_t m = __atomic_load_n(&ep_max_watches, __ATOMIC_RELAXED);

	if (!m) {
		memory_stats_t st;

		mm_memset(&st, 0, sizeof(st));
		mm_get_memory_stats(&st);
		m = (int64_t)((st.total_memory / 25) / EP_ITEM_COST);
		if (m < 4096)
			m = 4096;
		__atomic_store_n(&ep_max_watches, m, __ATOMIC_RELAXED);
	}
	return m;
}

/* ---------------------------------------------------------------------------
 * Instances
 * ------------------------------------------------------------------------ */

static void ep_memput(struct eventpoll *ep)
{
	if (__atomic_sub_fetch(&ep->memref, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	WARN_ON(ep->items != NULL || ep->rd_head != NULL ||
		ep->waiters != NULL); /* epoll set freed with items */
	mutex_destroy(&ep->mtx);
	kfree(ep);
}

/* The set behind an index the caller holds a reference on. */
static struct eventpoll *ep_lookup(int idx)
{
	struct eventpoll *ep;
	uint64_t fl;

	if (idx < 0 || idx >= MAX_EPOLL_INSTANCES)
		return NULL;
	spin_lock_irqsave(&ep_idx_lock, &fl);
	ep = ep_table[idx];
	spin_unlock_irqrestore(&ep_idx_lock, fl);
	return ep;
}

static int ep_create(void)
{
	struct eventpoll *ep = kalloc(sizeof(*ep));
	uint64_t fl;
	int idx = -1;

	if (!ep)
		return -ENOMEM;
	mm_memset(ep, 0, sizeof(*ep));
	spinlock_init(&ep->lock, "epoll");
	wq_head_init(&ep->poll_wq, "epoll-poll");
	mutex_init(&ep->mtx, "epoll-ctl");
	ep->users = 1; /* the descriptor about to be returned */
	ep->memref = 1; /* the table's */

	spin_lock_irqsave(&ep_idx_lock, &fl);
	for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
		if (!ep_table[i]) {
			idx = i;
			break;
		}
	}
	if (idx >= 0) {
		ep->idx = idx;
		ep_table[idx] = ep;
	}
	spin_unlock_irqrestore(&ep_idx_lock, fl);
	if (idx < 0) {
		mutex_destroy(&ep->mtx);
		kfree(ep);
		/* GLib aborts when epoll_create1 fails: say so loudly. */
		WARN_RATELIMIT(1, "epoll: instance table full (%d)",
			       MAX_EPOLL_INSTANCES);
		return -ENFILE;
	}
	return idx;
}

static void ep_teardown(struct eventpoll *ep);

void epoll_get(int idx)
{
	uint64_t fl;

	if (idx < 0 || idx >= MAX_EPOLL_INSTANCES)
		return;
	spin_lock_irqsave(&ep_idx_lock, &fl);
	if (ep_table[idx]) {
		/* Only ever on an instance a descriptor already holds. */
		WARN_ON(ep_table[idx]->users <= 0);
		ep_table[idx]->users++;
	}
	spin_unlock_irqrestore(&ep_idx_lock, fl);
}

int epoll_tryget(int idx)
{
	uint64_t fl;
	int ok = 0;

	if (idx < 0 || idx >= MAX_EPOLL_INSTANCES)
		return 0;
	spin_lock_irqsave(&ep_idx_lock, &fl);
	if (ep_table[idx] && ep_table[idx]->users > 0) {
		ep_table[idx]->users++;
		ok = 1;
	}
	spin_unlock_irqrestore(&ep_idx_lock, fl);
	return ok;
}

void epoll_put(int idx)
{
	struct eventpoll *ep;
	uint64_t fl;
	int last = 0;

	if (idx < 0 || idx >= MAX_EPOLL_INSTANCES)
		return;
	spin_lock_irqsave(&ep_idx_lock, &fl);
	ep = ep_table[idx];
	if (ep && ep->users > 0)
		last = --ep->users == 0;
	spin_unlock_irqrestore(&ep_idx_lock, fl);
	/* The slot stays taken (users 0: no new reference) until the
	 * teardown is through, so the index -- the marker other sets know
	 * this one by -- cannot name a new set before their items are gone. */
	if (last)
		ep_teardown(ep);
}

/* ---------------------------------------------------------------------------
 * Items: the table, the ready list
 * ------------------------------------------------------------------------ */

/* ep_tab_lock held */
static struct epitem *ep_find(struct eventpoll *ep, void *file, int fd)
{
	for (struct epitem *e = ep_hash[ep_hash_fn(file)]; e; e = e->h_next)
		if (e->ep == ep && e->file == file && e->fd == fd)
			return e;
	return NULL;
}

/* ep_tab_lock held */
static void ep_link(struct epitem *epi)
{
	struct eventpoll *ep = epi->ep;
	struct epitem **b = &ep_hash[ep_hash_fn(epi->file)];

	epi->h_next = *b;
	if (*b)
		(*b)->h_pprev = &epi->h_next;
	epi->h_pprev = b;
	__atomic_store_n(b, epi, __ATOMIC_RELEASE);

	epi->e_next = ep->items;
	if (ep->items)
		ep->items->e_pprev = &epi->e_next;
	epi->e_pprev = &ep->items;
	ep->items = epi;

	if (epi->tgt) {
		epi->t_next = ep->tgt_items;
		if (ep->tgt_items)
			ep->tgt_items->t_pprev = &epi->t_next;
		epi->t_pprev = &ep->tgt_items;
		ep->tgt_items = epi;
	}
	ep->nitems++;
	epi->hashed = 1;
}

/* ep_tab_lock held: the claim.  Whoever does this removes the item. */
static void ep_unlink(struct epitem *epi)
{
	*epi->h_pprev = epi->h_next;
	if (epi->h_next)
		epi->h_next->h_pprev = epi->h_pprev;
	*epi->e_pprev = epi->e_next;
	if (epi->e_next)
		epi->e_next->e_pprev = epi->e_pprev;
	if (epi->tgt) {
		*epi->t_pprev = epi->t_next;
		if (epi->t_next)
			epi->t_next->t_pprev = epi->t_pprev;
	}
	epi->ep->nitems--;
	epi->hashed = 0;
}

/* ep->lock held */
static void rd_append(struct eventpoll *ep, struct epitem *epi)
{
	epi->rd_next = NULL;
	epi->rd_prev = ep->rd_tail;
	if (ep->rd_tail)
		ep->rd_tail->rd_next = epi;
	else
		ep->rd_head = epi;
	ep->rd_tail = epi;
	ep->nready++;
	epi->rdstate = EPI_READY;
}

/* ep->lock held */
static void rd_push_head(struct eventpoll *ep, struct epitem *epi)
{
	epi->rd_prev = NULL;
	epi->rd_next = ep->rd_head;
	if (ep->rd_head)
		ep->rd_head->rd_prev = epi;
	else
		ep->rd_tail = epi;
	ep->rd_head = epi;
	ep->nready++;
	epi->rdstate = EPI_READY;
}

/* ep->lock held */
static void rd_unlink(struct eventpoll *ep, struct epitem *epi)
{
	if (epi->rd_prev)
		epi->rd_prev->rd_next = epi->rd_next;
	else
		ep->rd_head = epi->rd_next;
	if (epi->rd_next)
		epi->rd_next->rd_prev = epi->rd_prev;
	else
		ep->rd_tail = epi->rd_prev;
	epi->rd_next = epi->rd_prev = NULL;
	ep->nready--;
	epi->rdstate = EPI_IDLE;
}

/* No lock held. */
static void ep_item_free(struct epitem *epi)
{
	if (epi->item_held)
		poll_obj_item_put(epi->file);
	__atomic_sub_fetch(&ep_watches, 1, __ATOMIC_RELAXED);
	kfree(epi);
}

/* Take an item off every queue it is on.  Once this has returned, no
 * callback for it is running or can start.  A queue freed meanwhile has
 * already sent the item away (its head pointer is NULL). */
static void ep_unregister(struct epitem *epi)
{
	uint64_t fl;

	spin_lock_irqsave(&ep_pollfree_lock, &fl);
	for (int i = 0; i < epi->nwait; i++)
		if (epi->pwq[i].head)
			wq_remove(epi->pwq[i].head, &epi->pwq[i].wait);
	spin_unlock_irqrestore(&ep_pollfree_lock, fl);
}

/* Remove an item this caller claimed.  No lock held; the set is kept alive by
 * the caller. */
static void ep_remove_claimed(struct epitem *epi)
{
	struct eventpoll *ep = epi->ep;
	uint64_t fl;
	int do_free;

	/* Off the object's queues first. */
	ep_unregister(epi);

	spin_lock_irqsave(&ep->lock, &fl);
	if (epi->rdstate == EPI_READY)
		rd_unlink(ep, epi);
	epi->dead = 1;
	do_free = epi->pin == 0;
	spin_unlock_irqrestore(&ep->lock, fl);
	if (do_free)
		ep_item_free(epi);
}

/* Wake one thread in epoll_wait() (ep->lock held).  The most recent waiter
 * first: it is the one most likely to still be warm. */
static task_t *ep_claim_waiter(struct eventpoll *ep)
{
	for (struct ep_waiter *w = ep->waiters; w; w = w->next) {
		if (sched_claim_wake(w->task, TASK_BLOCKED)) {
			w->task->wait_channel = NULL;
			w->task->wakeup_tick = 0;
			return w->task;
		}
	}
	return NULL;
}

/* Deferred from a callback: wake whoever polls this set. */
static void ep_poll_wq_wake(void *arg)
{
	struct eventpoll *ep = arg;

	wq_wake_poll(&ep->poll_wq, POLLIN | POLLRDNORM);
	ep_memput(ep);
}

/* An object an item watches announced a change.  Runs under that object's
 * queue lock -- see the lock order at the top. */
static int ep_poll_callback(struct wait_queue_entry *wait, unsigned key,
			    struct wq_wake_ctx *ctx)
{
	struct epitem *epi = wait->priv;
	struct eventpoll *ep = epi->ep;
	uint64_t fl;
	int woke = 0;

	spin_lock_irqsave(&ep->lock, &fl);
	/* Dead, or disarmed by EPOLLONESHOT until epoll_ctl(MOD). */
	if (epi->dead || !(epi->events & ~EP_PRIVATE_BITS))
		goto out;
	/* A wake that names what changed, none of which this item wants. */
	if (key && !(key & epi->events))
		goto out;

	if (epi->rdstate == EPI_IDLE)
		rd_append(ep, epi);
	else if (epi->rdstate == EPI_TX)
		epi->tx_pending = 1;

	if (ep->waiters) {
		task_t *t = ep_claim_waiter(ep);

		if (t) {
			ctx->tasks[ctx->ntasks++] = t;
			woke = 1;
		}
	}
	/* Whoever polls this set hears about it once every lock is dropped.
	 * Read under ep->lock, after the ready list changed: a poller that
	 * registers later asks the ready list after registering. */
	if (__atomic_load_n(&ep->poll_wq.first, __ATOMIC_ACQUIRE)) {
		__atomic_add_fetch(&ep->memref, 1, __ATOMIC_ACQ_REL);
		if (wq_ctx_defer(ctx, ep_poll_wq_wake, ep))
			woke = 1;
		else
			__atomic_sub_fetch(&ep->memref, 1, __ATOMIC_ACQ_REL);
	}
out:
	spin_unlock_irqrestore(&ep->lock, fl);
	return woke;
}

/* epoll_ctl(ADD)'s poll table: register the item on each queue named. */
struct ep_pqueue {
	struct poll_table pt;
	struct epitem *epi;
};

static void ep_ptable_queue_proc(struct poll_table *pt, void *owner,
				 struct wait_queue_head *head)
{
	struct epitem *epi = ((struct ep_pqueue *)pt)->epi;
	struct ep_pwq *pwq;

	(void)owner;
	if (epi->nwait >= EP_NWAIT) {
		epi->nwait_overflow = 1;
		return;
	}
	pwq = &epi->pwq[epi->nwait];
	wq_entry_init_func(&pwq->wait, NULL, ep_poll_callback, epi);
	pwq->head = head;
	if (epi->exclusive)
		wq_add_exclusive(head, &pwq->wait);
	else
		wq_add(head, &pwq->wait);
	epi->nwait++;
}

/* The readiness an item reports, from the object's poll bits. */
static uint32_t ep_item_mask(uint32_t events, short rev)
{
	return (uint32_t)(unsigned short)rev &
	       ((events & ~EP_PRIVATE_BITS) | EPOLLERR | EPOLLHUP) &
	       ~(uint32_t)POLLNVAL;
}

/* ---------------------------------------------------------------------------
 * Looking at the ready list
 * ------------------------------------------------------------------------ */

struct ep_chunk {
	struct epitem *epi[EP_CHUNK];
	uint32_t ev[EP_CHUNK];
	uint32_t rev[EP_CHUNK];
	uint8_t held[EP_CHUNK]; /* 0 gone, 1 reference to drop, 2 item's */
	int n;
};

/* Take up to `max' items off the head of the ready list (ep->lock held). */
static void ep_chunk_pop(struct eventpoll *ep, struct ep_chunk *ch, int max)
{
	ch->n = 0;
	while (ch->n < max && ch->n < EP_CHUNK && ep->rd_head) {
		struct epitem *epi = ep->rd_head;
		int i = ch->n++;

		rd_unlink(ep, epi);
		epi->rdstate = EPI_TX;
		epi->tx_pending = 0;
		epi->pin++;
		ch->epi[i] = epi;
		ch->ev[i] = epi->events;
		ch->rev[i] = 0;
		/* Alive under ep->lock, so its release has not finished and
		 * its memory is there to take a reference on. */
		if (epi->item_held)
			ch->held[i] = poll_obj_closed(epi->file) ? 0 : 2;
		else
			ch->held[i] = poll_obj_tryhold(epi->file) ? 1 : 0;
	}
}

/* Ask each popped item's object (no lock held). */
static void ep_chunk_poll(struct ep_chunk *ch, int depth)
{
	for (int i = 0; i < ch->n; i++) {
		struct epitem *epi = ch->epi[i];

		if (!ch->held[i] || !(ch->ev[i] & ~EP_PRIVATE_BITS))
			continue;
		ch->rev[i] = ep_item_mask(
			ch->ev[i], poll_entry(epi->file, epi->fd,
					      (short)(ch->ev[i] & 0xffff),
					      NULL, depth));
	}
	for (int i = 0; i < ch->n; i++)
		if (ch->held[i] == 1)
			poll_obj_put(ch->epi[i]->file);
}

/* After ep_chunk_finish: free what died while pinned, drop the UNIX sockets
 * found closed.  No lock held. */
static void ep_chunk_cleanup(struct eventpoll *ep, struct epitem **dead,
			     int ndead, struct epitem **reap, int nreap)
{
	uint64_t fl;

	for (int i = 0; i < ndead; i++)
		ep_item_free(dead[i]);
	for (int i = 0; i < nreap; i++) {
		struct epitem *epi = reap[i];
		int claimed, do_free;

		spin_lock_irqsave(&ep_tab_lock, &fl);
		claimed = epi->hashed;
		if (claimed)
			ep_unlink(epi);
		spin_unlock_irqrestore(&ep_tab_lock, fl);
		if (claimed)
			ep_unregister(epi);
		spin_lock_irqsave(&ep->lock, &fl);
		if (claimed) {
			if (epi->rdstate == EPI_READY)
				rd_unlink(ep, epi);
			epi->dead = 1;
		}
		epi->pin--;
		do_free = epi->dead && epi->pin == 0;
		spin_unlock_irqrestore(&ep->lock, fl);
		if (do_free)
			ep_item_free(epi);
	}
}

/* Report the ready events into `uevents' (user memory): at most `maxevents',
 * each ready item once.  Returns the count, or -EFAULT when nothing could be
 * copied. */
static int ep_send_events(struct eventpoll *ep, struct epoll_event *uevents,
			  int maxevents, int depth)
{
	int count = 0, budget;
	uint64_t fl;
	task_t *more = NULL;

	spin_lock_irqsave(&ep->lock, &fl);
	/* The items on the list now, each looked at once: a level-triggered
	 * item that is still ready goes back at the TAIL, behind the ones not
	 * yet reported (that is what makes a full maxevents fair), and must
	 * not be reported twice in one call. */
	budget = ep->nready;
	spin_unlock_irqrestore(&ep->lock, fl);

	while (count < maxevents && budget > 0) {
		struct ep_chunk ch;
		struct epoll_event out[EP_CHUNK];
		struct epitem *dead[EP_CHUNK], *reap[EP_CHUNK];
		int nout = 0, ndead = 0, nreap = 0;
		int want = maxevents - count;

		if (want > budget)
			want = budget;
		spin_lock_irqsave(&ep->lock, &fl);
		ep_chunk_pop(ep, &ch, want);
		spin_unlock_irqrestore(&ep->lock, fl);
		if (ch.n == 0)
			break;
		budget -= ch.n;

		ep_chunk_poll(&ch, depth);

		spin_lock_irqsave(&ep->lock, &fl);
		for (int i = 0; i < ch.n; i++) {
			struct epitem *epi = ch.epi[i];
			int requeue = 0;

			if (epi->dead) {
				if (--epi->pin == 0)
					dead[ndead++] = epi;
				continue;
			}
			if (epi->item_held && ch.held[i] == 0) {
				/* Last descriptor gone: keep the pin, drop
				 * the item below. */
				epi->rdstate = EPI_IDLE;
				reap[nreap++] = epi;
				continue;
			}
			epi->pin--;
			if (ch.rev[i]) {
				out[nout].events = ch.rev[i];
				out[nout].data.u64 = epi->data;
				nout++;
				if (epi->events & EPOLLONESHOT)
					epi->events &= EP_PRIVATE_BITS;
				else if (!(epi->events & EPOLLET))
					requeue = 1;
			}
			if (requeue || epi->tx_pending)
				rd_append(ep, epi);
			else
				epi->rdstate = EPI_IDLE;
		}
		if (ep->rd_head && ep->waiters && !more)
			more = ep_claim_waiter(ep);
		spin_unlock_irqrestore(&ep->lock, fl);

		ep_chunk_cleanup(ep, dead, ndead, reap, nreap);

		if (nout) {
			if (copy_to_user(uevents + count, out,
					 (size_t)nout * sizeof(out[0]))) {
				if (more)
					sched_enqueue_ready(more);
				return count ? count : -EFAULT;
			}
			count += nout;
		}
	}
	/* Events left behind for maxevents: another waiter takes them. */
	if (more)
		sched_enqueue_ready(more);
	return count;
}

/* Is anything on the ready list really ready?  Items found not ready are
 * dropped from it (their next wake puts them back); ready ones stay, in
 * order.  Used by poll() on the set and by sets watching it. */
static int ep_has_ready(struct eventpoll *ep, int depth)
{
	int budget, found = 0;
	uint64_t fl;

	spin_lock_irqsave(&ep->lock, &fl);
	budget = ep->nready;
	spin_unlock_irqrestore(&ep->lock, fl);

	while (!found && budget > 0) {
		struct ep_chunk ch;
		struct epitem *dead[EP_CHUNK], *reap[EP_CHUNK],
			*keep[EP_CHUNK];
		int ndead = 0, nreap = 0, nkeep = 0;

		spin_lock_irqsave(&ep->lock, &fl);
		ep_chunk_pop(ep, &ch, budget);
		spin_unlock_irqrestore(&ep->lock, fl);
		if (ch.n == 0)
			break;
		budget -= ch.n;

		ep_chunk_poll(&ch, depth);

		spin_lock_irqsave(&ep->lock, &fl);
		for (int i = 0; i < ch.n; i++) {
			struct epitem *epi = ch.epi[i];

			if (epi->dead) {
				if (--epi->pin == 0)
					dead[ndead++] = epi;
				continue;
			}
			if (epi->item_held && ch.held[i] == 0) {
				epi->rdstate = EPI_IDLE;
				reap[nreap++] = epi;
				continue;
			}
			epi->pin--;
			if (ch.rev[i]) {
				keep[nkeep++] = epi;
				found = 1;
			} else if (epi->tx_pending) {
				rd_append(ep, epi);
			} else {
				epi->rdstate = EPI_IDLE;
			}
		}
		/* Back at the head, in the order they were taken. */
		for (int i = nkeep - 1; i >= 0; i--)
			rd_push_head(ep, keep[i]);
		spin_unlock_irqrestore(&ep->lock, fl);

		ep_chunk_cleanup(ep, dead, ndead, reap, nreap);
	}
	return found;
}

short ep_eventpoll_poll(int idx, short events, struct poll_table *pt,
			int depth)
{
	struct eventpoll *ep = ep_lookup(idx);

	if (!ep)
		return POLLNVAL;
	poll_wait(pt, MAKE_EPOLL_FD(idx), &ep->poll_wq);
	if (!(events & (POLLIN | POLLRDNORM)) || depth > EP_MAX_NESTS)
		return 0;
	return ep_has_ready(ep, depth + 1) ? (POLLIN | POLLRDNORM) : 0;
}

/* ---------------------------------------------------------------------------
 * Removal: by the object's release, by the set's teardown
 * ------------------------------------------------------------------------ */

void eventpoll_release(void *file)
{
	struct epitem *claimed = NULL, *epi, *next;
	unsigned b = ep_hash_fn(file);
	uint64_t fl;

	/* Nothing watches anything in this bucket.  An item for this object
	 * was inserted while its adder held a reference, which this release
	 * comes after, so it would be visible here. */
	if (!__atomic_load_n(&ep_hash[b], __ATOMIC_ACQUIRE))
		return;

	spin_lock_irqsave(&ep_tab_lock, &fl);
	for (epi = ep_hash[b]; epi; epi = next) {
		next = epi->h_next;
		if (epi->file != file)
			continue;
		ep_unlink(epi);
		/* The set stays while this works on it. */
		__atomic_add_fetch(&epi->ep->memref, 1, __ATOMIC_ACQ_REL);
		epi->h_next = claimed; /* reused as the claim list */
		claimed = epi;
	}
	spin_unlock_irqrestore(&ep_tab_lock, fl);

	for (epi = claimed; epi; epi = next) {
		struct eventpoll *ep = epi->ep;

		next = epi->h_next;
		ep_remove_claimed(epi);
		ep_memput(ep);
	}
}

/* A wait queue is about to be freed while its owner lives on (a task's
 * signalfd queue, freed with the task while a set elsewhere still watches a
 * signalfd that registered on it): every epoll item on it leaves it now, and
 * hears nothing more from it. */
void eventpoll_wq_free(struct wait_queue_head *h)
{
	struct wait_queue_entry *e, *next;
	uint64_t fl, hfl;

	if (!h)
		return;
	spin_lock_irqsave(&ep_pollfree_lock, &fl);
	spin_lock_irqsave(&h->lock, &hfl);
	for (e = h->first; e; e = next) {
		next = e->next;
		if (e->func != ep_poll_callback)
			continue;
		wq_remove_locked(h, e);
		/* The item's queue pointer: e is the first member of its
		 * struct ep_pwq. */
		((struct ep_pwq *)e)->head = NULL;
	}
	spin_unlock_irqrestore(&h->lock, hfl);
	spin_unlock_irqrestore(&ep_pollfree_lock, fl);
}

/* The last reference to the set is gone. */
static void ep_teardown(struct eventpoll *ep)
{
	uint64_t fl;

	/* Sets watching this one first, while its index still names it. */
	eventpoll_release(MAKE_EPOLL_FD(ep->idx));

	/* Then its own items, a batch at a time.  An item whose object is
	 * being released right now (no reference to be had) is left to that
	 * release, which holds this set (memref) for as long as it needs. */
	for (;;) {
		struct epitem *batch[EP_CHUNK];
		uint8_t held[EP_CHUNK];
		int n = 0;

		spin_lock_irqsave(&ep_tab_lock, &fl);
		for (struct epitem *epi = ep->items; epi && n < EP_CHUNK;
		     epi = epi->e_next) {
			int h;

			if (epi->item_held)
				h = 2;
			else if (poll_obj_tryhold(epi->file))
				h = 1;
			else
				continue;
			held[n] = (uint8_t)h;
			batch[n++] = epi;
		}
		for (int i = 0; i < n; i++)
			ep_unlink(batch[i]);
		spin_unlock_irqrestore(&ep_tab_lock, fl);
		if (n == 0)
			break;
		for (int i = 0; i < n; i++) {
			void *file = batch[i]->file;

			ep_remove_claimed(batch[i]);
			if (held[i] == 1)
				poll_obj_put(file);
		}
	}

	spin_lock_irqsave(&ep_idx_lock, &fl);
	if (ep_table[ep->idx] == ep)
		ep_table[ep->idx] = NULL;
	spin_unlock_irqrestore(&ep_idx_lock, fl);
	ep_memput(ep);
}

/* ---------------------------------------------------------------------------
 * epoll_ctl
 * ------------------------------------------------------------------------ */

/* The deepest chain of sets below `e' (0: it watches none), or -1 for a loop
 * back to ep_lc_target or a chain too deep.  ep_tab_lock held. */
static int ep_depth_down(struct eventpoll *e, int level)
{
	int max = 0;

	if (e == ep_lc_target || level > EP_MAX_NESTS)
		return -1;
	if (e->lc_gen_down == ep_lc_gen)
		return e->lc_down;
	for (struct epitem *i = e->tgt_items; i; i = i->t_next) {
		int d = ep_depth_down(i->tgt, level + 1);

		if (d < 0)
			return -1;
		if (d + 1 > max)
			max = d + 1;
	}
	e->lc_gen_down = ep_lc_gen;
	e->lc_down = max;
	return max;
}

/* The deepest chain of sets above `e' (watching it), or -1 when too deep. */
static int ep_depth_up(struct eventpoll *e, int level)
{
	void *marker = MAKE_EPOLL_FD(e->idx);
	int max = 0;

	if (level > EP_MAX_NESTS)
		return -1;
	if (e->lc_gen_up == ep_lc_gen)
		return e->lc_up;
	for (struct epitem *i = ep_hash[ep_hash_fn(marker)]; i;
	     i = i->h_next) {
		int d;

		if (i->file != marker)
			continue;
		d = ep_depth_up(i->ep, level + 1);
		if (d < 0)
			return -1;
		if (d + 1 > max)
			max = d + 1;
	}
	e->lc_gen_up = ep_lc_gen;
	e->lc_up = max;
	return max;
}

/* May `ep' watch `tgt'?  No loop, and the whole chain through the new link
 * at most EP_MAX_NESTS + 1 sets long.  ep_tab_lock held. */
static int ep_nest_ok(struct eventpoll *ep, struct eventpoll *tgt)
{
	int down, up;

	ep_lc_gen++;
	ep_lc_target = ep;
	down = ep_depth_down(tgt, 0);
	ep_lc_target = NULL;
	if (down < 0)
		return 0;
	up = ep_depth_up(ep, 0);
	if (up < 0)
		return 0;
	return up + down + 2 <= EP_MAX_NESTS + 1;
}

static void ep_wake_after_insert(struct eventpoll *ep, struct epitem *epi,
				 uint32_t revents)
{
	uint64_t fl;
	task_t *t = NULL;
	int poll_waiters = 0;

	if (!revents)
		return;
	spin_lock_irqsave(&ep->lock, &fl);
	if (!epi->dead && epi->rdstate == EPI_IDLE &&
	    (epi->events & ~EP_PRIVATE_BITS)) {
		rd_append(ep, epi);
		t = ep_claim_waiter(ep);
		poll_waiters = __atomic_load_n(&ep->poll_wq.first,
					       __ATOMIC_ACQUIRE) != NULL;
	}
	spin_unlock_irqrestore(&ep->lock, fl);
	if (t)
		sched_enqueue_ready(t);
	if (poll_waiters)
		wq_wake_poll(&ep->poll_wq, POLLIN | POLLRDNORM);
}

/* `file' is held by the caller (fdget), ep->mtx held. */
static int ep_insert(struct eventpoll *ep, const struct epoll_event *ev,
		     void *file, int fd)
{
	struct epitem *epi;
	struct ep_pqueue epq;
	uint64_t fl;
	short rev;
	int err = 0;

	if (__atomic_add_fetch(&ep_watches, 1, __ATOMIC_RELAXED) >
	    ep_watch_limit()) {
		__atomic_sub_fetch(&ep_watches, 1, __ATOMIC_RELAXED);
		return -ENOSPC;
	}
	epi = kalloc(sizeof(*epi));
	if (!epi) {
		__atomic_sub_fetch(&ep_watches, 1, __ATOMIC_RELAXED);
		return -ENOMEM;
	}
	mm_memset(epi, 0, sizeof(*epi));
	epi->ep = ep;
	epi->file = file;
	epi->fd = fd;
	epi->events = ev->events;
	epi->data = ev->data.u64;
	epi->rdstate = EPI_IDLE;
	epi->exclusive = (ev->events & EPOLLEXCLUSIVE) != 0;
	if (IS_EPOLL_FD(file))
		epi->tgt = ep_lookup(EPOLL_FD_IDX(file));
	switch (poll_obj_item_hold(file)) {
	case 0:
		__atomic_sub_fetch(&ep_watches, 1, __ATOMIC_RELAXED);
		kfree(epi);
		return -EBADF;
	case 2:
		epi->item_held = 1; /* its memory, until the item goes */
		break;
	default:
		break;
	}

	spin_lock_irqsave(&ep_tab_lock, &fl);
	if (ep_find(ep, file, fd))
		err = -EEXIST;
	else if (epi->tgt && !ep_nest_ok(ep, epi->tgt))
		err = -ELOOP;
	else
		ep_link(epi);
	spin_unlock_irqrestore(&ep_tab_lock, fl);
	if (err) {
		ep_item_free(epi);
		return err;
	}

	/* On the object's queues, and its state now.  From the first queue
	 * on, a wake may put the item on the ready list. */
	epq.pt.qproc = ep_ptable_queue_proc;
	epq.pt.nopoll = 0;
	epq.pt.key = ev->events;
	epq.pt.ents = NULL;
	epq.pt.n = epq.pt.cap = 0;
	epq.pt.unregistered = 0;
	epq.pt.triggered = 0;
	epq.epi = epi;
	rev = poll_entry(file, fd, (short)(ev->events & 0xffff), &epq.pt, 0);

	if (epq.pt.nopoll || epi->nwait_overflow) {
		/* A plain file has nothing to wait on (EPERM, as everywhere);
		 * an object with more queues than an item can hold cannot be
		 * watched reliably. */
		WARN_ON_ONCE(epi->nwait_overflow);
		spin_lock_irqsave(&ep_tab_lock, &fl);
		if (epi->hashed)
			ep_unlink(epi);
		spin_unlock_irqrestore(&ep_tab_lock, fl);
		ep_remove_claimed(epi);
		return epq.pt.nopoll ? -EPERM : -ENOMEM;
	}

	ep_wake_after_insert(ep, epi, ep_item_mask(ev->events, rev));
	return 0;
}

/* ep->mtx held, `file' held. */
static int ep_modify(struct eventpoll *ep, struct epitem *epi,
		     const struct epoll_event *ev)
{
	uint64_t fl;
	short rev;

	spin_lock_irqsave(&ep->lock, &fl);
	epi->events = ev->events;
	epi->data = ev->data.u64;
	spin_unlock_irqrestore(&ep->lock, fl);
	/* The new events are published (the unlock) before the object is
	 * asked: a wake from here on sees them, and one before is caught by
	 * the question. */
	rev = poll_entry(epi->file, epi->fd, (short)(ev->events & 0xffff),
			 NULL, 0);
	ep_wake_after_insert(ep, epi, ep_item_mask(ev->events, rev));
	return 0;
}

/* The set a descriptor names, with a reference (released with fdput(held)).
 * EBADF: not open; EINVAL: not an epoll set. */
static int ep_from_fd(uint64_t fd, struct eventpoll **out, void **held)
{
	task_t *cur = sched_current();
	void *entry;

	*held = NULL;
	if (!cur || fd >= TASK_MAX_FDS)
		return -EBADF;
	entry = fdget(cur, (int)fd);
	if (!entry)
		return -EBADF;
	if (!IS_EPOLL_FD(entry)) {
		fdput(entry);
		return -EINVAL;
	}
	*out = ep_lookup(EPOLL_FD_IDX(entry));
	if (!*out) {
		fdput(entry);
		return -EBADF;
	}
	*held = entry;
	return 0;
}

int64_t sys_epoll_ctl(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	task_t *cur = sched_current();
	struct eventpoll *ep;
	struct epoll_event kev;
	void *eph, *tf;
	int op = (int)a2, fd = (int)a3;
	int64_t ret;

	ret = ep_from_fd(a1, &ep, &eph);
	if (ret < 0)
		return ret;
	if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_DEL &&
	    op != EPOLL_CTL_MOD) {
		fdput(eph);
		return -EINVAL;
	}
	if (op != EPOLL_CTL_DEL) {
		if (!a4 || !validate_user_ptr(a4, sizeof(kev)) ||
		    copy_from_user(&kev, (void *)a4, sizeof(kev))) {
			fdput(eph);
			return -EFAULT;
		}
		/* Errors and hangups are always reported. */
		kev.events |= EPOLLERR | EPOLLHUP;
		/* Keeping the machine awake is a privilege. */
		if ((kev.events & EPOLLWAKEUP) && !capable())
			kev.events &= ~EPOLLWAKEUP;
	}

	tf = (fd >= 0 && fd < TASK_MAX_FDS) ? fdget(cur, fd) : NULL;
	if (!tf) {
		/* The console in an empty 0/1/2 slot is the dup marker 1. */
		if (fd >= 0 && fd < 3 && task_fd_is_console(cur, (uint64_t)fd)) {
			tf = (void *)1;
		} else {
			fdput(eph);
			return -EBADF;
		}
	}

	/* A set cannot watch itself. */
	if (tf == eph) {
		ret = -EINVAL;
		goto out;
	}
	if (op == EPOLL_CTL_ADD && (kev.events & EPOLLEXCLUSIVE)) {
		/* Exclusive wakeups: only with these events, never on a set. */
		if ((kev.events & ~EP_EXCLUSIVE_OK_BITS) || IS_EPOLL_FD(tf)) {
			ret = -EINVAL;
			goto out;
		}
	}

	mutex_lock(&ep->mtx);
	if (op == EPOLL_CTL_ADD) {
		ret = ep_insert(ep, &kev, tf, fd);
	} else {
		struct epitem *epi;
		uint64_t fl, efl;

		spin_lock_irqsave(&ep_tab_lock, &fl);
		epi = ep_find(ep, tf, fd);
		if (epi && op == EPOLL_CTL_DEL) {
			ep_unlink(epi);
		} else if (epi) {
			/* Pinned: a closed UNIX socket's item can be dropped
			 * by an epoll_wait meanwhile. */
			spin_lock_irqsave(&ep->lock, &efl);
			epi->pin++;
			spin_unlock_irqrestore(&ep->lock, efl);
		}
		spin_unlock_irqrestore(&ep_tab_lock, fl);
		if (!epi) {
			ret = -ENOENT;
		} else if (op == EPOLL_CTL_DEL) {
			ep_remove_claimed(epi);
			ret = 0;
		} else {
			int do_free;

			if ((kev.events & EPOLLEXCLUSIVE) || epi->exclusive)
				/* An exclusive item cannot be modified, nor
				 * an item made exclusive after the fact. */
				ret = -EINVAL;
			else
				ret = ep_modify(ep, epi, &kev);
			spin_lock_irqsave(&ep->lock, &efl);
			do_free = --epi->pin == 0 && epi->dead;
			spin_unlock_irqrestore(&ep->lock, efl);
			if (do_free)
				ep_item_free(epi);
		}
	}
	mutex_unlock(&ep->mtx);
out:
	if ((uintptr_t)tf > 3)
		fdput(tf);
	fdput(eph);
	return ret;
}

/* ---------------------------------------------------------------------------
 * epoll_wait
 * ------------------------------------------------------------------------ */

static int ep_poll(struct eventpoll *ep, struct epoll_event *uevents,
		   int maxevents, uint64_t timeout_ns)
{
	task_t *cur = sched_current();
	int have_deadline = timeout_ns != 0 && timeout_ns != (uint64_t)-1;
	uint64_t deadline_ns = 0, deadline_ticks = 0;

	if (have_deadline) {
		uint64_t now = hrtimer_now_ns();

		deadline_ns = timeout_ns > ~0ULL - now ? ~0ULL :
							 now + timeout_ns;
		/* +1: the tick in progress; a timeout may be late, never
		 * early.  The nanosecond deadline is the real one. */
		deadline_ticks =
			timer_ticks() + timer_ns_to_ticks(timeout_ns) + 1;
	}

	for (;;) {
		struct ep_waiter w;
		uint64_t fl;
		int n = ep_send_events(ep, uevents, maxevents, 0);

		if (n != 0 || timeout_ns == 0)
			return n;
		if (have_deadline && hrtimer_now_ns() >= deadline_ns)
			return 0;
		if (!cur)
			return 0;
		/* A deliverable signal ends the wait; masked ones do not. */
		if (signal_pending(cur))
			return -EINTR;

		w.task = cur;
		spin_lock_irqsave(&ep->lock, &fl);
		if (ep->rd_head) {
			spin_unlock_irqrestore(&ep->lock, fl);
			continue;
		}
		/* Queued and parked under ep->lock: a callback, which needs
		 * the lock, either ran before (the ready list above was not
		 * empty) or finds this task parked and wakes it. */
		w.next = ep->waiters;
		if (w.next)
			w.next->pprev = &w.next;
		w.pprev = &ep->waiters;
		ep->waiters = &w;
		cur->wait_channel = (void *)ep;
		if (have_deadline) {
			uint64_t now_t = timer_ticks();

			cur->wakeup_tick = deadline_ticks > now_t ?
						   deadline_ticks :
						   now_t + 1;
		} else {
			cur->wakeup_tick = 0;
		}
		task_set_blocked(cur);
		spin_unlock_irqrestore(&ep->lock, fl);

		if (have_deadline)
			hrtimer_wait_arm(cur, deadline_ns);
		sched_schedule();
		if (have_deadline)
			hrtimer_wait_disarm(cur);

		spin_lock_irqsave(&ep->lock, &fl);
		*w.pprev = w.next;
		if (w.next)
			w.next->pprev = w.pprev;
		spin_unlock_irqrestore(&ep->lock, fl);
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		if (cur->state != TASK_RUNNING)
			cur->state = TASK_RUNNING;
	}
}

/* The common part of epoll_wait / epoll_pwait / epoll_pwait2. */
static int64_t ep_wait_common(uint64_t epfd, uint64_t uevents,
			      uint64_t maxevents_arg, uint64_t timeout_ns,
			      uint64_t sigmask)
{
	struct eventpoll *ep;
	void *eph;
	int maxevents = (int)maxevents_arg;
	int64_t ret;

	/* maxevents and the buffer first, as everywhere: EINVAL/EFAULT even
	 * for a descriptor that is not a set. */
	if (maxevents <= 0 || maxevents > EP_MAX_EVENTS)
		return -EINVAL;
	if (!validate_user_ptr(uevents,
			       (size_t)maxevents * sizeof(struct epoll_event)))
		return -EFAULT;
	ret = ep_from_fd(epfd, &ep, &eph);
	if (ret < 0)
		return ret;
	if (sigmask) {
		kernel_sigset_t saved;
		int r = poll_sigmask_install(sigmask, &saved);

		if (r < 0) {
			fdput(eph);
			return r;
		}
		/* Restored after signal delivery (see sys_ppoll). */
	}
	ret = ep_poll(ep, (struct epoll_event *)uevents, maxevents,
		      timeout_ns);
	fdput(eph);
	return ret;
}

static uint64_t ep_timeout_ms(int64_t ms)
{
	if (ms < 0)
		return (uint64_t)-1;
	return (uint64_t)ms * 1000000ULL;
}

__attribute__((noinline)) int64_t
sys_epoll_wait(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	return ep_wait_common(a1, a2, a3, ep_timeout_ms((int)(int64_t)a4), 0);
}

/* (epfd, events, maxevents, timeout ms, sigmask) */
int64_t sys_epoll_pwait(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
			uint64_t a5)
{
	return ep_wait_common(a1, a2, a3, ep_timeout_ms((int)(int64_t)a4),
			      a5);
}

/* (epfd, events, maxevents, const struct timespec *timeout, sigmask,
 * sigsetsize): a nanosecond timeout, NULL to wait for ever. */
int64_t sys_epoll_pwait2(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
			 uint64_t a5, uint64_t a6)
{
	uint64_t timeout_ns = (uint64_t)-1;

	if (a5 && a6 != sizeof(kernel_sigset_t))
		return -EINVAL;
	if (a4) {
		int64_t ts[2];

		if (!validate_user_ptr(a4, sizeof(ts)) ||
		    copy_from_user(ts, (void *)a4, sizeof(ts)))
			return -EFAULT;
		if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL)
			return -EINVAL;
		timeout_ns = (uint64_t)ts[0] > (~0ULL - 1) / 1000000000ULL ?
				     (uint64_t)-1 - 1 :
				     (uint64_t)ts[0] * 1000000000ULL +
					     (uint64_t)ts[1];
	}
	return ep_wait_common(a1, a2, a3, timeout_ns, a5);
}

/* ---------------------------------------------------------------------------
 * epoll_create
 * ------------------------------------------------------------------------ */

static int64_t ep_create_fd(int flags)
{
	task_t *cur = sched_current();
	int idx, fd;

	if (!cur)
		return -EFAULT;
	idx = ep_create();
	if (idx < 0)
		return idx;
	/* fd_install_from claims the slot under the descriptor-table lock
	 * and clears its stale flag byte. */
	fd = fd_install_from(cur, MAKE_EPOLL_FD(idx), 3);
	if (fd < 0) {
		epoll_put(idx);
		return fd;
	}
	/* EPOLL_CLOEXEC has to be RECORDED, not just accepted: a set left
	 * open across exec() hands an unrelated program a handle on it. */
	if (flags & EPOLL_CLOEXEC)
		task_set_fd_flags(cur, (unsigned)fd, FD_CLOEXEC);
	return fd;
}

int64_t sys_epoll_create(uint64_t a1)
{
	/* The size is a hint nobody uses any more, but it must be positive. */
	if ((int)a1 <= 0)
		return -EINVAL;
	return ep_create_fd(0);
}

int64_t sys_epoll_create1(uint64_t a1)
{
	if ((int)a1 & ~EPOLL_CLOEXEC)
		return -EINVAL;
	return ep_create_fd((int)a1);
}
