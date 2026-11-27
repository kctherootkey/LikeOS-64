// LikeOS evdev - event devices of the input core
//
// One node per registered input device (struct evdev), one client per open
// descriptor (struct evdev_client).  The input core hands every complete
// packet to evdev_pass_values(), which copies it into the queue of every
// client -- or only the grabbing client's while a grab is held -- stamped
// with the client's chosen clock.
//
// Client queues.  A power-of-two ring of input_event records with three
// indices: head (where the next event goes), tail (the next to read) and
// packet_head (the end of the last complete packet).  Readers only ever get
// whole packets, up to packet_head.  When the ring fills up, everything
// unread is dropped: the queue is left holding a SYN_DROPPED record and the
// newest event, and the client is expected to re-read the device state
// (EVIOCGKEY, EVIOCGABS, EVIOCGMTSLOTS ...) at the next SYN_REPORT.  Reading
// a state bitmap (EVIOCGKEY/LED/SND/SW) also removes the queued events of
// that type, so the snapshot and the queue never disagree.  The ring size
// follows the device's events-per-packet estimate (8 packets, at least 64
// events, rounded up to a power of two); the built-in keyboard and mouse
// keep the 256 events they always had.
//
// A grab makes the device the grabbing client's alone: the other clients
// and the in-kernel handles (input.c) get nothing until it is released.
//
// Locking:
//   g_evdev_table_lock   the node table (minor -> node)
//   dev->event_lock      (input core) packets reach evdev_pass_values()
//                        under it; the node link dev->evdev changes under it
//   evdev->client_lock   client list, every client queue, the grab
//   evdev->mutex         ioctl / write / open / close against disconnect;
//                        `exist' and `open' change under it
// Order: evdev->mutex -> dev->mutex; dev->event_lock -> evdev->client_lock.
//
// Producers run in interrupt context (spin_lock_irqsave, no prints, no
// sleeping); readers park with the established wait_channel/BLOCKED
// protocol on the node's wait channel.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 1999-2002 Vojtech Pavlik

#include <kernel/ke/waitq.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/dev/input/input.h>
#include <kernel/dev/input/input-mt.h>
#include <kernel/dev/input/keyboard.h>
#include <kernel/uapi/input.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/uaccess_range.h>

// Wake the pollers registered on one queue (kernel/net/poll.c); `key' names
// the POLL* bits that changed.
extern void poll_notify_wq_key(struct wait_queue_head *h, unsigned key);

/* The standard poll bits (the full set lives in kernel/net/net.h). */
#define EVDEV_POLLIN 0x0001
#define EVDEV_POLLOUT 0x0004
#define EVDEV_POLLERR 0x0008
#define EVDEV_POLLHUP 0x0010
#define EVDEV_POLLRDNORM 0x0040
#define EVDEV_POLLWRNORM 0x0100

#define EVDEV_MIN_BUFFER_SIZE 64u // events
#define EVDEV_BUF_PACKETS 8u // packets a queue holds at the estimate
/* Allocation bound per descriptor (192 KB of events): only devices with
 * more than about a hundred slots would ask for more. */
#define EVDEV_MAX_BUFFER_SIZE 8192u
#define EVDEV_BUILTIN_BUFFER 256u // keyboard and mouse, as always
#define EVDEV_READ_BURST 32 // events copied to user space per pass
#define EVDEV_BUILTIN_VALS 16 // packet buffer of a built-in device
/* Bytes one write() injects at most (170 events): the node's mutex is not
 * held for long. */
#define EVDEV_WRITE_MAX 4096
#define EVDEV_MT_CHUNK 64 // EVIOCGMTSLOTS values copied per pass

/* Clocks a client can have its events stamped with (EVIOCSCLOCKID). */
enum { EVDEV_CLK_REAL = 0, EVDEV_CLK_MONO, EVDEV_CLK_BOOT };

struct evdev_client {
	struct evdev *evdev;
	struct evdev_client *next; // on evdev->clients
	struct input_event *buffer;
	unsigned int bufsize; // power of two
	unsigned int head, tail, packet_head;
	int clk_type;
	int revoked; // EVIOCREVOKE: dead descriptor from now on
	/* EVIOCSMASK: per-type code masks, [0] masks the types themselves.
	 * NULL lets everything of that type through. */
	unsigned long *evmasks[EV_CNT];
};

struct evdev {
	int minor;
	int is_static; // a built-in node, never freed
	volatile int exist; // registered; cleared at disconnect (mutex)
	int open; // clients that opened the device (mutex)
	refcount_t refcount; // the node table's + one per client
	struct input_dev *dev;
	spinlock_t client_lock;
	struct evdev_client *clients;
	struct evdev_client *grab;
	struct mutex mutex;
	/* Blocked readers park on this channel; pollers on poll_wq.  Both
	 * belong to the node, which every client keeps alive, so a wake from
	 * the event path can never reach freed memory. */
	int wait_key;
	struct wait_queue_head poll_wq;
};

static spinlock_t g_evdev_table_lock = SPINLOCK_INIT("evdev_table");
static struct evdev *g_evdev_table[EVDEV_MINORS];

// ---------------------------------------------------------------------------
// The built-in devices
// ---------------------------------------------------------------------------

static struct input_dev g_kbd_dev;
static struct input_dev g_mouse_dev;
static struct input_value g_kbd_vals[EVDEV_BUILTIN_VALS];
static struct input_value g_mouse_vals[EVDEV_BUILTIN_VALS];
static struct evdev g_builtin_nodes[EVDEV_BUILTIN_UNITS];
static spinlock_t g_builtin_lock = SPINLOCK_INIT("evdev_builtin");
static int g_builtin_done;
/* Screen size the relative motion of another source is scaled by while the
 * mouse axes are absolute (the mouse's event lock). */
static int g_abs_screen_w, g_abs_screen_h;

/* What a client writes to the keyboard node, for the keyboard itself: the
 * lock LEDs go to the keyboard driver, which drives the PS/2 and USB
 * keyboards' LEDs from them while the console does not have the keyboard.
 * The core has already recorded the new state in dev->led (event lock). */
static int evdev_kbd_event(struct input_dev *dev, unsigned int type,
			   unsigned int code, int value)
{
	(void)code;
	(void)value;
	if (type == EV_LED)
		keyboard_client_leds(input_test_bit(LED_NUML, dev->led),
				     input_test_bit(LED_CAPSL, dev->led),
				     input_test_bit(LED_SCROLLL, dev->led));
	return 0;
}

static void evdev_setup_keyboard(struct input_dev *kbd)
{
	unsigned int i;

	kbd->name = "LikeOS Keyboard";
	/* Physical path in the conventional serio form; a display server uses
	 * it to tell devices apart and to match configuration rules. */
	kbd->phys = "isa0060/serio0/input0";
	kbd->devpath = "/devices/platform/i8042/serio0";
	kbd->id.bustype = BUS_I8042;
	kbd->id.vendor = 0x0001;
	kbd->id.product = 0x0001;
	kbd->id.version = 0x0100;
	/* The keyboard reports the three standard lock LEDs.  A display
	 * server writes their state, and the keyboards show it
	 * (evdev_kbd_event). */
	kbd->event = evdev_kbd_event;
	input_set_bit(EV_LED, kbd->evbit);
	input_set_bit(LED_NUML, kbd->ledbit);
	input_set_bit(LED_CAPSL, kbd->ledbit);
	input_set_bit(LED_SCROLLL, kbd->ledbit);
	input_set_bit(EV_SYN, kbd->evbit);
	input_set_bit(EV_KEY, kbd->evbit);
	// Base set-1 make codes map 1:1 onto key codes 1..88, plus the
	// extended navigation/modifier block.
	for (i = KEY_ESC; i <= KEY_F12; i++)
		input_set_bit(i, kbd->keybit);
	for (i = KEY_KPENTER; i <= KEY_DELETE; i++)
		input_set_bit(i, kbd->keybit);
	input_set_bit(KEY_KPEQUAL, kbd->keybit);
	input_set_bit(KEY_PAUSE, kbd->keybit);
	input_set_bit(KEY_RO, kbd->keybit);
	input_set_bit(KEY_HENKAN, kbd->keybit);
	input_set_bit(KEY_KATAKANAHIRAGANA, kbd->keybit);
	input_set_bit(KEY_MUHENKAN, kbd->keybit);
	input_set_bit(KEY_KPCOMMA, kbd->keybit);
	input_set_bit(KEY_HANGEUL, kbd->keybit);
	input_set_bit(KEY_HANJA, kbd->keybit);
	input_set_bit(KEY_YEN, kbd->keybit);
	input_set_bit(KEY_LEFTMETA, kbd->keybit);
	input_set_bit(KEY_RIGHTMETA, kbd->keybit);
	input_set_bit(KEY_COMPOSE, kbd->keybit);
	kbd->vals = g_kbd_vals;
	kbd->max_vals = EVDEV_BUILTIN_VALS;
}

