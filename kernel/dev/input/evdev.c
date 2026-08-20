// LikeOS-64 evdev input core - implementation
//
// Ring-buffered input events with the standard event-device semantics:
// whole input_event records via read(), EV_SYN frame terminators, drop-
// oldest overflow with SYN_DROPPED notification, capability bitmap ioctls,
// and single-holder exclusive grabs.
//
// Locking: one spinlock per device guards the ring, key-state bitmap and
// grab holder.  Producers run in interrupt context (spin_lock_irqsave, no
// prints, no sleeping); readers park with the established wait_channel/
// BLOCKED protocol.

#include <kernel/ke/waitq.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/uapi/input.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

// Wake pollers sleeping in poll()/select() (kernel/net/poll.c)
extern void poll_notify_io_ready(void);

#define EVDEV_RING_SIZE 256 // events; power of two not required
#define EVDEV_MAX_READ_BURST 32 // events copied per read() pass

typedef struct {
	spinlock_t lock;
	struct input_event ring[EVDEV_RING_SIZE];
	unsigned head; // producer index
	unsigned tail; // consumer index
	unsigned count;
	uint32_t dropped; // ring overflows (reported once per overflow)
	int64_t grab_task; // task id holding the grab, 0 = none
	/* The descriptor the grab was taken through.
	 *
	 * The task id alone is not enough to release it: the release happens
	 * when the descriptor is closed, and a descriptor is not always closed
	 * by the task that opened it -- process teardown, a shared descriptor
	 * table, or a reaper can all do it.  When the ids did not match the grab
	 * was simply left in place, and with it every pointer event stayed
	 * suppressed: after an X session ended the console cursor was drawn but
	 * would not move, forever.
	 *
	 * Keyed on the handle, the release is exact and does not care who is
	 * doing the closing. */
	void *grab_owner;
	uint8_t key_state[KEY_CNT / 8]; // current down/up bitmap
	struct input_id id;
	const char *name;
	const char *phys; // physical location, e.g. "isa0060/serio0/input0"
	uint8_t evbits[EV_CNT / 8];
	uint8_t keybits[KEY_CNT / 8];
	uint8_t relbits[REL_CNT / 8];
	uint8_t propbits[INPUT_PROP_CNT / 8]; // device properties (EVIOCGPROP)
	uint8_t ledbits[LED_CNT / 8]; // LEDs this device has
	uint8_t led_state[LED_CNT / 8]; // which of them are lit
	/* Clock used for event timestamps.  Display servers set this to
	 * CLOCK_MONOTONIC so input timing is immune to the wall clock being
	 * stepped; the default stays CLOCK_REALTIME for compatibility. */
	int clock_id;
	int wq; // wait-channel key for blocked readers
	/* Who is polling this device.  Separate from `wq' above, which is the
	 * channel a BLOCKING read parks on -- a poller waits for readiness,
	 * a reader waits for a byte, and they are woken from the same place
	 * but tracked apart. */
	struct wait_queue_head poll_wq;
} evdev_dev_t;

static evdev_dev_t g_evdev[EVDEV_NUM_UNITS];
static int g_evdev_initialized;

static inline void bit_set(uint8_t *map, unsigned bit)
{
	map[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static inline void bit_clear(uint8_t *map, unsigned bit)
{
	map[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static inline int bit_test(const uint8_t *map, unsigned bit)
{
	return (map[bit >> 3] >> (bit & 7)) & 1;
}

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

/* The poll wait queue of one unit, for poll()/select() to register on.
 * NULL when the unit number is out of range. */
struct wait_queue_head *evdev_pollq(int unit)
{
	if (unit < 0 || unit >= EVDEV_NUM_UNITS)
		return NULL;
	return &g_evdev[unit].poll_wq;
}

static void evdev_init_devices(void)
{
	evdev_dev_t *kbd = &g_evdev[EVDEV_UNIT_KEYBOARD];
	evdev_dev_t *mou = &g_evdev[EVDEV_UNIT_MOUSE];
	unsigned i;

	if (g_evdev_initialized)
		return;

	spinlock_init(&kbd->lock, "evdev_kbd");
	for (i = 0; i < EVDEV_NUM_UNITS; i++)
		wq_head_init(&g_evdev[i].poll_wq, "evdev-poll");
	kbd->name = "LikeOS Keyboard";
	/* Physical path in the conventional serio form; a display server uses
	 * it to tell devices apart and to match configuration rules. */
	kbd->phys = "isa0060/serio0/input0";
	kbd->clock_id = CLOCK_REALTIME;
	/* The keyboard reports the three standard lock LEDs.  Their state is
	 * tracked but not driven yet, so EVIOCGLED reads them all unlit. */
	bit_set(kbd->evbits, EV_LED);
	bit_set(kbd->ledbits, LED_NUML);
	bit_set(kbd->ledbits, LED_CAPSL);
	bit_set(kbd->ledbits, LED_SCROLLL);
	kbd->id.bustype = BUS_I8042;
	kbd->id.vendor = 0x0001;
	kbd->id.product = 0x0001;
	kbd->id.version = 0x0100;
	bit_set(kbd->evbits, EV_SYN);
	bit_set(kbd->evbits, EV_KEY);
	// Base set-1 make codes map 1:1 onto key codes 1..88, plus the
	// extended navigation/modifier block.
	for (i = KEY_ESC; i <= KEY_F12; i++)
		bit_set(kbd->keybits, i);
	for (i = KEY_KPENTER; i <= KEY_DELETE; i++)
		bit_set(kbd->keybits, i);
	bit_set(kbd->keybits, KEY_KPEQUAL);
	bit_set(kbd->keybits, KEY_PAUSE);
	bit_set(kbd->keybits, KEY_LEFTMETA);
	bit_set(kbd->keybits, KEY_RIGHTMETA);
	bit_set(kbd->keybits, KEY_COMPOSE);

	spinlock_init(&mou->lock, "evdev_mouse");
	mou->name = "LikeOS Mouse";
	mou->phys = "isa0060/serio1/input0";
	mou->clock_id = CLOCK_REALTIME;
	/* INPUT_PROP_POINTER: motion is relative and drives a cursor, rather
	 * than mapping onto screen coordinates the way a touchscreen does. */
	bit_set(mou->propbits, INPUT_PROP_POINTER);
	mou->id.bustype = BUS_I8042;
	mou->id.vendor = 0x0001;
	mou->id.product = 0x0002;
	mou->id.version = 0x0100;
	bit_set(mou->evbits, EV_SYN);
	bit_set(mou->evbits, EV_KEY);
	bit_set(mou->evbits, EV_REL);
	bit_set(mou->keybits, BTN_LEFT);
	bit_set(mou->keybits, BTN_RIGHT);
	bit_set(mou->keybits, BTN_MIDDLE);
	bit_set(mou->keybits, BTN_SIDE);
	bit_set(mou->keybits, BTN_EXTRA);
	bit_set(mou->relbits, REL_X);
	bit_set(mou->relbits, REL_Y);
	bit_set(mou->relbits, REL_WHEEL);
	bit_set(mou->relbits, REL_HWHEEL);

	__asm__ volatile("" ::: "memory");
	g_evdev_initialized = 1;
}

static void evdev_timestamp(evdev_dev_t *dev, struct input_event *ev)
{
	uint64_t us = timer_get_precise_us();

	/* CLOCK_MONOTONIC is time since boot; CLOCK_REALTIME adds the boot
	 * epoch.  Both come from the same counter, so events stay ordered
	 * whichever a client picks. */
	if (dev && dev->clock_id == CLOCK_MONOTONIC)
		ev->time.tv_sec = (int64_t)(us / 1000000);
	else
		ev->time.tv_sec =
			(int64_t)(timer_get_boot_epoch() + us / 1000000);
	ev->time.tv_usec = (int64_t)(us % 1000000);
}

// Queue one event; caller holds dev->lock.  Interrupt-context safe:
// no prints, no allocation, no sleeping.
static void evdev_queue_locked(evdev_dev_t *dev, uint16_t type, uint16_t code,
			       int32_t value)
{
	struct input_event *ev;

	if (dev->count == EVDEV_RING_SIZE) {
		// Overflow: drop everything and leave a SYN_DROPPED marker so
		// clients know to re-sync their state (standard semantics).
		dev->head = 0;
		dev->tail = 0;
		dev->count = 0;
		dev->dropped++;
		ev = &dev->ring[dev->head];
		evdev_timestamp(dev, ev);
		ev->type = EV_SYN;
		ev->code = SYN_DROPPED;
		ev->value = 0;
		dev->head = (dev->head + 1) % EVDEV_RING_SIZE;
		dev->count++;
	}
	ev = &dev->ring[dev->head];
	evdev_timestamp(dev, ev);
	ev->type = type;
	ev->code = code;
	ev->value = value;
	dev->head = (dev->head + 1) % EVDEV_RING_SIZE;
	dev->count++;
}

static void evdev_wake_readers(evdev_dev_t *dev)
{
	extern void poll_notify_wq(struct wait_queue_head *);

	sched_wake_channel((void *)&dev->wq);
	/* Only the tasks polling THIS device.  The display server polls the
	 * keyboard and the mouse as separate descriptors, and a mouse event
	 * has no business waking anything else on the machine. */
	poll_notify_wq(&dev->poll_wq);
}

// Emit a key event with automatic press/release/repeat value derivation.
static void evdev_report_key(evdev_dev_t *dev, uint16_t code, int pressed)
{
	uint64_t f;
	int32_t value;

	if (code == KEY_RESERVED || code > KEY_MAX)
		return;
	spin_lock_irqsave(&dev->lock, &f);
	if (pressed) {
		value = bit_test(dev->key_state, code) ? 2 : 1; // repeat?
		bit_set(dev->key_state, code);
	} else {
		if (!bit_test(dev->key_state, code)) {
			// Release without press (lost sync): drop silently.
			spin_unlock_irqrestore(&dev->lock, f);
			return;
		}
		value = 0;
		bit_clear(dev->key_state, code);
	}
	evdev_queue_locked(dev, EV_KEY, code, value);
	evdev_queue_locked(dev, EV_SYN, SYN_REPORT, 0);
	spin_unlock_irqrestore(&dev->lock, f);
	evdev_wake_readers(dev);
}

// ---------------------------------------------------------------------------
// Producer entry points (called from the keyboard/mouse interrupt paths)
// ---------------------------------------------------------------------------

int evdev_feed_keyboard(uint8_t scan_code)
{
	static int e0_prefix; // producer-side prefix state (IRQ serialised)
	evdev_dev_t *dev = &g_evdev[EVDEV_UNIT_KEYBOARD];
	uint16_t code;
	int pressed;

	if (!g_evdev_initialized)
		evdev_init_devices();

	if (scan_code == 0xE0) {
		e0_prefix = 1;
		return evdev_kbd_grabbed();
	}
	pressed = !(scan_code & 0x80);
	if (e0_prefix) {
		e0_prefix = 0;
		code = evdev_e0_keycode(scan_code & 0x7F);
	} else {
		code = scan_code & 0x7F; // set-1 make codes == key codes
		if (code > 0x58)
			code = KEY_RESERVED;
	}
	evdev_report_key(dev, code, pressed);
	return evdev_kbd_grabbed();
}

int evdev_feed_mouse(int dx, int dy, uint8_t buttons, int wheel, int hwheel)
{
	static uint8_t last_buttons; // producer-side state (IRQ serialised)
	evdev_dev_t *dev = &g_evdev[EVDEV_UNIT_MOUSE];
	uint64_t f;
	uint8_t changed;
	int queued = 0;

	if (!g_evdev_initialized)
		evdev_init_devices();

	// Button edges as EV_KEY events
	changed = buttons ^ last_buttons;
	if (changed & 0x01)
		evdev_report_key(dev, BTN_LEFT, buttons & 0x01);
	if (changed & 0x02)
		evdev_report_key(dev, BTN_RIGHT, buttons & 0x02);
	if (changed & 0x04)
		evdev_report_key(dev, BTN_MIDDLE, buttons & 0x04);
	last_buttons = buttons;

	spin_lock_irqsave(&dev->lock, &f);
	if (dx) {
		evdev_queue_locked(dev, EV_REL, REL_X, dx);
		queued = 1;
	}
	if (dy) {
		evdev_queue_locked(dev, EV_REL, REL_Y, dy);
		queued = 1;
	}
	if (wheel) {
		evdev_queue_locked(dev, EV_REL, REL_WHEEL, wheel);
		queued = 1;
	}
	if (hwheel) {
		evdev_queue_locked(dev, EV_REL, REL_HWHEEL, hwheel);
		queued = 1;
	}
	if (queued)
		evdev_queue_locked(dev, EV_SYN, SYN_REPORT, 0);
	spin_unlock_irqrestore(&dev->lock, f);
	if (queued)
		evdev_wake_readers(dev);
	return evdev_mouse_grabbed();
}

int evdev_kbd_grabbed(void)
{
	return g_evdev[EVDEV_UNIT_KEYBOARD].grab_task != 0;
}

int evdev_mouse_grabbed(void)
{
	return g_evdev[EVDEV_UNIT_MOUSE].grab_task != 0;
}

// ---------------------------------------------------------------------------
// Reader interface
// ---------------------------------------------------------------------------

static evdev_dev_t *evdev_get(int unit)
{
	if (unit < 0 || unit >= EVDEV_NUM_UNITS)
		return NULL;
	if (!g_evdev_initialized)
		evdev_init_devices();
	return &g_evdev[unit];
}

long evdev_read(int unit, void *user_buf, long bytes, int nonblock)
{
	evdev_dev_t *dev = evdev_get(unit);
	struct input_event burst[EVDEV_MAX_READ_BURST];
	task_t *cur = sched_current();
	unsigned want, got;
	uint64_t f;

	if (!dev || !user_buf)
		return -EINVAL;
	if (bytes < (long)sizeof(struct input_event))
		return -EINVAL;
	BUILD_BUG_ON(sizeof(struct input_event) != 24);

	want = (unsigned)(bytes / sizeof(struct input_event));
	if (want > EVDEV_MAX_READ_BURST)
		want = EVDEV_MAX_READ_BURST;

	for (;;) {
		got = 0;
		spin_lock_irqsave(&dev->lock, &f);
		while (dev->count > 0 && got < want) {
			burst[got++] = dev->ring[dev->tail];
			dev->tail = (dev->tail + 1) % EVDEV_RING_SIZE;
			dev->count--;
		}
		spin_unlock_irqrestore(&dev->lock, f);

		if (got > 0) {
			smap_disable();
			kmemcpy(user_buf, burst,
				got * sizeof(struct input_event));
			smap_enable();
			return (long)(got * sizeof(struct input_event));
		}
		if (nonblock)
			return -EAGAIN;
		if (!cur)
			return -EAGAIN; // no task context: cannot block

		// Park atomically wrt the producer (same protocol as
		// tty_read): publish BLOCKED + channel, re-check the ring
		// under the lock, then schedule.
		cur->wait_channel = (void *)&dev->wq;
		cur->state = TASK_BLOCKED;
		spin_lock_irqsave(&dev->lock, &f);
		if (dev->count > 0) {
			cur->state = TASK_READY;
			cur->wait_channel = NULL;
			spin_unlock_irqrestore(&dev->lock, f);
			continue;
		}
		spin_unlock_irqrestore(&dev->lock, f);
		sched_schedule();
		if (cur->state == TASK_ZOMBIE || cur->has_exited ||
		    signal_pending(cur))
			return -EINTR;
	}
}

short evdev_poll(int unit, short events)
{
	evdev_dev_t *dev = evdev_get(unit);
	short rev = 0;

	if (!dev)
		return 0;
	// POLLIN|POLLRDNORM when events are queued; never writable.
	if ((events & 0x0041 /* POLLIN|POLLRDNORM */) && dev->count > 0)
		rev |= (short)(events & 0x0041);
	return rev;
}

/* Release a grab held through this descriptor, whoever is closing it. */
void evdev_release_grab_by_owner(int unit, void *owner)
{
	evdev_dev_t *dev = evdev_get(unit);
	uint64_t f;

	if (!dev || !owner)
		return;
	spin_lock_irqsave(&dev->lock, &f);
	if (dev->grab_owner == owner) {
		dev->grab_owner = NULL;
		dev->grab_task = 0;
	}
	spin_unlock_irqrestore(&dev->lock, f);
}

void evdev_release_grab_for(int unit, int64_t task_id)
{
	evdev_dev_t *dev = evdev_get(unit);
	uint64_t f;

	if (!dev)
		return;
	spin_lock_irqsave(&dev->lock, &f);
	if (dev->grab_task == task_id)
		dev->grab_task = 0;
	spin_unlock_irqrestore(&dev->lock, f);
}

// ---------------------------------------------------------------------------
// ioctl interface
// ---------------------------------------------------------------------------

static int evdev_copy_out(void *user_dst, const void *src, size_t len)
{
	if (!user_dst)
		return -EFAULT;
	smap_disable();
	kmemcpy(user_dst, src, len);
	smap_enable();
	return 0;
}

static int evdev_copy_in(void *dst, const void *user_src, size_t len)
{
	if (!user_src)
		return -EFAULT;
	smap_disable();
	kmemcpy(dst, user_src, len);
	smap_enable();
	return 0;
}

// Copy a capability bitmap, honouring the user-supplied size; returns the
// number of bytes copied (standard EVIOCGBIT return convention).
static int evdev_copy_bits(void *argp, const uint8_t *bits, size_t bits_len,
			   size_t user_len)
{
	size_t n = user_len < bits_len ? user_len : bits_len;
	int rc;

	if (n == 0)
		return 0;
	rc = evdev_copy_out(argp, bits, n);
	if (rc != 0)
		return rc;
	// Zero-fill the tail when the caller asked for more than we have.
	if (user_len > bits_len) {
		static const uint8_t zeros[32];
		size_t off = bits_len;
		while (off < user_len) {
			size_t chunk = user_len - off;
			if (chunk > sizeof(zeros))
				chunk = sizeof(zeros);
			smap_disable();
			kmemcpy((uint8_t *)argp + off, zeros, chunk);
			smap_enable();
			off += chunk;
		}
	}
	return (int)n;
}

int evdev_ioctl(int unit, unsigned long req, void *argp, struct task *cur,
		void *owner)
{
	evdev_dev_t *dev = evdev_get(unit);
	unsigned int nr = (unsigned int)((req >> _INPUT_IOC_NRSHIFT) & 0xFF);
	unsigned int type =
		(unsigned int)((req >> _INPUT_IOC_TYPESHIFT) & 0xFF);
	size_t user_len =
		(size_t)((req >> _INPUT_IOC_SIZESHIFT) & 0x3FFF);

	if (!dev)
		return -EINVAL;
	if (type != 'E')
		return -ENOTTY;

	if (req == EVIOCGVERSION) {
		int v = EV_VERSION;
		return evdev_copy_out(argp, &v, sizeof(v));
	}
	if (req == EVIOCGID)
		return evdev_copy_out(argp, &dev->id, sizeof(dev->id));
	if (nr == 0x06) { // EVIOCGNAME(len)
		size_t len = 0;
		while (dev->name[len])
			len++;
		len++; // NUL
		if (len > user_len)
			len = user_len;
		if (len == 0)
			return 0;
		if (evdev_copy_out(argp, dev->name, len) != 0)
			return -EFAULT;
		return (int)len;
	}
	if (nr == 0x18) { // EVIOCGKEY(len)
		uint64_t f;
		uint8_t snapshot[sizeof(dev->key_state)];
		spin_lock_irqsave(&dev->lock, &f);
		kmemcpy(snapshot, (const void *)dev->key_state,
			sizeof(snapshot));
		spin_unlock_irqrestore(&dev->lock, f);
		return evdev_copy_bits(argp, snapshot, sizeof(snapshot),
				       user_len);
	}
	if (nr >= 0x20 && nr <= 0x20 + EV_MAX) { // EVIOCGBIT(ev, len)
		unsigned int ev = nr - 0x20;
		if (ev == 0)
			return evdev_copy_bits(argp, dev->evbits,
					       sizeof(dev->evbits), user_len);
		if (ev == EV_KEY)
			return evdev_copy_bits(argp, dev->keybits,
					       sizeof(dev->keybits),
					       user_len);
		if (ev == EV_REL)
			return evdev_copy_bits(argp, dev->relbits,
					       sizeof(dev->relbits),
					       user_len);
		// Unsupported event class: report an empty bitmap.
		{
			static const uint8_t none[8];
			return evdev_copy_bits(argp, none, sizeof(none),
					       user_len);
		}
	}
	if (nr == 0x07) { // EVIOCGPHYS(len) - physical location
		const char *ph = dev->phys ? dev->phys : "";
		size_t len = 0;
		while (ph[len])
			len++;
		len++; // NUL
		if (len > user_len)
			len = user_len;
		if (len == 0)
			return 0;
		if (evdev_copy_out(argp, ph, len) != 0)
			return -EFAULT;
		return (int)len;
	}
	if (nr == 0x08) { // EVIOCGUNIQ(len) - unique identifier
		/* These devices have no serial number.  Reporting "no such
		 * entry" is how a driver says the property is absent; an empty
		 * string would claim it exists and is blank. */
		return -ENOENT;
	}
	if (nr == 0x09) // EVIOCGPROP(len) - device properties
		return evdev_copy_bits(argp, dev->propbits,
				       sizeof(dev->propbits), user_len);
	if (nr == 0x19) // EVIOCGLED(len) - which LEDs are lit
		return evdev_copy_bits(argp, dev->led_state,
				       sizeof(dev->led_state), user_len);
	if (nr == 0x1a) { // EVIOCGSND(len) - sound-source state
		static const uint8_t none[8];
		return evdev_copy_bits(argp, none, sizeof(none), user_len);
	}
	if (nr == 0x1b) { // EVIOCGSW(len) - switch state
		static const uint8_t none[8];
		return evdev_copy_bits(argp, none, sizeof(none), user_len);
	}
	if (nr >= 0x40 && nr <= 0x40 + ABS_MAX) { // EVIOCGABS(abs)
		/* No device here reports absolute motion.  A zeroed absinfo is
		 * the conventional answer for an axis the device does not
		 * have: the query succeeds and the ranges read as empty, which
		 * is what a driver checks. */
		struct input_absinfo abs = { 0, 0, 0, 0, 0, 0 };
		return evdev_copy_out(argp, &abs, sizeof(abs));
	}
	if (req == EVIOCSCLOCKID) { // select the clock used for timestamps
		int id;
		uint64_t f;

		if (!argp)
			return -EFAULT;
		if (evdev_copy_in(&id, argp, sizeof(id)) != 0)
			return -EFAULT;
		if (id != CLOCK_REALTIME && id != CLOCK_MONOTONIC)
			return -EINVAL;
		spin_lock_irqsave(&dev->lock, &f);
		dev->clock_id = id;
		spin_unlock_irqrestore(&dev->lock, f);
		return 0;
	}
	if (req == EVIOCGRAB) {
		// The argument is the flag value itself, not a pointer.
		long grab = (long)(uint64_t)argp;
		int64_t tid = cur ? cur->id : 0;
		uint64_t f;
		int rc = 0;

		if (WARN_ON_ONCE(!cur))
			return -EINVAL;
		spin_lock_irqsave(&dev->lock, &f);
		if (grab) {
			if (dev->grab_task == 0 || dev->grab_task == tid) {
				dev->grab_task = tid;
				dev->grab_owner = owner;
			} else {
				rc = -EBUSY;
			}
		} else {
			if (dev->grab_task == tid || dev->grab_task == 0) {
				dev->grab_task = 0;
				dev->grab_owner = NULL;
			} else {
				rc = -EINVAL;
			}
		}
		spin_unlock_irqrestore(&dev->lock, f);
		return rc;
	}
	return -EINVAL;
}