static void evdev_setup_mouse(struct input_dev *mou)
{
	mou->name = "LikeOS Mouse";
	mou->phys = "isa0060/serio1/input0";
	mou->devpath = "/devices/platform/i8042/serio1";
	/* INPUT_PROP_POINTER: motion is relative and drives a cursor, rather
	 * than mapping onto screen coordinates the way a touchscreen does. */
	input_set_bit(INPUT_PROP_POINTER, mou->propbit);
	mou->id.bustype = BUS_I8042;
	mou->id.vendor = 0x0001;
	mou->id.product = 0x0002;
	mou->id.version = 0x0100;
	input_set_bit(EV_SYN, mou->evbit);
	input_set_bit(EV_KEY, mou->evbit);
	input_set_bit(EV_REL, mou->evbit);
	input_set_bit(BTN_LEFT, mou->keybit);
	input_set_bit(BTN_RIGHT, mou->keybit);
	input_set_bit(BTN_MIDDLE, mou->keybit);
	input_set_bit(BTN_SIDE, mou->keybit);
	input_set_bit(BTN_EXTRA, mou->keybit);
	input_set_bit(REL_X, mou->relbit);
	input_set_bit(REL_Y, mou->relbit);
	input_set_bit(REL_WHEEL, mou->relbit);
	input_set_bit(REL_HWHEEL, mou->relbit);
	mou->vals = g_mouse_vals;
	mou->max_vals = EVDEV_BUILTIN_VALS;
}

void evdev_builtin_init(void)
{
	uint64_t f;

	if (__atomic_load_n(&g_builtin_done, __ATOMIC_ACQUIRE))
		return;
	/* Any context, including the interrupt that reports the first key:
	 * the registration of a built-in device takes spinlocks only. */
	spin_lock_irqsave(&g_builtin_lock, &f);
	if (!g_builtin_done) {
		evdev_setup_keyboard(&g_kbd_dev);
		evdev_setup_mouse(&g_mouse_dev);
		WARN_ON(input_register_builtin_device(
			&g_kbd_dev, &g_builtin_nodes[EVDEV_UNIT_KEYBOARD],
			EVDEV_UNIT_KEYBOARD));
		WARN_ON(input_register_builtin_device(
			&g_mouse_dev, &g_builtin_nodes[EVDEV_UNIT_MOUSE],
			EVDEV_UNIT_MOUSE));
		__atomic_store_n(&g_builtin_done, 1, __ATOMIC_RELEASE);
	}
	spin_unlock_irqrestore(&g_builtin_lock, f);
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

static void evdev_node_init(struct evdev *e, struct input_dev *dev, int minor,
			    int is_static)
{
	e->minor = minor;
	e->is_static = is_static;
	e->exist = 1;
	e->open = 0;
	refcount_set(&e->refcount, 1); // the node table's
	e->dev = dev;
	spinlock_init(&e->client_lock, "evdev_clients");
	e->clients = NULL;
	e->grab = NULL;
	mutex_init(&e->mutex, "evdev");
	wq_head_init(&e->poll_wq, "evdev-poll");
}

static void evdev_put(struct evdev *e)
{
	if (!refcount_dec_and_test(&e->refcount))
		return;
	if (WARN_ON(e->is_static))
		return;
	WARN_ON(e->clients != NULL);
	input_put_device(e->dev);
	kfree(e);
}

/* Link the node into its device: from now on packets reach it. */
static void evdev_link(struct input_dev *dev, struct evdev *e)
{
	uint64_t f;

	spin_lock_irqsave(&dev->event_lock, &f);
	dev->evdev = e;
	spin_unlock_irqrestore(&dev->event_lock, f);
}

int evdev_connect_builtin(struct input_dev *dev, struct evdev *node,
			  int minor)
{
	uint64_t f;
	int rc = 0;

	if (!dev || !node || minor < 0 || minor >= EVDEV_BUILTIN_UNITS)
		return -EINVAL;
	evdev_node_init(node, dev, minor, 1);
	spin_lock_irqsave(&g_evdev_table_lock, &f);
	if (g_evdev_table[minor])
		rc = -EBUSY;
	else
		g_evdev_table[minor] = node;
	spin_unlock_irqrestore(&g_evdev_table_lock, f);
	if (!rc)
		evdev_link(dev, node);
	return rc;
}

int evdev_connect(struct input_dev *dev)
{
	struct evdev *e;
	uint64_t f;
	int minor;

	might_sleep();
	e = (struct evdev *)kcalloc(1, sizeof(*e));
	if (!e)
		return -ENOMEM;
	spin_lock_irqsave(&g_evdev_table_lock, &f);
	/* Lowest free number above the built-in ones, so a device that
	 * comes back after being unplugged gets its old name again. */
	for (minor = EVDEV_BUILTIN_UNITS; minor < EVDEV_MINORS; minor++)
		if (!g_evdev_table[minor])
			break;
	if (minor == EVDEV_MINORS) {
		spin_unlock_irqrestore(&g_evdev_table_lock, f);
		kfree(e);
		return -ENFILE;
	}
	input_get_device(dev); // the node keeps its device
	evdev_node_init(e, dev, minor, 0);
	g_evdev_table[minor] = e;
	spin_unlock_irqrestore(&g_evdev_table_lock, f);
	evdev_link(dev, e);
	return 0;
}

int evdev_minor_of(const struct evdev *evdev)
{
	return evdev ? evdev->minor : -1;
}

static void evdev_wake(struct evdev *e, unsigned int key)
{
	sched_wake_channel((void *)&e->wait_key);
	poll_notify_wq_key(&e->poll_wq, key);
}

void evdev_disconnect(struct input_dev *dev)
{
	struct evdev *e;
	uint64_t f;

	might_sleep();
	spin_lock_irqsave(&dev->event_lock, &f);
	e = dev->evdev;
	spin_unlock_irqrestore(&dev->event_lock, f);
	if (!e || WARN_ON(e->is_static))
		return;

	/* No ioctl, write or open is in flight past this point, and none
	 * starts: they all check `exist' under the mutex. */
	mutex_lock(&e->mutex);
	__atomic_store_n(&e->exist, 0, __ATOMIC_SEQ_CST);
	mutex_unlock(&e->mutex);

	spin_lock_irqsave(&g_evdev_table_lock, &f);
	if (g_evdev_table[e->minor] == e)
		g_evdev_table[e->minor] = NULL;
	spin_unlock_irqrestore(&g_evdev_table_lock, f);

	/* No more packets: after this the event path never sees the node. */
	evdev_link(dev, NULL);

	/* Hang up: readers and pollers wake to ENODEV / POLLHUP|POLLERR. */
	spin_lock_irqsave(&e->client_lock, &f);
	e->grab = NULL;
	spin_unlock_irqrestore(&e->client_lock, f);
	evdev_wake(e, EVDEV_POLLHUP | EVDEV_POLLERR);

	/* The device is closed for its users now; their descriptors stay
	 * valid but dead until they are closed. */
	mutex_lock(&e->mutex);
	if (e->open) {
		e->open = 0;
		input_close_device(dev);
	}
	mutex_unlock(&e->mutex);

	evdev_put(e); // the node table's reference
}

int evdev_unit_exists(int unit)
{
	uint64_t f;
	int r;

	if (unit < 0 || unit >= EVDEV_MINORS)
		return 0;
	evdev_builtin_init();
	spin_lock_irqsave(&g_evdev_table_lock, &f);
	r = g_evdev_table[unit] &&
	    __atomic_load_n(&g_evdev_table[unit]->exist, __ATOMIC_ACQUIRE);
	spin_unlock_irqrestore(&g_evdev_table_lock, f);
	return r;
}

int evdev_next_unit(int from)
{
	for (int u = from < 0 ? 0 : from; u < EVDEV_MINORS; u++)
		if (evdev_unit_exists(u))
			return u;
	return -1;
}

// ---------------------------------------------------------------------------
// Client queues (client_lock held)
// ---------------------------------------------------------------------------

static void evdev_set_time(struct input_event *ev, uint64_t ns)
{
	ev->time.tv_sec = (int64_t)(ns / 1000000000ULL);
	ev->time.tv_usec = (int64_t)((ns % 1000000000ULL) / 1000ULL);
}

/* Clocks: CLOCK_MONOTONIC (and BOOTTIME, the same here: the machine never
 * suspends) is time since boot; REALTIME adds the wall clock's offset.  Both
 * come from one counter, so events stay ordered whichever a client picks. */

static void evdev_queue_event(struct evdev_client *c,
			      const struct input_event *ev)
{
	unsigned int mask = c->bufsize - 1;

	c->buffer[c->head] = *ev;
	c->head = (c->head + 1) & mask;
	if (unlikely(c->head == c->tail)) {
		/* Full: drop everything unread.  What is left is a
		 * SYN_DROPPED record and the event just queued; the client
		 * resynchronises at the next SYN_REPORT. */
		c->tail = (c->head - 2) & mask;
		c->buffer[c->tail].time = ev->time;
		c->buffer[c->tail].type = EV_SYN;
		c->buffer[c->tail].code = SYN_DROPPED;
		c->buffer[c->tail].value = 0;
		c->packet_head = c->tail;
	}
	if (ev->type == EV_SYN && ev->code == SYN_REPORT)
		c->packet_head = c->head;
}

/* The time a SYN_DROPPED record carries on the client's clock: the time of
 * the device's packet in progress if it has one, else now.  Takes the
 * device's event lock: not under the client lock. */
static uint64_t evdev_syn_dropped_ns(const struct evdev_client *c, int clk)
{
	struct input_dev *dev = c->evdev->dev;
	uint64_t mono, f;

	spin_lock_irqsave(&dev->event_lock, &f);
	mono = dev->timestamp_set ? dev->timestamp_ns : timer_get_precise_ns();
	spin_unlock_irqrestore(&dev->event_lock, f);
	if (clk == EVDEV_CLK_REAL)
		mono += timer_realtime_offset_ns(mono);
	return mono;
}

/* A SYN_DROPPED record at the end of the queue: the client's view of the
 * device state is no longer complete. */
static void evdev_queue_syn_dropped_locked(struct evdev_client *c,
					   uint64_t ns)
{
	unsigned int mask = c->bufsize - 1;
	struct input_event *ev = &c->buffer[c->head];

	evdev_set_time(ev, ns);
	ev->type = EV_SYN;
	ev->code = SYN_DROPPED;
	ev->value = 0;
	c->head = (c->head + 1) & mask;
	if (unlikely(c->head == c->tail)) {
		/* Drop the queue but keep the SYN_DROPPED record. */
		c->tail = (c->head - 1) & mask;
		c->packet_head = c->tail;
	}
}

/* Remove every queued event of `type', and the SYN_REPORTs of packets
 * that become empty that way.  Used when the client reads that type's
 * state, so the snapshot and the queue agree. */
static void evdev_flush_type_locked(struct evdev_client *c, unsigned int type)
{
	unsigned int mask = c->bufsize - 1;
	unsigned int rd, wr = c->tail;
	/* Events kept since the last kept SYN_REPORT.  Starting at 1 keeps a
	 * SYN_REPORT at the very front, which ends a packet read before. */
	unsigned int kept = 1;

	c->packet_head = c->tail;
	for (rd = c->tail; rd != c->head; rd = (rd + 1) & mask) {
		const struct input_event *ev = &c->buffer[rd];
		int report = ev->type == EV_SYN && ev->code == SYN_REPORT;

		if (ev->type == type)
			continue;
		if (report && kept == 0)
			continue;
		if (wr != rd)
			c->buffer[wr] = *ev;
		wr = (wr + 1) & mask;
		kept++;
		if (report) {
			kept = 0;
			c->packet_head = wr;
		}
	}
	c->head = wr;
}

static unsigned int evdev_mask_cnt(unsigned int type)
{
	switch (type) {
	case EV_SYN:
		return EV_CNT; // [0]: the mask of types
	case EV_KEY:
		return KEY_CNT;
	case EV_REL:
		return REL_CNT;
	case EV_ABS:
		return ABS_CNT;
	case EV_MSC:
		return MSC_CNT;
	case EV_SW:
		return SW_CNT;
	case EV_LED:
		return LED_CNT;
	case EV_SND:
		return SND_CNT;
	case EV_FF:
		return FF_CNT;
	default:
		return 0;
	}
}

/* Masked out by EVIOCSMASK?  EV_SYN never is: packets must stay framed. */
static int evdev_is_filtered(const struct evdev_client *c, unsigned int type,
			     unsigned int code)
{
	const unsigned long *m;
	unsigned int cnt;

	if (type == EV_SYN)
		return 0;
	m = c->evmasks[0];
	if (m && type < EV_CNT && !input_test_bit(type, m))
		return 1;
	cnt = evdev_mask_cnt(type);
	if (!cnt || code >= cnt || type >= EV_CNT)
		return 0;
	m = c->evmasks[type];
	return m && !input_test_bit(code, m);
}

/* Queue one packet for one client; returns whether it completed a packet
 * the client can read now. */
static int evdev_pass_client(struct evdev_client *c,
			     const struct input_value *vals,
			     unsigned int count, uint64_t mono_ns,
			     uint64_t real_ns)
{
	struct input_event ev;
	int wake = 0;

	if (c->revoked)
		return 0;
	evdev_set_time(&ev, c->clk_type == EVDEV_CLK_REAL ? real_ns : mono_ns);
	for (unsigned int i = 0; i < count; i++) {
		const struct input_value *v = &vals[i];

		if (evdev_is_filtered(c, v->type, v->code))
			continue;
		if (v->type == EV_SYN && v->code == SYN_REPORT) {
			/* Everything of the packet was masked out. */
			if (c->packet_head == c->head)
				continue;
			wake = 1;
		}
		ev.type = v->type;
		ev.code = v->code;
		ev.value = v->value;
		evdev_queue_event(c, &ev);
	}
	return wake;
}

int evdev_pass_values(struct evdev *e, const struct input_value *vals,
		      unsigned int count, uint64_t mono_ns)
{
	struct evdev_client *c;
	uint64_t real_ns, f;
	int wake = 0, grabbed;

	if (!e || !count)
		return 0;
	real_ns = mono_ns + timer_realtime_offset_ns(mono_ns);
	spin_lock_irqsave(&e->client_lock, &f);
	grabbed = e->grab != NULL;
	if (grabbed) {
		wake = evdev_pass_client(e->grab, vals, count, mono_ns,
					 real_ns);
	} else {
		for (c = e->clients; c; c = c->next)
			wake |= evdev_pass_client(c, vals, count, mono_ns,
						  real_ns);
	}
	spin_unlock_irqrestore(&e->client_lock, f);
	/* Still under the device's event lock: the node cannot be unlinked
	 * and freed before this wake is done. */
	if (wake)
		evdev_wake(e, EVDEV_POLLIN | EVDEV_POLLRDNORM | EVDEV_POLLOUT |
				      EVDEV_POLLWRNORM);
	return grabbed;
}

// ---------------------------------------------------------------------------
// Open / release
// ---------------------------------------------------------------------------

static unsigned int evdev_compute_bufsize(const struct evdev *e)
{
	const struct input_dev *dev = e->dev;
	unsigned int n, size;

	if (e->is_static)
		return EVDEV_BUILTIN_BUFFER;
	n = dev->hint_events_per_packet * EVDEV_BUF_PACKETS;
	if (n < EVDEV_MIN_BUFFER_SIZE)
		n = EVDEV_MIN_BUFFER_SIZE;
	if (n > EVDEV_MAX_BUFFER_SIZE)
		n = EVDEV_MAX_BUFFER_SIZE;
	for (size = 1; size < n; size <<= 1)
		;
	return size;
}

int evdev_open(int unit, struct evdev_client **out)
{
	struct evdev_client *c;
	struct evdev *e = NULL;
	uint64_t f;
	int rc = 0;

	might_sleep();
	if (!out || unit < 0 || unit >= EVDEV_MINORS)
		return -ENODEV;
	evdev_builtin_init();

	spin_lock_irqsave(&g_evdev_table_lock, &f);
	if (g_evdev_table[unit] &&
	    __atomic_load_n(&g_evdev_table[unit]->exist, __ATOMIC_ACQUIRE) &&
	    refcount_inc_not_zero(&g_evdev_table[unit]->refcount))
		e = g_evdev_table[unit];
	spin_unlock_irqrestore(&g_evdev_table_lock, f);
	if (!e)
		return -ENODEV;

	c = (struct evdev_client *)kcalloc(1, sizeof(*c));
	if (c) {
		c->bufsize = evdev_compute_bufsize(e);
		c->buffer = (struct input_event *)kcalloc(
			c->bufsize, sizeof(struct input_event));
	}
	if (!c || !c->buffer) {
		kfree(c);
		evdev_put(e);
		return -ENOMEM;
	}
	c->evdev = e;
	c->clk_type = EVDEV_CLK_REAL;

	mutex_lock(&e->mutex);
	if (!e->exist) {
		rc = -ENODEV;
	} else {
		/* The client is attached before the device opens, so it sees
		 * whatever the device reports from its open() on. */
		spin_lock_irqsave(&e->client_lock, &f);
		c->next = e->clients;
		e->clients = c;
		spin_unlock_irqrestore(&e->client_lock, f);
		if (e->open++ == 0) {
			rc = input_open_device(e->dev);
			if (rc)
				e->open--;
		}
		if (rc) {
			struct evdev_client **pp;

			spin_lock_irqsave(&e->client_lock, &f);
			for (pp = &e->clients; *pp; pp = &(*pp)->next) {
				if (*pp == c) {
					*pp = c->next;
					break;
				}
			}
			spin_unlock_irqrestore(&e->client_lock, f);
		}
	}
	mutex_unlock(&e->mutex);

	if (rc) {
		kfree(c->buffer);
		kfree(c);
		evdev_put(e);
		return rc;
	}
	*out = c;
	return 0;
}

void evdev_release(struct evdev_client *c)
{
	struct evdev_client **pp;
	struct evdev *e;
	uint64_t f;

	might_sleep();
	if (!c)
		return;
	e = c->evdev;

	mutex_lock(&e->mutex);
	spin_lock_irqsave(&e->client_lock, &f);
	/* A grab held through this descriptor ends with it, whoever closes
	 * it -- a descriptor is not always closed by the task that opened
	 * it, and a grab left behind would swallow every event for good. */
	if (e->grab == c)
		e->grab = NULL;
	for (pp = &e->clients; *pp; pp = &(*pp)->next) {
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	}
	spin_unlock_irqrestore(&e->client_lock, f);
	/* After a disconnect the device was already closed for everybody
	 * (and `open' reset); only a live node counts its users down. */
	if (e->exist && !WARN_ON(e->open <= 0) && --e->open == 0)
		input_close_device(e->dev);
	mutex_unlock(&e->mutex);

	for (unsigned int i = 0; i < EV_CNT; i++)
		kfree(c->evmasks[i]);
	kfree(c->buffer);
	kfree(c);
	evdev_put(e);
}

int evdev_client_unit(const struct evdev_client *c)
{
	return c ? c->evdev->minor : -1;
}

// ---------------------------------------------------------------------------
// read / write / poll
// ---------------------------------------------------------------------------

/* SMAP-aware copies of whole, user-range-checked spans.  A zero length
 * touches nothing and always succeeds. */
static int evdev_copy_out(void *user_dst, const void *src, size_t len)
{
	if (!len)
		return 0;
	if (!user_dst || !user_range_ok((uint64_t)user_dst, len))
		return -EFAULT;
	smap_disable();
	kmemcpy(user_dst, src, len);
	smap_enable();
	return 0;
}

static int evdev_copy_in(void *dst, const void *user_src, size_t len)
{
	if (!len)
		return 0;
	if (!user_src || !user_range_ok((uint64_t)user_src, len))
		return -EFAULT;
	smap_disable();
	kmemcpy(dst, user_src, len);
	smap_enable();
	return 0;
}

static int evdev_zero_out(void *user_dst, size_t len)
{
	static const uint8_t zeros[64];
	size_t off = 0;

	if (!len)
		return 0;
	if (!user_dst || !user_range_ok((uint64_t)user_dst, len))
		return -EFAULT;
	while (off < len) {
		size_t chunk = len - off;

		if (chunk > sizeof(zeros))
			chunk = sizeof(zeros);
		smap_disable();
		kmemcpy((uint8_t *)user_dst + off, zeros, chunk);
		smap_enable();
		off += chunk;
	}
	return 0;
}

static int evdev_alive(const struct evdev_client *c)
{
	return __atomic_load_n(&c->evdev->exist, __ATOMIC_ACQUIRE) &&
	       !__atomic_load_n(&c->revoked, __ATOMIC_ACQUIRE);
}

static int evdev_has_packet(const struct evdev_client *c)
{
	return __atomic_load_n(&c->packet_head, __ATOMIC_ACQUIRE) !=
	       __atomic_load_n(&c->tail, __ATOMIC_ACQUIRE);
}

long evdev_read(struct evdev_client *c, void *user_buf, long bytes,
		int nonblock)
{
	struct input_event burst[EVDEV_READ_BURST];
	struct evdev *e;
	task_t *cur = sched_current();
	uint64_t f;

	BUILD_BUG_ON(sizeof(struct input_event) != 24);
	if (!c || bytes < 0)
		return -EINVAL;
	/* Whole records only (a zero-length read just reports readiness). */
	if (bytes != 0 && bytes < (long)sizeof(struct input_event))
		return -EINVAL;
	e = c->evdev;

	for (;;) {
		long done = 0;

		if (!evdev_alive(c))
			return -ENODEV;
		if (!evdev_has_packet(c) && nonblock)
			return -EAGAIN;
		if (bytes == 0)
			return 0;
		/* Checked before anything is taken off the queue: a bad
		 * buffer loses no events. */
		if (!user_buf ||
		    !user_range_ok((uint64_t)user_buf, (uint64_t)bytes))
			return -EFAULT;

		while (done + (long)sizeof(struct input_event) <= bytes) {
			unsigned int want, got = 0;

			want = (unsigned int)((bytes - done) /
					      (long)sizeof(struct input_event));
			if (want > EVDEV_READ_BURST)
				want = EVDEV_READ_BURST;
			spin_lock_irqsave(&e->client_lock, &f);
			while (got < want && c->tail != c->packet_head) {
				burst[got++] = c->buffer[c->tail];
				c->tail = (c->tail + 1) & (c->bufsize - 1);
			}
			spin_unlock_irqrestore(&e->client_lock, f);
			if (!got)
				break;
			if (evdev_copy_out((uint8_t *)user_buf + done, burst,
					   got * sizeof(struct input_event)))
				return -EFAULT;
			done += (long)(got * sizeof(struct input_event));
		}
		if (done)
			return done;
		if (nonblock)
			return -EAGAIN; // another reader of this queue won
		if (!cur)
			return -EAGAIN; // no task context: cannot block

		// Park atomically wrt the producer (same protocol as
		// tty_read): publish BLOCKED + channel, re-check the queue
		// under the lock, then schedule.
		cur->wait_channel = (void *)&e->wait_key;
		task_set_blocked(cur);
		spin_lock_irqsave(&e->client_lock, &f);
		if (c->packet_head != c->tail || !evdev_alive(c)) {
			task_state_t asleep = TASK_BLOCKED;

			/* Un-park by CAS, the way every waker claims: a trace
			 * stop may have claimed this sleep meanwhile, and a
			 * plain store ran on straight through it.  Claimed
			 * first -- woken, or stopped -- the scheduler below
			 * reconciles. */
			if (__atomic_compare_exchange_n(&cur->state, &asleep,
							TASK_READY, false,
							__ATOMIC_ACQ_REL,
							__ATOMIC_ACQUIRE)) {
				cur->wait_channel = NULL;
				spin_unlock_irqrestore(&e->client_lock, f);
				continue;
			}
		}
		spin_unlock_irqrestore(&e->client_lock, f);
		sched_schedule();
		if (cur->state == TASK_ZOMBIE || cur->has_exited ||
		    signal_pending(cur))
			return -EINTR;
	}
}

long evdev_write(struct evdev_client *c, const void *user_buf, long bytes)
{
	struct input_event ev;
	struct evdev *e;
	long done = 0;
	int rc;

	if (!c || bytes < 0)
		return -EINVAL;
	if (bytes > EVDEV_WRITE_MAX)
		bytes = EVDEV_WRITE_MAX;
	if (bytes != 0 && bytes < (long)sizeof(struct input_event))
		return -EINVAL;
	e = c->evdev;
	rc = mutex_lock_interruptible(&e->mutex);
	if (rc)
		return rc;
	if (!evdev_alive(c)) {
		done = -ENODEV;
		goto out;
	}
	while (done + (long)sizeof(ev) <= bytes) {
		if (evdev_copy_in(&ev, (const uint8_t *)user_buf + done,
				  sizeof(ev))) {
			done = -EFAULT;
			goto out;
		}
		done += (long)sizeof(ev);
		input_inject_event(e->dev, ev.type, ev.code, ev.value);
	}
out:
	mutex_unlock(&e->mutex);
	return done;
}

short evdev_poll(struct evdev_client *c, short events)
{
	short mask;

	if (!c)
		return EVDEV_POLLERR;
	mask = evdev_alive(c) ? (EVDEV_POLLOUT | EVDEV_POLLWRNORM) :
				(EVDEV_POLLHUP | EVDEV_POLLERR);
	if (evdev_has_packet(c))
		mask |= EVDEV_POLLIN | EVDEV_POLLRDNORM;
	return (short)(mask & (events | EVDEV_POLLHUP | EVDEV_POLLERR));
}

struct wait_queue_head *evdev_pollq(struct evdev_client *c)
{
	return c ? &c->evdev->poll_wq : NULL;
}

// ---------------------------------------------------------------------------
// ioctl helpers
// ---------------------------------------------------------------------------

/* A bitmap of codes 0..maxbit: as many whole words as the bitmap has, cut
 * to the caller's length.  Returns the bytes copied. */
static int evdev_bits_to_user(void *argp, const unsigned long *bits,
			      unsigned int maxbit, unsigned int maxlen)
{
	unsigned int len = INPUT_BITS_TO_LONGS(maxbit) * sizeof(unsigned long);

	if (len > maxlen)
		len = maxlen;
	return evdev_copy_out(argp, bits, len) ? -EFAULT : (int)len;
}

/* A string with its NUL, cut to the caller's length; -ENOENT for a device
 * that does not have the property at all. */
static int evdev_str_to_user(void *argp, const char *str, unsigned int maxlen)
{
	unsigned int len;

	if (!str)
		return -ENOENT;
	len = (unsigned int)kstrlen(str) + 1;
	if (len > maxlen)
		len = maxlen;
	return evdev_copy_out(argp, str, len) ? -EFAULT : (int)len;
}

static int evdev_handle_eviocgbit(struct input_dev *dev, unsigned int type,
				  unsigned int size, void *argp)
{
	unsigned long snap[INPUT_BITS_TO_LONGS(KEY_CNT)];
	const unsigned long *bits;
	unsigned int max;
	uint64_t f;

	switch (type) {
	case 0:
		bits = dev->evbit, max = EV_MAX;
		break;
	case EV_KEY:
		bits = dev->keybit, max = KEY_MAX;
		break;
	case EV_REL:
		bits = dev->relbit, max = REL_MAX;
		break;
	case EV_ABS:
		bits = dev->absbit, max = ABS_MAX;
		break;
	case EV_MSC:
		bits = dev->mscbit, max = MSC_MAX;
		break;
	case EV_LED:
		bits = dev->ledbit, max = LED_MAX;
		break;
	case EV_SND:
		bits = dev->sndbit, max = SND_MAX;
		break;
	case EV_FF:
		bits = dev->ffbit, max = FF_MAX;
		break;
	case EV_SW:
		bits = dev->swbit, max = SW_MAX;
		break;
	default:
		return -EINVAL;
	}
	/* A snapshot: the built-in mouse can turn absolute at run time. */
	spin_lock_irqsave(&dev->event_lock, &f);
	kmemcpy(snap, bits, INPUT_BITS_TO_LONGS(max) * sizeof(unsigned long));
	spin_unlock_irqrestore(&dev->event_lock, f);
	return evdev_bits_to_user(argp, snap, max, size);
}

/* EVIOCGKEY / EVIOCGLED / EVIOCGSND / EVIOCGSW: the state bitmap, with the
 * queued events of that type removed from the client's queue so the two
 * agree.  When the copy fails the client is told its view is incomplete. */
static int evdev_handle_get_state(struct evdev_client *c,
				  struct input_dev *dev, unsigned int type,
				  unsigned int size, void *argp)
{
	unsigned long snap[INPUT_BITS_TO_LONGS(KEY_CNT)];
	const unsigned long *bits;
	unsigned int max;
	uint64_t f, cf;
	int rc;

	switch (type) {
	case EV_KEY:
		bits = dev->key, max = KEY_MAX;
		break;
	case EV_LED:
		bits = dev->led, max = LED_MAX;
		break;
	case EV_SND:
		bits = dev->snd, max = SND_MAX;
		break;
	default:
		bits = dev->sw, max = SW_MAX;
		break;
	}
	spin_lock_irqsave(&dev->event_lock, &f);
	spin_lock_irqsave(&c->evdev->client_lock, &cf);
	kmemcpy(snap, bits, INPUT_BITS_TO_LONGS(max) * sizeof(unsigned long));
	evdev_flush_type_locked(c, type);
	spin_unlock_irqrestore(&c->evdev->client_lock, cf);
	spin_unlock_irqrestore(&dev->event_lock, f);

	rc = evdev_bits_to_user(argp, snap, max, size);
	if (rc < 0) {
		uint64_t ns = evdev_syn_dropped_ns(c, c->clk_type);

		spin_lock_irqsave(&c->evdev->client_lock, &cf);
		evdev_queue_syn_dropped_locked(c, ns);
		spin_unlock_irqrestore(&c->evdev->client_lock, cf);
	}
	return rc;
}

/* EVIOCGMTSLOTS: { u32 code; s32 values[]; } -- the code in, one value per
 * slot out, as many slots as the buffer has room for. */
static int evdev_handle_mt_request(struct input_dev *dev, unsigned int size,
				   void *argp)
{
	int32_t values[EVDEV_MT_CHUNK];
	uint32_t code;
	int max_slots, done = 0;

	/* Without room for the code the request is malformed (rather than
	 * a buffer of "minus one" slots). */
	if (size < sizeof(uint32_t))
		return -EINVAL;
	if (evdev_copy_in(&code, argp, sizeof(code)))
		return -EFAULT;
	if (!dev->mt || !input_is_mt_value((int)code))
		return -EINVAL;
	max_slots = (int)((size - sizeof(uint32_t)) / sizeof(int32_t));
	while (done < max_slots) {
		int want = max_slots - done < EVDEV_MT_CHUNK ?
				   max_slots - done :
				   EVDEV_MT_CHUNK;
		int n = input_mt_get_values(dev, code, done, values, want);

		if (n < 0)
			return n;
		if (n == 0)
			break;
		if (evdev_copy_out((uint8_t *)argp + sizeof(uint32_t) +
					   (size_t)done * sizeof(int32_t),
				   values, (size_t)n * sizeof(int32_t)))
			return -EFAULT;
		done += n;
	}
	return 0;
}

static int evdev_grab(struct evdev *e, struct evdev_client *c)
{
	uint64_t f;
	int rc = 0;

	spin_lock_irqsave(&e->client_lock, &f);
	if (e->grab)
		rc = -EBUSY;
	else
		e->grab = c;
	spin_unlock_irqrestore(&e->client_lock, f);
	return rc;
}

static int evdev_ungrab(struct evdev *e, struct evdev_client *c)
{
	uint64_t f;
	int rc = 0;

	spin_lock_irqsave(&e->client_lock, &f);
	if (e->grab != c)
		rc = -EINVAL;
	else
		e->grab = NULL;
	spin_unlock_irqrestore(&e->client_lock, f);
	return rc;
}

static int evdev_revoke(struct evdev *e, struct evdev_client *c)
{
	uint64_t f;

	spin_lock_irqsave(&e->client_lock, &f);
	__atomic_store_n(&c->revoked, 1, __ATOMIC_RELEASE);
	if (e->grab == c)
		e->grab = NULL;
	spin_unlock_irqrestore(&e->client_lock, f);
	evdev_wake(e, EVDEV_POLLHUP | EVDEV_POLLERR);
	return 0;
}

static int evdev_set_clk_type(struct evdev_client *c, int clkid)
{
	uint64_t f, ns;
	int t;

	switch (clkid) {
	case CLOCK_REALTIME:
		t = EVDEV_CLK_REAL;
		break;
	case CLOCK_MONOTONIC:
		t = EVDEV_CLK_MONO;
		break;
	case CLOCK_BOOTTIME:
		t = EVDEV_CLK_BOOT;
		break;
	default:
		return -EINVAL;
	}
	if (c->clk_type == t)
		return 0;
	/* Read before the client lock (the device's lock comes first);
	 * the clock is only changed here, under the node's mutex. */
	ns = evdev_syn_dropped_ns(c, t);
	spin_lock_irqsave(&c->evdev->client_lock, &f);
	c->clk_type = t;
	/* Queued events carry the other clock's time: drop them, and say
	 * so. */
	if (c->head != c->tail) {
		c->packet_head = c->head = c->tail;
		evdev_queue_syn_dropped_locked(c, ns);
	}
	spin_unlock_irqrestore(&c->evdev->client_lock, f);
	return 0;
}

/* Bytes of a mask of `cnt' codes as it travels between the client and the
 * kernel: whole words up to the highest code (cnt - 1), cut to `maxlen'. */
static unsigned int evdev_mask_xfer(unsigned int cnt, unsigned int maxlen)
{
	unsigned int len = INPUT_BITS_TO_LONGS(cnt - 1) * sizeof(unsigned long);

	return len > maxlen ? maxlen : len;
}

/* EVIOCGMASK: the client's mask of one type; all bits set (every byte 0xff)
 * when it has none.  Unknown types read as nothing; a caller's buffer
 * larger than the mask is zero-filled (forward compatibility). */
static int evdev_get_mask(struct evdev_client *c, unsigned int type,
			  void *codes, uint32_t codes_size)
{
	unsigned long snap[INPUT_BITS_TO_LONGS(KEY_CNT)];
	unsigned int cnt = evdev_mask_cnt(type);
	unsigned int size = INPUT_BITS_TO_LONGS(cnt) * sizeof(unsigned long);
	unsigned int xfer = codes_size < size ? codes_size : size;
	uint64_t f;

	if (cnt > 0) {
		unsigned int len = xfer;

		spin_lock_irqsave(&c->evdev->client_lock, &f);
		if (c->evmasks[type]) {
			len = evdev_mask_xfer(cnt, xfer);
			kmemcpy(snap, c->evmasks[type], len);
		} else {
			kmemset(snap, 0xff, xfer);
		}
		spin_unlock_irqrestore(&c->evdev->client_lock, f);
		if (evdev_copy_out(codes, snap, len))
			return -EFAULT;
	}
	if (xfer < codes_size &&
	    evdev_zero_out((uint8_t *)codes + xfer, codes_size - xfer))
		return -EFAULT;
	return 0;
}

/* EVIOCSMASK: replace the client's mask of one type.  Unknown types are
 * accepted and ignored, and so is the part of a caller's buffer beyond
 * the mask (forward compatibility); the buffer is whole words. */
static int evdev_set_mask(struct evdev_client *c, unsigned int type,
			  const void *codes, uint32_t codes_size)
{
	unsigned int cnt = evdev_mask_cnt(type);
	unsigned int size = INPUT_BITS_TO_LONGS(cnt) * sizeof(unsigned long);
	unsigned long *mask, *old;
	uint64_t f;

	if (!cnt)
		return 0;
	mask = (unsigned long *)kcalloc(1, size);
	if (!mask)
		return -ENOMEM;
	if (codes_size % sizeof(unsigned long)) {
		kfree(mask);
		return -EINVAL;
	}
	if (evdev_copy_in(mask, codes, evdev_mask_xfer(cnt, codes_size))) {
		kfree(mask);
		return -EFAULT;
	}
	spin_lock_irqsave(&c->evdev->client_lock, &f);
	old = c->evmasks[type];
	c->evmasks[type] = mask;
	spin_unlock_irqrestore(&c->evdev->client_lock, f);
	kfree(old);
	return 0;
}

/* EVIOCGKEYCODE / EVIOCSKEYCODE: the legacy form is { scancode, keycode }
 * as two unsigned ints, the _V2 form a struct input_keymap_entry. */
static int evdev_handle_get_keycode(struct input_dev *dev, void *argp)
{
	struct input_keymap_entry ke;
	unsigned int scancode;
	int rc;

	kmemset(&ke, 0, sizeof(ke));
	ke.len = sizeof(unsigned int);
	if (evdev_copy_in(&scancode, argp, sizeof(scancode)))
		return -EFAULT;
	kmemcpy(ke.scancode, &scancode, sizeof(scancode));
	rc = input_get_keycode(dev, &ke);
	if (rc)
		return rc;
	if (evdev_copy_out((unsigned int *)argp + 1, &ke.keycode,
			   sizeof(ke.keycode)))
		return -EFAULT;
	return 0;
}

static int evdev_handle_get_keycode_v2(struct input_dev *dev, void *argp)
{
	struct input_keymap_entry ke;
	int rc;

	if (evdev_copy_in(&ke, argp, sizeof(ke)))
		return -EFAULT;
	rc = input_get_keycode(dev, &ke);
	if (rc)
		return rc;
	if (evdev_copy_out(argp, &ke, sizeof(ke)))
		return -EFAULT;
	return 0;
}

static int evdev_handle_set_keycode(struct input_dev *dev, void *argp)
{
	struct input_keymap_entry ke;
	unsigned int scancode;

	kmemset(&ke, 0, sizeof(ke));
	ke.len = sizeof(unsigned int);
	if (evdev_copy_in(&scancode, argp, sizeof(scancode)))
		return -EFAULT;
	kmemcpy(ke.scancode, &scancode, sizeof(scancode));
	if (evdev_copy_in(&ke.keycode, (unsigned int *)argp + 1,
			  sizeof(ke.keycode)))
		return -EFAULT;
	return input_set_keycode(dev, &ke);
}

static int evdev_handle_set_keycode_v2(struct input_dev *dev, void *argp)
{
	struct input_keymap_entry ke;

	if (evdev_copy_in(&ke, argp, sizeof(ke)))
		return -EFAULT;
	if (ke.len > sizeof(ke.scancode))
		return -EINVAL;
	return input_set_keycode(dev, &ke);
}

#define EVDEV_IOC_NR(req) ((unsigned int)((req) >> _INPUT_IOC_NRSHIFT) & _INPUT_IOC_NRMASK)
#define EVDEV_IOC_TYPE(req) \
	((unsigned int)((req) >> _INPUT_IOC_TYPESHIFT) & _INPUT_IOC_TYPEMASK)
#define EVDEV_IOC_SIZE(req) \
	((unsigned int)((req) >> _INPUT_IOC_SIZESHIFT) & _INPUT_IOC_SIZEMASK)
#define EVDEV_IOC_DIR(req) \
	((unsigned int)((req) >> _INPUT_IOC_DIRSHIFT) & _INPUT_IOC_DIRMASK)
/* The request with its size field cleared: matches every length of a
 * variable-length request. */
#define EVDEV_IOC_NOSIZE(req) \
	((req) & ~((unsigned long)_INPUT_IOC_SIZEMASK << _INPUT_IOC_SIZESHIFT))

static int evdev_do_ioctl(struct evdev_client *c, struct evdev *e,
			  struct input_dev *dev, unsigned long req, void *argp)
{
	unsigned int size = EVDEV_IOC_SIZE(req);
	unsigned int nr = EVDEV_IOC_NR(req);
	uint64_t f;

	/* Fixed-length requests. */
	switch (req) {
	case EVIOCGVERSION: {
		int v = EV_VERSION;

		return evdev_copy_out(argp, &v, sizeof(v));
	}
	case EVIOCGID: {
		struct input_id id = dev->id;

		return evdev_copy_out(argp, &id, sizeof(id));
	}
	case EVIOCGREP: {
		unsigned int rep[2];

		if (!input_test_bit(EV_REP, dev->evbit))
			return -ENOSYS;
		spin_lock_irqsave(&dev->event_lock, &f);
		rep[0] = (unsigned int)dev->rep[REP_DELAY];
		rep[1] = (unsigned int)dev->rep[REP_PERIOD];
		spin_unlock_irqrestore(&dev->event_lock, f);
		return evdev_copy_out(argp, rep, sizeof(rep));
	}
	case EVIOCSREP: {
		unsigned int rep[2];

		if (!input_test_bit(EV_REP, dev->evbit))
			return -ENOSYS;
		if (evdev_copy_in(rep, argp, sizeof(rep)))
			return -EFAULT;
		input_inject_event(dev, EV_REP, REP_DELAY, (int)rep[0]);
		input_inject_event(dev, EV_REP, REP_PERIOD, (int)rep[1]);
		return 0;
	}
	case EVIOCRMFF:
		/* No device here stores effects: nothing to erase. */
		return input_test_bit(EV_FF, dev->evbit) ? -EINVAL : -ENOSYS;
	case EVIOCGEFFECTS: {
		int n = 0;

		return evdev_copy_out(argp, &n, sizeof(n));
	}
	case EVIOCGRAB:
		/* The argument is the flag itself, not a pointer. */
		return argp ? evdev_grab(e, c) : evdev_ungrab(e, c);
	case EVIOCREVOKE:
		return argp ? -EINVAL : evdev_revoke(e, c);
	case EVIOCGMASK:
	case EVIOCSMASK: {
		struct input_mask m;

		if (evdev_copy_in(&m, argp, sizeof(m)))
			return -EFAULT;
		if (req == EVIOCGMASK)
			return evdev_get_mask(c, m.type,
					      (void *)(unsigned long)m.codes_ptr,
					      m.codes_size);
		return evdev_set_mask(c, m.type,
				      (const void *)(unsigned long)m.codes_ptr,
				      m.codes_size);
	}
	case EVIOCSCLOCKID: {
		int id;

		if (evdev_copy_in(&id, argp, sizeof(id)))
			return -EFAULT;
		return evdev_set_clk_type(c, id);
	}
	case EVIOCGKEYCODE:
		return evdev_handle_get_keycode(dev, argp);
	case EVIOCSKEYCODE:
		return evdev_handle_set_keycode(dev, argp);
	case EVIOCGKEYCODE_V2:
		return evdev_handle_get_keycode_v2(dev, argp);
	case EVIOCSKEYCODE_V2:
		return evdev_handle_set_keycode_v2(dev, argp);
	default:
		break;
	}

	/* Variable-length requests. */
	switch (EVDEV_IOC_NOSIZE(req)) {
	case EVDEV_IOC_NOSIZE(EVIOCGPROP(0)): {
		unsigned long snap[INPUT_BITS_TO_LONGS(INPUT_PROP_CNT)];

		spin_lock_irqsave(&dev->event_lock, &f);
		kmemcpy(snap, dev->propbit, sizeof(snap));
		spin_unlock_irqrestore(&dev->event_lock, f);
		return evdev_bits_to_user(argp, snap, INPUT_PROP_MAX, size);
	}
	case EVDEV_IOC_NOSIZE(EVIOCGMTSLOTS(0)):
		return evdev_handle_mt_request(dev, size, argp);
	case EVDEV_IOC_NOSIZE(EVIOCGKEY(0)):
		return evdev_handle_get_state(c, dev, EV_KEY, size, argp);
	case EVDEV_IOC_NOSIZE(EVIOCGLED(0)):
		return evdev_handle_get_state(c, dev, EV_LED, size, argp);
	case EVDEV_IOC_NOSIZE(EVIOCGSND(0)):
		return evdev_handle_get_state(c, dev, EV_SND, size, argp);
	case EVDEV_IOC_NOSIZE(EVIOCGSW(0)):
		return evdev_handle_get_state(c, dev, EV_SW, size, argp);
	case EVDEV_IOC_NOSIZE(EVIOCGNAME(0)):
		return evdev_str_to_user(argp, dev->name, size);
	case EVDEV_IOC_NOSIZE(EVIOCGPHYS(0)):
		return evdev_str_to_user(argp, dev->phys, size);
	case EVDEV_IOC_NOSIZE(EVIOCGUNIQ(0)):
		return evdev_str_to_user(argp, dev->uniq, size);
	case EVDEV_IOC_NOSIZE(EVIOCSFF): {
		struct ff_effect effect;

		/* An effect of another layout cannot be read: -EFAULT, as a
		 * failed copy. */
		if (size != sizeof(effect))
			return -EFAULT;
		if (evdev_copy_in(&effect, argp, sizeof(effect)))
			return -EFAULT;
		return -ENOSYS; // nothing here plays effects
	}
	default:
		break;
	}

	/* Requests that carry a number in their nr field. */
	if (EVDEV_IOC_TYPE(req) != 'E')
		return -EINVAL;
	if (EVDEV_IOC_DIR(req) == _INPUT_IOC_READ) {
		if ((nr & ~(unsigned int)EV_MAX) == 0x20) // EVIOCGBIT(ev, len)
			return evdev_handle_eviocgbit(dev, nr & EV_MAX, size,
						      argp);
		if ((nr & ~(unsigned int)ABS_MAX) == 0x40) { // EVIOCGABS(abs)
			struct input_absinfo abs;
			unsigned int len = size < sizeof(abs) ? size :
								(unsigned int)sizeof(abs);
			int has_abs;

			spin_lock_irqsave(&dev->event_lock, &f);
			has_abs = input_test_bit(EV_ABS, dev->evbit);
			abs = dev->absinfo[nr & ABS_MAX];
			spin_unlock_irqrestore(&dev->event_lock, f);
			/* A device without absolute axes has no axis
			 * parameters to ask about. */
			if (!has_abs)
				return -EINVAL;
			return evdev_copy_out(argp, &abs, len);
		}
	}
	if (EVDEV_IOC_DIR(req) == _INPUT_IOC_WRITE) {
		if ((nr & ~(unsigned int)ABS_MAX) == 0xc0) { // EVIOCSABS(abs)
			struct input_absinfo abs = { 0, 0, 0, 0, 0, 0 };
			unsigned int axis = nr & ABS_MAX;
			unsigned int len = size < sizeof(abs) ? size :
								(unsigned int)sizeof(abs);

			if (!input_test_bit(EV_ABS, dev->evbit))
				return -EINVAL;
			if (evdev_copy_in(&abs, argp, len))
				return -EFAULT;
			if (size < sizeof(abs))
				abs.resolution = 0; // an older, shorter layout
			/* The slot count is fixed at registration. */
			if (axis == ABS_MT_SLOT)
				return -EINVAL;
			spin_lock_irqsave(&dev->event_lock, &f);
			dev->absinfo[axis] = abs;
			spin_unlock_irqrestore(&dev->event_lock, f);
			return 0;
		}
	}
	return -EINVAL;
}

int evdev_ioctl(struct evdev_client *c, unsigned long req, void *argp)
{
	struct evdev *e;
	int rc;

	if (!c)
		return -EINVAL;
	e = c->evdev;
	rc = mutex_lock_interruptible(&e->mutex);
	if (rc)
		return rc;
	/* Under the mutex: the device cannot be taken away while a request
	 * reads it, and after that every request answers ENODEV. */
	if (!evdev_alive(c))
		rc = -ENODEV;
	else
		rc = evdev_do_ioctl(c, e, e->dev, req, argp);
	mutex_unlock(&e->mutex);
	return rc;
}

// ---------------------------------------------------------------------------
// Producer entry points of the built-in devices (interrupt context)
// ---------------------------------------------------------------------------

// Set-1 extended (0xE0-prefixed) scancode -> key code
static uint16_t evdev_e0_keycode(uint8_t code)
{
	switch (code) {
	case 0x1C:
		return KEY_KPENTER;
	case 0x1D:
		return KEY_RIGHTCTRL;
	case 0x35:
		return KEY_KPSLASH;
	case 0x37:
		return KEY_SYSRQ;
	case 0x38:
		return KEY_RIGHTALT;
	case 0x47:
		return KEY_HOME;
	case 0x48:
		return KEY_UP;
	case 0x49:
		return KEY_PAGEUP;
	case 0x4B:
		return KEY_LEFT;
	case 0x4D:
		return KEY_RIGHT;
	case 0x4F:
		return KEY_END;
	case 0x50:
		return KEY_DOWN;
	case 0x51:
		return KEY_PAGEDOWN;
	case 0x52:
		return KEY_INSERT;
	case 0x53:
		return KEY_DELETE;
	case 0x5B:
		return KEY_LEFTMETA;
	case 0x5C:
		return KEY_RIGHTMETA;
	case 0x5D:
		return KEY_COMPOSE;
	default:
		return KEY_RESERVED;
	}
}

// Set-1 make codes above 0x58 (no prefix) -> key codes: the keys of
// Japanese, Korean and Brazilian keyboards.
static uint16_t evdev_high_keycode(uint8_t code)
{
	switch (code) {
	case 0x59:
		return KEY_KPEQUAL;
	case 0x70:
		return KEY_KATAKANAHIRAGANA;
	case 0x71:
		return KEY_HANJA;
	case 0x72:
		return KEY_HANGEUL;
	case 0x73:
		return KEY_RO;
	case 0x79:
		return KEY_HENKAN;
	case 0x7B:
		return KEY_MUHENKAN;
	case 0x7D:
		return KEY_YEN;
	case 0x7E:
		return KEY_KPCOMMA;
	default:
		return KEY_RESERVED;
	}
}

/* One key edge as its own packet: press (1), autorepeat (2: a press while
 * the key is already down) or release (0; one without a press is dropped
 * by the core). */
static void evdev_builtin_key(struct input_dev *dev, unsigned int code,
			      int pressed)
{
	uint64_t f;
	int value;

	if (code == KEY_RESERVED || code > KEY_MAX)
		return;
	spin_lock_irqsave(&dev->event_lock, &f);
	if (pressed)
		value = input_test_bit(code, dev->key) ? 2 : 1;
	else
		value = 0;
	input_handle_event_locked(dev, EV_KEY, code, value);
	input_handle_event_locked(dev, EV_SYN, SYN_REPORT, 0);
	spin_unlock_irqrestore(&dev->event_lock, f);
}

int evdev_feed_keyboard(uint8_t scan_code)
{
	static int e0_prefix; // producer-side prefix state (IRQ serialised)
	static int e1_left; // bytes still to come of an 0xE1 sequence
	static uint8_t e1_first;
	struct input_dev *dev = &g_kbd_dev;
	uint16_t code;
	int pressed;

	evdev_builtin_init();

	if (scan_code == 0xE0) {
		e0_prefix = 1;
		return evdev_kbd_grabbed();
	}
	// Pause is 0xE1 1D 45 (press) and 0xE1 9D C5 (release): one key, not
	// a Ctrl and a Num Lock.
	if (scan_code == 0xE1) {
		e1_left = 2;
		return evdev_kbd_grabbed();
	}
	if (e1_left) {
		if (--e1_left) {
			e1_first = scan_code;
			return evdev_kbd_grabbed();
		}
		if ((e1_first & 0x7F) == 0x1D && (scan_code & 0x7F) == 0x45)
			evdev_builtin_key(dev, KEY_PAUSE, !(scan_code & 0x80));
		return evdev_kbd_grabbed();
	}
	pressed = !(scan_code & 0x80);
	if (e0_prefix) {
		e0_prefix = 0;
		code = evdev_e0_keycode(scan_code & 0x7F);
	} else if (scan_code == 0xF1 || scan_code == 0xF2) {
		// Hanja / Hangul: a press with no release code.
		code = evdev_high_keycode(scan_code & 0x7F);
		evdev_builtin_key(dev, code, 1);
		pressed = 0;
	} else {
		code = scan_code & 0x7F; // set-1 make codes == key codes
		if (code > 0x58)
			code = evdev_high_keycode((uint8_t)code);
	}
	evdev_builtin_key(dev, code, pressed);
	return evdev_kbd_grabbed();
}

// Button edges as EV_KEY events, each its own packet.  The state is the
// device's, whichever source reports: the producers are serialised by the
// mouse driver.
static void evdev_mouse_buttons(struct input_dev *dev, uint8_t buttons)
{
	static uint8_t last_buttons;
	uint8_t changed = buttons ^ last_buttons;

	if (changed & 0x01)
		evdev_builtin_key(dev, BTN_LEFT, buttons & 0x01);
	if (changed & 0x02)
		evdev_builtin_key(dev, BTN_RIGHT, buttons & 0x02);
	if (changed & 0x04)
		evdev_builtin_key(dev, BTN_MIDDLE, buttons & 0x04);
	last_buttons = buttons;
}

// Move an absolute axis to `value' (clamped to its range); the core drops
// the event when the position does not change.  Caller holds the event lock.
static void evdev_abs_move_locked(struct input_dev *dev, unsigned int axis,
				  int64_t value)
{
	const struct input_absinfo *a = &dev->absinfo[axis];

	if (value < a->minimum)
		value = a->minimum;
	if (value > a->maximum)
		value = a->maximum;
	input_handle_event_locked(dev, EV_ABS, axis, (int)value);
}

void evdev_mouse_set_absolute(int32_t max, int screen_w, int screen_h)
{
	struct input_dev *dev = &g_mouse_dev;
	uint64_t f;

	evdev_builtin_init();
	spin_lock_irqsave(&dev->event_lock, &f);
	/* An absolute pointer: positions on the screen, no relative motion
	 * of its own, and so not INPUT_PROP_POINTER either.  The wheel stays
	 * relative. */
	input_set_bit(EV_ABS, dev->evbit);
	input_set_bit(ABS_X, dev->absbit);
	input_set_bit(ABS_Y, dev->absbit);
	input_clear_bit(REL_X, dev->relbit);
	input_clear_bit(REL_Y, dev->relbit);
	input_clear_bit(INPUT_PROP_POINTER, dev->propbit);
	for (unsigned int i = ABS_X; i <= ABS_Y; i++) {
		dev->absinfo[i].minimum = 0;
		dev->absinfo[i].maximum = max;
		dev->absinfo[i].value = max / 2;
		dev->absinfo[i].fuzz = 0;
		dev->absinfo[i].flat = 0;
	}
	g_abs_screen_w = screen_w;
	g_abs_screen_h = screen_h;
	spin_unlock_irqrestore(&dev->event_lock, f);
}

void evdev_mouse_set_abs_screen(int screen_w, int screen_h)
{
	struct input_dev *dev = &g_mouse_dev;
	uint64_t f;

	evdev_builtin_init();
	spin_lock_irqsave(&dev->event_lock, &f);
	g_abs_screen_w = screen_w;
	g_abs_screen_h = screen_h;
	spin_unlock_irqrestore(&dev->event_lock, f);
}

int evdev_feed_mouse_abs(uint32_t x, uint32_t y, uint8_t buttons, int wheel)
{
	struct input_dev *dev = &g_mouse_dev;
	uint64_t f;

	evdev_builtin_init();
	evdev_mouse_buttons(dev, buttons);

	spin_lock_irqsave(&dev->event_lock, &f);
	if (input_test_bit(EV_ABS, dev->evbit)) {
		evdev_abs_move_locked(dev, ABS_X, x);
		evdev_abs_move_locked(dev, ABS_Y, y);
	}
	input_handle_event_locked(dev, EV_REL, REL_WHEEL, wheel);
	input_handle_event_locked(dev, EV_SYN, SYN_REPORT, 0);
	spin_unlock_irqrestore(&dev->event_lock, f);
	return evdev_mouse_grabbed();
}

int evdev_feed_mouse(int dx, int dy, uint8_t buttons, int wheel, int hwheel)
{
	struct input_dev *dev = &g_mouse_dev;
	uint64_t f;

	evdev_builtin_init();
	evdev_mouse_buttons(dev, buttons);

	spin_lock_irqsave(&dev->event_lock, &f);
	if (input_test_bit(EV_ABS, dev->evbit)) {
		/* The axes are absolute: relative motion from another source
		 * (the emulated mouse, before the absolute pointer has said
		 * anything) moves the position by the same share of the
		 * screen it would move a pointer. */
		int w = g_abs_screen_w > 0 ? g_abs_screen_w : 1;
		int h = g_abs_screen_h > 0 ? g_abs_screen_h : 1;
		const struct input_absinfo *ax = &dev->absinfo[ABS_X];
		const struct input_absinfo *ay = &dev->absinfo[ABS_Y];

		if (dx)
			evdev_abs_move_locked(
				dev, ABS_X,
				(int64_t)ax->value +
					(int64_t)dx * ((int64_t)ax->maximum + 1) /
						w);
		if (dy)
			evdev_abs_move_locked(
				dev, ABS_Y,
				(int64_t)ay->value +
					(int64_t)dy * ((int64_t)ay->maximum + 1) /
						h);
	} else {
		input_handle_event_locked(dev, EV_REL, REL_X, dx);
		input_handle_event_locked(dev, EV_REL, REL_Y, dy);
	}
	input_handle_event_locked(dev, EV_REL, REL_WHEEL, wheel);
	input_handle_event_locked(dev, EV_REL, REL_HWHEEL, hwheel);
	input_handle_event_locked(dev, EV_SYN, SYN_REPORT, 0);
	spin_unlock_irqrestore(&dev->event_lock, f);
	return evdev_mouse_grabbed();
}

int evdev_kbd_grabbed(void)
{
	return __atomic_load_n(&g_builtin_nodes[EVDEV_UNIT_KEYBOARD].grab,
			       __ATOMIC_RELAXED) != NULL;
}

int evdev_mouse_grabbed(void)
{
	return __atomic_load_n(&g_builtin_nodes[EVDEV_UNIT_MOUSE].grab,
			       __ATOMIC_RELAXED) != NULL;
}
